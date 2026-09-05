/*
 * sansky-source.c  --  M14
 *
 * OBS Studio async source plugin for SanskyStream.
 *
 * Architecture:
 *   - Runs inside obs64.exe as obs-plugins/64bit/sansky-source.dll.
 *   - Opens the named shared memory segment written by SanskyStream.exe
 *     via OBSBridge ("SanskyStream_OBS_Bridge_v1").
 *   - On every OBS video_tick(), reads the latest NV12 video frame and
 *     PCM16 audio chunk (seqlock read protocol) and calls:
 *       obs_source_output_video2()   -- async NV12 video to OBS
 *       obs_source_output_audio()    -- PCM16 stereo to OBS mixer
 *   - OBS A/V sync is driven by the timestamps written by OBSBridge
 *     (video_pts_us / audio_pts_us, converted us -> ns for OBS).
 *
 * Seqlock read protocol (matches OBSBridge writer):
 *   1. Read seq (atomic load acquire).
 *   2. If seq is ODD, writer is mid-write -- skip this tick.
 *   3. Read data (memcpy).
 *   4. Re-read seq.  If changed (or still odd), retry up to MAX_RETRIES.
 *   5. On retry exhaustion, skip this tick (old frame shown by OBS).
 *
 * Single-slot semantics:
 *   Each shmem slot holds the LATEST frame/chunk only.  If OBS ticks
 *   faster than SanskyStream produces frames, OBS shows the previous frame
 *   (normal for async sources).  No queuing, no latency growth.
 *
 * Magic sentinel:
 *   Byte 0-3 of shmem contains 0x534F4253 ("SOBS") while SanskyStream is
 *   running.  OBSBridge::~OBSBridge() zeroes it.  The plugin checks this
 *   every tick and silently skips if SanskyStream is not running.
 *
 * Threading:
 *   obs_source_video_tick() is called from the OBS graphics thread.
 *   All shmem access happens on that thread only.  No internal locks needed.
 */

/* WIN32_LEAN_AND_MEAN and NOMINMAX are defined via CMake compile definitions */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* OBS plugin API */
#include <obs-module.h>
#include <obs-source.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>

/* ---------------------------------------------------------------------------
 * Shared memory layout constants
 * Must stay in sync with OBSBridge.h.
 * Mirrored here in plain C so the plugin has no C++ dependency.
 * -------------------------------------------------------------------------- */

#define SHMEM_MAGIC   0x534F4253u   /* 'SOBS' */
#define SHMEM_VERSION 1u
#define SHMEM_NAME    "SanskyStream_OBS_Bridge_v1"

#define SHMEM_MAX_WIDTH  4096u
#define SHMEM_MAX_HEIGHT 2160u
#define NV12_MAX_BYTES   ((size_t)SHMEM_MAX_WIDTH * SHMEM_MAX_HEIGHT * 3u / 2u)
#define AUDIO_MAX_BYTES  32768u

/* Video block offsets from start of shared memory */
#define SHMEM_VIDEO_SEQ_OFFSET     8u
#define SHMEM_VIDEO_W_OFFSET      16u
#define SHMEM_VIDEO_H_OFFSET      20u
#define SHMEM_VIDEO_PTS_OFFSET    24u
#define SHMEM_VIDEO_AVDIFF_OFFSET 32u
#define SHMEM_VIDEO_PIXELS_OFFSET 40u

/* Audio block */
#define SHMEM_AUDIO_BASE_OFFSET  (SHMEM_VIDEO_PIXELS_OFFSET + NV12_MAX_BYTES)
#define SHMEM_AUDIO_SEQ_OFFSET    0u
#define SHMEM_AUDIO_RATE_OFFSET   8u
#define SHMEM_AUDIO_CH_OFFSET    12u
#define SHMEM_AUDIO_BYTES_OFFSET 16u
#define SHMEM_AUDIO_PTS_OFFSET   20u
#define SHMEM_AUDIO_PCM_OFFSET   28u

#define SANSKY_BRIDGE_SHMEM_SIZE \
    (SHMEM_AUDIO_BASE_OFFSET + SHMEM_AUDIO_PCM_OFFSET + AUDIO_MAX_BYTES)

/* Seqlock retry limit per tick */
#define SEQLOCK_MAX_RETRIES 8

