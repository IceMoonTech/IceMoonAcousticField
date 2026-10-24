#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"

namespace IMAcousticRuralMapAuditTestPrivate
{
constexpr TCHAR RuralMapPath[] = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr int32 MaxComponentDetails = 128;
constexpr int32 MaxPlayerStartDetails = 128;
constexpr int32 MaxDependencyLogDetails = 32;

struct FIMRuralG0MeshInfo
{
	const UStaticMesh* Mesh = nullptr;
	int64 ComponentReferences = 0;
	int64 RepresentedInstances = 0;
	int64 LOD0Vertices = 0;
	int64 LOD0Triangles = 0;
	bool bHasLOD0 = false;
	bool bPositionDataReadable = false;
	bool bIndexDataReadable = false;
	bool bTriangleCountKnown = false;
};

class FIMRuralG0LoadLogObserver final : public FOutputDevice
{
public:
	void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (!Text || Verbosity > ELogVerbosity::Warning)
		{
			return;
		}

		const FString Message(Text);
		const bool bMissingDependency =
			Message.Contains(TEXT("couldn't find file for package"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("could not find file for package"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("failed to load package"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("failed to load object"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("failed to find object"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("couldn't find object"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("could not find object"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("can't find file"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("cannot find file"), ESearchCase::IgnoreCase)
			|| Message.Contains(TEXT("missing dependency"), ESearchCase::IgnoreCase);
		if (!bMissingDependency)
		{
			return;
		}

		FScopeLock Lock(&Mutex);
		++TotalHits;
		if (Lines.Num() < MaxDependencyLogDetails)
		{
			Lines.Add(FString::Printf(TEXT("%s: %s"), *Category.ToString(), Text));
		}
	}

	bool CanBeUsedOnAnyThread() const override { return true; }

	void Snapshot(TArray<FString>& OutLines, int32& OutTotalHits) const
	{
		FScopeLock Lock(&Mutex);
		OutLines = Lines;
		OutTotalHits = TotalHits;
	}

private:
	mutable FCriticalSection Mutex;
	TArray<FString> Lines;
	int32 TotalHits = 0;
};

static TSharedRef<FJsonObject> MakeVectorJson(const FVector& Value)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	Object->SetNumberField(TEXT("z"), Value.Z);
	return Object;
}

static TSharedRef<FJsonObject> MakeBoundsJson(const FBox& Bounds)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("valid"), Bounds.IsValid != 0);
	if (Bounds.IsValid)
	{
		Object->SetObjectField(TEXT("min"), MakeVectorJson(Bounds.Min));
		Object->SetObjectField(TEXT("max"), MakeVectorJson(Bounds.Max));
		Object->SetObjectField(TEXT("center"), MakeVectorJson(Bounds.GetCenter()));
		Object->SetObjectField(TEXT("extent"), MakeVectorJson(Bounds.GetExtent()));
	}
	return Object;
}

static TArray<TSharedPtr<FJsonValue>> MakeCountArray(const TMap<FString, int64>& Counts)
{
	TArray<FString> Keys;
	Counts.GetKeys(Keys);
	Keys.Sort();

	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Keys.Num());
	for (const FString& Key : Keys)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_path"), Key);
		Entry->SetNumberField(TEXT("count"), static_cast<double>(Counts.FindRef(Key)));
		Result.Add(MakeShared<FJsonValueObject>(Entry));
	}
	return Result;
}

static FString MobilityName(const EComponentMobility::Type Mobility)
{
	switch (Mobility)
	{
	case EComponentMobility::Static: return TEXT("Static");
	case EComponentMobility::Stationary: return TEXT("Stationary");
	case EComponentMobility::Movable: return TEXT("Movable");
	default: return TEXT("Unknown");
	}
}

static bool IsLandscapeComponentClass(const UClass* Class)
{
	for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
	{
		const UPackage* Package = Current->GetOutermost();
		if (Package && Package->GetName() == TEXT("/Script/Landscape"))
		{
			return true;
		}
	}
	return false;
}

static void AddMeshLOD0Data(const UStaticMesh* Mesh, FIMRuralG0MeshInfo& Info)
{
	Info.Mesh = Mesh;
	if (!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		return;
	}

	Info.bHasLOD0 = true;
	const FStaticMeshLODResources& LOD0 = Mesh->GetRenderData()->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD0.VertexBuffers.PositionVertexBuffer;
	const auto IndexView = LOD0.IndexBuffer.GetArrayView();
	Info.LOD0Vertices = static_cast<int64>(Positions.GetNumVertices());
	Info.LOD0Triangles = IndexView.Num() > 0 ? static_cast<int64>(IndexView.Num() / 3) : 0;
	Info.bPositionDataReadable = Positions.GetNumVertices() > 0 && Positions.GetVertexData() != nullptr;
	Info.bIndexDataReadable = IndexView.Num() > 0;
	Info.bTriangleCountKnown = Info.bIndexDataReadable;
}

static TSharedRef<FJsonObject> MakeMeshJson(const FString& MeshPath, const FIMRuralG0MeshInfo& Info)
{
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("asset_path"), MeshPath);
	Entry->SetStringField(TEXT("class_path"), Info.Mesh ? Info.Mesh->GetClass()->GetPathName() : TEXT("UNKNOWN"));
	Entry->SetStringField(TEXT("package_path"), Info.Mesh && Info.Mesh->GetOutermost()
		? Info.Mesh->GetOutermost()->GetName() : TEXT("UNKNOWN"));
	Entry->SetNumberField(TEXT("component_references"), static_cast<double>(Info.ComponentReferences));
	Entry->SetNumberField(TEXT("represented_instances"), static_cast<double>(Info.RepresentedInstances));
	Entry->SetBoolField(TEXT("lod0_present"), Info.bHasLOD0);
	if (Info.bHasLOD0)
	{
		Entry->SetNumberField(TEXT("lod0_vertex_count"), static_cast<double>(Info.LOD0Vertices));
		if (Info.bTriangleCountKnown)
		{
			Entry->SetNumberField(TEXT("lod0_triangle_count"), static_cast<double>(Info.LOD0Triangles));
		}
		else
		{
			Entry->SetStringField(TEXT("lod0_triangle_count"), TEXT("UNKNOWN_CPU_INDEX_DATA_UNREADABLE"));
		}
	}
	else
	{
		Entry->SetStringField(TEXT("lod0_vertex_count"), TEXT("UNKNOWN_NO_RENDER_DATA"));
		Entry->SetStringField(TEXT("lod0_triangle_count"), TEXT("UNKNOWN_NO_RENDER_DATA"));
	}
	Entry->SetBoolField(TEXT("lod0_position_data_readable"), Info.bPositionDataReadable);
	Entry->SetBoolField(TEXT("lod0_index_data_readable"), Info.bIndexDataReadable);
	Entry->SetBoolField(TEXT("lod0_capture_cpu_geometry_readable"),
		Info.bPositionDataReadable && Info.bIndexDataReadable);
	return Entry;
}

static void SetMemoryJson(TSharedRef<FJsonObject> Report,
	const FPlatformMemoryStats& Before, const FPlatformMemoryStats& After)
{
	constexpr double BytesPerMiB = 1024.0 * 1024.0;
	TSharedRef<FJsonObject> Memory = MakeShared<FJsonObject>();
	Memory->SetNumberField(TEXT("used_physical_mib_before"), Before.UsedPhysical / BytesPerMiB);
	Memory->SetNumberField(TEXT("used_physical_mib_after"), After.UsedPhysical / BytesPerMiB);
	Memory->SetNumberField(TEXT("available_physical_mib_before"), Before.AvailablePhysical / BytesPerMiB);
	Memory->SetNumberField(TEXT("available_physical_mib_after"), After.AvailablePhysical / BytesPerMiB);
	Memory->SetNumberField(TEXT("used_virtual_mib_before"), Before.UsedVirtual / BytesPerMiB);
	Memory->SetNumberField(TEXT("used_virtual_mib_after"), After.UsedVirtual / BytesPerMiB);
	Memory->SetNumberField(TEXT("peak_used_physical_mib_before"), Before.PeakUsedPhysical / BytesPerMiB);
	Memory->SetNumberField(TEXT("peak_used_physical_mib_after"), After.PeakUsedPhysical / BytesPerMiB);
	Memory->SetNumberField(TEXT("peak_used_virtual_mib_before"), Before.PeakUsedVirtual / BytesPerMiB);
	Memory->SetNumberField(TEXT("peak_used_virtual_mib_after"), After.PeakUsedVirtual / BytesPerMiB);
	Memory->SetStringField(TEXT("peak_note"), TEXT("Process-lifetime peak; it may include earlier Editor work."));
	Report->SetObjectField(TEXT("memory"), Memory);
}

static bool BuildRuralG0Report(const FString& RunId,
	const bool bLoadMapReturned, const double LoadMs, const FPlatformMemoryStats& MemoryBefore,
	const bool bLoadLogObserverAvailable,
	const TArray<FString>& MissingDependencyLogLines, const int32 MissingDependencyLogHitCount,
	TSharedRef<FJsonObject> Report, FString& OutTerminalStatus, FString& OutFailureReason)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FString LoadedWorldPackage = World && World->GetOutermost()
		? World->GetOutermost()->GetName() : FString();
	const bool bTargetWorldLoaded = LoadedWorldPackage == RuralMapPath;

