#pragma once

#include <memory>

#include "CoreMinimal.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticReverbData.h"
#include "IMAcousticExecuteTimingProbe.h"
#include "DSP/MultithreadedPatching.h"

class UAudioComponent;
class USoundBase;

struct FIMAcousticMetaSoundBlock
{
	uint64 Frame = 0, Sequence = 0, SpatialVersion = 0, ExpectedSpatialVersion = 0;
	double Seconds = 0;
	float ListenerX = 0, ListenerY = 0, ListenerZ = 0;
	bool Fresh = false;
	// Opt-in POD diagnostics. Snapshot the exact Fresh decision before a failed
	// block releases Current; the single environment owner release-publishes it.
	double CapturedSeconds = 0.0;
	uint64 InputSpatialVersion = 0, DeviceWorldGeneration = 0, PoolWorldGeneration = 0;
	uint32 RenderRoutes = 0;
	bool HasCurrent = false, DelayPriming = false, Enabled = false;
	bool RenderAttempted = false, Rendered = false;
};

enum class EIMAcousticBusReadResult : uint8
{
	Stopped, InvalidFrames, LatchedFault, NoInputPriming, NoInputSilence,
	ReservePriming, Complete, ShortAvailable, ShortPop
};

// Optional Q5 observation, not a device PCM clock. One admitted reader writes
// each GT-preallocated row once, then release-publishes the immutable prefix.
struct FIMAcousticBusReadBlock
{
	uint32 Ordinal = 0;
	int32 Frames = 0, PrimeFrames = 0, CapacityFrames = 0;
	int32 InputCount = 0, ActiveCount = 0, MinAvailable = -1, MaxAvailable = -1;
	int32 BlockingSlot = INDEX_NONE, BlockingAvailable = -1, BlockingQueueAfter = -1;
	int32 MinPopped = -1;
	uint64 BlockingAudioId = 0, BlockingPushBlocks = 0;
	uint64 UnderrunsBefore = 0, UnderrunsAfter = 0;
	uint64 ReadBlocks = 0, PrimingBlocks = 0, SilentBlocks = 0;
	uint64 PushBlocks = 0, PushFailures = 0;
	double BeginSeconds = 0.0, EndSeconds = 0.0;
	EIMAcousticBusReadResult Result = EIMAcousticBusReadResult::Stopped;
	bool PrimedBefore = false, PrimedAfter = false;
	bool TransportFailedBefore = false, TransportFailedAfter = false;
	bool BlockingActive = false, BlockingActiveAfter = false, ReturnedFresh = false;
};

// Q5-only joint observations. Claims order observer calls, never device frames
// or the internal FIFO commit. Every row has one writer and a release ready bit.
enum class EIMAcousticTransportOrderPhase : uint8
{
	SourceEnter, SourceProbeLink, PushBegin, PushReturn, PushSkipped,
	ActiveStore, SourceCompleted, ReleaseBegin, ReleaseInactive,
	ReaderBegin, ActiveLoad, QueueReadBegin, QueueReadReturn, ActiveRecheck,
	PopReturn, ReaderDecision, ReaderEnd, SourceBeforeRenderer, SourceAfterRenderer, Count
};

enum class EIMAcousticTransportOrderReason : uint8
{
	None, MissingProducer, DisabledTransport, StoppedContext, Finished,
	Reset, Destruction, Explicit, InitialQueue, InactiveQueue
};

// Diagnostic snapshots only. Audible flags copy route enable bits, not energy.
enum class EIMAcousticTransportWorkFlag : uint32
{
	Fresh = 1u, FirstAcceptedAttempt = 2u, DirectValid = 4u, PathValid = 8u,
	Rendered = 16u, DirectAudible = 32u, PathAudible = 64u,
	ThreadCpuQueryAttempted = 128u, ThreadCpuQuerySucceeded = 256u
};

