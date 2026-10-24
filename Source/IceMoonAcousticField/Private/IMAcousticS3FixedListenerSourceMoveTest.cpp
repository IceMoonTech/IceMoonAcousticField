#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticTestSupport.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "UnrealEdGlobals.h"

namespace IMAcousticS3FixedListenerSourceMoveTestPrivate
{
constexpr const TCHAR* IMS3Map = TEXT("/IceMoonAcousticField/Tests/IM_W1Door");
constexpr double IMS3WindowSeconds = 1.5;
constexpr double IMS3WarmupSeconds = 0.6;
constexpr double IMS3LegSettleSeconds = 0.2;
// Snapshot listener positions are float UE centimeters. 0.05 cm is below one
// millimeter and covers float representation at these fixture coordinates.
constexpr double IMS3PositionToleranceCm = 0.05;
constexpr double IMS3SourceMoveMinimumMeters = 0.1;
constexpr float IMS3PcmNonzeroThreshold = 1.e-9f;

const FVector IMS3Listener(-0.0, 200.0, 150.0);
const FVector IMS3Waypoints[] = {
	FVector(0.0, -200.0, 150.0),    // A: source side of the partition.
	FVector(-150.0, 0.0, 150.0),    // Door: fixed point at the W1 aperture.
	FVector(-200.0, 200.0, 150.0),  // B: opposite side, offset from listener.
	FVector(0.0, -200.0, 150.0)     // Return to A through the same aperture.
};

struct FIMS3Window
{
	int32 Leg = 0;
	FVector StartPosition = FVector::ZeroVector;
	FVector EndPosition = FVector::ZeroVector;
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
	uint32 SourceTraceBegin = 0;
	uint32 SourceTraceEnd = 0;
	uint32 SourcePcmBegin = 0;
	uint32 SourcePcmEnd = 0;
	uint32 EnvironmentTraceBegin = 0;
	uint32 EnvironmentTraceEnd = 0;
	uint32 WetPcmBegin = 0;
	uint32 WetPcmEnd = 0;
	uint64 SnapshotBegin = 0;
	uint64 SnapshotEnd = 0;
	uint32 SourceRows = 0;
	uint32 AcceptedRows = 0;
	uint32 RejectedRows = 0;
	uint32 IdentityRows = 0;
	uint32 DirectNonzeroRows = 0;
	uint32 PathRows = 0;
	uint32 PathNonzeroRows = 0;
	uint32 OutputNonzeroRows = 0;
	uint32 SnapshotRows = 0;
	uint32 SnapshotSourceIdentityRows = 0;
	uint32 SourcePcmSamples = 0;
	uint32 WetPcmSamples = 0;
	uint32 WetNonzeroFrames = 0;
	uint64 AudioComponentId = 0;
	uint64 WorldGeneration = 0;
	uint64 VoiceGeneration = 0;
	uint64 SpatialVersion = 0;
	uint64 FirstSequence = 0;
	uint64 LastSequence = 0;
	uint32 Voice = MAX_uint32;
	FVector SourceSdkStart = FVector::ZeroVector;
	FVector SourceSdkEnd = FVector::ZeroVector;
	double SourcePcmEnergy = 0.0;
	double WetPcmEnergy = 0.0;
	double DirectEnergy = 0.0;
	double PathEnergy = 0.0;
	double OcclusionMean = 0.0;
	double OcclusionMin = 1.0;
	double OcclusionMax = 0.0;
	FVector MeanDirection = FVector::ZeroVector;
	double ListenerMaxDeltaCm = 0.0;
	double SourceMaxDeltaCm = 0.0;
	bool bContinuityPass = true;
	bool bListenerPass = true;
	bool bPass = false;
	FString Failure;
};

struct FIMS3State
{
	FAutomationTestBase* Test = nullptr;
	FString RunId;
	FString EvidenceDirectory;
	FString OriginalMap;
	FString OriginalMapDiskPath;
	FString OriginalMapHashBefore;
	FString OriginalMapHashAfter;
	FString MapDiskPath;
	FString MapHashBefore;
	FString MapHashAfter;
	FString SourceDiskPath;
	FString SourceHashBefore;
	FString SourceHashAfter;
	FString BakeDiskPath;
	FString BakeHashBefore;
	FString BakeHashAfter;
	FString BakeAssetPath;
	FString BakeFingerprint;
	FString GraphAssetPath = IMAcousticTestSupport::GraphSourceAssetPath;
	FString Failure;
	FString ResultMessage;
	float OriginalBackgroundVolume = 1.0f;
	bool bOriginalAllowBackgroundAudio = false;
	bool bGlobalsRestored = false;
	bool bMapRestoreVerified = false;
	bool bAssetsRestoreVerified = false;
	bool bPassed = true;
	bool bFinalReportSaved = false;
	uint64 AudioComponentId = 0;
	uint64 WorldGeneration = 0;
	uint64 VoiceGeneration = 0;
	uint64 SpatialVersion = 0;
	uint64 LastSequence = 0;
	uint32 Voice = MAX_uint32;
	double ListenerMaxDeltaCm = 0.0;
	double SourceMaxDeltaCm = 0.0;
	TArray<FIMS3Window> Windows;
	TArray<FString> PositionRows;
	FIMAcousticMetaSoundContextPtr MetaContext;
	TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;

	void AddFailure(const FString& Why)
	{
		bPassed = false;
		if (!Failure.IsEmpty()) Failure += TEXT(" | ");
		Failure += Why;
	}

