#pragma once

#include "VideoPacket.h"
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// FrameAssembler
//
// Receives individual UDP video fragment payloads and reassembles them into
// complete encoded H.264 frames.
//
// Thread-safety: NOT internally synchronized.
// VideoUdpReceiver calls this only from its own receive thread.
//
// Design decisions:
//   - Maximum 32 in-flight partial frames (MAP_SIZE_LIMIT).
//     If exceeded, the oldest (lowest frameId) entry is evicted.
//   - Frames older than STALE_THRESHOLD_US (200 ms) relative to the newest
//     frame seen are evicted without delivery.  This keeps memory bounded and
//     prevents stale video from accumulating when fragments are lost.
//   - No retransmission: a frame that can't be completed is silently dropped.
//     The decoder (M7) will wait for the next keyframe to re-sync.
// ---------------------------------------------------------------------------
class FrameAssembler {
public:
    // Invoked when a complete frame is available.
    // Called from the receive thread — the callback must return quickly.
    using FrameCallback = std::function<void(CompleteFrame)>;

    explicit FrameAssembler(FrameCallback callback);
    ~FrameAssembler() = default;

    FrameAssembler(const FrameAssembler&) = delete;
    FrameAssembler& operator=(const FrameAssembler&) = delete;

    // Process one validated fragment.
    //   frameId        — frame this fragment belongs to
    //   presentationUs — PTS in microseconds (same for all fragments of a frame)
    //   isKeyframe     — true if this frame is an IDR keyframe
    //   packetSeq      — global UDP sequence number (for gap detection logging)
    //   fragmentIndex  — 0-based index of this fragment
    //   fragmentCount  — total expected fragments for this frame
    //   payload        — pointer to fragment byte data
    //   payloadSize    — byte count of payload
    void AddFragment(uint32_t frameId,
                     uint64_t presentationUs,
                     bool     isKeyframe,
                     uint32_t packetSeq,
                     uint16_t fragmentIndex,
                     uint16_t fragmentCount,
                     const uint8_t* payload,
                     uint32_t       payloadSize);

private:
    // ---------------------------------------------------------------------------
    // Internal state for one partially-received frame.
    // ---------------------------------------------------------------------------
    struct PartialFrame {
        uint64_t                          presentationUs   = 0;
        bool                              isKeyframe       = false;
        uint16_t                          fragmentCount    = 0;
        uint16_t                          receivedCount    = 0;
        std::vector<std::vector<uint8_t>> fragments;      // indexed by fragmentIndex

        explicit PartialFrame(uint16_t count, uint64_t pts, bool kf)
            : presentationUs(pts)
            , isKeyframe(kf)
            , fragmentCount(count)
            , receivedCount(0)
            , fragments(count)
        {}
    };

    void TryComplete(uint32_t frameId, PartialFrame& pf);
    void EvictStaleFrames(uint64_t newestPtsUs);
    void EvictOldestFrame();

    FrameCallback m_callback;

    // Key = frameId.  Ordered map so eviction of "oldest" is O(1).
    std::map<uint32_t, PartialFrame> m_frames;

    // Maximum number of simultaneously tracked partial frames.
    static constexpr size_t MAP_SIZE_LIMIT = 32;

    // Frames whose PTS is more than this many µs behind the newest seen PTS
    // are considered stale and discarded without delivery.
    static constexpr uint64_t STALE_THRESHOLD_US = 200'000; // 200 ms

    // PTS of the most recently seen fragment (any frame).
    uint64_t m_newestPtsUs = 0;
};

} // namespace SanskyStream
