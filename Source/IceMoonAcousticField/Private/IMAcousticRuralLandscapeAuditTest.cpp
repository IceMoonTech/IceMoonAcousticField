#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <limits>

namespace IMAcousticRuralLandscapeAuditTestPrivate
{
constexpr TCHAR TargetMapPackage[] = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr TCHAR CineCameraClassPath[] = TEXT("/Script/CinematicCamera.CineCameraActor");
constexpr int32 ExpectedLandscapeCollisionComponents = 16;
constexpr int32 ExpectedCineCameraActors = 7;
constexpr int32 MaxComponentMaterialPaths = 128;
constexpr int32 MaxLandscapeCollisionQuads = 1024;
constexpr int32 MaxPerComponentJsonDetails = 16;
constexpr double CandidateVerticalHalfExtentCm = 500.0;
constexpr double CandidateRadiiCm[] = { 500.0, 2500.0 };

struct FIMBoundsAccumulator
{
	FBox Box = FBox(ForceInit);
	int64 ValidBoundsCount = 0;
	int64 MissingBoundsCount = 0;

	void Add(const FBox& InBox)
	{
		if (InBox.IsValid)
		{
			Box += InBox;
			++ValidBoundsCount;
		}
		else
		{
			++MissingBoundsCount;
		}
	}
};

struct FIMCandidateWindowCounts
{
	double RadiusCm = 0.0;
	int64 StaticHISMInstances = 0;
	int64 StaticOrdinarySMComponents = 0;
	int64 StaticSplineComponents = 0;
	int64 MovableSplineComponents = 0;
	int64 LandscapeNonHoleQuads = 0;
};

struct FIMGridReadback
{
	int32 Quads = 0;
	int32 Vertices = 0;
	bool bHeightfieldRefPresent = false;
	bool bHeightFillSucceeded = false;
	bool bMaterialFillSucceeded = false;
	bool bAllHeightsFinite = false;
	TArray<float> Heights;
	TArray<uint8> MaterialIndices;

	bool IsComplete() const
	{
		return Quads > 0 && bHeightfieldRefPresent && bHeightFillSucceeded
			&& bMaterialFillSucceeded && bAllHeightsFinite;
	}
};

static TSharedRef<FJsonObject> MakeVectorJson(const FVector& Vector)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("x"), Vector.X);
	Json->SetNumberField(TEXT("y"), Vector.Y);
	Json->SetNumberField(TEXT("z"), Vector.Z);
	return Json;
}

static TSharedRef<FJsonObject> MakeRotatorJson(const FRotator& Rotation)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("pitch_degrees"), Rotation.Pitch);
	Json->SetNumberField(TEXT("yaw_degrees"), Rotation.Yaw);
	Json->SetNumberField(TEXT("roll_degrees"), Rotation.Roll);
	return Json;
}

static TSharedRef<FJsonObject> MakeBoundsJson(const FBox& Bounds)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("valid"), Bounds.IsValid != 0);
	if (Bounds.IsValid)
	{
		Json->SetObjectField(TEXT("min"), MakeVectorJson(Bounds.Min));
		Json->SetObjectField(TEXT("max"), MakeVectorJson(Bounds.Max));
		Json->SetObjectField(TEXT("center"), MakeVectorJson(Bounds.GetCenter()));
	}
	return Json;
}

static TSharedRef<FJsonObject> MakeBoundsAccumulatorJson(const FIMBoundsAccumulator& Bounds)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("valid_component_bounds_count"), static_cast<double>(Bounds.ValidBoundsCount));
	Json->SetNumberField(TEXT("missing_component_bounds_count"), static_cast<double>(Bounds.MissingBoundsCount));
	Json->SetObjectField(TEXT("union_world_bounds"), MakeBoundsJson(Bounds.Box));
	return Json;
}

static FIMGridReadback ReadCollisionGrid(ULandscapeHeightfieldCollisionComponent* Collision)
{
	FIMGridReadback Result;
	if (!Collision)
	{
		return Result;
	}

	Result.Quads = Collision->CollisionSizeQuads;
	Result.Vertices = Result.Quads + 1;
	Result.bHeightfieldRefPresent = Collision->HeightfieldRef.GetReference() != nullptr;
	if (Result.Quads <= 0 || Result.Quads > MaxLandscapeCollisionQuads || !Result.bHeightfieldRefPresent)
	{
		return Result;
	}

	const int64 SampleCount64 = static_cast<int64>(Result.Vertices) * Result.Vertices;
	if (SampleCount64 <= 0 || SampleCount64 > TNumericLimits<int32>::Max())
	{
		return Result;
	}

	const int32 SampleCount = static_cast<int32>(SampleCount64);
	Result.Heights.Init(std::numeric_limits<float>::quiet_NaN(), SampleCount);
	Result.MaterialIndices.Init(0, SampleCount);
	Result.bHeightFillSucceeded = Collision->FillHeightTile(MakeArrayView(Result.Heights), 0, Result.Vertices);
	Result.bMaterialFillSucceeded = Collision->FillMaterialIndexTile(MakeArrayView(Result.MaterialIndices), 0, Result.Vertices);
	Result.bAllHeightsFinite = Result.bHeightFillSucceeded;
	if (Result.bAllHeightsFinite)
	{
		for (const float Height : Result.Heights)
		{
			if (!FMath::IsFinite(Height))
			{
				Result.bAllHeightsFinite = false;
				break;
			}
		}
	}
	return Result;
}

