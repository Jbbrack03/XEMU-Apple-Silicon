package com.izzy2lost.x1box

import android.app.NativeActivity
import android.content.Context
import android.content.Intent

/**
 * Builds the one supported 2D-to-XR handoff.
 *
 * NativeActivity must be launched as its own task.  Inheriting a standard 2D
 * frontend task makes Horizon perform a second volumetric transition while
 * SDL is bootstrapping, which can terminate the first xemu process.
 */
object XrNativeActivityIntent {
  fun create(context: Context): Intent =
    Intent(context, NativeActivity::class.java).addFlags(
      Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP,
    )
}
