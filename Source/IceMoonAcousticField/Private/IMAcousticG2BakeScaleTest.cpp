#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/PhysicsVolume.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "MeshDescription.h"
#include "StaticMeshResources.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "IMAcousticSimulation.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/ReferencerFinder.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

namespace IMAcousticG2BakeScaleTestPrivate
{
constexpr const TCHAR* IMG2BakeScaleMap = TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
constexpr const TCHAR* IMG2BakeScaleWhiteboxTag = TEXT("IMAcousticWhitebox");
constexpr int32 IMG2BakeScaleBaselinePlacements = 60;
constexpr int32 IMG2BakeScaleBaselineProbes = 632;
constexpr int32 IMG2BakeScaleBaselineTriangles = 2880;
constexpr int32 IMG2BakeScaleCounts[] = {500, 3000};
constexpr int32 IMG2BakeScaleUniqueMeshCount = 1000;
constexpr double IMG2BakeScaleTriangleDigestQuantizationCm = 0.0001;
constexpr uint64 IMG2BakeScaleMinAvailableBytes = 8ull * 1024ull * 1024ull * 1024ull;
constexpr double IMG2BakeScaleMaxSeconds = 900.0;
constexpr int32 IMG2BakeScaleEditorTicks = 3;

enum class EIMG2BakeScaleRepresentation : uint8
{
	StaticMeshActors,
	InstancedStaticMesh,
	HierarchicalInstancedStaticMesh
};

static const TCHAR* IMG2BakeScaleRepresentationName(const EIMG2BakeScaleRepresentation Representation)
{
	switch (Representation)
	{
	case EIMG2BakeScaleRepresentation::InstancedStaticMesh: return TEXT("ISM");
	case EIMG2BakeScaleRepresentation::HierarchicalInstancedStaticMesh: return TEXT("HISM");
	default: return TEXT("StaticMeshActor");
	}
}

static const TCHAR* IMG2BakeScaleAutomationName(const int32 TargetPlacements,
	const bool bUniqueMeshScenario, const EIMG2BakeScaleRepresentation Representation)
{
	if (bUniqueMeshScenario)
	{
		if (Representation == EIMG2BakeScaleRepresentation::InstancedStaticMesh)
			return TEXT("IceMoon.AcousticField.G2.BakeScaleISMUnique1000");
		if (Representation == EIMG2BakeScaleRepresentation::HierarchicalInstancedStaticMesh)
			return TEXT("IceMoon.AcousticField.G2.BakeScaleHISMUnique1000");
		return TEXT("IceMoon.AcousticField.G2.BakeScaleUnique1000");
	}
	return TargetPlacements == 500 ? TEXT("IceMoon.AcousticField.G2.BakeScale500")
		: TEXT("IceMoon.AcousticField.G2.BakeScale3000");
}

static const TCHAR* IMG2BakeScalePassMarker(const bool bUniqueMeshScenario,
	const EIMG2BakeScaleRepresentation Representation)
{
	if (!bUniqueMeshScenario) return TEXT("G2BakeScale");
	if (Representation == EIMG2BakeScaleRepresentation::InstancedStaticMesh)
		return TEXT("G2BakeScaleISMUnique1000");
	if (Representation == EIMG2BakeScaleRepresentation::HierarchicalInstancedStaticMesh)
		return TEXT("G2BakeScaleHISMUnique1000");
	return TEXT("G2BakeScaleUnique1000");
}

static FString IMG2BakeScaleHashStrings(TArray<FString> Items)
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

static FString IMG2BakeScaleMaterialIdentity(const UStaticMeshComponent* Component)
{
	FString Result;
	if (!Component) return Result;
	for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
	{
		Result += FString::Printf(TEXT("|material[%d]=%s"), Index,
			*GetPathNameSafe(Component->GetMaterial(Index)));
	}
	return Result;
}

static FString IMG2BakeScaleWorldIdentity(UWorld* World)
{
	if (!IsValid(World)) return FString();
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
				Item += IMG2BakeScaleMaterialIdentity(MeshComponent);
			}
			Items.Add(MoveTemp(Item));
		}
	}
	return World->GetOutermost()->GetName() + TEXT("|") + IMG2BakeScaleHashStrings(MoveTemp(Items));
}

static FString IMG2BakeScaleAssetIdentity(UWorld* World, int32& OutVolumeCount)
{
	OutVolumeCount = 0;
	if (!IsValid(World)) return FString();
	TArray<FString> Items;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
	{
		++OutVolumeCount;
		UIMAcousticBakeAsset* Asset = It->BakedField;
		FString Item = It->GetPathName() + TEXT("|") + GetPathNameSafe(Asset);
		if (Asset)
		{
			FSHA1 StoredSectionsHash;
			if (Asset->SceneData.Num() > 0) StoredSectionsHash.Update(Asset->SceneData.GetData(), Asset->SceneData.Num());
			if (Asset->ProbeData.Num() > 0) StoredSectionsHash.Update(Asset->ProbeData.GetData(), Asset->ProbeData.Num());
			StoredSectionsHash.Final();
			uint8 StoredSectionsDigest[20];
			StoredSectionsHash.GetHash(StoredSectionsDigest);
			Item += TEXT("|") + Asset->WorldPackage + TEXT("|") + Asset->SceneFingerprint
				+ TEXT("|stored_payload_digest=") + Asset->PayloadDigest + TEXT("|stored_sections_sha1=")
				+ BytesToHex(StoredSectionsDigest, 20) + TEXT("|")
				+ IMG2BakeScaleHashStrings({Asset->MetadataJson});
		}
		Items.Add(MoveTemp(Item));
	}
	return IMG2BakeScaleHashStrings(MoveTemp(Items));
}

static FString IMG2BakeScaleVolumeRecipeIdentity(const AIMAcousticBakeVolume* Volume)
{
	if (!IsValid(Volume) || !IsValid(Volume->BakeBounds)) return FString();
	TArray<FString> Items;
	Items.Add(FString::Printf(TEXT("bounds_transform=%s"), *Volume->BakeBounds->GetComponentTransform().ToString()));
	Items.Add(FString::Printf(TEXT("bounds_extent=%s"), *Volume->BakeBounds->GetUnscaledBoxExtent().ToString()));
	Items.Add(FString::Printf(TEXT("probe_spacing=%.9g|probe_height=%.9g"),
		Volume->ProbeSpacingCm, Volume->ProbeHeightCm));
	for (int32 Index = 0; Index < Volume->Materials.Num(); ++Index)
	{
		const FIMAcousticMaterialMapping& Mapping = Volume->Materials[Index];
		Items.Add(FString::Printf(TEXT("material[%d]=%s|absorption=%s|scattering=%.9g"), Index,
			*GetPathNameSafe(Mapping.Material), *Mapping.Absorption.ToString(), Mapping.Scattering));
	}
	return IMG2BakeScaleHashStrings(MoveTemp(Items));
}

struct FIMG2BakeScaleSource
{
	FString WorldPackage;
	FString WorldIdentity;
	FString AssetIdentity;
	FString VolumeRecipeIdentity;
	FString MeshPath;
	FString MaterialPath;
	FString OriginalAssetPath;
	FString OriginalAssetMetadata;
	FString OriginalAssetPackageFile;
	FString OriginalAssetDiskDigest;
	int64 OriginalAssetDiskBytes = 0;
	int32 VolumeCount = 0;
	int32 WhiteboxPlacements = 0;
	int32 CubeLOD0Triangles = 0;
	int32 ProbeCount = 0;
	FVector MeshBoundsOrigin = FVector::ZeroVector;
	FVector MeshBoundsExtent = FVector::ZeroVector;
	FVector BoundsExtent = FVector::ZeroVector;
	FVector BoundsOrigin = FVector::ZeroVector;
	float ProbeSpacingCm = 0.0f;
	float ProbeHeightCm = 0.0f;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<UStaticMesh> Mesh;
	TWeakObjectPtr<UMaterialInterface> Material;
	// The whitebox cubes get their acoustic surface from this component-level override (the W2 bake is PhysicalMaterial-driven and the Cube
	// render material carries no physical material), so transient placements must carry the same override or the scene check rejects them.
	TWeakObjectPtr<UPhysicalMaterial> PhysicalMaterialOverride;
};

static bool IMG2BakeScaleHashFile(const FString& PackageName, FString& OutFilename,
	FString& OutDigest, int64& OutBytes, FString& OutError)
{
	OutFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	TArray<uint8> Bytes;
	if (OutFilename.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *OutFilename) || Bytes.IsEmpty())
	{
		OutError = TEXT("Could not read the saved bake asset package bytes: ") + OutFilename;
		return false;
	}
	FSHA1 Hash;
	Hash.Update(Bytes.GetData(), Bytes.Num());
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	OutDigest = BytesToHex(Digest, 20);
	OutBytes = Bytes.Num();
	return true;
}

static bool IMG2BakeScaleReadSource(UWorld* World, FIMG2BakeScaleSource& Out, FString& Error,
	const bool bValidateExistingBake)
{
	if (!IsValid(World))
	{
		Error = TEXT("Editor world is unavailable after loading the W2 mother map.");
		return false;
	}
	Out = FIMG2BakeScaleSource();
	Out.WorldPackage = World->GetOutermost()->GetName();
	Out.WorldIdentity = IMG2BakeScaleWorldIdentity(World);

	UStaticMesh* CubeMesh = nullptr;
	UMaterialInterface* CubeMaterial = nullptr;
	bool bUniformMesh = true;
	bool bUniformMaterial = true;
	UPhysicalMaterial* CubePhysicalOverride = nullptr;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
	{
		if (Out.Volume.IsValid())
		{
			Error = TEXT("W2 mother map contains multiple acoustic bake volumes.");
			return false;
		}
		Out.Volume = *It;
	}
	if (!Out.Volume.IsValid() || !Out.Volume->BakedField || !Out.Volume->BakeBounds)
	{
		Error = TEXT("W2 mother map does not contain its expected bake volume and bound asset.");
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->ActorHasTag(IMG2BakeScaleWhiteboxTag)) continue;
		TArray<UStaticMeshComponent*> MeshComponents;
		It->GetComponents(MeshComponents);
		for (UStaticMeshComponent* Component : MeshComponents)
		{
			if (!IsValid(Component) || !Component->GetStaticMesh()) continue;
			++Out.WhiteboxPlacements;
			if (Component->Mobility != EComponentMobility::Static)
			{
				Error = FString::Printf(TEXT("W2 whitebox component is not Static: %s"), *Component->GetPathName());
				return false;
			}
			if (!CubeMesh) CubeMesh = Component->GetStaticMesh();
			else bUniformMesh &= CubeMesh == Component->GetStaticMesh();
			if (!CubeMaterial) CubeMaterial = Component->GetMaterial(0);
			else bUniformMaterial &= CubeMaterial == Component->GetMaterial(0);
			// Transient placements copy the first whitebox cube's override; the saved cubes keep their own, so the baseline mapping set is preserved.
			if (Out.WhiteboxPlacements == 1) CubePhysicalOverride = Component->BodyInstance.GetPhysMaterialOverride();
		}
	}
	if (Out.WorldPackage != IMG2BakeScaleMap || Out.WhiteboxPlacements != IMG2BakeScaleBaselinePlacements
		|| !CubeMesh || !CubeMaterial || !bUniformMesh || !bUniformMaterial
		|| CubeMesh->GetStaticMaterials().Num() != 1 || !CubeMesh->GetRenderData()
		|| CubeMesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = FString::Printf(TEXT("W2 whitebox baseline drift: map=%s placements=%d mesh=%s material=%s"),
			*Out.WorldPackage, Out.WhiteboxPlacements, *GetPathNameSafe(CubeMesh), *GetPathNameSafe(CubeMaterial));
		return false;
	}

	int64 TriangleCount = 0;
	for (const FStaticMeshSection& Section : CubeMesh->GetRenderData()->LODResources[0].Sections)
		TriangleCount += Section.NumTriangles;
	if (TriangleCount <= 0 || TriangleCount > MAX_int32)
	{
		Error = TEXT("W2 Cube has an invalid LOD0 triangle count.");
		return false;
	}
	Out.CubeLOD0Triangles = static_cast<int32>(TriangleCount);
	Out.MeshBoundsOrigin = CubeMesh->GetBounds().Origin;
	Out.MeshBoundsExtent = CubeMesh->GetBounds().BoxExtent;
	Out.MeshPath = CubeMesh->GetPathName();
	Out.MaterialPath = CubeMaterial->GetPathName();
	Out.Mesh = CubeMesh;
	Out.Material = CubeMaterial;
	Out.PhysicalMaterialOverride = CubePhysicalOverride;

	AIMAcousticBakeVolume* Volume = Out.Volume.Get();
	Out.ProbeSpacingCm = Volume->ProbeSpacingCm;
	Out.ProbeHeightCm = Volume->ProbeHeightCm;
	Out.BoundsExtent = Volume->BakeBounds->GetUnscaledBoxExtent();
	Out.BoundsOrigin = Volume->BakeBounds->GetComponentLocation();
	if (!FMath::IsNearlyEqual(Out.ProbeSpacingCm, 100.0f)
		|| !FMath::IsNearlyEqual(Out.ProbeHeightCm, 150.0f)
		|| Volume->Materials.IsEmpty())
	{
		Error = TEXT("W2 bake recipe no longer matches the 1m spacing / 1.5m height baseline.");
		return false;
	}
	bool bMappedCubeMaterial = false;
	for (const FIMAcousticMaterialMapping& Mapping : Volume->Materials)
		bMappedCubeMaterial |= Mapping.Material == CubeMaterial;
	if (!bMappedCubeMaterial)
	{
		Error = TEXT("W2 bake volume has no explicit material mapping for the whitebox Cube material.");
		return false;
	}

	UIMAcousticBakeAsset* Asset = Volume->BakedField.Get();
	Out.OriginalAssetPath = Asset->GetPathName();
	Out.OriginalAssetMetadata = Asset->MetadataJson;
	Out.VolumeRecipeIdentity = IMG2BakeScaleVolumeRecipeIdentity(Volume);
	Out.AssetIdentity = IMG2BakeScaleAssetIdentity(World, Out.VolumeCount);
	if (Out.VolumeCount != 1)
	{
		Error = TEXT("W2 mother map must contain exactly one bake volume.");
		return false;
	}

	if (bValidateExistingBake)
	{
		FString ValidationFailure;
		if (!Volume->ValidateCurrentBake(ValidationFailure))
		{
			Error = TEXT("Existing W2 bake does not validate before scale test: ") + ValidationFailure;
			return false;
		}
		if (Volume->ExportedTriangles != IMG2BakeScaleBaselineTriangles
			|| Volume->ExportedTriangles != Out.WhiteboxPlacements * Out.CubeLOD0Triangles)
		{
			Error = FString::Printf(TEXT("W2 bake geometry baseline drift: triangles=%d expected=%d"),
				Volume->ExportedTriangles, Out.WhiteboxPlacements * Out.CubeLOD0Triangles);
			return false;
		}
		TArray<FVector4> Probes;
		FVector ProbeOrigin;
		if (!Asset->GetProbePreview(Asset->WorldPackage, Asset->SceneFingerprint,
				Probes, ProbeOrigin, ValidationFailure)
			|| Probes.Num() != IMG2BakeScaleBaselineProbes)
		{
			Error = FString::Printf(TEXT("W2 saved bake probe baseline drift: probes=%d detail=%s"),
				Probes.Num(), *ValidationFailure);
			return false;
		}
		Out.ProbeCount = Probes.Num();
	}

	if (!IMG2BakeScaleHashFile(Asset->GetOutermost()->GetName(), Out.OriginalAssetPackageFile,
			Out.OriginalAssetDiskDigest, Out.OriginalAssetDiskBytes, Error))
		return false;
	return true;
}

struct FIMG2BakeScaleMemorySample
{
	FString Stage;
	double ElapsedSeconds = 0.0;
	uint64 UsedPhysical = 0;
	uint64 PeakUsedPhysical = 0;
	uint64 UsedVirtual = 0;
	uint64 PeakUsedVirtual = 0;
	uint64 AvailablePhysical = 0;
};

enum class EIMG2BakeScaleTickerPhase : uint8
{
	Baseline,
	InstanceMoved,
	InstanceRestored,
	Complete
};

static const TCHAR* IMG2BakeScaleTickerPhaseName(const EIMG2BakeScaleTickerPhase Phase)
{
	switch (Phase)
	{
	case EIMG2BakeScaleTickerPhase::Baseline: return TEXT("baseline");
	case EIMG2BakeScaleTickerPhase::InstanceMoved: return TEXT("instance_x_plus_1cm");
	case EIMG2BakeScaleTickerPhase::InstanceRestored: return TEXT("instance_transform_restored");
	case EIMG2BakeScaleTickerPhase::Complete: return TEXT("complete");
	default: return TEXT("unknown");
	}
}

struct FIMG2BakeScaleTickerObservation
{
	int32 Sequence = 0;
	FString Phase;
	FString Status;
	double CaptureMs = 0.0;
	double AssetValidationMs = 0.0;
	double FullTickMs = 0.0;
	double ElapsedSeconds = 0.0;
	double PhaseElapsedSeconds = 0.0;
	int32 ExpectedCaptured = INDEX_NONE;
	int32 ExpectedBoundAssetValid = INDEX_NONE;
	int32 ExpectedTriangles = INDEX_NONE;
	int32 ObservedCaptured = INDEX_NONE;
	int32 ObservedBoundAssetValid = INDEX_NONE;
	int32 ObservedTriangles = INDEX_NONE;
};

static FBox IMG2BakeScaleTransformBounds(const UStaticMesh* Mesh, const FTransform& Transform)
{
	if (!IsValid(Mesh)) return FBox(ForceInit);
	const FBox LocalBounds = Mesh->GetBoundingBox();
	const FVector Corners[] = {
		FVector(LocalBounds.Min.X, LocalBounds.Min.Y, LocalBounds.Min.Z),
		FVector(LocalBounds.Min.X, LocalBounds.Min.Y, LocalBounds.Max.Z),
		FVector(LocalBounds.Min.X, LocalBounds.Max.Y, LocalBounds.Min.Z),
		FVector(LocalBounds.Min.X, LocalBounds.Max.Y, LocalBounds.Max.Z),
		FVector(LocalBounds.Max.X, LocalBounds.Min.Y, LocalBounds.Min.Z),
		FVector(LocalBounds.Max.X, LocalBounds.Min.Y, LocalBounds.Max.Z),
		FVector(LocalBounds.Max.X, LocalBounds.Max.Y, LocalBounds.Min.Z),
		FVector(LocalBounds.Max.X, LocalBounds.Max.Y, LocalBounds.Max.Z)
	};
	FBox WorldBounds(ForceInit);
	for (const FVector& Corner : Corners) WorldBounds += Transform.TransformPosition(Corner);
	return WorldBounds;
}

static double IMG2BakeScaleBoundsInteriorMargin(const FBox& WorldBounds,
	const FVector& RegionMin, const FVector& RegionMax)
{
	if (!WorldBounds.IsValid) return -TNumericLimits<double>::Max();
	const FVector Lower = WorldBounds.Min - RegionMin;
	const FVector Upper = RegionMax - WorldBounds.Max;
	return FMath::Min(FMath::Min(FMath::Min(Lower.X, Lower.Y), Lower.Z),
		FMath::Min(FMath::Min(Upper.X, Upper.Y), Upper.Z));
}

static FIMG2BakeScaleMemorySample IMG2BakeScaleReadMemory(const TCHAR* Stage, const double StartedAt)
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	FIMG2BakeScaleMemorySample Sample;
	Sample.Stage = Stage;
	Sample.ElapsedSeconds = FPlatformTime::Seconds() - StartedAt;
	Sample.UsedPhysical = Stats.UsedPhysical;
	Sample.PeakUsedPhysical = Stats.PeakUsedPhysical;
	Sample.UsedVirtual = Stats.UsedVirtual;
	Sample.PeakUsedVirtual = Stats.PeakUsedVirtual;
	Sample.AvailablePhysical = Stats.AvailablePhysical;
	return Sample;
}

static bool IMG2BakeScaleQuantizedPointToken(const FVector& Position, FString& OutToken)
{
	if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z)) return false;
	const double QX = Position.X / IMG2BakeScaleTriangleDigestQuantizationCm;
	const double QY = Position.Y / IMG2BakeScaleTriangleDigestQuantizationCm;
	const double QZ = Position.Z / IMG2BakeScaleTriangleDigestQuantizationCm;
	if (FMath::Abs(QX) > static_cast<double>(MAX_int64)
		|| FMath::Abs(QY) > static_cast<double>(MAX_int64)
		|| FMath::Abs(QZ) > static_cast<double>(MAX_int64)) return false;
	OutToken = FString::Printf(TEXT("%lld,%lld,%lld"), FMath::RoundToInt64(QX),
		FMath::RoundToInt64(QY), FMath::RoundToInt64(QZ));
	return true;
}

static bool IMG2BakeScaleOrientedTriangleMaterialKey(const int32 MaterialIndex,
	const FString& MaterialPath, const FVector& A, const FVector& B, const FVector& C,
	FString& OutKey)
{
	FString PointA, PointB, PointC;
	if (!IMG2BakeScaleQuantizedPointToken(A, PointA)
		|| !IMG2BakeScaleQuantizedPointToken(B, PointB)
		|| !IMG2BakeScaleQuantizedPointToken(C, PointC)) return false;
	FString Canonical = PointA + TEXT("|") + PointB + TEXT("|") + PointC;
	const FString RotateABC = PointB + TEXT("|") + PointC + TEXT("|") + PointA;
	const FString RotateCAB = PointC + TEXT("|") + PointA + TEXT("|") + PointB;
	if (RotateABC < Canonical) Canonical = RotateABC;
	if (RotateCAB < Canonical) Canonical = RotateCAB;
	OutKey = FString::Printf(TEXT("%d|%s|%s"), MaterialIndex, *MaterialPath, *Canonical);
	return true;
}

