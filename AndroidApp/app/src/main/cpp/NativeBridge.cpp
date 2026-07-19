#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <opus.h>
#include <cstdint>
#include <atomic>

static constexpr int32_t MaxDecodeFrames = 5760;

static AAudioStream* PlaybackStream = nullptr;
static OpusDecoder* Decoder = nullptr;
static std::atomic<bool> StreamActive{ false };
static int BytesPerFrame = 2;
static int16_t DecodeScratch[MaxDecodeFrames * 2];

extern "C" JNIEXPORT jboolean JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeInit(JNIEnv* Env, jclass Clazz,
    jint SampleRate, jint Channels) {

    AAudioStreamBuilder* Builder = nullptr;
    if (AAudio_createStreamBuilder(&Builder) != AAUDIO_OK || Builder == nullptr) {
        return JNI_FALSE;
    }

    BytesPerFrame = 2 * Channels;

    int DecoderError = 0;
    Decoder = opus_decoder_create(SampleRate, Channels, &DecoderError);
    if (Decoder == nullptr || DecoderError != OPUS_OK) {
        AAudioStreamBuilder_delete(Builder);
        return JNI_FALSE;
    }

    AAudioStreamBuilder_setDirection(Builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setSampleRate(Builder, SampleRate);
    AAudioStreamBuilder_setChannelCount(Builder, Channels);
    AAudioStreamBuilder_setFormat(Builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(Builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    aaudio_result_t Result = AAudioStreamBuilder_openStream(Builder, &PlaybackStream);
    AAudioStreamBuilder_delete(Builder);

    if (Result != AAUDIO_OK || PlaybackStream == nullptr) {
        opus_decoder_destroy(Decoder);
        Decoder = nullptr;
        return JNI_FALSE;
    }

    AAudioStream_setBufferSizeInFrames(PlaybackStream, AAudioStream_getFramesPerBurst(PlaybackStream) * 8);

    Result = AAudioStream_requestStart(PlaybackStream);
    if (Result != AAUDIO_OK) {
        AAudioStream_close(PlaybackStream);
        PlaybackStream = nullptr;
        opus_decoder_destroy(Decoder);
        Decoder = nullptr;
        return JNI_FALSE;
    }

    StreamActive.store(true, std::memory_order_release);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeWrite(JNIEnv* Env, jclass Clazz,
    jbyteArray Data, jint Length) {

    if (!StreamActive.load(std::memory_order_acquire) || PlaybackStream == nullptr || Decoder == nullptr || Length <= 0) return;

    jbyte* Bytes = static_cast<jbyte*>(Env->GetPrimitiveArrayCritical(Data, nullptr));
    if (Bytes == nullptr) return;

    int32_t FrameCount = opus_decode(Decoder, reinterpret_cast<const unsigned char*>(Bytes), Length,
        DecodeScratch, MaxDecodeFrames, 0);

    Env->ReleasePrimitiveArrayCritical(Data, Bytes, JNI_ABORT);

    if (FrameCount > 0) {
        AAudioStream_write(PlaybackStream, DecodeScratch, FrameCount, 50'000'000LL);
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
    if (Decoder != nullptr) {
        opus_decoder_destroy(Decoder);
        Decoder = nullptr;
    }
}