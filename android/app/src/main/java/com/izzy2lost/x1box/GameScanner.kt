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
    val title: String,
    val path: String,
    // Path relative to the scan root (incl. subdirs) — matches the id the 2D
    // SAF flow uses (GameLibraryActivity relativePath) so per-game overrides
    // land in the same bucket and same-basename games don't collide.
    val relativePath: String,
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
          out.add(Game(toTitle(name), child.absolutePath, rel.ifEmpty { name }))
        }
      }
    }
    out.sortBy { it.title.lowercase(Locale.ROOT) }
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
}
