#pragma once

// ---------------------------------------------------------------------------
// PipelineStats — M13
//
// Lightweight periodic latency/health diagnostic for the SanskyStream pipeline.
//
// Design:
//   - Zero per-frame overhead: stats are read from existing component members
//     at report time only (every REPORT_INTERVAL_MS milliseconds).
//   - No UI. Log-only output via Logger.
//   - Single Report() call from the render loop (main thread, ~60 Hz).
//     The QPC-based timer ensures we only log once per interval.
//
// What it measures / reports:
//   - Video pipeline: frames decoded, dropped by AVSync, total received
//   - Audio pipeline: ring buffer depth (ms), underflow count, overflow count
//   - AV synchronizer: anchored?, A/V diff (ms), total sync drops
//   - Current render FPS (from Renderer)
//
// Usage (in App::Run):
//   PipelineStatsSnapshot snap;
//   snap.framesDecoded   = m_videoReceiver->GetFramesDecoded();
//   snap.framesDropped   = m_videoReceiver->GetFramesDropped();
//   snap.framesReceived  = m_videoReceiver->GetFramesReceived();
//   snap.audioQueueMs    = m_audioReceiver->GetAudioQueueDepthMs();
//   snap.audioUnderflows = m_audioReceiver->GetUnderflowCount();
//   snap.audioOverflows  = m_audioReceiver->GetOverflowCount();
//   if (m_avSync) {
//       const SyncStats s = m_avSync->GetStats();
//       snap.avAnchored  = s.isAnchored;
//       snap.avDiffUs    = s.avDiffUs;
//       snap.avSyncDrops = s.droppedFrames;
//   }
//   m_pipelineStats->Report(snap);
// ---------------------------------------------------------------------------

#include "AVSynchronizer.h"
#include "Logger.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <windows.h> // QueryPerformanceCounter / QueryPerformanceFrequency

namespace SanskyStream {

// ---------------------------------------------------------------------------
// PipelineStatsSnapshot — caller fills in what it knows; unused fields = 0.
// ---------------------------------------------------------------------------
struct PipelineStatsSnapshot {
    // Video
    uint64_t framesDecoded   = 0; // Total decoded frames since start
    uint64_t framesDropped   = 0; // Total stale frames dropped by AVSync gate
    uint64_t framesReceived  = 0; // Total CompleteFrames handed to decoder

    // Audio
    uint32_t audioQueueMs    = 0; // Current audio ring buffer depth (ms)
    uint64_t audioUnderflows = 0; // WASAPI underflow count
    uint64_t audioOverflows  = 0; // Ring overflow (drop-oldest) count

    // A/V Sync
    bool     avAnchored      = false;
    int64_t  avDiffUs        = 0; // video PTS - audio clock (µs)
    uint64_t avSyncDrops     = 0; // Total AVSynchronizer frame drops

    // Render
    float    renderFps       = 0.0f;
};

// ---------------------------------------------------------------------------
// PipelineStats — periodic logger, no per-frame overhead.
// ---------------------------------------------------------------------------
class PipelineStats {
public:
    // How often to emit a stats log line (milliseconds).
    static constexpr uint32_t REPORT_INTERVAL_MS = 5000; // 5 seconds

    PipelineStats()
    {
        LARGE_INTEGER freq = {};
        QueryPerformanceFrequency(&freq);
        m_freq = static_cast<uint64_t>(freq.QuadPart);

        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        m_lastReportTick = static_cast<uint64_t>(now.QuadPart);
    }

    // Call from the render thread once per frame (60 Hz).
    // Only performs meaningful work every REPORT_INTERVAL_MS ms.
    void Report(const PipelineStatsSnapshot& snap)
    {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        const uint64_t tick      = static_cast<uint64_t>(now.QuadPart);
        const uint64_t elapsedMs =
            (m_freq > 0) ? ((tick - m_lastReportTick) * 1000u) / m_freq : 0u;

        if (elapsedMs < REPORT_INTERVAL_MS) return;
        m_lastReportTick = tick;

        char buf[512];
        snprintf(buf, sizeof(buf),
            "PipelineStats [M13] | "
            "Video: recv=%llu dec=%llu avdrop=%llu | "
            "Audio: q=%u ms uf=%llu of=%llu | "
            "AVSync: %s diff=%+.0f ms syncdrops=%llu | "
            "FPS: %.1f",
            static_cast<unsigned long long>(snap.framesReceived),
            static_cast<unsigned long long>(snap.framesDecoded),
            static_cast<unsigned long long>(snap.framesDropped),
            snap.audioQueueMs,
            static_cast<unsigned long long>(snap.audioUnderflows),
            static_cast<unsigned long long>(snap.audioOverflows),
            snap.avAnchored ? "Synced" : "Unsynced",
            static_cast<double>(snap.avDiffUs) / 1000.0,
            static_cast<unsigned long long>(snap.avSyncDrops),
            static_cast<double>(snap.renderFps));

        LOG_INFO(std::string(buf));
    }

private:
    uint64_t m_freq           = 0;
    uint64_t m_lastReportTick = 0;
};

} // namespace SanskyStream
