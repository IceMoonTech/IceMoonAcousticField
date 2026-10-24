#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticOperationMemorySampler.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticG2EditorTickerTestPrivate
{
// The transport loads a frozen map first. Count and measure the real scheduled
// ticker body without substituting repeated direct ValidateCurrentBake calls.
class FIMAcousticG2EditorTickerCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticG2EditorTickerCommand(FAutomationTestBase* InTest)
		: Test(InTest), MemorySampler(0.002) {}

	~FIMAcousticG2EditorTickerCommand() override
	{
		// Automation cancellation can destroy the command before Finish. The
		// GT ticker must lose its borrowed pointer before the sampler joins.
		DetachProfiler();
		MemorySampler.StopAndCollect();
	}

	bool Update() override
	{
		if (!Started)
		{
			Started = true;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World || World->IsGameWorld())
				return Finish(false, TEXT("Frozen non-PIE editor world must already be loaded."));
			MapPath = World->GetPackage()->GetName();
			for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
			{
				if (Volume.IsValid()) return Finish(false, TEXT("Multiple bake volumes in the target editor world."));
				Volume = *It;
			}
			if (!Volume.IsValid() || !Volume->BakedField)
				return Finish(false, TEXT("Current saved bake binding unavailable."));
			OriginalAssetPath = Volume->BakedField->GetPathName();
			SceneFingerprint = Volume->BakedField->SceneFingerprint;
			FString Failure;
			if (!Volume->ValidateCurrentBake(Failure))
				return Finish(false, TEXT("Current scene bake is stale: ") + Failure);
			Before = FPlatformMemory::GetStats();
			if (!MemorySampler.Start())
				return Finish(false, TEXT("Operation memory sampler did not start."));
			StartedAt = FPlatformTime::Seconds();
			Volume->IM_SetEditorValidationProfilingForTest(true, &MemorySampler);
			return false;
		}
		if (!Volume.IsValid()) return Finish(false, TEXT("Bake volume disappeared during Editor ticker measurement."));
		int32 Ticks = 0;
		double CaptureMs = 0.0, ValidationMs = 0.0, MaxTickMs = 0.0;
		Volume->IM_GetEditorValidationProfileForTest(Ticks, CaptureMs, ValidationMs, MaxTickMs);
		if (Ticks < 3)
		{
			if (FPlatformTime::Seconds() - StartedAt > 12.0)
				return Finish(false, FString::Printf(TEXT("Actual Editor ticker produced only %d of 3 required checks in 12 seconds."), Ticks));
			return false;
		}
		const FPlatformMemoryStats After = FPlatformMemory::GetStats();
		const bool bBindingSame = Volume->BakedField
			&& Volume->BakedField->GetPathName() == OriginalAssetPath
			&& Volume->BakedField->SceneFingerprint == SceneFingerprint;
		const bool bStatusValid = Volume->Status.StartsWith(TEXT("Bake valid"))
			|| Volume->Status.StartsWith(TEXT("Scene check passed"))
			|| Volume->Status.StartsWith(TEXT("Baked field"));
		if (!bBindingSame || !bStatusValid)
			return Finish(false, FString::Printf(TEXT("Editor ticker changed binding or rejected current bake: %s"), *Volume->Status));
		// Detach before collection so a later scheduled tick cannot call a
		// stopped sampler. No window includes the one-second idle interval.
		DetachProfiler();
		const auto MemoryReport = MemorySampler.StopAndCollect();
		const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/G2EditorTicker"), RunId);
		const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("run_id"), RunId);
		Report->SetStringField(TEXT("map"), MapPath);
		Report->SetStringField(TEXT("bound_asset"), OriginalAssetPath);
		Report->SetStringField(TEXT("fingerprint"), SceneFingerprint);
		Report->SetNumberField(TEXT("exported_triangles"), Volume->ExportedTriangles);
		Report->SetNumberField(TEXT("generated_probes"), Volume->GeneratedProbes);
		Report->SetNumberField(TEXT("scheduled_interval_s"), 1.0);
		Report->SetNumberField(TEXT("scheduled_ticks"), Ticks);
		Report->SetNumberField(TEXT("capture_sum_ms"), CaptureMs);
		Report->SetNumberField(TEXT("bound_asset_validate_sum_ms"), ValidationMs);
		Report->SetNumberField(TEXT("max_full_tick_ms"), MaxTickMs);
		const auto Counters = MakeShared<FJsonObject>();
		Counters->SetNumberField(TEXT("working_set_before_bytes"), double(Before.UsedPhysical));
		Counters->SetNumberField(TEXT("working_set_after_bytes"), double(After.UsedPhysical));
		Counters->SetNumberField(TEXT("commit_before_bytes"), double(Before.UsedVirtual));
		Counters->SetNumberField(TEXT("commit_after_bytes"), double(After.UsedVirtual));
		Counters->SetNumberField(TEXT("os_lifetime_peak_working_set_before_bytes"), double(Before.PeakUsedPhysical));
		Counters->SetNumberField(TEXT("os_lifetime_peak_working_set_after_bytes"), double(After.PeakUsedPhysical));
		Counters->SetNumberField(TEXT("os_lifetime_peak_commit_before_bytes"), double(Before.PeakUsedVirtual));
		Counters->SetNumberField(TEXT("os_lifetime_peak_commit_after_bytes"), double(After.PeakUsedVirtual));
		Report->SetObjectField(TEXT("process_counters"), Counters);
		Report->SetObjectField(TEXT("operation_memory"), IM_AcousticOperationMemoryReportToJson(MemoryReport));
		Report->SetStringField(TEXT("note"), TEXT("Actual scheduled ticker windows only; sampled maxima with coverage, not exact peaks. OS lifetime peaks include prior work. No fixed 60-placement/one-mesh assumption."));
		FString ReportText;
		if (!FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&ReportText))
			|| !IFileManager::Get().MakeDirectory(*Dir, true)
			|| !FFileHelper::SaveStringToFile(ReportText, *FPaths::Combine(Dir, TEXT("report.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			return Finish(false, TEXT("Editor ticker report could not be written."));
		UE_LOG(LogTemp, Display,
			TEXT("IMLogs AcousticG2EditorTickerSummary ticks=%d capture_sum_ms=%.3f asset_validate_sum_ms=%.3f max_tick_ms=%.3f dir=%s"),
			Ticks, CaptureMs, ValidationMs, MaxTickMs, *Dir);
		Test->AddInfo(FString::Printf(TEXT("G2 actual Editor ticker report: %s"), *Dir));
		return Finish(true, TEXT("Current bound-asset Editor ticker measured; inspect memory coverage separately."));
	}

private:
	void DetachProfiler()
	{
		if (Volume.IsValid()) Volume->IM_SetEditorValidationProfilingForTest(false);
	}
	bool Finish(bool Success, const FString& Message)
	{
		DetachProfiler();
		MemorySampler.StopAndCollect();
		if (!Success) Test->AddError(Message);
		else
		{
			Test->AddInfo(Message);
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] G2EditorTicker PASS"));
			UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS G2EditorTicker"));
		}
		return true;
	}
	FAutomationTestBase* Test = nullptr;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	FIMAcousticOperationMemorySampler MemorySampler;
	FString MapPath, OriginalAssetPath, SceneFingerprint;
	FPlatformMemoryStats Before{};
	double StartedAt = 0.0;
	bool Started = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2EditorTickerTest,
	"IceMoon.AcousticField.G2.EditorTickerBoundAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2EditorTickerTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2EditorTickerTestPrivate::FIMAcousticG2EditorTickerCommand(this));
	return true;
}
#endif
