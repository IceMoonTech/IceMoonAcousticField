#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticGrassCapture.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "GameFramework/PhysicsVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/ReferencerFinder.h"
#include "UObject/UObjectHash.h"

namespace IMAcousticRuralBakeBenchmarkTestPrivate
{
constexpr TCHAR RuralBenchmarkMap[] = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr TCHAR CineCameraClassPath[] = TEXT("/Script/CinematicCamera.CineCameraActor");
constexpr TCHAR TargetCameraName[] = TEXT("Camera3");
constexpr TCHAR SkyBackdropMeshPath[] = TEXT("/Engine/EngineSky/SM_SkySphere.SM_SkySphere");
constexpr TCHAR HorizonBackdropMeshPath[] = TEXT("/Game/RuralAustralia/Landscape/Horizon_01/SM_Horizon_01.SM_Horizon_01");
constexpr TCHAR IgnoreTag[] = TEXT("IMAcousticIgnore");
constexpr TCHAR RequiredTag[] = TEXT("IMAcousticRequired");
constexpr double DefaultRegionHalfExtentCm = 500.0;
constexpr double TerrainBelowMarginCm = 50.0;
constexpr double TerrainAboveMarginCm = 500.0; // Leaves the 150 cm probe height plus near-ground clearance.
constexpr float ProvisionalAbsorption = 0.1f;
constexpr float ProvisionalScattering = 0.5f;
constexpr uint64 MinAvailablePhysicalBytes = 8ull * 1024ull * 1024ull * 1024ull;
constexpr double MaxBenchmarkSeconds = 900.0;
constexpr double BakeMemorySampleIntervalSeconds = 1.0;
constexpr int32 MaxReportPaths = 256;

struct FIMRuralLandscapeTile
{
	ULandscapeHeightfieldCollisionComponent* Collision = nullptr;
	ULandscapeComponent* Render = nullptr;
	int32 Side = 0;
	TArray<float> Heights;
	TArray<uint8> SurfaceIndices;
};

struct FIMRuralMaterialUse
{
	UMaterialInterface* Material = nullptr;
	int32 StaticMeshSectionUses = 0;
	int32 LandscapeUses = 0;
};

struct FIMRuralBackdropTag
{
	TWeakObjectPtr<UPrimitiveComponent> Component;
	FString ComponentPath;
	FString MeshPath;
	bool bTagWasPresent = false;
	bool bRestored = false;
};

struct FIMRuralPackageFile
{
	FString Filename;
	FString Sha1;
	int64 Bytes = 0;
	bool bDeleted = false;
};

struct FIMRuralMemorySample
{
	FString Stage;
	double ElapsedSeconds = 0.0;
	uint64 UsedPhysical = 0;
	uint64 PeakUsedPhysical = 0;
	uint64 UsedVirtual = 0;
	uint64 PeakUsedVirtual = 0;
	uint64 AvailablePhysical = 0;
};

static bool IntersectsXY(const FBox& Box, const double MinX, const double MinY,
	const double MaxX, const double MaxY)
{
	return Box.IsValid
		&& Box.Min.X <= MaxX && Box.Max.X >= MinX
		&& Box.Min.Y <= MaxY && Box.Max.Y >= MinY;
}

static bool IsKnownEditorOnlyPrimitive(const UPrimitiveComponent* Primitive)
{
#if WITH_EDITORONLY_DATA
	return Primitive && (Primitive->IsVisualizationComponent() || Primitive->IsEditorOnly());
#else
	return false;
#endif
}

static FString Sha1Bytes(const TArray<uint8>& Bytes)
{
	FSHA1 Hash;
	if (!Bytes.IsEmpty()) Hash.Update(Bytes.GetData(), Bytes.Num());
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

static bool HashFile(const FString& Filename, FString& OutSha1, int64& OutBytes, FString& Error)
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
	OutSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	return true;
}

static FIMRuralMemorySample ReadMemory(const TCHAR* Stage, const double StartedAt)
{
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	FIMRuralMemorySample Sample;
	Sample.Stage = Stage;
	Sample.ElapsedSeconds = FPlatformTime::Seconds() - StartedAt;
	Sample.UsedPhysical = Stats.UsedPhysical;
	Sample.PeakUsedPhysical = Stats.PeakUsedPhysical;
	Sample.UsedVirtual = Stats.UsedVirtual;
	Sample.PeakUsedVirtual = Stats.PeakUsedVirtual;
	Sample.AvailablePhysical = Stats.AvailablePhysical;
	return Sample;
}

static TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& Value)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Add(MakeShared<FJsonValueNumber>(Value.X));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Y));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Z));
	return Result;
}

static void WriteSortedPathSet(TSharedRef<FJsonObject> Json, const TCHAR* Field,
	const TSet<FString>& Paths)
{
	TArray<FString> Sorted;
	for (const FString& Path : Paths) Sorted.Add(Path);
	Sorted.Sort();
	const int32 CountToWrite = FMath::Min(Sorted.Num(), MaxReportPaths);
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(CountToWrite);
	for (int32 Index = 0; Index < CountToWrite; ++Index)
		Values.Add(MakeShared<FJsonValueString>(Sorted[Index]));
	Json->SetNumberField(FString(Field) + TEXT("_count"), Sorted.Num());
	Json->SetArrayField(Field, Values);
	Json->SetBoolField(FString(Field) + TEXT("_truncated"), Sorted.Num() > CountToWrite);
}

class FIMRuralBakeLogCapture final : public FOutputDevice
{
public:
	void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
	{
		if (!Text || (!FCString::Strifind(Text, TEXT("IMLogs AcousticBakeAssetSave"))
			&& !FCString::Strifind(Text, TEXT("IMLogs AcousticCapturePhases"))
			&& !FCString::Strifind(Text, TEXT("IMLogs AcousticG2CaptureCore"))
			&& !FCString::Strifind(Text, TEXT("IMLogs AcousticLandscape"))))
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

class FIMAcousticRuralBakeBenchmarkCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticRuralBakeBenchmarkCommand(FAutomationTestBase* InTest)
		: Test(InTest), RunId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
	{
		StartedAt = FPlatformTime::Seconds();
	}

	bool Update() override
	{
		if (bFinished) return true;
		if (Stage == 0) return StartBenchmark();
		if (Stage == 1) return PollBake();
		return Finish(TEXT("FAIL"), TEXT("Unexpected Rural Bake benchmark stage."));
	}

private:
	void Sample(const TCHAR* StageName, const bool bForce = false)
	{
		const double Now = FPlatformTime::Seconds();
		if (!bForce && Now - LastMemorySampleAt < BakeMemorySampleIntervalSeconds) return;
		MemorySamples.Add(ReadMemory(StageName, StartedAt));
		LastMemorySampleAt = Now;
	}

	void AddIssue(const FString& Issue)
	{
		CoverageIssues.Add(Issue);
		bCoverageComplete = false;
	}

	bool CacheLandscapeAndFindGround(UWorld* World, const double CenterX, const double CenterY,
		double& OutGroundMin, double& OutGroundMax, FString& Error)
	{
		OutGroundMin = TNumericLimits<double>::Max();
		OutGroundMax = TNumericLimits<double>::Lowest();
		const double MinX = CenterX - RegionHalfExtentCm;
		const double MaxX = CenterX + RegionHalfExtentCm;
		const double MinY = CenterY - RegionHalfExtentCm;
		const double MaxY = CenterY + RegionHalfExtentCm;

		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			TArray<UActorComponent*> Components;
			ActorIt->GetComponents(Components);
			for (UActorComponent* ActorComponent : Components)
			{
				ULandscapeHeightfieldCollisionComponent* Collision =
					Cast<ULandscapeHeightfieldCollisionComponent>(ActorComponent);
				if (!Collision || !IntersectsXY(Collision->Bounds.GetBox(), MinX, MinY, MaxX, MaxY))
					continue;

				ULandscapeComponent* Render = Collision->GetRenderComponent();
				const int32 Quads = Collision->CollisionSizeQuads;
				const int32 Side = Quads + 1;
				const int64 SampleCount = static_cast<int64>(Side) * Side;
				const FTransform Transform = Collision->GetComponentTransform();
				if (!IsValid(Render) || Quads <= 0 || SampleCount > MAX_int32
					|| !FMath::IsNearlyEqual(Collision->CollisionScale, 1.0f)
					|| Render->ComponentSizeQuads != Quads
					|| Render->GetNumMaterials() != 1
					|| Collision->CookedPhysicalMaterials.IsEmpty()
					|| !Collision->CookedPhysicalMaterials[0]
					|| !Transform.TransformVectorNoScale(FVector::UpVector).Equals(FVector::UpVector, 1.e-4))
				{
					Error = TEXT("Camera3 terrain has an unsupported or incomplete Landscape collision/render pair: ")
						+ Collision->GetPathName();
					return false;
				}

				FIMRuralLandscapeTile Tile;
				Tile.Collision = Collision;
				Tile.Render = Render;
				Tile.Side = Side;
				Tile.Heights.SetNumUninitialized(static_cast<int32>(SampleCount));
				Tile.SurfaceIndices.SetNumUninitialized(static_cast<int32>(SampleCount));
				if (!Collision->FillHeightTile(MakeArrayView(Tile.Heights), 0, Side)
					|| !Collision->FillMaterialIndexTile(MakeArrayView(Tile.SurfaceIndices), 0, Side))
				{
					Error = TEXT("Landscape collision height/material readback failed: ") + Collision->GetPathName();
					return false;
				}

				for (int32 Y = 0; Y < Side; ++Y)
				{
					for (int32 X = 0; X < Side; ++X)
					{
						const int32 Index = Y * Side + X;
						const float Height = Tile.Heights[Index];
						if (!FMath::IsFinite(Height))
						{
							Error = TEXT("Landscape collision contains a non-finite height: ") + Collision->GetPathName();
							return false;
						}
						const FVector XY = Transform.TransformPosition(FVector(
							X * Collision->CollisionScale, Y * Collision->CollisionScale, 0.0));
						if (XY.X >= MinX && XY.X <= MaxX && XY.Y >= MinY && XY.Y <= MaxY)
						{
							OutGroundMin = FMath::Min(OutGroundMin, static_cast<double>(Height));
							OutGroundMax = FMath::Max(OutGroundMax, static_cast<double>(Height));
						}
					}
				}
				LandscapeTiles.Add(MoveTemp(Tile));
			}
		}

		if (LandscapeTiles.IsEmpty() || OutGroundMin > OutGroundMax)
		{
			Error = FString::Printf(TEXT("No actual Landscape collision samples cover the %.1f m Camera3 XY region."),
				RegionHalfExtentCm * 2.0 / 100.0);
			return false;
		}
		return true;
	}