static bool IMG2BakeScaleOrientedTriangleMaterialDigest(UStaticMesh* Mesh, FString& OutDigest,
	int32& OutTriangleCount, FString& Error)
{
	OutDigest.Reset();
	OutTriangleCount = 0;
	if (!IsValid(Mesh) || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = FString::Printf(TEXT("Mesh %s has no render LOD0 for geometry/material comparison."),
			*GetPathNameSafe(Mesh));
		return false;
	}
	const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD0.VertexBuffers.PositionVertexBuffer;
	const auto IndexView = LOD0.IndexBuffer.GetArrayView();
	if (Positions.GetNumVertices() <= 0 || !Positions.GetVertexData()
		|| IndexView.Num() <= 0 || (IndexView.Num() % 3) != 0)
	{
		Error = FString::Printf(TEXT("Mesh %s does not retain readable CPU LOD0 vertices and indices."),
			*GetPathNameSafe(Mesh));
		return false;
	}

	TArray<FString> TriangleKeys;
	TriangleKeys.Reserve(IndexView.Num() / 3);
	const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
	for (const FStaticMeshSection& Section : LOD0.Sections)
	{
		if (Section.MaterialIndex < 0 || !StaticMaterials.IsValidIndex(Section.MaterialIndex))
		{
			Error = FString::Printf(TEXT("Mesh %s LOD0 section references invalid material slot %d."),
				*GetPathNameSafe(Mesh), Section.MaterialIndex);
			return false;
		}
		UMaterialInterface* Material = Mesh->GetMaterial(Section.MaterialIndex);
		const uint64 SectionEnd = static_cast<uint64>(Section.FirstIndex)
			+ static_cast<uint64>(Section.NumTriangles) * 3ull;
		if (!Material || SectionEnd > static_cast<uint64>(IndexView.Num()))
		{
			Error = FString::Printf(TEXT("Mesh %s LOD0 section has invalid material or index range."),
				*GetPathNameSafe(Mesh));
			return false;
		}
		for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
		{
			const uint32 FirstIndex = Section.FirstIndex + TriangleIndex * 3;
			const uint32 IA = LOD0.IndexBuffer.GetIndex(FirstIndex + 0);
			const uint32 IB = LOD0.IndexBuffer.GetIndex(FirstIndex + 1);
			const uint32 IC = LOD0.IndexBuffer.GetIndex(FirstIndex + 2);
			if (IA >= Positions.GetNumVertices() || IB >= Positions.GetNumVertices()
				|| IC >= Positions.GetNumVertices())
			{
				Error = FString::Printf(TEXT("Mesh %s LOD0 triangle references a vertex outside CPU positions."),
					*GetPathNameSafe(Mesh));
				return false;
			}
			FString Key;
			if (!IMG2BakeScaleOrientedTriangleMaterialKey(Section.MaterialIndex, Material->GetPathName(),
					FVector(Positions.VertexPosition(IA)), FVector(Positions.VertexPosition(IB)),
					FVector(Positions.VertexPosition(IC)), Key))
			{
				Error = FString::Printf(TEXT("Mesh %s LOD0 triangle contains a non-finite position."),
					*GetPathNameSafe(Mesh));
				return false;
			}
			TriangleKeys.Add(MoveTemp(Key));
			++OutTriangleCount;
		}
	}
	if (OutTriangleCount <= 0 || OutTriangleCount * 3 != IndexView.Num())
	{
		Error = FString::Printf(TEXT("Mesh %s LOD0 section and index triangle totals disagree."),
			*GetPathNameSafe(Mesh));
		return false;
	}
	OutDigest = IMG2BakeScaleHashStrings(MoveTemp(TriangleKeys));
	return true;
}

static bool IMG2BakeScaleBuildUniqueMeshes(const FIMG2BakeScaleSource& Source, const int32 Count,
	const FString& RunId, const double StartedAt, TArray<FIMG2BakeScaleMemorySample>& OutMemorySamples,
	TArray<TStrongObjectPtr<UStaticMesh>>& OutMeshes, FString& OutSemanticDigest,
	int32& OutTriangleCount, double& OutBuildSeconds, bool& bOutResourceGuard, FString& Error)
{
	UStaticMesh* SourceMesh = Source.Mesh.Get();
	if (!IsValid(SourceMesh) || Count <= 0 || !SourceMesh->GetRenderData()
		|| SourceMesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = TEXT("Cannot build transient mesh identities without source CPU LOD0.");
		return false;
	}

	FMeshDescription SourceDescription;
	SourceMesh->ExportStaticMeshLOD(SourceMesh->GetRenderData()->LODResources[0], SourceDescription);
	if (SourceDescription.Vertices().Num() <= 0
		|| SourceDescription.Triangles().Num() != Source.CubeLOD0Triangles)
	{
		Error = TEXT("W2 Cube LOD0 CPU export did not produce a reusable MeshDescription.");
		return false;
	}
	const TArray<FStaticMaterial> SourceMaterials = SourceMesh->GetStaticMaterials();
	if (SourceMaterials.Num() != 1 || !SourceMaterials[0].MaterialInterface)
	{
		Error = TEXT("The unique-mesh bake fixture requires one valid source Cube material slot.");
		return false;
	}
	FString SourceSemanticDigest;
	int32 SourceTriangles = 0;
	if (!IMG2BakeScaleOrientedTriangleMaterialDigest(SourceMesh, SourceSemanticDigest, SourceTriangles, Error)
		|| SourceTriangles != Source.CubeLOD0Triangles)
		return false;

	TArray<const FMeshDescription*> Descriptions;
	Descriptions.Add(&SourceDescription);
	OutMeshes.Reset();
	OutMeshes.Reserve(Count);
	TSet<const UStaticMesh*> UniquePointers;
	TSet<FString> UniquePaths;
	TSet<uint32> UniqueIds;
	const double BuildStartedAt = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if ((Index % 16) == 0)
		{
			const FIMG2BakeScaleMemorySample BeforeBuild = IMG2BakeScaleReadMemory(TEXT("unique_mesh_build_progress"), StartedAt);
			OutMemorySamples.Add(BeforeBuild);
			if (BeforeBuild.AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
			{
				bOutResourceGuard = true;
				Error = FString::Printf(TEXT("Available system RAM fell below 8 GiB while building transient mesh %d of %d."),
					Index + 1, Count);
				OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
				return false;
			}
			if (BeforeBuild.ElapsedSeconds >= IMG2BakeScaleMaxSeconds)
			{
				bOutResourceGuard = true;
				Error = TEXT("The 900 second case guard expired while building transient mesh identities.");
				OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
				return false;
			}
		}

		const FName BaseName(*FString::Printf(TEXT("IM_G2UniqueBake_%s_%04d"), *RunId, Index));
		const FName MeshName = MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), BaseName);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), MeshName, RF_Transient);
		if (!IsValid(Mesh))
		{
			Error = FString::Printf(TEXT("Could not allocate transient UStaticMesh %d of %d."), Index + 1, Count);
			OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
			return false;
		}
		TStrongObjectPtr<UStaticMesh> StrongMesh(Mesh);
		Mesh->bAllowCPUAccess = true;
		Mesh->SetStaticMaterials(SourceMaterials);
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bCommitMeshDescription = false;
		Params.bFastBuild = true;
		Params.bAllowCpuAccess = true;
		if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params))
		{
			Error = FString::Printf(TEXT("BuildFromMeshDescriptions failed for transient mesh %d of %d."),
				Index + 1, Count);
			OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
			return false;
		}

		FString MeshSemanticDigest;
		int32 MeshTriangles = 0;
		if (!Mesh->bAllowCPUAccess
			|| Mesh->GetStaticMaterials().Num() != SourceMaterials.Num()
			|| !(Mesh->GetStaticMaterials()[0] == SourceMaterials[0])
			|| !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.Num() != 1
			|| !IMG2BakeScaleOrientedTriangleMaterialDigest(Mesh, MeshSemanticDigest, MeshTriangles, Error))
		{
			if (Error.IsEmpty()) Error = FString::Printf(TEXT("Transient mesh %d lost CPU LOD0 or source material identity."), Index + 1);
			OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
			return false;
		}
		if (MeshSemanticDigest != SourceSemanticDigest || MeshTriangles != SourceTriangles)
		{
			Error = FString::Printf(TEXT("Transient mesh %d LOD0 oriented triangle/material multiset differs from source Cube."),
				Index + 1);
			OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
			return false;
		}
		if (UniquePointers.Contains(Mesh) || UniquePaths.Contains(Mesh->GetPathName())
			|| UniqueIds.Contains(Mesh->GetUniqueID()))
		{
			Error = FString::Printf(TEXT("Transient mesh %d did not receive a distinct UObject identity."), Index + 1);
			OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
			return false;
		}
		UniquePointers.Add(Mesh);
		UniquePaths.Add(Mesh->GetPathName());
		UniqueIds.Add(Mesh->GetUniqueID());
		OutMeshes.Add(MoveTemp(StrongMesh));
	}
	OutBuildSeconds = FPlatformTime::Seconds() - BuildStartedAt;
	OutMemorySamples.Add(IMG2BakeScaleReadMemory(TEXT("unique_mesh_build_complete"), StartedAt));
	if (UniquePointers.Num() != Count || UniquePaths.Num() != Count || UniqueIds.Num() != Count
		|| OutMeshes.Num() != Count)
	{
		Error = TEXT("Transient mesh identity count did not exactly match the requested unique count.");
		return false;
	}
	OutSemanticDigest = SourceSemanticDigest;
	OutTriangleCount = SourceTriangles;
	return true;
}

struct FIMG2BakeScalePackageFileRecord
{
	FString Filename;
	FString Sha1;
	int64 Bytes = 0;
	bool bRemoved = false;
};

static FString IMG2BakeScaleCanonicalFilePath(const FString& Filename)
{
	FString Result = FPaths::ConvertRelativePathToFull(Filename);
	FPaths::NormalizeFilename(Result);
	return Result;
}

static bool IMG2BakeScaleHashFilePath(const FString& Filename, FString& OutSha1, int64& OutBytes,
	FString& Error)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.IsEmpty())
	{
		Error = TEXT("Could not read package file before cleanup: ") + Filename;
		return false;
	}
	FSHA1 Hash;
	Hash.Update(Bytes.GetData(), Bytes.Num());
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	OutSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	OutBytes = Bytes.Num();
	return true;
}

static FString IMG2BakeScaleHashByteArray(const TArray<uint8>& Bytes)
{
	if (Bytes.IsEmpty()) return FString();
	FSHA1 Hash;
	Hash.Update(Bytes.GetData(), Bytes.Num());
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

static bool IMG2BakeScaleSnapshotBakeDirectory(const FIMG2BakeScaleSource& Source,
	FString& OutDirectory, TSet<FString>& OutFiles, FString& Error)
{
	const FString ExpectedDirectory = IMG2BakeScaleCanonicalFilePath(FPaths::Combine(
		FPaths::ProjectPluginsDir(), TEXT("IceMoonAcousticField/Content/Bakes")));
	const FString SourceDirectory = IMG2BakeScaleCanonicalFilePath(
		FPaths::GetPath(Source.OriginalAssetPackageFile));
	if (!SourceDirectory.Equals(ExpectedDirectory, ESearchCase::IgnoreCase)
		|| !SourceDirectory.StartsWith(IMG2BakeScaleCanonicalFilePath(FPaths::ProjectDir()), ESearchCase::IgnoreCase)
		|| !FPaths::DirectoryExists(*SourceDirectory))
	{
		Error = TEXT("The original bake is not in the isolated project's exact IceMoonAcousticField/Content/Bakes directory.");
		return false;
	}
	OutDirectory = SourceDirectory;
	OutFiles.Reset();
	TArray<FString> Filenames;
	const FString ListingPattern = FPaths::Combine(OutDirectory, TEXT("*"));
	IFileManager::Get().FindFiles(Filenames, *ListingPattern, true, false);
	for (const FString& Filename : Filenames)
	{
		OutFiles.Add(IMG2BakeScaleCanonicalFilePath(FPaths::Combine(OutDirectory, Filename)).ToLower());
	}
	return true;
}

static bool IMG2BakeScaleValidateRunBakePackage(const FString& PackageName, const FString& AssetPath,
	const FString& PackageFile, const FString& BakeDirectory, const TSet<FString>& PreRunFiles,
	TArray<FIMG2BakeScalePackageFileRecord>& OutFiles, FString& Error)
{
	const FString ExpectedPrefix = FString::Printf(TEXT("/IceMoonAcousticField/Bakes/IM_%s_"),
		*FPackageName::GetShortName(IMG2BakeScaleMap));
	const FString ShortPackageName = FPackageName::GetShortName(PackageName);
	const FString PackagePrefix = TEXT("/IceMoonAcousticField/Bakes/") + ShortPackageName;
	if (!PackageName.StartsWith(ExpectedPrefix, ESearchCase::CaseSensitive)
		|| PackageName != PackagePrefix
		|| AssetPath != PackageName + TEXT(".") + ShortPackageName)
	{
		Error = TEXT("Generated asset path does not match the product's map/GUID bake package form.");
		return false;
	}
	const FString GuidDigits = ShortPackageName.RightChop(ExpectedPrefix.Len()
		- FString(TEXT("/IceMoonAcousticField/Bakes/")).Len());
	if (GuidDigits.Len() != 32)
	{
		Error = TEXT("Generated bake package name does not end in a 32-digit run GUID.");
		return false;
	}
	for (const TCHAR Digit : GuidDigits)
	{
		if (!FChar::IsHexDigit(Digit))
		{
			Error = TEXT("Generated bake package GUID contains a non-hexadecimal character.");
			return false;
		}
	}

	const FString ExpectedPrimaryFile = IMG2BakeScaleCanonicalFilePath(FPaths::Combine(
		BakeDirectory, ShortPackageName + FPackageName::GetAssetPackageExtension()));
	const FString ActualPrimaryFile = IMG2BakeScaleCanonicalFilePath(PackageFile);
	if (!ActualPrimaryFile.Equals(ExpectedPrimaryFile, ESearchCase::IgnoreCase)
		|| PreRunFiles.Contains(ActualPrimaryFile.ToLower()))
	{
		Error = TEXT("Generated package is outside the exact isolated Bakes directory or existed before this run.");
		return false;
	}

	TArray<FString> FoundNames;
	const FString PackageListingPattern = FPaths::Combine(BakeDirectory, ShortPackageName + TEXT(".*"));
	IFileManager::Get().FindFiles(FoundNames, *PackageListingPattern, true, false);
	OutFiles.Reset();
	bool bFoundPrimary = false;
	const TArray<FString> SupportedSuffixes = {
		FPackageName::GetAssetPackageExtension(), TEXT(".uexp"), TEXT(".ubulk"),
		TEXT(".uptnl"), TEXT(".m.ubulk"), TEXT(".o.ubulk")
	};
	for (const FString& FoundName : FoundNames)
	{
		const FString FullPath = IMG2BakeScaleCanonicalFilePath(FPaths::Combine(BakeDirectory, FoundName));
		if (PreRunFiles.Contains(FullPath.ToLower()))
		{
			Error = TEXT("A package file with this GUID existed before the run; cleanup is refused: ") + FullPath;
			return false;
		}
		const FString Suffix = FoundName.RightChop(ShortPackageName.Len());
		if (!SupportedSuffixes.ContainsByPredicate([&Suffix](const FString& Candidate)
				{ return Suffix.Equals(Candidate, ESearchCase::IgnoreCase); }))
		{
			Error = TEXT("Generated package has an unrecognized sidecar; exact cleanup is refused: ") + FullPath;
			return false;
		}
		FIMG2BakeScalePackageFileRecord Record;
		Record.Filename = FullPath;
		if (!IMG2BakeScaleHashFilePath(FullPath, Record.Sha1, Record.Bytes, Error)) return false;
		bFoundPrimary |= FullPath.Equals(ExpectedPrimaryFile, ESearchCase::IgnoreCase);
		OutFiles.Add(MoveTemp(Record));
	}
	if (!bFoundPrimary || OutFiles.IsEmpty())
	{
		Error = TEXT("The generated GUID package did not have a readable primary .uasset file.");
		return false;
	}
	OutFiles.Sort([&ExpectedPrimaryFile](const FIMG2BakeScalePackageFileRecord& A,
		const FIMG2BakeScalePackageFileRecord& B)
	{
		const bool bAIsPrimary = A.Filename.Equals(ExpectedPrimaryFile, ESearchCase::IgnoreCase);
		const bool bBIsPrimary = B.Filename.Equals(ExpectedPrimaryFile, ESearchCase::IgnoreCase);
		if (bAIsPrimary != bBIsPrimary) return !bAIsPrimary;
		return A.Filename < B.Filename;
	});
	return true;
}

class FIMG2BakeScaleLogCapture final : public FOutputDevice
{
public:
	void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
	{
		if (!Text || (!FCString::Strifind(Text, TEXT("IMLogs AcousticSDKStage"))
			&& !FCString::Strifind(Text, TEXT("IMLogs AcousticBakeAssetSave"))
			&& !FCString::Strifind(Text, TEXT("IMLogs AcousticG2CaptureCore"))
			&& !FCString::Strifind(Text, TEXT("IMLogs AcousticCapturePhases"))
			&& !FCString::Strifind(Text, TEXT("IMLogs AcousticG2EditorTicker"))))
			return;
		FScopeLock Lock(&Mutex);
		Lines.Add(Text);
	}

	bool CanBeUsedOnAnyThread() const override { return true; }

	TArray<FString> Snapshot() const
	{
		FScopeLock Lock(&Mutex);
		return Lines;
	}

private:
	mutable FCriticalSection Mutex;
	TArray<FString> Lines;
};

struct FIMG2BakeScalePlacementPlan
{
	int32 Columns = 0;
	int32 Rows = 0;
	float UniformScale = 0.0f;
	double CellWidthCm = 0.0;
	double CellHeightCm = 0.0;
	FVector CubeSizeCm = FVector::ZeroVector;
	TArray<FTransform> Transforms;
};

static bool IMG2BakeScaleBuildPlan(const FIMG2BakeScaleSource& Source, const int32 ExtraCount,
	FIMG2BakeScalePlacementPlan& Out, FString& Error)
{
	AIMAcousticBakeVolume* Volume = Source.Volume.Get();
	UStaticMesh* Mesh = Source.Mesh.Get();
	if (!IsValid(Volume) || !IsValid(Volume->BakeBounds) || !IsValid(Mesh) || ExtraCount <= 0)
	{
		Error = TEXT("Cannot build a placement plan without the W2 Cube and bake bounds.");
		return false;
	}
	const FTransform BoundsTransform = Volume->BakeBounds->GetComponentTransform();
	if (!BoundsTransform.GetRotation().Equals(FQuat::Identity)
		|| !BoundsTransform.GetScale3D().Equals(FVector::OneVector))
	{
		Error = TEXT("W2 BakeBounds is no longer axis aligned with unit scale.");
		return false;
	}
	const FVector Extent = Volume->BakeBounds->GetUnscaledBoxExtent();
	const double Width = Extent.X * 2.0;
	const double Height = Extent.Y * 2.0;
	const double Depth = Extent.Z * 2.0;
	if (Width <= 0.0 || Height <= 0.0 || Depth <= 0.0)
	{
		Error = TEXT("Original W2 BakeBounds has no usable 2D placement area or depth.");
		return false;
	}

	// A single grid is planned for the 3000-placement case. The 500 case uses
	// its prefix, so both cases share the same Cube dimensions and transform rule.
	constexpr int32 MaxExtraPlacements = IMG2BakeScaleCounts[1] - IMG2BakeScaleBaselinePlacements;
	Out.Columns = FMath::CeilToInt(FMath::Sqrt(static_cast<double>(MaxExtraPlacements) * Width / Height));
	Out.Rows = FMath::DivideAndRoundUp(MaxExtraPlacements, Out.Columns);
	Out.CellWidthCm = Width / Out.Columns;
	Out.CellHeightCm = Height / Out.Rows;
	const FVector MeshExtent = Source.MeshBoundsExtent;
	const double MaxMeshExtent = FMath::Max3(MeshExtent.X, MeshExtent.Y, MeshExtent.Z);
	const double SmallestCell = FMath::Min3(Out.CellWidthCm, Out.CellHeightCm, Depth);
	if (!FMath::IsFinite(MaxMeshExtent) || MaxMeshExtent <= 0.0 || SmallestCell <= 0.0)
	{
		Error = TEXT("W2 Cube local bounds cannot fit the deterministic 2D grid.");
		return false;
	}
	Out.UniformScale = static_cast<float>(FMath::Min(1.0, 0.9 * SmallestCell / (2.0 * MaxMeshExtent)));
	if (!FMath::IsFinite(Out.UniformScale) || Out.UniformScale <= 0.0f)
	{
		Error = TEXT("W2 Cube scale for the fixed grid is invalid.");
		return false;
	}
	Out.CubeSizeCm = MeshExtent * (2.0 * Out.UniformScale);
	Out.Transforms.Reset(ExtraCount);
	Out.Transforms.Reserve(ExtraCount);
	for (int32 Index = 0; Index < ExtraCount; ++Index)
	{
		const int32 Column = Index % Out.Columns;
		const int32 Row = Index / Out.Columns;
		const FVector BoundsCenterLocal(
			-Extent.X + (Column + 0.5) * Out.CellWidthCm,
			-Extent.Y + (Row + 0.5) * Out.CellHeightCm,
			0.0);
		const FVector PivotLocal = BoundsCenterLocal - Source.MeshBoundsOrigin * Out.UniformScale;
		const FVector WorldLocation = BoundsTransform.TransformPosition(PivotLocal);
		Out.Transforms.Emplace(BoundsTransform.GetRotation(), WorldLocation,
			FVector(Out.UniformScale));
	}
	return true;
}

static int32 IMG2BakeScaleCountEnumeratedPrimitiveComponents(UWorld* World,
	const AIMAcousticBakeVolume* Volume)
{
	if (!IsValid(World) || !IsValid(Volume)) return INDEX_NONE;
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		// CaptureScene excludes these engine-owned helpers before it counts
		// primitive components; the fixture baseline must use the same scope.
		if (*It == Volume || *It == World->GetDefaultBrush()
			|| *It == World->GetDefaultPhysicsVolume()) continue;
		TArray<UPrimitiveComponent*> ActorComponents;
		It->GetComponents(ActorComponents);
		Count += ActorComponents.Num();
	}
	return Count;
}

static bool IMG2BakeScaleInstanceIntersectsBakeRegion(const FIMG2BakeScaleSource& Source,
	const UStaticMesh* Mesh, const FTransform& WorldTransform, bool& bOutIntersects, FString& Error)
{
	const AIMAcousticBakeVolume* Volume = Source.Volume.Get();
	if (!IsValid(Volume) || !IsValid(Volume->BakeBounds) || !IsValid(Mesh))
	{
		Error = TEXT("Cannot determine bake-region inclusion for an invalid mesh, volume, or bounds component.");
		return false;
	}
	const FBox Region = Volume->BakeBounds->Bounds.GetBox();
	const FBox MeshBounds = Mesh->GetBounds().TransformBy(WorldTransform).GetBox();
	if (!Region.IsValid || !MeshBounds.IsValid)
	{
		Error = TEXT("Bake region or transformed LOD0 mesh bounds are invalid.");
		return false;
	}
	bOutIntersects = Region.Intersect(MeshBounds);
	return true;
}

static bool IMG2BakeScaleParseIntLogField(const FString& Line, const TCHAR* Field, int32& OutValue)
{
	TArray<FString> Tokens;
	Line.ParseIntoArrayWS(Tokens);
	const FString Prefix = FString(Field) + TEXT("=");
	for (const FString& Token : Tokens)
	{
		if (!Token.StartsWith(Prefix, ESearchCase::CaseSensitive)) continue;
		const FString Number = Token.RightChop(Prefix.Len());
		if (Number.IsEmpty()) return false;
		OutValue = FCString::Atoi(*Number);
		return true;
	}
	return false;
}

static bool IMG2BakeScaleCompareNumberField(const FJsonObject& A, const FJsonObject& B,
	const TCHAR* Field, FString& Error)
{
	double ValueA = 0.0;
	double ValueB = 0.0;
	if (!A.TryGetNumberField(Field, ValueA) || !B.TryGetNumberField(Field, ValueB)
		|| !FMath::IsNearlyEqual(ValueA, ValueB, 1.e-6))
	{
		Error = FString::Printf(TEXT("Bake recipe field differs: %s"), Field);
		return false;
	}
	return true;
}

static bool IMG2BakeScaleCompareStringField(const FJsonObject& A, const FJsonObject& B,
	const TCHAR* Field, FString& Error)
{
	FString ValueA;
	FString ValueB;
	if (!A.TryGetStringField(Field, ValueA) || !B.TryGetStringField(Field, ValueB) || ValueA != ValueB)
	{
		Error = FString::Printf(TEXT("Bake recipe field differs: %s"), Field);
		return false;
	}
	return true;
}

