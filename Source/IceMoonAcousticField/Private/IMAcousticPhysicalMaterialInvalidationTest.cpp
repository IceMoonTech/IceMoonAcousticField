#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/WorldInitializationValues.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
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
#include "UObject/UObjectIterator.h"

namespace IMAcousticPhysicalMaterialInvalidationTestPrivate
{
constexpr TCHAR RuralTargetMap[] = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr TCHAR WoodProfilePath[] = TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_Wood.PM_Acoustic_Wood");
constexpr TCHAR SoilProfilePath[] = TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_Soil.PM_Acoustic_Soil");
constexpr TCHAR StaleFailureText[] = TEXT("Acoustic bake is stale: scene, material or probe settings changed.");
constexpr double ProfileEditDelta = 0.001;
constexpr double PendingBoundsWaitSeconds = 5.0;

static FString MakeReportPath(const TCHAR* Suffix)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialInvalidation"),
		FString::Printf(TEXT("pm_invalidation_%s_%s.json"), Suffix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

static bool WriteReport(const FString& ReportPath, const TSharedRef<FJsonObject>& Report, FString& Error)
{
	const FString Directory = FPaths::GetPath(ReportPath);
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		Error = TEXT("Could not create evidence directory: ") + Directory;
		return false;
	}

	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Report, Writer))
	{
		Writer->Close();
		Error = TEXT("Could not serialize PhysicalMaterial invalidation JSON.");
		return false;
	}
	Writer->Close();
	if (!FFileHelper::SaveStringToFile(
			JsonText, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Error = TEXT("Could not write PhysicalMaterial invalidation JSON: ") + ReportPath;
		return false;
	}
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

struct FIMDynamicBlockerCase
{
	FString ComponentType;
	FString ComponentPath;
	FString PhysicalMaterialPath;
	int32 PositiveVertices = 0;
	int32 PositiveTriangles = 0;
	bool bPositiveCapturePassed = false;
	FString PositiveFailure;
	int32 InstanceCountAfterAdd = 0;
	bool bSecondInstanceAdded = false;
	int32 RejectedVertices = 0;
	int32 RejectedTriangles = 0;
	bool bRejectedCaptureReturnedFalse = false;
	bool bFailureHasExactlyOneInstance = false;
	bool bFailureHasUnsupported = false;
	FString RejectedFailure;
	bool bPassed = false;
};

class FIMTransientBlockerFixture
{
public:
	~FIMTransientBlockerFixture()
	{
		if (World && !World->IsBeingCleanedUp())
		{
			World->DestroyWorld(false);
		}
		World = nullptr;
	}

	bool Begin(bool bHierarchical, FIMDynamicBlockerCase& OutCase, FString& Error)
	{
		FWorldInitializationValues WorldValues;
		WorldValues.AllowAudioPlayback(false)
			.RequiresHitProxies(false)
			.CreatePhysicsScene(false)
			.CreateNavigation(false)
			.CreateAISystem(false)
			.ShouldSimulatePhysics(false)
			.CreateFXSystem(false)
			.SetTransactional(false);

		const FName WorldName = MakeUniqueObjectName(GetTransientPackage(), UWorld::StaticClass(),
			FName(bHierarchical ? TEXT("IM_PMInvalidationHISMWorld") : TEXT("IM_PMInvalidationISMWorld")));
		World = UWorld::CreateWorld(EWorldType::EditorPreview, false, WorldName,
			GetTransientPackage(), true, ERHIFeatureLevel::Num, &WorldValues);
		if (!World)
		{
			Error = TEXT("Could not create the isolated transient EditorPreview world.");
			return false;
		}

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Volume = World->SpawnActor<AIMAcousticBakeVolume>(FVector::ZeroVector,
			FRotator::ZeroRotator, SpawnParameters);
		if (!IsValid(Volume) || !IsValid(Volume->BakeBounds))
		{
			Error = TEXT("Could not spawn the transient BakeVolume for the empty blocker fixture.");
			return false;
		}
		Volume->MaterialDrive = EIMAcousticMaterialDrive::PhysicalMaterial;
		Volume->Materials.Reset();

		UPhysicalMaterial* PhysicalMaterial = NewObject<UPhysicalMaterial>(Volume,
			MakeUniqueObjectName(Volume, UPhysicalMaterial::StaticClass(), TEXT("PM_ValidSingleProfile")), RF_Transient);
		if (!IsValid(PhysicalMaterial))
		{
			Error = TEXT("Could not create the transient physical material profile.");
			return false;
		}
		FIMAcousticPhysicalMaterialMapping Mapping;
		Mapping.PhysicalMaterial = PhysicalMaterial;
		Mapping.Absorption = FVector(0.20, 0.30, 0.40);
		Mapping.Scattering = 0.25f;
		Mapping.bSolidGeometry = true;
		Mapping.bSupportsProbes = true;
		Volume->PhysicalMaterials.Add(Mapping);

		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!IsValid(Mesh) || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty()
			|| Mesh->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices() == 0
			|| Mesh->GetRenderData()->LODResources[0].IndexBuffer.GetArrayView().Num() == 0)
		{
			Error = TEXT("The Engine BasicShapes Cube has no readable LOD0 CPU geometry.");
			return false;
		}

		const FName ComponentName = MakeUniqueObjectName(Volume,
			bHierarchical ? UHierarchicalInstancedStaticMeshComponent::StaticClass()
				: UInstancedStaticMeshComponent::StaticClass(),
			bHierarchical ? TEXT("TestHISM") : TEXT("TestISM"));
		if (bHierarchical)
		{
			Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(
				Volume, ComponentName, RF_Transient);
			OutCase.ComponentType = TEXT("HISM");
		}
		else
		{
			Component = NewObject<UInstancedStaticMeshComponent>(Volume, ComponentName, RF_Transient);
			OutCase.ComponentType = TEXT("ISM");
		}
		if (!IsValid(Component))
		{
			Error = TEXT("Could not create the requested transient instanced component.");
			return false;
		}

		Component->SetupAttachment(Volume->GetRootComponent());
		Volume->AddInstanceComponent(Component);
		Component->SetStaticMesh(Mesh);
		Component->SetPhysMaterialOverride(PhysicalMaterial);
		Component->RegisterComponent();
		Instances = Cast<UInstancedStaticMeshComponent>(Component);
		if (!Instances || !IsValid(Component->GetStaticMesh()))
		{
			Error = TEXT("The transient instanced component did not register with the Cube mesh.");
			return false;
		}

		const int32 FirstInstance = Instances->AddInstance(FTransform::Identity, false);
		if (FirstInstance != 0 || Instances->GetInstanceCount() != 1)
		{
			Error = FString::Printf(TEXT("Could not establish the one-instance positive control: first=%d count=%d."),
				FirstInstance, Instances->GetInstanceCount());
			return false;
		}

		OutCase.ComponentPath = Component->GetPathName();
		OutCase.PhysicalMaterialPath = PhysicalMaterial->GetPathName();
		return true;
	}

	AIMAcousticBakeVolume* GetVolume() const { return Volume; }
	UInstancedStaticMeshComponent* GetInstances() const { return Instances; }
	UStaticMeshComponent* GetComponent() const { return Component; }
	UStaticMesh* GetMesh() const { return Mesh; }

private:
	UWorld* World = nullptr;
	AIMAcousticBakeVolume* Volume = nullptr;
	UStaticMesh* Mesh = nullptr;
	UStaticMeshComponent* Component = nullptr;
	UInstancedStaticMeshComponent* Instances = nullptr;
};