/* ---------------------------------------------------------------------------
 * Scratch buffers (heap-allocated per instance)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t nv12[NV12_MAX_BYTES];  /* Y then UV planes, up to 4K */
    uint8_t pcm[AUDIO_MAX_BYTES];  /* 16-bit LE interleaved PCM  */
} sansky_scratch_t;

/* ---------------------------------------------------------------------------
 * Per-instance state
 * -------------------------------------------------------------------------- */
typedef struct {
    obs_source_t     *source;
    HANDLE            map_handle;
    const uint8_t    *mapped;         /* read-only view of shmem */
    sansky_scratch_t *scratch;
    uint64_t          last_audio_seq; /* detect new audio chunks  */
    uint32_t          last_width;
    uint32_t          last_height;
} sansky_ctx_t;

/* ---------------------------------------------------------------------------
 * Scalar read helpers (no alignment assumptions)
 * -------------------------------------------------------------------------- */
static inline uint32_t rd_u32(const uint8_t *base, size_t off)
{
    uint32_t v; memcpy(&v, base + off, 4u); return v;
}
static inline uint64_t rd_u64(const uint8_t *base, size_t off)
{
    uint64_t v; memcpy(&v, base + off, 8u); return v;
}

/* ---------------------------------------------------------------------------
 * Seqlock atomic load
 * Uses InterlockedAdd64(ptr, 0) for a sequentially-consistent read on
 * MSVC/x64 without requiring C11 stdatomic.
 * -------------------------------------------------------------------------- */
static inline uint64_t seq_load(const uint8_t *base, size_t off)
{
    volatile LONGLONG *p = (volatile LONGLONG *)(base + off);
    return (uint64_t)InterlockedAdd64(p, 0LL);
}

/* ---------------------------------------------------------------------------
 * Module boilerplate
 * -------------------------------------------------------------------------- */
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("sansky-source", "en-US")

/* Forward declaration */
static struct obs_source_info s_sansky_info;

bool obs_module_load(void)
{
    obs_register_source(&s_sansky_info);
    blog(LOG_INFO, "[sansky-source] Module loaded. "
                   "SanskyStream OBS integration active (M14).");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[sansky-source] Module unloaded.");
}

/* ---------------------------------------------------------------------------
 * Source callbacks
 * -------------------------------------------------------------------------- */

static const char *sansky_get_name(void *unused)
{
    (void)unused;
    return "SanskyStream iPhone";
}

/* ----------------------------------------------------------------------- */
static void *sansky_create(obs_data_t *settings, obs_source_t *source)
{
    (void)settings;

    sansky_ctx_t *ctx = bzalloc(sizeof(sansky_ctx_t));
    if (!ctx) {
        blog(LOG_ERROR, "[sansky-source] bzalloc(ctx) failed.");
        return NULL;
    }
    ctx->source = source;

    /* Allocate scratch buffers (NV12_MAX_BYTES alone is ~12 MB for 4K). */
    ctx->scratch = bzalloc(sizeof(sansky_scratch_t));
    if (!ctx->scratch) {
        blog(LOG_ERROR, "[sansky-source] bzalloc(scratch) failed.");
        bfree(ctx);
        return NULL;
    }

    /* Try to open the shmem segment.
     * If SanskyStream is not running yet this will fail -- that is fine,
     * video_tick() will retry every frame. */
    ctx->map_handle = OpenFileMappingA(FILE_MAP_READ, FALSE, SHMEM_NAME);
    if (ctx->map_handle) {
        ctx->mapped = (const uint8_t *)
            MapViewOfFile(ctx->map_handle, FILE_MAP_READ, 0, 0, 0);
        if (!ctx->mapped) {
            blog(LOG_ERROR,
                 "[sansky-source] MapViewOfFile failed (error %lu).",
                 GetLastError());
            CloseHandle(ctx->map_handle);
            ctx->map_handle = NULL;
        } else {
            blog(LOG_INFO,
                 "[sansky-source] Shared memory '%s' opened. "
                 "Waiting for SanskyStream to stream.", SHMEM_NAME);
        }
    } else {
        blog(LOG_INFO,
             "[sansky-source] Shared memory '%s' not yet available "
             "(SanskyStream not running). Will retry each tick.", SHMEM_NAME);
    }

    return ctx;
}

