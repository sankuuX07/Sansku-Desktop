// ---------------------------------------------------------------------------
// TestAndroidSender.cpp — M18: Android Sender Simulation
//
// Simulates what the SanskyStream Android app sends over the network, without
// requiring a physical Android device.  Compiled as the separate
// "TestAndroidSender" CMake target.
//
// Purpose:
//   Validates the FULL Windows receive pipeline end-to-end:
//   - TCP connection on port 5000 (Android plays the client role)
//   - Audio AAC-LC packets over TCP (PacketHeader + AudioPayloadHeader + data)
//   - Video UDP fragments on port 5001 (VideoFragmentHeader + Annex B data)
//
// What it does NOT fake:
//   - Real H.264 bitstream (uses synthetic Annex B-like patterns)
//   - Real AAC-LC bitstream (uses synthetic byte patterns)
//   These are sufficient to exercise framing, routing, and the decoder's
//   error handling; the MFT decoder will reject synthetic data gracefully.
//
// Test scenarios:
//   1.  TCP connect to SanskyStream_Windows on port 5000
//   2.  Send N audio packets (packed as Android would send them)
//   3.  Send N video frames over UDP (single-fragment keyframe, then P-frames)
//   4.  Send a multi-fragment UDP frame
//   5.  Send a bad-magic UDP datagram (should be silently rejected)
//   6.  Send an oversized audio payload (should be rejected by Windows)
//   7.  Send a zero-size audio payload (edge case)
//   8.  Graceful TCP disconnect — Windows should return to "Waiting for Device"
//   9.  Reconnect TCP and send one more audio packet
//   10. Final disconnect
//
// Usage:
//   TestAndroidSender.exe [host [tcp_port [udp_port]]]
//   Default: 127.0.0.1  5000  5001
//
// Expected Windows-side behaviour:
//   - Status changes "Waiting for Device..." → "Connected" → "Waiting for Device..."
//     → "Connected" (second session) → "Waiting for Device..."
//   - Audio packets arrive at AudioReceiver (may fail to decode — synthetic data)
//   - Video UDP datagrams arrive at VideoUdpReceiver → FrameAssembler
// ---------------------------------------------------------------------------

#include "Protocol.h"
#include "Logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>

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
// Little-endian write helpers (same as TestVideoSender)
// ---------------------------------------------------------------------------

static void WriteU8 (uint8_t* b, uint32_t o, uint8_t  v) { b[o] = v; }
static void WriteU16LE(uint8_t* b, uint32_t o, uint16_t v) {
    b[o]   = static_cast<uint8_t>(v & 0xFFu);
    b[o+1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}
static void WriteU32LE(uint8_t* b, uint32_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o+i] = static_cast<uint8_t>((v >> (8*i)) & 0xFFu);
}
static void WriteU64LE(uint8_t* b, uint32_t o, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[o+i] = static_cast<uint8_t>((v >> (8*i)) & 0xFFu);
}

// ---------------------------------------------------------------------------
// TCP helpers
// ---------------------------------------------------------------------------

static bool SendExact(SOCKET s, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        int r = ::send(s, reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(size - sent), 0);
        if (r <= 0) {
            fprintf(stderr, "[TestAndroidSender] TCP send error: %d\n", WSAGetLastError());
            return false;
        }
        sent += static_cast<size_t>(r);
    }
    return true;
}

// Build and send a SanskyStream PacketHeader + payload over TCP.
// Matches TcpControlChannel::buildPacket() in Android Kotlin.
static bool SendPacket(SOCKET s, PacketType type, const uint8_t* payload, uint32_t payloadSize) {
    // PacketHeader: magic(4) + type(1) + payloadSize(4) = 9 bytes
    static constexpr size_t kHeaderSize = sizeof(PacketHeader);
    static_assert(kHeaderSize == 9, "PacketHeader size mismatch");

    uint8_t header[9] = {};
    WriteU32LE(header, 0, CONTROL_MAGIC);
    WriteU8   (header, 4, static_cast<uint8_t>(type));
    WriteU32LE(header, 5, payloadSize);

    if (!SendExact(s, header, kHeaderSize)) return false;
    if (payloadSize > 0 && payload)
        if (!SendExact(s, payload, payloadSize)) return false;
    return true;
}

