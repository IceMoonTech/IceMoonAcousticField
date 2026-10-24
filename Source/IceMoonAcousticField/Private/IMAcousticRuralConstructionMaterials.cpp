#include "IMAcousticRuralConstructionMaterials.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ComponentInstanceDataCache.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "K2Node_AddComponent.h"
#include "K2Node_CallParentFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/SavePackage.h"
#include "UObject/GCObject.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace IMAcousticRuralConstructionMaterialsPrivate
{
constexpr const TCHAR* SourceBlueprintObjectPath =
	TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/Spline_Fence.Spline_Fence");
constexpr const TCHAR* SourceGeneratedClassPath =
	TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/Spline_Fence.Spline_Fence_C");
constexpr const TCHAR* ChildBlueprintPackagePath =
	TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/BP_IM_RuralFence_Physical");
constexpr const TCHAR* ChildBlueprintObjectPath =
	TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/BP_IM_RuralFence_Physical.BP_IM_RuralFence_Physical");
constexpr const TCHAR* ExpectedWorldPackagePath =
	TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* StaticTemplateName = TEXT("NODE_AddStaticMeshComponent-0");
constexpr const TCHAR* SplineTemplateName = TEXT("NODE_AddSplineMeshComponent-2");
constexpr const TCHAR* StaticMeshObjectPath =
	TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/SM_Fence_02.SM_Fence_02");
constexpr const TCHAR* SplineMeshObjectPath =
	TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/SM_Fence_Wires_01.SM_Fence_Wires_01");
constexpr const TCHAR* WireMaterialObjectPath =
	TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/M_Fence_Wires_01.M_Fence_Wires_01");
constexpr const TCHAR* WoodMaterialObjectPath =
	TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/MI_Fence_01.MI_Fence_01");
constexpr const TCHAR* WireLodMaterialObjectPath =
	TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/M_Fence_Wires_01_LOD.M_Fence_Wires_01_LOD");
constexpr int32 ExpectedActorCount = 8;
constexpr int32 ExpectedGeneratedMeshComponentCount = 2090;
constexpr int32 ExpectedStaticMeshComponentCount = 1045;
constexpr int32 ExpectedSplineMeshComponentCount = 1045;
constexpr double SpatialToleranceCm = 0.001;
constexpr double AngularToleranceRadians = 1.0e-6;

static const FName ExpectedActorNames[] =
{
	FName(TEXT("Spline_Fence2")),
	FName(TEXT("Spline_Fence3_2")),
	FName(TEXT("Spline_Fence4_5")),
	FName(TEXT("Spline_Fence5_12")),
	FName(TEXT("Spline_Fence6_15")),
	FName(TEXT("Spline_Fence7_18")),
	FName(TEXT("Spline_Fence8_21")),
	FName(TEXT("Spline_Fence_2"))
};

struct FIMSourceTemplate
{
	UActorComponent* Component = nullptr;
	FComponentKey Key;
	FString Name;
	FString MeshPath;
	TArray<FString> MaterialPaths;
	bool bSpline = false;
};

struct FIMSourceComponentIdentity
{
	FString Name;
	FString ClassPath;
};

struct FIMActorComponentMatchValue
{
	FString Name;
	FString ClassPath;
	bool bCDONameMatch = false;
	bool bCDOClassMatch = false;
};

struct FIMPackageHash
{
	FString PackageName;
	FString Filename;
	FString BeforeSha1;
	FString AfterSha1;
	int64 ByteCount = 0;
	bool bDirtyBefore = false;
	bool bDirtyAfter = false;
};

struct FIMPropertyValue
{
	FString TypeName;
	FString ExportedValue;
};

struct FIMSplinePointValue
{
	FVector Location = FVector::ZeroVector;
	FVector ArriveTangent = FVector::ZeroVector;
	FVector LeaveTangent = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector Scale = FVector::OneVector;
	uint8 PointType = 0;
};

struct FIMComponentValue
{
	FString ComponentName;
	FString ClassPath;
	FString MeshPath;
	FString AttachParentClassPath;
	FString AttachSocketName;
	TArray<FName> Tags;
	EComponentMobility::Type Mobility = EComponentMobility::Movable;
	FTransform WorldTransform = FTransform::Identity;
	FTransform RelativeTransform = FTransform::Identity;
	TArray<FString> MaterialPaths;
	TArray<FTransform> InstanceWorldTransforms;
	TArray<FIMSplinePointValue> SplinePoints;
	FString SemanticSortKey;
	bool bStaticMesh = false;
	bool bSplineMesh = false;
	bool bSplineInput = false;
	bool bInstancedStaticMesh = false;
	bool bSplineClosedLoop = false;
	FVector SplineMeshStartPosition = FVector::ZeroVector;
	FVector SplineMeshStartTangent = FVector::ZeroVector;
	FVector SplineMeshEndPosition = FVector::ZeroVector;
	FVector SplineMeshEndTangent = FVector::ZeroVector;
	FVector2D SplineMeshStartScale = FVector2D::ZeroVector;
	FVector2D SplineMeshEndScale = FVector2D::ZeroVector;
	FVector2D SplineMeshStartOffset = FVector2D::ZeroVector;
	FVector2D SplineMeshEndOffset = FVector2D::ZeroVector;
	float SplineMeshStartRoll = 0.0f;
	float SplineMeshEndRoll = 0.0f;
	FVector SplineMeshUpDirection = FVector::UpVector;
	uint8 SplineMeshForwardAxis = 0;
	float SplineMeshBoundaryMin = 0.0f;
	float SplineMeshBoundaryMax = 0.0f;
	float SplineMeshNaniteBoundsScale = 1.0f;
	bool bSplineMeshSmoothInterpRollScale = false;
	EComponentCreationMethod CreationMethod = EComponentCreationMethod::Native;
	bool bSourceCDONameMatch = false;
	bool bSourceCDOClassMatch = false;
	bool bSplineHasBeenEdited = false;
	bool bInputSplinePointsToConstructionScript = false;
};

struct FIMActorValue
{
	FName ActorName = NAME_None; // Trace only; actor pairing uses world transform and RandomStream.InitialSeed.
	FTransform WorldTransform = FTransform::Identity;
	TMap<FName, FIMPropertyValue> EditableProperties;
	TArray<FName> Tags;
	TArray<FIMActorComponentMatchValue> NonUserComponentMatches;
	TArray<FIMComponentValue> Components;
	FString StableMatchKey;
	int32 InitialRandomSeed = 0;
	int32 MutableRandomSeed = 0;
	int32 ActorComponentCount = 0;
	int32 NonUserComponentCount = 0;
	int32 NonUserCDONameMatchCount = 0;
	int32 NonUserCDOClassMatchCount = 0;
	int32 GeneratedMeshCount = 0;
	int32 StaticMeshCount = 0;
	int32 SplineMeshCount = 0;
	int32 NonUserSplineInputCount = 0;
};

struct FIMActorComparisonResult
{
	bool bPassed = false;
	FString FirstDifference;
};

// These snapshots are local to one synchronous editor conversion. Heap-owned
// FGCObjects keep native instance-data references valid if construction triggers
// GC; only the input spline is copied, so new mesh PM bindings are not overwritten.
struct FIMSplineInputSnapshot final : FGCObject
{
	FString StableMatchKey;
	FName ComponentName;
	TObjectPtr<AActor> SourceActor = nullptr;
	TObjectPtr<USplineComponent> SourceSpline = nullptr;
	TObjectPtr<UObject> SourceTemplate = nullptr;
	int32 SourcePointCount = 0;
	bool bInputToUCS = false;
	TStructOnScope<FActorComponentInstanceData> Data;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObject(SourceActor);
		Collector.AddReferencedObject(SourceSpline);
		Collector.AddReferencedObject(SourceTemplate);
		Data.AddReferencedObjects(Collector);
		if (Data.IsValid())
		{
			Data.Get()->AddReferencedObjects(Collector);
		}
	}

	virtual FString GetReferencerName() const override
	{
		return TEXT("IMAcousticRuralFenceSplineInputSnapshot");
	}
};

static bool IsPluginPackageName(const FString& PackageName)
{
	return PackageName.StartsWith(TEXT("/IceMoonAcousticField/"), ESearchCase::CaseSensitive);
}

static FString VectorSortKey(const FVector& Value)
{
	return FString::Printf(TEXT("%+.9f,%+.9f,%+.9f"), Value.X, Value.Y, Value.Z);
}

static FString Vector2DSortKey(const FVector2D& Value)
{
	return FString::Printf(TEXT("%+.9f,%+.9f"), Value.X, Value.Y);
}

static FString QuaternionSortKey(const FQuat& Value)
{
	return FString::Printf(TEXT("%+.9f,%+.9f,%+.9f,%+.9f"), Value.X, Value.Y, Value.Z, Value.W);
}

static FString TransformSortKey(const FTransform& Value)
{
	return VectorSortKey(Value.GetLocation()) + TEXT("|")
		+ QuaternionSortKey(Value.GetRotation()) + TEXT("|")
		+ VectorSortKey(Value.GetScale3D());
}

static bool SameSpatialVector(const FVector& A, const FVector& B)
{
	return A.Equals(B, SpatialToleranceCm);
}

static bool SameAngularRotation(const FQuat& A, const FQuat& B)
{
	return A.AngularDistance(B) <= AngularToleranceRadians;
}

static bool SameTransform(const FTransform& A, const FTransform& B, FString& Error, const FString& Label)
{
	if (!SameSpatialVector(A.GetLocation(), B.GetLocation()))
	{
		Error = Label + TEXT(" location changed beyond 0.001 cm.");
		return false;
	}
	if (!SameAngularRotation(A.GetRotation(), B.GetRotation()))
	{
		Error = Label + TEXT(" rotation changed beyond 1e-6 radians.");
		return false;
	}
	if (A.GetScale3D() != B.GetScale3D())
	{
		Error = Label + TEXT(" scale changed.");
		return false;
	}
	return true;
}

static FString ComponentSemanticKey(const FIMComponentValue& Value)
{
	FString Key = Value.ClassPath + TEXT("|") + Value.MeshPath + TEXT("|")
		+ FString::FromInt(static_cast<int32>(Value.Mobility)) + TEXT("|")
		+ TransformSortKey(Value.WorldTransform) + TEXT("|")
		+ TransformSortKey(Value.RelativeTransform) + TEXT("|")
		+ Value.AttachParentClassPath + TEXT("|") + Value.AttachSocketName;

	if (Value.bSplineMesh)
	{
		Key += TEXT("|SM|")
			+ VectorSortKey(Value.SplineMeshStartPosition) + TEXT("|")
			+ VectorSortKey(Value.SplineMeshStartTangent) + TEXT("|")
			+ VectorSortKey(Value.SplineMeshEndPosition) + TEXT("|")
			+ VectorSortKey(Value.SplineMeshEndTangent) + TEXT("|")
			+ Vector2DSortKey(Value.SplineMeshStartScale) + TEXT("|")
			+ Vector2DSortKey(Value.SplineMeshEndScale) + TEXT("|")
			+ Vector2DSortKey(Value.SplineMeshStartOffset) + TEXT("|")
			+ Vector2DSortKey(Value.SplineMeshEndOffset) + TEXT("|")
			+ FString::Printf(TEXT("%.9f|%.9f|"), Value.SplineMeshStartRoll, Value.SplineMeshEndRoll)
			+ VectorSortKey(Value.SplineMeshUpDirection) + TEXT("|")
			+ FString::FromInt(Value.SplineMeshForwardAxis) + TEXT("|")
			+ FString::Printf(TEXT("%.9f|%.9f|%.9f|%d"), Value.SplineMeshBoundaryMin,
				Value.SplineMeshBoundaryMax, Value.SplineMeshNaniteBoundsScale,
				Value.bSplineMeshSmoothInterpRollScale ? 1 : 0);
	}

	if (Value.bSplineInput)
	{
		Key += Value.bSplineClosedLoop ? TEXT("|SC|") : TEXT("|SO|");
		for (const FIMSplinePointValue& Point : Value.SplinePoints)
		{
			Key += VectorSortKey(Point.Location) + TEXT("|")
				+ VectorSortKey(Point.ArriveTangent) + TEXT("|")
				+ VectorSortKey(Point.LeaveTangent) + TEXT("|")
				+ QuaternionSortKey(Point.Rotation) + TEXT("|")
				+ VectorSortKey(Point.Scale) + TEXT("|")
				+ FString::FromInt(Point.PointType) + TEXT("|");
		}
	}

	for (const FTransform& InstanceTransform : Value.InstanceWorldTransforms)
	{
		Key += TEXT("|I|") + TransformSortKey(InstanceTransform);
	}
	return Key;
}

static FString StableActorMatchKey(const FIMActorValue& Value)
{
	// Generated construction-script components can appear or disappear during conversion. Pair actors
	// only by stable instance data so a changed generated mesh inventory cannot reorder actor matches.
	return TransformSortKey(Value.WorldTransform) + TEXT("|InitialSeed|")
		+ FString::FromInt(Value.InitialRandomSeed);
}

static bool HashPackageFile(const FString& PackageName, FString& OutFilename, FString& OutSha1, int64& OutBytes, FString& Error)
{
	if (!FPackageName::DoesPackageExist(PackageName, &OutFilename))
	{
		Error = TEXT("Source package is missing on disk: ") + PackageName;
		return false;
	}
	FPaths::NormalizeFilename(OutFilename);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *OutFilename) || Bytes.IsEmpty())
	{
		Error = TEXT("Could not read source package bytes for immutability verification: ") + OutFilename;
		return false;
	}
	uint8 Digest[20];
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
	OutSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	OutBytes = Bytes.Num();
	return true;
}

