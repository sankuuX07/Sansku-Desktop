package com.sanskystream.android

// ---------------------------------------------------------------------------
// StreamingService.kt — M18: Foreground Service orchestrating all streaming
//
// A foreground service is required on Android 8.0+ (API 26+) to perform
// screen capture in the background. Without this, the OS would terminate
// the capture when the user leaves the app.
//
// Responsibilities:
//   - Own the MediaProjection lifecycle (received from MainActivity via Intent)
//   - Create VideoEncoder → get input Surface
//   - Create ScreenCapture (VirtualDisplay) → feed Surface to encoder
//   - Run VideoTransport on a dedicated network thread
//   - Run AudioCapture (API 29+) and AudioEncoder on an audio thread
//   - Run AudioTransport on the same TCP channel as the control connection
//   - Advertise via ServiceAdvertiser (NSD/mDNS)
//   - Handle orientation changes by reconfiguring the encoder and VirtualDisplay
//   - Clean up all resources on stop
//
// Lifecycle: START_NOT_STICKY — does not restart after process death.
// ---------------------------------------------------------------------------

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

private const val TAG                  = "SanskyStreamingService"
private const val NOTIF_CHANNEL_ID    = "SanskyStream"
private const val NOTIF_ID             = 1001

const val EXTRA_RESULT_CODE    = "result_code"
const val EXTRA_DATA_INTENT    = "data_intent"
const val EXTRA_WINDOWS_HOST   = "windows_host"

class StreamingService : Service() {

    // -----------------------------------------------------------------------
    // Components
    // -----------------------------------------------------------------------
    private var mediaProjection:  MediaProjection?    = null
    private var screenCapture:    ScreenCapture?      = null
    private var videoEncoder:     VideoEncoder?       = null
    private var videoTransport:   VideoTransport?     = null
    private var audioCapture:     AudioCapture?       = null
    private var audioEncoder:     AudioEncoder?       = null
    private var audioTransport:   AudioTransport?     = null
    private var tcpChannel:       TcpControlChannel?  = null
    private var serviceAdvertiser: ServiceAdvertiser? = null

    /** Executor for network I/O (TCP connect, UDP send). */
    private var networkExecutor: ExecutorService? = null
    /** Executor for audio encode + send. */
    private var audioExecutor:   ExecutorService? = null

    @Volatile
    private var streaming = false

