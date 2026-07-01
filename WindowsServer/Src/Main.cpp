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
static constexpr uint8_t CodecRiceAdpcm = 2;
static constexpr uint32_t MaxChunkFrames = 65535;

static std::atomic<bool> HeaderSent{ false };
static std::atomic<bool> ClientAlive{ false };
static BluetoothServer* GlobalServer = nullptr;

struct AdpcmState {
    int Predictor = 0;
    int Index = 0;
};

struct RiceState {
    uint32_t RunningSum = 32;
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
static RiceState LeftRice;
static RiceState RightRice;
static std::vector<uint8_t> BitBuffer;
static std::vector<uint8_t> FrameBuffer;

static inline void ResetCodecState() {
    LeftState = AdpcmState{};
    RightState = AdpcmState{};
    LeftRice = RiceState{};
    RightRice = RiceState{};
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

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& Out) : Output(Out) {}

    void WriteBit(uint32_t Bit) {
        Accumulator = (Accumulator << 1) | (Bit & 1u);
        if (++BitCount == 8) {
            Output.push_back(static_cast<uint8_t>(Accumulator));
            Accumulator = 0;
            BitCount = 0;
        }
    }

    void WriteBits(uint32_t Value, int Count) {
        for (int I = Count - 1; I >= 0; --I) {
            WriteBit((Value >> I) & 1u);
        }
    }

    void Flush() {
        while (BitCount != 0) WriteBit(0);
    }

private:
    std::vector<uint8_t>& Output;
    uint32_t Accumulator = 0;
    int BitCount = 0;
};

static inline int RiceParam(const RiceState& State) {
    uint32_t Mean = State.RunningSum >> 5;
    int K = 0;
    while (K < 3 && (1u << (K + 1)) <= Mean + 1) ++K;
    return K;
}

static inline void RiceUpdate(RiceState& State, uint32_t Value) {
    State.RunningSum += (Value << 1) - (State.RunningSum >> 5);
}

static inline void RiceEncode(BitWriter& Writer, uint32_t Value, int K) {
    if (K >= 3) {
        Writer.WriteBits(Value & 0x7u, 3);
        return;
    }
    uint32_t Quotient = Value >> K;
    while (Quotient--) Writer.WriteBit(1);
    Writer.WriteBit(0);
    if (K > 0) Writer.WriteBits(Value & ((1u << K) - 1), K);
}

static bool SendChunk(const float* Samples, uint32_t ChunkFrames, uint16_t Channels, uint32_t SampleRate) {
    BitBuffer.clear();
    {
        BitWriter Writer(BitBuffer);
        for (uint32_t I = 0; I < ChunkFrames; ++I) {
            float Left = Samples[I * Channels + 0];
            float Right = (Channels > 1) ? Samples[I * Channels + 1] : Left;

            uint8_t LeftCode = AdpcmEncodeSample(LeftState, FloatToInt16(Left));
            uint32_t LeftMagnitude = LeftCode & 0x7u;
            int LeftK = RiceParam(LeftRice);
            Writer.WriteBit((LeftCode >> 3) & 1u);
            RiceEncode(Writer, LeftMagnitude, LeftK);
            RiceUpdate(LeftRice, LeftMagnitude);

            uint8_t RightCode = AdpcmEncodeSample(RightState, FloatToInt16(Right));
            uint32_t RightMagnitude = RightCode & 0x7u;
            int RightK = RiceParam(RightRice);
            Writer.WriteBit((RightCode >> 3) & 1u);
            RiceEncode(Writer, RightMagnitude, RightK);
            RiceUpdate(RightRice, RightMagnitude);
        }
        Writer.Flush();
    }

    if (BitBuffer.empty()) return true;

    uint16_t SampleCount = static_cast<uint16_t>(ChunkFrames);
    uint16_t PayloadLength = static_cast<uint16_t>(BitBuffer.size());

    FrameBuffer.clear();
    FrameBuffer.reserve(4 + BitBuffer.size());
    FrameBuffer.push_back(static_cast<uint8_t>(SampleCount & 0xFF));
    FrameBuffer.push_back(static_cast<uint8_t>((SampleCount >> 8) & 0xFF));
    FrameBuffer.push_back(static_cast<uint8_t>(PayloadLength & 0xFF));
    FrameBuffer.push_back(static_cast<uint8_t>((PayloadLength >> 8) & 0xFF));
    FrameBuffer.insert(FrameBuffer.end(), BitBuffer.begin(), BitBuffer.end());

    if (!HeaderSent.load(std::memory_order_relaxed)) {
        StreamHeader Header{ StreamMagic, SampleRate, 2, 16, CodecRiceAdpcm };
        if (!GlobalServer->Send(reinterpret_cast<const uint8_t*>(&Header), sizeof(Header))) {
            return false;
        }
        HeaderSent.store(true, std::memory_order_relaxed);
    }

    return GlobalServer->Send(FrameBuffer.data(), static_cast<uint32_t>(FrameBuffer.size()));
}

static void OnAudioData(const uint8_t* Data, uint32_t Size, uint32_t SampleRate, uint16_t Channels, uint16_t BitsPerSample) {
    if (!GlobalServer || !ClientAlive.load(std::memory_order_relaxed)) return;
    if (BitsPerSample != 32 || Channels == 0) return;

    const float* Samples = reinterpret_cast<const float*>(Data);
    uint32_t FrameCount = Size / (Channels * sizeof(float));
    if (FrameCount == 0) return;

    uint32_t Offset = 0;
    while (Offset < FrameCount) {
        uint32_t ChunkFrames = FrameCount - Offset;
        if (ChunkFrames > MaxChunkFrames) ChunkFrames = MaxChunkFrames;

        if (!SendChunk(Samples + static_cast<size_t>(Offset) * Channels, ChunkFrames, Channels, SampleRate)) {
            ClientAlive.store(false, std::memory_order_relaxed);
            HeaderSent.store(false, std::memory_order_relaxed);
            return;
        }

        Offset += ChunkFrames;
    }
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    AudioCapture Capture;
    if (!Capture.Initialize()) return 1;

    BluetoothServer Server;
    GlobalServer = &Server;
    if (!Server.Start()) return 1;

    BitBuffer.reserve(4096);
    FrameBuffer.reserve(4096);

    while (true) {
        if (!Server.WaitForClient()) continue;

        HeaderSent.store(false, std::memory_order_relaxed);
        ClientAlive.store(true, std::memory_order_relaxed);
        ResetCodecState();

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
