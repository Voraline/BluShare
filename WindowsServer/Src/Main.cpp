#include "BluetoothServer.h"
#include "AudioCapture.h"
#include <windows.h>
#include <cstdlib>
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

static std::atomic<bool> HeaderSent{ false };
static std::atomic<bool> ClientAlive{ false };
static BluetoothServer* GlobalServer = nullptr;

struct AdpcmState {
    int Predictor = 0;
    int Index = 0;
};

static const int IndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int StepSizeTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static AdpcmState LeftState;
static AdpcmState RightState;
static std::vector<uint8_t> ConversionBuffer;

static inline void ResetAdpcmState() {
    LeftState = AdpcmState{};
    RightState = AdpcmState{};
}

static inline uint8_t AdpcmEncodeSample(AdpcmState& State, int16_t Sample) {
    int Diff = Sample - State.Predictor;
    int Sign = 0;
    if (Diff < 0) { Sign = 8; Diff = -Diff; }

    int Step = StepSizeTable[State.Index];
    int DiffAccum = Step >> 3;
    int Code = 0;

    int TempStep = Step;
    if (Diff >= TempStep) { Code |= 4; Diff -= TempStep; DiffAccum += TempStep; }
    TempStep >>= 1;
    if (Diff >= TempStep) { Code |= 2; Diff -= TempStep; DiffAccum += TempStep; }
    TempStep >>= 1;
    if (Diff >= TempStep) { Code |= 1; DiffAccum += TempStep; }

    Code |= Sign;

    if (Sign) State.Predictor -= DiffAccum;
    else State.Predictor += DiffAccum;

    if (State.Predictor > 32767) State.Predictor = 32767;
    if (State.Predictor < -32768) State.Predictor = -32768;

    State.Index += IndexTable[Code];
    if (State.Index < 0) State.Index = 0;
    if (State.Index > 88) State.Index = 88;

    return static_cast<uint8_t>(Code);
}

static inline int16_t FloatToInt16(float Value) {
    if (Value > 1.0f) Value = 1.0f;
    if (Value < -1.0f) Value = -1.0f;
    return static_cast<int16_t>(Value * 32767.0f);
}

static void OnAudioData(const uint8_t* Data, uint32_t Size, uint32_t SampleRate, uint16_t Channels, uint16_t BitsPerSample) {
    if (!GlobalServer || !ClientAlive.load(std::memory_order_relaxed)) return;
    if (BitsPerSample != 32 || Channels == 0) return;

    const float* Samples = reinterpret_cast<const float*>(Data);
    uint32_t FrameCount = Size / (Channels * sizeof(float));

    ConversionBuffer.clear();
    if (ConversionBuffer.capacity() < FrameCount) {
        ConversionBuffer.reserve(FrameCount);
    }

    for (uint32_t I = 0; I < FrameCount; ++I) {
        float Left = Samples[I * Channels + 0];
        float Right = (Channels > 1) ? Samples[I * Channels + 1] : Left;

        uint8_t LeftCode = AdpcmEncodeSample(LeftState, FloatToInt16(Left));
        uint8_t RightCode = AdpcmEncodeSample(RightState, FloatToInt16(Right));

        ConversionBuffer.push_back(static_cast<uint8_t>((LeftCode << 4) | RightCode));
    }

    if (ConversionBuffer.empty()) return;

    uint32_t OutSize = static_cast<uint32_t>(ConversionBuffer.size());

    if (!HeaderSent.load(std::memory_order_relaxed)) {
        StreamHeader Header{ StreamMagic, SampleRate, 2, 16, 1 };
        if (!GlobalServer->Send(reinterpret_cast<const uint8_t*>(&Header), sizeof(Header))) {
            ClientAlive.store(false, std::memory_order_relaxed);
            return;
        }
        HeaderSent.store(true, std::memory_order_relaxed);
    }

    if (!GlobalServer->Send(ConversionBuffer.data(), OutSize)) {
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

    ConversionBuffer.reserve(4096);

    while (true) {
        if (!Server.WaitForClient()) continue;

        HeaderSent.store(false, std::memory_order_relaxed);
        ClientAlive.store(true, std::memory_order_relaxed);
        ResetAdpcmState();

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
