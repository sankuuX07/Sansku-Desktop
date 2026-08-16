import Foundation

// ---------------------------------------------------------------------------
// VideoTransport
//
// Sends encoded H.264 video fragments to the Windows SanskyStream receiver
// over UDP (port 5001).
//
// Responsibilities:
//   - Open a POSIX UDP socket to the Windows host.
//   - Fragment large H.264 NALU payloads into ≤ VIDEO_MAX_PAYLOAD-byte chunks.
//   - Serialize each fragment with the M6 VideoFragmentHeader.
//   - Maintain a monotonically incrementing global packetSeq.
//   - Maintain a monotonically incrementing frameId.
//
// This class is NOT thread-safe. It must only be called from the
// BroadcastExtension sample-handler queue.
//
// Protocol Wire Format (little-endian, 31-byte fixed header):
//
//   Offset  Size  Field
//   ------  ----  -----------------
//     0      4    magic            (0x564D5653 = 'SSMV' LE)
//     4      1    version          (1)
//     5      1    packetType       (0x01 = VideoFragment)
//     6      1    flags            (bit 0 = isKeyframe)
//     7      4    frameId
//    11      8    presentationUs
//    19      4    packetSeq
//    23      2    fragmentIndex
//    25      2    fragmentCount
//    27      4    payloadSize
//    31     --    payload          (raw H.264 Annex B bytes)
//
// These offsets MUST match VideoOffset:: constants in Protocol.h exactly.
// ---------------------------------------------------------------------------

final class VideoTransport {

    // -----------------------------------------------------------------------
    // Protocol constants — must match Protocol.h exactly
    // -----------------------------------------------------------------------

    /// Magic identifier for video UDP datagrams ('SSMV' in little-endian).
    private static let VIDEO_MAGIC: UInt32            = 0x564D5653
    private static let VIDEO_PROTOCOL_VERSION: UInt8  = 1
    private static let VIDEO_FRAGMENT_TYPE: UInt8     = 0x01
    private static let VIDEO_FLAG_KEYFRAME: UInt8     = 0x01
    private static let VIDEO_HEADER_SIZE: Int          = 31

    /// Maximum H.264 payload bytes per UDP datagram.
    /// Matches VIDEO_MAX_PAYLOAD in Protocol.h (1300 bytes).
    private static let VIDEO_MAX_PAYLOAD: Int          = 1300

    /// UDP port on the Windows receiver.
    private static let VIDEO_UDP_PORT: UInt16          = 5001

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    private var socketFd: Int32 = -1
    private var destAddr        = sockaddr_in()

    /// Global packet sequence number, incremented for every datagram sent.
    private var packetSeq: UInt32 = 0

    /// Frame counter, incremented for every call to sendFrame().
    private var frameId: UInt32 = 0

    // -----------------------------------------------------------------------
    // Initialisation
    // -----------------------------------------------------------------------

    /// - Parameter windowsHost: IPv4 address of the Windows SanskyStream receiver.
    init?(windowsHost: String) {
        socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socketFd >= 0 else {
            print("SanskyStream VideoTransport: socket() failed: \(errno)")
            return nil
        }

        // Build the destination sockaddr_in.
        destAddr.sin_family = sa_family_t(AF_INET)
        destAddr.sin_port   = VideoTransport.VIDEO_UDP_PORT.bigEndian

        let result = windowsHost.withCString { cStr in
            inet_pton(AF_INET, cStr, &destAddr.sin_addr)
        }
        guard result == 1 else {
            print("SanskyStream VideoTransport: inet_pton() failed for host: \(windowsHost)")
            close(socketFd)
            socketFd = -1
            return nil
        }

        print("SanskyStream VideoTransport: initialized. Target: \(windowsHost):\(VideoTransport.VIDEO_UDP_PORT)")
    }

