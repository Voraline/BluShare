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
static constexpr uint8_t CodecPredictiveRice = 3;
static constexpr uint8_t CodecLosslessRice = 4;
static constexpr bool UseLossless = true;
static constexpr uint32_t MaxChunkFrames = 480;
static constexpr int32_t MinStepSize = 2;
static constexpr int32_t MaxStepSize = 512;
static constexpr uint32_t RiceEscapeThreshold = 24;
static constexpr int MaxRiceParam = 20;

static std::atomic<bool> HeaderSent{ false };
static std::atomic<bool> ClientAlive{ false };
static BluetoothServer* GlobalServer = nullptr;

struct PredictorState {
    int32_t Prev1 = 0;
    int32_t Prev2 = 0;
    int32_t StepSize = 16;
};

struct RiceState {
    uint32_t RunningSum = 32;
};

struct LosslessPredictorState {
    int32_t Prev1 = 0;
    int32_t Prev2 = 0;
};

static PredictorState MonoPredictor;
static LosslessPredictorState MonoLossless;
static RiceState MonoRice;
static std::vector<uint8_t> BitBuffer;
static std::vector<uint8_t> FrameBuffer;

static inline void ResetCodecState() {
    MonoPredictor = PredictorState{};
    MonoLossless = LosslessPredictorState{};
    MonoRice = RiceState{};
}

static inline int16_t ClampSample(int32_t Value) {
    if (Value > 32767) return 32767;
    if (Value < -32768) return -32768;
    return static_cast<int16_t>(Value);
}

static inline int16_t FloatToInt16(float Value) {
    if (Value > 1.0f) Value = 1.0f;
    if (Value < -1.0f) Value = -1.0f;
    return static_cast<int16_t>(Value * 32767.0f);
}

static inline uint32_t ZigzagEncode(int32_t Value) {
    return (static_cast<uint32_t>(Value) << 1) ^ static_cast<uint32_t>(Value >> 31);
}

static inline void AdaptStep(int32_t& StepSize, int32_t AbsLevel) {
    if (AbsLevel <= 1) {
        StepSize -= StepSize >> 4;
    } else {
        StepSize += (StepSize * (AbsLevel - 1)) >> 2;
    }
    if (StepSize < MinStepSize) StepSize = MinStepSize;
    if (StepSize > MaxStepSize) StepSize = MaxStepSize;
}

static inline uint32_t EncodeSample(PredictorState& State, int16_t Sample) {
    int32_t Predicted = ClampSample(2 * State.Prev1 - State.Prev2);
    int32_t Diff = static_cast<int32_t>(Sample) - Predicted;
    int32_t Step = State.StepSize;
    int32_t Half = Step >> 1;
    int32_t Level = (Diff >= 0) ? (Diff + Half) / Step : -((-Diff + Half) / Step);
    int32_t Reconstructed = ClampSample(Predicted + Level * Step);

    State.Prev2 = State.Prev1;
    State.Prev1 = Reconstructed;
    AdaptStep(State.StepSize, Level < 0 ? -Level : Level);

    return ZigzagEncode(Level);
}

static inline uint32_t EncodeSampleLossless(LosslessPredictorState& State, int16_t Sample) {
    int32_t Predicted = 2 * State.Prev1 - State.Prev2;
    int32_t Diff = static_cast<int32_t>(Sample) - Predicted;

    State.Prev2 = State.Prev1;
    State.Prev1 = Sample;

    return ZigzagEncode(Diff);
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
    while (K < MaxRiceParam && (1u << (K + 1)) <= Mean + 1) ++K;
    return K;
}

static inline void RiceUpdate(RiceState& State, uint32_t Value) {
    State.RunningSum += (Value << 1) - (State.RunningSum >> 5);
}

static inline void RiceEncode(BitWriter& Writer, uint32_t Value, int K) {
    uint32_t Quotient = Value >> K;
    if (Quotient < RiceEscapeThreshold) {
        for (uint32_t I = 0; I < Quotient; ++I) Writer.WriteBit(1);
        Writer.WriteBit(0);
        if (K > 0) Writer.WriteBits(Value & ((1u << K) - 1), K);
    } else {
        for (uint32_t I = 0; I < RiceEscapeThreshold; ++I) Writer.WriteBit(1);
        Writer.WriteBits(Value, 32);
    }
}

static bool SendChunk(const float* Samples, uint32_t ChunkFrames, uint16_t Channels, uint32_t SampleRate) {
    BitBuffer.clear();
    {
        BitWriter Writer(BitBuffer);
        for (uint32_t I = 0; I < ChunkFrames; ++I) {
            float Left = Samples[I * Channels + 0];
            float Right = (Channels > 1) ? Samples[I * Channels + 1] : Left;
            float Mono = (Channels > 1) ? (Left + Right) * 0.5f : Left;

            int16_t Int16Sample = FloatToInt16(Mono);
            uint32_t Code = UseLossless ? EncodeSampleLossless(MonoLossless, Int16Sample)
                                         : EncodeSample(MonoPredictor, Int16Sample);
            int K = RiceParam(MonoRice);
            RiceEncode(Writer, Code, K);
            RiceUpdate(MonoRice, Code);
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
        StreamHeader Header{ StreamMagic, SampleRate, 1, 16, UseLossless ? CodecLosslessRice : CodecPredictiveRice };
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