	bool PrepareBackdropExclusions(UWorld* World, const FBox& RegionBounds, FString& Error)
	{
		TMap<FString, int32> MatchedCounts;
		MatchedCounts.Add(SkyBackdropMeshPath, 0);
		MatchedCounts.Add(HorizonBackdropMeshPath, 0);
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			TArray<UActorComponent*> Components;
			ActorIt->GetComponents(Components);
			for (UActorComponent* ActorComponent : Components)
			{
				UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(ActorComponent);
				if (!MeshComponent || !RegionBounds.Intersect(MeshComponent->Bounds.GetBox())) continue;
				const FString MeshPath = GetPathNameSafe(MeshComponent->GetStaticMesh());
				if (!MatchedCounts.Contains(MeshPath)) continue;
				if (MeshComponent->Mobility != EComponentMobility::Static
					|| MeshComponent->ComponentHasTag(FName(RequiredTag)))
				{
					Error = TEXT("Exact backdrop asset is not a non-required Static mesh component: ")
						+ MeshComponent->GetPathName();
					return false;
				}
				++MatchedCounts.FindOrAdd(MeshPath);
				FIMRuralBackdropTag& Record = BackdropTags.AddDefaulted_GetRef();
				Record.Component = MeshComponent;
				Record.ComponentPath = MeshComponent->GetPathName();
				Record.MeshPath = MeshPath;
				Record.bTagWasPresent = MeshComponent->ComponentHasTag(FName(IgnoreTag));
				if (!Record.bTagWasPresent)
					MeshComponent->ComponentTags.AddUnique(FName(IgnoreTag));
			}
		}

