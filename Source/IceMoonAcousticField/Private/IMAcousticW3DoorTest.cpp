#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticSimulation.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticCoordinates.h"
#include "IMAcousticTestSupport.h"
#include "Engine/World.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Audio.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
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
#include "Misc/Guid.h"
#include "Sound/SoundWaveProcedural.h"
#include "Tests/AutomationEditorCommon.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/SecureHash.h"

// H1-local probe exporter (same protocol as W1; snapshots CSV carries the
// door capture columns). File-local: unity builds merge all test TUs.
namespace IMAcousticW3DoorTestPrivate
{
uint64 H1ExportComplete(const std::atomic<uint64>* Done, uint32 Capacity, uint64 Pushes)
{
	const uint64 Limit = Pushes < Capacity ? Pushes : Capacity;
	for (uint64 I = 0; I < Limit; ++I)
	{
		if (Done[I].load(std::memory_order_acquire) != I + 1) { return I; }
	}
	return Limit;
}
double H1TimingP99Us(const FIMAcousticTiming& Timing)
{
	const uint64 Count=Timing.Count.load(std::memory_order_acquire);
	if(Count==0)return 0.0;
	const uint64 Target=(Count*99+99)/100;
	uint64 Seen=0;
	for(uint32 I=0;I<FIMAcousticTiming::BinCount;++I)
	{
		Seen+=Timing.Bins[I].load(std::memory_order_relaxed);
		if(Seen>=Target)return double(I)*FIMAcousticTiming::BinMicroseconds;
	}
	return double(FIMAcousticTiming::BinCount-1)*FIMAcousticTiming::BinMicroseconds;
}
void H1ExportProbeTrace(FIMAcousticDeviceBridge* Bridge, const TArray<FString>& StateWindows, const FString& EvidenceDir,
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
	const uint64 BlockComplete = H1ExportComplete(Bridge->BlockDone.data(), FIMAcousticDeviceBridge::ProbeBlockCapacity, BlockPushes);
	const uint64 SnapComplete = H1ExportComplete(Bridge->SnapshotDone.data(), FIMAcousticDeviceBridge::ProbeSnapshotCapacity, SnapPushes);
	const uint64 WorkerComplete = H1ExportComplete(Bridge->WorkerDone.data(), FIMAcousticDeviceBridge::ProbeWorkerCapacity, WorkerPushes);
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
	Snaps += TEXT("block,world,captured,submit,lis_ue_x,lis_ue_y,lis_ue_z,lis_sdk_x,lis_sdk_y,lis_sdk_z,src0_sdk_x,src0_sdk_y,src0_sdk_z,src0_audio_id,num_sources,submitted,fail_code,num_dynamic,dyn_skipped,door0_x,door0_y,door0_z\n");
	for (uint64 I = 0; I < SnapComplete; ++I)
	{
		const FIMAcousticSnapshotProbe& S = Bridge->SnapshotProbes[I];
		Snaps += FString::Printf(TEXT("%llu,%llu,%.6f,%.6f,%.2f,%.2f,%.2f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%u,%u,%u,%u,%u,%.6f,%.6f,%.6f\n"),
			S.Block, S.WorldGeneration, S.CapturedSeconds, S.SubmitSeconds, S.ListenerUEX, S.ListenerUEY, S.ListenerUEZ,
			S.ListenerSDKX, S.ListenerSDKY, S.ListenerSDKZ, S.Source0X, S.Source0Y, S.Source0Z, S.Source0AudioId,
			S.NumSources, S.Submitted, S.FailCode, S.NumDynamicMeshes, S.NumDynamicSkipped, S.Door0TX, S.Door0TY, S.Door0TZ);
	}
	FFileHelper::SaveStringToFile(Snaps, *FPaths::Combine(EvidenceDir, TEXT("IM_probe_snapshots.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString Workers;
	Workers.Reserve(64 * 1024);
	Workers += TEXT("block,world,loop_start,snap_captured,snap_valid,snap_reason,num_inputs,published,eval_start,eval_end,eval_ok,reverb_attempt,reverb_ok,reverb_start,reverb_end,reverb_seq,reverb_captured,push_at,push_ok,wait_end,voice,audio_id,voice_gen,seq,direct_flags,occlusion,dist_gain,path_valid0,num_dyn,applied_val\n");
	for (uint64 I = 0; I < WorkerComplete; ++I)
	{
		const FIMAcousticWorkerProbe& R = Bridge->WorkerProbes[I];
		Workers += FString::Printf(TEXT("%llu,%llu,%.6f,%.6f,%u,%u,%u,%u,%.6f,%.6f,%u,%u,%u,%.6f,%.6f,%llu,%.6f,%.6f,%u,%.6f,%u,%llu,%llu,%llu,%u,%.6f,%.6f,%u,%u,%d\n"),
			R.Block, R.WorldGeneration, R.LoopStartSeconds, R.SnapshotCaptured, R.SnapValid, R.SnapReason, R.NumInputs, R.ResultsPublished,
			R.EvalStartSeconds, R.EvalEndSeconds, R.EvalOk, R.ReverbAttempt, R.ReverbOk, R.ReverbStartSeconds, R.ReverbEndSeconds,
			R.ReverbSequence, R.ReverbCaptured, R.PushSeconds, R.PushOk, R.WaitEndSeconds, R.FirstVoice, R.FirstAudioId, R.FirstVoiceGen, R.FirstSeq,
			R.DirectFlags, R.Occlusion, R.DistanceGain, R.PathValid0, R.NumDynamicSynced, R.AppliedValidationSnap);
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
	FP += TEXT(",\"w3\":\"") + HashOne(FPaths::Combine(SrcRoot, TEXT("IMAcousticW3DoorTest.cpp"))) + TEXT("\"}");
	FFileHelper::SaveStringToFile(FP, *FPaths::Combine(EvidenceDir, TEXT("IM_fingerprints.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FString Windows = TEXT("[");
	for (int32 W = 0; W < StateWindows.Num(); ++W) { if (W > 0) Windows += TEXT(","); Windows += StateWindows[W]; }
	Windows += TEXT("]");
	const uint64 BlockLimit = BlockPushes < FIMAcousticDeviceBridge::ProbeBlockCapacity ? BlockPushes : FIMAcousticDeviceBridge::ProbeBlockCapacity;
	const uint64 SnapLimit = SnapPushes < FIMAcousticDeviceBridge::ProbeSnapshotCapacity ? SnapPushes : FIMAcousticDeviceBridge::ProbeSnapshotCapacity;
	const uint64 WorkerLimit = WorkerPushes < FIMAcousticDeviceBridge::ProbeWorkerCapacity ? WorkerPushes : FIMAcousticDeviceBridge::ProbeWorkerCapacity;
	const bool ProbeComplete = (BlockOverflow == 0) && (SnapOverflow == 0) && (WorkerOverflow == 0)
		&& (BlockComplete == BlockLimit) && (SnapComplete == SnapLimit) && (WorkerComplete == WorkerLimit);
	const uint64 TransitChecks=Bridge->ApertureTransitChecks.load(std::memory_order_acquire);
	const uint64 TransitBlocked=Bridge->ApertureTransitBlocked.load(std::memory_order_acquire);
	const double TransitP99=H1TimingP99Us(Bridge->ApertureTransitTiming);
	const double TransitMax=FPlatformTime::ToSeconds64(Bridge->ApertureTransitTiming.MaxCycles.load(std::memory_order_relaxed))*1.e6;
	const FString Summary = FString::Printf(TEXT("{\"available\":true,\"complete\":%s,\"block_pushes\":%llu,\"block_exported\":%llu,\"block_overflow\":%llu,\"snapshot_pushes\":%llu,\"snapshot_exported\":%llu,\"snapshot_overflow\":%llu,\"worker_pushes\":%llu,\"worker_exported\":%llu,\"worker_overflow\":%llu,\"aperture_transit\":{\"checks\":%llu,\"blocked\":%llu,\"p99_us\":%.3f,\"max_us\":%.3f,\"budget_p99_us\":50.0,\"budget_max_us\":200.0},\"route_windows\":%s}"),
		ProbeComplete ? TEXT("true") : TEXT("false"), BlockPushes, BlockComplete, BlockOverflow, SnapPushes, SnapComplete, SnapOverflow, WorkerPushes, WorkerComplete, WorkerOverflow, TransitChecks, TransitBlocked, TransitP99, TransitMax, *Windows);
	FFileHelper::SaveStringToFile(Summary, *FPaths::Combine(EvidenceDir, TEXT("IM_probe_summary.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticH1ProbeExport complete=%d blocks=%llu/%llu overflow=%llu snaps=%llu/%llu overflow=%llu workers=%llu/%llu overflow=%llu evidence=%s"),
		ProbeComplete ? 1 : 0, BlockComplete, BlockPushes, BlockOverflow, SnapComplete, SnapPushes, SnapOverflow, WorkerComplete, WorkerPushes, WorkerOverflow, *EvidenceDir);
}

void H1ExportMetaSoundTrace(const FIMAcousticMetaSoundContextPtr& Context, const FString& EvidenceDir)
{
	if (!Context.IsValid())
	{
		FFileHelper::SaveStringToFile(TEXT("{\"available\":false,\"reason\":\"no MetaSound context observed\"}"),
			*FPaths::Combine(EvidenceDir, TEXT("IM_metasound_trace_missing.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return;
	}
	const uint32 Pushes = Context->CapturedSourceBlockCount.load(std::memory_order_acquire);
	const uint32 Capacity = uint32(Context->CapturedSourceBlocks.Num());
	const uint32 Limit = Pushes < Capacity ? Pushes : Capacity;
	FString Rows = TEXT("block,reject,routes,listener_x,listener_y,listener_z,distance_gain,callback_distance_cm,degraded_gain,input_e,output_e,direct_valid,path_valid,result_seq\n");
	Rows.Reserve(256 * 1024);
	for (uint32 I = 0; I < Limit; ++I)
	{
		const FIMAcousticBlockProbe& E = Context->CapturedSourceBlocks[int32(I)];
		Rows += FString::Printf(TEXT("%llu,%u,%u,%.6f,%.6f,%.6f,%.6f,%.3f,%.6f,%.9g,%.9g,%u,%u,%llu\n"),
			E.Block, uint32(E.Reject), E.Routes, E.ListenerX, E.ListenerY, E.ListenerZ, E.DistanceGain,
			E.CallbackDistanceCm, E.DegradedGain, E.InputEnergy, E.OutputEnergy, uint32(E.DirectValid), uint32(E.PathValid), E.ResultSequence);
	}
	FFileHelper::SaveStringToFile(Rows, *FPaths::Combine(EvidenceDir, TEXT("IM_metasound_source_blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const uint64 Rejected = Context->Device.IsValid() ? Context->Device->RejectedBlocks.load(std::memory_order_relaxed) : 0;
	const uint64 Rendered = Context->Device.IsValid() ? Context->Device->RenderedBlocks.load(std::memory_order_relaxed) : 0;
	const FString Summary = FString::Printf(TEXT("{\"available\":true,\"telemetry\":\"MetaSound.CapturedSourceBlocks\",\"blocks\":%u,\"capacity\":%u,\"device_rendered\":%llu,\"device_rejected\":%llu}"),
		Limit, Capacity, Rendered, Rejected);
	FFileHelper::SaveStringToFile(Summary, *FPaths::Combine(EvidenceDir, TEXT("IM_metasound_trace_summary.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}


namespace IMAcousticW3DoorTestPrivate
{
constexpr const TCHAR* H1DoorMap=TEXT("/IceMoonAcousticField/Tests/IM_H1Door");
float H1DoorOriginalBackgroundVolume=1;
bool H1DoorOriginalBackgroundAudio=false;
FString H1DoorEvidence()
{
	static FString Dir;
	if(Dir.IsEmpty())Dir=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/H1-UE"),FString::Printf(TEXT("IMCF_W3_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	return Dir;
}
// Door states in UE cm. Fixture frame = IMBuildW1Map Box() convention: room
// x in [-4.1,4.1], partition slab at UE Y~0, aperture UE X in [-250,-50],
// Z in [0,250] (derived; falsifiable: the CLOSED state energy decides).
// CLOSED covers the hole with overlap; OPEN slides in-plane clear of it;
// PARKED lifts the panel above the room (baseline, aperture fully open).
// CLOSED/OPEN height: hole spans UE Z 0..250; center Z=125 gives 10cm overlap
// with 270cm panel. Z=275 left a 140cm under-door gap and leaked closed-state
// path audio (W3 07:37 run d_path=188).
const FVector H1DoorClosed(-150,0,125);
const FVector H1DoorClosedScale(2.2f,0.1f,2.7f);
const FVector H1DoorOpen(150,0,125);
const FVector H1DoorParked(0,0,600);
bool BuildH1DoorMap(FString& Error)
{
	if(FPackageName::DoesPackageExist(H1DoorMap))
		return FEditorFileUtils::LoadMap(H1DoorMap,false,true);
	UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
	auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
	auto* Plane=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane"));
	if(!World||!Cube||!Plane){Error=TEXT("Cannot create H1 fixture world or load engine cube/plane.");return false;}
	auto Box=[World,Cube](FVector Lo,FVector Hi)
	{
		const FVector Center=(Lo+Hi)*.5,Size=Hi-Lo;
		auto* Actor=World->SpawnActor<AStaticMeshActor>();
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Actor->SetActorLocation(FVector(-Center.Z,Center.X,Center.Y)*100);
		Actor->SetActorScale3D(FVector(Size.Z,Size.X,Size.Y));
		Actor->Tags.Add(TEXT("IMH1Fixture"));
	};
	// W1 room geometry verbatim (partition with aperture around the hole).
	Box({-4.1,-.1,-3.1},{4.1,0,3.1});Box({-4.1,3,-3.1},{4.1,3.1,3.1});
	Box({-4.1,0,-3.1},{-4,3,3.1});Box({4,0,-3.1},{4.1,3,3.1});
	Box({-4,0,-3.1},{4,3,-3});Box({-4,0,3},{4,3,3.1});
	Box({-.05,0,-3},{.05,3,.5});Box({-.05,0,2.5},{.05,3,3});Box({-.05,2.5,.5},{.05,3,2.5});
	auto* Volume=World->SpawnActor<AIMAcousticBakeVolume>();
	Volume->SetActorLocation(FVector(0,0,150));Volume->BakeBounds->SetBoxExtent(FVector(320,420,170));
	FIMAcousticMaterialMapping Material;Material.Material=Cube->GetMaterial(0);Material.Absorption=FVector(.25);Volume->Materials.Add(Material);
	// Door panel: movable (never baked), registered for per-tick sync.
	// inc121 PLANEDOOR experiment (diagnostic, revert after verdict): clean-quad
	// Plane leaf through the real builder+worker chain. Plane is 100x100cm in XY
	// facing +Z; Roll 90 maps localX->worldX, localY->worldZ, localZ->worldY, so
	// scale (2.2,2.7,1.0) spans 220cm UE-X by 270cm UE-Z at Y~0 like the Cube door.
	auto* Door=World->SpawnActor<AStaticMeshActor>();
	Door->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Door->GetStaticMeshComponent()->SetStaticMesh(Plane);
	Door->SetActorRotation(FRotator(0.f,0.f,90.f));
	Door->SetActorLocation(H1DoorParked);Door->SetActorScale3D(FVector(2.2f,2.7f,1.0f));
	Door->GetStaticMeshComponent()->SetMaterial(0,Cube->GetMaterial(0));
	Door->Tags.Add(TEXT("IMH1Door"));
	Volume->DynamicBlockers.Add(TObjectPtr<UStaticMeshComponent>(Door->GetStaticMeshComponent()));
	const FString File=FPackageName::LongPackageNameToFilename(H1DoorMap,FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File),true);
	if(!FEditorFileUtils::SaveLevel(World->PersistentLevel,File)){Error=TEXT("Cannot save plugin-owned H1 fixture map.");return false;}
	return true;
}
AStaticMeshActor* FindH1Door(UWorld* World)
{
	for(TActorIterator<AStaticMeshActor> It(World);It;++It){if(It->ActorHasTag(TEXT("IMH1Door")))return *It;}
	return nullptr;
}
class FIMAcousticW3DoorCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticW3DoorCommand(FAutomationTestBase* InTest,int32 InitialStage=0):Test(InTest),Started(FPlatformTime::Seconds()),Stage(InitialStage){}
	bool Update() override
	{
		const double Now=FPlatformTime::Seconds();
		if(Now-Started>300)return Finish(false,TEXT("W3 door sequence timed out."));
		if(Stage==0)
		{
			UWorld* World=GEditor->GetEditorWorldContext().World();
			FString Error;
			// Map load applies on the next tick; find the volume only afterwards.
			if(!bMapLoaded){ if(!BuildH1DoorMap(Error))return Finish(false,Error); bMapLoaded=true; return false; }
			for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It){if(Volume.IsValid())return Finish(false,TEXT("H1 fixture has multiple bake volumes."));Volume=*It;}
			if(!Volume.IsValid())return Finish(false,TEXT("H1 fixture missing bake volume."));
			// Door must exist and be registered; repair (H1-only map) then persist.
			AStaticMeshActor* Door=FindH1Door(World);
			auto* PlaneFix=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane"));
			auto* CubeFix=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
			if(!PlaneFix||!CubeFix)return Finish(false,TEXT("Cannot load engine plane/cube for H1 door."));
			if(!Door)
			{
				Door=World->SpawnActor<AStaticMeshActor>();
				Door->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
				Door->Tags.Add(TEXT("IMH1Door"));
			}
			// inc121 PLANEDOOR experiment (diagnostic, revert after verdict):
			// enforce the clean-quad Plane leaf on the shared fixture map.
			Door->GetStaticMeshComponent()->SetStaticMesh(PlaneFix);
			Door->SetActorRotation(FRotator(0.f,0.f,90.f));
			Door->SetActorLocation(H1DoorParked);Door->SetActorScale3D(FVector(2.2f,2.7f,1.0f));
			Door->GetStaticMeshComponent()->SetMaterial(0,CubeFix->GetMaterial(0));
			if(!Volume->DynamicBlockers.Contains(TObjectPtr<UStaticMeshComponent>(Door->GetStaticMeshComponent())))Volume->DynamicBlockers.Add(TObjectPtr<UStaticMeshComponent>(Door->GetStaticMeshComponent()));
			FString BakeError;
			if(Volume->ValidateCurrentBake(BakeError))
			{
				const FString File=FPackageName::LongPackageNameToFilename(H1DoorMap,FPackageName::GetMapPackageExtension());
				if(!FEditorFileUtils::SaveLevel(World->PersistentLevel,File))return Finish(false,TEXT("Cannot persist H1 fixture binding."));
				FString LoadError;GUnrealEd->AutomationLoadMap(H1DoorMap,false,&LoadError);
				if(!LoadError.IsEmpty())return Finish(false,LoadError);
				ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticW3DoorCommand(Test,2));
				return true;
			}
			Volume->GenerateProbes();
			if(Volume->GeneratedProbes<=0)
			{
				for(const FString& Issue:Volume->SceneIssues)UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticH1DoorScene %s"),*Issue);
				return Finish(false,Volume->Status);
			}
			PreviousAsset=Volume->BakedField;Volume->Bake();Stage=1;return false;
		}
		if(Stage==1)
		{
			if(!Volume.IsValid())return Finish(false,TEXT("Bake owner destroyed."));
			if(Volume->Status.Contains(TEXT("failed"),ESearchCase::IgnoreCase)||Volume->Status.Contains(TEXT("discarded")))return Finish(false,Volume->Status);
			if(!Volume->BakedField||Volume->BakedField.Get()==PreviousAsset.Get())return false;
			IFileManager::Get().MakeDirectory(*H1DoorEvidence(),true);
			FFileHelper::SaveArrayToFile(Volume->BakedField->SceneData,*FPaths::Combine(H1DoorEvidence(),TEXT("scene.bin")));
			FFileHelper::SaveArrayToFile(Volume->BakedField->ProbeData,*FPaths::Combine(H1DoorEvidence(),TEXT("probes.bin")));
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorBake triangles=%d probes=%d evidence=%s"),Volume->ExportedTriangles,Volume->GeneratedProbes,*H1DoorEvidence());
			const FString File=FPackageName::LongPackageNameToFilename(H1DoorMap,FPackageName::GetMapPackageExtension());
			if(!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel,File))return Finish(false,TEXT("Cannot persist H1 bake binding."));
			FString Error;GUnrealEd->AutomationLoadMap(H1DoorMap,false,&Error);
			if(!Error.IsEmpty())return Finish(false,Error);
			ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticW3DoorCommand(Test,2));
			return true;
		}
		UWorld* PIE=nullptr;
		for(const auto& Context:GEngine->GetWorldContexts())if(Context.WorldType==EWorldType::PIE){PIE=Context.World();break;}
		if(!PIE)return Stage==2?false:Finish(false,TEXT("PIE ended before door evidence."));
		if(Stage>=2)FeedAudio();
		if(Stage==2)
		{
			for(TActorIterator<AIMAcousticBakeVolume> It(PIE);It;++It)Volume=*It;
			if(!Volume.IsValid())return Finish(false,TEXT("PIE bake volume missing."));
			DoorActor=FindH1Door(PIE);
			if(!DoorActor.IsValid())return Finish(false,TEXT("PIE door actor missing."));
			// H1 W3 PIE rebind: PIE duplicates Volume with Editor DynamicBlockers
			// refs (parked transform). Rebind to PIE door so Tick snapshots follow
			// SetActorLocation; stale refs freeze door0_sdk at parked and leak
			// closed-state path audio.
			Volume->DynamicBlockers.Empty();
			if(UStaticMeshComponent* PIEDoorComp=DoorActor->GetStaticMeshComponent())
			{
				Volume->DynamicBlockers.Add(TObjectPtr<UStaticMeshComponent>(PIEDoorComp));
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorRebind PIE door rebound, blockers=%d"),Volume->DynamicBlockers.Num());
			}
			else
			{
				return Finish(false,TEXT("PIE door has no static mesh component."));
			}
			// H1-B runtime contract: the aperture is an authored test input,
			// not a product-side fixture constant.  The policy is conservative
			// and only blocks path output while current dynamic geometry covers
			// this volume; parked/open/no-door remain unblocked.
			if(!Volume->SetApertureTransitForTest(true,FVector(-150,0,125),FVector(110,5,135)))
				return Finish(false,TEXT("Cannot enable aperture-transit validation."));
			APlayerController* Listener=PIE->GetFirstPlayerController();if(!Listener)return false;
			Listener->SetAudioListenerOverride(nullptr,FVector(100,200,150),FRotator(0,-90,0));
			auto InitBridge=IMAcousticTestSupport::FindBridge(PIE);
			if(!InitBridge.IsValid())return false;
			if(!ProbeBridge.IsValid())ProbeBridge=InitBridge;
			if(!ListenerConverged(InitBridge))
			{
				if(Now-LastDiagnostic>5){LastDiagnostic=Now;UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorListenerWait snaps=%llu"),InitBridge->SnapshotProbePushes.load(std::memory_order_acquire));}
				return false;
			}
			if(!bSourceSpawned){ if(!SpawnH1Source(PIE))return Finish(false,TEXT("H1 source spawn failed.")); bSourceSpawned=true; GraphGateBefore=GraphSourceBlocks(); }
			if(!MetaContext.IsValid())
			{
				if(FAudioDevice* AudioDevice=PIE->GetAudioDeviceRaw())
					MetaContext=IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
			}
			if(MetaContext.IsValid()&&GraphGateBefore==0)GraphGateBefore=GraphSourceBlocks();
			Volume->bDirectRoute=false;Volume->bPathRoute=true;Volume->bReverbRoute=false;
			// Rendered-audio gate: the new source is a MetaSound graph, so the
			// legacy device ring's PathNonzeroBlocks is intentionally empty.
			// Gate on the graph operator's accepted path output instead.
			auto GBridge=IMAcousticTestSupport::FindBridge(PIE);
			if(!GBridge.IsValid())return false;
			if(!ProbeBridge.IsValid() || ProbeBridge != GBridge)ProbeBridge=GBridge;
			uint64 GraphPathBlocks=0,GraphAcceptedBlocks=0,GraphRejectedBlocks=0,GraphSeq=0;
			GraphWindow(GraphGateBefore,GraphSourceBlocks(),GraphAcceptedBlocks,GraphPathBlocks,GraphRejectedBlocks,GraphSeq);
			if(MetaContext.IsValid()&&GraphPathBlocks>0)
			{
				DoorActor->SetActorLocation(H1DoorParked);
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorInitReady t=%.3f"),Now-Started);
				SettleUntil=Now+1.2;Stage=3;StateIndex=0;return false;
			}
			// Spawn-race watchdog (W1-proven): playing+active voice can sit on
			// zero dry input; recovery is bounded respawn (max 2), then fail.
			if(GateDead0==0)GateDead0=Now;
			if(GBridge->PushDryInputNonzero.load()!=GateDry0){GateDry0=GBridge->PushDryInputNonzero.load();GateT0=Now;GateInit=true;}
			if(GateRe<2&&GateInit&&Now-GateT0>10)
			{++GateRe;GateInit=false;GateT0=0;if(auto* Old=MovingSource.Get()){if(auto* OA=Old->GetOwner())OA->Destroy();}MovingSource.Reset();
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorRespawn n=%d"),GateRe);
			if(!SpawnH1Source(PIE))return true;return false;}
			if(GateDead0!=0&&Now-GateDead0>40)return Finish(false,TEXT("No rendered path audio before recording (spawn race unrecovered)."));
			if(Now-LastDiagnostic>5){LastDiagnostic=Now;UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorRenderWait graph_blocks=%u accepted=%llu path=%llu rejected=%llu legacy_rendered=%llu legacy_path=%llu drynz=%llu"),GraphSourceBlocks(),GraphAcceptedBlocks,GraphPathBlocks,GraphRejectedBlocks,GBridge->RenderedBlocks.load(),GBridge->PathNonzeroBlocks.load(),GBridge->PushDryInputNonzero.load());}
			return false;
		}
		const int StateCount=5;
		auto StateLoc=[](int I)->FVector{ return (I==1||I>=3)?H1DoorClosed:(I==2?H1DoorOpen:H1DoorParked); };
		auto StateSecs=[](int I)->double{ return I==4?3.0:4.0; };
		if(Stage==3)
		{
			if(StateIndex==4)
			{
				if(OffPhase==0)
				{
					if(!Volume.IsValid()||!Volume->SetPathingValidationForTest(false))return Finish(false,TEXT("OFF control request failed."));
					OffPhase=1;OffRequestStart=Now;return false;
				}
				if(OffPhase==1)
				{
					const int Applied=Volume.IsValid()?Volume->ReadAppliedPathingValidationForTest():-2;
					if(Applied!=0){ if(Now-OffRequestStart>5)return Finish(false,TEXT("OFF control not applied.")); return false; }
					OffPhase=2;SettleUntil=Now+1.0;return false;
				}
			}
			if(Now<SettleUntil)return false;
			auto QBridge=IMAcousticTestSupport::FindBridge(PIE);if(!QBridge.IsValid())return Finish(false,TEXT("Bridge lost before state window."));
			// The recorder flushes asynchronously after StopRecordingOutput.
			// Mute the production route at that boundary so stale-result
			// fallback blocks cannot contaminate the exact-zero WAV tail.
			QBridge->RenderRoutes.store(2,std::memory_order_relaxed);
			DirectBefore=QBridge->DirectNonzeroBlocks.load();PathBefore=QBridge->PathNonzeroBlocks.load();RejectedBefore=QBridge->RejectedBlocks.load();
			RenderedBefore=QBridge->RenderedBlocks.load();PushesBefore=QBridge->SnapshotProbePushes.load(std::memory_order_acquire);
			GraphWindowBefore=GraphSourceBlocks();
			UAudioMixerBlueprintLibrary::StartRecordingOutput(PIE,3);WinStart=Now;WavStopped=false;Stage=4;return false;
		}
		if(Stage==4)
		{
			if(Now-WinStart<StateSecs(StateIndex))return false;
			auto QBridge=IMAcousticTestSupport::FindBridge(PIE);if(!QBridge.IsValid())return Finish(false,TEXT("Bridge lost in state window."));
			const FString SN=StateName(StateIndex);
			const FString WavPath=FPaths::Combine(H1DoorEvidence(),SN+TEXT(".wav"));
			if(!WavStopped){ QBridge->RenderRoutes.store(0,std::memory_order_relaxed); UAudioMixerBlueprintLibrary::StopRecordingOutput(PIE,EAudioRecordingExportType::WavFile,SN,H1DoorEvidence()); TArray<FString> RecFiles;IFileManager::Get().FindFiles(RecFiles,*H1DoorEvidence(),TEXT("*")); UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorRecStop state=%s wav=%s dir_exists=%d files=%d"),*SN,*WavPath,IFileManager::Get().DirectoryExists(*H1DoorEvidence())?1:0,RecFiles.Num()); WavStopped=true; StopTime=Now; return false; }
			// Recorder flush latency is environment-dependent (W1 polls unboundedly
			// inside its 120s cap); 30s bounds one state, the 300s total cap bounds all.
			// Single discriminating probe (2026-09-16): W1 records fine through the
			// same API, so measure this recorder's actual flush latency once with
			// size diagnostics instead of assuming a bound.
			const int64 WavSize=IFileManager::Get().FileSize(*WavPath);
			if(WavSize<=44)
			{
				if(Now-LastDiagnostic>5){LastDiagnostic=Now;UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorWavWait state=%s size=%lld t=%.1f"),*SN,WavSize,Now-StopTime);}
				if(Now-StopTime>90)return Finish(false,FString::Printf(TEXT("%s: state wav never flushed (90s)."),*SN));
				return false;
			}
			uint64 DDirect=QBridge->DirectNonzeroBlocks.load()-DirectBefore,DPath=QBridge->PathNonzeroBlocks.load()-PathBefore;
			uint64 DRej=QBridge->RejectedBlocks.load()-RejectedBefore,DRen=QBridge->RenderedBlocks.load()-RenderedBefore;
			uint64 GraphMaxSequence=0;
			if(MetaContext.IsValid())
			{
				uint64 GraphAccepted=0,GraphPath=0,GraphRejected=0,GraphSeq=0;
				GraphWindow(GraphWindowBefore,GraphSourceBlocks(),GraphAccepted,GraphPath,GraphRejected,GraphSeq);
				GraphMaxSequence=GraphSeq;
				DDirect=0;DPath=GraphPath;DRej=GraphRejected;DRen=GraphAccepted;
			}
			const uint64 PushesNow=QBridge->SnapshotProbePushes.load(std::memory_order_acquire);
			double Energy=0;uint32 PcmBytes=0;FString WavErr;
			if(!ParseStateWav(WavPath,Energy,PcmBytes,WavErr))return Finish(false,WavErr);
			const FVector DoorUE=DoorActor.IsValid()?DoorActor->GetActorLocation():FVector(0,0,0);
			if(!DoorActor.IsValid())return Finish(false,TEXT("Door actor lost in state window."));
			FVector Door0SDK=FVector::ZeroVector;bool bDoor0Found=false;
			const uint64 SPushes=FMath::Min<uint64>(PushesNow,uint64(FIMAcousticDeviceBridge::ProbeSnapshotCapacity));
			for(uint64 I=SPushes;I>0;--I){const uint64 Idx=I-1;if(QBridge->SnapshotDone[Idx].load(std::memory_order_acquire)!=Idx+1)continue;const FIMAcousticSnapshotProbe& S=QBridge->SnapshotProbes[Idx];if(S.CapturedSeconds<WinStart-0.1)break;if(S.NumDynamicMeshes==0)continue;Door0SDK=FVector(S.Door0TX,S.Door0TY,S.Door0TZ);bDoor0Found=true;break;}
			if(!bDoor0Found)return Finish(false,FString::Printf(TEXT("%s: no captured door in window."),*SN));
			uint64 WinMaxSeq=GraphMaxSequence;
			if(!MetaContext.IsValid())
			{
				const uint64 BPushes=FMath::Min<uint64>(QBridge->BlockProbePushes.load(std::memory_order_acquire),uint64(FIMAcousticDeviceBridge::ProbeBlockCapacity));
				for(uint64 I=0;I<BPushes;++I){if(QBridge->BlockDone[I].load(std::memory_order_acquire)!=I+1)continue;const FIMAcousticBlockProbe& B=QBridge->BlockProbes[I];if(B.ConsumedSeconds>=WinStart&&B.ConsumedSeconds<=Now&&B.ResultSequence>WinMaxSeq)WinMaxSeq=B.ResultSequence;}
			}
			if(PushesNow<=PushesBefore)return Finish(false,FString::Printf(TEXT("%s: snapshots did not advance."),*SN));
			if(DRen==0)return Finish(false,FString::Printf(TEXT("%s: no audio consumed in window."),*SN));
			if(WinMaxSeq<=PrevMaxSeq)return Finish(false,FString::Printf(TEXT("%s: consumed sequence not monotonic (%llu<=%llu)."),*SN,WinMaxSeq,PrevMaxSeq));
			PrevMaxSeq=WinMaxSeq;
			if(DDirect!=0)return Finish(false,FString::Printf(TEXT("%s: direct must stay sealed (d_direct=%llu)."),*SN,DDirect));
			const bool bExpectPath=(StateIndex==0||StateIndex==2||StateIndex==4);
			if(bExpectPath&&(DPath==0||Energy==0))return Finish(false,FString::Printf(TEXT("%s: open/OFF state must carry path audio (d_path=%llu energy=%.6g)."),*SN,DPath,Energy));
			if(!bExpectPath&&(DPath!=0||Energy!=0))return Finish(false,FString::Printf(TEXT("%s: closed state must seal path audio (d_path=%llu energy=%.6g)."),*SN,DPath,Energy));
			Door0PerState.Add(Door0SDK);
			StateWindows.Add(FString::Printf(TEXT("{\"state\":\"%s\",\"win_start\":%.3f,\"win_end\":%.3f,\"d_direct\":%llu,\"d_path\":%llu,\"d_rejected\":%llu,\"rendered\":%llu,\"energy\":%.6g,\"pcm_bytes\":%u,\"door_ue\":[%.1f,%.1f,%.1f],\"door0_sdk\":[%.4g,%.4g,%.4g],\"max_seq\":%llu}"),*SN,WinStart,Now,DDirect,DPath,DRej,DRen,Energy,PcmBytes,DoorUE.X,DoorUE.Y,DoorUE.Z,Door0SDK.X,Door0SDK.Y,Door0SDK.Z,WinMaxSeq));
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorState %s d_direct=%llu d_path=%llu energy=%.6g max_seq=%llu"),*SN,DDirect,DPath,Energy,WinMaxSeq);
			QBridge->RenderRoutes.store(2,std::memory_order_relaxed);
			if(StateIndex==4){ if(Volume.IsValid())Volume->SetPathingValidationForTest(true); OnRequestStart=Now; Stage=5; return false; }
			++StateIndex;DoorActor->SetActorLocation(StateLoc(StateIndex));SettleUntil=Now+1.2;Stage=3;return false;
		}
		if(Stage==5)
		{
			const int Applied=Volume.IsValid()?Volume->ReadAppliedPathingValidationForTest():-2;
			if(Applied!=1){ if(Now-OnRequestStart>5)return Finish(false,TEXT("Validation restore not applied.")); return false; }
			Stage=6;return false;
		}
		if(Stage==6)
		{
			// Consecutive-pair door deltas must match the commanded slide (SDK z ~3.0).
			auto Dz=[&](int A,int B){ return Door0PerState.IsValidIndex(A)&&Door0PerState.IsValidIndex(B)?(Door0PerState[B]-Door0PerState[A]):FVector(0,0,0); };
			const FVector D12=Dz(1,2),D23=Dz(2,3);
			if(FMath::Abs(D12.Z+3.0)>0.05||FMath::Abs(D12.X)>0.05||FMath::Abs(D12.Y)>0.05)return Finish(false,FString::Printf(TEXT("CLOSED->OPEN door delta wrong: (%.4g,%.4g,%.4g)."),D12.X,D12.Y,D12.Z));
			if(FMath::Abs(D23.Z-3.0)>0.05||FMath::Abs(D23.X)>0.05||FMath::Abs(D23.Y)>0.05)return Finish(false,FString::Printf(TEXT("OPEN->CLOSED2 door delta wrong: (%.4g,%.4g,%.4g)."),D23.X,D23.Y,D23.Z));
			if((Door0PerState[0]-Door0PerState[1]).Size()<1.0)return Finish(false,TEXT("PARKED must differ from CLOSED capture."));
			if(!ProbeBridge.IsValid())return Finish(false,TEXT("Aperture-transit performance bridge missing."));
			const uint64 TransitChecks=ProbeBridge->ApertureTransitChecks.load(std::memory_order_acquire);
			const double TransitP99=H1TimingP99Us(ProbeBridge->ApertureTransitTiming);
			const double TransitMax=FPlatformTime::ToSeconds64(ProbeBridge->ApertureTransitTiming.MaxCycles.load(std::memory_order_relaxed))*1.e6;
			if(TransitChecks==0)return Finish(false,TEXT("Aperture-transit gate was never exercised."));
			if(TransitP99>50.0||TransitMax>200.0)
				return Finish(false,FString::Printf(TEXT("Aperture-transit budget exceeded: checks=%llu p99=%.3fus max=%.3fus (budgets 50/200)."),TransitChecks,TransitP99,TransitMax));
			return Finish(true,TEXT("Door sequence closed->open->closed with OFF red control passed; deltas match slide."));
		}
		return false;
	}
	uint32 GraphSourceBlocks() const
	{
		return MetaContext.IsValid() ? MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire) : 0;
	}
	void GraphWindow(uint32 Begin, uint32 End, uint64& Accepted, uint64& Path, uint64& Rejected, uint64& MaxSequence) const
	{
		Accepted=0;Path=0;Rejected=0;MaxSequence=0;
		if(!MetaContext.IsValid())return;
		const uint32 Capacity=uint32(MetaContext->CapturedSourceBlocks.Num());
		const uint32 Limit=End<Capacity?End:Capacity;
		const uint32 First=Begin<Limit?Begin:Limit;
		for(uint32 I=First;I<Limit;++I)
		{
			const FIMAcousticBlockProbe& E=MetaContext->CapturedSourceBlocks[int32(I)];
			if(E.ResultSequence>MaxSequence)MaxSequence=E.ResultSequence;
			if(E.Reject!=EIMAcousticProbeReject::Accepted){++Rejected;continue;}
			++Accepted;
			if((E.Routes&2u)!=0&&E.OutputEnergy>1e-9)++Path;
		}
	}
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
			return FMath::Abs(double(S.ListenerUEX)-100.0)<1e-3&&FMath::Abs(double(S.ListenerUEY)-200.0)<1e-3&&FMath::Abs(double(S.ListenerUEZ)-150.0)<1e-3;
		}
		return false;
	}
	bool SpawnH1Source(UWorld* PIE)
	{
		if(!PIE){Finish(false,TEXT("H1 source spawn failed."));return false;}
		auto* Actor=PIE->SpawnActor<AActor>();if(!Actor){Finish(false,TEXT("H1 source spawn failed."));return false;}
		// C-geometry (slab ray): hole-axis starved the render gate (W3-hole run: 1400 rendered, path=0); wall-segment ray keeps direct blocked in every state. States/thresholds unchanged.
		Actor->SetActorLocation(FVector(100,-200,150));
		auto* Audio=NewObject<UAudioComponent>(Actor);Actor->SetRootComponent(Audio);Actor->AddInstanceComponent(Audio);
		Audio->bAutoActivate=false;Audio->RegisterComponent();Audio->SetWorldLocation(FVector(100,-200,150));
		MovingSource=Audio;
		auto* Source=NewObject<UIMAcousticSourceComponent>(Actor);Actor->AddInstanceComponent(Source);Source->AudioComponent=Audio;Source->RegisterComponent();
		FString Error;
		if(!IMAcousticTestSupport::ConfigureGraphSource(Audio,Error)){Finish(false,Error);return false;}
		if(!Source->ValidateSource(Error)){Finish(false,Error);return false;}
		Audio->Play();return true;
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
	FString StateName(int I) const
	{
		if(I==0)return TEXT("IM_nodoor");if(I==1)return TEXT("IM_closed");if(I==2)return TEXT("IM_open");if(I==3)return TEXT("IM_closed2");return TEXT("IM_off_control");
	}
	bool ParseStateWav(const FString& Path,double& Energy,uint32& PcmBytes,FString& Err)
	{
		TArray<uint8> Bytes;FWaveModInfo WaveInfo;
		if(!FFileHelper::LoadFileToArray(Bytes,*Path)||!WaveInfo.ReadWaveInfo(Bytes.GetData(),Bytes.Num())){Err=FString::Printf(TEXT("Cannot read state wav: %s"),*Path);return false;}
		if(!WaveInfo.pBitsPerSample||*WaveInfo.pBitsPerSample!=16){Err=TEXT("Unexpected state PCM format.");return false;}
		if(WaveInfo.SampleDataSize<500000){Err=FString::Printf(TEXT("State wav truncated: %u bytes."),WaveInfo.SampleDataSize);return false;}
		Energy=0;
		for(uint32 I=0;I+1<WaveInfo.SampleDataSize;I+=2){int16 Sample;FMemory::Memcpy(&Sample,WaveInfo.SampleDataStart+I,sizeof(Sample));Energy+=double(Sample)*Sample;}
		PcmBytes=WaveInfo.SampleDataSize;
		return true;
	}
	bool Finish(bool Pass,const FString& Message)
	{
		uint64 CRen=0,CRej=0,CDir=0,CPath=0;
		if(ProbeBridge.IsValid()){CRen=ProbeBridge->RenderedBlocks.load();CRej=ProbeBridge->RejectedBlocks.load();CDir=ProbeBridge->DirectNonzeroBlocks.load();CPath=ProbeBridge->PathNonzeroBlocks.load();}
		const FString RC=FString::Printf(TEXT("{\"started\":%.6f,\"states\":%d,\"windows\":%d,\"rendered\":%llu,\"rejected\":%llu,\"direct\":%llu,\"path\":%llu}"),Started,StateIndex+1,StateWindows.Num(),CRen,CRej,CDir,CPath);
		if(ProbeBridge.IsValid()){H1ExportProbeTrace(ProbeBridge.Get(),StateWindows,H1DoorEvidence(),RC);}
		else{H1ExportProbeTrace(nullptr,StateWindows,H1DoorEvidence(),RC);}
		H1ExportMetaSoundTrace(MetaContext,H1DoorEvidence());
		if(!Pass)Test->AddError(Message);else Test->AddInfo(Message);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1DoorSequence %s %s"),Pass?TEXT("PASS"):TEXT("FAIL"),*Message);
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticH1DoorSequence %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
		FApp::SetUnfocusedVolumeMultiplier(H1DoorOriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=H1DoorOriginalBackgroundAudio;
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		if(GUnrealEd)GUnrealEd->RequestEndPlayMap();return true;
	}
	FAutomationTestBase* Test;
	double Started,WinStart=0,SettleUntil=0,LastDiagnostic=0,OffRequestStart=0,OnRequestStart=0,StopTime=0,GateT0=0,GateDead0=0;
	bool bMapLoaded=false;
	bool bSourceSpawned=false;
	bool WavStopped=false;
	int32 Stage=0,StateIndex=0,OffPhase=0;
	TArray<int16> FeedPCM;int32 FeedCursor=0;
	uint64 DirectBefore=0,PathBefore=0,RejectedBefore=0,RenderedBefore=0,PushesBefore=0,PrevMaxSeq=0,GateDry0=0;
	int GateRe=0;bool GateInit=false;
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> ProbeBridge;
	FIMAcousticMetaSoundContextPtr MetaContext;
	uint32 GraphGateBefore=0,GraphWindowBefore=0;
	TArray<FString> StateWindows;
	TArray<FVector> Door0PerState;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<UIMAcousticBakeAsset> PreviousAsset;
	TWeakObjectPtr<AStaticMeshActor> DoorActor;
	TWeakObjectPtr<UAudioComponent> MovingSource;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticDoorSequenceTest,"IceMoon.AcousticField.H1.DoorSequence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ProductFilter)
bool FIMAcousticDoorSequenceTest::RunTest(const FString&)
{
	FString Error;if(!IMAcousticW3DoorTestPrivate::BuildH1DoorMap(Error)){AddError(Error);return false;}
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	IMAcousticW3DoorTestPrivate::H1DoorOriginalBackgroundVolume=FApp::GetUnfocusedVolumeMultiplier();
	FApp::SetUnfocusedVolumeMultiplier(1);
	IMAcousticW3DoorTestPrivate::H1DoorOriginalBackgroundAudio=GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=true;
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3DoorTestPrivate::FIMAcousticW3DoorCommand(this));return true;
}
// H1 no-audio isolation (inc104): does the baked static scene occlude a ray
// through the solid partition (A, expect occ>=0.99) while passing a ray through
// the open hole (B, expect occ<=0.01)? The dynamic door is never synced here
// (parked), so B is unobstructed. A shared IMToSDKSpace convention with the
// runtime is used; origin (0,0,150) is the bound bake bounds_origin_cm and
// matches live snapshot cross-checks (door0_sdk, src0_sdk, lis_sdk).
// H1 wall-map diagnostic (inc104): shoot a grid of occlusion rays across the
// partition plane (X=0 crossing, source X=-2 -> listener X=+2) and print which
// (Y,Z) cells the baked scene blocks. Diagnostic only: verdict PASS unless
// the sim API itself errors, so the printed map survives to the log.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticStaticOcclusionTest,"IceMoon.AcousticField.H1.StaticOcclusion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ProductFilter)
bool FIMAcousticStaticOcclusionTest::RunTest(const FString&)
{
	FString Error;if(!IMAcousticW3DoorTestPrivate::BuildH1DoorMap(Error)){AddError(Error);return false;}
	UWorld* World=GEditor->GetEditorWorldContext().World();
	AIMAcousticBakeVolume* Volume=nullptr;
	for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It){Volume=*It;break;}
	if(!Volume){AddError(TEXT("H1 fixture missing bake volume."));return false;}
	FString BakeError;
	if(!Volume->ValidateCurrentBake(BakeError)){AddError(BakeError);return false;}
	UIMAcousticBakeAsset* Bake=Volume->BakedField.Get();
	if(!Bake||Bake->SceneData.Num()==0||Bake->ProbeData.Num()==0){AddError(TEXT("Bound bake payload empty."));return false;}
	FIMAcousticBakeData Data;
	Data.Scene.assign(Bake->SceneData.GetData(),Bake->SceneData.GetData()+Bake->SceneData.Num());
	Data.ProbeBatch.assign(Bake->ProbeData.GetData(),Bake->ProbeData.GetData()+Bake->ProbeData.Num());
	FIMAcousticSimulation Sim;
	std::string SetError;
	if(!Sim.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(),SetError)){AddError(TEXT("SetPathingOptions failed."));return false;}
	std::string LoadError;
	if(!Sim.Load(Data,48000,1024,LoadError)){AddError(TEXT("Simulation Load failed."));return false;}
	const FVector Origin(0,0,150);
	auto SpaceAt=[&](const FVector& P){return ToSDKSpace(FTransform(FQuat::Identity,P),Origin);};
	auto SdkToUE=[&](float x,float y,float z){return FVector(-z*100.0f,x*100.0f,y*100.0f+150.0f);};
	const float Ys[]={-2.0f,-1.5f,-1.0f,-0.5f,0.0f,0.5f,1.0f,1.5f,2.0f};
	const float Zs[]={-3.5f,-3.0f,-2.5f,-2.0f,-1.5f,-1.0f,-0.5f,0.0f,0.5f,1.0f,1.5f,2.0f,2.5f,3.0f,3.5f};
	bool ApiOk=true;
	for(float Y:Ys){
		FString Row=FString::Printf(TEXT("Y=%g:"),Y);
		for(float Z:Zs){
			FIMAcousticAudioFrame Frame;std::string Err;
			if(!Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,Y,Z)),SpaceAt(SdkToUE(2.0f,Y,Z)),Frame,Err)){ApiOk=false;Row+=TEXT(" E");continue;}
			Row+=FString::Printf(TEXT(" %g"),Frame.Direct.occlusion);
		}
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1WallMap%s"),*Row);
	}
	if(!ApiOk){AddError(TEXT("Evaluate failed during wall map."));return false;}
	// inc105 reverse-direction probe (single causal diagnostic): same segments
	// north->south. If SIM matches UE only in reverse, the baked triangles are
	// single-sided with flipped winding; if both directions miss, geometry is
	// missing/misplaced rather than wound wrong.
	for(float Y:Ys){
		FString Row=FString::Printf(TEXT("Y=%g:"),Y);
		for(float Z:Zs){
			FIMAcousticAudioFrame Frame;std::string Err;
			if(!Sim.Evaluate(7,1,SpaceAt(SdkToUE(2.0f,Y,Z)),SpaceAt(SdkToUE(-2.0f,Y,Z)),Frame,Err)){ApiOk=false;Row+=TEXT(" E");continue;}
			Row+=FString::Printf(TEXT(" %g"),Frame.Direct.occlusion);
		}
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1WallMapRev%s"),*Row);
	}
	if(!ApiOk){AddError(TEXT("Evaluate failed during reverse wall map."));return false;}
	// inc106 hand-built dynamic door through the adapter (single causal test).
	// Native H1 probe proved this world-space leaf (SDK meters, identity
	// instance) blocks the aperture. Replay it via SyncDynamicMeshes on this
	// bound scene: HANDDOOR_CLOSED expect 0 (blocked), after clearing the
	// instance HANDDOOR_OPEN expect 1 (visible). If CLOSED stays 1, the adapter
	// dynamic path is broken; if CLOSED blocks here but the UE-synced door does
	// not, the UE snapshot payload is wrong. Diagnostic only, still PASS.
	{
		FIMAcousticDynamicMeshInput HandDoor;
		HandDoor.Key = 0x1D006;
		HandDoor.GeometryHash = 0x1D006;
		const float X0=-0.20f,X1=0.25f,Y0=-1.60f,Y1=1.10f,Z0=0.45f,Z1=2.55f;
		IPLVector3 Corners[8]={{X0,Y0,Z0},{X1,Y0,Z0},{X1,Y1,Z0},{X0,Y1,Z0},{X0,Y0,Z1},{X1,Y0,Z1},{X1,Y1,Z1},{X0,Y1,Z1}};
		for(int I=0;I<8;++I){HandDoor.Geometry.Vertices.push_back(Corners[I]);}
		const int Faces[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
		for(const auto& F : Faces){IPLTriangle Tri{};Tri.indices[0]=F[0];Tri.indices[1]=F[1];Tri.indices[2]=F[2];HandDoor.Geometry.Triangles.push_back(Tri);HandDoor.Geometry.MaterialIndices.push_back(0);}
		IPLMaterial DoorMat{};DoorMat.absorption[0]=0.25f;DoorMat.absorption[1]=0.25f;DoorMat.absorption[2]=0.25f;DoorMat.scattering=0.05f;DoorMat.transmission[0]=DoorMat.transmission[1]=DoorMat.transmission[2]=0.0f;
		HandDoor.Geometry.Materials.push_back(DoorMat);
		IPLMatrix4x4 Identity{};for(int R=0;R<4;++R){for(int C=0;C<4;++C){Identity.elements[R][C]=(R==C)?1.0f:0.0f;}}
		HandDoor.Transform = Identity;
		std::string SyncErr;
		if(!Sim.SyncDynamicMeshes({HandDoor},SyncErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1HandDoor SYNC_FAIL %s"),UTF8_TO_TCHAR(SyncErr.c_str()));}
		else{
			FIMAcousticAudioFrame ShutFrame;std::string ShutErr;
			const bool ShutOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),ShutFrame,ShutErr);
			double ShutEq = 0.0, ShutSh = 0.0;
			for(float V:ShutFrame.PathEQ){ ShutEq += V; }
			for(float V:ShutFrame.PathSH){ ShutSh += double(V)*double(V); }
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1HandDoor CLOSED ok=%d occ=%g pathvalid=%d eq=%.6g sh=%.6g err=%s"),ShutOk?1:0,ShutOk?ShutFrame.Direct.occlusion:-1.0f,ShutOk?(ShutFrame.PathValid?1:0):-1,ShutEq,ShutSh,UTF8_TO_TCHAR(ShutErr.c_str()));
			std::string ClearErr;Sim.SyncDynamicMeshes({},ClearErr);
			FIMAcousticAudioFrame OpenFrame;std::string OpenErr;
			const bool OpenOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),OpenFrame,OpenErr);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1HandDoor OPEN ok=%d occ=%g pathvalid=%d err=%s"),OpenOk?1:0,OpenOk?OpenFrame.Direct.occlusion:-1.0f,OpenOk?(OpenFrame.PathValid?1:0):-1,UTF8_TO_TCHAR(OpenErr.c_str()));
		}
	}
	// inc110 transformed-box cell (single causal test): SAME analytic 12-tri
	// box as HandDoor but carried by ClosedT translation (verts localized by
	// subtracting the translation) instead of world-space identity. Fresh key.
	// Seals (pv=0) => instance-transform path sound, 48-tri Cube data stands
	// alone indicted. Conducts (pv=1) => instance transforms misplace sealed
	// panels in the sim (runtime door suspect), mesh data exonerated.
	{
		FIMAcousticDynamicMeshInput TBox;
		TBox.Key = 0x1D009; TBox.GeometryHash = 0x1D009;
		const float X0=-0.20f,X1=0.25f,Y0=-1.60f,Y1=1.10f,Z0=0.45f,Z1=2.55f;
		IPLVector3 LC[8]={{X0,Y0+0.25f,Z0-1.5f},{X1,Y0+0.25f,Z0-1.5f},{X1,Y1+0.25f,Z0-1.5f},{X0,Y1+0.25f,Z0-1.5f},{X0,Y0+0.25f,Z1-1.5f},{X1,Y0+0.25f,Z1-1.5f},{X1,Y1+0.25f,Z1-1.5f},{X0,Y1+0.25f,Z1-1.5f}};
		for(int I=0;I<8;++I){TBox.Geometry.Vertices.push_back(LC[I]);}
		const int TF[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
		for(const auto& F : TF){IPLTriangle Tri{};Tri.indices[0]=F[0];Tri.indices[1]=F[1];Tri.indices[2]=F[2];TBox.Geometry.Triangles.push_back(Tri);TBox.Geometry.MaterialIndices.push_back(0);}
		IPLMaterial TM{};TM.absorption[0]=TM.absorption[1]=TM.absorption[2]=0.25f;TM.scattering=0.05f;
		TBox.Geometry.Materials.push_back(TM);
		IPLMatrix4x4 CT{};for(int R=0;R<4;++R){for(int C=0;C<4;++C){CT.elements[R][C]=(R==C)?1.0f:0.0f;}}
		CT.elements[0][3]=0.0f;CT.elements[1][3]=-0.25f;CT.elements[2][3]=1.5f;
		TBox.Transform = CT;
		std::string TErr;
		if(!Sim.SyncDynamicMeshes({TBox},TErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1TransBox SYNC_FAIL %s"),UTF8_TO_TCHAR(TErr.c_str()));}
		else{
			FIMAcousticAudioFrame TFrame;std::string TOpErr;
			const bool TOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),TFrame,TOpErr);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1TransBox ok=%d occ=%g pathvalid=%d err=%s"),TOk?1:0,TOk?TFrame.Direct.occlusion:-1.0f,TOk?(TFrame.PathValid?1:0):-1,UTF8_TO_TCHAR(TOpErr.c_str()));
		}
	}
	// inc107 real UE door payload (single causal test): read the editor IMH1Door
	// component exactly like the GT snapshot builder (scale into vertices,
	// rigid instance transform), print its numbers, then Sync THAT payload and
	// Evaluate the door axis. If UEDOOR blocks, the builder math is sound and
	// the W3 leak is a PIE/runtime issue; if not, the payload itself is bad.
	{
		AStaticMeshActor* DoorActor = nullptr;
		for(TActorIterator<AStaticMeshActor> It(World);It;++It){if(It->ActorHasTag(TEXT("IMH1Door"))){DoorActor=*It;break;}}
		if(!DoorActor){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor NO_DOOR"));}
		else{
			UStaticMeshComponent* Comp = DoorActor->GetStaticMeshComponent();
			const FTransform T = Comp ? Comp->GetComponentTransform() : FTransform::Identity;
			const FVector Loc = T.GetLocation(), Scl = T.GetScale3D();
			const FRotator Rot = T.GetRotation().Rotator();
			UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
			int32 NumV = 0; FVector VMin(1e9f,1e9f,1e9f), VMax(-1e9f,-1e9f,-1e9f);
			int32 NumT = 0;
			if(Mesh && Mesh->GetRenderData() && !Mesh->GetRenderData()->LODResources.IsEmpty()){
				const auto& LOD = Mesh->GetRenderData()->LODResources[0];
				NumV = int32(LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices());
				for(uint32 I=0;I<LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();++I){
					const FVector Raw(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I));
					const FVector S(Raw.X*Scl.X,Raw.Y*Scl.Y,Raw.Z*Scl.Z);
					VMin = VMin.ComponentMin(S); VMax = VMax.ComponentMax(S);
				}
				NumT = int32(LOD.IndexBuffer.GetArrayView().Num()/3);
			}
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor loc=%s scale=%s rot=%s mesh=%s verts=%d tris=%d scaled_min=%s scaled_max=%s"),*Loc.ToString(),*Scl.ToString(),*Rot.ToString(),Mesh?*Mesh->GetPathName():TEXT("null"),NumV,NumT,*VMin.ToString(),*VMax.ToString());
					// inc112 signed-volume winding check (single number): sum
					// dot(v0,cross(v1,v2))/6 over the SDK tris WITH the builder
					// Flip applied. Closed outward 220x10x270 panel ~= +0.0594
					// m^3 (sign per outward convention); near-zero or partial =
					// mixed/inward winding leaking validation rays.
					if(Mesh && NumT>0){
						const auto& LODV = Mesh->GetRenderData()->LODResources[0];
						const auto& VBV = LODV.VertexBuffers.PositionVertexBuffer;
						const auto IdxV = LODV.IndexBuffer.GetArrayView();
						const bool FlipV = T.GetDeterminant() > 0.0f;
						auto SdkOf = [&](uint32 VI)->FVector{
							const FVector Raw(VBV.VertexPosition(VI));
							const FVector S(Raw.X*Scl.X,Raw.Y*Scl.Y,Raw.Z*Scl.Z);
							return FVector(S.Y*0.01f,S.Z*0.01f,-S.X*0.01f);
						};
						double Vol = 0.0;
						for(int I=0;I<NumT;++I){
							uint32 A=IdxV[I*3+0],B=IdxV[I*3+1],C=IdxV[I*3+2];
							if(FlipV){const uint32 Tmp=B;B=C;C=Tmp;}
							const FVector VA = SdkOf(A),VB = SdkOf(B),VC = SdkOf(C);
							Vol += FVector::DotProduct(VA, FVector::CrossProduct(VB, VC)) / 6.0;
						}
						UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor VOL vol=%.6g expected=%.6g"),Vol, 2.2*0.1*2.7);
					// inc111 tri-integrity dump (diagnostic): min edge, degenerate
					// count; HandDoor reference is a clean 8v/12t box. Clean dump
					// here falsifies the mesh-data hypothesis.
					// inc112 weld/crack dump (diagnostic-only): 0.1mm-welded unique
					// verts plus boundary/nonmanifold edge counts. Closed box expects
					// boundary=0; nonzero boundary with degenerate=0 means split-vert
					// cracks that rays slip through despite clean minedge.
					{
						double MinEdge = 1e9; int Deg = 0;
						for(int I=0;I<NumT;++I){
							const uint32 A=IdxV[I*3+0],B=IdxV[I*3+1],C=IdxV[I*3+2];
							const FVector VA=SdkOf(A),VB=SdkOf(B),VC=SdkOf(C);
							const double E0=FVector::Dist(VA,VB),E1=FVector::Dist(VB,VC),E2=FVector::Dist(VC,VA);
							MinEdge=FMath::Min(MinEdge,FMath::Min(E0,FMath::Min(E1,E2)));
							if(FVector::CrossProduct(VB-VA,VC-VA).SizeSquared()<1e-12)++Deg;
						}
						TMap<FIntVector,int32> WeldId;
						TArray<int32> WeldOf;
						WeldOf.SetNum(NumV);
						int32 WeldCount = 0;
						for(int32 VI=0;VI<NumV;++VI){
							const FVector P=SdkOf(uint32(VI));
							const FIntVector Key(FMath::RoundToInt(P.X*10000.0f),FMath::RoundToInt(P.Y*10000.0f),FMath::RoundToInt(P.Z*10000.0f));
							if(int32* Found=WeldId.Find(Key)){ WeldOf[VI]=*Found; }
							else{ WeldOf[VI]=WeldCount; WeldId.Add(Key,WeldCount); ++WeldCount; }
						}
						TMap<int64,int32> EdgeUse;
						for(int I=0;I<NumT;++I){
							const uint32 A=IdxV[I*3+0],B=IdxV[I*3+1],C=IdxV[I*3+2];
							const int32 WA=WeldOf[int32(A)],WB=WeldOf[int32(B)],WC=WeldOf[int32(C)];
							const int64 E0=((int64)FMath::Min(WA,WB)<<32)|(uint32)FMath::Max(WA,WB);
							const int64 E1=((int64)FMath::Min(WB,WC)<<32)|(uint32)FMath::Max(WB,WC);
							const int64 E2=((int64)FMath::Min(WC,WA)<<32)|(uint32)FMath::Max(WC,WA);
							EdgeUse.FindOrAdd(E0)++; EdgeUse.FindOrAdd(E1)++; EdgeUse.FindOrAdd(E2)++;
						}
						int32 Boundary=0,NonManifold=0;
						for(const auto& KV:EdgeUse){ if(KV.Value==1)++Boundary; else if(KV.Value>2)++NonManifold; }
						UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor TRIDUMP tris=%d minedge=%.6g degenerate=%d rawverts=%d welded=%d boundary=%d nonmanifold=%d"),NumT,MinEdge,Deg,NumV,WeldCount,Boundary,NonManifold);
					}
					}
			if(Mesh && NumV>0 && NumT>0){
				FIMAcousticDynamicMeshInput UeDoor;
				UeDoor.Key = 0x1D007; UeDoor.GeometryHash = 0x1D007;
				const auto& LOD = Mesh->GetRenderData()->LODResources[0];
				for(uint32 I=0;I<LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();++I){
					const FVector Raw(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I));
					const FVector S(Raw.X*Scl.X,Raw.Y*Scl.Y,Raw.Z*Scl.Z);
					UeDoor.Geometry.Vertices.push_back(IPLVector3{float(S.Y*0.01f),float(S.Z*0.01f),float(-S.X*0.01f)});
				}
				const auto IdxView = LOD.IndexBuffer.GetArrayView();
				const bool Flip = T.GetDeterminant() > 0.0f;
				for(int I=0;I<NumT;++I){
					IPLTriangle Tri{};
					Tri.indices[0]=IPLint32(IdxView[I*3+0]);Tri.indices[1]=IPLint32(IdxView[I*3+1]);Tri.indices[2]=IPLint32(IdxView[I*3+2]);
					if(Flip){const IPLint32 Tmp=Tri.indices[1];Tri.indices[1]=Tri.indices[2];Tri.indices[2]=Tmp;}
					UeDoor.Geometry.Triangles.push_back(Tri);UeDoor.Geometry.MaterialIndices.push_back(0);
				}
				IPLMaterial M{};M.absorption[0]=M.absorption[1]=M.absorption[2]=0.25f;M.scattering=0.05f;
				UeDoor.Geometry.Materials.push_back(M);
				// Same math as IMToSDKDynamicTransform (BakeVolume.cpp): rigid
				// rotation basis, translation in column 3. Inlined because that
				// TU does not export the symbol to tests.
				{
					IPLMatrix4x4 DoorMat4{};
					const FQuat Rigid = T.GetRotation();
					const FVector UEAxes[3] = {FVector::ForwardVector, FVector::RightVector, FVector::UpVector};
					for(int32 Col=0;Col<3;++Col){
						const IPLVector3 Axis = ToSDKDirection(Rigid.RotateVector(UEAxes[Col]));
						DoorMat4.elements[0][Col]=Axis.x;DoorMat4.elements[1][Col]=Axis.y;DoorMat4.elements[2][Col]=Axis.z;
					}
					const IPLVector3 Tr = ToSDKPosition(T.GetLocation(), Origin);
					DoorMat4.elements[0][3]=Tr.x;DoorMat4.elements[1][3]=Tr.y;DoorMat4.elements[2][3]=Tr.z;DoorMat4.elements[3][3]=1.0f;
					UeDoor.Transform = DoorMat4;
				}
				std::string UErr;
				if(!Sim.SyncDynamicMeshes({UeDoor},UErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor SYNC_FAIL %s"),UTF8_TO_TCHAR(UErr.c_str()));}
				else{
					FIMAcousticAudioFrame UFrame;std::string UOpErr;
					const bool UOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),UFrame,UOpErr);
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor SHUT ok=%d occ=%g pathvalid=%d err=%s"),UOk?1:0,UOk?UFrame.Direct.occlusion:-1.0f,UOk?(UFrame.PathValid?1:0):-1,UTF8_TO_TCHAR(UOpErr.c_str()));
					std::string ClrErr;Sim.SyncDynamicMeshes({},ClrErr);
					// inc110 real-Cube payload at CLOSED (single causal test):
					// same vertices, translation overridden to closed SDK
					// (0,-0.25,1.5). CREATE path (fresh key after clear) then
					// UPDATE path (parked->closed, mirrors worker history).
					UeDoor.Key = 0x1D00C; UeDoor.GeometryHash = 0x1D00C;
					UeDoor.Transform.elements[0][3]=0.0f;UeDoor.Transform.elements[1][3]=-0.25f;UeDoor.Transform.elements[2][3]=1.5f;
					std::string UCcErr;
					if(!Sim.SyncDynamicMeshes({UeDoor},UCcErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor CLOSED_CREATE SYNC_FAIL %s"),UTF8_TO_TCHAR(UCcErr.c_str()));}
					else{
						FIMAcousticAudioFrame UCcF;std::string UCcOpErr;
						const bool UCcOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),UCcF,UCcOpErr);
						double UCcEq = 0.0, UCcSh = 0.0;
						for(float V:UCcF.PathEQ){ UCcEq += V; }
						for(float V:UCcF.PathSH){ UCcSh += double(V)*double(V); }
						UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor CLOSED_CREATE ok=%d occ=%g pathvalid=%d eq=%.6g sh=%.6g err=%s"),UCcOk?1:0,UCcOk?UCcF.Direct.occlusion:-1.0f,UCcOk?(UCcF.PathValid?1:0):-1,UCcEq,UCcSh,UTF8_TO_TCHAR(UCcOpErr.c_str()));
					}
					std::string UCcClrErr;Sim.SyncDynamicMeshes({},UCcClrErr);
					// inc111 winding-vs-thickness paradox (single causal pair):
					// A=real-Cube tris WITHOUT Flip at closed; B=real-Cube verts
					// with 5x UE-Y thickness (protrudes past coplanar wall faces)
					// WITH Flip at closed. Worker-ray verdict decides the fix.
					{
						const auto& LOD2 = Mesh->GetRenderData()->LODResources[0];
						IPLMatrix4x4 ClosedT{};
						{
							const FQuat Rigid = T.GetRotation();
							const FVector UEAxes[3] = {FVector::ForwardVector, FVector::RightVector, FVector::UpVector};
							for(int32 Col=0;Col<3;++Col){
								const IPLVector3 Axis = ToSDKDirection(Rigid.RotateVector(UEAxes[Col]));
								ClosedT.elements[0][Col]=Axis.x;ClosedT.elements[1][Col]=Axis.y;ClosedT.elements[2][Col]=Axis.z;
							}
							ClosedT.elements[0][3]=0.0f;ClosedT.elements[1][3]=-0.25f;ClosedT.elements[2][3]=1.5f;ClosedT.elements[3][3]=1.0f;
						}
						// Variant A: no Flip, aperture ray (outward-thin-panel cell;
						// THICK and later variants still probe the wall ray, see inc109).
						FIMAcousticDynamicMeshInput DoorA;
						DoorA.Key = 0x1D00D; DoorA.GeometryHash = 0x1D00D;
						for(uint32 I=0;I<LOD2.VertexBuffers.PositionVertexBuffer.GetNumVertices();++I){
							const FVector Raw(LOD2.VertexBuffers.PositionVertexBuffer.VertexPosition(I));
							const FVector S(Raw.X*Scl.X,Raw.Y*Scl.Y,Raw.Z*Scl.Z);
							DoorA.Geometry.Vertices.push_back(IPLVector3{float(S.Y*0.01f),float(S.Z*0.01f),float(-S.X*0.01f)});
						}
						{
							const auto IdxA = LOD2.IndexBuffer.GetArrayView();
							for(int I=0;I<NumT;++I){
								IPLTriangle Tri{};
								Tri.indices[0]=IPLint32(IdxA[I*3+0]);Tri.indices[1]=IPLint32(IdxA[I*3+1]);Tri.indices[2]=IPLint32(IdxA[I*3+2]);
								DoorA.Geometry.Triangles.push_back(Tri);DoorA.Geometry.MaterialIndices.push_back(0);
							}
						}
						IPLMaterial MA{};MA.absorption[0]=MA.absorption[1]=MA.absorption[2]=0.25f;MA.scattering=0.05f;
						DoorA.Geometry.Materials.push_back(MA);
						DoorA.Transform = ClosedT;
						std::string AErr;
						if(!Sim.SyncDynamicMeshes({DoorA},AErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor NOFLIP SYNC_FAIL %s"),UTF8_TO_TCHAR(AErr.c_str()));}
						else{
							FIMAcousticAudioFrame AF;std::string AOpErr;
							const bool AOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),AF,AOpErr);
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor NOFLIP ok=%d occ=%g pathvalid=%d err=%s"),AOk?1:0,AOk?AF.Direct.occlusion:-1.0f,AOk?(AF.PathValid?1:0):-1,UTF8_TO_TCHAR(AOpErr.c_str()));
						}
						std::string AClrErr;Sim.SyncDynamicMeshes({},AClrErr);
						// inc117 WELD (single causal test): real-Cube verts welded
						// by 0.1mm position key with remapped indices, no Flip,
						// ClosedT, aperture ray. Seals => split verts are the cause
						// (fix = weld in GT builder); leaks => internal coplanar
						// edges leak regardless of sharing (fix = coplanar merge or
						// clean-leaf requirement, needs review).
						{
							const auto& LODW = Mesh->GetRenderData()->LODResources[0];
							const uint32 WNumV = LODW.VertexBuffers.PositionVertexBuffer.GetNumVertices();
							TMap<FIntVector,int32> WlId;
							TArray<IPLVector3> WlV;
							TArray<int32> WlMap;
							WlMap.SetNum(WNumV);
							for(uint32 I=0;I<WNumV;++I){
								const FVector Raw(LODW.VertexBuffers.PositionVertexBuffer.VertexPosition(I));
								const FVector S(Raw.X*Scl.X,Raw.Y*Scl.Y,Raw.Z*Scl.Z);
								const FVector P(S.Y*0.01f,S.Z*0.01f,-S.X*0.01f);
								const FIntVector Key(FMath::RoundToInt(P.X*10000.0f),FMath::RoundToInt(P.Y*10000.0f),FMath::RoundToInt(P.Z*10000.0f));
								if(int32* F=WlId.Find(Key)){ WlMap[I]=*F; }
								else{ WlMap[I]=WlV.Num(); WlId.Add(Key,WlV.Num()); WlV.Add(IPLVector3{float(P.X),float(P.Y),float(P.Z)}); }
							}
							FIMAcousticDynamicMeshInput DoorWl;
							DoorWl.Key = 0x1D012; DoorWl.GeometryHash = 0x1D012;
							for(const auto& V:WlV){ DoorWl.Geometry.Vertices.push_back(V); }
							const auto IdxWl = LODW.IndexBuffer.GetArrayView();
							for(int I=0;I<NumT;++I){
								IPLTriangle Tri{};
								Tri.indices[0]=IPLint32(WlMap[IdxWl[I*3+0]]);Tri.indices[1]=IPLint32(WlMap[IdxWl[I*3+1]]);Tri.indices[2]=IPLint32(WlMap[IdxWl[I*3+2]]);
								DoorWl.Geometry.Triangles.push_back(Tri);DoorWl.Geometry.MaterialIndices.push_back(0);
							}
							IPLMaterial MWl{};MWl.absorption[0]=MWl.absorption[1]=MWl.absorption[2]=0.25f;MWl.scattering=0.05f;
							DoorWl.Geometry.Materials.push_back(MWl);
							DoorWl.Transform = ClosedT;
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor WELD verts=%d->%d"),WNumV,WlV.Num());
							std::string WlErr;
							if(!Sim.SyncDynamicMeshes({DoorWl},WlErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor WELD SYNC_FAIL %s"),UTF8_TO_TCHAR(WlErr.c_str()));}
							else{
								FIMAcousticAudioFrame WlF;std::string WlOpErr;
								const bool WlOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),WlF,WlOpErr);
								double WlEq = 0.0, WlSh = 0.0;
								for(float V:WlF.PathEQ){ WlEq += V; }
								for(float V:WlF.PathSH){ WlSh += double(V)*double(V); }
								UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor WELD ok=%d occ=%g pathvalid=%d eq=%.6g sh=%.6g err=%s"),WlOk?1:0,WlOk?WlF.Direct.occlusion:-1.0f,WlOk?(WlF.PathValid?1:0):-1,WlEq,WlSh,UTF8_TO_TCHAR(WlOpErr.c_str()));
							}
							std::string WlClrErr;Sim.SyncDynamicMeshes({},WlClrErr);
						}
						// inc118 SUBDIV (single causal test): clean BIGBOX extents,
						// each of its 12 tris mid-subdivided into 4 (48 tris,
						// split verts, same winding as parent), ClosedT, aperture
						// ray. Conducts => subdivision topology alone suffices
						// (fix = coplanar merge); seals => Cube positions are
						// peculiar (reopen position-level suspects, inc112 verdict
						// was premature).
						{
							FIMAcousticDynamicMeshInput DoorSd;
							DoorSd.Key = 0x1D013; DoorSd.GeometryHash = 0x1D013;
							const float SX0=-0.05f,SX1=0.05f,SY0=-1.35f,SY1=1.35f,SZ0=-1.10f,SZ1=1.10f;
							IPLVector3 SBC[8]={{SX0,SY0,SZ0},{SX1,SY0,SZ0},{SX1,SY1,SZ0},{SX0,SY1,SZ0},{SX0,SY0,SZ1},{SX1,SY0,SZ1},{SX1,SY1,SZ1},{SX0,SY1,SZ1}};
							for(int I=0;I<8;++I){DoorSd.Geometry.Vertices.push_back(SBC[I]);}
							const int SBF[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
							auto SdMid = [&](int A,int B)->int32{
								const IPLVector3 PA=DoorSd.Geometry.Vertices[size_t(A)],PB=DoorSd.Geometry.Vertices[size_t(B)];
								IPLVector3 M{};M.x=(PA.x+PB.x)*0.5f;M.y=(PA.y+PB.y)*0.5f;M.z=(PA.z+PB.z)*0.5f;
								int32 Id=int32(DoorSd.Geometry.Vertices.size());
								DoorSd.Geometry.Vertices.push_back(M);
								return Id;
							};
							for(const auto& F : SBF){
								const int32 A=F[0],B=F[1],C=F[2];
								const int32 MAB=SdMid(A,B),MBC=SdMid(B,C),MCA=SdMid(C,A);
								const int32 QQ[4][3]={{A,MAB,MCA},{MAB,B,MBC},{MCA,MBC,C},{MAB,MBC,MCA}};
								for(const auto& Q : QQ){IPLTriangle Tri{};Tri.indices[0]=Q[0];Tri.indices[1]=Q[1];Tri.indices[2]=Q[2];DoorSd.Geometry.Triangles.push_back(Tri);DoorSd.Geometry.MaterialIndices.push_back(0);}
							}
							IPLMaterial SM{};SM.absorption[0]=SM.absorption[1]=SM.absorption[2]=0.25f;SM.scattering=0.05f;
							DoorSd.Geometry.Materials.push_back(SM);
							DoorSd.Transform = ClosedT;
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor SUBDIV verts=%d tris=%d"),int32(DoorSd.Geometry.Vertices.size()),int32(DoorSd.Geometry.Triangles.size()));
							std::string SdErr;
							if(!Sim.SyncDynamicMeshes({DoorSd},SdErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor SUBDIV SYNC_FAIL %s"),UTF8_TO_TCHAR(SdErr.c_str()));}
							else{
								FIMAcousticAudioFrame SdF;std::string SdOpErr;
								const bool SdOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),SdF,SdOpErr);
								double SdEq = 0.0, SdSh = 0.0;
								for(float V:SdF.PathEQ){ SdEq += V; }
								for(float V:SdF.PathSH){ SdSh += double(V)*double(V); }
								UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor SUBDIV ok=%d occ=%g pathvalid=%d eq=%.6g sh=%.6g err=%s"),SdOk?1:0,SdOk?SdF.Direct.occlusion:-1.0f,SdOk?(SdF.PathValid?1:0):-1,SdEq,SdSh,UTF8_TO_TCHAR(SdOpErr.c_str()));
							}
							std::string SdClrErr;Sim.SyncDynamicMeshes({},SdClrErr);
						}
						// Variant B: 5x UE-Y thickness, WITH Flip, aperture ray
						// (thickness-cell rerun; wall-ray reading retired, see inc109).
						FIMAcousticDynamicMeshInput DoorB;
						DoorB.Key = 0x1D00E; DoorB.GeometryHash = 0x1D00E;
						for(uint32 I=0;I<LOD2.VertexBuffers.PositionVertexBuffer.GetNumVertices();++I){
							const FVector Raw(LOD2.VertexBuffers.PositionVertexBuffer.VertexPosition(I));
							const FVector S(Raw.X*Scl.X,Raw.Y*Scl.Y*5.0f,Raw.Z*Scl.Z);
							DoorB.Geometry.Vertices.push_back(IPLVector3{float(S.Y*0.01f),float(S.Z*0.01f),float(-S.X*0.01f)});
						}
						{
							const auto IdxB = LOD2.IndexBuffer.GetArrayView();
							const bool FlipB = T.GetDeterminant() > 0.0f;
							for(int I=0;I<NumT;++I){
								IPLTriangle Tri{};
								Tri.indices[0]=IPLint32(IdxB[I*3+0]);Tri.indices[1]=IPLint32(IdxB[I*3+1]);Tri.indices[2]=IPLint32(IdxB[I*3+2]);
								if(FlipB){const IPLint32 Tmp=Tri.indices[1];Tri.indices[1]=Tri.indices[2];Tri.indices[2]=Tmp;}
								DoorB.Geometry.Triangles.push_back(Tri);DoorB.Geometry.MaterialIndices.push_back(0);
							}
						}
						IPLMaterial MB{};MB.absorption[0]=MB.absorption[1]=MB.absorption[2]=0.25f;MB.scattering=0.05f;
						DoorB.Geometry.Materials.push_back(MB);
						DoorB.Transform = ClosedT;
						std::string BErr;
						if(!Sim.SyncDynamicMeshes({DoorB},BErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor THICK SYNC_FAIL %s"),UTF8_TO_TCHAR(BErr.c_str()));}
						else{
							FIMAcousticAudioFrame BF;std::string BOpErr;
							const bool BOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),BF,BOpErr);
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor THICK ok=%d occ=%g pathvalid=%d err=%s"),BOk?1:0,BOk?BF.Direct.occlusion:-1.0f,BOk?(BF.PathValid?1:0):-1,UTF8_TO_TCHAR(BOpErr.c_str()));
						}
						std::string BClrErr;Sim.SyncDynamicMeshes({},BClrErr);
					}
					// inc113 BIGBOX (single causal test): 12 clean tris at the
					// EXACT Cube-door extents (SDK x +-0.05, y -1.6..1.1, z
					// 0.4..2.6), identity transform, APERTURE ray (corrected inc116;
					// wall-ray reading retired). Seals => triangulation is
					// the cause (fix = clean-box leaf); leaks => thin/coplanar
					// extents leak regardless (fix = bigger overlaps).
					{
						FIMAcousticDynamicMeshInput BigBox;
						BigBox.Key = 0x1D00F; BigBox.GeometryHash = 0x1D00F;
						const float X0=-0.05f,X1=0.05f,Y0=-1.60f,Y1=1.10f,Z0=0.40f,Z1=2.60f;
						IPLVector3 BC[8]={{X0,Y0,Z0},{X1,Y0,Z0},{X1,Y1,Z0},{X0,Y1,Z0},{X0,Y0,Z1},{X1,Y0,Z1},{X1,Y1,Z1},{X0,Y1,Z1}};
						for(int I=0;I<8;++I){BigBox.Geometry.Vertices.push_back(BC[I]);}
						const int BF[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
						for(const auto& F : BF){IPLTriangle Tri{};Tri.indices[0]=F[0];Tri.indices[1]=F[1];Tri.indices[2]=F[2];BigBox.Geometry.Triangles.push_back(Tri);BigBox.Geometry.MaterialIndices.push_back(0);}
						IPLMaterial BM{};BM.absorption[0]=BM.absorption[1]=BM.absorption[2]=0.25f;BM.scattering=0.05f;
						BigBox.Geometry.Materials.push_back(BM);
						IPLMatrix4x4 BId{};for(int R=0;R<4;++R){for(int C=0;C<4;++C){BId.elements[R][C]=(R==C)?1.0f:0.0f;}}
						BigBox.Transform = BId;
						std::string BBErr;
						if(!Sim.SyncDynamicMeshes({BigBox},BBErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor BIGBOX SYNC_FAIL %s"),UTF8_TO_TCHAR(BBErr.c_str()));}
						else{
							FIMAcousticAudioFrame BBF;std::string BBOpErr;
							const bool BBOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),BBF,BBOpErr);
							double BBEq = 0.0, BBSh = 0.0;
							for(float V:BBF.PathEQ){ BBEq += V; }
							for(float V:BBF.PathSH){ BBSh += double(V)*double(V); }
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor BIGBOX ok=%d occ=%g pathvalid=%d eq=%.6g sh=%.6g err=%s"),BBOk?1:0,BBOk?BBF.Direct.occlusion:-1.0f,BBOk?(BBF.PathValid?1:0):-1,BBEq,BBSh,UTF8_TO_TCHAR(BBOpErr.c_str()));
						}
						std::string BBClrErr;Sim.SyncDynamicMeshes({},BBClrErr);
					}
					// inc114 WIDE (single causal test): real-Cube verts scaled
					// to 280x10x330 (UE-X 280 wide, UE-Z 330 high, same 10cm
					// thickness, WITH Flip) at closed center. Seals => edge
					// overlap/diffraction is the cause (fix = bigger leaf,
					// keep Cube mesh); leaks => triangulation (fix = clean-box
					// procedural leaf).
					{
						FIMAcousticDynamicMeshInput WideDoor;
						WideDoor.Key = 0x1D010; WideDoor.GeometryHash = 0x1D010;
						for(uint32 I=0;I<LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();++I){
							const FVector Raw(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I));
							const FVector S(Raw.X*Scl.X*1.2727f,Raw.Y*Scl.Y,Raw.Z*Scl.Z*1.2222f);
							WideDoor.Geometry.Vertices.push_back(IPLVector3{float(S.Y*0.01f),float(S.Z*0.01f),float(-S.X*0.01f)});
						}
						{
							const auto IdxW = LOD.IndexBuffer.GetArrayView();
							const bool FlipW = T.GetDeterminant() > 0.0f;
							for(int I=0;I<NumT;++I){
								IPLTriangle Tri{};
								Tri.indices[0]=IPLint32(IdxW[I*3+0]);Tri.indices[1]=IPLint32(IdxW[I*3+1]);Tri.indices[2]=IPLint32(IdxW[I*3+2]);
								if(FlipW){const IPLint32 Tmp=Tri.indices[1];Tri.indices[1]=Tri.indices[2];Tri.indices[2]=Tmp;}
								WideDoor.Geometry.Triangles.push_back(Tri);WideDoor.Geometry.MaterialIndices.push_back(0);
							}
						}
						IPLMaterial MW{};MW.absorption[0]=MW.absorption[1]=MW.absorption[2]=0.25f;MW.scattering=0.05f;
						WideDoor.Geometry.Materials.push_back(MW);
						{
							IPLMatrix4x4 WClosed{};
							const FQuat WRigid = T.GetRotation();
							const FVector WUEAxes[3] = {FVector::ForwardVector, FVector::RightVector, FVector::UpVector};
							for(int32 WCol=0;WCol<3;++WCol){
								const IPLVector3 WAxis = ToSDKDirection(WRigid.RotateVector(WUEAxes[WCol]));
								WClosed.elements[0][WCol]=WAxis.x;WClosed.elements[1][WCol]=WAxis.y;WClosed.elements[2][WCol]=WAxis.z;
							}
							WClosed.elements[0][3]=0.0f;WClosed.elements[1][3]=-0.25f;WClosed.elements[2][3]=1.5f;WClosed.elements[3][3]=1.0f;
							WideDoor.Transform = WClosed;
						}
						std::string WdErr;
						if(!Sim.SyncDynamicMeshes({WideDoor},WdErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor WIDE SYNC_FAIL %s"),UTF8_TO_TCHAR(WdErr.c_str()));}
						else{
							FIMAcousticAudioFrame WdF;std::string WdOpErr;
							const bool WdOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,0.0f,0.0f)),SpaceAt(SdkToUE(2.0f,0.0f,0.0f)),WdF,WdOpErr);
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor WIDE ok=%d occ=%g pathvalid=%d err=%s"),WdOk?1:0,WdOk?WdF.Direct.occlusion:-1.0f,WdOk?(WdF.PathValid?1:0):-1,UTF8_TO_TCHAR(WdOpErr.c_str()));
						}
						std::string WdClrErr;Sim.SyncDynamicMeshes({},WdClrErr);
					}
					// inc115 Plane-actor replication (single causal test): run
					// the EXACT GT builder math on /Engine/BasicShapes/Plane
					// (280x330, simulating a clean single-quad leaf) at closed
					// center, Sync, Evaluate BOTH directions. Seals both =>
					// fixture can use Plane actors; leaks either => need the
					// procedural clean box instead.
					{
						UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
						if(!PlaneMesh){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor PLANE NO_MESH"));}
						else{
							const auto& LODP = PlaneMesh->GetRenderData()->LODResources[0];
							const uint32 PNumV = LODP.VertexBuffers.PositionVertexBuffer.GetNumVertices();
							const uint32 PNumT = LODP.IndexBuffer.GetArrayView().Num()/3;
							UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor PLANE verts=%u tris=%u"),PNumV,PNumT);
							// Leaf 280 wide (UE-X) x 330 high (UE-Z), 10cm
							// thick illusion via single quad at partition plane
							// UE-Y=0; mimic actor scale (2.8,1.0,3.3) on a 100cm
							// plane (Plane is 100x100cm in XY). Closed center
							// UE (-150,0,125).
							const FVector PScl(2.8f,1.0f,3.3f);
							const FVector PLoc(-150.0f,0.0f,125.0f);
							FIMAcousticDynamicMeshInput PlaneDoor;
							PlaneDoor.Key = 0x1D011; PlaneDoor.GeometryHash = 0x1D011;
							for(uint32 I=0;I<PNumV;++I){
								const FVector Raw(LODP.VertexBuffers.PositionVertexBuffer.VertexPosition(I));
								const FVector Sv(Raw.X*PScl.X,Raw.Y*PScl.Y,Raw.Z*PScl.Z);
								// Plane lies in XY facing +Z; rotate -90deg pitch
								// so it faces +Y (north): (x,y,z)->(x,z,-y)? Use
								// actor-rotation equivalent: Rotator(0,0,0) keeps
								// XY plane; instead map plane XY to wall XZ by
								// axis swap here and identity instance rotation.
								// UE wall plane axes: X=width, Z=height. Plane
								// local X->width, Y->height: emit (x, 0, y).
								const FVector W(PLoc.X+Sv.X, PLoc.Y, PLoc.Z+Sv.Y);
								PlaneDoor.Geometry.Vertices.push_back(IPLVector3{float((W.Y-0.0f)*0.01f),float((W.Z-150.0f)*0.01f),float(-(W.X-0.0f)*0.01f)});
							}
							{
								const auto IdxP = LODP.IndexBuffer.GetArrayView();
								for(uint32 I=0;I<PNumT;++I){
									IPLTriangle Tri{};
									Tri.indices[0]=IPLint32(IdxP[I*3+0]);Tri.indices[1]=IPLint32(IdxP[I*3+1]);Tri.indices[2]=IPLint32(IdxP[I*3+2]);
									{const IPLint32 Tmp=Tri.indices[1];Tri.indices[1]=Tri.indices[2];Tri.indices[2]=Tmp;}
									PlaneDoor.Geometry.Triangles.push_back(Tri);PlaneDoor.Geometry.MaterialIndices.push_back(0);
								}
							}
							IPLMaterial PM{};PM.absorption[0]=PM.absorption[1]=PM.absorption[2]=0.25f;PM.scattering=0.05f;
							PlaneDoor.Geometry.Materials.push_back(PM);
							IPLMatrix4x4 PId{};for(int R=0;R<4;++R){for(int C=0;C<4;++C){PId.elements[R][C]=(R==C)?1.0f:0.0f;}}
							PlaneDoor.Transform = PId;
							std::string PlErr;
							if(!Sim.SyncDynamicMeshes({PlaneDoor},PlErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor PLANE SYNC_FAIL %s"),UTF8_TO_TCHAR(PlErr.c_str()));}
							else{
								FIMAcousticAudioFrame PlF;std::string PlOpErr;
								const bool PlOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),PlF,PlOpErr);
								UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor PLANE_SN ok=%d occ=%g pathvalid=%d err=%s"),PlOk?1:0,PlOk?PlF.Direct.occlusion:-1.0f,PlOk?(PlF.PathValid?1:0):-1,UTF8_TO_TCHAR(PlOpErr.c_str()));
								FIMAcousticAudioFrame PlR;std::string PlROpErr;
								const bool PlROk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(2.0f,-0.25f,1.5f)),SpaceAt(SdkToUE(-2.0f,-0.25f,1.5f)),PlR,PlROpErr);
								UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UeDoor PLANE_NS ok=%d occ=%g pathvalid=%d err=%s"),PlROk?1:0,PlROk?PlR.Direct.occlusion:-1.0f,PlROk?(PlR.PathValid?1:0):-1,UTF8_TO_TCHAR(PlROpErr.c_str()));
							}
							std::string PlClrErr;Sim.SyncDynamicMeshes({},PlClrErr);
						}
					}
				}
			}
		}
	}
	// inc108 worker-ray with hand door (single causal test): the exact W3
	// source/listener pair (SDK -2,0,0 -> 2,0,0; direct through solid wall at
	// UE X=0, indirect via aperture). Hand door re-synced at closed. Expect
	// occ=0 and pathvalid=0. If pathvalid=1 here, the door cannot seal THIS
	// ray even with perfect payload (geometry/semantics); if 0, the payload
	// path is proven and the W3 leak is a PIE/worker runtime issue.
	{
		FIMAcousticDynamicMeshInput WDoor;
		WDoor.Key = 0x1D008; WDoor.GeometryHash = 0x1D008;
		const float X0=-0.20f,X1=0.25f,Y0=-1.60f,Y1=1.10f,Z0=0.45f,Z1=2.55f;
		IPLVector3 WC[8]={{X0,Y0,Z0},{X1,Y0,Z0},{X1,Y1,Z0},{X0,Y1,Z0},{X0,Y0,Z1},{X1,Y0,Z1},{X1,Y1,Z1},{X0,Y1,Z1}};
		for(int I=0;I<8;++I){WDoor.Geometry.Vertices.push_back(WC[I]);}
		const int WF[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
		for(const auto& F : WF){IPLTriangle Tri{};Tri.indices[0]=F[0];Tri.indices[1]=F[1];Tri.indices[2]=F[2];WDoor.Geometry.Triangles.push_back(Tri);WDoor.Geometry.MaterialIndices.push_back(0);}
		IPLMaterial WM{};WM.absorption[0]=WM.absorption[1]=WM.absorption[2]=0.25f;WM.scattering=0.05f;
		WDoor.Geometry.Materials.push_back(WM);
		IPLMatrix4x4 WId{};for(int R=0;R<4;++R){for(int C=0;C<4;++C){WId.elements[R][C]=(R==C)?1.0f:0.0f;}}
		WDoor.Transform = WId;
		std::string WSErr;
		if(!Sim.SyncDynamicMeshes({WDoor},WSErr)){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1WorkerRay SYNC_FAIL %s"),UTF8_TO_TCHAR(WSErr.c_str()));}
		else{
			FIMAcousticAudioFrame WFrame;std::string WOpErr;
			const bool WOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,0.0f,0.0f)),SpaceAt(SdkToUE(2.0f,0.0f,0.0f)),WFrame,WOpErr);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1WorkerRay SHUT ok=%d occ=%g pathvalid=%d err=%s"),WOk?1:0,WOk?WFrame.Direct.occlusion:-1.0f,WOk?(WFrame.PathValid?1:0):-1,UTF8_TO_TCHAR(WOpErr.c_str()));
			std::string WClrErr;Sim.SyncDynamicMeshes({},WClrErr);
			FIMAcousticAudioFrame WOpen;std::string WOpenErr;
			const bool WOpenOk = Sim.Evaluate(7,1,SpaceAt(SdkToUE(-2.0f,0.0f,0.0f)),SpaceAt(SdkToUE(2.0f,0.0f,0.0f)),WOpen,WOpenErr);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1WorkerRay OPEN ok=%d occ=%g pathvalid=%d err=%s"),WOpenOk?1:0,WOpenOk?WOpen.Direct.occlusion:-1.0f,WOpenOk?(WOpen.PathValid?1:0):-1,UTF8_TO_TCHAR(WOpenErr.c_str()));
		}
	}
	// UE-side ground truth: trace the EDITOR world along the same segments and
	// print the matching map. Cells where UE and SIM disagree localize the bug.
	FCollisionObjectQueryParams ObjQuery;
	ObjQuery.AddObjectTypesToQuery(ECC_WorldStatic);
	for(float Y:Ys){
		FString Row=FString::Printf(TEXT("Y=%g:"),Y);
		for(float Z:Zs){
			FHitResult Hit;
			const bool bHit=World->LineTraceSingleByObjectType(Hit,SdkToUE(-2.0f,Y,Z),SdkToUE(2.0f,Y,Z),ObjQuery);
			Row+=bHit?TEXT(" H"):TEXT(" .");
		}
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UETrace%s"),*Row);
	}
	{
		FHitResult HitA;
		const bool bHitA=World->LineTraceSingleByObjectType(HitA,SdkToUE(-2.0f,0.0f,0.0f),SdkToUE(2.0f,0.0f,0.0f),ObjQuery);
		if(bHitA&&HitA.GetActor()){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UEHitA actor=%s loc=%s"),*HitA.GetActor()->GetName(),*HitA.Location.ToString());}
		else{UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UEHitA NO-HIT"));}
	}
	// UE-space dual sweep: same Y-travel segments stepped in UE X, reporting
	// UE collision vs Steam occlusion side by side in UE coords (no SDK layer).
	for(float UX=-350.0f;UX<=350.0f;UX+=25.0f)
	{
		FHitResult HitX;
		const bool bHitX=World->LineTraceSingleByObjectType(HitX,FVector(UX,-200.0f,150.0f),FVector(UX,200.0f,150.0f),ObjQuery);
		FIMAcousticAudioFrame XF;std::string XErr;
		const bool XOk=Sim.Evaluate(7,1,SpaceAt(FVector(UX,-200.0f,150.0f)),SpaceAt(FVector(UX,200.0f,150.0f)),XF,XErr);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1UESweep ux=%g uehit=%d steamok=%d steamocc=%g"),UX,bHitX?1:0,XOk?1:0,XOk?XF.Direct.occlusion:-1.0f);
	}
	// Analytic-scene control: ONE hand-built slab through raw IPL, no UE data,
	// no bake bytes. Slab mimics partition A (SDK x[-.05,.05], y[-1.5,1.5],
	// z[-3,.5]). Steam occ polarity (phonon.h: value = fraction unoccluded):
	// 1 = open/audible, 0 = blocked. Rays: through-slab expect 0 (blocked),
	// clear-miss expect 1 (open), hole-axis expect 1 (open). Diagnostic only.
	{
		IPLContext ACtx0 = IMAcousticSDKContext::GetAcousticSDKContext();
		if(ACtx0) iplContextRetain(ACtx0);
		IPLScene AScene = nullptr; IPLStaticMesh AMesh = nullptr;
		IPLSimulator ASim = nullptr; IPLSource ASrc = nullptr;
		TArray<IPLVector3> AVerts;
		TArray<IPLTriangle> ATris;
		TArray<IPLMaterial> AMats;
		TArray<IPLint32> AMatIdx;
		bool AOk = (ACtx0 != nullptr);
		std::string AErr;
		// 8 corners of the slab.
		const float AX0=-0.05f,AX1=0.05f,AY0=-1.5f,AY1=1.5f,AZ0=-3.0f,AZ1=0.5f;
		IPLVector3 ABC[8]={{AX0,AY0,AZ0},{AX1,AY0,AZ0},{AX1,AY1,AZ0},{AX0,AY1,AZ0},{AX0,AY0,AZ1},{AX1,AY0,AZ1},{AX1,AY1,AZ1},{AX0,AY1,AZ1}};
		for(int I=0;I<8;++I)AVerts.Add(ABC[I]);
		const int ABF[12][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
		for(const auto& F : ABF){IPLTriangle T{};T.indices[0]=F[0];T.indices[1]=F[1];T.indices[2]=F[2];ATris.Add(T);AMatIdx.Add(0);}
		IPLMaterial AMm{};AMm.absorption[0]=AMm.absorption[1]=AMm.absorption[2]=0.25f;AMm.scattering=0.05f;
		AMats.Add(AMm);
		auto AClean = [&](){ if(ASrc){iplSourceRelease(&ASrc);} if(ASim){iplSimulatorRelease(&ASim);} if(AMesh){iplStaticMeshRelease(&AMesh);} if(AScene){iplSceneRelease(&AScene);} if(ACtx0){iplContextRelease(&ACtx0);} };
		auto AOcc = [&](FVector SrcUE, FVector LisUE, float& OutOcc)->bool
		{
			IPLSimulationSharedInputs AShared{};
			AShared.listener = SpaceAt(LisUE);
			AShared.numRays = 1; AShared.numBounces = 1; AShared.duration = 0.1f;
			AShared.order = 1; AShared.irradianceMinDistance = 1.0f;
			iplSimulatorSetSharedInputs(ASim, IPL_SIMULATIONFLAGS_DIRECT, &AShared);
			IPLSimulationInputs AIn{};
			AIn.flags = IPL_SIMULATIONFLAGS_DIRECT;
			AIn.directFlags = IPLDirectSimulationFlags(IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION | IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
			AIn.source = SpaceAt(SrcUE);
			AIn.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
			AIn.distanceAttenuationModel.minDistance = 1.0f;
			AIn.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
			AIn.directivity.dipoleWeight = 0.0f; AIn.directivity.dipolePower = 1.0f;
			AIn.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
			AIn.occlusionRadius = 0.0f; AIn.numOcclusionSamples = 1;
			AIn.baked = IPL_FALSE;
			iplSourceSetInputs(ASrc, IPL_SIMULATIONFLAGS_DIRECT, &AIn);
			iplSimulatorCommit(ASim);
			iplSimulatorRunDirect(ASim);
			IPLSimulationOutputs AOut{};
			AOut.direct.directivity = 1.0f;
			iplSourceGetOutputs(ASrc, IPL_SIMULATIONFLAGS_DIRECT, &AOut);
			OutOcc = AOut.direct.occlusion;
			return true;
		};
		if(AOk)
		{
			IPLSceneSettings ASet{}; ASet.type = IPL_SCENETYPE_DEFAULT;
			AOk = AOk && iplSceneCreate(ACtx0, &ASet, &AScene) == IPL_STATUS_SUCCESS && AScene;
		}
		if(AOk)
		{
			IPLStaticMeshSettings AMS{};
			AMS.numVertices = AVerts.Num(); AMS.numTriangles = ATris.Num(); AMS.numMaterials = AMats.Num();
			AMS.vertices = AVerts.GetData(); AMS.triangles = ATris.GetData();
			AMS.materialIndices = AMatIdx.GetData(); AMS.materials = AMats.GetData();
			AOk = AOk && iplStaticMeshCreate(AScene, &AMS, &AMesh) == IPL_STATUS_SUCCESS && AMesh;
			if(AOk){ iplStaticMeshAdd(AMesh, AScene); iplSceneCommit(AScene); }
		}
		if(AOk)
		{
			IPLSimulationSettings ASSet{};
			ASSet.flags = IPL_SIMULATIONFLAGS_DIRECT;
			ASSet.sceneType = IPL_SCENETYPE_DEFAULT;
			ASSet.maxNumOcclusionSamples = 1; ASSet.maxNumRays = 1; ASSet.numDiffuseSamples = 1;
			ASSet.maxDuration = 0.1f; ASSet.maxOrder = 1; ASSet.maxNumSources = 2;
			ASSet.numThreads = 1; ASSet.rayBatchSize = 1; ASSet.numVisSamples = 1;
			ASSet.samplingRate = 48000; ASSet.frameSize = 1024;
			AOk = AOk && iplSimulatorCreate(ACtx0, &ASSet, &ASim) == IPL_STATUS_SUCCESS && ASim;
			if(AOk)
			{
				iplSimulatorSetScene(ASim, AScene);
				IPLSourceSettings ASS{}; ASS.flags = IPL_SIMULATIONFLAGS_DIRECT;
				AOk = AOk && iplSourceCreate(ASim, &ASS, &ASrc) == IPL_STATUS_SUCCESS && ASrc;
				if(AOk){ iplSourceAdd(ASrc, ASim); iplSimulatorCommit(ASim); }
			}
		}
		if(!AOk){ AddError(TEXT("Analytic control setup failed.")); AClean(); return false; }
		{
			float Occ = -1; const FVector HoleA(-150,-200,150), HoleB(-150,200,150);
			const FVector SlabA(100,-200,150), SlabB(100,200,150);
			const FVector MissA(-150,-200,150), MissB(-150,200,-50);
			AOcc(SlabA, SlabB, Occ);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticAnalytic slab_through occ=%g (expect 0=blocked)"),Occ);
			AOcc(MissA, MissB, Occ);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticAnalytic clear_miss occ=%g (expect 1=open)"),Occ);
			AOcc(HoleA, HoleB, Occ);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticAnalytic hole_axis occ=%g (expect 1=open)"),Occ);
		}
		AClean();
	}
	// Grid-phase diagnostic: same Y-travel ray at several UE X under default
	// Hybrid options. Probe grid is 100cm (63 probes); X=-150 sits midway
	// between grid lines, X=-200/-100/0 on grid. If only on-grid yields path
	// energy, pathing quantizes to probes (visRadius boundary).
	{
		FIMAcousticPathingOptions DOpt = FIMAcousticPathingOptions::DefaultHybrid();
		std::string DSErr;
		const bool DSetOk = Sim.SetPathingOptions(DOpt, DSErr);
		const float UXs[4] = {-200.0f, -150.0f, -100.0f, 0.0f};
		for(int I = 0; I < 4; ++I)
		{
			FIMAcousticAudioFrame VF; std::string VErr;
			const bool EvOk = DSetOk && Sim.Evaluate(0xB0 + I, 1, SpaceAt(FVector(UXs[I],-200.0f,150.0f)), SpaceAt(FVector(UXs[I],200.0f,150.0f)), VF, VErr);
			const float EQ = VF.PathEQ[0] + VF.PathEQ[1] + VF.PathEQ[2];
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticGridPhase ux=%g set=%d ev=%d occ=%g pathvalid=%d eqsum=%.6g err=%s"),UXs[I],DSetOk?1:0,EvOk?1:0,EvOk?VF.Direct.occlusion:-1.0f,EvOk?(VF.PathValid?1:0):-1,EQ,UTF8_TO_TCHAR(VErr.c_str()));
		}
	}
	AddInfo(TEXT("Wall map printed; see AcousticH1WallMap rows."));
	Volume->GenerateProbes();
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1BakeInventory tris=%d probes=%d issues=%d status=%s"),Volume->ExportedTriangles,Volume->GeneratedProbes,Volume->SceneIssues.Num(),*Volume->Status);
	for(const FString& Issue:Volume->SceneIssues){UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1BakeIssue %s"),*Issue);}
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticH1StaticOcclusion PASS diagnostic-map-complete"));
	UE_LOG(LogTemp,Display,TEXT("IMExitEditor PASS"));
	UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticH1StaticOcclusion PASS"));
	return true;
}
#endif