static bool IMG2BakeScaleCompareNumberArray(const FJsonObject& A, const FJsonObject& B,
	const TCHAR* Field, const int32 ExpectedCount, FString& Error)
{
	const TArray<TSharedPtr<FJsonValue>>* ArrayA = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ArrayB = nullptr;
	if (!A.TryGetArrayField(Field, ArrayA) || !B.TryGetArrayField(Field, ArrayB)
		|| ArrayA->Num() != ExpectedCount || ArrayB->Num() != ExpectedCount)
	{
		Error = FString::Printf(TEXT("Bake recipe array is absent or has an unexpected length: %s"), Field);
		return false;
	}
	for (int32 Index = 0; Index < ExpectedCount; ++Index)
	{
		if (!FMath::IsNearlyEqual((*ArrayA)[Index]->AsNumber(), (*ArrayB)[Index]->AsNumber(), 1.e-5))
		{
			Error = FString::Printf(TEXT("Bake recipe array differs: %s[%d]"), Field, Index);
			return false;
		}
	}
	return true;
}

static bool IMG2BakeScaleCompareMetadata(const FString& OriginalJson, const FString& NewJson,
	const int32 ExpectedTriangles, const int32 ExpectedProbes, FString& Error)
{
	TSharedPtr<FJsonObject> Original;
	TSharedPtr<FJsonObject> New;
	const TSharedRef<TJsonReader<>> OriginalReader = TJsonReaderFactory<>::Create(OriginalJson);
	const TSharedRef<TJsonReader<>> NewReader = TJsonReaderFactory<>::Create(NewJson);
	if (!FJsonSerializer::Deserialize(OriginalReader, Original) || !Original.IsValid()
		|| !FJsonSerializer::Deserialize(NewReader, New) || !New.IsValid())
	{
		Error = TEXT("Original or generated bake metadata is not valid JSON.");
		return false;
	}
	const TCHAR* NumberFields[] = {
		TEXT("recipe_version"), TEXT("sdk_version"),
		TEXT("probe_spacing_cm"), TEXT("probe_height_cm")};
	for (const TCHAR* Field : NumberFields)
		if (!IMG2BakeScaleCompareNumberField(*Original, *New, Field, Error)) return false;
	double NewProbeCount = 0.0;
	if (!New->TryGetNumberField(TEXT("probe_count"), NewProbeCount)
		|| static_cast<int32>(NewProbeCount) != ExpectedProbes)
	{
		Error = FString::Printf(TEXT("New bake metadata probe count differs from generated count %d."), ExpectedProbes);
		return false;
	}

	double NewTriangles = 0.0;
	if (!New->TryGetNumberField(TEXT("triangles"), NewTriangles)
		|| static_cast<int32>(NewTriangles) != ExpectedTriangles)
	{
		Error = FString::Printf(TEXT("New bake metadata triangle count differs from expected %d."), ExpectedTriangles);
		return false;
	}
	if (!IMG2BakeScaleCompareNumberArray(*Original, *New, TEXT("bounds_origin_cm"), 3, Error)
		|| !IMG2BakeScaleCompareNumberArray(*Original, *New, TEXT("bounds_extent_cm"), 3, Error))
		return false;

	const TArray<TSharedPtr<FJsonValue>>* MaterialsA = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MaterialsB = nullptr;
	if (!Original->TryGetArrayField(TEXT("materials"), MaterialsA)
		|| !New->TryGetArrayField(TEXT("materials"), MaterialsB)
		|| MaterialsA->Num() != MaterialsB->Num())
	{
		Error = TEXT("Bake material mapping count changed.");
		return false;
	}
	for (int32 Index = 0; Index < MaterialsA->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> MaterialA = (*MaterialsA)[Index]->AsObject();
		const TSharedPtr<FJsonObject> MaterialB = (*MaterialsB)[Index]->AsObject();
		if (!MaterialA.IsValid() || !MaterialB.IsValid()
			|| !IMG2BakeScaleCompareStringField(*MaterialA, *MaterialB, TEXT("asset_path"), Error)
			|| !IMG2BakeScaleCompareNumberField(*MaterialA, *MaterialB, TEXT("scattering"), Error)
			|| !IMG2BakeScaleCompareNumberArray(*MaterialA, *MaterialB, TEXT("absorption"), 3, Error))
			return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ProbesA = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ProbesB = nullptr;
	if (!Original->TryGetArrayField(TEXT("probes_m"), ProbesA)
		|| !New->TryGetArrayField(TEXT("probes_m"), ProbesB)
		|| ProbesA->Num() != IMG2BakeScaleBaselineProbes
		|| ProbesB->Num() != ExpectedProbes)
	{
		Error = TEXT("Original or generated bake probe metadata is incomplete.");
		return false;
	}
	// UNIFORMFLOOR places probes against actual scene geometry: adding meshes can
	// change the count while spacing, height, bounds and SDK recipe stay fixed.
	for (int32 ProbeIndex = 0; ProbeIndex < ProbesB->Num(); ++ProbeIndex)
	{
		const TArray<TSharedPtr<FJsonValue>>& Probe = (*ProbesB)[ProbeIndex]->AsArray();
		if (Probe.Num() != 4)
		{
			Error = FString::Printf(TEXT("Generated probe metadata tuple %d is malformed."), ProbeIndex);
			return false;
		}
		for (int32 Component = 0; Component < 4; ++Component)
		{
			if (!FMath::IsFinite(Probe[Component]->AsNumber()))
			{
				Error = FString::Printf(TEXT("Generated probe %d component %d is not finite."), ProbeIndex, Component);
				return false;
			}
		}
		if (Probe[3]->AsNumber() <= 0.0)
		{
			Error = FString::Printf(TEXT("Generated probe %d has no positive radius."), ProbeIndex);
			return false;
		}
	}

	const TSharedPtr<FJsonObject>* ReflectionA = nullptr;
	const TSharedPtr<FJsonObject>* ReflectionB = nullptr;
	if (!Original->TryGetObjectField(TEXT("reflection"), ReflectionA)
		|| !New->TryGetObjectField(TEXT("reflection"), ReflectionB)
		|| !ReflectionA || !ReflectionB || !ReflectionA->IsValid() || !ReflectionB->IsValid())
	{
		Error = TEXT("Reflection recipe metadata is missing.");
		return false;
	}
	const TCHAR* ReflectionNumbers[] = {TEXT("num_rays"), TEXT("num_bounces"),
		TEXT("num_diffuse_samples"), TEXT("irradiance_min_m"), TEXT("sim_duration_s"),
		TEXT("saved_duration_s"), TEXT("order")};
	for (const TCHAR* Field : ReflectionNumbers)
		if (!IMG2BakeScaleCompareNumberField(**ReflectionA, **ReflectionB, Field, Error)) return false;
	if (!IMG2BakeScaleCompareStringField(**ReflectionA, **ReflectionB, TEXT("type"), Error)) return false;

	const TSharedPtr<FJsonObject>* PathA = nullptr;
	const TSharedPtr<FJsonObject>* PathB = nullptr;
	if (!Original->TryGetObjectField(TEXT("path"), PathA)
		|| !New->TryGetObjectField(TEXT("path"), PathB)
		|| !PathA || !PathB || !PathA->IsValid() || !PathB->IsValid())
	{
		Error = TEXT("Pathing recipe metadata is missing.");
		return false;
	}
	const TCHAR* PathNumbers[] = {TEXT("num_samples"), TEXT("radius_m"), TEXT("threshold"),
		TEXT("vis_range_m"), TEXT("path_range_m")};
	for (const TCHAR* Field : PathNumbers)
		if (!IMG2BakeScaleCompareNumberField(**PathA, **PathB, Field, Error)) return false;
	return true;
}

static const TCHAR* IMG2BakeScaleOutcomeName(const TCHAR* Outcome)
{
	return Outcome;
}

class FIMAcousticG2BakeScaleCommand final : public IAutomationLatentCommand
{
public:
	FIMAcousticG2BakeScaleCommand(FAutomationTestBase* InTest, const int32 InTargetPlacements,
		const int32 InUniqueMeshCount = 0,
		const EIMG2BakeScaleRepresentation InRepresentation = EIMG2BakeScaleRepresentation::StaticMeshActors)
		: Test(InTest), TargetPlacements(InTargetPlacements), UniqueMeshCount(InUniqueMeshCount),
			Representation(InRepresentation), bUniqueMeshScenario(InUniqueMeshCount > 0),
			bInstancedMeshScenario(InRepresentation != EIMG2BakeScaleRepresentation::StaticMeshActors),
			RunId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
	{
		StartedAt = FPlatformTime::Seconds();
		LogCapture = MakeUnique<FIMG2BakeScaleLogCapture>();
	}

	bool Update() override
	{
		if (bFinished) return true;
		if (Stage == 0) return StartCase();
		if (Stage == 3) return GenerateAndStartBake();
		if (Stage == 1) return PollBake();
		if (Stage == 2) return PollEditorTicker();
		return Finish(TEXT("FAIL"), TEXT("Internal G2 BakeScale stage was invalid."));
	}

private:
	bool StartCase()
	{
		SampleMemory(TEXT("before_map_load"), true);
		if (!FEditorFileUtils::LoadMap(IMG2BakeScaleMap, false, true))
			return Finish(TEXT("FAIL"), TEXT("Cannot load the required saved W2 whitebox map."));
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FString Error;
		if (!IMG2BakeScaleReadSource(World, Source, Error, true))
			return Finish(TEXT("FAIL"), Error);
		bBaselineCaptured = true;
		BaselineEnumeratedPrimitiveComponentCount =
			IMG2BakeScaleCountEnumeratedPrimitiveComponents(World, Source.Volume.Get());
		if (BaselineEnumeratedPrimitiveComponentCount == INDEX_NONE)
			return Finish(TEXT("FAIL"), TEXT("Could not snapshot the original world primitive-component count."));
		SampleMemory(TEXT("validated_w2_baseline"), true);
		if (bUniqueMeshScenario)
		{
			if (TargetPlacements != IMG2BakeScaleCounts[1]
				|| UniqueMeshCount != IMG2BakeScaleUniqueMeshCount)
				return Finish(TEXT("FAIL"), TEXT("Unique-mesh BakeScale supports only the fixed 3000-placement/1000-identity case."));
			if (!IMG2BakeScaleSnapshotBakeDirectory(Source, BakeDirectory, PreRunBakeFiles, Error))
				return Finish(TEXT("FAIL"), Error);
			PreRunBakeFileCount = PreRunBakeFiles.Num();
		}

		if (LogCapture.IsValid() && GLog)
		{
			GLog->AddOutputDevice(LogCapture.Get());
			bLogCaptureAttached = true;
		}
		else
		{
			return Finish(TEXT("FAIL"), TEXT("Could not attach the product SDK-stage log observer."));
		}

		const FIMG2BakeScaleMemorySample Preflight = IMG2BakeScaleReadMemory(TEXT("preflight_before_scale"), StartedAt);
		RememberMemory(Preflight);
		if (Preflight.AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			ResourceReason = TEXT("System available physical RAM was below 8 GiB before scale construction.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}
		if (FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds)
		{
			ResourceReason = TEXT("The 900 second single-case duration guard expired before scale construction.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}

		FIMG2BakeScalePlacementPlan Plan;
		const int32 ExtraCount = TargetPlacements - IMG2BakeScaleBaselinePlacements;
		if (!IMG2BakeScaleBuildPlan(Source, ExtraCount, Plan, Error))
			return Finish(TEXT("FAIL"), Error);
		GridColumns = Plan.Columns;
		GridRows = Plan.Rows;
		CubeScale = Plan.UniformScale;
		CubeSizeCm = Plan.CubeSizeCm;
		GridCellWidthCm = Plan.CellWidthCm;
		GridCellHeightCm = Plan.CellHeightCm;
		if (bUniqueMeshScenario)
		{
			SampleMemory(TEXT("before_unique_mesh_build"), true);
			bool bMeshBuildResourceGuard = false;
			if (!IMG2BakeScaleBuildUniqueMeshes(Source, UniqueMeshCount, RunId, StartedAt,
					MemorySamples, UniqueMeshes, UniqueMeshSemanticDigest, UniqueMeshTriangleCount,
					UniqueMeshBuildSeconds, bMeshBuildResourceGuard, Error))
			{
				RecomputeMemoryAggregates();
				if (bMeshBuildResourceGuard)
				{
					ResourceReason = Error;
					return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
				}
				return Finish(TEXT("FAIL"), Error);
			}
			RecomputeMemoryAggregates();
			if (UniqueMeshes.Num() != UniqueMeshCount
				|| UniqueMeshTriangleCount != Source.CubeLOD0Triangles)
				return Finish(TEXT("FAIL"), TEXT("Transient mesh identity fixture did not retain the exact 1000-mesh/LOD0 triangle contract."));
		}
		const double PlacementBuildStartedAt = FPlatformTime::Seconds();

		AIMAcousticBakeVolume* Volume = Source.Volume.Get();
		AActor* LastActor = nullptr;
		if (bUniqueMeshScenario) UniqueMeshPlacementCounts.Init(0, UniqueMeshes.Num());
		if (bInstancedMeshScenario)
		{
			TransientPlacementsInRegion = 0;
			TransientPlacementsOutsideRegion = 0;
			bool bPlacementResourceGuard = false;
			if (!BuildInstancedPlacements(World, Plan, LastActor, Error, bPlacementResourceGuard))
			{
				if (bPlacementResourceGuard)
				{
					ResourceReason = Error;
					return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
				}
				return Finish(TEXT("FAIL"), Error);
			}
		}
		else
		{
			for (int32 Index = 0; Index < Plan.Transforms.Num(); ++Index)
			{
				if ((Index % 64) == 0)
				{
					SampleMemory(FString::Printf(TEXT("constructing_placements_%d"), Index), true);
					if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
					{
						ResourceReason = FString::Printf(
							TEXT("System available physical RAM fell below 8 GiB while constructing placement %d of %d; scale construction stopped."),
							Index, Plan.Transforms.Num());
						return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
					}
					if (FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds)
					{
						ResourceReason = TEXT("The 900 second single-case duration guard expired during placement construction.");
						return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
					}
				}

				FActorSpawnParameters SpawnParameters;
				SpawnParameters.ObjectFlags |= RF_Transient;
				AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
					FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
				if (!IsValid(Actor))
					return Finish(TEXT("FAIL"), FString::Printf(TEXT("Could not create transient StaticMeshActor %d."), Index));
				Actor->SetActorTransform(Plan.Transforms[Index]);
				UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
				if (!IsValid(Component))
					return Finish(TEXT("FAIL"), FString::Printf(TEXT("Transient StaticMeshActor %d has no component."), Index));
				Component->SetMobility(EComponentMobility::Static);
				const int32 UniqueMeshIndex = bUniqueMeshScenario ? Index % UniqueMeshes.Num() : INDEX_NONE;
				UStaticMesh* PlacementMesh = bUniqueMeshScenario
					? UniqueMeshes[UniqueMeshIndex].Get() : Source.Mesh.Get();
				Component->SetStaticMesh(PlacementMesh);
				Component->SetMaterial(0, Source.Material.Get());
				if (UPhysicalMaterial* PhysicalOverride = Source.PhysicalMaterialOverride.Get())
					Component->BodyInstance.SetPhysMaterialOverride(PhysicalOverride);
				if (Component->Mobility != EComponentMobility::Static
					|| Component->GetStaticMesh() != PlacementMesh
					|| Component->GetMaterial(0) != Source.Material.Get())
				{
					return Finish(TEXT("FAIL"), FString::Printf(
						TEXT("Transient placement %d did not retain the W2 Cube, material, and Static mobility."), Index));
				}
				if (bUniqueMeshScenario) ++UniqueMeshPlacementCounts[UniqueMeshIndex];
				LastActor = Actor;
				++BuiltPlacements;
				++TransientMeshComponentCount;
			}
		}
		BuildSeconds = FPlatformTime::Seconds() - PlacementBuildStartedAt;
		if (BuiltPlacements != ExtraCount || !IsValid(LastActor))
		{
			return Finish(TEXT("FAIL"), FString::Printf(TEXT("Built %d of %d requested transient placements."),
				BuiltPlacements, ExtraCount));
		}
		ExpectedCapturePhaseComponentCount = BaselineEnumeratedPrimitiveComponentCount
			+ TransientMeshComponentCount;
		if (bInstancedMeshScenario
			&& (Source.WhiteboxPlacements != IMG2BakeScaleBaselinePlacements
				|| TransientPlacementsInRegion != ExtraCount || TransientPlacementsOutsideRegion != 0))
		{
			return Finish(TEXT("FAIL"), FString::Printf(
				TEXT("%s placement region audit found saved=%d, transient inside=%d, transient outside=%d; expected 60 + %d + 0."),
				IMG2BakeScaleRepresentationName(Representation), Source.WhiteboxPlacements,
				TransientPlacementsInRegion, TransientPlacementsOutsideRegion, ExtraCount));
		}
		if (bUniqueMeshScenario)
		{
			int32 ThreePlacementMeshes = 0;
			int32 TwoPlacementMeshes = 0;
			for (int32 Index = 0; Index < UniqueMeshPlacementCounts.Num(); ++Index)
			{
				const int32 ExpectedUseCount = Index < 940 ? 3 : 2;
				if (UniqueMeshPlacementCounts[Index] != ExpectedUseCount)
					return Finish(TEXT("FAIL"), FString::Printf(
						TEXT("Transient mesh identity %d has %d added placements; expected %d."),
						Index, UniqueMeshPlacementCounts[Index], ExpectedUseCount));
				if (ExpectedUseCount == 3) ++ThreePlacementMeshes;
				else ++TwoPlacementMeshes;
			}
			if (ThreePlacementMeshes != 940 || TwoPlacementMeshes != 60)
				return Finish(TEXT("FAIL"), TEXT("Unique-mesh placement distribution is not exactly 940x3 plus 60x2."));
			UniqueMeshesPlacedThreeTimes = ThreePlacementMeshes;
			UniqueMeshesPlacedTwice = TwoPlacementMeshes;
		}
		SampleMemory(TEXT("placements_built"), true);
		Stage = 3;
		return false;
	}

	// Runs on a later Editor tick than the placement construction: the product defers the scene check ("retry validation after the Editor
	// tick") while freshly registered components still report empty cached bounds, so the same call is repeated until it stops deferring.
	bool GenerateAndStartBake()
	{
		AIMAcousticBakeVolume* Volume = Source.Volume.Get();
		FString Error;
		const double ProbeStartedAt = FPlatformTime::Seconds();
		Volume->GenerateProbes();
		if (Volume->ExportedTriangles == 0 && Volume->Status.StartsWith(TEXT("Scene components are still updating bounds"))
			&& FPlatformTime::Seconds() - StartedAt < IMG2BakeScaleMaxSeconds)
			return false;
		GenerateProbesSeconds = FPlatformTime::Seconds() - ProbeStartedAt;
		SampleMemory(TEXT("after_generate_probes"), true);
		ExpectedTriangles = TargetPlacements * Source.CubeLOD0Triangles;
		ActualTriangles = Volume->ExportedTriangles;
		ProbeCount = Volume->GeneratedProbes;
		if (Volume->ExportedTriangles != ExpectedTriangles)
		{
			// Name the product's own explanation (status and the first scene issues) so a short capture is diagnosable.
			// Group the issues by their text before the first object path so hundreds of per-component lines collapse into kinds,
			// then keep the last issue verbatim (the first ones are the map's own editor-only helpers).
			TMap<FString, int32> IssueKinds;
			for (const FString& Issue : Volume->SceneIssues)
			{
				int32 PathStart = INDEX_NONE;
				Issue.FindChar(TEXT('/'), PathStart);
				IssueKinds.FindOrAdd(PathStart > 0 ? Issue.Left(PathStart) : Issue) += 1;
			}
			FString Issues;
			for (const TPair<FString, int32>& Kind : IssueKinds)
				Issues += FString::Printf(TEXT(" | %s x%d"), *Kind.Key, Kind.Value);
			if (Volume->SceneIssues.Num() > 0)
				Issues += TEXT(" | last: ") + Volume->SceneIssues.Last();
			return Finish(TEXT("FAIL"), FString::Printf(
				TEXT("Production scene capture exported %d triangles; expected %d for %d Cube placements. Volume status: %s; scene issues (%d):%s"),
				Volume->ExportedTriangles, ExpectedTriangles, TargetPlacements, *Volume->Status, Volume->SceneIssues.Num(), *Issues));
		}
		if (bInstancedMeshScenario && !ValidateInstancedCapturePhase(Error))
			return Finish(TEXT("FAIL"), Error);
		if (Volume->GeneratedProbes <= 0)
			return Finish(TEXT("FAIL"), TEXT("Product GenerateProbes produced no valid probes for the scaled scene."));
		bObservedVolumeRecipeUnchanged = IMG2BakeScaleVolumeRecipeIdentity(Volume) == Source.VolumeRecipeIdentity;
		if (!bObservedVolumeRecipeUnchanged)
			return Finish(TEXT("FAIL"), TEXT("Volume BakeBounds or acoustic recipe changed during scene construction."));
		if (FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds)
		{
			ResourceReason = TEXT("The 900 second single-case duration guard expired after probe generation.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}
		if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			ResourceReason = TEXT("System available physical RAM fell below 8 GiB after probe generation; SDK Bake was not started.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}

		BakeStartedAt = FPlatformTime::Seconds();
		Volume->Bake();
		if (!Volume->Status.StartsWith(TEXT("Baking")))
			return Finish(TEXT("FAIL"), TEXT("Product Bake did not start: ") + Volume->Status);
		bBakeStarted = true;
		Stage = 1;
		SampleMemory(TEXT("bake_started"), true);
		return false;
	}

	bool RecordGeneratedAsset(UIMAcousticBakeAsset* CurrentAsset, FString& Error)
	{
		if (!IsValid(CurrentAsset) || CurrentAsset->GetPathName() == Source.OriginalAssetPath)
		{
			Error = TEXT("Product Bake did not produce a new bake asset.");
			return false;
		}
		NewAssetPath = CurrentAsset->GetPathName();
		NewAssetMetadata = CurrentAsset->MetadataJson;
		NewAssetSceneFingerprint = CurrentAsset->SceneFingerprint;
		NewPackedSceneBytes = CurrentAsset->SceneData.Num();
		NewPackedProbeBytes = CurrentAsset->ProbeData.Num();
		NewAssetPackedSceneDigest = IMG2BakeScaleHashByteArray(CurrentAsset->SceneData);
		NewAssetPackedProbeDigest = IMG2BakeScaleHashByteArray(CurrentAsset->ProbeData);
		NewAssetPackedPayloadDigest = CurrentAsset->PayloadDigest;
		NewAssetPackageName = CurrentAsset->GetOutermost()->GetName();
		if (!IMG2BakeScaleHashFile(NewAssetPackageName, NewAssetPackageFile,
				NewAssetPackageSha1, NewAssetPackageBytes, Error))
			return false;
		const FString ProjectRoot = IMG2BakeScaleCanonicalFilePath(FPaths::ProjectDir());
		const FString NewAssetFullPath = IMG2BakeScaleCanonicalFilePath(NewAssetPackageFile);
		bNewAssetInIsolatedProject = NewAssetFullPath.StartsWith(ProjectRoot, ESearchCase::IgnoreCase);
		if (NewAssetPath.IsEmpty() || !NewAssetPath.StartsWith(TEXT("/IceMoonAcousticField/Bakes/"))
			|| NewAssetPackageBytes <= 0 || !bNewAssetInIsolatedProject)
		{
			Error = TEXT("Generated bake asset is not a saved package under this isolated project.");
			return false;
		}
		if (bUniqueMeshScenario)
		{
			if (IMG2BakeScaleCanonicalFilePath(NewAssetPackageFile).Equals(
					IMG2BakeScaleCanonicalFilePath(Source.OriginalAssetPackageFile), ESearchCase::IgnoreCase))
			{
				Error = TEXT("Generated bake points at the original bound asset package; cleanup is refused.");
				return false;
			}
			if (!IMG2BakeScaleValidateRunBakePackage(NewAssetPackageName, NewAssetPath,
					NewAssetPackageFile, BakeDirectory, PreRunBakeFiles, NewAssetPackageFiles, Error)) return false;
			bNewAssetCreatedThisRun = true;
			bNewAssetCleanupCandidate = true;
		}
		return true;
	}

	bool BuildInstancedPlacements(UWorld* World, const FIMG2BakeScalePlacementPlan& Plan,
		AActor*& OutLastActor, FString& Error, bool& bOutResourceGuard)
	{
		bOutResourceGuard = false;
		OutLastActor = nullptr;
		if (!bUniqueMeshScenario || !bInstancedMeshScenario || !IsValid(World)
			|| UniqueMeshes.Num() != UniqueMeshCount || UniqueMeshPlacementCounts.Num() != UniqueMeshCount)
		{
			Error = TEXT("Instanced fixture prerequisites are incomplete.");
			return false;
		}

		const int32 ExpectedAddedPlacements = TargetPlacements - IMG2BakeScaleBaselinePlacements;
		if (Plan.Transforms.Num() != ExpectedAddedPlacements
			|| ExpectedAddedPlacements != 940 * 3 + 60 * 2)
		{
			Error = TEXT("Instanced fixture does not have the fixed 940x3 plus 60x2 world-transform plan.");
			return false;
		}

		for (int32 MeshIndex = 0; MeshIndex < UniqueMeshes.Num(); ++MeshIndex)
		{
			if ((MeshIndex % 32) == 0)
			{
				SampleMemory(FString::Printf(TEXT("constructing_%s_components_%d"),
					IMG2BakeScaleRepresentationName(Representation), MeshIndex), true);
				if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
				{
					bOutResourceGuard = true;
					Error = FString::Printf(
						TEXT("System available physical RAM fell below 8 GiB while creating %s component %d of %d."),
						IMG2BakeScaleRepresentationName(Representation), MeshIndex, UniqueMeshes.Num());
					return false;
				}
				if (FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds)
				{
					bOutResourceGuard = true;
					Error = FString::Printf(TEXT("The 900 second case guard expired while creating %s components."),
						IMG2BakeScaleRepresentationName(Representation));
					return false;
				}
			}

			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags |= RF_Transient;
			AActor* Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
			if (!IsValid(Actor))
			{
				Error = FString::Printf(TEXT("Could not create transient owner actor for %s mesh identity %d."),
					IMG2BakeScaleRepresentationName(Representation), MeshIndex);
				return false;
			}
			Actor->SetActorTransform(FTransform::Identity);

			UInstancedStaticMeshComponent* Component = nullptr;
			if (Representation == EIMG2BakeScaleRepresentation::HierarchicalInstancedStaticMesh)
			{
				Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient);
			}
			else
			{
				Component = NewObject<UInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient);
			}
			UStaticMesh* PlacementMesh = UniqueMeshes[MeshIndex].Get();
			if (!IsValid(Component) || !IsValid(PlacementMesh))
			{
				Error = FString::Printf(TEXT("Could not create %s component or unique mesh %d."),
					IMG2BakeScaleRepresentationName(Representation), MeshIndex);
				return false;
			}
			Component->SetMobility(EComponentMobility::Static);
			Component->SetStaticMesh(PlacementMesh);
			Component->SetMaterial(0, Source.Material.Get());
			if (UPhysicalMaterial* PhysicalOverride = Source.PhysicalMaterialOverride.Get())
				Component->BodyInstance.SetPhysMaterialOverride(PhysicalOverride);
			Actor->AddInstanceComponent(Component);
			// Use the instanced mesh itself as root so each clone contributes exactly one primitive component.
			Actor->SetRootComponent(Component);
			Component->SetRelativeTransform(FTransform::Identity);
			Component->RegisterComponent();
			if (!Component->IsRegistered() || Component->GetOwner() != Actor || Actor->GetRootComponent() != Component
				|| !Component->GetComponentTransform().Equals(FTransform::Identity, 1.e-3f))
			{
				Error = FString::Printf(TEXT("Transient %s component %d did not register as its owner's root."),
					IMG2BakeScaleRepresentationName(Representation), MeshIndex);
				return false;
			}

			const int32 ExpectedInstanceCount = MeshIndex < 940 ? 3 : 2;
			Component->PreAllocateInstancesMemory(ExpectedInstanceCount);
			for (int32 InstanceIndex = 0; InstanceIndex < ExpectedInstanceCount; ++InstanceIndex)
			{
				// Each 1000-transform batch is row-major; this preserves the same world placements as the SM case.
				const int32 TransformIndex = MeshIndex + InstanceIndex * UniqueMeshCount;
				const FTransform& ExpectedWorldTransform = Plan.Transforms[TransformIndex];
				bool bIntersectsRegion = false;
				if (!IMG2BakeScaleInstanceIntersectsBakeRegion(Source, PlacementMesh,
						ExpectedWorldTransform, bIntersectsRegion, Error))
					return false;
				if (bIntersectsRegion) ++TransientPlacementsInRegion;
				else ++TransientPlacementsOutsideRegion;
				const int32 AddedIndex = Component->AddInstance(ExpectedWorldTransform, true);
				if (AddedIndex != InstanceIndex)
				{
					Error = FString::Printf(
						TEXT("%s identity %d inserted instance at index %d; expected %d."),
						IMG2BakeScaleRepresentationName(Representation), MeshIndex, AddedIndex, InstanceIndex);
					return false;
				}
			}

			const int32 ActualInstanceCount = Component->GetInstanceCount();
			if (Component->Mobility != EComponentMobility::Static
				|| Component->GetStaticMesh() != PlacementMesh
				|| Component->GetMaterial(0) != Source.Material.Get()
				|| ActualInstanceCount != ExpectedInstanceCount)
			{
				Error = FString::Printf(
					TEXT("%s identity %d did not retain Static mobility, the W2 material, its unique Cube mesh, and all %d instances (actual=%d)."),
					IMG2BakeScaleRepresentationName(Representation), MeshIndex,
					ExpectedInstanceCount, ActualInstanceCount);
				return false;
			}
			for (int32 InstanceIndex = 0; InstanceIndex < ExpectedInstanceCount; ++InstanceIndex)
			{
				FTransform ActualWorldTransform;
				const int32 TransformIndex = MeshIndex + InstanceIndex * UniqueMeshCount;
				if (!Component->GetInstanceTransform(InstanceIndex, ActualWorldTransform, true)
					|| !ActualWorldTransform.Equals(Plan.Transforms[TransformIndex], 1.e-3f))
				{
					Error = FString::Printf(
						TEXT("%s identity %d instance %d did not retain its exact planned world transform."),
						IMG2BakeScaleRepresentationName(Representation), MeshIndex, InstanceIndex);
					return false;
				}
			}

			UniqueMeshPlacementCounts[MeshIndex] = ActualInstanceCount;
			TransientMeshComponentCount += 1;
			TransientMeshInstanceCount += ActualInstanceCount;
			BuiltPlacements += ActualInstanceCount;
			OutLastActor = Actor;
		}

		if (TransientMeshComponentCount != UniqueMeshCount
			|| TransientMeshInstanceCount != ExpectedAddedPlacements)
		{
			Error = FString::Printf(
				TEXT("Instanced fixture built %d components and %d instances; expected %d components and %d instances."),
				TransientMeshComponentCount, TransientMeshInstanceCount, UniqueMeshCount, ExpectedAddedPlacements);
			return false;
		}
		return true;
	}

	bool ValidateInstancedCapturePhase(FString& Error)
	{
		bool bFoundExpectedCapture = false;
		if (LogCapture.IsValid())
		{
			for (const FString& Line : LogCapture->Snapshot())
			{
				if (!Line.Contains(TEXT("IMLogs AcousticCapturePhases"))) continue;
				++CapturePhaseLogRecords;
				int32 ComponentCount = INDEX_NONE;
				int32 TriangleCount = INDEX_NONE;
				if (!IMG2BakeScaleParseIntLogField(Line, TEXT("components"), ComponentCount)
					|| !IMG2BakeScaleParseIntLogField(Line, TEXT("triangles"), TriangleCount))
					continue;
				CapturePhaseComponentCount = ComponentCount;
				CapturePhaseTriangleCount = TriangleCount;
				bFoundExpectedCapture |= ComponentCount == ExpectedCapturePhaseComponentCount
					&& TriangleCount == ExpectedTriangles;
			}
		}
		if (!bFoundExpectedCapture)
		{
			Error = FString::Printf(
				TEXT("Production CaptureScene did not report the expected %s representation: components=%d (expected %d), triangles=%d (expected %d), capture_phase_logs=%d."),
				IMG2BakeScaleRepresentationName(Representation), CapturePhaseComponentCount,
				ExpectedCapturePhaseComponentCount, CapturePhaseTriangleCount, ExpectedTriangles,
				CapturePhaseLogRecords);
			return false;
		}
		return true;
	}

	bool PollBake()
	{
		AIMAcousticBakeVolume* Volume = Source.Volume.Get();
		if (!IsValid(Volume)) return Finish(TEXT("FAIL"), TEXT("W2 bake volume was destroyed during product Bake."));
		const double Now = FPlatformTime::Seconds();
		if (Now - LastMemorySampleAt >= 1.0) SampleMemory(bCancelRequested ? TEXT("bake_cancel_wait") : TEXT("bake_wait"), true);

		if (!bCancelRequested)
		{
			if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
			{
				ResourceReason = TEXT("System available physical RAM fell below 8 GiB during product Bake; cancellation requested.");
				bCancelRequested = true;
				Volume->CancelBake();
			}
			else if (Now - StartedAt >= IMG2BakeScaleMaxSeconds)
			{
				ResourceReason = TEXT("The 900 second single-case duration guard expired during product Bake; cancellation requested.");
				bCancelRequested = true;
				Volume->CancelBake();
			}
		}

		if (bCancelRequested)
		{
			if (Volume->Status.StartsWith(TEXT("Cancellation requested"))
				|| Volume->Status.StartsWith(TEXT("Baking")))
				return false;
			BakeSeconds = FPlatformTime::Seconds() - BakeStartedAt;
			SampleMemory(TEXT("bake_cancelled_or_guarded"), true);
			if (UIMAcousticBakeAsset* CurrentAsset = Volume->BakedField.Get())
			{
				if (CurrentAsset->GetPathName() != Source.OriginalAssetPath)
				{
					FString CaptureError;
					if (!RecordGeneratedAsset(CurrentAsset, CaptureError)) CleanupFailure = CaptureError;
				}
			}
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}

		UIMAcousticBakeAsset* CurrentAsset = Volume->BakedField.Get();
		if (!CurrentAsset || CurrentAsset->GetPathName() == Source.OriginalAssetPath)
		{
			if (Volume->Status.StartsWith(TEXT("Baking"))
				|| Volume->Status.Contains(TEXT("Cancellation requested"))) return false;
			return Finish(TEXT("FAIL"), TEXT("Product Bake did not produce a new asset: ") + Volume->Status);
		}

		BakeSeconds = FPlatformTime::Seconds() - BakeStartedAt;
		FString CaptureError;
		if (!RecordGeneratedAsset(CurrentAsset, CaptureError))
		{
			return Finish(TEXT("FAIL"), TEXT("Could not capture the generated bake asset package: ") + CaptureError);
		}
		SampleMemory(TEXT("bake_completed_asset_saved"), true);

		FString ValidationFailure;
		const double ValidationStartedAt = FPlatformTime::Seconds();
		const bool bValid = Volume->ValidateCurrentBake(ValidationFailure);
		ValidateSeconds = FPlatformTime::Seconds() - ValidationStartedAt;
		SampleMemory(TEXT("after_validate_current_bake"), true);
		if (!bValid)
			return Finish(TEXT("FAIL"), TEXT("Generated scale bake failed ValidateCurrentBake: ") + ValidationFailure);
		if (Volume->ExportedTriangles != ExpectedTriangles
			|| Volume->GeneratedProbes != ProbeCount)
			return Finish(TEXT("FAIL"), FString::Printf(
				TEXT("Validated product state drifted: triangles=%d probes=%d."),
				Volume->ExportedTriangles, Volume->GeneratedProbes));
		bObservedVolumeRecipeUnchanged = IMG2BakeScaleVolumeRecipeIdentity(Volume) == Source.VolumeRecipeIdentity;
		if (!bObservedVolumeRecipeUnchanged)
			return Finish(TEXT("FAIL"), TEXT("Original BakeBounds or recipe changed while baking."));
		if (!IMG2BakeScaleCompareMetadata(Source.OriginalAssetMetadata, NewAssetMetadata,
				ExpectedTriangles, ProbeCount, ValidationFailure))
			return Finish(TEXT("FAIL"), TEXT("Generated bake metadata does not preserve the W2 recipe: ") + ValidationFailure);
		bMetadataRecipeMatches = true;
		if (FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds)
		{
			ResourceReason = TEXT("The 900 second single-case duration guard expired during product validation.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}
		if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			ResourceReason = TEXT("System available physical RAM fell below 8 GiB after product validation.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}

		Volume->IM_SetEditorValidationProfilingForTest(true);
		EditorTickerStartedAt = FPlatformTime::Seconds();
		LastObservedEditorTickerTicks = 0;
		LastObservedEditorTickerCaptureMs = 0.0;
		LastObservedEditorTickerAssetValidationMs = 0.0;
		EditorTickerObservations.Reset();
		SetTickerPhase(EIMG2BakeScaleTickerPhase::Baseline, EditorTickerStartedAt);
		Stage = 2;
		return false;
	}

	void SetTickerPhase(const EIMG2BakeScaleTickerPhase Phase, const double Now)
	{
		TickerPhase = Phase;
		EditorTickerPhaseTicks = 0;
		EditorTickerPhaseStartedAt = Now;
	}

	bool RecordEditorTickerObservation(AIMAcousticBakeVolume* Volume, const int32 Ticks,
		const double CaptureMs, const double AssetValidationMs, const double Now, FString& Error)
	{
		FIMG2BakeScaleTickerObservation Observation;
		Observation.Sequence = Ticks;
		Observation.Phase = IMG2BakeScaleTickerPhaseName(TickerPhase);
		Observation.Status = IsValid(Volume) ? Volume->Status : FString();
		Observation.CaptureMs = CaptureMs - LastObservedEditorTickerCaptureMs;
		Observation.AssetValidationMs = AssetValidationMs - LastObservedEditorTickerAssetValidationMs;
		Observation.FullTickMs = Observation.CaptureMs + Observation.AssetValidationMs;
		Observation.ElapsedSeconds = Now - StartedAt;
		Observation.PhaseElapsedSeconds = Now - EditorTickerPhaseStartedAt;
		Observation.ExpectedCaptured = 1;
		Observation.ExpectedBoundAssetValid = 1;
		Observation.ExpectedTriangles = ExpectedTriangles;

		FString StateFailure;
		if (!IsValid(Volume))
		{
			StateFailure = TEXT("Bake volume was invalid while recording an actual Editor ticker callback.");
		}
		else if (!FMath::IsFinite(Observation.CaptureMs) || !FMath::IsFinite(Observation.AssetValidationMs)
			|| Observation.CaptureMs < 0.0 || Observation.AssetValidationMs < 0.0)
		{
			StateFailure = TEXT("Editor ticker timing deltas were non-finite or moved backwards.");
		}
		else if (TickerPhase == EIMG2BakeScaleTickerPhase::Baseline)
		{
			if (Observation.Status.IsEmpty()
				|| Observation.Status.StartsWith(TEXT("Acoustic bake is stale"))
				|| Observation.Status.StartsWith(TEXT("Bake stale:")))
				StateFailure = TEXT("Baseline Editor ticker did not retain a non-stale bake status: ") + Observation.Status;
		}
		else if (TickerPhase == EIMG2BakeScaleTickerPhase::InstanceMoved)
		{
			Observation.ExpectedBoundAssetValid = 0;
			if (!Observation.Status.StartsWith(TEXT("Acoustic bake is stale")))
				StateFailure = TEXT("Moving the in-region ISM/HISM instance did not produce the expected stale-asset status: ") + Observation.Status;
		}
		else if (TickerPhase == EIMG2BakeScaleTickerPhase::InstanceRestored)
		{
			if (!Observation.Status.Equals(TEXT("Bake valid for current scene.")))
				StateFailure = TEXT("Restoring the exact instance transform did not recover the current-bake status: ") + Observation.Status;
		}
		else
		{
			StateFailure = TEXT("An unexpected phase was active when an Editor ticker callback was observed.");
		}

		EditorTickerObservations.Add(MoveTemp(Observation));
		if (!StateFailure.IsEmpty())
		{
			Error = FString::Printf(TEXT("Editor ticker sequence %d phase %s: %s"), Ticks,
				IMG2BakeScaleTickerPhaseName(TickerPhase), *StateFailure);
			return false;
		}
		return true;
	}

	bool BeginTickerInstanceEdit(AIMAcousticBakeVolume* Volume, const double Now, FString& Error)
	{
		UWorld* World = IsValid(Volume) ? Volume->GetWorld() : nullptr;
		if (!IsValid(World) || !Volume->BakedField)
		{
			Error = TEXT("Cannot start the G2 instance invalidation phase without the baked world and bound asset.");
			return false;
		}

		int32 AssetVolumeCount = 0;
		TickerBoundAssetPath = Volume->BakedField->GetPathName();
		TickerBoundAssetIdentityBefore = IMG2BakeScaleAssetIdentity(World, AssetVolumeCount);
		if (AssetVolumeCount != 1 || TickerBoundAssetPath != NewAssetPath
			|| TickerBoundAssetIdentityBefore.IsEmpty())
		{
			Error = TEXT("G2 ticker invalidation did not begin with the exact newly baked asset identity bound to its sole volume.");
			return false;
		}

		const FVector RegionMin = Source.BoundsOrigin - Source.BoundsExtent;
		const FVector RegionMax = Source.BoundsOrigin + Source.BoundsExtent;
		double BestMargin = -TNumericLimits<double>::Max();
		UInstancedStaticMeshComponent* BestComponent = nullptr;
		int32 BestInstanceIndex = INDEX_NONE;
		FTransform BestTransform = FTransform::Identity;
		UWorld* CurrentWorld = Volume->GetWorld();
		for (TActorIterator<AActor> ActorIt(CurrentWorld); ActorIt; ++ActorIt)
		{
			TArray<UPrimitiveComponent*> Components;
			ActorIt->GetComponents(Components);
			for (UPrimitiveComponent* Primitive : Components)
			{
				UInstancedStaticMeshComponent* Component = Cast<UInstancedStaticMeshComponent>(Primitive);
				if (!IsValid(Component) || !IsValid(Component->GetStaticMesh())) continue;
				bool bTransientIdentity = false;
				for (const TStrongObjectPtr<UStaticMesh>& MeshIdentity : UniqueMeshes)
				{
					bTransientIdentity |= MeshIdentity.Get() == Component->GetStaticMesh();
					if (bTransientIdentity) break;
				}
				if (!bTransientIdentity) continue;

				for (int32 InstanceIndex = 0; InstanceIndex < Component->GetInstanceCount(); ++InstanceIndex)
				{
					FTransform Transform;
					if (!Component->GetInstanceTransform(InstanceIndex, Transform, true)) continue;
					const FBox Bounds = IMG2BakeScaleTransformBounds(Component->GetStaticMesh(), Transform);
					const double Margin = IMG2BakeScaleBoundsInteriorMargin(Bounds, RegionMin, RegionMax);
					if (Margin > BestMargin)
					{
						BestMargin = Margin;
						BestComponent = Component;
						BestInstanceIndex = InstanceIndex;
						BestTransform = Transform;
					}
				}
			}
		}
		if (!IsValid(BestComponent) || BestInstanceIndex == INDEX_NONE || BestMargin < 5.0)
		{
			Error = FString::Printf(
				TEXT("No transient ISM/HISM instance has a full-mesh margin of at least 5 cm inside BakeBounds (best=%.3f cm)."),
				BestMargin);
			return false;
		}

		TickerInstanceComponent = BestComponent;
		TickerInstanceComponentPath = BestComponent->GetPathName();
		TickerInstanceIndex = BestInstanceIndex;
		TickerInstanceSafetyMarginCm = BestMargin;
		TickerInstanceOriginalTransform = BestTransform;
		TickerInstanceEditedTransform = BestTransform;
		TickerInstanceEditedTransform.AddToTranslation(FVector(1.0, 0.0, 0.0));
		const FVector Delta = TickerInstanceEditedTransform.GetLocation() - TickerInstanceOriginalTransform.GetLocation();
		bTickerInstanceEditWasOnlyXPlusOneCm = FMath::IsNearlyEqual(Delta.X, 1.0, 1.e-4)
			&& FMath::IsNearlyZero(Delta.Y, 1.e-4) && FMath::IsNearlyZero(Delta.Z, 1.e-4)
			&& TickerInstanceEditedTransform.GetRotation().Equals(TickerInstanceOriginalTransform.GetRotation(), 1.e-6)
			&& TickerInstanceEditedTransform.GetScale3D().Equals(TickerInstanceOriginalTransform.GetScale3D(), 1.e-6);
		if (!bTickerInstanceEditWasOnlyXPlusOneCm
			|| !BestComponent->UpdateInstanceTransform(BestInstanceIndex, TickerInstanceEditedTransform, true, true, true))
		{
			Error = TEXT("The selected transient ISM/HISM instance could not be changed by exactly X + 1 cm.");
			return false;
		}
		bTickerInstanceEditApplied = true;
		FTransform ActualEditedTransform;
		const bool bActualEditMatches = BestComponent->GetInstanceTransform(BestInstanceIndex, ActualEditedTransform, true)
			&& ActualEditedTransform.Equals(TickerInstanceEditedTransform, 1.e-3f);
		if (!bActualEditMatches)
		{
			Error = TEXT("The selected instance did not retain the exact requested X + 1 cm world transform.");
			return false;
		}
		SetTickerPhase(EIMG2BakeScaleTickerPhase::InstanceMoved, Now);
		return true;
	}

	bool RestoreTickerInstanceTransform(FString& Error)
	{
		UInstancedStaticMeshComponent* Component = TickerInstanceComponent.Get();
		if (!bTickerInstanceEditApplied || !IsValid(Component)
			|| TickerInstanceIndex < 0 || TickerInstanceIndex >= Component->GetInstanceCount()
			|| !Component->UpdateInstanceTransform(TickerInstanceIndex, TickerInstanceOriginalTransform, true, true, true))
		{
			Error = TEXT("Could not restore the exact saved ISM/HISM world transform.");
			return false;
		}
		FTransform ActualRestoredTransform;
		bTickerInstanceTransformRestored = Component->GetInstanceTransform(
			TickerInstanceIndex, ActualRestoredTransform, true)
			&& ActualRestoredTransform.Equals(TickerInstanceOriginalTransform, 1.e-3f);
		if (!bTickerInstanceTransformRestored)
		{
			Error = TEXT("The selected ISM/HISM instance did not return to its exact saved world transform.");
			return false;
		}
		return true;
	}

	bool CompleteBaselineTickerCheck(AIMAcousticBakeVolume* Volume)
	{
		SnapshotTickerProfile();
		FString Error;
		if (!ValidateEditorTickerLogs(false, Error)) return Finish(TEXT("FAIL"), Error);
		Volume->IM_SetEditorValidationProfilingForTest(false);
		SampleMemory(TEXT("after_three_editor_ticks"), true);
		FString ValidationFailure;
		const double FinalValidationStartedAt = FPlatformTime::Seconds();
		const bool bStillValid = Volume->ValidateCurrentBake(ValidationFailure);
		PostTickerValidateSeconds = FPlatformTime::Seconds() - FinalValidationStartedAt;
		if (!bStillValid || Volume->ExportedTriangles != ExpectedTriangles)
			return Finish(TEXT("FAIL"), TEXT("Bake became invalid after scheduled Editor ticks: ") + ValidationFailure);
		bObservedVolumeRecipeUnchanged = IMG2BakeScaleVolumeRecipeIdentity(Volume) == Source.VolumeRecipeIdentity;
		if (!bObservedVolumeRecipeUnchanged)
			return Finish(TEXT("FAIL"), TEXT("Bake recipe changed during scheduled Editor validation ticks."));
		SampleMemory(TEXT("after_post_ticker_validation"), true);
		if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			ResourceReason = TEXT("System available physical RAM fell below 8 GiB after scheduled Editor validation ticks.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}
		return Finish(TEXT("PASS"), TEXT("G2 scale product Bake, validation, and three scheduled Editor ticks completed."));
	}

	bool CompleteTickerInstanceEdit(AIMAcousticBakeVolume* Volume, const double Now)
	{
		FString Error;
		UWorld* World = IsValid(Volume) ? Volume->GetWorld() : nullptr;
		int32 AssetVolumeCount = 0;
		TickerBoundAssetIdentityAfter = IsValid(World)
			? IMG2BakeScaleAssetIdentity(World, AssetVolumeCount) : FString();
		bTickerBoundAssetIdentityPreserved = IsValid(Volume) && Volume->BakedField
			&& AssetVolumeCount == 1 && Volume->BakedField->GetPathName() == TickerBoundAssetPath
			&& TickerBoundAssetIdentityAfter == TickerBoundAssetIdentityBefore;
		bObservedVolumeRecipeUnchanged = IMG2BakeScaleVolumeRecipeIdentity(Volume) == Source.VolumeRecipeIdentity;
		if (!bTickerInstanceTransformRestored || !bTickerBoundAssetIdentityPreserved
			|| !bObservedVolumeRecipeUnchanged)
		{
			return Finish(TEXT("FAIL"), TEXT("Ticker invalidation recovery did not restore the instance, volume recipe, and bound asset/payload identity exactly."));
		}

		if (!Volume->Status.Equals(TEXT("Bake valid for current scene.")))
			return Finish(TEXT("FAIL"), TEXT("Recovered Editor ticker did not report the bound bake valid: ") + Volume->Status);

		FString ValidationFailure;
		const double FinalValidationStartedAt = FPlatformTime::Seconds();
		const bool bStillValid = Volume->ValidateCurrentBake(ValidationFailure);
		PostTickerValidateSeconds = FPlatformTime::Seconds() - FinalValidationStartedAt;
		// A stale edit discards session-generated probes. Recovery validates the
		// persisted bake; it does not regenerate that transient preview cache.
		if (!bStillValid || Volume->ExportedTriangles != ExpectedTriangles)
			return Finish(TEXT("FAIL"), TEXT("Final ValidateCurrentBake failed after ticker recovery: ") + ValidationFailure);
		bTickerFinalValidatePassed = true;

		SampleMemory(TEXT("after_post_ticker_validation"), true);
		if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			ResourceReason = TEXT("System available physical RAM fell below 8 GiB after ticker invalidation recovery.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}

		bEditorTickerInvalidationComplete = true;
		TickerPhase = EIMG2BakeScaleTickerPhase::Complete;
		SnapshotTickerProfile();
		Volume->IM_SetEditorValidationProfilingForTest(false);
		SampleMemory(TEXT("after_ticker_invalidation_and_restore"), true);
		bWaitingForEditorTickerLogDrain = true;
		EditorTickerLogDrainStartedAt = FPlatformTime::Seconds();
		return false;
	}

	TArray<FString> SnapshotEditorTickerLogLines() const
	{
		TArray<FString> Result;
		if (LogCapture.IsValid())
		{
			for (const FString& Line : LogCapture->Snapshot())
				if (Line.Contains(TEXT("IMLogs AcousticG2EditorTicker"))) Result.Add(Line);
		}
		return Result;
	}

	bool ValidateEditorTickerLogs(const bool bRequireEveryTick, FString& Error)
	{
		const TArray<FString> Lines = SnapshotEditorTickerLogLines();
		ObservedEditorTickerLogs = Lines.Num();
		if (Lines.Num() > EditorTickerObservations.Num())
		{
			bEditorTickerLogsValid = false;
			Error = FString::Printf(TEXT("Observed %d Editor ticker log lines for only %d actual ticker callbacks."),
				Lines.Num(), EditorTickerObservations.Num());
			return false;
		}
		if (bRequireEveryTick && Lines.Num() != EditorTickerObservations.Num())
		{
			bEditorTickerLogsValid = false;
			Error = FString::Printf(TEXT("Only %d of %d actual Editor ticker callbacks reached the log observer."),
				Lines.Num(), EditorTickerObservations.Num());
			return false;
		}

		bEditorTickerLogsValid = true;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			FIMG2BakeScaleTickerObservation& Observation = EditorTickerObservations[Index];
			const FString& Line = Lines[Index];
			const bool bParsed = IMG2BakeScaleParseIntLogField(Line, TEXT("captured"), Observation.ObservedCaptured)
				&& IMG2BakeScaleParseIntLogField(Line, TEXT("bound_asset_valid"), Observation.ObservedBoundAssetValid)
				&& IMG2BakeScaleParseIntLogField(Line, TEXT("triangles"), Observation.ObservedTriangles);
			if (!bParsed || Observation.ObservedCaptured != Observation.ExpectedCaptured
				|| Observation.ObservedBoundAssetValid != Observation.ExpectedBoundAssetValid
				|| Observation.ObservedTriangles != Observation.ExpectedTriangles)
			{
				bEditorTickerLogsValid = false;
				Error = FString::Printf(
					TEXT("Editor ticker log sequence %d did not match phase %s: parsed=%d captured=%d/%d bound_asset_valid=%d/%d triangles=%d/%d."),
					Observation.Sequence, *Observation.Phase, bParsed ? 1 : 0,
					Observation.ObservedCaptured, Observation.ExpectedCaptured,
					Observation.ObservedBoundAssetValid, Observation.ExpectedBoundAssetValid,
					Observation.ObservedTriangles, Observation.ExpectedTriangles);
				return false;
			}
		}
		bEditorTickerLogValidationComplete = bRequireEveryTick
			&& Lines.Num() == EditorTickerObservations.Num();
		return true;
	}

	bool PollEditorTickerLogDrain()
	{
		const TArray<FString> Lines = SnapshotEditorTickerLogLines();
		ObservedEditorTickerLogs = Lines.Num();
		if (Lines.Num() > EditorTickerObservations.Num())
			return Finish(TEXT("FAIL"), FString::Printf(
				TEXT("Editor ticker observer received %d log lines for %d measured callbacks."),
				Lines.Num(), EditorTickerObservations.Num()));
		if (Lines.Num() < EditorTickerObservations.Num())
		{
			if (FPlatformTime::Seconds() - EditorTickerLogDrainStartedAt > 10.0)
				return Finish(TEXT("FAIL"), FString::Printf(
					TEXT("Editor ticker observer received only %d of %d required per-callback log lines after 10 seconds."),
					Lines.Num(), EditorTickerObservations.Num()));
			return false;
		}
		FString Error;
		if (!ValidateEditorTickerLogs(true, Error)) return Finish(TEXT("FAIL"), Error);
		if (!bEditorTickerInvalidationComplete || !bTickerInstanceTransformRestored
			|| !bTickerBoundAssetIdentityPreserved || !bObservedVolumeRecipeUnchanged)
			return Finish(TEXT("FAIL"), TEXT("Editor ticker invalidation evidence was incomplete after log drain."));
		return Finish(TEXT("PASS"), TEXT("G2 ISM/HISM product Bake recovered exact transform edits through actual Editor ticker callbacks."));
	}

	bool PollEditorTicker()
	{
		if (bWaitingForEditorTickerLogDrain) return PollEditorTickerLogDrain();

		AIMAcousticBakeVolume* Volume = Source.Volume.Get();
		if (!IsValid(Volume)) return Finish(TEXT("FAIL"), TEXT("Bake volume was destroyed before scheduled Editor validation ticks completed."));
		const double Now = FPlatformTime::Seconds();
		if (Now - LastMemorySampleAt >= 1.0) SampleMemory(TEXT("editor_ticker_wait"), true);
		if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			ResourceReason = TEXT("System available physical RAM fell below 8 GiB while waiting for scheduled Editor validation ticks.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}
		if (Now - StartedAt >= IMG2BakeScaleMaxSeconds)
		{
			ResourceReason = TEXT("The 900 second single-case duration guard expired while waiting for scheduled Editor validation ticks.");
			return Finish(TEXT("INCONCLUSIVE"), ResourceReason);
		}

		int32 Ticks = 0;
		double CaptureMs = 0.0;
		double AssetValidationMs = 0.0;
		double MaxTickMs = 0.0;
		Volume->IM_GetEditorValidationProfileForTest(Ticks, CaptureMs, AssetValidationMs, MaxTickMs);
		if (Ticks < LastObservedEditorTickerTicks)
			return Finish(TEXT("FAIL"), TEXT("Scheduled Editor ticker counter moved backwards."));
		if (Ticks == LastObservedEditorTickerTicks) return false;
		if (Ticks != LastObservedEditorTickerTicks + 1)
			return Finish(TEXT("FAIL"), FString::Printf(
				TEXT("Could not attribute every scheduled Editor ticker callback to one observed phase: previous=%d current=%d."),
				LastObservedEditorTickerTicks, Ticks));
		FString ObservationError;
		if (!RecordEditorTickerObservation(Volume, Ticks, CaptureMs, AssetValidationMs, Now, ObservationError))
			return Finish(TEXT("FAIL"), ObservationError);

		LastObservedEditorTickerTicks = Ticks;
		LastObservedEditorTickerCaptureMs = CaptureMs;
		LastObservedEditorTickerAssetValidationMs = AssetValidationMs;
		if (TickerPhase == EIMG2BakeScaleTickerPhase::InstanceRestored)
			bTickerInstanceRestoreTickerObserved = true;
		++EditorTickerPhaseTicks;

		if (TickerPhase == EIMG2BakeScaleTickerPhase::Baseline)
		{
			if (EditorTickerPhaseTicks < IMG2BakeScaleEditorTicks) return false;
			if (!bInstancedMeshScenario) return CompleteBaselineTickerCheck(Volume);
			if (!BeginTickerInstanceEdit(Volume, Now, ObservationError))
				return Finish(TEXT("FAIL"), ObservationError);
			return false;
		}
		if (TickerPhase == EIMG2BakeScaleTickerPhase::InstanceMoved)
		{
			if (!RestoreTickerInstanceTransform(ObservationError))
				return Finish(TEXT("FAIL"), ObservationError);
			SetTickerPhase(EIMG2BakeScaleTickerPhase::InstanceRestored, Now);
			return false;
		}
		if (TickerPhase == EIMG2BakeScaleTickerPhase::InstanceRestored)
			return CompleteTickerInstanceEdit(Volume, Now);

		return Finish(TEXT("FAIL"), TEXT("Unexpected G2 Editor ticker invalidation phase."));
	}

	bool UnloadPackageByExactName(bool& bOutUnloaded, FString& Error)
	{
		bOutUnloaded = false;
		UPackage* Package = FindPackage(nullptr, *NewAssetPackageName);
		if (!Package)
		{
			bOutUnloaded = true;
			return true;
		}
		if (Package->IsDirty())
		{
			Error = TEXT("Generated bake package became dirty; refusing to unload or delete it.");
			return false;
		}

		TArray<UPackage*> PackagesToUnload;
		PackagesToUnload.Add(Package);
		UPackageTools::FUnloadPackageParams Params(PackagesToUnload);
		Params.bUnloadDirtyPackages = false;
		Params.bResetTransBuffer = false;
		const bool bUnloadReported = UPackageTools::UnloadPackages(Params);
		bOutUnloaded = bUnloadReported && FindPackage(nullptr, *NewAssetPackageName) == nullptr;
		if (!bOutUnloaded)
		{
			Error = TEXT("UPackageTools could not unload the exact generated bake package without resetting the transaction buffer.");
			return false;
		}
		return true;
	}

	bool HasExternalPackageReferencers(UPackage* Package, FString& Error) const
	{
		if (!IsValid(Package))
		{
			Error = TEXT("Cannot verify references for an invalid generated package.");
			return false;
		}
		TArray<UObject*> PackageObjects;
		ForEachObjectWithPackage(Package, [&PackageObjects](UObject* Object)
		{
			PackageObjects.Add(Object);
			return true;
		});
		PackageObjects.Add(Package);
		const TArray<UObject*> Referencers = FReferencerFinder::GetAllReferencers(
			PackageObjects, nullptr, EReferencerFinderFlags::SkipWeakReferences);
		for (UObject* Referencer : Referencers)
		{
			if (IsValid(Referencer) && !Referencer->IsIn(Package))
			{
				Error = TEXT("Generated bake package still has an external UObject referencer: ")
					+ Referencer->GetPathName();
				return false;
			}
		}
		return true;
	}

	bool ColdLoadGeneratedPackage(FString& Error)
	{
		if (!bUniqueMeshScenario || !bNewAssetCleanupCandidate || !bNewAssetCreatedThisRun)
		{
			Error = TEXT("Unique-mesh cold load requires a validated, run-created GUID package.");
			return false;
		}
		if (FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds)
		{
			bColdLoadResourceGuard = true;
			ResourceReason = TEXT("The 900 second case guard expired before generated bake package cold load.");
			Error = ResourceReason;
			return false;
		}
		SampleMemory(TEXT("before_generated_package_cold_load"), true);
		if (MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			bColdLoadResourceGuard = true;
			ResourceReason = TEXT("System available physical RAM fell below 8 GiB before generated package cold load.");
			Error = ResourceReason;
			return false;
		}

		if (!UnloadPackageByExactName(bPackageUnloadedBeforeColdLoad, Error)) return false;
		bPackageColdLoadAttempted = true;
		const double PackageLoadStartedAt = FPlatformTime::Seconds();
		UPackage* ColdPackage = LoadPackage(nullptr, *NewAssetPackageName, LOAD_None);
		PackageColdLoadSeconds = FPlatformTime::Seconds() - PackageLoadStartedAt;
		if (!ColdPackage)
		{
			Error = TEXT("LoadPackage could not deserialize the generated bake asset from disk.");
			return false;
		}
		const FString AssetName = FPackageName::GetShortName(NewAssetPackageName);
		UIMAcousticBakeAsset* ColdAsset = FindObject<UIMAcousticBakeAsset>(ColdPackage, *AssetName);
		if (!IsValid(ColdAsset) || ColdAsset->GetPathName() != NewAssetPath)
		{
			Error = TEXT("Cold-loaded package did not contain the expected generated UIMAcousticBakeAsset.");
			return false;
		}
		FString ValidationError;
		if (!ColdAsset->Validate(Source.WorldPackage, NewAssetSceneFingerprint, ValidationError)
			|| ColdAsset->MetadataJson != NewAssetMetadata
			|| ColdAsset->SceneData.Num() != NewPackedSceneBytes
			|| ColdAsset->ProbeData.Num() != NewPackedProbeBytes
			|| IMG2BakeScaleHashByteArray(ColdAsset->SceneData) != NewAssetPackedSceneDigest
			|| IMG2BakeScaleHashByteArray(ColdAsset->ProbeData) != NewAssetPackedProbeDigest
			|| ColdAsset->PayloadDigest != NewAssetPackedPayloadDigest)
		{
			Error = TEXT("Cold-loaded UAsset content differs from the in-memory Bake output: ") + ValidationError;
			return false;
		}
		FString ColdFile;
		FString ColdFileSha1;
		int64 ColdFileBytes = 0;
		if (!IMG2BakeScaleHashFile(NewAssetPackageName, ColdFile, ColdFileSha1, ColdFileBytes, Error)
			|| !ColdFile.Equals(NewAssetPackageFile, ESearchCase::IgnoreCase)
			|| ColdFileSha1 != NewAssetPackageSha1 || ColdFileBytes != NewAssetPackageBytes)
		{
			if (Error.IsEmpty()) Error = TEXT("Generated UAsset package bytes changed during cold load.");
			return false;
		}
		TArray<FVector4> ProbePreview;
		FVector ProbeOriginCm;
		if (!ColdAsset->GetProbePreview(Source.WorldPackage, NewAssetSceneFingerprint,
				ProbePreview, ProbeOriginCm, ValidationError)
			|| ProbePreview.Num() != ProbeCount)
		{
			Error = TEXT("Cold-loaded bake asset probe preview differs from Bake output: ") + ValidationError;
			return false;
		}
		bPackageColdLoadSucceeded = true;
		SampleMemory(TEXT("generated_package_cold_loaded"), true);
		if (FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds
			|| MemorySamples.Last().AvailablePhysical < IMG2BakeScaleMinAvailableBytes)
		{
			bColdLoadResourceGuard = true;
			ResourceReason = FPlatformTime::Seconds() - StartedAt >= IMG2BakeScaleMaxSeconds
				? TEXT("The 900 second case guard expired before SDK cold load.")
				: TEXT("System available physical RAM fell below 8 GiB before SDK cold load.");
			Error = ResourceReason;
			return false;
		}

		TArray<uint8> RawScene;
		TArray<uint8> RawProbes;
		if (!ColdAsset->ReadPayload(Source.WorldPackage, NewAssetSceneFingerprint, RawScene, RawProbes, ValidationError))
		{
			Error = TEXT("Cold-loaded bake asset raw SDK payload read failed: ") + ValidationError;
			return false;
		}
		FIMAcousticBakeData Data;
		Data.Scene.assign(RawScene.GetData(), RawScene.GetData() + RawScene.Num());
		Data.ProbeBatch.assign(RawProbes.GetData(), RawProbes.GetData() + RawProbes.Num());
		Data.CoverageProbes.reserve(ProbePreview.Num());
		for (const FVector4& Probe : ProbePreview)
		{
			Data.CoverageProbes.push_back({{static_cast<float>(Probe.X), static_cast<float>(Probe.Y),
				static_cast<float>(Probe.Z)}, static_cast<float>(Probe.W)});
		}
		if (Data.Scene.empty() || Data.ProbeBatch.empty() || Data.CoverageProbes.empty())
		{
			Error = TEXT("Cold-loaded asset contains an empty SDK scene, probe batch, or coverage list.");
			return false;
		}

		FIMAcousticSimulation Simulation;
		std::string SdkError;
		const double PathingStartedAt = FPlatformTime::Seconds();
		if (!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), SdkError))
		{
			SdkPathingOptionsSeconds = FPlatformTime::Seconds() - PathingStartedAt;
			Error = FString::Printf(TEXT("SDK pathing setup failed for generated bake cold load: %s"),
				UTF8_TO_TCHAR(SdkError.c_str()));
			return false;
		}
		SdkPathingOptionsSeconds = FPlatformTime::Seconds() - PathingStartedAt;
		bSdkColdLoadAttempted = true;
		const double SdkLoadStartedAt = FPlatformTime::Seconds();
		const bool bLoaded = Simulation.Load(Data, 48000, 512, SdkError);
		SdkColdLoadSeconds = FPlatformTime::Seconds() - SdkLoadStartedAt;
		if (!bLoaded)
		{
			Error = FString::Printf(TEXT("IM_AcousticSimulation::Load failed for generated bake data: %s"),
				UTF8_TO_TCHAR(SdkError.c_str()));
			return false;
		}
		bSdkColdLoadSucceeded = true;
		SampleMemory(TEXT("generated_sdk_cold_load_complete"), true);
		return true;
	}

	bool DeleteGeneratedPackageFiles(FString& Error)
	{
		if (!bUniqueMeshScenario || NewAssetPackageName.IsEmpty()) return true;
		if (!bNewAssetCleanupCandidate || !bNewAssetCreatedThisRun || NewAssetPackageFiles.IsEmpty())
		{
			Error = TEXT("A generated bake path was recorded, but it did not pass the exact run-GUID/pre-run-manifest safety checks; package was preserved.");
			return false;
		}
		if (!bOriginalMapRestoredBeforeCleanup || !bOriginalAssetBytesRestoredBeforeCleanup)
		{
			Error = TEXT("Original W2 map and bound bake bytes were not verified before cleanup; generated package was preserved.");
			return false;
		}

		for (FIMG2BakeScalePackageFileRecord& Record : NewAssetPackageFiles)
		{
			FString CurrentSha1;
			int64 CurrentBytes = 0;
			FString ReadError;
			if (!IMG2BakeScaleHashFilePath(Record.Filename, CurrentSha1, CurrentBytes, ReadError)
				|| CurrentSha1 != Record.Sha1 || CurrentBytes != Record.Bytes)
			{
				Error = ReadError.IsEmpty()
					? TEXT("Generated package file changed after Bake; cleanup refused: ") + Record.Filename
					: ReadError;
				return false;
			}
			if (IFileManager::Get().IsReadOnly(*Record.Filename))
			{
				Error = TEXT("Generated package file is read-only; cleanup refused without a prompt: ") + Record.Filename;
				return false;
			}
		}

		UPackage* Package = FindPackage(nullptr, *NewAssetPackageName);
		if (!Package)
		{
			Error = TEXT("Generated package is not loaded for external-reference verification; cleanup refused.");
			return false;
		}
		if (!HasExternalPackageReferencers(Package, Error)) return false;
		FAssetRegistryModule::PackageDeleted(Package);
		TWeakObjectPtr<UPackage> WeakPackage(Package);
		bool bUnloaded = false;
		if (!UnloadPackageByExactName(bUnloaded, Error) || !bUnloaded)
		{
			if (UIMAcousticBakeAsset* StillLoadedAsset = WeakPackage.IsValid()
				? FindObject<UIMAcousticBakeAsset>(WeakPackage.Get(), *FPackageName::GetShortName(NewAssetPackageName))
				: nullptr)
			{
				FAssetRegistryModule::AssetCreated(StillLoadedAsset);
			}
			return false;
		}
		bPackageUnloadedBeforeDelete = true;

		for (FIMG2BakeScalePackageFileRecord& Record : NewAssetPackageFiles)
		{
			if (!IFileManager::Get().Delete(*Record.Filename, true, false, true))
			{
				Error = TEXT("Exact generated package file deletion failed; path retained for root recovery: ") + Record.Filename;
				return false;
			}
			Record.bRemoved = IFileManager::Get().FileSize(*Record.Filename) == INDEX_NONE;
			if (!Record.bRemoved)
			{
				Error = TEXT("Generated package file still exists after exact deletion: ") + Record.Filename;
				return false;
			}
		}
		bNewAssetFilesAbsentAfterCleanup = true;
		for (const FIMG2BakeScalePackageFileRecord& Record : NewAssetPackageFiles)
			bNewAssetFilesAbsentAfterCleanup &= Record.bRemoved;
		bNewAssetDeleted = bNewAssetFilesAbsentAfterCleanup;
		if (!bNewAssetFilesAbsentAfterCleanup)
		{
			if (Error.IsEmpty()) Error = TEXT("At least one exact generated package file remains after cleanup.");
			return false;
		}
		return true;
	}

	void SnapshotTickerProfile()
	{
		if (bEditorTickerMetricsCaptured) return;
		if (AIMAcousticBakeVolume* Volume = Source.Volume.Get())
		{
			int32 Ticks = 0;
			double CaptureMs = 0.0;
			double AssetValidationMs = 0.0;
			double MaxTickMs = 0.0;
			Volume->IM_GetEditorValidationProfileForTest(Ticks, CaptureMs, AssetValidationMs, MaxTickMs);
			if (Ticks > 0)
			{
				EditorTickerTicks = Ticks;
				EditorTickerCaptureMs = CaptureMs;
				EditorTickerAssetValidationMs = AssetValidationMs;
				EditorTickerMaxTickMs = MaxTickMs;
				EditorTickerSeconds = FPlatformTime::Seconds() - EditorTickerStartedAt;
				ObservedEditorTickerLogs = SnapshotEditorTickerLogLines().Num();
				bEditorTickerMetricsCaptured = true;
			}
		}
	}

	void RememberMemory(const FIMG2BakeScaleMemorySample& Sample)
	{
		MemorySamples.Add(Sample);
		LastMemorySampleAt = FPlatformTime::Seconds();
		MinimumAvailablePhysical = FMath::Min(MinimumAvailablePhysical, Sample.AvailablePhysical);
		ProcessPeakPhysical = FMath::Max(ProcessPeakPhysical, Sample.PeakUsedPhysical);
		ProcessPeakVirtual = FMath::Max(ProcessPeakVirtual, Sample.PeakUsedVirtual);
	}

	void RecomputeMemoryAggregates()
	{
		MinimumAvailablePhysical = MAX_uint64;
		ProcessPeakPhysical = 0;
		ProcessPeakVirtual = 0;
		for (const FIMG2BakeScaleMemorySample& Sample : MemorySamples)
		{
			MinimumAvailablePhysical = FMath::Min(MinimumAvailablePhysical, Sample.AvailablePhysical);
			ProcessPeakPhysical = FMath::Max(ProcessPeakPhysical, Sample.PeakUsedPhysical);
			ProcessPeakVirtual = FMath::Max(ProcessPeakVirtual, Sample.PeakUsedVirtual);
		}
		LastMemorySampleAt = FPlatformTime::Seconds();
	}

	void SampleMemory(const FString& StageName, const bool bForce)
	{
		const double Now = FPlatformTime::Seconds();
		if (!bForce && Now - LastMemorySampleAt < 1.0) return;
		RememberMemory(IMG2BakeScaleReadMemory(*StageName, StartedAt));
	}

	bool WriteReport(const TCHAR* FinalOutcome, const FString& FinalReason, const bool bMapRestored,
		const FString& FinalMapIdentity, const FString& FinalAssetIdentity,
		const FString& FinalAssetDiskDigest, const int64 FinalAssetDiskBytes)
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("AcousticV2/G2BakeScale"), RunId);
		ReportPath = FPaths::Combine(Directory, TEXT("report.json"));
		if (!IFileManager::Get().MakeDirectory(*Directory, true)) return false;

		uint64 BakeWindowSampledPeakWorkingSet = 0;
		uint64 BakeWindowSampledPeakCommit = 0;
		int32 BakeWindowSampleCount = 0;
		if (bBakeStarted && BakeSeconds >= 0.0)
		{
			const double BakeWindowStart = BakeStartedAt - StartedAt;
			const double BakeWindowEnd = BakeWindowStart + BakeSeconds;
			for (const FIMG2BakeScaleMemorySample& Sample : MemorySamples)
			{
				if (Sample.ElapsedSeconds + 1.e-6 < BakeWindowStart
					|| Sample.ElapsedSeconds - 1.e-6 > BakeWindowEnd) continue;
				++BakeWindowSampleCount;
				BakeWindowSampledPeakWorkingSet = FMath::Max(BakeWindowSampledPeakWorkingSet, Sample.UsedPhysical);
				BakeWindowSampledPeakCommit = FMath::Max(BakeWindowSampledPeakCommit, Sample.UsedVirtual);
			}
		}

		FString Json;
		auto Writer = TJsonWriterFactory<>::Create(&Json);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("schema"), bInstancedMeshScenario
			? TEXT("im_acoustic_g2_bake_scale_v3")
			: (bUniqueMeshScenario ? TEXT("im_acoustic_g2_bake_scale_v2")
				: TEXT("im_acoustic_g2_bake_scale_v1")));
		Writer->WriteValue(TEXT("run_id"), RunId);
		Writer->WriteValue(TEXT("automation_test"), IMG2BakeScaleAutomationName(
			TargetPlacements, bUniqueMeshScenario, Representation));
		Writer->WriteValue(TEXT("outcome"), IMG2BakeScaleOutcomeName(FinalOutcome));
		Writer->WriteValue(TEXT("reason"), FinalReason);
		Writer->WriteValue(TEXT("map"), IMG2BakeScaleMap);
		Writer->WriteValue(TEXT("map_identity_before"), Source.WorldIdentity);
		Writer->WriteValue(TEXT("map_identity_after_reload"), FinalMapIdentity);
		Writer->WriteValue(TEXT("map_identity_restored"), bMapRestored);
		Writer->WriteValue(TEXT("original_asset_path"), Source.OriginalAssetPath);
		Writer->WriteValue(TEXT("original_asset_identity_before"), Source.AssetIdentity);
		Writer->WriteValue(TEXT("original_asset_identity_after_reload"), FinalAssetIdentity);
		Writer->WriteValue(TEXT("original_asset_unchanged"),
			!Source.AssetIdentity.IsEmpty() && Source.AssetIdentity == FinalAssetIdentity
				&& !Source.OriginalAssetDiskDigest.IsEmpty()
				&& Source.OriginalAssetDiskDigest == FinalAssetDiskDigest
				&& Source.OriginalAssetDiskBytes == FinalAssetDiskBytes);
		Writer->WriteValue(TEXT("asset_identity_storage_basis"), TEXT("Serialized SceneData and ProbeData plus metadata; Format 2 stores raw sections and Format 3 stores packed sections."));
		Writer->WriteValue(TEXT("original_asset_package_file"), Source.OriginalAssetPackageFile);
		Writer->WriteValue(TEXT("original_asset_package_bytes_before"), static_cast<double>(Source.OriginalAssetDiskBytes));
		Writer->WriteValue(TEXT("original_asset_package_bytes_after"), static_cast<double>(FinalAssetDiskBytes));
		Writer->WriteValue(TEXT("original_asset_package_sha1_before"), Source.OriginalAssetDiskDigest);
		Writer->WriteValue(TEXT("original_asset_package_sha1_after"), FinalAssetDiskDigest);
		Writer->WriteValue(TEXT("new_asset_path"), NewAssetPath);
		Writer->WriteValue(TEXT("new_asset_package_file"), NewAssetPackageFile);
		Writer->WriteValue(TEXT("new_asset_package_bytes"), static_cast<double>(NewAssetPackageBytes));
		Writer->WriteValue(TEXT("new_asset_package_sha1_before_cleanup"), NewAssetPackageSha1);
		Writer->WriteValue(TEXT("new_asset_in_isolated_project"), bNewAssetInIsolatedProject);
		Writer->WriteValue(TEXT("new_asset_created_by_this_run"), bNewAssetCreatedThisRun);
		Writer->WriteValue(TEXT("new_asset_packed_scene_bytes"), static_cast<double>(NewPackedSceneBytes));
		Writer->WriteValue(TEXT("new_asset_packed_probe_bytes"), static_cast<double>(NewPackedProbeBytes));
		Writer->WriteValue(TEXT("new_asset_packed_payload_bytes"), static_cast<double>(NewPackedSceneBytes + NewPackedProbeBytes));
		Writer->WriteValue(TEXT("new_asset_packed_scene_sha1"), NewAssetPackedSceneDigest);
		Writer->WriteValue(TEXT("new_asset_packed_probe_sha1"), NewAssetPackedProbeDigest);
		Writer->WriteValue(TEXT("new_asset_packed_payload_digest"), NewAssetPackedPayloadDigest);
		Writer->WriteValue(TEXT("requested_total_placements"), TargetPlacements);
		Writer->WriteValue(TEXT("original_whitebox_placements"), IMG2BakeScaleBaselinePlacements);
		Writer->WriteValue(TEXT("added_transient_placements"), BuiltPlacements);
		Writer->WriteValue(TEXT("total_scene_placements"), IMG2BakeScaleBaselinePlacements + BuiltPlacements);
		Writer->WriteValue(TEXT("per_instance_region_audit_performed"), bInstancedMeshScenario);
		Writer->WriteValue(TEXT("saved_whitebox_placements_inside_bake_region"), Source.WhiteboxPlacements);
		Writer->WriteValue(TEXT("transient_placements_inside_bake_region"), TransientPlacementsInRegion);
		Writer->WriteValue(TEXT("transient_placements_outside_bake_region"), TransientPlacementsOutsideRegion);
		Writer->WriteValue(TEXT("total_placements_inside_bake_region"), bInstancedMeshScenario
			? Source.WhiteboxPlacements + TransientPlacementsInRegion : INDEX_NONE);
		Writer->WriteValue(TEXT("total_placements_outside_bake_region"), bInstancedMeshScenario
			? TransientPlacementsOutsideRegion : INDEX_NONE);
		Writer->WriteValue(TEXT("representation_kind"), IMG2BakeScaleRepresentationName(Representation));
		Writer->WriteValue(TEXT("baseline_enumerated_primitive_component_count"), BaselineEnumeratedPrimitiveComponentCount);
		Writer->WriteValue(TEXT("transient_mesh_component_count"), TransientMeshComponentCount);
		Writer->WriteValue(TEXT("transient_instanced_instance_count"), TransientMeshInstanceCount);
		Writer->WriteValue(TEXT("one_transient_mesh_identity_per_component"), bInstancedMeshScenario
			&& TransientMeshComponentCount == UniqueMeshes.Num());
		Writer->WriteValue(TEXT("fixture_primitive_component_count"),
			IMG2BakeScaleBaselinePlacements + TransientMeshComponentCount);
		Writer->WriteValue(TEXT("expected_capture_phase_component_count"), ExpectedCapturePhaseComponentCount);
		Writer->WriteValue(TEXT("capture_phase_component_count"), CapturePhaseComponentCount);
		Writer->WriteValue(TEXT("capture_phase_triangle_count"), CapturePhaseTriangleCount);
		Writer->WriteValue(TEXT("capture_phase_log_records_observed"), CapturePhaseLogRecords);
		Writer->WriteValue(TEXT("unique_mesh_assets_in_scene"), bUniqueMeshScenario ? UniqueMeshCount + 1 : 1);
		Writer->WriteValue(TEXT("saved_content_mesh_asset_identity_count_in_scene"), 1);
		Writer->WriteValue(TEXT("transient_mesh_uobject_identity_count"), UniqueMeshes.Num());
		Writer->WriteValue(TEXT("original_source_mesh_identities_in_scene"), 1);
		Writer->WriteValue(TEXT("transient_mesh_identities_in_scene"), bUniqueMeshScenario ? UniqueMeshCount : 0);
		Writer->WriteValue(TEXT("transient_mesh_identity_count"), UniqueMeshes.Num());
		Writer->WriteValue(TEXT("in_scene_distinct_mesh_identity_count"), bUniqueMeshScenario ? UniqueMeshCount + 1 : 1);
		Writer->WriteValue(TEXT("unique_mesh_identity_count_in_scene"), bUniqueMeshScenario ? UniqueMeshCount + 1 : 1);
		Writer->WriteValue(TEXT("transient_mesh_placement_frequency_3"), UniqueMeshesPlacedThreeTimes);
		Writer->WriteValue(TEXT("transient_mesh_placement_frequency_2"), UniqueMeshesPlacedTwice);
		Writer->WriteValue(TEXT("transient_mesh_identity_semantic_lod0_triangle_material_sha1"), UniqueMeshSemanticDigest);
		Writer->WriteValue(TEXT("transient_mesh_lod0_triangle_count"), UniqueMeshTriangleCount);
		Writer->WriteValue(TEXT("transient_mesh_build_seconds"), UniqueMeshBuildSeconds);
		Writer->WriteValue(TEXT("transient_meshes_not_persisted_as_content_assets"), bUniqueMeshScenario);
		Writer->WriteValue(TEXT("pre_run_bakes_directory_file_count"), PreRunBakeFileCount);
		Writer->WriteValue(TEXT("unique_asset_package_cold_load_attempted"), bPackageColdLoadAttempted);
		Writer->WriteValue(TEXT("unique_asset_package_cold_load_succeeded"), bPackageColdLoadSucceeded);
		Writer->WriteValue(TEXT("unique_asset_package_cold_load_seconds"), PackageColdLoadSeconds);
		Writer->WriteValue(TEXT("unique_asset_sdk_cold_load_attempted"), bSdkColdLoadAttempted);
		Writer->WriteValue(TEXT("unique_asset_sdk_cold_load_succeeded"), bSdkColdLoadSucceeded);
		Writer->WriteValue(TEXT("unique_asset_sdk_cold_load_seconds"), SdkColdLoadSeconds);
		Writer->WriteValue(TEXT("unique_asset_sdk_pathing_options_seconds"), SdkPathingOptionsSeconds);
		Writer->WriteValue(TEXT("package_unloaded_before_cold_load"), bPackageUnloadedBeforeColdLoad);
		Writer->WriteValue(TEXT("package_unloaded_before_delete"), bPackageUnloadedBeforeDelete);
		Writer->WriteValue(TEXT("new_asset_deleted"), bNewAssetDeleted);
		Writer->WriteValue(TEXT("new_asset_files_absent_after_cleanup"), bNewAssetFilesAbsentAfterCleanup);
		Writer->WriteValue(TEXT("original_map_restored_before_cleanup"), bOriginalMapRestoredBeforeCleanup);
		Writer->WriteValue(TEXT("original_asset_bytes_restored_before_cleanup"), bOriginalAssetBytesRestoredBeforeCleanup);
		Writer->WriteValue(TEXT("unique_asset_cleanup_seconds"), CleanupSeconds);
		Writer->WriteValue(TEXT("unique_asset_cold_load_failure"), ColdLoadFailure);
		Writer->WriteValue(TEXT("unique_asset_cleanup_failure"), CleanupFailure);
		Writer->WriteValue(TEXT("unique_asset_package_files_before_delete"), NewAssetPackageFiles.Num());
		Writer->WriteArrayStart(TEXT("unique_asset_package_files"));
		for (const FIMG2BakeScalePackageFileRecord& Record : NewAssetPackageFiles)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("file"), Record.Filename);
			Writer->WriteValue(TEXT("bytes_before_delete"), static_cast<double>(Record.Bytes));
			Writer->WriteValue(TEXT("sha1_before_delete"), Record.Sha1);
			Writer->WriteValue(TEXT("removed"), Record.bRemoved);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteValue(TEXT("representation"), bInstancedMeshScenario
			? (Representation == EIMG2BakeScaleRepresentation::InstancedStaticMesh
				? TEXT("60 saved Cube placements plus 1000 transient Static ISM components, each assigned one CPU-readable transient Cube UStaticMesh identity and containing 2/3 world-space instances")
				: TEXT("60 saved Cube placements plus 1000 transient Static HISM components, each assigned one CPU-readable transient Cube UStaticMesh identity and containing 2/3 world-space instances"))
			: (bUniqueMeshScenario
				? TEXT("60 saved Cube placements plus transient actors assigned among 1000 CPU-readable transient Cube UStaticMesh identities")
				: TEXT("independent transient StaticMeshActor / Static UStaticMeshComponent")));
		Writer->WriteValue(TEXT("cube_mesh_path"), Source.MeshPath);
		Writer->WriteValue(TEXT("cube_material_path"), Source.MaterialPath);
		Writer->WriteValue(TEXT("cube_uniform_scale"), CubeScale);
		auto WriteVector = [&Writer](const TCHAR* Name, const FVector& Value)
		{
			Writer->WriteArrayStart(Name);
			Writer->WriteValue(Value.X); Writer->WriteValue(Value.Y); Writer->WriteValue(Value.Z);
			Writer->WriteArrayEnd();
		};
		WriteVector(TEXT("cube_size_cm"), CubeSizeCm);
		WriteVector(TEXT("original_bake_bounds_origin_cm"), Source.BoundsOrigin);
		WriteVector(TEXT("original_bake_bounds_extent_cm"), Source.BoundsExtent);
		Writer->WriteValue(TEXT("grid_columns"), GridColumns);
		Writer->WriteValue(TEXT("grid_rows"), GridRows);
		Writer->WriteValue(TEXT("grid_cell_width_cm"), GridCellWidthCm);
		Writer->WriteValue(TEXT("grid_cell_height_cm"), GridCellHeightCm);
		Writer->WriteValue(TEXT("grid_layout"), TEXT("deterministic row-major prefix on the original BakeBounds XY plane; all case transforms and Cube size derive from the 3000 case grid"));
		Writer->WriteValue(TEXT("probe_spacing_cm"), Source.ProbeSpacingCm);
		Writer->WriteValue(TEXT("probe_height_cm"), Source.ProbeHeightCm);
		Writer->WriteValue(TEXT("probe_count"), ProbeCount);
		Writer->WriteValue(TEXT("expected_triangles"), ExpectedTriangles);
		Writer->WriteValue(TEXT("actual_exported_triangles"), ActualTriangles);
		Writer->WriteValue(TEXT("generated_probes_seconds"), GenerateProbesSeconds);
		Writer->WriteValue(TEXT("bake_async_seconds"), BakeSeconds);
		Writer->WriteValue(TEXT("validate_current_bake_seconds"), ValidateSeconds);
		Writer->WriteValue(TEXT("post_ticker_validate_seconds"), PostTickerValidateSeconds);
		Writer->WriteValue(TEXT("transient_placement_build_seconds"), BuildSeconds);
		Writer->WriteValue(TEXT("bake_started"), bBakeStarted);
		Writer->WriteValue(TEXT("editor_ticker_wait_seconds"), EditorTickerSeconds);
		Writer->WriteValue(TEXT("editor_ticker_observed_ticks"), EditorTickerTicks);
		Writer->WriteValue(TEXT("editor_ticker_observed_log_lines"), ObservedEditorTickerLogs);
		Writer->WriteValue(TEXT("editor_ticker_log_lines_valid"), bEditorTickerLogsValid);
		Writer->WriteValue(TEXT("editor_ticker_capture_ms_sum"), EditorTickerCaptureMs);
		Writer->WriteValue(TEXT("editor_ticker_asset_validation_ms_sum"), EditorTickerAssetValidationMs);
		Writer->WriteValue(TEXT("editor_ticker_max_tick_ms"), EditorTickerMaxTickMs);
		Writer->WriteObjectStart(TEXT("editor_ticker_invalidation"));
		Writer->WriteValue(TEXT("required_for_this_case"), bInstancedMeshScenario);
		Writer->WriteValue(TEXT("complete"), bInstancedMeshScenario && bEditorTickerInvalidationComplete);
		Writer->WriteValue(TEXT("log_validation_complete"), bEditorTickerLogValidationComplete);
		Writer->WriteValue(TEXT("final_validate_current_bake_passed"), bTickerFinalValidatePassed);
		Writer->WriteValue(TEXT("bound_asset_path_before"), TickerBoundAssetPath);
		Writer->WriteValue(TEXT("bound_asset_identity_before"), TickerBoundAssetIdentityBefore);
		Writer->WriteValue(TEXT("bound_asset_identity_after"), TickerBoundAssetIdentityAfter);
		Writer->WriteValue(TEXT("bound_asset_identity_preserved"), bTickerBoundAssetIdentityPreserved);
		Writer->WriteValue(TEXT("baked_packed_payload_digest"), NewAssetPackedPayloadDigest);
		Writer->WriteValue(TEXT("instance_component_path"), TickerInstanceComponentPath);
		Writer->WriteValue(TEXT("instance_index"), TickerInstanceIndex);
		Writer->WriteValue(TEXT("instance_interior_margin_cm"), TickerInstanceSafetyMarginCm);
		Writer->WriteValue(TEXT("instance_edit_was_only_x_plus_1cm"), bTickerInstanceEditWasOnlyXPlusOneCm);
		Writer->WriteValue(TEXT("instance_edit_applied"), bTickerInstanceEditApplied);
		Writer->WriteValue(TEXT("instance_transform_restored_exactly"), bTickerInstanceTransformRestored);
		Writer->WriteValue(TEXT("instance_restore_ticker_observed"), bTickerInstanceRestoreTickerObserved);
		auto WriteTransform = [&Writer, &WriteVector](const TCHAR* Name, const FTransform& Transform)
		{
			Writer->WriteObjectStart(Name);
			WriteVector(TEXT("translation_cm"), Transform.GetLocation());
			const FQuat Rotation = Transform.GetRotation();
			Writer->WriteArrayStart(TEXT("rotation_xyzw"));
			Writer->WriteValue(Rotation.X); Writer->WriteValue(Rotation.Y);
			Writer->WriteValue(Rotation.Z); Writer->WriteValue(Rotation.W);
			Writer->WriteArrayEnd();
			WriteVector(TEXT("scale"), Transform.GetScale3D());
			Writer->WriteObjectEnd();
		};
		WriteTransform(TEXT("instance_original_world_transform"), TickerInstanceOriginalTransform);
		WriteTransform(TEXT("instance_edited_world_transform"), TickerInstanceEditedTransform);
		Writer->WriteArrayStart(TEXT("tick_observations"));
		for (const FIMG2BakeScaleTickerObservation& Observation : EditorTickerObservations)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("tick_seq"), Observation.Sequence);
			Writer->WriteValue(TEXT("phase"), Observation.Phase);
			Writer->WriteValue(TEXT("status"), Observation.Status);
			Writer->WriteValue(TEXT("elapsed_seconds"), Observation.ElapsedSeconds);
			Writer->WriteValue(TEXT("phase_elapsed_seconds"), Observation.PhaseElapsedSeconds);
			Writer->WriteValue(TEXT("capture_ms"), Observation.CaptureMs);
			Writer->WriteValue(TEXT("asset_validation_ms"), Observation.AssetValidationMs);
			Writer->WriteValue(TEXT("full_tick_ms"), Observation.FullTickMs);
			Writer->WriteValue(TEXT("expected_captured"), Observation.ExpectedCaptured);
			Writer->WriteValue(TEXT("observed_captured"), Observation.ObservedCaptured);
			Writer->WriteValue(TEXT("expected_bound_asset_valid"), Observation.ExpectedBoundAssetValid);
			Writer->WriteValue(TEXT("observed_bound_asset_valid"), Observation.ObservedBoundAssetValid);
			Writer->WriteValue(TEXT("expected_triangles"), Observation.ExpectedTriangles);
			Writer->WriteValue(TEXT("observed_triangles"), Observation.ObservedTriangles);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
		Writer->WriteValue(TEXT("total_case_elapsed_seconds"), FPlatformTime::Seconds() - StartedAt);
		Writer->WriteValue(TEXT("product_sdk_recipe_metadata_matches_original"), bMetadataRecipeMatches);
		Writer->WriteValue(TEXT("volume_recipe_identity_matches_original"), bObservedVolumeRecipeUnchanged);
		Writer->WriteValue(TEXT("memory_semantics"), TEXT("FPlatformMemoryStats UsedPhysical/UsedVirtual are current process working-set/commit snapshots; PeakUsedPhysical/PeakUsedVirtual are cumulative Editor-process lifetime peaks that may predate this case; AvailablePhysical is system-available RAM."));
		Writer->WriteValue(TEXT("minimum_available_system_physical_bytes"),
			MinimumAvailablePhysical == MAX_uint64 ? 0.0 : static_cast<double>(MinimumAvailablePhysical));
		Writer->WriteValue(TEXT("process_peak_working_set_bytes_observed"), static_cast<double>(ProcessPeakPhysical));
		Writer->WriteValue(TEXT("process_peak_commit_bytes_observed"), static_cast<double>(ProcessPeakVirtual));
		Writer->WriteValue(TEXT("editor_process_lifetime_peak_working_set_bytes_observed"), static_cast<double>(ProcessPeakPhysical));
		Writer->WriteValue(TEXT("editor_process_lifetime_peak_commit_bytes_observed"), static_cast<double>(ProcessPeakVirtual));
		Writer->WriteValue(TEXT("bake_window_start_elapsed_seconds"), bBakeStarted
			? BakeStartedAt - StartedAt : -1.0);
		Writer->WriteValue(TEXT("bake_window_end_elapsed_seconds"), bBakeStarted
			? BakeStartedAt - StartedAt + BakeSeconds : -1.0);
		Writer->WriteValue(TEXT("bake_window_sampled_peak_working_set_bytes"), static_cast<double>(BakeWindowSampledPeakWorkingSet));
		Writer->WriteValue(TEXT("bake_window_sampled_peak_commit_bytes"), static_cast<double>(BakeWindowSampledPeakCommit));
		Writer->WriteValue(TEXT("bake_window_sample_count"), BakeWindowSampleCount);
		Writer->WriteValue(TEXT("bake_window_memory_scope"), TEXT("Maximum of current UsedPhysical/UsedVirtual snapshots whose elapsed timestamps fall between Bake start and Bake completion or cancellation; sampled values, not a continuous or Bake-exclusive process peak."));
		Writer->WriteValue(TEXT("sampled_process_peaks_are_lower_bounds"), true);
		Writer->WriteArrayStart(TEXT("memory_samples"));
		for (const FIMG2BakeScaleMemorySample& Sample : MemorySamples)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("stage"), Sample.Stage);
			Writer->WriteValue(TEXT("elapsed_seconds"), Sample.ElapsedSeconds);
			Writer->WriteValue(TEXT("used_physical_bytes"), static_cast<double>(Sample.UsedPhysical));
			Writer->WriteValue(TEXT("peak_used_physical_bytes"), static_cast<double>(Sample.PeakUsedPhysical));
			Writer->WriteValue(TEXT("used_virtual_bytes"), static_cast<double>(Sample.UsedVirtual));
			Writer->WriteValue(TEXT("peak_used_virtual_bytes"), static_cast<double>(Sample.PeakUsedVirtual));
			Writer->WriteValue(TEXT("available_system_physical_bytes"), static_cast<double>(Sample.AvailablePhysical));
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteArrayStart(TEXT("product_sdk_stage_logs"));
		if (LogCapture.IsValid())
			for (const FString& Line : LogCapture->Snapshot()) Writer->WriteValue(Line);
		Writer->WriteArrayEnd();
		Writer->WriteArrayStart(TEXT("limits_and_scope"));
		Writer->WriteValue(TEXT("Stop scale growth when system available RAM is below 8 GiB; report INCONCLUSIVE."));
		Writer->WriteValue(TEXT("Request product cancellation when the single case reaches 900 seconds; report INCONCLUSIVE and wait for owned async work to finish."));
		Writer->WriteValue(TEXT("No target-count downshift or quality reduction is permitted."));
		Writer->WriteValue(TEXT("This workload is a synthetic dense field of repeated small Cube meshes; it does not predict a production level or thousands of unique meshes."));
		if (bUniqueMeshScenario)
		{
			Writer->WriteValue(TEXT("This case uses 1000 transient mesh UObject identities in one fixed 3000-placement scene; it does not create 1000 persistent content assets."));
			Writer->WriteValue(TEXT("Placement arithmetic is 60 saved Cube placements plus 2940 transient placements (940 identities used three times and 60 identities twice), for 3000 total placements."));
			Writer->WriteValue(TEXT("Generated bake package load and SDK simulation load are separate measurements; the exact GUID package and known package sidecars are removed only after byte/hash/reference checks."));
			Writer->WriteValue(TEXT("If package cleanup cannot prove this run created the GUID files or unload the package, the test fails and leaves the path for root recovery."));
		}
		if (bInstancedMeshScenario)
		{
			Writer->WriteValue(TEXT("Each transient unique mesh identity owns exactly one Static ISM or HISM component; that component stores exactly two or three verified world-space instance transforms."));
			Writer->WriteValue(TEXT("The 60 saved placements plus 2940 new instances are expected inside BakeBounds; per-instance transformed mesh bounds are counted with the same FBox intersection rule as product CaptureScene."));
			Writer->WriteValue(TEXT("The test requires a CaptureScene phase log with the exact dynamically measured primitive-component count and 144000 triangles before Bake."));
		}
		Writer->WriteValue(TEXT("GenerateProbes and Bake include CaptureScene phase logs; only summarized capture, core, SDK and Editor ticker lines are copied into this report."));
		Writer->WriteValue(TEXT("Bake writes the generated asset only in this isolated project copy. The map is reloaded without saving, restoring its original bake binding."));
		Writer->WriteValue(TEXT("The 3000 case is a separate automation and must be selected only after Root reviews the actual 500 result as resource-manageable."));
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
		Writer->Close();
		return FFileHelper::SaveStringToFile(Json, *ReportPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool Finish(const TCHAR* RequestedOutcome, const FString& Message)
	{
		if (bFinished) return true;
		bFinished = true;
		FString TickerRestoreFailure;
		if (AIMAcousticBakeVolume* Volume = Source.Volume.Get())
		{
			SnapshotTickerProfile();
			if (bTickerInstanceEditApplied && !bTickerInstanceTransformRestored
				&& !RestoreTickerInstanceTransform(TickerRestoreFailure))
			{
				bTickerInstanceTransformRestored = false;
			}
			if (Stage == 2 || EditorTickerTicks > 0)
				Volume->IM_SetEditorValidationProfilingForTest(false);
		}
		if (bLogCaptureAttached && GLog && LogCapture.IsValid())
		{
			GLog->RemoveOutputDevice(LogCapture.Get());
			bLogCaptureAttached = false;
		}

		const bool bReloaded = FEditorFileUtils::LoadMap(IMG2BakeScaleMap, false, true);
		bool bMapRestored = false;
		FString FinalMapIdentity;
		FString FinalAssetIdentity;
		FString FinalAssetDigest;
		FString FinalAssetFilename;
		int64 FinalAssetBytes = 0;
		FString RestoreError;
		if (bReloaded && GEditor)
		{
			UWorld* RestoredWorld = GEditor->GetEditorWorldContext().World();
			FIMG2BakeScaleSource Restored;
			if (IMG2BakeScaleReadSource(RestoredWorld, Restored, RestoreError, false))
			{
				int32 RestoredVolumeCount = 0;
				FinalMapIdentity = IMG2BakeScaleWorldIdentity(RestoredWorld);
				FinalAssetIdentity = IMG2BakeScaleAssetIdentity(RestoredWorld, RestoredVolumeCount);
				const bool bAssetDiskReadable = Restored.Volume.IsValid() && Restored.Volume->BakedField
					&& IMG2BakeScaleHashFile(Restored.Volume->BakedField->GetOutermost()->GetName(),
						FinalAssetFilename, FinalAssetDigest, FinalAssetBytes, RestoreError);
				bOriginalMapRestoredBeforeCleanup = bBaselineCaptured && RestoredVolumeCount == 1
					&& FinalMapIdentity == Source.WorldIdentity
					&& FinalAssetIdentity == Source.AssetIdentity
					&& Restored.OriginalAssetPath == Source.OriginalAssetPath
					&& Restored.MeshPath == Source.MeshPath && Restored.MaterialPath == Source.MaterialPath
					&& Restored.WorldPackage == IMG2BakeScaleMap;
				bOriginalAssetBytesRestoredBeforeCleanup = bAssetDiskReadable
					&& FinalAssetDigest == Source.OriginalAssetDiskDigest
					&& FinalAssetBytes == Source.OriginalAssetDiskBytes;
				bMapRestored = bOriginalMapRestoredBeforeCleanup && bOriginalAssetBytesRestoredBeforeCleanup;
			}
		}
		SampleMemory(TEXT("after_final_map_reload"), true);

		if (bUniqueMeshScenario && !NewAssetPackageName.IsEmpty())
		{
			if (!bOriginalMapRestoredBeforeCleanup || !bOriginalAssetBytesRestoredBeforeCleanup)
			{
				CleanupFailure = TEXT("Original W2 map or bound asset did not restore before generated-package cleanup; the package was preserved.");
			}
			else
			{
				const double CleanupStartedAt = FPlatformTime::Seconds();
				if (bNewAssetCleanupCandidate)
				{
					FString ColdLoadError;
					if (!ColdLoadGeneratedPackage(ColdLoadError)) ColdLoadFailure = ColdLoadError;
					FString DeleteError;
					if (!DeleteGeneratedPackageFiles(DeleteError)) CleanupFailure = DeleteError;
				}
				else
				{
					CleanupFailure = TEXT("A generated asset package path was recorded without passing exact GUID and pre-run file-manifest checks; it was preserved: ")
						+ NewAssetPackageFile;
				}
				CleanupSeconds = FPlatformTime::Seconds() - CleanupStartedAt;
			}
		}

		if (bUniqueMeshScenario && bReloaded && GEditor)
		{
			UWorld* FinalWorld = GEditor->GetEditorWorldContext().World();
			FIMG2BakeScaleSource FinalSource;
			FString FinalVerifyError;
			if (IMG2BakeScaleReadSource(FinalWorld, FinalSource, FinalVerifyError, false))
			{
				int32 FinalVolumeCount = 0;
				FinalMapIdentity = IMG2BakeScaleWorldIdentity(FinalWorld);
				FinalAssetIdentity = IMG2BakeScaleAssetIdentity(FinalWorld, FinalVolumeCount);
				const bool bAssetDiskReadable = FinalSource.Volume.IsValid() && FinalSource.Volume->BakedField
					&& IMG2BakeScaleHashFile(FinalSource.Volume->BakedField->GetOutermost()->GetName(),
						FinalAssetFilename, FinalAssetDigest, FinalAssetBytes, FinalVerifyError);
				bMapRestored = bOriginalMapRestoredBeforeCleanup && FinalVolumeCount == 1
					&& FinalMapIdentity == Source.WorldIdentity
					&& FinalAssetIdentity == Source.AssetIdentity
					&& FinalSource.OriginalAssetPath == Source.OriginalAssetPath
					&& FinalSource.MeshPath == Source.MeshPath && FinalSource.MaterialPath == Source.MaterialPath
					&& FinalSource.WorldPackage == IMG2BakeScaleMap
					&& bAssetDiskReadable && FinalAssetDigest == Source.OriginalAssetDiskDigest
					&& FinalAssetBytes == Source.OriginalAssetDiskBytes;
				if (!bMapRestored && RestoreError.IsEmpty())
					RestoreError = TEXT("Original map or bound asset changed during generated-package cleanup.");
			}
			else
			{
				bMapRestored = false;
				if (RestoreError.IsEmpty()) RestoreError = FinalVerifyError;
			}
		}

		const bool bReportReady = bMapRestored || !bBaselineCaptured;
		const TCHAR* FinalOutcome = RequestedOutcome;
		FString FinalReason = Message;
		if (!TickerRestoreFailure.IsEmpty())
		{
			FinalOutcome = TEXT("FAIL");
			FinalReason += TEXT("; emergency instance-transform restore failed: ") + TickerRestoreFailure;
		}
		if (bInstancedMeshScenario && FCString::Strcmp(RequestedOutcome, TEXT("PASS")) == 0
			&& (!bEditorTickerInvalidationComplete || !bTickerInstanceRestoreTickerObserved
				|| !bEditorTickerLogValidationComplete || !bTickerBoundAssetIdentityPreserved))
		{
			FinalOutcome = TEXT("FAIL");
			FinalReason += TEXT("; instance invalidation ticker evidence did not satisfy every required recovery gate.");
		}
		if (!ColdLoadFailure.IsEmpty()) FinalReason += TEXT("; generated package cold load: ") + ColdLoadFailure;
		if (!CleanupFailure.IsEmpty()) FinalReason += TEXT("; generated package cleanup: ") + CleanupFailure;
		if (!bReportReady)
		{
			FinalOutcome = TEXT("FAIL");
			if (!RestoreError.IsEmpty()) FinalReason += TEXT("; final map/asset restoration check: ") + RestoreError;
			else FinalReason += TEXT("; the source map identity or original asset package bytes did not restore exactly.");
		}
		if (bUniqueMeshScenario)
		{
			if (!CleanupFailure.IsEmpty()) FinalOutcome = TEXT("FAIL");
			else if (!NewAssetPackageName.IsEmpty() && !bNewAssetDeleted) FinalOutcome = TEXT("FAIL");
			else if (!ColdLoadFailure.IsEmpty()) FinalOutcome = bColdLoadResourceGuard
				? (FCString::Strcmp(RequestedOutcome, TEXT("FAIL")) == 0 ? TEXT("FAIL") : TEXT("INCONCLUSIVE"))
				: TEXT("FAIL");
		}
		const bool bReportSaved = WriteReport(FinalOutcome, FinalReason, bMapRestored,
			FinalMapIdentity, FinalAssetIdentity, FinalAssetDigest, FinalAssetBytes);
		if (!bReportSaved)
		{
			FinalOutcome = TEXT("FAIL");
			FinalReason += TEXT("; G2 BakeScale JSON report could not be written.");
		}

		UE_LOG(LogTemp, Display,
			TEXT("IMLogs AcousticG2BakeScale run=%s target=%d placements=%d representation=%s mesh_components=%d instances=%d unique_mesh_identities=%d triangles=%d probes=%d result=%s restored=%d package_deleted=%d report_saved=%d report=%s reason=%s"),
			*RunId, TargetPlacements, BuiltPlacements + IMG2BakeScaleBaselinePlacements,
			IMG2BakeScaleRepresentationName(Representation), TransientMeshComponentCount,
			TransientMeshInstanceCount,
			bUniqueMeshScenario ? UniqueMeshCount + 1 : 1,
			ActualTriangles, ProbeCount, FinalOutcome, bMapRestored ? 1 : 0,
			bNewAssetDeleted ? 1 : 0, bReportSaved ? 1 : 0, *ReportPath, *FinalReason);
		if (FCString::Strcmp(FinalOutcome, TEXT("PASS")) == 0 && bReportSaved && bMapRestored)
		{
			Test->AddInfo(FString::Printf(TEXT("G2 BakeScale report: %s"), *ReportPath));
			const TCHAR* Marker = IMG2BakeScalePassMarker(bUniqueMeshScenario, Representation);
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] %s PASS"), Marker);
			UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS %s"), Marker);
		}
		else
		{
			Test->AddError(FinalReason);
		}
		return true;
	}

	FAutomationTestBase* Test = nullptr;
	const int32 TargetPlacements;
	const int32 UniqueMeshCount;
	const EIMG2BakeScaleRepresentation Representation;
	const bool bUniqueMeshScenario;
	const bool bInstancedMeshScenario;
	FString RunId;
	FString ReportPath;
	FIMG2BakeScaleSource Source;
	TUniquePtr<FIMG2BakeScaleLogCapture> LogCapture;
	TArray<FIMG2BakeScaleMemorySample> MemorySamples;
	TArray<FIMG2BakeScaleTickerObservation> EditorTickerObservations;
	TArray<TStrongObjectPtr<UStaticMesh>> UniqueMeshes;
	TArray<int32> UniqueMeshPlacementCounts;
	TWeakObjectPtr<UInstancedStaticMeshComponent> TickerInstanceComponent;
	TArray<FIMG2BakeScalePackageFileRecord> NewAssetPackageFiles;
	TSet<FString> PreRunBakeFiles;
	FString BakeDirectory;
	FString UniqueMeshSemanticDigest;
	FString ResourceReason;
	FString NewAssetPath;
	FString NewAssetPackageName;
	FString NewAssetPackageFile;
	FString NewAssetPackageSha1;
	FString NewAssetMetadata;
	FString NewAssetSceneFingerprint;
	FString NewAssetPackedSceneDigest;
	FString NewAssetPackedProbeDigest;
	FString NewAssetPackedPayloadDigest;
	FString TickerBoundAssetPath;
	FString TickerBoundAssetIdentityBefore;
	FString TickerBoundAssetIdentityAfter;
	FString TickerInstanceComponentPath;
	FString ColdLoadFailure;
	FString CleanupFailure;
	int64 NewAssetPackageBytes = -1;
	int64 NewPackedSceneBytes = 0;
	int64 NewPackedProbeBytes = 0;
	int32 PreRunBakeFileCount = 0;
	int32 UniqueMeshTriangleCount = 0;
	int32 UniqueMeshesPlacedThreeTimes = 0;
	int32 UniqueMeshesPlacedTwice = 0;
	uint64 MinimumAvailablePhysical = MAX_uint64;
	uint64 ProcessPeakPhysical = 0;
	uint64 ProcessPeakVirtual = 0;
	double StartedAt = 0.0;
	double LastMemorySampleAt = 0.0;
	double BuildSeconds = 0.0;
	double GenerateProbesSeconds = 0.0;
	double BakeStartedAt = 0.0;
	double BakeSeconds = 0.0;
	double ValidateSeconds = 0.0;
	double PostTickerValidateSeconds = 0.0;
	double UniqueMeshBuildSeconds = 0.0;
	double EditorTickerStartedAt = 0.0;
	double EditorTickerSeconds = 0.0;
	double PackageColdLoadSeconds = -1.0;
	double SdkColdLoadSeconds = -1.0;
	double SdkPathingOptionsSeconds = -1.0;
	double CleanupSeconds = 0.0;
	double EditorTickerCaptureMs = 0.0;
	double EditorTickerAssetValidationMs = 0.0;
	double EditorTickerMaxTickMs = 0.0;
	double LastObservedEditorTickerCaptureMs = 0.0;
	double LastObservedEditorTickerAssetValidationMs = 0.0;
	double EditorTickerPhaseStartedAt = 0.0;
	double EditorTickerLogDrainStartedAt = 0.0;
	double TickerInstanceSafetyMarginCm = 0.0;
	double GridCellWidthCm = 0.0;
	double GridCellHeightCm = 0.0;
	int32 BuiltPlacements = 0;
	int32 BaselineEnumeratedPrimitiveComponentCount = INDEX_NONE;
	int32 TransientMeshComponentCount = 0;
	int32 TransientMeshInstanceCount = 0;
	int32 TransientPlacementsInRegion = INDEX_NONE;
	int32 TransientPlacementsOutsideRegion = INDEX_NONE;
	int32 ExpectedCapturePhaseComponentCount = INDEX_NONE;
	int32 CapturePhaseComponentCount = INDEX_NONE;
	int32 CapturePhaseTriangleCount = INDEX_NONE;
	int32 CapturePhaseLogRecords = 0;
	int32 GridColumns = 0;
	int32 GridRows = 0;
	int32 ExpectedTriangles = 0;
	int32 ActualTriangles = 0;
	int32 ProbeCount = 0;
	int32 EditorTickerTicks = 0;
	int32 ObservedEditorTickerLogs = 0;
	int32 LastObservedEditorTickerTicks = 0;
	int32 EditorTickerPhaseTicks = 0;
	int32 TickerInstanceIndex = INDEX_NONE;
	float CubeScale = 0.0f;
	FVector CubeSizeCm = FVector::ZeroVector;
	FTransform TickerInstanceOriginalTransform = FTransform::Identity;
	FTransform TickerInstanceEditedTransform = FTransform::Identity;
	bool bBaselineCaptured = false;
	bool bLogCaptureAttached = false;
	bool bCancelRequested = false;
	bool bEditorTickerLogsValid = true;
	bool bEditorTickerMetricsCaptured = false;
	bool bEditorTickerLogValidationComplete = false;
	bool bEditorTickerInvalidationComplete = false;
	bool bWaitingForEditorTickerLogDrain = false;
	bool bTickerFinalValidatePassed = false;
	bool bTickerInstanceEditWasOnlyXPlusOneCm = false;
	bool bTickerInstanceEditApplied = false;
	bool bTickerInstanceTransformRestored = false;
	bool bTickerInstanceRestoreTickerObserved = false;
	bool bTickerBoundAssetIdentityPreserved = false;
	bool bMetadataRecipeMatches = false;
	bool bObservedVolumeRecipeUnchanged = true;
	bool bBakeStarted = false;
	bool bNewAssetInIsolatedProject = false;
	bool bNewAssetCreatedThisRun = false;
	bool bNewAssetCleanupCandidate = false;
	bool bPackageColdLoadAttempted = false;
	bool bPackageColdLoadSucceeded = false;
	bool bSdkColdLoadAttempted = false;
	bool bSdkColdLoadSucceeded = false;
	bool bColdLoadResourceGuard = false;
	bool bPackageUnloadedBeforeColdLoad = false;
	bool bPackageUnloadedBeforeDelete = false;
	bool bNewAssetDeleted = false;
	bool bNewAssetFilesAbsentAfterCleanup = false;
	bool bOriginalMapRestoredBeforeCleanup = false;
	bool bOriginalAssetBytesRestoredBeforeCleanup = false;
	bool bFinished = false;
	int32 Stage = 0;
	EIMG2BakeScaleTickerPhase TickerPhase = EIMG2BakeScaleTickerPhase::Baseline;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2BakeScale500Test,
	"IceMoon.AcousticField.G2.BakeScale500",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2BakeScale500Test::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2BakeScaleTestPrivate::FIMAcousticG2BakeScaleCommand(this, IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleCounts[0]));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2BakeScale3000Test,
	"IceMoon.AcousticField.G2.BakeScale3000",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2BakeScale3000Test::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2BakeScaleTestPrivate::FIMAcousticG2BakeScaleCommand(this, IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleCounts[1]));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2BakeScaleUnique1000Test,
	"IceMoon.AcousticField.G2.BakeScaleUnique1000",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2BakeScaleUnique1000Test::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2BakeScaleTestPrivate::FIMAcousticG2BakeScaleCommand(this,
		IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleCounts[1], IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleUniqueMeshCount));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2BakeScaleUnique1000ISMTest,
	"IceMoon.AcousticField.G2.BakeScaleISMUnique1000",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2BakeScaleUnique1000ISMTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2BakeScaleTestPrivate::FIMAcousticG2BakeScaleCommand(this,
		IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleCounts[1], IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleUniqueMeshCount,
		IMAcousticG2BakeScaleTestPrivate::EIMG2BakeScaleRepresentation::InstancedStaticMesh));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2BakeScaleUnique1000HISMTest,
	"IceMoon.AcousticField.G2.BakeScaleHISMUnique1000",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2BakeScaleUnique1000HISMTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2BakeScaleTestPrivate::FIMAcousticG2BakeScaleCommand(this,
		IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleCounts[1], IMAcousticG2BakeScaleTestPrivate::IMG2BakeScaleUniqueMeshCount,
		IMAcousticG2BakeScaleTestPrivate::EIMG2BakeScaleRepresentation::HierarchicalInstancedStaticMesh));
	return true;
}

