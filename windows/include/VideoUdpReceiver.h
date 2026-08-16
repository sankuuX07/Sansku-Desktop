#pragma once

#include "FrameAssembler.h"
#include "VideoPacket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// VideoUdpReceiver
//
// Binds a UDP socket on VIDEO_UDP_PORT (5001) and runs a dedicated receive
// thread that reads datagrams, validates the M6 video-fragment header, tracks
// packet sequence numbers, and feeds a FrameAssembler.
//
// Responsibilities:
//   - Parse and validate every VideoFragmentHeader field.
//   - Detect missing / duplicate / out-of-order packets by packetSeq.
//   - Reject oversized, corrupt, or version-mismatched datagrams without
//     crashing.
//   - Feed valid fragments to FrameAssembler for frame reassembly.
//   - Deliver CompleteFrame objects to the supplied FrameCallback.
//
// NOT responsible for:
//   - Decoding H.264.
//   - Rendering.
//   - Audio.
//
// Thread-safety: Start/Stop are called from the main/App thread.
// The receive thread and FrameAssembler are accessed only from within the
// receive thread.
// ---------------------------------------------------------------------------
class VideoUdpReceiver {
public:
    using FrameCallback = FrameAssembler::FrameCallback;

    explicit VideoUdpReceiver(FrameCallback onCompleteFrame);
    ~VideoUdpReceiver();

    VideoUdpReceiver(const VideoUdpReceiver&)            = delete;
    VideoUdpReceiver& operator=(const VideoUdpReceiver&) = delete;

    // Open the UDP socket and start the receive thread.
    // Returns false if the socket cannot be opened.
    bool Start(uint16_t port);

    // Close the socket and join the receive thread.
    void Stop();

    bool IsRunning() const { return m_isRunning.load(); }

private:
    void ReceiveThread();

    // Parse one validated datagram and feed it to the FrameAssembler.
    void ProcessDatagram(const uint8_t* buf, int len);

    FrameAssembler       m_assembler;
    SOCKET               m_socket;
    std::thread          m_thread;
    std::atomic<bool>    m_isRunning;

    // Sequence-number tracking (global across all frames).
    bool     m_seqInitialized = false;
    uint32_t m_lastSeq        = 0;

    // Receive buffer — large enough for header + max payload + some margin.
    // Stack-allocated; no heap allocation per datagram.
    static constexpr int RECV_BUF_SIZE = 2048;
};

} // namespace SanskyStream
