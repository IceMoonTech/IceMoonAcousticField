#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "IMAcousticLandscapeGrassMaterials.h"
#include "IMAcousticGrassWorldTransforms.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticGrassBakePlan.h"
#include "LandscapeComponent.h"
#include "LandscapeGrassType.h"
#include "LandscapeProxy.h"
#include "MaterialCachedData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Containers/StringConv.h"

namespace IMAcousticGrassGenerationProbeTestPrivate
{
constexpr const TCHAR* TestMap = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* GrassComponentClassPath = TEXT("/Script/Foliage.GrassInstancedStaticMeshComponent");
constexpr const TCHAR* PluginGrassTypeRoot = TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/GrassTypes/");
constexpr const TCHAR* PluginMaterialRoot = TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/");
constexpr double CameraToleranceCm = 0.001;
const FVector Camera3Location(-6597.06298828125, 14720.8662109375, 395.2615966796875);

struct FIMComponentBinding
{
	ALandscapeProxy* Proxy = nullptr;
	ULandscapeComponent* Component = nullptr;
	TMap<FName, TObjectPtr<ULandscapeGrassType>> SourceNamedGrassTypes;
	TMap<FName, TObjectPtr<ULandscapeGrassType>> OriginalPluginNamedGrassTypes;
};

struct FIMGrassSlotRecord
{
	int32 SlotIndex = INDEX_NONE;
	FString SlotName;
	FString MaterialPath;
	FString PhysicalMaterialPath;
};

struct FIMGrassComponentRecord
{
	int32 Seed = 0;
	FString ComponentClass;
	FString ComponentPath;
	FString OwnerPath;
	FString MeshPath;
	bool bRegistered = false;
	bool bTransient = false;
	int32 InstanceCount = 0;
	int32 CPUInstanceCount = 0;
	int32 RenderInstanceCount = 0;
	int32 BufferInstanceCount = 0;
	bool bSceneProxyPresent = false;
	bool bSceneBufferObserved = false;
	bool bGPUOnly = false;
	TArray<FIMGrassSlotRecord> Slots;
	TArray<FTransform> WorldTransforms;
	FString TransformSha1;
	FVector BoundsMin = FVector::ZeroVector;
	FVector BoundsMax = FVector::ZeroVector;
};

struct FIMPhaseRecord
{
	FString Name;
	double GenerationMilliseconds = 0.0;
	double TransformReadMilliseconds = 0.0;
	double TransformCollectionMilliseconds = 0.0;
	int32 GrassComponentCount = 0;
	int32 ComponentsWithInstances = 0;
	int64 TotalInstances = 0;
	int64 TotalCPUInstances = 0;
	int32 ComponentsWithRenderInstances = 0;
	int64 TotalRenderInstances = 0;
	TArray<FIMGrassComponentRecord> Components;
};

struct FIMSeedCandidate
{
	ULandscapeComponent* LandscapeComponent = nullptr;
	FName OutputName;
	ULandscapeGrassType* SourceType = nullptr;
	ULandscapeGrassType* PluginType = nullptr;
	FString SeedText;
	int32 Seed = 0;
	int32 SectionBaseX = INDEX_NONE;
	int32 SectionBaseY = INDEX_NONE;
	int32 VarietyIndex = INDEX_NONE;
	int32 SubsectionX = INDEX_NONE;
	int32 SubsectionY = INDEX_NONE;
};

struct FIMTrackedPackage
{
	UPackage* Package = nullptr;
	bool bDirtyBefore = false;
	bool bSourceGamePackage = false;
	FString Filename;
	FString Sha1Before;
	FString Sha1After;
	int64 BytesBefore = 0;
	int64 BytesAfter = 0;
	bool bSha1BeforeRead = false;
	bool bSha1AfterRead = false;
};

static FString ObjectPath(const UObject* Object)
{
	return IsValid(Object) ? Object->GetPathName() : FString();
}

static TArray<FName> SortedKeys(const TMap<FName, TObjectPtr<ULandscapeGrassType>>& Map)
{
	TArray<FName> Keys;
	Map.GetKeys(Keys);
	Keys.Sort([](const FName& A, const FName& B)
	{
		return A.ToString() < B.ToString();
	});
	return Keys;
}

static TSharedRef<FJsonObject> NamedGrassTypesToJson(
	const TMap<FName, TObjectPtr<ULandscapeGrassType>>& NamedGrassTypes)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FName& Key : SortedKeys(NamedGrassTypes))
	{
		const TObjectPtr<ULandscapeGrassType>* Type = NamedGrassTypes.Find(Key);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("output_name"), Key.ToString());
		Row->SetStringField(TEXT("grass_type"), Type ? ObjectPath(Type->Get()) : FString());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("entries"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return Result;
}

static FString NamedGrassTypesHash(const TMap<FName, TObjectPtr<ULandscapeGrassType>>& NamedGrassTypes)
{
	FString Content;
	for (const FName& Key : SortedKeys(NamedGrassTypes))
	{
		const TObjectPtr<ULandscapeGrassType>* Type = NamedGrassTypes.Find(Key);
		Content += Key.ToString();
		Content += TEXT("|");
		Content += Type ? ObjectPath(Type->Get()) : TEXT("<null>");
		Content += TEXT("\n");
	}
	uint8 Digest[20];
	FSHA1::HashBuffer(reinterpret_cast<const uint8*>(*Content), Content.Len() * sizeof(TCHAR), Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

static bool HashFile(const FString& Filename, FString& OutSha1, int64& OutBytes, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
	{
		OutError = TEXT("Could not read source package bytes: ") + Filename;
		return false;
	}
	uint8 Digest[20];
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
	OutSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	OutBytes = Bytes.Num();
	return true;
}

static bool HashPackageFile(UPackage* Package, FString& OutFilename, FString& OutSha1, int64& OutBytes, FString& OutError)
{
	if (!IsValid(Package))
	{
		OutError = TEXT("A source package reference is invalid.");
		return false;
	}
	const FString PackageName = Package->GetName();
	OutFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	OutFilename = FPaths::ConvertRelativePathToFull(OutFilename);
	if (!FPaths::FileExists(OutFilename))
	{
		OutError = TEXT("A source package file is missing: ") + OutFilename;
		return false;
	}
	return HashFile(OutFilename, OutSha1, OutBytes, OutError);
}

static void AddObjectPackage(UObject* Object, TSet<UPackage*>& Packages)
{
	if (IsValid(Object) && IsValid(Object->GetOutermost()))
	{
		Packages.Add(Object->GetOutermost());
	}
}

static void AddMaterialPackages(UMaterialInterface* Material, TSet<UPackage*>& Packages)
{
	TSet<UMaterialInterface*> Visited;
	while (IsValid(Material) && !Visited.Contains(Material))
	{
		Visited.Add(Material);
		AddObjectPackage(Material, Packages);
		AddObjectPackage(Material->GetPhysicalMaterial(), Packages);
		UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Material);
		Material = MaterialInstance ? MaterialInstance->Parent.Get() : nullptr;
	}
}

static UMaterialInterface* GetVarietyMaterial(const FGrassVariety& Variety, const int32 SlotIndex)
{
	if (Variety.OverrideMaterials.IsValidIndex(SlotIndex) && IsValid(Variety.OverrideMaterials[SlotIndex].Get()))
	{
		return Variety.OverrideMaterials[SlotIndex].Get();
	}
	return IsValid(Variety.GrassMesh.Get()) ? Variety.GrassMesh->GetMaterial(SlotIndex) : nullptr;
}

static bool HashGrassData(const TArray<FIMComponentBinding>& Bindings, FString& OutHash, FString& OutError)
{
	TArray<const FIMComponentBinding*> SortedBindings;
	for (const FIMComponentBinding& Binding : Bindings)
	{
		SortedBindings.Add(&Binding);
	}
	SortedBindings.Sort([](const FIMComponentBinding& A, const FIMComponentBinding& B)
	{
		return A.Component->GetPathName() < B.Component->GetPathName();
	});

	FString Content;
	for (const FIMComponentBinding* Binding : SortedBindings)
	{
		if (!Binding || !IsValid(Binding->Component))
		{
			OutError = TEXT("A LandscapeComponent disappeared while hashing source GrassData.");
			return false;
		}
		const FLandscapeComponentGrassData& Data = Binding->Component->GrassData.Get();
		uint8 DataDigest[20] = {};
		if (!Data.HeightWeightData.IsEmpty())
		{
			FSHA1::HashBuffer(Data.HeightWeightData.GetData(), Data.HeightWeightData.Num(), DataDigest);
		}
		Content += Binding->Component->GetPathName();
		Content += FString::Printf(TEXT("|%u|%d|%s|"), Data.GenerationHash, Data.NumElements, *BytesToHex(DataDigest, UE_ARRAY_COUNT(DataDigest)));
		TArray<FName> WeightKeys;
		Data.WeightOffsets.GetKeys(WeightKeys);
		WeightKeys.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
		for (const FName& Key : WeightKeys)
		{
			Content += FString::Printf(TEXT("%s:%d;"), *Key.ToString(), Data.WeightOffsets.FindChecked(Key));
		}
		Content += TEXT("\n");
	}
	uint8 Digest[20];
	FSHA1::HashBuffer(reinterpret_cast<const uint8*>(*Content), Content.Len() * sizeof(TCHAR), Digest);
	OutHash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	return true;
}

static FString SeedTextAndValue(
	const FName GrassName,
	const ULandscapeGrassType* GrassType,
	const FIntPoint SectionBase,
	const int32 SubsectionX,
	const int32 SubsectionY,
	const int32 VarietyIndex,
	int32& OutSeed)
{
	const FString Text = FString::Printf(
		TEXT("%s_%s_%d_%d_%d_%d_%d"),
		*GrassName.ToString().ToLower(),
		(IsValid(GrassType) ? *GrassType->GetName().ToLower() : TEXT("<invalid>")),
		SectionBase.X,
		SectionBase.Y,
		SubsectionX,
		SubsectionY,
		VarietyIndex);
	OutSeed = FCrc::StrCrc32(StringCast<ANSICHAR>(*Text).Get());
	if (OutSeed == 0)
	{
		++OutSeed;
	}
	return Text;
}

static TSharedRef<FJsonObject> SeedCandidateToJson(const FIMSeedCandidate& Candidate)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("landscape_component"), ObjectPath(Candidate.LandscapeComponent));
	Row->SetStringField(TEXT("output_name"), Candidate.OutputName.ToString());
	Row->SetStringField(TEXT("source_grass_type"), ObjectPath(Candidate.SourceType));
	Row->SetStringField(TEXT("plugin_grass_type"), ObjectPath(Candidate.PluginType));
	Row->SetStringField(TEXT("source_grass_type_short_name"), IsValid(Candidate.SourceType) ? Candidate.SourceType->GetName() : FString());
	Row->SetStringField(TEXT("plugin_grass_type_short_name"), IsValid(Candidate.PluginType) ? Candidate.PluginType->GetName() : FString());
	Row->SetStringField(TEXT("seed_text"), Candidate.SeedText);
	Row->SetNumberField(TEXT("observed_seed"), Candidate.Seed);
	Row->SetNumberField(TEXT("section_base_x"), Candidate.SectionBaseX);
	Row->SetNumberField(TEXT("section_base_y"), Candidate.SectionBaseY);
	Row->SetNumberField(TEXT("variety_index"), Candidate.VarietyIndex);
	Row->SetNumberField(TEXT("subsection_x"), Candidate.SubsectionX);
	Row->SetNumberField(TEXT("subsection_y"), Candidate.SubsectionY);
	return Row;
}

static bool BuildSeedAssociations(
	const TArray<FIMComponentBinding>& Bindings,
	const TSet<int32>& ObservedSeeds,
	TMap<int32, TArray<FIMSeedCandidate>>& OutCandidates,
	FString& OutError)
{
	// UE 5.8 creates grass components with a CRC32 of output name, grass-type short
	// name, section base, subsection coordinates, and variety index. Subsection
	// count is clamped to 16 in LandscapeGrass.cpp; scanning that bounded domain
	// avoids guessing from component or mesh names.
	for (const FIMComponentBinding& Binding : Bindings)
	{
		const FIntPoint SectionBase = Binding.Component->GetSectionBase();
		for (const FName& OutputName : SortedKeys(Binding.SourceNamedGrassTypes))
		{
			const TObjectPtr<ULandscapeGrassType>* SourceTypeValue = Binding.SourceNamedGrassTypes.Find(OutputName);
			const TObjectPtr<ULandscapeGrassType>* PluginTypeValue = Binding.OriginalPluginNamedGrassTypes.Find(OutputName);
			ULandscapeGrassType* SourceType = SourceTypeValue ? SourceTypeValue->Get() : nullptr;
			ULandscapeGrassType* PluginType = PluginTypeValue ? PluginTypeValue->Get() : nullptr;
			if (!IsValid(SourceType) || !IsValid(PluginType)
				|| SourceType->GetName() != PluginType->GetName()
				|| SourceType->GrassVarieties.Num() != PluginType->GrassVarieties.Num())
			{
				OutError = TEXT("Source and original plugin GrassType identity or variety counts differ for output ")
					+ OutputName.ToString();
				return false;
			}
			for (int32 VarietyIndex = 0; VarietyIndex < SourceType->GrassVarieties.Num(); ++VarietyIndex)
			{
				for (int32 SubsectionX = 0; SubsectionX < 16; ++SubsectionX)
				{
					for (int32 SubsectionY = 0; SubsectionY < 16; ++SubsectionY)
					{
						int32 Seed = 0;
						const FString SeedText = SeedTextAndValue(
							OutputName, SourceType, SectionBase, SubsectionX, SubsectionY, VarietyIndex, Seed);
						if (!ObservedSeeds.Contains(Seed))
						{
							continue;
						}
						FIMSeedCandidate& Candidate = OutCandidates.FindOrAdd(Seed).AddDefaulted_GetRef();
						Candidate.LandscapeComponent = Binding.Component;
						Candidate.OutputName = OutputName;
						Candidate.SourceType = SourceType;
						Candidate.PluginType = PluginType;
						Candidate.SeedText = SeedText;
						Candidate.Seed = Seed;
						Candidate.SectionBaseX = SectionBase.X;
						Candidate.SectionBaseY = SectionBase.Y;
						Candidate.VarietyIndex = VarietyIndex;
						Candidate.SubsectionX = SubsectionX;
						Candidate.SubsectionY = SubsectionY;
					}
				}
			}
		}
	}
	return true;
}

static TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
{
	const FVector Location = Transform.GetLocation();
	const FQuat Rotation = Transform.GetRotation();
	const FVector Scale = Transform.GetScale3D();
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LocationValues;
	LocationValues.Add(MakeShared<FJsonValueNumber>(Location.X));
	LocationValues.Add(MakeShared<FJsonValueNumber>(Location.Y));
	LocationValues.Add(MakeShared<FJsonValueNumber>(Location.Z));
	TArray<TSharedPtr<FJsonValue>> RotationValues;
	RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.X));
	RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.Y));
	RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.Z));
	RotationValues.Add(MakeShared<FJsonValueNumber>(Rotation.W));
	TArray<TSharedPtr<FJsonValue>> ScaleValues;
	ScaleValues.Add(MakeShared<FJsonValueNumber>(Scale.X));
	ScaleValues.Add(MakeShared<FJsonValueNumber>(Scale.Y));
	ScaleValues.Add(MakeShared<FJsonValueNumber>(Scale.Z));
	Row->SetArrayField(TEXT("location_world_cm"), LocationValues);
	Row->SetArrayField(TEXT("rotation_world_quaternion_xyzw"), RotationValues);
	Row->SetArrayField(TEXT("scale_world"), ScaleValues);
	return Row;
}

static TSharedRef<FJsonObject> ComponentRecordToJson(const FIMGrassComponentRecord& Record)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("instancing_random_seed"), Record.Seed);
	Row->SetStringField(TEXT("component_class"), Record.ComponentClass);
	Row->SetStringField(TEXT("component"), Record.ComponentPath);
	Row->SetStringField(TEXT("owner"), Record.OwnerPath);
	Row->SetStringField(TEXT("mesh"), Record.MeshPath);
	Row->SetBoolField(TEXT("registered"), Record.bRegistered);
	Row->SetBoolField(TEXT("transient"), Record.bTransient);
	Row->SetNumberField(TEXT("instance_count"), Record.InstanceCount);
	Row->SetNumberField(TEXT("cpu_instance_count"), Record.CPUInstanceCount);
	Row->SetNumberField(TEXT("render_instance_count"), Record.RenderInstanceCount);
	Row->SetNumberField(TEXT("synchronized_buffer_instance_count"), Record.BufferInstanceCount);
	Row->SetBoolField(TEXT("scene_proxy_present"), Record.bSceneProxyPresent);
	Row->SetBoolField(TEXT("scene_buffer_observed"), Record.bSceneBufferObserved);
	Row->SetBoolField(TEXT("instance_data_gpu_only"), Record.bGPUOnly);
	Row->SetStringField(TEXT("transform_source"), TEXT("SynchronizedRendererWorldMatrix; component and high-world offset already included"));
	Row->SetStringField(TEXT("world_transform_sha1_sorted"), Record.TransformSha1);
	Row->SetStringField(TEXT("world_transform_hash_encoding"), TEXT("IMAcousticGrassExactTransform.v1; sorted exact double scalars"));
	Row->SetNumberField(TEXT("world_transform_count"), Record.WorldTransforms.Num());
	TArray<TSharedPtr<FJsonValue>> SlotRows;
	for (const FIMGrassSlotRecord& Slot : Record.Slots)
	{
		TSharedRef<FJsonObject> SlotRow = MakeShared<FJsonObject>();
		SlotRow->SetNumberField(TEXT("slot_index"), Slot.SlotIndex);
		SlotRow->SetStringField(TEXT("mesh_slot_name"), Slot.SlotName);
		SlotRow->SetStringField(TEXT("actual_material"), Slot.MaterialPath);
		SlotRow->SetStringField(TEXT("actual_physical_material"), Slot.PhysicalMaterialPath);
		SlotRows.Add(MakeShared<FJsonValueObject>(SlotRow));
	}
	Row->SetArrayField(TEXT("material_slots"), SlotRows);
	if (Record.WorldTransforms.IsEmpty())
	{
		Row->SetStringField(TEXT("transform_summary_status"), TEXT("NOT_OBSERVED_NO_INSTANCES"));
	}
	else
	{
		const FVector ExtentMin = Record.BoundsMin;
		const FVector ExtentMax = Record.BoundsMax;
		TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetArrayField(TEXT("min_world_cm"), {
			MakeShared<FJsonValueNumber>(ExtentMin.X),
			MakeShared<FJsonValueNumber>(ExtentMin.Y),
			MakeShared<FJsonValueNumber>(ExtentMin.Z)});
		Bounds->SetArrayField(TEXT("max_world_cm"), {
			MakeShared<FJsonValueNumber>(ExtentMax.X),
			MakeShared<FJsonValueNumber>(ExtentMax.Y),
			MakeShared<FJsonValueNumber>(ExtentMax.Z)});
		Row->SetStringField(TEXT("transform_summary_status"), TEXT("OBSERVED_ALL_INSTANCE_TRANSFORMS"));
		Row->SetObjectField(TEXT("world_bounds"), Bounds);
		Row->SetObjectField(TEXT("first_sorted_world_transform"), TransformToJson(Record.WorldTransforms[0]));
	}
	return Row;
}