namespace IMAcousticG2BakeScaleTestPrivate
{
struct FIMG2ProbeSpacingTickerObservation
{
	int32 Sequence = INDEX_NONE;
	FString Phase;
	FString Status;
	double CaptureMs = 0.0;
	double AssetValidationMs = 0.0;
	double FullTickMs = 0.0;
	double ElapsedSeconds = 0.0;
	double PhaseElapsedSeconds = 0.0;
	int32 ObservedCaptured = INDEX_NONE;
	int32 ObservedBoundAssetValid = INDEX_NONE;
	int32 ObservedTriangles = INDEX_NONE;
};

class FIMAcousticG2ProbeSpacingTickerCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticG2ProbeSpacingTickerCommand(FAutomationTestBase* InTest)
		: Test(InTest), RunId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
	{
		StartedAt = FPlatformTime::Seconds();
	}

	bool Update() override
	{
		if (bFinished) return true;
		switch (Stage)
		{
		case 0: return StartCase();
		case 1: return WaitForInvalidTicker();
		case 2: return WaitForRecoveredTicker();
		case 3: return ValidateRecoveredBake();
		case 4: return DrainTickerLogs();
		default: return Finish(false, TEXT("Internal G2 ProbeSpacingTicker stage was invalid."));
		}
	}

private:
	bool StartCase()
	{
		if (!FEditorFileUtils::LoadMap(IMG2BakeScaleMap, false, true))
			return Finish(false, TEXT("Could not load the saved W2 map for the ProbeSpacing ticker check."));
		bMapLoaded = true;
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FString Error;
		if (!IMG2BakeScaleReadSource(World, Source, Error, true))
			return Finish(false, TEXT("Saved W2 Bake precondition failed: ") + Error);
		bBaselineCaptured = true;
		Volume = Source.Volume;
		OriginalProbeSpacingCm = Volume->ProbeSpacingCm;
		MapPackageFile = FPackageName::LongPackageNameToFilename(
			Source.WorldPackage, FPackageName::GetMapPackageExtension());
		if (!IMG2BakeScaleHashFilePath(MapPackageFile, BaselineMapSha1, BaselineMapBytes, Error))
			return Finish(false, TEXT("Could not snapshot saved W2 map bytes: ") + Error);

		LogCapture = MakeUnique<FIMG2BakeScaleLogCapture>();
		if (!LogCapture.IsValid() || !GLog)
			return Finish(false, TEXT("Could not create the actual Editor ticker log observer."));
		GLog->AddOutputDevice(LogCapture.Get());
		bLogCaptureAttached = true;

		Volume->IM_SetEditorValidationProfilingForTest(true);
		bProfilingEnabled = true;
		LastProfileCaptureMs = 0.0;
		LastProfileAssetValidationMs = 0.0;
		PhaseStartedAt = FPlatformTime::Seconds();
		Volume->ProbeSpacingCm = 0.0f;
		bTemporarySpacingApplied = true;
		if (Volume->ProbeSpacingCm != 0.0f)
			return Finish(false, TEXT("ProbeSpacingCm did not retain the exact temporary zero value."));

		Stage = 1;
		return false;
	}

