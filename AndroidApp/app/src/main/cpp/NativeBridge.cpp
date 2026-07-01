#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <vector>

#define LogTag "BluShareNative"
#define LogError(...) __android_log_print(ANDROID_LOG_ERROR, LogTag, __VA_ARGS__)

static AAudioStream* PlaybackStream = nullptr;
static std::atomic<bool> StreamActive{ false };
static int BytesPerFrame = 4;
static int ActiveChannels = 2;
static int ActiveCodec = 0;

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

struct AdpcmState {
    int Predictor = 0;
    int Index = 0;
};

struct RiceState {
    uint32_t RunningSum = 32;
};

static AdpcmState LeftDecoderState;
static AdpcmState RightDecoderState;
static RiceState LeftRiceDecoder;
static RiceState RightRiceDecoder;
static std::vector<int16_t> DecodeBuffer;
static std::vector<uint8_t> PendingBytes;

static inline int16_t AdpcmDecodeSample(AdpcmState& State, uint8_t Code) {
    int Step = StepSizeTable[State.Index];
    int Diff = Step >> 3;
    if (Code & 4) Diff += Step;
    if (Code & 2) Diff += Step >> 1;
    if (Code & 1) Diff += Step >> 2;

    if (Code & 8) State.Predictor -= Diff;
    else State.Predictor += Diff;

    if (State.Predictor > 32767) State.Predictor = 32767;
    if (State.Predictor < -32768) State.Predictor = -32768;

    State.Index += IndexTable[Code & 0x0F];
    if (State.Index < 0) State.Index = 0;
    if (State.Index > 88) State.Index = 88;

    return static_cast<int16_t>(State.Predictor);
}

static inline int RiceParam(const RiceState& State) {
    uint32_t Mean = State.RunningSum >> 5;
    int K = 0;
    while (K < 3 && (1u << (K + 1)) <= Mean + 1) ++K;
    return K;
}

static inline void RiceUpdate(RiceState& State, uint32_t Value) {
    State.RunningSum += (Value << 1) - (State.RunningSum >> 5);
}

class BitReader {
public:
    BitReader(const uint8_t* InData, size_t InLength) : Data(InData), Length(InLength) {}

    uint32_t ReadBit() {
        if (BytePos >= Length) return 0;
        uint32_t Bit = (Data[BytePos] >> (7 - BitPos)) & 1u;
        if (++BitPos == 8) { BitPos = 0; ++BytePos; }
        return Bit;
    }

    uint32_t ReadBits(int Count) {
        uint32_t Value = 0;
        for (int I = 0; I < Count; ++I) Value = (Value << 1) | ReadBit();
        return Value;
    }

private:
    const uint8_t* Data;
    size_t Length;
    size_t BytePos = 0;
    int BitPos = 0;
};

static inline uint32_t RiceDecode(BitReader& Reader, int K) {
    if (K >= 3) {
        return Reader.ReadBits(3);
    }
    uint32_t Quotient = 0;
    while (Reader.ReadBit()) ++Quotient;
    uint32_t Remainder = (K > 0) ? Reader.ReadBits(K) : 0;
    return (Quotient << K) | Remainder;
}

static void DecodeRiceFrame(const uint8_t* FrameData, uint16_t PayloadLength, uint16_t SampleCount) {
    BitReader Reader(FrameData, PayloadLength);

    DecodeBuffer.clear();
    size_t NeededCapacity = static_cast<size_t>(SampleCount) * static_cast<size_t>(ActiveChannels > 1 ? 2 : 1);
    if (DecodeBuffer.capacity() < NeededCapacity) {
        DecodeBuffer.reserve(NeededCapacity);
    }

    for (uint16_t I = 0; I < SampleCount; ++I) {
        uint32_t LeftSign = Reader.ReadBit();
        int LeftK = RiceParam(LeftRiceDecoder);
        uint32_t LeftMagnitude = RiceDecode(Reader, LeftK);
        RiceUpdate(LeftRiceDecoder, LeftMagnitude);
        uint8_t LeftCode = static_cast<uint8_t>((LeftSign << 3) | (LeftMagnitude & 0x7u));
        DecodeBuffer.push_back(AdpcmDecodeSample(LeftDecoderState, LeftCode));

        if (ActiveChannels > 1) {
            uint32_t RightSign = Reader.ReadBit();
            int RightK = RiceParam(RightRiceDecoder);
            uint32_t RightMagnitude = RiceDecode(Reader, RightK);
            RiceUpdate(RightRiceDecoder, RightMagnitude);
            uint8_t RightCode = static_cast<uint8_t>((RightSign << 3) | (RightMagnitude & 0x7u));
            DecodeBuffer.push_back(AdpcmDecodeSample(RightDecoderState, RightCode));
        }
    }

    int32_t FrameCountOut = static_cast<int32_t>(DecodeBuffer.size() / ActiveChannels);
    if (FrameCountOut > 0) {
        AAudioStream_write(PlaybackStream, DecodeBuffer.data(), FrameCountOut, 50'000'000LL);
    }
}

