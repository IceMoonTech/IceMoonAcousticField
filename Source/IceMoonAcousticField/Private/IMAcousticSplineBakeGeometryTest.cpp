#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticCoordinates.h"
#include "Components/BoxComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

namespace IMAcousticSplineBakeGeometryTestPrivate
{
constexpr TCHAR IMG1SplineMap[] = TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
constexpr TCHAR IMG1IgnoreTag[] = TEXT("IMAcousticIgnore");
constexpr TCHAR IMG1RequiredTag[] = TEXT("IMAcousticRequired");
constexpr float IMG1VertexToleranceMeters = 1.0e-4f; // 0.01 cm: covers FVector3f collision/render storage and SDK float conversion.
constexpr float IMG1MinimumBendMeters = 0.05f;       // 5 cm, below the configured 75 cm Hermite midpoint offset.

struct FIMG1Triangle
{
	FVector Vertices[3];
	int32 MaterialIndex = INDEX_NONE;
};

struct FIMG1Snapshot
{
	TArray<FVector> Vertices;
	TArray<FIMG1Triangle> Triangles;
	FString Fingerprint;
};

bool IMG1PointLess(const FVector& A, const FVector& B)
{
	if (A.X != B.X) return A.X < B.X;
	if (A.Y != B.Y) return A.Y < B.Y;
	return A.Z < B.Z;
}

FIMG1Triangle IMG1CanonicalizeWinding(const FIMG1Triangle& Input)
{
	// Canonicalize only cyclic rotations. Reversing two corners remains a mismatch,
	// so this multiset comparison still proves oriented triangle winding.
	int32 BestRotation = 0;
	for (int32 Rotation = 1; Rotation < 3; ++Rotation)
	{
		bool bLess = false;
		bool bDifferent = false;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector& Candidate = Input.Vertices[(Rotation + Corner) % 3];
			const FVector& Current = Input.Vertices[(BestRotation + Corner) % 3];
			if (Candidate.Equals(Current, 0.0)) continue;
			bLess = IMG1PointLess(Candidate, Current);
			bDifferent = true;
			break;
		}
		if (bDifferent && bLess) BestRotation = Rotation;
	}

	FIMG1Triangle Result;
	Result.MaterialIndex = Input.MaterialIndex;
	for (int32 Corner = 0; Corner < 3; ++Corner)
		Result.Vertices[Corner] = Input.Vertices[(BestRotation + Corner) % 3];
	return Result;
}

bool IMG1VectorClose(const FVector& A, const FVector& B, const float ToleranceMeters)
{
	return (A - B).GetAbsMax() <= ToleranceMeters;
}

bool IMG1CompareVertexMultisets(const TArray<FVector>& Expected, const TArray<FVector>& Actual,
	const float ToleranceMeters, FString& Failure)
{
	if (Expected.Num() != Actual.Num())
	{
		Failure = FString::Printf(TEXT("vertex count differs: collision=%d capture=%d"), Expected.Num(), Actual.Num());
		return false;
	}
	TArray<bool> Matched;
	Matched.Init(false, Actual.Num());
	for (int32 ExpectedIndex = 0; ExpectedIndex < Expected.Num(); ++ExpectedIndex)
	{
		int32 MatchIndex = INDEX_NONE;
		for (int32 ActualIndex = 0; ActualIndex < Actual.Num(); ++ActualIndex)
		{
			if (!Matched[ActualIndex] && IMG1VectorClose(Expected[ExpectedIndex], Actual[ActualIndex], ToleranceMeters))
			{
				MatchIndex = ActualIndex;
				break;
			}
		}
		if (MatchIndex == INDEX_NONE)
		{
			Failure = FString::Printf(TEXT("no collision vertex within %.6f m of capture vertex %d=(%.6f,%.6f,%.6f)"),
				ToleranceMeters, ExpectedIndex, Expected[ExpectedIndex].X, Expected[ExpectedIndex].Y, Expected[ExpectedIndex].Z);
			return false;
		}
		Matched[MatchIndex] = true;
	}
	return true;
}

bool IMG1TriangleClose(const FIMG1Triangle& A, const FIMG1Triangle& B, const float ToleranceMeters)
{
	if (A.MaterialIndex != B.MaterialIndex) return false;
	for (int32 Corner = 0; Corner < 3; ++Corner)
		if (!IMG1VectorClose(A.Vertices[Corner], B.Vertices[Corner], ToleranceMeters)) return false;
	return true;
}

bool IMG1CompareTriangleMultisets(const TArray<FIMG1Triangle>& Expected, const TArray<FIMG1Triangle>& Actual,
	const float ToleranceMeters, FString& Failure)
{
	if (Expected.Num() != Actual.Num())
	{
		Failure = FString::Printf(TEXT("triangle count differs: collision=%d capture=%d"), Expected.Num(), Actual.Num());
		return false;
	}
	TArray<bool> Matched;
	Matched.Init(false, Actual.Num());
	for (const FIMG1Triangle& UncanonicalExpected : Expected)
	{
		const FIMG1Triangle CanonicalExpected = IMG1CanonicalizeWinding(UncanonicalExpected);
		int32 MatchIndex = INDEX_NONE;
		for (int32 ActualIndex = 0; ActualIndex < Actual.Num(); ++ActualIndex)
		{
			if (!Matched[ActualIndex]
				&& IMG1TriangleClose(CanonicalExpected, IMG1CanonicalizeWinding(Actual[ActualIndex]), ToleranceMeters))
			{
				MatchIndex = ActualIndex;
				break;
			}
		}
		if (MatchIndex == INDEX_NONE)
		{
			Failure = FString::Printf(TEXT("oriented collision triangle with material %d has no capture match"), CanonicalExpected.MaterialIndex);
			return false;
		}
		Matched[MatchIndex] = true;
	}
	return true;
}

bool IMG1BuildCollisionOracle(USplineMeshComponent* Spline, const FVector& AcousticOrigin,
	const TMap<UMaterialInterface*, int32>& MaterialIndices, TArray<FVector>& OutVertices,
	TArray<FIMG1Triangle>& OutTriangles, FString& Failure)
{
	FTriMeshCollisionData Collision;
	if (!Spline || !Spline->GetPhysicsTriMeshData(&Collision, true))
	{
		Failure = TEXT("UE spline collision provider did not return LOD0 triangle data.");
		return false;
	}
	if (Collision.Vertices.IsEmpty() || Collision.Indices.IsEmpty()
		|| Collision.MaterialIndices.Num() != Collision.Indices.Num())
	{
		Failure = FString::Printf(TEXT("UE collision oracle is incomplete: vertices=%d triangles=%d material_indices=%d"),
			Collision.Vertices.Num(), Collision.Indices.Num(), Collision.MaterialIndices.Num());
		return false;
	}

	const FTransform ComponentTransform = Spline->GetComponentTransform();
	OutVertices.Reserve(Collision.Vertices.Num());
	for (const FVector3f& CollisionVertex : Collision.Vertices)
	{
		const FVector WorldPosition = ComponentTransform.TransformPosition(FVector(CollisionVertex));
		const IPLVector3 SDKPosition = ToSDKPosition(WorldPosition, AcousticOrigin);
		const FVector Converted(SDKPosition.x, SDKPosition.y, SDKPosition.z);
		if (!FMath::IsFinite(Converted.X) || !FMath::IsFinite(Converted.Y) || !FMath::IsFinite(Converted.Z))
		{
			Failure = TEXT("UE collision oracle produced a non-finite world vertex.");
			return false;
		}
		OutVertices.Add(Converted);
	}

	OutTriangles.Reserve(Collision.Indices.Num());
	for (int32 TriangleIndex = 0; TriangleIndex < Collision.Indices.Num(); ++TriangleIndex)
	{
		const FTriIndices& Indices = Collision.Indices[TriangleIndex];
		if (!Collision.Vertices.IsValidIndex(Indices.v0) || !Collision.Vertices.IsValidIndex(Indices.v1)
			|| !Collision.Vertices.IsValidIndex(Indices.v2))
		{
			Failure = FString::Printf(TEXT("UE collision oracle triangle %d references an invalid vertex."), TriangleIndex);
			return false;
		}
		const uint16 SourceMaterialSlot = Collision.MaterialIndices[TriangleIndex];
		if (SourceMaterialSlot >= Spline->GetNumMaterials())
		{
			Failure = FString::Printf(TEXT("UE collision oracle triangle %d references material slot %d outside the component."),
				TriangleIndex, int32(SourceMaterialSlot));
			return false;
		}
		UMaterialInterface* EffectiveMaterial = Spline->GetMaterial(SourceMaterialSlot);
		const int32* AcousticMaterialIndex = MaterialIndices.Find(EffectiveMaterial);
		if (!AcousticMaterialIndex)
		{
			Failure = FString::Printf(TEXT("UE collision oracle triangle %d has no explicit acoustic mapping."), TriangleIndex);
			return false;
		}

		FIMG1Triangle Triangle;
		Triangle.Vertices[0] = OutVertices[Indices.v0];
		Triangle.Vertices[1] = OutVertices[Indices.v1];
		Triangle.Vertices[2] = OutVertices[Indices.v2];
		Triangle.MaterialIndex = *AcousticMaterialIndex;
		OutTriangles.Add(Triangle);
	}
	return true;
}

