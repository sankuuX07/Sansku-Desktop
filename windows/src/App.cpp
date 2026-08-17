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
    // Renderer (D3D11 swap chain + NV12 shaders — M8)
    // -----------------------------------------------------------------------
    m_renderer = std::make_unique<Renderer>(m_window.get());
    if (!m_renderer->Initialize()) {
        LOG_ERROR("Failed to initialize renderer.");
        m_isRunning = false;
        return;
    }

    // -----------------------------------------------------------------------
    // VideoFrameQueue — single-slot latest-frame store shared between
    // VideoReceiver (receive thread producer) and Renderer (main thread consumer).
    // Constructed before VideoReceiver so it is ready before any frame arrives.
    // -----------------------------------------------------------------------
    m_frameQueue = std::make_unique<VideoFrameQueue>();

    // -----------------------------------------------------------------------
    // VideoReceiver — owns H264Decoder (M7); decoded frames go to the queue.
    // -----------------------------------------------------------------------
    m_videoReceiver = std::make_unique<VideoReceiver>();
    m_videoReceiver->SetFrameQueue(m_frameQueue.get());

    // Give the renderer the same queue so it can TryPop on every frame.
    m_renderer->SetFrameQueue(m_frameQueue.get());

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
    // -----------------------------------------------------------------------
    m_network = std::make_unique<Network>();
    m_network->SetStatusCallback([this](const std::string& status) {
        OnNetworkStatus(status);
    });

    m_window->SetStatusText(
        "SanskyStream\r\n\r\nWaiting for video...\r\n"
        "Control: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
        "\r\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT));

    if (!m_network->StartServer(Protocol::CONTROL_TCP_PORT)) {
        LOG_WARN("Network server failed to start. Running without networking.");
        m_window->SetStatusText(
            "SanskyStream\r\n\r\nNetwork Error\r\n"
            "Control: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
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
            "SanskyStream\r\n\r\nStatus: " + status +
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

        // Renderer::Render() internally:
        //   - handles pending window resize (TakeResizePending)
        //   - clears the back buffer
        //   - pops the latest decoded frame from the queue (if any)
        //   - uploads NV12 to GPU and draws the letterbox video quad
        //   - updates the FPS status overlay text
        //   - calls Present(0,0) and sleeps 1 ms when idle
        m_renderer->Render();

        m_window->DrawStatusOverlay();
    }
}

} // namespace SanskyStream
