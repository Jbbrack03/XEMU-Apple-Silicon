package com.izzy2lost.x1box

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface

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

  private var games: List<GameScanner.Game> = emptyList()
  private var selected = 0
  private var scrollTop = 0
  private var visibleRows = 1
  private var dirty = true

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

  init {
    reloadGames()
  }

  /** Rescan the library and reset dirty state. */
  fun refresh() {
    reloadGames()
  }

  private fun reloadGames() {
    games = try {
      GameScanner.scan(appContext)
    } catch (_: Exception) {
      emptyList()
    }
    if (selected >= games.size) selected = (games.size - 1).coerceAtLeast(0)
    dirty = true
  }

  fun count(): Int = games.size

  fun isDirty(): Boolean = dirty

  fun moveSelection(delta: Int) {
    if (games.isEmpty()) return
    val next = (selected + delta).coerceIn(0, games.size - 1)
    if (next != selected) {
      selected = next
      dirty = true
    }
  }

  /** Flip the per-game FP JIT override for the highlighted title. */
  fun toggleFpJit() {
    val game = games.getOrNull(selected) ?: return
    // Merge into existing overrides — saveOverrides rewrites every key, so a
    // partial map would wipe the game's other per-game settings.
    val merged = PerGameSettingsManager.loadOverrides(appContext, game.fileName).toMutableMap()
    val enabled = merged["setting_hard_fpu"] != "true"
    merged["setting_hard_fpu"] = if (enabled) "true" else "false"
    PerGameSettingsManager.saveOverrides(appContext, game.fileName, merged)
    dirty = true
  }

  private fun isFpJit(game: GameScanner.Game): Boolean {
    return PerGameSettingsManager.loadOverrides(appContext, game.fileName)["setting_hard_fpu"] == "true"
  }

  /**
   * Commit the highlighted game as the active title and return its filesystem
   * path for the shell to hand to xemu_xr_request_load_disc(). Also flushes the
   * disc selection + per-game overrides into prefs so a later cold boot (or the
   * 2D launcher) lands on the same game with the same settings.
   */
  fun activate(): String? {
    val game = games.getOrNull(selected) ?: return null
    val editor = appContext.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE).edit()
    PerGameSettingsManager.applyRuntimeOverridesToEditor(appContext, editor, game.fileName)
    editor.putString("dvdPath", game.path).remove("dvdUri").commit()
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
    return bmp
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

    val padX = panel.left + w * 0.04f
    val headerY = panel.top + titleSize * 1.6f
    c.drawText("XEMU  •  Select a Game", padX, headerY, headerPaint)

    val listTop = headerY + titleSize * 0.9f
    val footerH = footerSize * 2.2f
    val listBottom = panel.bottom - footerH
    val rowH = rowSize * 1.9f
    visibleRows = ((listBottom - listTop) / rowH).toInt().coerceAtLeast(1)

    // Keep the selection inside the visible window.
    if (selected < scrollTop) scrollTop = selected
    if (selected >= scrollTop + visibleRows) scrollTop = selected - visibleRows + 1
    if (scrollTop < 0) scrollTop = 0

    if (games.isEmpty()) {
      c.drawText("No games found.", padX, listTop + rowH, dimPaint)
      c.drawText(
        "Drop .iso/.xiso files in /sdcard/Download/xemu-games",
        padX, listTop + rowH * 2, footerPaint,
      )
    } else {
      val last = minOf(scrollTop + visibleRows, games.size)
      for (i in scrollTop until last) {
        val game = games[i]
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
    c.drawText(
      "Up/Down: move   A: play   X: FP JIT   B: close",
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
