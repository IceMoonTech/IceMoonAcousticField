#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/MemoryWriter.h"
#include "StaticMeshResources.h"
#include "UObject/StrongObjectPtr.h"

namespace IMAcousticG2UniqueMeshTestPrivate
{
constexpr const TCHAR* MapPath = TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
constexpr const TCHAR* IgnoreTag = TEXT("IMAcousticIgnore");
constexpr const TCHAR* WhiteboxTag = TEXT("IMAcousticWhitebox");
constexpr int32 ExpectedWhiteboxComponents = 60;
constexpr int32 PlacementCount = 3000;
constexpr int64 ExpectedTotalTriangles = 144000;
constexpr double RegionWidthCm = 4000.0;
constexpr double RegionHeightCm = 3200.0;
constexpr double RegionHalfDepthCm = 1000.0;
constexpr float ProbeSpacingCm = 100.0f;
constexpr float ProbeHeightCm = 150.0f;
constexpr float MaterialAbsorption = 0.25f;
constexpr float MaterialScattering = 0.5f;
constexpr uint64 ResourceGuardBytes = 8ull * 1024ull * 1024ull * 1024ull;
constexpr double TransformTolerance = 0.0001;
constexpr double TriangleDigestQuantizationCm = 0.0001;

struct FIMMemorySample
{
	FString Stage;
	double ElapsedSeconds = 0.0;
	uint64 UsedPhysical = 0;
	uint64 PeakUsedPhysical = 0;
	uint64 UsedVirtual = 0;
	uint64 PeakUsedVirtual = 0;
	uint64 AvailablePhysical = 0;
};

static FIMMemorySample ReadMemory(const TCHAR* Stage, const double StartSeconds)
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	FIMMemorySample Sample;
	Sample.Stage = Stage;
	Sample.ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
	Sample.UsedPhysical = Stats.UsedPhysical;
	Sample.PeakUsedPhysical = Stats.PeakUsedPhysical;
	Sample.UsedVirtual = Stats.UsedVirtual;
	Sample.PeakUsedVirtual = Stats.PeakUsedVirtual;
	Sample.AvailablePhysical = Stats.AvailablePhysical;
	return Sample;
}

static bool IsBelowResourceGuard(const FIMMemorySample& Sample)
{
	return Sample.AvailablePhysical < ResourceGuardBytes;
}

static FString HashStrings(TArray<FString> Items)
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
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

static void HashDouble(FSHA1& Hash, const double Value)
{
	uint64 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	Hash.Update(reinterpret_cast<const uint8*>(&Bits), sizeof(Bits));
}

static void HashInt32(FSHA1& Hash, const int32 Value)
{
	Hash.Update(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
}

static void HashVector(FSHA1& Hash, const FVector& Value)
{
	HashDouble(Hash, Value.X);
	HashDouble(Hash, Value.Y);
	HashDouble(Hash, Value.Z);
}

static FString HashSceneArrays(const TArray<FVector>& Vertices,
	const TArray<FIntVector>& Triangles, const TArray<int32>& MaterialIndices)
{
	FSHA1 Hash;
	HashInt32(Hash, Vertices.Num());
	for (const FVector& Vertex : Vertices) HashVector(Hash, Vertex);
	HashInt32(Hash, Triangles.Num());
	for (const FIntVector& Triangle : Triangles)
	{
		HashInt32(Hash, Triangle.X);
		HashInt32(Hash, Triangle.Y);
		HashInt32(Hash, Triangle.Z);
	}
	HashInt32(Hash, MaterialIndices.Num());
	for (const int32 MaterialIndex : MaterialIndices) HashInt32(Hash, MaterialIndex);
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

static bool QuantizedPointToken(const FVector& Position, FString& OutToken)
{
	if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z)) return false;
	const double QX = Position.X / TriangleDigestQuantizationCm;
	const double QY = Position.Y / TriangleDigestQuantizationCm;
	const double QZ = Position.Z / TriangleDigestQuantizationCm;
	if (FMath::Abs(QX) > static_cast<double>(MAX_int64)
		|| FMath::Abs(QY) > static_cast<double>(MAX_int64)
		|| FMath::Abs(QZ) > static_cast<double>(MAX_int64)) return false;
	OutToken = FString::Printf(TEXT("%lld,%lld,%lld"), FMath::RoundToInt64(QX),
		FMath::RoundToInt64(QY), FMath::RoundToInt64(QZ));
	return true;
}

static bool OrientedTriangleMaterialKey(const int32 MaterialIndex, const FString& MaterialPath,
	const FVector& A, const FVector& B, const FVector& C, FString& OutKey)
{
	FString PointA;
	FString PointB;
	FString PointC;
	if (!QuantizedPointToken(A, PointA) || !QuantizedPointToken(B, PointB)
		|| !QuantizedPointToken(C, PointC)) return false;

	// Canonicalize only cyclic rotations, never reversed winding, so index reordering
	// is ignored while an orientation change still produces a different key.
	FString Canonical = PointA + TEXT("|") + PointB + TEXT("|") + PointC;
	const FString RotateABC = PointB + TEXT("|") + PointC + TEXT("|") + PointA;
	const FString RotateCAB = PointC + TEXT("|") + PointA + TEXT("|") + PointB;
	if (RotateABC < Canonical) Canonical = RotateABC;
	if (RotateCAB < Canonical) Canonical = RotateCAB;
	OutKey = FString::Printf(TEXT("%d|%s|%s"), MaterialIndex, *MaterialPath, *Canonical);
	return true;
}

static bool MeshOrientedTriangleMaterialDigest(UStaticMesh* Mesh, FString& OutDigest,
	int32& OutTriangleCount, FString& Error)
{
	OutDigest.Reset();
	OutTriangleCount = 0;
	if (!IsValid(Mesh) || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = FString::Printf(TEXT("Mesh %s has no render LOD0 for oriented-triangle comparison."),
			*GetPathNameSafe(Mesh));
		return false;
	}
	const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD0.VertexBuffers.PositionVertexBuffer;
	const auto IndexView = LOD0.IndexBuffer.GetArrayView();
	if (Positions.GetNumVertices() <= 0 || !Positions.GetVertexData() || IndexView.Num() <= 0
		|| (IndexView.Num() % 3) != 0)
	{
		Error = FString::Printf(TEXT("Mesh %s LOD0 CPU vertex/index readback is unavailable."),
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
			Error = FString::Printf(TEXT("Mesh %s LOD0 section references invalid static material slot %d."),
				*GetPathNameSafe(Mesh), Section.MaterialIndex);
			return false;
		}
		UMaterialInterface* Material = Mesh->GetMaterial(Section.MaterialIndex);
		if (!Material
			|| static_cast<uint64>(Section.FirstIndex) + static_cast<uint64>(Section.NumTriangles) * 3ull > static_cast<uint64>(IndexView.Num()))
		{
			Error = FString::Printf(TEXT("Mesh %s LOD0 section has invalid material or index range (material=%d first=%u triangles=%u)."),
				*GetPathNameSafe(Mesh), Section.MaterialIndex, Section.FirstIndex, Section.NumTriangles);
			return false;
		}
		for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
		{
			const uint32 FirstIndex = Section.FirstIndex + TriangleIndex * 3;
			const uint32 IA = LOD0.IndexBuffer.GetIndex(FirstIndex + 0);
			const uint32 IB = LOD0.IndexBuffer.GetIndex(FirstIndex + 1);
			const uint32 IC = LOD0.IndexBuffer.GetIndex(FirstIndex + 2);
			if (IA >= Positions.GetNumVertices() || IB >= Positions.GetNumVertices() || IC >= Positions.GetNumVertices())
			{
				Error = FString::Printf(TEXT("Mesh %s LOD0 triangle %u references a vertex outside the CPU position buffer."),
					*GetPathNameSafe(Mesh), FirstIndex / 3);
				return false;
			}
			FString Key;
			if (!OrientedTriangleMaterialKey(Section.MaterialIndex, Material->GetPathName(),
				FVector(Positions.VertexPosition(IA)), FVector(Positions.VertexPosition(IB)),
				FVector(Positions.VertexPosition(IC)), Key))
			{
				Error = FString::Printf(TEXT("Mesh %s LOD0 triangle %u contains a non-finite position."),
					*GetPathNameSafe(Mesh), FirstIndex / 3);
				return false;
			}
			TriangleKeys.Add(MoveTemp(Key));
			++OutTriangleCount;
		}
	}
	if (OutTriangleCount <= 0 || OutTriangleCount * 3 != IndexView.Num())
	{
		Error = FString::Printf(TEXT("Mesh %s LOD0 sections cover %d triangles but the CPU index buffer has %d indices."),
			*GetPathNameSafe(Mesh), OutTriangleCount, IndexView.Num());
		return false;
	}
	OutDigest = HashStrings(MoveTemp(TriangleKeys));
	return true;
}

