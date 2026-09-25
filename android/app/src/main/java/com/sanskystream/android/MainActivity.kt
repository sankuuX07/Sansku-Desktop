package com.sanskystream.android

// ---------------------------------------------------------------------------
// MainActivity.kt — M18: SanskyStream Android UI
//
// Provides the permission flow and stream control UI.
//
// Responsibilities:
//   - Request RECORD_AUDIO permission (required for AudioPlaybackCapture)
//   - Launch MediaProjection permission dialog
//   - Start/stop StreamingService with projection token + Windows host
//   - Show connection status from service broadcasts
//   - Handle orientation changes gracefully (configChanges in manifest)
//
// UI elements:
//   - App title: SanskyStream
//   - Windows PC IP input
//   - Start Streaming button
//   - Stop Streaming button
//   - Status text (discovery status, streaming status, errors)
//   - Permission status messages
// ---------------------------------------------------------------------------

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

private const val TAG = "SanskyMainActivity"

class MainActivity : AppCompatActivity() {

    // -----------------------------------------------------------------------
    // Views
    // -----------------------------------------------------------------------
    private lateinit var tvStatus:     TextView
    private lateinit var tvPermissions: TextView
    private lateinit var etWindowsHost: EditText
    private lateinit var btnStart:     Button
    private lateinit var btnStop:      Button

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    private var isStreaming = false

    // -----------------------------------------------------------------------
    // Permissions
    // -----------------------------------------------------------------------

    private val requestAudioPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            updatePermissionStatus()
            if (granted) {
                Log.i(TAG, "RECORD_AUDIO permission granted.")
            } else {
                setStatus("Audio permission denied — video-only streaming will be used.")
            }
        }

    // -----------------------------------------------------------------------
    // MediaProjection permission
    // -----------------------------------------------------------------------

    private val requestMediaProjection =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK && result.data != null) {
                Log.i(TAG, "MediaProjection permission granted.")
                val host = etWindowsHost.text.toString().trim()
                if (host.isEmpty()) {
                    setStatus("Error: Enter Windows PC IP address first.")
                    return@registerForActivityResult
                }
                launchStreamingService(result.resultCode, result.data!!, host)
            } else {
                Log.w(TAG, "MediaProjection permission denied.")
                setStatus("Screen capture permission denied.\nStreaming cannot start without this permission.")
                updateButtons(streaming = false)
            }
        }

    // -----------------------------------------------------------------------
    // Status receiver from StreamingService
    // -----------------------------------------------------------------------

    private val statusReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val status = intent.getStringExtra(StreamingService.EXTRA_STATUS) ?: return
            setStatus(status)
            if (status.startsWith("Error") || status.startsWith("Stopped")) {
                isStreaming = false
                updateButtons(streaming = false)
            }
        }
    }

    // -----------------------------------------------------------------------
    // Activity lifecycle
    // -----------------------------------------------------------------------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus      = findViewById(R.id.tvStatus)
        tvPermissions = findViewById(R.id.tvPermissions)
        etWindowsHost = findViewById(R.id.etWindowsHost)
        btnStart      = findViewById(R.id.btnStartStreaming)
        btnStop       = findViewById(R.id.btnStopStreaming)

        btnStart.setOnClickListener { onStartClicked() }
        btnStop.setOnClickListener  { onStopClicked()  }

        updateButtons(streaming = false)
        updatePermissionStatus()
        setStatus("Ready. Enter Windows PC IP and tap Start.")
    }

    override fun onResume() {
        super.onResume()
        val filter = IntentFilter(StreamingService.ACTION_STATUS_UPDATE)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(statusReceiver, filter, RECEIVER_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(statusReceiver, filter)
        }
    }

    override fun onPause() {
        super.onPause()
        try { unregisterReceiver(statusReceiver) } catch (_: Exception) {}
    }

    // -----------------------------------------------------------------------
    // Button handlers
    // -----------------------------------------------------------------------

    private fun onStartClicked() {
        val host = etWindowsHost.text.toString().trim()
        if (host.isEmpty()) {
            Toast.makeText(this, "Enter Windows PC IP address", Toast.LENGTH_SHORT).show()
            return
        }

        // Request RECORD_AUDIO if not granted (needed for AudioPlaybackCapture).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
                setStatus("Requesting audio permission...")
                requestAudioPermission.launch(Manifest.permission.RECORD_AUDIO)
                // After permission result, user must tap Start again.
                // This is required by Android — we cannot chain permission + MediaProjection automatically.
                return
            }
        }

        // Launch MediaProjection permission dialog.
        // Android requires this on every app start — cannot be persisted.
        setStatus("Waiting for screen capture permission...")
        val pm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        requestMediaProjection.launch(pm.createScreenCaptureIntent())
    }

    private fun onStopClicked() {
        stopStreamingService()
        isStreaming = false
        updateButtons(streaming = false)
        setStatus("Streaming stopped.")
    }

    // -----------------------------------------------------------------------
    // Service management
    // -----------------------------------------------------------------------

    private fun launchStreamingService(resultCode: Int, dataIntent: Intent, host: String) {
        val intent = Intent(this, StreamingService::class.java).apply {
            putExtra(EXTRA_RESULT_CODE,  resultCode)
            putExtra(EXTRA_DATA_INTENT,  dataIntent)
            putExtra(EXTRA_WINDOWS_HOST, host)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
        isStreaming = true
        updateButtons(streaming = true)
        setStatus("Starting stream to $host...")
        Log.i(TAG, "StreamingService launched. Host: $host")
    }

    private fun stopStreamingService() {
        stopService(Intent(this, StreamingService::class.java))
    }

    // -----------------------------------------------------------------------
    // UI helpers
    // -----------------------------------------------------------------------

    private fun setStatus(text: String) {
        runOnUiThread { tvStatus.text = text }
    }

    private fun updateButtons(streaming: Boolean) {
        btnStart.isEnabled = !streaming
        btnStop.isEnabled  = streaming
    }

    private fun updatePermissionStatus() {
        val audioGranted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
                PackageManager.PERMISSION_GRANTED
        } else true

        val audioStatus = if (audioGranted) "RECORD_AUDIO: granted" else "RECORD_AUDIO: not granted (tap Start to request)"
        val apiStatus   = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            "Internal audio: supported (API ${Build.VERSION.SDK_INT})"
        } else {
            "Internal audio: unavailable (API ${Build.VERSION.SDK_INT} < 29)"
        }

        tvPermissions.text = "$audioStatus\n$apiStatus"
    }
}