    deinit {
        if socketFd >= 0 {
            close(socketFd)
        }
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    /// Send one complete encoded H.264 frame.
    /// The frame will be split into fragments if it exceeds VIDEO_MAX_PAYLOAD.
    ///
    /// - Parameters:
    ///   - naluData:       Raw H.264 bytes in Annex B format (start codes present).
    ///                     For keyframes, SPS/PPS precede the IDR NALU.
    ///   - presentationUs: Presentation timestamp in microseconds.
    ///   - isKeyframe:     True if this is an IDR/keyframe.
    func sendFrame(naluData: Data, presentationUs: UInt64, isKeyframe: Bool) {
        guard socketFd >= 0 else { return }
        guard !naluData.isEmpty else { return }

        let maxPayload      = VideoTransport.VIDEO_MAX_PAYLOAD
        let headerSize      = VideoTransport.VIDEO_HEADER_SIZE
        let totalBytes      = naluData.count
        let fragmentCount16 = UInt16((totalBytes + maxPayload - 1) / maxPayload)
        let fragmentCount   = Int(fragmentCount16)

        let currentFrameId = frameId
        frameId = frameId &+ 1  // wraps at UInt32.max

        for fi in 0 ..< fragmentCount {
            let payloadStart  = fi * maxPayload
            let payloadEnd    = min(payloadStart + maxPayload, totalBytes)
            let payloadSlice  = naluData[payloadStart ..< payloadEnd]
            let payloadSize   = payloadSlice.count

            let datagramSize = headerSize + payloadSize
            var datagram     = Data(count: datagramSize)

            let currentSeq = packetSeq
            packetSeq = packetSeq &+ 1

            // Serialize header fields at their defined byte offsets.
            datagram.writeU32LE(offset:  0, value: VideoTransport.VIDEO_MAGIC)
            datagram.writeU8  (offset:  4, value: VideoTransport.VIDEO_PROTOCOL_VERSION)
            datagram.writeU8  (offset:  5, value: VideoTransport.VIDEO_FRAGMENT_TYPE)
            let flags: UInt8 = isKeyframe ? VideoTransport.VIDEO_FLAG_KEYFRAME : 0
            datagram.writeU8  (offset:  6, value: flags)
            datagram.writeU32LE(offset:  7, value: currentFrameId)
            datagram.writeU64LE(offset: 11, value: presentationUs)
            datagram.writeU32LE(offset: 19, value: currentSeq)
            datagram.writeU16LE(offset: 23, value: UInt16(fi))
            datagram.writeU16LE(offset: 25, value: fragmentCount16)
            datagram.writeU32LE(offset: 27, value: UInt32(payloadSize))

            // Copy payload.
            datagram.replaceSubrange(headerSize ..< datagramSize, with: payloadSlice)

            // Send.
            sendDatagram(datagram)
        }
    }

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    private func sendDatagram(_ data: Data) {
        var addr = destAddr
        let sent = data.withUnsafeBytes { rawBuf -> Int in
            withUnsafeBytes(of: &addr) { addrBuf -> Int in
                let addrPtr = addrBuf.baseAddress!.assumingMemoryBound(to: sockaddr.self)
                return sendto(socketFd,
                              rawBuf.baseAddress,
                              rawBuf.count,
                              0,
                              addrPtr,
                              socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if sent < 0 {
            print("SanskyStream VideoTransport: sendto() failed: \(errno)")
        }
    }
}

// ---------------------------------------------------------------------------
// Data extension — explicit little-endian serialization helpers.
// These write each byte individually to guarantee LE byte order regardless
// of the host architecture and without relying on any struct memory layout.
// ---------------------------------------------------------------------------

private extension Data {

    mutating func writeU8(offset: Int, value: UInt8) {
        self[offset] = value
    }

    mutating func writeU16LE(offset: Int, value: UInt16) {
        self[offset]     = UInt8(value & 0xFF)
        self[offset + 1] = UInt8((value >> 8) & 0xFF)
    }

    mutating func writeU32LE(offset: Int, value: UInt32) {
        self[offset]     = UInt8(value & 0xFF)
        self[offset + 1] = UInt8((value >> 8)  & 0xFF)
        self[offset + 2] = UInt8((value >> 16) & 0xFF)
        self[offset + 3] = UInt8((value >> 24) & 0xFF)
    }

    mutating func writeU64LE(offset: Int, value: UInt64) {
        for i in 0 ..< 8 {
            self[offset + i] = UInt8((value >> (8 * i)) & 0xFF)
        }
    }
}
