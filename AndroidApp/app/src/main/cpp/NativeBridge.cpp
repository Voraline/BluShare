#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <cstdint>
#include <atomic>

#define LogTag "BluetoothAudioNative"
#define LogError(...) __android_log_print(ANDROID_LOG_ERROR, LogTag, __VA_ARGS__)

static AAudioStream* PlaybackStream = nullptr;
static std::atomic<bool> StreamActive{ false };
static int BytesPerFrame = 4;

extern "C" JNIEXPORT jboolean JNICALL
Java_com_bluetoothaudio_receiver_NativeBridge_NativeInit(JNIEnv* Env, jclass Clazz,
    jint SampleRate, jint Channels, jint BitsPerSample) {

    AAudioStreamBuilder* Builder = nullptr;
    if (AAudio_createStreamBuilder(&Builder) != AAUDIO_OK || Builder == nullptr) {
        LogError("Failed to create stream builder");
        return JNI_FALSE;
    }

    aaudio_format_t Format = (BitsPerSample == 16) ? AAUDIO_FORMAT_PCM_I16 : AAUDIO_FORMAT_PCM_FLOAT;
    BytesPerFrame = ((BitsPerSample == 16) ? 2 : 4) * Channels;

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

    int32_t FrameCount = Length / BytesPerFrame;
    if (FrameCount > 0) {
        AAudioStream_write(PlaybackStream, Bytes, FrameCount, 50'000'000LL);
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