static FIMDynamicBlockerCase RunDynamicBlockerCase(FAutomationTestBase* Test, const bool bHierarchical)
{
	FIMDynamicBlockerCase Result;
	FIMTransientBlockerFixture Fixture;
	FString SetupError;
	if (!Fixture.Begin(bHierarchical, Result, SetupError))
	{
		Result.PositiveFailure = SetupError;
		return Result;
	}

	AIMAcousticBakeVolume* Volume = Fixture.GetVolume();
	UStaticMeshComponent* Component = Fixture.GetComponent();
	UInstancedStaticMeshComponent* Instances = Fixture.GetInstances();
	if (!Volume || !Component || !Instances)
	{
		Result.PositiveFailure = TEXT("The fixture lost its transient volume or component.");
		return Result;
	}

	const bool bPositiveCaptureReturned = Volume->IM_CaptureDynamicGeometryForTest(
		Component, Result.PositiveVertices, Result.PositiveTriangles, Result.PositiveFailure);
	Result.bPositiveCapturePassed = bPositiveCaptureReturned && Result.PositiveVertices > 0
		&& Result.PositiveTriangles > 0 && Result.PositiveFailure.IsEmpty();
	if (!Result.bPositiveCapturePassed)
	{
		return Result;
	}

	// The only input change between captures is adding a second identical instance.
	const int32 AddedInstance = Instances->AddInstance(FTransform::Identity, false);
	Result.InstanceCountAfterAdd = Instances->GetInstanceCount();
	Result.bSecondInstanceAdded = AddedInstance == 1 && Result.InstanceCountAfterAdd == 2;
	if (!Result.bSecondInstanceAdded)
	{
		Result.RejectedFailure = FString::Printf(
			TEXT("The second instance was not added as the sole negative-control change: index=%d count=%d."),
			AddedInstance, Result.InstanceCountAfterAdd);
		return Result;
	}

	Result.bRejectedCaptureReturnedFalse = !Volume->IM_CaptureDynamicGeometryForTest(
		Component, Result.RejectedVertices, Result.RejectedTriangles, Result.RejectedFailure);
	Result.bFailureHasExactlyOneInstance = Result.RejectedFailure.Contains(
		TEXT("exactly one instance"), ESearchCase::CaseSensitive);
	Result.bFailureHasUnsupported = Result.RejectedFailure.Contains(
		TEXT("unsupported"), ESearchCase::CaseSensitive);
	Result.bPassed = Result.bPositiveCapturePassed && Result.bSecondInstanceAdded
		&& Result.bRejectedCaptureReturnedFalse && Result.RejectedVertices == 0
		&& Result.RejectedTriangles == 0 && Result.bFailureHasExactlyOneInstance
		&& Result.bFailureHasUnsupported;
	return Result;
}

static TSharedRef<FJsonObject> DynamicCaseToJson(const FIMDynamicBlockerCase& Case)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("component_type"), Case.ComponentType);
	Json->SetStringField(TEXT("component_path"), Case.ComponentPath);
	Json->SetStringField(TEXT("physical_material_path"), Case.PhysicalMaterialPath);
	Json->SetBoolField(TEXT("positive_capture_passed"), Case.bPositiveCapturePassed);
	Json->SetNumberField(TEXT("positive_vertices"), Case.PositiveVertices);
	Json->SetNumberField(TEXT("positive_triangles"), Case.PositiveTriangles);
	Json->SetStringField(TEXT("positive_failure"), Case.PositiveFailure);
	Json->SetBoolField(TEXT("second_instance_added"), Case.bSecondInstanceAdded);
	Json->SetNumberField(TEXT("instance_count_after_add"), Case.InstanceCountAfterAdd);
	Json->SetBoolField(TEXT("rejected_capture_returned_false"), Case.bRejectedCaptureReturnedFalse);
	Json->SetNumberField(TEXT("rejected_vertices"), Case.RejectedVertices);
	Json->SetNumberField(TEXT("rejected_triangles"), Case.RejectedTriangles);
	Json->SetStringField(TEXT("rejected_failure"), Case.RejectedFailure);
	Json->SetBoolField(TEXT("failure_has_exactly_one_instance"), Case.bFailureHasExactlyOneInstance);
	Json->SetBoolField(TEXT("failure_has_unsupported"), Case.bFailureHasUnsupported);
	Json->SetBoolField(TEXT("passed"), Case.bPassed);
	return Json;
}

static bool CountDirtyPersistentPackages(UPackage* ExceptPackage, FString& FirstDirtyPackage)
{
	FirstDirtyPackage.Reset();
	bool bAnyDirty = false;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!IsValid(Package) || Package == GetTransientPackage()
			|| Package->HasAnyFlags(RF_Transient)
			|| Package->HasAnyPackageFlags(PKG_PlayInEditor)
			// Compiled-in /Script packages cannot be saved as content; startup CDO creation
			// can dirty them without introducing user-authored persistent package edits.
			|| Package->HasAnyPackageFlags(PKG_CompiledIn)
			|| Package == ExceptPackage)
		{
			continue;
		}
		if (Package->IsDirty())
		{
			bAnyDirty = true;
			if (FirstDirtyPackage.IsEmpty())
			{
				FirstDirtyPackage = Package->GetName();
			}
		}
	}
	return bAnyDirty;
}

struct FIMMappingSnapshot
{
	TObjectPtr<UPhysicalMaterial> PhysicalMaterial;
	FVector Absorption = FVector::ZeroVector;
	float Scattering = 0.0f;
	bool bSolidGeometry = false;
	bool bSupportsProbes = false;
};

enum class EIMRuralMutation : uint8
{
	AbsorptionLowBand,
	Scattering,
	WoodSolidGeometry,
	SoilProbeSupport
};

struct FIMRuralMutationCase
{
	EIMRuralMutation Mutation = EIMRuralMutation::AbsorptionLowBand;
	FString Name;
	FString UniqueVariable;
	FString PhysicalMaterialPath;
	int32 MappingIndex = INDEX_NONE;
	bool bBaselineValidatePassed = false;
	FString BaselineValidateFailure;
	FString BaselineFingerprint;
	bool bAfterValidatePassed = false;
	FString AfterValidateFailure;
	bool bAfterFailureIsStaleFingerprint = false;
	bool bRestoredValidatePassed = false;
	FString RestoredValidateFailure;
	bool bRestoredFingerprintMatchesBaseline = false;
	bool bPassed = false;
};