static bool CapturePhase(
	const FString& Name,
	const TArray<ALandscapeProxy*>& Proxies,
	TArray<FVector> Cameras,
	FIMPhaseRecord& OutPhase,
	FString& OutError)
{
	OutPhase.Name = Name;
	const double StartTime = FPlatformTime::Seconds();
	for (ALandscapeProxy* Proxy : Proxies)
	{
		if (!IsValid(Proxy) || !Proxy->ShouldGenerateGrass())
		{
			OutError = TEXT("A target Landscape proxy became invalid or stopped generating grass during phase ") + Name;
			return false;
		}
		Proxy->UpdateGrass(Cameras, true);
	}
	OutPhase.GenerationMilliseconds = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	const double CollectionStartTime = FPlatformTime::Seconds();
	TSet<UInstancedStaticMeshComponent*> SeenComponents;
	TArray<UInstancedStaticMeshComponent*> GeneratedComponents;
	for (ALandscapeProxy* Proxy : Proxies)
	{
		TArray<UInstancedStaticMeshComponent*> InstancedComponents;
		Proxy->GetComponents<UInstancedStaticMeshComponent>(InstancedComponents);
		for (UInstancedStaticMeshComponent* Component : InstancedComponents)
		{
			if (!IsValid(Component) || Component->GetClass()->GetPathName() != GrassComponentClassPath
				|| Component->GetWorld() != Proxy->GetWorld())
			{
				continue;
			}
			if (SeenComponents.Contains(Component))
			{
				OutError = TEXT("A generated Grass ISM was returned twice in phase ") + Name;
				return false;
			}
			SeenComponents.Add(Component);
			GeneratedComponents.Add(Component);
		}
	}
	TArray<FIMAcousticGrassWorldTransformSnapshot> Snapshots;
	const double ReadStartTime = FPlatformTime::Seconds();
	if (!IMReadLandscapeGrassWorldTransforms(GeneratedComponents, Snapshots, OutError))
	{
		OutError = TEXT("Grass transform read failed in phase ") + Name + TEXT(": ") + OutError;
		return false;
	}
	OutPhase.TransformReadMilliseconds = (FPlatformTime::Seconds() - ReadStartTime) * 1000.0;
	if (Snapshots.Num() != GeneratedComponents.Num())
	{
		OutError = TEXT("Grass transform batch size mismatch in phase ") + Name;
		return false;
	}
	for (int32 ComponentIndex = 0; ComponentIndex < GeneratedComponents.Num(); ++ComponentIndex)
	{
		UInstancedStaticMeshComponent* Component = GeneratedComponents[ComponentIndex];
		FIMAcousticGrassWorldTransformSnapshot& Snapshot = Snapshots[ComponentIndex];
		FIMGrassComponentRecord& Record = OutPhase.Components.AddDefaulted_GetRef();
		Record.Seed = Component->InstancingRandomSeed;
		Record.ComponentClass = Component->GetClass()->GetPathName();
		Record.ComponentPath = Component->GetPathName();
		Record.OwnerPath = ObjectPath(Component->GetOwner());
		Record.MeshPath = ObjectPath(Component->GetStaticMesh());
		Record.bRegistered = Component->IsRegistered();
		Record.bTransient = Component->HasAnyFlags(RF_Transient);
		Record.CPUInstanceCount = Snapshot.CPUInstanceCount;
		Record.RenderInstanceCount = Snapshot.RenderInstanceCount;
		Record.BufferInstanceCount = Snapshot.BufferInstanceCount;
		Record.bSceneProxyPresent = Snapshot.bSceneProxyPresent;
		Record.bSceneBufferObserved = Snapshot.bSceneBufferObserved;
		Record.bGPUOnly = Snapshot.bGPUOnly;
		Record.WorldTransforms = MoveTemp(Snapshot.WorldTransforms);
		// The exported geometry count must cover the entire renderer buffer.
		// Grass's ordinary CPU count may legitimately be zero after handoff.
		Record.InstanceCount = Record.WorldTransforms.Num();
		if (Record.InstanceCount != Record.RenderInstanceCount
			|| (Record.InstanceCount > 0 && Record.InstanceCount != Record.BufferInstanceCount))
		{
			OutError = TEXT("Grass world-transform export count mismatch: ") + Record.ComponentPath;
			return false;
		}
		if (!Record.bRegistered || !Record.bTransient || Record.Seed == 0 || Record.MeshPath.IsEmpty())
		{
			OutError = TEXT("A generated Grass ISM is unregistered, persistent, unseeded, or lacks a mesh: ") + Record.ComponentPath;
			return false;
		}
		++OutPhase.GrassComponentCount;
		OutPhase.TotalCPUInstances += Record.CPUInstanceCount;
		if (Record.InstanceCount > 0)
		{
			++OutPhase.ComponentsWithInstances;
			OutPhase.TotalInstances += Record.InstanceCount;
		}
		if (Record.RenderInstanceCount > 0)
		{
			++OutPhase.ComponentsWithRenderInstances;
			OutPhase.TotalRenderInstances += Record.RenderInstanceCount;
		}

		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const TArray<FStaticMaterial>* StaticMaterials = Mesh ? &Mesh->GetStaticMaterials() : nullptr;
		for (int32 SlotIndex = 0; SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
		{
			FIMGrassSlotRecord& Slot = Record.Slots.AddDefaulted_GetRef();
			Slot.SlotIndex = SlotIndex;
			Slot.SlotName = StaticMaterials && StaticMaterials->IsValidIndex(SlotIndex)
				? (*StaticMaterials)[SlotIndex].MaterialSlotName.ToString() : TEXT("<unresolved-slot-name>");
			UMaterialInterface* Material = Component->GetMaterial(SlotIndex);
			Slot.MaterialPath = ObjectPath(Material);
			Slot.PhysicalMaterialPath = ObjectPath(Material ? Material->GetPhysicalMaterial() : nullptr);
		}

		Record.WorldTransforms.Sort(IMCompareAcousticGrassWorldTransforms);
		if (!Record.WorldTransforms.IsEmpty())
		{
			Record.BoundsMin = Record.WorldTransforms[0].GetLocation();
			Record.BoundsMax = Record.BoundsMin;
			for (const FTransform& Transform : Record.WorldTransforms)
			{
				Record.BoundsMin = Record.BoundsMin.ComponentMin(Transform.GetLocation());
				Record.BoundsMax = Record.BoundsMax.ComponentMax(Transform.GetLocation());
			}
		}
		Record.TransformSha1 = IMHashSortedAcousticGrassWorldTransforms(Record.WorldTransforms);
	}

	OutPhase.Components.Sort([](const FIMGrassComponentRecord& A, const FIMGrassComponentRecord& B)
	{
		return A.Seed < B.Seed;
	});
	OutPhase.TransformCollectionMilliseconds = (FPlatformTime::Seconds() - CollectionStartTime) * 1000.0;
	return true;
}

static bool ValidatePhaseAssociationsAndSlots(
	FIMPhaseRecord& Phase,
	const TMap<int32, TArray<FIMSeedCandidate>>& Candidates,
	const bool bPluginPhase,
	TSharedRef<FJsonObject> PhaseJson,
	FString& OutError)
{
	TSet<int32> SeenSeeds;
	TArray<TSharedPtr<FJsonValue>> AssociationRows;
	bool bAllSlotsExact = true;
	for (FIMGrassComponentRecord& Record : Phase.Components)
	{
		if (SeenSeeds.Contains(Record.Seed))
		{
			OutError = FString::Printf(TEXT("InstancingRandomSeed %d is not a unique pairing key in phase %s."),
				Record.Seed, *Phase.Name);
			return false;
		}
		SeenSeeds.Add(Record.Seed);

		const TArray<FIMSeedCandidate>* SeedCandidates = Candidates.Find(Record.Seed);
		TSharedRef<FJsonObject> Association = MakeShared<FJsonObject>();
		Association->SetNumberField(TEXT("instancing_random_seed"), Record.Seed);
		if (!SeedCandidates || SeedCandidates->Num() != 1)
		{
			Association->SetStringField(TEXT("association_status"),
				!SeedCandidates ? TEXT("UNMATCHED_SEED") : TEXT("AMBIGUOUS_SEED"));
			Association->SetNumberField(TEXT("candidate_count"), SeedCandidates ? SeedCandidates->Num() : 0);
			AssociationRows.Add(MakeShared<FJsonValueObject>(Association));
			OutError = FString::Printf(TEXT("Grass seed %d has %d source section/output/type/variety/subsection candidates."),
				Record.Seed, SeedCandidates ? SeedCandidates->Num() : 0);
			return false;
		}

		const FIMSeedCandidate& Candidate = (*SeedCandidates)[0];
		Association->SetStringField(TEXT("association_status"), TEXT("UNIQUE_ENGINE_SEED_KEY"));
		Association->SetObjectField(TEXT("semantic_key"), SeedCandidateToJson(Candidate));
		AssociationRows.Add(MakeShared<FJsonValueObject>(Association));
		const FGrassVariety* Variety = nullptr;
		ULandscapeGrassType* ExpectedType = bPluginPhase ? Candidate.PluginType : Candidate.SourceType;
		if (IsValid(ExpectedType) && ExpectedType->GrassVarieties.IsValidIndex(Candidate.VarietyIndex))
		{
			Variety = &ExpectedType->GrassVarieties[Candidate.VarietyIndex];
		}
		if (!Variety || !IsValid(Variety->GrassMesh.Get()) || Variety->GrassMesh->GetPathName() != Record.MeshPath)
		{
			bAllSlotsExact = false;
			OutError = FString::Printf(TEXT("Grass seed %d resolved to a variety whose mesh differs from the generated component."),
				Record.Seed);
			break;
		}

		if (Record.Slots.Num() != Variety->GrassMesh->GetStaticMaterials().Num())
		{
			bAllSlotsExact = false;
			OutError = FString::Printf(TEXT("Grass seed %d has %d actual slots; its resolved variety mesh has %d."),
				Record.Seed, Record.Slots.Num(), Variety->GrassMesh->GetStaticMaterials().Num());
			break;
		}
		for (int32 SlotIndex = 0; SlotIndex < Record.Slots.Num(); ++SlotIndex)
		{
			const FIMGrassSlotRecord& Actual = Record.Slots[SlotIndex];
			UMaterialInterface* ExpectedMaterial = GetVarietyMaterial(*Variety, SlotIndex);
			const FString ExpectedPath = ObjectPath(ExpectedMaterial);
			const bool bExactMaterial = IsValid(ExpectedMaterial) && Actual.MaterialPath == ExpectedPath;
			bool bPhaseRootExact = false;
			bool bPhysicalMaterialExact = false;
			if (bPluginPhase)
			{
				UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(ExpectedMaterial);
				UPhysicalMaterial* PluginPhysicalMaterial = MIC ? MIC->GetPhysicalMaterial() : nullptr;
				bPhaseRootExact = MIC && ExpectedPath.StartsWith(PluginMaterialRoot)
					&& IsValid(PluginPhysicalMaterial)
					&& PluginPhysicalMaterial->GetOutermost()->GetName().StartsWith(PluginMaterialRoot);
				bPhysicalMaterialExact = bPhaseRootExact
					&& MIC->PhysMaterial.Get() == PluginPhysicalMaterial
					&& Actual.PhysicalMaterialPath == ObjectPath(PluginPhysicalMaterial);
			}
			else
			{
				bPhaseRootExact = ExpectedPath.StartsWith(TEXT("/Game/"));
				bPhysicalMaterialExact = bExactMaterial
					&& Actual.PhysicalMaterialPath == ObjectPath(ExpectedMaterial->GetPhysicalMaterial());
			}
			if (!bExactMaterial || !bPhaseRootExact || !bPhysicalMaterialExact
				|| Actual.SlotIndex != SlotIndex
				|| Actual.SlotName != Variety->GrassMesh->GetStaticMaterials()[SlotIndex].MaterialSlotName.ToString())
			{
				bAllSlotsExact = false;
				OutError = FString::Printf(TEXT("%s material/PhysicalMaterial slot mismatch for seed %d, slot %d."),
					bPluginPhase ? TEXT("Plugin") : TEXT("Source"), Record.Seed, SlotIndex);
				break;
			}
		}
		if (!bAllSlotsExact)
		{
			break;
		}
	}
	PhaseJson->SetArrayField(TEXT("seed_associations"), AssociationRows);
	PhaseJson->SetBoolField(TEXT("seed_keys_unique"), SeenSeeds.Num() == Phase.Components.Num());
	PhaseJson->SetBoolField(TEXT("semantic_association_complete"), AssociationRows.Num() == Phase.Components.Num());
	PhaseJson->SetBoolField(TEXT("material_and_physical_material_slots_exact"), bAllSlotsExact);
	if (Phase.TotalInstances <= 0)
	{
		OutError = FString::Printf(TEXT("Phase %s observed no positive grass instances."), *Phase.Name);
		PhaseJson->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_NO_POSITIVE_INSTANCES"));
		return false;
	}
	PhaseJson->SetStringField(TEXT("geometry_status"), bAllSlotsExact ? TEXT("OBSERVED") : TEXT("FAIL_CLOSED"));
	return bAllSlotsExact;
}

static bool ComparePhases(
	const FIMPhaseRecord& Source,
	const FIMPhaseRecord& Plugin,
	TSharedRef<FJsonObject> Comparison,
	FString& OutError)
{
	Comparison->SetNumberField(TEXT("source_component_count"), Source.GrassComponentCount);
	Comparison->SetNumberField(TEXT("plugin_component_count"), Plugin.GrassComponentCount);
	Comparison->SetNumberField(TEXT("source_positive_component_count"), Source.ComponentsWithInstances);
	Comparison->SetNumberField(TEXT("plugin_positive_component_count"), Plugin.ComponentsWithInstances);
	Comparison->SetNumberField(TEXT("source_instance_count"), Source.TotalInstances);
	Comparison->SetNumberField(TEXT("plugin_instance_count"), Plugin.TotalInstances);
	Comparison->SetNumberField(TEXT("max_position_error_cm"), 0.0);
	Comparison->SetNumberField(TEXT("max_rotation_error_radians"), 0.0);
	Comparison->SetNumberField(TEXT("max_scale_error"), 0.0);
	Comparison->SetBoolField(TEXT("geometry_equal"), false);

	TMap<int32, const FIMGrassComponentRecord*> SourceBySeed;
	TMap<int32, const FIMGrassComponentRecord*> PluginBySeed;
	for (const FIMGrassComponentRecord& Record : Source.Components)
	{
		if (SourceBySeed.Contains(Record.Seed))
		{
			OutError = FString::Printf(TEXT("Source phase seed %d is ambiguous."), Record.Seed);
			Comparison->SetStringField(TEXT("first_difference"), OutError);
			return false;
		}
		SourceBySeed.Add(Record.Seed, &Record);
	}
	for (const FIMGrassComponentRecord& Record : Plugin.Components)
	{
		if (PluginBySeed.Contains(Record.Seed))
		{
			OutError = FString::Printf(TEXT("Plugin phase seed %d is ambiguous."), Record.Seed);
			Comparison->SetStringField(TEXT("first_difference"), OutError);
			return false;
		}
		PluginBySeed.Add(Record.Seed, &Record);
	}
	if (SourceBySeed.Num() != PluginBySeed.Num())
	{
		OutError = FString::Printf(TEXT("Grass component seed counts differ: source=%d plugin=%d."),
			SourceBySeed.Num(), PluginBySeed.Num());
		Comparison->SetStringField(TEXT("first_difference"), OutError);
		return false;
	}

	double MaxPositionError = 0.0;
	double MaxRotationError = 0.0;
	double MaxScaleError = 0.0;
	int64 ExactTransformDifferences = 0;
	TSharedPtr<FJsonObject> FirstTransformDifference;
	for (const TPair<int32, const FIMGrassComponentRecord*>& Pair : SourceBySeed)
	{
		const FIMGrassComponentRecord* PluginRecord = PluginBySeed.FindRef(Pair.Key);
		const FIMGrassComponentRecord* SourceRecord = Pair.Value;
		if (!PluginRecord)
		{
			OutError = FString::Printf(TEXT("Plugin phase is missing source seed %d."), Pair.Key);
			Comparison->SetStringField(TEXT("first_difference"), OutError);
			return false;
		}
		if (SourceRecord->MeshPath != PluginRecord->MeshPath)
		{
			OutError = FString::Printf(TEXT("Mesh path differs for unique grass seed %d."), Pair.Key);
			Comparison->SetStringField(TEXT("first_difference"), OutError);
			return false;
		}
		if (SourceRecord->Slots.Num() != PluginRecord->Slots.Num()
			|| SourceRecord->WorldTransforms.Num() != PluginRecord->WorldTransforms.Num())
		{
			OutError = FString::Printf(TEXT("Slot or instance count differs for unique grass seed %d."), Pair.Key);
			Comparison->SetStringField(TEXT("first_difference"), OutError);
			return false;
		}
		for (int32 Index = 0; Index < SourceRecord->Slots.Num(); ++Index)
		{
			if (SourceRecord->Slots[Index].SlotName != PluginRecord->Slots[Index].SlotName)
			{
				OutError = FString::Printf(TEXT("Mesh material slot name differs for seed %d, slot %d."), Pair.Key, Index);
				Comparison->SetStringField(TEXT("first_difference"), OutError);
				return false;
			}
		}
		for (int32 Index = 0; Index < SourceRecord->WorldTransforms.Num(); ++Index)
		{
			const FTransform& A = SourceRecord->WorldTransforms[Index];
			const FTransform& B = PluginRecord->WorldTransforms[Index];
			const bool bExactTransform = A.Equals(B, 0.0);
			if (!bExactTransform) ++ExactTransformDifferences;
			const double PositionError = FVector::Distance(A.GetLocation(), B.GetLocation());
			const double RotationError = A.GetRotation().AngularDistance(B.GetRotation());
			const FVector ScaleDelta = A.GetScale3D() - B.GetScale3D();
			const double ScaleError = FMath::Max3(
				FMath::Abs(ScaleDelta.X), FMath::Abs(ScaleDelta.Y), FMath::Abs(ScaleDelta.Z));
			MaxPositionError = FMath::Max(MaxPositionError, PositionError);
			MaxRotationError = FMath::Max(MaxRotationError, RotationError);
			MaxScaleError = FMath::Max(MaxScaleError, ScaleError);
			if (!FirstTransformDifference.IsValid() && !bExactTransform)
			{
				FirstTransformDifference = MakeShared<FJsonObject>();
				FirstTransformDifference->SetNumberField(TEXT("seed"), Pair.Key);
				FirstTransformDifference->SetNumberField(TEXT("sorted_instance_index"), Index);
				FirstTransformDifference->SetNumberField(TEXT("position_error_cm"), PositionError);
				FirstTransformDifference->SetNumberField(TEXT("rotation_error_radians"), RotationError);
				FirstTransformDifference->SetNumberField(TEXT("scale_error"), ScaleError);
				FirstTransformDifference->SetObjectField(TEXT("source_transform"), TransformToJson(A));
				FirstTransformDifference->SetObjectField(TEXT("plugin_transform"), TransformToJson(B));
			}
		}
	}
	Comparison->SetNumberField(TEXT("max_position_error_cm"), MaxPositionError);
	Comparison->SetNumberField(TEXT("max_rotation_error_radians"), MaxRotationError);
	Comparison->SetNumberField(TEXT("max_scale_error"), MaxScaleError);
	Comparison->SetNumberField(TEXT("exact_transform_difference_count"), ExactTransformDifferences);
	Comparison->SetStringField(TEXT("transform_gate"), TEXT("FTransform::Equals with zero tolerance; derived angular-distance values are diagnostics only."));
	if (FirstTransformDifference.IsValid())
	{
		Comparison->SetObjectField(TEXT("first_transform_difference"), FirstTransformDifference.ToSharedRef());
	}
	const bool bEqual = ExactTransformDifferences == 0 && Source.TotalInstances == Plugin.TotalInstances;
	Comparison->SetBoolField(TEXT("geometry_equal"), bEqual);
	if (!bEqual)
	{
		OutError = TEXT("Source and original plugin Grass geometry fail the exact world-transform gate.");
		Comparison->SetStringField(TEXT("first_difference"),
			FirstTransformDifference.IsValid() ? TEXT("Per-instance world transform is not exactly equal.")
				: TEXT("Aggregate Grass instance counts differ."));
		return false;
	}
	Comparison->SetStringField(TEXT("first_difference"), TEXT(""));
	return true;
}

static TArray<FString> DirtyPackageNames()
{
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyPackages(DirtyPackages);
	TArray<FString> Names;
	for (UPackage* Package : DirtyPackages)
	{
		if (IsValid(Package))
		{
			Names.Add(Package->GetName());
		}
	}
	Names.Sort();
	return Names;
}

static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Json;
	for (const FString& Value : Values)
	{
		Json.Add(MakeShared<FJsonValueString>(Value));
	}
	return Json;
}

static TArray<TSharedPtr<FJsonValue>> ComponentRecordsToJson(const FIMPhaseRecord& Phase)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FIMGrassComponentRecord& Record : Phase.Components)
	{
		Rows.Add(MakeShared<FJsonValueObject>(ComponentRecordToJson(Record)));
	}
	return Rows;
}