static bool CapturedWorldTriangleMaterialDigest(const TArray<FVector>& Vertices,
	const TArray<FIntVector>& Triangles, const TArray<int32>& MaterialIndices,
	const AIMAcousticBakeVolume* Volume, FString& OutDigest, FString& Error)
{
	OutDigest.Reset();
	if (!IsValid(Volume) || Triangles.IsEmpty() || MaterialIndices.Num() != Triangles.Num())
	{
		Error = TEXT("Captured world-space triangle/material arrays have invalid counts or no volume.");
		return false;
	}
	TArray<FString> TriangleKeys;
	TriangleKeys.Reserve(Triangles.Num());
	for (int32 TriangleIndex = 0; TriangleIndex < Triangles.Num(); ++TriangleIndex)
	{
		const FIntVector& Triangle = Triangles[TriangleIndex];
		const int32 MaterialIndex = MaterialIndices[TriangleIndex];
		if (Triangle.X < 0 || Triangle.Y < 0 || Triangle.Z < 0
			|| Triangle.X >= Vertices.Num() || Triangle.Y >= Vertices.Num() || Triangle.Z >= Vertices.Num()
			|| !Volume->Materials.IsValidIndex(MaterialIndex) || !Volume->Materials[MaterialIndex].Material)
		{
			Error = FString::Printf(TEXT("Captured triangle %d references invalid vertices or acoustic material index %d."),
				TriangleIndex, MaterialIndex);
			return false;
		}
		FString Key;
		if (!OrientedTriangleMaterialKey(MaterialIndex,
			Volume->Materials[MaterialIndex].Material->GetPathName(),
			Vertices[Triangle.X], Vertices[Triangle.Y], Vertices[Triangle.Z], Key))
		{
			Error = FString::Printf(TEXT("Captured world-space triangle %d contains a non-finite position."), TriangleIndex);
			return false;
		}
		TriangleKeys.Add(MoveTemp(Key));
	}
	OutDigest = HashStrings(MoveTemp(TriangleKeys));
	return true;
}

static FString HashTransformList(const TArray<FTransform>& Transforms)
{
	FSHA1 Hash;
	HashInt32(Hash, Transforms.Num());
	for (const FTransform& Transform : Transforms)
	{
		HashVector(Hash, Transform.GetLocation());
		const FQuat Rotation = Transform.GetRotation();
		HashDouble(Hash, Rotation.X);
		HashDouble(Hash, Rotation.Y);
		HashDouble(Hash, Rotation.Z);
		HashDouble(Hash, Rotation.W);
		HashVector(Hash, Transform.GetScale3D());
	}
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

static bool SameTransform(const FTransform& A, const FTransform& B)
{
	return A.GetLocation().Equals(B.GetLocation(), TransformTolerance)
		&& A.GetRotation().Equals(B.GetRotation(), TransformTolerance)
		&& A.GetScale3D().Equals(B.GetScale3D(), TransformTolerance);
}

static FString MaterialIdentity(const UStaticMeshComponent* Component)
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

static FString WorldIdentity(UWorld* World)
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
				Item += MaterialIdentity(MeshComponent);
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
	return World->GetOutermost()->GetName() + TEXT("|") + HashStrings(MoveTemp(Items));
}

static FString BakeAssetIdentity(UWorld* World, int32& OutVolumeCount)
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
			FSHA1 StoredSectionsHash;
			if (Asset->SceneData.Num() > 0) StoredSectionsHash.Update(Asset->SceneData.GetData(), Asset->SceneData.Num());
			if (Asset->ProbeData.Num() > 0) StoredSectionsHash.Update(Asset->ProbeData.GetData(), Asset->ProbeData.Num());
			StoredSectionsHash.Final();
			uint8 StoredSectionsDigest[20];
			StoredSectionsHash.GetHash(StoredSectionsDigest);
			Item += TEXT("|") + Asset->WorldPackage + TEXT("|") + Asset->SceneFingerprint
				+ TEXT("|stored_payload_digest=") + Asset->PayloadDigest + TEXT("|stored_sections_sha1=")
				+ BytesToHex(StoredSectionsDigest, UE_ARRAY_COUNT(StoredSectionsDigest))
				+ TEXT("|") + HashStrings({Asset->MetadataJson});
		}
		Items.Add(MoveTemp(Item));
	}
	return HashStrings(MoveTemp(Items));
}

static bool MeshLOD0Digest(UStaticMesh* Mesh, FString& OutDigest, FString& OutSectionFingerprint,
	int32& OutVertices, int32& OutTriangles, FString& Error)
{
	OutDigest.Reset();
	OutSectionFingerprint.Reset();
	OutVertices = 0;
	OutTriangles = 0;
	if (!IsValid(Mesh) || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = FString::Printf(TEXT("Mesh %s has no render LOD0."), *GetPathNameSafe(Mesh));
		return false;
	}

	const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];
	OutVertices = LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	if (OutVertices <= 0 || LOD0.VertexBuffers.PositionVertexBuffer.GetVertexData() == nullptr
		|| LOD0.IndexBuffer.GetNumIndices() <= 0 || (LOD0.IndexBuffer.GetNumIndices() % 3) != 0)
	{
		Error = FString::Printf(TEXT("Mesh %s does not retain readable CPU LOD0 vertex/index buffers."),
			*GetPathNameSafe(Mesh));
		return false;
	}

	TArray<FString> SectionItems;
	for (int32 SectionIndex = 0; SectionIndex < LOD0.Sections.Num(); ++SectionIndex)
	{
		const FStaticMeshSection& Section = LOD0.Sections[SectionIndex];
		OutTriangles += Section.NumTriangles;
		SectionItems.Add(FString::Printf(TEXT("%d|%d|%u|%u|%u|%u"), SectionIndex,
			Section.MaterialIndex, Section.FirstIndex, Section.NumTriangles,
			Section.MinVertexIndex, Section.MaxVertexIndex));
	}
	if (OutTriangles <= 0 || static_cast<uint32>(OutTriangles * 3) != LOD0.IndexBuffer.GetNumIndices())
	{
		Error = FString::Printf(TEXT("Mesh %s LOD0 section and index counts disagree."), *GetPathNameSafe(Mesh));
		return false;
	}

	// ExportStaticMeshLOD reads the CPU vertex and index buffers back into a MeshDescription.
	// Serializing that readback gives every transient mesh a comparable topology/content digest.
	FMeshDescription Readback;
	Mesh->ExportStaticMeshLOD(LOD0, Readback);
	if (Readback.Vertices().Num() <= 0 || Readback.Triangles().Num() != OutTriangles)
	{
		Error = FString::Printf(TEXT("Mesh %s CPU LOD0 readback returned invalid topology."), *GetPathNameSafe(Mesh));
		return false;
	}
	TArray<uint8> Serialized;
	FMemoryWriter Writer(Serialized, true);
	Readback.Serialize(Writer);
	Writer.Flush();
	if (Serialized.IsEmpty())
	{
		Error = FString::Printf(TEXT("Mesh %s CPU LOD0 readback serialized no content."), *GetPathNameSafe(Mesh));
		return false;
	}
	FSHA1 Hash;
	Hash.Update(Serialized.GetData(), Serialized.Num());
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	OutDigest = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	OutSectionFingerprint = HashStrings(MoveTemp(SectionItems));
	return true;
}

struct FIMSource
{
	FString WorldPackage;
	FString MapIdentity;
	FString WhiteboxIdentity;
	FString OriginalBakeAssetIdentity;
	FString MeshPath;
	FString MaterialPath;
	FString LOD0Digest;
	FString OrientedTriangleMaterialDigest;
	FString SectionFingerprint;
	int32 OriginalBakeVolumeCount = 0;
	int32 WhiteboxComponentCount = 0;
	int32 UniqueWhiteboxMeshCount = 0;
	int32 LOD0Vertices = 0;
	int32 LOD0Triangles = 0;
	FVector MeshBoundsOrigin = FVector::ZeroVector;
	FVector MeshBoundsExtent = FVector::ZeroVector;
	TWeakObjectPtr<UStaticMesh> Mesh;
	TWeakObjectPtr<UMaterialInterface> Material;
};

static bool ReadSource(UWorld* World, FIMSource& Out, FString& Error)
{
	if (!IsValid(World))
	{
		Error = TEXT("Editor world is unavailable after loading the W2 whitebox mother map.");
		return false;
	}
	Out = FIMSource();
	Out.WorldPackage = World->GetOutermost()->GetName();
	Out.MapIdentity = WorldIdentity(World);
	Out.OriginalBakeAssetIdentity = BakeAssetIdentity(World, Out.OriginalBakeVolumeCount);
	TSet<UStaticMesh*> UniqueMeshes;
	UStaticMesh* SourceMesh = nullptr;
	UMaterialInterface* SourceMaterial = nullptr;
	bool bUniformMaterial = true;
	TArray<FString> WhiteboxItems;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->ActorHasTag(WhiteboxTag)) continue;
		TArray<UStaticMeshComponent*> Components;
		It->GetComponents(Components);
		for (UStaticMeshComponent* Component : Components)
		{
			if (!IsValid(Component) || !Component->GetStaticMesh()) continue;
			++Out.WhiteboxComponentCount;
			UniqueMeshes.Add(Component->GetStaticMesh());
			if (!SourceMesh) SourceMesh = Component->GetStaticMesh();
			if (Out.WhiteboxComponentCount == 1) SourceMaterial = Component->GetMaterial(0);
			else if (SourceMaterial != Component->GetMaterial(0)) bUniformMaterial = false;
			WhiteboxItems.Add(Component->GetPathName() + TEXT("|")
				+ GetPathNameSafe(Component->GetStaticMesh()) + TEXT("|")
				+ Component->GetComponentTransform().ToString() + TEXT("|")
				+ GetPathNameSafe(Component->GetMaterial(0)));
		}
	}
	Out.UniqueWhiteboxMeshCount = UniqueMeshes.Num();
	Out.WhiteboxIdentity = HashStrings(MoveTemp(WhiteboxItems));
	if (Out.WorldPackage != MapPath || Out.WhiteboxComponentCount != ExpectedWhiteboxComponents
		|| Out.UniqueWhiteboxMeshCount != 1 || !SourceMesh || !SourceMaterial || !bUniformMaterial
		|| SourceMesh->GetStaticMaterials().Num() != 1)
	{
		Error = FString::Printf(TEXT("W2 source drift: package=%s whitebox_components=%d unique_meshes=%d mesh=%s material=%s slots=%d"),
			*Out.WorldPackage, Out.WhiteboxComponentCount, Out.UniqueWhiteboxMeshCount,
			*GetPathNameSafe(SourceMesh), *GetPathNameSafe(SourceMaterial),
			SourceMesh ? SourceMesh->GetStaticMaterials().Num() : 0);
		return false;
	}
	if (!MeshLOD0Digest(SourceMesh, Out.LOD0Digest, Out.SectionFingerprint,
		Out.LOD0Vertices, Out.LOD0Triangles, Error)) return false;
	int32 OrientedTriangleCount = 0;
	if (!MeshOrientedTriangleMaterialDigest(SourceMesh, Out.OrientedTriangleMaterialDigest,
		OrientedTriangleCount, Error)) return false;
	if (OrientedTriangleCount != Out.LOD0Triangles)
	{
		Error = FString::Printf(TEXT("Source Cube LOD0 oriented triangle readback found %d triangles; section summary found %d."),
			OrientedTriangleCount, Out.LOD0Triangles);
		return false;
	}
	Out.MeshBoundsOrigin = SourceMesh->GetBounds().Origin;
	Out.MeshBoundsExtent = SourceMesh->GetBounds().BoxExtent;
	Out.MeshPath = SourceMesh->GetPathName();
	Out.MaterialPath = SourceMaterial->GetPathName();
	Out.Mesh = SourceMesh;
	Out.Material = SourceMaterial;
	return true;
}

