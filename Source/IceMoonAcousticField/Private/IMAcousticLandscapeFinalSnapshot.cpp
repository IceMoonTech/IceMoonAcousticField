#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

#include "IMAcousticLandscapeFinalSnapshot.h"

#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ImageCore.h"
#include "Misc/PackageName.h"
#include "LandscapeLayerInfoObject.h"
#include "Landscape.h"
#include "LandscapeEditLayer.h"
#include "LandscapeProxy.h"

namespace IMAcousticLandscapeFinalSnapshotPrivate
{
constexpr int64 MaxTextureMipEntries = 4096;
constexpr int64 MaxTextureBytesPerTexture = 128LL * 1024LL * 1024LL;
constexpr int64 MaxSnapshotBytes = 1024LL * 1024LL * 1024LL;
constexpr int64 MaxCollisionSamplesPerComponent = 16LL * 1024LL * 1024LL;
constexpr int32 MaxLandscapeComponents = 16384;
constexpr int32 MaxUniqueTextures = 4096;

bool IsOwnedObject(const UObject* Object, const UPackage* OwnedPackage)
{
	return IsValid(Object) && OwnedPackage != nullptr && Object->GetOutermost() == OwnedPackage;
}

// InitializeProxyLayersWeightmapUsage checks these invariants internally. Check
// the complete final+edit-layer ownership graph first, before any metadata write.
bool PreflightWeightmapUsage(
	UWorld* World, UPackage* OwnedPackage,
	const TMap<ULandscapeComponent*, TArray<FWeightmapLayerAllocationInfo>>& PlannedFinalAllocations,
	TArray<ALandscapeProxy*>& OutProxies, FString& OutError,
	const bool bInspectInitializedUsage = false)
{
	OutProxies.Reset();
	TSet<ULandscapeComponent*> RegisteredComponents;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Proxy = *It;
		ALandscape* Landscape = Proxy->GetLandscapeActor();
		if (!IsOwnedObject(Proxy, OwnedPackage) || !IsOwnedObject(Landscape, OwnedPackage))
		{
			OutError = TEXT("Weightmap usage requires a valid task-owned Landscape and proxy.");
			return false;
		}
		TArray<FGuid> LayerGuids{ FGuid() };
		for (const ULandscapeEditLayerBase* Layer : Landscape->GetEditLayersConst())
		{
			if (!IsValid(Layer) || !Layer->GetGuid().IsValid() || LayerGuids.Contains(Layer->GetGuid()))
			{
				OutError = TEXT("Edit-layer identity is invalid or duplicated during usage preflight.");
				return false;
			}
			LayerGuids.Add(Layer->GetGuid());
		}
		TMap<UTexture2D*, FGuid> TextureLayerGuids;
		TMap<TPair<UTexture2D*, uint8>, ULandscapeComponent*> ChannelOwners;
		for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
		{
			if (!IsOwnedObject(Component, OwnedPackage) || Component->GetLandscapeProxy() != Proxy
				|| Component->GetLandscapeActor() != Landscape || !PlannedFinalAllocations.Contains(Component)
				|| RegisteredComponents.Contains(Component))
			{
				OutError = TEXT("Usage preflight encountered an unplanned or foreign Landscape component.");
				return false;
			}
			RegisteredComponents.Add(Component);
			const ULandscapeComponent* ReadOnly = Component;
			for (const FGuid& Guid : LayerGuids)
			{
				if (Guid.IsValid())
				{
					const FLandscapeLayerComponentData* Data = ReadOnly->GetLayerData(Guid);
					if (!Data || !Data->IsInitialized()) continue;
				}
				const auto& Textures = ReadOnly->GetWeightmapTextures(Guid);
				const auto& Usages = ReadOnly->GetWeightmapTexturesUsage(Guid);
				if (bInspectInitializedUsage && Usages.Num() != Textures.Num())
				{
					OutError = TEXT("Final/edit-layer usage count differs from the texture array.");
					return false;
				}
				for (UTexture2D* Texture : Textures)
				{
					if (!IsOwnedObject(Texture, OwnedPackage)
						|| (TextureLayerGuids.Contains(Texture) && TextureLayerGuids.FindChecked(Texture) != Guid))
					{
						OutError = TEXT("Weightmap texture is foreign, invalid, or shared between edit layers.");
						return false;
					}
					TextureLayerGuids.Add(Texture, Guid);
				}
				const auto& Allocations = Guid.IsValid() ? ReadOnly->GetWeightmapLayerAllocations(Guid)
					: PlannedFinalAllocations.FindChecked(Component);
				TSet<FName> Names;
				for (const FWeightmapLayerAllocationInfo& Allocation : Allocations)
				{
					if (!Allocation.LayerInfo || !Landscape->HasTargetLayer(Allocation.LayerInfo)
						|| Names.Contains(Allocation.LayerInfo->GetLayerName()))
					{
						OutError = TEXT("Weightmap allocation has a null, duplicate, or unregistered parent target-layer identity.");
						return false;
					}
					Names.Add(Allocation.LayerInfo->GetLayerName());
					if (!Allocation.IsAllocated())
					{
						if (Allocation.WeightmapTextureIndex != 255 || Allocation.WeightmapTextureChannel != 255)
						{
							OutError = TEXT("Weightmap allocation is only partially unallocated.");
							return false;
						}
						continue;
					}
					if (!Textures.IsValidIndex(Allocation.WeightmapTextureIndex) || Allocation.WeightmapTextureChannel >= 4)
					{
						OutError = TEXT("Weightmap allocation texture/channel is out of range.");
						return false;
					}
					const TPair<UTexture2D*, uint8> Key(Textures[Allocation.WeightmapTextureIndex], Allocation.WeightmapTextureChannel);
					if (ChannelOwners.Contains(Key))
					{
						OutError = TEXT("Planned weightmap channel has multiple allocation owners.");
						return false;
					}
					ChannelOwners.Add(Key, Component);
					if (bInspectInitializedUsage)
					{
						ULandscapeWeightmapUsage* Usage = Usages[Allocation.WeightmapTextureIndex];
						const auto* InMap = Proxy->WeightmapUsageMap.Find(Key.Key);
						if (!IsValid(Usage) || !InMap || InMap->Get() != Usage || Usage->LayerGuid != Guid
							|| Usage->ChannelUsage[Allocation.WeightmapTextureChannel] != Component)
						{
							OutError = TEXT("Final/edit-layer initialized usage has a different GUID or channel owner.");
							return false;
						}
					}
				}
			}
		}
		OutProxies.Add(Proxy);
	}
	if (OutProxies.IsEmpty() || RegisteredComponents.Num() != PlannedFinalAllocations.Num())
	{
		OutError = TEXT("Landscape proxy registry does not cover every planned component exactly once.");
		return false;
	}
	return true;
}

