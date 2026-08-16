#include "H264Decoder.h"
#include "Logger.h"

#include <mferror.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cassert>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Minimal Exp-Golomb / bitstream reader used for SPS parsing.
// Operates on a pre-processed RBSP vector (emulation prevention bytes removed).
// All reads are MSB-first (H.264 bitstream convention).
// ---------------------------------------------------------------------------
namespace {

class BitReader {
public:
    BitReader(const std::vector<uint8_t>& data)
        : m_data(data), m_byteIndex(0), m_bitIndex(7)
    {}

    // Read a single bit. Returns false and sets error flag on overread.
    bool ReadBit(uint32_t& out) {
        if (m_byteIndex >= m_data.size()) {
            m_error = true;
            out = 0;
            return false;
        }
        out = (m_data[m_byteIndex] >> m_bitIndex) & 1u;
        if (m_bitIndex == 0) {
            m_bitIndex = 7;
            ++m_byteIndex;
        } else {
            --m_bitIndex;
        }
        return true;
    }

    // Read n bits as unsigned integer (n <= 32).
    bool ReadBits(uint32_t n, uint32_t& out) {
        out = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t bit;
            if (!ReadBit(bit)) return false;
            out = (out << 1) | bit;
        }
        return true;
    }

    // Read unsigned Exp-Golomb coded value.
    bool ReadUEV(uint32_t& out) {
        uint32_t leadingZeros = 0;
        uint32_t bit;
        while (true) {
            if (!ReadBit(bit)) return false;
            if (bit == 1) break;
            ++leadingZeros;
            if (leadingZeros > 31) { m_error = true; return false; }
        }
        uint32_t suffix = 0;
        if (leadingZeros > 0) {
            if (!ReadBits(leadingZeros, suffix)) return false;
        }
        out = (1u << leadingZeros) - 1u + suffix;
        return true;
    }

    // Read signed Exp-Golomb coded value (used for some SPS fields).
    bool ReadSEV(int32_t& out) {
        uint32_t raw;
        if (!ReadUEV(raw)) return false;
        // k = raw: if even → -(raw/2), if odd → (raw+1)/2
        out = (raw & 1u) ? static_cast<int32_t>((raw + 1u) / 2u)
                         : -static_cast<int32_t>(raw / 2u);
        return true;
    }

    bool HasError() const { return m_error; }

private:
    const std::vector<uint8_t>& m_data;
    size_t   m_byteIndex;
    uint32_t m_bitIndex;
    bool     m_error = false;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

H264Decoder::H264Decoder(FrameCallback onDecodedFrame)
    : m_callback(std::move(onDecodedFrame))
{
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: MFStartup failed. HRESULT=0x" +
                  [hr]{ char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }());
    } else {
        LOG_INFO("H264Decoder: Media Foundation started.");
    }
}

H264Decoder::~H264Decoder()
{
    Shutdown();
    MFShutdown();
    LOG_INFO("H264Decoder: Media Foundation shut down.");
}

// ---------------------------------------------------------------------------
// Public API — SubmitFrame
// ---------------------------------------------------------------------------

