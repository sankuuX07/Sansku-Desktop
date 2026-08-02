#pragma once

#include <windows.h>
#include <string>

namespace SanskyStream {

class Window {
public:
    Window(int width, int height, const std::wstring& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool ProcessMessages();
    HWND GetHWND() const { return m_hwnd; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd;
    HINSTANCE m_hInstance;
    std::wstring m_windowClass;
    int m_width;
    int m_height;
};

} // namespace SanskyStream
