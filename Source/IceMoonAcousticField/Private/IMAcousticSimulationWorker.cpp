#include "IMAcousticSimulationWorker.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

IM_AcousticSimulationWorker::IM_AcousticSimulationWorker() = default;
IM_AcousticSimulationWorker::~IM_AcousticSimulationWorker() { StopAndJoin(); }

bool IM_AcousticSimulationWorker::Start(TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> InBridge,
    uint64 InWorldGeneration, IM_AcousticBakeData&& InBake,
    TSharedPtr<IM_AcousticReverbPool,ESPMode::ThreadSafe> InReverb)
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
    WorkerState.store(State::Loading, std::memory_order_release);
    Thread.Reset(FRunnableThread::Create(this, TEXT("IMAcousticSimulation"), 0, TPri_Normal));
    if (!Thread) { StopAndJoin(); return false; }
    return true;
}

bool IM_AcousticSimulationWorker::Submit(TSharedPtr<const IM_AcousticWorldSnapshot, ESPMode::ThreadSafe> Snapshot)
{
    check(IsInGameThread());
    return Thread && Snapshot && Snapshot->WorldGeneration == WorldGeneration && Snapshots.Push(Snapshot);
}

void IM_AcousticSimulationWorker::Stop() { StopRequested.store(true, std::memory_order_release); }

void IM_AcousticSimulationWorker::StopAndJoin()
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
    TSharedPtr<const IM_AcousticWorldSnapshot, ESPMode::ThreadSafe> Discard;
    while (Snapshots.Pop(Discard)) {}
    Bake = {};
    Reverb.Reset();
    WorldGeneration = 0;
    WorkerState.store(State::Stopped, std::memory_order_release);
}