bool H264Decoder::SubmitFrame(const CompleteFrame& frame)
{
    // ------------------------------------------------------------------
    // 1. Keyframe-first enforcement
    // ------------------------------------------------------------------
    if (m_state == State::Uninitialized || m_state == State::Flushed) {
        if (!frame.isKeyframe) {
            LOG_WARN("H264Decoder: Discarding non-keyframe (frame " +
                     std::to_string(frame.frameId) +
                     ") — waiting for keyframe to initialize decoder.");
            return false;
        }
    }

    // ------------------------------------------------------------------
    // 2. Lazy initialization on first keyframe
    // ------------------------------------------------------------------
    if ((m_state == State::Uninitialized || m_state == State::Flushed) &&
        frame.isKeyframe)
    {
        uint32_t width = 0, height = 0;

        const uint8_t* spsPayload = nullptr;
        size_t         spsPayloadSize = 0;

        if (!FindNALU(frame.data.data(), frame.data.size(), 7u,
                      spsPayload, spsPayloadSize)) {
            LOG_WARN("H264Decoder: Keyframe (frame " +
                     std::to_string(frame.frameId) +
                     ") contains no SPS NALU — cannot initialize decoder.");
            return false;
        }

        if (!ParseSPSDimensions(spsPayload, spsPayloadSize, width, height) ||
            width == 0 || height == 0) {
            LOG_WARN("H264Decoder: Failed to parse SPS dimensions from frame " +
                     std::to_string(frame.frameId) + ".");
            return false;
        }

        if (!InitializeMFT(width, height)) {
            // InitializeMFT already logged the error.
            return false;
        }

        m_width  = width;
        m_height = height;
    }

    // ------------------------------------------------------------------
    // 3. Stale frame detection
    // ------------------------------------------------------------------
    if (m_lastDecodedPtsUs > 0 &&
        frame.presentationUs < m_lastDecodedPtsUs &&
        (m_lastDecodedPtsUs - frame.presentationUs) > STALE_THRESHOLD_US)
    {
        LOG_WARN("H264Decoder: Discarding stale frame " +
                 std::to_string(frame.frameId) +
                 " (PTS=" + std::to_string(frame.presentationUs) +
                 " µs, last decoded=" + std::to_string(m_lastDecodedPtsUs) + " µs).");
        return false;
    }

    // ------------------------------------------------------------------
    // 4. Build IMFSample containing the Annex B data
    // ------------------------------------------------------------------
    ComPtr<IMFSample>      sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: MFCreateSample failed. HRESULT=0x" +
                  [hr]{ char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }());
        return false;
    }

    const DWORD dataSize = static_cast<DWORD>(frame.data.size());
    hr = MFCreateMemoryBuffer(dataSize, &buffer);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: MFCreateMemoryBuffer failed. HRESULT=0x" +
                  [hr]{ char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }());
        return false;
    }

    // Copy the frame data into the MF buffer.
    {
        BYTE* bufPtr = nullptr;
        hr = buffer->Lock(&bufPtr, nullptr, nullptr);
        if (FAILED(hr)) {
            LOG_ERROR("H264Decoder: IMFMediaBuffer::Lock failed.");
            return false;
        }
        std::memcpy(bufPtr, frame.data.data(), dataSize);
        buffer->Unlock();
        buffer->SetCurrentLength(dataSize);
    }

    sample->AddBuffer(buffer.Get());

    // Set presentation time (MF uses 100-nanosecond units).
    sample->SetSampleTime(static_cast<LONGLONG>(frame.presentationUs) * 10LL);

    // Mark keyframes so the decoder knows this is a random-access point.
    if (frame.isKeyframe) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    // ------------------------------------------------------------------
    // 5. ProcessInput
    // ------------------------------------------------------------------
    hr = m_transform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: ProcessInput failed for frame " +
                  std::to_string(frame.frameId) + ". HRESULT=0x" +
                  [hr]{ char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }());
        return false;
    }

    // ------------------------------------------------------------------
    // 6. Drain any output the decoder has ready
    // ------------------------------------------------------------------
    DrainOutput(frame.frameId, frame.presentationUs);
    return true;
}

// ---------------------------------------------------------------------------
// Public API — Flush
// ---------------------------------------------------------------------------

void H264Decoder::Flush()
{
    if (m_state != State::Running) {
        m_state = State::Uninitialized;
        return;
    }

    LOG_INFO("H264Decoder: Flushing decoder pipeline.");

    // Drain remaining output (the decoder may have buffered frames).
    HRESULT hr = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        LOG_WARN("H264Decoder: COMMAND_DRAIN failed — flushing anyway.");
    } else {
        // Drain output after the drain command.
        DrainOutput(UINT32_MAX, 0);
    }

    hr = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(hr)) {
        LOG_WARN("H264Decoder: COMMAND_FLUSH failed.");
    }

    m_lastDecodedPtsUs = 0;
    m_state            = State::Flushed;
    LOG_INFO("H264Decoder: Flushed. Awaiting next keyframe.");
}

// ---------------------------------------------------------------------------
// Private — InitializeMFT
// ---------------------------------------------------------------------------

