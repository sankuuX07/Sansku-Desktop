#pragma once

#include "VideoPacket.h"   // CompleteFrame
#include "DecodedFrame.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// H264Decoder
//
// Wraps the Windows Media Foundation H.264 decoder MFT.
//
// Decoder technology: MFT (Media Foundation Transform)
//   - No FFmpeg; no third-party dependencies.
//   - The MFT is located via MFTEnumEx (sync MFTs = software, which run
//     without a D3D device).  Hardware / async MFT (DXVA / D3D11VA) will
//     be added in M8 when the D3D11 device can be shared.
//   - Outputs MFVideoFormat_NV12 in a CPU-accessible IMFMediaBuffer.
//
// Input contract:
//   - CompleteFrame.data is Annex B H.264 (start codes, SPS+PPS on keyframes).
//   - Timestamps are in microseconds (µs).
//
// Initialization strategy (lazy, on first keyframe):
//   The MFT is NOT initialized at construction time.  It is initialized the
//   first time a keyframe is submitted, because:
//     1. Frame dimensions are read from the SPS in the keyframe.
//     2. We cannot decode without SPS/PPS (they are embedded in the keyframe).
//
// Keyframe-first enforcement:
//   Non-keyframes submitted before the first keyframe are discarded with a
//   warning.  Attempting to decode a P-frame without a reference keyframe
//   produces corrupted output; discarding is safer.
//
// Stale frame strategy:
//   A frame is considered stale if its PTS is more than 500 ms behind the
//   most recently decoded frame's PTS.  Stale frames are discarded with a
//   warning, preventing stale video from occupying the decoder after a
//   network pause or reconnect.
//
// Thread-safety: NOT internally synchronized.
//   H264Decoder must be called only from one thread at a time.
//   VideoReceiver calls it from the VideoUdpReceiver receive thread.
//
// ---------------------------------------------------------------------------
class H264Decoder {
public:
    // Called once per successfully decoded frame, from the receive thread.
    // Must return quickly.
    using FrameCallback = std::function<void(DecodedFrame)>;

    explicit H264Decoder(FrameCallback onDecodedFrame);
    ~H264Decoder();

    H264Decoder(const H264Decoder&)            = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Submit a complete reassembled H.264 frame for decoding.
    // Returns true if the frame was accepted; false if rejected (e.g., no
    // keyframe yet, stale, or SPS parse failure).
    bool SubmitFrame(const CompleteFrame& frame);

    // Flush the decoder pipeline.
    // Drains any buffered output, then resets state to Uninitialized.
    // Must be called before reuse after a stream discontinuity.
    void Flush();

    // True after the first keyframe has been successfully processed.
    bool IsInitialized() const { return m_state == State::Running; }

private:
    // -----------------------------------------------------------------------
    // Decoder state machine
    // -----------------------------------------------------------------------
    enum class State {
        Uninitialized,  // Waiting for first keyframe
        Running,        // MFT initialized, accepting frames
        Flushed         // After Flush(); waiting for next keyframe
    };

    // -----------------------------------------------------------------------
    // SPS parsing — minimal bitstream reader
    // -----------------------------------------------------------------------

    // Removes H.264 emulation prevention bytes (00 00 03 -> 00 00)
    // from a NALU payload slice and returns the raw RBSP bytes.
    static std::vector<uint8_t> StripEmulationPreventionBytes(
        const uint8_t* naluPayload, size_t naluPayloadSize);

    // Locate the first NALU of 'naluType' in an Annex B byte stream.
    // On success, sets naluPayload and naluPayloadSize to the bytes
    // AFTER the NALU header byte (i.e., the RBSP content).
    // Returns true on success.
    static bool FindNALU(const uint8_t* data, size_t size, uint8_t naluType,
                         const uint8_t*& naluPayload, size_t& naluPayloadSize);

    // Parse pic_width_in_mbs_minus1 and pic_height_in_map_units_minus1
    // from an SPS NALU payload (after the header byte, EPB intact).
    // Fills outWidth and outHeight with pixel dimensions.
    // Returns false if the SPS cannot be parsed.
    static bool ParseSPSDimensions(const uint8_t* naluPayload, size_t naluPayloadSize,
                                   uint32_t& outWidth, uint32_t& outHeight);

    // -----------------------------------------------------------------------
    // MFT lifecycle
    // -----------------------------------------------------------------------

    // Create and configure the H.264 decoder MFT for the given dimensions.
    bool InitializeMFT(uint32_t width, uint32_t height);

    // Drain all pending output samples from the MFT after ProcessInput.
    // Invokes m_callback for each decoded DecodedFrame produced.
    void DrainOutput(uint32_t frameId, uint64_t presentationUs);

    // Select and set the best NV12 output media type.
    bool SetNV12OutputType();

    // Re-negotiate output type after MF_E_TRANSFORM_STREAM_CHANGE.
    void RenegotiateOutputType();

    // Tear down the MFT and reset state to Uninitialized.
    void Shutdown();

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    FrameCallback                        m_callback;
    Microsoft::WRL::ComPtr<IMFTransform> m_transform;
    State                                m_state           = State::Uninitialized;
    uint32_t                             m_width           = 0;
    uint32_t                             m_height          = 0;
    uint64_t                             m_lastDecodedPtsUs = 0;

    // M13: cached output stream info — populated by InitializeMFT() and
    // re-populated by RenegotiateOutputType().  Avoids a COM query
    // (GetOutputStreamInfo) on every call to DrainOutput().
    MFT_OUTPUT_STREAM_INFO               m_cachedStreamInfo = {};

    // M13: frame counter for periodic diagnostic logging.
    uint64_t                             m_decodedFrameCount = 0;

    // Stale-frame threshold: discard frames more than 500 ms behind the
    // most recently decoded frame.
    static constexpr uint64_t STALE_THRESHOLD_US = 500'000; // 500 ms
};

} // namespace SanskyStream
