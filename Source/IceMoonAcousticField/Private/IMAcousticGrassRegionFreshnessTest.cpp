#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Components/BoxComponent.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticGrassBakePlan.h"
#include "LandscapeComponent.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "LandscapeGrassType.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace IMAcousticGrassRegionFreshnessTestPrivate
{
constexpr const TCHAR* TestMap = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* OracleReceiptRelativePath = TEXT("Saved/AcousticV2/PhysicalMaterialDrive/pm_grass_generation_r22_repair1_receipt_20261001.json");
constexpr const TCHAR* OracleReceiptSha256 = TEXT("ddde01607f754aa23532a52aa771fc8ea87ead309604b570dc949d1c5ae632b4");
constexpr const TCHAR* OracleReceiptSha1 = TEXT("5D673D117D2EBF6A8D9BE13F185575A46AEA2FDA");
constexpr int32 OracleReceiptBytes = 4820;
constexpr const TCHAR* ExpectedFullFingerprint = TEXT("955D1C0A2195BAE4DEB1EEADD3ACC4F1FEDAE103");
constexpr int64 ExpectedSolidTriangles = 420432;
constexpr int64 ExpectedNonSolidTriangles = 174484;

struct FIMExpectedPhysicalMaterialCount
{
	const TCHAR* Path;
	int64 Triangles;
};

constexpr FIMExpectedPhysicalMaterialCount ExpectedPhysicalMaterials[] =
{
	{TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_Wood.PM_Acoustic_Wood"), 276413},
	{TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_Stone.PM_Acoustic_Stone"), 143777},
	{TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_Soil.PM_Acoustic_Soil"), 242},
	{TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_Leaf.PM_Acoustic_Leaf"), 0},
	{TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_Grass.PM_Acoustic_Grass"), 0},
	{TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_HardProp.PM_Acoustic_HardProp"), 0},
	{TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/PM_Acoustic_HardGround.PM_Acoustic_HardGround"), 0},
};

struct FIMPhaseStamp
{
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
	uint64 StartFrame = 0;
	uint64 EndFrame = 0;
};

struct FIMPlanObservation
{
	bool bRan = false;
	bool bSucceeded = false;
	FString Error;
	FIMAcousticGrassBakePlan Plan;
	FIMPhaseStamp Stamp;
};

struct FIMNativeObservation
{
	bool bRan = false;
	bool bCaptured = false;
	FString Error;
	FString Fingerprint;
	int32 VertexCount = 0;
	int32 TriangleCount = 0;
	int32 MaterialIndexCount = 0;
	int32 InvalidMaterialIndexCount = 0;
	int64 SolidTrianglesByProfile = 0;
	int64 NonSolidTrianglesByProfile = 0;
	int32 NonSolidTriangles = 0;
	int32 NonSolidSections = 0;
	TMap<FString, int64> PhysicalMaterialHistogram;
	FIMPhaseStamp Stamp;
};

struct FIMOracleObservation
{
	FString Path;
		FString ExpectedSha256 = OracleReceiptSha256;
		FString ActualSha1;
		int32 ActualBytes = -1;
	FString Fingerprint;
	int64 NonSolidTriangles = -1;
	TMap<FString, int64> PhysicalMaterialHistogram;
	FString Error;
		bool bRuntimeBytesMatch = false;
	bool bOracleContentsMatch = false;
};

struct FIMScaleBoundsObservation
{
	FString MeshPath;
	FString Error;
	double MeshUnscaledRadius = 0.0;
	double FreeRadius = 0.0;
	double LockXYRadius = 0.0;
	double AllFixedNegativeRadius = 0.0;
	FString ExtremeIntervalError;
	FIMPhaseStamp Stamp;
	bool bRan = false;
	bool bPassed = false;
	bool bExtremeIntervalRejected = false;
};

struct FIMPhysicalMaterialSnapshot
{
	UPhysicalMaterial* Asset = nullptr;
	FVector Absorption = FVector::ZeroVector;
	float Scattering = 0.0f;
	bool bSolidGeometry = false;
	bool bSupportsProbes = false;
};

struct FIMWorldTransformSnapshot
{
	ULandscapeComponent* Component = nullptr;
	FString ComponentPath;
	FTransform Transform = FTransform::Identity;
};

static FString PointerToString(const void* Pointer)
{
	return Pointer ? FString::Printf(TEXT("%p"), Pointer) : TEXT("null");
}

static FString HashBufferSha1(const void* Data, const uint64 Size)
{
	uint8 Digest[FSHA1::DigestSize] = {};
	if (Data && Size > 0)
	{
		FSHA1::HashBuffer(Data, Size, Digest);
	}
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

// Hash the serialized grass-map payload and its current-format element and
// offset metadata. NumElements < 0 is UE's ungenerated/unvalidated state;
// zero is a valid, explicitly empty grass map.
static FString GrassDataFingerprint(const FLandscapeComponentGrassData& Data)
{
	FString Canonical;
	Canonical += FString::Printf(TEXT("GenerationHash=%u|NumElements=%d|"), Data.GenerationHash, Data.NumElements);
	Canonical += FString::Printf(TEXT("HeightWeightData=%d:%s|"), Data.HeightWeightData.Num(),
		*HashBufferSha1(Data.HeightWeightData.GetData(), uint64(Data.HeightWeightData.Num())));

	TArray<FName> WeightNames;
	Data.WeightOffsets.GetKeys(WeightNames);
	WeightNames.Sort([](const FName& A, const FName& B)
	{
		return A.ToString() < B.ToString();
	});
	for (const FName& Name : WeightNames)
	{
		Canonical += FString::Printf(TEXT("WeightOffset=%s:%d|"), *Name.ToString(), Data.WeightOffsets.FindChecked(Name));
	}

	TArray<int32> HeightMipLods;
	Data.HeightMipData.GenerateKeyArray(HeightMipLods);
	HeightMipLods.Sort();
	for (const int32 Lod : HeightMipLods)
	{
		const TArray<uint16>& HeightMip = Data.HeightMipData.FindChecked(Lod);
		Canonical += FString::Printf(TEXT("HeightMip=%d:%d:%s|"), Lod, HeightMip.Num(),
			*HashBufferSha1(HeightMip.GetData(), uint64(HeightMip.Num()) * sizeof(uint16)));
	}

	return HashBufferSha1(*Canonical, uint64(Canonical.Len()) * sizeof(TCHAR));
}

static TArray<FString> DirtyPackageNames()
{
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyPackages(DirtyPackages);
	TArray<FString> Names;
	for (UPackage* Package : DirtyPackages)
	{
		if (IsValid(Package))
		{
			Names.Add(Package->GetName());
		}
	}
	Names.Sort();
	return Names;
}

static void BeginPhase(FIMPhaseStamp& Stamp)
{
	Stamp.StartSeconds = FPlatformTime::Seconds();
	Stamp.StartFrame = GFrameCounter;
}

static void EndPhase(FIMPhaseStamp& Stamp)
{
	Stamp.EndFrame = GFrameCounter;
	Stamp.EndSeconds = FPlatformTime::Seconds();
}

static void RunPlan(UWorld* World, const FBox& Region, FIMPlanObservation& Out)
{
	Out.bRan = true;
	BeginPhase(Out.Stamp);
	Out.bSucceeded = IMBuildAcousticGrassBakePlan(World, Region, Out.Plan, Out.Error);
	EndPhase(Out.Stamp);
}

static void RunNativeCapture(AIMAcousticBakeVolume* Volume, FIMNativeObservation& Out)
{
	Out.bRan = true;
	TArray<FVector> Vertices;
	TArray<FIntVector> Triangles;
	TArray<int32> MaterialIndices;
	BeginPhase(Out.Stamp);
	Out.bCaptured = IsValid(Volume)
		&& Volume->IM_CaptureSceneGeometryForTest(Vertices, Triangles, MaterialIndices, Out.Fingerprint);
	EndPhase(Out.Stamp);

	if (!IsValid(Volume))
	{
		Out.Error = TEXT("BakeVolume became invalid before Native capture.");
		return;
	}

	Out.VertexCount = Vertices.Num();
	Out.TriangleCount = Triangles.Num();
	Out.MaterialIndexCount = MaterialIndices.Num();
	Out.NonSolidTriangles = Volume->NonSolidTriangles;
	Out.NonSolidSections = Volume->NonSolidSections;
	if (!Out.bCaptured)
	{
		Out.Error = Volume->Status;
		if (!Volume->SceneIssues.IsEmpty())
		{
			if (!Out.Error.IsEmpty())
			{
				Out.Error += TEXT(" | ");
			}
			Out.Error += Volume->SceneIssues[0];
		}
		if (Out.Error.IsEmpty())
		{
			Out.Error = TEXT("IM_CaptureSceneGeometryForTest returned false without a Native error string.");
		}
		return;
	}

	if (MaterialIndices.Num() != Triangles.Num())
	{
		Out.Error = FString::Printf(TEXT("Native material-index count %d differs from triangle count %d."),
			MaterialIndices.Num(), Triangles.Num());
	}

	const int32 Count = FMath::Min(MaterialIndices.Num(), Triangles.Num());
	for (int32 TriangleIndex = 0; TriangleIndex < Count; ++TriangleIndex)
	{
		const int32 MaterialIndex = MaterialIndices[TriangleIndex];
		if (!Volume->PhysicalMaterials.IsValidIndex(MaterialIndex))
		{
			++Out.InvalidMaterialIndexCount;
			continue;
		}

		const FIMAcousticPhysicalMaterialMapping& Mapping = Volume->PhysicalMaterials[MaterialIndex];
		UPhysicalMaterial* PhysicalMaterial = Mapping.PhysicalMaterial.Get();
		if (!IsValid(PhysicalMaterial))
		{
			++Out.InvalidMaterialIndexCount;
			continue;
		}

		const FString PhysicalMaterialPath = PhysicalMaterial->GetPathName();
		Out.PhysicalMaterialHistogram.FindOrAdd(PhysicalMaterialPath)++;
		if (Mapping.bSolidGeometry)
		{
			++Out.SolidTrianglesByProfile;
		}
		else
		{
			++Out.NonSolidTrianglesByProfile;
		}
	}
}

static bool NativeMatchesExpected(const FIMNativeObservation& Native, FString& OutError)
{
	if (!Native.bCaptured)
	{
		OutError = Native.Error.IsEmpty() ? TEXT("Native capture did not complete.") : Native.Error;
		return false;
	}
	if (Native.Fingerprint != ExpectedFullFingerprint)
	{
		OutError = FString::Printf(TEXT("Full fingerprint %s differs from frozen R22 %s."),
			*Native.Fingerprint, ExpectedFullFingerprint);
		return false;
	}
	if (Native.TriangleCount != ExpectedSolidTriangles || Native.MaterialIndexCount != Native.TriangleCount
		|| Native.SolidTrianglesByProfile != ExpectedSolidTriangles
		|| Native.NonSolidTrianglesByProfile != 0
		|| Native.NonSolidTriangles != ExpectedNonSolidTriangles)
	{
		OutError = FString::Printf(TEXT("Native solid/material/non-solid counts differ: triangles=%d material_indices=%d solid_by_profile=%lld non_solid_output=%lld non_solid_audit=%d; expected solid=%lld non_solid_audit=%lld."),
			Native.TriangleCount, Native.MaterialIndexCount,
			static_cast<long long>(Native.SolidTrianglesByProfile),
			static_cast<long long>(Native.NonSolidTrianglesByProfile), Native.NonSolidTriangles,
			static_cast<long long>(ExpectedSolidTriangles), static_cast<long long>(ExpectedNonSolidTriangles));
		return false;
	}
	if (Native.InvalidMaterialIndexCount != 0)
	{
		OutError = FString::Printf(TEXT("Native output contains %d invalid physical-material indices."), Native.InvalidMaterialIndexCount);
		return false;
	}

	for (const FIMExpectedPhysicalMaterialCount& Expected : ExpectedPhysicalMaterials)
	{
		const int64 Actual = Native.PhysicalMaterialHistogram.FindRef(Expected.Path);
		if (Actual != Expected.Triangles)
		{
			OutError = FString::Printf(TEXT("PhysicalMaterial path %s has %lld triangles; expected %lld."),
				Expected.Path, static_cast<long long>(Actual), static_cast<long long>(Expected.Triangles));
			return false;
		}
	}
	for (const TPair<FString, int64>& Pair : Native.PhysicalMaterialHistogram)
	{
		bool bKnownPath = false;
		for (const FIMExpectedPhysicalMaterialCount& Expected : ExpectedPhysicalMaterials)
		{
			bKnownPath |= Pair.Key == Expected.Path;
		}
		if (!bKnownPath)
		{
			OutError = TEXT("Native output contains an unexpected PhysicalMaterial path: ") + Pair.Key;
			return false;
		}
	}

	return true;
}

static bool NativeMatchesBaseline(const FIMNativeObservation& Candidate, const FIMNativeObservation& Baseline)
{
	if (!(Candidate.bCaptured && Baseline.bCaptured
		&& Candidate.Fingerprint == Baseline.Fingerprint
		&& Candidate.VertexCount == Baseline.VertexCount
		&& Candidate.TriangleCount == Baseline.TriangleCount
		&& Candidate.MaterialIndexCount == Baseline.MaterialIndexCount
		&& Candidate.InvalidMaterialIndexCount == Baseline.InvalidMaterialIndexCount
		&& Candidate.SolidTrianglesByProfile == Baseline.SolidTrianglesByProfile
		&& Candidate.NonSolidTrianglesByProfile == Baseline.NonSolidTrianglesByProfile
		&& Candidate.NonSolidTriangles == Baseline.NonSolidTriangles
		&& Candidate.PhysicalMaterialHistogram.Num() == Baseline.PhysicalMaterialHistogram.Num()))
	{
		return false;
	}
	for (const TPair<FString, int64>& Pair : Candidate.PhysicalMaterialHistogram)
	{
		const int64* BaselineCount = Baseline.PhysicalMaterialHistogram.Find(Pair.Key);
		if (!BaselineCount || *BaselineCount != Pair.Value)
		{
			return false;
		}
	}
	return true;
}

static FIMScaleBoundsObservation RunScaleBoundsBoundaryControl()
{
	FIMScaleBoundsObservation Result;
	Result.bRan = true;
	BeginPhase(Result.Stamp);
	UStaticMesh* TransientMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!IsValid(TransientMesh))
	{
		Result.Error = TEXT("Could not create a transient StaticMesh for scale-boundary control.");
		EndPhase(Result.Stamp);
		return Result;
	}

	// A non-degenerate local box with a known one-unit Y boundary lets the test
	// call the production reader and assert the actual four-unit reach directly.
	const FBox RequestedBounds(FVector(-0.25, -1.0, -0.25), FVector(0.25, 1.0, 0.25));
	TransientMesh->SetExtendedBounds(FBoxSphereBounds(RequestedBounds));
	const FBox MeshBounds = TransientMesh->GetBounds().GetBox();
	if (!MeshBounds.IsValid || MeshBounds.Min.ContainsNaN() || MeshBounds.Max.ContainsNaN())
	{
		Result.Error = TEXT("Transient StaticMesh did not retain a valid ExtendedBounds box.");
		EndPhase(Result.Stamp);
		return Result;
	}
	Result.MeshPath = TransientMesh->GetPathName();
	const FVector MaxCoordinate = MeshBounds.Min.GetAbs().ComponentMax(MeshBounds.Max.GetAbs());
	Result.MeshUnscaledRadius = MaxCoordinate.Size();

	auto RunReader = [&Result, TransientMesh](const TCHAR* CaseName, FGrassVariety& Variety, double& OutRadius)
	{
		Variety.GrassMesh = TransientMesh;
		FString Error;
		if (!IMAcousticGrassBakePlan::ReadVarietyBounds(Variety, OutRadius, Error))
		{
			Result.Error = FString::Printf(TEXT("ReadVarietyBounds failed for %s: %s"), CaseName, *Error);
			return false;
		}
		return true;
	};

	FGrassVariety Free;
	Free.Scaling = EGrassScaling::Free;
	Free.ScaleX.Min = 1.0f;
	Free.ScaleX.Max = 2.0f;
	Free.ScaleY.Min = -4.0f;
	Free.ScaleY.Max = -4.0f;
	Free.ScaleZ.Min = 1.0f;
	Free.ScaleZ.Max = 1.0f;
	if (!RunReader(TEXT("Free X varies while Y is fixed at -4"), Free, Result.FreeRadius))
	{
		EndPhase(Result.Stamp);
		return Result;
	}

	FGrassVariety LockXY;
	LockXY.Scaling = EGrassScaling::LockXY;
	LockXY.ScaleX.Min = -4.0f;
	LockXY.ScaleX.Max = -4.0f;
	LockXY.ScaleZ.Min = 1.0f;
	LockXY.ScaleZ.Max = 2.0f;
	if (!RunReader(TEXT("LockXY X is fixed at -4 while Z varies"), LockXY, Result.LockXYRadius))
	{
		EndPhase(Result.Stamp);
		return Result;
	}

	FGrassVariety FixedNegative;
	FixedNegative.Scaling = EGrassScaling::Free;
	FixedNegative.ScaleX.Min = -2.0f;
	FixedNegative.ScaleX.Max = -2.0f;
	FixedNegative.ScaleY.Min = -4.0f;
	FixedNegative.ScaleY.Max = -4.0f;
	FixedNegative.ScaleZ.Min = -1.0f;
	FixedNegative.ScaleZ.Max = -1.0f;
	if (!RunReader(TEXT("all fixed negative axes"), FixedNegative, Result.AllFixedNegativeRadius))
	{
		EndPhase(Result.Stamp);
		return Result;
	}

	FGrassVariety OverflowInterval;
	OverflowInterval.Scaling = EGrassScaling::Free;
	OverflowInterval.ScaleX.Min = -TNumericLimits<float>::Max();
	OverflowInterval.ScaleX.Max = TNumericLimits<float>::Max();
	OverflowInterval.ScaleY.Min = 1.0f;
	OverflowInterval.ScaleY.Max = 1.0f;
	OverflowInterval.ScaleZ.Min = 1.0f;
	OverflowInterval.ScaleZ.Max = 1.0f;
	OverflowInterval.GrassMesh = TransientMesh;
	double OverflowRadius = 0.0;
	const bool bOverflowAccepted = IMAcousticGrassBakePlan::ReadVarietyBounds(
		OverflowInterval, OverflowRadius, Result.ExtremeIntervalError);
	Result.bExtremeIntervalRejected = !bOverflowAccepted && !Result.ExtremeIntervalError.IsEmpty();

	const bool bFreeCoversNegativeFixedY = Result.FreeRadius >= 4.0;
	const bool bLockXYCoversNegativeFixedX = Result.LockXYRadius >= 4.0;
	const bool bFixedNegativeUsesUEDefaultOne = FMath::IsNearlyEqual(
		Result.AllFixedNegativeRadius, Result.MeshUnscaledRadius, 1.0e-6);
	Result.bPassed = bFreeCoversNegativeFixedY && bLockXYCoversNegativeFixedX
		&& bFixedNegativeUsesUEDefaultOne && Result.bExtremeIntervalRejected;
	if (!Result.bPassed)
	{
		Result.Error = FString::Printf(TEXT("Scale-boundary results do not cover UE behavior: unscaled=%.9f free=%.9f lock_xy=%.9f fixed_negative=%.9f overflow_rejected=%d; expected Free and LockXY >= 4, all-fixed-negative == unscaled/default-1, and opposite FLT_MAX interval rejected. %s"),
			Result.MeshUnscaledRadius, Result.FreeRadius, Result.LockXYRadius, Result.AllFixedNegativeRadius,
			Result.bExtremeIntervalRejected, *Result.ExtremeIntervalError);
	}
	EndPhase(Result.Stamp);
	return Result;
}

static bool PlansMatch(const FIMAcousticGrassBakePlan& Candidate, const FIMAcousticGrassBakePlan& Baseline, FString& OutError)
{
	if (Candidate.SourceInputSha1 != Baseline.SourceInputSha1
		|| Candidate.Sources.Num() != Baseline.Sources.Num()
		|| Candidate.Cameras.Num() != Baseline.Cameras.Num()
		|| Candidate.XYDisjointComponents.Num() != Baseline.XYDisjointComponents.Num())
	{
		OutError = TEXT("Plan input SHA, selected source/camera count, or XY-disjoint count changed.");
		return false;
	}

	TMap<FString, FString> BaselineSources;
	for (const FIMAcousticGrassBakeSource& Source : Baseline.Sources)
	{
		if (!IsValid(Source.Component) || BaselineSources.Contains(Source.Component->GetPathName()))
		{
			OutError = TEXT("Baseline plan contains an invalid or duplicate component identity.");
			return false;
		}
		BaselineSources.Add(Source.Component->GetPathName(), Source.SourceInputSha1);
	}
	for (const FIMAcousticGrassBakeSource& Source : Candidate.Sources)
	{
		if (!IsValid(Source.Component))
		{
			OutError = TEXT("Candidate plan contains an invalid source component.");
			return false;
		}
		const FString Path = Source.Component->GetPathName();
		const FString* BaselineSha = BaselineSources.Find(Path);
		if (!BaselineSha || *BaselineSha != Source.SourceInputSha1)
		{
			OutError = TEXT("Selected source identity or SourceInputSha1 changed: ") + Path;
			return false;
		}
	}

	TSet<FString> BaselineDisjoint;
	for (ULandscapeComponent* Component : Baseline.XYDisjointComponents)
	{
		if (!IsValid(Component) || BaselineDisjoint.Contains(Component->GetPathName()))
		{
			OutError = TEXT("Baseline XY-disjoint list contains an invalid or duplicate component.");
			return false;
		}
		BaselineDisjoint.Add(Component->GetPathName());
	}
	for (ULandscapeComponent* Component : Candidate.XYDisjointComponents)
	{
		if (!IsValid(Component) || !BaselineDisjoint.Contains(Component->GetPathName()))
		{
			OutError = TEXT("XY-disjoint component set changed after far GrassData substitution.");
			return false;
		}
	}

	for (int32 Index = 0; Index < Candidate.Cameras.Num(); ++Index)
	{
		if (!Candidate.Cameras[Index].Equals(Baseline.Cameras[Index], 0.0))
		{
			OutError = FString::Printf(TEXT("Camera %d changed after far GrassData substitution."), Index);
			return false;
		}
	}
	return true;
}

static void CapturePhysicalMaterialProfile(AIMAcousticBakeVolume* Volume, TArray<FIMPhysicalMaterialSnapshot>& OutProfile)
{
	OutProfile.Reset();
	if (!IsValid(Volume))
	{
		return;
	}
	OutProfile.Reserve(Volume->PhysicalMaterials.Num());
	for (const FIMAcousticPhysicalMaterialMapping& Mapping : Volume->PhysicalMaterials)
	{
		FIMPhysicalMaterialSnapshot& Snapshot = OutProfile.AddDefaulted_GetRef();
		Snapshot.Asset = Mapping.PhysicalMaterial.Get();
		Snapshot.Absorption = Mapping.Absorption;
		Snapshot.Scattering = Mapping.Scattering;
		Snapshot.bSolidGeometry = Mapping.bSolidGeometry;
		Snapshot.bSupportsProbes = Mapping.bSupportsProbes;
	}
}

static bool PhysicalMaterialProfileMatches(AIMAcousticBakeVolume* Volume,
	const EIMAcousticMaterialDrive BaselineDrive, const TArray<FIMPhysicalMaterialSnapshot>& BaselineProfile,
	const float BaselineProbeSpacing, const float BaselineProbeHeight)
{
	if (!IsValid(Volume) || Volume->MaterialDrive != BaselineDrive
		|| Volume->ProbeSpacingCm != BaselineProbeSpacing || Volume->ProbeHeightCm != BaselineProbeHeight
		|| Volume->PhysicalMaterials.Num() != BaselineProfile.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < BaselineProfile.Num(); ++Index)
	{
		const FIMAcousticPhysicalMaterialMapping& Current = Volume->PhysicalMaterials[Index];
		const FIMPhysicalMaterialSnapshot& Baseline = BaselineProfile[Index];
		if (Current.PhysicalMaterial.Get() != Baseline.Asset || Current.Absorption != Baseline.Absorption
			|| Current.Scattering != Baseline.Scattering || Current.bSolidGeometry != Baseline.bSolidGeometry
			|| Current.bSupportsProbes != Baseline.bSupportsProbes)
		{
			return false;
		}
	}
	return true;
}

static void CaptureTransformSnapshots(const FIMAcousticGrassBakePlan& Plan, TArray<FIMWorldTransformSnapshot>& OutSnapshots)
{
	OutSnapshots.Reset();
	TSet<ULandscapeComponent*> Seen;
	auto AddComponent = [&OutSnapshots, &Seen](ULandscapeComponent* Component)
	{
		if (!IsValid(Component) || Seen.Contains(Component))
		{
			return;
		}
		Seen.Add(Component);
		FIMWorldTransformSnapshot& Snapshot = OutSnapshots.AddDefaulted_GetRef();
		Snapshot.Component = Component;
		Snapshot.ComponentPath = Component->GetPathName();
		Snapshot.Transform = Component->GetComponentTransform();
	};
	for (const FIMAcousticGrassBakeSource& Source : Plan.Sources)
	{
		AddComponent(Source.Component);
	}
	for (ULandscapeComponent* Component : Plan.XYDisjointComponents)
	{
		AddComponent(Component);
	}
}

static bool TransformSnapshotsMatch(const TArray<FIMWorldTransformSnapshot>& Snapshots)
{
	for (const FIMWorldTransformSnapshot& Snapshot : Snapshots)
	{
		if (!IsValid(Snapshot.Component) || Snapshot.Component->GetPathName() != Snapshot.ComponentPath
			|| !Snapshot.Component->GetComponentTransform().Equals(Snapshot.Transform, 0.0))
		{
			return false;
		}
	}
	return true;
}

class FIMScopedGrassDataFault
{
public:
	explicit FIMScopedGrassDataFault(ULandscapeComponent* InComponent)
		: Component(InComponent)
		, OriginalGrassData(InComponent->GrassData)
		, OriginalDataHash(GrassDataFingerprint(OriginalGrassData.Get()))
		, bOriginalWasValid(OriginalGrassData->NumElements >= 0)
	{
	}

	~FIMScopedGrassDataFault()
	{
		Restore();
	}

	bool SubstituteInvalid()
	{
		if (!IsValid(Component.Get()) || !IsInGameThread() || bActive)
		{
			return false;
		}
		Component->GrassData = MakeShared<FLandscapeComponentGrassData, ESPMode::ThreadSafe>();
		bActive = true;
		return true;
	}

	void Restore()
	{
		if (bActive && IsValid(Component.Get()))
		{
			Component->GrassData = OriginalGrassData;
			bActive = false;
		}
	}

	bool IsRestoredExactly() const
	{
		return IsValid(Component.Get()) && !bActive && &Component->GrassData.Get() == &OriginalGrassData.Get()
			&& GrassDataFingerprint(Component->GrassData.Get()) == OriginalDataHash;
	}

	const TSharedRef<FLandscapeComponentGrassData, ESPMode::ThreadSafe>& GetOriginalGrassData() const { return OriginalGrassData; }
	const FString& GetOriginalDataHash() const { return OriginalDataHash; }
	bool WasOriginalValid() const { return bOriginalWasValid; }

private:
	TStrongObjectPtr<ULandscapeComponent> Component;
	TSharedRef<FLandscapeComponentGrassData, ESPMode::ThreadSafe> OriginalGrassData;
	FString OriginalDataHash;
	bool bOriginalWasValid = false;
	bool bActive = false;
};

static TSharedRef<FJsonObject> VectorToJson(const FVector& Vector)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("x"), Vector.X);
	Json->SetNumberField(TEXT("y"), Vector.Y);
	Json->SetNumberField(TEXT("z"), Vector.Z);
	return Json;
}

static TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("translation_cm"), VectorToJson(Transform.GetTranslation()));
	Json->SetObjectField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
	const FQuat Rotation = Transform.GetRotation();
	TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
	RotationJson->SetNumberField(TEXT("x"), Rotation.X);
	RotationJson->SetNumberField(TEXT("y"), Rotation.Y);
	RotationJson->SetNumberField(TEXT("z"), Rotation.Z);
	RotationJson->SetNumberField(TEXT("w"), Rotation.W);
	Json->SetObjectField(TEXT("rotation_xyzw"), RotationJson);
	return Json;
}

static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

static TSharedRef<FJsonObject> PhaseStampToJson(const FString& Name, const FIMPhaseStamp& Stamp)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetNumberField(TEXT("start_platform_seconds"), Stamp.StartSeconds);
	Json->SetNumberField(TEXT("end_platform_seconds"), Stamp.EndSeconds);
	Json->SetNumberField(TEXT("elapsed_ms"), Stamp.EndSeconds >= Stamp.StartSeconds
		? (Stamp.EndSeconds - Stamp.StartSeconds) * 1000.0 : 0.0);
	Json->SetNumberField(TEXT("start_gt_frame"), double(Stamp.StartFrame));
	Json->SetNumberField(TEXT("end_gt_frame"), double(Stamp.EndFrame));
	return Json;
}

static TSharedRef<FJsonObject> PlanToJson(const FIMPlanObservation& Observation)
{
	TSharedRef<FJsonObject> Json = PhaseStampToJson(TEXT("pure_plan"), Observation.Stamp);
	Json->SetBoolField(TEXT("ran"), Observation.bRan);
	Json->SetBoolField(TEXT("succeeded"), Observation.bSucceeded);
	Json->SetStringField(TEXT("error"), Observation.Error);
	if (!Observation.bRan)
	{
		return Json;
	}

	const FIMAcousticGrassBakePlan& Plan = Observation.Plan;
	Json->SetStringField(TEXT("source_input_sha1"), Plan.SourceInputSha1);
	Json->SetNumberField(TEXT("selected_source_count"), Plan.Sources.Num());
	Json->SetNumberField(TEXT("camera_count"), Plan.Cameras.Num());
	Json->SetNumberField(TEXT("xy_disjoint_count"), Plan.XYDisjointComponents.Num());

	TArray<TSharedPtr<FJsonValue>> SourceRows;
	for (const FIMAcousticGrassBakeSource& Source : Plan.Sources)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("component"), IsValid(Source.Component) ? Source.Component->GetPathName() : TEXT("invalid"));
		Row->SetStringField(TEXT("source_input_sha1"), Source.SourceInputSha1);
		if (IsValid(Source.Component))
		{
			Row->SetObjectField(TEXT("component_transform"), TransformToJson(Source.Component->GetComponentTransform()));
			Row->SetStringField(TEXT("grassdata_pointer"), PointerToString(&Source.Component->GrassData.Get()));
			Row->SetNumberField(TEXT("grassdata_num_elements"), Source.Component->GrassData->NumElements);
			Row->SetBoolField(TEXT("grassdata_valid_by_num_elements"), Source.Component->GrassData->NumElements >= 0);
		}
		SourceRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Json->SetArrayField(TEXT("sources"), SourceRows);

	TArray<TSharedPtr<FJsonValue>> CameraRows;
	for (const FVector& Camera : Plan.Cameras)
	{
		CameraRows.Add(MakeShared<FJsonValueObject>(VectorToJson(Camera)));
	}
	Json->SetArrayField(TEXT("cameras"), CameraRows);

	TArray<FString> DisjointPaths;
	for (ULandscapeComponent* Component : Plan.XYDisjointComponents)
	{
		DisjointPaths.Add(IsValid(Component) ? Component->GetPathName() : TEXT("invalid"));
	}
	DisjointPaths.Sort();
	Json->SetArrayField(TEXT("xy_disjoint_components"), StringsToJson(DisjointPaths));
	return Json;
}

static TSharedRef<FJsonObject> NativeToJson(const FString& Name, const FIMNativeObservation& Observation)
{
	TSharedRef<FJsonObject> Json = PhaseStampToJson(Name, Observation.Stamp);
	Json->SetBoolField(TEXT("ran"), Observation.bRan);
	Json->SetBoolField(TEXT("captured"), Observation.bCaptured);
	Json->SetStringField(TEXT("error"), Observation.Error);
	Json->SetStringField(TEXT("scene_fingerprint"), Observation.Fingerprint);
	Json->SetNumberField(TEXT("vertex_count"), Observation.VertexCount);
	Json->SetNumberField(TEXT("solid_triangle_count"), Observation.TriangleCount);
	Json->SetNumberField(TEXT("material_index_count"), Observation.MaterialIndexCount);
	Json->SetNumberField(TEXT("invalid_material_index_count"), Observation.InvalidMaterialIndexCount);
	Json->SetNumberField(TEXT("solid_triangles_by_profile"), double(Observation.SolidTrianglesByProfile));
	Json->SetNumberField(TEXT("non_solid_triangles_by_profile_in_output"), double(Observation.NonSolidTrianglesByProfile));
	Json->SetNumberField(TEXT("non_solid_triangle_audit_count"), Observation.NonSolidTriangles);
	Json->SetNumberField(TEXT("non_solid_section_audit_count"), Observation.NonSolidSections);

	TArray<FString> Paths;
	for (const FIMExpectedPhysicalMaterialCount& Expected : ExpectedPhysicalMaterials)
	{
		Paths.Add(Expected.Path);
	}
	TArray<FString> ObservedPaths;
	Observation.PhysicalMaterialHistogram.GetKeys(ObservedPaths);
	Paths.Append(ObservedPaths);
	Paths.Sort();
	for (int32 Index = Paths.Num() - 1; Index > 0; --Index)
	{
		if (Paths[Index] == Paths[Index - 1])
		{
			Paths.RemoveAt(Index);
		}
	}
	TArray<TSharedPtr<FJsonValue>> HistogramRows;
	for (const FString& Path : Paths)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("physical_material_path"), Path);
		Row->SetNumberField(TEXT("solid_triangles"), double(Observation.PhysicalMaterialHistogram.FindRef(Path)));
		HistogramRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Json->SetArrayField(TEXT("physical_material_histogram_by_path"), HistogramRows);
	return Json;
}

static bool ReadOracleReceipt(FIMOracleObservation& Out)
{
	Out.Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), OracleReceiptRelativePath));
	TArray<uint8> ReceiptBytes;
	if (!FFileHelper::LoadFileToArray(ReceiptBytes, *Out.Path))
	{
		Out.Error = TEXT("Frozen R22 oracle receipt could not be read: ") + Out.Path;
		return false;
	}
	// This Windows UE build inherits the asserting generic SHA-256 platform stub.
	// FSHA1 is implemented in Core; the root additionally binds SHA-256 before launch.
	uint8 Digest[20];
	FSHA1::HashBuffer(ReceiptBytes.GetData(), ReceiptBytes.Num(), Digest);
	Out.ActualSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	Out.ActualBytes = ReceiptBytes.Num();
	Out.bRuntimeBytesMatch = Out.ActualSha1 == OracleReceiptSha1 && Out.ActualBytes == OracleReceiptBytes;
	if (!Out.bRuntimeBytesMatch)
	{
		Out.Error = FString::Printf(TEXT("Frozen R22 receipt runtime bytes changed: SHA1=%s bytes=%d; expected SHA1=%s bytes=%d."),
			*Out.ActualSha1, Out.ActualBytes, OracleReceiptSha1, OracleReceiptBytes);
		return false;
	}

	FString ReceiptText;
	if (!FFileHelper::LoadFileToString(ReceiptText, *Out.Path))
	{
		Out.Error = TEXT("Frozen R22 receipt bytes matched but UTF-8 text read failed.");
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReceiptText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		Out.Error = TEXT("Frozen R22 oracle receipt is not valid JSON.");
		return false;
	}

	const TSharedPtr<FJsonObject>* NativePointer = nullptr;
	if (!Root->TryGetObjectField(TEXT("native"), NativePointer) || !NativePointer || !NativePointer->IsValid())
	{
		Out.Error = TEXT("Frozen R22 receipt has no native result object.");
		return false;
	}
	const TSharedRef<FJsonObject> Native = NativePointer->ToSharedRef();
	if (!Native->TryGetStringField(TEXT("full_fingerprint"), Out.Fingerprint)
		|| !Native->TryGetNumberField(TEXT("full_non_solid_triangles"), Out.NonSolidTriangles))
	{
		Out.Error = TEXT("Frozen R22 receipt is missing its full fingerprint or non-solid triangle count.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Histogram = nullptr;
	if (!Native->TryGetArrayField(TEXT("physical_material_triangle_histogram"), Histogram) || !Histogram)
	{
		Out.Error = TEXT("Frozen R22 receipt is missing its PhysicalMaterial histogram.");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Histogram)
	{
		const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Path;
		double Triangles = -1.0;
		if (!Row.IsValid() || !Row->TryGetStringField(TEXT("physical_material"), Path)
			|| !Row->TryGetNumberField(TEXT("full_triangles"), Triangles))
		{
			Out.Error = TEXT("Frozen R22 receipt contains a malformed PhysicalMaterial histogram row.");
			return false;
		}
		Out.PhysicalMaterialHistogram.Add(Path, static_cast<int64>(Triangles));
	}

	if (Out.Fingerprint != ExpectedFullFingerprint || Out.NonSolidTriangles != ExpectedNonSolidTriangles)
	{
		Out.Error = TEXT("Frozen R22 receipt content differs from its compiled fingerprint/non-solid oracle.");
		return false;
	}
	for (const FIMExpectedPhysicalMaterialCount& Expected : ExpectedPhysicalMaterials)
	{
		if (Out.PhysicalMaterialHistogram.FindRef(Expected.Path) != Expected.Triangles)
		{
			Out.Error = TEXT("Frozen R22 receipt PhysicalMaterial counts differ at path: ") + FString(Expected.Path);
			return false;
		}
	}
	if (Out.PhysicalMaterialHistogram.Num() != UE_ARRAY_COUNT(ExpectedPhysicalMaterials))
	{
		Out.Error = TEXT("Frozen R22 receipt contains an unexpected PhysicalMaterial path.");
		return false;
	}
	Out.bOracleContentsMatch = true;
	return true;
}

static TSharedRef<FJsonObject> OracleToJson(const FIMOracleObservation& Oracle)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("receipt_path"), Oracle.Path);
	Json->SetStringField(TEXT("expected_sha256"), Oracle.ExpectedSha256);
	Json->SetStringField(TEXT("sha256_verification_scope"), TEXT("Root prelaunch/build/run receipt; not computed by this runtime test."));
	Json->SetStringField(TEXT("expected_sha1"), OracleReceiptSha1);
	Json->SetStringField(TEXT("actual_sha1"), Oracle.ActualSha1);
	Json->SetNumberField(TEXT("expected_bytes"), OracleReceiptBytes);
	Json->SetNumberField(TEXT("actual_bytes"), Oracle.ActualBytes);
	Json->SetBoolField(TEXT("runtime_bytes_match"), Oracle.bRuntimeBytesMatch);
	Json->SetStringField(TEXT("expected_full_fingerprint"), ExpectedFullFingerprint);
	Json->SetStringField(TEXT("receipt_full_fingerprint"), Oracle.Fingerprint);
	Json->SetNumberField(TEXT("expected_full_non_solid_triangles"), double(ExpectedNonSolidTriangles));
	Json->SetNumberField(TEXT("receipt_full_non_solid_triangles"), double(Oracle.NonSolidTriangles));
	Json->SetBoolField(TEXT("receipt_contents_match"), Oracle.bOracleContentsMatch);
	Json->SetStringField(TEXT("error"), Oracle.Error);
	return Json;
}

