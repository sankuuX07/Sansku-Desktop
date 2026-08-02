#pragma once

#include "VideoReceiver.h"
#include "AudioReceiver.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <memory>

namespace SanskyStream {

class Network {
public:
    Network(std::shared_ptr<VideoReceiver> videoRecv, std::shared_ptr<AudioReceiver> audioRecv);
    ~Network();

    bool Connect(const std::string& ipAddress, uint16_t port);
    void Disconnect();

private:
    void ReceiveLoop();

    std::shared_ptr<VideoReceiver> m_videoReceiver;
    std::shared_ptr<AudioReceiver> m_audioReceiver;

    SOCKET m_socket;
    std::thread m_recvThread;
    std::atomic<bool> m_isRunning;
};

} // namespace SanskyStream
