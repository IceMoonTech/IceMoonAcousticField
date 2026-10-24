#if WITH_EDITOR
// Footstep-in-V2-audition tooling: console commands the capture script drives, and one generic automation stage that opens a
// map and runs a Python script from the evidence folder. The stage keeps C++ rebuilds out of the scene/capture iteration loop.
#include "IMAcousticFootstepCharacter.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSDKContext.h"
#include "Tests/IMAcousticAuditionCapture.h"
#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <phonon.h>

namespace IMAcousticFootstepV2StagePrivate
{
// Hard ceiling of one stage run; the wrapper's own process timeout is the outer bound.
constexpr double IMStageDefaultTimeoutSeconds = 3000.;

FString EvidencePath(const TCHAR* Name)
{
	return FPaths::Combine(IMAcousticAuditionCapture::OutputDirectory(), Name);
}

bool SaveJson(const TSharedRef<FJsonObject>& Object, const FString& File)
{
	FString Text;
	return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FAutoConsoleCommandWithWorldAndArgs VideoCommand(TEXT("IM.AcousticField.VideoCapture"),
	TEXT("start <dir> <width> <height> <fps> <fovDegrees> <ev100> | stop: fixed-step PNG frames of the replay, one per game tick."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.IsEmpty()) return;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor)
		{
			if (Arguments[0] == TEXT("stop")) Actor->StopVideoCapture();
			else if (Arguments[0] == TEXT("start") && Arguments.Num() == 7)
			{
				const bool bStarted = Actor->StartVideoCapture(Arguments[1], FCString::Atoi(*Arguments[2]), FCString::Atoi(*Arguments[3]),
					FCString::Atof(*Arguments[4]), FCString::Atof(*Arguments[5]), FCString::Atof(*Arguments[6]));
				if (!bStarted) UE_LOG(LogTemp, Error, TEXT("IMLogs FootstepVideo start_rejected"));
			}
		}
	}));

FAutoConsoleCommandWithWorldAndArgs CompensationCommand(TEXT("IM.AcousticField.PresenceCompensation"),
	TEXT("<gain>: equal-loudness gain for this session instead of the calibration file; 0 restores the file."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.Num() != 1) return;
		for (TActorIterator<AIMAcousticFootstepCharacter> Actor(World); Actor; ++Actor)
			Actor->SetPresenceCompensationOverride(FCString::Atof(*Arguments[0]));
	}));

// Reads back what every bake volume of the world holds and whether the bake still matches the scene (the same check the runtime does).
FAutoConsoleCommandWithWorldAndArgs ValidateBakeCommand(TEXT("IM.AcousticField.ValidateBake"),
	TEXT("<file>: write the bake volumes of this world (bounds, material drive, probe/triangle counts, ValidateCurrentBake result) as JSON."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.Num() != 1) return;
		auto Report = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
		{
			FString Failure;
			const bool bValid = It->ValidateCurrentBake(Failure);
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("actor"), It->GetPathName());
			Row->SetBoolField(TEXT("valid"), bValid);
			Row->SetStringField(TEXT("failure"), Failure);
			Row->SetStringField(TEXT("status"), It->Status);
			Row->SetStringField(TEXT("bake_asset"), GetPathNameSafe(It->BakedField.Get()));
			Row->SetNumberField(TEXT("generated_probes"), It->GeneratedProbes);
			Row->SetNumberField(TEXT("exported_triangles"), It->ExportedTriangles);
			Row->SetNumberField(TEXT("probe_spacing_cm"), It->ProbeSpacingCm);
			Row->SetNumberField(TEXT("probe_height_cm"), It->ProbeHeightCm);
			Row->SetStringField(TEXT("material_drive"), It->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial ? TEXT("PhysicalMaterial") : TEXT("LegacyRenderMaterial"));
			Row->SetNumberField(TEXT("physical_material_profiles"), It->PhysicalMaterials.Num());
			Row->SetNumberField(TEXT("render_material_profiles"), It->Materials.Num());
			Row->SetNumberField(TEXT("non_solid_triangles"), It->NonSolidTriangles);
			if (It->BakeBounds)
			{
				const FBox Box = It->BakeBounds->Bounds.GetBox();
				Row->SetStringField(TEXT("bounds_min_cm"), Box.Min.ToString());
				Row->SetStringField(TEXT("bounds_max_cm"), Box.Max.ToString());
			}
			if (It->BakedField)
			{
				Row->SetStringField(TEXT("bake_world_package"), It->BakedField->WorldPackage);
				Row->SetStringField(TEXT("bake_scene_fingerprint"), It->BakedField->SceneFingerprint);
			}
			TArray<TSharedPtr<FJsonValue>> Issues;
			for (const FString& Issue : It->SceneIssues) Issues.Add(MakeShared<FJsonValueString>(Issue));
			Row->SetArrayField(TEXT("scene_issues"), Issues);
			Row->SetBoolField(TEXT("v2"), It->bEnableV2);
			Row->SetBoolField(TEXT("direct_route"), It->bDirectRoute);
			Row->SetBoolField(TEXT("path_route"), It->bPathRoute);
			Row->SetBoolField(TEXT("reverb_route"), It->bReverbRoute);
			Row->SetNumberField(TEXT("reverb_wet_gain"), It->ReverbWetGain);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Report->SetArrayField(TEXT("volumes"), Rows);
		Report->SetStringField(TEXT("world"), World->GetPathName());
		if (!SaveJson(Report, Arguments[0])) UE_LOG(LogTemp, Error, TEXT("IMLogs ValidateBake could not write %s"), *Arguments[0]);
	}));

