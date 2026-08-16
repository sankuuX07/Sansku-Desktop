#include "App.h"
#include "Logger.h"
#include "Protocol.h"

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
    // VideoReceiver — logs complete H.264 frames; decoder wired in M7.
    // -----------------------------------------------------------------------
    m_videoReceiver = std::make_unique<VideoReceiver>();

    // -----------------------------------------------------------------------
    // VideoUdpReceiver — binds UDP port 5001, feeds FrameAssembler,
    // delivers CompleteFrame objects to VideoReceiver.
    // -----------------------------------------------------------------------
    m_videoUdpReceiver = std::make_unique<VideoUdpReceiver>(
        [this](CompleteFrame frame) {
            m_videoReceiver->OnCompleteFrame(std::move(frame));
        });

    if (!m_videoUdpReceiver->Start(Protocol::VIDEO_UDP_PORT)) {
        LOG_WARN("VideoUdpReceiver failed to start. Video transport disabled.");
    }

    // -----------------------------------------------------------------------
    // Network — TCP server on port 5000 (control / HELLO channel).
    // This connection is unchanged from M1–M3.
    // -----------------------------------------------------------------------
    m_network = std::make_unique<Network>();
    m_network->SetStatusCallback([this](const std::string& status) {
        OnNetworkStatus(status);
    });

    m_window->SetStatusText(
        "SanskyStream Client\r\n\r\nStatus: Initializing..."
        "\r\nControl: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
        "\r\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT));

    if (!m_network->StartServer(Protocol::CONTROL_TCP_PORT)) {
        LOG_WARN("Network server failed to start. Running without networking.");
        m_window->SetStatusText(
            "SanskyStream Client\r\n\r\nStatus: Network Error"
            "\r\nControl: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
            "\r\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT));
    }
}

App::~App() {
    if (m_videoUdpReceiver) {
        m_videoUdpReceiver->Stop();
    }
    if (m_network) {
        m_network->StopServer();
    }
    LOG_INFO("Application shutting down.");
}

// Called from the network thread — only updates window state (fast).
void App::OnNetworkStatus(const std::string& status) {
    if (m_window) {
        m_window->SetStatusText(
            "SanskyStream Client\r\n\r\nStatus: " + status +
            "\r\nControl: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
            "\r\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT));
    }
}

void App::Run() {
    if (!m_isRunning) return;

    LOG_INFO("Application entering main loop.");

    while (m_isRunning) {
        if (!m_window->ProcessMessages()) {
            m_isRunning = false;
            break;
        }

        m_renderer->ClearColor(0.05f, 0.07f, 0.12f, 1.0f);
        m_renderer->Render();
        m_window->DrawStatusOverlay();
    }
}

} // namespace SanskyStream
