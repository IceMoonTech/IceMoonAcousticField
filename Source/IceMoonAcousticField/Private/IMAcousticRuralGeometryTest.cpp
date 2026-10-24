#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PhysicsVolume.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "IMAcousticBakeVolume.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <limits>

namespace IMAcousticRuralGeometryTestPrivate
{
constexpr TCHAR TargetMapPackage[] = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr int32 ExpectedLandscapeBlocks = 1;
constexpr int32 ExpectedLandscapeQuads = 255;
constexpr double RegionHalfExtentCm = 500.0;
const FVector RegionCenterCm(-6597.06298828125, 14720.8662109375, 395.2615966796875); // Rural Camera3.
constexpr double ProbeSpacingCm = 100.0;
constexpr double ProbeHeightCm = 150.0;
constexpr int32 MaxLandscapeQuadsForOracle = 1024;
constexpr int32 MaxFailureDetails = 64;
constexpr double PositionKeyUnitsPerMeter = 10000.0; // 0.1 mm quantization of the accessor's SDK-space vertices.

struct FIMPositionKey
{
	int32 X = 0;
	int32 Y = 0;
	int32 Z = 0;

	bool operator==(const FIMPositionKey& Other) const
	{
		return X == Other.X && Y == Other.Y && Z == Other.Z;
	}
};

struct FIMTriangleKey
{
	FIMPositionKey A;
	FIMPositionKey B;
	FIMPositionKey C;
};

struct FIMExpectedVertex
{
	FIMPositionKey Key;
	int32 BlockIndex = INDEX_NONE;
};

struct FIMExpectedTriangle
{
	FIMTriangleKey Key;
	int32 BlockIndex = INDEX_NONE;
};

struct FIMObservedTriangle
{
	FIMTriangleKey Key;
	bool bNormalPointsUp = false;
	bool bNonDegenerate = false;
};

struct FIMLandscapeBlockOracle
{
	ULandscapeHeightfieldCollisionComponent* Collision = nullptr;
	ULandscapeComponent* Render = nullptr;
	int32 Quads = 0;
	int32 Side = 0;
	int64 VisibilityHoleQuads = 0;
	int64 ExpectedTriangles = 0;
	int64 MatchedTriangles = 0;
	int64 MatchedVertices = 0;
	int64 CapturedUpwardTriangles = 0;
	int32 RepresentativeFaceX = INDEX_NONE;
	int32 RepresentativeFaceY = INDEX_NONE;
	int32 RepresentativeHoleX = INDEX_NONE;
	int32 RepresentativeHoleY = INDEX_NONE;
	TArray<float> Heights;
	TArray<uint8> SurfaceIndices;
	TArray<FVector> SdkVertices;
};

static FIMPositionKey MakePositionKey(const FVector& Position)
{
	return {
		static_cast<int32>(FMath::RoundToInt64(Position.X * PositionKeyUnitsPerMeter)),
		static_cast<int32>(FMath::RoundToInt64(Position.Y * PositionKeyUnitsPerMeter)),
		static_cast<int32>(FMath::RoundToInt64(Position.Z * PositionKeyUnitsPerMeter))
	};
}

static FVector ToOracleSdkPosition(const FVector& WorldPosition, const FVector& Origin)
{
	const FVector DeltaMeters = (WorldPosition - Origin) * 0.01;
	// Keep this small coordinate oracle local: SDK conversion is (UE Y, UE Z, -UE X), in meters.
	return FVector(static_cast<float>(DeltaMeters.Y), static_cast<float>(DeltaMeters.Z), static_cast<float>(-DeltaMeters.X));
}

static FIMTriangleKey MakeTriangleKey(const FVector& A, const FVector& B, const FVector& C)
{
	return { MakePositionKey(A), MakePositionKey(B), MakePositionKey(C) };
}

static bool PositionKeyLess(const FIMPositionKey& A, const FIMPositionKey& B)
{
	if (A.X != B.X) return A.X < B.X;
	if (A.Y != B.Y) return A.Y < B.Y;
	return A.Z < B.Z;
}

static bool TriangleKeyLess(const FIMTriangleKey& A, const FIMTriangleKey& B)
{
	if (!(A.A == B.A)) return PositionKeyLess(A.A, B.A);
	if (!(A.B == B.B)) return PositionKeyLess(A.B, B.B);
	return PositionKeyLess(A.C, B.C);
}

static bool TriangleKeyEqual(const FIMTriangleKey& A, const FIMTriangleKey& B)
{
	return A.A == B.A && A.B == B.B && A.C == B.C;
}

static bool ExpectedVertexLess(const FIMExpectedVertex& A, const FIMExpectedVertex& B)
{
	if (!(A.Key == B.Key)) return PositionKeyLess(A.Key, B.Key);
	return A.BlockIndex < B.BlockIndex;
}

static bool ExpectedTriangleLess(const FIMExpectedTriangle& A, const FIMExpectedTriangle& B)
{
	if (!TriangleKeyEqual(A.Key, B.Key)) return TriangleKeyLess(A.Key, B.Key);
	return A.BlockIndex < B.BlockIndex;
}

static bool ObservedTriangleLess(const FIMObservedTriangle& A, const FIMObservedTriangle& B)
{
	return TriangleKeyLess(A.Key, B.Key);
}

static TSharedRef<FJsonObject> MakeVectorJson(const FVector& Value)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("x"), Value.X);
	Json->SetNumberField(TEXT("y"), Value.Y);
	Json->SetNumberField(TEXT("z"), Value.Z);
	return Json;
}

static void AddFailure(TArray<FString>& Failures, const FString& Message)
{
	if (Failures.Num() < MaxFailureDetails)
	{
		Failures.AddUnique(Message);
	}
}

static bool IsValidFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}

static bool IsExpectedTerrainQuad(const uint8 SurfaceIndex)
{
	return SurfaceIndex == 0 || SurfaceIndex == TNumericLimits<uint8>::Max();
}

static bool IsDefaultHelperActor(const UWorld* World, const AActor* Actor)
{
	return World && Actor && (Actor == World->GetDefaultBrush() || Actor == World->GetDefaultPhysicsVolume());
}

static bool AddMeshMaterial(UMaterialInterface* Material, TSet<UMaterialInterface*>& UniqueMaterials,
	TArray<FString>& Failures, const FString& Context)
{
	if (!Material)
	{
		AddFailure(Failures, FString::Printf(TEXT("Missing material mapping source: %s."), *Context));
		return false;
	}
	UniqueMaterials.Add(Material);
	return true;
}

static bool IsInstanceInsideRegion(const UStaticMesh* Mesh, const FTransform& Transform, const FBox& Region)
{
	return Mesh && Region.Intersect(Mesh->GetBounds().TransformBy(Transform).GetBox());
}

