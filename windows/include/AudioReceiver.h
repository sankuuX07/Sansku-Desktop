#pragma once
#include "Decoder.h"
#include <memory>
#include <vector>

namespace SanskyStream {

class AudioReceiver {
public:
    AudioReceiver(std::shared_ptr<Decoder> decoder);
    ~AudioReceiver() = default;

    void OnAudioPacketReceived(const std::vector<uint8_t>& payload);

private:
    std::shared_ptr<Decoder> m_decoder;
};

} // namespace SanskyStream