// Reads the baked reverb layer of the volume (energy field per probe, 10 ms bins, 3 bands, Ambisonic channels) straight from the probe batch.
// Writes per-probe band/channel energy statistics as JSON and, for the selected probes, the impulse response the SDK reconstructs from that
// energy field (the reconstruction the runtime convolution uses) as float32 [channel][sample].
bool DumpReverb(const AIMAcousticBakeVolume& Volume, const FString& Base, const TArray<int32>& Selected, FString& Failure)
{
	const UIMAcousticBakeAsset* Asset = Volume.BakedField.Get();
	if (!Asset) { Failure = TEXT("volume has no bake"); return false; }
	TArray<uint8> Scene, Probes;
	if (!Asset->ReadPayload(Asset->WorldPackage, Asset->SceneFingerprint, Scene, Probes, Failure)) return false;
	TArray<FVector4> Preview;
	FVector OriginCm;
	if (!Asset->GetProbePreview(Asset->WorldPackage, Asset->SceneFingerprint, Preview, OriginCm, Failure)) return false;
	IPLContext Context = IMAcousticSDKContext::GetAcousticSDKContext();
	if (!Context) { Failure = TEXT("no SDK context"); return false; }
	IPLSerializedObject Object = nullptr;
	IPLSerializedObjectSettings ObjectSettings{};
	ObjectSettings.data = Probes.GetData();
	ObjectSettings.size = size_t(Probes.Num());
	IPLProbeBatch Batch = nullptr;
	if (iplSerializedObjectCreate(Context, &ObjectSettings, &Object) != IPL_STATUS_SUCCESS
		|| iplProbeBatchLoad(Context, Object, &Batch) != IPL_STATUS_SUCCESS) { Failure = TEXT("probe batch load failed"); return false; }
	IPLBakedDataIdentifier Id{};
	Id.type = IPL_BAKEDDATATYPE_REFLECTIONS;
	Id.variation = IPL_BAKEDDATAVARIATION_REVERB;
	constexpr float DurationSeconds = 2.f;
	constexpr int32 SampleRate = 48000;
	IPLEnergyFieldSettings FieldSettings{DurationSeconds, 1};
	IPLEnergyField Field = nullptr;
	IPLImpulseResponseSettings ResponseSettings{DurationSeconds, 1, SampleRate};
	IPLImpulseResponse Response = nullptr;
	IPLReconstructorSettings ReconstructorSettings{DurationSeconds, 1, SampleRate};
	IPLReconstructor Reconstructor = nullptr;
	if (iplEnergyFieldCreate(Context, &FieldSettings, &Field) != IPL_STATUS_SUCCESS
		|| iplImpulseResponseCreate(Context, &ResponseSettings, &Response) != IPL_STATUS_SUCCESS
		|| iplReconstructorCreate(Context, &ReconstructorSettings, &Reconstructor) != IPL_STATUS_SUCCESS) { Failure = TEXT("SDK object create failed"); return false; }
	const int32 ProbeCount = iplProbeBatchGetNumProbes(Batch);
	const int32 Channels = iplEnergyFieldGetNumChannels(Field), Bins = iplEnergyFieldGetNumBins(Field);
	// Windows over the 10 ms bins: 0-100 ms, 100-400 ms, 400 ms-end.
	const int32 WindowEnds[3] = {10, 40, Bins};
	auto Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("probe_count"), ProbeCount);
	Report->SetNumberField(TEXT("preview_probes"), Preview.Num());
	Report->SetNumberField(TEXT("channels"), Channels);
	Report->SetNumberField(TEXT("bins"), Bins);
	Report->SetStringField(TEXT("origin_cm"), OriginCm.ToString());
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (int32 Probe = 0; Probe < ProbeCount; ++Probe)
	{
		iplProbeBatchGetEnergyField(Batch, &Id, Probe, Field);
		const float* Data = iplEnergyFieldGetData(Field);
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), Probe);
		if (Preview.IsValidIndex(Probe))
		{
			TArray<TSharedPtr<FJsonValue>> Position;
			for (int32 Axis = 0; Axis < 4; ++Axis) Position.Add(MakeShared<FJsonValueNumber>(Preview[Probe][Axis]));
			Row->SetArrayField(TEXT("probe_m"), Position);
		}
		// energy[channel][band] summed over all bins, and the W channel per band in the three windows.
		TArray<TSharedPtr<FJsonValue>> Total, Windows;
		for (int32 Channel = 0; Channel < Channels; ++Channel)
			for (int32 Band = 0; Band < IPL_NUM_BANDS; ++Band)
			{
				double Sum = 0.;
				for (int32 Bin = 0; Bin < Bins; ++Bin) Sum += Data[(Channel * IPL_NUM_BANDS + Band) * Bins + Bin];
				Total.Add(MakeShared<FJsonValueNumber>(Sum));
			}
		for (int32 Band = 0; Band < IPL_NUM_BANDS; ++Band)
		{
			int32 Begin = 0;
			for (int32 Window = 0; Window < 3; ++Window)
			{
				double Sum = 0.;
				for (int32 Bin = Begin; Bin < WindowEnds[Window]; ++Bin) Sum += Data[Band * Bins + Bin];
				Windows.Add(MakeShared<FJsonValueNumber>(Sum));
				Begin = WindowEnds[Window];
			}
		}
		Row->SetArrayField(TEXT("energy_channel_band"), Total);
		Row->SetArrayField(TEXT("w_band_window"), Windows);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		if (!Selected.Contains(Probe)) continue;
		IPLReconstructorInputs Input{Field};
		IPLReconstructorSharedInputs Shared{DurationSeconds, 1};
		IPLReconstructorOutputs Output{Response};
		iplReconstructorReconstruct(Reconstructor, 1, &Input, &Shared, &Output);
		TArray<float> Samples;
		const int32 ResponseChannels = iplImpulseResponseGetNumChannels(Response), ResponseSamples = iplImpulseResponseGetNumSamples(Response);
		for (int32 Channel = 0; Channel < ResponseChannels; ++Channel)
			Samples.Append(iplImpulseResponseGetChannel(Response, Channel), ResponseSamples);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Samples.GetData()), Samples.Num() * sizeof(float));
		FFileHelper::SaveArrayToFile(Bytes, *FString::Printf(TEXT("%s_ir_%d.f32"), *Base, Probe));
	}
	Report->SetArrayField(TEXT("probes"), Rows);
	iplReconstructorRelease(&Reconstructor);
	iplImpulseResponseRelease(&Response);
	iplEnergyFieldRelease(&Field);
	iplProbeBatchRelease(&Batch);
	iplSerializedObjectRelease(&Object);
	return SaveJson(Report, Base + TEXT(".json"));
}