	bool WaitForInvalidTicker()
	{
		FIMG2ProbeSpacingTickerObservation Observation;
		if (!SnapshotTicker(1, TEXT("spacing_zero_capture_failure"), Observation))
		{
			if (FPlatformTime::Seconds() - PhaseStartedAt > 12.0)
				return Finish(false, TEXT("No actual Editor ticker observed ProbeSpacingCm=0 within 12 seconds."));
			return false;
		}
		Observations.Add(MoveTemp(Observation));
		const FString& Status = Observations.Last().Status;
		if (!Status.StartsWith(TEXT("Bake stale: scene export is incomplete"), ESearchCase::CaseSensitive))
			return Finish(false, TEXT("Zero-spacing ticker did not report the expected incomplete-export stale state: ") + Status);
		bInvalidTickerObserved = true;

		if (!Volume.IsValid())
			return Finish(false, TEXT("Bake volume disappeared before ProbeSpacingCm restoration."));
		Volume->ProbeSpacingCm = OriginalProbeSpacingCm;
		bTemporarySpacingApplied = false;
		bSpacingRestored = Volume->ProbeSpacingCm == OriginalProbeSpacingCm;
		if (!bSpacingRestored)
			return Finish(false, TEXT("ProbeSpacingCm did not restore to its exact original float value."));
		PhaseStartedAt = FPlatformTime::Seconds();
		Stage = 2;
		return false;
	}