bool CheckedMultiply(const int64 A, const int64 B, int64& OutProduct)
{
	if (A < 0 || B < 0 || (A != 0 && B > MAX_int64 / A))
	{
		return false;
	}

	OutProduct = A * B;
	return true;
}

bool CollectLandscapeComponents(
	UWorld* World,
	UPackage* OwnedPackage,
	TArray<ULandscapeComponent*>& OutComponents,
	FString& OutError)
{
	OutComponents.Reset();
	OutError.Reset();
	if (!IsValid(World) || !IsValid(OwnedPackage))
	{
		OutError = TEXT("Snapshot world or owned package is invalid.");
		return false;
	}

	TSet<ULandscapeComponent*> SeenComponents;
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!IsValid(Actor))
		{
			OutError = TEXT("World iteration returned an invalid actor.");
			return false;
		}

		TArray<ULandscapeComponent*> ActorComponents;
		Actor->GetComponents(ActorComponents);
		if (ActorComponents.IsEmpty())
		{
			continue;
		}

		if (!IsOwnedObject(Actor, OwnedPackage))
		{
			OutError = FString::Printf(TEXT("Landscape owner actor %s is not owned by the requested duplicate map package."), *Actor->GetPathName());
			return false;
		}

		for (ULandscapeComponent* Component : ActorComponents)
		{
			if (!IsValid(Component) || Component->GetWorld() != World || !IsOwnedObject(Component, OwnedPackage))
			{
				OutError = FString::Printf(TEXT("Landscape component is invalid, belongs to another world, or is outside the duplicate map package (owner=%s)."), *Actor->GetPathName());
				return false;
			}

			if (!SeenComponents.Contains(Component))
			{
				SeenComponents.Add(Component);
				OutComponents.Add(Component);
				if (OutComponents.Num() > MaxLandscapeComponents)
				{
					OutError = TEXT("Landscape component count exceeds the bounded snapshot limit.");
					return false;
				}
			}
		}
	}

	return true;
}

bool GetMipByteCount(const FImageInfo& ImageInfo, int64& OutByteCount)
{
	OutByteCount = 0;
	if (!ImageInfo.IsImageInfoValid())
	{
		return false;
	}

	const int64 Width = ImageInfo.SizeX;
	const int64 Height = ImageInfo.SizeY;
	const int64 SliceCount = ImageInfo.NumSlices;
	const int64 BytesPerPixel = ImageInfo.GetBytesPerPixel();
	int64 PixelCount = 0;
	if (Width <= 0 || Height <= 0 || SliceCount <= 0 || BytesPerPixel <= 0
		|| !CheckedMultiply(Width, Height, PixelCount)
		|| !CheckedMultiply(PixelCount, SliceCount, PixelCount)
		|| !CheckedMultiply(PixelCount, BytesPerPixel, OutByteCount))
	{
		return false;
	}

	return OutByteCount > 0 && OutByteCount <= MaxTextureBytesPerTexture;
}

bool GetCollisionHeightElementCount(const ULandscapeHeightfieldCollisionComponent& Collision, int64& OutElementCount)
{
	OutElementCount = 0;
	if (Collision.CollisionSizeQuads < 1 || Collision.SimpleCollisionSizeQuads < 0)
	{
		return false;
	}

	const int64 CollisionVerts = int64(Collision.CollisionSizeQuads) + 1;
	const int64 SimpleVerts = Collision.SimpleCollisionSizeQuads > 0
		? int64(Collision.SimpleCollisionSizeQuads) + 1
		: 0;
	int64 CollisionSamples = 0;
	int64 SimpleSamples = 0;
	if (!CheckedMultiply(CollisionVerts, CollisionVerts, CollisionSamples)
		|| !CheckedMultiply(SimpleVerts, SimpleVerts, SimpleSamples)
		|| CollisionSamples > MaxCollisionSamplesPerComponent
		|| SimpleSamples > MaxCollisionSamplesPerComponent - CollisionSamples)
	{
		return false;
	}

	OutElementCount = CollisionSamples + SimpleSamples;
	return OutElementCount > 0 && OutElementCount <= MaxCollisionSamplesPerComponent;
}
} // namespace

using namespace IMAcousticLandscapeFinalSnapshotPrivate;

void FIMAcousticLandscapeFinalSnapshot::Reset()
{
	CapturedWorld.Reset();
	CapturedPackage.Reset();
	OwnedPluginPackageName.Reset();
	ComponentSnapshots.Reset();
	TextureSnapshots.Reset();
	CollisionSnapshots.Reset();

	bCaptured = false;
	bRestoreAttempted = false;
	bRestoreComplete = false;
	bRestorePartial = false;
	LandscapeComponentCount = 0;
	TextureMipCount = 0;
	TextureSourceByteCount = 0;
	CollisionHeightByteCount = 0;
	RestoredTextureCount = 0;
	RestoredMipCount = 0;
	RestoredCollisionCount = 0;
	CollisionRecreateCallCount = 0;
	AllocationLayoutsRestored = 0;
	WeightmapUsageInitializations = 0;
	CollisionLayerDataUpdates = 0;
	RestoredTextureByteCount = 0;
	RestoredCollisionByteCount = 0;
}