static bool AddPackageHash(const UObject* Object, TMap<FString, FIMPackageHash>& InOutHashes, FString& Error)
{
	if (!IsValid(Object))
	{
		Error = TEXT("Cannot hash a missing source asset object.");
		return false;
	}
	UPackage* Package = Object->GetOutermost();
	if (!Package || !Package->GetName().StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
	{
		Error = TEXT("Expected a source /Game asset package, got: ") + GetPathNameSafe(Object);
		return false;
	}
	const FString PackageName = Package->GetName();
	if (InOutHashes.Contains(PackageName))
	{
		return true;
	}

	FIMPackageHash Record;
	Record.PackageName = PackageName;
	Record.bDirtyBefore = Package->IsDirty();
	if (!HashPackageFile(PackageName, Record.Filename, Record.BeforeSha1, Record.ByteCount, Error))
	{
		return false;
	}
	InOutHashes.Add(PackageName, MoveTemp(Record));
	return true;
}

static bool VerifyPackageHashes(TMap<FString, FIMPackageHash>& InOutHashes, FString& Error)
{
	TArray<FString> PackageNames;
	InOutHashes.GetKeys(PackageNames);
	PackageNames.Sort();
	for (const FString& PackageName : PackageNames)
	{
		FIMPackageHash& Record = InOutHashes.FindChecked(PackageName);
		if (!HashPackageFile(PackageName, Record.Filename, Record.AfterSha1, Record.ByteCount, Error))
		{
			return false;
		}
		UPackage* Package = FindPackage(nullptr, *PackageName);
		Record.bDirtyAfter = Package ? Package->IsDirty() : false;
		if (Record.BeforeSha1 != Record.AfterSha1)
		{
			Error = TEXT("Source asset bytes changed during plugin-only authoring: ") + PackageName;
			return false;
		}
		if (Record.bDirtyBefore != Record.bDirtyAfter)
		{
			Error = TEXT("Source asset dirty state changed during plugin-only authoring: ") + PackageName;
			return false;
		}
	}
	return true;
}

static bool CollectSourceTemplates(UBlueprint* SourceBlueprint, UBlueprintGeneratedClass* SourceClass,
	TArray<FIMSourceTemplate>& OutTemplates, FString& Error)
{
	if (!SourceBlueprint || !SourceClass || SourceBlueprint->GeneratedClass != SourceClass
		|| SourceBlueprint->GetPathName() != SourceBlueprintObjectPath
		|| SourceClass->GetPathName() != SourceGeneratedClassPath)
	{
		Error = TEXT("The Rural fence source Blueprint or its GeneratedClass does not match the frozen package identity.");
		return false;
	}
	if (SourceBlueprint->ComponentTemplates.Num() != 2 || SourceClass->ComponentTemplates.Num() != 2)
	{
		Error = TEXT("The Rural fence source must expose exactly the two observed K2 component templates.");
		return false;
	}

	TMap<FString, FIMSourceTemplate> ByName;
	for (UActorComponent* ComponentTemplate : SourceBlueprint->ComponentTemplates)
	{
		if (!IsValid(ComponentTemplate))
		{
			Error = TEXT("The source Blueprint contains a null K2 component template.");
			return false;
		}
		FIMSourceTemplate Record;
		Record.Component = ComponentTemplate;
		Record.Name = ComponentTemplate->GetName();
		if (Record.Name == StaticTemplateName)
		{
			UStaticMeshComponent* StaticTemplate = Cast<UStaticMeshComponent>(ComponentTemplate);
			if (!StaticTemplate || StaticTemplate->IsA<USplineMeshComponent>()
				|| ComponentTemplate->GetClass() != UStaticMeshComponent::StaticClass()
				|| !StaticTemplate->GetStaticMesh()
				|| StaticTemplate->GetStaticMesh()->GetPathName() != StaticMeshObjectPath)
			{
				Error = TEXT("The static K2 template class or mesh no longer matches the observed fence template.");
				return false;
			}
			Record.MeshPath = StaticTemplate->GetStaticMesh()->GetPathName();
			Record.bSpline = false;
		}
		else if (Record.Name == SplineTemplateName)
		{
			USplineMeshComponent* SplineTemplate = Cast<USplineMeshComponent>(ComponentTemplate);
			if (!SplineTemplate || ComponentTemplate->GetClass() != USplineMeshComponent::StaticClass()
				|| !SplineTemplate->GetStaticMesh()
				|| SplineTemplate->GetStaticMesh()->GetPathName() != SplineMeshObjectPath)
			{
				Error = TEXT("The spline K2 template class or mesh no longer matches the observed fence template.");
				return false;
			}
			Record.MeshPath = SplineTemplate->GetStaticMesh()->GetPathName();
			Record.bSpline = true;
		}
		else
		{
			Error = TEXT("Unexpected component template in the source fence Blueprint: ") + Record.Name;
			return false;
		}

		UStaticMeshComponent* MeshTemplate = Cast<UStaticMeshComponent>(ComponentTemplate);
		if (!MeshTemplate || MeshTemplate->GetNumMaterials() != 2)
		{
			Error = TEXT("The source fence templates must each expose exactly the two observed material slots.");
			return false;
		}
		for (int32 SlotIndex = 0; SlotIndex < MeshTemplate->GetNumMaterials(); ++SlotIndex)
		{
			UMaterialInterface* Material = MeshTemplate->GetMaterial(SlotIndex);
			if (!Material)
			{
				Error = TEXT("A source fence material slot is null.");
				return false;
			}
			Record.MaterialPaths.Add(Material->GetPathName());
		}

		const FString ExpectedSlot0 = WireMaterialObjectPath;
		const FString ExpectedSlot1 = Record.bSpline ? WireLodMaterialObjectPath
			: (Record.Name == StaticTemplateName ? WoodMaterialObjectPath : TEXT(""));
		if (Record.MaterialPaths[0] != ExpectedSlot0 || Record.MaterialPaths[1] != ExpectedSlot1)
		{
			Error = TEXT("A source fence template material slot no longer matches the observed Wire/Wood/WireLOD binding: ")
				+ Record.Name;
			return false;
		}
		if (SourceClass->FindComponentTemplateByName(ComponentTemplate->GetFName()) != ComponentTemplate)
		{
			Error = TEXT("A K2 template in the source Blueprint is not the template resolved by its GeneratedClass: ")
				+ Record.Name;
			return false;
		}
		ByName.Add(Record.Name, MoveTemp(Record));
	}

	if (!ByName.Contains(StaticTemplateName) || !ByName.Contains(SplineTemplateName))
	{
		Error = TEXT("The source fence Blueprint is missing one of the two observed K2 component templates.");
		return false;
	}
	// A copied raw template is removed from the editor list on PostLoad when
	// the child has no AddComponent nodes. Inherited UCS overrides instead use
	// the real parent node GUID, which ICH preserves across load and compile.
	TArray<UK2Node_AddComponent*> SourceNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(SourceBlueprint, SourceNodes);
	TSet<FGuid> MatchedNodeGuids;
	for (TPair<FString, FIMSourceTemplate>& Entry : ByName)
	{
		int32 MatchCount = 0;
		for (UK2Node_AddComponent* Node : SourceNodes)
		{
			if (Node && Node->GetTemplateFromNode() == Entry.Value.Component)
			{
				Entry.Value.Key = FComponentKey(SourceBlueprint, FUCSComponentId(Node));
				++MatchCount;
			}
		}
		if (MatchCount != 1 || !Entry.Value.Key.IsValid() || !Entry.Value.Key.IsUCSKey()
			|| Entry.Value.Key.GetComponentOwner() != SourceClass
			|| MatchedNodeGuids.Contains(Entry.Value.Key.GetAssociatedGuid())
			|| Entry.Value.Key.GetOriginalTemplate(Entry.Value.Component->GetFName()) != Entry.Value.Component)
		{
			Error = TEXT("A source K2 template does not resolve to one unique original parent node: ") + Entry.Key;
			return false;
		}
		MatchedNodeGuids.Add(Entry.Value.Key.GetAssociatedGuid());
	}
	OutTemplates.Add(ByName.FindChecked(StaticTemplateName));
	OutTemplates.Add(ByName.FindChecked(SplineTemplateName));

	return true;
}

static bool ValidateRedirects(const TArray<FIMSourceTemplate>& Templates,
	const TMap<UMaterialInterface*, UMaterialInterface*>& Redirects,
	TMap<FString, FString>& OutSourceMaterialToCopyPath,
	TSet<FString>& OutCopyMaterialPaths,
	FString& Error)
{
	TSet<UMaterialInterface*> ExpectedSourceMaterials;
	for (const FIMSourceTemplate& Template : Templates)
	{
		UStaticMeshComponent* MeshTemplate = CastChecked<UStaticMeshComponent>(Template.Component);
		for (int32 SlotIndex = 0; SlotIndex < MeshTemplate->GetNumMaterials(); ++SlotIndex)
		{
			UMaterialInterface* SourceMaterial = MeshTemplate->GetMaterial(SlotIndex);
			if (!SourceMaterial)
			{
				Error = TEXT("A source fence template material pointer is null.");
				return false;
			}
			ExpectedSourceMaterials.Add(SourceMaterial);
		}
	}
	if (Redirects.Num() != ExpectedSourceMaterials.Num() || ExpectedSourceMaterials.Num() != 3)
	{
		Error = FString::Printf(TEXT("Redirects must contain exactly the three distinct observed source material pointers (got %d redirects, %d expected)."),
			Redirects.Num(), ExpectedSourceMaterials.Num());
		return false;
	}

	for (UMaterialInterface* SourceMaterial : ExpectedSourceMaterials)
	{
		UMaterialInterface* const* CopyValue = Redirects.Find(SourceMaterial);
		UMaterialInstanceConstant* Copy = CopyValue ? Cast<UMaterialInstanceConstant>(*CopyValue) : nullptr;
		if (!Copy || !IsPluginPackageName(Copy->GetOutermost()->GetName())
			|| Copy->GetPathName() == SourceMaterial->GetPathName()
			|| !Copy->bOverridePhysMaterial || !Copy->PhysMaterial
			|| !Copy->GetPhysicalMaterial() || Copy->GetPhysicalMaterial() != Copy->PhysMaterial
			|| !IsPluginPackageName(Copy->PhysMaterial->GetOutermost()->GetName()))
		{
			Error = TEXT("A Redirects value must be a plugin MIC with an explicit plugin PhysicalMaterial override for ")
				+ SourceMaterial->GetPathName();
			return false;
		}
		OutSourceMaterialToCopyPath.Add(SourceMaterial->GetPathName(), Copy->GetPathName());
		OutCopyMaterialPaths.Add(Copy->GetPathName());
	}

	for (const TPair<UMaterialInterface*, UMaterialInterface*>& Pair : Redirects)
	{
		if (!Pair.Key || !ExpectedSourceMaterials.Contains(Pair.Key))
		{
			Error = TEXT("Redirects contains an extra or null source material key.");
			return false;
		}
	}
	return true;
}

static bool FindExpectedActors(UWorld* World, UClass* SourceClass, UClass* ChildClass,
	TArray<AActor*>& OutActors, FString& Error)
{
	TSet<FName> ExpectedNames;
	for (const FName Name : ExpectedActorNames)
	{
		ExpectedNames.Add(Name);
	}
	OutActors.Reset(ExpectedActorCount);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || (Actor->GetClass() != SourceClass && Actor->GetClass() != ChildClass))
		{
			continue;
		}
		if (Actor->GetClass() == SourceClass && !ExpectedNames.Contains(Actor->GetFName()))
		{
			Error = TEXT("The Rural preview contains an unobserved original fence actor: ")
				+ Actor->GetName();
			return false;
		}
		OutActors.Add(Actor);
	}
	if (OutActors.Num() != ExpectedActorCount)
	{
		Error = FString::Printf(TEXT("Expected eight exact source/preview-child fence actors, found %d."), OutActors.Num());
		return false;
	}
	// DoConvertActors spawns a fresh actor without an explicit object name.
	// Compare the complete spatial/geometry snapshots below to preserve identity.
	return true;
}

static FProperty* FindActorProperty(UClass* ActorClass, const FName Name, const FName Alias = NAME_None)
{
	FProperty* Property = FindFProperty<FProperty>(ActorClass, Name);
	if (!Property && Alias != NAME_None)
	{
		Property = FindFProperty<FProperty>(ActorClass, Alias);
	}
	return Property;
}

static bool ExportActorProperty(AActor* Actor, const FName PropertyName, const FName Alias,
	FIMPropertyValue& OutValue, FString& Error)
{
	FProperty* Property = FindActorProperty(Actor->GetClass(), PropertyName, Alias);
	if (!Property)
	{
		Error = TEXT("Required editable fence Blueprint property is missing: ") + PropertyName.ToString();
		return false;
	}
	OutValue.TypeName = Property->GetClass()->GetName();
	// ExportText_InContainer is a delta export: false can mean a legitimate
	// zero/default value equals its null delta, rather than a capture failure.
	// Capture the actual item unconditionally so default-valued construction
	// inputs remain part of the before/after equality and restoration contract.
	Property->ExportTextItem_InContainer(OutValue.ExportedValue, Actor, nullptr, Actor, PPF_None);
	return true;
}

static FString MakeMaterialPathForMode(const FString& SourcePath, const TMap<FString, FString>& SourceToCopy,
	bool bExpectPluginCopies, FString& Error)
{
	if (!bExpectPluginCopies)
	{
		if (!SourceToCopy.Contains(SourcePath))
		{
			Error = TEXT("An actor component uses an unknown source material: ") + SourcePath;
		}
		return SourcePath;
	}
	if (const FString* CopyPath = SourceToCopy.Find(SourcePath))
	{
		return *CopyPath;
	}
	for (const TPair<FString, FString>& Pair : SourceToCopy)
	{
		if (Pair.Value == SourcePath)
		{
			return SourcePath;
		}
	}
	Error = TEXT("An actor component does not use one of the exact plugin material redirects: ") + SourcePath;
	return FString();
}

static FString ComponentExpectedTemplateName(const UStaticMeshComponent* Component)
{
	return Component->IsA<USplineMeshComponent>() ? FString(SplineTemplateName) : FString(StaticTemplateName);
}

