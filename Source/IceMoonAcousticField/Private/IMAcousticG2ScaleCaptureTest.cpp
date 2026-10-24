#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "StaticMeshResources.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Containers/StringConv.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticG2ScaleCaptureTestPrivate
{
constexpr const TCHAR* IMG2Map = TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
constexpr const TCHAR* IMG2IgnoreTag = TEXT("IMAcousticIgnore");
constexpr const TCHAR* IMG2WhiteboxTag = TEXT("IMAcousticWhitebox");
constexpr double IMG2ResourceGuardBytes = 8.0 * 1024.0 * 1024.0 * 1024.0;
constexpr double IMG2RegionWidthCm = 4000.0;
constexpr double IMG2RegionHeightCm = 3200.0;
constexpr double IMG2RegionHalfDepthCm = 1000.0;
constexpr float IMG2ProbeSpacingCm = 100.0f;
constexpr float IMG2ProbeHeightCm = 150.0f;
constexpr float IMG2MaterialAbsorption = 0.25f;
constexpr float IMG2MaterialScattering = 0.5f;
constexpr int32 IMG2ExpectedWhiteboxComponents = 60;
constexpr int32 IMG2Counts[] = {60, 500, 3000};

enum class EIMG2Representation : uint8
{
	IndependentStaticMeshes,
	ISM,
	HISM
};

static const TCHAR* IMG2RepresentationName(EIMG2Representation Representation)
{
	switch (Representation)
	{
		case EIMG2Representation::IndependentStaticMeshes: return TEXT("independent_sm");
		case EIMG2Representation::ISM: return TEXT("ism");
		case EIMG2Representation::HISM: return TEXT("hism");
		default: return TEXT("unknown");
	}
}

static FString IMG2HashStrings(TArray<FString> Items)
{
	Items.Sort();
	FSHA1 Hash;
	for (const FString& Item : Items)
	{
		FTCHARToUTF8 Bytes(*Item);
		Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
		const uint8 Separator = 0;
		Hash.Update(&Separator, 1);
	}
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, 20);
}

static FString IMG2MaterialIdentity(const UStaticMeshComponent* Component)
{
	FString Result;
	if (!Component) return Result;
	for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
	{
		Result += FString::Printf(TEXT("|material[%d]=%s"), Index, *GetPathNameSafe(Component->GetMaterial(Index)));
	}
	return Result;
}

static FString IMG2WorldIdentity(UWorld* World)
{
	if (!World) return FString();
	TArray<FString> Items;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor)) continue;
		Items.Add(FString::Printf(TEXT("actor|%s|%s|%s"), *Actor->GetPathName(),
			*Actor->GetClass()->GetPathName(), *Actor->GetActorTransform().ToString()));
		TArray<UPrimitiveComponent*> Components;
		Actor->GetComponents(Components);
		for (UPrimitiveComponent* Primitive : Components)
		{
			if (!IsValid(Primitive)) continue;
			FString Item = FString::Printf(TEXT("primitive|%s|%s|%s|mobility=%d"),
				*Primitive->GetPathName(), *Primitive->GetClass()->GetPathName(),
				*Primitive->GetComponentTransform().ToString(), static_cast<int32>(Primitive->Mobility));
			TArray<FString> Tags;
			for (const FName& Tag : Primitive->ComponentTags) Tags.Add(Tag.ToString());
			Tags.Sort();
			for (const FString& Tag : Tags) Item += TEXT("|tag=") + Tag;
			if (const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Primitive))
			{
				Item += TEXT("|mesh=") + GetPathNameSafe(MeshComponent->GetStaticMesh());
				Item += IMG2MaterialIdentity(MeshComponent);
				if (const UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(MeshComponent))
				{
					Item += FString::Printf(TEXT("|instance_count=%d"), Instances->GetInstanceCount());
					for (int32 InstanceIndex = 0; InstanceIndex < Instances->GetInstanceCount(); ++InstanceIndex)
					{
						FTransform InstanceTransform;
						if (!Instances->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
						{
							Item += TEXT("|instance_transform_unavailable");
							continue;
						}
						Item += TEXT("|instance=") + InstanceTransform.ToString();
					}
				}
			}
			Items.Add(MoveTemp(Item));
		}
	}
	return World->GetOutermost()->GetName() + TEXT("|") + IMG2HashStrings(MoveTemp(Items));
}

static FString IMG2BakeAssetIdentity(UWorld* World, int32& OutVolumeCount)
{
	OutVolumeCount = 0;
	if (!World) return FString();
	TArray<FString> Items;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
	{
		++OutVolumeCount;
		UIMAcousticBakeAsset* Asset = It->BakedField;
		FString Item = It->GetPathName() + TEXT("|") + GetPathNameSafe(Asset);
		if (Asset)
		{
			TArray<FString> Metadata;
			Metadata.Add(Asset->MetadataJson);
			FSHA1 StoredSectionsHash;
			if (Asset->SceneData.Num() > 0) StoredSectionsHash.Update(Asset->SceneData.GetData(), Asset->SceneData.Num());
			if (Asset->ProbeData.Num() > 0) StoredSectionsHash.Update(Asset->ProbeData.GetData(), Asset->ProbeData.Num());
			StoredSectionsHash.Final();
			uint8 StoredSectionsDigest[20];
			StoredSectionsHash.GetHash(StoredSectionsDigest);
			Item += TEXT("|") + Asset->WorldPackage + TEXT("|") + Asset->SceneFingerprint
				+ TEXT("|stored_payload_digest=") + Asset->PayloadDigest + TEXT("|stored_sections_sha1=")
				+ BytesToHex(StoredSectionsDigest, 20) + TEXT("|") + IMG2HashStrings(MoveTemp(Metadata));
		}
		Items.Add(MoveTemp(Item));
	}
	return IMG2HashStrings(MoveTemp(Items));
}

struct FIMG2Source
{
	FString WorldPackage;
	FString MapIdentity;
	FString WhiteboxIdentity;
	FString OriginalBakeAssetIdentity;
	FString MeshPath;
	FString MaterialPath;
	int32 OriginalBakeVolumeCount = 0;
	int32 WhiteboxComponentCount = 0;
	int32 UniqueWhiteboxMeshCount = 0;
	int32 CubeLOD0Triangles = 0;
	FVector MeshBoundsOrigin = FVector::ZeroVector;
	FVector MeshBoundsExtent = FVector::ZeroVector;
	TWeakObjectPtr<UStaticMesh> Mesh;
	TWeakObjectPtr<UMaterialInterface> Material;
};

static bool IMG2ReadSource(UWorld* World, FIMG2Source& Out, FString& Error)
{
	if (!IsValid(World))
	{
		Error = TEXT("Editor world is unavailable after loading the W2 mother map.");
		return false;
	}
	Out = FIMG2Source();
	Out.WorldPackage = World->GetOutermost()->GetName();
	Out.MapIdentity = IMG2WorldIdentity(World);
	Out.OriginalBakeAssetIdentity = IMG2BakeAssetIdentity(World, Out.OriginalBakeVolumeCount);
	TSet<UStaticMesh*> UniqueMeshes;
	UStaticMesh* CubeMesh = nullptr;
	UMaterialInterface* CubeMaterial = nullptr;
	bool bUniformMaterial = true;
	TArray<FString> WhiteboxItems;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->ActorHasTag(IMG2WhiteboxTag)) continue;
		TArray<UStaticMeshComponent*> MeshComponents;
		It->GetComponents(MeshComponents);
		for (UStaticMeshComponent* Component : MeshComponents)
		{
			if (!IsValid(Component) || !Component->GetStaticMesh()) continue;
			UStaticMesh* Mesh = Component->GetStaticMesh();
			++Out.WhiteboxComponentCount;
			UniqueMeshes.Add(Mesh);
			if (!CubeMesh) CubeMesh = Mesh;
			if (Out.WhiteboxComponentCount == 1) CubeMaterial = Component->GetMaterial(0);
			else if (CubeMaterial != Component->GetMaterial(0)) bUniformMaterial = false;
			WhiteboxItems.Add(Component->GetPathName() + TEXT("|") + GetPathNameSafe(Mesh) + TEXT("|")
				+ Component->GetComponentTransform().ToString() + TEXT("|") + GetPathNameSafe(Component->GetMaterial(0)));
		}
	}
	Out.UniqueWhiteboxMeshCount = UniqueMeshes.Num();
	Out.WhiteboxIdentity = IMG2HashStrings(MoveTemp(WhiteboxItems));
	if (Out.WorldPackage != IMG2Map || Out.WhiteboxComponentCount != IMG2ExpectedWhiteboxComponents
		|| Out.UniqueWhiteboxMeshCount != 1 || !CubeMesh || !CubeMaterial || !bUniformMaterial)
	{
		Error = FString::Printf(TEXT("W2 mother map source drift: package=%s whitebox_components=%d unique_meshes=%d mesh=%s material=%s"),
			*Out.WorldPackage, Out.WhiteboxComponentCount, Out.UniqueWhiteboxMeshCount,
			*GetPathNameSafe(CubeMesh), *GetPathNameSafe(CubeMaterial));
		return false;
	}
	if (CubeMesh->GetStaticMaterials().Num() != 1 || !CubeMesh->GetRenderData()
		|| CubeMesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = TEXT("W2 whitebox Cube no longer has one material slot and readable LOD0 data.");
		return false;
	}
	const FStaticMeshLODResources& LOD0 = CubeMesh->GetRenderData()->LODResources[0];
	int64 TriangleCount = 0;
	for (const FStaticMeshSection& Section : LOD0.Sections) TriangleCount += Section.NumTriangles;
	if (TriangleCount <= 0 || TriangleCount > MAX_int32)
	{
		Error = TEXT("W2 whitebox Cube has an invalid LOD0 triangle count.");
		return false;
	}
	Out.CubeLOD0Triangles = static_cast<int32>(TriangleCount);
	Out.MeshBoundsOrigin = CubeMesh->GetBounds().Origin;
	Out.MeshBoundsExtent = CubeMesh->GetBounds().BoxExtent;
	Out.MeshPath = CubeMesh->GetPathName();
	Out.MaterialPath = CubeMaterial->GetPathName();
	Out.Mesh = CubeMesh;
	Out.Material = CubeMaterial;
	return true;
}

struct FIMG2MemorySample
{
	FString Stage;
	uint64 UsedPhysical = 0;
	uint64 PeakUsedPhysical = 0;
	uint64 UsedVirtual = 0;
	uint64 PeakUsedVirtual = 0;
	uint64 AvailablePhysical = 0;
};

static FIMG2MemorySample IMG2ReadMemory(const TCHAR* Stage)
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	FIMG2MemorySample Sample;
	Sample.Stage = Stage;
	Sample.UsedPhysical = Stats.UsedPhysical;
	Sample.PeakUsedPhysical = Stats.PeakUsedPhysical;
	Sample.UsedVirtual = Stats.UsedVirtual;
	Sample.PeakUsedVirtual = Stats.PeakUsedVirtual;
	Sample.AvailablePhysical = Stats.AvailablePhysical;
	return Sample;
}

static bool IMG2BelowResourceGuard(const FIMG2MemorySample& Sample)
{
	return static_cast<double>(Sample.AvailablePhysical) < IMG2ResourceGuardBytes;
}

static void IMG2AddMemorySample(TArray<FIMG2MemorySample>& Samples, const TCHAR* Stage)
{
	Samples.Add(IMG2ReadMemory(Stage));
}