struct FIMAcousticTransportOrderEvent
{
	uint32 Index = 0, Group = MAX_uint32, ReaderOrdinal = MAX_uint32, SourceTraceIndex = MAX_uint32;
	uint32 ThreadId = 0, DeviceId = 0, SampleRate = 0;
	int32 Slot = INDEX_NONE;
	uint64 AudioId = 0, WorldGeneration = 0, SourceGeneration = 0;
	double Seconds = 0.0;
	uint64 PushBlocks = 0, Underruns = 0;
	int32 Frames = 0, Available = -1, Returned = -1, Active = -1;
	EIMAcousticTransportOrderPhase Phase = EIMAcousticTransportOrderPhase::SourceEnter;
	EIMAcousticTransportOrderReason Reason = EIMAcousticTransportOrderReason::None;
	bool Finished = false, Fresh = false;
	uint32 WorkFlags = 0; // Reuse alignment padding to retain the 120-byte observation ABI.
	uint64 WorkCycles = 0; // Cycles64/QPC wall ticks on the two Renderer endpoints only.
	uint64 ThreadCpuCycles = 0; // Current Source thread's reported user+kernel raw CPU cycles; never wall ticks.
};
static_assert(sizeof(FIMAcousticTransportOrderEvent) == 120, "Verify the v3 observation ABI during cold compilation.");
static_assert(sizeof(FIMAcousticTransportOrderEvent) <= 128, "Bound the opt-in GT trace payload to 1 MiB.");

// Focused ordinary-entry negative control.  The GT test publishes one request
// at a time; the source operator consumes it on the audio thread and applies
// the same result-identity predicate as a worker publication.  The bounded
// atomics keep the test-only handoff allocation-free and leave user audio
// parameters out of the production contract.
struct FIMAcousticIdentityInjectionProbe
{
	std::atomic<uint32> Observed{0}, Rejected{0}, FiniteOutput{0}, RejectDetail{0};
	std::atomic<uint64> RenderedDelta{0}, RejectedDelta{0};
	std::atomic<uint64> InjectedAudioId{0}, InjectedWorldGeneration{0}, InjectedVoiceGeneration{0}, InjectedSequence{0}, InjectedSpatialVersion{0};
	std::atomic<uint64> ActualAudioId{0}, ActualWorldGeneration{0}, ActualVoiceGeneration{0}, ActualSpatialVersion{0};
	std::atomic<float> FrozenListenerX{0}, FrozenListenerY{0}, FrozenListenerZ{0};
};

struct FIMAcousticIdentityInjection
{
	std::atomic<uint32> PendingKind{0}; // 1 = bad AudioComponentId, 2 = missing identity fields, 3 = frozen-A spatial version
	std::atomic<uint64> AudioId{0}, WorldGeneration{0}, VoiceGeneration{0}, Sequence{0}, SpatialVersion{0};
	std::atomic<float> ListenerX{0}, ListenerY{0}, ListenerZ{0};
	std::array<FIMAcousticIdentityInjectionProbe, 3> Probes{};

