#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

// Persistent Rural Camera3 preview authoring.  This command deliberately does
// not run PIE: Root owns the later ordinary Play/listener evidence.  The first
// phase is read-only with respect to the source map and rejects WorldPartition,
// streaming levels, external actors, existing audio, and an existing target
// package before a duplicated map can be saved.

#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "CollisionShape.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AmbientSound.h"
#include "StaticMeshResources.h"
#include "Components/SplineMeshComponent.h"
#include "UObject/Package.h"
#include "ObjectTools.h"

namespace IMAcousticRuralPreviewPrepareTestPrivate
{
constexpr const TCHAR* RuralSourceMap = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr const TCHAR* RuralTargetMap = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* RuralCameraClass = TEXT("/Script/CinematicCamera.CineCameraActor");
constexpr const TCHAR* RuralCameraName = TEXT("Camera3");
constexpr const TCHAR* SkyMeshPath = TEXT("/Engine/EngineSky/SM_SkySphere.SM_SkySphere");
constexpr const TCHAR* HorizonMeshPath = TEXT("/Game/RuralAustralia/Landscape/Horizon_01/SM_Horizon_01.SM_Horizon_01");
constexpr const TCHAR* WaterDropMetaSound = TEXT("/IceMoonAcousticField/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay");
constexpr const TCHAR* SourceTag = TEXT("IMAcousticRuralPreviewWaterDropV1");
constexpr const TCHAR* ReadyTag = TEXT("IMAcousticRuralPreviewPreparedV1");
constexpr const TCHAR* PlayerStartTag = TEXT("IMAcousticRuralPreviewPlayerStartV1");
constexpr const TCHAR* IgnoreTag = TEXT("IMAcousticIgnore");
constexpr const TCHAR* ProvisionalBackdropTag = TEXT("IMAcousticRuralPreviewProvisionalBackdrop");
constexpr const TCHAR* ProvisionalBlockingTag = TEXT("IMAcousticRuralPreviewProvisionalBlockingVolume");
constexpr const TCHAR* MutedBackgroundTag = TEXT("IMAcousticRuralPreviewMutedOriginalAmbient");
constexpr const TCHAR* OriginalBackgroundSound = TEXT("/Game/RuralAustralia/Audio/RuralAustralia_01.RuralAustralia_01");
constexpr const TCHAR* BlockingVolumeClass = TEXT("/Script/Engine.BlockingVolume");
constexpr const TCHAR* LandscapeSplineClass = TEXT("/Script/Landscape.LandscapeSplinesComponent");
constexpr const TCHAR* BrushComponentClass = TEXT("/Script/Engine.BrushComponent");
constexpr const TCHAR* LightmassImportanceClass = TEXT("/Script/Engine.LightmassImportanceVolume");
constexpr double RegionHalfExtentCm = 500.0;
constexpr double TerrainBelowMarginCm = 50.0;
constexpr double TerrainAboveMarginCm = 500.0;
constexpr float ProvisionalAbsorption = 0.1f;
constexpr float ProvisionalScattering = 0.5f;
constexpr double MaxSeconds = 900.0;
constexpr int32 MaxReportPaths = 256;

struct FIMRuralGeometryComponent
{
	FString RelativePath;
	FString ComponentClass;
	FString MeshPath;
	FString LODKey;
	FString MaterialKey;
	FString MobilityCollisionKey;
	FString TransformKey;
	FString InstanceKey;
	FString StrictKey;
	FString SplineKey;
	TArray<double> SplineShapeCm;
	TArray<double> SplineShapeUnitless;
	FVector WorldLocation = FVector::ZeroVector;
	bool bGeneratedSpline = false;
};

struct FIMRuralGeometry
{
	FString Hash;
	int32 StaticMeshComponents = 0;
	int32 LandscapeComponents = 0;
	int32 GeneratedSplineComponents = 0;
	TArray<FString> Rows;
	TArray<FIMRuralGeometryComponent> Components;
};

static FString HashBytes(const uint8* Data, const int32 Count)
{
	FSHA1 Hash;
	if (Data && Count > 0) Hash.Update(Data, Count);
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

static bool HashFile(const FString& Filename, FString& OutHash, int64& OutBytes, FString& Error)
{
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
	if (!Reader)
	{
		Error = TEXT("Cannot open file for hashing: ") + Filename;
		return false;
	}
	OutBytes = Reader->TotalSize();
	FSHA1 Hash;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(1024 * 1024);
	while (!Reader->AtEnd())
	{
		const int64 Remaining = Reader->TotalSize() - Reader->Tell();
		const int64 Chunk = FMath::Min<int64>(Remaining, Buffer.Num());
		if (Chunk <= 0) break;
		Reader->Serialize(Buffer.GetData(), Chunk);
		if (Reader->IsError())
		{
			Error = TEXT("Read failed while hashing: ") + Filename;
			return false;
		}
		Hash.Update(Buffer.GetData(), static_cast<uint32>(Chunk));
	}
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	OutHash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	return true;
}

static FString TransformKey(const FTransform& Transform)
{
	const FVector P = Transform.GetLocation();
	const FQuat Q = Transform.GetRotation();
	const FVector S = Transform.GetScale3D();
	return FString::Printf(TEXT("%.17g,%.17g,%.17g|%.17g,%.17g,%.17g,%.17g|%.17g,%.17g,%.17g"),
		P.X, P.Y, P.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
}

static FString VectorKey(const FVector& Value)
{
	return FString::Printf(TEXT("%.17g,%.17g,%.17g"), Value.X, Value.Y, Value.Z);
}

static FString Vector2Key(const FVector2D& Value)
{
	return FString::Printf(TEXT("%.17g,%.17g"), Value.X, Value.Y);
}

static FString RotationScaleKey(const FTransform& Transform)
{
	const FQuat Q = Transform.GetRotation();
	const FVector S = Transform.GetScale3D();
	return FString::Printf(TEXT("rot=%.17g,%.17g,%.17g,%.17g|scale=%.17g,%.17g,%.17g"),
		Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
}

// A duplicated level may recreate a layer-local MID under the target world
// package. Normalize only that package prefix, preserving the level/actor/
// component/MID suffix. Regular material assets remain strict path matches.
static FString CanonicalMaterialPath(const UMaterialInterface* Material)
{
	const FString Path = GetPathNameSafe(Material);
	if (!Material || !Material->IsA<UMaterialInstanceDynamic>()) return Path;
	const int32 PersistentLevel = Path.Find(TEXT(":PersistentLevel."));
	const int32 LastDot = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (PersistentLevel == INDEX_NONE || LastDot <= PersistentLevel
		|| !Path.Mid(LastDot + 1).StartsWith(TEXT("MID_")))
		return Path;
	return TEXT("<LayerInternalMID>") + Path.Mid(PersistentLevel);
}

static FString LODSignature(const UStaticMesh* Mesh)
{
	const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
	if (!RenderData) return TEXT("lods=<missing>");
	FString Result = FString::Printf(TEXT("lods=%d"), RenderData->LODResources.Num());
	for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); ++LODIndex)
	{
		const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
		Result += FString::Printf(TEXT("|lod%d_vertices=%u|lod%d_indices=%d|lod%d_sections=%d"),
			LODIndex, LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices(), LODIndex,
			LOD.IndexBuffer.GetArrayView().Num(), LODIndex, LOD.Sections.Num());
		for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
		{
			const FStaticMeshSection& Section = LOD.Sections[SectionIndex];
			Result += FString::Printf(TEXT("|lod%d_section%d_material=%d|lod%d_section%d_triangles=%d|lod%d_section%d_first=%d|lod%d_section%d_min=%d|lod%d_section%d_max=%d"),
				LODIndex, SectionIndex, Section.MaterialIndex, LODIndex, SectionIndex, Section.NumTriangles,
				LODIndex, SectionIndex, Section.FirstIndex, LODIndex, SectionIndex, Section.MinVertexIndex,
				LODIndex, SectionIndex, Section.MaxVertexIndex);
		}
	}
	return Result;
}

static FString SplineShapeKey(const USplineMeshComponent& Spline)
{
	return FString::Printf(
		TEXT("start=%s|start_tangent=%s|start_scale=%s|start_roll=%.17g|start_offset=%s|end=%s|end_tangent=%s|end_scale=%s|end_roll=%.17g|end_offset=%s|up=%s|boundary_min=%.17g|boundary_max=%.17g|axis=%d|smooth=%d"),
		*VectorKey(Spline.GetStartPosition()), *VectorKey(Spline.GetStartTangent()), *Vector2Key(Spline.GetStartScale()),
		Spline.GetStartRoll(), *Vector2Key(Spline.GetStartOffset()), *VectorKey(Spline.GetEndPosition()),
		*VectorKey(Spline.GetEndTangent()), *Vector2Key(Spline.GetEndScale()), Spline.GetEndRoll(),
		*Vector2Key(Spline.GetEndOffset()), *VectorKey(Spline.GetSplineUpDir()), Spline.GetBoundaryMin(),
		Spline.GetBoundaryMax(), int32(Spline.GetForwardAxis()), int32(Spline.bSmoothInterpRollScale));
}

static bool Geometry(UWorld* World, FIMRuralGeometry& Out, FString& Error)
{
	Out = {};
	if (!World || !World->PersistentLevel)
	{
		Error = TEXT("No persistent level is available for geometry identity.");
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<UStaticMeshComponent*> Meshes;
		It->GetComponents(Meshes);
		for (UStaticMeshComponent* Component : Meshes)
		{
			if (!IsValid(Component) || Component->HasAnyFlags(RF_Transient)) continue;
#if WITH_EDITORONLY_DATA
			if (Component->IsEditorOnly()) continue;
#endif
			UStaticMesh* Mesh = Component->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				Error = TEXT("Cannot verify LOD0 geometry: ") + Component->GetPathName();
				return false;
			}
			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const uint32 Vertices = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
			const auto Indices = LOD.IndexBuffer.GetArrayView();
			if (!Vertices || !Indices.Num())
			{
				Error = TEXT("Empty or unavailable CPU LOD0: ") + Component->GetPathName();
				return false;
			}
			FSHA1 MeshHash;
			for (uint32 Index = 0; Index < Vertices; ++Index)
			{
				const FVector3f Position = LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Index);
				MeshHash.Update(reinterpret_cast<const uint8*>(&Position), sizeof(Position));
			}
			for (int32 Index = 0; Index < Indices.Num(); ++Index)
			{
				const uint32 Value = Indices[Index];
				if (Value >= Vertices)
				{
					Error = TEXT("Invalid LOD0 index in: ") + Component->GetPathName();
					return false;
				}
				MeshHash.Update(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
			}
			MeshHash.Final();
			uint8 Digest[20];
			MeshHash.GetHash(Digest);
			const FString RelativePath = Component->GetPathName(World->PersistentLevel);
			const FString ComponentClass = Component->GetClass()->GetPathName();
			const FString MeshPath = Mesh->GetPathName();
			const FString ComponentTransformKey = TransformKey(Component->GetComponentTransform());
			const FString ComponentLODKey = LODSignature(Mesh)
				+ TEXT("|lod0_digest=") + BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
			const FString MobilityCollisionKey = FString::Printf(TEXT("mobility=%d|collision=%d|profile=%s"),
				int32(Component->Mobility), int32(Component->GetCollisionEnabled()),
				*Component->GetCollisionProfileName().ToString());
			FString CanonicalMaterialKey;
			FString Row = RelativePath + TEXT("|") + ComponentClass + TEXT("|") + MeshPath + TEXT("|")
				+ ComponentTransformKey + TEXT("|") + BytesToHex(Digest, UE_ARRAY_COUNT(Digest))
				+ FString::Printf(TEXT("|mobility=%d|collision=%d|vertices=%u|indices=%d"),
					int32(Component->Mobility), int32(Component->GetCollisionEnabled()), Vertices, Indices.Num());
			for (int32 MaterialIndex = 0; MaterialIndex < Component->GetNumMaterials(); ++MaterialIndex)
			{
				UMaterialInterface* Material = Component->GetMaterial(MaterialIndex);
				const FString RawPath = GetPathNameSafe(Material);
				const FString CanonicalPath = CanonicalMaterialPath(Material);
				Row += TEXT("|material=") + RawPath;
				CanonicalMaterialKey += TEXT("|material=") + CanonicalPath;
			}
			Row += TEXT("|") + ComponentLODKey;
			FString InstanceKey;
			if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Component))
			{
				Row += FString::Printf(TEXT("|instances=%d"), ISM->GetInstanceCount());
				InstanceKey = FString::Printf(TEXT("instances=%d"), ISM->GetInstanceCount());
				for (int32 InstanceIndex = 0; InstanceIndex < ISM->GetInstanceCount(); ++InstanceIndex)
				{
					FTransform InstanceTransform;
					if (!ISM->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
					{
						Error = TEXT("Cannot read instance transform: ") + Component->GetPathName();
						return false;
					}
					const FString InstanceKeyPart = TransformKey(InstanceTransform);
					Row += TEXT("|") + InstanceKeyPart;
					InstanceKey += TEXT("|") + InstanceKeyPart;
				}
			}
			Out.Rows.Add(Row);
			FIMRuralGeometryComponent Identity;
			Identity.RelativePath = RelativePath;
			Identity.ComponentClass = ComponentClass;
			Identity.MeshPath = MeshPath;
			Identity.LODKey = ComponentLODKey;
			Identity.MaterialKey = CanonicalMaterialKey;
			Identity.MobilityCollisionKey = MobilityCollisionKey;
			Identity.TransformKey = ComponentTransformKey;
			Identity.InstanceKey = InstanceKey;
			Identity.WorldLocation = Component->GetComponentLocation();
			Identity.bGeneratedSpline = Component->IsA<USplineMeshComponent>();
			if (USplineMeshComponent* Spline = Cast<USplineMeshComponent>(Component))
			{
				// Preserve the deformation fields in failure evidence so a copy mismatch
				// can be distinguished from component renaming or transform roundoff.
				Row += TEXT("|spline_shape=") + SplineShapeKey(*Spline);
				Out.Rows.Last() = Row;
				const auto AddVector = [](TArray<double>& Values, const FVector& Value)
				{
					Values.Add(Value.X);
					Values.Add(Value.Y);
					Values.Add(Value.Z);
				};
				const auto AddVector2 = [](TArray<double>& Values, const FVector2D& Value)
				{
					Values.Add(Value.X);
					Values.Add(Value.Y);
				};
				AddVector(Identity.SplineShapeCm, Spline->GetStartPosition());
				AddVector(Identity.SplineShapeCm, Spline->GetStartTangent());
				AddVector2(Identity.SplineShapeCm, Spline->GetStartOffset());
				AddVector(Identity.SplineShapeCm, Spline->GetEndPosition());
				AddVector(Identity.SplineShapeCm, Spline->GetEndTangent());
				AddVector2(Identity.SplineShapeCm, Spline->GetEndOffset());
				Identity.SplineShapeCm.Add(Spline->GetBoundaryMin());
				Identity.SplineShapeCm.Add(Spline->GetBoundaryMax());
				AddVector2(Identity.SplineShapeUnitless, Spline->GetStartScale());
				Identity.SplineShapeUnitless.Add(Spline->GetStartRoll());
				AddVector2(Identity.SplineShapeUnitless, Spline->GetEndScale());
				Identity.SplineShapeUnitless.Add(Spline->GetEndRoll());
				AddVector(Identity.SplineShapeUnitless, Spline->GetSplineUpDir());
				Identity.SplineKey = ComponentClass + TEXT("|") + MeshPath + TEXT("|") + ComponentLODKey
					+ CanonicalMaterialKey + TEXT("|") + MobilityCollisionKey + TEXT("|")
					+ RotationScaleKey(Component->GetComponentTransform())
					+ FString::Printf(TEXT("|axis=%d|smooth=%d|"),
						int32(Spline->GetForwardAxis()), int32(Spline->bSmoothInterpRollScale))
					+ InstanceKey;
			}
			Identity.StrictKey = RelativePath + TEXT("|") + ComponentClass + TEXT("|") + MeshPath + TEXT("|")
				+ ComponentLODKey + CanonicalMaterialKey + TEXT("|") + MobilityCollisionKey + TEXT("|")
				+ ComponentTransformKey + TEXT("|") + InstanceKey;
			Out.Components.Add(MoveTemp(Identity));
			++Out.StaticMeshComponents;
			if (Out.Components.Last().bGeneratedSpline) ++Out.GeneratedSplineComponents;
		}

		TArray<ULandscapeComponent*> Landscapes;
		It->GetComponents(Landscapes);
		for (ULandscapeComponent* Landscape : Landscapes)
		{
			if (!IsValid(Landscape) || Landscape->HasAnyFlags(RF_Transient)) continue;
			FString Row = Landscape->GetPathName(World->PersistentLevel) + TEXT("|")
				+ Landscape->GetClass()->GetPathName() + TEXT("|")
				+ TransformKey(Landscape->GetComponentTransform())
				+ FString::Printf(TEXT("|size_quads=%d|materials=%d"), Landscape->ComponentSizeQuads, Landscape->GetNumMaterials());
			for (int32 MaterialIndex = 0; MaterialIndex < Landscape->GetNumMaterials(); ++MaterialIndex)
				Row += TEXT("|material=") + GetPathNameSafe(Landscape->GetMaterial(MaterialIndex));
			if (ULandscapeHeightfieldCollisionComponent* Collision = Landscape->GetCollisionComponent())
				Row += FString::Printf(TEXT("|collision=%s|collision_scale=%.9g|collision_quads=%d"),
					*Collision->GetPathName(World->PersistentLevel), Collision->CollisionScale, Collision->CollisionSizeQuads);
			Out.Rows.Add(Row);
			FIMRuralGeometryComponent Identity;
			Identity.RelativePath = Landscape->GetPathName(World->PersistentLevel);
			Identity.ComponentClass = Landscape->GetClass()->GetPathName();
			Identity.MeshPath = TEXT("<Landscape>");
			Identity.LODKey = FString::Printf(TEXT("size_quads=%d|materials=%d"),
				Landscape->ComponentSizeQuads, Landscape->GetNumMaterials());
			for (int32 MaterialIndex = 0; MaterialIndex < Landscape->GetNumMaterials(); ++MaterialIndex)
				Identity.MaterialKey += TEXT("|material=") + CanonicalMaterialPath(Landscape->GetMaterial(MaterialIndex));
			Identity.MobilityCollisionKey = FString::Printf(TEXT("mobility=%d|collision=%d|profile=%s"),
				int32(Landscape->Mobility), int32(Landscape->GetCollisionEnabled()),
				*Landscape->GetCollisionProfileName().ToString());
			Identity.TransformKey = TransformKey(Landscape->GetComponentTransform());
			Identity.WorldLocation = Landscape->GetComponentLocation();
			if (ULandscapeHeightfieldCollisionComponent* Collision = Landscape->GetCollisionComponent())
				Identity.LODKey += FString::Printf(TEXT("|collision_scale=%.17g|collision_quads=%d"),
					Collision->CollisionScale, Collision->CollisionSizeQuads);
			Identity.StrictKey = Identity.RelativePath + TEXT("|") + Identity.ComponentClass + TEXT("|")
				+ Identity.MeshPath + TEXT("|") + Identity.LODKey + Identity.MaterialKey + TEXT("|")
				+ Identity.MobilityCollisionKey + TEXT("|") + Identity.TransformKey;
			Out.Components.Add(MoveTemp(Identity));
			++Out.LandscapeComponents;
		}
	}

