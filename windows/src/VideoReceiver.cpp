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
// Transport → Decoder hand-off (called from receive thread)
// ---------------------------------------------------------------------------

void VideoReceiver::OnCompleteFrame(CompleteFrame frame)
{
    // Log the incoming encoded frame (M6 behaviour retained for diagnostics).
    LOG_INFO("CompleteFrame received"
             " | ID: "       + std::to_string(frame.frameId) +
             " | Size: "     + std::to_string(frame.data.size()) + " bytes" +
             " | Keyframe: " + (frame.isKeyframe ? "YES" : "NO") +
             " | PTS: "      + std::to_string(frame.presentationUs) + " \xc2\xb5s");

    // Submit to the H.264 decoder.  SubmitFrame is a no-op if:
    //   - the frame is not a keyframe and the decoder is not yet initialised,
    //   - the frame is stale (>500 ms behind last decoded PTS),
    //   - the SPS cannot be parsed.
    m_decoder->SubmitFrame(frame);
}

// ---------------------------------------------------------------------------
// Decoder → Next stage hand-off (called from the same thread)
// ---------------------------------------------------------------------------

void VideoReceiver::OnDecodedFrame(DecodedFrame frame)
{
    // M7 endpoint: log the decoded frame.
    // M8 will forward this to the Renderer instead.
    LOG_INFO("DecodedFrame ready"
             " | ID: "     + std::to_string(frame.frameId) +
             " | "         + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
             " | NV12: "   + std::to_string(frame.nv12Data.size()) + " bytes" +
             " | PTS: "    + std::to_string(frame.presentationUs) + " \xc2\xb5s");

    // TODO (M8): pass frame to Renderer for D3D11 texture upload and display.
    (void)frame;
}

} // namespace SanskyStream
