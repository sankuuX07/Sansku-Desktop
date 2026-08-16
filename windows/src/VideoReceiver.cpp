#include "VideoReceiver.h"
#include "Logger.h"

namespace SanskyStream {

// ---------------------------------------------------------------------------
// M6: Log the complete reassembled frame.
//
// This is the hand-off point between the transport layer (M6) and the
// decoder (M7).  In M7, OnCompleteFrame will pass the frame data to the
// Media Foundation H.264 decoder MFT.
// ---------------------------------------------------------------------------

void VideoReceiver::OnCompleteFrame(CompleteFrame frame)
{
    const char* kfStr = frame.isKeyframe ? "YES" : "NO";

    LOG_INFO("CompleteFrame received"
             " | ID: "       + std::to_string(frame.frameId) +
             " | Size: "     + std::to_string(frame.data.size()) + " bytes" +
             " | Keyframe: " + kfStr +
             " | PTS: "      + std::to_string(frame.presentationUs) + " \xc2\xb5s");

    // TODO (M7): pass frame.data to the H.264 decoder MFT.
    // The decoder will require frame.isKeyframe and frame.presentationUs
    // for correct initialization and timestamp propagation.
    (void)frame; // suppress unused-variable warning until M7
}

} // namespace SanskyStream
