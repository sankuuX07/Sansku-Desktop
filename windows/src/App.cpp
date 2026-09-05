// ---------------------------------------------------------------------------
// App.cpp — M17: Application, wiring UI panel to pipeline
//
// What changed in M17:
//   - Window::SetUICallbacks() wires button/list events to App methods.
//   - OnDeviceFound / OnDeviceLost call Window::UpdateDeviceList().
//   - OnConnectClicked / OnDisconnectClicked manage the Network server.
//   - OnManualConnect() allows manual IP/port override.
//   - UpdateStreamStatsIfDue() pushes pipeline stats to the window ~500 ms.
//   - Renderer::SetPanelHeight() reduces the video viewport by kPanelHeight.
//   - Renderer no longer owns SetStatusText; App owns all status updates.
// ---------------------------------------------------------------------------

#include "App.h"
#include "Logger.h"
#include "Protocol.h"

#include <algorithm>
#include <mutex>
#include <string>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

App::App() : m_isRunning(true) {
    LOG_INFO("Initializing Application (M17)...");

    QueryPerformanceFrequency(&m_statsFreq);
    QueryPerformanceCounter(&m_statsLastUpdate);

    // M13: pipeline diagnostics.
    m_pipelineStats = std::make_unique<PipelineStats>();

    // M17: initial status strings.
    m_networkStatus   = "Waiting for Device...";
    m_discoveryStatus.clear();

    // M12: A/V synchronizer — first so other components can hold a raw ptr.
    m_avSync = std::make_unique<AVSynchronizer>();

    // -----------------------------------------------------------------------
    // M16: Device discovery.
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
    // M14: OBS Bridge.
    // -----------------------------------------------------------------------
    m_obsBridge = std::make_unique<OBSBridge>();
    if (m_obsBridge->IsReady()) {
        LOG_INFO("App: OBSBridge ready. OBS plugin can connect.");
    } else {
        LOG_WARN("App: OBSBridge not available. OBS integration disabled.");
    }

    // -----------------------------------------------------------------------
    // Window (M17: 1280×720 + kPanelHeight for the UI panel).
    // The total window height already includes the panel; the Renderer will
    // be told to use only the top (height − kPanelHeight) rows for video.
    // -----------------------------------------------------------------------
    m_window = std::make_unique<Window>(1280, 720 + Window::kPanelHeight,
                                        L"SanskyStream");
    if (!m_window->GetHWND()) {
        LOG_ERROR("Failed to initialize main window.");
        m_isRunning = false;
        return;
    }

    // Wire UI callbacks (all fire on main thread via WM_COMMAND).
    {
        UICallbacks cb;
        cb.onDeviceSelected  = [this](int idx) { OnDeviceSelected(idx); };
        cb.onConnectClicked  = [this]()         { OnConnectClicked();   };
        cb.onDisconnectClicked = [this]()       { OnDisconnectClicked(); };
        cb.onManualConnect   = [this](const std::string& ip,
                                     const std::string& port) {
            OnManualConnect(ip, port);
        };
        m_window->SetUICallbacks(std::move(cb));
    }

    // Set initial connection state so buttons are correctly enabled.
    m_window->UpdateConnectionState(ConnectionState::Discovering);

    // -----------------------------------------------------------------------
    // Renderer (D3D11, M8).
    // Pass panel height so the letterbox viewport stays in the video area.
    // -----------------------------------------------------------------------
    m_renderer = std::make_unique<Renderer>(m_window.get());
    m_renderer->SetPanelHeight(Window::kPanelHeight);
    if (!m_renderer->Initialize()) {
        LOG_ERROR("Failed to initialize renderer.");
        m_isRunning = false;
        return;
    }
    m_renderer->SetAVSync(m_avSync.get()); // M12

    // -----------------------------------------------------------------------
    // Video pipeline.
    // -----------------------------------------------------------------------
    m_frameQueue = std::make_unique<VideoFrameQueue>();

    m_videoReceiver = std::make_unique<VideoReceiver>();
    m_videoReceiver->SetFrameQueue(m_frameQueue.get());
    m_videoReceiver->SetAVSync(m_avSync.get());
    m_videoReceiver->SetOBSBridge(m_obsBridge.get());
    m_renderer->SetFrameQueue(m_frameQueue.get());

    m_videoUdpReceiver = std::make_unique<VideoUdpReceiver>(
        [this](CompleteFrame frame) {
            m_videoReceiver->OnCompleteFrame(std::move(frame));
        });

    if (!m_videoUdpReceiver->Start(Protocol::VIDEO_UDP_PORT)) {
        LOG_WARN("VideoUdpReceiver failed to start. Video transport disabled.");
    }

    // -----------------------------------------------------------------------
    // Audio pipeline (M11).
    // -----------------------------------------------------------------------
    m_audioReceiver = std::make_unique<AudioReceiver>();
    m_audioReceiver->SetAVSync(m_avSync.get());
    m_audioReceiver->SetOBSBridge(m_obsBridge.get());
    if (!m_audioReceiver->Start(Protocol::AUDIO_DEFAULT_SAMPLE_RATE,
                                Protocol::AUDIO_DEFAULT_CHANNELS)) {
        LOG_WARN("AudioReceiver failed to start. Audio playback disabled.");
        m_audioReceiver.reset();
    }

    // -----------------------------------------------------------------------
    // Network — TCP server on port 5000.
    // -----------------------------------------------------------------------
    m_network = std::make_unique<Network>();
    m_network->SetStatusCallback([this](const std::string& status) {
        OnNetworkStatus(status);
    });
    m_network->SetAudioPacketCallback([this](const uint8_t* data, size_t size) {
        OnAudioPacket(data, size);
    });

    if (!m_network->StartServer(Protocol::CONTROL_TCP_PORT)) {
        LOG_WARN("Network server failed to start. Running without networking.");
        m_window->UpdateConnectionState(ConnectionState::Error,
                                        "Network unavailable — check firewall");
    }

    // Push initial stream stats to the stat bar.
    {
        StreamStats s;
        s.audioOk  = (m_audioReceiver != nullptr);
        s.obsReady = (m_obsBridge && m_obsBridge->IsReady());
        m_window->UpdateStreamStats(s);
    }

    LOG_INFO("App initialisation complete.");
}

// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------

App::~App() {
    if (m_audioReceiver)     m_audioReceiver->Stop();
    if (m_videoUdpReceiver)  m_videoUdpReceiver->Stop();
    if (m_network)           m_network->StopServer();
    if (m_deviceDiscovery)   m_deviceDiscovery->Stop();
    LOG_INFO("Application shutting down.");
}

// ---------------------------------------------------------------------------
// Run — main loop
// ---------------------------------------------------------------------------

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

        // M13: periodic pipeline diagnostics (every 5 s, log only).
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

        // M17: update stat bar ~every 500 ms.
        UpdateStreamStatsIfDue();
    }
}

// ---------------------------------------------------------------------------
// UpdateStreamStatsIfDue — throttled at ~500 ms
// ---------------------------------------------------------------------------

void App::UpdateStreamStatsIfDue() {
    if (!m_window) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t elapsedMs = (m_statsFreq.QuadPart > 0)
        ? ((now.QuadPart - m_statsLastUpdate.QuadPart) * 1000LL) /
           m_statsFreq.QuadPart
        : 1000LL;

    if (elapsedMs < 500) return;
    m_statsLastUpdate = now;

    StreamStats s;
    s.fps           = m_renderer ? m_renderer->GetFPS()      : 0.0f;
    s.videoWidth    = m_renderer ? m_renderer->VideoWidth()  : 0;
    s.videoHeight   = m_renderer ? m_renderer->VideoHeight() : 0;
    s.hasVideo      = m_renderer && m_renderer->HasVideo();
    s.audioOk       = (m_audioReceiver != nullptr);
    s.obsReady      = (m_obsBridge && m_obsBridge->IsReady());
    s.framesDropped = m_videoReceiver ? m_videoReceiver->GetFramesDropped() : 0;

    if (m_avSync) {
        const SyncStats ss = m_avSync->GetStats();
        s.avDiffMs   = ss.avDiffUs / 1000LL;
        s.avAnchored = ss.isAnchored;
    }

    m_window->UpdateStreamStats(s);
}

// ---------------------------------------------------------------------------
// OnNetworkStatus — called from the network thread
// ---------------------------------------------------------------------------