static FVector GridCellWorldCenter(const ULandscapeHeightfieldCollisionComponent* Collision,
	const int32 X, const int32 Y)
{
	const FVector LocalCenter(
		(static_cast<double>(X) + 0.5) * Collision->CollisionScale,
		(static_cast<double>(Y) + 0.5) * Collision->CollisionScale,
		0.0);
	return Collision->GetComponentTransform().TransformPosition(LocalCenter);
}

static bool FindOriginGroundSample(
	const TArray<ULandscapeHeightfieldCollisionComponent*>& Collisions,
	double& OutWorldZ, FVector& OutSampleLocation, double& OutHorizontalDistanceCm)
{
	double BestDistanceSquared = TNumericLimits<double>::Max();
	double BestCellHalfDiagonal = 0.0;
	bool bFound = false;

	for (ULandscapeHeightfieldCollisionComponent* Collision : Collisions)
	{
		const FIMGridReadback Grid = ReadCollisionGrid(Collision);
		if (!Grid.IsComplete())
		{
			continue;
		}

		const FTransform ComponentTransform = Collision->GetComponentTransform();
		const FVector XStep = ComponentTransform.TransformVector(FVector(Collision->CollisionScale, 0.0, 0.0));
		const FVector YStep = ComponentTransform.TransformVector(FVector(0.0, Collision->CollisionScale, 0.0));
		const double CellHalfDiagonal = 0.5 * FMath::Sqrt(XStep.SizeSquared() + YStep.SizeSquared());

		for (int32 Y = 0; Y < Grid.Quads; ++Y)
		{
			for (int32 X = 0; X < Grid.Quads; ++X)
			{
				const int32 MaterialSample = Y * Grid.Vertices + X;
				if (Grid.MaterialIndices[MaterialSample] == TNumericLimits<uint8>::Max())
				{
					continue;
				}

				const FVector WorldCenter = GridCellWorldCenter(Collision, X, Y);
				const double DistanceSquared = FMath::Square(WorldCenter.X) + FMath::Square(WorldCenter.Y);
				if (DistanceSquared >= BestDistanceSquared)
				{
					continue;
				}

				const int32 I00 = Y * Grid.Vertices + X;
				const int32 I10 = I00 + 1;
				const int32 I01 = I00 + Grid.Vertices;
				const int32 I11 = I01 + 1;
				const double AverageWorldZ = 0.25 * (Grid.Heights[I00] + Grid.Heights[I10] + Grid.Heights[I01] + Grid.Heights[I11]);
				BestDistanceSquared = DistanceSquared;
				BestCellHalfDiagonal = CellHalfDiagonal;
				OutWorldZ = AverageWorldZ;
				OutSampleLocation = FVector(WorldCenter.X, WorldCenter.Y, AverageWorldZ);
				bFound = true;
			}
		}
	}

	OutHorizontalDistanceCm = bFound ? FMath::Sqrt(BestDistanceSquared) : TNumericLimits<double>::Max();
	return bFound && OutHorizontalDistanceCm <= BestCellHalfDiagonal + 1.0;
}

static bool IsInCandidateWindow(const FVector& Position, const double RadiusCm, const double GroundZ)
{
	if (Position.ContainsNaN())
	{
		return false;
	}
	const double DistanceSquaredXY = FMath::Square(Position.X) + FMath::Square(Position.Y);
	return DistanceSquaredXY <= FMath::Square(RadiusCm)
		&& FMath::Abs(Position.Z - GroundZ) <= CandidateVerticalHalfExtentCm;
}

static TSharedRef<FJsonObject> MakeHistogramJson(const TMap<int32, int64>& Histogram)
{
	TArray<int32> SortedIndices;
	Histogram.GetKeys(SortedIndices);
	SortedIndices.Sort();

	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const int32 Index : SortedIndices)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		Entry->SetNumberField(TEXT("sample_count"), static_cast<double>(Histogram.FindChecked(Index)));
		Entry->SetBoolField(TEXT("is_visibility_hole_sentinel"), Index == TNumericLimits<uint8>::Max());
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("indices"), Entries);
	Json->SetNumberField(TEXT("visibility_hole_sentinel_255_count"),
		static_cast<double>(Histogram.FindRef(TNumericLimits<uint8>::Max())));
	return Json;
}

static bool AddMaterialPaths(const TArray<TObjectPtr<UPhysicalMaterial>>& Materials,
	TArray<TSharedPtr<FJsonValue>>& OutPaths, FString& OutFailure)
{
	const int32 CountToWrite = FMath::Min(Materials.Num(), MaxComponentMaterialPaths);
	for (int32 Index = 0; Index < CountToWrite; ++Index)
	{
		UPhysicalMaterial* Material = Materials[Index].Get();
		if (Material)
		{
			OutPaths.Add(MakeShared<FJsonValueString>(Material->GetPathName()));
		}
		else
		{
			OutPaths.Add(MakeShared<FJsonValueString>(TEXT("NULL_MATERIAL_REFERENCE")));
			OutFailure = TEXT("CookedPhysicalMaterials contains a null entry.");
		}
	}
	return Materials.Num() <= MaxComponentMaterialPaths && OutFailure.IsEmpty();
}

