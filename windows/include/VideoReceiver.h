#pragma once

#include "VideoPacket.h"   // CompleteFrame
#include "DecodedFrame.h"
#include "H264Decoder.h"

#include <memory>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// VideoReceiver
//
// Sits between the transport layer (FrameAssembler) and the decoder (M7).
//
// M6: only logged CompleteFrame metadata.
// M7: owns an H264Decoder and feeds it each CompleteFrame.
//     After decode, OnCompleteFrame logs the resulting DecodedFrame.
// M8: the DecodedFrame will be handed to the Renderer.
//
// Called from the VideoUdpReceiver receive thread — must return quickly.
// H264Decoder is called synchronously; it is not thread-safe internally.
// ---------------------------------------------------------------------------
class VideoReceiver {
public:
    VideoReceiver();
    ~VideoReceiver() = default;

    VideoReceiver(const VideoReceiver&)            = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // Invoked by FrameAssembler when a complete encoded frame is ready.
    // Forwards the frame to H264Decoder for decoding.
    void OnCompleteFrame(CompleteFrame frame);

private:
    // Called by H264Decoder when a frame has been decoded.
    void OnDecodedFrame(DecodedFrame frame);

    // The decoder is owned exclusively by VideoReceiver.
    // No shared ownership needed — it is called only from the receive thread.
    std::unique_ptr<H264Decoder> m_decoder;
};

} // namespace SanskyStream
