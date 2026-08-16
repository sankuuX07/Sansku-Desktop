#pragma once

#include "VideoPacket.h"

namespace SanskyStream {

// ---------------------------------------------------------------------------
// VideoReceiver
//
// Sits between the transport layer (FrameAssembler) and the future decoder
// layer (M7).  For M6 it only logs the received frame; no decoding occurs.
//
// Called from the VideoUdpReceiver receive thread.
// ---------------------------------------------------------------------------
class VideoReceiver {
public:
    VideoReceiver()  = default;
    ~VideoReceiver() = default;

    VideoReceiver(const VideoReceiver&)            = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // Invoked by FrameAssembler when a complete frame is ready.
    // Must return quickly — does not decode.
    void OnCompleteFrame(CompleteFrame frame);
};

} // namespace SanskyStream
