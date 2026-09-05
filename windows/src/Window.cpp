// ---------------------------------------------------------------------------
// Window.cpp — M17: Win32 host window with integrated UI panel
//
// New in M17:
//   - Bottom 160-px panel with LISTBOX, BUTTON, EDIT, STATIC child controls
//   - WM_COMMAND routing to UICallbacks
//   - Settings persistence: %APPDATA%\SanskyStream\settings.ini
//   - DrawStatusOverlay rewritten to render stat bar inside panel
// ---------------------------------------------------------------------------

#include "Window.h"
#include "Logger.h"

#include <algorithm>
#include <cassert>
#include <shlobj.h>   // SHGetFolderPathW
#include <cstdio>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

namespace SanskyStream {

// ---------------------------------------------------------------------------
// Font helpers — create once per DrawStatusOverlay call, delete after use.
// ---------------------------------------------------------------------------
namespace {

// Standard UI font size used throughout the panel.
static constexpr int kPanelFontSize = 14;
static constexpr int kStatFontSize  = 13;

// Background colours.
static constexpr COLORREF kPanelBg   = RGB(15,  18,  30);   // dark navy
static constexpr COLORREF kListBg    = RGB(22,  26,  42);   // slightly lighter
static constexpr COLORREF kStatBg    = RGB(10,  13,  22);   // darkest strip
static constexpr COLORREF kFgNormal  = RGB(210, 215, 230);
static constexpr COLORREF kFgGreen   = RGB( 80, 210, 130);
static constexpr COLORREF kFgOrange  = RGB(240, 160,  50);
static constexpr COLORREF kFgRed     = RGB(220,  80,  80);

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Window::Window(int width, int height, const std::wstring& title)
    : m_width(width), m_height(height),
      m_hInstance(GetModuleHandle(nullptr)),
      m_windowClass(L"SanskyStreamWindowClass_M17")
{
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = m_hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kPanelBg);
    wc.lpszClassName = m_windowClass.c_str();

    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("Failed to register window class.");
        return;
    }

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
        nullptr, nullptr,
        m_hInstance,
        this);

    if (!m_hwnd) {
        LOG_ERROR("Failed to create window.");
        return;
    }

    CreateChildControls();
    LoadSettings();

    ShowWindow(m_hwnd, SW_SHOW);
    LOG_INFO("Window created (M17 UI panel active).");
}

// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------

Window::~Window() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassW(m_windowClass.c_str(), m_hInstance);
    LOG_INFO("Window destroyed.");
}

// ---------------------------------------------------------------------------
// Child control creation
// ---------------------------------------------------------------------------

void Window::CreateChildControls() {
    if (!m_hwnd) return;

    // Fetch a GDI font for the controls (Segoe UI 14pt).
    HFONT hFont = CreateFontW(
        kPanelFontSize + 2, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    auto applyFont = [&](HWND h) {
        if (h && hFont) SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    };

    // Device list-box (left half of panel, excluding stat bar).
    m_hDeviceList = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 1, 1,   // sized in LayoutChildControls
        m_hwnd, (HMENU)(INT_PTR)ID_DEVICE_LIST,
        m_hInstance, nullptr);
    applyFont(m_hDeviceList);

    // Connect button.
    m_hBtnConnect = CreateWindowExW(
        0, L"BUTTON", L"Connect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 1, 1,
        m_hwnd, (HMENU)(INT_PTR)ID_BTN_CONNECT,
        m_hInstance, nullptr);
    applyFont(m_hBtnConnect);

    // Disconnect button.
    m_hBtnDisconn = CreateWindowExW(
        0, L"BUTTON", L"Disconnect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 1, 1,
        m_hwnd, (HMENU)(INT_PTR)ID_BTN_DISCONN,
        m_hInstance, nullptr);
    applyFont(m_hBtnDisconn);

    // IP label.
    m_hStaticIP = CreateWindowExW(
        0, L"STATIC", L"IP:",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 1, 1,
        m_hwnd, nullptr, m_hInstance, nullptr);
    applyFont(m_hStaticIP);

    // IP edit.
    m_hEditIP = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 1, 1,
        m_hwnd, (HMENU)(INT_PTR)ID_EDIT_IP,
        m_hInstance, nullptr);
    applyFont(m_hEditIP);
    SendMessage(m_hEditIP, EM_SETLIMITTEXT, 64, 0);

    // Port label.
    m_hStaticPort = CreateWindowExW(
        0, L"STATIC", L"Port:",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 1, 1,
        m_hwnd, nullptr, m_hInstance, nullptr);
    applyFont(m_hStaticPort);

    // Port edit.
    m_hEditPort = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"5000",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
        0, 0, 1, 1,
        m_hwnd, (HMENU)(INT_PTR)ID_EDIT_PORT,
        m_hInstance, nullptr);
    applyFont(m_hEditPort);
    SendMessage(m_hEditPort, EM_SETLIMITTEXT, 5, 0);

    // Status label.
    m_hStaticStatus = CreateWindowExW(
        0, L"STATIC", L"Waiting for device...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 1, 1,
        m_hwnd, nullptr, m_hInstance, nullptr);
    applyFont(m_hStaticStatus);

    // Initial layout at construction size.
    LayoutChildControls(m_width, m_height);
}

