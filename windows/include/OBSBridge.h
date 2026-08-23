#pragma once

// ---------------------------------------------------------------------------
// OBSBridge -- M14
//
// Named shared memory IPC bridge between SanskyStream and the OBS plugin
// (sansky-source.dll running inside obs64.exe).
//
// Design:
//   - SanskyStream WRITES only; OBS plugin READS only.
//   - Single-slot video: newest NV12 frame overwrites any unread frame.
//   - Single-slot audio: newest PCM chunk overwrites any unread chunk.
//   - Seqlock pattern for lock-free reads:
//       Writer: ++seq (odd); write data; ++seq (even).
//       Reader: read seq; read data; re-read seq; retry if changed.
//   - SanskyStream continues normally if OBSBridge fails (non-fatal).
//
// Shared memory layout (SANSKY_BRIDGE_SHMEM_SIZE bytes):
//
//   Offset  Size  Field
//   ------  ----  -----
//   0        4    magic (0x534F4253 = 'SOBS')
//   4        4    version (1)
//   8        8    video_seq   [seqlock, atomic]
//   16       4    video_width
//   20       4    video_height
//   24       8    video_pts_us
//   32       8    avdiff_us   (A/V diff from AVSynchronizer, us)
//   40       NV12_MAX_BYTES   NV12 pixel data
//   +0       8    audio_seq   [seqlock, atomic] (base = 40+NV12_MAX_BYTES)
//   +8       4    audio_sample_rate
//   +12      4    audio_channels
//   +16      4    audio_bytes
//   +20      8    audio_pts_us  (note: stored at 8-byte aligned +20)
//   +28      AUDIO_MAX_BYTES   PCM data
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace SanskyStream {

// Maximum supported video dimensions (4K NV12).
static constexpr uint32_t SHMEM_MAX_WIDTH  = 4096;
static constexpr uint32_t SHMEM_MAX_HEIGHT = 2160;
static constexpr size_t   NV12_MAX_BYTES   =
    static_cast<size_t>(SHMEM_MAX_WIDTH) * SHMEM_MAX_HEIGHT * 3u / 2u;

// Maximum audio chunk (8 x 1024-sample AAC frames, stereo 16-bit).
static constexpr size_t AUDIO_MAX_BYTES = 32768u;

// Video block offsets from start of shared memory:
static constexpr size_t SHMEM_VIDEO_SEQ_OFFSET   =  8u;
static constexpr size_t SHMEM_VIDEO_W_OFFSET     = 16u;
static constexpr size_t SHMEM_VIDEO_H_OFFSET     = 20u;
static constexpr size_t SHMEM_VIDEO_PTS_OFFSET   = 24u;
static constexpr size_t SHMEM_VIDEO_AVDIFF_OFFSET= 32u;
static constexpr size_t SHMEM_VIDEO_PIXELS_OFFSET= 40u;

// Audio block base offset:
static constexpr size_t SHMEM_AUDIO_BASE_OFFSET =
    SHMEM_VIDEO_PIXELS_OFFSET + NV12_MAX_BYTES;

// Audio block offsets relative to SHMEM_AUDIO_BASE_OFFSET:
static constexpr size_t SHMEM_AUDIO_SEQ_OFFSET   =  0u;
static constexpr size_t SHMEM_AUDIO_RATE_OFFSET  =  8u;
static constexpr size_t SHMEM_AUDIO_CH_OFFSET    = 12u;
static constexpr size_t SHMEM_AUDIO_BYTES_OFFSET = 16u;
static constexpr size_t SHMEM_AUDIO_PTS_OFFSET   = 20u;  // int64 at +20
static constexpr size_t SHMEM_AUDIO_PCM_OFFSET   = 28u;

// Total shared memory size:
static constexpr size_t SANSKY_BRIDGE_SHMEM_SIZE =
    SHMEM_AUDIO_BASE_OFFSET + SHMEM_AUDIO_PCM_OFFSET + AUDIO_MAX_BYTES;

// Shared memory name and magic:
static constexpr const char* SHMEM_NAME    = "SanskyStream_OBS_Bridge_v1";
static constexpr uint32_t    SHMEM_MAGIC   = 0x534F4253u; // 'SOBS'
static constexpr uint32_t    SHMEM_VERSION = 1u;

// ---------------------------------------------------------------------------
// OBSBridge -- writer side (runs inside SanskyStream.exe)
// ---------------------------------------------------------------------------
class OBSBridge {
public:
    OBSBridge();
    ~OBSBridge();

    OBSBridge(const OBSBridge&)            = delete;
    OBSBridge& operator=(const OBSBridge&) = delete;

    // True if shared memory was created successfully.
    bool IsReady() const { return m_mapped != nullptr; }

    // Push a decoded NV12 frame. Called from the video receive thread.
    // nv12Data must point to width*height*3/2 bytes (Y plane then UV plane).
    void PushVideoFrame(const uint8_t* nv12Data,
                        uint32_t width, uint32_t height,
                        uint64_t ptsUs, int64_t avDiffUs);

    // Push a decoded PCM audio chunk. Called from the network/audio thread.
    // byteCount must be <= AUDIO_MAX_BYTES. pcmData is 16-bit LE interleaved.
    void PushAudioData(const uint8_t* pcmData, size_t byteCount,
                       uint32_t sampleRate, uint32_t channels,
                       uint64_t ptsUs);

private:
    template<typename T>
    void WriteAt(size_t offset, T value) {
        std::memcpy(static_cast<uint8_t*>(m_mapped) + offset, &value, sizeof(T));
    }

    HANDLE m_mapHandle = nullptr;
    void*  m_mapped    = nullptr;
};

} // namespace SanskyStream
