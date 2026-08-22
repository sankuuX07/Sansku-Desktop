#include "AudioReceiver.h"
#include "AVSynchronizer.h"
#include "Logger.h"
#include "Protocol.h"

#include <cstring>

namespace SanskyStream {

// The AudioPayloadHeader is defined in Protocol.h as:
//   struct AudioPayloadHeader { uint64_t timestamp; };
// which is 8 bytes in practice (packed).
// We use the sizeof to stay in sync with any future changes.
static constexpr size_t kAudioHeaderSize = sizeof(Protocol::AudioPayloadHeader);

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioReceiver::AudioReceiver()  = default;
AudioReceiver::~AudioReceiver() { Stop(); }

// ---------------------------------------------------------------------------
// SetAVSync (M12)
// ---------------------------------------------------------------------------

void AudioReceiver::SetAVSync(AVSynchronizer* sync)
{
    m_avSync = sync;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

bool AudioReceiver::Start(uint32_t sampleRate, uint32_t channelCount)
{
    if (m_running) {
        LOG_WARN("AudioReceiver: Already running.");
        return true;
    }

    LOG_INFO("AudioReceiver: Starting audio pipeline (" +
             std::to_string(sampleRate) + " Hz, " +
             std::to_string(channelCount) + " ch)...");

    // Create the AAC decoder.
    // The callback feeds decoded PCM directly to the player.
    m_decoder = std::make_unique<AACDecoder>(
        [this](DecodedAudioPacket pkt) { OnDecodedAudio(std::move(pkt)); });

    if (!m_decoder->Initialize(sampleRate, channelCount)) {
        LOG_ERROR("AudioReceiver: AACDecoder initialization failed.");
        m_decoder.reset();
        return false;
    }

    // Create and initialize the WASAPI player.
    m_player = std::make_unique<AudioPlayer>();
    if (!m_player->Initialize(sampleRate, channelCount, 16 /*bitsPerSample*/)) {
        LOG_ERROR("AudioReceiver: AudioPlayer initialization failed.");
        m_decoder.reset();
        m_player.reset();
        return false;
    }

    if (!m_player->Start()) {
        LOG_ERROR("AudioReceiver: AudioPlayer failed to start.");
        m_decoder.reset();
        m_player.reset();
        return false;
    }

    m_running = true;
    LOG_INFO("AudioReceiver: Audio pipeline running.");
    return true;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void AudioReceiver::Stop()
{
    if (!m_running) return;

    LOG_INFO("AudioReceiver: Stopping audio pipeline...");

    // Flush any remaining decoder output before stopping playback.
    if (m_decoder) {
        m_decoder->Flush();
        m_decoder.reset();
    }

    if (m_player) {
        m_player->Stop();
        m_player.reset();
    }

    m_running = false;

    LOG_INFO("AudioReceiver: Stopped. Packets received=" +
             std::to_string(m_packetsReceived) + ", invalid=" +
             std::to_string(m_packetsInvalid) + ".");
}

// ---------------------------------------------------------------------------
// OnAudioPacketReceived
//
// Called from the Network thread.
// Payload layout (per Protocol.h):
//   [AudioPayloadHeader: uint64_t timestamp] [raw AAC bytes...]
// ---------------------------------------------------------------------------

void AudioReceiver::OnAudioPacketReceived(const uint8_t* payload,
                                           size_t payloadSize)
{
    ++m_packetsReceived;

    if (!m_running || !m_decoder) {
        // Pipeline not started yet — silently discard.
        return;
    }

    // Validate minimum size.
    if (!payload || payloadSize <= kAudioHeaderSize) {
        ++m_packetsInvalid;
        LOG_WARN("AudioReceiver: Packet too small (" +
                 std::to_string(payloadSize) + " bytes) — discarding.");
        return;
    }

    // Extract timestamp from AudioPayloadHeader.
    // The header field is little-endian uint64_t at offset 0.
    uint64_t timestampUs = 0;
    std::memcpy(&timestampUs, payload, sizeof(uint64_t));
    // Note: Protocol.h specifies little-endian for all multi-byte integers.
    // On little-endian Windows (x86/x64), memcpy gives the correct value.

    // AAC data follows immediately after the header.
    const uint8_t* aacData  = payload + kAudioHeaderSize;
    const size_t   aacSize  = payloadSize - kAudioHeaderSize;

    if (aacSize == 0) {
        ++m_packetsInvalid;
        LOG_WARN("AudioReceiver: Packet has header but no AAC data — discarding.");
        return;
    }

    // Submit to the decoder. On success the decoded PCM callback fires
    // synchronously, which calls AudioPlayer::SubmitPCM().
    if (!m_decoder->SubmitPacket(aacData, aacSize, timestampUs)) {
        ++m_packetsInvalid;
        // SubmitPacket already logs the failure.
    }
}

// ---------------------------------------------------------------------------
// OnDecodedAudio — callback from AACDecoder (network thread)
// ---------------------------------------------------------------------------

void AudioReceiver::OnDecodedAudio(DecodedAudioPacket packet)
{
    if (!m_player || packet.pcmData.empty()) return;

    // M12: notify the A/V synchronizer of this audio timestamp so it can
    // establish (or maintain) the playback clock anchor.
    // The first non-zero call anchors the clock; subsequent calls update
    // m_lastAudioPtsUs for drift detection.
    if (m_avSync && packet.timestampUs > 0) {
        m_avSync->NotifyAudioTimestamp(packet.timestampUs);
    }

    // Submit PCM to the player's ring buffer.
    // AudioPlayer::SubmitPCM() is thread-safe.
    m_player->SubmitPCM(packet.pcmData.data(), packet.pcmData.size());
}

} // namespace SanskyStream