struct FIMG2Case
{
	FString Name;
	FString Representation;
	FString Outcome = TEXT("NOT_STARTED");
	FString Error;
	FString TransformFingerprint;
	FString SceneFingerprint;
	FString ValidationFailure;
	int32 RequestedPlacements = 0;
	int32 BuiltPlacements = 0;
	int32 PrimitiveComponents = 0;
	int32 CubeLOD0Triangles = 0;
	int32 ExpectedTriangles = 0;
	int32 CapturedVertices = 0;
	int32 CapturedTriangles = 0;
	int32 CapturedMaterialIndices = 0;
	int32 NonZeroMaterialIndices = 0;
	bool bCaptureSucceeded = false;
	bool bTriangleCountExact = false;
	bool bMaterialCountExact = false;
	bool bValidationReturnedTrue = false;
	bool bMissingAssetGuardObserved = false;
	double ConstructionWallSeconds = 0.0;
	double CaptureAccessorWallSeconds = 0.0;
	double ValidateCurrentBakeWallSeconds = 0.0;
	TArray<FIMG2MemorySample> MemorySamples;
};

class FIMG2Fixture
{
public:
	~FIMG2Fixture() { Cleanup(); }

	void Cleanup()
	{
		for (const TWeakObjectPtr<AActor>& Actor : TransientActors) if (Actor.IsValid()) Actor->Destroy();
		TransientActors.Reset();
		if (Volume.IsValid()) Volume->Destroy();
		Volume.Reset();
		for (const TWeakObjectPtr<UPrimitiveComponent>& Component : TaggedComponents)
		{
			if (Component.IsValid()) Component->ComponentTags.Remove(IMG2IgnoreTag);
		}
		TaggedComponents.Reset();
		World = nullptr;
	}

	bool Prime(FIMG2Source& Out, FString& Error)
	{
		Cleanup();
		if (!FEditorFileUtils::LoadMap(IMG2Map, false, true))
		{
			Error = TEXT("Cannot load the required W2 whitebox mother map.");
			return false;
		}
		World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!IMG2ReadSource(World, Out, Error)) return false;
		BaselineMapIdentity = Out.MapIdentity;
		BaselineWhiteboxIdentity = Out.WhiteboxIdentity;
		BaselineBakeAssetIdentity = Out.OriginalBakeAssetIdentity;
		BaselineMeshPath = Out.MeshPath;
		BaselineMaterialPath = Out.MaterialPath;
		return true;
	}

	bool PrepareCase(FIMG2Source& Out, FString& Error)
	{
		Cleanup();
		if (!FEditorFileUtils::LoadMap(IMG2Map, false, true))
		{
			Error = TEXT("Cannot reload the required W2 whitebox mother map for an isolated case.");
			return false;
		}
		World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!IMG2ReadSource(World, Out, Error)) return false;
		if (Out.MapIdentity != BaselineMapIdentity || Out.WhiteboxIdentity != BaselineWhiteboxIdentity
			|| Out.OriginalBakeAssetIdentity != BaselineBakeAssetIdentity
			|| Out.MeshPath != BaselineMeshPath || Out.MaterialPath != BaselineMaterialPath)
		{
			Error = TEXT("Mother map or source asset identity changed between isolated cases.");
			return false;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UPrimitiveComponent*> Components;
			It->GetComponents(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				if (!IsValid(Component) || Component->ComponentHasTag(IMG2IgnoreTag)) continue;
				Component->ComponentTags.Add(IMG2IgnoreTag);
				TaggedComponents.Add(Component);
			}
		}

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		Volume = World->SpawnActor<AIMAcousticBakeVolume>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
		if (!Volume.IsValid())
		{
			Error = TEXT("Could not create the transient G2 capture volume.");
			return false;
		}
		Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
		Volume->SetActorLocation(FVector::ZeroVector);
		Volume->SetActorTickEnabled(false);
		Volume->BakedField = nullptr;
		Volume->BakeBounds->SetMobility(EComponentMobility::Static);
		Volume->BakeBounds->SetBoxExtent(FVector(IMG2RegionWidthCm * 0.5, IMG2RegionHeightCm * 0.5, IMG2RegionHalfDepthCm));
		Volume->ProbeSpacingCm = IMG2ProbeSpacingCm;
		Volume->ProbeHeightCm = IMG2ProbeHeightCm;
		FIMAcousticMaterialMapping Mapping;
		Mapping.Material = Out.Material.Get();
		Mapping.Absorption = FVector(IMG2MaterialAbsorption);
		Mapping.Scattering = IMG2MaterialScattering;
		Volume->Materials = {Mapping};
		return true;
	}

	UWorld* GetWorld() const { return World; }
	AIMAcousticBakeVolume* GetVolume() const { return Volume.Get(); }
	void TrackActor(AActor* Actor) { if (IsValid(Actor)) TransientActors.Add(Actor); }

	bool RestoreAndCompare(FString& OutMapIdentity, FString& OutAssetIdentity, FString& Error)
	{
		Cleanup();
		if (!FEditorFileUtils::LoadMap(IMG2Map, false, true))
		{
			Error = TEXT("Final reload of the W2 mother map failed.");
			return false;
		}
		UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FIMG2Source Restored;
		if (!IMG2ReadSource(RestoredWorld, Restored, Error)) return false;
		OutMapIdentity = Restored.MapIdentity;
		OutAssetIdentity = Restored.OriginalBakeAssetIdentity;
		return Restored.MapIdentity == BaselineMapIdentity
			&& Restored.WhiteboxIdentity == BaselineWhiteboxIdentity
			&& Restored.OriginalBakeAssetIdentity == BaselineBakeAssetIdentity
			&& Restored.MeshPath == BaselineMeshPath && Restored.MaterialPath == BaselineMaterialPath;
	}

private:
	UWorld* World = nullptr;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TArray<TWeakObjectPtr<AActor>> TransientActors;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> TaggedComponents;
	FString BaselineMapIdentity;
	FString BaselineWhiteboxIdentity;
	FString BaselineBakeAssetIdentity;
	FString BaselineMeshPath;
	FString BaselineMaterialPath;
};

static bool IMG2BuildTransforms(int32 Count, const FIMG2Source& Source, float CubeScale,
	TArray<FTransform>& OutTransforms, FString& Error)
{
	if (Count <= 0 || Source.CubeLOD0Triangles <= 0 || CubeScale <= 0.0f)
	{
		Error = TEXT("Invalid deterministic placement input.");
		return false;
	}
	const int32 Columns = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Count * IMG2RegionWidthCm / IMG2RegionHeightCm)));
	const int32 Rows = FMath::DivideAndRoundUp(Count, Columns);
	const double CellWidth = IMG2RegionWidthCm / Columns;
	const double CellHeight = IMG2RegionHeightCm / Rows;
	const double HalfX = Source.MeshBoundsExtent.X * CubeScale;
	const double HalfY = Source.MeshBoundsExtent.Y * CubeScale;
	if (HalfX > CellWidth * 0.5 || HalfY > CellHeight * 0.5)
	{
		Error = TEXT("Scaled Cube bounds do not fit the fixed 40m x 32m placement cells.");
		return false;
	}
	OutTransforms.Reset(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row = Index / Columns;
		const FVector BoundsCenter(
			-IMG2RegionWidthCm * 0.5 + (Column + 0.5) * CellWidth,
			-IMG2RegionHeightCm * 0.5 + (Row + 0.5) * CellHeight,
			0.0);
		const FVector PivotLocation = BoundsCenter - Source.MeshBoundsOrigin * CubeScale;
		OutTransforms.Emplace(FQuat::Identity, PivotLocation, FVector(CubeScale));
	}
	return true;
}

static float IMG2ChooseCubeScale(const FIMG2Source& Source)
{
	const int32 MaxColumns = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(IMG2Counts[2] * IMG2RegionWidthCm / IMG2RegionHeightCm)));
	const int32 MaxRows = FMath::DivideAndRoundUp(IMG2Counts[2], MaxColumns);
	const double MinCell = FMath::Min(IMG2RegionWidthCm / MaxColumns, IMG2RegionHeightCm / MaxRows);
	const double MaxExtent = FMath::Max3(Source.MeshBoundsExtent.X, Source.MeshBoundsExtent.Y, Source.MeshBoundsExtent.Z);
	if (!FMath::IsFinite(MaxExtent) || MaxExtent <= 0.0) return 0.0f;
	// Pick one scale from the densest grid and keep it unchanged in all nine cases.
	return static_cast<float>(FMath::Min(1.0, 0.9 * MinCell / (2.0 * MaxExtent)));
}

static bool IMG2AddStaticMeshActor(FIMG2Fixture& Fixture, const FIMG2Source& Source,
	const FTransform& Transform, FString& Error)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AStaticMeshActor* Actor = Fixture.GetWorld()->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
	if (!IsValid(Actor))
	{
		Error = TEXT("Could not spawn an independent transient StaticMeshActor.");
		return false;
	}
	Fixture.TrackActor(Actor);
	Actor->SetActorTransform(Transform);
	UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
	if (!IsValid(Component))
	{
		Error = TEXT("Spawned StaticMeshActor has no static mesh component.");
		return false;
	}
	Component->SetMobility(EComponentMobility::Static);
	Component->SetStaticMesh(Source.Mesh.Get());
	Component->SetMaterial(0, Source.Material.Get());
	if (Component->Mobility != EComponentMobility::Static || Component->GetStaticMesh() != Source.Mesh.Get())
	{
		Error = TEXT("Independent mesh component did not retain the static Cube setup.");
		return false;
	}
	return true;
}

static bool IMG2AddInstancedActor(FIMG2Fixture& Fixture, const FIMG2Source& Source,
	EIMG2Representation Representation, const TArray<FTransform>& Transforms,
	FIMG2Case& Case, bool& bOutResourceGuard, FString& Error)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AActor* Actor = Fixture.GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!IsValid(Actor))
	{
		Error = TEXT("Could not spawn the transient instancing owner actor.");
		return false;
	}
	Fixture.TrackActor(Actor);
	USceneComponent* Root = NewObject<USceneComponent>(Actor, NAME_None, RF_Transient);
	if (!Root)
	{
		Error = TEXT("Could not create the transient instancing root component.");
		return false;
	}
	Root->SetMobility(EComponentMobility::Static);
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponent();

	UInstancedStaticMeshComponent* Component = Representation == EIMG2Representation::HISM
		? static_cast<UInstancedStaticMeshComponent*>(NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient))
		: NewObject<UInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient);
	if (!Component)
	{
		Error = TEXT("Could not create the transient ISM/HISM component.");
		return false;
	}
	Actor->AddInstanceComponent(Component);
	Component->SetupAttachment(Root);
	Component->SetMobility(EComponentMobility::Static);
	Component->SetStaticMesh(Source.Mesh.Get());
	Component->SetMaterial(0, Source.Material.Get());
	for (int32 Index = 0; Index < Transforms.Num(); ++Index)
	{
		if (Component->AddInstance(Transforms[Index], true) < 0)
		{
			Error = TEXT("ISM/HISM rejected a deterministic world-space Cube transform.");
			return false;
		}
		Case.BuiltPlacements = Index + 1;
		if ((Index + 1) % 128 == 0)
		{
			const FIMG2MemorySample Progress = IMG2ReadMemory(TEXT("build_progress"));
			Case.MemorySamples.Add(Progress);
			if (IMG2BelowResourceGuard(Progress))
			{
				bOutResourceGuard = true;
				Error = FString::Printf(TEXT("System available RAM fell below the %.1f GiB guard after %d instances."),
					IMG2ResourceGuardBytes / (1024.0 * 1024.0 * 1024.0), Index + 1);
				return false;
			}
		}
	}
	Component->RegisterComponent();
	if (Component->Mobility != EComponentMobility::Static || Component->GetInstanceCount() != Transforms.Num())
	{
		Error = TEXT("ISM/HISM did not retain the requested static instance count.");
		return false;
	}
	return true;
}