static TArray<TSharedPtr<FJsonValue>> MakeRenderMaterialPaths(ULandscapeComponent* RenderComponent,
	bool& bOutComplete)
{
	TArray<TSharedPtr<FJsonValue>> Paths;
	bOutComplete = RenderComponent != nullptr;
	if (!RenderComponent)
	{
		return Paths;
	}

	const int32 MaterialCount = RenderComponent->GetNumMaterials();
	const int32 CountToWrite = FMath::Min(MaterialCount, MaxComponentMaterialPaths);
	for (int32 Index = 0; Index < CountToWrite; ++Index)
	{
		UMaterialInterface* Material = RenderComponent->GetMaterial(Index);
		if (Material)
		{
			Paths.Add(MakeShared<FJsonValueString>(Material->GetPathName()));
		}
		else
		{
			Paths.Add(MakeShared<FJsonValueString>(TEXT("NULL_MATERIAL_REFERENCE")));
			bOutComplete = false;
		}
	}
	bOutComplete &= MaterialCount <= MaxComponentMaterialPaths;
	return Paths;
}

static TSharedRef<FJsonObject> MakeLandscapeComponentJson(
	ULandscapeHeightfieldCollisionComponent* Collision,
	const FIMGridReadback& Grid,
	const TMap<int32, int64>& AllSampleHistogram,
	const TMap<int32, int64>& QuadHistogram,
	bool& bOutComplete)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("component_path"), Collision ? Collision->GetPathName() : TEXT("NULL_COMPONENT"));
	Json->SetStringField(TEXT("owner_path"), Collision && Collision->GetOwner() ? Collision->GetOwner()->GetPathName() : TEXT("UNKNOWN_OWNER"));
	Json->SetStringField(TEXT("component_class"), Collision ? Collision->GetClass()->GetPathName() : TEXT("UNKNOWN_CLASS"));
	Json->SetNumberField(TEXT("collision_size_quads"), Grid.Quads);
	Json->SetNumberField(TEXT("collision_scale_local_cm"), Collision ? Collision->CollisionScale : 0.0);
	Json->SetBoolField(TEXT("heightfield_ref_present"), Grid.bHeightfieldRefPresent);
	Json->SetBoolField(TEXT("fill_height_tile_succeeded"), Grid.bHeightFillSucceeded);
	Json->SetBoolField(TEXT("fill_material_index_tile_succeeded"), Grid.bMaterialFillSucceeded);
	Json->SetBoolField(TEXT("all_height_samples_finite"), Grid.bAllHeightsFinite);
	Json->SetObjectField(TEXT("collision_component_world_bounds"),
		MakeBoundsJson(Collision ? Collision->Bounds.GetBox() : FBox(ForceInit)));

	ULandscapeComponent* RenderComponent = Collision ? Collision->GetRenderComponent() : nullptr;
	Json->SetStringField(TEXT("render_component_path"), RenderComponent ? RenderComponent->GetPathName() : TEXT("NULL_RENDER_COMPONENT"));
	Json->SetObjectField(TEXT("render_component_world_bounds"),
		MakeBoundsJson(RenderComponent ? RenderComponent->Bounds.GetBox() : FBox(ForceInit)));

	TArray<TSharedPtr<FJsonValue>> CookedPhysicalMaterialPaths;
	FString PhysicalMaterialFailure;
	const bool bPhysicalMaterialPathsComplete = Collision
		? AddMaterialPaths(Collision->CookedPhysicalMaterials, CookedPhysicalMaterialPaths, PhysicalMaterialFailure)
		: false;
	Json->SetNumberField(TEXT("cooked_physical_material_path_count"), Collision ? Collision->CookedPhysicalMaterials.Num() : 0);
	Json->SetArrayField(TEXT("cooked_physical_material_paths"), CookedPhysicalMaterialPaths);
	Json->SetBoolField(TEXT("cooked_physical_material_paths_truncated"), !bPhysicalMaterialPathsComplete);
	Json->SetStringField(TEXT("cooked_physical_material_path_issue"), PhysicalMaterialFailure.IsEmpty() ? TEXT("NONE") : PhysicalMaterialFailure);

	bool bRenderMaterialPathsComplete = false;
	const TArray<TSharedPtr<FJsonValue>> RenderMaterialPaths = MakeRenderMaterialPaths(RenderComponent, bRenderMaterialPathsComplete);
	Json->SetNumberField(TEXT("render_material_slot_count"), RenderComponent ? RenderComponent->GetNumMaterials() : 0);
	Json->SetArrayField(TEXT("render_material_paths"), RenderMaterialPaths);
	Json->SetBoolField(TEXT("render_material_paths_complete"), bRenderMaterialPathsComplete);

	Json->SetObjectField(TEXT("fill_material_index_tile_sample_histogram"), MakeHistogramJson(AllSampleHistogram));
	Json->SetObjectField(TEXT("collision_quad_material_index_histogram"), MakeHistogramJson(QuadHistogram));
	Json->SetStringField(TEXT("material_index_note"),
		TEXT("The full FillMaterialIndexTile lattice histogram includes boundary samples; the quad histogram counts only x,y in [0, CollisionSizeQuads), where Landscape collision stores per-quad indices. Index 255 is the visibility-hole sentinel used by UE Landscape collision generation."));

	const int32 SampleX = FMath::Clamp(Grid.Quads / 2, 0, FMath::Max(0, Grid.Vertices - 1));
	const int32 SampleY = FMath::Clamp(Grid.Quads / 2, 0, FMath::Max(0, Grid.Vertices - 1));
	const int32 SampleIndex = SampleY * Grid.Vertices + SampleX;
	FVector GridSampleLocation = FVector::ZeroVector;
	bool bGridSampleValid = false;
	if (Collision && Grid.IsComplete() && Grid.Heights.IsValidIndex(SampleIndex))
	{
		const FVector LocalPoint(SampleX * Collision->CollisionScale, SampleY * Collision->CollisionScale, 0.0);
		GridSampleLocation = Collision->GetComponentTransform().TransformPosition(LocalPoint);
		GridSampleLocation.Z = Grid.Heights[SampleIndex];
		bGridSampleValid = !GridSampleLocation.ContainsNaN();
	}
	TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
	Sample->SetNumberField(TEXT("grid_x"), SampleX);
	Sample->SetNumberField(TEXT("grid_y"), SampleY);
	Sample->SetBoolField(TEXT("valid"), bGridSampleValid);
	if (bGridSampleValid)
	{
		Sample->SetObjectField(TEXT("world_location"), MakeVectorJson(GridSampleLocation));
	}
	Json->SetObjectField(TEXT("sample_grid_location"), Sample);

	bOutComplete = Grid.IsComplete() && bPhysicalMaterialPathsComplete && bRenderMaterialPathsComplete
		&& RenderComponent && Collision->Bounds.GetBox().IsValid && RenderComponent->Bounds.GetBox().IsValid
		&& bGridSampleValid;
	return Json;
}

