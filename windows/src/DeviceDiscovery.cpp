// ---------------------------------------------------------------------------
// DeviceDiscovery.cpp — M16: Local Network Device Discovery
//
// Windows native mDNS / DNS-SD implementation.
// API: DnsServiceRegister / DnsServiceBrowse / DnsServiceResolve (dnsapi.dll)
// SDK: Windows 10 SDK 10.0.17763+ (windns.h)
// Runtime requirement: DNS Client service (Dnscache), enabled by default.
// No Bonjour SDK, no Apple runtime, no cloud.
// ---------------------------------------------------------------------------

#include "DeviceDiscovery.h"
#include "Logger.h"
#include "Protocol.h"     // CONTROL_TCP_PORT

// winsock2.h / ws2tcpip.h must precede any inclusion of windows.h
// sub-headers to avoid redefinition errors.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

// Link dnsapi at compile time.  ws2_32 already linked by Network.cpp.
#pragma comment(lib, "dnsapi.lib")

namespace SanskyStream {

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------
namespace {

// Convert a NUL-terminated wide string to a UTF-8 std::string.
// Returns an empty string if ws is null or conversion fails.
static std::string WstrToUtf8(const wchar_t* ws) {
    if (!ws || ws[0] == L'\0') return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string s(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), needed, nullptr, nullptr);
    return s;
}

// Convert DNS_SERVICE_INSTANCE's IP4_ADDRESS (network-byte-order DWORD)
// to a dotted-decimal string.  Returns empty string on failure.
static std::string Ip4ToString(const IP4_ADDRESS* addr) {
    if (!addr) return {};
    char buf[INET_ADDRSTRLEN] = {};
    // IP4_ADDRESS is DWORD in network byte order — cast to in_addr.
    const auto* inAddr = reinterpret_cast<const IN_ADDR*>(addr);
    if (!inet_ntop(AF_INET, inAddr, buf, sizeof(buf))) return {};
    return { buf };
}

// Find a TXT record value by key in a DNS_SERVICE_INSTANCE (case-insensitive).
static std::string GetTxtValue(PDNS_SERVICE_INSTANCE inst, const wchar_t* key) {
    if (!inst || inst->dwPropertyCount == 0 || !inst->keys || !inst->values) {
        return {};
    }
    for (DWORD i = 0; i < inst->dwPropertyCount; ++i) {
        if (inst->keys[i] && _wcsicmp(inst->keys[i], key) == 0) {
            return inst->values[i] ? WstrToUtf8(inst->values[i]) : std::string{};
        }
    }
    return {};
}

// Strip the service-type suffix and trailing dots from a DNS-SD instance name.
// e.g. "John's iPhone._sanskystream._tcp.local." → "John's iPhone"
static std::string StripSuffix(const wchar_t* fullNameW) {
    std::string name = WstrToUtf8(fullNameW);
    // Strip trailing dot.
    while (!name.empty() && name.back() == '.') name.pop_back();
    // Strip service type suffix.
    static const std::string kSuffix = "._sanskystream._tcp.local";
    if (name.size() > kSuffix.size()) {
        const std::string tail = name.substr(name.size() - kSuffix.size());
        if (tail == kSuffix) {
            name.resize(name.size() - kSuffix.size());
        }
    }
    return name;
}

// Parse "role" TXT value to DeviceRole enum.
static DeviceRole ParseRole(const std::string& roleStr) {
    if (roleStr == "sender")   return DeviceRole::Sender;
    if (roleStr == "receiver") return DeviceRole::Receiver;
    return DeviceRole::Unknown;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DeviceDiscovery::DeviceDiscovery() = default;

DeviceDiscovery::~DeviceDiscovery() {
    Stop();
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

// static
bool DeviceDiscovery::IsValidDevice(const DiscoveredDevice& dev) {
    if (dev.port == 0) return false;
    if (dev.ipAddress.empty() && dev.hostName.empty()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

void DeviceDiscovery::SetDeviceFoundCallback(DeviceFoundCallback cb) {
    m_foundCallback = std::move(cb);
}

void DeviceDiscovery::SetDeviceLostCallback(DeviceLostCallback cb) {
    m_lostCallback = std::move(cb);
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

bool DeviceDiscovery::Start() {
    if (m_running.load()) {
        LOG_WARN("DeviceDiscovery: Already running.");
        return true;
    }
    m_running = true;

    // Registration failure is non-fatal — browse still provides value.
    if (!StartRegistration()) {
        LOG_WARN("DeviceDiscovery: Registration unavailable "
                 "(non-fatal — iPhone browser will still find this PC if mDNS is active).");
    }

    const bool browsing = StartBrowse();
    if (browsing) {
        LOG_INFO("DeviceDiscovery: Started. Browsing for " +
                 WstrToUtf8(SERVICE_TYPE) + ".");
    } else {
        LOG_WARN("DeviceDiscovery: Browse failed. "
                 "Existing manual connection still functional.");
    }
    return browsing;
}

void DeviceDiscovery::Stop() {
    if (!m_running.exchange(false)) return; // Already stopped.

    StopBrowse();
    StopRegistration();
    CancelAllResolves();

    // Give thread-pool callbacks ~150 ms to observe m_running==false and return.
    // All callbacks guard themselves with if (!m_running.load()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    LOG_INFO("DeviceDiscovery: Stopped.");
}

// ---------------------------------------------------------------------------
// Registration — advertise this Windows PC as a receiver
// ---------------------------------------------------------------------------

bool DeviceDiscovery::StartRegistration() {
    // Build the service instance name: "SanskyStream <COMPUTER>._sanskystream._tcp.local"
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD   cnLen = static_cast<DWORD>(MAX_COMPUTERNAME_LENGTH + 1);
    if (!GetComputerNameW(computerName, &cnLen)) {
        wcscpy_s(computerName, L"Windows");
    }

    const std::wstring instanceName =
        std::wstring(L"SanskyStream ") + computerName +
        L"._sanskystream._tcp.local";
    const std::wstring hostName =
        std::wstring(computerName) + L".local";

    // TXT records: ver=1, role=receiver
    static const wchar_t* kKeys[]   = { L"ver", L"role" };
    static const wchar_t* kValues[] = { L"1",   L"receiver" };

    const WORD port = static_cast<WORD>(Protocol::CONTROL_TCP_PORT);

    m_serviceInstance = DnsServiceConstructInstance(
        instanceName.c_str(),
        hostName.c_str(),
        nullptr, nullptr,         // ip4 / ip6 — OS resolves our addresses
        port,
        0, 0,                     // priority, weight
        2,                        // TXT property count
        kKeys,
        kValues
    );
    if (!m_serviceInstance) {
        LOG_WARN("DeviceDiscovery: DnsServiceConstructInstance failed.");
        return false;
    }

    DNS_SERVICE_REGISTER_REQUEST req = {};
    req.Version                      = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex               = 0; // all interfaces
    req.pServiceInstance             = m_serviceInstance;
    req.pRegisterCompletionCallback  = RegisterCallback;
    req.pQueryContext                = this;
    req.hCredentials                 = nullptr;
    req.unicastEnabled               = FALSE;

    const DNS_STATUS st = DnsServiceRegister(&req, &m_registerCancel);
    if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
        LOG_WARN("DeviceDiscovery: DnsServiceRegister failed. Status=" +
                 std::to_string(st));
        DnsServiceFreeInstance(m_serviceInstance);
        m_serviceInstance = nullptr;
        return false;
    }

    m_registerActive = true;
    LOG_INFO("DeviceDiscovery: Advertising '" +
             WstrToUtf8(instanceName.c_str()) +
             "' on port " + std::to_string(port) + ".");
    return true;
}

void DeviceDiscovery::StopRegistration() {
    if (!m_registerActive) return;
    m_registerActive = false;
    DnsServiceRegisterCancel(&m_registerCancel);
    if (m_serviceInstance) {
        DnsServiceFreeInstance(m_serviceInstance);
        m_serviceInstance = nullptr;
    }
    LOG_INFO("DeviceDiscovery: Service de-registered.");
}

// static WINAPI
void WINAPI DeviceDiscovery::RegisterCallback(DWORD status,
                                               PVOID pQueryContext,
                                               PDNS_SERVICE_INSTANCE /*pInstance*/)
{
    const auto* self = static_cast<const DeviceDiscovery*>(pQueryContext);
    if (!self || !self->m_running.load()) return;

    if (status == ERROR_SUCCESS) {
        LOG_INFO("DeviceDiscovery: Registration confirmed by mDNS stack.");
    } else {
        LOG_WARN("DeviceDiscovery: Registration callback status=" +
                 std::to_string(status) + " (non-fatal).");
    }
}

// ---------------------------------------------------------------------------
// Browse — find iPhones on the local network
// ---------------------------------------------------------------------------

bool DeviceDiscovery::StartBrowse() {
    DNS_SERVICE_BROWSE_REQUEST req = {};
    req.Version         = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex  = 0; // all interfaces
    req.QueryName       = SERVICE_TYPE;
    req.pBrowseCallback = BrowseCallback;
    req.pQueryContext   = this;

    const DNS_STATUS st = DnsServiceBrowse(&req, &m_browseCancel);
    if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
        LOG_WARN("DeviceDiscovery: DnsServiceBrowse failed. Status=" +
                 std::to_string(st));
        return false;
    }

    m_browseActive = true;
    return true;
}

void DeviceDiscovery::StopBrowse() {
    if (!m_browseActive) return;
    m_browseActive = false;
    DnsServiceBrowseCancel(&m_browseCancel);
}

// static WINAPI
void WINAPI DeviceDiscovery::BrowseCallback(DWORD status,
                                             PVOID pQueryContext,
                                             PDNS_RECORD pRecord)
{
    auto* self = static_cast<DeviceDiscovery*>(pQueryContext);
    if (!self || !self->m_running.load()) {
        if (pRecord) DnsRecordListFree(pRecord, DnsFreeRecordList);
        return;
    }
    if (status != ERROR_SUCCESS || !pRecord) return;

    // Walk the linked PTR record list.
    for (PDNS_RECORD r = pRecord; r != nullptr; r = r->pNext) {
        if (r->wType != DNS_TYPE_PTR) continue;
        if (!r->Data.PTR.pNameHost)   continue;

        const std::wstring instName = r->Data.PTR.pNameHost;

        if (r->dwTtl == 0) {
            // Goodbye packet — device is leaving.
            self->OnServiceLost(instName);
        } else {
            // Device appeared or re-announced.
            self->OnServiceFound(instName);
        }
    }

    DnsRecordListFree(pRecord, DnsFreeRecordList);
}

// ---------------------------------------------------------------------------
// Resolve — map PTR instance name to IP address + port
// ---------------------------------------------------------------------------

void DeviceDiscovery::ResolveInstance(const std::wstring& instanceName) {
    // Allocate context on the heap — must stay alive until callback fires.
    auto* ctx = new ResolveCtx{ this, instanceName, {} };

    DNS_SERVICE_RESOLVE_REQUEST req = {};
    req.Version                         = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex                  = 0;
    req.QueryName                       = const_cast<PWSTR>(ctx->instanceName.c_str());
    req.pResolveCompletionCallback      = ResolveCallback;
    req.pQueryContext                   = ctx;

    const DNS_STATUS st = DnsServiceResolve(&req, &ctx->cancel);
    if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
        LOG_WARN("DeviceDiscovery: DnsServiceResolve failed for '" +
                 WstrToUtf8(instanceName.c_str()) +
                 "'. Status=" + std::to_string(st));
        delete ctx;
        return;
    }

    std::lock_guard<std::mutex> lk(m_resolveMutex);
    m_pendingResolves.push_back(ctx);
}

void DeviceDiscovery::CancelAllResolves() {
    std::vector<ResolveCtx*> pending;
    {
        std::lock_guard<std::mutex> lk(m_resolveMutex);
        pending.swap(m_pendingResolves);
    }
    for (ResolveCtx* ctx : pending) {
        // Cancel fires the callback with ERROR_OPERATION_ABORTED.
        // The callback checks m_running==false and returns without processing.
        // The context is deleted inside the callback.
        DnsServiceResolveCancel(&ctx->cancel);
    }
}

// static WINAPI
void WINAPI DeviceDiscovery::ResolveCallback(DWORD status,
                                              PVOID pQueryContext,
                                              PDNS_SERVICE_INSTANCE pInstance)
{
    auto* ctx = static_cast<ResolveCtx*>(pQueryContext);
    if (!ctx) return;

    DeviceDiscovery* self     = ctx->self;
    std::wstring     instName = std::move(ctx->instanceName);

    // Remove from pending list before freeing — do this under the resolve mutex.
    if (self) {
        std::lock_guard<std::mutex> lk(self->m_resolveMutex);
        auto& pend = self->m_pendingResolves;
        pend.erase(std::remove(pend.begin(), pend.end(), ctx), pend.end());
    }
    delete ctx;

    const bool ok = (status == ERROR_SUCCESS) && pInstance && self &&
                    self->m_running.load();
    if (!ok) {
        if (pInstance) DnsServiceFreeInstance(pInstance);
        return;
    }

    self->OnServiceResolved(instName, pInstance);
    DnsServiceFreeInstance(pInstance);
}

// ---------------------------------------------------------------------------
// Service lifecycle — called from thread-pool callbacks
// ---------------------------------------------------------------------------

void DeviceDiscovery::OnServiceFound(const std::wstring& instanceName) {
    if (!m_running.load()) return;

    // Avoid duplicate resolves for an already-available device.
    {
        std::lock_guard<std::mutex> lk(m_devicesMutex);
        const std::string name = StripSuffix(instanceName.c_str());
        for (const auto& d : m_devices) {
            if (d.displayName == name && d.state == DeviceState::Available) {
                return; // Already known — skip.
            }
        }
    }
    ResolveInstance(instanceName);
}

void DeviceDiscovery::OnServiceLost(const std::wstring& instanceName) {
    if (!m_running.load()) return;
    RemoveDevice(StripSuffix(instanceName.c_str()));
}

void DeviceDiscovery::OnServiceResolved(const std::wstring& /*instanceName*/,
                                         PDNS_SERVICE_INSTANCE pInstance)
{
    if (!pInstance) return;

    DiscoveredDevice dev;

    if (pInstance->pszInstanceName) {
        dev.displayName = StripSuffix(pInstance->pszInstanceName);
    }
    if (pInstance->pszHostName) {
        dev.hostName = WstrToUtf8(pInstance->pszHostName);
    }
    if (pInstance->ip4Address) {
        dev.ipAddress = Ip4ToString(pInstance->ip4Address);
    }

    dev.port  = pInstance->wPort;
    dev.state = DeviceState::Available;

    // Parse TXT records.
    const std::string verStr  = GetTxtValue(pInstance, L"ver");
    const std::string roleStr = GetTxtValue(pInstance, L"role");

    if (!verStr.empty()) {
        try {
            dev.protocolVersion = static_cast<uint32_t>(std::stoul(verStr));
        } catch (...) {
            dev.protocolVersion = 0;
        }
    }
    dev.role = ParseRole(roleStr);

    // Reject invalid devices.
    if (!IsValidDevice(dev)) {
        LOG_WARN("DeviceDiscovery: Resolved device '" + dev.displayName +
                 "' rejected (port=" + std::to_string(dev.port) +
                 " ip='" + dev.ipAddress + "').");
        return;
    }

    // Skip our own advertisement — we're a receiver, not a sender.
    if (dev.role == DeviceRole::Receiver) {
        return;
    }

    LOG_INFO("DeviceDiscovery: Found '" + dev.displayName +
             "' role=" + roleStr +
             " at " + dev.ipAddress + ":" + std::to_string(dev.port) +
             " ver=" + std::to_string(dev.protocolVersion));

    UpsertDevice(std::move(dev));
}

// ---------------------------------------------------------------------------
// Device list management
// ---------------------------------------------------------------------------

void DeviceDiscovery::UpsertDevice(DiscoveredDevice dev) {
    // Capture a copy for the callback (fired outside the lock).
    DiscoveredDevice snapshot = dev;
    {
        std::lock_guard<std::mutex> lk(m_devicesMutex);
        for (auto& existing : m_devices) {
            if (existing.displayName == dev.displayName) {
                existing = dev;
                goto fireCallback; // NOLINT — cleaner than double-checking bool
            }
        }
        m_devices.push_back(dev);
    }
fireCallback:
    if (m_foundCallback) m_foundCallback(snapshot);
}

void DeviceDiscovery::RemoveDevice(const std::string& displayName) {
    bool wasAvailable = false;
    {
        std::lock_guard<std::mutex> lk(m_devicesMutex);
        for (auto& d : m_devices) {
            if (d.displayName == displayName &&
                d.state == DeviceState::Available) {
                d.state      = DeviceState::Unavailable;
                wasAvailable = true;
                break;
            }
        }
    }
    if (wasAvailable) {
        LOG_INFO("DeviceDiscovery: '" + displayName + "' is no longer available.");
        if (m_lostCallback) m_lostCallback(displayName);
    }
}

// ---------------------------------------------------------------------------
// GetDevices — thread-safe snapshot
// ---------------------------------------------------------------------------

std::vector<DiscoveredDevice> DeviceDiscovery::GetDevices() const {
    std::lock_guard<std::mutex> lk(m_devicesMutex);
    return m_devices;
}

// ---------------------------------------------------------------------------
// Testing helpers
// ---------------------------------------------------------------------------

void DeviceDiscovery::InjectDeviceForTesting(DiscoveredDevice device) {
    if (!IsValidDevice(device)) {
        LOG_WARN("DeviceDiscovery::InjectDeviceForTesting: invalid device '" +
                 device.displayName +
                 "' (port=" + std::to_string(device.port) +
                 " ip='" + device.ipAddress + "') — rejected.");
        return;
    }
    UpsertDevice(std::move(device));
}

void DeviceDiscovery::RemoveDeviceForTesting(const std::string& displayName) {
    RemoveDevice(displayName);
}

} // namespace SanskyStream
