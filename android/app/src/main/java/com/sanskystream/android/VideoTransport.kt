package com.sanskystream.android

// ---------------------------------------------------------------------------
// VideoTransport.kt — M18: Video UDP transport
//
// Sends encoded H.264 video fragments to the Windows SanskyStream receiver
// over UDP (port 5001).
//
// EXACT mirror of the iOS VideoTransport.swift, adapted to Kotlin.
// Uses identical Protocol.h wire format — the Windows VideoUdpReceiver
// receives packets from both iPhone and Android without modification.
//
// Thread-safety: NOT thread-safe. Call only from the encoding callback
// thread (MediaCodec output buffer callback thread).
// ---------------------------------------------------------------------------

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

private const val TAG = "SanskyVideoTransport"

class VideoTransport(private val windowsHost: String) {

    private var socket: DatagramSocket? = null
    private var destAddress: InetAddress? = null

    /** Global packet sequence number, incremented for every datagram sent. */
    private var packetSeq: UInt = 0u

    /** Frame counter, incremented for every call to sendFrame(). */
    private var frameId: UInt = 0u

    /** Reusable send buffer — avoids per-fragment allocation. */
    private val datagramBuf = ByteArray(Protocol.VIDEO_HEADER_SIZE + Protocol.VIDEO_MAX_PAYLOAD)

    init {
        try {
            destAddress = InetAddress.getByName(windowsHost)
            socket = DatagramSocket()
            Log.i(TAG, "VideoTransport initialized. Target: $windowsHost:${Protocol.VIDEO_UDP_PORT}")
        } catch (e: Exception) {
            Log.e(TAG, "VideoTransport init failed: ${e.message}")
        }
    }

    /**
     * Send one complete encoded H.264 frame.
     * The frame is split into fragments if it exceeds VIDEO_MAX_PAYLOAD.
     *
     * @param naluData       Raw H.264 bytes in Annex B format (start codes present).
     *                       For keyframes, SPS/PPS precede the IDR NALU.
     * @param presentationUs Presentation timestamp in microseconds.
     * @param isKeyframe     True if this is an IDR / keyframe.
     */
    fun sendFrame(naluData: ByteArray, presentationUs: Long, isKeyframe: Boolean) {
        val sock = socket ?: return
        val dest = destAddress ?: return
        if (naluData.isEmpty()) return

        val totalBytes     = naluData.size
        val maxPayload     = Protocol.VIDEO_MAX_PAYLOAD
        val fragmentCount  = (totalBytes + maxPayload - 1) / maxPayload
        val currentFrameId = frameId
        frameId = (frameId + 1u) and 0xFFFFFFFFu

        for (fi in 0 until fragmentCount) {
            val payloadStart = fi * maxPayload
            val payloadEnd   = minOf(payloadStart + maxPayload, totalBytes)
            val payloadSize  = payloadEnd - payloadStart
            val datagramSize = Protocol.VIDEO_HEADER_SIZE + payloadSize
            val currentSeq   = packetSeq
            packetSeq        = (packetSeq + 1u) and 0xFFFFFFFFu

            // Serialize header fields at fixed byte offsets (little-endian).
            // Matches VideoOffset:: constants in Protocol.h exactly.
            val buf = ByteBuffer.wrap(datagramBuf, 0, datagramSize)
                .order(ByteOrder.LITTLE_ENDIAN)

            buf.putInt(Protocol.VIDEO_MAGIC.toInt())                   // offset 0
            buf.put(Protocol.VIDEO_PROTOCOL_VERSION.toByte())           // offset 4
            buf.put(Protocol.VIDEO_FRAGMENT_TYPE.toByte())              // offset 5
            val flags: Byte = if (isKeyframe) Protocol.VIDEO_FLAG_KEYFRAME.toByte() else 0
            buf.put(flags)                                               // offset 6
            buf.putInt(currentFrameId.toInt())                          // offset 7
            buf.putLong(presentationUs)                                  // offset 11
            buf.putInt(currentSeq.toInt())                              // offset 19
            buf.putShort(fi.toShort())                                  // offset 23
            buf.putShort(fragmentCount.toShort())                       // offset 25
            buf.putInt(payloadSize)                                      // offset 27
            // offset 31: payload
            System.arraycopy(naluData, payloadStart, datagramBuf,
                Protocol.VIDEO_HEADER_SIZE, payloadSize)

            try {
                val packet = DatagramPacket(datagramBuf, datagramSize,
                    dest, Protocol.VIDEO_UDP_PORT)
                sock.send(packet)
            } catch (e: Exception) {
                Log.e(TAG, "sendto failed: ${e.message}")
            }
        }
    }

    fun close() {
        socket?.close()
        socket = null
        Log.i(TAG, "VideoTransport closed.")
    }
}