struct FIMCase
{
	int32 UniqueMeshCount = 0;
	FString Outcome = TEXT("NOT_STARTED");
	FString Error;
	FString SourceMeshDigest;
	FString CommonMeshDigest;
	FString SourceOrientedTriangleMaterialDigest;
	FString CommonCloneOrientedTriangleMaterialDigest;
	FString SectionFingerprint;
	FString ExpectedTransformFingerprint;
	FString TransformFingerprint;
	FString CapturedArrayDigest;
	FString CapturedWorldTriangleMaterialDigest;
	FString SceneFingerprint;
	int32 PlacementsRequested = PlacementCount;
	int32 PlacementsBuilt = 0;
	int32 StaticMeshComponents = 0;
	int32 DistinctMeshPointers = 0;
	int32 DistinctMeshPaths = 0;
	int32 DistinctMeshUniqueIds = 0;
	int32 MeshLODsCPUReadable = 0;
	int32 SourceLOD0Triangles = 0;
	int64 ExpectedTriangles = ExpectedTotalTriangles;
	int32 CapturedVertices = 0;
	int32 CapturedTriangles = 0;
	int32 CapturedMaterialIndices = 0;
	int32 NonZeroMaterialIndices = 0;
	int32 GeneratedProbeCount = 0;
	int32 ProductExportedTriangles = 0;
	int32 ActualTransformsMatched = 0;
	int32 ActualMaterialsMatched = 0;
	int32 MinimumPlacementsPerMesh = 0;
	int32 MaximumPlacementsPerMesh = 0;
	bool bMeshIdentityExact = false;
	bool bMeshContentExact = false;
	bool bSectionLayoutMatchesSource = false;
	bool bFrequencyExact = false;
	bool bTransformsExact = false;
	bool bMaterialsExact = false;
	bool bStaticMobilityExact = false;
	bool bCaptureSucceeded = false;
	bool bCaptureGeometryExact = false;
	bool bCapturedWorldTriangleMultisetValid = false;
	bool bProbeGenerationSucceeded = false;
	double MeshBuildWallSeconds = 0.0;
	double PlacementWallSeconds = 0.0;
	double CaptureAccessorWallSeconds = 0.0;
	double CaptureMultisetWallSeconds = 0.0;
	double GenerateProbesWallSeconds = 0.0;
	TArray<FIMMemorySample> MemorySamples;
};

class FIMFixture
{
public:
	~FIMFixture() { Cleanup(); }

	void Cleanup()
	{
		for (const TWeakObjectPtr<AActor>& Actor : TransientActors)
			if (Actor.IsValid()) Actor->Destroy();
		TransientActors.Reset();
		if (Volume.IsValid()) Volume->Destroy();
		Volume.Reset();
		for (const TWeakObjectPtr<UPrimitiveComponent>& Component : TaggedComponents)
			if (Component.IsValid()) Component->ComponentTags.Remove(IgnoreTag);
		TaggedComponents.Reset();
		World = nullptr;
	}

