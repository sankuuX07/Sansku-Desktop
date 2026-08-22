#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Network
//
// TCP server on port 5000 (control + audio channel).
// Accepts one client at a time on a dedicated server thread.
//
// M11 additions:
//   - Proper PacketHeader framing in the receive loop.
//   - SetAudioPacketCallback() dispatches PacketType::Audio payloads.
//   - RecvExact() for reliable framed reads.
// ---------------------------------------------------------------------------
class Network {
public:
    Network();
    ~Network();

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    // Start listening on the given port. Returns false if socket cannot open.
    bool StartServer(uint16_t port);

    // Stop the server and join the server thread cleanly.
    void StopServer();

    // Thread-safe query of whether a client is currently connected.
    bool IsClientConnected() const { return m_isConnected.load(); }

    // Register a callback invoked (from the server thread) when status changes.
    void SetStatusCallback(std::function<void(const std::string&)> callback);

    // Register a callback invoked (from the server thread) when a complete
    // audio payload is received.
    // payload: pointer to AudioPayloadHeader + raw AAC bytes.
    // size: total byte count of the payload.
    // The callback must NOT hold the data beyond the call — copy if needed.
    void SetAudioPacketCallback(
        std::function<void(const uint8_t*, size_t)> callback);

private:
    void ServerThread();

    // Read exactly 'n' bytes from 'sock' into 'buf'.
    // Returns false if the socket is closed, errors, or m_isRunning goes false.
    bool RecvExact(SOCKET sock, uint8_t* buf, size_t n);

    std::function<void(const std::string&)>     m_statusCallback;
    std::function<void(const uint8_t*, size_t)> m_audioCallback;

    SOCKET              m_listenSocket;
    SOCKET              m_clientSocket;
    std::mutex          m_clientSocketMutex;  // Guards m_clientSocket

    std::thread         m_serverThread;
    std::atomic<bool>   m_isRunning;
    std::atomic<bool>   m_isConnected;

    // M13: reusable receive buffer — avoids per-packet heap allocation.
    // Only accessed from ServerThread; no synchronisation needed.
    std::vector<uint8_t> m_recvBuffer;
};

} // namespace SanskyStream
