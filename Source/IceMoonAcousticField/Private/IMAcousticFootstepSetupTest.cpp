#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticFootstepCharacter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetCompilingManager.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/SoundFactory.h"
#include "FileHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace IMAcousticFootstepSetupTestPrivate
{
constexpr const TCHAR* IMMap = TEXT("/IceMoonAcousticField/Tests/IM_FootstepAudition");
constexpr const TCHAR* IMGraph = TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/MS_IMFootstep");
constexpr const TCHAR* IMPMFolder = TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/");
FString ObjectPath(const FString& Package) { return Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package); }
FString IMEvidence() { return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/GameFootstepR89_20261007")); }

bool IMSaveAsset(UObject* Asset)
{
	if (!Asset) return false;
	UPackage* Package = Asset->GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	Asset->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	return UPackage::SavePackage(Package, Asset, *File, Args);
}

USoundWave* IMImportStep(const TCHAR* Clip)
{
	const FString Name = TEXT("SW_IMFootstep_") + FString(Clip);
	const FString PackageName = TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/") + Name;
	if (auto* Existing = LoadObject<USoundWave>(nullptr, *ObjectPath(PackageName))) return Existing;
	const FString File = FPaths::Combine(IMEvidence(), TEXT("materials"), FString(Clip) + TEXT(".wav"));
	if (!IFileManager::Get().FileExists(*File)) return nullptr;
	UPackage* Package = CreatePackage(*PackageName);
	auto* Factory = NewObject<USoundFactory>();
	auto* Wave = Cast<USoundWave>(UFactory::StaticImportObject(USoundWave::StaticClass(), Package,
		FName(Name), RF_Public | RF_Standalone, *File, nullptr, Factory));
	if (!Wave || Wave->NumChannels != 1 || Wave->Duration <= 0 || Wave->Duration > 1.f) return nullptr;
	FAssetRegistryModule::AssetCreated(Wave);
	return IMSaveAsset(Wave) ? Wave : nullptr;
}

bool IMEnsureFootstepExecutionSettings(UMetaSoundSource* Source, FString& Error)
{
	if (!Source) { Error = TEXT("Footstep graph source is missing."); return false; }
	const auto Before = Source->GetOperatorSettings(48000);
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo graph_settings_before sample_override=%d block_override=%.6f resolved_rate=%d frames=%d"),
		Source->SampleRateOverride.Default, Source->BlockRateOverride.Default, int32(Before.GetSampleRate()), Before.GetNumFramesPerBlock());
	if (Before.GetSampleRate() == 48000 && Before.GetNumFramesPerBlock() == 512) return true;
	// BuildToAsset copies the frontend document, not transient builder UObject properties.
	// Apply the existing fixed acoustic ABI to this saved source; preserve every node/edge/ID.
	Source->Modify();
	Source->SampleRateOverride.Default = 48000;
	Source->BlockRateOverride.Default = 48000.f / 512.f;
	Source->PostEditChange();
	const auto After = Source->GetOperatorSettings(48000);
	if (After.GetSampleRate() != 48000 || After.GetNumFramesPerBlock() != 512 || !IMSaveAsset(Source))
	{ Error = TEXT("Saved footstep source execution settings did not reach the 48k/512 contract."); return false; }
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo graph_settings_saved rate=%d frames=%d"), int32(After.GetSampleRate()), After.GetNumFramesPerBlock());
	return true;
}

