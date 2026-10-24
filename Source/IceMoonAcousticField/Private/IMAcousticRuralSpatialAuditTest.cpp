#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticRuralSpatialAuditTestPrivate
{
constexpr TCHAR RuralSpatialMapPackage[] = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr TCHAR CineCameraActorClassPath[] = TEXT("/Script/CinematicCamera.CineCameraActor");
constexpr int32 ExpectedCineCameraCount = 7;
constexpr double RegionHalfWidthsCm[] = {1000.0, 2000.0}; // 20 m and 40 m XY boxes.
constexpr double LandscapeVerticalMarginCm = 5000.0;
constexpr double FallbackVerticalHalfRangeCm = 10000.0;
constexpr int32 MaxReportedAssetPaths = 256;

struct FIMRuralCandidateCenter
{
	FString Name;
	FString ActorPath;
	FVector Location = FVector::ZeroVector;
};

struct FIMRuralRegionStats
{
	FString CenterName;
	FString CenterActorPath;
	FVector Center = FVector::ZeroVector;
	double HalfWidthCm = 0.0;
	FBox Bounds = FBox(ForceInit);
	FString ZSource;
	bool bZFromLandscapeBounds = false;

	TSet<const UPrimitiveComponent*> PrimitiveComponents;
	TSet<const UStaticMeshComponent*> MeshComponents;
	TSet<const UStaticMeshComponent*> StaticSMComponents;
	TSet<const UStaticMeshComponent*> MovableSMComponents;
	TSet<const UStaticMeshComponent*> StationarySMComponents;
	TSet<const UInstancedStaticMeshComponent*> StaticISMComponents;
	TSet<const UInstancedStaticMeshComponent*> MovableISMComponents;
	TSet<const UInstancedStaticMeshComponent*> StationaryISMComponents;
	TSet<const UHierarchicalInstancedStaticMeshComponent*> StaticHISMComponents;
	TSet<const UHierarchicalInstancedStaticMeshComponent*> MovableHISMComponents;
	TSet<const UHierarchicalInstancedStaticMeshComponent*> StationaryHISMComponents;
	TSet<const USplineMeshComponent*> StaticSplineComponents;
	TSet<const USplineMeshComponent*> MovableSplineComponents;
	TSet<const USplineMeshComponent*> StationarySplineComponents;
	TSet<const ULandscapeHeightfieldCollisionComponent*> LandscapeCollisionComponents;
	TSet<const UStaticMeshComponent*> ComponentsWithRecordedMaterials;
	TSet<FString> UniqueMeshPaths;
	TSet<FString> EffectiveMaterialPaths;
	TSet<FString> MeshesWithoutLOD0;

	int64 StaticISMInstances = 0;
	int64 MovableISMInstances = 0;
	int64 StationaryISMInstances = 0;
	int64 StaticHISMInstances = 0;
	int64 MovableHISMInstances = 0;
	int64 StationaryHISMInstances = 0;
	int64 MissingEffectiveMaterialSlots = 0;
	int64 EstimatedLOD0Triangles = 0;
	int64 TriangleEstimateUnknownPlacements = 0;
	int64 InstanceTransformReadFailures = 0;
	int64 MeshComponentsWithoutAsset = 0;
};

static bool IMRuralIntersectsXY(const FBox& Bounds, const FVector& Center, const double HalfWidthCm)
{
	return Bounds.IsValid
		&& Bounds.Min.X <= Center.X + HalfWidthCm && Bounds.Max.X >= Center.X - HalfWidthCm
		&& Bounds.Min.Y <= Center.Y + HalfWidthCm && Bounds.Max.Y >= Center.Y - HalfWidthCm;
}

static bool IMRuralIntersects3D(const FBox& A, const FBox& B)
{
	return A.IsValid && B.IsValid
		&& A.Min.X <= B.Max.X && A.Max.X >= B.Min.X
		&& A.Min.Y <= B.Max.Y && A.Max.Y >= B.Min.Y
		&& A.Min.Z <= B.Max.Z && A.Max.Z >= B.Min.Z;
}

static TArray<TSharedPtr<FJsonValue>> IMRuralVectorArray(const FVector& Value)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Add(MakeShared<FJsonValueNumber>(Value.X));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Y));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Z));
	return Result;
}

