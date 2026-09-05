#pragma once

// ---------------------------------------------------------------------------
// Window — M17: Win32 host window with integrated UI panel
//
// Layout (top-to-bottom inside the client rect):
//
//   ┌─────────────────────────────────────────────────────┐
//   │                                                     │
//   │        D3D11 VIDEO AREA  (letterboxed)              │
//   │        height = clientH − kPanelHeight              │
//   │                                                     │
//   ├────────────────────┬────────────────────────────────┤
//   │ Device list        │  [Connect] [Disconnect]        │
//   │  (LBS_NOTIFY)      │  IP: [_________] Port: [5000] │
//   │                    │  Status label                  │
//   ├────────────────────┴────────────────────────────────┤
//   │  Stat bar: FPS / resolution / audio / OBS / drops  │
//   └─────────────────────────────────────────────────────┘
//
// Child controls are standard Win32 LISTBOX, BUTTON, EDIT, STATIC.
// No third-party UI library.
//
// Threading:
//   All control creation and message handling: main thread.
//   SetStatusText / UpdateDeviceList / UpdateConnectionState /
//   UpdateStreamStats: must be called from the main thread, or
//   sent via PostMessage.  App calls these from the render loop
//   (main thread) so no extra locking is needed.
// ---------------------------------------------------------------------------

#include "DiscoveredDevice.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace SanskyStream {

// ---------------------------------------------------------------------------
// ConnectionState — exposed so App can drive button enable/disable.
// ---------------------------------------------------------------------------
enum class ConnectionState : uint8_t {
    Idle,         // No client, listening, no device selected
    Discovering,  // Discovery running, no device visible yet
    WaitingClient,// Device selected; waiting for iPhone to connect
    Connected,    // iPhone is connected
    Error,        // Network error
};

// ---------------------------------------------------------------------------
// StreamStats — filled by App::Run() every ~500 ms and pushed to Window.
// ---------------------------------------------------------------------------
struct StreamStats {
    float    fps           = 0.0f;
    uint32_t videoWidth    = 0;
    uint32_t videoHeight   = 0;
    bool     hasVideo      = false;
    bool     audioOk       = false;
    bool     obsReady      = false;
    uint64_t framesDropped = 0;
    int64_t  avDiffMs      = 0;   // A/V diff in ms; 0 = no data
    bool     avAnchored    = false;
};

// ---------------------------------------------------------------------------
// UICallbacks — set by App to respond to user actions.
// All callbacks are invoked on the main (UI) thread.
// ---------------------------------------------------------------------------
struct UICallbacks {
    std::function<void(int deviceIndex)> onDeviceSelected;  // -1 = none
    std::function<void()>                onConnectClicked;
    std::function<void()>                onDisconnectClicked;
    std::function<void(const std::string& ip, const std::string& port)> onManualConnect;
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
class Window {
public:
    Window(int width, int height, const std::wstring& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Pump Windows messages. Returns false when WM_QUIT is received.
    bool ProcessMessages();

    // Draw GDI overlay (stat bar text) on the panel area.
    // Call from the main thread after Renderer::Render().
    void DrawStatusOverlay();

    // Thread-safe: legacy path kept for compatibility.
    void SetStatusText(const std::string& text);
    std::string GetStatusText() const;

    // Returns true if the window was resized since the last call.
    // newW / newH are set to new client dimensions.
    bool TakeResizePending(int& newW, int& newH);

    // -----------------------------------------------------------------------
    // M17: UI update methods — call from main thread only.
    // -----------------------------------------------------------------------

    // Wire action callbacks before calling ProcessMessages().
    void SetUICallbacks(UICallbacks callbacks);

    // Rebuild the device list-box from the current discovered device vector.
    // Preserves the current selection if the device name is still present.
    void UpdateDeviceList(const std::vector<DiscoveredDevice>& devices);

    // Enable/disable buttons and update the status label.
    void UpdateConnectionState(ConnectionState state,
                               const std::string& detail = {});

    // Update the bottom stat bar (called ~every 500 ms from App::Run).
    void UpdateStreamStats(const StreamStats& stats);

    // Return the zero-based index of the selected device, or -1 if none.
    int GetSelectedDeviceIndex() const;

    // Return current manual IP / Port strings from the edit controls.
    std::string GetManualIP()   const;
    std::string GetManualPort() const;

    // -----------------------------------------------------------------------
    // Geometry
    // -----------------------------------------------------------------------
    HWND GetHWND()  const { return m_hwnd;   }
    int  GetWidth() const { return m_width;  }
    int  GetHeight()const { return m_height; }

    // Height of the UI panel at the bottom of the client rect (px).
    static constexpr int kPanelHeight   = 160;
    // Height of the bottom stat bar inside the panel (px).
    static constexpr int kStatBarHeight = 24;

private:
    // -----------------------------------------------------------------------
    // Child control creation / layout
    // -----------------------------------------------------------------------
    void CreateChildControls();
    void LayoutChildControls(int clientW, int clientH);

    // -----------------------------------------------------------------------
    // Settings persistence (WritePrivateProfileString to settings.ini)
    // -----------------------------------------------------------------------
    std::wstring SettingsPath() const;
    void         LoadSettings();
    void         SaveSettings() const;

    // -----------------------------------------------------------------------
    // WndProc
    // -----------------------------------------------------------------------
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg,
                                       WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    // Read UTF-8 text from an edit control child window.
    std::string GetEditText(HWND hEdit) const;

    // Build the stat bar text from m_stats and write it to m_statusText.
    void RebuildStatText();

    // -----------------------------------------------------------------------
    // Win32 handles
    // -----------------------------------------------------------------------
    HWND         m_hwnd      = nullptr;
    HINSTANCE    m_hInstance = nullptr;
    std::wstring m_windowClass;
    int          m_width     = 0;
    int          m_height    = 0;

    // Resize pending — consumed by Renderer on the main thread.
    bool m_resizePending  = false;
    int  m_pendingWidth   = 0;
    int  m_pendingHeight  = 0;

    // -----------------------------------------------------------------------
    // Child control HWNDs (all created in CreateChildControls)
    // -----------------------------------------------------------------------
    HWND m_hDeviceList   = nullptr; // LISTBOX  — discovered devices
    HWND m_hBtnConnect   = nullptr; // BUTTON   — Connect
    HWND m_hBtnDisconn   = nullptr; // BUTTON   — Disconnect
    HWND m_hStaticStatus = nullptr; // STATIC   — status label
    HWND m_hEditIP       = nullptr; // EDIT     — manual IP
    HWND m_hEditPort     = nullptr; // EDIT     — manual port
    HWND m_hStaticIP     = nullptr; // STATIC   — "IP:" label
    HWND m_hStaticPort   = nullptr; // STATIC   — "Port:" label

    // -----------------------------------------------------------------------
    // Child control IDs
    // -----------------------------------------------------------------------
    static constexpr int ID_DEVICE_LIST  = 1001;
    static constexpr int ID_BTN_CONNECT  = 1002;
    static constexpr int ID_BTN_DISCONN  = 1003;
    static constexpr int ID_EDIT_IP      = 1004;
    static constexpr int ID_EDIT_PORT    = 1005;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    UICallbacks      m_callbacks;
    ConnectionState  m_connState = ConnectionState::Idle;
    StreamStats      m_stats;
    std::string      m_statusText;           // stat bar text (legacy + stats)
    mutable std::mutex m_statusMutex;
};

} // namespace SanskyStream
