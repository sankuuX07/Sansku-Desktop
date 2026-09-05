#pragma once

#include "Window.h"
#include "Renderer.h"
#include "Network.h"
#include "VideoUdpReceiver.h"
#include "VideoReceiver.h"
#include "VideoFrameQueue.h"
#include "AudioReceiver.h"
#include "AVSynchronizer.h"
#include "OBSBridge.h"       // M14: shared-memory IPC bridge to OBS plugin
#include "DeviceDiscovery.h"  // M16: local-network mDNS/DNS-SD discovery
#include "PipelineStats.h"  // M13: lightweight periodic pipeline diagnostics

#include <memory>
#include <string>

namespace SanskyStream {

class App {
public:
    App();
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    void Run();

private:
    // Invoked from the network thread when the TCP control connection changes state.
    void OnNetworkStatus(const std::string& status);

    // Invoked from the network thread when a complete
    // audio payload is received over TCP.
    void OnAudioPacket(const uint8_t* payload, size_t size);

    // M16: Invoked from a DeviceDiscovery thread-pool callback when a device
    // is found or lost.  Rebuilds the status overlay text.
    void OnDeviceFound(const DiscoveredDevice& device);
    void OnDeviceLost(const std::string& displayName);

    // Rebuild and push the combined status text (network + discovery).
    void UpdateStatusOverlay();

    // M12: A/V synchronizer — owned here, shared (non-owning) with VideoReceiver,
    // AudioReceiver, and Renderer.  Must be constructed before those components.
    std::unique_ptr<AVSynchronizer>    m_avSync;            // M12: master A/V clock

    // M16: Device discovery — owns the mDNS browser and advertiser.
    // Declared before m_window so it is destroyed after Stop() and before
    // WinSock2 is cleaned up by Network's destructor.
    std::unique_ptr<DeviceDiscovery>   m_deviceDiscovery;   // M16: LAN discovery

    // M14: OBS shared-memory bridge — must be declared before m_videoReceiver and
    // m_audioReceiver so it is destroyed AFTER both stop writing to the segment.
    std::unique_ptr<OBSBridge>         m_obsBridge;         // M14: shmem IPC to OBS plugin

    std::unique_ptr<Window>            m_window;
    std::unique_ptr<Renderer>          m_renderer;
    std::unique_ptr<Network>           m_network;           // TCP control (port 5000)
    std::unique_ptr<VideoFrameQueue>   m_frameQueue;        // shared between VideoReceiver + Renderer
    std::unique_ptr<VideoReceiver>     m_videoReceiver;     // H264 decoder
    std::unique_ptr<VideoUdpReceiver>  m_videoUdpReceiver;  // UDP video transport
    std::unique_ptr<AudioReceiver>     m_audioReceiver;     // M11: AAC decoder + WASAPI playback
    std::unique_ptr<PipelineStats>     m_pipelineStats;     // M13: periodic latency diagnostics

    // M16: Status text components — combined by UpdateStatusOverlay().
    std::string m_networkStatus;    // Last value from OnNetworkStatus()
    std::string m_discoveryStatus;  // Formatted discovered-device list
    std::mutex  m_statusMutex;      // Guards both status strings

    bool m_isRunning;
};

} // namespace SanskyStream
