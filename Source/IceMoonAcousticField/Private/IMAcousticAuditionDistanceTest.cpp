#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
// Audition-map distance/attenuation measurement (evidence, not a new gate).
//
// Why this exists: after listening in IM_V2Audition the user reported that
// outside the room nothing seems to attenuate, and asked whether the distance
// from the probe samples (探针/采样点) is the cause. The pipeline is supposed to
// attenuate three ways: (1) direct = SDK inverse distance + air absorption,
// applied once by the DirectEffect from Frame.Direct.flags; (2) path = SDK path
// EQ/SH through openings; (3) the reverb send uses its own broad room-send
// distance curve before entering the submix. Before changing any parameter we measure the actual
// per-position gains on the audition map: three lines (+X toward the player
// start, +Y, -X) at 1/2/4/8/16 m, which cross the bake bounds so "inside vs
// outside the baked field" is covered with the same instrument.
//
// This shell only guarantees the measurement is valid (every planned point got
// accepted blocks with a converged listener snapshot); the numeric curves are
// written to evidence for analysis. It changes no product behavior and is not
// part of the W0-W3 acceptance set.
// A' addendum (2026-09-13): outside points gate on the degraded window itself:
// every captured stale/missing block must carry the live listener-emitter
// callback distance (within 2cm of the geometry) and scale the dry input by
// exactly g^2, g=1/max(d_m,1) - the evidence for Astra decision A'.
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "IMAcousticSpatialization.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Editor/EditorPerformanceSettings.h"
// bAllowBackgroundAudio twin (CrossFloor/Room/W1/Lifecycle/W3): an unfocused
// unattended editor otherwise zero-feeds a playing+active voice.
#include "Settings/LevelEditorMiscSettings.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
constexpr const TCHAR* IMAudMap=TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
constexpr double IMAudConvergeTimeout=3.0;  // per point: wait for the device-side listener
// A' (2026-09-13): the reference loop is 8s (bursts 0-3s, gap 3-4.5s, tones
// 4.5-7s, gap 7-8s), so a window must span at least one loop to be guaranteed
// real audio; collection exits early once enough audio blocks are captured.
constexpr double IMAudCollectTimeout=8.0;   // per point: window cap (one full loop)
constexpr int32 IMAudBlocksFloor=10;        // ~0.21s of blocks at 48kHz/1024
constexpr int32 IMAudAudioBlocksFloor=10;   // blocks with nonzero input required
constexpr int32 IMAudMaxBlockRows=3400;     // per-block evidence rows (17 x <=200)
constexpr int32 IMAudMinAccepted=4;         // below this the point is evidence-invalid
// A' evidence (2026-09-13): the outside-point collection starts after this
// settle window (250ms parameter lease + GT publish stop + margin) so every
// captured block belongs to the new listener position, not to the transition.
constexpr double IMAudOutsideSettleSeconds=0.45;
constexpr int32 IMAudMaxDistOther=2;        // allowed non-matching distances in the window
float IMAudOriginalBackgroundVolume=1;
bool IMAudOriginalBackgroundAudio=false;

