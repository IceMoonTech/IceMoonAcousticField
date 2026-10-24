#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

namespace IMAcousticPhysicalMaterialTestPrivate
{
constexpr const TCHAR* IMPMMap = TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
constexpr const TCHAR* IMPMIgnoreTag = TEXT("IMAcousticIgnore");
constexpr double IMPMCoordinateToleranceMeters = 0.0005;

struct FIMPMCapture
{
	TArray<FVector> Vertices;
	TArray<FIntVector> Triangles;
	TArray<int32> MaterialIndices;
	FString Fingerprint;
};

struct FIMPMCentroidAccumulator
{
	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;

	void Add(const FVector& Value)
	{
		Sum += Value;
		++Count;
	}

	FVector Mean() const
	{
		return Count > 0 ? Sum / double(Count) : FVector::ZeroVector;
	}
};

struct FIMPMGeometrySummary
{
	TArray<FString> TriangleKeys;
	TMap<int32, int32> TriangleCounts;
	TMap<int32, FIMPMCentroidAccumulator> Centroids;
};

struct FIMPMExpectedObject
{
	UStaticMesh* Mesh = nullptr;
	FTransform WorldTransform = FTransform::Identity;
	// Indexed by FStaticMeshSection::MaterialIndex; values are profile-table indices.
	TArray<int32> ProfileByMaterialSlot;
};

struct FIMPMCaseResult
{
	FString Name;
	bool bPassed = false;
	int32 ExpectedTriangles = INDEX_NONE;
	int32 ActualTriangles = INDEX_NONE;
	int32 ExpectedNonSolidSections = INDEX_NONE;
	int32 ActualNonSolidSections = INDEX_NONE;
	int32 ExpectedNonSolidTriangles = INDEX_NONE;
	int32 ActualNonSolidTriangles = INDEX_NONE;
	FString Fingerprint;
	FString Details;
};

static FString IMPMHashStrings(TArray<FString> Items)
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

static FString IMPMReadMapIdentity(UWorld* World, UStaticMesh*& OutMesh,
	UMaterialInterface*& OutBaseMaterial, int32& OutWhiteboxComponents,
	int32& OutUniqueMeshes)
{
	OutMesh = nullptr;
	OutBaseMaterial = nullptr;
	OutWhiteboxComponents = 0;
	OutUniqueMeshes = 0;
	TSet<UStaticMesh*> UniqueMeshes;
	bool bUniformMaterial = true;
	TArray<FString> IdentityParts;

	if (!World)
	{
		return FString();
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->ActorHasTag(TEXT("IMAcousticWhitebox")))
		{
			continue;
		}

		TArray<UStaticMeshComponent*> Components;
		It->GetComponents(Components);
		for (UStaticMeshComponent* Component : Components)
		{
			UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			if (!Mesh)
			{
				continue;
			}

			++OutWhiteboxComponents;
			UniqueMeshes.Add(Mesh);
			if (!OutMesh)
			{
				OutMesh = Mesh;
			}
			if (OutWhiteboxComponents == 1)
			{
				OutBaseMaterial = Component->GetMaterial(0);
			}
			else if (OutBaseMaterial != Component->GetMaterial(0))
			{
				bUniformMaterial = false;
			}

			IdentityParts.Add(Component->GetPathName() + TEXT("|")
				+ GetPathNameSafe(Mesh) + TEXT("|")
				+ Component->GetComponentTransform().ToString() + TEXT("|")
				+ GetPathNameSafe(Component->GetMaterial(0)));
		}
	}

	OutUniqueMeshes = UniqueMeshes.Num();
	if (!bUniformMaterial)
	{
		OutBaseMaterial = nullptr;
	}
	return World->GetOutermost()->GetName() + TEXT("|") + IMPMHashStrings(MoveTemp(IdentityParts));
}

static FIntVector IMPMQuantizeSDKPoint(const FVector& Point)
{
	return FIntVector(
		FMath::RoundToInt(Point.X / IMPMCoordinateToleranceMeters),
		FMath::RoundToInt(Point.Y / IMPMCoordinateToleranceMeters),
		FMath::RoundToInt(Point.Z / IMPMCoordinateToleranceMeters));
}

static bool IMPMPointLess(const FIntVector& A, const FIntVector& B)
{
	if (A.X != B.X)
	{
		return A.X < B.X;
	}
	if (A.Y != B.Y)
	{
		return A.Y < B.Y;
	}
	return A.Z < B.Z;
}

static bool IMPMRotationLess(const FIntVector Points[3], int32 A, int32 B)
{
	for (int32 Offset = 0; Offset < 3; ++Offset)
	{
		const FIntVector& PA = Points[(A + Offset) % 3];
		const FIntVector& PB = Points[(B + Offset) % 3];
		if (PA == PB)
		{
			continue;
		}
		return IMPMPointLess(PA, PB);
	}
	return false;
}

static FString IMPMTriangleKey(const FVector& A, const FVector& B,
	const FVector& C, int32 MaterialIndex)
{
	const FIntVector Points[3] = {
		IMPMQuantizeSDKPoint(A), IMPMQuantizeSDKPoint(B), IMPMQuantizeSDKPoint(C)
	};
	int32 First = 0;
	for (int32 Candidate = 1; Candidate < 3; ++Candidate)
	{
		if (IMPMRotationLess(Points, Candidate, First))
		{
			First = Candidate;
		}
	}

	const FIntVector& P0 = Points[First];
	const FIntVector& P1 = Points[(First + 1) % 3];
	const FIntVector& P2 = Points[(First + 2) % 3];
	return FString::Printf(TEXT("%d|%d,%d,%d>%d,%d,%d>%d,%d,%d"), MaterialIndex,
		P0.X, P0.Y, P0.Z, P1.X, P1.Y, P1.Z, P2.X, P2.Y, P2.Z);
}

static FVector IMPMToSDKPosition(const FVector& WorldPosition, const FVector& Origin)
{
	const FVector DeltaMeters = (WorldPosition - Origin) * 0.01;
	// UE centimetres and XYZ become Steam Audio metres with XYZ=(UE Y, UE Z, -UE X).
	return FVector(DeltaMeters.Y, DeltaMeters.Z, -DeltaMeters.X);
}

static bool IMPMAppendExpectedObject(const FIMPMExpectedObject& Expected,
	const FVector& Origin, const TArray<bool>& SolidByProfile,
	FIMPMGeometrySummary& InOutSummary, int32& InOutNonSolidSections,
	int32& InOutNonSolidTriangles, FString& OutError)
{
	if (!Expected.Mesh || !Expected.Mesh->GetRenderData()
		|| Expected.Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		OutError = TEXT("Expected oracle mesh has no CPU-readable LOD0.");
		return false;
	}

	const FStaticMeshLODResources& LOD = Expected.Mesh->GetRenderData()->LODResources[0];
	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		if (!Expected.ProfileByMaterialSlot.IsValidIndex(Section.MaterialIndex))
		{
			OutError = FString::Printf(TEXT("LOD0 section material index %d is outside the fixture slot map."),
				Section.MaterialIndex);
			return false;
		}
		const int32 ProfileIndex = Expected.ProfileByMaterialSlot[Section.MaterialIndex];
		if (!SolidByProfile.IsValidIndex(ProfileIndex))
		{
			OutError = FString::Printf(TEXT("Expected profile index %d is outside the fixture profile table."),
				ProfileIndex);
			return false;
		}
		if (!SolidByProfile[ProfileIndex])
		{
			++InOutNonSolidSections;
			InOutNonSolidTriangles += int32(Section.NumTriangles);
			continue;
		}

		for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
		{
			FVector SDKPoints[3];
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Index = LOD.IndexBuffer.GetIndex(Section.FirstIndex + Triangle * 3 + Corner);
				if (Index >= LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices())
				{
					OutError = TEXT("LOD0 index buffer points outside the source position buffer.");
					return false;
				}
				const FVector LocalPosition(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Index));
				SDKPoints[Corner] = IMPMToSDKPosition(
					Expected.WorldTransform.TransformPosition(LocalPosition), Origin);
			}

			// A reflection reverses triangle winding. The exported scene preserves outward orientation.
			if (Expected.WorldTransform.GetDeterminant() < 0.0)
			{
				Swap(SDKPoints[1], SDKPoints[2]);
			}

			InOutSummary.TriangleKeys.Add(IMPMTriangleKey(
				SDKPoints[0], SDKPoints[1], SDKPoints[2], ProfileIndex));
			++InOutSummary.TriangleCounts.FindOrAdd(ProfileIndex);
			InOutSummary.Centroids.FindOrAdd(ProfileIndex).Add(
				(SDKPoints[0] + SDKPoints[1] + SDKPoints[2]) / 3.0);
		}
	}
	return true;
}

static bool IMPMBuildExpected(const TArray<FIMPMExpectedObject>& Objects,
	const FVector& Origin, const TArray<bool>& SolidByProfile,
	FIMPMGeometrySummary& OutSummary, int32& OutNonSolidSections,
	int32& OutNonSolidTriangles, FString& OutError)
{
	OutSummary = {};
	OutNonSolidSections = 0;
	OutNonSolidTriangles = 0;
	for (const FIMPMExpectedObject& Object : Objects)
	{
		if (!IMPMAppendExpectedObject(Object, Origin, SolidByProfile, OutSummary,
			OutNonSolidSections, OutNonSolidTriangles, OutError))
		{
			return false;
		}
	}
	OutSummary.TriangleKeys.Sort();
	return true;
}

static bool IMPMSummarizeCapture(const FIMPMCapture& Capture,
	FIMPMGeometrySummary& OutSummary, FString& OutError)
{
	OutSummary = {};
	if (Capture.Triangles.Num() != Capture.MaterialIndices.Num())
	{
		OutError = TEXT("Captured triangle/material-index cardinality mismatch.");
		return false;
	}

	OutSummary.TriangleKeys.Reserve(Capture.Triangles.Num());
	for (int32 TriangleIndex = 0; TriangleIndex < Capture.Triangles.Num(); ++TriangleIndex)
	{
		const FIntVector Triangle = Capture.Triangles[TriangleIndex];
		if (!Capture.Vertices.IsValidIndex(Triangle.X)
			|| !Capture.Vertices.IsValidIndex(Triangle.Y)
			|| !Capture.Vertices.IsValidIndex(Triangle.Z))
		{
			OutError = FString::Printf(TEXT("Captured triangle %d has an invalid vertex index."), TriangleIndex);
			return false;
		}
		const FVector& A = Capture.Vertices[Triangle.X];
		const FVector& B = Capture.Vertices[Triangle.Y];
		const FVector& C = Capture.Vertices[Triangle.Z];
		const int32 MaterialIndex = Capture.MaterialIndices[TriangleIndex];
		OutSummary.TriangleKeys.Add(IMPMTriangleKey(A, B, C, MaterialIndex));
		++OutSummary.TriangleCounts.FindOrAdd(MaterialIndex);
		OutSummary.Centroids.FindOrAdd(MaterialIndex).Add((A + B + C) / 3.0);
	}
	OutSummary.TriangleKeys.Sort();
	return true;
}

