package com.izzy2lost.x1box

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.os.BatteryManager
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

/**
 * Backing model + Canvas renderer for the XR-native shell UI. The OpenXR shell
 * (xr_shell_main.c) owns the composition-layer quad, the laser-pointer ray, and
 * controller input; it drives this object entirely over JNI. Every method here
 * runs on the native `android_main` thread (never the UI thread), so plain
 * file/prefs/Canvas work is safe and needs no marshaling.
 *
 * The UI is a Horizon-style landscape panel: a left nav rail with three pages
 * (Library / Settings / System), a cover-art game grid, grouped settings rows
 * that persist to the same `x1box_prefs` keys the 2D Settings uses, and a
 * system page with live telemetry plus shell actions. One focus concept is
 * shared by the pointer and the gamepad: pointer hover moves focus, dpad/stick
 * moves focus geometrically, A or trigger-click activates.
 *
 * Emulator-affecting settings (FP JIT, SSAA, ...) are read at boot, so edits
 * here persist to prefs and become authoritative on the next cold launch;
 * the UI says so. Switching the active game in-VR remains a live disc swap
 * (xemu_xr_request_load_disc) that does not reboot the process.
 */
class XrMenuBridge(context: Context) {
  private val activity = context as Activity
  private val appContext = context.applicationContext
  private val prefs = appContext.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)

  // Window placement lives in its own prefs file: it changes whenever the
  // user moves a window and must not perturb the emulator settings file.
  private val windowPrefs =
    appContext.getSharedPreferences("xr_window_prefs", Context.MODE_PRIVATE)

  // ---------------------------------------------------------------------
  // Commands raised by UI actions, drained by the native shell.
  // ---------------------------------------------------------------------
  companion object {
    const val CMD_NONE = 0
    const val CMD_CLOSE = 1
    const val CMD_QUIT_TO_DASHBOARD = 2 // native: eject + reset, land in library
    const val CMD_RECENTER = 3
    const val CMD_LAUNCH = 4
    const val CMD_MIC_TOGGLE = 5

    private const val PAGE_LIBRARY = 0
    private const val PAGE_SETTINGS = 1
    private const val PAGE_ONLINE = 2
    private const val PAGE_SYSTEM = 3
  }

  // Scanned off-thread; the immutable list reference is swapped atomically so
  // the native frame/input thread never blocks on filesystem I/O.
  @Volatile private var games: List<GameScanner.Game> = emptyList()
  @Volatile private var scanning = false
  private var selected = 0
  @Volatile private var dirty = true

  private var page = PAGE_LIBRARY
  private var pendingCommand = CMD_NONE

  // Whether the emulator process bootstrap has run (pushed by native on open).
  // Running: the menu is an in-game overlay — Library (disc swap) plus the
  // SAFE-while-running System actions only; cold-boot settings (Settings /
  // Online pages) are offered only before a game is up.
  @Volatile private var emulatorRunning = false

  // Focusables are rebuilt on every render pass (screen coordinates).
  private data class Focusable(val id: String, val rect: RectF)
  private val focusables = ArrayList<Focusable>()
  private var focusId: String? = null
  private var hoverId: String? = null
  private var pointerInside = false
  private var pressedId: String? = null

  private var libraryScroll = 0f
  private var settingsScroll = 0f
  private var libraryMaxScroll = 0f
  private var settingsMaxScroll = 0f

  // Rects needed for scroll clamping / focus autoscroll, refreshed per render.
  private var contentTop = 0f
  private var contentBottom = 0f

  // Running process's active FP JIT mode (pushed from native on open).
  private var activeFpJitKnown = false
  private var activeFpJitValue = false

  // Live telemetry pushed by the shell / polled from Android.
  private var guestFrameMs = 0f
  private var micState = -1
  private var batteryPct = -1
  private var batteryCharging = false
  private var clockText = ""
  private var lastStatusPollMs = 0L

  // Settings keys changed since the menu was created (drives the amber dot).
  private val changedKeys = HashSet<String>()

  // Online (Insignia) page: plain file-presence checks only. NEVER call the
  // native HDD tools (nativeInspectDashboardFlags etc.) from this class: they
  // initialize the QEMU block layer, and this process runs (or will run) the
  // emulator — the second qemu_clock_init aborts the process.
  private data class OnlineStatus(
    val hasHdd: Boolean,
    val hasEeprom: Boolean,
  )
  @Volatile private var onlineStatus: OnlineStatus? = null

  private val covers = XrCoverArt(appContext) { dirty = true }

  // Debug: when XEMU_DUMP_MENU=<dir> is set, every (re)render is also written
  // there as a PNG so the menu can be inspected without a headset. Read lazily
  // per render: with the native-first launch order this bridge is constructed
  // before SDL/xemu exports the env_vars pref into the process environment.
  private var dumpCounter = 0

  private var bitmap: Bitmap? = null

  // ---------------------------------------------------------------------
  // Theme
  // ---------------------------------------------------------------------
  private object Th {
    const val PANEL_BG = 0xF20D1117.toInt()
    const val PANEL_STROKE = 0x1FFFFFFF
    const val RAIL_DIVIDER = 0x14FFFFFF
    const val TEXT_PRIMARY = 0xFFF4F7FA.toInt()
    const val TEXT_SECONDARY = 0xFF9AA6B2.toInt()
    const val TEXT_TERTIARY = 0xFF69737E.toInt()
    const val ACCENT = 0xFF66E08A.toInt()
    const val ACCENT_DIM = 0x2966E08A
    const val HOVER_FILL = 0x14FFFFFF
    const val PRESS_FILL = 0x24FFFFFF
    const val CARD_BG = 0x0AFFFFFF
    const val CARD_STROKE = 0x12FFFFFF
    const val TRACK_OFF = 0xFF39424E.toInt()
    const val DANGER = 0xFFFF8A8A.toInt()
    const val DANGER_DIM = 0x24E05656
    const val AMBER = 0xFFFFC46B.toInt()
    const val BTN_A = 0xFF7CB94D.toInt()
    const val BTN_B = 0xFFD05252.toInt()
    const val BTN_X = 0xFF4A83D4.toInt()
    const val BTN_Y = 0xFFC9A72F.toInt()
    const val RAIL_W = 250f
    const val RADIUS = 40f
  }

  private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
  private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
  private val text = Paint(Paint.ANTI_ALIAS_FLAG)
  private val tfMedium: Typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
  private val tfRegular: Typeface = Typeface.create("sans-serif", Typeface.NORMAL)
  private val tfBold: Typeface = Typeface.create("sans-serif", Typeface.BOLD)

  // ---------------------------------------------------------------------
  // Settings model
  // ---------------------------------------------------------------------
  private sealed class Setting(val key: String, val label: String, val desc: String) {
    class Toggle(key: String, label: String, desc: String, val def: Boolean) :
      Setting(key, label, desc)

    class SegInt(key: String, label: String, desc: String, val options: List<Int>,
                 val names: List<String>, val def: Int) : Setting(key, label, desc)

    class SegStr(key: String, label: String, desc: String, val options: List<String>,
                 val names: List<String>, val def: String) : Setting(key, label, desc)
  }

  private class Section(val title: String, val items: List<Setting>)

  // Only settings that are safe on the Quest build are offered. Image
  // quality, pacing, console memory, FPU JIT, shader cache, and DSP JIT are
  // fixed at the validated configuration by the native launcher; per-game
  // compatibility overrides remain available through the library.
  private val sections = listOf(
    Section("DISPLAY", listOf(
      Setting.SegStr("setting_filtering", "Texture Filtering",
        "Smooth blends texels; Sharp keeps hard pixel edges",
        listOf("nearest", "linear"), listOf("Sharp", "Smooth"), "linear"),
      Setting.Toggle("show_fps", "FPS Overlay",
        "Draw a frame-rate readout over the game", false),
    )),
    Section("SYSTEM", listOf(
      Setting.Toggle("setting_skip_boot_anim", "Skip Boot Animation",
        "Boot straight into the game or dashboard", true),
    )),
    Section("AUDIO", listOf(
      Setting.Toggle("setting_use_dsp", "Enhanced Audio Accuracy",
        "Fixes music and effects in the few games that need it — uses more battery", false),
      Setting.Toggle("setting_hrtf", "3D Headphone Audio",
        "Positional surround sound tuned for the headset speakers", false),
      Setting.Toggle("setting_voice_chat", "Xbox Live Voice Chat",
        "Use the headset microphone for in-game voice on Insignia", false),
    )),
    Section("NETWORK", listOf(
      Setting.Toggle("setting_network_enable", "Online Play (Insignia)",
        "Connect to the Insignia service for Xbox Live-era online play", false),
    )),
  )

  init {
    reloadGames()
    inspectOnlineStatusOnce()
  }

  private fun resolveHddFileForOnline(): File? =
    prefs.getString("hddPath", null)?.let(::File)?.takeIf { it.isFile }

  private fun resolveEepromFileForOnline(): File {
    val base = appContext.getExternalFilesDir(null) ?: appContext.filesDir
    return File(File(base, "x1box"), "eeprom.bin")
  }

  private fun inspectOnlineStatusOnce() {
    Thread {
      runCatching {
        onlineStatus = OnlineStatus(
          hasHdd = resolveHddFileForOnline() != null,
          hasEeprom = resolveEepromFileForOnline().isFile,
        )
        dirty = true
      }
    }.apply { isDaemon = true }.start()
  }

  /**
   * Deferred Insignia preparation: the Online page only marks preparation as
   * pending, because the DNS writes touch the HDD image and EEPROM, which must
   * not be modified while xemu owns them. This runs in startEmulator(), the
   * one point where the emulator is guaranteed not to be running yet.
   */
  private fun applyPendingInsigniaPrepare() {
    if (!prefs.getBoolean("insignia_prepare_pending", false)) return
    try {
      val hddFile = resolveHddFileForOnline()
        ?: throw IllegalStateException("No hard-drive image found")
      val eepromFile = resolveEepromFileForOnline()
      if (!eepromFile.isFile) throw IllegalStateException("No console EEPROM found")
      XboxInsigniaHelper.applyConfigSectorDns(hddFile)
      XboxEepromEditor.applyXboxLiveDns(eepromFile, XboxInsigniaHelper.primaryDnsBytes())
      prefs.edit()
        .putBoolean("insignia_prepare_pending", false)
        .putLong("insignia_prepared_ms", System.currentTimeMillis())
        .remove("insignia_prepare_error")
        .apply()
    } catch (t: Throwable) {
      // Always clear pending (no silent retry loops); surface the reason.
      prefs.edit()
        .putBoolean("insignia_prepare_pending", false)
        .putString("insignia_prepare_error", t.message ?: t.javaClass.simpleName)
        .apply()
    }
  }

  // ---------------------------------------------------------------------
  // JNI surface — lifecycle & model
  // ---------------------------------------------------------------------

  /**
   * Invert the old launch order: the already-immersive NativeActivity owns the
   * process first, then starts SDL/xemu in-process and immediately returns
   * itself to the foreground. The native bootstrap independently waits for and
   * verifies the immersive cpuset before it creates QEMU workers.
   */
  fun startEmulator() {
    // Safe point for deferred Insignia DNS writes: xemu is not running yet.
    applyPendingInsigniaPrepare()
    // Voice chat needs the microphone; ask once here so the in-VR system
    // dialog appears before the game grabs focus. Denial is non-fatal (the
    // communicator captures silence until granted on a later boot).
    if (prefs.getBoolean("setting_voice_chat", false) &&
      appContext.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) !=
      android.content.pm.PackageManager.PERMISSION_GRANTED
    ) {
      activity.runOnUiThread {
        activity.requestPermissions(
          arrayOf(android.Manifest.permission.RECORD_AUDIO), 7301)
      }
    }
    activity.runOnUiThread {
      val emulatorIntent = Intent(activity, XrEmulatorActivity::class.java).apply {
        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        activity.intent?.extras?.let(::putExtras)
      }
      activity.startActivity(emulatorIntent)
    }
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

  fun isDirty(): Boolean {
    pollStatus()
    return dirty
  }

  /**
   * Consume the one-shot auto-boot request. Every deliberate launch (frontend
   * intent, 2D picker, staged bench prefs) sets xr_pending_boot=true; a plain
   * app launch leaves it unset so the shell opens on the library instead of
   * cold-booting the emulator with no game chosen.
   */
  fun shouldAutoBoot(): Boolean {
    val pending = prefs.getBoolean("xr_pending_boot", false)
    if (pending) {
      prefs.edit().putBoolean("xr_pending_boot", false).commit()
    }
    return pending
  }

  /** Native pushes whether the emulator bootstrap has run (on menu open). */
  fun setEmulatorState(running: Boolean) {
    if (running != emulatorRunning) {
      emulatorRunning = running
      if (!visiblePages().contains(page)) {
        switchPage(PAGE_LIBRARY)
      }
      dirty = true
    }
  }

  /** Native lands the wearer here after quit-to-library. */
  fun showLibrary() {
    switchPage(PAGE_LIBRARY)
  }

  private fun visiblePages(): IntArray =
    if (emulatorRunning) intArrayOf(PAGE_LIBRARY, PAGE_SYSTEM)
    else intArrayOf(PAGE_LIBRARY, PAGE_SETTINGS, PAGE_ONLINE, PAGE_SYSTEM)

  /** Push the running process's active FP JIT mode (called from native on open). */
  fun setActiveFpJit(known: Boolean, value: Boolean) {
    if (known != activeFpJitKnown || value != activeFpJitValue) {
      activeFpJitKnown = known
      activeFpJitValue = value
      dirty = true
    }
  }

  /** Shell pushes the guest flip EWMA (0 = unknown/stale) about once a second. */
  fun setGuestFrameMs(ms: Float) {
    if (abs(ms - guestFrameMs) > 0.15f) {
      guestFrameMs = ms
      if (page == PAGE_SYSTEM) dirty = true
    }
  }

  /** Shell pushes the voice-chat mic state: -1 = voice off, 0 = live, 1 = muted. */
  fun setMicState(state: Int) {
    if (state != micState) {
      micState = state
      if (page == PAGE_SYSTEM) dirty = true
    }
  }

  /** Stable per-game window-placement key: the active disc's filename. */
  private fun windowKey(): String =
    prefs.getString("dvdPath", null)?.let { File(it).name } ?: "default"

  /**
   * Saved 6DOF placement for the active game's window as [px, py, pz, qx, qy,
   * qz, qw, width_m]. Falls back to the last placement any game saved; empty
   * when no window has ever been moved. Height is never stored — it always
   * derives from the live guest 4:3/16:9 aspect in the shell.
   */
  fun getWindowPlacement(): FloatArray {
    val raw = windowPrefs.getString("win:${windowKey()}", null)
      ?: windowPrefs.getString("win:default", null)
      ?: return FloatArray(0)
    val parts = raw.split(",")
    if (parts.size != 8) return FloatArray(0)
    val out = FloatArray(8)
    for (i in parts.indices) {
      out[i] = parts[i].toFloatOrNull() ?: return FloatArray(0)
    }
    return out
  }

  /** Persist the active game's window placement (also the new-game default). */
  fun saveWindowPlacement(px: Float, py: Float, pz: Float,
                          qx: Float, qy: Float, qz: Float, qw: Float,
                          width: Float) {
    val v = listOf(px, py, pz, qx, qy, qz, qw, width).joinToString(",")
    windowPrefs.edit()
      .putString("win:${windowKey()}", v)
      .putString("win:default", v)
      .apply()
  }

  /** Native drains one queued UI command per call (CMD_*). */
  fun consumeCommand(): Int {
    val cmd = pendingCommand
    pendingCommand = CMD_NONE
    return cmd
  }

  // ---------------------------------------------------------------------
  // JNI surface — input
  // ---------------------------------------------------------------------

  /** Geometric focus move (gamepad dpad / left stick), dx/dy in {-1,0,1}. */
  fun moveSelection(dx: Int, dy: Int) {
    if (dx == 0 && dy == 0) return
    val current = focusables.find { it.id == focusId }
    if (current == null) {
      focusId = focusables.firstOrNull()?.id
      dirty = true
      return
    }
    // On a segmented control, horizontal input cycles the value in place.
    if (dx != 0 && current.id.startsWith("set:")) {
      val s = findSetting(current.id.removePrefix("set:"))
      if (s != null && s !is Setting.Toggle) {
        cycleSetting(s, dx)
        return
      }
    }
    val cx = current.rect.centerX()
    val cy = current.rect.centerY()
    var best: Focusable? = null
    var bestScore = Float.MAX_VALUE
    for (f in focusables) {
      if (f.id == current.id) continue
      val fx = f.rect.centerX() - cx
      val fy = f.rect.centerY() - cy
      val along = fx * dx + fy * dy
      if (along <= 1f) continue // must lie in the requested direction
      val ortho = abs(fx * dy) + abs(fy * dx)
      val score = along + ortho * 2.5f
      if (score < bestScore) {
        bestScore = score
        best = f
      }
    }
    if (best != null) {
      setFocus(best.id)
    }
  }

  /** LB/RB page cycling (within the pages the current mode offers). */
  fun navPage(delta: Int) {
    val pages = visiblePages()
    val cur = pages.indexOf(page).coerceAtLeast(0)
    val next = pages[((cur + delta) % pages.size + pages.size) % pages.size]
    if (next != page) {
      switchPage(next)
    }
  }

  /**
   * Pointer ray hit in panel pixels. Returns true while the pointer is over
   * an interactive element (native shows the cursor either way; the return
   * value is informational). Press edge activates the hovered element.
   */
  fun pointer(x: Float, y: Float, pressed: Boolean): Boolean {
    pointerInside = true
    val hit = focusables.lastOrNull { it.rect.contains(x, y) }
    val newHover = hit?.id
    if (newHover != hoverId) {
      hoverId = newHover
      if (newHover != null) focusId = newHover
      dirty = true
    }
    if (pressed) {
      if (pressedId == null && hit != null) {
        pressedId = hit.id
        dirty = true
      }
    } else {
      if (pressedId != null) {
        val released = pressedId
        pressedId = null
        if (released != null && released == hit?.id) {
          activateId(released)
        }
        dirty = true
      }
    }
    return hit != null
  }

  fun pointerExit() {
    if (hoverId != null || pressedId != null) {
      hoverId = null
      pressedId = null
      dirty = true
    }
    pointerInside = false
  }

  /** Smooth scroll (thumbstick), dy in panel pixels. */
  fun scroll(dy: Float) {
    when (page) {
      PAGE_LIBRARY -> {
        val next = (libraryScroll + dy).coerceIn(0f, libraryMaxScroll)
        if (next != libraryScroll) {
          libraryScroll = next
          dirty = true
        }
      }
      PAGE_SETTINGS -> {
        val next = (settingsScroll + dy).coerceIn(0f, settingsMaxScroll)
        if (next != settingsScroll) {
          settingsScroll = next
          dirty = true
        }
      }
    }
  }

  /** Contextual face buttons: 0=A (activate), 1=X (FP JIT), 2=Y (unused). */
  fun gamepadButton(code: Int) {
    when (code) {
      0 -> focusId?.let { activateId(it) }
      1 -> if (page == PAGE_LIBRARY) toggleFpJit()
    }
  }

  /** Debug/autotest: select the entry whose file name matches. */
  fun selectByName(name: String): Boolean {
    val idx = games.indexOfFirst { File(it.path).name == name }
    if (idx < 0) return false
    selected = idx
    if (page != PAGE_LIBRARY) switchPage(PAGE_LIBRARY)
    setFocus("game:$idx")
    return true
  }

  /** Flip the per-game FP JIT override for the focused/selected title. */
  fun toggleFpJit() {
    val idx = focusedGameIndex()
    val game = games.getOrNull(idx) ?: return
    // Merge into existing overrides — saveOverrides rewrites every key, so a
    // partial map would wipe the game's other per-game settings.
    val merged = PerGameSettingsManager.loadOverrides(appContext, game.relativePath).toMutableMap()
    val enabled = merged["setting_hard_fpu"] != "true"
    merged["setting_hard_fpu"] = if (enabled) "true" else "false"
    PerGameSettingsManager.saveOverrides(appContext, game.relativePath, merged)
    dirty = true
  }

  /**
   * Commit the selected game as the active title and return its filesystem
   * path for the shell to hand to xemu_xr_request_load_disc(). Also flushes the
   * disc selection + per-game overrides into prefs so a later cold boot (or the
   * 2D launcher) lands on the same game with the same settings.
   */
  fun activate(): String? {
    val game = games.getOrNull(selected) ?: return null
    // Per-game runtime overrides matter only on a future cold boot -> async apply().
    prefs.edit().also {
      PerGameSettingsManager.applyRuntimeOverridesToEditor(appContext, it, game.relativePath)
    }.apply()
    // dvdPath must be durable before the guest reset can boot the new game -> commit().
    prefs.edit().putString("dvdPath", game.path).remove("dvdUri").commit()
    return game.path
  }

  // ---------------------------------------------------------------------
  // Action dispatch
  // ---------------------------------------------------------------------

  private fun focusedGameIndex(): Int {
    val id = focusId ?: return selected
    return if (id.startsWith("game:")) id.removePrefix("game:").toIntOrNull() ?: selected
    else selected
  }

  private fun setFocus(id: String) {
    if (focusId != id) {
      focusId = id
      dirty = true
    }
    ensureFocusVisible()
  }

  private fun switchPage(next: Int) {
    if (!visiblePages().contains(next)) {
      return
    }
    page = next
    hoverId = null
    pressedId = null
    focusId = "nav:$next"
    dirty = true
  }

  private fun activateId(id: String) {
    when {
      id.startsWith("nav:") -> {
        val p = id.removePrefix("nav:").toIntOrNull() ?: return
        switchPage(p)
        focusId = "nav:$p"
      }
      id.startsWith("game:") -> {
        val idx = id.removePrefix("game:").toIntOrNull() ?: return
        if (idx in games.indices) {
          selected = idx
          pendingCommand = CMD_LAUNCH
        }
      }
      id.startsWith("set:") -> {
        val s = findSetting(id.removePrefix("set:")) ?: return
        when (s) {
          is Setting.Toggle -> {
            val cur = prefs.getBoolean(s.key, s.def)
            prefs.edit().putBoolean(s.key, !cur).apply()
            changedKeys.add(s.key)
            dirty = true
          }
          else -> cycleSetting(s, 1)
        }
      }
      id == "btn:rescan" -> refresh()
      id == "sys:recenter" -> { pendingCommand = CMD_RECENTER; dirty = true }
      id == "sys:quit" -> pendingCommand = CMD_QUIT_TO_DASHBOARD
      id == "sys:mic" -> { pendingCommand = CMD_MIC_TOGGLE; dirty = true }
      id == "sys:close" -> pendingCommand = CMD_CLOSE
      id == "ins:net" -> {
        val cur = prefs.getBoolean("setting_network_enable", false)
        prefs.edit().putBoolean("setting_network_enable", !cur).apply()
        changedKeys.add("setting_network_enable")
        dirty = true
      }
      id == "ins:prepare" -> {
        // Mark only: the HDD/EEPROM writes run at the next emulator start,
        // when xemu does not own the files (see applyPendingInsigniaPrepare).
        prefs.edit()
          .putBoolean("setting_network_enable", true)
          .putBoolean("insignia_prepare_pending", true)
          .apply()
        changedKeys.add("setting_network_enable")
        dirty = true
      }
      id == "ins:setup" -> {
        val idx = games.indexOfFirst {
          it.title.contains("insignia", true) ||
            it.title.contains("setup assistant", true)
        }
        if (idx >= 0) {
          selected = idx
          pendingCommand = CMD_LAUNCH
        }
      }
    }
  }

  private fun findSetting(key: String): Setting? {
    for (sec in sections) {
      sec.items.find { it.key == key }?.let { return it }
    }
    return null
  }

  private fun cycleSetting(s: Setting, dir: Int) {
    when (s) {
      is Setting.SegInt -> {
        val cur = prefs.getInt(s.key, s.def)
        val i = s.options.indexOf(cur).let { if (it < 0) 0 else it }
        val next = s.options[((i + dir) % s.options.size + s.options.size) % s.options.size]
        prefs.edit().putInt(s.key, next).apply()
      }
      is Setting.SegStr -> {
        val cur = prefs.getString(s.key, s.def) ?: s.def
        val i = s.options.indexOf(cur).let { if (it < 0) 0 else it }
        val next = s.options[((i + dir) % s.options.size + s.options.size) % s.options.size]
        prefs.edit().putString(s.key, next).apply()
      }
      is Setting.Toggle -> {
        val cur = prefs.getBoolean(s.key, s.def)
        prefs.edit().putBoolean(s.key, !cur).apply()
      }
    }
    changedKeys.add(s.key)
    dirty = true
  }

  private fun ensureFocusVisible() {
    val f = focusables.find { it.id == focusId } ?: return
    if (!f.id.startsWith("game:") && !f.id.startsWith("set:")) return
    val margin = 24f
    if (f.rect.top < contentTop + margin) {
      scroll(f.rect.top - contentTop - margin)
    } else if (f.rect.bottom > contentBottom - margin) {
      scroll(f.rect.bottom - contentBottom + margin)
    }
  }

  // ---------------------------------------------------------------------
  // Status polling (clock / battery)
  // ---------------------------------------------------------------------

  private fun pollStatus() {
    val now = System.currentTimeMillis()
    if (now - lastStatusPollMs < 5000) return
    lastStatusPollMs = now
    val clock = SimpleDateFormat("h:mm", Locale.US).format(Date(now))
    if (clock != clockText) {
      clockText = clock
      dirty = true
    }
    try {
      val intent = appContext.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
      if (intent != null) {
        val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val pct = if (level >= 0 && scale > 0) level * 100 / scale else -1
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
          status == BatteryManager.BATTERY_STATUS_FULL
        if (pct != batteryPct || charging != batteryCharging) {
          batteryPct = pct
          batteryCharging = charging
          dirty = true
        }
      }
    } catch (_: Exception) {
    }
  }

  // ---------------------------------------------------------------------
  // Rendering
  // ---------------------------------------------------------------------

  /** (Re)draw the menu into a cached bitmap sized [w] x [h] and return it. */
  fun render(w: Int, h: Int): Bitmap? {
    if (w <= 0 || h <= 0) return null
    var bmp = bitmap
    if (bmp == null || bmp.width != w || bmp.height != h) {
      bmp?.recycle()
      bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
      bitmap = bmp
    }
    drawInto(bmp, w.toFloat(), h.toFloat())
    dirty = false
    dumpIfRequested(bmp)
    return bmp
  }

  private fun dumpIfRequested(bmp: Bitmap) {
    val dir = System.getenv("XEMU_DUMP_MENU")?.takeIf { it.isNotBlank() } ?: return
    try {
      File(dir).mkdirs()
      val out = File(dir, "menu_%04d.png".format(dumpCounter++))
      FileOutputStream(out).use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
    } catch (_: Exception) {
    }
  }

  private fun drawInto(bmp: Bitmap, w: Float, h: Float) {
    val c = Canvas(bmp)
    c.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)
    focusables.clear()

    // Panel body + hairline stroke + soft top sheen.
    val panel = RectF(2f, 2f, w - 2f, h - 2f)
    fill.shader = null
    fill.color = Th.PANEL_BG
    c.drawRoundRect(panel, Th.RADIUS, Th.RADIUS, fill)
    fill.shader = LinearGradient(0f, 0f, 0f, h * 0.24f, 0x0FFFFFFF, 0x00FFFFFF,
      Shader.TileMode.CLAMP)
    c.drawRoundRect(RectF(2f, 2f, w - 2f, h * 0.24f), Th.RADIUS, Th.RADIUS, fill)
    fill.shader = null
    stroke.color = Th.PANEL_STROKE
    stroke.strokeWidth = 2.5f
    c.drawRoundRect(panel, Th.RADIUS, Th.RADIUS, stroke)

    drawNavRail(c, h)
    drawStatusCluster(c, w)

    contentTop = 128f
    contentBottom = h - 96f
    val contentLeft = Th.RAIL_W + 40f
    val contentRight = w - 48f

    c.save()
    c.clipRect(contentLeft - 20f, contentTop - 62f, contentRight + 20f, contentBottom + 6f)
    when (page) {
      PAGE_LIBRARY -> drawLibrary(c, contentLeft, contentRight)
      PAGE_SETTINGS -> drawSettings(c, contentLeft, contentRight)
      PAGE_ONLINE -> drawOnline(c, contentLeft, contentRight)
      PAGE_SYSTEM -> drawSystem(c, contentLeft, contentRight)
    }
    c.restore()

    drawFooter(c, w, h, contentLeft)
  }

  // --- chrome ---

  private fun drawNavRail(c: Canvas, h: Float) {
    stroke.color = Th.RAIL_DIVIDER
    stroke.strokeWidth = 2f
    c.drawLine(Th.RAIL_W, 28f, Th.RAIL_W, h - 28f, stroke)

    // App mark.
    fill.color = Th.ACCENT
    c.drawCircle(52f, 66f, 11f, fill)
    text.typeface = tfMedium
    text.textSize = 34f
    text.color = Th.TEXT_PRIMARY
    text.textAlign = Paint.Align.LEFT
    c.drawText("XEMU", 76f, 78f, text)
    text.typeface = tfRegular
    text.textSize = 20f
    text.color = Th.TEXT_TERTIARY
    c.drawText("QUEST EDITION", 77f, 104f, text)

    val labels = arrayOf("Library", "Settings", "Online", "System")
    val pages = visiblePages()
    for ((slot, i) in pages.withIndex()) {
      val top = 170f + slot * 88f
      val r = RectF(20f, top, Th.RAIL_W - 20f, top + 72f)
      val id = "nav:$i"
      focusables.add(Focusable(id, r))
      val isPage = page == i
      val isFocus = focusId == id
      if (isPage || isFocus || hoverId == id) {
        fill.color = when {
          pressedId == id -> Th.PRESS_FILL
          isPage -> 0x1FFFFFFF
          else -> Th.HOVER_FILL
        }
        c.drawRoundRect(r, 20f, 20f, fill)
      }
      if (isPage) {
        fill.color = Th.ACCENT
        c.drawRoundRect(RectF(r.left, r.centerY() - 16f, r.left + 6f, r.centerY() + 16f),
          3f, 3f, fill)
      }
      if (isFocus && !isPage) {
        stroke.color = Th.ACCENT
        stroke.strokeWidth = 2.5f
        c.drawRoundRect(r, 20f, 20f, stroke)
      }
      drawNavIcon(c, i, r.left + 42f, r.centerY(), if (isPage) Th.TEXT_PRIMARY else Th.TEXT_SECONDARY)
      text.typeface = tfMedium
      text.textSize = 28f
      text.color = if (isPage) Th.TEXT_PRIMARY else Th.TEXT_SECONDARY
      c.drawText(labels[i], r.left + 74f, r.centerY() + 10f, text)
    }
  }

  private fun drawNavIcon(c: Canvas, which: Int, cx: Float, cy: Float, color: Int) {
    stroke.color = color
    stroke.strokeWidth = 3f
    stroke.strokeCap = Paint.Cap.ROUND
    when (which) {
      0 -> { // library: 2x2 grid
        val s = 8f
        for (ix in 0..1) for (iy in 0..1) {
          val x = cx - 10f + ix * (s + 5f)
          val y = cy - 10f + iy * (s + 5f)
          c.drawRoundRect(RectF(x, y, x + s, y + s), 2.5f, 2.5f, stroke)
        }
      }
      1 -> { // settings: sliders
        for (i in 0..2) {
          val y = cy - 9f + i * 9f
          c.drawLine(cx - 12f, y, cx + 12f, y, stroke)
          fill.color = color
          val kx = cx + (if (i == 1) -6f else 6f)
          c.drawCircle(kx, y, 4.2f, fill)
        }
      }
      2 -> { // online: globe
        c.drawCircle(cx, cy, 12f, stroke)
        c.drawOval(RectF(cx - 5.5f, cy - 12f, cx + 5.5f, cy + 12f), stroke)
        c.drawLine(cx - 12f, cy, cx + 12f, cy, stroke)
      }
      3 -> { // system: pulse line
        val p = Path()
        p.moveTo(cx - 13f, cy)
        p.lineTo(cx - 5f, cy)
        p.lineTo(cx - 1f, cy - 10f)
        p.lineTo(cx + 3f, cy + 9f)
        p.lineTo(cx + 6f, cy)
        p.lineTo(cx + 13f, cy)
        c.drawPath(p, stroke)
      }
    }
  }

  private fun drawStatusCluster(c: Canvas, w: Float) {
    var x = w - 48f
    // Battery.
    if (batteryPct >= 0) {
      val bw = 46f
      val bh = 22f
      val by = 56f
      val body = RectF(x - bw, by, x, by + bh)
      stroke.color = Th.TEXT_SECONDARY
      stroke.strokeWidth = 2.5f
      c.drawRoundRect(body, 6f, 6f, stroke)
      fill.color = Th.TEXT_SECONDARY
      c.drawRoundRect(RectF(x + 2f, by + 6f, x + 6f, by + bh - 6f), 2f, 2f, fill)
      val frac = batteryPct / 100f
      fill.color = if (batteryPct <= 20 && !batteryCharging) Th.DANGER else Th.ACCENT
      c.drawRoundRect(RectF(body.left + 3.5f, by + 3.5f,
        body.left + 3.5f + (bw - 7f) * frac, by + bh - 3.5f), 3.5f, 3.5f, fill)
      if (batteryCharging) {
        val bolt = Path()
        val bx = body.centerX()
        bolt.moveTo(bx + 3f, by - 3f)
        bolt.lineTo(bx - 5f, by + bh * 0.62f)
        bolt.lineTo(bx - 0.5f, by + bh * 0.62f)
        bolt.lineTo(bx - 3f, by + bh + 3f)
        bolt.lineTo(bx + 5f, by + bh * 0.38f)
        bolt.lineTo(bx + 0.5f, by + bh * 0.38f)
        bolt.close()
        fill.color = Th.TEXT_PRIMARY
        c.drawPath(bolt, fill)
      }
      text.typeface = tfRegular
      text.textSize = 24f
      text.color = Th.TEXT_SECONDARY
      text.textAlign = Paint.Align.RIGHT
      c.drawText("$batteryPct%", body.left - 12f, by + bh - 3f, text)
      x = body.left - 12f - text.measureText("$batteryPct%") - 28f
    }
    // Clock.
    if (clockText.isNotEmpty()) {
      text.typeface = tfMedium
      text.textSize = 26f
      text.color = Th.TEXT_SECONDARY
      text.textAlign = Paint.Align.RIGHT
      c.drawText(clockText, x, 76f, text)
    }
    text.textAlign = Paint.Align.LEFT
  }

  private fun drawFooter(c: Canvas, w: Float, h: Float, left: Float) {
    val y = h - 48f
    var x = left
    fun hint(letter: String, color: Int, label: String) {
      fill.color = 0xFF22262C.toInt()
      c.drawCircle(x + 15f, y - 8f, 15f, fill)
      stroke.color = color
      stroke.strokeWidth = 2.5f
      c.drawCircle(x + 15f, y - 8f, 15f, stroke)
      text.typeface = tfBold
      text.textSize = 18f
      text.color = color
      text.textAlign = Paint.Align.CENTER
      c.drawText(letter, x + 15f, y - 1.5f, text)
      text.typeface = tfRegular
      text.textSize = 23f
      text.color = Th.TEXT_TERTIARY
      text.textAlign = Paint.Align.LEFT
      c.drawText(label, x + 38f, y, text)
      x += 38f + text.measureText(label) + 34f
    }
    when (page) {
      PAGE_LIBRARY -> {
        hint("A", Th.BTN_A, "Play")
        hint("X", Th.BTN_X, "Compat Mode")
        hint("B", Th.BTN_B, "Close")
      }
      PAGE_SETTINGS -> {
        hint("A", Th.BTN_A, "Change")
        hint("B", Th.BTN_B, "Close")
      }
      else -> {
        hint("A", Th.BTN_A, "Select")
        hint("B", Th.BTN_B, "Close")
      }
    }
    text.typeface = tfRegular
    text.textSize = 23f
    text.color = Th.TEXT_TERTIARY
    text.textAlign = Paint.Align.RIGHT
    c.drawText("Point · pull trigger to select", w - 48f, y, text)
    text.textAlign = Paint.Align.LEFT
  }

  // --- library page ---

  private fun drawLibrary(c: Canvas, left: Float, right: Float) {
    val list = games // snapshot: scan thread may swap it mid-draw
    if (selected >= list.size) selected = (list.size - 1).coerceAtLeast(0)

    text.typeface = tfMedium
    text.textSize = 44f
    text.color = Th.TEXT_PRIMARY
    c.drawText("Library", left, contentTop - 12f, text)
    val tw = text.measureText("Library")
    text.typeface = tfRegular
    text.textSize = 24f
    text.color = Th.TEXT_TERTIARY
    val countLabel = when {
      scanning -> "scanning…"
      list.size == 1 -> "1 game"
      else -> "${list.size} games"
    }
    c.drawText(countLabel, left + tw + 18f, contentTop - 12f, text)

    // Rescan pill.
    text.typeface = tfMedium
    text.textSize = 24f
    val rl = "Rescan"
    val rw = text.measureText(rl) + 76f
    val rr = RectF(right - rw, contentTop - 52f, right, contentTop - 4f)
    focusables.add(Focusable("btn:rescan", rr))
    fill.color = when {
      pressedId == "btn:rescan" -> Th.PRESS_FILL
      hoverId == "btn:rescan" || focusId == "btn:rescan" -> Th.HOVER_FILL
      else -> Th.CARD_BG
    }
    c.drawRoundRect(rr, 24f, 24f, fill)
    if (focusId == "btn:rescan") {
      stroke.color = Th.ACCENT
      stroke.strokeWidth = 2.5f
      c.drawRoundRect(rr, 24f, 24f, stroke)
    }
    // refresh arrow glyph
    stroke.color = Th.TEXT_SECONDARY
    stroke.strokeWidth = 3f
    val gx = rr.left + 28f
    val gy = rr.centerY()
    val arc = RectF(gx - 10f, gy - 10f, gx + 10f, gy + 10f)
    c.drawArc(arc, -40f, 280f, false, stroke)
    fill.color = Th.TEXT_SECONDARY
    val tri = Path()
    tri.moveTo(gx + 12f, gy - 12f)
    tri.lineTo(gx + 3f, gy - 9f)
    tri.lineTo(gx + 11f, gy - 1f)
    tri.close()
    c.drawPath(tri, fill)
    text.color = Th.TEXT_SECONDARY
    c.drawText(rl, gx + 20f, rr.centerY() + 9f, text)

    if (list.isEmpty()) {
      text.typeface = tfMedium
      text.textSize = 32f
      text.color = Th.TEXT_SECONDARY
      val msg = if (scanning) "Scanning library…" else "No games found"
      c.drawText(msg, left, contentTop + 120f, text)
      if (!scanning) {
        text.typeface = tfRegular
        text.textSize = 24f
        text.color = Th.TEXT_TERTIARY
        c.drawText("Drop .iso files in /sdcard/Download/xemu-games and rescan.",
          left, contentTop + 160f, text)
      }
      libraryMaxScroll = 0f
      return
    }

    val cols = 5
    val gap = 26f
    val cellW = (right - left - gap * (cols - 1)) / cols
    val coverH = cellW * 1.4f
    val rowH = coverH + 64f
    val gridTop = contentTop + 26f
    val rows = ceil(list.size / cols.toFloat())
    libraryMaxScroll = max(0f, rows * rowH + gap - (contentBottom - gridTop))
    libraryScroll = libraryScroll.coerceIn(0f, libraryMaxScroll)

    val currentDvd = prefs.getString("dvdPath", null)

    // Scrolled cards must never paint over the fixed header band.
    c.save()
    c.clipRect(left - 12f, contentTop + 2f, right + 28f, contentBottom + 2f)

    for (i in list.indices) {
      val col = i % cols
      val row = i / cols
      val cx = left + col * (cellW + gap)
      val cy = gridTop + row * rowH - libraryScroll
      val id = "game:$i"
      focusables.add(Focusable(id, RectF(cx, cy, cx + cellW, cy + rowH - 10f)))
      if (cy + rowH < contentTop - 40f || cy > contentBottom + 40f) continue
      val cover = RectF(cx, cy, cx + cellW, cy + coverH)
      val game = list[i]
      val focused = focusId == id
      val hovered = hoverId == id

      // Cover (art or procedural placeholder), rounded-clipped.
      c.save()
      val clip = Path()
      clip.addRoundRect(cover, 16f, 16f, Path.Direction.CW)
      c.clipPath(clip)
      val art = covers.get(game.title, cellW.toInt(), coverH.toInt())
      if (art != null) {
        val scale = max(cover.width() / art.width, cover.height() / art.height)
        val dw = art.width * scale
        val dh = art.height * scale
        val dst = RectF(cover.centerX() - dw / 2f, cover.centerY() - dh / 2f,
          cover.centerX() + dw / 2f, cover.centerY() + dh / 2f)
        c.drawBitmap(art, null, dst, fill.apply { color = Color.WHITE; shader = null })
        if (pressedId == id) {
          fill.color = 0x33000000
          c.drawRect(cover, fill)
        }
      } else {
        drawPlaceholderCover(c, cover, game.displayTitle, pressedId == id)
      }
      c.restore()

      stroke.color = Th.CARD_STROKE
      stroke.strokeWidth = 2f
      c.drawRoundRect(cover, 16f, 16f, stroke)
      if (focused || hovered) {
        stroke.color = Th.ACCENT
        stroke.strokeWidth = 3.5f
        val glow = RectF(cover)
        glow.inset(-2f, -2f)
        c.drawRoundRect(glow, 18f, 18f, stroke)
        stroke.color = Th.ACCENT_DIM
        stroke.strokeWidth = 9f
        glow.inset(-4f, -4f)
        c.drawRoundRect(glow, 21f, 21f, stroke)
      }

      // Badges.
      if (isFpJit(game)) {
        text.typeface = tfBold
        text.textSize = 17f
        val bw = text.measureText("FP") + 18f
        val br = RectF(cover.right - bw - 10f, cover.top + 10f, cover.right - 10f,
          cover.top + 38f)
        fill.color = 0xD0121821.toInt()
        c.drawRoundRect(br, 9f, 9f, fill)
        text.color = Th.ACCENT
        text.textAlign = Paint.Align.CENTER
        c.drawText("FP", br.centerX(), br.centerY() + 6f, text)
        text.textAlign = Paint.Align.LEFT
      }
      if (emulatorRunning && currentDvd != null && currentDvd == game.path) {
        text.typeface = tfBold
        text.textSize = 17f
        val label = "PLAYING"
        val bw = text.measureText(label) + 24f
        val br = RectF(cover.centerX() - bw / 2f, cover.bottom - 40f,
          cover.centerX() + bw / 2f, cover.bottom - 12f)
        fill.color = Th.ACCENT
        c.drawRoundRect(br, 14f, 14f, fill)
        text.color = 0xFF0D1117.toInt()
        text.textAlign = Paint.Align.CENTER
        c.drawText(label, br.centerX(), br.centerY() + 6f, text)
        text.textAlign = Paint.Align.LEFT
      }

      // Title.
      text.typeface = if (focused || hovered) tfMedium else tfRegular
      text.textSize = 23f
      text.color = if (focused || hovered) Th.TEXT_PRIMARY else Th.TEXT_SECONDARY
      text.textAlign = Paint.Align.CENTER
      c.drawText(ellipsize(game.displayTitle, text, cellW - 8f), cover.centerX(),
        cy + coverH + 36f, text)
      text.textAlign = Paint.Align.LEFT
    }

    drawScrollbar(c, right, libraryScroll, libraryMaxScroll)
    c.restore()

    // FP JIT cold-launch note (matches the old picker's warning).
    val focusedGame = list.getOrNull(focusedGameIndex())
    if (focusedGame != null && activeFpJitKnown && isFpJit(focusedGame) != activeFpJitValue) {
      text.typeface = tfRegular
      text.textSize = 22f
      text.color = Th.AMBER
      c.drawText("Compatibility change applies at next launch", left, contentBottom - 6f, text)
    }
  }

  private fun drawPlaceholderCover(c: Canvas, r: RectF, title: String, pressed: Boolean) {
    val hue = ((title.hashCode() % 360) + 360) % 360
    val top = Color.HSVToColor(floatArrayOf(hue.toFloat(), 0.42f, if (pressed) 0.24f else 0.30f))
    val bottom = Color.HSVToColor(floatArrayOf(hue.toFloat(), 0.52f, if (pressed) 0.10f else 0.13f))
    fill.shader = LinearGradient(r.left, r.top, r.left, r.bottom, top, bottom,
      Shader.TileMode.CLAMP)
    c.drawRect(r, fill)
    fill.shader = null
    val words = title.split(' ').filter { it.isNotBlank() && it.first().isLetterOrDigit() }
    val initials = words.take(2).mapNotNull { it.firstOrNull()?.uppercaseChar() }
      .joinToString("")
    text.typeface = tfBold
    text.textSize = r.width() * 0.36f
    text.color = 0x30FFFFFF
    text.textAlign = Paint.Align.CENTER
    c.drawText(initials, r.centerX(), r.centerY() + text.textSize * 0.32f, text)
    text.textAlign = Paint.Align.LEFT
  }

  // --- settings page ---

  private fun drawSettings(c: Canvas, left: Float, right: Float) {
    text.typeface = tfMedium
    text.textSize = 44f
    text.color = Th.TEXT_PRIMARY
    c.drawText("Settings", left, contentTop - 12f, text)
    text.typeface = tfRegular
    text.textSize = 22f
    text.color = Th.TEXT_TERTIARY
    text.textAlign = Paint.Align.RIGHT
    c.drawText("Changes apply the next time a game boots", right, contentTop - 16f, text)
    text.textAlign = Paint.Align.LEFT

    val rowH = 96f
    val headH = 64f
    var y = contentTop + 20f - settingsScroll
    var totalH = 20f

    // Scrolled rows must never paint over the fixed header band.
    c.save()
    c.clipRect(left - 12f, contentTop + 2f, right + 28f, contentBottom + 2f)

    for (sec in sections) {
      // Section header.
      if (y + headH > contentTop - 20f && y < contentBottom + 20f) {
        text.typeface = tfMedium
        text.textSize = 21f
        text.color = Th.TEXT_TERTIARY
        c.drawText(sec.title, left + 6f, y + headH - 18f, text)
      }
      y += headH
      totalH += headH
      for (s in sec.items) {
        val r = RectF(left, y, right, y + rowH - 10f)
        if (r.bottom > contentTop - 20f && r.top < contentBottom + 20f) {
          drawSettingRow(c, r, s)
        }
        focusables.add(Focusable("set:${s.key}", r))
        y += rowH
        totalH += rowH
      }
    }
    totalH += 8f
    settingsMaxScroll = max(0f, totalH - (contentBottom - contentTop))
    settingsScroll = settingsScroll.coerceIn(0f, settingsMaxScroll)

    drawScrollbar(c, right, settingsScroll, settingsMaxScroll)
    c.restore()
  }

  private fun drawSettingRow(c: Canvas, r: RectF, s: Setting) {
    val id = "set:${s.key}"
    val focused = focusId == id
    val hovered = hoverId == id
    if (focused || hovered) {
      fill.color = if (pressedId == id) Th.PRESS_FILL else Th.HOVER_FILL
      c.drawRoundRect(r, 18f, 18f, fill)
    }
    if (focused) {
      stroke.color = Th.ACCENT
      stroke.strokeWidth = 2.5f
      c.drawRoundRect(r, 18f, 18f, stroke)
    }
    val tx = r.left + 24f
    text.typeface = tfMedium
    text.textSize = 29f
    text.color = Th.TEXT_PRIMARY
    c.drawText(s.label, tx, r.top + 38f, text)
    if (changedKeys.contains(s.key)) {
      fill.color = Th.AMBER
      c.drawCircle(tx + text.measureText(s.label) + 16f, r.top + 29f, 5f, fill)
    }
    text.typeface = tfRegular
    text.textSize = 21f
    text.color = Th.TEXT_TERTIARY
    c.drawText(s.desc, tx, r.top + 68f, text)

    when (s) {
      is Setting.Toggle -> {
        val on = prefs.getBoolean(s.key, s.def)
        val tw = 84f
        val th = 44f
        val t = RectF(r.right - 24f - tw, r.centerY() - th / 2f, r.right - 24f,
          r.centerY() + th / 2f)
        fill.color = if (on) Th.ACCENT else Th.TRACK_OFF
        c.drawRoundRect(t, th / 2f, th / 2f, fill)
        fill.color = if (on) 0xFF0D1117.toInt() else 0xFFAAB4BE.toInt()
        val knobX = if (on) t.right - th / 2f else t.left + th / 2f
        c.drawCircle(knobX, t.centerY(), th / 2f - 5f, fill)
      }
      is Setting.SegInt -> drawSegmented(c, r, s.names,
        s.options.indexOf(prefs.getInt(s.key, s.def)).coerceAtLeast(0))
      is Setting.SegStr -> drawSegmented(c, r, s.names,
        s.options.indexOf(prefs.getString(s.key, s.def) ?: s.def).coerceAtLeast(0))
    }
  }

  private fun drawSegmented(c: Canvas, row: RectF, names: List<String>, active: Int) {
    text.typeface = tfMedium
    text.textSize = 23f
    val padX = 22f
    val h = 48f
    var totalW = 0f
    val widths = names.map { text.measureText(it) + padX * 2 }
    widths.forEach { totalW += it }
    val container = RectF(row.right - 24f - totalW - 8f, row.centerY() - h / 2f - 4f,
      row.right - 24f, row.centerY() + h / 2f + 4f)
    fill.color = 0x0FFFFFFF
    c.drawRoundRect(container, (h + 8f) / 2f, (h + 8f) / 2f, fill)
    var x = container.left + 4f
    for (i in names.indices) {
      val seg = RectF(x, container.top + 4f, x + widths[i], container.bottom - 4f)
      if (i == active) {
        fill.color = Th.ACCENT_DIM
        c.drawRoundRect(seg, h / 2f, h / 2f, fill)
        stroke.color = Th.ACCENT
        stroke.strokeWidth = 2f
        c.drawRoundRect(seg, h / 2f, h / 2f, stroke)
        text.color = Th.ACCENT
      } else {
        text.color = Th.TEXT_SECONDARY
      }
      text.textAlign = Paint.Align.CENTER
      c.drawText(names[i], seg.centerX(), seg.centerY() + 8f, text)
      x += widths[i]
    }
    text.textAlign = Paint.Align.LEFT
  }

  // --- system page ---

  /**
   * Online page: guided Insignia (Xbox Live revival) onboarding. Five steps
   * with live status; every actionable step is a focusable the pointer and
   * gamepad can activate. Copy avoids jargon — the user needs to know what to
   * do, not how DNS works.
   */
  private fun drawOnline(c: Canvas, left: Float, right: Float) {
    text.typeface = tfMedium
    text.textSize = 44f
    text.color = Th.TEXT_PRIMARY
    c.drawText("Online Play — Insignia", left, contentTop - 12f, text)

    val st = onlineStatus
    val netOn = prefs.getBoolean("setting_network_enable", false)
    val preparePending = prefs.getBoolean("insignia_prepare_pending", false)
    val preparedMs = prefs.getLong("insignia_prepared_ms", 0L)
    val setupIdx = games.indexOfFirst {
      it.title.contains("insignia", true) ||
        it.title.contains("setup assistant", true)
    }

    var y = contentTop + 14f
    val rowH = 118f
    val gap = 16f

    fun step(
      n: Int,
      title: String,
      detail: String,
      status: String,
      statusColor: Int,
      actionId: String?,
      actionLabel: String?,
    ) {
      val r = RectF(left, y, right, y + rowH)
      fill.color = Th.CARD_BG
      c.drawRoundRect(r, 20f, 20f, fill)
      stroke.color = Th.CARD_STROKE
      stroke.strokeWidth = 2f
      c.drawRoundRect(r, 20f, 20f, stroke)

      // Step number badge.
      fill.color = Th.ACCENT_DIM
      c.drawCircle(r.left + 46f, r.centerY(), 24f, fill)
      text.typeface = tfBold
      text.textSize = 26f
      text.color = Th.ACCENT
      text.textAlign = Paint.Align.CENTER
      c.drawText("$n", r.left + 46f, r.centerY() + 9f, text)
      text.textAlign = Paint.Align.LEFT

      text.typeface = tfMedium
      text.textSize = 28f
      text.color = Th.TEXT_PRIMARY
      c.drawText(title, r.left + 92f, r.top + 44f, text)
      text.typeface = tfRegular
      text.textSize = 21f
      text.color = Th.TEXT_SECONDARY
      c.drawText(ellipsize(detail, text, r.width() - 380f), r.left + 92f,
        r.top + 76f, text)
      text.typeface = tfRegular
      text.textSize = 21f
      text.color = statusColor
      c.drawText(ellipsize(status, text, r.width() - 380f), r.left + 92f,
        r.top + 104f, text)

      if (actionId != null && actionLabel != null) {
        text.typeface = tfMedium
        text.textSize = 24f
        val bw = text.measureText(actionLabel) + 60f
        val br = RectF(r.right - bw - 24f, r.centerY() - 30f, r.right - 24f,
          r.centerY() + 30f)
        focusables.add(Focusable(actionId, br))
        fill.color = when {
          pressedId == actionId -> Th.PRESS_FILL
          hoverId == actionId || focusId == actionId -> Th.HOVER_FILL
          else -> 0x1AFFFFFF
        }
        c.drawRoundRect(br, 30f, 30f, fill)
        if (focusId == actionId || hoverId == actionId) {
          stroke.color = Th.ACCENT
          stroke.strokeWidth = 2.5f
          c.drawRoundRect(br, 30f, 30f, stroke)
        }
        text.color = Th.TEXT_PRIMARY
        text.textAlign = Paint.Align.CENTER
        c.drawText(actionLabel, br.centerX(), br.centerY() + 8f, text)
        text.textAlign = Paint.Align.LEFT
      }
      y += rowH + gap
    }

    step(
      1, "Create a free Insignia account",
      "Visit insignia.live on your phone or computer and sign up.",
      "Accounts are free — you only need one per player.",
      Th.TEXT_TERTIARY, null, null,
    )

    step(
      2, "Turn on Online Play",
      "Lets games reach the Insignia service over your Wi-Fi.",
      if (netOn) "On" else "Off — turn this on to play online",
      if (netOn) Th.ACCENT else Th.AMBER,
      "ins:net", if (netOn) "Turn Off" else "Turn On",
    )

    val prepError = prefs.getString("insignia_prepare_error", null)
    val prepStatus: String
    val prepColor: Int
    when {
      preparePending -> {
        prepStatus = "Will be applied the next time a game starts"
        prepColor = Th.AMBER
      }
      preparedMs > 0L -> {
        prepStatus = "Done — this console is pointed at Insignia"
        prepColor = Th.ACCENT
      }
      prepError != null -> {
        prepStatus = "Setup failed: $prepError"
        prepColor = Th.DANGER
      }
      st == null -> {
        prepStatus = "Checking your console…"
        prepColor = Th.TEXT_TERTIARY
      }
      !st.hasHdd -> {
        prepStatus = "No hard-drive image found — finish first-time setup in 2D settings"
        prepColor = Th.DANGER
      }
      !st.hasEeprom -> {
        prepStatus = "No console EEPROM found — finish first-time setup in 2D settings"
        prepColor = Th.DANGER
      }
      else -> {
        prepStatus = "Not set up yet"
        prepColor = Th.AMBER
      }
    }
    step(
      3, "Point this console at Insignia",
      "One-time setup that tells the Xbox where Xbox Live lives now.",
      prepStatus, prepColor,
      if (st != null && st.hasHdd && st.hasEeprom && !preparePending)
        "ins:prepare" else null,
      if (preparedMs > 0L) "Set Up Again" else "Set Up",
    )

    step(
      4, "Register this console",
      "Boot the Insignia Setup Assistant once and follow its steps.",
      when {
        setupIdx >= 0 -> "Setup Assistant found in your library"
        else -> "Download it from insignia.live and copy it into your games folder"
      },
      if (setupIdx >= 0) Th.ACCENT else Th.TEXT_TERTIARY,
      if (setupIdx >= 0) "ins:setup" else null, "Boot It",
    )

    step(
      5, "Sign in from an online game",
      "Start a supported game and choose Xbox Live in its menus.",
      "Your account from step 1 signs in on the Xbox side.",
      Th.TEXT_TERTIARY, null, null,
    )
  }

  private fun drawSystem(c: Canvas, left: Float, right: Float) {
    text.typeface = tfMedium
    text.textSize = 44f
    text.color = Th.TEXT_PRIMARY
    c.drawText("System", left, contentTop - 12f, text)

    val gap = 24f
    val cardW = (right - left - gap) / 2f
    val cardH = 128f
    val currentDvd = prefs.getString("dvdPath", null)
    val playing = if (!emulatorRunning) {
      "Nothing yet"
    } else {
      currentDvd?.let { p ->
        games.find { it.path == p }?.displayTitle
          ?: File(p).name.substringBeforeLast('.').replace('_', ' ')
      } ?: "Xbox Dashboard"
    }
    val pace = if (guestFrameMs > 0.5f) {
      "%.1f ms · %.0f fps".format(guestFrameMs, 1000f / guestFrameMs)
    } else {
      "—"
    }
    val battery = if (batteryPct >= 0) {
      "$batteryPct%" + if (batteryCharging) " · charging" else ""
    } else "—"

    fun card(ix: Int, iy: Int, label: String, value: String, valueColor: Int) {
      val x = left + ix * (cardW + gap)
      val y = contentTop + 16f + iy * (cardH + gap)
      val r = RectF(x, y, x + cardW, y + cardH)
      fill.color = Th.CARD_BG
      c.drawRoundRect(r, 20f, 20f, fill)
      stroke.color = Th.CARD_STROKE
      stroke.strokeWidth = 2f
      c.drawRoundRect(r, 20f, 20f, stroke)
      text.typeface = tfRegular
      text.textSize = 21f
      text.color = Th.TEXT_TERTIARY
      c.drawText(label, x + 26f, y + 42f, text)
      text.typeface = tfMedium
      text.textSize = 36f
      text.color = valueColor
      c.drawText(ellipsize(value, text, cardW - 52f), x + 26f, y + 94f, text)
    }
    val scale = prefs.getInt("setting_surface_scale", 1)
    val renderer = "Vulkan · Turnip" +
      if (scale > 1) " · ${scale}× SSAA" else " · Native"
    card(0, 0, "NOW PLAYING", playing, Th.TEXT_PRIMARY)
    card(1, 0, "GUEST PACE", pace,
      if (guestFrameMs > 0.5f) Th.ACCENT else Th.TEXT_SECONDARY)
    card(0, 1, "HEADSET BATTERY", battery, Th.TEXT_PRIMARY)
    card(1, 1, "RENDERER", renderer, Th.TEXT_PRIMARY)
    var cardRows = 2
    if (micState >= 0) {
      card(0, 2, "MICROPHONE",
        if (micState == 1) "Muted" else "Live",
        if (micState == 1) Th.AMBER else Th.ACCENT)
      card(1, 2, "MIC CONTROL", "A on the right Touch controller",
        Th.TEXT_SECONDARY)
      cardRows = 3
    }

    // Actions.
    val actionsTop = contentTop + 16f + cardRows * (cardH + gap) + 18f
    text.typeface = tfMedium
    text.textSize = 21f
    text.color = Th.TEXT_TERTIARY
    c.drawText("ACTIONS", left + 6f, actionsTop, text)

    var x = left
    fun action(id: String, label: String, danger: Boolean) {
      text.typeface = tfMedium
      text.textSize = 26f
      val bw = text.measureText(label) + 72f
      val r = RectF(x, actionsTop + 18f, x + bw, actionsTop + 18f + 64f)
      focusables.add(Focusable(id, r))
      fill.color = when {
        pressedId == id -> Th.PRESS_FILL
        danger -> Th.DANGER_DIM
        hoverId == id || focusId == id -> Th.HOVER_FILL
        else -> Th.CARD_BG
      }
      c.drawRoundRect(r, 32f, 32f, fill)
      if (focusId == id || hoverId == id) {
        stroke.color = if (danger) Th.DANGER else Th.ACCENT
        stroke.strokeWidth = 2.5f
        c.drawRoundRect(r, 32f, 32f, stroke)
      }
      text.color = if (danger) Th.DANGER else Th.TEXT_PRIMARY
      text.textAlign = Paint.Align.CENTER
      c.drawText(label, r.centerX(), r.centerY() + 9f, text)
      text.textAlign = Paint.Align.LEFT
      x += bw + 20f
    }
    action("sys:recenter", "Recenter Screen", false)
    action("sys:close", "Close Menu", false)
    if (micState >= 0) {
      action("sys:mic", if (micState == 1) "Unmute Mic" else "Mute Mic", false)
    }
    if (emulatorRunning) {
      action("sys:quit", "Quit to Library", true)
    }

    // About footer.
    text.typeface = tfRegular
    text.textSize = 21f
    text.color = Th.TEXT_TERTIARY
    c.drawText("XEMU Quest Edition · original Xbox emulation on Quest 3",
      left, contentBottom - 8f, text)
  }

  // --- shared widgets ---

  private fun drawScrollbar(c: Canvas, right: Float, offset: Float, maxOffset: Float) {
    if (maxOffset <= 0f) return
    val trackTop = contentTop + 8f
    val trackBottom = contentBottom - 8f
    val trackH = trackBottom - trackTop
    val visible = trackH / (trackH + maxOffset)
    val thumbH = max(48f, trackH * visible)
    val thumbTop = trackTop + (trackH - thumbH) * (offset / maxOffset)
    fill.color = 0x14FFFFFF
    c.drawRoundRect(RectF(right + 18f, trackTop, right + 24f, trackBottom), 3f, 3f, fill)
    fill.color = 0x46FFFFFF
    c.drawRoundRect(RectF(right + 18f, thumbTop, right + 24f, thumbTop + thumbH), 3f, 3f, fill)
  }

  private fun isFpJit(game: GameScanner.Game): Boolean {
    return PerGameSettingsManager.loadOverrides(appContext, game.relativePath)["setting_hard_fpu"] == "true"
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
