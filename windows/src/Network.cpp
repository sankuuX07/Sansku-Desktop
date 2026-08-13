#include "Network.h"
#include "Logger.h"

#pragma comment(lib, "ws2_32.lib")

namespace SanskyStream {

// Maximum bytes read per recv() call for test data.
static constexpr int RECV_BUFFER_SIZE = 512;

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

bool Network::StartServer(uint16_t port) {
    LOG_INFO("Network server starting...");

    // Create the listening socket
    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) {
        LOG_ERROR("Failed to create listen socket. WSA error: " +
                  std::to_string(WSAGetLastError()));
        return false;
    }

    // Allow address reuse so a quick restart doesn't fail with EADDRINUSE
    int optVal = 1;
    if (setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&optVal),
                   static_cast<int>(sizeof(optVal))) == SOCKET_ERROR) {
        LOG_WARN("setsockopt SO_REUSEADDR failed. WSA error: " +
                 std::to_string(WSAGetLastError()));
    }

    // Bind to all local interfaces on the requested port
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

    // Begin listening (queue depth of 1 — one device at a time)
    if (listen(m_listenSocket, 1) == SOCKET_ERROR) {
        LOG_ERROR("listen() failed. WSA error: " + std::to_string(WSAGetLastError()));
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    LOG_INFO("Listening on port " + std::to_string(port));

    // Launch the dedicated server thread
    m_isRunning = true;
    m_serverThread = std::thread(&Network::ServerThread, this);

    // Notify the UI that we are ready
    if (m_statusCallback) {
        m_statusCallback("Waiting for Device...");
    }

    return true;
}

void Network::StopServer() {
    if (!m_isRunning && !m_serverThread.joinable()) {
        return; // Already stopped or never started
    }

    m_isRunning = false;

    // Close the client socket first — unblocks recv() in ServerThread
    {
        std::lock_guard<std::mutex> lock(m_clientSocketMutex);
        if (m_clientSocket != INVALID_SOCKET) {
            shutdown(m_clientSocket, SD_BOTH);
            closesocket(m_clientSocket);
            m_clientSocket = INVALID_SOCKET;
        }
    }

    // Close the listen socket — unblocks accept() in ServerThread
    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    m_isConnected = false;
    LOG_INFO("Network server stopped.");
}

// ---------------------------------------------------------------------------
// Server thread — runs entirely off the UI thread
// ---------------------------------------------------------------------------

void Network::ServerThread() {
    char recvBuffer[RECV_BUFFER_SIZE];

    while (m_isRunning) {
        // Block here until a client connects or the listen socket is closed
        sockaddr_in clientAddr = {};
        int clientAddrLen = static_cast<int>(sizeof(clientAddr));

        SOCKET clientSock = accept(m_listenSocket,
                                   reinterpret_cast<sockaddr*>(&clientAddr),
                                   &clientAddrLen);

        if (clientSock == INVALID_SOCKET) {
            // Expected when StopServer() closes the listen socket
            if (m_isRunning) {
                LOG_ERROR("accept() failed. WSA error: " +
                          std::to_string(WSAGetLastError()));
            }
            break;
        }

        // Guard against a stop signal arriving while we were blocked in accept()
        if (!m_isRunning) {
            closesocket(clientSock);
            break;
        }

        // Store client socket under lock so StopServer() can close it safely
        {
            std::lock_guard<std::mutex> lock(m_clientSocketMutex);
            m_clientSocket = clientSock;
        }

        m_isConnected = true;
        LOG_INFO("Client connected.");

        if (m_statusCallback) {
            m_statusCallback("Connected");
        }

        // ---------------------------------------------------------------
        // Receive loop — log any test data sent by the connected client
        // ---------------------------------------------------------------
        while (m_isRunning) {
            int bytesReceived = recv(m_clientSocket,
                                     recvBuffer,
                                     RECV_BUFFER_SIZE - 1,
                                     0);

            if (bytesReceived <= 0) {
                if (bytesReceived == 0) {
                    LOG_INFO("Client disconnected.");
                } else if (m_isRunning) {
                    LOG_ERROR("Network error during recv. WSA error: " +
                              std::to_string(WSAGetLastError()));
                }
                break;
            }

            // Log the test message (e.g. "HELLO") as a plain string
            LOG_INFO("Received: " +
                     std::string(recvBuffer, static_cast<size_t>(bytesReceived)));
        }

        // ---------------------------------------------------------------
        // Client has gone — clean up and loop back to accept()
        // ---------------------------------------------------------------
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
