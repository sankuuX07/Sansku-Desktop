#pragma once

#include <cstdint>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// CompleteFrame
//
// A fully reassembled H.264 encoded video frame, produced by FrameAssembler
// once all UDP fragments for a given frameId have been received.
//
// The payload is raw H.264 in Annex B format (start codes prepended).
// For keyframes the SPS and PPS NALUs appear first in the payload.
//
// This struct is the hand-off point between the transport layer (M6) and the
// decoder layer (M7).  The decoder must not be called until M7 is ready.
// ---------------------------------------------------------------------------
struct CompleteFrame {
    uint32_t             frameId;         // Monotonically increasing frame ID
    uint64_t             presentationUs;  // Presentation timestamp (µs, from encoder)
    bool                 isKeyframe;      // True if this is an IDR/keyframe
    std::vector<uint8_t> data;            // Raw H.264 NALU bytes (Annex B)
};

} // namespace SanskyStream
