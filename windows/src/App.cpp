#include "App.h"
#include "Logger.h"

namespace SanskyStream {

App::App() : m_isRunning(true) {
    LOG_INFO("Initializing Application...");

    // -----------------------------------------------------------------------
    // Window
    // -----------------------------------------------------------------------
    m_window = std::make_unique<Window>(1280, 720, L"SanskyStream Client");
    if (!m_window->GetHWND()) {
        LOG_ERROR("Failed to initialize main window.");
        m_isRunning = false;
        return;
    }

    // -----------------------------------------------------------------------
    // Renderer (D3D11 swap chain)
    // -----------------------------------------------------------------------
    m_renderer = std::make_unique<Renderer>(m_window.get());
    if (!m_renderer->Initialize()) {
        LOG_ERROR("Failed to initialize renderer.");
        m_isRunning = false;
        return;
    }

    // -----------------------------------------------------------------------
    // Network — TCP server on port 5000
    // -----------------------------------------------------------------------
    m_network = std::make_unique<Network>();

    // Wire the status callback: network thread -> Window status text
    m_network->SetStatusCallback([this](const std::string& status) {
        OnNetworkStatus(status);
    });

    // Show a placeholder before the server socket is ready
    m_window->SetStatusText("SanskyStream Client\r\n\r\nStatus: Initializing...\r\nPort: 5000");

    if (!m_network->StartServer(5000)) {
        LOG_WARN("Network server failed to start. Running without networking.");
        m_window->SetStatusText("SanskyStream Client\r\n\r\nStatus: Network Error\r\nPort: 5000");
    }
}

App::~App() {
    if (m_network) {
        m_network->StopServer();
    }
    LOG_INFO("Application shutting down.");
}

// Called from the network thread — only updates window state (fast, mutex-protected).
void App::OnNetworkStatus(const std::string& status) {
    if (m_window) {
        m_window->SetStatusText("SanskyStream Client\r\n\r\nStatus: " +
                                status + "\r\nPort: 5000");
    }
}

void App::Run() {
    if (!m_isRunning) return;

    LOG_INFO("Application entering main loop.");

    while (m_isRunning) {
        // Process Windows messages — returns false on WM_QUIT
        if (!m_window->ProcessMessages()) {
            m_isRunning = false;
            break;
        }

        // Clear the D3D back buffer with a dark background
        m_renderer->ClearColor(0.05f, 0.07f, 0.12f, 1.0f);
        m_renderer->Render(); // Present

        // Overlay GDI status text on top of the D3D output
        m_window->DrawStatusOverlay();
    }
}

} // namespace SanskyStream
