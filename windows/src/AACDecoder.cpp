#include "AACDecoder.h"
#include "Logger.h"

#include <mferror.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

std::string HrHex(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}

// ISO 14496-3 sampling frequency table (indices 0-12).
uint8_t SamplingFreqIndex(uint32_t rate) {
    static const uint32_t kTable[] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350
    };
    for (uint8_t i = 0; i < 13; ++i) {
        if (kTable[i] == rate) return i;
    }
    return 0x0F; // explicit frequency (not used in 2-byte ASC, but safe fallback)
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AACDecoder::AACDecoder(DecodedCallback onDecoded)
    : m_callback(std::move(onDecoded))
{
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        LOG_ERROR("AACDecoder: MFStartup failed. HRESULT=0x" + HrHex(hr));
    } else {
        LOG_INFO("AACDecoder: Media Foundation started.");
    }
}

AACDecoder::~AACDecoder()
{
    if (m_transform) {
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_transform.Reset();
    }
    MFShutdown();
    LOG_INFO("AACDecoder: Media Foundation shut down.");
}

// ---------------------------------------------------------------------------
// BuildAudioSpecificConfig
//
// Produces 2 bytes for AAC-LC AudioSpecificConfig (ISO 14496-3 §1.6.5):
//
//   Bits [15-11] audioObjectType    = 2 (AAC-LC)
//   Bits [10-7]  samplingFreqIndex
//   Bits [6-3]   channelConfig
//   Bits [2-0]   padding = 0
// ---------------------------------------------------------------------------
// static
std::vector<uint8_t> AACDecoder::BuildAudioSpecificConfig(uint32_t sampleRate,
                                                            uint32_t channelCount)
{
    const uint8_t objType  = 2; // AAC-LC
    const uint8_t freqIdx  = SamplingFreqIndex(sampleRate);
    const uint8_t chanCfg  = static_cast<uint8_t>(channelCount & 0x0F);

    // Pack: [objType(5)] [freqIdx(4)] [chanCfg(4)] → 13 significant bits → 2 bytes
    uint8_t b0 = static_cast<uint8_t>((objType << 3) | (freqIdx >> 1));
    uint8_t b1 = static_cast<uint8_t>(((freqIdx & 0x01) << 7) | (chanCfg << 3));
    return { b0, b1 };
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool AACDecoder::Initialize(uint32_t sampleRate, uint32_t channelCount)
{
    if (m_initialized) {
        LOG_WARN("AACDecoder: Already initialized.");
        return true;
    }
    if (sampleRate == 0 || channelCount == 0) {
        LOG_ERROR("AACDecoder: Invalid format parameters.");
        return false;
    }

    m_sampleRate   = sampleRate;
    m_channelCount = channelCount;

    LOG_INFO("AACDecoder: Initializing for " + std::to_string(sampleRate) +
             " Hz, " + std::to_string(channelCount) + " ch...");

    // ------------------------------------------------------------------
    // Find a synchronous AAC audio decoder MFT.
    // ------------------------------------------------------------------
    MFT_REGISTER_TYPE_INFO inInfo = {};
    inInfo.guidMajorType = MFMediaType_Audio;
    inInfo.guidSubtype   = MFAudioFormat_AAC;

    IMFActivate** ppAct = nullptr;
    UINT32        nAct  = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_AUDIO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inInfo, nullptr, &ppAct, &nAct);

    if (FAILED(hr) || nAct == 0) {
        LOG_ERROR("AACDecoder: No AAC decoder MFT found. "
                  "Windows Media Foundation must be installed.");
        if (ppAct) CoTaskMemFree(ppAct);
        return false;
    }

    hr = ppAct[0]->ActivateObject(IID_PPV_ARGS(&m_transform));
    for (UINT32 i = 0; i < nAct; ++i) ppAct[i]->Release();
    CoTaskMemFree(ppAct);

    if (FAILED(hr) || !m_transform) {
        LOG_ERROR("AACDecoder: ActivateObject failed. HRESULT=0x" + HrHex(hr));
        return false;
    }

    // ------------------------------------------------------------------
    // Set input type: AAC with AudioSpecificConfig in MF_MT_USER_DATA.
    //
    // The Windows AAC MFT expects MF_MT_USER_DATA to be a HEAACWAVEINFO
    // structure followed by the AudioSpecificConfig bytes.
    // HEAACWAVEINFO header = 12 bytes (wPayloadType, wAudioProfileLevelIndication,
    // wStructType, wReserved1, dwReserved2).
    // ------------------------------------------------------------------
    ComPtr<IMFMediaType> pIn;
    hr = MFCreateMediaType(&pIn);
    if (FAILED(hr)) { LOG_ERROR("AACDecoder: MFCreateMediaType(in) failed."); m_transform.Reset(); return false; }

    pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pIn->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_AAC);
    pIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    pIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        channelCount);
    pIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     16);
    pIn->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,          0); // 0 = raw AAC frames

    std::vector<uint8_t> asc = BuildAudioSpecificConfig(sampleRate, channelCount);
    // HEAACWAVEINFO prefix (12 bytes, all zero) + ASC
    std::vector<uint8_t> userData(12 + asc.size(), 0);
    std::memcpy(userData.data() + 12, asc.data(), asc.size());
    pIn->SetBlob(MF_MT_USER_DATA, userData.data(), static_cast<UINT32>(userData.size()));

    hr = m_transform->SetInputType(0, pIn.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("AACDecoder: SetInputType failed. HRESULT=0x" + HrHex(hr));
        m_transform.Reset();
        return false;
    }

    // ------------------------------------------------------------------
    // Set output type: 16-bit PCM.
    // Enumerate available types, pick the first MFAudioFormat_PCM.
    // ------------------------------------------------------------------
    bool outputSet = false;
    for (DWORD idx = 0; ; ++idx) {
        ComPtr<IMFMediaType> pAvail;
        hr = m_transform->GetOutputAvailableType(0, idx, &pAvail);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        GUID sub = GUID_NULL;
        pAvail->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFAudioFormat_PCM) {
            // Override to ensure 16-bit output at the correct rate/channels.
            pAvail->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     16);
            pAvail->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
            pAvail->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        channelCount);
            const UINT32 blk = channelCount * 2; // 16-bit = 2 bytes/ch
            pAvail->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,      blk);
            pAvail->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * blk);

            hr = m_transform->SetOutputType(0, pAvail.Get(), 0);
            if (SUCCEEDED(hr)) {
                LOG_INFO("AACDecoder: Output set to PCM 16-bit.");
                outputSet = true;
                break;
            }
        }
    }

    if (!outputSet) {
        // Fallback: build a PCM type from scratch.
        ComPtr<IMFMediaType> pOut;
        hr = MFCreateMediaType(&pOut);
        if (FAILED(hr)) { LOG_ERROR("AACDecoder: MFCreateMediaType(out) failed."); m_transform.Reset(); return false; }

        const UINT32 blk = channelCount * 2;
        pOut->SetGUID(MF_MT_MAJOR_TYPE,               MFMediaType_Audio);
        pOut->SetGUID(MF_MT_SUBTYPE,                  MFAudioFormat_PCM);
        pOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        pOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        channelCount);
        pOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     16);
        pOut->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,     blk);
        pOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * blk);

        hr = m_transform->SetOutputType(0, pOut.Get(), 0);
        if (FAILED(hr)) {
            LOG_ERROR("AACDecoder: SetOutputType(fallback) failed. HRESULT=0x" + HrHex(hr));
            m_transform.Reset();
            return false;
        }
        LOG_INFO("AACDecoder: Output set to PCM 16-bit (fallback).");
    }

    // ------------------------------------------------------------------
    // Begin streaming.
    // ------------------------------------------------------------------
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_initialized = true;
    LOG_INFO("AACDecoder: Ready (" + std::to_string(sampleRate) + " Hz, " +
             std::to_string(channelCount) + " ch, 16-bit PCM output).");
    return true;
}

