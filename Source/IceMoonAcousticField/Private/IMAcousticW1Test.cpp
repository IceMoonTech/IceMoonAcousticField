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
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "IMAcousticSimulation.h"
#include "IMAcousticCoordinates.h"
#include "IMAcousticTestSupport.h"

namespace IMAcousticW1TestPrivate
{
constexpr const TCHAR* W1Map=TEXT("/IceMoonAcousticField/Tests/IM_W1Door");
// Preserve the original moving-route cue margin; fail-closed transitions must
// not be hidden by weakening the product acceptance threshold.
constexpr double IMW1MinEarBalance=0.02;
float W1OriginalBackgroundVolume=1;
bool W1OriginalBackgroundAudio=false;
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
uint64 W1ExportComplete(const std::atomic<uint64>* Done, uint32 Capacity, uint64 Pushes)
{
	const uint64 Limit = Pushes < Capacity ? Pushes : Capacity;
	for (uint64 I = 0; I < Limit; ++I)
	{
		if (Done[I].load(std::memory_order_acquire) != I + 1) { return I; }
	}
	return Limit;
}
void W1ExportProbeTrace(FIMAcousticDeviceBridge* Bridge, const TArray<FString>& RouteWindows, const FString& EvidenceDir,
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

const TCHAR* IM_W1SourceRejectName(EIMAcousticProbeReject Reject)
{
	switch (Reject)
	{
	case EIMAcousticProbeReject::Accepted: return TEXT("Accepted");
	case EIMAcousticProbeReject::BadBlockShape: return TEXT("BadBlockShape");
	case EIMAcousticProbeReject::Bypassed: return TEXT("Bypassed");
	case EIMAcousticProbeReject::MissingResult: return TEXT("MissingResult");
	case EIMAcousticProbeReject::StaleResult: return TEXT("StaleResult");
	case EIMAcousticProbeReject::RendererFailed: return TEXT("RendererFailed");
	default: return TEXT("Unknown");
	}
}

void W1ExportMetaSoundTrace(const FIMAcousticMetaSoundContextPtr& Context, const FString& EvidenceDir)
{
	if (!Context.IsValid())
	{
		FFileHelper::SaveStringToFile(TEXT("{\"available\":false,\"reason\":\"no MetaSound context observed\"}"),
			*FPaths::Combine(EvidenceDir, TEXT("IM_metasound_trace_missing.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return;
	}

	const uint32 SourceClaimed = Context->CapturedSourceBlockClaimCount.load(std::memory_order_acquire);
	const uint32 SourceCapacity = uint32(Context->CapturedSourceBlocks.Num());
	const uint32 SourcePublished = Context->CapturedSourceBlockCount.load(std::memory_order_acquire);
	const uint32 SourceCaptured = FMath::Min<uint32>(SourcePublished, SourceCapacity);
	const uint32 SourceOverflow = SourceClaimed > SourceCapacity ? SourceClaimed - SourceCapacity : 0;
	uint32 SourceAccepted = 0;
	uint32 SourcePath = 0;
	uint32 SourceRejected = 0;
	uint32 SourcePrimingRows = 0;
	uint32 SourceRejectedPrimingRows = 0;

	FString SourceRows;
	SourceRows.Reserve(FMath::Min<uint32>(SourceCaptured, 4096u) * 256u);
	SourceRows += TEXT("block,voice,callback_audio_id,result_audio_id,result_world_generation,result_voice_generation,input_spatial_version,output_spatial_version,result_spatial_version,result_sequence,snapshot_captured_seconds,consumed_seconds,age_ms,listener_x,listener_y,listener_z,dir_x,dir_y,dir_z,occlusion,distance_gain,routes,direct_valid,path_valid,reject,reject_name,reject_detail,render_failure,fallback,input_delay_priming,reset_reason,input_energy,direct_energy,path_energy,output_energy,rendered_at,rejected_at,callback_distance_cm,degraded_gain\n");
	for (uint32 I = 0; I < SourceCaptured; ++I)
	{
		const FIMAcousticBlockProbe& E = Context->CapturedSourceBlocks[int32(I)];
		const bool bRejected = E.Reject != EIMAcousticProbeReject::Accepted;
		if (bRejected)
		{
			++SourceRejected;
			if (E.InputDelayPriming != 0) ++SourceRejectedPrimingRows;
		}
		else
		{
			++SourceAccepted;
			if ((E.Routes & 2u) != 0 && E.OutputEnergy > 1e-9) ++SourcePath;
		}
		if (E.InputDelayPriming != 0) ++SourcePrimingRows;

		SourceRows += FString::Printf(TEXT("%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%s,%u,%u,%u,%u,%u,%.17g,%.17g,%.17g,%.17g,%llu,%llu,%.9g,%.9g\n"),
			E.Block, E.Voice, E.CallbackAudioComponentId, E.ResultAudioComponentId, E.ResultWorldGeneration,
			E.ResultVoiceGeneration, E.InputSpatialVersion, E.OutputSpatialVersion, E.ResultSpatialVersion,
			E.ResultSequence, E.SnapshotCaptured, E.ConsumedSeconds, E.AgeMs,
			E.ListenerX, E.ListenerY, E.ListenerZ, E.DirX, E.DirY, E.DirZ, E.Occlusion, E.DistanceGain,
			E.Routes, E.DirectValid, E.PathValid, uint32(E.Reject), IM_W1SourceRejectName(E.Reject), E.RejectDetail,
			E.RenderFailure, E.Fallback, E.InputDelayPriming, E.ResetReason, E.InputEnergy, E.DirectEnergy,
			E.PathEnergy, E.OutputEnergy, E.RenderedAt, E.RejectedAt, E.CallbackDistanceCm, E.DegradedGain);
	}
	const bool SourceRowsSaved = FFileHelper::SaveStringToFile(SourceRows,
		*FPaths::Combine(EvidenceDir, TEXT("IM_metasound_source_blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const uint32 EnvPushes = Context->CapturedBlockCount.load(std::memory_order_acquire);
	const uint32 EnvLimit = FMath::Min<uint32>(EnvPushes, uint32(Context->CapturedBlocks.Num()));
	const uint32 EnvFrames = Context->CapturedEnvironmentFrames.load(std::memory_order_acquire);
	uint32 WetBlocks = 0;
	for (uint32 I = 0; I < EnvLimit; ++I)
	{
		const FIMAcousticMetaSoundBlock& E = Context->CapturedBlocks[int32(I)];
		const uint32 Begin = uint32(E.Frame);
		const uint32 End = FMath::Min<uint32>(Begin + FIMAcousticMetaSoundContext::Frames, EnvFrames);
		bool bNonzero = false;
		for (uint32 F = Begin; F < End; ++F)
		{
			if (Context->CapturedWet.IsValidIndex(int32(2 * F))
				&& (FMath::Abs(Context->CapturedWet[int32(2 * F)]) > 1e-9f || FMath::Abs(Context->CapturedWet[int32(2 * F + 1)]) > 1e-9f))
			{
				bNonzero = true;
				break;
			}
		}
		if (bNonzero) ++WetBlocks;
	}

	const uint64 SourcePrimingBlocks = Context->SourceDelayPrimingBlocks.load(std::memory_order_acquire);
	const FString Summary = FString::Printf(TEXT("{\"available\":true,\"telemetry\":\"MetaSound.CapturedSourceBlocks+CapturedBlocks\",\"source_trace\":\"IM_metasound_source_blocks.csv\",\"source_trace_saved\":%s,\"source_blocks\":%u,\"source_captured\":%u,\"source_claimed\":%u,\"source_capacity\":%u,\"source_overflow\":%u,\"source_accepted\":%u,\"source_path_nonzero\":%u,\"source_rejected\":%u,\"source_priming_rows\":%u,\"source_rejected_priming_rows\":%u,\"source_delay_priming_blocks\":%llu,\"source_input_delay_blocks\":%u,\"source_input_delay_frames\":%u,\"source_frames\":%llu,\"source_captured_frames\":%u,\"environment_blocks\":%u,\"environment_wet_nonzero\":%u,\"environment_frames\":%u,\"legacy_block_pushes\":%llu}"),
		SourceRowsSaved ? TEXT("true") : TEXT("false"), SourceCaptured, SourceCaptured, SourceClaimed, SourceCapacity, SourceOverflow,
		SourceAccepted, SourcePath, SourceRejected, SourcePrimingRows, SourceRejectedPrimingRows, SourcePrimingBlocks,
		uint32(FIMAcousticMetaSoundContext::SourceInputDelayBlocks), uint32(FIMAcousticMetaSoundContext::SourceInputDelayFrames),
		Context->SourceFrames.load(std::memory_order_acquire), Context->CapturedSourceFrames.load(std::memory_order_acquire),
		EnvLimit, WetBlocks, EnvFrames, Context->Device.IsValid() ? Context->Device->BlockProbePushes.load(std::memory_order_relaxed) : 0);
	FFileHelper::SaveStringToFile(Summary, *FPaths::Combine(EvidenceDir, TEXT("IM_metasound_trace_summary.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticW1MetaSoundTrace captured=%u claimed=%u capacity=%u overflow=%u priming_rows=%u priming_blocks=%llu rejected=%u trace_saved=%d evidence=%s"),
		SourceCaptured, SourceClaimed, SourceCapacity, SourceOverflow, SourcePrimingRows, SourcePrimingBlocks, SourceRejected, SourceRowsSaved ? 1 : 0, *EvidenceDir);
}
// This test may replace an in-memory binding, but never writes the original
// map/package. File presence is frozen as well as contents, including sidecars.
struct FIMW1FrozenFile
{
	FString Path;
	FString Sha1;
	bool bExists=false;
};

bool IMW1FileFingerprint(const FString& Path, FString& OutSha1)
{
	TArray<uint8> Bytes;
	if(!FFileHelper::LoadFileToArray(Bytes,*Path)||Bytes.IsEmpty())return false;
	OutSha1=FSHA1::HashBuffer(Bytes.GetData(),uint64(Bytes.Num())).ToString().ToLower();
	return true;
}

bool IMW1FreezePackage(const FString& PackageName, bool bMap,
	TArray<FIMW1FrozenFile>& Files, FString& Error)
{
	if(!FPackageName::IsValidLongPackageName(PackageName))
	{Error=TEXT("W1 requires a saved mounted package: ")+PackageName;return false;}
	const FString Main=FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName,
		bMap?FPackageName::GetMapPackageExtension():FPackageName::GetAssetPackageExtension()));
	const FString Paths[]={Main,FPaths::ChangeExtension(Main,TEXT("uexp")),
		FPaths::ChangeExtension(Main,TEXT("ubulk")),FPaths::ChangeExtension(Main,TEXT("uptnl")),
		FPaths::ChangeExtension(Main,TEXT("m.ubulk"))};
	if(!IFileManager::Get().FileExists(*Main))
	{Error=TEXT("W1 refuses to create a missing fixture/package: ")+Main;return false;}
	for(const FString& Path:Paths)
	{
		if(Files.ContainsByPredicate([&Path](const FIMW1FrozenFile& F){return F.Path==Path;}))continue;
		FIMW1FrozenFile File;File.Path=Path;File.bExists=IFileManager::Get().FileExists(*Path);
		if(File.bExists&&!IMW1FileFingerprint(Path,File.Sha1))
		{Error=TEXT("W1 cannot fingerprint package file: ")+Path;return false;}
		Files.Add(MoveTemp(File));
	}
	return true;
}

bool IMW1VerifyFiles(const TArray<FIMW1FrozenFile>& Files, FString& Error)
{
	for(const FIMW1FrozenFile& File:Files)
	{
		const bool bExists=IFileManager::Get().FileExists(*File.Path);
		FString Hash;
		if(bExists!=File.bExists||(bExists&&(!IMW1FileFingerprint(File.Path,Hash)||Hash!=File.Sha1)))
		{Error=TEXT("W1 package file changed; refusing overwrite/delete: ")+File.Path;return false;}
	}
	return true;
}

bool IMW1HasPIEWorld()
{
	if(GEngine)for(const FWorldContext& Context:GEngine->GetWorldContexts())
		if(Context.WorldType==EWorldType::PIE)return true;
	return GEditor&&GEditor->PlayWorld;
}

bool IMW1MemoryFingerprint(UWorld* World,AIMAcousticBakeVolume* OwnedVolume,
	FString& OutText,FString& OutHash,FString& Error)
{
	OutText.Reset();OutHash.Reset();
	if(!World){Error=TEXT("W1 memory fingerprint has no editor world.");return false;}
	TSet<UPackage*> Packages;Packages.Add(World->GetOutermost());
	for(ULevel* Level:World->GetLevels())if(Level)Packages.Add(Level->GetOutermost());
	TArray<UObject*> Objects;
	for(UPackage* Package:Packages)
	{
		TArray<UObject*> Part;
		GetObjectsWithPackage(Package,Part,EGetObjectsFlags::IncludeNestedObjects,RF_Transient);
		Objects.Append(Part);
	}
	Objects.Sort([](const UObject& A,const UObject& B){return A.GetPathName()<B.GetPathName();});
	const TSet<FName> ExpectedBakeOutputs={TEXT("BakedField"),TEXT("Status"),TEXT("SceneIssues"),
		TEXT("ExportedTriangles"),TEXT("GeneratedProbes"),TEXT("NonSolidTriangles"),TEXT("NonSolidSections")};
	for(UObject* Object:Objects)
	{
		OutText+=Object->GetPathName()+TEXT("\t")+Object->GetClass()->GetPathName()+TEXT("\n");
		for(TFieldIterator<FProperty> It(Object->GetClass());It;++It)
		{
			FProperty* Property=*It;
			if(Property->HasAnyPropertyFlags(CPF_Transient|CPF_DuplicateTransient|CPF_NonPIEDuplicateTransient|CPF_SkipSerialization))continue;
			// Only known authoring diagnostics and this test's binding may
			// change. All other serialized fields and object membership remain guarded.
			if(Object==OwnedVolume&&ExpectedBakeOutputs.Contains(Property->GetFName()))continue;
			for(int32 Index=0;Index<Property->ArrayDim;++Index)
			{
				FString Value;
				// ExportText_Direct suppresses values identical to a null/default
				// delta. Item export always includes zero/empty values as well.
				Property->ExportTextItem_Direct(Value,Property->ContainerPtrToValuePtr<void>(Object,Index),nullptr,Object,PPF_None);
				OutText+=Property->GetName()+FString::Printf(TEXT("[%d]=%d:"),Index,Value.Len())+Value+TEXT("\n");
			}
		}
	}
	if(Objects.IsEmpty()||OutText.IsEmpty()){Error=TEXT("W1 serialized memory snapshot is empty.");return false;}
	OutHash=FSHA1::HashBuffer(*OutText,uint64(OutText.Len())*sizeof(TCHAR)).ToString().ToLower();
	return true;
}

bool IMW1ValidateFileGuards(FString& Error)
{
	const FString Dir=FPaths::Combine(W1Evidence(),TEXT("restore-guard-controls"));
	if(IFileManager::Get().DirectoryExists(*Dir)||!IFileManager::Get().MakeDirectory(*Dir,true))
	{Error=TEXT("W1 file-guard controls require a fresh owned evidence directory.");return false;}
	const FString Main=FPaths::Combine(Dir,TEXT("owned-package.bin"));
	const FString Sidecar=FPaths::Combine(Dir,TEXT("owned-sidecar.bin"));
	const auto Write=[&](const FString& Path,const FString& Text)
	{return FFileHelper::SaveStringToFile(Text,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);};
	FIMW1FrozenFile Present;Present.Path=Main;Present.bExists=true;
	FIMW1FrozenFile Absent;Absent.Path=Sidecar;
	if(!Write(Main,TEXT("original-owned-control"))||!IMW1FileFingerprint(Main,Present.Sha1))
	{Error=TEXT("Cannot create W1 file-guard baseline.");return false;}
	const TArray<FIMW1FrozenFile> Files={Present,Absent};FString ExpectedFailure;
	const bool Correct=IMW1VerifyFiles(Files,ExpectedFailure);
	const bool Changed=Write(Main,TEXT("changed-owned-control"))&&!IMW1VerifyFiles(Files,ExpectedFailure);
	const bool Missing=IFileManager::Get().Delete(*Main,false,false,true)&&!IMW1VerifyFiles(Files,ExpectedFailure);
	const bool SidecarAppeared=Write(Main,TEXT("original-owned-control"))&&Write(Sidecar,TEXT("unexpected-owned-sidecar"))
		&&!IMW1VerifyFiles(Files,ExpectedFailure);
	const FString Json=FString::Printf(TEXT("{\"correct_accepted\":%s,\"changed_rejected\":%s,\"missing_rejected\":%s,\"unexpected_sidecar_rejected\":%s}"),
		Correct?TEXT("true"):TEXT("false"),Changed?TEXT("true"):TEXT("false"),
		Missing?TEXT("true"):TEXT("false"),SidecarAppeared?TEXT("true"):TEXT("false"));
	const bool Saved=Write(FPaths::Combine(Dir,TEXT("results.json")),Json);
	if(!Correct||!Changed||!Missing||!SidecarAppeared||!Saved)
	{Error=TEXT("W1 native package guards failed their correct/changed/missing/sidecar controls.");return false;}
	return true;
}

// Audit the normal UE editor writer protocol in addition to serialized values.
// BakeVolume::Tick does one owned Modify, followed by MarkPackageDirty in the
// same GT frame. A dirty bit without that writer, or any other modified object,
// must retain the world. This is not a claim to observe arbitrary raw C++ writes.
struct FIMW1MutationEvent
{
	FString Kind,Object,Package;
	uint64 Frame=0;
	bool bDirty=false;
};

struct FIMW1MutationGuard
{
	TSet<UPackage*> Packages;
	TWeakObjectPtr<AIMAcousticBakeVolume> OwnedVolume;
	TWeakObjectPtr<USceneComponent> OwnedRoot;
	TArray<FIMW1MutationEvent> Events;
	FDelegateHandle ModifiedHandle,MarkedHandle,DirtyHandle;

	~FIMW1MutationGuard(){Stop();}
	void Stop()
	{
		if(ModifiedHandle.IsValid())FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
		if(MarkedHandle.IsValid())UPackage::PackageMarkedDirtyEvent.Remove(MarkedHandle);
		if(DirtyHandle.IsValid())UPackage::PackageDirtyStateChangedEvent.Remove(DirtyHandle);
		ModifiedHandle.Reset();MarkedHandle.Reset();DirtyHandle.Reset();
	}
	void Start(UWorld* World,AIMAcousticBakeVolume* Volume)
	{
		Stop();Packages.Reset();Events.Reset();OwnedVolume=Volume;OwnedRoot=Volume?Volume->GetRootComponent():nullptr;
		Packages.Add(World->GetOutermost());
		for(ULevel* Level:World->GetLevels())if(Level)Packages.Add(Level->GetOutermost());
		ModifiedHandle=FCoreUObjectDelegates::OnObjectModified.AddLambda([this](UObject* Object)
		{
			if(Object&&Packages.Contains(Object->GetOutermost()))
				Events.Add({TEXT("Modify"),Object->GetPathName(),Object->GetOutermost()->GetName(),GFrameCounter,Object->GetOutermost()->IsDirty()});
		});
		MarkedHandle=UPackage::PackageMarkedDirtyEvent.AddLambda([this](const UPackage* Package,bool bWasDirty)
		{
			if(Packages.Contains(const_cast<UPackage*>(Package)))
				Events.Add({TEXT("MarkDirty"),FString(),Package->GetName(),GFrameCounter,Package->IsDirty()});
		});
		DirtyHandle=UPackage::PackageDirtyStateChangedEvent.AddLambda([this](const UPackage* Package)
		{
			if(Packages.Contains(const_cast<UPackage*>(Package)))
				Events.Add({TEXT("DirtyState"),FString(),Package->GetName(),GFrameCounter,Package->IsDirty()});
		});
	}
	bool Validate(FString& Error) const
	{
		AIMAcousticBakeVolume* Volume=OwnedVolume.Get();
		USceneComponent* Root=OwnedRoot.Get();
		if(!Volume||!Root||Root!=Volume->GetRootComponent()||Root->GetOwner()!=Volume
			||Root->IsCreatedByConstructionScript())
		{Error=TEXT("W1 mutation guard lost its exact owned volume/native root.");return false;}
		int32 Modified=0,RootModified=0,Marked=0,Dirty=0;uint64 OwnedFrame=0;
		for(const FIMW1MutationEvent& Event:Events)
		{
			if(Event.Kind==TEXT("Modify"))
			{
				if(Event.Object==Volume->GetPathName()){++Modified;OwnedFrame=Event.Frame;}
				else if(Event.Object==Root->GetPathName())++RootModified;
				else {Error=TEXT("Unexpected UE Modify writer: ")+Event.Object;return false;}
			}
			else if(Event.Kind==TEXT("MarkDirty"))++Marked;
			else if(Event.Kind==TEXT("DirtyState"))++Dirty;
		}
		// UE 5.8 AActor::Modify modifies this native root too. The fresh trace
		// and Actor.cpp call chain establish exactly two Modify notifications,
		// three MarkDirty notifications and one clean->dirty transition for
		// BakeVolume's Modify + binding MarkPackageDirty in the same GT frame.
		// A root-only Modify, additional writer, or extra event remains invalid.
		if(Modified!=1||RootModified!=1||Marked!=3||Dirty!=1)
		{Error=TEXT("W1 writer/dirty events differ from its exact actor/native-root binding update.");return false;}
		for(const FIMW1MutationEvent& Event:Events)
		{
			if(Modified!=1||Event.Frame!=OwnedFrame||Event.Package!=Volume->GetOutermost()->GetName()||!Event.bDirty)
			{Error=TEXT("Unattributed W1 package mutation; refusing dirty clear/reload.");return false;}
		}
		for(UPackage* Package:Packages)
		{
			const bool bOwnedDirty=Package==Volume->GetOutermost()&&Modified==1;
			if(Package->IsDirty()!=bOwnedDirty)
			{Error=TEXT("Unattributed W1 package dirty state: ")+Package->GetName();return false;}
		}
		return true;
	}
};

struct FIMW1RunState
{
	double Started=FPlatformTime::Seconds();
	double CleanupStarted=0;
	FString StartupMap,OriginalBakeObject,OriginalBakePackage;
	FString GeneratedBakeObject,GeneratedBakePackage,AudioMessage,RestoreError;
	FString MemoryBefore,MemorySha1Before,MemorySha1After;
	FString PositiveGeometryFingerprint;
	TArray<FIMW1FrozenFile> OriginalFiles,GeneratedFiles;
	FIMW1MutationGuard MutationGuard;
	TArray<FString> ObservedGeneratedObjects;
	bool bMutationGuardControl=false,bMutationGuardValidated=false,bGeneratedIdentityVerified=false;
	TWeakObjectPtr<AIMAcousticBakeVolume> EditorVolume;
	bool bFinishing=false,bAudioPass=false,bMapRestored=false,bGeneratedRemoved=false;
	bool bStartupRestored=false,bRestorePass=true,bSafeToChangeWorld=true,bMemoryGuardControl=false;

	void FailRestore(const FString& Error)
	{
		bRestorePass=false;
		if(!RestoreError.IsEmpty())RestoreError+=TEXT("; ");
		RestoreError+=Error;
	}

	bool FreezeCleanWorld(UWorld* World,FString& Error)
	{
		if(!World||!World->GetOutermost()||World->GetOutermost()->IsDirty())
		{Error=TEXT("W1 refuses to switch or discard a dirty/absent editor world.");return false;}
		for(ULevel* Level:World->GetLevels())
		{
			UPackage* Package=Level?Level->GetOutermost():nullptr;
			if(!Package||Package->IsDirty())
			{Error=TEXT("W1 refuses a dirty/absent level package.");return false;}
			if(!IMW1FreezePackage(Package->GetName(),true,OriginalFiles,Error))return false;
		}
		return IMW1FreezePackage(World->GetOutermost()->GetName(),true,OriginalFiles,Error);
	}

	bool FreezeW1(FString& Error)
	{
		UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
		if(!World||World->GetOutermost()->GetName()!=W1Map||IMW1HasPIEWorld()||!FreezeCleanWorld(World,Error))
		{if(Error.IsEmpty())Error=TEXT("W1 fixture/editor identity mismatch.");return false;}
		int32 Count=0;
		for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It){EditorVolume=*It;++Count;}
		UIMAcousticBakeAsset* Asset=EditorVolume.IsValid()?EditorVolume->BakedField.Get():nullptr;
		if(Count!=1||!Asset||Asset->GetOutermost()->IsDirty())
		{Error=TEXT("W1 requires exactly one volume with an original clean saved BakedField.");return false;}
		OriginalBakeObject=Asset->GetPathName();OriginalBakePackage=Asset->GetOutermost()->GetName();
		if(!IMW1FreezePackage(OriginalBakePackage,false,OriginalFiles,Error)
			||!IMW1MemoryFingerprint(World,EditorVolume.Get(),MemoryBefore,MemorySha1Before,Error))return false;
		// Counterfactual uses the actual serialized reader, then restores the
		// test-owned tag before Bake. No Modify or Save is performed.
		AActor* Control=nullptr;
		for(TActorIterator<AActor> It(World);It;++It)if(*It!=EditorVolume.Get()){Control=*It;break;}
		if(!Control){Error=TEXT("W1 has no actor for the memory Writer negative control.");return false;}
		const TArray<FName> OriginalTags=Control->Tags;
		Control->Tags.Add(TEXT("IMW1RestoreKnownBadWriter"));
		FString Changed,ChangedHash,Restored,RestoredHash;
		const bool bChangedRead=IMW1MemoryFingerprint(World,EditorVolume.Get(),Changed,ChangedHash,Error);
		Control->Tags=OriginalTags;
		const bool bRestoredRead=IMW1MemoryFingerprint(World,EditorVolume.Get(),Restored,RestoredHash,Error);
		bMemoryGuardControl=bChangedRead&&bRestoredRead&&ChangedHash!=MemorySha1Before&&RestoredHash==MemorySha1Before;
		if(!bMemoryGuardControl){if(Error.IsEmpty())Error=TEXT("W1 memory guard failed its changed/restored actor control.");return false;}
		// Negative controls do not alter serialized values. They reject a
		// non-owned actor Modify, an isolated owned-root Modify, and an
		// unattributed dirty-bit transition before observing the real bake.
		FString ExpectedFailure;
		MutationGuard.Start(World,EditorVolume.Get());
		Control->Modify(false);
		const bool bUnknownWriterRejected=!MutationGuard.Validate(ExpectedFailure);
		MutationGuard.Stop();
		if(World->GetOutermost()->IsDirty())
		{Error=TEXT("W1 Modify(false) control unexpectedly dirtied the fixture; retaining it.");return false;}
		MutationGuard.Start(World,EditorVolume.Get());
		USceneComponent* Root=EditorVolume->GetRootComponent();
		if(!Root){Error=TEXT("W1 has no native root for its root-only Modify control.");return false;}
		Root->Modify(false);
		const bool bRootOnlyWriterRejected=!MutationGuard.Validate(ExpectedFailure);
		MutationGuard.Stop();
		if(World->GetOutermost()->IsDirty())
		{Error=TEXT("W1 root Modify(false) control unexpectedly dirtied the fixture; retaining it.");return false;}
		MutationGuard.Start(World,EditorVolume.Get());
		World->GetOutermost()->SetDirtyFlag(true);
		const bool bUnattributedDirtyRejected=!MutationGuard.Validate(ExpectedFailure);
		MutationGuard.Stop();
		World->GetOutermost()->SetDirtyFlag(false); // Exact negative-control write above.
		bMutationGuardControl=bUnknownWriterRejected&&bRootOnlyWriterRejected&&bUnattributedDirtyRejected;
		if(!bMutationGuardControl){Error=TEXT("W1 mutation guards failed known-bad writer/dirty controls.");return false;}
		MutationGuard.Start(World,EditorVolume.Get());
		return true;
	}

	bool RegisterGenerated(FString& Error)
	{
		UIMAcousticBakeAsset* Asset=EditorVolume.IsValid()?EditorVolume->BakedField.Get():nullptr;
		if(!Asset||Asset->GetPathName()==OriginalBakeObject)return true;
		const FString Name=Asset->GetOutermost()->GetName();
		ObservedGeneratedObjects.AddUnique(Asset->GetPathName());
		bGeneratedRemoved=false;
		// Preserve rejected identity as evidence; an unverified identity can
		// never make the empty-file cleanup path report success.
		if(GeneratedBakePackage.IsEmpty())
		{GeneratedBakeObject=Asset->GetPathName();GeneratedBakePackage=Name;}
		const FString Prefix=TEXT("/IceMoonAcousticField/Bakes/IM_IM_W1Door_");
		FGuid Guid;
		if(!Name.StartsWith(Prefix,ESearchCase::CaseSensitive)||Name.Len()!=Prefix.Len()+32
			||!FGuid::ParseExact(Name.Right(32),EGuidFormats::Digits,Guid)||Name==OriginalBakePackage)
		{bGeneratedIdentityVerified=false;Error=TEXT("W1 generated bake identity is outside this fixture's GUID namespace.");return false;}
		if(GeneratedBakePackage!=Name)
		{bGeneratedIdentityVerified=false;Error=TEXT("W1 observed a second unexpected generated bake.");return false;}
		if(bGeneratedIdentityVerified)return IMW1VerifyFiles(GeneratedFiles,Error);
		GeneratedFiles.Reset();
		if(!IMW1FreezePackage(Name,false,GeneratedFiles,Error))return false;
		const FString OwnedBakes=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(),
			TEXT("IceMoonAcousticField/Content/Bakes")));
		for(const FIMW1FrozenFile& File:GeneratedFiles)
		{
			if(!FPaths::IsUnderDirectory(File.Path,OwnedBakes))
			{Error=TEXT("W1 generated bake is outside this physical project's plugin Bakes directory.");return false;}
		}
		bGeneratedIdentityVerified=true;
		return true;
	}
};

bool BuildW1Map(FString& Error)
{
	const FString File=FPackageName::LongPackageNameToFilename(W1Map,FPackageName::GetMapPackageExtension());
	if(!IFileManager::Get().FileExists(*File))
	{Error=TEXT("Missing saved W1 fixture; test will not reconstruct or overwrite it.");return false;}
	if(!FEditorFileUtils::LoadMap(W1Map,false,true))
	{Error=TEXT("Cannot load the saved W1 fixture.");return false;}
	return true;
}

// The positive direct control is fixed in UE world centimeters, in the
// positive-Y room. Its clearance is proved against the collector's exact
// current triangles, not inferred from a collision proxy or audible silence.
const FVector IMW1PositiveSource(-100,200,150);
const FVector IMW1PositiveListener(0,200,150);

bool IMW1VerifyPositiveGeometry(AIMAcousticBakeVolume* Volume,FString& OutFingerprint,FString& Failure)
{
	OutFingerprint.Reset();
	if(!Volume||!Volume->BakeBounds)
	{Failure=TEXT("Strict Q2 geometry owner/bounds are missing.");return false;}
	TArray<FVector> Vertices;
	TArray<FIntVector> Triangles;
	TArray<int32> MaterialIndices;
	FString Fingerprint;
	if(!Volume->IM_CaptureSceneGeometryForTest(Vertices,Triangles,MaterialIndices,Fingerprint)
		||Fingerprint.IsEmpty()||Vertices.IsEmpty()||Triangles.IsEmpty()||Triangles.Num()!=MaterialIndices.Num())
	{Failure=TEXT("Strict Q2 could not capture the exact static bake triangles.");return false;}
	const FVector Origin=Volume->BakeBounds->GetComponentLocation();
	TArray<TSharedPtr<FJsonValue>> PositiveHits,OccludedHits,DynamicEntries;
	for(const auto& Entry:Volume->DynamicBlockers)
		DynamicEntries.Add(MakeShared<FJsonValueString>(Entry?Entry->GetPathName():TEXT("<null>")));
	auto WorldVertex=[&](int32 Index)
	{
		const FVector& P=Vertices[Index];
		return FromSDKPosition(IPLVector3{float(P.X),float(P.Y),float(P.Z)},Origin);
	};
	for(int32 I=0;I<Triangles.Num();++I)
	{
		const FIntVector& T=Triangles[I];
		if(!Vertices.IsValidIndex(T.X)||!Vertices.IsValidIndex(T.Y)||!Vertices.IsValidIndex(T.Z))
		{Failure=TEXT("Strict Q2 captured an invalid triangle index.");return false;}
		const FVector A=WorldVertex(T.X),B=WorldVertex(T.Y),C=WorldVertex(T.Z);
		FVector Point,Normal;
		if(FMath::SegmentTriangleIntersection(IMW1PositiveSource,IMW1PositiveListener,A,B,C,Point,Normal))
			PositiveHits.Add(MakeShared<FJsonValueNumber>(I));
		// The original occluded route is a required negative control for the
		// coordinate conversion and triangle query; a universal no-hit result
		// cannot establish the positive pair's clearance.
		if(FMath::SegmentTriangleIntersection(FVector(0,-200,150),FVector(0,200,150),A,B,C,Point,Normal))
			OccludedHits.Add(MakeShared<FJsonValueNumber>(I));
	}
	// This saved fixture has no registered dynamic geometry. Refuse a changed
	// fixture instead of silently dropping blockers from a clearance claim.
	const bool Pass=PositiveHits.IsEmpty()&&!OccludedHits.IsEmpty()&&DynamicEntries.IsEmpty();
	auto Report=MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"),TEXT("IMR53W1DirectPositiveGeometry.v1"));
	Report->SetStringField(TEXT("status"),Pass?TEXT("PASS"):TEXT("FAIL"));
	Report->SetStringField(TEXT("world_package"),Volume->GetWorld()->GetOutermost()->GetName());
	Report->SetStringField(TEXT("static_fingerprint"),Fingerprint);
	Report->SetStringField(TEXT("collector"),TEXT("IM_CaptureSceneGeometryForTest exact bake arrays"));
	Report->SetStringField(TEXT("origin_cm"),Origin.ToString());
	Report->SetStringField(TEXT("positive_source_cm"),IMW1PositiveSource.ToString());
	Report->SetStringField(TEXT("positive_listener_cm"),IMW1PositiveListener.ToString());
	Report->SetNumberField(TEXT("vertices"),Vertices.Num());
	Report->SetNumberField(TEXT("triangles"),Triangles.Num());
	Report->SetArrayField(TEXT("positive_hit_triangle_indices"),PositiveHits);
	Report->SetArrayField(TEXT("occluded_hit_triangle_indices"),OccludedHits);
	Report->SetArrayField(TEXT("registered_dynamic_blockers"),DynamicEntries);
	Report->SetBoolField(TEXT("audio_or_SDK_equivalence_claim"),false);
	FString Text;const auto Writer=TJsonWriterFactory<>::Create(&Text);
	if(!FJsonSerializer::Serialize(Report,Writer)||!IFileManager::Get().MakeDirectory(*W1Evidence(),true)
		||!FFileHelper::SaveStringToFile(Text,*FPaths::Combine(W1Evidence(),TEXT("IM_direct_positive_geometry.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{Failure=TEXT("Strict Q2 geometry observation could not be saved.");return false;}
	if(!Pass)Failure=TEXT("Strict Q2 positive pair is obstructed, negative control has no hit, or dynamic fixture changed.");
	else OutFingerprint=Fingerprint;
	return Pass;
}

class FIMAcousticW1Command final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticW1Command(FAutomationTestBase* InTest,TSharedRef<FIMW1RunState> InState,int32 InitialStage=-1):Test(InTest),State(InState),Started(InState->Started),Stage(InitialStage){}
	bool Update() override
	{
		const double Now=FPlatformTime::Seconds();
		if(Stage>=90)return Cleanup(Now);
		if(Now-Started>120)return Finish(false,TEXT("W1 UE gate timed out."));
		if(Stage==-1)
		{
			FString Error;if(!BuildW1Map(Error))return Finish(false,Error);
			Stage=0;return false; // A world transition is the only operation in this latent update.
		}
		if(Stage==0)
		{
			FString Error;if(!State->FreezeW1(Error))return Finish(false,Error);
			Volume=State->EditorVolume;
			if(!Volume.IsValid())return Finish(false,TEXT("W1 bake volume missing."));
			// Existing fixture maps are reused, never reconstructed or cleared.
			Volume->GenerateProbes();
			if(Volume->GeneratedProbes<=0)
			{
				for(const FString& Issue:Volume->SceneIssues)UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticW1Scene %s"),*Issue);
				return Finish(false,Volume->Status);
			}
			if(!IMW1VerifyPositiveGeometry(Volume.Get(),State->PositiveGeometryFingerprint,Error))return Finish(false,Error);
			PreviousAsset=Volume->BakedField;Volume->Bake();Stage=1;return false;
		}
		if(Stage==1)
		{
			if(!Volume.IsValid())return Finish(false,TEXT("Bake owner destroyed."));
			if(Volume->Status.Contains(TEXT("failed"),ESearchCase::IgnoreCase)||Volume->Status.Contains(TEXT("discarded")))return Finish(false,Volume->Status);
			if(!Volume->BakedField||Volume->BakedField.Get()==PreviousAsset.Get())return false;
			FString Error;
			// Register the owned GUID package before payload/evidence I/O can fail.
			if(!State->RegisterGenerated(Error))return Finish(false,Error);
			IFileManager::Get().MakeDirectory(*W1Evidence(),true);
			UIMAcousticBakeAsset* BakeAsset=Volume->BakedField.Get();
			// Clearance is valid only for the exact static scene accepted by
			// this bake. A changed capture must fail before payload evidence.
			if(State->PositiveGeometryFingerprint.IsEmpty()||BakeAsset->SceneFingerprint!=State->PositiveGeometryFingerprint)
				return Finish(false,TEXT("Strict Q2 geometry fingerprint differs from the accepted bake payload."));
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1GeometryBinding captured=%s baked=%s match=1"),
				*State->PositiveGeometryFingerprint,*BakeAsset->SceneFingerprint);
			TArray<uint8> RawScene,RawProbes;FString PayloadError;
			if(!BakeAsset->ReadPayload(BakeAsset->WorldPackage,BakeAsset->SceneFingerprint,RawScene,RawProbes,PayloadError))
				return Finish(false,TEXT("Cannot read raw SDK payload for evidence: ")+PayloadError);
			if(!FFileHelper::SaveArrayToFile(RawScene,*FPaths::Combine(W1Evidence(),TEXT("scene.bin")))
				||!FFileHelper::SaveArrayToFile(RawProbes,*FPaths::Combine(W1Evidence(),TEXT("probes.bin"))))
				return Finish(false,TEXT("Cannot save raw SDK bake evidence."));
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Bake triangles=%d probes=%d evidence=%s"),Volume->ExportedTriangles,Volume->GeneratedProbes,*W1Evidence());
			// Start the current in-memory world with the freshly baked binding.
			// Saving the fixture would pollute subsequent native and ordinary tests.
			ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
			ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticW1Command(Test,State,2));return true;
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
				if(!SpawnW1Source(PIE))return false;return false;}
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
			if((RouteIndex==0||RouteIndex==5)&&!ListenerConverged(Bridge))
			{
				if(Now-LastDiagnostic>5)
				{
					LastDiagnostic=Now;
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1ListenerWait snaps=%llu rendered=%llu direct=%llu"),Bridge->SnapshotProbePushes.load(std::memory_order_acquire),Bridge->RenderedBlocks.load(std::memory_order_relaxed),Bridge->DirectNonzeroBlocks.load(std::memory_order_relaxed));
				}
				return false;
			}
			if(Now-CaptureStart<.3)return false; // GT mask publication plus DSP block transition.
			if(!MetaContext.IsValid())return Finish(false,TEXT("Strict Q2 requires the actual MetaSound source/environment context."));
			if(RouteIndex==5&&!Volume->DynamicBlockers.IsEmpty())return Finish(false,TEXT("Strict Q2 direct-positive dynamic fixture changed during PIE."));
			DirectBefore=Bridge->DirectNonzeroBlocks.load();PathBefore=Bridge->PathNonzeroBlocks.load();RejectedBefore=Bridge->RejectedBlocks.load();
			ReverbBefore=Bridge->ReverbNonzeroBlocks.load();
			GraphSourceBefore=GraphSourceBlocks();
			GraphEnvironmentBefore=GraphEnvironmentBlocks();
			// Begin counters precede the queued recording start. The end
			// counters below follow the synchronous recording stop. This is a
			// conservative bracket, not an exact master-sample cursor claim.
			CaptureStart=FPlatformTime::Seconds();
			UAudioMixerBlueprintLibrary::StartRecordingOutput(PIE,3);Stage=4;return false;
		}
		if(Stage==4)
		{
			if(IsMovingRoute())MovePair(Now-CaptureStart);
			if(Now-CaptureStart<(IsMovingRoute()?5:2))return false;
			UAudioMixerBlueprintLibrary::StopRecordingOutput(PIE,EAudioRecordingExportType::WavFile,RouteName(),W1Evidence());
			const uint32 GraphSourceEnd=GraphSourceBlocks();
			const uint32 GraphEnvironmentEnd=GraphEnvironmentBlocks();
			const double CaptureEnd=FPlatformTime::Seconds();
			if(!MetaContext.IsValid()||GraphSourceEnd<=GraphSourceBefore||GraphEnvironmentEnd<=GraphEnvironmentBefore
				||GraphSourceEnd>=uint32(MetaContext->CapturedSourceBlocks.Num())
				||GraphEnvironmentEnd>=uint32(MetaContext->CapturedBlocks.Num()))
				return Finish(false,TEXT("Strict Q2 capture trace is absent, empty or overflowed."));
			uint64 Direct=Bridge->DirectNonzeroBlocks.load()-DirectBefore,Path=Bridge->PathNonzeroBlocks.load()-PathBefore;
			uint64 Rejected=Bridge->RejectedBlocks.load()-RejectedBefore;
			uint64 SpatialVersionRejected=0,FailClosedRejected=0,UnexpectedRejected=0;
			IFileManager::Get().MakeDirectory(*W1Evidence(),true);
			uint64 Reverb=Bridge->ReverbNonzeroBlocks.load()-ReverbBefore;
			if(MetaContext.IsValid())
			{
				GraphSourceWindow(GraphSourceBefore,GraphSourceEnd,Direct,Path,Rejected);
				GraphSourceRejectKinds(GraphSourceBefore,GraphSourceEnd,SpatialVersionRejected,FailClosedRejected,UnexpectedRejected);
				Reverb=GraphWetWindow(GraphEnvironmentBefore,GraphEnvironmentEnd);
			}
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1RouteVec route=%s d_direct=%llu d_path=%llu d_reverb=%llu d_rej=%llu spatial_rej=%llu safe_degraded_rej=%llu unexpected_rej=%llu push=%llu/inputnz=%llu/rinputnz=%llu notfresh=%llu"),*RouteName(),Direct,Path,Reverb,Rejected,SpatialVersionRejected,FailClosedRejected,UnexpectedRejected,Bridge->PushDryCalls.load(),Bridge->PushDryInputNonzero.load(),Bridge->RenderInputNonzero.load(),Bridge->ReverbNotFreshBlocks.load());
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Intervals route=%s gt_max_us=%llu worker_max_us=%llu stale=%llu missing=%llu"),*RouteName(),Bridge->MaxSnapshotGapUs.load(),Bridge->MaxWorkerGapUs.load(),Bridge->StaleResultBlocks.load(),Bridge->MissingResultBlocks.load());
			const FString Data=FString::Printf(TEXT("{\"scope\":\"UE-open-door-isolated-route\",\"route\":\"%s\",\"direct_nonzero_blocks\":%llu,\"path_nonzero_blocks\":%llu,\"degraded_blocks\":%llu,\"spatial_version_rejections\":%llu,\"fail_closed_rejections\":%llu,\"unexpected_rejections\":%llu,\"reverb_nonzero_blocks\":%llu}"),*RouteName(),Direct,Path,Rejected,SpatialVersionRejected,FailClosedRejected,UnexpectedRejected,Reverb);
			FFileHelper::SaveStringToFile(Data,*FPaths::Combine(W1Evidence(),RouteName()+TEXT(".json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			// GT recording bracket and frozen graph cursors for a bounded
			// probe join. Do not label these as exact master PCM sample bounds.
			if (ProbeRouteWindows.Num() < 8)
			{
				ProbeRouteWindows.Add(FString::Printf(TEXT("{\"route\":\"%s\",\"capture_start\":%.6f,\"capture_end\":%.6f,\"d_direct\":%llu,\"d_path\":%llu,\"d_reverb\":%llu,\"d_rejected\":%llu,\"graph_source_begin\":%u,\"graph_source_end\":%u,\"graph_environment_begin\":%u,\"graph_environment_end\":%u,\"recording_boundary\":\"conservative_GT_bracket\",\"exact_master_sample_alignment\":false}"),
					*RouteName(), CaptureStart, CaptureEnd, Direct, Path, Reverb, Rejected,
					GraphSourceBefore,GraphSourceEnd,GraphEnvironmentBefore,GraphEnvironmentEnd));
			}
			// r4 requires total measured-window Rejected == 0, including
			// spatial-version and fail-closed blocks. Startup priming precedes
			// these frozen route baselines and remains separately observable.
			const bool DirectPass=RouteIndex==5?Direct>0:Direct==0;
			const bool PathPass=(RouteIndex==1||IsMovingRoute())?Path>0:Path==0;
			AudioPass=DirectPass&&Rejected==0&&PathPass&&(RouteIndex==2?Reverb>0:Reverb==0);Stage=5;return false;
		}
		if(Stage==5)
		{
			const FString File=FPaths::Combine(W1Evidence(),RouteName()+TEXT(".wav"));
			if(IFileManager::Get().FileSize(*File)<=44)return false;
			TArray<uint8> Bytes;FWaveModInfo WaveInfo;
			if(!FFileHelper::LoadFileToArray(Bytes,*File)||!WaveInfo.ReadWaveInfo(Bytes.GetData(),Bytes.Num()))return false;
			if(!WaveInfo.pBitsPerSample||*WaveInfo.pBitsPerSample!=16||!WaveInfo.pChannels||*WaveInfo.pChannels!=2
				||WaveInfo.SampleDataSize%4!=0)return Finish(false,TEXT("Strict Q2 requires complete stereo PCM16 frames."));
			double Energy=0,LeftEnergy=0,RightEnergy=0;
			for(uint32 I=0;I+1<WaveInfo.SampleDataSize;I+=2)
			{int16 Sample;FMemory::Memcpy(&Sample,WaveInfo.SampleDataStart+I,sizeof(Sample));const double E=double(Sample)*Sample;Energy+=E;if((I/2)%2)RightEnergy+=E;else LeftEnergy+=E;}
			const double EarBalance=Energy>0?(LeftEnergy-RightEnergy)/Energy:0;
			AudioPass=AudioPass&&(RouteIndex==0?Energy==0:Energy>0)&&WaveInfo.SampleDataSize>96000;
			// r4 fixes the physical side: the left doorway must produce
			// positive left-minus-right energy, and the 180-degree listener
			// reversal must produce a negative balance at the same threshold.
			bool CuePass=true;
			if(RouteIndex==1||RouteIndex==3)CuePass=EarBalance>IMW1MinEarBalance;
			else if(RouteIndex==4)CuePass=EarBalance<-IMW1MinEarBalance;
			AudioPass=AudioPass&&CuePass;
			FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"pcm16_energy\":%.17g,\"pcm_bytes\":%u,\"ear_balance\":%.9g}"),Energy,WaveInfo.SampleDataSize,EarBalance),*FPaths::Combine(W1Evidence(),RouteName()+TEXT("-energy.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1DirectPath evidence=%s"),*W1Evidence());
			if(!AudioPass)return Finish(false,TEXT("UE route counter/recording gate failed: ")+RouteName());
			if(++RouteIndex<6){SetRoute();CaptureStart=Now;Stage=6;return false;}
			return Finish(true,TEXT("Strict Q2 route counts, PCM16, absolute doorway cues and unobstructed direct-positive control passed; original packages still require separate restore verification."));
		}
		return false;
	}
private:
	FString RouteName() const{return RouteIndex==0?TEXT("IM_direct"):RouteIndex==1?TEXT("IM_path"):RouteIndex==2?TEXT("IM_reverb"):RouteIndex==3?TEXT("IM_moving_front"):RouteIndex==4?TEXT("IM_moving_back"):TEXT("IM_direct_positive");}
	bool IsMovingRoute() const{return RouteIndex==3||RouteIndex==4;}
	void MovePair(double T)
	{
		if(MovingSource.IsValid())MovingSource->SetWorldLocation(FVector(-25*FMath::Sin(T*.7),-200+25*FMath::Sin(T),150));
		if(MovingListener.IsValid())MovingListener->SetAudioListenerOverride(nullptr,FVector(-25*FMath::Cos(T*.9),200+25*FMath::Cos(T),150),FRotator(0,RouteIndex==4?90:-90,0));
	}
	void SetRoute()
	{
		Volume->bDirectRoute=RouteIndex==0||RouteIndex==5;
		Volume->bPathRoute=RouteIndex==1||IsMovingRoute();Volume->bReverbRoute=RouteIndex==2;
		if(IsMovingRoute())MovePair(0);
		else if(RouteIndex==5)
		{
			// Same room as the listener, 1 m apart in UE cm and away from the
			// y=0 separator. This defeats an implementation that mutes direct
			// globally while still passing the occluded doorway counterexample.
			if(MovingSource.IsValid())MovingSource->SetWorldLocation(IMW1PositiveSource);
			if(MovingListener.IsValid())MovingListener->SetAudioListenerOverride(nullptr,IMW1PositiveListener,FRotator(0,-90,0));
		}
	}
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
	void GraphSourceRejectKinds(uint32 Begin,uint32 End,uint64& SpatialVersionRejected,uint64& FailClosedRejected,uint64& UnexpectedRejected) const
	{
		SpatialVersionRejected=0;
		FailClosedRejected=0;
		UnexpectedRejected=0;
		if(!MetaContext.IsValid())return;
		const uint32 Limit=FMath::Min<uint32>(End,uint32(MetaContext->CapturedSourceBlocks.Num()));
		const uint32 First=FMath::Min<uint32>(Begin,Limit);
		for(uint32 I=First;I<Limit;++I)
		{
			const FIMAcousticBlockProbe& E=MetaContext->CapturedSourceBlocks[int32(I)];
			if(E.Reject==EIMAcousticProbeReject::Accepted)continue;
			if((E.Reject==EIMAcousticProbeReject::MissingResult||E.Reject==EIMAcousticProbeReject::StaleResult)
				&& E.RejectDetail==2048u)
			{
				++SpatialVersionRejected;
			}
			else if((E.Reject==EIMAcousticProbeReject::MissingResult||E.Reject==EIMAcousticProbeReject::StaleResult)
				&& E.RejectDetail==0u)
			{
				++FailClosedRejected;
			}
			else
			{
				++UnexpectedRejected;
			}
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
		if(State->bFinishing)return false;
		State->bFinishing=true;State->bAudioPass=Pass;State->AudioMessage=Message;
		State->CleanupStarted=FPlatformTime::Seconds();
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
		FApp::SetUnfocusedVolumeMultiplier(W1OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=W1OriginalBackgroundAudio;
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		if(State->EditorVolume.IsValid()&&!State->EditorVolume->IM_CancelAndDrainBakeForTest())
			State->FailRestore(TEXT("Owned W1 bake did not drain."));
		FString Error;
		if(!State->RegisterGenerated(Error))State->FailRestore(Error);
		if(GUnrealEd)GUnrealEd->RequestEndPlayMap();
		Stage=90;return false;
	}

	bool RestoreW1()
	{
		FString Error;
		if(!IMW1VerifyFiles(State->OriginalFiles,Error)){State->bSafeToChangeWorld=false;State->FailRestore(Error);return false;}
		if(State->OriginalBakeObject.IsEmpty())return true; // Preflight failed before W1 mutation.
		FString CurrentMemory;
		UWorld* EditorWorld=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
		if(!IMW1MemoryFingerprint(EditorWorld,State->EditorVolume.Get(),CurrentMemory,State->MemorySha1After,Error)
			||State->MemorySha1After!=State->MemorySha1Before)
		{
			State->bSafeToChangeWorld=false;
			IFileManager::Get().MakeDirectory(*W1Evidence(),true);
			FFileHelper::SaveStringToFile(State->MemoryBefore,*FPaths::Combine(W1Evidence(),TEXT("memory_before.txt")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			FFileHelper::SaveStringToFile(CurrentMemory,*FPaths::Combine(W1Evidence(),TEXT("memory_unexpected.txt")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			State->FailRestore(Error.IsEmpty()?TEXT("Unexpected serialized in-memory Writer change; refusing dirty clear/reload."):Error);return false;
		}
		State->bMutationGuardValidated=State->MutationGuard.Validate(Error);
		if(!State->bMutationGuardValidated)
		{State->bSafeToChangeWorld=false;State->FailRestore(Error);return false;}
		State->MutationGuard.Stop(); // No callbacks retain the old world across reload.
		UIMAcousticBakeAsset* Original=LoadObject<UIMAcousticBakeAsset>(nullptr,*State->OriginalBakeObject);
		if(!State->EditorVolume.IsValid()||!Original)
		{State->FailRestore(TEXT("Original editor volume/bake unavailable; refusing generated-package deletion."));return false;}
		State->EditorVolume->BakedField=Original;
		State->EditorVolume->GetOutermost()->SetDirtyFlag(false); // Only the clean, owned fixture mutated by this test.
		Volume.Reset();PreviousAsset.Reset();ProbeBridge.Reset();MetaContext.Reset();
		if(!FEditorFileUtils::LoadMap(W1Map,false,true))
		{State->FailRestore(TEXT("Cannot reload unchanged W1 fixture."));return false;}
		UWorld* World=GEditor->GetEditorWorldContext().World();
		int32 Count=0;UIMAcousticBakeAsset* Bound=nullptr;
		if(World)for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It){++Count;Bound=It->BakedField.Get();}
		State->bMapRestored=World&&World->GetOutermost()->GetName()==W1Map
			&&!World->GetOutermost()->IsDirty()&&Count==1&&Bound&&Bound->GetPathName()==State->OriginalBakeObject
			&&!Bound->GetOutermost()->IsDirty();
		if(!State->bMapRestored)State->FailRestore(TEXT("W1 reload failed original binding/dirty-state checks."));
		return State->bMapRestored;
	}

	void RemoveGenerated()
	{
		if(State->GeneratedBakePackage.IsEmpty()){State->bGeneratedRemoved=true;return;}
		if(!State->bGeneratedIdentityVerified)
		{State->FailRestore(TEXT("Unverified generated identity retained for exact outer recovery."));return;}
		FString Error;
		if(!State->bMapRestored||!IMW1VerifyFiles(State->GeneratedFiles,Error))
		{State->FailRestore(Error.IsEmpty()?TEXT("Generated bake retained because original map was not restored."):Error);return;}
		UIMAcousticBakeAsset* Asset=LoadObject<UIMAcousticBakeAsset>(nullptr,*State->GeneratedBakeObject);
		if(Asset)
		{
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public|RF_Standalone);Asset->MarkAsGarbage();
		}
		else{State->FailRestore(TEXT("Owned generated asset could not be identified for AssetRegistry deletion."));return;}
		if(UPackage* Package=FindPackage(nullptr,*State->GeneratedBakePackage))
		{Package->SetDirtyFlag(false);Package->ClearFlags(RF_Public|RF_Standalone);Package->MarkAsGarbage();}
		CollectGarbage(RF_NoFlags);
		if(FindObject<UObject>(nullptr,*State->GeneratedBakeObject)||FindPackage(nullptr,*State->GeneratedBakePackage))
		{State->FailRestore(TEXT("Owned generated asset/package survived GC; retaining its files."));return;}
		State->bGeneratedRemoved=true;
		for(const FIMW1FrozenFile& File:State->GeneratedFiles)
		{
			if(File.bExists&&!IFileManager::Get().Delete(*File.Path,false,true,true))State->bGeneratedRemoved=false;
			if(IFileManager::Get().FileExists(*File.Path))State->bGeneratedRemoved=false;
		}
		if(!State->bGeneratedRemoved)State->FailRestore(TEXT("Cannot remove the unchanged owned GUID bake package/sidecars."));
	}

	bool PublishResult()
	{
		FString Error;
		if(!IMW1VerifyFiles(State->OriginalFiles,Error))State->FailRestore(Error);
		auto Root=MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"),TEXT("IMAcousticW1Restore.v1"));
		Root->SetStringField(TEXT("file_hash_algorithm"),TEXT("SHA1"));
		Root->SetBoolField(TEXT("audio_gate_pass"),State->bAudioPass);
		Root->SetStringField(TEXT("audio_message"),State->AudioMessage);
		Root->SetBoolField(TEXT("restore_pass"),State->bRestorePass&&State->bStartupRestored&&State->bGeneratedRemoved);
		Root->SetBoolField(TEXT("map_restored"),State->bMapRestored);
		Root->SetBoolField(TEXT("startup_restored"),State->bStartupRestored);
		Root->SetBoolField(TEXT("generated_removed"),State->bGeneratedRemoved);
		Root->SetStringField(TEXT("startup_map"),State->StartupMap);
		Root->SetStringField(TEXT("original_bake_object"),State->OriginalBakeObject);
		Root->SetStringField(TEXT("generated_bake_package"),State->GeneratedBakePackage);
		Root->SetStringField(TEXT("restore_error"),State->RestoreError);
		Root->SetBoolField(TEXT("memory_guard_control"),State->bMemoryGuardControl);
		Root->SetBoolField(TEXT("mutation_guard_control"),State->bMutationGuardControl);
		Root->SetBoolField(TEXT("mutation_guard_validated"),State->bMutationGuardValidated);
		Root->SetBoolField(TEXT("generated_identity_verified"),State->bGeneratedIdentityVerified);
		TArray<TSharedPtr<FJsonValue>> Observed;
		for(const FString& Object:State->ObservedGeneratedObjects)Observed.Add(MakeShared<FJsonValueString>(Object));
		Root->SetArrayField(TEXT("observed_generated_objects"),Observed);
		TArray<TSharedPtr<FJsonValue>> Mutations;
		for(const FIMW1MutationEvent& Event:State->MutationGuard.Events)
		{
			auto Item=MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("kind"),Event.Kind);Item->SetStringField(TEXT("object"),Event.Object);
			Item->SetStringField(TEXT("package"),Event.Package);Item->SetNumberField(TEXT("frame"),double(Event.Frame));
			Item->SetBoolField(TEXT("dirty"),Event.bDirty);Mutations.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("mutation_events"),Mutations);
		Root->SetStringField(TEXT("memory_sha1_before"),State->MemorySha1Before);
		Root->SetStringField(TEXT("memory_sha1_after"),State->MemorySha1After);
		TArray<TSharedPtr<FJsonValue>> Files;
		for(const FIMW1FrozenFile& File:State->OriginalFiles)
		{
			auto Item=MakeShared<FJsonObject>();FString After;
			const bool bExists=IFileManager::Get().FileExists(*File.Path);
			if(bExists)IMW1FileFingerprint(File.Path,After);
			Item->SetStringField(TEXT("path"),File.Path);Item->SetBoolField(TEXT("exists_before"),File.bExists);
			Item->SetBoolField(TEXT("exists_after"),bExists);Item->SetStringField(TEXT("sha1_before"),File.Sha1);
			Item->SetStringField(TEXT("sha1_after"),After);Files.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("original_files"),Files);
		TArray<TSharedPtr<FJsonValue>> Generated;
		for(const FIMW1FrozenFile& File:State->GeneratedFiles)
		{
			auto Item=MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("path"),File.Path);Item->SetStringField(TEXT("sha1_before"),File.Sha1);
			Item->SetBoolField(TEXT("exists_before"),File.bExists);
			Item->SetBoolField(TEXT("exists_after"),IFileManager::Get().FileExists(*File.Path));
			Generated.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("generated_files"),Generated);
		FString Json;auto Writer=TJsonWriterFactory<>::Create(&Json);
		const bool bWritten=FJsonSerializer::Serialize(Root,Writer)
			&&FFileHelper::SaveStringToFile(Json,*FPaths::Combine(W1Evidence(),TEXT("restore.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const bool Pass=State->bAudioPass&&State->bRestorePass&&State->bStartupRestored&&State->bGeneratedRemoved&&bWritten;
		if(!State->bAudioPass)Test->AddError(State->AudioMessage);
		if(!State->bRestorePass||!State->bStartupRestored||!State->bGeneratedRemoved)Test->AddError(State->RestoreError);
		if(!bWritten)Test->AddError(TEXT("W1 restore evidence could not be saved."));
		if(Pass)Test->AddInfo(State->AudioMessage+TEXT("; original packages/binding/startup restored."));
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1Restore %s map=%d startup=%d generated_removed=%d evidence=%s error=%s"),
			State->bRestorePass&&State->bStartupRestored&&State->bGeneratedRemoved?TEXT("PASS"):TEXT("FAIL"),
			State->bMapRestored,State->bStartupRestored,State->bGeneratedRemoved,*W1Evidence(),*State->RestoreError);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW1DirectPath %s %s"),Pass?TEXT("PASS"):TEXT("FAIL"),*State->AudioMessage);
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW1 %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
		return true;
	}

	bool Cleanup(double Now)
	{
		if(Stage==90)
		{
			if(IMW1HasPIEWorld())
			{
				if(Now-State->CleanupStarted<15)return false;
				State->FailRestore(TEXT("PIE world remained alive 15s after the owned end request."));return PublishResult();
			}
			Stage=91;return false;
		}
		if(Stage==91){RestoreW1();Stage=92;return false;}
		if(Stage==92){RemoveGenerated();Stage=93;return false;}
		if(Stage==93)
		{
			if(!State->bSafeToChangeWorld)
			{State->FailRestore(TEXT("Startup reload skipped to preserve the unexpected Writer state."));return PublishResult();}
			UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
			if(!World||World->GetOutermost()->GetName()!=State->StartupMap)
			{
				if(!FEditorFileUtils::LoadMap(State->StartupMap,false,true))
					State->FailRestore(TEXT("Cannot restore the original startup map."));
			}
			Stage=94;return false;
		}
		UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
		State->bStartupRestored=World&&World->GetOutermost()->GetName()==State->StartupMap&&!World->GetOutermost()->IsDirty();
		if(World)for(ULevel* Level:World->GetLevels())
			if(!Level||Level->GetOutermost()->IsDirty())State->bStartupRestored=false;
		if(!State->bStartupRestored)State->FailRestore(TEXT("Startup map identity/dirty state did not restore."));
		return PublishResult();
	}
	FAutomationTestBase* Test;
	TSharedRef<FIMW1RunState> State;
	double Started,CaptureStart=0,LastDiagnostic=0;
	int32 Stage=0;
	int32 RouteIndex=0;
	uint64 W1In0=0;double W1T0=0;int32 W1Re=0;bool W1Init=false;
	TArray<int16> FeedPCM;int32 FeedCursor=0;
	uint64 DirectBefore=0,PathBefore=0,RejectedBefore=0;
	uint64 ReverbBefore=0;
	uint32 GraphGateSourceBefore=0,GraphGateEnvironmentBefore=0,GraphSourceBefore=0,GraphEnvironmentBefore=0;
	bool AudioPass=false;
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
	if(!FApp::IsUnattended()||!GEditor||IMAcousticW1TestPrivate::IMW1HasPIEWorld())
	{AddError(TEXT("W1 restore validation requires the owned unattended Editor with no PIE world."));return false;}
	auto State=MakeShared<IMAcousticW1TestPrivate::FIMW1RunState>();FString Error;
	UWorld* Startup=GEditor->GetEditorWorldContext().World();
	if(!State->FreezeCleanWorld(Startup,Error)){AddError(Error);return false;}
	State->StartupMap=Startup->GetOutermost()->GetName();
	if(!IMAcousticW1TestPrivate::IMW1ValidateFileGuards(Error)){AddError(Error);return false;}
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	IMAcousticW1TestPrivate::W1OriginalBackgroundVolume=FApp::GetUnfocusedVolumeMultiplier();
	FApp::SetUnfocusedVolumeMultiplier(1); // Deterministic audio in this owned unattended Editor only.
	IMAcousticW1TestPrivate::W1OriginalBackgroundAudio=GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=true;
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW1TestPrivate::FIMAcousticW1Command(this,State));return true;
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
namespace IMAcousticW1TestPrivate
{

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
namespace IMAcousticW1TestPrivate
{

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
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "IMAcousticBakeAsset.h"
#include "Editor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace IMAcousticW1TestPrivate
{
FString IMExportSHA1(const TArray<uint8>& Bytes)
{
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash);
	return BytesToHex(Hash, UE_ARRAY_COUNT(Hash));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticBakePayloadExportTest,
	"IceMoon.AcousticField.BakePayloadExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticBakePayloadExportTest::RunTest(const FString&)
{
	// This is an observation of one existing asset, not a bake or format change.
	// Python protects these non-editor byte arrays; the production reader remains
	// the authority for their digest, recipe identity and lossless decoding.
	FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	FPaths::NormalizeFilename(Project);
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!Project.Equals(TEXT("E:/BaiduSyncdisk/project/AgentWorkSpaceProject/im-acoustic-v2-bake/MyProject.uproject"), ESearchCase::IgnoreCase)
		|| !IsInGameThread() || !FApp::IsUnattended() || !GEditor || IMAcousticW1TestPrivate::IMW1HasPIEWorld() || !EditorWorld
		|| EditorWorld->GetOutermost()->GetName() != TEXT("/IceMoonAcousticField/Tests/IM_W1Door"))
	{
		AddError(TEXT("Raw export requires the exact idle generation-2 Editor project."));
		return false;
	}
	const FString AssetPath = TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_CE06F85C4DBFC6E0D63FF8A345E1C1A6");
	const FString World = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
	const FString Fingerprint = TEXT("955D1C0A2195BAE4DEB1EEADD3ACC4F1FEDAE103");
	const FString Output = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AcousticV2/PhysicalMaterialDrive/R53RestoreSafeW1_20261004/RawPayloadExport01/NativeExport01")));
	if (IFileManager::Get().DirectoryExists(*Output))
	{
		AddError(TEXT("Refusing to overwrite an existing native raw-export directory."));
		return false;
	}
	const FString PackagePath = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension()));
	TArray<uint8> PackageBefore;
	if (!FFileHelper::LoadFileToArray(PackageBefore, *PackagePath) || PackageBefore.Num() != 5128266)
	{
		AddError(TEXT("Frozen CE06 package is missing or has changed size."));
		return false;
	}
	UIMAcousticBakeAsset* Asset = LoadObject<UIMAcousticBakeAsset>(nullptr, *AssetPath);
	if (!Asset || Asset->GetOutermost()->IsDirty() || Asset->FormatVersion != 3
		|| Asset->PayloadDigest != TEXT("25BBA85CF45A8A12CCBD34E11C1434CE8CE7B23D"))
	{
		AddError(TEXT("CE06 identity/clean-package precondition failed."));
		return false;
	}
	TArray<uint8> Scene, Probes;
	FString Error;
	if (!Asset->ReadPayload(World, Fingerprint, Scene, Probes, Error)
		|| Scene.Num() != 13724688 || Probes.Num() != 1250180)
	{
		AddError(TEXT("Production ReadPayload failed or raw lengths changed: ") + Error);
		return false;
	}
	TArray<uint8> PackageAfter;
	const FString PackageSHA1 = IMAcousticW1TestPrivate::IMExportSHA1(PackageBefore);
	if (!FFileHelper::LoadFileToArray(PackageAfter, *PackagePath)
		|| IMAcousticW1TestPrivate::IMExportSHA1(PackageAfter) != PackageSHA1 || Asset->GetOutermost()->IsDirty())
	{
		AddError(TEXT("CE06 package changed during read-only production decoding."));
		return false;
	}
	auto Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("IMR53NativeRawPayloadExport.v1"));
	Report->SetStringField(TEXT("status"), TEXT("PASS"));
	Report->SetStringField(TEXT("execution"), TEXT("EDITOR_NATIVE_READPAYLOAD_ONLY"));
	Report->SetStringField(TEXT("target_asset"), AssetPath);
	Report->SetStringField(TEXT("package_path"), PackagePath);
	Report->SetStringField(TEXT("package_sha1_before"), PackageSHA1);
	Report->SetStringField(TEXT("package_sha1_after"), IMAcousticW1TestPrivate::IMExportSHA1(PackageAfter));
	Report->SetNumberField(TEXT("package_bytes"), PackageBefore.Num());
	Report->SetNumberField(TEXT("format_version"), Asset->FormatVersion);
	Report->SetNumberField(TEXT("steam_audio_version"), Asset->SteamAudioVersion);
	Report->SetStringField(TEXT("world_package"), Asset->WorldPackage);
	Report->SetStringField(TEXT("scene_fingerprint"), Asset->SceneFingerprint);
	Report->SetStringField(TEXT("payload_digest"), Asset->PayloadDigest);
	Report->SetNumberField(TEXT("packed_scene_bytes"), Asset->SceneData.Num());
	Report->SetNumberField(TEXT("packed_probe_bytes"), Asset->ProbeData.Num());
	Report->SetNumberField(TEXT("scene_bytes"), Scene.Num());
	Report->SetNumberField(TEXT("probe_bytes"), Probes.Num());
	Report->SetStringField(TEXT("scene_sha1"), IMAcousticW1TestPrivate::IMExportSHA1(Scene));
	Report->SetStringField(TEXT("probe_sha1"), IMAcousticW1TestPrivate::IMExportSHA1(Probes));
	Report->SetBoolField(TEXT("asset_mutation_or_save"), false);
	Report->SetBoolField(TEXT("rebaked"), false);
	Report->SetBoolField(TEXT("steam_audio_sdk_load"), false);
	Report->SetStringField(TEXT("sdk_scene_equivalence_claim"), TEXT("NONE"));
	FString Json;
	const auto Writer = TJsonWriterFactory<>::Create(&Json);
	// Capture the saved W1 fixture independently of its audible direct oracle.
	// Bounds are observations only; Root must confirm which geometry contributes
	// to the bake and inspect any intersection before choosing a positive pair.
	TArray<TSharedPtr<FJsonValue>> Components;
	for (TActorIterator<AActor> It(EditorWorld); It; ++It)
	{
		TArray<UStaticMeshComponent*> MeshComponents;
		It->GetComponents(MeshComponents);
		for (UStaticMeshComponent* Component : MeshComponents)
		{
			if (!Component || !Component->GetStaticMesh()) continue;
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("actor"), It->GetPathName());
			Row->SetStringField(TEXT("component"), Component->GetPathName());
			Row->SetStringField(TEXT("mesh"), Component->GetStaticMesh()->GetPathName());
			Row->SetBoolField(TEXT("editor_only"), Component->IsEditorOnly());
			Row->SetNumberField(TEXT("ism_instances"), Cast<UInstancedStaticMeshComponent>(Component)
				? Cast<UInstancedStaticMeshComponent>(Component)->GetInstanceCount() : 0);
			const FBox Bounds = Component->Bounds.GetBox();
			Row->SetBoolField(TEXT("bounds_valid"), Bounds.IsValid != 0);
			Row->SetNumberField(TEXT("min_x_cm"), Bounds.Min.X);
			Row->SetNumberField(TEXT("min_y_cm"), Bounds.Min.Y);
			Row->SetNumberField(TEXT("min_z_cm"), Bounds.Min.Z);
			Row->SetNumberField(TEXT("max_x_cm"), Bounds.Max.X);
			Row->SetNumberField(TEXT("max_y_cm"), Bounds.Max.Y);
			Row->SetNumberField(TEXT("max_z_cm"), Bounds.Max.Z);
			Components.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	auto Geometry = MakeShared<FJsonObject>();
	Geometry->SetStringField(TEXT("schema"), TEXT("IMR53W1ComponentBoundsObservation.v1"));
	Geometry->SetStringField(TEXT("world_package"), EditorWorld->GetOutermost()->GetName());
	Geometry->SetArrayField(TEXT("static_mesh_components"), Components);
	Geometry->SetBoolField(TEXT("baked_geometry_equivalence_claim"), false);
	FString GeometryJson;
	const auto GeometryWriter = TJsonWriterFactory<>::Create(&GeometryJson);
	if (!FJsonSerializer::Serialize(Report, Writer)
		|| !FJsonSerializer::Serialize(Geometry, GeometryWriter)
		|| !IFileManager::Get().MakeDirectory(*Output, true)
		|| !FFileHelper::SaveArrayToFile(Scene, *FPaths::Combine(Output, TEXT("scene.bin")))
		|| !FFileHelper::SaveArrayToFile(Probes, *FPaths::Combine(Output, TEXT("probes.bin")))
		|| !FFileHelper::SaveStringToFile(GeometryJson, *FPaths::Combine(Output, TEXT("w1_geometry.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Output, TEXT("receipt.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		AddError(TEXT("Could not publish native raw export; partial evidence must remain FAIL."));
		return false;
	}
	return true;
}

#endif

// Append this fragment to the existing IMAcousticW1Test.cpp translation unit;
// do not add a separate Source file. It remains outside the plugin until Root
// integrates it, uses transient assets, and never calls Steam Audio.
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "IMAcousticBakeAsset.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace IMAcousticW1TestPrivate
{
constexpr uint32 IMCodecTestChunkBytes = 1024u * 1024u;
const FString IMCodecTestWorld = TEXT("/Game/IMCodecTransientWorld");
const FString IMCodecTestFingerprint = TEXT("IM_CODEC_TRANSIENT_FINGERPRINT");

FString IMCodecTestMetadata()
{
	return TEXT("{\"recipe_version\":2,\"sdk_version\":264193,\"triangles\":1,\"probe_count\":1,"
		"\"probe_spacing_cm\":25,\"probe_height_cm\":25,\"bounds_origin_cm\":[0,0,0],"
		"\"bounds_extent_cm\":[1,1,1],\"materials\":[{\"asset_path\":\"/Game/PM.PM\","
		"\"absorption\":[0,0,0],\"scattering\":0}],\"probes_m\":[[0,0,0,1]],"
		"\"reflection\":{\"type\":\"CONVOLUTION\",\"num_rays\":4096,\"num_bounces\":64,"
		"\"num_diffuse_samples\":32,\"sim_duration_s\":2,\"saved_duration_s\":2,"
		"\"irradiance_min_m\":0.01,\"order\":1},\"path\":{\"num_samples\":1,"
		"\"radius_m\":0.5,\"threshold\":0.5,\"vis_range_m\":8,\"path_range_m\":16}}");
}

FString IMCodecTestDigest(const UIMAcousticBakeAsset& Asset)
{
	FTCHARToUTF8 Utf8(*Asset.MetadataJson);
	TArray<uint8> MetadataBytes;
	MetadataBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	const TArray<uint8>* Sections[] = {&MetadataBytes, &Asset.SceneData, &Asset.ProbeData};
	FSHA1 Hash;
	for (const TArray<uint8>* Data : Sections)
	{
		const uint32 Count = static_cast<uint32>(Data->Num());
		const uint8 Length[4] = {static_cast<uint8>(Count), static_cast<uint8>(Count >> 8),
			static_cast<uint8>(Count >> 16), static_cast<uint8>(Count >> 24)};
		Hash.Update(Length, sizeof(Length));
		if (Count) Hash.Update(Data->GetData(), Count);
	}
	Hash.Final();
	uint8 Bytes[20];
	Hash.GetHash(Bytes);
	return BytesToHex(Bytes, UE_ARRAY_COUNT(Bytes));
}

void IMCodecTestSetU32LE(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
{
	if (Offset < 0 || Offset > Bytes.Num() - 4) return;
	Bytes[Offset] = static_cast<uint8>(Value);
	Bytes[Offset + 1] = static_cast<uint8>(Value >> 8);
	Bytes[Offset + 2] = static_cast<uint8>(Value >> 16);
	Bytes[Offset + 3] = static_cast<uint8>(Value >> 24);
}

bool IMCodecTestPackV3(const TArray<uint8>& Raw, TArray<uint8>& Packed)
{
	int64 Bound = 0;
	if (Raw.IsEmpty() || !FCompression::CompressMemoryBound(NAME_Zlib, Bound, Raw.Num())
		|| Bound <= 0 || Bound > MAX_int32 - 4) return false;
	Packed.SetNumUninitialized(static_cast<int32>(Bound) + 4);
	const uint32 RawBytes = static_cast<uint32>(Raw.Num());
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Packed[Index] = static_cast<uint8>(RawBytes >> (Index * 8));
	}
	int64 CompressedBytes = Bound;
	if (!FCompression::CompressMemory(NAME_Zlib, Packed.GetData() + 4, CompressedBytes,
		Raw.GetData(), Raw.Num()) || CompressedBytes <= 0 || CompressedBytes > Bound)
	{
		Packed.Reset();
		return false;
	}
	Packed.SetNum(static_cast<int32>(CompressedBytes) + 4, EAllowShrinking::No);
	return true;
}

void IMCodecTestMakeRaw(TArray<uint8>& Scene, TArray<uint8>& Probes)
{
	Scene.SetNumUninitialized(2 * static_cast<int32>(IMCodecTestChunkBytes) + 3);
	for (int32 Index = 0; Index < Scene.Num(); ++Index)
	{
		Scene[Index] = static_cast<uint8>((Index * 37 + (Index >> 3) * 19) & 0xff);
	}
	Probes.SetNumUninitialized(static_cast<int32>(IMCodecTestChunkBytes) + 1);
	for (int32 Index = 0; Index < Probes.Num(); ++Index)
	{
		Probes[Index] = static_cast<uint8>((Index * 29 + (Index / 3) * 11) & 0xff);
	}
}

bool IMCodecTestHasPIEWorld()
{
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE) return true;
		}
	}
	return GEditor && GEditor->PlayWorld;
}

FString IMCodecTestSHA1(const TArray<uint8>& Bytes)
{
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash);
	return BytesToHex(Hash, UE_ARRAY_COUNT(Hash));
}

struct FIMCodecTestMemorySample
{
	uint64 UsedPhysical = 0;
	uint64 UsedVirtual = 0;
	uint64 PeakUsedPhysical = 0;
	uint64 PeakUsedVirtual = 0;
};

FIMCodecTestMemorySample IMCodecTestSampleMemory()
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	FIMCodecTestMemorySample Sample;
	Sample.UsedPhysical = Stats.UsedPhysical;
	Sample.UsedVirtual = Stats.UsedVirtual;
	Sample.PeakUsedPhysical = Stats.PeakUsedPhysical;
	Sample.PeakUsedVirtual = Stats.PeakUsedVirtual;
	return Sample;
}

TSharedPtr<FJsonObject> IMCodecTestMemorySampleJson(const FIMCodecTestMemorySample& Sample)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("used_physical_bytes"), static_cast<double>(Sample.UsedPhysical));
	Json->SetNumberField(TEXT("used_virtual_bytes"), static_cast<double>(Sample.UsedVirtual));
	Json->SetNumberField(TEXT("process_lifetime_peak_used_physical_bytes"),
		static_cast<double>(Sample.PeakUsedPhysical));
	Json->SetNumberField(TEXT("process_lifetime_peak_used_virtual_bytes"),
		static_cast<double>(Sample.PeakUsedVirtual));
	Json->SetStringField(TEXT("peak_scope"), TEXT("process_lifetime"));
	return Json;
}

TSharedPtr<FJsonObject> IMCodecTestMemoryPhaseJson(
	const FIMCodecTestMemorySample& Before, const FIMCodecTestMemorySample& After)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("before"), IMCodecTestMemorySampleJson(Before));
	Json->SetObjectField(TEXT("after"), IMCodecTestMemorySampleJson(After));
	return Json;
}

TSharedPtr<FJsonObject> IMCodecTestPayloadStatsJson(
	const TArray<uint8>& SceneBytes, const TArray<uint8>& ProbeBytes)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("scene_bytes"), SceneBytes.Num());
	Json->SetNumberField(TEXT("probe_bytes"), ProbeBytes.Num());
	Json->SetNumberField(TEXT("total_bytes"),
		static_cast<double>(SceneBytes.Num()) + static_cast<double>(ProbeBytes.Num()));
	Json->SetStringField(TEXT("scene_sha1"), IMCodecTestSHA1(SceneBytes));
	Json->SetStringField(TEXT("probe_sha1"), IMCodecTestSHA1(ProbeBytes));
	return Json;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPayloadCodecV4Test,
	"IceMoon.AcousticField.BakePayloadCodecV4",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticPayloadCodecV4Test::RunTest(const FString&)
{
	TArray<uint8> Scene;
	TArray<uint8> Probes;
	IMAcousticW1TestPrivate::IMCodecTestMakeRaw(Scene, Probes);

	UIMAcousticBakeAsset* V4 = NewObject<UIMAcousticBakeAsset>(
		GetTransientPackage(), NAME_None, RF_Transient);
	if (!TestNotNull(TEXT("transient format 4 asset"), V4)) return false;
	TestTrue(TEXT("format 4 test asset is RF_Transient"), V4->HasAnyFlags(RF_Transient));
	TArray<uint8> SceneToCommit = Scene;
	TArray<uint8> ProbesToCommit = Probes;
	FString Failure;
	if (!V4->CommitCompleteBake(IMAcousticW1TestPrivate::IMCodecTestWorld, IMAcousticW1TestPrivate::IMCodecTestFingerprint, IMAcousticW1TestPrivate::IMCodecTestMetadata(),
		MoveTemp(SceneToCommit), MoveTemp(ProbesToCommit), Failure))
	{
		AddError(TEXT("Transient format 4 commit failed: ") + Failure);
		return false;
	}
	TestEqual(TEXT("writer selects format 4"), V4->FormatVersion, 4);

	const TArray<uint8> ValidSceneData = V4->SceneData;
	const TArray<uint8> ValidProbeData = V4->ProbeData;
	const uint8 ExpectedMagic[8] = {'I', 'M', 'A', 'C', 'P', 'K', '4', 0};
	constexpr int32 ExpectedMagicBytes = 8;
	bool bMagicMatches = V4->SceneData.Num() > 20;
	for (int32 Index = 0; bMagicMatches && Index < ExpectedMagicBytes; ++Index)
	{
		bMagicMatches = V4->SceneData[Index] == ExpectedMagic[Index];
	}
	TestTrue(TEXT("format 4 scene carries the pinned header magic"), bMagicMatches);

	TArray<uint8> DecodedScene;
	TArray<uint8> DecodedProbes;
	if (!V4->ReadPayload(IMAcousticW1TestPrivate::IMCodecTestWorld, IMAcousticW1TestPrivate::IMCodecTestFingerprint,
		DecodedScene, DecodedProbes, Failure))
	{
		AddError(TEXT("Format 4 roundtrip failed: ") + Failure);
		return false;
	}
	TestTrue(TEXT("format 4 preserves multi-chunk scene bytes and partial four-byte tail"), DecodedScene == Scene);
	TestTrue(TEXT("format 4 preserves probe chunk boundary and partial lane"), DecodedProbes == Probes);

	auto CheckRejected = [this, V4, &ValidSceneData, &ValidProbeData](
		const TCHAR* Label, TArray<uint8> Mutated)
	{
		V4->SceneData = MoveTemp(Mutated);
		V4->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*V4);
		TArray<uint8> OutScene;
		TArray<uint8> OutProbes;
		OutScene.Add(0x5a);
		OutProbes.Add(0xa5);
		FString Error;
		const bool bRead = V4->ReadPayload(IMAcousticW1TestPrivate::IMCodecTestWorld, IMAcousticW1TestPrivate::IMCodecTestFingerprint,
			OutScene, OutProbes, Error);
		TestTrue(Label, !bRead && OutScene.IsEmpty() && OutProbes.IsEmpty());
		V4->SceneData = ValidSceneData;
		V4->ProbeData = ValidProbeData;
		V4->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*V4);
	};

	TArray<uint8> Bad = ValidSceneData;
	Bad[0] ^= 0x80;
	CheckRejected(TEXT("reject malformed format 4 magic"), MoveTemp(Bad));

	Bad = ValidSceneData;
	Bad.SetNum(19, EAllowShrinking::No);
	CheckRejected(TEXT("reject truncated fixed header"), MoveTemp(Bad));

	Bad = ValidSceneData;
	IMAcousticW1TestPrivate::IMCodecTestSetU32LE(Bad, 8, 0);
	CheckRejected(TEXT("reject zero raw section length"), MoveTemp(Bad));

	Bad = ValidSceneData;
	IMAcousticW1TestPrivate::IMCodecTestSetU32LE(Bad, 12, 4);
	CheckRejected(TEXT("reject unexpected fixed chunk size"), MoveTemp(Bad));

	Bad = ValidSceneData;
	IMAcousticW1TestPrivate::IMCodecTestSetU32LE(Bad, 16, 1);
	CheckRejected(TEXT("reject chunk count inconsistent with raw length"), MoveTemp(Bad));

	Bad = ValidSceneData;
	IMAcousticW1TestPrivate::IMCodecTestSetU32LE(Bad, 20, 4);
	CheckRejected(TEXT("reject per-chunk raw length mismatch"), MoveTemp(Bad));

	Bad = ValidSceneData;
	IMAcousticW1TestPrivate::IMCodecTestSetU32LE(Bad, 24, MAX_uint32);
	CheckRejected(TEXT("reject compressed length beyond remaining bytes"), MoveTemp(Bad));

	Bad = ValidSceneData;
	Bad.SetNum(Bad.Num() - 1, EAllowShrinking::No);
	CheckRejected(TEXT("reject truncated final chunk"), MoveTemp(Bad));

	Bad = ValidSceneData;
	Bad.Add(0xa5);
	CheckRejected(TEXT("reject bytes after the final chunk"), MoveTemp(Bad));

	Bad = ValidSceneData;
	Bad[28] = 0;
	V4->SceneData = MoveTemp(Bad);
	V4->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*V4);
	// UE matches the first registered pattern and increments its counter.
	// One registration covers the two corrupt calls, with a separate exact-one
	// assertion per call; duplicate registrations would count 2 and 0.
	AddExpectedError(TEXT("FCompression::UncompressMemory - Failed to uncompress memory"),
		EAutomationExpectedErrorFlags::Contains, 2);
	auto CompressionErrorCount = [this]()
	{
		TArray<FAutomationExpectedMessage> Messages;
		GetExpectedMessages(Messages, ELogVerbosity::Warning);
		for (const FAutomationExpectedMessage& Message : Messages)
		{
			if (Message.MessagePatternString == TEXT("FCompression::UncompressMemory - Failed to uncompress memory"))
				return Message.ActualNumberOfOccurrences;
		}
		return 0;
	};
	const int32 BeforeSceneErrors = CompressionErrorCount();
	TArray<uint8> FailedScene;
	TArray<uint8> FailedProbes;
	FString CompressionError;
	const bool bCorruptStreamRead = V4->ReadPayload(IMAcousticW1TestPrivate::IMCodecTestWorld, IMAcousticW1TestPrivate::IMCodecTestFingerprint,
		FailedScene, FailedProbes, CompressionError);
	TestTrue(TEXT("reject corrupt zlib data before publishing decoded outputs"),
		!bCorruptStreamRead && FailedScene.IsEmpty() && FailedProbes.IsEmpty());
	TestEqual(TEXT("corrupt scene emits exactly one expected compression error"),
		CompressionErrorCount() - BeforeSceneErrors, 1);
	V4->SceneData = ValidSceneData;
	V4->ProbeData = ValidProbeData;
	V4->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*V4);

	Bad = ValidProbeData;
	Bad[28] = 0;
	V4->ProbeData = MoveTemp(Bad);
	V4->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*V4);
	const int32 BeforeProbeErrors = CompressionErrorCount();
	TArray<uint8> FailedProbeScene;
	TArray<uint8> FailedProbeProbes;
	FailedProbeScene.Add(0x5a);
	FailedProbeProbes.Add(0xa5);
	FString CorruptProbeError;
	const bool bCorruptProbeRead = V4->ReadPayload(IMAcousticW1TestPrivate::IMCodecTestWorld, IMAcousticW1TestPrivate::IMCodecTestFingerprint,
		FailedProbeScene, FailedProbeProbes, CorruptProbeError);
	TestTrue(TEXT("corrupt probe zlib rejects the read without publishing the decoded scene"),
		!bCorruptProbeRead && FailedProbeScene.IsEmpty() && FailedProbeProbes.IsEmpty());
	TestEqual(TEXT("corrupt probe emits exactly one expected compression error"),
		CompressionErrorCount() - BeforeProbeErrors, 1);
	V4->SceneData = ValidSceneData;
	V4->ProbeData = ValidProbeData;
	V4->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*V4);

	V4->SceneData = ValidSceneData;
	V4->SceneData.Last() ^= 0x01;
	FString FailureWithStaleDigest;
	TestFalse(TEXT("stored section digest still rejects changed packed bytes"),
		V4->Validate(IMAcousticW1TestPrivate::IMCodecTestWorld, IMAcousticW1TestPrivate::IMCodecTestFingerprint, FailureWithStaleDigest));
	V4->SceneData = ValidSceneData;
	V4->ProbeData = ValidProbeData;
	V4->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*V4);

	auto CheckLegacyRoundtrip = [this](int32 Format, const TArray<uint8>& RawScene,
		const TArray<uint8>& RawProbes)
	{
		UIMAcousticBakeAsset* Legacy = NewObject<UIMAcousticBakeAsset>(
			GetTransientPackage(), NAME_None, RF_Transient);
		if (!TestNotNull(TEXT("transient legacy asset"), Legacy)) return;
		TestTrue(TEXT("legacy test asset is RF_Transient"), Legacy->HasAnyFlags(RF_Transient));
		Legacy->WorldPackage = IMAcousticW1TestPrivate::IMCodecTestWorld;
		Legacy->SceneFingerprint = IMAcousticW1TestPrivate::IMCodecTestFingerprint;
		Legacy->MetadataJson = IMAcousticW1TestPrivate::IMCodecTestMetadata();
		Legacy->SteamAudioVersion = UIMAcousticBakeAsset::SDKVersion;
		Legacy->FormatVersion = Format;
		if (Format == 2)
		{
			Legacy->SceneData = RawScene;
			Legacy->ProbeData = RawProbes;
		}
		else
		{
			TestTrue(TEXT("build legacy format 3 scene control"), IMAcousticW1TestPrivate::IMCodecTestPackV3(RawScene, Legacy->SceneData));
			TestTrue(TEXT("build legacy format 3 probe control"), IMAcousticW1TestPrivate::IMCodecTestPackV3(RawProbes, Legacy->ProbeData));
		}
		Legacy->PayloadDigest = IMAcousticW1TestPrivate::IMCodecTestDigest(*Legacy);
		TArray<uint8> OutScene;
		TArray<uint8> OutProbes;
		FString Error;
		const bool bRead = Legacy->ReadPayload(IMAcousticW1TestPrivate::IMCodecTestWorld, IMAcousticW1TestPrivate::IMCodecTestFingerprint,
			OutScene, OutProbes, Error);
		TestTrue(Format == 2 ? TEXT("format 2 remains readable") : TEXT("format 3 remains readable"),
			bRead && OutScene == RawScene && OutProbes == RawProbes);
	};
	TArray<uint8> LegacyScene;
	LegacyScene.Add(0); LegacyScene.Add(1); LegacyScene.Add(2); LegacyScene.Add(3);
	LegacyScene.Add(4); LegacyScene.Add(5); LegacyScene.Add(6);
	TArray<uint8> LegacyProbes;
	LegacyProbes.Add(7); LegacyProbes.Add(8); LegacyProbes.Add(9);
	LegacyProbes.Add(10); LegacyProbes.Add(11);
	CheckLegacyRoundtrip(2, LegacyScene, LegacyProbes);
	CheckLegacyRoundtrip(3, LegacyScene, LegacyProbes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPayloadCodecCE06Test,
	"IceMoon.AcousticField.BakePayloadCodecCE06V3ToV4",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticPayloadCodecCE06Test::RunTest(const FString&)
{
	FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	FPaths::NormalizeFilename(Project);
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!Project.Equals(TEXT("E:/BaiduSyncdisk/project/AgentWorkSpaceProject/im-acoustic-v2-bake/MyProject.uproject"), ESearchCase::IgnoreCase)
		|| !IsInGameThread() || !FApp::IsUnattended() || !GEditor || IMAcousticW1TestPrivate::IMCodecTestHasPIEWorld()
		|| !EditorWorld || EditorWorld->GetOutermost()->GetName() != TEXT("/IceMoonAcousticField/Tests/IM_W1Door"))
	{
		AddError(TEXT("CE06 codec comparison requires the exact idle generation-2 Editor fixture."));
		return false;
	}

	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AcousticV2/PhysicalMaterialDrive/R53RestoreSafeW1_20261004/LosslessProductCodec04/NativeCodecGate01/NativeReport01"));
	const FString ReportFile = FPaths::Combine(ReportDirectory, TEXT("codec_ce06.json"));
	if (IFileManager::Get().DirectoryExists(*ReportDirectory)
		|| IFileManager::Get().FileExists(*ReportFile))
	{
		AddError(TEXT("Refusing to overwrite an existing NativeReport01 directory or CE06 report."));
		return false;
	}

	constexpr const TCHAR* AssetPath = TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_CE06F85C4DBFC6E0D63FF8A345E1C1A6");
	constexpr const TCHAR* World = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
	constexpr const TCHAR* Fingerprint = TEXT("955D1C0A2195BAE4DEB1EEADD3ACC4F1FEDAE103");
	constexpr const TCHAR* ExpectedPackageSHA1 = TEXT("C4E46C62867DB86CAAB56E6E0C0FA710641630FE");
	constexpr const TCHAR* ExpectedSceneSHA1 = TEXT("BAFE6F1D4D32BA7341DF940DB7830D97FCC52594");
	constexpr const TCHAR* ExpectedProbeSHA1 = TEXT("EFFA2C7EB34BFB5FC1232962D2F867A36A081ECA");
	UIMAcousticBakeAsset* Original = LoadObject<UIMAcousticBakeAsset>(nullptr, AssetPath);
	if (!TestNotNull(TEXT("frozen CE06 source asset"), Original)) return false;
	const bool bOriginalIdentityPinned = !Original->GetOutermost()->IsDirty()
		&& Original->FormatVersion == 3
		&& Original->PayloadDigest == TEXT("25BBA85CF45A8A12CCBD34E11C1434CE8CE7B23D");
	if (!bOriginalIdentityPinned)
	{
		AddError(TEXT("Frozen CE06 source must remain a clean format 3 asset."));
		return false;
	}

	const FString PackagePath = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension()));
	TArray<uint8> PackageBefore;
	if (!FFileHelper::LoadFileToArray(PackageBefore, *PackagePath) || PackageBefore.Num() != 5128266)
	{
		AddError(TEXT("Cannot snapshot frozen CE06 package bytes."));
		return false;
	}
	const FString PackageSHA1Before = IMAcousticW1TestPrivate::IMCodecTestSHA1(PackageBefore);
	if (PackageSHA1Before != ExpectedPackageSHA1)
	{
		AddError(TEXT("Frozen CE06 package SHA1 differs from the pinned format 3 input."));
		return false;
	}

	TArray<uint8> RawScene;
	TArray<uint8> RawProbes;
	FString Failure;
	const IMAcousticW1TestPrivate::FIMCodecTestMemorySample V3ReadMemoryBefore = IMAcousticW1TestPrivate::IMCodecTestSampleMemory();
	const double V3ReadStartSeconds = FPlatformTime::Seconds();
	const bool bV3Read = Original->ReadPayload(World, Fingerprint, RawScene, RawProbes, Failure);
	const double V3ReadEndSeconds = FPlatformTime::Seconds();
	const IMAcousticW1TestPrivate::FIMCodecTestMemorySample V3ReadMemoryAfter = IMAcousticW1TestPrivate::IMCodecTestSampleMemory();
	const double V3ReadPayloadWallMs = (V3ReadEndSeconds - V3ReadStartSeconds) * 1000.0;
	if (!bV3Read || RawScene.Num() != 13724688 || RawProbes.Num() != 1250180)
	{
		AddError(TEXT("Frozen CE06 v3 payload could not be read: ") + Failure);
		return false;
	}
	const FString RawSceneSHA1 = IMAcousticW1TestPrivate::IMCodecTestSHA1(RawScene);
	const FString RawProbeSHA1 = IMAcousticW1TestPrivate::IMCodecTestSHA1(RawProbes);
	const bool bRawLengthsPinned = RawScene.Num() == 13724688 && RawProbes.Num() == 1250180;
	const bool bRawHashesPinned = RawSceneSHA1 == ExpectedSceneSHA1
		&& RawProbeSHA1 == ExpectedProbeSHA1;
	TestTrue(TEXT("raw CE06 scene/probe lengths match the frozen export"), bRawLengthsPinned);
	TestTrue(TEXT("raw CE06 scene/probe SHA1 match the frozen Run01 export"), bRawHashesPinned);

	UIMAcousticBakeAsset* Candidate = NewObject<UIMAcousticBakeAsset>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!TestNotNull(TEXT("transient CE06 format 4 candidate"), Candidate)) return false;
	const bool bCandidateTransient = Candidate->HasAnyFlags(RF_Transient);
	TestTrue(TEXT("CE06 format 4 candidate is RF_Transient"), bCandidateTransient);
	TArray<uint8> SceneToCommit = RawScene;
	TArray<uint8> ProbesToCommit = RawProbes;
	const IMAcousticW1TestPrivate::FIMCodecTestMemorySample V4CommitMemoryBefore = IMAcousticW1TestPrivate::IMCodecTestSampleMemory();
	const double V4CommitStartSeconds = FPlatformTime::Seconds();
	const bool bV4Commit = Candidate->CommitCompleteBake(World, Fingerprint, Original->MetadataJson,
		MoveTemp(SceneToCommit), MoveTemp(ProbesToCommit), Failure);
	const double V4CommitEndSeconds = FPlatformTime::Seconds();
	const IMAcousticW1TestPrivate::FIMCodecTestMemorySample V4CommitMemoryAfter = IMAcousticW1TestPrivate::IMCodecTestSampleMemory();
	const double V4CommitWallMs = (V4CommitEndSeconds - V4CommitStartSeconds) * 1000.0;
	if (!bV4Commit || Candidate->FormatVersion != UIMAcousticBakeAsset::CurrentFormat)
	{
		AddError(TEXT("Transient CE06 format 4 commit failed: ") + Failure);
		return false;
	}

	TArray<uint8> V4Scene;
	TArray<uint8> V4Probes;
	const IMAcousticW1TestPrivate::FIMCodecTestMemorySample V4ReadMemoryBefore = IMAcousticW1TestPrivate::IMCodecTestSampleMemory();
	const double V4ReadStartSeconds = FPlatformTime::Seconds();
	const bool bV4Read = Candidate->ReadPayload(World, Fingerprint, V4Scene, V4Probes, Failure);
	const double V4ReadEndSeconds = FPlatformTime::Seconds();
	const IMAcousticW1TestPrivate::FIMCodecTestMemorySample V4ReadMemoryAfter = IMAcousticW1TestPrivate::IMCodecTestSampleMemory();
	const double V4ReadPayloadWallMs = (V4ReadEndSeconds - V4ReadStartSeconds) * 1000.0;
	if (!bV4Read)
	{
		AddError(TEXT("Transient CE06 format 4 readback failed: ") + Failure);
		return false;
	}
	const bool bSceneExact = V4Scene == RawScene;
	const bool bProbesExact = V4Probes == RawProbes;
	TestTrue(TEXT("format 3 CE06 raw scene bytes survive format 4 exactly"), bSceneExact);
	TestTrue(TEXT("format 3 CE06 raw probe bytes survive format 4 exactly"), bProbesExact);

	TArray<uint8> PackageAfter;
	const bool bPackageReadAfter = FFileHelper::LoadFileToArray(PackageAfter, *PackagePath);
	const FString PackageSHA1After = bPackageReadAfter ? IMAcousticW1TestPrivate::IMCodecTestSHA1(PackageAfter) : FString();
	const bool bPackageUnchanged = bPackageReadAfter && PackageAfter == PackageBefore
		&& PackageSHA1Before == ExpectedPackageSHA1 && PackageSHA1After == ExpectedPackageSHA1
		&& Original->FormatVersion == 3
		&& Original->PayloadDigest == TEXT("25BBA85CF45A8A12CCBD34E11C1434CE8CE7B23D")
		&& !Original->GetOutermost()->IsDirty();
	TestTrue(TEXT("original saved CE06 v3 package remains byte-identical and clean"), bPackageUnchanged);

	const bool bAllConditions = bOriginalIdentityPinned && bRawLengthsPinned && bRawHashesPinned
		&& bCandidateTransient && bV4Commit
		&& Candidate->FormatVersion == UIMAcousticBakeAsset::CurrentFormat
		&& bV4Read && bSceneExact && bProbesExact && bPackageUnchanged;
	TestTrue(TEXT("CE06 native report PASS requires every frozen codec condition"), bAllConditions);

	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("IMR53NativeCodecCE06.v1"));
	Report->SetStringField(TEXT("status"), bAllConditions ? TEXT("PASS") : TEXT("FAIL"));
	Report->SetStringField(TEXT("execution"), TEXT("EDITOR_NATIVE_V3_READ_V4_TRANSIENT_COMMIT_AND_READ"));
	Report->SetStringField(TEXT("target_asset"), AssetPath);
	Report->SetStringField(TEXT("package_path"), PackagePath);
	Report->SetStringField(TEXT("world_package"), World);
	Report->SetStringField(TEXT("scene_fingerprint"), Fingerprint);
	Report->SetNumberField(TEXT("original_format_version"), Original->FormatVersion);
	Report->SetNumberField(TEXT("new_format_version"), Candidate->FormatVersion);
	Report->SetNumberField(TEXT("original_package_bytes"), PackageBefore.Num());
	Report->SetStringField(TEXT("original_package_sha1_before"), PackageSHA1Before);
	Report->SetStringField(TEXT("original_package_sha1_after"), PackageSHA1After);
	Report->SetBoolField(TEXT("original_package_unchanged"), bPackageUnchanged);
	Report->SetStringField(TEXT("original_payload_digest"), Original->PayloadDigest);
	Report->SetObjectField(TEXT("original_v3_packed"),
		IMAcousticW1TestPrivate::IMCodecTestPayloadStatsJson(Original->SceneData, Original->ProbeData));
	Report->SetObjectField(TEXT("transient_v4_packed"),
		IMAcousticW1TestPrivate::IMCodecTestPayloadStatsJson(Candidate->SceneData, Candidate->ProbeData));
	Report->SetObjectField(TEXT("raw_payload"), IMAcousticW1TestPrivate::IMCodecTestPayloadStatsJson(RawScene, RawProbes));
	Report->SetStringField(TEXT("expected_raw_scene_sha1"), ExpectedSceneSHA1);
	Report->SetStringField(TEXT("expected_raw_probe_sha1"), ExpectedProbeSHA1);
	Report->SetBoolField(TEXT("raw_lengths_pinned"), bRawLengthsPinned);
	Report->SetBoolField(TEXT("raw_hashes_pinned"), bRawHashesPinned);
	Report->SetBoolField(TEXT("byte_exact_scene_roundtrip"), bSceneExact);
	Report->SetBoolField(TEXT("byte_exact_probe_roundtrip"), bProbesExact);
	Report->SetBoolField(TEXT("sdk_equivalence"), false);
	Report->SetStringField(TEXT("sdk_equivalence_claim"), TEXT("NONE"));

	TSharedPtr<FJsonObject> Timings = MakeShared<FJsonObject>();
	Timings->SetNumberField(TEXT("v3ReadPayload_wall_ms"), V3ReadPayloadWallMs);
	Timings->SetNumberField(TEXT("v4Commit_wall_ms"), V4CommitWallMs);
	Timings->SetNumberField(TEXT("v4ReadPayload_wall_ms"), V4ReadPayloadWallMs);
	Timings->SetStringField(TEXT("clock"), TEXT("FPlatformTime::Seconds at both timing boundaries"));
	Report->SetObjectField(TEXT("timings"), Timings);

	TSharedPtr<FJsonObject> Memory = MakeShared<FJsonObject>();
	Memory->SetStringField(TEXT("sample_semantics"),
		TEXT("before/after observations; not scoped stage peaks"));
	Memory->SetObjectField(TEXT("v3ReadPayload"),
		IMAcousticW1TestPrivate::IMCodecTestMemoryPhaseJson(V3ReadMemoryBefore, V3ReadMemoryAfter));
	Memory->SetObjectField(TEXT("v4Commit"),
		IMAcousticW1TestPrivate::IMCodecTestMemoryPhaseJson(V4CommitMemoryBefore, V4CommitMemoryAfter));
	Memory->SetObjectField(TEXT("v4ReadPayload"),
		IMAcousticW1TestPrivate::IMCodecTestMemoryPhaseJson(V4ReadMemoryBefore, V4ReadMemoryAfter));
	Report->SetObjectField(TEXT("memory_samples"), Memory);

	FString Json;
	const auto Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Report.ToSharedRef(), Writer))
	{
		AddError(TEXT("Could not serialize the native CE06 codec report."));
		return false;
	}
	const FString ReportParent = FPaths::GetPath(ReportDirectory);
	if (!IFileManager::Get().MakeDirectory(*ReportParent, true)
		|| !IFileManager::Get().MakeDirectory(*ReportDirectory, false)
		|| !FFileHelper::SaveStringToFile(Json, *ReportFile,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		AddError(TEXT("Could not create a fresh native CE06 report directory and file."));
		return false;
	}
	return bAllConditions;
}

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
