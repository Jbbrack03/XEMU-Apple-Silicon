package com.izzy2lost.x1box

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.LruCache
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

/**
 * Cover art resolver for the XR-native UI. Shares the exact on-disk cache the
 * 2D library uses (`files/custom_covers` first, then `files/downloaded_covers`,
 * keyed by the collapsed title), so covers fetched by either UI serve both.
 * Misses are resolved against the bundled `X1_Covers.txt` index and downloaded
 * once on a single background thread; [onLoaded] fires so the caller can mark
 * its frame dirty. All lookup calls are non-blocking.
 */
class XrCoverArt(context: Context, private val onLoaded: () -> Unit) {
  private val appContext = context.applicationContext
  private val memory = LruCache<String, Bitmap>(24)
  private val pending = ConcurrentHashMap.newKeySet<String>()
  private val misses = ConcurrentHashMap.newKeySet<String>()
  private val executor = Executors.newSingleThreadExecutor { r ->
    Thread(r, "xr-cover-art").apply { isDaemon = true }
  }
  @Volatile private var index: Map<String, String>? = null

  /** Non-blocking: cached bitmap, or null while (or after a failed) resolve. */
  fun get(title: String, targetW: Int, targetH: Int): Bitmap? {
    val key = collapse(title)
    if (key.isEmpty()) return null
    memory.get(key)?.let { return it }
    if (misses.contains(key) || !pending.add(key)) return null
    executor.execute { resolve(key, targetW, targetH) }
    return null
  }

  private fun resolve(key: String, targetW: Int, targetH: Int) {
    try {
      val candidates = listOf(key) + (ALIASES[key] ?: emptyList())
      var file: File? = null
      for (k in candidates) {
        val custom = File(File(appContext.filesDir, "custom_covers"), "$k.png")
        val downloaded = File(File(appContext.filesDir, "downloaded_covers"), "$k.png")
        file = when {
          custom.exists() && custom.length() > 0L -> custom
          downloaded.exists() && downloaded.length() > 0L -> downloaded
          else -> null
        }
        if (file != null) break
      }
      if (file == null) {
        val target = File(File(appContext.filesDir, "downloaded_covers"), "$key.png")
        file = download(candidates, target)
      }
      if (file == null) {
        misses.add(key)
        return
      }
      val bmp = decodeScaled(file, targetW, targetH)
      if (bmp != null) {
        memory.put(key, bmp)
        onLoaded()
      } else {
        misses.add(key)
      }
    } catch (_: Exception) {
      misses.add(key)
    } finally {
      pending.remove(key)
    }
  }

  private fun download(candidates: List<String>, target: File): File? {
    val url = candidates.firstNotNullOfOrNull { lookupUrl(it) } ?: return null
    var connection: HttpURLConnection? = null
    return try {
      target.parentFile?.mkdirs()
      val tmp = File(target.parentFile, "${target.name}.tmp")
      connection = (URL(url).openConnection() as HttpURLConnection).apply {
        connectTimeout = 5000
        readTimeout = 10000
        instanceFollowRedirects = true
      }
      connection.inputStream.use { input ->
        FileOutputStream(tmp).use { output -> input.copyTo(output) }
      }
      if (tmp.length() <= 0L) {
        tmp.delete()
        null
      } else {
        if (target.exists()) target.delete()
        if (!tmp.renameTo(target)) {
          tmp.copyTo(target, overwrite = true)
          tmp.delete()
        }
        target
      }
    } catch (_: Exception) {
      null
    } finally {
      connection?.disconnect()
    }
  }

  /** Collapsed-key lookup against the bundled cover index (loaded once). */
  private fun lookupUrl(key: String): String? {
    var idx = index
    if (idx == null) {
      idx = buildMap {
        try {
          appContext.assets.open("X1_Covers.txt").bufferedReader().useLines { lines ->
            for (line in lines) {
              val fileName = line.trim()
              if (fileName.isEmpty() || !fileName.endsWith(".png", ignoreCase = true)) continue
              val gameName = fileName.removeSuffix(".png").removeSuffix(".PNG").trim()
              val stripped = gameName.replace(Regex("(\\s*(\\([^\\)]*\\)|\\[[^\\]]*\\]))+$"), "")
              val encoded = URLEncoder.encode(fileName, "UTF-8").replace("+", "%20")
              val url = COVER_REPO_BASE + encoded
              for (candidate in arrayOf(gameName, stripped)) {
                val k = collapse(candidate)
                if (k.isNotEmpty() && !containsKey(k)) put(k, url)
              }
            }
          }
        } catch (_: Exception) {
          // No index asset: local cache still works, downloads are skipped.
        }
      }
      index = idx
    }
    idx[key]?.let { return it }
    if (key.length < 4) return null
    // Prefix fallback for filename-style titles ("burnout3" vs the index's
    // "Burnout 3 - Takedown"): only index keys that EXTEND the query qualify
    // (the reverse direction would let "burnout3" collide with "Burnout"),
    // and the match must be unambiguous.
    var match: String? = null
    for ((k, url) in idx) {
      if (k.startsWith(key)) {
        if (match != null && match != url) return null
        match = url
      }
    }
    return match
  }

  companion object {
    private const val COVER_REPO_BASE =
      "https://raw.githubusercontent.com/izzy2lost/X1_Covers/main/"

    /** Common community abbreviations -> canonical collapsed index keys. */
    private val ALIASES: Map<String, List<String>> = mapOf(
      "sc2" to listOf("soulcalibur2", "soulcaliburii"),
      "doa3" to listOf("deadoralive3"),
      "hl2" to listOf("halflife2"),
      "jsrf" to listOf("jetsetradiofuture"),
      "kotor" to listOf("starwarsknightsoftheoldrepublic"),
      "ngb" to listOf("ninjagaidenblack"),
      "pgr" to listOf("projectgothamracing"),
      "pgr2" to listOf("projectgothamracing2"),
      "rtcw" to listOf("returntocastlewolfenstein"),
      "sof2" to listOf("soldieroffortuneii"),
      "r63" to listOf("tomclancysrainbowsix3"),
      "halo" to listOf("halocombatevolved"),
      "morrowind" to listOf("theelderscrollsiiimorrowind"),
      "panzer" to listOf("panzerdragoonorta"),
    )

    /** Must match GameLibraryActivity's normalize+collapse so cache files are shared. */
    fun collapse(input: String): String {
      var t = input.lowercase(Locale.ROOT).trim()
      t = t.replace('_', ' ')
      t = t.replace('’', '\'')
      t = t.replace(Regex("\\s+"), " ")
      return t.replace(Regex("[^a-z0-9]+"), "")
    }

    private fun decodeScaled(file: File, targetW: Int, targetH: Int): Bitmap? {
      val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
      BitmapFactory.decodeFile(file.absolutePath, opts)
      if (opts.outWidth <= 0 || opts.outHeight <= 0) return null
      var sample = 1
      while (opts.outWidth / (sample * 2) >= targetW &&
        opts.outHeight / (sample * 2) >= targetH) {
        sample *= 2
      }
      val decode = BitmapFactory.Options().apply { inSampleSize = sample }
      return BitmapFactory.decodeFile(file.absolutePath, decode)
    }
  }
}
