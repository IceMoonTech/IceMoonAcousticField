#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UMaterialInterface;
class UWorld;

bool IMPrepareLandscapeGrassMaterialCopies(
	UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
	TSharedPtr<FJsonObject>& OutSummary,
	FString& OutError);

bool IMValidateLandscapeGrassMaterialCopies(
	UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
	TSharedPtr<FJsonObject>& OutSummary,
	FString& OutError);

bool IMInspectLandscapeGrassMaterialSources(
	UWorld* World,
	TSharedPtr<FJsonObject>& OutSummary,
	FString& OutError);