	void Request(uint32 Kind, uint64 InAudioId, uint64 InWorldGeneration,
		uint64 InVoiceGeneration, uint64 InSequence, float InListenerX,
		float InListenerY, float InListenerZ, uint64 InSpatialVersion)
	{
		AudioId.store(InAudioId, std::memory_order_relaxed);
		WorldGeneration.store(InWorldGeneration, std::memory_order_relaxed);
		VoiceGeneration.store(InVoiceGeneration, std::memory_order_relaxed);
		Sequence.store(InSequence, std::memory_order_relaxed);
		SpatialVersion.store(InSpatialVersion, std::memory_order_relaxed);
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
	// Keep source PCM and its listener-pose identity aligned while the worker
	// finishes a spatial solve; the environment IR consumer mirrors this delay.
	static constexpr uint32 SourceInputDelayBlocks = 2;
	static constexpr uint32 SourceInputDelayFrames = Frames * SourceInputDelayBlocks;
	static constexpr uint32 MaxVoices = 64;
	static constexpr uint32 TransportOrderEventCapacity = 8192;
	// Source operators may execute concurrently. The preallocated ready bitset
	// lets the exporter report only a contiguous, fully-written prefix. The larger
	// ceiling is allocated only when the formal W3 trace explicitly requests it.
	static constexpr uint32 CapturedSourceBlockReadyMaxWordCount = 16384;
	static constexpr uint32 CapturedSourceBlockTraceDefaultCapacity = 131072;
	static constexpr uint32 CapturedSourceBlockTraceMaxCapacity = CapturedSourceBlockReadyMaxWordCount * 64u;
	static constexpr uint32 CapturedEnvironmentBlockTraceMaxCapacity = 131072;
	TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> Device;
	TSharedPtr<FIMAcousticReverbPool, ESPMode::ThreadSafe> Pool;
	std::array<std::atomic<uint64>, MaxVoices> AudioIds{};
	// Per-source authored volume. ReverbSendGains applies the independent room
	// coupling curve below; it must not be replaced with direct attenuation.
	std::array<std::atomic<float>, MaxVoices> SendGains{};
	std::array<std::atomic<float>, MaxVoices> ReverbSendGains{};
	std::array<std::atomic<float>, MaxVoices> DistanceGains{};
	// GT-published IMAcousticPresenceFlags per source slot (0 = physical render).
	// The source operator only reads it; the physical path never sees a nonzero value.
	std::array<std::atomic<uint8>, MaxVoices> PresenceFlags{};
	// GT-published lateral azimuth (radians) of the source relative to its owner's body axis; 0 for sources without one.
	std::array<std::atomic<float>, MaxVoices> PresenceLateralAzimuth{};
	// Side-channel gain applied by the environment operator to its wet output only.
	// 1 leaves the convolution output untouched.
	std::atomic<float> PresenceWetSideGain{1.f};
	// GT-published exaggeration strength S (already clamped); the source operator maps it to the direct azimuth scale and limit.
	std::atomic<float> PresenceStrength{1.f};
	std::array<std::atomic<bool>, MaxVoices> SourceConsumers{};
	std::atomic<bool> EnvironmentConsumer{false};
	std::atomic<bool> BusReaderConsumer{false};
	// GT allocates both handles before release-publishing the voice AudioId.
	// Only the admitted source operator writes; one environment reader sums
	// complete FIFO blocks. Keep each input alive until context destruction:
	// UE's FPatchOutput::PopAudio refuses a stale output even with queued tail.
	// Stopping a source clears SourceSendActive without erasing its queued audio.
	std::array<Audio::FPatchOutputStrongPtr, MaxVoices> SourceSendPatches;
	std::array<TUniquePtr<Audio::FPatchInput>, MaxVoices> SourceSendInputs;
	std::array<std::atomic<bool>, MaxVoices> SourceSendActive{};
	std::array<std::atomic<uint64>, MaxVoices> SourceSendBlockCounts{};
	bool SourceSendTransportEnabled = false; // GT initializes before graph construction.
	int32 SourceSendCapacityFrames = 8192;
	std::atomic<uint64> SourceSendPushBlocks{0}, SourceSendPushFailures{0};
	std::atomic<bool> SourceSendTransportFailed{false};
	uint32 BusId = 0;
	int32 BusPrimeFrames = Frames;
	std::atomic<uint64> BusUnderruns{0}, BusReadBlocks{0}, BusPrimingBlocks{0}, BusSilentBlocks{0};
	std::atomic<int32> BusMinAvailable{MAX_int32}, BusMaxAvailable{0};
	std::atomic<bool> Stopped{false};
	std::atomic<float> WetGain{0.25f};
	std::atomic<uint64> SourceBlocks{0}, EnvironmentBlocks{0}, InvalidBlocks{0};
	std::atomic<uint64> DuplicateConsumers{0}, NoIRBlocks{0}, LastIRSequence{0};
	std::atomic<uint32> LastRejectDetail{0};
	std::atomic<uint64> ListenerSpatialVersion{0};
	std::atomic<uint64> SourceFrames{0}, EnvironmentFrames{0};
	std::atomic<uint64> SourceDelayPrimingBlocks{0};
	uint32 DeviceId = 0;
	// GT sets this only before registry publication; consumed capture storage
	// and its ID remain owned by this fresh context until its normal teardown.
	uint64 TestCapturePreparationId = 0;
	// Optional W3 maximum-event observation. New contexts begin empty; there is
	// no reset/reuse while a graph consumer can still publish to this context.
	std::atomic<bool> CaptureExecuteMaximums{false};
	std::array<FIMAcousticExecuteMaximumProbe, MaxVoices> SourceExecuteMaximums{};
	FIMAcousticExecuteMaximumProbe EnvironmentExecuteMaximum;
	std::atomic<uint64> BusExecuteMaxCycles{0}, BusExecuteCount{0};
	FIMAcousticExecuteMaximumProbe BusExecuteMaximum;
	// Optional bounded diagnostics allocated on GT before generator construction.
	TArray<float> CapturedSource, CapturedDry, CapturedWet, CapturedBus;
	// Graph-native source-block telemetry. This deliberately reuses the POD
	// probe shape used by the legacy adapter, but is written by the MetaSound
	// source operator itself so W4 cannot mistake an empty legacy callback ring
	// for silence on the new graph.
	TArray<FIMAcousticBlockProbe> CapturedSourceBlocks;
	TArray<FIMAcousticMetaSoundBlock> CapturedBlocks;
	// Explicit Q5 opt-in only; ordinary runtime and formal W3 allocate no rows.
	TArray<FIMAcousticBusReadBlock> CapturedBusReads;
	std::atomic<uint32> CapturedBusReadCount{0};
	std::atomic<bool> CapturedBusReadOverflow{false};
	// Independent opt-in trace; GT prepares storage and scans ready rows.
	TArray<FIMAcousticTransportOrderEvent> CapturedTransportOrder;
	std::unique_ptr<std::atomic<uint64>[]> CapturedTransportOrderReady;
	uint32 CapturedTransportOrderReadyWordCount = 0;
	std::atomic<uint32> CapturedTransportOrderClaims{0};
	std::atomic<bool> CapturedTransportOrderOverflow{false};
	FIMAcousticIdentityInjection IdentityInjection;
	std::array<std::atomic<float>, MaxVoices> DistanceCm{};
	// Sized with the source trace on GT; concurrent source operators only read it.
	uint32 CapturedSourceBlockReadyWordCount = 0;
	std::unique_ptr<std::atomic<uint64>[]> CapturedSourceBlockReady;
	std::atomic<uint32> CapturedSourceFrames{0}, CapturedSourceBlockCount{0}, CapturedSourceBlockClaimCount{0};
	std::atomic<uint32> CapturedEnvironmentFrames{0}, CapturedBlockCount{0}; // Bounded environment PCM frames and blocks.
	std::atomic<uint32> CapturedEnvironmentTraceBlockCount{0}; // Full metadata trace, independent of PCM capacity.
};
static_assert(FIMAcousticMetaSoundContext::TransportOrderEventCapacity == 8192, "Preserve the opt-in event capacity.");
static_assert(sizeof(FIMAcousticTransportOrderEvent) * FIMAcousticMetaSoundContext::TransportOrderEventCapacity
	+ sizeof(std::atomic<uint64>) * ((FIMAcousticMetaSoundContext::TransportOrderEventCapacity + 63u) / 64u) == 984064,
	"Verify actual payload plus ready-word storage during cold compilation.");
static_assert(sizeof(FIMAcousticTransportOrderEvent) * FIMAcousticMetaSoundContext::TransportOrderEventCapacity
	+ sizeof(std::atomic<uint64>) * ((FIMAcousticMetaSoundContext::TransportOrderEventCapacity + 63u) / 64u) <= 1048576,
	"Keep opt-in payload plus ready words within 1 MiB.");

uint32 IM_RecordAcousticTransportOrder(FIMAcousticMetaSoundContext& Context, FIMAcousticTransportOrderEvent Event);

using FIMAcousticMetaSoundContextPtr = TSharedPtr<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>;
namespace IMAcousticMetaSound
{
FIMAcousticMetaSoundContextPtr CreateAcousticMetaSoundContext(uint32 DeviceId, uint32 SampleRate, uint64 Epoch);
FIMAcousticMetaSoundContextPtr FindAcousticMetaSoundContext(uint32 DeviceId);
void StopAcousticMetaSoundContext(const FIMAcousticMetaSoundContextPtr& Context);
int32 RegisterAcousticMetaSoundSource(const FIMAcousticMetaSoundContextPtr& Context, uint64 AudioId);
bool IsAcousticMetaSound(const USoundBase* Sound);
void EnableAcousticMetaSoundCaptureForTest(
	bool Enabled,
	uint32 SourceTraceCapacityOverride = 0,
	uint32 EnvironmentTraceCapacityOverride = 0,
	bool bCapturePCM = true,
	bool bCaptureBusReads = false,
	bool bCaptureTransportOrder = false);
// GT test-only handoff. Zero ID prepares; nonzero ID only validates the
// same device/rate/enable session. Cancellation never touches a live context.
bool IM_PrepareAcousticMetaSoundCaptureForTest(
	uint32 DeviceId, uint32 SampleRate, uint64& InOutPreparationId, FString& OutError);
bool IM_CancelPreparedAcousticMetaSoundCaptureForTest(uint64 PreparationId);
}
