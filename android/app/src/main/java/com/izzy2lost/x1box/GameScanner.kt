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
 */
object GameScanner {
  private val gameExts = setOf("iso", "xiso", "cso", "cci")

  data class Game(val title: String, val path: String) {
    val fileName: String get() = File(path).name
  }

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
    val seen = HashSet<String>()
    for (root in roots.distinctBy { it.absolutePath }) {
      if (!root.isDirectory) continue
      val stack = ArrayDeque<File>()
      stack.add(root)
      while (stack.isNotEmpty()) {
        val dir = stack.removeLast()
        val children = dir.listFiles() ?: continue
        for (child in children) {
          if (child.isDirectory) {
            stack.add(child)
            continue
          }
          val name = child.name
          if (!child.isFile || !isSupported(name)) continue
          if (!seen.add(child.absolutePath.lowercase(Locale.ROOT))) continue
          out.add(Game(toTitle(name), child.absolutePath))
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
