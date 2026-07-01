#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <cstdint>
#include <atomic>
#include <vector>

#define LogTag "BluetoothAudioNative"
#define LogError(...) __android_log_print(ANDROID_LOG_ERROR, LogTag, __VA_ARGS__)

static AAudioStream* PlaybackStream = nullptr;
static std::atomic<bool> StreamActive{ false };
static int BytesPerFrame = 4;
static int ActiveChannels = 2;
static int ActiveCodec = 0; // 0 = raw PCM16, 1 = IMA ADPCM

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

static AdpcmState LeftDecoderState;
static AdpcmState RightDecoderState;

static int16_t AdpcmDecodeSample(AdpcmState& State, uint8_t Code) {
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeInit(JNIEnv* Env, jclass Clazz,
    jint SampleRate, jint Channels, jint BitsPerSample, jint Codec) {

    AAudioStreamBuilder* Builder = nullptr;
    if (AAudio_createStreamBuilder(&Builder) != AAUDIO_OK || Builder == nullptr) {
        LogError("Failed to create stream builder");
        return JNI_FALSE;
    }

    // ADPCM always decodes to 16-bit PCM regardless of the original bit depth.
    aaudio_format_t Format = (Codec == 1 || BitsPerSample == 16) ? AAUDIO_FORMAT_PCM_I16 : AAUDIO_FORMAT_PCM_FLOAT;
    BytesPerFrame = ((BitsPerSample == 16) ? 2 : 4) * Channels;
    ActiveChannels = Channels;
    ActiveCodec = Codec;

    LeftDecoderState = AdpcmState{};
    RightDecoderState = AdpcmState{};

    AAudioStreamBuilder_setDirection(Builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(Builder, SampleRate);
    AAudioStreamBuilder_setChannelCount(Builder, Channels);
    AAudioStreamBuilder_setFormat(Builder, Format);
    // LOW_LATENCY uses a very small internal buffer, which is great for
    // games/instruments but leaves no cushion for Bluetooth's natural
    // timing jitter -- that's what causes the audible "tick" when the
    // buffer briefly runs dry. NONE uses a larger, more forgiving buffer;
    // a bit more latency here is a good tradeoff since this isn't a
    // latency-sensitive use case.
    AAudioStreamBuilder_setPerformanceMode(Builder, AAUDIO_PERFORMANCE_MODE_NONE);

    aaudio_result_t Result = AAudioStreamBuilder_openStream(Builder, &PlaybackStream);
    AAudioStreamBuilder_delete(Builder);

    if (Result != AAUDIO_OK || PlaybackStream == nullptr) {
        LogError("Failed to open stream: %d", Result);
        return JNI_FALSE;
    }

    // Grow the buffer to its full capacity for extra headroom against
    // jitter from the Bluetooth link.
    int32_t Capacity = AAudioStream_getBufferCapacityInFrames(PlaybackStream);
    if (Capacity > 0) {
        AAudioStream_setBufferSizeInFrames(PlaybackStream, Capacity);
    }

    Result = AAudioStream_requestStart(PlaybackStream);
    if (Result != AAUDIO_OK) {
        LogError("Failed to start stream: %d", Result);
        AAudioStream_close(PlaybackStream);
        PlaybackStream = nullptr;
        return JNI_FALSE;
    }

    StreamActive.store(true);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeWrite(JNIEnv* Env, jclass Clazz,
    jbyteArray Data, jint Length) {

    if (!StreamActive.load() || PlaybackStream == nullptr || Length <= 0) return;

    jbyte* Bytes = Env->GetByteArrayElements(Data, nullptr);
    if (Bytes == nullptr) return;

    if (ActiveCodec == 1) {
        // Each input byte is one stereo frame: high nibble = left ADPCM
        // code, low nibble = right ADPCM code. Decode to 16-bit PCM.
        static std::vector<int16_t> DecodeBuffer;
        DecodeBuffer.clear();
        DecodeBuffer.reserve(static_cast<size_t>(Length) * 2);

        for (jint i = 0; i < Length; ++i) {
            uint8_t Byte = static_cast<uint8_t>(Bytes[i]);
            uint8_t LeftCode = (Byte >> 4) & 0x0F;
            uint8_t RightCode = Byte & 0x0F;

            DecodeBuffer.push_back(AdpcmDecodeSample(LeftDecoderState, LeftCode));
            if (ActiveChannels > 1) {
                DecodeBuffer.push_back(AdpcmDecodeSample(RightDecoderState, RightCode));
            }
        }

        int32_t FrameCount = static_cast<int32_t>(DecodeBuffer.size() / ActiveChannels);
        if (FrameCount > 0) {
            AAudioStream_write(PlaybackStream, DecodeBuffer.data(), FrameCount, 50'000'000LL);
        }
    } else {
        int32_t FrameCount = Length / BytesPerFrame;
        if (FrameCount > 0) {
            AAudioStream_write(PlaybackStream, Bytes, FrameCount, 50'000'000LL);
        }
    }

    Env->ReleaseByteArrayElements(Data, Bytes, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeShutdown(JNIEnv* Env, jclass Clazz) {
    StreamActive.store(false);
    if (PlaybackStream != nullptr) {
        AAudioStream_requestStop(PlaybackStream);
        AAudioStream_close(PlaybackStream);
        PlaybackStream = nullptr;
    }
}
