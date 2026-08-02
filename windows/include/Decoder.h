#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <vector>

namespace SanskyStream {

class Decoder {
public:
    Decoder();
    ~Decoder();

    bool InitializeVideoDecoder();
    bool InitializeAudioDecoder();

    // Stubs for decoding processes
    void DecodeVideoPacket(const std::vector<uint8_t>& naluData, uint64_t timestamp);
    void DecodeAudioPacket(const std::vector<uint8_t>& aacData, uint64_t timestamp);

private:
    Microsoft::WRL::ComPtr<IMFTransform> m_videoDecoderMFT;
    Microsoft::WRL::ComPtr<IMFTransform> m_audioDecoderMFT;
};

} // namespace SanskyStream
