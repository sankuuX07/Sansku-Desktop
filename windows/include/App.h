#pragma once

#include "Window.h"
#include "Renderer.h"
#include "Network.h"
#include <memory>
#include <string>

namespace SanskyStream {

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void Run();

private:
    // Invoked from the network thread when connection state changes.
    void OnNetworkStatus(const std::string& status);

    std::unique_ptr<Window>   m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<Network>  m_network;

    bool m_isRunning;
};

} // namespace SanskyStream
