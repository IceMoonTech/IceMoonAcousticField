#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSimulation.h"
#include "IMAcousticOperationMemorySampler.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticG2BakeOperationTestPrivate
{
// Evidence-only owner: ordinary editor ticks drive Bake's weak GT completion
// ticker. We never tick the world manually, save the level, or reuse an old
// lifetime memory maximum as the operation maximum. The new saved asset stays
// as evidence; the original level binding and dirty flag are restored in memory.
struct FIMCurrentBakeOperationState
{
	explicit FIMCurrentBakeOperationState(FAutomationTestBase& InTest,
		AIMAcousticBakeVolume& InVolume)
		: Test(InTest), Volume(&InVolume), Original(InVolume.BakedField.Get()),
			Memory(0.002), Report(MakeShared<FJsonObject>())
	{
		WorldPath = InVolume.GetWorld()->GetPackage()->GetName();
		Fingerprint = Original->SceneFingerprint;
		OriginalStatus = InVolume.Status;
		bOriginalDirty = InVolume.GetPackage()->IsDirty();
		RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		ReportDir = FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("AcousticV2/G2BakeOperation"), RunId);
		Report->SetStringField(TEXT("run_id"), RunId);
		Report->SetStringField(TEXT("world"), WorldPath);
		Report->SetStringField(TEXT("original_asset"), Original->GetPathName());
		Report->SetStringField(TEXT("fingerprint"), Fingerprint);
		Report->SetStringField(TEXT("measurement_scope"),
			TEXT("Whole Editor process sampled during actual Bake call through GT publication; includes capture, input copy, async SDK bake, freshness check, encoding and package save. Poll observation tail is included."));
		Report->SetBoolField(TEXT("level_save_requested"), false);
		Report->SetBoolField(TEXT("cold_package_io_measured"), false);
		Report->SetBoolField(TEXT("pure_plugin_memory_delta_measured"), false);
	}

	~FIMCurrentBakeOperationState()
	{
		// Automation latent destruction is GT-owned. It may occur before the
		// first Update or during cancellation; never let its bake publish later.
		check(IsInGameThread());
		if (!bFinished)
		{
			DrainAndRestore();
			Memory.StopAndCollect();
		}
	}

	uint64 BeginWindow(const TCHAR* Name)
	{
		const uint64 Id = Memory.BeginOperation(Name);
		if (!Id) Fail(FString(TEXT("Memory operation begin failed: ")) + Name);
		return Id;
	}

	bool EndWindow(uint64& Id)
	{
		const bool bEnded = Id && Memory.EndOperation(Id);
		if (!bEnded) Fail(TEXT("Memory operation end failed."));
		Id = 0;
		return bEnded;
	}

	void Fail(const FString& Message)
	{
		bFailed = true;
		Errors.Add(MakeShared<FJsonValueString>(Message));
		Test.AddError(Message);
	}

	bool Start()
	{
		FString Error;
		TArray<FVector4> OldProbes;
		FVector Origin;
		if (!Original->GetProbePreview(WorldPath, Fingerprint, OldProbes, Origin, Error))
		{
			Fail(TEXT("Original probe readback failed: ") + Error);
			return Finish();
		}
		ExpectedProbes = OldProbes.Num();
		ExpectedProbePreview = MoveTemp(OldProbes);
		if (!Memory.Start())
		{
			Fail(TEXT("Bake operation sampler did not start."));
			return Finish();
		}
		uint64 GenerateId = BeginWindow(TEXT("current_region_generate_probes"));
		if (!GenerateId) return Finish();
		Volume->GenerateProbes();
		if (!EndWindow(GenerateId)) return Finish();
		if (Volume->GeneratedProbes != ExpectedProbes)
		{
			Fail(TEXT("Generated probe count differs from the unchanged bound bake."));
			return Finish();
		}
		ExpectedTriangles = Volume->ExportedTriangles;
		Report->SetNumberField(TEXT("expected_probe_count"), ExpectedProbes);
		Report->SetNumberField(TEXT("expected_solid_triangles"), ExpectedTriangles);
		BakeId = BeginWindow(TEXT("actual_bake_call_to_observed_saved_binding"));
		if (!BakeId) return Finish();
		BakeStarted = FPlatformTime::Seconds();
		Volume->Bake();
		Report->SetNumberField(TEXT("bake_submit_ms"),
			(FPlatformTime::Seconds() - BakeStarted) * 1000.0);
		if (!Volume->Status.StartsWith(TEXT("Baking.")))
		{
			Fail(TEXT("Bake did not enter its existing async path: ") + Volume->Status);
			return Finish();
		}
		return true;
	}

	bool Update()
	{
		if (!Volume.IsValid())
		{
			Fail(TEXT("Evidence bake volume disappeared before completion."));
			return Finish();
		}
		if (Volume->BakedField.Get() != Original.Get())
		{
			if (!EndWindow(BakeId)) return Finish();
			Report->SetNumberField(TEXT("bake_ready_wall_ms_including_publication_tail"),
				(FPlatformTime::Seconds() - BakeStarted) * 1000.0);
			if (bCancelRequested)
				Fail(TEXT("A new asset was published after evidence cancellation."));
			ObserveCompletedAsset();
			return Finish();
		}
		const bool bPending = Volume->Status.StartsWith(TEXT("Baking."))
			|| Volume->Status.StartsWith(TEXT("Cancellation requested"));
		if (!bPending)
		{
			Fail(TEXT("Actual bake terminated without a new saved binding: ") + Volume->Status);
			return Finish();
		}
		const double Elapsed = FPlatformTime::Seconds() - BakeStarted;
		if (!bCancelRequested && Elapsed > 45.0)
		{
			bCancelRequested = true;
			Volume->CancelBake();
			Fail(TEXT("Evidence bake exceeded 45 seconds; existing cancellation requested."));
		}
		if (Elapsed > 90.0)
		{
			Fail(TEXT("Cancellation did not settle before the evidence deadline; no success verdict."));
			return Finish();
		}
		return false;
	}

	void ObserveCompletedAsset()
	{
		NewAsset = TStrongObjectPtr<UIMAcousticBakeAsset>(Volume->BakedField.Get());
		if (!NewAsset.IsValid()) { Fail(TEXT("Published evidence asset is invalid.")); return; }
		const FString NewPath = NewAsset->GetPathName();
		const FString EvidenceFilename = FPackageName::LongPackageNameToFilename(
			NewAsset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
		const int64 PackageBytes = IFileManager::Get().FileSize(*EvidenceFilename);
		if (PackageBytes <= 0)
		{
			Fail(TEXT("Published evidence bake has no saved package bytes.")); return;
		}
		FString Error;
		TArray<FVector4> Probes;
		FVector Origin;
		TArray<uint8> SceneBytes, ProbeBytes;
		if (!NewAsset->Validate(WorldPath, Fingerprint, Error)
			|| !NewAsset->GetProbePreview(WorldPath, Fingerprint, Probes, Origin, Error)
			|| !NewAsset->ReadPayload(WorldPath, Fingerprint, SceneBytes, ProbeBytes, Error))
		{
			Fail(TEXT("New evidence bake failed complete asset validation: ") + Error);
			return;
		}
		if (Probes.Num() != ExpectedProbes || Volume->ExportedTriangles != ExpectedTriangles)
		{
			Fail(TEXT("Evidence bake changed the fixed probe or captured triangle population."));
			return;
		}
		for (int32 Index = 0; Index < Probes.Num(); ++Index)
		{
			if (Probes[Index] != ExpectedProbePreview[Index])
			{
				Fail(TEXT("Evidence bake changed a fixed probe center or influence radius.")); return;
			}
		}
		Report->SetBoolField(TEXT("probe_centers_and_radii_unchanged"), true);
		FIMAcousticBakeData Data;
		Data.Scene.assign(SceneBytes.GetData(), SceneBytes.GetData() + SceneBytes.Num());
		Data.ProbeBatch.assign(ProbeBytes.GetData(), ProbeBytes.GetData() + ProbeBytes.Num());
		for (const FVector4& Probe : Probes)
			Data.CoverageProbes.push_back({{float(Probe.X), float(Probe.Y), float(Probe.Z)}, float(Probe.W)});
		FIMAcousticSimulation Simulation;
		std::string SDKError;
		if (!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), SDKError))
		{
			Fail(UTF8_TO_TCHAR(SDKError.c_str())); return;
		}
		uint64 LoadId = BeginWindow(TEXT("new_evidence_bake_sdk_load"));
		if (!LoadId) return;
		const bool bLoaded = Simulation.Load(Data, 48000, 512, SDKError);
		if (!EndWindow(LoadId)) return;
		if (!bLoaded) { Fail(FString(TEXT("New evidence bake SDK load failed: ")) + UTF8_TO_TCHAR(SDKError.c_str())); return; }
		Report->SetStringField(TEXT("evidence_asset"), NewPath);
		Report->SetStringField(TEXT("evidence_fingerprint"), NewAsset->SceneFingerprint);
		Report->SetNumberField(TEXT("probe_count"), Probes.Num());
		Report->SetNumberField(TEXT("solid_triangles"), Volume->ExportedTriangles);
		Report->SetNumberField(TEXT("non_solid_triangles"), Volume->NonSolidTriangles);
		Report->SetNumberField(TEXT("non_solid_sections"), Volume->NonSolidSections);
		Report->SetNumberField(TEXT("raw_scene_bytes"), SceneBytes.Num());
		Report->SetNumberField(TEXT("raw_probe_bytes"), ProbeBytes.Num());
		Report->SetNumberField(TEXT("packed_scene_bytes"), NewAsset->SceneData.Num());
		Report->SetNumberField(TEXT("packed_probe_bytes"), NewAsset->ProbeData.Num());
		Report->SetNumberField(TEXT("package_bytes"), double(PackageBytes));
		Report->SetStringField(TEXT("evidence_filename"), EvidenceFilename);
		Report->SetBoolField(TEXT("new_sdk_load_succeeded"), true);
	}

	bool Finish()
	{
		const bool bDrainedAndRestored = DrainAndRestore();
		if (!bDrainedAndRestored) Fail(TEXT("Evidence bake drain or original binding restore failed."));
		if (BakeId) EndWindow(BakeId);
		const auto Samples = Memory.StopAndCollect();
		Report->SetObjectField(TEXT("operation_memory"), IM_AcousticOperationMemoryReportToJson(Samples));
		bool bSamplesValid = Samples.bConfigurationValid && Samples.bWorkerStarted
			&& Samples.bProcessIdentityCaptured && Samples.bProcessIdentityVerifiedForAllSamples
			&& !Samples.bThreadStartFailed && !Samples.bConfigurationFailed
			&& !Samples.bProcessIdentityQueryFailed && !Samples.bProcessIdentityChanged
			&& !Samples.bMemoryQueryResultInvalid && !Samples.bWorkerExitedUnexpectedly
			&& !Samples.bSampleOverflowed && !Samples.bHasUnscopedInvalidSample
			&& Samples.InvalidSampleCount == 0 && Samples.DroppedSampleCount == 0
			&& Samples.InvalidOperationCallCount == 0 && Samples.Operations.Num() == 3;
		for (const auto& Window : Samples.Operations)
		{
			bSamplesValid = bSamplesValid && Window.bMaximaValid && Window.ValidSampleCount >= 2
				&& Window.EndSeconds.IsSet() && Window.ElapsedSeconds.IsSet()
				&& Window.ElapsedSeconds.GetValue() > 0.0
				&& Window.InvalidSampleCount == 0 && Window.DroppedSampleCount == 0;
		}
		if (!bSamplesValid && !bFailed) Fail(TEXT("Required Bake memory windows or sampler coverage are invalid."));
		Report->SetBoolField(TEXT("operation_sample_maxima_valid"), bSamplesValid);
		Report->SetBoolField(TEXT("original_binding_and_dirty_flag_restored"), bDrainedAndRestored);
		Report->SetBoolField(TEXT("bake_job_and_publication_ticker_drained_before_restore"), bDrainedAndRestored);
		Report->SetBoolField(TEXT("cancel_requested"), bCancelRequested);
		Report->SetArrayField(TEXT("errors"), Errors);
		Report->SetStringField(TEXT("status"), bFailed ? TEXT("FAIL") : TEXT("MEASURED_REQUIRES_ROOT_REVIEW"));
		FString Json;
		const bool bSaved = FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json))
			&& IFileManager::Get().MakeDirectory(*ReportDir, true)
			&& FFileHelper::SaveStringToFile(Json, *FPaths::Combine(ReportDir, TEXT("report.json")),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bSaved) Test.AddError(TEXT("Bake operation report could not be written."));
		Test.AddInfo(TEXT("G2 actual Bake operation report: ") + ReportDir);
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticG2BakeOperation success=%d dir=%s"),
			int(!bFailed && bSaved), *ReportDir);
		bFinished = true;
		return true;
	}

	bool DrainAndRestore()
	{
		if (!Volume.IsValid() || !Volume->IM_CancelAndDrainBakeForTest()) return false;
		Volume->BakedField = Original.Get();
		Volume->Status = OriginalStatus;
		Volume->GetPackage()->SetDirtyFlag(bOriginalDirty);
		return Volume->BakedField.Get() == Original.Get()
			&& Volume->GetPackage()->IsDirty() == bOriginalDirty;
	}

	FAutomationTestBase& Test;
	TStrongObjectPtr<AIMAcousticBakeVolume> Volume;
	TStrongObjectPtr<UIMAcousticBakeAsset> Original, NewAsset;
	FIMAcousticOperationMemorySampler Memory;
	TSharedRef<FJsonObject> Report;
	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<FVector4> ExpectedProbePreview;
	FString WorldPath, Fingerprint, OriginalStatus, RunId, ReportDir;
	int32 ExpectedProbes = 0, ExpectedTriangles = 0;
	uint64 BakeId = 0;
	double BakeStarted = 0.0;
	bool bOriginalDirty = false, bFailed = false, bCancelRequested = false, bFinished = false;
};

class FIMCurrentBakeOperationCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMCurrentBakeOperationCommand(TSharedRef<FIMCurrentBakeOperationState> InState)
		: State(MoveTemp(InState)) {}
	bool Update() override { return State->Update(); }
private:
	TSharedRef<FIMCurrentBakeOperationState> State;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2BakeOperationTest,
	"IceMoon.AcousticField.G2.CurrentBakeOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2BakeOperationTest::RunTest(const FString&)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("IMAcousticAllowEvidenceBake")))
	{
		AddError(TEXT("Actual bake measurement requires the explicit evidence-bake launch switch.")); return false;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || World->IsGameWorld() || GEditor->PlayWorld
		|| World->GetPackage()->GetName() != TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m"))
	{
		AddError(TEXT("Evidence bake requires the fixed rural 10m editor world without PIE.")); return false;
	}
	AIMAcousticBakeVolume* Volume = nullptr;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
	{
		if (Volume) { AddError(TEXT("Evidence world has more than one bake volume.")); return false; }
		Volume = *It;
	}
	FString Error;
	if (!Volume || !Volume->BakedField || !Volume->ValidateCurrentBake(Error))
	{
		AddError(TEXT("Evidence bake original binding is absent or stale: ") + Error); return false;
	}
	auto State = MakeShared<IMAcousticG2BakeOperationTestPrivate::FIMCurrentBakeOperationState>(*this, *Volume);
	if (!State->Start() || State->bFailed) return false;
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2BakeOperationTestPrivate::FIMCurrentBakeOperationCommand(State));
	return true;
}
#endif