	Out.Rows.Sort();
	FSHA1 Hash;
	for (const FString& Row : Out.Rows)
	{
		FTCHARToUTF8 Bytes(*Row);
		const uint32 Length = Bytes.Length();
		Hash.Update(reinterpret_cast<const uint8*>(&Length), sizeof(Length));
		Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Length);
	}
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	Out.Hash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	if (Out.StaticMeshComponents <= 0 && Out.LandscapeComponents <= 0)
	{
		Error = TEXT("Rural map contains no verifiable static mesh or Landscape geometry.");
		return false;
	}
	return true;
}

static bool CompareGeometry(const FIMRuralGeometry& Source, const FIMRuralGeometry& Copy, FString& Error)
{
	constexpr double SplinePositionToleranceCm = 0.01;
	// Landscape spline components are regenerated during level duplication.
	// The observed regeneration drift is at most 0.01063 cm in a tangent
	// component; retain a 0.02 cm bound on deformation fields while requiring
	// a unique partner and strict mesh, material, LOD, axis and collision data.
	constexpr double SplineShapeToleranceCm = 0.02;
	constexpr double SplineUnitlessTolerance = 1e-6;
	const double ToleranceSquared = SplinePositionToleranceCm * SplinePositionToleranceCm;
	const auto ShapeEquivalent = [](const FIMRuralGeometryComponent& A, const FIMRuralGeometryComponent& B)
	{
		if (A.SplineShapeCm.Num() != B.SplineShapeCm.Num()
			|| A.SplineShapeUnitless.Num() != B.SplineShapeUnitless.Num()) return false;
		for (int32 Index = 0; Index < A.SplineShapeCm.Num(); ++Index)
			if (!FMath::IsNearlyEqual(A.SplineShapeCm[Index], B.SplineShapeCm[Index], SplineShapeToleranceCm)) return false;
		for (int32 Index = 0; Index < A.SplineShapeUnitless.Num(); ++Index)
			if (!FMath::IsNearlyEqual(A.SplineShapeUnitless[Index], B.SplineShapeUnitless[Index], SplineUnitlessTolerance)) return false;
		return true;
	};
	if (Source.Components.Num() != Copy.Components.Num())
	{
		Error = FString::Printf(TEXT("Geometry comparator: total component count differs source=%d copy=%d."),
			Source.Components.Num(), Copy.Components.Num());
		return false;
	}

	TMap<FString, int32> SourceStrictCounts;
	TMap<FString, int32> CopyStrictCounts;
	TMap<FString, FString> SourceStrictPaths;
	TMap<FString, FString> CopyStrictPaths;
	TArray<const FIMRuralGeometryComponent*> SourceSplines;
	TArray<const FIMRuralGeometryComponent*> CopySplines;
	for (const FIMRuralGeometryComponent& Component : Source.Components)
	{
		if (Component.bGeneratedSpline) SourceSplines.Add(&Component);
		else
		{
			++SourceStrictCounts.FindOrAdd(Component.StrictKey);
			SourceStrictPaths.FindOrAdd(Component.StrictKey, Component.RelativePath);
		}
	}
	for (const FIMRuralGeometryComponent& Component : Copy.Components)
	{
		if (Component.bGeneratedSpline) CopySplines.Add(&Component);
		else
		{
			++CopyStrictCounts.FindOrAdd(Component.StrictKey);
			CopyStrictPaths.FindOrAdd(Component.StrictKey, Component.RelativePath);
		}
	}

	TSet<FString> StrictKeys;
	for (const TPair<FString, int32>& Pair : SourceStrictCounts) StrictKeys.Add(Pair.Key);
	for (const TPair<FString, int32>& Pair : CopyStrictCounts) StrictKeys.Add(Pair.Key);
	for (const FString& Key : StrictKeys)
	{
		const int32 SourceCount = SourceStrictCounts.FindRef(Key);
		const int32 CopyCount = CopyStrictCounts.FindRef(Key);
		if (SourceCount != CopyCount)
		{
			const FString SourcePath = SourceStrictPaths.FindRef(Key);
			const FString CopyPath = CopyStrictPaths.FindRef(Key);
			Error = FString::Printf(TEXT("Geometry comparator: non-generated component strict mismatch source_path=%s copy_path=%s source_count=%d copy_count=%d key=%s. Only layer-internal MID package prefixes are normalized."),
				*SourcePath, *CopyPath, SourceCount, CopyCount, *Key.Left(768));
			return false;
		}
	}

	if (SourceSplines.Num() != CopySplines.Num())
	{
		Error = FString::Printf(TEXT("Geometry comparator: generated SplineMeshComponent count differs source=%d copy=%d."),
			SourceSplines.Num(), CopySplines.Num());
		return false;
	}

	TMap<FString, TArray<int32>> SourceGroups;
	TMap<FString, TArray<int32>> CopyGroups;
	for (int32 Index = 0; Index < SourceSplines.Num(); ++Index)
		SourceGroups.FindOrAdd(SourceSplines[Index]->SplineKey).Add(Index);
	for (int32 Index = 0; Index < CopySplines.Num(); ++Index)
		CopyGroups.FindOrAdd(CopySplines[Index]->SplineKey).Add(Index);

	TSet<FString> SplineKeys;
	for (const TPair<FString, TArray<int32>>& Pair : SourceGroups) SplineKeys.Add(Pair.Key);
	for (const TPair<FString, TArray<int32>>& Pair : CopyGroups) SplineKeys.Add(Pair.Key);
	for (const FString& SplineKey : SplineKeys)
	{
		const TArray<int32>* SourceGroup = SourceGroups.Find(SplineKey);
		const TArray<int32>* CopyGroup = CopyGroups.Find(SplineKey);
		const int32 SourceCount = SourceGroup ? SourceGroup->Num() : 0;
		const int32 CopyCount = CopyGroup ? CopyGroup->Num() : 0;
		if (SourceCount != CopyCount)
		{
			const FString SourcePath = SourceCount ? SourceSplines[(*SourceGroup)[0]]->RelativePath : FString();
			const FString CopyPath = CopyCount ? CopySplines[(*CopyGroup)[0]]->RelativePath : FString();
			Error = FString::Printf(TEXT("Geometry comparator: generated spline mesh/LOD/material/rotation/scale/axis/collision group differs source_path=%s copy_path=%s source_count=%d copy_count=%d key=%s."),
				*SourcePath, *CopyPath, SourceCount, CopyCount, *SplineKey.Left(768));
			return false;
		}
		if (SourceCount == 0) continue;

		struct FIMSplineEdge
		{
			int32 CopyIndex = INDEX_NONE;
			double DistanceSquared = 0.0;
		};
		TArray<TArray<FIMSplineEdge>> Edges;
		Edges.SetNum(SourceCount);
		for (int32 LocalSource = 0; LocalSource < SourceCount; ++LocalSource)
		{
			const FVector SourceLocation = SourceSplines[(*SourceGroup)[LocalSource]]->WorldLocation;
			for (int32 LocalCopy = 0; LocalCopy < CopyCount; ++LocalCopy)
			{
				const double DistanceSquared = FVector::DistSquared(
					SourceLocation, CopySplines[(*CopyGroup)[LocalCopy]]->WorldLocation);
				if (DistanceSquared <= ToleranceSquared
					&& ShapeEquivalent(*SourceSplines[(*SourceGroup)[LocalSource]],
						*CopySplines[(*CopyGroup)[LocalCopy]]))
					Edges[LocalSource].Add({LocalCopy, DistanceSquared});
			}
			Edges[LocalSource].Sort([](const FIMSplineEdge& A, const FIMSplineEdge& B)
			{
				return A.DistanceSquared == B.DistanceSquared
					? A.CopyIndex < B.CopyIndex : A.DistanceSquared < B.DistanceSquared;
			});
		}

		// Hopcroft-Karp supplies a bounded, deterministic one-to-one pairing;
		// a nearest-neighbor check alone could reuse one overlapping copy.
		TArray<int32> LeftMatch;
		TArray<int32> RightMatch;
		TArray<int32> Level;
		LeftMatch.Init(INDEX_NONE, SourceCount);
		RightMatch.Init(INDEX_NONE, CopyCount);
		Level.Init(INDEX_NONE, SourceCount);
		TFunction<bool()> BuildLayers = [&]()
		{
			TArray<int32> Queue;
			bool bFoundFreeRight = false;
			for (int32 LocalSource = 0; LocalSource < SourceCount; ++LocalSource)
			{
				Level[LocalSource] = LeftMatch[LocalSource] == INDEX_NONE ? 0 : INDEX_NONE;
				if (Level[LocalSource] == 0) Queue.Add(LocalSource);
			}
			for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
			{
				const int32 LocalSource = Queue[QueueIndex];
				for (const FIMSplineEdge& Edge : Edges[LocalSource])
				{
					const int32 MatchedSource = RightMatch[Edge.CopyIndex];
					if (MatchedSource == INDEX_NONE) bFoundFreeRight = true;
					else if (Level[MatchedSource] == INDEX_NONE)
					{
						Level[MatchedSource] = Level[LocalSource] + 1;
						Queue.Add(MatchedSource);
					}
				}
			}
			return bFoundFreeRight;
		};
		TFunction<bool(int32)> Augment = [&](const int32 LocalSource)
		{
			for (const FIMSplineEdge& Edge : Edges[LocalSource])
			{
				const int32 MatchedSource = RightMatch[Edge.CopyIndex];
				if (MatchedSource == INDEX_NONE
					|| (Level[MatchedSource] == Level[LocalSource] + 1 && Augment(MatchedSource)))
				{
					LeftMatch[LocalSource] = Edge.CopyIndex;
					RightMatch[Edge.CopyIndex] = LocalSource;
					return true;
				}
			}
			Level[LocalSource] = INDEX_NONE;
			return false;
		};
		int32 MatchedCount = 0;
		while (BuildLayers())
		{
			for (int32 LocalSource = 0; LocalSource < SourceCount; ++LocalSource)
				if (LeftMatch[LocalSource] == INDEX_NONE && Augment(LocalSource)) ++MatchedCount;
		}
		if (MatchedCount != SourceCount)
		{
			int32 UnmatchedSource = INDEX_NONE;
			for (int32 LocalSource = 0; LocalSource < SourceCount; ++LocalSource)
				if (LeftMatch[LocalSource] == INDEX_NONE) { UnmatchedSource = LocalSource; break; }
			if (UnmatchedSource == INDEX_NONE) continue;
			const FIMRuralGeometryComponent& SourceComponent = *SourceSplines[(*SourceGroup)[UnmatchedSource]];
			double ClosestDistanceSquared = TNumericLimits<double>::Max();
			FString ClosestPath;
			for (int32 LocalCopy = 0; LocalCopy < CopyCount; ++LocalCopy)
			{
				const double DistanceSquared = FVector::DistSquared(
					SourceComponent.WorldLocation, CopySplines[(*CopyGroup)[LocalCopy]]->WorldLocation);
				if (DistanceSquared < ClosestDistanceSquared)
				{
					ClosestDistanceSquared = DistanceSquared;
					ClosestPath = CopySplines[(*CopyGroup)[LocalCopy]]->RelativePath;
				}
			}
			Error = FString::Printf(TEXT("Geometry comparator: generated spline one-to-one pairing failed source_path=%s closest_copy_path=%s closest_distance_cm=%.9g position_tolerance_cm=%.9g shape_tolerance_cm=%.9g; mesh/LOD/material/rotation/scale/mobility/collision/axis and bounded spline shape fields were required."),
				*SourceComponent.RelativePath, *ClosestPath, FMath::Sqrt(ClosestDistanceSquared), SplinePositionToleranceCm, SplineShapeToleranceCm);
			return false;
		}
	}
	return true;
}

static TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& Value)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Add(MakeShared<FJsonValueNumber>(Value.X));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Y));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Z));
	return Result;
}

static void SetPathArray(const TSharedRef<FJsonObject>& Json, const TCHAR* Field, const TArray<FString>& Paths)
{
	TArray<FString> Sorted = Paths;
	Sorted.Sort();
	const int32 Count = FMath::Min(Sorted.Num(), MaxReportPaths);
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
		Values.Add(MakeShared<FJsonValueString>(Sorted[Index]));
	Json->SetNumberField(FString(Field) + TEXT("_count"), Sorted.Num());
	Json->SetBoolField(FString(Field) + TEXT("_truncated"), Sorted.Num() > Count);
	Json->SetArrayField(Field, Values);
}

static bool FindBakeVolume(UWorld* World, AIMAcousticBakeVolume*& Out, FString& Error)
{
	Out = nullptr;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
	{
		if (Out)
		{
			Error = TEXT("Expected exactly one Rural preview BakeVolume; found multiple.");
			return false;
		}
		Out = *It;
	}
	if (!Out)
	{
		Error = TEXT("Rural preview BakeVolume is missing.");
		return false;
	}
	return true;
}

static void CountAudioAndMarkers(UWorld* World, int32& OutAudio, int32& OutMarkers,
	TArray<FString>& AudioPaths, TArray<FString>& MarkerPaths)
{
	OutAudio = 0;
	OutMarkers = 0;
	AudioPaths.Reset();
	MarkerPaths.Reset();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<UAudioComponent*> Audio;
		It->GetComponents(Audio);
		for (UAudioComponent* Component : Audio)
		{
			++OutAudio;
			AudioPaths.Add(FString::Printf(TEXT("%s|owner=%s|sound=%s|auto_activate=%d|volume=%.9g"),
				*Component->GetPathName(), *GetPathNameSafe(Component->GetOwner()),
				*GetPathNameSafe(Component->Sound), int32(Component->bAutoActivate),
				double(Component->VolumeMultiplier)));
		}
		TArray<UIMAcousticSourceComponent*> Markers;
		It->GetComponents(Markers);
		for (UIMAcousticSourceComponent* Marker : Markers)
		{
			++OutMarkers;
			MarkerPaths.Add(Marker->GetPathName());
		}
	}
}

