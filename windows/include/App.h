#pragma once

#include "Window.h"
#include "Renderer.h"
#include "Network.h"
#include "VideoUdpReceiver.h"
#include "VideoReceiver.h"
#include "VideoFrameQueue.h"

#include <memory>
#include <string>

namespace SanskyStream {

class App {
public:
    App();
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    void Run();

private:
    // Invoked from the network thread when the TCP control connection changes state.
    void OnNetworkStatus(const std::string& status);

    std::unique_ptr<Window>           m_window;
    std::unique_ptr<Renderer>         m_renderer;
    std::unique_ptr<Network>          m_network;          // TCP control (port 5000)
    std::unique_ptr<VideoFrameQueue>  m_frameQueue;       // M8: shared between VideoReceiver + Renderer
    std::unique_ptr<VideoReceiver>    m_videoReceiver;    // M7: H264 decoder
    std::unique_ptr<VideoUdpReceiver> m_videoUdpReceiver; // M6: UDP video transport

    bool m_isRunning;
};

} // namespace SanskyStream