static bool IMPMCompareGeometry(const FIMPMGeometrySummary& Expected,
	const FIMPMGeometrySummary& Actual, FString& OutError)
{
	if (Expected.TriangleKeys != Actual.TriangleKeys)
	{
		OutError = FString::Printf(TEXT("Independent LOD0 oracle mismatch: expected %d oriented triangles, captured %d."),
			Expected.TriangleKeys.Num(), Actual.TriangleKeys.Num());
		return false;
	}
	if (Expected.TriangleCounts.Num() != Actual.TriangleCounts.Num())
	{
		OutError = TEXT("Independent per-profile triangle-count table has different material-index keys.");
		return false;
	}

	for (const TPair<int32, int32>& Pair : Expected.TriangleCounts)
	{
		const int32* ActualCount = Actual.TriangleCounts.Find(Pair.Key);
		const FIMPMCentroidAccumulator* ExpectedCenter = Expected.Centroids.Find(Pair.Key);
		const FIMPMCentroidAccumulator* ActualCenter = Actual.Centroids.Find(Pair.Key);
		if (!ActualCount || *ActualCount != Pair.Value || !ExpectedCenter || !ActualCenter)
		{
			OutError = FString::Printf(TEXT("Profile/material index %d triangle count differs (expected %d, actual %d)."),
				Pair.Key, Pair.Value, ActualCount ? *ActualCount : INDEX_NONE);
			return false;
		}
		if ((ExpectedCenter->Mean() - ActualCenter->Mean()).Size()
			> IMPMCoordinateToleranceMeters)
		{
			OutError = FString::Printf(TEXT("Profile/material index %d SDK triangle-centroid mean differs: expected %s, actual %s."),
				Pair.Key, *ExpectedCenter->Mean().ToString(), *ActualCenter->Mean().ToString());
			return false;
		}
	}
	return true;
}

static FIMAcousticPhysicalMaterialMapping IMPMMakeProfile(
	UPhysicalMaterial* PhysicalMaterial, const FVector& Absorption,
	float Scattering, bool bSolidGeometry = true, bool bSupportsProbes = true)
{
	FIMAcousticPhysicalMaterialMapping Profile;
	Profile.PhysicalMaterial = PhysicalMaterial;
	Profile.Absorption = Absorption;
	Profile.Scattering = Scattering;
	Profile.bSolidGeometry = bSolidGeometry;
	Profile.bSupportsProbes = bSupportsProbes;
	return Profile;
}

static TArray<int32> IMPMUniformSlotMap(UStaticMesh* Mesh, int32 ProfileIndex)
{
	TArray<int32> Result;
	if (Mesh)
	{
		Result.Init(ProfileIndex, Mesh->GetStaticMaterials().Num());
	}
	return Result;
}

static void IMPMAppendCountsAndCentroids(FString& InOutText,
	const TCHAR* Prefix, const FIMPMGeometrySummary& Summary)
{
	TArray<int32> Indices;
	Summary.TriangleCounts.GetKeys(Indices);
	Indices.Sort();
	for (int32 Index : Indices)
	{
		const FIMPMCentroidAccumulator* Centroid = Summary.Centroids.Find(Index);
		InOutText += FString::Printf(TEXT("%s profile_index=%d triangles=%d sdk_centroid_mean_m=%s; "),
			Prefix, Index, Summary.TriangleCounts.FindRef(Index),
			Centroid ? *Centroid->Mean().ToString() : TEXT("missing"));
	}
}

class FIMPMContractFixture
{
public:
	~FIMPMContractFixture()
	{
		CleanupTransientScene();
		RestoreMap();
	}

	bool Begin(FString& OutError)
	{
		if (!FEditorFileUtils::LoadMap(IMPMMap, false, true))
		{
			OutError = TEXT("Could not load the existing acoustic whitebox automation map.");
			return false;
		}
		World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			OutError = TEXT("Editor world is unavailable after loading the whitebox map.");
			return false;
		}

		int32 UniqueMeshes = 0;
		MapIdentity = IMPMReadMapIdentity(World, SourceMesh, BaseMaterial,
			WhiteboxComponents, UniqueMeshes);
		if (WhiteboxComponents != 60 || UniqueMeshes != 1 || !SourceMesh || !BaseMaterial
			|| SourceMesh->GetStaticMaterials().Num() != 1
			|| !SourceMesh->GetRenderData() || SourceMesh->GetRenderData()->LODResources.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Whitebox fixture drift: components=%d unique_meshes=%d mesh=%s material=%s."),
				WhiteboxComponents, UniqueMeshes, *GetPathNameSafe(SourceMesh), *GetPathNameSafe(BaseMaterial));
			return false;
		}
		if (!IMPMTagExistingScene(OutError))
		{
			return false;
		}
		return true;
	}

	bool StartCaptureCase(const TArray<FIMAcousticPhysicalMaterialMapping>& Profiles,
		const FVector& BoundsExtent, FString& OutError)
	{
		CleanupTransientScene();
		if (!World)
		{
			OutError = TEXT("Editor world disappeared before a capture case.");
			return false;
		}

		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Volume = World->SpawnActor<AIMAcousticBakeVolume>(
			FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Volume.IsValid() || !Volume->BakeBounds)
		{
			OutError = TEXT("Could not spawn a transient acoustic volume fixture.");
			return false;
		}
		Volume->SetActorTransform(FTransform::Identity);
		Volume->MaterialDrive = EIMAcousticMaterialDrive::PhysicalMaterial;
		Volume->PhysicalMaterials = Profiles;
		Volume->Materials.Reset();
		Volume->BakeBounds->SetBoxExtent(BoundsExtent);
		return true;
	}

	UPhysicalMaterial* MakePhysicalMaterial(const FName BaseName)
	{
		const FName Name = MakeUniqueObjectName(GetTransientPackage(),
			UPhysicalMaterial::StaticClass(), BaseName);
		UPhysicalMaterial* Material = NewObject<UPhysicalMaterial>(GetTransientPackage(), Name, RF_Transient);
		if (Material)
		{
			OwnedPhysicalMaterials.Emplace(Material);
		}
		return Material;
	}

	UMaterialInstanceConstant* MakeMaterialInstance(const FName BaseName,
		UPhysicalMaterial* PhysicalMaterial)
	{
		const FName Name = MakeUniqueObjectName(GetTransientPackage(),
			UMaterialInstanceConstant::StaticClass(), BaseName);
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
			GetTransientPackage(), Name, RF_Transient);
		if (!Instance)
		{
			return nullptr;
		}
		// This transient MIC provides the actual slot physical material read by GetMaterial(slot)->GetPhysicalMaterial().
		Instance->Parent = BaseMaterial;
		Instance->PhysMaterial = PhysicalMaterial;
		Instance->bOverridePhysMaterial = 1;
		OwnedMaterialInstances.Emplace(Instance);
		return Instance->GetPhysicalMaterial() == PhysicalMaterial ? Instance : nullptr;
	}

	UStaticMeshComponent* AddStatic(const FTransform& WorldTransform,
		UStaticMesh* Mesh, UMaterialInterface* SlotMaterial,
		UPhysicalMaterial* BodyOverride = nullptr)
	{
		if (!World || !Mesh)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		TransientActors.Add(Actor);
		Actor->SetActorTransform(WorldTransform);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		if (!Component)
		{
			return nullptr;
		}
		Component->SetStaticMesh(Mesh);
		if (SlotMaterial)
		{
			for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
			{
				Component->SetMaterial(Slot, SlotMaterial);
			}
		}
		if (BodyOverride)
		{
			Component->SetPhysMaterialOverride(BodyOverride);
		}
		return Component;
	}

	UStaticMeshComponent* AddSectionedStatic(const FTransform& WorldTransform,
		UStaticMesh* Mesh, UMaterialInstanceConstant* LeafMaterial,
		UMaterialInstanceConstant* WoodMaterial, int32 LeafSlot, int32 WoodSlot)
	{
		if (!World || !Mesh || !LeafMaterial || !WoodMaterial)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		TransientActors.Add(Actor);
		Actor->SetActorTransform(WorldTransform);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		if (!Component)
		{
			return nullptr;
		}
		Component->SetStaticMesh(Mesh);
		Component->SetMaterial(LeafSlot, LeafMaterial);
		Component->SetMaterial(WoodSlot, WoodMaterial);
		return Component;
	}

	UInstancedStaticMeshComponent* AddInstanced(bool bHierarchical,
		const FTransform& InstanceTransform, UStaticMesh* Mesh,
		UMaterialInterface* SlotMaterial, UPhysicalMaterial* BodyOverride = nullptr)
	{
		if (!World || !Mesh)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform::Identity, Params);
		if (!Actor)
		{
			return nullptr;
		}
		TransientActors.Add(Actor);

		USceneComponent* Root = NewObject<USceneComponent>(Actor, NAME_None, RF_Transient);
		if (!Root)
		{
			return nullptr;
		}
		Root->SetMobility(EComponentMobility::Static);
		Actor->SetRootComponent(Root);
		Actor->AddInstanceComponent(Root);
		Root->RegisterComponent();

		UInstancedStaticMeshComponent* Component = bHierarchical
			? static_cast<UInstancedStaticMeshComponent*>(NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient))
			: NewObject<UInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient);
		if (!Component)
		{
			return nullptr;
		}
		Actor->AddInstanceComponent(Component);
		Component->SetupAttachment(Root);
		Component->SetStaticMesh(Mesh);
		Component->SetMobility(EComponentMobility::Static);
		if (SlotMaterial)
		{
			for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
			{
				Component->SetMaterial(Slot, SlotMaterial);
			}
		}
		if (BodyOverride)
		{
			Component->SetPhysMaterialOverride(BodyOverride);
		}
		if (Component->AddInstance(InstanceTransform, true) < 0)
		{
			return nullptr;
		}
		Component->RegisterComponent();
		return Component;
	}

	bool Capture(FIMPMCapture& OutCapture, FString& OutError) const
	{
		OutCapture = {};
		if (!Volume.IsValid())
		{
			OutError = TEXT("Transient acoustic volume fixture is unavailable.");
			return false;
		}
		if (!Volume->IM_CaptureSceneGeometryForTest(OutCapture.Vertices,
			OutCapture.Triangles, OutCapture.MaterialIndices, OutCapture.Fingerprint))
		{
			OutError = Volume->Status;
			for (const FString& Issue : Volume->SceneIssues)
			{
				OutError += TEXT("; ") + Issue;
			}
			return false;
		}
		if (OutCapture.Triangles.IsEmpty()
			|| OutCapture.Triangles.Num() != OutCapture.MaterialIndices.Num()
			|| OutCapture.Vertices.IsEmpty() || OutCapture.Fingerprint.IsEmpty())
		{
			OutError = TEXT("Capture accessor returned empty or inconsistent geometry.");
			return false;
		}
		return true;
	}

	bool CaptureRejected(FIMPMCapture& OutCapture, FString& OutError) const
	{
		OutCapture = {};
		if (!Volume.IsValid())
		{
			OutError = TEXT("Transient acoustic volume fixture is unavailable.");
			return false;
		}
		const bool bCaptured = Volume->IM_CaptureSceneGeometryForTest(
			OutCapture.Vertices, OutCapture.Triangles,
			OutCapture.MaterialIndices, OutCapture.Fingerprint);
		OutError = Volume->Status;
		if (bCaptured)
		{
			OutError = TEXT("Invalid physical-material input unexpectedly produced a scene.");
		}
		return !bCaptured && OutCapture.Vertices.IsEmpty()
			&& OutCapture.Triangles.IsEmpty() && OutCapture.MaterialIndices.IsEmpty()
			&& Volume->ExportedTriangles == 0 && Volume->GeneratedProbes == 0
			&& !Volume->Status.IsEmpty();
	}

	bool BuildSectionedCube(UStaticMesh*& OutMesh, int32& OutLeafSlot,
		int32& OutWoodSlot, int32& OutLeafTriangles, int32& OutWoodTriangles,
		FString& OutError)
	{
		OutMesh = nullptr;
		OutLeafSlot = INDEX_NONE;
		OutWoodSlot = INDEX_NONE;
		OutLeafTriangles = 0;
		OutWoodTriangles = 0;
		if (!SourceMesh || !SourceMesh->GetRenderData()
			|| SourceMesh->GetRenderData()->LODResources.IsEmpty()
			|| SourceMesh->GetStaticMaterials().Num() != 1)
		{
			OutError = TEXT("Sectioned-Cube source must expose CPU-readable LOD0 and one material slot.");
			return false;
		}

		FMeshDescription Description;
		SourceMesh->ExportStaticMeshLOD(SourceMesh->GetRenderData()->LODResources[0], Description);
		TArray<FPolygonID> PolygonIds;
		for (const FPolygonID Id : Description.Polygons().GetElementIDs()) PolygonIds.Add(Id);
		TArray<FPolygonGroupID> GroupIds;
		for (const FPolygonGroupID Id : Description.PolygonGroups().GetElementIDs()) GroupIds.Add(Id);
		if (PolygonIds.Num() < 2 || GroupIds.Num() != 1)
		{
			OutError = FString::Printf(TEXT("Cube source topology cannot be split into two material sections: polygons=%d groups=%d."),
				PolygonIds.Num(), GroupIds.Num());
			return false;
		}

		const FName LeafSlotName(TEXT("IMAcoustic_PM_Leaf"));
		const FName WoodSlotName(TEXT("IMAcoustic_PM_Wood"));
		const FName ImportedSlotAttributeName(TEXT("ImportedMaterialSlotName"));
		TPolygonGroupAttributesRef<FName> ImportedSlotNames =
			Description.PolygonGroupAttributes().GetAttributesRef<FName>(ImportedSlotAttributeName);
		const FPolygonGroupID LeafGroup = GroupIds[0];
		const FPolygonGroupID WoodGroup = Description.CreatePolygonGroup();
		ImportedSlotNames[LeafGroup] = LeafSlotName;
		ImportedSlotNames[WoodGroup] = WoodSlotName;

		// Split complete source polygons so the mesh builder produces two real sections.
		for (int32 PolygonIndex = 0; PolygonIndex < PolygonIds.Num(); ++PolygonIndex)
		{
			const FPolygonGroupID Group = (PolygonIndex % 2 == 0) ? LeafGroup : WoodGroup;
			Description.SetPolygonPolygonGroup(PolygonIds[PolygonIndex], Group);
		}

		const FName MeshName = MakeUniqueObjectName(GetTransientPackage(),
			UStaticMesh::StaticClass(), TEXT("IMAcoustic_PM_SectionedCube"));
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), MeshName, RF_Transient);
		if (!Mesh)
		{
			OutError = TEXT("Could not create the transient two-section Cube.");
			return false;
		}
		SectionedMesh = TStrongObjectPtr<UStaticMesh>(Mesh);
		Mesh->bAllowCPUAccess = true;

		TArray<FStaticMaterial> StaticMaterials;
		StaticMaterials.Add(FStaticMaterial(BaseMaterial, LeafSlotName, LeafSlotName));
		StaticMaterials.Add(FStaticMaterial(BaseMaterial, WoodSlotName, WoodSlotName));
		Mesh->SetStaticMaterials(StaticMaterials);
		TArray<const FMeshDescription*> Descriptions;
		Descriptions.Add(&Description);
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bCommitMeshDescription = false;
		Params.bFastBuild = true;
		Params.bAllowCpuAccess = true;
		if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params)
			|| !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
		{
			OutError = TEXT("BuildFromMeshDescriptions did not produce the transient two-section Cube.");
			return false;
		}

		OutLeafSlot = Mesh->GetMaterialIndex(LeafSlotName);
		OutWoodSlot = Mesh->GetMaterialIndex(WoodSlotName);
		const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
		bool bFoundLeaf = false;
		bool bFoundWood = false;
		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			if (Section.MaterialIndex == OutLeafSlot)
			{
				OutLeafTriangles += int32(Section.NumTriangles);
				bFoundLeaf = true;
			}
			else if (Section.MaterialIndex == OutWoodSlot)
			{
				OutWoodTriangles += int32(Section.NumTriangles);
				bFoundWood = true;
			}
		}
		if (OutLeafSlot == INDEX_NONE || OutWoodSlot == INDEX_NONE
			|| OutLeafSlot == OutWoodSlot || LOD.Sections.Num() != 2
			|| !bFoundLeaf || !bFoundWood || OutLeafTriangles <= 0 || OutWoodTriangles <= 0)
		{
			OutError = FString::Printf(TEXT("Transient Cube did not retain two populated material sections: sections=%d leaf_slot=%d leaf_triangles=%d wood_slot=%d wood_triangles=%d."),
				LOD.Sections.Num(), OutLeafSlot, OutLeafTriangles, OutWoodSlot, OutWoodTriangles);
			return false;
		}

		OutMesh = Mesh;
		return true;
	}

	void CleanupTransientScene()
	{
		for (const TWeakObjectPtr<AActor>& Actor : TransientActors)
		{
			if (Actor.IsValid())
			{
				Actor->Destroy();
			}
		}
		TransientActors.Reset();
		if (Volume.IsValid())
		{
			Volume->Destroy();
		}
		Volume.Reset();
	}

	bool RestoreMap()
	{
		CleanupTransientScene();
		for (const TWeakObjectPtr<UPrimitiveComponent>& Component : TaggedComponents)
		{
			if (Component.IsValid())
			{
				Component->ComponentTags.Remove(IMPMIgnoreTag);
			}
		}
		TaggedComponents.Reset();
		if (!bMapLoaded)
		{
			return true;
		}
		if (!FEditorFileUtils::LoadMap(IMPMMap, false, true))
		{
			return false;
		}
		bMapLoaded = false;
		UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		UStaticMesh* RestoredMesh = nullptr;
		UMaterialInterface* RestoredMaterial = nullptr;
		int32 RestoredComponents = 0;
		int32 RestoredUniqueMeshes = 0;
		const FString RestoredIdentity = IMPMReadMapIdentity(RestoredWorld,
			RestoredMesh, RestoredMaterial, RestoredComponents, RestoredUniqueMeshes);
		const bool bRestored = RestoredIdentity == MapIdentity
			&& RestoredComponents == WhiteboxComponents && RestoredUniqueMeshes == 1
			&& RestoredMesh == SourceMesh && RestoredMaterial == BaseMaterial;
		World = RestoredWorld;
		return bRestored;
	}

	UWorld* World = nullptr;
	UStaticMesh* SourceMesh = nullptr;
	UMaterialInterface* BaseMaterial = nullptr;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TArray<TWeakObjectPtr<AActor>> TransientActors;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> TaggedComponents;
	TArray<TStrongObjectPtr<UPhysicalMaterial>> OwnedPhysicalMaterials;
	TArray<TStrongObjectPtr<UMaterialInstanceConstant>> OwnedMaterialInstances;
	TStrongObjectPtr<UStaticMesh> SectionedMesh;
	FString MapIdentity;
	int32 WhiteboxComponents = 0;
	bool bMapLoaded = false;

