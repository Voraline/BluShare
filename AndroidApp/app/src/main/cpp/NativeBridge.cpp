#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <cstdint>
#include <atomic>

static AAudioStream* PlaybackStream = nullptr;
static std::atomic<bool> StreamActive{ false };
static int BytesPerFrame = 2;

extern "C" JNIEXPORT jboolean JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeInit(JNIEnv* Env, jclass Clazz,
    jint SampleRate, jint Channels, jint BitsPerSample, jint Codec) {

    AAudioStreamBuilder* Builder = nullptr;
    if (AAudio_createStreamBuilder(&Builder) != AAUDIO_OK || Builder == nullptr) {
        return JNI_FALSE;
    }

    BytesPerFrame = 2 * Channels;

    AAudioStreamBuilder_setDirection(Builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setSampleRate(Builder, SampleRate);
    AAudioStreamBuilder_setChannelCount(Builder, Channels);
    AAudioStreamBuilder_setFormat(Builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(Builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    aaudio_result_t Result = AAudioStreamBuilder_openStream(Builder, &PlaybackStream);
    AAudioStreamBuilder_delete(Builder);

    if (Result != AAUDIO_OK || PlaybackStream == nullptr) {
        return JNI_FALSE;
    }

    AAudioStream_setBufferSizeInFrames(PlaybackStream, AAudioStream_getFramesPerBurst(PlaybackStream) * 4);

    Result = AAudioStream_requestStart(PlaybackStream);
    if (Result != AAUDIO_OK) {
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

    int32_t FrameCount = Length / BytesPerFrame;
    if (FrameCount > 0) {
        AAudioStream_write(PlaybackStream, Bytes, FrameCount, 50'000'000LL);
    }

    Env->ReleasePrimitiveArrayCritical(Data, Bytes, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeShutdown(JNIEnv* Env, jclass Clazz) {
    StreamActive.store(false, std::memory_order_release);
    if (PlaybackStream != nullptr) {
        AAudioStream_requestStop(PlaybackStream);
        AAudioStream_close(PlaybackStream);
        PlaybackStream = nullptr;
    }
}
