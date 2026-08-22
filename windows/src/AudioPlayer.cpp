#include "AudioPlayer.h"
#include "Logger.h"

// Full COM/WASAPI headers — only included in the .cpp.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "ole32.lib")

namespace SanskyStream {

namespace {
std::string HrHex(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(hr));
    return std::string(buf);
}
} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer()  = default;
AudioPlayer::~AudioPlayer() { Stop(); }

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool AudioPlayer::Initialize(uint32_t sampleRate, uint32_t channelCount,
                              uint32_t bitsPerSample)
{
    if (m_initialized.load()) {
        LOG_WARN("AudioPlayer: Already initialized.");
        return true;
    }
    if (sampleRate == 0 || channelCount == 0 || bitsPerSample == 0) {
        LOG_ERROR("AudioPlayer: Invalid PCM format parameters.");
        return false;
    }

    m_sampleRate    = sampleRate;
    m_channelCount  = channelCount;
    m_bitsPerSample = bitsPerSample;
    m_blockAlign    = channelCount * (bitsPerSample / 8);

    LOG_INFO("AudioPlayer: Initializing WASAPI (" +
             std::to_string(sampleRate) + " Hz, " +
             std::to_string(channelCount) + " ch, " +
             std::to_string(bitsPerSample) + "-bit)...");

    // ------------------------------------------------------------------
    // COM initialization (main/init thread).
    // Playback thread does its own CoInitializeEx.
    // ------------------------------------------------------------------
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        LOG_ERROR("AudioPlayer: CoInitializeEx failed. HRESULT=0x" + HrHex(hr));
        return false;
    }

    // ------------------------------------------------------------------
    // Get the default audio render endpoint.
    // ------------------------------------------------------------------
    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&pEnum));
    if (FAILED(hr) || !pEnum) {
        LOG_ERROR("AudioPlayer: CoCreateInstance(MMDeviceEnumerator) failed. HRESULT=0x" + HrHex(hr));
        return false;
    }

    IMMDevice* pDevice = nullptr;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pEnum->Release();
    if (FAILED(hr) || !pDevice) {
        LOG_ERROR("AudioPlayer: GetDefaultAudioEndpoint failed. HRESULT=0x" + HrHex(hr));
        return false;
    }
    m_device = pDevice;

    // ------------------------------------------------------------------
    // Activate IAudioClient.
    // ------------------------------------------------------------------
    IAudioClient* pAC = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&pAC));
    if (FAILED(hr) || !pAC) {
        LOG_ERROR("AudioPlayer: IAudioClient activation failed. HRESULT=0x" + HrHex(hr));
        return false;
    }
    m_audioClient = pAC;

    // ------------------------------------------------------------------
    // Determine the format to use for WASAPI initialization.
    //
    // Strategy: In shared mode WASAPI runs at the system's mix format
    // (typically 48 kHz, 32-bit float, stored as WAVEFORMATEXTENSIBLE).
    // Passing any other format — even a valid PCM one — to Initialize()
    // will fail with E_INVALIDARG unless it exactly matches that mix format.
    //
    // The correct approach for shared mode:
    //   1. Call GetMixFormat() to learn the device's authoritative format.
    //   2. Initialize() with exactly that format — guaranteed to succeed.
    //   3. Record the actual sample rate / channels / bit depth for use
    //      in the ring buffer and playback thread.
    //
    // The AACDecoder outputs 16-bit PCM, so any format mismatch between
    // the decoded PCM and the WASAPI mix format is a format mismatch that
    // Windows will handle via its audio engine (software remix).
    // We do NOT perform our own sample-rate conversion in M11.
    // ------------------------------------------------------------------
    WAVEFORMATEX* pMixFormat = nullptr;
    hr = pAC->GetMixFormat(&pMixFormat);
    if (FAILED(hr) || !pMixFormat) {
        LOG_ERROR("AudioPlayer: GetMixFormat() failed. HRESULT=0x" + HrHex(hr));
        return false;
    }

    // Log the actual mix format for diagnostics.
    LOG_INFO("AudioPlayer: Device mix format: " +
             std::to_string(pMixFormat->nSamplesPerSec) + " Hz, " +
             std::to_string(pMixFormat->nChannels) + " ch, " +
             std::to_string(pMixFormat->wBitsPerSample) + "-bit. " +
             "Requested: " + std::to_string(sampleRate) + " Hz, " +
             std::to_string(channelCount) + " ch, " +
             std::to_string(bitsPerSample) + "-bit.");

    // Update our internal format to match WASAPI (for ring buffer sizing).
    m_sampleRate    = pMixFormat->nSamplesPerSec;
    m_channelCount  = pMixFormat->nChannels;
    m_bitsPerSample = pMixFormat->wBitsPerSample;
    m_blockAlign    = pMixFormat->nBlockAlign;

    // ------------------------------------------------------------------
    // Create the WASAPI buffer-ready event.
    // ------------------------------------------------------------------
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        LOG_ERROR("AudioPlayer: CreateEvent failed. Error=" +
                  std::to_string(GetLastError()));
        CoTaskMemFree(pMixFormat);
        return false;
    }
    m_bufferEvent = hEvent;

    // Initialize with the mix format. Buffer period: 40 ms (400000 × 100 ns).
    const REFERENCE_TIME kBufferDuration = 400000LL;
    hr = pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                         kBufferDuration, 0, pMixFormat, nullptr);
    CoTaskMemFree(pMixFormat);  // Always free regardless of outcome.
    pMixFormat = nullptr;

    if (FAILED(hr)) {
        LOG_ERROR("AudioPlayer: IAudioClient::Initialize failed. HRESULT=0x" + HrHex(hr));
        CloseHandle(hEvent); m_bufferEvent = nullptr;
        return false;
    }

    hr = pAC->SetEventHandle(hEvent);
    if (FAILED(hr)) {
        LOG_ERROR("AudioPlayer: SetEventHandle failed. HRESULT=0x" + HrHex(hr));
        CloseHandle(hEvent); m_bufferEvent = nullptr;
        return false;
    }

    hr = pAC->GetBufferSize(&m_wasapiFrames);
    if (FAILED(hr)) {
        LOG_ERROR("AudioPlayer: GetBufferSize failed. HRESULT=0x" + HrHex(hr));
        return false;
    }

    // ------------------------------------------------------------------
    // Get IAudioRenderClient.
    // ------------------------------------------------------------------
    IAudioRenderClient* pRC = nullptr;
    hr = pAC->GetService(__uuidof(IAudioRenderClient),
                         reinterpret_cast<void**>(&pRC));
    if (FAILED(hr) || !pRC) {
        LOG_ERROR("AudioPlayer: GetService(IAudioRenderClient) failed. HRESULT=0x" + HrHex(hr));
        return false;
    }
    m_renderClient = pRC;

    // M13: Allocate ring buffer ~60 ms at the actual mix format rate.
    // Reduced from ~100 ms: cuts audio pipeline latency by ~40 ms.
    // 60 ms is still sufficient to absorb normal WASAPI callback jitter
    // (WASAPI shared mode period ~10-40 ms depending on driver/device).
    // Tradeoff: slightly higher underflow risk if network/decoder stalls for
    // >60 ms; the underflow path fills with silence (no crash).
    const size_t bytesPerSec = static_cast<size_t>(m_sampleRate) * m_blockAlign;
    m_ringCap = ((bytesPerSec * 60 / 1000 + m_blockAlign - 1) / m_blockAlign) * m_blockAlign;
    if (m_ringCap < 4096) m_ringCap = 4096;

    m_ring.assign(m_ringCap, 0);
    m_ringHead = m_ringTail = m_ringUsed = 0;

    m_initialized = true;
    LOG_INFO("AudioPlayer: Ready. WASAPI buffer=" +
             std::to_string(m_wasapiFrames) + " frames (~" +
             std::to_string(m_wasapiFrames * 1000 / m_sampleRate) +
             " ms). Ring buffer=" + std::to_string(m_ringCap) + " bytes (~60 ms). [M13]");
    return true;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

