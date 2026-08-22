#pragma once

#include "Window.h"
#include "Renderer.h"
#include "Network.h"
#include "VideoUdpReceiver.h"
#include "VideoReceiver.h"
#include "VideoFrameQueue.h"
#include "AudioReceiver.h"
#include "AVSynchronizer.h"

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

    // Invoked from the network thread when an audio packet arrives over TCP.
    void OnAudioPacket(const uint8_t* payload, size_t size);

    // M12: A/V synchronizer — owned here, shared (non-owning) with VideoReceiver,
    // AudioReceiver, and Renderer.  Must be constructed before those components.
    std::unique_ptr<AVSynchronizer>    m_avSync;            // M12: master A/V clock

    std::unique_ptr<Window>            m_window;
    std::unique_ptr<Renderer>          m_renderer;
    std::unique_ptr<Network>           m_network;           // TCP control (port 5000)
    std::unique_ptr<VideoFrameQueue>   m_frameQueue;        // shared between VideoReceiver + Renderer
    std::unique_ptr<VideoReceiver>     m_videoReceiver;     // H264 decoder
    std::unique_ptr<VideoUdpReceiver>  m_videoUdpReceiver;  // UDP video transport
    std::unique_ptr<AudioReceiver>     m_audioReceiver;     // M11: AAC decoder + WASAPI playback

    bool m_isRunning;
};

} // namespace SanskyStream
