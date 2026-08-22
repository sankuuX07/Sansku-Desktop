#pragma once

#include "VideoPacket.h"   // CompleteFrame
#include "DecodedFrame.h"
#include "H264Decoder.h"
#include "VideoFrameQueue.h"

#include <memory>

namespace SanskyStream {

// Forward declaration — AVSynchronizer is an optional dependency.
// Including the full header here would drag in windows.h through AVSynchronizer.h.
class AVSynchronizer;

// ---------------------------------------------------------------------------
// VideoReceiver
//
// Sits between the transport layer (FrameAssembler) and the decoder (M7).
//
// M6: only logged CompleteFrame metadata.
// M7: owns an H264Decoder; logs the resulting DecodedFrame.
// M8: pushes DecodedFrame into VideoFrameQueue for the render thread.
// M12: optionally filters decoded frames through AVSynchronizer before
//      enqueuing; stale frames (FrameDecision::Drop) are discarded.
//
// Threading:
//   OnCompleteFrame() is called from the VideoUdpReceiver receive thread.
//   VideoFrameQueue::Push() is thread-safe (O(1) mutex swap).
//   AVSynchronizer::CheckVideoFrame() is thread-safe (internally locked).
//   All other work remains single-threaded (receive thread).
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

    // Connect the frame queue that receives decoded frames.
    // Call once from the main thread before any video arrives.
    // Pass nullptr to disable forwarding (headless / test mode).
    void SetFrameQueue(VideoFrameQueue* queue);

    // Connect the A/V synchronizer (M12).
    // When set, decoded frames are evaluated with CheckVideoFrame() before
    // being enqueued. Stale frames are silently dropped.
    // Pass nullptr to disable sync filtering (pre-M12 behaviour).
    void SetAVSync(AVSynchronizer* sync);

private:
    // Called by H264Decoder when a frame has been decoded.
    void OnDecodedFrame(DecodedFrame frame);

    // The decoder is owned exclusively by VideoReceiver.
    // Called only from the receive thread — not internally synchronised.
    std::unique_ptr<H264Decoder> m_decoder;

    // Non-owning pointer to the shared VideoFrameQueue (owned by App).
    // Null until SetFrameQueue() is called.
    VideoFrameQueue* m_frameQueue = nullptr;

    // Non-owning pointer to the AVSynchronizer (owned by App).
    // Null until SetAVSync() is called (M12).
    AVSynchronizer* m_avSync = nullptr;
};

} // namespace SanskyStream