bool AudioPlayer::Start()
{
    if (!m_initialized.load()) {
        LOG_ERROR("AudioPlayer: Start() called before Initialize().");
        return false;
    }
    if (m_playing.load()) {
        LOG_WARN("AudioPlayer: Already playing.");
        return true;
    }

    IAudioClient*       pAC = static_cast<IAudioClient*>(m_audioClient);
    IAudioRenderClient* pRC = static_cast<IAudioRenderClient*>(m_renderClient);

    // Pre-fill with silence to prevent an initial underrun click.
    {
        BYTE* pBuf = nullptr;
        HRESULT hr = pRC->GetBuffer(m_wasapiFrames, &pBuf);
        if (SUCCEEDED(hr)) {
            std::memset(pBuf, 0, static_cast<size_t>(m_wasapiFrames) * m_blockAlign);
            pRC->ReleaseBuffer(m_wasapiFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    HRESULT hr = pAC->Start();
    if (FAILED(hr)) {
        LOG_ERROR("AudioPlayer: IAudioClient::Start failed. HRESULT=0x" + HrHex(hr));
        return false;
    }

    m_playing = true;
    m_thread  = std::thread(&AudioPlayer::PlaybackThread, this);
    LOG_INFO("AudioPlayer: Playback started.");
    return true;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void AudioPlayer::Stop()
{
    // Signal the thread to exit and wake it in case it's blocked on the event.
    bool wasPlaying = m_playing.exchange(false);
    if (m_bufferEvent) {
        SetEvent(static_cast<HANDLE>(m_bufferEvent));
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }

    // Stop the WASAPI stream.
    if (m_audioClient && wasPlaying) {
        static_cast<IAudioClient*>(m_audioClient)->Stop();
    }

    if (wasPlaying) {
        LOG_INFO("AudioPlayer: Stopped. Underflows=" +
                 std::to_string(m_underflowCount.load()) + ", Overflows=" +
                 std::to_string(m_overflowCount.load()) + ", BytesPlayed=" +
                 std::to_string(m_bytesPlayed.load()) + ".");
    }

    // Release COM resources.
    if (m_renderClient) {
        static_cast<IAudioRenderClient*>(m_renderClient)->Release();
        m_renderClient = nullptr;
    }
    if (m_audioClient) {
        static_cast<IAudioClient*>(m_audioClient)->Release();
        m_audioClient = nullptr;
    }
    if (m_device) {
        static_cast<IMMDevice*>(m_device)->Release();
        m_device = nullptr;
    }
    if (m_bufferEvent) {
        CloseHandle(static_cast<HANDLE>(m_bufferEvent));
        m_bufferEvent = nullptr;
    }

    m_initialized = false;
}

// ---------------------------------------------------------------------------
// SubmitPCM — called from network thread
// ---------------------------------------------------------------------------

size_t AudioPlayer::SubmitPCM(const uint8_t* data, size_t size)
{
    if (!data || size == 0 || !m_initialized.load()) return 0;

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    // If incoming data is larger than the ring itself, keep only the tail.
    if (size > m_ringCap) {
        data += (size - m_ringCap);
        size  = m_ringCap;
        m_overflowCount.fetch_add(1);
    }

    // If not enough free space, drop oldest data (overflow policy).
    while (RingFree() < size) {
        const size_t drop =
            ((size - RingFree() + m_blockAlign - 1) / m_blockAlign) * m_blockAlign;
        const size_t actual = (std::min)(drop, m_ringUsed);
        m_ringTail = (m_ringTail + actual) % m_ringCap;
        m_ringUsed -= actual;
        m_overflowCount.fetch_add(1);
    }

    RingWrite(data, size);
    return size;
}

// ---------------------------------------------------------------------------
// PlaybackThread
// ---------------------------------------------------------------------------

void AudioPlayer::PlaybackThread()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsUninit = SUCCEEDED(hr) || hr == S_FALSE;

    IAudioClient*       pAC    = static_cast<IAudioClient*>(m_audioClient);
    IAudioRenderClient* pRC    = static_cast<IAudioRenderClient*>(m_renderClient);
    HANDLE              hEvent = static_cast<HANDLE>(m_bufferEvent);

    while (m_playing.load()) {
        // Wait for WASAPI to signal that buffer space is available.
        DWORD wr = WaitForSingleObject(hEvent, 100 /*ms timeout*/);
        if (!m_playing.load()) break;
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) break;

        UINT32 padding = 0;
        hr = pAC->GetCurrentPadding(&padding);
        if (FAILED(hr)) continue;

        const UINT32 avail = m_wasapiFrames - padding;
        if (avail == 0) continue;

        BYTE* pBuf = nullptr;
        hr = pRC->GetBuffer(avail, &pBuf);
        if (FAILED(hr)) continue;

        const size_t needed = static_cast<size_t>(avail) * m_blockAlign;
        size_t got = 0;
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            got = RingRead(pBuf, needed);
        }

        if (got < needed) {
            std::memset(pBuf + got, 0, needed - got);
            m_underflowCount.fetch_add(1);
        }

        const DWORD flags = (got == 0) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
        pRC->ReleaseBuffer(avail, flags);
        m_bytesPlayed.fetch_add(got);
    }

    if (needsUninit) CoUninitialize();
    LOG_INFO("AudioPlayer: Playback thread exited.");
}

// ---------------------------------------------------------------------------
// Ring buffer helpers
// ---------------------------------------------------------------------------

size_t AudioPlayer::RingFree() const  { return m_ringCap - m_ringUsed; }
size_t AudioPlayer::RingUsed() const  { return m_ringUsed; }

size_t AudioPlayer::RingRead(uint8_t* dest, size_t maxBytes)
{
    const size_t n = (std::min)(maxBytes, m_ringUsed);
    if (n == 0) return 0;

    const size_t chunk1 = (std::min)(n, m_ringCap - m_ringTail);
    std::memcpy(dest, m_ring.data() + m_ringTail, chunk1);
    if (n > chunk1)
        std::memcpy(dest + chunk1, m_ring.data(), n - chunk1);

    m_ringTail = (m_ringTail + n) % m_ringCap;
    m_ringUsed -= n;
    return n;
}

void AudioPlayer::RingWrite(const uint8_t* src, size_t bytes)
{
    if (bytes == 0) return;
    const size_t chunk1 = (std::min)(bytes, m_ringCap - m_ringHead);
    std::memcpy(m_ring.data() + m_ringHead, src, chunk1);
    if (bytes > chunk1)
        std::memcpy(m_ring.data(), src + chunk1, bytes - chunk1);

    m_ringHead = (m_ringHead + bytes) % m_ringCap;
    m_ringUsed += bytes;
}

} // namespace SanskyStream
