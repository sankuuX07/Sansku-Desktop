#include "VideoUdpReceiver.h"
#include "Logger.h"
#include "Protocol.h"

#include <cstring>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Portable little-endian read helpers.
// These deliberately avoid any UB from type-punning or unaligned access.
// ---------------------------------------------------------------------------

static uint8_t ReadU8(const uint8_t* buf, uint32_t offset)
{
    return buf[offset];
}

static uint16_t ReadU16LE(const uint8_t* buf, uint32_t offset)
{
    return static_cast<uint16_t>(
        static_cast<uint32_t>(buf[offset])       |
       (static_cast<uint32_t>(buf[offset + 1]) << 8));
}

static uint32_t ReadU32LE(const uint8_t* buf, uint32_t offset)
{
    return (static_cast<uint32_t>(buf[offset])        |
           (static_cast<uint32_t>(buf[offset + 1]) <<  8) |
           (static_cast<uint32_t>(buf[offset + 2]) << 16) |
           (static_cast<uint32_t>(buf[offset + 3]) << 24));
}

static uint64_t ReadU64LE(const uint8_t* buf, uint32_t offset)
{
    return (static_cast<uint64_t>(buf[offset])        |
           (static_cast<uint64_t>(buf[offset + 1]) <<  8) |
           (static_cast<uint64_t>(buf[offset + 2]) << 16) |
           (static_cast<uint64_t>(buf[offset + 3]) << 24) |
           (static_cast<uint64_t>(buf[offset + 4]) << 32) |
           (static_cast<uint64_t>(buf[offset + 5]) << 40) |
           (static_cast<uint64_t>(buf[offset + 6]) << 48) |
           (static_cast<uint64_t>(buf[offset + 7]) << 56));
}

// ---------------------------------------------------------------------------
// Sequence-number gap detection.
// Handles uint32_t wrap-around: a "gap" is only reported when delta > 0 and
// delta < UINT32_MAX/2.  A negative delta (delta >= UINT32_MAX/2) means
// out-of-order or duplicate; we discard those silently.
// ---------------------------------------------------------------------------

static bool IsOutOfOrderOrDuplicate(uint32_t last, uint32_t incoming)
{
    // Compute the unsigned delta.
    uint32_t delta = incoming - last; // wraps naturally in uint32_t arithmetic
    // If delta >= 2^31, incoming is "before" last — treat as duplicate/OOO.
    return delta >= 0x80000000U;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VideoUdpReceiver::VideoUdpReceiver(FrameCallback onCompleteFrame)
    : m_assembler(std::move(onCompleteFrame))
    , m_socket(INVALID_SOCKET)
    , m_isRunning(false)
{}

VideoUdpReceiver::~VideoUdpReceiver()
{
    Stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool VideoUdpReceiver::Start(uint16_t port)
{
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        LOG_ERROR("VideoUdpReceiver: socket() failed. WSA error: " +
                  std::to_string(WSAGetLastError()));
        return false;
    }

    // Allow address reuse so a quick restart doesn't fail.
    int optVal = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&optVal),
                   static_cast<int>(sizeof(optVal))) == SOCKET_ERROR) {
        LOG_WARN("VideoUdpReceiver: SO_REUSEADDR failed. WSA error: " +
                 std::to_string(WSAGetLastError()));
    }

    // Set a receive timeout so the thread can check m_isRunning periodically.
    DWORD recvTimeoutMs = 500;
    if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&recvTimeoutMs),
                   static_cast<int>(sizeof(recvTimeoutMs))) == SOCKET_ERROR) {
        LOG_WARN("VideoUdpReceiver: SO_RCVTIMEO failed. WSA error: " +
                 std::to_string(WSAGetLastError()));
    }

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(m_socket,
             reinterpret_cast<const sockaddr*>(&addr),
             static_cast<int>(sizeof(addr))) == SOCKET_ERROR) {
        LOG_ERROR("VideoUdpReceiver: bind() failed on port " +
                  std::to_string(port) +
                  ". WSA error: " + std::to_string(WSAGetLastError()));
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }

    LOG_INFO("VideoUdpReceiver: Listening on UDP port " + std::to_string(port));

    m_isRunning = true;
    m_thread    = std::thread(&VideoUdpReceiver::ReceiveThread, this);
    return true;
}

void VideoUdpReceiver::Stop()
{
    if (!m_isRunning && !m_thread.joinable()) return;

    m_isRunning = false;

    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    LOG_INFO("VideoUdpReceiver: stopped.");
}

// ---------------------------------------------------------------------------
// Receive thread
// ---------------------------------------------------------------------------

void VideoUdpReceiver::ReceiveThread()
{
    uint8_t buf[RECV_BUF_SIZE];

    while (m_isRunning) {
        sockaddr_in senderAddr = {};
        int         senderLen  = static_cast<int>(sizeof(senderAddr));

        int received = recvfrom(m_socket,
                                reinterpret_cast<char*>(buf),
                                RECV_BUF_SIZE,
                                0,
                                reinterpret_cast<sockaddr*>(&senderAddr),
                                &senderLen);

        if (received == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEINTR) {
                // Normal timeout or interrupted — loop back and check m_isRunning.
                continue;
            }
            if (m_isRunning) {
                LOG_ERROR("VideoUdpReceiver: recvfrom() error. WSA error: " +
                          std::to_string(err));
            }
            break;
        }

        if (received == 0) continue;

        ProcessDatagram(buf, received);
    }
}

