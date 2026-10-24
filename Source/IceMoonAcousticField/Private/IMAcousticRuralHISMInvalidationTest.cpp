#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticCoordinates.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

namespace IMAcousticRuralHISMInvalidationTestPrivate
{
constexpr TCHAR TargetMapPackage[] = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr double CapturePositionToleranceMeters = 1.0e-5;
const FVector InstanceMoveDeltaCm(100.0, 0.0, 0.0);
const TCHAR StaleFailureText[] = TEXT("Acoustic bake is stale: scene, material or probe settings changed.");

struct FIMProvenInstance
{
	TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component;
	int32 InstanceIndex = INDEX_NONE;
	FTransform OriginalWorldTransform = FTransform::Identity;
	FTransform MovedWorldTransform = FTransform::Identity;
	int32 CapturedVertexStart = INDEX_NONE;
	int32 CapturedTriangleStart = INDEX_NONE;
	int32 CapturedVertexCount = 0;
	int32 CapturedTriangleCount = 0;
};

static bool HashFileSHA1(const FString& Filename, FString& OutHash, int64& OutBytes, FString& Error)
{
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
	if (!Reader)
	{
		Error = TEXT("Cannot open package file for SHA1: ") + Filename;
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
		if (Chunk <= 0)
		{
			break;
		}

		Reader->Serialize(Buffer.GetData(), Chunk);
		if (Reader->IsError())
		{
			Error = TEXT("Read failed while hashing package file: ") + Filename;
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

static TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& Value)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Add(MakeShared<FJsonValueNumber>(Value.X));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Y));
	Result.Add(MakeShared<FJsonValueNumber>(Value.Z));
	return Result;
}

static void SetTransformFields(TSharedRef<FJsonObject> Json, const FString& Prefix, const FTransform& Transform)
{
	Json->SetArrayField(Prefix + TEXT("_translation_cm"), VectorToJson(Transform.GetLocation()));
	TArray<TSharedPtr<FJsonValue>> Rotation;
	Rotation.Add(MakeShared<FJsonValueNumber>(Transform.GetRotation().X));
	Rotation.Add(MakeShared<FJsonValueNumber>(Transform.GetRotation().Y));
	Rotation.Add(MakeShared<FJsonValueNumber>(Transform.GetRotation().Z));
	Rotation.Add(MakeShared<FJsonValueNumber>(Transform.GetRotation().W));
	Json->SetArrayField(Prefix + TEXT("_rotation_quaternion_xyzw"), MoveTemp(Rotation));
	Json->SetArrayField(Prefix + TEXT("_scale"), VectorToJson(Transform.GetScale3D()));
}

static bool TransformNearlyEquals(const FTransform& A, const FTransform& B, const double Tolerance)
{
	return A.GetLocation().Equals(B.GetLocation(), Tolerance)
		&& A.GetRotation().AngularDistance(B.GetRotation()) <= Tolerance
		&& A.GetScale3D().Equals(B.GetScale3D(), Tolerance);
}

static bool BuildExpectedInstanceGeometry(
	UHierarchicalInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	const FTransform& WorldTransform,
	const FVector& Origin,
	const AIMAcousticBakeVolume* Volume,
	TArray<FVector>& OutVertices,
	TArray<FIntVector>& OutTriangles,
	TArray<int32>& OutMaterialIndices,
	FString& Error)
{
	OutVertices.Reset();
	OutTriangles.Reset();
	OutMaterialIndices.Reset();

	UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
	if (!IsValid(Mesh) || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		Error = TEXT("HISM has no readable LOD0 mesh.");
		return false;
	}

	const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData()
		|| LOD.IndexBuffer.GetArrayView().Num() == 0)
	{
		Error = TEXT("HISM LOD0 CPU positions or indices are unavailable.");
		return false;
	}

	OutVertices.Reserve(static_cast<int32>(Positions.GetNumVertices()));
	for (uint32 VertexIndex = 0; VertexIndex < Positions.GetNumVertices(); ++VertexIndex)
	{
		const FVector LocalPosition(Positions.VertexPosition(VertexIndex));
		const IPLVector3 SDKPosition = ToSDKPosition(WorldTransform.TransformPosition(LocalPosition), Origin);
		OutVertices.Emplace(SDKPosition.x, SDKPosition.y, SDKPosition.z);
	}

	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		UMaterialInterface* SectionMaterial = Component->GetMaterial(static_cast<int32>(Section.MaterialIndex));
		int32 MappedMaterialIndex = INDEX_NONE;
		bool bSolidGeometry = true;
		if (Volume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial)
		{
			UPhysicalMaterial* PhysicalMaterial = Component->BodyInstance.GetPhysMaterialOverride();
			if (!PhysicalMaterial)
			{
				if (FBodyInstance* Body = Component->GetBodyInstance(NAME_None, false, InstanceIndex))
				{
					PhysicalMaterial = Body->GetPhysMaterialOverride();
				}
			}
			if (!PhysicalMaterial && SectionMaterial)
			{
				PhysicalMaterial = SectionMaterial->GetPhysicalMaterial();
			}
			for (int32 MappingIndex = 0; MappingIndex < Volume->PhysicalMaterials.Num(); ++MappingIndex)
			{
				const FIMAcousticPhysicalMaterialMapping& Mapping = Volume->PhysicalMaterials[MappingIndex];
				if (Mapping.PhysicalMaterial.Get() == PhysicalMaterial)
				{
					MappedMaterialIndex = MappingIndex;
					bSolidGeometry = Mapping.bSolidGeometry;
					break;
				}
			}
		}
		else
		{
			for (int32 MappingIndex = 0; MappingIndex < Volume->Materials.Num(); ++MappingIndex)
			{
				if (Volume->Materials[MappingIndex].Material == SectionMaterial)
				{
					MappedMaterialIndex = MappingIndex;
					break;
				}
			}
		}
		if (MappedMaterialIndex == INDEX_NONE)
		{
			Error = FString::Printf(TEXT("HISM section acoustic profile is not in the BakeVolume mapping: %s section %d."),
				*Component->GetPathName(), Section.MaterialIndex);
			return false;
		}
		if (!bSolidGeometry)
		{
			continue;
		}

		for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
		{
			const uint32 IndexOffset = Section.FirstIndex + TriangleIndex * 3U;
			const uint32 A = LOD.IndexBuffer.GetIndex(IndexOffset);
			uint32 B = LOD.IndexBuffer.GetIndex(IndexOffset + 1U);
			uint32 C = LOD.IndexBuffer.GetIndex(IndexOffset + 2U);
			const uint32 VertexCount = static_cast<uint32>(OutVertices.Num());
			if (A >= VertexCount || B >= VertexCount || C >= VertexCount)
			{
				Error = FString::Printf(TEXT("HISM LOD0 section index exceeds the captured vertex range: %s."),
					*Component->GetPathName());
				return false;
			}

			if (WorldTransform.GetDeterminant() < 0.0)
			{
				Swap(B, C);
			}
			OutTriangles.Emplace(static_cast<int32>(A), static_cast<int32>(B), static_cast<int32>(C));
			OutMaterialIndices.Add(MappedMaterialIndex);
		}
	}

	if (Volume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial)
	{
		TArray<int32> VertexRemap;
		VertexRemap.Init(INDEX_NONE, OutVertices.Num());
		TArray<FVector> ReferencedVertices;
		ReferencedVertices.Reserve(OutVertices.Num());
		const auto RemapReferencedVertex = [&VertexRemap, &OutVertices, &ReferencedVertices](int32& VertexIndex)
		{
			int32& RemappedIndex = VertexRemap[VertexIndex];
			if (RemappedIndex == INDEX_NONE)
			{
				RemappedIndex = ReferencedVertices.Num();
				ReferencedVertices.Add(OutVertices[VertexIndex]);
			}
			VertexIndex = RemappedIndex;
		};
		for (FIntVector& Triangle : OutTriangles)
		{
			RemapReferencedVertex(Triangle.X);
			RemapReferencedVertex(Triangle.Y);
			RemapReferencedVertex(Triangle.Z);
		}
		OutVertices = MoveTemp(ReferencedVertices);
	}

	if (OutVertices.IsEmpty() || OutTriangles.IsEmpty()
		|| OutTriangles.Num() != OutMaterialIndices.Num())
	{
		Error = FString::Printf(TEXT("HISM has no complete LOD0 geometry to prove: %s."), *Component->GetPathName());
		return false;
	}
	return true;
}