// The source map contains one autoplay ambient bed. Preserve it on the source
// map and explicitly mute only its duplicated component for isolated water-drop listening.
static UAudioComponent* FindOriginalBackgroundAudio(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if ((*It)->GetFName() != FName(TEXT("Australia_01_2"))) continue;
		TArray<UAudioComponent*> Audio;
		It->GetComponents(Audio);
		for (UAudioComponent* Component : Audio)
			if (IsValid(Component) && Component->GetFName() == FName(TEXT("AudioComponent0"))
				&& GetPathNameSafe(Component->Sound) == OriginalBackgroundSound)
				return Component;
	}
	return nullptr;
}

static bool IsBackgroundMutedInPreview(UWorld* World)
{
	UAudioComponent* Background = FindOriginalBackgroundAudio(World);
	return Background && Background->ComponentHasTag(FName(MutedBackgroundTag))
		&& !Background->bAutoActivate && FMath::IsNearlyZero(Background->VolumeMultiplier);
}

static bool FindCamera3(UWorld* World, AActor*& OutCamera, FString& Error)
{
	OutCamera = nullptr;
	const UClass* CameraClass = FindObject<UClass>(nullptr, RuralCameraClass);
	int32 Matches = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (CameraClass && (*It)->GetFName() == FName(RuralCameraName) && (*It)->IsA(CameraClass))
		{
			OutCamera = *It;
			++Matches;
		}
	}
	if (Matches != 1 || !IsValid(OutCamera))
	{
		Error = TEXT("Expected exactly one CineCameraActor named Camera3.");
		return false;
	}
	const FVector Location = OutCamera->GetActorLocation();
	if (FMath::Abs(Location.X - (-6597.06298828125)) > 2.0
		|| FMath::Abs(Location.Y - 14720.8662109375) > 2.0)
	{
		Error = FString::Printf(TEXT("Camera3 XY differs from the Rural 10 m benchmark center: %s."), *Location.ToString());
		return false;
	}
	return true;
}

static bool FindGround(UWorld* World, const double CenterX, const double CenterY,
	double& OutGroundMin, double& OutGroundMax, FString& Error)
{
	OutGroundMin = TNumericLimits<double>::Max();
	OutGroundMax = TNumericLimits<double>::Lowest();
	const double MinX = CenterX - RegionHalfExtentCm;
	const double MaxX = CenterX + RegionHalfExtentCm;
	const double MinY = CenterY - RegionHalfExtentCm;
	const double MaxY = CenterY + RegionHalfExtentCm;
	int32 TileCount = 0;

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		TArray<UActorComponent*> Components;
		ActorIt->GetComponents(Components);
		for (UActorComponent* ActorComponent : Components)
		{
			ULandscapeHeightfieldCollisionComponent* Collision = Cast<ULandscapeHeightfieldCollisionComponent>(ActorComponent);
			if (!Collision || !Collision->Bounds.GetBox().IsValid
				|| Collision->Bounds.GetBox().Max.X < MinX || Collision->Bounds.GetBox().Min.X > MaxX
				|| Collision->Bounds.GetBox().Max.Y < MinY || Collision->Bounds.GetBox().Min.Y > MaxY)
				continue;
			ULandscapeComponent* Render = Collision->GetRenderComponent();
			const int32 Quads = Collision->CollisionSizeQuads;
			const int32 Side = Quads + 1;
			const int64 SampleCount = int64(Side) * Side;
			const FTransform Transform = Collision->GetComponentTransform();
			if (!IsValid(Render) || Quads <= 0 || SampleCount > MAX_int32
				|| !FMath::IsNearlyEqual(Collision->CollisionScale, 1.0f)
				|| Render->ComponentSizeQuads != Quads || Render->GetNumMaterials() != 1
				|| Collision->CookedPhysicalMaterials.IsEmpty() || !Collision->CookedPhysicalMaterials[0]
				|| !Transform.TransformVectorNoScale(FVector::UpVector).Equals(FVector::UpVector, 1.e-4))
			{
				Error = TEXT("Camera3 Landscape collision/render pair is unsupported or incomplete: ") + Collision->GetPathName();
				return false;
			}
			TArray<float> Heights;
			Heights.SetNumUninitialized(int32(SampleCount));
			if (!Collision->FillHeightTile(MakeArrayView(Heights), 0, Side))
			{
				Error = TEXT("Landscape collision height readback failed: ") + Collision->GetPathName();
				return false;
			}
			for (int32 Y = 0; Y < Side; ++Y)
			{
				for (int32 X = 0; X < Side; ++X)
				{
					const float Height = Heights[Y * Side + X];
					if (!FMath::IsFinite(Height))
					{
						Error = TEXT("Landscape collision contains a non-finite height: ") + Collision->GetPathName();
						return false;
					}
					const FVector XY = Transform.TransformPosition(FVector(X * Collision->CollisionScale, Y * Collision->CollisionScale, 0.0));
					if (XY.X >= MinX && XY.X <= MaxX && XY.Y >= MinY && XY.Y <= MaxY)
					{
						OutGroundMin = FMath::Min(OutGroundMin, double(Height));
						OutGroundMax = FMath::Max(OutGroundMax, double(Height));
					}
				}
			}
			++TileCount;
		}
	}
	if (TileCount == 0 || OutGroundMin > OutGroundMax)
	{
		Error = TEXT("No paired Landscape collision samples cover the Camera3 10 m XY region.");
		return false;
	}
	return true;
}

static bool PrepareBackdrops(UWorld* World, const FBox& Region, TArray<FString>& OutPaths, FString& Error)
{
	TMap<FString, int32> Counts;
	Counts.Add(SkyMeshPath, 0);
	Counts.Add(HorizonMeshPath, 0);
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		TArray<UStaticMeshComponent*> Meshes;
		ActorIt->GetComponents(Meshes);
		for (UStaticMeshComponent* Component : Meshes)
		{
			if (!IsValid(Component) || !Region.Intersect(Component->Bounds.GetBox())) continue;
			const FString MeshPath = GetPathNameSafe(Component->GetStaticMesh());
			if (!Counts.Contains(MeshPath)) continue;
			if (Component->Mobility != EComponentMobility::Static || Component->ComponentHasTag(FName(TEXT("IMAcousticRequired"))))
			{
				Error = TEXT("Exact Rural backdrop is not a non-required StaticMeshComponent: ") + Component->GetPathName();
				return false;
			}
			++Counts.FindOrAdd(MeshPath);
			Component->Modify();
			Component->ComponentTags.AddUnique(FName(IgnoreTag));
			Component->ComponentTags.AddUnique(FName(ProvisionalBackdropTag));
			OutPaths.Add(Component->GetPathName());
		}
	}
	for (const TPair<FString, int32>& Pair : Counts)
	{
		if (Pair.Value != 1)
		{
			Error = FString::Printf(TEXT("Expected exactly one in-region provisional backdrop for %s; found %d."), *Pair.Key, Pair.Value);
			return false;
		}
	}
	return true;
}

static bool PrepareBlockingVolumes(UWorld* World, const FBox& Region,
	TArray<FString>& OutPaths, FString& Error)
{
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		if ((*ActorIt)->GetClass()->GetPathName() != BlockingVolumeClass) continue;
		TArray<UPrimitiveComponent*> Primitives;
		(*ActorIt)->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (!IsValid(Primitive) || !Region.Intersect(Primitive->Bounds.GetBox())) continue;
			if (Primitive->ComponentHasTag(FName(TEXT("IMAcousticRequired"))))
			{
				Error = TEXT("An in-region BlockingVolume is explicitly required and cannot be provisionally excluded: ")
					+ Primitive->GetPathName();
				return false;
			}
			(*ActorIt)->Modify();
			(*ActorIt)->Tags.AddUnique(FName(ProvisionalBlockingTag));
			Primitive->Modify();
			Primitive->ComponentTags.AddUnique(FName(IgnoreTag));
			Primitive->ComponentTags.AddUnique(FName(ProvisionalBlockingTag));
			OutPaths.Add(Primitive->GetPathName());
		}
	}
	return true;
}

static bool IsEditorOnlyPrimitive(const UPrimitiveComponent* Primitive)
{
#if WITH_EDITORONLY_DATA
	return Primitive && (Primitive->IsVisualizationComponent() || Primitive->IsEditorOnly());
#else
	return false;
#endif
}

static bool CollectMaterialsAndExclusions(UWorld* World, const FBox& Region,
	TSet<UMaterialInterface*>& OutMaterials, TArray<FString>& DynamicPaths,
	TArray<FString>& KnownExcludedPaths, TArray<FString>& UnsupportedPaths, FString& Error)
{
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		TArray<UPrimitiveComponent*> Primitives;
		Actor->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (!IsValid(Primitive) || !Region.Intersect(Primitive->Bounds.GetBox())) continue;
			if (Actor == World->GetDefaultBrush() || Actor == World->GetDefaultPhysicsVolume() || IsEditorOnlyPrimitive(Primitive))
			{
				KnownExcludedPaths.Add(Primitive->GetPathName());
				continue;
			}
			if (Primitive->ComponentHasTag(FName(IgnoreTag)))
			{
				const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive);
				const FString MeshPath = Mesh ? GetPathNameSafe(Mesh->GetStaticMesh()) : FString();
				if (MeshPath == SkyMeshPath || MeshPath == HorizonMeshPath
					|| Primitive->ComponentHasTag(FName(ProvisionalBlockingTag)))
				{
					KnownExcludedPaths.Add(Primitive->GetPathName());
					continue;
				}
				Error = TEXT("An unexpected pre-existing or unclassified IMAcousticIgnore omits an in-region primitive: ")
					+ Primitive->GetPathName();
				return false;
			}
			if (Primitive->Mobility != EComponentMobility::Static)
			{
				if (Primitive->ComponentHasTag(FName(TEXT("IMAcousticRequired"))))
				{
					Error = TEXT("An in-region required non-static primitive cannot enter the static bake: ")
						+ Primitive->GetPathName();
					return false;
				}
				DynamicPaths.Add(Primitive->GetPathName());
				continue;
			}
			if (ULandscapeHeightfieldCollisionComponent* Collision = Cast<ULandscapeHeightfieldCollisionComponent>(Primitive))
			{
				if (!IsValid(Collision->GetRenderComponent()) || Collision->GetRenderComponent()->GetCollisionComponent() != Collision)
				{
					Error = TEXT("Landscape collision lacks its paired render component: ") + Collision->GetPathName();
					return false;
				}
				KnownExcludedPaths.Add(Collision->GetPathName());
				continue;
			}
			if (ULandscapeComponent* Landscape = Cast<ULandscapeComponent>(Primitive))
			{
				if (!IsValid(Landscape->GetCollisionComponent()) || !Region.Intersect(Landscape->GetCollisionComponent()->Bounds.GetBox()))
				{
					Error = TEXT("Landscape render surface has no paired collision inside the bake region: ") + Landscape->GetPathName();
					return false;
				}
				if (Landscape->GetNumMaterials() != 1 || !IsValid(Landscape->GetMaterial(0)))
				{
					Error = TEXT("Landscape render surface has no explicit effective material: ") + Landscape->GetPathName();
					return false;
				}
				OutMaterials.Add(Landscape->GetMaterial(0));
				continue;
			}
			if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive))
			{
				if (!IsValid(Mesh->GetStaticMesh()) || Mesh->GetNumMaterials() <= 0)
				{
					Error = TEXT("In-region static mesh has no explicit material slots: ") + Mesh->GetPathName();
					return false;
				}
				for (int32 MaterialIndex = 0; MaterialIndex < Mesh->GetNumMaterials(); ++MaterialIndex)
				{
					UMaterialInterface* Material = Mesh->GetMaterial(MaterialIndex);
					if (!IsValid(Material))
					{
						Error = TEXT("In-region static mesh has a null material slot: ") + Mesh->GetPathName();
						return false;
					}
					OutMaterials.Add(Material);
				}
				continue;
			}
			const FString ComponentClass = Primitive->GetClass()->GetPathName();
			const FString ActorClass = Actor->GetClass()->GetPathName();
			const bool bKnownHelper = ComponentClass == LandscapeSplineClass
				|| (ComponentClass == BrushComponentClass
					&& (ActorClass == LightmassImportanceClass || ActorClass == BlockingVolumeClass));
			if (bKnownHelper)
			{
				KnownExcludedPaths.Add(Primitive->GetPathName());
				continue;
			}
			UnsupportedPaths.Add(Primitive->GetPathName());
			Error = TEXT("An unsupported static non-mesh primitive overlaps the Rural bake region: ")
				+ Primitive->GetPathName();
			return false;
		}
	}
	if (OutMaterials.IsEmpty())
	{
		Error = TEXT("No explicit provisional material mappings were discovered in the Rural bake region.");
		return false;
	}
	return true;
}

