#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

namespace IMAcousticConstructionMaterialAuditTestPrivate
{
constexpr const TCHAR* Map = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");

static FString PackageHash(const FString& Package)
{
	FString File;
	if (!FPackageName::DoesPackageExist(Package, &File)) return FString();
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *File)) return FString();
	FSHAHash Hash;
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
	return Hash.ToString();
}

static TSharedRef<FJsonObject> MeshRow(const UStaticMeshComponent& Mesh)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("path"), Mesh.GetPathName());
	Row->SetStringField(TEXT("archetype"), Mesh.GetArchetype()->GetPathName());
	Row->SetStringField(TEXT("mesh"), GetPathNameSafe(Mesh.GetStaticMesh()));
	Row->SetNumberField(TEXT("creation_method"), int32(Mesh.CreationMethod));
	Row->SetNumberField(TEXT("ucs_serialization_index"), Mesh.GetUCSSerializationIndex());
	Row->SetBoolField(TEXT("editable_when_inherited"), Mesh.IsEditableWhenInherited());
	Row->SetNumberField(TEXT("override_material_count"), Mesh.GetNumOverrideMaterials());
	TArray<TSharedPtr<FJsonValue>> Materials;
	for (int32 Slot = 0; Slot < Mesh.GetNumMaterials(); ++Slot)
	{
		TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
		UMaterialInterface* MI = Mesh.GetMaterial(Slot);
		Material->SetNumberField(TEXT("slot"), Slot);
		Material->SetStringField(TEXT("material"), GetPathNameSafe(MI));
		Material->SetStringField(TEXT("physical_material"), MI ? GetPathNameSafe(MI->GetPhysicalMaterial()) : FString());
		Materials.Add(MakeShared<FJsonValueObject>(Material));
	}
	Row->SetArrayField(TEXT("materials"), Materials);
	return Row;
}

class FIMCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMCommand(FAutomationTestBase* InTest) : Test(InTest) {}
	bool Update() override
	{
		if (!Loaded)
		{
			UWorld* Before = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!Before) return Finish(false, TEXT("No Editor world."));
			StartupMap = Before->GetOutermost()->GetName();
			BeforeHash = PackageHash(Map);
			if (BeforeHash.IsEmpty() || !FEditorFileUtils::LoadMap(Map, false, true))
				return Finish(false, TEXT("Read-only construction binding audit could not load the preview."));
			Loaded = true;
			LoadFrame = GFrameCounter;
			LoadedAt = FPlatformTime::Seconds();
			return false;
		}
		if (GFrameCounter <= LoadFrame + 2) return false;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World || World->GetOutermost()->GetName() != Map)
			return Finish(false, TEXT("Audit world identity changed."));
		TArray<TSharedPtr<FJsonValue>> ActorRows;
		TArray<TSharedPtr<FJsonValue>> ClassRows;
		TSet<UBlueprintGeneratedClass*> SeenClasses;
		int32 Components = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UStaticMeshComponent*> Meshes;
			It->GetComponents(Meshes);
			TArray<TSharedPtr<FJsonValue>> GeneratedMeshes;
			for (UStaticMeshComponent* Mesh : Meshes)
			{
				if (IsValid(Mesh) && Mesh->CreationMethod == EComponentCreationMethod::UserConstructionScript)
				{
					GeneratedMeshes.Add(MakeShared<FJsonValueObject>(MeshRow(*Mesh)));
					++Components;
				}
			}
			if (GeneratedMeshes.IsEmpty()) continue;
			TSharedRef<FJsonObject> Actor = MakeShared<FJsonObject>();
			Actor->SetStringField(TEXT("actor"), It->GetPathName());
			Actor->SetStringField(TEXT("class"), It->GetClass()->GetPathName());
			Actor->SetArrayField(TEXT("generated_meshes"), GeneratedMeshes);
			TArray<TSharedPtr<FJsonValue>> Properties;
			for (TFieldIterator<FProperty> Prop(It->GetClass(), EFieldIteratorFlags::ExcludeSuper); Prop; ++Prop)
			{
				if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
				FString Value;
				Prop->ExportText_InContainer(0, Value, *It, nullptr, *It, PPF_None);
				TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
				Property->SetStringField(TEXT("name"), Prop->GetName());
				Property->SetStringField(TEXT("type"), Prop->GetCPPType());
				Property->SetBoolField(TEXT("value_omitted_over_512_chars"), Value.Len() > 512);
				if (Value.Len() <= 512) Property->SetStringField(TEXT("value"), Value);
				Properties.Add(MakeShared<FJsonValueObject>(Property));
			}
			Actor->SetArrayField(TEXT("editable_blueprint_properties"), Properties);
			ActorRows.Add(MakeShared<FJsonValueObject>(Actor));
			UBlueprintGeneratedClass* Class = Cast<UBlueprintGeneratedClass>(It->GetClass());
			if (!Class || SeenClasses.Contains(Class)) continue;
			SeenClasses.Add(Class);
			TSharedRef<FJsonObject> ClassRow = MakeShared<FJsonObject>();
			ClassRow->SetStringField(TEXT("class"), Class->GetPathName());
			ClassRow->SetStringField(TEXT("package_sha1"), PackageHash(Class->GetOutermost()->GetName()));
			TArray<TSharedPtr<FJsonValue>> Templates;
			for (UActorComponent* Template : Class->ComponentTemplates)
				if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Template))
					Templates.Add(MakeShared<FJsonValueObject>(MeshRow(*Mesh)));
			ClassRow->SetArrayField(TEXT("k2_component_templates"), Templates);
			ClassRows.Add(MakeShared<FJsonValueObject>(ClassRow));
		}
		if (Components == 0 && FPlatformTime::Seconds() - LoadedAt < 10.0) return false;
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("scope"), TEXT("READ_ONLY_OBSERVATION_NOT_BINDING_OR_PRODUCT_ACCEPTANCE"));
		Report->SetStringField(TEXT("status"), Components > 0 ? TEXT("OBSERVED") : TEXT("INCOMPLETE"));
		Report->SetStringField(TEXT("map"), Map);
		Report->SetNumberField(TEXT("post_load_frame_delta"), double(GFrameCounter - LoadFrame));
		Report->SetNumberField(TEXT("generated_static_mesh_components"), Components);
		Report->SetArrayField(TEXT("actors"), ActorRows);
		Report->SetArrayField(TEXT("classes"), ClassRows);
		const bool Restored = FEditorFileUtils::LoadMap(StartupMap, false, true);
		const FString AfterHash = PackageHash(Map);
		Report->SetBoolField(TEXT("startup_map_restored"), Restored);
		Report->SetBoolField(TEXT("maps_or_assets_saved"), false);
		Report->SetStringField(TEXT("map_sha1_before"), BeforeHash);
		Report->SetStringField(TEXT("map_sha1_after"), AfterHash);
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialDrive"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Path = FPaths::Combine(Directory, TEXT("construction_binding_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"));
		FString Json;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		const bool Saved = FJsonSerializer::Serialize(Report, Writer) && FFileHelper::SaveStringToFile(Json, *Path);
		Test->AddInfo(TEXT("Read-only construction binding report: ") + Path);
		return Finish(Saved && Restored && Components > 0 && BeforeHash == AfterHash,
			TEXT("Read-only construction component observation completed; no binding acceptance implied."));
	}
private:
	bool Finish(bool Passed, const FString& Message)
	{
		if (!Passed) Test->AddError(Message);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] ConstructionMaterialAudit %s"), Passed ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s ConstructionMaterialAudit"), Passed ? TEXT("PASS") : TEXT("FAIL"));
		return true;
	}
	FAutomationTestBase* Test;
	FString StartupMap, BeforeHash;
	bool Loaded = false;
	uint64 LoadFrame = 0;
	double LoadedAt = 0.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticConstructionMaterialAuditTest,
	"IceMoon.AcousticField.Rural.ConstructionMaterialAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticConstructionMaterialAuditTest::RunTest(const FString&)
{
	AddCommand(new IMAcousticConstructionMaterialAuditTestPrivate::FIMCommand(this));
	return true;
}

#endif