FString IMAudEvidence()
{
    static const FString Path=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("AcousticV2/AuditionDistance"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    return Path;
}

// One measured listener position. Energies are means over the accepted blocks
// that consumed the converged snapshot; reverb counters are absolute reads at
// collection time (analysis uses per-point deltas).
struct IM_AudSample
{
    FString Label;
    double DistanceM=0.0;
    double TargetUE[3]={0,0,0};
    bool bOutside=false;      // target outside the bake volume's own bounds
    bool bDegradedWindow=false; // no fresh snapshot: wall-interior/bounds fail-closed path
    bool Converged=false;
    int32 Accepted=0,DirectValid=0,PathValid=0;
    int32 StaleBlocks=0,MissingBlocks=0,FallbackBlocks=0;
    double FallbackInputE=0.0,FallbackOutputE=0.0; // degraded (stale/missing) output
    uint64 RejectedDelta=0;
    double DistGain=0.0,Occlusion=0.0,AgeMs=0.0;
    double InputE=0.0,DirectE=0.0,PathE=0.0,OutputE=0.0;
    // A' evidence: degraded (stale/missing) blocks must carry the live
    // listener-emitter callback distance (0 = unavailable) and scale the dry
    // input by exactly g^2, g=1/max(d_m,1). DegDistOther counts captured blocks
    // whose distance matches no expected geometry (transition/listener lag).
    int32 DegDistBlocks=0,DegDistOther=0,DegDistMissing=0,DegRatioMismatch=0;
    int32 AudioBlocks=0;     // window blocks (any verdict) with nonzero input
    int32 DegAudioBlocks=0;  // stale/missing blocks with nonzero input (Astra's count)
    double DegDistM=0.0,DegGainAvg=0.0,DegRatio=0.0,DegRatioPred=0.0;
    int32 AccDistBlocks=0,AccDistOther=0,AccDistMissing=0;
    uint64 RevProcessed=0,RevNotFresh=0,RevRejected=0,RevNonzero=0;
};

class IM_AcousticAuditionDistanceCommand final : public IAutomationLatentCommand
{
public:
    IM_AcousticAuditionDistanceCommand(FAutomationTestBase* InTest,int32 InStage=0)
        :Test(InTest),Stage(InStage){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();
        if(Stage==0)
        {
            // AutomationLoadMap queues PIE startup; yield this command completely
            // and resume behind those queued commands (waiting here deadlocks).
            // This stage runs before any PIE world exists.
            FString Error;
            GUnrealEd->AutomationLoadMap(IMAudMap,false,&Error);
            Stage=9;
            if(!Error.IsEmpty())return Finish(false,Error);
            ADD_LATENT_AUTOMATION_COMMAND(IM_AcousticAuditionDistanceCommand(Test,1));
            return true;
        }
        UWorld* PIE=nullptr;
        for(const auto& Context:GEngine->GetWorldContexts())if(Context.WorldType==EWorldType::PIE){PIE=Context.World();break;}
        if(!PIE)return Finish(false,TEXT("PIE ended before the distance measurement finished."));
        if(Stage==1)
        {
            for(TActorIterator<AIMAcousticBakeVolume> It(PIE);It;++It)Volume=*It;
            if(!Volume.IsValid())return Finish(false,TEXT("Audition map has no AIMAcousticBakeVolume."));
            if(!Volume->BakedField)return Finish(false,TEXT("Audition bake volume has no bound bake asset."));
            for(TActorIterator<AActor> It(PIE);It;++It)
            {
                TArray<UIMAcousticSourceComponent*> Markers;It->GetComponents(Markers);
                if(Markers.Num()==1&&Markers[0]&&Markers[0]->AudioComponent)
                {Source=Markers[0]->AudioComponent;SourcePos=It->GetActorLocation();break;}
            }
            if(!Source.IsValid())return Finish(false,TEXT("Audition map has no marked acoustic source."));
            Listener=PIE->GetFirstPlayerController();
            if(!Listener.IsValid())return false;
            Bridge=IM_AcousticTestSupport::FindBridge(PIE);
            if(!Bridge.IsValid())return false;
            if(FAudioDevice* AudioDevice=PIE->GetAudioDeviceRaw())
                MetaContext=IM_FindAcousticMetaSoundContext(AudioDevice->DeviceID);
            if(!MetaContext.IsValid()||MetaContext->CapturedSourceBlocks.Num()==0)
                return Finish(false,TEXT("Audition map did not expose graph-native MetaSound source capture."));
            if(!Source->IsPlaying())return false;
            if(Bridge->RenderedBlocks.load(std::memory_order_relaxed)==0)
            {
                if(Now-Started>30.0)
                    return Finish(false,FString::Printf(TEXT("V2 never rendered on the audition map; status=%s"),*Volume->Status));
                return false;
            }
            // Measurement needs the full mask and the live V2 path; the user's
            // audition mask applies to later sessions only and is restored here.
            RoutesBefore=Bridge->RenderRoutes.load(std::memory_order_relaxed);
            Bridge->RenderRoutes.store(7,std::memory_order_relaxed);
            if(!Bridge->Enabled.load(std::memory_order_relaxed))Bridge->Enabled.store(true,std::memory_order_relaxed);
            BuildTargets();
            StartBlocks=GraphSourceBlocks();
            Stage=2;
            ApplyPoint(Now);
            return false;
        }
        // Stage 2: sequential per-point convergence + collection.
        if(WaitingSettle)
        {
            if(Now>=SettleDeadline)
            {
                WaitingSettle=false;
                ConvergeBlocks=GraphSourceBlocks();
                RejectedBefore=Bridge->RejectedBlocks.load(std::memory_order_relaxed);
                CollectDeadline=Now+IMAudCollectTimeout;
            }
            return false;
        }
        if(WaitingSnapshot)
        {
            if(FindTargetSnapshot()||Now>ConvergeDeadline)
            {
                const bool bFound=FindTargetSnapshot();
                Samples[PointIndex].Converged=bFound;
                Samples[PointIndex].bDegradedWindow=!bFound;
                ConvergeBlocks=GraphSourceBlocks();
                RejectedBefore=Bridge->RejectedBlocks.load(std::memory_order_relaxed);
                CollectDeadline=Now+IMAudCollectTimeout;
                WaitingSnapshot=false;
            }
            else Diag(Now);
            return false;
        }
        const uint64 Pushes=GraphSourceBlocks();
        // A' (2026-09-13): a window only closes early when it has both enough
        // blocks and real audio; otherwise it runs to the 8s cap (one loop).
        if(Now<CollectDeadline
            &&(Pushes<ConvergeBlocks+IMAudBlocksFloor||CountWindowAudioBlocks(Pushes)<IMAudAudioBlocksFloor))return false;
        Collect(Pushes);
        ++PointIndex;
        if(PointIndex>=Targets.Num())return Finish(AllPointsValid(),FinishMessage());
        ApplyPoint(Now);
        return false;
    }
private:
    void BuildTargets()
    {
        // +Y is denser: that line is the one that leaves the room through a wall
        // and later the bake bounds, so the occlusion and the degraded window are
        // visible with enough resolution to explain the user's listening report.
        const double DistsX[]={1.0,2.0,4.0,8.0,16.0};
        const double DistsY[]={1.0,2.0,3.0,4.0,6.0,8.0,12.0};
        const FVector Dirs[]={FVector(1,0,0),FVector(0,1,0),FVector(-1,0,0)};
        const TCHAR* Names[]={TEXT("+X"),TEXT("+Y"),TEXT("-X")};
        for(int32 D=0;D<3;++D)
        {
            const double* Dists=(D==1)?DistsY:DistsX;
            const int32 Count=(D==1)?int32(UE_ARRAY_COUNT(DistsY)):int32(UE_ARRAY_COUNT(DistsX));
            for(int32 M=0;M<Count;++M)
            {
                FVector T=SourcePos+Dirs[D]*Dists[M]*100.0;T.Z=SourcePos.Z;
                Targets.Add(T);Distances.Add(Dists[M]);
                Labels.Add(FString::Printf(TEXT("%s_%gm"),Names[D],Dists[M]));
            }
        }
        Samples.SetNum(Targets.Num());
        for(int32 I=0;I<Samples.Num();++I){Samples[I].Label=Labels[I];Samples[I].DistanceM=Distances[I];
            Samples[I].TargetUE[0]=Targets[I].X;Samples[I].TargetUE[1]=Targets[I].Y;Samples[I].TargetUE[2]=Targets[I].Z;}
    }
    void ApplyPoint(double Now)
    {
        const FVector T=Targets[PointIndex];
        const double Yaw=FMath::RadiansToDegrees(FMath::Atan2(SourcePos.Y-T.Y,SourcePos.X-T.X));
        Listener->SetAudioListenerOverride(nullptr,T,FRotator(0,Yaw,0));
        // Outside the volume's own bounds the plugin intentionally stops
        // publishing snapshots; there is nothing to converge to. Collect the
        // degraded window instead of waiting out the convergence timeout.
        const FBox Region=Volume.IsValid()?Volume->BakeBounds->Bounds.GetBox():FBox(ForceInit);
        Samples[PointIndex].bOutside=Region.IsValid&&!Region.IsInsideOrOn(T);
        if(Samples[PointIndex].bOutside)
        {
            // No snapshot will ever match here (the GT publish gate uses these
            // same bake bounds), so collect the degraded window instead of
            // waiting out the convergence timeout. Settle first: the 250ms
            // parameter lease must expire so the captured blocks are stale at
            // the NEW listener position, not the tail of the previous point.
            Samples[PointIndex].Converged=true; // by construction
            WaitingSettle=true;SettleDeadline=Now+IMAudOutsideSettleSeconds;
            return;
        }
        WaitingSnapshot=true;ConvergeDeadline=Now+IMAudConvergeTimeout;LastDiag=Now;
    }
    bool FindTargetSnapshot()
    {
        if(!Bridge.IsValid())return false;
        const uint64 Pushes=Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
        const uint64 Limit=Pushes<IM_AcousticDeviceBridge::ProbeSnapshotCapacity?Pushes:IM_AcousticDeviceBridge::ProbeSnapshotCapacity;
        for(uint64 I=Limit;I>0;--I)
        {
            const uint64 Idx=I-1;
            if(Bridge->SnapshotDone[Idx].load(std::memory_order_acquire)!=Idx+1)continue;
            const IM_AcousticSnapshotProbe& S=Bridge->SnapshotProbes[Idx];
            const FVector T=Targets[PointIndex];
            if(FMath::Abs(double(S.ListenerUEX)-T.X)<1.0&&FMath::Abs(double(S.ListenerUEY)-T.Y)<1.0&&FMath::Abs(double(S.ListenerUEZ)-T.Z)<1.0)
            {TargetSDK[0]=S.ListenerSDKX;TargetSDK[1]=S.ListenerSDKY;TargetSDK[2]=S.ListenerSDKZ;return true;}
            return false;
        }
        return false;
    }
    uint64 GraphSourceBlocks() const
    {
        return MetaContext.IsValid()
            ? MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire) : 0;
    }
    void Collect(uint64 Pushes)
    {
        if(MetaContext.IsValid()){CollectGraph(Pushes);return;}
        IM_AudSample& Out=Samples[PointIndex];
        const uint64 Limit=Pushes<IM_AcousticDeviceBridge::ProbeBlockCapacity?Pushes:IM_AcousticDeviceBridge::ProbeBlockCapacity;
        const double GeomCm=Distances[PointIndex]*100.0;
        double SumDistGain=0,SumOcc=0,SumAge=0,SumIn=0,SumDir=0,SumPath=0,SumOut=0;
        double SumDegIn=0,SumDegOut=0;
        double DegIn=0,DegOut=0,DegPred=0;
        int32 BlockRowSeq=0;
        for(uint64 I=ConvergeBlocks;I<Limit;++I)
        {
            if(Bridge->BlockDone[I].load(std::memory_order_acquire)!=I+1)continue;
            const IM_AcousticBlockProbe& E=Bridge->BlockProbes[I];
            if(BlockRows.Num()<IMAudMaxBlockRows)
                BlockRows.Add(FString::Printf(TEXT("%d,%d,%d,%.6f,%.1f,%.4f,%.6f,%.4f"),
                    PointIndex,BlockRowSeq++,int32(E.Reject),E.InputEnergy,double(E.CallbackDistanceCm),
                    double(E.DegradedGain),E.DistanceGain,E.Occlusion));
            if(E.InputEnergy>1e-9)++Out.AudioBlocks;
            if(E.Reject!=IM_AcousticProbeReject::Accepted)
            {
                // Degraded window (user-relevant): stale/missing blocks keep this
                // block's live distance factor (A'); bypass / bad-shape / an
                // unavailable distance keep the plain constant-power dry path.
                if(E.Reject==IM_AcousticProbeReject::StaleResult)++Out.StaleBlocks;
                else if(E.Reject==IM_AcousticProbeReject::MissingResult)++Out.MissingBlocks;
                else ++Out.FallbackBlocks;
                SumDegIn+=E.InputEnergy>0?E.InputEnergy:0.0;
                SumDegOut+=E.OutputEnergy>0?E.OutputEnergy:0.0;
                if(E.Reject==IM_AcousticProbeReject::StaleResult||E.Reject==IM_AcousticProbeReject::MissingResult)
                {
                    if(E.InputEnergy>1e-9)++Out.DegAudioBlocks;
                    if(E.CallbackDistanceCm<=0.0f)++Out.DegDistMissing;
                    else if(FMath::Abs(double(E.CallbackDistanceCm)-GeomCm)<=2.0)
                    {
                        ++Out.DegDistBlocks;
                        Out.DegDistM+=double(E.CallbackDistanceCm);
                        Out.DegGainAvg+=E.DegradedGain;
                    }
                    else ++Out.DegDistOther;
                    // Predicted Eout/Ein: g^2 with the applied distance factor,
                    // 0.5 when the distance was unavailable (constant-power mono
                    // to stereo keeps exactly half the mono sum-of-squares).
                    const double Pred=E.DegradedGain>0.0f
                        ?double(E.DegradedGain)*double(E.DegradedGain):0.5;
                    if(E.InputEnergy>1e-9&&E.OutputEnergy>=0.0)
                    {
                        const double Ratio=E.OutputEnergy/E.InputEnergy;
                        if(FMath::Abs(Ratio-Pred)>0.01*Pred)++Out.DegRatioMismatch;
                        DegIn+=E.InputEnergy;DegOut+=E.OutputEnergy;DegPred+=E.InputEnergy*Pred;
                    }
                }
                continue;
            }
            if(FMath::Abs(double(E.ListenerX)-double(TargetSDK[0]))>1e-3
                ||FMath::Abs(double(E.ListenerY)-double(TargetSDK[1]))>1e-3
                ||FMath::Abs(double(E.ListenerZ)-double(TargetSDK[2]))>1e-3)continue;
            ++Out.Accepted;
            if(E.CallbackDistanceCm<=0.0f)++Out.AccDistMissing;
            else if(FMath::Abs(double(E.CallbackDistanceCm)-GeomCm)<=2.0)++Out.AccDistBlocks;
            else ++Out.AccDistOther;
            Out.DirectValid+=E.DirectValid?1:0;
            Out.PathValid+=E.PathValid?1:0;
            SumDistGain+=E.DistanceGain;SumOcc+=E.Occlusion;SumAge+=E.AgeMs;
            SumIn+=E.InputEnergy;SumDir+=E.DirectEnergy;SumPath+=E.PathEnergy;SumOut+=E.OutputEnergy;
        }
        if(Out.DegDistBlocks>0){Out.DegDistM/=Out.DegDistBlocks;Out.DegGainAvg/=Out.DegDistBlocks;}
        if(DegIn>0){Out.DegRatio=DegOut/DegIn;Out.DegRatioPred=DegPred/DegIn;}
        const int32 Degraded=Out.StaleBlocks+Out.MissingBlocks+Out.FallbackBlocks;
        if(Degraded>0){Out.FallbackInputE=SumDegIn/Degraded;Out.FallbackOutputE=SumDegOut/Degraded;}
        if(Out.Accepted>0)
        {
            const double N=Out.Accepted;
            Out.DistGain=SumDistGain/N;Out.Occlusion=SumOcc/N;Out.AgeMs=SumAge/N;
            Out.InputE=SumIn/N;Out.DirectE=SumDir/N;Out.PathE=SumPath/N;Out.OutputE=SumOut/N;
        }
        Out.RejectedDelta=Bridge->RejectedBlocks.load(std::memory_order_relaxed)-RejectedBefore;
        Out.RevProcessed=Bridge->ReverbProcessedBlocks.load(std::memory_order_relaxed);
        Out.RevNotFresh=Bridge->ReverbNotFreshBlocks.load(std::memory_order_relaxed);
        Out.RevRejected=Bridge->ReverbRejectedBlocks.load(std::memory_order_relaxed);
        Out.RevNonzero=Bridge->ReverbNonzeroBlocks.load(std::memory_order_relaxed);
    }
    void CollectGraph(uint64 Pushes)
    {
        IM_AudSample& Out=Samples[PointIndex];
        const uint32 Capacity=uint32(MetaContext->CapturedSourceBlocks.Num());
        const uint64 Limit=Pushes<Capacity?Pushes:Capacity;
        const double GeomCm=Distances[PointIndex]*100.0;
        double SumDistGain=0,SumOcc=0,SumAge=0,SumIn=0,SumOut=0;
        double SumDegIn=0,SumDegOut=0;
        double DegIn=0,DegOut=0,DegPred=0;
        int32 BlockRowSeq=0;
        for(uint64 I=ConvergeBlocks;I<Limit;++I)
        {
            const IM_AcousticBlockProbe& E=MetaContext->CapturedSourceBlocks[int32(I)];
            if(BlockRows.Num()<IMAudMaxBlockRows)
                BlockRows.Add(FString::Printf(TEXT("%d,%d,%d,%.6f,%.1f,%.4f,%.6f,%.4f"),
                    PointIndex,BlockRowSeq++,int32(E.Reject),E.InputEnergy,double(E.CallbackDistanceCm),
                    double(E.DegradedGain),E.DistanceGain,E.Occlusion));
            if(E.InputEnergy>1e-9)++Out.AudioBlocks;
            if(E.Reject!=IM_AcousticProbeReject::Accepted)
            {
                if(E.Reject==IM_AcousticProbeReject::StaleResult)++Out.StaleBlocks;
                else if(E.Reject==IM_AcousticProbeReject::MissingResult)++Out.MissingBlocks;
                else ++Out.FallbackBlocks;
                SumDegIn+=E.InputEnergy>0?E.InputEnergy:0.0;
                SumDegOut+=E.OutputEnergy>0?E.OutputEnergy:0.0;
                if(E.Reject==IM_AcousticProbeReject::StaleResult||E.Reject==IM_AcousticProbeReject::MissingResult)
                {
                    if(E.InputEnergy>1e-9)++Out.DegAudioBlocks;
                    if(E.CallbackDistanceCm<=0.0f)++Out.DegDistMissing;
                    else if(FMath::Abs(double(E.CallbackDistanceCm)-GeomCm)<=2.0)
                    {
                        ++Out.DegDistBlocks;Out.DegDistM+=double(E.CallbackDistanceCm);Out.DegGainAvg+=E.DegradedGain;
                    }
                    else ++Out.DegDistOther;
                    const double Pred=E.DegradedGain>0.0f?double(E.DegradedGain)*double(E.DegradedGain):0.5;
                    if(E.InputEnergy>1e-9&&E.OutputEnergy>=0.0)
                    {
                        const double Ratio=E.OutputEnergy/E.InputEnergy;
                        if(FMath::Abs(Ratio-Pred)>0.01*Pred)++Out.DegRatioMismatch;
                        DegIn+=E.InputEnergy;DegOut+=E.OutputEnergy;DegPred+=E.InputEnergy*Pred;
                    }
                }
                continue;
            }
            if(FMath::Abs(double(E.ListenerX)-double(TargetSDK[0]))>1e-3
                ||FMath::Abs(double(E.ListenerY)-double(TargetSDK[1]))>1e-3
                ||FMath::Abs(double(E.ListenerZ)-double(TargetSDK[2]))>1e-3)continue;
            ++Out.Accepted;
            if(E.CallbackDistanceCm<=0.0f)++Out.AccDistMissing;
            else if(FMath::Abs(double(E.CallbackDistanceCm)-GeomCm)<=2.0)++Out.AccDistBlocks;
            else ++Out.AccDistOther;
            Out.DirectValid+=E.DirectValid?1:0;Out.PathValid+=E.PathValid?1:0;
            SumDistGain+=E.DistanceGain;SumOcc+=E.Occlusion;SumAge+=E.AgeMs;
            SumIn+=E.InputEnergy;SumOut+=E.OutputEnergy;
        }
        if(Out.DegDistBlocks>0){Out.DegDistM/=Out.DegDistBlocks;Out.DegGainAvg/=Out.DegDistBlocks;}
        if(DegIn>0){Out.DegRatio=DegOut/DegIn;Out.DegRatioPred=DegPred/DegIn;}
        const int32 Degraded=Out.StaleBlocks+Out.MissingBlocks+Out.FallbackBlocks;
        if(Degraded>0){Out.FallbackInputE=SumDegIn/Degraded;Out.FallbackOutputE=SumDegOut/Degraded;}
        if(Out.Accepted>0)
        {
            const double N=Out.Accepted;
            Out.DistGain=SumDistGain/N;Out.Occlusion=SumOcc/N;Out.AgeMs=SumAge/N;
            Out.InputE=SumIn/N;Out.OutputE=SumOut/N;
        }
        Out.RejectedDelta=MetaContext->Device->RejectedBlocks.load(std::memory_order_relaxed)-RejectedBefore;
        Out.RevProcessed=MetaContext->EnvironmentBlocks.load(std::memory_order_relaxed);
        Out.RevNotFresh=MetaContext->NoIRBlocks.load(std::memory_order_relaxed);
        Out.RevRejected=MetaContext->InvalidBlocks.load(std::memory_order_relaxed);
        Out.RevNonzero=MetaContext->Device->ReverbNonzeroBlocks.load(std::memory_order_relaxed);
    }
    int32 CountWindowAudioBlocks(uint64 Pushes) const
    {
        if(MetaContext.IsValid())
        {
            const uint64 Limit=Pushes<uint64(MetaContext->CapturedSourceBlocks.Num())?Pushes:uint64(MetaContext->CapturedSourceBlocks.Num());
            int32 Count=0;
            for(uint64 I=ConvergeBlocks;I<Limit;++I)
                if(MetaContext->CapturedSourceBlocks[int32(I)].InputEnergy>1e-9)++Count;
            return Count;
        }
        const uint64 Limit=Pushes<IM_AcousticDeviceBridge::ProbeBlockCapacity?Pushes:IM_AcousticDeviceBridge::ProbeBlockCapacity;
        int32 Count=0;
        for(uint64 I=ConvergeBlocks;I<Limit;++I)
        {
            if(Bridge->BlockDone[I].load(std::memory_order_acquire)!=I+1)continue;
            if(Bridge->BlockProbes[I].InputEnergy>1e-9)++Count;
        }
        return Count;
    }
    static bool PointValid(const IM_AudSample& S)
    {
        const int32 Degraded=S.StaleBlocks+S.MissingBlocks+S.FallbackBlocks;
        if(S.bOutside||S.bDegradedWindow)
        {
            // A' gate: the captured stale window carries the live callback
            // distance matching the geometry, real audio, and the exact g^2
            // scaling on every audio block.
            if(Degraded<IMAudMinAccepted)return false;
            if(S.DegDistBlocks<IMAudMinAccepted)return false;
            if(S.DegDistMissing>0||S.DegDistOther>IMAudMaxDistOther)return false;
            if(S.DegRatioMismatch>0)return false;
            if(S.DegAudioBlocks<IMAudAudioBlocksFloor)return false;
            return true;
        }
        return S.Converged&&S.Accepted>=IMAudMinAccepted&&S.AudioBlocks>=IMAudAudioBlocksFloor;
    }
    bool AllPointsValid() const
    {
        for(const IM_AudSample& S:Samples)
        {
            // Inside: accepted blocks on the converged snapshot. Outside: the
            // plugin stops publishing by design, so a valid point is a captured
            // degraded window (stale/missing blocks) meeting the A' gate.
            if(!PointValid(S))return false;
        }
        return Samples.Num()>0;
    }
    FString FinishMessage() const
    {
        int32 Bad=0;FString First;
        for(const IM_AudSample& S:Samples)
        {
            if(PointValid(S))continue;
            ++Bad;
            if(First.IsEmpty())
            {
                const int32 Degraded=S.StaleBlocks+S.MissingBlocks+S.FallbackBlocks;
                First=FString::Printf(TEXT("%s(converged=%d,accepted=%d,degraded=%d,dist_ok=%d,dist_missing=%d,dist_other=%d,ratio_bad=%d,audio=%d,deg_audio=%d)"),
                    *S.Label,int(S.Converged),S.Accepted,Degraded,S.DegDistBlocks,S.DegDistMissing,S.DegDistOther,S.DegRatioMismatch,S.AudioBlocks,S.DegAudioBlocks);
            }
        }
        return Bad==0
            ?FString::Printf(TEXT("Audition distance measurement complete: %d/%d points valid (inside=accepted blocks on the converged snapshot; outside=stale window with live callback distance, real audio and Eout/Ein==g^2)."),Samples.Num(),Samples.Num())
            :FString::Printf(TEXT("Audition distance measurement incomplete: %d/%d points invalid, first=%s."),Bad,Samples.Num(),*First);
    }
    void Diag(double Now)
    {
        if(Now-LastDiag<5.0)return;
        LastDiag=Now;
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticAuditionDistance waiting point=%d label=%s snaps=%llu blocks=%llu"),
            PointIndex,*Labels[PointIndex],
            Bridge.IsValid()?Bridge->SnapshotProbePushes.load(std::memory_order_acquire):0ull,
            Bridge.IsValid()?Bridge->BlockProbePushes.load(std::memory_order_acquire):0ull);
    }
    void WriteEvidence()
    {
        const FString Dir=IMAudEvidence();
        IFileManager::Get().MakeDirectory(*Dir,true);
        FString Csv=TEXT("index,label,distance_m,outside,converged,accepted,stale,missing,fallback,rejected_delta,dist_gain,occlusion,age_ms,input_e,direct_e,path_e,output_e,fallback_input_e,fallback_output_e,reverb_processed,reverb_not_fresh,reverb_rejected,reverb_nonzero,target_x,target_y,target_z,deg_dist_m,deg_gain,deg_ratio,deg_ratio_pred,deg_dist_blocks,deg_dist_other,deg_dist_missing,deg_ratio_mismatch,acc_dist_blocks,acc_dist_other,acc_dist_missing,audio_blocks,deg_audio_blocks\n");
        for(int32 I=0;I<Samples.Num();++I)
        {
            const IM_AudSample& S=Samples[I];
            Csv+=FString::Printf(TEXT("%d,%s,%.3f,%d,%d,%d,%d,%d,%d,%llu,%.6f,%.6f,%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.4f,%.4f,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"),
                I,*S.Label,S.DistanceM,int(S.bOutside),int(S.Converged),S.Accepted,S.StaleBlocks,S.MissingBlocks,S.FallbackBlocks,S.RejectedDelta,
                S.DistGain,S.Occlusion,S.AgeMs,S.InputE,S.DirectE,S.PathE,S.OutputE,S.FallbackInputE,S.FallbackOutputE,
                S.RevProcessed,S.RevNotFresh,S.RevRejected,S.RevNonzero,
                S.TargetUE[0],S.TargetUE[1],S.TargetUE[2],
                S.DegDistM,S.DegGainAvg,S.DegRatio,S.DegRatioPred,S.DegDistBlocks,S.DegDistOther,S.DegDistMissing,S.DegRatioMismatch,
                S.AccDistBlocks,S.AccDistOther,S.AccDistMissing,S.AudioBlocks,S.DegAudioBlocks);
        }
        FFileHelper::SaveStringToFile(Csv,*FPaths::Combine(Dir,TEXT("distance.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FString Json=FString::Printf(TEXT("{\"scope\":\"audition-map distance/attenuation measurement; three lines x 1/2/4/8/16m\",\"telemetry\":\"MetaSound.CapturedSourceBlocks\",\"source_cm\":[%.2f,%.2f,%.2f],\"valid\":%s,\"points\":["),
            SourcePos.X,SourcePos.Y,SourcePos.Z,AllPointsValid()?TEXT("true"):TEXT("false"));
        for(int32 I=0;I<Samples.Num();++I)
        {
            const IM_AudSample& S=Samples[I];
            Json+=FString::Printf(TEXT("%s{\"index\":%d,\"label\":\"%s\",\"distance_m\":%.3f,\"outside\":%s,\"degraded_window\":%s,\"converged\":%s,\"accepted\":%d,\"rejected_delta\":%llu,\"dist_gain\":%.6f,\"occlusion\":%.6f,\"age_ms\":%.3f,\"input_e\":%.6f,\"direct_e\":%.6f,\"path_e\":%.6f,\"output_e\":%.6f,\"direct_valid\":%d,\"path_valid\":%d,\"reverb_processed\":%llu,\"reverb_not_fresh\":%llu,\"reverb_rejected\":%llu,\"deg_dist_m\":%.4f,\"deg_gain\":%.4f,\"deg_ratio\":%.6f,\"deg_ratio_pred\":%.6f,\"deg_ratio_mismatch\":%d,\"deg_dist_blocks\":%d,\"deg_dist_other\":%d,\"deg_dist_missing\":%d,\"acc_dist_blocks\":%d,\"acc_dist_other\":%d,\"acc_dist_missing\":%d,\"audio_blocks\":%d,\"deg_audio_blocks\":%d}"),
                I==0?TEXT(""):TEXT(","),I,*S.Label,S.DistanceM,S.bOutside?TEXT("true"):TEXT("false"),S.bDegradedWindow?TEXT("true"):TEXT("false"),S.Converged?TEXT("true"):TEXT("false"),S.Accepted,S.RejectedDelta,
                S.DistGain,S.Occlusion,S.AgeMs,S.InputE,S.DirectE,S.PathE,S.OutputE,S.DirectValid,S.PathValid,
                S.RevProcessed,S.RevNotFresh,S.RevRejected,
                S.DegDistM,S.DegGainAvg,S.DegRatio,S.DegRatioPred,S.DegRatioMismatch,S.DegDistBlocks,S.DegDistOther,
                S.DegDistMissing,S.AccDistBlocks,S.AccDistOther,S.AccDistMissing,S.AudioBlocks,S.DegAudioBlocks);
        }
        Json+=TEXT("]}");
        FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Dir,TEXT("distance.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        // Per-block window evidence: reject code, input energy, callback distance,
        // degraded gain, accepted-path distance gain and occlusion for each block.
        const FString Blocks=TEXT("# point,seq,reject,input_e,callback_cm,degraded_gain,dist_gain,occlusion\n")
            +FString::Join(BlockRows,TEXT("\n"))+TEXT("\n");
        FFileHelper::SaveStringToFile(Blocks,*FPaths::Combine(Dir,TEXT("blocks.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticAuditionDistance evidence=%s"),*Dir);
    }
    bool Finish(bool Pass,const FString& Message)
    {
        WriteEvidence();
        IM_EnableAcousticMetaSoundCaptureForTest(false);
        if(Bridge.IsValid())Bridge->RenderRoutes.store(RoutesBefore,std::memory_order_relaxed);
        if(!Pass)Test->AddError(Message);else Test->AddInfo(Message);
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticAuditionDistance %s %s"),Pass?TEXT("PASS"):TEXT("FAIL"),*Message);
        UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
        UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticAuditionDistance %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
        FApp::SetUnfocusedVolumeMultiplier(IMAudOriginalBackgroundVolume);
        GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=IMAudOriginalBackgroundAudio;
        if(GUnrealEd)GUnrealEd->RequestEndPlayMap();
        return true;
    }
    FAutomationTestBase* Test;
    int32 Stage=0,PointIndex=0;
    double Started=FPlatformTime::Seconds(),LastDiag=0,ConvergeDeadline=0,CollectDeadline=0,SettleDeadline=0;
    bool WaitingSnapshot=false,WaitingSettle=false;
    uint64 StartBlocks=0,ConvergeBlocks=0,RejectedBefore=0;
    uint32 RoutesBefore=7;
    TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
    TWeakObjectPtr<UAudioComponent> Source;
    TWeakObjectPtr<APlayerController> Listener;
    TSharedPtr<IM_AcousticDeviceBridge,ESPMode::ThreadSafe> Bridge;
    IM_AcousticMetaSoundContextPtr MetaContext;
    FVector SourcePos=FVector::ZeroVector;
    TArray<FVector> Targets;
    TArray<double> Distances;
    TArray<FString> Labels;
    TArray<IM_AudSample> Samples;
    TArray<FString> BlockRows;
    float TargetSDK[3]={0,0,0};
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticAuditionDistanceTest,"IceMoon.AcousticField.W3.AuditionDistance",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::ProductFilter)
bool FIMAcousticAuditionDistanceTest::RunTest(const FString&)
{
    IM_EnableAcousticMetaSoundCaptureForTest(true);
    IMAudOriginalBackgroundVolume=FApp::GetUnfocusedVolumeMultiplier();
    FApp::SetUnfocusedVolumeMultiplier(1); // Deterministic audio in this owned unattended Editor only.
    IMAudOriginalBackgroundAudio=GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
    GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=true;
    GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground=false;
    ADD_LATENT_AUTOMATION_COMMAND(IM_AcousticAuditionDistanceCommand(this,0));
    return true;
}
#endif
