#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "IMAcousticSpatialization.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Audio.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Sound/SoundWaveProcedural.h"

namespace IMAcousticLifecycleTestPrivate
{
struct FIMLifecycleState
{
    FAutomationTestBase* Test=nullptr;double Started=0;float BackgroundVolume=1;bool bAllowBackgroundAudioOrig=false;int32 Cycle=0;
    FString Directory;uint64 PreviousEpoch=0;
    TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> PreviousBridge;
};
class FIMAcousticLifecycleCommand final:public IAutomationLatentCommand
{
public:
    explicit FIMAcousticLifecycleCommand(TSharedRef<FIMLifecycleState> In):State(In){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(Now-State->Started>120)return Finish(false,FString::Printf(TEXT("Lifecycle test timed out at stage %d."),Stage));
        if(Now-LastDiagnostic>10&&Bridge&&Volume.IsValid())
        {
            LastDiagnostic=Now;
            UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycleState stage=%d rendered=%llu path=%llu reverb=%llu rejected=%llu reverb_rejected=%llu enabled=%d routes=%u status=%s"),Stage,Bridge->RenderedBlocks.load(),Bridge->PathNonzeroBlocks.load(),Bridge->ReverbNonzeroBlocks.load(),Bridge->RejectedBlocks.load(),Bridge->ReverbRejectedBlocks.load(),int(Bridge->Enabled.load()),Bridge->RenderRoutes.load(),*Volume->Status);
            UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycleReverb calls=%llu dry=%llu raw=%llu dropped=%llu wet_gain=%g push=%llu/nodirect=%llu/zerogain=%llu/inputnz=%llu notfresh=%llu rinputnz=%llu"),Bridge->ReverbProcessedBlocks.load(),Bridge->ReverbDryBlocks.load(),Bridge->ReverbRawNonzeroBlocks.load(),Bridge->DryDroppedBlocks.load(),Volume->ReverbWetGain,Bridge->PushDryCalls.load(),Bridge->PushDryInvalidDirect.load(),Bridge->PushDryZeroGain.load(),Bridge->PushDryInputNonzero.load(),Bridge->ReverbNotFreshBlocks.load(),Bridge->RenderInputNonzero.load());
            UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycleStages instances=%d dry_nonzero=%llu ambi_nonzero=%llu"),Bridge->ReverbEffectInstances.load(),Bridge->ReverbDryNonzeroBlocks.load(),Bridge->ReverbAmbisonicsNonzeroBlocks.load());
            if(Source.IsValid())
            {if(auto* DiagAudio=Source->FindComponentByClass<UAudioComponent>())
            {auto* DiagWave=Cast<USoundWaveProcedural>(DiagAudio->Sound);
            UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecyclePlayback playing=%d active=%d sound=%s proc=%d queuebytes=%d vol=%g pitch=%g"),int(DiagAudio->IsPlaying()),int(DiagAudio->IsActive()),*GetNameSafe(DiagAudio->Sound),int(DiagWave!=nullptr),DiagWave?DiagWave->GetAvailableAudioByteCount():-1,DiagAudio->VolumeMultiplier,DiagAudio->PitchMultiplier);}
            else UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecyclePlayback nosourceaudio"));}
            if (Listener.IsValid() && Listener->GetWorld()) { if (FAudioDevice* DiagDevice = Listener->GetWorld()->GetAudioDeviceRaw()) { UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticLifecycleDevice dev=%p muted=%d rate=%g primary=%g"), DiagDevice, int(DiagDevice->IsAudioDeviceMuted()), DiagDevice->GetSampleRate(), DiagDevice->GetPrimaryVolume()); } }
        }
        UWorld* World=nullptr;for(const auto& C:GEngine->GetWorldContexts())if(C.WorldType==EWorldType::PIE){World=C.World();break;}
        if(Stage==12)
        {
            if(World)return false;
            if(State->PreviousBridge->WorldGeneration.load()==State->PreviousEpoch)return Finish(false,TEXT("Ended PIE retained its world lease."));
            if(State->Cycle==2)return Finish(true,TEXT("Two PIE cycles: bypass, bounds rejection, source reuse and post-destroy silence passed."));
            FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/Tests/IM_W1Door"),false,&Error);
            if(!Error.IsEmpty())return Finish(false,Error);
            // Yield the current command, allowing the queued PIE startup to run.
            ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticLifecycleCommand(State));return true;
        }
        if(!World)return Stage==0?false:Finish(false,TEXT("PIE ended before lifecycle evidence."));
        if(Stage==0)
        {
            for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It)Volume=*It;
            Listener=World->GetFirstPlayerController();Bridge=IMAcousticTestSupport::FindBridge(World);
            if(!Volume.IsValid()||!Listener.IsValid()||!Bridge||!Bridge->WorldGeneration.load())return false;
            if(State->PreviousEpoch==Bridge->WorldGeneration.load())return Finish(false,TEXT("PIE reused a world generation."));
            if(State->PreviousBridge&&State->PreviousBridge->WorldGeneration.load()==State->PreviousEpoch)return Finish(false,TEXT("Old device still publishes the previous world."));
            Volume->bEnableV2=true;Volume->bDirectRoute=false;Volume->bPathRoute=true;Volume->bReverbRoute=true;
            RestoreListener();SpawnSource(World,233);Bridge=IMAcousticTestSupport::FindBridge(World);
            if(FAudioDevice* AudioDevice=World->GetAudioDeviceRaw())
                MetaContext=IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
            GraphWindowSourceStart=GraphSourceBlocks();
            GraphWindowEnvironmentStart=GraphEnvironmentBlocks();
            PathStart=Bridge?Bridge->PathNonzeroBlocks.load():0;ReverbStart=Bridge?Bridge->ReverbNonzeroBlocks.load():0;InputStart=Bridge?Bridge->PushDryInputNonzero.load():0;Stage=1;Stamp=Now;
        }
        Bridge=IMAcousticTestSupport::FindBridge(World);
        if(!Volume.IsValid()||!Listener.IsValid()||!Bridge)return Finish(false,TEXT("Lifecycle owner lost."));
        if(MetaContext.IsValid()&&MetaContext->Stopped.load(std::memory_order_acquire))
            MetaContext.Reset();
        if(!MetaContext.IsValid())
        {
            if(FAudioDevice* AudioDevice=World->GetAudioDeviceRaw())
                MetaContext=IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
        }
        if(MetaContext.IsValid()&&GraphWindowSourceStart==0)
        {
            GraphWindowSourceStart=GraphSourceBlocks();
            GraphWindowEnvironmentStart=GraphEnvironmentBlocks();
        }
        if(Stage==1)
        {
            // Audibility requires genuine dry input at the plugin, not bottom
            // noise: nonzero path/reverb output only counts alongside nonzero
            // input growth within the same window.
            if(MetaContext.IsValid())
            {
                uint64 GraphPath=0,GraphRejected=0,GraphInput=0,GraphOutput=0;
                GraphSourceWindow(GraphWindowSourceStart,GraphSourceBlocks(),GraphPath,GraphRejected,GraphInput,GraphOutput);
                const uint64 GraphWet=GraphWetWindow(GraphWindowEnvironmentStart,GraphEnvironmentBlocks());
                if(GraphPath<8||GraphWet<8||GraphInput<8)
                {if(Now-Stamp>15)return Finish(false,TEXT("MetaSound path/reverb/dry-input did not become audible within 15 seconds."));return false;}
            }
            else if(Bridge->PathNonzeroBlocks.load()<PathStart+8||Bridge->ReverbNonzeroBlocks.load()<ReverbStart+8||Bridge->PushDryInputNonzero.load()<InputStart+8)
            {if(Now-Stamp>15)return Finish(false,TEXT("Path/reverb/dry-input did not become audible within 15 seconds."));return false;}
            // A second valid volume must fail enrollment, then be harmless to
            // the active owner when destroyed. Copy settings before BeginPlay
            // so its rejection exercises the device lease, not stale metadata.
            const FTransform Transform=Volume->GetActorTransform();
            Competitor=World->SpawnActorDeferred<AIMAcousticBakeVolume>(AIMAcousticBakeVolume::StaticClass(),Transform);
            if(!Competitor.IsValid())return Finish(false,TEXT("Cannot spawn competing acoustic owner."));
            Competitor->BakeBounds->SetBoxExtent(Volume->BakeBounds->GetUnscaledBoxExtent());
            Competitor->Materials=Volume->Materials;Competitor->ProbeSpacingCm=Volume->ProbeSpacingCm;
            Competitor->ProbeHeightCm=Volume->ProbeHeightCm;Competitor->BakedField=Volume->BakedField;
            Competitor->FinishSpawning(Transform);OwnerEpoch=Bridge->ReverbWorldGeneration.load();Stage=13;Stamp=Now;return false;
        }
        if(Stage==13&&Now-Stamp>.25)
        {
            const bool bOwnershipRejected=Competitor.IsValid()
                &&(Competitor->Status.Contains(TEXT("already belongs"),ESearchCase::IgnoreCase)
                   ||Competitor->Status.Contains(TEXT("already owned"),ESearchCase::IgnoreCase)
                   ||Competitor->Status.Contains(TEXT("ownership"),ESearchCase::IgnoreCase));
            UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycleCompetitor status=%s rejected=%d"),*Competitor->Status,int(bOwnershipRejected));
            if(!bOwnershipRejected)
                return Finish(false,TEXT("Competing volume did not exercise device ownership rejection."));
            Competitor->Destroy();Competitor.Reset();
            if(!OwnerEpoch||Bridge->ReverbWorldGeneration.load()!=OwnerEpoch)
                return Finish(false,TEXT("Rejected volume revoked the active reverb owner."));
            ReverbStart=Bridge->ReverbNonzeroBlocks.load();Stage=14;Stamp=Now;return false;
        }
        if(Stage==14&&Now-Stamp>.5)
        {
            if(Bridge->ReverbWorldGeneration.load()!=OwnerEpoch||Bridge->ReverbNonzeroBlocks.load()<=ReverbStart)
                return Finish(false,TEXT("Active owner's reverb stopped after rejected volume destruction."));
            Volume->bDirectRoute=true;
            Volume->bEnableV2=false;Stage=2;Stamp=Now;return false;
        }
        if(Stage==2&&Now-Stamp>.4)
        {
            PathStart=Bridge->PathNonzeroBlocks.load();ReverbStart=Bridge->ReverbNonzeroBlocks.load();BypassStart=Bridge->BypassedBlocks.load();
            GraphWindowSourceStart=GraphSourceBlocks();
            GraphWindowEnvironmentStart=GraphEnvironmentBlocks();
            UAudioMixerBlueprintLibrary::StartRecordingOutput(World,2);Stage=3;Stamp=Now;
        }
        if(Stage==3&&Now-Stamp>1)
        {
            UAudioMixerBlueprintLibrary::StopRecordingOutput(World,EAudioRecordingExportType::WavFile,Name(TEXT("bypass")),State->Directory);
            if(MetaContext.IsValid())
            {
                uint64 GraphPath=0,GraphRejected=0,GraphInput=0,GraphOutput=0;
                GraphSourceWindow(GraphWindowSourceStart,GraphSourceBlocks(),GraphPath,GraphRejected,GraphInput,GraphOutput);
                const uint64 GraphWet=GraphWetWindow(GraphWindowEnvironmentStart,GraphEnvironmentBlocks());
                UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycleBypassGraph path=%llu wet=%llu rejected=%llu input=%llu output=%llu source_window=%u-%u environment_window=%u-%u"),
                    GraphPath,GraphWet,GraphRejected,GraphInput,GraphOutput,GraphWindowSourceStart,GraphSourceBlocks(),GraphWindowEnvironmentStart,GraphEnvironmentBlocks());
                if(GraphPath!=0||GraphWet!=0||GraphInput==0||GraphOutput==0)
                    return Finish(false,TEXT("MetaSound bypass retained indirect output or did not render dry reference."));
            }
            else if(Bridge->PathNonzeroBlocks.load()!=PathStart||Bridge->ReverbNonzeroBlocks.load()!=ReverbStart||Bridge->BypassedBlocks.load()<=BypassStart)
                return Finish(false,TEXT("V2 bypass retained indirect output or did not render dry reference."));
            Volume->bDirectRoute=false;
            Volume->bEnableV2=true;Stage=4;Stamp=Now;
            GraphWindowSourceStart=GraphSourceBlocks();
            GraphWindowEnvironmentStart=GraphEnvironmentBlocks();
        }
        if(Stage==4&&Now-Stamp>.5)
        {
            double Energy=0,Balance=0;if(!ReadRecording(TEXT("bypass"),Energy,Balance))return false;
            if(Energy<=0||FMath::Abs(Balance)>1.e-5)return Finish(false,TEXT("Bypass recording is not symmetric nonzero dry audio."));
            if(MetaContext.IsValid())
            {
                uint64 GraphPath=0,GraphRejected=0,GraphInput=0,GraphOutput=0;
                GraphSourceWindow(GraphWindowSourceStart,GraphSourceBlocks(),GraphPath,GraphRejected,GraphInput,GraphOutput);
                if(GraphPath==0)return false;
            }
            else if(Bridge->PathNonzeroBlocks.load()<=PathStart)return false;
            const FVector Outside=Volume->BakeBounds->Bounds.Origin+FVector(0,Volume->BakeBounds->Bounds.BoxExtent.Y+1000,0);
            Listener->SetAudioListenerOverride(nullptr,Outside,FRotator(0,-90,0));
            GraphWindowSourceStart=GraphSourceBlocks();
            GraphWindowEnvironmentStart=GraphEnvironmentBlocks();
            Stage=5;Stamp=Now;
        }
        if(Stage==5&&Now-Stamp>.6)
        {PathStart=Bridge->PathNonzeroBlocks.load();ReverbStart=Bridge->ReverbNonzeroBlocks.load();RejectedStart=Bridge->RejectedBlocks.load();GraphWindowSourceStart=GraphSourceBlocks();GraphWindowEnvironmentStart=GraphEnvironmentBlocks();Stage=6;Stamp=Now;}
        if(Stage==6&&Now-Stamp>.5)
        {
            if(MetaContext.IsValid())
            {
                uint64 GraphPath=0,GraphRejected=0,GraphInput=0,GraphOutput=0;
                GraphSourceWindow(GraphWindowSourceStart,GraphSourceBlocks(),GraphPath,GraphRejected,GraphInput,GraphOutput);
                const uint64 GraphWet=GraphWetWindow(GraphWindowEnvironmentStart,GraphEnvironmentBlocks());
                if(GraphPath!=0||GraphWet!=0||GraphRejected==0)
                    return Finish(false,TEXT("Out-of-bounds listener retained indirect output or was reported as success."));
            }
            else if(Bridge->PathNonzeroBlocks.load()!=PathStart||Bridge->ReverbNonzeroBlocks.load()!=ReverbStart||Bridge->RejectedBlocks.load()<=RejectedStart)
                return Finish(false,TEXT("Out-of-bounds listener retained indirect output or was reported as success."));
            RestoreListener();GraphWindowSourceStart=GraphSourceBlocks();GraphWindowEnvironmentStart=GraphEnvironmentBlocks();Stage=7;Stamp=Now;
        }
        if(Stage==7&&Now-Stamp>.5)
        {
            bool bRecovered=false;
            if(MetaContext.IsValid())
            {
                uint64 GraphPath=0,GraphRejected=0,GraphInput=0,GraphOutput=0;
                GraphSourceWindow(GraphWindowSourceStart,GraphSourceBlocks(),GraphPath,GraphRejected,GraphInput,GraphOutput);
                bRecovered=GraphPath>0;
            }
            else bRecovered=Bridge->PathNonzeroBlocks.load()>PathStart;
            if(!bRecovered)
            {if(Now-Stamp>2)return Finish(false,TEXT("Valid source failed to recover within 2 seconds after SDK recreation."));return false;}
            Generations.Reset();for(const auto& Voice:Bridge->Voices)Generations.Add(Voice->LiveGeneration.load());
            if(!Source.IsValid())return Finish(false,TEXT("Expected live source missing."));
            PreviousMetaContext=MetaContext;
            Source->Destroy();Source.Reset();Stage=8;Stamp=Now;
        }
        if(Stage==8&&Now-Stamp>2.5)
        {UAudioMixerBlueprintLibrary::StartRecordingOutput(World,2);Stage=9;Stamp=Now;}
        if(Stage==9&&Now-Stamp>1)
        {UAudioMixerBlueprintLibrary::StopRecordingOutput(World,EAudioRecordingExportType::WavFile,Name(TEXT("destroyed")),State->Directory);Stage=10;Stamp=Now;}
        if(Stage==10)
        {
            double Energy=0,Balance=0;if(!ReadRecording(TEXT("destroyed"),Energy,Balance))return false;
            if(Energy!=0)return Finish(false,TEXT("Destroyed source left nonzero audio after the full IR tail."));
            PathStart=Bridge->PathNonzeroBlocks.load();SpawnSource(World,997);
            GraphWindowSourceStart=GraphSourceBlocks();
            GraphWindowEnvironmentStart=GraphEnvironmentBlocks();
            Stage=11;Stamp=Now;
        }
        if(Stage==11&&Now-Stamp>.5)
        {
            if(MetaContext.IsValid())
            {
                uint64 GraphPath=0,GraphRejected=0,GraphInput=0,GraphOutput=0;
                GraphSourceWindow(GraphWindowSourceStart,GraphSourceBlocks(),GraphPath,GraphRejected,GraphInput,GraphOutput);
                if(GraphPath==0)return false;
            }
            else if(Bridge->PathNonzeroBlocks.load()<=PathStart)return false;
            bool Reused=false;
            for(int32 I=0;I<Generations.Num();++I)if(Generations[I]&&Bridge->Voices.IsValidIndex(I)&&Bridge->Voices[I]->LiveGeneration.load()>=Generations[I]+2)Reused=true;
            bool GraphContextRecreated=false;
            if(MetaContext.IsValid())
            {
                GraphContextRecreated=PreviousMetaContext.IsValid()&&MetaContext.Get()!=PreviousMetaContext.Get();
                UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycleSourceRecreate graph_context_recreated=%d source_slot0=%llu"),
                    int(GraphContextRecreated),MetaContext->AudioIds[0].load(std::memory_order_acquire));
                if(!GraphContextRecreated)return Finish(false,TEXT("New graph source did not acquire a fresh MetaSound context after the old context stopped."));
            }
            else if(!Reused)return Finish(false,TEXT("No actual SourceId generation reuse was observed on the legacy route."));
            const FString Data=FString::Printf(TEXT("{\"cycle\":%d,\"world_generation\":%llu,\"source_id_reused\":%s,\"graph_context_recreated\":%s,\"destroyed_pcm16_energy\":0}"),
                State->Cycle,Bridge->WorldGeneration.load(),Reused?TEXT("true"):TEXT("false"),GraphContextRecreated?TEXT("true"):TEXT("false"));
            if(!FFileHelper::SaveStringToFile(Data,*FPaths::Combine(State->Directory,Name(TEXT("receipt"))+TEXT(".json"))))return Finish(false,TEXT("Cannot persist lifecycle receipt."));
            State->PreviousBridge=Bridge;State->PreviousEpoch=Bridge->WorldGeneration.load();++State->Cycle;
            GEditor->RequestEndPlayMap();Stage=12;
        }
        return false;
    }
private:
    uint32 GraphSourceBlocks() const
    {
        return MetaContext.IsValid()?MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire):0;
    }
    uint32 GraphEnvironmentBlocks() const
    {
        return MetaContext.IsValid()?MetaContext->CapturedBlockCount.load(std::memory_order_acquire):0;
    }
    void GraphSourceWindow(uint32 Begin,uint32 End,uint64& Path,uint64& Rejected,uint64& Input,uint64& Output) const
    {
        Path=0;Rejected=0;Input=0;Output=0;
        if(!MetaContext.IsValid())return;
        const uint32 Limit=FMath::Min<uint32>(End,uint32(MetaContext->CapturedSourceBlocks.Num()));
        const uint32 First=FMath::Min<uint32>(Begin,Limit);
        for(uint32 I=First;I<Limit;++I)
        {
            const FIMAcousticBlockProbe& E=MetaContext->CapturedSourceBlocks[int32(I)];
            if(E.InputEnergy>1e-9)++Input;
            if(E.OutputEnergy>1e-9)++Output;
            if(E.Reject!=EIMAcousticProbeReject::Accepted){++Rejected;continue;}
            if((E.Routes&2u)!=0&&E.OutputEnergy>1e-9)++Path;
        }
    }
    uint64 GraphWetWindow(uint32 Begin,uint32 End) const
    {
        if(!MetaContext.IsValid())return 0;
        const uint32 Limit=FMath::Min<uint32>(End,uint32(MetaContext->CapturedBlocks.Num()));
        const uint32 First=FMath::Min<uint32>(Begin,Limit);
        const uint32 Frames=MetaContext->CapturedEnvironmentFrames.load(std::memory_order_acquire);
        uint64 Nonzero=0;
        for(uint32 I=First;I<Limit;++I)
        {
            const FIMAcousticMetaSoundBlock& E=MetaContext->CapturedBlocks[int32(I)];
            const uint32 Start=uint32(E.Frame);
            const uint32 Finish=FMath::Min<uint32>(Start+FIMAcousticMetaSoundContext::Frames,Frames);
            bool bWet=false;
            for(uint32 F=Start;F<Finish;++F)
            {
                if(MetaContext->CapturedWet.IsValidIndex(int32(2*F))
                    && (FMath::Abs(MetaContext->CapturedWet[int32(2*F)])>1e-9f||FMath::Abs(MetaContext->CapturedWet[int32(2*F+1)])>1e-9f))
                {bWet=true;break;}
            }
            if(bWet)++Nonzero;
        }
        return Nonzero;
    }
    FString Name(const TCHAR* Route) const{return FString::Printf(TEXT("IM_%s_cycle%d"),Route,State->Cycle);}
    void RestoreListener(){Listener->SetAudioListenerOverride(nullptr,FVector(0,200,150),FRotator(0,-90,0));}
    void SpawnSource(UWorld* World,int32 Frequency)
    {
        Source=World->SpawnActor<AActor>();auto* Audio=NewObject<UAudioComponent>(Source.Get());Source->SetRootComponent(Audio);Source->AddInstanceComponent(Audio);
        Audio->bAutoActivate=false;Audio->RegisterComponent();Audio->SetWorldLocation(FVector(0,-200,150));
        auto* Marker=NewObject<UIMAcousticSourceComponent>(Source.Get());Source->AddInstanceComponent(Marker);Marker->AudioComponent=Audio;Marker->RegisterComponent();
        FString Error;
        if(!IMAcousticTestSupport::ConfigureGraphSource(Audio,Error))
        {
            UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticLifecycleSource FAIL %s"),*Error);
            return;
        }
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycleSourceGraph asset=%s diagnostic_frequency=%d"),*GetNameSafe(Audio->Sound),Frequency);
        Audio->Play();
    }
    bool ReadRecording(const TCHAR* Route,double& Energy,double& Balance)
    {
        TArray<uint8> Bytes;FWaveModInfo Info;const FString File=FPaths::Combine(State->Directory,Name(Route)+TEXT(".wav"));
        if(IFileManager::Get().FileSize(*File)<=44||!FFileHelper::LoadFileToArray(Bytes,*File)||!Info.ReadWaveInfo(Bytes.GetData(),Bytes.Num()))return false;
        if(!Info.pBitsPerSample||*Info.pBitsPerSample!=16||Info.SampleDataSize<96000)return false;
        double Left=0,Right=0;for(uint32 I=0;I+1<Info.SampleDataSize;I+=2){int16 V;FMemory::Memcpy(&V,Info.SampleDataStart+I,2);const double E=double(V)*V;if((I/2)%2)Right+=E;else Left+=E;}
        Energy=Left+Right;Balance=Energy?(Left-Right)/Energy:0;return true;
    }
    bool Finish(bool Success,const FString& Message)
    {
        if(!Success)State->Test->AddError(Message);FApp::SetUnfocusedVolumeMultiplier(State->BackgroundVolume);GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=State->bAllowBackgroundAudioOrig;
        IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticLifecycle %s evidence=%s"),*Message,*State->Directory);
        UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticLifecycle %s"),Success?TEXT("PASS"):TEXT("FAIL"));
        UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));GEditor->RequestEndPlayMap();return true;
    }
    TSharedRef<FIMLifecycleState> State;int32 Stage=0;double Stamp=0,LastDiagnostic=0;
    TWeakObjectPtr<AIMAcousticBakeVolume> Volume;TWeakObjectPtr<APlayerController> Listener;TWeakObjectPtr<AActor> Source;
    TWeakObjectPtr<AIMAcousticBakeVolume> Competitor;uint64 OwnerEpoch=0;
    TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Bridge;TArray<uint64> Generations;
    FIMAcousticMetaSoundContextPtr MetaContext;
    FIMAcousticMetaSoundContextPtr PreviousMetaContext;
    uint32 GraphWindowSourceStart=0,GraphWindowEnvironmentStart=0;
    uint64 PathStart=0,ReverbStart=0,BypassStart=0,RejectedStart=0,InputStart=0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticLifecycle,"IceMoon.AcousticField.W3.Lifecycle",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticLifecycle::RunTest(const FString&)
{
    auto State=MakeShared<IMAcousticLifecycleTestPrivate::FIMLifecycleState>();State->Test=this;State->Started=FPlatformTime::Seconds();
    State->BackgroundVolume=FApp::GetUnfocusedVolumeMultiplier();FApp::SetUnfocusedVolumeMultiplier(1);
    State->bAllowBackgroundAudioOrig=GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=true;
    State->Directory=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W3-Lifecycle"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    IFileManager::Get().MakeDirectory(*State->Directory,true);
    FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/Tests/IM_W1Door"),false,&Error);
    if(!Error.IsEmpty()){FApp::SetUnfocusedVolumeMultiplier(State->BackgroundVolume);AddError(Error);return false;}
    IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
    ADD_LATENT_AUTOMATION_COMMAND(IMAcousticLifecycleTestPrivate::FIMAcousticLifecycleCommand(State));return true;
}
#endif
