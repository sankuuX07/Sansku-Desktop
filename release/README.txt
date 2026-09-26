SanskyStream 1.0.0
==================

Low-latency screen mirroring and audio streaming from iPhone or Android
to Windows, with optional OBS Studio integration.

---------------------------------------------------------------------------
REQUIREMENTS
---------------------------------------------------------------------------

Windows receiver:
  - Windows 10 version 1903 or later (64-bit)
  - Direct3D 11 compatible GPU
  - Active Wi-Fi connection on a LOCAL network
  - Visual C++ Redistributable 2015-2022 (included in installer)

iPhone sender:
  - iPhone running iOS 12 or later
  - SanskyStream iOS app (requires Mac + Xcode to build)
  - Same Wi-Fi network as the Windows receiver

Android sender:
  - Android 8.0 (API 26) or later
  - Android 10 (API 29) or later for internal audio capture
  - SanskyStream-Android.apk (side-loaded, unsigned development build)
  - Same Wi-Fi network as the Windows receiver

---------------------------------------------------------------------------
WINDOWS INSTALLATION (Installer)
---------------------------------------------------------------------------

1. Run SanskyStream-Setup.ps1 as Administrator:
     Right-click SanskyStream-Setup.ps1 -> Run with PowerShell (Admin)

2. The installer will:
   - Create C:\Program Files\SanskyStream\
   - Copy SanskyStream_Windows.exe and required DLLs
   - Create a Start Menu shortcut
   - Create a Desktop shortcut
   - Register an uninstaller

3. Launch SanskyStream from the Start Menu or Desktop shortcut.

---------------------------------------------------------------------------
WINDOWS INSTALLATION (Portable)
---------------------------------------------------------------------------

1. Copy the contents of Windows\Portable\ to any folder.
2. Run SanskyStream_Windows.exe directly. No installation required.

---------------------------------------------------------------------------
ANDROID INSTALLATION
---------------------------------------------------------------------------

The Android APK is an unsigned development build. To install:

1. Enable "Install from Unknown Sources" on your Android device:
     Settings > Security > Install Unknown Apps

2. Transfer SanskyStream-Android.apk to your Android device.

3. Open the APK file on your Android device and tap Install.

4. Grant permissions when prompted (screen capture, microphone, network).

NOTE: This APK is NOT signed with a production certificate.
      It is suitable for personal and development use only.

---------------------------------------------------------------------------
BASIC USAGE
---------------------------------------------------------------------------

1. Start SanskyStream on your Windows PC.
   Status shows: "Waiting for Device..."

2. Ensure your phone is on the SAME Wi-Fi network as your Windows PC.

3a. iPhone: Launch SanskyStream iOS app and tap "Start Streaming".

3b. Android: Open SanskyStream, enter the Windows PC IP address,
    and tap "Start Streaming". Grant MediaProjection permission.

4. Video appears in the SanskyStream window within 1-2 seconds.
   Audio plays through your Windows audio output.

5. To stop: tap "Stop" on the phone, or close SanskyStream on Windows.

---------------------------------------------------------------------------
DEVICE DISCOVERY
---------------------------------------------------------------------------

SanskyStream uses mDNS (Bonjour/DNS-SD) for automatic discovery.

- Discovered phones appear in the device list with platform badges:
    [iOS]     for iPhone
    [Android] for Android

- Both devices MUST be on the same local network segment.
- Discovery does NOT work across VPNs or different subnets.

Manual connection:
  Enter the Windows PC local IP address in the Android app's IP field.
  Default ports: TCP 5000 (control/audio), UDP 5001 (video).

---------------------------------------------------------------------------
OBS STUDIO INTEGRATION
---------------------------------------------------------------------------

SanskyStream includes an OBS plugin (sansky-source.dll).

Setup:
  1. Run install-plugin.ps1 (in repository) as Administrator.
  2. In OBS, add a new Source: "SanskyStream Source".
  3. Start streaming from your phone.
  4. The phone screen and audio appear as an OBS source.

---------------------------------------------------------------------------
NETWORK PORTS
---------------------------------------------------------------------------

  TCP 5000   Control + audio
  UDP 5001   H.264 video
  UDP 5353   mDNS discovery

Allow SanskyStream_Windows.exe through Windows Firewall if prompted.

---------------------------------------------------------------------------
KNOWN LIMITATIONS
---------------------------------------------------------------------------

1. iPhone physical testing PENDING (Mac/Xcode required)
2. Android physical testing PENDING (physical device required)
3. OBS live streaming test PENDING (physical streaming session required)
4. Long-duration stability test PENDING (physical device required)
5. Android APK is UNSIGNED (requires Unknown Sources to install)
6. Orientation change not tested on physical device

---------------------------------------------------------------------------
VERSION
---------------------------------------------------------------------------

SanskyStream 1.0.0
Build date: 2026-09-26
Windows: SanskyStream_Windows.exe (Release x64)
Android: SanskyStream-Android.apk (API 26+, Java 11)