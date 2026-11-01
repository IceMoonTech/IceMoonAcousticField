#include "IMAcousticGrassBakePlan.h"

#include "IMAcousticBakeIdentity.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeGrassType.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"
#include "Misc/SecureHash.h"

namespace IMAcousticGrassBakePlanPrivate
{
void HashText(FSHA1& Hash, const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text);
	const int32 Bytes = Utf8.Length();
	Hash.Update(reinterpret_cast<const uint8*>(&Bytes), sizeof(Bytes));
	Hash.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Bytes);
}

template <typename T> void HashValue(FSHA1& Hash, const T& Value)
{
	Hash.Update(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
}

FString FinishHash(FSHA1& Hash)
{
	Hash.Final();
	uint8 Bytes[20];
	Hash.GetHash(Bytes);
	return BytesToHex(Bytes, UE_ARRAY_COUNT(Bytes));
}

bool ReadPositiveCVar(const TCHAR* Name, double& Value, FString& Error)
{
	const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
	Value = Variable ? double(Variable->GetFloat()) : 0.0;
	if (!FMath::IsFinite(Value) || Value <= 0.0)
	{
		Error = TEXT("Static Grass capture requires a positive current setting: ") + FString(Name);
		return false;
	}
	return true;
}

double ScaleMaximum(const FFloatInterval& Interval, bool RandomScale)
{
	// FAsyncGrassBuilder selects random scaling for the whole variety. A fixed
	// negative axis therefore keeps its magnitude when another used axis varies.
	// Weight attenuation stays in [0,1], so interpolation cannot exceed endpoints.
	if (RandomScale)
		return FMath::Max(FMath::Abs(double(Interval.Min)), FMath::Abs(double(Interval.Max)));
	return Interval.Min > 0.0f && FMath::IsNearlyZero(Interval.Size()) ? double(Interval.Min) : 1.0;
}

bool ReadVarietyBounds(const FGrassVariety& Variety, double& Radius, FString& Error)
{
	const auto IsValidScaleInterval = [](const FFloatInterval& Interval)
	{
		return FMath::IsFinite(Interval.Min) && FMath::IsFinite(Interval.Max)
			&& Interval.Min <= Interval.Max && FMath::IsFinite(Interval.Size());
	};
	bool bUsesRandomScale = false;
	bool bValidScaleIntervals = IsValidScaleInterval(Variety.ScaleX);
	switch (Variety.Scaling)
	{
	case EGrassScaling::Uniform:
		bUsesRandomScale = Variety.ScaleX.Size() > 0.0f;
		break;
	case EGrassScaling::Free:
		bValidScaleIntervals &= IsValidScaleInterval(Variety.ScaleY) && IsValidScaleInterval(Variety.ScaleZ);
		bUsesRandomScale = Variety.ScaleX.Size() > 0.0f || Variety.ScaleY.Size() > 0.0f
			|| Variety.ScaleZ.Size() > 0.0f;
		break;
	case EGrassScaling::LockXY:
		bValidScaleIntervals &= IsValidScaleInterval(Variety.ScaleZ);
		bUsesRandomScale = Variety.ScaleX.Size() > 0.0f || Variety.ScaleZ.Size() > 0.0f;
		break;
	default:
		bValidScaleIntervals = false;
		break;
	}
	if (!bValidScaleIntervals || (Variety.bWeightAttenuatesMaxScale
		&& !FMath::IsFinite(Variety.MaxScaleWeightAttenuation)))
	{
		Error = TEXT("Grass variety has invalid static scale intervals: ")
			+ IMStableBakeObjectPath(Variety.GrassMesh);
		return false;
	}
	const FBox MeshLocalBounds = Variety.GrassMesh->GetBounds().GetBox();
	const double MaxMeshScaleX = ScaleMaximum(Variety.ScaleX, bUsesRandomScale);
	const double MaxMeshScaleY = Variety.Scaling == EGrassScaling::Free ? ScaleMaximum(Variety.ScaleY, bUsesRandomScale) : MaxMeshScaleX;
	const double MaxMeshScaleZ = Variety.Scaling == EGrassScaling::Uniform ? MaxMeshScaleX : ScaleMaximum(Variety.ScaleZ, bUsesRandomScale);
	const FVector MaxAbsMeshLocalCoordinates = MeshLocalBounds.Min.GetAbs().ComponentMax(MeshLocalBounds.Max.GetAbs());
	Radius = (MaxAbsMeshLocalCoordinates * FVector(MaxMeshScaleX, MaxMeshScaleY, MaxMeshScaleZ)).Size();
	if (!MeshLocalBounds.IsValid || MeshLocalBounds.Min.ContainsNaN() || MeshLocalBounds.Max.ContainsNaN()
		|| !FMath::IsFinite(Radius) || Radius <= 0.0)
	{
		Error = TEXT("Grass mesh scale/bounds cannot provide a finite static footprint: ")
			+ IMStableBakeObjectPath(Variety.GrassMesh);
		return false;
	}
	return true;
}

bool ShouldSkipGrassVariety(const FGrassVariety& Variety)
{
	return !Variety.GrassMesh || Variety.GetDensity() <= 0 || Variety.GetEndCullDistance() <= 0;
}

bool ClassifyXYDisjoint(ULandscapeComponent* Component, const FBox& Region,
	bool& Disjoint, FString& Error)
{
	Disjoint = false;
	ALandscapeProxy* Proxy = Component->GetLandscapeProxy();
	if (!Proxy || !Proxy->GetRootComponent() || Component->ComponentSizeQuads <= 0)
	{
		Error = TEXT("Grass source has no finite builder frame: ") + IMStableBakeObjectPath(Component);
		return false;
	}
	const FVector DrawScale = Proxy->GetRootComponent()->GetRelativeScale3D();
	const FTransform NoScale(Proxy->GetRootComponent()->GetComponentTransform().ToMatrixNoScale());
	if (DrawScale.ContainsNaN() || DrawScale.GetAbs().GetMin() <= 0.0 || NoScale.ContainsNaN())
	{
		Error = TEXT("Grass source builder frame is invalid: ") + IMStableBakeObjectPath(Component);
		return false;
	}
	// GrassData supplies Z (including encoded Landscape Z WPO). With exactly
	// vertical local Z, no possible height changes this world XY projection.
	// Do not use an angular tolerance: even a small tilt can couple unknown Z
	// into XY, so tilted sources retain full data/freshness validation.
	const FVector Up = NoScale.GetRotation().RotateVector(FVector::UpVector);
	if (Up.X != 0.0 || Up.Y != 0.0) return true;
	double Radius = 0.0;
	for (const auto& Entry : Component->GetNamedGrassTypes())
	{
		const ULandscapeGrassType* Type = Entry.Value;
		if (!IsValid(Type))
		{
			Error = TEXT("Grass source has an unresolved named type: ") + Entry.Key.ToString();
			return false;
		}
		for (const FGrassVariety& Variety : Type->GrassVarieties)
		{
			if (ShouldSkipGrassVariety(Variety)) continue;
			double VarietyRadius = 0.0;
			if (!ReadVarietyBounds(Variety, VarietyRadius, Error)) return false;
			Radius = FMath::Max(Radius, VarietyRadius);
		}
	}
	if (Radius <= 0.0) return true;
	const FIntPoint Section = Component->GetSectionBase();
	const FIntPoint Offset = Proxy->GetSectionBase();
	const FVector Start(DrawScale.X * (Section.X - Offset.X),
		DrawScale.Y * (Section.Y - Offset.Y), 0.0);
	const FVector End = Start + FVector(DrawScale.X * Component->ComponentSizeQuads,
		DrawScale.Y * Component->ComponentSizeQuads, 0.0);
	// Same origin/jitter tile as FGrassBuilderBase; transform its complete XY
	// rectangle, then expand by a rotation-independent full mesh footprint.
	// This is only a conservative rejection; the accepted sources still use
	// their exact heights and every renderer instance's transformed mesh box.
	const FBox Tile = FBox(Start.ComponentMin(End), Start.ComponentMax(End)).TransformBy(NoScale);
	const FBox ExpandedRegion = Region.ExpandBy(Radius);
	if (!Tile.IsValid || Tile.Min.ContainsNaN() || Tile.Max.ContainsNaN()
		|| !ExpandedRegion.IsValid || ExpandedRegion.Min.ContainsNaN() || ExpandedRegion.Max.ContainsNaN())
	{
		Error = TEXT("Grass source XY footprint is invalid: ") + IMStableBakeObjectPath(Component);
		return false;
	}
	Disjoint = Tile.Max.X < ExpandedRegion.Min.X || Tile.Min.X > ExpandedRegion.Max.X
		|| Tile.Max.Y < ExpandedRegion.Min.Y || Tile.Min.Y > ExpandedRegion.Max.Y;
	return true;
}

void HashVariety(FSHA1& Hash, const FGrassVariety& Variety)
{
	HashText(Hash, IMStableBakeObjectPath(Variety.GrassMesh));
	const float Scalars[] = {Variety.GetDensity(), Variety.PlacementJitter,
		Variety.ScaleX.Min, Variety.ScaleX.Max, Variety.ScaleY.Min, Variety.ScaleY.Max,
		Variety.ScaleZ.Min, Variety.ScaleZ.Max, Variety.AllowedDensityRange.Min,
		Variety.AllowedDensityRange.Max, Variety.MaxScaleWeightAttenuation};
	Hash.Update(reinterpret_cast<const uint8*>(Scalars), sizeof(Scalars));
	const uint8 Flags[] = {uint8(Variety.Scaling), uint8(Variety.bUseGrid),
		uint8(Variety.RandomRotation), uint8(Variety.AlignToSurface),
		uint8(Variety.bAlignToTriangleNormals), uint8(Variety.bWeightAttenuatesMaxScale)};
	Hash.Update(Flags, sizeof(Flags));
	HashValue(Hash, Variety.GetStartCullDistance());
	HashValue(Hash, Variety.GetEndCullDistance());
	const int32 MaterialCount = Variety.OverrideMaterials.Num();
	HashValue(Hash, MaterialCount);
	for (const auto& Material : Variety.OverrideMaterials)
		HashText(Hash, IMStableBakeObjectPath(Material));
}

bool AppendSubsectionCameras(ULandscapeComponent* Component, const FVector& DrawScale,
	float Density, int32 MaxInstancesPerComponent, TArray<FVector>& Cameras, FString& Error)
{
	// Mirror UE5.8 LandscapeGrass.cpp: density is instances per 1,000,000 cm2.
	// These are Grass builder subdivisions, not Landscape's asset subsections.
	const FVector ComponentScaledSize = DrawScale * Component->ComponentSizeQuads;
	const double SqrtEstimatedInstanceCount = FMath::Sqrt(FMath::Abs(
		ComponentScaledSize.X * ComponentScaledSize.Y * Density / 1000.0f / 1000.0f));
	if (!FMath::IsFinite(SqrtEstimatedInstanceCount) || SqrtEstimatedInstanceCount > MAX_int32 - 1)
	{
		Error = TEXT("Grass subdivision input is nonfinite or unaddressable; no density reduction was substituted.");
		return false;
	}
	const int32 EstimatedInstancesPerAxis = FMath::CeilToInt(SqrtEstimatedInstanceCount);
	const int32 SubsectionsPerAxis = FMath::Clamp(FMath::CeilToInt(float(EstimatedInstancesPerAxis)
		/ FMath::Sqrt(float(MaxInstancesPerComponent))), 1, 16);
	const FBox& ComponentLocalBounds = Component->CachedLocalBox;
	const FVector SubsectionLocalSize = (ComponentLocalBounds.Max - ComponentLocalBounds.Min)
		* FVector(1.0f / float(SubsectionsPerAxis), 1.0f / float(SubsectionsPerAxis), 1.0f);
	if (Cameras.Num() > MAX_int32 - SubsectionsPerAxis * SubsectionsPerAxis)
	{
		Error = TEXT("Grass subsection camera array is unaddressable; no reduced coverage was substituted.");
		return false;
	}
	for (int32 X = 0; X < SubsectionsPerAxis; ++X)
		for (int32 Y = 0; Y < SubsectionsPerAxis; ++Y)
		{
			const FVector SubsectionLocalMin = ComponentLocalBounds.Min
				+ SubsectionLocalSize * FVector(float(X), float(Y), 0.0f);
			const FVector SubsectionLocalMax = ComponentLocalBounds.Min
				+ SubsectionLocalSize * FVector(float(X + 1), float(Y + 1), 1.0f);
			const FBox SubsectionWorldBounds = FBox(SubsectionLocalMin, SubsectionLocalMax)
				.TransformBy(Component->GetComponentTransform());
			if (!SubsectionWorldBounds.IsValid || SubsectionWorldBounds.Min.ContainsNaN()
				|| SubsectionWorldBounds.Max.ContainsNaN())
			{
				Error = TEXT("Grass generation subsection has invalid bounds.");
				return false;
			}
			// Distance to this actual admission AABB is zero, independent of
			// small cull radii. Same-sized varieties share exactly equal centers.
			Cameras.AddUnique(SubsectionWorldBounds.GetCenter());
		}
	return true;
}

bool ReadSource(ULandscapeComponent* Component, double GuardBand, double CullScale,
	float DensityScale, int32 MaxInstancesPerComponent,
	FIMAcousticGrassBakeSource& Source, FString& Error)
{
	ALandscapeProxy* Proxy = Component->GetLandscapeProxy();
	const auto& NamedTypes = Component->GetNamedGrassTypes();
	const auto& GrassData = *Component->GrassData;
	if (!Proxy || !Component->IsRegistered() || !Proxy->GetRootComponent()
		|| GrassData.NumElements < 0 || (GrassData.NumElements == 0 && !GrassData.WeightOffsets.IsEmpty()))
	{
		Error = FString::Printf(TEXT("Grass source/registered data is unavailable: %s [proxy=%d registered=%d root=%d data_address=%llu elements=%d bytes=%d weight_offsets=%d generation_hash=%u named_types=%d]"),
			*IMStableBakeObjectPath(Component), Proxy ? 1 : 0,
			Component->IsRegistered() ? 1 : 0, Proxy && Proxy->GetRootComponent() ? 1 : 0,
			uint64(reinterpret_cast<UPTRINT>(&GrassData)), GrassData.NumElements,
			GrassData.HeightWeightData.Num(), GrassData.WeightOffsets.Num(), GrassData.GenerationHash, NamedTypes.Num());
		return false;
	}
#if WITH_EDITOR
	if (Component->GetWorld()->WorldType == EWorldType::Editor
		&& GrassData.GenerationHash != Component->ComputeGrassMapGenerationHash())
	{
		Error = TEXT("GrassData is stale; current generated geometry cannot be certified: ")
			+ IMStableBakeObjectPath(Component);
		return false;
	}
#endif
	Source.Component = Component;
	Source.GenerationWorldBounds = Component->CachedLocalBox.TransformBy(Component->GetComponentTransform());
	Source.CreationRadiusCm = TNumericLimits<double>::Max();
	FSHA1 SourceInputHash;
	HashText(SourceInputHash, IMStableBakeObjectPath(Component));
	const FIntPoint Section = Component->GetSectionBase();
	const FIntPoint Offset = Proxy->GetSectionBase();
	const FVector DrawScale = Proxy->GetRootComponent()->GetRelativeScale3D();
	const FTransform NoScaleTransform(Proxy->GetRootComponent()->GetComponentTransform().ToMatrixNoScale());
	HashValue(SourceInputHash, Section.X); HashValue(SourceInputHash, Section.Y);
	HashValue(SourceInputHash, Offset.X); HashValue(SourceInputHash, Offset.Y);
	HashValue(SourceInputHash, DrawScale.X); HashValue(SourceInputHash, DrawScale.Y); HashValue(SourceInputHash, DrawScale.Z);
	const FVector Location = NoScaleTransform.GetLocation();
	const FQuat Rotation = NoScaleTransform.GetRotation();
	HashValue(SourceInputHash, Location.X); HashValue(SourceInputHash, Location.Y); HashValue(SourceInputHash, Location.Z);
	HashValue(SourceInputHash, Rotation.X); HashValue(SourceInputHash, Rotation.Y);
	HashValue(SourceInputHash, Rotation.Z); HashValue(SourceInputHash, Rotation.W);
	HashValue(SourceInputHash, GrassData.NumElements);
	const int32 GrassDataByteCount = GrassData.HeightWeightData.Num();
	HashValue(SourceInputHash, GrassDataByteCount);
	SourceInputHash.Update(GrassData.HeightWeightData.GetData(), GrassDataByteCount);
	TArray<FName> WeightNames;
	GrassData.WeightOffsets.GetKeys(WeightNames);
	WeightNames.Sort(FNameLexicalLess());
	HashValue(SourceInputHash, WeightNames.Num());
	for (FName Name : WeightNames)
	{
		const int32 WeightOffset = GrassData.WeightOffsets.FindChecked(Name);
		if (GrassData.NumElements <= 0 || int64(WeightOffset) < int64(GrassData.NumElements) * int64(sizeof(uint16))
			|| int64(WeightOffset) + GrassData.NumElements > GrassDataByteCount)
		{
			Error = TEXT("GrassData named weight range is incomplete: ") + IMStableBakeObjectPath(Component);
			return false;
		}
		HashText(SourceInputHash, Name.ToString());
		HashValue(SourceInputHash, WeightOffset);
	}
	TArray<FName> GrassTypeNames;
	NamedTypes.GetKeys(GrassTypeNames);
	GrassTypeNames.Sort(FNameLexicalLess());
	HashValue(SourceInputHash, GrassTypeNames.Num());
	for (FName Name : GrassTypeNames)
	{
		ULandscapeGrassType* GrassType = NamedTypes.FindChecked(Name);
		if (!GrassType)
		{
			Error = TEXT("Grass producer has an unresolved named type: ") + Name.ToString();
			return false;
		}
		HashText(SourceInputHash, Name.ToString());
		HashText(SourceInputHash, IMStableBakeObjectPath(GrassType));
		const uint8 ScalingEnabled = GrassType->bEnableDensityScaling;
		HashValue(SourceInputHash, ScalingEnabled);
		HashValue(SourceInputHash, GrassType->GrassVarieties.Num());
		for (const FGrassVariety& Variety : GrassType->GrassVarieties)
		{
			HashVariety(SourceInputHash, Variety);
			if (ShouldSkipGrassVariety(Variety)) continue;
			// A stripped all-zero named weight has no generated instances.
			if (!GrassData.WeightOffsets.Contains(Name)) continue;
			const float Density = Variety.GetDensity() * (GrassType->bEnableDensityScaling ? DensityScale : 1.0f);
			if (Density <= 0.0f) continue;
			double VarietyMeshRadiusCm;
			if (!ReadVarietyBounds(Variety, VarietyMeshRadiusCm, Error)) return false;
			Source.MeshRadiusCm = FMath::Max(Source.MeshRadiusCm, VarietyMeshRadiusCm);
			Source.CreationRadiusCm = FMath::Min(Source.CreationRadiusCm,
				GuardBand * CullScale * Variety.GetEndCullDistance());
			if (!AppendSubsectionCameras(Component, DrawScale, Density, MaxInstancesPerComponent,
				Source.GenerationCameras, Error)) return false;
		}
	}
	Source.SourceInputSha1 = FinishHash(SourceInputHash);
	if (GrassData.NumElements == 0 || GrassData.WeightOffsets.IsEmpty()) return true;
	const int64 ExpectedHeightSampleCount = int64(Component->ComponentSizeQuads + 1) * (Component->ComponentSizeQuads + 1);
	if (GrassData.NumElements != ExpectedHeightSampleCount || int64(GrassDataByteCount) < ExpectedHeightSampleCount * int64(sizeof(uint16)))
	{
		Error = TEXT("GrassData height dimensions are incomplete: ") + IMStableBakeObjectPath(Component);
		return false;
	}
	// UE's GetHeightData exposes only a mutable view. Read the same packed
	// prefix as const after validating its exact dimensions and byte range.
	const TConstArrayView<uint16> Heights(reinterpret_cast<const uint16*>(GrassData.HeightWeightData.GetData()), GrassData.NumElements);
	uint16 MinHeight = MAX_uint16, MaxHeight = 0;
	for (uint16 Height : Heights)
	{
		MinHeight = FMath::Min(MinHeight, Height);
		MaxHeight = FMath::Max(MaxHeight, Height);
	}
	Source.HeightSampleCount = Heights.Num();
	// This mirrors FGrassBuilderBase + SampleLandscapeAtLocationLocal. Root
	// scale affects positions, not mesh scale. Height extrema include encoded
	// Landscape Z WPO; grid jitter remains within the component XY tile.
	const FVector Start(DrawScale.X * (Section.X - Offset.X),
		DrawScale.Y * (Section.Y - Offset.Y), DrawScale.Z * LandscapeDataAccess::GetLocalHeight(MinHeight));
	const FVector End(Start.X + DrawScale.X * Component->ComponentSizeQuads,
		Start.Y + DrawScale.Y * Component->ComponentSizeQuads,
		DrawScale.Z * LandscapeDataAccess::GetLocalHeight(MaxHeight));
	Source.OriginWorldBounds = FBox(Start.ComponentMin(End), Start.ComponentMax(End))
		.TransformBy(NoScaleTransform);
	if (!Source.GenerationWorldBounds.IsValid || !Source.OriginWorldBounds.IsValid
		|| Source.GenerationWorldBounds.Min.ContainsNaN() || Source.GenerationWorldBounds.Max.ContainsNaN()
		|| Source.OriginWorldBounds.Min.ContainsNaN() || Source.OriginWorldBounds.Max.ContainsNaN()
		|| DrawScale.ContainsNaN() || DrawScale.GetAbs().GetMin() <= 0.0)
	{
		Error = TEXT("Grass origin/generation bounds are invalid: ") + IMStableBakeObjectPath(Component);
		return false;
	}
	return true;
}

}

