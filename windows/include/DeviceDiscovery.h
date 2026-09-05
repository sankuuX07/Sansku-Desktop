#pragma once

// ---------------------------------------------------------------------------
// DeviceDiscovery — M16: Local Network Device Discovery
//
// Dual-role mDNS/DNS-SD component using the Windows native API
// (dnsapi.dll / windns.h, available on Windows 10 SDK 10.0.17763+).
// No Bonjour SDK, no Apple runtime, no cloud.
//
// Service type: _sanskystream._tcp.local
// TXT records : ver=<n>  role=sender|receiver
//
// ADVERTISER role
//   Registers this Windows PC as a "receiver" service so iPhones can
//   discover it automatically and pre-fill their IP/port fields.
//   Uses DnsServiceRegister().
//
// BROWSER role
//   Browses _sanskystream._tcp.local for "sender" devices (iPhones).
//   Uses DnsServiceBrowse() + DnsServiceResolve() to obtain IP and port.
//   Fires DeviceFoundCallback / DeviceLostCallback on the caller.
//
// Threading
//   Callbacks from DnsService* fire on system thread-pool threads.
//   All public methods are thread-safe.
//   Start() / Stop() must be called from the same thread.
//   Callbacks must not call Stop() to avoid a deadlock.
// ---------------------------------------------------------------------------

#include "DiscoveredDevice.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Pull in Windows and DNS-SD headers in the correct order.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windns.h>
#include <winsock2.h>

namespace SanskyStream {

// Callback invoked (from a thread-pool thread) when a device is added or updated.
using DeviceFoundCallback = std::function<void(const DiscoveredDevice&)>;

// Callback invoked (from a thread-pool thread) when a device is lost.
using DeviceLostCallback  = std::function<void(const std::string& displayName)>;

class DeviceDiscovery {
public:
    DeviceDiscovery();
    ~DeviceDiscovery();

    DeviceDiscovery(const DeviceDiscovery&)            = delete;
    DeviceDiscovery& operator=(const DeviceDiscovery&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // Start advertising + browsing. Non-fatal: returns false if mDNS is
    // unavailable, without crashing.  The existing manual connection still works.
    bool Start();

    // Stop all async operations and wait for in-flight callbacks to drain.
    // Safe to call multiple times.
    void Stop();

    bool IsRunning() const { return m_running.load(); }

    // -----------------------------------------------------------------------
    // Callbacks — set before calling Start().
    // -----------------------------------------------------------------------
    void SetDeviceFoundCallback(DeviceFoundCallback cb);
    void SetDeviceLostCallback(DeviceLostCallback  cb);

    // Thread-safe snapshot of all currently tracked devices (any DeviceState).
    std::vector<DiscoveredDevice> GetDevices() const;

    // -----------------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------------
    static constexpr const wchar_t* SERVICE_TYPE     = L"_sanskystream._tcp.local";
    static constexpr uint32_t        PROTOCOL_VERSION = 1u;

    // -----------------------------------------------------------------------
    // Validation helper — used by TestDiscovery and internally before upsert.
    // Returns true if the device has a valid port and at least one address.
    // -----------------------------------------------------------------------
    static bool IsValidDevice(const DiscoveredDevice& dev);

    // -----------------------------------------------------------------------
    // Testing helpers — inject / remove devices without real mDNS.
    // InjectDeviceForTesting runs IsValidDevice() and drops invalid devices.
    // -----------------------------------------------------------------------
    void InjectDeviceForTesting(DiscoveredDevice device);
    void RemoveDeviceForTesting(const std::string& displayName);

private:
    // -----------------------------------------------------------------------
    // Registration (advertise this PC as a receiver)
    // -----------------------------------------------------------------------
    bool StartRegistration();
    void StopRegistration();

    static void WINAPI RegisterCallback(DWORD status,
                                        PVOID pQueryContext,
                                        PDNS_SERVICE_INSTANCE pInstance);

    // -----------------------------------------------------------------------
    // Browse (find iPhones)
    // -----------------------------------------------------------------------
    bool StartBrowse();
    void StopBrowse();

    static void WINAPI BrowseCallback(DWORD status,
                                      PVOID pQueryContext,
                                      PDNS_RECORD pRecord);

    // -----------------------------------------------------------------------
    // Resolve (PTR name → IP + port)
    // -----------------------------------------------------------------------

    // Heap-allocated context for each in-flight DnsServiceResolve call.
    // Must remain stable in memory (not moved/copied) after the call.
    struct ResolveCtx {
        DeviceDiscovery* self;
        std::wstring     instanceName;
        DNS_SERVICE_CANCEL cancel;
    };

    void ResolveInstance(const std::wstring& instanceName);
    void CancelAllResolves();

    static void WINAPI ResolveCallback(DWORD status,
                                       PVOID pQueryContext,
                                       PDNS_SERVICE_INSTANCE pInstance);

    // -----------------------------------------------------------------------
    // Internal event handlers (called from callback threads)
    // -----------------------------------------------------------------------
    void OnServiceFound(const std::wstring& instanceName);
    void OnServiceLost(const std::wstring& instanceName);
    void OnServiceResolved(const std::wstring& instanceName,
                           PDNS_SERVICE_INSTANCE pInstance);

    void UpsertDevice(DiscoveredDevice dev);
    void RemoveDevice(const std::string& displayName);

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    mutable std::mutex            m_devicesMutex;
    std::vector<DiscoveredDevice> m_devices;

    mutable std::mutex            m_resolveMutex;
    std::vector<ResolveCtx*>      m_pendingResolves; // heap-allocated

    DeviceFoundCallback           m_foundCallback;
    DeviceLostCallback            m_lostCallback;

    // Registration state
    DNS_SERVICE_CANCEL            m_registerCancel{};
    PDNS_SERVICE_INSTANCE         m_serviceInstance = nullptr;
    bool                          m_registerActive  = false;

    // Browse state
    DNS_SERVICE_CANCEL            m_browseCancel{};
    bool                          m_browseActive    = false;

    std::atomic<bool>             m_running{ false };
};

} // namespace SanskyStream
