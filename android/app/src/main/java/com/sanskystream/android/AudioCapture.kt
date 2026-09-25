package com.sanskystream.android

// ---------------------------------------------------------------------------
// AudioCapture.kt — M18: Android Audio Capture
//
// Phase 6: Android audio capture implementation.
//
// Priority: Internal audio (game/app audio) via AudioPlaybackCapture (API 29+).
//
// Limitations:
//   - AudioPlaybackCapture requires API 29 (Android 10).
//   - Target apps must NOT have allowAudioPlaybackCapture=false in their manifest.
//   - Some OEM ROMs (MIUI, ColorOS) may block this regardless of API level.
//   - On API < 29, audio capture is unavailable; video-only mode is used.
//   - RECORD_AUDIO permission is required even for AudioPlaybackCapture.
//
// The Windows AACDecoder already accepts arbitrary sample rates and channel
// counts — no Windows changes needed for 44100 Hz or 48000 Hz.
// ---------------------------------------------------------------------------

import android.annotation.TargetApi
import android.content.Context
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.os.Build
import android.util.Log

private const val TAG = "SanskyAudioCapture"

// Sample rate for internal audio capture — 44100 Hz matches Protocol default.
// Use 44100 rather than 48000: more universally supported and matches the
// AAC-LC default in Protocol.h / AudioReceiver defaults on Windows.
private const val SAMPLE_RATE    = 44100
private const val CHANNEL_CONFIG = AudioFormat.CHANNEL_IN_STEREO
private const val AUDIO_FORMAT   = AudioFormat.ENCODING_PCM_16BIT
private const val CHANNEL_COUNT  = 2

/** Callback invoked with raw PCM16 stereo data + timestamp in microseconds. */
typealias AudioFrameCallback = (pcmData: ByteArray, sampleRate: Int, channelCount: Int, timestampUs: Long) -> Unit

class AudioCapture(
    private val context: Context,
    private val mediaProjection: MediaProjection?
) {

    var onAudioFrame: AudioFrameCallback? = null

    private var audioRecord: AudioRecord? = null
    private var captureThread: Thread? = null

    @Volatile
    private var running = false

    val isAvailable: Boolean
        get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && mediaProjection != null

    /**
     * Start audio capture.
     * On API < 29 or if MediaProjection is null, does nothing and returns false.
     * Returns true if capture started successfully.
     */
    fun start(): Boolean {
        if (!isAvailable) {
            Log.w(TAG, "AudioPlaybackCapture requires API 29+ and a MediaProjection. " +
                "Audio capture disabled. Video-only mode.")
            return false
        }

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startApi29()
        } else {
            false
        }
    }

    @TargetApi(Build.VERSION_CODES.Q)
    private fun startApi29(): Boolean {
        try {
            val projection = mediaProjection ?: return false

            val config = AudioPlaybackCaptureConfiguration.Builder(projection)
                .addMatchingUsage(android.media.AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(android.media.AudioAttributes.USAGE_GAME)
                .addMatchingUsage(android.media.AudioAttributes.USAGE_UNKNOWN)
                .build()

            val minBufSize = AudioRecord.getMinBufferSize(
                SAMPLE_RATE, CHANNEL_CONFIG, AUDIO_FORMAT)
            val bufSize = maxOf(minBufSize, SAMPLE_RATE * CHANNEL_COUNT * 2) // 1 second max

            val recorder = AudioRecord.Builder()
                .setAudioPlaybackCaptureConfig(config)
                .setAudioFormat(AudioFormat.Builder()
                    .setEncoding(AUDIO_FORMAT)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(CHANNEL_CONFIG)
                    .build())
                .setBufferSizeInBytes(bufSize)
                .build()

            if (recorder.state != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord failed to initialize — internal audio capture unavailable.")
                recorder.release()
                return false
            }

            audioRecord = recorder
            running     = true
            recorder.startRecording()

            captureThread = Thread({ captureLoop() }, "SanskyAudioCapture").also { it.start() }
            Log.i(TAG, "AudioPlaybackCapture started. ${SAMPLE_RATE}Hz stereo PCM16.")
            return true

        } catch (e: SecurityException) {
            Log.e(TAG, "RECORD_AUDIO permission denied: ${e.message}")
            return false
        } catch (e: Exception) {
            Log.e(TAG, "AudioCapture start failed: ${e.message}", e)
            return false
        }
    }

    fun stop() {
        running = false
        captureThread?.interrupt()
        captureThread?.join(2000)
        captureThread = null

        try {
            audioRecord?.stop()
            audioRecord?.release()
        } catch (_: Exception) {}
        audioRecord = null
        Log.i(TAG, "AudioCapture stopped.")
    }

    private fun captureLoop() {
        // 20 ms buffer at 44100 Hz stereo 16-bit = 44100*0.020*2*2 = 3528 bytes
        val readSize = (SAMPLE_RATE * CHANNEL_COUNT * 2 * 20) / 1000
        val pcmBuf   = ByteArray(readSize)
        var startTimeUs = System.nanoTime() / 1000L

        while (running) {
            val read = audioRecord?.read(pcmBuf, 0, readSize) ?: -1
            if (read < 0) {
                Log.e(TAG, "AudioRecord.read() error: $read")
                break
            }
            if (read > 0) {
                // Monotonic timestamp derived from wall clock.
                val nowUs = System.nanoTime() / 1000L
                val data  = pcmBuf.copyOf(read)
                onAudioFrame?.invoke(data, SAMPLE_RATE, CHANNEL_COUNT, nowUs)
            }
        }
        Log.i(TAG, "AudioCapture loop exited.")
    }
}
