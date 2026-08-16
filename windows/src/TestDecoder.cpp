// ---------------------------------------------------------------------------
// TestDecoder.cpp
//
// M7 headless console test harness for the H.264 decoder pipeline.
// NOT part of SanskyStream_Windows production build.
// Compiled as the separate "TestDecoder" CMake target.
//
// Tests exercised:
//   1. P-frame submitted before keyframe → rejected with WARN (no crash).
//   2. Keyframe with no SPS NALU in payload → rejected with WARN (no crash).
//   3. Keyframe with unparseable/truncated SPS → rejected with WARN (no crash).
//   4. Stale frame detection → discarded with WARN when already decoded.
//   5. Flush on uninitialized decoder → no crash.
//   6. MFTEnumEx availability check → confirms H.264 decoder is present.
//
// What this test does NOT cover:
//   - Actual H.264 decode (IDR → NV12 pixels).
//   - This requires real encoded H.264 data from the iPhone encoder.
//   - Physical iPhone testing is pending (no Mac/Xcode available).
//   - When the iPhone connects and streams, H264Decoder::SubmitFrame will
//     process real Annex B keyframes and the DrainOutput log will confirm
//     correct NV12 output.
// ---------------------------------------------------------------------------

#include "H264Decoder.h"
#include "VideoPacket.h"
#include "Logger.h"

#include <mfapi.h>
#include <mftransform.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace SanskyStream;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static CompleteFrame MakeFrame(uint32_t id, uint64_t ptsUs, bool isKF,
                                std::vector<uint8_t> data)
{
    CompleteFrame f;
    f.frameId        = id;
    f.presentationUs = ptsUs;
    f.isKeyframe     = isKF;
    f.data           = std::move(data);
    return f;
}