static bool FindLandscapePlayerStart(UWorld* World, const FBox& Region, const FVector& CameraLocation,
	FVector& OutLocation, FString& Error)
{
	// An editor-world LineTraceSingleByChannel returned no WorldStatic hits for
	// any Camera3 candidate, although the audited Landscape collision tiles are
	// present. Use their actual height data to author the spawn position; PIE
	// collision and occupancy still need validation before audition acceptance.
	const FVector Offsets[] = {
		FVector::ZeroVector, FVector(250, 0, 0), FVector(-250, 0, 0),
		FVector(0, 250, 0), FVector(0, -250, 0), FVector(250, 250, 0), FVector(-250, -250, 0)
	};
	for (const FVector& Offset : Offsets)
	{
		const FVector XY = CameraLocation + Offset;
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			TArray<ULandscapeHeightfieldCollisionComponent*> Tiles;
			ActorIt->GetComponents(Tiles);
			for (ULandscapeHeightfieldCollisionComponent* Tile : Tiles)
			{
				if (!IsValid(Tile) || Tile->CollisionSizeQuads < 1 || Tile->CollisionScale <= 0.0f) continue;
				const FTransform Transform = Tile->GetComponentTransform();
				const FVector Local = Transform.InverseTransformPosition(FVector(XY.X, XY.Y, 0.0));
				const double GridX = Local.X / Tile->CollisionScale;
				const double GridY = Local.Y / Tile->CollisionScale;
				if (GridX < 0.0 || GridY < 0.0
					|| GridX >= Tile->CollisionSizeQuads || GridY >= Tile->CollisionSizeQuads) continue;
				const int32 Side = Tile->CollisionSizeQuads + 1;
				TArray<float> Heights;
				Heights.SetNumUninitialized(Side * Side);
				if (!Tile->FillHeightTile(MakeArrayView(Heights), 0, Side))
				{
					Error = TEXT("Cannot sample Landscape collision tile for PlayerStart: ") + Tile->GetPathName();
					return false;
				}
				const int32 X0 = FMath::FloorToInt(GridX);
				const int32 Y0 = FMath::FloorToInt(GridY);
				const double FX = GridX - X0;
				const double FY = GridY - Y0;
				const double H00 = Heights[Y0 * Side + X0];
				const double H10 = Heights[Y0 * Side + X0 + 1];
				const double H01 = Heights[(Y0 + 1) * Side + X0];
				const double H11 = Heights[(Y0 + 1) * Side + X0 + 1];
				const double Height = FMath::Lerp(FMath::Lerp(H00, H10, FX), FMath::Lerp(H01, H11, FX), FY);
				const double CellX = Transform.TransformVector(FVector(Tile->CollisionScale, 0.0, 0.0)).Size();
				const double CellY = Transform.TransformVector(FVector(0.0, Tile->CollisionScale, 0.0)).Size();
				const double GradientX = FMath::Lerp(H10 - H00, H11 - H01, FY) / CellX;
				const double GradientY = FMath::Lerp(H01 - H00, H11 - H10, FX) / CellY;
				const double NormalZ = 1.0 / FMath::Sqrt(1.0 + GradientX * GradientX + GradientY * GradientY);
				const FVector Candidate(XY.X, XY.Y, Height + 100.0);
				if (!FMath::IsFinite(Height) || NormalZ < 0.5 || !Region.IsInsideOrOn(Candidate)) continue;
				OutLocation = Candidate;
				return true;
			}
		}
	}
	Error = TEXT("No Landscape heightfield PlayerStart candidate with suitable slope was found inside the Camera3 BakeBounds.");
	return false;
}

static bool ChooseSourceLocation(const FBox& Region, const FVector& PlayerLocation,
	FVector& OutLocation, FString& Error)
{
	const FVector Candidates[] = {
		PlayerLocation + FVector(250, 0, 0), PlayerLocation + FVector(-250, 0, 0),
		PlayerLocation + FVector(0, 250, 0), PlayerLocation + FVector(0, -250, 0),
		PlayerLocation + FVector(250, 250, 0), PlayerLocation + FVector(-250, -250, 0)
	};
	for (const FVector& Candidate : Candidates)
	{
		if (Region.IsInsideOrOn(Candidate))
		{
			OutLocation = Candidate;
			return true;
		}
	}
	Error = TEXT("No water-drop source candidate lies inside the Camera3 BakeBounds.");
	return false;
}

class FIMAcousticRuralPreviewPrepareCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticRuralPreviewPrepareCommand(FAutomationTestBase* InTest)
		: Test(InTest), StartedAt(FPlatformTime::Seconds())
	{
	}

	bool Update() override
	{
		if (bFinished) return true;
		if (Stage == 0 && Evidence.IsEmpty())
		{
			Evidence = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralPreviewPrepare"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*Evidence, true);
		}
		if (!GEditor || GEditor->PlayWorld)
			return Finish(false, TEXT("Rural preview preparation requires an idle owned Editor with no PIE world."));
		if (FPlatformTime::Seconds() - StartedAt > MaxSeconds && Stage != 2)
			return Finish(false, TEXT("Rural preview preparation exceeded its bounded 900 second authoring guard."));
		if (Stage == 0) return StartAuthoring();
		if (Stage == 1) return StartBake();
		if (Stage == 2) return PollBake();
		if (Stage == 3) return ReloadAndValidate();
		return Finish(false, TEXT("Unexpected Rural preview preparation stage."));
	}