static bool CaptureConsoleVariables(TMap<FString, FString>& OutValues, FString& OutError)
{
	static const TCHAR* Names[] = {
		TEXT("grass.Enable"),
		TEXT("grass.densityScale"),
		TEXT("grass.CullDistanceScale"),
		TEXT("grass.MaxInstancesPerComponent"),
		TEXT("grass.MaxAsyncTasks"),
		TEXT("grass.MaxCreatePerFrame"),
		TEXT("grass.TickInterval"),
		TEXT("grass.CullSubsections"),
		TEXT("grass.DisableGPUCull"),
		TEXT("grass.IgnoreExcludeBoxes"),
		TEXT("grass.DiscardDataOnLoad")
	};
	for (const TCHAR* Name : Names)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!Variable)
		{
			OutError = TEXT("A required Landscape Grass CVar is not registered: ") + FString(Name);
			return false;
		}
		OutValues.Add(Name, Variable->GetString());
	}
	return true;
}

static bool AreStringMapsEqual(const TMap<FString, FString>& A, const TMap<FString, FString>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	for (const TPair<FString, FString>& Pair : A)
	{
		const FString* OtherValue = B.Find(Pair.Key);
		if (OtherValue == nullptr || *OtherValue != Pair.Value)
		{
			return false;
		}
	}
	return true;
}

static TSharedRef<FJsonObject> ConsoleVariablesToJson(const TMap<FString, FString>& Values)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	TArray<FString> Names;
	Values.GetKeys(Names);
	Names.Sort();
	for (const FString& Name : Names)
	{
		Json->SetStringField(Name, Values.FindRef(Name));
	}
	return Json;
}

static void AddEffectiveVarietySettings(
	const TArray<FIMComponentBinding>& Bindings,
	const TMap<FString, FString>& ConsoleVariables,
	TSharedRef<FJsonObject> Report)
{
	const double DensityScale = FCString::Atod(*ConsoleVariables.FindRef(TEXT("grass.densityScale")));
	const double CullScale = FCString::Atod(*ConsoleVariables.FindRef(TEXT("grass.CullDistanceScale")));
	TSet<FString> SeenRows;
	TArray<TSharedPtr<FJsonValue>> TypeRows;
	for (const FIMComponentBinding& Binding : Bindings)
	{
		for (const FName& OutputName : SortedKeys(Binding.SourceNamedGrassTypes))
		{
			ULandscapeGrassType* Source = Binding.SourceNamedGrassTypes.FindRef(OutputName).Get();
			ULandscapeGrassType* Plugin = Binding.OriginalPluginNamedGrassTypes.FindRef(OutputName).Get();
			if (!IsValid(Source) || !IsValid(Plugin))
			{
				continue;
			}
			const FString RowKey = OutputName.ToString() + TEXT("|") + Source->GetPathName();
			if (SeenRows.Contains(RowKey))
			{
				continue;
			}
			SeenRows.Add(RowKey);
			TSharedRef<FJsonObject> TypeRow = MakeShared<FJsonObject>();
			TypeRow->SetStringField(TEXT("output_name"), OutputName.ToString());
			TypeRow->SetStringField(TEXT("source_type"), Source->GetPathName());
			TypeRow->SetStringField(TEXT("original_plugin_type"), Plugin->GetPathName());
			TypeRow->SetBoolField(TEXT("density_scaling_enabled"), Source->bEnableDensityScaling);
			TArray<TSharedPtr<FJsonValue>> VarietyRows;
			for (int32 VarietyIndex = 0; VarietyIndex < Source->GrassVarieties.Num(); ++VarietyIndex)
			{
				const FGrassVariety& Variety = Source->GrassVarieties[VarietyIndex];
				TSharedRef<FJsonObject> VarietyRow = MakeShared<FJsonObject>();
				VarietyRow->SetNumberField(TEXT("variety_index"), VarietyIndex);
				VarietyRow->SetStringField(TEXT("mesh"), ObjectPath(Variety.GrassMesh.Get()));
				VarietyRow->SetNumberField(TEXT("quality_resolved_density_per_10m2"), Variety.GetDensity());
				VarietyRow->SetNumberField(TEXT("effective_density_after_grass_density_scale"),
					Variety.GetDensity() * (Source->bEnableDensityScaling ? DensityScale : 1.0));
				VarietyRow->SetNumberField(TEXT("quality_resolved_start_cull_cm"), Variety.GetStartCullDistance());
				VarietyRow->SetNumberField(TEXT("quality_resolved_end_cull_cm"), Variety.GetEndCullDistance());
				VarietyRow->SetNumberField(TEXT("effective_start_cull_cm"), Variety.GetStartCullDistance() * CullScale);
				VarietyRow->SetNumberField(TEXT("effective_end_cull_cm"), Variety.GetEndCullDistance() * CullScale);
				VarietyRows.Add(MakeShared<FJsonValueObject>(VarietyRow));
			}
			TypeRow->SetArrayField(TEXT("varieties"), VarietyRows);
			TypeRows.Add(MakeShared<FJsonValueObject>(TypeRow));
		}
	}
	Report->SetArrayField(TEXT("effective_variety_density_and_cull"), TypeRows);
}

static bool CollectComponentBindings(
	UWorld* World,
	TArray<ALandscapeProxy*>& OutProxies,
	TArray<FIMComponentBinding>& OutBindings,
	TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>>& OutComponentsByProxy,
	TArray<TSharedPtr<FJsonValue>>& OutPreconditions,
	FString& OutError)
{
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Proxy = *It;
		if (!IsValid(Proxy) || Proxy->GetWorld() != World)
		{
			continue;
		}
		TArray<ULandscapeComponent*> LandscapeComponents;
		Proxy->GetComponents<ULandscapeComponent>(LandscapeComponents);
		TArray<FIMComponentBinding> ProxyBindings;
		for (ULandscapeComponent* Component : LandscapeComponents)
		{
			if (!IsValid(Component) || Component->GetWorld() != World)
			{
				continue;
			}
			UMaterialInterface* BaseLandscapeMaterial = Component->GetLandscapeMaterial();
			if (!IsValid(BaseLandscapeMaterial))
			{
				continue;
			}
			const TMap<FName, TObjectPtr<ULandscapeGrassType>>& SourceTypes =
				BaseLandscapeMaterial->GetCachedExpressionData().NamedGrassTypes;
			if (SourceTypes.IsEmpty())
			{
				continue;
			}

			FIMComponentBinding& Binding = ProxyBindings.AddDefaulted_GetRef();
			Binding.Proxy = Proxy;
			Binding.Component = Component;
			Binding.SourceNamedGrassTypes = SourceTypes;
			Binding.OriginalPluginNamedGrassTypes = Component->GetNamedGrassTypes();
			const FLandscapeComponentGrassData& Data = *Component->GrassData;
			TSharedRef<FJsonObject> Precondition = MakeShared<FJsonObject>();
			Precondition->SetStringField(TEXT("component"), Component->GetPathName());
			Precondition->SetStringField(TEXT("proxy"), Proxy->GetPathName());
			Precondition->SetNumberField(TEXT("observation_platform_seconds"), FPlatformTime::Seconds());
			Precondition->SetNumberField(TEXT("observation_gt_frame"), double(GFrameCounter));
			Precondition->SetNumberField(TEXT("grassdata_address"), double(reinterpret_cast<UPTRINT>(&Data)));
			Precondition->SetBoolField(TEXT("registered"), Component->IsRegistered());
			Precondition->SetBoolField(TEXT("should_generate_grass"), Proxy->ShouldGenerateGrass());
			Precondition->SetBoolField(TEXT("can_render_grass_map"), Component->CanRenderGrassMap());
			Precondition->SetNumberField(TEXT("num_elements"), Data.NumElements);
			Precondition->SetNumberField(TEXT("height_weight_bytes"), Data.HeightWeightData.Num());
			Precondition->SetNumberField(TEXT("stored_generation_hash"), Data.GenerationHash);
			Precondition->SetNumberField(TEXT("current_generation_hash"), Component->ComputeGrassMapGenerationHash());
			TArray<TSharedPtr<FJsonValue>> KeyRows;
			for (const FName& Key : SortedKeys(SourceTypes))
			{
				TSharedRef<FJsonObject> KeyRow = MakeShared<FJsonObject>();
				const int32* Offset = Data.WeightOffsets.Find(Key);
				KeyRow->SetStringField(TEXT("name"), Key.ToString());
				KeyRow->SetBoolField(TEXT("present"), Offset != nullptr);
				KeyRow->SetNumberField(TEXT("offset"), Offset ? *Offset : INDEX_NONE);
				KeyRows.Add(MakeShared<FJsonValueObject>(KeyRow));
			}
			Precondition->SetArrayField(TEXT("source_weight_keys"), KeyRows);
			Precondition->SetStringField(TEXT("absent_key_semantics"), TEXT("UE may strip all-zero weights. Absence is not an invalid GrassData verdict; validity/current generation hash and overall positive instances remain separate gates."));
			OutPreconditions.Add(MakeShared<FJsonValueObject>(Precondition));
			if (!Component->IsRegistered())
			{
				OutError = TEXT("A task-map LandscapeComponent with source GrassTypes is not registered: ") + Component->GetPathName();
				return false;
			}
			if (Binding.SourceNamedGrassTypes.Num() != Binding.OriginalPluginNamedGrassTypes.Num())
			{
				OutError = TEXT("Source base-material and original plugin NamedGrassTypes key counts differ: ") + Component->GetPathName();
				return false;
			}
			for (const FName& Key : SortedKeys(Binding.SourceNamedGrassTypes))
			{
				ULandscapeGrassType* SourceType = Binding.SourceNamedGrassTypes.FindRef(Key).Get();
				ULandscapeGrassType* PluginType = Binding.OriginalPluginNamedGrassTypes.FindRef(Key).Get();
				if (!IsValid(SourceType) || !IsValid(PluginType)
					|| SourceType == PluginType
					|| SourceType->GetOutermost()->GetName().StartsWith(TEXT("/Game/")) == false
					|| !PluginType->GetOutermost()->GetName().StartsWith(PluginGrassTypeRoot)
					|| SourceType->GetName() != PluginType->GetName()
					|| SourceType->GrassVarieties.Num() != PluginType->GrassVarieties.Num())
				{
					OutError = TEXT("Source and original plugin GrassType binding failed exact identity checks for ")
						+ Component->GetPathName() + TEXT(" / ") + Key.ToString();
					return false;
				}
				const FLandscapeComponentGrassData& GrassData = *Component->GrassData;
				const int32* WeightOffset = GrassData.WeightOffsets.Find(Key);
				const bool bGrassDataValid = GrassData.NumElements >= 0;
				// InitializeFrom can strip all-zero named weights. A current empty
				// component is legal; the complete phase must still have instances.
				const bool bWeightRangeValid = !WeightOffset || (GrassData.NumElements > 0
					&& int64(*WeightOffset) >= int64(GrassData.NumElements) * int64(sizeof(uint16))
					&& int64(*WeightOffset) + GrassData.NumElements <= GrassData.HeightWeightData.Num());
				if (!bGrassDataValid || !bWeightRangeValid)
				{
					OutError = FString::Printf(
						TEXT("GrassData precondition failed for %s / %s: valid=%d elements=%d weight_range_valid=%d offset=%d."),
						*Component->GetPathName(), *Key.ToString(),
						bGrassDataValid ? 1 : 0,
						GrassData.NumElements,
						bWeightRangeValid ? 1 : 0,
						WeightOffset ? *WeightOffset : INDEX_NONE);
					return false;
				}
			}
			if (!Component->CanRenderGrassMap())
			{
				OutError = TEXT("CanRenderGrassMap is false for an existing source GrassData component; generation was not observed: ")
					+ Component->GetPathName();
				return false;
			}
			if (Component->ComputeGrassMapGenerationHash() != Component->GrassData->GenerationHash)
			{
				OutError = FString::Printf(
					TEXT("GrassData generation hash is stale on %s: stored=%u current=%u; BuildGrassMaps is intentionally not called."),
					*Component->GetPathName(), Component->GrassData->GenerationHash,
					Component->ComputeGrassMapGenerationHash());
				return false;
			}
			OutComponentsByProxy.FindOrAdd(Proxy).Add(Component);
		}
		if (!ProxyBindings.IsEmpty())
		{
			if (!Proxy->ShouldGenerateGrass())
			{
				OutError = TEXT("ShouldGenerateGrass is false for a Landscape proxy with task-map GrassTypes: ") + Proxy->GetPathName();
				return false;
			}
			OutProxies.Add(Proxy);
			OutBindings.Append(ProxyBindings);
		}
	}
	if (OutProxies.IsEmpty() || OutBindings.IsEmpty())
	{
		OutError = TEXT("The task preview world contains no registered Landscape source GrassTypes to probe.");
		return false;
	}
	OutProxies.Sort([](const ALandscapeProxy& A, const ALandscapeProxy& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	return true;
}

static bool TrackAndHashPackages(
	const TArray<FIMComponentBinding>& Bindings,
	UWorld* World,
	TMap<UPackage*, FIMTrackedPackage>& OutPackages,
	FString& OutError)
{
	TSet<UPackage*> Packages;
	AddObjectPackage(World, Packages);
	for (const FIMComponentBinding& Binding : Bindings)
	{
		AddObjectPackage(Binding.Proxy, Packages);
		AddObjectPackage(Binding.Component, Packages);
		UMaterialInterface* BaseLandscapeMaterial = Binding.Component->GetLandscapeMaterial();
		AddMaterialPackages(BaseLandscapeMaterial, Packages);
		for (const FName& Key : SortedKeys(Binding.SourceNamedGrassTypes))
		{
			ULandscapeGrassType* SourceType = Binding.SourceNamedGrassTypes.FindRef(Key).Get();
			ULandscapeGrassType* PluginType = Binding.OriginalPluginNamedGrassTypes.FindRef(Key).Get();
			AddObjectPackage(SourceType, Packages);
			AddObjectPackage(PluginType, Packages);
			if (!IsValid(SourceType) || !IsValid(PluginType))
			{
				continue;
			}
			for (const FGrassVariety& Variety : SourceType->GrassVarieties)
			{
				AddObjectPackage(Variety.GrassMesh.Get(), Packages);
				AddObjectPackage(Variety.GrassMesh.Get() ? Variety.GrassMesh->GetOutermost() : nullptr, Packages);
				if (IsValid(Variety.GrassMesh.Get()))
				{
					for (int32 Slot = 0; Slot < Variety.GrassMesh->GetStaticMaterials().Num(); ++Slot)
					{
						AddMaterialPackages(GetVarietyMaterial(Variety, Slot), Packages);
					}
				}
			}
			for (const FGrassVariety& Variety : PluginType->GrassVarieties)
			{
				AddObjectPackage(Variety.GrassMesh.Get(), Packages);
				if (IsValid(Variety.GrassMesh.Get()))
				{
					for (int32 Slot = 0; Slot < Variety.GrassMesh->GetStaticMaterials().Num(); ++Slot)
					{
						AddMaterialPackages(GetVarietyMaterial(Variety, Slot), Packages);
					}
				}
			}
		}
	}

	TArray<UPackage*> SortedPackages = Packages.Array();
	SortedPackages.Sort([](const UPackage& A, const UPackage& B)
	{
		return A.GetName() < B.GetName();
	});
	for (UPackage* Package : SortedPackages)
	{
		FIMTrackedPackage& Row = OutPackages.Add(Package);
		Row.Package = Package;
		Row.bDirtyBefore = Package->IsDirty();
		Row.bSourceGamePackage = Package->GetName().StartsWith(TEXT("/Game/"));
		if (Row.bSourceGamePackage)
		{
			if (!HashPackageFile(Package, Row.Filename, Row.Sha1Before, Row.BytesBefore, OutError))
			{
				return false;
			}
			Row.bSha1BeforeRead = true;
		}
	}
	return true;
}

static bool RecheckTrackedPackages(TMap<UPackage*, FIMTrackedPackage>& Packages, FString& OutError)
{
	bool bAllUnchanged = true;
	TArray<UPackage*> Keys;
	Packages.GetKeys(Keys);
	Keys.Sort([](const UPackage& A, const UPackage& B) { return A.GetName() < B.GetName(); });
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (UPackage* Package : Keys)
	{
		FIMTrackedPackage& State = Packages.FindChecked(Package);
		if (!IsValid(Package))
		{
			OutError = TEXT("A tracked package was invalidated during the probe.");
			return false;
		}
		const bool bDirtyAfter = Package->IsDirty();
		bAllUnchanged &= State.bDirtyBefore == bDirtyAfter;
		if (State.bSourceGamePackage)
		{
			FString HashError;
			State.bSha1AfterRead = HashPackageFile(Package, State.Filename, State.Sha1After, State.BytesAfter, HashError);
			if (!State.bSha1AfterRead)
			{
				OutError = HashError;
				return false;
			}
			bAllUnchanged &= State.Sha1Before == State.Sha1After && State.BytesBefore == State.BytesAfter;
		}
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), Package->GetName());
		Row->SetBoolField(TEXT("dirty_before"), State.bDirtyBefore);
		Row->SetBoolField(TEXT("dirty_after"), bDirtyAfter);
		Row->SetBoolField(TEXT("source_game_package"), State.bSourceGamePackage);
		Row->SetStringField(TEXT("filename"), State.Filename);
		Row->SetStringField(TEXT("sha1_before"), State.Sha1Before);
		Row->SetStringField(TEXT("sha1_after"), State.Sha1After);
		Row->SetNumberField(TEXT("bytes_before"), State.BytesBefore);
		Row->SetNumberField(TEXT("bytes_after"), State.BytesAfter);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	if (!bAllUnchanged && OutError.IsEmpty())
	{
		OutError = TEXT("A tracked package dirty flag or source /Game package hash changed during the grass geometry probe.");
	}
	return bAllUnchanged;
}