private:
	bool IMPMTagExistingScene(FString& OutError)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UPrimitiveComponent*> Components;
			It->GetComponents(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				if (Component && !Component->ComponentHasTag(IMPMIgnoreTag))
				{
					Component->ComponentTags.Add(IMPMIgnoreTag);
					TaggedComponents.Add(Component);
				}
			}
		}
		bMapLoaded = true;
		return true;
	}
};

static bool IMPMWriteReport(const FString& RunId, const FString& MapIdentity,
	const FString& SourceMeshPath, const TArray<FIMPMCaseResult>& Cases,
	bool bPassed, const FString& Summary, FString& OutPath)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AcousticV2/PhysicalMaterialDrive"));
	OutPath = FPaths::Combine(Directory,
		FString::Printf(TEXT("pm_contract_test_%s.json"), *RunId));

	FString Json;
	auto Writer = TJsonWriterFactory<>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema"), 1);
	Writer->WriteValue(TEXT("run_id"), RunId);
	Writer->WriteValue(TEXT("automation_test"), TEXT("IceMoon.AcousticField.PhysicalMaterial.SceneContract"));
	Writer->WriteValue(TEXT("scope"), TEXT("Editor automation contract gate only; no bake, listening, pressure test, or user acceptance."));
	Writer->WriteValue(TEXT("source_map"), IMPMMap);
	Writer->WriteValue(TEXT("source_map_identity"), MapIdentity);
	Writer->WriteValue(TEXT("source_cube_mesh"), SourceMeshPath);
	Writer->WriteValue(TEXT("sdk_coordinate_convention"), TEXT("UE world centimetres relative to BakeBounds component origin; SDK metres (UE Y, UE Z, -UE X)."));
	Writer->WriteValue(TEXT("coordinate_tolerance_meters"), IMPMCoordinateToleranceMeters);
	Writer->WriteValue(TEXT("final_result"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
	Writer->WriteValue(TEXT("summary"), Summary);
	Writer->WriteArrayStart(TEXT("cases"));
	for (const FIMPMCaseResult& Case : Cases)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("name"), Case.Name);
		Writer->WriteValue(TEXT("passed"), Case.bPassed);
		Writer->WriteValue(TEXT("expected_triangles"), Case.ExpectedTriangles);
		Writer->WriteValue(TEXT("actual_triangles"), Case.ActualTriangles);
		Writer->WriteValue(TEXT("expected_non_solid_sections"), Case.ExpectedNonSolidSections);
		Writer->WriteValue(TEXT("actual_non_solid_sections"), Case.ActualNonSolidSections);
		Writer->WriteValue(TEXT("expected_non_solid_triangles"), Case.ExpectedNonSolidTriangles);
		Writer->WriteValue(TEXT("actual_non_solid_triangles"), Case.ActualNonSolidTriangles);
		Writer->WriteValue(TEXT("fingerprint"), Case.Fingerprint);
		Writer->WriteValue(TEXT("details"), Case.Details);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("not_covered"));
	Writer->WriteValue(TEXT("Steam Audio bake payload and cold runtime load."));
	Writer->WriteValue(TEXT("interactive listening, device audio, and sustained pressure."));
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	return IFileManager::Get().MakeDirectory(*Directory, true)
		&& FFileHelper::SaveStringToFile(Json, *OutPath);
}

static FIMPMCaseResult IMPMFailedCase(const FString& Name, const FString& Error)
{
	FIMPMCaseResult Result;
	Result.Name = Name;
	Result.bPassed = false;
	Result.Details = Error;
	return Result;
}
}