static TSharedRef<FJsonObject> ProfileToJson(AIMAcousticBakeVolume* Volume)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!IsValid(Volume))
	{
		return Json;
	}
	Json->SetStringField(TEXT("material_drive"), Volume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial
		? TEXT("PhysicalMaterial") : TEXT("LegacyRenderMaterial"));
	Json->SetNumberField(TEXT("probe_spacing_cm"), Volume->ProbeSpacingCm);
	Json->SetNumberField(TEXT("probe_height_cm"), Volume->ProbeHeightCm);
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FIMAcousticPhysicalMaterialMapping& Mapping : Volume->PhysicalMaterials)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("physical_material_path"), IsValid(Mapping.PhysicalMaterial.Get())
			? Mapping.PhysicalMaterial->GetPathName() : TEXT("invalid"));
		Row->SetObjectField(TEXT("absorption"), VectorToJson(Mapping.Absorption));
		Row->SetNumberField(TEXT("scattering"), Mapping.Scattering);
		Row->SetBoolField(TEXT("solid_geometry"), Mapping.bSolidGeometry);
		Row->SetBoolField(TEXT("supports_probes"), Mapping.bSupportsProbes);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Json->SetArrayField(TEXT("physical_material_profile"), Rows);
	return Json;
}

struct FIMRuntimeCVarObservation
{
	bool bGrassCaptureTraceDataFound = false;
	int32 GrassCaptureTraceDataValue = INDEX_NONE;
	bool bDFShadowAsyncComputeFound = false;
	int32 DFShadowAsyncComputeValue = INDEX_NONE;

