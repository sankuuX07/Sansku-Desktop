#include "App.h"
#include "Logger.h"
#include "Protocol.h"

#include <mutex>   // std::lock_guard — for m_statusMutex
#include <string>

namespace SanskyStream {

App::App() : m_isRunning(true) {
    LOG_INFO("Initializing Application...");

    // M13: pipeline diagnostics — constructed first, available to Run() loop.
    m_pipelineStats = std::make_unique<PipelineStats>();

    // M16: initialise status strings so UpdateStatusOverlay() can be called at
    // any time after construction.
    m_networkStatus   = "Waiting for Device...";
    m_discoveryStatus.clear();

    // -----------------------------------------------------------------------
    // M12: A/V Synchronizer — constructed first so all other components can
    // receive a raw pointer to it safely.  The synchronizer starts in the
    // unanchored state; the first decoded audio packet sets the clock anchor.
    // -----------------------------------------------------------------------
    m_avSync = std::make_unique<AVSynchronizer>();

    // -----------------------------------------------------------------------
    // M16: Device Discovery — start advertising this PC and browsing for
    // iPhones running SanskyStream.  Non-fatal: if mDNS is unavailable,
    // the existing manual connection still works.
    // -----------------------------------------------------------------------
    m_deviceDiscovery = std::make_unique<DeviceDiscovery>();
    m_deviceDiscovery->SetDeviceFoundCallback([this](const DiscoveredDevice& dev) {
        OnDeviceFound(dev);
    });
    m_deviceDiscovery->SetDeviceLostCallback([this](const std::string& name) {
        OnDeviceLost(name);
    });
    if (!m_deviceDiscovery->Start()) {
        LOG_WARN("App: DeviceDiscovery unavailable — manual IP entry still works.");
    }

    // -----------------------------------------------------------------------
    // M14: OBS Bridge — creates the named shared memory segment that the
    // OBS plugin (sansky-source.dll) reads.  Non-fatal: if OBS is not
    // installed or CreateFileMapping fails, the pipeline continues normally.
    // Must be constructed before VideoReceiver and AudioReceiver so it is
    // valid when those components call SetOBSBridge().
    // -----------------------------------------------------------------------
    m_obsBridge = std::make_unique<OBSBridge>();
    if (m_obsBridge->IsReady()) {
        LOG_INFO("App: OBSBridge ready. OBS plugin can connect.");
    } else {
        LOG_WARN("App: OBSBridge not available. OBS integration disabled.");
    }

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
    m_videoReceiver->SetAVSync(m_avSync.get());   // M12
    m_videoReceiver->SetOBSBridge(m_obsBridge.get()); // M14: forward decoded frames to OBS shmem
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
    m_audioReceiver->SetAVSync(m_avSync.get());        // M12 — must be set before Start()
    m_audioReceiver->SetOBSBridge(m_obsBridge.get()); // M14: forward decoded PCM to OBS shmem
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

    // Initial status text via unified overlay helper.
    UpdateStatusOverlay();

    if (!m_network->StartServer(Protocol::CONTROL_TCP_PORT)) {
        LOG_WARN("Network server failed to start. Running without networking.");
        {
            std::lock_guard<std::mutex> lk(m_statusMutex);
            m_networkStatus = "Network Error";
        }
        UpdateStatusOverlay();
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
    // M16: stop discovery before WinSock cleanup.
    if (m_deviceDiscovery) {
        m_deviceDiscovery->Stop();
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

    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        m_networkStatus = status;
    }
    UpdateStatusOverlay();
}

// ---------------------------------------------------------------------------
// M16: Discovery event handlers
// ---------------------------------------------------------------------------

void App::OnDeviceFound(const DiscoveredDevice& device) {
    // Build a discovery status line from all available devices.
    const auto allDevices = m_deviceDiscovery->GetDevices();

    std::string disc;
    int senderCount = 0;
    for (const auto& d : allDevices) {
        if (d.state != DeviceState::Available) continue;
        if (d.role == DeviceRole::Receiver) continue; // skip ourselves
        ++senderCount;
        disc += "\r\n  iPhone: " + d.displayName +
                "  " + d.ipAddress + ":" + std::to_string(d.port) +
                "  ver=" + std::to_string(d.protocolVersion);
    }

    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        if (senderCount > 0) {
            m_discoveryStatus = "\r\n--- Discovered Devices ---" + disc;
        } else {
            m_discoveryStatus.clear();
        }
    }
    UpdateStatusOverlay();

    LOG_INFO("App: DeviceFound '" + device.displayName +
             "' at " + device.ipAddress + ":" + std::to_string(device.port));
}

void App::OnDeviceLost(const std::string& displayName) {
    // Rebuild discovery status from current available list (excluding lost device).
    const auto allDevices = m_deviceDiscovery->GetDevices();

    std::string disc;
    int senderCount = 0;
    for (const auto& d : allDevices) {
        if (d.state != DeviceState::Available) continue;
        if (d.role == DeviceRole::Receiver)   continue;
        ++senderCount;
        disc += "\r\n  iPhone: " + d.displayName +
                "  " + d.ipAddress + ":" + std::to_string(d.port);
    }

    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        if (senderCount > 0) {
            m_discoveryStatus = "\r\n--- Discovered Devices ---" + disc;
        } else {
            m_discoveryStatus.clear();
        }
    }
    UpdateStatusOverlay();

    LOG_INFO("App: DeviceLost '" + displayName + "'.");
}

// Combines m_networkStatus + m_discoveryStatus and pushes to the window.
// Called from any thread; Window::SetStatusText is internally mutex-guarded.
void App::UpdateStatusOverlay() {
    std::string net;
    std::string disc;
    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        net  = m_networkStatus;
        disc = m_discoveryStatus;
    }

    const std::string text =
        "SanskyStream\r\n\r\nStatus: " + net +
        "\r\nControl: TCP :" + std::to_string(Protocol::CONTROL_TCP_PORT) +
        "\r\nVideo:   UDP :" + std::to_string(Protocol::VIDEO_UDP_PORT) +
        disc;

    if (m_window) {
        m_window->SetStatusText(text);
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
