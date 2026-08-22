#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// AudioPlayer
//
// Low-latency WASAPI shared-mode audio playback engine.
//
// Architecture:
//   - SubmitPCM() is called from the network thread (AACDecoder callback).
//   - A dedicated playback thread feeds WASAPI using event-driven buffering.
//   - A bounded ring buffer (~100 ms) bridges the two threads.
//
// Buffering strategy (documented per M11 spec):
//   - Ring buffer capacity: ~100 ms of audio (computed from sample rate).
//   - Overflow: oldest samples are silently overwritten (drop-oldest).
//     Prevents unbounded memory growth and keeps audio current.
//   - Underflow: WASAPI buffer filled with silence (zero samples).
//     Produces a brief gap instead of a glitch or stall.
//
// Device: default Windows audio render endpoint (no device UI, M11 scope).
//
// Threading:
//   - SubmitPCM(): network thread; protected by m_bufferMutex.
//   - PlaybackThread(): internal; event-driven from WASAPI.
//   - Stop(): safe to call from any thread.
//
// Lifecycle:
//   Initialize() → Start() → SubmitPCM() (many) → Stop()
// ---------------------------------------------------------------------------
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&)            = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Initialize WASAPI for the given PCM format.
    // Must be called before Start().
    bool Initialize(uint32_t sampleRate, uint32_t channelCount,
                    uint32_t bitsPerSample);

    // Start the playback thread and WASAPI stream.
    bool Start();

    // Stop playback, drain the thread, release all COM/WASAPI resources.
    void Stop();

    // Enqueue PCM data for playback. Thread-safe.
    // data: raw PCM bytes matching the format given to Initialize().
    // Returns bytes written (may be less than size on overflow).
    size_t SubmitPCM(const uint8_t* data, size_t size);

    bool IsPlaying()     const { return m_playing.load(); }
    bool IsInitialized() const { return m_initialized.load(); }

    // M13: Returns approximate current audio queue depth in milliseconds.
    // Useful for periodic pipeline diagnostics. Thread-safe.
    uint32_t GetQueueDepthMs() const {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        if (m_sampleRate == 0 || m_blockAlign == 0) return 0;
        const size_t bytesPerMs =
            (static_cast<size_t>(m_sampleRate) * m_blockAlign) / 1000u;
        return (bytesPerMs > 0)
                   ? static_cast<uint32_t>(m_ringUsed / bytesPerMs)
                   : 0u;
    }

    // M13: diagnostic counters — thread-safe atomic reads.
    uint64_t GetUnderflowCount() const { return m_underflowCount.load(); }
    uint64_t GetOverflowCount()  const { return m_overflowCount.load();  }

private:
    void PlaybackThread();

    // Ring buffer helpers (caller must hold m_bufferMutex).
    size_t RingFree()     const;
    size_t RingUsed()     const;
    size_t RingRead(uint8_t* dest, size_t maxBytes);
    void   RingWrite(const uint8_t* src, size_t bytes);

    // PCM format
    uint32_t m_sampleRate    = 0;
    uint32_t m_channelCount  = 0;
    uint32_t m_bitsPerSample = 0;
    uint32_t m_blockAlign    = 0;

    // WASAPI resources stored as void* to avoid pulling COM headers into .h
    void* m_audioClient      = nullptr; // IAudioClient*
    void* m_renderClient     = nullptr; // IAudioRenderClient*
    void* m_device           = nullptr; // IMMDevice*
    void* m_bufferEvent      = nullptr; // HANDLE (WASAPI buffer-ready event)
    uint32_t m_wasapiFrames  = 0;       // WASAPI buffer size in frames

    // Ring buffer
    std::vector<uint8_t> m_ring;
    size_t               m_ringCap  = 0;
    size_t               m_ringHead = 0; // write position
    size_t               m_ringTail = 0; // read position
    size_t               m_ringUsed = 0;
    mutable std::mutex   m_bufferMutex;

    // Thread control
    std::thread       m_thread;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_initialized{false};

    // Diagnostics
    std::atomic<uint64_t> m_underflowCount{0};
    std::atomic<uint64_t> m_overflowCount{0};
    std::atomic<uint64_t> m_bytesPlayed{0};
};

} // namespace SanskyStream