static bool AddMeshComponentMaterialsAndCounts(UStaticMeshComponent* MeshComponent, const FBox& Region,
	TSet<UMaterialInterface*>& UniqueMaterials, int64& OutComponentCount, int64& OutInstanceCount,
	int64& OutExpectedTriangles, TArray<FString>& Failures)
{
	if (!MeshComponent || !Region.Intersect(MeshComponent->Bounds.GetBox()))
	{
		return true;
	}

	UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
	if (!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		AddFailure(Failures, FString::Printf(TEXT("Static mesh component has no readable LOD0: %s."),
			*MeshComponent->GetPathName()));
		return false;
	}
	const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD0.VertexBuffers.PositionVertexBuffer;
	if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData() || LOD0.IndexBuffer.GetArrayView().Num() == 0)
	{
		AddFailure(Failures, FString::Printf(TEXT("Static mesh LOD0 CPU geometry unavailable: %s."), *Mesh->GetPathName()));
		return false;
	}

	UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(MeshComponent);
	int64 IncludedInstanceCount = 0;
	if (Instances)
	{
		for (int32 InstanceIndex = 0; InstanceIndex < Instances->GetInstanceCount(); ++InstanceIndex)
		{
			FTransform InstanceTransform;
			if (!Instances->GetInstanceTransform(InstanceIndex, InstanceTransform, true)
				|| InstanceTransform.ContainsNaN() || FMath::IsNearlyZero(InstanceTransform.GetDeterminant()))
			{
				AddFailure(Failures, FString::Printf(TEXT("Static instance transform unavailable or invalid: %s[%d]."),
					*MeshComponent->GetPathName(), InstanceIndex));
				return false;
			}
			if (IsInstanceInsideRegion(Mesh, InstanceTransform, Region))
			{
				++IncludedInstanceCount;
			}
		}
	}
	else
	{
		const FTransform Transform = MeshComponent->GetComponentTransform();
		if (Transform.ContainsNaN() || FMath::IsNearlyZero(Transform.GetDeterminant()))
		{
			AddFailure(Failures, FString::Printf(TEXT("Static mesh component transform is invalid: %s."), *MeshComponent->GetPathName()));
			return false;
		}
		IncludedInstanceCount = 1;
	}

	if (IncludedInstanceCount == 0)
	{
		return true;
	}

	++OutComponentCount;
	OutInstanceCount += IncludedInstanceCount;
	int64 TrianglesPerInstance = 0;
	for (const FStaticMeshSection& Section : LOD0.Sections)
	{
		TrianglesPerInstance += Section.NumTriangles;
		AddMeshMaterial(MeshComponent->GetMaterial(Section.MaterialIndex), UniqueMaterials, Failures,
			FString::Printf(TEXT("%s LOD0 section %d"), *MeshComponent->GetPathName(), Section.MaterialIndex));
	}
	OutExpectedTriangles += TrianglesPerInstance * IncludedInstanceCount;
	return true;
}

static bool CompareExpectedVertices(TArray<FIMExpectedVertex>& Expected, TArray<FIMPositionKey>& Captured,
	TArray<int64>& MatchedByBlock, int64& OutMissing)
{
	Expected.Sort(ExpectedVertexLess);
	Captured.Sort(PositionKeyLess);
	int32 ExpectedIndex = 0;
	int32 CapturedIndex = 0;
	OutMissing = 0;
	while (ExpectedIndex < Expected.Num())
	{
		const FIMPositionKey ExpectedKey = Expected[ExpectedIndex].Key;
		while (CapturedIndex < Captured.Num() && PositionKeyLess(Captured[CapturedIndex], ExpectedKey))
		{
			++CapturedIndex;
		}
		if (CapturedIndex < Captured.Num() && Captured[CapturedIndex] == ExpectedKey)
		{
			++MatchedByBlock[Expected[ExpectedIndex].BlockIndex];
			++ExpectedIndex;
			++CapturedIndex;
		}
		else
		{
			++OutMissing;
			++ExpectedIndex;
		}
	}
	return OutMissing == 0;
}

static bool CompareExpectedTriangles(TArray<FIMExpectedTriangle>& Expected, TArray<FIMObservedTriangle>& Captured,
	TArray<int64>& MatchedByBlock, int64& OutMissing, int64& OutBadNormals)
{
	Expected.Sort(ExpectedTriangleLess);
	Captured.Sort(ObservedTriangleLess);
	int32 ExpectedIndex = 0;
	int32 CapturedIndex = 0;
	OutMissing = 0;
	OutBadNormals = 0;
	while (ExpectedIndex < Expected.Num())
	{
		const FIMTriangleKey& ExpectedKey = Expected[ExpectedIndex].Key;
		while (CapturedIndex < Captured.Num() && TriangleKeyLess(Captured[CapturedIndex].Key, ExpectedKey))
		{
			++CapturedIndex;
		}
		if (CapturedIndex < Captured.Num() && TriangleKeyEqual(Captured[CapturedIndex].Key, ExpectedKey))
		{
			++MatchedByBlock[Expected[ExpectedIndex].BlockIndex];
			if (!Captured[CapturedIndex].bNormalPointsUp || !Captured[CapturedIndex].bNonDegenerate)
			{
				++OutBadNormals;
			}
			++ExpectedIndex;
			++CapturedIndex;
		}
		else
		{
			++OutMissing;
			++ExpectedIndex;
		}
	}
	return OutMissing == 0 && OutBadNormals == 0;
}

static int64 CountCapturedHoleTriangles(TArray<FIMTriangleKey>& ExpectedHoles, TArray<FIMObservedTriangle>& Captured)
{
	ExpectedHoles.Sort(TriangleKeyLess);
	Captured.Sort(ObservedTriangleLess);
	int32 HoleIndex = 0;
	int32 CapturedIndex = 0;
	int64 Found = 0;
	while (HoleIndex < ExpectedHoles.Num() && CapturedIndex < Captured.Num())
	{
		const FIMTriangleKey& HoleKey = ExpectedHoles[HoleIndex];
		const FIMTriangleKey& CapturedKey = Captured[CapturedIndex].Key;
		if (TriangleKeyEqual(HoleKey, CapturedKey))
		{
			++Found;
			++HoleIndex;
			++CapturedIndex;
		}
		else if (TriangleKeyLess(CapturedKey, HoleKey))
		{
			++CapturedIndex;
		}
		else
		{
			++HoleIndex;
		}
	}
	return Found;
}

static TSharedRef<FJsonObject> MakeMemoryJson(const FPlatformMemoryStats& Before, const FPlatformMemoryStats& After)
{
	constexpr double BytesPerMiB = 1024.0 * 1024.0;
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("used_physical_mib_before"), Before.UsedPhysical / BytesPerMiB);
	Json->SetNumberField(TEXT("used_physical_mib_after"), After.UsedPhysical / BytesPerMiB);
	Json->SetNumberField(TEXT("peak_used_physical_mib_before"), Before.PeakUsedPhysical / BytesPerMiB);
	Json->SetNumberField(TEXT("peak_used_physical_mib_after"), After.PeakUsedPhysical / BytesPerMiB);
	Json->SetNumberField(TEXT("used_virtual_mib_before"), Before.UsedVirtual / BytesPerMiB);
	Json->SetNumberField(TEXT("used_virtual_mib_after"), After.UsedVirtual / BytesPerMiB);
	Json->SetNumberField(TEXT("peak_used_virtual_mib_before"), Before.PeakUsedVirtual / BytesPerMiB);
	Json->SetNumberField(TEXT("peak_used_virtual_mib_after"), After.PeakUsedVirtual / BytesPerMiB);
	Json->SetStringField(TEXT("peak_note"), TEXT("Process-lifetime peak; may include Editor work before this capture."));
	return Json;
}