bool FIMAcousticLandscapeFinalSnapshot::CaptureTexture(UTexture2D* Texture, UPackage* OwnedPackage, FString& OutError)
{
	OutError.Reset();
	if (!IsValid(Texture) || !IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Texture, OwnedPackage))
	{
		OutError = TEXT("Final Landscape texture is invalid or is not owned by the duplicate map package.");
		return false;
	}

	FTextureSource& Source = Texture->Source;
	if (!Source.IsValid())
	{
		OutError = FString::Printf(TEXT("Final Landscape texture source is unavailable: %s."), *Texture->GetPathName());
		return false;
	}

	FIMTextureSnapshot Snapshot;
	Snapshot.Texture.Reset(Texture);
	Snapshot.BlockCount = Source.GetNumBlocks();
	Snapshot.LayerCount = Source.GetNumLayers();
	Snapshot.MipCount = Source.GetNumMips();
	if (Snapshot.BlockCount <= 0 || Snapshot.LayerCount <= 0 || Snapshot.MipCount <= 0)
	{
		OutError = FString::Printf(TEXT("Final Landscape texture has an empty source layout: %s."), *Texture->GetPathName());
		return false;
	}

	const int64 BlockLayerCount = int64(Snapshot.BlockCount) * Snapshot.LayerCount;
	if (BlockLayerCount <= 0 || BlockLayerCount > IMAcousticLandscapeFinalSnapshotPrivate::MaxTextureMipEntries
		|| Snapshot.MipCount > IMAcousticLandscapeFinalSnapshotPrivate::MaxTextureMipEntries / BlockLayerCount)
	{
		OutError = FString::Printf(TEXT("Final Landscape texture block/layer/mip layout exceeds the snapshot limit: %s."), *Texture->GetPathName());
		return false;
	}

	const int64 ExpectedMipEntries = BlockLayerCount * Snapshot.MipCount;
	Snapshot.Mips.Reserve(static_cast<int32>(ExpectedMipEntries));
	for (int32 BlockIndex = 0; BlockIndex < Snapshot.BlockCount; ++BlockIndex)
	{
		for (int32 LayerIndex = 0; LayerIndex < Snapshot.LayerCount; ++LayerIndex)
		{
			for (int32 MipIndex = 0; MipIndex < Snapshot.MipCount; ++MipIndex)
			{
				FImageInfo ImageInfo;
				int64 ExpectedByteCount = 0;
				if (!Source.GetMipImageInfo(ImageInfo, BlockIndex, LayerIndex, MipIndex)
					|| !GetMipByteCount(ImageInfo, ExpectedByteCount)
					|| ExpectedByteCount > IMAcousticLandscapeFinalSnapshotPrivate::MaxTextureBytesPerTexture - Snapshot.ByteCount)
				{
					OutError = FString::Printf(TEXT("Final Landscape texture mip metadata or bounded size is invalid: texture=%s block=%d layer=%d mip=%d."),
						*Texture->GetPathName(), BlockIndex, LayerIndex, MipIndex);
					return false;
				}

				if (TextureSourceByteCount > IMAcousticLandscapeFinalSnapshotPrivate::MaxSnapshotBytes - CollisionHeightByteCount - ExpectedByteCount)
				{
					OutError = TEXT("Combined Landscape texture and collision snapshot exceeds the bounded byte limit.");
					return false;
				}

				FIMTextureMipSnapshot Mip;
				Mip.BlockIndex = BlockIndex;
				Mip.LayerIndex = LayerIndex;
				Mip.MipIndex = MipIndex;
				Mip.SizeX = ImageInfo.SizeX;
				Mip.SizeY = ImageInfo.SizeY;
				Mip.NumSlices = ImageInfo.NumSlices;
				Mip.BytesPerPixel = ImageInfo.GetBytesPerPixel();
				Mip.SourceFormat = static_cast<int32>(Source.GetFormat(LayerIndex));
				if (!Source.GetMipData(Mip.RawBytes, BlockIndex, LayerIndex, MipIndex)
					|| Mip.RawBytes.Num() != ExpectedByteCount)
				{
					OutError = FString::Printf(TEXT("Final Landscape texture mip read was incomplete: texture=%s block=%d layer=%d mip=%d."),
						*Texture->GetPathName(), BlockIndex, LayerIndex, MipIndex);
					return false;
				}

				Snapshot.ByteCount += Mip.RawBytes.Num();
				Snapshot.Mips.Add(MoveTemp(Mip));
			}
		}
	}

	if (Snapshot.Mips.Num() != ExpectedMipEntries
		|| TextureSourceByteCount + Snapshot.ByteCount > IMAcousticLandscapeFinalSnapshotPrivate::MaxSnapshotBytes - CollisionHeightByteCount)
	{
		OutError = FString::Printf(TEXT("Final Landscape texture snapshot is incomplete or exceeds the total byte limit: %s."), *Texture->GetPathName());
		return false;
	}

	TextureMipCount += Snapshot.Mips.Num();
	TextureSourceByteCount += Snapshot.ByteCount;
	TextureSnapshots.Add(MoveTemp(Snapshot));
	return true;
}

