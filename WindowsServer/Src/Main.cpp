#include "BluetoothServer.h"
#include "AudioCapture.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <vector>

#pragma pack(push, 1)
struct StreamHeader {
    uint32_t Magic;
    uint32_t SampleRate;
    uint16_t Channels;
    uint16_t BitsPerSample;
    uint8_t Codec;
};
#pragma pack(pop)

static constexpr uint32_t StreamMagic = 0x42415354;
static constexpr uint8_t CodecRaw = 0;
static constexpr uint32_t TargetSampleRate = 48000;

static std::atomic<bool> HeaderSent{ false };
static std::atomic<bool> ClientAlive{ false };
static BluetoothServer* GlobalServer = nullptr;
static std::vector<int16_t> PcmBuffer;

static inline int16_t FloatToInt16(float Value) {
    if (Value > 1.0f) Value = 1.0f;
    if (Value < -1.0f) Value = -1.0f;
    return static_cast<int16_t>(Value * 32767.0f);
}

static bool SendChunk(const float* Samples, uint32_t ChunkFrames, uint16_t Channels) {
    PcmBuffer.clear();
    PcmBuffer.reserve(ChunkFrames);

    for (uint32_t I = 0; I < ChunkFrames; ++I) {
        float Left = Samples[I * Channels + 0];
        float Right = (Channels > 1) ? Samples[I * Channels + 1] : Left;
        float Mono = (Channels > 1) ? (Left + Right) * 0.5f : Left;
        PcmBuffer.push_back(FloatToInt16(Mono));
    }

    if (PcmBuffer.empty()) return true;

    if (!HeaderSent.load(std::memory_order_relaxed)) {
        StreamHeader Header{ StreamMagic, TargetSampleRate, 1, 16, CodecRaw };
        if (!GlobalServer->Send(reinterpret_cast<const uint8_t*>(&Header), sizeof(Header))) {
            return false;
        }
        HeaderSent.store(true, std::memory_order_relaxed);
    }

    return GlobalServer->Send(reinterpret_cast<const uint8_t*>(PcmBuffer.data()),
        static_cast<uint32_t>(PcmBuffer.size() * sizeof(int16_t)));
}

static void OnAudioData(const uint8_t* Data, uint32_t Size, uint32_t SampleRate, uint16_t Channels, uint16_t BitsPerSample) {
    if (!GlobalServer || !ClientAlive.load(std::memory_order_relaxed)) return;
    if (BitsPerSample != 32 || Channels == 0) return;

    const float* Samples = reinterpret_cast<const float*>(Data);
    uint32_t FrameCount = Size / (Channels * sizeof(float));
    if (FrameCount == 0) return;

    if (!SendChunk(Samples, FrameCount, Channels)) {
        ClientAlive.store(false, std::memory_order_relaxed);
        HeaderSent.store(false, std::memory_order_relaxed);
    }
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    AudioCapture Capture;
    if (!Capture.Initialize()) return 1;

    BluetoothServer Server;
    GlobalServer = &Server;
    if (!Server.Start()) return 1;

    PcmBuffer.reserve(4096);

    while (true) {
        if (!Server.WaitForClient()) continue;

        HeaderSent.store(false, std::memory_order_relaxed);
        ClientAlive.store(true, std::memory_order_relaxed);

        if (!Capture.Start(OnAudioData)) {
            Server.DropClient();
            continue;
        }

        while (ClientAlive.load(std::memory_order_relaxed)) {
            Sleep(200);
        }

        Capture.Stop();
        Server.DropClient();
    }
}