bool IMG1CaptureAndCompare(AIMAcousticBakeVolume* Volume, USplineMeshComponent* Spline,
	const FVector& AcousticOrigin, const TMap<UMaterialInterface*, int32>& MaterialIndices,
	FIMG1Snapshot& OutSnapshot, int32& OutCollisionVertexCount, int32& OutCollisionTriangleCount, FString& Failure)
{
	TArray<FVector> ExpectedVertices;
	TArray<FIMG1Triangle> ExpectedTriangles;
	if (!IMG1BuildCollisionOracle(Spline, AcousticOrigin, MaterialIndices, ExpectedVertices, ExpectedTriangles, Failure))
		return false;
	OutCollisionVertexCount = ExpectedVertices.Num();
	OutCollisionTriangleCount = ExpectedTriangles.Num();

	TArray<FIntVector> CapturedTriangles;
	TArray<int32> CapturedMaterialIndices;
	if (!Volume->IM_CaptureSceneGeometryForTest(OutSnapshot.Vertices, CapturedTriangles, CapturedMaterialIndices, OutSnapshot.Fingerprint))
	{
		Failure = TEXT("BakeVolume failed to capture the spline fixture scene: ") + Volume->Status;
		return false;
	}
	if (OutSnapshot.Vertices.Num() != ExpectedVertices.Num())
	{
		Failure = FString::Printf(TEXT("exported vertex count differs from UE collision oracle: capture=%d collision=%d"),
			OutSnapshot.Vertices.Num(), ExpectedVertices.Num());
		return false;
	}
	if (CapturedTriangles.Num() != CapturedMaterialIndices.Num())
	{
		Failure = FString::Printf(TEXT("capture material-index count differs from triangle count: triangles=%d materials=%d"),
			CapturedTriangles.Num(), CapturedMaterialIndices.Num());
		return false;
	}

	if (!IMG1CompareVertexMultisets(ExpectedVertices, OutSnapshot.Vertices, IMG1VertexToleranceMeters, Failure)) return false;
	OutSnapshot.Triangles.Reserve(CapturedTriangles.Num());
	for (int32 TriangleIndex = 0; TriangleIndex < CapturedTriangles.Num(); ++TriangleIndex)
	{
		const FIntVector& Indices = CapturedTriangles[TriangleIndex];
		if (!OutSnapshot.Vertices.IsValidIndex(Indices.X) || !OutSnapshot.Vertices.IsValidIndex(Indices.Y)
			|| !OutSnapshot.Vertices.IsValidIndex(Indices.Z))
		{
			Failure = FString::Printf(TEXT("capture triangle %d references an invalid vertex."), TriangleIndex);
			return false;
		}
		FIMG1Triangle Triangle;
		Triangle.Vertices[0] = OutSnapshot.Vertices[Indices.X];
		Triangle.Vertices[1] = OutSnapshot.Vertices[Indices.Y];
		Triangle.Vertices[2] = OutSnapshot.Vertices[Indices.Z];
		Triangle.MaterialIndex = CapturedMaterialIndices[TriangleIndex];
		OutSnapshot.Triangles.Add(Triangle);
	}
	if (!IMG1CompareTriangleMultisets(ExpectedTriangles, OutSnapshot.Triangles, IMG1VertexToleranceMeters, Failure)) return false;
	if (OutSnapshot.Fingerprint.IsEmpty())
	{
		Failure = TEXT("capture returned an empty scene fingerprint.");
		return false;
	}
	return true;
}

double IMG1MaxNearestVertexDistance(const TArray<FVector>& A, const TArray<FVector>& B)
{
	double MaximumNearestDistance = 0.0;
	for (const FVector& Point : A)
	{
		double NearestDistanceSquared = TNumericLimits<double>::Max();
		for (const FVector& Candidate : B)
			NearestDistanceSquared = FMath::Min(NearestDistanceSquared, FVector::DistSquared(Point, Candidate));
		MaximumNearestDistance = FMath::Max(MaximumNearestDistance, FMath::Sqrt(NearestDistanceSquared));
	}
	return MaximumNearestDistance;
}

struct FIMG1TransientRestore
{
	TWeakObjectPtr<AIMAcousticBakeVolume> TestVolume;
	TWeakObjectPtr<AActor> SplineOwner;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> AddedIgnoreTags;
	UPackage* FixturePackage = nullptr;
	bool bFixtureWasDirty = false;
	bool bFixtureDirtyRestored = false;
	bool bRestorePreviousMap = false;
	FString PreviousMapPath;
	bool bRestored = false;
	bool bRestoreSucceeded = true;

