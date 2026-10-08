#pragma once

#include "CoreMinimal.h"
#include "IMAcousticAudioRenderer.h"
#include "IMAcousticTiming.h"
#include <atomic>

class FAudioDevice;

// W1 causal probe: preallocated, bounded, zero-behavior per-block telemetry.
// Links GT AudioDevice listener snapshot -> worker result sequence/age ->
// audio callback consumption/reject/fallback -> W1 Route0 recording window.
// Audio-callback writes use only an atomic slot claim plus POD stores plus one
// release store of the slot completion flag:
// no UE_LOG, no file I/O, no wait, no lock, no dynamic allocation.
// Correlation keys reuse existing fields only: CapturedSeconds (GT snapshot,
// passed through as worker Result.PublishedSeconds), WorldGeneration, voice
// Generation, AudioComponentId, Frame.Sequence. No new state machine,
// manager, or dependency.
enum class EIMAcousticProbeReject : uint8
{
	Accepted = 0,      // rendered through FIMAcousticAudioRenderer
	BadBlockShape = 1, // null buffer, unknown voice, or unsupported layout
	Bypassed = 2,      // whole-V2 bypass (Enabled==false), dry reference
	MissingResult = 3, // no usable result yet (never a stale age)
	StaleResult = 4,   // result age exceeded the frozen 250ms safety lease
	RendererFailed = 5 // FIMAcousticAudioRenderer::Render returned false
};

// One GT snapshot publication. Producer is GT Tick only; consumer is GT/test-end export only.
// Clocks: CapturedSeconds/SubmitSeconds are FPlatformTime::Seconds() wall clock, unit seconds.
// Lifecycle: preallocated in FIMAcousticDeviceBridge at device init, never grown/freed until device shutdown.
// Overflow: bounded write-once log; once full, newest entries are dropped and counted, earliest preserved.
struct FIMAcousticSnapshotProbe
{
	uint64 Block = 0; // log index; publication order key (write-once, never overwritten)
	uint64 WorldGeneration = 0;
	double CapturedSeconds = 0.0; // GT capture time (FPlatformTime::Seconds, s)
	double SubmitSeconds = 0.0;   // GT time just after Worker->Submit returns (FPlatformTime::Seconds, s; 0 if no submit attempted)
	float ListenerUEX = 0.0f, ListenerUEY = 0.0f, ListenerUEZ = 0.0f; // cm, actual AudioDevice listener
	float ListenerSDKX = 0.0f, ListenerSDKY = 0.0f, ListenerSDKZ = 0.0f; // m, SDK listener origin (Snapshot->Listener.origin)
	float Source0X = 0.0f, Source0Y = 0.0f, Source0Z = 0.0f; // m, SDK first-source origin (0 when NumSources==0)
	uint64 Source0AudioId = 0;
	uint32 NumSources = 0;
	uint8 Submitted = 0;  // Worker->Submit return (1 ok, 0 failed)
	uint8 FailCode = 0;   // 0 submitted-ok, 1 submit-queue/gen fail, 2 no worker at submit
	uint8 Pad[2] = {};
	uint32 NumDynamicMeshes = 0; // dynamic meshes attached to this snapshot
	uint32 NumDynamicSkipped = 0; // registered blockers omitted this tick
	float Door0TX = 0.0f, Door0TY = 0.0f, Door0TZ = 0.0f; // first captured dynamic mesh translation, SDK m (0 when none)
};

// One audio-callback block consumption. Producers are concurrent UE source
// jobs; each claims a slot with a single atomic fetch_add (wait-free).
struct FIMAcousticBlockProbe
{
	uint64 Block = 0; // audio-side monotonic claim index; PCM-window order key
	uint32 Voice = 0;
	uint64 CallbackAudioComponentId = 0; // identity seen by the audio callback
	uint64 ResultAudioComponentId = 0;   // identity carried by the worker result
	uint64 ResultWorldGeneration = 0;
	uint64 ResultVoiceGeneration = 0; // Frame.Generation
	uint64 ResultSequence = 0;        // Frame.Sequence (per-voice publication order)
	double SnapshotCaptured = 0.0;    // PublishedSeconds == GT CapturedSeconds
	double ConsumedSeconds = 0.0;     // audio wall clock; join key to W1 route WAV windows
	double AgeMs = 0.0;               // (Consumed - SnapshotCaptured) * 1000
	float ListenerX = 0.0f, ListenerY = 0.0f, ListenerZ = 0.0f; // SDK m, worker-used snapshot listener
	float DirX = 0.0f, DirY = 0.0f, DirZ = -1.0f;               // Frame.ListenerLocalDirection
	float Occlusion = 0.0f;           // Frame.Direct.occlusion
	float DistanceGain = 0.0f;        // Frame.Direct.distanceAttenuation
	uint32 Routes = 0;                // audition mask observed at consumption (direct=1,path=2,reverb=4)
	uint8 DirectValid = 0, PathValid = 0;
	EIMAcousticProbeReject Reject = EIMAcousticProbeReject::Accepted;
	uint8 RenderFailure = 0; // EIMAcousticRenderFailure code when Reject==RendererFailed
	uint8 Fallback = 0;      // dry fallback output taken
	uint8 ResetReason = 0;   // renderer Reset cause: 0 none,1 invalid-result,2 world-change,3 bypass,4 render-fail
	uint8 Pad = 0;
	// RejectDetail records every failed accept conjunct (0 when accepted/bypassed).
	// Bitmask: 1 no-renderer,2 bad-channels,4 bad-input-frames,8 bad-output-frames,
	// 16 no-world,32 world-mismatch,64 voice-gen-mismatch,128 audio-id-mismatch,
	// 256 no-valid-stems,512 published-in-future,1024 stale-age.
	uint32 RejectDetail = 0;
	uint32 DirectFlags = 0;  // IPLDirectEffectFlags numeric of the consumed frame
	double InputEnergy = -1.0;  // mono input sum-of-squares, unset (-1) when no input buffer
	double DirectEnergy = -1.0; // Metrics on accepted blocks, else unset
	double PathEnergy = -1.0;
	double OutputEnergy = -1.0; // stereo output sum-of-squares after render/fallback, else unset
	uint64 RenderedAt = 0, RejectedAt = 0; // device counters observed after this block
	// Astra decision A' (2026-09-13): degraded-output distance evidence.
	// CallbackDistanceCm = In.SpatializationParams->Distance for THIS block
	// (listener->emitter, UE cm; 0 when unavailable). DegradedGain = distance
	// factor applied to a degraded output: 1 = unity / not applied (accepted,
	// bypass, bad-shape, or a source within 1 m), 0 = distance unavailable
	// (original constant-power fallback kept), other = applied factor.
	float CallbackDistanceCm = 0.0f;
	float DegradedGain = 0.0f;
};

