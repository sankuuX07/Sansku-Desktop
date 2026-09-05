#pragma once

// ---------------------------------------------------------------------------
// DiscoveredDevice — M16: Device Discovery
//
// Plain data struct representing one SanskyStream instance found on the LAN
// via mDNS / DNS-SD (_sanskystream._tcp).
//
// Populated by DeviceDiscovery::OnServiceResolved().
// The port and ipAddress are ready to be passed directly to the existing
// Network connection layer.  No IP is ever hard-coded here.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace SanskyStream {

// Discovery lifecycle state of a found device.
enum class DeviceState : uint8_t {
    Available,   // Resolved and present on the network.
    Unavailable, // Goodbye packet received, TTL expired, or network change.
};

// Role advertised in the DNS-SD TXT record.
// "sender"   = iPhone running SanskyStream (source of video/audio).
// "receiver" = Windows PC running SanskyStream (sink, TCP server on port 5000).
enum class DeviceRole : uint8_t {
    Unknown  = 0,
    Sender   = 1,
    Receiver = 2,
};

struct DiscoveredDevice {
    std::string  displayName;        // Human-readable name (e.g. "John's iPhone")
    std::string  hostName;           // mDNS hostname   (e.g. "JohnsIphone.local")
    std::string  ipAddress;          // Resolved IPv4   (e.g. "192.168.1.42")
    uint16_t     port            = 0; // TCP control port from DNS-SD record
    uint32_t     protocolVersion = 0; // "ver" TXT value; 0 = unknown
    DeviceRole   role = DeviceRole::Unknown;
    DeviceState  state = DeviceState::Available;
};

} // namespace SanskyStream
