#pragma once

#include <cstdint>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// DecodedAudioPacket
//
// A single decoded PCM audio buffer produced by AACDecoder.
//
// PCM format (determined by MFT output configuration):
//   - 16-bit signed integer, little-endian (MFAudioFormat_PCM)
//   - Interleaved channels: [ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...]
//   - For stereo 16-bit: each sample frame = 4 bytes
//
// Timestamps are preserved for M12 A/V synchronization.
// M11 does NOT use timestamps for playback timing.
// ---------------------------------------------------------------------------
struct DecodedAudioPacket {
    /// Raw interleaved PCM sample data (16-bit LE).
    std::vector<uint8_t> pcmData;

    /// Audio sample rate in Hz (e.g. 44100, 48000).
    uint32_t sampleRate = 0;

    /// Number of audio channels (1 = mono, 2 = stereo).
    uint32_t channelCount = 0;

    /// Bits per sample per channel (16 for PCM output).
    uint32_t bitsPerSample = 0;

    /// Original presentation timestamp in microseconds.
    /// Preserved from the iPhone encoder via AudioPayloadHeader.
    /// Available for M12 A/V synchronization; unused in M11.
    uint64_t timestampUs = 0;

    /// Number of PCM sample frames in this packet (per-channel count).
    /// Total bytes = sampleCount * channelCount * (bitsPerSample / 8)
    uint32_t sampleCount = 0;
};

} // namespace SanskyStream
