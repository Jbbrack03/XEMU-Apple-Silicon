package com.izzy2lost.x1box

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import java.io.File
import java.io.FileOutputStream

/**
 * Backing model + Canvas renderer for the XR-native game picker. The OpenXR
 * shell (xr_shell_main.c) owns the composition-layer quad and controller input;
 * it drives this object entirely over JNI. Every method here runs on the native
 * `android_main` thread (never the UI thread), so plain file/prefs/Canvas work
 * is safe and needs no marshaling.
 *
 * The emulator reads FP JIT (`setting_hard_fpu`) only at boot, so toggling it
 * here persists a per-game override via [PerGameSettingsManager]; it becomes
 * authoritative the next time that title is cold-launched. Switching the active
 * game in-VR is a live disc swap (see xemu_xr_request_load_disc), which changes
 * the ISO without rebooting the process.
 */
class XrMenuBridge(context: Context) {
  private val appContext = context.applicationContext

  // Scanned off-thread; the immutable list reference is swapped atomically so
  // the native frame/input thread never blocks on filesystem I/O.
  @Volatile private var games: List<GameScanner.Game> = emptyList()
  @Volatile private var scanning = false
  private var selected = 0
  private var scrollTop = 0
  private var visibleRows = 1
  // Written by the scan thread on completion, read/cleared on the frame thread.
  @Volatile private var dirty = true

  // Running process's active FP JIT mode (pushed from native on open); lets us
  // flag per-game toggles that only take effect on the next cold launch.
  private var activeFpJitKnown = false
  private var activeFpJitValue = false

  // Debug: when XEMU_DUMP_MENU=<dir> is set, every (re)render is also written
  // there as a PNG so the menu can be inspected without a headset.
  private val dumpDir: String? = System.getenv("XEMU_DUMP_MENU")?.takeIf { it.isNotBlank() }
  private var dumpCounter = 0

  private var bitmap: Bitmap? = null

