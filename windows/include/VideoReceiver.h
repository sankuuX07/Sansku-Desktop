#pragma once
#include "Decoder.h"
#include <memory>
#include <vector>

namespace SanskyStream {

class VideoReceiver {
public:
    VideoReceiver(std::shared_ptr<Decoder> decoder);
    ~VideoReceiver() = default;

    void OnVideoPacketReceived(const std::vector<uint8_t>& payload);

private:
    std::shared_ptr<Decoder> m_decoder;
};

} // namespace SanskyStream
