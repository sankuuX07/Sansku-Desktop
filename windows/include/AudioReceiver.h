#pragma once

#include "AACDecoder.h"
#include "AudioPlayer.h"
#include "DecodedAudioPacket.h"

#include <cstdint>
#include <memory>

namespace SanskyStream {

// Forward declaration — keeps AVSynchronizer.h out of this header.
class AVSynchronizer;

// ---------------------------------------------------------------------------
// AudioReceiver — M11 / M12
//
// Owns and wires the full audio pipeline:
//
//   Network thread calls OnAudioPacketReceived()
//        ↓  parse AudioPayloadHeader (timestamp + AAC data)
//   AACDecoder::SubmitPacket()          [inline decode on network thread]
//        ↓  callback: OnDecodedAudio()
//   AVSynchronizer::NotifyAudioTimestamp()   [M12 — establishes clock anchor]
//   AudioPlayer::SubmitPCM()            [thread-safe ring buffer write]
//        ↓
//   AudioPlayer playback thread → WASAPI → speakers
//
// Threading:
//   OnAudioPacketReceived() is called from the Network thread.
//   AACDecoder is single-threaded (network thread only).
//   AVSynchronizer::NotifyAudioTimestamp() is thread-safe (internally locked).
//   AudioPlayer::SubmitPCM() is thread-safe (ring buffer + mutex).
//
// Lifecycle:
//   Construct → SetAVSync() [optional] → Start() → OnAudioPacketReceived() (many) → Stop()
// ---------------------------------------------------------------------------
class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();

    AudioReceiver(const AudioReceiver&)            = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    // Connect the A/V synchronizer (M12).
    // Must be called before Start() if sync is desired.
    // Pass nullptr to disable (pre-M12 behaviour).
    void SetAVSync(AVSynchronizer* sync);

    // Initialize decoder + player and start playback.
    // sampleRate/channelCount: expected iOS audio format.
    // Returns true on success; false if decoder or device init fails.
    bool Start(uint32_t sampleRate = 44100, uint32_t channelCount = 2);

    // Stop playback and release all resources.
    void Stop();

    // Process one audio payload received over TCP.
    // payload: bytes after PacketHeader — AudioPayloadHeader + raw AAC data.
    // payloadSize: total byte count (must be > sizeof(AudioPayloadHeader)).
    void OnAudioPacketReceived(const uint8_t* payload, size_t payloadSize);

    bool IsRunning() const { return m_running; }

private:
    void OnDecodedAudio(DecodedAudioPacket packet);

    std::unique_ptr<AACDecoder>  m_decoder;
    std::unique_ptr<AudioPlayer> m_player;

    // Non-owning pointer to the shared AVSynchronizer (owned by App).
    // Null until SetAVSync() is called.
    AVSynchronizer* m_avSync = nullptr;

    bool     m_running         = false;
    uint64_t m_packetsReceived = 0;
    uint64_t m_packetsInvalid  = 0;
};

} // namespace SanskyStream
