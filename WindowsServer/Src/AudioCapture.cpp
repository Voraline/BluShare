#include "AudioCapture.h"
#include <mmdeviceapi.h>
#include <avrt.h>
#include <cstdio>

static const IID IID_IAudioClient_Local = __uuidof(IAudioClient);
static const IID IID_IAudioCaptureClient_Local = __uuidof(IAudioCaptureClient);
static const CLSID CLSID_MMDeviceEnumerator_Local = __uuidof(MMDeviceEnumerator);
static const IID IID_IMMDeviceEnumerator_Local = __uuidof(IMMDeviceEnumerator);

AudioCapture::~AudioCapture() {
    Stop();
    if (Format) CoTaskMemFree(Format);
    if (CaptureClient) CaptureClient->Release();
    if (Client) Client->Release();
    if (Device) Device->Release();
    if (DeviceEnumerator) DeviceEnumerator->Release();
}

bool AudioCapture::Initialize() {
    HRESULT Result = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr, CLSCTX_ALL,
        IID_IMMDeviceEnumerator_Local, reinterpret_cast<void**>(&DeviceEnumerator));
    if (FAILED(Result)) return false;

    Result = DeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device);
    if (FAILED(Result)) return false;

    Result = Device->Activate(IID_IAudioClient_Local, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&Client));
    if (FAILED(Result)) return false;

    Result = Client->GetMixFormat(&Format);
    if (FAILED(Result)) return false;

    REFERENCE_TIME BufferDuration = 20000000;
    Result = Client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        BufferDuration, 0, Format, nullptr);
    if (FAILED(Result)) return false;

    Result = Client->GetService(IID_IAudioCaptureClient_Local, reinterpret_cast<void**>(&CaptureClient));
    if (FAILED(Result)) return false;

    return true;
}

bool AudioCapture::Start(DataCallback Callback) {
    OnData = std::move(Callback);
    if (FAILED(Client->Start())) return false;
    Running = true;
    CaptureThread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    return CaptureThread != nullptr;
}

void AudioCapture::Stop() {
    if (!Running) return;
    Running = false;
    if (CaptureThread) {
        WaitForSingleObject(CaptureThread, 2000);
        CloseHandle(CaptureThread);
        CaptureThread = nullptr;
    }
    if (Client) Client->Stop();
}

DWORD WINAPI AudioCapture::ThreadProc(LPVOID Param) {
    reinterpret_cast<AudioCapture*>(Param)->CaptureLoop();
    return 0;
}

void AudioCapture::CaptureLoop() {
    DWORD TaskIndex = 0;
    HANDLE AvrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &TaskIndex);

    while (Running) {
        Sleep(10);

        UINT32 PacketLength = 0;
        HRESULT Result = CaptureClient->GetNextPacketSize(&PacketLength);
        while (SUCCEEDED(Result) && PacketLength > 0) {
            BYTE* Data = nullptr;
            UINT32 FramesAvailable = 0;
            DWORD Flags = 0;
            Result = CaptureClient->GetBuffer(&Data, &FramesAvailable, &Flags, nullptr, nullptr);
            if (FAILED(Result)) break;

            uint32_t ByteCount = FramesAvailable * Format->nBlockAlign;
            if (OnData && ByteCount > 0 && !(Flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                OnData(reinterpret_cast<const uint8_t*>(Data), ByteCount,
                    Format->nSamplesPerSec, Format->nChannels, Format->wBitsPerSample);
            }

            CaptureClient->ReleaseBuffer(FramesAvailable);
            Result = CaptureClient->GetNextPacketSize(&PacketLength);
        }
    }

    if (AvrtHandle) AvRevertMmThreadCharacteristics(AvrtHandle);
}