static int32 FindUniquePhysicalMaterialMapping(
	const AIMAcousticBakeVolume* Volume, const FString& ExpectedAssetPath)
{
	if (!Volume)
	{
		return INDEX_NONE;
	}
	int32 Match = INDEX_NONE;
	for (int32 Index = 0; Index < Volume->PhysicalMaterials.Num(); ++Index)
	{
		const UPhysicalMaterial* PhysicalMaterial = Volume->PhysicalMaterials[Index].PhysicalMaterial.Get();
		if (PhysicalMaterial && PhysicalMaterial->GetPathName().Equals(
				ExpectedAssetPath, ESearchCase::CaseSensitive))
		{
			if (Match != INDEX_NONE)
			{
				return INDEX_NONE;
			}
			Match = Index;
		}
	}
	return Match;
}

static bool CaptureMappingSnapshot(const AIMAcousticBakeVolume* Volume,
	TArray<FIMMappingSnapshot>& OutSnapshot)
{
	OutSnapshot.Reset();
	if (!Volume || Volume->PhysicalMaterials.IsEmpty())
	{
		return false;
	}
	OutSnapshot.Reserve(Volume->PhysicalMaterials.Num());
	for (const FIMAcousticPhysicalMaterialMapping& Mapping : Volume->PhysicalMaterials)
	{
		FIMMappingSnapshot& Snapshot = OutSnapshot.AddDefaulted_GetRef();
		Snapshot.PhysicalMaterial = Mapping.PhysicalMaterial;
		Snapshot.Absorption = Mapping.Absorption;
		Snapshot.Scattering = Mapping.Scattering;
		Snapshot.bSolidGeometry = Mapping.bSolidGeometry;
		Snapshot.bSupportsProbes = Mapping.bSupportsProbes;
	}
	return true;
}

static bool MappingSnapshotMatches(const AIMAcousticBakeVolume* Volume,
	const TArray<FIMMappingSnapshot>& Snapshot)
{
	if (!Volume || Volume->PhysicalMaterials.Num() != Snapshot.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Snapshot.Num(); ++Index)
	{
		const FIMAcousticPhysicalMaterialMapping& Mapping = Volume->PhysicalMaterials[Index];
		const FIMMappingSnapshot& Expected = Snapshot[Index];
		if (Mapping.PhysicalMaterial.Get() != Expected.PhysicalMaterial.Get()
			|| Mapping.Absorption.X != Expected.Absorption.X
			|| Mapping.Absorption.Y != Expected.Absorption.Y
			|| Mapping.Absorption.Z != Expected.Absorption.Z
			|| Mapping.Scattering != Expected.Scattering
			|| Mapping.bSolidGeometry != Expected.bSolidGeometry
			|| Mapping.bSupportsProbes != Expected.bSupportsProbes)
		{
			return false;
		}
	}
	return true;
}

static bool OnlySelectedMappingVariableChanged(const AIMAcousticBakeVolume* Volume,
	const TArray<FIMMappingSnapshot>& Snapshot, const FIMRuralMutationCase& Case)
{
	if (!Volume || Volume->PhysicalMaterials.Num() != Snapshot.Num()
		|| !Snapshot.IsValidIndex(Case.MappingIndex))
	{
		return false;
	}
	for (int32 Index = 0; Index < Snapshot.Num(); ++Index)
	{
		const FIMAcousticPhysicalMaterialMapping& Mapping = Volume->PhysicalMaterials[Index];
		const FIMMappingSnapshot& Before = Snapshot[Index];
		if (Mapping.PhysicalMaterial.Get() != Before.PhysicalMaterial.Get())
		{
			return false;
		}
		if (Index != Case.MappingIndex)
		{
			if (Mapping.Absorption.X != Before.Absorption.X || Mapping.Absorption.Y != Before.Absorption.Y
				|| Mapping.Absorption.Z != Before.Absorption.Z || Mapping.Scattering != Before.Scattering
				|| Mapping.bSolidGeometry != Before.bSolidGeometry
				|| Mapping.bSupportsProbes != Before.bSupportsProbes)
			{
				return false;
			}
			continue;
		}

		switch (Case.Mutation)
		{
		case EIMRuralMutation::AbsorptionLowBand:
			if (Mapping.Absorption.X != Before.Absorption.X + ProfileEditDelta
				|| Mapping.Absorption.Y != Before.Absorption.Y || Mapping.Absorption.Z != Before.Absorption.Z
				|| Mapping.Scattering != Before.Scattering || Mapping.bSolidGeometry != Before.bSolidGeometry
				|| Mapping.bSupportsProbes != Before.bSupportsProbes) return false;
			break;
		case EIMRuralMutation::Scattering:
			if (Mapping.Absorption.X != Before.Absorption.X || Mapping.Absorption.Y != Before.Absorption.Y
				|| Mapping.Absorption.Z != Before.Absorption.Z
				|| Mapping.Scattering != Before.Scattering + static_cast<float>(ProfileEditDelta)
				|| Mapping.bSolidGeometry != Before.bSolidGeometry
				|| Mapping.bSupportsProbes != Before.bSupportsProbes) return false;
			break;
		case EIMRuralMutation::WoodSolidGeometry:
			if (Mapping.Absorption.X != Before.Absorption.X || Mapping.Absorption.Y != Before.Absorption.Y
				|| Mapping.Absorption.Z != Before.Absorption.Z || Mapping.Scattering != Before.Scattering
				|| Mapping.bSolidGeometry != false || Mapping.bSupportsProbes != Before.bSupportsProbes) return false;
			break;
		case EIMRuralMutation::SoilProbeSupport:
			if (Mapping.Absorption.X != Before.Absorption.X || Mapping.Absorption.Y != Before.Absorption.Y
				|| Mapping.Absorption.Z != Before.Absorption.Z || Mapping.Scattering != Before.Scattering
				|| Mapping.bSolidGeometry != Before.bSolidGeometry || Mapping.bSupportsProbes != false) return false;
			break;
		}
	}
	return true;
}

static bool FindPendingBounds(UWorld* World, FString& OutComponentPath)
{
	OutComponentPath.Reset();
	if (!World)
	{
		return false;
	}
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		TArray<UStaticMeshComponent*> MeshComponents;
		ActorIt->GetComponents<UStaticMeshComponent>(MeshComponents);
		for (UStaticMeshComponent* MeshComponent : MeshComponents)
		{
			if (IsValid(MeshComponent) && MeshComponent->IsRegistered()
				&& MeshComponent->Bounds.BoxExtent.IsNearlyZero()
				&& !MeshComponent->CalcBounds(MeshComponent->GetComponentTransform())
					.GetBox().GetExtent().IsNearlyZero())
			{
				OutComponentPath = MeshComponent->GetPathName();
				return true;
			}
		}
	}
	return false;
}

