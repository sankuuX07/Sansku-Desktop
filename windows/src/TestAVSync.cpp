// ---------------------------------------------------------------------------
// TestAVSync.cpp
//
// Standalone console test harness for M12 AVSynchronizer.
// NOT part of the SanskyStream_Windows production build.
//
// Tests (12 scenarios):
//   1.  Unanchored clock returns 0 from GetPlaybackClockUs().
//   2.  First audio timestamp anchors the clock (IsAnchored() == true).
//   3.  Second audio call does not re-anchor (clock stays stable).
//   4.  Video with matching PTS (within 50ms) → FrameDecision::Render.
//   5.  Video slightly early (< 500ms ahead) → FrameDecision::Render.
//   6.  Video severely stale (> 300ms behind) → FrameDecision::Drop.
//   7.  Video frame before anchor (unanchored clock) → FrameDecision::Render.
//   8.  Timestamp discontinuity (> 5s jump) triggers re-anchor log safely.
//   9.  Reset() clears the anchor (IsAnchored() == false).
//  10.  After Reset(), next audio call re-anchors.
//  11.  GetStats() reports correct dropped/rendered frame counts.
//  12.  Zero timestamp is ignored by NotifyAudioTimestamp().
//
// Note: Wall-clock timing is not tested (cannot be deterministic in a unit
// test).  We inject timestamps that exercise each FrameDecision branch.
// ---------------------------------------------------------------------------

#include "AVSynchronizer.h"
#include "Logger.h"

#include <cstdio>
#include <windows.h>   // Sleep()

using namespace SanskyStream;

// ---------------------------------------------------------------------------
// Simple pass/fail tracking
// ---------------------------------------------------------------------------
static int g_passed = 0;
static int g_failed = 0;

static void Check(bool cond, const char* desc) {
    if (cond) {
        printf("  [PASS] %s\n", desc);
        ++g_passed;
    } else {
        printf("  [FAIL] %s\n", desc);
        ++g_failed;
    }
}

// ---------------------------------------------------------------------------
// Test 1: Unanchored clock returns 0
// ---------------------------------------------------------------------------
static void TestUnanchoredClockIsZero()
{
    printf("\n--- Test 1: Unanchored clock returns 0 ---\n");
    AVSynchronizer sync;
    Check(!sync.IsAnchored(),         "IsAnchored() == false before first audio");
    Check(sync.GetPlaybackClockUs() == 0, "GetPlaybackClockUs() == 0 when unanchored");
}

// ---------------------------------------------------------------------------
// Test 2: First audio anchors the clock
// ---------------------------------------------------------------------------
static void TestFirstAudioAnchors()
{
    printf("\n--- Test 2: First audio anchors the clock ---\n");
    AVSynchronizer sync;
    sync.NotifyAudioTimestamp(1'000'000); // 1 second
    Check(sync.IsAnchored(), "IsAnchored() == true after first audio");

    const uint64_t clock = sync.GetPlaybackClockUs();
    Check(clock > 0, "GetPlaybackClockUs() > 0 after anchor");
}

// ---------------------------------------------------------------------------
// Test 3: Second audio does not crash; clock stays reasonable
// ---------------------------------------------------------------------------
static void TestSecondAudioSafe()
{
    printf("\n--- Test 3: Second audio timestamp is safe ---\n");
    bool threw = false;
    try {
        AVSynchronizer sync;
        sync.NotifyAudioTimestamp(1'000'000);
        Sleep(5); // let wall clock tick a tiny bit
        sync.NotifyAudioTimestamp(1'020'000); // +20ms (normal audio packet)
        Check(sync.IsAnchored(), "Still anchored after second audio");
    } catch (...) { threw = true; }
    Check(!threw, "No exception from second NotifyAudioTimestamp()");
}