	bool Prime(FIMSource& Out, FString& Error)
	{
		Cleanup();
		if (!FEditorFileUtils::LoadMap(MapPath, false, true))
		{
			Error = TEXT("Could not load the required W2 whitebox mother map.");
			return false;
		}
		World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!ReadSource(World, Out, Error)) return false;
		BaselineMapIdentity = Out.MapIdentity;
		BaselineWhiteboxIdentity = Out.WhiteboxIdentity;
		BaselineBakeAssetIdentity = Out.OriginalBakeAssetIdentity;
		BaselineMeshPath = Out.MeshPath;
		BaselineMaterialPath = Out.MaterialPath;
		return true;
	}

	bool PrepareCase(FIMSource& Out, FString& Error)
	{
		Cleanup();
		if (!FEditorFileUtils::LoadMap(MapPath, false, true))
		{
			Error = TEXT("Could not reload the W2 whitebox mother map for an isolated unique-mesh case.");
			return false;
		}
		World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!ReadSource(World, Out, Error)) return false;
		if (Out.MapIdentity != BaselineMapIdentity || Out.WhiteboxIdentity != BaselineWhiteboxIdentity
			|| Out.OriginalBakeAssetIdentity != BaselineBakeAssetIdentity
			|| Out.MeshPath != BaselineMeshPath || Out.MaterialPath != BaselineMaterialPath)
		{
			Error = TEXT("W2 map, whitebox, source mesh, material, or bake binding changed between cases.");
			return false;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UPrimitiveComponent*> Components;
			It->GetComponents(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				if (!IsValid(Component) || Component->ComponentHasTag(IgnoreTag)) continue;
				Component->ComponentTags.Add(IgnoreTag);
				TaggedComponents.Add(Component);
			}
		}
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		Volume = World->SpawnActor<AIMAcousticBakeVolume>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
		if (!Volume.IsValid())
		{
			Error = TEXT("Could not spawn the transient unique-mesh acoustic bake volume.");
			return false;
		}
		Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
		Volume->SetActorLocation(FVector::ZeroVector);
		Volume->SetActorTickEnabled(false);
		Volume->BakedField = nullptr;
		Volume->BakeBounds->SetMobility(EComponentMobility::Static);
		Volume->BakeBounds->SetBoxExtent(FVector(RegionWidthCm * 0.5, RegionHeightCm * 0.5, RegionHalfDepthCm));
		Volume->ProbeSpacingCm = ProbeSpacingCm;
		Volume->ProbeHeightCm = ProbeHeightCm;
		FIMAcousticMaterialMapping Mapping;
		Mapping.Material = Out.Material.Get();
		Mapping.Absorption = FVector(MaterialAbsorption);
		Mapping.Scattering = MaterialScattering;
		Volume->Materials = {Mapping};
		return true;
	}

	UWorld* GetWorld() const { return World; }
	AIMAcousticBakeVolume* GetVolume() const { return Volume.Get(); }
	void TrackActor(AActor* Actor) { if (IsValid(Actor)) TransientActors.Add(Actor); }

	bool RestoreAndCompare(FString& OutMapIdentity, FString& OutAssetIdentity, FString& Error)
	{
		Cleanup();
		if (!FEditorFileUtils::LoadMap(MapPath, false, true))
		{
			Error = TEXT("Final W2 mother-map reload failed.");
			return false;
		}
		UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FIMSource Restored;
		if (!ReadSource(RestoredWorld, Restored, Error)) return false;
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

static bool BuildTransforms(const FIMSource& Source, TArray<FTransform>& OutTransforms, FString& Error)
{
	if (Source.LOD0Triangles != 48 || Source.MeshBoundsExtent.IsNearlyZero())
	{
		Error = FString::Printf(TEXT("Expected the W2 Cube LOD0 (48 triangles) with nonzero bounds; found %d."),
			Source.LOD0Triangles);
		return false;
	}
	const int32 Columns = FMath::CeilToInt(FMath::Sqrt(
		static_cast<float>(PlacementCount * RegionWidthCm / RegionHeightCm)));
	const int32 Rows = FMath::DivideAndRoundUp(PlacementCount, Columns);
	const double CellWidth = RegionWidthCm / Columns;
	const double CellHeight = RegionHeightCm / Rows;
	const double MaxExtent = FMath::Max3(Source.MeshBoundsExtent.X, Source.MeshBoundsExtent.Y,
		Source.MeshBoundsExtent.Z);
	if (!FMath::IsFinite(MaxExtent) || MaxExtent <= 0.0)
	{
		Error = TEXT("W2 Cube bounds are invalid for the fixed placement grid.");
		return false;
	}
	const float UniformScale = static_cast<float>(FMath::Min(1.0, 0.9 * FMath::Min(CellWidth, CellHeight)
		/ (2.0 * MaxExtent)));
	if (Source.MeshBoundsExtent.X * UniformScale > CellWidth * 0.5
		|| Source.MeshBoundsExtent.Y * UniformScale > CellHeight * 0.5)
	{
		Error = TEXT("Scaled Cube bounds do not fit the fixed 40m x 32m grid cells.");
		return false;
	}
	OutTransforms.Reset(PlacementCount);
	for (int32 Index = 0; Index < PlacementCount; ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row = Index / Columns;
		const FVector BoundsCenter(-RegionWidthCm * 0.5 + (Column + 0.5) * CellWidth,
			-RegionHeightCm * 0.5 + (Row + 0.5) * CellHeight, 0.0);
		const FVector PivotLocation = BoundsCenter - Source.MeshBoundsOrigin * UniformScale;
		OutTransforms.Emplace(FQuat::Identity, PivotLocation, FVector(UniformScale));
	}
	return true;
}

static bool BuildUniqueMeshes(const FIMSource& Source, int32 Count, const FString& RunId,
	TArray<TStrongObjectPtr<UStaticMesh>>& OutMeshes, TArray<FString>& OutMeshDigests,
	TArray<FString>& OutSectionFingerprints, TArray<FString>& OutOrientedDigests,
	FIMCase& Case, bool& bOutResourceGuard, FString& Error)
{
	UStaticMesh* SourceMesh = Source.Mesh.Get();
	if (!IsValid(SourceMesh) || Count <= 0)
	{
		Error = TEXT("Source mesh or requested unique mesh count is invalid.");
		return false;
	}
	if (!SourceMesh->GetRenderData() || SourceMesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = TEXT("Source mesh render LOD0 disappeared before transient mesh construction.");
		return false;
	}
	FMeshDescription SourceDescription;
	SourceMesh->ExportStaticMeshLOD(SourceMesh->GetRenderData()->LODResources[0], SourceDescription);
	if (SourceDescription.Vertices().Num() <= 0 || SourceDescription.Triangles().Num() != Source.LOD0Triangles)
	{
		Error = TEXT("Source LOD0 export did not produce a reusable CPU MeshDescription.");
		return false;
	}
	TArray<const FMeshDescription*> Descriptions;
	Descriptions.Add(&SourceDescription);
	const TArray<FStaticMaterial> SourceMaterials = SourceMesh->GetStaticMaterials();
	if (SourceMaterials.Num() != 1)
	{
		Error = TEXT("The test fixture requires the source Cube's single material slot.");
		return false;
	}

	const double Started = FPlatformTime::Seconds();
	OutMeshes.Reset();
	OutMeshDigests.Reset();
	OutSectionFingerprints.Reset();
	OutOrientedDigests.Reset();
	OutMeshes.Reserve(Count);
	OutMeshDigests.Reserve(Count);
	OutSectionFingerprints.Reserve(Count);
	OutOrientedDigests.Reserve(Count);
	TSet<const UStaticMesh*> UniquePointers;
	TSet<FString> UniquePaths;
	TSet<uint32> UniqueIds;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FIMMemorySample BeforeBuild = ReadMemory(TEXT("mesh_build_progress"), Started);
		Case.MemorySamples.Add(BeforeBuild);
		if (IsBelowResourceGuard(BeforeBuild))
		{
			bOutResourceGuard = true;
			Error = FString::Printf(TEXT("Available system RAM fell below the %.1f GiB guard while building mesh %d of %d."),
				ResourceGuardBytes / (1024.0 * 1024.0 * 1024.0), Index + 1, Count);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}

		const FName BaseName(*FString::Printf(TEXT("IMG2_Unique_%s_%03d"), *RunId, Index));
		const FName MeshName = MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), BaseName);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), MeshName, RF_Transient);
		if (!IsValid(Mesh))
		{
			Error = FString::Printf(TEXT("Could not allocate transient UStaticMesh %d of %d."), Index + 1, Count);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
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
			Error = FString::Printf(TEXT("BuildFromMeshDescriptions failed for transient mesh %d of %d."), Index + 1, Count);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}

		FString Digest;
		FString SectionFingerprint;
		FString OrientedDigest;
		int32 Vertices = 0;
		int32 Triangles = 0;
		int32 OrientedTriangles = 0;
		FString DigestError;
		const bool bStaticMaterialsExact = Mesh->GetStaticMaterials().Num() == SourceMaterials.Num()
			&& Mesh->GetStaticMaterials()[0] == SourceMaterials[0];
		if (!Mesh->bAllowCPUAccess)
		{
			Error = FString::Printf(TEXT("Transient mesh %d lost bAllowCPUAccess."), Index + 1);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}
		if (!bStaticMaterialsExact)
		{
			Error = FString::Printf(TEXT("Transient mesh %d did not preserve the source static material slot."), Index + 1);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}
		if (!MeshLOD0Digest(Mesh, Digest, SectionFingerprint, Vertices, Triangles, DigestError))
		{
			Error = FString::Printf(TEXT("Transient mesh %d failed CPU LOD0 readback: %s"), Index + 1, *DigestError);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}
		if (!MeshOrientedTriangleMaterialDigest(Mesh, OrientedDigest, OrientedTriangles, DigestError))
		{
			Error = FString::Printf(TEXT("Transient mesh %d failed semantic CPU LOD0 readback: %s"), Index + 1, *DigestError);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}
		if (OrientedDigest != Source.OrientedTriangleMaterialDigest
			|| Triangles != Source.LOD0Triangles || OrientedTriangles != Source.LOD0Triangles
			|| Mesh->GetRenderData()->LODResources.Num() != 1)
		{
			Error = FString::Printf(TEXT("Transient mesh %d differs from source oriented LOD0 triangles/materials: source_semantic=%s clone_semantic=%s source_serialized_diagnostic=%s clone_serialized_diagnostic=%s source_sections=%s clone_sections=%s triangles=%d/%d."),
				Index + 1, *Source.OrientedTriangleMaterialDigest, *OrientedDigest,
				*Source.LOD0Digest, *Digest, *Source.SectionFingerprint,
				*SectionFingerprint, OrientedTriangles, Triangles);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}
		if (UniquePointers.Contains(Mesh) || UniquePaths.Contains(Mesh->GetPathName())
			|| UniqueIds.Contains(Mesh->GetUniqueID()))
		{
			Error = FString::Printf(TEXT("Transient mesh %d did not receive a unique UObject identity."), Index + 1);
			Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
			return false;
		}
		UniquePointers.Add(Mesh);
		UniquePaths.Add(Mesh->GetPathName());
		UniqueIds.Add(Mesh->GetUniqueID());
		OutMeshes.Add(MoveTemp(StrongMesh));
		OutMeshDigests.Add(Digest);
		OutSectionFingerprints.Add(SectionFingerprint);
		OutOrientedDigests.Add(OrientedDigest);
		++Case.MeshLODsCPUReadable;
	}
	Case.MeshBuildWallSeconds = FPlatformTime::Seconds() - Started;
	Case.DistinctMeshPointers = UniquePointers.Num();
	Case.DistinctMeshPaths = UniquePaths.Num();
	Case.DistinctMeshUniqueIds = UniqueIds.Num();
	Case.bMeshIdentityExact = Case.DistinctMeshPointers == Count
		&& Case.DistinctMeshPaths == Count && Case.DistinctMeshUniqueIds == Count
		&& Case.MeshLODsCPUReadable == Count;
	Case.bMeshContentExact = OutOrientedDigests.Num() == Count
		&& OutOrientedDigests.ContainsByPredicate([&](const FString& Digest)
			{ return Digest != Source.OrientedTriangleMaterialDigest; }) == false;
	Case.SourceMeshDigest = Source.LOD0Digest;
	Case.CommonMeshDigest = OutMeshDigests.IsEmpty() ? FString() : OutMeshDigests[0];
	Case.SourceOrientedTriangleMaterialDigest = Source.OrientedTriangleMaterialDigest;
	Case.CommonCloneOrientedTriangleMaterialDigest = OutOrientedDigests.IsEmpty() ? FString() : OutOrientedDigests[0];
	Case.SectionFingerprint = OutSectionFingerprints.IsEmpty() ? FString() : OutSectionFingerprints[0];
	Case.bSectionLayoutMatchesSource = OutSectionFingerprints.Num() == Count
		&& OutSectionFingerprints.ContainsByPredicate([&](const FString& Value)
			{ return Value != Source.SectionFingerprint; }) == false;
	Case.SourceLOD0Triangles = Source.LOD0Triangles;
	Case.MemorySamples.Add(ReadMemory(TEXT("mesh_build_complete"), Started));
	return Case.bMeshIdentityExact && Case.bMeshContentExact;
}

static bool AddStaticMeshPlacement(FIMFixture& Fixture, UStaticMesh* Mesh,
	UMaterialInterface* Material, const FTransform& Transform,
	TWeakObjectPtr<AStaticMeshActor>& OutActor, FString& Error)
{
	OutActor.Reset();
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AStaticMeshActor* Actor = Fixture.GetWorld()->SpawnActor<AStaticMeshActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
	if (!IsValid(Actor))
	{
		Error = TEXT("Could not spawn an independent transient StaticMeshActor placement.");
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
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, Material);
	if (Component->Mobility != EComponentMobility::Static || Component->GetStaticMesh() != Mesh
		|| Component->GetMaterial(0) != Material || Component->GetNumMaterials() != 1)
	{
		Error = TEXT("Static placement did not retain the requested mesh identity and effective material.");
		return false;
	}
	OutActor = Actor;
	return true;
}