static bool FindUniqueCapturedVertexBlock(
	const TArray<FVector>& CapturedVertices,
	const TArray<FVector>& ExpectedVertices,
	int32& OutStart)
{
	OutStart = INDEX_NONE;
	if (ExpectedVertices.IsEmpty() || ExpectedVertices.Num() > CapturedVertices.Num())
	{
		return false;
	}

	int32 MatchCount = 0;
	for (int32 Start = 0; Start <= CapturedVertices.Num() - ExpectedVertices.Num(); ++Start)
	{
		if (!CapturedVertices[Start].Equals(ExpectedVertices[0], CapturePositionToleranceMeters))
		{
			continue;
		}

		bool bMatches = true;
		for (int32 VertexIndex = 1; VertexIndex < ExpectedVertices.Num(); ++VertexIndex)
		{
			if (!CapturedVertices[Start + VertexIndex].Equals(
					ExpectedVertices[VertexIndex], CapturePositionToleranceMeters))
			{
				bMatches = false;
				break;
			}
		}
		if (bMatches)
		{
			OutStart = Start;
			++MatchCount;
			if (MatchCount > 1)
			{
				OutStart = INDEX_NONE;
				return false;
			}
		}
	}
	return MatchCount == 1;
}

static bool FindUniqueCapturedTriangleBlock(
	const TArray<FIntVector>& CapturedTriangles,
	const TArray<int32>& CapturedMaterialIndices,
	const int32 CapturedVertexStart,
	const TArray<FIntVector>& ExpectedRelativeTriangles,
	const TArray<int32>& ExpectedMaterialIndices,
	int32& OutStart)
{
	OutStart = INDEX_NONE;
	if (ExpectedRelativeTriangles.IsEmpty()
		|| ExpectedRelativeTriangles.Num() != ExpectedMaterialIndices.Num()
		|| CapturedTriangles.Num() != CapturedMaterialIndices.Num()
		|| ExpectedRelativeTriangles.Num() > CapturedTriangles.Num())
	{
		return false;
	}

	int32 MatchCount = 0;
	for (int32 Start = 0; Start <= CapturedTriangles.Num() - ExpectedRelativeTriangles.Num(); ++Start)
	{
		const FIntVector& FirstRelative = ExpectedRelativeTriangles[0];
		const FIntVector ExpectedFirst(
			CapturedVertexStart + FirstRelative.X,
			CapturedVertexStart + FirstRelative.Y,
			CapturedVertexStart + FirstRelative.Z);
		if (CapturedTriangles[Start] != ExpectedFirst
			|| CapturedMaterialIndices[Start] != ExpectedMaterialIndices[0])
		{
			continue;
		}

		bool bMatches = true;
		for (int32 TriangleIndex = 1; TriangleIndex < ExpectedRelativeTriangles.Num(); ++TriangleIndex)
		{
			const FIntVector& Relative = ExpectedRelativeTriangles[TriangleIndex];
			const FIntVector Expected(
				CapturedVertexStart + Relative.X,
				CapturedVertexStart + Relative.Y,
				CapturedVertexStart + Relative.Z);
			if (CapturedTriangles[Start + TriangleIndex] != Expected
				|| CapturedMaterialIndices[Start + TriangleIndex] != ExpectedMaterialIndices[TriangleIndex])
			{
				bMatches = false;
				break;
			}
		}

		if (bMatches)
		{
			OutStart = Start;
			++MatchCount;
			if (MatchCount > 1)
			{
				OutStart = INDEX_NONE;
				return false;
			}
		}
	}
	return MatchCount == 1;
}