    // -----------------------------------------------------------------------
    // Service lifecycle
    // -----------------------------------------------------------------------

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        Log.i(TAG, "StreamingService created.")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent == null) { stopSelf(); return START_NOT_STICKY }

        val resultCode  = intent.getIntExtra(EXTRA_RESULT_CODE, -1)
        val dataIntent  = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(EXTRA_DATA_INTENT, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(EXTRA_DATA_INTENT)
        }
        val windowsHost = intent.getStringExtra(EXTRA_WINDOWS_HOST) ?: ""

        if (dataIntent == null || windowsHost.isEmpty()) {
            Log.e(TAG, "StreamingService: missing MediaProjection data or host. Stopping.")
            stopSelf()
            return START_NOT_STICKY
        }

        startForeground(NOTIF_ID, buildNotification("Connecting..."))
        startStreaming(resultCode, dataIntent, windowsHost)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopStreaming()
        super.onDestroy()
        Log.i(TAG, "StreamingService destroyed.")
    }

    // -----------------------------------------------------------------------
    // Core streaming logic
    // -----------------------------------------------------------------------

    private fun startStreaming(resultCode: Int, dataIntent: Intent, windowsHost: String) {
        networkExecutor = Executors.newSingleThreadExecutor()
        audioExecutor   = Executors.newSingleThreadExecutor()

        networkExecutor?.execute {
            try {
                doStartStreaming(resultCode, dataIntent, windowsHost)
            } catch (e: Exception) {
                Log.e(TAG, "Streaming startup error: ${e.message}", e)
                broadcastStatus("Error: ${e.message}")
                stopSelf()
            }
        }
    }

    /**
     * TCP reconnect monitor — runs on the network executor thread after streaming starts.
     *
     * The MediaProjection / VirtualDisplay / VideoEncoder are NOT touched during
     * reconnect: they keep running so the encoder state (SPS/PPS) remains valid.
     * Only the TCP socket and AudioTransport reference need re-establishment.
     *
     * Attempts up to RECONNECT_MAX_ATTEMPTS with RECONNECT_INTERVAL_MS between tries.
     * If all attempts fail, stops the service.
     */
    private fun runTcpReconnectMonitor(windowsHost: String) {
        var attempt = 0
        while (streaming && attempt < RECONNECT_MAX_ATTEMPTS) {
            val tcp = tcpChannel ?: break
            if (!tcp.isConnected) {
                attempt++
                broadcastStatus("Reconnecting ($attempt/$RECONNECT_MAX_ATTEMPTS)...")
                updateNotification("Reconnecting ($attempt/$RECONNECT_MAX_ATTEMPTS)")
                Log.i(TAG, "TCP dropped. Reconnect attempt $attempt/$RECONNECT_MAX_ATTEMPTS")

                val newTcp = TcpControlChannel()
                if (newTcp.connect(windowsHost)) {
                    // Swap in the new channel atomically.
                    tcpChannel   = newTcp
                    audioTransport = AudioTransport(newTcp)
                    tcp.close()  // close the old broken socket
                    attempt = 0  // reset counter on success
                    broadcastStatus("Reconnected — streaming to $windowsHost")
                    updateNotification("Streaming to $windowsHost")
                    Log.i(TAG, "TCP reconnected to $windowsHost")
                } else {
                    newTcp.close()
                    Thread.sleep(RECONNECT_INTERVAL_MS)
                }
            } else {
                // Connection healthy — poll every 500 ms.
                Thread.sleep(500)
            }
        }

        if (streaming && attempt >= RECONNECT_MAX_ATTEMPTS) {
            Log.e(TAG, "TCP reconnect exhausted after $RECONNECT_MAX_ATTEMPTS attempts. Stopping.")
            broadcastStatus("Stopped: lost connection to $windowsHost")
            stopSelf()
        }
    }

    private fun doStartStreaming(resultCode: Int, dataIntent: Intent, windowsHost: String) {
        broadcastStatus("Connecting to $windowsHost...")

        // 1. MediaProjection
        val pm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val projection = pm.getMediaProjection(resultCode, dataIntent)
        if (projection == null) {
            broadcastStatus("Error: MediaProjection unavailable")
            stopSelf(); return
        }
        mediaProjection = projection

        // 2. Display dimensions (dynamic — no hardcoded resolution)
        val (screenW, screenH, density) = ScreenCapture.readDisplayMetrics(this)
        Log.i(TAG, "Display: ${screenW}x${screenH} density=$density")

        // 3. TCP connection to Windows
        val tcp = TcpControlChannel()
        tcpChannel = tcp
        if (!tcp.connect(windowsHost)) {
            broadcastStatus("Error: Cannot connect to $windowsHost:${Protocol.CONTROL_TCP_PORT}")
            projection.stop()
            stopSelf(); return
        }

        // 4. Video encoder
        val encoder = VideoEncoder(width = screenW, height = screenH)
        videoEncoder = encoder
        val encoderSurface = encoder.prepare()
        if (encoderSurface == null) {
            broadcastStatus("Error: H.264 encoder unavailable")
            tcp.close(); projection.stop(); stopSelf(); return
        }

        // 5. Video transport (UDP)
        val vTransport = VideoTransport(windowsHost)
        videoTransport = vTransport
        encoder.onEncodedFrame = { naluData, presentationUs, isKeyframe ->
            vTransport.sendFrame(naluData, presentationUs, isKeyframe)
        }

        // 6. Screen capture (VirtualDisplay)
        val capture = ScreenCapture(this, projection, screenW, screenH, density, encoderSurface)
        capture.onProjectionStopped = {
            Log.w(TAG, "MediaProjection stopped externally — stopping streaming.")
            broadcastStatus("Stopped: screen capture revoked")
            stopSelf()
        }
        screenCapture = capture
        if (!capture.start()) {
            broadcastStatus("Error: VirtualDisplay creation failed")
            encoder.release(); vTransport.close()
            tcp.close(); projection.stop(); stopSelf(); return
        }

        // 7. Audio (API 29+ only)
        val aTransport = AudioTransport(tcp)
        audioTransport = aTransport

        val aCapture = AudioCapture(this, projection)
        audioCapture = aCapture
        if (aCapture.isAvailable) {
            val aEncoder = AudioEncoder(sampleRate = 44100, channelCount = 2)
            audioEncoder = aEncoder
            if (aEncoder.start()) {
                aEncoder.onEncoded = { aacData, timestampUs ->
                    aTransport.sendAudio(aacData, timestampUs)
                }
                aCapture.onAudioFrame = { pcmData, _, _, timestampUs ->
                    audioExecutor?.execute {
                        aEncoder.encode(pcmData, timestampUs)
                    }
                }
                if (!aCapture.start()) {
                    Log.w(TAG, "AudioCapture.start() failed — video-only mode.")
                    aEncoder.stop()
                    audioEncoder = null
                }
            } else {
                Log.w(TAG, "AudioEncoder.start() failed — video-only mode.")
                audioEncoder = null
            }
        } else {
            Log.w(TAG, "Internal audio not available on this device/API — video-only.")
        }

        // 8. mDNS advertisement
        val advertiser = ServiceAdvertiser(this)
        serviceAdvertiser = advertiser
        advertiser.start()

        streaming = true
        broadcastStatus("Streaming to $windowsHost")
        updateNotification("Streaming to $windowsHost")
        Log.i(TAG, "Streaming started.")

        // Block the network executor thread on the reconnect monitor.
        // This keeps the executor busy (so it doesn't terminate) and monitors
        // the TCP channel for drops, reconnecting transparently.
        runTcpReconnectMonitor(windowsHost)
    }

    private fun stopStreaming() {
        if (!streaming && mediaProjection == null) return
        streaming = false
        Log.i(TAG, "Stopping streaming...")

        serviceAdvertiser?.stop()
        audioCapture?.stop()
        audioEncoder?.stop()
        screenCapture?.stop()
        videoEncoder?.release()
        videoTransport?.close()
        tcpChannel?.close()

        audioExecutor?.shutdownNow()
        networkExecutor?.shutdownNow()

        serviceAdvertiser = null
        audioCapture      = null
        audioEncoder      = null
        audioTransport    = null
        screenCapture     = null
        videoEncoder      = null
        videoTransport    = null
        tcpChannel        = null
        mediaProjection   = null

        Log.i(TAG, "Streaming stopped. All resources released.")
    }

    // -----------------------------------------------------------------------
    // Notification helpers
    // -----------------------------------------------------------------------

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIF_CHANNEL_ID,
                "SanskyStream",
                NotificationManager.IMPORTANCE_LOW
            ).apply { description = "Screen streaming notification" }
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(text: String): Notification {
        return NotificationCompat.Builder(this, NOTIF_CHANNEL_ID)
            .setContentTitle("SanskyStream")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIF_ID, buildNotification(text))
    }

    private fun broadcastStatus(status: String) {
        val intent = Intent(ACTION_STATUS_UPDATE).putExtra(EXTRA_STATUS, status)
        sendBroadcast(intent)
    }

    companion object {
        const val ACTION_STATUS_UPDATE = "com.sanskystream.android.STATUS_UPDATE"
        const val EXTRA_STATUS         = "status"

        // TCP reconnect policy:
        //   3 s between attempts × 30 max = up to 90 s of reconnect attempts.
        //   After 90 s of broken TCP, the service stops itself.
        private const val RECONNECT_INTERVAL_MS:  Long = 3_000L
        private const val RECONNECT_MAX_ATTEMPTS: Int  = 30
    }
}