FAutoConsoleCommandWithWorldAndArgs ReverbDumpCommand(TEXT("IM.AcousticField.ReverbDump"),
	TEXT("<basePath> [probeIndex...]: baked reverb energy statistics of every probe (<base>.json) and the reconstructed IR of the listed probes (<base>_ir_<n>.f32)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		if (!World || Arguments.IsEmpty()) return;
		TArray<int32> Selected;
		for (int32 Index = 1; Index < Arguments.Num(); ++Index) Selected.Add(FCString::Atoi(*Arguments[Index]));
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
		{
			FString Failure;
			if (!DumpReverb(**It, Arguments[0], Selected, Failure))
			{
				UE_LOG(LogTemp, Error, TEXT("IMLogs ReverbDump failed: %s"), *Failure);
			}
			else
			{
				UE_LOG(LogTemp, Display, TEXT("IMLogs ReverbDump wrote %s"), *Arguments[0]);
			}
			break;
		}
	}));

// IMStageControl.json is either one stage {map, script, receipt, timeout_seconds} or {"stages": [ ... ]}. With a list, each automation run
// takes the next entry, counted in IMStageCursor.txt (the editor restarts between runs, so the position lives in the evidence folder).
// That lets one queued job run import, rebake and capture as separate editor sessions without a new control file in between.
TSharedPtr<FJsonObject> SelectStage(FString& Failure)
{
	FString Text;
	TSharedPtr<FJsonObject> Control;
	if (!FFileHelper::LoadFileToString(Text, *EvidencePath(TEXT("IMStageControl.json")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Control) || !Control.IsValid())
	{
		Failure = TEXT("IMStageControl.json is missing or invalid.");
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
	if (!Control->TryGetArrayField(TEXT("stages"), Stages)) return Control;
	const FString CursorFile = EvidencePath(TEXT("IMStageCursor.txt"));
	FString CursorText;
	FFileHelper::LoadFileToString(CursorText, *CursorFile);
	const int32 Cursor = FCString::Atoi(*CursorText);
	if (!Stages->IsValidIndex(Cursor))
	{
		Failure = FString::Printf(TEXT("IMStageControl.json has %d stages; run %d has none."), Stages->Num(), Cursor);
		return nullptr;
	}
	FFileHelper::SaveStringToFile(FString::FromInt(Cursor + 1), *CursorFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return (*Stages)[Cursor]->AsObject();
}

struct FIMStageSelection
{
	TSharedPtr<FJsonObject> Stage;
};

// Opens the map of the selected stage, dispatches its script, then waits for the receipt file the script writes.
class FIMFootstepStageStart final : public IAutomationLatentCommand
{
public:
	FIMFootstepStageStart(FAutomationTestBase* InTest, TSharedRef<FIMStageSelection> InSelection) : Test(InTest), Selection(InSelection) {}
	bool Update() override
	{
		FString Failure;
		Selection->Stage = SelectStage(Failure);
		if (!Selection->Stage.IsValid())
		{
			Test->AddError(Failure);
			return true;
		}
		const FString Receipt = Selection->Stage->GetStringField(TEXT("receipt"));
		// A receipt left by an earlier stage must not satisfy this one.
		IFileManager::Get().Delete(*Receipt, false, true, true);
		const FString Map = Selection->Stage->GetStringField(TEXT("map"));
		if (!Map.IsEmpty() && !FEditorFileUtils::LoadMap(Map, false, true))
		{
			Test->AddError(TEXT("Stage map did not load: ") + Map);
			Selection->Stage.Reset();
			return true;
		}
		const FString Command = FString::Printf(TEXT("py %s"), *Selection->Stage->GetStringField(TEXT("script")));
		Test->TestTrue(TEXT("Stage script dispatched"), GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command));
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FIMStageSelection> Selection;
};

class FIMFootstepStageAwait final : public IAutomationLatentCommand
{
public:
	FIMFootstepStageAwait(FAutomationTestBase* InTest, TSharedRef<FIMStageSelection> InSelection) : Test(InTest), Selection(InSelection), Started(FPlatformTime::Seconds()) {}
	bool Update() override
	{
		if (!Selection->Stage.IsValid()) return true;
		const TSharedPtr<FJsonObject>& Stage = Selection->Stage;
		const double Timeout = Stage->HasTypedField<EJson::Number>(TEXT("timeout_seconds")) ? Stage->GetNumberField(TEXT("timeout_seconds")) : IMStageDefaultTimeoutSeconds;
		const FString Receipt = Stage->GetStringField(TEXT("receipt"));
		if (!FPaths::FileExists(Receipt))
		{
			if (FPlatformTime::Seconds() - Started > Timeout) Test->AddError(TEXT("Stage receipt did not appear before the stage timeout."));
			else return false;
			return true;
		}
		FString Text;
		TSharedPtr<FJsonObject> Report;
		const bool bParsed = FFileHelper::LoadFileToString(Text, *Receipt)
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Report) && Report.IsValid();
		Test->TestTrue(TEXT("Stage receipt is valid"), bParsed);
		if (bParsed) Test->TestTrue(TEXT("Stage reports success"), Report->GetBoolField(TEXT("success")));
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FIMStageSelection> Selection;
	double Started;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticFootstepV2StageTest, "IceMoon.AcousticField.FootstepV2.Stage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticFootstepV2StageTest::RunTest(const FString&)
{
	using namespace IMAcousticFootstepV2StagePrivate;
	const TSharedRef<FIMStageSelection> Selection = MakeShared<FIMStageSelection>();
	ADD_LATENT_AUTOMATION_COMMAND(FIMFootstepStageStart(this, Selection));
	ADD_LATENT_AUTOMATION_COMMAND(FIMFootstepStageAwait(this, Selection));
	return true;
}
#endif