	bool WaitForRecoveredTicker()
	{
		FIMG2ProbeSpacingTickerObservation Observation;
		if (!SnapshotTicker(2, TEXT("spacing_restored_capture_recovery"), Observation))
		{
			if (FPlatformTime::Seconds() - PhaseStartedAt > 12.0)
				return Finish(false, TEXT("No next actual Editor ticker observed the restored ProbeSpacingCm within 12 seconds."));
			return false;
		}
		Observations.Add(MoveTemp(Observation));
		if (!Observations.Last().Status.Equals(TEXT("Bake valid for current scene."), ESearchCase::CaseSensitive))
			return Finish(false, TEXT("Ticker did not clear the stale status after exact ProbeSpacingCm restoration: ")
				+ Observations.Last().Status);
		bRecoveredTickerObserved = true;
		Stage = 3;
		return false;
	}

	bool ValidateRecoveredBake()
	{
		AIMAcousticBakeVolume* CurrentVolume = Volume.Get();
		if (!IsValid(CurrentVolume))
			return Finish(false, TEXT("Bake volume disappeared before final ValidateCurrentBake."));
		if (CurrentVolume->ProbeSpacingCm != OriginalProbeSpacingCm)
			return Finish(false, TEXT("ProbeSpacingCm drifted after the recovery ticker."));
		const double ValidationStartedAt = FPlatformTime::Seconds();
		FString ValidationFailure;
		bFinalValidatePassed = CurrentVolume->ValidateCurrentBake(ValidationFailure);
		FinalValidateSeconds = FPlatformTime::Seconds() - ValidationStartedAt;
		if (!bFinalValidatePassed)
			return Finish(false, TEXT("Final ValidateCurrentBake failed after ticker recovery: ") + ValidationFailure);
		if (CurrentVolume->ExportedTriangles != IMG2BakeScaleBaselineTriangles)
			return Finish(false, FString::Printf(TEXT("Recovered Bake triangle count drifted: %d."),
				CurrentVolume->ExportedTriangles));

		int32 VolumeCount = 0;
		RecoveredAssetIdentity = IMG2BakeScaleAssetIdentity(CurrentVolume->GetWorld(), VolumeCount);
		RecoveredRecipeIdentity = IMG2BakeScaleVolumeRecipeIdentity(CurrentVolume);
		if (VolumeCount != 1 || !CurrentVolume->BakedField
			|| CurrentVolume->BakedField->GetPathName() != Source.OriginalAssetPath
			|| RecoveredAssetIdentity != Source.AssetIdentity
			|| RecoveredRecipeIdentity != Source.VolumeRecipeIdentity)
			return Finish(false, TEXT("Recovery changed the original bound Bake asset/payload identity or volume recipe."));

		if (bProfilingEnabled)
		{
			CurrentVolume->IM_SetEditorValidationProfilingForTest(false);
			bProfilingEnabled = false;
		}
		LogDrainStartedAt = FPlatformTime::Seconds();
		Stage = 4;
		return false;
	}

