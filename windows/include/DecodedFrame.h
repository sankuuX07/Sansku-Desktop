#pragma once

#include <cstdint>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// DecodedFrame
//
// A fully decoded video frame produced by H264Decoder.
//
// Pixel format: NV12 (YUV 4:2:0, semi-planar)
//   Layout: [ Y plane (width * height bytes) ]
//           [ UV plane (width * height / 2 bytes, interleaved U and V) ]
//   Total bytes: width * height * 3 / 2
//
// NV12 is chosen because:
//   - It is the native output format of the Windows H.264 MFT decoder.
//   - It maps directly to DXGI_FORMAT_NV12 for zero-copy D3D11 upload in M8.
//
// M7: data is a CPU-side copy (std::vector).
// M8: this struct will gain a ComPtr<ID3D11Texture2D> for GPU-resident output.
//
// Timestamps: presentationUs comes directly from the iPhone encoder (via M6)
// and must not be overwritten with arrival time on Windows.
// ---------------------------------------------------------------------------
struct DecodedFrame {
    uint32_t             frameId;         // Monotonically increasing frame ID (from CompleteFrame)
    uint64_t             presentationUs;  // Original encoder PTS in microseconds
    uint32_t             width;           // Decoded frame width  (pixels, from SPS)
    uint32_t             height;          // Decoded frame height (pixels, from SPS)
    std::vector<uint8_t> nv12Data;        // NV12 pixel data (Y then UV plane)
};

} // namespace SanskyStream
