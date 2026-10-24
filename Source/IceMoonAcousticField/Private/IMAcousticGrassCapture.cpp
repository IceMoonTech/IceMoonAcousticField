#include "IMAcousticGrassCapture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "IMAcousticBakeIdentity.h"
#include "IMAcousticGrassBakePlan.h"
#include "IMAcousticGrassWorldTransforms.h"
#include "LandscapeComponent.h"
#include "LandscapeGrassType.h"
#include "LandscapeProxy.h"
#include "Misc/Crc.h"

namespace IMAcousticGrassCapturePrivate
{
constexpr const TCHAR* GrassClass = TEXT("/Script/Foliage.GrassInstancedStaticMeshComponent");

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
TAutoConsoleVariable<int32> DataTrace(TEXT("IM.Acoustic.GrassCaptureTraceData"), 0,
	TEXT("Task diagnostic: log source GrassData metadata transitions around static capture; no raw samples."));
TAutoConsoleVariable<int32> CommitTrace(TEXT("IM.Acoustic.GrassCommitTraceData"), 0,
	TEXT("Task diagnostic: read selected source registration and GrassData fields at asset commit boundaries; no getters or generation."));
void ObserveData(UWorld* World, FIMAcousticGrassGenerationState& State, const TCHAR* Stage)
{
	if (DataTrace.GetValueOnGameThread() == 0 || !World) return;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		for (ULandscapeComponent* Component : It->LandscapeComponents)
		{
			if (!IsValid(Component)) continue;
			const auto& Data = *Component->GrassData;
			const uint64 Address = reinterpret_cast<UPTRINT>(&Data);
			auto& Previous = State.DiagnosticData.FindOrAdd(Component->GetUniqueID());
			if (Previous.DataAddress == Address && Previous.Elements == Data.NumElements
				&& Previous.Bytes == Data.HeightWeightData.Num() && Previous.GenerationHash == Data.GenerationHash) continue;
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticGrassDataTransition stage=%s world_id=%u gt_frame=%llu clock=%.9f component=%s previous_address=%llu address=%llu previous_elements=%d elements=%d previous_bytes=%d bytes=%d previous_hash=%u hash=%u named_types=%d registered=%d proxy_registered=%d should_generate=%d"),
				Stage, World->GetUniqueID(), GFrameCounter, FPlatformTime::Seconds(),
				*IMStableBakeObjectPath(Component), Previous.DataAddress, Address,
				Previous.Elements, Data.NumElements, Previous.Bytes, Data.HeightWeightData.Num(),
				Previous.GenerationHash, Data.GenerationHash, Component->GetNamedGrassTypes().Num(),
				Component->IsRegistered() ? 1 : 0, It->HasActorRegisteredAllComponents() ? 1 : 0,
				It->ShouldGenerateGrass() ? 1 : 0);
			Previous = {Address, Data.NumElements, Data.HeightWeightData.Num(), Data.GenerationHash};
		}
}
#else
void ObserveData(UWorld*, FIMAcousticGrassGenerationState&, const TCHAR*) {}
#endif

struct FIMProducer
{
	ALandscapeProxy* Proxy = nullptr;
	UStaticMesh* Mesh = nullptr;
	FString Identity;
};

void AddSourceForFlush(ULandscapeComponent* Component,
	TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>>& Groups)
{
	if (IsValid(Component))
		if (ALandscapeProxy* Proxy = Component->GetLandscapeProxy())
			Groups.FindOrAdd(Proxy).Add(Component);
}

