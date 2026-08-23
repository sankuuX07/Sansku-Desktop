#include "OBSBridge.h"
#include "Logger.h"

#include <cstring>

// Seqlock write memory ordering: on x86/x64, std::atomic with
// release/acquire semantics prevents compiler and hardware reordering
// across the seq increment boundaries.
// Writer protocol:
//   1. atomic_fetch_add(seq, 1)  --> seq becomes ODD (busy)
//   2. write data
//   3. atomic_fetch_add(seq, 1)  --> seq becomes EVEN (done)
// Reader (OBS plugin) verifies seq is EVEN and unchanged after reading.

namespace SanskyStream {

OBSBridge::OBSBridge()
{
    // Create the named shared memory segment.
    m_mapHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(SANSKY_BRIDGE_SHMEM_SIZE),
        SHMEM_NAME);

    if (!m_mapHandle) {
        LOG_WARN("OBSBridge: CreateFileMapping failed (error " +
                 std::to_string(GetLastError()) + "). OBS integration disabled.");
        return;
    }

    m_mapped = MapViewOfFile(m_mapHandle, FILE_MAP_WRITE, 0, 0, 0);
    if (!m_mapped) {
        LOG_WARN("OBSBridge: MapViewOfFile failed (error " +
                 std::to_string(GetLastError()) + "). OBS integration disabled.");
        CloseHandle(m_mapHandle);
        m_mapHandle = nullptr;
        return;
    }

    // Zero the entire segment, then write header.
    std::memset(m_mapped, 0, SANSKY_BRIDGE_SHMEM_SIZE);
    WriteAt<uint32_t>(0,                 SHMEM_MAGIC);
    WriteAt<uint32_t>(sizeof(uint32_t),  SHMEM_VERSION);

    LOG_INFO("OBSBridge: Shared memory '" + std::string(SHMEM_NAME) +
             "' created (" +
             std::to_string(SANSKY_BRIDGE_SHMEM_SIZE / 1024) + " KB). " +
             "OBS plugin can connect.");
}

OBSBridge::~OBSBridge()
{
    if (m_mapped) {
        // Mark the segment as dead so the OBS plugin stops reading.
        WriteAt<uint32_t>(0, 0u); // Clear magic
        UnmapViewOfFile(m_mapped);
        m_mapped = nullptr;
    }
    if (m_mapHandle) {
        CloseHandle(m_mapHandle);
        m_mapHandle = nullptr;
    }
    LOG_INFO("OBSBridge: Shared memory released.");
}

void OBSBridge::PushVideoFrame(const uint8_t* nv12Data,
                                uint32_t width, uint32_t height,
                                uint64_t ptsUs, int64_t avDiffUs)
{
    if (!m_mapped) return;

    const size_t frameBytes =
        static_cast<size_t>(width) * height * 3u / 2u;

    // Guard against oversized frames (> max supported resolution).
    if (frameBytes > NV12_MAX_BYTES || width > SHMEM_MAX_WIDTH || height > SHMEM_MAX_HEIGHT) {
        LOG_WARN("OBSBridge: PushVideoFrame: frame " +
                 std::to_string(width) + "x" + std::to_string(height) +
                 " exceeds max supported size -- skipping.");
        return;
    }

    auto* seqPtr = reinterpret_cast<std::atomic<uint64_t>*>(
        static_cast<uint8_t*>(m_mapped) + SHMEM_VIDEO_SEQ_OFFSET);

    // --- Seqlock write: begin ---
    seqPtr->fetch_add(1u, std::memory_order_release); // seq -> ODD

    WriteAt<uint32_t>(SHMEM_VIDEO_W_OFFSET,      width);
    WriteAt<uint32_t>(SHMEM_VIDEO_H_OFFSET,       height);
    WriteAt<uint64_t>(SHMEM_VIDEO_PTS_OFFSET,     ptsUs);
    WriteAt<int64_t> (SHMEM_VIDEO_AVDIFF_OFFSET,  avDiffUs);

    std::memcpy(
        static_cast<uint8_t*>(m_mapped) + SHMEM_VIDEO_PIXELS_OFFSET,
        nv12Data,
        frameBytes);

    seqPtr->fetch_add(1u, std::memory_order_release); // seq -> EVEN
    // --- Seqlock write: end ---
}

void OBSBridge::PushAudioData(const uint8_t* pcmData, size_t byteCount,
                               uint32_t sampleRate, uint32_t channels,
                               uint64_t ptsUs)
{
    if (!m_mapped) return;
    if (!pcmData || byteCount == 0) return;
    if (byteCount > AUDIO_MAX_BYTES) {
        byteCount = AUDIO_MAX_BYTES; // Clamp silently -- should never happen.
    }

    auto* seqPtr = reinterpret_cast<std::atomic<uint64_t>*>(
        static_cast<uint8_t*>(m_mapped) + SHMEM_AUDIO_BASE_OFFSET + SHMEM_AUDIO_SEQ_OFFSET);

    // --- Seqlock write: begin ---
    seqPtr->fetch_add(1u, std::memory_order_release);

    const size_t base = SHMEM_AUDIO_BASE_OFFSET;
    WriteAt<uint32_t>(base + SHMEM_AUDIO_RATE_OFFSET,  sampleRate);
    WriteAt<uint32_t>(base + SHMEM_AUDIO_CH_OFFSET,    channels);
    WriteAt<uint32_t>(base + SHMEM_AUDIO_BYTES_OFFSET, static_cast<uint32_t>(byteCount));
    WriteAt<uint64_t>(base + SHMEM_AUDIO_PTS_OFFSET,   ptsUs);

    std::memcpy(
        static_cast<uint8_t*>(m_mapped) + base + SHMEM_AUDIO_PCM_OFFSET,
        pcmData,
        byteCount);

    seqPtr->fetch_add(1u, std::memory_order_release);
    // --- Seqlock write: end ---
}

} // namespace SanskyStream