static bool BuildLandscapeAuditReport(const FString& RunId, const bool bLoadMapReturned,
	const double LoadMs, TSharedRef<FJsonObject> Report, FString& OutFailureReason)
{
	TArray<FString> FailureReasons;
	const auto AddFailure = [&FailureReasons](const FString& Reason)
	{
		if (!Reason.IsEmpty() && FailureReasons.Num() < 64)
		{
			FailureReasons.AddUnique(Reason);
		}
	};

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FString LoadedWorldPackage = World && World->GetOutermost()
		? World->GetOutermost()->GetName() : TEXT("UNKNOWN_NO_EDITOR_WORLD");
	const bool bTargetWorldLoaded = LoadedWorldPackage.Equals(TargetMapPackage, ESearchCase::CaseSensitive);
	if (!bLoadMapReturned || !bTargetWorldLoaded)
	{
		AddFailure(FString::Printf(TEXT("Target map load/identity mismatch: loaded=%s expected=%s returned=%d."),
			*LoadedWorldPackage, TargetMapPackage, bLoadMapReturned ? 1 : 0));
	}

	TArray<ULandscapeHeightfieldCollisionComponent*> LandscapeCollisions;
	TArray<AActor*> CineCameraActors;
	const UClass* CineCameraClass = FindObject<UClass>(nullptr, CineCameraClassPath);
	if (!World)
	{
		AddFailure(TEXT("Editor world is unavailable after LoadMap."));
	}
	else
	{
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			AActor* Actor = *ActorIt;
			if (CineCameraClass && Actor->IsA(CineCameraClass))
			{
				CineCameraActors.Add(Actor);
			}

			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (ULandscapeHeightfieldCollisionComponent* Collision = Cast<ULandscapeHeightfieldCollisionComponent>(Component))
				{
					LandscapeCollisions.Add(Collision);
				}
			}
		}
	}

	LandscapeCollisions.Sort([](const ULandscapeHeightfieldCollisionComponent& A, const ULandscapeHeightfieldCollisionComponent& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	CineCameraActors.Sort([](const AActor& A, const AActor& B)
	{
		return A.GetPathName() < B.GetPathName();
	});

	if (!CineCameraClass)
	{
		AddFailure(TEXT("CineCameraActor class is not loaded; camera inventory cannot be verified."));
	}
	if (LandscapeCollisions.Num() != ExpectedLandscapeCollisionComponents)
	{
		AddFailure(FString::Printf(TEXT("Expected %d LandscapeHeightfieldCollisionComponents, found %d."),
			ExpectedLandscapeCollisionComponents, LandscapeCollisions.Num()));
	}
	if (CineCameraActors.Num() != ExpectedCineCameraActors)
	{
		AddFailure(FString::Printf(TEXT("Expected %d CineCameraActor instances, found %d."),
			ExpectedCineCameraActors, CineCameraActors.Num()));
	}

	double OriginGroundZ = 0.0;
	double OriginGroundDistanceCm = 0.0;
	FVector OriginGroundSample = FVector::ZeroVector;
	const bool bOriginGroundKnown = FindOriginGroundSample(
		LandscapeCollisions, OriginGroundZ, OriginGroundSample, OriginGroundDistanceCm);
	if (!bOriginGroundKnown)
	{
		AddFailure(TEXT("No valid non-hole Landscape collision sample was found close enough to world origin; candidate-window counts are not evaluated."));
	}

	TArray<FIMCandidateWindowCounts> WindowCounts;
	for (const double RadiusCm : CandidateRadiiCm)
	{
		FIMCandidateWindowCounts Counts;
		Counts.RadiusCm = RadiusCm;
		WindowCounts.Add(Counts);
	}

	int64 LandscapeHeightfieldRefCount = 0;
	int64 LandscapeGridReadbackValidCount = 0;
	int64 LandscapeQuadCount = 0;
	int64 LandscapeNonHoleQuadCount = 0;
	int64 LandscapeVisibilityHoleQuadCount = 0;
	int64 StaticHISMComponentCount = 0;
	int64 StaticHISMInstanceTotal = 0;
	int64 StaticOrdinarySMComponentCount = 0;
	int64 InstancedStaticMeshComponentCount = 0;
	int64 SplineStaticCount = 0;
	int64 SplineMovableCount = 0;
	int64 SplineStationaryCount = 0;
	int64 SplineOtherMobilityCount = 0;
	int64 SplineMissingBoundsCount = 0;
	int64 InstanceTransformReadFailures = 0;
	FIMBoundsAccumulator StaticSplineBounds;
	FIMBoundsAccumulator MovableSplineBounds;
	FIMBoundsAccumulator StationarySplineBounds;
	FIMBoundsAccumulator OtherSplineBounds;

	TArray<TSharedPtr<FJsonValue>> LandscapeDetails;
	TMap<int32, int64> GlobalQuadHistogram;
	bool bLandscapeDetailTruncated = false;
	bool bLandscapeDataComplete = LandscapeCollisions.Num() == ExpectedLandscapeCollisionComponents;

	for (ULandscapeHeightfieldCollisionComponent* Collision : LandscapeCollisions)
	{
		const FIMGridReadback Grid = ReadCollisionGrid(Collision);
		TMap<int32, int64> AllSampleHistogram;
		TMap<int32, int64> QuadHistogram;
		if (Grid.IsComplete())
		{
			++LandscapeGridReadbackValidCount;
			++LandscapeHeightfieldRefCount;
			for (const uint8 MaterialIndex : Grid.MaterialIndices)
			{
				++AllSampleHistogram.FindOrAdd(static_cast<int32>(MaterialIndex));
			}
			for (int32 Y = 0; Y < Grid.Quads; ++Y)
			{
				for (int32 X = 0; X < Grid.Quads; ++X)
				{
					const int32 MaterialSample = Y * Grid.Vertices + X;
					const uint8 MaterialIndex = Grid.MaterialIndices[MaterialSample];
					++QuadHistogram.FindOrAdd(static_cast<int32>(MaterialIndex));
					++GlobalQuadHistogram.FindOrAdd(static_cast<int32>(MaterialIndex));
					++LandscapeQuadCount;
					if (MaterialIndex == TNumericLimits<uint8>::Max())
					{
						++LandscapeVisibilityHoleQuadCount;
					}
					else
					{
						++LandscapeNonHoleQuadCount;
						const int32 I00 = Y * Grid.Vertices + X;
						const int32 I10 = I00 + 1;
						const int32 I01 = I00 + Grid.Vertices;
						const int32 I11 = I01 + 1;
						const double WorldZ = 0.25 * (Grid.Heights[I00] + Grid.Heights[I10] + Grid.Heights[I01] + Grid.Heights[I11]);
						FVector WorldCenter = GridCellWorldCenter(Collision, X, Y);
						WorldCenter.Z = WorldZ;
						if (bOriginGroundKnown)
						{
							for (FIMCandidateWindowCounts& Window : WindowCounts)
							{
								if (IsInCandidateWindow(WorldCenter, Window.RadiusCm, OriginGroundZ))
								{
									++Window.LandscapeNonHoleQuads;
								}
							}
						}
					}
				}
			}
		}
		else
		{
			bLandscapeDataComplete = false;
			AddFailure(FString::Printf(TEXT("Landscape grid readback failed for %s (q=%d ref=%d height=%d material=%d finite=%d)."),
				*Collision->GetPathName(), Grid.Quads, Grid.bHeightfieldRefPresent ? 1 : 0,
				Grid.bHeightFillSucceeded ? 1 : 0, Grid.bMaterialFillSucceeded ? 1 : 0,
				Grid.bAllHeightsFinite ? 1 : 0));
		}

		if (LandscapeDetails.Num() < MaxPerComponentJsonDetails)
		{
			bool bComponentComplete = false;
			TSharedRef<FJsonObject> Detail = MakeLandscapeComponentJson(Collision, Grid,
				AllSampleHistogram, QuadHistogram, bComponentComplete);
			LandscapeDetails.Add(MakeShared<FJsonValueObject>(Detail));
			if (!bComponentComplete)
			{
				bLandscapeDataComplete = false;
				AddFailure(FString::Printf(TEXT("Landscape component metadata is incomplete for %s."), *Collision->GetPathName()));
			}
		}
		else
		{
			bLandscapeDetailTruncated = true;
			bLandscapeDataComplete = false;
			AddFailure(TEXT("Landscape component JSON detail cap exceeded."));
		}
	}

	if (World)
	{
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			TArray<UActorComponent*> Components;
			ActorIt->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (USplineMeshComponent* Spline = Cast<USplineMeshComponent>(Component))
				{
					const FBox SplineBox = Spline->Bounds.GetBox();
					const EComponentMobility::Type Mobility = Spline->Mobility;
					switch (Mobility)
					{
					case EComponentMobility::Static:
						++SplineStaticCount;
						StaticSplineBounds.Add(SplineBox);
						break;
					case EComponentMobility::Movable:
						++SplineMovableCount;
						MovableSplineBounds.Add(SplineBox);
						break;
					case EComponentMobility::Stationary:
						++SplineStationaryCount;
						StationarySplineBounds.Add(SplineBox);
						break;
					default:
						++SplineOtherMobilityCount;
						OtherSplineBounds.Add(SplineBox);
						break;
					}
					if (!SplineBox.IsValid)
					{
						++SplineMissingBoundsCount;
						AddFailure(FString::Printf(TEXT("SplineMeshComponent has invalid world bounds: %s."), *Spline->GetPathName()));
					}
					else if (bOriginGroundKnown)
					{
						for (FIMCandidateWindowCounts& Window : WindowCounts)
						{
							if (IsInCandidateWindow(SplineBox.GetCenter(), Window.RadiusCm, OriginGroundZ))
							{
								if (Mobility == EComponentMobility::Static)
								{
									++Window.StaticSplineComponents;
								}
								else if (Mobility == EComponentMobility::Movable)
								{
									++Window.MovableSplineComponents;
								}
							}
						}
					}
				}

				if (UHierarchicalInstancedStaticMeshComponent* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>(Component))
				{
					if (HISM->Mobility == EComponentMobility::Static)
					{
						++StaticHISMComponentCount;
						StaticHISMInstanceTotal += HISM->GetInstanceCount();
						if (bOriginGroundKnown)
						{
							for (int32 InstanceIndex = 0; InstanceIndex < HISM->GetInstanceCount(); ++InstanceIndex)
							{
								FTransform InstanceTransform;
								if (!HISM->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
								{
									++InstanceTransformReadFailures;
									continue;
								}
								const FVector InstanceLocation = InstanceTransform.GetLocation();
								for (FIMCandidateWindowCounts& Window : WindowCounts)
								{
									if (IsInCandidateWindow(InstanceLocation, Window.RadiusCm, OriginGroundZ))
									{
										++Window.StaticHISMInstances;
									}
								}
							}
						}
					}
				}
				else if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Component))
				{
					++InstancedStaticMeshComponentCount;
					(void)ISM;
				}
				else if (UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Component))
				{
					if (StaticMesh->Mobility == EComponentMobility::Static)
					{
						++StaticOrdinarySMComponentCount;
						if (bOriginGroundKnown)
						{
							const FVector Center = StaticMesh->Bounds.Origin;
							for (FIMCandidateWindowCounts& Window : WindowCounts)
							{
								if (IsInCandidateWindow(Center, Window.RadiusCm, OriginGroundZ))
								{
									++Window.StaticOrdinarySMComponents;
								}
							}
						}
					}
				}
			}
		}
	}

	if (InstanceTransformReadFailures > 0)
	{
		AddFailure(FString::Printf(TEXT("Failed to read %lld static HISM instance transforms."), InstanceTransformReadFailures));
	}
	if (SplineMissingBoundsCount > 0)
	{
		AddFailure(TEXT("Some spline bounds are unavailable; the full-map spline bounds are incomplete."));
	}

	TArray<TSharedPtr<FJsonValue>> CameraDetails;
	const int32 CameraDetailsToWrite = FMath::Min(CineCameraActors.Num(), ExpectedCineCameraActors);
	for (int32 CameraIndex = 0; CameraIndex < CameraDetailsToWrite; ++CameraIndex)
	{
		AActor* Camera = CineCameraActors[CameraIndex];
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("actor_path"), Camera->GetPathName());
		Detail->SetStringField(TEXT("class_path"), Camera->GetClass()->GetPathName());
		Detail->SetObjectField(TEXT("world_location"), MakeVectorJson(Camera->GetActorLocation()));
		Detail->SetObjectField(TEXT("world_rotation"), MakeRotatorJson(Camera->GetActorRotation()));
		CameraDetails.Add(MakeShared<FJsonValueObject>(Detail));
	}

	const bool bInventoryComplete = bLoadMapReturned && bTargetWorldLoaded
		&& LandscapeCollisions.Num() == ExpectedLandscapeCollisionComponents
		&& bLandscapeDataComplete && LandscapeDetails.Num() == LandscapeCollisions.Num()
		&& !bLandscapeDetailTruncated && bOriginGroundKnown
		&& CineCameraClass && CineCameraActors.Num() == ExpectedCineCameraActors
		&& InstanceTransformReadFailures == 0 && SplineMissingBoundsCount == 0;
	const FString TerminalStatus = bInventoryComplete ? TEXT("PASS") : TEXT("FAIL");
	OutFailureReason = FString::Join(FailureReasons, TEXT(" "));

	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetStringField(TEXT("requested_map_package"), TargetMapPackage);
	Report->SetStringField(TEXT("loaded_world_package"), LoadedWorldPackage);
	Report->SetBoolField(TEXT("load_map_returned_success"), bLoadMapReturned);
	Report->SetBoolField(TEXT("target_world_identity_matches"), bTargetWorldLoaded);
	Report->SetNumberField(TEXT("map_load_ms"), LoadMs);
	Report->SetStringField(TEXT("map_package_filename"), FPackageName::LongPackageNameToFilename(
		TargetMapPackage, FPackageName::GetMapPackageExtension()));
	Report->SetStringField(TEXT("map_package_sha256"), TEXT("UNKNOWN_EXTERNAL_VERIFICATION_REQUIRED"));
	Report->SetStringField(TEXT("terminal_status"), TerminalStatus);
	Report->SetStringField(TEXT("terminal_reason"), OutFailureReason.IsEmpty() ? TEXT("All required Landscape, window, spline, and camera inventory checks completed.") : OutFailureReason);
	Report->SetStringField(TEXT("operation_scope"), TEXT("Read-only map load and component/data inventory. No bake, asset mutation, or map save is performed by this test."));

	TSharedRef<FJsonObject> LandscapeSummary = MakeShared<FJsonObject>();
	LandscapeSummary->SetNumberField(TEXT("expected_heightfield_collision_components"), ExpectedLandscapeCollisionComponents);
	LandscapeSummary->SetNumberField(TEXT("heightfield_collision_component_count"), LandscapeCollisions.Num());
	LandscapeSummary->SetNumberField(TEXT("heightfield_ref_present_and_grid_readback_valid_count"), LandscapeHeightfieldRefCount);
	LandscapeSummary->SetNumberField(TEXT("grid_readback_valid_component_count"), LandscapeGridReadbackValidCount);
	LandscapeSummary->SetNumberField(TEXT("collision_quad_count_including_holes"), static_cast<double>(LandscapeQuadCount));
	LandscapeSummary->SetNumberField(TEXT("collision_non_hole_quad_count"), static_cast<double>(LandscapeNonHoleQuadCount));
	LandscapeSummary->SetNumberField(TEXT("collision_visibility_hole_quad_count"), static_cast<double>(LandscapeVisibilityHoleQuadCount));
	LandscapeSummary->SetObjectField(TEXT("collision_quad_material_index_histogram"), MakeHistogramJson(GlobalQuadHistogram));
	LandscapeSummary->SetBoolField(TEXT("per_component_details_truncated"), bLandscapeDetailTruncated);
	LandscapeSummary->SetArrayField(TEXT("components"), LandscapeDetails);
	Report->SetObjectField(TEXT("landscape_collision_inventory"), LandscapeSummary);

	TSharedRef<FJsonObject> GroundSample = MakeShared<FJsonObject>();
	GroundSample->SetBoolField(TEXT("known"), bOriginGroundKnown);
	GroundSample->SetStringField(TEXT("method"), TEXT("Nearest non-hole Landscape collision quad center to world XY origin; Z is the mean of its four FillHeightTile corner heights. Accepted only within that quad's half diagonal plus 1 cm."));
	if (bOriginGroundKnown)
	{
		GroundSample->SetNumberField(TEXT("world_z_cm"), OriginGroundZ);
		GroundSample->SetNumberField(TEXT("horizontal_sample_distance_cm"), OriginGroundDistanceCm);
		GroundSample->SetObjectField(TEXT("sample_world_location"), MakeVectorJson(OriginGroundSample));
	}
	Report->SetObjectField(TEXT("world_origin_landscape_ground_sample"), GroundSample);

	TSharedRef<FJsonObject> CandidateWindowRule = MakeShared<FJsonObject>();
	CandidateWindowRule->SetStringField(TEXT("shape"), TEXT("Vertical cylinder centered at world XY origin; XY radius is 500 cm or 2500 cm."));
	CandidateWindowRule->SetNumberField(TEXT("vertical_half_extent_cm_from_origin_ground"), CandidateVerticalHalfExtentCm);
	CandidateWindowRule->SetStringField(TEXT("point_rule"), TEXT("HISM instance and component counts use instance location or component world-bounds center; Landscape uses collision quad centers and excludes index-255 holes. This is a read-only spatial query, not a created or serialized BakeBounds."));
	CandidateWindowRule->SetStringField(TEXT("ground_sample_status"), bOriginGroundKnown ? TEXT("KNOWN") : TEXT("UNKNOWN_COUNTS_NOT_EVALUATED"));
	TArray<TSharedPtr<FJsonValue>> CandidateWindowsJson;
	for (const FIMCandidateWindowCounts& Window : WindowCounts)
	{
		TSharedRef<FJsonObject> WindowJson = MakeShared<FJsonObject>();
		WindowJson->SetNumberField(TEXT("xy_radius_cm"), Window.RadiusCm);
		if (bOriginGroundKnown)
		{
			WindowJson->SetNumberField(TEXT("z_min_cm"), OriginGroundZ - CandidateVerticalHalfExtentCm);
			WindowJson->SetNumberField(TEXT("z_max_cm"), OriginGroundZ + CandidateVerticalHalfExtentCm);
			WindowJson->SetNumberField(TEXT("static_hism_instance_count"), static_cast<double>(Window.StaticHISMInstances));
			WindowJson->SetNumberField(TEXT("static_ordinary_static_mesh_component_count"), static_cast<double>(Window.StaticOrdinarySMComponents));
			WindowJson->SetNumberField(TEXT("static_spline_mesh_component_count"), static_cast<double>(Window.StaticSplineComponents));
			WindowJson->SetNumberField(TEXT("movable_spline_mesh_component_count"), static_cast<double>(Window.MovableSplineComponents));
			WindowJson->SetNumberField(TEXT("landscape_non_hole_collision_quad_count"), static_cast<double>(Window.LandscapeNonHoleQuads));
			WindowJson->SetStringField(TEXT("count_status"), TEXT("EVALUATED"));
		}
		else
		{
			WindowJson->SetStringField(TEXT("count_status"), TEXT("NOT_EVALUATED_GROUND_UNKNOWN"));
		}
		CandidateWindowsJson.Add(MakeShared<FJsonValueObject>(WindowJson));
	}
	CandidateWindowRule->SetArrayField(TEXT("windows"), CandidateWindowsJson);
	Report->SetObjectField(TEXT("candidate_bake_window_queries"), CandidateWindowRule);

	TSharedRef<FJsonObject> SplineSummary = MakeShared<FJsonObject>();
	SplineSummary->SetNumberField(TEXT("total_spline_mesh_components"),
		SplineStaticCount + SplineMovableCount + SplineStationaryCount + SplineOtherMobilityCount);
	SplineSummary->SetNumberField(TEXT("static_count"), SplineStaticCount);
	SplineSummary->SetNumberField(TEXT("movable_count"), SplineMovableCount);
	SplineSummary->SetNumberField(TEXT("stationary_count"), SplineStationaryCount);
	SplineSummary->SetNumberField(TEXT("other_mobility_count"), SplineOtherMobilityCount);
	SplineSummary->SetNumberField(TEXT("components_with_invalid_bounds"), SplineMissingBoundsCount);
	SplineSummary->SetObjectField(TEXT("static_world_bounds"), MakeBoundsAccumulatorJson(StaticSplineBounds));
	SplineSummary->SetObjectField(TEXT("movable_world_bounds"), MakeBoundsAccumulatorJson(MovableSplineBounds));
	SplineSummary->SetObjectField(TEXT("stationary_world_bounds"), MakeBoundsAccumulatorJson(StationarySplineBounds));
	SplineSummary->SetObjectField(TEXT("other_mobility_world_bounds"), MakeBoundsAccumulatorJson(OtherSplineBounds));
	SplineSummary->SetStringField(TEXT("candidate_window_point_rule"), TEXT("SplineMeshComponent world-bounds center; mobility counted as Static or Movable."));
	Report->SetObjectField(TEXT("spline_mesh_inventory"), SplineSummary);

	TSharedRef<FJsonObject> MeshCounts = MakeShared<FJsonObject>();
	MeshCounts->SetNumberField(TEXT("static_hism_component_count"), static_cast<double>(StaticHISMComponentCount));
	MeshCounts->SetNumberField(TEXT("static_hism_instance_total"), static_cast<double>(StaticHISMInstanceTotal));
	MeshCounts->SetNumberField(TEXT("static_ordinary_static_mesh_component_count"), static_cast<double>(StaticOrdinarySMComponentCount));
	MeshCounts->SetNumberField(TEXT("non_hierarchical_instanced_static_mesh_component_count"), static_cast<double>(InstancedStaticMeshComponentCount));
	MeshCounts->SetNumberField(TEXT("failed_static_hism_instance_transform_reads"), static_cast<double>(InstanceTransformReadFailures));
	Report->SetObjectField(TEXT("static_mesh_inventory"), MeshCounts);

	TSharedRef<FJsonObject> CameraSummary = MakeShared<FJsonObject>();
	CameraSummary->SetStringField(TEXT("class_path"), CineCameraClassPath);
	CameraSummary->SetNumberField(TEXT("expected_count"), ExpectedCineCameraActors);
	CameraSummary->SetNumberField(TEXT("found_count"), CineCameraActors.Num());
	CameraSummary->SetBoolField(TEXT("class_available"), CineCameraClass != nullptr);
	CameraSummary->SetBoolField(TEXT("details_truncated"), CineCameraActors.Num() > CameraDetailsToWrite);
	CameraSummary->SetArrayField(TEXT("actors"), CameraDetails);
	Report->SetObjectField(TEXT("cine_camera_inventory"), CameraSummary);

	TArray<TSharedPtr<FJsonValue>> FailureJson;
	for (const FString& Failure : FailureReasons)
	{
		FailureJson.Add(MakeShared<FJsonValueString>(Failure));
	}
	Report->SetNumberField(TEXT("failure_reason_count"), FailureReasons.Num());
	Report->SetArrayField(TEXT("failure_reasons"), FailureJson);

	return bInventoryComplete;
}