static bool ProveInstanceInCapture(
	UHierarchicalInstancedStaticMeshComponent* Component,
	const int32 InstanceIndex,
	const FTransform& WorldTransform,
	const FVector& Origin,
	const AIMAcousticBakeVolume* Volume,
	const TArray<FVector>& CapturedVertices,
	const TArray<FIntVector>& CapturedTriangles,
	const TArray<int32>& CapturedMaterialIndices,
	FIMProvenInstance& OutProof)
{
	TArray<FVector> ExpectedVertices;
	TArray<FIntVector> ExpectedTriangles;
	TArray<int32> ExpectedMaterialIndices;
	FString GeometryError;
	if (!BuildExpectedInstanceGeometry(Component, InstanceIndex, WorldTransform, Origin, Volume,
			ExpectedVertices, ExpectedTriangles, ExpectedMaterialIndices, GeometryError))
	{
		return false;
	}

	int32 VertexStart = INDEX_NONE;
	if (!FindUniqueCapturedVertexBlock(CapturedVertices, ExpectedVertices, VertexStart))
	{
		return false;
	}

	int32 TriangleStart = INDEX_NONE;
	if (!FindUniqueCapturedTriangleBlock(CapturedTriangles, CapturedMaterialIndices,
			VertexStart, ExpectedTriangles, ExpectedMaterialIndices, TriangleStart))
	{
		return false;
	}

	OutProof.Component = Component;
	OutProof.InstanceIndex = InstanceIndex;
	OutProof.OriginalWorldTransform = WorldTransform;
	OutProof.CapturedVertexStart = VertexStart;
	OutProof.CapturedTriangleStart = TriangleStart;
	OutProof.CapturedVertexCount = ExpectedVertices.Num();
	OutProof.CapturedTriangleCount = ExpectedTriangles.Num();
	return true;
}

static bool FindCapturedStaticHISMInstance(
	UWorld* World,
	AIMAcousticBakeVolume* Volume,
	const FVector& Origin,
	const TArray<FVector>& CapturedVertices,
	const TArray<FIntVector>& CapturedTriangles,
	const TArray<int32>& CapturedMaterialIndices,
	FIMProvenInstance& OutProof,
	int32& OutEligibleInstanceCount)
{
	OutEligibleInstanceCount = 0;
	const FBox Region = Volume->BakeBounds->Bounds.GetBox();
	TArray<UHierarchicalInstancedStaticMeshComponent*> Components;
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		TArray<UHierarchicalInstancedStaticMeshComponent*> ActorComponents;
		ActorIt->GetComponents<UHierarchicalInstancedStaticMeshComponent>(ActorComponents);
		Components.Append(ActorComponents);
	}
	Components.Sort([](const UHierarchicalInstancedStaticMeshComponent& A,
		const UHierarchicalInstancedStaticMeshComponent& B)
	{
		return A.GetPathName() < B.GetPathName();
	});

	for (UHierarchicalInstancedStaticMeshComponent* Component : Components)
	{
		if (!IsValid(Component) || Component->Mobility != EComponentMobility::Static
			|| Component->ComponentHasTag(TEXT("IMAcousticIgnore"))
			|| !Region.Intersect(Component->Bounds.GetBox()))
		{
			continue;
		}
#if WITH_EDITORONLY_DATA
		if (Component->IsVisualizationComponent() || Component->IsEditorOnly())
		{
			continue;
		}
#endif

		UStaticMesh* Mesh = Component->GetStaticMesh();
		if (!IsValid(Mesh) || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
		{
			continue;
		}

		for (int32 InstanceIndex = 0; InstanceIndex < Component->GetInstanceCount(); ++InstanceIndex)
		{
			FTransform WorldTransform;
			if (!Component->GetInstanceTransform(InstanceIndex, WorldTransform, true)
				|| WorldTransform.ContainsNaN() || FMath::IsNearlyZero(WorldTransform.GetDeterminant()))
			{
				continue;
			}

			const FBox OriginalBounds = Mesh->GetBounds().TransformBy(WorldTransform).GetBox();
			if (!Region.Intersect(OriginalBounds))
			{
				continue;
			}
			++OutEligibleInstanceCount;

			FTransform MovedWorldTransform = WorldTransform;
			MovedWorldTransform.AddToTranslation(InstanceMoveDeltaCm);
			const FBox MovedBounds = Mesh->GetBounds().TransformBy(MovedWorldTransform).GetBox();
			if (!Region.Intersect(MovedBounds))
			{
				continue;
			}

			if (ProveInstanceInCapture(Component, InstanceIndex, WorldTransform, Origin, Volume,
					CapturedVertices, CapturedTriangles, CapturedMaterialIndices, OutProof))
			{
				OutProof.MovedWorldTransform = MovedWorldTransform;
				return true;
			}
		}
	}
	return false;
}

static bool WriteReport(const FString& ReportPath, const TSharedRef<FJsonObject>& Report, FString& Error)
{
	const FString ReportDirectory = FPaths::GetPath(ReportPath);
	if (!IFileManager::Get().MakeDirectory(*ReportDirectory, true))
	{
		Error = TEXT("Could not create evidence directory: ") + ReportDirectory;
		return false;
	}

	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Report, Writer))
	{
		Writer->Close();
		Error = TEXT("Could not serialize RuralHISMInvalidation report JSON.");
		return false;
	}
	Writer->Close();
	if (!FFileHelper::SaveStringToFile(
			JsonText, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Error = TEXT("Could not write RuralHISMInvalidation report: ") + ReportPath;
		return false;
	}
	return true;
}