// Short-run callback timing probe. Enabled only by the bounded W3 diagnostic;
// source callbacks write it and OnAllSourcesProcessed finalizes the same-block
// valid-source count. No callback performs I/O, waits, or allocation.
struct FIMAcousticCallbackProbe
{
	uint64 CallbackIndex = 0;
	uint64 AudioBlock = 0;
	uint32 Voice = 0;
	uint32 ValidSourceCount = 0;
	uint32 ResultDrainUs = 0;
	uint32 ValidityUs = 0;
	uint32 DryPushUs = 0;
	uint32 RenderUs = 0;
	uint32 FallbackUs = 0;
	uint32 TotalUs = 0;
	uint32 RejectDetail = 0;
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
	uint8 Valid = 0;
	uint8 Outcome = 0; // 0 accepted, 1 stale/missing, 2 renderer failure, 3 bad block shape
	uint8 Pad[2] = {};
};

// Short-run source-block join. The audio extension publishes the exact
// PendingSourceCycles sum after all source callbacks for one audio block have
// completed; GT/test-end exports it with the per-source callback trace.
struct FIMAcousticSourceBlockProbe
{
	uint64 AudioBlock = 0;
	uint32 CallbackCount = 0;
	uint32 ValidSourceCount = 0;
	double SourceSumUs = 0.0;
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
};

// Short-run submix timing join. The probe is written by the audio submix
// callback and is enabled only by the bounded W3 diagnostic.
struct FIMAcousticReverbBlockProbe
{
	uint64 AudioBlock = 0;
	uint32 Outcome = 0; // 0 normal, 1 bypass, 2 invalid, 3 rendered, 4 stale/fail
	uint8 Fresh = 0;
	uint8 Rendered = 0;
	uint8 Pad[2] = {};
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
};

// Long-window rare-tail contract. Only a source/reverb window that reaches a
// configured budget trigger is written. The ring is exported after both audio
// flags are disabled and the final callbacks have drained; an overwrite is an
// explicit evidence failure, never silently treated as complete.
struct FIMAcousticRareTailProbe
{
	uint64 AudioBlock = 0; // reverb block label, or source block for source-only trigger
	uint64 SourceBlock = 0;
	uint32 TriggerMask = 0; // 1 source-near, 2 reverb-near, 4 combined-near, 8 combined-over, 16 reverb-tail, 32 self-test
	uint32 Outcome = 0;
	uint8 SourceJoinValid = 0;
	uint8 Fresh = 0;
	uint8 Rendered = 0;
	uint8 SelfTest = 0;
	double SourceSumUs = 0.0;
	double ReverbUs = 0.0;
	double CombinedUs = 0.0;
	double ReverbStartSeconds = 0.0;
	double ReverbEndSeconds = 0.0;
	uint64 MaxSnapshotGapUs = 0;
	uint64 MaxWorkerGapUs = 0;
};

// Source finalizer publishes one atomic join value per measured audio block.
// Reverb reads it by sequence; no source callback trace is retained in a long
// window unless the paired budget predicate fires.
struct FIMAcousticRareTailSourceJoin
{
	std::atomic<uint64> Sequence{0}; // stored as audio block + 1; zero means empty
	std::atomic<uint64> SourceCycles{0};
};

