#include "Window.h"
#include "Logger.h"

#pragma comment(lib, "gdi32.lib")

namespace SanskyStream {

Window::Window(int width, int height, const std::wstring& title)
    : m_width(width), m_height(height),
      m_hInstance(GetModuleHandle(nullptr)),
      m_windowClass(L"SanskyStreamWindowClass"),
      m_hwnd(nullptr)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = WindowProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = m_hInstance;
    wc.hIcon         = nullptr;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszMenuName  = nullptr;
    wc.lpszClassName = m_windowClass.c_str();
    wc.hIconSm       = nullptr;

    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("Failed to register window class.");
        return;
    }

    // Adjust window rect so the *client* area matches the requested dimensions
    RECT wr = { 0, 0, m_width, m_height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        m_windowClass.c_str(),
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right  - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        m_hInstance,
        this    // Passed to WindowProc via WM_CREATE so we can store the pointer
    );

    if (!m_hwnd) {
        LOG_ERROR("Failed to create window.");
        return;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    LOG_INFO("Window created successfully.");
}

Window::~Window() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassW(m_windowClass.c_str(), m_hInstance);
    LOG_INFO("Window destroyed.");
}

bool Window::ProcessMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Status text — thread-safe (called from the network thread)
// ---------------------------------------------------------------------------

void Window::SetStatusText(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusText = text;
}

std::string Window::GetStatusText() const {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_statusText;
}

// ---------------------------------------------------------------------------
// GDI status overlay — called from the main thread after D3D Present()
// ---------------------------------------------------------------------------

void Window::DrawStatusOverlay() {
    if (!m_hwnd) return;

    // Snapshot the status string under lock, then release before GDI calls
    std::wstring wStatus;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        // Status text is pure ASCII — widen character-by-character
        wStatus.assign(m_statusText.begin(), m_statusText.end());
    }

    if (wStatus.empty()) return;

    HDC hdc = GetDC(m_hwnd);
    if (!hdc) return;

    // ----- Dark background panel so text is readable over any D3D color -----
    HBRUSH bgBrush = CreateSolidBrush(RGB(8, 12, 24));
    if (bgBrush) {
        RECT bgRect = { 15L, 15L, 430L, 135L };
        FillRect(hdc, &bgRect, bgBrush);
        DeleteObject(bgBrush);
    }

    // ----- Text rendering -----
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(210, 215, 230));

    // Segoe UI gives a clean, modern look
    HFONT hFont = CreateFontW(
        22,                         // Height (logical units)
        0,                          // Width  (0 = auto)
        0, 0,                       // Escapement, Orientation
        FW_NORMAL,                  // Weight
        FALSE, FALSE, FALSE,        // Italic, Underline, StrikeOut
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    if (hFont) {
        HGDIOBJ hOldFont = SelectObject(hdc, hFont);

        RECT textRect = { 25L, 22L, 420L, 128L };
        // DT_EDITCONTROL makes DrawTextW honour \r\n line breaks
        DrawTextW(hdc, wStatus.c_str(), -1, &textRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
    }

    ReleaseDC(m_hwnd, hdc);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        Window* pState = reinterpret_cast<Window*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pState));
    } else {
        Window* pState = reinterpret_cast<Window*>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (pState) {
            switch (uMsg) {
                case WM_DESTROY:
                    PostQuitMessage(0);
                    return 0;
                // Additional message handling (resize, etc.) added in future milestones
            }
        }
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

} // namespace SanskyStream