using namespace IMAcousticPhysicalMaterialTestPrivate;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPhysicalMaterialSceneContract,
	"IceMoon.AcousticField.PhysicalMaterial.SceneContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticPhysicalMaterialSceneContract::RunTest(const FString&)
{
	FIMPMContractFixture Fixture;
	TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult> Cases;
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString FixtureError;
	if (!Fixture.Begin(FixtureError))
	{
		AddError(FixtureError);
		const bool bRestored = Fixture.RestoreMap();
		FString ReportPath;
		const bool bReport = IMAcousticPhysicalMaterialTestPrivate::IMPMWriteReport(RunId, Fixture.MapIdentity,
			GetPathNameSafe(Fixture.SourceMesh), Cases, false,
			TEXT("Fixture setup failed before the six contract cases could run."), ReportPath);
		AddInfo(FString::Printf(TEXT("Map restored=%s; report written=%s path=%s"),
			bRestored ? TEXT("true") : TEXT("false"), bReport ? TEXT("true") : TEXT("false"), *ReportPath));
		return false;
	}

	UStaticMesh* Cube = Fixture.SourceMesh;
	TArray<FStaticMaterial> CubeMaterials = Cube->GetStaticMaterials();
	const FStaticMeshLODResources& CubeLOD = Cube->GetRenderData()->LODResources[0];
	const int32 CubeTriangles = [&CubeLOD]()
	{
		int32 Total = 0;
		for (const FStaticMeshSection& Section : CubeLOD.Sections)
		{
			Total += int32(Section.NumTriangles);
		}
		return Total;
	}();

	UPhysicalMaterial* SlotPhysicalMaterial = Fixture.MakePhysicalMaterial(TEXT("IMPM_SlotStone"));
	UPhysicalMaterial* BodyPhysicalMaterial = Fixture.MakePhysicalMaterial(TEXT("IMPM_BodyWood"));
	UPhysicalMaterial* LeafPhysicalMaterial = Fixture.MakePhysicalMaterial(TEXT("IMPM_Leaf"));
	UPhysicalMaterial* WoodPhysicalMaterial = Fixture.MakePhysicalMaterial(TEXT("IMPM_WoodSolid"));
	UPhysicalMaterial* UnknownPhysicalMaterial = Fixture.MakePhysicalMaterial(TEXT("IMPM_Unknown"));
	UPhysicalMaterial* ComponentPhysicalMaterial = Fixture.MakePhysicalMaterial(TEXT("IMPM_ComponentOverride"));
	UMaterialInstanceConstant* SlotMaterial = Fixture.MakeMaterialInstance(TEXT("IMPM_SlotMIC"), SlotPhysicalMaterial);
	UMaterialInstanceConstant* AlternateSlotMaterial = Fixture.MakeMaterialInstance(TEXT("IMPM_AlternateSlotMIC"), BodyPhysicalMaterial);
	UMaterialInstanceConstant* LeafMaterial = Fixture.MakeMaterialInstance(TEXT("IMPM_LeafMIC"), LeafPhysicalMaterial);
	UMaterialInstanceConstant* WoodMaterial = Fixture.MakeMaterialInstance(TEXT("IMPM_WoodMIC"), WoodPhysicalMaterial);
	if (CubeMaterials.Num() != 1 || CubeTriangles <= 0 || !SlotPhysicalMaterial
		|| !BodyPhysicalMaterial || !LeafPhysicalMaterial || !WoodPhysicalMaterial
		|| !UnknownPhysicalMaterial || !ComponentPhysicalMaterial || !SlotMaterial
		|| !AlternateSlotMaterial || !LeafMaterial || !WoodMaterial)
	{
		AddError(TEXT("Could not construct the single-slot Cube or transient physical-material/MIC inputs."));
		Fixture.RestoreMap();
		return false;
	}

	TArray<bool> GeometryProfilesSolid;
	GeometryProfilesSolid.Add(true);
	GeometryProfilesSolid.Add(true);
	TArray<FIMAcousticPhysicalMaterialMapping> GeometryProfiles;
	GeometryProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(SlotPhysicalMaterial, FVector(.15, .25, .35), .2f));
	GeometryProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(BodyPhysicalMaterial, FVector(.55, .45, .25), .65f));

	// Case 1 and 2 share one capture: six distinct world transforms provide an independent LOD0 oracle,
	// while each SM/ISM/HISM family has one actual MIC slot-PM case and one BodyInstance override case.
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult GeometryCase;
	GeometryCase.Name = TEXT("six_cube_components_native_geometry_and_centroids");
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult PriorityCase;
	PriorityCase.Name = TEXT("mic_slot_physical_material_vs_body_instance_priority_sm_ism_hism");
	bool bHaveGeometryCapture = false;
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture GeometryCapture;
	IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary ExpectedGeometrySummary;
	IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary ActualGeometrySummary;
	TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> GeometryExpectedObjects;
	FString GeometryError;

	if (!Fixture.StartCaptureCase(GeometryProfiles, FVector(1200.0, 1200.0, 1200.0), GeometryError))
	{
		GeometryCase.Details = GeometryError;
		PriorityCase.Details = TEXT("Skipped because the shared six-object capture fixture failed.");
	}
	else
	{
		const TArray<FTransform> Transforms = {
			FTransform(FRotator(0, 0, 0), FVector(-420, -280, 0), FVector(1.0, 1.0, 1.0)),
			FTransform(FRotator(11, 17, 27), FVector(-210, 230, 50), FVector(-1.2, .7, 1.4)),
			FTransform(FRotator(25, 5, 35), FVector(0, -330, 100), FVector(.8, 1.3, 1.1)),
			FTransform(FRotator(-15, 30, -20), FVector(160, -90, 90), FVector(1.7, .6, -.9)),
			FTransform(FRotator(7, -22, 51), FVector(210, 240, -40), FVector(.55, 1.6, .75)),
			FTransform(FRotator(32, 15, 8), FVector(350, -210, 130), FVector(-.9, 1.1, 1.8))
		};
		TArray<int32> ExpectedIndices = {0, 1, 0, 1, 0, 1};
		UStaticMeshComponent* StaticSlot = Fixture.AddStatic(Transforms[0], Cube, SlotMaterial);
		UStaticMeshComponent* StaticOverride = Fixture.AddStatic(Transforms[1], Cube, SlotMaterial, BodyPhysicalMaterial);
		UInstancedStaticMeshComponent* ISMSlot = Fixture.AddInstanced(false, Transforms[2], Cube, SlotMaterial);
		UInstancedStaticMeshComponent* ISMOverride = Fixture.AddInstanced(false, Transforms[3], Cube, SlotMaterial, BodyPhysicalMaterial);
		UInstancedStaticMeshComponent* HISMSlot = Fixture.AddInstanced(true, Transforms[4], Cube, SlotMaterial);
		UInstancedStaticMeshComponent* HISMOverride = Fixture.AddInstanced(true, Transforms[5], Cube, SlotMaterial, BodyPhysicalMaterial);
		const bool bComponentsCreated = StaticSlot && StaticOverride && ISMSlot && ISMOverride && HISMSlot && HISMOverride
			&& ISMSlot->GetInstanceCount() == 1 && ISMOverride->GetInstanceCount() == 1
			&& HISMSlot->GetInstanceCount() == 1 && HISMOverride->GetInstanceCount() == 1;
		if (!bComponentsCreated)
		{
			GeometryError = TEXT("Could not create the required 2 SM + 2 ISM + 2 HISM Cube objects.");
		}
		else
		{
			for (int32 ObjectIndex = 0; ObjectIndex < Transforms.Num(); ++ObjectIndex)
			{
				IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& Expected = GeometryExpectedObjects.AddDefaulted_GetRef();
				Expected.Mesh = Cube;
				Expected.WorldTransform = Transforms[ObjectIndex];
				Expected.ProfileByMaterialSlot = IMPMUniformSlotMap(Cube, ExpectedIndices[ObjectIndex]);
			}
			const FVector Origin = Fixture.Volume->BakeBounds->GetComponentLocation();
			int32 ExpectedNonSolidSections = 0;
			int32 ExpectedNonSolidTriangles = 0;
			bHaveGeometryCapture = Fixture.Capture(GeometryCapture, GeometryError)
				&& IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(GeometryExpectedObjects, Origin, GeometryProfilesSolid,
					ExpectedGeometrySummary, ExpectedNonSolidSections,
					ExpectedNonSolidTriangles, GeometryError)
				&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(GeometryCapture, ActualGeometrySummary, GeometryError);
			if (bHaveGeometryCapture)
			{
				GeometryCase.bPassed = IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(ExpectedGeometrySummary,
					ActualGeometrySummary, GeometryError)
					&& ExpectedGeometrySummary.TriangleKeys.Num() == 6 * CubeTriangles
					&& Fixture.Volume->ExportedTriangles == 6 * CubeTriangles
					&& Fixture.Volume->NonSolidSections == 0
					&& Fixture.Volume->NonSolidTriangles == 0;
				GeometryCase.ExpectedTriangles = 6 * CubeTriangles;
				GeometryCase.ActualTriangles = GeometryCapture.Triangles.Num();
				GeometryCase.ExpectedNonSolidSections = 0;
				GeometryCase.ActualNonSolidSections = Fixture.Volume->NonSolidSections;
				GeometryCase.ExpectedNonSolidTriangles = 0;
				GeometryCase.ActualNonSolidTriangles = Fixture.Volume->NonSolidTriangles;
				GeometryCase.Fingerprint = GeometryCapture.Fingerprint;
				GeometryCase.Details = FString::Printf(TEXT("Objects=2 SM + 2 ISM + 2 HISM, one instance each; %s; %s; expected reflected winding follows transform determinant."),
					*FString::Printf(TEXT("SDK axis map is metres=(UE Y, UE Z, -UE X), origin=%s cm"), *Origin.ToString()),
					*GeometryError);
				if (GeometryError.IsEmpty())
				{
					GeometryCase.Details = FString::Printf(TEXT("Objects=2 SM + 2 ISM + 2 HISM, one instance each; SDK metres=(UE Y, UE Z, -UE X) relative to BakeBounds origin %s cm; %d oriented LOD0 triangles matched; determinant-aware winding, per-profile counts, and SDK triangle-centroid means matched."),
						*Origin.ToString(), GeometryCase.ExpectedTriangles);
				}
				FString CentroidDetails;
				IMPMAppendCountsAndCentroids(CentroidDetails, TEXT("expected"), ExpectedGeometrySummary);
				IMPMAppendCountsAndCentroids(CentroidDetails, TEXT("captured"), ActualGeometrySummary);
				GeometryCase.Details += TEXT(" ") + CentroidDetails;

				const TArray<int32> ExpectedByType = {0, 1, 0, 1, 0, 1};
				const TCHAR* TypeNames[6] = {TEXT("SM slot"), TEXT("SM override"), TEXT("ISM slot"),
					TEXT("ISM override"), TEXT("HISM slot"), TEXT("HISM override")};
				FString PriorityDetails;
				bool bPriorityCounts = true;
				for (int32 ObjectIndex = 0; ObjectIndex < 6; ++ObjectIndex)
				{
					const int32 ExpectedIndex = ExpectedByType[ObjectIndex];
					PriorityDetails += FString::Printf(TEXT("%s=>%d; "), TypeNames[ObjectIndex], ExpectedIndex);
					if (ExpectedGeometrySummary.TriangleCounts.FindRef(ExpectedIndex) != 3 * CubeTriangles)
					{
						bPriorityCounts = false;
					}
				}
				PriorityCase.bPassed = GeometryCase.bPassed && bPriorityCounts
					&& SlotMaterial->GetPhysicalMaterial() == SlotPhysicalMaterial
					&& StaticOverride->BodyInstance.GetPhysMaterialOverride() == BodyPhysicalMaterial
					&& ISMOverride->BodyInstance.GetPhysMaterialOverride() == BodyPhysicalMaterial
					&& HISMOverride->BodyInstance.GetPhysMaterialOverride() == BodyPhysicalMaterial;
				PriorityCase.ExpectedTriangles = 6 * CubeTriangles;
				PriorityCase.ActualTriangles = GeometryCapture.Triangles.Num();
				PriorityCase.Fingerprint = GeometryCapture.Fingerprint;
				PriorityCase.Details = FString::Printf(TEXT("Actual slot MIC returns PM %s; component BodyInstance override PM is %s. Expected/captured geometry places each family’s unoverridden slot at profile index 0 and each BodyInstance override at profile index 1. %s"),
					*GetPathNameSafe(SlotPhysicalMaterial), *GetPathNameSafe(BodyPhysicalMaterial), *PriorityDetails);
			}
			else
			{
				GeometryCase.Details = GeometryError;
				PriorityCase.Details = TEXT("Could not evaluate per-type PM precedence because the six-object capture failed.");
			}
		}
	}
	Cases.Add(GeometryCase);
	Cases.Add(PriorityCase);

	// Case 3: one-instance ISM/HISM bodies are distinct from their component override and section MIC fallback.
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult InstancePriorityCase;
	InstancePriorityCase.Name = TEXT("ism_hism_instance_body_component_override_and_section_fallback_priority");
	TArray<FIMAcousticPhysicalMaterialMapping> InstancePriorityProfiles;
	InstancePriorityProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(SlotPhysicalMaterial, FVector(.15, .25, .35), .2f));
	InstancePriorityProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(BodyPhysicalMaterial, FVector(.55, .45, .25), .65f));
	InstancePriorityProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(ComponentPhysicalMaterial, FVector(.25, .55, .45), .4f));
	const TArray<FTransform> InstancePriorityTransforms = {
		FTransform(FRotator(0, 5, 10), FVector(-350, -280, 40)),
		FTransform(FRotator(5, 10, 15), FVector(-220, 180, 70)),
		FTransform(FRotator(10, 15, 20), FVector(-60, -330, 100)),
		FTransform(FRotator(15, 20, 25), FVector(120, 260, -30)),
		FTransform(FRotator(20, 25, 30), FVector(280, -170, 90)),
		FTransform(FRotator(25, 30, 35), FVector(410, 130, -60))
	};
	const TCHAR* PriorityRowNames[6] = {
		TEXT("ISM section fallback"), TEXT("ISM instance override"),
		TEXT("HISM section fallback"), TEXT("HISM instance override"),
		TEXT("ISM component plus instance override"), TEXT("HISM component plus instance override")
	};
	FString InstancePriorityError;
	if (Fixture.StartCaptureCase(InstancePriorityProfiles, FVector(1200), InstancePriorityError))
	{
		UInstancedStaticMeshComponent* ISMSectionFallback = Fixture.AddInstanced(
			false, InstancePriorityTransforms[0], Cube, SlotMaterial);
		UInstancedStaticMeshComponent* ISMInstanceOverride = Fixture.AddInstanced(
			false, InstancePriorityTransforms[1], Cube, SlotMaterial);
		UInstancedStaticMeshComponent* HISMSectionFallback = Fixture.AddInstanced(
			true, InstancePriorityTransforms[2], Cube, SlotMaterial);
		UInstancedStaticMeshComponent* HISMInstanceOverride = Fixture.AddInstanced(
			true, InstancePriorityTransforms[3], Cube, SlotMaterial);
		UInstancedStaticMeshComponent* ISMComponentAndInstanceOverride = Fixture.AddInstanced(
			false, InstancePriorityTransforms[4], Cube, SlotMaterial);
		UInstancedStaticMeshComponent* HISMComponentAndInstanceOverride = Fixture.AddInstanced(
			true, InstancePriorityTransforms[5], Cube, SlotMaterial);
		TArray<UInstancedStaticMeshComponent*> PriorityComponents = {
			ISMSectionFallback, ISMInstanceOverride, HISMSectionFallback,
			HISMInstanceOverride, ISMComponentAndInstanceOverride,
			HISMComponentAndInstanceOverride
		};
		bool bOneInstanceEach = true;
		for (UInstancedStaticMeshComponent* Component : PriorityComponents)
		{
			bOneInstanceEach &= Component && Component->GetInstanceCount() == 1;
		}

		bool bBodyOverridesConfigured = bOneInstanceEach;
		FBodyInstance* ISMInstanceBody = ISMInstanceOverride
			? ISMInstanceOverride->GetBodyInstance(NAME_None, false, 0) : nullptr;
		FBodyInstance* HISMInstanceBody = HISMInstanceOverride
			? HISMInstanceOverride->GetBodyInstance(NAME_None, false, 0) : nullptr;
		if (ISMComponentAndInstanceOverride)
		{
			ISMComponentAndInstanceOverride->SetPhysMaterialOverride(ComponentPhysicalMaterial);
		}
		if (HISMComponentAndInstanceOverride)
		{
			HISMComponentAndInstanceOverride->SetPhysMaterialOverride(ComponentPhysicalMaterial);
		}
		FBodyInstance* ISMCombinedBody = ISMComponentAndInstanceOverride
			? ISMComponentAndInstanceOverride->GetBodyInstance(NAME_None, false, 0) : nullptr;
		FBodyInstance* HISMCombinedBody = HISMComponentAndInstanceOverride
			? HISMComponentAndInstanceOverride->GetBodyInstance(NAME_None, false, 0) : nullptr;
		if (ISMInstanceBody)
		{
			ISMInstanceBody->SetPhysMaterialOverride(BodyPhysicalMaterial);
		}
		if (HISMInstanceBody)
		{
			HISMInstanceBody->SetPhysMaterialOverride(BodyPhysicalMaterial);
		}
		if (ISMCombinedBody)
		{
			ISMCombinedBody->SetPhysMaterialOverride(BodyPhysicalMaterial);
		}
		if (HISMCombinedBody)
		{
			HISMCombinedBody->SetPhysMaterialOverride(BodyPhysicalMaterial);
		}
		bBodyOverridesConfigured &= ISMInstanceBody && HISMInstanceBody
			&& ISMCombinedBody && HISMCombinedBody
			&& ISMInstanceBody->GetPhysMaterialOverride() == BodyPhysicalMaterial
			&& HISMInstanceBody->GetPhysMaterialOverride() == BodyPhysicalMaterial
			&& ISMCombinedBody->GetPhysMaterialOverride() == BodyPhysicalMaterial
			&& HISMCombinedBody->GetPhysMaterialOverride() == BodyPhysicalMaterial
			&& ISMComponentAndInstanceOverride->BodyInstance.GetPhysMaterialOverride() == ComponentPhysicalMaterial
			&& HISMComponentAndInstanceOverride->BodyInstance.GetPhysMaterialOverride() == ComponentPhysicalMaterial
			&& ISMSectionFallback->BodyInstance.GetPhysMaterialOverride() == nullptr
			&& HISMSectionFallback->BodyInstance.GetPhysMaterialOverride() == nullptr
			&& ISMInstanceOverride->BodyInstance.GetPhysMaterialOverride() == nullptr
			&& HISMInstanceOverride->BodyInstance.GetPhysMaterialOverride() == nullptr;

		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture PriorityCapture;
		bool bPriorityCapture = bOneInstanceEach && bBodyOverridesConfigured
			&& Fixture.Capture(PriorityCapture, InstancePriorityError);
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary PriorityExpected;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary PriorityActual;
		int32 PriorityExpectedNonSolidSections = 0;
		int32 PriorityExpectedNonSolidTriangles = 0;
		TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> PriorityExpectedObjects;
		const TArray<int32> PriorityProfileIndices = {0, 1, 0, 1, 2, 2};
		for (int32 ObjectIndex = 0; ObjectIndex < InstancePriorityTransforms.Num(); ++ObjectIndex)
		{
			IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& Expected = PriorityExpectedObjects.AddDefaulted_GetRef();
			Expected.Mesh = Cube;
			Expected.WorldTransform = InstancePriorityTransforms[ObjectIndex];
			Expected.ProfileByMaterialSlot = IMPMUniformSlotMap(Cube, PriorityProfileIndices[ObjectIndex]);
		}
		TArray<bool> InstancePriorityProfilesSolid = {true, true, true};
		const FVector PriorityOrigin = Fixture.Volume->BakeBounds->GetComponentLocation();
		bPriorityCapture &= IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(PriorityExpectedObjects, PriorityOrigin,
			InstancePriorityProfilesSolid, PriorityExpected, PriorityExpectedNonSolidSections,
			PriorityExpectedNonSolidTriangles, InstancePriorityError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(PriorityCapture, PriorityActual, InstancePriorityError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(PriorityExpected, PriorityActual, InstancePriorityError);
		const bool bExpectedProfileCounts = PriorityActual.TriangleCounts.FindRef(0) == 2 * CubeTriangles
			&& PriorityActual.TriangleCounts.FindRef(1) == 2 * CubeTriangles
			&& PriorityActual.TriangleCounts.FindRef(2) == 2 * CubeTriangles;
		InstancePriorityCase.bPassed = bOneInstanceEach && bBodyOverridesConfigured
			&& bPriorityCapture && bExpectedProfileCounts;
		InstancePriorityCase.ExpectedTriangles = 6 * CubeTriangles;
		InstancePriorityCase.ActualTriangles = PriorityCapture.Triangles.Num();
		InstancePriorityCase.Fingerprint = PriorityCapture.Fingerprint;
		FString PriorityRows;
		for (int32 ObjectIndex = 0; ObjectIndex < 6; ++ObjectIndex)
		{
			PriorityRows += FString::Printf(TEXT("%s=>profile %d; "),
				PriorityRowNames[ObjectIndex], PriorityProfileIndices[ObjectIndex]);
		}
		InstancePriorityCase.Details = FString::Printf(
			TEXT("Each ISM/HISM has exactly one instance. Section MIC=%s; per-instance BodyInstance override=%s; component BodyInstance override=%s. Expected component > per-instance > section selection; actual profile triangle counts are %d/%d/%d. %s %s"),
			*GetPathNameSafe(SlotPhysicalMaterial), *GetPathNameSafe(BodyPhysicalMaterial),
			*GetPathNameSafe(ComponentPhysicalMaterial), PriorityActual.TriangleCounts.FindRef(0),
			PriorityActual.TriangleCounts.FindRef(1), PriorityActual.TriangleCounts.FindRef(2),
			*PriorityRows, *InstancePriorityError);
	}
	else
	{
		InstancePriorityCase.Details = TEXT("Could not construct the three-profile ISM/HISM body precedence fixture: ")
			+ InstancePriorityError;
	}
	Cases.Add(InstancePriorityCase);

	// Case 4: switch only the section MIC's physical material; geometry must stay exact while the fingerprint changes.
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult SectionPhysicalMaterialSwitchCase;
	SectionPhysicalMaterialSwitchCase.Name = TEXT("section_mic_physical_material_only_switch_preserves_geometry_and_changes_fingerprint");
	TArray<FIMAcousticPhysicalMaterialMapping> SectionSwitchProfiles;
	SectionSwitchProfiles.Add(GeometryProfiles[0]);
	SectionSwitchProfiles.Add(GeometryProfiles[1]);
	const FTransform SectionSwitchTransform(FRotator(13, 29, 7), FVector(170, -210, 80), FVector(1.2, .8, 1.1));
	FString SectionSwitchError;
	if (Fixture.StartCaptureCase(SectionSwitchProfiles, FVector(1200), SectionSwitchError))
	{
		UStaticMeshComponent* SectionSwitchComponent = Fixture.AddStatic(
			SectionSwitchTransform, Cube, SlotMaterial);
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureBeforeSwitch;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary ExpectedBeforeSwitch;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary ActualBeforeSwitch;
		int32 BeforeNonSolidSections = 0;
		int32 BeforeNonSolidTriangles = 0;
		TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> BeforeObjects;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& BeforeObject = BeforeObjects.AddDefaulted_GetRef();
		BeforeObject.Mesh = Cube;
		BeforeObject.WorldTransform = SectionSwitchTransform;
		BeforeObject.ProfileByMaterialSlot = IMPMUniformSlotMap(Cube, 0);
		TArray<bool> SectionSwitchProfilesSolid = {true, true};
		const FVector SectionSwitchOrigin = Fixture.Volume->BakeBounds->GetComponentLocation();
		bool bBeforeSwitch = SectionSwitchComponent
			&& SectionSwitchComponent->GetMaterial(0) == SlotMaterial
			&& SlotMaterial->GetPhysicalMaterial() == SlotPhysicalMaterial
			&& Fixture.Capture(CaptureBeforeSwitch, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(BeforeObjects, SectionSwitchOrigin, SectionSwitchProfilesSolid,
				ExpectedBeforeSwitch, BeforeNonSolidSections, BeforeNonSolidTriangles, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureBeforeSwitch, ActualBeforeSwitch, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(ExpectedBeforeSwitch, ActualBeforeSwitch, SectionSwitchError);

		if (bBeforeSwitch)
		{
			SectionSwitchComponent->SetMaterial(0, AlternateSlotMaterial);
		}
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureAfterSwitch;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary ExpectedAfterSwitch;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary ActualAfterSwitch;
		int32 AfterNonSolidSections = 0;
		int32 AfterNonSolidTriangles = 0;
		TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> AfterObjects;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& AfterObject = AfterObjects.AddDefaulted_GetRef();
		AfterObject.Mesh = Cube;
		AfterObject.WorldTransform = SectionSwitchTransform;
		AfterObject.ProfileByMaterialSlot = IMPMUniformSlotMap(Cube, 1);
		const bool bAfterSwitch = bBeforeSwitch
			&& SectionSwitchComponent->GetMaterial(0) == AlternateSlotMaterial
			&& AlternateSlotMaterial->GetPhysicalMaterial() == BodyPhysicalMaterial
			&& Fixture.Capture(CaptureAfterSwitch, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(AfterObjects, SectionSwitchOrigin, SectionSwitchProfilesSolid,
				ExpectedAfterSwitch, AfterNonSolidSections, AfterNonSolidTriangles, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureAfterSwitch, ActualAfterSwitch, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(ExpectedAfterSwitch, ActualAfterSwitch, SectionSwitchError);

		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture GeometryOnlyBefore = CaptureBeforeSwitch;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture GeometryOnlyAfter = CaptureAfterSwitch;
		GeometryOnlyBefore.MaterialIndices.Init(0, GeometryOnlyBefore.Triangles.Num());
		GeometryOnlyAfter.MaterialIndices.Init(0, GeometryOnlyAfter.Triangles.Num());
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary GeometryOnlyBeforeSummary;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary GeometryOnlyAfterSummary;
		const bool bGeometryOnlyControl = bBeforeSwitch && bAfterSwitch
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(GeometryOnlyBefore, GeometryOnlyBeforeSummary, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(GeometryOnlyAfter, GeometryOnlyAfterSummary, SectionSwitchError)
			&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(GeometryOnlyBeforeSummary, GeometryOnlyAfterSummary, SectionSwitchError);
		const bool bFingerprintNegativeControl = !CaptureBeforeSwitch.Fingerprint.IsEmpty()
			&& !CaptureAfterSwitch.Fingerprint.IsEmpty()
			&& CaptureBeforeSwitch.Fingerprint != CaptureAfterSwitch.Fingerprint;
		SectionPhysicalMaterialSwitchCase.bPassed = bBeforeSwitch && bAfterSwitch
			&& bGeometryOnlyControl && bFingerprintNegativeControl
			&& ActualBeforeSwitch.TriangleCounts.FindRef(0) == CubeTriangles
			&& ActualAfterSwitch.TriangleCounts.FindRef(1) == CubeTriangles;
		SectionPhysicalMaterialSwitchCase.ExpectedTriangles = CubeTriangles;
		SectionPhysicalMaterialSwitchCase.ActualTriangles = CaptureAfterSwitch.Triangles.Num();
		SectionPhysicalMaterialSwitchCase.Fingerprint = CaptureAfterSwitch.Fingerprint;
		SectionPhysicalMaterialSwitchCase.Details = FString::Printf(
			TEXT("Only section MIC changed from %s (PM=%s, profile 0) to %s (PM=%s, profile 1); same component, transform, mesh, bounds, and profiles. Geometry stayed exact=%s; fingerprint changed=%s. %s"),
			*GetPathNameSafe(SlotMaterial), *GetPathNameSafe(SlotPhysicalMaterial),
			*GetPathNameSafe(AlternateSlotMaterial), *GetPathNameSafe(BodyPhysicalMaterial),
			bGeometryOnlyControl ? TEXT("true") : TEXT("false"),
			bFingerprintNegativeControl ? TEXT("true") : TEXT("false"), *SectionSwitchError);
	}
	else
	{
		SectionPhysicalMaterialSwitchCase.Details = TEXT("Could not construct section MIC switch fixture: ") + SectionSwitchError;
	}
	Cases.Add(SectionPhysicalMaterialSwitchCase);

	// Case 5: unknown references, duplicate identities, and nonfinite coefficients must reject the whole scene.
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult InvalidInputCase;
	InvalidInputCase.Name = TEXT("unknown_physical_material_duplicate_mapping_and_nonfinite_fail_closed");
	bool bUnknownRejected = false;
	bool bDuplicateRejected = false;
	bool bNonfiniteRejected = false;
	FString InvalidDetails;
	TArray<FIMAcousticPhysicalMaterialMapping> OneKnownProfile;
	OneKnownProfile.Add(GeometryProfiles[0]);
	if (Fixture.StartCaptureCase(OneKnownProfile, FVector(1200), FixtureError))
	{
		UStaticMeshComponent* UnknownComponent = Fixture.AddStatic(
			FTransform(FRotator::ZeroRotator, FVector(0, 0, 0)), Cube,
			SlotMaterial, UnknownPhysicalMaterial);
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture RejectedCapture;
		FString Reason;
		bUnknownRejected = UnknownComponent && Fixture.CaptureRejected(RejectedCapture, Reason);
		InvalidDetails += FString::Printf(TEXT("unknownPM rejected=%s reason=%s; "),
			bUnknownRejected ? TEXT("true") : TEXT("false"), *Reason);
	}
	else
	{
		InvalidDetails += TEXT("unknownPM fixture failed; ");
	}

	TArray<FIMAcousticPhysicalMaterialMapping> DuplicateProfiles;
	DuplicateProfiles.Add(GeometryProfiles[0]);
	DuplicateProfiles.Add(GeometryProfiles[0]);
	if (Fixture.StartCaptureCase(DuplicateProfiles, FVector(1200), FixtureError))
	{
		Fixture.AddStatic(FTransform(FRotator::ZeroRotator, FVector(0, 0, 0)), Cube, SlotMaterial);
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture RejectedCapture;
		FString Reason;
		bDuplicateRejected = Fixture.CaptureRejected(RejectedCapture, Reason);
		InvalidDetails += FString::Printf(TEXT("duplicateMapping rejected=%s reason=%s; "),
			bDuplicateRejected ? TEXT("true") : TEXT("false"), *Reason);
	}
	else
	{
		InvalidDetails += TEXT("duplicateMapping fixture failed; ");
	}

	TArray<FIMAcousticPhysicalMaterialMapping> NonfiniteProfiles;
	FIMAcousticPhysicalMaterialMapping Nonfinite = GeometryProfiles[0];
	Nonfinite.Absorption.X = std::numeric_limits<double>::quiet_NaN();
	Nonfinite.Scattering = std::numeric_limits<float>::infinity();
	NonfiniteProfiles.Add(Nonfinite);
	if (Fixture.StartCaptureCase(NonfiniteProfiles, FVector(1200), FixtureError))
	{
		Fixture.AddStatic(FTransform(FRotator::ZeroRotator, FVector(0, 0, 0)), Cube, SlotMaterial);
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture RejectedCapture;
		FString Reason;
		bNonfiniteRejected = Fixture.CaptureRejected(RejectedCapture, Reason);
		InvalidDetails += FString::Printf(TEXT("nonfiniteProfile rejected=%s reason=%s"),
			bNonfiniteRejected ? TEXT("true") : TEXT("false"), *Reason);
	}
	else
	{
		InvalidDetails += TEXT("nonfiniteProfile fixture failed");
	}
	InvalidInputCase.bPassed = bUnknownRejected && bDuplicateRejected && bNonfiniteRejected;
	InvalidInputCase.ExpectedTriangles = 0;
	InvalidInputCase.ActualTriangles = 0;
	InvalidInputCase.Details = InvalidDetails;
	Cases.Add(InvalidInputCase);

	// Case 6: numeric twins with different UPhysicalMaterial identity must change the scene fingerprint.
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult IdentityCase;
	IdentityCase.Name = TEXT("same_numeric_profile_different_physical_material_identity_changes_fingerprint");
	UPhysicalMaterial* IdentityA = Fixture.MakePhysicalMaterial(TEXT("IMPM_IdentityA"));
	UPhysicalMaterial* IdentityB = Fixture.MakePhysicalMaterial(TEXT("IMPM_IdentityB"));
	FIMAcousticPhysicalMaterialMapping IdentityProfileA = IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(
		IdentityA, FVector(.31, .42, .53), .37f, true, true);
	FIMAcousticPhysicalMaterialMapping IdentityProfileB = IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(
		IdentityB, FVector(.31, .42, .53), .37f, true, true);
	TArray<FIMAcousticPhysicalMaterialMapping> IdentityProfilesA;
	IdentityProfilesA.Add(IdentityProfileA);
	TArray<FIMAcousticPhysicalMaterialMapping> IdentityProfilesB;
	IdentityProfilesB.Add(IdentityProfileB);
	FString IdentityError;
	if (IdentityA && IdentityB && Fixture.StartCaptureCase(IdentityProfilesA,
		FVector(1200), IdentityError))
	{
		UStaticMeshComponent* IdentityComponent = Fixture.AddStatic(
			FTransform(FRotator(9, 23, 41), FVector(80, -140, 20), FVector(1.3, .8, 1.1)),
			Cube, SlotMaterial, IdentityA);
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureA;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary SummaryA;
		IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary ExpectedA;
		FString ErrorA;
		TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> ExpectedObjects;
		if (IdentityComponent && Fixture.Capture(CaptureA, ErrorA))
		{
			IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& Expected = ExpectedObjects.AddDefaulted_GetRef();
			Expected.Mesh = Cube;
			Expected.WorldTransform = IdentityComponent->GetComponentTransform();
			Expected.ProfileByMaterialSlot = IMPMUniformSlotMap(Cube, 0);
			int32 ExpectedNS = 0;
			int32 ExpectedNT = 0;
			const bool bExpectedA = IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(ExpectedObjects,
				Fixture.Volume->BakeBounds->GetComponentLocation(), GeometryProfilesSolid,
				ExpectedA, ExpectedNS, ExpectedNT, ErrorA)
				&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureA, SummaryA, ErrorA)
				&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(ExpectedA, SummaryA, ErrorA);

			Fixture.Volume->PhysicalMaterials = IdentityProfilesB;
			IdentityComponent->SetPhysMaterialOverride(IdentityB);
			IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureB;
			IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary SummaryB;
			FString ErrorB;
			const bool bCaptureB = Fixture.Capture(CaptureB, ErrorB)
				&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureB, SummaryB, ErrorB);
			FString GeometrySameError;
			const bool bGeometrySame = bCaptureB && SummaryA.TriangleKeys == SummaryB.TriangleKeys
				&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(SummaryA, SummaryB, GeometrySameError);
			IdentityCase.bPassed = bExpectedA && bGeometrySame
				&& !CaptureA.Fingerprint.IsEmpty()
				&& CaptureA.Fingerprint != CaptureB.Fingerprint;
			IdentityCase.ExpectedTriangles = ExpectedA.TriangleKeys.Num();
			IdentityCase.ActualTriangles = CaptureB.Triangles.Num();
			IdentityCase.Fingerprint = CaptureB.Fingerprint;
			IdentityCase.Details = FString::Printf(TEXT("PM identity A=%s changed to B=%s; absorption=(0.31,0.42,0.53), scattering=0.37, profile index remains 0; geometry/index/centroid stable=%s; fingerprint changed=%s. %s %s"),
				*GetPathNameSafe(IdentityA), *GetPathNameSafe(IdentityB),
				bGeometrySame ? TEXT("true") : TEXT("false"),
				CaptureA.Fingerprint != CaptureB.Fingerprint ? TEXT("true") : TEXT("false"),
				*ErrorA, *GeometrySameError);
		}
		else
		{
			IdentityCase.Details = ErrorA.IsEmpty() ? IdentityError : ErrorA;
		}
	}
	else
	{
		IdentityCase.Details = IdentityError.IsEmpty() ? TEXT("Could not construct two distinct transient physical materials.") : IdentityError;
	}
	Cases.Add(IdentityCase);

	// Case 7: only the leaf section is omitted; wood remains. A separate leaf-only placement then proves
	// non-solid transforms and profile edits remain part of identity even while its geometry is filtered.
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult LeafWoodCase;
	LeafWoodCase.Name = TEXT("leaf_section_filter_wood_retention_and_non_solid_identity_invalidation");
	FString SectionError;
	UStaticMesh* SectionedCube = nullptr;
	int32 LeafSlot = INDEX_NONE;
	int32 WoodSlot = INDEX_NONE;
	int32 LeafTriangles = 0;
	int32 WoodTriangles = 0;
	bool bSectionFilter = false;
	bool bNonSolidMove = false;
	bool bCoefficientChange = false;
	bool bRoleChange = false;
	FString LeafDetails;

	if (Fixture.BuildSectionedCube(SectionedCube, LeafSlot, WoodSlot,
		LeafTriangles, WoodTriangles, SectionError))
	{
		TArray<FIMAcousticPhysicalMaterialMapping> LeafWoodProfiles;
		LeafWoodProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(LeafPhysicalMaterial, FVector(.22, .32, .42), .3f, false, false));
		LeafWoodProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(WoodPhysicalMaterial, FVector(.45, .35, .25), .55f, true, true));
		if (Fixture.StartCaptureCase(LeafWoodProfiles, FVector(1200), SectionError))
		{
			UStaticMeshComponent* SectionedComponent = Fixture.AddSectionedStatic(
				FTransform(FRotator(0, 12, 0), FVector(-100, 40, 15), FVector(1.1, .9, 1.2)),
				SectionedCube, LeafMaterial, WoodMaterial, LeafSlot, WoodSlot);
			IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture SectionCapture;
			IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary SectionExpected;
			IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary SectionActual;
			TArray<bool> LeafWoodSolid = {false, true};
			TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> SectionExpectedObjects;
			FString Error;
			if (SectionedComponent && Fixture.Capture(SectionCapture, Error))
			{
				IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& Expected = SectionExpectedObjects.AddDefaulted_GetRef();
				Expected.Mesh = SectionedCube;
				Expected.WorldTransform = SectionedComponent->GetComponentTransform();
				Expected.ProfileByMaterialSlot.Init(INDEX_NONE, SectionedCube->GetStaticMaterials().Num());
				Expected.ProfileByMaterialSlot[LeafSlot] = 0;
				Expected.ProfileByMaterialSlot[WoodSlot] = 1;
				int32 ExpectedNonSolidSections = 0;
				int32 ExpectedNonSolidTriangles = 0;
				const bool bOracle = IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(SectionExpectedObjects,
					Fixture.Volume->BakeBounds->GetComponentLocation(), LeafWoodSolid,
					SectionExpected, ExpectedNonSolidSections, ExpectedNonSolidTriangles, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(SectionCapture, SectionActual, Error);
				bSectionFilter = bOracle && IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(SectionExpected, SectionActual, Error)
					&& ExpectedNonSolidSections == 1
					&& ExpectedNonSolidTriangles == LeafTriangles
					&& SectionExpected.TriangleKeys.Num() == WoodTriangles
					&& Fixture.Volume->NonSolidSections == 1
					&& Fixture.Volume->NonSolidTriangles == LeafTriangles
					&& Fixture.Volume->ExportedTriangles == WoodTriangles;
				LeafDetails = FString::Printf(TEXT("two real sections: leaf slot=%d triangles=%d filtered; wood slot=%d triangles=%d retained; counters actual sections=%d triangles=%d exported=%d. %s"),
					LeafSlot, LeafTriangles, WoodSlot, WoodTriangles,
					Fixture.Volume->NonSolidSections, Fixture.Volume->NonSolidTriangles,
					Fixture.Volume->ExportedTriangles, *Error);
				LeafWoodCase.ExpectedTriangles = WoodTriangles;
				LeafWoodCase.ActualTriangles = SectionCapture.Triangles.Num();
				LeafWoodCase.ExpectedNonSolidSections = 1;
				LeafWoodCase.ActualNonSolidSections = Fixture.Volume->NonSolidSections;
				LeafWoodCase.ExpectedNonSolidTriangles = LeafTriangles;
				LeafWoodCase.ActualNonSolidTriangles = Fixture.Volume->NonSolidTriangles;
				LeafWoodCase.Fingerprint = SectionCapture.Fingerprint;
			}
			else
			{
				SectionError = Error.IsEmpty() ? TEXT("Could not create/capture the two-section leaf/wood fixture.") : Error;
			}
		}
	}

	if (SectionError.IsEmpty())
	{
		TArray<FIMAcousticPhysicalMaterialMapping> LeafWoodProfiles;
		LeafWoodProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(LeafPhysicalMaterial, FVector(.22, .32, .42), .3f, false, false));
		LeafWoodProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(WoodPhysicalMaterial, FVector(.45, .35, .25), .55f, true, true));
		if (Fixture.StartCaptureCase(LeafWoodProfiles, FVector(1200), SectionError))
		{
			UStaticMeshComponent* WoodAnchor = Fixture.AddStatic(
				FTransform(FRotator(0, 0, 0), FVector(-180, 0, 0)), Cube, WoodMaterial, WoodPhysicalMaterial);
			UStaticMeshComponent* LeafMotion = Fixture.AddStatic(
				FTransform(FRotator(0, 0, 0), FVector(180, 0, 0)), Cube, SlotMaterial, LeafPhysicalMaterial);
			if (!WoodAnchor || !LeafMotion)
			{
				SectionError = TEXT("Could not create the separate solid-wood anchor and non-solid leaf placement.");
			}
			else
			{
				TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> ExpectedObjects;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& WoodExpected = ExpectedObjects.AddDefaulted_GetRef();
				WoodExpected.Mesh = Cube;
				WoodExpected.WorldTransform = WoodAnchor->GetComponentTransform();
				WoodExpected.ProfileByMaterialSlot = IMPMUniformSlotMap(Cube, 1);
				TArray<bool> LeafWoodSolid = {false, true};
				IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary Expected;
				int32 ExpectedNonSolidSections = 0;
				int32 ExpectedNonSolidTriangles = 0;
				FString ExpectedError;
				bool bExpected = IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(ExpectedObjects,
					Fixture.Volume->BakeBounds->GetComponentLocation(), LeafWoodSolid,
					Expected, ExpectedNonSolidSections, ExpectedNonSolidTriangles, ExpectedError);

				IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureBeforeMove;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary BeforeMoveSummary;
				FString Error;
				bool bBeforeMove = Fixture.Capture(CaptureBeforeMove, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureBeforeMove, BeforeMoveSummary, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(Expected, BeforeMoveSummary, Error)
					&& Fixture.Volume->NonSolidSections == 1
					&& Fixture.Volume->NonSolidTriangles == CubeTriangles;
				LeafMotion->GetOwner()->SetActorLocation(FVector(280, 130, 65));
				IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureAfterMove;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary AfterMoveSummary;
				bool bAfterMove = Fixture.Capture(CaptureAfterMove, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureAfterMove, AfterMoveSummary, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(Expected, AfterMoveSummary, Error)
					&& Fixture.Volume->NonSolidSections == 1
					&& Fixture.Volume->NonSolidTriangles == CubeTriangles;
				bNonSolidMove = bExpected && bBeforeMove && bAfterMove
					&& CaptureBeforeMove.Fingerprint != CaptureAfterMove.Fingerprint;

				const FString MoveFingerprint = CaptureAfterMove.Fingerprint;
				Fixture.Volume->PhysicalMaterials[0].Absorption = FVector(.28, .36, .44);
				Fixture.Volume->PhysicalMaterials[0].Scattering = .41f;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureAfterCoefficient;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary CoefficientSummary;
				bCoefficientChange = Fixture.Capture(CaptureAfterCoefficient, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureAfterCoefficient, CoefficientSummary, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(Expected, CoefficientSummary, Error)
					&& Fixture.Volume->NonSolidTriangles == CubeTriangles
					&& CaptureAfterCoefficient.Fingerprint != MoveFingerprint;

				const FString CoefficientFingerprint = CaptureAfterCoefficient.Fingerprint;
				Fixture.Volume->PhysicalMaterials[0].bSolidGeometry = true;
				Fixture.Volume->PhysicalMaterials[0].bSupportsProbes = true;
				TArray<IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject> BothExpectedObjects = ExpectedObjects;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMExpectedObject& LeafExpected = BothExpectedObjects.AddDefaulted_GetRef();
				LeafExpected.Mesh = Cube;
				LeafExpected.WorldTransform = LeafMotion->GetComponentTransform();
				LeafExpected.ProfileByMaterialSlot = IMPMUniformSlotMap(Cube, 0);
				TArray<bool> AllSolid = {true, true};
				IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary BothExpected;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMGeometrySummary BothActual;
				int32 NoExpectedNonSolidSections = 0;
				int32 NoExpectedNonSolidTriangles = 0;
				IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture CaptureAfterRole;
				bRoleChange = IMAcousticPhysicalMaterialTestPrivate::IMPMBuildExpected(BothExpectedObjects,
						Fixture.Volume->BakeBounds->GetComponentLocation(), AllSolid,
						BothExpected, NoExpectedNonSolidSections, NoExpectedNonSolidTriangles, Error)
					&& Fixture.Capture(CaptureAfterRole, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMSummarizeCapture(CaptureAfterRole, BothActual, Error)
					&& IMAcousticPhysicalMaterialTestPrivate::IMPMCompareGeometry(BothExpected, BothActual, Error)
					&& Fixture.Volume->NonSolidSections == 0
					&& Fixture.Volume->NonSolidTriangles == 0
					&& CaptureAfterRole.Fingerprint != CoefficientFingerprint;

				LeafWoodCase.bPassed = bSectionFilter && bNonSolidMove
					&& bCoefficientChange && bRoleChange;
				LeafWoodCase.ActualTriangles = CaptureAfterRole.Triangles.Num();
				LeafWoodCase.ExpectedTriangles = BothExpected.TriangleKeys.Num();
				LeafWoodCase.ActualNonSolidSections = Fixture.Volume->NonSolidSections;
				LeafWoodCase.ExpectedNonSolidSections = 0;
				LeafWoodCase.ActualNonSolidTriangles = Fixture.Volume->NonSolidTriangles;
				LeafWoodCase.ExpectedNonSolidTriangles = 0;
				LeafWoodCase.Fingerprint = CaptureAfterRole.Fingerprint;
				LeafDetails += FString::Printf(TEXT("Moved non-solid-only leaf changed fingerprint=%s with identical wood-anchor geometry; coefficient edit changed fingerprint=%s with geometry preserved; role edit changed fingerprint=%s and restored leaf geometry. movement=%s coefficient=%s role=%s oracle_error=%s"),
					CaptureBeforeMove.Fingerprint != CaptureAfterMove.Fingerprint ? TEXT("true") : TEXT("false"),
					CaptureAfterCoefficient.Fingerprint != MoveFingerprint ? TEXT("true") : TEXT("false"),
					CaptureAfterRole.Fingerprint != CoefficientFingerprint ? TEXT("true") : TEXT("false"),
					bNonSolidMove ? TEXT("PASS") : TEXT("FAIL"),
					bCoefficientChange ? TEXT("PASS") : TEXT("FAIL"),
					bRoleChange ? TEXT("PASS") : TEXT("FAIL"), *Error);
			}
		}
	}
	LeafWoodCase.bPassed = LeafWoodCase.bPassed || (bSectionFilter && bNonSolidMove && bCoefficientChange && bRoleChange);
	if (LeafWoodCase.Details.IsEmpty())
	{
		LeafWoodCase.Details = LeafDetails + TEXT(" ") + SectionError;
	}
	else
	{
		LeafWoodCase.Details += TEXT(" ") + LeafDetails + TEXT(" ") + SectionError;
	}
	Cases.Add(LeafWoodCase);

	// Case 8: probe support is independent of solid geometry. The SDK's 5 m
	// grid uses half-metre columns, so put the floating object on a real column.
	IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult ProbeCase;
	ProbeCase.Name = TEXT("probe_support_floor_height_floating_wood_exclusion_and_empty_support_failure");
	TArray<FIMAcousticPhysicalMaterialMapping> ProbeProfiles;
	ProbeProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(SlotPhysicalMaterial, FVector(.1, .12, .14), .15f, true, true));
	ProbeProfiles.Add(IMAcousticPhysicalMaterialTestPrivate::IMPMMakeProfile(WoodPhysicalMaterial, FVector(.4, .32, .22), .45f, true, false));
	FString ProbeError;
	bool bProbePositive = false;
	bool bProbeEmptyFailure = false;
	FString ProbeDetails;
	if (Fixture.StartCaptureCase(ProbeProfiles, FVector(250, 250, 200), ProbeError))
	{
		Fixture.Volume->ProbeHeightCm = 100.0f;
		Fixture.Volume->ProbeSpacingCm = 100.0f;
		const FTransform FloorTransform(FRotator::ZeroRotator,
			FVector(0, 0, -5), FVector(4.0, 4.0, .1));
		const FTransform FloatingWoodTransform(FRotator::ZeroRotator,
			FVector(50, 50, 50), FVector(.25, .25, .25));
		UStaticMeshComponent* Floor = Fixture.AddStatic(FloorTransform, Cube, SlotMaterial);
		UStaticMeshComponent* FloatingWood = Fixture.AddStatic(
			FloatingWoodTransform, Cube, WoodMaterial);
		IMAcousticPhysicalMaterialTestPrivate::FIMPMCapture ProbeCapture;
		FString CaptureError;
		const bool bGeometryCaptured = Floor && FloatingWood
			&& Fixture.Capture(ProbeCapture, CaptureError)
			&& Fixture.Volume->ExportedTriangles == 2 * CubeTriangles
			&& Fixture.Volume->NonSolidSections == 0
			&& Fixture.Volume->NonSolidTriangles == 0;
		TArray<FVector> ProbeCenters;
		if (bGeometryCaptured)
		{
			const FTransform BoundsTransform = Fixture.Volume->BakeBounds->GetComponentTransform();
			const bool bUnitAxisBounds = BoundsTransform.GetRotation().Equals(FQuat::Identity)
				&& BoundsTransform.GetScale3D().Equals(FVector::OneVector);
			Fixture.Volume->GenerateProbes();
			Fixture.Volume->IM_GetGeneratedProbeCentersForTest(ProbeCenters);
			bool bAllGroundColumnsFound = ProbeCenters.Num() == 16;
			for (int32 X = 0; X < 4; ++X)
			{
				for (int32 Z = 0; Z < 4; ++Z)
				{
					const FVector Expected(-1.5 + X, 1.0, -1.5 + Z);
					bAllGroundColumnsFound &= ProbeCenters.ContainsByPredicate([&Expected](const FVector& Center)
					{
						return FVector::Distance(Center, Expected) <= .001;
					});
				}
			}
			const FVector FloatingSupportCenter(.5, 1.625, -.5);
			const auto HasFloatingSupport = [&FloatingSupportCenter](const TArray<FVector>& Centers)
			{
				return Centers.ContainsByPredicate([&FloatingSupportCenter](const FVector& Center)
				{
					return FVector::Distance(Center, FloatingSupportCenter) <= .001;
				});
			};
			const bool bFloatingWoodCenterFound = HasFloatingSupport(ProbeCenters);
			const bool bGroundPositive = bUnitAxisBounds && bAllGroundColumnsFound
				&& Fixture.Volume->GeneratedProbes == ProbeCenters.Num()
				&& !bFloatingWoodCenterFound;

			// Positive control proves this ray actually intersects the object:
			// change only support participation, retaining the full solid scene.
			Fixture.Volume->PhysicalMaterials[1].bSupportsProbes = true;
			Fixture.Volume->GenerateProbes();
			TArray<FVector> WithWoodSupport;
			Fixture.Volume->IM_GetGeneratedProbeCentersForTest(WithWoodSupport);
			const bool bWoodSupportPositive = HasFloatingSupport(WithWoodSupport);
			bProbePositive = bGroundPositive && bWoodSupportPositive;
			ProbeDetails = FString::Printf(TEXT("bounds rotation=%s scale=%s; floor center Z=-5cm scale=(4,4,0.1) top=0cm; floating solid wood center=(50,50,50)cm scale=(0.25,0.25,0.25); spacing=100cm height=100cm; supports=false generated=%d all_16_ground_columns=%s floating_wood_height_sample=%s; only wood support=true elevated_column_positive=%s; status=%s"),
				bUnitAxisBounds ? TEXT("identity") : TEXT("invalid"), *BoundsTransform.GetScale3D().ToString(),
				ProbeCenters.Num(), bAllGroundColumnsFound ? TEXT("found") : TEXT("missing"),
				bFloatingWoodCenterFound ? TEXT("found") : TEXT("absent"),
				bWoodSupportPositive ? TEXT("found") : TEXT("missing"), *Fixture.Volume->Status);
			ProbeCase.ExpectedTriangles = 2 * CubeTriangles;
			ProbeCase.ActualTriangles = ProbeCapture.Triangles.Num();
			ProbeCase.Fingerprint = ProbeCapture.Fingerprint;
		}
		else
		{
			ProbeError = CaptureError.IsEmpty() ? TEXT("Floor/floating-wood geometry did not satisfy the solid scene contract.") : CaptureError;
		}

		TArray<FIMAcousticPhysicalMaterialMapping> NoSupportProfiles = ProbeProfiles;
		for (FIMAcousticPhysicalMaterialMapping& Profile : NoSupportProfiles)
		{
			Profile.bSolidGeometry = true;
			Profile.bSupportsProbes = false;
		}
		if (Fixture.StartCaptureCase(NoSupportProfiles, FVector(250, 250, 200), ProbeError))
		{
			Fixture.Volume->ProbeHeightCm = 100.0f;
			Fixture.Volume->ProbeSpacingCm = 100.0f;
			Fixture.AddStatic(FloorTransform, Cube, SlotMaterial);
			Fixture.AddStatic(FloatingWoodTransform, Cube, WoodMaterial);
			Fixture.Volume->GenerateProbes();
			TArray<FVector> NoSupportCenters;
			Fixture.Volume->IM_GetGeneratedProbeCentersForTest(NoSupportCenters);
			bProbeEmptyFailure = Fixture.Volume->Status.Contains(TEXT("No physical profile supports probe placement"))
				&& Fixture.Volume->GeneratedProbes == 0 && NoSupportCenters.IsEmpty();
			ProbeDetails += FString::Printf(TEXT("; all profiles supports=false => explicit_failure=%s generated=%d centers=%d status=%s"),
				bProbeEmptyFailure ? TEXT("true") : TEXT("false"), Fixture.Volume->GeneratedProbes,
				NoSupportCenters.Num(), *Fixture.Volume->Status);
		}
		else
		{
			ProbeDetails += TEXT("; no-support fixture failed: ") + ProbeError;
		}
	}
	else
	{
		ProbeDetails = ProbeError;
	}
	ProbeCase.bPassed = bProbePositive && bProbeEmptyFailure;
	ProbeCase.Details = ProbeDetails;
	Cases.Add(ProbeCase);

	bool bAllCasesPassed = Cases.Num() == 8;
	for (const IMAcousticPhysicalMaterialTestPrivate::FIMPMCaseResult& Case : Cases)
	{
		bAllCasesPassed &= Case.bPassed;
		if (!Case.bPassed)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Case.Name, *Case.Details));
		}
	}

	const bool bMapRestored = Fixture.RestoreMap();
	if (!bMapRestored)
	{
		AddError(TEXT("The original whitebox map identity could not be restored after the transient test fixture."));
	}
	FString ReportPath;
	const bool bReportWritten = IMAcousticPhysicalMaterialTestPrivate::IMPMWriteReport(RunId, Fixture.MapIdentity,
		GetPathNameSafe(Cube), Cases, bAllCasesPassed && bMapRestored,
		bAllCasesPassed && bMapRestored
			? TEXT("Eight bounded PhysicalMaterial scene-contract cases passed.")
			: TEXT("One or more PhysicalMaterial scene-contract cases or map restoration checks failed."),
		ReportPath);
	if (!bReportWritten)
	{
		AddError(FString::Printf(TEXT("Could not write the gate report under Saved/AcousticV2/PhysicalMaterialDrive: %s"), *ReportPath));
	}
	AddInfo(FString::Printf(TEXT("PhysicalMaterial scene-contract report: %s"), *ReportPath));

	const bool bPassed = bAllCasesPassed && bMapRestored && bReportWritten;
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s PhysicalMaterialSceneContract"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
	if (bPassed)
	{
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] PASS PhysicalMaterial.SceneContract gate-only report=%s"), *ReportPath);
		return true;
	}
	UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] FAIL PhysicalMaterial.SceneContract gate-only report=%s"), *ReportPath);
	return false;
}
#endif