// ---------------------------------------------------------------------------
// SubmitPacket
// ---------------------------------------------------------------------------

bool AACDecoder::SubmitPacket(const uint8_t* aacData, size_t aacSize,
                               uint64_t timestampUs)
{
    if (!m_initialized || !m_transform) {
        LOG_WARN("AACDecoder: SubmitPacket called before Initialize().");
        return false;
    }
    if (!aacData || aacSize == 0) {
        LOG_WARN("AACDecoder: Empty AAC packet — skipping.");
        return false;
    }

    // Build IMFSample.
    ComPtr<IMFSample>      sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) { LOG_ERROR("AACDecoder: MFCreateSample failed. HRESULT=0x" + HrHex(hr)); return false; }

    hr = MFCreateMemoryBuffer(static_cast<DWORD>(aacSize), &buffer);
    if (FAILED(hr)) { LOG_ERROR("AACDecoder: MFCreateMemoryBuffer failed. HRESULT=0x" + HrHex(hr)); return false; }

    {
        BYTE* p = nullptr;
        hr = buffer->Lock(&p, nullptr, nullptr);
        if (FAILED(hr)) { LOG_ERROR("AACDecoder: Buffer lock failed."); return false; }
        std::memcpy(p, aacData, aacSize);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(aacSize));
    }

    sample->AddBuffer(buffer.Get());
    // MF uses 100-nanosecond units; timestamps arrive in microseconds.
    sample->SetSampleTime(static_cast<LONGLONG>(timestampUs) * 10LL);

    hr = m_transform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("AACDecoder: ProcessInput failed. HRESULT=0x" + HrHex(hr));
        return false;
    }

    DrainOutput(timestampUs);
    return true;
}