/* ----------------------------------------------------------------------- */
static void sansky_destroy(void *data)
{
    sansky_ctx_t *ctx = (sansky_ctx_t *)data;
    if (!ctx) return;

    if (ctx->mapped)     { UnmapViewOfFile(ctx->mapped); ctx->mapped = NULL; }
    if (ctx->map_handle) { CloseHandle(ctx->map_handle); ctx->map_handle = NULL; }
    if (ctx->scratch)    { bfree(ctx->scratch); ctx->scratch = NULL; }
    bfree(ctx);

    blog(LOG_INFO, "[sansky-source] Source destroyed.");
}

/* ----------------------------------------------------------------------- */
static uint32_t sansky_get_width(void *data)
{
    const sansky_ctx_t *ctx = (const sansky_ctx_t *)data;
    return ctx ? ctx->last_width : 0u;
}

static uint32_t sansky_get_height(void *data)
{
    const sansky_ctx_t *ctx = (const sansky_ctx_t *)data;
    return ctx ? ctx->last_height : 0u;
}

/* ---------------------------------------------------------------------------
 * sansky_video_tick
 *
 * Called by OBS once per output frame (e.g. 60 Hz).
 * Reads video + audio from shmem and pushes to OBS.
 * -------------------------------------------------------------------------- */