		for (const TPair<FString, int32>& Count : MatchedCounts)
		{
			if (Count.Value != 1)
			{
				Error = FString::Printf(TEXT("Expected exactly one in-region backdrop component for %s; found %d."),
					*Count.Key, Count.Value);
				return false;
			}
		}
		bBackdropClassificationConfirmed = true;
		return true;
	}

	FIMRuralLandscapeTile* FindLandscapeTile(ULandscapeHeightfieldCollisionComponent* Collision)
	{
		for (FIMRuralLandscapeTile& Tile : LandscapeTiles)
			if (Tile.Collision == Collision) return &Tile;
		return nullptr;
	}

	void AddMaterialUse(UMaterialInterface* Material, const bool bLandscape)
	{
		if (!IsValid(Material))
		{
			AddIssue(bLandscape
				? TEXT("Landscape surface has a null effective material.")
				: TEXT("An included LOD0 section has a null effective material."));
			return;
		}
		FIMRuralMaterialUse& Use = MaterialUses.FindOrAdd(Material);
		Use.Material = Material;
		if (bLandscape) ++Use.LandscapeUses;
		else ++Use.StaticMeshSectionUses;
	}

	bool CountLandscapeTriangles(FIMRuralLandscapeTile& Tile, const FBox& RegionBounds,
		int64& OutLocalTriangles, int64& OutFullTileTriangles, FString& Error)
	{
		OutLocalTriangles = 0;
		OutFullTileTriangles = 0;
		ULandscapeHeightfieldCollisionComponent* Collision = Tile.Collision;
		ULandscapeComponent* Render = Tile.Render;
		if (!IsValid(Collision) || !IsValid(Render)
			|| Render->GetCollisionComponent() != Collision
			|| Render->GetNumMaterials() != 1)
		{
			Error = TEXT("Landscape collision/render identity changed before capture: ")
				+ (IsValid(Collision) ? Collision->GetPathName() : TEXT("invalid"));
			return false;
		}
		AddMaterialUse(Render->GetMaterial(0), true);
		const FTransform Transform = Collision->GetComponentTransform();
		for (int32 Y = 0; Y < Collision->CollisionSizeQuads; ++Y)
		{
			for (int32 X = 0; X < Collision->CollisionSizeQuads; ++X)
			{
				const int32 Index = Y * Tile.Side + X;
				const uint8 Surface = Tile.SurfaceIndices[Index];
				if (Surface != 0 && Surface != 255)
				{
					Error = TEXT("Landscape tile contains unsupported physical material indices: ")
						+ Collision->GetPathName();
					return false;
				}
				if (Surface == 255) continue;
				OutFullTileTriangles += 2;

				FBox QuadBounds(ForceInit);
				const int32 CornerIndices[] = { Index, Index + 1, Index + Tile.Side, Index + Tile.Side + 1 };
				for (const int32 CornerIndex : CornerIndices)
				{
					const int32 VertexY = CornerIndex / Tile.Side;
					const int32 VertexX = CornerIndex % Tile.Side;
					const FVector XY = Transform.TransformPosition(FVector(
						VertexX * Collision->CollisionScale, VertexY * Collision->CollisionScale, 0.0));
					QuadBounds += FVector(XY.X, XY.Y, Tile.Heights[CornerIndex]);
				}
				if (RegionBounds.Intersect(QuadBounds)) OutLocalTriangles += 2;
			}
		}
		return true;
	}

	bool ReadMeshSectionTriangles(UStaticMeshComponent* Component, UStaticMesh* Mesh,
		int64& OutTriangles, FString& Error)
	{
		OutTriangles = 0;
		const FStaticMeshRenderData* RenderData = IsValid(Mesh) ? Mesh->GetRenderData() : nullptr;
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			Error = TEXT("Included mesh has no render LOD0: ") + GetPathNameSafe(Mesh);
			return false;
		}
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData()
			|| LOD.IndexBuffer.GetArrayView().Num() == 0 || LOD.Sections.IsEmpty())
		{
			Error = TEXT("Included mesh has unreadable CPU LOD0 geometry: ") + Mesh->GetPathName();
			return false;
		}
		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			if (Section.NumTriangles == 0) continue;
			UMaterialInterface* Material = Component->GetMaterial(Section.MaterialIndex);
			if (!IsValid(Material))
			{
				Error = FString::Printf(TEXT("Included mesh section %d has no effective material: %s."),
					Section.MaterialIndex, *Component->GetPathName());
				return false;
			}
			AddMaterialUse(Material, false);
			OutTriangles += Section.NumTriangles;
		}
		return OutTriangles > 0;
	}

	bool AuditSceneCoverage(UWorld* World, const FBox& RegionBounds, FString& Error)
	{
		// Landscape grass components are produced by the product's finite grass capture: the product ignores their cached Bounds and
		// their lazily generated own instances, and expands exactly the captured region instances.
		FString GrassError;
		if (!IMCaptureAcousticGrassRegion(World, RegionBounds, GrassState, GrassCapture, GrassError))
		{
			Error = TEXT("Product grass region capture failed: ") + GrassError;
			return false;
		}
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			AActor* Actor = *ActorIt;
			TArray<UPrimitiveComponent*> Primitives;
			Actor->GetComponents(Primitives);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				const FIMAcousticGrassCapturedComponent* CapturedGrass = IsValid(Primitive)
					? GrassCapture.Components.Find(Cast<UInstancedStaticMeshComponent>(Primitive)) : nullptr;
				if (!IsValid(Primitive) || (!CapturedGrass && !RegionBounds.Intersect(Primitive->Bounds.GetBox()))) continue;
				if (Actor == World->GetDefaultBrush() || Actor == World->GetDefaultPhysicsVolume())
				{
					KnownExcludedComponents.Add(Primitive->GetPathName());
					continue;
				}
				if (IsKnownEditorOnlyPrimitive(Primitive))
				{
					KnownExcludedComponents.Add(Primitive->GetPathName());
					continue;
				}

				ULandscapeComponent* LandscapeRender = Cast<ULandscapeComponent>(Primitive);
				if (LandscapeRender)
				{
					ULandscapeHeightfieldCollisionComponent* Collision = LandscapeRender->GetCollisionComponent();
					if (!IsValid(Collision) || !RegionBounds.Intersect(Collision->Bounds.GetBox()))
						AddIssue(TEXT("Landscape render surface has no paired collision inside the bake region: ")
							+ LandscapeRender->GetPathName());
					continue;
				}
				if (ULandscapeHeightfieldCollisionComponent* Collision =
					Cast<ULandscapeHeightfieldCollisionComponent>(Primitive))
				{
					FIMRuralLandscapeTile* Tile = FindLandscapeTile(Collision);
					if (!Tile)
					{
						AddIssue(TEXT("Intersecting Landscape collision was not included in terrain height readback: ")
							+ Collision->GetPathName());
						continue;
					}
					int64 LocalTriangles = 0;
					int64 FullTileTriangles = 0;
					FString LandscapeError;
					if (!CountLandscapeTriangles(*Tile, RegionBounds, LocalTriangles, FullTileTriangles, LandscapeError))
					{
						AddIssue(LandscapeError);
						continue;
					}
					++LandscapeCollisionComponents;
					LandscapeSelectedTriangles += LocalTriangles;
					LandscapeFullTileTriangles += FullTileTriangles;
					ExpectedTriangles += LocalTriangles;
					continue;
				}

				if (Primitive->ComponentHasTag(FName(IgnoreTag)))
				{
					const UStaticMeshComponent* TaggedMesh = Cast<UStaticMeshComponent>(Primitive);
					const FString MeshPath = TaggedMesh ? GetPathNameSafe(TaggedMesh->GetStaticMesh()) : FString();
					if (MeshPath == SkyBackdropMeshPath || MeshPath == HorizonBackdropMeshPath)
					{
						BackdropExcludedPaths.Add(Primitive->GetPathName());
						continue;
					}
					AddIssue(TEXT("Unexpected pre-existing IMAcousticIgnore would omit an in-region component: ")
						+ Primitive->GetPathName());
					continue;
				}
				if (Primitive->Mobility != EComponentMobility::Static)
				{
					// This benchmark measures the product's authored-static
					// bake scope; retain every dynamic exclusion in the report.
					if (Primitive->ComponentHasTag(FName(RequiredTag)))
						AddIssue(TEXT("Required non-static primitive cannot enter static bake: ") + Primitive->GetPathName());
					else
						KnownExcludedComponents.Add(Primitive->GetPathName());
					continue;
				}

				UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Primitive);
				if (!MeshComponent)
				{
					const FString ComponentClass = Primitive->GetClass()->GetPathName();
					const FString ActorClass = Actor->GetClass()->GetPathName();
					const bool bKnownHelper = ComponentClass == TEXT("/Script/Landscape.LandscapeSplinesComponent")
						|| (ComponentClass == TEXT("/Script/Engine.BrushComponent")
							&& (ActorClass == TEXT("/Script/Engine.LightmassImportanceVolume")
								|| ActorClass == TEXT("/Script/Engine.BlockingVolume")));
					if (bKnownHelper && !Primitive->ComponentHasTag(FName(RequiredTag)))
						KnownExcludedComponents.Add(Primitive->GetPathName());
					else
						AddIssue(TEXT("Unmapped or required non-mesh primitive overlaps the acoustic region: ")
							+ Primitive->GetPathName());
					continue;
				}
				if (Primitive->ComponentHasTag(FName(RequiredTag)))
				{
					AddIssue(TEXT("Required component overlaps the region; its acoustic surface must not be excluded: ")
						+ Primitive->GetPathName());
					continue;
				}
				UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
				if (!IsValid(Mesh))
				{
					AddIssue(TEXT("In-region StaticMeshComponent has no UStaticMesh asset: ")
						+ Primitive->GetPathName());
					continue;
				}
				int64 MeshTriangles = 0;
				FString MeshError;
				if (!ReadMeshSectionTriangles(MeshComponent, Mesh, MeshTriangles, MeshError))
				{
					AddIssue(MeshError);
					continue;
				}

				UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(MeshComponent);
				UHierarchicalInstancedStaticMeshComponent* HISM =
					Cast<UHierarchicalInstancedStaticMeshComponent>(MeshComponent);
				int32 ComponentPlacements = 0;
				if (CapturedGrass)
				{
					ComponentPlacements = CapturedGrass->RegionInstances.Num();
					InRegionISMInstances += ComponentPlacements;
					if (HISM) InRegionHISMInstances += ComponentPlacements;
					ExpectedTriangles += MeshTriangles * ComponentPlacements;
				}
				else if (ISM)
				{
					for (int32 InstanceIndex = 0; InstanceIndex < ISM->GetInstanceCount(); ++InstanceIndex)
					{
						FTransform WorldTransform;
						if (!ISM->GetInstanceTransform(InstanceIndex, WorldTransform, true))
						{
							AddIssue(FString::Printf(TEXT("Cannot read in-region instance transform %d: %s."),
								InstanceIndex, *Primitive->GetPathName()));
							continue;
						}
						if (WorldTransform.ContainsNaN() || FMath::IsNearlyZero(WorldTransform.GetDeterminant()))
						{
							AddIssue(FString::Printf(TEXT("Invalid in-region instance transform %d: %s."),
								InstanceIndex, *Primitive->GetPathName()));
							continue;
						}
						if (!RegionBounds.Intersect(Mesh->GetBounds().TransformBy(WorldTransform).GetBox())) continue;
						++ComponentPlacements;
						++InRegionISMInstances;
						if (HISM) ++InRegionHISMInstances;
						ExpectedTriangles += MeshTriangles;
					}
				}
				else if (RegionBounds.Intersect(MeshComponent->Bounds.GetBox()))
				{
					ComponentPlacements = 1;
					++InRegionOrdinaryMeshPlacements;
					if (Cast<USplineMeshComponent>(MeshComponent)) ++InRegionSplineMeshComponents;
					ExpectedTriangles += MeshTriangles;
				}

				if (ComponentPlacements == 0) continue;
				++InRegionStaticMeshComponents;
				if (HISM) ++InRegionHISMComponents;
				else if (ISM) ++InRegionISMComponents;
				else if (Cast<USplineMeshComponent>(MeshComponent)) {}
				else ++InRegionPlainStaticMeshComponents;
				UniqueMeshPaths.Add(Mesh->GetPathName());
			}
		}

		if (LandscapeCollisionComponents == 0)
			AddIssue(TEXT("The selected region contains no paired Landscape collision component."));
		if (InRegionHISMInstances == 0)
			AddIssue(TEXT("The selected region contains no in-region foliage HISM instance; fixture is not representative."));
		if (BackdropExcludedPaths.Num() != 2)
			AddIssue(FString::Printf(TEXT("Expected to exclude only the two verified sky/horizon backdrops; tagged %d components."),
				BackdropExcludedPaths.Num()));
		if (ExpectedTriangles <= 0)
			AddIssue(TEXT("No supported static mesh or local Landscape triangles were identified."));

		if (!bCoverageComplete)
		{
			Error = CoverageIssues.IsEmpty() ? TEXT("Static-scene coverage audit was incomplete.") : CoverageIssues[0];
			return false;
		}
		return true;
	}

	bool SnapshotBakeDirectory(FString& Error)
	{
		BakeDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(), TEXT("IceMoonAcousticField/Content/Bakes")));
		FPaths::NormalizeFilename(BakeDirectory);
		if (!FPaths::DirectoryExists(*BakeDirectory))
		{
			Error = TEXT("Exact isolated IceMoonAcousticField/Content/Bakes directory is unavailable.");
			return false;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(BakeDirectory, TEXT("*")), true, false);
		for (const FString& File : Files)
		{
			FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(BakeDirectory, File));
			FPaths::NormalizeFilename(FullPath);
			PreRunBakeFiles.Add(FullPath.ToLower());
		}
		return true;
	}

	bool PrepareMaterialsAndVolume(FString& Error)
	{
		if (MaterialUses.IsEmpty())
		{
			Error = TEXT("No explicit provisional acoustic material mappings were discovered.");
			return false;
		}
		TArray<UMaterialInterface*> SortedMaterials;
		for (const TPair<UMaterialInterface*, FIMRuralMaterialUse>& Entry : MaterialUses)
			SortedMaterials.Add(Entry.Key);
		SortedMaterials.Sort([](const UMaterialInterface& A, const UMaterialInterface& B)
		{
			return A.GetPathName() < B.GetPathName();
		});

		Volume->Materials.Reset();
		for (UMaterialInterface* Material : SortedMaterials)
		{
			if (!IsValid(Material))
			{
				Error = TEXT("A provisional material mapping lost its UMaterialInterface before InspectScene.");
				return false;
			}
			FIMAcousticMaterialMapping& Mapping = Volume->Materials.AddDefaulted_GetRef();
			Mapping.Material = Material;
			Mapping.Absorption = FVector(ProvisionalAbsorption, ProvisionalAbsorption, ProvisionalAbsorption);
			Mapping.Scattering = ProvisionalScattering;
		}
		return true;
	}

	bool ResolveRegionHalfExtentArgument(FString& Error)
	{
		RegionHalfExtentCm = DefaultRegionHalfExtentCm;
		RequestedRegionHalfExtentArgument = TEXT("<absent; default=500>");
		bRegionHalfExtentArgumentValid = false;

		const TCHAR* Cursor = FCommandLine::Get();
		FString Token;
		int32 MatchingArgumentCount = 0;
		while (FParse::Token(Cursor, Token, false))
		{
			int32 OptionStart = 0;
			while (Token.IsValidIndex(OptionStart)
				&& (Token[OptionStart] == TEXT('-') || Token[OptionStart] == TEXT('/')))
			{
				++OptionStart;
			}
			const FString Candidate = Token.Mid(OptionStart);
			if (!Candidate.StartsWith(TEXT("IMRuralHalfExtentCm"), ESearchCase::IgnoreCase))
				continue;

			++MatchingArgumentCount;
			if (MatchingArgumentCount == 1)
				RequestedRegionHalfExtentArgument = Token;
			else
				RequestedRegionHalfExtentArgument += TEXT(" | ") + Token;
			if (MatchingArgumentCount > 1)
			{
				Error = TEXT("IMRuralHalfExtentCm was supplied more than once; provide exactly one value.");
				return false;
			}

			if (Token == TEXT("-IMRuralHalfExtentCm=500"))
				RegionHalfExtentCm = 500.0;
			else if (Token == TEXT("-IMRuralHalfExtentCm=1000"))
				RegionHalfExtentCm = 1000.0;
			else if (Token == TEXT("-IMRuralHalfExtentCm=2000"))
				RegionHalfExtentCm = 2000.0;
			else if (Token == TEXT("-IMRuralHalfExtentCm=3000"))
				RegionHalfExtentCm = 3000.0;
			else
			{
				Error = FString::Printf(TEXT("Unsupported IMRuralHalfExtentCm argument '%s'; supported values are 500, 1000, 2000, and 3000 cm."),
					*Token);
				return false;
			}
		}

		bRegionHalfExtentArgumentValid = true;
		return true;
	}

	bool StartBenchmark()
	{
		FString Error;
		if (!ResolveRegionHalfExtentArgument(Error))
			return Finish(TEXT("FAIL"), Error);
		Sample(TEXT("before_map_load"), true);
		if (!HashFile(FPackageName::LongPackageNameToFilename(
				RuralBenchmarkMap, FPackageName::GetMapPackageExtension()), MapSha1Before, MapBytesBefore, FailureReason))
			return Finish(TEXT("FAIL"), FailureReason);
		if (MemorySamples.Last().AvailablePhysical < MinAvailablePhysicalBytes)
			return Finish(TEXT("INCONCLUSIVE"), TEXT("System available physical RAM was below 8 GiB before Rural map load."));
		const double MapLoadStarted = FPlatformTime::Seconds();
		const bool bMapLoadSucceeded = FEditorFileUtils::LoadMap(RuralBenchmarkMap, false, true);
		MapLoadSeconds = FPlatformTime::Seconds() - MapLoadStarted;
		if (!bMapLoadSucceeded)
			return Finish(TEXT("FAIL"), TEXT("Could not load the required isolated RuralAustralia map."));
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!IsValid(World) || World->GetOutermost()->GetName() != RuralBenchmarkMap)
			return Finish(TEXT("FAIL"), TEXT("Loaded Editor world does not match the exact RuralAustralia package."));
		MapPackage = World->GetOutermost();
		bMapDirtyAtBaseline = MapPackage->IsDirty();
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
			++PreExistingBakeVolumeCount;
		if (PreExistingBakeVolumeCount != 0)
			return Finish(TEXT("FAIL"), TEXT("Rural map already contains a bake volume; benchmark refuses to touch its binding."));

		const UClass* CineCameraClass = FindObject<UClass>(nullptr, CineCameraClassPath);
		AActor* Camera = nullptr;
		int32 CameraMatches = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetFName() == FName(TargetCameraName)
				&& CineCameraClass && (*It)->IsA(CineCameraClass))
			{
				Camera = *It;
				++CameraMatches;
			}
		}
		if (CameraMatches != 1 || !IsValid(Camera))
			return Finish(TEXT("FAIL"), TEXT("Expected exactly one CineCameraActor named Camera3."));
		CameraPath = Camera->GetPathName();
		CameraLocation = Camera->GetActorLocation();
		if (FMath::Abs(CameraLocation.X - (-6597.06298828125)) > 2.0
			|| FMath::Abs(CameraLocation.Y - 14720.8662109375) > 2.0)
			return Finish(TEXT("FAIL"), TEXT("Camera3 XY location differs from the RuralSpatialAudit target center."));

		double GroundMin = 0.0;
		double GroundMax = 0.0;
		const double TerrainStarted = FPlatformTime::Seconds();
		if (!CacheLandscapeAndFindGround(World, CameraLocation.X, CameraLocation.Y, GroundMin, GroundMax, Error))
			return Finish(TEXT("FAIL"), Error);
		TerrainDiscoverySeconds = FPlatformTime::Seconds() - TerrainStarted;
		GroundHeightMinCm = GroundMin;
		GroundHeightMaxCm = GroundMax;
		RegionMinZ = GroundMin - TerrainBelowMarginCm;
		RegionMaxZ = GroundMax + TerrainAboveMarginCm;
		Region = FBox(
			FVector(CameraLocation.X - RegionHalfExtentCm, CameraLocation.Y - RegionHalfExtentCm, RegionMinZ),
			FVector(CameraLocation.X + RegionHalfExtentCm, CameraLocation.Y + RegionHalfExtentCm, RegionMaxZ));
		GroundCenter = FVector(CameraLocation.X, CameraLocation.Y, (RegionMinZ + RegionMaxZ) * 0.5);
		GroundExtent = FVector(RegionHalfExtentCm, RegionHalfExtentCm, (RegionMaxZ - RegionMinZ) * 0.5);

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		Volume = World->SpawnActor<AIMAcousticBakeVolume>(GroundCenter, FRotator::ZeroRotator, SpawnParams);
		if (!IsValid(Volume.Get()) || !IsValid(Volume->BakeBounds))
			return Finish(TEXT("FAIL"), TEXT("Could not create the transient Rural BakeVolume."));
		Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
		Volume->SetActorLocation(GroundCenter);
		Volume->BakeBounds->SetBoxExtent(GroundExtent, false);
		Volume->BakeBounds->SetWorldLocation(GroundCenter, false, nullptr, ETeleportType::TeleportPhysics);
		Volume->BakeBounds->UpdateComponentToWorld();
		Volume->BakeBounds->UpdateBounds();
		Volume->ProbeSpacingCm = 100.0f;
		Volume->ProbeHeightCm = 150.0f;
		Volume->BakedField = nullptr;
		Region = Volume->BakeBounds->Bounds.GetBox();
		const FVector ActualBakeBoundsCenter = Region.GetCenter();
		if (!Region.IsValid || !ActualBakeBoundsCenter.Equals(GroundCenter, 0.1)
			|| !FMath::IsNearlyEqual(Region.GetExtent().X, RegionHalfExtentCm, 0.1)
			|| !FMath::IsNearlyEqual(Region.GetExtent().Y, RegionHalfExtentCm, 0.1))
			return Finish(TEXT("FAIL"), FString::Printf(
				TEXT("Transient BakeBounds center/extent mismatch: actual_center=%s expected_center=%s extent=%s."),
				*ActualBakeBoundsCenter.ToString(), *GroundCenter.ToString(), *Region.GetExtent().ToString()));
		if (!PrepareBackdropExclusions(World, Region, Error))
			return Finish(TEXT("FAIL"), Error);

		const double AuditStarted = FPlatformTime::Seconds();
		if (!AuditSceneCoverage(World, Region, Error))
			return Finish(TEXT("FAIL"), Error);
		CoverageAuditSeconds = FPlatformTime::Seconds() - AuditStarted;
		if (!PrepareMaterialsAndVolume(Error))
			return Finish(TEXT("FAIL"), Error);
		if (!SnapshotBakeDirectory(Error))
			return Finish(TEXT("FAIL"), Error);

		LogCapture = MakeUnique<FIMRuralBakeLogCapture>();
		if (!LogCapture.IsValid() || !GLog)
			return Finish(TEXT("FAIL"), TEXT("Could not attach the product CaptureScene/Bake log observer."));
		GLog->AddOutputDevice(LogCapture.Get());
		bLogCaptureAttached = true;

		if (FPlatformTime::Seconds() - StartedAt >= MaxBenchmarkSeconds)
			return Finish(TEXT("INCONCLUSIVE"), TEXT("900 second benchmark guard expired before InspectScene."));
		Sample(TEXT("before_inspect_scene"), true);
		const double InspectStarted = FPlatformTime::Seconds();
		Volume->InspectScene();
		InspectSceneSeconds = FPlatformTime::Seconds() - InspectStarted;
		ActualTriangles = Volume->ExportedTriangles;
		Sample(TEXT("after_inspect_scene"), true);
		if (!Volume->Status.StartsWith(TEXT("Scene check passed"))
			|| Volume->ExportedTriangles != ExpectedTriangles)
		{
			// Name the product's own blocking reasons: only REQUIRED/pending issues fail the scene check, the rest are exclusions.
			FString BlockingIssues;
			int32 BlockingCount = 0;
			for (const FString& Issue : Volume->SceneIssues)
			{
				if (!Issue.Contains(TEXT("REQUIRED")) && !Issue.Contains(TEXT("pending"))) continue;
				if (++BlockingCount <= 6) BlockingIssues += TEXT(" | ") + Issue.Left(300);
			}
			return Finish(TEXT("FAIL"), FString::Printf(
				TEXT("Product InspectScene did not match the local-region coverage oracle: status='%s', actual_triangles=%d, expected_local_triangles=%lld (full Landscape tiles=%lld); blocking scene issues (%d):%s"),
				*Volume->Status, Volume->ExportedTriangles, static_cast<long long>(ExpectedTriangles),
				static_cast<long long>(LandscapeFullTileTriangles), BlockingCount, *BlockingIssues));
		}

		const double ProbeStarted = FPlatformTime::Seconds();
		Volume->GenerateProbes();
		GenerateProbesSeconds = FPlatformTime::Seconds() - ProbeStarted;
		ActualTriangles = Volume->ExportedTriangles;
		Sample(TEXT("after_generate_probes"), true);
		ProbeCount = Volume->GeneratedProbes;
		if (!Volume->Status.Contains(TEXT("generated probes")) || ProbeCount <= 0)
			return Finish(TEXT("FAIL"), TEXT("Product GenerateProbes did not create a non-empty probe set: ") + Volume->Status);
		if (Volume->ExportedTriangles != ExpectedTriangles)
			return Finish(TEXT("FAIL"), TEXT("GenerateProbes changed the captured triangle count after InspectScene."));
		if (MemorySamples.Last().AvailablePhysical < MinAvailablePhysicalBytes)
			return Finish(TEXT("INCONCLUSIVE"), TEXT("Available physical RAM fell below 8 GiB after probe generation; SDK Bake was not started."));

		if (!SnapshotBakeDirectory(Error))
			return Finish(TEXT("FAIL"), Error);
		BakeStartedAt = FPlatformTime::Seconds();
		const double BakeCallStarted = FPlatformTime::Seconds();
		Volume->Bake();
		BakeCallSeconds = FPlatformTime::Seconds() - BakeCallStarted;
		if (!Volume->Status.StartsWith(TEXT("Baking")))
			return Finish(TEXT("FAIL"), TEXT("Product Bake did not start after valid scene/probes: ") + Volume->Status);
		Sample(TEXT("bake_started"), true);
		Stage = 1;
		return false;
	}

	bool PollBake()
	{
		if (!IsValid(Volume.Get()))
			return Finish(TEXT("FAIL"), TEXT("Transient BakeVolume was destroyed while the product bake was pending."));
		Sample(TEXT("bake_wait"));
		const bool bPending = Volume->Status.StartsWith(TEXT("Baking"))
			|| Volume->Status.StartsWith(TEXT("Cancellation requested"));
		if (bPending)
		{
			const double Elapsed = FPlatformTime::Seconds() - BakeStartedAt;
			if (MemorySamples.Last().AvailablePhysical < MinAvailablePhysicalBytes
				&& !bCancellationRequested)
			{
				PendingOutcome = TEXT("INCONCLUSIVE");
				FailureReason = TEXT("Available physical RAM fell below 8 GiB during the Bake; cancellation was requested.");
				Volume->CancelBake();
				bCancellationRequested = true;
			}
			else if (Elapsed >= MaxBenchmarkSeconds && !bCancellationRequested)
			{
				PendingOutcome = TEXT("INCONCLUSIVE");
				FailureReason = TEXT("900 second Bake guard expired; cancellation was requested.");
				Volume->CancelBake();
				bCancellationRequested = true;
			}
			return false;
		}

		BakeWaitSeconds = FPlatformTime::Seconds() - BakeStartedAt;
		Sample(TEXT("bake_terminal"), true);
		if (!PendingOutcome.IsEmpty())
			return Finish(*PendingOutcome, FailureReason);
		if (!Volume->Status.StartsWith(TEXT("Bake saved.")))
			return Finish(TEXT("FAIL"), TEXT("Product Bake finished without saving its generated asset: ") + Volume->Status);

		const double ReadbackStarted = FPlatformTime::Seconds();
		if (!RecordBakeAsset(FailureReason))
			return Finish(TEXT("FAIL"), FailureReason);
		AssetReadbackSeconds = FPlatformTime::Seconds() - ReadbackStarted;
		if (!bCurrentBakeValid)
			return Finish(TEXT("FAIL"), TEXT("Saved Rural acoustic asset failed ValidateCurrentBake."));
		return Finish(TEXT("PASS"), FString::Printf(
			TEXT("One Camera3 %.1f m square region completed product InspectScene, GenerateProbes, Bake, save, and raw-payload readback."),
			RegionHalfExtentCm * 2.0 / 100.0));
	}

	bool RecordBakeAsset(FString& Error)
	{
		UIMAcousticBakeAsset* Asset = IsValid(Volume.Get()) ? Volume->BakedField.Get() : nullptr;
		if (!IsValid(Asset))
		{
			Error = TEXT("Product Bake did not bind its saved UIMAcousticBakeAsset to the transient volume.");
			return false;
		}
		NewAsset = Asset;
		NewAssetPath = Asset->GetPathName();
		NewAssetPackageName = Asset->GetOutermost()->GetName();
		AssetFormatVersion = Asset->FormatVersion;
		SceneFingerprint = Asset->SceneFingerprint;
		MetadataJson = Asset->MetadataJson;
		PackedSceneBytes = Asset->SceneData.Num();
		PackedProbeBytes = Asset->ProbeData.Num();
		PackedSceneSha1 = Sha1Bytes(Asset->SceneData);
		PackedProbeSha1 = Sha1Bytes(Asset->ProbeData);
		if (Asset->WorldPackage != RuralBenchmarkMap || Asset->FormatVersion != UIMAcousticBakeAsset::CurrentFormat
			|| PackedSceneBytes <= 0 || PackedProbeBytes <= 0
			|| Volume->ExportedTriangles != ExpectedTriangles || Volume->GeneratedProbes != ProbeCount)
		{
			Error = TEXT("Saved asset identity, packed payload, or product counts do not match the benchmark inputs.");
			return false;
		}
		if (!ValidateGeneratedPackage(Error)) return false;

		const double PayloadStarted = FPlatformTime::Seconds();
		TArray<uint8> RawScene;
		TArray<uint8> RawProbes;
		FString PayloadFailure;
		if (!Asset->ReadPayload(RuralBenchmarkMap, SceneFingerprint,
				RawScene, RawProbes, PayloadFailure)
			|| RawScene.IsEmpty() || RawProbes.IsEmpty())
		{
			Error = TEXT("Saved packed payload did not decode to non-empty raw SDK bytes: ") + PayloadFailure;
			return false;
		}
		RawSceneBytes = RawScene.Num();
		RawProbeBytes = RawProbes.Num();
		RawSceneSha1 = Sha1Bytes(RawScene);
		RawProbeSha1 = Sha1Bytes(RawProbes);
		PayloadReadSeconds = FPlatformTime::Seconds() - PayloadStarted;

		FString ValidateFailure;
		bCurrentBakeValid = Volume->ValidateCurrentBake(ValidateFailure);
		if (!bCurrentBakeValid)
			Error = TEXT("ValidateCurrentBake failed: ") + ValidateFailure;
		return true;
	}

	bool ValidateGeneratedPackage(FString& Error)
	{
		const FString ExpectedPrefix = FString::Printf(TEXT("/IceMoonAcousticField/Bakes/IM_%s_"),
			*FPackageName::GetShortName(RuralBenchmarkMap));
		const FString ShortPackage = FPackageName::GetShortName(NewAssetPackageName);
		const FString AssetPath = NewAssetPackageName + TEXT(".") + ShortPackage;
		if (!NewAssetPackageName.StartsWith(ExpectedPrefix, ESearchCase::CaseSensitive)
			|| AssetPath != NewAssetPath)
		{
			Error = TEXT("Generated acoustic package does not match the product Rural map/GUID package identity.");
			return false;
		}
		const FString GuidSuffix = ShortPackage.RightChop(ExpectedPrefix.Len()
			- FString(TEXT("/IceMoonAcousticField/Bakes/")).Len());
		if (GuidSuffix.Len() != 32)
		{
			Error = TEXT("Generated acoustic package name does not end with the product's 32-digit GUID.");
			return false;
		}
		for (const TCHAR Digit : GuidSuffix)
			if (!FChar::IsHexDigit(Digit))
			{
				Error = TEXT("Generated acoustic package GUID is not hexadecimal.");
				return false;
			}

		PrimaryUAssetFilename = FPackageName::LongPackageNameToFilename(
			NewAssetPackageName, FPackageName::GetAssetPackageExtension());
		FString CanonicalPrimary = FPaths::ConvertRelativePathToFull(PrimaryUAssetFilename);
		FString CanonicalDirectory = FPaths::ConvertRelativePathToFull(BakeDirectory);
		FPaths::NormalizeFilename(CanonicalPrimary);
		FPaths::NormalizeFilename(CanonicalDirectory);
		if (!FPaths::GetPath(CanonicalPrimary).Equals(CanonicalDirectory, ESearchCase::IgnoreCase)
			|| PreRunBakeFiles.Contains(CanonicalPrimary.ToLower()))
		{
			Error = TEXT("Generated UAsset is outside the exact isolated Bakes directory or existed before the run.");
			return false;
		}

		const FString Pattern = FPaths::Combine(BakeDirectory, ShortPackage + TEXT(".*"));
		TArray<FString> SidecarNames;
		IFileManager::Get().FindFiles(SidecarNames, *Pattern, true, false);
		const TArray<FString> AllowedExtensions = {
			FPackageName::GetAssetPackageExtension(), TEXT(".uexp"), TEXT(".ubulk"),
			TEXT(".uptnl"), TEXT(".m.ubulk"), TEXT(".o.ubulk")
		};
		bool bFoundPrimary = false;
		for (const FString& SidecarName : SidecarNames)
		{
			const FString Suffix = SidecarName.RightChop(ShortPackage.Len());
			const bool bAllowed = AllowedExtensions.ContainsByPredicate([&Suffix](const FString& Extension)
			{
				return Suffix.Equals(Extension, ESearchCase::IgnoreCase);
			});
			if (!bAllowed)
			{
				Error = TEXT("Generated package has an unrecognized sidecar; preserving it: ") + SidecarName;
				return false;
			}
			FString Filename = FPaths::ConvertRelativePathToFull(FPaths::Combine(BakeDirectory, SidecarName));
			FPaths::NormalizeFilename(Filename);
			if (PreRunBakeFiles.Contains(Filename.ToLower()))
			{
				Error = TEXT("Generated package sidecar existed before this run; cleanup is refused: ") + Filename;
				return false;
			}
			FIMRuralPackageFile& Record = GeneratedPackageFiles.AddDefaulted_GetRef();
			Record.Filename = Filename;
			if (!HashFile(Filename, Record.Sha1, Record.Bytes, Error)) return false;
			GeneratedPackageBytes += Record.Bytes;
			if (Filename.Equals(CanonicalPrimary, ESearchCase::IgnoreCase))
			{
				PrimaryUAssetBytes = Record.Bytes;
				PrimaryUAssetSha1 = Record.Sha1;
				bFoundPrimary = true;
			}
		}
		if (!bFoundPrimary || PrimaryUAssetBytes <= 0)
		{
			Error = TEXT("Product Bake reported success but no physical primary .uasset file was found.");
			return false;
		}
		bGeneratedPackageVerified = true;
		return true;
	}

	bool HasExternalPackageReferencers(UPackage* Package, FString& Error) const
	{
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
				Error = TEXT("Generated Rural acoustic package still has an external reference: ")
					+ Referencer->GetPathName();
				return false;
			}
		}
		return true;
	}

	bool CleanupGeneratedPackage(FString& Error)
	{
		if (NewAssetPackageName.IsEmpty()) return true;
		if (!bGeneratedPackageVerified || GeneratedPackageFiles.IsEmpty())
		{
			Error = TEXT("Generated acoustic package failed exact path/GUID/pre-run checks; it was preserved.");
			return false;
		}
		for (const FIMRuralPackageFile& Record : GeneratedPackageFiles)
		{
			FString CurrentSha1;
			int64 CurrentBytes = 0;
			FString HashError;
			if (!HashFile(Record.Filename, CurrentSha1, CurrentBytes, HashError)
				|| CurrentSha1 != Record.Sha1 || CurrentBytes != Record.Bytes
				|| IFileManager::Get().IsReadOnly(*Record.Filename))
			{
				Error = HashError.IsEmpty()
					? TEXT("Generated package changed or became read-only; cleanup refused: ") + Record.Filename
					: HashError;
				return false;
			}
		}

		UPackage* Package = FindPackage(nullptr, *NewAssetPackageName);
		if (!IsValid(Package) || Package->IsDirty())
		{
			Error = TEXT("Generated bake package is missing or dirty; cleanup refused.");
			return false;
		}
		if (!HasExternalPackageReferencers(Package, Error)) return false;
		FAssetRegistryModule::PackageDeleted(Package);
		TArray<UPackage*> PackagesToUnload;
		PackagesToUnload.Add(Package);
		UPackageTools::FUnloadPackageParams Params(PackagesToUnload);
		Params.bUnloadDirtyPackages = false;
		Params.bResetTransBuffer = false;
		if (!UPackageTools::UnloadPackages(Params) || FindPackage(nullptr, *NewAssetPackageName))
		{
			Error = TEXT("Could not unload the exact generated package without resetting the transaction buffer.");
			if (UIMAcousticBakeAsset* StillLoaded = IsValid(Package)
				? FindObject<UIMAcousticBakeAsset>(Package, *FPackageName::GetShortName(NewAssetPackageName))
				: nullptr)
				FAssetRegistryModule::AssetCreated(StillLoaded);
			return false;
		}

		for (FIMRuralPackageFile& Record : GeneratedPackageFiles)
		{
			if (!IFileManager::Get().Delete(*Record.Filename, true, false, true)
				|| IFileManager::Get().FileSize(*Record.Filename) != INDEX_NONE)
			{
				Error = TEXT("Exact generated package deletion failed; path retained for root recovery: ")
					+ Record.Filename;
				return false;
			}
			Record.bDeleted = true;
		}
		bGeneratedPackageDeleted = true;
		return true;
	}

	void RestoreBackdropTags()
	{
		for (FIMRuralBackdropTag& Record : BackdropTags)
		{
			UPrimitiveComponent* Component = Record.Component.Get();
			if (IsValid(Component) && !Record.bTagWasPresent)
				Component->ComponentTags.Remove(FName(IgnoreTag));
			Record.bRestored = !IsValid(Component)
				|| Component->ComponentHasTag(FName(IgnoreTag)) == Record.bTagWasPresent;
		}
	}

	bool Finish(const TCHAR* RequestedOutcome, const FString& Reason)
	{
		if (bFinished) return true;
		if (IsValid(Volume.Get()) && (Volume->Status.StartsWith(TEXT("Baking"))
			|| Volume->Status.StartsWith(TEXT("Cancellation requested"))))
		{
			if (!bCancellationRequested)
			{
				Volume->CancelBake();
				bCancellationRequested = true;
			}
			return false;
		}

		FString FinalOutcome = RequestedOutcome;
		if (!Reason.IsEmpty()) FailureReason = Reason;
		if (RequestedOutcome == FString(TEXT("FAIL"))) bAnyFailure = true;
		const double CleanupStarted = FPlatformTime::Seconds();
		if (bLogCaptureAttached && GLog && LogCapture.IsValid())
		{
			GLog->RemoveOutputDevice(LogCapture.Get());
			bLogCaptureAttached = false;
		}
		if (IsValid(Volume.Get()))
		{
			Volume->BakedField = nullptr;
			Volume->Destroy();
			Volume = nullptr;
		}
		RestoreBackdropTags();
		for (const FIMRuralBackdropTag& Record : BackdropTags)
			if (!Record.bRestored) bAnyFailure = true;

		UPackage* CurrentMapPackage = MapPackage.Get();
		if (CurrentMapPackage && CurrentMapPackage->IsDirty() != bMapDirtyAtBaseline)
		{
			bAnyFailure = true;
			AppendFailure(TEXT("In-memory Rural map dirty state changed; no map save was attempted."));
		}
		FString MapHashError;
		if (!MapSha1Before.IsEmpty()
			&& HashFile(FPackageName::LongPackageNameToFilename(
					RuralBenchmarkMap, FPackageName::GetMapPackageExtension()),
				MapSha1After, MapBytesAfter, MapHashError))
		{
			bMapBytesUnchanged = MapBytesBefore == MapBytesAfter && MapSha1Before == MapSha1After;
		}
		if (!MapHashError.IsEmpty() || !bMapBytesUnchanged)
		{
			bAnyFailure = true;
			AppendFailure(MapHashError.IsEmpty()
				? TEXT("Rural map package bytes changed during the benchmark.")
				: MapHashError);
		}

		FString CleanupError;
		if (NewAssetPackageName.IsEmpty())
			bGeneratedPackageDeleted = true;
		else if (!CleanupGeneratedPackage(CleanupError))
		{
			bAnyFailure = true;
			AppendFailure(CleanupError);
		}
		CleanupSeconds = FPlatformTime::Seconds() - CleanupStarted;
		if (bAnyFailure) FinalOutcome = TEXT("FAIL");
		if (RequestedOutcome == FString(TEXT("INCONCLUSIVE")) && !bAnyFailure)
			FinalOutcome = TEXT("INCONCLUSIVE");

		const bool bReportSaved = WriteReport(FinalOutcome);
		bFinished = true;
		if (!bReportSaved)
		{
			Test->AddError(TEXT("Rural Bake benchmark report could not be saved."));
			UE_LOG(LogTemp, Error, TEXT("IMLogs RuralBakeBenchmark FAIL report_write_failed"));
			return true;
		}
		if (FinalOutcome == TEXT("FAIL"))
			Test->AddError(FString::Printf(TEXT("Rural Bake benchmark failed: %s (report %s)"),
				*FailureReason, *ReportPath));
		else if (FinalOutcome == TEXT("INCONCLUSIVE"))
			Test->AddInfo(FString::Printf(TEXT("Rural Bake benchmark inconclusive: %s (report %s)"),
				*FailureReason, *ReportPath));
		else
			Test->AddInfo(FString::Printf(TEXT("Rural Bake benchmark completed: %s"), *ReportPath));
		UE_LOG(LogTemp, Display, TEXT("IMLogs RuralBakeBenchmark result=%s expected_triangles=%lld actual_triangles=%d probes=%d scene_raw=%d scene_packed=%d probe_raw=%d probe_packed=%d uasset_bytes=%lld report=%s reason=%s"),
			*FinalOutcome, static_cast<long long>(ExpectedTriangles), ActualTriangles, ProbeCount,
			RawSceneBytes, PackedSceneBytes, RawProbeBytes, PackedProbeBytes,
			static_cast<long long>(PrimaryUAssetBytes), *ReportPath, *FailureReason);
		UE_LOG(LogTemp, Display, TEXT("[IM][EDITOR_TEST] RuralBakeBenchmark %s"), *FinalOutcome);
		if (FinalOutcome == TEXT("PASS"))
		{
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralBakeBenchmark PASS"));
			UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS RuralBakeBenchmark"));
		}
		return true;
	}

	void AppendFailure(const FString& Additional)
	{
		if (Additional.IsEmpty()) return;
		if (!FailureReason.IsEmpty()) FailureReason += TEXT(" | ");
		FailureReason += Additional;
	}

	bool WriteReport(const FString& Outcome)
	{
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("im_acoustic_rural_bake_benchmark_v1"));
		Report->SetStringField(TEXT("run_id"), RunId);
		Report->SetStringField(TEXT("result"), Outcome);
		Report->SetStringField(TEXT("failure_reason"), FailureReason);
		Report->SetStringField(TEXT("map_package"), RuralBenchmarkMap);
		Report->SetStringField(TEXT("map_sha1_before"), MapSha1Before);
		Report->SetStringField(TEXT("map_sha1_after"), MapSha1After);
		Report->SetNumberField(TEXT("map_bytes_before"), static_cast<double>(MapBytesBefore));
		Report->SetNumberField(TEXT("map_bytes_after"), static_cast<double>(MapBytesAfter));
		Report->SetBoolField(TEXT("map_package_bytes_unchanged"), bMapBytesUnchanged);
		Report->SetBoolField(TEXT("map_saved_by_test"), false);
		Report->SetBoolField(TEXT("map_dirty_state_restored"), MapPackage.IsValid()
			? MapPackage->IsDirty() == bMapDirtyAtBaseline : true);
		Report->SetNumberField(TEXT("preexisting_bake_volume_count"), PreExistingBakeVolumeCount);
		Report->SetStringField(TEXT("camera_actor_path"), CameraPath);
		Report->SetArrayField(TEXT("camera_location_cm"), VectorJson(CameraLocation));
		Report->SetBoolField(TEXT("backdrop_asset_classification_confirmed"), bBackdropClassificationConfirmed);
		Report->SetStringField(TEXT("backdrop_exclusion_policy"),
			TEXT("Only in-region StaticMeshComponents using the exact RuralSpatialAudit assets SM_SkySphere and SM_Horizon_01 are temporarily tagged IMAcousticIgnore. Their prior tag presence is recorded and restored; no other component is filtered."));
		Report->SetNumberField(TEXT("backdrop_component_count"), BackdropTags.Num());
		Report->SetArrayField(TEXT("region_min_cm"), VectorJson(Region.Min));
		Report->SetArrayField(TEXT("region_max_cm"), VectorJson(Region.Max));
		Report->SetArrayField(TEXT("region_center_cm"), VectorJson(GroundCenter));
		Report->SetArrayField(TEXT("region_extent_cm"), VectorJson(GroundExtent));
		Report->SetStringField(TEXT("region_half_extent_argument"), RequestedRegionHalfExtentArgument);
		Report->SetBoolField(TEXT("region_half_extent_argument_valid"), bRegionHalfExtentArgumentValid);
		Report->SetNumberField(TEXT("region_half_extent_cm"), RegionHalfExtentCm);
		Report->SetArrayField(TEXT("region_xy_dimensions_cm"),
			VectorJson(FVector(RegionHalfExtentCm * 2.0, RegionHalfExtentCm * 2.0, 0.0)));
		Report->SetNumberField(TEXT("region_xy_full_width_cm"), RegionHalfExtentCm * 2.0);
		Report->SetNumberField(TEXT("region_xy_full_width_m"), RegionHalfExtentCm * 2.0 / 100.0);
		Report->SetStringField(TEXT("z_bounds_source"),
			TEXT("Actual Landscape collision height samples within the XY box; min is ground minimum minus 50 cm and max is ground maximum plus 500 cm."));
		Report->SetNumberField(TEXT("ground_sample_min_z_cm"), GroundHeightMinCm);
		Report->SetNumberField(TEXT("ground_sample_max_z_cm"), GroundHeightMaxCm);
		Report->SetNumberField(TEXT("landscape_collision_components"), LandscapeCollisionComponents);
		Report->SetNumberField(TEXT("landscape_selected_quads"), LandscapeSelectedTriangles / 2);
		Report->SetNumberField(TEXT("landscape_selected_triangles_expected"), static_cast<double>(LandscapeSelectedTriangles));
		Report->SetNumberField(TEXT("landscape_full_tile_triangles_if_uncropped"), static_cast<double>(LandscapeFullTileTriangles));
		Report->SetStringField(TEXT("landscape_clip_oracle"), FString::Printf(
			TEXT("Expected local terrain triangles are two per non-hole collision quad whose world-space 3D AABB intersects the %.1f m region. The entire 255x255 tiles are not accepted as regional output."),
			RegionHalfExtentCm * 2.0 / 100.0));
		Report->SetNumberField(TEXT("static_mesh_components_in_region"), InRegionStaticMeshComponents);
		Report->SetNumberField(TEXT("plain_static_mesh_components_in_region"), InRegionPlainStaticMeshComponents);
		Report->SetNumberField(TEXT("ism_components_in_region"), InRegionISMComponents);
		Report->SetNumberField(TEXT("hism_components_in_region"), InRegionHISMComponents);
		Report->SetNumberField(TEXT("spline_mesh_components_in_region"), InRegionSplineMeshComponents);
		Report->SetNumberField(TEXT("ordinary_static_mesh_placements"), InRegionOrdinaryMeshPlacements);
		Report->SetNumberField(TEXT("ism_instances_in_region"), InRegionISMInstances);
		Report->SetNumberField(TEXT("hism_instances_in_region"), InRegionHISMInstances);
		Report->SetNumberField(TEXT("unique_mesh_asset_count"), UniqueMeshPaths.Num());
		WriteSortedPathSet(Report, TEXT("unique_mesh_asset_paths"), UniqueMeshPaths);
		Report->SetNumberField(TEXT("expected_lod0_triangles"), static_cast<double>(ExpectedTriangles));
		Report->SetNumberField(TEXT("actual_product_exported_triangles"), ActualTriangles);
		Report->SetNumberField(TEXT("generated_probe_count"), ProbeCount);
		Report->SetNumberField(TEXT("probe_spacing_cm"), 100.0);
		Report->SetNumberField(TEXT("probe_height_cm"), 150.0);
		Report->SetNumberField(TEXT("provisional_material_mapping_count"), MaterialUses.Num());
		Report->SetStringField(TEXT("material_mapping_quality_label"), TEXT("PROVISIONAL_UNIFORM_TEST_ONLY"));
		Report->SetNumberField(TEXT("provisional_absorption_low_mid_high"), ProvisionalAbsorption);
		Report->SetNumberField(TEXT("provisional_scattering"), ProvisionalScattering);
		TArray<TSharedPtr<FJsonValue>> MaterialMappings;
		TArray<FString> SortedMaterialPaths;
		for (const TPair<UMaterialInterface*, FIMRuralMaterialUse>& Entry : MaterialUses)
			if (IsValid(Entry.Key)) SortedMaterialPaths.Add(Entry.Key->GetPathName());
		SortedMaterialPaths.Sort();
		for (const FString& Path : SortedMaterialPaths)
		{
			UMaterialInterface* Material = nullptr;
			FIMRuralMaterialUse* Use = nullptr;
			for (TPair<UMaterialInterface*, FIMRuralMaterialUse>& Entry : MaterialUses)
				if (IsValid(Entry.Key) && Entry.Key->GetPathName() == Path)
				{
					Material = Entry.Key;
					Use = &Entry.Value;
					break;
				}
			if (!Material || !Use) continue;
			TSharedRef<FJsonObject> Mapping = MakeShared<FJsonObject>();
			Mapping->SetStringField(TEXT("asset_path"), Path);
			Mapping->SetStringField(TEXT("label"), TEXT("PROVISIONAL_UNIFORM_TEST_ONLY"));
			Mapping->SetArrayField(TEXT("absorption_low_mid_high"),
				VectorJson(FVector(ProvisionalAbsorption, ProvisionalAbsorption, ProvisionalAbsorption)));
			Mapping->SetNumberField(TEXT("scattering"), ProvisionalScattering);
			Mapping->SetNumberField(TEXT("static_mesh_section_uses"), Use->StaticMeshSectionUses);
			Mapping->SetNumberField(TEXT("landscape_uses"), Use->LandscapeUses);
			MaterialMappings.Add(MakeShared<FJsonValueObject>(Mapping));
		}
		Report->SetArrayField(TEXT("material_mappings"), MaterialMappings);
		Report->SetNumberField(TEXT("coverage_issue_count"), CoverageIssues.Num());
		TArray<TSharedPtr<FJsonValue>> Issues;
		for (const FString& Issue : CoverageIssues)
			Issues.Add(MakeShared<FJsonValueString>(Issue));
		Report->SetArrayField(TEXT("coverage_issues"), Issues);
		WriteSortedPathSet(Report, TEXT("known_editor_only_or_world_helper_exclusions"), KnownExcludedComponents);
		WriteSortedPathSet(Report, TEXT("verified_backdrop_exclusions"), BackdropExcludedPaths);

		Report->SetBoolField(TEXT("inspect_scene_succeeded"), InspectSceneSeconds >= 0.0 && ActualTriangles > 0);
		Report->SetBoolField(TEXT("generate_probes_succeeded"), GenerateProbesSeconds >= 0.0 && ProbeCount > 0);
		Report->SetBoolField(TEXT("product_bake_saved"), !NewAssetPath.IsEmpty());
		Report->SetStringField(TEXT("bake_asset_path"), NewAssetPath);
		Report->SetStringField(TEXT("bake_asset_package"), NewAssetPackageName);
		Report->SetNumberField(TEXT("bake_asset_format_version"), AssetFormatVersion);
		Report->SetStringField(TEXT("scene_fingerprint"), SceneFingerprint);
		Report->SetNumberField(TEXT("scene_raw_bytes"), RawSceneBytes);
		Report->SetNumberField(TEXT("scene_packed_bytes"), PackedSceneBytes);
		Report->SetStringField(TEXT("scene_raw_sha1"), RawSceneSha1);
		Report->SetStringField(TEXT("scene_packed_sha1"), PackedSceneSha1);
		Report->SetNumberField(TEXT("probe_raw_bytes"), RawProbeBytes);
		Report->SetNumberField(TEXT("probe_packed_bytes"), PackedProbeBytes);
		Report->SetStringField(TEXT("probe_raw_sha1"), RawProbeSha1);
		Report->SetStringField(TEXT("probe_packed_sha1"), PackedProbeSha1);
		Report->SetBoolField(TEXT("read_payload_raw_sdk_bytes_succeeded"), RawSceneBytes > 0 && RawProbeBytes > 0);
		Report->SetBoolField(TEXT("validate_current_bake_succeeded"), bCurrentBakeValid);
		Report->SetStringField(TEXT("metadata_json_recipe"), MetadataJson);
		Report->SetNumberField(TEXT("primary_uasset_bytes"), static_cast<double>(PrimaryUAssetBytes));
		Report->SetNumberField(TEXT("generated_package_total_bytes"), static_cast<double>(GeneratedPackageBytes));
		Report->SetStringField(TEXT("primary_uasset_sha1_before_cleanup"), PrimaryUAssetSha1);
		Report->SetNumberField(TEXT("generated_package_files"), GeneratedPackageFiles.Num());
		Report->SetBoolField(TEXT("generated_package_deleted"), bGeneratedPackageDeleted);
		Report->SetBoolField(TEXT("backdrop_tags_restored"), BackdropTags.Num() > 0
			&& BackdropTags.ContainsByPredicate([](const FIMRuralBackdropTag& Tag) { return !Tag.bRestored; }) == false);

		TArray<TSharedPtr<FJsonValue>> BackdropDetails;
		for (const FIMRuralBackdropTag& Record : BackdropTags)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("component_path"), Record.ComponentPath);
			Item->SetStringField(TEXT("mesh_asset_path"), Record.MeshPath);
			Item->SetStringField(TEXT("reason"), TEXT("verified distant sky/horizon backdrop, excluded only to benchmark local acoustic surfaces"));
			Item->SetBoolField(TEXT("ignore_tag_present_before"), Record.bTagWasPresent);
			Item->SetBoolField(TEXT("ignore_tag_restored"), Record.bRestored);
			BackdropDetails.Add(MakeShared<FJsonValueObject>(Item));
		}
		Report->SetArrayField(TEXT("backdrop_components"), BackdropDetails);

		Report->SetNumberField(TEXT("map_load_seconds"), MapLoadSeconds);
		Report->SetNumberField(TEXT("terrain_height_discovery_seconds"), TerrainDiscoverySeconds);
		Report->SetNumberField(TEXT("coverage_audit_seconds"), CoverageAuditSeconds);
		Report->SetNumberField(TEXT("inspect_scene_seconds"), InspectSceneSeconds);
		Report->SetNumberField(TEXT("generate_probes_seconds"), GenerateProbesSeconds);
		Report->SetNumberField(TEXT("bake_call_seconds"), BakeCallSeconds);
		Report->SetNumberField(TEXT("bake_wait_and_product_save_seconds"), BakeWaitSeconds);
		Report->SetNumberField(TEXT("raw_payload_read_seconds"), PayloadReadSeconds);
		Report->SetNumberField(TEXT("asset_readback_and_validate_seconds"), AssetReadbackSeconds);
		Report->SetNumberField(TEXT("cleanup_seconds"), CleanupSeconds);
		Report->SetNumberField(TEXT("total_seconds"), FPlatformTime::Seconds() - StartedAt);
		Report->SetStringField(TEXT("bake_timing_semantics"),
			TEXT("Bake wait includes the asynchronous SDK Bake and product package encode/save; Bake call and raw payload readback are timed separately. Product AcousticBakeAssetSave timing is preserved in the captured log lines."));

		uint64 SampledPeakWorkingSet = 0;
		uint64 SampledPeakCommit = 0;
		uint64 EditorLifetimePeakWorkingSet = 0;
		uint64 EditorLifetimePeakCommit = 0;
		uint64 BakeWindowSampledPeakWorkingSet = 0;
		uint64 BakeWindowSampledPeakCommit = 0;
		TArray<TSharedPtr<FJsonValue>> Samples;
		for (const FIMRuralMemorySample& SampleEntry : MemorySamples)
		{
			SampledPeakWorkingSet = FMath::Max(SampledPeakWorkingSet, SampleEntry.UsedPhysical);
			SampledPeakCommit = FMath::Max(SampledPeakCommit, SampleEntry.UsedVirtual);
			EditorLifetimePeakWorkingSet = FMath::Max(EditorLifetimePeakWorkingSet, SampleEntry.PeakUsedPhysical);
			EditorLifetimePeakCommit = FMath::Max(EditorLifetimePeakCommit, SampleEntry.PeakUsedVirtual);
			if (SampleEntry.Stage.StartsWith(TEXT("bake_")))
			{
				BakeWindowSampledPeakWorkingSet = FMath::Max(BakeWindowSampledPeakWorkingSet, SampleEntry.UsedPhysical);
				BakeWindowSampledPeakCommit = FMath::Max(BakeWindowSampledPeakCommit, SampleEntry.UsedVirtual);
			}
			TSharedRef<FJsonObject> SampleJson = MakeShared<FJsonObject>();
			SampleJson->SetStringField(TEXT("stage"), SampleEntry.Stage);
			SampleJson->SetNumberField(TEXT("elapsed_seconds"), SampleEntry.ElapsedSeconds);
			SampleJson->SetNumberField(TEXT("used_physical_bytes"), static_cast<double>(SampleEntry.UsedPhysical));
			SampleJson->SetNumberField(TEXT("peak_used_physical_bytes_cumulative"), static_cast<double>(SampleEntry.PeakUsedPhysical));
			SampleJson->SetNumberField(TEXT("used_virtual_bytes"), static_cast<double>(SampleEntry.UsedVirtual));
			SampleJson->SetNumberField(TEXT("peak_used_virtual_bytes_cumulative"), static_cast<double>(SampleEntry.PeakUsedVirtual));
			SampleJson->SetNumberField(TEXT("available_physical_bytes"), static_cast<double>(SampleEntry.AvailablePhysical));
			Samples.Add(MakeShared<FJsonValueObject>(SampleJson));
		}
		Report->SetNumberField(TEXT("sampled_peak_working_set_bytes"), static_cast<double>(SampledPeakWorkingSet));
		Report->SetNumberField(TEXT("sampled_peak_commit_bytes"), static_cast<double>(SampledPeakCommit));
		Report->SetNumberField(TEXT("editor_lifetime_peak_working_set_bytes_cumulative"), static_cast<double>(EditorLifetimePeakWorkingSet));
		Report->SetNumberField(TEXT("editor_lifetime_peak_commit_bytes_cumulative"), static_cast<double>(EditorLifetimePeakCommit));
		Report->SetNumberField(TEXT("bake_window_sampled_peak_working_set_bytes"), static_cast<double>(BakeWindowSampledPeakWorkingSet));
		Report->SetNumberField(TEXT("bake_window_sampled_peak_commit_bytes"), static_cast<double>(BakeWindowSampledPeakCommit));
		Report->SetStringField(TEXT("memory_semantics"),
			TEXT("UsedPhysical/UsedVirtual are process snapshots. PeakUsedPhysical/PeakUsedVirtual are cumulative Editor-lifetime peaks and may predate this run. Bake-window values are sampled current usage only, not a guaranteed instantaneous peak."));
		Report->SetArrayField(TEXT("memory_samples"), Samples);

		TArray<FString> LogLines = LogCapture.IsValid() ? LogCapture->Snapshot() : TArray<FString>();
		const int32 LogLimit = 128;
		TArray<TSharedPtr<FJsonValue>> Logs;
		for (int32 Index = 0; Index < FMath::Min(LogLines.Num(), LogLimit); ++Index)
			Logs.Add(MakeShared<FJsonValueString>(LogLines[Index]));
		Report->SetArrayField(TEXT("product_timing_log_lines"), Logs);
		Report->SetNumberField(TEXT("product_timing_log_line_count"), LogLines.Num());
		Report->SetBoolField(TEXT("product_timing_logs_truncated"), LogLines.Num() > LogLimit);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
		Writer->Close();
		ReportPath = FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("AcousticV2/RuralBakeBenchmark"), RunId, TEXT("report.json"));
		return bSerialized
			&& IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true)
			&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FAutomationTestBase* Test = nullptr;
	FString RunId;
	FString StageOutcome;
	FString PendingOutcome;
	FString FailureReason;
	FString ReportPath;
	FString CameraPath;
	FString MapSha1Before;
	FString MapSha1After;
	FString BakeDirectory;
	FString NewAssetPath;
	FString NewAssetPackageName;
	FString PrimaryUAssetFilename;
	FString PrimaryUAssetSha1;
	FString SceneFingerprint;
	FString MetadataJson;
	FString RequestedRegionHalfExtentArgument;
	FString PackedSceneSha1;
	FString PackedProbeSha1;
	FString RawSceneSha1;
	FString RawProbeSha1;
	TWeakObjectPtr<UPackage> MapPackage;
	TWeakObjectPtr<UIMAcousticBakeAsset> NewAsset;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TUniquePtr<FIMRuralBakeLogCapture> LogCapture;
	TArray<FIMRuralLandscapeTile> LandscapeTiles;
	TArray<FIMRuralBackdropTag> BackdropTags;
	TArray<FIMRuralPackageFile> GeneratedPackageFiles;
	TArray<FIMRuralMemorySample> MemorySamples;
	TSet<FString> PreRunBakeFiles;
	TSet<FString> CoverageIssuesAsSet;
	TSet<FString> CoverageIssuePaths;
	TSet<FString> KnownExcludedComponents;
	TSet<FString> BackdropExcludedPaths;
	TSet<FString> UniqueMeshPaths;
	TMap<UMaterialInterface*, FIMRuralMaterialUse> MaterialUses;
	FIMAcousticGrassGenerationState GrassState;
	FIMAcousticGrassCapture GrassCapture;
	TArray<FString> CoverageIssues;
	FVector CameraLocation = FVector::ZeroVector;
	FVector GroundCenter = FVector::ZeroVector;
	FVector GroundExtent = FVector::ZeroVector;
	FBox Region = FBox(ForceInit);
	double StartedAt = 0.0;
	double LastMemorySampleAt = 0.0;
	double MapLoadSeconds = 0.0;
	double TerrainDiscoverySeconds = 0.0;
	double CoverageAuditSeconds = 0.0;
	double InspectSceneSeconds = -1.0;
	double GenerateProbesSeconds = -1.0;
	double BakeStartedAt = 0.0;
	double BakeCallSeconds = 0.0;
	double BakeWaitSeconds = 0.0;
	double PayloadReadSeconds = 0.0;
	double AssetReadbackSeconds = 0.0;
	double CleanupSeconds = 0.0;
	double GroundHeightMinCm = 0.0;
	double GroundHeightMaxCm = 0.0;
	double RegionMinZ = 0.0;
	double RegionMaxZ = 0.0;
	double RegionHalfExtentCm = DefaultRegionHalfExtentCm;
	int64 MapBytesBefore = 0;
	int64 MapBytesAfter = 0;
	int64 ExpectedTriangles = 0;
	int64 LandscapeSelectedTriangles = 0;
	int64 LandscapeFullTileTriangles = 0;
	int64 GeneratedPackageBytes = 0;
	int64 PrimaryUAssetBytes = 0;
	int32 PreExistingBakeVolumeCount = 0;
	int32 LandscapeCollisionComponents = 0;
	int32 InRegionStaticMeshComponents = 0;
	int32 InRegionPlainStaticMeshComponents = 0;
	int32 InRegionISMComponents = 0;
	int32 InRegionHISMComponents = 0;
	int32 InRegionSplineMeshComponents = 0;
	int32 InRegionOrdinaryMeshPlacements = 0;
	int32 InRegionISMInstances = 0;
	int32 InRegionHISMInstances = 0;
	int32 ActualTriangles = 0;
	int32 ProbeCount = 0;
	int32 AssetFormatVersion = 0;
	int32 RawSceneBytes = 0;
	int32 RawProbeBytes = 0;
	int32 PackedSceneBytes = 0;
	int32 PackedProbeBytes = 0;
	int32 Stage = 0;
	bool bMapDirtyAtBaseline = false;
	bool bMapBytesUnchanged = false;
	bool bCoverageComplete = true;
	bool bBackdropClassificationConfirmed = false;
	bool bLogCaptureAttached = false;
	bool bCancellationRequested = false;
	bool bCurrentBakeValid = false;
	bool bGeneratedPackageVerified = false;
	bool bGeneratedPackageDeleted = false;
	bool bFinished = false;
	bool bAnyFailure = false;
	bool bRegionHalfExtentArgumentValid = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralBakeBenchmarkTest,
	"IceMoon.AcousticField.G0.RuralBakeBenchmark",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralBakeBenchmarkTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticRuralBakeBenchmarkTestPrivate::FIMAcousticRuralBakeBenchmarkCommand(this));
	return true;
}

#endif