	int64 ActorCount = 0;
	int64 PrimitiveComponentCount = 0;
	int64 StaticMeshComponentCount = 0;
	int64 PlainStaticMeshComponentCount = 0;
	int64 InstancedStaticMeshComponentCount = 0;
	int64 HierarchicalInstancedStaticMeshComponentCount = 0;
	int64 PlainStaticMeshInstances = 0;
	int64 InstancedStaticMeshInstances = 0;
	int64 HierarchicalInstancedStaticMeshInstances = 0;
	int64 LandscapeComponentCount = 0;
	int64 NonMeshPrimitiveComponentCount = 0;
	int64 NonStaticPrimitiveComponentCount = 0;
	int64 MeshComponentsWithoutAsset = 0;
	int64 TotalMaterialSlots = 0;
	int64 ValidMaterialSlots = 0;
	int64 InvalidMaterialSlots = 0;
	int64 StaticPrimitiveCount = 0;
	int64 StationaryPrimitiveCount = 0;
	int64 MovablePrimitiveCount = 0;
	int64 StaticMeshStaticCount = 0;
	int64 StaticMeshStationaryCount = 0;
	int64 StaticMeshMovableCount = 0;
	int64 LOD0VertexTotal = 0;
	int64 LOD0TriangleTotal = 0;
	int64 LOD0TriangleCountKnownMeshCount = 0;
	int64 LOD0UnreadableMeshCount = 0;
	int64 PlayerStartCount = 0;
	int64 ComponentDetailTotal = 0;

	TMap<FString, int64> ActorClassCounts;
	TMap<FString, int64> PrimitiveClassCounts;
	TMap<FString, int64> PrimitiveMobilityCounts;
	TMap<FString, int64> StaticMeshMobilityCounts;
	TMap<FString, FIMRuralG0MeshInfo> UniqueMeshes;
	TSet<FString> ValidMaterialPaths;
	TArray<TSharedPtr<FJsonValue>> ComponentDetails;
	TArray<TSharedPtr<FJsonValue>> PlayerStarts;
	FBox PrimitiveBounds(ForceInit);
	FBox PlayerStartBounds(ForceInit);

	if (bTargetWorldLoaded)
	{
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			AActor* Actor = *ActorIt;
			if (!IsValid(Actor))
			{
				continue;
			}

			++ActorCount;
			++ActorClassCounts.FindOrAdd(Actor->GetClass()->GetPathName());

			if (APlayerStart* PlayerStart = Cast<APlayerStart>(Actor))
			{
				++PlayerStartCount;
				PlayerStartBounds += PlayerStart->GetActorLocation();
				if (PlayerStarts.Num() < MaxPlayerStartDetails)
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("actor_path"), PlayerStart->GetPathName());
					Entry->SetObjectField(TEXT("location"), MakeVectorJson(PlayerStart->GetActorLocation()));
					PlayerStarts.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}

			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
				if (!IsValid(Primitive))
				{
					continue;
				}