static bool IMG2BuildScene(FIMG2Fixture& Fixture, const FIMG2Source& Source,
	EIMG2Representation Representation, const TArray<FTransform>& Transforms,
	FIMG2Case& Case, bool& bOutResourceGuard, FString& Error)
{
	const double Started = FPlatformTime::Seconds();
	Case.PrimitiveComponents = Representation == EIMG2Representation::IndependentStaticMeshes ? Transforms.Num() : 1;
	if (Representation == EIMG2Representation::IndependentStaticMeshes)
	{
		for (int32 Index = 0; Index < Transforms.Num(); ++Index)
		{
			if (!IMG2AddStaticMeshActor(Fixture, Source, Transforms[Index], Error))
			{
				Case.BuiltPlacements = Index;
				Case.ConstructionWallSeconds = FPlatformTime::Seconds() - Started;
				return false;
			}
			++Case.BuiltPlacements;
			if ((Index + 1) % 128 == 0)
			{
				const FIMG2MemorySample Progress = IMG2ReadMemory(TEXT("build_progress"));
				Case.MemorySamples.Add(Progress);
				if (IMG2BelowResourceGuard(Progress))
				{
					bOutResourceGuard = true;
					Error = FString::Printf(TEXT("System available RAM fell below the %.1f GiB guard after %d placements."),
						IMG2ResourceGuardBytes / (1024.0 * 1024.0 * 1024.0), Index + 1);
					Case.ConstructionWallSeconds = FPlatformTime::Seconds() - Started;
					return false;
				}
			}
		}
	}
	else
	{
		if (!IMG2AddInstancedActor(Fixture, Source, Representation, Transforms, Case, bOutResourceGuard, Error))
		{
			Case.ConstructionWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}
		Case.BuiltPlacements = Transforms.Num();
	}
	Case.ConstructionWallSeconds = FPlatformTime::Seconds() - Started;
	IMG2AddMemorySample(Case.MemorySamples, TEXT("post_build"));
	return true;
}

template<typename TWriter>
static void IMG2WriteMemorySample(TWriter& Writer, const FIMG2MemorySample& Sample)
{
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stage"), Sample.Stage);
	Writer->WriteValue(TEXT("used_physical_bytes"), static_cast<double>(Sample.UsedPhysical));
	Writer->WriteValue(TEXT("peak_used_physical_bytes"), static_cast<double>(Sample.PeakUsedPhysical));
	Writer->WriteValue(TEXT("used_virtual_bytes"), static_cast<double>(Sample.UsedVirtual));
	Writer->WriteValue(TEXT("peak_used_virtual_bytes"), static_cast<double>(Sample.PeakUsedVirtual));
	Writer->WriteValue(TEXT("available_system_physical_bytes"), static_cast<double>(Sample.AvailablePhysical));
	Writer->WriteObjectEnd();
}

