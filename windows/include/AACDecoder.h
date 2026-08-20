#pragma once

#include "DecodedAudioPacket.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// AACDecoder
//
// Decodes AAC-LC audio using the Windows Media Foundation AAC decoder MFT.
//
// Decoder technology: MFT (Media Foundation Transform)
//   - Uses MFTEnumEx with MFT_CATEGORY_AUDIO_DECODER.
//   - No FFmpeg; no third-party dependencies.
//   - Matches the H264Decoder pattern already established in this project.
//   - Input:  raw AAC access units (one per SubmitPacket() call).
//   - Output: 16-bit signed integer PCM, interleaved channels.
//
// Initialization:
//   Initialize() must be called once before SubmitPacket().
//   sampleRate and channelCount must match what the iOS encoder sends.
//   Default values for AAC-LC: 44100 Hz (ReplayKit app audio), stereo.
//
// Thread-safety: NOT internally synchronized.
//   AACDecoder must be called from one thread at a time (network thread).
//
// Lifecycle:
//   Construct → Initialize() → SubmitPacket() (many) → Flush() / destructor
// ---------------------------------------------------------------------------
class AACDecoder {
public:
    // Invoked once per decoded PCM buffer, from the calling thread.
    // Must return quickly — enqueue PCM for playback, do not block.
    using DecodedCallback = std::function<void(DecodedAudioPacket)>;

    explicit AACDecoder(DecodedCallback onDecoded);
    ~AACDecoder();

    AACDecoder(const AACDecoder&)            = delete;
    AACDecoder& operator=(const AACDecoder&) = delete;

    // Initialize the AAC decoder MFT.
    // sampleRate:   e.g. 44100, 48000
    // channelCount: e.g. 1 (mono), 2 (stereo)
    // Returns true on success.
    bool Initialize(uint32_t sampleRate, uint32_t channelCount);

    // Submit one raw AAC access unit for decoding.
    // aacData/aacSize: AAC bitstream bytes (one access unit, no ADTS header).
    // timestampUs: presentation timestamp in microseconds (preserved for M12).
    // Returns true if the packet was accepted.
    bool SubmitPacket(const uint8_t* aacData, size_t aacSize, uint64_t timestampUs);

    // Flush and drain any pending decoder output.
    void Flush();

    bool     IsInitialized()  const { return m_initialized; }
    uint32_t GetSampleRate()  const { return m_sampleRate; }
    uint32_t GetChannelCount()const { return m_channelCount; }

private:
    // Drain all output samples from the MFT after ProcessInput.
    void DrainOutput(uint64_t timestampUs);

    // Build 2-byte MPEG-4 AudioSpecificConfig for AAC-LC.
    static std::vector<uint8_t> BuildAudioSpecificConfig(uint32_t sampleRate,
                                                          uint32_t channelCount);

    DecodedCallback                      m_callback;
    Microsoft::WRL::ComPtr<IMFTransform> m_transform;

    bool     m_initialized   = false;
    uint32_t m_sampleRate    = 0;
    uint32_t m_channelCount  = 0;
    uint32_t m_bitsPerSample = 16; // Output is always 16-bit PCM
};

} // namespace SanskyStream