static void IMRuralWritePathSet(TSharedRef<FJsonObject> Json, const FString& FieldPrefix,
	const TSet<FString>& Paths)
{
	TArray<FString> SortedPaths;
	for (const FString& Path : Paths) SortedPaths.Add(Path);
	SortedPaths.Sort();
	const int32 WrittenCount = FMath::Min(SortedPaths.Num(), MaxReportedAssetPaths);
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(WrittenCount);
	for (int32 Index = 0; Index < WrittenCount; ++Index)
		Values.Add(MakeShared<FJsonValueString>(SortedPaths[Index]));
	Json->SetNumberField(FieldPrefix + TEXT("_count"), SortedPaths.Num());
	Json->SetArrayField(FieldPrefix, Values);
	Json->SetBoolField(FieldPrefix + TEXT("_truncated"), SortedPaths.Num() > WrittenCount);
}

static TSharedRef<FJsonObject> IMRuralRegionToJson(const FIMRuralRegionStats& Stats)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("candidate_center_name"), Stats.CenterName);
	Json->SetStringField(TEXT("candidate_center_actor_path"), Stats.CenterActorPath);
	Json->SetArrayField(TEXT("center_cm"), IMRuralVectorArray(Stats.Center));
	Json->SetNumberField(TEXT("box_xy_width_m"), (Stats.HalfWidthCm * 2.0) / 100.0);
	Json->SetArrayField(TEXT("world_bounds_min_cm"), IMRuralVectorArray(Stats.Bounds.Min));
	Json->SetArrayField(TEXT("world_bounds_max_cm"), IMRuralVectorArray(Stats.Bounds.Max));
	Json->SetStringField(TEXT("z_bounds_source"), Stats.ZSource);
	Json->SetStringField(TEXT("bounds_filter_rule"), TEXT("Primitive world AABBs are tested against the region; ISM/HISM uses each instance world transform and the mesh local bounds. Landscape Z is the union of bounds for collision components whose XY bounds overlap, expanded by the reported vertical margin."));

	Json->SetNumberField(TEXT("primitive_components_overlapping"), Stats.PrimitiveComponents.Num());
	Json->SetNumberField(TEXT("mesh_components_overlapping"), Stats.MeshComponents.Num());
	Json->SetNumberField(TEXT("static_sm_components"), Stats.StaticSMComponents.Num());
	Json->SetNumberField(TEXT("movable_sm_components"), Stats.MovableSMComponents.Num());
	Json->SetNumberField(TEXT("stationary_sm_components"), Stats.StationarySMComponents.Num());
	Json->SetNumberField(TEXT("static_ism_components"), Stats.StaticISMComponents.Num());
	Json->SetNumberField(TEXT("movable_ism_components"), Stats.MovableISMComponents.Num());
	Json->SetNumberField(TEXT("stationary_ism_components"), Stats.StationaryISMComponents.Num());
	Json->SetNumberField(TEXT("static_hism_components"), Stats.StaticHISMComponents.Num());
	Json->SetNumberField(TEXT("movable_hism_components"), Stats.MovableHISMComponents.Num());
	Json->SetNumberField(TEXT("stationary_hism_components"), Stats.StationaryHISMComponents.Num());
	Json->SetNumberField(TEXT("static_spline_mesh_components"), Stats.StaticSplineComponents.Num());
	Json->SetNumberField(TEXT("movable_spline_mesh_components"), Stats.MovableSplineComponents.Num());
	Json->SetNumberField(TEXT("stationary_spline_mesh_components"), Stats.StationarySplineComponents.Num());
	Json->SetNumberField(TEXT("static_ism_instances"), static_cast<double>(Stats.StaticISMInstances));
	Json->SetNumberField(TEXT("movable_ism_instances"), static_cast<double>(Stats.MovableISMInstances));
	Json->SetNumberField(TEXT("stationary_ism_instances"), static_cast<double>(Stats.StationaryISMInstances));
	Json->SetNumberField(TEXT("static_hism_instances"), static_cast<double>(Stats.StaticHISMInstances));
	Json->SetNumberField(TEXT("movable_hism_instances"), static_cast<double>(Stats.MovableHISMInstances));
	Json->SetNumberField(TEXT("stationary_hism_instances"), static_cast<double>(Stats.StationaryHISMInstances));
	Json->SetNumberField(TEXT("landscape_collision_components"), Stats.LandscapeCollisionComponents.Num());
	Json->SetNumberField(TEXT("mesh_components_without_asset"), static_cast<double>(Stats.MeshComponentsWithoutAsset));

	Json->SetNumberField(TEXT("unique_mesh_asset_count"), Stats.UniqueMeshPaths.Num());
	IMRuralWritePathSet(Json, TEXT("unique_mesh_asset_paths"), Stats.UniqueMeshPaths);
	Json->SetNumberField(TEXT("effective_material_asset_path_count"), Stats.EffectiveMaterialPaths.Num());
	IMRuralWritePathSet(Json, TEXT("effective_material_asset_paths"), Stats.EffectiveMaterialPaths);
	Json->SetNumberField(TEXT("missing_effective_material_slots"), static_cast<double>(Stats.MissingEffectiveMaterialSlots));
	Json->SetNumberField(TEXT("estimated_lod0_triangles"), static_cast<double>(Stats.EstimatedLOD0Triangles));
	Json->SetBoolField(TEXT("lod0_triangle_estimate_complete"), Stats.MeshesWithoutLOD0.Num() == 0
		&& Stats.MeshComponentsWithoutAsset == 0 && Stats.InstanceTransformReadFailures == 0);
	Json->SetNumberField(TEXT("lod0_triangle_unknown_placements"), static_cast<double>(Stats.TriangleEstimateUnknownPlacements));
	Json->SetNumberField(TEXT("unique_meshes_without_lod0_count"), Stats.MeshesWithoutLOD0.Num());
	IMRuralWritePathSet(Json, TEXT("unique_meshes_without_lod0_paths"), Stats.MeshesWithoutLOD0);
	Json->SetNumberField(TEXT("instance_transform_read_failures"), static_cast<double>(Stats.InstanceTransformReadFailures));
	Json->SetStringField(TEXT("triangle_estimate_method"), TEXT("Sum UStaticMesh RenderData LOD0 GetNumTriangles once per intersecting ordinary placement and once per intersecting ISM/HISM instance; unavailable render LOD0 is reported as incomplete, not estimated."));
	return Json;
}

