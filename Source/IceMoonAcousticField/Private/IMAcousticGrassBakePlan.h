#pragma once

#include "CoreMinimal.h"

class UWorld;
class ULandscapeComponent;
class ALandscapeProxy;
struct FGrassVariety;

// GT-owned one-capture data; no UObject pointers may leave the game thread.
struct FIMAcousticGrassBakeSource
{
	ULandscapeComponent* Component = nullptr;
	FBox OriginWorldBounds = FBox(ForceInit);
	FBox GenerationWorldBounds = FBox(ForceInit);
	double MeshRadiusCm = 0.0;
	double CreationRadiusCm = 0.0;
	int32 HeightSampleCount = 0;
	TArray<FVector> GenerationCameras;
	FString SourceInputSha1;
};

struct FIMAcousticGrassBakePlan
{
	TArray<FIMAcousticGrassBakeSource> Sources;
	// Geometrically proven irrelevant before GrassData freshness/read checks.
	// GT-only audit identities; these sources cannot contribute to this region.
	TArray<ULandscapeComponent*> XYDisjointComponents;
	TArray<ALandscapeProxy*> Proxies;
	TArray<FVector> Cameras;
	FString SourceInputSha1;
};

// Plans finite generation coverage for the inherited static LOD0 acoustic
// geometry. GrassData already includes the Landscape material's encoded Z WPO;
// ordinary heightmap bounds are used only for UE's generation admission boxes.
// Does not generate, flush, save, change CVars, or change quality settings.
// With exactly vertical local Z, builder XY + full mesh radius can prove a
// source irrelevant without depending on its transient GrassData height. A
// tilted Landscape conservatively keeps the original full-data validation.
namespace IMAcousticGrassBakePlan
{
// Test hook: forwards to the production per-variety radius reader without exposing the file-private namespace.
bool ReadVarietyBounds(const FGrassVariety& Variety, double& Radius, FString& Error);
}

bool IMBuildAcousticGrassBakePlan(UWorld* World, const FBox& Region,
	FIMAcousticGrassBakePlan& OutPlan, FString& OutError);