class FIMRuralPhysicalMaterialInvalidationCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMRuralPhysicalMaterialInvalidationCommand(FAutomationTestBase* InTest)
		: Test(InTest), ReportPath(MakeReportPath(TEXT("rural"))), StartedAt(FPlatformTime::Seconds())
	{
	}

	bool Update() override
	{
		if (bFinished)
		{
			return true;
		}
		if (Stage == EIMStage::Start) return Start();
		if (Stage == EIMStage::WaitTargetTick) return WaitForTargetTick();
		if (Stage == EIMStage::WaitTargetReady) return WaitForTargetReady();
		if (Stage == EIMStage::CaseBaseline) return ValidateCaseBaseline();
		if (Stage == EIMStage::ApplyMutation) return ApplyCurrentMutation();
		if (Stage == EIMStage::ValidateMutation) return ValidateCurrentMutation();
		if (Stage == EIMStage::RestoreMutation) return RestoreCurrentMutation();
		if (Stage == EIMStage::ValidateRestored) return ValidateRestoredMutation();
		if (Stage == EIMStage::PrepareStartupRestore) return PrepareStartupRestore();
		if (Stage == EIMStage::WaitStartupTick) return VerifyStartupRestore();
		if (Stage == EIMStage::Finish) return Finish();
		return FailAndRestore(TEXT("Unexpected Rural invalidation command stage."));
	}