static bool IMG2WriteReport(const FString& Directory, const FString& ReportPath, const FString& RunId,
	const FIMG2Source& Source, const FString& FinalMapIdentity, const FString& FinalAssetIdentity,
	bool bOriginalMapRestored, bool bOriginalAssetsUnchanged, bool bResourceGuardHit,
	const FString& ResourceGuardReason, const FString& FinalResult,
	const TArray<FIMG2MemorySample>& RunMemorySamples, const TArray<FIMG2Case>& Cases,
	const TArray<FString>& SkippedCases, float CubeScale)
{
	FString Json;
	auto Writer = TJsonWriterFactory<>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema"), TEXT("im-acoustic-g2-capture-scale-v1"));
	Writer->WriteValue(TEXT("run_id"), RunId);
	Writer->WriteValue(TEXT("final_result"), FinalResult);
	Writer->WriteValue(TEXT("scope"), TEXT("W2 whitebox repeated-Cube scene capture scaling; no SDK Bake, payload commit, or cold runtime bake validation."));
	Writer->WriteValue(TEXT("map_path"), IMG2Map);
	Writer->WriteValue(TEXT("world_package"), Source.WorldPackage);
	Writer->WriteValue(TEXT("baseline_map_identity"), Source.MapIdentity);
	Writer->WriteValue(TEXT("final_map_identity"), FinalMapIdentity);
	Writer->WriteValue(TEXT("original_map_restored"), bOriginalMapRestored);
	Writer->WriteValue(TEXT("original_bake_asset_identity"), Source.OriginalBakeAssetIdentity);
	Writer->WriteValue(TEXT("final_bake_asset_identity"), FinalAssetIdentity);
	Writer->WriteValue(TEXT("bake_asset_identity_storage_basis"), TEXT("Serialized SceneData and ProbeData plus metadata; Format 2 stores raw sections and Format 3 stores packed sections."));
	Writer->WriteValue(TEXT("original_bake_assets_unchanged"), bOriginalAssetsUnchanged);
	Writer->WriteValue(TEXT("original_bake_volume_count"), Source.OriginalBakeVolumeCount);
	Writer->WriteValue(TEXT("whitebox_identity"), Source.WhiteboxIdentity);
	Writer->WriteValue(TEXT("whitebox_components"), Source.WhiteboxComponentCount);
	Writer->WriteValue(TEXT("cube_mesh_path"), Source.MeshPath);
	Writer->WriteValue(TEXT("cube_material_path"), Source.MaterialPath);
	Writer->WriteValue(TEXT("cube_lod"), 0);
	Writer->WriteValue(TEXT("cube_lod0_triangles"), Source.CubeLOD0Triangles);
	Writer->WriteValue(TEXT("cube_bounds_origin_cm"), Source.MeshBoundsOrigin.ToString());
	Writer->WriteValue(TEXT("cube_bounds_extent_cm"), Source.MeshBoundsExtent.ToString());
	Writer->WriteValue(TEXT("cube_uniform_scale"), CubeScale);
	Writer->WriteValue(TEXT("placement_region_width_m"), 40.0);
	Writer->WriteValue(TEXT("placement_region_height_m"), 32.0);
	Writer->WriteValue(TEXT("probe_spacing_cm"), IMG2ProbeSpacingCm);
	Writer->WriteValue(TEXT("probe_height_cm"), IMG2ProbeHeightCm);
	Writer->WriteValue(TEXT("material_absorption_rgb"), FVector(IMG2MaterialAbsorption).ToString());
	Writer->WriteValue(TEXT("material_scattering"), IMG2MaterialScattering);
	Writer->WriteValue(TEXT("validation_measurement"), TEXT("ValidateCurrentBake with transient BakedField unset; elapsed time includes recapture and the Missing acoustic bake asset guard only."));
	Writer->WriteValue(TEXT("editor_ticker_measured"), false);
	Writer->WriteValue(TEXT("editor_ticker_note"), TEXT("No editor ticker scheduling or delayed invalidation refresh is measured; only the direct ValidateCurrentBake call is timed."));
	Writer->WriteValue(TEXT("capture_measurement"), TEXT("IM_CaptureSceneGeometryForTest wall time includes CaptureScene, its current diagnostic log output, and test-array readback copy; core capture timing is emitted separately by the capture accessor."));
	Writer->WriteValue(TEXT("resource_guard_hit"), bResourceGuardHit);
	Writer->WriteValue(TEXT("resource_guard_bytes_available_minimum"), IMG2ResourceGuardBytes);
	Writer->WriteValue(TEXT("resource_guard_reason"), ResourceGuardReason);

	uint64 PeakProcessPhysical = 0;
	uint64 PeakProcessVirtual = 0;
	uint64 MaxSampledPhysical = 0;
	uint64 MaxSampledVirtual = 0;
	uint64 MinimumAvailablePhysical = MAX_uint64;
	uint64 InitialPeakPhysical = 0;
	uint64 InitialPeakVirtual = 0;
	bool bHasInitial = false;
	auto AccumulateMemory = [&](const FIMG2MemorySample& Sample)
	{
		PeakProcessPhysical = FMath::Max(PeakProcessPhysical, Sample.PeakUsedPhysical);
		PeakProcessVirtual = FMath::Max(PeakProcessVirtual, Sample.PeakUsedVirtual);
		MaxSampledPhysical = FMath::Max(MaxSampledPhysical, Sample.UsedPhysical);
		MaxSampledVirtual = FMath::Max(MaxSampledVirtual, Sample.UsedVirtual);
		MinimumAvailablePhysical = FMath::Min(MinimumAvailablePhysical, Sample.AvailablePhysical);
		if (!bHasInitial)
		{
			InitialPeakPhysical = Sample.PeakUsedPhysical;
			InitialPeakVirtual = Sample.PeakUsedVirtual;
			bHasInitial = true;
		}
	};
	for (const FIMG2MemorySample& Sample : RunMemorySamples) AccumulateMemory(Sample);
	for (const FIMG2Case& Case : Cases) for (const FIMG2MemorySample& Sample : Case.MemorySamples) AccumulateMemory(Sample);
	Writer->WriteObjectStart(TEXT("memory_summary"));
	Writer->WriteValue(TEXT("process_peak_used_physical_bytes"), static_cast<double>(PeakProcessPhysical));
	Writer->WriteValue(TEXT("process_peak_used_virtual_bytes"), static_cast<double>(PeakProcessVirtual));
	Writer->WriteValue(TEXT("highest_sampled_used_physical_bytes"), static_cast<double>(MaxSampledPhysical));
	Writer->WriteValue(TEXT("highest_sampled_used_virtual_bytes"), static_cast<double>(MaxSampledVirtual));
	Writer->WriteValue(TEXT("lowest_sampled_available_system_physical_bytes"),
		MinimumAvailablePhysical == MAX_uint64 ? 0.0 : static_cast<double>(MinimumAvailablePhysical));
	Writer->WriteValue(TEXT("process_peak_physical_delta_since_test_start_bytes"),
		static_cast<double>(PeakProcessPhysical >= InitialPeakPhysical ? PeakProcessPhysical - InitialPeakPhysical : 0));
	Writer->WriteValue(TEXT("process_peak_virtual_delta_since_test_start_bytes"),
		static_cast<double>(PeakProcessVirtual >= InitialPeakVirtual ? PeakProcessVirtual - InitialPeakVirtual : 0));
	Writer->WriteObjectEnd();

	Writer->WriteArrayStart(TEXT("run_memory_samples"));
	for (const FIMG2MemorySample& Sample : RunMemorySamples) IMG2WriteMemorySample(Writer, Sample);
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("cases"));
	for (const FIMG2Case& Case : Cases)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("name"), Case.Name);
		Writer->WriteValue(TEXT("representation"), Case.Representation);
		Writer->WriteValue(TEXT("outcome"), Case.Outcome);
		Writer->WriteValue(TEXT("error"), Case.Error);
		Writer->WriteValue(TEXT("requested_placements"), Case.RequestedPlacements);
		Writer->WriteValue(TEXT("built_placements"), Case.BuiltPlacements);
		Writer->WriteValue(TEXT("primitive_components"), Case.PrimitiveComponents);
		Writer->WriteValue(TEXT("cube_lod0_triangles"), Case.CubeLOD0Triangles);
		Writer->WriteValue(TEXT("expected_triangles"), Case.ExpectedTriangles);
		Writer->WriteValue(TEXT("captured_vertices"), Case.CapturedVertices);
		Writer->WriteValue(TEXT("captured_triangles"), Case.CapturedTriangles);
		Writer->WriteValue(TEXT("captured_material_indices"), Case.CapturedMaterialIndices);
		Writer->WriteValue(TEXT("nonzero_material_indices"), Case.NonZeroMaterialIndices);
		Writer->WriteValue(TEXT("capture_succeeded"), Case.bCaptureSucceeded);
		Writer->WriteValue(TEXT("triangle_count_exact"), Case.bTriangleCountExact);
		Writer->WriteValue(TEXT("material_count_exact"), Case.bMaterialCountExact);
		Writer->WriteValue(TEXT("scene_fingerprint"), Case.SceneFingerprint);
		Writer->WriteValue(TEXT("transform_fingerprint"), Case.TransformFingerprint);
		Writer->WriteValue(TEXT("construction_wall_seconds"), Case.ConstructionWallSeconds);
		Writer->WriteValue(TEXT("capture_accessor_wall_seconds"), Case.CaptureAccessorWallSeconds);
		Writer->WriteValue(TEXT("validate_current_bake_wall_seconds"), Case.ValidateCurrentBakeWallSeconds);
		Writer->WriteValue(TEXT("validate_current_bake_returned_true"), Case.bValidationReturnedTrue);
		Writer->WriteValue(TEXT("missing_asset_guard_observed"), Case.bMissingAssetGuardObserved);
		Writer->WriteValue(TEXT("validation_failure"), Case.ValidationFailure);
		Writer->WriteArrayStart(TEXT("memory_samples"));
		for (const FIMG2MemorySample& Sample : Case.MemorySamples) IMG2WriteMemorySample(Writer, Sample);
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("cases_not_run"));
	for (const FString& Name : SkippedCases) Writer->WriteValue(Name);
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("not_covered"));
	Writer->WriteValue(TEXT("Steam Audio SDK bake, SDK scene commit, persistent payload validation, or cold runtime load."));
	Writer->WriteValue(TEXT("Valid bound-bake ValidateCurrentBake cost; this test intentionally measures only recapture plus the missing-asset guard."));
	Writer->WriteValue(TEXT("Acoustic output quality, listening, runtime spatialization, and GPU performance."));
	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("report_path"), ReportPath);
	Writer->WriteObjectEnd();
	Writer->Close();
	return IFileManager::Get().MakeDirectory(*Directory, true)
		&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticG2ScaleCapture,
	"IceMoon.AcousticField.G2.CaptureScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool IM_AcousticG2ScaleCapture::RunTest(const FString&)
{
	IMAcousticG2ScaleCaptureTestPrivate::FIMG2Fixture Fixture;
	IMAcousticG2ScaleCaptureTestPrivate::FIMG2Source InitialSource;
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/G2Capture"), RunId);
	const FString ReportPath = FPaths::Combine(Directory, TEXT("report.json"));
	TArray<IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample> RunMemorySamples;
	TArray<IMAcousticG2ScaleCaptureTestPrivate::FIMG2Case> Cases;
	TArray<FString> SkippedCases;
	IMAcousticG2ScaleCaptureTestPrivate::IMG2AddMemorySample(RunMemorySamples, TEXT("run_start"));

	FString Failure;
	FString ResourceGuardReason;
	FString FinalMapIdentity;
	FString FinalAssetIdentity;
	bool bResourceGuardHit = false;
	bool bHardFailure = false;
	bool bOriginalMapRestored = false;
	bool bOriginalAssetsUnchanged = false;
	if (!Fixture.Prime(InitialSource, Failure))
	{
		bHardFailure = true;
	}
	else
	{
		IMAcousticG2ScaleCaptureTestPrivate::IMG2AddMemorySample(RunMemorySamples, TEXT("mother_map_primed"));
	}

	const float CubeScale = InitialSource.CubeLOD0Triangles > 0 ? IMAcousticG2ScaleCaptureTestPrivate::IMG2ChooseCubeScale(InitialSource) : 0.0f;
	if (!bHardFailure && CubeScale <= 0.0f)
	{
		Failure = TEXT("Could not choose a finite fixed Cube scale for the 3000-placement grid.");
		bHardFailure = true;
	}

	if (!bHardFailure && InitialSource.CubeLOD0Triangles > MAX_int32 / IMAcousticG2ScaleCaptureTestPrivate::IMG2Counts[2])
	{
		Failure = TEXT("Cube LOD0 triangle count is too large for exact int32 capture-count reporting.");
		bHardFailure = true;
	}

	if (!bHardFailure)
	{
		const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample Preflight = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("before_first_scale"));
		RunMemorySamples.Add(Preflight);
		if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(Preflight))
		{
			bResourceGuardHit = true;
			ResourceGuardReason = FString::Printf(TEXT("Available system RAM was %.2f GiB before the first scale; the %.2f GiB safety floor stopped the run."),
				Preflight.AvailablePhysical / (1024.0 * 1024.0 * 1024.0), IMAcousticG2ScaleCaptureTestPrivate::IMG2ResourceGuardBytes / (1024.0 * 1024.0 * 1024.0));
		}
	}

	for (int32 CountIndex = 0; CountIndex < UE_ARRAY_COUNT(IMAcousticG2ScaleCaptureTestPrivate::IMG2Counts) && !bHardFailure && !bResourceGuardHit; ++CountIndex)
	{
		const int32 Count = IMAcousticG2ScaleCaptureTestPrivate::IMG2Counts[CountIndex];
		TArray<FTransform> Transforms;
		if (!IMAcousticG2ScaleCaptureTestPrivate::IMG2BuildTransforms(Count, InitialSource, CubeScale, Transforms, Failure))
		{
			bHardFailure = true;
			break;
		}
		TArray<FString> TransformItems;
		TransformItems.Reserve(Transforms.Num());
		for (const FTransform& Transform : Transforms) TransformItems.Add(Transform.ToString());
		const FString TransformFingerprint = IMAcousticG2ScaleCaptureTestPrivate::IMG2HashStrings(MoveTemp(TransformItems));

		const IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation Representations[] = {
			IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation::IndependentStaticMeshes,
			IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation::ISM,
			IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation::HISM
		};
		for (const IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation Representation : Representations)
		{
			const FString CaseName = FString::Printf(TEXT("N%d_%s"), Count, IMAcousticG2ScaleCaptureTestPrivate::IMG2RepresentationName(Representation));
			const FString BeforeCaseStage = FString::Printf(TEXT("before_%s"), *CaseName);
			const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample BeforeCase = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(*BeforeCaseStage);
			RunMemorySamples.Add(BeforeCase);
			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(BeforeCase))
			{
				bResourceGuardHit = true;
				ResourceGuardReason = FString::Printf(TEXT("Available system RAM fell below %.2f GiB before %s."),
					IMAcousticG2ScaleCaptureTestPrivate::IMG2ResourceGuardBytes / (1024.0 * 1024.0 * 1024.0), *CaseName);
				break;
			}

			IMAcousticG2ScaleCaptureTestPrivate::FIMG2Case Case;
			Case.Name = CaseName;
			Case.Representation = IMAcousticG2ScaleCaptureTestPrivate::IMG2RepresentationName(Representation);
			Case.RequestedPlacements = Count;
			Case.CubeLOD0Triangles = InitialSource.CubeLOD0Triangles;
			Case.ExpectedTriangles = Count * InitialSource.CubeLOD0Triangles;
			Case.TransformFingerprint = TransformFingerprint;

			IMAcousticG2ScaleCaptureTestPrivate::FIMG2Source CurrentSource;
			if (!Fixture.PrepareCase(CurrentSource, Failure))
			{
				Case.Outcome = TEXT("FAIL_SETUP");
				Case.Error = Failure;
				Cases.Add(MoveTemp(Case));
				bHardFailure = true;
				break;
			}
			IMAcousticG2ScaleCaptureTestPrivate::IMG2AddMemorySample(Case.MemorySamples, TEXT("case_ready_before_build"));
			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(Case.MemorySamples.Last()))
			{
				Case.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
				Case.Error = TEXT("Available system RAM fell below the safety floor after loading the case map and volume.");
				bResourceGuardHit = true;
				ResourceGuardReason = Case.Error;
				Cases.Add(MoveTemp(Case));
				Fixture.Cleanup();
				break;
			}

			bool bCaseResourceGuard = false;
			if (!IMAcousticG2ScaleCaptureTestPrivate::IMG2BuildScene(Fixture, CurrentSource, Representation, Transforms, Case, bCaseResourceGuard, Failure))
			{
				Case.Error = Failure;
				if (bCaseResourceGuard)
				{
					Case.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
					bResourceGuardHit = true;
					ResourceGuardReason = Failure;
				}
				else
				{
					Case.Outcome = TEXT("FAIL_BUILD");
					bHardFailure = true;
				}
				Cases.Add(MoveTemp(Case));
				Fixture.Cleanup();
				break;
			}

			const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample BeforeCapture = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("pre_capture"));
			Case.MemorySamples.Add(BeforeCapture);
			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(BeforeCapture))
			{
				Case.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
				Case.Error = TEXT("Available system RAM fell below the safety floor before capture.");
				bResourceGuardHit = true;
				ResourceGuardReason = Case.Error;
				Cases.Add(MoveTemp(Case));
				Fixture.Cleanup();
				break;
			}

			TArray<FVector> Vertices;
			TArray<FIntVector> Triangles;
			TArray<int32> MaterialIndices;
			FString SceneFingerprint;
			const double CaptureStarted = FPlatformTime::Seconds();
			Case.bCaptureSucceeded = Fixture.GetVolume()
				&& Fixture.GetVolume()->IM_CaptureSceneGeometryForTest(Vertices, Triangles, MaterialIndices, SceneFingerprint);
			Case.CaptureAccessorWallSeconds = FPlatformTime::Seconds() - CaptureStarted;
			const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample AfterCapture = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("post_capture"));
			Case.MemorySamples.Add(AfterCapture);
			Case.CapturedVertices = Vertices.Num();
			Case.CapturedTriangles = Triangles.Num();
			Case.CapturedMaterialIndices = MaterialIndices.Num();
			Case.SceneFingerprint = SceneFingerprint;
			for (const int32 MaterialIndex : MaterialIndices) if (MaterialIndex != 0) ++Case.NonZeroMaterialIndices;
			Case.bTriangleCountExact = Case.bCaptureSucceeded && Case.CapturedTriangles == Case.ExpectedTriangles;
			Case.bMaterialCountExact = Case.bCaptureSucceeded && Case.CapturedMaterialIndices == Case.ExpectedTriangles
				&& Case.NonZeroMaterialIndices == 0;

			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(AfterCapture))
			{
				Case.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
				Case.Error = TEXT("Available system RAM fell below the safety floor after capture; duplicate validation capture was skipped.");
				bResourceGuardHit = true;
				ResourceGuardReason = Case.Error;
				Cases.Add(MoveTemp(Case));
				Fixture.Cleanup();
				break;
			}

			Vertices.Empty();
			Triangles.Empty();
			MaterialIndices.Empty();
			const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample BeforeValidation = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("pre_validate_current_bake"));
			Case.MemorySamples.Add(BeforeValidation);
			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(BeforeValidation))
			{
				Case.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
				Case.Error = TEXT("Available system RAM fell below the safety floor before the validation recapture.");
				bResourceGuardHit = true;
				ResourceGuardReason = Case.Error;
				Cases.Add(MoveTemp(Case));
				Fixture.Cleanup();
				break;
			}

			FString ValidationFailure;
			const double ValidationStarted = FPlatformTime::Seconds();
			Case.bValidationReturnedTrue = Fixture.GetVolume()->ValidateCurrentBake(ValidationFailure);
			Case.ValidateCurrentBakeWallSeconds = FPlatformTime::Seconds() - ValidationStarted;
			Case.ValidationFailure = ValidationFailure;
			const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample AfterValidation = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("post_validate_current_bake"));
			Case.MemorySamples.Add(AfterValidation);
			Case.bMissingAssetGuardObserved = !Case.bValidationReturnedTrue
				&& ValidationFailure == TEXT("Missing acoustic bake asset.");

			if (!Case.bCaptureSucceeded)
			{
				Case.Outcome = TEXT("FAIL_CAPTURE");
				Case.Error = Fixture.GetVolume() ? Fixture.GetVolume()->Status : TEXT("Transient capture volume disappeared.");
				bHardFailure = true;
			}
			else if (!Case.bTriangleCountExact || !Case.bMaterialCountExact)
			{
				Case.Outcome = TEXT("FAIL_GEOMETRY_COUNTS");
				Case.Error = FString::Printf(TEXT("Expected %d triangles and material indices; captured %d triangles and %d indices with %d nonzero material indices."),
					Case.ExpectedTriangles, Case.CapturedTriangles, Case.CapturedMaterialIndices, Case.NonZeroMaterialIndices);
				bHardFailure = true;
			}
			else if (!Case.bMissingAssetGuardObserved)
			{
				Case.Outcome = TEXT("FAIL_VALIDATION_GUARD");
				Case.Error = Case.bValidationReturnedTrue
					? TEXT("ValidateCurrentBake unexpectedly accepted a transient volume with no bound bake asset.")
					: TEXT("ValidateCurrentBake did not reach the expected missing-asset guard: ") + ValidationFailure;
				bHardFailure = true;
			}
			else
			{
				Case.Outcome = TEXT("PASS_CAPTURE_AND_MISSING_ASSET_GUARD");
			}

			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(AfterValidation))
			{
				bResourceGuardHit = true;
				ResourceGuardReason = FString::Printf(TEXT("Available system RAM fell below %.2f GiB after %s; larger cases were stopped."),
					IMAcousticG2ScaleCaptureTestPrivate::IMG2ResourceGuardBytes / (1024.0 * 1024.0 * 1024.0), *CaseName);
			}
			Cases.Add(MoveTemp(Case));
			Fixture.Cleanup();
			if (bHardFailure || bResourceGuardHit) break;
		}
	}

	for (int32 CountIndex = 0; CountIndex < UE_ARRAY_COUNT(IMAcousticG2ScaleCaptureTestPrivate::IMG2Counts); ++CountIndex)
	{
		for (const IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation Representation : {IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation::IndependentStaticMeshes, IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation::ISM, IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation::HISM})
		{
			const FString Name = FString::Printf(TEXT("N%d_%s"), IMAcousticG2ScaleCaptureTestPrivate::IMG2Counts[CountIndex], IMAcousticG2ScaleCaptureTestPrivate::IMG2RepresentationName(Representation));
			const bool bRecorded = Cases.ContainsByPredicate([&](const IMAcousticG2ScaleCaptureTestPrivate::FIMG2Case& Case) { return Case.Name == Name; });
			if (!bRecorded) SkippedCases.Add(Name);
		}
	}

	FString RestoreError;
	const bool bRestoreSucceeded = Fixture.RestoreAndCompare(FinalMapIdentity, FinalAssetIdentity, RestoreError);
	bOriginalMapRestored = bRestoreSucceeded && FinalMapIdentity == InitialSource.MapIdentity;
	bOriginalAssetsUnchanged = bRestoreSucceeded && FinalAssetIdentity == InitialSource.OriginalBakeAssetIdentity;
	if (!bRestoreSucceeded)
	{
		if (!Failure.IsEmpty()) Failure += TEXT("; ");
		Failure += RestoreError.IsEmpty() ? TEXT("Final map/asset identity differs from its baseline.") : RestoreError;
		bHardFailure = true;
	}
	IMAcousticG2ScaleCaptureTestPrivate::IMG2AddMemorySample(RunMemorySamples, TEXT("final_map_reloaded"));

	const bool bAllCasesPassed = Cases.Num() == UE_ARRAY_COUNT(IMAcousticG2ScaleCaptureTestPrivate::IMG2Counts) * 3
		&& Cases.ContainsByPredicate([](const IMAcousticG2ScaleCaptureTestPrivate::FIMG2Case& Case) { return Case.Outcome != TEXT("PASS_CAPTURE_AND_MISSING_ASSET_GUARD"); }) == false;
	FString FinalResult = bHardFailure ? TEXT("FAIL") : ((bResourceGuardHit || !bAllCasesPassed) ? TEXT("INCONCLUSIVE") : TEXT("PASS"));
	const bool bReportSaved = IMAcousticG2ScaleCaptureTestPrivate::IMG2WriteReport(Directory, ReportPath, RunId, InitialSource,
		FinalMapIdentity, FinalAssetIdentity, bOriginalMapRestored, bOriginalAssetsUnchanged,
		bResourceGuardHit, ResourceGuardReason, FinalResult, RunMemorySamples, Cases, SkippedCases, CubeScale);
	if (!bReportSaved)
	{
		AddError(TEXT("Required G2 capture report could not be written: ") + ReportPath);
		bHardFailure = true;
		FinalResult = TEXT("FAIL");
	}

	if (bHardFailure)
	{
		AddError(Failure.IsEmpty() ? TEXT("G2 capture scale test failed; see report.json for case details.") : Failure);
	}
	else if (FinalResult == TEXT("INCONCLUSIVE"))
	{
		AddWarning(TEXT("G2 capture scale run is INCONCLUSIVE: ") + ResourceGuardReason);
		AddError(TEXT("G2 capture scale run stopped before all nine cases; see report.json for the resource guard and skipped cases."));
	}

	const TCHAR* DiagnosticResult = FinalResult == TEXT("PASS") ? TEXT("PASS")
		: FinalResult == TEXT("INCONCLUSIVE") ? TEXT("INCONCLUSIVE") : TEXT("ERROR");
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticG2CaptureScale run=%s cases=%d map_restored=%d assets_unchanged=%d report_saved=%d result=%s report=%s"),
		*RunId, Cases.Num(), bOriginalMapRestored, bOriginalAssetsUnchanged, bReportSaved, DiagnosticResult, *ReportPath);
	if (FinalResult == TEXT("PASS") && bReportSaved && bOriginalMapRestored && bOriginalAssetsUnchanged)
	{
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] G2CaptureScale PASS"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS G2CaptureScale"));
		return true;
	}
	return false;
}