				++PrimitiveComponentCount;
				++ComponentDetailTotal;
				const FString ComponentClassPath = Primitive->GetClass()->GetPathName();
				++PrimitiveClassCounts.FindOrAdd(ComponentClassPath);
				const FString Mobility = MobilityName(Primitive->Mobility);
				++PrimitiveMobilityCounts.FindOrAdd(Mobility);

				if (Primitive->Mobility == EComponentMobility::Static)
				{
					++StaticPrimitiveCount;
				}
				else if (Primitive->Mobility == EComponentMobility::Stationary)
				{
					++StationaryPrimitiveCount;
					++NonStaticPrimitiveComponentCount;
				}
				else if (Primitive->Mobility == EComponentMobility::Movable)
				{
					++MovablePrimitiveCount;
					++NonStaticPrimitiveComponentCount;
				}

				if (IsLandscapeComponentClass(Primitive->GetClass()))
				{
					++LandscapeComponentCount;
				}

				const FBox ComponentBox = Primitive->Bounds.GetBox();
				if (ComponentBox.IsValid)
				{
					PrimitiveBounds += ComponentBox;
				}

				UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Primitive);
				UStaticMesh* Mesh = MeshComponent ? MeshComponent->GetStaticMesh() : nullptr;
				if (!MeshComponent)
				{
					++NonMeshPrimitiveComponentCount;
				}
				else
				{
					++StaticMeshComponentCount;
					++StaticMeshMobilityCounts.FindOrAdd(Mobility);
					if (Primitive->Mobility == EComponentMobility::Static)
					{
						++StaticMeshStaticCount;
					}
					else if (Primitive->Mobility == EComponentMobility::Stationary)
					{
						++StaticMeshStationaryCount;
					}
					else if (Primitive->Mobility == EComponentMobility::Movable)
					{
						++StaticMeshMovableCount;
					}

					int64 RepresentedInstances = 1;
					if (UHierarchicalInstancedStaticMeshComponent* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>(MeshComponent))
					{
						++HierarchicalInstancedStaticMeshComponentCount;
						RepresentedInstances = HISM->GetInstanceCount();
						HierarchicalInstancedStaticMeshInstances += RepresentedInstances;
					}
					else if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(MeshComponent))
					{
						++InstancedStaticMeshComponentCount;
						RepresentedInstances = ISM->GetInstanceCount();
						InstancedStaticMeshInstances += RepresentedInstances;
					}
					else
					{
						++PlainStaticMeshComponentCount;
						++PlainStaticMeshInstances;
					}

					if (!Mesh)
					{
						++MeshComponentsWithoutAsset;
					}
					else
					{
						const FString MeshPath = Mesh->GetPathName();
						FIMRuralG0MeshInfo& MeshInfo = UniqueMeshes.FindOrAdd(MeshPath);
						if (MeshInfo.ComponentReferences == 0)
						{
							AddMeshLOD0Data(Mesh, MeshInfo);
						}
						++MeshInfo.ComponentReferences;
						MeshInfo.RepresentedInstances += RepresentedInstances;
					}

