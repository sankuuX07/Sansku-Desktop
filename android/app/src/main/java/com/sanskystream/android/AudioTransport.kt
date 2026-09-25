package com.sanskystream.android

// ---------------------------------------------------------------------------
// AudioTransport.kt — M18: Send encoded AAC audio over TCP to Windows
//
// Phase 7: Audio transport implementation.
//
// Sends AAC-LC access units wrapped in the SanskyStream protocol:
//   PacketHeader { magic, PacketType::Audio, payloadSize }
//   AudioPayloadHeader { timestampUs (uint64, little-endian) }
//   raw AAC-LC access unit bytes (no ADTS)
//
// This is the EXACT same format as the iOS audio transport.
// The Windows AudioReceiver::OnAudioPacketReceived() handles it without changes.
//
// Thread-safety: NOT thread-safe. Call from a single worker thread.
// ---------------------------------------------------------------------------

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

private const val TAG = "SanskyAudioTransport"

class AudioTransport(private val tcpChannel: TcpControlChannel) {

    private var packetsDropped: Long = 0
    private var packetsSent:    Long = 0

    /**
     * Send one raw AAC-LC access unit over the existing TCP control channel.
     *
     * @param aacData      Raw AAC-LC bytes (no ADTS header).
     * @param timestampUs  Presentation timestamp in microseconds.
     */
    fun sendAudio(aacData: ByteArray, timestampUs: Long) {
        if (!tcpChannel.isConnected) {
            packetsDropped++
            return
        }

        // Build AudioPayloadHeader: timestampUs (8 bytes LE).
        val payload = ByteBuffer.allocate(Protocol.AUDIO_PAYLOAD_HEADER_SIZE + aacData.size)
            .order(ByteOrder.LITTLE_ENDIAN)
        payload.putLong(timestampUs)
        payload.put(aacData)

        val packet = TcpControlChannel.buildPacket(Protocol.PACKET_TYPE_AUDIO, payload.array())
        val ok = tcpChannel.send(packet)
        if (ok) {
            packetsSent++
        } else {
            packetsDropped++
            Log.w(TAG, "Audio packet dropped (TCP disconnected).")
        }
    }

    fun getPacketsSent()   = packetsSent
    fun getPacketsDropped() = packetsDropped
}
