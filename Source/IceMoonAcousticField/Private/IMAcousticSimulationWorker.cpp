#include "IMAcousticSimulationWorker.h"
#include "HAL/Event.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IMAcousticStallDiagnostics.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Tasks/Task.h"

namespace IMAcousticSimulationWorkerPrivate
{
struct FIMAcousticReverbWorkState
{
	FIMAcousticSimulation Simulation;
	FIMAcousticWorkerProbe Probe{};
	IPLCoordinateSpace3 Listener{};
	std::string Error;
	uint64 Sequence = 0;
	bool Cancelled = false;
};
}

FIMAcousticSimulationWorker::FIMAcousticSimulationWorker() = default;
FIMAcousticSimulationWorker::~FIMAcousticSimulationWorker()
{
	StopAndJoin();
	if (SnapshotEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(SnapshotEvent);
		SnapshotEvent = nullptr;
	}
}

bool FIMAcousticSimulationWorker::Start(TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> InBridge,
	uint64 InWorldGeneration, FIMAcousticBakeData&& InBake,
	TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> InReverb)
{
	check(IsInGameThread());
	if (Thread || !InBridge || !InBridge->Alive.load(std::memory_order_acquire)
		|| InWorldGeneration == 0 || InBake.Scene.empty() || InBake.ProbeBatch.empty()) { return false; }
	uint64 Unowned = 0;
	if (!InBridge->WorldGeneration.compare_exchange_strong(Unowned, InWorldGeneration,
		std::memory_order_acq_rel)) { return false; }
	Bridge = MoveTemp(InBridge);
	WorldGeneration = InWorldGeneration;
	Bake = MoveTemp(InBake);
	Reverb=MoveTemp(InReverb);
	if (!SnapshotEvent) { SnapshotEvent = FPlatformProcess::GetSynchEventFromPool(false); }
	if (!SnapshotEvent) { StopAndJoin(); return false; }
	StopRequested.store(false, std::memory_order_release);
	WorkerState.store(EIMWorkerState::Loading, std::memory_order_release);
	// The worker feeds the audio callback deadline: a snapshot must become a result within the 2-block (about 21 ms) source input delay.
	// Measured on Pressure16x600 (copy im-acoustic-v2-bake, 2026-10-27): snapshot wake lag is 0.02 ms median but up to 12-15 ms when other
	// processes load the CPU, which is what consumed the margin. AboveNormal makes the scheduler wake it ahead of ordinary threads.
	Thread.Reset(FRunnableThread::Create(this, TEXT("IMAcousticSimulation"), 0, TPri_AboveNormal));
	if (!Thread) { StopAndJoin(); return false; }
	return true;
}

bool FIMAcousticSimulationWorker::Submit(TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe> Snapshot)
{
	check(IsInGameThread());
	const bool Pushed = Thread && Snapshot && Snapshot->WorldGeneration == WorldGeneration && Snapshots.Push(Snapshot);
	if (Pushed && SnapshotEvent) { SnapshotEvent->Trigger(); }
	return Pushed;
}

void FIMAcousticSimulationWorker::Stop()
{
	StopRequested.store(true, std::memory_order_release);
	if (SnapshotEvent) { SnapshotEvent->Trigger(); }
}

void FIMAcousticSimulationWorker::StopAndJoin()
{
	check(IsInGameThread());
	Stop();
	if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
	// Only GT joins. SDK state is destroyed by its sole worker before the device
	// lease can transfer to another world, preserving one producer per result ring.
	if (Thread) { Thread->WaitForCompletion(); Thread.Reset(); }
	if (Bridge)
	{
		uint64 Owned = WorldGeneration;
		Bridge->WorldGeneration.compare_exchange_strong(Owned, 0, std::memory_order_acq_rel);
		Bridge.Reset();
	}
	TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe> Discard;
	while (Snapshots.Pop(Discard)) {}
	Bake = {};
	Reverb.Reset();
	WorldGeneration = 0;
	WorkerState.store(EIMWorkerState::Stopped, std::memory_order_release);
}

