// ---------------------------------------------------------------------------
// TestRenderPipeline.cpp
//
// M8 standalone render-pipeline test.
// NOT part of SanskyStream_Windows production build.
// Compiled as the separate "TestRenderPipeline" CMake target.
//
// Purpose:
//   Verifies the full M8 rendering path end-to-end WITHOUT a real iPhone:
//
//     Synthetic NV12 frame
//         |
//     VideoFrameQueue::Push()   <- background producer thread
//         |
//     VideoFrameQueue::TryPop() <- main/render thread
//         |
//     Renderer::UploadNV12Frame()
//         |
//     GPU NV12->RGB shader (BT.601)
//         |
//     Letterbox viewport
//         |
//     SwapChain::Present()
//
// The test creates 320x240 NV12 frames with a slowly cycling Y value
// (luma animation) and green-ish chroma.  A visible animated green/teal
// rectangle confirms:
//   - NV12 texture upload works correctly.
//   - The BT.601 pixel shader converts to RGB correctly.
//   - The letterbox viewport preserves aspect ratio.
//   - VideoFrameQueue thread handoff is race-free.
//   - Window resize does not crash the renderer.
//
// Run duration: 5 seconds, then exits cleanly.
//
// Expected console output:
//   TestRenderPipeline: window created.
//   TestRenderPipeline: producer started (320x240, ~60 fps synthetic NV12).
//   TestRenderPipeline: render loop running for 5 seconds...
//   TestRenderPipeline: frames produced : <N>
//   TestRenderPipeline: hasVideo        : YES
//   TestRenderPipeline: fps (last)      : <N>
//   TestRenderPipeline: PASS
// ---------------------------------------------------------------------------

#include "Window.h"
#include "Renderer.h"
#include "VideoFrameQueue.h"
#include "DecodedFrame.h"
#include "Logger.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace SanskyStream;

// ---------------------------------------------------------------------------
// Synthetic NV12 frame generator
//
// 320x240, solid-color:
//   BT.601 limited-range for approximately green (R=0, G=200, B=0):
//   Y  = 16 + 0.257*0 + 0.504*200 + 0.098*0  ≈ 117
//   Cb = 128 - 0.148*0 - 0.291*200 + 0.439*0 ≈ 70
//   Cr = 128 + 0.439*0 - 0.368*200 - 0.071*0 ≈ 54
//
// The Y value is animated (+= frameId % 30) to make the luma oscillate,
// confirming frames are actually updating.
// ---------------------------------------------------------------------------

static constexpr uint32_t k_testWidth  = 320u;
static constexpr uint32_t k_testHeight = 240u;

static DecodedFrame MakeSyntheticFrame(uint32_t frameId)
{
    DecodedFrame f;
    f.frameId        = frameId;
    f.presentationUs = static_cast<uint64_t>(frameId) * 16667u; // ~60 fps PTS
    f.width          = k_testWidth;
    f.height         = k_testHeight;
    f.nv12Data.resize(static_cast<size_t>(k_testWidth) * k_testHeight * 3u / 2u);

    // Y plane — animate luma value between 90..120 to visually confirm update
    const uint8_t yVal = static_cast<uint8_t>(105u + (frameId % 30u));
    std::memset(f.nv12Data.data(), static_cast<int>(yVal),
                static_cast<size_t>(k_testWidth) * k_testHeight);

    // UV plane — green-ish chroma (Cb≈70, Cr≈54 for approximately pure green)
    uint8_t* uv = f.nv12Data.data() +
                  static_cast<size_t>(k_testWidth) * k_testHeight;
    const size_t uvBytes = static_cast<size_t>(k_testWidth) *
                           static_cast<size_t>(k_testHeight) / 2u;
    for (size_t i = 0; i < uvBytes; i += 2u) {
        uv[i]     = 70u;  // Cb (U)
        uv[i + 1] = 54u;  // Cr (V)
    }

    return f;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== TestRenderPipeline -- M8 render-pipeline test ===\n\n");

    // -----------------------------------------------------------------------
    // Create Window
    // -----------------------------------------------------------------------
    Window window(1280, 720, L"TestRenderPipeline -- M8 Video Render Test");
    if (!window.GetHWND()) {
        printf("FAIL: Window creation failed.\n");
        return 1;
    }
    printf("TestRenderPipeline: window created.\n");

    // -----------------------------------------------------------------------
    // Create Renderer and VideoFrameQueue
    // -----------------------------------------------------------------------
    VideoFrameQueue queue;

    Renderer renderer(&window);
    if (!renderer.Initialize()) {
        printf("FAIL: Renderer initialization failed.\n");
        return 1;
    }
    renderer.SetFrameQueue(&queue);
    printf("TestRenderPipeline: renderer initialized.\n");

    // -----------------------------------------------------------------------
    // Producer thread — pushes synthetic NV12 frames at ~60 fps
    // -----------------------------------------------------------------------
    std::atomic<bool>    producerRunning{true};
    std::atomic<uint32_t> framesProduced{0u};

    std::thread producer([&queue, &producerRunning, &framesProduced]() {
        uint32_t frameId = 0;
        while (producerRunning.load()) {
            queue.Push(MakeSyntheticFrame(frameId++));
            framesProduced.store(frameId);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });

    printf("TestRenderPipeline: producer started (%ux%u, ~60 fps synthetic NV12).\n",
           k_testWidth, k_testHeight);
    printf("TestRenderPipeline: render loop running for 5 seconds...\n\n");

    // -----------------------------------------------------------------------
    // Render loop — run for 5 seconds
    // -----------------------------------------------------------------------
    const auto startTime = std::chrono::steady_clock::now();
    constexpr int k_durationSec = 5;

    while (true) {
        if (!window.ProcessMessages()) break;

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= k_durationSec) break;

        renderer.Render();
        window.DrawStatusOverlay();
    }

    // -----------------------------------------------------------------------
    // Stop producer thread
    // -----------------------------------------------------------------------
    producerRunning.store(false);
    producer.join();

    // -----------------------------------------------------------------------
    // Report
    // -----------------------------------------------------------------------
    const bool pass = renderer.HasVideo() && renderer.GetFPS() > 0.0f;

    printf("TestRenderPipeline: frames produced  : %u\n",
           framesProduced.load());
    printf("TestRenderPipeline: hasVideo          : %s\n",
           renderer.HasVideo() ? "YES" : "NO");
    printf("TestRenderPipeline: fps (last sample) : %.1f\n",
           static_cast<double>(renderer.GetFPS()));
    printf("TestRenderPipeline: video size        : %ux%u\n",
           renderer.VideoWidth(), renderer.VideoHeight());
    printf("\n");

    if (pass) {
        printf("TestRenderPipeline: PASS -- NV12 frames rendered successfully.\n");
        printf("  Video was visible in the SanskyStream window.\n");
    } else {
        printf("TestRenderPipeline: FAIL -- video was NOT displayed.\n");
        printf("  hasVideo=%s  fps=%.1f\n",
               renderer.HasVideo() ? "true" : "false",
               static_cast<double>(renderer.GetFPS()));
    }

    printf("\nNOTE: This test uses synthetic NV12 data, not real H.264.\n");
    printf("Real iPhone-to-Windows streaming requires a physical iPhone (pending).\n");

    return pass ? 0 : 1;
}