	bool DrainTickerLogs()
	{
		TArray<FString> TickerLines;
		if (LogCapture.IsValid())
		{
			for (const FString& Line : LogCapture->Snapshot())
				if (Line.Contains(TEXT("IMLogs AcousticG2EditorTicker"))) TickerLines.Add(Line);
		}
		TickerLogLines = TickerLines;
		if (TickerLines.Num() < 2)
		{
			if (FPlatformTime::Seconds() - LogDrainStartedAt > 5.0)
				return Finish(false, FString::Printf(TEXT("Expected two actual Editor ticker log records; observed %d."),
					TickerLines.Num()));
			return false;
		}
		if (TickerLines.Num() != 2)
			return Finish(false, FString::Printf(TEXT("Expected exactly two profiled Editor ticker records; observed %d."),
				TickerLines.Num()));

		const int32 ExpectedCaptured[] = {0, 1};
		const int32 ExpectedBoundValid[] = {0, 1};
		for (int32 Index = 0; Index < TickerLines.Num(); ++Index)
		{
			int32 Captured = INDEX_NONE;
			int32 BoundValid = INDEX_NONE;
			int32 Triangles = INDEX_NONE;
			if (!IMG2BakeScaleParseIntLogField(TickerLines[Index], TEXT("captured"), Captured)
				|| !IMG2BakeScaleParseIntLogField(TickerLines[Index], TEXT("bound_asset_valid"), BoundValid)
				|| !IMG2BakeScaleParseIntLogField(TickerLines[Index], TEXT("triangles"), Triangles))
				return Finish(false, FString::Printf(TEXT("Ticker log %d was missing captured/bound_asset_valid/triangles fields."), Index + 1));
			Observations[Index].ObservedCaptured = Captured;
			Observations[Index].ObservedBoundAssetValid = BoundValid;
			Observations[Index].ObservedTriangles = Triangles;
			if (Captured != ExpectedCaptured[Index] || BoundValid != ExpectedBoundValid[Index])
				return Finish(false, FString::Printf(TEXT("Ticker log %d state mismatch: captured=%d bound_asset_valid=%d."),
					Index + 1, Captured, BoundValid));
			if (Index == 1 && Triangles != IMG2BakeScaleBaselineTriangles)
				return Finish(false, FString::Printf(TEXT("Recovered ticker triangle count mismatch: %d."), Triangles));
		}
		return Finish(true, FString());
	}

