#include "BluetoothServer.h"
#include "AudioCapture.h"
#include "PacketQueue.h"
#include <windows.h>
#include <opus.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

static constexpr uint32_t TargetSampleRate = 48000;
static constexpr int32_t OpusFrameSamples = 960;
static constexpr int32_t OpusBitrate = 128000;
static constexpr size_t MaxOpusPacketBytes = 1500;

static std::atomic<bool> ClientAlive{ false };
static BluetoothServer* GlobalServer = nullptr;
static OpusEncoder* Encoder = nullptr;
static PacketQueue SendQueue(32);

static std::vector<float> ResampleInput;
static double ResamplePos = 0.0;
static std::vector<int16_t> EncodeAccum;
static std::vector<float> MonoScratch;
static std::vector<uint8_t> EncodedPacket;

static uint64_t KbpsBytesAccum = 0;
static ULONGLONG KbpsWindowStartTick = 0;

static void UpdateKbpsDisplay(uint32_t Bytes) {
    KbpsBytesAccum += Bytes;
    ULONGLONG Now = GetTickCount64();
    if (KbpsWindowStartTick == 0) KbpsWindowStartTick = Now;
    ULONGLONG Elapsed = Now - KbpsWindowStartTick;
    if (Elapsed >= 1000) {
        double Kbps = (KbpsBytesAccum * 8.0) / static_cast<double>(Elapsed);
        printf("\r%.1f", Kbps);
        fflush(stdout);
        KbpsBytesAccum = 0;
        KbpsWindowStartTick = Now;
    }
}

static inline int16_t FloatToInt16(float Value) {
    if (Value > 1.0f) Value = 1.0f;
    if (Value < -1.0f) Value = -1.0f;
    return static_cast<int16_t>(Value * 32767.0f);
}

static void ResetStreamState() {
    ResampleInput.clear();
    ResamplePos = 0.0;
    EncodeAccum.clear();
}

static void ResampleAppend(const float* Mono, uint32_t FrameCount, uint32_t SourceRate) {
    if (SourceRate == TargetSampleRate) {
        for (uint32_t I = 0; I < FrameCount; ++I) {
            EncodeAccum.push_back(FloatToInt16(Mono[I]));
        }
        return;
    }

    ResampleInput.insert(ResampleInput.end(), Mono, Mono + FrameCount);

    double Ratio = static_cast<double>(SourceRate) / static_cast<double>(TargetSampleRate);

    while (true) {
        size_t Index = static_cast<size_t>(ResamplePos);
        if (Index + 1 >= ResampleInput.size()) break;

        double Frac = ResamplePos - static_cast<double>(Index);
        float Sample = static_cast<float>(ResampleInput[Index] * (1.0 - Frac) + ResampleInput[Index + 1] * Frac);
        EncodeAccum.push_back(FloatToInt16(Sample));

        ResamplePos += Ratio;
    }

    size_t Consumed = static_cast<size_t>(ResamplePos);
    if (Consumed >= ResampleInput.size()) {
        ResamplePos -= static_cast<double>(ResampleInput.size());
        ResampleInput.clear();
    } else if (Consumed > 0) {
        ResampleInput.erase(ResampleInput.begin(), ResampleInput.begin() + Consumed);
        ResamplePos -= static_cast<double>(Consumed);
    }
}

static bool EncodeAndSend() {
    while (EncodeAccum.size() >= static_cast<size_t>(OpusFrameSamples)) {
        EncodedPacket.resize(MaxOpusPacketBytes);
        int32_t Bytes = opus_encode(Encoder, EncodeAccum.data(), OpusFrameSamples,
            EncodedPacket.data(), static_cast<int32_t>(EncodedPacket.size()));

        EncodeAccum.erase(EncodeAccum.begin(), EncodeAccum.begin() + OpusFrameSamples);

        if (Bytes < 0) return false;

        uint16_t PacketLength = static_cast<uint16_t>(Bytes);
        std::vector<uint8_t> Frame(sizeof(PacketLength) + static_cast<size_t>(Bytes));
        memcpy(Frame.data(), &PacketLength, sizeof(PacketLength));
        memcpy(Frame.data() + sizeof(PacketLength), EncodedPacket.data(), static_cast<size_t>(Bytes));
        SendQueue.Push(std::move(Frame));

        UpdateKbpsDisplay(static_cast<uint32_t>(Frame.size()));
    }
    return true;
}

static void SenderThreadProc() {
    std::vector<uint8_t> Frame;
    while (SendQueue.Pop(Frame)) {
        if (!GlobalServer->Send(Frame.data(), static_cast<uint32_t>(Frame.size()))) {
            ClientAlive.store(false, std::memory_order_relaxed);
        }
    }
}

static void OnAudioData(const uint8_t* Data, uint32_t Size, uint32_t SampleRate, uint16_t Channels, uint16_t BitsPerSample) {
    if (!GlobalServer || !ClientAlive.load(std::memory_order_relaxed)) return;
    if (BitsPerSample != 32 || Channels == 0) return;

    const float* Samples = reinterpret_cast<const float*>(Data);
    uint32_t FrameCount = Size / (Channels * sizeof(float));
    if (FrameCount == 0) return;

    MonoScratch.resize(FrameCount);
    for (uint32_t I = 0; I < FrameCount; ++I) {
        float Left = Samples[I * Channels + 0];
        float Right = (Channels > 1) ? Samples[I * Channels + 1] : Left;
        MonoScratch[I] = (Channels > 1) ? (Left + Right) * 0.5f : Left;
    }

    ResampleAppend(MonoScratch.data(), FrameCount, SampleRate);

    if (!EncodeAndSend()) {
        ClientAlive.store(false, std::memory_order_relaxed);
        ResetStreamState();
    }
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    AudioCapture Capture;
    if (!Capture.Initialize()) return 1;

    BluetoothServer Server;
    GlobalServer = &Server;
    if (!Server.Start()) return 1;

    int EncoderError = 0;
    Encoder = opus_encoder_create(TargetSampleRate, 1, OPUS_APPLICATION_AUDIO, &EncoderError);
    if (Encoder == nullptr || EncoderError != OPUS_OK) return 1;

    opus_encoder_ctl(Encoder, OPUS_SET_BITRATE(OpusBitrate));
    opus_encoder_ctl(Encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(Encoder, OPUS_SET_COMPLEXITY(8));
    opus_encoder_ctl(Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(Encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(Encoder, OPUS_SET_PACKET_LOSS_PERC(10));

    EncodedPacket.reserve(MaxOpusPacketBytes);

    while (true) {
        if (!Server.WaitForClient()) continue;

        ClientAlive.store(true, std::memory_order_relaxed);
        ResetStreamState();
        SendQueue.Reopen();

        std::thread SenderThread(SenderThreadProc);

        if (!Capture.Start(OnAudioData)) {
            ClientAlive.store(false, std::memory_order_relaxed);
            SendQueue.Close();
            SenderThread.join();
            Server.DropClient();
            continue;
        }

        while (ClientAlive.load(std::memory_order_relaxed)) {
            Sleep(200);
        }

        Capture.Stop();
        SendQueue.Close();
        SenderThread.join();
        Server.DropClient();
    }
}