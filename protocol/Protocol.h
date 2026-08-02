#pragma once

#include <stdint.h>

namespace SanskyStream {
namespace Protocol {

constexpr uint32_t MAGIC_BYTES = 0x52545353; // 'SSTR' in little-endian

enum class PacketType : uint8_t {
    Video = 1,
    Audio = 2,
    Control = 3
};

#pragma pack(push, 1)

// All network communication is assumed to be Little Endian.

struct PacketHeader {
    uint32_t magic;      // MAGIC_BYTES
    PacketType type;     // PacketType enum
    uint32_t payloadSize;// Size of the payload following this header
};

struct VideoPayloadHeader {
    uint64_t timestamp;  // Presentation timestamp (microseconds)
    uint8_t isKeyFrame;  // 1 if keyframe (IDR), 0 otherwise
    // Followed by raw H.264 NALUs
};

struct AudioPayloadHeader {
    uint64_t timestamp;  // Presentation timestamp (microseconds)
    // Followed by raw AAC data
};

#pragma pack(pop)

} // namespace Protocol
} // namespace SanskyStream
