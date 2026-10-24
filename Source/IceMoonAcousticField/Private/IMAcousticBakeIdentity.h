#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "UObject/Package.h"

inline FString IMStableBakeObjectPath(const UObject* Object)
{
	const FString Path = GetPathNameSafe(Object);
	const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
	if (Package && Package->HasAnyPackageFlags(PKG_PlayInEditor))
	{
		// PIE renames the package, not authored actor/component identities.
		const FString PackageName = Package->GetName();
		if (Path.StartsWith(PackageName + TEXT("."), ESearchCase::CaseSensitive))
			return UWorld::RemovePIEPrefix(PackageName) + Path.Mid(PackageName.Len());
	}
	return Path;
}
