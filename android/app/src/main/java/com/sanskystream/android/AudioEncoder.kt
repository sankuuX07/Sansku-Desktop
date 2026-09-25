package com.sanskystream.android

// ---------------------------------------------------------------------------
// AudioEncoder.kt — M18: Android AAC-LC Encoder
//
// Phase 7: Encodes raw PCM16 stereo frames to AAC-LC access units.
//
// Output: raw AAC-LC access units WITHOUT ADTS header.
// This matches the iOS AudioToolbox output expected by the Windows AACDecoder.
// The Windows AACDecoder already handles both 44100 Hz and 48000 Hz.
//
// Thread-safety: NOT thread-safe. Call from a single worker thread.
// ---------------------------------------------------------------------------

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer

private const val TAG = "SanskyAudioEncoder"
private const val MIME_TYPE = "audio/mp4a-latm"

/** Callback: (rawAacBytes, timestampUs) */
typealias AacEncodedCallback = (aacData: ByteArray, timestampUs: Long) -> Unit

class AudioEncoder(
    private val sampleRate: Int = 44100,
    private val channelCount: Int = 2,
    private val bitrateBps: Int = 128_000  // 128 kbps
) {
    var onEncoded: AacEncodedCallback? = null

    private var codec: MediaCodec? = null
    private var inputIndex = -1

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    fun start(): Boolean {
        return try {
            val format = MediaFormat.createAudioFormat(MIME_TYPE, sampleRate, channelCount).apply {
                setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
                setInteger(MediaFormat.KEY_AAC_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                // Maximum encoder latency — 0 = lowest latency (no look-ahead).
                setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16 * 1024)
            }

            val encoder = MediaCodec.createEncoderByType(MIME_TYPE)
            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            encoder.start()
            codec = encoder
            Log.i(TAG, "AudioEncoder started: ${sampleRate}Hz ${channelCount}ch ${bitrateBps/1000}kbps AAC-LC")
            true
        } catch (e: Exception) {
            Log.e(TAG, "AudioEncoder start failed: ${e.message}", e)
            false
        }
    }

    fun stop() {
        try {
            codec?.stop()
            codec?.release()
            codec = null
        } catch (e: Exception) {
            Log.e(TAG, "AudioEncoder stop error: ${e.message}")
        }
        Log.i(TAG, "AudioEncoder stopped.")
    }

    /**
     * Encode a PCM16 audio buffer.
     * Blocks until the encoder accepts the input and produces output.
     * Timeout is 50 ms per operation.
     */
    fun encode(pcmData: ByteArray, timestampUs: Long) {
        val encoder = codec ?: return

        // Queue input.
        val inIdx = encoder.dequeueInputBuffer(50_000L)
        if (inIdx >= 0) {
            val inBuf = encoder.getInputBuffer(inIdx) ?: return
            inBuf.clear()
            inBuf.put(pcmData)
            encoder.queueInputBuffer(inIdx, 0, pcmData.size, timestampUs, 0)
        }

        // Drain output.
        drainOutput(encoder)
    }

    fun flush() {
        val encoder = codec ?: return
        // Signal end of stream.
        val inIdx = encoder.dequeueInputBuffer(50_000L)
        if (inIdx >= 0) {
            encoder.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
        }
        drainOutput(encoder)
    }

    private fun drainOutput(encoder: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (true) {
            val outIdx = encoder.dequeueOutputBuffer(info, 0L)
            when {
                outIdx == MediaCodec.INFO_TRY_AGAIN_LATER -> break
                outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    Log.i(TAG, "AAC output format: ${encoder.outputFormat}")
                }
                outIdx >= 0 -> {
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                        // Codec-config (AudioSpecificConfig) — skip, not transmitted.
                        encoder.releaseOutputBuffer(outIdx, false)
                        continue
                    }
                    if (info.size > 0) {
                        val outBuf = encoder.getOutputBuffer(outIdx)
                        if (outBuf != null) {
                            outBuf.position(info.offset)
                            outBuf.limit(info.offset + info.size)
                            val aacData = ByteArray(info.size)
                            outBuf.get(aacData)
                            // Raw AAC access unit, no ADTS — matches iOS output.
                            onEncoded?.invoke(aacData, info.presentationTimeUs)
                        }
                    }
                    encoder.releaseOutputBuffer(outIdx, false)
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                }
                else -> break
            }
        }
    }
}