// One worker-loop iteration. Producer is the single simulation worker thread;
// consumer is GT/test-end export only. Times are FPlatformTime::Seconds() wall
// clock, unit seconds. First-result fields describe the first published voice
// result of the iteration (W1 runs a single source, so identity is complete for
// the W1 join keys); multi-voice iterations record the count plus first only.
// Lifecycle: preallocated in FIMAcousticDeviceBridge at device init, never
// grown/freed until device shutdown. Overflow drops newest, preserves earliest.
struct FIMAcousticWorkerProbe
{
	uint64 Block = 0; // log index; loop order key (write-once, never overwritten)
	uint64 WorldGeneration = 0;
	double LoopStartSeconds = 0.0; // loop-top time (FPlatformTime::Seconds, s)
	double SnapshotCaptured = 0.0; // Latest snapshot CapturedSeconds, s (0 when none)
	uint8 SnapValid = 0;   // SnapshotValid this iteration (1/0)
	uint8 SnapReason = 0;  // 0 valid-or-none?,1 no-snapshot,2 world-gen-mismatch,3 lease-expired,4 future-dated
	uint8 NumInputs = 0;
	uint8 ResultsPublished = 0;
	double EvalStartSeconds = 0.0, EvalEndSeconds = 0.0; // 0 when no EvaluateBatch ran
	uint8 EvalOk = 0;      // 1 batch ok, 0 fail, 2 no-inputs (no eval attempted)
	uint8 ReverbAttempt = 0, ReverbOk = 0, Pad0 = 0;
	double ReverbStartSeconds = 0.0, ReverbEndSeconds = 0.0; // 0 when not attempted
	uint64 ReverbSequence = 0;
	double ReverbCaptured = 0.0; // slot CapturedSeconds on success, else 0
	double PushSeconds = 0.0;    // time just after the result Push loop, s (0 when nothing pushed)
	uint8 PushOk = 0;            // 1 all pushes ok, 0 at least one push failed, 2 no result to push
	uint8 Pad1[7] = {};
	double WaitEndSeconds = 0.0; // time after SleepNoStats (wait-return moment), s
	uint32 FirstVoice = 0;
	uint64 FirstAudioId = 0, FirstVoiceGen = 0, FirstSeq = 0; // publication identity
	uint32 DirectFlags = 0;  // IPLDirectEffectFlags numeric of the first published frame
	float Occlusion = 0.0f, DistanceGain = 0.0f, Directivity = 1.0f;
	float AirAbsorption[3] = {};
	float PathEQ[3] = {};
	float PathSH[4] = {};    // FIMAcousticAudioFrame::Coefficients (Order 1)
	// inc109 worker-side door truth (append-only, diagnostic): first-frame
	// path verdict, applied validation flag, dynamic-mesh count this loop.
	uint8 PathValid0 = 0;         // 1 path valid, 0 invalid, 2 no frame published
	uint8 NumDynamicSynced = 0;   // DynamicInputs passed to SyncDynamicMeshes
	int8 AppliedValidationSnap = -1; // worker AppliedValidation this loop
	uint8 Pad2[5] = {};
};

// Fixed SPSC ring: one producer and one consumer per voice. Neither side can
// overwrite a slot the other owns. Queue-full drops a publication, never blocks.
template<typename T, uint32 Capacity = 8>
class FIMAcousticSpscRing final
{
public:
	bool Push(const T& Value)
	{
		const uint32 W = Write.load(std::memory_order_relaxed);
		const uint32 Next = (W + 1) % Capacity;
		if (Next == Read.load(std::memory_order_acquire)) { return false; }
		Slots[W] = Value;
		Write.store(Next, std::memory_order_release);
		return true;
	}
	bool Pop(T& Value)
	{
		const uint32 R = Read.load(std::memory_order_relaxed);
		if (R == Write.load(std::memory_order_acquire)) { return false; }
		Value = Slots[R];
		Read.store((R + 1) % Capacity, std::memory_order_release);
		return true;
	}
private:
	std::array<T, Capacity> Slots{};
	alignas(64) std::atomic<uint32> Read{0};
	alignas(64) std::atomic<uint32> Write{0};
};

struct FIMAcousticVoiceRequest
{
	uint32 Voice = 0;
	uint64 Generation = 0;
	uint64 AudioComponentId = 0;
	bool Active = false;
};

struct FIMAcousticVoiceResult
{
	FIMAcousticAudioFrame Frame;
	uint64 AudioComponentId = 0;
	uint64 WorldGeneration = 0;
	double PublishedSeconds = 0.0;
};

struct FIMAcousticVoiceBridge
{
	FIMAcousticSpscRing<FIMAcousticVoiceRequest> Requests;
	FIMAcousticSpscRing<FIMAcousticVoiceResult> Results;
	// One outstanding dry block per voice. Source jobs finish before UE submix
	// processing, but this ownership ring also makes skipped submix blocks safe.
	// Buffers allocate at device init, never in either callback.
	struct FIMDrySlot { TArray<float> Samples; uint64 Epoch=0,Generation=0; double Captured=0; };
	std::array<FIMDrySlot,2> Dry;
	std::atomic<uint32> DryWrite{0},DryRead{0};
	std::atomic<uint64> LiveGeneration{0};
	void InitDry(uint32 Frames){for(auto& Slot:Dry)Slot.Samples.SetNumZeroed(Frames);}
	bool PushDry(const float* Samples,uint32 Frames,uint64 Epoch,uint64 Generation,double Captured,float ReverbSendGain)
	{
		const uint32 W=DryWrite.load(std::memory_order_relaxed),Next=(W+1)%2;
		if(Next==DryRead.load(std::memory_order_acquire)||Dry[W].Samples.Num()!=int32(Frames))return false;
		auto& Slot=Dry[W];
		// Apply the independent room-send distance curve once before summing
		// environmental sends. This excludes direct occlusion and route mute.
		for(uint32 I=0;I<Frames;++I)Slot.Samples[I]=Samples[I]*ReverbSendGain;
		Slot.Epoch=Epoch;Slot.Generation=Generation;Slot.Captured=Captured;
		DryWrite.store(Next,std::memory_order_release);return true;
	}
	bool MixDry(float* Sum,uint32 Frames,uint64 Epoch,double Now)
	{
		const uint32 R=DryRead.load(std::memory_order_relaxed);
		if(R==DryWrite.load(std::memory_order_acquire))return false;
		const auto& Slot=Dry[R];
		const bool Valid=Slot.Epoch==Epoch&&Slot.Generation==LiveGeneration.load(std::memory_order_acquire)
			&&Slot.Samples.Num()==int32(Frames)&&Now>=Slot.Captured&&Now-Slot.Captured<=.1;
		if(Valid)
			for(uint32 I=0;I<Frames;++I)Sum[I]+=Slot.Samples[I];
		DryRead.store((R+1)%2,std::memory_order_release);
		return Valid;
	}
};