static void ProcessPendingFrames() {
    size_t Offset = 0;
    while (PendingBytes.size() - Offset >= 4) {
        uint16_t SampleCount = static_cast<uint16_t>(PendingBytes[Offset]) |
            (static_cast<uint16_t>(PendingBytes[Offset + 1]) << 8);
        uint16_t PayloadLength = static_cast<uint16_t>(PendingBytes[Offset + 2]) |
            (static_cast<uint16_t>(PendingBytes[Offset + 3]) << 8);

        if (PendingBytes.size() - Offset < static_cast<size_t>(4 + PayloadLength)) break;

        DecodeRiceFrame(PendingBytes.data() + Offset + 4, PayloadLength, SampleCount);
        Offset += 4 + PayloadLength;
    }

    if (Offset > 0) {
        PendingBytes.erase(PendingBytes.begin(), PendingBytes.begin() + static_cast<long>(Offset));
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeInit(JNIEnv* Env, jclass Clazz,
    jint SampleRate, jint Channels, jint BitsPerSample, jint Codec) {

    AAudioStreamBuilder* Builder = nullptr;
    if (AAudio_createStreamBuilder(&Builder) != AAUDIO_OK || Builder == nullptr) {
        LogError("Failed to create stream builder");
        return JNI_FALSE;
    }

    aaudio_format_t Format = (Codec == 1 || Codec == 2 || BitsPerSample == 16) ? AAUDIO_FORMAT_PCM_I16 : AAUDIO_FORMAT_PCM_FLOAT;
    BytesPerFrame = ((BitsPerSample == 16) ? 2 : 4) * Channels;
    ActiveChannels = Channels;
    ActiveCodec = Codec;

    LeftDecoderState = AdpcmState{};
    RightDecoderState = AdpcmState{};
    LeftRiceDecoder = RiceState{};
    RightRiceDecoder = RiceState{};
    DecodeBuffer.reserve(16384);
    PendingBytes.clear();
    PendingBytes.reserve(16384);

    AAudioStreamBuilder_setDirection(Builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(Builder, SampleRate);
    AAudioStreamBuilder_setChannelCount(Builder, Channels);
    AAudioStreamBuilder_setFormat(Builder, Format);
    AAudioStreamBuilder_setPerformanceMode(Builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    aaudio_result_t Result = AAudioStreamBuilder_openStream(Builder, &PlaybackStream);
    AAudioStreamBuilder_delete(Builder);

    if (Result != AAUDIO_OK || PlaybackStream == nullptr) {
        LogError("Failed to open stream: %d", Result);
        return JNI_FALSE;
    }

    // Keep just enough buffer to absorb normal Bluetooth jitter (a few bursts),
    // not the entire capacity. Full capacity = maximum, unnecessary latency.
    int32_t BurstSize = AAudioStream_getFramesPerBurst(PlaybackStream);
    int32_t Capacity = AAudioStream_getBufferCapacityInFrames(PlaybackStream);
    if (BurstSize > 0 && Capacity > 0) {
        int32_t TargetSize = BurstSize * 3; // tune 2-4x if you hear underrun clicks/pops
        if (TargetSize > Capacity) TargetSize = Capacity;
        AAudioStream_setBufferSizeInFrames(PlaybackStream, TargetSize);
    }

    Result = AAudioStream_requestStart(PlaybackStream);
    if (Result != AAUDIO_OK) {
        LogError("Failed to start stream: %d", Result);
        AAudioStream_close(PlaybackStream);
        PlaybackStream = nullptr;
        return JNI_FALSE;
    }

    StreamActive.store(true, std::memory_order_release);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeWrite(JNIEnv* Env, jclass Clazz,
    jbyteArray Data, jint Length) {

    if (!StreamActive.load(std::memory_order_acquire) || PlaybackStream == nullptr || Length <= 0) return;

    jbyte* Bytes = static_cast<jbyte*>(Env->GetPrimitiveArrayCritical(Data, nullptr));
    if (Bytes == nullptr) return;

    if (ActiveCodec == 2) {
        size_t OldSize = PendingBytes.size();
        PendingBytes.resize(OldSize + static_cast<size_t>(Length));
        std::memcpy(PendingBytes.data() + OldSize, Bytes, static_cast<size_t>(Length));
        Env->ReleasePrimitiveArrayCritical(Data, Bytes, JNI_ABORT);
        ProcessPendingFrames();
    } else if (ActiveCodec == 1) {
        DecodeBuffer.clear();
        size_t NeededCapacity = static_cast<size_t>(Length) * 2;
        if (DecodeBuffer.capacity() < NeededCapacity) {
            DecodeBuffer.reserve(NeededCapacity);
        }

        for (jint I = 0; I < Length; ++I) {
            uint8_t Byte = static_cast<uint8_t>(Bytes[I]);
            uint8_t LeftCode = (Byte >> 4) & 0x0F;
            uint8_t RightCode = Byte & 0x0F;

            DecodeBuffer.push_back(AdpcmDecodeSample(LeftDecoderState, LeftCode));
            if (ActiveChannels > 1) {
                DecodeBuffer.push_back(AdpcmDecodeSample(RightDecoderState, RightCode));
            }
        }

        Env->ReleasePrimitiveArrayCritical(Data, Bytes, JNI_ABORT);

        int32_t FrameCount = static_cast<int32_t>(DecodeBuffer.size() / ActiveChannels);
        if (FrameCount > 0) {
            AAudioStream_write(PlaybackStream, DecodeBuffer.data(), FrameCount, 50'000'000LL);
        }
    } else {
        int32_t FrameCount = Length / BytesPerFrame;
        if (FrameCount > 0) {
            AAudioStream_write(PlaybackStream, Bytes, FrameCount, 50'000'000LL);
        }
        Env->ReleasePrimitiveArrayCritical(Data, Bytes, JNI_ABORT);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeShutdown(JNIEnv* Env, jclass Clazz) {
    StreamActive.store(false, std::memory_order_release);
    if (PlaybackStream != nullptr) {
        AAudioStream_requestStop(PlaybackStream);
        AAudioStream_close(PlaybackStream);
        PlaybackStream = nullptr;
    }
    PendingBytes.clear();
}
