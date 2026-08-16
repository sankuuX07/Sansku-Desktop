#include "FrameAssembler.h"
#include "Logger.h"
#include "Protocol.h"

#include <cassert>
#include <numeric>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FrameAssembler::FrameAssembler(FrameCallback callback)
    : m_callback(std::move(callback))
{}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void FrameAssembler::AddFragment(uint32_t       frameId,
                                 uint64_t       presentationUs,
                                 bool           isKeyframe,
                                 uint32_t       /*packetSeq — logged by VideoUdpReceiver*/,
                                 uint16_t       fragmentIndex,
                                 uint16_t       fragmentCount,
                                 const uint8_t* payload,
                                 uint32_t       payloadSize)
{
    // --- Input bounds already validated by VideoUdpReceiver ---
    // fragmentCount and fragmentIndex are pre-checked.
    // payloadSize is pre-checked against VIDEO_MAX_PAYLOAD.

    // Update our record of the most-recent PTS seen (for stale eviction).
    if (presentationUs > m_newestPtsUs) {
        m_newestPtsUs = presentationUs;
    }

    // Evict frames that are too old.
    EvictStaleFrames(m_newestPtsUs);

    // Evict the oldest frame if we are at the map size limit.
    while (m_frames.size() >= MAP_SIZE_LIMIT) {
        EvictOldestFrame();
    }

    // Find or create the partial-frame entry.
    auto it = m_frames.find(frameId);
    if (it == m_frames.end()) {
        // First fragment for this frame.
        auto [inserted_it, ok] = m_frames.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(frameId),
            std::forward_as_tuple(fragmentCount, presentationUs, isKeyframe));
        (void)ok;
        it = inserted_it;
    }

    PartialFrame& pf = it->second;

    // Validate fragmentIndex against the already-established fragmentCount.
    if (fragmentIndex >= pf.fragmentCount) {
        LOG_WARN("FrameAssembler: frameId=" + std::to_string(frameId) +
                 " fragmentIndex=" + std::to_string(fragmentIndex) +
                 " >= fragmentCount=" + std::to_string(pf.fragmentCount) +
                 " — discarding fragment.");
        return;
    }

    // Drop duplicate fragments silently.
    if (!pf.fragments[fragmentIndex].empty()) {
        return;
    }

    // Store the fragment payload.
    pf.fragments[fragmentIndex].assign(payload, payload + payloadSize);
    ++pf.receivedCount;

    // Check if all fragments have arrived.
    if (pf.receivedCount == pf.fragmentCount) {
        TryComplete(frameId, pf);
        m_frames.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void FrameAssembler::TryComplete(uint32_t frameId, PartialFrame& pf)
{
    // Compute total data size.
    size_t totalSize = 0;
    for (const auto& frag : pf.fragments) {
        totalSize += frag.size();
    }

    // Safety: reject absurdly large frames.
    if (totalSize > Protocol::VIDEO_MAX_FRAME_SIZE) {
        LOG_WARN("FrameAssembler: frameId=" + std::to_string(frameId) +
                 " assembled size " + std::to_string(totalSize) +
                 " exceeds VIDEO_MAX_FRAME_SIZE — discarding.");
        return;
    }

    CompleteFrame frame;
    frame.frameId        = frameId;
    frame.presentationUs = pf.presentationUs;
    frame.isKeyframe     = pf.isKeyframe;
    frame.data.reserve(totalSize);

    for (auto& frag : pf.fragments) {
        frame.data.insert(frame.data.end(), frag.begin(), frag.end());
    }

    // Hand off to the registered callback (VideoReceiver).
    if (m_callback) {
        m_callback(std::move(frame));
    }
}

void FrameAssembler::EvictStaleFrames(uint64_t newestPtsUs)
{
    auto it = m_frames.begin();
    while (it != m_frames.end()) {
        const PartialFrame& pf = it->second;
        // Unsigned subtraction: only evict if newestPtsUs is genuinely larger.
        if (newestPtsUs > pf.presentationUs &&
            (newestPtsUs - pf.presentationUs) > STALE_THRESHOLD_US)
        {
            LOG_WARN("FrameAssembler: evicting stale frameId=" +
                     std::to_string(it->first) +
                     " (age=" +
                     std::to_string((newestPtsUs - pf.presentationUs) / 1000) +
                     " ms, received " +
                     std::to_string(pf.receivedCount) + "/" +
                     std::to_string(pf.fragmentCount) + " fragments).");
            it = m_frames.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameAssembler::EvictOldestFrame()
{
    if (m_frames.empty()) return;

    // std::map is ordered by key (frameId), so begin() is the oldest.
    auto it = m_frames.begin();
    LOG_WARN("FrameAssembler: map full, evicting oldest frameId=" +
             std::to_string(it->first) + ".");
    m_frames.erase(it);
}

} // namespace SanskyStream