// Created once at device initialization. Worker leases keep queues alive after
// device shutdown; Alive closes publication before audio resources are released.
struct FIMAcousticDeviceBridge
{
	// W3 pressure attribution is bounded, preallocated telemetry. It is
	// reset at the start of each measured window and exported by the test on
	// the game thread; audio callbacks only perform atomic increments/CAS.
	static constexpr uint32 PressureRejectDetailCapacity = 2048;
	static constexpr uint32 PressureReverbRejectReasonCapacity = 4;
	static constexpr uint32 CallbackBlockProbeCapacity = 2048; // >30 s at 48 kHz / 1024 frames
	static constexpr uint32 RareTailProbeCapacity = 512;
	std::array<std::atomic<uint64>, PressureRejectDetailCapacity> PressureRejectDetails{};
	std::array<std::atomic<uint64>, PressureReverbRejectReasonCapacity> PressureReverbRejectReasons{};
	std::atomic<uint64> FirstPressureRejectTimeUs{0};
	std::atomic<uint32> FirstPressureRejectDetail{0}, FirstPressureRejectVoice{0};
	std::atomic<uint64> FirstPressureReverbRejectTimeUs{0};
	std::atomic<uint32> FirstPressureReverbRejectReason{0};

	void ResetPressureDiagnostics()
	{
		for (auto& Count : PressureRejectDetails) Count.store(0, std::memory_order_relaxed);
		for (auto& Count : PressureReverbRejectReasons) Count.store(0, std::memory_order_relaxed);
		FirstPressureRejectTimeUs.store(0, std::memory_order_relaxed);
		FirstPressureRejectDetail.store(0, std::memory_order_relaxed);
		FirstPressureRejectVoice.store(0, std::memory_order_relaxed);
		FirstPressureReverbRejectTimeUs.store(0, std::memory_order_relaxed);
		FirstPressureReverbRejectReason.store(0, std::memory_order_relaxed);
	}

	void RecordPressureReject(uint32 Detail, uint32 Voice, double Now)
	{
		const uint32 Index = Detail < PressureRejectDetailCapacity ? Detail : PressureRejectDetailCapacity - 1;
		PressureRejectDetails[Index].fetch_add(1, std::memory_order_relaxed);
		const uint64 TimeUs = Now > 0.0 ? static_cast<uint64>(Now * 1.e6) : 1;
		uint64 Expected = 0;
		if (FirstPressureRejectTimeUs.compare_exchange_strong(Expected, TimeUs, std::memory_order_relaxed))
		{
			FirstPressureRejectDetail.store(Detail, std::memory_order_relaxed);
			FirstPressureRejectVoice.store(Voice, std::memory_order_relaxed);
		}
	}

	void RecordPressureReverbReject(uint32 Reason, double Now)
	{
		const uint32 Index = Reason < PressureReverbRejectReasonCapacity ? Reason : PressureReverbRejectReasonCapacity - 1;
		PressureReverbRejectReasons[Index].fetch_add(1, std::memory_order_relaxed);
		const uint64 TimeUs = Now > 0.0 ? static_cast<uint64>(Now * 1.e6) : 1;
		uint64 Expected = 0;
		if (FirstPressureReverbRejectTimeUs.compare_exchange_strong(Expected, TimeUs, std::memory_order_relaxed))
		{
			FirstPressureReverbRejectReason.store(Reason, std::memory_order_relaxed);
		}
	}

