#include "BluetoothServer.h"
#include "AudioCapture.h"
#include <windows.h>
#include <cstdio>
#include <atomic>

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

static void OnAudioData(const uint8_t* Data, uint32_t Size, uint32_t SampleRate, uint16_t Channels, uint16_t BitsPerSample) {
    if (!GlobalServer || !ClientAlive.load()) return;

    if (!HeaderSent.load()) {
        printf("Sending header: SampleRate=%u Channels=%u BitsPerSample=%u\n", SampleRate, Channels, BitsPerSample);
        StreamHeader Header{ StreamMagic, SampleRate, Channels, BitsPerSample };
        if (!GlobalServer->Send(reinterpret_cast<const uint8_t*>(&Header), sizeof(Header))) {
            printf("Failed to send header, dropping client\n");
            ClientAlive.store(false);
            return;
        }
        HeaderSent.store(true);
    }

    if (!GlobalServer->Send(Data, Size)) {
        printf("Send failed after %u bytes, dropping client\n", Size);
        ClientAlive.store(false);
        HeaderSent.store(false);
        return;
    }

    static uint64_t TotalBytes = 0;
    static ULONGLONG LastLog = 0;
    TotalBytes += Size;
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