					const int32 MaterialCount = MeshComponent->GetNumMaterials();
					TotalMaterialSlots += MaterialCount;
					for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
					{
						UMaterialInterface* Material = MeshComponent->GetMaterial(MaterialIndex);
						if (IsValid(Material))
						{
							++ValidMaterialSlots;
							ValidMaterialPaths.Add(Material->GetPathName());
						}
						else
						{
							++InvalidMaterialSlots;
						}
					}
				}

				if (ComponentDetails.Num() < MaxComponentDetails)
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("actor_path"), Actor->GetPathName());
					Entry->SetStringField(TEXT("component_path"), Primitive->GetPathName());
					Entry->SetStringField(TEXT("class_path"), ComponentClassPath);
					Entry->SetStringField(TEXT("mobility"), Mobility);
					Entry->SetObjectField(TEXT("bounds"), MakeBoundsJson(ComponentBox));
					if (MeshComponent)
					{
						Entry->SetStringField(TEXT("static_mesh_path"), Mesh ? Mesh->GetPathName() : TEXT("UNKNOWN_NO_ASSIGNED_MESH"));
					}
					ComponentDetails.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}

		for (const TPair<FString, FIMRuralG0MeshInfo>& Pair : UniqueMeshes)
		{
			const FIMRuralG0MeshInfo& Info = Pair.Value;
			if (Info.bHasLOD0)
			{
				LOD0VertexTotal += Info.LOD0Vertices;
				if (Info.bTriangleCountKnown)
				{
					LOD0TriangleTotal += Info.LOD0Triangles;
					++LOD0TriangleCountKnownMeshCount;
				}
				if (!(Info.bPositionDataReadable && Info.bIndexDataReadable))
				{
					++LOD0UnreadableMeshCount;
				}
			}
			else
			{
				++LOD0UnreadableMeshCount;
			}
		}
	}

	const int64 TotalUniqueMeshCount = UniqueMeshes.Num();
	const int64 TotalStaticMeshInstances = PlainStaticMeshInstances
		+ InstancedStaticMeshInstances + HierarchicalInstancedStaticMeshInstances;
	const int64 MissingDependencyMeshEvidence = MeshComponentsWithoutAsset;
	const bool bHasDependencyFailure = MissingDependencyLogHitCount > 0 || MissingDependencyMeshEvidence > 0;
	const bool bHasCaptureCoverageGap = LandscapeComponentCount > 0
		|| NonMeshPrimitiveComponentCount > 0 || NonStaticPrimitiveComponentCount > 0
		|| LOD0UnreadableMeshCount > 0 || InvalidMaterialSlots > 0 || !bLoadLogObserverAvailable;

	OutTerminalStatus = TEXT("PASS");
	OutFailureReason.Reset();
	if (!bLoadMapReturned || !bTargetWorldLoaded)
	{
		OutTerminalStatus = TEXT("FAIL");
		OutFailureReason = FString::Printf(TEXT("Target map load/identity failed: load_returned=%d loaded_world=%s expected=%s."),
			bLoadMapReturned ? 1 : 0, *LoadedWorldPackage, RuralMapPath);
	}
	else if (bHasDependencyFailure)
	{
		OutTerminalStatus = TEXT("FAIL");
		OutFailureReason = FString::Printf(TEXT("Map load produced missing-dependency evidence: log_hits=%d static_mesh_components_without_asset=%lld."),
			MissingDependencyLogHitCount, MissingDependencyMeshEvidence);
	}
	else if (bHasCaptureCoverageGap)
	{
		OutTerminalStatus = TEXT("INCONCLUSIVE");
		OutFailureReason = FString::Printf(TEXT("Map inventory is complete, but CaptureScene coverage/load diagnostics need review: landscape=%lld non_mesh_primitive=%lld non_static_primitive=%lld unreadable_lod0_mesh=%lld invalid_material_slots=%lld load_log_observer_available=%d."),
			LandscapeComponentCount, NonMeshPrimitiveComponentCount, NonStaticPrimitiveComponentCount,
			LOD0UnreadableMeshCount, InvalidMaterialSlots, bLoadLogObserverAvailable ? 1 : 0);
	}

	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetStringField(TEXT("requested_map_package"), RuralMapPath);
	Report->SetStringField(TEXT("loaded_world_package"), LoadedWorldPackage.IsEmpty() ? TEXT("UNKNOWN") : LoadedWorldPackage);
	Report->SetBoolField(TEXT("load_map_returned_success"), bLoadMapReturned);
	Report->SetBoolField(TEXT("target_world_identity_matches"), bTargetWorldLoaded);
	Report->SetNumberField(TEXT("map_load_ms"), LoadMs);
	Report->SetStringField(TEXT("map_package_filename"), FPackageName::LongPackageNameToFilename(
		RuralMapPath, FPackageName::GetMapPackageExtension()));
	Report->SetStringField(TEXT("map_package_sha256"), TEXT("UNKNOWN_EXTERNAL_VERIFICATION_REQUIRED"));
	Report->SetStringField(TEXT("terminal_status"), OutTerminalStatus);
	Report->SetStringField(TEXT("terminal_reason"), OutFailureReason);
	Report->SetStringField(TEXT("capture_scene_coverage_rule"),
		TEXT("AIMAcousticBakeVolume::CaptureScene accepts UStaticMeshComponent; non-mesh primitive components are excluded, and non-static primitives are skipped. This G0 audit does not call CaptureScene."));

	TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("actor_count"), static_cast<double>(ActorCount));
	Counts->SetNumberField(TEXT("primitive_component_count"), static_cast<double>(PrimitiveComponentCount));
	Counts->SetNumberField(TEXT("static_mesh_component_count"), static_cast<double>(StaticMeshComponentCount));
	Counts->SetNumberField(TEXT("plain_static_mesh_component_count"), static_cast<double>(PlainStaticMeshComponentCount));
	Counts->SetNumberField(TEXT("instanced_static_mesh_component_count"), static_cast<double>(InstancedStaticMeshComponentCount));
	Counts->SetNumberField(TEXT("hierarchical_instanced_static_mesh_component_count"), static_cast<double>(HierarchicalInstancedStaticMeshComponentCount));
	Counts->SetNumberField(TEXT("plain_static_mesh_represented_instance_count"), static_cast<double>(PlainStaticMeshInstances));
	Counts->SetNumberField(TEXT("instanced_static_mesh_instance_count"), static_cast<double>(InstancedStaticMeshInstances));
	Counts->SetNumberField(TEXT("hierarchical_instanced_static_mesh_instance_count"), static_cast<double>(HierarchicalInstancedStaticMeshInstances));
	Counts->SetNumberField(TEXT("static_mesh_represented_instance_total"), static_cast<double>(TotalStaticMeshInstances));
	Counts->SetNumberField(TEXT("unique_static_mesh_asset_count"), static_cast<double>(TotalUniqueMeshCount));
	Counts->SetNumberField(TEXT("landscape_component_count"), static_cast<double>(LandscapeComponentCount));
	Counts->SetNumberField(TEXT("non_mesh_primitive_component_count"), static_cast<double>(NonMeshPrimitiveComponentCount));
	Counts->SetNumberField(TEXT("non_static_primitive_component_count"), static_cast<double>(NonStaticPrimitiveComponentCount));
	Counts->SetNumberField(TEXT("static_primitive_component_count"), static_cast<double>(StaticPrimitiveCount));
	Counts->SetNumberField(TEXT("stationary_primitive_component_count"), static_cast<double>(StationaryPrimitiveCount));
	Counts->SetNumberField(TEXT("movable_primitive_component_count"), static_cast<double>(MovablePrimitiveCount));
	Counts->SetNumberField(TEXT("static_static_mesh_component_count"), static_cast<double>(StaticMeshStaticCount));
	Counts->SetNumberField(TEXT("stationary_static_mesh_component_count"), static_cast<double>(StaticMeshStationaryCount));
	Counts->SetNumberField(TEXT("movable_static_mesh_component_count"), static_cast<double>(StaticMeshMovableCount));
	Counts->SetNumberField(TEXT("mesh_components_without_asset_count"), static_cast<double>(MeshComponentsWithoutAsset));
	Counts->SetNumberField(TEXT("mesh_material_slot_count"), static_cast<double>(TotalMaterialSlots));
	Counts->SetNumberField(TEXT("valid_mesh_material_slot_count"), static_cast<double>(ValidMaterialSlots));
	Counts->SetNumberField(TEXT("invalid_mesh_material_slot_count"), static_cast<double>(InvalidMaterialSlots));
	Counts->SetNumberField(TEXT("unique_valid_material_asset_count"), static_cast<double>(ValidMaterialPaths.Num()));
	Counts->SetNumberField(TEXT("lod0_unique_mesh_vertex_total"), static_cast<double>(LOD0VertexTotal));
	Counts->SetNumberField(TEXT("lod0_unique_mesh_triangle_total_known"), static_cast<double>(LOD0TriangleTotal));
	Counts->SetNumberField(TEXT("lod0_triangle_count_known_mesh_count"), static_cast<double>(LOD0TriangleCountKnownMeshCount));
	Counts->SetNumberField(TEXT("lod0_mesh_with_unreadable_cpu_data_count"), static_cast<double>(LOD0UnreadableMeshCount));
	Counts->SetNumberField(TEXT("player_start_count"), static_cast<double>(PlayerStartCount));
	Report->SetObjectField(TEXT("counts"), Counts);

	Report->SetArrayField(TEXT("actor_class_counts"), MakeCountArray(ActorClassCounts));
	Report->SetArrayField(TEXT("primitive_component_class_counts"), MakeCountArray(PrimitiveClassCounts));
	Report->SetArrayField(TEXT("primitive_component_mobility_counts"), MakeCountArray(PrimitiveMobilityCounts));
	Report->SetArrayField(TEXT("static_mesh_component_mobility_counts"), MakeCountArray(StaticMeshMobilityCounts));

	TArray<FString> MeshPaths;
	UniqueMeshes.GetKeys(MeshPaths);
	MeshPaths.Sort();
	TArray<TSharedPtr<FJsonValue>> MeshEntries;
	MeshEntries.Reserve(MeshPaths.Num());
	for (const FString& MeshPath : MeshPaths)
	{
		MeshEntries.Add(MakeShared<FJsonValueObject>(MakeMeshJson(MeshPath, UniqueMeshes.FindChecked(MeshPath))));
	}
	Report->SetArrayField(TEXT("unique_static_mesh_assets"), MeshEntries);

	Report->SetNumberField(TEXT("component_detail_limit"), MaxComponentDetails);
	Report->SetNumberField(TEXT("component_detail_total"), static_cast<double>(ComponentDetailTotal));
	Report->SetBoolField(TEXT("component_details_truncated"), ComponentDetailTotal > ComponentDetails.Num());
	Report->SetArrayField(TEXT("component_details"), ComponentDetails);
	Report->SetNumberField(TEXT("player_start_detail_limit"), MaxPlayerStartDetails);
	Report->SetBoolField(TEXT("player_start_details_truncated"), PlayerStartCount > PlayerStarts.Num());
	Report->SetArrayField(TEXT("player_starts"), PlayerStarts);
	Report->SetObjectField(TEXT("primitive_component_world_bounds"), MakeBoundsJson(PrimitiveBounds));
	Report->SetObjectField(TEXT("player_start_location_bounds"), MakeBoundsJson(PlayerStartBounds));
	Report->SetStringField(TEXT("bounds_note"), TEXT("Primitive bounds are the union of loaded UPrimitiveComponent world bounds; PlayerStart bounds cover listed start locations."));

	Report->SetNumberField(TEXT("missing_dependency_log_hit_count"), MissingDependencyLogHitCount);
	Report->SetStringField(TEXT("load_diagnostics_status"), bLoadLogObserverAvailable
		? TEXT("CAPTURED_DURING_SYNCHRONOUS_MAP_LOAD") : TEXT("UNKNOWN_GLOG_UNAVAILABLE"));
	Report->SetBoolField(TEXT("missing_dependency_log_details_truncated"), MissingDependencyLogHitCount > MissingDependencyLogLines.Num());
	TArray<TSharedPtr<FJsonValue>> DependencyLogEntries;
	for (const FString& Line : MissingDependencyLogLines)
	{
		DependencyLogEntries.Add(MakeShared<FJsonValueString>(Line));
	}
	Report->SetArrayField(TEXT("missing_dependency_log_hits"), DependencyLogEntries);
	Report->SetStringField(TEXT("capture_coverage_status"), LandscapeComponentCount > 0
		? TEXT("INCONCLUSIVE_UNSUPPORTED_LANDSCAPE_PRESENT")
		: (bHasCaptureCoverageGap ? TEXT("INCONCLUSIVE_COMPONENT_OR_CPU_DATA_REVIEW_REQUIRED")
								 : TEXT("NO_CAPTURE_COVERAGE_GAP_FOUND")));
	SetMemoryJson(Report, MemoryBefore, FPlatformMemory::GetStats());

	return bTargetWorldLoaded && bLoadMapReturned && OutTerminalStatus == TEXT("PASS");
}