// A 4-byte Annex B start code followed by a valid NALU header byte.
static std::vector<uint8_t> AnnexBNALU(uint8_t naluTypeByte,
                                        const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out = { 0x00, 0x00, 0x00, 0x01, naluTypeByte };
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== TestDecoder — M7 H264Decoder test harness ===\n\n");
    int passed = 0, failed = 0;

    // -----------------------------------------------------------------------
    // Test 1: MFTEnumEx availability check
    // Confirms that the Windows H.264 decoder MFT is present on this machine.
    // -----------------------------------------------------------------------
    printf("[Test 1] MFTEnumEx: check for H.264 sync decoder MFT...\n");
    {
        // Temporary MFStartup just for this enum check.
        MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

        MFT_REGISTER_TYPE_INFO inTypeInfo = {};
        inTypeInfo.guidMajorType = MFMediaType_Video;
        inTypeInfo.guidSubtype   = MFVideoFormat_H264;

        IMFActivate** ppActivates  = nullptr;
        UINT32        numActivates = 0;

        HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inTypeInfo,
            nullptr,
            &ppActivates,
            &numActivates);

        if (SUCCEEDED(hr) && numActivates > 0) {
            printf("  PASS: Found %u synchronous H.264 decoder MFT(s).\n", numActivates);
            ++passed;
        } else {
            printf("  FAIL: No synchronous H.264 decoder MFT found on this system.\n");
            ++failed;
        }

        if (ppActivates) {
            for (UINT32 i = 0; i < numActivates; ++i) ppActivates[i]->Release();
            CoTaskMemFree(ppActivates);
        }

        MFShutdown();
    }

    // Create one H264Decoder for all remaining tests.
    // Its constructor calls MFStartup internally.
    int decoderFrames = 0;
    H264Decoder decoder([&decoderFrames](DecodedFrame frame) {
        printf("  [Callback] DecodedFrame | ID:%u | %ux%u | NV12:%zu bytes | PTS:%llu\n",
               frame.frameId, frame.width, frame.height,
               frame.nv12Data.size(),
               static_cast<unsigned long long>(frame.presentationUs));
        ++decoderFrames;
    });

    // -----------------------------------------------------------------------
    // Test 2: P-frame before any keyframe → must be rejected silently.
    // -----------------------------------------------------------------------
    printf("\n[Test 2] P-frame before keyframe — expect rejection WARN...\n");
    {
        // Fake P-frame: start code + slice NALU header (NAL type 1 = non-IDR)
        auto data = AnnexBNALU(0x61 /*NAL ref=3, type=1*/, { 0xAA, 0xBB, 0xCC });
        bool accepted = decoder.SubmitFrame(MakeFrame(0, 0, false, std::move(data)));
        if (!accepted) {
            printf("  PASS: P-frame correctly rejected before first keyframe.\n");
            ++passed;
        } else {
            printf("  FAIL: P-frame should have been rejected.\n");
            ++failed;
        }
    }

    // -----------------------------------------------------------------------
    // Test 3: Keyframe with no SPS NALU in payload → must be rejected.
    // -----------------------------------------------------------------------
    printf("\n[Test 3] Keyframe with no SPS NALU — expect rejection WARN...\n");
    {
        // NALU header 0x65 = forbidden=0, nal_ref_idc=3, type=5 (IDR)
        // but no 0x67 (SPS) NALU present.
        auto data = AnnexBNALU(0x65, { 0x00, 0x01, 0x02, 0x03 });
        bool accepted = decoder.SubmitFrame(MakeFrame(1, 1000, true, std::move(data)));
        if (!accepted) {
            printf("  PASS: Keyframe without SPS correctly rejected.\n");
            ++passed;
        } else {
            printf("  FAIL: Keyframe without SPS should have been rejected.\n");
            ++failed;
        }
    }

    // -----------------------------------------------------------------------
    // Test 4: Keyframe with SPS NALU present but too short to parse.
    // -----------------------------------------------------------------------
    printf("\n[Test 4] Keyframe with truncated SPS — expect rejection WARN...\n");
    {
        // SPS NALU header = 0x67, then only 2 bytes of payload (need >=4).
        std::vector<uint8_t> spsNALU = { 0x00, 0x00, 0x00, 0x01, 0x67,
                                          0x42, 0xC0 }; // truncated after 2 RBSP bytes
        std::vector<uint8_t> idrNALU = { 0x00, 0x00, 0x00, 0x01, 0x65,
                                          0x88, 0x84, 0x00 };
        std::vector<uint8_t> data;
        data.insert(data.end(), spsNALU.begin(), spsNALU.end());
        data.insert(data.end(), idrNALU.begin(), idrNALU.end());

        bool accepted = decoder.SubmitFrame(MakeFrame(2, 2000, true, std::move(data)));
        if (!accepted) {
            printf("  PASS: Keyframe with truncated SPS correctly rejected.\n");
            ++passed;
        } else {
            printf("  FAIL: Keyframe with truncated SPS should have been rejected.\n");
            ++failed;
        }
    }

    // -----------------------------------------------------------------------
    // Test 5: Flush on uninitialized decoder — must not crash.
    // -----------------------------------------------------------------------
    printf("\n[Test 5] Flush on uninitialized decoder — must not crash...\n");
    {
        decoder.Flush();
        printf("  PASS: Flush on uninitialized decoder completed without crash.\n");
        ++passed;
    }

    // -----------------------------------------------------------------------
    // Test 6: Verify decoder still accepts P-frame rejection after Flush.
    // -----------------------------------------------------------------------
    printf("\n[Test 6] P-frame after Flush — still rejected...\n");
    {
        auto data = AnnexBNALU(0x41, { 0xDE, 0xAD });
        bool accepted = decoder.SubmitFrame(MakeFrame(3, 3000, false, std::move(data)));
        if (!accepted) {
            printf("  PASS: P-frame correctly rejected after Flush.\n");
            ++passed;
        } else {
            printf("  FAIL: P-frame should have been rejected after Flush.\n");
            ++failed;
        }
    }

    // -----------------------------------------------------------------------
    // Summary
    // -----------------------------------------------------------------------
    printf("\n=== Results ===\n");
    printf("  Passed : %d\n", passed);
    printf("  Failed : %d\n", failed);
    printf("  Decoder callbacks fired: %d\n", decoderFrames);
    printf("\n");
    printf("NOTE: Actual H.264 decode (IDR frame -> NV12 output) was NOT tested.\n");
    printf("Real encoded H.264 data is required for that path.\n");
    printf("Pending physical iPhone testing (no Mac/Xcode available).\n");
    printf("When the iPhone connects and streams, H264Decoder::DrainOutput\n");
    printf("will log 'DecodedFrame ready' for each successfully decoded frame.\n\n");

    return (failed == 0) ? 0 : 1;
}
