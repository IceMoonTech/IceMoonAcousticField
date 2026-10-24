#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AmbientSound.h"

namespace IMAcousticRuralPersistentInspectTestPrivate
{
constexpr const TCHAR* RuralTargetMap = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* SourceTagText = TEXT("IMAcousticRuralPreviewWaterDropV1");
constexpr const TCHAR* ExpectedMetaSoundPackage = TEXT("/IceMoonAcousticField/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay");

static FString GetMetaSoundOutputFormatName(const EMetaSoundOutputAudioFormat Format)
{
	switch (Format)
	{
	case EMetaSoundOutputAudioFormat::Mono: return TEXT("Mono");
	case EMetaSoundOutputAudioFormat::Stereo: return TEXT("Stereo");
	case EMetaSoundOutputAudioFormat::Quad: return TEXT("Quad");
	case EMetaSoundOutputAudioFormat::FiveDotOne: return TEXT("5.1");
	case EMetaSoundOutputAudioFormat::SevenDotOne: return TEXT("7.1");
	case EMetaSoundOutputAudioFormat::ChannelAgnostic: return TEXT("ChannelAgnostic");
	case EMetaSoundOutputAudioFormat::COUNT: return TEXT("COUNT");
	default: return TEXT("Unknown");
	}
}

static TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Strings.Num());
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralPersistentInspectTest,
	"IceMoon.AcousticField.Rural.InspectPersistentCamera3Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralPersistentInspectTest::RunTest(const FString&)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralPersistentInspect"), RunId);
	const FString ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
	const FName SourceTag(IMAcousticRuralPersistentInspectTestPrivate::SourceTagText);
	TArray<FString> Errors;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FString LoadedWorldPackage = World && World->GetOutermost()
		? World->GetOutermost()->GetName()
		: FString();
	const bool bEditorIdle = GEditor && GEditor->PlayWorld == nullptr;
	const bool bTargetWorldLoaded = World && LoadedWorldPackage == IMAcousticRuralPersistentInspectTestPrivate::RuralTargetMap;
	if (!GEditor) Errors.Add(TEXT("GEditor is unavailable."));
	if (!World) Errors.Add(TEXT("Editor world is unavailable."));
	if (GEditor && !bEditorIdle) Errors.Add(TEXT("Editor is running PIE; inspection requires the idle Editor world."));
	if (World && !bTargetWorldLoaded) Errors.Add(TEXT("Loaded Editor world does not match the requested Rural preview map."));

	TArray<AActor*> TaggedActors;
	TArray<AAmbientSound*> TaggedAmbientSounds;
	TArray<FString> TaggedActorPaths;
	if (World)
	{
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			AActor* Actor = *ActorIt;
			if (IsValid(Actor) && Actor->ActorHasTag(SourceTag))
			{
				TaggedActors.Add(Actor);
				TaggedActorPaths.Add(Actor->GetPathName());
				if (AAmbientSound* Ambient = Cast<AAmbientSound>(Actor))
				{
					TaggedAmbientSounds.Add(Ambient);
				}
			}
		}
	}
	TaggedActorPaths.Sort();
	TaggedAmbientSounds.Sort([](const AAmbientSound& A, const AAmbientSound& B)
	{
		return A.GetPathName() < B.GetPathName();
	});

	AAmbientSound* Ambient = TaggedAmbientSounds.IsEmpty() ? nullptr : TaggedAmbientSounds[0];
	TArray<UIMAcousticSourceComponent*> SourceMarkers;
	TArray<UAudioComponent*> AmbientAudioComponents;
	UAudioComponent* AmbientAudio = nullptr;
	if (IsValid(Ambient))
	{
		Ambient->GetComponents(SourceMarkers);
		Ambient->GetComponents(AmbientAudioComponents);
		SourceMarkers.Sort([](const UIMAcousticSourceComponent& A, const UIMAcousticSourceComponent& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		AmbientAudioComponents.Sort([](const UAudioComponent& A, const UAudioComponent& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		AmbientAudio = Ambient->GetAudioComponent();
	}

	UIMAcousticSourceComponent* Marker = SourceMarkers.IsEmpty() ? nullptr : SourceMarkers[0];
	UAudioComponent* MarkerAudio = IsValid(Marker) ? Marker->AudioComponent.Get() : nullptr;
	UAudioComponent* Audio = IsValid(MarkerAudio) ? MarkerAudio : AmbientAudio;
	const bool bMarkerOwnerMatches = IsValid(Marker) && Marker->GetOwner() == Ambient;
	const bool bMarkerWorldMatches = IsValid(Marker) && Marker->GetWorld() == World;
	const bool bAudioOwnerMatches = IsValid(Audio) && Audio->GetOwner() == Ambient;
	const bool bAudioWorldMatches = IsValid(Audio) && Audio->GetWorld() == World;
	const bool bMarkerAudioMatchesAmbientAudio = IsValid(MarkerAudio) && MarkerAudio == AmbientAudio;

	bool bValidateSourceCalled = false;
	bool bValidateSourceResult = false;
	FString ValidateSourceError;
	if (IsValid(Marker))
	{
		bValidateSourceCalled = true;
		bValidateSourceResult = Marker->ValidateSource(ValidateSourceError);
	}

	int32 DirectlyTaggedSourceMarkerCount = 0;
	int32 DirectlyTaggedAudioComponentCount = 0;
	TArray<FString> DirectlyTaggedSourceMarkerPaths;
	TArray<FString> DirectlyTaggedAudioComponentPaths;
	if (World)
	{
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			TArray<UIMAcousticSourceComponent*> Markers;
			(*ActorIt)->GetComponents(Markers);
			for (const UIMAcousticSourceComponent* Candidate : Markers)
			{
				if (IsValid(Candidate) && Candidate->ComponentHasTag(SourceTag))
				{
					++DirectlyTaggedSourceMarkerCount;
					DirectlyTaggedSourceMarkerPaths.Add(Candidate->GetPathName());
				}
			}
			TArray<UAudioComponent*> AudioComponents;
			(*ActorIt)->GetComponents(AudioComponents);
			for (const UAudioComponent* Candidate : AudioComponents)
			{
				if (IsValid(Candidate) && Candidate->ComponentHasTag(SourceTag))
				{
					++DirectlyTaggedAudioComponentCount;
					DirectlyTaggedAudioComponentPaths.Add(Candidate->GetPathName());
				}
			}
		}
	}

	DirectlyTaggedSourceMarkerPaths.Sort();
	DirectlyTaggedAudioComponentPaths.Sort();

	const UMetaSoundSource* MetaSound = IsValid(Audio) ? Cast<UMetaSoundSource>(Audio->Sound) : nullptr;
	const bool bAudioContractMatches = IsValid(Audio) && IsValid(MetaSound)
		&& MetaSound->GetOutermost()->GetName() == IMAcousticRuralPersistentInspectTestPrivate::ExpectedMetaSoundPackage
		&& MetaSound->OutputFormat == EMetaSoundOutputAudioFormat::Stereo && MetaSound->NumChannels == 2
		&& Audio->bAutoActivate && !Audio->bAutoDestroy && Audio->bStopWhenOwnerDestroyed
		&& !Audio->bAllowSpatialization && !Audio->bIsUISound && !Audio->bOverrideAttenuation
		&& FMath::IsNearlyEqual(Audio->VolumeMultiplier, 0.7f)
		&& FMath::IsNearlyEqual(Audio->PitchMultiplier, 1.0f);

	TArray<AIMAcousticBakeVolume*> BakeVolumes;
	AIMAcousticBakeVolume* BakeVolume = nullptr;
	TArray<FString> BakeVolumePaths;
	if (World)
	{
		for (TActorIterator<AIMAcousticBakeVolume> VolumeIt(World); VolumeIt; ++VolumeIt)
		{
			if (IsValid(*VolumeIt))
			{
				BakeVolumes.Add(*VolumeIt);
			}
		}
	}
	BakeVolumes.Sort([](const AIMAcousticBakeVolume& A, const AIMAcousticBakeVolume& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	const int32 BakeVolumeCount = BakeVolumes.Num();
	BakeVolume = BakeVolumes.IsEmpty() ? nullptr : BakeVolumes[0];
	const FString BakeVolumeMaterialDrive = !IsValid(BakeVolume)
		? TEXT("unavailable")
		: BakeVolume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial
			? TEXT("physical_material")
			: TEXT("legacy_render_material");
	for (const AIMAcousticBakeVolume* Candidate : BakeVolumes)
	{
		BakeVolumePaths.Add(Candidate->GetPathName());
	}
	BakeVolumePaths.Sort();

	bool bBoundBakeAssetReadable = false;
	FString BoundBakeAssetPath;
	FString BoundBakeWorldPackage;
	int32 GeneratedProbeCountBeforeValidation = -1;
	int32 GeneratedProbeCountAfterValidation = -1;
	bool bValidateCurrentBakeCalled = false;
	bool bValidateCurrentBakeResult = false;
	FString ValidateCurrentBakeError;
	bool bVisibleBakeVolumeStateRestored = false;
	if (BakeVolumeCount == 1 && IsValid(BakeVolume))
	{
		GeneratedProbeCountBeforeValidation = BakeVolume->GeneratedProbes;
		if (IsValid(BakeVolume->BakedField))
		{
			bBoundBakeAssetReadable = true;
			BoundBakeAssetPath = BakeVolume->BakedField->GetPathName();
			BoundBakeWorldPackage = BakeVolume->BakedField->WorldPackage;

			// ValidateCurrentBake recaptures geometry and refreshes editor-visible fields.
			// Preserve those serialized fields after recording the result; no Bake or Save is performed.
			const FString PreviousStatus = BakeVolume->Status;
			const TArray<FString> PreviousSceneIssues = BakeVolume->SceneIssues;
			const int32 PreviousExportedTriangles = BakeVolume->ExportedTriangles;
			const int32 PreviousGeneratedProbes = BakeVolume->GeneratedProbes;
			const EIMAcousticMaterialDrive PreviousMaterialDrive = BakeVolume->MaterialDrive;
			bValidateCurrentBakeCalled = true;
			bValidateCurrentBakeResult = BakeVolume->ValidateCurrentBake(ValidateCurrentBakeError);
			GeneratedProbeCountAfterValidation = BakeVolume->GeneratedProbes;
			BakeVolume->Status = PreviousStatus;
			BakeVolume->SceneIssues = PreviousSceneIssues;
			BakeVolume->ExportedTriangles = PreviousExportedTriangles;
			BakeVolume->GeneratedProbes = PreviousGeneratedProbes;
			bVisibleBakeVolumeStateRestored = BakeVolume->Status == PreviousStatus
				&& BakeVolume->SceneIssues == PreviousSceneIssues
				&& BakeVolume->ExportedTriangles == PreviousExportedTriangles
				&& BakeVolume->GeneratedProbes == PreviousGeneratedProbes
				&& BakeVolume->MaterialDrive == PreviousMaterialDrive;
		}
		else
		{
			ValidateCurrentBakeError = TEXT("Not called because the BakeVolume has no valid bound BakedField.");
		}
	}
	else
	{
		ValidateCurrentBakeError = TEXT("Not called because a unique BakeVolume could not be resolved.");
	}

	const bool bAudioBindingReadable = IsValid(Ambient) && SourceMarkers.Num() == 1 && IsValid(Marker)
		&& bMarkerOwnerMatches && bMarkerWorldMatches && IsValid(MarkerAudio) && bMarkerAudioMatchesAmbientAudio
		&& bAudioOwnerMatches && bAudioWorldMatches;
	const bool bStructureReadable = bTargetWorldLoaded && bEditorIdle
		&& TaggedActors.Num() == 1 && TaggedAmbientSounds.Num() == 1
		&& bAudioBindingReadable && BakeVolumeCount == 1 && bBoundBakeAssetReadable;
	if (TaggedActors.Num() != 1) Errors.Add(FString::Printf(TEXT("Expected one actor with the source tag; found %d."), TaggedActors.Num()));
	if (TaggedAmbientSounds.Num() != 1) Errors.Add(FString::Printf(TEXT("Expected one tagged AAmbientSound; found %d."), TaggedAmbientSounds.Num()));
	if (IsValid(Ambient) && SourceMarkers.Num() != 1) Errors.Add(FString::Printf(TEXT("Expected one source marker on the tagged AAmbientSound; found %d."), SourceMarkers.Num()));
	if (IsValid(Ambient) && !bAudioBindingReadable) Errors.Add(TEXT("The marker-to-AudioComponent ownership or world binding could not be read consistently."));
	if (BakeVolumeCount != 1) Errors.Add(FString::Printf(TEXT("Expected one BakeVolume; found %d."), BakeVolumeCount));
	if (BakeVolumeCount == 1 && !bBoundBakeAssetReadable) Errors.Add(TEXT("The unique BakeVolume has no valid bound BakedField."));
	if (!bAudioContractMatches) Errors.Add(TEXT("The saved water-drop AudioComponent differs from the explicit stereo/autoplay/native-processing contract."));
	if (bValidateCurrentBakeCalled && (!bValidateCurrentBakeResult || BoundBakeWorldPackage != IMAcousticRuralPersistentInspectTestPrivate::RuralTargetMap))
		Errors.Add(TEXT("The persisted Bake binding is stale or belongs to another world."));
	if (bValidateCurrentBakeCalled && !bVisibleBakeVolumeStateRestored)
		Errors.Add(TEXT("ValidateCurrentBake changed editor-visible BakeVolume fields that the test could not restore."));

	TSharedRef<FJsonObject> SourceObject = MakeShared<FJsonObject>();
	SourceObject->SetNumberField(TEXT("tagged_actor_count"), TaggedActors.Num());
	SourceObject->SetArrayField(TEXT("tagged_actor_paths"), IMAcousticRuralPersistentInspectTestPrivate::ToJsonStringArray(TaggedActorPaths));
	SourceObject->SetNumberField(TEXT("tagged_ambient_sound_count"), TaggedAmbientSounds.Num());
	SourceObject->SetStringField(TEXT("ambient_sound_path"), GetPathNameSafe(Ambient));
	SourceObject->SetStringField(TEXT("ambient_sound_label"), IsValid(Ambient) ? Ambient->GetActorLabel() : FString());
	SourceObject->SetBoolField(TEXT("ambient_sound_has_source_tag"), IsValid(Ambient) && Ambient->ActorHasTag(SourceTag));
	SourceObject->SetNumberField(TEXT("source_marker_count_on_ambient_sound"), SourceMarkers.Num());
	SourceObject->SetStringField(TEXT("source_marker_path"), GetPathNameSafe(Marker));
	SourceObject->SetBoolField(TEXT("source_marker_has_direct_source_tag"), IsValid(Marker) && Marker->ComponentHasTag(SourceTag));
	SourceObject->SetStringField(TEXT("marker_audio_component_path"), GetPathNameSafe(MarkerAudio));
	SourceObject->SetBoolField(TEXT("marker_audio_matches_ambient_audio_component"), bMarkerAudioMatchesAmbientAudio);
	SourceObject->SetBoolField(TEXT("marker_owner_matches_tagged_ambient_sound"), bMarkerOwnerMatches);
	SourceObject->SetBoolField(TEXT("marker_world_matches_editor_world"), bMarkerWorldMatches);
	SourceObject->SetBoolField(TEXT("validate_source_called"), bValidateSourceCalled);
	SourceObject->SetBoolField(TEXT("validate_source_result"), bValidateSourceResult);
	SourceObject->SetStringField(TEXT("validate_source_error"), ValidateSourceError);

	TSharedRef<FJsonObject> AudioObject = MakeShared<FJsonObject>();
	AudioObject->SetNumberField(TEXT("audio_component_count_on_ambient_sound"), AmbientAudioComponents.Num());
	AudioObject->SetStringField(TEXT("audio_component_path"), GetPathNameSafe(Audio));
	AudioObject->SetBoolField(TEXT("audio_component_has_direct_source_tag"), IsValid(Audio) && Audio->ComponentHasTag(SourceTag));
	AudioObject->SetBoolField(TEXT("audio_component_owner_matches_tagged_ambient_sound"), bAudioOwnerMatches);
	AudioObject->SetBoolField(TEXT("audio_component_world_matches_editor_world"), bAudioWorldMatches);
	AudioObject->SetStringField(TEXT("sound_path"), IsValid(Audio) ? GetPathNameSafe(Audio->Sound) : FString());
	AudioObject->SetBoolField(TEXT("metasound_found"), IsValid(MetaSound));
	AudioObject->SetBoolField(TEXT("expected_audio_contract_matches"), bAudioContractMatches);
	AudioObject->SetStringField(TEXT("metasound_path"), GetPathNameSafe(MetaSound));
	AudioObject->SetStringField(TEXT("metasound_output_format"), IsValid(MetaSound)
		? IMAcousticRuralPersistentInspectTestPrivate::GetMetaSoundOutputFormatName(MetaSound->OutputFormat)
		: TEXT("not_metasound"));
	AudioObject->SetNumberField(TEXT("metasound_output_format_value"), IsValid(MetaSound)
		? static_cast<int32>(MetaSound->OutputFormat)
		: -1);
	AudioObject->SetNumberField(TEXT("metasound_num_channels"), IsValid(MetaSound) ? MetaSound->NumChannels : -1);
	AudioObject->SetBoolField(TEXT("bAutoActivate"), IsValid(Audio) && Audio->bAutoActivate);
	AudioObject->SetBoolField(TEXT("bAutoDestroy"), IsValid(Audio) && Audio->bAutoDestroy);
	AudioObject->SetBoolField(TEXT("bStopWhenOwnerDestroyed"), IsValid(Audio) && Audio->bStopWhenOwnerDestroyed);
	AudioObject->SetBoolField(TEXT("bAllowSpatialization"), IsValid(Audio) && Audio->bAllowSpatialization);
	AudioObject->SetBoolField(TEXT("bIsUISound"), IsValid(Audio) && Audio->bIsUISound);
	AudioObject->SetBoolField(TEXT("bOverrideAttenuation"), IsValid(Audio) && Audio->bOverrideAttenuation);
	AudioObject->SetNumberField(TEXT("VolumeMultiplier"), IsValid(Audio) ? Audio->VolumeMultiplier : -1.0);
	AudioObject->SetNumberField(TEXT("PitchMultiplier"), IsValid(Audio) ? Audio->PitchMultiplier : -1.0);

	TSharedRef<FJsonObject> DirectComponentTagsObject = MakeShared<FJsonObject>();
	DirectComponentTagsObject->SetNumberField(TEXT("source_marker_count"), DirectlyTaggedSourceMarkerCount);
	DirectComponentTagsObject->SetArrayField(TEXT("source_marker_paths"), IMAcousticRuralPersistentInspectTestPrivate::ToJsonStringArray(DirectlyTaggedSourceMarkerPaths));
	DirectComponentTagsObject->SetNumberField(TEXT("audio_component_count"), DirectlyTaggedAudioComponentCount);
	DirectComponentTagsObject->SetArrayField(TEXT("audio_component_paths"), IMAcousticRuralPersistentInspectTestPrivate::ToJsonStringArray(DirectlyTaggedAudioComponentPaths));

	TSharedRef<FJsonObject> BakeObject = MakeShared<FJsonObject>();
	BakeObject->SetNumberField(TEXT("bake_volume_count"), BakeVolumeCount);
	BakeObject->SetArrayField(TEXT("bake_volume_paths"), IMAcousticRuralPersistentInspectTestPrivate::ToJsonStringArray(BakeVolumePaths));
	BakeObject->SetStringField(TEXT("bake_volume_path"), GetPathNameSafe(BakeVolume));
	BakeObject->SetStringField(TEXT("material_drive"), BakeVolumeMaterialDrive);
	BakeObject->SetBoolField(TEXT("bound_bake_asset_readable"), bBoundBakeAssetReadable);
	BakeObject->SetStringField(TEXT("bound_bake_asset_path"), BoundBakeAssetPath);
	BakeObject->SetStringField(TEXT("bound_world_package"), BoundBakeWorldPackage);
	BakeObject->SetNumberField(TEXT("generated_probe_count_before_validation"), GeneratedProbeCountBeforeValidation);
	BakeObject->SetNumberField(TEXT("generated_probe_count_after_validation_before_restore"), GeneratedProbeCountAfterValidation);
	BakeObject->SetBoolField(TEXT("validate_current_bake_called"), bValidateCurrentBakeCalled);
	BakeObject->SetBoolField(TEXT("validate_current_bake_result"), bValidateCurrentBakeResult);
	BakeObject->SetStringField(TEXT("validate_current_bake_error"), ValidateCurrentBakeError);
	BakeObject->SetBoolField(TEXT("visible_bake_volume_state_restored"), bVisibleBakeVolumeStateRestored);
	BakeObject->SetBoolField(TEXT("validate_current_bake_may_refresh_runtime_capture"), true);

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("im_acoustic_rural_persistent_inspect_v1"));
	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetStringField(TEXT("target_map_package"), IMAcousticRuralPersistentInspectTestPrivate::RuralTargetMap);
	Report->SetStringField(TEXT("loaded_world_package"), LoadedWorldPackage);
	Report->SetBoolField(TEXT("editor_world_available"), World != nullptr);
	Report->SetBoolField(TEXT("editor_idle_no_pie"), bEditorIdle);
	Report->SetBoolField(TEXT("target_world_loaded"), bTargetWorldLoaded);
	Report->SetBoolField(TEXT("asset_or_map_save_performed"), false);
	Report->SetBoolField(TEXT("bake_requested"), false);
	Report->SetStringField(TEXT("source_tag"), IMAcousticRuralPersistentInspectTestPrivate::SourceTagText);
	Report->SetObjectField(TEXT("source"), SourceObject);
	Report->SetObjectField(TEXT("audio"), AudioObject);
	Report->SetObjectField(TEXT("direct_component_tags"), DirectComponentTagsObject);
	Report->SetObjectField(TEXT("bake_volume"), BakeObject);
	Report->SetArrayField(TEXT("failure_reasons"), IMAcousticRuralPersistentInspectTestPrivate::ToJsonStringArray(Errors));
	const bool bInspectionComplete = bStructureReadable && Errors.IsEmpty();
	Report->SetStringField(TEXT("inspection_result"), bInspectionComplete ? TEXT("PASS") : TEXT("FAIL"));
	Report->SetStringField(TEXT("report_path"), ReportPath);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
	Writer->Close();
	const bool bReportSaved = bSerialized
		&& IFileManager::Get().MakeDirectory(*ReportDirectory, true)
		&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!bReportSaved)
	{
		Errors.Add(FString::Printf(TEXT("Could not create and write the required inspection report: %s"), *ReportPath));
		AddError(TEXT("RuralPersistentInspect report creation or write failed."));
	}
	if (!bInspectionComplete)
	{
		AddError(FString::Join(Errors, TEXT(" ")));
	}

	const bool bPass = bInspectionComplete && bReportSaved;
	UE_LOG(LogTemp, Display, TEXT("IMLogs RuralPersistentInspect result=%s source_markers=%d audio=%s bake_volume_count=%d validate_current_bake=%d report=%s"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), SourceMarkers.Num(), *GetPathNameSafe(Audio), BakeVolumeCount,
		bValidateCurrentBakeCalled && bValidateCurrentBakeResult ? 1 : 0, *ReportPath);
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralPersistentInspect %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
	if (bReportSaved)
	{
		AddInfo(FString::Printf(TEXT("RuralPersistentInspect report: %s"), *ReportPath));
	}
	return bPass;
}

#endif