// ---------------------------------------------------------------------------
// Datagram processing and header validation
// ---------------------------------------------------------------------------

void VideoUdpReceiver::ProcessDatagram(const uint8_t* buf, int len)
{
    using namespace Protocol;

    // Minimum: must be at least as large as the fixed header.
    if (len < static_cast<int>(VIDEO_HEADER_SIZE)) {
        LOG_WARN("VideoUdpReceiver: datagram too short (" +
                 std::to_string(len) + " bytes) — discarding.");
        return;
    }

    // --- Magic ---
    uint32_t magic = ReadU32LE(buf, VideoOffset::Magic);
    if (magic != VIDEO_MAGIC) {
        LOG_WARN("VideoUdpReceiver: bad magic 0x" +
                 [&](){
                     char tmp[16];
                     snprintf(tmp, sizeof(tmp), "%08X", magic);
                     return std::string(tmp);
                 }() + " — discarding.");
        return;
    }

    // --- Version ---
    uint8_t version = ReadU8(buf, VideoOffset::Version);
    if (version != VIDEO_PROTOCOL_VERSION) {
        LOG_WARN("VideoUdpReceiver: unsupported version " +
                 std::to_string(version) + " — discarding.");
        return;
    }

    // --- Packet type ---
    uint8_t pktType = ReadU8(buf, VideoOffset::PacketType);
    if (pktType != VIDEO_FRAGMENT_TYPE) {
        LOG_WARN("VideoUdpReceiver: unexpected packet type 0x" +
                 std::to_string(pktType) + " — discarding.");
        return;
    }

    // --- Flags ---
    uint8_t flags      = ReadU8(buf, VideoOffset::Flags);
    bool    isKeyframe = (flags & VIDEO_FLAG_KEYFRAME) != 0;

    // --- Frame ID ---
    uint32_t frameId = ReadU32LE(buf, VideoOffset::FrameId);

    // --- Presentation timestamp ---
    uint64_t presentationUs = ReadU64LE(buf, VideoOffset::PresentationUs);

    // --- Global packet sequence ---
    uint32_t packetSeq = ReadU32LE(buf, VideoOffset::PacketSeq);

    // Sequence number tracking.
    if (!m_seqInitialized) {
        m_lastSeq        = packetSeq;
        m_seqInitialized = true;
    } else {
        if (IsOutOfOrderOrDuplicate(m_lastSeq, packetSeq)) {
            // packetSeq is behind or equal to lastSeq — drop silently.
            return;
        }
        uint32_t gap = packetSeq - m_lastSeq - 1;
        if (gap > 0) {
            LOG_WARN("VideoUdpReceiver: detected " + std::to_string(gap) +
                     " missing packet(s) before seq " +
                     std::to_string(packetSeq));
        }
        m_lastSeq = packetSeq;
    }

    // --- Fragment index and count ---
    uint16_t fragmentIndex = ReadU16LE(buf, VideoOffset::FragmentIndex);
    uint16_t fragmentCount = ReadU16LE(buf, VideoOffset::FragmentCount);

    if (fragmentCount == 0) {
        LOG_WARN("VideoUdpReceiver: fragmentCount=0 in frame " +
                 std::to_string(frameId) + " — discarding.");
        return;
    }
    if (fragmentCount > VIDEO_MAX_FRAGMENTS_PER_FRAME) {
        LOG_WARN("VideoUdpReceiver: fragmentCount=" +
                 std::to_string(fragmentCount) + " exceeds limit for frame " +
                 std::to_string(frameId) + " — discarding.");
        return;
    }
    if (fragmentIndex >= fragmentCount) {
        LOG_WARN("VideoUdpReceiver: fragmentIndex=" +
                 std::to_string(fragmentIndex) + " >= fragmentCount=" +
                 std::to_string(fragmentCount) + " — discarding.");
        return;
    }

    // --- Payload size ---
    uint32_t payloadSize = ReadU32LE(buf, VideoOffset::PayloadSize);

    // Validate payloadSize against the actual datagram length received.
    if (payloadSize > VIDEO_MAX_PAYLOAD) {
        LOG_WARN("VideoUdpReceiver: payloadSize=" +
                 std::to_string(payloadSize) +
                 " exceeds VIDEO_MAX_PAYLOAD — discarding.");
        return;
    }

    uint32_t expectedTotal = VIDEO_HEADER_SIZE + payloadSize;
    if (static_cast<uint32_t>(len) < expectedTotal) {
        LOG_WARN("VideoUdpReceiver: datagram length " +
                 std::to_string(len) + " < header+payload " +
                 std::to_string(expectedTotal) + " — discarding.");
        return;
    }

    // All fields validated — hand off to FrameAssembler.
    m_assembler.AddFragment(frameId,
                            presentationUs,
                            isKeyframe,
                            packetSeq,
                            fragmentIndex,
                            fragmentCount,
                            buf + VIDEO_HEADER_SIZE,
                            payloadSize);
}

} // namespace SanskyStream