	bool Passed() const
	{
		return bGrassCaptureTraceDataFound && GrassCaptureTraceDataValue == 0
			&& bDFShadowAsyncComputeFound && DFShadowAsyncComputeValue == 0;
	}
};

static FIMRuntimeCVarObservation ReadRuntimeCVarObservation()
{
	FIMRuntimeCVarObservation Out;
	if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("IM.Acoustic.GrassCaptureTraceData")))
	{
		Out.bGrassCaptureTraceDataFound = true;
		Out.GrassCaptureTraceDataValue = Variable->GetInt();
	}
	if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DFShadowAsyncCompute")))
	{
		Out.bDFShadowAsyncComputeFound = true;
		Out.DFShadowAsyncComputeValue = Variable->GetInt();
	}
	return Out;
}

static TSharedRef<FJsonObject> MakeRunReport(const FString& RunId, const FString& StartUtc,
	const FString& WorldPath, const FString& WorldPointer, AIMAcousticBakeVolume* Volume,
	const FBox& Region, const TArray<FString>& DirtyBefore, const TArray<FString>& DirtyAfter,
	const FIMOracleObservation& Oracle, const FIMPlanObservation& BaselinePlan, const FIMNativeObservation& BaselineNative,
	const FIMPlanObservation& FarPlan, const FIMNativeObservation& FarNative,
	const FIMPlanObservation& ParticipatingPlan, const FIMNativeObservation& ParticipatingNative,
	const FIMPlanObservation& RestoredPlan, const FIMNativeObservation& RestoredNative,
	ULandscapeComponent* FarComponent, const FString& FarTransition, int32 FarNumElements,
	const FString& FarDataPointerBefore, const FString& FarDataPointerAfter, const FString& FarHashBefore, const FString& FarHashAfter,
	ULandscapeComponent* ParticipatingComponent, int32 ParticipatingNumElements,
	const FString& ParticipatingPointerBefore, const FString& ParticipatingPointerAfter,
	const FString& ParticipatingHashBefore, const FString& ParticipatingHashAfter,
		const FString& OriginalBakedFieldPointer, const FString& OriginalSceneFingerprint,
		const FString& BakedFieldPointerAfter, const FString& SceneFingerprintAfter,
		const FIMScaleBoundsObservation& ScaleBounds, const FIMRuntimeCVarObservation& RuntimeCVars,
		const TArray<FString>& Errors, bool bPreconditions, bool bFarPass, bool bParticipatingFailClosed,
		bool bRestoredPass, bool bStatePass, bool bPass)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("test"), TEXT("IceMoon.AcousticField.Rural.GrassRegionFreshnessContract"));
	Root->SetStringField(TEXT("run_id"), RunId);
	Root->SetStringField(TEXT("started_utc"), StartUtc);
	Root->SetStringField(TEXT("result"), bPass ? TEXT("PASS") : TEXT("FAIL"));
	Root->SetBoolField(TEXT("preconditions_passed"), bPreconditions);
	Root->SetBoolField(TEXT("far_region_exclusion_passed"), bFarPass);
	Root->SetBoolField(TEXT("participating_missing_grassdata_failed_closed"), bParticipatingFailClosed);
	Root->SetBoolField(TEXT("restored_native_passed"), bRestoredPass);
	Root->SetBoolField(TEXT("state_restoration_passed"), bStatePass);
	Root->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	TSharedRef<FJsonObject> CVarJson = MakeShared<FJsonObject>();
	CVarJson->SetBoolField(TEXT("passed"), RuntimeCVars.Passed());
	CVarJson->SetBoolField(TEXT("im_acoustic_grass_capture_trace_data_registered"), RuntimeCVars.bGrassCaptureTraceDataFound);
	CVarJson->SetNumberField(TEXT("im_acoustic_grass_capture_trace_data_effective_value"), RuntimeCVars.GrassCaptureTraceDataValue);
	CVarJson->SetBoolField(TEXT("r_df_shadow_async_compute_registered"), RuntimeCVars.bDFShadowAsyncComputeFound);
	CVarJson->SetNumberField(TEXT("r_df_shadow_async_compute_effective_value"), RuntimeCVars.DFShadowAsyncComputeValue);
	Root->SetObjectField(TEXT("runtime_cvars"), CVarJson);
	TSharedRef<FJsonObject> ScaleBoundsJson = MakeShared<FJsonObject>();
	ScaleBoundsJson->SetBoolField(TEXT("ran"), ScaleBounds.bRan);
	ScaleBoundsJson->SetBoolField(TEXT("passed"), ScaleBounds.bPassed);
	ScaleBoundsJson->SetBoolField(TEXT("opposite_flt_max_interval_rejected"), ScaleBounds.bExtremeIntervalRejected);
	ScaleBoundsJson->SetStringField(TEXT("transient_mesh_path"), ScaleBounds.MeshPath);
	ScaleBoundsJson->SetStringField(TEXT("error"), ScaleBounds.Error);
	ScaleBoundsJson->SetStringField(TEXT("extreme_interval_error"), ScaleBounds.ExtremeIntervalError);
	ScaleBoundsJson->SetNumberField(TEXT("start_platform_seconds"), ScaleBounds.Stamp.StartSeconds);
	ScaleBoundsJson->SetNumberField(TEXT("end_platform_seconds"), ScaleBounds.Stamp.EndSeconds);
	ScaleBoundsJson->SetNumberField(TEXT("start_gt_frame"), double(ScaleBounds.Stamp.StartFrame));
	ScaleBoundsJson->SetNumberField(TEXT("end_gt_frame"), double(ScaleBounds.Stamp.EndFrame));
	ScaleBoundsJson->SetNumberField(TEXT("known_negative_y_reach_cm"), 4.0);
	ScaleBoundsJson->SetNumberField(TEXT("mesh_unscaled_radius_cm"), ScaleBounds.MeshUnscaledRadius);
	ScaleBoundsJson->SetNumberField(TEXT("free_x_varying_y_fixed_minus4_radius_cm"), ScaleBounds.FreeRadius);
	ScaleBoundsJson->SetNumberField(TEXT("lock_xy_x_fixed_minus4_z_varying_radius_cm"), ScaleBounds.LockXYRadius);
	ScaleBoundsJson->SetNumberField(TEXT("all_fixed_negative_radius_cm"), ScaleBounds.AllFixedNegativeRadius);
	ScaleBoundsJson->SetStringField(TEXT("oracle"), TEXT("Production IMAcousticGrassBakePlan::ReadVarietyBounds (forwarding to the production reader) called directly on transient StaticMesh bounds."));
	Root->SetObjectField(TEXT("scale_bounds_boundary_control"), ScaleBoundsJson);

	TSharedRef<FJsonObject> Editor = MakeShared<FJsonObject>();
	Editor->SetStringField(TEXT("world_package"), WorldPath);
	Editor->SetStringField(TEXT("world_identity"), WorldPointer);
	Editor->SetStringField(TEXT("bake_volume_identity"), IsValid(Volume) ? PointerToString(Volume) : TEXT("null"));
	Editor->SetStringField(TEXT("bake_volume_path"), IsValid(Volume) ? Volume->GetPathName() : TEXT("invalid"));
	Editor->SetBoolField(TEXT("idle_editor_and_exact_world"), bPreconditions);
	Editor->SetArrayField(TEXT("dirty_packages_before"), StringsToJson(DirtyBefore));
	Editor->SetArrayField(TEXT("dirty_packages_after"), StringsToJson(DirtyAfter));
	Editor->SetBoolField(TEXT("global_dirty_set_unchanged"), DirtyBefore == DirtyAfter);
	Root->SetObjectField(TEXT("editor"), Editor);

	TSharedRef<FJsonObject> RegionJson = MakeShared<FJsonObject>();
	RegionJson->SetObjectField(TEXT("min_world_cm"), VectorToJson(Region.Min));
	RegionJson->SetObjectField(TEXT("max_world_cm"), VectorToJson(Region.Max));
	Root->SetObjectField(TEXT("region"), RegionJson);
	Root->SetObjectField(TEXT("frozen_r22_oracle"), OracleToJson(Oracle));
	Root->SetObjectField(TEXT("physical_material_profile_after"), ProfileToJson(Volume));

	TArray<TSharedPtr<FJsonValue>> Phases;
	Phases.Add(MakeShared<FJsonValueObject>(PlanToJson(BaselinePlan)));
	Phases.Add(MakeShared<FJsonValueObject>(NativeToJson(TEXT("baseline_native_capture"), BaselineNative)));
	Phases.Add(MakeShared<FJsonValueObject>(PlanToJson(FarPlan)));
	Phases.Add(MakeShared<FJsonValueObject>(NativeToJson(TEXT("far_source_invalid_native_capture"), FarNative)));
	Phases.Add(MakeShared<FJsonValueObject>(PlanToJson(ParticipatingPlan)));
	Phases.Add(MakeShared<FJsonValueObject>(NativeToJson(TEXT("participating_source_invalid_native_capture"), ParticipatingNative)));
	Phases.Add(MakeShared<FJsonValueObject>(PlanToJson(RestoredPlan)));
	Phases.Add(MakeShared<FJsonValueObject>(NativeToJson(TEXT("restored_native_capture"), RestoredNative)));
	Root->SetArrayField(TEXT("phases"), Phases);

	TSharedRef<FJsonObject> Faults = MakeShared<FJsonObject>();
	Faults->SetStringField(TEXT("far_component"), IsValid(FarComponent) ? FarComponent->GetPathName() : TEXT("unavailable"));
	Faults->SetStringField(TEXT("far_original_transition"), FarTransition);
	Faults->SetNumberField(TEXT("far_original_num_elements"), FarNumElements);
	Faults->SetStringField(TEXT("far_data_pointer_before"), FarDataPointerBefore);
	Faults->SetStringField(TEXT("far_data_pointer_after_restore"), FarDataPointerAfter);
	Faults->SetStringField(TEXT("far_data_sha1_before"), FarHashBefore);
	Faults->SetStringField(TEXT("far_data_sha1_after_restore"), FarHashAfter);
	Faults->SetStringField(TEXT("participating_component"), IsValid(ParticipatingComponent)
		? ParticipatingComponent->GetPathName() : TEXT("unavailable"));
	Faults->SetNumberField(TEXT("participating_original_num_elements"), ParticipatingNumElements);
	Faults->SetStringField(TEXT("participating_data_pointer_before"), ParticipatingPointerBefore);
	Faults->SetStringField(TEXT("participating_data_pointer_after_restore"), ParticipatingPointerAfter);
	Faults->SetStringField(TEXT("participating_data_sha1_before"), ParticipatingHashBefore);
	Faults->SetStringField(TEXT("participating_data_sha1_after_restore"), ParticipatingHashAfter);
	Root->SetObjectField(TEXT("fault_injections"), Faults);

	TSharedRef<FJsonObject> BoundField = MakeShared<FJsonObject>();
	BoundField->SetStringField(TEXT("baked_field_pointer_before"), OriginalBakedFieldPointer);
	BoundField->SetStringField(TEXT("scene_fingerprint_before"), OriginalSceneFingerprint);
	BoundField->SetStringField(TEXT("baked_field_pointer_after"), BakedFieldPointerAfter);
	BoundField->SetStringField(TEXT("scene_fingerprint_after"), SceneFingerprintAfter);
	BoundField->SetBoolField(TEXT("pointer_and_fingerprint_unchanged"),
		OriginalBakedFieldPointer == BakedFieldPointerAfter && OriginalSceneFingerprint == SceneFingerprintAfter);
	Root->SetObjectField(TEXT("bound_baked_field"), BoundField);
	return Root;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticGrassRegionFreshnessContract,
	"IceMoon.AcousticField.Rural.GrassRegionFreshnessContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticGrassRegionFreshnessContract::RunTest(const FString&)
{
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const FString StartedUtc = FDateTime::UtcNow().ToIso8601();
	TArray<FString> Errors;
	const IMAcousticGrassRegionFreshnessTestPrivate::FIMRuntimeCVarObservation RuntimeCVars = IMAcousticGrassRegionFreshnessTestPrivate::ReadRuntimeCVarObservation();
	auto AddError = [&Errors](const FString& Error)
	{
		if (!Error.IsEmpty())
		{
			Errors.Add(Error);
		}
	};

	const TArray<FString> DirtyBefore = IMAcousticGrassRegionFreshnessTestPrivate::DirtyPackageNames();
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FString WorldPath = IsValid(World) ? World->GetOutermost()->GetName() : TEXT("invalid");
	const FString WorldPointer = IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(World);
	TArray<AIMAcousticBakeVolume*> BakeVolumes;
	if (IsValid(World))
	{
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
		{
			BakeVolumes.Add(*It);
		}
	}
	AIMAcousticBakeVolume* Volume = BakeVolumes.Num() == 1 ? BakeVolumes[0] : nullptr;
	FBox Region(ForceInit);
	if (IsValid(Volume) && IsValid(Volume->BakeBounds))
	{
		Region = Volume->BakeBounds->Bounds.GetBox();
	}

	bool bPreconditions = GEditor && IsValid(World) && World->WorldType == EWorldType::Editor
		&& GEditor->PlayWorld == nullptr && GEditor->GetEditorWorldContext().World() == World
		&& WorldPath == IMAcousticGrassRegionFreshnessTestPrivate::TestMap && !World->GetOutermost()->IsDirty() && DirtyBefore.IsEmpty()
		&& BakeVolumes.Num() == 1 && IsValid(Volume) && IsValid(Volume->BakeBounds)
		&& Region.IsValid && Volume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial
		&& IsValid(Volume->BakedField) && Volume->BakedField->WorldPackage == IMAcousticGrassRegionFreshnessTestPrivate::TestMap
		&& !Volume->BakedField->SceneFingerprint.IsEmpty();
	if (!bPreconditions)
	{
		AddError(FString::Printf(TEXT("Preconditions require Idle Editor, exact clean map, no dirty WIP packages, exactly one BakeVolume, PhysicalMaterial drive, and a bound old BakeField; world=%s volume_count=%d dirty_packages=%d."),
			*WorldPath, BakeVolumes.Num(), DirtyBefore.Num()));
	}
	const bool bRuntimeCVarsPass = RuntimeCVars.Passed();
	if (!bRuntimeCVarsPass)
	{
		AddError(FString::Printf(TEXT("Runtime CVars require IM.Acoustic.GrassCaptureTraceData=0 and r.DFShadowAsyncCompute=0; found registered=%s value=%d, registered=%s value=%d."),
			RuntimeCVars.bGrassCaptureTraceDataFound ? TEXT("true") : TEXT("false"), RuntimeCVars.GrassCaptureTraceDataValue,
			RuntimeCVars.bDFShadowAsyncComputeFound ? TEXT("true") : TEXT("false"), RuntimeCVars.DFShadowAsyncComputeValue));
	}

	IMAcousticGrassRegionFreshnessTestPrivate::FIMOracleObservation Oracle;
	const bool bOracleValid = IMAcousticGrassRegionFreshnessTestPrivate::ReadOracleReceipt(Oracle);
	if (!bOracleValid)
	{
		AddError(Oracle.Error);
	}
	const IMAcousticGrassRegionFreshnessTestPrivate::FIMScaleBoundsObservation ScaleBounds = IMAcousticGrassRegionFreshnessTestPrivate::RunScaleBoundsBoundaryControl();
	if (!ScaleBounds.bPassed)
	{
		AddError(ScaleBounds.Error);
	}

	IMAcousticGrassRegionFreshnessTestPrivate::FIMPlanObservation BaselinePlan;
	IMAcousticGrassRegionFreshnessTestPrivate::FIMNativeObservation BaselineNative;
	IMAcousticGrassRegionFreshnessTestPrivate::FIMPlanObservation FarPlan;
	IMAcousticGrassRegionFreshnessTestPrivate::FIMNativeObservation FarNative;
	IMAcousticGrassRegionFreshnessTestPrivate::FIMPlanObservation ParticipatingPlan;
	IMAcousticGrassRegionFreshnessTestPrivate::FIMNativeObservation ParticipatingNative;
	IMAcousticGrassRegionFreshnessTestPrivate::FIMPlanObservation RestoredPlan;
	IMAcousticGrassRegionFreshnessTestPrivate::FIMNativeObservation RestoredNative;

	const FString OriginalBakedFieldPointer = IsValid(Volume) ? IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(Volume->BakedField.Get()) : TEXT("null");
	const FString OriginalSceneFingerprint = IsValid(Volume) && IsValid(Volume->BakedField)
		? Volume->BakedField->SceneFingerprint : FString();
	const FTransform OriginalVolumeTransform = IsValid(Volume) ? Volume->GetActorTransform() : FTransform::Identity;
	const FTransform OriginalBakeBoundsTransform = IsValid(Volume) && IsValid(Volume->BakeBounds)
		? Volume->BakeBounds->GetComponentTransform() : FTransform::Identity;
	const EIMAcousticMaterialDrive OriginalMaterialDrive = IsValid(Volume)
		? Volume->MaterialDrive : EIMAcousticMaterialDrive::LegacyRenderMaterial;
	const float OriginalProbeSpacing = IsValid(Volume) ? Volume->ProbeSpacingCm : 0.0f;
	const float OriginalProbeHeight = IsValid(Volume) ? Volume->ProbeHeightCm : 0.0f;
	TArray<IMAcousticGrassRegionFreshnessTestPrivate::FIMPhysicalMaterialSnapshot> OriginalPhysicalProfile;
	IMAcousticGrassRegionFreshnessTestPrivate::CapturePhysicalMaterialProfile(Volume, OriginalPhysicalProfile);

	bool bFarPass = false;
	bool bParticipatingFailClosed = false;
	bool bRestoredPass = false;
	bool bStatePass = false;
	ULandscapeComponent* FarComponent = nullptr;
	ULandscapeComponent* ParticipatingComponent = nullptr;
	FString FarTransition = TEXT("NOT_RUN");
	int32 FarNumElements = INDEX_NONE;
	int32 ParticipatingNumElements = INDEX_NONE;
	FString FarDataPointerBefore = TEXT("unavailable");
	FString FarDataPointerAfter = TEXT("unavailable");
	FString FarHashBefore = TEXT("unavailable");
	FString FarHashAfter = TEXT("unavailable");
	FString ParticipatingPointerBefore = TEXT("unavailable");
	FString ParticipatingPointerAfter = TEXT("unavailable");
	FString ParticipatingHashBefore = TEXT("unavailable");
	FString ParticipatingHashAfter = TEXT("unavailable");
	TArray<IMAcousticGrassRegionFreshnessTestPrivate::FIMWorldTransformSnapshot> SourceTransformsBefore;

	if (bPreconditions && bOracleValid && bRuntimeCVarsPass)
	{
		IMAcousticGrassRegionFreshnessTestPrivate::RunPlan(World, Region, BaselinePlan);
		IMAcousticGrassRegionFreshnessTestPrivate::RunNativeCapture(Volume, BaselineNative);
		if (!BaselinePlan.bSucceeded)
		{
			AddError(TEXT("Baseline pure Plan failed: ") + BaselinePlan.Error);
		}
		if (BaselinePlan.bSucceeded)
		{
			IMAcousticGrassRegionFreshnessTestPrivate::CaptureTransformSnapshots(BaselinePlan.Plan, SourceTransformsBefore);
			if (BaselinePlan.Plan.SourceInputSha1.IsEmpty() || BaselinePlan.Plan.Sources.IsEmpty()
				|| BaselinePlan.Plan.Cameras.IsEmpty() || BaselinePlan.Plan.XYDisjointComponents.IsEmpty())
			{
				AddError(TEXT("Baseline plan must contain an input SHA, selected sources, cameras, and at least one XY-disjoint component."));
			}
		}

		FString BaselineNativeError;
		const bool bBaselineNativeExpected = IMAcousticGrassRegionFreshnessTestPrivate::NativeMatchesExpected(BaselineNative, BaselineNativeError);
		if (!bBaselineNativeExpected)
		{
			AddError(TEXT("Baseline Native output differs from frozen R22: ") + BaselineNativeError);
		}

		TArray<ULandscapeComponent*> SortedDisjoint = BaselinePlan.Plan.XYDisjointComponents;
		SortedDisjoint.Sort([](const ULandscapeComponent& A, const ULandscapeComponent& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		for (ULandscapeComponent* Candidate : SortedDisjoint)
		{
			if (IsValid(Candidate))
			{
				FarComponent = Candidate;
				break;
			}
		}
		TArray<const FIMAcousticGrassBakeSource*> SortedSources;
		for (const FIMAcousticGrassBakeSource& Source : BaselinePlan.Plan.Sources)
		{
			SortedSources.Add(&Source);
		}
		SortedSources.Sort([](const FIMAcousticGrassBakeSource& A, const FIMAcousticGrassBakeSource& B)
		{
			const FString PathA = IsValid(A.Component) ? A.Component->GetPathName() : FString();
			const FString PathB = IsValid(B.Component) ? B.Component->GetPathName() : FString();
			return PathA < PathB;
		});
		for (const FIMAcousticGrassBakeSource* Source : SortedSources)
		{
			if (Source && IsValid(Source->Component) && Source->Component->GrassData->NumElements >= 0)
			{
				ParticipatingComponent = Source->Component;
				break;
			}
		}
		if (!IsValid(FarComponent) || !IsValid(ParticipatingComponent))
		{
			AddError(TEXT("Fault injection requires one XY-disjoint component and one currently valid selected Plan source."));
		}

		if (IsValid(FarComponent) && IsValid(ParticipatingComponent)
			&& BaselinePlan.bSucceeded && BaselineNative.bCaptured)
		{
			FarNumElements = FarComponent->GrassData->NumElements;
			FarTransition = FarNumElements >= 0 ? TEXT("valid_to_invalid") : TEXT("invalid_to_invalid");
			FarDataPointerBefore = IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(&FarComponent->GrassData.Get());
			FarHashBefore = IMAcousticGrassRegionFreshnessTestPrivate::GrassDataFingerprint(FarComponent->GrassData.Get());
			{
				IMAcousticGrassRegionFreshnessTestPrivate::FIMScopedGrassDataFault Fault(FarComponent);
				if (!Fault.SubstituteInvalid())
				{
					AddError(TEXT("Could not replace selected XY-disjoint GrassData with the invalid empty-data state."));
				}
				else
				{
					IMAcousticGrassRegionFreshnessTestPrivate::RunPlan(World, Region, FarPlan);
					IMAcousticGrassRegionFreshnessTestPrivate::RunNativeCapture(Volume, FarNative);
				}
				Fault.Restore();
				FarDataPointerAfter = IsValid(FarComponent) ? IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(&FarComponent->GrassData.Get()) : TEXT("invalid_component");
				FarHashAfter = IsValid(FarComponent) ? IMAcousticGrassRegionFreshnessTestPrivate::GrassDataFingerprint(FarComponent->GrassData.Get()) : TEXT("invalid_component");
				if (!Fault.IsRestoredExactly())
				{
					AddError(TEXT("XY-disjoint GrassData pointer/content did not restore exactly after fault injection."));
				}
			}

			FString FarPlanError;
			const bool bFarPlanMatches = FarPlan.bSucceeded
				&& IMAcousticGrassRegionFreshnessTestPrivate::PlansMatch(FarPlan.Plan, BaselinePlan.Plan, FarPlanError);
			if (!FarPlan.bSucceeded)
			{
				FarPlanError = FarPlan.Error;
			}
			FString FarNativeError;
			const bool bFarNativeMatches = IMAcousticGrassRegionFreshnessTestPrivate::NativeMatchesExpected(FarNative, FarNativeError)
				&& IMAcousticGrassRegionFreshnessTestPrivate::NativeMatchesBaseline(FarNative, BaselineNative);
			if (!bFarNativeMatches && FarNativeError.IsEmpty())
			{
				FarNativeError = TEXT("Native fingerprint or counts changed from baseline after far GrassData invalidation.");
			}
			if (!bFarPlanMatches)
			{
				AddError(TEXT("Far XY-disjoint missing data changed pure Plan: ") + FarPlanError);
			}
			if (!bFarNativeMatches)
			{
				AddError(TEXT("Far XY-disjoint missing data changed actual Native output: ") + FarNativeError);
			}
			bFarPass = BaselinePlan.bRan && BaselinePlan.bSucceeded && BaselineNative.bRan && BaselineNative.bCaptured
				&& FarPlan.bRan && FarNative.bRan
				&& bFarPlanMatches && bFarNativeMatches
				&& FarDataPointerBefore == FarDataPointerAfter && FarHashBefore == FarHashAfter;

			ParticipatingNumElements = ParticipatingComponent->GrassData->NumElements;
			ParticipatingPointerBefore = IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(&ParticipatingComponent->GrassData.Get());
			ParticipatingHashBefore = IMAcousticGrassRegionFreshnessTestPrivate::GrassDataFingerprint(ParticipatingComponent->GrassData.Get());
			{
				IMAcousticGrassRegionFreshnessTestPrivate::FIMScopedGrassDataFault Fault(ParticipatingComponent);
				if (!Fault.WasOriginalValid() || !Fault.SubstituteInvalid())
				{
					AddError(TEXT("Participating source was not valid immediately before invalid-data injection."));
				}
				else
				{
					IMAcousticGrassRegionFreshnessTestPrivate::RunPlan(World, Region, ParticipatingPlan);
					IMAcousticGrassRegionFreshnessTestPrivate::RunNativeCapture(Volume, ParticipatingNative);
				}
				Fault.Restore();
				ParticipatingPointerAfter = IsValid(ParticipatingComponent)
					? IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(&ParticipatingComponent->GrassData.Get()) : TEXT("invalid_component");
				ParticipatingHashAfter = IsValid(ParticipatingComponent)
					? IMAcousticGrassRegionFreshnessTestPrivate::GrassDataFingerprint(ParticipatingComponent->GrassData.Get()) : TEXT("invalid_component");
				if (!Fault.IsRestoredExactly())
				{
					AddError(TEXT("Participating GrassData pointer/content did not restore exactly after fault injection."));
				}
			}

			const bool bParticipatingPlanFailed = ParticipatingPlan.bRan && !ParticipatingPlan.bSucceeded
				&& !ParticipatingPlan.Error.IsEmpty();
			const bool bParticipatingNativeFailed = ParticipatingNative.bRan && !ParticipatingNative.bCaptured
				&& !ParticipatingNative.Error.IsEmpty();
			const bool bBoundFieldUnchangedAfterFailure = IsValid(Volume)
				&& IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(Volume->BakedField.Get()) == OriginalBakedFieldPointer
				&& IsValid(Volume->BakedField) && Volume->BakedField->SceneFingerprint == OriginalSceneFingerprint;
			bParticipatingFailClosed = bParticipatingPlanFailed && bParticipatingNativeFailed && bBoundFieldUnchangedAfterFailure
				&& ParticipatingPointerBefore == ParticipatingPointerAfter && ParticipatingHashBefore == ParticipatingHashAfter;
			if (!bParticipatingPlanFailed)
			{
				AddError(TEXT("Pure Plan did not fail with a retained error when a participating source's GrassData was invalid."));
			}
			if (!bParticipatingNativeFailed)
			{
				AddError(TEXT("Native capture did not fail with a retained error when a participating source's GrassData was invalid."));
			}
			if (!bBoundFieldUnchangedAfterFailure)
			{
				AddError(TEXT("Bound old BakedField pointer or SceneFingerprint changed during expected Native failure."));
			}

			IMAcousticGrassRegionFreshnessTestPrivate::RunPlan(World, Region, RestoredPlan);
			IMAcousticGrassRegionFreshnessTestPrivate::RunNativeCapture(Volume, RestoredNative);
			FString RestoredPlanError;
			const bool bRestoredPlanMatches = RestoredPlan.bSucceeded
				&& IMAcousticGrassRegionFreshnessTestPrivate::PlansMatch(RestoredPlan.Plan, BaselinePlan.Plan, RestoredPlanError);
			if (!RestoredPlan.bSucceeded)
			{
				RestoredPlanError = RestoredPlan.Error;
			}
			FString RestoredNativeError;
			const bool bRestoredNativeMatches = IMAcousticGrassRegionFreshnessTestPrivate::NativeMatchesExpected(RestoredNative, RestoredNativeError)
				&& IMAcousticGrassRegionFreshnessTestPrivate::NativeMatchesBaseline(RestoredNative, BaselineNative);
			if (!bRestoredPlanMatches)
			{
				AddError(TEXT("Restored pure Plan differs from baseline: ") + RestoredPlanError);
			}
			if (!bRestoredNativeMatches)
			{
				AddError(TEXT("Restored Native output differs from baseline or frozen R22: ") + RestoredNativeError);
			}
			bRestoredPass = bRestoredPlanMatches && bRestoredNativeMatches;
		}
	}

	const TArray<FString> DirtyAfter = IMAcousticGrassRegionFreshnessTestPrivate::DirtyPackageNames();
	const FString BakedFieldPointerAfter = IsValid(Volume) ? IMAcousticGrassRegionFreshnessTestPrivate::PointerToString(Volume->BakedField.Get()) : TEXT("null");
	const FString SceneFingerprintAfter = IsValid(Volume) && IsValid(Volume->BakedField)
		? Volume->BakedField->SceneFingerprint : FString();
	const bool bBoundFieldUnchangedAfterAll = OriginalBakedFieldPointer == BakedFieldPointerAfter
		&& OriginalSceneFingerprint == SceneFingerprintAfter;
	const bool bVolumeTransformUnchanged = IsValid(Volume) && Volume->GetActorTransform().Equals(OriginalVolumeTransform, 0.0)
		&& IsValid(Volume->BakeBounds) && Volume->BakeBounds->GetComponentTransform().Equals(OriginalBakeBoundsTransform, 0.0);
	const bool bProfileUnchanged = IMAcousticGrassRegionFreshnessTestPrivate::PhysicalMaterialProfileMatches(Volume, OriginalMaterialDrive,
		OriginalPhysicalProfile, OriginalProbeSpacing, OriginalProbeHeight);
	const bool bSourceTransformsUnchanged = IMAcousticGrassRegionFreshnessTestPrivate::TransformSnapshotsMatch(SourceTransformsBefore);
	const bool bDirtyStateUnchanged = DirtyBefore == DirtyAfter && IsValid(World)
		&& !World->GetOutermost()->IsDirty() && DirtyAfter.IsEmpty();
	bStatePass = bBoundFieldUnchangedAfterAll && bVolumeTransformUnchanged && bProfileUnchanged
		&& bSourceTransformsUnchanged && bDirtyStateUnchanged
		&& FarDataPointerBefore == FarDataPointerAfter && FarHashBefore == FarHashAfter
		&& ParticipatingPointerBefore == ParticipatingPointerAfter && ParticipatingHashBefore == ParticipatingHashAfter;
	if (!bBoundFieldUnchangedAfterAll)
	{
		AddError(TEXT("Bound BakedField pointer or SceneFingerprint changed during the complete contract test."));
	}
	if (!bVolumeTransformUnchanged || !bSourceTransformsUnchanged)
	{
		AddError(TEXT("BakeVolume, BakeBounds, selected source, or XY-disjoint source transform changed."));
	}
	if (!bProfileUnchanged)
	{
		AddError(TEXT("PhysicalMaterial drive/profile or probe profile changed during the contract test."));
	}
	if (!bDirtyStateUnchanged)
	{
		AddError(TEXT("Map or global dirty package flags changed during the contract test."));
	}

	FString BaselinePlanError;
	const bool bBaselinePlanComplete = BaselinePlan.bRan && BaselinePlan.bSucceeded
		&& !BaselinePlan.Plan.SourceInputSha1.IsEmpty() && !BaselinePlan.Plan.Sources.IsEmpty()
		&& !BaselinePlan.Plan.Cameras.IsEmpty() && !BaselinePlan.Plan.XYDisjointComponents.IsEmpty();
	const bool bBaselineNativeComplete = BaselineNative.bRan && IMAcousticGrassRegionFreshnessTestPrivate::NativeMatchesExpected(BaselineNative, BaselinePlanError);
	const bool bBaselinePass = bBaselinePlanComplete && bBaselineNativeComplete;
	const bool bAllPass = bPreconditions && bOracleValid && bRuntimeCVarsPass && ScaleBounds.bRan && ScaleBounds.bPassed && bBaselinePass && bFarPass
		&& bParticipatingFailClosed && bRestoredPass && bStatePass;

	TSharedRef<FJsonObject> Report = IMAcousticGrassRegionFreshnessTestPrivate::MakeRunReport(RunId, StartedUtc, WorldPath, WorldPointer, Volume,
		Region, DirtyBefore, DirtyAfter, Oracle, BaselinePlan, BaselineNative, FarPlan, FarNative,
		ParticipatingPlan, ParticipatingNative, RestoredPlan, RestoredNative, FarComponent, FarTransition,
		FarNumElements, FarDataPointerBefore, FarDataPointerAfter, FarHashBefore, FarHashAfter,
		ParticipatingComponent, ParticipatingNumElements, ParticipatingPointerBefore, ParticipatingPointerAfter,
		ParticipatingHashBefore, ParticipatingHashAfter, OriginalBakedFieldPointer, OriginalSceneFingerprint,
		BakedFieldPointerAfter, SceneFingerprintAfter, ScaleBounds, RuntimeCVars, Errors, bPreconditions, bFarPass,
		bParticipatingFailClosed, bRestoredPass, bStatePass, bAllPass);
	FString ReportText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportText);
	const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
	Writer->Close();
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialDrive"));
	IFileManager::Get().MakeDirectory(*ReportDirectory, true);
	const FString ReportPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ReportDirectory,
		FString::Printf(TEXT("region_freshness_%s.json"), *RunId)));
	const bool bReportSaved = bSerialized
		&& FFileHelper::SaveStringToFile(ReportText, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!bReportSaved)
	{
		AddError(TEXT("Could not serialize or save region-freshness run report: ") + ReportPath);
	}
	const bool bFinalPass = bAllPass && bReportSaved;

	TestTrue(TEXT("Preconditions: idle GEditor, exact clean map, unique PhysicalMaterial BakeVolume, no dirty WIP packages"), bPreconditions);
	TestTrue(TEXT("Runtime CVars: IM.Acoustic.GrassCaptureTraceData=0 and r.DFShadowAsyncCompute=0"), bRuntimeCVarsPass);
	TestTrue(TEXT("Frozen R22 oracle receipt path, SHA-1/byte count, fingerprint, and PM histogram"), bOracleValid);
	TestTrue(TEXT("Production scale-bound reader covers negative-axis cases, fixed-negative default scale, and invalid interval failure"),
		ScaleBounds.bRan && ScaleBounds.bPassed);
	TestTrue(TEXT("Baseline pure Plan and Native output match the frozen region oracle"), bBaselinePass);
	TestTrue(TEXT("Missing GrassData on XY-disjoint component preserves pure Plan and Native output"), bFarPass);
	TestTrue(TEXT("Missing GrassData on participating component fails Plan and Native while preserving BakedField"), bParticipatingFailClosed);
	TestTrue(TEXT("Restored Plan and Native output match baseline and frozen R22"), bRestoredPass);
	TestTrue(TEXT("GrassData, source transforms, dirty flags, PM profile, and bound BakedField are unchanged"), bStatePass);
	TestTrue(TEXT("Run report was serialized and saved"), bReportSaved);

	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] %s GrassRegionFreshnessContract report=%s"),
		bFinalPass ? TEXT("PASS") : TEXT("FAIL"), *ReportPath);
	// The task monitor requires the declared exit policy as well as native TestExit evidence.
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor policy=EngineTestExitQueueEmpty GrassRegionFreshnessContract result=%s report=%s"),
		bFinalPass ? TEXT("PASS") : TEXT("FAIL"), *ReportPath);
	return bFinalPass;
}

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
