package com.sanskystream.android

// ---------------------------------------------------------------------------
// VideoEncoder.kt — M18: MediaCodec H.264 Hardware Encoder
//
// Encodes frames from a VirtualDisplay Surface into H.264 Annex B.
// Uses MediaCodec in async mode with a Surface input — this is the
// standard Android hardware-encode path for screen capture.
//
// Design mirrors iOS VideoEncoder.swift:
//   - Hardware encoder preferred (video/avc)
//   - Baseline profile, no B-frames (low latency)
//   - SPS/PPS prepended to keyframes in Annex B format
//   - Configurable bitrate, FPS, and resolution
//
// Thread-safety:
//   MediaCodec async callbacks fire on an internal codec thread.
//   onEncodedFrame is invoked from that thread — the caller must
//   be thread-safe (VideoTransport.sendFrame is single-threaded but
//   called from MediaCodec's callback, which is serialised).
// ---------------------------------------------------------------------------

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

private const val TAG = "SanskyVideoEncoder"
private const val MIME_TYPE = "video/avc"

/** Callback invoked on every encoded Annex B frame. */
typealias EncodedFrameCallback = (naluData: ByteArray, presentationUs: Long, isKeyframe: Boolean) -> Unit

class VideoEncoder(
    private val width: Int,
    private val height: Int,
    private val bitrateBps: Int = 4_000_000,   // 4 Mbps default
    private val frameRate: Int  = 60
) {

    var onEncodedFrame: EncodedFrameCallback? = null

    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null

    /** SPS/PPS bytes extracted from the codec-config output buffer. */
    private var spsData: ByteArray? = null
    private var ppsData: ByteArray? = null

    // Per-frame state: presentation timestamp.
    private var lastPresentationUs: Long = 0

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * Create and configure the MediaCodec encoder.
     * @return The encoder input Surface to attach to a VirtualDisplay, or null on failure.
     */
    fun prepare(): Surface? {
        try {
            val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height).apply {
                setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
                setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)  // keyframe every 1 second
                setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)

                // Low-latency encoder settings (mirrors iOS M13 settings).
                // Baseline profile, no B-frames.
                setInteger(MediaFormat.KEY_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
                setInteger(MediaFormat.KEY_LEVEL,
                    MediaCodecInfo.CodecProfileLevel.AVCLevel31)

                // Prioritize low latency over quality (API 25+).
                setInteger(MediaFormat.KEY_PRIORITY, 0)  // 0=realtime

                // M13 equivalent: MaxFrameDelayCount=0
                // Android equivalent: latency=0 (API 30+).
                if (android.os.Build.VERSION.SDK_INT >= 30) {
                    setInteger(MediaFormat.KEY_LATENCY, 0)
                }
            }

            val encoder = MediaCodec.createEncoderByType(MIME_TYPE)
            encoder.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
                    // Surface input mode — input buffers are not used.
                }

                override fun onOutputBufferAvailable(
                    codec: MediaCodec,
                    index: Int,
                    info: MediaCodec.BufferInfo
                ) {
                    handleOutputBuffer(codec, index, info)
                }

                override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                    Log.e(TAG, "Encoder error: ${e.diagnosticInfo}", e)
                }

                override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                    Log.i(TAG, "Output format changed: $format")
                    // SPS/PPS may be embedded in the first BUFFER_FLAG_CODEC_CONFIG buffer.
                }
            })

            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            inputSurface = encoder.createInputSurface()
            encoder.start()
            codec = encoder

            Log.i(TAG, "VideoEncoder prepared: ${width}x${height} ${bitrateBps/1000}kbps ${frameRate}fps")
            return inputSurface

        } catch (e: Exception) {
            Log.e(TAG, "VideoEncoder prepare failed: ${e.message}", e)
            return null
        }
    }

    fun release() {
        try {
            codec?.stop()
            codec?.release()
            codec = null
            inputSurface?.release()
            inputSurface = null
            spsData = null
            ppsData  = null
            Log.i(TAG, "VideoEncoder released.")
        } catch (e: Exception) {
            Log.e(TAG, "VideoEncoder release error: ${e.message}")
        }
    }

    val surface: Surface? get() = inputSurface

    // -----------------------------------------------------------------------
    // Output buffer handling
    // -----------------------------------------------------------------------

    private val annexBStartCode = byteArrayOf(0x00, 0x00, 0x00, 0x01)

    private fun handleOutputBuffer(
        codec: MediaCodec,
        index: Int,
        info: MediaCodec.BufferInfo
    ) {
        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
            // Codec config: extract SPS and PPS for later prepending to keyframes.
            val buf = codec.getOutputBuffer(index) ?: run {
                codec.releaseOutputBuffer(index, false)
                return
            }
            extractSpsAndPps(buf, info.offset, info.size)
            codec.releaseOutputBuffer(index, false)
            return
        }

        if (info.size <= 0) {
            codec.releaseOutputBuffer(index, false)
            return
        }

        val isKeyframe = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
        val pts        = info.presentationTimeUs
        lastPresentationUs = pts

        val outputBuf = codec.getOutputBuffer(index) ?: run {
            codec.releaseOutputBuffer(index, false)
            return
        }

        outputBuf.position(info.offset)
        outputBuf.limit(info.offset + info.size)

        // Read the raw encoded bytes (AVCC format from MediaCodec).
        val encoded = ByteArray(info.size)
        outputBuf.get(encoded)
        codec.releaseOutputBuffer(index, false)

        // Convert from AVCC to Annex B and prepend SPS/PPS on keyframes.
        val annexB = buildAnnexBFrame(encoded, isKeyframe)
        if (annexB != null) {
            onEncodedFrame?.invoke(annexB, pts, isKeyframe)
        }
    }

    /**
     * Extract SPS and PPS NALUs from a codec-config buffer.
     * MediaCodec delivers them concatenated in AVCC format.
     */
    private fun extractSpsAndPps(buf: ByteBuffer, offset: Int, size: Int) {
        val data = ByteArray(size)
        buf.position(offset)
        buf.get(data)

        // Walk AVCC: 4-byte BE length prefix before each NALU.
        var pos = 0
        val results = mutableListOf<ByteArray>()
        while (pos + 4 <= size) {
            val naluLen = ((data[pos].toInt() and 0xFF) shl 24) or
                          ((data[pos+1].toInt() and 0xFF) shl 16) or
                          ((data[pos+2].toInt() and 0xFF) shl 8)  or
                          (data[pos+3].toInt() and 0xFF)
            pos += 4
            if (naluLen <= 0 || pos + naluLen > size) break
            results.add(data.copyOfRange(pos, pos + naluLen))
            pos += naluLen
        }

        // SPS is NALU type 7, PPS is type 8.
        for (nalu in results) {
            if (nalu.isEmpty()) continue
            when ((nalu[0].toInt() and 0x1F)) {
                7 -> { spsData = nalu; Log.d(TAG, "SPS extracted (${nalu.size} bytes)") }
                8 -> { ppsData = nalu; Log.d(TAG, "PPS extracted (${nalu.size} bytes)") }
            }
        }
    }

    /**
     * Convert a MediaCodec AVCC output buffer to Annex B format.
     * For keyframes, SPS and PPS are prepended.
     */
    private fun buildAnnexBFrame(avcc: ByteArray, isKeyframe: Boolean): ByteArray? {
        val out = mutableListOf<Byte>()

        // Prepend SPS + PPS on keyframes.
        if (isKeyframe) {
            val sps = spsData
            val pps = ppsData
            if (sps != null) { out.addAll(annexBStartCode.toList()); out.addAll(sps.toList()) }
            if (pps != null) { out.addAll(annexBStartCode.toList()); out.addAll(pps.toList()) }
        }

        // Convert each AVCC NALU to Annex B.
        var pos = 0
        while (pos + 4 <= avcc.size) {
            val naluLen = ((avcc[pos].toInt() and 0xFF) shl 24) or
                          ((avcc[pos+1].toInt() and 0xFF) shl 16) or
                          ((avcc[pos+2].toInt() and 0xFF) shl 8)  or
                          (avcc[pos+3].toInt() and 0xFF)
            pos += 4
            if (naluLen <= 0 || pos + naluLen > avcc.size) break
            out.addAll(annexBStartCode.toList())
            for (i in pos until pos + naluLen) out.add(avcc[i])
            pos += naluLen
        }

        return if (out.isEmpty()) null else out.toByteArray()
    }
}
