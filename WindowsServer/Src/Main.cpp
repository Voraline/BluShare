#include "BluetoothServer.h"
#include "AudioCapture.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <vector>

#pragma pack(push, 1)
struct StreamHeader {
    uint32_t Magic;
    uint32_t SampleRate;
    uint16_t Channels;
    uint16_t BitsPerSample;
};
#pragma pack(pop)

static constexpr uint32_t StreamMagic = 0x42415354;

static std::atomic<bool> HeaderSent{ false };
static std::atomic<bool> ClientAlive{ false };
static BluetoothServer* GlobalServer = nullptr;

// Classic Bluetooth RFCOMM/SPP typically only sustains ~100-250 KB/s in
// practice. Raw 48kHz/32-bit-float stereo audio needs ~384 KB/s, which is
// why audio would play briefly then stall once the send buffer fell behind.
// We convert to 16-bit mono and drop every other sample (half sample rate)
// before sending, cutting bandwidth roughly 8x to comfortably fit RFCOMM.
static std::vector<int16_t> ConversionBuffer;

static void OnAudioData(const uint8_t* Data, uint32_t Size, uint32_t SampleRate, uint16_t Channels, uint16_t BitsPerSample) {
    if (!GlobalServer || !ClientAlive.load()) return;

    // Assumes the WASAPI mix format is 32-bit IEEE float, which is the
    // standard default output format on Windows.
    if (BitsPerSample != 32 || Channels == 0) return;

    const float* Samples = reinterpret_cast<const float*>(Data);
    uint32_t FrameCount = Size / (Channels * sizeof(float));

    ConversionBuffer.clear();
    ConversionBuffer.reserve(FrameCount / 2 + 1);
    for (uint32_t i = 0; i < FrameCount; i += 2) { // keep every other frame -> half sample rate
        float Left = Samples[i * Channels + 0];
        float Right = (Channels > 1) ? Samples[i * Channels + 1] : Left;
        float Mono = (Left + Right) * 0.5f;
        if (Mono > 1.0f) Mono = 1.0f;
        if (Mono < -1.0f) Mono = -1.0f;
        ConversionBuffer.push_back(static_cast<int16_t>(Mono * 32767.0f));
    }

    if (ConversionBuffer.empty()) return;

    uint32_t OutSampleRate = SampleRate / 2;
    uint32_t OutSize = static_cast<uint32_t>(ConversionBuffer.size() * sizeof(int16_t));

    if (!HeaderSent.load()) {
        printf("Sending header: SampleRate=%u Channels=1 BitsPerSample=16\n", OutSampleRate);
        StreamHeader Header{ StreamMagic, OutSampleRate, 1, 16 };
        if (!GlobalServer->Send(reinterpret_cast<const uint8_t*>(&Header), sizeof(Header))) {
            printf("Failed to send header, dropping client\n");
            ClientAlive.store(false);
            return;
        }
        HeaderSent.store(true);
    }

    if (!GlobalServer->Send(reinterpret_cast<const uint8_t*>(ConversionBuffer.data()), OutSize)) {
        printf("Send failed after %u bytes, dropping client\n", OutSize);
        ClientAlive.store(false);
        HeaderSent.store(false);
        return;
    }

    static uint64_t TotalBytes = 0;
    static ULONGLONG LastLog = 0;
    TotalBytes += OutSize;
    ULONGLONG Now = GetTickCount64();
    if (Now - LastLog > 2000) {
        printf("Streamed %llu bytes so far\n", (unsigned long long)TotalBytes);
        LastLog = Now;
    }
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    AudioCapture Capture;
    if (!Capture.Initialize()) {
        printf("Failed to initialize audio capture\n");
        return 1;
    }

    BluetoothServer Server;
    GlobalServer = &Server;
    if (!Server.Start()) {
        printf("Failed to start Bluetooth server\n");
        return 1;
    }

    printf("Waiting for Android client...\n");

    while (true) {
        if (!Server.WaitForClient()) {
            continue;
        }
        printf("Client connected\n");
        HeaderSent.store(false);
        ClientAlive.store(true);

        if (!Capture.Start(OnAudioData)) {
            printf("Failed to start audio capture\n");
            Server.DropClient();
            continue;
        }

        while (ClientAlive.load()) {
            Sleep(200);
        }

        Capture.Stop();
        Server.DropClient();
        printf("Client disconnected, waiting for reconnection...\n");
    }
}