	void RestoreGlobals()
	{
		if (bGlobalsRestored) return;
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = bOriginalAllowBackgroundAudio;
		bGlobalsRestored = true;
	}
};

FString IMS3HashFile(const FString& FilePath)
{
	if (IFileManager::Get().FileSize(*FilePath) <= 0) return FString();
	return LexToString(FMD5Hash::HashFile(*FilePath));
}

bool IMS3SaveJson(const FString& FilePath, const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	return FJsonSerializer::Serialize(Object, Writer)
		&& FFileHelper::SaveStringToFile(Json, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool IMS3SaveFloatWindow(const FString& FilePath, const TArray<float>& Samples,
	uint32 BeginFrame, uint32 EndFrame, uint32 Channels)
{
	if (EndFrame <= BeginFrame || Channels == 0) return false;
	const uint64 BeginSample = uint64(BeginFrame) * Channels;
	const uint64 EndSample = uint64(EndFrame) * Channels;
	if (EndSample > uint64(Samples.Num())) return false;
	const TArrayView<const uint8> Bytes(
		reinterpret_cast<const uint8*>(Samples.GetData() + BeginSample),
		SIZE_T((EndSample - BeginSample) * sizeof(float)));
	return FFileHelper::SaveArrayToFile(Bytes, *FilePath);
}

uint32 IMS3SourceTraceCount(const FIMAcousticMetaSoundContextPtr& Context)
{
	return Context.IsValid()
		? FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire), uint32(Context->CapturedSourceBlocks.Num()))
		: 0;
}

uint32 IMS3SourcePcmFrames(const FIMAcousticMetaSoundContextPtr& Context)
{
	return Context.IsValid()
		? FMath::Min<uint32>(Context->CapturedSourceFrames.load(std::memory_order_acquire), uint32(Context->CapturedSource.Num()))
		: 0;
}

uint32 IMS3WetPcmFrames(const FIMAcousticMetaSoundContextPtr& Context)
{
	return Context.IsValid()
		? FMath::Min<uint32>(Context->CapturedEnvironmentFrames.load(std::memory_order_acquire), uint32(Context->CapturedWet.Num() / 2))
		: 0;
}

bool IMS3WindowJson(const FIMS3Window& W, const FString& Directory)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("leg_index"), W.Leg);
	O->SetStringField(TEXT("leg"), W.Leg == 0 ? TEXT("A_to_door") : W.Leg == 1 ? TEXT("door_to_B") : TEXT("B_to_A"));
	O->SetNumberField(TEXT("source_start_ue_x_cm"), W.StartPosition.X);
	O->SetNumberField(TEXT("source_start_ue_y_cm"), W.StartPosition.Y);
	O->SetNumberField(TEXT("source_start_ue_z_cm"), W.StartPosition.Z);
	O->SetNumberField(TEXT("source_end_ue_x_cm"), W.EndPosition.X);
	O->SetNumberField(TEXT("source_end_ue_y_cm"), W.EndPosition.Y);
	O->SetNumberField(TEXT("source_end_ue_z_cm"), W.EndPosition.Z);
	O->SetNumberField(TEXT("start_seconds"), W.StartSeconds);
	O->SetNumberField(TEXT("end_seconds"), W.EndSeconds);
	O->SetNumberField(TEXT("source_trace_begin"), W.SourceTraceBegin);
	O->SetNumberField(TEXT("source_trace_end"), W.SourceTraceEnd);
	O->SetNumberField(TEXT("source_trace_rows"), W.SourceRows);
	O->SetNumberField(TEXT("source_pcm_frame_begin"), W.SourcePcmBegin);
	O->SetNumberField(TEXT("source_pcm_frame_end"), W.SourcePcmEnd);
	O->SetNumberField(TEXT("source_pcm_samples_mono"), W.SourcePcmSamples);
	O->SetNumberField(TEXT("source_pcm_sum_squares"), W.SourcePcmEnergy);
	O->SetNumberField(TEXT("wet_pcm_frame_begin"), W.WetPcmBegin);
	O->SetNumberField(TEXT("wet_pcm_frame_end"), W.WetPcmEnd);
	O->SetNumberField(TEXT("wet_pcm_samples_stereo"), W.WetPcmSamples);
	O->SetNumberField(TEXT("wet_pcm_nonzero_frames"), W.WetNonzeroFrames);
	O->SetNumberField(TEXT("wet_pcm_sum_squares"), W.WetPcmEnergy);
	O->SetNumberField(TEXT("environment_trace_begin"), W.EnvironmentTraceBegin);
	O->SetNumberField(TEXT("environment_trace_end"), W.EnvironmentTraceEnd);
	O->SetNumberField(TEXT("snapshot_begin"), double(W.SnapshotBegin));
	O->SetNumberField(TEXT("snapshot_end"), double(W.SnapshotEnd));
	O->SetNumberField(TEXT("snapshot_rows"), W.SnapshotRows);
	O->SetNumberField(TEXT("snapshot_source_identity_rows"), W.SnapshotSourceIdentityRows);
	O->SetStringField(TEXT("audio_component_id"), LexToString(W.AudioComponentId));
	O->SetStringField(TEXT("world_generation"), LexToString(W.WorldGeneration));
	O->SetNumberField(TEXT("voice"), W.Voice == MAX_uint32 ? -1 : int32(W.Voice));
	O->SetStringField(TEXT("voice_generation"), LexToString(W.VoiceGeneration));
	O->SetStringField(TEXT("spatial_version"), LexToString(W.SpatialVersion));
	O->SetStringField(TEXT("sequence_first"), LexToString(W.FirstSequence));
	O->SetStringField(TEXT("sequence_last"), LexToString(W.LastSequence));
	O->SetNumberField(TEXT("accepted_rows"), W.AcceptedRows);
	O->SetNumberField(TEXT("rejected_rows"), W.RejectedRows);
	O->SetNumberField(TEXT("identity_rows"), W.IdentityRows);
	O->SetNumberField(TEXT("direct_nonzero_rows"), W.DirectNonzeroRows);
	O->SetNumberField(TEXT("direct_sum_squares"), W.DirectEnergy);
	O->SetNumberField(TEXT("path_rows"), W.PathRows);
	O->SetNumberField(TEXT("path_nonzero_rows"), W.PathNonzeroRows);
	O->SetNumberField(TEXT("path_sum_squares"), W.PathEnergy);
	O->SetNumberField(TEXT("output_nonzero_rows"), W.OutputNonzeroRows);
	O->SetNumberField(TEXT("occlusion_mean"), W.AcceptedRows ? W.OcclusionMean / W.AcceptedRows : 0.0);
	O->SetNumberField(TEXT("occlusion_min"), W.AcceptedRows ? W.OcclusionMin : 0.0);
	O->SetNumberField(TEXT("occlusion_max"), W.OcclusionMax);
	O->SetNumberField(TEXT("direction_mean_x"), W.AcceptedRows ? W.MeanDirection.X / W.AcceptedRows : 0.0);
	O->SetNumberField(TEXT("direction_mean_y"), W.AcceptedRows ? W.MeanDirection.Y / W.AcceptedRows : 0.0);
	O->SetNumberField(TEXT("direction_mean_z"), W.AcceptedRows ? W.MeanDirection.Z / W.AcceptedRows : 0.0);
	O->SetNumberField(TEXT("source_sdk_start_x_m"), W.SourceSdkStart.X);
	O->SetNumberField(TEXT("source_sdk_start_y_m"), W.SourceSdkStart.Y);
	O->SetNumberField(TEXT("source_sdk_start_z_m"), W.SourceSdkStart.Z);
	O->SetNumberField(TEXT("source_sdk_end_x_m"), W.SourceSdkEnd.X);
	O->SetNumberField(TEXT("source_sdk_end_y_m"), W.SourceSdkEnd.Y);
	O->SetNumberField(TEXT("source_sdk_end_z_m"), W.SourceSdkEnd.Z);
	O->SetNumberField(TEXT("listener_max_delta_cm"), W.ListenerMaxDeltaCm);
	O->SetNumberField(TEXT("source_max_command_delta_cm"), W.SourceMaxDeltaCm);
	O->SetBoolField(TEXT("continuity_pass"), W.bContinuityPass);
	O->SetBoolField(TEXT("listener_fixed_pass"), W.bListenerPass);
	O->SetBoolField(TEXT("pass"), W.bPass);
	O->SetStringField(TEXT("failure"), W.Failure);
	O->SetStringField(TEXT("source_pcm_file"), FString::Printf(TEXT("window-%d-source-mono.f32le"), W.Leg));
	O->SetStringField(TEXT("wet_pcm_file"), FString::Printf(TEXT("window-%d-wet-stereo.f32le"), W.Leg));
	return IMS3SaveJson(FPaths::Combine(Directory, FString::Printf(TEXT("window-%d.json"), W.Leg)), O);
}