static void sansky_video_tick(void *data, float seconds)
{
    (void)seconds;
    sansky_ctx_t *ctx = (sansky_ctx_t *)data;
    if (!ctx || !ctx->scratch) return;

    /* ------------------------------------------------------------------ */
    /* 0.  Try to (re)connect to shmem if not yet open.                    */
    /* ------------------------------------------------------------------ */
    if (!ctx->mapped) {
        ctx->map_handle = OpenFileMappingA(FILE_MAP_READ, FALSE, SHMEM_NAME);
        if (ctx->map_handle) {
            ctx->mapped = (const uint8_t *)
                MapViewOfFile(ctx->map_handle, FILE_MAP_READ, 0, 0, 0);
            if (!ctx->mapped) {
                CloseHandle(ctx->map_handle);
                ctx->map_handle = NULL;
            } else {
                blog(LOG_INFO,
                     "[sansky-source] (Re)connected to SanskyStream shmem.");
            }
        }
        if (!ctx->mapped) return;
    }

    const uint8_t *mem = ctx->mapped;

    /* ------------------------------------------------------------------ */
    /* 1.  Verify magic -- detect SanskyStream shutdown.                   */
    /* ------------------------------------------------------------------ */
    if (rd_u32(mem, 0u) != SHMEM_MAGIC) {
        UnmapViewOfFile(ctx->mapped);  ctx->mapped     = NULL;
        CloseHandle(ctx->map_handle);  ctx->map_handle = NULL;
        ctx->last_audio_seq = 0;
        ctx->last_width     = 0;
        ctx->last_height    = 0;
        blog(LOG_INFO, "[sansky-source] SanskyStream disconnected (magic gone).");
        return;
    }

    /* ================================================================== */
    /* VIDEO                                                                 */
    /* ================================================================== */
    {
        uint32_t vid_w = 0, vid_h = 0;
        uint64_t vid_pts = 0;
        bool vid_ok = false;

        for (int r = 0; r < SEQLOCK_MAX_RETRIES; ++r) {
            uint64_t s0 = seq_load(mem, SHMEM_VIDEO_SEQ_OFFSET);
            if (s0 & 1u) continue; /* writer busy */

            vid_w   = rd_u32(mem, SHMEM_VIDEO_W_OFFSET);
            vid_h   = rd_u32(mem, SHMEM_VIDEO_H_OFFSET);
            vid_pts = rd_u64(mem, SHMEM_VIDEO_PTS_OFFSET);

            if (vid_w == 0 || vid_w > SHMEM_MAX_WIDTH ||
                vid_h == 0 || vid_h > SHMEM_MAX_HEIGHT) break;

            const size_t fb = (size_t)vid_w * (size_t)vid_h * 3u / 2u;
            memcpy(ctx->scratch->nv12, mem + SHMEM_VIDEO_PIXELS_OFFSET, fb);

            uint64_t s1 = seq_load(mem, SHMEM_VIDEO_SEQ_OFFSET);
            if (s1 == s0 && !(s1 & 1u)) { vid_ok = true; break; }
            /* else writer updated mid-copy -- retry */
        }

        if (vid_ok && vid_w > 0 && vid_h > 0) {
            ctx->last_width  = vid_w;
            ctx->last_height = vid_h;

            struct obs_source_frame2 frame;
            memset(&frame, 0, sizeof(frame));

            frame.width     = vid_w;
            frame.height    = vid_h;
            frame.timestamp = vid_pts * 1000ULL; /* us -> ns */
            frame.format    = VIDEO_FORMAT_NV12;
            frame.range     = VIDEO_RANGE_PARTIAL; /* BT.601 limited (iOS default) */
            frame.flip      = false;

            /* Y plane: full resolution, 1 byte/sample */
            frame.data[0]     = ctx->scratch->nv12;
            frame.linesize[0] = vid_w;

            /* UV plane: immediately after Y, 2 bytes per 2x2 chroma block */
            frame.data[1]     = ctx->scratch->nv12 + (size_t)vid_w * (size_t)vid_h;
            frame.linesize[1] = vid_w;

            obs_source_output_video2(ctx->source, &frame);
        }
    }

    /* ================================================================== */
    /* AUDIO                                                                 */
    /* ================================================================== */
    {
        const size_t ab = SHMEM_AUDIO_BASE_OFFSET;

        /* Peek at the audio seqlock to check if there is a new chunk. */
        uint64_t audio_seq = seq_load(mem, ab + SHMEM_AUDIO_SEQ_OFFSET);

        /* Only read if: sequence is even (complete write) AND changed since last. */
        if (!(audio_seq & 1u) && audio_seq != ctx->last_audio_seq && audio_seq > 0) {

            uint32_t aud_rate = 0, aud_ch = 0, aud_bytes = 0;
            uint64_t aud_pts = 0;
            bool aud_ok = false;

            for (int r = 0; r < SEQLOCK_MAX_RETRIES; ++r) {
                uint64_t s0 = seq_load(mem, ab + SHMEM_AUDIO_SEQ_OFFSET);
                if (s0 & 1u) continue;

                aud_rate  = rd_u32(mem, ab + SHMEM_AUDIO_RATE_OFFSET);
                aud_ch    = rd_u32(mem, ab + SHMEM_AUDIO_CH_OFFSET);
                aud_bytes = rd_u32(mem, ab + SHMEM_AUDIO_BYTES_OFFSET);
                aud_pts   = rd_u64(mem, ab + SHMEM_AUDIO_PTS_OFFSET);

                if (aud_bytes == 0 || aud_bytes > AUDIO_MAX_BYTES ||
                    aud_rate  == 0 || aud_ch == 0 || aud_ch > 8) break;

                memcpy(ctx->scratch->pcm, mem + ab + SHMEM_AUDIO_PCM_OFFSET, aud_bytes);

                uint64_t s1 = seq_load(mem, ab + SHMEM_AUDIO_SEQ_OFFSET);
                if (s1 == s0 && !(s1 & 1u)) {
                    ctx->last_audio_seq = s1;
                    aud_ok = true;
                    break;
                }
            }

            if (aud_ok) {
                /* sample_frames = bytes / (channels * 2 bytes per 16-bit sample) */
                const uint32_t bpf     = aud_ch * 2u;
                const uint32_t frames  = (bpf > 0) ? (aud_bytes / bpf) : 0u;

                if (frames > 0) {
                    struct obs_source_audio audio;
                    memset(&audio, 0, sizeof(audio));

                    audio.format          = AUDIO_FORMAT_16BIT; /* 16-bit signed LE */
                    audio.samples_per_sec = aud_rate;
                    audio.frames          = frames;
                    audio.timestamp       = aud_pts * 1000ULL;  /* us -> ns */
                    audio.data[0]         = ctx->scratch->pcm;  /* interleaved */

                    /* Map channel count to OBS speaker layout */
                    switch (aud_ch) {
                        case 1:  audio.speakers = SPEAKERS_MONO;    break;
                        case 2:  audio.speakers = SPEAKERS_STEREO;  break;
                        case 4:  audio.speakers = SPEAKERS_4POINT0; break;
                        case 6:  audio.speakers = SPEAKERS_5POINT1; break;
                        default: audio.speakers = SPEAKERS_STEREO;  break;
                    }

                    obs_source_output_audio(ctx->source, &audio);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Source info struct
 * -------------------------------------------------------------------------- */
static struct obs_source_info s_sansky_info = {
    .id           = "sansky_iphone_source",
    .type         = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
    .icon_type    = OBS_ICON_TYPE_CAMERA,
    .get_name     = sansky_get_name,
    .create       = sansky_create,
    .destroy      = sansky_destroy,
    .get_width    = sansky_get_width,
    .get_height   = sansky_get_height,
    .video_tick   = sansky_video_tick,
};
