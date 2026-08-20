// ---------------------------------------------------------------------------
// TestAudioPipeline.cpp
//
// Standalone console test harness for M11 audio pipeline.
// NOT part of the SanskyStream_Windows production build.
//
// Tests:
//   1. AACDecoder initialization succeeds.
//   2. Synthetic minimal AAC packet is submitted without crash.
//   3. Malformed/empty packets are rejected without crash.
//   4. AudioPlayer initializes with the default device.
//   5. AudioPlayer accepts PCM data without crash.
//   6. AudioPlayer underflow produces silence without crash.
//   7. AudioPlayer and AACDecoder shut down cleanly.
//   8. AudioReceiver full pipeline: Start() → packet → Stop().
//
// Note: We cannot generate a valid decodable AAC frame without an iOS
// encoder, so PCM output cannot be verified from synthetic data.
// The decoder will safely return MF_E_TRANSFORM_NEED_MORE_INPUT for
// random bytes — this is expected and tested as a safe failure.
// ---------------------------------------------------------------------------

#include "AACDecoder.h"
#include "AudioPlayer.h"
#include "AudioReceiver.h"
#include "Logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace SanskyStream;

// Simple pass/fail tracking.
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
// Test 1: AACDecoder initialization
// ---------------------------------------------------------------------------
static void TestAACDecoderInit() {
    printf("\n--- Test 1: AACDecoder initialization ---\n");

    bool decodedCalled = false;
    AACDecoder dec([&](DecodedAudioPacket) { decodedCalled = true; });

    bool ok = dec.Initialize(44100, 2);
    Check(ok, "AACDecoder::Initialize(44100, 2) returns true");
    Check(dec.IsInitialized(), "AACDecoder::IsInitialized() == true after init");
    Check(dec.GetSampleRate()   == 44100, "GetSampleRate() == 44100");
    Check(dec.GetChannelCount() == 2,     "GetChannelCount() == 2");
}

// ---------------------------------------------------------------------------
// Test 2: AACDecoder rejects empty/null input safely
// ---------------------------------------------------------------------------
static void TestAACDecoderRejectsInvalid() {
    printf("\n--- Test 2: AACDecoder rejects invalid input ---\n");

    AACDecoder dec([](DecodedAudioPacket) {});
    dec.Initialize(44100, 2);

    // Null pointer.
    bool r1 = dec.SubmitPacket(nullptr, 10, 0);
    Check(!r1, "SubmitPacket(nullptr) returns false");

    // Zero size.
    uint8_t dummy[4] = {0, 1, 2, 3};
    bool r2 = dec.SubmitPacket(dummy, 0, 0);
    Check(!r2, "SubmitPacket(size=0) returns false");
}

// ---------------------------------------------------------------------------
// Test 3: AACDecoder handles random/garbage bytes safely
// ---------------------------------------------------------------------------
static void TestAACDecoderGarbage() {
    printf("\n--- Test 3: AACDecoder handles garbage bytes safely ---\n");

    bool threw = false;
    try {
        AACDecoder dec([](DecodedAudioPacket) {});
        dec.Initialize(44100, 2);

        // Submit random garbage — MFT will return an error internally.
        // The decoder must not crash; it should log a warning and return false.
        uint8_t garbage[64];
        std::memset(garbage, 0xFF, sizeof(garbage));
        // Return value may be true (MFT accepted) or false (MFT rejected).
        // Either way, no crash is the requirement.
        dec.SubmitPacket(garbage, sizeof(garbage), 12345);
    } catch (...) {
        threw = true;
    }

    Check(!threw, "Garbage AAC data does not throw an exception");
}

// ---------------------------------------------------------------------------
// Test 4: AACDecoder flush before packets
// ---------------------------------------------------------------------------
static void TestAACDecoderFlushEmpty() {
    printf("\n--- Test 4: AACDecoder flush on empty pipeline ---\n");

    bool threw = false;
    try {
        AACDecoder dec([](DecodedAudioPacket) {});
        dec.Initialize(44100, 2);
        dec.Flush(); // Must not crash on empty pipeline.
    } catch (...) {
        threw = true;
    }
    Check(!threw, "AACDecoder::Flush() on empty pipeline does not crash");
}

// ---------------------------------------------------------------------------
// Test 5: AudioPlayer initialization
// ---------------------------------------------------------------------------
static void TestAudioPlayerInit() {
    printf("\n--- Test 5: AudioPlayer initialization ---\n");

    AudioPlayer player;
    bool ok = player.Initialize(44100, 2, 16);
    Check(ok, "AudioPlayer::Initialize(44100, 2, 16) returns true");
    Check(player.IsInitialized(), "AudioPlayer::IsInitialized() == true");

    // Start playback.
    bool started = player.Start();
    Check(started, "AudioPlayer::Start() returns true");
    Check(player.IsPlaying(), "AudioPlayer::IsPlaying() == true after Start()");

    player.Stop();
    Check(!player.IsPlaying(), "AudioPlayer::IsPlaying() == false after Stop()");
}

// ---------------------------------------------------------------------------
// Test 6: AudioPlayer accepts PCM data without crash
// ---------------------------------------------------------------------------
static void TestAudioPlayerSubmitPCM() {
    printf("\n--- Test 6: AudioPlayer PCM submission ---\n");

    AudioPlayer player;
    player.Initialize(44100, 2, 16);
    player.Start();

    // Generate 100 ms of silence (44100 * 2 ch * 2 bytes * 0.1 = 17640 bytes).
    const size_t pcmBytes = 44100 * 2 * 2 / 10;
    std::vector<uint8_t> silence(pcmBytes, 0);

    size_t written = player.SubmitPCM(silence.data(), silence.size());
    Check(written == pcmBytes, "SubmitPCM(100ms silence) accepts all bytes");

    // Let it play for a brief moment to exercise the playback thread.
    Sleep(200);

    player.Stop();
    Check(!player.IsPlaying(), "AudioPlayer stops cleanly after PCM submission");
}

