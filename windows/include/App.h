#pragma once

#include "Window.h"
#include "Renderer.h"
#include "Decoder.h"
#include "Network.h"
#include "VideoReceiver.h"
#include "AudioReceiver.h"
#include <memory>

namespace SanskyStream {

class App {
public:
    App();
    ~App();

    void Run();

private:
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::shared_ptr<Decoder> m_decoder;
    std::shared_ptr<VideoReceiver> m_videoReceiver;
    std::shared_ptr<AudioReceiver> m_audioReceiver;
    std::unique_ptr<Network> m_network;
    
    bool m_isRunning;
};

} // namespace SanskyStream