bool PrepareGeneration(UWorld* World, const FIMAcousticGrassBakePlan& Plan,
	FIMAcousticGrassGenerationState& State, FString& Error)
{
	if (!State.bPrepared || State.InputSha1 != Plan.SourceInputSha1)
	{
		TMap<ALandscapeProxy*, TSet<ULandscapeComponent*>> Groups;
		if (!State.bPrepared)
		{
			// The first capture must not inherit an unknown camera/type cache.
			// Reset transient Grass once in this owned world; the finite plan
			// recreates its complete region and normal rendering resumes its views.
			for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
				for (ULandscapeComponent* Component : It->LandscapeComponents)
					AddSourceForFlush(Component, Groups);
		}
		else
		{
			for (const auto& Previous : State.Sources) AddSourceForFlush(Previous.Get(), Groups);
			for (const auto& Source : Plan.Sources) AddSourceForFlush(Source.Component, Groups);
		}
		for (auto& Group : Groups) Group.Key->FlushGrassComponents(&Group.Value, false);
	}
	ObserveData(World, State, TEXT("AfterTransientFlush"));
	for (ALandscapeProxy* Proxy : Plan.Proxies)
	{
		// UpdateGrass returns void and silently skips a proxy that cannot
		// generate. Recheck after transient component retirement, not just in
		// the pure plan, so a pending registration never certifies empty output.
		if (!IsValid(Proxy) || !Proxy->ShouldGenerateGrass())
		{
			Error = TEXT("Required Grass proxy cannot generate after transient retirement; retry after registration.");
			return false;
		}
		Proxy->UpdateGrass(Plan.Cameras, true);
		if (!IsValid(Proxy) || !Proxy->ShouldGenerateGrass())
		{
			Error = TEXT("Required Grass proxy lost generation readiness during static preparation.");
			return false;
		}
	}
	return true;
}

void BuildProducerCandidates(const FIMAcousticGrassBakePlan& Plan,
	const TSet<int32>& ObservedSeeds, TMap<int32, TArray<FIMProducer>>& Candidates)
{
	// UE clamps the builder subsection domain to 16. Match only observed seeds;
	// include proxy ownership so repeated Landscapes with the same section/type
	// seed do not become the same producer. A real collision still fails closed.
	for (const auto& Source : Plan.Sources)
	{
		ULandscapeComponent* Component = Source.Component;
		const FIntPoint Section = Component->GetSectionBase();
		const auto& Types = Component->GetNamedGrassTypes();
		TArray<FName> Names;
		Types.GetKeys(Names);
		Names.Sort(FNameLexicalLess());
		for (FName Name : Names)
		{
			ULandscapeGrassType* Type = Types.FindChecked(Name);
			for (int32 Variety = 0; Variety < Type->GrassVarieties.Num(); ++Variety)
				for (int32 X = 0; X < 16; ++X)
					for (int32 Y = 0; Y < 16; ++Y)
					{
						const FString SeedText = FString::Printf(TEXT("%s_%s_%d_%d_%d_%d_%d"),
							*Name.ToString().ToLower(), *Type->GetName().ToLower(),
							Section.X, Section.Y, X, Y, Variety);
						int32 Seed = FCrc::StrCrc32(StringCast<ANSICHAR>(*SeedText).Get());
						if (Seed == 0) ++Seed;
						if (!ObservedSeeds.Contains(Seed)) continue;
						FIMProducer& Producer = Candidates.FindOrAdd(Seed).AddDefaulted_GetRef();
						Producer.Proxy = Component->GetLandscapeProxy();
						Producer.Mesh = Type->GrassVarieties[Variety].GrassMesh;
						Producer.Identity = IMStableBakeObjectPath(Component) + TEXT("|Grass|")
							+ Name.ToString() + TEXT("|") + IMStableBakeObjectPath(Type)
							+ FString::Printf(TEXT("|%d|%d|%d|%d"), Variety, X, Y, Seed);
					}
		}
	}
}