bool H264Decoder::InitializeMFT(uint32_t width, uint32_t height)
{
    LOG_INFO("H264Decoder: Initializing MFT for " +
             std::to_string(width) + "x" + std::to_string(height) + "...");

    // ------------------------------------------------------------------
    // Find a synchronous H.264 video decoder MFT.
    // We request SYNCMFT (software) so no D3D device is needed in M7.
    // M8 will switch to hardware/async with a shared D3D11 device.
    // ------------------------------------------------------------------
    MFT_REGISTER_TYPE_INFO inTypeInfo = {};
    inTypeInfo.guidMajorType = MFMediaType_Video;
    inTypeInfo.guidSubtype   = MFVideoFormat_H264;

    IMFActivate** ppActivates  = nullptr;
    UINT32        numActivates = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inTypeInfo,
        nullptr,
        &ppActivates,
        &numActivates);

    if (FAILED(hr) || numActivates == 0) {
        LOG_ERROR("H264Decoder: No synchronous H.264 decoder MFT found. "
                  "Ensure Windows Media Foundation is installed.");
        if (ppActivates) CoTaskMemFree(ppActivates);
        return false;
    }

    hr = ppActivates[0]->ActivateObject(IID_PPV_ARGS(&m_transform));
    for (UINT32 i = 0; i < numActivates; ++i) {
        ppActivates[i]->Release();
    }
    CoTaskMemFree(ppActivates);

    if (FAILED(hr) || !m_transform) {
        LOG_ERROR("H264Decoder: ActivateObject failed for H.264 MFT.");
        return false;
    }

    // ------------------------------------------------------------------
    // Set input media type: H.264 byte stream, known dimensions.
    // ------------------------------------------------------------------
    ComPtr<IMFMediaType> pInputType;
    hr = MFCreateMediaType(&pInputType);
    if (FAILED(hr)) { LOG_ERROR("H264Decoder: MFCreateMediaType (input) failed."); return false; }

    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_H264);
    MFSetAttributeSize(pInputType.Get(), MF_MT_FRAME_SIZE, width, height);
    pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = m_transform->SetInputType(0, pInputType.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: SetInputType failed. HRESULT=0x" +
                  [hr]{ char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }());
        m_transform.Reset();
        return false;
    }

    // ------------------------------------------------------------------
    // Set output media type: NV12.
    // ------------------------------------------------------------------
    if (!SetNV12OutputType()) {
        m_transform.Reset();
        return false;
    }

    // ------------------------------------------------------------------
    // Begin streaming.
    // ------------------------------------------------------------------
    hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        LOG_WARN("H264Decoder: NOTIFY_BEGIN_STREAMING failed (non-fatal).");
    }

    hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        LOG_WARN("H264Decoder: NOTIFY_START_OF_STREAM failed (non-fatal).");
    }

    m_state = State::Running;
    LOG_INFO("H264Decoder: MFT initialized successfully (" +
             std::to_string(width) + "x" + std::to_string(height) + ").");
    return true;
}

// ---------------------------------------------------------------------------
// Private — SetNV12OutputType
// ---------------------------------------------------------------------------

bool H264Decoder::SetNV12OutputType()
{
    // Enumerate available output types and select NV12.
    for (DWORD idx = 0; ; ++idx) {
        ComPtr<IMFMediaType> pType;
        HRESULT hr = m_transform->GetOutputAvailableType(0, idx, &pType);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        GUID subtype = GUID_NULL;
        pType->GetGUID(MF_MT_SUBTYPE, &subtype);

        if (subtype == MFVideoFormat_NV12) {
            hr = m_transform->SetOutputType(0, pType.Get(), 0);
            if (SUCCEEDED(hr)) {
                LOG_INFO("H264Decoder: Output type set to NV12.");
                return true;
            }
        }
    }

    // NV12 not available as-is — try modifying the first available type.
    ComPtr<IMFMediaType> pBase;
    HRESULT hr = m_transform->GetOutputAvailableType(0, 0, &pBase);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: GetOutputAvailableType(0) failed.");
        return false;
    }

    ComPtr<IMFMediaType> pNV12;
    hr = MFCreateMediaType(&pNV12);
    if (FAILED(hr)) { LOG_ERROR("H264Decoder: MFCreateMediaType (output) failed."); return false; }
    pBase->CopyAllItems(pNV12.Get());
    pNV12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

    hr = m_transform->SetOutputType(0, pNV12.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: SetOutputType (NV12 modified) failed. HRESULT=0x" +
                  [hr]{ char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }());
        return false;
    }
    LOG_INFO("H264Decoder: Output type set to NV12 (modified).");
    return true;
}

