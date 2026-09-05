#include "VideoReceiver.h"
#include "AVSynchronizer.h"
#include "OBSBridge.h"   // M14
#include "Logger.h"

#include <cstdint>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

VideoReceiver::VideoReceiver()
    : m_decoder(std::make_unique<H264Decoder>(
          [this](DecodedFrame frame) { OnDecodedFrame(std::move(frame)); }))
{}

// ---------------------------------------------------------------------------
// Public API — SetFrameQueue
// ---------------------------------------------------------------------------

void VideoReceiver::SetFrameQueue(VideoFrameQueue* queue) {
    m_frameQueue = queue;
}

// ---------------------------------------------------------------------------
// Public API — SetAVSync (M12)
// ---------------------------------------------------------------------------

void VideoReceiver::SetAVSync(AVSynchronizer* sync) {
    m_avSync = sync;
}

// ---------------------------------------------------------------------------
// Public API — SetOBSBridge (M14)
// ---------------------------------------------------------------------------

void VideoReceiver::SetOBSBridge(OBSBridge* bridge) {
    m_obsBridge = bridge;
}

// ---------------------------------------------------------------------------
// Transport → Decoder hand-off (called from receive thread)
// ---------------------------------------------------------------------------

void VideoReceiver::OnCompleteFrame(CompleteFrame frame)
{
    // M13: periodic diagnostic log every 300 frames (~5 s at 60 fps).
    // Per-frame LOG_INFO was causing ~60 std::to_string() heap allocations/sec
    // on the hot receive path, adding measurable CPU cost.
    ++m_framesReceived;
    if (m_framesReceived % 300 == 1) {
        LOG_INFO("CompleteFrame: " + std::to_string(m_framesReceived)
                 + " received | last ID: " + std::to_string(frame.frameId)
                 + " | " + std::to_string(frame.data.size()) + " bytes"
                 + " | " + (frame.isKeyframe ? "KEYFRAME" : "P-frame")
                 + " | PTS: " + std::to_string(frame.presentationUs) + " µs");
    }

    // Submit to the H.264 decoder.  SubmitFrame is a no-op if:
    //   - the frame is not a keyframe and the decoder is not yet initialised,
    //   - the frame is stale (> 500 ms behind last decoded PTS),
    //   - the SPS cannot be parsed.
    m_decoder->SubmitFrame(frame);
}

// ---------------------------------------------------------------------------
// Decoder → Renderer hand-off (called from the same receive thread)
//
// M12: before enqueuing, ask AVSynchronizer whether the frame is on-time
// or severely stale.  Stale frames are discarded here; the synchronizer
// logs the drop internally so we do not need to log again.
// ---------------------------------------------------------------------------

void VideoReceiver::OnDecodedFrame(DecodedFrame frame)
{
    // M13: periodic log — every 300 decoded frames to avoid hot-path allocs.
    ++m_framesDecoded;
    if (m_framesDecoded % 300 == 1) {
        LOG_INFO("DecodedFrame: " + std::to_string(m_framesDecoded)
                 + " decoded | last ID: " + std::to_string(frame.frameId)
                 + " | " + std::to_string(frame.width) + "x" + std::to_string(frame.height)
                 + " | NV12: " + std::to_string(frame.nv12Data.size()) + " bytes"
                 + " | PTS: " + std::to_string(frame.presentationUs) + " µs");
    }

    // M12: synchronization gate — drop stale frames to prevent latency growth.
    if (m_avSync) {
        const FrameDecision decision = m_avSync->CheckVideoFrame(frame.presentationUs);
        if (decision == FrameDecision::Drop) {
            ++m_framesDropped; // M13: track for periodic stats
            return;
        }
    }

    // M14: push the sync-gated NV12 frame to the OBS shared-memory bridge.
    // This is the same decoded frame that goes to the renderer — no second
    // decode, no extra buffering.  OBSBridge::PushVideoFrame uses a seqlock
    // write so the OBS plugin can read it lock-free from obs64.exe.
    if (m_obsBridge) {
        const int64_t avDiffUs = m_avSync ? m_avSync->GetStats().avDiffUs : 0;
        m_obsBridge->PushVideoFrame(
            frame.nv12Data.data(),
            frame.width,
            frame.height,
            frame.presentationUs,
            avDiffUs);
    }

    // M8: forward to Renderer via the shared frame queue.
    // If no queue is connected (headless / test mode), the frame is discarded.
    if (m_frameQueue) {
        m_frameQueue->Push(std::move(frame));
    }
}

} // namespace SanskyStream
