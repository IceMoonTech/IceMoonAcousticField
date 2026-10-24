#pragma once

#include "CoreMinimal.h"

class UInstancedStaticMeshComponent;

struct FIMAcousticGrassWorldTransformSnapshot
{
	int32 CPUInstanceCount = 0;
	int32 RenderInstanceCount = 0;
	int32 BufferInstanceCount = 0;
	bool bSceneProxyPresent = false;
	bool bSceneBufferObserved = false;
	bool bGPUOnly = false;
	FVector PrimitiveWorldSpaceOffset = FVector::ZeroVector;
	TArray<FTransform> WorldTransforms;
};

// Exact scalar ordering, without formatting/quantization or allocations per
// comparison. Hash input must already be sorted using this order.
bool IMCompareAcousticGrassWorldTransforms(const FTransform& A, const FTransform& B);
FString IMHashSortedAcousticGrassWorldTransforms(TConstArrayView<FTransform> Transforms);

// Editor/PIE/Game GT static capture. Completes one batch RT copy before returning, with no UObject
// access on RT. Values are renderer world transforms in UE centimeters; their
// component transform and high world offset are already included exactly once.
bool IMReadLandscapeGrassWorldTransforms(
	TConstArrayView<UInstancedStaticMeshComponent*> Components,
	TArray<FIMAcousticGrassWorldTransformSnapshot>& OutSnapshots,
	FString& OutError);
