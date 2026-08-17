#pragma once

#include "DecodedFrame.h"

#include <memory>
#include <mutex>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// VideoFrameQueue
//
// A thread-safe single-slot "latest frame" store.
//
// Design:
//   - Push() always overwrites the pending slot with the newest frame.
//   - TryPop() returns the latest pending frame (if any) to the caller.
//   - If the render thread is slow, older frames are silently overwritten.
//   - The renderer always gets the freshest available frame — latency never
//     accumulates.  This is the correct strategy for live video streaming.
//
// Threading:
//   Push() is called from the UDP receive thread (VideoReceiver).
//   TryPop() is called from the UI/render thread (Renderer::Render).
//   The internal mutex is held only for pointer-swap duration (O(1)).
//   Expensive memory deallocation of the overwritten frame happens outside
//   the mutex.
// ---------------------------------------------------------------------------
class VideoFrameQueue {
public:
    VideoFrameQueue()  = default;
    ~VideoFrameQueue() = default;

    VideoFrameQueue(const VideoFrameQueue&)            = delete;
    VideoFrameQueue& operator=(const VideoFrameQueue&) = delete;

    // Store the newest decoded frame.  Discards any frame not yet consumed.
    // Called from the receive thread.
    void Push(DecodedFrame frame);

    // Retrieve the latest frame since the last call.
    // Returns true and moves the frame into 'out' if a new frame is available.
    // Returns false if no new frame has arrived since the last TryPop.
    // Called from the render thread.
    bool TryPop(DecodedFrame& out);

private:
    std::mutex                    m_mutex;
    std::unique_ptr<DecodedFrame> m_pending; // null when no new frame
};

} // namespace SanskyStream
