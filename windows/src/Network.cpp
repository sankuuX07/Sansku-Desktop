#include "Network.h"
#include "Logger.h"
#include "Protocol.h"

#pragma comment(lib, "ws2_32.lib")

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Sanity limits for TCP packet framing.
// ---------------------------------------------------------------------------
// Maximum audio payload we will buffer in one receive (4 MiB safety cap).
static constexpr uint32_t MAX_AUDIO_PAYLOAD_SIZE = 4 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Network::Network()
    : m_listenSocket(INVALID_SOCKET)
    , m_clientSocket(INVALID_SOCKET)
    , m_isRunning(false)
    , m_isConnected(false)
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        LOG_ERROR("WSAStartup failed with error: " + std::to_string(result));
    } else {
        LOG_INFO("WinSock2 initialized.");
    }
}

Network::~Network() {
    StopServer();
    WSACleanup();
    LOG_INFO("WinSock2 cleaned up.");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Network::SetStatusCallback(std::function<void(const std::string&)> callback) {
    m_statusCallback = std::move(callback);
}

void Network::SetAudioPacketCallback(
    std::function<void(const uint8_t*, size_t)> callback) {
    m_audioCallback = std::move(callback);
}

bool Network::StartServer(uint16_t port) {
    LOG_INFO("Network server starting...");

    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) {
        LOG_ERROR("Failed to create listen socket. WSA error: " +
                  std::to_string(WSAGetLastError()));
        return false;
    }

    int optVal = 1;
    if (setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&optVal),
                   static_cast<int>(sizeof(optVal))) == SOCKET_ERROR) {
        LOG_WARN("setsockopt SO_REUSEADDR failed. WSA error: " +
                 std::to_string(WSAGetLastError()));
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port        = htons(port);

    if (bind(m_listenSocket,
             reinterpret_cast<const sockaddr*>(&serverAddr),
             static_cast<int>(sizeof(serverAddr))) == SOCKET_ERROR) {
        LOG_ERROR("bind() failed. WSA error: " + std::to_string(WSAGetLastError()));
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, 1) == SOCKET_ERROR) {
        LOG_ERROR("listen() failed. WSA error: " + std::to_string(WSAGetLastError()));
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    LOG_INFO("Listening on port " + std::to_string(port));
    m_isRunning    = true;
    m_serverThread = std::thread(&Network::ServerThread, this);

    if (m_statusCallback) m_statusCallback("Waiting for Device...");
    return true;
}

void Network::StopServer() {
    if (!m_isRunning && !m_serverThread.joinable()) return;

    m_isRunning = false;

    {
        std::lock_guard<std::mutex> lock(m_clientSocketMutex);
        if (m_clientSocket != INVALID_SOCKET) {
            shutdown(m_clientSocket, SD_BOTH);
            closesocket(m_clientSocket);
            m_clientSocket = INVALID_SOCKET;
        }
    }

    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    if (m_serverThread.joinable()) m_serverThread.join();

    m_isConnected = false;
    LOG_INFO("Network server stopped.");
}

// ---------------------------------------------------------------------------
// RecvExact
//
// Reliably reads exactly 'n' bytes from 'sock'.
// Returns false on disconnect, error, or shutdown.
// ---------------------------------------------------------------------------

bool Network::RecvExact(SOCKET sock, uint8_t* buf, size_t n) {
    size_t received = 0;
    while (received < n && m_isRunning) {
        int r = recv(sock,
                     reinterpret_cast<char*>(buf + received),
                     static_cast<int>(n - received), 0);
        if (r <= 0) {
            if (r == 0) {
                LOG_INFO("Network: Client disconnected during framed read.");
            } else if (m_isRunning) {
                LOG_ERROR("Network: recv() error. WSA error: " +
                          std::to_string(WSAGetLastError()));
            }
            return false;
        }
        received += static_cast<size_t>(r);
    }
    return m_isRunning;
}

// ---------------------------------------------------------------------------
// ServerThread
//
// Runs entirely off the UI thread.
// M11: uses PacketHeader framing to identify and dispatch audio packets.
//
// Wire format per Protocol.h (little-endian):
//   Offset 0: uint32_t magic       (CONTROL_MAGIC = 0x52545353)
//   Offset 4: uint8_t  type        (PacketType enum)
//   Offset 5: uint32_t payloadSize (bytes that follow)
//   Offset 9: [payloadSize bytes]
// ---------------------------------------------------------------------------

