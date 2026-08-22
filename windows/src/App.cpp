#include "App.h"
#include "Logger.h"
#include "Protocol.h"

namespace SanskyStream {

App::App() : m_isRunning(true) {
    LOG_INFO("Initializing Application...");

    // M13: pipeline diagnostics — constructed first, available to Run() loop.
    m_pipelineStats = std::make_unique<PipelineStats>();

    // -----------------------------------------------------------------------
    // M12: A/V Synchronizer — constructed first so all other components can
    // receive a raw pointer to it safely.  The synchronizer starts in the
    // unanchored state; the first decoded audio packet sets the clock anchor.
    // -----------------------------------------------------------------------
    m_avSync = std::make_unique<AVSynchronizer>();

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
    // M12: give the renderer a non-owning pointer so it can display sync stats.
    m_renderer->SetAVSync(m_avSync.get());

    // -----------------------------------------------------------------------
    // VideoFrameQueue — single-slot latest-frame store shared between
    // VideoReceiver (receive thread producer) and Renderer (main thread consumer).
    // -----------------------------------------------------------------------
    m_frameQueue = std::make_unique<VideoFrameQueue>();

    // -----------------------------------------------------------------------
    // VideoReceiver — owns H264Decoder (M7); decoded frames go to the queue.
    // M12: wire AVSynchronizer so stale frames are dropped before enqueuing.
    // -----------------------------------------------------------------------
    m_videoReceiver = std::make_unique<VideoReceiver>();
    m_videoReceiver->SetFrameQueue(m_frameQueue.get());
    m_videoReceiver->SetAVSync(m_avSync.get());  // M12
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
    // M12: wire AVSynchronizer so decoded audio timestamps anchor the clock.
    // -----------------------------------------------------------------------
    m_audioReceiver = std::make_unique<AudioReceiver>();
    m_audioReceiver->SetAVSync(m_avSync.get());  // M12 — must be set before Start()
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
    // m_avSync is destroyed last (it is the first member declared in App.h,
    // so it is destroyed last by C++ destruction order — correct).
    LOG_INFO("Application shutting down.");
}

// Called from the network thread — only updates window state (fast).
void App::OnNetworkStatus(const std::string& status) {
    // M12: reset the synchronizer on disconnect so stale timestamps from the
    // previous stream do not corrupt the next connection.
    if (status == "Waiting for Device..." && m_avSync) {
        m_avSync->Reset();
        LOG_INFO("App: AVSynchronizer reset on client disconnect.");
    }

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

        // M13: periodic pipeline diagnostics — builds snapshot and logs every 5 s.
        // Zero overhead between reports (QPC guard in PipelineStats::Report).
        if (m_pipelineStats) {
            PipelineStatsSnapshot snap;
            if (m_videoReceiver) {
                snap.framesReceived = m_videoReceiver->GetFramesReceived();
                snap.framesDecoded  = m_videoReceiver->GetFramesDecoded();
                snap.framesDropped  = m_videoReceiver->GetFramesDropped();
            }
            if (m_audioReceiver) {
                snap.audioQueueMs    = m_audioReceiver->GetAudioQueueDepthMs();
                snap.audioUnderflows = m_audioReceiver->GetUnderflowCount();
                snap.audioOverflows  = m_audioReceiver->GetOverflowCount();
            }
            if (m_avSync) {
                const SyncStats s    = m_avSync->GetStats();
                snap.avAnchored      = s.isAnchored;
                snap.avDiffUs        = s.avDiffUs;
                snap.avSyncDrops     = s.droppedFrames;
            }
            snap.renderFps = m_renderer ? m_renderer->GetFPS() : 0.0f;
            m_pipelineStats->Report(snap);
        }
    }
}

} // namespace SanskyStream