namespace IMAcousticG2ScaleCaptureTestPrivate
{
constexpr int32 IMG2DetailedLogCostPlacements = 3000;
constexpr const TCHAR* IMG2DetailedLogCostCVarName = TEXT("IM.AcousticField.DetailedGeometryLog");

struct FIMG2DetailedLogCounts
{
	int32 Bounds = 0;
	int32 Actor = 0;
	int32 Place = 0;
	uint64 SerializedTCharBytes = 0;
};

struct FIMG2DetailedLogCapture
{
	FString Mode;
	FString Fingerprint;
	FString Error;
	int32 Vertices = 0;
	int32 Triangles = 0;
	int32 MaterialIndices = 0;
	int32 NonzeroMaterialIndices = 0;
	double CaptureCoreSeconds = 0.0;
	double LogDrainSeconds = 0.0;
	double CaptureWallSeconds = 0.0;
	bool bDetailedLogRequested = false;
	bool bCaptureSucceeded = false;
	bool bGeometryCountsExact = false;
	bool bFingerprintMatchesFirst = false;
	bool bVerticesMatchFirst = false;
	bool bTrianglesMatchFirst = false;
	bool bMaterialsMatchFirst = false;
	bool bLogCountsExact = false;
	bool bLogBytesValid = false;
	FIMG2DetailedLogCounts Logs;
	FIMG2MemorySample Before;
	FIMG2MemorySample After;
};

class FIMG2DetailedLogSetting
{
public:
	FIMG2DetailedLogSetting()
		: Variable(IConsoleManager::Get().FindConsoleVariable(IMG2DetailedLogCostCVarName))
	{
		if (Variable)
		{
			OriginalValue = Variable->GetInt();
		}
	}

	~FIMG2DetailedLogSetting()
	{
		if (Variable && !bRestored)
		{
			Variable->SetWithCurrentPriority(OriginalValue);
		}
	}

	bool IsAvailable() const { return Variable != nullptr; }
	int32 GetOriginalValue() const { return OriginalValue; }
	int32 GetCurrentValue() const { return Variable ? Variable->GetInt() : -1; }

	bool SetEnabled(bool bEnabled, FString& Error)
	{
		if (!Variable)
		{
			Error = FString::Printf(TEXT("Required logging CVar is unavailable: %s"), IMG2DetailedLogCostCVarName);
			return false;
		}
		const int32 Expected = bEnabled ? 1 : 0;
		Variable->SetWithCurrentPriority(Expected);
		if (Variable->GetInt() != Expected)
		{
			Error = FString::Printf(TEXT("Could not set %s to %d; current value is %d."),
				IMG2DetailedLogCostCVarName, Expected, Variable->GetInt());
			return false;
		}
		return true;
	}

	bool Restore(FString& Error)
	{
		if (!Variable)
		{
			Error = FString::Printf(TEXT("Cannot restore unavailable logging CVar: %s"), IMG2DetailedLogCostCVarName);
			return false;
		}
		Variable->SetWithCurrentPriority(OriginalValue);
		bRestored = Variable->GetInt() == OriginalValue;
		if (!bRestored)
		{
			Error = FString::Printf(TEXT("%s did not return to its original value %d (current %d)."),
				IMG2DetailedLogCostCVarName, OriginalValue, Variable->GetInt());
		}
		return bRestored;
	}

private:
	IConsoleVariable* Variable = nullptr;
	int32 OriginalValue = 0;
	bool bRestored = false;
};

class FIMG2DetailedLogObserver final : public FOutputDevice
{
public:
	void BeginCapture()
	{
		FScopeLock Lock(&Mutex);
		Current = FIMG2DetailedLogCounts();
		bCapturing = true;
	}

	FIMG2DetailedLogCounts EndCapture()
	{
		FScopeLock Lock(&Mutex);
		bCapturing = false;
		return Current;
	}

	void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
	{
		if (!Text) return;
		int32 Family = INDEX_NONE;
		if (FCString::Strstr(Text, TEXT("IMLogs AcousticBakeBounds"))) Family = 0;
		else if (FCString::Strstr(Text, TEXT("IMLogs AcousticBakeActor"))) Family = 1;
		else if (FCString::Strstr(Text, TEXT("IMLogs AcousticBakePlace"))) Family = 2;
		if (Family == INDEX_NONE) return;

		const uint64 PayloadBytes = static_cast<uint64>(FCString::Strlen(Text)) * sizeof(TCHAR);
		FScopeLock Lock(&Mutex);
		if (!bCapturing) return;
		if (Family == 0) ++Current.Bounds;
		else if (Family == 1) ++Current.Actor;
		else ++Current.Place;
		Current.SerializedTCharBytes += PayloadBytes;
	}

private:
	FCriticalSection Mutex;
	FIMG2DetailedLogCounts Current;
	bool bCapturing = false;
};

class FIMG2DetailedLogObserverScope
{
public:
	~FIMG2DetailedLogObserverScope() { Detach(); }