void Network::ServerThread() {
    // PacketHeader is 9 bytes: 4 (magic) + 1 (type) + 4 (payloadSize).
    static constexpr size_t kHeaderSize = sizeof(Protocol::PacketHeader);
    static_assert(kHeaderSize == 9, "PacketHeader size mismatch");

    while (m_isRunning) {
        sockaddr_in clientAddr    = {};
        int         clientAddrLen = static_cast<int>(sizeof(clientAddr));

        SOCKET clientSock = accept(m_listenSocket,
                                   reinterpret_cast<sockaddr*>(&clientAddr),
                                   &clientAddrLen);
        if (clientSock == INVALID_SOCKET) {
            if (m_isRunning) {
                LOG_ERROR("accept() failed. WSA error: " +
                          std::to_string(WSAGetLastError()));
            }
            break;
        }
        if (!m_isRunning) { closesocket(clientSock); break; }

        {
            std::lock_guard<std::mutex> lock(m_clientSocketMutex);
            m_clientSocket = clientSock;
        }
        m_isConnected = true;
        LOG_INFO("Client connected.");
        if (m_statusCallback) m_statusCallback("Connected");

        // ------------------------------------------------------------------
        // Receive loop — read framed packets.
        // ------------------------------------------------------------------
        while (m_isRunning) {
            // Read the 9-byte PacketHeader.
            uint8_t headerBuf[kHeaderSize];
            if (!RecvExact(clientSock, headerBuf, kHeaderSize)) break;

            // Deserialize (little-endian, explicit byte reads as per Protocol.h).
            uint32_t magic       = 0;
            uint32_t payloadSize = 0;
            uint8_t  typeRaw     = 0;

            magic  = static_cast<uint32_t>(headerBuf[0])
                   | (static_cast<uint32_t>(headerBuf[1]) << 8)
                   | (static_cast<uint32_t>(headerBuf[2]) << 16)
                   | (static_cast<uint32_t>(headerBuf[3]) << 24);
            typeRaw     = headerBuf[4];
            payloadSize = static_cast<uint32_t>(headerBuf[5])
                        | (static_cast<uint32_t>(headerBuf[6]) << 8)
                        | (static_cast<uint32_t>(headerBuf[7]) << 16)
                        | (static_cast<uint32_t>(headerBuf[8]) << 24);

            // Validate magic.
            if (magic != Protocol::CONTROL_MAGIC) {
                LOG_ERROR("Network: Bad packet magic 0x" + [magic]{
                    char b[9]; snprintf(b, sizeof(b), "%08X", magic);
                    return std::string(b); }() + " — dropping client.");
                break;
            }

            const auto pktType = static_cast<Protocol::PacketType>(typeRaw);

            // Sanity-check payload size.
            if (payloadSize > MAX_AUDIO_PAYLOAD_SIZE) {
                LOG_ERROR("Network: Oversized payload " +
                          std::to_string(payloadSize) + " bytes — dropping client.");
                break;
            }

            // Read the payload.
            std::vector<uint8_t> payload(payloadSize);
            if (payloadSize > 0) {
                if (!RecvExact(clientSock, payload.data(), payloadSize)) break;
            }

            // Dispatch by packet type.
            switch (pktType) {
            case Protocol::PacketType::Audio:
                if (m_audioCallback && !payload.empty()) {
                    m_audioCallback(payload.data(), payload.size());
                }
                break;

            case Protocol::PacketType::Control:
                // Log control messages as plain text (existing behavior).
                if (!payload.empty()) {
                    LOG_INFO("Received control message: " +
                             std::string(reinterpret_cast<const char*>(payload.data()),
                                         payload.size()));
                }
                break;

            case Protocol::PacketType::Video:
                // Video travels over UDP (port 5001), not TCP.
                // Log unexpected video-on-TCP for diagnostics.
                LOG_WARN("Network: Unexpected video packet on TCP (ignored).");
                break;

            default:
                LOG_WARN("Network: Unknown packet type " +
                         std::to_string(typeRaw) + " — ignoring.");
                break;
            }
        }

        // ------------------------------------------------------------------
        // Client disconnected — clean up and wait for next connection.
        // ------------------------------------------------------------------
        {
            std::lock_guard<std::mutex> lock(m_clientSocketMutex);
            if (m_clientSocket != INVALID_SOCKET) {
                closesocket(m_clientSocket);
                m_clientSocket = INVALID_SOCKET;
            }
        }
        m_isConnected = false;

        if (m_isRunning && m_statusCallback) {
            m_statusCallback("Waiting for Device...");
        }
    }
}

} // namespace SanskyStream
