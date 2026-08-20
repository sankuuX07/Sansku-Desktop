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
    // -----------------------------------------------------------------------
    m_frameQueue = std::make_unique<VideoFrameQueue>();

    // -----------------------------------------------------------------------
    // VideoReceiver — owns H264Decoder (M7); decoded frames go to the queue.
    // -----------------------------------------------------------------------
    m_videoReceiver = std::make_unique<VideoReceiver>();
    m_videoReceiver->SetFrameQueue(m_frameQueue.get());
    m_renderer->SetFrameQueue(m_frameQueue.get());

    // -----------------------------------------------------------------------
    // VideoUdpReceiver — binds UDP port 5001.
    // -----------------------------------------------------------------------
    m_videoUdpReceiver = std::make_unique<VideoUdpReceiver>(
        [this](CompleteFrame frame) {
            m_videoReceiver->OnCompleteFrame(std::move(frame));
        });

    if (!m_videoUdpReceiver->Start(Protocol::VIDEO_UDP_PORT)) {
        LOG_WARN("VideoUdpReceiver failed to start. Video transport disabled.");
    }

    // -----------------------------------------------------------------------
    // AudioReceiver — M11: AAC decoder + WASAPI playback.
    // Start with default parameters; the actual format will be negotiated
    // from the MFT output type on the first decoded packet.
    // -----------------------------------------------------------------------
    m_audioReceiver = std::make_unique<AudioReceiver>();
    if (!m_audioReceiver->Start(Protocol::AUDIO_DEFAULT_SAMPLE_RATE,
                                 Protocol::AUDIO_DEFAULT_CHANNELS)) {
        LOG_WARN("AudioReceiver failed to start. Audio playback disabled.");
        // Non-fatal: video pipeline continues.
        m_audioReceiver.reset();
    }

    // -----------------------------------------------------------------------
    // Network — TCP server on port 5000.
    // -----------------------------------------------------------------------
    m_network = std::make_unique<Network>();
    m_network->SetStatusCallback([this](const std::string& status) {
        OnNetworkStatus(status);
    });

    // M11: wire audio packet dispatch.
    m_network->SetAudioPacketCallback([this](const uint8_t* data, size_t size) {
        OnAudioPacket(data, size);
    });

    m_window->SetStatusText(
        "SanskyStream\\r\\n\\r\\nWaiting for video...\\r\\n"
        "Control: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
        "\\r\\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT));

    if (!m_network->StartServer(Protocol::CONTROL_TCP_PORT)) {
        LOG_WARN("Network server failed to start. Running without networking.");
        m_window->SetStatusText(
            "SanskyStream\\r\\n\\r\\nNetwork Error\\r\\n"
            "Control: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
            "\\r\\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT));
    }
}

App::~App() {
    // Stop audio first so the decode/playback threads shut down cleanly
    // before the network thread is stopped.
    if (m_audioReceiver) {
        m_audioReceiver->Stop();
    }
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
            "SanskyStream\\r\\n\\r\\nStatus: " + status +
            "\\r\\nControl: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
            "\\r\\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT));
    }
}

// Called from the network thread when an audio packet arrives.
void App::OnAudioPacket(const uint8_t* payload, size_t size) {
    if (m_audioReceiver) {
        m_audioReceiver->OnAudioPacketReceived(payload, size);
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

        m_renderer->Render();
        m_window->DrawStatusOverlay();
    }
}

} // namespace SanskyStream