static bool CaptureSourceCDOComponents(UClass* SourceClass,
	TArray<FIMSourceComponentIdentity>& OutComponents, FString& Error)
{
	OutComponents.Reset();
	AActor* SourceCDO = SourceClass ? Cast<AActor>(SourceClass->GetDefaultObject()) : nullptr;
	if (!SourceCDO)
	{
		Error = TEXT("Could not read the source Rural fence Blueprint CDO component inventory.");
		return false;
	}

	TInlineComponentArray<UActorComponent*> Components;
	SourceCDO->GetComponents(Components);
	for (UActorComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			Error = TEXT("The source Rural fence Blueprint CDO contains an invalid component.");
			return false;
		}
		FIMSourceComponentIdentity& Identity = OutComponents.AddDefaulted_GetRef();
		Identity.Name = Component->GetName();
		Identity.ClassPath = Component->GetClass()->GetPathName();
	}
	OutComponents.Sort([](const FIMSourceComponentIdentity& A, const FIMSourceComponentIdentity& B)
	{
		return A.Name == B.Name ? A.ClassPath < B.ClassPath : A.Name < B.Name;
	});
	return true;
}

static bool CaptureComponent(AActor* Actor, USceneComponent* SceneComponent,
	const TMap<FString, FString>& SourceToCopyMaterialPaths, bool bExpectPluginCopies,
	FIMComponentValue& OutValue, FString& Error)
{
	if (!IsValid(SceneComponent))
	{
		Error = TEXT("An actor contains an invalid scene component while capturing fence semantics.");
		return false;
	}

	OutValue.ComponentName = SceneComponent->GetName();
	OutValue.ClassPath = SceneComponent->GetClass()->GetPathName();
	OutValue.CreationMethod = SceneComponent->CreationMethod;
	OutValue.Mobility = SceneComponent->GetMobility();
	OutValue.WorldTransform = SceneComponent->GetComponentTransform();
	OutValue.RelativeTransform = SceneComponent->GetRelativeTransform();
	OutValue.AttachParentClassPath = SceneComponent->GetAttachParent()
		? SceneComponent->GetAttachParent()->GetClass()->GetPathName() : FString();
	OutValue.AttachSocketName = SceneComponent->GetAttachSocketName().ToString();
	OutValue.Tags = SceneComponent->ComponentTags;

	if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(SceneComponent))
	{
		OutValue.bStaticMesh = true;
		UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
		if (!Mesh || MeshComponent->CreationMethod != EComponentCreationMethod::UserConstructionScript)
		{
			Error = TEXT("Every observed generated fence mesh must remain a valid UserConstructionScript static mesh component on ")
				+ Actor->GetName();
			return false;
		}
		OutValue.MeshPath = Mesh->GetPathName();

		const FString ExpectedTemplateName = ComponentExpectedTemplateName(MeshComponent);
		UObject* Archetype = MeshComponent->GetArchetype();
		if (!Archetype || Archetype->GetName() != ExpectedTemplateName)
		{
			Error = TEXT("A generated fence component no longer resolves to its exact K2 template: ")
				+ MeshComponent->GetName();
			return false;
		}
		if (MeshComponent->IsA<USplineMeshComponent>())
		{
			if (OutValue.MeshPath != SplineMeshObjectPath)
			{
				Error = TEXT("A spline mesh component uses an unknown mesh: ") + OutValue.MeshPath;
				return false;
			}
			OutValue.bSplineMesh = true;
		}
		else
		{
			if (MeshComponent->GetClass() != UStaticMeshComponent::StaticClass()
				|| OutValue.MeshPath != StaticMeshObjectPath)
			{
				Error = TEXT("A static fence mesh component uses an unknown class or mesh: ") + OutValue.MeshPath;
				return false;
			}
		}

		const TArray<FString>& ExpectedSourceSlots = ExpectedTemplateName == StaticTemplateName
			? TArray<FString>({ WireMaterialObjectPath, WoodMaterialObjectPath })
			: TArray<FString>({ WireMaterialObjectPath, WireLodMaterialObjectPath });
		if (MeshComponent->GetNumMaterials() != ExpectedSourceSlots.Num())
		{
			Error = TEXT("A generated fence component changed its material slot count.");
			return false;
		}
		for (int32 SlotIndex = 0; SlotIndex < MeshComponent->GetNumMaterials(); ++SlotIndex)
		{
			UMaterialInterface* Material = MeshComponent->GetMaterial(SlotIndex);
			if (!Material)
			{
				Error = TEXT("A generated fence component has a null material slot.");
				return false;
			}
			FString ExpectedPath = MakeMaterialPathForMode(ExpectedSourceSlots[SlotIndex],
				SourceToCopyMaterialPaths, bExpectPluginCopies, Error);
			if (ExpectedPath.IsEmpty() || Material->GetPathName() != ExpectedPath)
			{
				if (Error.IsEmpty())
				{
					Error = FString::Printf(TEXT("Fence material redirect differs: component=%s slot=%d expected=%s actual=%s."),
						*MeshComponent->GetPathName(), SlotIndex, *ExpectedPath, *Material->GetPathName());
				}
				return false;
			}
			OutValue.MaterialPaths.Add(Material->GetPathName());
		}

		if (UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(MeshComponent))
		{
			OutValue.bInstancedStaticMesh = true;
			const int32 InstanceCount = Instanced->GetInstanceCount();
			OutValue.InstanceWorldTransforms.Reserve(InstanceCount);
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; ++InstanceIndex)
			{
				FTransform InstanceTransform;
				if (!Instanced->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
				{
					Error = TEXT("Could not read an actual InstancedStaticMesh world transform.");
					return false;
				}
				OutValue.InstanceWorldTransforms.Add(InstanceTransform);
			}
			OutValue.InstanceWorldTransforms.Sort([](const FTransform& A, const FTransform& B)
			{
				return TransformSortKey(A) < TransformSortKey(B);
			});
		}

		if (USplineMeshComponent* SplineMesh = Cast<USplineMeshComponent>(MeshComponent))
		{
			OutValue.SplineMeshStartPosition = SplineMesh->GetStartPosition();
			OutValue.SplineMeshStartTangent = SplineMesh->GetStartTangent();
			OutValue.SplineMeshEndPosition = SplineMesh->GetEndPosition();
			OutValue.SplineMeshEndTangent = SplineMesh->GetEndTangent();
			OutValue.SplineMeshStartScale = SplineMesh->GetStartScale();
			OutValue.SplineMeshEndScale = SplineMesh->GetEndScale();
			OutValue.SplineMeshStartOffset = SplineMesh->GetStartOffset();
			OutValue.SplineMeshEndOffset = SplineMesh->GetEndOffset();
			OutValue.SplineMeshStartRoll = SplineMesh->GetStartRoll();
			OutValue.SplineMeshEndRoll = SplineMesh->GetEndRoll();
			OutValue.SplineMeshUpDirection = SplineMesh->GetSplineUpDir();
			OutValue.SplineMeshForwardAxis = static_cast<uint8>(SplineMesh->GetForwardAxis());
			OutValue.SplineMeshBoundaryMin = SplineMesh->GetBoundaryMin();
			OutValue.SplineMeshBoundaryMax = SplineMesh->GetBoundaryMax();
			OutValue.SplineMeshNaniteBoundsScale = SplineMesh->SplineParams.NaniteClusterBoundsScale;
			OutValue.bSplineMeshSmoothInterpRollScale = SplineMesh->bSmoothInterpRollScale;
		}
	}

	if (USplineComponent* SplineComponent = Cast<USplineComponent>(SceneComponent))
	{
		OutValue.bSplineInput = true;
		OutValue.bSplineHasBeenEdited = SplineComponent->bSplineHasBeenEdited;
		OutValue.bInputSplinePointsToConstructionScript = SplineComponent->bInputSplinePointsToConstructionScript;
		OutValue.bSplineClosedLoop = SplineComponent->IsClosedLoop();
		const int32 PointCount = SplineComponent->GetNumberOfSplinePoints();
		if (SceneComponent->CreationMethod != EComponentCreationMethod::UserConstructionScript)
		{
			if (PointCount < 2)
			{
				Error = TEXT("The non-UCS fence spline input must contain at least two observable points.");
				return false;
			}
		}
		OutValue.SplinePoints.Reserve(PointCount);
		for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
		{
			FIMSplinePointValue Point;
			Point.Location = SplineComponent->GetLocationAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local);
			Point.ArriveTangent = SplineComponent->GetArriveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local);
			Point.LeaveTangent = SplineComponent->GetLeaveTangentAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local);
			Point.Rotation = SplineComponent->GetRotationAtSplinePoint(PointIndex, ESplineCoordinateSpace::Local).Quaternion();
			Point.Scale = SplineComponent->GetScaleAtSplinePoint(PointIndex);
			Point.PointType = static_cast<uint8>(SplineComponent->GetSplinePointType(PointIndex));
			OutValue.SplinePoints.Add(Point);
		}
	}

	OutValue.SemanticSortKey = ComponentSemanticKey(OutValue);
	return true;
}

static bool CaptureActor(AActor* Actor, UClass* SourceClass, UClass* ChildClass,
	const TArray<FIMSourceComponentIdentity>& SourceCDOComponents,
	const TMap<FString, FString>& SourceToCopyMaterialPaths, TSet<FString>* InOutNamesToConsider,
	FIMActorValue& OutValue, FString& Error)
{
	if (!IsValid(Actor) || (Actor->GetClass() != SourceClass && Actor->GetClass() != ChildClass))
	{
		Error = TEXT("A Rural fence actor has an unexpected class before/after conversion: ") + GetPathNameSafe(Actor);
		return false;
	}
	const bool bExpectPluginCopies = Actor->GetClass() == ChildClass;
	OutValue.ActorName = Actor->GetFName();
	OutValue.WorldTransform = Actor->GetActorTransform();
	OutValue.Tags = Actor->Tags;

	if (!ExportActorProperty(Actor, TEXT("Spacing"), NAME_None,
			OutValue.EditableProperties.FindOrAdd(TEXT("Spacing")), Error)
		|| !ExportActorProperty(Actor, TEXT("TangentScale"), NAME_None,
			OutValue.EditableProperties.FindOrAdd(TEXT("TangentScale")), Error)
		// Blueprint VarName tag (not its display label) in the source asset.
		|| !ExportActorProperty(Actor, TEXT("Collision"), TEXT("Collision (Disable while editing)"),
			OutValue.EditableProperties.FindOrAdd(TEXT("Collision")), Error)
		|| !ExportActorProperty(Actor, TEXT("RandomStream"), NAME_None,
			OutValue.EditableProperties.FindOrAdd(TEXT("RandomStream")), Error))
	{
		return false;
	}
	FStructProperty* RandomStreamProperty = CastField<FStructProperty>(
		FindActorProperty(Actor->GetClass(), TEXT("RandomStream")));
	if (!RandomStreamProperty || RandomStreamProperty->Struct != TBaseStructure<FRandomStream>::Get())
	{
		Error = TEXT("The RandomStream Blueprint property no longer has the observed FRandomStream type.");
		return false;
	}
	const FRandomStream* RandomStream = RandomStreamProperty->ContainerPtrToValuePtr<FRandomStream>(Actor);
	OutValue.InitialRandomSeed = RandomStream->GetInitialSeed();
	OutValue.MutableRandomSeed = RandomStream->GetCurrentSeed();
	OutValue.StableMatchKey = StableActorMatchKey(OutValue);

	TInlineComponentArray<UActorComponent*> ActorComponents;
	Actor->GetComponents(ActorComponents);
	for (UActorComponent* Component : ActorComponents)
	{
		if (!IsValid(Component))
		{
			Error = TEXT("A Rural fence actor contains an invalid component.");
			return false;
		}
		++OutValue.ActorComponentCount;
		bool bSourceCDONameMatch = false;
		bool bSourceCDOClassMatch = false;
		const FString ComponentClassPath = Component->GetClass()->GetPathName();
		for (const FIMSourceComponentIdentity& SourceIdentity : SourceCDOComponents)
		{
			if (SourceIdentity.Name == Component->GetName())
			{
				bSourceCDONameMatch = true;
				bSourceCDOClassMatch |= SourceIdentity.ClassPath == ComponentClassPath;
			}
		}
		if (Component->CreationMethod != EComponentCreationMethod::UserConstructionScript)
		{
			++OutValue.NonUserComponentCount;
			OutValue.NonUserCDONameMatchCount += bSourceCDONameMatch ? 1 : 0;
			OutValue.NonUserCDOClassMatchCount += bSourceCDOClassMatch ? 1 : 0;
			FIMActorComponentMatchValue& Match = OutValue.NonUserComponentMatches.AddDefaulted_GetRef();
			Match.Name = Component->GetName();
			Match.ClassPath = ComponentClassPath;
			Match.bCDONameMatch = bSourceCDONameMatch;
			Match.bCDOClassMatch = bSourceCDOClassMatch;
		}
		if (InOutNamesToConsider && Component->CreationMethod != EComponentCreationMethod::UserConstructionScript)
		{
			// DoConvertActors keys its property-copy helper by each actor component's actual GetName().
			// Preserve every non-UCS input component, including the source actor's spline curve.
			InOutNamesToConsider->Add(Component->GetName());
		}

		USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
		if (!SceneComponent)
		{
			continue;
		}

		FIMComponentValue ComponentValue;
		if (!CaptureComponent(Actor, SceneComponent, SourceToCopyMaterialPaths,
				bExpectPluginCopies, ComponentValue, Error))
		{
			return false;
		}
		ComponentValue.bSourceCDONameMatch = bSourceCDONameMatch;
		ComponentValue.bSourceCDOClassMatch = bSourceCDOClassMatch;
		if (ComponentValue.bStaticMesh)
		{
			++OutValue.GeneratedMeshCount;
			if (ComponentValue.bSplineMesh)
			{
				++OutValue.SplineMeshCount;
			}
			else
			{
				++OutValue.StaticMeshCount;
			}
		}
		if (ComponentValue.bSplineInput && Component->CreationMethod != EComponentCreationMethod::UserConstructionScript)
		{
			++OutValue.NonUserSplineInputCount;
		}
		OutValue.Components.Add(MoveTemp(ComponentValue));
	}

	if (OutValue.GeneratedMeshCount == 0 || OutValue.NonUserSplineInputCount == 0)
	{
		Error = TEXT("A Rural fence actor is missing generated fence meshes or its non-UCS spline input: ")
			+ Actor->GetName();
		return false;
	}
	OutValue.Components.Sort([](const FIMComponentValue& A, const FIMComponentValue& B)
	{
		return A.SemanticSortKey < B.SemanticSortKey;
	});
	OutValue.StableMatchKey = StableActorMatchKey(OutValue);
	return true;
}

