#include "Window.h"
#include "Logger.h"

namespace SanskyStream {

Window::Window(int width, int height, const std::wstring& title) 
    : m_width(width), m_height(height), m_hInstance(GetModuleHandle(nullptr)), m_windowClass(L"SanskyStreamWindowClass") {
    
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = m_hInstance;
    wc.hIcon = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = m_windowClass.c_str();
    wc.hIconSm = nullptr;

    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("Failed to register window class.");
        return;
    }

    // Calculate window size based on desired client area size
    RECT wr = { 0, 0, m_width, m_height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        m_windowClass.c_str(),
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        m_hInstance,
        this // Pass instance to use in WindowProc
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

LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        Window* pState = reinterpret_cast<Window*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pState);
    } else {
        Window* pState = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (pState) {
            switch (uMsg) {
                case WM_DESTROY:
                    PostQuitMessage(0);
                    return 0;
                // Add more message handling here as needed (e.g. resize)
            }
        }
    }
    
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

} // namespace SanskyStream
