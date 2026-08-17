#include "VideoFrameQueue.h"

namespace SanskyStream {

void VideoFrameQueue::Push(DecodedFrame frame)
{
    // Build the new frame node outside the lock so no allocations happen
    // while holding the mutex.
    std::unique_ptr<DecodedFrame> incoming =
        std::make_unique<DecodedFrame>(std::move(frame));

    // Swap atomically — the displaced (older) frame is destroyed AFTER the
    // mutex is released, keeping the critical section O(1).
    std::unique_ptr<DecodedFrame> displaced;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        displaced  = std::move(m_pending);
        m_pending  = std::move(incoming);
    }
    // displaced destructor runs here — potentially frees a large NV12 vector,
    // but outside the mutex so it cannot block the render thread.
}

bool VideoFrameQueue::TryPop(DecodedFrame& out)
{
    std::unique_ptr<DecodedFrame> taken;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pending) return false;
        taken = std::move(m_pending);
    }
    out = std::move(*taken);
    return true;
}

} // namespace SanskyStream
