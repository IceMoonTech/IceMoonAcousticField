#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Components/StaticMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/StaticMesh.h"
#include "Sound/SoundBase.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace IMAcousticPhysicalMaterialWhiteboxTestPrivate
{
constexpr const TCHAR* IMPMWhiteboxMap=TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");

UPhysicalMaterial* IMPMWhiteboxAsset(const TCHAR* Name,FString& Failure)
{
	const FString PackageName=FString(TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/"))+Name;
	if(UPhysicalMaterial* Existing=LoadObject<UPhysicalMaterial>(nullptr,*(PackageName+TEXT(".")+Name)))return Existing;
	const FString File=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
	if(IFileManager::Get().FileExists(*File)){Failure=TEXT("Physical material package exists with the wrong type.");return nullptr;}
	UPackage* Package=CreatePackage(*PackageName);
	UPhysicalMaterial* Asset=NewObject<UPhysicalMaterial>(Package,Name,RF_Public|RF_Standalone);
	Asset->MarkPackageDirty();
	FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
	if(!UPackage::SavePackage(Package,Asset,*File,Args)){Failure=TEXT("Cannot save whitebox physical material.");return nullptr;}
	FAssetRegistryModule::AssetCreated(Asset);return Asset;
}

FString IMPMWhiteboxUnchangedIdentity(UWorld* World)
{
	TArray<FString> Items;
	for(TActorIterator<AActor> It(World);It;++It)
	{
		TArray<UStaticMeshComponent*> Meshes;It->GetComponents(Meshes);
		for(auto* Component:Meshes)
			Items.Add(Component->GetPathName()+TEXT("|")+GetPathNameSafe(Component->GetStaticMesh())+TEXT("|")+Component->GetComponentTransform().ToString());
		TArray<UAudioComponent*> Audio;It->GetComponents(Audio);
		for(auto* Component:Audio)
			Items.Add(Component->GetPathName()+TEXT("|")+GetPathNameSafe(Component->Sound)+TEXT("|")
				+FString::Printf(TEXT("%.9g|%s"),Component->VolumeMultiplier,*Component->GetComponentTransform().ToString()));
	}
	Items.Sort();FSHA1 Hash;
	for(const FString& Item:Items){FTCHARToUTF8 Utf8(*Item);Hash.Update(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());}
	Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);return BytesToHex(Digest,20);
}

class FIMPMWhiteboxPrepareCommand final:public IAutomationLatentCommand
{
public:
	explicit FIMPMWhiteboxPrepareCommand(FAutomationTestBase* InTest,const TCHAR* InMap)
		:Test(InTest),Started(FPlatformTime::Seconds()),MapPackage(InMap){}
	bool Update() override
	{
		if(FPlatformTime::Seconds()-Started>900.0)return Finish(false,TEXT("Physical-material whitebox preparation timed out."));
		if(Stage==0)
		{
			if(!GEditor||GEditor->PlayWorld)return Finish(false,TEXT("Requires an idle owned Editor."));
			UWorld* Current=GEditor->GetEditorWorldContext().World();
			if(Current&&Current->GetOutermost()->IsDirty())return Finish(false,TEXT("Current map has unsaved changes."));
			if(!FEditorFileUtils::LoadMap(MapPackage,false,true))return Finish(false,TEXT("Existing whitebox map is missing."));
			LoadedFrame=GFrameCounter;Stage=1;return false;
		}
		if(Stage==1)
		{
			if(GFrameCounter<=LoadedFrame)return false;
			UWorld* World=GEditor->GetEditorWorldContext().World();
			if(!World)return Finish(false,TEXT("Whitebox world failed to load."));
			for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It)
			{
				if(Volume.IsValid())return Finish(false,TEXT("Ambiguous whitebox bake owner."));
				Volume=*It;
			}
			if(!Volume.IsValid()||!Volume->BakedField)return Finish(false,TEXT("Missing existing whitebox bake binding."));
			Before=IMPMWhiteboxUnchangedIdentity(World);OldBake=Volume->BakedField->GetPathName();
			FString Failure;
			UPhysicalMaterial* Stone=IMPMWhiteboxAsset(TEXT("PM_Acoustic_Stone"),Failure);
			UPhysicalMaterial* Wood=IMPMWhiteboxAsset(TEXT("PM_Acoustic_Wood"),Failure);
			if(!Stone||!Wood)return Finish(false,Failure);
			Volume->Modify();Volume->MaterialDrive=EIMAcousticMaterialDrive::PhysicalMaterial;
			Volume->PhysicalMaterials.Reset();
			FIMAcousticPhysicalMaterialMapping StoneProfile;
			StoneProfile.PhysicalMaterial=Stone;StoneProfile.Absorption=FVector(.13,.20,.24);StoneProfile.Scattering=.05f;
			Volume->PhysicalMaterials.Add(StoneProfile);
			FIMAcousticPhysicalMaterialMapping WoodProfile;
			WoodProfile.PhysicalMaterial=Wood;WoodProfile.Absorption=FVector(.11,.07,.06);WoodProfile.Scattering=.05f;
			WoodProfile.bSupportsProbes=false;Volume->PhysicalMaterials.Add(WoodProfile);
			for(TActorIterator<AActor> It(World);It;++It)
			{
				TArray<UStaticMeshComponent*> Meshes;It->GetComponents(Meshes);
				for(auto* Component:Meshes)
				{
					if(Component->ComponentHasTag(TEXT("IMAcousticIgnore")))continue;
					const bool Dynamic=Volume->DynamicBlockers.Contains(Component);
					if(Component->Mobility!=EComponentMobility::Static&&!Dynamic)continue;
					Component->Modify();Component->SetPhysMaterialOverride(Dynamic?Wood:Stone);++Assigned;
				}
			}
			if(Assigned==0)return Finish(false,TEXT("No existing whitebox acoustic meshes were assigned."));
			Previous=Volume->BakedField;ReadyStarted=FPlatformTime::Seconds();Stage=2;return false;
		}
		if(!Volume.IsValid())return Finish(false,TEXT("Whitebox owner was destroyed."));
		if(Stage==2)
		{
			Volume->GenerateProbes();
			if(Volume->GeneratedProbes<=0)
			{
				if(Volume->Status==TEXT("Scene components are still updating bounds; retry validation after the Editor tick.")
					&& FPlatformTime::Seconds()-ReadyStarted<=5.0)return false;
				return Finish(false,TEXT("Whitebox physical probes failed: ")+Volume->Status);
			}
			Volume->Bake();
			if(!Volume->Status.StartsWith(TEXT("Baking.")))return Finish(false,TEXT("Whitebox physical bake did not start: ")+Volume->Status);
			Stage=3;return false;
		}
		if(Stage==4)
		{
			if(!FEditorFileUtils::LoadMap(MapPackage,false,true))return Finish(false,TEXT("Saved whitebox map reload failed."));
			Volume.Reset();
			for(TActorIterator<AIMAcousticBakeVolume> It(GEditor->GetEditorWorldContext().World());It;++It)
			{
				if(Volume.IsValid())return Finish(false,TEXT("Reloaded whitebox bake owner is ambiguous."));
				Volume=*It;
			}
			LoadedFrame=GFrameCounter;ReadyStarted=FPlatformTime::Seconds();Stage=5;return false;
		}
		if(Stage==5)
		{
			if(GFrameCounter<=LoadedFrame)return false;
			FString Failure;
			if(!Volume->ValidateCurrentBake(Failure))
			{
				if(Volume->Status==TEXT("Scene components are still updating bounds; retry validation after the Editor tick.")
					&& FPlatformTime::Seconds()-ReadyStarted<=5.0)return false;
				return Finish(false,TEXT("Reloaded whitebox bake failed: ")+Failure);
			}
			if(Volume->MaterialDrive!=EIMAcousticMaterialDrive::PhysicalMaterial
				|| GetPathNameSafe(Volume->BakedField.Get())!=SavedBake
				|| Before!=IMPMWhiteboxUnchangedIdentity(GEditor->GetEditorWorldContext().World()))
				return Finish(false,TEXT("Whitebox mode, bake binding or protected geometry/source identity changed after reload."));
			return Finish(true,TEXT("Whitebox physical-material bake saved and reloaded; listening remains required."));
		}
		if(Stage!=3)return Finish(false,TEXT("Unexpected whitebox preparation stage."));
		if(!Volume->BakedField||Volume->BakedField==Previous.Get())
		{
			if(Volume->Status.Contains(TEXT("failed"),ESearchCase::IgnoreCase)||Volume->Status.Contains(TEXT("discarded"),ESearchCase::IgnoreCase))
				return Finish(false,Volume->Status);
			return false;
		}
		UWorld* World=GEditor->GetEditorWorldContext().World();
		if(Before!=IMPMWhiteboxUnchangedIdentity(World))return Finish(false,TEXT("Whitebox geometry/source identity changed."));
		FString Failure;
		if(!Volume->ValidateCurrentBake(Failure))return Finish(false,Failure);
		World->MarkPackageDirty();
		const FString File=FPackageName::LongPackageNameToFilename(MapPackage,FPackageName::GetMapPackageExtension());
		if(!FEditorFileUtils::SaveLevel(World->PersistentLevel,File))return Finish(false,TEXT("Whitebox map binding save failed."));
		SavedBake=Volume->BakedField->GetPathName();Stage=4;return false;
	}
private:
	bool Finish(bool Success,const FString& Detail)
	{
		const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/PhysicalMaterialDrive"));
		IFileManager::Get().MakeDirectory(*Directory,true);
		FString Json;auto Writer=TJsonWriterFactory<>::Create(&Json);Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("status"),Success?TEXT("PREPARED_NOT_LISTENING_ACCEPTED"):TEXT("FAILED"));
		Writer->WriteValue(TEXT("detail"),Detail);Writer->WriteValue(TEXT("map"),MapPackage);
		Writer->WriteValue(TEXT("assigned_components"),Assigned);Writer->WriteValue(TEXT("old_bake"),OldBake);
		Writer->WriteValue(TEXT("new_bake"),Volume.IsValid()?GetPathNameSafe(Volume->BakedField.Get()):FString());
		Writer->WriteValue(TEXT("unchanged_geometry_source_identity"),Before);Writer->WriteObjectEnd();Writer->Close();
		const FString Evidence=FPaths::Combine(Directory,TEXT("whitebox_authoring_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".json"));
		if(!FFileHelper::SaveStringToFile(Json,*Evidence,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Success=false;Test->AddError(TEXT("Cannot save required whitebox preparation evidence: ")+Evidence);
		}
		if(Success){UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] PASS test=PhysicalMaterialWhiteboxPrepare detail=%s"),*Detail);}
		else{Test->AddError(Detail);UE_LOG(LogTemp,Error,TEXT("[IM][PIE_TEST] FAIL test=PhysicalMaterialWhiteboxPrepare detail=%s"),*Detail);}
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s PhysicalMaterialWhiteboxPrepare"),Success?TEXT("PASS"):TEXT("FAIL"));
		return true;
	}
	FAutomationTestBase* Test;double Started,ReadyStarted=0;uint64 LoadedFrame=0;int32 Stage=0,Assigned=0;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;TWeakObjectPtr<UIMAcousticBakeAsset> Previous;
	FString Before,OldBake,MapPackage,SavedBake;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMPhysicalMaterialWhiteboxPrepareTest,"IceMoon.AcousticField.PhysicalMaterial.WhiteboxPrepare",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMPhysicalMaterialWhiteboxPrepareTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticPhysicalMaterialWhiteboxTestPrivate::FIMPMWhiteboxPrepareCommand(this,IMAcousticPhysicalMaterialWhiteboxTestPrivate::IMPMWhiteboxMap));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMPhysicalMaterialDoorFixturePrepareTest,"IceMoon.AcousticField.PhysicalMaterial.DoorFixturePrepare",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMPhysicalMaterialDoorFixturePrepareTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticPhysicalMaterialWhiteboxTestPrivate::FIMPMWhiteboxPrepareCommand(this,TEXT("/IceMoonAcousticField/Tests/IM_W1Door")));return true;
}
// The original W3 map predates MaterialDrive/PhysicalMaterials serialization.
// Prepare its existing geometry and source layout with the same stone/wood
// profiles before testing the new default; keep the original pressure load.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMPhysicalMaterialPressureFixturePrepareTest,"IceMoon.AcousticField.PhysicalMaterial.PressureFixturePrepare",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMPhysicalMaterialPressureFixturePrepareTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticPhysicalMaterialWhiteboxTestPrivate::FIMPMWhiteboxPrepareCommand(this,TEXT("/IceMoonAcousticField/L_IceMoonAcousticField")));return true;
}
#endif