bool FIMAcousticLandscapeFinalSnapshot::Capture(UWorld* World, const FString& InOwnedPluginPackageName, FString& OutError)
{
	Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Landscape final snapshots must be captured on the game thread.");
		return false;
	}
	if (!IsValid(World))
	{
		OutError = TEXT("Snapshot world is invalid.");
		return false;
	}
	if (!InOwnedPluginPackageName.StartsWith(TEXT("/IceMoonAcousticField/Tests/"), ESearchCase::CaseSensitive)
		|| !FPackageName::IsValidLongPackageName(InOwnedPluginPackageName))
	{
		OutError = TEXT("Owned package name must be a valid IceMoonAcousticField Tests long package name.");
		return false;
	}

	UPackage* WorldPackage = Cast<UPackage>(World->GetOutermost());
	if (!IsValid(WorldPackage)
		|| !WorldPackage->GetName().Equals(InOwnedPluginPackageName, ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(TEXT("World package does not match the declared owned package: expected=%s actual=%s."),
			*InOwnedPluginPackageName, WorldPackage ? *WorldPackage->GetName() : TEXT("<null>"));
		return false;
	}

	CapturedWorld.Reset(World);
	CapturedPackage.Reset(WorldPackage);
	OwnedPluginPackageName = InOwnedPluginPackageName;

	TArray<ULandscapeComponent*> LandscapeComponents;
	FString CollectionError;
	if (!CollectLandscapeComponents(World, WorldPackage, LandscapeComponents, CollectionError))
	{
		Reset();
		OutError = MoveTemp(CollectionError);
		return false;
	}
	if (LandscapeComponents.IsEmpty())
	{
		Reset();
		OutError = TEXT("Duplicate world contains no Landscape components to snapshot.");
		return false;
	}

	TMap<UTexture2D*, int32> TextureIndexByPointer;
	TextureIndexByPointer.Reserve(LandscapeComponents.Num() * 2);
	for (ULandscapeComponent* Component : LandscapeComponents)
	{
		const ULandscapeComponent* ReadOnlyComponent = Component;
		UTexture2D* Heightmap = ReadOnlyComponent->GetHeightmap(false);
		if (!IsValid(Heightmap))
		{
			Reset();
			OutError = FString::Printf(TEXT("Final Landscape heightmap is unavailable: %s."), *Component->GetPathName());
			return false;
		}

		if (!TextureIndexByPointer.Contains(Heightmap))
		{
			if (TextureSnapshots.Num() >= IMAcousticLandscapeFinalSnapshotPrivate::MaxUniqueTextures || !CaptureTexture(Heightmap, WorldPackage, OutError))
			{
				Reset();
				if (OutError.IsEmpty())
				{
					OutError = TEXT("Unique final Landscape texture count exceeds the snapshot limit.");
				}
				return false;
			}
			TextureIndexByPointer.Add(Heightmap, TextureSnapshots.Num() - 1);
		}

		const TArray<UTexture2D*>& Weightmaps = ReadOnlyComponent->GetWeightmapTextures(false);
		if (Weightmaps.Num() > IMAcousticLandscapeFinalSnapshotPrivate::MaxUniqueTextures)
		{
			Reset();
			OutError = FString::Printf(TEXT("Final Landscape weightmap count exceeds the snapshot limit: %s."), *Component->GetPathName());
			return false;
		}
		for (UTexture2D* Weightmap : Weightmaps)
		{
			if (!IsValid(Weightmap))
			{
				Reset();
				OutError = FString::Printf(TEXT("Final Landscape weightmap contains an invalid texture reference: %s."), *Component->GetPathName());
				return false;
			}

			if (!TextureIndexByPointer.Contains(Weightmap))
			{
				if (TextureSnapshots.Num() >= IMAcousticLandscapeFinalSnapshotPrivate::MaxUniqueTextures || !CaptureTexture(Weightmap, WorldPackage, OutError))
				{
					Reset();
					if (OutError.IsEmpty())
					{
						OutError = TEXT("Unique final Landscape texture count exceeds the snapshot limit.");
					}
					return false;
				}
				TextureIndexByPointer.Add(Weightmap, TextureSnapshots.Num() - 1);
			}
		}

		ULandscapeHeightfieldCollisionComponent* Collision = Component->GetCollisionComponent();
		if (!IsValid(Collision) || Collision->GetRenderComponent() != Component || !IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Collision, WorldPackage))
		{
			Reset();
			OutError = FString::Printf(TEXT("Landscape collision component is missing, mismatched, or shared outside the duplicate map package: %s."), *Component->GetPathName());
			return false;
		}
		if (Collision->CollisionHeightData.IsLocked() || Collision->CollisionHeightData.GetElementSize() != sizeof(uint16))
		{
			Reset();
			OutError = FString::Printf(TEXT("Landscape collision height bulk data is locked or has an unexpected element size: %s."), *Collision->GetPathName());
			return false;
		}

		int64 ExpectedCollisionElements = 0;
		if (!GetCollisionHeightElementCount(*Collision, ExpectedCollisionElements)
			|| Collision->CollisionHeightData.GetElementCount() != ExpectedCollisionElements
			|| ExpectedCollisionElements > MAX_int32)
		{
			Reset();
			OutError = FString::Printf(TEXT("Landscape collision height bulk element count/layout is invalid: collision=%s actual=%lld."),
				*Collision->GetPathName(), Collision->CollisionHeightData.GetElementCount());
			return false;
		}

		const int64 ExpectedCollisionBytes = ExpectedCollisionElements * sizeof(uint16);
		if (TextureSourceByteCount > IMAcousticLandscapeFinalSnapshotPrivate::MaxSnapshotBytes - CollisionHeightByteCount - ExpectedCollisionBytes)
		{
			Reset();
			OutError = TEXT("Combined Landscape texture and collision snapshot exceeds the bounded byte limit.");
			return false;
		}

		FIMCollisionSnapshot CollisionSnapshot;
		CollisionSnapshot.RenderComponent.Reset(Component);
		CollisionSnapshot.Collision.Reset(Collision);
		CollisionSnapshot.CollisionSizeQuads = Collision->CollisionSizeQuads;
		CollisionSnapshot.SimpleCollisionSizeQuads = Collision->SimpleCollisionSizeQuads;
		CollisionSnapshot.ElementCount = ExpectedCollisionElements;
		CollisionSnapshot.HeightSamples.SetNumUninitialized(static_cast<int32>(ExpectedCollisionElements));
		const void* CollisionData = Collision->CollisionHeightData.LockReadOnly();
		if (CollisionData == nullptr)
		{
			Reset();
			OutError = FString::Printf(TEXT("Landscape collision height bulk data could not be read: %s."), *Collision->GetPathName());
			return false;
		}
		FMemory::Memcpy(CollisionSnapshot.HeightSamples.GetData(), CollisionData, static_cast<SIZE_T>(ExpectedCollisionBytes));
		Collision->CollisionHeightData.Unlock();

		CollisionHeightByteCount += ExpectedCollisionBytes;
		CollisionSnapshots.Add(MoveTemp(CollisionSnapshot));

		FIMComponentSnapshot ComponentSnapshot;
		ComponentSnapshot.Component.Reset(Component);
		ComponentSnapshot.FinalHeightmap.Reset(Heightmap);
		ComponentSnapshot.FinalWeightmaps = Weightmaps;
		TSet<FName> LayerNames;
		for (const FWeightmapLayerAllocationInfo& Allocation : ReadOnlyComponent->GetWeightmapLayerAllocations(false))
		{
			if (!Allocation.LayerInfo || LayerNames.Contains(Allocation.LayerInfo->GetLayerName())
				|| (Allocation.IsAllocated() && (!Weightmaps.IsValidIndex(Allocation.WeightmapTextureIndex)
					|| Allocation.WeightmapTextureChannel >= 4)))
			{
				Reset();
				OutError = TEXT("Captured final layer identity/address is invalid or duplicated.");
				return false;
			}
			FIMLayerAllocationSnapshot Layer;
			Layer.LayerName = Allocation.LayerInfo->GetLayerName();
			Layer.TextureIndex = Allocation.WeightmapTextureIndex;
			Layer.Channel = Allocation.WeightmapTextureChannel;
			LayerNames.Add(Layer.LayerName);
			ComponentSnapshot.FinalLayerAllocations.Add(Layer);
		}
		ComponentSnapshot.Collision.Reset(Collision);
		ComponentSnapshots.Add(MoveTemp(ComponentSnapshot));
	}

	LandscapeComponentCount = LandscapeComponents.Num();
	if (CollisionSnapshots.Num() != LandscapeComponentCount)
	{
		Reset();
		OutError = TEXT("Landscape and collision component counts do not match.");
		return false;
	}

	bCaptured = true;
	return true;
}

bool FIMAcousticLandscapeFinalSnapshot::BuildMatchingAllocationLayout(
	const FIMComponentSnapshot& Reference, ULandscapeComponent* Target,
	TArray<FWeightmapLayerAllocationInfo>& OutAllocations, FString& OutError) const
{
	OutAllocations.Reset();
	const ULandscapeComponent* ReadOnlyTarget = Target;
	const auto& Current = ReadOnlyTarget->GetWeightmapLayerAllocations(false);
	if (Current.Num() != Reference.FinalLayerAllocations.Num())
	{
		OutError = TEXT("Reference and target final layer allocation counts differ: ") + Target->GetPathName();
		return false;
	}
	TMap<FName, int32> ByName;
	for (int32 Index = 0; Index < Current.Num(); ++Index)
	{
		if (!Current[Index].LayerInfo || ByName.Contains(Current[Index].LayerInfo->GetLayerName()))
		{
			OutError = TEXT("Target final layer identity is null or duplicated: ") + Target->GetPathName();
			return false;
		}
		ByName.Add(Current[Index].LayerInfo->GetLayerName(), Index);
	}
	for (int32 Index = 0; Index < Reference.FinalLayerAllocations.Num(); ++Index)
	{
		const FIMLayerAllocationSnapshot& Layer = Reference.FinalLayerAllocations[Index];
		const int32* TargetIndex = ByName.Find(Layer.LayerName);
		if (!TargetIndex)
		{
			OutError = TEXT("A captured final layer is absent in the target: ") + Layer.LayerName.ToString();
			return false;
		}
		FWeightmapLayerAllocationInfo Desired = Current[*TargetIndex];
		UE_LOG(LogTemp, Display, TEXT("[IM][TERRAIN_RETARGET_ALLOC] component=%s source_ordinal=%d source_layer=%s matched_target_ordinal=%d source_texture=%u source_channel=%u matched_target_texture=%u matched_target_channel=%u"),
			*Target->GetPathName(Target->GetWorld()), Index, *Layer.LayerName.ToString(), *TargetIndex,
			uint32(Layer.TextureIndex), uint32(Layer.Channel),
			uint32(Desired.WeightmapTextureIndex), uint32(Desired.WeightmapTextureChannel));
		Desired.WeightmapTextureIndex = Layer.TextureIndex;
		Desired.WeightmapTextureChannel = Layer.Channel;
		OutAllocations.Add(Desired);
	}
	return true;
}

