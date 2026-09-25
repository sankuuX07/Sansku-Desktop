package com.sanskystream.android

// ---------------------------------------------------------------------------
// Protocol.kt — M18: SanskyStream wire-protocol constants
//
// Mirrors protocol/Protocol.h exactly.
// Both files MUST stay in sync.  If you change a constant here,
// update Protocol.h and vice-versa.
//
// Byte order: LITTLE-ENDIAN for all multi-byte integers.
// ---------------------------------------------------------------------------
object Protocol {

    // -----------------------------------------------------------------------
    // Control / Audio channel — TCP port 5000
    // -----------------------------------------------------------------------

    // PacketHeader magic ('SSTR' in little-endian).
    const val CONTROL_MAGIC: UInt = 0x52545353u

    // Packet type values (PacketType enum in C++).
    const val PACKET_TYPE_VIDEO:   UByte = 1u
    const val PACKET_TYPE_AUDIO:   UByte = 2u
    const val PACKET_TYPE_CONTROL: UByte = 3u

    // PacketHeader size in bytes: magic(4) + type(1) + payloadSize(4) = 9.
    const val PACKET_HEADER_SIZE: Int = 9

    // AudioPayloadHeader size: timestampUs(8).
    const val AUDIO_PAYLOAD_HEADER_SIZE: Int = 8

    // TCP port for control + audio channel.
    const val CONTROL_TCP_PORT: Int = 5000

    // -----------------------------------------------------------------------
    // Video transport channel — UDP port 5001
    // -----------------------------------------------------------------------

    // Magic identifying a SanskyStream video UDP datagram ('SSMV' in LE).
    const val VIDEO_MAGIC: UInt = 0x564D5653u

    // Current video protocol version.
    const val VIDEO_PROTOCOL_VERSION: UByte = 1u

    // The only defined video fragment packet type.
    const val VIDEO_FRAGMENT_TYPE: UByte = 0x01u

    // Flag bit in the flags byte: set when this fragment belongs to a keyframe.
    const val VIDEO_FLAG_KEYFRAME: UByte = 0x01u

    // Fixed size of the VideoFragmentHeader in bytes (matches C++ VIDEO_HEADER_SIZE).
    const val VIDEO_HEADER_SIZE: Int = 31

    // Maximum H.264 payload bytes per UDP datagram (matches VIDEO_MAX_PAYLOAD).
    const val VIDEO_MAX_PAYLOAD: Int = 1300

    // UDP port for the video media channel.
    const val VIDEO_UDP_PORT: Int = 5001

    // -----------------------------------------------------------------------
    // Video header byte offsets (VideoOffset namespace in C++)
    // -----------------------------------------------------------------------
    object VideoOffset {
        const val Magic          = 0   // uint32 (4 bytes)
        const val Version        = 4   // uint8  (1 byte)
        const val PacketType     = 5   // uint8  (1 byte)
        const val Flags          = 6   // uint8  (1 byte)
        const val FrameId        = 7   // uint32 (4 bytes)
        const val PresentationUs = 11  // uint64 (8 bytes)
        const val PacketSeq      = 19  // uint32 (4 bytes)
        const val FragmentIndex  = 23  // uint16 (2 bytes)
        const val FragmentCount  = 25  // uint16 (2 bytes)
        const val PayloadSize    = 27  // uint32 (4 bytes)
        const val Payload        = 31  // variable
    }

    // Sanity bounds.
    const val VIDEO_MAX_FRAGMENTS_PER_FRAME: Int = 1024
    const val VIDEO_MAX_FRAME_SIZE: Int = 4 * 1024 * 1024 // 4 MiB

    // -----------------------------------------------------------------------
    // Audio defaults (AAC-LC)
    // -----------------------------------------------------------------------

    const val AUDIO_DEFAULT_SAMPLE_RATE: Int  = 44100
    const val AUDIO_DEFAULT_CHANNELS:    Int  = 2
    const val AUDIO_AAC_OBJECT_TYPE:     Int  = 2  // AAC-LC

    // -----------------------------------------------------------------------
    // mDNS / DNS-SD discovery
    // -----------------------------------------------------------------------

    const val SERVICE_TYPE:      String = "_sanskystream._tcp"
    const val TXT_VER_KEY:       String = "ver"
    const val TXT_ROLE_KEY:      String = "role"
    const val TXT_PLATFORM_KEY:  String = "platform"
    const val TXT_VER_VALUE:     String = "1"
    const val TXT_ROLE_SENDER:   String = "sender"
    const val TXT_PLATFORM_ANDROID: String = "android"
}