// ---------------------------------------------------------------------------
// Test 7: AudioPlayer underflow (no PCM submitted, no crash)
// ---------------------------------------------------------------------------
static void TestAudioPlayerUnderflow() {
    printf("\n--- Test 7: AudioPlayer underflow handling ---\n");

    bool threw = false;
    try {
        AudioPlayer player;
        player.Initialize(44100, 2, 16);
        player.Start();

        // Submit nothing — the playback thread must fill with silence.
        Sleep(150);

        player.Stop();
    } catch (...) {
        threw = true;
    }

    Check(!threw, "AudioPlayer underflow (no PCM) does not crash");
}

// ---------------------------------------------------------------------------
// Test 8: AudioPlayer overflow (submit more than ring capacity)
// ---------------------------------------------------------------------------
static void TestAudioPlayerOverflow() {
    printf("\n--- Test 8: AudioPlayer overflow handling ---\n");

    bool threw = false;
    try {
        AudioPlayer player;
        player.Initialize(44100, 2, 16);
        // Do NOT start() — just test ring buffer overflow handling.
        // SubmitPCM is valid even before Start().
        player.Initialize(44100, 2, 16); // second call is a no-op

        // Submit 200ms of data (ring is ~100ms, so this must overflow safely).
        const size_t bigBuf = 44100 * 2 * 2 / 5; // 200 ms
        std::vector<uint8_t> data(bigBuf, 0xAA);
        player.SubmitPCM(data.data(), data.size());
    } catch (...) {
        threw = true;
    }

    Check(!threw, "AudioPlayer ring-buffer overflow does not crash");
}

// ---------------------------------------------------------------------------
// Test 9: AudioReceiver full pipeline Start/Stop
// ---------------------------------------------------------------------------
static void TestAudioReceiverLifecycle() {
    printf("\n--- Test 9: AudioReceiver lifecycle ---\n");

    AudioReceiver recv;
    bool started = recv.Start(44100, 2);
    Check(started, "AudioReceiver::Start(44100, 2) returns true");
    Check(recv.IsRunning(), "AudioReceiver::IsRunning() == true");

    recv.Stop();
    Check(!recv.IsRunning(), "AudioReceiver::IsRunning() == false after Stop()");
}

// ---------------------------------------------------------------------------
// Test 10: AudioReceiver handles malformed packets safely
// ---------------------------------------------------------------------------
static void TestAudioReceiverBadPackets() {
    printf("\n--- Test 10: AudioReceiver malformed packet handling ---\n");

    AudioReceiver recv;
    recv.Start(44100, 2);

    bool threw = false;
    try {
        // Too short (less than AudioPayloadHeader).
        uint8_t tinyBuf[4] = {0, 0, 0, 0};
        recv.OnAudioPacketReceived(tinyBuf, sizeof(tinyBuf));

        // Null pointer.
        recv.OnAudioPacketReceived(nullptr, 100);

        // Zero size.
        recv.OnAudioPacketReceived(tinyBuf, 0);

        // Garbage with valid header size but random AAC data.
        uint8_t fakePkt[32];
        std::memset(fakePkt, 0x00, 8); // timestamp = 0
        std::memset(fakePkt + 8, 0xFF, 24); // garbage AAC
        recv.OnAudioPacketReceived(fakePkt, sizeof(fakePkt));
    } catch (...) {
        threw = true;
    }

    Check(!threw, "AudioReceiver handles all malformed packets without exception");

    recv.Stop();
    Check(!recv.IsRunning(), "AudioReceiver stops cleanly after malformed packets");
}

// ---------------------------------------------------------------------------
// Test 11: Double stop is safe
// ---------------------------------------------------------------------------
static void TestDoubleStop() {
    printf("\n--- Test 11: Double Stop() safety ---\n");

    bool threw = false;
    try {
        AudioReceiver recv;
        recv.Start(44100, 2);
        recv.Stop();
        recv.Stop(); // Must be a no-op.
    } catch (...) {
        threw = true;
    }
    Check(!threw, "AudioReceiver::Stop() called twice does not crash");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    printf("============================================================\n");
    printf("  SanskyStream M11 — Audio Pipeline Test Suite\n");
    printf("============================================================\n");

    // Suppress debug output flooding the test console.
    // Logger still writes to the debugger output stream.

    TestAACDecoderInit();
    TestAACDecoderRejectsInvalid();
    TestAACDecoderGarbage();
    TestAACDecoderFlushEmpty();
    TestAudioPlayerInit();
    TestAudioPlayerSubmitPCM();
    TestAudioPlayerUnderflow();
    TestAudioPlayerOverflow();
    TestAudioReceiverLifecycle();
    TestAudioReceiverBadPackets();
    TestDoubleStop();

    printf("\n============================================================\n");
    printf("  Results: %d passed, %d failed\n", g_passed, g_failed);
    printf("============================================================\n");
    printf("\nNOTE: Real iPhone audio testing requires the M10 iOS encoder\n");
    printf("      to be implemented. That is a separate milestone.\n");
    printf("      These tests verify Windows-side pipeline correctness only.\n\n");

    return (g_failed > 0) ? 1 : 0;
}