static bool RunCase(FIMFixture& Fixture, const FIMSource& InitialSource, const int32 UniqueMeshCount,
	const FString& RunId, FIMCase& Out, bool& bOutResourceGuard, FString& Error)
{
	Out = FIMCase();
	Out.UniqueMeshCount = UniqueMeshCount;
	const double CaseStarted = FPlatformTime::Seconds();
	Out.MemorySamples.Add(ReadMemory(TEXT("case_start"), CaseStarted));
	if (IsBelowResourceGuard(Out.MemorySamples.Last()))
	{
		bOutResourceGuard = true;
		Error = TEXT("Available system RAM was below the 8 GiB guard at case start.");
		Out.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
		Out.Error = Error;
		return false;
	}

	FIMSource Source;
	if (!Fixture.PrepareCase(Source, Error))
	{
		Out.Outcome = TEXT("FAIL_SETUP");
		Out.Error = Error;
		return false;
	}
	if (Source.LOD0Digest != InitialSource.LOD0Digest)
	{
		Error = TEXT("Source Cube CPU LOD0 digest changed after W2 reload.");
		Out.Outcome = TEXT("FAIL_SOURCE_ASSET_DRIFT");
		Out.Error = Error;
		return false;
	}

	TArray<FTransform> Transforms;
	if (!BuildTransforms(Source, Transforms, Error))
	{
		Out.Outcome = TEXT("FAIL_TRANSFORM_SETUP");
		Out.Error = Error;
		return false;
	}
	Out.ExpectedTransformFingerprint = HashTransformList(Transforms);
	Out.SourceLOD0Triangles = Source.LOD0Triangles;
	Out.ExpectedTriangles = static_cast<int64>(PlacementCount) * Source.LOD0Triangles;

	TArray<TStrongObjectPtr<UStaticMesh>> UniqueMeshes;
	TArray<FString> MeshDigests;
	TArray<FString> SectionFingerprints;
	TArray<FString> OrientedMeshDigests;
	if (!BuildUniqueMeshes(Source, UniqueMeshCount, RunId, UniqueMeshes, MeshDigests,
		SectionFingerprints, OrientedMeshDigests, Out, bOutResourceGuard, Error))
	{
		Out.Outcome = bOutResourceGuard ? TEXT("INCONCLUSIVE_RESOURCE_GUARD") : TEXT("FAIL_MESH_FIXTURE");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}
	if (!Out.MemorySamples.IsEmpty() && IsBelowResourceGuard(Out.MemorySamples.Last()))
	{
		bOutResourceGuard = true;
		Error = TEXT("Available system RAM fell below the 8 GiB guard after building all transient mesh UObjects.");
		Out.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}

	TArray<int32> Frequencies;
	Frequencies.Init(0, UniqueMeshCount);
	TArray<TWeakObjectPtr<AStaticMeshActor>> PlacementActors;
	PlacementActors.Reserve(PlacementCount);
	const double PlacementStarted = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Transforms.Num(); ++Index)
	{
		const int32 MeshIndex = Index % UniqueMeshCount;
		TWeakObjectPtr<AStaticMeshActor> PlacementActor;
		if (!AddStaticMeshPlacement(Fixture, UniqueMeshes[MeshIndex].Get(), Source.Material.Get(),
			Transforms[Index], PlacementActor, Error))
		{
			Out.PlacementsBuilt = Index;
			Out.PlacementWallSeconds = FPlatformTime::Seconds() - PlacementStarted;
			Out.Outcome = TEXT("FAIL_PLACEMENT_BUILD");
			Out.Error = Error;
			Fixture.Cleanup();
			return false;
		}
		PlacementActors.Add(PlacementActor);
		++Frequencies[MeshIndex];
		++Out.PlacementsBuilt;
		if ((Index + 1) % 250 == 0)
		{
			const FIMMemorySample Progress = ReadMemory(TEXT("placement_progress"), CaseStarted);
			Out.MemorySamples.Add(Progress);
			if (IsBelowResourceGuard(Progress))
			{
				bOutResourceGuard = true;
				Error = FString::Printf(TEXT("Available system RAM fell below the 8 GiB guard after %d placements."), Index + 1);
				Out.PlacementWallSeconds = FPlatformTime::Seconds() - PlacementStarted;
				Out.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
				Out.Error = Error;
				Fixture.Cleanup();
				return false;
			}
		}
	}
	Out.PlacementWallSeconds = FPlatformTime::Seconds() - PlacementStarted;
	Out.StaticMeshComponents = Out.PlacementsBuilt;
	Out.MinimumPlacementsPerMesh = Frequencies.IsEmpty() ? 0 : MAX_int32;
	for (const int32 Frequency : Frequencies)
	{
		Out.MinimumPlacementsPerMesh = FMath::Min(Out.MinimumPlacementsPerMesh, Frequency);
		Out.MaximumPlacementsPerMesh = FMath::Max(Out.MaximumPlacementsPerMesh, Frequency);
	}
	Out.bFrequencyExact = Out.PlacementsBuilt == PlacementCount
		&& Out.MinimumPlacementsPerMesh == PlacementCount / UniqueMeshCount
		&& Out.MaximumPlacementsPerMesh == PlacementCount / UniqueMeshCount;

	TArray<FTransform> ActualTransforms;
	ActualTransforms.Reserve(PlacementCount);
	TArray<int32> ActualFrequencies;
	ActualFrequencies.Init(0, UniqueMeshCount);
	int32 TransformMatches = 0;
	int32 MaterialMatches = 0;
	int32 StaticMobilityMatches = 0;
	for (int32 PlacementIndex = 0; PlacementIndex < PlacementActors.Num(); ++PlacementIndex)
	{
		AStaticMeshActor* Actor = PlacementActors[PlacementIndex].Get();
		if (!IsValid(Actor))
		{
			Error = FString::Printf(TEXT("Placement actor %d became invalid before verification."), PlacementIndex);
			break;
		}
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		if (!IsValid(Component))
		{
			Error = FString::Printf(TEXT("Placement %d lost its static mesh component."), PlacementIndex);
			break;
		}
		const int32 ExpectedMeshIndex = PlacementIndex % UniqueMeshCount;
		const bool bMeshMatches = Component->GetStaticMesh() == UniqueMeshes[ExpectedMeshIndex].Get();
		const bool bTransformMatches = SameTransform(Component->GetComponentTransform(), Transforms[PlacementIndex]);
		const bool bMaterialMatches = Component->GetNumMaterials() == 1
			&& Component->GetMaterial(0) == Source.Material.Get();
		if (bTransformMatches) ++TransformMatches;
		if (bMaterialMatches) ++MaterialMatches;
		if (Component->Mobility == EComponentMobility::Static) ++StaticMobilityMatches;
		ActualTransforms.Add(Component->GetComponentTransform());
		if (!bMeshMatches)
		{
			Error = FString::Printf(TEXT("Placement %d did not retain mesh asset slot %d."),
				PlacementIndex, ExpectedMeshIndex);
			break;
		}
		++ActualFrequencies[ExpectedMeshIndex];
	}
	Out.PlacementsBuilt = PlacementActors.Num();
	Out.StaticMeshComponents = ActualTransforms.Num();
	Out.ActualTransformsMatched = TransformMatches;
	Out.ActualMaterialsMatched = MaterialMatches;
	Out.MinimumPlacementsPerMesh = ActualFrequencies.IsEmpty() ? 0 : MAX_int32;
	Out.MaximumPlacementsPerMesh = 0;
	for (const int32 Frequency : ActualFrequencies)
	{
		Out.MinimumPlacementsPerMesh = FMath::Min(Out.MinimumPlacementsPerMesh, Frequency);
		Out.MaximumPlacementsPerMesh = FMath::Max(Out.MaximumPlacementsPerMesh, Frequency);
	}
	Out.bFrequencyExact = Out.PlacementsBuilt == PlacementCount
		&& Out.MinimumPlacementsPerMesh == PlacementCount / UniqueMeshCount
		&& Out.MaximumPlacementsPerMesh == PlacementCount / UniqueMeshCount;
	Out.TransformFingerprint = HashTransformList(ActualTransforms);
	Out.bTransformsExact = Out.PlacementsBuilt == PlacementCount && TransformMatches == PlacementCount;
	Out.bMaterialsExact = Out.PlacementsBuilt == PlacementCount && MaterialMatches == PlacementCount;
	Out.bStaticMobilityExact = StaticMobilityMatches == PlacementCount;
	if (!Out.bTransformsExact || !Out.bMaterialsExact || !Out.bFrequencyExact
		|| !Out.bStaticMobilityExact || !Error.IsEmpty())
	{
		if (Error.IsEmpty()) Error = FString::Printf(TEXT("Placement validation mismatch: actors=%d transforms=%d materials=%d frequency=%d..%d expected_each=%d."),
			Out.PlacementsBuilt, TransformMatches, MaterialMatches, Out.MinimumPlacementsPerMesh,
			Out.MaximumPlacementsPerMesh, PlacementCount / UniqueMeshCount);
		Out.Outcome = TEXT("FAIL_PLACEMENT_VALIDATION");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}

	const FIMMemorySample BeforeCapture = ReadMemory(TEXT("pre_capture"), CaseStarted);
	Out.MemorySamples.Add(BeforeCapture);
	if (IsBelowResourceGuard(BeforeCapture))
	{
		bOutResourceGuard = true;
		Error = TEXT("Available system RAM fell below the 8 GiB guard before CaptureScene.");
		Out.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}
	TArray<FVector> Vertices;
	TArray<FIntVector> Triangles;
	TArray<int32> MaterialIndices;
	const double CaptureStarted = FPlatformTime::Seconds();
	Out.bCaptureSucceeded = Fixture.GetVolume()->IM_CaptureSceneGeometryForTest(
		Vertices, Triangles, MaterialIndices, Out.SceneFingerprint);
	Out.CaptureAccessorWallSeconds = FPlatformTime::Seconds() - CaptureStarted;
	Out.CapturedVertices = Vertices.Num();
	Out.CapturedTriangles = Triangles.Num();
	Out.CapturedMaterialIndices = MaterialIndices.Num();
	Out.CapturedArrayDigest = HashSceneArrays(Vertices, Triangles, MaterialIndices);
	FString CapturedTriangleDigestError;
	const double MultisetStarted = FPlatformTime::Seconds();
	Out.bCapturedWorldTriangleMultisetValid = CapturedWorldTriangleMaterialDigest(Vertices,
		Triangles, MaterialIndices, Fixture.GetVolume(),
		Out.CapturedWorldTriangleMaterialDigest, CapturedTriangleDigestError);
	Out.CaptureMultisetWallSeconds = FPlatformTime::Seconds() - MultisetStarted;
	for (const int32 MaterialIndex : MaterialIndices) if (MaterialIndex != 0) ++Out.NonZeroMaterialIndices;
	Out.bCaptureGeometryExact = Out.bCaptureSucceeded && Out.CapturedTriangles == Out.ExpectedTriangles
		&& Out.CapturedMaterialIndices == Out.ExpectedTriangles && Out.NonZeroMaterialIndices == 0
		&& Out.CapturedVertices > 0 && Out.bCapturedWorldTriangleMultisetValid;
	Out.MemorySamples.Add(ReadMemory(TEXT("post_capture"), CaseStarted));
	Vertices.Empty();
	Triangles.Empty();
	MaterialIndices.Empty();
	if (IsBelowResourceGuard(Out.MemorySamples.Last()))
	{
		bOutResourceGuard = true;
		Error = TEXT("Available system RAM fell below the 8 GiB guard after CaptureScene.");
		Out.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}
	if (!Out.bCaptureGeometryExact)
	{
		Error = FString::Printf(TEXT("CaptureScene result mismatch: success=%d vertices=%d triangles=%d/%lld material_indices=%d/%lld nonzero=%d semantic_error=%s."),
			Out.bCaptureSucceeded ? 1 : 0, Out.CapturedVertices, Out.CapturedTriangles,
			Out.ExpectedTriangles, Out.CapturedMaterialIndices, Out.ExpectedTriangles,
			Out.NonZeroMaterialIndices, *CapturedTriangleDigestError);
		Out.Outcome = TEXT("FAIL_CAPTURE_GEOMETRY");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}

	const double ProbeStarted = FPlatformTime::Seconds();
	Fixture.GetVolume()->GenerateProbes();
	Out.GenerateProbesWallSeconds = FPlatformTime::Seconds() - ProbeStarted;
	Out.GeneratedProbeCount = Fixture.GetVolume()->GeneratedProbes;
	Out.ProductExportedTriangles = Fixture.GetVolume()->ExportedTriangles;
	Out.bProbeGenerationSucceeded = Out.GeneratedProbeCount > 0
		&& Out.ProductExportedTriangles == Out.ExpectedTriangles;
	Out.MemorySamples.Add(ReadMemory(TEXT("post_generate_probes"), CaseStarted));
	if (IsBelowResourceGuard(Out.MemorySamples.Last()))
	{
		bOutResourceGuard = true;
		Error = TEXT("Available system RAM fell below the 8 GiB guard after GenerateProbes.");
		Out.Outcome = TEXT("INCONCLUSIVE_RESOURCE_GUARD");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}
	if (!Out.bProbeGenerationSucceeded)
	{
		Error = FString::Printf(TEXT("GenerateProbes result mismatch: probes=%d exported_triangles=%d expected=%lld status=%s."),
			Out.GeneratedProbeCount, Out.ProductExportedTriangles, Out.ExpectedTriangles,
			*Fixture.GetVolume()->Status);
		Out.Outcome = TEXT("FAIL_PROBE_GENERATION");
		Out.Error = Error;
		Fixture.Cleanup();
		return false;
	}

	Out.Outcome = TEXT("PASS_CAPTURE_AND_PROBES");
	Out.MemorySamples.Add(ReadMemory(TEXT("case_complete"), CaseStarted));
	Fixture.Cleanup();
	return true;
}