bool FIMAcousticLandscapeFinalSnapshot::RetargetMatchingOwnedWorld(
	UWorld* World, const FString& TargetPackageName, FString& OutError,
	const bool bRestoreReferenceAllocationLayout)
{
	OutError.Reset();
	if (!IsInGameThread() || !bCaptured || bRestoreAttempted || !IsValid(World)
		|| !TargetPackageName.StartsWith(TEXT("/IceMoonAcousticField/Tests/"), ESearchCase::CaseSensitive)
		|| World->GetOutermost()->GetName() != TargetPackageName
		|| World == CapturedWorld.Get())
	{
		OutError = TEXT("Retarget requires a fresh captured reference and a different task-owned Tests world.");
		return false;
	}
	UPackage* TargetPackage = World->GetOutermost();
	TArray<ULandscapeComponent*> TargetComponents;
	if (!CollectLandscapeComponents(World, TargetPackage, TargetComponents, OutError)
		|| TargetComponents.Num() != ComponentSnapshots.Num())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Reference and target Landscape component counts differ.");
		return false;
	}
	TMap<FString, ULandscapeComponent*> ByRelativePath;
	for (ULandscapeComponent* Component : TargetComponents)
	{
		const FString Key = Component->GetPathName(World);
		if (ByRelativePath.Contains(Key))
		{
			OutError = TEXT("Target Landscape component identity is ambiguous: ") + Key;
			return false;
		}
		ByRelativePath.Add(Key, Component);
	}
	TMap<UTexture2D*, UTexture2D*> TextureRedirects;
	TSet<UTexture2D*> UsedTargets;
	TArray<ULandscapeComponent*> Matches;
	TMap<ULandscapeComponent*, TArray<FWeightmapLayerAllocationInfo>> PlannedAllocations;
	auto BindTexture = [&](UTexture2D* Reference, UTexture2D* Target) -> bool
	{
		if (!IsValid(Reference) || !IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Target, TargetPackage)
			|| Reference->GetFName() != Target->GetFName()) return false;
		if (UTexture2D** Bound = TextureRedirects.Find(Reference)) return *Bound == Target;
		if (UsedTargets.Contains(Target)) return false;
		TextureRedirects.Add(Reference, Target);
		UsedTargets.Add(Target);
		return true;
	};
	for (const FIMComponentSnapshot& Reference : ComponentSnapshots)
	{
		const ULandscapeComponent* Source = Reference.Component.Get();
		const FString Key = Source->GetPathName(CapturedWorld.Get());
		ULandscapeComponent* Target = ByRelativePath.FindRef(Key);
		if (!Target || !Source->GetComponentTransform().Equals(Target->GetComponentTransform(), 0.0)
			|| Source->GetSectionBase() != Target->GetSectionBase()
			|| Source->ComponentSizeQuads != Target->ComponentSizeQuads
			|| Source->NumSubsections != Target->NumSubsections
			|| Source->SubsectionSizeQuads != Target->SubsectionSizeQuads
			|| Source->HeightmapScaleBias != Target->HeightmapScaleBias
			|| Source->WeightmapScaleBias != Target->WeightmapScaleBias)
		{
			OutError = TEXT("Reference and target Landscape identity/topology/placement differ: ") + Key;
			return false;
		}
		const ULandscapeComponent* ReadOnlyTarget = Target;
		const auto& TargetAllocations = ReadOnlyTarget->GetWeightmapLayerAllocations(false);
		TArray<FWeightmapLayerAllocationInfo> DesiredAllocations;
		if (!BuildMatchingAllocationLayout(Reference, Target, DesiredAllocations, OutError)) return false;
		if (!bRestoreReferenceAllocationLayout)
		{
			for (const auto& Desired : DesiredAllocations)
			{
				const auto* Actual = TargetAllocations.FindByPredicate([&Desired](const auto& Allocation)
				{ return Allocation.LayerInfo == Desired.LayerInfo; });
				if (!Actual || Actual->WeightmapTextureIndex != Desired.WeightmapTextureIndex
					|| Actual->WeightmapTextureChannel != Desired.WeightmapTextureChannel)
				{
					OutError = TEXT("Reference and target named-layer texture/channel differ: ") + Key;
					return false;
				}
			}
		}
		PlannedAllocations.Add(Target, MoveTemp(DesiredAllocations));
		const TArray<UTexture2D*>& TargetWeights = ReadOnlyTarget->GetWeightmapTextures(false);
		if (TargetWeights.Num() != Reference.FinalWeightmaps.Num()
			|| !BindTexture(Reference.FinalHeightmap.Get(), ReadOnlyTarget->GetHeightmap(false)))
		{
			OutError = TEXT("Reference and target heightmap/weightmap identities differ: ") + Key;
			return false;
		}
		for (int32 Index = 0; Index < TargetWeights.Num(); ++Index)
		{
			if (!BindTexture(Reference.FinalWeightmaps[Index], TargetWeights[Index]))
			{
				OutError = TEXT("Reference and target weightmap identity/sharing differ: ") + Key;
				return false;
			}
		}
		ULandscapeHeightfieldCollisionComponent* Collision = Target->GetCollisionComponent();
		const ULandscapeHeightfieldCollisionComponent* SourceCollision = Reference.Collision.Get();
		const bool bOwnedCollision = IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Collision, TargetPackage);
		const bool bRenderMatches = IsValid(Collision) && Collision->GetRenderComponent() == Target;
		const bool bTransformMatches = IsValid(Collision)
			&& SourceCollision->GetComponentTransform().Equals(Collision->GetComponentTransform(), 0.0);
		const bool bShapeMatches = IsValid(Collision)
			&& Collision->CollisionSizeQuads == SourceCollision->CollisionSizeQuads
			&& Collision->SimpleCollisionSizeQuads == SourceCollision->SimpleCollisionSizeQuads;
		UE_LOG(LogTemp, Display, TEXT("[IM][TERRAIN_RETARGET_COLLISION] component=%s source_collision=%s target_collision=%s target_render=%s owned=%d render_matches=%d transform_matches=%d shape_matches=%d source_size=%d/%d target_size=%d/%d"),
			*Key, *GetPathNameSafe(SourceCollision), *GetPathNameSafe(Collision),
			*GetPathNameSafe(IsValid(Collision) ? Collision->GetRenderComponent() : nullptr),
			bOwnedCollision, bRenderMatches, bTransformMatches, bShapeMatches,
			SourceCollision->CollisionSizeQuads, SourceCollision->SimpleCollisionSizeQuads,
			IsValid(Collision) ? Collision->CollisionSizeQuads : -1,
			IsValid(Collision) ? Collision->SimpleCollisionSizeQuads : -1);
		// LandscapeEdit.cpp creates rebuilt collision components with NAME_None.
		// Their generated names are cache identities. Match the exact render
		// component, owner, placement and shape; full bulk layout is checked below.
		if (!bOwnedCollision || !bRenderMatches || !bTransformMatches || !bShapeMatches)
		{
			OutError = TEXT("Reference and target collision identity differ: ") + Key;
			return false;
		}
		Matches.Add(Target);
	}
	if (TextureRedirects.Num() != TextureSnapshots.Num())
	{
		OutError = TEXT("Not every captured texture has one exact target.");
		return false;
	}
	TArray<ALandscapeProxy*> UsageProxies;
	if (bRestoreReferenceAllocationLayout
		&& !IMAcousticLandscapeFinalSnapshotPrivate::PreflightWeightmapUsage(World, TargetPackage, PlannedAllocations, UsageProxies, OutError)) return false;
	// All object identities are replaced together. Raw bytes retain the proven
	// reference values; no UObject from that old world remains GC-rooted here.
	for (FIMTextureSnapshot& Texture : TextureSnapshots)
		Texture.Texture.Reset(TextureRedirects.FindChecked(Texture.Texture.Get()));
	for (int32 Index = 0; Index < Matches.Num(); ++Index)
	{
		ULandscapeComponent* Target = Matches[Index];
		const ULandscapeComponent* ReadOnlyTarget = Target;
		FIMComponentSnapshot& Component = ComponentSnapshots[Index];
		Component.Component.Reset(Target);
		Component.FinalHeightmap.Reset(ReadOnlyTarget->GetHeightmap(false));
		Component.FinalWeightmaps = ReadOnlyTarget->GetWeightmapTextures(false);
		Component.Collision.Reset(Target->GetCollisionComponent());
		FIMCollisionSnapshot& Collision = CollisionSnapshots[Index];
		Collision.RenderComponent.Reset(Target);
		Collision.Collision.Reset(Target->GetCollisionComponent());
	}
	CapturedWorld.Reset(World);
	CapturedPackage.Reset(TargetPackage);
	OwnedPluginPackageName = TargetPackageName;
	if (!ValidateRestorePreconditions(OutError, !bRestoreReferenceAllocationLayout))
	{
		// A failed layout gate must not leave an actionable partial retarget.
		const FString Failure = OutError;
		Reset();
		OutError = TEXT("No bytes written; retarget layout rejected: ") + Failure;
		return false;
	}
	if (bRestoreReferenceAllocationLayout)
	{
		// This repairs an already rejected task map. Keep its PM assets by
		// matching names; restore only the pinned reference's complete packing.
		// Every final+edit-layer channel claim was checked before this write.
		// An unexpected failure after this point leaves an explicitly dirty,
		// rejected task map. It cannot be mistaken for a read-only preflight.
		TargetPackage->MarkPackageDirty();
		for (ULandscapeComponent* Target : Matches)
		{
			Target->Modify();
			Target->GetWeightmapLayerAllocations(false) = PlannedAllocations.FindChecked(Target);
			++AllocationLayoutsRestored;
		}
		for (ALandscapeProxy* Proxy : UsageProxies)
		{
			Proxy->InitializeProxyLayersWeightmapUsage();
			++WeightmapUsageInitializations;
		}
		for (ULandscapeComponent* Target : Matches) Target->UpdateMaterialInstances();
		if (!ValidateRestorePreconditions(OutError))
		{
			OutError = TEXT("Task allocation metadata was repaired in memory, not saved; strict post-repair validation failed: ")
				+ OutError + TEXT(" ") + GetCaptureSummary();
			return false;
		}
	}
	return true;
}

