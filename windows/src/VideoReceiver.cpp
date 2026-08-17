#include "VideoReceiver.h"
#include "Logger.h"

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
// Transport → Decoder hand-off (called from receive thread)
// ---------------------------------------------------------------------------

void VideoReceiver::OnCompleteFrame(CompleteFrame frame)
{
    // Diagnostic log retained from M6/M7.
    LOG_INFO("CompleteFrame received"
             " | ID: "       + std::to_string(frame.frameId) +
             " | Size: "     + std::to_string(frame.data.size()) + " bytes" +
             " | Keyframe: " + (frame.isKeyframe ? "YES" : "NO") +
             " | PTS: "      + std::to_string(frame.presentationUs) + " \xc2\xb5s");

    // Submit to the H.264 decoder.  SubmitFrame is a no-op if:
    //   - the frame is not a keyframe and the decoder is not yet initialised,
    //   - the frame is stale (> 500 ms behind last decoded PTS),
    //   - the SPS cannot be parsed.
    m_decoder->SubmitFrame(frame);
}

// ---------------------------------------------------------------------------
// Decoder → Renderer hand-off (called from the same receive thread)
// ---------------------------------------------------------------------------

void VideoReceiver::OnDecodedFrame(DecodedFrame frame)
{
    LOG_INFO("DecodedFrame ready"
             " | ID: "   + std::to_string(frame.frameId) +
             " | "       + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
             " | NV12: " + std::to_string(frame.nv12Data.size()) + " bytes"
             " | PTS: "  + std::to_string(frame.presentationUs) + " \xc2\xb5s");

    // M8: forward to Renderer via the shared frame queue.
    // If no queue is connected (headless / test mode), the frame is discarded.
    if (m_frameQueue) {
        m_frameQueue->Push(std::move(frame));
    }
}

} // namespace SanskyStream