private:
	enum class EIMStage : uint8
	{
		Start,
		WaitTargetTick,
		WaitTargetReady,
		CaseBaseline,
		ApplyMutation,
		ValidateMutation,
		RestoreMutation,
		ValidateRestored,
		PrepareStartupRestore,
		WaitStartupTick,
		Finish
	};

	bool FailAndRestore(const FString& Reason)
	{
		MarkFailure(Reason);
		Stage = bMutationApplied ? EIMStage::RestoreMutation : EIMStage::PrepareStartupRestore;
		return false;
	}

	void MarkFailure(const FString& Reason)
	{
		bFailed = true;
		if (FailureReason.IsEmpty())
		{
			FailureReason = Reason;
		}
	}

	UWorld* CurrentEditorWorld() const
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FString CurrentWorldPackageName() const
	{
		UWorld* World = CurrentEditorWorld();
		return World ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) : FString();
	}

	bool Start()
	{
		if (!GEditor || GEditor->PlayWorld)
		{
			return FailAndRestore(TEXT("Rural physical-material invalidation requires an idle Editor with no PIE world."));
		}
		UWorld* StartupWorld = CurrentEditorWorld();
		if (!StartupWorld)
		{
			return FailAndRestore(TEXT("The original Editor startup map is unavailable."));
		}
		StartupMapPackage = UWorld::RemovePIEPrefix(StartupWorld->GetOutermost()->GetName());
		FString DirtyPath;
		if (CountDirtyPersistentPackages(nullptr, DirtyPath))
		{
			return FailAndRestore(TEXT("Refusing map transitions because a persistent package is already dirty: ") + DirtyPath);
		}
		if (StartupMapPackage.IsEmpty() || StartupMapPackage.StartsWith(TEXT("/Temp/"), ESearchCase::CaseSensitive))
		{
			return FailAndRestore(TEXT("The original startup map does not have a reloadable package path."));
		}
		if (!FPackageName::DoesPackageExist(RuralTargetMap, &MapFilename))
		{
			return FailAndRestore(TEXT("The exact persistent Rural Camera3 target map is missing from disk."));
		}
		FString HashError;
		bMapHashBeforeValid = HashFileSHA1(MapFilename, MapSha1Before, MapBytesBefore, HashError);
		if (!bMapHashBeforeValid)
		{
			return FailAndRestore(HashError);
		}

		bTargetLoadReturned = FEditorFileUtils::LoadMap(RuralTargetMap, false, true);
		bTargetMapWasLoaded = CurrentWorldPackageName() == RuralTargetMap;
		if (!bTargetLoadReturned || !bTargetMapWasLoaded)
		{
			return FailAndRestore(TEXT("FEditorFileUtils::LoadMap did not load the exact persistent Rural target map."));
		}
		TargetMapLoadFrame = GFrameCounter;
		TargetReadyWaitStartedAt = FPlatformTime::Seconds();
		Stage = EIMStage::WaitTargetTick;
		return false;
	}

	bool WaitForTargetTick()
	{
		if (!GEditor || GEditor->PlayWorld)
		{
			return FailAndRestore(TEXT("PIE started while waiting for the persistent target map."));
		}
		if (GFrameCounter <= TargetMapLoadFrame)
		{
			return false;
		}
		Stage = EIMStage::WaitTargetReady;
		return false;
	}

	bool WaitForTargetReady()
	{
		if (!GEditor || GEditor->PlayWorld || CurrentWorldPackageName() != RuralTargetMap)
		{
			return FailAndRestore(TEXT("The Editor world changed before the Rural target became ready."));
		}
		UWorld* World = CurrentEditorWorld();
		FString PendingComponent;
		if (FindPendingBounds(World, PendingComponent))
		{
			PendingBoundsComponent = PendingComponent;
			if (FPlatformTime::Seconds() - TargetReadyWaitStartedAt <= PendingBoundsWaitSeconds)
			{
				return false;
			}
			return FailAndRestore(TEXT("Registered Rural mesh bounds remained pending for more than five seconds: ")
				+ PendingComponent);
		}

		int32 BakeVolumeCount = 0;
		for (TActorIterator<AIMAcousticBakeVolume> VolumeIt(World); VolumeIt; ++VolumeIt)
		{
			if (IsValid(*VolumeIt))
			{
				++BakeVolumeCount;
				Volume = *VolumeIt;
			}
		}
		if (BakeVolumeCount != 1 || !IsValid(Volume))
		{
			return FailAndRestore(FString::Printf(
				TEXT("Expected exactly one BakeVolume in the loaded Rural map; found %d."), BakeVolumeCount));
		}
		if (GEditor->PlayWorld || Volume->MaterialDrive != EIMAcousticMaterialDrive::PhysicalMaterial
			|| !IsValid(Volume->BakedField) || Volume->BakedField->WorldPackage != RuralTargetMap
			|| Volume->BakedField->SceneFingerprint.IsEmpty())
		{
			return FailAndRestore(TEXT("The unique Rural BakeVolume is not bound to a valid PhysicalMaterial bake for the target map."));
		}
		if (!Volume->BakeBounds)
		{
			return FailAndRestore(TEXT("The unique Rural BakeVolume has no BakeBounds."));
		}

		MapPackage = World->GetOutermost();
		if (!MapPackage || MapPackage->IsDirty())
		{
			return FailAndRestore(TEXT("The persistent target map package is dirty after load."));
		}
		BakePackage = Volume->BakedField->GetOutermost();
		if (!BakePackage || BakePackage->IsDirty())
		{
			return FailAndRestore(TEXT("The bound Bake asset package is missing or dirty at baseline."));
		}
		BakeAssetPath = Volume->BakedField->GetPathName();
		BakeFilename = FPackageName::LongPackageNameToFilename(
			BakePackage->GetName(), FPackageName::GetAssetPackageExtension());
		FString HashError;
		bBakeHashBeforeValid = HashFileSHA1(BakeFilename, BakeSha1Before, BakeBytesBefore, HashError);
		if (!bBakeHashBeforeValid)
		{
			return FailAndRestore(HashError);
		}

		const int32 WoodIndex = FindUniquePhysicalMaterialMapping(Volume, WoodProfilePath);
		const int32 SoilIndex = FindUniquePhysicalMaterialMapping(Volume, SoilProfilePath);
		if (WoodIndex == INDEX_NONE || SoilIndex == INDEX_NONE || WoodIndex == SoilIndex
			|| !Volume->PhysicalMaterials.IsValidIndex(WoodIndex)
			|| !Volume->PhysicalMaterials.IsValidIndex(SoilIndex))
		{
			return FailAndRestore(TEXT("The persistent PhysicalMaterials table lacks unique Wood and Soil profile identities."));
		}
		const FIMAcousticPhysicalMaterialMapping& Wood = Volume->PhysicalMaterials[WoodIndex];
		const FIMAcousticPhysicalMaterialMapping& Soil = Volume->PhysicalMaterials[SoilIndex];
		if (!Wood.bSolidGeometry || Wood.bSupportsProbes
			|| !Soil.bSolidGeometry || !Soil.bSupportsProbes)
		{
			return FailAndRestore(TEXT("Wood must begin solid/non-supporting and Soil solid/supporting for legal role toggles."));
		}
		if (!CaptureMappingSnapshot(Volume, MappingBaseline))
		{
			return FailAndRestore(TEXT("Could not snapshot the persistent PhysicalMaterials table."));
		}

		FIMRuralMutationCase& Absorption = Cases.AddDefaulted_GetRef();
		Absorption.Mutation = EIMRuralMutation::AbsorptionLowBand;
		Absorption.Name = TEXT("wood_absorption_low_band_plus_0_001");
		Absorption.UniqueVariable = TEXT("PhysicalMaterials[Wood].Absorption.X");
		Absorption.MappingIndex = WoodIndex;
		Absorption.PhysicalMaterialPath = Wood.PhysicalMaterial->GetPathName();
		FIMRuralMutationCase& Scattering = Cases.AddDefaulted_GetRef();
		Scattering.Mutation = EIMRuralMutation::Scattering;
		Scattering.Name = TEXT("wood_scattering_plus_0_001");
		Scattering.UniqueVariable = TEXT("PhysicalMaterials[Wood].Scattering");
		Scattering.MappingIndex = WoodIndex;
		Scattering.PhysicalMaterialPath = Wood.PhysicalMaterial->GetPathName();
		FIMRuralMutationCase& WoodSolid = Cases.AddDefaulted_GetRef();
		WoodSolid.Mutation = EIMRuralMutation::WoodSolidGeometry;
		WoodSolid.Name = TEXT("wood_solid_geometry_true_to_false_support_remains_false");
		WoodSolid.UniqueVariable = TEXT("PhysicalMaterials[Wood].bSolidGeometry");
		WoodSolid.MappingIndex = WoodIndex;
		WoodSolid.PhysicalMaterialPath = Wood.PhysicalMaterial->GetPathName();
		FIMRuralMutationCase& SoilSupport = Cases.AddDefaulted_GetRef();
		SoilSupport.Mutation = EIMRuralMutation::SoilProbeSupport;
		SoilSupport.Name = TEXT("soil_supports_probes_true_to_false_remains_solid");
		SoilSupport.UniqueVariable = TEXT("PhysicalMaterials[Soil].bSupportsProbes");
		SoilSupport.MappingIndex = SoilIndex;
		SoilSupport.PhysicalMaterialPath = Soil.PhysicalMaterial->GetPathName();

		InitialSceneFingerprint = Volume->BakedField->SceneFingerprint;
		Stage = EIMStage::CaseBaseline;
		return false;
	}

	bool ValidateCaseBaseline()
	{
		if (CaseIndex >= Cases.Num())
		{
			Stage = EIMStage::PrepareStartupRestore;
			return false;
		}
		if (!MappingSnapshotMatches(Volume, MappingBaseline))
		{
			return FailAndRestore(TEXT("The PhysicalMaterials table changed outside the currently requested test variable."));
		}
		FIMRuralMutationCase& Case = Cases[CaseIndex];
		FString Failure;
		Case.bBaselineValidatePassed = Volume->ValidateCurrentBake(Failure);
		Case.BaselineValidateFailure = Failure;
		Case.BaselineFingerprint = Volume->BakedField->SceneFingerprint;
		if (!Case.bBaselineValidatePassed || !Failure.IsEmpty()
			|| Case.BaselineFingerprint != InitialSceneFingerprint)
		{
			MarkFailure(TEXT("ValidateCurrentBake did not PASS immediately before ") + Case.UniqueVariable
				+ TEXT(": ") + Failure);
			Stage = EIMStage::PrepareStartupRestore;
			return false;
		}
		Stage = EIMStage::ApplyMutation;
		return false;
	}

	void ApplyMutationField(FIMAcousticPhysicalMaterialMapping& Mapping, const FIMRuralMutationCase& Case)
	{
		const FIMMappingSnapshot& Before = MappingBaseline[Case.MappingIndex];
		switch (Case.Mutation)
		{
		case EIMRuralMutation::AbsorptionLowBand:
			Mapping.Absorption.X = Before.Absorption.X + ProfileEditDelta;
			break;
		case EIMRuralMutation::Scattering:
			Mapping.Scattering = Before.Scattering + static_cast<float>(ProfileEditDelta);
			break;
		case EIMRuralMutation::WoodSolidGeometry:
			Mapping.bSolidGeometry = false;
			break;
		case EIMRuralMutation::SoilProbeSupport:
			Mapping.bSupportsProbes = false;
			break;
		}
	}

	void RestoreMutationField(FIMAcousticPhysicalMaterialMapping& Mapping, const FIMRuralMutationCase& Case)
	{
		const FIMMappingSnapshot& Before = MappingBaseline[Case.MappingIndex];
		switch (Case.Mutation)
		{
		case EIMRuralMutation::AbsorptionLowBand:
			Mapping.Absorption.X = Before.Absorption.X;
			break;
		case EIMRuralMutation::Scattering:
			Mapping.Scattering = Before.Scattering;
			break;
		case EIMRuralMutation::WoodSolidGeometry:
			Mapping.bSolidGeometry = Before.bSolidGeometry;
			break;
		case EIMRuralMutation::SoilProbeSupport:
			Mapping.bSupportsProbes = Before.bSupportsProbes;
			break;
		}
	}

	bool ApplyCurrentMutation()
	{
		if (!IsValid(Volume) || !Cases.IsValidIndex(CaseIndex)
			|| !Volume->PhysicalMaterials.IsValidIndex(Cases[CaseIndex].MappingIndex))
		{
			return FailAndRestore(TEXT("The selected profile mapping disappeared before its single-variable edit."));
		}
		if (GEditor && GEditor->PlayWorld)
		{
			return FailAndRestore(TEXT("PIE started before the profile mutation."));
		}
		FIMRuralMutationCase& Case = Cases[CaseIndex];
		FIMAcousticPhysicalMaterialMapping& Mapping = Volume->PhysicalMaterials[Case.MappingIndex];
		ApplyMutationField(Mapping, Case);
		bMutationApplied = true;
		bOnlyRequestedVariableChanged = OnlySelectedMappingVariableChanged(Volume, MappingBaseline, Case);
		if (!bOnlyRequestedVariableChanged)
		{
			MarkFailure(TEXT("The mutation changed more than the single requested PhysicalMaterials field: ")
				+ Case.UniqueVariable);
		}
		Stage = EIMStage::ValidateMutation;
		return false;
	}

	bool ValidateCurrentMutation()
	{
		FIMRuralMutationCase& Case = Cases[CaseIndex];
		FString Failure;
		Case.bAfterValidatePassed = Volume->ValidateCurrentBake(Failure);
		Case.AfterValidateFailure = Failure;
		Case.bAfterFailureIsStaleFingerprint = !Case.bAfterValidatePassed
			&& Failure.Equals(StaleFailureText, ESearchCase::CaseSensitive);
		if (Case.bAfterValidatePassed || !Case.bAfterFailureIsStaleFingerprint)
		{
			MarkFailure(TEXT("The changed profile did not fail specifically on the baked scene fingerprint: ")
				+ Case.UniqueVariable + TEXT(" failure=") + Failure);
		}
		Stage = EIMStage::RestoreMutation;
		return false;
	}

	bool RestoreCurrentMutation()
	{
		if (!bMutationApplied)
		{
			Stage = EIMStage::PrepareStartupRestore;
			return false;
		}
		if (!IsValid(Volume) || !Cases.IsValidIndex(CaseIndex)
			|| !Volume->PhysicalMaterials.IsValidIndex(Cases[CaseIndex].MappingIndex))
		{
			MarkFailure(TEXT("Could not restore the active PhysicalMaterials mutation because its BakeVolume disappeared."));
			bMutationApplied = false;
			Stage = EIMStage::PrepareStartupRestore;
			return false;
		}
		FIMRuralMutationCase& Case = Cases[CaseIndex];
		RestoreMutationField(Volume->PhysicalMaterials[Case.MappingIndex], Case);
		bMutationApplied = false;
		if (!MappingSnapshotMatches(Volume, MappingBaseline))
		{
			MarkFailure(TEXT("The selected PhysicalMaterials field did not restore to its exact baseline: ")
				+ Case.UniqueVariable);
		}
		Stage = EIMStage::ValidateRestored;
		return false;
	}

	bool ValidateRestoredMutation()
	{
		if (!Cases.IsValidIndex(CaseIndex) || !IsValid(Volume))
		{
			MarkFailure(TEXT("The BakeVolume disappeared before restoration validation."));
			Stage = EIMStage::PrepareStartupRestore;
			return false;
		}
		FIMRuralMutationCase& Case = Cases[CaseIndex];
		FString Failure;
		Case.bRestoredValidatePassed = Volume->ValidateCurrentBake(Failure);
		Case.RestoredValidateFailure = Failure;
		Case.bRestoredFingerprintMatchesBaseline = Case.bRestoredValidatePassed
			&& Failure.IsEmpty() && Volume->BakedField
			&& Volume->BakedField->SceneFingerprint == InitialSceneFingerprint
			&& MappingSnapshotMatches(Volume, MappingBaseline);
		Case.bPassed = Case.bBaselineValidatePassed && !Case.bAfterValidatePassed
			&& Case.bAfterFailureIsStaleFingerprint && Case.bRestoredFingerprintMatchesBaseline
			&& bOnlyRequestedVariableChanged;
		if (!Case.bRestoredValidatePassed || !Case.bRestoredFingerprintMatchesBaseline)
		{
			MarkFailure(TEXT("ValidateCurrentBake did not return to the baseline after restoring ")
				+ Case.UniqueVariable + TEXT(": ") + Failure);
			Stage = EIMStage::PrepareStartupRestore;
			return false;
		}
		++CaseIndex;
		if (CaseIndex < Cases.Num())
		{
			// This exact restoration validation is the next case's immediate pre-edit baseline.
			FIMRuralMutationCase& NextCase = Cases[CaseIndex];
			NextCase.bBaselineValidatePassed = true;
			NextCase.BaselineValidateFailure.Reset();
			NextCase.BaselineFingerprint = InitialSceneFingerprint;
			Stage = EIMStage::ApplyMutation;
		}
		else
		{
			Stage = EIMStage::PrepareStartupRestore;
		}
		return false;
	}

	bool PrepareStartupRestore()
	{
		if (bMutationApplied)
		{
			Stage = EIMStage::RestoreMutation;
			return false;
		}
		if (Volume && !MappingSnapshotMatches(Volume, MappingBaseline))
		{
			MarkFailure(TEXT("The persistent PhysicalMaterials configuration is not fully restored."));
		}
		if (MapPackage && MapPackage->IsDirty() && MappingSnapshotMatches(Volume, MappingBaseline)
			&& BakePackage && !BakePackage->IsDirty())
		{
			// Baseline was clean, all owned config edits are restored, and this gate never saves the map.
			MapPackage->SetDirtyFlag(false);
		}
		if (BakePackage && BakePackage->IsDirty())
		{
			MarkFailure(TEXT("The Bake asset package became dirty; the test did not modify or save it."));
		}
		FString DirtyPath;
		if (CountDirtyPersistentPackages(MapPackage, DirtyPath))
		{
			MarkFailure(TEXT("A non-target persistent package became dirty during the gate: ") + DirtyPath);
		}
		if (GEditor && !GEditor->PlayWorld && CurrentWorldPackageName() == StartupMapPackage)
		{
			bStartupMapRestored = true;
			Stage = EIMStage::Finish;
			return false;
		}
		if (!GEditor || GEditor->PlayWorld)
		{
			MarkFailure(TEXT("Cannot restore the original startup map while the Editor is unavailable or in PIE."));
			Stage = EIMStage::Finish;
			return false;
		}
		if (CountDirtyPersistentPackages(nullptr, DirtyPath))
		{
			MarkFailure(TEXT("Refusing to switch back to the startup map while a persistent package is dirty: ") + DirtyPath);
			Stage = EIMStage::Finish;
			return false;
		}
		bStartupMapLoadReturned = FEditorFileUtils::LoadMap(StartupMapPackage, false, true);
		if (!bStartupMapLoadReturned)
		{
			MarkFailure(TEXT("Could not restore the original Editor startup map: ") + StartupMapPackage);
			Stage = EIMStage::Finish;
			return false;
		}
		StartupRestoreFrame = GFrameCounter;
		Stage = EIMStage::WaitStartupTick;
		return false;
	}

	bool VerifyStartupRestore()
	{
		if (GFrameCounter <= StartupRestoreFrame)
		{
			return false;
		}
		bStartupMapRestored = GEditor && !GEditor->PlayWorld
			&& CurrentWorldPackageName() == StartupMapPackage;
		if (!bStartupMapRestored)
		{
			MarkFailure(TEXT("The Editor did not return to the exact original startup map after the normal tick."));
		}
		FString DirtyPath;
		if (CountDirtyPersistentPackages(nullptr, DirtyPath))
		{
			MarkFailure(TEXT("A persistent package is dirty after startup map restoration: ") + DirtyPath);
		}
		Stage = EIMStage::Finish;
		return false;
	}

	bool Finish()
	{
		if (bFinished)
		{
			return true;
		}
		bFinished = true;
		FString HashError;
		if (!MapFilename.IsEmpty())
		{
			bMapHashAfterValid = HashFileSHA1(MapFilename, MapSha1After, MapBytesAfter, HashError);
			if (!bMapHashAfterValid)
			{
				MarkFailure(HashError);
			}
			else if (bMapHashBeforeValid && (MapSha1Before != MapSha1After || MapBytesBefore != MapBytesAfter))
			{
				MarkFailure(TEXT("The target map disk SHA1 or byte count changed; no map save is permitted."));
			}
		}
		if (!BakeFilename.IsEmpty())
		{
			HashError.Reset();
			bBakeHashAfterValid = HashFileSHA1(BakeFilename, BakeSha1After, BakeBytesAfter, HashError);
			if (!bBakeHashAfterValid)
			{
				MarkFailure(HashError);
			}
			else if (bBakeHashBeforeValid && (BakeSha1Before != BakeSha1After || BakeBytesBefore != BakeBytesAfter))
			{
				MarkFailure(TEXT("The Bake asset disk SHA1 or byte count changed; no bake or save is permitted."));
			}
		}
		if (GEditor && !GEditor->PlayWorld && CurrentWorldPackageName() == StartupMapPackage)
		{
			bStartupMapRestored = true;
		}
		FString DirtyPath;
		bNoDirtyPersistentPackages = !CountDirtyPersistentPackages(nullptr, DirtyPath);
		if (!bNoDirtyPersistentPackages)
		{
			MarkFailure(TEXT("Persistent package remains dirty after restoration: ") + DirtyPath);
		}

		bool bAllCasesPassed = Cases.Num() == 4;
		TArray<TSharedPtr<FJsonValue>> CaseRows;
		CaseRows.Reserve(Cases.Num());
		for (const FIMRuralMutationCase& Case : Cases)
		{
			bAllCasesPassed = bAllCasesPassed && Case.bPassed;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("case"), Case.Name);
			Row->SetStringField(TEXT("unique_variable"), Case.UniqueVariable);
			Row->SetStringField(TEXT("physical_material_path"), Case.PhysicalMaterialPath);
			Row->SetNumberField(TEXT("mapping_index"), Case.MappingIndex);
			Row->SetBoolField(TEXT("baseline_validate_passed"), Case.bBaselineValidatePassed);
			Row->SetStringField(TEXT("baseline_validate_failure"), Case.BaselineValidateFailure);
			Row->SetStringField(TEXT("baseline_fingerprint"), Case.BaselineFingerprint);
			Row->SetBoolField(TEXT("after_validate_passed"), Case.bAfterValidatePassed);
			Row->SetStringField(TEXT("after_failure"), Case.AfterValidateFailure);
			Row->SetBoolField(TEXT("after_failure_is_stale_fingerprint"), Case.bAfterFailureIsStaleFingerprint);
			Row->SetBoolField(TEXT("restored_validate_passed"), Case.bRestoredValidatePassed);
			Row->SetStringField(TEXT("restored_failure"), Case.RestoredValidateFailure);
			Row->SetBoolField(TEXT("restored_fingerprint_matches_baseline"), Case.bRestoredFingerprintMatchesBaseline);
			Row->SetBoolField(TEXT("passed"), Case.bPassed);
			CaseRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		const bool bDiskHashesStable = bMapHashBeforeValid && bMapHashAfterValid
			&& bBakeHashBeforeValid && bBakeHashAfterValid
			&& MapSha1Before == MapSha1After && MapBytesBefore == MapBytesAfter
			&& BakeSha1Before == BakeSha1After && BakeBytesBefore == BakeBytesAfter;
		const bool bPassed = bAllCasesPassed && bTargetLoadReturned && bStartupMapRestored
			&& bNoDirtyPersistentPackages && bDiskHashesStable && !bFailed;
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("schema"), TEXT("IMAcousticPhysicalMaterialInvalidation.v1"));
		Report->SetStringField(TEXT("status"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
		Report->SetStringField(TEXT("test"), TEXT("IceMoon.AcousticField.Rural.PersistentPhysicalMaterialEditsInvalidateBake"));
		Report->SetStringField(TEXT("target_map_package"), RuralTargetMap);
		Report->SetStringField(TEXT("startup_map_package"), StartupMapPackage);
		Report->SetBoolField(TEXT("target_map_load_returned"), bTargetLoadReturned);
		Report->SetBoolField(TEXT("startup_map_load_returned"), bStartupMapLoadReturned);
		Report->SetBoolField(TEXT("startup_map_restored"), bStartupMapRestored);
		Report->SetStringField(TEXT("bake_asset_path"), BakeAssetPath);
		Report->SetStringField(TEXT("baseline_scene_fingerprint"), InitialSceneFingerprint);
		Report->SetStringField(TEXT("map_file"), MapFilename);
		Report->SetStringField(TEXT("map_sha1_before"), MapSha1Before);
		Report->SetStringField(TEXT("map_sha1_after"), MapSha1After);
		Report->SetNumberField(TEXT("map_bytes_before"), MapBytesBefore);
		Report->SetNumberField(TEXT("map_bytes_after"), MapBytesAfter);
		Report->SetStringField(TEXT("bake_file"), BakeFilename);
		Report->SetStringField(TEXT("bake_sha1_before"), BakeSha1Before);
		Report->SetStringField(TEXT("bake_sha1_after"), BakeSha1After);
		Report->SetNumberField(TEXT("bake_bytes_before"), BakeBytesBefore);
		Report->SetNumberField(TEXT("bake_bytes_after"), BakeBytesAfter);
		Report->SetBoolField(TEXT("disk_hashes_stable"), bDiskHashesStable);
		Report->SetBoolField(TEXT("no_dirty_persistent_packages"), bNoDirtyPersistentPackages);
		Report->SetBoolField(TEXT("pending_bounds_wait_used"), !PendingBoundsComponent.IsEmpty());
		Report->SetStringField(TEXT("pending_bounds_component"), PendingBoundsComponent);
		Report->SetNumberField(TEXT("mutation_case_count"), Cases.Num());
		Report->SetArrayField(TEXT("cases"), CaseRows);
		if (!FailureReason.IsEmpty())
		{
			Report->SetStringField(TEXT("failure_reason"), FailureReason);
		}
		Report->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);

		FString ReportError;
		const bool bReportWritten = WriteReport(ReportPath, Report, ReportError);
		const bool bFinalPassed = bPassed && bReportWritten;
		if (bReportWritten)
		{
			Test->AddInfo(FString::Printf(TEXT("Rural PhysicalMaterial invalidation report: %s"), *ReportPath));
		}
		else
		{
			Test->AddError(ReportError);
		}
		if (!bFinalPassed)
		{
			Test->AddError(FailureReason.IsEmpty()
				? TEXT("Rural PersistentPhysicalMaterialEditsInvalidateBake did not meet every required assertion.")
				: FailureReason);
		}
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralPersistentPhysicalMaterialInvalidation %s%s"),
			bFinalPassed ? TEXT("PASS") : TEXT("FAIL"),
			bFinalPassed ? TEXT("") : *FString::Printf(TEXT(" reason=%s"),
				FailureReason.IsEmpty() ? TEXT("required assertions were not all satisfied") : *FailureReason));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s RuralPersistentPhysicalMaterialInvalidation"),
			bFinalPassed ? TEXT("PASS") : TEXT("FAIL"));
		return true;
	}

	FAutomationTestBase* Test = nullptr;
	FString ReportPath;
	FString StartupMapPackage;
	FString MapFilename;
	FString BakeFilename;
	FString BakeAssetPath;
	FString MapSha1Before;
	FString MapSha1After;
	FString BakeSha1Before;
	FString BakeSha1After;
	FString InitialSceneFingerprint;
	FString FailureReason;
	FString PendingBoundsComponent;
	int64 MapBytesBefore = 0;
	int64 MapBytesAfter = 0;
	int64 BakeBytesBefore = 0;
	int64 BakeBytesAfter = 0;
	uint64 TargetMapLoadFrame = 0;
	uint64 StartupRestoreFrame = 0;
	double StartedAt = 0.0;
	double TargetReadyWaitStartedAt = 0.0;
	UWorld* LoadedTargetWorld = nullptr;
	AIMAcousticBakeVolume* Volume = nullptr;
	UPackage* MapPackage = nullptr;
	UPackage* BakePackage = nullptr;
	TArray<FIMMappingSnapshot> MappingBaseline;
	TArray<FIMRuralMutationCase> Cases;
	int32 CaseIndex = 0;
	EIMStage Stage = EIMStage::Start;
	bool bFinished = false;
	bool bFailed = false;
	bool bMutationApplied = false;
	bool bOnlyRequestedVariableChanged = false;
	bool bTargetLoadReturned = false;
	bool bTargetMapWasLoaded = false;
	bool bStartupMapLoadReturned = false;
	bool bStartupMapRestored = false;
	bool bMapHashBeforeValid = false;
	bool bMapHashAfterValid = false;
	bool bBakeHashBeforeValid = false;
	bool bBakeHashAfterValid = false;
	bool bNoDirtyPersistentPackages = false;
};

