#pragma once

#include "CoreMinimal.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticReverbData.h"
#include "DSP/MultithreadedPatching.h"

class UAudioComponent;
class USoundBase;

struct FIMAcousticMetaSoundBlock
{
	uint64 Frame = 0, Sequence = 0;
	double Seconds = 0;
	float ListenerX = 0, ListenerY = 0, ListenerZ = 0;
	bool Fresh = false;
};

// Focused ordinary-entry negative control.  The GT test publishes one request
// at a time; the source operator consumes it on the audio thread and applies
// the same result-identity predicate as a worker publication.  The bounded
// atomics keep the test-only handoff allocation-free and leave user audio
// parameters out of the production contract.
struct FIMAcousticIdentityInjectionProbe
{
	std::atomic<uint32> Observed{0}, Rejected{0}, FiniteOutput{0}, RejectDetail{0};
	std::atomic<uint64> RenderedDelta{0}, RejectedDelta{0};
	std::atomic<uint64> InjectedAudioId{0}, InjectedWorldGeneration{0}, InjectedVoiceGeneration{0}, InjectedSequence{0};
	std::atomic<uint64> ActualAudioId{0}, ActualWorldGeneration{0}, ActualVoiceGeneration{0};
	std::atomic<float> FrozenListenerX{0}, FrozenListenerY{0}, FrozenListenerZ{0};
};

struct FIMAcousticIdentityInjection
{
	std::atomic<uint32> PendingKind{0}; // 1 = bad AudioComponentId, 2 = missing identity fields
	std::atomic<uint64> AudioId{0}, WorldGeneration{0}, VoiceGeneration{0}, Sequence{0};
	std::atomic<float> ListenerX{0}, ListenerY{0}, ListenerZ{0};
	std::array<FIMAcousticIdentityInjectionProbe, 2> Probes{};

	void Request(uint32 Kind, uint64 InAudioId, uint64 InWorldGeneration,
		uint64 InVoiceGeneration, uint64 InSequence, float InListenerX,
		float InListenerY, float InListenerZ)
	{
		AudioId.store(InAudioId, std::memory_order_relaxed);
		WorldGeneration.store(InWorldGeneration, std::memory_order_relaxed);
		VoiceGeneration.store(InVoiceGeneration, std::memory_order_relaxed);
		Sequence.store(InSequence, std::memory_order_relaxed);
		ListenerX.store(InListenerX, std::memory_order_relaxed);
		ListenerY.store(InListenerY, std::memory_order_relaxed);
		ListenerZ.store(InListenerZ, std::memory_order_relaxed);
		PendingKind.store(Kind, std::memory_order_release);
	}
};

// GT creates the world binding before any generator is started. Operators only
// acquire it during construction; Execute never looks up UObjects or registries.
struct FIMAcousticMetaSoundContext
{
	static constexpr uint32 Frames = 512;
	static constexpr uint32 MaxVoices = 64;
	TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> Device;
	TSharedPtr<FIMAcousticReverbPool, ESPMode::ThreadSafe> Pool;
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
	TArray<FIMAcousticBlockProbe> CapturedSourceBlocks;
	TArray<FIMAcousticMetaSoundBlock> CapturedBlocks;
	FIMAcousticIdentityInjection IdentityInjection;
	std::array<std::atomic<float>, MaxVoices> DistanceCm{};
	std::atomic<uint32> CapturedSourceFrames{0}, CapturedSourceBlockCount{0}, CapturedEnvironmentFrames{0}, CapturedBlockCount{0};
};

using FIMAcousticMetaSoundContextPtr = TSharedPtr<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>;
namespace IMAcousticMetaSound
{
FIMAcousticMetaSoundContextPtr CreateAcousticMetaSoundContext(uint32 DeviceId, uint32 SampleRate, uint64 Epoch);
FIMAcousticMetaSoundContextPtr FindAcousticMetaSoundContext(uint32 DeviceId);
void StopAcousticMetaSoundContext(const FIMAcousticMetaSoundContextPtr& Context);
int32 RegisterAcousticMetaSoundSource(const FIMAcousticMetaSoundContextPtr& Context, uint64 AudioId);
bool IsAcousticMetaSound(const USoundBase* Sound);
void EnableAcousticMetaSoundCaptureForTest(bool Enabled);
}