static bool CaptureSplineInputSnapshot(AActor* Actor, const FIMActorValue& Before,
	TUniquePtr<FIMSplineInputSnapshot>& OutSnapshot, FString& Error)
{
	TInlineComponentArray<USplineComponent*> Splines;
	Actor->GetComponents(Splines);
	if (Splines.Num() != 1 || !IsValid(Splines[0])
		|| Splines[0]->CreationMethod != EComponentCreationMethod::SimpleConstructionScript
		|| !Splines[0]->bSplineHasBeenEdited || Before.NonUserSplineInputCount != 1)
	{
		Error = TEXT("The source fence does not expose exactly one edited inherited SCS spline input.");
		return false;
	}
	OutSnapshot = MakeUnique<FIMSplineInputSnapshot>();
	OutSnapshot->StableMatchKey = Before.StableMatchKey;
	OutSnapshot->ComponentName = Splines[0]->GetFName();
	OutSnapshot->SourceActor = Actor;
	OutSnapshot->SourceSpline = Splines[0];
	OutSnapshot->SourceTemplate = Splines[0]->GetArchetype();
	OutSnapshot->SourcePointCount = Splines[0]->GetNumberOfSplinePoints();
	OutSnapshot->bInputToUCS = Splines[0]->bInputSplinePointsToConstructionScript;
	OutSnapshot->Data = Splines[0]->GetComponentInstanceData();
	if (!IsValid(OutSnapshot->SourceTemplate) || !OutSnapshot->Data.IsValid()
		|| (!OutSnapshot->Data.Cast<FSplineInstanceData>()
			&& !OutSnapshot->Data.Cast<FSplineComponentInstanceData>())
		|| !OutSnapshot->Data.Get()->MatchesComponent(Splines[0], OutSnapshot->SourceTemplate))
	{
		Error = TEXT("The source spline did not provide matching complete native spline instance data.");
		return false;
	}
	return true;
}

static USplineComponent* FindSnapshotSpline(AActor* Actor, const FIMSplineInputSnapshot& Snapshot, FString& Error)
{
	TInlineComponentArray<USplineComponent*> Splines;
	Actor->GetComponents(Splines);
	USplineComponent* Spline = Splines.Num() == 1 ? Splines[0] : nullptr;
	if (!IsValid(Spline) || Spline->GetFName() != Snapshot.ComponentName
		|| Spline->CreationMethod != EComponentCreationMethod::SimpleConstructionScript
		|| Spline->GetArchetype() != Snapshot.SourceTemplate
		|| !Snapshot.Data.Get()->MatchesComponent(Spline, Spline->GetArchetype()))
	{
		Error = FString::Printf(TEXT("Converted fence has no exact matching inherited SCS spline: actor=%s input=%s count=%d."),
			*Actor->GetPathName(), *Snapshot.ComponentName.ToString(), Splines.Num());
		return nullptr;
	}
	return Spline;
}

static bool RestoreConvertedSplineInput(AActor* Actor, FIMSplineInputSnapshot& Snapshot,
	TSharedPtr<FJsonObject>& RestoreEvidence, FString& Error)
{
	USplineComponent* Spline = FindSnapshotSpline(Actor, Snapshot, Error);
	if (!Spline)
	{
		return false;
	}
	// An Undo annotation contains a different cached curve. This author owns
	// newly converted actors, so require the normal path which captures the
	// seeded current input for the actor's own SCS -> UCS reconstruction.
	if (Actor->CurrentTransactionAnnotation.IsValid())
	{
		Error = TEXT("A newly converted fence unexpectedly has an Undo reconstruction annotation.");
		return false;
	}
	RestoreEvidence = MakeShared<FJsonObject>();
	RestoreEvidence->SetStringField(TEXT("stable_match_key"), Snapshot.StableMatchKey);
	RestoreEvidence->SetStringField(TEXT("actor"), Actor->GetName());
	RestoreEvidence->SetStringField(TEXT("input"), Spline->GetName());
	RestoreEvidence->SetStringField(TEXT("native_instance_data_type"), Snapshot.Data.GetStruct()->GetName());
	RestoreEvidence->SetNumberField(TEXT("source_point_count"), Snapshot.SourcePointCount);
	RestoreEvidence->SetNumberField(TEXT("converted_point_count"), Spline->GetNumberOfSplinePoints());
	RestoreEvidence->SetBoolField(TEXT("converted_edited"), Spline->bSplineHasBeenEdited);
	Spline->Modify();
	Spline->bInputSplinePointsToConstructionScript = Snapshot.bInputToUCS;
	Snapshot.Data.Get()->ApplyToComponent(Spline, ECacheApplyPhase::PostSimpleConstructionScript);
	RestoreEvidence->SetNumberField(TEXT("seeded_point_count"), Spline->GetNumberOfSplinePoints());
	Actor->RerunConstructionScripts();
	// Rerun clears this annotation only when reconstruction succeeds. Do not
	// let the explicit PostUCS restore hide a failed construction script.
	if (Actor->CurrentTransactionAnnotation.IsValid())
	{
		Error = TEXT("Fence reconstruction failed and retained its Undo annotation; spline restore was stopped.");
		return false;
	}
	Spline = FindSnapshotSpline(Actor, Snapshot, Error);
	if (!Spline)
	{
		return false;
	}
	if (Spline->bInputSplinePointsToConstructionScript != Snapshot.bInputToUCS)
	{
		Error = TEXT("The construction script changed the frozen input-to-UCS flag.");
		return false;
	}
	Snapshot.Data.Get()->ApplyToComponent(Spline, ECacheApplyPhase::PostUserConstructionScript);
	RestoreEvidence->SetNumberField(TEXT("restored_point_count"), Spline->GetNumberOfSplinePoints());
	RestoreEvidence->SetBoolField(TEXT("restored_edited"), Spline->bSplineHasBeenEdited);
	RestoreEvidence->SetBoolField(TEXT("input_to_ucs"), Spline->bInputSplinePointsToConstructionScript);
	RestoreEvidence->SetBoolField(TEXT("complete_geometry_verified"), false);
	// Full source/target curve, transform, mesh and material comparisons below
	// remain the acceptance gate; successful instance-data application is not it.
	return true;
}

static bool CompareSplineMesh(const FIMComponentValue& A, const FIMComponentValue& B, FString& Error)
{
	if (A.bSplineMesh != B.bSplineMesh)
	{
		Error = TEXT("A component changed between spline mesh and static mesh.");
		return false;
	}
	if (!A.bSplineMesh)
	{
		return true;
	}
	if (!SameSpatialVector(A.SplineMeshStartPosition, B.SplineMeshStartPosition)
		|| !SameSpatialVector(A.SplineMeshStartTangent, B.SplineMeshStartTangent)
		|| !SameSpatialVector(A.SplineMeshEndPosition, B.SplineMeshEndPosition)
		|| !SameSpatialVector(A.SplineMeshEndTangent, B.SplineMeshEndTangent))
	{
		Error = TEXT("SplineMesh start/end positions or tangents changed beyond 0.001 cm.");
		return false;
	}
	if (A.SplineMeshStartScale != B.SplineMeshStartScale
		|| A.SplineMeshEndScale != B.SplineMeshEndScale
		|| A.SplineMeshStartOffset != B.SplineMeshStartOffset
		|| A.SplineMeshEndOffset != B.SplineMeshEndOffset)
	{
		Error = TEXT("SplineMesh scale or offset settings changed.");
		return false;
	}
	if (FMath::Abs(A.SplineMeshStartRoll - B.SplineMeshStartRoll) > AngularToleranceRadians
		|| FMath::Abs(A.SplineMeshEndRoll - B.SplineMeshEndRoll) > AngularToleranceRadians)
	{
		Error = TEXT("SplineMesh roll changed beyond 1e-6 radians.");
		return false;
	}
	if (!A.SplineMeshUpDirection.GetSafeNormal().Equals(B.SplineMeshUpDirection.GetSafeNormal(), 1.0e-6)
		|| A.SplineMeshForwardAxis != B.SplineMeshForwardAxis
		|| A.SplineMeshBoundaryMin != B.SplineMeshBoundaryMin
		|| A.SplineMeshBoundaryMax != B.SplineMeshBoundaryMax
		|| A.SplineMeshNaniteBoundsScale != B.SplineMeshNaniteBoundsScale
		|| A.bSplineMeshSmoothInterpRollScale != B.bSplineMeshSmoothInterpRollScale)
	{
		Error = TEXT("SplineMesh up direction, forward axis, bounds, Nanite scale, or roll/scale interpolation changed.");
		return false;
	}
	return true;
}

static bool CompareSplineInput(const FIMComponentValue& A, const FIMComponentValue& B, FString& Error)
{
	if (A.bSplineInput != B.bSplineInput)
	{
		Error = TEXT("A component changed between spline input and another scene component.");
		return false;
	}
	if (!A.bSplineInput)
	{
		return true;
	}
	if (A.CreationMethod != B.CreationMethod || A.bSplineHasBeenEdited != B.bSplineHasBeenEdited
		|| A.bInputSplinePointsToConstructionScript != B.bInputSplinePointsToConstructionScript)
	{
		Error = TEXT("The source spline creation, edited, or input-to-UCS state changed.");
		return false;
	}
	if (A.bSplineClosedLoop != B.bSplineClosedLoop || A.SplinePoints.Num() != B.SplinePoints.Num())
	{
		Error = TEXT("The source spline closed-loop state or point count changed.");
		return false;
	}
	for (int32 PointIndex = 0; PointIndex < A.SplinePoints.Num(); ++PointIndex)
	{
		const FIMSplinePointValue& PointA = A.SplinePoints[PointIndex];
		const FIMSplinePointValue& PointB = B.SplinePoints[PointIndex];
		if (!SameSpatialVector(PointA.Location, PointB.Location)
			|| !SameSpatialVector(PointA.ArriveTangent, PointB.ArriveTangent)
			|| !SameSpatialVector(PointA.LeaveTangent, PointB.LeaveTangent))
		{
			Error = FString::Printf(TEXT("Source spline point %d location or tangent changed beyond 0.001 cm."), PointIndex);
			return false;
		}
		if (!SameAngularRotation(PointA.Rotation, PointB.Rotation)
			|| PointA.Scale != PointB.Scale || PointA.PointType != PointB.PointType)
		{
			Error = FString::Printf(TEXT("Source spline point %d rotation, scale, or interpolation type changed."), PointIndex);
			return false;
		}
	}
	return true;
}

static bool CompareActorValues(const FIMActorValue& A, const FIMActorValue& B,
	const TMap<FString, FString>& SourceToCopyMaterialPaths, FString& Error)
{
	if (!SameTransform(A.WorldTransform, B.WorldTransform, Error, TEXT("Actor")))
	{
		return false;
	}
	if (A.InitialRandomSeed != B.InitialRandomSeed)
	{
		Error = TEXT("RandomStream.InitialSeed changed during actor conversion.");
		return false;
	}
	if (A.Tags != B.Tags)
	{
		Error = TEXT("Rural fence actor tags changed during conversion.");
		return false;
	}
	if (A.EditableProperties.Num() != B.EditableProperties.Num())
	{
		Error = TEXT("The Rural fence actor's editable Blueprint property set changed.");
		return false;
	}
	for (const TPair<FName, FIMPropertyValue>& Pair : A.EditableProperties)
	{
		const FIMPropertyValue* AfterValue = B.EditableProperties.Find(Pair.Key);
		if (!AfterValue || Pair.Value.TypeName != AfterValue->TypeName)
		{
			Error = TEXT("An editable Rural fence Blueprint property changed type or disappeared: ")
				+ Pair.Key.ToString();
			return false;
		}
		if (Pair.Key != TEXT("RandomStream") && Pair.Value.ExportedValue != AfterValue->ExportedValue)
		{
			Error = TEXT("An editable Rural fence Blueprint property changed value: ") + Pair.Key.ToString();
			return false;
		}
	}
	if (A.Components.Num() != B.Components.Num()
		|| A.GeneratedMeshCount != B.GeneratedMeshCount
		|| A.StaticMeshCount != B.StaticMeshCount
		|| A.SplineMeshCount != B.SplineMeshCount
		|| A.NonUserSplineInputCount != B.NonUserSplineInputCount)
	{
		Error = TEXT("Rural fence scene component or generated mesh component counts changed.");
		return false;
	}
	for (int32 ComponentIndex = 0; ComponentIndex < A.Components.Num(); ++ComponentIndex)
	{
		const FIMComponentValue& ComponentA = A.Components[ComponentIndex];
		const FIMComponentValue& ComponentB = B.Components[ComponentIndex];
		if (ComponentA.ClassPath != ComponentB.ClassPath || ComponentA.MeshPath != ComponentB.MeshPath
			|| ComponentA.Mobility != ComponentB.Mobility
			|| ComponentA.Tags != ComponentB.Tags
			|| ComponentA.AttachParentClassPath != ComponentB.AttachParentClassPath
			|| ComponentA.AttachSocketName != ComponentB.AttachSocketName)
		{
			Error = TEXT("A Rural fence component changed class, mesh, mobility, or scene attachment semantics.");
			return false;
		}
		if (!SameTransform(ComponentA.WorldTransform, ComponentB.WorldTransform, Error, TEXT("Component world transform"))
			|| !SameTransform(ComponentA.RelativeTransform, ComponentB.RelativeTransform, Error, TEXT("Component relative transform")))
		{
			return false;
		}
		if (ComponentA.MaterialPaths.Num() != ComponentB.MaterialPaths.Num())
		{
			Error = TEXT("A Rural fence component changed its material slot count.");
			return false;
		}
		for (int32 SlotIndex = 0; SlotIndex < ComponentA.MaterialPaths.Num(); ++SlotIndex)
		{
			FString ExpectedAfterPath;
			if (const FString* CopyPath = SourceToCopyMaterialPaths.Find(ComponentA.MaterialPaths[SlotIndex]))
			{
				ExpectedAfterPath = *CopyPath;
			}
			else
			{
				for (const TPair<FString, FString>& Redirect : SourceToCopyMaterialPaths)
				{
					if (Redirect.Value == ComponentA.MaterialPaths[SlotIndex])
					{
						ExpectedAfterPath = ComponentA.MaterialPaths[SlotIndex];
						break;
					}
				}
			}
			if (ExpectedAfterPath.IsEmpty() || ComponentB.MaterialPaths[SlotIndex] != ExpectedAfterPath)
			{
				Error = TEXT("A Rural fence material slot did not resolve to its exact plugin MIC redirect.");
				return false;
			}
		}
		if (ComponentA.bInstancedStaticMesh != ComponentB.bInstancedStaticMesh
			|| ComponentA.InstanceWorldTransforms.Num() != ComponentB.InstanceWorldTransforms.Num())
		{
			Error = TEXT("An InstancedStaticMesh component or its actual instance count changed.");
			return false;
		}
		for (int32 InstanceIndex = 0; InstanceIndex < ComponentA.InstanceWorldTransforms.Num(); ++InstanceIndex)
		{
			if (!SameTransform(ComponentA.InstanceWorldTransforms[InstanceIndex],
					ComponentB.InstanceWorldTransforms[InstanceIndex], Error, TEXT("InstancedStaticMesh instance transform")))
			{
				return false;
			}
		}
		if (!CompareSplineMesh(ComponentA, ComponentB, Error)
			|| !CompareSplineInput(ComponentA, ComponentB, Error))
		{
			return false;
		}
	}
	return true;
}