static bool RunTest(FAutomationTestBase* Test)
{
	const double TestStartedAt = FPlatformTime::Seconds();
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ReportPath = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralHISMInvalidation"), RunId, TEXT("report.json"));
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("IMAcousticRuralHISMInvalidation.v1"));
	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetStringField(TEXT("map_package_expected"), TargetMapPackage);
	Report->SetStringField(TEXT("evidence_layer"),
		TEXT("Editor map state, product CaptureScene readback, and ValidateCurrentBake; no visual or PIE claim."));
	Report->SetStringField(TEXT("change_contract"), TEXT("One static in-region HISM instance world transform moves +100 cm on X, then is restored."));
	Report->SetStringField(TEXT("report_path"), ReportPath);

	FString FailureReason;
	bool bHadHardFailure = false;
	bool bHadInconclusive = false;
	auto MarkFailure = [&](const FString& Reason)
	{
		bHadHardFailure = true;
		if (FailureReason.IsEmpty())
		{
			FailureReason = Reason;
		}
	};
	auto MarkInconclusive = [&](const FString& Reason)
	{
		bHadInconclusive = true;
		if (FailureReason.IsEmpty())
		{
			FailureReason = Reason;
		}
	};

	UWorld* World = nullptr;
	AIMAcousticBakeVolume* Volume = nullptr;
	UPackage* MapPackage = nullptr;
	UPackage* BakePackage = nullptr;
	bool bMapDirtyBaseline = false;
	bool bBakeDirtyBaseline = false;
	bool bHaveMapDirtyBaseline = false;
	bool bHaveBakeDirtyBaseline = false;
	FString MapFilename;
	FString BakeFilename;
	FString MapSha1Before;
	FString MapSha1After;
	FString BakeSha1Before;
	FString BakeSha1After;
	int64 MapBytesBefore = 0;
	int64 MapBytesAfter = 0;
	int64 BakeBytesBefore = 0;
	int64 BakeBytesAfter = 0;
	bool bMapHashBeforeValid = false;
	bool bMapHashAfterValid = false;
	bool bBakeHashBeforeValid = false;
	bool bBakeHashAfterValid = false;
	bool bMapLoadReturned = false;
	double MapLoadMs = 0.0;
	double InitialValidateMs = 0.0;
	double InitialCaptureMs = 0.0;
	double InstanceProofMs = 0.0;
	double MovedValidateMs = 0.0;
	double RestoredValidateMs = 0.0;
	double ReportWriteMs = 0.0;
	bool bInitialValidatePassed = false;
	bool bInitialCaptureReadbackPassed = false;
	bool bMovedValidatePassed = false;
	bool bMovedCaptureReadbackPassed = false;
	bool bRestoredValidatePassed = false;
	bool bRestoredCaptureReadbackPassed = false;
	bool bMovedReasonIsFingerprintStale = false;
	bool bMovedFingerprintChanged = false;
	bool bRestoredFingerprintMatchesInitial = false;
	bool bCoreAssertionsPassed = false;
	bool bMutationAttempted = false;
	bool bMoveUpdateReturned = false;
	bool bRestoreUpdateReturned = true;
	bool bRestoredTransformMatchesOriginal = false;
	bool bMapDirtyAfterOperations = false;
	bool bBakeDirtyAfterOperations = false;
	bool bMapDirtyFinal = false;
	bool bBakeDirtyFinal = false;
	bool bHasSelectedInstance = false;
	int32 EligibleInRegionHISMInstances = 0;
	FIMProvenInstance Selected;
	FTransform ActualMovedWorldTransform = FTransform::Identity;
	FTransform ActualRestoredWorldTransform = FTransform::Identity;
	FString InitialFingerprint;
	FString MovedFingerprint;
	FString RestoredFingerprint;
	FString InitialValidateFailure;
	FString MovedValidateFailure;
	FString RestoredValidateFailure;
	FString InitialCaptureFailure;
	FString MovedCaptureFailure;
	FString RestoredCaptureFailure;
	TArray<FVector> InitialCapturedVertices;
	TArray<FIntVector> InitialCapturedTriangles;
	TArray<int32> InitialCapturedMaterialIndices;
	TArray<FVector> MovedCapturedVertices;
	TArray<FIntVector> MovedCapturedTriangles;
	TArray<int32> MovedCapturedMaterialIndices;
	TArray<FVector> RestoredCapturedVertices;
	TArray<FIntVector> RestoredCapturedTriangles;
	TArray<int32> RestoredCapturedMaterialIndices;
	FString BoundBakeFingerprint;
	FString InitialBakeBoundsBox;
	FString InitialBakeCalculatedBox;
	FString InitialKnownHISMBoundsBox;
	FString InitialKnownHISMComponentPath;
	FString InitialKnownHISMCalculatedBox;
	FString InitialKnownHISMInstance100Location;
	bool bInitialKnownHISMRegistered = false;
	int32 InitialKnownHISMInstanceCount = INDEX_NONE;
	int32 InitialKnownHISMMatchCount = 0;

	const double MapHashStartedAt = FPlatformTime::Seconds();
	if (!GEditor)
	{
		MarkInconclusive(TEXT("GEditor is unavailable; the Editor map cannot be freshly loaded."));
	}
	else
	{
		UWorld* StartingWorld = GEditor->GetEditorWorldContext().World();
		UPackage* StartingPackage = StartingWorld ? StartingWorld->GetOutermost() : nullptr;
		UPackage* AlreadyLoadedTarget = FindPackage(nullptr, TargetMapPackage);
		if ((StartingPackage && StartingPackage->IsDirty())
			|| (AlreadyLoadedTarget && AlreadyLoadedTarget->IsDirty()))
		{
			MarkInconclusive(TEXT("An Editor map package is already dirty; refusing to reload or discard it."));
		}
		else if (!FPackageName::DoesPackageExist(TargetMapPackage, &MapFilename))
		{
			MarkInconclusive(TEXT("The persistent Camera3 10 m target map is missing on disk."));
		}
		else
		{
			FString HashError;
			bMapHashBeforeValid = HashFileSHA1(MapFilename, MapSha1Before, MapBytesBefore, HashError);
			if (!bMapHashBeforeValid)
			{
				MarkInconclusive(HashError);
			}
			else
			{
				const double MapLoadStartedAt = FPlatformTime::Seconds();
				bMapLoadReturned = FEditorFileUtils::LoadMap(TargetMapPackage, false, true);
				MapLoadMs = (FPlatformTime::Seconds() - MapLoadStartedAt) * 1000.0;
				if (!bMapLoadReturned)
				{
					MarkInconclusive(TEXT("FEditorFileUtils::LoadMap failed for the persistent Camera3 10 m map."));
				}
				else
				{
					World = GEditor->GetEditorWorldContext().World();
					if (!World || UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) != TargetMapPackage)
					{
						MarkInconclusive(TEXT("The freshly loaded Editor world does not match the exact target map package."));
					}
					else
					{
						// LoadMap can return before every map component has registered its
						// bounds. CaptureScene uses those bounds for the acoustic region.
						// Bring the loaded world to the same component-ready state reached
						// on the next Editor tick before checking the persisted bake.
						World->UpdateWorldComponents(false, false);
						MapPackage = World->GetOutermost();
						bMapDirtyBaseline = MapPackage->IsDirty();
						bHaveMapDirtyBaseline = true;

						TArray<AIMAcousticBakeVolume*> BoundVolumes;
						for (TActorIterator<AIMAcousticBakeVolume> VolumeIt(World); VolumeIt; ++VolumeIt)
						{
							if (IsValid(*VolumeIt) && IsValid(VolumeIt->BakedField)
								&& VolumeIt->BakedField->WorldPackage == TargetMapPackage)
							{
								BoundVolumes.Add(*VolumeIt);
							}
						}
						if (BoundVolumes.Num() != 1)
						{
							MarkInconclusive(FString::Printf(
								TEXT("Expected one BakeVolume bound to the target map; found %d."), BoundVolumes.Num()));
						}
						else
						{
							Volume = BoundVolumes[0];
							BakePackage = Volume->BakedField->GetOutermost();
							BoundBakeFingerprint = Volume->BakedField->SceneFingerprint;
							if (!BakePackage)
							{
								MarkInconclusive(TEXT("The bound Bake asset has no outer package."));
							}
							else
							{
								bBakeDirtyBaseline = BakePackage->IsDirty();
								bHaveBakeDirtyBaseline = true;
								BakeFilename = FPackageName::LongPackageNameToFilename(
									BakePackage->GetName(), FPackageName::GetAssetPackageExtension());
								bBakeHashBeforeValid = HashFileSHA1(
									BakeFilename, BakeSha1Before, BakeBytesBefore, HashError);
								if (!bBakeHashBeforeValid)
								{
									MarkInconclusive(HashError);
								}
								else if (bMapDirtyBaseline || bBakeDirtyBaseline)
								{
									MarkInconclusive(TEXT("The freshly loaded map or bound Bake asset package is dirty at baseline."));
								}
								else if (!Volume->BakeBounds)
								{
									MarkInconclusive(TEXT("The bound BakeVolume has no BakeBounds component."));
								}
								else
								{
									InitialBakeBoundsBox = Volume->BakeBounds->Bounds.GetBox().ToString();
									InitialBakeCalculatedBox = Volume->BakeBounds->CalcBounds(
										Volume->BakeBounds->GetComponentTransform()).GetBox().ToString();
									for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
									{
										TArray<UHierarchicalInstancedStaticMeshComponent*> HISMs;
										ActorIt->GetComponents<UHierarchicalInstancedStaticMeshComponent>(HISMs);
										for (UHierarchicalInstancedStaticMeshComponent* HISM : HISMs)
										{
											// Match the complete component suffix: Contains also matched
											// Component_10, _11, etc. and overwrote this diagnostic identity.
											if (!HISM->GetPathName().EndsWith(TEXT(".InstancedFoliageActor_0.FoliageInstancedStaticMeshComponent_1"), ESearchCase::CaseSensitive)) continue;
											++InitialKnownHISMMatchCount;
											InitialKnownHISMComponentPath = HISM->GetPathName();
											bInitialKnownHISMRegistered = HISM->IsRegistered();
											InitialKnownHISMInstanceCount = HISM->GetInstanceCount();
											InitialKnownHISMBoundsBox = HISM->Bounds.GetBox().ToString();
											InitialKnownHISMCalculatedBox = HISM->CalcBounds(HISM->GetComponentTransform()).GetBox().ToString();
											FTransform InstanceTransform;
											if (HISM->GetInstanceTransform(100, InstanceTransform, true))
												InitialKnownHISMInstance100Location = InstanceTransform.GetLocation().ToString();
										}
									}
									const double InitialValidateStartedAt = FPlatformTime::Seconds();
									bInitialValidatePassed = Volume->ValidateCurrentBake(InitialValidateFailure);
									InitialValidateMs = (FPlatformTime::Seconds() - InitialValidateStartedAt) * 1000.0;
									if (!bInitialValidatePassed)
									{
										const double InitialCaptureStartedAt = FPlatformTime::Seconds();
										bInitialCaptureReadbackPassed = Volume->IM_CaptureSceneGeometryForTest(
											InitialCapturedVertices, InitialCapturedTriangles,
											InitialCapturedMaterialIndices, InitialFingerprint);
										InitialCaptureMs = (FPlatformTime::Seconds() - InitialCaptureStartedAt) * 1000.0;
										if (!bInitialCaptureReadbackPassed) InitialCaptureFailure = Volume->Status;
										MarkFailure(TEXT("Initial ValidateCurrentBake did not PASS: ") + InitialValidateFailure);
									}
									else
									{
										const double InitialCaptureStartedAt = FPlatformTime::Seconds();
										bInitialCaptureReadbackPassed = Volume->IM_CaptureSceneGeometryForTest(
											InitialCapturedVertices, InitialCapturedTriangles,
											InitialCapturedMaterialIndices, InitialFingerprint);
										InitialCaptureMs = (FPlatformTime::Seconds() - InitialCaptureStartedAt) * 1000.0;
										if (!bInitialCaptureReadbackPassed)
										{
											InitialCaptureFailure = Volume->Status;
										}
										if (!bInitialCaptureReadbackPassed || InitialFingerprint.IsEmpty()
											|| InitialFingerprint != BoundBakeFingerprint
											|| InitialCapturedVertices.IsEmpty() || InitialCapturedTriangles.IsEmpty())
										{
											MarkInconclusive(TEXT("Could not read back the exact valid initial CaptureScene geometry and fingerprint: ")
												+ InitialCaptureFailure);
										}
										else
										{
											const double ProofStartedAt = FPlatformTime::Seconds();
											bHasSelectedInstance = FindCapturedStaticHISMInstance(
												World, Volume, Volume->BakeBounds->GetComponentLocation(),
												InitialCapturedVertices, InitialCapturedTriangles,
												InitialCapturedMaterialIndices, Selected,
												EligibleInRegionHISMInstances);
											InstanceProofMs = (FPlatformTime::Seconds() - ProofStartedAt) * 1000.0;
											if (!bHasSelectedInstance)
											{
												MarkInconclusive(FString::Printf(
													TEXT("No unique in-region static HISM instance could be proven in the product CaptureScene vertex and triangle readback; eligible_instances=%d."),
													EligibleInRegionHISMInstances));
											}
											else
											{
												bMutationAttempted = true;
												UHierarchicalInstancedStaticMeshComponent* Component = Selected.Component.Get();
												bMoveUpdateReturned = Component
													&& Component->UpdateInstanceTransform(Selected.InstanceIndex,
														Selected.MovedWorldTransform, true, true, true);
												if (!bMoveUpdateReturned)
												{
													MarkFailure(TEXT("UpdateInstanceTransform failed for the uniquely captured HISM instance."));
												}
												else if (!Component->GetInstanceTransform(
														Selected.InstanceIndex, ActualMovedWorldTransform, true)
													|| !TransformNearlyEquals(ActualMovedWorldTransform,
														Selected.MovedWorldTransform, 1.0e-4))
												{
													MarkFailure(TEXT("The HISM instance world transform did not reach the requested +100 cm X position."));
												}
												else
												{
													const double MovedValidateStartedAt = FPlatformTime::Seconds();
													bMovedValidatePassed = Volume->ValidateCurrentBake(MovedValidateFailure);
													MovedValidateMs = (FPlatformTime::Seconds() - MovedValidateStartedAt) * 1000.0;
													bMovedReasonIsFingerprintStale = !bMovedValidatePassed
														&& MovedValidateFailure.Contains(StaleFailureText, ESearchCase::CaseSensitive);

													bMovedCaptureReadbackPassed = Volume->IM_CaptureSceneGeometryForTest(
														MovedCapturedVertices, MovedCapturedTriangles,
														MovedCapturedMaterialIndices, MovedFingerprint);
													if (!bMovedCaptureReadbackPassed)
													{
														MovedCaptureFailure = Volume->Status;
													}
													bMovedFingerprintChanged = bMovedCaptureReadbackPassed
														&& !MovedFingerprint.IsEmpty()
														&& MovedFingerprint != InitialFingerprint;

													if (bMovedValidatePassed)
													{
														MarkFailure(TEXT("ValidateCurrentBake incorrectly passed after the HISM instance moved."));
													}
													else if (!bMovedReasonIsFingerprintStale)
													{
														MarkFailure(TEXT("Moved ValidateCurrentBake failed for a reason other than the required stale fingerprint: ")
															+ MovedValidateFailure);
													}
													else if (!bMovedCaptureReadbackPassed || !bMovedFingerprintChanged)
													{
														MarkFailure(TEXT("Moved CaptureScene readback did not produce a changed fingerprint: ")
															+ MovedCaptureFailure);
													}
													else
													{
														bCoreAssertionsPassed = true;
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (MapPackage && bHaveMapDirtyBaseline)
	{
		bMapDirtyAfterOperations = MapPackage->IsDirty();
	}
	if (BakePackage && bHaveBakeDirtyBaseline)
	{
		bBakeDirtyAfterOperations = BakePackage->IsDirty();
	}

	if (bMutationAttempted)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = Selected.Component.Get();
		if (!Component)
		{
			bRestoreUpdateReturned = false;
			MarkFailure(TEXT("The selected HISM component disappeared before transform restoration."));
		}
		else
		{
			bRestoreUpdateReturned = Component->UpdateInstanceTransform(
				Selected.InstanceIndex, Selected.OriginalWorldTransform, true, true, true);
			const bool bRestoredReadback = Component->GetInstanceTransform(
				Selected.InstanceIndex, ActualRestoredWorldTransform, true);
			bRestoredTransformMatchesOriginal = bRestoredReadback
				&& TransformNearlyEquals(ActualRestoredWorldTransform, Selected.OriginalWorldTransform, 1.0e-4);
			if (!bRestoreUpdateReturned || !bRestoredTransformMatchesOriginal)
			{
				MarkFailure(TEXT("The original HISM instance world transform could not be fully restored."));
			}

			if (Volume)
			{
				const double RestoredValidateStartedAt = FPlatformTime::Seconds();
				bRestoredValidatePassed = Volume->ValidateCurrentBake(RestoredValidateFailure);
				RestoredValidateMs = (FPlatformTime::Seconds() - RestoredValidateStartedAt) * 1000.0;
				bRestoredCaptureReadbackPassed = Volume->IM_CaptureSceneGeometryForTest(
					RestoredCapturedVertices, RestoredCapturedTriangles,
					RestoredCapturedMaterialIndices, RestoredFingerprint);
				if (!bRestoredCaptureReadbackPassed)
				{
					RestoredCaptureFailure = Volume->Status;
				}
				bRestoredFingerprintMatchesInitial = bRestoredCaptureReadbackPassed
					&& RestoredFingerprint == InitialFingerprint;
				if (!bRestoredValidatePassed || !RestoredValidateFailure.IsEmpty())
				{
					MarkFailure(TEXT("ValidateCurrentBake did not PASS after restoring the HISM instance: ")
						+ RestoredValidateFailure);
				}
				if (!bRestoredCaptureReadbackPassed || !bRestoredFingerprintMatchesInitial)
				{
					MarkFailure(TEXT("Restoring the HISM instance did not restore the initial CaptureScene fingerprint."));
				}
			}
		}
	}

	if (MapPackage && bHaveMapDirtyBaseline && MapPackage->IsDirty() != bMapDirtyBaseline)
	{
		MapPackage->SetDirtyFlag(bMapDirtyBaseline);
	}
	if (BakePackage && bHaveBakeDirtyBaseline && BakePackage->IsDirty() != bBakeDirtyBaseline)
	{
		BakePackage->SetDirtyFlag(bBakeDirtyBaseline);
	}
	if (MapPackage && bHaveMapDirtyBaseline)
	{
		bMapDirtyFinal = MapPackage->IsDirty();
		if (bMapDirtyFinal != bMapDirtyBaseline)
		{
			MarkFailure(TEXT("The map package dirty state was not restored to its original value."));
		}
	}
	if (BakePackage && bHaveBakeDirtyBaseline)
	{
		bBakeDirtyFinal = BakePackage->IsDirty();
		if (bBakeDirtyFinal != bBakeDirtyBaseline)
		{
			MarkFailure(TEXT("The Bake asset package dirty state was not restored to its original value."));
		}
	}

	if (!MapFilename.IsEmpty())
	{
		FString HashError;
		bMapHashAfterValid = HashFileSHA1(MapFilename, MapSha1After, MapBytesAfter, HashError);
		if (!bMapHashAfterValid)
		{
			MarkFailure(HashError);
		}
		else if (bMapHashBeforeValid && (MapSha1Before != MapSha1After || MapBytesBefore != MapBytesAfter))
		{
			MarkFailure(TEXT("The target map disk SHA1 or byte count changed during the test; no map save is permitted."));
		}
	}
	if (!BakeFilename.IsEmpty())
	{
		FString HashError;
		bBakeHashAfterValid = HashFileSHA1(BakeFilename, BakeSha1After, BakeBytesAfter, HashError);
		if (!bBakeHashAfterValid)
		{
			MarkFailure(HashError);
		}
		else if (bBakeHashBeforeValid && (BakeSha1Before != BakeSha1After || BakeBytesBefore != BakeBytesAfter))
		{
			MarkFailure(TEXT("The bound Bake asset disk SHA1 or byte count changed during the test; no asset save is permitted."));
		}
	}

	const bool bDiskShaStable = bMapHashBeforeValid && bMapHashAfterValid
		&& bBakeHashBeforeValid && bBakeHashAfterValid
		&& MapSha1Before == MapSha1After && MapBytesBefore == MapBytesAfter
		&& BakeSha1Before == BakeSha1After && BakeBytesBefore == BakeBytesAfter;
	if (!bDiskShaStable && !bHadHardFailure)
	{
		MarkInconclusive(TEXT("Map/Bake disk SHA1 before/after could not both be proven identical."));
	}

	const bool bPassed = bCoreAssertionsPassed && bInitialValidatePassed
		&& bMovedReasonIsFingerprintStale && bMovedFingerprintChanged
		&& bRestoredValidatePassed && bRestoredFingerprintMatchesInitial
		&& bRestoreUpdateReturned && bRestoredTransformMatchesOriginal
		&& bDiskShaStable && !bHadHardFailure && !bHadInconclusive;
	const FString Status = bPassed ? TEXT("PASS")
		: (bHadHardFailure ? TEXT("FAIL") : TEXT("INCONCLUSIVE"));
	Report->SetStringField(TEXT("status"), Status);
	if (!FailureReason.IsEmpty())
	{
		Report->SetStringField(TEXT("failure_reason"), FailureReason);
	}

	Report->SetBoolField(TEXT("map_load_returned"), bMapLoadReturned);
	Report->SetNumberField(TEXT("map_load_ms"), MapLoadMs);
	Report->SetBoolField(TEXT("initial_validate_passed"), bInitialValidatePassed);
	Report->SetStringField(TEXT("initial_validate_failure"), InitialValidateFailure);
	Report->SetNumberField(TEXT("initial_validate_ms"), InitialValidateMs);
	Report->SetBoolField(TEXT("initial_capture_readback_passed"), bInitialCaptureReadbackPassed);
	Report->SetStringField(TEXT("initial_capture_failure"), InitialCaptureFailure);
	Report->SetStringField(TEXT("bound_bake_fingerprint"), BoundBakeFingerprint);
	Report->SetStringField(TEXT("initial_bake_bounds_box"), InitialBakeBoundsBox);
	Report->SetStringField(TEXT("initial_bake_calculated_box"), InitialBakeCalculatedBox);
	Report->SetStringField(TEXT("initial_known_hism_bounds_box"), InitialKnownHISMBoundsBox);
	Report->SetStringField(TEXT("initial_known_hism_component_path"), InitialKnownHISMComponentPath);
	Report->SetNumberField(TEXT("initial_known_hism_match_count"), InitialKnownHISMMatchCount);
	Report->SetStringField(TEXT("initial_known_hism_calculated_box"), InitialKnownHISMCalculatedBox);
	Report->SetStringField(TEXT("initial_known_hism_instance100_location"), InitialKnownHISMInstance100Location);
	Report->SetBoolField(TEXT("initial_known_hism_registered"), bInitialKnownHISMRegistered);
	Report->SetNumberField(TEXT("initial_known_hism_instance_count"), InitialKnownHISMInstanceCount);
	Report->SetStringField(TEXT("initial_capture_fingerprint"), InitialFingerprint);
	Report->SetNumberField(TEXT("initial_capture_ms"), InitialCaptureMs);
	Report->SetNumberField(TEXT("eligible_in_region_static_hism_instances"), EligibleInRegionHISMInstances);
	Report->SetNumberField(TEXT("instance_capture_proof_ms"), InstanceProofMs);
	Report->SetBoolField(TEXT("selected_instance_proven_in_product_capture"), bHasSelectedInstance);
	Report->SetNumberField(TEXT("captured_vertex_count_initial"), InitialCapturedVertices.Num());
	Report->SetNumberField(TEXT("captured_triangle_count_initial"), InitialCapturedTriangles.Num());
	Report->SetBoolField(TEXT("move_update_returned"), bMoveUpdateReturned);
	Report->SetBoolField(TEXT("moved_validate_passed"), bMovedValidatePassed);
	Report->SetStringField(TEXT("moved_validate_failure"), MovedValidateFailure);
	Report->SetBoolField(TEXT("moved_reason_is_fingerprint_stale"), bMovedReasonIsFingerprintStale);
	Report->SetNumberField(TEXT("moved_validate_ms"), MovedValidateMs);
	Report->SetBoolField(TEXT("moved_capture_readback_passed"), bMovedCaptureReadbackPassed);
	Report->SetStringField(TEXT("moved_capture_failure"), MovedCaptureFailure);
	Report->SetStringField(TEXT("moved_capture_fingerprint"), MovedFingerprint);
	Report->SetBoolField(TEXT("moved_fingerprint_changed"), bMovedFingerprintChanged);
	Report->SetBoolField(TEXT("restore_update_returned"), bRestoreUpdateReturned);
	Report->SetBoolField(TEXT("restored_transform_matches_original"), bRestoredTransformMatchesOriginal);
	Report->SetBoolField(TEXT("restored_validate_passed"), bRestoredValidatePassed);
	Report->SetStringField(TEXT("restored_validate_failure"), RestoredValidateFailure);
	Report->SetNumberField(TEXT("restored_validate_ms"), RestoredValidateMs);
	Report->SetBoolField(TEXT("restored_capture_readback_passed"), bRestoredCaptureReadbackPassed);
	Report->SetStringField(TEXT("restored_capture_failure"), RestoredCaptureFailure);
	Report->SetStringField(TEXT("restored_capture_fingerprint"), RestoredFingerprint);
	Report->SetBoolField(TEXT("restored_fingerprint_matches_initial"), bRestoredFingerprintMatchesInitial);
	Report->SetStringField(TEXT("map_package"), MapPackage ? MapPackage->GetName() : FString());
	Report->SetStringField(TEXT("map_file"), MapFilename);
	Report->SetStringField(TEXT("map_sha1_before"), MapSha1Before);
	Report->SetStringField(TEXT("map_sha1_after"), MapSha1After);
	Report->SetNumberField(TEXT("map_bytes_before"), MapBytesBefore);
	Report->SetNumberField(TEXT("map_bytes_after"), MapBytesAfter);
	Report->SetStringField(TEXT("bake_asset_path"), Volume && Volume->BakedField ? Volume->BakedField->GetPathName() : FString());
	Report->SetStringField(TEXT("bake_package"), BakePackage ? BakePackage->GetName() : FString());
	Report->SetStringField(TEXT("bake_file"), BakeFilename);
	Report->SetStringField(TEXT("bake_sha1_before"), BakeSha1Before);
	Report->SetStringField(TEXT("bake_sha1_after"), BakeSha1After);
	Report->SetNumberField(TEXT("bake_bytes_before"), BakeBytesBefore);
	Report->SetNumberField(TEXT("bake_bytes_after"), BakeBytesAfter);
	Report->SetBoolField(TEXT("disk_sha_stable"), bDiskShaStable);
	Report->SetBoolField(TEXT("map_package_dirty_baseline_known"), bHaveMapDirtyBaseline);
	Report->SetBoolField(TEXT("map_package_dirty_baseline"), bMapDirtyBaseline);
	Report->SetBoolField(TEXT("map_package_dirty_after_operations"), bMapDirtyAfterOperations);
	Report->SetBoolField(TEXT("map_package_dirty_final"), bMapDirtyFinal);
	Report->SetBoolField(TEXT("bake_package_dirty_baseline_known"), bHaveBakeDirtyBaseline);
	Report->SetBoolField(TEXT("bake_package_dirty_baseline"), bBakeDirtyBaseline);
	Report->SetBoolField(TEXT("bake_package_dirty_after_operations"), bBakeDirtyAfterOperations);
	Report->SetBoolField(TEXT("bake_package_dirty_final"), bBakeDirtyFinal);
	if (bHasSelectedInstance)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = Selected.Component.Get();
		Report->SetStringField(TEXT("instance_identity"),
			FString::Printf(TEXT("%s#instance_index=%d"), Component ? *Component->GetPathName() : TEXT("INVALID"),
				Selected.InstanceIndex));
		Report->SetStringField(TEXT("instance_component_path"), Component ? Component->GetPathName() : FString());
		Report->SetNumberField(TEXT("instance_index"), Selected.InstanceIndex);
		Report->SetNumberField(TEXT("captured_vertex_start"), Selected.CapturedVertexStart);
		Report->SetNumberField(TEXT("captured_vertex_count"), Selected.CapturedVertexCount);
		Report->SetNumberField(TEXT("captured_triangle_start"), Selected.CapturedTriangleStart);
		Report->SetNumberField(TEXT("captured_triangle_count"), Selected.CapturedTriangleCount);
		SetTransformFields(Report, TEXT("instance_original_world"), Selected.OriginalWorldTransform);
		SetTransformFields(Report, TEXT("instance_requested_moved_world"), Selected.MovedWorldTransform);
		SetTransformFields(Report, TEXT("instance_actual_moved_world"), ActualMovedWorldTransform);
		SetTransformFields(Report, TEXT("instance_actual_restored_world"), ActualRestoredWorldTransform);
	}

	Report->SetNumberField(TEXT("workflow_elapsed_ms"), (FPlatformTime::Seconds() - TestStartedAt) * 1000.0);
	const double ReportWriteStartedAt = FPlatformTime::Seconds();
	FString ReportWriteFailure;
	const bool bReportWritten = WriteReport(ReportPath, Report, ReportWriteFailure);
	ReportWriteMs = (FPlatformTime::Seconds() - ReportWriteStartedAt) * 1000.0;
	if (!bReportWritten)
	{
		MarkFailure(ReportWriteFailure);
	}

	const bool bFinalPassed = bPassed && bReportWritten && !bHadHardFailure && !bHadInconclusive;
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralHISMInvalidation %s%s"),
		bFinalPassed ? TEXT("PASS") : TEXT("FAIL"),
		bFinalPassed ? TEXT("") : *FString::Printf(TEXT(" status=%s reason=%s"), *Status,
			FailureReason.IsEmpty() ? TEXT("acceptance conditions were not all satisfied") : *FailureReason));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s RuralHISMInvalidation"),
		bFinalPassed ? TEXT("PASS") : TEXT("FAIL"));

	if (bReportWritten)
	{
		Test->AddInfo(FString::Printf(TEXT("Rural HISM invalidation report: %s (report_write_ms=%.3f)"),
			*ReportPath, ReportWriteMs));
	}
	if (!bFinalPassed)
	{
		Test->AddError(FString::Printf(TEXT("Rural HISM invalidation failed closed: %s. Report: %s"),
			FailureReason.IsEmpty() ? TEXT("required stale-invalidation and restoration assertions did not all pass") : *FailureReason,
			*ReportPath));
	}
	return bFinalPassed;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralHISMInvalidationTest,
	"IceMoon.AcousticField.Rural.PersistentHISMEditInvalidatesBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralHISMInvalidationTest::RunTest(const FString&)
{
	return IMAcousticRuralHISMInvalidationTestPrivate::RunTest(this);
}

#endif
