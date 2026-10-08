#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "IMAcousticSpatialization.h"
#include "AudioMixerBlueprintLibrary.h"
#include "AudioDevice.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Sound/SoundWaveProcedural.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticW3TestPrivate
{
constexpr int32 W3Voices=16,W3Rate=48000;
constexpr double W3MotionPeriod=0.05; // Match the product's 20 Hz GT snapshot cadence.
constexpr double W3Duration=600;
constexpr double W3RareTailNearBudgetFraction=0.90;
constexpr uint32 W3RareTailNearTailUs=20000;
constexpr uint32 W3RareTailSelfTestUs=10000;
class FIMAcousticUnderrunObserver final:public FOutputDevice
{
public:
	std::atomic<uint64> Count{0};std::atomic<bool> Enabled{false};
	void Serialize(const TCHAR* Text,ELogVerbosity::Type,const FName&) override
	{if(Enabled.load(std::memory_order_relaxed)&&FCString::Strifind(Text,TEXT("Audio Buffer Underrun (starvation) detected")))Count.fetch_add(1,std::memory_order_relaxed);}
	bool CanBeUsedOnAnyThread() const override{return true;}
};
static uint64 W3ProbeComplete(const std::atomic<uint64>* Done,uint32 Capacity,uint64 Pushes)
{
	const uint64 Limit=Pushes<Capacity?Pushes:Capacity;
	for(uint64 I=0;I<Limit;++I)if(Done[I].load(std::memory_order_acquire)!=I+1)return I;
	return Limit;
}
static bool W3ExportCallbackCorrelation(FIMAcousticDeviceBridge* Bridge,const FString& EvidenceDir)
{
	if(!Bridge)return false;
	const uint64 SourcePushes=Bridge->SourceBlockProbePushes.load(std::memory_order_acquire);
	const uint64 SourceLimit=SourcePushes<FIMAcousticDeviceBridge::CallbackBlockProbeCapacity?SourcePushes:FIMAcousticDeviceBridge::CallbackBlockProbeCapacity;
	const uint64 SourceComplete=W3ProbeComplete(Bridge->SourceBlockDone.data(),FIMAcousticDeviceBridge::CallbackBlockProbeCapacity,SourcePushes);
	const uint64 ReverbPushes=Bridge->ReverbBlockProbePushes.load(std::memory_order_acquire);
	const uint64 ReverbLimit=ReverbPushes<FIMAcousticDeviceBridge::CallbackBlockProbeCapacity?ReverbPushes:FIMAcousticDeviceBridge::CallbackBlockProbeCapacity;
	const uint64 ReverbComplete=W3ProbeComplete(Bridge->ReverbBlockDone.data(),FIMAcousticDeviceBridge::CallbackBlockProbeCapacity,ReverbPushes);
	FString Sources=TEXT("audio_block,callback_count,valid_source_count,source_sum_us,start_s,end_s\n");
	for(uint64 I=0;I<SourceComplete;++I)
	{
		const auto& P=Bridge->SourceBlockProbes[I];
		Sources+=FString::Printf(TEXT("%llu,%u,%u,%.3f,%.9f,%.9f\n"),P.AudioBlock,P.CallbackCount,P.ValidSourceCount,P.SourceSumUs,P.StartSeconds,P.EndSeconds);
	}
	FString Reverbs=TEXT("audio_block,outcome,fresh,rendered,start_s,end_s,duration_us\n");
	for(uint64 I=0;I<ReverbComplete;++I)
	{
		const auto& P=Bridge->ReverbBlockProbes[I];
		Reverbs+=FString::Printf(TEXT("%llu,%u,%u,%u,%.9f,%.9f,%.3f\n"),P.AudioBlock,P.Outcome,P.Fresh,P.Rendered,P.StartSeconds,P.EndSeconds,(P.EndSeconds-P.StartSeconds)*1.e6);
	}
	FString Snapshots=TEXT("block,world,captured_s,submit_s,submit_span_us,num_sources,submitted,fail_code\n");
	const uint64 SnapshotPushes=Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
	const uint64 SnapshotComplete=W3ProbeComplete(Bridge->SnapshotDone.data(),FIMAcousticDeviceBridge::ProbeSnapshotCapacity,SnapshotPushes);
	for(uint64 I=0;I<SnapshotComplete;++I)
	{
		const auto& P=Bridge->SnapshotProbes[I];
		Snapshots+=FString::Printf(TEXT("%llu,%llu,%.9f,%.9f,%.3f,%u,%u,%u\n"),P.Block,P.WorldGeneration,P.CapturedSeconds,P.SubmitSeconds,(P.SubmitSeconds-P.CapturedSeconds)*1.e6,P.NumSources,P.Submitted,P.FailCode);
	}
	FString Workers=TEXT("block,world,loop_start_s,snap_captured_s,eval_start_s,eval_end_s,reverb_start_s,reverb_end_s,push_s,wait_end_s,num_inputs,published,eval_ok,reverb_attempt,reverb_ok\n");
	const uint64 WorkerPushes=Bridge->WorkerProbePushes.load(std::memory_order_acquire);
	const uint64 WorkerComplete=W3ProbeComplete(Bridge->WorkerDone.data(),FIMAcousticDeviceBridge::ProbeWorkerCapacity,WorkerPushes);
	for(uint64 I=0;I<WorkerComplete;++I)
	{
		const auto& P=Bridge->WorkerProbes[I];
		Workers+=FString::Printf(TEXT("%llu,%llu,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%u,%u,%u,%u,%u\n"),P.Block,P.WorldGeneration,P.LoopStartSeconds,P.SnapshotCaptured,P.EvalStartSeconds,P.EvalEndSeconds,P.ReverbStartSeconds,P.ReverbEndSeconds,P.PushSeconds,P.WaitEndSeconds,P.NumInputs,P.ResultsPublished,P.EvalOk,P.ReverbAttempt,P.ReverbOk);
	}
	const bool SourceOk=Bridge->SourceBlockProbeOverflows.load(std::memory_order_acquire)==0&&SourceComplete==SourceLimit;
	const bool ReverbOk=Bridge->ReverbBlockProbeOverflows.load(std::memory_order_acquire)==0&&ReverbComplete==ReverbLimit;
	const bool SnapshotOk=Bridge->SnapshotProbeOverflows.load(std::memory_order_acquire)==0&&SnapshotComplete==(SnapshotPushes<FIMAcousticDeviceBridge::ProbeSnapshotCapacity?SnapshotPushes:FIMAcousticDeviceBridge::ProbeSnapshotCapacity);
	const bool WorkerOk=Bridge->WorkerProbeOverflows.load(std::memory_order_acquire)==0&&WorkerComplete==(WorkerPushes<FIMAcousticDeviceBridge::ProbeWorkerCapacity?WorkerPushes:FIMAcousticDeviceBridge::ProbeWorkerCapacity);
	const FString Summary=FString::Printf(TEXT("{\"complete\":%s,\"source_blocks\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu},\"reverb_blocks\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu},\"snapshots\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu},\"workers\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu}}"),
		(SourceOk&&ReverbOk&&SnapshotOk&&WorkerOk)?TEXT("true"):TEXT("false"),SourceOk?TEXT("true"):TEXT("false"),SourcePushes,SourceComplete,Bridge->SourceBlockProbeOverflows.load(),ReverbOk?TEXT("true"):TEXT("false"),ReverbPushes,ReverbComplete,Bridge->ReverbBlockProbeOverflows.load(),SnapshotOk?TEXT("true"):TEXT("false"),SnapshotPushes,SnapshotComplete,Bridge->SnapshotProbeOverflows.load(),WorkerOk?TEXT("true"):TEXT("false"),WorkerPushes,WorkerComplete,Bridge->WorkerProbeOverflows.load());
	const bool SavedSources=FFileHelper::SaveStringToFile(Sources,*FPaths::Combine(EvidenceDir,TEXT("callback-source-blocks.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedReverbs=FFileHelper::SaveStringToFile(Reverbs,*FPaths::Combine(EvidenceDir,TEXT("callback-reverb-blocks.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSnapshots=FFileHelper::SaveStringToFile(Snapshots,*FPaths::Combine(EvidenceDir,TEXT("callback-gt-snapshots.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedWorkers=FFileHelper::SaveStringToFile(Workers,*FPaths::Combine(EvidenceDir,TEXT("callback-worker-loops.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSummary=FFileHelper::SaveStringToFile(Summary,*FPaths::Combine(EvidenceDir,TEXT("callback-correlation-summary.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3CallbackCorrelation complete=%d source=%llu/%llu reverb=%llu/%llu snapshots=%llu workers=%llu evidence=%s"),SourceOk&&ReverbOk&&SnapshotOk&&WorkerOk,SourceComplete,SourcePushes,ReverbComplete,ReverbPushes,SnapshotComplete,WorkerComplete,*EvidenceDir);
	return SavedSources&&SavedReverbs&&SavedSnapshots&&SavedWorkers&&SavedSummary&&SourceOk&&ReverbOk&&SnapshotOk&&WorkerOk;
}
static bool W3ExportMetaSoundCorrelation(const FIMAcousticMetaSoundContextPtr& Context,const FString& EvidenceDir)
{
	if (!Context.IsValid()) return false;
	const uint32 SourceCount = FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire), uint32(Context->CapturedSourceBlocks.Num()));
	const uint32 EnvironmentCount = FMath::Min<uint32>(Context->CapturedBlockCount.load(std::memory_order_acquire), uint32(Context->CapturedBlocks.Num()));
	FString Sources = TEXT("block,voice,callback_audio_id,result_audio_id,world_generation,voice_generation,sequence,age_ms,routes,direct_valid,path_valid,reject,fallback,input_energy,output_energy\n");
	for (uint32 I = 0; I < SourceCount; ++I)
	{
		const auto& P = Context->CapturedSourceBlocks[int32(I)];
		Sources += FString::Printf(TEXT("%llu,%u,%llu,%llu,%llu,%llu,%llu,%.6f,%u,%u,%u,%u,%u,%.9g,%.9g\n"),
			P.Block, P.Voice, P.CallbackAudioComponentId, P.ResultAudioComponentId, P.ResultWorldGeneration,
			P.ResultVoiceGeneration, P.ResultSequence, P.AgeMs, P.Routes, P.DirectValid, P.PathValid,
			uint32(P.Reject), P.Fallback, P.InputEnergy, P.OutputEnergy);
	}
	FString Environments = TEXT("block,frame,sequence,seconds,fresh,listener_x,listener_y,listener_z\n");
	for (uint32 I = 0; I < EnvironmentCount; ++I)
	{
		const auto& E = Context->CapturedBlocks[int32(I)];
		Environments += FString::Printf(TEXT("%u,%llu,%llu,%.9f,%u,%.6f,%.6f,%.6f\n"),
			I, E.Frame, E.Sequence, E.Seconds, E.Fresh ? 1u : 0u, E.ListenerX, E.ListenerY, E.ListenerZ);
	}
	const FString Summary = FString::Printf(TEXT("{\"source_blocks\":%u,\"environment_blocks\":%u,\"source_frames\":%u,\"environment_frames\":%u,\"source_overflow\":%s,\"environment_overflow\":%s}"),
		SourceCount, EnvironmentCount, Context->CapturedSourceFrames.load(std::memory_order_acquire),
		Context->CapturedEnvironmentFrames.load(std::memory_order_acquire),
		SourceCount < uint32(Context->CapturedSourceBlocks.Num()) ? TEXT("false") : TEXT("true"),
		EnvironmentCount < uint32(Context->CapturedBlocks.Num()) ? TEXT("false") : TEXT("true"));
	const bool SavedSources = FFileHelper::SaveStringToFile(Sources, *FPaths::Combine(EvidenceDir, TEXT("metasound-source-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedEnvironments = FFileHelper::SaveStringToFile(Environments, *FPaths::Combine(EvidenceDir, TEXT("metasound-environment-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSummary = FFileHelper::SaveStringToFile(Summary, *FPaths::Combine(EvidenceDir, TEXT("metasound-correlation-summary.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticW3MetaSoundCorrelation source=%u environment=%u evidence=%s"), SourceCount, EnvironmentCount, *EvidenceDir);
	return SavedSources && SavedEnvironments && SavedSummary && SourceCount > 0 && EnvironmentCount > 0;
}
static bool W3ExportRareTailCorrelation(FIMAcousticDeviceBridge* Bridge,const FString& EvidenceDir,bool Requested,bool SelfTest)
{
	if(!Bridge)return false;
	const uint64 Pushes=Bridge->RareTailProbePushes.load(std::memory_order_acquire);
	const uint64 Capacity=FIMAcousticDeviceBridge::RareTailProbeCapacity;
	const uint64 Begin=Pushes>Capacity?Pushes-Capacity:0;
	const uint64 Retained=Pushes<Capacity?Pushes:Capacity;
	bool Complete=Bridge->RareTailProbeOverflows.load(std::memory_order_acquire)==0;
	FString CSV=TEXT("sequence,audio_block,source_block,trigger_mask,source_join_valid,source_sum_us,reverb_us,combined_us,outcome,fresh,rendered,self_test,reverb_start_s,reverb_end_s,max_snapshot_gap_us,max_worker_gap_us\n");
	uint64 Exported=0;
	for(uint64 N=Begin;N<Pushes;++N)
	{
		const uint32 Slot=static_cast<uint32>(N%Capacity);
		if(Bridge->RareTailDone[Slot].load(std::memory_order_acquire)!=N+1){Complete=false;continue;}
		const auto& P=Bridge->RareTailProbes[Slot];
		CSV+=FString::Printf(TEXT("%llu,%llu,%llu,%u,%u,%.3f,%.3f,%.3f,%u,%u,%u,%u,%.9f,%.9f,%llu,%llu\n"),
			N,P.AudioBlock,P.SourceBlock,P.TriggerMask,P.SourceJoinValid,P.SourceSumUs,P.ReverbUs,P.CombinedUs,P.Outcome,P.Fresh,P.Rendered,P.SelfTest,
			P.ReverbStartSeconds,P.ReverbEndSeconds,P.MaxSnapshotGapUs,P.MaxWorkerGapUs);
		++Exported;
	}
	const uint32 DeviceBudgetUs=Bridge->RareTailDeviceBudgetUs.load(std::memory_order_relaxed);
	const uint32 NearBudgetUs=Bridge->RareTailNearBudgetUs.load(std::memory_order_relaxed);
	const uint32 NearTailUs=Bridge->RareTailNearTailUs.load(std::memory_order_relaxed);
	const uint64 Overflow=Bridge->RareTailProbeOverflows.load(std::memory_order_acquire);
	const FString Summary=FString::Printf(TEXT("{\"requested\":%s,\"self_test\":%s,\"negative_control\":%s,\"device_budget_us\":%u,\"near_budget_us\":%u,\"near_tail_us\":%u,\"capacity\":%llu,\"pushed\":%llu,\"retained\":%llu,\"exported\":%llu,\"overflow\":%llu,\"complete\":%s,\"closing_condition\":\"flags_disabled_plus_0.3s_drain\"}"),
		Requested?TEXT("true"):TEXT("false"),SelfTest?TEXT("true"):TEXT("false"),(!Requested)?TEXT("true"):TEXT("false"),DeviceBudgetUs,NearBudgetUs,NearTailUs,Capacity,Pushes,Retained,Exported,Overflow,Complete?TEXT("true"):TEXT("false"));
	const bool SavedCSV=FFileHelper::SaveStringToFile(CSV,*FPaths::Combine(EvidenceDir,TEXT("callback-rare-tail.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSummary=FFileHelper::SaveStringToFile(Summary,*FPaths::Combine(EvidenceDir,TEXT("callback-rare-tail-contract.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3RareTail requested=%d self_test=%d complete=%d pushed=%llu exported=%llu overflow=%llu thresholds=%u/%u/%u evidence=%s"),
		Requested,SelfTest,Complete,Pushes,Exported,Overflow,DeviceBudgetUs,NearBudgetUs,NearTailUs,*EvidenceDir);
	return SavedCSV&&SavedSummary&&Complete;
}
class FIMAcousticW3PressureCommand final:public IAutomationLatentCommand
{
public:
	explicit FIMAcousticW3PressureCommand(FAutomationTestBase* In,double InDuration=W3Duration,bool InDropWet=false,bool InStaleProbe=false):Test(In),Created(FPlatformTime::Seconds()),Duration(InDuration),DropWet(InDropWet),StaleProbe(InStaleProbe){}
	~FIMAcousticW3PressureCommand() override {Cleanup();}
	bool Update() override
	{
		const double Now=FPlatformTime::Seconds();
		if(Now-Created>Duration+90)return Finish(false,TEXT("Pressure test exceeded its bounded runtime."));
		UWorld* World=nullptr;for(const auto& C:GEngine->GetWorldContexts())if(C.WorldType==EWorldType::PIE){World=C.World();break;}
		if(!World)return Stage==0?false:Finish(false,TEXT("PIE ended during pressure test."));
		if(Stage==0)
		{
			for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It)Volume=*It;
			Listener=World->GetFirstPlayerController();if(!Volume.IsValid()||!Listener.IsValid())return false;
			Bridge=IMAcousticTestSupport::FindBridge(World);if(!Bridge)return false;
			BackgroundVolume=FApp::GetUnfocusedVolumeMultiplier();FApp::SetUnfocusedVolumeMultiplier(1);
			bAllowBackgroundAudioOrig=GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=true;SettingsChanged=true;
			auto* Performance=GetMutableDefault<UEditorPerformanceSettings>();PreviousThrottle=Performance->bThrottleCPUWhenNotForeground;
			// An unattended editor must continue GT snapshots when another app
			// has focus. This is test-local and restored without saving config.
			Performance->bThrottleCPUWhenNotForeground=false;
			LogInterval=IConsoleManager::Get().FindConsoleVariable(TEXT("au.MinLogTimeBetweenUnderrunWarnings"));
			if(LogInterval){PreviousLogInterval=LogInterval->GetFloat();LogInterval->Set(0.f,ECVF_SetByCode);}
			GLog->AddOutputDevice(&Underruns);ObserverRegistered=true;
			PCM.SetNumUninitialized(W3Rate*2);
			// Two seconds of low-level periodic input; all voices together remain
			// below clipping. Queue replenishment is GT-only and measured separately.
			for(int32 I=0;I<PCM.Num();++I)PCM[I]=int16(300*FMath::Sin(2*PI*440*I/W3Rate));
			for(int32 I=0;I<W3Voices;++I)
			{
				auto* Actor=World->SpawnActor<AActor>();auto* Audio=NewObject<UAudioComponent>(Actor);
				Actor->SetRootComponent(Audio);Actor->AddInstanceComponent(Audio);Audio->bAutoActivate=false;Audio->RegisterComponent();
				auto* Source=NewObject<UIMAcousticSourceComponent>(Actor);Actor->AddInstanceComponent(Source);Source->AudioComponent=Audio;Source->RegisterComponent();
				FString SourceError;
				if(!IMAcousticTestSupport::ConfigureGraphSource(Audio,SourceError))return Finish(false,SourceError);
				FString ValidationError;
				if(!Source->ValidateSource(ValidationError))return Finish(false,ValidationError);
				Audio->Play();Sources.Add(Audio);Sounds.Add(Audio->Sound);
			}
			if (FAudioDevice* AudioDevice = World->GetAudioDeviceRaw())
				MetaContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
			Volume->bEnableV2=true;Volume->bDirectRoute=true;Volume->bPathRoute=true;Volume->bReverbRoute=true;
			Directory=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W3-UE"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*Directory,true);CSV=TEXT("elapsed_s,rendered,rejected,reverb_rejected,dry_dropped,used_physical_bytes,reverb_calls,reverb_dry,reverb_raw,reverb_audible,max_snapshot_gap_us,max_worker_gap_us\n");
			Stage=1;StageStarted=Now;
		}
		Bridge=IMAcousticTestSupport::FindBridge(World);
		if (!MetaContext.IsValid())
		{
			if (FAudioDevice* AudioDevice = World->GetAudioDeviceRaw())
				MetaContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
		}
		if(!Volume.IsValid()||!Listener.IsValid()||!Bridge||!Bridge->Alive.load())return Finish(false,TEXT("Pressure world/device disappeared."));
		const double Motion=Now-Created;
		if(Now>=NextMotionUpdate)
		{
			NextMotionUpdate=Now+W3MotionPeriod;
			Listener->SetAudioListenerOverride(nullptr,FVector(900+350*FMath::Sin(Motion*.23),300,150),FRotator(0,20*FMath::Sin(Motion*.17),0));
			if(FParse::Param(FCommandLine::Get(),TEXT("nullrhi")))
			{
				// NullRHI omits GameViewportClient::Draw's listener update. Mirror
				// that GT update for this diagnostic only; normal acceptance draws
				// the real viewport and uses the engine's listener publication.
				FVector Location,Front,Right;Listener->GetAudioListenerPosition(Location,Front,Right);
				FTransform Transform(FRotationMatrix::MakeFromXY(Front,Right));Transform.SetTranslation(Location);Transform.NormalizeRotation();
				World->GetAudioDeviceRaw()->SetListener(World,0,Transform,World->GetDeltaSeconds());
			}
			for(int32 I=0;I<Sources.Num();++I)
			{
				if(!Sources[I].IsValid()||!Sounds[I].IsValid())return Finish(false,TEXT("Managed pressure source destroyed unexpectedly."));
				Sources[I]->SetWorldLocation(FVector(900+400*FMath::Sin(Motion*.21+I*.4),300+25*FMath::Sin(Motion*.31+I),150));
			}
		}
		for(int32 I=0;I<Sources.Num();++I)
		{
			if(!Sources[I].IsValid()||!Sounds[I].IsValid())return Finish(false,TEXT("Managed pressure source destroyed unexpectedly."));
		}
		// Whitebox first publication measured at ~5.5s after Stage1 under load
		// (2s GT hitches during heavy-map startup). Warmup lets the system reach
		// steady state; all acceptance thresholds below are unchanged.
		if(Stage==1&&Now-StageStarted>=30)
		{
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3RenderFaultCounts frame=%llu input=%llu direct=%llu path=%llu"),
				Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::InvalidFrame)].load(),Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::NonfiniteInput)].load(),
				Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::NonfiniteDirect)].load(),Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::NonfinitePath)].load());
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3Warmup elapsed_s=%g rendered=%llu reverb=%llu rejected=%llu stale=%llu missing=%llu reverb_calls=%llu dry=%llu raw=%llu snapshot_gap_us=%llu worker_gap_us=%llu status=%s"),
				Now-StageStarted,Bridge->RenderedBlocks.load(),Bridge->ReverbNonzeroBlocks.load(),Bridge->RejectedBlocks.load(),
				Bridge->StaleResultBlocks.load(),Bridge->MissingResultBlocks.load(),Bridge->ReverbProcessedBlocks.load(),
				Bridge->ReverbDryBlocks.load(),Bridge->ReverbRawNonzeroBlocks.load(),Bridge->MaxSnapshotGapUs.load(),Bridge->MaxWorkerGapUs.load(),*Volume->Status);
			if(Bridge->RenderedBlocks.load()<W3Voices*50||Bridge->ReverbNonzeroBlocks.load()<10)
				return Finish(false,TEXT("Actual complex-scene sound routes did not become audible."));
			RejectedStart=Bridge->RejectedBlocks.load();ReverbRejectedStart=Bridge->ReverbRejectedBlocks.load();
			DryDroppedStart=Bridge->DryDroppedBlocks.load();RenderedStart=Bridge->RenderedBlocks.load();
			Bridge->ResetPressureDiagnostics();
			Bridge->MaxSnapshotGapUs.store(0);Bridge->MaxWorkerGapUs.store(0);
			Bridge->ProfilingEnabled.store(true);CallbackTraceRequested=Duration<=30.0;
			RareTailRequested=!DropWet&&!StaleProbe;RareTailSelfTest=RareTailRequested&&Duration<W3Duration;
			const uint32 DeviceBudgetUs=static_cast<uint32>(FMath::CeilToDouble(double(Bridge->BlockFrames)/Bridge->SampleRate*1.e6));
			const uint32 NearBudgetUs=RareTailSelfTest?W3RareTailSelfTestUs:static_cast<uint32>(FMath::CeilToDouble(double(DeviceBudgetUs)*W3RareTailNearBudgetFraction));
			const uint32 NearTailUs=RareTailSelfTest?W3RareTailSelfTestUs:W3RareTailNearTailUs;
			Bridge->ConfigureRareTailDiagnostics(RareTailRequested,RareTailSelfTest,DeviceBudgetUs,NearBudgetUs,NearTailUs);
			Bridge->CallbackDiagnosticsEnabled.store(CallbackTraceRequested);Underruns.Enabled.store(true);StartMemory=FPlatformMemory::GetStats().UsedPhysical;
			UAudioMixerBlueprintLibrary::StartRecordingOutput(World,12,nullptr);Recording=true;
			LastReverbCalls=Bridge->ReverbProcessedBlocks.load();LastReverbDry=Bridge->ReverbDryBlocks.load();
			LastReverbRaw=Bridge->ReverbRawNonzeroBlocks.load();LastReverbAudible=Bridge->ReverbNonzeroBlocks.load();
			Stage=2;StageStarted=Now;MeasurementStarted=Now;NextSample=Now+1;
		}
		if(Stage==2)
		{
			const double Elapsed=Now-StageStarted;
			// Separate negative-control test: the normal pressure command never
			// injects an outage and still requires continuous wet output.
			if(DropWet)Volume->bReverbRoute=!(Elapsed>=10&&Elapsed<13);
			// Stale-drain probe: halt GT once so snapshots age past the 250ms
			// lease while audio callbacks keep pushing dry sends. This forces a
			// genuine not-fresh IR window with zero product-path changes; the
			// verdict checks the drain fix and only the drain fix.
			if(StaleProbe&&!StaleInjected&&Elapsed>=2)
			{
				StaleN0=Bridge->ReverbNotFreshBlocks.load();StaleD0=Bridge->DryDroppedBlocks.load();StaleR0=Bridge->ReverbNonzeroBlocks.load();
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3StaleInject notfresh=%llu drydropped=%llu reverbnonzero=%llu"),StaleN0,StaleD0,StaleR0);
				FPlatformProcess::Sleep(0.7f);StaleInjected=true;
			}
			if(StaleProbe&&StaleInjected&&Elapsed>=8)
			{
				const uint64 N1=Bridge->ReverbNotFreshBlocks.load(),D1=Bridge->DryDroppedBlocks.load(),R1=Bridge->ReverbNonzeroBlocks.load();
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3StaleVerdict notfresh=%llu(+%llu) drydropped=%llu(+%llu) reverbnonzero=%llu(+%llu)"),N1,N1-StaleN0,D1,D1-StaleD0,R1,R1-StaleR0);
				const FString JS=FString::Printf(TEXT("{\"notfresh_before\":%llu,\"notfresh_after\":%llu,\"drydropped_before\":%llu,\"drydropped_after\":%llu,\"reverbnonzero_before\":%llu,\"reverbnonzero_after\":%llu}"),StaleN0,N1,StaleD0,D1,StaleR0,R1);
				FFileHelper::SaveStringToFile(JS,*FPaths::Combine(Directory,TEXT("stale-probe.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				if(N1==StaleN0)return Finish(false,TEXT("Stale-drain probe INCONCLUSIVE: injection produced no not-fresh window."));
				if(D1!=StaleD0)return Finish(false,TEXT("Stale-drain probe FAIL: dry sends dropped during not-fresh window."));
				if(R1<=StaleR0)return Finish(false,TEXT("Stale-drain probe FAIL: reverb did not recover after stale window."));
				return Finish(true,TEXT("Stale-drain probe PASS: not-fresh window drained without dry drops and reverb recovered."));
			}
			if(Recording&&Elapsed>=10)
			{
				UAudioMixerBlueprintLibrary::StopRecordingOutput(World,EAudioRecordingExportType::WavFile,TEXT("IM_complex_16voices"),Directory,nullptr,nullptr);Recording=false;
			}
			if(Now>=NextSample)
			{
				NextSample=Now+1;const uint64 Memory=FPlatformMemory::GetStats().UsedPhysical;PeakMemory=FMath::Max(PeakMemory,Memory);
				const uint64 Calls=Bridge->ReverbProcessedBlocks.load(),Dry=Bridge->ReverbDryBlocks.load();
				const uint64 Raw=Bridge->ReverbRawNonzeroBlocks.load(),Audible=Bridge->ReverbNonzeroBlocks.load();
				if(DropWet&&Elapsed>=11&&Elapsed<13&&Audible==LastReverbAudible)WetDropDetected=true;
				if(DropWet&&Elapsed>=14&&Audible>LastReverbAudible)WetRecoveryObserved=true;
				// Continuous periodic source input must reach the environmental
				// output in every sampled second, not only during warmup.
				ContinuousReverb=ContinuousReverb&&Calls>LastReverbCalls&&Dry>LastReverbDry&&Raw>LastReverbRaw&&Audible>LastReverbAudible;
				LastReverbCalls=Calls;LastReverbDry=Dry;LastReverbRaw=Raw;LastReverbAudible=Audible;
				CSV+=FString::Printf(TEXT("%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n"),Elapsed,Bridge->RenderedBlocks.load()-RenderedStart,
					Bridge->RejectedBlocks.load()-RejectedStart,Bridge->ReverbRejectedBlocks.load()-ReverbRejectedStart,
					Bridge->DryDroppedBlocks.load()-DryDroppedStart,Memory,Calls,Dry,Raw,Audible,
					Bridge->MaxSnapshotGapUs.load(),Bridge->MaxWorkerGapUs.load());
			}
			if(Elapsed<Duration)return false;
			Bridge->ProfilingEnabled.store(false);Bridge->CallbackDiagnosticsEnabled.store(false);Bridge->RareTailDiagnosticsEnabled.store(false,std::memory_order_release);Underruns.Enabled.store(false);MeasuredSeconds=Elapsed;Stage=3;StageStarted=Now;
		}
		if(Stage==3&&Now-StageStarted>.3)
		{
			// Profiling is disabled before serialization; allow in-flight callback
			// scopes to retire so histogram totals are stable without audio locks.
			FString Json;auto W=TJsonWriterFactory<>::Create(&Json);W->WriteObjectStart();
			W->WriteValue(TEXT("duration_s"),MeasuredSeconds);W->WriteValue(TEXT("voices"),W3Voices);
			const bool HeadlessDiagnostic=FParse::Param(FCommandLine::Get(),TEXT("nullrhi"));
			W->WriteValue(TEXT("nullrhi_diagnostic_only"),HeadlessDiagnostic);
			W->WriteValue(TEXT("sample_rate"),Bridge->SampleRate);W->WriteValue(TEXT("block_frames"),Bridge->BlockFrames);
			const double DeadlineUs=double(Bridge->BlockFrames)/Bridge->SampleRate*1.e6;W->WriteValue(TEXT("device_block_deadline_us"),DeadlineUs);
			auto Stats=[&W](const TCHAR* Name,const FIMAcousticTiming& T)
			{
				const uint64 Count=T.Count.load();uint64 Cumulative=0;double P99=-1;
				W->WriteObjectStart(Name);W->WriteValue(TEXT("count"),double(Count));W->WriteValue(TEXT("bin_width_us"),T.BinMicroseconds);
				W->WriteArrayStart(TEXT("histogram_counts"));
				for(uint32 I=0;I<T.BinCount;++I){const uint64 N=T.Bins[I].load();W->WriteValue(double(N));Cumulative+=N;if(P99<0&&Count&&Cumulative>=uint64(FMath::CeilToDouble(Count*.99)))P99=I==T.BinCount-1?-2:(I+1)*T.BinMicroseconds;}
				W->WriteArrayEnd();W->WriteValue(TEXT("p99_upper_us"),P99);
				W->WriteValue(TEXT("max_us"),FPlatformTime::ToSeconds64(T.MaxCycles.load())*1.e6);
				W->WriteValue(TEXT("mean_us"),Count?FPlatformTime::ToSeconds64(T.TotalCycles.load())*1.e6/Count:0);W->WriteObjectEnd();
			};
			Stats(TEXT("source_callback"),Bridge->SourceTiming);Stats(TEXT("all_sources_cpu_per_block"),Bridge->SourceBlockTiming);
			Stats(TEXT("reverb_callback"),Bridge->ReverbTiming);Stats(TEXT("worker_update"),Bridge->WorkerTiming);Stats(TEXT("gt_snapshot"),Bridge->GameThreadTiming);
			// Sub-phase diagnostics (zero behavior; histograms only, no gate input).
			Stats(TEXT("worker_snapshot"),Bridge->WorkerSnapshotTiming);Stats(TEXT("worker_solve"),Bridge->WorkerSolveTiming);
			Stats(TEXT("worker_reverb"),Bridge->WorkerReverbTiming);Stats(TEXT("worker_publish"),Bridge->WorkerPublishTiming);
			Stats(TEXT("gt_traverse"),Bridge->GTTraverseTiming);Stats(TEXT("gt_submit"),Bridge->GTSubmitTiming);
			const double ConservativeMaxUs=FPlatformTime::ToSeconds64(Bridge->SourceBlockTiming.MaxCycles.load()+Bridge->ReverbTiming.MaxCycles.load())*1.e6;
			W->WriteValue(TEXT("feature_callback_max_sum_us"),ConservativeMaxUs);W->WriteValue(TEXT("underrun_log_events"),double(Underruns.Count.load()));
			W->WriteValue(TEXT("callback_trace_requested"),CallbackTraceRequested);W->WriteValue(TEXT("callback_trace_records"),double(Bridge->CallbackProbePushes.load()));W->WriteValue(TEXT("callback_trace_overflows"),double(Bridge->CallbackProbeOverflows.load()));
			W->WriteValue(TEXT("rare_tail_requested"),RareTailRequested);W->WriteValue(TEXT("rare_tail_self_test"),RareTailSelfTest);
			W->WriteValue(TEXT("rare_tail_device_budget_us"),double(Bridge->RareTailDeviceBudgetUs.load()));W->WriteValue(TEXT("rare_tail_near_budget_us"),double(Bridge->RareTailNearBudgetUs.load()));W->WriteValue(TEXT("rare_tail_near_tail_us"),double(Bridge->RareTailNearTailUs.load()));
			W->WriteValue(TEXT("rare_tail_probe_records"),double(Bridge->RareTailProbePushes.load()));W->WriteValue(TEXT("rare_tail_probe_overflows"),double(Bridge->RareTailProbeOverflows.load()));
			W->WriteValue(TEXT("rejected_blocks"),double(Bridge->RejectedBlocks.load()-RejectedStart));
			W->WriteValue(TEXT("reverb_rejected_blocks"),double(Bridge->ReverbRejectedBlocks.load()-ReverbRejectedStart));
			W->WriteValue(TEXT("dry_dropped_blocks"),double(Bridge->DryDroppedBlocks.load()-DryDroppedStart));
			W->WriteValue(TEXT("max_gt_snapshot_gap_us"),double(Bridge->MaxSnapshotGapUs.load()));W->WriteValue(TEXT("max_worker_gap_us"),double(Bridge->MaxWorkerGapUs.load()));
			const uint64 FirstRejectUs=Bridge->FirstPressureRejectTimeUs.load(std::memory_order_acquire);
			const uint64 FirstReverbRejectUs=Bridge->FirstPressureReverbRejectTimeUs.load(std::memory_order_acquire);
			W->WriteValue(TEXT("first_reject_time_s"),FirstRejectUs?double(FirstRejectUs)*1.e-6-MeasurementStarted:-1.0);
			W->WriteValue(TEXT("first_reject_detail"),double(Bridge->FirstPressureRejectDetail.load(std::memory_order_relaxed)));
			W->WriteValue(TEXT("first_reject_voice"),double(Bridge->FirstPressureRejectVoice.load(std::memory_order_relaxed)));
			W->WriteValue(TEXT("first_reverb_reject_time_s"),FirstReverbRejectUs?double(FirstReverbRejectUs)*1.e-6-MeasurementStarted:-1.0);
			W->WriteValue(TEXT("first_reverb_reject_reason"),double(Bridge->FirstPressureReverbRejectReason.load(std::memory_order_relaxed)));
			W->WriteObjectStart(TEXT("pressure_reject_detail_counts"));
			for(uint32 I=0;I<FIMAcousticDeviceBridge::PressureRejectDetailCapacity;++I)
			{
				const uint64 Count=Bridge->PressureRejectDetails[I].load(std::memory_order_relaxed);
				if(Count)W->WriteValue(FString::Printf(TEXT("%u"),I),double(Count));
			}
			W->WriteObjectEnd();
			W->WriteArrayStart(TEXT("pressure_reverb_reject_reason_counts"));
			for(const auto& Count:Bridge->PressureReverbRejectReasons)W->WriteValue(double(Count.load(std::memory_order_relaxed)));
			W->WriteArrayEnd();
			W->WriteValue(TEXT("stale_result_blocks_including_warmup"),double(Bridge->StaleResultBlocks.load()));
			W->WriteValue(TEXT("missing_result_blocks_including_warmup"),double(Bridge->MissingResultBlocks.load()));
			W->WriteValue(TEXT("invalid_result_blocks_including_warmup"),double(Bridge->InvalidResultBlocks.load()));
			W->WriteValue(TEXT("renderer_failed_blocks_including_warmup"),double(Bridge->RendererFailedBlocks.load()));
			W->WriteArrayStart(TEXT("render_failure_counts_none_frame_input_direct_path"));
			for(const auto& Count:Bridge->RenderFailures)W->WriteValue(double(Count.load()));
			W->WriteArrayEnd();
			W->WriteValue(TEXT("previous_background_throttle"),PreviousThrottle);
			W->WriteValue(TEXT("continuous_reverb_each_sample"),ContinuousReverb);
			W->WriteValue(TEXT("injected_wet_outage_control"),DropWet);
			W->WriteValue(TEXT("wet_outage_detected"),WetDropDetected);W->WriteValue(TEXT("wet_recovery_observed"),WetRecoveryObserved);
			W->WriteValue(TEXT("start_physical_bytes"),double(StartMemory));W->WriteValue(TEXT("peak_physical_bytes"),double(PeakMemory));
			W->WriteObjectEnd();W->Close();
			FString CallbackTraceCSV;const bool CallbackTraceComplete=!CallbackTraceRequested||Bridge->ExportCallbackProbes(CallbackTraceCSV);
			const bool CallbackTraceSaved=!CallbackTraceRequested||(
				CallbackTraceComplete&&FFileHelper::SaveStringToFile(CallbackTraceCSV,*FPaths::Combine(Directory,TEXT("callback-trace.csv"))));
			const bool CallbackCorrelationSaved=!CallbackTraceRequested||(
				MetaContext.IsValid() ? W3ExportMetaSoundCorrelation(MetaContext,Directory) : W3ExportCallbackCorrelation(Bridge.Get(),Directory));
			const bool RareTailCorrelationSaved=W3ExportRareTailCorrelation(Bridge.Get(),Directory,RareTailRequested,RareTailSelfTest);
			const bool Saved=FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Directory,TEXT("pressure.json")))
				&&FFileHelper::SaveStringToFile(CSV,*FPaths::Combine(Directory,TEXT("pressure-timeseries.csv")))&&CallbackTraceSaved&&CallbackCorrelationSaved&&RareTailCorrelationSaved;
			const double Expected=MeasuredSeconds*Bridge->SampleRate/Bridge->BlockFrames*W3Voices;
			const bool WetOracle=DropWet?(!ContinuousReverb&&WetDropDetected&&WetRecoveryObserved):ContinuousReverb;
			const bool Success=Saved&&(!HeadlessDiagnostic||Duration<W3Duration)&&WetOracle&&Bridge->SourceTiming.Count.load()>=Expected*.9&&Bridge->SourceBlockTiming.Count.load()>0
				&&ConservativeMaxUs<DeadlineUs&&Underruns.Count.load()==0&&Bridge->RejectedBlocks.load()==RejectedStart
				&&Bridge->ReverbRejectedBlocks.load()==ReverbRejectedStart&&Bridge->DryDroppedBlocks.load()==DryDroppedStart;
			if(DropWet)return Finish(Success,Success?TEXT("Wet-outage negative control detected silence and recovery."):TEXT("Wet-outage negative control did not meet its detector and audio gates."));
			return Finish(Success,Success?FString::Printf(TEXT("16 voices / %.1f seconds actual audio path passed."),MeasuredSeconds):TEXT("Pressure gate failed; retain raw histogram/time series and fix before acceptance."));
		}
		return false;
	}
private:
	void Cleanup()
	{
		if(Bridge){Bridge->ProfilingEnabled.store(false);Bridge->CallbackDiagnosticsEnabled.store(false);Bridge->RareTailDiagnosticsEnabled.store(false,std::memory_order_release);}
		Underruns.Enabled.store(false);if(ObserverRegistered){GLog->RemoveOutputDevice(&Underruns);ObserverRegistered=false;}
		if(SettingsChanged){FApp::SetUnfocusedVolumeMultiplier(BackgroundVolume);GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=bAllowBackgroundAudioOrig;GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground=PreviousThrottle;if(LogInterval)LogInterval->Set(PreviousLogInterval,ECVF_SetByCode);SettingsChanged=false;}
	}
	bool Finish(bool Success,const FString& Message)
	{
		Cleanup();if(!Success)Test->AddError(Message);
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3Pressure %s evidence=%s"),*Message,*Directory);
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW3Pressure %s"),Success?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));GEditor->RequestEndPlayMap();return true;
	}
	FAutomationTestBase* Test;double Created,Duration,StageStarted=0,MeasurementStarted=0,NextSample=0,NextMotionUpdate=0,MeasuredSeconds=0;int32 Stage=0;
	bool ContinuousReverb=true,CallbackTraceRequested=false,RareTailRequested=false,RareTailSelfTest=false;
	const bool DropWet;bool WetDropDetected=false,WetRecoveryObserved=false;
	const bool StaleProbe;bool StaleInjected=false;uint64 StaleN0=0,StaleD0=0,StaleR0=0;
	uint64 LastReverbCalls=0,LastReverbDry=0,LastReverbRaw=0,LastReverbAudible=0;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;TWeakObjectPtr<APlayerController> Listener;
	TArray<TWeakObjectPtr<UAudioComponent>> Sources;TArray<TWeakObjectPtr<USoundBase>> Sounds;TArray<int16> PCM;
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Bridge;FIMAcousticMetaSoundContextPtr MetaContext;FIMAcousticUnderrunObserver Underruns;
	FString Directory,CSV;bool ObserverRegistered=false,SettingsChanged=false,Recording=false,PreviousThrottle=false,bAllowBackgroundAudioOrig=false;float BackgroundVolume=1,PreviousLogInterval=0;
	IConsoleVariable* LogInterval=nullptr;uint64 RejectedStart=0,ReverbRejectedStart=0,DryDroppedStart=0,RenderedStart=0,StartMemory=0,PeakMemory=0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3Pressure,"IceMoon.AcousticField.W3.Pressure16x600",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3Pressure::RunTest(const FString&)
{
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3Diagnostic,"IceMoon.AcousticField.W3.Diagnostic16x30",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3Diagnostic::RunTest(const FString&)
{
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this,30));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3WetDrop,"IceMoon.AcousticField.W3.WetDropCounterexample",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3WetDrop::RunTest(const FString&)
{
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this,30,true));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3StaleDrain,"IceMoon.AcousticField.W3.StaleDrainProbe",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3StaleDrain::RunTest(const FString&)
{
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this,45,false,true));return true;
}
#endif
