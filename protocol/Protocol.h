#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// SanskyStream Wire Protocol
//
// Byte order: LITTLE-ENDIAN for all multi-byte integers.
// Fields are serialized explicitly — never cast raw structs to wire format.
// ---------------------------------------------------------------------------

namespace SanskyStream {
namespace Protocol {

// ---------------------------------------------------------------------------
// Control channel (TCP, port 5000)
// ---------------------------------------------------------------------------

constexpr uint32_t CONTROL_MAGIC = 0x52545353U; // 'SSTR' LE

enum class PacketType : uint8_t {
    Video   = 1,
    Audio   = 2,
    Control = 3
};

#pragma pack(push, 1)

// Simple control-channel framing (M1–M3, preserved).
struct PacketHeader {
    uint32_t  magic;        // CONTROL_MAGIC
    PacketType type;        // PacketType enum
    uint32_t  payloadSize;  // Bytes of payload following this header
};

// Legacy video payload header (kept for reference; superseded by M6 UDP header).
struct VideoPayloadHeader {
    uint64_t timestamp;  // Presentation timestamp (microseconds)
    uint8_t  isKeyFrame; // 1 = keyframe (IDR), 0 = non-keyframe
    // Followed by raw H.264 NALUs in Annex B format
};

// Audio payload header — M10/M11.
// Sent over TCP (port 5000) inside a PacketHeader with PacketType::Audio.
struct AudioPayloadHeader {
    uint64_t timestamp;  // Presentation timestamp (microseconds, little-endian)
    // Followed immediately by raw AAC-LC access unit bytes.
    // The AAC frame contains NO ADTS header — raw access unit only.
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Video transport channel (UDP, port 5001) — Milestone 6
//
// Every video datagram begins with a VideoFragmentHeader (31 bytes),
// followed immediately by the fragment payload.
//
// Field layout (little-endian):
//   Offset  Size  Name             Description
//   ------  ----  ---------------  ----------------------------------------
//     0      4    magic            VIDEO_MAGIC (0x564D5653 — 'SSMV' LE)
//     4      1    version          VIDEO_PROTOCOL_VERSION (currently 1)
//     5      1    packetType       Must equal VideoFragmentType (0x01)
//     6      1    flags            Bit 0: isKeyframe. Bits 1-7: reserved = 0
//     7      4    frameId          Monotonically increasing frame counter
//    11      8    presentationUs   Presentation timestamp in microseconds
//    19      4    packetSeq        Global UDP packet sequence number
//    23      2    fragmentIndex    0-based index of this fragment in the frame
//    25      2    fragmentCount    Total fragments for this frame (>= 1)
//    27      4    payloadSize      Byte count of payload following header
//    31     --    payload          Raw H.264 NALU bytes (Annex B)
// ---------------------------------------------------------------------------

// Magic identifying a SanskyStream video UDP datagram ('SSMV' in LE).
constexpr uint32_t VIDEO_MAGIC            = 0x564D5653U;

// Current video protocol version.
constexpr uint8_t  VIDEO_PROTOCOL_VERSION = 1;

// The only defined packet type for the video channel in M6.
constexpr uint8_t  VIDEO_FRAGMENT_TYPE    = 0x01;

// Flag bit in the flags byte.
constexpr uint8_t  VIDEO_FLAG_KEYFRAME    = 0x01;

// Fixed size of the VideoFragmentHeader in bytes.
constexpr uint32_t VIDEO_HEADER_SIZE      = 31;

// Maximum H.264 payload bytes per UDP datagram.
// Chosen to keep total datagram <= ~1331 bytes (well under 1472-byte
// Ethernet MTU minus IP+UDP headers, leaving room for Wi-Fi/PPPoE overhead).
constexpr uint32_t VIDEO_MAX_PAYLOAD      = 1300;

// UDP port for the video media channel.
constexpr uint16_t VIDEO_UDP_PORT         = 5001;

// TCP port for the control channel (audio also travels here as PacketType::Audio).
constexpr uint16_t CONTROL_TCP_PORT       = 5000;

// ---------------------------------------------------------------------------
// Audio constants — M10/M11
//
// The iOS encoder uses AVAudioFormatInt16 (Float32 resampled or direct 16-bit)
// via AudioToolbox AAC-LC encoder. ReplayKit delivers 44100 Hz stereo on most
// devices; 48000 Hz is also possible. We negotiate the actual rate from the
// first decoded output type rather than hard-coding here.
// These defaults are used by AudioReceiver when no explicit rate is specified.
// ---------------------------------------------------------------------------

// Default AAC-LC sample rate (ReplayKit app audio, most devices).
constexpr uint32_t AUDIO_DEFAULT_SAMPLE_RATE  = 44100;

// Default channel count (stereo app audio from ReplayKit).
constexpr uint32_t AUDIO_DEFAULT_CHANNELS     = 2;

// AAC-LC audio object type (ISO 14496-3).
constexpr uint8_t  AUDIO_AAC_OBJECT_TYPE      = 2;

// ---------------------------------------------------------------------------
// Byte-offset constants for VideoFragmentHeader serialization.
// Both the Swift sender and C++ receiver use these to read/write fields
// at fixed offsets, avoiding any dependence on struct padding or alignment.
// ---------------------------------------------------------------------------
namespace VideoOffset {
    constexpr uint32_t Magic           =  0; // uint32_t (4 bytes)
    constexpr uint32_t Version         =  4; // uint8_t  (1 byte)
    constexpr uint32_t PacketType      =  5; // uint8_t  (1 byte)
    constexpr uint32_t Flags           =  6; // uint8_t  (1 byte)
    constexpr uint32_t FrameId         =  7; // uint32_t (4 bytes)
    constexpr uint32_t PresentationUs  = 11; // uint64_t (8 bytes)
    constexpr uint32_t PacketSeq       = 19; // uint32_t (4 bytes)
    constexpr uint32_t FragmentIndex   = 23; // uint16_t (2 bytes)
    constexpr uint32_t FragmentCount   = 25; // uint16_t (2 bytes)
    constexpr uint32_t PayloadSize     = 27; // uint32_t (4 bytes)
    constexpr uint32_t Payload         = 31; // variable
} // namespace VideoOffset

// Sanity bounds to protect against malicious/corrupt packets.
constexpr uint16_t VIDEO_MAX_FRAGMENTS_PER_FRAME = 1024;
constexpr uint32_t VIDEO_MAX_FRAME_SIZE          = 4 * 1024 * 1024; // 4 MiB

} // namespace Protocol
} // namespace SanskyStream