bool SelectComponents(UWorld* World, const FBox& Region, const FIMAcousticGrassBakePlan& Plan,
	TArray<UInstancedStaticMeshComponent*>& Selected, TArray<FString>& Identities, FString& Error)
{
	TArray<UInstancedStaticMeshComponent*> Generated;
	TSet<int32> Seeds;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		TArray<UInstancedStaticMeshComponent*> Components;
		It->GetComponents(Components);
		for (UInstancedStaticMeshComponent* Component : Components)
		{
			if (!IsValid(Component) || Component->GetClass()->GetPathName() != GrassClass) continue;
			if (!Component->IsRegistered() || Component->GetNumRenderInstances() <= 0) continue;
			Generated.Add(Component);
			Seeds.Add(Component->InstancingRandomSeed);
		}
	}
	TMap<int32, TArray<FIMProducer>> Candidates;
	BuildProducerCandidates(Plan, Seeds, Candidates);
	TSet<FString> UsedIdentities;
	for (UInstancedStaticMeshComponent* Component : Generated)
	{
		const FIMProducer* Match = nullptr;
		if (const auto* Choices = Candidates.Find(Component->InstancingRandomSeed))
			for (const FIMProducer& Choice : *Choices)
				if (Choice.Proxy == Component->GetOwner())
				{
					if (Match)
					{
						Error = TEXT("Grass producer seed is ambiguous in its owning proxy: ") + Component->GetPathName();
						return false;
					}
					Match = &Choice;
				}
		if (!Match)
		{
			// Selected producers are read regardless of cached component bounds.
			// An unresolved producer touching the region is an error, not zero
			// geometry. Unrelated normal view caches need no acoustic export.
			if (Region.Intersect(Component->Bounds.GetBox()) || Component->Bounds.BoxExtent.IsNearlyZero())
			{
				Error = TEXT("An intersecting/pending Grass component has no current region producer: ")
					+ Component->GetPathName();
				return false;
			}
			continue;
		}
		if (Match->Mesh != Component->GetStaticMesh() || UsedIdentities.Contains(Match->Identity))
		{
			Error = TEXT("Grass producer mesh or unique component identity is inconsistent: ") + Component->GetPathName();
			return false;
		}
		UsedIdentities.Add(Match->Identity);
		Selected.Add(Component);
		Identities.Add(Match->Identity);
	}
	return true;
}
}

void IMTraceAcousticGrassSourceState(UWorld* World,
	const FIMAcousticGrassGenerationState& State, const TCHAR* Stage)
{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	check(IsInGameThread());
	if (IMAcousticGrassCapturePrivate::CommitTrace.GetValueOnGameThread() == 0 || !World) return;
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticGrassCommitBoundary stage=%s world_id=%u gt_frame=%llu clock=%.9f selected_sources=%d"),
		Stage, World->GetUniqueID(), GFrameCounter, FPlatformTime::Seconds(), State.Sources.Num());
	for (const auto& WeakSource : State.Sources)
	{
		ULandscapeComponent* Component = WeakSource.Get();
		if (!IsValid(Component))
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticGrassCommitSource stage=%s valid=0"), Stage);
			continue;
		}
		ALandscapeProxy* Proxy = Component->GetLandscapeProxy();
		const auto& Data = *Component->GrassData;
		// These are direct fields/pure status queries. In particular, avoid
		// GetNamedGrassTypes/ComputeGrassMapGenerationHash in this observer.
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticGrassCommitSource stage=%s world_id=%u gt_frame=%llu clock=%.9f component=%s valid=1 registered=%d proxy_valid=%d proxy_registered=%d root_present=%d root_registered=%d address=%llu elements=%d bytes=%d weight_offsets=%d hash=%u"),
			Stage, World->GetUniqueID(), GFrameCounter, FPlatformTime::Seconds(),
			*IMStableBakeObjectPath(Component), Component->IsRegistered() ? 1 : 0,
			IsValid(Proxy) ? 1 : 0, Proxy && Proxy->HasActorRegisteredAllComponents() ? 1 : 0,
			Proxy && Proxy->GetRootComponent() ? 1 : 0,
			Proxy && Proxy->GetRootComponent() && Proxy->GetRootComponent()->IsRegistered() ? 1 : 0,
			uint64(reinterpret_cast<UPTRINT>(&Data)), Data.NumElements,
			Data.HeightWeightData.Num(), Data.WeightOffsets.Num(), Data.GenerationHash);
	}
#endif
}