// ---------------------------------------------------------------------------
// LayoutChildControls — called on construction and every WM_SIZE.
//
// Panel occupies bottom kPanelHeight px of the client rect.
//   Left half  (~55%): device list.
//   Right half (~45%): buttons, IP/port row, status label.
//   Bottom kStatBarHeight: stat bar (painted by DrawStatusOverlay, no child).
// ---------------------------------------------------------------------------

void Window::LayoutChildControls(int cW, int cH) {
    if (cW <= 0 || cH <= 0) return;

    const int panelTop  = cH - kPanelHeight;
    const int listH     = kPanelHeight - kStatBarHeight;
    const int listW     = (cW * 55) / 100;
    const int rightX    = listW + 6;
    const int rightW    = cW - rightX - 4;

    // Margins inside the right column.
    const int rTop  = panelTop + 6;
    const int btnH  = 28;
    const int btnW  = (rightW - 6) / 2;
    const int rowH  = 22;
    const int gap   = 6;
    const int lblW  = 36;
    const int editW = rightW - lblW - gap;

    // Device list.
    if (m_hDeviceList)
        SetWindowPos(m_hDeviceList, nullptr,
                     2, panelTop,
                     listW, listH,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    // Connect / Disconnect buttons (side-by-side).
    if (m_hBtnConnect)
        SetWindowPos(m_hBtnConnect, nullptr,
                     rightX, rTop,
                     btnW, btnH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_hBtnDisconn)
        SetWindowPos(m_hBtnDisconn, nullptr,
                     rightX + btnW + 6, rTop,
                     btnW, btnH,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    // IP row.
    const int ipRowY = rTop + btnH + gap;
    if (m_hStaticIP)
        SetWindowPos(m_hStaticIP, nullptr,
                     rightX, ipRowY + 2,
                     lblW, rowH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_hEditIP)
        SetWindowPos(m_hEditIP, nullptr,
                     rightX + lblW + gap, ipRowY,
                     editW, rowH,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    // Port row.
    const int portRowY = ipRowY + rowH + gap;
    if (m_hStaticPort)
        SetWindowPos(m_hStaticPort, nullptr,
                     rightX, portRowY + 2,
                     lblW, rowH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_hEditPort)
        SetWindowPos(m_hEditPort, nullptr,
                     rightX + lblW + gap, portRowY,
                     (editW / 2), rowH,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    // Status label.
    const int statusY = portRowY + rowH + gap;
    if (m_hStaticStatus)
        SetWindowPos(m_hStaticStatus, nullptr,
                     rightX, statusY,
                     rightW, rowH + 4,
                     SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// ProcessMessages
// ---------------------------------------------------------------------------

bool Window::ProcessMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
}

// ---------------------------------------------------------------------------
// TakeResizePending
// ---------------------------------------------------------------------------

bool Window::TakeResizePending(int& newW, int& newH) {
    if (!m_resizePending) return false;
    m_width         = m_pendingWidth;
    m_height        = m_pendingHeight;
    newW            = m_pendingWidth;
    newH            = m_pendingHeight;
    m_resizePending = false;
    return true;
}

// ---------------------------------------------------------------------------
// SetUICallbacks
// ---------------------------------------------------------------------------

void Window::SetUICallbacks(UICallbacks callbacks) {
    m_callbacks = std::move(callbacks);
}

// ---------------------------------------------------------------------------
// UpdateDeviceList
// ---------------------------------------------------------------------------

void Window::UpdateDeviceList(const std::vector<DiscoveredDevice>& devices) {
    if (!m_hDeviceList) return;

    // Remember current selection by name.
    std::string selName;
    const int prevSel = (int)SendMessage(m_hDeviceList, LB_GETCURSEL, 0, 0);
    if (prevSel != LB_ERR) {
        const int len = (int)SendMessage(m_hDeviceList, LB_GETTEXTLEN, (WPARAM)prevSel, 0);
        if (len > 0) {
            std::wstring ws(static_cast<size_t>(len), L'\0');
            SendMessage(m_hDeviceList, LB_GETTEXT, (WPARAM)prevSel, (LPARAM)ws.data());
            // Convert back to UTF-8 narrow for matching.
            int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                             nullptr, 0, nullptr, nullptr);
            if (needed > 0) {
                selName.resize(static_cast<size_t>(needed - 1));
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                    selName.data(), needed, nullptr, nullptr);
            }
        }
    }

    SendMessage(m_hDeviceList, LB_RESETCONTENT, 0, 0);

    int reselectIdx = -1;
    int idx         = 0;
    for (const auto& d : devices) {
        if (d.state != DeviceState::Available) continue;
        if (d.role  == DeviceRole::Receiver)   continue; // skip ourselves

        // Build display string: "📱 <name>   <ip>:<port>"
        // Use plain ASCII since LISTBOX is ANSI by default; we'll use wide API.
        std::string line = d.displayName;
        if (!d.ipAddress.empty()) {
            line += "   ";
            line += d.ipAddress;
            line += ":";
            line += std::to_string(d.port);
        }

        // Convert to wide for UNICODE builds.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        std::wstring wline(static_cast<size_t>(wlen - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wline.data(), wlen);

        SendMessage(m_hDeviceList, LB_ADDSTRING, 0, (LPARAM)wline.c_str());

        if (!selName.empty() && d.displayName == selName) {
            reselectIdx = idx;
        }
        ++idx;
    }

    if (reselectIdx >= 0) {
        SendMessage(m_hDeviceList, LB_SETCURSEL, (WPARAM)reselectIdx, 0);
    }
}

// ---------------------------------------------------------------------------
// UpdateConnectionState
// ---------------------------------------------------------------------------

void Window::UpdateConnectionState(ConnectionState state,
                                   const std::string& detail)
{
    m_connState = state;

    // Determine button enable/disable.
    const bool canConnect    =
        (state == ConnectionState::Idle ||
         state == ConnectionState::Discovering ||
         state == ConnectionState::Error);
    const bool canDisconnect =
        (state == ConnectionState::Connected ||
         state == ConnectionState::WaitingClient);

    if (m_hBtnConnect)
        EnableWindow(m_hBtnConnect, canConnect ? TRUE : FALSE);
    if (m_hBtnDisconn)
        EnableWindow(m_hBtnDisconn, canDisconnect ? TRUE : FALSE);

    // Build status label text.
    std::string label;
    switch (state) {
    case ConnectionState::Idle:
        label = "Waiting for device...";
        break;
    case ConnectionState::Discovering:
        label = "Discovering...";
        break;
    case ConnectionState::WaitingClient:
        label = "Ready — connect from iPhone";
        break;
    case ConnectionState::Connected:
        label = "Connected";
        break;
    case ConnectionState::Error:
        label = detail.empty() ? "Error" : detail;
        break;
    }
    if (!detail.empty() && state != ConnectionState::Error) {
        label += " — ";
        label += detail;
    }

    if (m_hStaticStatus) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, nullptr, 0);
        std::wstring wlabel(static_cast<size_t>(wlen - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, wlabel.data(), wlen);
        SetWindowTextW(m_hStaticStatus, wlabel.c_str());
    }
}

// ---------------------------------------------------------------------------
// UpdateStreamStats — store stats and rebuild stat-bar text.
// ---------------------------------------------------------------------------

void Window::UpdateStreamStats(const StreamStats& stats) {
    m_stats = stats;
    RebuildStatText();
}

// RebuildStatText — called whenever stats change.
void Window::RebuildStatText() {
    char buf[256];

    if (m_stats.hasVideo) {
        snprintf(buf, sizeof(buf),
                 "FPS: %.1f  |  %ux%u  |  Dropped: %llu  |  A/V: %+.0f ms  |  "
                 "Audio: %s  |  OBS: %s",
                 static_cast<double>(m_stats.fps),
                 m_stats.videoWidth, m_stats.videoHeight,
                 static_cast<unsigned long long>(m_stats.framesDropped),
                 static_cast<double>(m_stats.avDiffMs),
                 m_stats.audioOk  ? "ON"        : "Unavailable",
                 m_stats.obsReady ? "Available" : "Unavailable");
    } else {
        snprintf(buf, sizeof(buf),
                 "FPS: --  |  Resolution: --  |  "
                 "Audio: %s  |  OBS: %s",
                 m_stats.audioOk  ? "ON"        : "Unavailable",
                 m_stats.obsReady ? "Available" : "Unavailable");
    }

    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        m_statusText = buf;
    }
}

// ---------------------------------------------------------------------------
// GetSelectedDeviceIndex
// ---------------------------------------------------------------------------

int Window::GetSelectedDeviceIndex() const {
    if (!m_hDeviceList) return -1;
    const int sel = (int)SendMessage(m_hDeviceList, LB_GETCURSEL, 0, 0);
    return (sel == LB_ERR) ? -1 : sel;
}

// ---------------------------------------------------------------------------
// GetManualIP / GetManualPort
// ---------------------------------------------------------------------------

std::string Window::GetManualIP() const {
    return GetEditText(m_hEditIP);
}

std::string Window::GetManualPort() const {
    return GetEditText(m_hEditPort);
}

std::string Window::GetEditText(HWND hEdit) const {
    if (!hEdit) return {};
    const int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return {};
    std::wstring ws(static_cast<size_t>(len), L'\0');
    GetWindowTextW(hEdit, ws.data(), len + 1);
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string s(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), needed,
                        nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// SetStatusText / GetStatusText — legacy compat (App still calls SetStatusText
// for the network status; we forward into m_statusText for overlay).
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
// DrawStatusOverlay — GDI stat bar at the bottom of the panel.
// ---------------------------------------------------------------------------

void Window::DrawStatusOverlay() {
    if (!m_hwnd) return;

    std::wstring wStatus;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        if (m_statusText.empty()) return;
        int wlen = MultiByteToWideChar(
            CP_UTF8, 0, m_statusText.c_str(), -1, nullptr, 0);
        wStatus.resize(static_cast<size_t>(wlen > 0 ? wlen - 1 : 0));
        if (wlen > 0)
            MultiByteToWideChar(CP_UTF8, 0, m_statusText.c_str(), -1,
                                wStatus.data(), wlen);
    }
    if (wStatus.empty()) return;

    HDC hdc = GetDC(m_hwnd);
    if (!hdc) return;

    // Stat bar occupies the bottom kStatBarHeight pixels of the client rect.
    const int barTop  = m_height - kStatBarHeight;
    RECT barRect      = { 0L, (LONG)barTop, (LONG)m_width, (LONG)m_height };

    HBRUSH bgBrush = CreateSolidBrush(kStatBg);
    if (bgBrush) {
        FillRect(hdc, &barRect, bgBrush);
        DeleteObject(bgBrush);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kFgNormal);

    HFONT hFont = CreateFontW(
        kStatFontSize, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    if (hFont) {
        HGDIOBJ old = SelectObject(hdc, hFont);
        RECT textRc = { 8L, (LONG)(barTop + 4), (LONG)(m_width - 8), (LONG)m_height };
        DrawTextW(hdc, wStatus.c_str(), -1, &textRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
        DeleteObject(hFont);
    }

    ReleaseDC(m_hwnd, hdc);
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

std::wstring Window::SettingsPath() const {
    wchar_t appData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    std::wstring dir = appData;
    dir += L"\\SanskyStream";
    CreateDirectoryW(dir.c_str(), nullptr); // no-op if already exists
    return dir + L"\\settings.ini";
}

void Window::LoadSettings() {
    const std::wstring path = SettingsPath();
    wchar_t ipBuf[128]   = {};
    wchar_t portBuf[16]  = {};
    GetPrivateProfileStringW(L"Network", L"LastIP",   L"",     ipBuf,   128, path.c_str());
    GetPrivateProfileStringW(L"Network", L"LastPort", L"5000", portBuf,  16, path.c_str());

    if (m_hEditIP   && ipBuf[0])   SetWindowTextW(m_hEditIP,   ipBuf);
    if (m_hEditPort && portBuf[0]) SetWindowTextW(m_hEditPort, portBuf);
}

void Window::SaveSettings() const {
    const std::wstring path = SettingsPath();
    const std::string  ip   = GetManualIP();
    const std::string  port = GetManualPort();

    auto toWide = [](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };

    if (!ip.empty())
        WritePrivateProfileStringW(L"Network", L"LastIP",
                                   toWide(ip).c_str(), path.c_str());
    if (!port.empty())
        WritePrivateProfileStringW(L"Network", L"LastPort",
                                   toWide(port).c_str(), path.c_str());
}

// ---------------------------------------------------------------------------
// WndProc (static trampoline → HandleMessage)
// ---------------------------------------------------------------------------

// static
LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT uMsg,
                                    WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_CREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        auto* pState  = reinterpret_cast<Window*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(pState));
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    auto* pState = reinterpret_cast<Window*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (pState)
        return pState->HandleMessage(uMsg, wParam, lParam);

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {

    case WM_DESTROY:
        SaveSettings();
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            m_pendingWidth  = static_cast<int>(LOWORD(lParam));
            m_pendingHeight = static_cast<int>(HIWORD(lParam));
            m_resizePending = true;
            LayoutChildControls(m_pendingWidth, m_pendingHeight);
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        // Paint static labels and the status label with dark background.
        HDC hdcCtrl = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdcCtrl, TRANSPARENT);
        SetTextColor(hdcCtrl, kFgNormal);
        static HBRUSH hBrPanelBg = CreateSolidBrush(kPanelBg);
        return reinterpret_cast<LRESULT>(hBrPanelBg);
    }

    case WM_CTLCOLOREDIT: {
        // Paint edit controls with a slightly lighter background.
        HDC hdcCtrl = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdcCtrl, OPAQUE);
        SetBkColor(hdcCtrl, kListBg);
        SetTextColor(hdcCtrl, kFgNormal);
        static HBRUSH hBrEditBg = CreateSolidBrush(kListBg);
        return reinterpret_cast<LRESULT>(hBrEditBg);
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdcCtrl = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdcCtrl, kListBg);
        SetTextColor(hdcCtrl, kFgNormal);
        static HBRUSH hBrListBg = CreateSolidBrush(kListBg);
        return reinterpret_cast<LRESULT>(hBrListBg);
    }

    case WM_COMMAND: {
        const int  ctrlId  = LOWORD(wParam);
        const int  notif   = HIWORD(wParam);

        switch (ctrlId) {
        case ID_BTN_CONNECT:
            if (notif == BN_CLICKED && m_callbacks.onConnectClicked)
                m_callbacks.onConnectClicked();
            break;

        case ID_BTN_DISCONN:
            if (notif == BN_CLICKED && m_callbacks.onDisconnectClicked)
                m_callbacks.onDisconnectClicked();
            break;

        case ID_DEVICE_LIST:
            if (notif == LBN_SELCHANGE && m_callbacks.onDeviceSelected) {
                const int sel = (int)SendMessage(m_hDeviceList,
                                                 LB_GETCURSEL, 0, 0);
                m_callbacks.onDeviceSelected(sel == LB_ERR ? -1 : sel);
            }
            break;

        case ID_EDIT_IP:
        case ID_EDIT_PORT:
            // Save settings when the user leaves the IP / port field.
            if (notif == EN_KILLFOCUS)
                SaveSettings();
            // Fire manual-connect callback when Enter is pressed in an edit.
            if (notif == EN_CHANGE)
                break; // nothing on every keystroke
            break;

        default:
            break;
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
}

} // namespace SanskyStream
