#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Windows QPC header is included in the .cpp only.
// Here we just forward-declare the wall-clock helper return type.

namespace SanskyStream {

// ---------------------------------------------------------------------------
// AVSynchronizer — M12
//
// Provides a shared timing reference for audio/video synchronization.
//
// Design: Audio-anchored wall clock
// ===================================
// Both audio and video timestamps arrive in microseconds (µs) from the
// iPhone encoder (ReplayKit CMSampleBuffer). The units are identical —
// no conversion is needed between streams.
//
// The synchronizer uses audio as the primary timing anchor:
//   - On the first audio timestamp, it records:
//       m_anchorPtsUs  = first audio PTS (media time)
//       m_anchorWallUs = wall clock at that moment (QueryPerformanceCounter)
//   - The playback clock is then:
//       PlaybackClock(t) = m_anchorPtsUs + (wallNow(t) - m_anchorWallUs)
//
// Why audio as anchor:
//   - WASAPI plays audio continuously; gaps are immediately audible.
//   - Video frames can be silently dropped without perceptible harm (1-2 frames).
//   - This is the standard approach for media players (FFmpeg, VLC, etc.).
//
// Video scheduling:
//   CheckVideoFrame(framePts) compares framePts against PlaybackClock()
//   and returns a FrameDecision:
//     Render — frame is on time or within tolerance.
//     Drop   — frame is severely stale; discard to prevent latency growth.
//
// Thread-safety:
//   All public methods are thread-safe (protected by m_mutex).
//   NotifyAudioTimestamp() is called from the network thread.
//   CheckVideoFrame() / GetPlaybackClockUs() are called from the render thread.
//
// Lifecycle:
//   Construct → NotifyAudioTimestamp() (sets anchor) →
//   CheckVideoFrame() (many) → Reset() (on disconnect/reconnect)
// ---------------------------------------------------------------------------

// Decision returned by CheckVideoFrame().
enum class FrameDecision {
    Render, // Frame is on-time or within acceptable tolerance — display it.
    Drop,   // Frame is severely stale — discard to prevent latency growth.
};

// Snapshot of synchronization statistics.
struct SyncStats {
    int64_t  avDiffUs       = 0;  // video PTS - audio clock (µs); negative = video behind
    uint64_t droppedFrames  = 0;  // Total video frames dropped as stale
    uint64_t renderedFrames = 0;  // Total video frames rendered
    uint64_t audioNotifies  = 0;  // Total audio timestamp notifications received
    bool     isAnchored     = false; // True once the clock has been established
};

class AVSynchronizer {
public:
    // ---------------------------------------------------------------------------
    // Synchronization thresholds
    //
    // All values are in microseconds. Named constants with documented rationale.
    // ---------------------------------------------------------------------------

    // A video frame more than this many µs AHEAD of the playback clock is
    // suspicious (clock jump or reconnect artifact). Render it anyway to avoid
    // blank screen — the clock will catch up.
    // M13: reduced 500 ms → 200 ms.  The encoder pipeline does not produce
    // 500 ms of pre-roll; a 200 ms early window is a safe generous tolerance
    // while preventing spurious early-frame passthrough.
    static constexpr int64_t SYNC_TOLERANCE_EARLY_US = 200'000; // 200 ms

    // A video frame up to this many µs BEHIND the playback clock is within
    // normal jitter tolerance (≤ 1.5 frames at 30 fps). Render immediately.
    static constexpr int64_t SYNC_TOLERANCE_LATE_US  = 50'000;  // 50 ms

    // A video frame more than this many µs BEHIND the playback clock is
    // stale. Drop it to prevent latency from growing.
    // M13: reduced 300 ms → 150 ms.
    // At 60 fps, 300 ms tolerance allowed up to 18 stale frames to render
    // before dropping, allowing significant latency to accumulate.  150 ms
    // (9 frames at 60 fps, 4.5 frames at 30 fps) is a better compromise:
    // tight enough to evict backlog quickly, wide enough to absorb normal jitter.
    static constexpr int64_t SYNC_DROP_THRESHOLD_US  = 150'000; // 150 ms

    // M13: log sync diagnostics every 150 rendered frames (~2.5 s at 60 fps,
    // ~5 s at 30 fps).  Previously 300 frames gave too infrequent feedback.
    static constexpr uint64_t SYNC_LOG_INTERVAL_FRAMES = 150;

    // ---------------------------------------------------------------------------

    AVSynchronizer();
    ~AVSynchronizer() = default;

    AVSynchronizer(const AVSynchronizer&)            = delete;
    AVSynchronizer& operator=(const AVSynchronizer&) = delete;

    // Notify the synchronizer that a decoded audio packet has arrived.
    // Called from the network/decoder thread on every decoded audio frame.
    // The first call after construction or Reset() sets the playback clock anchor.
    // timestampUs: original media PTS in microseconds from the iPhone encoder.
    void NotifyAudioTimestamp(uint64_t timestampUs);

    // Evaluate whether a decoded video frame should be rendered or dropped.
    // Called from the render thread before UploadNV12Frame().
    // framePts: DecodedFrame::presentationUs (µs from iPhone encoder).
    FrameDecision CheckVideoFrame(uint64_t framePts);

    // Return the current estimated playback clock in microseconds.
    // Returns 0 if the clock has not been anchored yet.
    uint64_t GetPlaybackClockUs() const;

    // Reset all timing state. Call on:
    //   - Network disconnect
    //   - Stream reconnect
    //   - Decoder flush after discontinuity
    void Reset();

    // Returns true once the first audio timestamp has been received.
    bool IsAnchored() const;

    // Snapshot of current sync statistics (for diagnostics/logging).
    SyncStats GetStats() const;

private:
    // Current wall clock in microseconds (from QueryPerformanceCounter).
    static uint64_t WallClockUs();

    // Internal clock query without locking (caller must hold m_mutex).
    uint64_t PlaybackClockUsLocked() const;

    mutable std::mutex m_mutex;

    // Clock anchor (set on first audio notification after construction/Reset).
    bool     m_anchored       = false;
    uint64_t m_anchorPtsUs    = 0;  // Audio media PTS at anchor point
    uint64_t m_anchorWallUs   = 0;  // Wall clock at anchor point

    // Most recent timestamps seen (for A/V diff reporting).
    uint64_t m_lastAudioPtsUs = 0;
    uint64_t m_lastVideoPtsUs = 0;

    // Statistics.
    uint64_t m_droppedFrames  = 0;
    uint64_t m_renderedFrames = 0;
    uint64_t m_audioNotifies  = 0;
};

} // namespace SanskyStream