static bool VerifyConstructionGraph(UBlueprint* SourceBlueprint, UBlueprint* ChildBlueprint, FString& Error)
{
	UEdGraph* SourceGraph = FBlueprintEditorUtils::FindUserConstructionScript(SourceBlueprint);
	UEdGraph* ChildGraph = FBlueprintEditorUtils::FindUserConstructionScript(ChildBlueprint);
	if (!SourceGraph || !ChildGraph || ChildGraph->GetOuter() != ChildBlueprint
		|| SourceGraph->GetFName() != ChildGraph->GetFName()
		|| SourceGraph->GetClass() != ChildGraph->GetClass()
		|| SourceGraph->Schema != ChildGraph->Schema || SourceGraph->Nodes.Num() != ChildGraph->Nodes.Num())
	{
		FString ChildGraphNames;
		for (const UEdGraph* Graph : ChildBlueprint->FunctionGraphs)
		{
			ChildGraphNames += GetPathNameSafe(Graph) + TEXT(";");
		}
		Error = FString::Printf(
			TEXT("The child construction graph identity differs: source=%s child=%s nodes=%d/%d child_functions=%s."),
			*GetPathNameSafe(SourceGraph), *GetPathNameSafe(ChildGraph),
			SourceGraph ? SourceGraph->Nodes.Num() : INDEX_NONE,
			ChildGraph ? ChildGraph->Nodes.Num() : INDEX_NONE, *ChildGraphNames);
		return false;
	}
	TArray<UK2Node_CallParentFunction*> ParentCalls;
	SourceGraph->GetNodesOfClass(ParentCalls);
	// A copied parent call would now dispatch to the source UCS a second time.
	// This observed source has none; reject a future graph with that ambiguity.
	if (!ParentCalls.IsEmpty())
	{
		Error = TEXT("The source construction graph contains a parent call whose copied dispatch would differ.");
		return false;
	}
	auto LinkKeys = [](const UEdGraphPin* Pin)
	{
		TArray<FString> Keys;
		for (const UEdGraphPin* Link : Pin->LinkedTo)
		{
			Keys.Add(Link->GetOwningNode()->NodeGuid.ToString() + TEXT(":") + Link->PinId.ToString());
		}
		Keys.Sort();
		return Keys;
	};
	TSet<FGuid> Seen;
	for (const UEdGraphNode* SourceNode : SourceGraph->Nodes)
	{
		const TObjectPtr<UEdGraphNode>* ChildNodePtr = ChildGraph->Nodes.FindByPredicate(
			[SourceNode](const TObjectPtr<UEdGraphNode>& Node)
			{ return Node && SourceNode && Node->NodeGuid == SourceNode->NodeGuid; });
		const UEdGraphNode* ChildNode = ChildNodePtr ? ChildNodePtr->Get() : nullptr;
		if (!SourceNode || !ChildNode || !SourceNode->NodeGuid.IsValid() || Seen.Contains(SourceNode->NodeGuid)
			|| SourceNode->GetClass() != ChildNode->GetClass() || SourceNode->Pins.Num() != ChildNode->Pins.Num()
			|| SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString()
				!= ChildNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString())
		{
			Error = FString::Printf(TEXT("Copied construction node differs: source=%s child=%s titles=%s/%s pins=%d/%d."),
				*GetPathNameSafe(SourceNode), *GetPathNameSafe(ChildNode),
				SourceNode ? *SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : TEXT("none"),
				ChildNode ? *ChildNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : TEXT("none"),
				SourceNode ? SourceNode->Pins.Num() : INDEX_NONE, ChildNode ? ChildNode->Pins.Num() : INDEX_NONE);
			return false;
		}
		Seen.Add(SourceNode->NodeGuid);
		for (int32 Index = 0; Index < SourceNode->Pins.Num(); ++Index)
		{
			const UEdGraphPin* SourcePin = SourceNode->Pins[Index];
			const UEdGraphPin* ChildPin = ChildNode->Pins[Index];
			if (!SourcePin || !ChildPin || SourcePin->PinId != ChildPin->PinId
				|| SourcePin->PinName != ChildPin->PinName || SourcePin->Direction != ChildPin->Direction
				|| SourcePin->PinType != ChildPin->PinType || SourcePin->DefaultValue != ChildPin->DefaultValue
				|| SourcePin->AutogeneratedDefaultValue != ChildPin->AutogeneratedDefaultValue
				|| SourcePin->DefaultObject != ChildPin->DefaultObject
				|| !SourcePin->DefaultTextValue.EqualTo(ChildPin->DefaultTextValue)
				|| LinkKeys(SourcePin) != LinkKeys(ChildPin))
			{
				Error = FString::Printf(TEXT("Copied construction pin differs: node=%s index=%d source=%s child=%s types=%s/%s defaults=%s/%s objects=%s/%s."),
					*SourceNode->GetPathName(), Index,
					SourcePin ? *SourcePin->PinName.ToString() : TEXT("none"),
					ChildPin ? *ChildPin->PinName.ToString() : TEXT("none"),
					SourcePin ? *GetPathNameSafe(SourcePin->PinType.PinSubCategoryObject.Get()) : TEXT("none"),
					ChildPin ? *GetPathNameSafe(ChildPin->PinType.PinSubCategoryObject.Get()) : TEXT("none"),
					SourcePin ? *SourcePin->DefaultValue : TEXT("none"), ChildPin ? *ChildPin->DefaultValue : TEXT("none"),
					SourcePin ? *GetPathNameSafe(SourcePin->DefaultObject) : TEXT("none"),
					ChildPin ? *GetPathNameSafe(ChildPin->DefaultObject) : TEXT("none"));
				return false;
			}
		}
	}
	return true;
}

static bool VerifyChildTemplates(UBlueprint* ChildBlueprint, UBlueprintGeneratedClass* ChildClass,
	const TArray<FIMSourceTemplate>& SourceTemplates, const TMap<UMaterialInterface*, UMaterialInterface*>& Redirects,
	FString& Error)
{
	if (!ChildBlueprint || !ChildClass || ChildBlueprint->GeneratedClass != ChildClass
		|| ChildBlueprint->ParentClass != ChildClass->GetSuperClass()
		|| ChildBlueprint->ComponentTemplates.Num() != SourceTemplates.Num()
		|| ChildClass->ComponentTemplates.Num() != SourceTemplates.Num())
	{
		Error = TEXT("The plugin Rural child Blueprint has an unexpected parent or raw component template inventory.");
		return false;
	}
	UInheritableComponentHandler* Handler = ChildBlueprint->GetInheritableComponentHandler(false);
	if (Handler != ChildClass->GetInheritableComponentHandler(false) || (Handler && !Handler->IsEmpty()))
	{
		Error = TEXT("The copied UCS must consume its own raw templates, with no inherited override records.");
		return false;
	}
	UBlueprint* SourceBlueprint = Cast<UBlueprint>(ChildClass->GetSuperClass()->ClassGeneratedBy);
	if (!SourceBlueprint || !VerifyConstructionGraph(SourceBlueprint, ChildBlueprint, Error))
	{
		return false;
	}
	TArray<UK2Node_AddComponent*> ChildNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(ChildBlueprint, ChildNodes);
	if (ChildNodes.Num() != SourceTemplates.Num())
	{
		Error = TEXT("The copied construction graph must own exactly the two real AddComponent nodes.");
		return false;
	}
	for (const FIMSourceTemplate& SourceTemplate : SourceTemplates)
	{
		UActorComponent* ChildTemplate = ChildBlueprint->FindTemplateByName(FName(*SourceTemplate.Name));
		UK2Node_AddComponent* const* ChildNode = ChildNodes.FindByPredicate(
			[&SourceTemplate](const UK2Node_AddComponent* Node)
			{ return Node && Node->NodeGuid == SourceTemplate.Key.GetAssociatedGuid(); });
		if (!IsValid(ChildTemplate) || ChildTemplate->GetOuter() != ChildClass
			|| ChildTemplate->GetName() != SourceTemplate.Name
			|| ChildTemplate->GetClass() != SourceTemplate.Component->GetClass()
			|| ChildTemplate->HasAnyFlags(RF_Transient)
			|| !ChildNode || (*ChildNode)->GetTemplateFromNode() != ChildTemplate)
		{
			Error = TEXT("The plugin child Blueprint is missing an exact template owned by the copied source node: ") + SourceTemplate.Name;
			return false;
		}
		UStaticMeshComponent* ChildMeshTemplate = Cast<UStaticMeshComponent>(ChildTemplate);
		UStaticMeshComponent* SourceMeshTemplate = Cast<UStaticMeshComponent>(SourceTemplate.Component);
		if (!ChildMeshTemplate || !SourceMeshTemplate
			|| ChildClass->FindComponentTemplateByName(ChildTemplate->GetFName()) != ChildTemplate
			|| ChildMeshTemplate->GetStaticMesh() != SourceMeshTemplate->GetStaticMesh()
			|| ChildMeshTemplate->GetNumMaterials() != SourceMeshTemplate->GetNumMaterials())
		{
			Error = TEXT("The plugin child GeneratedClass does not resolve the expected same-name K2 template: ")
				+ SourceTemplate.Name;
			return false;
		}
		for (int32 SlotIndex = 0; SlotIndex < SourceMeshTemplate->GetNumMaterials(); ++SlotIndex)
		{
			UMaterialInterface* SourceMaterial = SourceMeshTemplate->GetMaterial(SlotIndex);
			UMaterialInterface* const* Redirected = SourceMaterial ? Redirects.Find(SourceMaterial) : nullptr;
			if (!Redirected || ChildMeshTemplate->GetMaterial(SlotIndex) != *Redirected)
			{
				Error = TEXT("The plugin child template did not bind the exact Redirects MIC at slot ")
					+ FString::FromInt(SlotIndex) + TEXT(": ") + SourceTemplate.Name;
				return false;
			}
		}
	}
	return true;
}

