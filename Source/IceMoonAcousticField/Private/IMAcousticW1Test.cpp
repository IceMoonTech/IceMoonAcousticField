#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSpatialization.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Audio.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
// bAllowBackgroundAudio twin of the Lifecycle/W3 fix: a backgrounded editor
// otherwise feeds zeros to a playing+active voice (frozen queue, zero input).
#include "Settings/LevelEditorMiscSettings.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/SecureHash.h"
#include "Interfaces/IPluginManager.h"
#include "Sound/SoundWaveProcedural.h"
#include "Tests/AutomationEditorCommon.h"
#include "IMAcousticSimulation.h"
#include "IMAcousticTestSupport.h"

namespace IMAcousticW1TestPrivate
{
constexpr const TCHAR* W1Map=TEXT("/IceMoonAcousticField/Tests/IM_W1Door");
float W1OriginalBackgroundVolume=1;
static bool W1OriginalBackgroundAudio=false;
FString W1Evidence()
{
	// PCM writer interprets relative paths beneath Saved/BouncedWavFiles, unlike
	// FileHelper. Resolve once so binary snapshots and device WAVs share a root.
	static const FString Path=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W1-UE"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	return Path;
}
// W1 causal-probe export. GT/test-end only (called from Finish, never from an
// audio callback). Bounded: at most ProbeBlockCapacity block rows plus
// ProbeSnapshotCapacity snapshot rows plus ProbeWorkerCapacity worker rows,
// overflow counters, and this run route WAV windows. Never changes the test
// verdict; a missing bridge is recorded as missing instead of failing export.
// Completion protocol: export reads only slots whose Done flag is complete
// (acquire); any claimed-but-incomplete slot is an explicit evidence gap, and
// any nonzero overflow drops the END of the timeline (start preserved), so the
// run is evidence-INCONCLUSIVE for full causality.
static uint64 W1ExportComplete(const std::atomic<uint64>* Done, uint32 Capacity, uint64 Pushes)
{
	const uint64 Limit = Pushes < Capacity ? Pushes : Capacity;
	for (uint64 I = 0; I < Limit; ++I)
	{
		if (Done[I].load(std::memory_order_acquire) != I + 1) { return I; }
	}
	return Limit;
}
static void W1ExportProbeTrace(FIMAcousticDeviceBridge* Bridge, const TArray<FString>& RouteWindows, const FString& EvidenceDir,
	const FString& RunContextJson = TEXT("{}"))
{
	IFileManager::Get().MakeDirectory(*EvidenceDir, true);
	FFileHelper::SaveStringToFile(RunContextJson, *FPaths::Combine(EvidenceDir, TEXT("IM_run_context.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!Bridge)
	{
		FFileHelper::SaveStringToFile(TEXT("{\"available\":false,\"reason\":\"no device bridge observed\"}"),
			*FPaths::Combine(EvidenceDir, TEXT("IM_probe_trace_missing.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return;
	}
	const uint64 BlockPushes = Bridge->BlockProbePushes.load(std::memory_order_acquire);
	const uint64 BlockOverflow = Bridge->BlockProbeOverflows.load(std::memory_order_relaxed);
	const uint64 SnapPushes = Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
	const uint64 SnapOverflow = Bridge->SnapshotProbeOverflows.load(std::memory_order_relaxed);
	const uint64 WorkerPushes = Bridge->WorkerProbePushes.load(std::memory_order_acquire);
	const uint64 WorkerOverflow = Bridge->WorkerProbeOverflows.load(std::memory_order_relaxed);
	const uint64 BlockComplete = W1ExportComplete(Bridge->BlockDone.data(), FIMAcousticDeviceBridge::ProbeBlockCapacity, BlockPushes);
	const uint64 SnapComplete = W1ExportComplete(Bridge->SnapshotDone.data(), FIMAcousticDeviceBridge::ProbeSnapshotCapacity, SnapPushes);
	const uint64 WorkerComplete = W1ExportComplete(Bridge->WorkerDone.data(), FIMAcousticDeviceBridge::ProbeWorkerCapacity, WorkerPushes);
	FString Blocks;
	Blocks.Reserve(128 * 1024);
	Blocks += TEXT("block,voice,cb_audio_id,result_audio_id,result_world,result_gen,result_seq,snapshot_captured,consumed,age_ms,lis_x,lis_y,lis_z,dir_x,dir_y,dir_z,occlusion,dist_gain,direct_flags,routes,direct_valid,path_valid,reject,reject_detail,render_failure,fallback,reset_reason,input_e,direct_e,path_e,output_e,rendered_at,rejected_at\n");
	for (uint64 I = 0; I < BlockComplete; ++I)
	{
		const FIMAcousticBlockProbe& E = Bridge->BlockProbes[I];
		Blocks += FString::Printf(TEXT("%llu,%u,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.3f,%.6f,%.6f,%.6f,%.4f,%.4f,%.4f,%.6f,%.6f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%llu,%llu\n"),
			E.Block, E.Voice, E.CallbackAudioComponentId, E.ResultAudioComponentId, E.ResultWorldGeneration,
			E.ResultVoiceGeneration, E.ResultSequence, E.SnapshotCaptured, E.ConsumedSeconds, E.AgeMs,
			E.ListenerX, E.ListenerY, E.ListenerZ, E.DirX, E.DirY, E.DirZ, E.Occlusion, E.DistanceGain,
			E.DirectFlags, E.Routes, E.DirectValid, E.PathValid, uint32(E.Reject), E.RejectDetail, E.RenderFailure, E.Fallback,
			E.ResetReason, E.InputEnergy, E.DirectEnergy, E.PathEnergy, E.OutputEnergy, E.RenderedAt, E.RejectedAt);
	}
	FFileHelper::SaveStringToFile(Blocks, *FPaths::Combine(EvidenceDir, TEXT("IM_probe_blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString Snaps;
	Snaps.Reserve(64 * 1024);
	Snaps += TEXT("block,world,captured,submit,lis_ue_x,lis_ue_y,lis_ue_z,lis_sdk_x,lis_sdk_y,lis_sdk_z,src0_sdk_x,src0_sdk_y,src0_sdk_z,src0_audio_id,num_sources,submitted,fail_code\n");
	for (uint64 I = 0; I < SnapComplete; ++I)
	{
		const FIMAcousticSnapshotProbe& S = Bridge->SnapshotProbes[I];
		Snaps += FString::Printf(TEXT("%llu,%llu,%.6f,%.6f,%.2f,%.2f,%.2f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%u,%u,%u\n"),
			S.Block, S.WorldGeneration, S.CapturedSeconds, S.SubmitSeconds, S.ListenerUEX, S.ListenerUEY, S.ListenerUEZ,
			S.ListenerSDKX, S.ListenerSDKY, S.ListenerSDKZ, S.Source0X, S.Source0Y, S.Source0Z, S.Source0AudioId,
			S.NumSources, S.Submitted, S.FailCode);
	}
	FFileHelper::SaveStringToFile(Snaps, *FPaths::Combine(EvidenceDir, TEXT("IM_probe_snapshots.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString Workers;
	Workers.Reserve(64 * 1024);
	Workers += TEXT("block,world,loop_start,snap_captured,snap_valid,snap_reason,num_inputs,published,eval_start,eval_end,eval_ok,reverb_attempt,reverb_ok,reverb_start,reverb_end,reverb_seq,reverb_captured,push_at,push_ok,wait_end,voice,audio_id,voice_gen,seq,direct_flags,occlusion,dist_gain\n");
	for (uint64 I = 0; I < WorkerComplete; ++I)
	{
		const FIMAcousticWorkerProbe& R = Bridge->WorkerProbes[I];
		Workers += FString::Printf(TEXT("%llu,%llu,%.6f,%.6f,%u,%u,%u,%u,%.6f,%.6f,%u,%u,%u,%.6f,%.6f,%llu,%.6f,%.6f,%u,%.6f,%u,%llu,%llu,%llu,%u,%.6f,%.6f\n"),
			R.Block, R.WorldGeneration, R.LoopStartSeconds, R.SnapshotCaptured, R.SnapValid, R.SnapReason, R.NumInputs, R.ResultsPublished,
			R.EvalStartSeconds, R.EvalEndSeconds, R.EvalOk, R.ReverbAttempt, R.ReverbOk, R.ReverbStartSeconds, R.ReverbEndSeconds,
			R.ReverbSequence, R.ReverbCaptured, R.PushSeconds, R.PushOk, R.WaitEndSeconds, R.FirstVoice, R.FirstAudioId, R.FirstVoiceGen, R.FirstSeq,
			R.DirectFlags, R.Occlusion, R.DistanceGain);
	}
	FFileHelper::SaveStringToFile(Workers, *FPaths::Combine(EvidenceDir, TEXT("IM_probe_workers.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString PlBase;
	if (auto Pl = IPluginManager::Get().FindPlugin(TEXT("IceMoonAcousticField"))) { PlBase = Pl->GetBaseDir(); }
	auto HashOne = [](const FString& P)->FString { return LexToString(FMD5Hash::HashFile(*P)); };
	const FString SrcRoot = FPaths::Combine(PlBase, TEXT("Source/IceMoonAcousticField/Private"));
	FString FP = TEXT("{\"scene\":\"") + HashOne(FPaths::Combine(EvidenceDir, TEXT("scene.bin"))) + TEXT("\",\"probes\":\"") + HashOne(FPaths::Combine(EvidenceDir, TEXT("probes.bin"))) + TEXT("\"");
	FP += TEXT(",\"dll\":\"") + HashOne(FPaths::Combine(PlBase, TEXT("Binaries/ThirdParty/SteamAudio/Win64/phonon.dll"))) + TEXT("\"");
	FP += TEXT(",\"h\":\"") + HashOne(FPaths::Combine(SrcRoot, TEXT("IMAcousticSpatialization.h"))) + TEXT("\",\"bake\":\"") + HashOne(FPaths::Combine(SrcRoot, TEXT("IMAcousticBakeVolume.cpp"))) + TEXT("\"");
	FP += TEXT(",\"worker\":\"") + HashOne(FPaths::Combine(SrcRoot, TEXT("IMAcousticSimulationWorker.cpp"))) + TEXT("\",\"spatial\":\"") + HashOne(FPaths::Combine(SrcRoot, TEXT("IMAcousticSpatialization.cpp"))) + TEXT("\"");
	FP += TEXT(",\"w1\":\"") + HashOne(FPaths::Combine(SrcRoot, TEXT("IMAcousticW1Test.cpp"))) + TEXT("\"}");
	FFileHelper::SaveStringToFile(FP, *FPaths::Combine(EvidenceDir, TEXT("IM_fingerprints.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString Windows = TEXT("[");
	for (int32 W = 0; W < RouteWindows.Num(); ++W) { if (W > 0) Windows += TEXT(","); Windows += RouteWindows[W]; }
	Windows += TEXT("]");
	const uint64 BlockLimit = BlockPushes < FIMAcousticDeviceBridge::ProbeBlockCapacity ? BlockPushes : FIMAcousticDeviceBridge::ProbeBlockCapacity;
	const uint64 SnapLimit = SnapPushes < FIMAcousticDeviceBridge::ProbeSnapshotCapacity ? SnapPushes : FIMAcousticDeviceBridge::ProbeSnapshotCapacity;
	const uint64 WorkerLimit = WorkerPushes < FIMAcousticDeviceBridge::ProbeWorkerCapacity ? WorkerPushes : FIMAcousticDeviceBridge::ProbeWorkerCapacity;
	const bool ProbeComplete = (BlockOverflow == 0) && (SnapOverflow == 0) && (WorkerOverflow == 0)
		&& (BlockComplete == BlockLimit) && (SnapComplete == SnapLimit) && (WorkerComplete == WorkerLimit);
	const FString Summary = FString::Printf(TEXT("{\"available\":true,\"complete\":%s,\"block_pushes\":%llu,\"block_exported\":%llu,\"block_overflow\":%llu,\"snapshot_pushes\":%llu,\"snapshot_exported\":%llu,\"snapshot_overflow\":%llu,\"worker_pushes\":%llu,\"worker_exported\":%llu,\"worker_overflow\":%llu,\"route_windows\":%s}"),
		ProbeComplete ? TEXT("true") : TEXT("false"), BlockPushes, BlockComplete, BlockOverflow, SnapPushes, SnapComplete, SnapOverflow, WorkerPushes, WorkerComplete, WorkerOverflow, *Windows);
	FFileHelper::SaveStringToFile(Summary, *FPaths::Combine(EvidenceDir, TEXT("IM_probe_summary.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticW1ProbeExport complete=%d blocks=%llu/%llu overflow=%llu snaps=%llu/%llu overflow=%llu workers=%llu/%llu overflow=%llu evidence=%s"),
		ProbeComplete ? 1 : 0, BlockComplete, BlockPushes, BlockOverflow, SnapComplete, SnapPushes, SnapOverflow, WorkerComplete, WorkerPushes, WorkerOverflow, *EvidenceDir);
}

static void W1ExportMetaSoundTrace(const FIMAcousticMetaSoundContextPtr& Context, const FString& EvidenceDir)
{
	if (!Context.IsValid())
	{
		FFileHelper::SaveStringToFile(TEXT("{\"available\":false,\"reason\":\"no MetaSound context observed\"}"),
			*FPaths::Combine(EvidenceDir, TEXT("IM_metasound_trace_missing.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return;
	}
	const uint32 SourcePushes=Context->CapturedSourceBlockCount.load(std::memory_order_acquire);
	const uint32 SourceLimit=FMath::Min<uint32>(SourcePushes,uint32(Context->CapturedSourceBlocks.Num()));
	uint32 SourceAccepted=0,SourcePath=0,SourceRejected=0;
	for(uint32 I=0;I<SourceLimit;++I)
	{
		const FIMAcousticBlockProbe& E=Context->CapturedSourceBlocks[int32(I)];
		if(E.Reject!=EIMAcousticProbeReject::Accepted){++SourceRejected;continue;}
		++SourceAccepted;
		if((E.Routes&2u)!=0&&E.OutputEnergy>1e-9)++SourcePath;
	}
	const uint32 EnvPushes=Context->CapturedBlockCount.load(std::memory_order_acquire);
	const uint32 EnvLimit=FMath::Min<uint32>(EnvPushes,uint32(Context->CapturedBlocks.Num()));
	const uint32 EnvFrames=Context->CapturedEnvironmentFrames.load(std::memory_order_acquire);
	uint32 WetBlocks=0;
	for(uint32 I=0;I<EnvLimit;++I)
	{
		const FIMAcousticMetaSoundBlock& E=Context->CapturedBlocks[int32(I)];
		const uint32 Begin=uint32(E.Frame);
		const uint32 End=FMath::Min<uint32>(Begin+FIMAcousticMetaSoundContext::Frames,EnvFrames);
		bool bNonzero=false;
		for(uint32 F=Begin;F<End;++F)
		{
			if(Context->CapturedWet.IsValidIndex(int32(2*F))
				&& (FMath::Abs(Context->CapturedWet[int32(2*F)])>1e-9f||FMath::Abs(Context->CapturedWet[int32(2*F+1)])>1e-9f))
			{bNonzero=true;break;}
		}
		if(bNonzero)++WetBlocks;
	}
	const FString Summary=FString::Printf(TEXT("{\"available\":true,\"telemetry\":\"MetaSound.CapturedSourceBlocks+CapturedBlocks\",\"source_blocks\":%u,\"source_capacity\":%d,\"source_accepted\":%u,\"source_path_nonzero\":%u,\"source_rejected\":%u,\"environment_blocks\":%u,\"environment_wet_nonzero\":%u,\"environment_frames\":%u,\"legacy_block_pushes\":%llu}"),
		SourceLimit,Context->CapturedSourceBlocks.Num(),SourceAccepted,SourcePath,SourceRejected,EnvLimit,WetBlocks,EnvFrames,
		Context->Device.IsValid()?Context->Device->BlockProbePushes.load(std::memory_order_relaxed):0);
	FFileHelper::SaveStringToFile(Summary,*FPaths::Combine(EvidenceDir,TEXT("IM_metasound_trace_summary.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
bool BuildW1Map(FString& Error)
{
	if(FPackageName::DoesPackageExist(W1Map))
		return FEditorFileUtils::LoadMap(W1Map,false,true);
	UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
	auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
	if(!World||!Cube){Error=TEXT("Cannot create W1 fixture world or load engine cube.");return false;}
	auto Box=[World,Cube](FVector Lo,FVector Hi)
	{
		const FVector Center=(Lo+Hi)*.5,Size=Hi-Lo;
		auto* Actor=World->SpawnActor<AStaticMeshActor>();
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		// Fixture uses SDK axes in meters; convert into UE centimeter transforms.
		Actor->SetActorLocation(FVector(-Center.Z,Center.X,Center.Y)*100);
		Actor->SetActorScale3D(FVector(Size.Z,Size.X,Size.Y));
		Actor->Tags.Add(TEXT("IMW1Fixture"));
	};
	Box({-4.1,-.1,-3.1},{4.1,0,3.1});Box({-4.1,3,-3.1},{4.1,3.1,3.1});
	Box({-4.1,0,-3.1},{-4,3,3.1});Box({4,0,-3.1},{4.1,3,3.1});
	Box({-4,0,-3.1},{4,3,-3});Box({-4,0,3},{4,3,3.1});
	Box({-.05,0,-3},{.05,3,.5});Box({-.05,0,2.5},{.05,3,3});Box({-.05,2.5,.5},{.05,3,2.5});
	auto* Volume=World->SpawnActor<AIMAcousticBakeVolume>();
	Volume->SetActorLocation(FVector(0,0,150));Volume->BakeBounds->SetBoxExtent(FVector(320,420,170));
	FIMAcousticMaterialMapping Material;Material.Material=Cube->GetMaterial(0);Material.Absorption=FVector(.25);Volume->Materials.Add(Material);
	const FString File=FPackageName::LongPackageNameToFilename(W1Map,FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File),true);
	if(!FEditorFileUtils::SaveLevel(World->PersistentLevel,File)){Error=TEXT("Cannot save plugin-owned W1 fixture map.");return false;}
	return true;
}

class FIMAcousticW1Command final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticW1Command(FAutomationTestBase* InTest,int32 InitialStage=0):Test(InTest),Started(FPlatformTime::Seconds()),Stage(InitialStage){}
	bool Update() override
	{
		const double Now=FPlatformTime::Seconds();
		if(Now-Started>120)return Finish(false,TEXT("W1 UE gate timed out."));
		if(Stage==0)
		{
			UWorld* World=GEditor->GetEditorWorldContext().World();
			for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It){if(Volume.IsValid())return Finish(false,TEXT("W1 fixture has multiple bake volumes."));Volume=*It;}
			if(!Volume.IsValid())return Finish(false,TEXT("W1 fixture missing production bake volume."));
			// Existing fixture maps are reused, never reconstructed or cleared.
			Volume->GenerateProbes();
			if(Volume->GeneratedProbes<=0)
			{
				for(const FString& Issue:Volume->SceneIssues)UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticW1Scene %s"),*Issue);
				return Finish(false,Volume->Status);
			}
			PreviousAsset=Volume->BakedField;Volume->Bake();Stage=1;return false;
		}
		if(Stage==1)
		{
			if(!Volume.IsValid())return Finish(false,TEXT("Bake owner destroyed."));
			if(Volume->Status.Contains(TEXT("failed"),ESearchCase::IgnoreCase)||Volume->Status.Contains(TEXT("discarded")))return Finish(false,Volume->Status);
			if(!Volume->BakedField||Volume->BakedField.Get()==PreviousAsset.Get())return false;
			IFileManager::Get().MakeDirectory(*W1Evidence(),true);
			FFileHelper::SaveArrayToFile(Volume->BakedField->SceneData,*FPaths::Combine(W1Evidence(),TEXT("scene.bin")));
			FFileHelper::SaveArrayToFile(Volume->BakedField->ProbeData,*FPaths::Combine(W1Evidence(),TEXT("probes.bin")));
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Bake triangles=%d probes=%d evidence=%s"),Volume->ExportedTriangles,Volume->GeneratedProbes,*W1Evidence());
			const FString File=FPackageName::LongPackageNameToFilename(W1Map,FPackageName::GetMapPackageExtension());
			if(!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel,File))return Finish(false,TEXT("Cannot persist W1 bake binding."));
			FString Error;GUnrealEd->AutomationLoadMap(W1Map,false,&Error);
			if(!Error.IsEmpty())return Finish(false,Error);
			// AutomationLoadMap queues PIE startup. Yield this command completely,
			// then resume behind those commands; waiting here would deadlock the queue.
			ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticW1Command(Test,2));
			return true;
		}
		UWorld* PIE=nullptr;
		for(const auto& Context:GEngine->GetWorldContexts())if(Context.WorldType==EWorldType::PIE){PIE=Context.World();break;}
		if(!PIE)return Stage==2?false:Finish(false,TEXT("PIE ended before audio evidence."));
		// Chunked GT replenishment (W3-proven): bulk upfront queues freeze
		// intermittently in unattended editors with playing=1/active=1 and a
		// full unconsumed queue. 2s chunks wrap seamlessly (233/997/3109Hz all
		// complete integer cycles in 2s); signal, gates and timing unchanged.
		if(Stage>=2)FeedAudio();
		if(Stage==2)
		{
			for(TActorIterator<AIMAcousticBakeVolume> It(PIE);It;++It)Volume=*It;
			if(!Volume.IsValid())return Finish(false,TEXT("PIE bake volume missing."));
			APlayerController* Listener=PIE->GetFirstPlayerController();if(!Listener)return false;
			MovingListener=Listener;
			// Branch A init order: set the Route0 override first, then wait for
			// the actual device-side listener (GT snapshot ring) to converge
			// BEFORE the source exists. Position-only convergence is enough:
			// override position+rotation are one call/one struct picked up by
			// the device in the same frame, so converged position implies the
			// -90deg rotation was picked up too (no independent rotation race).
			Listener->SetAudioListenerOverride(nullptr,FVector(0,200,150),FRotator(0,-90,0));
			auto InitBridge=IMAcousticTestSupport::FindBridge(PIE);
			if(!InitBridge.IsValid())return false; // device not up yet; the 120s total cap bounds this wait.
			if(!ProbeBridge.IsValid())ProbeBridge=InitBridge;
			if(!ListenerConverged(InitBridge))
			{
				if(Now-LastDiagnostic>5)
				{
					LastDiagnostic=Now;
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1ListenerWait pre-spawn snaps=%llu"),InitBridge->SnapshotProbePushes.load(std::memory_order_acquire));
				}
				return false;
			}
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1InitReady t=%.3f snaps=%llu"),Now-Started,InitBridge->SnapshotProbePushes.load(std::memory_order_acquire));
			if(!SpawnW1Source(PIE))return Finish(false,TEXT("W1 source spawn failed."));
			if(FAudioDevice* AudioDevice=PIE->GetAudioDeviceRaw())
				MetaContext=IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
			GraphGateSourceBefore=GraphSourceBlocks();
			GraphGateEnvironmentBefore=GraphEnvironmentBlocks();
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1InitSpawn t=%.3f"),Now-Started);
			Stage=3;return false;
		}
		auto Bridge=IMAcousticTestSupport::FindBridge(PIE);
		if(!Bridge)return Finish(false,TEXT("W1 has no IceMoon audio device bridge; configure plugin before launching."));
		if(!ProbeBridge.IsValid() || ProbeBridge != Bridge)ProbeBridge=Bridge; // Rebind from the pre-spawn legacy bridge to the graph owner when delayed binding completes.
		if(!MetaContext.IsValid())
		{
			if(FAudioDevice* AudioDevice=PIE->GetAudioDeviceRaw())
				MetaContext=IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
		}
		if(MetaContext.IsValid()&&GraphGateSourceBefore==0)
		{
			GraphGateSourceBefore=GraphSourceBlocks();
			GraphGateEnvironmentBefore=GraphEnvironmentBlocks();
		}
		if(Stage==3)
		{
			if(Now-LastDiagnostic>15)
			{
				LastDiagnostic=Now;
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Waiting rendered=%llu rejected=%llu direct=%llu path=%llu epoch=%llu"),Bridge->RenderedBlocks.load(),Bridge->RejectedBlocks.load(),Bridge->DirectNonzeroBlocks.load(),Bridge->PathNonzeroBlocks.load(),Bridge->WorldGeneration.load());
				for(TActorIterator<AIMAcousticBakeVolume> It(PIE);It;++It)
				{
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Status %s"),*It->Status);
					for(const FString& Issue:It->SceneIssues)UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Issue %s"),*Issue);
				}
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Reverb calls=%llu dry=%llu raw=%llu dropped=%llu wet_gain=%g push=%llu/nodirect=%llu/zerogain=%llu/inputnz=%llu notfresh=%llu rinputnz=%llu"),Bridge->ReverbProcessedBlocks.load(),Bridge->ReverbDryBlocks.load(),Bridge->ReverbRawNonzeroBlocks.load(),Bridge->DryDroppedBlocks.load(),Volume->ReverbWetGain,Bridge->PushDryCalls.load(),Bridge->PushDryInvalidDirect.load(),Bridge->PushDryZeroGain.load(),Bridge->PushDryInputNonzero.load(),Bridge->ReverbNotFreshBlocks.load(),Bridge->RenderInputNonzero.load());
				if(MovingSource.IsValid())
				{auto* VA=MovingSource.Get();auto* VW=Cast<USoundWaveProcedural>(VA->Sound);
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Voice voices=%d playing=%d active=%d queuebytes=%d"),Bridge->Voices.Num(),int(VA->IsPlaying()),int(VA->IsActive()),VW?VW->GetAvailableAudioByteCount():-1);}
			}
			if(MetaContext.IsValid())
			{
				uint64 GraphDirect=0,GraphPath=0,GraphRejected=0;
				GraphSourceWindow(GraphGateSourceBefore,GraphSourceBlocks(),GraphDirect,GraphPath,GraphRejected);
				const uint64 GraphWet=GraphWetWindow(GraphGateEnvironmentBefore,GraphEnvironmentBlocks());
				if(Now-LastDiagnostic>5)
				{
					LastDiagnostic=Now;
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Waiting graph_source=%u accepted_direct=%llu path=%llu rejected=%llu graph_env=%u wet=%llu legacy_path=%llu legacy_reverb=%llu"),GraphSourceBlocks(),GraphDirect,GraphPath,GraphRejected,GraphEnvironmentBlocks(),GraphWet,Bridge->PathNonzeroBlocks.load(),Bridge->ReverbNonzeroBlocks.load());
				}
				if(GraphPath<4||GraphWet<4)return false;
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1GatePass graph_path=%llu graph_wet=%llu legacy_path=%llu legacy_reverb=%llu"),GraphPath,GraphWet,Bridge->PathNonzeroBlocks.load(),Bridge->ReverbNonzeroBlocks.load());
			}
			else
			{
				// Legacy fallback is kept only for an actually absent graph
				// context; ordinary W1 runs must take the branch above.
				if(!W1Init){W1Init=true;W1In0=Bridge->PushDryInputNonzero.load();W1T0=Now;}
				if(W1Re<2&&Bridge->PushDryInputNonzero.load()==W1In0&&Now-W1T0>10)
				{++W1Re;W1Init=false;if(auto* Old=MovingSource.Get()){if(auto* OA=Old->GetOwner())OA->Destroy();}MovingSource.Reset();
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Respawn n=%d"),W1Re);
				if(!SpawnW1Source(PIE))return true;return false;}
				if(Bridge->PathNonzeroBlocks.load()<4||Bridge->ReverbNonzeroBlocks.load()<4)return false;
			}
			SetRoute();CaptureStart=Now;Stage=6;return false;
		}
		if(Stage==6)
		{
			// Route0 readiness (test-side only): the first published snapshot can
			// still carry the pre-override listener (direct-leak race). Wait for
			// the latest Done-complete snapshot to report the override target
			// before taking baselines. Other routes keep the legacy 0.3s window
			// only. Verdict logic and counter baselines order unchanged.
			if(RouteIndex==0&&!ListenerConverged(Bridge))
			{
				if(Now-LastDiagnostic>5)
				{
					LastDiagnostic=Now;
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1ListenerWait snaps=%llu rendered=%llu direct=%llu"),Bridge->SnapshotProbePushes.load(std::memory_order_acquire),Bridge->RenderedBlocks.load(std::memory_order_relaxed),Bridge->DirectNonzeroBlocks.load(std::memory_order_relaxed));
				}
				return false;
			}
			if(Now-CaptureStart<.3)return false; // GT mask publication plus DSP block transition.
			DirectBefore=Bridge->DirectNonzeroBlocks.load();PathBefore=Bridge->PathNonzeroBlocks.load();RejectedBefore=Bridge->RejectedBlocks.load();
			ReverbBefore=Bridge->ReverbNonzeroBlocks.load();
			GraphSourceBefore=GraphSourceBlocks();
			GraphEnvironmentBefore=GraphEnvironmentBlocks();
			UAudioMixerBlueprintLibrary::StartRecordingOutput(PIE,3);CaptureStart=Now;Stage=4;return false;
		}
		if(Stage==4)
		{
			if(RouteIndex>=3)MovePair(Now-CaptureStart);
			if(Now-CaptureStart<(RouteIndex>=3?5:2))return false;
			uint64 Direct=Bridge->DirectNonzeroBlocks.load()-DirectBefore,Path=Bridge->PathNonzeroBlocks.load()-PathBefore;
			uint64 Rejected=Bridge->RejectedBlocks.load()-RejectedBefore;
			IFileManager::Get().MakeDirectory(*W1Evidence(),true);
			uint64 Reverb=Bridge->ReverbNonzeroBlocks.load()-ReverbBefore;
			if(MetaContext.IsValid())
			{
				GraphSourceWindow(GraphSourceBefore,GraphSourceBlocks(),Direct,Path,Rejected);
				Reverb=GraphWetWindow(GraphEnvironmentBefore,GraphEnvironmentBlocks());
			}
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1RouteVec route=%s d_direct=%llu d_path=%llu d_reverb=%llu d_rej=%llu push=%llu/inputnz=%llu/rinputnz=%llu notfresh=%llu"),*RouteName(),Direct,Path,Reverb,Rejected,Bridge->PushDryCalls.load(),Bridge->PushDryInputNonzero.load(),Bridge->RenderInputNonzero.load(),Bridge->ReverbNotFreshBlocks.load());
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Intervals route=%s gt_max_us=%llu worker_max_us=%llu stale=%llu missing=%llu"),*RouteName(),Bridge->MaxSnapshotGapUs.load(),Bridge->MaxWorkerGapUs.load(),Bridge->StaleResultBlocks.load(),Bridge->MissingResultBlocks.load());
			UAudioMixerBlueprintLibrary::StopRecordingOutput(PIE,EAudioRecordingExportType::WavFile,RouteName(),W1Evidence());
			const FString Data=FString::Printf(TEXT("{\"scope\":\"UE-open-door-isolated-route\",\"route\":\"%s\",\"direct_nonzero_blocks\":%llu,\"path_nonzero_blocks\":%llu,\"degraded_blocks\":%llu,\"reverb_nonzero_blocks\":%llu}"),*RouteName(),Direct,Path,Rejected,Reverb);
			FFileHelper::SaveStringToFile(Data,*FPaths::Combine(W1Evidence(),RouteName()+TEXT(".json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			// Route WAV window for the probe join: wall-clock start/end of this
			// route recording plus its counter deltas. Bounded (one per route).
			if (ProbeRouteWindows.Num() < 8)
			{
				ProbeRouteWindows.Add(FString::Printf(TEXT("{\"route\":\"%s\",\"capture_start\":%.6f,\"capture_end\":%.6f,\"d_direct\":%llu,\"d_path\":%llu,\"d_reverb\":%llu,\"d_rejected\":%llu}"),
					*RouteName(), CaptureStart, Now, Direct, Path, Reverb, Rejected));
			}
			AudioPass=Direct==0&&Rejected==0&&((RouteIndex==1||RouteIndex>=3)?Path>0:Path==0)&&(RouteIndex==2?Reverb>0:Reverb==0);Stage=5;return false;
		}
		if(Stage==5)
		{
			const FString File=FPaths::Combine(W1Evidence(),RouteName()+TEXT(".wav"));
			if(IFileManager::Get().FileSize(*File)<=44)return false;
			TArray<uint8> Bytes;FWaveModInfo WaveInfo;
			if(!FFileHelper::LoadFileToArray(Bytes,*File)||!WaveInfo.ReadWaveInfo(Bytes.GetData(),Bytes.Num()))return false;
			if(!WaveInfo.pBitsPerSample||*WaveInfo.pBitsPerSample!=16)return Finish(false,TEXT("Unexpected device recording PCM format."));
			double Energy=0,LeftEnergy=0,RightEnergy=0;
			for(uint32 I=0;I+1<WaveInfo.SampleDataSize;I+=2)
			{int16 Sample;FMemory::Memcpy(&Sample,WaveInfo.SampleDataStart+I,sizeof(Sample));const double E=double(Sample)*Sample;Energy+=E;if((I/2)%2)RightEnergy+=E;else LeftEnergy+=E;}
			const double EarBalance=Energy>0?(LeftEnergy-RightEnergy)/Energy:0;
			AudioPass=AudioPass&&(RouteIndex==0?Energy==0:Energy>0)&&WaveInfo.SampleDataSize>96000;
			// The graph's left/right convention may be mirrored relative to the
			// legacy source callback.  The product contract is an audible cue
			// with a stable sign for the front move and the opposite sign after
			// the listener rotates 180 degrees, not a hard-coded absolute side.
			bool CuePass=true;
			if(RouteIndex==1)
			{
				CuePass=FMath::Abs(EarBalance)>.02;
				if(CuePass){RouteCueSign=EarBalance; bRouteCueReference=true;}
			}
			else if(RouteIndex==3)
			{
				CuePass=bRouteCueReference&&FMath::Abs(EarBalance)>.02&&(EarBalance*RouteCueSign>0.0);
			}
			else if(RouteIndex==4)
			{
				CuePass=bRouteCueReference&&FMath::Abs(EarBalance)>.02&&(EarBalance*RouteCueSign<0.0);
			}
			AudioPass=AudioPass&&CuePass;
			FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"pcm16_energy\":%.17g,\"pcm_bytes\":%u,\"ear_balance\":%.9g}"),Energy,WaveInfo.SampleDataSize,EarBalance),*FPaths::Combine(W1Evidence(),RouteName()+TEXT("-energy.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1DirectPath evidence=%s"),*W1Evidence());
			if(!AudioPass)return Finish(false,TEXT("UE route counter/recording gate failed: ")+RouteName());
			if(++RouteIndex<5){SetRoute();CaptureStart=Now;Stage=6;return false;}
			return Finish(true,TEXT("UE direct/path/baked-reverb routes and moving source/listener recordings passed; door cue reverses with listener orientation."));
		}
		return false;
	}
private:
	FString RouteName() const{return RouteIndex==0?TEXT("IM_direct"):RouteIndex==1?TEXT("IM_path"):RouteIndex==2?TEXT("IM_reverb"):RouteIndex==3?TEXT("IM_moving_front"):TEXT("IM_moving_back");}
	void MovePair(double T)
	{
		if(MovingSource.IsValid())MovingSource->SetWorldLocation(FVector(-25*FMath::Sin(T*.7),-200+25*FMath::Sin(T),150));
		if(MovingListener.IsValid())MovingListener->SetAudioListenerOverride(nullptr,FVector(-25*FMath::Cos(T*.9),200+25*FMath::Cos(T),150),FRotator(0,RouteIndex==4?90:-90,0));
	}
	void SetRoute(){Volume->bDirectRoute=RouteIndex==0;Volume->bPathRoute=RouteIndex==1||RouteIndex>=3;Volume->bReverbRoute=RouteIndex==2;if(RouteIndex>=3)MovePair(0);}
	bool SpawnW1Source(UWorld* PIE)
	{
		if(!PIE){Finish(false,TEXT("W1 source spawn failed."));return false;}
		auto* Actor=PIE->SpawnActor<AActor>();if(!Actor){Finish(false,TEXT("W1 source spawn failed."));return false;}
		Actor->SetActorLocation(FVector(0,-200,150));
		auto* Audio=NewObject<UAudioComponent>(Actor);Actor->SetRootComponent(Audio);Actor->AddInstanceComponent(Audio);
		Audio->bAutoActivate=false;Audio->RegisterComponent();Audio->SetWorldLocation(FVector(0,-200,150));
		MovingSource=Audio;
		auto* Source=NewObject<UIMAcousticSourceComponent>(Actor);Actor->AddInstanceComponent(Source);Source->AudioComponent=Audio;Source->RegisterComponent();
		FString Error;
		if(!IMAcousticTestSupport::ConfigureGraphSource(Audio,Error)){Finish(false,Error);return false;}
		if(!Source->ValidateSource(Error)){Finish(false,Error);return false;}
		Audio->Play();return true;
	}
	// Route0 override target in UE cm (see Stage 2 SetAudioListenerOverride).
	// Latest Done-complete GT snapshot must report it within 1e-3 before
	// route0 baselines are taken. Same-thread GT read: producer is GT Tick,
	// this latent update runs on GT. Test-side wait only, never a verdict.
	bool ListenerConverged(const TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe>& InBridge) const
	{
		if(!InBridge.IsValid())return false;
		const uint64 Pushes=InBridge->SnapshotProbePushes.load(std::memory_order_acquire);
		const uint64 Limit=Pushes<FIMAcousticDeviceBridge::ProbeSnapshotCapacity?Pushes:FIMAcousticDeviceBridge::ProbeSnapshotCapacity;
		if(Limit==0)return false;
		for(uint64 I=Limit;I>0;--I)
		{
			const uint64 Idx=I-1;
			if(InBridge->SnapshotDone[Idx].load(std::memory_order_acquire)!=Idx+1)continue;
			const FIMAcousticSnapshotProbe& S=InBridge->SnapshotProbes[Idx];
			return FMath::Abs(double(S.ListenerUEX)-0.0)<1e-3&&FMath::Abs(double(S.ListenerUEY)-200.0)<1e-3&&FMath::Abs(double(S.ListenerUEZ)-150.0)<1e-3;
		}
		return false;
	}
	uint32 GraphSourceBlocks() const
	{
		return MetaContext.IsValid()?MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire):0;
	}
	uint32 GraphEnvironmentBlocks() const
	{
		return MetaContext.IsValid()?MetaContext->CapturedBlockCount.load(std::memory_order_acquire):0;
	}
	void GraphSourceWindow(uint32 Begin,uint32 End,uint64& Direct,uint64& Path,uint64& Rejected) const
	{
		Direct=0;Path=0;Rejected=0;
		if(!MetaContext.IsValid())return;
		const uint32 Limit=FMath::Min<uint32>(End,uint32(MetaContext->CapturedSourceBlocks.Num()));
		const uint32 First=FMath::Min<uint32>(Begin,Limit);
		for(uint32 I=First;I<Limit;++I)
		{
			const FIMAcousticBlockProbe& E=MetaContext->CapturedSourceBlocks[int32(I)];
			if(E.Reject!=EIMAcousticProbeReject::Accepted){++Rejected;continue;}
			if((E.Routes&1u)!=0&&E.OutputEnergy>1e-9)++Direct;
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
	void FeedAudio()
	{
		if(FeedPCM.Num()==0||!MovingSource.IsValid())return;
		auto* Audio=MovingSource.Get();auto* Wave=Cast<USoundWaveProcedural>(Audio->Sound);
		if(!Wave)return;
		static constexpr int32 ChunkSamples=48000*2;
		int32 Fed=0;
		while(Wave->GetAvailableAudioByteCount()<ChunkSamples*sizeof(int16)&&Fed<ChunkSamples*2)
		{
			TArray<int16> Chunk;Chunk.SetNumUninitialized(ChunkSamples);
			for(int32 I=0;I<ChunkSamples;++I)Chunk[I]=FeedPCM[(FeedCursor+I)%FeedPCM.Num()];
			Wave->QueueAudio(reinterpret_cast<const uint8*>(Chunk.GetData()),ChunkSamples*sizeof(int16));
			FeedCursor=(FeedCursor+ChunkSamples)%FeedPCM.Num();Fed+=ChunkSamples;
		}
	}
	bool Finish(bool Pass,const FString& Message)
	{
		// Causal-probe handoff: export the bounded GT/worker/audio trace to
		// this run evidence dir before EndPlay tears down the world/bridge.
		// Zero verdict behavior: export-only, never flips the W1 gate result.
		FVector ActL(0, 0, 0), ActS(0, 0, 0);
		int32 QBytes = -1;
		if (MovingListener.IsValid()) { if (const APawn* LP = MovingListener->GetPawn()) { ActL = LP->GetActorLocation(); } }
		if (MovingSource.IsValid()) { ActS = MovingSource->GetComponentLocation(); if (auto* VW = Cast<USoundWaveProcedural>(MovingSource->Sound)) { QBytes = VW->GetAvailableAudioByteCount(); } }
		uint64 CRen = 0, CRej = 0, CDir = 0, CPath = 0;
		if (ProbeBridge.IsValid()) { CRen = ProbeBridge->RenderedBlocks.load(); CRej = ProbeBridge->RejectedBlocks.load(); CDir = ProbeBridge->DirectNonzeroBlocks.load(); CPath = ProbeBridge->PathNonzeroBlocks.load(); }
		const FString RC = FString::Printf(TEXT("{\"started\":%.6f,\"route_index\":%d,\"route_windows\":%d,\"feed_cursor\":%d,\"feed_total\":%d,\"queue_bytes\":%d,\"target_lis\":[0,200,150],\"actual_lis\":[%.2f,%.2f,%.2f],\"actual_src\":[%.2f,%.2f,%.2f],\"rendered\":%llu,\"rejected\":%llu,\"direct\":%llu,\"path\":%llu}"),
			Started, RouteIndex, ProbeRouteWindows.Num(), FeedCursor, FeedPCM.Num(), QBytes, ActL.X, ActL.Y, ActL.Z, ActS.X, ActS.Y, ActS.Z, CRen, CRej, CDir, CPath);
		if (ProbeBridge.IsValid()) { W1ExportProbeTrace(ProbeBridge.Get(), ProbeRouteWindows, W1Evidence(), RC); }
		else { W1ExportProbeTrace(nullptr, ProbeRouteWindows, W1Evidence(), RC); }
		W1ExportMetaSoundTrace(MetaContext,W1Evidence());
		if(!Pass)Test->AddError(Message);else Test->AddInfo(Message);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1DirectPath %s %s"),Pass?TEXT("PASS"):TEXT("FAIL"),*Message);
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW1 %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
		FApp::SetUnfocusedVolumeMultiplier(W1OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=W1OriginalBackgroundAudio;
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		if(GUnrealEd)GUnrealEd->RequestEndPlayMap();return true;
	}
	FAutomationTestBase* Test;
	double Started,CaptureStart=0,LastDiagnostic=0;
	int32 Stage=0;
	int32 RouteIndex=0;
	uint64 W1In0=0;double W1T0=0;int32 W1Re=0;bool W1Init=false;
	TArray<int16> FeedPCM;int32 FeedCursor=0;
	uint64 DirectBefore=0,PathBefore=0,RejectedBefore=0;
	uint64 ReverbBefore=0;
	uint32 GraphGateSourceBefore=0,GraphGateEnvironmentBefore=0,GraphSourceBefore=0,GraphEnvironmentBefore=0;
	bool AudioPass=false;
	double RouteCueSign=0.0;
	bool bRouteCueReference=false;
	TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> ProbeBridge; // Finish-time export only; never used on audio threads.
	FIMAcousticMetaSoundContextPtr MetaContext;
	TArray<FString> ProbeRouteWindows; // Bounded route WAV windows for the probe join (set in Stage4).
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<UIMAcousticBakeAsset> PreviousAsset;
	TWeakObjectPtr<UAudioComponent> MovingSource;
	TWeakObjectPtr<APlayerController> MovingListener;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW1Test,"IceMoon.AcousticField.W1.DirectPath",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ProductFilter)
bool FIMAcousticW1Test::RunTest(const FString&)
{
	FString Error;if(!IMAcousticW1TestPrivate::BuildW1Map(Error)){AddError(Error);return false;}
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	IMAcousticW1TestPrivate::W1OriginalBackgroundVolume=FApp::GetUnfocusedVolumeMultiplier();
	FApp::SetUnfocusedVolumeMultiplier(1); // Deterministic audio in this owned unattended Editor only.
	IMAcousticW1TestPrivate::W1OriginalBackgroundAudio=GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=true;
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW1TestPrivate::FIMAcousticW1Command(this));return true;
}
// H1 W1 product-side readback proof (mechanism, not an acoustic claim).
namespace IMAcousticW1TestPrivate
{
IPLCoordinateSpace3 H1POFrame(float OX, float OY, float OZ)
{
	IPLCoordinateSpace3 F{};
	F.right = {1.0f, 0.0f, 0.0f};
	F.up = {0.0f, 0.0f, 1.0f};
	F.ahead = {0.0f, 1.0f, 0.0f};
	F.origin = {OX, OY, OZ};
	return F;
}
void H1POBoxRoom(FIMAcousticSceneInput& Out)
{
	// Closed box x in [-3,3], y in [-2,2], z in [0,3], outward winding, meters.
	Out.Vertices = {{-3,-2,0},{3,-2,0},{3,2,0},{-3,2,0},{-3,-2,3},{3,-2,3},{3,2,3},{-3,2,3}};
	const int T[][3] = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,5,4},{0,1,5},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
	for (const auto& R : T) { IPLTriangle Tr{}; Tr.indices[0]=R[0]; Tr.indices[1]=R[1]; Tr.indices[2]=R[2]; Out.Triangles.push_back(Tr); }
	IPLMaterial M{};
	M.absorption[0]=0.2f; M.absorption[1]=0.25f; M.absorption[2]=0.3f;
	M.scattering=0.1f;
	Out.Materials.push_back(M);
	Out.MaterialIndices.assign(Out.Triangles.size(), 0);
	Out.Probes = {{{-1.0f,0.0f,1.5f},0.6f},{{1.0f,0.0f,1.5f},0.6f}};
}
FString H1PORbJson(const FIMAcousticPathingReadback& R)
{
	return FString::Printf(TEXT("{\"validation\":%d,\"alternates\":%d,\"applied_to_sdk\":%d,\"at\":\"%s\",\"apply_count\":%llu,\"sources\":%llu,\"key\":%llu,\"gen\":%llu}"),
		R.EnableValidationApplied?1:0, R.FindAlternatePathsApplied?1:0, R.AppliedToSdk?1:0,
		ANSI_TO_TCHAR(R.AppliedAt.c_str()), R.ApplyCount, R.AppliedSources, R.LastSourceKey, R.LastGeneration);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPathingOptionsTest, "IceMoon.AcousticField.H1.PathingOptions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FIMAcousticPathingOptionsTest::RunTest(const FString&)
{
	std::string Error;
	// 1. Fresh simulator: missing options fail closed with the field named (no silent default).
	FIMAcousticSimulation Sim;
	std::vector<FIMAcousticSourceInput> In(1);
	In[0].SourceKey = 7; In[0].Generation = 1; In[0].Source = IMAcousticW1TestPrivate::H1POFrame(2.0f, 0.0f, 1.5f);
	std::vector<FIMAcousticAudioFrame> OutFrames;
	if (Sim.EvaluateBatch(In, IMAcousticW1TestPrivate::H1POFrame(-2.0f, 0.0f, 1.5f), OutFrames, Error)) { AddError(TEXT("EvaluateBatch without options must fail.")); return false; }
	const FString FreshEvalErr = ANSI_TO_TCHAR(Error.c_str());
	if (!FreshEvalErr.Contains(TEXT("enableValidation"))) { AddError(FString::Printf(TEXT("EvaluateBatch gate must name the field, got: %s"), *FreshEvalErr)); return false; }
	FIMAcousticBakeData Empty;
	if (Sim.Load(Empty, 48000, 512, Error)) { AddError(TEXT("Load without options must fail.")); return false; }
	const FString FreshLoadErr = ANSI_TO_TCHAR(Error.c_str());
	if (!FreshLoadErr.Contains(TEXT("Load")) || !FreshLoadErr.Contains(TEXT("findAlternatePaths"))) { AddError(FString::Printf(TEXT("Load gate must name context+field, got: %s"), *FreshLoadErr)); return false; }
	// 2. Incomplete structs fail with the missing field named; state unchanged.
	FIMAcousticPathingOptions EmptyOpts;
	if (Sim.SetPathingOptions(EmptyOpts, Error)) { AddError(TEXT("Empty options must be rejected.")); return false; }
	const FString EmptySetErr = ANSI_TO_TCHAR(Error.c_str());
	FIMAcousticPathingOptions Partial; Partial.EnableValidation = false; Partial.HasEnableValidation = true;
	if (Sim.SetPathingOptions(Partial, Error)) { AddError(TEXT("Partial options must be rejected.")); return false; }
	const FString PartialSetErr = ANSI_TO_TCHAR(Error.c_str());
	if (!PartialSetErr.Contains(TEXT("findAlternatePaths"))) { AddError(FString::Printf(TEXT("Partial gate must name findAlternatePaths, got: %s"), *PartialSetErr)); return false; }
	if (Sim.GetPathingOptions().IsComplete()) { AddError(TEXT("Failed Set must not change stored options.")); return false; }
	// 3. Hybrid default: explicit set -> change readback (not yet applied to the SDK).
	if (!Sim.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), Error)) { AddError(ANSI_TO_TCHAR(Error.c_str())); return false; }
	const FIMAcousticPathingReadback SetRb = Sim.GetPathingReadback();
	if (!SetRb.EnableValidationApplied || !SetRb.FindAlternatePathsApplied || SetRb.AppliedToSdk || SetRb.ApplyCount != 0) { AddError(TEXT("Set readback must show ON/ON pending SDK application.")); return false; }
	// 4. Creation binds the struct identity: real Bake + Load -> Load readback.
	FIMAcousticSceneInput Scene; IMAcousticW1TestPrivate::H1POBoxRoom(Scene);
	FIMAcousticBakeData Bake;
	if (!Sim.Bake(Scene, Bake, Error)) { AddError(FString::Printf(TEXT("Synthetic bake failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	if (!Sim.Load(Bake, 48000, 512, Error)) { AddError(FString::Printf(TEXT("Load with Hybrid options failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	const FIMAcousticPathingReadback LoadRb = Sim.GetPathingReadback();
	if (!LoadRb.AppliedToSdk || !LoadRb.EnableValidationApplied || !LoadRb.FindAlternatePathsApplied || LoadRb.ApplyCount != 1) { AddError(TEXT("Load readback must show ON/ON applied once.")); return false; }
	// 5. Change path: EvaluateBatch applies the exact IPL values -> batch readback.
	std::vector<FIMAcousticAudioFrame> OutFrames2;
	if (!Sim.EvaluateBatch(In, IMAcousticW1TestPrivate::H1POFrame(-2.0f, 0.0f, 1.5f), OutFrames2, Error)) { AddError(FString::Printf(TEXT("EvaluateBatch failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	const FIMAcousticPathingReadback BatchRb = Sim.GetPathingReadback();
	if (!BatchRb.AppliedToSdk || BatchRb.AppliedSources != 1 || BatchRb.LastSourceKey != 7 || BatchRb.LastGeneration != 1 || BatchRb.ApplyCount != 2) { AddError(TEXT("Batch readback must show 1 applied source, key 7 gen 1, count 2.")); return false; }
	// 6. Runtime change to OFF/OFF (W3 validation-OFF counterexample vehicle) -> readback follows.
	FIMAcousticPathingOptions Off; Off.EnableValidation = false; Off.FindAlternatePaths = false;
	Off.HasEnableValidation = true; Off.HasFindAlternatePaths = true;
	if (!Sim.SetPathingOptions(Off, Error)) { AddError(ANSI_TO_TCHAR(Error.c_str())); return false; }
	std::vector<FIMAcousticAudioFrame> OutFrames3;
	if (!Sim.EvaluateBatch(In, IMAcousticW1TestPrivate::H1POFrame(-2.0f, 0.0f, 1.5f), OutFrames3, Error)) { AddError(FString::Printf(TEXT("OFF/OFF EvaluateBatch failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	const FIMAcousticPathingReadback OffRb = Sim.GetPathingReadback();
	if (OffRb.EnableValidationApplied || OffRb.FindAlternatePathsApplied || OffRb.ApplyCount != 3) { AddError(TEXT("OFF/OFF readback must show false/false at count 3.")); return false; }
	// 7. Evidence: one JSON receipt with the raw readbacks and gate errors.
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/H1-UE"), FString::Printf(TEXT("IMCF_W1_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString Json = FString::Printf(TEXT("{\"gate_fresh_evaluate\":\"%s\",\"gate_fresh_load\":\"%s\",\"gate_empty_set\":\"%s\",\"gate_partial_set\":\"%s\",\"set_readback\":%s,\"load_readback\":%s,\"batch_readback\":%s,\"off_off_readback\":%s,\"evaluate_ok\":true}"),
		*FreshEvalErr, *FreshLoadErr, *EmptySetErr, *PartialSetErr,
		*IMAcousticW1TestPrivate::H1PORbJson(SetRb), *IMAcousticW1TestPrivate::H1PORbJson(LoadRb), *IMAcousticW1TestPrivate::H1PORbJson(BatchRb), *IMAcousticW1TestPrivate::H1PORbJson(OffRb));
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Dir, TEXT("IM_pathing_readback.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticW1Pathing set=%s load=%s batch=%s off=%s dir=%s"),
		*IMAcousticW1TestPrivate::H1PORbJson(SetRb), *IMAcousticW1TestPrivate::H1PORbJson(LoadRb), *IMAcousticW1TestPrivate::H1PORbJson(BatchRb), *IMAcousticW1TestPrivate::H1PORbJson(OffRb), *Dir);
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticH1PathingOptions PASS"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS"));
	AddInfo(FString::Printf(TEXT("Pathing readback evidence: %s"), *Dir));
	return true;
}
#endif