bool IMS3WriteTraceEvidence(const TSharedRef<FIMS3State>& S)
{
	bool bSaved = true;
	FString Blocks = TEXT("window,index,block,voice,callback_audio_id,result_audio_id,world_generation,voice_generation,result_spatial_version,sequence,consumed_s,listener_sdk_x_m,listener_sdk_y_m,listener_sdk_z_m,direction_x,direction_y,direction_z,occlusion,distance_gain,routes,direct_valid,path_valid,reject,reject_detail,input_delay_priming,input_energy,direct_energy,path_energy,output_energy\n");
	const uint32 SourceCount = IMS3SourceTraceCount(S->MetaContext);
	for (int32 WIndex = 0; WIndex < S->Windows.Num(); ++WIndex)
	{
		const FIMS3Window& W = S->Windows[WIndex];
		const uint32 Begin = FMath::Min(W.SourceTraceBegin, SourceCount);
		const uint32 End = FMath::Min(W.SourceTraceEnd, SourceCount);
		for (uint32 I = Begin; I < End; ++I)
		{
			const FIMAcousticBlockProbe& B = S->MetaContext->CapturedSourceBlocks[int32(I)];
			Blocks += FString::Printf(TEXT("%d,%u,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g\n"),
				W.Leg, I, B.Block, B.Voice, B.CallbackAudioComponentId, B.ResultAudioComponentId,
				B.ResultWorldGeneration, B.ResultVoiceGeneration, B.ResultSpatialVersion, B.ResultSequence, B.ConsumedSeconds,
				B.ListenerX, B.ListenerY, B.ListenerZ, B.DirX, B.DirY, B.DirZ, B.Occlusion, B.DistanceGain,
				B.Routes, B.DirectValid, B.PathValid, uint32(B.Reject), B.RejectDetail, B.InputDelayPriming,
				B.InputEnergy, B.DirectEnergy, B.PathEnergy, B.OutputEnergy);
		}
		const FString SourceFile = FPaths::Combine(S->EvidenceDirectory, FString::Printf(TEXT("window-%d-source-mono.f32le"), W.Leg));
		const FString WetFile = FPaths::Combine(S->EvidenceDirectory, FString::Printf(TEXT("window-%d-wet-stereo.f32le"), W.Leg));
		bSaved = IMS3SaveFloatWindow(SourceFile, S->MetaContext->CapturedSource, W.SourcePcmBegin, W.SourcePcmEnd, 1) && bSaved;
		bSaved = IMS3SaveFloatWindow(WetFile, S->MetaContext->CapturedWet, W.WetPcmBegin, W.WetPcmEnd, 2) && bSaved;
		bSaved = IMS3WindowJson(W, S->EvidenceDirectory) && bSaved;
	}
	bSaved = FFileHelper::SaveStringToFile(Blocks, *FPaths::Combine(S->EvidenceDirectory, TEXT("metasound-source-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && bSaved;

	FString Snapshots = TEXT("window,index,block,world_generation,spatial_version,captured_s,submit_s,listener_ue_x_cm,listener_ue_y_cm,listener_ue_z_cm,listener_sdk_x_m,listener_sdk_y_m,listener_sdk_z_m,source0_sdk_x_m,source0_sdk_y_m,source0_sdk_z_m,source0_audio_id,num_sources,submitted,fail_code\n");
	const uint64 SnapshotCount = S->Bridge.IsValid()
		? FMath::Min<uint64>(S->Bridge->SnapshotProbePushes.load(std::memory_order_acquire), FIMAcousticDeviceBridge::ProbeSnapshotCapacity)
		: 0;
	for (const FIMS3Window& W : S->Windows)
	{
		const uint64 Begin = FMath::Min<uint64>(W.SnapshotBegin, SnapshotCount);
		const uint64 End = FMath::Min<uint64>(W.SnapshotEnd, SnapshotCount);
		for (uint64 I = Begin; I < End; ++I)
		{
			if (S->Bridge->SnapshotDone[I].load(std::memory_order_acquire) != I + 1) continue;
			const FIMAcousticSnapshotProbe& P = S->Bridge->SnapshotProbes[I];
			Snapshots += FString::Printf(TEXT("%d,%llu,%llu,%llu,%llu,%.9f,%.9f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%u,%u,%u\n"),
				W.Leg, I, P.Block, P.WorldGeneration, P.SpatialVersion, P.CapturedSeconds, P.SubmitSeconds,
				P.ListenerUEX, P.ListenerUEY, P.ListenerUEZ, P.ListenerSDKX, P.ListenerSDKY, P.ListenerSDKZ,
				P.Source0X, P.Source0Y, P.Source0Z, P.Source0AudioId, P.NumSources, P.Submitted, P.FailCode);
		}
	}
	bSaved = FFileHelper::SaveStringToFile(Snapshots, *FPaths::Combine(S->EvidenceDirectory, TEXT("device-snapshots.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && bSaved;
	FString Positions = TEXT("time_s,window,alpha,commanded_source_x_cm,commanded_source_y_cm,commanded_source_z_cm,actual_source_x_cm,actual_source_y_cm,actual_source_z_cm,fixed_listener_x_cm,fixed_listener_y_cm,fixed_listener_z_cm\n");
	for (const FString& Row : S->PositionRows) Positions += Row;
	bSaved = FFileHelper::SaveStringToFile(Positions, *FPaths::Combine(S->EvidenceDirectory, TEXT("positions.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && bSaved;
	return bSaved;
}

bool IMS3WriteSummary(const TSharedRef<FIMS3State>& S, bool bCleanupComplete)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("IMAcousticS3FixedListenerSourceMove.v1"));
	Root->SetStringField(TEXT("run_id"), S->RunId);
	Root->SetStringField(TEXT("outcome"), !S->bPassed ? TEXT("FAIL") : bCleanupComplete ? TEXT("PASS") : TEXT("PASS_PENDING_CLEANUP"));
	Root->SetStringField(TEXT("result_message"), S->ResultMessage);
	Root->SetStringField(TEXT("failure"), S->Failure);
	Root->SetStringField(TEXT("map"), IMS3Map);
	Root->SetStringField(TEXT("original_editor_map"), S->OriginalMap);
	Root->SetStringField(TEXT("original_editor_map_disk_path"), S->OriginalMapDiskPath);
	Root->SetStringField(TEXT("original_editor_map_md5_before"), S->OriginalMapHashBefore);
	Root->SetStringField(TEXT("original_editor_map_md5_after"), S->OriginalMapHashAfter);
	Root->SetStringField(TEXT("map_disk_path"), S->MapDiskPath);
	Root->SetStringField(TEXT("map_md5_before"), S->MapHashBefore);
	Root->SetStringField(TEXT("map_md5_after"), S->MapHashAfter);
	Root->SetStringField(TEXT("source_asset"), S->GraphAssetPath);
	Root->SetStringField(TEXT("source_disk_path"), S->SourceDiskPath);
	Root->SetStringField(TEXT("source_asset_md5_before"), S->SourceHashBefore);
	Root->SetStringField(TEXT("source_asset_md5_after"), S->SourceHashAfter);
	Root->SetStringField(TEXT("baked_asset"), S->BakeAssetPath);
	Root->SetStringField(TEXT("baked_asset_md5_before"), S->BakeHashBefore);
	Root->SetStringField(TEXT("baked_asset_md5_after"), S->BakeHashAfter);
	Root->SetStringField(TEXT("bake_scene_fingerprint"), S->BakeFingerprint);
	Root->SetStringField(TEXT("movement_route"), TEXT("A -> door -> B -> A; only UAudioComponent world position changes"));
	Root->SetStringField(TEXT("previous_w1_difference"), TEXT("W1 moving_front/back called MovePair and changed source plus listener pose; this test fixes listener pose once for the full route."));
	Root->SetStringField(TEXT("graph_source_kind"), TEXT("ordinary MetaSound from IM_AcousticTestSupport::ConfigureGraphSource"));
	Root->SetStringField(TEXT("route_contract"), TEXT("direct+path+reverb enabled; each leg needs output, A-to-door and B-to-A need path, door-to-B needs direct"));
	Root->SetStringField(TEXT("graph_identity_trace"), TEXT("metasound-source-blocks.csv: callback/result audio component IDs, world/voice generations, spatial version and sequence"));
	Root->SetStringField(TEXT("positions_trace"), TEXT("positions.csv and device-snapshots.csv"));
	Root->SetStringField(TEXT("pcm_format"), TEXT("float32 little-endian; source is mono input; wet is stereo environment output"));
	Root->SetNumberField(TEXT("position_tolerance_cm"), IMS3PositionToleranceCm);
	Root->SetNumberField(TEXT("minimum_observed_source_motion_m"), IMS3SourceMoveMinimumMeters);
	Root->SetNumberField(TEXT("listener_commanded_x_cm"), IMS3Listener.X);
	Root->SetNumberField(TEXT("listener_commanded_y_cm"), IMS3Listener.Y);
	Root->SetNumberField(TEXT("listener_commanded_z_cm"), IMS3Listener.Z);
	Root->SetNumberField(TEXT("listener_max_observed_delta_cm"), S->ListenerMaxDeltaCm);
	Root->SetNumberField(TEXT("source_max_command_delta_cm"), S->SourceMaxDeltaCm);
	Root->SetNumberField(TEXT("window_count"), S->Windows.Num());
	Root->SetBoolField(TEXT("cleanup_complete"), bCleanupComplete);
	Root->SetBoolField(TEXT("map_restore_verified"), S->bMapRestoreVerified);
	Root->SetBoolField(TEXT("assets_unchanged_verified"), S->bAssetsRestoreVerified);
	Root->SetBoolField(TEXT("map_or_asset_saved_by_test"), false);
	Root->SetStringField(TEXT("window_metrics"), TEXT("window-0.json, window-1.json, window-2.json"));
	Root->SetStringField(TEXT("pcm_files"), TEXT("window-<leg>-source-mono.f32le; window-<leg>-wet-stereo.f32le"));
	Root->SetStringField(TEXT("other_traces"), TEXT("metasound-source-blocks.csv; device-snapshots.csv; positions.csv"));
	Root->SetStringField(TEXT("not_covered"), TEXT("No controlled A/B audio comparison, no multi-region route, no baked asset creation, no direct-route-only negative control, no separate direction-sign contract."));
	return IMS3SaveJson(FPaths::Combine(S->EvidenceDirectory, TEXT("summary.json")), Root);
}

UWorld* IMS3FindPIEWorld()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		if (Context.WorldType == EWorldType::PIE) return Context.World();
	return nullptr;
}

class FIMAcousticS3FixedListenerCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticS3FixedListenerCommand(TSharedRef<FIMS3State> InState)
		: State(InState), StartedAt(FPlatformTime::Seconds())
	{
	}

	bool Update() override
	{
		check(IsInGameThread());
		const double Now = FPlatformTime::Seconds();
		if (Stage != 90 && Now - StartedAt > 60.0) return Finish(false, TEXT("Timed out waiting for the S3 PIE capture."));

		if (Stage == 0)
		{
			UWorld* PIE = IMS3FindPIEWorld();
			if (!PIE) return false;
			for (TActorIterator<AIMAcousticBakeVolume> It(PIE); It; ++It)
			{
				if (Volume.IsValid()) return Finish(false, TEXT("W1 fixture contains more than one bake volume."));
				Volume = *It;
			}
			if (!Volume.IsValid() || !Volume->BakedField) return Finish(false, TEXT("W1 map has no already-baked field; test refuses to bake or save assets."));
			Listener = PIE->GetFirstPlayerController();
			if (!Listener.IsValid()) return Finish(false, TEXT("PIE has no player controller for the fixed audio listener."));

			State->BakeAssetPath = Volume->BakedField->GetPathName();
			State->BakeFingerprint = Volume->BakedField->SceneFingerprint;
			const FString BakePackage = Volume->BakedField->GetOutermost()->GetName();
			State->BakeDiskPath = FPackageName::LongPackageNameToFilename(BakePackage, FPackageName::GetAssetPackageExtension());
			State->BakeHashBefore = IMS3HashFile(State->BakeDiskPath);
			if (State->BakeHashBefore.IsEmpty()) return Finish(false, TEXT("Could not fingerprint the referenced baked asset on disk."));

			Volume->bDirectRoute = true;
			Volume->bPathRoute = true;
			Volume->bReverbRoute = true;
			Listener->SetAudioListenerOverride(nullptr, IMS3Listener, FRotator(0.0, -90.0, 0.0));
			if (!SpawnGraphSource(PIE)) return Finish(false, SpawnFailure);
			if (FAudioDevice* Device = PIE->GetAudioDeviceRaw()) State->MetaContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID);
			Stage = 1;
			StageAt = Now;
			return false;
		}

		if (Stage == 1)
		{
			if (!State->MetaContext.IsValid())
			{
				if (UWorld* PIE = IMS3FindPIEWorld())
					if (FAudioDevice* Device = PIE->GetAudioDeviceRaw()) State->MetaContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID);
			}
			if (!State->MetaContext.IsValid())
			{
				if (Now - StageAt > 20.0) return Finish(false, TEXT("Ordinary MetaSound context did not initialize."));
				return false;
			}
			State->Bridge = State->MetaContext->Device;
			if (!State->Bridge.IsValid()) return Finish(false, TEXT("MetaSound context has no device bridge."));
			const uint32 SourceFrames = IMS3SourcePcmFrames(State->MetaContext);
			const uint32 SourceRows = IMS3SourceTraceCount(State->MetaContext);
			if (SourceFrames == 0 || SourceRows < 8)
			{
				if (Now - StageAt > 20.0) return Finish(false, TEXT("MetaSound source PCM/trace did not become active."));
				return false;
			}
			if (Now - StageAt < IMS3WarmupSeconds) return false;
			if (!ListenerStillFixed()) return Finish(false, TEXT("Device listener snapshot did not converge to the declared fixed pose."));
			BeginWindow(0, Now);
			Stage = 2;
			return false;
		}

		if (Stage == 2)
		{
			if (!SourceAudio.IsValid()) return Finish(false, TEXT("Moving source was destroyed during capture."));
			const double Elapsed = Now - WindowStartAt;
			const float Alpha = float(FMath::Clamp(Elapsed / IMS3WindowSeconds, 0.0, 1.0));
			const FVector Commanded = FMath::Lerp(IMS3Waypoints[Leg], IMS3Waypoints[Leg + 1], Alpha);
			SourceAudio->SetWorldLocation(Commanded);
			SamplePositions(Now, Alpha);
			if (Elapsed < IMS3WindowSeconds) return false;
			SourceAudio->SetWorldLocation(IMS3Waypoints[Leg + 1]);
			SamplePositions(Now, 1.0f);
			EndWindow(Now);
			if (!State->Windows.Last().bPass) return Finish(false, FString::Printf(TEXT("Window %d failed: %s"), Leg, *State->Windows.Last().Failure));
			if (Leg == 2) return Finish(true, TEXT("Fixed-listener source move captured all three A-to-door-to-B-to-A windows."));
			++Leg;
			SourceAudio->SetWorldLocation(IMS3Waypoints[Leg]);
			StageAt = Now;
			Stage = 3;
			return false;
		}

		if (Stage == 3)
		{
			SamplePositions(Now, 0.0f);
			if (Now - StageAt < IMS3LegSettleSeconds) return false;
			BeginWindow(Leg, Now);
			Stage = 2;
			return false;
		}

		if (Stage == 90)
		{
			if (IMS3FindPIEWorld())
			{
				if (Now - CleanupAt > 30.0)
				{
					State->AddFailure(TEXT("PIE did not end after RequestEndPlayMap."));
					return FinalizeAfterCleanup();
				}
				return false;
			}
			if (!bMapRestored)
			{
				bMapRestored = true;
				if (State->OriginalMap != IMS3Map)
				{
					const bool bLoaded = FEditorFileUtils::LoadMap(*State->OriginalMap, false, true);
					const UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
					const FString RestoredPackage = RestoredWorld ? RestoredWorld->GetOutermost()->GetName() : FString();
					State->bMapRestoreVerified = bLoaded && RestoredPackage == State->OriginalMap;
				}
				else
				{
					const UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
					State->bMapRestoreVerified = RestoredWorld && RestoredWorld->GetOutermost()->GetName() == State->OriginalMap;
				}
				if (!State->bMapRestoreVerified) State->AddFailure(TEXT("Original editor map was not restored to its recorded package identity."));
				State->MapHashAfter = IMS3HashFile(State->MapDiskPath);
				State->OriginalMapHashAfter = IMS3HashFile(State->OriginalMapDiskPath);
				State->SourceHashAfter = IMS3HashFile(State->SourceDiskPath);
				State->BakeHashAfter = IMS3HashFile(State->BakeDiskPath);
				State->bAssetsRestoreVerified = !State->MapHashBefore.IsEmpty()
					&& State->MapHashBefore == State->MapHashAfter
					&& !State->OriginalMapHashBefore.IsEmpty() && State->OriginalMapHashBefore == State->OriginalMapHashAfter
					&& !State->SourceHashBefore.IsEmpty() && State->SourceHashBefore == State->SourceHashAfter
					&& !State->BakeHashBefore.IsEmpty() && State->BakeHashBefore == State->BakeHashAfter;
				if (!State->bAssetsRestoreVerified) State->AddFailure(TEXT("Original/test map, ordinary MetaSound source, or baked asset disk fingerprint changed during the test."));
			}
			return FinalizeAfterCleanup();
		}
		return false;
	}

private:
	bool SpawnGraphSource(UWorld* PIE)
	{
		AActor* Actor = PIE ? PIE->SpawnActor<AActor>() : nullptr;
		if (!Actor)
		{
			SpawnFailure = TEXT("Could not spawn transient PIE actor for ordinary MetaSound source.");
			return false;
		}
		Actor->SetActorLocation(IMS3Waypoints[0]);
		UAudioComponent* Audio = NewObject<UAudioComponent>(Actor);
		if (!Audio)
		{
			Actor->Destroy();
			SpawnFailure = TEXT("Could not create transient audio component.");
			return false;
		}
		Actor->SetRootComponent(Audio);
		Actor->AddInstanceComponent(Audio);
		Audio->bAutoActivate = false;
		Audio->RegisterComponent();
		Audio->SetWorldLocation(IMS3Waypoints[0]);
		UIMAcousticSourceComponent* Source = NewObject<UIMAcousticSourceComponent>(Actor);
		if (!Source)
		{
			Actor->Destroy();
			SpawnFailure = TEXT("Could not create transient IceMoon source component.");
			return false;
		}
		Actor->AddInstanceComponent(Source);
		Source->AudioComponent = Audio;
		Source->RegisterComponent();
		FString Error;
		if (!IMAcousticTestSupport::ConfigureGraphSource(Audio, Error) || !Source->ValidateSource(Error))
		{
			Actor->Destroy();
			SpawnFailure = Error.IsEmpty() ? TEXT("Ordinary MetaSound source validation failed.") : Error;
			return false;
		}
		SourceActor = Actor;
		SourceAudio = Audio;
		Audio->Play();
		return true;
	}

	bool ListenerStillFixed() const
	{
		if (!State->Bridge.IsValid()) return false;
		const uint64 Pushes = State->Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
		const uint64 Limit = FMath::Min<uint64>(Pushes, FIMAcousticDeviceBridge::ProbeSnapshotCapacity);
		for (uint64 I = Limit; I > 0; --I)
		{
			const uint64 Index = I - 1;
			if (State->Bridge->SnapshotDone[Index].load(std::memory_order_acquire) != Index + 1) continue;
			const FIMAcousticSnapshotProbe& P = State->Bridge->SnapshotProbes[Index];
			return FVector::Distance(FVector(P.ListenerUEX, P.ListenerUEY, P.ListenerUEZ), IMS3Listener) <= IMS3PositionToleranceCm;
		}
		return false;
	}

	void BeginWindow(int32 Index, double Now)
	{
		FIMS3Window W;
		W.Leg = Index;
		W.StartPosition = SourceAudio.IsValid() ? SourceAudio->GetComponentLocation() : IMS3Waypoints[Index];
		W.EndPosition = IMS3Waypoints[Index + 1];
		W.StartSeconds = Now;
		W.SourceTraceBegin = IMS3SourceTraceCount(State->MetaContext);
		W.SourcePcmBegin = IMS3SourcePcmFrames(State->MetaContext);
		W.EnvironmentTraceBegin = State->MetaContext->CapturedBlockCount.load(std::memory_order_acquire);
		W.WetPcmBegin = IMS3WetPcmFrames(State->MetaContext);
		W.SnapshotBegin = State->Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
		State->Windows.Add(W);
		WindowStartAt = Now;
		LastPositionSampleAt = -1.0e30;
		WindowSourceMaxDeltaCm = 0.0;
		SamplePositions(Now, 0.0f);
	}

	void SamplePositions(double Now, float Alpha)
	{
		if (!SourceAudio.IsValid()) return;
		if (Now - LastPositionSampleAt < 0.1 && Alpha < 1.0f) return;
		LastPositionSampleAt = Now;
		const FVector Actual = SourceAudio->GetComponentLocation();
		const FVector Expected = FMath::Lerp(IMS3Waypoints[Leg], IMS3Waypoints[Leg + 1], Alpha);
		WindowSourceMaxDeltaCm = FMath::Max(WindowSourceMaxDeltaCm, FVector::Distance(Actual, Expected));
		State->SourceMaxDeltaCm = FMath::Max(State->SourceMaxDeltaCm, FVector::Distance(Actual, Expected));
		State->PositionRows.Add(FString::Printf(TEXT("%.6f,%d,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n"),
			Now - StartedAt, Leg, Alpha, Expected.X, Expected.Y, Expected.Z, Actual.X, Actual.Y, Actual.Z,
			IMS3Listener.X, IMS3Listener.Y, IMS3Listener.Z));
	}

	void EndWindow(double Now)
	{
		FIMS3Window& W = State->Windows.Last();
		W.EndSeconds = Now;
		W.SourceMaxDeltaCm = WindowSourceMaxDeltaCm;
		W.SourceTraceEnd = IMS3SourceTraceCount(State->MetaContext);
		W.SourcePcmEnd = IMS3SourcePcmFrames(State->MetaContext);
		W.EnvironmentTraceEnd = State->MetaContext->CapturedBlockCount.load(std::memory_order_acquire);
		W.WetPcmEnd = IMS3WetPcmFrames(State->MetaContext);
		W.SnapshotEnd = State->Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
		const uint32 SourceBegin = FMath::Min(W.SourceTraceBegin, W.SourceTraceEnd);
		for (uint32 I = SourceBegin; I < W.SourceTraceEnd; ++I)
		{
			const FIMAcousticBlockProbe& B = State->MetaContext->CapturedSourceBlocks[int32(I)];
			++W.SourceRows;
			if (B.Reject != EIMAcousticProbeReject::Accepted)
			{
				++W.RejectedRows;
				continue;
			}
			++W.AcceptedRows;
			if (State->AudioComponentId == 0 && B.CallbackAudioComponentId != 0)
			{
				State->AudioComponentId = B.CallbackAudioComponentId;
				State->WorldGeneration = B.ResultWorldGeneration;
				State->VoiceGeneration = B.ResultVoiceGeneration;
				State->SpatialVersion = B.ResultSpatialVersion;
				State->Voice = B.Voice;
			}
			if (B.CallbackAudioComponentId == State->AudioComponentId && B.ResultAudioComponentId == State->AudioComponentId
				&& B.ResultWorldGeneration == State->WorldGeneration && B.ResultVoiceGeneration == State->VoiceGeneration
				&& B.Voice == State->Voice && B.ResultSpatialVersion == State->SpatialVersion)
			{
				++W.IdentityRows;
			}
			else
			{
				W.bContinuityPass = false;
			}
			if (B.ResultSequence == 0 || (State->LastSequence != 0 && B.ResultSequence < State->LastSequence)) W.bContinuityPass = false;
			if (W.FirstSequence == 0) W.FirstSequence = B.ResultSequence;
			W.LastSequence = B.ResultSequence;
			State->LastSequence = B.ResultSequence;
			if (FMath::IsFinite(B.DirectEnergy) && B.DirectEnergy > 1.e-9)
			{
				++W.DirectNonzeroRows;
				W.DirectEnergy += B.DirectEnergy;
			}
			if ((B.Routes & 2u) != 0 && B.PathValid)
			{
				++W.PathRows;
			}
			if (FMath::IsFinite(B.PathEnergy) && B.PathEnergy > 1.e-9)
			{
				++W.PathNonzeroRows;
				W.PathEnergy += B.PathEnergy;
			}
			if (FMath::IsFinite(B.OutputEnergy) && B.OutputEnergy > 1.e-9) ++W.OutputNonzeroRows;
			if (FMath::IsFinite(B.Occlusion))
			{
				W.OcclusionMean += B.Occlusion;
				W.OcclusionMin = FMath::Min(W.OcclusionMin, double(B.Occlusion));
				W.OcclusionMax = FMath::Max(W.OcclusionMax, double(B.Occlusion));
			}
			if (FMath::IsFinite(B.DirX) && FMath::IsFinite(B.DirY) && FMath::IsFinite(B.DirZ))
				W.MeanDirection += FVector(B.DirX, B.DirY, B.DirZ);
			else W.bContinuityPass = false;
		}
		W.AudioComponentId = State->AudioComponentId;
		W.WorldGeneration = State->WorldGeneration;
		W.VoiceGeneration = State->VoiceGeneration;
		W.SpatialVersion = State->SpatialVersion;
		W.Voice = State->Voice;

		const uint64 SnapshotLimit = FMath::Min<uint64>(W.SnapshotEnd, FIMAcousticDeviceBridge::ProbeSnapshotCapacity);
		const uint64 SnapshotBegin = FMath::Min<uint64>(W.SnapshotBegin, SnapshotLimit);
		bool bHaveSourceStart = false;
		for (uint64 I = SnapshotBegin; I < SnapshotLimit; ++I)
		{
			if (State->Bridge->SnapshotDone[I].load(std::memory_order_acquire) != I + 1) continue;
			const FIMAcousticSnapshotProbe& P = State->Bridge->SnapshotProbes[I];
			++W.SnapshotRows;
			const double ListenerDelta = FVector::Distance(FVector(P.ListenerUEX, P.ListenerUEY, P.ListenerUEZ), IMS3Listener);
			W.ListenerMaxDeltaCm = FMath::Max(W.ListenerMaxDeltaCm, ListenerDelta);
			State->ListenerMaxDeltaCm = FMath::Max(State->ListenerMaxDeltaCm, ListenerDelta);
			if (ListenerDelta > IMS3PositionToleranceCm) W.bListenerPass = false;
			if (P.Source0AudioId == State->AudioComponentId)
			{
				++W.SnapshotSourceIdentityRows;
				const FVector SourceSdk(P.Source0X, P.Source0Y, P.Source0Z);
				if (!bHaveSourceStart)
				{
					W.SourceSdkStart = SourceSdk;
					bHaveSourceStart = true;
				}
				W.SourceSdkEnd = SourceSdk;
			}
		}

		const uint32 SourceFrames = W.SourcePcmEnd >= W.SourcePcmBegin ? W.SourcePcmEnd - W.SourcePcmBegin : 0;
		const uint32 WetFrames = W.WetPcmEnd >= W.WetPcmBegin ? W.WetPcmEnd - W.WetPcmBegin : 0;
		W.SourcePcmSamples = SourceFrames;
		W.WetPcmSamples = WetFrames * 2;
		for (uint32 I = W.SourcePcmBegin; I < W.SourcePcmEnd; ++I)
		{
			const double V = State->MetaContext->CapturedSource[int32(I)];
			W.SourcePcmEnergy += V * V;
		}
		for (uint32 I = W.WetPcmBegin; I < W.WetPcmEnd; ++I)
		{
			const float L = State->MetaContext->CapturedWet[int32(2 * I)];
			const float R = State->MetaContext->CapturedWet[int32(2 * I + 1)];
			W.WetPcmEnergy += double(L) * L + double(R) * R;
			if (FMath::Abs(L) > IMS3PcmNonzeroThreshold || FMath::Abs(R) > IMS3PcmNonzeroThreshold) ++W.WetNonzeroFrames;
		}
		const double SourceMotionMeters = FVector::Distance(W.SourceSdkStart, W.SourceSdkEnd);
		const bool bExpectedRoute = W.OutputNonzeroRows > 0
			&& (W.Leg == 1 ? W.DirectNonzeroRows > 0 : W.PathNonzeroRows > 0);
		W.bPass = W.SourceRows > 0 && W.AcceptedRows > 0 && W.RejectedRows == 0 && W.IdentityRows == W.AcceptedRows
			&& bExpectedRoute && W.SourcePcmSamples > 0 && W.SourcePcmEnergy > 0.0
			&& W.WetPcmSamples > 0 && W.SnapshotRows > 0 && W.SnapshotSourceIdentityRows > 0
			&& SourceMotionMeters >= IMS3SourceMoveMinimumMeters && W.SourceMaxDeltaCm <= IMS3PositionToleranceCm
			&& W.bContinuityPass && W.LastSequence > W.FirstSequence && W.bListenerPass;
		if (!W.bPass)
		{
			W.Failure = FString::Printf(TEXT("rows=%u accepted=%u identity=%u direct=%u path=%u/%u output=%u source_samples=%u wet_samples=%u snapshots=%u source_snapshots=%u source_motion_m=%.4f listener_delta_cm=%.4f continuity=%d listener=%d"),
				W.SourceRows, W.AcceptedRows, W.IdentityRows, W.DirectNonzeroRows, W.PathNonzeroRows, W.PathRows, W.OutputNonzeroRows, W.SourcePcmSamples, W.WetPcmSamples,
				W.SnapshotRows, W.SnapshotSourceIdentityRows, SourceMotionMeters, W.ListenerMaxDeltaCm,
				W.bContinuityPass ? 1 : 0, W.bListenerPass ? 1 : 0);
		}
	}

	bool Finish(bool bPass, const FString& Message)
	{
		if (Stage == 90) return false;
		if (!bPass) State->AddFailure(Message);
		State->ResultMessage = Message;
		if (State->Windows.Num() != 3) State->AddFailure(FString::Printf(TEXT("Expected 3 route windows; captured %d."), State->Windows.Num()));
		if (State->AudioComponentId == 0) State->AddFailure(TEXT("No ordinary MetaSound component identity was observed."));
		if (SourceAudio.IsValid()) SourceAudio->Stop();
		if (!IMS3WriteTraceEvidence(State)) State->AddFailure(TEXT("Failed to write a PCM or trace evidence file; fail-closed."));
		State->RestoreGlobals();
		if (GUnrealEd) GUnrealEd->RequestEndPlayMap();
		CleanupAt = FPlatformTime::Seconds();
		Stage = 90;
		return false;
	}

	bool FinalizeAfterCleanup()
	{
		if (!State->bMapRestoreVerified) State->AddFailure(TEXT("Map restore not verified."));
		if (!State->bAssetsRestoreVerified) State->AddFailure(TEXT("Persistent map/source/bake assets not verified unchanged."));
		const bool bSavedSummary = IMS3WriteSummary(State, true);
		State->bFinalReportSaved = bSavedSummary;
		if (!bSavedSummary) State->AddFailure(TEXT("Failed to write final summary report; fail-closed."));
		if (State->Test)
		{
			if (State->bPassed) State->Test->AddInfo(FString::Printf(TEXT("S3 fixed-listener source move complete; evidence=%s"), *State->EvidenceDirectory));
			else State->Test->AddError(FString::Printf(TEXT("S3 fixed-listener source move failed: %s; evidence=%s"), *State->Failure, *State->EvidenceDirectory));
		}
		if (State->bPassed)
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS3FixedListener PASS evidence=%s"), *State->EvidenceDirectory);
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS3FixedListener FAIL reason=%s evidence=%s"),
				*State->Failure, *State->EvidenceDirectory);
		}
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticS3FixedListenerSourceMove %s evidence=%s"),
			State->bPassed ? TEXT("PASS") : TEXT("FAIL"), *State->EvidenceDirectory);
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), State->bPassed ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] IMExitEditor %s"), State->bPassed ? TEXT("PASS") : TEXT("FAIL"));
		return true;
	}

	TSharedRef<FIMS3State> State;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<AActor> SourceActor;
	TWeakObjectPtr<UAudioComponent> SourceAudio;
	FString SpawnFailure;
	double StartedAt = 0.0;
	double StageAt = 0.0;
	double WindowStartAt = 0.0;
	double LastPositionSampleAt = -1.0e30;
	double WindowSourceMaxDeltaCm = 0.0;
	double CleanupAt = 0.0;
	int32 Stage = 0;
	int32 Leg = 0;
	bool bMapRestored = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticS3FixedListenerSourceMoveTest,
	"IceMoon.AcousticField.S3.FixedListenerSourceMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIMAcousticS3FixedListenerSourceMoveTest::RunTest(const FString&)
{
	const auto FailBeforePIE = [this](const FString& Reason)
	{
		AddError(Reason);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] AcousticS3FixedListenerSourceMove FAIL %s"), *Reason);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] IMExitEditor FAIL"));
		return false;
	};
	if (!GEditor || !GUnrealEd)
	{
		return FailBeforePIE(TEXT("Editor automation requires an active Unreal Editor."));
	}
	if (IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3FindPIEWorld())
	{
		return FailBeforePIE(TEXT("Refusing to replace an already-running PIE world."));
	}
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld || !EditorWorld->GetOutermost() || EditorWorld->GetOutermost()->IsDirty())
	{
		return FailBeforePIE(TEXT("Refusing to change maps while the current editor map is missing or dirty."));
	}
	const FString OriginalMap = EditorWorld->GetOutermost()->GetName();
	if (!FPackageName::IsValidLongPackageName(OriginalMap) || !FPackageName::DoesPackageExist(OriginalMap))
	{
		return FailBeforePIE(TEXT("Refusing to start because the current editor map cannot be restored from a saved package."));
	}
	if (!FPackageName::DoesPackageExist(IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Map))
	{
		return FailBeforePIE(TEXT("The read-only W1 doorway map is missing; the test will not generate or save it."));
	}

	TSharedRef<IMAcousticS3FixedListenerSourceMoveTestPrivate::FIMS3State> State = MakeShared<IMAcousticS3FixedListenerSourceMoveTestPrivate::FIMS3State>();
	State->Test = this;
	State->RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	State->OriginalMap = OriginalMap;
	State->OriginalMapDiskPath = FPackageName::LongPackageNameToFilename(OriginalMap, FPackageName::GetMapPackageExtension());
	State->EvidenceDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/S3FixedListenerSourceMove"), State->RunId);
	State->MapDiskPath = FPackageName::LongPackageNameToFilename(IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Map, FPackageName::GetMapPackageExtension());
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("IceMoonAcousticField"));
	if (!Plugin.IsValid())
	{
		return FailBeforePIE(TEXT("Could not resolve IceMoonAcousticField plugin for read-only asset fingerprinting."));
	}
	State->SourceDiskPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Content/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay.uasset"));
	State->MapHashBefore = IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3HashFile(State->MapDiskPath);
	State->OriginalMapHashBefore = IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3HashFile(State->OriginalMapDiskPath);
	State->SourceHashBefore = IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3HashFile(State->SourceDiskPath);
	if (State->MapHashBefore.IsEmpty() || State->OriginalMapHashBefore.IsEmpty() || State->SourceHashBefore.IsEmpty())
	{
		return FailBeforePIE(TEXT("Could not fingerprint the read-only map and ordinary MetaSound source before the test."));
	}
	if (!IFileManager::Get().MakeDirectory(*State->EvidenceDirectory, true))
	{
		return FailBeforePIE(FString::Printf(TEXT("Could not create test evidence directory: %s"), *State->EvidenceDirectory));
	}
	TSharedRef<FJsonObject> Start = MakeShared<FJsonObject>();
	Start->SetStringField(TEXT("schema"), TEXT("IMAcousticS3FixedListenerSourceMove.start.v1"));
	Start->SetStringField(TEXT("run_id"), State->RunId);
	Start->SetStringField(TEXT("test_map"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Map);
	Start->SetStringField(TEXT("original_editor_map"), State->OriginalMap);
	Start->SetStringField(TEXT("original_editor_map_md5_before"), State->OriginalMapHashBefore);
	Start->SetStringField(TEXT("map_md5_before"), State->MapHashBefore);
	Start->SetStringField(TEXT("source_asset"), State->GraphAssetPath);
	Start->SetStringField(TEXT("source_asset_md5_before"), State->SourceHashBefore);
	Start->SetStringField(TEXT("movement_route"), TEXT("A -> door -> B -> A; only source location changes"));
	Start->SetStringField(TEXT("listener_contract"), TEXT("one fixed SetAudioListenerOverride for the entire PIE route"));
	Start->SetNumberField(TEXT("listener_x_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Listener.X);
	Start->SetNumberField(TEXT("listener_y_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Listener.Y);
	Start->SetNumberField(TEXT("listener_z_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Listener.Z);
	Start->SetNumberField(TEXT("position_tolerance_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3PositionToleranceCm);
	Start->SetNumberField(TEXT("route_window_seconds"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3WindowSeconds);
	Start->SetNumberField(TEXT("route_warmup_seconds"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3WarmupSeconds);
	Start->SetNumberField(TEXT("route_settle_seconds"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3LegSettleSeconds);
	Start->SetNumberField(TEXT("A_x_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Waypoints[0].X);
	Start->SetNumberField(TEXT("A_y_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Waypoints[0].Y);
	Start->SetNumberField(TEXT("door_x_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Waypoints[1].X);
	Start->SetNumberField(TEXT("door_y_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Waypoints[1].Y);
	Start->SetNumberField(TEXT("B_x_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Waypoints[2].X);
	Start->SetNumberField(TEXT("B_y_cm"), IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Waypoints[2].Y);
	Start->SetStringField(TEXT("map_asset_policy"), TEXT("existing map and baked asset are read-only; route flags and transient source exist only in PIE; no map save or bake"));
	if (!IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3SaveJson(FPaths::Combine(State->EvidenceDirectory, TEXT("run-context.json")), Start))
	{
		return FailBeforePIE(TEXT("Could not write initial evidence manifest; test stopped before changing editor state."));
	}

	State->OriginalBackgroundVolume = FApp::GetUnfocusedVolumeMultiplier();
	State->bOriginalAllowBackgroundAudio = GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	FApp::SetUnfocusedVolumeMultiplier(1.0f);
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;
	GUnrealEd->AutomationLoadMap(IMAcousticS3FixedListenerSourceMoveTestPrivate::IMS3Map, false, &Error);
	if (!Error.IsEmpty())
	{
		State->RestoreGlobals();
		return FailBeforePIE(FString::Printf(TEXT("Could not load the existing read-only W1 test map: %s"), *Error));
	}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticS3FixedListenerSourceMoveTestPrivate::FIMAcousticS3FixedListenerCommand(State));
	return true;
}

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
