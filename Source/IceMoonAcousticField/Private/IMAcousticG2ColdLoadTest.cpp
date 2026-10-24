#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "UObject/StrongObjectPtr.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "IMAcousticSimulation.h"
#include "IMAcousticOperationMemorySampler.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Read-only current-bound-asset measurement. Keep asset readback distinct from
// Steam Audio deserialization; a package byte count is not runtime memory.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2ColdLoadTest,
	"IceMoon.AcousticField.G2.CurrentBakeColdLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2ColdLoadTest::RunTest(const FString&)
{
	// Resolve the actual saved binding after validating its current scene.
	// This map already loads its hard-referenced UAsset; only the new SDK
	// simulator below is cold. Do not call resident asset readback cold I/O.
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || World->IsGameWorld())
	{
		AddError(TEXT("Current audition editor world must be loaded before binding readback."));
		return false;
	}
	const FString WorldPath = World->GetPackage()->GetName();
	AIMAcousticBakeVolume* Volume = nullptr;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
	{
		if (Volume)
		{
			AddError(TEXT("Current audition world contains multiple bake-volume bindings."));
			return false;
		}
		Volume = *It;
	}
	FIMAcousticOperationMemorySampler MemorySampler(0.002);
	if (!MemorySampler.Start()) { AddError(TEXT("Operation memory sampler did not start.")); return false; }
	const uint64 BindingMemoryId = MemorySampler.BeginOperation(TEXT("current_scene_capture_and_bake_validation"));
	FString BindingFailure;
	if (!Volume || !Volume->BakedField || !Volume->ValidateCurrentBake(BindingFailure))
	{
		AddError(TEXT("Current audition bake binding is absent or stale: ") + BindingFailure);
		return false;
	}
	MemorySampler.EndOperation(BindingMemoryId);
	const FString AssetPath = Volume->BakedField->GetPathName();
	const FString SceneFingerprint = Volume->BakedField->SceneFingerprint;
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDir = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AcousticV2/G2ColdLoad"), RunId);
	const FPlatformMemoryStats Before = FPlatformMemory::GetStats();
	const uint64 ReadbackMemoryId = MemorySampler.BeginOperation(TEXT("resident_asset_readback_and_sdk_input_copy"));
	const double ReadbackStart = FPlatformTime::Seconds();
	TStrongObjectPtr<UIMAcousticBakeAsset> Asset(Volume->BakedField.Get());
	if (!Asset.IsValid()) { AddError(TEXT("Current audition bake asset cannot be loaded.")); return false; }
	FString Failure;
	if (!Asset->Validate(WorldPath, SceneFingerprint, Failure))
	{
		AddError(FString::Printf(TEXT("Current audition bake rejected: %s"), *Failure));
		return false;
	}
	TArray<FVector4> ProbePreview;
	FVector OriginCm;
	if (!Asset->GetProbePreview(WorldPath, SceneFingerprint, ProbePreview, OriginCm, Failure))
	{
		AddError(FString::Printf(TEXT("Current audition probe preview rejected: %s"), *Failure));
		return false;
	}
	TArray<uint8> RawScene;
	TArray<uint8> RawProbes;
	if (!Asset->ReadPayload(WorldPath, SceneFingerprint, RawScene, RawProbes, Failure))
	{
		AddError(FString::Printf(TEXT("Current audition raw SDK payload read failed: %s"), *Failure));
		return false;
	}
	FIMAcousticBakeData Data;
	Data.Scene.assign(RawScene.GetData(), RawScene.GetData() + RawScene.Num());
	Data.ProbeBatch.assign(RawProbes.GetData(), RawProbes.GetData() + RawProbes.Num());
	Data.CoverageProbes.reserve(ProbePreview.Num());
	for (const FVector4& Probe : ProbePreview)
	{
		Data.CoverageProbes.push_back({{static_cast<float>(Probe.X), static_cast<float>(Probe.Y),
			static_cast<float>(Probe.Z)}, static_cast<float>(Probe.W)});
	}
	const double ReadbackMs = (FPlatformTime::Seconds() - ReadbackStart) * 1000.0;
	MemorySampler.EndOperation(ReadbackMemoryId);
	const FPlatformMemoryStats AfterReadback = FPlatformMemory::GetStats();
	if (Data.Scene.empty() || Data.ProbeBatch.empty() || Data.CoverageProbes.empty())
	{
		AddError(TEXT("Current bake contains an empty serialized section or coverage set."));
		return false;
	}
	FIMAcousticSimulation Simulation;
	std::string SDKError;
	if (!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), SDKError))
	{
		AddError(UTF8_TO_TCHAR(SDKError.c_str())); return false;
	}
	const uint64 LoadMemoryId = MemorySampler.BeginOperation(TEXT("new_sdk_simulation_load"));
	const double LoadStart = FPlatformTime::Seconds();
	if (!Simulation.Load(Data, 48000, 512, SDKError))
	{
		AddError(FString::Printf(TEXT("Current audition SDK cold load failed: %s"),
			UTF8_TO_TCHAR(SDKError.c_str())));
		return false;
	}
	const double LoadMs = (FPlatformTime::Seconds() - LoadStart) * 1000.0;
	MemorySampler.EndOperation(LoadMemoryId);
	const FPlatformMemoryStats AfterLoad = FPlatformMemory::GetStats();
	const auto MemoryReport = MemorySampler.StopAndCollect();
	FString MemoryJson;
	FJsonSerializer::Serialize(IM_AcousticOperationMemoryReportToJson(MemoryReport),
		TJsonWriterFactory<>::Create(&MemoryJson));
	const double MiB = 1024.0 * 1024.0;
	const FString Report = FString::Printf(
		TEXT("{\"run_id\":\"%s\",\"asset\":\"%s\",\"world\":\"%s\",\"fingerprint\":\"%s\",\"raw_scene_bytes\":%lld,\"raw_probe_bytes\":%lld,\"packed_scene_bytes\":%lld,\"packed_probe_bytes\":%lld,\"probe_count\":%d,\"resident_asset_readback_ms\":%.3f,\"sdk_load_ms\":%.3f,\"working_set_mib\":[%.3f,%.3f,%.3f],\"commit_mib\":[%.3f,%.3f,%.3f],\"process_peak_working_set_mib\":%.3f,\"process_peak_commit_mib\":%.3f,\"package_io_measured\":false,\"sdk_simulation_created_for_this_load\":true,\"memory_note\":\"Process lifetime peaks include prior work; operation sampled maxima and coverage are separate.\",\"operation_memory\":%s}"),
		*RunId, *AssetPath, *WorldPath, *SceneFingerprint,
		static_cast<long long>(Data.Scene.size()), static_cast<long long>(Data.ProbeBatch.size()),
		static_cast<long long>(Asset->SceneData.Num()), static_cast<long long>(Asset->ProbeData.Num()),
		static_cast<int32>(Data.CoverageProbes.size()), ReadbackMs, LoadMs,
		Before.UsedPhysical / MiB, AfterReadback.UsedPhysical / MiB, AfterLoad.UsedPhysical / MiB,
		Before.UsedVirtual / MiB, AfterReadback.UsedVirtual / MiB, AfterLoad.UsedVirtual / MiB,
		AfterLoad.PeakUsedPhysical / MiB, AfterLoad.PeakUsedVirtual / MiB, *MemoryJson);
	if (!IFileManager::Get().MakeDirectory(*ReportDir, true)
		|| !FFileHelper::SaveStringToFile(Report,
			*FPaths::Combine(ReportDir, TEXT("report.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		AddError(TEXT("G2 cold-load report could not be written.")); return false;
	}
	UE_LOG(LogTemp, Display,
		TEXT("IMLogs AcousticG2ColdLoad raw_scene_bytes=%lld raw_probe_bytes=%lld packed_scene_bytes=%d packed_probe_bytes=%d probes=%d readback_ms=%.3f sdk_load_ms=%.3f dir=%s"),
		static_cast<long long>(Data.Scene.size()), static_cast<long long>(Data.ProbeBatch.size()),
		Asset->SceneData.Num(), Asset->ProbeData.Num(), static_cast<int32>(Data.CoverageProbes.size()),
		ReadbackMs, LoadMs, *ReportDir);
	AddInfo(FString::Printf(TEXT("G2 current-bake cold-load report: %s"), *ReportDir));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] G2CurrentBakeColdLoad PASS"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS G2CurrentBakeColdLoad"));
	return true;
}
#endif
