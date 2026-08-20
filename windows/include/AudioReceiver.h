#pragma once

#include "AACDecoder.h"
#include "AudioPlayer.h"
#include "DecodedAudioPacket.h"

#include <cstdint>
#include <memory>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// AudioReceiver — M11
//
// Owns and wires the full audio pipeline:
//
//   Network thread calls OnAudioPacketReceived()
//        ↓  parse AudioPayloadHeader (timestamp + AAC data)
//   AACDecoder::SubmitPacket()          [inline decode on network thread]
//        ↓  callback: OnDecodedAudio()
//   AudioPlayer::SubmitPCM()            [thread-safe ring buffer write]
//        ↓
//   AudioPlayer playback thread → WASAPI → speakers
//
// Threading:
//   OnAudioPacketReceived() is called from the Network thread.
//   AACDecoder is single-threaded (network thread only).
//   AudioPlayer::SubmitPCM() is thread-safe (ring buffer + mutex).
//
// Lifecycle:
//   Construct → Start() → OnAudioPacketReceived() (many) → Stop()
// ---------------------------------------------------------------------------
class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();

    AudioReceiver(const AudioReceiver&)            = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

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

    bool     m_running         = false;
    uint64_t m_packetsReceived = 0;
    uint64_t m_packetsInvalid  = 0;
};

} // namespace SanskyStream