bool FIMAcousticLandscapeFinalSnapshot::ValidateCurrentLayout(FString& OutError) const
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Landscape layout validation requires the game thread.");
		return false;
	}
	return ValidateRestorePreconditions(OutError);
}

bool FIMAcousticLandscapeFinalSnapshot::ValidateRestorePreconditions(FString& OutError, const bool bValidateLayerAddresses) const
{
	OutError.Reset();
	UWorld* World = CapturedWorld.Get();
	UPackage* OwnedPackage = CapturedPackage.Get();
	if (!bCaptured || !IsValid(World) || !IsValid(OwnedPackage)
		|| World->GetOutermost() != OwnedPackage
		|| !OwnedPackage->GetName().Equals(OwnedPluginPackageName, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Captured world/package identity is unavailable or has drifted.");
		return false;
	}

	TArray<ULandscapeComponent*> CurrentComponents;
	if (!CollectLandscapeComponents(World, OwnedPackage, CurrentComponents, OutError))
	{
		return false;
	}
	if (CurrentComponents.Num() != LandscapeComponentCount
		|| CurrentComponents.Num() != ComponentSnapshots.Num()
		|| ComponentSnapshots.Num() != CollisionSnapshots.Num())
	{
		OutError = FString::Printf(TEXT("Landscape component set drifted: captured=%d current=%d collision_snapshots=%d."),
			LandscapeComponentCount, CurrentComponents.Num(), CollisionSnapshots.Num());
		return false;
	}

	TSet<ULandscapeComponent*> CurrentComponentSet;
	CurrentComponentSet.Reserve(CurrentComponents.Num());
	for (ULandscapeComponent* Component : CurrentComponents)
	{
		CurrentComponentSet.Add(Component);
	}

	for (const FIMComponentSnapshot& Snapshot : ComponentSnapshots)
	{
		ULandscapeComponent* Component = Snapshot.Component.Get();
		if (!IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Component, OwnedPackage) || !CurrentComponentSet.Contains(Component))
		{
			OutError = TEXT("A captured Landscape component is invalid, no longer in the world, or has changed outer package.");
			return false;
		}

		const ULandscapeComponent* ReadOnlyComponent = Component;
		if (ReadOnlyComponent->GetHeightmap(false) != Snapshot.FinalHeightmap.Get())
		{
			OutError = FString::Printf(TEXT("Final heightmap object changed after capture: %s."), *Component->GetPathName());
			return false;
		}

		const TArray<UTexture2D*>& CurrentWeightmaps = ReadOnlyComponent->GetWeightmapTextures(false);
		if (CurrentWeightmaps.Num() != Snapshot.FinalWeightmaps.Num())
		{
			OutError = FString::Printf(TEXT("Final weightmap count changed after capture: %s."), *Component->GetPathName());
			return false;
		}
		for (int32 WeightmapIndex = 0; WeightmapIndex < CurrentWeightmaps.Num(); ++WeightmapIndex)
		{
			if (CurrentWeightmaps[WeightmapIndex] != Snapshot.FinalWeightmaps[WeightmapIndex])
			{
				OutError = FString::Printf(TEXT("Final weightmap object/order changed after capture: %s index=%d."), *Component->GetPathName(), WeightmapIndex);
				return false;
			}
		}
		if (bValidateLayerAddresses)
		{
			const auto& Usages = ReadOnlyComponent->GetWeightmapTexturesUsage(false);
			ALandscapeProxy* Proxy = Component->GetLandscapeProxy();
			if (!IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Proxy, OwnedPackage) || Usages.Num() != CurrentWeightmaps.Num())
			{
				OutError = TEXT("Final weightmap usage proxy/count differs from current textures.");
				return false;
			}
			const auto& CurrentLayers = ReadOnlyComponent->GetWeightmapLayerAllocations(false);
			TMap<FName, const FWeightmapLayerAllocationInfo*> ByName;
			for (const auto& Layer : CurrentLayers)
			{
				if (!Layer.LayerInfo || ByName.Contains(Layer.LayerInfo->GetLayerName()))
				{
					OutError = TEXT("Final layer identity became null or duplicated after capture.");
					return false;
				}
				ByName.Add(Layer.LayerInfo->GetLayerName(), &Layer);
				if (Layer.IsAllocated())
				{
					const int32 Index = Layer.WeightmapTextureIndex;
					if (!CurrentWeightmaps.IsValidIndex(Index) || Layer.WeightmapTextureChannel >= 4
						|| !IsValid(Usages[Index]))
					{
						OutError = TEXT("Final weightmap allocation has an invalid texture/channel/usage.");
						return false;
					}
					const auto* UsageInMap = Proxy->WeightmapUsageMap.Find(CurrentWeightmaps[Index]);
					if (!UsageInMap || UsageInMap->Get() != Usages[Index] || Usages[Index]->LayerGuid.IsValid()
						|| Usages[Index]->ChannelUsage[Layer.WeightmapTextureChannel] != Component)
					{
						OutError = TEXT("Final weightmap channel ownership differs from the initialized proxy usage map.");
						return false;
					}
				}
			}
			if (ByName.Num() != Snapshot.FinalLayerAllocations.Num())
			{
				OutError = TEXT("Final layer set changed after capture.");
				return false;
			}
			for (const FIMLayerAllocationSnapshot& Layer : Snapshot.FinalLayerAllocations)
			{
				const auto* const* Current = ByName.Find(Layer.LayerName);
				if (!Current || (*Current)->WeightmapTextureIndex != Layer.TextureIndex
					|| (*Current)->WeightmapTextureChannel != Layer.Channel)
				{
					OutError = TEXT("Final named-layer texture/channel changed after capture: ")
						+ Component->GetPathName() + TEXT(" / ") + Layer.LayerName.ToString();
					return false;
				}
			}
		}

		ULandscapeHeightfieldCollisionComponent* Collision = Component->GetCollisionComponent();
		if (Collision != Snapshot.Collision.Get() || !IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Collision, OwnedPackage)
			|| Collision->GetRenderComponent() != Component)
		{
			OutError = FString::Printf(TEXT("Landscape collision component identity or outer changed after capture: %s."), *Component->GetPathName());
			return false;
		}
	}

	for (const FIMTextureSnapshot& Snapshot : TextureSnapshots)
	{
		UTexture2D* Texture = Snapshot.Texture.Get();
		if (!IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Texture, OwnedPackage) || !Texture->Source.IsValid())
		{
			OutError = TEXT("A captured final texture is invalid, shared outside the map package, or no longer has source data.");
			return false;
		}

		const FTextureSource& Source = Texture->Source;
		if (Source.GetNumBlocks() != Snapshot.BlockCount
			|| Source.GetNumLayers() != Snapshot.LayerCount
			|| Source.GetNumMips() != Snapshot.MipCount)
		{
			OutError = FString::Printf(TEXT("Final texture source block/layer/mip layout changed after capture: %s."), *Texture->GetPathName());
			return false;
		}

		const int64 ExpectedMipEntries = int64(Snapshot.BlockCount) * Snapshot.LayerCount * Snapshot.MipCount;
		if (Snapshot.Mips.Num() != ExpectedMipEntries)
		{
			OutError = FString::Printf(TEXT("Captured final texture mip entry count is internally inconsistent: %s."), *Texture->GetPathName());
			return false;
		}

		for (const FIMTextureMipSnapshot& Mip : Snapshot.Mips)
		{
			FImageInfo ImageInfo;
			int64 CurrentByteCount = 0;
			if (!Source.GetMipImageInfo(ImageInfo, Mip.BlockIndex, Mip.LayerIndex, Mip.MipIndex)
				|| !GetMipByteCount(ImageInfo, CurrentByteCount)
				|| ImageInfo.SizeX != Mip.SizeX || ImageInfo.SizeY != Mip.SizeY
				|| ImageInfo.NumSlices != Mip.NumSlices
				|| ImageInfo.GetBytesPerPixel() != Mip.BytesPerPixel
				|| static_cast<int32>(Source.GetFormat(Mip.LayerIndex)) != Mip.SourceFormat
				|| CurrentByteCount != Mip.RawBytes.Num())
			{
				OutError = FString::Printf(TEXT("Final texture mip dimensions, format, or byte layout changed after capture: texture=%s block=%d layer=%d mip=%d."),
					*Texture->GetPathName(), Mip.BlockIndex, Mip.LayerIndex, Mip.MipIndex);
				return false;
			}
		}
	}

	for (const FIMCollisionSnapshot& Snapshot : CollisionSnapshots)
	{
		ULandscapeHeightfieldCollisionComponent* Collision = Snapshot.Collision.Get();
		int64 ExpectedElementCount = 0;
		if (!IMAcousticLandscapeFinalSnapshotPrivate::IsOwnedObject(Collision, OwnedPackage)
			|| Collision->GetRenderComponent() != Snapshot.RenderComponent.Get()
			|| Collision->CollisionSizeQuads != Snapshot.CollisionSizeQuads
			|| Collision->SimpleCollisionSizeQuads != Snapshot.SimpleCollisionSizeQuads
			|| !GetCollisionHeightElementCount(*Collision, ExpectedElementCount)
			|| ExpectedElementCount != Snapshot.ElementCount
			|| Collision->CollisionHeightData.IsLocked()
			|| Collision->CollisionHeightData.GetElementSize() != sizeof(uint16)
			|| Collision->CollisionHeightData.GetElementCount() != Snapshot.ElementCount
			|| Snapshot.HeightSamples.Num() != Snapshot.ElementCount)
		{
			OutError = FString::Printf(TEXT("Collision height bulk layout or lock state changed after capture: %s."),
				Collision ? *Collision->GetPathName() : TEXT("<null>"));
			return false;
		}
	}
	if (bValidateLayerAddresses)
	{
		TMap<ULandscapeComponent*, TArray<FWeightmapLayerAllocationInfo>> CurrentAllocations;
		for (ULandscapeComponent* Component : CurrentComponents)
		{
			const ULandscapeComponent* ReadOnly = Component;
			CurrentAllocations.Add(Component, ReadOnly->GetWeightmapLayerAllocations(false));
		}
		TArray<ALandscapeProxy*> Proxies;
		if (!IMAcousticLandscapeFinalSnapshotPrivate::PreflightWeightmapUsage(World, OwnedPackage, CurrentAllocations, Proxies, OutError, true)) return false;
	}

	return true;
}