// ---------------------------------------------------------------------------
// DrainOutput
// ---------------------------------------------------------------------------

void AACDecoder::DrainOutput(uint64_t timestampUs)
{
    MFT_OUTPUT_STREAM_INFO si = {};
    HRESULT hr = m_transform->GetOutputStreamInfo(0, &si);
    if (FAILED(hr)) { LOG_ERROR("AACDecoder: GetOutputStreamInfo failed."); return; }

    const bool mftOwns =
        (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                       MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    while (true) {
        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        ComPtr<IMFSample>      allocSample;
        ComPtr<IMFMediaBuffer> allocBuf;

        if (!mftOwns) {
            DWORD sz = (si.cbSize > 0) ? si.cbSize : 8192;
            MFCreateSample(&allocSample);
            MFCreateMemoryBuffer(sz, &allocBuf);
            if (allocSample && allocBuf) {
                allocSample->AddBuffer(allocBuf.Get());
                outBuf.pSample = allocSample.Get();
            }
        }

        DWORD status = 0;
        hr = m_transform->ProcessOutput(0, 1, &outBuf, &status);

        if (outBuf.pEvents) { outBuf.pEvents->Release(); outBuf.pEvents = nullptr; }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outBuf.pSample && mftOwns) outBuf.pSample->Release();
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (outBuf.pSample && mftOwns) outBuf.pSample->Release();
            // Accept the new output type automatically.
            ComPtr<IMFMediaType> pNewType;
            if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &pNewType))) {
                m_transform->SetOutputType(0, pNewType.Get(), 0);
                UINT32 sr = 0, ch = 0;
                pNewType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                pNewType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                if (sr > 0) m_sampleRate   = sr;
                if (ch > 0) m_channelCount = ch;
                LOG_INFO("AACDecoder: Format changed to " +
                         std::to_string(m_sampleRate) + " Hz, " +
                         std::to_string(m_channelCount) + " ch.");
            }
            continue;
        }
        if (FAILED(hr)) {
            LOG_ERROR("AACDecoder: ProcessOutput failed. HRESULT=0x" + HrHex(hr));
            if (outBuf.pSample && mftOwns) outBuf.pSample->Release();
            break;
        }
        if (!outBuf.pSample) break;

        // Extract PCM.
        {
            ComPtr<IMFSample> out;
            if (mftOwns) { out.Attach(outBuf.pSample); outBuf.pSample = nullptr; }
            else         { out = allocSample; }

            ComPtr<IMFMediaBuffer> cont;
            if (FAILED(out->ConvertToContiguousBuffer(&cont))) {
                LOG_ERROR("AACDecoder: ConvertToContiguousBuffer failed."); continue;
            }

            BYTE* pData = nullptr; DWORD curLen = 0;
            if (FAILED(cont->Lock(&pData, nullptr, &curLen))) {
                LOG_ERROR("AACDecoder: Buffer lock failed."); continue;
            }

            if (curLen > 0 && pData) {
                const uint32_t frameBytes = m_channelCount * (m_bitsPerSample / 8);
                DecodedAudioPacket pkt;
                pkt.pcmData.assign(pData, pData + curLen);
                pkt.sampleRate    = m_sampleRate;
                pkt.channelCount  = m_channelCount;
                pkt.bitsPerSample = m_bitsPerSample;
                pkt.timestampUs   = timestampUs;
                pkt.sampleCount   = (frameBytes > 0) ? (curLen / frameBytes) : 0;
                cont->Unlock();

                if (m_callback) m_callback(std::move(pkt));
            } else {
                cont->Unlock();
            }
        }

        if (!(status & MFT_OUTPUT_STATUS_SAMPLE_READY)) break;
    }
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

void AACDecoder::Flush()
{
    if (!m_initialized || !m_transform) return;
    LOG_INFO("AACDecoder: Flushing.");

    HRESULT hr = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (SUCCEEDED(hr)) DrainOutput(0);

    m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    LOG_INFO("AACDecoder: Flushed.");
}

} // namespace SanskyStream
