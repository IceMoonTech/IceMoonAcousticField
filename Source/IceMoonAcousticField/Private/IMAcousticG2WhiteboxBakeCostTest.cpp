#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace IMAcousticG2WhiteboxBakeCostTestPrivate
{
constexpr const TCHAR* IMG2WhiteboxMap = TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");

class FIMAcousticG2WhiteboxBakeCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticG2WhiteboxBakeCommand(FAutomationTestBase* InTest) : Test(InTest) {}

	bool Update() override
	{
		if (Stage == 0)
		{
			if (!FEditorFileUtils::LoadMap(IMG2WhiteboxMap, false, true))
				return Finish(false, TEXT("Cannot load the existing whitebox map."));
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) return Finish(false, TEXT("Editor world unavailable after whitebox load."));
			for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
			{
				if (Volume.IsValid()) return Finish(false, TEXT("Multiple acoustic bake volumes in whitebox map."));
				Volume = *It;
			}
			if (!Volume.IsValid() || !Volume->BakedField)
				return Finish(false, TEXT("Existing whitebox bake binding is absent."));
			OriginalAssetPath = Volume->BakedField->GetPathName();
			StartedAt = FPlatformTime::Seconds();
			Before = FPlatformMemory::GetStats();
			ObserveMemory(Before);
			Volume->GenerateProbes();
			ObserveMemory(FPlatformMemory::GetStats());
			ProbeMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;
			if (Volume->GeneratedProbes != 632 || Volume->ExportedTriangles != 2880)
				return Finish(false, FString::Printf(
					TEXT("Whitebox baseline changed: triangles=%d probes=%d status=%s"),
					Volume->ExportedTriangles, Volume->GeneratedProbes, *Volume->Status));
			Volume->Bake();
			Stage = 1;
			return false;
		}
		if (!Volume.IsValid()) return Finish(false, TEXT("Whitebox bake owner was destroyed."));
		ObserveMemory(FPlatformMemory::GetStats());
		if (FPlatformTime::Seconds() - StartedAt > 900.0)
		{
			Volume->CancelBake();
			return Finish(false, TEXT("Whitebox bake exceeded the 900 second resource guard."));
		}
		UIMAcousticBakeAsset* Asset = Volume->BakedField.Get();
		if (!Asset || Asset->GetPathName() == OriginalAssetPath)
		{
			if (Volume->Status.Contains(TEXT("failed"), ESearchCase::IgnoreCase)
				|| Volume->Status.Contains(TEXT("discarded"), ESearchCase::IgnoreCase)
				|| Volume->Status.Contains(TEXT("cancelled"), ESearchCase::IgnoreCase)
				|| Volume->Status.StartsWith(TEXT("Bake:")))
				return Finish(false, Volume->Status);
			return false;
		}
		const double TotalMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;
		FString Failure;
		if (!Volume->ValidateCurrentBake(Failure))
			return Finish(false, FString::Printf(TEXT("New whitebox bake invalid: %s"), *Failure));
		if (Volume->ExportedTriangles != 2880 || Volume->GeneratedProbes != 632)
			return Finish(false, TEXT("Whitebox geometry or probe count changed after bake."));
		const FPlatformMemoryStats After = FPlatformMemory::GetStats();
		ObserveMemory(After);
		const FString NewAssetPath = Asset->GetPathName();
		const int64 PackedSceneBytes = Asset->SceneData.Num();
		const int64 PackedProbeBytes = Asset->ProbeData.Num();
		const double MiB = 1024.0 * 1024.0;
		const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("AcousticV2/G2WhiteboxBake"), RunId);
		const FString Report = FString::Printf(
			TEXT("{\"run_id\":\"%s\",\"map\":\"%s\",\"placements\":60,\"unique_mesh_assets\":1,\"triangles\":2880,\"probes\":632,\"probe_generate_ms\":%.3f,\"total_bake_ms\":%.3f,\"packed_scene_bytes\":%lld,\"packed_probe_bytes\":%lld,\"original_asset\":\"%s\",\"new_asset\":\"%s\",\"working_set_mib_before_after\":[%.3f,%.3f],\"commit_mib_before_after\":[%.3f,%.3f],\"sampled_peak_working_set_mib\":%.3f,\"sampled_peak_commit_mib\":%.3f,\"process_peak_working_set_mib_before_after\":[%.3f,%.3f],\"process_peak_commit_mib_before_after\":[%.3f,%.3f],\"memory_note\":\"Sampled peak is a lower bound; process lifetime peak includes work outside this phase.\"}"),
			*RunId, IMG2WhiteboxMap, ProbeMs, TotalMs,
			static_cast<long long>(PackedSceneBytes), static_cast<long long>(PackedProbeBytes),
			*OriginalAssetPath, *NewAssetPath,
			Before.UsedPhysical / MiB, After.UsedPhysical / MiB,
			Before.UsedVirtual / MiB, After.UsedVirtual / MiB,
			ObservedPeakWorkingSet / MiB, ObservedPeakCommit / MiB,
			Before.PeakUsedPhysical / MiB, After.PeakUsedPhysical / MiB,
			Before.PeakUsedVirtual / MiB, After.PeakUsedVirtual / MiB);
		if (!IFileManager::Get().MakeDirectory(*Dir, true)
			|| !FFileHelper::SaveStringToFile(Report,
				*FPaths::Combine(Dir, TEXT("report.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			return Finish(false, TEXT("G2 whitebox bake report could not be written."));
		UE_LOG(LogTemp, Display,
			TEXT("IMLogs AcousticG2WhiteboxBake triangles=2880 probes=632 probe_generate_ms=%.3f total_bake_ms=%.3f packed_scene_bytes=%lld packed_probe_bytes=%lld dir=%s"),
			ProbeMs, TotalMs, static_cast<long long>(PackedSceneBytes),
			static_cast<long long>(PackedProbeBytes), *Dir);
		Test->AddInfo(FString::Printf(TEXT("G2 whitebox bake report: %s"), *Dir));
		return Finish(true, TEXT("G2 whitebox product bake completed."));
	}

private:
	void ObserveMemory(const FPlatformMemoryStats& Memory)
	{
		ObservedPeakWorkingSet = FMath::Max(ObservedPeakWorkingSet, Memory.UsedPhysical);
		ObservedPeakCommit = FMath::Max(ObservedPeakCommit, Memory.UsedVirtual);
	}

	bool Finish(bool Success, const FString& Message)
	{
		// Bake saves a new isolated asset, but the source map must retain its
		// original disk binding. Reload discards only this test's in-memory edit.
		const bool Restored = FEditorFileUtils::LoadMap(IMG2WhiteboxMap, false, true);
		bool BindingRestored = false;
		if (Restored && GEditor)
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				int32 VolumeCount = 0;
				for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
				{
					++VolumeCount;
					BindingRestored = It->BakedField
						&& It->BakedField->GetPathName() == OriginalAssetPath;
				}
				BindingRestored &= VolumeCount == 1;
			}
		}
		if (!Success || !BindingRestored)
			Test->AddError(FString::Printf(TEXT("%s; map binding restored=%d"),
				*Message, BindingRestored ? 1 : 0));
		else
		{
			Test->AddInfo(Message);
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] G2WhiteboxBakeCost PASS"));
			UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS G2WhiteboxBakeCost"));
		}
		return true;
	}

	FAutomationTestBase* Test = nullptr;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	FString OriginalAssetPath;
	FPlatformMemoryStats Before{};
	uint64 ObservedPeakWorkingSet = 0;
	uint64 ObservedPeakCommit = 0;
	double StartedAt = 0.0;
	double ProbeMs = 0.0;
	int32 Stage = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2WhiteboxBakeCostTest,
	"IceMoon.AcousticField.G2.WhiteboxBakeCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2WhiteboxBakeCostTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2WhiteboxBakeCostTestPrivate::FIMAcousticG2WhiteboxBakeCommand(this));
	return true;
}
#endif