template<typename TWriter>
static void WriteMemorySample(TWriter& Writer, const FIMMemorySample& Sample)
{
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stage"), Sample.Stage);
	Writer->WriteValue(TEXT("elapsed_seconds"), Sample.ElapsedSeconds);
	Writer->WriteValue(TEXT("used_physical_bytes"), static_cast<double>(Sample.UsedPhysical));
	Writer->WriteValue(TEXT("process_peak_used_physical_bytes"), static_cast<double>(Sample.PeakUsedPhysical));
	Writer->WriteValue(TEXT("used_virtual_bytes"), static_cast<double>(Sample.UsedVirtual));
	Writer->WriteValue(TEXT("process_peak_used_virtual_bytes"), static_cast<double>(Sample.PeakUsedVirtual));
	Writer->WriteValue(TEXT("available_system_physical_bytes"), static_cast<double>(Sample.AvailablePhysical));
	Writer->WriteObjectEnd();
}

template<typename TWriter>
static void WriteCase(TWriter& Writer, const FIMCase& Case)
{
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("transient_unique_mesh_uobjects_requested"), Case.UniqueMeshCount);
	Writer->WriteValue(TEXT("outcome"), Case.Outcome);
	Writer->WriteValue(TEXT("error"), Case.Error);
	Writer->WriteValue(TEXT("placements_requested"), Case.PlacementsRequested);
	Writer->WriteValue(TEXT("placements_built"), Case.PlacementsBuilt);
	Writer->WriteValue(TEXT("static_mesh_components"), Case.StaticMeshComponents);
	Writer->WriteValue(TEXT("distinct_mesh_pointers"), Case.DistinctMeshPointers);
	Writer->WriteValue(TEXT("distinct_mesh_paths"), Case.DistinctMeshPaths);
	Writer->WriteValue(TEXT("distinct_mesh_unique_ids"), Case.DistinctMeshUniqueIds);
	Writer->WriteValue(TEXT("mesh_lod0_cpu_readbacks"), Case.MeshLODsCPUReadable);
	Writer->WriteValue(TEXT("mesh_identity_exact"), Case.bMeshIdentityExact);
	Writer->WriteValue(TEXT("mesh_oriented_triangle_material_multiset_exact_against_source_lod0"), Case.bMeshContentExact);
	Writer->WriteValue(TEXT("source_mesh_lod0_serialized_digest_sha1_diagnostic"), Case.SourceMeshDigest);
	Writer->WriteValue(TEXT("common_clone_lod0_serialized_digest_sha1_diagnostic"), Case.CommonMeshDigest);
	Writer->WriteValue(TEXT("source_oriented_triangle_material_multiset_sha1"), Case.SourceOrientedTriangleMaterialDigest);
	Writer->WriteValue(TEXT("common_clone_oriented_triangle_material_multiset_sha1"), Case.CommonCloneOrientedTriangleMaterialDigest);
	Writer->WriteValue(TEXT("common_clone_section_fingerprint_sha1_diagnostic"), Case.SectionFingerprint);
	Writer->WriteValue(TEXT("section_layout_matches_source_diagnostic"), Case.bSectionLayoutMatchesSource);
	Writer->WriteValue(TEXT("source_lod0_triangles_per_mesh"), Case.SourceLOD0Triangles);
	Writer->WriteValue(TEXT("expected_capture_triangles"), static_cast<double>(Case.ExpectedTriangles));
	Writer->WriteValue(TEXT("mesh_frequency_min"), Case.MinimumPlacementsPerMesh);
	Writer->WriteValue(TEXT("mesh_frequency_max"), Case.MaximumPlacementsPerMesh);
	Writer->WriteValue(TEXT("mesh_frequency_exact"), Case.bFrequencyExact);
	Writer->WriteValue(TEXT("expected_transform_fingerprint_sha1"), Case.ExpectedTransformFingerprint);
	Writer->WriteValue(TEXT("transform_fingerprint_sha1"), Case.TransformFingerprint);
	Writer->WriteValue(TEXT("transforms_matched_within_cm_and_component_tolerance"), Case.ActualTransformsMatched);
	Writer->WriteValue(TEXT("transform_tolerance"), TransformTolerance);
	Writer->WriteValue(TEXT("transforms_exact"), Case.bTransformsExact);
	Writer->WriteValue(TEXT("effective_materials_matched"), Case.ActualMaterialsMatched);
	Writer->WriteValue(TEXT("materials_exact"), Case.bMaterialsExact);
	Writer->WriteValue(TEXT("static_mobility_exact"), Case.bStaticMobilityExact);
	Writer->WriteValue(TEXT("capture_succeeded"), Case.bCaptureSucceeded);
	Writer->WriteValue(TEXT("captured_vertices"), Case.CapturedVertices);
	Writer->WriteValue(TEXT("captured_triangles"), Case.CapturedTriangles);
	Writer->WriteValue(TEXT("captured_material_indices"), Case.CapturedMaterialIndices);
	Writer->WriteValue(TEXT("nonzero_material_indices"), Case.NonZeroMaterialIndices);
	Writer->WriteValue(TEXT("capture_geometry_exact"), Case.bCaptureGeometryExact);
	Writer->WriteValue(TEXT("captured_world_oriented_triangle_material_multiset_valid"), Case.bCapturedWorldTriangleMultisetValid);
	Writer->WriteValue(TEXT("captured_world_oriented_triangle_material_multiset_sha1"), Case.CapturedWorldTriangleMaterialDigest);
	Writer->WriteValue(TEXT("captured_arrays_digest_sha1_diagnostic"), Case.CapturedArrayDigest);
	Writer->WriteValue(TEXT("scene_fingerprint"), Case.SceneFingerprint);
	Writer->WriteValue(TEXT("generated_probe_count"), Case.GeneratedProbeCount);
	Writer->WriteValue(TEXT("product_exported_triangles"), Case.ProductExportedTriangles);
	Writer->WriteValue(TEXT("probe_generation_succeeded"), Case.bProbeGenerationSucceeded);
	Writer->WriteValue(TEXT("mesh_build_wall_seconds"), Case.MeshBuildWallSeconds);
	Writer->WriteValue(TEXT("placement_wall_seconds"), Case.PlacementWallSeconds);
	Writer->WriteValue(TEXT("capture_accessor_wall_seconds"), Case.CaptureAccessorWallSeconds);
	Writer->WriteValue(TEXT("capture_multiset_canonicalization_wall_seconds"), Case.CaptureMultisetWallSeconds);
	Writer->WriteValue(TEXT("generate_probes_wall_seconds"), Case.GenerateProbesWallSeconds);
	Writer->WriteArrayStart(TEXT("memory_samples"));
	for (const FIMMemorySample& Sample : Case.MemorySamples) WriteMemorySample(Writer, Sample);
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
}