static bool WriteReport(const FString& Filename, const TSharedRef<FJsonObject>& Report, FString& OutError)
{
	const FString Directory = FPaths::GetPath(Filename);
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		OutError = TEXT("Could not create Grass generation evidence directory: ") + Directory;
		return false;
	}
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Report, Writer))
	{
		Writer->Close();
		OutError = TEXT("Could not serialize Grass generation evidence.");
		return false;
	}
	Writer->Close();
	if (!FFileHelper::SaveStringToFile(Json, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("Could not write Grass generation evidence: ") + Filename;
		return false;
	}
	return true;
}

static bool GetPreviewBakeRegion(UWorld* World, FBox& Region, FString& Error)
{
	AIMAcousticBakeVolume* Volume = nullptr;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
	{
		if (Volume)
		{
			Error = TEXT("Region plan observation requires exactly one preview bake volume.");
			return false;
		}
		Volume = *It;
	}
	if (!Volume || !Volume->BakeBounds)
	{
		Error = TEXT("Region plan observation has no preview bake bounds.");
		return false;
	}
	Region = Volume->BakeBounds->Bounds.GetBox();
	if (!Region.IsValid || Region.Min.ContainsNaN() || Region.Max.ContainsNaN())
	{
		Error = TEXT("Region plan observation has invalid preview bounds.");
		return false;
	}
	return true;
}

static bool FilterPhaseToRegion(const FIMPhaseRecord& Phase, const FBox& Region,
	FIMPhaseRecord& Result, FString& Error)
{
	Result = {};
	Result.Name = Phase.Name + TEXT("_region_intersection");
	for (const FIMGrassComponentRecord& FullRecord : Phase.Components)
	{
		UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *FullRecord.MeshPath);
		if (!Mesh)
		{
			Error = TEXT("Coverage filter cannot resolve the generated mesh: ") + FullRecord.MeshPath;
			return false;
		}
		FIMGrassComponentRecord Record = FullRecord;
		Record.WorldTransforms.RemoveAll([&](const FTransform& Transform)
			{ return !Mesh->GetBounds().TransformBy(Transform).GetBox().Intersect(Region); });
		if (Record.WorldTransforms.IsEmpty()) continue;
		Record.WorldTransforms.Shrink();
		Record.InstanceCount = Record.WorldTransforms.Num();
		Record.TransformSha1 = IMHashSortedAcousticGrassWorldTransforms(Record.WorldTransforms);
		Result.TotalInstances += Record.InstanceCount;
		++Result.ComponentsWithInstances;
		++Result.GrassComponentCount;
		// Render/CPU fields retain the original full-component observation.
		// This filtered record is used only by the region transform comparison.
		Result.Components.Add(MoveTemp(Record));
	}
	return true;
}

static bool ObserveGrassBakePlan(UWorld* World, const FIMPhaseRecord& Phase,
	const TMap<int32, TArray<FIMSeedCandidate>>& SeedCandidates,
	const TSharedRef<FJsonObject>& Report, FString& Error)
{
	TSharedRef<FJsonObject> Observation = MakeShared<FJsonObject>();
	Report->SetObjectField(TEXT("static_region_plan_observation"), Observation);
	Observation->SetStringField(TEXT("scope"), TEXT("EXISTING_CAMERA3_OBSERVED_INSTANCES_ONLY"));
	FBox Region(ForceInit);
	if (!GetPreviewBakeRegion(World, Region, Error)) return false;
	FIMAcousticGrassBakePlan Plan;
	const double Started = FPlatformTime::Seconds();
	const bool bPlanned = IMBuildAcousticGrassBakePlan(World, Region, Plan, Error);
	Observation->SetNumberField(TEXT("planning_milliseconds"), (FPlatformTime::Seconds() - Started) * 1000.0);
	Observation->SetStringField(TEXT("source_input_sha1"), Plan.SourceInputSha1);
	Observation->SetNumberField(TEXT("selected_source_count"), Plan.Sources.Num());
	Observation->SetNumberField(TEXT("finite_camera_count"), Plan.Cameras.Num());
	if (!bPlanned)
	{
		Observation->SetStringField(TEXT("status"), TEXT("FAIL_CLOSED_PLAN_INPUT"));
		Observation->SetStringField(TEXT("error"), Error);
		return false;
	}
	TSet<ULandscapeComponent*> SelectedSources;
	TMap<ULandscapeComponent*, FBox> OriginBounds;
	for (const FIMAcousticGrassBakeSource& Source : Plan.Sources)
	{
		SelectedSources.Add(Source.Component);
		OriginBounds.Add(Source.Component, Source.OriginWorldBounds);
	}
	int64 IncludedInstances = 0;
	int64 MissingSourceInstances = 0;
	int64 OriginOutsideEnvelope = 0;
	for (const FIMGrassComponentRecord& Record : Phase.Components)
	{
		if (Record.WorldTransforms.IsEmpty()) continue;
		const TArray<FIMSeedCandidate>* Candidates = SeedCandidates.Find(Record.Seed);
		UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *Record.MeshPath);
		if (!Candidates || Candidates->Num() != 1 || !Mesh)
		{
			Error = TEXT("Region plan observation cannot resolve the existing instance source/mesh: ") + Record.ComponentPath;
			Observation->SetStringField(TEXT("status"), TEXT("FAIL_CLOSED_OBSERVATION_ASSOCIATION"));
			Observation->SetStringField(TEXT("error"), Error);
			return false;
		}
		ULandscapeComponent* Source = (*Candidates)[0].LandscapeComponent;
		for (const FTransform& Transform : Record.WorldTransforms)
		{
			if (!Mesh->GetBounds().TransformBy(Transform).GetBox().Intersect(Region)) continue;
			++IncludedInstances;
			if (!SelectedSources.Contains(Source)) ++MissingSourceInstances;
			if (const FBox* Envelope = OriginBounds.Find(Source))
			{
				// Render matrices contain float rounding. This diagnostic is not
				// a zero-tolerance gate for a height/XY boundary sample.
				if (!Envelope->IsInsideOrOn(Transform.GetLocation())) ++OriginOutsideEnvelope;
			}
		}
	}
	Observation->SetNumberField(TEXT("region_intersecting_observed_instance_count"), double(IncludedInstances));
	Observation->SetNumberField(TEXT("missing_selected_source_instance_count"), double(MissingSourceInstances));
	Observation->SetNumberField(TEXT("origin_outside_exact_envelope_diagnostic_count"), double(OriginOutsideEnvelope));
	const bool bPass = IncludedInstances > 0 && MissingSourceInstances == 0;
	Observation->SetStringField(TEXT("status"), bPass
		? TEXT("PASS_EXISTING_OBSERVED_SUBSET_SELECTION") : TEXT("FAIL_CLOSED_OBSERVED_SUBSET_SELECTION"));
	if (!bPass) Error = TEXT("Static region planner omitted an observed intersecting source or observed no intersecting instances.");
	return bPass;
}

class FIMGrassGenerationGeometryProbeCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMGrassGenerationGeometryProbeCommand(FAutomationTestBase* InTest)
		: Test(InTest)
		, RunId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
		, ReportPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialDrive"),
			FString::Printf(TEXT("grass_generation_%s.json"), *RunId)))
	{
		Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("IMAcousticGrassGenerationGeometryProbe.v6"));
		Report->SetStringField(TEXT("test"), TEXT("IceMoon.AcousticField.Rural.GrassGenerationGeometryProbe"));
		Report->SetStringField(TEXT("run_id"), RunId);
		Report->SetStringField(TEXT("evidence_path"), ReportPath);
		Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED"));
		Report->SetStringField(TEXT("audio_status"), TEXT("NOT_ASSESSED"));
		Report->SetStringField(TEXT("formal_gpu_performance_status"), TEXT("NOT_ASSESSED"));
		Report->SetBoolField(TEXT("maps_or_assets_saved"), false);
		Report->SetBoolField(TEXT("build_grass_maps_called"), false);
		Report->SetStringField(TEXT("data_scope"),
			TEXT("Landscape GrassData validity and source/plugin generated instance geometry; no product audio, Bake acceptance, or formal GPU performance claim."));
	}

	virtual bool Update() override
	{
		if (bFinished)
		{
			return true;
		}
		bFinished = true;
		bool bSuccess = false;
		FString Error;
		if (!IsInGameThread())
		{
			Error = TEXT("Grass generation probe must run in its single game-thread latent callback.");
			return Finish(false, Error);
		}
		Report->SetBoolField(TEXT("game_thread_callback"), true);

		const TArray<FString> DirtyBefore = DirtyPackageNames();
		Report->SetArrayField(TEXT("dirty_packages_before"), StringArrayToJson(DirtyBefore));
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		Report->SetBoolField(TEXT("editor_present"), GEditor != nullptr);
		Report->SetBoolField(TEXT("play_world_absent"), GEditor && GEditor->PlayWorld == nullptr);
		Report->SetBoolField(TEXT("editor_world_valid"), IsValid(World));
		if (IsValid(World))
		{
			Report->SetStringField(TEXT("editor_world_package"), World->GetOutermost()->GetName());
		Report->SetNumberField(TEXT("editor_world_type"), static_cast<int32>(World->WorldType));
			Report->SetBoolField(TEXT("target_preview_world_exact"), World->GetOutermost()->GetName() == TestMap);
			Report->SetBoolField(TEXT("world_package_dirty_before"), World->GetOutermost()->IsDirty());
			const FString StartupFilename = FPackageName::LongPackageNameToFilename(
				World->GetOutermost()->GetName(), FPackageName::GetMapPackageExtension());
			Report->SetStringField(TEXT("entry_editor_map_filename"), FPaths::ConvertRelativePathToFull(StartupFilename));
			Report->SetBoolField(TEXT("entry_editor_map_recoverable"), FPaths::FileExists(StartupFilename));
			Report->SetStringField(TEXT("map_restore_action"), TEXT("NO_MAP_SWITCHED; entry Editor map remains loaded."));
		}
		Report->SetBoolField(TEXT("can_ever_render"), FApp::CanEverRender());
		Report->SetBoolField(TEXT("null_rhi"), GUsingNullRHI);
		Report->SetBoolField(TEXT("dynamic_rhi_available"), GDynamicRHI != nullptr);

		if (!GEditor || !World || World->WorldType != EWorldType::Editor || GEditor->PlayWorld
			|| World->GetOutermost()->GetName() != TestMap
			|| World->GetOutermost()->IsDirty() || !DirtyBefore.IsEmpty()
			|| !FApp::CanEverRender() || GUsingNullRHI || GDynamicRHI == nullptr)
		{
			Error = TEXT("Preconditions failed: require the clean exact task preview map in the normal RHI Editor with no PIE world and a recoverable entry map.");
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_PRECONDITION_FAILED"));
			return Finish(false, Error);
		}

		AActor* Camera3 = nullptr;
		int32 CameraCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetFName() == FName(TEXT("Camera3"))
				&& (*It)->GetClass()->GetPathName() == TEXT("/Script/CinematicCamera.CineCameraActor"))
			{
				Camera3 = *It;
				++CameraCount;
			}
		}
		const bool bCameraExact = CameraCount == 1 && IsValid(Camera3)
			&& FVector::Distance(Camera3->GetActorLocation(), Camera3Location) <= CameraToleranceCm;
		Report->SetNumberField(TEXT("camera3_match_count"), CameraCount);
		if (IsValid(Camera3))
		{
			TArray<TSharedPtr<FJsonValue>> CameraLocation;
			CameraLocation.Add(MakeShared<FJsonValueNumber>(Camera3->GetActorLocation().X));
			CameraLocation.Add(MakeShared<FJsonValueNumber>(Camera3->GetActorLocation().Y));
			CameraLocation.Add(MakeShared<FJsonValueNumber>(Camera3->GetActorLocation().Z));
			Report->SetArrayField(TEXT("camera3_actual_location_cm"), CameraLocation);
		}
		Report->SetArrayField(TEXT("camera3_expected_location_cm"), {
			MakeShared<FJsonValueNumber>(Camera3Location.X),
			MakeShared<FJsonValueNumber>(Camera3Location.Y),
			MakeShared<FJsonValueNumber>(Camera3Location.Z)});
		Report->SetBoolField(TEXT("camera3_exact"), bCameraExact);
		if (!bCameraExact)
		{
			Error = TEXT("Camera3 did not match the frozen actor identity and complete double-precision position.");
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_CAMERA_PRECONDITION_FAILED"));
			return Finish(false, Error);
		}

		if (!CaptureConsoleVariables(ConsoleVariablesBefore, Error))
		{
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_CVAR_PRECONDITION_FAILED"));
			return Finish(false, Error);
		}
		Report->SetObjectField(TEXT("grass_cvars_before"), ConsoleVariablesToJson(ConsoleVariablesBefore));
		if (FCString::Atoi(*ConsoleVariablesBefore.FindRef(TEXT("grass.Enable"))) == 0)
		{
			Error = TEXT("grass.Enable is zero; no generation was attempted.");
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_GRASS_DISABLED"));
			return Finish(false, Error);
		}

		TArray<ALandscapeProxy*> Proxies;
		TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>> ComponentsByProxy;
		TArray<TSharedPtr<FJsonValue>> Preconditions;
		const bool bBindingsValid = CollectComponentBindings(World, Proxies, Bindings, ComponentsByProxy, Preconditions, Error);
		Report->SetArrayField(TEXT("grassdata_preconditions_observed"), Preconditions);
		if (!bBindingsValid)
		{
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_GRASSDATA_PRECONDITION_FAILED"));
			return Finish(false, Error);
		}
		Report->SetNumberField(TEXT("landscape_proxy_count"), Proxies.Num());
		Report->SetNumberField(TEXT("landscape_component_count"), Bindings.Num());
		TArray<TSharedPtr<FJsonValue>> ProxyRows;
		for (ALandscapeProxy* Proxy : Proxies)
		{
			TSharedRef<FJsonObject> ProxyRow = MakeShared<FJsonObject>();
			ProxyRow->SetStringField(TEXT("proxy"), Proxy->GetPathName());
			ProxyRow->SetBoolField(TEXT("should_generate_grass"), Proxy->ShouldGenerateGrass());
			ProxyRow->SetNumberField(TEXT("registered_component_count"), ComponentsByProxy.FindChecked(Proxy).Num());
			ProxyRows.Add(MakeShared<FJsonValueObject>(ProxyRow));
		}
		Report->SetArrayField(TEXT("landscape_proxies"), ProxyRows);

		const FVector CameraPosition = Camera3->GetActorLocation();
		const TArray<FVector> Cameras{CameraPosition};
		Report->SetArrayField(TEXT("update_grass_camera_cm"), {
			MakeShared<FJsonValueNumber>(CameraPosition.X),
			MakeShared<FJsonValueNumber>(CameraPosition.Y),
			MakeShared<FJsonValueNumber>(CameraPosition.Z)});
		Report->SetStringField(TEXT("grassmap_policy"),
			TEXT("Existing GrassData only. Each relevant component must be valid, current by ComputeGrassMapGenerationHash and renderable. Missing named weights may be stripped all-zero data; present ranges are checked. Both complete phases must contain positive instances. BuildGrassMaps is never called."));

		FString InitialGrassDataHash;
		if (!HashGrassData(Bindings, InitialGrassDataHash, Error))
		{
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_GRASSDATA_HASH_FAILED"));
			return Finish(false, Error);
		}
		Report->SetStringField(TEXT("source_grassdata_sha1_before"), InitialGrassDataHash);
		for (const FIMComponentBinding& Binding : Bindings)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("landscape_component"), Binding.Component->GetPathName());
			Row->SetStringField(TEXT("landscape_proxy"), Binding.Proxy->GetPathName());
			Row->SetBoolField(TEXT("should_generate_grass"), Binding.Proxy->ShouldGenerateGrass());
			Row->SetBoolField(TEXT("registered"), Binding.Component->IsRegistered());
			Row->SetBoolField(TEXT("can_render_grass_map"), Binding.Component->CanRenderGrassMap());
			const FLandscapeComponentGrassData& GrassData = *Binding.Component->GrassData;
			Row->SetBoolField(TEXT("grass_data_valid"), GrassData.NumElements >= 0);
			Row->SetBoolField(TEXT("grass_data_has_weight_data"), GrassData.WeightOffsets.Num() > 0);
			Row->SetNumberField(TEXT("grass_data_num_elements"), GrassData.NumElements);
			Row->SetNumberField(TEXT("grass_data_generation_hash"), GrassData.GenerationHash);
			Row->SetNumberField(TEXT("grass_map_generation_hash_current"), Binding.Component->ComputeGrassMapGenerationHash());
			Row->SetStringField(TEXT("source_named_grassdata_hash"), NamedGrassTypesHash(Binding.SourceNamedGrassTypes));
			Row->SetStringField(TEXT("original_plugin_named_grassdata_hash"), NamedGrassTypesHash(Binding.OriginalPluginNamedGrassTypes));
			TArray<TSharedPtr<FJsonValue>> WeightRows;
			for (const FName& Key : SortedKeys(Binding.SourceNamedGrassTypes))
			{
				TSharedRef<FJsonObject> WeightRow = MakeShared<FJsonObject>();
				WeightRow->SetStringField(TEXT("expected_weight_key"), Key.ToString());
				const int32* Offset = GrassData.WeightOffsets.Find(Key);
				WeightRow->SetBoolField(TEXT("grass_data_contains_key"), Offset != nullptr);
				WeightRow->SetNumberField(TEXT("weight_offset"), Offset ? *Offset : INDEX_NONE);
				ULandscapeGrassType* SourceType = Binding.SourceNamedGrassTypes.FindRef(Key).Get();
				ULandscapeGrassType* PluginType = Binding.OriginalPluginNamedGrassTypes.FindRef(Key).Get();
				WeightRow->SetStringField(TEXT("source_grass_type"), ObjectPath(SourceType));
				WeightRow->SetStringField(TEXT("original_plugin_grass_type"), ObjectPath(PluginType));
				WeightRows.Add(MakeShared<FJsonValueObject>(WeightRow));
			}
			Row->SetArrayField(TEXT("expected_grass_weight_keys"), WeightRows);
			Row->SetObjectField(TEXT("source_base_material_named_grass_types"), NamedGrassTypesToJson(Binding.SourceNamedGrassTypes));
			Row->SetObjectField(TEXT("original_plugin_named_grass_types"), NamedGrassTypesToJson(Binding.OriginalPluginNamedGrassTypes));
			BindingRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Report->SetArrayField(TEXT("landscape_component_preconditions"), BindingRows);
		AddEffectiveVarietySettings(Bindings, ConsoleVariablesBefore, Report.ToSharedRef());

		TSharedPtr<FJsonObject> SourceInventory;
		FString InventoryError;
		const bool bInventoryObserved = IMInspectLandscapeGrassMaterialSources(World, SourceInventory, InventoryError);
		Report->SetBoolField(TEXT("source_material_inventory_observed"), bInventoryObserved);
		if (SourceInventory.IsValid())
		{
			Report->SetObjectField(TEXT("source_material_inventory"), SourceInventory.ToSharedRef());
		}
		if (!bInventoryObserved)
		{
			Error = TEXT("Read-only base-material source inventory failed: ") + InventoryError;
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_SOURCE_INVENTORY_FAILED"));
			return Finish(false, Error);
		}

		if (!TrackAndHashPackages(Bindings, World, TrackedPackages, Error))
		{
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_SOURCE_PACKAGE_HASH_FAILED"));
			return Finish(false, Error);
		}
		TArray<TSharedPtr<FJsonValue>> PackageRowsBefore;
		for (const TPair<UPackage*, FIMTrackedPackage>& Pair : TrackedPackages)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package"), Pair.Key->GetName());
			Row->SetBoolField(TEXT("dirty_before"), Pair.Value.bDirtyBefore);
			Row->SetBoolField(TEXT("source_game_package"), Pair.Value.bSourceGamePackage);
			Row->SetStringField(TEXT("filename"), Pair.Value.Filename);
			Row->SetStringField(TEXT("sha1_before"), Pair.Value.Sha1Before);
			Row->SetNumberField(TEXT("bytes_before"), Pair.Value.BytesBefore);
			PackageRowsBefore.Add(MakeShared<FJsonValueObject>(Row));
		}
		Report->SetArrayField(TEXT("tracked_packages_before"), PackageRowsBefore);

		for (FIMComponentBinding& Binding : Bindings)
		{
			Binding.Component->SetNamedGrassTypes(Binding.SourceNamedGrassTypes);
		}
		for (ALandscapeProxy* Proxy : Proxies)
		{
			Proxy->FlushGrassComponents(&ComponentsByProxy.FindChecked(Proxy), false);
		}
		FIMPhaseRecord SourcePhase;
		if (!CapturePhase(TEXT("source_named_grass_types"), Proxies, Cameras, SourcePhase, Error))
		{
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_SOURCE_PHASE_FAILED"));
			Report->SetObjectField(TEXT("source_phase"), PhaseToJson(SourcePhase));
			RestorePluginCachesAndFlush(Proxies, ComponentsByProxy, Bindings);
			return Finish(false, Error);
		}
		TSharedRef<FJsonObject> SourcePhaseJson = PhaseToJson(SourcePhase);

		for (FIMComponentBinding& Binding : Bindings)
		{
			Binding.Component->SetNamedGrassTypes(Binding.OriginalPluginNamedGrassTypes);
		}
		for (ALandscapeProxy* Proxy : Proxies)
		{
			Proxy->FlushGrassComponents(&ComponentsByProxy.FindChecked(Proxy), false);
		}
		FIMPhaseRecord PluginPhase;
		if (!CapturePhase(TEXT("original_plugin_named_grass_types"), Proxies, Cameras, PluginPhase, Error))
		{
			Report->SetStringField(TEXT("geometry_status"), TEXT("NOT_OBSERVED_PLUGIN_PHASE_FAILED"));
			Report->SetObjectField(TEXT("source_phase"), SourcePhaseJson);
			Report->SetObjectField(TEXT("plugin_phase"), PhaseToJson(PluginPhase));
			RestorePluginCachesAndFlush(Proxies, ComponentsByProxy, Bindings);
			return Finish(false, Error);
		}
		TSharedRef<FJsonObject> PluginPhaseJson = PhaseToJson(PluginPhase);

		TSet<int32> ObservedSeeds;
		for (const FIMGrassComponentRecord& Record : SourcePhase.Components) ObservedSeeds.Add(Record.Seed);
		for (const FIMGrassComponentRecord& Record : PluginPhase.Components) ObservedSeeds.Add(Record.Seed);
		TMap<int32, TArray<FIMSeedCandidate>> SeedCandidates;
		if (!BuildSeedAssociations(Bindings, ObservedSeeds, SeedCandidates, Error))
		{
			Report->SetStringField(TEXT("geometry_status"), TEXT("FAIL_CLOSED_SEED_ASSOCIATION"));
			Report->SetObjectField(TEXT("source_phase"), SourcePhaseJson);
			Report->SetObjectField(TEXT("plugin_phase"), PluginPhaseJson);
			RestorePluginCachesAndFlush(Proxies, ComponentsByProxy, Bindings);
			return Finish(false, Error);
		}

		const bool bSourcePhaseValid = ValidatePhaseAssociationsAndSlots(
			SourcePhase, SeedCandidates, false, SourcePhaseJson, Error);
		FString SourcePhaseError = Error;
		Error.Reset();
		const bool bPluginPhaseValid = ValidatePhaseAssociationsAndSlots(
			PluginPhase, SeedCandidates, true, PluginPhaseJson, Error);
		FString PluginPhaseError = Error;
		Error.Reset();
		bool bGeometryEqual = false;
		TSharedRef<FJsonObject> Comparison = MakeShared<FJsonObject>();
		if (bSourcePhaseValid && bPluginPhaseValid)
		{
			bGeometryEqual = ComparePhases(SourcePhase, PluginPhase, Comparison, Error);
		}
		else
		{
			Error = !SourcePhaseError.IsEmpty() ? SourcePhaseError : PluginPhaseError;
			Comparison->SetBoolField(TEXT("geometry_equal"), false);
			Comparison->SetStringField(TEXT("first_difference"), Error);
			Comparison->SetStringField(TEXT("status"), TEXT("NOT_OBSERVED_PHASE_SLOT_OR_SEED_GATE_FAILED"));
		}
		Report->SetObjectField(TEXT("source_phase"), SourcePhaseJson);
		Report->SetObjectField(TEXT("plugin_phase"), PluginPhaseJson);
		Report->SetObjectField(TEXT("source_plugin_geometry_comparison"), Comparison);
		Report->SetNumberField(TEXT("source_generation_milliseconds"), SourcePhase.GenerationMilliseconds);
		Report->SetNumberField(TEXT("plugin_generation_milliseconds"), PluginPhase.GenerationMilliseconds);
		Report->SetBoolField(TEXT("source_phase_materials_are_source"), bSourcePhaseValid);
		Report->SetBoolField(TEXT("plugin_phase_materials_are_plugin_mic_and_pm"), bPluginPhaseValid);
		FString PlanObservationError;
		const bool bPlanObservationValid = ObserveGrassBakePlan(World, PluginPhase, SeedCandidates, Report.ToSharedRef(), PlanObservationError);
		FString CoverageError;
		const bool bCoverageValid = bPlanObservationValid && ValidateRegionCoverage(World, Bindings,
			Proxies, ComponentsByProxy, Report.ToSharedRef(), CoverageError);

		RestorePluginCachesAndFlush(Proxies, ComponentsByProxy, Bindings);
		Report->SetBoolField(TEXT("named_grass_types_restored_to_original_plugin_cache"),
			ArePluginCachesRestored(Bindings));
		FString FinalGrassDataHash;
		const bool bGrassDataHashRead = HashGrassData(Bindings, FinalGrassDataHash, Error);
		Report->SetStringField(TEXT("source_grassdata_sha1_after"), FinalGrassDataHash);
		Report->SetBoolField(TEXT("source_grassdata_hash_unchanged"), bGrassDataHashRead && FinalGrassDataHash == InitialGrassDataHash);
		bool bGrassCVarsUnchanged = false;
		if (!CaptureConsoleVariables(ConsoleVariablesAfter, Error))
		{
			Report->SetStringField(TEXT("grass_cvars_after_status"), TEXT("NOT_OBSERVED"));
			Report->SetBoolField(TEXT("grass_cvars_unchanged"), false);
		}
		else
		{
			Report->SetObjectField(TEXT("grass_cvars_after"), ConsoleVariablesToJson(ConsoleVariablesAfter));
			bGrassCVarsUnchanged = AreStringMapsEqual(ConsoleVariablesBefore, ConsoleVariablesAfter);
			Report->SetBoolField(TEXT("grass_cvars_unchanged"), bGrassCVarsUnchanged);
		}

		FString PackageError;
		const bool bPackagesUnchanged = RecheckTrackedPackages(TrackedPackages, PackageError);
		TArray<TSharedPtr<FJsonValue>> PackageRowsAfter;
		TArray<UPackage*> SortedPackageKeys;
		TrackedPackages.GetKeys(SortedPackageKeys);
		SortedPackageKeys.Sort([](const UPackage& A, const UPackage& B) { return A.GetName() < B.GetName(); });
		for (UPackage* Package : SortedPackageKeys)
		{
			const FIMTrackedPackage& PackageRow = TrackedPackages.FindChecked(Package);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package"), Package->GetName());
			Row->SetBoolField(TEXT("dirty_before"), PackageRow.bDirtyBefore);
			Row->SetBoolField(TEXT("dirty_after"), Package->IsDirty());
			Row->SetBoolField(TEXT("source_game_package"), PackageRow.bSourceGamePackage);
			Row->SetStringField(TEXT("filename"), PackageRow.Filename);
			Row->SetStringField(TEXT("sha1_before"), PackageRow.Sha1Before);
			Row->SetStringField(TEXT("sha1_after"), PackageRow.Sha1After);
			Row->SetNumberField(TEXT("bytes_before"), PackageRow.BytesBefore);
			Row->SetNumberField(TEXT("bytes_after"), PackageRow.BytesAfter);
			PackageRowsAfter.Add(MakeShared<FJsonValueObject>(Row));
		}
		Report->SetArrayField(TEXT("tracked_packages_after"), PackageRowsAfter);
		Report->SetBoolField(TEXT("tracked_package_dirty_and_source_hashes_unchanged"), bPackagesUnchanged);
		Report->SetBoolField(TEXT("maps_or_assets_saved"), false);
		Report->SetBoolField(TEXT("generated_grass_components_saved"), false);
		Report->SetArrayField(TEXT("dirty_packages_after"), StringArrayToJson(DirtyPackageNames()));

		const bool bFinalDirtyClear = DirtyPackageNames().IsEmpty()
			&& !World->GetOutermost()->IsDirty();
		const bool bCachesRestored = ArePluginCachesRestored(Bindings);
		Report->SetBoolField(TEXT("all_dirty_packages_clear_after"), bFinalDirtyClear);
		Report->SetBoolField(TEXT("world_package_dirty_restored"), !World->GetOutermost()->IsDirty());
		const bool bAllPass = bSourcePhaseValid && bPluginPhaseValid && bGeometryEqual && bPlanObservationValid && bCoverageValid
			&& Report->GetBoolField(TEXT("source_grassdata_hash_unchanged"))
			&& bGrassCVarsUnchanged
			&& bPackagesUnchanged && bFinalDirtyClear && bCachesRestored;
		Report->SetStringField(TEXT("geometry_status"), bAllPass
			? TEXT("PASS_SOURCE_PLUGIN_GRASS_GEOMETRY_EQUIVALENT")
			: TEXT("FAIL_CLOSED"));
		if (!bAllPass && Error.IsEmpty())
		{
			Error = !CoverageError.IsEmpty() ? CoverageError : !PlanObservationError.IsEmpty() ? PlanObservationError : !SourcePhaseError.IsEmpty() ? SourcePhaseError
				: (!PluginPhaseError.IsEmpty() ? PluginPhaseError
					: (!PackageError.IsEmpty() ? PackageError : TEXT("A source/plugin Grass geometry or restoration gate failed.")));
		}
		bSuccess = bAllPass;
		return Finish(bSuccess, Error);
	}