bool FIMAcousticLandscapeFinalSnapshot::FailAfterWrite(const FString& Reason, FString& OutError)
{
	bRestoreComplete = false;
	if (bRestorePartial && CapturedPackage.IsValid())
	{
		CapturedPackage->MarkPackageDirty();
	}
	OutError = FString::Printf(TEXT("%s %s"), *Reason, *GetRestoreSummary());
	return false;
}

bool FIMAcousticLandscapeFinalSnapshot::Restore(FString& OutError)
{
	OutError.Reset();
	bRestoreAttempted = true;
	bRestoreComplete = false;
	bRestorePartial = false;
	RestoredTextureCount = 0;
	RestoredMipCount = 0;
	RestoredCollisionCount = 0;
	CollisionRecreateCallCount = 0;
	CollisionLayerDataUpdates = 0;
	RestoredTextureByteCount = 0;
	RestoredCollisionByteCount = 0;

	if (!IsInGameThread())
	{
		OutError = FString::Printf(TEXT("Landscape final restoration must run on the game thread. %s"), *GetRestoreSummary());
		return false;
	}

	FString PreconditionsError;
	if (!ValidateRestorePreconditions(PreconditionsError))
	{
		OutError = FString::Printf(TEXT("No data was written: %s %s"), *PreconditionsError, *GetRestoreSummary());
		return false;
	}

	for (FIMTextureSnapshot& Snapshot : TextureSnapshots)
	{
		UTexture2D* Texture = Snapshot.Texture.Get();
		check(IsValid(Texture));
		FTextureSource& Source = Texture->Source;

		// From this point a failure is reported as partial because PreEditChange can affect the asset's edit state.
		bRestorePartial = true;
		Texture->PreEditChange(nullptr);
		FString TextureWriteError;
		for (const FIMTextureMipSnapshot& Mip : Snapshot.Mips)
		{
			uint8* Destination = Source.LockMip(Mip.BlockIndex, Mip.LayerIndex, Mip.MipIndex);
			if (Destination == nullptr)
			{
				TextureWriteError = FString::Printf(TEXT("Texture source mip write lock failed: texture=%s block=%d layer=%d mip=%d."),
					*Texture->GetPathName(), Mip.BlockIndex, Mip.LayerIndex, Mip.MipIndex);
				break;
			}

			FMemory::Memcpy(Destination, Mip.RawBytes.GetData(), static_cast<SIZE_T>(Mip.RawBytes.Num()));
			Source.UnlockMip(Mip.BlockIndex, Mip.LayerIndex, Mip.MipIndex);
			++RestoredMipCount;
			RestoredTextureByteCount += Mip.RawBytes.Num();
		}

		Texture->PostEditChange();
		if (!TextureWriteError.IsEmpty())
		{
			return FailAfterWrite(TextureWriteError, OutError);
		}
		++RestoredTextureCount;
	}

	for (const FIMCollisionSnapshot& Snapshot : CollisionSnapshots)
	{
		ULandscapeHeightfieldCollisionComponent* Collision = Snapshot.Collision.Get();
		check(IsValid(Collision));
		bRestorePartial = true;
		void* Destination = Collision->CollisionHeightData.Lock(LOCK_READ_WRITE);
		if (Destination == nullptr)
		{
			Collision->CollisionHeightData.Unlock();
			return FailAfterWrite(FString::Printf(TEXT("Collision height bulk write lock failed: %s."), *Collision->GetPathName()), OutError);
		}

		const int64 ByteCount = Snapshot.ElementCount * sizeof(uint16);
		FMemory::Memcpy(Destination, Snapshot.HeightSamples.GetData(), static_cast<SIZE_T>(ByteCount));
		Collision->CollisionHeightData.Unlock();
		++RestoredCollisionCount;
		RestoredCollisionByteCount += ByteCount;
		// DLD bytes index ComponentLayerInfos. Rebuild both from the restored
		// weightmaps and current PM LayerInfo bindings, never copy pristine IDs.
		Snapshot.RenderComponent->UpdateCollisionLayerData();
		++CollisionLayerDataUpdates;
		Collision->RecreateCollision();
		++CollisionRecreateCallCount;
	}

	if (CapturedPackage.IsValid())
	{
		CapturedPackage->MarkPackageDirty();
	}
	bRestoreComplete = true;
	bRestorePartial = false;
	return true;
}

