#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSimulation.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW2Inventory,"IceMoon.AcousticField.W2.Inventory",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW2Inventory::RunTest(const FString&)
{
	const bool Loaded=FEditorFileUtils::LoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,true);
	if(!Loaded){AddError(TEXT("Existing whitebox map missing."));return false;}
	UWorld* World=GEditor->GetEditorWorldContext().World();
	FString Json;auto W=TJsonWriterFactory<>::Create(&Json);W->WriteArrayStart();int32 MeshCount=0;
	for(TActorIterator<AActor> It(World);It;++It)
	{
		TArray<UPrimitiveComponent*> Components;It->GetComponents(Components);
		for(auto* Component:Components)
		{
			W->WriteObjectStart();W->WriteValue(TEXT("actor"),It->GetActorLabel());W->WriteValue(TEXT("component"),Component->GetPathName());
			W->WriteValue(TEXT("class"),Component->GetClass()->GetName());W->WriteValue(TEXT("mobility"),int32(Component->Mobility));
			W->WriteValue(TEXT("transform"),Component->GetComponentTransform().ToString());
			W->WriteValue(TEXT("bounds"),Component->Bounds.GetBox().ToString());
			if(auto* Mesh=Cast<UStaticMeshComponent>(Component))
			{
				++MeshCount;W->WriteValue(TEXT("mesh"),GetPathNameSafe(Mesh->GetStaticMesh()));
				W->WriteArrayStart(TEXT("materials"));for(int32 I=0;I<Mesh->GetNumMaterials();++I)W->WriteValue(GetPathNameSafe(Mesh->GetMaterial(I)));W->WriteArrayEnd();
				W->WriteValue(TEXT("physical_material"),GetPathNameSafe(Mesh->BodyInstance.GetSimplePhysicalMaterial()));
				if(auto* Instances=Cast<UInstancedStaticMeshComponent>(Mesh))W->WriteValue(TEXT("instances"),Instances->GetInstanceCount());
			}
			W->WriteObjectEnd();
		}
	}
	W->WriteArrayEnd();W->Close();
	const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W2-UE"));IFileManager::Get().MakeDirectory(*Directory,true);
	const bool Success=MeshCount>0&&FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Directory,TEXT("existing-whitebox-inventory.json")));
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW2Inventory meshes=%d"),MeshCount);
	UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW2Inventory %s"),Success?TEXT("PASS"):TEXT("FAIL"));
	UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));
	return Success;
}
namespace IMAcousticW2TestPrivate
{
constexpr const TCHAR* W2Map=TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
FString W2GeometryIdentity(UWorld* World)
{
	TArray<FString> Items;
	for(TActorIterator<AActor> It(World);It;++It)
	{
		TArray<UStaticMeshComponent*> Components;It->GetComponents(Components);
		for(auto* M:Components)Items.Add(M->GetPathName()+TEXT("|")+GetPathNameSafe(M->GetStaticMesh())+TEXT("|")+M->GetComponentTransform().ToString());
	}
	Items.Sort();FSHA1 Hash;for(const FString& Item:Items){FTCHARToUTF8 Bytes(*Item);Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());}
	Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);return BytesToHex(Digest,20);
}
class FIMAcousticW2BakeCommand final:public IAutomationLatentCommand
{
public:
	explicit FIMAcousticW2BakeCommand(FAutomationTestBase* InTest):Test(InTest),Started(FPlatformTime::Seconds()){}
	bool Update() override
	{
		if(FPlatformTime::Seconds()-Started>900)return Finish(false,TEXT("Actual whitebox bake timed out."));
		if(Stage==0)
		{
			if(!FEditorFileUtils::LoadMap(W2Map,false,true))return Finish(false,TEXT("Existing whitebox map missing."));
			UWorld* World=GEditor->GetEditorWorldContext().World();GeometryBefore=W2GeometryIdentity(World);
			FBox Bounds(ForceInit);int32 WhiteboxCount=0;UMaterialInterface* Material=nullptr;
			for(TActorIterator<AActor> It(World);It;++It)
			{
				if(auto* Existing=Cast<AIMAcousticBakeVolume>(*It))
				{if(Volume.IsValid())return Finish(false,TEXT("Ambiguous multiple acoustic bake volumes."));Volume=Existing;}
				TArray<UPrimitiveComponent*> Components;It->GetComponents(Components);
				for(auto* C:Components)
				{
					// This fixture's visual sky and spawn marker are not surfaces.
					// Persist explicit exclusions instead of weakening the exporter.
					if(It->GetActorLabel()==TEXT("SM_SkySphere")||It->IsA<APlayerStart>())
					{C->Modify();C->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));}
					if(!It->ActorHasTag(TEXT("IMAcousticWhitebox")))continue;
					if(auto* M=Cast<UStaticMeshComponent>(C))
					{
						++WhiteboxCount;Bounds+=M->Bounds.GetBox();
						if(Material&&Material!=M->GetMaterial(0))return Finish(false,TEXT("Whitebox material inventory changed; explicit mapping needs review."));
						Material=M->GetMaterial(0);
					}
				}
			}
			if(WhiteboxCount!=60||!Bounds.IsValid||!Material)return Finish(false,TEXT("Existing whitebox geometry inventory differs from 60 verified meshes."));
			if(!Volume.IsValid())
			{
				Volume=World->SpawnActor<AIMAcousticBakeVolume>();Volume->MaterialDrive=EIMAcousticMaterialDrive::LegacyRenderMaterial;Volume->SetActorLabel(TEXT("IM_V2Bake"));
				Volume->SetActorLocation(Bounds.GetCenter());Volume->BakeBounds->SetBoxExtent(Bounds.GetExtent()+FVector(25));
				FIMAcousticMaterialMapping Mapping;Mapping.Material=Material;Mapping.Absorption=FVector(.25);Mapping.Scattering=.5f;
				Volume->Materials.Add(Mapping);
			}
			Volume->GenerateProbes();
			if(Volume->GeneratedProbes<=0)
			{for(const auto& Issue:Volume->SceneIssues)UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticW2Scene %s"),*Issue);return Finish(false,Volume->Status);}
			Previous=Volume->BakedField;Volume->Bake();Stage=1;return false;
		}
		if(!Volume.IsValid())return Finish(false,TEXT("Bake owner destroyed."));
		if(Stage==1)
		{
			if(Volume->Status.Contains(TEXT("failed"),ESearchCase::IgnoreCase)||Volume->Status.Contains(TEXT("discarded")))return Finish(false,Volume->Status);
			if(!Volume->BakedField||Volume->BakedField==Previous.Get())return false;
			FString Error;if(!Volume->ValidateCurrentBake(Error))return Finish(false,Error);
			if(W2GeometryIdentity(Volume->GetWorld())!=GeometryBefore)return Finish(false,TEXT("Test changed existing scene geometry."));
			auto* Asset=Volume->BakedField.Get();
			auto* Corrupt=DuplicateObject<UIMAcousticBakeAsset>(Asset,GetTransientPackage());
			Corrupt->ProbeData[0]^=1;
			if(Corrupt->Validate(Asset->WorldPackage,Asset->SceneFingerprint,Error))return Finish(false,TEXT("Corrupt payload accepted."));
			if(Asset->Validate(TEXT("/IceMoonAcousticField/Tests/IM_W1Door"),Asset->SceneFingerprint,Error))return Finish(false,TEXT("Wrong map accepted."));
			TArray<uint8> RawScene,RawProbes;
			if(!Asset->ReadPayload(Asset->WorldPackage,Asset->SceneFingerprint,RawScene,RawProbes,Error))
				return Finish(false,TEXT("Cannot read raw SDK payload for W2 checks: ")+Error);
			const FString OldDigest=Corrupt->PayloadDigest,OldMetadata=Corrupt->MetadataJson;
			TArray<uint8> Scene=RawScene,Probes=RawProbes;
			if(Corrupt->CommitCompleteBake(Asset->WorldPackage,Asset->SceneFingerprint,TEXT("{}"),MoveTemp(Scene),MoveTemp(Probes),Error)
				||Corrupt->PayloadDigest!=OldDigest||Corrupt->MetadataJson!=OldMetadata)return Finish(false,TEXT("Invalid commit mutated previous asset."));
			const FVector Absorption=Volume->Materials[0].Absorption;Volume->Materials[0].Absorption=FVector(.8);
			const bool StaleAccepted=Volume->ValidateCurrentBake(Error);Volume->Materials[0].Absorption=Absorption;
			if(StaleAccepted||!Volume->ValidateCurrentBake(Error))return Finish(false,TEXT("Material stale/restore validation failed."));
			const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W2-UE"));
			FFileHelper::SaveStringToFile(Asset->MetadataJson,*FPaths::Combine(Directory,TEXT("bake-metadata.json")));
			if(!FFileHelper::SaveArrayToFile(RawScene,*FPaths::Combine(Directory,TEXT("scene.bin")))
				||!FFileHelper::SaveArrayToFile(RawProbes,*FPaths::Combine(Directory,TEXT("probes.bin"))))
				return Finish(false,TEXT("Cannot save raw SDK bake evidence."));
			FFileHelper::SaveStringToFile(GeometryBefore,*FPaths::Combine(Directory,TEXT("geometry-identity.txt")));
			const FString File=FPackageName::LongPackageNameToFilename(W2Map,FPackageName::GetMapPackageExtension());
			if(!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel,File))return Finish(false,TEXT("Cannot save whitebox bake binding."));
			Previous=Asset;Volume->GenerateProbes();Volume->Bake();Volume->CancelBake();Stage=2;return false;
		}
		if(!Volume->Status.StartsWith(TEXT("Bake cancelled")))return false;
		FString Error;
		if(Volume->BakedField!=Previous.Get()||!Volume->ValidateCurrentBake(Error))return Finish(false,TEXT("Cancelled bake replaced valid asset."));
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW2Bake triangles=%d probes=%d elapsed_s=%g geometry_preserved=1 negatives=4"),Volume->ExportedTriangles,Volume->GeneratedProbes,FPlatformTime::Seconds()-Started);
		return Finish(true,TEXT("Actual whitebox bake, metadata, stale/corrupt/wrong-map/cancel gates passed."));
	}
private:
	bool Finish(bool Success,const FString& Message)
	{
		if(!Success)Test->AddError(Message);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW2 %s"),*Message);
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW2 %s"),Success?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));return true;
	}
	FAutomationTestBase* Test;double Started;int32 Stage=0;FString GeometryBefore;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;TWeakObjectPtr<UIMAcousticBakeAsset> Previous;
};
}
namespace IMAcousticW2TestPrivate
{
constexpr const TCHAR* IMW2RuralSource = TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_CE06F85C4DBFC6E0D63FF8A345E1C1A6.IM_IM_RuralCamera3Preview_10m_CE06F85C4DBFC6E0D63FF8A345E1C1A6");
constexpr const TCHAR* IMW2RuralWorld = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* IMW2RuralFingerprint = TEXT("955D1C0A2195BAE4DEB1EEADD3ACC4F1FEDAE103");
constexpr const TCHAR* IMW2RuralSourcePackageSha1 = TEXT("C4E46C62867DB86CAAB56E6E0C0FA710641630FE");
constexpr const TCHAR* IMW2RuralPayloadDigest = TEXT("25BBA85CF45A8A12CCBD34E11C1434CE8CE7B23D");

FString IMW2ByteSha1(const TArray<uint8>& Bytes)
{
	return FSHA1::HashBuffer(Bytes.GetData(), uint64(Bytes.Num())).ToString().ToUpper();
}

bool IMW2FileSha1(const FString& Filename, FString& Out)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.IsEmpty()) return false;
	Out = IMW2ByteSha1(Bytes);
	return true;
}

bool IMW2RoundTripGuid(FString& RunId, FString& Error)
{
	if (!FParse::Value(FCommandLine::Get(), TEXT("IMStorageRoundTripGuid="), RunId) || RunId.Len() != 32)
	{ Error = TEXT("Rural serialization needs an explicit 32-hex -IMStorageRoundTripGuid."); return false; }
	for (const TCHAR Character : RunId)
	{
		if (!FChar::IsHexDigit(Character))
		{ Error = TEXT("Rural serialization GUID contains a non-hex character."); return false; }
	}
	return true;
}

FString IMW2RoundTripDirectory(const FString& RunId)
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AcousticV2/StorageRoundTrip"), RunId));
}

bool IMW2RuralReadPayload(UIMAcousticBakeAsset* Asset, int32 ExpectedFormat,
	TArray<uint8>& Scene, TArray<uint8>& Probes, FString& Error)
{
	if (!Asset || Asset->FormatVersion != ExpectedFormat)
	{ Error = TEXT("Rural round-trip asset has the wrong storage format."); return false; }
	if (!Asset->ReadPayload(IMW2RuralWorld, IMW2RuralFingerprint, Scene, Probes, Error)) return false;
	if (Scene.Num() != 13724688 || Probes.Num() != 1250180
		|| IMW2ByteSha1(Scene) != TEXT("BAFE6F1D4D32BA7341DF940DB7830D97FCC52594")
		|| IMW2ByteSha1(Probes) != TEXT("EFFA2C7EB34BFB5FC1232962D2F867A36A081ECA"))
	{ Error = TEXT("Rural raw SDK sections differ from the frozen CE06 bytes."); return false; }
	return true;
}

bool IMW2RuralSourceRead(UIMAcousticBakeAsset*& Source, TArray<uint8>& Scene,
	TArray<uint8>& Probes, FString& SourceFile, FString& Error)
{
	Source = LoadObject<UIMAcousticBakeAsset>(nullptr, IMW2RuralSource);
	if (!Source) { Error = TEXT("Frozen Rural CE06 source could not be loaded."); return false; }
	SourceFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(
		Source->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()));
	FString PackageHash;
	if (!IMW2FileSha1(SourceFile, PackageHash) || PackageHash != IMW2RuralSourcePackageSha1
		|| Source->PayloadDigest != IMW2RuralPayloadDigest || Source->GetOutermost()->IsDirty())
	{ Error = TEXT("Frozen Rural source package, digest or clean state changed."); return false; }
	return IMW2RuralReadPayload(Source, 3, Scene, Probes, Error);
}

TSharedRef<FJsonObject> IMW2RuralReport(const TCHAR* Schema, const FString& RunId)
{
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), Schema);
	Report->SetStringField(TEXT("status"), TEXT("PASS"));
	Report->SetStringField(TEXT("run_guid"), RunId);
	Report->SetNumberField(TEXT("process_id"), FPlatformProcess::GetCurrentProcessId());
	Report->SetStringField(TEXT("world_package"), IMW2RuralWorld);
	Report->SetStringField(TEXT("scene_fingerprint"), IMW2RuralFingerprint);
	return Report;
}

bool IMW2SaveRuralReport(const TSharedRef<FJsonObject>& Report, const FString& Filename)
{
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	return FJsonSerializer::Serialize(Report, Writer)
		&& FFileHelper::SaveStringToFile(Json, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// This branch serializes a new, unreferenced GUID package. It never resaves CE06
// or the preview map; Root removes the exact GUID package after both PIDs exit.
bool IMW2SerializeRural(FAutomationTestBase& Test)
{
	auto Fail = [&Test](const FString& Error) { Test.AddError(Error); return false; };
	FString RunId, Error;
	if (!IMW2RoundTripGuid(RunId, Error)) return Fail(Error);
	const FString Directory = IMW2RoundTripDirectory(RunId);
	const FString ReportFile = FPaths::Combine(Directory, TEXT("stage1.json"));
	const FString SavedCopy = FPaths::Combine(Directory, TEXT("packed-rural.uasset"));
	const FString PackageName = TEXT("/IceMoonAcousticField/Bakes/IM_StorageRoundTrip_") + RunId;
	const FString AssetName = FPackageName::GetShortName(PackageName);
	const FString AssetPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension()));
	const FString Stem = FPaths::Combine(FPaths::GetPath(Filename), AssetName);
	for (const TCHAR* Extension : {TEXT(".uasset"), TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl"), TEXT(".m.ubulk")})
	{
		if (IFileManager::Get().FileExists(*(Stem + Extension)))
			return Fail(TEXT("Rural serialization GUID package path already exists."));
	}
	if (FindPackage(nullptr, *PackageName) || IFileManager::Get().FileExists(*ReportFile)
		|| IFileManager::Get().FileExists(*SavedCopy))
		return Fail(TEXT("Rural serialization GUID or report is already in use."));

	UIMAcousticBakeAsset* Source = nullptr;
	TArray<uint8> Scene, Probes;
	FString SourceFile;
	if (!IMW2RuralSourceRead(Source, Scene, Probes, SourceFile, Error)) return Fail(Error);
	if (!IFileManager::Get().MakeDirectory(*Directory, true)) return Fail(TEXT("Cannot create Rural round-trip evidence directory."));
	UPackage* Package = CreatePackage(*PackageName);
	UIMAcousticBakeAsset* Persisted = NewObject<UIMAcousticBakeAsset>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Persisted) return Fail(TEXT("Cannot create Rural format4 package candidate."));
	TArray<uint8> CommitScene = Scene, CommitProbes = Probes;
	if (!Persisted->CommitCompleteBake(IMW2RuralWorld, IMW2RuralFingerprint, Source->MetadataJson,
		MoveTemp(CommitScene), MoveTemp(CommitProbes), Error)) return Fail(Error);
	TArray<uint8> DecodedScene, DecodedProbes;
	if (!IMW2RuralReadPayload(Persisted, 4, DecodedScene, DecodedProbes, Error)) return Fail(Error);
	if (DecodedScene != Scene || DecodedProbes != Probes) return Fail(TEXT("Rural format4 changed raw SDK bytes before serialization."));
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const double SaveStart = FPlatformTime::Seconds();
	if (!UPackage::SavePackage(Package, Persisted, *Filename, SaveArgs)) return Fail(TEXT("Cannot save new Rural format4 package."));
	const double SaveMs = (FPlatformTime::Seconds() - SaveStart) * 1000.0;
	TArray<uint8> SerializedBytes;
	if (!FFileHelper::LoadFileToArray(SerializedBytes, *Filename)
		|| !FFileHelper::SaveArrayToFile(SerializedBytes, *SavedCopy)) return Fail(TEXT("Cannot preserve serialized Rural package evidence."));
	FString SourceAfter, SavedHash;
	if (!IMW2FileSha1(SourceFile, SourceAfter) || SourceAfter != IMW2RuralSourcePackageSha1
		|| Source->GetOutermost()->IsDirty() || !IMW2FileSha1(SavedCopy, SavedHash)
		|| SavedHash != IMW2ByteSha1(SerializedBytes)) return Fail(TEXT("Rural source or serialized evidence copy changed."));
	const int64 SourceBytes = IFileManager::Get().FileSize(*SourceFile);
	if (SerializedBytes.IsEmpty() || SerializedBytes.Num() >= SourceBytes)
		return Fail(TEXT("Rural serialized format4 package is not smaller than the frozen format3 package."));
	TSharedRef<FJsonObject> Report = IMW2RuralReport(TEXT("IMAcousticW2SerializedFormat4Stage1.v1"), RunId);
	Report->SetStringField(TEXT("source_asset"), IMW2RuralSource);
	Report->SetNumberField(TEXT("source_format"), 3);
	Report->SetNumberField(TEXT("output_format"), Persisted->FormatVersion);
	Report->SetStringField(TEXT("source_package_sha1_before"), IMW2RuralSourcePackageSha1);
	Report->SetStringField(TEXT("source_package_sha1_after"), SourceAfter);
	Report->SetStringField(TEXT("original_payload_digest"), Source->PayloadDigest);
	Report->SetNumberField(TEXT("raw_scene_bytes"), Scene.Num());
	Report->SetStringField(TEXT("raw_scene_sha1"), IMW2ByteSha1(Scene));
	Report->SetNumberField(TEXT("raw_probe_bytes"), Probes.Num());
	Report->SetStringField(TEXT("raw_probe_sha1"), IMW2ByteSha1(Probes));
	Report->SetBoolField(TEXT("raw_bytes_equal"), true);
	Report->SetStringField(TEXT("serialized_asset_path"), AssetPath);
	Report->SetStringField(TEXT("content_package_file"), Filename);
	Report->SetNumberField(TEXT("source_package_bytes"), SourceBytes);
	Report->SetNumberField(TEXT("serialized_package_bytes"), SerializedBytes.Num());
	Report->SetStringField(TEXT("serialized_package_sha1"), IMW2ByteSha1(SerializedBytes));
	Report->SetStringField(TEXT("saved_package_copy"), SavedCopy);
	Report->SetNumberField(TEXT("format4_packed_scene_bytes"), Persisted->SceneData.Num());
	Report->SetNumberField(TEXT("format4_packed_probe_bytes"), Persisted->ProbeData.Num());
	Report->SetNumberField(TEXT("format4_packed_total_bytes"), Persisted->SceneData.Num() + Persisted->ProbeData.Num());
	Report->SetNumberField(TEXT("save_ms"), SaveMs);
	if (!IMW2SaveRuralReport(Report, ReportFile)) return Fail(TEXT("Cannot save Rural serialization stage1 report."));
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticSerializedRuralStage1 guid=%s pid=%u package=%s bytes=%d raw_bytes_equal=1"),
		*RunId, FPlatformProcess::GetCurrentProcessId(), *AssetPath, SerializedBytes.Num());
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS AcousticSerializedRuralStage1"));
	return true;
}

