#include "AVSynchronizer.h"
#include "Logger.h"

#include <windows.h> // QueryPerformanceCounter, QueryPerformanceFrequency

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AVSynchronizer::AVSynchronizer()
{
    LOG_INFO("AVSynchronizer: Created. Waiting for first audio timestamp.");
}

// ---------------------------------------------------------------------------
// WallClockUs — static helper
//
// Returns current wall time in microseconds using QueryPerformanceCounter.
// This is the same clock already used in Renderer for FPS measurement.
// Precision: typically sub-microsecond on modern Windows.
// ---------------------------------------------------------------------------

// static
uint64_t AVSynchronizer::WallClockUs()
{
    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
    }

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);

    // Convert ticks → microseconds without overflow (intermediate division).
    // freq is typically 10 MHz on modern hardware → ticks/freq ≈ 100 ns resolution.
    return static_cast<uint64_t>(
        (now.QuadPart * 1'000'000ULL) / static_cast<uint64_t>(s_freq.QuadPart));
}

// ---------------------------------------------------------------------------
// PlaybackClockUsLocked — caller must hold m_mutex
// ---------------------------------------------------------------------------

uint64_t AVSynchronizer::PlaybackClockUsLocked() const
{
    if (!m_anchored) return 0;

    const uint64_t wallNow = WallClockUs();
    // Guard against wall clock going backward (unlikely but safe).
    if (wallNow < m_anchorWallUs) return m_anchorPtsUs;

    return m_anchorPtsUs + (wallNow - m_anchorWallUs);
}

// ---------------------------------------------------------------------------
// NotifyAudioTimestamp
//
// Called from the network/decoder thread on each decoded audio packet.
// The FIRST call establishes the playback clock anchor.
// Subsequent calls update m_lastAudioPtsUs (for stats) and silently
// re-anchor if the stream is reset or timestamps jump discontinuously.
// ---------------------------------------------------------------------------

void AVSynchronizer::NotifyAudioTimestamp(uint64_t timestampUs)
{
    if (timestampUs == 0) return; // Ignore zero timestamps

    std::lock_guard<std::mutex> lock(m_mutex);

    ++m_audioNotifies;
    m_lastAudioPtsUs = timestampUs;

    if (!m_anchored) {
        // First audio timestamp — establish the clock anchor.
        m_anchorPtsUs  = timestampUs;
        m_anchorWallUs = WallClockUs();
        m_anchored     = true;

        LOG_INFO("AVSynchronizer: Clock anchored. Audio PTS=" +
                 std::to_string(timestampUs) + " µs, WallClock=" +
                 std::to_string(m_anchorWallUs) + " µs.");
        return;
    }

    // Check for a large timestamp discontinuity (e.g., stream restart).
    // If audio jumps more than 5 seconds from the expected value, re-anchor.
    const uint64_t expectedAudio = PlaybackClockUsLocked();
    const int64_t  delta = static_cast<int64_t>(timestampUs) -
                           static_cast<int64_t>(expectedAudio);

    constexpr int64_t kDiscontinuityThresholdUs = 5'000'000; // 5 seconds

    if (delta > kDiscontinuityThresholdUs || delta < -kDiscontinuityThresholdUs) {
        LOG_WARN("AVSynchronizer: Audio timestamp discontinuity detected. "
                 "Expected ~" + std::to_string(expectedAudio) +
                 " µs, got " + std::to_string(timestampUs) +
                 " µs (delta=" + std::to_string(delta) +
                 " µs). Re-anchoring clock.");
        m_anchorPtsUs  = timestampUs;
        m_anchorWallUs = WallClockUs();
    }
}

// ---------------------------------------------------------------------------
// CheckVideoFrame
//
// Called from the render thread (main thread) before UploadNV12Frame.
// Returns FrameDecision::Render or FrameDecision::Drop.
// ---------------------------------------------------------------------------

FrameDecision AVSynchronizer::CheckVideoFrame(uint64_t framePts)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_lastVideoPtsUs = framePts;

    // If the clock is not anchored yet, render all frames unconditionally.
    // This prevents a blank screen during the brief startup window before
    // the first audio packet arrives.
    if (!m_anchored) {
        ++m_renderedFrames;
        return FrameDecision::Render;
    }

    const uint64_t clockNow = PlaybackClockUsLocked();
    // Positive = video is AHEAD of the clock (early).
    // Negative = video is BEHIND the clock (late).
    const int64_t diff = static_cast<int64_t>(framePts) -
                         static_cast<int64_t>(clockNow);

    // Periodic diagnostic log (every SYNC_LOG_INTERVAL_FRAMES rendered frames).
    if (m_renderedFrames % SYNC_LOG_INTERVAL_FRAMES == 0 && m_renderedFrames > 0) {
        LOG_INFO("AVSynchronizer: Video=" + std::to_string(framePts) +
                 " µs, Clock=" + std::to_string(clockNow) +
                 " µs, Diff=" + std::to_string(diff) +
                 " µs, Dropped=" + std::to_string(m_droppedFrames) +
                 ", Rendered=" + std::to_string(m_renderedFrames) + ".");
    }

    // Frame is severely stale — drop to prevent latency growth.
    if (diff < -SYNC_DROP_THRESHOLD_US) {
        ++m_droppedFrames;
        // Log only occasionally to avoid log spam when many frames are stale.
        if (m_droppedFrames % 30 == 1) {
            LOG_WARN("AVSynchronizer: Stale video frame dropped. "
                     "PTS=" + std::to_string(framePts) +
                     " µs, Clock=" + std::to_string(clockNow) +
                     " µs, Behind=" + std::to_string(-diff) + " µs, "
                     "TotalDropped=" + std::to_string(m_droppedFrames) + ".");
        }
        return FrameDecision::Drop;
    }

    // Frame is within the render window (on-time, slightly late, or early).
    ++m_renderedFrames;
    return FrameDecision::Render;
}

// ---------------------------------------------------------------------------
// GetPlaybackClockUs
// ---------------------------------------------------------------------------

uint64_t AVSynchronizer::GetPlaybackClockUs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return PlaybackClockUsLocked();
}

// ---------------------------------------------------------------------------
// Reset
//
// Clears all timing state. The next audio notification will re-anchor.
// ---------------------------------------------------------------------------

void AVSynchronizer::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const bool wasAnchored = m_anchored;

    m_anchored       = false;
    m_anchorPtsUs    = 0;
    m_anchorWallUs   = 0;
    m_lastAudioPtsUs = 0;
    m_lastVideoPtsUs = 0;
    m_droppedFrames  = 0;
    m_renderedFrames = 0;
    m_audioNotifies  = 0;

    if (wasAnchored) {
        LOG_INFO("AVSynchronizer: Reset. Clock anchor cleared.");
    }
}

// ---------------------------------------------------------------------------
// IsAnchored
// ---------------------------------------------------------------------------

bool AVSynchronizer::IsAnchored() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_anchored;
}

// ---------------------------------------------------------------------------
// GetStats
// ---------------------------------------------------------------------------

SyncStats AVSynchronizer::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    SyncStats s;
    s.isAnchored     = m_anchored;
    s.droppedFrames  = m_droppedFrames;
    s.renderedFrames = m_renderedFrames;
    s.audioNotifies  = m_audioNotifies;

    if (m_anchored && m_lastVideoPtsUs > 0 && m_lastAudioPtsUs > 0) {
        s.avDiffUs = static_cast<int64_t>(m_lastVideoPtsUs) -
                     static_cast<int64_t>(m_lastAudioPtsUs);
    }

    return s;
}

} // namespace SanskyStream