uint32 IM_AcousticSimulationWorker::Run()
{
    IM_AcousticSimulation Simulation;
    std::string Error;
    // H1 W1: Hybrid pathing is explicit (validation on, alternate paths on).
    // The adapter fails closed on unset options, so the product path sets them
    // before Load; creation then binds this known identity (see Load readback).
    if (!Simulation.SetPathingOptions(IM_AcousticPathingOptions::DefaultHybrid(), Error))
    {
        WorkerState.store(State::LoadFailed, std::memory_order_release);
        if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
        return 1;
    }
    AppliedValidation.store(1, std::memory_order_release);
    if (!Simulation.Load(Bake, Bridge->SampleRate, Bridge->BlockFrames, Error))
    {
        WorkerState.store(State::LoadFailed, std::memory_order_release);
        if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
        return 1;
    }
    Bake = {}; // No longer needed after SDK has loaded the immutable bytes.
    WorkerState.store(State::Running, std::memory_order_release);
    struct IM_RequestLease
    {
        IM_AcousticVoiceRequest Request;double Seen=0.0;bool InSimulator=false;
        // Publication belongs to the world/voice channel, not the SDK source.
        // A stale snapshot removes SDK state, but recreating that state must not
        // restart sequence=1: audio would reject it behind the old high sequence.
        uint64 PublicationSequence=0;
    };
    TArray<IM_RequestLease> Leases;
    Leases.SetNum(Bridge->Voices.Num());
    TSharedPtr<const IM_AcousticWorldSnapshot, ESPMode::ThreadSafe> Latest;
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
        if(PreviousUpdate)IM_AcousticRecordMaximum(Bridge->MaxWorkerGapUs,uint64((Started-PreviousUpdate)*1.e6));
        PreviousUpdate=Started;
        const uint64 ProfileStart=Bridge->ProfilingEnabled.load(std::memory_order_relaxed)?FPlatformTime::Cycles64():0;
        TSharedPtr<const IM_AcousticWorldSnapshot, ESPMode::ThreadSafe> Next;
        while (Snapshots.Pop(Next)) { Latest = MoveTemp(Next); }
        const bool SnapshotValid = Latest && Latest->WorldGeneration == WorldGeneration
            && Started >= Latest->CapturedSeconds && Started - Latest->CapturedSeconds <= LeaseSeconds;
        std::vector<IM_AcousticDynamicMeshInput> DynamicInputs;
        if (SnapshotValid)
        {
            DynamicInputs.reserve(Latest->DynamicMeshes.Num());
            for (const IM_AcousticDynamicMeshSnapshot& Dynamic : Latest->DynamicMeshes)
            {
                IM_AcousticDynamicMeshInput Input;
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
            std::vector<IM_AcousticDynamicMeshInput> EmptyDynamicInputs;
            std::string ClearDynamicError;
            Simulation.SyncDynamicMeshes(EmptyDynamicInputs, ClearDynamicError);
        }
        else if (!LastDynamicError.empty())
        {
            UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticDynamicSync recovered"));
            LastDynamicError.clear();
        }
        IM_AcousticApertureTransitPolicy TransitPolicy;
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
        IM_AcousticPathingOptions FlipOptions;
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
        std::vector<IM_AcousticSourceInput> Inputs;
        std::vector<uint32> VoiceIndices;
        // W1 worker probe (worker thread only): per-loop timing plus the first
        // published result identity/params. W1 runs a single source, so the join
        // keys are complete; multi-voice iterations record the count plus first.
        // POD stores only, no log, no I/O, no wait, no lock, no allocation.
        // All Seconds fields are FPlatformTime::Seconds() wall clock, seconds.
        IM_AcousticWorkerProbe IMProbe{};
        IMProbe.WorldGeneration = WorldGeneration;
        IMProbe.LoopStartSeconds = Started;
        IMProbe.SnapshotCaptured = Latest ? Latest->CapturedSeconds : 0.0;
        if (!Latest) { IMProbe.SnapReason = 1; }
        else if (Latest->WorldGeneration != WorldGeneration) { IMProbe.SnapReason = 2; }
        else if (Started < Latest->CapturedSeconds) { IMProbe.SnapReason = 4; }
        else if (Started - Latest->CapturedSeconds > LeaseSeconds) { IMProbe.SnapReason = 3; }
        else { IMProbe.SnapReason = 0; }
        IMProbe.SnapValid = SnapshotValid ? 1 : 0;
        IMProbe.EvalOk = 2;
        IMProbe.PushOk = 2; // no result to push unless a batch publishes below
        for (int32 I = 0; I < Leases.Num(); ++I)
        {
            auto& Lease = Leases[I];
            IM_AcousticVoiceRequest Request;
            while (Bridge->Voices[I]->Requests.Pop(Request))
            {
                if (Request.Voice == static_cast<uint32>(I)) { Lease.Request = Request; Lease.Seen = Started; }
            }
            const IM_AcousticSourceSnapshot* Source = nullptr;
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
        IMProbe.NumInputs = uint8(Inputs.size() > 255 ? 255 : Inputs.size());
        IMProbe.NumDynamicSynced = uint8(DynamicInputs.size() > 255 ? 255 : DynamicInputs.size());
        IMProbe.AppliedValidationSnap = int8(AppliedValidation.load(std::memory_order_acquire));
        // Sub-phase diagnostic: voice solve incl. result push (nested publish below).
        const uint64 IMSolveStart=ProfileStart?FPlatformTime::Cycles64():0;
        if (!Inputs.empty())
        {
            std::vector<IM_AcousticAudioFrame> Outputs;
            IMProbe.EvalStartSeconds = FPlatformTime::Seconds();
            const bool IMEvalOk = Simulation.EvaluateBatch(Inputs, Latest->Listener, Outputs, Error)
                && Outputs.size() == Inputs.size();
            if (Simulation.GetApertureTransitPolicy().Enabled)
            {
                Bridge->ApertureTransitChecks.fetch_add(1, std::memory_order_relaxed);
                Bridge->ApertureTransitTiming.RecordCycles(Simulation.GetLastApertureTransitCycles());
                if (Simulation.WasApertureTransitBlocked())
                    Bridge->ApertureTransitBlocked.fetch_add(1, std::memory_order_relaxed);
            }
            IMProbe.EvalEndSeconds = FPlatformTime::Seconds();
            IMProbe.EvalOk = IMEvalOk ? 1 : 0;
            if (IMEvalOk)
            {
                IMProbe.ResultsPublished = uint8(Outputs.size() > 255 ? 255 : Outputs.size());
                // Sub-phase diagnostic: result-push loop isolated from solve.
                const uint64 IMPubStart=ProfileStart?FPlatformTime::Cycles64():0;
                bool IMAllPushed = true;
                for (size_t I = 0; I < Outputs.size(); ++I)
                {
                    const uint32 Voice = VoiceIndices[I];
                    IM_AcousticVoiceResult Result;
                    Result.Frame = Outputs[I];
                    Result.Frame.Sequence=++Leases[Voice].PublicationSequence;
                    if (I == 0)
                    {
                        IMProbe.FirstVoice = Voice;
                        IMProbe.FirstVoiceGen = Result.Frame.Generation;
                        IMProbe.FirstSeq = Result.Frame.Sequence;
                        IMProbe.DirectFlags = static_cast<uint32>(Result.Frame.Direct.flags);
                        IMProbe.Occlusion = Result.Frame.Direct.occlusion;
                        IMProbe.DistanceGain = Result.Frame.Direct.distanceAttenuation;
                        IMProbe.Directivity = Result.Frame.Direct.directivity;
                        for (int B = 0; B < 3; ++B)
                        {
                            IMProbe.AirAbsorption[B] = Result.Frame.Direct.airAbsorption[B];
                            IMProbe.PathEQ[B] = Result.Frame.PathEQ[B];
                        }
                        for (int C = 0; C < 4; ++C) { IMProbe.PathSH[C] = Result.Frame.PathSH[C]; }
                        IMProbe.PathValid0 = Result.Frame.PathValid ? 1 : 0;
                    }
                    if(Result.Frame.Sequence==1)
                        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticFirstFrame voice=%u source=(%g,%g,%g) listener=(%g,%g,%g) direct=%d occlusion=%g path=%d eq=(%g,%g,%g)"),Voice,Inputs[I].Source.origin.x,Inputs[I].Source.origin.y,Inputs[I].Source.origin.z,Latest->Listener.origin.x,Latest->Listener.origin.y,Latest->Listener.origin.z,int(Result.Frame.DirectValid),Result.Frame.Direct.occlusion,int(Result.Frame.PathValid),Result.Frame.PathEQ[0],Result.Frame.PathEQ[1],Result.Frame.PathEQ[2]);
                    // Single-source occlusion flip detector (zero behavior):
                    // static scene + static transforms must hold occlusion
                    // constant; any flip with movement-free inputs proves
                    // simulator-side nondeterminism, not GT/render races.
                    if(Inputs.size()==1)
                    {
                        static bool IM_OccInit=false;static float IM_OccLast=0;static IPLVector3 IM_SrcLast{0,0,0};static IPLVector3 IM_LisLast{0,0,0};
                        const float OccNow=Result.Frame.Direct.occlusion;
                        const IPLVector3 SrcNow=Inputs[I].Source.origin;const IPLVector3 LisNow=Latest->Listener.origin;
                        const bool Moved=SrcNow.x!=IM_SrcLast.x||SrcNow.y!=IM_SrcLast.y||SrcNow.z!=IM_SrcLast.z||LisNow.x!=IM_LisLast.x||LisNow.y!=IM_LisLast.y||LisNow.z!=IM_LisLast.z;
                        if(IM_OccInit&&(OccNow!=IM_OccLast||Moved))
                            UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticOccFlip seq=%llu occ_old=%g occ_new=%g moved=%d src=(%g,%g,%g) lis=(%g,%g,%g)"),Result.Frame.Sequence,IM_OccLast,OccNow,int(Moved),SrcNow.x,SrcNow.y,SrcNow.z,LisNow.x,LisNow.y,LisNow.z);
                        IM_OccInit=true;IM_OccLast=OccNow;IM_SrcLast=SrcNow;IM_LisLast=LisNow;
                    }
                    Result.AudioComponentId = Leases[Voice].Request.AudioComponentId;
                    if (I == 0) { IMProbe.FirstAudioId = Result.AudioComponentId; }
                    Result.WorldGeneration = WorldGeneration;
                    // Age is based on the original GT snapshot, not republished now.
                    Result.PublishedSeconds = Latest->CapturedSeconds;
                    if (!Bridge->Voices[Voice]->Results.Push(Result)) { IMAllPushed = false; }
                }
                if(IMPubStart)Bridge->WorkerPublishTiming.Record(IMPubStart);
                IMProbe.PushSeconds = FPlatformTime::Seconds();
                IMProbe.PushOk = IMAllPushed ? 1 : 0;
            }
            else { FailedUpdates.fetch_add(1, std::memory_order_relaxed); IMProbe.PushOk = 2; }
            if(IMSolveStart)Bridge->WorkerSolveTiming.Record(IMSolveStart);
        }
        else if(IMSolveStart)Bridge->WorkerSolveTiming.Record(IMSolveStart);
        if(GeometrySnapshotValid&&Reverb&&!Reverb->Stopped.load(std::memory_order_acquire))
        {
            for(auto& Slot:Reverb->Slots)
            {
                auto Expected=IM_AcousticIRState::Free;
                if(!Slot.State.compare_exchange_strong(Expected,IM_AcousticIRState::Writing,std::memory_order_acq_rel))continue;
                IMProbe.ReverbAttempt = 1;
                IMProbe.ReverbStartSeconds = FPlatformTime::Seconds();
                // Sub-phase diagnostic: SDK reverb eval isolated from solve/publish.
                const uint64 IMRevStart=ProfileStart?FPlatformTime::Cycles64():0;
                const bool IMReverbOk = Simulation.EvaluateReverb(Slot,Latest->Listener,Error);
                if(IMRevStart)Bridge->WorkerReverbTiming.Record(IMRevStart);
                IMProbe.ReverbEndSeconds = FPlatformTime::Seconds();
                if(IMReverbOk)
                {
                    if(!LastReverbError.empty()){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticReverbCoverage recovered"));LastReverbError.clear();}
                    Slot.Sequence=++ReverbSequence;Slot.CapturedSeconds=Latest->CapturedSeconds;
                    IMProbe.ReverbOk = 1;
                    IMProbe.ReverbSequence = Slot.Sequence;
                    IMProbe.ReverbCaptured = Slot.CapturedSeconds;
                    Slot.State.store(IM_AcousticIRState::Ready,std::memory_order_release);
                }
                else
                {
                    if(Error!=LastReverbError)
                    {UE_LOG(LogTemp,Warning,TEXT("IMLogs AcousticReverbDegraded %s listener=(%g,%g,%g)"),UTF8_TO_TCHAR(Error.c_str()),Latest->Listener.origin.x,Latest->Listener.origin.y,Latest->Listener.origin.z);LastReverbError=Error;}
                    FailedUpdates.fetch_add(1);Slot.State.store(IM_AcousticIRState::Free,std::memory_order_release);
                }
                break;
            }
        }
        if(ProfileStart)Bridge->WorkerTiming.Record(ProfileStart);
        const double Remaining = UpdateSeconds - (FPlatformTime::Seconds() - Started);
        if (Remaining > 0.0) { FPlatformProcess::SleepNoStats(static_cast<float>(Remaining)); }
        IMProbe.WaitEndSeconds = FPlatformTime::Seconds();
        Bridge->TraceWorker(IMProbe);
    }
    if(Reverb)Reverb->Stopped.store(true,std::memory_order_release);
    return 0; // Simulator dies here; detached source leases survive in the shared IR pool.
}
