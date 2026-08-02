#include "Decoder.h"
#include "Logger.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

namespace SanskyStream {

Decoder::Decoder() {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to start Media Foundation.");
    } else {
        LOG_INFO("Media Foundation started.");
    }
}

Decoder::~Decoder() {
    MFShutdown();
    LOG_INFO("Media Foundation shut down.");
}

bool Decoder::InitializeVideoDecoder() {
    LOG_INFO("Initializing Video Decoder (Stub).");
    // TODO: Create H264 Decoder MFT, set input/output media types
    return true;
}

bool Decoder::InitializeAudioDecoder() {
    LOG_INFO("Initializing Audio Decoder (Stub).");
    // TODO: Create AAC Decoder MFT, set input/output media types
    return true;
}

void Decoder::DecodeVideoPacket(const std::vector<uint8_t>& naluData, uint64_t timestamp) {
    (void)naluData;
    (void)timestamp;
    // LOG_INFO("Received video packet of size " + std::to_string(naluData.size()));
    // TODO: Wrap data in IMFSample and pass to MFT
}

void Decoder::DecodeAudioPacket(const std::vector<uint8_t>& aacData, uint64_t timestamp) {
    (void)aacData;
    (void)timestamp;
    // LOG_INFO("Received audio packet of size " + std::to_string(aacData.size()));
    // TODO: Wrap data in IMFSample and pass to MFT
}

} // namespace SanskyStream