FString FIMAcousticLandscapeFinalSnapshot::GetCaptureSummary() const
{
	return FString::Printf(
		TEXT("captured=%s; landscape_components=%d; collision_components=%d; unique_textures=%d; texture_mips=%d; texture_source_bytes=%lld; collision_height_bytes=%lld; allocation_layouts_restored=%d; weightmap_usage_initializations=%d"),
		bCaptured ? TEXT("true") : TEXT("false"),
		LandscapeComponentCount,
		CollisionSnapshots.Num(),
		TextureSnapshots.Num(),
		TextureMipCount,
		TextureSourceByteCount,
		CollisionHeightByteCount,
		AllocationLayoutsRestored,
		WeightmapUsageInitializations);
}

FString FIMAcousticLandscapeFinalSnapshot::GetRestoreSummary() const
{
	return FString::Printf(
		TEXT("restore_attempted=%s; restore_complete=%s; restore_partial=%s; textures_written=%d/%d; mips_written=%d/%d; texture_bytes_written=%lld/%lld; collisions_written=%d/%d; collision_bytes_written=%lld/%lld; collision_recreate_calls=%d; collision_layer_data_updates=%d"),
		bRestoreAttempted ? TEXT("true") : TEXT("false"),
		bRestoreComplete ? TEXT("true") : TEXT("false"),
		bRestorePartial ? TEXT("true") : TEXT("false"),
		RestoredTextureCount,
		TextureSnapshots.Num(),
		RestoredMipCount,
		TextureMipCount,
		RestoredTextureByteCount,
		TextureSourceByteCount,
		RestoredCollisionCount,
		CollisionSnapshots.Num(),
		RestoredCollisionByteCount,
		CollisionHeightByteCount,
		CollisionRecreateCallCount,
		CollisionLayerDataUpdates);
}

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA
