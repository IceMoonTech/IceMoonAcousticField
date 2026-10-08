#include "IMAcousticSimulationWorker.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

FIMAcousticSimulationWorker::FIMAcousticSimulationWorker() = default;
FIMAcousticSimulationWorker::~FIMAcousticSimulationWorker() { StopAndJoin(); }

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
	StopRequested.store(false, std::memory_order_release);
	WorkerState.store(EIMWorkerState::Loading, std::memory_order_release);
	Thread.Reset(FRunnableThread::Create(this, TEXT("IMAcousticSimulation"), 0, TPri_Normal));
	if (!Thread) { StopAndJoin(); return false; }
	return true;
}

bool FIMAcousticSimulationWorker::Submit(TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe> Snapshot)
{
	check(IsInGameThread());
	return Thread && Snapshot && Snapshot->WorldGeneration == WorldGeneration && Snapshots.Push(Snapshot);
}

void FIMAcousticSimulationWorker::Stop() { StopRequested.store(true, std::memory_order_release); }

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
	constexpr double UpdateSeconds = 0.050; // 20Hz simulation; audio interpolates/render state separately.
	uint64 ReverbSequence=0;
	std::string LastReverbError;
	std::string LastDynamicError;
	std::string LastTransitError;
	double PreviousUpdate=0;
	while (!StopRequested.load(std::memory_order_acquire) && Bridge->Alive.load(std::memory_order_acquire))
	{
		const double Started = FPlatformTime::Seconds();
		if(PreviousUpdate)IMAcousticTiming::AcousticRecordMaximum(Bridge->MaxWorkerGapUs,uint64((Started-PreviousUpdate)*1.e6));
		PreviousUpdate=Started;
		const uint64 ProfileStart=Bridge->ProfilingEnabled.load(std::memory_order_relaxed)?FPlatformTime::Cycles64():0;
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
		FIMAcousticWorkerProbe ProbeLocal{};
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
		for (int32 I = 0; I < Leases.Num(); ++I)
		{
			auto& Lease = Leases[I];
			FIMAcousticVoiceRequest Request;
			while (Bridge->Voices[I]->Requests.Pop(Request))
			{
				if (Request.Voice == static_cast<uint32>(I)) { Lease.Request = Request; Lease.Seen = Started; }
			}
			const FIMAcousticSourceSnapshot* Source = nullptr;
			if (GeometrySnapshotValid && Lease.Request.Active && Started - Lease.Seen <= LeaseSeconds)
			{
				Source = Latest->Sources.FindByPredicate([&Lease](const auto& S)
					{ return S.AudioComponentId == Lease.Request.AudioComponentId; });
			}
			if (!Source)
			{
				if (Lease.InSimulator) { Simulation.Remove(I); Lease.InSimulator = false; }
				continue;
			}
			Inputs.push_back({static_cast<uint64>(I), Lease.Request.Generation, Source->Source});
			VoiceIndices.push_back(I);
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
				for (size_t I = 0; I < Outputs.size(); ++I)
				{
					const uint32 Voice = VoiceIndices[I];
					FIMAcousticVoiceResult Result;
					Result.Frame = Outputs[I];
					Result.Frame.Sequence=++Leases[Voice].PublicationSequence;
					if (I == 0)
					{
						ProbeLocal.FirstVoice = Voice;
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
						UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticFirstFrame voice=%u source=(%g,%g,%g) listener=(%g,%g,%g) direct=%d occlusion=%g path=%d eq=(%g,%g,%g)"),Voice,Inputs[I].Source.origin.x,Inputs[I].Source.origin.y,Inputs[I].Source.origin.z,Latest->Listener.origin.x,Latest->Listener.origin.y,Latest->Listener.origin.z,int(Result.Frame.DirectValid),Result.Frame.Direct.occlusion,int(Result.Frame.PathValid),Result.Frame.PathEQ[0],Result.Frame.PathEQ[1],Result.Frame.PathEQ[2]);
					// Single-source occlusion flip detector (zero behavior):
					// static scene + static transforms must hold occlusion
					// constant; any flip with movement-free inputs proves
					// simulator-side nondeterminism, not GT/render races.
					if(Inputs.size()==1)
					{
						static bool OccInit=false;static float OccLast=0;static IPLVector3 SrcLast{0,0,0};static IPLVector3 LisLast{0,0,0};
						const float OccNow=Result.Frame.Direct.occlusion;
						const IPLVector3 SrcNow=Inputs[I].Source.origin;const IPLVector3 LisNow=Latest->Listener.origin;
						const bool Moved=SrcNow.x!=SrcLast.x||SrcNow.y!=SrcLast.y||SrcNow.z!=SrcLast.z||LisNow.x!=LisLast.x||LisNow.y!=LisLast.y||LisNow.z!=LisLast.z;
						if(OccInit&&(OccNow!=OccLast||Moved))
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticOccFlip seq=%llu occ_old=%g occ_new=%g moved=%d src=(%g,%g,%g) lis=(%g,%g,%g)"),Result.Frame.Sequence,OccLast,OccNow,int(Moved),SrcNow.x,SrcNow.y,SrcNow.z,LisNow.x,LisNow.y,LisNow.z);
						OccInit=true;OccLast=OccNow;SrcLast=SrcNow;LisLast=LisNow;
					}
					Result.AudioComponentId = Leases[Voice].Request.AudioComponentId;
					if (I == 0) { ProbeLocal.FirstAudioId = Result.AudioComponentId; }
					Result.WorldGeneration = WorldGeneration;
					// Age is based on the original GT snapshot, not republished now.
					Result.PublishedSeconds = Latest->CapturedSeconds;
					if (!Bridge->Voices[Voice]->Results.Push(Result)) { AllPushed = false; }
				}
				if(PubStart)Bridge->WorkerPublishTiming.Record(PubStart);
				ProbeLocal.PushSeconds = FPlatformTime::Seconds();
				ProbeLocal.PushOk = AllPushed ? 1 : 0;
			}
			else { FailedUpdates.fetch_add(1, std::memory_order_relaxed); ProbeLocal.PushOk = 2; }
			if(SolveStart)Bridge->WorkerSolveTiming.Record(SolveStart);
		}
		else if(SolveStart)Bridge->WorkerSolveTiming.Record(SolveStart);
		if(GeometrySnapshotValid&&Reverb&&!Reverb->Stopped.load(std::memory_order_acquire))
		{
			for(auto& Slot:Reverb->Slots)
			{
				auto Expected=EIMAcousticIRState::Free;
				if(!Slot.State.compare_exchange_strong(Expected,EIMAcousticIRState::Writing,std::memory_order_acq_rel))continue;
				ProbeLocal.ReverbAttempt = 1;
				ProbeLocal.ReverbStartSeconds = FPlatformTime::Seconds();
				// Sub-phase diagnostic: SDK reverb eval isolated from solve/publish.
				const uint64 RevStart=ProfileStart?FPlatformTime::Cycles64():0;
				const bool ReverbOkLocal = Simulation.EvaluateReverb(Slot,Latest->Listener,Error);
				if(RevStart)Bridge->WorkerReverbTiming.Record(RevStart);
				ProbeLocal.ReverbEndSeconds = FPlatformTime::Seconds();
				if(ReverbOkLocal)
				{
					if(!LastReverbError.empty()){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticReverbCoverage recovered"));LastReverbError.clear();}
					Slot.Sequence=++ReverbSequence;Slot.CapturedSeconds=Latest->CapturedSeconds;
					ProbeLocal.ReverbOk = 1;
					ProbeLocal.ReverbSequence = Slot.Sequence;
					ProbeLocal.ReverbCaptured = Slot.CapturedSeconds;
					Slot.State.store(EIMAcousticIRState::Ready,std::memory_order_release);
				}
				else
				{
					if(Error!=LastReverbError)
					{UE_LOG(LogTemp,Warning,TEXT("IMLogs AcousticReverbDegraded %s listener=(%g,%g,%g)"),UTF8_TO_TCHAR(Error.c_str()),Latest->Listener.origin.x,Latest->Listener.origin.y,Latest->Listener.origin.z);LastReverbError=Error;}
					FailedUpdates.fetch_add(1);Slot.State.store(EIMAcousticIRState::Free,std::memory_order_release);
				}
				break;
			}
		}
		if(ProfileStart)Bridge->WorkerTiming.Record(ProfileStart);
		const double Remaining = UpdateSeconds - (FPlatformTime::Seconds() - Started);
		if (Remaining > 0.0) { FPlatformProcess::SleepNoStats(static_cast<float>(Remaining)); }
		ProbeLocal.WaitEndSeconds = FPlatformTime::Seconds();
		Bridge->TraceWorker(ProbeLocal);
	}
	if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
	return 0; // Simulator dies here; detached source leases survive in the shared IR pool.
}