static bool RunRuralG0Audit(FAutomationTestBase* Test)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralG0"), RunId);
	const FString ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
	const FPlatformMemoryStats MemoryBefore = FPlatformMemory::GetStats();

	FIMRuralG0LoadLogObserver LogObserver;
	const bool bLogObserverAttached = GLog != nullptr;
	if (bLogObserverAttached)
	{
		GLog->AddOutputDevice(&LogObserver);
	}

	const double LoadStartedAt = FPlatformTime::Seconds();
	const bool bLoadMapReturned = FEditorFileUtils::LoadMap(RuralMapPath, false, true);
	const double LoadMs = (FPlatformTime::Seconds() - LoadStartedAt) * 1000.0;

	if (bLogObserverAttached && GLog)
	{
		GLog->RemoveOutputDevice(&LogObserver);
	}

	TArray<FString> MissingDependencyLogLines;
	int32 MissingDependencyLogHitCount = 0;
	LogObserver.Snapshot(MissingDependencyLogLines, MissingDependencyLogHitCount);

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	FString TerminalStatus;
	FString FailureReason;
	const bool bInventoryAccepted = BuildRuralG0Report(RunId, bLoadMapReturned, LoadMs,
		MemoryBefore, bLogObserverAttached,
		MissingDependencyLogLines, MissingDependencyLogHitCount,
		Report, TerminalStatus, FailureReason);

	const bool bDirectoryReady = IFileManager::Get().MakeDirectory(*ReportDirectory, true);
	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
	Writer->Close();
	const bool bReportWritten = bDirectoryReady && bSerialized
		&& FFileHelper::SaveStringToFile(JsonText, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	if (!bReportWritten)
	{
		TerminalStatus = TEXT("FAIL");
		FailureReason = TEXT("Could not create or write the Rural G0 JSON report.");
	}

	UE_LOG(LogTemp, Display,
		TEXT("IMLogs AcousticRuralG0Summary run_id=%s status=%s load_ms=%.3f report=%s"),
		*RunId, *TerminalStatus, LoadMs, *ReportPath);
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s AcousticRuralG0 run_id=%s"), *TerminalStatus, *RunId);

	if (TerminalStatus == TEXT("PASS") && bReportWritten && bInventoryAccepted)
	{
		Test->AddInfo(FString::Printf(TEXT("Rural G0 inventory report: %s"), *ReportPath));
		Test->AddInfo(TEXT("Rural G0 terminal status: PASS."));
		return true;
	}

	Test->AddError(FString::Printf(TEXT("Rural G0 terminal status: %s. %s Report: %s"),
		*TerminalStatus, *FailureReason, *ReportPath));
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralMapAuditTest,
	"IceMoon.AcousticField.G0.RuralMapReadOnlyAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralMapAuditTest::RunTest(const FString&)
{
	return IMAcousticRuralMapAuditTestPrivate::RunRuralG0Audit(this);
}

#endif
