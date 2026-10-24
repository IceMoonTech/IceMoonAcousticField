#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UWorld;

bool IMPrepareRuralConstructionMaterialCopies(
	UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& Redirects,
	FString& Summary,
	FString& Error);