	bool Restore()
	{
		if (bRestored) return bRestoreSucceeded;
		bRestored = true;
		if (SplineOwner.IsValid() && !SplineOwner->Destroy()) bRestoreSucceeded = false;
		if (TestVolume.IsValid() && !TestVolume->Destroy()) bRestoreSucceeded = false;
		for (const TWeakObjectPtr<UPrimitiveComponent>& WeakComponent : AddedIgnoreTags)
		{
			if (!WeakComponent.IsValid()) continue;
			WeakComponent->ComponentTags.Remove(FName(IMG1IgnoreTag));
			if (WeakComponent->ComponentHasTag(IMG1IgnoreTag)) bRestoreSucceeded = false;
		}
		if (FixturePackage && IsValid(FixturePackage))
		{
			FixturePackage->SetDirtyFlag(bFixtureWasDirty);
			bFixtureDirtyRestored = FixturePackage->IsDirty() == bFixtureWasDirty;
		}

		if (GEditor)
		{
			UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World();
			const FString CurrentMap = CurrentWorld && CurrentWorld->GetOutermost()
				? UWorld::RemovePIEPrefix(CurrentWorld->GetOutermost()->GetName()) : FString();
			if (bRestorePreviousMap && CurrentMap != PreviousMapPath
				&& (!FEditorFileUtils::LoadMap(PreviousMapPath, false, true)))
				bRestoreSucceeded = false;
			UWorld* RestoredWorld = GEditor->GetEditorWorldContext().World();
			const FString RestoredMap = RestoredWorld && RestoredWorld->GetOutermost()
				? UWorld::RemovePIEPrefix(RestoredWorld->GetOutermost()->GetName()) : FString();
			if (RestoredMap != PreviousMapPath) bRestoreSucceeded = false;
		}
		else if (bRestorePreviousMap) bRestoreSucceeded = false;
		return bRestoreSucceeded;
	}