private:
	bool CompareCopiedGeometry(FString& Error)
	{
		if (CompareGeometry(BaselineGeometry, CopiedGeometry, Error))
		{
			bGeometryEquivalent = true;
			return true;
		}
		const bool bSourceRowsSaved = FFileHelper::SaveStringArrayToFile(BaselineGeometry.Rows,
			*FPaths::Combine(Evidence, TEXT("source-geometry-identity.txt")));
		const bool bCopyRowsSaved = FFileHelper::SaveStringArrayToFile(CopiedGeometry.Rows,
			*FPaths::Combine(Evidence, TEXT("copy-geometry-identity.txt")));
		bGeometryEquivalent = false;
		Error += FString::Printf(TEXT(" Geometry comparator row evidence saved=%d/%d."),
			int32(bSourceRowsSaved), int32(bCopyRowsSaved));
		return false;
	}

	bool StartAuthoring()
	{
		FString Error;
		UWorld* Current = GEditor->GetEditorWorldContext().World();
		if (Current && Current->GetOutermost()->IsDirty())
			return Finish(false, TEXT("Current Editor map has unsaved changes; use a clean owned session."));
		if (!FPackageName::DoesPackageExist(RuralSourceMap, &SourceFile)
			|| !HashFile(SourceFile, SourceSha1, SourceBytes, Error))
			return Finish(false, TEXT("Rural source map file is missing or cannot be hashed: ") + Error);
		TargetFile = FPackageName::LongPackageNameToFilename(RuralTargetMap, FPackageName::GetMapPackageExtension());
		if (FPackageName::DoesPackageExist(RuralTargetMap) || FindPackage(nullptr, RuralTargetMap)
			|| IFileManager::Get().FileExists(*TargetFile))
			return Finish(false, FString(TEXT("Refusing to overwrite an existing Rural preview map package: ")) + RuralTargetMap);
		if (!FEditorFileUtils::LoadMap(RuralSourceMap, false, true))
			return Finish(false, TEXT("Cannot load the exact Rural source map."));
		UWorld* Source = GEditor->GetEditorWorldContext().World();
		if (!Source || Source->GetOutermost()->GetName() != RuralSourceMap || !Source->PersistentLevel)
			return Finish(false, TEXT("Loaded Editor world does not match the exact Rural source package."));
		if (Source->GetWorldPartition() || Source->GetStreamingLevels().Num() != 0
			|| Source->PersistentLevel->IsUsingExternalActors())
			return Finish(false, TEXT("Rural source uses WorldPartition, streaming levels, or external actors; duplication is fail-closed."));
		WorldPartitionState = TEXT("absent");
		StreamingLevelCount = Source->GetStreamingLevels().Num();
		bExternalActors = Source->PersistentLevel->IsUsingExternalActors();
		if (!Geometry(Source, BaselineGeometry, Error)) return Finish(false, Error);
		if (!FindCamera3(Source, Camera, Error)) return Finish(false, Error);
		CameraLocation = Camera->GetActorLocation();
		CameraActorPath = Camera->GetPathName();
		Camera = nullptr;
		CountAudioAndMarkers(Source, SourceAudioCount, SourceMarkerCount, SourceAudioPaths, SourceMarkerPaths);
		UAudioComponent* OriginalBackground = FindOriginalBackgroundAudio(Source);
		if (SourceAudioCount != 1 || SourceMarkerCount != 0 || !OriginalBackground
			|| !OriginalBackground->bAutoActivate)
		{
			return Finish(false, TEXT("Rural source audio differs from its one known autoplay ambient bed; no unknown audio may be changed."));
		}
		int32 ExistingBakeVolumes = 0;
		for (TActorIterator<AIMAcousticBakeVolume> It(Source); It; ++It) ++ExistingBakeVolumes;
		if (ExistingBakeVolumes != 0)
			return Finish(false, TEXT("Rural source already contains a BakeVolume; source binding will not be altered."));
		if (!FindGround(Source, CameraLocation.X, CameraLocation.Y, GroundMin, GroundMax, Error))
			return Finish(false, Error);

		RegionMin = FVector(CameraLocation.X - RegionHalfExtentCm, CameraLocation.Y - RegionHalfExtentCm, GroundMin - TerrainBelowMarginCm);
		RegionMax = FVector(CameraLocation.X + RegionHalfExtentCm, CameraLocation.Y + RegionHalfExtentCm, GroundMax + TerrainAboveMarginCm);
		Region = FBox(RegionMin, RegionMax);
		GroundCenter = Region.GetCenter();
		GroundExtent = Region.GetExtent();
		if (!FindLandscapePlayerStart(Source, Region, CameraLocation, PlayerLocation, Error))
			return Finish(false, Error);

		ObjectTools::FPackageGroupName PackageName;
		PackageName.PackageName = RuralTargetMap;
		PackageName.ObjectName = FPackageName::GetLongPackageAssetName(RuralTargetMap);
		TSet<UPackage*> RefusedPackages;
		UWorld* Copy = Cast<UWorld>(ObjectTools::DuplicateSingleObject(Source, PackageName, RefusedPackages, false));
		if (!Copy || Copy == Source || Copy->GetOutermost()->GetName() != RuralTargetMap)
			return Finish(false, TEXT("Rural source duplication failed or retained source package ownership."));
		if (Source->PersistentLevel->MapBuildData && Copy->PersistentLevel
			&& Copy->PersistentLevel->MapBuildData == Source->PersistentLevel->MapBuildData)
			return Finish(false, TEXT("Rural copy retained source MapBuildData; shared build data cannot be saved."));
		if (!Geometry(Copy, CopiedGeometry, Error))
			return Finish(false, TEXT("Duplicated Rural geometry could not be inspected: ") + Error);
		if (!CompareCopiedGeometry(Error))
			return Finish(false, TEXT("Duplicated Rural geometry equivalence failed before authoring: ") + Error);
		UAudioComponent* CopiedBackground = FindOriginalBackgroundAudio(Copy);
		if (!CopiedBackground || !CopiedBackground->bAutoActivate)
			return Finish(false, TEXT("Copied Rural ambient bed differs before explicit preview muting."));
		CopiedBackground->Modify();
		CopiedBackground->Stop();
		CopiedBackground->bAutoActivate = false;
		CopiedBackground->VolumeMultiplier = 0.0f;
		CopiedBackground->ComponentTags.AddUnique(FName(MutedBackgroundTag));
		if (!FPackageName::DoesPackageExist(RuralTargetMap) && IFileManager::Get().FileExists(*TargetFile))
			return Finish(false, TEXT("Target map appeared concurrently; refusing overwrite: ") + TargetFile);

		const FBox CopyRegion(RegionMin, RegionMax);
		if (!PrepareBackdrops(Copy, CopyRegion, BackdropPaths, Error)) return Finish(false, Error);
		if (!PrepareBlockingVolumes(Copy, CopyRegion, BlockingVolumePaths, Error)) return Finish(false, Error);
		if (!CollectMaterialsAndExclusions(Copy, CopyRegion, Materials, DynamicExclusionPaths,
			KnownExcludedPaths, UnsupportedPaths, Error)) return Finish(false, Error);
		if (!ChooseSourceLocation(CopyRegion, PlayerLocation, SourceLocation, Error)) return Finish(false, Error);
		if (!SnapshotBakeDirectory(Error)) return Finish(false, Error);

		FActorSpawnParameters Spawn;
		Spawn.Name = TEXT("IM_RuralCamera3Preview_10m_BakeVolume");
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Volume = Copy->SpawnActor<AIMAcousticBakeVolume>(GroundCenter, FRotator::ZeroRotator, Spawn);
		if (!IsValid(Volume.Get()) || !IsValid(Volume->BakeBounds))
			return Finish(false, TEXT("Could not create the persistent Rural BakeVolume in the duplicate."));
		Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
		Volume->Modify();
		Volume->SetActorLabel(TEXT("IM_RuralCamera3Preview_10m_BakeVolume_PROVISIONAL_MATERIALS"));
		Volume->BakeBounds->SetBoxExtent(GroundExtent, false);
		Volume->BakeBounds->SetWorldLocation(GroundCenter, false, nullptr, ETeleportType::TeleportPhysics);
		Volume->BakeBounds->UpdateComponentToWorld();
		Volume->BakeBounds->UpdateBounds();
		Volume->ProbeSpacingCm = 100.0f;
		Volume->ProbeHeightCm = 150.0f;
		Volume->BakedField = nullptr;
		Volume->DynamicBlockers.Reset();
		Volume->bEnableV2 = true;
		Volume->bDirectRoute = true;
		Volume->bPathRoute = true;
		Volume->bReverbRoute = true;
		Volume->ReverbWetGain = 0.25f;
		TArray<UMaterialInterface*> SortedMaterials;
		for (UMaterialInterface* Material : Materials) SortedMaterials.Add(Material);
		SortedMaterials.Sort([](const UMaterialInterface& A, const UMaterialInterface& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		for (UMaterialInterface* Material : SortedMaterials)
		{
			if (!IsValid(Material)) return Finish(false, TEXT("A provisional material mapping became invalid before save."));
			FIMAcousticMaterialMapping& Mapping = Volume->Materials.AddDefaulted_GetRef();
			Mapping.Material = Material;
			Mapping.Absorption = FVector(ProvisionalAbsorption, ProvisionalAbsorption, ProvisionalAbsorption);
			Mapping.Scattering = ProvisionalScattering;
			MaterialPaths.Add(Material->GetPathName());
		}

		FActorSpawnParameters StartSpawn;
		StartSpawn.Name = TEXT("IM_RuralCamera3Preview_PlayerStart");
		StartSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APlayerStart* Start = Copy->SpawnActor<APlayerStart>(PlayerLocation, FRotator::ZeroRotator, StartSpawn);
		if (!Start) return Finish(false, TEXT("Could not create the persistent Rural PlayerStart."));
		Start->Modify();
		Start->PlayerStartTag = PlayerStartTag;
		Start->SetActorLabel(TEXT("IM_RuralCamera3Preview_PlayerStart_LANDSCAPE_HEIGHTFIELD"));
		TArray<UPrimitiveComponent*> StartComponents;
		Start->GetComponents(StartComponents);
		for (UPrimitiveComponent* Component : StartComponents)
		{
			Component->Modify();
			Component->ComponentTags.AddUnique(FName(IgnoreTag));
			Component->ComponentTags.AddUnique(FName(TEXT("IMAcousticRuralPreviewPlayerStartHelper")));
		}

		UMetaSoundSource* MetaSound = LoadObject<UMetaSoundSource>(nullptr, WaterDropMetaSound);
		if (!IsValid(MetaSound) || MetaSound->OutputFormat != EMetaSoundOutputAudioFormat::Stereo || MetaSound->NumChannels != 2)
			return Finish(false, TEXT("The existing stereo water-drop MetaSound is missing or changed."));
		FActorSpawnParameters SourceSpawn;
		SourceSpawn.Name = TEXT("IM_RuralCamera3Preview_WaterDropSource");
		SourceSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AAmbientSound* Ambient = Copy->SpawnActor<AAmbientSound>(SourceLocation, FRotator::ZeroRotator, SourceSpawn);
		if (!Ambient) return Finish(false, TEXT("Could not create the persistent Rural water-drop source."));
		Ambient->Modify();
		Ambient->Tags.AddUnique(FName(SourceTag));
		Ambient->SetActorLabel(TEXT("IM_RuralCamera3Preview_WaterDropSource_PROVISIONAL_STEREO"));
		UAudioComponent* Audio = Ambient->GetAudioComponent();
		if (!Audio) return Finish(false, TEXT("Rural water-drop source has no AudioComponent."));
		Audio->bAutoActivate = true;
		Audio->bAutoDestroy = false;
		Audio->bStopWhenOwnerDestroyed = true;
		Audio->bAllowSpatialization = false;
		Audio->bIsUISound = false;
		Audio->bOverrideAttenuation = false;
		Audio->VolumeMultiplier = 0.7f;
		Audio->PitchMultiplier = 1.0f;
		Audio->SetSound(MetaSound);
		UIMAcousticSourceComponent* Marker = NewObject<UIMAcousticSourceComponent>(Ambient, TEXT("IM_RuralAcousticSource"), RF_Transactional);
		if (!Marker) return Finish(false, TEXT("Could not create the persistent Rural managed source component."));
		Marker->AudioComponent = Audio;
		Ambient->AddInstanceComponent(Marker);
		Marker->RegisterComponent();
		if (!Marker->ValidateSource(Error)) return Finish(false, TEXT("Rural water-drop source validation failed: ") + Error);
		Copy->GetWorldSettings()->Modify();
		Copy->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
		if (!Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(PlayerLocation)
			|| !Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(SourceLocation))
			return Finish(false, TEXT("Rural PlayerStart or water-drop source lies outside BakeBounds."));
		if (!Geometry(Copy, CopiedGeometry, Error))
			return Finish(false, TEXT("Rural authoring geometry could not be inspected: ") + Error);
		if (!CompareCopiedGeometry(Error))
			return Finish(false, TEXT("Rural authoring changed captured map geometry: ") + Error);
		if (!FEditorFileUtils::SaveLevel(Copy->PersistentLevel, TargetFile))
			return Finish(false, TEXT("Initial persistent Rural duplicate map save failed."));
		bTargetMapSaved = true;
		Stage = 1;
		return false;
	}

	bool StartBake()
	{
		FString Error;
		if (!FEditorFileUtils::LoadMap(RuralTargetMap, false, true))
			return Finish(false, TEXT("Cannot reload the newly saved Rural duplicate before baking."));
		bTargetMapReloadedBeforeBake = true;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World || World->GetOutermost()->GetName() != RuralTargetMap)
			return Finish(false, TEXT("Pre-bake reload opened the wrong Rural target world."));
		if (!FindBakeVolume(World, VolumePtr, Error)) return Finish(false, Error);
		Volume = VolumePtr;
		if (Volume->BakedField) return Finish(false, TEXT("Rural duplicate retained an unexpected old BakeField binding."));
		int32 AudioCount = 0;
		int32 MarkerCount = 0;
		TArray<FString> AudioPaths;
		TArray<FString> MarkerPaths;
		CountAudioAndMarkers(World, AudioCount, MarkerCount, AudioPaths, MarkerPaths);
		if (AudioCount != 2 || MarkerCount != 1 || !IsBackgroundMutedInPreview(World))
			return Finish(false, TEXT("Rural duplicate must contain one explicitly muted original ambient bed and one managed water-drop source."));
		if (!Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(PlayerLocation)
			|| !Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(SourceLocation))
			return Finish(false, TEXT("Rural source/listener coordinates are outside BakeBounds after reload."));
		if (!Geometry(World, CopiedGeometry, Error))
			return Finish(false, TEXT("Rural pre-bake geometry could not be inspected: ") + Error);
		if (!CompareCopiedGeometry(Error))
			return Finish(false, TEXT("Rural geometry changed before Bake: ") + Error);
		if (Volume->Materials.Num() <= 0 || Volume->DynamicBlockers.Num() != 0)
			return Finish(false, TEXT("Rural BakeVolume material mapping or dynamic-blocker contract is invalid."));
		Volume->GenerateProbes();
		// ShowProbeCoverage changes Status to an overlay description after a
		// successful GenerateProbes call; a fresh loaded volume starts at zero.
		if (Volume->GeneratedProbes <= 0)
			return Finish(false, TEXT("Rural GenerateProbes failed: ") + Volume->Status);
		ProbeCount = Volume->GeneratedProbes;
		Volume->Bake();
		if (!Volume->Status.StartsWith(TEXT("Baking")))
			return Finish(false, TEXT("Rural Bake did not start: ") + Volume->Status);
		Stage = 2;
		return false;
	}

	bool PollBake()
	{
		if (!Volume.IsValid()) return Finish(false, TEXT("Rural BakeVolume was destroyed during the asynchronous bake."));
		const double Elapsed = FPlatformTime::Seconds() - StartedAt;
		if (Elapsed > MaxSeconds)
		{
			if (Volume->Status.StartsWith(TEXT("Baking")) || Volume->Status.StartsWith(TEXT("Cancellation requested")))
			{
				if (!Volume->Status.StartsWith(TEXT("Cancellation requested"))) Volume->CancelBake();
				return false;
			}
			return Finish(false, TEXT("Rural bake exceeded its bounded 900 second guard: ") + Volume->Status);
		}
		if (!Volume->BakedField)
		{
			if (Volume->Status.StartsWith(TEXT("Baking"))) return false;
			return Finish(false, TEXT("Rural bake finished without a saved asset: ") + Volume->Status);
		}
		FString Error;
		if (!Volume->ValidateCurrentBake(Error)) return Finish(false, TEXT("Rural ValidateCurrentBake failed: ") + Error);
		if (Volume->BakedField->WorldPackage != RuralTargetMap)
			return Finish(false, TEXT("Rural generated bake asset points at the wrong world package."));
		BakedAssetPath = Volume->BakedField->GetPathName();
		BakedWorldPackage = Volume->BakedField->WorldPackage;
		BakedFingerprint = Volume->BakedField->SceneFingerprint;
		BakedPackageName = Volume->BakedField->GetOutermost()->GetName();
		if (!VerifyGeneratedBakePackage(Error)) return Finish(false, Error);
		if (!Geometry(Volume->GetWorld(), CopiedGeometry, Error))
			return Finish(false, TEXT("Rural post-bake geometry could not be inspected: ") + Error);
		if (!CompareCopiedGeometry(Error))
			return Finish(false, TEXT("Rural bake changed captured geometry: ") + Error);
		TArray<FVector4> Preview;
		FVector Origin;
		if (!Volume->BakedField->GetProbePreview(RuralTargetMap, BakedFingerprint, Preview, Origin, Error) || Preview.IsEmpty())
			return Finish(false, TEXT("Rural generated bake has no probe preview: ") + Error);
		ProbeCount = Preview.Num();
		Volume->Modify();
		Volume->Tags.AddUnique(FName(ReadyTag));
		Volume->MarkPackageDirty();
		if (!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel, TargetFile))
			return Finish(false, TEXT("Rural final map save failed; incomplete target and generated asset are preserved."));
		bTargetMapFinalSave = true;
		Volume = nullptr;
		Stage = 3;
		return false;
	}

	bool ReloadAndValidate()
	{
		FString Error;
		if (!FEditorFileUtils::LoadMap(RuralTargetMap, false, true))
			return Finish(false, TEXT("Rural final map failed reload after Bake binding save."));
		bTargetMapReloadedFinal = true;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World || World->GetOutermost()->GetName() != RuralTargetMap)
			return Finish(false, TEXT("Rural final reload opened the wrong world package."));
		if (!Geometry(World, CopiedGeometry, Error))
			return Finish(false, TEXT("Rural final reload geometry could not be inspected: ") + Error);
		if (!CompareCopiedGeometry(Error))
			return Finish(false, TEXT("Rural final reload geometry differs from the source copy: ") + Error);
		if (!FindBakeVolume(World, VolumePtr, Error)) return Finish(false, Error);
		Volume = VolumePtr;
		if (!Volume->ActorHasTag(FName(ReadyTag)) || !Volume->BakedField)
			return Finish(false, TEXT("Rural final reload lost the prepared tag or BakeField binding."));
		if (!Volume->ValidateCurrentBake(Error)) return Finish(false, TEXT("Rural final reload ValidateCurrentBake failed: ") + Error);
		if (Volume->BakedField->WorldPackage != RuralTargetMap || Volume->BakedField->GetPathName() != BakedAssetPath)
			return Finish(false, TEXT("Rural final reload resolved a different or wrong-world bake asset."));
		TArray<FVector4> Preview;
		FVector Origin;
		if (!Volume->BakedField->GetProbePreview(RuralTargetMap, Volume->BakedField->SceneFingerprint, Preview, Origin, Error)
			|| Preview.IsEmpty())
			return Finish(false, TEXT("Rural final reload has no current probe preview: ") + Error);
		ProbeCount = Preview.Num();
		if (!Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(PlayerLocation)
			|| !Volume->BakeBounds->Bounds.GetBox().IsInsideOrOn(SourceLocation))
			return Finish(false, TEXT("Rural final reload source/listener coordinates are outside BakeBounds."));
		int32 AudioCount = 0;
		int32 MarkerCount = 0;
		TArray<FString> AudioPaths;
		TArray<FString> MarkerPaths;
		CountAudioAndMarkers(World, AudioCount, MarkerCount, AudioPaths, MarkerPaths);
		if (AudioCount != 2 || MarkerCount != 1 || !IsBackgroundMutedInPreview(World))
			return Finish(false, TEXT("Rural final reload lost its explicit ambient mute or contains unexpected audio."));
		UIMAcousticSourceComponent* Marker = nullptr;
		APlayerStart* Start = nullptr;
		int32 StartCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UIMAcousticSourceComponent*> Markers;
			(*It)->GetComponents(Markers);
			for (UIMAcousticSourceComponent* Candidate : Markers) Marker = Candidate;
			if (APlayerStart* Candidate = Cast<APlayerStart>(*It))
			{
				++StartCount;
				Start = Candidate;
			}
		}
		if (!Marker || !Start || StartCount != 1 || Start->PlayerStartTag != FName(PlayerStartTag)
			|| !Start->GetActorLocation().Equals(PlayerLocation, 0.1)
			|| !Marker->GetOwner()->GetActorLocation().Equals(SourceLocation, 0.1))
			return Finish(false, TEXT("Rural final reload PlayerStart/source identity or coordinates differ."));
		if (!Marker->ValidateSource(Error)) return Finish(false, TEXT("Rural final reload source validation failed: ") + Error);
		UAudioComponent* Audio = Marker->AudioComponent;
		const UMetaSoundSource* MetaSound = Audio ? Cast<UMetaSoundSource>(Audio->Sound) : nullptr;
		if (!Audio || !MetaSound || MetaSound->GetOutermost()->GetName() != FString(WaterDropMetaSound)
			|| MetaSound->OutputFormat != EMetaSoundOutputAudioFormat::Stereo || MetaSound->NumChannels != 2
			|| !Audio->bAutoActivate || Audio->bAutoDestroy || !Audio->bStopWhenOwnerDestroyed
			|| Audio->bAllowSpatialization || Audio->bIsUISound || Audio->bOverrideAttenuation
			|| !FMath::IsNearlyEqual(Audio->VolumeMultiplier, 0.7f) || !FMath::IsNearlyEqual(Audio->PitchMultiplier, 1.0f))
			return Finish(false, TEXT("Rural final reload water-drop autoplay, stereo, gain, or native processing contract differs."));
		if (!Marker->GetOwner()->ActorHasTag(FName(SourceTag)))
			return Finish(false, TEXT("Rural final reload water-drop source tag is missing."));
		if (World->GetWorldSettings()->DefaultGameMode != AGameModeBase::StaticClass())
			return Finish(false, TEXT("Rural final reload does not use the free-movement GameModeBase."));
		if (Volume->DynamicBlockers.Num() != 0)
			return Finish(false, TEXT("Rural final reload unexpectedly populated DynamicBlockers."));
		Stage = 4;
		return Finish(true, TEXT("Persistent Rural Camera3 10 m preview prepared and reloaded; ordinary Play/listener continuity and user listening remain pending."));
	}

	bool SnapshotBakeDirectory(FString& Error)
	{
		BakeDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(), TEXT("IceMoonAcousticField/Content/Bakes")));
		FPaths::NormalizeFilename(BakeDirectory);
		if (!FPaths::DirectoryExists(*BakeDirectory))
		{
			Error = TEXT("The isolated IceMoonAcousticField/Content/Bakes directory is unavailable.");
			return false;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(BakeDirectory, TEXT("*")), true, false);
		for (const FString& File : Files)
		{
			FString Full = FPaths::ConvertRelativePathToFull(FPaths::Combine(BakeDirectory, File));
			FPaths::NormalizeFilename(Full);
			PreRunBakeFiles.Add(Full.ToLower());
		}
		return true;
	}

	bool VerifyGeneratedBakePackage(FString& Error)
	{
		const FString ExpectedPrefix = FString::Printf(TEXT("/IceMoonAcousticField/Bakes/IM_%s_"),
			*FPackageName::GetShortName(RuralTargetMap));
		if (!BakedPackageName.StartsWith(ExpectedPrefix, ESearchCase::CaseSensitive))
		{
			Error = TEXT("Rural generated bake package is not a product Bakes GUID package: ") + BakedPackageName;
			return false;
		}
		const FString Primary = FPackageName::LongPackageNameToFilename(BakedPackageName, FPackageName::GetAssetPackageExtension());
		FString Canonical = FPaths::ConvertRelativePathToFull(Primary);
		FPaths::NormalizeFilename(Canonical);
		if (!IFileManager::Get().FileExists(*Canonical) || PreRunBakeFiles.Contains(Canonical.ToLower()))
		{
			Error = TEXT("Rural generated primary bake package was missing or existed before this run: ") + Canonical;
			return false;
		}
		TArray<FString> Sidecars;
		IFileManager::Get().FindFiles(Sidecars, *FPaths::Combine(BakeDirectory,
			FPackageName::GetShortName(BakedPackageName) + TEXT(".*")), true, false);
		GeneratedBakePaths.Reset();
		for (const FString& Name : Sidecars)
		{
			FString Full = FPaths::ConvertRelativePathToFull(FPaths::Combine(BakeDirectory, Name));
			FPaths::NormalizeFilename(Full);
			if (PreRunBakeFiles.Contains(Full.ToLower()))
			{
				Error = TEXT("Rural generated bake sidecar existed before this run; overwrite is refused: ") + Full;
				return false;
			}
			GeneratedBakePaths.Add(Full);
		}
		if (GeneratedBakePaths.IsEmpty())
		{
			Error = TEXT("Rural Bake reported success but no physical generated package files were found.");
			return false;
		}
		bGeneratedBakeVerified = true;
		return true;
	}

	bool Finish(const bool bRequestedPass, const FString& Reason)
	{
		if (bFinished) return true;
		bool bPass = bRequestedPass;
		FString Message = Reason;
		FString CurrentSha1;
		FString HashError;
		if (!SourceSha1.IsEmpty())
		{
			bSourceMapUnchanged = HashFile(SourceFile, CurrentSha1, SourceBytesAfter, HashError)
				&& CurrentSha1 == SourceSha1;
			if (!bSourceMapUnchanged)
			{
				bPass = false;
				Message += TEXT(" Source Rural map bytes changed during preparation.");
			}
		}
		if (!bPass && bTargetMapSaved)
			Message += TEXT(" Incomplete target map and any generated bake package are intentionally preserved for root review.");
		if (!WriteReport(bPass, Message))
		{
			bPass = false;
			Message += TEXT(" Rural preview preparation receipt could not be written.");
		}
		if (!bPass) Test->AddError(Message); else Test->AddInfo(Message);
		UE_LOG(LogTemp, Display, TEXT("IMLogs RuralPreviewPrepare %s evidence=%s"), *Message, *Evidence);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralPreviewPrepare %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
		bFinished = true;
		return true;
	}

	bool WriteReport(const bool bPass, const FString& Message)
	{
		if (Evidence.IsEmpty()) return false;
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("schema"), TEXT("IMAcousticRuralPreviewPrepare.v1"));
		Json->SetStringField(TEXT("status"), bPass ? TEXT("PASS_PREPARED_RUNTIME_UNVERIFIED") : TEXT("FAIL_CLOSED"));
		Json->SetStringField(TEXT("message"), Message);
		Json->SetStringField(TEXT("scope"), TEXT("Persistent Rural Camera3 local open 10 m preview; ordinary Play/listener continuity pending; no Rural entrance/doorway claim."));
		Json->SetStringField(TEXT("source_map"), RuralSourceMap);
		Json->SetStringField(TEXT("target_map"), RuralTargetMap);
		Json->SetStringField(TEXT("target_file"), TargetFile);
		Json->SetStringField(TEXT("evidence_directory"), Evidence);
		Json->SetBoolField(TEXT("writes_attempted"), bTargetMapSaved);
		Json->SetBoolField(TEXT("target_map_saved"), bTargetMapSaved);
		Json->SetBoolField(TEXT("target_map_reloaded_before_bake"), bTargetMapReloadedBeforeBake);
		Json->SetBoolField(TEXT("target_map_final_save"), bTargetMapFinalSave);
		Json->SetBoolField(TEXT("target_map_reloaded_final"), bTargetMapReloadedFinal);
		Json->SetBoolField(TEXT("preserved_incomplete_target_on_failure"), !bPass && bTargetMapSaved);
		Json->SetStringField(TEXT("source_map_sha1_before"), SourceSha1);
		Json->SetStringField(TEXT("source_map_sha1_after"), SourceSha1.IsEmpty() ? FString() : (bSourceMapUnchanged ? SourceSha1 : TEXT("<changed-or-unreadable>")));
		Json->SetBoolField(TEXT("source_map_bytes_unchanged"), bSourceMapUnchanged);
		Json->SetStringField(TEXT("world_partition"), WorldPartitionState);
		Json->SetNumberField(TEXT("streaming_level_count"), StreamingLevelCount);
		Json->SetBoolField(TEXT("external_actors"), bExternalActors);
		Json->SetStringField(TEXT("camera_actor_path"), CameraActorPath);
		Json->SetArrayField(TEXT("camera_location_cm"), VectorJson(CameraLocation));
		Json->SetArrayField(TEXT("region_min_cm"), VectorJson(RegionMin));
		Json->SetArrayField(TEXT("region_max_cm"), VectorJson(RegionMax));
		Json->SetArrayField(TEXT("region_center_cm"), VectorJson(GroundCenter));
		Json->SetArrayField(TEXT("region_extent_cm"), VectorJson(GroundExtent));
		Json->SetNumberField(TEXT("ground_min_cm"), GroundMin);
		Json->SetNumberField(TEXT("ground_max_cm"), GroundMax);
		Json->SetStringField(TEXT("source_geometry_sha1"), BaselineGeometry.Hash);
		Json->SetStringField(TEXT("target_geometry_sha1"), CopiedGeometry.Hash);
		Json->SetBoolField(TEXT("geometry_unchanged"), bGeometryEquivalent);
		Json->SetStringField(TEXT("geometry_comparator_policy"), TEXT("Non-generated components require strict multiset equality with only layer-internal MID package prefixes normalized; generated SplineMeshComponents require exact mesh/LOD0 digest/material/mobility/collision/rotation/scale/axis/smooth fields, one-to-one world-location pairing within 0.01 cm, deformation position/tangent/offset/boundary fields within 0.02 cm, and scale/roll/up fields within 1e-6."));
		Json->SetNumberField(TEXT("source_static_mesh_component_count"), BaselineGeometry.StaticMeshComponents);
		Json->SetNumberField(TEXT("target_static_mesh_component_count"), CopiedGeometry.StaticMeshComponents);
		Json->SetNumberField(TEXT("source_landscape_component_count"), BaselineGeometry.LandscapeComponents);
		Json->SetNumberField(TEXT("target_landscape_component_count"), CopiedGeometry.LandscapeComponents);
		Json->SetNumberField(TEXT("source_generated_spline_component_count"), BaselineGeometry.GeneratedSplineComponents);
		Json->SetNumberField(TEXT("target_generated_spline_component_count"), CopiedGeometry.GeneratedSplineComponents);
		Json->SetBoolField(TEXT("provisional_uniform_material_mapping"), true);
		Json->SetStringField(TEXT("original_background_sound"), OriginalBackgroundSound);
		Json->SetStringField(TEXT("copy_background_policy"), TEXT("The source map's exact Australia_01_2 ambient bed is preserved in the source and explicitly muted only in this duplicate for isolated water-drop audition."));
		Json->SetBoolField(TEXT("copy_original_background_muted"), bTargetMapSaved && IsBackgroundMutedInPreview(GEditor->GetEditorWorldContext().World()));
		Json->SetStringField(TEXT("material_mapping_quality_label"), TEXT("PROVISIONAL_UNIFORM_TEST_ONLY"));
		Json->SetNumberField(TEXT("provisional_absorption"), ProvisionalAbsorption);
		Json->SetNumberField(TEXT("provisional_scattering"), ProvisionalScattering);
		SetPathArray(Json, TEXT("material_paths"), MaterialPaths);
		Json->SetBoolField(TEXT("provisional_blocking_volume_exclusion"), true);
		Json->SetStringField(TEXT("blocking_volume_exclusion_label"), ProvisionalBlockingTag);
		SetPathArray(Json, TEXT("blocking_volume_component_paths"), BlockingVolumePaths);
		Json->SetStringField(TEXT("backdrop_exclusion_policy"), TEXT("Only exact in-region SM_SkySphere and SM_Horizon_01 components are tagged IMAcousticIgnore in the duplicate; this remains provisional and no other backdrop is filtered."));
		SetPathArray(Json, TEXT("backdrop_component_paths"), BackdropPaths);
		Json->SetStringField(TEXT("dynamic_exclusion_policy"), TEXT("Non-static in-region primitives are omitted by the product static capture rule and enumerated here; DynamicBlockers remains empty. This preview does not validate runtime dynamic blocker updates."));
		SetPathArray(Json, TEXT("dynamic_exclusion_paths"), DynamicExclusionPaths);
		SetPathArray(Json, TEXT("known_static_exclusion_paths"), KnownExcludedPaths);
		SetPathArray(Json, TEXT("unsupported_static_paths"), UnsupportedPaths);
		SetPathArray(Json, TEXT("source_existing_audio_paths"), SourceAudioPaths);
		SetPathArray(Json, TEXT("source_existing_marker_paths"), SourceMarkerPaths);
		Json->SetNumberField(TEXT("source_existing_audio_count"), SourceAudioCount);
		Json->SetNumberField(TEXT("source_existing_marker_count"), SourceMarkerCount);
		Json->SetNumberField(TEXT("final_probe_count"), ProbeCount);
		Json->SetNumberField(TEXT("probe_spacing_cm"), 100.0);
		Json->SetNumberField(TEXT("probe_height_cm"), 150.0);
		Json->SetStringField(TEXT("bake_asset_path"), BakedAssetPath);
		Json->SetStringField(TEXT("bake_world_package"), BakedWorldPackage);
		Json->SetStringField(TEXT("bake_scene_fingerprint"), BakedFingerprint);
		Json->SetBoolField(TEXT("generated_bake_package_verified_new"), bGeneratedBakeVerified);
		SetPathArray(Json, TEXT("generated_bake_files"), GeneratedBakePaths);
		Json->SetArrayField(TEXT("player_start_location_cm"), VectorJson(PlayerLocation));
		Json->SetArrayField(TEXT("water_drop_source_location_cm"), VectorJson(SourceLocation));
		Json->SetBoolField(TEXT("player_start_inside_bake_bounds"), Region.IsInsideOrOn(PlayerLocation));
		Json->SetStringField(TEXT("player_start_ground_policy"), TEXT("Authored from source map Landscape collision height tile and slope before duplication; editor-world WorldStatic traces returned no hits. Duplicate terrain equivalence and PIE collision/occupancy validation pending."));
		Json->SetBoolField(TEXT("water_drop_source_inside_bake_bounds"), Region.IsInsideOrOn(SourceLocation));
		Json->SetStringField(TEXT("ordinary_play_entry"), TEXT("Use persistent duplicate map IM_RuralCamera3Preview_10m in ordinary Play; manual listener/source movement remains Root-owned evidence."));
		FString Serialized;
		auto Writer = TJsonWriterFactory<>::Create(&Serialized);
		FJsonSerializer::Serialize(Json, Writer);
		Writer->Close();
		return FFileHelper::SaveStringToFile(Serialized, *FPaths::Combine(Evidence, TEXT("prepare-result.json")));
	}

	FAutomationTestBase* Test = nullptr;
	double StartedAt = 0.0;
	int32 Stage = 0;
	bool bFinished = false;
	bool bTargetMapSaved = false;
	bool bTargetMapReloadedBeforeBake = false;
	bool bTargetMapFinalSave = false;
	bool bTargetMapReloadedFinal = false;
	bool bExternalActors = false;
	bool bSourceMapUnchanged = false;
	bool bGeometryEquivalent = false;
	bool bGeneratedBakeVerified = false;
	int32 StreamingLevelCount = 0;
	int32 SourceAudioCount = 0;
	int32 SourceMarkerCount = 0;
	int32 ProbeCount = 0;
	int64 SourceBytes = 0;
	int64 SourceBytesAfter = 0;
	double GroundMin = 0.0;
	double GroundMax = 0.0;
	FString Evidence;
	FString SourceFile;
	FString SourceSha1;
	FString CameraActorPath;
	FString WorldPartitionState = TEXT("not_checked");
	FString TargetFile;
	FString BakeDirectory;
	FString BakedAssetPath;
	FString BakedWorldPackage;
	FString BakedFingerprint;
	FString BakedPackageName;
	FVector CameraLocation = FVector::ZeroVector;
	FVector RegionMin = FVector::ZeroVector;
	FVector RegionMax = FVector::ZeroVector;
	FVector GroundCenter = FVector::ZeroVector;
	FVector GroundExtent = FVector::ZeroVector;
	FVector PlayerLocation = FVector::ZeroVector;
	FVector SourceLocation = FVector::ZeroVector;
	AActor* Camera = nullptr;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	AIMAcousticBakeVolume* VolumePtr = nullptr;
	FBox Region;
	FIMRuralGeometry BaselineGeometry;
	FIMRuralGeometry CopiedGeometry;
	TSet<UMaterialInterface*> Materials;
	TSet<FString> PreRunBakeFiles;
	TArray<FString> MaterialPaths;
	TArray<FString> BackdropPaths;
	TArray<FString> BlockingVolumePaths;
	TArray<FString> DynamicExclusionPaths;
	TArray<FString> KnownExcludedPaths;
	TArray<FString> UnsupportedPaths;
	TArray<FString> GeneratedBakePaths;
	TArray<FString> SourceAudioPaths;
	TArray<FString> SourceMarkerPaths;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralPreviewPrepare,
	"IceMoon.AcousticField.Rural.PreparePersistentCamera3Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralPreviewPrepare::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticRuralPreviewPrepareTestPrivate::FIMAcousticRuralPreviewPrepareCommand(this));
	return true;
}

#endif