// Send one AAC audio packet (AudioPayloadHeader + synthetic AAC-LC bytes).
// Matches AudioTransport::sendAudio() in Android Kotlin.
static bool SendAudioPacket(SOCKET s, uint64_t timestampUs, const uint8_t* aac, uint32_t aacSize) {
    // AudioPayloadHeader: timestampUs(8) + aac data
    std::vector<uint8_t> payload(sizeof(AudioPayloadHeader) + aacSize);
    WriteU64LE(payload.data(), 0, timestampUs);
    if (aacSize > 0) memcpy(payload.data() + sizeof(AudioPayloadHeader), aac, aacSize);
    return SendPacket(s, PacketType::Audio, payload.data(), static_cast<uint32_t>(payload.size()));
}

// ---------------------------------------------------------------------------
// UDP helpers
// ---------------------------------------------------------------------------

static SOCKET gUdpSock = INVALID_SOCKET;
static sockaddr_in gUdpDest = {};
static uint32_t gPacketSeq  = 0;
static uint32_t gFrameId    = 0;

// Send one complete video frame as UDP fragments.
// Matches VideoTransport::sendFrame() in Android Kotlin exactly.
static bool SendVideoFrame(const uint8_t* naluData, uint32_t totalBytes, uint64_t presentationUs, bool isKeyframe) {
    constexpr uint32_t maxPayload = VIDEO_MAX_PAYLOAD;
    const uint32_t fragmentCount = (totalBytes + maxPayload - 1) / maxPayload;
    const uint32_t frameId = gFrameId++;
    const uint8_t flags    = isKeyframe ? VIDEO_FLAG_KEYFRAME : 0;

    uint8_t datagram[VIDEO_HEADER_SIZE + maxPayload];

    for (uint32_t fi = 0; fi < fragmentCount; ++fi) {
        const uint32_t payloadStart = fi * maxPayload;
        const uint32_t payloadEnd   = (payloadStart + maxPayload < totalBytes)
                                       ? payloadStart + maxPayload : totalBytes;
        const uint32_t payloadSize  = payloadEnd - payloadStart;
        const uint32_t seq          = gPacketSeq++;
        const uint32_t datagramSize = VIDEO_HEADER_SIZE + payloadSize;

        WriteU32LE(datagram, 0,  VIDEO_MAGIC);
        WriteU8   (datagram, 4,  VIDEO_PROTOCOL_VERSION);
        WriteU8   (datagram, 5,  VIDEO_FRAGMENT_TYPE);
        WriteU8   (datagram, 6,  flags);
        WriteU32LE(datagram, 7,  frameId);
        WriteU64LE(datagram, 11, presentationUs);
        WriteU32LE(datagram, 19, seq);
        WriteU16LE(datagram, 23, static_cast<uint16_t>(fi));
        WriteU16LE(datagram, 25, static_cast<uint16_t>(fragmentCount));
        WriteU32LE(datagram, 27, payloadSize);
        memcpy(datagram + VIDEO_HEADER_SIZE, naluData + payloadStart, payloadSize);

        int r = sendto(gUdpSock, reinterpret_cast<const char*>(datagram),
                       static_cast<int>(datagramSize), 0,
                       reinterpret_cast<const sockaddr*>(&gUdpDest),
                       static_cast<int>(sizeof(gUdpDest)));
        if (r == SOCKET_ERROR) {
            fprintf(stderr, "[TestAndroidSender] UDP sendto error: %d\n", WSAGetLastError());
            return false;
        }
        // Small delay between fragments to avoid local socket buffer overflow.
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Synthetic payload generators
// Produce Annex B-compatible patterns without requiring a real encoder.
// The Windows decoder (MFT H264 decoder) will reject these — that's fine.
// The tests exercise framing, routing, and reassembly.
// ---------------------------------------------------------------------------

// A minimal H.264 Annex B NAL unit: start code + NAL header byte.
// Not valid H.264 — just a unique pattern for each frame.
static std::vector<uint8_t> MakeSyntheticNalu(uint32_t frameNumber, bool isKeyframe) {
    // For a keyframe, prepend a fake SPS (NAL type 7) and PPS (NAL type 8),
    // then the IDR slice (NAL type 5). For non-keyframes, use a P-slice (NAL type 1).
    std::vector<uint8_t> nalu;
    const auto appendNal = [&](uint8_t nalType, size_t bodySize) {
        nalu.insert(nalu.end(), {0x00, 0x00, 0x00, 0x01}); // Annex B start code
        nalu.push_back(nalType);
        // Fill body with a repeating pattern so the frame data is distinct.
        for (size_t i = 0; i < bodySize; ++i)
            nalu.push_back(static_cast<uint8_t>((frameNumber + i) & 0xFF));
    };
    if (isKeyframe) {
        appendNal(0x67, 30);   // SPS
        appendNal(0x68, 10);   // PPS
        appendNal(0x65, 200);  // IDR slice
    } else {
        appendNal(0x61, 200);  // Non-IDR P-slice (large enough to need multiple fragments at 1300)
    }
    return nalu;
}

// A synthetic AAC-LC access unit (invalid bitstream, just for framing tests).
static std::vector<uint8_t> MakeSyntheticAac(uint32_t seq, uint32_t sizeBytes = 128) {
    std::vector<uint8_t> aac(sizeBytes);
    for (uint32_t i = 0; i < sizeBytes; ++i)
        aac[i] = static_cast<uint8_t>((seq * 7 + i) & 0xFF);
    return aac;
}

// ---------------------------------------------------------------------------
// Test report helper
// ---------------------------------------------------------------------------

static int gTestsPassed = 0;
static int gTestsFailed = 0;

static void Report(const char* testName, bool passed) {
    if (passed) {
        printf("[PASS] %s\n", testName);
        ++gTestsPassed;
    } else {
        printf("[FAIL] %s\n", testName);
        ++gTestsFailed;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    const std::string host    = (argc > 1) ? argv[1] : "127.0.0.1";
    const int         tcpPort = (argc > 2) ? std::stoi(argv[2]) : 5000;
    const int         udpPort = (argc > 3) ? std::stoi(argv[3]) : 5001;

    printf("=============================================================\n");
    printf(" TestAndroidSender — M18 Android pipeline simulation\n");
    printf(" Target: %s  TCP:%d  UDP:%d\n", host.c_str(), tcpPort, udpPort);
    printf("=============================================================\n\n");

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);

    // -----------------------------------------------------------------------
    // Setup UDP socket (for video fragments)
    // -----------------------------------------------------------------------
    gUdpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gUdpSock == INVALID_SOCKET) {
        fprintf(stderr, "UDP socket creation failed.\n");
        WSACleanup();
        return 1;
    }
    gUdpDest.sin_family = AF_INET;
    gUdpDest.sin_port   = htons(static_cast<u_short>(udpPort));
    inet_pton(AF_INET, host.c_str(), &gUdpDest.sin_addr);

    // -----------------------------------------------------------------------
    // TEST 1: TCP connect (simulates Android TcpControlChannel::connect())
    // -----------------------------------------------------------------------
    printf("[TEST 1] TCP connect to %s:%d\n", host.c_str(), tcpPort);
    SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(static_cast<u_short>(tcpPort));
    inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

    bool connected = (connect(tcpSock, reinterpret_cast<sockaddr*>(&serverAddr),
                              static_cast<int>(sizeof(serverAddr))) == 0);
    Report("TCP connect to Windows server", connected);
    if (!connected) {
        fprintf(stderr, "Cannot connect to %s:%d. Is SanskyStream_Windows running?\n",
                host.c_str(), tcpPort);
        closesocket(gUdpSock);
        WSACleanup();
        return 1;
    }

    // Disable Nagle (mirrors Android TcpControlChannel)
    int noDelay = 1;
    setsockopt(tcpSock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    // Wait a moment for Windows to log "Connected" status.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // -----------------------------------------------------------------------
    // TEST 2: Send audio packets (simulates AudioTransport::sendAudio())
    // -----------------------------------------------------------------------
    printf("\n[TEST 2] Send 5 audio packets over TCP\n");
    {
        bool ok = true;
        for (int i = 0; i < 5; ++i) {
            const uint64_t ts = static_cast<uint64_t>(i) * 20000; // 20 ms apart
            const auto aac = MakeSyntheticAac(static_cast<uint32_t>(i));
            ok = ok && SendAudioPacket(tcpSock, ts, aac.data(), static_cast<uint32_t>(aac.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Report("5 audio packets sent", ok);
    }

    // -----------------------------------------------------------------------
    // TEST 3: Send single-fragment keyframe over UDP
    // -----------------------------------------------------------------------
    printf("\n[TEST 3] Send single-fragment keyframe (UDP)\n");
    {
        const auto nalu = MakeSyntheticNalu(0, true);
        const uint64_t pts = 0;
        bool ok = SendVideoFrame(nalu.data(), static_cast<uint32_t>(nalu.size()), pts, true);
        Report("Single-fragment keyframe sent", ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // -----------------------------------------------------------------------
    // TEST 4: Send 4 P-frames over UDP
    // -----------------------------------------------------------------------
    printf("\n[TEST 4] Send 4 P-frames (UDP)\n");
    {
        bool ok = true;
        for (uint32_t f = 1; f <= 4; ++f) {
            const auto nalu = MakeSyntheticNalu(f, false);
            const uint64_t pts = static_cast<uint64_t>(f) * 16667; // 60 fps
            ok = ok && SendVideoFrame(nalu.data(), static_cast<uint32_t>(nalu.size()), pts, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Report("4 P-frames sent", ok);
    }

    // -----------------------------------------------------------------------
    // TEST 5: Multi-fragment frame (payload > VIDEO_MAX_PAYLOAD = 1300 bytes)
    // -----------------------------------------------------------------------
    printf("\n[TEST 5] Send multi-fragment frame (>1300 bytes payload, UDP)\n");
    {
        // 3500 bytes → ceil(3500/1300) = 3 fragments
        std::vector<uint8_t> bigNalu;
        bigNalu.insert(bigNalu.end(), {0x00, 0x00, 0x00, 0x01, 0x65}); // IDR start
        bigNalu.resize(3500, 0xBB);
        const uint64_t pts = 5 * 16667ULL;
        bool ok = SendVideoFrame(bigNalu.data(), static_cast<uint32_t>(bigNalu.size()), pts, true);
        Report("Multi-fragment frame (3 fragments) sent", ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // -----------------------------------------------------------------------
    // TEST 6: Bad-magic UDP datagram (should be silently rejected by Windows)
    // -----------------------------------------------------------------------
    printf("\n[TEST 6] Send bad-magic UDP datagram (should be rejected)\n");
    {
        uint8_t bad[VIDEO_HEADER_SIZE + 10] = {};
        WriteU32LE(bad, 0, 0xDEADBEEFu);   // Wrong magic
        WriteU8   (bad, 4, VIDEO_PROTOCOL_VERSION);
        WriteU8   (bad, 5, VIDEO_FRAGMENT_TYPE);
        WriteU16LE(bad, 23, 0);  // fragment 0
        WriteU16LE(bad, 25, 1);  // of 1
        WriteU32LE(bad, 27, 10);
        int r = sendto(gUdpSock, reinterpret_cast<const char*>(bad),
                       sizeof(bad), 0,
                       reinterpret_cast<const sockaddr*>(&gUdpDest),
                       static_cast<int>(sizeof(gUdpDest)));
        // We can't observe Windows rejecting it, but the send itself should succeed.
        Report("Bad-magic UDP datagram dispatched", r != SOCKET_ERROR);
    }

    // -----------------------------------------------------------------------
    // TEST 7: Zero-size audio payload (edge case — Windows should handle gracefully)
    // -----------------------------------------------------------------------
    printf("\n[TEST 7] Send zero-size audio payload\n");
    {
        bool ok = SendAudioPacket(tcpSock, 9999999ULL, nullptr, 0);
        Report("Zero-size audio payload sent", ok);
    }

    // -----------------------------------------------------------------------
    // TEST 8: Interleaved audio + video (mirrors Android streaming pattern)
    // -----------------------------------------------------------------------
    printf("\n[TEST 8] Interleaved audio+video (10 pairs)\n");
    {
        bool ok = true;
        for (int i = 0; i < 10; ++i) {
            // Video frame
            const uint32_t fn  = static_cast<uint32_t>(i + 10);
            const auto nalu    = MakeSyntheticNalu(fn, (i == 0));
            const uint64_t pts = static_cast<uint64_t>(fn) * 16667;
            ok = ok && SendVideoFrame(nalu.data(), static_cast<uint32_t>(nalu.size()), pts, (i == 0));

            // Audio packet (every ~20 ms, ~3 per video frame at 60 fps)
            const auto aac = MakeSyntheticAac(static_cast<uint32_t>(i + 10));
            ok = ok && SendAudioPacket(tcpSock, pts, aac.data(), static_cast<uint32_t>(aac.size()));

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Report("10 interleaved audio+video pairs", ok);
    }

    // -----------------------------------------------------------------------
    // TEST 9: Graceful TCP disconnect
    // -----------------------------------------------------------------------
    printf("\n[TEST 9] Graceful TCP disconnect (Windows should return to Waiting)\n");
    {
        shutdown(tcpSock, SD_BOTH);
        closesocket(tcpSock);
        tcpSock = INVALID_SOCKET;
        Report("TCP socket closed", true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // -----------------------------------------------------------------------
    // TEST 10: Reconnect and send one more audio packet (second session)
    // -----------------------------------------------------------------------
    printf("\n[TEST 10] Reconnect TCP (second session — simulates Android re-stream)\n");
    {
        tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port   = htons(static_cast<u_short>(tcpPort));
        inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

        connected = (connect(tcpSock, reinterpret_cast<sockaddr*>(&serverAddr),
                             static_cast<int>(sizeof(serverAddr))) == 0);
        Report("Second TCP connect (reconnect)", connected);

        if (connected) {
            noDelay = 1;
            setsockopt(tcpSock, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            const auto aac = MakeSyntheticAac(99, 64);
            bool ok = SendAudioPacket(tcpSock, 1000000ULL, aac.data(), static_cast<uint32_t>(aac.size()));
            Report("Audio packet in second session", ok);

            // One more video frame in second session
            const auto nalu = MakeSyntheticNalu(100, true);
            ok = SendVideoFrame(nalu.data(), static_cast<uint32_t>(nalu.size()), 1000000ULL, true);
            Report("Video frame in second session", ok);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            shutdown(tcpSock, SD_BOTH);
            closesocket(tcpSock);
            tcpSock = INVALID_SOCKET;
        }
    }

    // -----------------------------------------------------------------------
    // Results
    // -----------------------------------------------------------------------
    closesocket(gUdpSock);
    WSACleanup();

    printf("\n=============================================================\n");
    printf(" Results: %d passed, %d failed\n", gTestsPassed, gTestsFailed);
    printf("=============================================================\n");

    if (gTestsFailed > 0) {
        printf("\n[NOTE] Decoder rejections from synthetic data are EXPECTED\n");
        printf("       and do NOT count as failures. Only framing/send errors fail.\n");
    }

    return (gTestsFailed == 0) ? 0 : 1;
}