bool IMAcousticGrassBakePlan::ReadVarietyBounds(const FGrassVariety& Variety, double& Radius, FString& Error)
{
	return IMAcousticGrassBakePlanPrivate::ReadVarietyBounds(Variety, Radius, Error);
}

bool IMBuildAcousticGrassBakePlan(UWorld* World, const FBox& Region,
	FIMAcousticGrassBakePlan& OutPlan, FString& OutError)
{
	using namespace IMAcousticGrassBakePlanPrivate;
	OutPlan = {};
	OutError.Reset();
	if (!IsInGameThread() || !World || !Region.IsValid
		|| (World->WorldType != EWorldType::Editor && !World->IsGameWorld()))
	{
		OutError = TEXT("Static Grass planning requires the game thread and an Editor or game world.");
		return false;
	}
	TArray<ULandscapeComponent*> Components;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		for (ULandscapeComponent* Component : It->LandscapeComponents)
			if (IsValid(Component) && !Component->GetNamedGrassTypes().IsEmpty()) Components.Add(Component);
	Components.Sort([](const ULandscapeComponent& A, const ULandscapeComponent& B)
		{ return IMStableBakeObjectPath(&A) < IMStableBakeObjectPath(&B); });
	if (Components.IsEmpty()) return true;
	double Enable, CreateBand, DiscardBand, CullScale, DensityScale;
	if (!ReadPositiveCVar(TEXT("grass.Enable"), Enable, OutError)
		|| !ReadPositiveCVar(TEXT("grass.GuardBandMultiplier"), CreateBand, OutError)
		|| !ReadPositiveCVar(TEXT("grass.GuardBandDiscardMultiplier"), DiscardBand, OutError)
		|| !ReadPositiveCVar(TEXT("grass.CullDistanceScale"), CullScale, OutError)
		|| !ReadPositiveCVar(TEXT("grass.densityScale"), DensityScale, OutError)) return false;
	const IConsoleVariable* MaxInstances = IConsoleManager::Get().FindConsoleVariable(TEXT("grass.MaxInstancesPerComponent"));
	if (!MaxInstances)
	{
		OutError = TEXT("Grass subdivision setting is unavailable.");
		return false;
	}
	const int32 MaxInstancesPerComponent = FMath::Max(1024, MaxInstances->GetInt());
	FSHA1 Input;
	HashText(Input, TEXT("static-grass-region-v1"));
	HashValue(Input, DensityScale);
	HashValue(Input, CullScale);
	HashValue(Input, MaxInstancesPerComponent);
	for (ULandscapeComponent* Component : Components)
	{
		bool XYDisjoint = false;
		if (!ClassifyXYDisjoint(Component, Region, XYDisjoint, OutError)) return false;
		if (XYDisjoint)
		{
			OutPlan.XYDisjointComponents.Add(Component);
			continue;
		}
		FIMAcousticGrassBakeSource Source;
		if (!ReadSource(Component, FMath::Min(CreateBand, DiscardBand), CullScale,
			float(DensityScale), MaxInstancesPerComponent, Source, OutError)) return false;
		if (!Source.OriginWorldBounds.IsValid || Source.MeshRadiusCm <= 0
			|| !Region.ExpandBy(Source.MeshRadiusCm).Intersect(Source.OriginWorldBounds)) continue;
		ALandscapeProxy* Proxy = Component->GetLandscapeProxy();
		if (!Proxy->ShouldGenerateGrass())
		{
			OutError = TEXT("A required Grass source cannot generate in the current world: ") + IMStableBakeObjectPath(Proxy);
			return false;
		}
		if (OutPlan.Cameras.Num() > MAX_int32 - Source.GenerationCameras.Num())
		{
			OutError = TEXT("Grass plan camera array is unaddressable; no reduced coverage was substituted.");
			return false;
		}
		OutPlan.Cameras.Append(Source.GenerationCameras);
		HashText(Input, Source.SourceInputSha1);
		OutPlan.Proxies.AddUnique(Proxy);
		OutPlan.Sources.Add(MoveTemp(Source));
	}
	OutPlan.SourceInputSha1 = FinishHash(Input);
	return true;
}