static bool RunDynamicBlockerTest(FAutomationTestBase* Test)
{
	const FString ReportPath = MakeReportPath(TEXT("dynamic"));
	FIMDynamicBlockerCase ISM = RunDynamicBlockerCase(Test, false);
	FIMDynamicBlockerCase HISM = RunDynamicBlockerCase(Test, true);
	const bool bPassed = ISM.bPassed && HISM.bPassed;

	TArray<TSharedPtr<FJsonValue>> Cases;
	Cases.Add(MakeShared<FJsonValueObject>(DynamicCaseToJson(ISM)));
	Cases.Add(MakeShared<FJsonValueObject>(DynamicCaseToJson(HISM)));
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("IMAcousticDynamicInstancedBlockerRejected.v1"));
	Report->SetStringField(TEXT("status"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
	Report->SetStringField(TEXT("test"), TEXT("IceMoon.AcousticField.PhysicalMaterial.DynamicInstancedBlockerRejected"));
	Report->SetStringField(TEXT("scene_contract"), TEXT("Separate transient empty EditorPreview worlds; one valid PM and Engine Cube LOD0 CPU geometry per case."));
	Report->SetStringField(TEXT("negative_control"), TEXT("Only add one identical second instance after the successful one-instance capture."));
	Report->SetArrayField(TEXT("cases"), Cases);
	FString ReportError;
	const bool bReportWritten = WriteReport(ReportPath, Report, ReportError);
	const bool bFinalPassed = bPassed && bReportWritten;
	if (bReportWritten)
	{
		Test->AddInfo(FString::Printf(TEXT("Dynamic instanced blocker report: %s"), *ReportPath));
	}
	else
	{
		Test->AddError(ReportError);
	}
	if (!bFinalPassed)
	{
		Test->AddError(FString::Printf(TEXT("Dynamic instanced blocker fail-closed contract did not pass. ISM=%s HISM=%s"),
			*ISM.RejectedFailure, *HISM.RejectedFailure));
	}
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] DynamicInstancedBlockerRejected %s"),
		bFinalPassed ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s DynamicInstancedBlockerRejected"),
		bFinalPassed ? TEXT("PASS") : TEXT("FAIL"));
	return bFinalPassed;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticDynamicInstancedBlockerRejectedTest,
	"IceMoon.AcousticField.PhysicalMaterial.DynamicInstancedBlockerRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticDynamicInstancedBlockerRejectedTest::RunTest(const FString&)
{
	return IMAcousticPhysicalMaterialInvalidationTestPrivate::RunDynamicBlockerTest(this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralPersistentPhysicalMaterialInvalidationTest,
	"IceMoon.AcousticField.Rural.PersistentPhysicalMaterialEditsInvalidateBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralPersistentPhysicalMaterialInvalidationTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(
		IMAcousticPhysicalMaterialInvalidationTestPrivate::FIMRuralPhysicalMaterialInvalidationCommand(this));
	return true;
}

#endif