  private val bgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xF0121821.toInt() }
  private val headerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    color = 0xFF5FD0FF.toInt()
    typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
  }
  private val rowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }
  private val dimPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFB8C2CC.toInt() }
  private val tagPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    color = 0xFF7CFFA0.toInt()
    typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
  }
  private val highlightPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x40FFFFFF }
  private val footerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF8894A0.toInt() }
  private val notePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    color = 0xFFFFC46B.toInt()
    typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
  }

  init {
    reloadGames()
  }

  /** Kick an async library rescan (safe to call from the frame thread). */
  fun refresh() {
    reloadGames()
  }

  private fun reloadGames() {
    synchronized(this) {
      if (scanning) return
      scanning = true
    }
    dirty = true // show the scanning state immediately
    Thread {
      val scanned = try {
        GameScanner.scan(appContext)
      } catch (_: Exception) {
        emptyList()
      }
      games = scanned
      scanning = false
      dirty = true
    }.apply { isDaemon = true }.start()
  }

  fun count(): Int = games.size

  fun isDirty(): Boolean = dirty

  /** Push the running process's active FP JIT mode (called from native on open). */
  fun setActiveFpJit(known: Boolean, value: Boolean) {
    if (known != activeFpJitKnown || value != activeFpJitValue) {
      activeFpJitKnown = known
      activeFpJitValue = value
      dirty = true
    }
  }

  fun moveSelection(delta: Int) {
    val list = games
    if (list.isEmpty()) return
    val next = (selected + delta).coerceIn(0, list.size - 1)
    if (next != selected) {
      selected = next
      dirty = true
    }
  }

  /** Debug/autotest: select the entry whose file name matches. */
  fun selectByName(name: String): Boolean {
    val idx = games.indexOfFirst { java.io.File(it.path).name == name }
    if (idx < 0) return false
    selected = idx
    dirty = true
    return true
  }

  /** Flip the per-game FP JIT override for the highlighted title. */
  fun toggleFpJit() {
    val game = games.getOrNull(selected) ?: return
    // Merge into existing overrides — saveOverrides rewrites every key, so a
    // partial map would wipe the game's other per-game settings.
    val merged = PerGameSettingsManager.loadOverrides(appContext, game.relativePath).toMutableMap()
    val enabled = merged["setting_hard_fpu"] != "true"
    merged["setting_hard_fpu"] = if (enabled) "true" else "false"
    PerGameSettingsManager.saveOverrides(appContext, game.relativePath, merged)
    dirty = true
  }

  private fun isFpJit(game: GameScanner.Game): Boolean {
    return PerGameSettingsManager.loadOverrides(appContext, game.relativePath)["setting_hard_fpu"] == "true"
  }

  /**
   * Commit the highlighted game as the active title and return its filesystem
   * path for the shell to hand to xemu_xr_request_load_disc(). Also flushes the
   * disc selection + per-game overrides into prefs so a later cold boot (or the
   * 2D launcher) lands on the same game with the same settings.
   */
  fun activate(): String? {
    val game = games.getOrNull(selected) ?: return null
    val prefs = appContext.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    // Per-game runtime overrides matter only on a future cold boot -> async apply().
    prefs.edit().also {
      PerGameSettingsManager.applyRuntimeOverridesToEditor(appContext, it, game.relativePath)
    }.apply()
    // dvdPath must be durable before the guest reset can boot the new game -> commit().
    prefs.edit().putString("dvdPath", game.path).remove("dvdUri").commit()
    return game.path
  }

  /** (Re)draw the menu into a cached bitmap sized [w] x [h] and return it. */
  fun render(w: Int, h: Int): Bitmap? {
    if (w <= 0 || h <= 0) return null
    var bmp = bitmap
    if (bmp == null || bmp.width != w || bmp.height != h) {
      bmp?.recycle()
      bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
      bitmap = bmp
    }
    drawInto(bmp, w, h)
    dirty = false
    dumpIfRequested(bmp)
    return bmp
  }

  private fun dumpIfRequested(bmp: Bitmap) {
    val dir = dumpDir ?: return
    try {
      File(dir).mkdirs()
      val out = File(dir, "menu_%04d.png".format(dumpCounter++))
      FileOutputStream(out).use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
    } catch (_: Exception) {
    }
  }

  private fun drawInto(bmp: Bitmap, w: Int, h: Int) {
    val c = Canvas(bmp)
    c.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)

    val margin = w * 0.05f
    val panel = RectF(margin, margin, w - margin, h - margin)
    val radius = w * 0.03f
    c.drawRoundRect(panel, radius, radius, bgPaint)

    val titleSize = h * 0.045f
    val rowSize = h * 0.032f
    val footerSize = h * 0.024f
    headerPaint.textSize = titleSize
    rowPaint.textSize = rowSize
    dimPaint.textSize = rowSize
    tagPaint.textSize = rowSize * 0.7f
    footerPaint.textSize = footerSize
    notePaint.textSize = footerSize

    val padX = panel.left + w * 0.04f
    val headerY = panel.top + titleSize * 1.6f
    c.drawText("XEMU  •  Select a Game", padX, headerY, headerPaint)

    val list = games // snapshot: scan thread may swap it mid-draw
    if (selected >= list.size) selected = (list.size - 1).coerceAtLeast(0)

    val listTop = headerY + titleSize * 0.9f
    val footerH = footerSize * 3.6f
    val listBottom = panel.bottom - footerH
    val rowH = rowSize * 1.9f
    visibleRows = ((listBottom - listTop) / rowH).toInt().coerceAtLeast(1)

    // Keep the selection inside the visible window.
    if (selected < scrollTop) scrollTop = selected
    if (selected >= scrollTop + visibleRows) scrollTop = selected - visibleRows + 1
    if (scrollTop < 0) scrollTop = 0

    if (list.isEmpty()) {
      if (scanning) {
        c.drawText("Scanning library…", padX, listTop + rowH, dimPaint)
      } else {
        c.drawText("No games found.", padX, listTop + rowH, dimPaint)
        c.drawText(
          "Drop .iso/.xiso files in /sdcard/Download/xemu-games",
          padX, listTop + rowH * 2, footerPaint,
        )
      }
    } else {
      val last = minOf(scrollTop + visibleRows, list.size)
      for (i in scrollTop until last) {
        val game = list[i]
        val top = listTop + (i - scrollTop) * rowH
        val baseline = top + rowH * 0.7f
        if (i == selected) {
          c.drawRoundRect(
            RectF(panel.left + w * 0.02f, top, panel.right - w * 0.02f, top + rowH),
            radius * 0.4f, radius * 0.4f, highlightPaint,
          )
        }
        val paint = if (i == selected) rowPaint else dimPaint
        c.drawText(ellipsize(game.title, paint, panel.width() * 0.78f), padX, baseline, paint)
        if (isFpJit(game)) {
          val tag = "FP JIT"
          val tw = tagPaint.measureText(tag)
          c.drawText(tag, panel.right - w * 0.04f - tw, baseline, tagPaint)
        }
      }
    }

    val footerY = panel.bottom - footerSize * 0.8f
    // Warn when the selected game's FP JIT differs from the running process:
    // the change only lands on a cold launch, not this session's disc swap.
    val selectedGame = list.getOrNull(selected)
    if (selectedGame != null && activeFpJitKnown &&
      isFpJit(selectedGame) != activeFpJitValue) {
      c.drawText(
        "FP JIT change applies at next launch",
        padX, footerY - footerSize * 1.4f, notePaint,
      )
    }
    c.drawText(
      "Up/Down: move   A: play   X: FP JIT   Y: quit   B: close",
      padX, footerY, footerPaint,
    )
  }

  private fun ellipsize(text: String, paint: Paint, maxWidth: Float): String {
    if (paint.measureText(text) <= maxWidth) return text
    var end = text.length
    while (end > 1 && paint.measureText(text.substring(0, end) + "…") > maxWidth) {
      end--
    }
    return text.substring(0, end) + "…"
  }
}