static bool IMRuralRecordMeshPlacement(FIMRuralRegionStats& Stats, UStaticMeshComponent* Component,
	UStaticMesh* Mesh, const int64 LOD0Triangles, const bool bInstanced, const bool bHierarchical,
	USplineMeshComponent* SplineComponent)
{
	if (!Component || !Mesh)
	{
		if (Component)
		{
			Stats.PrimitiveComponents.Add(Component);
			Stats.MeshComponents.Add(Component);
			++Stats.MeshComponentsWithoutAsset;
		}
		return false;
	}

	Stats.PrimitiveComponents.Add(Component);
	Stats.MeshComponents.Add(Component);
	Stats.UniqueMeshPaths.Add(Mesh->GetPathName());
	if (LOD0Triangles < 0)
	{
		Stats.MeshesWithoutLOD0.Add(Mesh->GetPathName());
		++Stats.TriangleEstimateUnknownPlacements;
	}
	else
	{
		Stats.EstimatedLOD0Triangles += LOD0Triangles;
	}

	const bool bStatic = Component->Mobility == EComponentMobility::Static;
	const bool bStationary = Component->Mobility == EComponentMobility::Stationary;
	if (bInstanced)
	{
		UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Component);
		UHierarchicalInstancedStaticMeshComponent* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>(Component);
		if (bHierarchical && HISM)
		{
			if (bStatic) { Stats.StaticHISMComponents.Add(HISM); ++Stats.StaticHISMInstances; }
			else if (bStationary) { Stats.StationaryHISMComponents.Add(HISM); ++Stats.StationaryHISMInstances; }
			else { Stats.MovableHISMComponents.Add(HISM); ++Stats.MovableHISMInstances; }
		}
		else if (ISM)
		{
			if (bStatic) { Stats.StaticISMComponents.Add(ISM); ++Stats.StaticISMInstances; }
			else if (bStationary) { Stats.StationaryISMComponents.Add(ISM); ++Stats.StationaryISMInstances; }
			else { Stats.MovableISMComponents.Add(ISM); ++Stats.MovableISMInstances; }
		}
	}
	else if (SplineComponent)
	{
		if (bStatic) Stats.StaticSplineComponents.Add(SplineComponent);
		else if (bStationary) Stats.StationarySplineComponents.Add(SplineComponent);
		else Stats.MovableSplineComponents.Add(SplineComponent);
	}
	else
	{
		if (bStatic) Stats.StaticSMComponents.Add(Component);
		else if (bStationary) Stats.StationarySMComponents.Add(Component);
		else Stats.MovableSMComponents.Add(Component);
	}

	if (!Stats.ComponentsWithRecordedMaterials.Contains(Component))
	{
		Stats.ComponentsWithRecordedMaterials.Add(Component);
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			if (UMaterialInterface* Material = Component->GetMaterial(Slot))
				Stats.EffectiveMaterialPaths.Add(Material->GetPathName());
			else
				++Stats.MissingEffectiveMaterialSlots;
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralSpatialAuditTest,
	"IceMoon.AcousticField.G0.RuralSpatialAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralSpatialAuditTest::RunTest(const FString&)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralSpatialAudit"), RunId);
	const double LoadStarted = FPlatformTime::Seconds();
	const bool bLoadMapReturned = FEditorFileUtils::LoadMap(IMAcousticRuralSpatialAuditTestPrivate::RuralSpatialMapPackage, false, true);
	const double LoadMs = (FPlatformTime::Seconds() - LoadStarted) * 1000.0;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FString LoadedWorldPackage = World ? World->GetOutermost()->GetName() : FString();
	const bool bTargetWorldLoaded = LoadedWorldPackage == IMAcousticRuralSpatialAuditTestPrivate::RuralSpatialMapPackage;

	TArray<ULandscapeHeightfieldCollisionComponent*> LandscapeCollisions;
	TArray<AActor*> Cameras;
	const UClass* CineCameraClass = FindObject<UClass>(nullptr, IMAcousticRuralSpatialAuditTestPrivate::CineCameraActorClassPath);
	if (World)
	{
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			AActor* Actor = *ActorIt;
			if (CineCameraClass && Actor->IsA(CineCameraClass)) Cameras.Add(Actor);
			TArray<UActorComponent*> ActorComponents;
			Actor->GetComponents(ActorComponents);
			for (UActorComponent* ActorComponent : ActorComponents)
			{
				if (ULandscapeHeightfieldCollisionComponent* Collision = Cast<ULandscapeHeightfieldCollisionComponent>(ActorComponent))
					LandscapeCollisions.Add(Collision);
			}
		}
	}
	Cameras.Sort([](const AActor& A, const AActor& B) { return A.GetPathName() < B.GetPathName(); });

	TArray<IMAcousticRuralSpatialAuditTestPrivate::FIMRuralCandidateCenter> Centers;
	for (AActor* Camera : Cameras)
	{
		IMAcousticRuralSpatialAuditTestPrivate::FIMRuralCandidateCenter& Candidate = Centers.AddDefaulted_GetRef();
		Candidate.Name = TEXT("CineCamera");
		Candidate.ActorPath = Camera->GetPathName();
		Candidate.Location = Camera->GetActorLocation();
	}
	IMAcousticRuralSpatialAuditTestPrivate::FIMRuralCandidateCenter& Origin = Centers.AddDefaulted_GetRef();
	Origin.Name = TEXT("WorldOrigin");
	Origin.ActorPath = TEXT("world_origin");
	Origin.Location = FVector::ZeroVector;

	TArray<IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats> Regions;
	for (const IMAcousticRuralSpatialAuditTestPrivate::FIMRuralCandidateCenter& Center : Centers)
	{
		for (const double HalfWidthCm : IMAcousticRuralSpatialAuditTestPrivate::RegionHalfWidthsCm)
		{
			IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats = Regions.AddDefaulted_GetRef();
			Stats.CenterName = Center.Name;
			Stats.CenterActorPath = Center.ActorPath;
			Stats.Center = Center.Location;
			Stats.HalfWidthCm = HalfWidthCm;

			double MinLandscapeZ = TNumericLimits<double>::Max();
			double MaxLandscapeZ = TNumericLimits<double>::Lowest();
			for (const ULandscapeHeightfieldCollisionComponent* Collision : LandscapeCollisions)
			{
				if (!Collision) continue;
				const FBox CollisionBounds = Collision->Bounds.GetBox();
				if (IMAcousticRuralSpatialAuditTestPrivate::IMRuralIntersectsXY(CollisionBounds, Center.Location, HalfWidthCm))
				{
					MinLandscapeZ = FMath::Min(MinLandscapeZ, CollisionBounds.Min.Z);
					MaxLandscapeZ = FMath::Max(MaxLandscapeZ, CollisionBounds.Max.Z);
				}
			}
			double MinZ = 0.0;
			double MaxZ = 0.0;
			if (MinLandscapeZ <= MaxLandscapeZ)
			{
				MinZ = MinLandscapeZ - IMAcousticRuralSpatialAuditTestPrivate::LandscapeVerticalMarginCm;
				MaxZ = MaxLandscapeZ + IMAcousticRuralSpatialAuditTestPrivate::LandscapeVerticalMarginCm;
				Stats.bZFromLandscapeBounds = true;
				Stats.ZSource = TEXT("Union of overlapping Landscape collision component world bounds, expanded by 5000 cm above and below.");
			}
			else
			{
				MinZ = Center.Location.Z - IMAcousticRuralSpatialAuditTestPrivate::FallbackVerticalHalfRangeCm;
				MaxZ = Center.Location.Z + IMAcousticRuralSpatialAuditTestPrivate::FallbackVerticalHalfRangeCm;
				Stats.ZSource = TEXT("No Landscape collision bounds overlap this XY box; fallback is candidate center Z plus or minus 10000 cm." );
			}
			Stats.Bounds = FBox(
				FVector(Center.Location.X - HalfWidthCm, Center.Location.Y - HalfWidthCm, MinZ),
				FVector(Center.Location.X + HalfWidthCm, Center.Location.Y + HalfWidthCm, MaxZ));

			for (const ULandscapeHeightfieldCollisionComponent* Collision : LandscapeCollisions)
			{
				if (Collision && IMAcousticRuralSpatialAuditTestPrivate::IMRuralIntersects3D(Collision->Bounds.GetBox(), Stats.Bounds))
				{
					Stats.LandscapeCollisionComponents.Add(Collision);
					Stats.PrimitiveComponents.Add(Collision);
				}
			}
		}
	}

	int64 GlobalInstanceTransformFailures = 0;
	TSet<FString> GlobalMeshesWithoutLOD0;
	TMap<const UStaticMesh*, int64> LOD0TriangleCache;
	const auto GetLOD0Triangles = [&LOD0TriangleCache](const UStaticMesh* Mesh) -> int64
	{
		if (const int64* Cached = LOD0TriangleCache.Find(Mesh)) return *Cached;
		int64 Triangles = INDEX_NONE;
		const FStaticMeshRenderData* RenderData = IsValid(Mesh) ? Mesh->GetRenderData() : nullptr;
		if (RenderData && RenderData->LODResources.IsValidIndex(0))
			Triangles = RenderData->LODResources[0].GetNumTriangles();
		LOD0TriangleCache.Add(Mesh, Triangles);
		return Triangles;
	};

	if (World)
	{
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			TArray<UActorComponent*> ActorComponents;
			ActorIt->GetComponents(ActorComponents);
			for (UActorComponent* ActorComponent : ActorComponents)
			{
				UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(ActorComponent);
				if (!Primitive) continue;
				UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Primitive);
				if (!MeshComponent)
				{
					const FBox PrimitiveBounds = Primitive->Bounds.GetBox();
					for (IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
						if (IMAcousticRuralSpatialAuditTestPrivate::IMRuralIntersects3D(PrimitiveBounds, Stats.Bounds)) Stats.PrimitiveComponents.Add(Primitive);
					continue;
				}

				UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
				if (!Mesh)
				{
					for (IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
					{
						if (IMAcousticRuralSpatialAuditTestPrivate::IMRuralIntersects3D(MeshComponent->Bounds.GetBox(), Stats.Bounds))
						{
							Stats.PrimitiveComponents.Add(MeshComponent);
							Stats.MeshComponents.Add(MeshComponent);
							++Stats.MeshComponentsWithoutAsset;
						}
					}
					continue;
				}

				const int64 LOD0Triangles = GetLOD0Triangles(Mesh);
				UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(MeshComponent);
				UHierarchicalInstancedStaticMeshComponent* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>(MeshComponent);
				USplineMeshComponent* SplineComponent = Cast<USplineMeshComponent>(MeshComponent);
				if (ISM)
				{
					const int32 InstanceCount = ISM->GetInstanceCount();
					for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
					{
						FTransform InstanceWorldTransform;
						if (!ISM->GetInstanceTransform(InstanceIndex, InstanceWorldTransform, true))
						{
							bool bCouldOverlapCandidateRegion = false;
							for (IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
							{
								if (IMAcousticRuralSpatialAuditTestPrivate::IMRuralIntersects3D(MeshComponent->Bounds.GetBox(), Stats.Bounds))
								{
									++Stats.InstanceTransformReadFailures;
									bCouldOverlapCandidateRegion = true;
								}
							}
							if (bCouldOverlapCandidateRegion) ++GlobalInstanceTransformFailures;
							continue;
						}
						const FBox InstanceBox = Mesh->GetBounds().TransformBy(InstanceWorldTransform).GetBox();
						for (IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
						{
							if (IMAcousticRuralSpatialAuditTestPrivate::IMRuralIntersects3D(InstanceBox, Stats.Bounds))
								IMAcousticRuralSpatialAuditTestPrivate::IMRuralRecordMeshPlacement(Stats, MeshComponent, Mesh, LOD0Triangles, true, HISM != nullptr, nullptr);
						}
					}
					continue;
				}

				const FBox ComponentBounds = MeshComponent->Bounds.GetBox();
				for (IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
				{
					if (IMAcousticRuralSpatialAuditTestPrivate::IMRuralIntersects3D(ComponentBounds, Stats.Bounds))
						IMAcousticRuralSpatialAuditTestPrivate::IMRuralRecordMeshPlacement(Stats, MeshComponent, Mesh, LOD0Triangles, false, false, SplineComponent);
				}
			}
		}
	}

	TSet<FString> CandidateUniqueMeshPaths;
	for (const IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
	{
		for (const FString& Path : Stats.MeshesWithoutLOD0) GlobalMeshesWithoutLOD0.Add(Path);
		for (const FString& Path : Stats.UniqueMeshPaths) CandidateUniqueMeshPaths.Add(Path);
	}
	int64 CandidateMeshComponentsWithoutAsset = 0;
	int32 RegionsUsingFallbackZ = 0;
	for (const IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
	{
		CandidateMeshComponentsWithoutAsset += Stats.MeshComponentsWithoutAsset;
		if (!Stats.bZFromLandscapeBounds) ++RegionsUsingFallbackZ;
	}

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("im_acoustic_rural_spatial_audit_v1"));
	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetStringField(TEXT("map_path"), IMAcousticRuralSpatialAuditTestPrivate::RuralSpatialMapPackage);
	Report->SetStringField(TEXT("loaded_world_package"), LoadedWorldPackage);
	Report->SetBoolField(TEXT("load_map_returned_success"), bLoadMapReturned);
	Report->SetBoolField(TEXT("target_world_loaded"), bTargetWorldLoaded);
	Report->SetBoolField(TEXT("scene_write_performed"), false);
	Report->SetNumberField(TEXT("map_load_ms"), LoadMs);
	Report->SetNumberField(TEXT("expected_cine_camera_count"), IMAcousticRuralSpatialAuditTestPrivate::ExpectedCineCameraCount);
	Report->SetNumberField(TEXT("found_cine_camera_count"), Cameras.Num());
	Report->SetBoolField(TEXT("camera_inventory_complete"), Cameras.Num() == IMAcousticRuralSpatialAuditTestPrivate::ExpectedCineCameraCount);
	Report->SetNumberField(TEXT("candidate_center_count"), Centers.Num());
	Report->SetNumberField(TEXT("region_count"), Regions.Num());
	Report->SetNumberField(TEXT("reported_asset_path_limit_per_set"), IMAcousticRuralSpatialAuditTestPrivate::MaxReportedAssetPaths);
	Report->SetNumberField(TEXT("landscape_vertical_margin_cm"), IMAcousticRuralSpatialAuditTestPrivate::LandscapeVerticalMarginCm);
	Report->SetNumberField(TEXT("fallback_vertical_half_range_cm"), IMAcousticRuralSpatialAuditTestPrivate::FallbackVerticalHalfRangeCm);
	Report->SetNumberField(TEXT("global_instance_transform_read_failures"), static_cast<double>(GlobalInstanceTransformFailures));
	Report->SetNumberField(TEXT("global_unique_meshes_without_lod0_count"), GlobalMeshesWithoutLOD0.Num());
	Report->SetNumberField(TEXT("candidate_region_unique_mesh_asset_path_count"), CandidateUniqueMeshPaths.Num());
	Report->SetNumberField(TEXT("candidate_mesh_components_without_asset"), static_cast<double>(CandidateMeshComponentsWithoutAsset));
	Report->SetNumberField(TEXT("regions_using_z_fallback"), RegionsUsingFallbackZ);
	Report->SetStringField(TEXT("triangle_estimate_rule"), TEXT("Each ordinary intersecting static mesh or spline mesh component contributes its UStaticMesh RenderData LOD0 triangle count once; each intersecting ISM/HISM instance contributes that count once. This is a render LOD0 estimate, not collision or bake-export triangle proof."));

	TArray<TSharedPtr<FJsonValue>> CameraDetails;
	for (const IMAcousticRuralSpatialAuditTestPrivate::FIMRuralCandidateCenter& Center : Centers)
	{
		TSharedRef<FJsonObject> CameraJson = MakeShared<FJsonObject>();
		CameraJson->SetStringField(TEXT("name"), Center.Name);
		CameraJson->SetStringField(TEXT("actor_path"), Center.ActorPath);
		CameraJson->SetArrayField(TEXT("location_cm"), IMAcousticRuralSpatialAuditTestPrivate::IMRuralVectorArray(Center.Location));
		CameraDetails.Add(MakeShared<FJsonValueObject>(CameraJson));
	}
	Report->SetArrayField(TEXT("candidate_centers"), CameraDetails);

	TArray<TSharedPtr<FJsonValue>> RegionDetails;
	int64 TotalTransformFailures = 0;
	int64 TotalUnknownTrianglePlacements = 0;
	for (const IMAcousticRuralSpatialAuditTestPrivate::FIMRuralRegionStats& Stats : Regions)
	{
		RegionDetails.Add(MakeShared<FJsonValueObject>(IMAcousticRuralSpatialAuditTestPrivate::IMRuralRegionToJson(Stats)));
		TotalTransformFailures += Stats.InstanceTransformReadFailures;
		TotalUnknownTrianglePlacements += Stats.TriangleEstimateUnknownPlacements;
	}
	Report->SetArrayField(TEXT("regions"), RegionDetails);
	Report->SetNumberField(TEXT("region_instance_transform_failure_sum"), static_cast<double>(TotalTransformFailures));
	Report->SetNumberField(TEXT("region_unknown_triangle_placement_sum"), static_cast<double>(TotalUnknownTrianglePlacements));

	const bool bComplete = bLoadMapReturned && bTargetWorldLoaded
		&& CineCameraClass && Cameras.Num() == IMAcousticRuralSpatialAuditTestPrivate::ExpectedCineCameraCount
		&& GlobalInstanceTransformFailures == 0 && GlobalMeshesWithoutLOD0.Num() == 0
		&& CandidateMeshComponentsWithoutAsset == 0 && RegionsUsingFallbackZ == 0;
	Report->SetStringField(TEXT("audit_result"), bComplete ? TEXT("PASS") : TEXT("INCOMPLETE"));

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
	Writer->Close();
	const FString ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
	const bool bReportSaved = bSerialized && IFileManager::Get().MakeDirectory(*ReportDirectory, true)
		&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!bReportSaved)
	{
		AddError(TEXT("Rural spatial audit report could not be written."));
		UE_LOG(LogTemp, Error, TEXT("IMLogs RuralSpatialAudit FAIL report_write_failed path=%s"), *ReportPath);
		return false;
	}

	AddInfo(FString::Printf(TEXT("Rural spatial audit report: %s"), *ReportPath));
	if (!bComplete)
	{
		AddError(FString::Printf(TEXT("Rural spatial audit is incomplete: map_loaded=%d cameras=%d/%d instance_transform_failures=%lld unique_meshes_without_lod0=%d."),
			bTargetWorldLoaded ? 1 : 0, Cameras.Num(), IMAcousticRuralSpatialAuditTestPrivate::ExpectedCineCameraCount,
			static_cast<long long>(GlobalInstanceTransformFailures), GlobalMeshesWithoutLOD0.Num()));
	}
	UE_LOG(LogTemp, Display, TEXT("IMLogs RuralSpatialAudit result=%s cameras=%d/%d centers=%d regions=%d transform_errors=%lld meshes_without_lod0=%d report=%s"),
		bComplete ? TEXT("PASS") : TEXT("INCOMPLETE"), Cameras.Num(), IMAcousticRuralSpatialAuditTestPrivate::ExpectedCineCameraCount, Centers.Num(), Regions.Num(),
		static_cast<long long>(GlobalInstanceTransformFailures), GlobalMeshesWithoutLOD0.Num(), *ReportPath);
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralSpatialAudit %s"), bComplete ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s RuralSpatialAudit"), bComplete ? TEXT("PASS") : TEXT("FAIL"));
	return bComplete;
}

#endif