	bool Attach()
	{
		if (!GLog) return false;
		GLog->AddOutputDevice(&Observer);
		bAttached = true;
		return true;
	}

	void Detach()
	{
		if (bAttached && GLog)
		{
			GLog->RemoveOutputDevice(&Observer);
			bAttached = false;
		}
	}

	FIMG2DetailedLogObserver& GetObserver() { return Observer; }

private:
	FIMG2DetailedLogObserver Observer;
	bool bAttached = false;
};

static bool IMG2DetailedVectorsEqual(const TArray<FVector>& A, const TArray<FVector>& B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index].X != B[Index].X || A[Index].Y != B[Index].Y || A[Index].Z != B[Index].Z)
			return false;
	}
	return true;
}

static bool IMG2DetailedTrianglesEqual(const TArray<FIntVector>& A, const TArray<FIntVector>& B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index].X != B[Index].X || A[Index].Y != B[Index].Y || A[Index].Z != B[Index].Z)
			return false;
	}
	return true;
}

static bool IMG2DetailedIntsEqual(const TArray<int32>& A, const TArray<int32>& B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index] != B[Index]) return false;
	}
	return true;
}

template<typename TWriter>
static void IMG2WriteDetailedLogCapture(TWriter& Writer, const FIMG2DetailedLogCapture& Capture)
{
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("mode"), Capture.Mode);
	Writer->WriteValue(TEXT("detailed_log_requested"), Capture.bDetailedLogRequested);
	Writer->WriteValue(TEXT("capture_succeeded"), Capture.bCaptureSucceeded);
	Writer->WriteValue(TEXT("capture_core_seconds_including_accessor_readback"), Capture.CaptureCoreSeconds);
	Writer->WriteValue(TEXT("log_drain_seconds"), Capture.LogDrainSeconds);
	Writer->WriteValue(TEXT("capture_and_drain_wall_seconds"), Capture.CaptureWallSeconds);
	Writer->WriteValue(TEXT("vertices"), Capture.Vertices);
	Writer->WriteValue(TEXT("triangles"), Capture.Triangles);
	Writer->WriteValue(TEXT("material_indices"), Capture.MaterialIndices);
	Writer->WriteValue(TEXT("nonzero_material_indices"), Capture.NonzeroMaterialIndices);
	Writer->WriteValue(TEXT("geometry_counts_exact"), Capture.bGeometryCountsExact);
	Writer->WriteValue(TEXT("scene_fingerprint"), Capture.Fingerprint);
	Writer->WriteValue(TEXT("fingerprint_matches_first"), Capture.bFingerprintMatchesFirst);
	Writer->WriteValue(TEXT("vertices_match_first_exactly"), Capture.bVerticesMatchFirst);
	Writer->WriteValue(TEXT("triangles_match_first_exactly"), Capture.bTrianglesMatchFirst);
	Writer->WriteValue(TEXT("material_indices_match_first_exactly"), Capture.bMaterialsMatchFirst);
	Writer->WriteValue(TEXT("log_counts_exact"), Capture.bLogCountsExact);
	Writer->WriteValue(TEXT("log_bytes_valid"), Capture.bLogBytesValid);
	Writer->WriteValue(TEXT("error"), Capture.Error);
	Writer->WriteObjectStart(TEXT("detailed_log_counts"));
	Writer->WriteValue(TEXT("bounds"), Capture.Logs.Bounds);
	Writer->WriteValue(TEXT("actor"), Capture.Logs.Actor);
	Writer->WriteValue(TEXT("place"), Capture.Logs.Place);
	Writer->WriteValue(TEXT("serialized_tchar_text_argument_bytes"),
		static_cast<double>(Capture.Logs.SerializedTCharBytes));
	Writer->WriteObjectEnd();
	auto WriteMemoryField = [&](const TCHAR* Name, const FIMG2MemorySample& Sample)
	{
		Writer->WriteObjectStart(Name);
		Writer->WriteValue(TEXT("stage"), Sample.Stage);
		Writer->WriteValue(TEXT("used_physical_bytes"), static_cast<double>(Sample.UsedPhysical));
		Writer->WriteValue(TEXT("peak_used_physical_bytes"), static_cast<double>(Sample.PeakUsedPhysical));
		Writer->WriteValue(TEXT("used_virtual_bytes"), static_cast<double>(Sample.UsedVirtual));
		Writer->WriteValue(TEXT("peak_used_virtual_bytes"), static_cast<double>(Sample.PeakUsedVirtual));
		Writer->WriteValue(TEXT("available_system_physical_bytes"), static_cast<double>(Sample.AvailablePhysical));
		Writer->WriteObjectEnd();
	};
	WriteMemoryField(TEXT("memory_before"), Capture.Before);
	WriteMemoryField(TEXT("memory_after"), Capture.After);
	Writer->WriteObjectEnd();
}