	bool SnapshotTicker(const int32 ExpectedSequence, const TCHAR* Phase,
		FIMG2ProbeSpacingTickerObservation& OutObservation)
	{
		AIMAcousticBakeVolume* CurrentVolume = Volume.Get();
		if (!IsValid(CurrentVolume))
		{
			Finish(false, TEXT("Bake volume disappeared while waiting for the scheduled Editor ticker."));
			return false;
		}
		int32 Ticks = 0;
		double CaptureMs = 0.0;
		double AssetValidationMs = 0.0;
		double MaxTickMs = 0.0;
		// The getter returns false before the first scheduled tick; the counter
		// remains authoritative for this bounded wait.
		CurrentVolume->IM_GetEditorValidationProfileForTest(
			Ticks, CaptureMs, AssetValidationMs, MaxTickMs);
		if (Ticks < ExpectedSequence) return false;
		if (Ticks != ExpectedSequence)
		{
			Finish(false, FString::Printf(TEXT("Ticker sequence skipped expected phase %d; observed %d."),
				ExpectedSequence, Ticks));
			return false;
		}
		const double Now = FPlatformTime::Seconds();
		OutObservation.Sequence = Ticks;
		OutObservation.Phase = Phase;
		OutObservation.Status = CurrentVolume->Status;
		OutObservation.CaptureMs = CaptureMs - LastProfileCaptureMs;
		OutObservation.AssetValidationMs = AssetValidationMs - LastProfileAssetValidationMs;
		OutObservation.FullTickMs = OutObservation.CaptureMs + OutObservation.AssetValidationMs;
		OutObservation.ElapsedSeconds = Now - StartedAt;
		OutObservation.PhaseElapsedSeconds = Now - PhaseStartedAt;
		LastProfileCaptureMs = CaptureMs;
		LastProfileAssetValidationMs = AssetValidationMs;
		return true;
	}

	bool Finish(const bool bRequestedSuccess, const FString& Error)
	{
		if (bFinished) return true;
		bFinished = true;
		if (!Error.IsEmpty()) FailureReason = Error;

		if (Volume.IsValid())
		{
			if (bTemporarySpacingApplied)
			{
				Volume->ProbeSpacingCm = OriginalProbeSpacingCm;
				bTemporarySpacingApplied = false;
			}
			bSpacingRestored = bBaselineCaptured
				&& Volume->ProbeSpacingCm == OriginalProbeSpacingCm;
			if (bProfilingEnabled)
			{
				Volume->IM_SetEditorValidationProfilingForTest(false);
				bProfilingEnabled = false;
			}
		}
		if (LogCapture.IsValid())
		{
			TickerLogLines.Reset();
			for (const FString& Line : LogCapture->Snapshot())
				if (Line.Contains(TEXT("IMLogs AcousticG2EditorTicker"))) TickerLogLines.Add(Line);
		}
		if (bLogCaptureAttached && GLog && LogCapture.IsValid())
		{
			GLog->RemoveOutputDevice(LogCapture.Get());
			bLogCaptureAttached = false;
		}

		if (bMapLoaded)
		{
			bMapReloadedForCleanup = FEditorFileUtils::LoadMap(IMG2BakeScaleMap, false, true);
			if (bMapReloadedForCleanup && bBaselineCaptured)
			{
				UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				FIMG2BakeScaleSource Restored;
				FString RestoreError;
				if (IMG2BakeScaleReadSource(RestoredWorld, Restored, RestoreError, true))
				{
					int32 VolumeCount = 0;
					FinalWorldIdentity = IMG2BakeScaleWorldIdentity(RestoredWorld);
					FinalAssetIdentity = IMG2BakeScaleAssetIdentity(RestoredWorld, VolumeCount);
					FinalRecipeIdentity = Restored.VolumeRecipeIdentity;
					const bool bFinalMapDiskReadable = IMG2BakeScaleHashFilePath(
						MapPackageFile, FinalMapSha1, FinalMapBytes, RestoreError);
					const bool bFinalAssetDiskReadable = Restored.Volume.IsValid() && Restored.Volume->BakedField
						&& IMG2BakeScaleHashFile(Restored.Volume->BakedField->GetOutermost()->GetName(),
							FinalAssetPackageFile, FinalAssetSha1, FinalAssetBytes, RestoreError);
					bDiskBytesUnchanged = bFinalMapDiskReadable && bFinalAssetDiskReadable
						&& BaselineMapSha1 == FinalMapSha1 && BaselineMapBytes == FinalMapBytes
						&& Source.OriginalAssetDiskDigest == FinalAssetSha1
						&& Source.OriginalAssetDiskBytes == FinalAssetBytes;
					bFinalIdentityUnchanged = VolumeCount == 1
						&& FinalWorldIdentity == Source.WorldIdentity
						&& FinalAssetIdentity == Source.AssetIdentity
						&& FinalRecipeIdentity == Source.VolumeRecipeIdentity
						&& Restored.OriginalAssetPath == Source.OriginalAssetPath;
					if (!bDiskBytesUnchanged && FailureReason.IsEmpty())
						FailureReason = TEXT("Saved map or bound Bake asset package bytes changed during the test.");
					if (!bFinalIdentityUnchanged && FailureReason.IsEmpty())
						FailureReason = TEXT("Reloaded W2 world, recipe, or bound Bake asset/payload identity drifted.");
				}
				else if (FailureReason.IsEmpty())
				{
					FailureReason = TEXT("Could not revalidate the original W2 Bake after cleanup: ") + RestoreError;
				}
			}
			if (!bMapReloadedForCleanup && FailureReason.IsEmpty())
				FailureReason = TEXT("Could not reload the original W2 map after restoring ProbeSpacingCm.");
		}

		bPassed = bRequestedSuccess && FailureReason.IsEmpty() && bSpacingRestored
			&& bMapReloadedForCleanup && bDiskBytesUnchanged && bFinalIdentityUnchanged;
		if (!bPassed && FailureReason.IsEmpty())
			FailureReason = TEXT("ProbeSpacing ticker check did not satisfy every recovery and cleanup gate.");
		const bool bReportWritten = WriteReport();
		if (!bReportWritten)
		{
			bPassed = false;
			if (FailureReason.IsEmpty()) FailureReason = TEXT("Could not write the bounded ProbeSpacing ticker report.");
		}

		if (bPassed)
		{
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] G2ProbeSpacingTicker PASS report=%s"), *ReportPath);
			UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS G2ProbeSpacingTicker"));
			Test->AddInfo(FString::Printf(TEXT("G2 ProbeSpacing actual Editor ticker report: %s"), *ReportPath));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] G2ProbeSpacingTicker FAIL reason=%s report=%s"),
				*FailureReason, *ReportPath);
			Test->AddError(FailureReason);
		}
		return true;
	}

	bool WriteReport()
	{
		ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/G2ProbeSpacingTicker"), *RunId);
		IFileManager::Get().MakeDirectory(*ReportDirectory, true);
		ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
		FString Json;
		auto Writer = TJsonWriterFactory<>::Create(&Json);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("schema"), TEXT("im.acoustic.g2.probe-spacing-ticker.v1"));
		Writer->WriteValue(TEXT("run_id"), RunId);
		Writer->WriteValue(TEXT("outcome"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
		Writer->WriteValue(TEXT("failure_reason"), FailureReason);
		Writer->WriteValue(TEXT("map_package"), Source.WorldPackage);
		Writer->WriteValue(TEXT("map_package_file"), MapPackageFile);
		Writer->WriteValue(TEXT("baseline_world_identity"), Source.WorldIdentity);
		Writer->WriteValue(TEXT("final_world_identity"), FinalWorldIdentity);
		Writer->WriteValue(TEXT("baseline_volume_recipe_identity"), Source.VolumeRecipeIdentity);
		Writer->WriteValue(TEXT("recovered_volume_recipe_identity"), RecoveredRecipeIdentity);
		Writer->WriteValue(TEXT("final_volume_recipe_identity"), FinalRecipeIdentity);
		Writer->WriteValue(TEXT("baseline_asset_path"), Source.OriginalAssetPath);
		Writer->WriteValue(TEXT("baseline_asset_identity"), Source.AssetIdentity);
		Writer->WriteValue(TEXT("recovered_asset_identity"), RecoveredAssetIdentity);
		Writer->WriteValue(TEXT("final_asset_identity"), FinalAssetIdentity);
		Writer->WriteValue(TEXT("baseline_map_sha1"), BaselineMapSha1);
		Writer->WriteValue(TEXT("final_map_sha1"), FinalMapSha1);
		Writer->WriteValue(TEXT("baseline_map_bytes"), BaselineMapBytes);
		Writer->WriteValue(TEXT("final_map_bytes"), FinalMapBytes);
		Writer->WriteValue(TEXT("baseline_asset_sha1"), Source.OriginalAssetDiskDigest);
		Writer->WriteValue(TEXT("final_asset_sha1"), FinalAssetSha1);
		Writer->WriteValue(TEXT("baseline_asset_bytes"), Source.OriginalAssetDiskBytes);
		Writer->WriteValue(TEXT("final_asset_bytes"), FinalAssetBytes);
		Writer->WriteValue(TEXT("disk_bytes_unchanged"), bDiskBytesUnchanged);
		Writer->WriteValue(TEXT("final_identity_unchanged"), bFinalIdentityUnchanged);
		Writer->WriteValue(TEXT("spacing_original_cm"), OriginalProbeSpacingCm);
		Writer->WriteValue(TEXT("spacing_temporary_cm"), 0.0);
		Writer->WriteValue(TEXT("spacing_restored_exactly"), bSpacingRestored);
		Writer->WriteValue(TEXT("invalid_ticker_observed"), bInvalidTickerObserved);
		Writer->WriteValue(TEXT("recovered_ticker_observed"), bRecoveredTickerObserved);
		Writer->WriteValue(TEXT("final_validate_current_bake_passed"), bFinalValidatePassed);
		Writer->WriteValue(TEXT("final_validate_seconds"), FinalValidateSeconds);
		Writer->WriteValue(TEXT("map_reloaded_for_cleanup"), bMapReloadedForCleanup);
		Writer->WriteArrayStart(TEXT("ticker_observations"));
		for (const FIMG2ProbeSpacingTickerObservation& Observation : Observations)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("sequence"), Observation.Sequence);
			Writer->WriteValue(TEXT("phase"), Observation.Phase);
			Writer->WriteValue(TEXT("status"), Observation.Status);
			Writer->WriteValue(TEXT("elapsed_seconds"), Observation.ElapsedSeconds);
			Writer->WriteValue(TEXT("phase_elapsed_seconds"), Observation.PhaseElapsedSeconds);
			Writer->WriteValue(TEXT("capture_ms"), Observation.CaptureMs);
			Writer->WriteValue(TEXT("asset_validation_ms"), Observation.AssetValidationMs);
			Writer->WriteValue(TEXT("full_tick_ms"), Observation.FullTickMs);
			Writer->WriteValue(TEXT("observed_captured"), Observation.ObservedCaptured);
			Writer->WriteValue(TEXT("observed_bound_asset_valid"), Observation.ObservedBoundAssetValid);
			Writer->WriteValue(TEXT("observed_triangles"), Observation.ObservedTriangles);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteArrayStart(TEXT("ticker_log_lines"));
		for (const FString& Line : TickerLogLines) Writer->WriteValue(Line);
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
		Writer->Close();
		return !ReportPath.IsEmpty() && FFileHelper::SaveStringToFile(
			Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FAutomationTestBase* Test = nullptr;
	FIMG2BakeScaleSource Source;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TUniquePtr<FIMG2BakeScaleLogCapture> LogCapture;
	TArray<FIMG2ProbeSpacingTickerObservation> Observations;
	TArray<FString> TickerLogLines;
	FString RunId;
	FString FailureReason;
	FString MapPackageFile;
	FString BaselineMapSha1;
	FString FinalMapSha1;
	FString FinalAssetPackageFile;
	FString FinalAssetSha1;
	FString RecoveredAssetIdentity;
	FString FinalAssetIdentity;
	FString RecoveredRecipeIdentity;
	FString FinalRecipeIdentity;
	FString FinalWorldIdentity;
	FString ReportDirectory;
	FString ReportPath;
	int64 BaselineMapBytes = 0;
	int64 FinalMapBytes = 0;
	int64 FinalAssetBytes = 0;
	float OriginalProbeSpacingCm = 0.0f;
	double StartedAt = 0.0;
	double PhaseStartedAt = 0.0;
	double LogDrainStartedAt = 0.0;
	double LastProfileCaptureMs = 0.0;
	double LastProfileAssetValidationMs = 0.0;
	double FinalValidateSeconds = 0.0;
	int32 Stage = 0;
	bool bMapLoaded = false;
	bool bMapReloadedForCleanup = false;
	bool bBaselineCaptured = false;
	bool bLogCaptureAttached = false;
	bool bProfilingEnabled = false;
	bool bTemporarySpacingApplied = false;
	bool bSpacingRestored = false;
	bool bInvalidTickerObserved = false;
	bool bRecoveredTickerObserved = false;
	bool bFinalValidatePassed = false;
	bool bDiskBytesUnchanged = false;
	bool bFinalIdentityUnchanged = false;
	bool bPassed = false;
	bool bFinished = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2ProbeSpacingTickerTest,
	"IceMoon.AcousticField.G2.ProbeSpacingTicker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2ProbeSpacingTickerTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticG2BakeScaleTestPrivate::FIMAcousticG2ProbeSpacingTickerCommand(this));
	return true;
}
#endif
