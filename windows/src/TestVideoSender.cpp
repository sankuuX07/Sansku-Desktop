// ---------------------------------------------------------------------------
// TestVideoSender.cpp
//
// M6 transport-layer test tool.
// NOT part of the SanskyStream_Windows production build.
// Compiled as the separate "TestVideoSender" CMake target.
//
// Purpose:
//   Sends fake H.264-like UDP datagrams in the exact M6 wire format to
//   the VideoUdpReceiver running in SanskyStream_Windows.  Validates that
//   VideoUdpReceiver parses headers, FrameAssembler reassembles fragments,
//   and VideoReceiver logs the completed frame — all without a real iPhone.
//
// The payload bytes are NOT real H.264.  They are synthetic patterns used
// solely to exercise the transport layer.  This is clearly documented and
// must never be mistaken for an actual encoded video stream.
//
// Test scenarios exercised:
//   1. Single-fragment keyframe  (frame 0)
//   2. Single-fragment P-frame   (frame 1)
//   3. Multi-fragment P-frame    (frame 2, 3 fragments)
//   4. Simulated packet gap      (frame 3, packetSeq skip of 5)
//   5. Duplicate packet          (frame 3, fragment 0 sent twice)
//   6. Invalid magic datagram    (should be silently rejected)
//   7. Zero-payload datagram     (too short, should be rejected)
//
// Usage:
//   TestVideoSender.exe [host [port]]
//   Default: 127.0.0.1 5001
// ---------------------------------------------------------------------------

#include "Protocol.h"
#include "Logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace SanskyStream;
using namespace SanskyStream::Protocol;

// ---------------------------------------------------------------------------
// Little-endian write helpers
// ---------------------------------------------------------------------------

static void WriteU8(uint8_t* buf, uint32_t offset, uint8_t v)
{
    buf[offset] = v;
}

