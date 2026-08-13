#include <windows.h>
#include "App.h"
#include "Logger.h"

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nShowCmd)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    LOG_INFO("SanskyStream Windows Receiver starting...");
    LOG_INFO("Milestone 2 - Network Receiver.");

    SanskyStream::App app;
    app.Run();

    return 0;
}