static bool PrepareChildConstructionGraph(UBlueprint* SourceBlueprint, UBlueprint* ChildBlueprint,
	const TArray<FIMSourceTemplate>& SourceTemplates,
	const TMap<UMaterialInterface*, UMaterialInterface*>& Redirects, FString& Error)
{
	UBlueprintGeneratedClass* ChildClass = Cast<UBlueprintGeneratedClass>(ChildBlueprint->GeneratedClass);
	TArray<UK2Node_AddComponent*> ChildNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(ChildBlueprint, ChildNodes);
	if (!ChildClass || (ChildNodes.Num() != 0 && ChildNodes.Num() != SourceTemplates.Num())
		|| (ChildBlueprint->ComponentTemplates.Num() != 0
			&& ChildBlueprint->ComponentTemplates.Num() != SourceTemplates.Num())
		|| (ChildClass->ComponentTemplates.Num() != 0
			&& ChildClass->ComponentTemplates.Num() != SourceTemplates.Num()))
	{
		Error = TEXT("The plugin child has unknown local AddComponent nodes or raw template state.");
		return false;
	}

	const bool bNeedsConstructionCopy = ChildNodes.IsEmpty();
	if (!bNeedsConstructionCopy && !VerifyConstructionGraph(SourceBlueprint, ChildBlueprint, Error))
	{
		return false;
	}
	UInheritableComponentHandler* Handler = ChildBlueprint->GetInheritableComponentHandler(false);
	if (Handler != ChildClass->GetInheritableComponentHandler(false)
		|| (Handler && Handler->GetOuter() != ChildClass))
	{
		Error = TEXT("The child Blueprint/Class handler is detached before construction-graph migration.");
		return false;
	}
	TArray<UActorComponent*> ExistingTemplates;
	for (UActorComponent* Template : ChildClass->ComponentTemplates)
	{
		ExistingTemplates.AddUnique(Template);
	}
	if (Handler)
	{
		TArray<UActorComponent*> OverrideTemplates;
		Handler->GetAllTemplates(OverrideTemplates);
		for (UActorComponent* Template : OverrideTemplates)
		{
			ExistingTemplates.AddUnique(Template);
		}
		for (auto It = Handler->CreateRecordIterator(); It; ++It)
		{
			const FComponentOverrideRecord& Record = *It;
			if (!SourceTemplates.ContainsByPredicate([&Record](const FIMSourceTemplate& Source)
				{ return Record.ComponentKey.Match(Source.Key); }))
			{
				Error = TEXT("The child handler contains an override outside the two frozen parent UCS nodes.");
				return false;
			}
		}
	}
	if (ExistingTemplates.Num() != 0 && ExistingTemplates.Num() != SourceTemplates.Num())
	{
		Error = TEXT("The child contains a partial or ambiguous old template inventory.");
		return false;
	}
	// Validate task-owned old raw/ICH copies before retiring them through the
	// engine API and full compilation. Never rename or delete source objects.
	TSet<FString> LegacyNames;
	for (UActorComponent* Legacy : ExistingTemplates)
	{
		const FIMSourceTemplate* Source = SourceTemplates.FindByPredicate(
			[Legacy](const FIMSourceTemplate& Candidate)
			{
				return Legacy && Legacy->GetName() == Candidate.Name;
			});
		UStaticMeshComponent* LegacyMesh = Cast<UStaticMeshComponent>(Legacy);
		UStaticMeshComponent* SourceMesh = Source ? Cast<UStaticMeshComponent>(Source->Component) : nullptr;
		if (!IsValid(Legacy) || Legacy->GetOuter() != ChildClass || !Source || !LegacyMesh || !SourceMesh
			|| Legacy->GetClass() != Source->Component->GetClass() || LegacyNames.Contains(Source->Name)
			|| LegacyMesh->GetStaticMesh() != SourceMesh->GetStaticMesh()
			|| LegacyMesh->GetNumMaterials() != SourceMesh->GetNumMaterials())
		{
			Error = TEXT("An old raw child template is not one of the exact owned fence copies.");
			return false;
		}
		for (int32 Slot = 0; Slot < SourceMesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* const* Copy = Redirects.Find(SourceMesh->GetMaterial(Slot));
			if (!Copy || LegacyMesh->GetMaterial(Slot) != *Copy)
			{
				Error = TEXT("An old raw child template has an unexpected material binding: ") + Source->Name;
				return false;
			}
		}
		LegacyNames.Add(Source->Name);
	}

	ChildBlueprint->Modify();
	if (bNeedsConstructionCopy)
	{
		if (!ChildBlueprint->ComponentTemplates.IsEmpty())
		{
			Error = TEXT("A child without AddComponent nodes unexpectedly retains raw editor templates.");
			return false;
		}
		if (Handler)
		{
			Handler->Modify();
			for (const FIMSourceTemplate& Source : SourceTemplates)
			{
				Handler->RemoveOverridenComponentTemplate(Source.Key);
			}
			if (!Handler->IsEmpty())
			{
				Error = TEXT("The engine did not retire all exact old inherited UCS overrides.");
				return false;
			}
		}
		// Full compile owns cleanup of obsolete generated-class subobjects.
		FKismetEditorUtilities::CompileBlueprint(ChildBlueprint);
		ChildClass = Cast<UBlueprintGeneratedClass>(ChildBlueprint->GeneratedClass);
		if (ChildBlueprint->Status == BS_Error || !ChildClass || !ChildClass->ComponentTemplates.IsEmpty())
		{
			Error = TEXT("The engine could not clean the old child template state before graph copying.");
			return false;
		}
		UEdGraph* SourceGraph = FBlueprintEditorUtils::FindUserConstructionScript(SourceBlueprint);
		TArray<UK2Node_CallParentFunction*> ParentCalls;
		if (SourceGraph)
		{
			SourceGraph->GetNodesOfClass(ParentCalls);
		}
		if (!SourceGraph || !ParentCalls.IsEmpty())
		{
			Error = TEXT("The frozen source UCS graph is absent or would duplicate a parent dispatch.");
			return false;
		}
		if (UEdGraph* PreviousGraph = FBlueprintEditorUtils::FindUserConstructionScript(ChildBlueprint))
		{
			FBlueprintEditorUtils::RemoveGraph(ChildBlueprint, PreviousGraph, EGraphRemoveFlags::MarkTransient);
		}
		UEdGraph* CopiedGraph = FEdGraphUtilities::CloneGraph(SourceGraph, ChildBlueprint, nullptr, false);
		if (!CopiedGraph || CopiedGraph->HasAnyFlags(RF_Transient))
		{
			Error = TEXT("The engine could not create the complete persistent child construction graph.");
			return false;
		}
		// CloneGraph already copies the complete function entry and exit nodes;
		// AddFunctionGraph would create extra terminators. Register this owned
		// function graph directly and mark the completed Blueprint structurally.
		ChildBlueprint->FunctionGraphs.Add(CopiedGraph);
		for (const FIMSourceTemplate& Source : SourceTemplates)
		{
			if (FindObjectFast<UObject>(ChildClass, Source.Component->GetFName()))
			{
				Error = TEXT("An obsolete child template name remains occupied after engine cleanup: ") + Source.Name;
				return false;
			}
			UActorComponent* CopiedTemplate = Cast<UActorComponent>(
				StaticDuplicateObject(Source.Component, ChildClass, Source.Component->GetFName()));
			if (!CopiedTemplate)
			{
				Error = TEXT("The engine could not copy a template for the real child AddComponent node: ") + Source.Name;
				return false;
			}
			CopiedTemplate->SetFlags(RF_ArchetypeObject | RF_Public);
			CopiedTemplate->ClearFlags(RF_InheritableComponentTemplate);
			ChildBlueprint->ComponentTemplates.Add(CopiedTemplate);
		}
		// CloneGraph uses StaticDuplicateObjectEx with no destination name, so
		// it creates a generated graph name. UCS lookup requires the canonical
		// function graph name; the editor rename API also updates entry metadata.
		FBlueprintEditorUtils::RenameGraph(CopiedGraph, SourceGraph->GetName());
		if (CopiedGraph->GetFName() != SourceGraph->GetFName())
		{
			Error = TEXT("The editor could not assign the canonical UCS graph name to the owned copy.");
			return false;
		}
	}
	for (const FIMSourceTemplate& Source : SourceTemplates)
	{
		UActorComponent* Override = ChildBlueprint->FindTemplateByName(FName(*Source.Name));
		UStaticMeshComponent* OverrideMesh = Cast<UStaticMeshComponent>(Override);
		UStaticMeshComponent* SourceMesh = Cast<UStaticMeshComponent>(Source.Component);
		if (!IsValid(Override) || Override->GetOuter() != ChildClass || Override->GetName() != Source.Name
			|| Override->GetClass() != Source.Component->GetClass() || !OverrideMesh || !SourceMesh
			|| OverrideMesh->GetStaticMesh() != SourceMesh->GetStaticMesh()
			|| OverrideMesh->GetNumMaterials() != SourceMesh->GetNumMaterials())
		{
			Error = TEXT("The real child construction node did not resolve its exact owned template: ") + Source.Name;
			return false;
		}
		Override->Modify();
		for (int32 Slot = 0; Slot < SourceMesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* const* Copy = Redirects.Find(SourceMesh->GetMaterial(Slot));
			if (!Copy || !*Copy)
			{
				Error = TEXT("The exact parent template material has no plugin Redirects entry: ") + Source.Name;
				return false;
			}
			OverrideMesh->SetMaterial(Slot, *Copy);
		}
	}
	if (!VerifyConstructionGraph(SourceBlueprint, ChildBlueprint, Error))
	{
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ChildBlueprint);
	return true;
}

static bool SaveChildBlueprint(UBlueprint* ChildBlueprint, FString& Error)
{
	if (!IsValid(ChildBlueprint) || ChildBlueprint->GetOutermost()->GetName() != ChildBlueprintPackagePath)
	{
		Error = TEXT("Refusing to save a Rural fence Blueprint outside the exact plugin package.");
		return false;
	}
	UPackage* Package = ChildBlueprint->GetOutermost();
	const FString ExpectedFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExpectedFilename), true);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, ChildBlueprint, *ExpectedFilename, Args))
	{
		Error = TEXT("Saving the plugin Rural fence Blueprint failed: ") + Package->GetName();
		return false;
	}
	FString ActualFilename;
	if (!FPackageName::DoesPackageExist(Package->GetName(), &ActualFilename))
	{
		Error = TEXT("The saved plugin Rural fence Blueprint package cannot be resolved: ") + Package->GetName();
		return false;
	}
	FPaths::NormalizeFilename(ActualFilename);
	FString NormalizedExpectedFilename = ExpectedFilename;
	FPaths::NormalizeFilename(NormalizedExpectedFilename);
	if (!ActualFilename.Equals(NormalizedExpectedFilename, ESearchCase::IgnoreCase))
	{
		Error = TEXT("The Rural fence Blueprint was saved to an unexpected package file: ") + ActualFilename;
		return false;
	}
	return true;
}

static bool SortActorValuesByStableKey(TArray<FIMActorValue>& Values, const TCHAR* SnapshotName, FString& Error)
{
	for (FIMActorValue& Value : Values)
	{
		if (Value.StableMatchKey.IsEmpty())
		{
			Value.StableMatchKey = StableActorMatchKey(Value);
		}
	}
	Values.Sort([](const FIMActorValue& A, const FIMActorValue& B)
	{
		return A.StableMatchKey < B.StableMatchKey;
	});
	for (int32 Index = 1; Index < Values.Num(); ++Index)
	{
		if (Values[Index - 1].StableMatchKey == Values[Index].StableMatchKey)
		{
			Error = FString::Printf(
				TEXT("AMBIGUOUS stable actor match key in %s snapshot (world transform + RandomStream.InitialSeed); refusing name-based pairing."),
				SnapshotName);
			return false;
		}
	}
	return true;
}

static bool ActorKeysAreUnique(const TArray<FIMActorValue>& Values,
	bool& bOutHasDuplicate, bool& bOutHasMissing)
{
	bOutHasDuplicate = false;
	bOutHasMissing = false;
	TSet<FString> Keys;
	for (const FIMActorValue& Value : Values)
	{
		if (Value.StableMatchKey.IsEmpty())
		{
			bOutHasMissing = true;
			continue;
		}
		if (Keys.Contains(Value.StableMatchKey))
		{
			bOutHasDuplicate = true;
			continue;
		}
		Keys.Add(Value.StableMatchKey);
	}
	return !bOutHasDuplicate && !bOutHasMissing;
}

static TSharedRef<FJsonObject> VectorToJson(const FVector& Value)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), Value.X);
	Result->SetNumberField(TEXT("y"), Value.Y);
	Result->SetNumberField(TEXT("z"), Value.Z);
	return Result;
}

static TSharedRef<FJsonObject> QuaternionToJson(const FQuat& Value)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), Value.X);
	Result->SetNumberField(TEXT("y"), Value.Y);
	Result->SetNumberField(TEXT("z"), Value.Z);
	Result->SetNumberField(TEXT("w"), Value.W);
	return Result;
}

static TSharedRef<FJsonObject> BuildActorDiagnostic(const FIMActorValue& Value)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name_for_trace_only"), Value.ActorName.ToString());
	Result->SetStringField(TEXT("stable_match_key"), Value.StableMatchKey);
	Result->SetNumberField(TEXT("initial_seed"), Value.InitialRandomSeed);
	Result->SetNumberField(TEXT("scene_components"), Value.Components.Num());
	Result->SetNumberField(TEXT("generated_meshes"), Value.GeneratedMeshCount);
	Result->SetNumberField(TEXT("static_meshes"), Value.StaticMeshCount);
	Result->SetNumberField(TEXT("spline_meshes"), Value.SplineMeshCount);
	Result->SetNumberField(TEXT("non_ucs_components"), Value.NonUserComponentCount);
	Result->SetNumberField(TEXT("non_ucs_cdo_name_matches"), Value.NonUserCDONameMatchCount);
	Result->SetNumberField(TEXT("non_ucs_cdo_exact_name_class_matches"), Value.NonUserCDOClassMatchCount);

	TArray<TSharedPtr<FJsonValue>> NonUcsComponents;
	for (const FIMActorComponentMatchValue& Component : Value.NonUserComponentMatches)
	{
		TSharedRef<FJsonObject> Match = MakeShared<FJsonObject>();
		Match->SetStringField(TEXT("name"), Component.Name);
		Match->SetStringField(TEXT("class"), Component.ClassPath);
		Match->SetBoolField(TEXT("source_cdo_name_match"), Component.bCDONameMatch);
		Match->SetBoolField(TEXT("source_cdo_exact_name_class_match"), Component.bCDOClassMatch);
		NonUcsComponents.Add(MakeShared<FJsonValueObject>(Match));
	}
	Result->SetArrayField(TEXT("non_ucs_component_cdo_matches"), MoveTemp(NonUcsComponents));

	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (const TPair<FName, FIMPropertyValue>& Pair : Value.EditableProperties)
	{
		Properties->SetStringField(Pair.Key.ToString(), Pair.Value.ExportedValue);
	}
	Result->SetObjectField(TEXT("editable_properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> SplineInputs;
	for (const FIMComponentValue& Component : Value.Components)
	{
		if (!Component.bSplineInput || Component.CreationMethod == EComponentCreationMethod::UserConstructionScript)
		{
			continue;
		}
		TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("name"), Component.ComponentName);
		Input->SetStringField(TEXT("class"), Component.ClassPath);
		Input->SetNumberField(TEXT("creation_method"), static_cast<int32>(Component.CreationMethod));
		Input->SetBoolField(TEXT("source_cdo_name_match"), Component.bSourceCDONameMatch);
		Input->SetBoolField(TEXT("source_cdo_exact_name_class_match"), Component.bSourceCDOClassMatch);
		Input->SetBoolField(TEXT("bSplineHasBeenEdited"), Component.bSplineHasBeenEdited);
		Input->SetBoolField(TEXT("bInputSplinePointsToConstructionScript"),
			Component.bInputSplinePointsToConstructionScript);
		Input->SetBoolField(TEXT("closed_loop"), Component.bSplineClosedLoop);

		TArray<TSharedPtr<FJsonValue>> Points;
		for (const FIMSplinePointValue& Point : Component.SplinePoints)
		{
			TSharedRef<FJsonObject> PointRow = MakeShared<FJsonObject>();
			PointRow->SetObjectField(TEXT("location"), VectorToJson(Point.Location));
			PointRow->SetObjectField(TEXT("arrive_tangent"), VectorToJson(Point.ArriveTangent));
			PointRow->SetObjectField(TEXT("leave_tangent"), VectorToJson(Point.LeaveTangent));
			PointRow->SetObjectField(TEXT("rotation"), QuaternionToJson(Point.Rotation));
			PointRow->SetObjectField(TEXT("scale"), VectorToJson(Point.Scale));
			PointRow->SetNumberField(TEXT("point_type"), Point.PointType);
			Points.Add(MakeShared<FJsonValueObject>(PointRow));
		}
		Input->SetArrayField(TEXT("points"), MoveTemp(Points));
		SplineInputs.Add(MakeShared<FJsonValueObject>(Input));
	}
	Result->SetArrayField(TEXT("non_ucs_spline_inputs"), MoveTemp(SplineInputs));
	return Result;
}

