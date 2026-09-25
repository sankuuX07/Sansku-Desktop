package com.sanskystream.android

// ---------------------------------------------------------------------------
// ScreenCapture.kt — M18: MediaProjection + VirtualDisplay lifecycle
//
// Owns the MediaProjection, VirtualDisplay, and encoder Surface lifecycle.
//
// Phase 2: MediaProjection screen capture implementation.
// Phase 3: Integrates VideoEncoder Surface input.
//
// Threading: All methods must be called from the StreamingService thread.
// ---------------------------------------------------------------------------

import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.util.DisplayMetrics
import android.util.Log
import android.view.Surface
import android.view.WindowManager

private const val TAG = "SanskyScreenCapture"

class ScreenCapture(
    private val context: Context,
    private val mediaProjection: MediaProjection,
    private val screenWidth: Int,
    private val screenHeight: Int,
    private val screenDensity: Int,
    private val encoderSurface: Surface
) {

    private var virtualDisplay: VirtualDisplay? = null

    /** Callback invoked if MediaProjection is stopped externally (e.g. user revokes). */
    var onProjectionStopped: (() -> Unit)? = null

    // MediaProjection callback to detect external revocation.
    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.w(TAG, "MediaProjection.Callback.onStop() — projection revoked externally.")
            onProjectionStopped?.invoke()
        }
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * Start screen capture.
     * Registers the MediaProjection callback and creates a VirtualDisplay
     * backed by the encoder's input Surface.
     *
     * @return true if capture started; false on error.
     */
    fun start(): Boolean {
        try {
            // Register stop callback before creating the VirtualDisplay.
            mediaProjection.registerCallback(projectionCallback, null)

            virtualDisplay = mediaProjection.createVirtualDisplay(
                "SanskyStream",
                screenWidth,
                screenHeight,
                screenDensity,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                encoderSurface,
                null,  // VirtualDisplay.Callback — optional
                null   // Handler — null = caller's thread
            )

            Log.i(TAG, "VirtualDisplay created: ${screenWidth}x${screenHeight} density=$screenDensity")
            return true

        } catch (e: Exception) {
            Log.e(TAG, "VirtualDisplay creation failed: ${e.message}", e)
            return false
        }
    }

    /**
     * Stop screen capture and release all resources.
     * After calling stop(), do not use this ScreenCapture instance again.
     */
    fun stop() {
        try {
            virtualDisplay?.release()
            virtualDisplay = null
            Log.i(TAG, "VirtualDisplay released.")
        } catch (e: Exception) {
            Log.e(TAG, "VirtualDisplay release error: ${e.message}")
        }

        try {
            mediaProjection.unregisterCallback(projectionCallback)
            mediaProjection.stop()
            Log.i(TAG, "MediaProjection stopped.")
        } catch (e: Exception) {
            Log.e(TAG, "MediaProjection stop error: ${e.message}")
        }
    }

    companion object {
        /**
         * Read current display dimensions and density.
         * Returns a Triple(width, height, densityDpi).
         * Does NOT hardcode any resolution — reads from the actual display.
         */
        fun readDisplayMetrics(context: Context): Triple<Int, Int, Int> {
            val wm = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
            return if (android.os.Build.VERSION.SDK_INT >= 30) {
                val bounds = wm.currentWindowMetrics.bounds
                val metrics = DisplayMetrics()
                @Suppress("DEPRECATION")
                wm.defaultDisplay.getMetrics(metrics)
                Triple(bounds.width(), bounds.height(), metrics.densityDpi)
            } else {
                val metrics = DisplayMetrics()
                @Suppress("DEPRECATION")
                wm.defaultDisplay.getRealMetrics(metrics)
                Triple(metrics.widthPixels, metrics.heightPixels, metrics.densityDpi)
            }
        }
    }
}