static bool RunOracle(FAutomationTestBase* Test, const FString& RunId, const bool bMapLoadReturned,
	const double MapLoadMs, TSharedRef<FJsonObject> Report, FString& OutFailure)
{
	TArray<FString> Failures;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FString LoadedMap = World && World->GetOutermost() ? World->GetOutermost()->GetName() : TEXT("UNKNOWN_NO_EDITOR_WORLD");
	const bool bMapIdentityMatches = LoadedMap.Equals(TargetMapPackage, ESearchCase::CaseSensitive);
	if (!bMapLoadReturned || !bMapIdentityMatches)
	{
		AddFailure(Failures, FString::Printf(TEXT("Rural map load/identity mismatch: loaded=%s expected=%s load_returned=%d."),
			*LoadedMap, TargetMapPackage, bMapLoadReturned ? 1 : 0));
	}
	if (!World || !bMapIdentityMatches)
	{
		Report->SetStringField(TEXT("run_id"), RunId);
		Report->SetStringField(TEXT("requested_map_package"), TargetMapPackage);
		Report->SetStringField(TEXT("loaded_map_package"), LoadedMap);
		Report->SetBoolField(TEXT("load_map_returned_success"), bMapLoadReturned);
		Report->SetBoolField(TEXT("target_map_identity_matches"), bMapIdentityMatches);
		Report->SetStringField(TEXT("terminal_status"), TEXT("FAIL"));
		OutFailure = FString::Join(Failures, TEXT(" "));
		Report->SetStringField(TEXT("terminal_reason"), OutFailure);
		return false;
	}

	TArray<UPrimitiveComponent*> RegionPrimitives;
	TArray<ULandscapeHeightfieldCollisionComponent*> LandscapeCollisions;
	TSet<ULandscapeHeightfieldCollisionComponent*> UniqueLandscapeCollisions;
	TSet<ULandscapeComponent*> UniqueLandscapeRenderComponents;
	TSet<UMaterialInterface*> UniqueMaterials;
	TArray<FString> UnsupportedStaticNonMeshPaths;
	TArray<FString> ProvisionalNonAcousticHelperPaths;
	TArray<TSharedPtr<FJsonValue>> IncludedMeshComponentsJson;
	int64 StaticMeshComponentCount = 0;
	int64 StaticISMComponentCount = 0;
	int64 StaticHISMComponentCount = 0;
	int64 StaticMeshInstanceCount = 0;
	int64 ExpectedStaticMeshTriangles = 0;
	int64 IgnoredComponentCount = 0;
	bool bEnumerationComplete = true;

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AIMAcousticBakeVolume* Volume = World->SpawnActor<AIMAcousticBakeVolume>(
		RegionCenterCm, FRotator::ZeroRotator, SpawnParameters);
	if (!Volume || !Volume->BakeBounds)
	{
		AddFailure(Failures, TEXT("Could not spawn a transient AIMAcousticBakeVolume."));
		Report->SetStringField(TEXT("run_id"), RunId);
		Report->SetStringField(TEXT("terminal_status"), TEXT("FAIL"));
		Report->SetStringField(TEXT("terminal_reason"), TEXT("Transient bake volume spawn failed."));
		OutFailure = TEXT("Transient bake volume spawn failed.");
		if (Volume) Volume->Destroy();
		return false;
	}

	Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
	Volume->SetActorLocation(RegionCenterCm);
	Volume->BakeBounds->SetRelativeLocation(FVector::ZeroVector);
	Volume->BakeBounds->SetRelativeRotation(FRotator::ZeroRotator);
	Volume->BakeBounds->SetRelativeScale3D(FVector::OneVector);
	Volume->BakeBounds->SetBoxExtent(FVector(RegionHalfExtentCm, RegionHalfExtentCm, RegionHalfExtentCm), false);
	Volume->BakeBounds->SetWorldLocation(RegionCenterCm);
	Volume->BakeBounds->UpdateComponentToWorld();
	Volume->ProbeSpacingCm = ProbeSpacingCm;
	Volume->ProbeHeightCm = ProbeHeightCm;
	Volume->SetActorTickEnabled(false);
	Volume->BakeBounds->UpdateBounds();
	const FBox Region = Volume->BakeBounds->Bounds.GetBox();
	if (!Region.GetCenter().Equals(RegionCenterCm, 0.01))
	{
		AddFailure(Failures, FString::Printf(TEXT("BakeBounds world center differs from Rural Camera3: actual=%s expected=%s."),
			*Region.GetCenter().ToString(), *RegionCenterCm.ToString()));
	}

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!Actor || Actor == Volume || IsDefaultHelperActor(World, Actor))
		{
			continue;
		}
		TArray<UPrimitiveComponent*> ActorPrimitives;
		Actor->GetComponents(ActorPrimitives);
		for (UPrimitiveComponent* Primitive : ActorPrimitives)
		{
			if (!IsValid(Primitive) || !Region.Intersect(Primitive->Bounds.GetBox()))
			{
				continue;
			}
#if WITH_EDITORONLY_DATA
			if (Primitive->IsVisualizationComponent() || Primitive->IsEditorOnly())
			{
				++IgnoredComponentCount;
				continue;
			}
#endif
			if (Primitive->ComponentHasTag(TEXT("IMAcousticIgnore")))
			{
				++IgnoredComponentCount;
				continue;
			}
			if (Primitive->Mobility != EComponentMobility::Static)
			{
				++IgnoredComponentCount;
				continue;
			}
			RegionPrimitives.Add(Primitive);

			if (ULandscapeComponent* RenderLandscape = Cast<ULandscapeComponent>(Primitive))
			{
				ULandscapeHeightfieldCollisionComponent* Collision = RenderLandscape->GetCollisionComponent();
				if (!Collision || !Region.Intersect(Collision->Bounds.GetBox()))
				{
					AddFailure(Failures, FString::Printf(TEXT("Intersecting Landscape render component has no paired intersecting collision: %s."),
						*RenderLandscape->GetPathName()));
					bEnumerationComplete = false;
					continue;
				}
				UniqueLandscapeRenderComponents.Add(RenderLandscape);
				UniqueLandscapeCollisions.Add(Collision);
			}
			else if (ULandscapeHeightfieldCollisionComponent* Collision = Cast<ULandscapeHeightfieldCollisionComponent>(Primitive))
			{
				UniqueLandscapeCollisions.Add(Collision);
				if (ULandscapeComponent* PairedRenderLandscape = Collision->GetRenderComponent())
				{
					UniqueLandscapeRenderComponents.Add(PairedRenderLandscape);
				}
			}
			else if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Primitive))
			{
				const int64 PreviousInstanceCount = StaticMeshInstanceCount;
				const int64 PreviousTriangleCount = ExpectedStaticMeshTriangles;
				const bool bMeshRead = AddMeshComponentMaterialsAndCounts(MeshComponent, Region, UniqueMaterials,
					StaticMeshComponentCount, StaticMeshInstanceCount, ExpectedStaticMeshTriangles, Failures);
				if (bMeshRead && StaticMeshInstanceCount > PreviousInstanceCount)
				{
					TSharedRef<FJsonObject> MeshItem = MakeShared<FJsonObject>();
					MeshItem->SetStringField(TEXT("component_path"), MeshComponent->GetPathName());
					MeshItem->SetStringField(TEXT("mesh_asset_path"), GetPathNameSafe(MeshComponent->GetStaticMesh()));
					MeshItem->SetNumberField(TEXT("included_instances"), StaticMeshInstanceCount - PreviousInstanceCount);
					MeshItem->SetNumberField(TEXT("exported_lod0_triangles"), ExpectedStaticMeshTriangles - PreviousTriangleCount);
					MeshItem->SetNumberField(TEXT("collision_enabled"), static_cast<int32>(MeshComponent->GetCollisionEnabled()));
					MeshItem->SetBoolField(TEXT("visible"), MeshComponent->IsVisible());
					MeshItem->SetBoolField(TEXT("hidden_in_game"), MeshComponent->bHiddenInGame);
					IncludedMeshComponentsJson.Add(MakeShared<FJsonValueObject>(MeshItem));
					if (Cast<UHierarchicalInstancedStaticMeshComponent>(MeshComponent)) ++StaticHISMComponentCount;
					else if (Cast<UInstancedStaticMeshComponent>(MeshComponent)) ++StaticISMComponentCount;
				}
				if (!bMeshRead)
				{
					bEnumerationComplete = false;
				}
			}
			else
			{
				const FString ComponentClass = Primitive->GetClass()->GetPathName();
				const FString ActorClass = Actor->GetClass()->GetPathName();
				const bool bKnownNonAcousticHelper =
					ComponentClass == TEXT("/Script/Landscape.LandscapeSplinesComponent")
					|| (ComponentClass == TEXT("/Script/Engine.BrushComponent")
						&& (ActorClass == TEXT("/Script/Engine.LightmassImportanceVolume")
							|| ActorClass == TEXT("/Script/Engine.BlockingVolume")));
				if (bKnownNonAcousticHelper && !Primitive->ComponentHasTag(TEXT("IMAcousticRequired")))
				{
					// These are editor spline controls, a lighting helper, or
					// an invisible gameplay blocker. The blocker exclusion is
					// provisional until its acoustic authoring intent is set.
					ProvisionalNonAcousticHelperPaths.Add(Primitive->GetPathName());
					continue;
				}
				UnsupportedStaticNonMeshPaths.Add(Primitive->GetPathName());
				AddFailure(Failures, FString::Printf(TEXT("Unsupported static non-mesh primitive intersects the bake region: %s (%s)."),
					*Primitive->GetPathName(), *Primitive->GetClass()->GetPathName()));
				bEnumerationComplete = false;
			}
		}
	}

	LandscapeCollisions = UniqueLandscapeCollisions.Array();
	LandscapeCollisions.Sort([](const ULandscapeHeightfieldCollisionComponent& A, const ULandscapeHeightfieldCollisionComponent& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	TArray<ULandscapeComponent*> LandscapeRenderComponents = UniqueLandscapeRenderComponents.Array();
	LandscapeRenderComponents.Sort([](const ULandscapeComponent& A, const ULandscapeComponent& B)
	{
		return A.GetPathName() < B.GetPathName();
	});

	for (ULandscapeHeightfieldCollisionComponent* Collision : LandscapeCollisions)
	{
		ULandscapeComponent* Render = Collision ? Collision->GetRenderComponent() : nullptr;
		if (!Collision || !Render || Render->GetCollisionComponent() != Collision)
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape collision/render pairing is invalid: %s."),
				Collision ? *Collision->GetPathName() : TEXT("NULL_COMPONENT")));
			bEnumerationComplete = false;
			continue;
		}
		if (Render->GetNumMaterials() != 1 || !AddMeshMaterial(Render->GetMaterial(0), UniqueMaterials, Failures, Render->GetPathName()))
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape render material slot contract is not one valid MI: %s."), *Render->GetPathName()));
			bEnumerationComplete = false;
		}
	}

	if (LandscapeCollisions.Num() != ExpectedLandscapeBlocks || LandscapeRenderComponents.Num() != ExpectedLandscapeBlocks)
	{
		AddFailure(Failures, FString::Printf(TEXT("Expected %d paired intersecting Landscape blocks, found collision=%d render=%d."),
			ExpectedLandscapeBlocks, LandscapeCollisions.Num(), LandscapeRenderComponents.Num()));
		bEnumerationComplete = false;
	}
	if (UniqueMaterials.IsEmpty())
	{
		AddFailure(Failures, TEXT("No static mesh or Landscape render materials were mapped."));
		bEnumerationComplete = false;
	}

	TArray<UMaterialInterface*> SortedMaterials = UniqueMaterials.Array();
	SortedMaterials.Sort([](const UMaterialInterface& A, const UMaterialInterface& B)
	{
		return A.GetPathName() < B.GetPathName();
	});
	Volume->Materials.Reset();
	TMap<UMaterialInterface*, int32> MaterialIndices;
	TArray<TSharedPtr<FJsonValue>> MaterialJson;
	for (int32 MaterialIndex = 0; MaterialIndex < SortedMaterials.Num(); ++MaterialIndex)
	{
		UMaterialInterface* Material = SortedMaterials[MaterialIndex];
		FIMAcousticMaterialMapping Mapping;
		Mapping.Material = Material;
		Mapping.Absorption = FVector(0.1, 0.1, 0.1);
		Mapping.Scattering = 0.5f;
		Volume->Materials.Add(Mapping);
		MaterialIndices.Add(Material, MaterialIndex);

		TSharedRef<FJsonObject> MaterialEntry = MakeShared<FJsonObject>();
		MaterialEntry->SetNumberField(TEXT("capture_material_index"), MaterialIndex);
		MaterialEntry->SetStringField(TEXT("asset_path"), Material->GetPathName());
		MaterialEntry->SetObjectField(TEXT("absorption"), MakeVectorJson(Mapping.Absorption));
		MaterialEntry->SetNumberField(TEXT("scattering"), Mapping.Scattering);
		MaterialJson.Add(MakeShared<FJsonValueObject>(MaterialEntry));
	}

	TArray<FIMExpectedVertex> ExpectedVertices;
	TArray<FIMExpectedTriangle> ExpectedTerrainTriangles;
	TArray<FIMTriangleKey> ExpectedHoleTriangles;
	TArray<FIMLandscapeBlockOracle> Blocks;
	int64 ExpectedLandscapeTriangles = 0;
	int64 LandscapePotentialTriangles = 0;
	int64 LandscapeVisibilityHoleQuads = 0;
	int32 LandscapeMaterialIndex = INDEX_NONE;
	for (ULandscapeHeightfieldCollisionComponent* Collision : LandscapeCollisions)
	{
		FIMLandscapeBlockOracle Block;
		Block.Collision = Collision;
		Block.Render = Collision ? Collision->GetRenderComponent() : nullptr;
		Block.Quads = Collision ? Collision->CollisionSizeQuads : 0;
		Block.Side = Block.Quads + 1;
		const int32 BlockIndex = Blocks.Add(MoveTemp(Block));
		FIMLandscapeBlockOracle& Current = Blocks[BlockIndex];

		if (!Collision || !Current.Render || Current.Quads != ExpectedLandscapeQuads
			|| Current.Quads <= 0 || Current.Quads > MaxLandscapeQuadsForOracle
			|| !FMath::IsNearlyEqual(Collision->CollisionScale, 1.0f)
			|| Current.Render->ComponentSizeQuads != Current.Quads
			|| Current.Render->GetNumMaterials() != 1
			|| !Collision->CookedPhysicalMaterials.IsValidIndex(0)
			|| !Collision->CookedPhysicalMaterials[0]
			|| !Collision->GetComponentTransform().TransformVectorNoScale(FVector::UpVector).Equals(FVector::UpVector, 1.e-4))
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape block violates the full-resolution oracle preconditions: %s."),
				Collision ? *Collision->GetPathName() : TEXT("NULL_COMPONENT")));
			bEnumerationComplete = false;
			continue;
		}

		UMaterialInterface* TerrainMaterial = Current.Render->GetMaterial(0);
		const int32* FoundMaterialIndex = MaterialIndices.Find(TerrainMaterial);
		if (!FoundMaterialIndex)
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape render MI is absent from the explicit mapping: %s."),
				*Current.Render->GetPathName()));
			bEnumerationComplete = false;
			continue;
		}
		if (LandscapeMaterialIndex == INDEX_NONE)
		{
			LandscapeMaterialIndex = *FoundMaterialIndex;
		}
		else if (LandscapeMaterialIndex != *FoundMaterialIndex)
		{
			AddFailure(Failures, TEXT("Intersecting Landscape blocks do not share one render material index."));
			bEnumerationComplete = false;
		}

		const int64 SampleCount64 = static_cast<int64>(Current.Side) * Current.Side;
		if (SampleCount64 > MAX_int32)
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape tile sample count exceeds TArray bounds: %s."), *Collision->GetPathName()));
			bEnumerationComplete = false;
			continue;
		}
		Current.Heights.Init(std::numeric_limits<float>::quiet_NaN(), static_cast<int32>(SampleCount64));
		Current.SurfaceIndices.Init(0, static_cast<int32>(SampleCount64));
		const bool bHeightFilled = Collision->FillHeightTile(MakeArrayView(Current.Heights), 0, Current.Side);
		const bool bMaterialsFilled = Collision->FillMaterialIndexTile(MakeArrayView(Current.SurfaceIndices), 0, Current.Side);
		if (!bHeightFilled || !bMaterialsFilled)
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape FillHeightTile/FillMaterialIndexTile failed: %s."), *Collision->GetPathName()));
			bEnumerationComplete = false;
			continue;
		}

		bool bFiniteHeights = true;
		for (const float Height : Current.Heights)
		{
			bFiniteHeights &= FMath::IsFinite(Height);
		}
		if (!bFiniteHeights)
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape height tile contains non-finite samples: %s."), *Collision->GetPathName()));
			bEnumerationComplete = false;
			continue;
		}

		Current.SdkVertices.SetNumUninitialized(static_cast<int32>(SampleCount64));
		const FTransform Transform = Collision->GetComponentTransform();
		for (int32 Y = 0; Y < Current.Side; ++Y)
		{
			for (int32 X = 0; X < Current.Side; ++X)
			{
				const int32 Index = Y * Current.Side + X;
				const FVector XY = Transform.TransformPosition(FVector(X * Collision->CollisionScale,
					Y * Collision->CollisionScale, 0.0));
				const FVector WorldPosition(XY.X, XY.Y, Current.Heights[Index]);
				const FVector SdkPosition = ToOracleSdkPosition(WorldPosition, Volume->BakeBounds->GetComponentLocation());
				Current.SdkVertices[Index] = SdkPosition;
			}
		}

		const bool bFlip = Transform.GetDeterminant() < 0.0;
		TSet<int32> SelectedVertexIndices;
		int64 SelectedQuadCount = 0;
		for (int32 Y = 0; Y < Current.Quads; ++Y)
		{
			for (int32 X = 0; X < Current.Quads; ++X)
			{
				const int32 I = Y * Current.Side + X;
				const uint8 Surface = Current.SurfaceIndices[I];
				if (!IsExpectedTerrainQuad(Surface))
				{
					AddFailure(Failures, FString::Printf(TEXT("Landscape block has an unsupported physical-material index %u: %s."),
						static_cast<uint32>(Surface), *Collision->GetPathName()));
					bEnumerationComplete = false;
					continue;
				}

				const int32 CellCorners[4] = { I, I + Current.Side, I + 1, I + Current.Side + 1 };
				FBox CellBounds(ForceInit);
				for (const int32 CornerIndex : CellCorners)
				{
					const int32 CornerX = CornerIndex % Current.Side;
					const int32 CornerY = CornerIndex / Current.Side;
					const FVector XY = Transform.TransformPosition(FVector(CornerX * Collision->CollisionScale,
						CornerY * Collision->CollisionScale, 0.0));
					CellBounds += FVector(XY.X, XY.Y, Current.Heights[CornerIndex]);
				}
				if (!Region.Intersect(CellBounds)) continue;
				++SelectedQuadCount;
				if (Surface != TNumericLimits<uint8>::Max())
				{
					for (const int32 CornerIndex : CellCorners) SelectedVertexIndices.Add(CornerIndex);
				}

				const bool bHole = Surface == TNumericLimits<uint8>::Max();
				if (bHole)
				{
					++Current.VisibilityHoleQuads;
					++LandscapeVisibilityHoleQuads;
					if (Current.RepresentativeHoleX == INDEX_NONE)
					{
						Current.RepresentativeHoleX = X;
						Current.RepresentativeHoleY = Y;
					}
				}
				else if (Current.RepresentativeFaceX == INDEX_NONE)
				{
					Current.RepresentativeFaceX = X;
					Current.RepresentativeFaceY = Y;
				}

				const int32 TriangleIndices[2][3] = {
					{ I, I + Current.Side, I + 1 },
					{ I + 1, I + Current.Side, I + Current.Side + 1 }
				};
				for (int32 TriangleIndex = 0; TriangleIndex < 2; ++TriangleIndex)
				{
					int32 A = TriangleIndices[TriangleIndex][0];
					int32 B = TriangleIndices[TriangleIndex][1];
					int32 C = TriangleIndices[TriangleIndex][2];
					if (bFlip) Swap(B, C);
					const FIMTriangleKey Key = MakeTriangleKey(Current.SdkVertices[A], Current.SdkVertices[B], Current.SdkVertices[C]);
					if (bHole)
					{
						ExpectedHoleTriangles.Add(Key);
					}
					else
					{
						ExpectedTerrainTriangles.Add({ Key, BlockIndex });
					}
				}
			}
		}

		for (const int32 VertexIndex : SelectedVertexIndices)
		{
			ExpectedVertices.Add({ MakePositionKey(Current.SdkVertices[VertexIndex]), BlockIndex });
		}
		LandscapePotentialTriangles += SelectedQuadCount * 2;
		Current.ExpectedTriangles = SelectedQuadCount * 2 - Current.VisibilityHoleQuads * 2;
		ExpectedLandscapeTriangles += Current.ExpectedTriangles;
	}

	if (LandscapeMaterialIndex == INDEX_NONE)
	{
		AddFailure(Failures, TEXT("No single Landscape capture material index could be established."));
		bEnumerationComplete = false;
	}

	TArray<TSharedPtr<FJsonValue>> BlockJson;
	for (const FIMLandscapeBlockOracle& Block : Blocks)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("collision_component_path"), Block.Collision ? Block.Collision->GetPathName() : TEXT("NULL_COMPONENT"));
		Item->SetStringField(TEXT("render_component_path"), Block.Render ? Block.Render->GetPathName() : TEXT("NULL_RENDER_COMPONENT"));
		Item->SetNumberField(TEXT("collision_size_quads"), Block.Quads);
		Item->SetNumberField(TEXT("collision_scale_cm"), Block.Collision ? Block.Collision->CollisionScale : 0.0);
		Item->SetNumberField(TEXT("height_sample_count"), Block.Heights.Num());
		Item->SetNumberField(TEXT("potential_triangles_before_holes"), static_cast<double>(Block.ExpectedTriangles + Block.VisibilityHoleQuads * 2));
		Item->SetNumberField(TEXT("whole_collision_block_potential_triangles"), static_cast<double>(Block.Quads) * Block.Quads * 2.0);
		Item->SetNumberField(TEXT("visibility_hole_quad_count_255"), Block.VisibilityHoleQuads);
		Item->SetNumberField(TEXT("expected_triangles_after_holes"), Block.ExpectedTriangles);
		Item->SetNumberField(TEXT("matched_capture_triangles"), Block.MatchedTriangles);
		Item->SetNumberField(TEXT("matched_capture_vertices"), Block.MatchedVertices);
		Item->SetNumberField(TEXT("captured_upward_triangle_count"), Block.CapturedUpwardTriangles);
		Item->SetNumberField(TEXT("representative_non_hole_face_x"), Block.RepresentativeFaceX);
		Item->SetNumberField(TEXT("representative_non_hole_face_y"), Block.RepresentativeFaceY);
		Item->SetNumberField(TEXT("representative_hole_x"), Block.RepresentativeHoleX);
		Item->SetNumberField(TEXT("representative_hole_y"), Block.RepresentativeHoleY);
		if (Block.Collision)
		{
			Item->SetObjectField(TEXT("collision_bounds"), MakeVectorJson(Block.Collision->Bounds.Origin));
			Item->SetNumberField(TEXT("collision_bounds_sphere_radius_cm"), Block.Collision->Bounds.SphereRadius);
		}
		BlockJson.Add(MakeShared<FJsonValueObject>(Item));
	}

	TArray<FVector> CapturedVertices;
	TArray<FIntVector> CapturedTriangles;
	TArray<int32> CapturedMaterialIndices;
	FString Fingerprint;
	const FPlatformMemoryStats MemoryBefore = FPlatformMemory::GetStats();
	const double CaptureStartedAt = FPlatformTime::Seconds();
	const bool bCaptureSucceeded = bEnumerationComplete && Volume->IM_CaptureSceneGeometryForTest(
		CapturedVertices, CapturedTriangles, CapturedMaterialIndices, Fingerprint);
	const double CaptureWallMs = (FPlatformTime::Seconds() - CaptureStartedAt) * 1000.0;
	const FPlatformMemoryStats MemoryAfter = FPlatformMemory::GetStats();
	if (!bCaptureSucceeded)
	{
		AddFailure(Failures, FString::Printf(TEXT("Product IM_CaptureSceneGeometryForTest failed: %s."), *Volume->Status));
	}

	TArray<FIMExpectedTriangle> ExpectedTriangles;
	TArray<FIMObservedTriangle> CapturedLandscapeMaterialTriangles;
	TArray<FIMPositionKey> CapturedVertexKeys;
	int64 CapturedTrianglesByMaterialCount = 0;
	TArray<int64> CapturedTrianglesPerMaterial;
	CapturedTrianglesPerMaterial.Init(0, SortedMaterials.Num());
	bool bCaptureArraysValid = bCaptureSucceeded && CapturedTriangles.Num() == CapturedMaterialIndices.Num()
		&& !Fingerprint.IsEmpty();
	if (!bCaptureArraysValid)
	{
		AddFailure(Failures, TEXT("Capture arrays have inconsistent counts or the scene fingerprint is empty."));
	}

	if (bCaptureArraysValid)
	{
		CapturedVertexKeys.Reserve(CapturedVertices.Num());
		for (const FVector& Vertex : CapturedVertices)
		{
			if (!IsValidFiniteVector(Vertex))
			{
				bCaptureArraysValid = false;
				AddFailure(Failures, TEXT("Captured vertex array contains a non-finite SDK coordinate."));
				break;
			}
			CapturedVertexKeys.Add(MakePositionKey(Vertex));
		}
		for (int32 TriangleIndex = 0; TriangleIndex < CapturedTriangles.Num(); ++TriangleIndex)
		{
			const FIntVector& Triangle = CapturedTriangles[TriangleIndex];
			const int32 MaterialIndex = CapturedMaterialIndices[TriangleIndex];
			if (Triangle.X < 0 || Triangle.Y < 0 || Triangle.Z < 0
				|| Triangle.X >= CapturedVertices.Num() || Triangle.Y >= CapturedVertices.Num() || Triangle.Z >= CapturedVertices.Num()
				|| !CapturedTrianglesPerMaterial.IsValidIndex(MaterialIndex))
			{
				bCaptureArraysValid = false;
				AddFailure(Failures, FString::Printf(TEXT("Captured triangle/material index is out of range at triangle %d."), TriangleIndex));
				break;
			}
			++CapturedTrianglesPerMaterial[MaterialIndex];
			if (MaterialIndex == LandscapeMaterialIndex)
			{
				const FVector A = CapturedVertices[Triangle.X];
				const FVector B = CapturedVertices[Triangle.Y];
				const FVector C = CapturedVertices[Triangle.Z];
				const FVector Normal = FVector::CrossProduct(B - A, C - A);
				FIMObservedTriangle Observed;
				Observed.Key = MakeTriangleKey(A, B, C);
				Observed.bNormalPointsUp = Normal.Y > 1.e-12;
				Observed.bNonDegenerate = Normal.SizeSquared() > 1.e-20;
				CapturedLandscapeMaterialTriangles.Add(Observed);
				++CapturedTrianglesByMaterialCount;
			}
		}
	}

	TArray<int64> MatchedVerticesPerBlock;
	TArray<int64> MatchedTrianglesPerBlock;
	MatchedVerticesPerBlock.Init(0, Blocks.Num());
	MatchedTrianglesPerBlock.Init(0, Blocks.Num());
	int64 MissingTerrainVertices = 0;
	int64 MissingTerrainTriangles = 0;
	int64 BadTerrainNormals = 0;
	int64 CapturedHoleTriangles = 0;
	if (bCaptureArraysValid)
	{
		CompareExpectedVertices(ExpectedVertices, CapturedVertexKeys, MatchedVerticesPerBlock, MissingTerrainVertices);
		CompareExpectedTriangles(ExpectedTerrainTriangles, CapturedLandscapeMaterialTriangles,
			MatchedTrianglesPerBlock, MissingTerrainTriangles, BadTerrainNormals);
		CapturedHoleTriangles = CountCapturedHoleTriangles(ExpectedHoleTriangles, CapturedLandscapeMaterialTriangles);
		for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
		{
			Blocks[BlockIndex].MatchedVertices = MatchedVerticesPerBlock[BlockIndex];
			Blocks[BlockIndex].MatchedTriangles = MatchedTrianglesPerBlock[BlockIndex];
			Blocks[BlockIndex].CapturedUpwardTriangles = MatchedTrianglesPerBlock[BlockIndex];
		}
	}

	if (MissingTerrainVertices != 0 || MissingTerrainTriangles != 0)
	{
		AddFailure(Failures, FString::Printf(TEXT("Capture is missing terrain oracle geometry: vertices=%lld triangles=%lld."),
			MissingTerrainVertices, MissingTerrainTriangles));
	}
	if (BadTerrainNormals != 0)
	{
		AddFailure(Failures, FString::Printf(TEXT("Captured terrain triangles have invalid/upside-down normals: %lld."), BadTerrainNormals));
	}
	if (CapturedHoleTriangles != 0)
	{
		AddFailure(Failures, FString::Printf(TEXT("Capture emitted %lld triangles matching 255 visibility-hole cells."), CapturedHoleTriangles));
	}
	if (bCaptureSucceeded && CapturedTriangles.Num() != ExpectedStaticMeshTriangles + ExpectedLandscapeTriangles)
	{
		AddFailure(Failures, FString::Printf(TEXT("Captured triangle total differs from the independent SM+Landscape oracle: captured=%d sm_expected=%lld terrain_expected=%lld."),
			CapturedTriangles.Num(), ExpectedStaticMeshTriangles, ExpectedLandscapeTriangles));
	}
	if (bCaptureSucceeded && Volume->ExportedTriangles != CapturedTriangles.Num())
	{
		AddFailure(Failures, TEXT("Volume ExportedTriangles differs from the test accessor triangle count."));
	}

	// Explicitly test representative source positions and faces from every tile,
	// in addition to the whole-grid vertex and material-tagged triangle multisets.
	int64 RepresentativeVertexChecks = 0;
	int64 RepresentativeFaceChecks = 0;
	for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
	{
		const FIMLandscapeBlockOracle& Block = Blocks[BlockIndex];
		if (Block.SdkVertices.Num() != Block.Side * Block.Side || Block.RepresentativeFaceX == INDEX_NONE)
		{
			AddFailure(Failures, FString::Printf(TEXT("Landscape block has no valid representative sample: %s."),
				Block.Collision ? *Block.Collision->GetPathName() : TEXT("NULL_COMPONENT")));
			continue;
		}
		// The geometric middle of a 255-quad collision block can lie far
		// outside this 10 m bake region; sample a selected face instead.
		const int32 SampleX = Block.RepresentativeFaceX;
		const int32 SampleY = Block.RepresentativeFaceY;
		const FVector& Sample = Block.SdkVertices[SampleY * Block.Side + SampleX];
		bool bSampleFound = false;
		for (const FVector& Captured : CapturedVertices)
		{
			if (MakePositionKey(Captured) == MakePositionKey(Sample))
			{
				bSampleFound = true;
				break;
			}
		}
		++RepresentativeVertexChecks;
		if (!bSampleFound)
		{
			AddFailure(Failures, FString::Printf(TEXT("Representative Landscape SDK-space vertex was not found for %s."),
				Block.Collision ? *Block.Collision->GetPathName() : TEXT("NULL_COMPONENT")));
		}

		const int32 FaceX = Block.RepresentativeFaceX;
		const int32 FaceY = Block.RepresentativeFaceY;
		const int32 I = FaceY * Block.Side + FaceX;
		const bool bFlip = Block.Collision->GetComponentTransform().GetDeterminant() < 0.0;
		const int32 CandidateTriangles[2][3] = {
			{ I, I + Block.Side, I + 1 },
			{ I + 1, I + Block.Side, I + Block.Side + 1 }
		};
		bool bBothFacesFound = true;
		for (int32 Face = 0; Face < 2; ++Face)
		{
			int32 A = CandidateTriangles[Face][0];
			int32 B = CandidateTriangles[Face][1];
			int32 C = CandidateTriangles[Face][2];
			if (bFlip) Swap(B, C);
			const FIMTriangleKey ExpectedFace = MakeTriangleKey(Block.SdkVertices[A], Block.SdkVertices[B], Block.SdkVertices[C]);
			bool bFound = false;
			for (int32 CapturedIndex = 0; CapturedIndex < CapturedTriangles.Num(); ++CapturedIndex)
			{
				if (CapturedMaterialIndices[CapturedIndex] != LandscapeMaterialIndex) continue;
				const FIntVector& CapturedTriangle = CapturedTriangles[CapturedIndex];
				const FIMTriangleKey ActualFace = MakeTriangleKey(CapturedVertices[CapturedTriangle.X],
					CapturedVertices[CapturedTriangle.Y], CapturedVertices[CapturedTriangle.Z]);
				if (TriangleKeyEqual(ExpectedFace, ActualFace))
				{
					const FVector ActualNormal = FVector::CrossProduct(
						CapturedVertices[CapturedTriangle.Y] - CapturedVertices[CapturedTriangle.X],
						CapturedVertices[CapturedTriangle.Z] - CapturedVertices[CapturedTriangle.X]);
					bFound = ActualNormal.Y > 1.e-12 && ActualNormal.SizeSquared() > 1.e-20;
					break;
				}
			}
			bBothFacesFound &= bFound;
			++RepresentativeFaceChecks;
		}
		if (!bBothFacesFound)
		{
			AddFailure(Failures, FString::Printf(TEXT("Representative Landscape cell faces/SDK-up normals are absent: %s cell=(%d,%d)."),
				Block.Collision ? *Block.Collision->GetPathName() : TEXT("NULL_COMPONENT"), FaceX, FaceY));
		}
	}

	if (!bCaptureSucceeded || !bCaptureArraysValid)
	{
		bEnumerationComplete = false;
	}
	const bool bPassed = bMapIdentityMatches && bEnumerationComplete && bCaptureSucceeded && bCaptureArraysValid
		&& Blocks.Num() == ExpectedLandscapeBlocks && LandscapePotentialTriangles > 0
		&& LandscapePotentialTriangles == ExpectedLandscapeTriangles + LandscapeVisibilityHoleQuads * 2
		&& ExpectedTerrainTriangles.Num() == ExpectedLandscapeTriangles
		&& MissingTerrainVertices == 0 && MissingTerrainTriangles == 0 && BadTerrainNormals == 0
		&& CapturedHoleTriangles == 0 && RepresentativeVertexChecks == ExpectedLandscapeBlocks
		&& RepresentativeFaceChecks == ExpectedLandscapeBlocks * 2 && Failures.IsEmpty();
	OutFailure = FString::Join(Failures, TEXT(" "));

	for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
	{
		TSharedPtr<FJsonValue> Value = BlockJson.IsValidIndex(BlockIndex) ? BlockJson[BlockIndex] : nullptr;
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			Value->AsObject()->SetNumberField(TEXT("matched_capture_triangles"), Blocks[BlockIndex].MatchedTriangles);
			Value->AsObject()->SetNumberField(TEXT("matched_capture_vertices"), Blocks[BlockIndex].MatchedVertices);
			Value->AsObject()->SetNumberField(TEXT("captured_upward_triangle_count"), Blocks[BlockIndex].CapturedUpwardTriangles);
		}
	}

	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetStringField(TEXT("requested_map_package"), TargetMapPackage);
	Report->SetStringField(TEXT("loaded_map_package"), LoadedMap);
	Report->SetBoolField(TEXT("load_map_returned_success"), bMapLoadReturned);
	Report->SetBoolField(TEXT("target_map_identity_matches"), bMapIdentityMatches);
	Report->SetNumberField(TEXT("map_load_ms"), MapLoadMs);
	Report->SetStringField(TEXT("map_package_filename"), FPackageName::LongPackageNameToFilename(
		TargetMapPackage, FPackageName::GetMapPackageExtension()));
	Report->SetStringField(TEXT("map_package_sha256"), TEXT("UNKNOWN_EXTERNAL_VERIFICATION_REQUIRED"));
	Report->SetStringField(TEXT("terminal_status"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
	Report->SetStringField(TEXT("terminal_reason"), OutFailure.IsEmpty()
		? TEXT("Independent static mesh and Landscape geometry oracle matched the product capture.") : OutFailure);
	Report->SetStringField(TEXT("scope"), TEXT("Transient AIMAcousticBakeVolume only. Calls IM_CaptureSceneGeometryForTest; does not GenerateProbes, Bake, or save the Rural map."));

	TSharedRef<FJsonObject> RegionJson = MakeShared<FJsonObject>();
	RegionJson->SetObjectField(TEXT("center_world_cm"), MakeVectorJson(RegionCenterCm));
	RegionJson->SetObjectField(TEXT("actual_bounds_center_world_cm"), MakeVectorJson(Region.GetCenter()));
	RegionJson->SetObjectField(TEXT("half_extent_world_cm"), MakeVectorJson(FVector(RegionHalfExtentCm, RegionHalfExtentCm, RegionHalfExtentCm)));
	RegionJson->SetNumberField(TEXT("probe_spacing_cm"), ProbeSpacingCm);
	RegionJson->SetNumberField(TEXT("probe_height_cm"), ProbeHeightCm);
	Report->SetObjectField(TEXT("capture_region"), RegionJson);

	TSharedRef<FJsonObject> GeometryJson = MakeShared<FJsonObject>();
	GeometryJson->SetNumberField(TEXT("static_mesh_component_count_in_region"), StaticMeshComponentCount);
	GeometryJson->SetNumberField(TEXT("static_ism_component_count_in_region"), StaticISMComponentCount);
	GeometryJson->SetNumberField(TEXT("static_hism_component_count_in_region"), StaticHISMComponentCount);
	GeometryJson->SetNumberField(TEXT("static_mesh_instance_count_in_region"), StaticMeshInstanceCount);
	GeometryJson->SetNumberField(TEXT("expected_static_mesh_lod0_triangles"), ExpectedStaticMeshTriangles);
	GeometryJson->SetArrayField(TEXT("included_mesh_components"), IncludedMeshComponentsJson);
	GeometryJson->SetNumberField(TEXT("unsupported_static_non_mesh_count"), UnsupportedStaticNonMeshPaths.Num());
	GeometryJson->SetNumberField(TEXT("provisional_non_acoustic_helper_count"), ProvisionalNonAcousticHelperPaths.Num());
	TArray<TSharedPtr<FJsonValue>> HelperPathsJson;
	for (const FString& Path : ProvisionalNonAcousticHelperPaths)
	{
		HelperPathsJson.Add(MakeShared<FJsonValueString>(Path));
	}
	GeometryJson->SetArrayField(TEXT("provisional_non_acoustic_helper_paths"), HelperPathsJson);
	GeometryJson->SetStringField(TEXT("blocking_volume_acoustic_intent"), TEXT("UNRESOLVED_PROVISIONAL_EXCLUSION"));
	GeometryJson->SetNumberField(TEXT("ignored_non_static_or_explicitly_ignored_component_count"), IgnoredComponentCount);
	TArray<TSharedPtr<FJsonValue>> UnsupportedPathsJson;
	const int32 UnsupportedPathCountToWrite = FMath::Min(UnsupportedStaticNonMeshPaths.Num(), MaxFailureDetails);
	for (int32 Index = 0; Index < UnsupportedPathCountToWrite; ++Index)
	{
		UnsupportedPathsJson.Add(MakeShared<FJsonValueString>(UnsupportedStaticNonMeshPaths[Index]));
	}
	GeometryJson->SetArrayField(TEXT("unsupported_static_non_mesh_paths"), UnsupportedPathsJson);
	Report->SetObjectField(TEXT("geometry_inventory"), GeometryJson);

	TSharedRef<FJsonObject> TerrainJson = MakeShared<FJsonObject>();
	TerrainJson->SetNumberField(TEXT("expected_intersecting_landscape_blocks"), ExpectedLandscapeBlocks);
	TerrainJson->SetNumberField(TEXT("intersecting_landscape_collision_blocks"), LandscapeCollisions.Num());
	TerrainJson->SetNumberField(TEXT("intersecting_landscape_render_blocks"), LandscapeRenderComponents.Num());
	TerrainJson->SetNumberField(TEXT("quads_per_block_expected"), ExpectedLandscapeQuads * ExpectedLandscapeQuads);
	TerrainJson->SetNumberField(TEXT("potential_triangles_per_block_before_holes"), ExpectedLandscapeQuads * ExpectedLandscapeQuads * 2);
	TerrainJson->SetNumberField(TEXT("potential_triangles_all_blocks_before_holes"), static_cast<double>(LandscapePotentialTriangles));
	TerrainJson->SetNumberField(TEXT("visibility_hole_quad_count_255"), static_cast<double>(LandscapeVisibilityHoleQuads));
	TerrainJson->SetNumberField(TEXT("expected_triangles_after_holes"), static_cast<double>(ExpectedLandscapeTriangles));
	TerrainJson->SetNumberField(TEXT("matched_capture_terrain_triangles"), static_cast<double>(ExpectedTerrainTriangles.Num() - MissingTerrainTriangles));
	TerrainJson->SetNumberField(TEXT("missing_capture_terrain_triangles"), static_cast<double>(MissingTerrainTriangles));
	TerrainJson->SetNumberField(TEXT("expected_terrain_vertex_samples"), static_cast<double>(ExpectedVertices.Num()));
	TerrainJson->SetNumberField(TEXT("matched_capture_terrain_vertex_samples"), static_cast<double>(ExpectedVertices.Num() - MissingTerrainVertices));
	TerrainJson->SetNumberField(TEXT("missing_capture_terrain_vertex_samples"), static_cast<double>(MissingTerrainVertices));
	TerrainJson->SetNumberField(TEXT("representative_vertex_checks"), RepresentativeVertexChecks);
	TerrainJson->SetNumberField(TEXT("representative_face_checks"), RepresentativeFaceChecks);
	TerrainJson->SetNumberField(TEXT("invalid_captured_terrain_normals"), static_cast<double>(BadTerrainNormals));
	TerrainJson->SetNumberField(TEXT("captured_triangles_matching_expected_hole_cells"), static_cast<double>(CapturedHoleTriangles));
	TerrainJson->SetNumberField(TEXT("capture_landscape_material_index"), LandscapeMaterialIndex);
	TerrainJson->SetNumberField(TEXT("position_quantization_m"), 1.0 / PositionKeyUnitsPerMeter);
	TerrainJson->SetStringField(TEXT("coordinate_basis"), TEXT("Accessor positions are SDK meters (UE Y, UE Z, -UE X), relative to the transient BakeBounds origin; expected positions are recomputed from FillHeightTile and component transforms."));
	TerrainJson->SetStringField(TEXT("normal_basis"), TEXT("Triangle cross products are checked against SDK +Y, which is world-up under the capture coordinate mapping."));
	TerrainJson->SetStringField(TEXT("hole_rule"), TEXT("Each quad index 255 is counted as a visibility hole; its two expected triangle keys must be absent from captured triangles carrying the Landscape material index."));
	TerrainJson->SetArrayField(TEXT("blocks"), BlockJson);
	Report->SetObjectField(TEXT("landscape_geometry_oracle"), TerrainJson);

	TArray<TSharedPtr<FJsonValue>> MaterialsByPath;
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialJson.Num(); ++MaterialIndex)
	{
		TSharedPtr<FJsonValue> MaterialValue = MaterialJson[MaterialIndex];
		if (MaterialValue.IsValid() && MaterialValue->Type == EJson::Object)
		{
			MaterialValue->AsObject()->SetNumberField(TEXT("captured_triangle_count"),
				CapturedTrianglesPerMaterial.IsValidIndex(MaterialIndex) ? CapturedTrianglesPerMaterial[MaterialIndex] : 0);
		}
		MaterialsByPath.Add(MaterialValue);
	}
	Report->SetArrayField(TEXT("explicit_unique_material_mappings"), MaterialsByPath);
	Report->SetNumberField(TEXT("unique_material_count"), SortedMaterials.Num());
	Report->SetNumberField(TEXT("captured_landscape_material_triangle_count"), CapturedTrianglesByMaterialCount);

	TSharedRef<FJsonObject> CaptureJson = MakeShared<FJsonObject>();
	CaptureJson->SetBoolField(TEXT("product_capture_succeeded"), bCaptureSucceeded);
	CaptureJson->SetStringField(TEXT("scene_fingerprint"), Fingerprint.IsEmpty() ? TEXT("EMPTY") : Fingerprint);
	CaptureJson->SetNumberField(TEXT("capture_accessor_wall_ms"), CaptureWallMs);
	CaptureJson->SetNumberField(TEXT("captured_vertex_count"), CapturedVertices.Num());
	CaptureJson->SetNumberField(TEXT("captured_triangle_count"), CapturedTriangles.Num());
	CaptureJson->SetNumberField(TEXT("volume_exported_triangle_count"), Volume->ExportedTriangles);
	CaptureJson->SetStringField(TEXT("timing_note"), TEXT("Wall time around IM_CaptureSceneGeometryForTest includes CaptureScene, current diagnostic output, and test-array readback copy; the accessor separately emits IMLogs AcousticG2CaptureCore for core CaptureScene timing."));
	Report->SetObjectField(TEXT("product_capture"), CaptureJson);
	Report->SetObjectField(TEXT("process_memory"), MakeMemoryJson(MemoryBefore, MemoryAfter));

	TArray<TSharedPtr<FJsonValue>> FailureJson;
	for (const FString& Failure : Failures)
	{
		FailureJson.Add(MakeShared<FJsonValueString>(Failure));
	}
	Report->SetNumberField(TEXT("failure_count"), Failures.Num());
	Report->SetArrayField(TEXT("failures"), FailureJson);

	Volume->Destroy();
	return bPassed;
}