// ---------------------------------------------------------------------------
// Test 4: On-time video frame → Render
//
// Strategy: anchor at T=1s, then immediately check a video frame at the same
// PTS. Wall clock hasn't advanced meaningfully, so video ≈ on time.
// ---------------------------------------------------------------------------
static void TestOnTimeVideoRenders()
{
    printf("\n--- Test 4: On-time video → Render ---\n");

    AVSynchronizer sync;
    constexpr uint64_t kAudioPts = 5'000'000; // 5 s
    sync.NotifyAudioTimestamp(kAudioPts);

    // Video at exactly the same PTS → diff ≈ 0 (minus tiny wall-clock advance).
    // SYNC_TOLERANCE_LATE_US = 50ms, so this must render.
    const FrameDecision d = sync.CheckVideoFrame(kAudioPts);
    Check(d == FrameDecision::Render, "Video at anchor PTS → Render");
}

// ---------------------------------------------------------------------------
// Test 5: Slightly early video → Render
//
// Video 200ms AHEAD of the clock. SYNC_TOLERANCE_EARLY_US = 500ms → Render.
// ---------------------------------------------------------------------------
static void TestSlightlyEarlyVideoRenders()
{
    printf("\n--- Test 5: Slightly early video → Render ---\n");

    AVSynchronizer sync;
    constexpr uint64_t kAudioPts = 10'000'000; // 10 s
    sync.NotifyAudioTimestamp(kAudioPts);

    // Video 200ms ahead.  Should still render (within 500ms tolerance).
    const FrameDecision d = sync.CheckVideoFrame(kAudioPts + 200'000);
    Check(d == FrameDecision::Render, "Video 200ms early → Render");
}

// ---------------------------------------------------------------------------
// Test 6: Severely stale video → Drop
//
// Video 400ms BEHIND the clock. SYNC_DROP_THRESHOLD_US = 300ms → Drop.
// ---------------------------------------------------------------------------
static void TestStaleVideoDropped()
{
    printf("\n--- Test 6: Severely stale video → Drop ---\n");

    AVSynchronizer sync;
    // Anchor at T=10s.
    constexpr uint64_t kAudioPts = 10'000'000;
    sync.NotifyAudioTimestamp(kAudioPts);

    // Simulate wall clock advancing 400ms by giving video a PTS 400ms BEHIND
    // the anchor PTS.  From the synchronizer's perspective, the clock has not
    // advanced (we just anchored), but the video PTS is 400ms less than the
    // anchor PTS → diff = -400ms → behind SYNC_DROP_THRESHOLD_US (300ms).
    const FrameDecision d = sync.CheckVideoFrame(kAudioPts - 400'000);
    Check(d == FrameDecision::Drop, "Video 400ms stale → Drop");

    const SyncStats s = sync.GetStats();
    Check(s.droppedFrames == 1, "droppedFrames == 1 after stale drop");
}

// ---------------------------------------------------------------------------
// Test 7: Video before clock is anchored → Render (no blank screen policy)
// ---------------------------------------------------------------------------
static void TestVideoBeforeAnchorRenders()
{
    printf("\n--- Test 7: Video before anchor → Render ---\n");

    AVSynchronizer sync;
    // Do NOT call NotifyAudioTimestamp — clock is unanchored.
    const FrameDecision d = sync.CheckVideoFrame(9'000'000);
    Check(d == FrameDecision::Render, "Video before anchor → Render (no blank screen)");

    const SyncStats s = sync.GetStats();
    Check(s.renderedFrames == 1, "renderedFrames == 1");
    Check(!s.isAnchored,         "isAnchored == false");
}

// ---------------------------------------------------------------------------
// Test 8: Timestamp discontinuity (> 5 s jump) is handled safely
// ---------------------------------------------------------------------------
static void TestDiscontinuityHandledSafely()
{
    printf("\n--- Test 8: Timestamp discontinuity ---\n");
    bool threw = false;
    try {
        AVSynchronizer sync;
        sync.NotifyAudioTimestamp(1'000'000);            // anchor at 1s
        sync.NotifyAudioTimestamp(10'000'000'000ULL);    // jump to 10000s
        Check(sync.IsAnchored(), "Still anchored after discontinuity");
    } catch (...) { threw = true; }
    Check(!threw, "No exception on timestamp discontinuity");
}

// ---------------------------------------------------------------------------
// Test 9: Reset() clears the anchor
// ---------------------------------------------------------------------------
static void TestResetClearsAnchor()
{
    printf("\n--- Test 9: Reset() clears anchor ---\n");

    AVSynchronizer sync;
    sync.NotifyAudioTimestamp(1'000'000);
    Check(sync.IsAnchored(), "Anchored before Reset()");

    sync.Reset();
    Check(!sync.IsAnchored(),         "IsAnchored() == false after Reset()");
    Check(sync.GetPlaybackClockUs() == 0, "GetPlaybackClockUs() == 0 after Reset()");
}

// ---------------------------------------------------------------------------
// Test 10: After Reset(), next audio re-anchors
// ---------------------------------------------------------------------------
static void TestReanchorAfterReset()
{
    printf("\n--- Test 10: Re-anchor after Reset() ---\n");

    AVSynchronizer sync;
    sync.NotifyAudioTimestamp(1'000'000);
    sync.Reset();
    sync.NotifyAudioTimestamp(5'000'000);  // new stream begins

    Check(sync.IsAnchored(), "Re-anchored after Reset() + new audio");
    Check(sync.GetPlaybackClockUs() > 0, "Clock > 0 after re-anchor");
}

// ---------------------------------------------------------------------------
// Test 11: GetStats() reflects correct counts
// ---------------------------------------------------------------------------
static void TestStatsAccuracy()
{
    printf("\n--- Test 11: GetStats() accuracy ---\n");

    AVSynchronizer sync;
    constexpr uint64_t kAudioPts = 20'000'000; // 20 s
    sync.NotifyAudioTimestamp(kAudioPts);

    // Render 2 on-time frames.
    sync.CheckVideoFrame(kAudioPts);
    sync.CheckVideoFrame(kAudioPts);
    // Drop 1 stale frame.
    sync.CheckVideoFrame(kAudioPts - 500'000);

    const SyncStats s = sync.GetStats();
    Check(s.isAnchored,          "isAnchored == true");
    Check(s.renderedFrames == 2, "renderedFrames == 2");
    Check(s.droppedFrames  == 1, "droppedFrames == 1");
    Check(s.audioNotifies  == 1, "audioNotifies == 1");
}

// ---------------------------------------------------------------------------
// Test 12: Zero timestamp is ignored
// ---------------------------------------------------------------------------
static void TestZeroTimestampIgnored()
{
    printf("\n--- Test 12: Zero timestamp ignored ---\n");

    AVSynchronizer sync;
    sync.NotifyAudioTimestamp(0);  // must be a no-op
    Check(!sync.IsAnchored(), "IsAnchored() == false after zero timestamp");

    sync.NotifyAudioTimestamp(1'000'000);  // real timestamp
    Check(sync.IsAnchored(), "IsAnchored() == true after real timestamp");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    printf("============================================================\n");
    printf("  SanskyStream M12 — AVSynchronizer Test Suite\n");
    printf("============================================================\n");

    TestUnanchoredClockIsZero();
    TestFirstAudioAnchors();
    TestSecondAudioSafe();
    TestOnTimeVideoRenders();
    TestSlightlyEarlyVideoRenders();
    TestStaleVideoDropped();
    TestVideoBeforeAnchorRenders();
    TestDiscontinuityHandledSafely();
    TestResetClearsAnchor();
    TestReanchorAfterReset();
    TestStatsAccuracy();
    TestZeroTimestampIgnored();

    printf("\n============================================================\n");
    printf("  Results: %d passed, %d failed\n", g_passed, g_failed);
    printf("============================================================\n");
    printf("\nNOTE: Real iPhone A/V synchronization testing requires the full\n");
    printf("      M10 iOS pipeline and is NOT possible without Mac/Xcode.\n");
    printf("      SOURCE IMPLEMENTATION COMPLETE\n");
    printf("      REAL IPHONE END-TO-END TEST PENDING\n\n");

    return (g_failed > 0) ? 1 : 0;
}
