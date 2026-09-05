#pragma once

#include "Window.h"
#include "Renderer.h"
#include "Network.h"
#include "VideoUdpReceiver.h"
#include "VideoReceiver.h"
#include "VideoFrameQueue.h"
#include "AudioReceiver.h"
#include "AVSynchronizer.h"
#include "OBSBridge.h"         // M14: shared-memory IPC bridge to OBS plugin
#include "DeviceDiscovery.h"   // M16: local-network mDNS/DNS-SD discovery
#include "PipelineStats.h"     // M13: lightweight periodic pipeline diagnostics
#include "DiscoveredDevice.h"  // M17: for m_selectedDevice

#include <memory>
#include <string>
#include <vector>

namespace SanskyStream {

class App {
public:
    App();
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    void Run();

private:
    // -----------------------------------------------------------------------
    // Network / discovery callbacks (called from non-UI threads).
    // -----------------------------------------------------------------------
    void OnNetworkStatus(const std::string& status);
    void OnAudioPacket(const uint8_t* payload, size_t size);

    // M16: DeviceDiscovery callbacks.
    void OnDeviceFound(const DiscoveredDevice& device);
    void OnDeviceLost(const std::string& displayName);

    // -----------------------------------------------------------------------
    // M17: UI action callbacks (called from main thread via WM_COMMAND).
    // -----------------------------------------------------------------------
    void OnDeviceSelected(int index);
    void OnConnectClicked();
    void OnDisconnectClicked();
    void OnManualConnect(const std::string& ip, const std::string& port);

    // -----------------------------------------------------------------------
    // M17: Periodic stats update (~500 ms, called from Run loop).
    // -----------------------------------------------------------------------
    void UpdateStreamStatsIfDue();

    // M17: Push combined status to the Window's connection state display.
    void PushConnectionState();

    // M16 legacy: Rebuild and push the combined status text (kept for compat).
    void UpdateStatusOverlay();

    // -----------------------------------------------------------------------
    // Owned components — construction order = declaration order here.
    // Destruction order is reversed (last declared → first destroyed).
    // -----------------------------------------------------------------------

    // M12: A/V synchronizer — constructed first; shared with other components.
    std::unique_ptr<AVSynchronizer>    m_avSync;

    // M16: Device discovery.
    std::unique_ptr<DeviceDiscovery>   m_deviceDiscovery;

    // M14: OBS shared-memory bridge.
    std::unique_ptr<OBSBridge>         m_obsBridge;

    // Window (M17: hosts the UI panel).
    std::unique_ptr<Window>            m_window;

    // Renderer (M8: D3D11 NV12 → RGB, letterbox, FPS).
    std::unique_ptr<Renderer>          m_renderer;

    // Network — TCP server on port 5000.
    std::unique_ptr<Network>           m_network;

    // Video pipeline.
    std::unique_ptr<VideoFrameQueue>   m_frameQueue;
    std::unique_ptr<VideoReceiver>     m_videoReceiver;
    std::unique_ptr<VideoUdpReceiver>  m_videoUdpReceiver;

    // Audio pipeline (M11).
    std::unique_ptr<AudioReceiver>     m_audioReceiver;

    // M13: periodic pipeline diagnostics.
    std::unique_ptr<PipelineStats>     m_pipelineStats;

    // -----------------------------------------------------------------------
    // M17: UI state
    // -----------------------------------------------------------------------

    // Currently tracked list of discovered sender devices (Available only).
    std::vector<DiscoveredDevice>  m_visibleDevices;

    // Zero-based index into m_visibleDevices; -1 = nothing selected.
    int                            m_selectedDeviceIndex = -1;

    // Current connection state (drives button enable/disable + status label).
    ConnectionState                m_connState = ConnectionState::Idle;

    // QPC timestamp of last stream-stats update (for ~500 ms throttle).
    LARGE_INTEGER  m_statsLastUpdate = {};
    LARGE_INTEGER  m_statsFreq       = {};

    bool m_isRunning = true;

    // M16 legacy status strings (kept so UpdateStatusOverlay still compiles).
    std::string m_networkStatus;
    std::string m_discoveryStatus;
    std::mutex  m_statusMutex;
};

} // namespace SanskyStream
