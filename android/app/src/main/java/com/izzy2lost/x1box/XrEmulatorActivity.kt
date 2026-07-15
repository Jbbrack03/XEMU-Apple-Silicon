package com.izzy2lost.x1box

import android.os.Bundle

/**
 * SDL/xemu bootstrap used only by the NativeActivity-first XR path.
 *
 * Unlike the legacy [MainActivity] declaration, this activity runs in the
 * package's main process so Horizon's immersive cpuset applies to every xemu
 * worker. NativeActivity is immediately reordered back to the foreground;
 * the SDL activity stays backgrounded solely to provide SDL's Android/JNI
 * runtime and the existing emulator bootstrap.
 */
class XrEmulatorActivity : MainActivity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    // Starting this separate task is asynchronous. Hand focus back only after
    // this activity has actually been created; doing it from the caller races
    // ahead of ActivityTaskManager and leaves the 2D SDL task on top.
    window.decorView.postDelayed({
      startActivity(XrNativeActivityIntent.create(this))
    }, 250L)
  }
}