static void BuildSummary(const TArray<FIMActorValue>& BeforeActors, const TArray<FIMActorValue>& AfterActors,
	const TArray<FIMSourceComponentIdentity>& SourceCDOComponents,
	const TMap<FString, FIMPackageHash>& SourceHashes,
	const TMap<FString, FIMActorComparisonResult>& ActorComparisonResults,
	bool bSemanticPass, const FString& FirstDifference, FString& OutSummary,
	const TArray<TSharedPtr<FJsonValue>>* InputRestoreEvidence = nullptr)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("world_package"), ExpectedWorldPackagePath);
	Root->SetStringField(TEXT("source_blueprint"), SourceBlueprintObjectPath);
	Root->SetStringField(TEXT("child_blueprint"), ChildBlueprintObjectPath);
	Root->SetStringField(TEXT("scope"), TEXT("RURAL_CONSTRUCTION_CONVERSION_DIAGNOSTIC"));
	Root->SetStringField(TEXT("semantic_comparison_status"), bSemanticPass ? TEXT("COMPLETE_PASS") : TEXT("INCOMPLETE"));
	Root->SetStringField(TEXT("first_difference"), FirstDifference.IsEmpty()
		? (bSemanticPass ? TEXT("none") : TEXT("Comparison did not complete.")) : FirstDifference);
	Root->SetBoolField(TEXT("semantic_geometry_equivalent"), bSemanticPass);
	Root->SetBoolField(TEXT("map_saved_by_helper"), false);
	if (InputRestoreEvidence)
	{
		Root->SetArrayField(TEXT("spline_input_restore"), *InputRestoreEvidence);
	}
	Root->SetNumberField(TEXT("actor_count_before"), BeforeActors.Num());
	Root->SetNumberField(TEXT("actor_count_after"), AfterActors.Num());

	int32 BeforeGeneratedMeshes = 0;
	int32 AfterGeneratedMeshes = 0;
	int32 BeforeStaticMeshes = 0;
	int32 AfterStaticMeshes = 0;
	int32 BeforeSplineMeshes = 0;
	int32 AfterSplineMeshes = 0;
	for (const FIMActorValue& Actor : BeforeActors)
	{
		BeforeGeneratedMeshes += Actor.GeneratedMeshCount;
		BeforeStaticMeshes += Actor.StaticMeshCount;
		BeforeSplineMeshes += Actor.SplineMeshCount;
	}
	for (const FIMActorValue& Actor : AfterActors)
	{
		AfterGeneratedMeshes += Actor.GeneratedMeshCount;
		AfterStaticMeshes += Actor.StaticMeshCount;
		AfterSplineMeshes += Actor.SplineMeshCount;
	}
	Root->SetNumberField(TEXT("generated_mesh_components_before"), BeforeGeneratedMeshes);
	Root->SetNumberField(TEXT("generated_mesh_components_after"), AfterGeneratedMeshes);
	Root->SetNumberField(TEXT("static_mesh_components_before"), BeforeStaticMeshes);
	Root->SetNumberField(TEXT("static_mesh_components_after"), AfterStaticMeshes);
	Root->SetNumberField(TEXT("spline_mesh_components_before"), BeforeSplineMeshes);
	Root->SetNumberField(TEXT("spline_mesh_components_after"), AfterSplineMeshes);

	TArray<TSharedPtr<FJsonValue>> CDOComponents;
	for (const FIMSourceComponentIdentity& Component : SourceCDOComponents)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Component.Name);
		Row->SetStringField(TEXT("class"), Component.ClassPath);
		CDOComponents.Add(MakeShared<FJsonValueObject>(Row));
	}
	Root->SetArrayField(TEXT("source_cdo_components"), MoveTemp(CDOComponents));

	bool bBeforeHasDuplicateKey = false;
	bool bBeforeHasMissingKey = false;
	bool bAfterHasDuplicateKey = false;
	bool bAfterHasMissingKey = false;
	const bool bBeforeKeysUnique = ActorKeysAreUnique(BeforeActors,
		bBeforeHasDuplicateKey, bBeforeHasMissingKey);
	const bool bAfterKeysUnique = ActorKeysAreUnique(AfterActors,
		bAfterHasDuplicateKey, bAfterHasMissingKey);
	const bool bKeysUnique = bBeforeKeysUnique && bAfterKeysUnique;
	TMap<FString, int32> AfterByKey;
	if (bKeysUnique)
	{
		for (int32 Index = 0; Index < AfterActors.Num(); ++Index)
		{
			AfterByKey.Add(AfterActors[Index].StableMatchKey, Index);
		}
	}
	TArray<TSharedPtr<FJsonValue>> Pairs;
	TArray<TSharedPtr<FJsonValue>> BeforeUnpaired;
	TArray<TSharedPtr<FJsonValue>> AfterUnpaired;
	int32 MatchedCount = 0;
	if (bKeysUnique)
	{
		for (const FIMActorValue& Before : BeforeActors)
		{
			const int32* AfterIndex = AfterByKey.Find(Before.StableMatchKey);
			if (!AfterIndex)
			{
				if (BeforeUnpaired.Num() < ExpectedActorCount)
				{
					BeforeUnpaired.Add(MakeShared<FJsonValueObject>(BuildActorDiagnostic(Before)));
				}
				continue;
			}
			const FIMActorValue& After = AfterActors[*AfterIndex];
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("stable_match_key"), Before.StableMatchKey);
			Row->SetStringField(TEXT("before_actor_name_for_trace"), Before.ActorName.ToString());
			Row->SetStringField(TEXT("after_actor_name_for_trace"), After.ActorName.ToString());
			Row->SetNumberField(TEXT("initial_seed_before"), Before.InitialRandomSeed);
			Row->SetNumberField(TEXT("initial_seed_after"), After.InitialRandomSeed);
			Row->SetNumberField(TEXT("mutable_seed_before_audit_only"), Before.MutableRandomSeed);
			Row->SetNumberField(TEXT("mutable_seed_after_audit_only"), After.MutableRandomSeed);
			Row->SetNumberField(TEXT("generated_mesh_count_before"), Before.GeneratedMeshCount);
			Row->SetNumberField(TEXT("generated_mesh_count_after"), After.GeneratedMeshCount);
			Row->SetNumberField(TEXT("non_ucs_spline_inputs_before"), Before.NonUserSplineInputCount);
			Row->SetNumberField(TEXT("non_ucs_spline_inputs_after"), After.NonUserSplineInputCount);
			Row->SetObjectField(TEXT("before"), BuildActorDiagnostic(Before));
			Row->SetObjectField(TEXT("after"), BuildActorDiagnostic(After));
			const FIMActorComparisonResult* Comparison = ActorComparisonResults.Find(Before.StableMatchKey);
			Row->SetBoolField(TEXT("semantic_actor_comparison_passed"), Comparison && Comparison->bPassed);
			if (Comparison && !Comparison->FirstDifference.IsEmpty())
			{
				Row->SetStringField(TEXT("semantic_actor_first_difference"), Comparison->FirstDifference);
			}
			TArray<const FIMComponentValue*> BeforeInputs;
			TArray<const FIMComponentValue*> AfterInputs;
			for (const FIMComponentValue& Component : Before.Components)
			{
				if (Component.bSplineInput && Component.CreationMethod != EComponentCreationMethod::UserConstructionScript)
				{
					BeforeInputs.Add(&Component);
				}
			}
			for (const FIMComponentValue& Component : After.Components)
			{
				if (Component.bSplineInput && Component.CreationMethod != EComponentCreationMethod::UserConstructionScript)
				{
					AfterInputs.Add(&Component);
				}
			}
			if (BeforeInputs.Num() == 1 && AfterInputs.Num() == 1)
			{
				FString CurveDifference;
				const bool bCurveEqual = CompareSplineInput(*BeforeInputs[0], *AfterInputs[0], CurveDifference);
				Row->SetBoolField(TEXT("input_curve_equivalent"), bCurveEqual);
				Row->SetStringField(TEXT("input_curve_first_difference"), CurveDifference);
				Row->SetBoolField(TEXT("input_component_name_equal"),
					BeforeInputs[0]->ComponentName == AfterInputs[0]->ComponentName);
			}
			else
			{
				Row->SetBoolField(TEXT("input_curve_equivalent"), false);
				Row->SetStringField(TEXT("input_curve_first_difference"), FString::Printf(
					TEXT("Expected one non-UCS spline input on each actor; found %d before and %d after."),
					BeforeInputs.Num(), AfterInputs.Num()));
			}
			Pairs.Add(MakeShared<FJsonValueObject>(Row));
			++MatchedCount;
		}
		TSet<FString> BeforeKeys;
		for (const FIMActorValue& Before : BeforeActors)
		{
			BeforeKeys.Add(Before.StableMatchKey);
		}
		for (const FIMActorValue& After : AfterActors)
		{
			if (!BeforeKeys.Contains(After.StableMatchKey) && AfterUnpaired.Num() < ExpectedActorCount)
			{
				AfterUnpaired.Add(MakeShared<FJsonValueObject>(BuildActorDiagnostic(After)));
			}
		}
	}
	else
	{
		Root->SetStringField(TEXT("actor_pairing_status"),
			bBeforeHasDuplicateKey || bAfterHasDuplicateKey ? TEXT("AMBIGUOUS_STABLE_KEY")
			: TEXT("INCOMPLETE_STABLE_KEY"));
		for (int32 Index = 0; Index < FMath::Min(BeforeActors.Num(), ExpectedActorCount); ++Index)
		{
			BeforeUnpaired.Add(MakeShared<FJsonValueObject>(BuildActorDiagnostic(BeforeActors[Index])));
		}
		for (int32 Index = 0; Index < FMath::Min(AfterActors.Num(), ExpectedActorCount); ++Index)
		{
			AfterUnpaired.Add(MakeShared<FJsonValueObject>(BuildActorDiagnostic(AfterActors[Index])));
		}
	}
	if (bKeysUnique)
	{
		Root->SetStringField(TEXT("actor_pairing_status"),
			MatchedCount == BeforeActors.Num() && MatchedCount == AfterActors.Num()
				? TEXT("UNIQUE_STABLE_KEYS") : TEXT("UNMATCHED_STABLE_KEYS"));
	}
	Root->SetNumberField(TEXT("matched_actor_count"), MatchedCount);
	Root->SetArrayField(TEXT("actors"), MoveTemp(Pairs));
	Root->SetArrayField(TEXT("actors_before_unpaired"), MoveTemp(BeforeUnpaired));
	Root->SetArrayField(TEXT("actors_after_unpaired"), MoveTemp(AfterUnpaired));

	TArray<FString> PackageNames;
	SourceHashes.GetKeys(PackageNames);
	PackageNames.Sort();
	TArray<TSharedPtr<FJsonValue>> PackageRows;
	for (const FString& PackageName : PackageNames)
	{
		const FIMPackageHash& Hash = SourceHashes.FindChecked(PackageName);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);
		Row->SetStringField(TEXT("filename"), Hash.Filename);
		Row->SetStringField(TEXT("sha1_before"), Hash.BeforeSha1);
		Row->SetStringField(TEXT("sha1_after"), Hash.AfterSha1);
		Row->SetNumberField(TEXT("bytes"), Hash.ByteCount);
		Row->SetBoolField(TEXT("dirty_before"), Hash.bDirtyBefore);
		Row->SetBoolField(TEXT("dirty_after"), Hash.bDirtyAfter);
		PackageRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Root->SetArrayField(TEXT("source_package_hashes"), MoveTemp(PackageRows));
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutSummary);
	FJsonSerializer::Serialize(Root, Writer);
}

} // namespace IMAcousticRuralConstructionMaterialsPrivate

