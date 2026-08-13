#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

namespace SanskyStream {

// TCP server that listens on a fixed port and accepts one client at a time.
// All blocking socket work runs on a dedicated server thread, never on the UI thread.
class Network {
public:
    Network();
    ~Network();

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    // Start listening on the given port. Returns false if the socket cannot be opened.
    bool StartServer(uint16_t port);

    // Stop the server and join the server thread cleanly.
    void StopServer();

    // Thread-safe query of whether a client is currently connected.
    bool IsClientConnected() const { return m_isConnected.load(); }

    // Register a callback invoked (from the server thread) when status changes.
    // Called with "Waiting for Device..." or "Connected".
    void SetStatusCallback(std::function<void(const std::string&)> callback);

private:
    void ServerThread();

    std::function<void(const std::string&)> m_statusCallback;

    SOCKET              m_listenSocket;
    SOCKET              m_clientSocket;
    std::mutex          m_clientSocketMutex;    // Guards m_clientSocket

    std::thread         m_serverThread;
    std::atomic<bool>   m_isRunning;
    std::atomic<bool>   m_isConnected;
};

} // namespace SanskyStream
