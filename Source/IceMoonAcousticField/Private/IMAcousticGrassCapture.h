#pragma once

#include "CoreMinimal.h"

class UWorld;
class ULandscapeComponent;
class UInstancedStaticMeshComponent;

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
struct FIMAcousticGrassDataObservation
{
	uint64 DataAddress = 0;
	int32 Elements = -2;
	int32 Bytes = -1;
	uint32 GenerationHash = 0;
};
#endif

// Owned by one bake volume on GT. No actor, component or render proxy is cached
// across captures. Weak sources are used only to retire a previous region's cache.
struct FIMAcousticGrassGenerationState
{
	bool bPrepared = false;
	FString InputSha1;
	TArray<TWeakObjectPtr<ULandscapeComponent>> Sources;
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	// Opt-in diagnostic metadata only; IDs do not retain source UObjects/data.
	TMap<uint32, FIMAcousticGrassDataObservation> DiagnosticData;
#endif
};

struct FIMAcousticGrassCapturedInstance
{
	FTransform WorldTransform;
	// Keep the original renderer index for the existing body/PM lookup. Sorted
	// output order is a separate identity ordinal, never a new body index.
	int32 RenderIndex = INDEX_NONE;
};

struct FIMAcousticGrassCapturedComponent
{
	FString ProducerIdentity;
	int32 RenderInstanceCount = 0;
	TArray<FIMAcousticGrassCapturedInstance> RegionInstances;
};

struct FIMAcousticGrassCapture
{
	FString InputSha1;
	TMap<UInstancedStaticMeshComponent*, FIMAcousticGrassCapturedComponent> Components;
	int32 SelectedSources = 0;
	int32 Cameras = 0;
	int32 ReadComponents = 0;
	int64 ReadInstances = 0;
	int64 RegionInstances = 0;
	double PlanningMilliseconds = 0.0;
	double GenerationMilliseconds = 0.0;
	double SelectionMilliseconds = 0.0;
	double ReadMilliseconds = 0.0;
	double FilterMilliseconds = 0.0;
	double VerificationMilliseconds = 0.0;
};

// Finite static region preparation, full synchronized transform read, then
// exact mesh-bounds region filtering. Never changes density/quality or assets.
bool IMCaptureAcousticGrassRegion(UWorld* World, const FBox& Region,
	FIMAcousticGrassGenerationState& State, FIMAcousticGrassCapture& Out,
	FString& Error);

// Opt-in GT commit diagnostic. Reads only already-selected sources' fields:
// no lazy grass getters, hash recomputation, rendering flush or regeneration.
void IMTraceAcousticGrassSourceState(UWorld* World,
	const FIMAcousticGrassGenerationState& State, const TCHAR* Stage);