	~FIMG1TransientRestore() { Restore(); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticG1SplineBakeGeometry,
	"IceMoon.AcousticField.G1.SplineBakeGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool IM_AcousticG1SplineBakeGeometry::RunTest(const FString&)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/G1SplineBake"), RunId);
	FString Failure;
	FString InitialMapPath;
	FString OriginalBakeAssetPath;
	FString InitialFingerprint;
	FString MovedFingerprint;
	FString RestoredFingerprint;
	FString SourceMaterialPath;
	int32 InitialVertices = 0, InitialTriangles = 0, InitialCollisionVertices = 0, InitialCollisionTriangles = 0;
	int32 MovedVertices = 0, MovedTriangles = 0, MovedCollisionVertices = 0, MovedCollisionTriangles = 0;
	int32 RestoredVertices = 0, RestoredTriangles = 0, RestoredCollisionVertices = 0, RestoredCollisionTriangles = 0;
	bool bInitialOracleMatch = false, bMovedOracleMatch = false, bRestoredOracleMatch = false;
	bool bReversedWindingRejected = false, bFingerprintChangedOnMove = false, bFingerprintRestored = false;
	bool bAssetUnchanged = false, bMapRestored = false;
	bool bSuccess = true;
	auto Fail = [this, &bSuccess, &Failure](const FString& Message)
	{
		AddError(Message);
		if (Failure.IsEmpty()) Failure = Message;
		bSuccess = false;
	};

	if (!IFileManager::Get().MakeDirectory(*ReportDirectory, true))
		Fail(TEXT("Cannot create G1 spline test evidence directory."));

	IMAcousticSplineBakeGeometryTestPrivate::FIMG1Snapshot InitialSnapshot, MovedSnapshot, RestoredSnapshot;
	IMAcousticSplineBakeGeometryTestPrivate::FIMG1TransientRestore Restore;
	FString OriginalMapPackagePath;
	bool bOriginalMapWasDirty = false;
	UPackage* OriginalMapPackage = nullptr;
	UStaticMesh* SourceMesh = nullptr;
	UMaterialInterface* FirstSourceMaterial = nullptr;
	UPackage* SourceMeshPackage = nullptr;
	UPackage* SourceMaterialPackage = nullptr;
	bool bSourceMeshPackageWasDirty = false;
	bool bSourceMaterialPackageWasDirty = false;
	UIMAcousticBakeAsset* OriginalBakeAsset = nullptr;
	UPackage* OriginalBakeAssetPackage = nullptr;
	bool bOriginalBakeAssetPackageWasDirty = false;
	AIMAcousticBakeVolume* ExistingVolume = nullptr;
	AIMAcousticBakeVolume* FixtureVolume = nullptr;
	AActor* SplineOwner = nullptr;
	USplineMeshComponent* Spline = nullptr;
	FVector AcousticOrigin = FVector::ZeroVector;
	TMap<UMaterialInterface*, int32> MaterialIndices;
	TArray<FVector> RawStaticVertices;

	do
	{
		if (!bSuccess) break;
		if (!GEditor)
		{
			Fail(TEXT("Editor automation requires GEditor."));
			break;
		}
		UWorld* StartingWorld = GEditor->GetEditorWorldContext().World();
		if (!StartingWorld || StartingWorld->IsGameWorld() || !StartingWorld->GetOutermost())
		{
			Fail(TEXT("Could not identify the non-PIE Editor map to restore."));
			break;
		}
		OriginalMapPackagePath = UWorld::RemovePIEPrefix(StartingWorld->GetOutermost()->GetName());
		OriginalMapPackage = StartingWorld->GetOutermost();
		bOriginalMapWasDirty = OriginalMapPackage->IsDirty();
		InitialMapPath = OriginalMapPackagePath;
		Restore.PreviousMapPath = OriginalMapPackagePath;
		Restore.bRestorePreviousMap = OriginalMapPackagePath != IMAcousticSplineBakeGeometryTestPrivate::IMG1SplineMap;
		if (Restore.bRestorePreviousMap && bOriginalMapWasDirty)
		{
			Fail(TEXT("Refusing to switch away from a dirty Editor map; restore would discard unsaved state."));
			break;
		}
		if (Restore.bRestorePreviousMap && !FPackageName::DoesPackageExist(OriginalMapPackagePath))
		{
			Fail(TEXT("Starting Editor map has no loadable package path for restoration."));
			break;
		}

		if (OriginalMapPackagePath != IMAcousticSplineBakeGeometryTestPrivate::IMG1SplineMap
			&& !FEditorFileUtils::LoadMap(IMAcousticSplineBakeGeometryTestPrivate::IMG1SplineMap, false, true))
		{
			Fail(TEXT("Existing IceMoon acoustic whitebox map could not be loaded."));
			break;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World || World->IsGameWorld() || !World->GetOutermost()
			|| UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) != IMAcousticSplineBakeGeometryTestPrivate::IMG1SplineMap)
		{
			Fail(TEXT("Active Editor world does not match the required acoustic map."));
			break;
		}
		Restore.FixturePackage = World->GetOutermost();
		Restore.bFixtureWasDirty = Restore.FixturePackage->IsDirty();

		int32 ExistingVolumeCount = 0;
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
		{
			++ExistingVolumeCount;
			ExistingVolume = *It;
		}
		if (ExistingVolumeCount != 1 || !ExistingVolume || !ExistingVolume->BakedField)
		{
			Fail(FString::Printf(TEXT("Expected one existing acoustic bake volume with a baked asset; found=%d."), ExistingVolumeCount));
			break;
		}
		OriginalBakeAsset = ExistingVolume->BakedField;
		OriginalBakeAssetPath = GetPathNameSafe(OriginalBakeAsset);
		OriginalBakeAssetPackage = OriginalBakeAsset->GetOutermost();
		bOriginalBakeAssetPackageWasDirty = OriginalBakeAssetPackage && OriginalBakeAssetPackage->IsDirty();

		SourceMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!SourceMesh || !SourceMesh->GetRenderData() || SourceMesh->GetRenderData()->LODResources.IsEmpty()
			|| SourceMesh->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices() == 0)
		{
			Fail(TEXT("Engine BasicShapes Cube LOD0 render vertices are unavailable."));
			break;
		}
		SourceMeshPackage = SourceMesh->GetOutermost();
		bSourceMeshPackageWasDirty = SourceMeshPackage && SourceMeshPackage->IsDirty();
		if (SourceMesh->GetStaticMaterials().IsEmpty())
		{
			Fail(TEXT("Engine BasicShapes Cube has no material slot for the explicit acoustic mapping."));
			break;
		}

		// Isolate the transient spline fixture from every pre-existing non-required
		// map component. Newly-added tags are restored below; no map package is saved.
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UPrimitiveComponent*> Components;
			It->GetComponents(Components);
			for (UPrimitiveComponent* Component : Components)
			{
				if (!Component || Component->ComponentHasTag(IMAcousticSplineBakeGeometryTestPrivate::IMG1RequiredTag)
					|| Component->ComponentHasTag(IMAcousticSplineBakeGeometryTestPrivate::IMG1IgnoreTag)) continue;
				Component->ComponentTags.Add(FName(IMAcousticSplineBakeGeometryTestPrivate::IMG1IgnoreTag));
				Restore.AddedIgnoreTags.Add(Component);
			}
		}

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		FixtureVolume = World->SpawnActor<AIMAcousticBakeVolume>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
		if (!FixtureVolume || !FixtureVolume->BakeBounds)
		{
			Fail(TEXT("Could not create a transient acoustic bake volume fixture."));
			break;
		}
		FixtureVolume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
		Restore.TestVolume = FixtureVolume;
		FixtureVolume->SetActorLocation(FVector::ZeroVector);
		FixtureVolume->BakeBounds->SetBoxExtent(FVector(500.0), false);

		for (int32 SlotIndex = 0; SlotIndex < SourceMesh->GetStaticMaterials().Num(); ++SlotIndex)
		{
			UMaterialInterface* Material = SourceMesh->GetMaterial(SlotIndex);
			if (!Material)
			{
				Fail(FString::Printf(TEXT("Cube material slot %d is null."), SlotIndex));
				break;
			}
			int32* ExistingIndex = MaterialIndices.Find(Material);
			if (!ExistingIndex)
			{
				FIMAcousticMaterialMapping Mapping;
				Mapping.Material = Material;
				const int32 MappingIndex = FixtureVolume->Materials.Add(Mapping);
				MaterialIndices.Add(Material, MappingIndex);
			}
			if (SlotIndex == 0)
			{
				FirstSourceMaterial = Material;
				SourceMaterialPath = GetPathNameSafe(Material);
			}
		}
		if (!bSuccess) break;
		if (!FirstSourceMaterial)
		{
			Fail(TEXT("Could not resolve the Cube's first material slot."));
			break;
		}
		SourceMaterialPackage = FirstSourceMaterial->GetOutermost();
		bSourceMaterialPackageWasDirty = SourceMaterialPackage && SourceMaterialPackage->IsDirty();

		SplineOwner = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
		if (!SplineOwner)
		{
			Fail(TEXT("Could not create a transient spline owner actor."));
			break;
		}
		Restore.SplineOwner = SplineOwner;
		Spline = NewObject<USplineMeshComponent>(SplineOwner, NAME_None, RF_Transient);
		if (!Spline)
		{
			Fail(TEXT("Could not create a transient static spline mesh component."));
			break;
		}
		SplineOwner->AddInstanceComponent(Spline);
		SplineOwner->SetRootComponent(Spline);
		Spline->SetMobility(EComponentMobility::Static);
		Spline->SetStaticMesh(SourceMesh);
		Spline->SetForwardAxis(ESplineMeshAxis::X, false);
		Spline->SetStartAndEnd(FVector(-50.0, 0.0, 0.0), FVector(100.0, 300.0, 0.0),
			FVector(50.0, 0.0, 0.0), FVector(100.0, -300.0, 0.0), false);
		Spline->RegisterComponentWithWorld(World);
		if (!Spline->IsRegistered() || Spline->Mobility != EComponentMobility::Static
			|| Spline->GetStaticMesh() != SourceMesh || Spline->GetNumMaterials() <= 0)
		{
			Fail(TEXT("Transient spline component did not register as a static Cube mesh."));
			break;
		}
		AcousticOrigin = FixtureVolume->BakeBounds->GetComponentLocation();

		const auto CaptureCase = [&](IMAcousticSplineBakeGeometryTestPrivate::FIMG1Snapshot& Snapshot, int32& CollisionVertexCount,
			int32& CollisionTriangleCount, bool& bOracleMatch) -> bool
		{
			FString CaseFailure;
			const bool bCasePassed = IMAcousticSplineBakeGeometryTestPrivate::IMG1CaptureAndCompare(FixtureVolume, Spline, AcousticOrigin,
				MaterialIndices, Snapshot, CollisionVertexCount, CollisionTriangleCount, CaseFailure);
			bOracleMatch = bCasePassed;
			if (!bCasePassed) Fail(CaseFailure);
			return bCasePassed;
		};

		if (!CaptureCase(InitialSnapshot, InitialCollisionVertices, InitialCollisionTriangles, bInitialOracleMatch)) break;
		InitialVertices = InitialSnapshot.Vertices.Num();
		InitialTriangles = InitialSnapshot.Triangles.Num();

		// Check the winding matcher itself with an adversarial reversal: a valid
		// oracle comparison may rotate a triangle, but must reject reversed winding.
		TArray<IMAcousticSplineBakeGeometryTestPrivate::FIMG1Triangle> ReversedTriangles = InitialSnapshot.Triangles;
		for (IMAcousticSplineBakeGeometryTestPrivate::FIMG1Triangle& Triangle : ReversedTriangles) Swap(Triangle.Vertices[1], Triangle.Vertices[2]);
		FString WindingNegativeControlFailure;
		bReversedWindingRejected = !IMAcousticSplineBakeGeometryTestPrivate::IMG1CompareTriangleMultisets(InitialSnapshot.Triangles,
			ReversedTriangles, IMAcousticSplineBakeGeometryTestPrivate::IMG1VertexToleranceMeters, WindingNegativeControlFailure);
		if (!bReversedWindingRejected)
		{
			Fail(TEXT("Triangle multiset comparator accepted a reversed-winding negative control."));
			break;
		}

		const FStaticMeshLODResources& LOD = SourceMesh->GetRenderData()->LODResources[0];
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		RawStaticVertices.Reserve(Positions.GetNumVertices());
		for (uint32 VertexIndex = 0; VertexIndex < Positions.GetNumVertices(); ++VertexIndex)
		{
			const FVector WorldPosition = Spline->GetComponentTransform().TransformPosition(FVector(Positions.VertexPosition(VertexIndex)));
			const IPLVector3 SDKPosition = ToSDKPosition(WorldPosition, AcousticOrigin);
			RawStaticVertices.Emplace(SDKPosition.x, SDKPosition.y, SDKPosition.z);
		}
		const double BendDistanceMeters = IMAcousticSplineBakeGeometryTestPrivate::IMG1MaxNearestVertexDistance(InitialSnapshot.Vertices, RawStaticVertices);
		if (BendDistanceMeters <= IMAcousticSplineBakeGeometryTestPrivate::IMG1MinimumBendMeters)
		{
			Fail(FString::Printf(TEXT("Configured non-collinear spline did not deform the Cube beyond the predeclared %.3f m bend floor; observed=%.6f m."),
				IMAcousticSplineBakeGeometryTestPrivate::IMG1MinimumBendMeters, BendDistanceMeters));
			break;
		}

		// Move the actual spline endpoint control point by 25 cm; preserve tangents.
		Spline->SetStartAndEnd(FVector(-50.0, 0.0, 0.0), FVector(100.0, 300.0, 0.0),
			FVector(50.0, 25.0, 0.0), FVector(100.0, -300.0, 0.0), true);
		if (!CaptureCase(MovedSnapshot, MovedCollisionVertices, MovedCollisionTriangles, bMovedOracleMatch)) break;
		MovedVertices = MovedSnapshot.Vertices.Num();
		MovedTriangles = MovedSnapshot.Triangles.Num();
		bFingerprintChangedOnMove = MovedSnapshot.Fingerprint != InitialSnapshot.Fingerprint;
		if (!bFingerprintChangedOnMove)
		{
			Fail(TEXT("Moving the spline endpoint by 25 cm did not change the scene fingerprint."));
			break;
		}
		FString MoveGeometryFailure;
		if (IMAcousticSplineBakeGeometryTestPrivate::IMG1CompareVertexMultisets(InitialSnapshot.Vertices, MovedSnapshot.Vertices,
			IMAcousticSplineBakeGeometryTestPrivate::IMG1VertexToleranceMeters, MoveGeometryFailure))
		{
			Fail(TEXT("Moving the spline endpoint changed fingerprint but exported geometry did not change."));
			break;
		}

		Spline->SetStartAndEnd(FVector(-50.0, 0.0, 0.0), FVector(100.0, 300.0, 0.0),
			FVector(50.0, 0.0, 0.0), FVector(100.0, -300.0, 0.0), true);
		if (!CaptureCase(RestoredSnapshot, RestoredCollisionVertices, RestoredCollisionTriangles, bRestoredOracleMatch)) break;
		RestoredVertices = RestoredSnapshot.Vertices.Num();
		RestoredTriangles = RestoredSnapshot.Triangles.Num();
		bFingerprintRestored = RestoredSnapshot.Fingerprint == InitialSnapshot.Fingerprint;
		if (!bFingerprintRestored)
		{
			Fail(TEXT("Restoring the original spline endpoint did not restore the original scene fingerprint."));
			break;
		}
		FString RestoreGeometryFailure;
		if (!IMAcousticSplineBakeGeometryTestPrivate::IMG1CompareVertexMultisets(InitialSnapshot.Vertices, RestoredSnapshot.Vertices,
			IMAcousticSplineBakeGeometryTestPrivate::IMG1VertexToleranceMeters, RestoreGeometryFailure)
			|| !IMAcousticSplineBakeGeometryTestPrivate::IMG1CompareTriangleMultisets(InitialSnapshot.Triangles, RestoredSnapshot.Triangles,
				IMAcousticSplineBakeGeometryTestPrivate::IMG1VertexToleranceMeters, RestoreGeometryFailure))
		{
			Fail(TEXT("Restoring the original spline endpoint did not restore its exported geometry: ") + RestoreGeometryFailure);
			break;
		}

		bAssetUnchanged = ExistingVolume->BakedField == OriginalBakeAsset
			&& OriginalBakeAsset->GetOutermost() == OriginalBakeAssetPackage
			&& (!OriginalBakeAssetPackage || OriginalBakeAssetPackage->IsDirty() == bOriginalBakeAssetPackageWasDirty)
			&& (!SourceMeshPackage || SourceMeshPackage->IsDirty() == bSourceMeshPackageWasDirty)
			&& (!SourceMaterialPackage || SourceMaterialPackage->IsDirty() == bSourceMaterialPackageWasDirty);
		if (!bAssetUnchanged)
		{
			Fail(TEXT("Transient spline test changed the original baked field binding or source asset package dirty state."));
			break;
		}
		bSuccess = true;
	} while (false);

	bMapRestored = Restore.Restore();
	if (!bMapRestored) Fail(TEXT("Transient spline actors/tags or the original Editor map did not restore cleanly."));
	UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UPackage* RestoredPackage = RestoredWorld ? RestoredWorld->GetOutermost() : nullptr;
	if (!RestoredPackage || UWorld::RemovePIEPrefix(RestoredPackage->GetName()) != OriginalMapPackagePath
		|| RestoredPackage->IsDirty() != bOriginalMapWasDirty)
	{
		if (RestoredPackage && UWorld::RemovePIEPrefix(RestoredPackage->GetName()) == OriginalMapPackagePath)
			RestoredPackage->SetDirtyFlag(bOriginalMapWasDirty);
		Fail(TEXT("Original Editor map dirty state changed during spline test cleanup."));
	}
	if (Restore.FixturePackage && !Restore.bFixtureDirtyRestored)
		Fail(TEXT("Fixture map package dirty state was not restored."));

	if (IFileManager::Get().DirectoryExists(*ReportDirectory))
	{
		const FString Report = FString::Printf(
			TEXT("run_id=%s\nmap=%s\nsource_mesh=%s\nsource_material=%s\noriginal_bake_asset=%s\n")
			TEXT("oracle=USplineMeshComponent::GetPhysicsTriMeshData(true), transformed by component world transform and IMToSDKPosition\n")
			TEXT("vertex_tolerance_m=%.7f\nminimum_bend_m=%.3f\nendpoint_move_cm=25\n")
			TEXT("initial_capture_vertices=%d\ninitial_collision_vertices=%d\ninitial_capture_triangles=%d\ninitial_collision_triangles=%d\ninitial_fingerprint=%s\ninitial_oracle_match=%d\n")
			TEXT("moved_capture_vertices=%d\nmoved_collision_vertices=%d\nmoved_capture_triangles=%d\nmoved_collision_triangles=%d\nmoved_fingerprint=%s\nmoved_oracle_match=%d\nfingerprint_changed_on_move=%d\n")
			TEXT("restored_capture_vertices=%d\nrestored_collision_vertices=%d\nrestored_capture_triangles=%d\nrestored_collision_triangles=%d\nrestored_fingerprint=%s\nrestored_oracle_match=%d\nfingerprint_restored=%d\n")
			TEXT("reversed_winding_negative_control_rejected=%d\nasset_unchanged=%d\nmap_restored=%d\nno_bake_or_save_called=1\nresult=%s\nfailure=%s\n"),
			*RunId, IMAcousticSplineBakeGeometryTestPrivate::IMG1SplineMap, *GetPathNameSafe(SourceMesh), *SourceMaterialPath, *OriginalBakeAssetPath,
			IMAcousticSplineBakeGeometryTestPrivate::IMG1VertexToleranceMeters, IMAcousticSplineBakeGeometryTestPrivate::IMG1MinimumBendMeters,
			InitialVertices, InitialCollisionVertices, InitialTriangles, InitialCollisionTriangles, *InitialSnapshot.Fingerprint, bInitialOracleMatch ? 1 : 0,
			MovedVertices, MovedCollisionVertices, MovedTriangles, MovedCollisionTriangles, *MovedSnapshot.Fingerprint, bMovedOracleMatch ? 1 : 0, bFingerprintChangedOnMove ? 1 : 0,
			RestoredVertices, RestoredCollisionVertices, RestoredTriangles, RestoredCollisionTriangles, *RestoredSnapshot.Fingerprint, bRestoredOracleMatch ? 1 : 0, bFingerprintRestored ? 1 : 0,
			bReversedWindingRejected ? 1 : 0, bAssetUnchanged ? 1 : 0, bMapRestored ? 1 : 0,
			bSuccess ? TEXT("PASS") : TEXT("FAIL"), *Failure);
		if (!FFileHelper::SaveStringToFile(Report, *FPaths::Combine(ReportDirectory, TEXT("report.txt")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Fail(TEXT("Could not write G1 spline geometry test report."));
		}
	}
	else
	{
		Fail(TEXT("G1 spline test evidence directory is missing."));
	}

	UE_LOG(LogTemp, Display,
		TEXT("IMLogs AcousticG1SplineBake result=%s map=%s mesh=%s vertices=%d triangles=%d moved_fp=%d restored_fp=%d oracle=%d report=%s issue=%s"),
		bSuccess ? TEXT("PASS") : TEXT("FAIL"), IMAcousticSplineBakeGeometryTestPrivate::IMG1SplineMap, *GetPathNameSafe(SourceMesh), InitialVertices,
		InitialTriangles, bFingerprintChangedOnMove ? 1 : 0, bFingerprintRestored ? 1 : 0,
		bInitialOracleMatch && bMovedOracleMatch && bRestoredOracleMatch ? 1 : 0,
		*FPaths::Combine(ReportDirectory, TEXT("report.txt")), *Failure);
	if (bSuccess)
	{
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] SplineBakeGeometry PASS"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS SplineBakeGeometry"));
	}
	return bSuccess;
}

#endif