bool IMCaptureAcousticGrassRegion(UWorld* World, const FBox& Region,
	FIMAcousticGrassGenerationState& State, FIMAcousticGrassCapture& Out, FString& Error)
{
	using namespace IMAcousticGrassCapturePrivate;
	Out = {};
	ObserveData(World, State, TEXT("BeforePlan"));
	const double Started = FPlatformTime::Seconds();
	FIMAcousticGrassBakePlan Plan;
	if (!IMBuildAcousticGrassBakePlan(World, Region, Plan, Error)) return false;
	ObserveData(World, State, TEXT("AfterPlan"));
	const double Planned = FPlatformTime::Seconds();
	Out.InputSha1 = Plan.SourceInputSha1;
	Out.SelectedSources = Plan.Sources.Num();
	Out.Cameras = Plan.Cameras.Num();
	Out.PlanningMilliseconds = (Planned - Started) * 1000.0;
	if (!PrepareGeneration(World, Plan, State, Error)) return false;
	ObserveData(World, State, TEXT("AfterGeneration"));
	const double Generated = FPlatformTime::Seconds();
	Out.GenerationMilliseconds = (Generated - Planned) * 1000.0;
	TArray<UInstancedStaticMeshComponent*> Components;
	TArray<FString> Identities;
	if (!SelectComponents(World, Region, Plan, Components, Identities, Error)) return false;
	TArray<FIMAcousticGrassWorldTransformSnapshot> Snapshots;
	const double BeforeRead = FPlatformTime::Seconds();
	Out.SelectionMilliseconds = (BeforeRead - Generated) * 1000.0;
	if (!IMReadLandscapeGrassWorldTransforms(Components, Snapshots, Error)) return false;
	ObserveData(World, State, TEXT("AfterTransformRead"));
	if (Snapshots.Num() != Components.Num())
	{
		Error = TEXT("Grass static capture returned an incomplete synchronized component batch.");
		return false;
	}
	const double Read = FPlatformTime::Seconds();
	Out.ReadMilliseconds = (Read - BeforeRead) * 1000.0;
	Out.ReadComponents = Components.Num();
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		UInstancedStaticMeshComponent* Component = Components[Index];
		const auto& Snapshot = Snapshots[Index];
		Out.ReadInstances += Snapshot.RenderInstanceCount;
		FIMAcousticGrassCapturedComponent Capture;
		Capture.ProducerIdentity = Identities[Index];
		Capture.RenderInstanceCount = Snapshot.RenderInstanceCount;
		const FBoxSphereBounds MeshBounds = Component->GetStaticMesh()->GetBounds();
		for (int32 Instance = 0; Instance < Snapshot.WorldTransforms.Num(); ++Instance)
		{
			const FTransform& Transform = Snapshot.WorldTransforms[Instance];
			if (!MeshBounds.TransformBy(Transform).GetBox().Intersect(Region)) continue;
			Capture.RegionInstances.Add({Transform, Instance});
		}
		if (Capture.RegionInstances.IsEmpty()) continue;
		Capture.RegionInstances.Sort([](const auto& A, const auto& B)
		{
			if (IMCompareAcousticGrassWorldTransforms(A.WorldTransform, B.WorldTransform)) return true;
			if (IMCompareAcousticGrassWorldTransforms(B.WorldTransform, A.WorldTransform)) return false;
			return A.RenderIndex < B.RenderIndex;
		});
		Out.RegionInstances += Capture.RegionInstances.Num();
		Out.Components.Add(Component, MoveTemp(Capture));
	}
	const double Filtered = FPlatformTime::Seconds();
	Out.FilterMilliseconds = (Filtered - Read) * 1000.0;
	FIMAcousticGrassBakePlan After;
	if (!IMBuildAcousticGrassBakePlan(World, Region, After, Error)) return false;
	ObserveData(World, State, TEXT("AfterVerification"));
	if (After.SourceInputSha1 != Plan.SourceInputSha1)
	{
		Error = TEXT("Grass source inputs changed during static generation/read; retry without certifying partial geometry.");
		return false;
	}
	State.bPrepared = true;
	State.InputSha1 = Plan.SourceInputSha1;
	State.Sources.Reset();
	for (const auto& Source : Plan.Sources) State.Sources.Add(Source.Component);
	Out.VerificationMilliseconds = (FPlatformTime::Seconds() - Filtered) * 1000.0;
	return true;
}