static bool WriteReport(const FString& ReportPath, const FString& RunId, const FString& PairName,
	const FString& FinalResult, const FIMSource& Source, const FString& FinalMapIdentity,
	const FString& FinalBakeAssetIdentity, const bool bMapRestored, const bool bAssetsUnchanged,
	const bool bResourceGuardHit, const bool bProbeCountsEqual, const bool bSceneFingerprintsEqual,
	const bool bCapturedArraysEqual, const bool bMeshSemanticEqual,
	const bool bCapturedWorldTriangleMultisetsEqual, const FIMCase& Baseline, const FIMCase& Candidate,
	const FString& FinalReason)
{
	FString Json;
	auto Writer = TJsonWriterFactory<>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema"), TEXT("im-acoustic-g2-unique-mesh-identity-v3"));
	Writer->WriteValue(TEXT("run_id"), RunId);
	Writer->WriteValue(TEXT("comparison"), PairName);
	Writer->WriteValue(TEXT("final_result"), FinalResult);
	Writer->WriteValue(TEXT("final_reason"), FinalReason);
	Writer->WriteValue(TEXT("scope"), TEXT("CaptureScene and GenerateProbes only; transient identity-only UStaticMesh fixtures; no SDK Bake, persistent payload commit, or cold runtime validation."));
	Writer->WriteValue(TEXT("mesh_fixture_persistence"), TEXT("Every generated UStaticMesh is RF_Transient under GetTransientPackage; no package is saved. Counts measure in-memory UObject identities only and do not represent persisted/on-disk mesh asset scale."));
	Writer->WriteValue(TEXT("geometry_equivalence_method"), TEXT("Compare sorted world-space oriented triangle plus effective acoustic material multisets. Keys quantize each coordinate to 0.0001 cm, canonicalize cyclic vertex rotations only, preserve winding, include material index and material path, and retain duplicate triangle multiplicity. Raw arrays, serialized mesh digests, section fingerprints, and product scene fingerprints are recorded as representation diagnostics because render-LOD rebuilds may repack vertices/indices."));
	Writer->WriteValue(TEXT("map_path"), MapPath);
	Writer->WriteValue(TEXT("world_package"), Source.WorldPackage);
	Writer->WriteValue(TEXT("baseline_map_identity"), Source.MapIdentity);
	Writer->WriteValue(TEXT("final_map_identity"), FinalMapIdentity);
	Writer->WriteValue(TEXT("original_map_restored"), bMapRestored);
	Writer->WriteValue(TEXT("original_bake_asset_identity"), Source.OriginalBakeAssetIdentity);
	Writer->WriteValue(TEXT("final_bake_asset_identity"), FinalBakeAssetIdentity);
	Writer->WriteValue(TEXT("bake_asset_identity_storage_basis"), TEXT("Serialized SceneData and ProbeData plus metadata; Format 2 stores raw sections and Format 3 stores packed sections."));
	Writer->WriteValue(TEXT("original_bake_assets_unchanged"), bAssetsUnchanged);
	Writer->WriteValue(TEXT("original_bake_volume_count"), Source.OriginalBakeVolumeCount);
	Writer->WriteValue(TEXT("whitebox_identity"), Source.WhiteboxIdentity);
	Writer->WriteValue(TEXT("whitebox_components"), Source.WhiteboxComponentCount);
	Writer->WriteValue(TEXT("whitebox_unique_source_mesh_assets"), Source.UniqueWhiteboxMeshCount);
	Writer->WriteValue(TEXT("source_mesh_path"), Source.MeshPath);
	Writer->WriteValue(TEXT("source_effective_material_path"), Source.MaterialPath);
	Writer->WriteValue(TEXT("source_lod"), 0);
	Writer->WriteValue(TEXT("source_lod0_vertices"), Source.LOD0Vertices);
	Writer->WriteValue(TEXT("source_lod0_triangles"), Source.LOD0Triangles);
	Writer->WriteValue(TEXT("source_lod0_cpu_content_digest_sha1"), Source.LOD0Digest);
	Writer->WriteValue(TEXT("source_lod0_section_fingerprint_sha1"), Source.SectionFingerprint);
	Writer->WriteValue(TEXT("source_lod0_oriented_triangle_material_multiset_sha1"), Source.OrientedTriangleMaterialDigest);
	Writer->WriteValue(TEXT("placements_per_case"), PlacementCount);
	Writer->WriteValue(TEXT("expected_triangles_per_case"), static_cast<double>(ExpectedTotalTriangles));
	Writer->WriteValue(TEXT("placement_region_width_cm"), RegionWidthCm);
	Writer->WriteValue(TEXT("placement_region_height_cm"), RegionHeightCm);
	Writer->WriteValue(TEXT("placement_region_half_depth_cm"), RegionHalfDepthCm);
	Writer->WriteValue(TEXT("probe_spacing_cm"), ProbeSpacingCm);
	Writer->WriteValue(TEXT("probe_height_cm"), ProbeHeightCm);
	Writer->WriteValue(TEXT("material_absorption"), MaterialAbsorption);
	Writer->WriteValue(TEXT("material_scattering"), MaterialScattering);
	Writer->WriteValue(TEXT("mesh_assignment"), TEXT("placement_index modulo M; M divides 3000 so every unique mesh receives exactly 3000/M placements."));
	Writer->WriteValue(TEXT("mesh_fixture_api"), TEXT("NewObject<UStaticMesh> + ExportStaticMeshLOD(source LOD0) + BuildFromMeshDescriptions; bAllowCPUAccess and bAllowCpuAccess enabled; RF_Transient; no DuplicateObject."));
	Writer->WriteValue(TEXT("unique_mesh_uobject_identity_scales"), TEXT("only transient UObject mesh identity/count varies; LOD0 content, static material slot, component mobility, transforms, region and probe configuration are held constant."));
	Writer->WriteValue(TEXT("comparison_probe_counts_equal"), bProbeCountsEqual);
	Writer->WriteValue(TEXT("comparison_scene_fingerprints_equal_diagnostic"), bSceneFingerprintsEqual);
	Writer->WriteValue(TEXT("comparison_captured_arrays_equal_diagnostic"), bCapturedArraysEqual);
	Writer->WriteValue(TEXT("comparison_mesh_oriented_triangle_material_multisets_equal"), bMeshSemanticEqual);
	Writer->WriteValue(TEXT("comparison_captured_world_oriented_triangle_material_multisets_equal"), bCapturedWorldTriangleMultisetsEqual);
	Writer->WriteValue(TEXT("resource_guard_hit"), bResourceGuardHit);
	Writer->WriteValue(TEXT("resource_guard_bytes_available_minimum"), static_cast<double>(ResourceGuardBytes));
	Writer->WriteValue(TEXT("memory_semantics"), TEXT("FPlatformMemoryStats process Used/Peak values and system AvailablePhysical; sampled peaks are lower bounds, while Peak values are process-lifetime highs."));
	uint64 ProcessPeakPhysical = 0;
	uint64 ProcessPeakVirtual = 0;
	uint64 HighestSampledPhysical = 0;
	uint64 HighestSampledVirtual = 0;
	uint64 MinimumAvailablePhysical = MAX_uint64;
	uint64 InitialPeakPhysical = 0;
	uint64 InitialPeakVirtual = 0;
	bool bHasInitialPeak = false;
	auto AccumulateMemory = [&](const FIMMemorySample& Sample)
	{
		ProcessPeakPhysical = FMath::Max(ProcessPeakPhysical, Sample.PeakUsedPhysical);
		ProcessPeakVirtual = FMath::Max(ProcessPeakVirtual, Sample.PeakUsedVirtual);
		HighestSampledPhysical = FMath::Max(HighestSampledPhysical, Sample.UsedPhysical);
		HighestSampledVirtual = FMath::Max(HighestSampledVirtual, Sample.UsedVirtual);
		MinimumAvailablePhysical = FMath::Min(MinimumAvailablePhysical, Sample.AvailablePhysical);
		if (!bHasInitialPeak)
		{
			InitialPeakPhysical = Sample.PeakUsedPhysical;
			InitialPeakVirtual = Sample.PeakUsedVirtual;
			bHasInitialPeak = true;
		}
	};
	for (const FIMMemorySample& Sample : Baseline.MemorySamples) AccumulateMemory(Sample);
	for (const FIMMemorySample& Sample : Candidate.MemorySamples) AccumulateMemory(Sample);
	Writer->WriteObjectStart(TEXT("memory_summary"));
	Writer->WriteValue(TEXT("process_peak_used_physical_bytes_observed"), static_cast<double>(ProcessPeakPhysical));
	Writer->WriteValue(TEXT("process_peak_used_virtual_bytes_observed"), static_cast<double>(ProcessPeakVirtual));
	Writer->WriteValue(TEXT("highest_sampled_used_physical_bytes"), static_cast<double>(HighestSampledPhysical));
	Writer->WriteValue(TEXT("highest_sampled_used_virtual_bytes"), static_cast<double>(HighestSampledVirtual));
	Writer->WriteValue(TEXT("lowest_sampled_available_system_physical_bytes"),
		MinimumAvailablePhysical == MAX_uint64 ? 0.0 : static_cast<double>(MinimumAvailablePhysical));
	Writer->WriteValue(TEXT("process_peak_physical_delta_since_case_start_bytes"),
		static_cast<double>(ProcessPeakPhysical >= InitialPeakPhysical ? ProcessPeakPhysical - InitialPeakPhysical : 0));
	Writer->WriteValue(TEXT("process_peak_virtual_delta_since_case_start_bytes"),
		static_cast<double>(ProcessPeakVirtual >= InitialPeakVirtual ? ProcessPeakVirtual - InitialPeakVirtual : 0));
	Writer->WriteValue(TEXT("observed_process_peak_values_are_lifetime_highs"), true);
	Writer->WriteObjectEnd();
	Writer->WriteArrayStart(TEXT("cases"));
	WriteCase(Writer, Baseline);
	WriteCase(Writer, Candidate);
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("not_covered"));
	Writer->WriteValue(TEXT("SDK Bake/commit cost, bake payload size, cold runtime loading, acoustic quality, and production maps."));
	Writer->WriteValue(TEXT("Any conclusion beyond the W2 repeated-Cube 3000-placement capture/probe fixture, including thousands of unique meshes."));
	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("report_path"), ReportPath);
	Writer->WriteObjectEnd();
	Writer->Close();
	return IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true)
		&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

