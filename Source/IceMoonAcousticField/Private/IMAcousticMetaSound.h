#pragma once

#include "CoreMinimal.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticReverbData.h"
#include "DSP/MultithreadedPatching.h"

class UAudioComponent;
class USoundBase;

struct IM_AcousticMetaSoundBlock
{
    uint64 Frame = 0, Sequence = 0;
    double Seconds = 0;
    float ListenerX = 0, ListenerY = 0, ListenerZ = 0;
    bool Fresh = false;
};

// GT creates the world binding before any generator is started. Operators only
// acquire it during construction; Execute never looks up UObjects or registries.
struct IM_AcousticMetaSoundContext
{
    static constexpr uint32 Frames = 512;
    static constexpr uint32 MaxVoices = 64;
    TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> Device;
    TSharedPtr<IM_AcousticReverbPool, ESPMode::ThreadSafe> Pool;
    std::array<std::atomic<uint64>, MaxVoices> AudioIds{};
    // Per-source authored volume. ReverbSendGains applies the independent room
    // coupling curve below; it must not be replaced with direct attenuation.
    std::array<std::atomic<float>, MaxVoices> SendGains{};
    std::array<std::atomic<float>, MaxVoices> ReverbSendGains{};
    std::array<std::atomic<float>, MaxVoices> DistanceGains{};
    std::array<std::atomic<bool>, MaxVoices> SourceConsumers{};
    std::atomic<bool> EnvironmentConsumer{false};
    std::atomic<bool> BusReaderConsumer{false};
    // GT publishes the patch before constructing either graph. Its producer is
    // the mixer bus; exactly one graph operator consumes it. No UObject lookup
    // or patch construction occurs in Execute.
    Audio::FPatchOutputStrongPtr BusPatch;
    uint32 BusId = 0;
    int32 BusPrimeFrames = 4096;
    std::atomic<uint64> BusUnderruns{0}, BusReadBlocks{0}, BusPrimingBlocks{0};
    std::atomic<int32> BusMinAvailable{MAX_int32}, BusMaxAvailable{0};
    std::atomic<bool> Stopped{false};
    std::atomic<float> WetGain{0.25f};
    std::atomic<uint64> SourceBlocks{0}, EnvironmentBlocks{0}, InvalidBlocks{0};
    std::atomic<uint64> DuplicateConsumers{0}, NoIRBlocks{0}, LastIRSequence{0};
    std::atomic<uint64> SourceFrames{0}, EnvironmentFrames{0};
    uint32 DeviceId = 0;
    // Optional bounded diagnostics allocated on GT before construction. A single
    // source/environment writer publishes a completed prefix with release.
    TArray<float> CapturedSource, CapturedDry, CapturedWet, CapturedBus;
    // Graph-native source-block telemetry. This deliberately reuses the POD
    // probe shape used by the legacy adapter, but is written by the MetaSound
    // source operator itself so W4 cannot mistake an empty legacy callback ring
    // for silence on the new graph.
    TArray<IM_AcousticBlockProbe> CapturedSourceBlocks;
    TArray<IM_AcousticMetaSoundBlock> CapturedBlocks;
    std::array<std::atomic<float>, MaxVoices> DistanceCm{};
    std::atomic<uint32> CapturedSourceFrames{0}, CapturedSourceBlockCount{0}, CapturedEnvironmentFrames{0}, CapturedBlockCount{0};
};

using IM_AcousticMetaSoundContextPtr = TSharedPtr<IM_AcousticMetaSoundContext, ESPMode::ThreadSafe>;
IM_AcousticMetaSoundContextPtr IM_CreateAcousticMetaSoundContext(uint32 DeviceId, uint32 SampleRate, uint64 Epoch);
IM_AcousticMetaSoundContextPtr IM_FindAcousticMetaSoundContext(uint32 DeviceId);
void IM_StopAcousticMetaSoundContext(const IM_AcousticMetaSoundContextPtr& Context);
int32 IM_RegisterAcousticMetaSoundSource(const IM_AcousticMetaSoundContextPtr& Context, uint64 AudioId);
bool IM_IsAcousticMetaSound(const USoundBase* Sound);
void IM_EnableAcousticMetaSoundCaptureForTest(bool Enabled);