// ---------------------------------------------------------------------------
// Private — RenegotiateOutputType
// ---------------------------------------------------------------------------

void H264Decoder::RenegotiateOutputType()
{
    LOG_INFO("H264Decoder: Output format changed — renegotiating output type.");
    if (SetNV12OutputType()) {
        // Update stored dimensions from the new output type.
        ComPtr<IMFMediaType> pCurrent;
        if (SUCCEEDED(m_transform->GetOutputCurrentType(0, &pCurrent))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(pCurrent.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (w > 0 && h > 0) {
                m_width  = w;
                m_height = h;
                LOG_INFO("H264Decoder: New decoded dimensions: " +
                         std::to_string(w) + "x" + std::to_string(h) + ".");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Private — DrainOutput
// ---------------------------------------------------------------------------

void H264Decoder::DrainOutput(uint32_t frameId, uint64_t presentationUs)
{
    // Check if the MFT provides its own output samples.
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    HRESULT hr = m_transform->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        LOG_ERROR("H264Decoder: GetOutputStreamInfo failed.");
        return;
    }

    const bool mftProvidesSamples =
        (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    // Drain in a loop — the MFT may produce multiple output samples
    // per call to ProcessInput (unusual for H.264, but possible).
    while (true) {
        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        outBuf.dwStreamID = 0;

        // Allocate a sample+buffer if the MFT does not provide its own.
        ComPtr<IMFSample>      allocSample;
        ComPtr<IMFMediaBuffer> allocBuffer;
        if (!mftProvidesSamples && streamInfo.cbSize > 0) {
            MFCreateSample(&allocSample);
            MFCreateMemoryBuffer(streamInfo.cbSize, &allocBuffer);
            if (allocSample && allocBuffer) {
                allocSample->AddBuffer(allocBuffer.Get());
                outBuf.pSample = allocSample.Get();
            }
        }

        DWORD processStatus = 0;
        hr = m_transform->ProcessOutput(0, 1, &outBuf, &processStatus);

        // Release any MFT-generated events (usually null).
        if (outBuf.pEvents) {
            outBuf.pEvents->Release();
            outBuf.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // Decoder needs more compressed data before it can produce output.
            // This is expected for the first few frames of a stream.
            if (outBuf.pSample && !mftProvidesSamples) { /* allocSample handles release */ }
            else if (outBuf.pSample) { outBuf.pSample->Release(); }
            break;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Output format changed (e.g., resolution change mid-stream).
            if (outBuf.pSample && !mftProvidesSamples) { /* allocSample handles release */ }
            else if (outBuf.pSample) { outBuf.pSample->Release(); }
            RenegotiateOutputType();
            continue; // Retry ProcessOutput with the new type.
        }

        if (FAILED(hr)) {
            LOG_ERROR("H264Decoder: ProcessOutput failed. HRESULT=0x" +
                      [hr]{ char b[16]; snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(hr)); return std::string(b); }());
            if (outBuf.pSample && !mftProvidesSamples) { /* allocSample handles release */ }
            else if (outBuf.pSample) { outBuf.pSample->Release(); }
            break;
        }

        if (!outBuf.pSample) {
            // No sample produced this iteration.
            break;
        }

        // ------------------------------------------------------------------
        // Extract NV12 pixel data from the output sample.
        // ------------------------------------------------------------------
        {
            ComPtr<IMFSample> outputSample;
            if (mftProvidesSamples) {
                // MFT gave us a sample; wrap it in ComPtr for RAII.
                outputSample.Attach(outBuf.pSample);
                outBuf.pSample = nullptr;
            } else {
                // We allocated the sample; ComPtr already owns it.
                outputSample = allocSample;
            }

            ComPtr<IMFMediaBuffer> contiguous;
            hr = outputSample->ConvertToContiguousBuffer(&contiguous);
            if (FAILED(hr)) {
                LOG_ERROR("H264Decoder: ConvertToContiguousBuffer failed.");
                continue;
            }

            BYTE*  pData    = nullptr;
            DWORD  maxLen   = 0;
            DWORD  curLen   = 0;
            hr = contiguous->Lock(&pData, &maxLen, &curLen);
            if (FAILED(hr)) {
                LOG_ERROR("H264Decoder: IMFMediaBuffer::Lock failed.");
                continue;
            }

            // Validate the buffer size (NV12: width * height * 3/2).
            const DWORD expectedMinSize = m_width * m_height * 3u / 2u;
            if (curLen < expectedMinSize) {
                LOG_WARN("H264Decoder: Output buffer too small (" +
                         std::to_string(curLen) + " bytes, expected >= " +
                         std::to_string(expectedMinSize) + ").");
                contiguous->Unlock();
                continue;
            }

            DecodedFrame decoded;
            decoded.frameId        = frameId;
            decoded.presentationUs = presentationUs;
            decoded.width          = m_width;
            decoded.height         = m_height;
            decoded.nv12Data.assign(pData, pData + curLen);

            contiguous->Unlock();

            m_lastDecodedPtsUs = presentationUs;

            LOG_INFO("H264Decoder: DecodedFrame"
                     " | ID: "     + std::to_string(decoded.frameId) +
                     " | "         + std::to_string(decoded.width) + "x" + std::to_string(decoded.height) +
                     " | NV12: "   + std::to_string(decoded.nv12Data.size()) + " bytes" +
                     " | PTS: "    + std::to_string(decoded.presentationUs) + " \xc2\xb5s");

            if (m_callback) {
                m_callback(std::move(decoded));
            }
        }

        // If no more output is signalled, stop draining.
        if (!(processStatus & MFT_OUTPUT_STATUS_SAMPLE_READY)) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Private — Shutdown
// ---------------------------------------------------------------------------

void H264Decoder::Shutdown()
{
    if (!m_transform) return;

    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_transform.Reset();
    m_state            = State::Uninitialized;
    m_width            = 0;
    m_height           = 0;
    m_lastDecodedPtsUs = 0;
    LOG_INFO("H264Decoder: MFT shut down.");
}

// ---------------------------------------------------------------------------
// SPS parsing helpers
// ---------------------------------------------------------------------------

// static
std::vector<uint8_t> H264Decoder::StripEmulationPreventionBytes(
    const uint8_t* naluPayload, size_t naluPayloadSize)
{
    std::vector<uint8_t> rbsp;
    rbsp.reserve(naluPayloadSize);
    for (size_t i = 0; i < naluPayloadSize; ++i) {
        // Emulation prevention: 00 00 03 → 00 00 (skip the 0x03)
        if (i + 2 < naluPayloadSize &&
            naluPayload[i]     == 0x00 &&
            naluPayload[i + 1] == 0x00 &&
            naluPayload[i + 2] == 0x03)
        {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2; // skip the 0x03 byte
        } else {
            rbsp.push_back(naluPayload[i]);
        }
    }
    return rbsp;
}

// static
bool H264Decoder::FindNALU(const uint8_t* data, size_t size, uint8_t naluType,
                             const uint8_t*& naluPayload, size_t& naluPayloadSize)
{
    // Scan for 4-byte Annex B start code followed by the desired NALU type.
    for (size_t i = 0; i + 5 <= size; ++i) {
        if (data[i]     == 0x00 && data[i + 1] == 0x00 &&
            data[i + 2] == 0x00 && data[i + 3] == 0x01 &&
            (data[i + 4] & 0x1Fu) == naluType)
        {
            const size_t payloadStart = i + 5; // After start code (4B) + NALU header (1B)

            // Find the end of this NALU = next 4-byte start code or end of data.
            size_t payloadEnd = size;
            for (size_t j = payloadStart; j + 4 <= size; ++j) {
                if (data[j]     == 0x00 && data[j + 1] == 0x00 &&
                    data[j + 2] == 0x00 && data[j + 3] == 0x01)
                {
                    payloadEnd = j;
                    break;
                }
            }

            naluPayload     = data + payloadStart;
            naluPayloadSize = payloadEnd - payloadStart;
            return true;
        }
    }
    return false;
}

// static
bool H264Decoder::ParseSPSDimensions(const uint8_t* naluPayload, size_t naluPayloadSize,
                                      uint32_t& outWidth, uint32_t& outHeight)
{
    // naluPayload starts at the byte AFTER the 0x67 NALU header.
    // Layout: profile_idc (1B) | constraint_flags (1B) | level_idc (1B) | RBSP...
    if (naluPayloadSize < 4) return false;

    const uint8_t profile_idc = naluPayload[0];
    // Bytes 1 (constraint_flags) and 2 (level_idc) are not needed for dimensions.

    // Strip emulation prevention bytes from byte 3 onwards (the RBSP proper).
    std::vector<uint8_t> rbsp = StripEmulationPreventionBytes(
        naluPayload + 3, naluPayloadSize - 3);

    BitReader r(rbsp);
    uint32_t u; int32_t s;

    // seq_parameter_set_id
    if (!r.ReadUEV(u)) return false;

    // Extended-profile block (present when profile_idc is one of these values).
    // iPhone uses Baseline (66), so this block is normally absent — but we
    // handle it for robustness in case the profile changes in the future.
    static const uint8_t kExtProfiles[] = {
        100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135
    };
    bool isExtProfile = false;
    for (uint8_t p : kExtProfiles) {
        if (profile_idc == p) { isExtProfile = true; break; }
    }

    if (isExtProfile) {
        uint32_t chroma_format_idc;
        if (!r.ReadUEV(chroma_format_idc)) return false;
        if (chroma_format_idc == 3) {
            uint32_t sep;
            if (!r.ReadBits(1, sep)) return false; // separate_colour_plane_flag
        }
        if (!r.ReadUEV(u)) return false; // bit_depth_luma_minus8
        if (!r.ReadUEV(u)) return false; // bit_depth_chroma_minus8
        uint32_t qp_bypass;
        if (!r.ReadBits(1, qp_bypass)) return false; // qpprime_y_zero_transform_bypass_flag
        uint32_t scaling_present;
        if (!r.ReadBits(1, scaling_present)) return false; // seq_scaling_matrix_present_flag
        if (scaling_present) {
            // Skip scaling lists — up to 12 lists (6 for 4x4, 6 for 8x8).
            // Each list entry is an SE coded delta; use_default flag is 1 bit.
            // For simplicity: if scaling matrix is present, abandon parse.
            // The iPhone encoder does not use scaling matrices in Baseline.
            LOG_WARN("H264Decoder: SPS has scaling matrix — dimension parse aborted.");
            return false;
        }
    }

    // log2_max_frame_num_minus4
    if (!r.ReadUEV(u)) return false;

    // pic_order_cnt_type
    uint32_t poc_type;
    if (!r.ReadUEV(poc_type)) return false;
    if (poc_type == 0) {
        if (!r.ReadUEV(u)) return false; // log2_max_pic_order_cnt_lsb_minus4
    } else if (poc_type == 1) {
        uint32_t b;
        if (!r.ReadBits(1, b)) return false;  // delta_pic_order_always_zero_flag
        if (!r.ReadSEV(s)) return false;       // offset_for_non_ref_pic
        if (!r.ReadSEV(s)) return false;       // offset_for_top_to_bottom_field
        uint32_t num_ref_frames_in_cycle;
        if (!r.ReadUEV(num_ref_frames_in_cycle)) return false;
        if (num_ref_frames_in_cycle > 256) return false; // sanity
        for (uint32_t i = 0; i < num_ref_frames_in_cycle; ++i) {
            if (!r.ReadSEV(s)) return false;
        }
    }
    // poc_type == 2: no extra fields.

    // max_num_ref_frames (H.264 2010+) — note: older specs used num_ref_frames
    if (!r.ReadUEV(u)) return false; // max_num_ref_frames
    uint32_t gaps_allowed;
    if (!r.ReadBits(1, gaps_allowed)) return false; // gaps_in_frame_num_value_allowed_flag

    // These are the fields we need.
    uint32_t pic_width_in_mbs_minus1;
    uint32_t pic_height_in_map_units_minus1;
    if (!r.ReadUEV(pic_width_in_mbs_minus1))       return false;
    if (!r.ReadUEV(pic_height_in_map_units_minus1)) return false;

    uint32_t frame_mbs_only_flag;
    if (!r.ReadBits(1, frame_mbs_only_flag)) return false;

    if (r.HasError()) return false;

    // Width and height in pixels.
    outWidth  = (pic_width_in_mbs_minus1  + 1u) * 16u;
    outHeight = (pic_height_in_map_units_minus1 + 1u) * 16u;
    // For interlaced video (frame_mbs_only_flag == 0), each "map unit" is a
    // field (8 lines), so height doubles.
    if (!frame_mbs_only_flag) {
        outHeight *= 2u;
    }

    return true;
}

} // namespace SanskyStream
