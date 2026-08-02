#include "AudioReceiver.h"
#include "Protocol.h"
#include "Logger.h"

namespace SanskyStream {

AudioReceiver::AudioReceiver(std::shared_ptr<Decoder> decoder) : m_decoder(decoder) {
}

void AudioReceiver::OnAudioPacketReceived(const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(Protocol::AudioPayloadHeader)) return;

    const Protocol::AudioPayloadHeader* header = reinterpret_cast<const Protocol::AudioPayloadHeader*>(payload.data());
    
    std::vector<uint8_t> aacData(payload.begin() + sizeof(Protocol::AudioPayloadHeader), payload.end());
    
    if (m_decoder) {
        m_decoder->DecodeAudioPacket(aacData, header->timestamp);
    }
}

} // namespace SanskyStream
