// ---------------------------------------------------------------------------
// TestReceiver.cpp
//
// Headless console test harness for M6 Windows-side transport pipeline.
// NOT part of SanskyStream_Windows production build.
//
// Links: VideoUdpReceiver, FrameAssembler, VideoReceiver, Logger.
// Does NOT link: Window, Renderer, Network, Decoder, AudioReceiver.
//
// Usage:
//   TestReceiver.exe [port]   (default: 5001)
//
// Run this first, then run TestVideoSender.exe in another terminal.
// The receiver will log all decoded-frame events to stdout and exit after
// a short idle timeout once all test datagrams have been processed.
// ---------------------------------------------------------------------------

#include "VideoUdpReceiver.h"
#include "VideoReceiver.h"
#include "Logger.h"
#include "Protocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

using namespace SanskyStream;

int main(int argc, char* argv[])
{
    uint16_t port = Protocol::VIDEO_UDP_PORT;
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    // WinSock init
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    printf("TestReceiver: listening on UDP port %u\n", port);
    printf("Run TestVideoSender.exe in another terminal now.\n\n");

    VideoReceiver receiver;

    VideoUdpReceiver udpReceiver(
        [&receiver](CompleteFrame frame) {
            receiver.OnCompleteFrame(std::move(frame));
        });

    if (!udpReceiver.Start(port)) {
        printf("Failed to start VideoUdpReceiver.\n");
        WSACleanup();
        return 1;
    }

    // Wait for 4 seconds to receive all test datagrams, then exit cleanly.
    std::this_thread::sleep_for(std::chrono::seconds(4));

    udpReceiver.Stop();
    WSACleanup();
    printf("\nTestReceiver: done.\n");
    return 0;
}