static void WriteU16LE(uint8_t* buf, uint32_t offset, uint16_t v)
{
    buf[offset]     = static_cast<uint8_t>(v & 0xFFu);
    buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

static void WriteU32LE(uint8_t* buf, uint32_t offset, uint32_t v)
{
    buf[offset]     = static_cast<uint8_t>(v & 0xFFu);
    buf[offset + 1] = static_cast<uint8_t>((v >> 8)  & 0xFFu);
    buf[offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

static void WriteU64LE(uint8_t* buf, uint32_t offset, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        buf[offset + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

// ---------------------------------------------------------------------------
// Build one M6 video fragment datagram.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> BuildFragment(
    uint32_t frameId,
    uint64_t presentationUs,
    bool     isKeyframe,
    uint32_t packetSeq,
    uint16_t fragmentIndex,
    uint16_t fragmentCount,
    const uint8_t* payload,
    uint32_t       payloadSize)
{
    std::vector<uint8_t> datagram(VIDEO_HEADER_SIZE + payloadSize);

    WriteU32LE(datagram.data(), VideoOffset::Magic,          VIDEO_MAGIC);
    WriteU8   (datagram.data(), VideoOffset::Version,        VIDEO_PROTOCOL_VERSION);
    WriteU8   (datagram.data(), VideoOffset::PacketType,     VIDEO_FRAGMENT_TYPE);
    uint8_t flags = isKeyframe ? VIDEO_FLAG_KEYFRAME : 0u;
    WriteU8   (datagram.data(), VideoOffset::Flags,          flags);
    WriteU32LE(datagram.data(), VideoOffset::FrameId,        frameId);
    WriteU64LE(datagram.data(), VideoOffset::PresentationUs, presentationUs);
    WriteU32LE(datagram.data(), VideoOffset::PacketSeq,      packetSeq);
    WriteU16LE(datagram.data(), VideoOffset::FragmentIndex,  fragmentIndex);
    WriteU16LE(datagram.data(), VideoOffset::FragmentCount,  fragmentCount);
    WriteU32LE(datagram.data(), VideoOffset::PayloadSize,    payloadSize);

    if (payload && payloadSize > 0) {
        std::memcpy(datagram.data() + VIDEO_HEADER_SIZE, payload, payloadSize);
    }

    return datagram;
}

// ---------------------------------------------------------------------------
// Send a datagram to the target socket
// ---------------------------------------------------------------------------
static bool SendDatagram(SOCKET sock,
                         const sockaddr_in& target,
                         const std::vector<uint8_t>& datagram)
{
    int sent = sendto(sock,
                      reinterpret_cast<const char*>(datagram.data()),
                      static_cast<int>(datagram.size()),
                      0,
                      reinterpret_cast<const sockaddr*>(&target),
                      static_cast<int>(sizeof(target)));
    if (sent == SOCKET_ERROR) {
        LOG_ERROR("TestVideoSender: sendto() failed. WSA error: " +
                  std::to_string(WSAGetLastError()));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fake H.264-like payload generator
// Produces 'size' bytes filled with a recognizable repeating pattern.
// NOT real H.264.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> FakePayload(uint32_t size, uint8_t seed)
{
    std::vector<uint8_t> p(size);
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = static_cast<uint8_t>((seed + i) & 0xFF);
    }
    return p;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    const char* host = "127.0.0.1";
    uint16_t    port = VIDEO_UDP_PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));

    printf("TestVideoSender: target %s:%u\n", host, port);
    printf("NOTE: payload is FAKE — transport-layer test only, not real H.264.\n\n");

    // --- WinSock init ---
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("socket() failed. WSA error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &target.sin_addr) != 1) {
        printf("Invalid host: %s\n", host);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    uint32_t packetSeq = 0;

    // ------------------------------------------------------------------
    // Test 1: Single-fragment KEYFRAME (frame 0)
    // ------------------------------------------------------------------
    {
        uint32_t frameId        = 0;
        uint64_t ptsUs          = 0;        // PTS = 0 µs
        bool     isKeyframe     = true;
        auto     payload        = FakePayload(512, 0xAA);
        auto     datagram       = BuildFragment(frameId, ptsUs, isKeyframe,
                                                packetSeq++,
                                                0, 1,
                                                payload.data(),
                                                static_cast<uint32_t>(payload.size()));
        printf("Sending Test 1: single-fragment keyframe (frame 0, 512 bytes)\n");
        SendDatagram(sock, target, datagram);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ------------------------------------------------------------------
    // Test 2: Single-fragment P-FRAME (frame 1)
    // ------------------------------------------------------------------
    {
        uint32_t frameId    = 1;
        uint64_t ptsUs      = 16667; // ~60 fps → 1/60 s ≈ 16667 µs
        bool     isKeyframe = false;
        auto     payload    = FakePayload(200, 0xBB);
        auto     datagram   = BuildFragment(frameId, ptsUs, isKeyframe,
                                            packetSeq++,
                                            0, 1,
                                            payload.data(),
                                            static_cast<uint32_t>(payload.size()));
        printf("Sending Test 2: single-fragment P-frame (frame 1, 200 bytes)\n");
        SendDatagram(sock, target, datagram);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ------------------------------------------------------------------
    // Test 3: Multi-fragment P-FRAME (frame 2, 3 fragments × 1300 bytes)
    // ------------------------------------------------------------------
    {
        uint32_t frameId    = 2;
        uint64_t ptsUs      = 33333;
        bool     isKeyframe = false;
        uint16_t fragCount  = 3;

        printf("Sending Test 3: multi-fragment P-frame (frame 2, 3 × 1300 bytes)\n");
        for (uint16_t fi = 0; fi < fragCount; ++fi) {
            auto payload  = FakePayload(VIDEO_MAX_PAYLOAD, static_cast<uint8_t>(0xCC + fi));
            auto datagram = BuildFragment(frameId, ptsUs, isKeyframe,
                                          packetSeq++,
                                          fi, fragCount,
                                          payload.data(),
                                          static_cast<uint32_t>(payload.size()));
            SendDatagram(sock, target, datagram);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ------------------------------------------------------------------
    // Test 4: Simulated packet gap — skip 5 sequence numbers
    // Frame 3 fragment 0: packetSeq jumps by 5 from the expected next value.
    // The receiver should log a "missing packets" warning.
    // ------------------------------------------------------------------
    {
        packetSeq += 5; // Simulate 5 dropped datagrams.

        uint32_t frameId    = 3;
        uint64_t ptsUs      = 50000;
        bool     isKeyframe = false;
        auto     payload    = FakePayload(100, 0xDD);
        auto     datagram   = BuildFragment(frameId, ptsUs, isKeyframe,
                                            packetSeq++,
                                            0, 1,
                                            payload.data(),
                                            static_cast<uint32_t>(payload.size()));
        printf("Sending Test 4: packet gap (packetSeq jump +5, frame 3)\n");
        SendDatagram(sock, target, datagram);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ------------------------------------------------------------------
    // Test 5: Duplicate packet — send frame 3 fragment 0 again.
    // The receiver should silently discard it (duplicate by frameId).
    // Frame 3 was already completed in Test 4 (single fragment), so this
    // hits the "already completed" path in FrameAssembler (entry removed).
    // ------------------------------------------------------------------
    {
        uint32_t frameId    = 3;
        uint64_t ptsUs      = 50000;
        bool     isKeyframe = false;
        auto     payload    = FakePayload(100, 0xDD);
        // Resend with a new packetSeq (the old entry is already gone from the
        // assembler, so OOO detection in VideoUdpReceiver handles this).
        // Use the previous packetSeq - 1 to trigger OOO detection.
        uint32_t dupSeq = packetSeq - 2;
        auto datagram   = BuildFragment(frameId, ptsUs, isKeyframe,
                                        dupSeq,
                                        0, 1,
                                        payload.data(),
                                        static_cast<uint32_t>(payload.size()));
        printf("Sending Test 5: duplicate/OOO packet (should be silently dropped)\n");
        SendDatagram(sock, target, datagram);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ------------------------------------------------------------------
    // Test 6: Bad magic — should be rejected by VideoUdpReceiver.
    // ------------------------------------------------------------------
    {
        printf("Sending Test 6: bad magic (should be rejected with WARN)\n");
        std::vector<uint8_t> bad(VIDEO_HEADER_SIZE + 10, 0xFFu);
        // Write a wrong magic value.
        WriteU32LE(bad.data(), VideoOffset::Magic, 0xDEADBEEFu);
        WriteU32LE(bad.data(), VideoOffset::PayloadSize, 10);
        sendto(sock,
               reinterpret_cast<const char*>(bad.data()),
               static_cast<int>(bad.size()),
               0,
               reinterpret_cast<const sockaddr*>(&target),
               static_cast<int>(sizeof(target)));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ------------------------------------------------------------------
    // Test 7: Too-short datagram — should be rejected.
    // ------------------------------------------------------------------
    {
        printf("Sending Test 7: too-short datagram (should be rejected with WARN)\n");
        std::vector<uint8_t> tiny(4, 0x00u);
        sendto(sock,
               reinterpret_cast<const char*>(tiny.data()),
               static_cast<int>(tiny.size()),
               0,
               reinterpret_cast<const sockaddr*>(&target),
               static_cast<int>(sizeof(target)));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    printf("\nTestVideoSender: all test datagrams sent.\n");
    printf("Check SanskyStream_Windows log output for frame confirmations.\n");

    closesocket(sock);
    WSACleanup();
    return 0;
}
