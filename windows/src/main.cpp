#include <iostream>
#include <windows.h>
#include "Protocol.h"
#include "App.h"
#include "Logger.h"
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    LOG_INFO("SanskyStream Windows Receiver starting...");
    LOG_INFO(std::string("Protocol Magic: 0x") + std::to_string(SanskyStream::Protocol::MAGIC_BYTES));
    LOG_INFO("Ready for Milestone 2 - Scaffolding started.");

    SanskyStream::App app;
    app.Run();

    return 0;
}
