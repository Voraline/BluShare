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
    uint8_t Codec; // 0 = raw PCM16, 1 = IMA ADPCM
};
#pragma pack(pop)

static constexpr uint32_t StreamMagic = 0x42415354;

static std::atomic<bool> HeaderSent{ false };
static std::atomic<bool> ClientAlive{ false };
static BluetoothServer* GlobalServer = nullptr;

// Classic Bluetooth RFCOMM/SPP typically only sustains ~100-250 KB/s in
// practice. Raw 48kHz/32-bit-float stereo audio needs ~384 KB/s. Instead of
// throwing away sample rate or channels (which would cut the top end off the
// audible frequency range), we keep the full 48kHz stereo signal and
// compress it with IMA ADPCM, a standard 4:1 lossy audio codec. That brings
// bandwidth down to roughly 48 KB/s while covering the full 0-20kHz range
// (Nyquist for 48kHz is 24kHz, above the limit of human hearing).

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

static void ResetAdpcmState() {
    LeftState = AdpcmState{};
    RightState = AdpcmState{};
}

static uint8_t AdpcmEncodeSample(AdpcmState& State, int16_t Sample) {
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

static void OnAudioData(const uint8_t* Data, uint32_t Size, uint32_t SampleRate, uint16_t Channels, uint16_t BitsPerSample) {
    if (!GlobalServer || !ClientAlive.load()) return;

    // Assumes the WASAPI mix format is 32-bit IEEE float, which is the
    // standard default output format on Windows.
    if (BitsPerSample != 32 || Channels == 0) return;

    const float* Samples = reinterpret_cast<const float*>(Data);
    uint32_t FrameCount = Size / (Channels * sizeof(float));

    ConversionBuffer.clear();
    ConversionBuffer.reserve(FrameCount);

    auto ToInt16 = [](float V) -> int16_t {
        if (V > 1.0f) V = 1.0f;
        if (V < -1.0f) V = -1.0f;
        return static_cast<int16_t>(V * 32767.0f);
    };

    for (uint32_t i = 0; i < FrameCount; ++i) {
        float Left = Samples[i * Channels + 0];
        float Right = (Channels > 1) ? Samples[i * Channels + 1] : Left;

        uint8_t LeftCode = AdpcmEncodeSample(LeftState, ToInt16(Left));
        uint8_t RightCode = AdpcmEncodeSample(RightState, ToInt16(Right));

        // Pack one stereo frame into a single byte: high nibble = left, low nibble = right
        ConversionBuffer.push_back(static_cast<uint8_t>((LeftCode << 4) | RightCode));
    }

    if (ConversionBuffer.empty()) return;

    uint32_t OutSize = static_cast<uint32_t>(ConversionBuffer.size());

    if (!HeaderSent.load()) {
        printf("Sending header: SampleRate=%u Channels=2 BitsPerSample=16 Codec=IMA_ADPCM\n", SampleRate);
        StreamHeader Header{ StreamMagic, SampleRate, 2, 16, 1 };
        if (!GlobalServer->Send(reinterpret_cast<const uint8_t*>(&Header), sizeof(Header))) {
            printf("Failed to send header, dropping client\n");
            ClientAlive.store(false);
            return;
        }
        HeaderSent.store(true);
    }

    if (!GlobalServer->Send(ConversionBuffer.data(), OutSize)) {
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
        ResetAdpcmState();

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