static bool IMG2WriteDetailedLogCostReport(const FString& Directory, const FString& ReportPath,
	const FString& RunId, const FIMG2Source& Source, const FString& FinalMapIdentity,
	const FString& FinalAssetIdentity, const FString& TransformFingerprint,
	const FString& Failure,
	bool bMapRestored, bool bAssetsUnchanged, bool bCVarRestored, int32 OriginalCVarValue,
	int32 FinalCVarValue, bool bResourceGuardHit, const FString& ResourceGuardReason,
	const FString& FinalResult, float CubeScale, int32 ExpectedVertices, int32 ExpectedTriangles,
	int32 BuiltPlacements, int32 PrimitiveComponents, double PlacementBuildSeconds,
	const TArray<FIMG2MemorySample>& RunMemory,
	const TArray<FIMG2DetailedLogCapture>& Captures, bool bCapturePairSummaryValid)
{
	double EnabledAverageSeconds = 0.0;
	double DisabledAverageSeconds = 0.0;
	if (bCapturePairSummaryValid && Captures.Num() == 4)
	{
		EnabledAverageSeconds = (Captures[0].CaptureWallSeconds + Captures[3].CaptureWallSeconds) * 0.5;
		DisabledAverageSeconds = (Captures[1].CaptureWallSeconds + Captures[2].CaptureWallSeconds) * 0.5;
	}

	uint64 PeakProcessPhysical = 0;
	uint64 PeakProcessVirtual = 0;
	uint64 MinimumAvailablePhysical = MAX_uint64;
	auto AccumulateMemory = [&](const FIMG2MemorySample& Sample)
	{
		PeakProcessPhysical = FMath::Max(PeakProcessPhysical, Sample.PeakUsedPhysical);
		PeakProcessVirtual = FMath::Max(PeakProcessVirtual, Sample.PeakUsedVirtual);
		MinimumAvailablePhysical = FMath::Min(MinimumAvailablePhysical, Sample.AvailablePhysical);
	};
	for (const FIMG2MemorySample& Sample : RunMemory) AccumulateMemory(Sample);
	for (const FIMG2DetailedLogCapture& Capture : Captures)
	{
		if (!Capture.Before.Stage.IsEmpty()) AccumulateMemory(Capture.Before);
		if (!Capture.After.Stage.IsEmpty()) AccumulateMemory(Capture.After);
	}

	FString Json;
	auto Writer = TJsonWriterFactory<>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema"), TEXT("im_acoustic_g2_detailed_log_cost_v2"));
	Writer->WriteValue(TEXT("run_id"), RunId);
	Writer->WriteValue(TEXT("automation_test"), TEXT("IceMoon.AcousticField.G2.DetailedLogCost3000"));
	Writer->WriteValue(TEXT("result"), FinalResult);
	Writer->WriteValue(TEXT("failure"), Failure);
	Writer->WriteValue(TEXT("scope"), TEXT("One fixed 3000-placement independent StaticMesh scene; captures alternate detailed diagnostic logging ON/OFF/OFF/ON. No SDK Bake."));
	Writer->WriteValue(TEXT("map_path"), IMG2Map);
	Writer->WriteValue(TEXT("world_package"), Source.WorldPackage);
	Writer->WriteValue(TEXT("baseline_map_identity"), Source.MapIdentity);
	Writer->WriteValue(TEXT("final_map_identity"), FinalMapIdentity);
	Writer->WriteValue(TEXT("original_map_restored"), bMapRestored);
	Writer->WriteValue(TEXT("original_bake_asset_identity"), Source.OriginalBakeAssetIdentity);
	Writer->WriteValue(TEXT("final_bake_asset_identity"), FinalAssetIdentity);
	Writer->WriteValue(TEXT("bake_asset_identity_storage_basis"), TEXT("Serialized SceneData and ProbeData plus metadata; Format 2 stores raw sections and Format 3 stores packed sections."));
	Writer->WriteValue(TEXT("original_bake_assets_unchanged"), bAssetsUnchanged);
	Writer->WriteValue(TEXT("whitebox_identity"), Source.WhiteboxIdentity);
	Writer->WriteValue(TEXT("whitebox_components"), Source.WhiteboxComponentCount);
	Writer->WriteValue(TEXT("cube_mesh_path"), Source.MeshPath);
	Writer->WriteValue(TEXT("cube_material_path"), Source.MaterialPath);
	Writer->WriteValue(TEXT("cube_lod"), 0);
	Writer->WriteValue(TEXT("cube_lod0_triangles"), Source.CubeLOD0Triangles);
	Writer->WriteValue(TEXT("requested_placements"), IMG2DetailedLogCostPlacements);
	Writer->WriteValue(TEXT("built_placements"), BuiltPlacements);
	Writer->WriteValue(TEXT("primitive_components"), PrimitiveComponents);
	Writer->WriteValue(TEXT("cube_uniform_scale"), CubeScale);
	Writer->WriteValue(TEXT("placement_region_width_m"), 40.0);
	Writer->WriteValue(TEXT("placement_region_height_m"), 32.0);
	Writer->WriteValue(TEXT("probe_spacing_cm"), IMG2ProbeSpacingCm);
	Writer->WriteValue(TEXT("probe_height_cm"), IMG2ProbeHeightCm);
	Writer->WriteValue(TEXT("material_absorption_rgb"), FVector(IMG2MaterialAbsorption).ToString());
	Writer->WriteValue(TEXT("material_scattering"), IMG2MaterialScattering);
	Writer->WriteValue(TEXT("transform_fingerprint"), TransformFingerprint);
	Writer->WriteValue(TEXT("expected_vertices"), ExpectedVertices);
	Writer->WriteValue(TEXT("expected_triangles"), ExpectedTriangles);
	Writer->WriteValue(TEXT("placement_build_seconds"), PlacementBuildSeconds);
	Writer->WriteValue(TEXT("logging_cvar"), IMG2DetailedLogCostCVarName);
	Writer->WriteValue(TEXT("logging_cvar_original_value"), OriginalCVarValue);
	Writer->WriteValue(TEXT("logging_cvar_final_value"), FinalCVarValue);
	Writer->WriteValue(TEXT("logging_cvar_restored"), bCVarRestored);
	Writer->WriteValue(TEXT("logging_cvar_priority_method"), TEXT("SetWithCurrentPriority preserves the current SetBy source while changing and restoring the temporary value."));
	Writer->WriteValue(TEXT("capture_order"), TEXT("ON,OFF,OFF,ON"));
	Writer->WriteValue(TEXT("capture_pair_summary_valid"), bCapturePairSummaryValid);
	Writer->WriteValue(TEXT("enabled_capture_average_seconds"), EnabledAverageSeconds);
	Writer->WriteValue(TEXT("disabled_capture_average_seconds"), DisabledAverageSeconds);
	Writer->WriteValue(TEXT("enabled_minus_disabled_seconds"), EnabledAverageSeconds - DisabledAverageSeconds);
	Writer->WriteValue(TEXT("resource_guard_hit"), bResourceGuardHit);
	Writer->WriteValue(TEXT("resource_guard_available_system_ram_bytes"), static_cast<double>(IMG2ResourceGuardBytes));
	Writer->WriteValue(TEXT("resource_guard_reason"), ResourceGuardReason);
	Writer->WriteValue(TEXT("process_peak_working_set_bytes_observed"), static_cast<double>(PeakProcessPhysical));
	Writer->WriteValue(TEXT("process_peak_commit_bytes_observed"), static_cast<double>(PeakProcessVirtual));
	Writer->WriteValue(TEXT("minimum_available_system_ram_bytes_observed"),
		MinimumAvailablePhysical == MAX_uint64 ? 0.0 : static_cast<double>(MinimumAvailablePhysical));
	Writer->WriteValue(TEXT("memory_semantics"),
		TEXT("Windows FPlatformMemoryStats UsedPhysical/PeakUsedPhysical are process working set; UsedVirtual/PeakUsedVirtual are process commit; AvailablePhysical is system available RAM."));
	Writer->WriteValue(TEXT("timing_semantics"),
		TEXT("Core time includes CaptureScene and test accessor array readback; drain time synchronously delivers pending records to the always-attached GLog observer. Their sum is the capture-and-drain wall time. Only detailed log emission changes; geometry export, diagnostics, fingerprinting, and recipe are held constant."));
	Writer->WriteValue(TEXT("log_byte_semantics"),
		TEXT("Bytes are FCString::Strlen(Serialize Text) * sizeof(TCHAR) for the three detailed log families; this is observer text size, not whole editor log-file bytes."));

	Writer->WriteArrayStart(TEXT("run_memory_samples"));
	for (const FIMG2MemorySample& Sample : RunMemory) IMG2WriteMemorySample(Writer, Sample);
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("captures"));
	for (const FIMG2DetailedLogCapture& Capture : Captures) IMG2WriteDetailedLogCapture(Writer, Capture);
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("not_covered"));
	Writer->WriteValue(TEXT("Steam Audio SDK bake, saved bake payload validation, editor ticker scheduling, acoustic output quality, and runtime spatialization."));
	Writer->WriteValue(TEXT("Causal CPU attribution is limited to this logging A/B; report the measured delta without treating log byte volume alone as timing evidence."));
	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("report_path"), ReportPath);
	Writer->WriteObjectEnd();
	Writer->Close();
	return IFileManager::Get().MakeDirectory(*Directory, true)
		&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticG2DetailedLogCost3000,
	"IceMoon.AcousticField.G2.DetailedLogCost3000",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool IM_AcousticG2DetailedLogCost3000::RunTest(const FString&)
{
	IMAcousticG2ScaleCaptureTestPrivate::FIMG2Fixture Fixture;
	IMAcousticG2ScaleCaptureTestPrivate::FIMG2DetailedLogSetting LogSetting;
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/G2CaptureDetailedLog"), RunId);
	const FString ReportPath = FPaths::Combine(Directory, TEXT("report.json"));
	TArray<IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample> RunMemory;
	TArray<IMAcousticG2ScaleCaptureTestPrivate::FIMG2DetailedLogCapture> Captures;
	IMAcousticG2ScaleCaptureTestPrivate::IMG2AddMemorySample(RunMemory, TEXT("run_start"));

	FString Failure;
	FString ResourceGuardReason;
	FString FinalMapIdentity;
	FString FinalAssetIdentity;
	FString TransformFingerprint;
	bool bHardFailure = false;
	bool bResourceGuardHit = false;
	bool bMapRestored = false;
	bool bAssetsUnchanged = false;
	bool bCVarRestored = false;
	bool bObserverAttached = false;
	bool bBaselineCaptured = false;
	double PlacementBuildSeconds = 0.0;
	float CubeScale = 0.0f;
	int32 ExpectedVertices = 0;
	int32 ExpectedTriangles = 0;
	int32 BuiltPlacements = 0;
	int32 PrimitiveComponents = 0;
	IMAcousticG2ScaleCaptureTestPrivate::FIMG2Source InitialSource;
	TArray<FVector> BaselineVertices;
	TArray<FIntVector> BaselineTriangles;
	TArray<int32> BaselineMaterials;
	FString BaselineFingerprint;

	auto RecordFailure = [&](const FString& Message)
	{
		if (!Message.IsEmpty() && Failure != Message)
		{
			if (Failure.IsEmpty()) Failure = Message;
			else Failure += TEXT(" | ") + Message;
		}
		bHardFailure = true;
	};
	auto RecordResourceGuard = [&](const FString& Message)
	{
		bResourceGuardHit = true;
		if (ResourceGuardReason.IsEmpty()) ResourceGuardReason = Message;
	};

	if (!LogSetting.IsAvailable())
	{
		RecordFailure(FString::Printf(TEXT("Required CVar is not registered: %s"), IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedLogCostCVarName));
	}

	if (!bHardFailure && !Fixture.Prime(InitialSource, Failure))
	{
		bHardFailure = true;
	}
	if (!bHardFailure)
	{
		IMAcousticG2ScaleCaptureTestPrivate::IMG2AddMemorySample(RunMemory, TEXT("mother_map_primed"));
		const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample Preflight = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("before_3000_scene_build"));
		RunMemory.Add(Preflight);
		if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(Preflight))
		{
			RecordResourceGuard(FString::Printf(TEXT("Available system RAM was %.2f GiB before the fixed 3000-placement scene."),
				Preflight.AvailablePhysical / (1024.0 * 1024.0 * 1024.0)));
		}
	}

	TArray<FTransform> Transforms;
	if (!bHardFailure && !bResourceGuardHit)
	{
		CubeScale = IMAcousticG2ScaleCaptureTestPrivate::IMG2ChooseCubeScale(InitialSource);
		if (CubeScale <= 0.0f)
		{
			RecordFailure(TEXT("Could not choose a finite Cube scale for the fixed 3000-placement grid."));
		}
		else if (InitialSource.CubeLOD0Triangles > MAX_int32 / IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedLogCostPlacements)
		{
			RecordFailure(TEXT("Cube LOD0 triangle count exceeds exact int32 reporting range."));
		}
		else if (!IMAcousticG2ScaleCaptureTestPrivate::IMG2BuildTransforms(IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedLogCostPlacements, InitialSource, CubeScale, Transforms, Failure))
		{
			bHardFailure = true;
		}
		else
		{
			ExpectedTriangles = IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedLogCostPlacements * InitialSource.CubeLOD0Triangles;
			TArray<FString> TransformItems;
			TransformItems.Reserve(Transforms.Num());
			for (const FTransform& Transform : Transforms) TransformItems.Add(Transform.ToString());
			TransformFingerprint = IMAcousticG2ScaleCaptureTestPrivate::IMG2HashStrings(MoveTemp(TransformItems));

			if (UStaticMesh* Mesh = InitialSource.Mesh.Get())
			{
				const uint64 VertexCount = static_cast<uint64>(Mesh->GetRenderData()->LODResources[0]
					.VertexBuffers.PositionVertexBuffer.GetNumVertices()) * IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedLogCostPlacements;
				if (VertexCount > MAX_int32)
				{
					RecordFailure(TEXT("Cube LOD0 vertex count exceeds exact int32 reporting range."));
				}
				else
				{
					ExpectedVertices = static_cast<int32>(VertexCount);
				}
			}
			else
			{
				RecordFailure(TEXT("Whitebox Cube mesh disappeared before the 3000-placement build."));
			}
		}
	}

	if (!bHardFailure && !bResourceGuardHit)
	{
		IMAcousticG2ScaleCaptureTestPrivate::FIMG2Source CurrentSource;
		if (!Fixture.PrepareCase(CurrentSource, Failure))
		{
			bHardFailure = true;
		}
		else
		{
			IMAcousticG2ScaleCaptureTestPrivate::FIMG2Case BuildCase;
			BuildCase.Name = TEXT("N3000_independent_sm");
			BuildCase.Representation = TEXT("independent_sm");
			BuildCase.RequestedPlacements = IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedLogCostPlacements;
			BuildCase.CubeLOD0Triangles = InitialSource.CubeLOD0Triangles;
			BuildCase.ExpectedTriangles = ExpectedTriangles;
			bool bBuildResourceGuard = false;
			if (!IMAcousticG2ScaleCaptureTestPrivate::IMG2BuildScene(Fixture, CurrentSource, IMAcousticG2ScaleCaptureTestPrivate::EIMG2Representation::IndependentStaticMeshes,
				Transforms, BuildCase, bBuildResourceGuard, Failure))
			{
				if (bBuildResourceGuard) RecordResourceGuard(Failure);
				else bHardFailure = true;
			}
			BuiltPlacements = BuildCase.BuiltPlacements;
			PrimitiveComponents = BuildCase.PrimitiveComponents;
			PlacementBuildSeconds = BuildCase.ConstructionWallSeconds;
			if (!bHardFailure && !bResourceGuardHit)
			{
				const IMAcousticG2ScaleCaptureTestPrivate::FIMG2MemorySample Built = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("3000_scene_built"));
				RunMemory.Add(Built);
				if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(Built))
				{
					RecordResourceGuard(TEXT("Available system RAM fell below the 8 GiB floor after building the fixed scene."));
				}
			}
		}
	}

	IMAcousticG2ScaleCaptureTestPrivate::FIMG2DetailedLogObserverScope ObserverScope;
	if (!bHardFailure && !bResourceGuardHit)
	{
		bObserverAttached = ObserverScope.Attach();
		if (!bObserverAttached)
		{
			RecordFailure(TEXT("Could not attach the detailed-log observer to GLog."));
		}
	}

	const bool bCaptureModes[] = { true, false, false, true };
	const TCHAR* CaptureLabels[] = { TEXT("logging_on_1"), TEXT("logging_off_1"), TEXT("logging_off_2"), TEXT("logging_on_2") };
	if (!bHardFailure && !bResourceGuardHit)
	{
		for (int32 CaptureIndex = 0; CaptureIndex < UE_ARRAY_COUNT(bCaptureModes); ++CaptureIndex)
		{
			IMAcousticG2ScaleCaptureTestPrivate::FIMG2DetailedLogCapture Capture;
			Capture.Mode = CaptureLabels[CaptureIndex];
			Capture.bDetailedLogRequested = bCaptureModes[CaptureIndex];
			if (!LogSetting.SetEnabled(Capture.bDetailedLogRequested, Capture.Error))
			{
				RecordFailure(Capture.Error);
				Captures.Add(MoveTemp(Capture));
				break;
			}

			Capture.Before = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("before_log_cost_capture"));
			RunMemory.Add(Capture.Before);
			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(Capture.Before))
			{
				RecordResourceGuard(TEXT("Available system RAM fell below the 8 GiB floor before a paired capture."));
				Capture.Error = ResourceGuardReason;
				Captures.Add(MoveTemp(Capture));
				break;
			}

			TArray<FVector> Vertices;
			TArray<FIntVector> Triangles;
			TArray<int32> MaterialIndices;
			FString SceneFingerprint;
			// The Editor may serialize records on a dedicated logging thread. Drain
			// the previous mode before opening this window, then include the current
			// mode's drain in its measured wall time and observed log counts.
			GLog->FlushThreadedLogs();
			ObserverScope.GetObserver().BeginCapture();
			const double CaptureStarted = FPlatformTime::Seconds();
			Capture.bCaptureSucceeded = Fixture.GetVolume()
				&& Fixture.GetVolume()->IM_CaptureSceneGeometryForTest(Vertices, Triangles, MaterialIndices, SceneFingerprint);
			const double CaptureCoreEnded = FPlatformTime::Seconds();
			GLog->FlushThreadedLogs();
			const double CaptureDrained = FPlatformTime::Seconds();
			Capture.CaptureCoreSeconds = CaptureCoreEnded - CaptureStarted;
			Capture.LogDrainSeconds = CaptureDrained - CaptureCoreEnded;
			Capture.CaptureWallSeconds = CaptureDrained - CaptureStarted;
			Capture.Logs = ObserverScope.GetObserver().EndCapture();

			Capture.Vertices = Vertices.Num();
			Capture.Triangles = Triangles.Num();
			Capture.MaterialIndices = MaterialIndices.Num();
			Capture.Fingerprint = SceneFingerprint;
			for (const int32 MaterialIndex : MaterialIndices)
				if (MaterialIndex != 0) ++Capture.NonzeroMaterialIndices;
			Capture.bGeometryCountsExact = Capture.bCaptureSucceeded
				&& Capture.Vertices == ExpectedVertices
				&& Capture.Triangles == ExpectedTriangles
				&& Capture.MaterialIndices == Capture.Triangles
				&& Capture.NonzeroMaterialIndices == 0;

			if (Capture.bCaptureSucceeded && !bBaselineCaptured)
			{
				BaselineVertices = Vertices;
				BaselineTriangles = Triangles;
				BaselineMaterials = MaterialIndices;
				BaselineFingerprint = SceneFingerprint;
				bBaselineCaptured = true;
				Capture.bFingerprintMatchesFirst = !SceneFingerprint.IsEmpty();
				Capture.bVerticesMatchFirst = true;
				Capture.bTrianglesMatchFirst = true;
				Capture.bMaterialsMatchFirst = true;
			}
			else if (Capture.bCaptureSucceeded && bBaselineCaptured)
			{
				Capture.bFingerprintMatchesFirst = SceneFingerprint == BaselineFingerprint;
				Capture.bVerticesMatchFirst = IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedVectorsEqual(Vertices, BaselineVertices);
				Capture.bTrianglesMatchFirst = IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedTrianglesEqual(Triangles, BaselineTriangles);
				Capture.bMaterialsMatchFirst = IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedIntsEqual(MaterialIndices, BaselineMaterials);
			}

			Capture.After = IMAcousticG2ScaleCaptureTestPrivate::IMG2ReadMemory(TEXT("after_log_cost_capture"));
			RunMemory.Add(Capture.After);
			const int32 ExpectedLogCount = Capture.bDetailedLogRequested ? IMAcousticG2ScaleCaptureTestPrivate::IMG2DetailedLogCostPlacements : 0;
			Capture.bLogCountsExact = Capture.Logs.Bounds == ExpectedLogCount
				&& Capture.Logs.Actor == ExpectedLogCount && Capture.Logs.Place == ExpectedLogCount;
			Capture.bLogBytesValid = Capture.bDetailedLogRequested
				? Capture.Logs.SerializedTCharBytes > 0 : Capture.Logs.SerializedTCharBytes == 0;
			if (!Capture.bCaptureSucceeded)
			{
				Capture.Error = Fixture.GetVolume()
					? Fixture.GetVolume()->Status : TEXT("Transient capture volume disappeared.");
				RecordFailure(Capture.Error);
			}
			else if (!Capture.bGeometryCountsExact || !Capture.bFingerprintMatchesFirst
				|| !Capture.bVerticesMatchFirst || !Capture.bTrianglesMatchFirst || !Capture.bMaterialsMatchFirst)
			{
				Capture.Error = TEXT("Captured geometry, material indices, or fingerprint changed across logging modes.");
				RecordFailure(Capture.Error);
			}
			else if (!Capture.bLogCountsExact || !Capture.bLogBytesValid)
			{
				Capture.Error = FString::Printf(TEXT("Unexpected detailed-log evidence for mode %s: bounds=%d actor=%d place=%d expected_each=%d payload_bytes=%llu."),
					*Capture.Mode, Capture.Logs.Bounds, Capture.Logs.Actor, Capture.Logs.Place,
					ExpectedLogCount, Capture.Logs.SerializedTCharBytes);
				RecordFailure(Capture.Error);
			}

			Captures.Add(MoveTemp(Capture));
			if (bHardFailure) break;
			if (IMAcousticG2ScaleCaptureTestPrivate::IMG2BelowResourceGuard(Captures.Last().After))
			{
				RecordResourceGuard(TEXT("Available system RAM fell below the 8 GiB floor after a paired capture."));
				break;
			}
		}
	}
	ObserverScope.Detach();

	FString CVarRestoreError;
	bCVarRestored = LogSetting.Restore(CVarRestoreError);
	if (!bCVarRestored) RecordFailure(CVarRestoreError);
	const int32 FinalCVarValue = LogSetting.GetCurrentValue();

	FString RestoreError;
	const bool bRestoreSucceeded = Fixture.RestoreAndCompare(FinalMapIdentity, FinalAssetIdentity, RestoreError);
	bMapRestored = bRestoreSucceeded && FinalMapIdentity == InitialSource.MapIdentity;
	bAssetsUnchanged = bRestoreSucceeded && FinalAssetIdentity == InitialSource.OriginalBakeAssetIdentity;
	if (!bRestoreSucceeded || !bMapRestored || !bAssetsUnchanged)
	{
		RecordFailure(RestoreError.IsEmpty()
			? TEXT("Final map or original bake asset identity differs from its baseline.")
			: RestoreError);
	}
	IMAcousticG2ScaleCaptureTestPrivate::IMG2AddMemorySample(RunMemory, TEXT("final_map_reloaded"));

	const bool bCapturePairSummaryValid = Captures.Num() == 4
		&& Captures[0].Mode == TEXT("logging_on_1") && Captures[1].Mode == TEXT("logging_off_1")
		&& Captures[2].Mode == TEXT("logging_off_2") && Captures[3].Mode == TEXT("logging_on_2")
		&& Captures[0].bCaptureSucceeded && Captures[1].bCaptureSucceeded
		&& Captures[2].bCaptureSucceeded && Captures[3].bCaptureSucceeded
		&& Captures[0].bGeometryCountsExact && Captures[1].bGeometryCountsExact
		&& Captures[2].bGeometryCountsExact && Captures[3].bGeometryCountsExact
		&& Captures[0].bFingerprintMatchesFirst && Captures[1].bFingerprintMatchesFirst
		&& Captures[2].bFingerprintMatchesFirst && Captures[3].bFingerprintMatchesFirst
		&& Captures[0].bVerticesMatchFirst && Captures[1].bVerticesMatchFirst
		&& Captures[2].bVerticesMatchFirst && Captures[3].bVerticesMatchFirst
		&& Captures[0].bTrianglesMatchFirst && Captures[1].bTrianglesMatchFirst
		&& Captures[2].bTrianglesMatchFirst && Captures[3].bTrianglesMatchFirst
		&& Captures[0].bMaterialsMatchFirst && Captures[1].bMaterialsMatchFirst
		&& Captures[2].bMaterialsMatchFirst && Captures[3].bMaterialsMatchFirst
		&& Captures[0].bLogCountsExact && Captures[1].bLogCountsExact
		&& Captures[2].bLogCountsExact && Captures[3].bLogCountsExact
		&& Captures[0].bLogBytesValid && Captures[1].bLogBytesValid
		&& Captures[2].bLogBytesValid && Captures[3].bLogBytesValid;
	bool bAllCapturesValid = bCapturePairSummaryValid;
	for (const IMAcousticG2ScaleCaptureTestPrivate::FIMG2DetailedLogCapture& Capture : Captures)
	{
		bAllCapturesValid = bAllCapturesValid && Capture.bCaptureSucceeded && Capture.bGeometryCountsExact
			&& Capture.bFingerprintMatchesFirst && Capture.bVerticesMatchFirst
			&& Capture.bTrianglesMatchFirst && Capture.bMaterialsMatchFirst;
	}
	if (!bHardFailure && !bResourceGuardHit && !bAllCapturesValid)
	{
		RecordFailure(TEXT("The four-capture ON/OFF/OFF/ON sequence did not complete with exact geometry parity."));
	}

	FString FinalResult = bHardFailure ? TEXT("FAIL")
		: bResourceGuardHit ? TEXT("INCONCLUSIVE")
		: bAllCapturesValid && bCVarRestored && bMapRestored && bAssetsUnchanged ? TEXT("PASS")
		: TEXT("FAIL");
	const bool bReportSaved = IMAcousticG2ScaleCaptureTestPrivate::IMG2WriteDetailedLogCostReport(Directory, ReportPath, RunId, InitialSource,
		FinalMapIdentity, FinalAssetIdentity, TransformFingerprint, Failure, bMapRestored, bAssetsUnchanged,
		bCVarRestored, LogSetting.GetOriginalValue(), FinalCVarValue, bResourceGuardHit,
		ResourceGuardReason, FinalResult, CubeScale, ExpectedVertices,
		ExpectedTriangles,
		BuiltPlacements, PrimitiveComponents, PlacementBuildSeconds, RunMemory, Captures, bCapturePairSummaryValid);
	if (!bReportSaved)
	{
		AddError(TEXT("Required detailed-log cost report could not be written; no PASS result is valid."));
		FinalResult = TEXT("FAIL");
		bHardFailure = true;
	}

	if (FinalResult == TEXT("INCONCLUSIVE"))
	{
		AddWarning(TEXT("Detailed-log A/B is INCONCLUSIVE: ") + ResourceGuardReason);
		AddError(TEXT("The paired 3000-placement logging sequence stopped at the resource guard; see report.json."));
	}
	else if (FinalResult != TEXT("PASS"))
	{
		AddError(Failure.IsEmpty()
			? TEXT("Detailed-log cost benchmark did not meet its capture, parity, logging, or restoration checks.")
			: Failure);
	}

	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticG2DetailedLogCost run=%s captures=%d map_restored=%d assets_unchanged=%d cvar_restored=%d report_saved=%d result=%s report=%s"),
		*RunId, Captures.Num(), bMapRestored, bAssetsUnchanged, bCVarRestored, bReportSaved,
		FinalResult == TEXT("PASS") ? TEXT("PASS") : FinalResult == TEXT("INCONCLUSIVE") ? TEXT("INCONCLUSIVE") : TEXT("ERROR"),
		*ReportPath);
	if (FinalResult == TEXT("PASS") && bReportSaved && bMapRestored && bAssetsUnchanged && bCVarRestored)
	{
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] G2DetailedLogCost3000 PASS"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS G2DetailedLogCost3000"));
		return true;
	}
	return false;
}
#endif
