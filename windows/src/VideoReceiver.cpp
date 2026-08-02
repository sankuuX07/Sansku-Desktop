#include "VideoReceiver.h"
#include "Protocol.h"
#include "Logger.h"

namespace SanskyStream {

VideoReceiver::VideoReceiver(std::shared_ptr<Decoder> decoder) : m_decoder(decoder) {
}

void VideoReceiver::OnVideoPacketReceived(const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(Protocol::VideoPayloadHeader)) return;

    const Protocol::VideoPayloadHeader* header = reinterpret_cast<const Protocol::VideoPayloadHeader*>(payload.data());
    
    std::vector<uint8_t> naluData(payload.begin() + sizeof(Protocol::VideoPayloadHeader), payload.end());
    
    if (m_decoder) {
        m_decoder->DecodeVideoPacket(naluData, header->timestamp);
    }
}

} // namespace SanskyStream