UMetaSoundSource* IMCreateFootstepGraph(USoundWave* InitialWave, FString& Error)
{
	if (auto* Existing = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath(IMGraph))) return Existing;
	auto* Subsystem = GEngine->GetEngineSubsystem<UMetaSoundBuilderSubsystem>();
	auto* EditorSubsystem = GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>();
	if (!Subsystem || !EditorSubsystem) { Error = TEXT("MetaSound builder subsystems unavailable."); return nullptr; }
	EMetaSoundBuilderResult Result;
	FMetaSoundBuilderNodeOutputHandle OnPlay;
	FMetaSoundBuilderNodeInputHandle OnFinished;
	TArray<FMetaSoundBuilderNodeInputHandle> Outputs;
	auto* Builder = Subsystem->CreateSourceBuilder(TEXT("IM_Footstep_R89"), OnPlay, OnFinished, Outputs,
		Result, EMetaSoundOutputAudioFormat::Stereo, false);
	if (!Builder || Result != EMetaSoundBuilderResult::Succeeded || Outputs.Num() != 2)
	{ Error = TEXT("Stereo continuous source builder failed."); return nullptr; }
	Builder->SetSampleRateOverride(48000);
	Builder->SetBlockRateOverride(48000.f / 512.f);
	bool Good = true;
	auto AddNode = [&](FMetasoundFrontendClassName Name)
	{
		auto Node = Builder->AddNodeByClassName(Name, Result, 1);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		return Node;
	};
	const auto Player = AddNode({TEXT("UE"), TEXT("Wave Player"), TEXT("Mono")});
	const auto Gain = AddNode({TEXT("UE"), TEXT("Multiply"), TEXT("Audio by Float")});
	const auto Spatial = AddNode({TEXT("IM"), TEXT("Acoustic Source"), TEXT("Stereo")});
	auto Input = [&](const TCHAR* Name, const TCHAR* Type, FMetasoundFrontendLiteral Literal)
	{
		auto Pin = Builder->AddGraphInputNode(Name, Type, Literal, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		return Pin;
	};
	FMetasoundFrontendLiteral TriggerLiteral; TriggerLiteral.Set(false);
	FMetasoundFrontendLiteral WaveLiteral; WaveLiteral.Set(static_cast<UObject*>(InitialWave));
	FMetasoundFrontendLiteral GainLiteral; GainLiteral.Set(1.f);
	FMetasoundFrontendLiteral PitchLiteral; PitchLiteral.Set(0.f);
	FMetasoundFrontendLiteral VoiceLiteral; VoiceLiteral.Set(int32(INDEX_NONE));
	const auto Contact = Input(TEXT("Footstep Contact"), TEXT("Trigger"), TriggerLiteral);
	const auto Wave = Input(TEXT("Footstep Wave"), TEXT("WaveAsset"), WaveLiteral);
	const auto Strength = Input(TEXT("Footstep Gain"), TEXT("Float"), GainLiteral);
	const auto Pitch = Input(TEXT("Footstep Pitch"), TEXT("Float"), PitchLiteral);
	const auto Voice = Input(TEXT("Acoustic Voice"), TEXT("Int32"), VoiceLiteral);
	auto Connect = [&](const FMetaSoundBuilderNodeOutputHandle& From, const FMetaSoundNodeHandle& To, const TCHAR* Pin)
	{
		auto In = Builder->FindNodeInputByName(To, Pin, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		Builder->ConnectNodes(From, In, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
	};
	auto Output = [&](const FMetaSoundNodeHandle& Node, const TCHAR* Name)
	{
		auto Pin = Builder->FindNodeOutputByName(Node, Name, Result);
		Good &= Result == EMetaSoundBuilderResult::Succeeded;
		return Pin;
	};
	Connect(Contact, Player, TEXT("Play"));
	Connect(Wave, Player, TEXT("Wave Asset"));
	Connect(Pitch, Player, TEXT("Pitch Shift"));
	Connect(Output(Player, TEXT("Out Mono")), Gain, TEXT("PrimaryOperand"));
	Connect(Strength, Gain, TEXT("AdditionalOperands"));
	Connect(Output(Gain, TEXT("Out")), Spatial, TEXT("Mono"));
	Connect(Voice, Spatial, TEXT("Voice"));
	Builder->SetNodeInputDefault(Player, TEXT("Loop"), false, Result);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	Builder->ConnectNodes(Output(Spatial, TEXT("Left")), Outputs[0], Result);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	Builder->ConnectNodes(Output(Spatial, TEXT("Right")), Outputs[1], Result);
	Good &= Result == EMetaSoundBuilderResult::Succeeded;
	// 不连接OnPlay到Wave Player，也不连接单个clip的OnFinished到Source。
	// 一个持久operator持续交付含静音的完整块，接触只重触发素材，避免每步重建。
	if (!Good) { Error = TEXT("Footstep graph node/pin/edge contract failed."); return nullptr; }
	auto Asset = EditorSubsystem->BuildToAsset(Builder, TEXT("IceMoon"), TEXT("MS_IMFootstep"),
		TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps"), Result);
	auto* Source = Cast<UMetaSoundSource>(Asset.GetObject());
	if (!Source || Result != EMetaSoundBuilderResult::Succeeded)
	{ Error = TEXT("New footstep graph asset build failed."); return nullptr; }
	if (!IMEnsureFootstepExecutionSettings(Source, Error)) return nullptr;
	Source->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
	if (!IMSaveAsset(Source)) { Error = TEXT("Footstep graph save failed."); return nullptr; }
	FString Document;
	FJsonObjectConverter::UStructToJsonObjectString(Source->GetConstDocument(), Document);
	FFileHelper::SaveStringToFile(Document, *FPaths::Combine(IMEvidence(), TEXT("graph_authored.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return Source;
}

UMaterial* IMCreateFloorMaterial(const TCHAR* Suffix, UPhysicalMaterial* Physical, FLinearColor Color)
{
	const FString Name = TEXT("IM_FootstepAudition_") + FString(Suffix);
	const FString PackageName = TEXT("/IceMoonAcousticField/Tests/") + Name;
	if (auto* Existing = LoadObject<UMaterial>(nullptr, *ObjectPath(PackageName))) return Existing;
	auto* Material = NewObject<UMaterial>(CreatePackage(*PackageName), FName(Name), RF_Public | RF_Standalone);
	auto* BaseColor = NewObject<UMaterialExpressionConstant3Vector>(Material);
	BaseColor->Constant = Color;
	Material->GetExpressionCollection().AddExpression(BaseColor);
	Material->GetEditorOnlyData()->BaseColor.Expression = BaseColor;
	auto* Roughness = NewObject<UMaterialExpressionConstant>(Material);
	Roughness->R = .85f;
	Material->GetExpressionCollection().AddExpression(Roughness);
	Material->GetEditorOnlyData()->Roughness.Expression = Roughness;
	Material->PhysMaterial = Physical;
	Material->PostEditChange();
	FAssetRegistryModule::AssetCreated(Material);
	return IMSaveAsset(Material) ? Material : nullptr;
}

bool IMLoadExistingProfiles(UPhysicalMaterial* Wood, UPhysicalMaterial* Stone,
	TArray<FIMAcousticPhysicalMaterialMapping>& Profiles)
{
	// 复用已保存Rural场景的物理材质声学配置，不从视觉颜色推断吸收率。
	const FString SourceMap = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
	auto* World = LoadObject<UWorld>(nullptr, *ObjectPath(SourceMap));
	if (!World || !World->PersistentLevel) return false;
	for (AActor* Actor : World->PersistentLevel->Actors)
	{
		auto* Volume = Cast<AIMAcousticBakeVolume>(Actor);
		if (!Volume) continue;
		for (const auto& Profile : Volume->PhysicalMaterials)
			if (Profile.PhysicalMaterial == Wood || Profile.PhysicalMaterial == Stone) Profiles.Add(Profile);
	}
	return Profiles.Num() == 2;
}

AStaticMeshActor* AddBox(UWorld* World, const TCHAR* Label, FVector Position,
	FVector Size, UMaterialInterface* Material, UPhysicalMaterial* Physical)
{
	auto* Actor = World->SpawnActor<AStaticMeshActor>();
	if (!Actor) return nullptr;
	Actor->SetActorLabel(Label);
	auto* Mesh = Actor->GetStaticMeshComponent();
	Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	Mesh->SetMaterial(0, Material);
	Mesh->SetPhysMaterialOverride(Physical);
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetCollisionResponseToAllChannels(ECR_Block);
	Actor->SetActorLocation(Position);
	Actor->SetActorScale3D(Size / 100.f);
	return Actor;
}

bool IMVerifyScene(UWorld* World, AIMAcousticBakeVolume*& Volume, FString& Error)
{
	int32 Characters = 0, Volumes = 0;
	for (TActorIterator<AIMAcousticFootstepCharacter> It(World); It; ++It)
	{
		++Characters;
		if (!It->FootstepSound || It->WoodSteps.Num() != 2 || It->StoneSteps.Num() != 1
			|| !It->WoodPhysicalMaterial || !It->StonePhysicalMaterial || !It->ObserverCamera)
		{ Error = TEXT("Saved footstep actor asset closure is incomplete."); return false; }
	}
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It) { ++Volumes; Volume = *It; }
	if (Characters != 1 || Volumes != 1) { Error = TEXT("Footstep actor/volume cardinality differs."); return false; }
	return true;
}

class FIMFootstepBakeWait final : public IAutomationLatentCommand
{
public:
	FIMFootstepBakeWait(FAutomationTestBase* InTest, AIMAcousticBakeVolume* InVolume)
		: Test(InTest), Volume(InVolume), Started(FPlatformTime::Seconds()) {}
	bool Update() override
	{
		if (!Volume.IsValid()) { Test->AddError(TEXT("Footstep bake owner lost.")); return true; }
		if (!Volume->BakedField)
		{
			if (FPlatformTime::Seconds() - Started < 300.) return false;
			Volume->CancelBake(); Test->AddError(TEXT("Footstep bake timeout: ") + Volume->Status); return true;
		}
		FString Error;
		if (!Volume->ValidateCurrentBake(Error)) { Test->AddError(Error); return true; }
		const FString File = FPackageName::LongPackageNameToFilename(IMMap, FPackageName::GetMapPackageExtension());
		if (!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel, File))
		{ Test->AddError(TEXT("Footstep map final bake binding save failed.")); return true; }
		FAssetCompilingManager::Get().FinishAllCompilation();
		FFileHelper::SaveStringToFile(Volume->BakedField->GetPathName(),
			*FPaths::Combine(IMEvidence(), TEXT("bake_asset_path.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo prepared map=%s bake=%s probes=%d triangles=%d"),
			IMMap, *Volume->BakedField->GetPathName(), Volume->GeneratedProbes, Volume->ExportedTriangles);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] Footstep.Prepare PASS scope=saved-assets-and-bake"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor Footstep.Prepare complete"));
		return true;
	}
private:
	FAutomationTestBase* Test;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	double Started;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticFootstepPrepare, "IceMoon.AcousticField.Footstep.Prepare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticFootstepPrepare::RunTest(const FString& Parameters)
{
	IFileManager::Get().MakeDirectory(*IMAcousticFootstepSetupTestPrivate::IMEvidence(), true);
	const FString MapFile = FPackageName::LongPackageNameToFilename(IMAcousticFootstepSetupTestPrivate::IMMap, FPackageName::GetMapPackageExtension());
	if (IFileManager::Get().FileExists(*MapFile))
	{
		if (!FEditorFileUtils::LoadMap(MapFile, false, true)) { AddError(TEXT("Existing footstep map load failed.")); return false; }
		AIMAcousticBakeVolume* Volume = nullptr; FString Error;
		if (!IMAcousticFootstepSetupTestPrivate::IMVerifyScene(GEditor->GetEditorWorldContext().World(), Volume, Error)) { AddError(Error); return false; }
		if (Volume->BakedField)
		{
			if (!Volume->ValidateCurrentBake(Error)) { AddError(Error); return false; }
			for (TActorIterator<AIMAcousticFootstepCharacter> It(Volume->GetWorld()); It; ++It)
			{
				// UniformFloor probes sit 150cm above this floor with a 100cm radius.
				// At z=235cm the camera misses even the nearest sphere (106cm); z=195cm is inside (78cm).
				auto* Camera = It->ObserverCamera.Get();
				const FVector Eye(0,-360,195), Look(0,0,75);
				const FRotator ViewRotation = (Look-Eye).Rotation();
				const bool bObserverChanged = !Camera->GetActorLocation().Equals(Eye, 0.01f)
					|| !Camera->GetActorRotation().Equals(ViewRotation, 0.01f);
				if (bObserverChanged)
				{
					Camera->Modify(); Camera->SetActorLocationAndRotation(Eye, ViewRotation);
					if (!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel, MapFile))
					{ AddError(TEXT("Footstep observer coverage pose save failed.")); return false; }
				}
				if (!Volume->ValidateCurrentBake(Error)) { AddError(Error); return false; }
				UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo observer_saved location_cm=%s changed=%d"), *Camera->GetActorLocation().ToString(), bObserverChanged);
				auto* SavedGraph = Cast<UMetaSoundSource>(It->FootstepSound);
				if (!IMAcousticFootstepSetupTestPrivate::IMEnsureFootstepExecutionSettings(SavedGraph, Error)) { AddError(Error); return false; }
				FString Document;
				if (!SavedGraph || !FJsonObjectConverter::UStructToJsonObjectString(SavedGraph->GetConstDocument(), Document)
					|| !FFileHelper::SaveStringToFile(Document, *FPaths::Combine(IMAcousticFootstepSetupTestPrivate::IMEvidence(), TEXT("graph_reopened.json")),
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{ AddError(TEXT("Fresh saved footstep graph readback failed.")); return false; }
			}
			UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo prepared_existing_map graph_readback_complete"));
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] Footstep.Prepare PASS scope=fresh-saved-asset-readback-and-observer-validation"));
			UE_LOG(LogTemp, Display, TEXT("IMExitEditor Footstep.Prepare complete"));
			return true;
		}
		Volume->GenerateProbes(); Volume->Bake();
		ADD_LATENT_AUTOMATION_COMMAND(IMAcousticFootstepSetupTestPrivate::FIMFootstepBakeWait(this, Volume));
		return true;
	}
	TStrongObjectPtr<USoundWave> Wood01(IMAcousticFootstepSetupTestPrivate::IMImportStep(TEXT("wood01")));
	TStrongObjectPtr<USoundWave> Wood03(IMAcousticFootstepSetupTestPrivate::IMImportStep(TEXT("wood03")));
	TStrongObjectPtr<USoundWave> Stone01(IMAcousticFootstepSetupTestPrivate::IMImportStep(TEXT("stone01")));
	TStrongObjectPtr<UPhysicalMaterial> Wood(LoadObject<UPhysicalMaterial>(nullptr, *IMAcousticFootstepSetupTestPrivate::ObjectPath(FString(IMAcousticFootstepSetupTestPrivate::IMPMFolder)+TEXT("PM_Acoustic_Wood"))));
	TStrongObjectPtr<UPhysicalMaterial> Stone(LoadObject<UPhysicalMaterial>(nullptr, *IMAcousticFootstepSetupTestPrivate::ObjectPath(FString(IMAcousticFootstepSetupTestPrivate::IMPMFolder)+TEXT("PM_Acoustic_Stone"))));
	if (!Wood01 || !Wood03 || !Stone01 || !Wood || !Stone) { AddError(TEXT("Recorded step/physical-material assets missing.")); return false; }
	FString Error;
	TStrongObjectPtr<UMetaSoundSource> Graph(IMAcousticFootstepSetupTestPrivate::IMCreateFootstepGraph(Wood03.Get(), Error));
	if (!Graph) { AddError(Error); return false; }
	TArray<FIMAcousticPhysicalMaterialMapping> Profiles;
	if (!IMAcousticFootstepSetupTestPrivate::IMLoadExistingProfiles(Wood.Get(), Stone.Get(), Profiles))
	{ AddError(TEXT("Rural saved Wood/Stone acoustic profiles not found; no guessed replacement.")); return false; }
	TStrongObjectPtr<UMaterial> WoodMaterial(IMAcousticFootstepSetupTestPrivate::IMCreateFloorMaterial(TEXT("Wood"), Wood.Get(), FLinearColor(.33f,.12f,.045f)));
	TStrongObjectPtr<UMaterial> StoneMaterial(IMAcousticFootstepSetupTestPrivate::IMCreateFloorMaterial(TEXT("Stone"), Stone.Get(), FLinearColor(.25f,.28f,.31f)));
	if (!WoodMaterial || !StoneMaterial) { AddError(TEXT("Footstep floor material create/save failed.")); return false; }
	UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
	if (!World) { AddError(TEXT("New footstep map creation failed.")); return false; }
	IMAcousticFootstepSetupTestPrivate::AddBox(World, TEXT("IM_WoodFloor"), FVector(-225,0,-10), FVector(450,800,20), WoodMaterial.Get(), Wood.Get());
	IMAcousticFootstepSetupTestPrivate::AddBox(World, TEXT("IM_StoneFloor"), FVector(225,0,-10), FVector(450,800,20), StoneMaterial.Get(), Stone.Get());
	IMAcousticFootstepSetupTestPrivate::AddBox(World, TEXT("IM_BackWall"), FVector(0,410,150), FVector(920,20,300), StoneMaterial.Get(), Stone.Get());
	IMAcousticFootstepSetupTestPrivate::AddBox(World, TEXT("IM_LeftWall"), FVector(-460,0,150), FVector(20,820,300), StoneMaterial.Get(), Stone.Get());
	IMAcousticFootstepSetupTestPrivate::AddBox(World, TEXT("IM_RightWall"), FVector(460,0,150), FVector(20,820,300), StoneMaterial.Get(), Stone.Get());
	auto* Light = World->SpawnActor<ADirectionalLight>();
	Light->SetActorRotation(FRotator(-45,-25,0)); Light->GetLightComponent()->SetIntensity(4.f);
	auto* Camera = World->SpawnActor<ACameraActor>();
	// Keep the observer inside the unchanged 100cm UniformFloor probe influence radius.
	const FVector Eye(0,-360,195), Look(0,0,75);
	Camera->SetActorLocation(Eye); Camera->SetActorRotation((Look-Eye).Rotation()); Camera->GetCameraComponent()->SetFieldOfView(80);
	auto* Character = World->SpawnActor<AIMAcousticFootstepCharacter>(FVector(0,0,92), FRotator::ZeroRotator);
	Character->FootstepSound = Graph.Get(); Character->WoodPhysicalMaterial = Wood.Get(); Character->StonePhysicalMaterial = Stone.Get();
	Character->WoodSteps = {Wood03.Get(), Wood01.Get()}; Character->StoneSteps = {Stone01.Get()}; Character->ObserverCamera = Camera;
	auto* Start = World->SpawnActor<APlayerStart>(FVector(0,-300,150), FRotator::ZeroRotator);
	Start->SetActorLabel(TEXT("IM_FootstepObserverStart"));
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	auto* Volume = World->SpawnActor<AIMAcousticBakeVolume>();
	Volume->SetActorLocation(FVector(0,0,195)); Volume->BakeBounds->SetBoxExtent(FVector(470,430,205));
	Volume->MaterialDrive = EIMAcousticMaterialDrive::PhysicalMaterial; Volume->PhysicalMaterials = Profiles;
	Volume->ProbeSpacingCm = 100; Volume->ProbeHeightCm = 150; Volume->ReverbWetGain = .25f;
	Volume->Tags.Add(TEXT("IM_FootstepAudition_R89"));
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (!FEditorFileUtils::SaveLevel(World->PersistentLevel, MapFile) || World->GetOutermost()->GetName() != IMAcousticFootstepSetupTestPrivate::IMMap)
	{ AddError(TEXT("Footstep map first save/world identity failed.")); return false; }
	if (!IMAcousticFootstepSetupTestPrivate::IMVerifyScene(World, Volume, Error)) { AddError(Error); return false; }
	Volume->GenerateProbes();
	if (!Volume->GeneratedProbes) { AddError(TEXT("Footstep probes missing: ")+Volume->Status); return false; }
	Volume->Bake();
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticFootstepSetupTestPrivate::FIMFootstepBakeWait(this, Volume));
	return true;
}
#endif
