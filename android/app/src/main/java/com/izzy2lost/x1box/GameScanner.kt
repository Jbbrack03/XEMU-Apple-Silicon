package com.izzy2lost.x1box

import android.content.Context
import android.os.Environment
import java.io.File
import java.util.Locale

/**
 * Direct-filesystem game scan shared by the XR-native picker. Mirrors the
 * zero-setup fixed-folder scan in [GameLibraryActivity.scanFixedFoldersForGames]
 * (SAF trees are excluded here — the XR shell has no Activity to drive a grant,
 * and MANAGE_EXTERNAL_STORAGE already covers the shared folders below, giving
 * every entry a real path the emulator can mount directly).
 *
 * Only raw ISO formats are listed: the runtime disc bridge mounts media as
 * "raw", so compressed .cso/.cci would boot garbage — they are intentionally
 * excluded.
 */
object GameScanner {
  private val gameExts = setOf("iso", "xiso")
  private const val MAX_DEPTH = 6

  data class Game(
    // Raw filename-derived title. This is the cover-art lookup key and must
    // stay stable; user-facing text uses displayTitle instead.
    val title: String,
    val path: String,
    // Path relative to the scan root (incl. subdirs) — matches the id the 2D
    // SAF flow uses (GameLibraryActivity relativePath) so per-game overrides
    // land in the same bucket and same-basename games don't collide.
    val relativePath: String,
    // Proper game name for the shelf label (curated map, else title-cased).
    val displayTitle: String = title,
  )

  fun scan(context: Context): List<Game> {
    val roots = ArrayList<File>()
    context.getExternalFilesDirs(null).forEach { base ->
      if (base != null) {
        roots.add(File(base, "games"))
        roots.add(File(base, "Games"))
        roots.add(base)
      }
    }
    roots.addAll(
      listOf(
        File("/sdcard/Games"),
        File("/sdcard/Download/xemu-games"),
        File("/sdcard/Xbox"),
        File("/sdcard/Roms/Xbox"),
        File(Environment.getExternalStorageDirectory(), "Games"),
      )
    )

    val out = ArrayList<Game>()
    val seenFiles = HashSet<String>()
    val visitedDirs = HashSet<String>() // canonical dir paths — breaks symlink loops
    for (root in roots.distinctBy { it.absolutePath }) {
      if (!root.isDirectory) continue
      val rootPath = root.absolutePath
      val stack = ArrayDeque<Pair<File, Int>>()
      stack.add(root to 0)
      while (stack.isNotEmpty()) {
        val (dir, depth) = stack.removeLast()
        val canon = runCatching { dir.canonicalPath }.getOrDefault(dir.absolutePath)
        if (!visitedDirs.add(canon)) continue
        val children = dir.listFiles() ?: continue
        for (child in children) {
          if (child.isDirectory) {
            if (depth < MAX_DEPTH) stack.add(child to depth + 1)
            continue
          }
          val name = child.name
          if (!child.isFile || !isSupported(name)) continue
          if (!seenFiles.add(child.absolutePath.lowercase(Locale.ROOT))) continue
          val rel = child.absolutePath.removePrefix(rootPath).trimStart('/')
          val raw = toTitle(name)
          out.add(Game(raw, child.absolutePath, rel.ifEmpty { name },
                       toDisplayTitle(raw)))
        }
      }
    }
    out.sortBy { it.displayTitle.lowercase(Locale.ROOT) }
    return out
  }

  private fun isSupported(name: String): Boolean {
    val lower = name.lowercase(Locale.ROOT)
    if (lower.endsWith(".xiso.iso")) return true
    val ext = lower.substringAfterLast('.', "")
    return ext.isNotEmpty() && gameExts.contains(ext)
  }

  private fun toTitle(fileName: String): String {
    val lower = fileName.lowercase(Locale.ROOT)
    val raw = when {
      lower.endsWith(".xiso.iso") -> fileName.dropLast(".xiso.iso".length)
      fileName.contains('.') -> fileName.substringBeforeLast('.')
      else -> fileName
    }
    return raw
      .replace(Regex("(\\s*(\\([^\\)]*\\)|\\[[^\\]]*\\]))+$"), "")
      .replace(Regex("\\s+"), " ")
      .trim()
      .ifEmpty { raw.trim() }
  }

  // Proper box names for common dump-style basenames; anything unknown gets
  // simple title casing so the shelf never shows raw abbreviations.
  private val knownTitles = mapOf(
    "burnout3" to "Burnout 3: Takedown",
    "crimson" to "Crimson Skies",
    "crimson skies" to "Crimson Skies: High Road to Revenge",
    "doa3" to "Dead or Alive 3",
    "fable" to "Fable",
    "forza" to "Forza Motorsport",
    "halo - combat evolved" to "Halo: Combat Evolved",
    "halo" to "Halo: Combat Evolved",
    "halo2" to "Halo 2",
    "hl2" to "Half-Life 2",
    "jsrf" to "Jet Set Radio Future",
    "kotor" to "Star Wars: Knights of the Old Republic",
    "morrowind" to "The Elder Scrolls III: Morrowind",
    "ngb" to "Ninja Gaiden Black",
    "outrun 2" to "OutRun 2",
    "outrun2" to "OutRun 2",
    "panzer" to "Panzer Dragoon Orta",
    "pgr2" to "Project Gotham Racing 2",
    "rainbow six 3" to "Tom Clancy's Rainbow Six 3",
    "rtcw" to "Return to Castle Wolfenstein",
    "sc2" to "SoulCalibur II",
    "soul calibur 2" to "SoulCalibur II",
    "sof2" to "Soldier of Fortune II: Double Helix",
  )

  private val smallWords = setOf("of", "the", "and", "in", "on", "at", "to", "a", "an")

  private fun toDisplayTitle(raw: String): String {
    knownTitles[raw.lowercase(Locale.ROOT)]?.let { return it }
    val words = raw.split(' ')
    return words.mapIndexed { i, w ->
      val lower = w.lowercase(Locale.ROOT)
      when {
        w.length <= 1 -> w.uppercase(Locale.ROOT)
        i > 0 && lower in smallWords -> lower
        else -> lower.replaceFirstChar { it.uppercase(Locale.ROOT) }
      }
    }.joinToString(" ")
  }
}