static bool RunLandscapeAudit(FAutomationTestBase* Test)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralLandscapeAudit"), RunId);
	const FString ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));

	const double LoadStartedAt = FPlatformTime::Seconds();
	const bool bLoadMapReturned = FEditorFileUtils::LoadMap(TargetMapPackage, false, true);
	const double LoadMs = (FPlatformTime::Seconds() - LoadStartedAt) * 1000.0;

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	FString FailureReason;
	const bool bInventoryAccepted = BuildLandscapeAuditReport(RunId, bLoadMapReturned, LoadMs, Report, FailureReason);

	const bool bDirectoryReady = IFileManager::Get().MakeDirectory(*ReportDirectory, true);
	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
	Writer->Close();
	const bool bReportWritten = bDirectoryReady && bSerialized
		&& FFileHelper::SaveStringToFile(JsonText, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	if (!bReportWritten)
	{
		FailureReason += TEXT(" Report directory creation, JSON serialization, or report write failed.");
	}

	UE_LOG(LogTemp, Display,
		TEXT("IMLogs AcousticRuralLandscapeAudit run_id=%s status=%s landscape_components=%d load_ms=%.3f report=%s"),
		*RunId, bInventoryAccepted && bReportWritten ? TEXT("PASS") : TEXT("FAIL"),
		ExpectedLandscapeCollisionComponents, LoadMs, *ReportPath);

	if (bInventoryAccepted && bReportWritten)
	{
		Test->AddInfo(FString::Printf(TEXT("Rural Landscape read-only audit report: %s"), *ReportPath));
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralLandscapeAudit PASS"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS RuralLandscapeAudit"));
		return true;
	}

	Test->AddError(FString::Printf(TEXT("Rural Landscape audit failed closed. %s Report: %s"), *FailureReason, *ReportPath));
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralLandscapeAuditTest,
	"IceMoon.AcousticField.Rural.LandscapeCollisionReadOnlyAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralLandscapeAuditTest::RunTest(const FString&)
{
	return IMAcousticRuralLandscapeAuditTestPrivate::RunLandscapeAudit(this);
}

#endif