static bool RunTest(FAutomationTestBase* Test)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralGeometryOracle"), RunId);
	const FString ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
	const double LoadStartedAt = FPlatformTime::Seconds();
	const bool bMapLoadReturned = FEditorFileUtils::LoadMap(TargetMapPackage, false, true);
	const double MapLoadMs = (FPlatformTime::Seconds() - LoadStartedAt) * 1000.0;

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	FString FailureReason;
	const bool bPassed = RunOracle(Test, RunId, bMapLoadReturned, MapLoadMs, Report, FailureReason);
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
		TEXT("IMLogs AcousticRuralGeometryOracle run_id=%s status=%s report=%s"),
		*RunId, bPassed && bReportWritten ? TEXT("PASS") : TEXT("INCONCLUSIVE"), *ReportPath);

	if (bPassed && bReportWritten)
	{
		Test->AddInfo(FString::Printf(TEXT("Rural region geometry oracle report: %s"), *ReportPath));
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralRegionGeometryOracle PASS"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS RuralRegionGeometryOracle"));
		return true;
	}

	Test->AddError(FString::Printf(TEXT("Rural region geometry oracle failed closed. %s Report: %s"),
		*FailureReason, *ReportPath));
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralRegionGeometryOracle,
	"IceMoon.AcousticField.Rural.RegionGeometryOracle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralRegionGeometryOracle::RunTest(const FString&)
{
	return IMAcousticRuralGeometryTestPrivate::RunTest(this);
}

#endif
