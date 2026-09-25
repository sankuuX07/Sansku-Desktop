package com.sanskystream.android

// ---------------------------------------------------------------------------
// TcpControlChannel.kt — M18: TCP connection to Windows SanskyStream (port 5000)
//
// The Windows side is the TCP SERVER. Android is the TCP CLIENT.
// This is the same role as the iPhone — connect outward to port 5000.
//
// Responsibilities:
//   - Connect to the Windows PC IP on port 5000 (CONTROL_TCP_PORT).
//   - Expose a method to send a raw byte frame (PacketHeader + payload).
//   - Handle connection errors and disconnects gracefully.
//   - NOT responsible for packet framing — callers pass pre-framed bytes.
//
// Thread-safety: NOT thread-safe. Call from a single worker thread.
// ---------------------------------------------------------------------------

import android.util.Log
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder

private const val TAG = "SanskyTcpChannel"
private const val CONNECT_TIMEOUT_MS = 5000

class TcpControlChannel {

    private var socket: Socket? = null
    private var outputStream: OutputStream? = null

    var isConnected: Boolean = false
        private set

    /** Connect to the Windows receiver. Returns true on success. */
    fun connect(host: String, port: Int = Protocol.CONTROL_TCP_PORT): Boolean {
        return try {
            val s = Socket()
            s.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
            s.tcpNoDelay = true  // Low-latency: disable Nagle's algorithm
            socket       = s
            outputStream = s.getOutputStream()
            isConnected  = true
            Log.i(TAG, "TCP connected to $host:$port")
            true
        } catch (e: Exception) {
            Log.e(TAG, "TCP connect failed: ${e.message}")
            isConnected = false
            false
        }
    }

    /**
     * Send a raw framed packet (PacketHeader already prepended by caller).
     * Returns false if the socket is disconnected or write fails.
     */
    fun send(data: ByteArray): Boolean {
        val out = outputStream ?: return false
        return try {
            out.write(data)
            out.flush()
            true
        } catch (e: Exception) {
            Log.e(TAG, "TCP send failed: ${e.message}")
            isConnected = false
            false
        }
    }

    /** Close the TCP connection. */
    fun close() {
        try {
            socket?.close()
        } catch (_: Exception) {}
        socket       = null
        outputStream = null
        isConnected  = false
        Log.i(TAG, "TCP channel closed.")
    }

    companion object {
        /**
         * Build a SanskyStream PacketHeader + payload byte array.
         * Matches the C++ PacketHeader struct (9 bytes, little-endian).
         *
         * Layout: magic(4) + type(1) + payloadSize(4) + payload
         */
        fun buildPacket(type: UByte, payload: ByteArray): ByteArray {
            val buf = ByteBuffer.allocate(Protocol.PACKET_HEADER_SIZE + payload.size)
                .order(ByteOrder.LITTLE_ENDIAN)
            buf.putInt(Protocol.CONTROL_MAGIC.toInt())   // 4 bytes
            buf.put(type.toByte())                        // 1 byte
            buf.putInt(payload.size)                      // 4 bytes
            buf.put(payload)
            return buf.array()
        }
    }
}