uint32 FIMAcousticSimulationWorker::Run()
{
	FIMAcousticSimulation Simulation;
	std::string Error;
	// H1 W1: Hybrid pathing is explicit (validation on, alternate paths on).
	// The adapter fails closed on unset options, so the product path sets them
	// before Load; creation then binds this known identity (see Load readback).
	if (!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), Error))
	{
		WorkerState.store(EIMWorkerState::LoadFailed, std::memory_order_release);
		if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
		return 1;
	}
	AppliedValidation.store(1, std::memory_order_release);
	if (!Simulation.Load(Bake, Bridge->SampleRate, Bridge->BlockFrames, Error))
	{
		WorkerState.store(EIMWorkerState::LoadFailed, std::memory_order_release);
		if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
		return 1;
	}
	TSharedPtr<IMAcousticSimulationWorkerPrivate::FIMAcousticReverbWorkState, ESPMode::ThreadSafe> ReverbWork;
	if (Reverb)
	{
		ReverbWork = MakeShared<IMAcousticSimulationWorkerPrivate::FIMAcousticReverbWorkState, ESPMode::ThreadSafe>();
		if (!ReverbWork->Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), Error)
			|| !ReverbWork->Simulation.Load(Bake, Bridge->SampleRate, Bridge->BlockFrames, Error))
		{
			WorkerState.store(EIMWorkerState::LoadFailed, std::memory_order_release);
			Reverb->Stopped.store(true, std::memory_order_release);
			return 1;
		}
	}
	Bake = {}; // No longer needed after SDK has loaded the immutable bytes.
	WorkerState.store(EIMWorkerState::Running, std::memory_order_release);
	struct FIMRequestLease
	{
		FIMAcousticVoiceRequest Request;double Seen=0.0;bool InSimulator=false;
		// Publication belongs to the world/voice channel, not the SDK source.
		// A stale snapshot removes SDK state, but recreating that state must not
		// restart sequence=1: audio would reject it behind the old high sequence.
		uint64 PublicationSequence=0;
	};
	TArray<FIMRequestLease> Leases;
	Leases.SetNum(Bridge->Voices.Num());
	TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe> Latest;
	constexpr double LeaseSeconds = 0.250; // Same safety lease as audio; missed release cannot leak a source forever.
	// The graph consumes 512 frames at 48 kHz (~10.67 ms). Keep the 50 ms
	// solve cadence so moving-route spatial samples remain stable, but let a
	// newly submitted GT snapshot wake the worker immediately. The 250 ms
	// lease remains the fail-closed lifetime; the event only removes idle
	// waiting after a listener or dynamic-geometry change.
	constexpr double UpdateSeconds = 0.050;
	UE::Tasks::FTask ReverbTask;
	// The reverb IR depends only on the snapshot (listener pose, dynamic meshes, transit policy), so solving the same
	// snapshot object again cannot change the IR or refresh its age: Target.CapturedSeconds is the snapshot's capture time.
	// The worker wakes twice per GT snapshot (50 ms timeout, then the snapshot event ~2 ms later). The timeout wake used to
	// start a redundant solve of the old snapshot; the single in-flight task was then still busy when the new snapshot's
	// wake arrived, so that version got no IR for ~50 ms and the environment operator counted it as not fresh
	// (W3.Pressure16x600: 24414 not-fresh blocks, 56 ms median from capture to IR ready). Cleared when a solve fails or is
	// cancelled so the same snapshot can be retried.
	TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe> LastReverbSnapshot;
	std::string LastReverbError;
	std::string LastDynamicError;
	std::string LastTransitError;
	const auto ConsumeReverbCompletion = [&]()
	{
		FIMAcousticWorkerProbe Completed{};
		if (!ReverbTask.IsValid() || !ReverbTask.IsCompleted()) return Completed;
		ReverbTask.Wait();
		Completed = ReverbWork->Probe;
		if (ReverbWork->Cancelled || !Completed.ReverbOk) LastReverbSnapshot.Reset();
		if (!ReverbWork->Cancelled && Completed.ReverbAttempt)
		{
			if (Completed.ReverbOk)
			{
				if (!LastReverbError.empty())
				{
					UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticReverbCoverage recovered"));
					LastReverbError.clear();
				}
			}
			else
			{
				if (ReverbWork->Error != LastReverbError)
				{
					UE_LOG(LogTemp, Warning, TEXT("IMLogs AcousticReverbDegraded %s listener=(%g,%g,%g)"),
						UTF8_TO_TCHAR(ReverbWork->Error.c_str()), ReverbWork->Listener.origin.x,
						ReverbWork->Listener.origin.y, ReverbWork->Listener.origin.z);
					LastReverbError = ReverbWork->Error;
				}
				FailedUpdates.fetch_add(1, std::memory_order_relaxed);
			}
		}
		ReverbTask = {};
		return Completed;
	};
	double PreviousUpdate=0;
	while (!StopRequested.load(std::memory_order_acquire) && Bridge->Alive.load(std::memory_order_acquire))
	{
		const double Started = FPlatformTime::Seconds();
		if(PreviousUpdate)IMAcousticTiming::AcousticRecordMaximum(Bridge->MaxWorkerGapUs,uint64((Started-PreviousUpdate)*1.e6));
		PreviousUpdate=Started;
		const uint64 ProfileStart=Bridge->ProfilingEnabled.load(std::memory_order_relaxed)?FPlatformTime::Cycles64():0;
		const bool bStallTiming = Bridge->StallTimingEnabled.load(std::memory_order_acquire);
		const FIMAcousticWorkerProbe CompletedReverbProbe = ConsumeReverbCompletion();
		TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe> Next;
		while (Snapshots.Pop(Next)) { Latest = MoveTemp(Next); }
		const bool SnapshotValid = Latest && Latest->WorldGeneration == WorldGeneration
			&& Started >= Latest->CapturedSeconds && Started - Latest->CapturedSeconds <= LeaseSeconds;
		std::vector<FIMAcousticDynamicMeshInput> DynamicInputs;
		if (SnapshotValid)
		{
			DynamicInputs.reserve(Latest->DynamicMeshes.Num());
			for (const FIMAcousticDynamicMeshSnapshot& Dynamic : Latest->DynamicMeshes)
			{
				FIMAcousticDynamicMeshInput Input;
				Input.Key = Dynamic.Key;
				Input.GeometryHash = Dynamic.GeometryHash;
				Input.Transform = Dynamic.Transform;
				Input.Geometry.Vertices.reserve(Dynamic.Vertices.Num());
				for (const IPLVector3& Vertex : Dynamic.Vertices) { Input.Geometry.Vertices.push_back(Vertex); }
				Input.Geometry.Triangles.reserve(Dynamic.Triangles.Num());
				for (const IPLTriangle& Triangle : Dynamic.Triangles) { Input.Geometry.Triangles.push_back(Triangle); }
				Input.Geometry.MaterialIndices.reserve(Dynamic.MaterialIndices.Num());
				for (int32 MaterialIndex : Dynamic.MaterialIndices)
				{
					Input.Geometry.MaterialIndices.push_back(static_cast<IPLint32>(MaterialIndex));
				}
				Input.Geometry.Materials.reserve(Dynamic.Materials.Num());
				for (const IPLMaterial& Material : Dynamic.Materials) { Input.Geometry.Materials.push_back(Material); }
				DynamicInputs.push_back(MoveTemp(Input));
			}
		}
		std::string DynamicError;
		const bool DynamicSyncValid = Simulation.SyncDynamicMeshes(DynamicInputs, DynamicError);
		if (!DynamicSyncValid)
		{
			FailedUpdates.fetch_add(1, std::memory_order_relaxed);
			if (DynamicError != LastDynamicError)
			{
				UE_LOG(LogTemp, Warning, TEXT("IMLogs AcousticDynamicSync FAIL error=%s"),
					UTF8_TO_TCHAR(DynamicError.c_str()));
				LastDynamicError = DynamicError;
			}
			std::vector<FIMAcousticDynamicMeshInput> EmptyDynamicInputs;
			std::string ClearDynamicError;
			Simulation.SyncDynamicMeshes(EmptyDynamicInputs, ClearDynamicError);
		}
		else if (!LastDynamicError.empty())
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticDynamicSync recovered"));
			LastDynamicError.clear();
		}
		FIMAcousticApertureTransitPolicy TransitPolicy;
		if (SnapshotValid && Latest->ApertureTransitEnabled)
		{
			TransitPolicy.Enabled = true;
			TransitPolicy.Center = Latest->ApertureTransitCenter;
			TransitPolicy.HalfExtent = Latest->ApertureTransitHalfExtent;
		}
		std::string TransitError;
		if (!Simulation.SetApertureTransitPolicy(TransitPolicy, TransitError))
		{
			FailedUpdates.fetch_add(1, std::memory_order_relaxed);
			if (TransitError != LastTransitError)
			{
				UE_LOG(LogTemp, Warning, TEXT("IMLogs AcousticApertureTransit FAIL error=%s"),
					UTF8_TO_TCHAR(TransitError.c_str()));
				LastTransitError = TransitError;
			}
		}
		else if (!LastTransitError.empty())
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticApertureTransit recovered"));
			LastTransitError.clear();
		}
	const bool GeometrySnapshotValid = SnapshotValid && DynamicSyncValid;
	// H1 W3 negative control: apply a GT-requested validation flip serially here.
	const int RequestedValidation = ValidationRequest.exchange(-1, std::memory_order_acq_rel);
	if (RequestedValidation >= 0)
	{
		FIMAcousticPathingOptions FlipOptions;
		FlipOptions.EnableValidation = RequestedValidation != 0;
		FlipOptions.FindAlternatePaths = RequestedValidation != 0;
		FlipOptions.HasEnableValidation = true;
		FlipOptions.HasFindAlternatePaths = true;
		std::string FlipError;
		if (Simulation.SetPathingOptions(FlipOptions, FlipError))
		{
			AppliedValidation.store(RequestedValidation, std::memory_order_release);
		}
		else { FailedUpdates.fetch_add(1, std::memory_order_relaxed); }
	}
		std::vector<FIMAcousticSourceInput> Inputs;
		std::vector<uint32> VoiceIndices;
		// W1 worker probe (worker thread only): per-loop timing plus the first
		// published result identity/params. W1 runs a single source, so the join
		// keys are complete; multi-voice iterations record the count plus first.
		// POD stores only, no log, no I/O, no wait, no lock, no allocation.
		// All Seconds fields are FPlatformTime::Seconds() wall clock, seconds.
		FIMAcousticWorkerProbe ProbeLocal = CompletedReverbProbe;
		ProbeLocal.WorldGeneration = WorldGeneration;
		ProbeLocal.LoopStartSeconds = Started;
		ProbeLocal.SnapshotCaptured = Latest ? Latest->CapturedSeconds : 0.0;
		if (!Latest) { ProbeLocal.SnapReason = 1; }
		else if (Latest->WorldGeneration != WorldGeneration) { ProbeLocal.SnapReason = 2; }
		else if (Started < Latest->CapturedSeconds) { ProbeLocal.SnapReason = 4; }
		else if (Started - Latest->CapturedSeconds > LeaseSeconds) { ProbeLocal.SnapReason = 3; }
		else { ProbeLocal.SnapReason = 0; }
		ProbeLocal.SnapValid = SnapshotValid ? 1 : 0;
		ProbeLocal.EvalOk = 2;
		ProbeLocal.PushOk = 2; // no result to push unless a batch publishes below
		for (int32 VoiceIndex = 0; VoiceIndex < Leases.Num(); ++VoiceIndex)
		{
			auto& Lease = Leases[VoiceIndex];
			FIMAcousticVoiceRequest Request;
			while (Bridge->Voices[VoiceIndex]->Requests.Pop(Request))
			{
				if (Request.Voice == static_cast<uint32>(VoiceIndex)) { Lease.Request = Request; Lease.Seen = Started; }
			}
			const FIMAcousticSourceSnapshot* Source = nullptr;
			if (GeometrySnapshotValid && Lease.Request.Active && Started - Lease.Seen <= LeaseSeconds)
			{
				Source = Latest->Sources.FindByPredicate([&Lease](const auto& S)
					{ return S.AudioComponentId == Lease.Request.AudioComponentId; });
			}
			if (!Source)
			{
				if (Lease.InSimulator) { Simulation.Remove(VoiceIndex); Lease.InSimulator = false; }
				continue;
			}
			Inputs.push_back({static_cast<uint64>(VoiceIndex), Lease.Request.Generation, Source->Source});
			VoiceIndices.push_back(VoiceIndex);
			Lease.InSimulator = true;
		}
		// Sub-phase diagnostic: snapshot drain + lease/request match (zero behavior).
		if(ProfileStart)Bridge->WorkerSnapshotTiming.Record(ProfileStart);
		ProbeLocal.NumInputs = uint8(Inputs.size() > 255 ? 255 : Inputs.size());
		ProbeLocal.NumDynamicSynced = uint8(DynamicInputs.size() > 255 ? 255 : DynamicInputs.size());
		ProbeLocal.AppliedValidationSnap = int8(AppliedValidation.load(std::memory_order_acquire));
		// Sub-phase diagnostic: voice solve incl. result push (nested publish below).
		const uint64 SolveStart=ProfileStart?FPlatformTime::Cycles64():0;
		if (!Inputs.empty())
		{
			std::vector<FIMAcousticAudioFrame> Outputs;
			ProbeLocal.EvalStartSeconds = FPlatformTime::Seconds();
			const bool EvalOkLocal = Simulation.EvaluateBatch(Inputs, Latest->Listener, Outputs, Error)
				&& Outputs.size() == Inputs.size();
			if (Simulation.GetApertureTransitPolicy().Enabled)
			{
				Bridge->ApertureTransitChecks.fetch_add(1, std::memory_order_relaxed);
				Bridge->ApertureTransitTiming.RecordCycles(Simulation.GetLastApertureTransitCycles());
				if (Simulation.WasApertureTransitBlocked())
					Bridge->ApertureTransitBlocked.fetch_add(1, std::memory_order_relaxed);
			}
			ProbeLocal.EvalEndSeconds = FPlatformTime::Seconds();
			ProbeLocal.EvalOk = EvalOkLocal ? 1 : 0;
			if (EvalOkLocal)
			{
				ProbeLocal.ResultsPublished = uint8(Outputs.size() > 255 ? 255 : Outputs.size());
				// Sub-phase diagnostic: result-push loop isolated from solve.
				const uint64 PubStart=ProfileStart?FPlatformTime::Cycles64():0;
				bool AllPushed = true;
				for (size_t BatchIndex = 0; BatchIndex < Outputs.size(); ++BatchIndex)
				{
					const uint32 VoiceIndex = VoiceIndices[BatchIndex];
					FIMAcousticVoiceResult Result;
					Result.Frame = Outputs[BatchIndex];
					Result.Frame.Sequence=++Leases[VoiceIndex].PublicationSequence;
					if (BatchIndex == 0)
					{
						ProbeLocal.FirstVoice = VoiceIndex;
						ProbeLocal.FirstVoiceGen = Result.Frame.Generation;
						ProbeLocal.FirstSeq = Result.Frame.Sequence;
						ProbeLocal.DirectFlags = static_cast<uint32>(Result.Frame.Direct.flags);
						ProbeLocal.Occlusion = Result.Frame.Direct.occlusion;
						ProbeLocal.DistanceGain = Result.Frame.Direct.distanceAttenuation;
						ProbeLocal.Directivity = Result.Frame.Direct.directivity;
						for (int B = 0; B < 3; ++B)
						{
							ProbeLocal.AirAbsorption[B] = Result.Frame.Direct.airAbsorption[B];
							ProbeLocal.PathEQ[B] = Result.Frame.PathEQ[B];
						}
						for (int C = 0; C < 4; ++C) { ProbeLocal.PathSH[C] = Result.Frame.PathSH[C]; }
						ProbeLocal.PathValid0 = Result.Frame.PathValid ? 1 : 0;
					}
					if(Result.Frame.Sequence==1)
						UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticFirstFrame voice=%u source=(%g,%g,%g) listener=(%g,%g,%g) direct=%d occlusion=%g path=%d eq=(%g,%g,%g)"),VoiceIndex,Inputs[BatchIndex].Source.origin.x,Inputs[BatchIndex].Source.origin.y,Inputs[BatchIndex].Source.origin.z,Latest->Listener.origin.x,Latest->Listener.origin.y,Latest->Listener.origin.z,int(Result.Frame.DirectValid),Result.Frame.Direct.occlusion,int(Result.Frame.PathValid),Result.Frame.PathEQ[0],Result.Frame.PathEQ[1],Result.Frame.PathEQ[2]);
					// Single-source occlusion flip detector (zero behavior):
					// static scene + static transforms must hold occlusion
					// constant; any flip with movement-free inputs proves
					// simulator-side nondeterminism, not GT/render races.
					if(Inputs.size()==1)
					{
						static bool OccInit=false;static float OccLast=0;static IPLVector3 SrcLast{0,0,0};static IPLVector3 LisLast{0,0,0};
						const float OccNow=Result.Frame.Direct.occlusion;
						const IPLVector3 SrcNow=Inputs[BatchIndex].Source.origin;const IPLVector3 LisNow=Latest->Listener.origin;
						const bool Moved=SrcNow.x!=SrcLast.x||SrcNow.y!=SrcLast.y||SrcNow.z!=SrcLast.z||LisNow.x!=LisLast.x||LisNow.y!=LisLast.y||LisNow.z!=LisLast.z;
						if(OccInit&&(OccNow!=OccLast||Moved))
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticOccFlip seq=%llu occ_old=%g occ_new=%g moved=%d src=(%g,%g,%g) lis=(%g,%g,%g)"),Result.Frame.Sequence,OccLast,OccNow,int(Moved),SrcNow.x,SrcNow.y,SrcNow.z,LisNow.x,LisNow.y,LisNow.z);
						OccInit=true;OccLast=OccNow;SrcLast=SrcNow;LisLast=LisNow;
					}
					Result.AudioComponentId = Leases[VoiceIndex].Request.AudioComponentId;
					if (BatchIndex == 0) { ProbeLocal.FirstAudioId = Result.AudioComponentId; }
					Result.WorldGeneration = WorldGeneration;
					Result.SpatialVersion = Latest->SpatialVersion;
					// Age is based on the original GT snapshot, not republished now.
					Result.PublishedSeconds = Latest->CapturedSeconds;
					if (!Bridge->Voices[VoiceIndex]->Results.Push(Result)) { AllPushed = false; }
				}
				if(PubStart)Bridge->WorkerPublishTiming.Record(PubStart);
				ProbeLocal.PushSeconds = FPlatformTime::Seconds();
				ProbeLocal.PushOk = AllPushed ? 1 : 0;
			}
			else { FailedUpdates.fetch_add(1, std::memory_order_relaxed); ProbeLocal.PushOk = 2; }
			if(SolveStart)Bridge->WorkerSolveTiming.Record(SolveStart);
		}
		else if(SolveStart)Bridge->WorkerSolveTiming.Record(SolveStart);
		const bool bReverbRouteEnabled = (Bridge->RenderRoutes.load(std::memory_order_relaxed) & 4u) != 0;
		if (GeometrySnapshotValid && bReverbRouteEnabled && ReverbWork && !ReverbTask.IsValid()
			&& Latest != LastReverbSnapshot && !Reverb->Stopped.load(std::memory_order_acquire))
		{
			for (auto& Slot : Reverb->Slots)
			{
				auto Expected = EIMAcousticIRState::Free;
				if (!Slot.State.compare_exchange_strong(Expected, EIMAcousticIRState::Writing, std::memory_order_acq_rel)) continue;
				const uint32 SlotIndex = uint32(&Slot - &Reverb->Slots[0]);
				LastReverbSnapshot = Latest;
				ReverbTask = UE::Tasks::Launch(UE_SOURCE_LOCATION,
					[Work = ReverbWork, Pool = Reverb, Device = Bridge, Snapshot = Latest,
						Meshes = MoveTemp(DynamicInputs), TransitPolicy, SlotIndex, bStallTiming,
						bProfile = ProfileStart != 0]()
					{
						Work->Probe = {};
						Work->Error.clear();
						Work->Cancelled = Pool->Stopped.load(std::memory_order_acquire)
							|| !Device->Alive.load(std::memory_order_acquire)
							|| Device->WorldGeneration.load(std::memory_order_acquire) != Snapshot->WorldGeneration;
						auto& Target = Pool->Slots[SlotIndex];
						if (Work->Cancelled)
						{
							Target.State.store(EIMAcousticIRState::Free, std::memory_order_release);
							return;
						}
						Work->Listener = Snapshot->Listener;
						auto& Probe = Work->Probe;
						Probe.ReverbAttempt = 1;
						Probe.ReverbSlot = SlotIndex;
						Probe.ReverbStartSeconds = FPlatformTime::Seconds();
						const uint64 ReverbProfileStart = bProfile ? FPlatformTime::Cycles64() : 0;
						FIMAcousticStallThreadClock CpuBefore, CpuAfter;
						if (bStallTiming) CpuBefore = IM_AcousticReadStallThreadClock();
						bool ReverbOk = false;
						{
							TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(IMDEBUG_AC0848_WorkerReverb, bStallTiming);
							ReverbOk = Work->Simulation.SyncDynamicMeshes(Meshes, Work->Error)
								&& Work->Simulation.SetApertureTransitPolicy(TransitPolicy, Work->Error)
								&& Work->Simulation.EvaluateReverb(Target, Snapshot->Listener, Work->Error);
						}
						if (bStallTiming)
						{
							CpuAfter = IM_AcousticReadStallThreadClock();
							Probe.ReverbCpuWallBeforeSeconds = CpuBefore.Seconds;
							Probe.ReverbCpuWallAfterSeconds = CpuAfter.Seconds;
							Probe.ReverbCpu100nsBefore = CpuBefore.Cpu100ns;
							Probe.ReverbCpu100nsAfter = CpuAfter.Cpu100ns;
							Probe.ReverbCpuCyclesBefore = CpuBefore.CpuCycles;
							Probe.ReverbCpuCyclesAfter = CpuAfter.CpuCycles;
							Probe.ReverbCpuThreadIdBefore = CpuBefore.ThreadId;
							Probe.ReverbCpuThreadIdAfter = CpuAfter.ThreadId;
							Probe.ReverbCpuValidBefore = CpuBefore.Valid;
							Probe.ReverbCpuValidAfter = CpuAfter.Valid;
						}
						if (ReverbProfileStart) Device->WorkerReverbTiming.Record(ReverbProfileStart);
						Probe.ReverbEndSeconds = FPlatformTime::Seconds();
						Work->Cancelled = Pool->Stopped.load(std::memory_order_acquire)
							|| !Device->Alive.load(std::memory_order_acquire)
							|| Device->WorldGeneration.load(std::memory_order_acquire) != Snapshot->WorldGeneration;
						if (ReverbOk && !Work->Cancelled)
						{
							Target.Sequence = ++Work->Sequence;
							Target.SpatialVersion = Snapshot->SpatialVersion;
							Target.CapturedSeconds = Snapshot->CapturedSeconds;
							Probe.ReverbOk = 1;
							Probe.ReverbSequence = Target.Sequence;
							Probe.ReverbCaptured = Target.CapturedSeconds;
							Probe.ReverbSpatialVersion = Target.SpatialVersion;
							if (bProfile) Probe.ReverbReadyBeforeSeconds = FPlatformTime::Seconds();
							Target.State.store(EIMAcousticIRState::Ready, std::memory_order_release);
							if (bProfile) Probe.ReverbReadyAfterSeconds = FPlatformTime::Seconds();
						}
						else Target.State.store(EIMAcousticIRState::Free, std::memory_order_release);
					},
					// High: capture-to-IR-ready is 7 ms median but must stay under the 21.3 ms input delay (2 blocks of 512 frames at 48 kHz); the
					// measured tail was one solve descheduled for up to 22 ms behind normal-priority work (p3 run, two not-fresh environment blocks).
					LowLevelTasks::ETaskPriority::High);
				break;
			}
		}
		if(ProfileStart)Bridge->WorkerTiming.Record(ProfileStart);
		const double Remaining = UpdateSeconds - (FPlatformTime::Seconds() - Started);
		if (SnapshotEvent)
		{
			const uint32 WaitMilliseconds = Remaining > 0.0
				? static_cast<uint32>(FMath::Clamp(FMath::CeilToInt(Remaining * 1000.0), 1, 1000))
				: 0;
			SnapshotEvent->Wait(WaitMilliseconds);
		}
		else if (Remaining > 0.0) { FPlatformProcess::SleepNoStats(static_cast<float>(Remaining)); }
		ProbeLocal.WaitEndSeconds = FPlatformTime::Seconds();
		Bridge->TraceWorker(ProbeLocal);
	}
	if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
	if (ReverbTask.IsValid())
	{
		ReverbTask.Wait();
		auto Completed = ConsumeReverbCompletion();
		Completed.WorldGeneration = WorldGeneration;
		Completed.LoopStartSeconds = FPlatformTime::Seconds();
		Completed.WaitEndSeconds = Completed.LoopStartSeconds;
		if (Completed.ReverbAttempt) Bridge->TraceWorker(Completed);
	}
	return 0; // Simulator dies here; detached source leases survive in the shared IR pool.
}
