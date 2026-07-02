#pragma once
#define WIN32_LEAN_AND_MEAN
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <windows.h>
#include <cstdint>
#include <functional>

class AudioCapture {
public:
    using DataCallback = std::function<void(const uint8_t*, uint32_t, uint32_t, uint16_t, uint16_t)>;

    AudioCapture() = default;
    ~AudioCapture();

    bool Initialize();
    bool Start(DataCallback Callback);
    void Stop();

    static constexpr bool MuteWhilePlaying = false;

private:
    static DWORD WINAPI ThreadProc(LPVOID Param);
    void CaptureLoop();

    IMMDeviceEnumerator* DeviceEnumerator = nullptr;
    IMMDevice* Device = nullptr;
    IAudioClient* Client = nullptr;
    IAudioCaptureClient* CaptureClient = nullptr;
    IAudioEndpointVolume* EndpointVolume = nullptr;
    BOOL WasMuted = FALSE;
    WAVEFORMATEX* Format = nullptr;
    HANDLE CaptureThread = nullptr;
    HANDLE CaptureEvent = nullptr;
    volatile bool Running = false;
    DataCallback OnData;
};
