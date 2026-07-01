# BluShare

Streams Windows system audio to an Android device over classic Bluetooth (RFCOMM/SPP).

## Components

- `WindowsServer/` — C++20 Windows service. Captures the default audio output via WASAPI loopback and streams raw PCM to a connected Android client over an RFCOMM Bluetooth socket (SDP-advertised under the standard SPP UUID).
- `AndroidApp/` — Android app (min SDK 26). A thin Java layer (required by the platform for Bluetooth/OS APIs) connects the RFCOMM socket and hands received buffers to a C++/AAudio native library for low-latency playback.

## Build

### Windows server
```
cmake -S WindowsServer -B WindowsServer/Build -DCMAKE_BUILD_TYPE=Release
cmake --build WindowsServer/Build --config Release
```

### Android app
```
cd AndroidApp
gradle assembleRelease
```

## Usage

1. Pair the Windows PC and Android device over Bluetooth first (Windows Settings > Bluetooth).
2. Run `BluShare.exe` on Windows. It registers an SPP service and waits for a connection.
3. Open the Android app, grant Bluetooth permissions, and tap the paired PC in the list.
4. Audio starts streaming automatically once connected.

## CI

`.github/workflows/build.yml` builds both targets on every push and publishes a combined `BluShare.zip` artifact containing the Windows executable and the Android APK.
