#pragma once

#include <windows.h>
#include <string>
#include <mutex>

namespace SanskyStream {

class Window {
public:
    Window(int width, int height, const std::wstring& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Pump Windows messages. Returns false when WM_QUIT is received.
    bool ProcessMessages();

    // Draw the current status text as a GDI overlay on top of the D3D output.
    // Call this from the main thread after Renderer::Render().
    void DrawStatusOverlay();

    // Thread-safe: may be called from the network thread.
    void SetStatusText(const std::string& text);

    // Thread-safe read of the current status text.
    std::string GetStatusText() const;

    // Returns true if the window was resized since the last call.
    // Resets the pending flag. Call from the render thread before Render().
    // newW and newH are set to the new client dimensions when true is returned.
    bool TakeResizePending(int& newW, int& newH);

    HWND GetHWND()  const { return m_hwnd;   }
    int  GetWidth() const { return m_width;  }
    int  GetHeight()const { return m_height; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND         m_hwnd;
    HINSTANCE    m_hInstance;
    std::wstring m_windowClass;
    int          m_width;
    int          m_height;

    // Resize pending — set by WM_SIZE (main thread), consumed by Renderer (main thread).
    // No mutex needed: both WM_SIZE (via DispatchMessage) and Render() run on the main thread.
    bool m_resizePending  = false;
    int  m_pendingWidth   = 0;
    int  m_pendingHeight  = 0;

    std::string        m_statusText;
    mutable std::mutex m_statusMutex;  // Guards m_statusText
};

} // namespace SanskyStream