bool IMW2ColdLoadRural(FAutomationTestBase& Test)
{
	auto Fail = [&Test](const FString& Error) { Test.AddError(Error); return false; };
	FString RunId, Error;
	if (!IMW2RoundTripGuid(RunId, Error)) return Fail(Error);
	const FString Directory = IMW2RoundTripDirectory(RunId);
	const FString ReportFile = FPaths::Combine(Directory, TEXT("stage2.json"));
	FString Stage1Json;
	TSharedPtr<FJsonObject> Stage1;
	if (IFileManager::Get().FileExists(*ReportFile)
		|| !FFileHelper::LoadFileToString(Stage1Json, *FPaths::Combine(Directory, TEXT("stage1.json")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Stage1Json), Stage1) || !Stage1.IsValid())
		return Fail(TEXT("Missing stage1 report or reused stage2 report path."));
	FString ReportGuid, Stage1Status, Stage1Schema, Stage1Hash;
	double Stage1Pid = 0.0;
	if (!Stage1->TryGetStringField(TEXT("schema"), Stage1Schema) || Stage1Schema != TEXT("IMAcousticW2SerializedFormat4Stage1.v1")
		|| !Stage1->TryGetStringField(TEXT("run_guid"), ReportGuid) || ReportGuid != RunId
		|| !Stage1->TryGetStringField(TEXT("status"), Stage1Status) || Stage1Status != TEXT("PASS")
		|| !Stage1->TryGetStringField(TEXT("serialized_package_sha1"), Stage1Hash)
		|| !Stage1->TryGetNumberField(TEXT("process_id"), Stage1Pid) || Stage1Pid <= 0.0
		|| Stage1Pid == FPlatformProcess::GetCurrentProcessId())
		return Fail(TEXT("Stage1 report does not identify a successful distinct serialization process."));
	const FString Name = TEXT("IM_StorageRoundTrip_") + RunId;
	const FString PackageName = TEXT("/IceMoonAcousticField/Bakes/") + Name;
	const FString AssetPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension()));
	FString StoredHash;
	if (FindObject<UIMAcousticBakeAsset>(nullptr, *AssetPath)
		|| !IMW2FileSha1(Filename, StoredHash) || StoredHash != Stage1Hash)
		return Fail(TEXT("Serialized Rural package is already loaded or differs from stage1 bytes."));
	UIMAcousticBakeAsset* Stored = LoadObject<UIMAcousticBakeAsset>(nullptr, *AssetPath);
	TArray<uint8> Scene, Probes;
	const double DecodeStart = FPlatformTime::Seconds();
	if (!IMW2RuralReadPayload(Stored, 4, Scene, Probes, Error)) return Fail(Error);
	const double DecodeMs = (FPlatformTime::Seconds() - DecodeStart) * 1000.0;
	UIMAcousticBakeAsset* Source = nullptr;
	TArray<uint8> SourceScene, SourceProbes;
	FString SourceFile;
	if (!IMW2RuralSourceRead(Source, SourceScene, SourceProbes, SourceFile, Error)) return Fail(Error);
	if (Scene != SourceScene || Probes != SourceProbes) return Fail(TEXT("Cold serialized Rural decode differs byte-for-byte from CE06."));
	TArray<FVector4> Preview;
	FVector OriginCm;
	if (!Stored->GetProbePreview(IMW2RuralWorld, IMW2RuralFingerprint, Preview, OriginCm, Error)) return Fail(Error);
	if (Preview.Num() != 121) return Fail(TEXT("Cold Rural format4 probe preview differs from 121 frozen probes."));
	FIMAcousticBakeData Data;
	Data.Scene.assign(Scene.GetData(), Scene.GetData() + Scene.Num());
	Data.ProbeBatch.assign(Probes.GetData(), Probes.GetData() + Probes.Num());
	for (const FVector4& Probe : Preview)
		Data.CoverageProbes.push_back({{float(Probe.X), float(Probe.Y), float(Probe.Z)}, float(Probe.W)});
	FIMAcousticSimulation Simulation;
	std::string SDKError;
	const double SDKStart = FPlatformTime::Seconds();
	if (!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), SDKError)
		|| !Simulation.Load(Data, 48000, 512, SDKError))
		return Fail(FString::Printf(TEXT("Cold Rural format4 SDK load failed: %s"), UTF8_TO_TCHAR(SDKError.c_str())));
	const double SDKMs = (FPlatformTime::Seconds() - SDKStart) * 1000.0;
	FString StoredAfter;
	if (!IMW2FileSha1(Filename, StoredAfter) || StoredAfter != StoredHash || Stored->GetOutermost()->IsDirty())
		return Fail(TEXT("Cold Rural SDK load changed the serialized package or dirty state."));
	TSharedRef<FJsonObject> Report = IMW2RuralReport(TEXT("IMAcousticW2SerializedFormat4Stage2.v1"), RunId);
	Report->SetStringField(TEXT("serialized_asset_path"), AssetPath);
	Report->SetStringField(TEXT("content_package_file"), Filename);
	Report->SetStringField(TEXT("serialized_package_sha1"), StoredHash);
	Report->SetNumberField(TEXT("loaded_format"), Stored->FormatVersion);
	Report->SetNumberField(TEXT("decoded_scene_bytes"), Scene.Num());
	Report->SetStringField(TEXT("decoded_scene_sha1"), IMW2ByteSha1(Scene));
	Report->SetNumberField(TEXT("decoded_probe_bytes"), Probes.Num());
	Report->SetStringField(TEXT("decoded_probe_sha1"), IMW2ByteSha1(Probes));
	Report->SetBoolField(TEXT("decoded_bytes_equal_frozen_source"), true);
	Report->SetNumberField(TEXT("probe_preview_count"), Preview.Num());
	Report->SetStringField(TEXT("pathing_options"), TEXT("DefaultHybrid"));
	Report->SetNumberField(TEXT("sdk_sample_rate"), 48000);
	Report->SetNumberField(TEXT("sdk_buffer_size"), 512);
	Report->SetBoolField(TEXT("sdk_load_pass"), true);
	Report->SetBoolField(TEXT("sdk_equivalence"), false);
	Report->SetNumberField(TEXT("decode_ms"), DecodeMs);
	Report->SetNumberField(TEXT("sdk_load_ms"), SDKMs);
	if (!IMW2SaveRuralReport(Report, ReportFile)) return Fail(TEXT("Cannot save Rural cold SDK stage2 report."));
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticSerializedRuralStage2 guid=%s pid=%u package=%s probes=%d raw_bytes_equal=1 sdk_load=1"),
		*RunId, FPlatformProcess::GetCurrentProcessId(), *AssetPath, Preview.Num());
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS AcousticSerializedRuralStage2"));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticW2FormatRoundTrip,"IceMoon.AcousticField.W2.LegacyFormatRoundTrip",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticW2FormatRoundTrip::RunTest(const FString&)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("IMStorageRoundTripRural"))) return IMAcousticW2TestPrivate::IMW2SerializeRural(*this);
	constexpr const TCHAR* LegacyAssetPath=TEXT("/IceMoonAcousticField/Bakes/IM_IM_V2Audition_51EB95704ADA37C752DC909BA5B90830.IM_IM_V2Audition_51EB95704ADA37C752DC909BA5B90830");
	constexpr const TCHAR* ExpectedWorld=TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
	constexpr const TCHAR* ExpectedFingerprint=TEXT("BB8D5F57F4B5830F89804A80967B557D278BCDF5");
	UIMAcousticBakeAsset* Legacy=LoadObject<UIMAcousticBakeAsset>(nullptr,LegacyAssetPath);
	if(!Legacy){AddError(TEXT("Frozen legacy bake asset could not be loaded."));return false;}
	if(Legacy->FormatVersion!=2)
	{AddError(FString::Printf(TEXT("Legacy compatibility source must be FormatVersion 2; actual=%d."),Legacy->FormatVersion));return false;}

	TArray<uint8> LegacyRawScene,LegacyRawProbes;FString Error;
	if(!Legacy->ReadPayload(ExpectedWorld,ExpectedFingerprint,LegacyRawScene,LegacyRawProbes,Error))
	{AddError(TEXT("Format 2 raw payload read failed: ")+Error);return false;}
	if(LegacyRawScene.IsEmpty()||LegacyRawProbes.IsEmpty())
	{AddError(TEXT("Format 2 source returned an empty raw SDK section."));return false;}

	TStrongObjectPtr<UIMAcousticBakeAsset> Packed(NewObject<UIMAcousticBakeAsset>(GetTransientPackage(),NAME_None,RF_Transient));
	if(!Packed.IsValid()){AddError(TEXT("Could not create transient packed round-trip asset."));return false;}
	TArray<uint8> CommitScene=LegacyRawScene,CommitProbes=LegacyRawProbes;
	if(!Packed->CommitCompleteBake(ExpectedWorld,ExpectedFingerprint,Legacy->MetadataJson,MoveTemp(CommitScene),MoveTemp(CommitProbes),Error))
	{AddError(TEXT("Transient packed commit from v2 raw payload failed: ")+Error);return false;}
	if(Packed->FormatVersion!=UIMAcousticBakeAsset::CurrentFormat)
	{AddError(FString::Printf(TEXT("Transient commit did not produce current format %d; actual=%d."),UIMAcousticBakeAsset::CurrentFormat,Packed->FormatVersion));return false;}

	TArray<uint8> RoundTripRawScene,RoundTripRawProbes;
	if(!Packed->ReadPayload(ExpectedWorld,ExpectedFingerprint,RoundTripRawScene,RoundTripRawProbes,Error))
	{AddError(TEXT("Transient packed raw payload readback failed: ")+Error);return false;}
	if(RoundTripRawScene!=LegacyRawScene||RoundTripRawProbes!=LegacyRawProbes)
	{AddError(TEXT("Format 2 -> transient packed -> raw readback changed SDK bytes."));return false;}
	if(Packed->SceneData.Num()>=RoundTripRawScene.Num()||Packed->ProbeData.Num()>=RoundTripRawProbes.Num())
	{AddError(TEXT("Transient packed section storage did not compress below its raw SDK byte count."));return false;}

	TArray<FVector4> ProbePreview;FVector OriginCm;
	if(!Packed->GetProbePreview(ExpectedWorld,ExpectedFingerprint,ProbePreview,OriginCm,Error))
	{AddError(TEXT("Packed probe preview failed: ")+Error);return false;}
	FIMAcousticBakeData Data;
	Data.Scene.assign(RoundTripRawScene.GetData(),RoundTripRawScene.GetData()+RoundTripRawScene.Num());
	Data.ProbeBatch.assign(RoundTripRawProbes.GetData(),RoundTripRawProbes.GetData()+RoundTripRawProbes.Num());
	for(const FVector4& Probe:ProbePreview)
	{Data.CoverageProbes.push_back({{float(Probe.X),float(Probe.Y),float(Probe.Z)},float(Probe.W)});}
	FIMAcousticSimulation Simulation;std::string SDKError;
	if(!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(),SDKError)
		||!Simulation.Load(Data,48000,512,SDKError))
	{AddError(FString::Printf(TEXT("Packed decoded payload SDK load failed: %s"),UTF8_TO_TCHAR(SDKError.c_str())));return false;}

	const FString RunId=FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/StorageRoundTrip"),RunId);
	if(!IFileManager::Get().MakeDirectory(*Directory,true))
	{AddError(TEXT("Cannot create storage round-trip evidence directory."));return false;}
	const FString PackageName=TEXT("/IceMoonAcousticField/Bakes/IM_StorageRoundTrip_")+RunId;
	const FString AssetName=FPackageName::GetShortName(PackageName);
	UPackage* Package=CreatePackage(*PackageName);
	UIMAcousticBakeAsset* Persisted=NewObject<UIMAcousticBakeAsset>(Package,*AssetName,RF_Public|RF_Standalone);
	if(!Persisted){AddError(TEXT("Cannot create packed package candidate."));return false;}
	TArray<uint8> PersistScene=LegacyRawScene,PersistProbes=LegacyRawProbes;
	if(!Persisted->CommitCompleteBake(ExpectedWorld,ExpectedFingerprint,Legacy->MetadataJson,
		MoveTemp(PersistScene),MoveTemp(PersistProbes),Error))
	{AddError(TEXT("Cannot commit persistent packed package candidate: ")+Error);return false;}
	FSavePackageArgs SaveArgs;SaveArgs.TopLevelFlags=RF_Public|RF_Standalone;
	const FString Filename=FPaths::Combine(Directory,TEXT("packed-audition.uasset"));
	const double SaveStartedAt=FPlatformTime::Seconds();
	if(!UPackage::SavePackage(Package,Persisted,*Filename,SaveArgs))
	{AddError(TEXT("Cannot serialize packed round-trip asset package."));return false;}
	const double SaveMs=(FPlatformTime::Seconds()-SaveStartedAt)*1000.0;
	const int64 PackageBytes=IFileManager::Get().FileSize(*Filename);
	const FString LegacyFilename=FPackageName::LongPackageNameToFilename(
		Legacy->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension());
	const int64 LegacyPackageBytes=IFileManager::Get().FileSize(*LegacyFilename);
	if(PackageBytes<=0||LegacyPackageBytes<=0||PackageBytes>=LegacyPackageBytes)
	{AddError(TEXT("Serialized packed package is missing or not smaller than the v2 source package."));return false;}
	const FString Report=FString::Printf(TEXT("{\"run_id\":\"%s\",\"source_format\":2,\"output_format\":%d,\"raw_scene_bytes\":%d,\"raw_probe_bytes\":%d,\"packed_scene_bytes\":%d,\"packed_probe_bytes\":%d,\"legacy_package_bytes\":%lld,\"packed_package_bytes\":%lld,\"packed_save_ms\":%.3f,\"raw_bytes_equal\":true,\"packed_sdk_load\":true}"),
		*RunId,Packed->FormatVersion,RoundTripRawScene.Num(),RoundTripRawProbes.Num(),Packed->SceneData.Num(),Packed->ProbeData.Num(),
		LegacyPackageBytes,PackageBytes,SaveMs);
	if(!FFileHelper::SaveStringToFile(Report,*FPaths::Combine(Directory,TEXT("report.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{AddError(TEXT("Cannot save storage round-trip report."));return false;}

	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW2FormatRoundTrip source_format=2 output_format=%d scene_raw_bytes=%d scene_packed_bytes=%d probe_raw_bytes=%d probe_packed_bytes=%d legacy_package_bytes=%lld packed_package_bytes=%lld packed_save_ms=%.3f raw_bytes_equal=1 sdk_load=1 report_dir=%s"),
		Packed->FormatVersion,RoundTripRawScene.Num(),Packed->SceneData.Num(),RoundTripRawProbes.Num(),Packed->ProbeData.Num(),
		LegacyPackageBytes,PackageBytes,SaveMs,*Directory);
	UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW2FormatRoundTrip PASS"));
	UE_LOG(LogTemp,Display,TEXT("IMExitEditor PASS AcousticW2FormatRoundTrip"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticW2StoredColdLoad,"IceMoon.AcousticField.W2.StoredCurrentFormatColdLoad",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticW2StoredColdLoad::RunTest(const FString&)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("IMStorageRoundTripRural"))) return IMAcousticW2TestPrivate::IMW2ColdLoadRural(*this);
	FString RunId;
	if(!FParse::Value(FCommandLine::Get(),TEXT("IMStorageRoundTripGuid="),RunId)
		||RunId.Len()!=32)
	{AddError(TEXT("Missing 32-character -IMStorageRoundTripGuid for stored packed cold load."));return false;}
	for(const TCHAR Character:RunId)
	{if(!FChar::IsHexDigit(Character)){AddError(TEXT("Invalid storage round-trip GUID."));return false;}}
	const FString Name=TEXT("IM_StorageRoundTrip_")+RunId;
	const FString AssetPath=TEXT("/IceMoonAcousticField/Bakes/")+Name+TEXT(".")+Name;
	UIMAcousticBakeAsset* Stored=LoadObject<UIMAcousticBakeAsset>(nullptr,*AssetPath);
	if(!Stored||Stored->FormatVersion!=UIMAcousticBakeAsset::CurrentFormat)
	{AddError(TEXT("Stored current-format package cannot be cold loaded by its logical asset path."));return false;}
	constexpr const TCHAR* World=TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
	constexpr const TCHAR* Fingerprint=TEXT("BB8D5F57F4B5830F89804A80967B557D278BCDF5");
	TArray<uint8> Scene,Probes;FString Error;
	const double DecodeStartedAt=FPlatformTime::Seconds();
	if(!Stored->ReadPayload(World,Fingerprint,Scene,Probes,Error))
	{AddError(TEXT("Stored packed payload cannot be decoded: ")+Error);return false;}
	const double DecodeMs=(FPlatformTime::Seconds()-DecodeStartedAt)*1000.0;
	if(Scene.Num()!=85088||Probes.Num()!=6573404)
	{AddError(TEXT("Stored packed raw section lengths differ from the frozen audition source."));return false;}
	TArray<FVector4> Preview;FVector OriginCm;
	if(!Stored->GetProbePreview(World,Fingerprint,Preview,OriginCm,Error))
	{AddError(TEXT("Stored packed probe preview failed: ")+Error);return false;}
	FIMAcousticBakeData Data;
	Data.Scene.assign(Scene.GetData(),Scene.GetData()+Scene.Num());
	Data.ProbeBatch.assign(Probes.GetData(),Probes.GetData()+Probes.Num());
	for(const FVector4& Probe:Preview)
	{Data.CoverageProbes.push_back({{float(Probe.X),float(Probe.Y),float(Probe.Z)},float(Probe.W)});}
	FIMAcousticSimulation Simulation;std::string SDKError;
	if(!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(),SDKError))
	{AddError(UTF8_TO_TCHAR(SDKError.c_str()));return false;}
	const double SDKStartedAt=FPlatformTime::Seconds();
	if(!Simulation.Load(Data,48000,512,SDKError))
	{AddError(FString::Printf(TEXT("Stored packed SDK load failed: %s"),UTF8_TO_TCHAR(SDKError.c_str())));return false;}
	const double SDKLoadMs=(FPlatformTime::Seconds()-SDKStartedAt)*1000.0;
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticStoredCurrentFormatColdLoad asset=%s raw_scene_bytes=%d raw_probe_bytes=%d packed_scene_bytes=%d packed_probe_bytes=%d probes=%d decode_ms=%.3f sdk_load_ms=%.3f"),
		*AssetPath,Scene.Num(),Probes.Num(),Stored->SceneData.Num(),Stored->ProbeData.Num(),Preview.Num(),DecodeMs,SDKLoadMs);
	UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticStoredCurrentFormatColdLoad PASS"));
	UE_LOG(LogTemp,Display,TEXT("IMExitEditor PASS AcousticStoredCurrentFormatColdLoad"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW2Bake,"IceMoon.AcousticField.W2.BakeExisting",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW2Bake::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW2TestPrivate::FIMAcousticW2BakeCommand(this));return true;}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW2Cold,"IceMoon.AcousticField.W2.ColdLoad",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW2Cold::RunTest(const FString&)
{
	bool Success=FEditorFileUtils::LoadMap(IMAcousticW2TestPrivate::W2Map,false,true);FString Error;int32 Found=0;
	if(Success)for(TActorIterator<AIMAcousticBakeVolume> It(GEditor->GetEditorWorldContext().World());It;++It)
	{
		++Found;Success=It->ValidateCurrentBake(Error)&&Success;
		if(Success){TArray<FVector4> Probes;FVector Origin;Success=It->BakedField->GetProbePreview(IMAcousticW2TestPrivate::W2Map,It->BakedField->SceneFingerprint,Probes,Origin,Error)&&!Probes.IsEmpty();}
	}
	Success=Success&&Found==1;if(!Success)AddError(TEXT("Cold-loaded bake invalid: ")+Error);
	UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW2ColdLoad %s"),Success?TEXT("PASS"):TEXT("FAIL"));
	UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));return Success;
}
#endif
