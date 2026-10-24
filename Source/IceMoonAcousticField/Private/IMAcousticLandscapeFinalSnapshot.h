#pragma once

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

class FIMAcousticLandscapeFinalSnapshot final
{
public:
	bool Capture(UWorld* World, const FString& OwnedPluginPackageName, FString& OutError);
	// Rebind a proven pristine duplicate's bytes to the same task-owned Landscape.
	// Explicit repair restores the proven reference's allocation addresses while
	// retaining each target layer's PM binding. Texture/layout/usage preflight is
	// complete before this task-owned metadata write; ordinary retarget is read-only.
	bool RetargetMatchingOwnedWorld(UWorld* World, const FString& OwnedPluginPackageName, FString& OutError,
		bool bRestoreReferenceAllocationLayout = false);
	bool Restore(FString& OutError);
	bool ValidateCurrentLayout(FString& OutError) const;
	void Reset();

	bool IsCaptured() const { return bCaptured; }
	bool WasRestoreAttempted() const { return bRestoreAttempted; }
	bool WasRestoreComplete() const { return bRestoreComplete; }
	bool WasRestorePartial() const { return bRestorePartial; }

	int32 GetLandscapeComponentCount() const { return LandscapeComponentCount; }
	int32 GetCollisionComponentCount() const { return CollisionSnapshots.Num(); }
	int32 GetUniqueTextureCount() const { return TextureSnapshots.Num(); }
	int32 GetTextureMipCount() const { return TextureMipCount; }
	int64 GetTextureSourceBytes() const { return TextureSourceByteCount; }
	int64 GetCollisionHeightBytes() const { return CollisionHeightByteCount; }

	int32 GetRestoredTextureCount() const { return RestoredTextureCount; }
	int32 GetRestoredMipCount() const { return RestoredMipCount; }
	int32 GetRestoredCollisionCount() const { return RestoredCollisionCount; }
	int32 GetCollisionRecreateCallCount() const { return CollisionRecreateCallCount; }
	int64 GetRestoredTextureBytes() const { return RestoredTextureByteCount; }
	int64 GetRestoredCollisionBytes() const { return RestoredCollisionByteCount; }

	FString GetCaptureSummary() const;
	FString GetRestoreSummary() const;

private:
	struct FIMTextureMipSnapshot
	{
		int32 BlockIndex = INDEX_NONE;
		int32 LayerIndex = INDEX_NONE;
		int32 MipIndex = INDEX_NONE;
		int32 SizeX = 0;
		int32 SizeY = 0;
		int32 NumSlices = 0;
		int32 BytesPerPixel = 0;
		int32 SourceFormat = INDEX_NONE;
		TArray64<uint8> RawBytes;
	};

	struct FIMTextureSnapshot
	{
		TStrongObjectPtr<UTexture2D> Texture;
		int32 BlockCount = 0;
		int32 LayerCount = 0;
		int32 MipCount = 0;
		int64 ByteCount = 0;
		TArray<FIMTextureMipSnapshot> Mips;
	};

	struct FIMLayerAllocationSnapshot
	{
		FName LayerName;
		uint8 TextureIndex = 255;
		uint8 Channel = 255;
	};

	struct FIMComponentSnapshot
	{
		TStrongObjectPtr<ULandscapeComponent> Component;
		TStrongObjectPtr<UTexture2D> FinalHeightmap;
		TArray<UTexture2D*> FinalWeightmaps;
		// Immutable identities; do not derive them from a mutable component later.
		TArray<FIMLayerAllocationSnapshot> FinalLayerAllocations;
		TStrongObjectPtr<ULandscapeHeightfieldCollisionComponent> Collision;
	};

	struct FIMCollisionSnapshot
	{
		TStrongObjectPtr<ULandscapeComponent> RenderComponent;
		TStrongObjectPtr<ULandscapeHeightfieldCollisionComponent> Collision;
		int32 CollisionSizeQuads = 0;
		int32 SimpleCollisionSizeQuads = 0;
		int64 ElementCount = 0;
		TArray<uint16> HeightSamples;
	};

	bool CaptureTexture(UTexture2D* Texture, UPackage* OwnedPackage, FString& OutError);
	bool ValidateRestorePreconditions(FString& OutError, bool bValidateLayerAddresses = true) const;
	bool BuildMatchingAllocationLayout(const FIMComponentSnapshot& Reference, ULandscapeComponent* Target,
		TArray<FWeightmapLayerAllocationInfo>& OutAllocations, FString& OutError) const;
	bool FailAfterWrite(const FString& Reason, FString& OutError);

	TStrongObjectPtr<UWorld> CapturedWorld;
	TStrongObjectPtr<UPackage> CapturedPackage;
	FString OwnedPluginPackageName;
	TArray<FIMComponentSnapshot> ComponentSnapshots;
	TArray<FIMTextureSnapshot> TextureSnapshots;
	TArray<FIMCollisionSnapshot> CollisionSnapshots;

	bool bCaptured = false;
	bool bRestoreAttempted = false;
	bool bRestoreComplete = false;
	bool bRestorePartial = false;
	int32 LandscapeComponentCount = 0;
	int32 TextureMipCount = 0;
	int64 TextureSourceByteCount = 0;
	int64 CollisionHeightByteCount = 0;
	int32 RestoredTextureCount = 0;
	int32 RestoredMipCount = 0;
	int32 RestoredCollisionCount = 0;
	int32 CollisionRecreateCallCount = 0;
	int32 AllocationLayoutsRestored = 0;
	int32 WeightmapUsageInitializations = 0;
	int32 CollisionLayerDataUpdates = 0;
	int64 RestoredTextureByteCount = 0;
	int64 RestoredCollisionByteCount = 0;
};

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA
