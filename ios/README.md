# SanskyStream — iOS Sender (Milestone 3)

Native iPhone app that establishes a TCP connection to the SanskyStream Windows receiver
and sends a `HELLO` test message. Built with **Swift + SwiftUI + Apple Network framework**.

---

## Requirements

| Item | Minimum |
|------|---------|
| Xcode | 15.0+ |
| macOS | 13 Ventura+ |
| iOS deployment target | 16.0 |
| Apple Developer account | Free account is sufficient for device testing |

> **This project cannot be built on Windows.** Transfer the entire `ios/` folder to a Mac
> and open it there.

---

## Project Structure

```
ios/
├── SanskyStream.xcodeproj/       ← Open this in Xcode
└── SanskyStream/
    ├── App/
    │   └── SanskyStreamApp.swift    @main entry point
    ├── Network/
    │   └── ConnectionManager.swift  NWConnection TCP logic
    └── UI/
        └── ConnectView.swift        SwiftUI interface
```

---

## Opening in Xcode

```bash
# On macOS — from the project root
open ios/SanskyStream.xcodeproj
```

Or drag `SanskyStream.xcodeproj` onto the Xcode icon in the Dock.

---

## First-Time Setup

1. **Open the project** in Xcode.
2. Select the **SanskyStream** target in the Project navigator.
3. Go to **Signing & Capabilities**.
4. Select your **Team** from the dropdown (any Apple ID works for device testing).
5. Xcode will automatically manage provisioning.
6. If the bundle ID `com.sanskystream.ios` conflicts, change it to something unique
   (e.g. `com.yourname.sanskystream`).

---

## Building and Running

### On a real iPhone (recommended for Wi-Fi testing)
1. Connect your iPhone via USB.
2. Trust the computer on the device if prompted.
3. Select your iPhone in the Xcode toolbar.
4. Press **⌘R** to build and run.

### On the Simulator
1. Select any iPhone simulator (iOS 16+).
2. Press **⌘R**.

> **Note:** Local network connections from the Simulator go through the Mac's network
> interface, not a real Wi-Fi radio. For actual iPhone → Windows Wi-Fi testing, use a
> physical device.

---

## How to Test (Milestone 3)

### Windows side (already running from Milestone 2)
1. Launch `SanskyStream_Windows.exe` on your Windows PC.
2. It should display **"Status: Waiting for Device..."** and listen on port 5000.
3. Find your Windows PC's local IPv4 address:
   ```
   ipconfig        (in Windows Command Prompt)
   ```
   Look for the address under your Wi-Fi adapter, e.g. `192.168.1.100`.

### iPhone side
1. Make sure the iPhone and Windows PC are on the **same Wi-Fi network**.
2. Launch the SanskyStream app on the iPhone.
3. Enter the Windows PC IP address (e.g. `192.168.1.100`).
4. Keep port `5000`.
5. Tap **Connect**.

### Expected results

| What | Expected |
|------|----------|
| iPhone status | **Connected** |
| Windows status | **Connected** |
| Windows log | `Client connected.` → `Received: HELLO` |
| After Disconnect | iPhone: **Disconnected**, Windows: **Waiting for Device...** |

---

## Troubleshooting

| Problem | Likely cause | Fix |
|---------|-------------|-----|
| "Error: The operation couldn't be completed" | Windows firewall blocking port 5000 | Allow port 5000 in Windows Defender Firewall |
| Local network prompt appears | iOS 14+ privacy | Tap **Allow** when iOS asks for local network access |
| Status stays "Connecting..." | Wrong IP, or devices on different networks | Verify IP with `ipconfig`, confirm same Wi-Fi |
| Xcode signing error | No team selected | Select a team in Signing & Capabilities |

### Windows firewall — allow port 5000
```
Windows Defender Firewall → Advanced Settings →
Inbound Rules → New Rule → Port → TCP → 5000 → Allow
```

---

## What Is NOT Implemented in Milestone 3

- Video streaming
- Audio streaming
- Screen/camera capture
- H.264 / AAC encoding
- Device discovery (Bonjour, QR codes)
- Authentication
- Cloud services

These belong to later milestones.