private:
	static TSharedRef<FJsonObject> PhaseToJson(const FIMPhaseRecord& Phase)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("phase"), Phase.Name);
		Json->SetStringField(TEXT("status"), Phase.TotalInstances > 0 ? TEXT("OBSERVED") : TEXT("NOT_OBSERVED"));
		Json->SetNumberField(TEXT("generation_milliseconds"), Phase.GenerationMilliseconds);
		Json->SetNumberField(TEXT("transform_read_milliseconds"), Phase.TransformReadMilliseconds);
		Json->SetNumberField(TEXT("transform_collection_milliseconds"), Phase.TransformCollectionMilliseconds);
		Json->SetNumberField(TEXT("grass_component_count"), Phase.GrassComponentCount);
		Json->SetNumberField(TEXT("components_with_instances"), Phase.ComponentsWithInstances);
		Json->SetNumberField(TEXT("total_instances"), Phase.TotalInstances);
		Json->SetNumberField(TEXT("total_cpu_instances"), Phase.TotalCPUInstances);
		Json->SetNumberField(TEXT("components_with_render_instances"), Phase.ComponentsWithRenderInstances);
		Json->SetNumberField(TEXT("total_render_instances"), Phase.TotalRenderInstances);
		Json->SetArrayField(TEXT("generated_grass_components"), ComponentRecordsToJson(Phase));
		return Json;
	}

		static bool ValidateNativeGrassGeometry(UWorld* World, const FIMPhaseRecord& ReferenceRegion,
			const TArray<FIMComponentBinding>& InBindings,
			const TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>>& InComponentsByProxy,
			const TSharedRef<FJsonObject>& Coverage, FString& OutError)
		{
			TSharedRef<FJsonObject> Native = MakeShared<FJsonObject>();
			Coverage->SetObjectField(TEXT("product_native_geometry_observation"), Native);
			Native->SetStringField(TEXT("status"), TEXT("FAIL_CLOSED"));
			Native->SetStringField(TEXT("scope"), TEXT("ACTUAL_CAPTURE_SCENE_ADDITIVE_TRIANGLE_PM_COUNTS_AND_REGENERATED_IDENTITY"));
			AIMAcousticBakeVolume* Volume = nullptr;
			for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
			{
				if (Volume) { OutError = TEXT("Native Grass fixture has multiple bake volumes."); return false; }
				Volume = *It;
			}
			if (!Volume || Volume->MaterialDrive != EIMAcousticMaterialDrive::PhysicalMaterial)
			{
				OutError = TEXT("Native Grass fixture requires one physical-material-driven bake volume.");
				return false;
			}
			TArray<int64> Expected;
			Expected.SetNumZeroed(Volume->PhysicalMaterials.Num());
			int64 ExpectedNonSolid = 0;
			for (const FIMGrassComponentRecord& Record : ReferenceRegion.Components)
			{
				if (Record.WorldTransforms.IsEmpty()) continue;
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Record.MeshPath);
				if (!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
				{
					OutError = TEXT("Full-reference Grass mesh has no LOD0 render data: ") + Record.MeshPath;
					return false;
				}
				for (const FStaticMeshSection& Section : Mesh->GetRenderData()->LODResources[0].Sections)
				{
					const FIMGrassSlotRecord* Slot = Record.Slots.FindByPredicate([&](const auto& Entry)
						{ return Entry.SlotIndex == Section.MaterialIndex; });
					const int32 PMIndex = Slot ? Volume->PhysicalMaterials.IndexOfByPredicate([&](const auto& Mapping)
						{ return ObjectPath(Mapping.PhysicalMaterial) == Slot->PhysicalMaterialPath; }) : INDEX_NONE;
					if (PMIndex == INDEX_NONE)
					{
						OutError = TEXT("Full-reference Grass section PM is absent from the bake table: ") + Record.MeshPath;
						return false;
					}
					const int64 Count = int64(Section.NumTriangles) * Record.WorldTransforms.Num();
					if (Volume->PhysicalMaterials[PMIndex].bSolidGeometry) Expected[PMIndex] += Count;
					else ExpectedNonSolid += Count;
				}
			}
			auto Capture = [&](TArray<int64>& Histogram, int64& NonSolid, FString& Fingerprint)
			{
				TArray<FVector> Vertices;
				TArray<FIntVector> Triangles;
				TArray<int32> MaterialIndices;
				if (!Volume->IM_CaptureSceneGeometryForTest(Vertices, Triangles, MaterialIndices, Fingerprint))
				{
					OutError = TEXT("Native Grass CaptureScene failed: ") + Volume->Status;
					return false;
				}
				if (Triangles.Num() != MaterialIndices.Num())
				{
					OutError = TEXT("Native Grass triangle/PM output lengths differ.");
					return false;
				}
				Histogram.SetNumZeroed(Expected.Num());
				for (int32 Index : MaterialIndices)
				{
					if (!Histogram.IsValidIndex(Index))
					{
						OutError = TEXT("Native Grass output has an unmapped PM index.");
						return false;
					}
					++Histogram[Index];
				}
				NonSolid = Volume->NonSolidTriangles;
				return true;
			};
			TArray<int64> Baseline, Full, Regenerated;
			int64 BaselineNonSolid = 0, FullNonSolid = 0, RegeneratedNonSolid = 0;
			FString BaselineFingerprint, FullFingerprint, RegeneratedFingerprint;
			// Only transient named-type caches change. Restore them before checking
			// capture success so every failure path preserves the authored fixture.
			for (const auto& Binding : InBindings) Binding.Component->SetNamedGrassTypes({});
			const bool bBaselineCaptured = Capture(Baseline, BaselineNonSolid, BaselineFingerprint);
			for (const auto& Binding : InBindings)
				Binding.Component->SetNamedGrassTypes(Binding.OriginalPluginNamedGrassTypes);
			if (!bBaselineCaptured || !Capture(Full, FullNonSolid, FullFingerprint)) return false;
			// Recreate components with new temporary names but identical producers.
			for (const auto& Pair : InComponentsByProxy) Pair.Key->FlushGrassComponents(&Pair.Value, false);
			if (!Capture(Regenerated, RegeneratedNonSolid, RegeneratedFingerprint)) return false;
			bool bCountsMatch = FullNonSolid - BaselineNonSolid == ExpectedNonSolid;
			TArray<TSharedPtr<FJsonValue>> Materials;
			for (int32 Index = 0; Index < Expected.Num(); ++Index)
			{
				const int64 Observed = Full[Index] - Baseline[Index];
				bCountsMatch &= Observed == Expected[Index];
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("physical_material"), ObjectPath(Volume->PhysicalMaterials[Index].PhysicalMaterial));
				Entry->SetNumberField(TEXT("baseline_triangles"), double(Baseline[Index]));
				Entry->SetNumberField(TEXT("full_triangles"), double(Full[Index]));
				Entry->SetNumberField(TEXT("expected_added_triangles"), double(Expected[Index]));
				Entry->SetNumberField(TEXT("observed_added_triangles"), double(Observed));
				Materials.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Native->SetArrayField(TEXT("physical_material_triangle_histogram"), Materials);
			Native->SetNumberField(TEXT("baseline_non_solid_triangles"), double(BaselineNonSolid));
			Native->SetNumberField(TEXT("full_non_solid_triangles"), double(FullNonSolid));
			Native->SetNumberField(TEXT("expected_added_non_solid_triangles"), double(ExpectedNonSolid));
			Native->SetBoolField(TEXT("reference_added_triangle_counts_match"), bCountsMatch);
			Native->SetStringField(TEXT("baseline_fingerprint"), BaselineFingerprint);
			Native->SetStringField(TEXT("full_fingerprint"), FullFingerprint);
			Native->SetStringField(TEXT("regenerated_fingerprint"), RegeneratedFingerprint);
			const bool bRegeneratedEqual = Full == Regenerated && FullNonSolid == RegeneratedNonSolid
				&& !FullFingerprint.IsEmpty() && FullFingerprint == RegeneratedFingerprint;
			Native->SetBoolField(TEXT("regenerated_output_and_fingerprint_equal"), bRegeneratedEqual);
			const bool bIdentityChanges = BaselineFingerprint != FullFingerprint;
			Native->SetBoolField(TEXT("empty_grass_negative_changes_fingerprint"), bIdentityChanges);
			const bool bPass = bCountsMatch && bRegeneratedEqual && bIdentityChanges;
			Native->SetStringField(TEXT("status"), bPass ? TEXT("PASS_NATIVE_GRASS_COUNTS_PM_AND_IDENTITY") : TEXT("FAIL_CLOSED"));
			if (!bPass) OutError = TEXT("Native Grass additive PM counts or regenerated identity gate failed.");
			return bPass;
		}

		static bool ValidateRegionCoverage(UWorld* World, const TArray<FIMComponentBinding>& InBindings,
		const TArray<ALandscapeProxy*>& InProxies,
		TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>>& InComponentsByProxy,
		const TSharedRef<FJsonObject>& InReport, FString& OutError)
	{
		TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
		InReport->SetObjectField(TEXT("finite_region_generation_coverage"), Coverage);
		Coverage->SetStringField(TEXT("scope"), TEXT("STATIC_LOD0_FULL_MESH_BOUNDS_INTERSECTION_WITH_PREVIEW_REGION"));
		Coverage->SetStringField(TEXT("status"), TEXT("FAIL_CLOSED"));
		const double Started = FPlatformTime::Seconds();
		FBox Region(ForceInit);
		FIMAcousticGrassBakePlan Plan;
		if (!GetPreviewBakeRegion(World, Region, OutError)
			|| !IMBuildAcousticGrassBakePlan(World, Region, Plan, OutError)) return false;
		if (Plan.Cameras.IsEmpty() || Plan.Proxies.IsEmpty())
		{
			OutError = TEXT("The positive region coverage fixture has an empty finite plan.");
			return false;
		}
		Coverage->SetNumberField(TEXT("finite_camera_count"), Plan.Cameras.Num());
		Coverage->SetNumberField(TEXT("selected_source_count"), Plan.Sources.Num());
		Coverage->SetStringField(TEXT("plan_input_sha1_before"), Plan.SourceInputSha1);
		auto CaptureResetPhase = [&](const FString& Name, const TArray<ALandscapeProxy*>& TargetProxies,
			const TArray<FVector>& PhaseCameras, FIMPhaseRecord& Phase)
		{
			// Reset identically before each sole camera-set change. Otherwise the
			// engine's minimum lifetime could leave last phase's distant instances.
			for (ALandscapeProxy* Proxy : InProxies)
				Proxy->FlushGrassComponents(&InComponentsByProxy.FindChecked(Proxy), false);
			return CapturePhase(Name, TargetProxies, PhaseCameras, Phase, OutError);
		};
		FIMPhaseRecord FiniteFull, ReferenceFull, NegativeFull;
		if (!CaptureResetPhase(TEXT("finite_subsection_cameras"), Plan.Proxies, Plan.Cameras, FiniteFull)) return false;
		// The direct UpdateGrass(empty,true) path disables distance admission and
		// instance GPU culling. It is a reference, never the product generation path.
		if (!CaptureResetPhase(TEXT("full_no_distance_cull_reference"), InProxies, {}, ReferenceFull)) return false;
		if (!CaptureResetPhase(TEXT("far_camera_negative_control"), InProxies,
			{FVector(1.0e9, 1.0e9, 1.0e9)}, NegativeFull)) return false;
		TSharedRef<FJsonObject> FiniteJson = PhaseToJson(FiniteFull);
		TSharedRef<FJsonObject> ReferenceJson = PhaseToJson(ReferenceFull);
		Coverage->SetObjectField(TEXT("finite_full_phase"), FiniteJson);
		Coverage->SetObjectField(TEXT("reference_full_phase"), ReferenceJson);
		Coverage->SetObjectField(TEXT("negative_full_phase"), PhaseToJson(NegativeFull));
		TSet<int32> CoverageSeeds;
		for (const FIMGrassComponentRecord& Record : FiniteFull.Components) CoverageSeeds.Add(Record.Seed);
		for (const FIMGrassComponentRecord& Record : ReferenceFull.Components) CoverageSeeds.Add(Record.Seed);
		TMap<int32, TArray<FIMSeedCandidate>> CoverageCandidates;
		if (!BuildSeedAssociations(InBindings, CoverageSeeds, CoverageCandidates, OutError)
			|| !ValidatePhaseAssociationsAndSlots(FiniteFull, CoverageCandidates, true, FiniteJson, OutError)
			|| !ValidatePhaseAssociationsAndSlots(ReferenceFull, CoverageCandidates, true, ReferenceJson, OutError)) return false;
		FIMPhaseRecord FiniteRegion, ReferenceRegion, NegativeRegion;
		if (!FilterPhaseToRegion(FiniteFull, Region, FiniteRegion, OutError)
			|| !FilterPhaseToRegion(ReferenceFull, Region, ReferenceRegion, OutError)
			|| !FilterPhaseToRegion(NegativeFull, Region, NegativeRegion, OutError)) return false;
		Coverage->SetNumberField(TEXT("finite_region_instance_count"), double(FiniteRegion.TotalInstances));
		Coverage->SetNumberField(TEXT("reference_region_instance_count"), double(ReferenceRegion.TotalInstances));
		Coverage->SetNumberField(TEXT("negative_region_instance_count"), double(NegativeRegion.TotalInstances));
		TSharedRef<FJsonObject> Comparison = MakeShared<FJsonObject>();
		Coverage->SetObjectField(TEXT("reference_vs_finite_region_comparison"), Comparison);
		FString ComparisonError;
		const bool bExactCoverage = ReferenceRegion.TotalInstances > 0
			&& ComparePhases(ReferenceRegion, FiniteRegion, Comparison, ComparisonError);
		TSharedRef<FJsonObject> NegativeComparison = MakeShared<FJsonObject>();
		FString ExpectedNegativeError;
		const bool bNegativeEqual = ComparePhases(ReferenceRegion, NegativeRegion, NegativeComparison, ExpectedNegativeError);
		Coverage->SetObjectField(TEXT("negative_comparison_expected_to_fail"), NegativeComparison);
		const bool bNegativeDetected = !bNegativeEqual && NegativeRegion.TotalInstances == 0;
		Coverage->SetBoolField(TEXT("negative_control_detected"), bNegativeDetected);
		FIMAcousticGrassBakePlan AfterPlan;
		FString AfterPlanError;
		const bool bAfterRead = IMBuildAcousticGrassBakePlan(World, Region, AfterPlan, AfterPlanError);
		Coverage->SetStringField(TEXT("plan_input_sha1_after"), AfterPlan.SourceInputSha1);
		const bool bInputsUnchanged = bAfterRead && AfterPlan.SourceInputSha1 == Plan.SourceInputSha1;
		Coverage->SetBoolField(TEXT("plan_input_unchanged"), bInputsUnchanged);
			Coverage->SetNumberField(TEXT("reference_coverage_validation_milliseconds"), (FPlatformTime::Seconds() - Started) * 1000.0);
			FString NativeError;
			const bool bNative = bExactCoverage && bNegativeDetected && bInputsUnchanged
				&& ValidateNativeGrassGeometry(World, ReferenceRegion, InBindings, InComponentsByProxy, Coverage, NativeError);
			const bool bPass = bExactCoverage && bNegativeDetected && bInputsUnchanged && bNative;
			Coverage->SetNumberField(TEXT("validation_milliseconds"), (FPlatformTime::Seconds() - Started) * 1000.0);
		Coverage->SetStringField(TEXT("status"), bPass ? TEXT("PASS_FINITE_REGION_COVERAGE_AND_NEGATIVE_CONTROL") : TEXT("FAIL_CLOSED"));
			if (!bPass) OutError = !ComparisonError.IsEmpty() ? ComparisonError
				: !AfterPlanError.IsEmpty() ? AfterPlanError
				: !NativeError.IsEmpty() ? NativeError
			: TEXT("Finite region/full-reference, negative-control or source stability gate failed.");
		return bPass;
	}

	static bool ArePluginCachesRestored(const TArray<FIMComponentBinding>& InBindings)
	{
		for (const FIMComponentBinding& Binding : InBindings)
		{
			if (!IsValid(Binding.Component)
				|| NamedGrassTypesHash(Binding.Component->GetNamedGrassTypes())
					!= NamedGrassTypesHash(Binding.OriginalPluginNamedGrassTypes))
			{
				return false;
			}
		}
		return true;
	}

	static void RestorePluginCachesAndFlush(
		const TArray<ALandscapeProxy*>& InProxies,
		const TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>>& InComponentsByProxy,
		const TArray<FIMComponentBinding>& InBindings)
	{
		for (const FIMComponentBinding& Binding : InBindings)
		{
			if (IsValid(Binding.Component))
			{
				Binding.Component->SetNamedGrassTypes(Binding.OriginalPluginNamedGrassTypes);
			}
		}
		for (const TPair<ALandscapeProxy*, TSet<ULandscapeComponent*>>& Pair : InComponentsByProxy)
		{
			if (!IsValid(Pair.Key))
			{
				continue;
			}
			Pair.Key->FlushGrassComponents(&Pair.Value, false);
		}
		for (ALandscapeProxy* Proxy : InProxies)
		{
			if (IsValid(Proxy))
			{
				TArray<FVector> Cameras{Camera3Location};
				Proxy->UpdateGrass(Cameras, true);
			}
		}
	}

	bool Finish(const bool bSuccess, const FString& Error)
	{
		Report->SetStringField(TEXT("automation_status"), bSuccess ? TEXT("PASS") : TEXT("FAIL_CLOSED"));
		Report->SetStringField(TEXT("automation_error"), Error);
		Report->SetStringField(TEXT("final_runtime_scope"),
			TEXT("Observed source and original plugin NamedGrassTypes against existing GrassData in the task preview world. Product audio, Bake acceptance, and formal GPU cost are separate."));
		FString WriteError;
		const bool bReportWritten = WriteReport(ReportPath, Report.ToSharedRef(), WriteError);
		if (bReportWritten)
		{
			Test->AddInfo(FString::Printf(TEXT("Grass generation geometry probe report: %s"), *ReportPath));
		}
		else
		{
			Test->AddError(WriteError);
		}
		if (!bSuccess)
		{
			Test->AddError(Error.IsEmpty() ? TEXT("Grass generation geometry probe failed closed.") : Error);
		}
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] %s GrassGenerationGeometryProbe report=%s"),
			bSuccess && bReportWritten ? TEXT("PASS") : TEXT("FAIL"), *ReportPath);
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s GrassGenerationGeometryProbe report=%s"),
			bSuccess && bReportWritten ? TEXT("PASS") : TEXT("FAIL"), *ReportPath);
		return true;
	}

	FAutomationTestBase* Test = nullptr;
	FString RunId;
	FString ReportPath;
	TSharedPtr<FJsonObject> Report;
	TArray<FIMComponentBinding> Bindings;
	TArray<TSharedPtr<FJsonValue>> BindingRows;
	TMap<UPackage*, FIMTrackedPackage> TrackedPackages;
	TMap<FString, FString> ConsoleVariablesBefore;
	TMap<FString, FString> ConsoleVariablesAfter;
	bool bFinished = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticGrassGenerationGeometryProbe,
	"IceMoon.AcousticField.Rural.GrassGenerationGeometryProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticGrassGenerationGeometryProbe::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(
		IMAcousticGrassGenerationProbeTestPrivate::FIMGrassGenerationGeometryProbeCommand(this));
	return true;
}

#endif