void App::OnNetworkStatus(const std::string& status) {
    // M12: reset AVSync on disconnect.
    if (status == "Waiting for Device..." && m_avSync) {
        m_avSync->Reset();
        LOG_INFO("App: AVSynchronizer reset on client disconnect.");
    }

    // Update the connection state.
    if (status == "Connected") {
        m_connState = ConnectionState::Connected;
    } else {
        // Back to Idle unless we're already in Error.
        if (m_connState != ConnectionState::Error)
            m_connState = ConnectionState::Idle;
    }

    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        m_networkStatus = status;
    }

    // UpdateConnectionState must be called on the main thread.
    // Network callbacks fire from the server thread, so post a message.
    if (m_window && m_window->GetHWND()) {
        // We use the connection state already set above; just invalidate the
        // window so DrawStatusOverlay fires on the next Render() iteration.
        // The actual UpdateConnectionState is called from PushConnectionState()
        // which runs from the main thread in UpdateStreamStatsIfDue.
        // Use PostMessage(WM_NULL) as a lightweight wake-up signal.
        PostMessage(m_window->GetHWND(), WM_NULL, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// PushConnectionState — called from main thread to sync UI buttons/label.
// ---------------------------------------------------------------------------

void App::PushConnectionState() {
    if (!m_window) return;
    std::string detail;
    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        detail = m_networkStatus;
    }
    m_window->UpdateConnectionState(m_connState, detail);
}

// ---------------------------------------------------------------------------
// OnAudioPacket — called from network thread
// ---------------------------------------------------------------------------

void App::OnAudioPacket(const uint8_t* payload, size_t size) {
    if (m_audioReceiver)
        m_audioReceiver->OnAudioPacketReceived(payload, size);
}

// ---------------------------------------------------------------------------
// M16 Discovery callbacks
// ---------------------------------------------------------------------------

void App::OnDeviceFound(const DiscoveredDevice& device) {
    LOG_INFO("App: DeviceFound '" + device.displayName +
             "' at " + device.ipAddress + ":" + std::to_string(device.port));

    // Rebuild visible device list from the current snapshot.
    const auto all = m_deviceDiscovery->GetDevices();
    m_visibleDevices.clear();
    for (const auto& d : all) {
        if (d.state != DeviceState::Available) continue;
        if (d.role  == DeviceRole::Receiver)   continue;
        m_visibleDevices.push_back(d);
    }

    // Clamp selected index.
    if (m_selectedDeviceIndex >= static_cast<int>(m_visibleDevices.size()))
        m_selectedDeviceIndex = -1;

    if (m_window) {
        m_window->UpdateDeviceList(m_visibleDevices);
        // Transition to Discovering if we were Idle, so the user knows
        // something appeared.
        if (m_connState == ConnectionState::Idle)
            m_connState = ConnectionState::Discovering;
        PushConnectionState();
    }

    (void)device; // name already used above
}

void App::OnDeviceLost(const std::string& displayName) {
    LOG_INFO("App: DeviceLost '" + displayName + "'.");

    const auto all = m_deviceDiscovery->GetDevices();
    m_visibleDevices.clear();
    for (const auto& d : all) {
        if (d.state != DeviceState::Available) continue;
        if (d.role  == DeviceRole::Receiver)   continue;
        m_visibleDevices.push_back(d);
    }

    if (m_selectedDeviceIndex >= static_cast<int>(m_visibleDevices.size()))
        m_selectedDeviceIndex = -1;

    if (m_window) {
        m_window->UpdateDeviceList(m_visibleDevices);

        // If the lost device was selected and we were waiting, update state.
        if (m_connState == ConnectionState::WaitingClient && m_visibleDevices.empty())
            m_connState = ConnectionState::Idle;

        PushConnectionState();
    }
}

// ---------------------------------------------------------------------------
// M17 UI action callbacks — called from main thread (WM_COMMAND)
// ---------------------------------------------------------------------------

void App::OnDeviceSelected(int index) {
    m_selectedDeviceIndex = index;
    LOG_INFO("App: Device selected index=" + std::to_string(index));

    // Enable Connect when a device is selected and we're not already connected.
    if (m_connState == ConnectionState::Idle ||
        m_connState == ConnectionState::Discovering) {
        PushConnectionState();
    }
}

void App::OnConnectClicked() {
    // Windows is the TCP server — it accepts connections from the iPhone.
    // "Connect" sets the state to WaitingClient so the user gets feedback.
    // The server is already listening (StartServer was called in App::App()).
    // If the server had been stopped (after Disconnect), restart it.
    if (m_network && !m_network->IsClientConnected()) {
        // Re-start the server if it is not currently running.
        // StopServer + StartServer is the cleanest way to reset.
        m_network->StopServer();
        if (!m_network->StartServer(Protocol::CONTROL_TCP_PORT)) {
            LOG_WARN("App: OnConnectClicked: failed to restart server.");
            m_connState = ConnectionState::Error;
            PushConnectionState();
            return;
        }
    }

    m_connState = ConnectionState::WaitingClient;
    PushConnectionState();

    LOG_INFO("App: Ready — waiting for iPhone to connect on TCP :" +
             std::to_string(Protocol::CONTROL_TCP_PORT));
}

void App::OnDisconnectClicked() {
    if (!m_network) return;

    LOG_INFO("App: Disconnect requested.");
    m_network->StopServer();
    m_connState = ConnectionState::Idle;
    PushConnectionState();

    // Reset A/V sync for the next session.
    if (m_avSync) m_avSync->Reset();

    // Restart listening immediately so the next connection can arrive.
    if (!m_network->StartServer(Protocol::CONTROL_TCP_PORT)) {
        LOG_WARN("App: Failed to re-listen after disconnect.");
        m_connState = ConnectionState::Error;
        PushConnectionState();
    }
}

void App::OnManualConnect(const std::string& ip, const std::string& port) {
    // Windows is the server — manual IP/port on this side is stored for
    // reference / display only. Log it and update status.
    LOG_INFO("App: Manual endpoint set to " + ip + ":" + port);
    // Nothing more to do — the server already listens on 5000.
    // The iPhone must connect to our IP on port 5000.
}

// ---------------------------------------------------------------------------
// UpdateStatusOverlay — M16 legacy (kept so the text overlay still works).
// The text is now set via UpdateStreamStats → RebuildStatText.
// ---------------------------------------------------------------------------

void App::UpdateStatusOverlay() {
    // This method is now a no-op; M17 routes everything through
    // UpdateStreamStats and PushConnectionState.
}

} // namespace SanskyStream
