#include "Network.h"
#include "Protocol.h"
#include "Logger.h"

#pragma comment(lib, "ws2_32.lib")

namespace SanskyStream {

Network::Network(std::shared_ptr<VideoReceiver> videoRecv, std::shared_ptr<AudioReceiver> audioRecv)
    : m_videoReceiver(videoRecv), m_audioReceiver(audioRecv), m_socket(INVALID_SOCKET), m_isRunning(false) {
    
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        LOG_ERROR("WSAStartup failed with error: " + std::to_string(result));
    }
}

Network::~Network() {
    Disconnect();
    WSACleanup();
}

bool Network::Connect(const std::string& ipAddress, uint16_t port) {
    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) {
        LOG_ERROR("Error creating socket.");
        return false;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, ipAddress.c_str(), &serverAddr.sin_addr);

    if (connect(m_socket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LOG_ERROR("Failed to connect to server.");
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }

    LOG_INFO("Connected to server at " + ipAddress + ":" + std::to_string(port));

    m_isRunning = true;
    m_recvThread = std::thread(&Network::ReceiveLoop, this);
    
    return true;
}

void Network::Disconnect() {
    m_isRunning = false;
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
    LOG_INFO("Disconnected from network.");
}

void Network::ReceiveLoop() {
    std::vector<uint8_t> buffer(1024 * 1024); // 1MB buffer

    while (m_isRunning) {
        // Read header
        Protocol::PacketHeader header;
        int bytesReceived = recv(m_socket, reinterpret_cast<char*>(&header), sizeof(Protocol::PacketHeader), 0);
        
        if (bytesReceived <= 0) {
            LOG_ERROR("Connection closed or error receiving.");
            m_isRunning = false;
            break;
        }

        if (header.magic != Protocol::MAGIC_BYTES) {
            LOG_ERROR("Invalid magic bytes received.");
            continue;
        }

        // Read payload
        std::vector<uint8_t> payload(header.payloadSize);
        uint32_t totalPayloadReceived = 0;
        
        while (totalPayloadReceived < header.payloadSize) {
            int received = recv(m_socket, reinterpret_cast<char*>(payload.data() + totalPayloadReceived), header.payloadSize - totalPayloadReceived, 0);
            if (received <= 0) {
                m_isRunning = false;
                break;
            }
            totalPayloadReceived += received;
        }

        if (!m_isRunning) break;

        // Dispatch
        if (header.type == Protocol::PacketType::Video && m_videoReceiver) {
            m_videoReceiver->OnVideoPacketReceived(payload);
        } else if (header.type == Protocol::PacketType::Audio && m_audioReceiver) {
            m_audioReceiver->OnAudioPacketReceived(payload);
        }
    }
}

} // namespace SanskyStream