	TArray<TUniquePtr<FIMAcousticVoiceBridge>> Voices;
	uint32 SampleRate = 0;
	uint32 BlockFrames = 0;
	std::atomic<bool> Alive{false};
	// GT acquires one world lease per device. Worker shutdown joins before release;
	// this is also the audio consumer's authority when discarding old-world packets.
	std::atomic<uint64> WorldGeneration{0};
	std::atomic<uint64> RejectedBlocks{0};
	std::atomic<uint64> InvalidResultBlocks{0},RendererFailedBlocks{0};
	std::array<std::atomic<uint64>,size_t(EIMAcousticRenderFailure::Count)> RenderFailures{};
	std::atomic<uint64> RenderedBlocks{0};
	std::atomic<uint64> DirectNonzeroBlocks{0};
	std::atomic<uint64> PathNonzeroBlocks{0};
	std::atomic<uint64> ReverbWorldGeneration{0};
	// Audition mask: direct=1, path=2, reverb=4. Intentional route mute is
	// distinct from stale/missing data, which always takes the degraded branch.
	std::atomic<uint32> RenderRoutes{7};
	// Whole V2 bypass is a dry, constant-power mono reference. Route mute above
	// preserves convolution history; bypass explicitly resets indirect history.
	std::atomic<bool> Enabled{true},ProfilingEnabled{false};
	std::atomic<uint64> BypassedBlocks{0};
	// W1 causal-probe stores. Preallocated with the bridge at device init;
	// never grown, never freed, never touched by file I/O on audio threads.
	// Bounded write-once logs: the earliest entries are never overwritten; once
	// full, newer entries are dropped and counted as overflow (earliest preserved).
	// Each slot has an explicit completion flag: the producer writes the POD
	// first, then publishes Done with release semantics. GT/test-end export reads
	// only Done-complete slots (acquire); any claimed-but-incomplete slot is an
	// evidence gap (INCONCLUSIVE), never silently treated as data.
	// Clocks: all Seconds fields are FPlatformTime::Seconds() wall clock, seconds.
	// Owner: GT Tick owns snapshots, the worker thread owns worker entries,
	// concurrent audio source jobs own block entries; GT/test-end owns export.
	static constexpr uint32 ProbeSnapshotCapacity = 4096; // 20Hz GT: ~204s, covers the 120s W1 cap
	static constexpr uint32 ProbeBlockCapacity = 8192;    // ~47 blocks/s/voice: ~174s single voice
	static constexpr uint32 ProbeWorkerCapacity = 4096;   // 20Hz worker loops: ~204s
	static constexpr uint32 CallbackProbeCapacity = 65536; // >=30s of 16-voice blocks at 48 kHz/1024 frames
	std::array<FIMAcousticSnapshotProbe, ProbeSnapshotCapacity> SnapshotProbes{};
	std::array<std::atomic<uint64>, ProbeSnapshotCapacity> SnapshotDone{};
	std::array<FIMAcousticBlockProbe, ProbeBlockCapacity> BlockProbes{};
	std::array<std::atomic<uint64>, ProbeBlockCapacity> BlockDone{};
	std::array<FIMAcousticWorkerProbe, ProbeWorkerCapacity> WorkerProbes{};
	std::array<std::atomic<uint64>, ProbeWorkerCapacity> WorkerDone{};
	std::array<FIMAcousticCallbackProbe, CallbackProbeCapacity> CallbackProbes{};
	std::array<std::atomic<uint64>, CallbackProbeCapacity> CallbackDone{};
	std::array<FIMAcousticSourceBlockProbe, CallbackBlockProbeCapacity> SourceBlockProbes{};
	std::array<std::atomic<uint64>, CallbackBlockProbeCapacity> SourceBlockDone{};
	std::array<FIMAcousticReverbBlockProbe, CallbackBlockProbeCapacity> ReverbBlockProbes{};
	std::array<std::atomic<uint64>, CallbackBlockProbeCapacity> ReverbBlockDone{};
	std::array<FIMAcousticRareTailProbe, RareTailProbeCapacity> RareTailProbes{};
	std::array<std::atomic<uint64>, RareTailProbeCapacity> RareTailDone{};
	std::array<FIMAcousticRareTailSourceJoin, CallbackBlockProbeCapacity> RareTailSourceJoins{};
	std::atomic<uint64> SnapshotProbePushes{0}, SnapshotProbeOverflows{0};
	std::atomic<uint64> BlockProbePushes{0}, BlockProbeOverflows{0};
	std::atomic<uint64> WorkerProbePushes{0}, WorkerProbeOverflows{0};
	std::atomic<uint64> CallbackProbePushes{0}, CallbackProbeOverflows{0};
	std::atomic<uint64> SourceBlockProbePushes{0}, SourceBlockProbeOverflows{0};
	std::atomic<uint64> ReverbBlockProbePushes{0}, ReverbBlockProbeOverflows{0};
	std::atomic<uint64> RareTailProbePushes{0}, RareTailProbeOverflows{0};
	std::atomic<uint64> CallbackAudioBlock{0};
	std::atomic<uint32> CallbackValidSources{0};
	std::atomic<bool> CallbackDiagnosticsEnabled{false};
	std::atomic<bool> RareTailDiagnosticsEnabled{false}, RareTailSelfTestEnabled{false};
	std::atomic<uint32> RareTailDeviceBudgetUs{0}, RareTailNearBudgetUs{0}, RareTailNearTailUs{0};
	uint64 CallbackBlockStartPush = 0;
	void InitProbes() // GT device init only, before Alive publishes the bridge
	{
		// Counters restart with the log: slots are indexed by push count, so a
		// re-init without counter reset would index past the array end.
		SnapshotProbePushes.store(0, std::memory_order_relaxed);
		SnapshotProbeOverflows.store(0, std::memory_order_relaxed);
		BlockProbePushes.store(0, std::memory_order_relaxed);
		BlockProbeOverflows.store(0, std::memory_order_relaxed);
		WorkerProbePushes.store(0, std::memory_order_relaxed);
		WorkerProbeOverflows.store(0, std::memory_order_relaxed);
		CallbackProbePushes.store(0, std::memory_order_relaxed);
		CallbackProbeOverflows.store(0, std::memory_order_relaxed);
		SourceBlockProbePushes.store(0, std::memory_order_relaxed);
		SourceBlockProbeOverflows.store(0, std::memory_order_relaxed);
		ReverbBlockProbePushes.store(0, std::memory_order_relaxed);
		ReverbBlockProbeOverflows.store(0, std::memory_order_relaxed);
		RareTailProbePushes.store(0, std::memory_order_relaxed);
		RareTailProbeOverflows.store(0, std::memory_order_relaxed);
		CallbackAudioBlock.store(0, std::memory_order_relaxed);
		CallbackValidSources.store(0, std::memory_order_relaxed);
		CallbackDiagnosticsEnabled.store(false, std::memory_order_relaxed);
		RareTailDiagnosticsEnabled.store(false, std::memory_order_relaxed);
		RareTailSelfTestEnabled.store(false, std::memory_order_relaxed);
		RareTailDeviceBudgetUs.store(0, std::memory_order_relaxed);
		RareTailNearBudgetUs.store(0, std::memory_order_relaxed);
		RareTailNearTailUs.store(0, std::memory_order_relaxed);
		CallbackBlockStartPush = 0;
		for (auto& D : SnapshotDone) D.store(0, std::memory_order_relaxed);
		for (auto& D : BlockDone) D.store(0, std::memory_order_relaxed);
		for (auto& D : WorkerDone) D.store(0, std::memory_order_relaxed);
		for (auto& D : CallbackDone) D.store(0, std::memory_order_relaxed);
		for (auto& D : SourceBlockDone) D.store(0, std::memory_order_relaxed);
		for (auto& D : ReverbBlockDone) D.store(0, std::memory_order_relaxed);
		for (auto& D : RareTailDone) D.store(0, std::memory_order_relaxed);
		for (auto& Join : RareTailSourceJoins)
		{
			Join.Sequence.store(0, std::memory_order_relaxed);
			Join.SourceCycles.store(0, std::memory_order_relaxed);
		}
	}
	void ConfigureRareTailDiagnostics(bool InEnabled, bool InSelfTest, uint32 DeviceBudgetUs, uint32 NearBudgetUs, uint32 NearTailUs)
	{
		RareTailDeviceBudgetUs.store(DeviceBudgetUs, std::memory_order_relaxed);
		RareTailNearBudgetUs.store(NearBudgetUs, std::memory_order_relaxed);
		RareTailNearTailUs.store(NearTailUs, std::memory_order_relaxed);
		RareTailSelfTestEnabled.store(InSelfTest, std::memory_order_relaxed);
		RareTailDiagnosticsEnabled.store(InEnabled, std::memory_order_release);
	}
	void TraceSnapshot(FIMAcousticSnapshotProbe& Entry) // GT Tick only
	{
		const uint64 N = SnapshotProbePushes.fetch_add(1, std::memory_order_relaxed);
		if (N >= ProbeSnapshotCapacity) { SnapshotProbeOverflows.fetch_add(1, std::memory_order_relaxed); return; }
		Entry.Block = N;
		SnapshotProbes[N] = Entry;
		SnapshotDone[N].store(N + 1, std::memory_order_release);
	}
	void TraceBlock(FIMAcousticBlockProbe& Entry) // audio callbacks, concurrent voices
	{
		// Wait-free: one atomic claim plus POD stores plus one release store.
		// No UE_LOG, no file I/O, no wait, no lock, no dynamic allocation.
		const uint64 N = BlockProbePushes.fetch_add(1, std::memory_order_relaxed);
		if (N >= ProbeBlockCapacity) { BlockProbeOverflows.fetch_add(1, std::memory_order_relaxed); return; }
		Entry.Block = N;
		BlockProbes[N] = Entry;
		BlockDone[N].store(N + 1, std::memory_order_release);
	}
	void TraceWorker(FIMAcousticWorkerProbe& Entry) // worker thread only
	{
		const uint64 N = WorkerProbePushes.fetch_add(1, std::memory_order_relaxed);
		if (N >= ProbeWorkerCapacity) { WorkerProbeOverflows.fetch_add(1, std::memory_order_relaxed); return; }
		Entry.Block = N;
		WorkerProbes[N] = Entry;
		WorkerDone[N].store(N + 1, std::memory_order_release);
	}
	void TraceCallback(FIMAcousticCallbackProbe& Entry) // source callbacks, concurrent voices
	{
		const uint64 N = CallbackProbePushes.fetch_add(1, std::memory_order_relaxed);
		if (N >= CallbackProbeCapacity) { CallbackProbeOverflows.fetch_add(1, std::memory_order_relaxed); return; }
		Entry.CallbackIndex = N;
		CallbackProbes[N] = Entry;
		CallbackDone[N].store(N + 1, std::memory_order_release);
	}
	void TraceSourceBlock(const FIMAcousticSourceBlockProbe& Entry) // audio extension join
	{
		const uint64 N = SourceBlockProbePushes.fetch_add(1, std::memory_order_relaxed);
		if (N >= CallbackBlockProbeCapacity) { SourceBlockProbeOverflows.fetch_add(1, std::memory_order_relaxed); return; }
		SourceBlockProbes[N] = Entry;
		SourceBlockDone[N].store(N + 1, std::memory_order_release);
	}
	void TraceReverbBlock(const FIMAcousticReverbBlockProbe& Entry) // submix callback
	{
		const uint64 N = ReverbBlockProbePushes.fetch_add(1, std::memory_order_relaxed);
		if (N >= CallbackBlockProbeCapacity) { ReverbBlockProbeOverflows.fetch_add(1, std::memory_order_relaxed); return; }
		ReverbBlockProbes[N] = Entry;
		ReverbBlockDone[N].store(N + 1, std::memory_order_release);
	}
	void TraceRareTail(const FIMAcousticRareTailProbe& Entry) // triggered audio evidence ring
	{
		const uint64 N = RareTailProbePushes.fetch_add(1, std::memory_order_relaxed);
		if (N >= RareTailProbeCapacity) RareTailProbeOverflows.fetch_add(1, std::memory_order_relaxed);
		const uint32 Slot = static_cast<uint32>(N % RareTailProbeCapacity);
		RareTailProbes[Slot] = Entry;
		RareTailDone[Slot].store(N + 1, std::memory_order_release);
	}
	void StoreRareTailSource(uint64 Sequence, uint64 SourceCycles)
	{
		auto& Join = RareTailSourceJoins[Sequence % CallbackBlockProbeCapacity];
		Join.SourceCycles.store(SourceCycles, std::memory_order_relaxed);
		Join.Sequence.store(Sequence + 1, std::memory_order_release);
	}
	bool LoadRareTailSource(uint64 Sequence, uint64& SourceCycles) const
	{
		const auto& Join = RareTailSourceJoins[Sequence % CallbackBlockProbeCapacity];
		const uint64 Expected = Sequence + 1;
		if (Join.Sequence.load(std::memory_order_acquire) != Expected) return false;
		SourceCycles = Join.SourceCycles.load(std::memory_order_relaxed);
		return Join.Sequence.load(std::memory_order_acquire) == Expected;
	}
	uint32 RareTailTriggerMask(double SourceUs, double ReverbUs, double CombinedUs) const
	{
		const double NearBudget = double(RareTailNearBudgetUs.load(std::memory_order_relaxed));
		const double DeviceBudget = double(RareTailDeviceBudgetUs.load(std::memory_order_relaxed));
		const double NearTail = double(RareTailNearTailUs.load(std::memory_order_relaxed));
		uint32 Mask = 0;
		if (NearBudget > 0.0 && SourceUs >= NearBudget) Mask |= 1;
		if (NearBudget > 0.0 && ReverbUs >= NearBudget) Mask |= 2;
		if (NearBudget > 0.0 && CombinedUs >= NearBudget) Mask |= 4;
		if (DeviceBudget > 0.0 && CombinedUs >= DeviceBudget) Mask |= 8;
		if (NearTail > 0.0 && ReverbUs >= NearTail) Mask |= 16;
		if (Mask && RareTailSelfTestEnabled.load(std::memory_order_relaxed)) Mask |= 32;
		return Mask;
	}
	void RecordRareTailSource(uint64 Sequence, uint64 SourceCycles)
	{
		if (!RareTailDiagnosticsEnabled.load(std::memory_order_relaxed)) return;
		StoreRareTailSource(Sequence, SourceCycles);
		const double SourceUs = FPlatformTime::ToSeconds64(SourceCycles) * 1.e6;
		const uint32 TriggerMask = RareTailTriggerMask(SourceUs, 0.0, SourceUs);
		if (!TriggerMask) return;
		FIMAcousticRareTailProbe Probe{};
		Probe.AudioBlock = Sequence;
		Probe.SourceBlock = Sequence;
		Probe.TriggerMask = TriggerMask;
		Probe.SourceJoinValid = 1;
		Probe.SelfTest = RareTailSelfTestEnabled.load(std::memory_order_relaxed) ? 1 : 0;
		Probe.SourceSumUs = SourceUs;
		Probe.CombinedUs = SourceUs;
		Probe.MaxSnapshotGapUs = MaxSnapshotGapUs.load(std::memory_order_relaxed);
		Probe.MaxWorkerGapUs = MaxWorkerGapUs.load(std::memory_order_relaxed);
		TraceRareTail(Probe);
	}
	void RecordRareTailReverb(uint64 ReverbBlock, uint32 Outcome, uint8 Fresh, uint8 Rendered, double StartSeconds, double EndSeconds)
	{
		if (!RareTailDiagnosticsEnabled.load(std::memory_order_relaxed)) return;
		const uint64 CandidateSourceBlock = ReverbBlock > 0 ? ReverbBlock - 1 : ReverbBlock;
		uint64 SourceCycles = 0;
		uint64 SourceBlock = CandidateSourceBlock;
		bool SourceValid = LoadRareTailSource(SourceBlock, SourceCycles);
		if (!SourceValid && ReverbBlock != CandidateSourceBlock)
		{
			SourceBlock = ReverbBlock;
			SourceValid = LoadRareTailSource(SourceBlock, SourceCycles);
		}
		const double SourceUs = SourceValid ? FPlatformTime::ToSeconds64(SourceCycles) * 1.e6 : 0.0;
		const double ReverbUs = FMath::Max(0.0, (EndSeconds - StartSeconds) * 1.e6);
		const double CombinedUs = SourceUs + ReverbUs;
		const uint32 TriggerMask = RareTailTriggerMask(SourceUs, ReverbUs, CombinedUs);
		if (!TriggerMask) return;
		FIMAcousticRareTailProbe Probe{};
		Probe.AudioBlock = ReverbBlock;
		Probe.SourceBlock = SourceBlock;
		Probe.TriggerMask = TriggerMask;
		Probe.Outcome = Outcome;
		Probe.SourceJoinValid = SourceValid ? 1 : 0;
		Probe.Fresh = Fresh;
		Probe.Rendered = Rendered;
		Probe.SelfTest = RareTailSelfTestEnabled.load(std::memory_order_relaxed) ? 1 : 0;
		Probe.SourceSumUs = SourceUs;
		Probe.ReverbUs = ReverbUs;
		Probe.CombinedUs = CombinedUs;
		Probe.ReverbStartSeconds = StartSeconds;
		Probe.ReverbEndSeconds = EndSeconds;
		Probe.MaxSnapshotGapUs = MaxSnapshotGapUs.load(std::memory_order_relaxed);
		Probe.MaxWorkerGapUs = MaxWorkerGapUs.load(std::memory_order_relaxed);
		TraceRareTail(Probe);
	}
	void FinalizeCallbackBlock(uint64 SourceCycles) // audio extension join after all source callbacks
	{
		const bool CallbackTrace = CallbackDiagnosticsEnabled.load(std::memory_order_relaxed);
		const bool RareTail = RareTailDiagnosticsEnabled.load(std::memory_order_relaxed);
		if (!CallbackTrace && !RareTail) return;
		const uint64 Sequence = CallbackAudioBlock.load(std::memory_order_relaxed);
		if (RareTail) RecordRareTailSource(Sequence, SourceCycles);
		if (CallbackTrace)
		{
			const uint64 End = CallbackProbePushes.load(std::memory_order_acquire);
			const uint32 Valid = CallbackValidSources.exchange(0, std::memory_order_acq_rel);
			double StartSeconds = 0.0, EndSeconds = 0.0;
			if (End >= CallbackBlockStartPush && End - CallbackBlockStartPush <= CallbackProbeCapacity)
			{
				for (uint64 N = CallbackBlockStartPush; N < End; ++N)
				{
					const uint32 Slot = static_cast<uint32>(N % CallbackProbeCapacity);
					if (CallbackDone[Slot].load(std::memory_order_acquire) == N + 1
						&& CallbackProbes[Slot].AudioBlock == Sequence)
					{
						CallbackProbes[Slot].ValidSourceCount = Valid;
						const double Start = CallbackProbes[Slot].StartSeconds;
						const double Finish = CallbackProbes[Slot].EndSeconds;
						if (Start > 0.0 && (StartSeconds == 0.0 || Start < StartSeconds)) StartSeconds = Start;
						if (Finish > EndSeconds) EndSeconds = Finish;
					}
				}
			}
			FIMAcousticSourceBlockProbe SourceBlock{};
			SourceBlock.AudioBlock = Sequence;
			SourceBlock.CallbackCount = End >= CallbackBlockStartPush ? uint32(End - CallbackBlockStartPush) : 0;
			SourceBlock.ValidSourceCount = Valid;
			SourceBlock.SourceSumUs = FPlatformTime::ToSeconds64(SourceCycles) * 1.e6;
			SourceBlock.StartSeconds = StartSeconds;
			SourceBlock.EndSeconds = EndSeconds;
			TraceSourceBlock(SourceBlock);
			CallbackBlockStartPush = End;
		}
		CallbackAudioBlock.fetch_add(1, std::memory_order_relaxed);
	}
	bool ExportCallbackProbes(FString& OutCSV) const
	{
		OutCSV = TEXT("callback_index,audio_block,voice,valid_source_count,valid,outcome,reject_detail,start_s,end_s,result_drain_us,validity_us,dry_push_us,render_us,fallback_us,total_us\n");
		const uint64 End = CallbackProbePushes.load(std::memory_order_acquire);
		const uint64 Begin = End > CallbackProbeCapacity ? End - CallbackProbeCapacity : 0;
		bool Complete = CallbackProbeOverflows.load(std::memory_order_acquire) == 0;
		for (uint64 N = Begin; N < End; ++N)
		{
			const uint32 Slot = static_cast<uint32>(N % CallbackProbeCapacity);
			if (CallbackDone[Slot].load(std::memory_order_acquire) != N + 1)
			{
				Complete = false;
				continue;
			}
			const FIMAcousticCallbackProbe& P = CallbackProbes[Slot];
			OutCSV += FString::Printf(TEXT("%llu,%llu,%u,%u,%u,%u,%u,%.9f,%.9f,%u,%u,%u,%u,%u,%u\n"),
				P.CallbackIndex, P.AudioBlock, P.Voice, P.ValidSourceCount, P.Valid, P.Outcome, P.RejectDetail,
				P.StartSeconds, P.EndSeconds, P.ResultDrainUs, P.ValidityUs, P.DryPushUs, P.RenderUs, P.FallbackUs, P.TotalUs);
		}
		return Complete && CallbackProbeOverflows.load(std::memory_order_acquire) == 0;
	}
	FIMAcousticTiming SourceTiming,SourceBlockTiming,ReverbTiming,WorkerTiming,GameThreadTiming;
	// Sub-phase diagnostic histograms (zero behavior; RecordCycles only, no
	// verdict/branch input). Worker loop: snapshot drain+match, voice solve,
	// reverb eval, result publish. GT tick: world traversal vs worker submit.
	FIMAcousticTiming WorkerSnapshotTiming,WorkerSolveTiming,WorkerReverbTiming,WorkerPublishTiming;
	FIMAcousticTiming GTTraverseTiming,GTSubmitTiming;
	FIMAcousticTiming ApertureTransitTiming;
	std::atomic<uint64> ApertureTransitChecks{0},ApertureTransitBlocked{0};
	std::atomic<uint64> PendingSourceCycles{0};
	std::atomic<uint64> MaxSnapshotGapUs{0},MaxWorkerGapUs{0},StaleResultBlocks{0},MissingResultBlocks{0};
	std::atomic<uint64> ReverbNonzeroBlocks{0},ReverbRejectedBlocks{0},DryDroppedBlocks{0};
	std::atomic<uint64> ReverbProcessedBlocks{0},ReverbDryBlocks{0},ReverbRawNonzeroBlocks{0};
	std::atomic<uint64> PushDryCalls{0},PushDryInvalidDirect{0},PushDryZeroGain{0},PushDryInputNonzero{0};
	std::atomic<uint64> ReverbNotFreshBlocks{0};
	std::atomic<uint64> RenderInputNonzero{0};
	std::atomic<uint64> ReverbDryNonzeroBlocks{0},ReverbAmbisonicsNonzeroBlocks{0};
	std::atomic<int32> ReverbEffectInstances{0};
};

// GT-only registry lookup. Never call this from an audio callback.
namespace IMAcousticSpatialization
{
TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> FindAcousticDevice(FAudioDevice* Device);
bool RegisterAcousticSpatialization();
void UnregisterAcousticSpatialization();
}