bool IMPrepareRuralConstructionMaterialCopies(UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& Redirects,
	FString& Summary,
	FString& Error)
{
	using namespace IMAcousticRuralConstructionMaterialsPrivate;
	Summary.Reset();
	Error.Reset();

	if (!IsInGameThread() || !GEditor || !IsValid(World) || !World->IsEditorWorld()
		|| GEditor->GetEditorWorldContext().World() != World)
	{
		Error = TEXT("Rural construction material binding requires the owned live Editor world on the game thread.");
		return false;
	}
	if (!World->GetPackage() || World->GetPackage()->GetName() != ExpectedWorldPackagePath)
	{
		Error = TEXT("Rural construction material binding requires the exact isolated preview map: ")
			+ FString(ExpectedWorldPackagePath);
		return false;
	}

	UBlueprint* SourceBlueprint = LoadObject<UBlueprint>(nullptr, SourceBlueprintObjectPath);
	UBlueprintGeneratedClass* SourceClass = SourceBlueprint
		? Cast<UBlueprintGeneratedClass>(SourceBlueprint->GeneratedClass) : nullptr;
	if (!SourceBlueprint || !SourceClass || SourceBlueprint->GeneratedClass->GetPathName() != SourceGeneratedClassPath)
	{
		Error = TEXT("Could not load the exact Rural spline fence source Blueprint GeneratedClass.");
		return false;
	}

	TArray<FIMSourceComponentIdentity> SourceCDOComponents;
	if (!CaptureSourceCDOComponents(SourceClass, SourceCDOComponents, Error))
	{
		return false;
	}

	TArray<FIMSourceTemplate> SourceTemplates;
	if (!CollectSourceTemplates(SourceBlueprint, SourceClass, SourceTemplates, Error))
	{
		return false;
	}
	TMap<FString, FString> SourceToCopyMaterialPaths;
	TSet<FString> CopyMaterialPaths;
	if (!ValidateRedirects(SourceTemplates, Redirects, SourceToCopyMaterialPaths, CopyMaterialPaths, Error))
	{
		return false;
	}

	const FString ChildPackagePath(ChildBlueprintPackagePath);
	const FString ChildObjectPath(ChildBlueprintObjectPath);
	UBlueprint* ChildBlueprint = LoadObject<UBlueprint>(nullptr, *ChildObjectPath);
	if (!ChildBlueprint)
	{
		FString ExistingFilename;
		if (FPackageName::DoesPackageExist(ChildPackagePath, &ExistingFilename))
		{
			Error = TEXT("The exact plugin child package already exists but is not a loadable UBlueprint: ")
				+ ChildPackagePath;
			return false;
		}
		UPackage* Package = CreatePackage(*ChildPackagePath);
		if (!Package)
		{
			Error = TEXT("Could not create the plugin Rural fence Blueprint package.");
			return false;
		}
		ChildBlueprint = FKismetEditorUtilities::CreateBlueprint(
			SourceClass, Package, FName(TEXT("BP_IM_RuralFence_Physical")), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("IMAcousticRuralConstructionMaterials")));
		if (!ChildBlueprint)
		{
			Error = TEXT("Could not create the plugin Rural fence child Blueprint.");
			return false;
		}
		FAssetRegistryModule::AssetCreated(ChildBlueprint);
		Package->MarkPackageDirty();
	}
	if (ChildBlueprint->GetPathName() != ChildBlueprintObjectPath
		|| ChildBlueprint->ParentClass != SourceClass
		|| !Cast<UBlueprintGeneratedClass>(ChildBlueprint->GeneratedClass))
	{
		Error = TEXT("The existing plugin Rural child Blueprint has a path, parent class, or GeneratedClass mismatch.");
		return false;
	}

	TArray<AActor*> ActorsBeforeCompile;
	if (!FindExpectedActors(World, SourceClass, ChildBlueprint->GeneratedClass, ActorsBeforeCompile, Error))
	{
		return false;
	}
	for (AActor* Actor : ActorsBeforeCompile)
	{
		if (Actor->GetClass() != SourceClass && Actor->GetClass() != ChildBlueprint->GeneratedClass)
		{
			Error = TEXT("An observed Rural fence actor has an unknown class before conversion: ") + Actor->GetName();
			return false;
		}
		if (Actor->GetClass() == ChildBlueprint->GeneratedClass
			&& ChildBlueprint->GeneratedClass->GetSuperClass() != SourceClass)
		{
			Error = TEXT("An already converted Rural actor's child Blueprint parent has drifted.");
			return false;
		}
	}

	TMap<FString, FIMPackageHash> SourceHashes;
	if (!AddPackageHash(SourceBlueprint, SourceHashes, Error))
	{
		return false;
	}
	for (const FIMSourceTemplate& Template : SourceTemplates)
	{
		UStaticMeshComponent* MeshTemplate = CastChecked<UStaticMeshComponent>(Template.Component);
		if (!AddPackageHash(MeshTemplate->GetStaticMesh(), SourceHashes, Error))
		{
			return false;
		}
		for (int32 SlotIndex = 0; SlotIndex < MeshTemplate->GetNumMaterials(); ++SlotIndex)
		{
			if (!AddPackageHash(MeshTemplate->GetMaterial(SlotIndex), SourceHashes, Error))
			{
				return false;
			}
		}
	}
	if (SourceHashes.Num() != 6)
	{
		Error = FString::Printf(TEXT("Expected six distinct source package hashes (Blueprint, two meshes, and three material assets), found %d."),
			SourceHashes.Num());
		return false;
	}

	TArray<FIMActorValue> BeforeActors;
	TSet<FString> ComponentNamesToConsider;
	for (AActor* Actor : ActorsBeforeCompile)
	{
		FIMActorValue Value;
		if (!CaptureActor(Actor, SourceClass, ChildBlueprint->GeneratedClass,
				SourceCDOComponents, SourceToCopyMaterialPaths, &ComponentNamesToConsider, Value, Error))
		{
			return false;
		}
		BeforeActors.Add(MoveTemp(Value));
	}
	if (BeforeActors.Num() != ExpectedActorCount)
	{
		Error = TEXT("The precompile snapshot did not capture exactly eight Rural fence actors.");
		return false;
	}
	ComponentNamesToConsider.Add(StaticTemplateName);
	ComponentNamesToConsider.Add(SplineTemplateName);

	if (!PrepareChildConstructionGraph(SourceBlueprint, ChildBlueprint, SourceTemplates, Redirects, Error))
	{
		// This gate precedes conversion. Keep source input diagnostics even if
		// old package migration cannot establish exact parent-node ownership.
		BuildSummary(BeforeActors, TArray<FIMActorValue>(), SourceCDOComponents, SourceHashes,
			TMap<FString, FIMActorComparisonResult>(), false, Error, Summary);
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(ChildBlueprint);
	UBlueprintGeneratedClass* ChildClass = Cast<UBlueprintGeneratedClass>(ChildBlueprint->GeneratedClass);
	if (ChildBlueprint->Status == BS_Error || !ChildClass || ChildBlueprint->ParentClass != SourceClass
		|| ChildClass->GetSuperClass() != SourceClass)
	{
		Error = TEXT("The plugin Rural child Blueprint failed compilation or changed its exact source parent.");
		return false;
	}
	if (!VerifyChildTemplates(ChildBlueprint, ChildClass, SourceTemplates, Redirects, Error))
	{
		return false;
	}
	if (!SaveChildBlueprint(ChildBlueprint, Error))
	{
		return false;
	}

	TArray<AActor*> ActorsToConvert;
	for (AActor* Actor : ActorsBeforeCompile)
	{
		if (Actor->GetClass() == SourceClass)
		{
			ActorsToConvert.Add(Actor);
		}
	}
	TArray<TUniquePtr<FIMSplineInputSnapshot>> SplineInputSnapshots;
	for (AActor* Actor : ActorsToConvert)
	{
		const FIMActorValue* Before = BeforeActors.FindByPredicate([Actor](const FIMActorValue& Value)
			{ return Value.ActorName == Actor->GetFName(); });
		TUniquePtr<FIMSplineInputSnapshot> Snapshot;
		if (!Before || !CaptureSplineInputSnapshot(Actor, *Before, Snapshot, Error))
		{
			if (!Before)
			{
				Error = TEXT("A source actor has no unique frozen input snapshot before conversion.");
			}
			BuildSummary(BeforeActors, TArray<FIMActorValue>(), SourceCDOComponents, SourceHashes,
				TMap<FString, FIMActorComparisonResult>(), false, Error, Summary);
			return false;
		}
		SplineInputSnapshots.Add(MoveTemp(Snapshot));
	}
	if (!ActorsToConvert.IsEmpty())
	{
		UEditorActorSubsystem::DoConvertActors(
			ActorsToConvert, ChildClass, ComponentNamesToConsider, false, FString());
	}

	TArray<AActor*> ActorsAfterConversion;
	TArray<FIMActorValue> AfterActors;
	TMap<FString, FIMActorComparisonResult> ActorComparisonResults;
	TArray<TSharedPtr<FJsonValue>> InputRestoreEvidence;
	if (!FindExpectedActors(World, SourceClass, ChildClass, ActorsAfterConversion, Error))
	{
		BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
			ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
		return false;
	}
	for (AActor* Actor : ActorsAfterConversion)
	{
		if (Actor->GetClass() != ChildClass || ChildClass->GetSuperClass() != SourceClass)
		{
			Error = TEXT("DoConvertActors did not leave every expected actor on the exact plugin child class: ")
				+ Actor->GetName();
			BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
				ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
			return false;
		}
		FIMActorValue Value;
		if (!CaptureActor(Actor, SourceClass, ChildClass, SourceCDOComponents,
				SourceToCopyMaterialPaths, nullptr, Value, Error))
		{
			AfterActors.Add(MoveTemp(Value));
			BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
				ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
			return false;
		}
		const TUniquePtr<FIMSplineInputSnapshot>* Snapshot = SplineInputSnapshots.FindByPredicate(
			[&Value](const TUniquePtr<FIMSplineInputSnapshot>& Candidate)
			{ return Candidate && Candidate->StableMatchKey == Value.StableMatchKey; });
		if (Snapshot)
		{
			TSharedPtr<FJsonObject> RestoreRow;
			const bool bRestored = RestoreConvertedSplineInput(Actor, **Snapshot, RestoreRow, Error);
			if (RestoreRow)
			{
				InputRestoreEvidence.Add(MakeShared<FJsonValueObject>(RestoreRow));
			}
			if (!bRestored)
			{
				AfterActors.Add(MoveTemp(Value));
				BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
					ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
				return false;
			}
			Value = FIMActorValue();
			if (!CaptureActor(Actor, SourceClass, ChildClass, SourceCDOComponents,
					SourceToCopyMaterialPaths, nullptr, Value, Error))
			{
				AfterActors.Add(MoveTemp(Value));
				BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
					ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
				return false;
			}
		}
		AfterActors.Add(MoveTemp(Value));
	}
	if (InputRestoreEvidence.Num() != SplineInputSnapshots.Num())
	{
		Error = TEXT("Not every converted actor matched and applied its unique source spline input snapshot.");
		BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
			ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
		return false;
	}

	if (!SortActorValuesByStableKey(BeforeActors, TEXT("before"), Error)
		|| !SortActorValuesByStableKey(AfterActors, TEXT("after"), Error))
	{
		BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
			ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
		return false;
	}
	if (BeforeActors.Num() != ExpectedActorCount || AfterActors.Num() != ExpectedActorCount)
	{
		Error = TEXT("The semantic comparison requires exactly eight before and after actor snapshots.");
		BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
			ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
		return false;
	}
	for (int32 ActorIndex = 0; ActorIndex < ExpectedActorCount; ++ActorIndex)
	{
		if (BeforeActors[ActorIndex].StableMatchKey != AfterActors[ActorIndex].StableMatchKey)
		{
			Error = TEXT("No unique stable actor pair matched by world transform and RandomStream.InitialSeed.");
			BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
				ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
			return false;
		}
	}

	int32 TotalGeneratedMeshesBefore = 0;
	int32 TotalGeneratedMeshesAfter = 0;
	int32 TotalStaticMeshesBefore = 0;
	int32 TotalStaticMeshesAfter = 0;
	int32 TotalSplineMeshesBefore = 0;
	int32 TotalSplineMeshesAfter = 0;
	bool bAllActorComparisonsPassed = true;
	FString FirstActorDifference;
	for (int32 ActorIndex = 0; ActorIndex < ExpectedActorCount; ++ActorIndex)
	{
		FIMActorComparisonResult& Comparison = ActorComparisonResults.Add(BeforeActors[ActorIndex].StableMatchKey);
		Comparison.bPassed = CompareActorValues(BeforeActors[ActorIndex], AfterActors[ActorIndex],
			SourceToCopyMaterialPaths, Comparison.FirstDifference);
		if (!Comparison.bPassed)
		{
			bAllActorComparisonsPassed = false;
			if (FirstActorDifference.IsEmpty())
			{
				FirstActorDifference = Comparison.FirstDifference;
			}
		}
		TotalGeneratedMeshesBefore += BeforeActors[ActorIndex].GeneratedMeshCount;
		TotalGeneratedMeshesAfter += AfterActors[ActorIndex].GeneratedMeshCount;
		TotalStaticMeshesBefore += BeforeActors[ActorIndex].StaticMeshCount;
		TotalStaticMeshesAfter += AfterActors[ActorIndex].StaticMeshCount;
		TotalSplineMeshesBefore += BeforeActors[ActorIndex].SplineMeshCount;
		TotalSplineMeshesAfter += AfterActors[ActorIndex].SplineMeshCount;
	}
	if (!bAllActorComparisonsPassed)
	{
		Error = TEXT("Rural fence semantic comparison failed: ") + FirstActorDifference;
		BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
			ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
		return false;
	}
	if (TotalGeneratedMeshesBefore != ExpectedGeneratedMeshComponentCount
		|| TotalGeneratedMeshesAfter != ExpectedGeneratedMeshComponentCount
		|| TotalStaticMeshesBefore != ExpectedStaticMeshComponentCount
		|| TotalStaticMeshesAfter != ExpectedStaticMeshComponentCount
		|| TotalSplineMeshesBefore != ExpectedSplineMeshComponentCount
		|| TotalSplineMeshesAfter != ExpectedSplineMeshComponentCount)
	{
		Error = FString::Printf(
			TEXT("Rural fence generated mesh inventory mismatch: before=%d after=%d static=%d/%d spline=%d/%d."),
			TotalGeneratedMeshesBefore, TotalGeneratedMeshesAfter,
			TotalStaticMeshesBefore, TotalStaticMeshesAfter,
			TotalSplineMeshesBefore, TotalSplineMeshesAfter);
		BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
			ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
		return false;
	}
	if (!VerifyPackageHashes(SourceHashes, Error))
	{
		BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
			ActorComparisonResults, false, Error, Summary, &InputRestoreEvidence);
		return false;
	}

	for (const TSharedPtr<FJsonValue>& RestoreRow : InputRestoreEvidence)
	{
		RestoreRow->AsObject()->SetBoolField(TEXT("complete_geometry_verified"), true);
	}
	BuildSummary(BeforeActors, AfterActors, SourceCDOComponents, SourceHashes,
		ActorComparisonResults, true, FString(), Summary, &InputRestoreEvidence);
	return true;
}

#else

bool IMPrepareRuralConstructionMaterialCopies(UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& Redirects,
	FString& Summary,
	FString& Error)
{
	(void)World;
	(void)Redirects;
	Summary.Reset();
	Error = TEXT("Rural construction material copies require WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS.");
	return false;
}

#endif