static bool RunComparison(FAutomationTestBase* Test, const FString& PairName,
	const int32 CandidateUniqueMeshCount, const TCHAR* MarkerName)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AcousticV2/G2UniqueMesh"), RunId);
	const FString ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
	FIMFixture Fixture;
	FIMSource InitialSource;
	FString Error;
	bool bResourceGuardHit = false;
	bool bPrimed = Fixture.Prime(InitialSource, Error);
	FIMCase Baseline;
	FIMCase Candidate;
	bool bBaselinePassed = false;
	bool bCandidatePassed = false;
	if (!bPrimed)
	{
		Baseline.Outcome = TEXT("FAIL_INITIAL_MAP");
		Baseline.Error = Error;
		Candidate.Outcome = TEXT("NOT_RUN");
		Candidate.UniqueMeshCount = CandidateUniqueMeshCount;
	}
	else
	{
		Baseline.UniqueMeshCount = 1;
		bBaselinePassed = RunCase(Fixture, InitialSource, 1, RunId, Baseline, bResourceGuardHit, Error);
		if (bBaselinePassed)
		{
			bCandidatePassed = RunCase(Fixture, InitialSource, CandidateUniqueMeshCount,
				RunId, Candidate, bResourceGuardHit, Error);
		}
		else
		{
			Candidate.UniqueMeshCount = CandidateUniqueMeshCount;
			Candidate.Outcome = TEXT("NOT_RUN_BASELINE_FAILED");
			Candidate.Error = TEXT("Candidate case was skipped because M=1 did not pass.");
		}
	}

	const bool bProbeCountsEqual = bBaselinePassed && bCandidatePassed
		&& Baseline.GeneratedProbeCount == Candidate.GeneratedProbeCount;
	const bool bSceneFingerprintsEqual = bBaselinePassed && bCandidatePassed
		&& Baseline.SceneFingerprint == Candidate.SceneFingerprint;
	const bool bCapturedArraysEqual = bBaselinePassed && bCandidatePassed
		&& Baseline.CapturedArrayDigest == Candidate.CapturedArrayDigest;
	const bool bMeshSemanticEqual = bBaselinePassed && bCandidatePassed
		&& Baseline.bMeshContentExact && Candidate.bMeshContentExact
		&& Baseline.CommonCloneOrientedTriangleMaterialDigest == Candidate.CommonCloneOrientedTriangleMaterialDigest;
	const bool bCapturedWorldTriangleMultisetsEqual = bBaselinePassed && bCandidatePassed
		&& Baseline.bCapturedWorldTriangleMultisetValid && Candidate.bCapturedWorldTriangleMultisetValid
		&& Baseline.CapturedWorldTriangleMaterialDigest == Candidate.CapturedWorldTriangleMaterialDigest;
	const bool bCaptureAndProbeEquivalent = bProbeCountsEqual && bMeshSemanticEqual
		&& bCapturedWorldTriangleMultisetsEqual
		&& Baseline.CapturedTriangles == Candidate.CapturedTriangles
		&& Baseline.CapturedMaterialIndices == Candidate.CapturedMaterialIndices
		&& Baseline.TransformFingerprint == Candidate.TransformFingerprint
		&& Baseline.ActualMaterialsMatched == PlacementCount
		&& Candidate.ActualMaterialsMatched == PlacementCount;

	FString FinalMapIdentity;
	FString FinalAssetIdentity;
	FString RestoreError;
	const bool bMapRestored = bPrimed && Fixture.RestoreAndCompare(FinalMapIdentity, FinalAssetIdentity, RestoreError);
	const bool bAssetsUnchanged = bMapRestored && FinalAssetIdentity == InitialSource.OriginalBakeAssetIdentity;
	const bool bPassed = bBaselinePassed && bCandidatePassed && bCaptureAndProbeEquivalent
		&& bMapRestored && bAssetsUnchanged;
	const FString FinalResult = bPassed ? TEXT("PASS_CAPTURE_PROBE_IDENTITY_EQUIVALENCE")
		: (bResourceGuardHit ? TEXT("INCONCLUSIVE_RESOURCE_GUARD") : TEXT("FAIL"));
	FString FinalReason;
	if (!bBaselinePassed) FinalReason = Baseline.Error;
	else if (!bCandidatePassed) FinalReason = Candidate.Error;
	else if (!bCaptureAndProbeEquivalent) FinalReason = TEXT("M=1 and candidate differed in probe count, source/clone semantic mesh content, world-space oriented triangle/material multiset, triangle/material counts, transforms, or effective material validation; raw capture arrays and product fingerprints are diagnostics only.");
	else if (!bMapRestored) FinalReason = RestoreError.IsEmpty() ? TEXT("Original W2 map identity did not restore exactly.") : RestoreError;
	else if (!bAssetsUnchanged) FinalReason = TEXT("Original bake asset identity changed after the transient test.");
	else FinalReason = TEXT("All fixed-workload identity-only checks passed.");

	const bool bReportSaved = WriteReport(ReportPath, RunId, PairName, FinalResult,
		InitialSource, FinalMapIdentity, FinalAssetIdentity, bMapRestored, bAssetsUnchanged,
		bResourceGuardHit, bProbeCountsEqual, bSceneFingerprintsEqual, bCapturedArraysEqual,
		bMeshSemanticEqual, bCapturedWorldTriangleMultisetsEqual, Baseline, Candidate, FinalReason);
	if (!bReportSaved)
	{
		FinalReason += TEXT("; JSON report could not be saved.");
	}
	Test->AddInfo(FString::Printf(TEXT("G2 unique-mesh report: %s"), *ReportPath));
	if (!bPassed || !bReportSaved)
	{
		Test->AddError(FString::Printf(TEXT("G2 unique-mesh %s did not pass: %s; map_restored=%d assets_unchanged=%d report_saved=%d."),
			*PairName, *FinalReason, bMapRestored ? 1 : 0, bAssetsUnchanged ? 1 : 0, bReportSaved ? 1 : 0));
		return false;
	}
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] %s PASS"), MarkerName);
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS %s"), MarkerName);
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2UniqueMeshM1M10Test,
	"IceMoon.AcousticField.G2.UniqueMeshIdentity.M01to010",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2UniqueMeshM1M10Test::RunTest(const FString&)
{
	return IMAcousticG2UniqueMeshTestPrivate::RunComparison(this, TEXT("M1_vs_M10"), 10,
		TEXT("G2UniqueMeshIdentityM1vsM10"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2UniqueMeshM1M100Test,
	"IceMoon.AcousticField.G2.UniqueMeshIdentity.M01to100",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2UniqueMeshM1M100Test::RunTest(const FString&)
{
	return IMAcousticG2UniqueMeshTestPrivate::RunComparison(this, TEXT("M1_vs_M100"), 100,
		TEXT("G2UniqueMeshIdentityM1vsM100"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2UniqueMeshM1M1000Test,
	"IceMoon.AcousticField.G2.UniqueMeshIdentity.M01to1K",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2UniqueMeshM1M1000Test::RunTest(const FString&)
{
	return IMAcousticG2UniqueMeshTestPrivate::RunComparison(this, TEXT("M1_vs_M1000"), 1000,
		TEXT("G2UniqueMeshIdentityM01to1K"));
}
#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
