#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "DumpGPU.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "IMAcousticLandscapeCells.h"
#include "IMAcousticLandscapeFinalSnapshot.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ObjectTools.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <limits>
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace IMAcousticRuralLandscapeCopyTestPrivate
{
constexpr TCHAR SourceMapPackage[] = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr TCHAR PreviewMapPackage[] = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr int64 MaxSamplesPerComponent = 16LL * 1024LL * 1024LL;
constexpr int64 MaxTextureSourceMipBytes = 128LL * 1024LL * 1024LL;
constexpr int64 MaxTextureSourceMipEntries = 4096;
constexpr int32 MaxDifferenceDetails = 256;

struct FIMRawDataSummary
{
	bool bReadable = false;
	bool bPresent = false;
	int64 ElementCount = 0;
	int64 ByteCount = 0;
	FString Sha1;
	FString Status = TEXT("NOT_READ");
};

struct FIMTextureMipSnapshot
{
	int32 BlockIndex = 0;
	int32 LayerIndex = 0;
	int32 MipIndex = 0;
	int32 Width = 0;
	int32 Height = 0;
	int32 NumSlices = 0;
	int32 SourceFormat = 0;
	int64 ByteCount = 0;
	FString Sha1;
};

struct FIMTextureMipChainSnapshot
{
	bool bReadable = false;
	int32 BlockCount = 0;
	int32 LayerCount = 0;
	int32 MipCount = 0;
	int64 ByteCount = 0;
	FString Status = TEXT("NOT_READ");
	FString Error;
	TArray<FIMTextureMipSnapshot> Mips;
};

struct FIMWeightLayerSnapshot
{
	FString LayerName;
	FString LayerInfoAssetPath;
	FString WeightTexturePath;
	bool bAllocated = false;
	bool bReadable = false;
	int64 SampleCount = 0;
	int64 ByteCount = 0;
	FString Sha1;
	TArray<FString> PhysicalMaterialPaths;
	// Test-only retained samples localize a failed hash comparison; they are not serialized as full arrays.
	TArray<uint8> WeightValues;
	FIMTextureMipChainSnapshot TextureMipChain;
	FString Status = TEXT("NOT_READ");
};

struct FIMComponentSnapshot
{
	FString IdentityKey;
	FIntPoint SectionBase = FIntPoint::ZeroValue;
	FVector Location = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector Scale = FVector::OneVector;
	int32 ComponentSizeQuads = 0;
	int32 NumSubsections = 0;
	int32 SubsectionSizeQuads = 0;

	FString HeightmapTexturePath;
	bool bHeightReadable = false;
	int64 HeightSampleCount = 0;
	int64 HeightTexelBytesRead = 0;
	int64 HeightValueBytesHashed = 0;
	FString HeightTexelSha1;
	FString HeightValueSha1;
	TArray<uint16> HeightValues;
	FIMTextureMipChainSnapshot HeightmapMipChain;

	TArray<FIMWeightLayerSnapshot> WeightLayers;
	bool bWeightsReadable = true;
	int64 WeightBytesRead = 0;
	bool bWeightMipsReadable = true;

	FString CollisionIdentityKey;
	bool bHasCollision = false;
	int32 CollisionSizeQuads = 0;
	int32 SimpleCollisionSizeQuads = 0;
	double CollisionScale = 0.0;
	FIMRawDataSummary CollisionHeightData;
	FIMRawDataSummary DominantLayerData;
	TArray<uint8> UsedDominantLayerIndices;
	FString CollisionQuadFlagsSha1;
	int64 CollisionQuadFlagsBytes = 0;
	bool bRuntimeCollisionHeightReadable = false;
	int64 RuntimeCollisionHeightSampleCount = 0;
	int64 RuntimeCollisionHeightBytesRead = 0;
	FString RuntimeCollisionHeightSha1;
	TArray<float> RuntimeCollisionHeightValues;

	bool bPhysicalMaterialCellsReadable = false;
	int32 StoredPhysicalMaterialIndexCount = 0;
	int64 PhysicalMaterialCellCount = 0;
	int64 PhysicalMaterialCellBytesRead = 0;
	int64 PhysicalMaterialHoleCellCount = 0;
	FString PhysicalMaterialCellSha1;
	FString PhysicalMaterialHoleMaskSha1;
	FString PhysicalMaterialCellStatus = TEXT("NOT_READ");
	FString PhysicalMaterialCellError;

	TArray<FString> CollisionLayerNames;
	TArray<FString> CollisionLayerInfoPaths;
	bool bHasValidPhysicalMaterialInputs = false;
	TArray<FString> PhysicalMaterialPaths;
	TArray<FString> RenderMaterialPaths;
	TArray<FString> WeightTexturePaths;
	bool bComplete = true;
};

struct FIMMapSnapshot
{
	FString CaptureKind = TEXT("EDITOR_MAP_LOAD");
	FString RequestedPackage;
	FString LoadedPackage;
	FString PackageFilename;
	FString PackageSha1;
	FString PackageSha1AfterRestore;
	int64 PackageBytesRead = 0;
	int64 PackageRecheckBytesRead = 0;
	bool bPackageRead = false;
	bool bPackageUnchanged = false;
	bool bLoadMapReturned = false;
	bool bWorldIdentityMatches = false;
	bool bNonWorldPartition = false;
	bool bLevelCoverageComplete = true;
	bool bHasLandscape = false;
	bool bCoverageComplete = true;
	bool bWorldReadinessConfirmed = false;
	int32 WorldReadinessWaitFrameCount = 0;
	double WorldReadinessWaitSeconds = 0.0;
	int32 WorldReadinessLandscapeComponentCount = 0;
	int32 WorldReadinessCollisionComponentCount = 0;
	int32 WorldReadinessStreamingLevelCount = 0;
	int32 WorldReadinessLoadedStreamingLevelCount = 0;
	bool bHasValidPhysicalMaterialInputs = false;
	int32 StreamingLevelCount = 0;
	int32 LoadedStreamingLevelCount = 0;
	int32 LandscapeComponentCount = 0;
	int32 CollisionComponentCount = 0;
	int32 HeightCompleteComponentCount = 0;
	int32 WeightCompleteComponentCount = 0;
	int32 HeightMipCompleteComponentCount = 0;
	int32 WeightMipCompleteComponentCount = 0;
	int32 CollisionHeightCompleteComponentCount = 0;
	int32 PhysicalMaterialCellCompleteComponentCount = 0;
	int32 DominantLayerBulkPresentCount = 0;
	int32 DominantLayerBulkEmptyCount = 0;
	int64 HeightTexelBytesRead = 0;
	int64 HeightValueBytesHashed = 0;
	int64 WeightBytesRead = 0;
	int64 CollisionHeightBytesRead = 0;
	int64 DominantLayerBytesRead = 0;
	int64 CollisionQuadFlagsBytesRead = 0;
	int64 RuntimeCollisionHeightBytesRead = 0;
	int32 TextureSourceMipTextureCount = 0;
	int32 TextureSourceMipCount = 0;
	int32 TextureSourceMipReadFailureCount = 0;
	int64 TextureSourceMipBytesRead = 0;
	int64 PhysicalMaterialCellCount = 0;
	int64 PhysicalMaterialCellBytesRead = 0;
	int64 PhysicalMaterialHoleCellCount = 0;
	TArray<FIMComponentSnapshot> Components;
	TArray<FString> LayerInfoAssetPaths;
	TArray<FString> PhysicalMaterialPaths;
	TArray<FString> RenderMaterialPaths;
	TArray<FString> HeightmapTexturePaths;
	TArray<FString> WeightTexturePaths;
	TArray<FString> Issues;
};

struct FIMComparison
{
	int32 MatchedComponentCount = 0;
	int32 SourceOnlyComponentCount = 0;
	int32 PreviewOnlyComponentCount = 0;
	int32 TopologyMismatchCount = 0;
	int32 HeightMismatchCount = 0;
	int32 HeightMipMismatchCount = 0;
	int32 CollisionHeightMismatchCount = 0;
	int32 RuntimeCollisionHeightMismatchCount = 0;
	int32 CollisionFlagMismatchCount = 0;
	int32 WeightMismatchCount = 0;
	int32 WeightMipMismatchCount = 0;
	int32 PhysicalMaterialCellMismatchCount = 0;
	int32 PhysicalMaterialHoleMaskMismatchCount = 0;
	int32 DominantLayerMismatchCount = 0;
	int32 DominantLayerPmCacheExceptionCount = 0;
	int32 CollisionTransformMismatchCount = 0;
	int32 HardMismatchCount = 0;
	TArray<FString> Differences;
	TArray<TSharedPtr<FJsonValue>> SampleDeltaDetails;
};

static FString HashBytesSha1(const void* Data, const int64 ByteCount)
{
	static const uint8 EmptyByte = 0;
	const void* SafeData = Data ? Data : &EmptyByte;
	return FSHA1::HashBuffer(SafeData, static_cast<uint64>(ByteCount)).ToString();
}

static bool ReadFileSha1(const FString& Filename, FString& OutSha1, int64& OutBytesRead, FString& OutError)
{
	OutSha1.Reset();
	OutBytesRead = 0;
	OutError.Reset();

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
	if (!Reader.IsValid())
	{
		OutError = TEXT("Could not open package file for read-only hashing.");
		return false;
	}

	const int64 FileSize = Reader->TotalSize();
	if (FileSize <= 0)
	{
		OutError = TEXT("Package file is empty or its size is unavailable.");
		return false;
	}

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(64 * 1024);
	FSHA1 Hasher;
	while (Reader->Tell() < FileSize)
	{
		const int64 StartOffset = Reader->Tell();
		const int64 BytesToRead = FMath::Min<int64>(Buffer.Num(), FileSize - StartOffset);
		Reader->Serialize(Buffer.GetData(), BytesToRead);
		if (Reader->IsError() || Reader->Tell() != StartOffset + BytesToRead)
		{
			OutError = TEXT("Package file read ended before the reported file size.");
			return false;
		}

		Hasher.Update(Buffer.GetData(), static_cast<uint64>(BytesToRead));
		OutBytesRead += BytesToRead;
	}

	if (OutBytesRead != FileSize)
	{
		OutError = TEXT("Package file byte coverage did not match its reported size.");
		return false;
	}

	Hasher.Final();
	uint8 DigestBytes[FSHA1::DigestSize];
	Hasher.GetHash(DigestBytes);
	FSHAHash Digest;
	FMemory::Memcpy(Digest.Hash, DigestBytes, sizeof(DigestBytes));
	OutSha1 = Digest.ToString();
	return true;
}

static FIMTextureMipChainSnapshot ReadTextureMipChain(
	UTexture2D* Texture,
	TMap<const UTexture2D*, FIMTextureMipChainSnapshot>& Cache)
{
	FIMTextureMipChainSnapshot Chain;
	if (!IsInGameThread())
	{
		Chain.Status = TEXT("WRONG_THREAD");
		Chain.Error = TEXT("Texture source mip data must be read on the game thread.");
		return Chain;
	}
	if (!IsValid(Texture))
	{
		Chain.Status = TEXT("TEXTURE_UNAVAILABLE");
		Chain.Error = TEXT("Texture pointer is invalid.");
		return Chain;
	}
	if (const FIMTextureMipChainSnapshot* Cached = Cache.Find(Texture))
	{
		return *Cached;
	}

	if (!Texture->Source.IsValid())
	{
		Chain.Status = TEXT("SOURCE_UNAVAILABLE");
		Chain.Error = TEXT("Texture source data is unavailable.");
		Cache.Add(Texture, Chain);
		return Chain;
	}

	FTextureSource& Source = Texture->Source;
	Chain.BlockCount = Source.GetNumBlocks();
	Chain.LayerCount = Source.GetNumLayers();
	Chain.MipCount = Source.GetNumMips();
	int64 MipEntryCount = 0;
	bool bMipLayoutValid = Chain.BlockCount > 0 && Chain.LayerCount > 0 && Chain.MipCount > 0;
	if (bMipLayoutValid)
	{
		const int64 BlockLayerCount = static_cast<int64>(Chain.BlockCount) * Chain.LayerCount;
		bMipLayoutValid = BlockLayerCount <= MaxTextureSourceMipEntries
			&& Chain.MipCount <= MaxTextureSourceMipEntries / BlockLayerCount;
		if (bMipLayoutValid)
		{
			MipEntryCount = BlockLayerCount * Chain.MipCount;
		}
	}
	if (!bMipLayoutValid)
	{
		Chain.Status = TEXT("INVALID_MIP_LAYOUT");
		Chain.Error = TEXT("Texture source has an invalid or excessive block/layer/mip layout.");
		Cache.Add(Texture, Chain);
		return Chain;
	}

	bool bReadComplete = true;
	for (int32 BlockIndex = 0; BlockIndex < Chain.BlockCount && bReadComplete; ++BlockIndex)
	{
		for (int32 LayerIndex = 0; LayerIndex < Chain.LayerCount && bReadComplete; ++LayerIndex)
		{
			for (int32 MipIndex = 0; MipIndex < Chain.MipCount; ++MipIndex)
			{
				FImageInfo ImageInfo;
				if (!Source.GetMipImageInfo(ImageInfo, BlockIndex, LayerIndex, MipIndex)
					|| !ImageInfo.IsImageInfoValid())
				{
					Chain.Status = TEXT("MIP_INFO_UNAVAILABLE");
					Chain.Error = FString::Printf(
						TEXT("Texture source mip metadata is unavailable for block=%d layer=%d mip=%d."),
						BlockIndex, LayerIndex, MipIndex);
					bReadComplete = false;
					break;
				}

				const int64 Width = ImageInfo.SizeX;
				const int64 Height = ImageInfo.SizeY;
				const int64 SliceCount = ImageInfo.NumSlices;
				const int64 BytesPerPixel = ImageInfo.GetBytesPerPixel();
				if (Width <= 0 || Height <= 0 || SliceCount <= 0 || BytesPerPixel <= 0
					|| Width > MaxSamplesPerComponent / Height
					|| Width * Height > MaxSamplesPerComponent / SliceCount)
				{
					Chain.Status = TEXT("MIP_SIZE_OUT_OF_RANGE");
					Chain.Error = FString::Printf(
						TEXT("Texture source mip dimensions or byte size exceed the bounded read limit at block=%d layer=%d mip=%d."),
						BlockIndex, LayerIndex, MipIndex);
					bReadComplete = false;
					break;
				}

				const int64 PixelCount = Width * Height * SliceCount;
				if (PixelCount > MaxTextureSourceMipBytes / BytesPerPixel)
				{
					Chain.Status = TEXT("MIP_SIZE_OUT_OF_RANGE");
					Chain.Error = FString::Printf(
						TEXT("Texture source mip byte size exceeds the bounded read limit at block=%d layer=%d mip=%d."),
						BlockIndex, LayerIndex, MipIndex);
					bReadComplete = false;
					break;
				}
				const int64 ExpectedByteCount = PixelCount * BytesPerPixel;

				if (Chain.ByteCount > MaxTextureSourceMipBytes - ExpectedByteCount)
				{
					Chain.Status = TEXT("MIP_CHAIN_SIZE_OUT_OF_RANGE");
					Chain.Error = TEXT("Texture source mip chain exceeds the bounded read limit.");
					bReadComplete = false;
					break;
				}

				TArray64<uint8> MipData;
				if (!Source.GetMipData(MipData, BlockIndex, LayerIndex, MipIndex)
					|| MipData.Num() != ExpectedByteCount)
				{
					Chain.Status = TEXT("MIP_READ_FAILED");
					Chain.Error = FString::Printf(
						TEXT("Texture source mip byte read was incomplete for block=%d layer=%d mip=%d."),
						BlockIndex, LayerIndex, MipIndex);
					bReadComplete = false;
					break;
				}

				FIMTextureMipSnapshot& Mip = Chain.Mips.AddDefaulted_GetRef();
				Mip.BlockIndex = BlockIndex;
				Mip.LayerIndex = LayerIndex;
				Mip.MipIndex = MipIndex;
				Mip.Width = ImageInfo.SizeX;
				Mip.Height = ImageInfo.SizeY;
				Mip.NumSlices = ImageInfo.NumSlices;
				Mip.SourceFormat = static_cast<int32>(Source.GetFormat(LayerIndex));
				Mip.ByteCount = MipData.Num();
				Mip.Sha1 = HashBytesSha1(MipData.GetData(), MipData.Num());
				Chain.ByteCount += MipData.Num();
			}
		}
	}

	Chain.bReadable = bReadComplete && Chain.Mips.Num() == MipEntryCount;
	if (Chain.bReadable)
	{
		Chain.Status = TEXT("READ");
	}
	else if (Chain.Status == TEXT("NOT_READ"))
	{
		Chain.Status = TEXT("MIP_CHAIN_INCOMPLETE");
		Chain.Error = TEXT("Texture source mip chain was incomplete.");
	}
	Cache.Add(Texture, Chain);
	return Chain;
}

template <typename BulkDataType>
static FIMRawDataSummary ReadRawBulkData(const BulkDataType& BulkData, const int64 ElementSize,
	TArray<uint8>* OutUsedByteIndices = nullptr)
{
	FIMRawDataSummary Result;
	if (OutUsedByteIndices)
	{
		OutUsedByteIndices->Reset();
		if (ElementSize != sizeof(uint8))
		{
			Result.Status = TEXT("INVALID_INDEX_ELEMENT_SIZE");
			return Result;
		}
	}
	Result.ElementCount = BulkData.GetElementCount();
	Result.ByteCount = BulkData.GetBulkDataSize();

	if (Result.ByteCount == 0 && Result.ElementCount == 0)
	{
		Result.bReadable = true;
		Result.Status = TEXT("EMPTY");
		Result.Sha1 = HashBytesSha1(nullptr, 0);
		return Result;
	}

	if (Result.ByteCount <= 0 || Result.ElementCount <= 0
		|| Result.ElementCount > MAX_int64 / ElementSize
		|| Result.ElementCount * ElementSize != Result.ByteCount)
	{
		Result.Status = TEXT("SIZE_MISMATCH");
		return Result;
	}

	const void* LockedData = BulkData.LockReadOnly();
	if (!LockedData)
	{
		Result.Status = TEXT("LOCK_FAILED");
		return Result;
	}

	Result.Sha1 = HashBytesSha1(LockedData, Result.ByteCount);
	if (OutUsedByteIndices)
	{
		// Scan every sample while the bulk lock is held. The distinct set is
		// bounded by uint8, and combined with raw equality covers every cell.
		bool Used[256]{};
		const uint8* Values = static_cast<const uint8*>(LockedData);
		for (int64 Index = 0; Index < Result.ByteCount; ++Index) Used[Values[Index]] = true;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Used); ++Index)
			if (Used[Index]) OutUsedByteIndices->Add(static_cast<uint8>(Index));
	}
	BulkData.Unlock();
	Result.bReadable = true;
	Result.bPresent = true;
	Result.Status = TEXT("READ");
	return Result;
}

static void AddIssue(FIMMapSnapshot& Map, const FString& Issue)
{
	Map.bCoverageComplete = false;
	Map.Issues.Add(Issue);
}

static void AddComponentIssue(FIMMapSnapshot& Map, FIMComponentSnapshot& Component, const FString& Issue)
{
	Component.bComplete = false;
	AddIssue(Map, FString::Printf(TEXT("%s: %s"), *Component.IdentityKey, *Issue));
}

static void AddUniquePath(TArray<FString>& Paths, const UObject* Object)
{
	if (IsValid(Object))
	{
		Paths.Add(Object->GetPathName());
	}
}

static void SortUniquePaths(TArray<FString>& Paths)
{
	Paths.Sort();
	for (int32 Index = Paths.Num() - 1; Index > 0; --Index)
	{
		if (Paths[Index] == Paths[Index - 1])
		{
			Paths.RemoveAt(Index, 1, EAllowShrinking::No);
		}
	}
}

static FQuat CanonicalRotation(const FQuat& InRotation)
{
	FQuat Rotation = InRotation.GetNormalized();
	bool bFlip = false;
	const double SignComponents[] = {Rotation.W, Rotation.Z, Rotation.Y, Rotation.X};
	for (const double SignComponent : SignComponents)
	{
		if (SignComponent != 0.0)
		{
			bFlip = SignComponent < 0.0;
			break;
		}
	}
	if (bFlip)
	{
		Rotation.X = -Rotation.X;
		Rotation.Y = -Rotation.Y;
		Rotation.Z = -Rotation.Z;
		Rotation.W = -Rotation.W;
	}
	return Rotation;
}

static FString MakeIdentityKey(const FIntPoint& SectionBase, const FTransform& Transform)
{
	const FVector Location = Transform.GetLocation();
	const FQuat Rotation = CanonicalRotation(Transform.GetRotation());
	const FVector Scale = Transform.GetScale3D();
	return FString::Printf(
		TEXT("%d,%d|%.17g,%.17g,%.17g|%.17g,%.17g,%.17g,%.17g|%.17g,%.17g,%.17g"),
		SectionBase.X, SectionBase.Y,
		Location.X, Location.Y, Location.Z,
		Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
		Scale.X, Scale.Y, Scale.Z);
}

static TSharedRef<FJsonObject> MakeVectorJson(const FVector& Value)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("x"), Value.X);
	Json->SetNumberField(TEXT("y"), Value.Y);
	Json->SetNumberField(TEXT("z"), Value.Z);
	return Json;
}

static TSharedRef<FJsonObject> MakeRotationJson(const FQuat& Value)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("x"), Value.X);
	Json->SetNumberField(TEXT("y"), Value.Y);
	Json->SetNumberField(TEXT("z"), Value.Z);
	Json->SetNumberField(TEXT("w"), Value.W);
	return Json;
}

static TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> JsonValues;
	JsonValues.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		JsonValues.Add(MakeShared<FJsonValueString>(Value));
	}
	return JsonValues;
}

static bool IsWithinSampleLimit(const int64 SampleCount)
{
	return SampleCount > 0 && SampleCount <= MaxSamplesPerComponent
		&& SampleCount <= MAX_int32;
}

static FIMWeightLayerSnapshot ReadWeightLayer(
	ULandscapeComponent* Component,
	const FWeightmapLayerAllocationInfo& Allocation,
	const TArray<UTexture2D*>& WeightTextures,
	TMap<const UTexture2D*, FIMTextureMipChainSnapshot>& TextureMipCache,
	FLandscapeComponentDataInterface* DataInterface,
	FIMMapSnapshot& Map,
	FIMComponentSnapshot& ComponentSnapshot,
	bool& bWeightMipsReadable)
{
	FIMWeightLayerSnapshot Layer;
	ULandscapeLayerInfoObject* LayerInfo = Allocation.LayerInfo.Get();
	Layer.bAllocated = Allocation.IsAllocated();

	UTexture2D* WeightTexture = nullptr;
	if (Layer.bAllocated)
	{
		if (Allocation.WeightmapTextureIndex < WeightTextures.Num()
			&& Allocation.WeightmapTextureChannel < 4)
		{
			WeightTexture = WeightTextures[Allocation.WeightmapTextureIndex];
			if (IsValid(WeightTexture))
			{
				Layer.WeightTexturePath = WeightTexture->GetPathName();
				ComponentSnapshot.WeightTexturePaths.Add(Layer.WeightTexturePath);
				Map.WeightTexturePaths.Add(Layer.WeightTexturePath);
				Layer.TextureMipChain = ReadTextureMipChain(WeightTexture, TextureMipCache);
				if (!Layer.TextureMipChain.bReadable)
				{
					bWeightMipsReadable = false;
					AddComponentIssue(Map, ComponentSnapshot, FString::Printf(
						TEXT("Allocated weightmap texture %s source mip chain is unreadable: %s"),
						*Layer.WeightTexturePath, *Layer.TextureMipChain.Error));
				}
			}
			else
			{
				bWeightMipsReadable = false;
			}
		}
		else
		{
			bWeightMipsReadable = false;
		}
	}

	if (IsValid(LayerInfo))
	{
		Layer.LayerName = LayerInfo->GetLayerName().ToString();
		Layer.LayerInfoAssetPath = LayerInfo->GetPathName();
		Map.LayerInfoAssetPaths.Add(Layer.LayerInfoAssetPath);
		if (UPhysicalMaterial* LayerPhysicalMaterial = LayerInfo->GetPhysicalMaterial().Get())
		{
			Layer.PhysicalMaterialPaths.Add(LayerPhysicalMaterial->GetPathName());
			Map.PhysicalMaterialPaths.Add(LayerPhysicalMaterial->GetPathName());
			ComponentSnapshot.PhysicalMaterialPaths.Add(LayerPhysicalMaterial->GetPathName());
			ComponentSnapshot.bHasValidPhysicalMaterialInputs = true;
			Map.bHasValidPhysicalMaterialInputs = true;
		}
	}
	else
	{
		Layer.LayerName = TEXT("<INVALID_LAYER_INFO>");
		Layer.Status = TEXT("INVALID_LAYER_INFO");
		ComponentSnapshot.bWeightsReadable = false;
		AddComponentIssue(Map, ComponentSnapshot, TEXT("A weight allocation has no valid LandscapeLayerInfoObject."));
		return Layer;
	}

	if (!Layer.bAllocated)
	{
		Layer.bReadable = true;
		Layer.Status = TEXT("UNALLOCATED_ZERO_SOURCE");
		Layer.Sha1 = HashBytesSha1(nullptr, 0);
		return Layer;
	}

	if (!DataInterface)
	{
		Layer.Status = TEXT("HEIGHTMAP_SOURCE_UNAVAILABLE");
		ComponentSnapshot.bWeightsReadable = false;
		AddComponentIssue(Map, ComponentSnapshot, TEXT("Weight data could not be read because the component height source was unavailable."));
		return Layer;
	}

	if (Allocation.WeightmapTextureIndex >= WeightTextures.Num()
		|| Allocation.WeightmapTextureChannel >= 4)
	{
		Layer.Status = TEXT("INVALID_WEIGHTMAP_ALLOCATION");
		ComponentSnapshot.bWeightsReadable = false;
		AddComponentIssue(Map, ComponentSnapshot, FString::Printf(
			TEXT("Layer %s has an invalid weightmap texture index or channel."), *Layer.LayerName));
		return Layer;
	}

	if (!IsValid(WeightTexture) || !WeightTexture->Source.IsValid())
	{
		Layer.Status = TEXT("WEIGHTMAP_SOURCE_UNAVAILABLE");
		ComponentSnapshot.bWeightsReadable = false;
		AddComponentIssue(Map, ComponentSnapshot, FString::Printf(
			TEXT("Layer %s has no readable weightmap texture source."), *Layer.LayerName));
		return Layer;
	}

	TArray<uint8> WeightValues;
	if (!DataInterface->GetWeightmapTextureData(LayerInfo, WeightValues, false, false))
	{
		Layer.Status = TEXT("WEIGHT_READ_FAILED");
		ComponentSnapshot.bWeightsReadable = false;
		AddComponentIssue(Map, ComponentSnapshot, FString::Printf(
			TEXT("GetWeightmapTextureData failed for layer %s."), *Layer.LayerName));
		return Layer;
	}

	Layer.SampleCount = WeightValues.Num();
	Layer.ByteCount = WeightValues.Num();
	Layer.Sha1 = HashBytesSha1(WeightValues.GetData(), Layer.ByteCount);
	Layer.bReadable = Layer.SampleCount > 0;
	Layer.Status = Layer.bReadable ? TEXT("READ") : TEXT("EMPTY_READ");
	ComponentSnapshot.WeightBytesRead += Layer.ByteCount;

	if (!Layer.bReadable || Layer.SampleCount != ComponentSnapshot.HeightSampleCount)
	{
		ComponentSnapshot.bWeightsReadable = false;
		AddComponentIssue(Map, ComponentSnapshot, FString::Printf(
			TEXT("Layer %s returned %lld weight bytes for %lld height samples."),
			*Layer.LayerName, Layer.ByteCount, ComponentSnapshot.HeightSampleCount));
	}

	Layer.WeightValues = MoveTemp(WeightValues);

	return Layer;
}

static void CaptureComponentData(
	ULandscapeComponent* Component,
	const int32 ComponentIndex,
	FIMMapSnapshot& Map,
	TMap<const ULandscapeComponent*, int32>& ComponentIndexByPointer,
	TMap<const UTexture2D*, FIMTextureMipChainSnapshot>& TextureMipCache)
{
	FIMComponentSnapshot Snapshot;
	Snapshot.SectionBase = Component->GetSectionBase();
	const FTransform ComponentTransform = Component->GetComponentTransform();
	Snapshot.IdentityKey = MakeIdentityKey(Snapshot.SectionBase, ComponentTransform);
	Snapshot.Location = ComponentTransform.GetLocation();
	Snapshot.Rotation = CanonicalRotation(ComponentTransform.GetRotation());
	Snapshot.Scale = ComponentTransform.GetScale3D();
	Snapshot.ComponentSizeQuads = Component->ComponentSizeQuads;
	Snapshot.NumSubsections = Component->NumSubsections;
	Snapshot.SubsectionSizeQuads = Component->SubsectionSizeQuads;
	ComponentIndexByPointer.Add(Component, ComponentIndex);

	if (Snapshot.ComponentSizeQuads <= 0 || Snapshot.NumSubsections <= 0 || Snapshot.SubsectionSizeQuads <= 0)
	{
		AddComponentIssue(Map, Snapshot, TEXT("Landscape component topology contains a non-positive size."));
	}

	UTexture2D* Heightmap = Component->GetHeightmap(false);
	Snapshot.HeightmapTexturePath = GetPathNameSafe(Heightmap);
	AddUniquePath(Map.HeightmapTexturePaths, Heightmap);
	Snapshot.HeightmapMipChain = ReadTextureMipChain(Heightmap, TextureMipCache);
	if (Snapshot.HeightmapMipChain.bReadable)
	{
		++Map.HeightMipCompleteComponentCount;
	}
	else
	{
		AddComponentIssue(Map, Snapshot, FString::Printf(
			TEXT("Heightmap source mip chain is unreadable: %s"), *Snapshot.HeightmapMipChain.Error));
	}
	if (IsValid(Heightmap) && Heightmap->Source.IsValid())
	{
		const int64 HeightmapSize = (static_cast<int64>(Component->SubsectionSizeQuads) + 1) * Component->NumSubsections;
		const int64 HeightSampleCount = HeightmapSize * HeightmapSize;
		if (IsWithinSampleLimit(HeightSampleCount))
		{
			FLandscapeComponentDataInterface DataInterface(Component, 0, false);
			TArray<FColor> HeightTexels;
			DataInterface.GetHeightmapTextureData(HeightTexels, false);
			if (HeightTexels.Num() != HeightSampleCount)
			{
				AddComponentIssue(Map, Snapshot, FString::Printf(
					TEXT("Height source returned %d texels; topology requires %lld."),
					HeightTexels.Num(), HeightSampleCount));
			}
			else
			{
				TArray<uint16> HeightValues;
				HeightValues.Reserve(HeightTexels.Num());
				for (const FColor& Texel : HeightTexels)
				{
					HeightValues.Add(static_cast<uint16>((static_cast<uint16>(Texel.R) << 8) | Texel.G));
				}

				Snapshot.bHeightReadable = true;
				Snapshot.HeightSampleCount = HeightValues.Num();
				Snapshot.HeightTexelBytesRead = static_cast<int64>(HeightTexels.Num()) * sizeof(FColor);
				Snapshot.HeightValueBytesHashed = static_cast<int64>(HeightValues.Num()) * sizeof(uint16);
				Snapshot.HeightTexelSha1 = HashBytesSha1(HeightTexels.GetData(), Snapshot.HeightTexelBytesRead);
				Snapshot.HeightValueSha1 = HashBytesSha1(HeightValues.GetData(), Snapshot.HeightValueBytesHashed);
				Map.HeightTexelBytesRead += Snapshot.HeightTexelBytesRead;
				Map.HeightValueBytesHashed += Snapshot.HeightValueBytesHashed;
				Snapshot.HeightValues = MoveTemp(HeightValues);
			}

			const ULandscapeComponent* ReadOnlyComponent = Component;
			const TArray<FWeightmapLayerAllocationInfo>& Allocations = ReadOnlyComponent->GetWeightmapLayerAllocations(false);
			const TArray<UTexture2D*>& WeightTextures = ReadOnlyComponent->GetWeightmapTextures(false);
			for (const FWeightmapLayerAllocationInfo& Allocation : Allocations)
			{
				Snapshot.WeightLayers.Add(ReadWeightLayer(
					Component, Allocation, WeightTextures, TextureMipCache,
					&DataInterface, Map, Snapshot, Snapshot.bWeightMipsReadable));
			}
		}
		else
		{
			AddComponentIssue(Map, Snapshot, FString::Printf(
				TEXT("Height source sample count %lld exceeds the bounded read limit."), HeightSampleCount));
		}
	}
	else
	{
		AddComponentIssue(Map, Snapshot, TEXT("Landscape heightmap texture source is unavailable."));
		Snapshot.bWeightsReadable = false;
		const ULandscapeComponent* ReadOnlyComponent = Component;
		const TArray<FWeightmapLayerAllocationInfo>& Allocations = ReadOnlyComponent->GetWeightmapLayerAllocations(false);
		const TArray<UTexture2D*>& WeightTextures = ReadOnlyComponent->GetWeightmapTextures(false);
		for (const FWeightmapLayerAllocationInfo& Allocation : Allocations)
		{
			Snapshot.WeightLayers.Add(ReadWeightLayer(
				Component, Allocation, WeightTextures, TextureMipCache,
				nullptr, Map, Snapshot, Snapshot.bWeightMipsReadable));
		}
	}

	for (FIMWeightLayerSnapshot& Layer : Snapshot.WeightLayers)
	{
		if (Layer.LayerName != TEXT("<INVALID_LAYER_INFO>") && Layer.LayerName != FName(NAME_None).ToString())
		{
			Snapshot.CollisionLayerNames.Add(Layer.LayerName);
		}
	}

	Map.WeightBytesRead += Snapshot.WeightBytesRead;
	if (Snapshot.bWeightMipsReadable)
	{
		++Map.WeightMipCompleteComponentCount;
	}
	if (Snapshot.bHeightReadable)
	{
		++Map.HeightCompleteComponentCount;
	}
	if (Snapshot.bWeightsReadable)
	{
		++Map.WeightCompleteComponentCount;
	}

	Map.LayerInfoAssetPaths.Append(Snapshot.CollisionLayerInfoPaths);
	Map.PhysicalMaterialPaths.Append(Snapshot.PhysicalMaterialPaths);
	Map.RenderMaterialPaths.Append(Snapshot.RenderMaterialPaths);
	Map.Components.Add(MoveTemp(Snapshot));
}

static void CaptureCollisionData(
	ULandscapeHeightfieldCollisionComponent* Collision,
	const TMap<const ULandscapeComponent*, int32>& ComponentIndexByPointer,
	FIMMapSnapshot& Map)
{
	ULandscapeComponent* RenderComponent = Collision->GetRenderComponent();
	if (!IsValid(RenderComponent))
	{
		AddIssue(Map, TEXT("A Landscape collision component has no valid render component."));
		return;
	}

	const int32* ComponentIndex = ComponentIndexByPointer.Find(RenderComponent);
	if (!ComponentIndex || !Map.Components.IsValidIndex(*ComponentIndex))
	{
		AddIssue(Map, TEXT("A Landscape collision component points outside the loaded render-component set."));
		return;
	}

	FIMComponentSnapshot& Snapshot = Map.Components[*ComponentIndex];
	if (Snapshot.bHasCollision)
	{
		AddComponentIssue(Map, Snapshot, TEXT("More than one collision component maps to the same Landscape component."));
		return;
	}
	Snapshot.bHasCollision = true;
	Snapshot.CollisionIdentityKey = MakeIdentityKey(Collision->GetSectionBase(), Collision->GetComponentTransform());
	Snapshot.CollisionSizeQuads = Collision->CollisionSizeQuads;
	Snapshot.SimpleCollisionSizeQuads = Collision->SimpleCollisionSizeQuads;
	Snapshot.CollisionScale = Collision->CollisionScale;

	if (Snapshot.CollisionIdentityKey != Snapshot.IdentityKey)
	{
		AddComponentIssue(Map, Snapshot, TEXT("Collision section base or transform does not match its render component."));
	}

	ALandscapeProxy* LandscapeProxy = Collision->GetLandscapeProxy();
	if (IsValid(LandscapeProxy) && IsValid(LandscapeProxy->DefaultPhysMaterial))
	{
		const FString PhysicalMaterialPath = LandscapeProxy->DefaultPhysMaterial->GetPathName();
		Snapshot.PhysicalMaterialPaths.Add(PhysicalMaterialPath);
		Map.PhysicalMaterialPaths.Add(PhysicalMaterialPath);
		Snapshot.bHasValidPhysicalMaterialInputs = true;
		Map.bHasValidPhysicalMaterialInputs = true;
	}

	Snapshot.CollisionLayerNames.Reset();
	Snapshot.CollisionLayerInfoPaths.Reset();
	for (ULandscapeLayerInfoObject* LayerInfo : Collision->ComponentLayerInfos)
	{
		if (IsValid(LayerInfo))
		{
			Snapshot.CollisionLayerNames.Add(LayerInfo->GetLayerName().ToString());
			Snapshot.CollisionLayerInfoPaths.Add(LayerInfo->GetPathName());
			Map.LayerInfoAssetPaths.Add(LayerInfo->GetPathName());
			if (UPhysicalMaterial* LayerPhysicalMaterial = LayerInfo->GetPhysicalMaterial().Get())
			{
				const FString PhysicalMaterialPath = LayerPhysicalMaterial->GetPathName();
				Snapshot.PhysicalMaterialPaths.Add(PhysicalMaterialPath);
				Map.PhysicalMaterialPaths.Add(PhysicalMaterialPath);
				Snapshot.bHasValidPhysicalMaterialInputs = true;
				Map.bHasValidPhysicalMaterialInputs = true;
			}
		}
		else
		{
			Snapshot.CollisionLayerNames.Add(TEXT("<INVALID_LAYER_INFO>"));
			AddComponentIssue(Map, Snapshot, TEXT("Collision component contains an invalid layer-info entry."));
		}
	}

	for (UPhysicalMaterial* PhysicalMaterial : Collision->PhysicalMaterialRenderObjects)
	{
		if (IsValid(PhysicalMaterial))
		{
			Snapshot.PhysicalMaterialPaths.Add(PhysicalMaterial->GetPathName());
			Map.PhysicalMaterialPaths.Add(PhysicalMaterial->GetPathName());
			Snapshot.bHasValidPhysicalMaterialInputs = true;
			Map.bHasValidPhysicalMaterialInputs = true;
		}
	}

	TArray<UMaterialInterface*> UsedMaterials;
	RenderComponent->GetUsedMaterials(UsedMaterials, false);
	for (UMaterialInterface* Material : UsedMaterials)
	{
		AddUniquePath(Snapshot.RenderMaterialPaths, Material);
		AddUniquePath(Map.RenderMaterialPaths, Material);
	}

	const int64 CollisionQuads = Collision->CollisionSizeQuads;
	const int64 SimpleCollisionQuads = Collision->SimpleCollisionSizeQuads;
	if (CollisionQuads <= 0 || CollisionQuads > 4096
		|| SimpleCollisionQuads < 0 || SimpleCollisionQuads > 4096)
	{
		AddComponentIssue(Map, Snapshot, TEXT("Collision sample dimensions are outside the bounded read limit."));
		return;
	}

	// UE 5.8 does not export GetCollisionSampleInfo from the Landscape module.
	// Mirror its sample layout using the public, already bounded dimensions.
	ULandscapeHeightfieldCollisionComponent::FCollisionSampleInfo SampleInfo;
	SampleInfo.CollisionSizeVerts = Collision->CollisionSizeQuads + 1;
	SampleInfo.SimpleCollisionSizeVerts = Collision->SimpleCollisionSizeQuads > 0
		? Collision->SimpleCollisionSizeQuads + 1 : 0;
	SampleInfo.NumSamples = FMath::Square(SampleInfo.CollisionSizeVerts);
	SampleInfo.NumSimpleSamples = FMath::Square(SampleInfo.SimpleCollisionSizeVerts);
	const int64 ExpectedHeightElements = static_cast<int64>(SampleInfo.NumSamples) + SampleInfo.NumSimpleSamples;
	if (!IsWithinSampleLimit(ExpectedHeightElements))
	{
		AddComponentIssue(Map, Snapshot, TEXT("Collision sample count is outside the bounded read limit."));
		return;
	}

	TArray<uint8> MaterialCells;
	FString MaterialCellReadError;
	if (!IMReadLandscapeMaterialCells(*Collision, MaterialCells, MaterialCellReadError,
			&Snapshot.StoredPhysicalMaterialIndexCount))
	{
		Snapshot.PhysicalMaterialCellStatus = TEXT("READ_FAILED");
		Snapshot.PhysicalMaterialCellError = MaterialCellReadError;
		AddComponentIssue(Map, Snapshot, FString::Printf(
			TEXT("Landscape physical material cell read failed: %s"), *MaterialCellReadError));
	}
	else
	{
		const int64 ExpectedCellCount = CollisionQuads * CollisionQuads;
		Snapshot.PhysicalMaterialCellCount = MaterialCells.Num();
		Snapshot.PhysicalMaterialCellBytesRead = MaterialCells.Num();
		TArray<uint8> HoleMask;
		HoleMask.SetNumUninitialized(MaterialCells.Num());
		for (int32 CellIndex = 0; CellIndex < MaterialCells.Num(); ++CellIndex)
		{
			const bool bIsHole = MaterialCells[CellIndex] == 255;
			HoleMask[CellIndex] = bIsHole ? 1 : 0;
			Snapshot.PhysicalMaterialHoleCellCount += bIsHole ? 1 : 0;
		}
		Snapshot.PhysicalMaterialCellSha1 = HashBytesSha1(
			MaterialCells.GetData(), Snapshot.PhysicalMaterialCellBytesRead);
		Snapshot.PhysicalMaterialHoleMaskSha1 = HashBytesSha1(HoleMask.GetData(), HoleMask.Num());
		Map.PhysicalMaterialCellCount += Snapshot.PhysicalMaterialCellCount;
		Map.PhysicalMaterialCellBytesRead += Snapshot.PhysicalMaterialCellBytesRead;
		Map.PhysicalMaterialHoleCellCount += Snapshot.PhysicalMaterialHoleCellCount;
		if (Snapshot.PhysicalMaterialCellCount != ExpectedCellCount)
		{
			Snapshot.PhysicalMaterialCellStatus = TEXT("CELL_COUNT_MISMATCH");
			Snapshot.PhysicalMaterialCellError = FString::Printf(
				TEXT("Shared Landscape cell reader returned %lld cells; expected %lld collision cells."),
				Snapshot.PhysicalMaterialCellCount, ExpectedCellCount);
			AddComponentIssue(Map, Snapshot, Snapshot.PhysicalMaterialCellError);
		}
		else
		{
			Snapshot.bPhysicalMaterialCellsReadable = true;
			Snapshot.PhysicalMaterialCellStatus = TEXT("READ");
			++Map.PhysicalMaterialCellCompleteComponentCount;
		}
	}

	Snapshot.CollisionHeightData = ReadRawBulkData(Collision->CollisionHeightData, sizeof(uint16));
	Map.CollisionHeightBytesRead += Snapshot.CollisionHeightData.ByteCount;
	const bool bCollisionHeightComplete = Snapshot.CollisionHeightData.bReadable
		&& Snapshot.CollisionHeightData.bPresent
		&& Snapshot.CollisionHeightData.ElementCount == ExpectedHeightElements;
	if (!bCollisionHeightComplete)
	{
		AddComponentIssue(Map, Snapshot, FString::Printf(
			TEXT("CollisionHeightData status=%s elements=%lld; expected %lld."),
			*Snapshot.CollisionHeightData.Status,
			Snapshot.CollisionHeightData.ElementCount, ExpectedHeightElements));
	}
	else
	{
		++Map.CollisionHeightCompleteComponentCount;
	}

	Snapshot.DominantLayerData = ReadRawBulkData(Collision->DominantLayerData, sizeof(uint8),
		&Snapshot.UsedDominantLayerIndices);
	Map.DominantLayerBytesRead += Snapshot.DominantLayerData.ByteCount;
	if (!Snapshot.DominantLayerData.bReadable)
	{
		AddComponentIssue(Map, Snapshot, FString::Printf(
			TEXT("DominantLayerData could not be read: %s."), *Snapshot.DominantLayerData.Status));
	}
	else if (Snapshot.DominantLayerData.bPresent)
	{
		if (Snapshot.DominantLayerData.ElementCount != ExpectedHeightElements)
		{
			AddComponentIssue(Map, Snapshot, FString::Printf(
				TEXT("DominantLayerData has %lld elements; expected %lld."),
				Snapshot.DominantLayerData.ElementCount, ExpectedHeightElements));
		}
		++Map.DominantLayerBulkPresentCount;
	}
	else
	{
		++Map.DominantLayerBulkEmptyCount;
	}

	Snapshot.CollisionQuadFlagsBytes = Collision->CollisionQuadFlags.Num();
	Snapshot.CollisionQuadFlagsSha1 = HashBytesSha1(
		Collision->CollisionQuadFlags.GetData(), Snapshot.CollisionQuadFlagsBytes);
	Map.CollisionQuadFlagsBytesRead += Snapshot.CollisionQuadFlagsBytes;

	TArray<float> RuntimeHeights;
	RuntimeHeights.Init(std::numeric_limits<float>::quiet_NaN(), SampleInfo.NumSamples);
	if (SampleInfo.NumSamples > 0
		&& Collision->FillHeightTile(MakeArrayView(RuntimeHeights), 0, SampleInfo.CollisionSizeVerts))
	{
		bool bAllRuntimeHeightsFinite = true;
		for (const float Height : RuntimeHeights)
		{
			if (!FMath::IsFinite(Height))
			{
				bAllRuntimeHeightsFinite = false;
				break;
			}
		}
		if (bAllRuntimeHeightsFinite)
		{
			Snapshot.bRuntimeCollisionHeightReadable = true;
			Snapshot.RuntimeCollisionHeightSampleCount = RuntimeHeights.Num();
			Snapshot.RuntimeCollisionHeightBytesRead = static_cast<int64>(RuntimeHeights.Num()) * sizeof(float);
			Snapshot.RuntimeCollisionHeightSha1 = HashBytesSha1(
				RuntimeHeights.GetData(), Snapshot.RuntimeCollisionHeightBytesRead);
			Map.RuntimeCollisionHeightBytesRead += Snapshot.RuntimeCollisionHeightBytesRead;
			Snapshot.RuntimeCollisionHeightValues = MoveTemp(RuntimeHeights);
		}
	}
}

static void CollectLandscapeWorldComponents(
	UWorld* World,
	TArray<ULandscapeComponent*>& OutLandscapeComponents,
	TArray<ULandscapeHeightfieldCollisionComponent*>& OutCollisionComponents);

static FIMMapSnapshot CaptureMapSnapshot(
	const FString& RequestedPackage, const bool bLoadMapReturned,
	UWorld* SnapshotWorld = nullptr, const bool bInMemoryDuplicate = false)
{
	FIMMapSnapshot Map;
	Map.RequestedPackage = RequestedPackage;
	Map.bLoadMapReturned = bLoadMapReturned;

	UWorld* World = SnapshotWorld ? SnapshotWorld : (GEditor ? GEditor->GetEditorWorldContext().World() : nullptr);
	Map.CaptureKind = bInMemoryDuplicate ? TEXT("IN_MEMORY_DUPLICATE_NOT_LOADED") : TEXT("EDITOR_MAP_LOAD");
	Map.LoadedPackage = IsValid(World) ? World->GetOutermost()->GetName() : FString();
	Map.bWorldIdentityMatches = Map.LoadedPackage == RequestedPackage;
	if ((!bLoadMapReturned && !bInMemoryDuplicate) || !Map.bWorldIdentityMatches || !IsValid(World) || !World->PersistentLevel)
	{
		AddIssue(Map, FString::Printf(
			TEXT("Map load or world identity failed: returned=%d loaded=%s expected=%s."),
			bLoadMapReturned ? 1 : 0, *Map.LoadedPackage, *RequestedPackage));
		return Map;
	}

	Map.bNonWorldPartition = !World->IsPartitionedWorld();
	if (!Map.bNonWorldPartition)
	{
		AddIssue(Map, TEXT("Target map is World Partition; the test requires a persistent non-World-Partition map."));
		return Map;
	}

	Map.StreamingLevelCount = World->GetStreamingLevels().Num();
	for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
	{
		if (!IsValid(StreamingLevel) || StreamingLevel->GetWorldAssetPackageFName().IsNone())
		{
			continue;
		}
		if (StreamingLevel->GetLoadedLevel())
		{
			++Map.LoadedStreamingLevelCount;
		}
		else
		{
			Map.bLevelCoverageComplete = false;
			AddIssue(Map, FString::Printf(
				TEXT("Referenced streaming level %s is not loaded; complete Landscape coverage cannot be proven."),
				*StreamingLevel->GetWorldAssetPackageFName().ToString()));
		}
	}

	TArray<ULandscapeComponent*> LandscapeComponents;
	TArray<ULandscapeHeightfieldCollisionComponent*> CollisionComponents;
	CollectLandscapeWorldComponents(World, LandscapeComponents, CollisionComponents);

	Map.LandscapeComponentCount = LandscapeComponents.Num();
	Map.CollisionComponentCount = CollisionComponents.Num();
	Map.bHasLandscape = Map.LandscapeComponentCount > 0;
	if (!Map.bHasLandscape)
	{
		AddIssue(Map, TEXT("No Landscape components were found in the loaded world."));
	}

	TMap<const ULandscapeComponent*, int32> ComponentIndexByPointer;
	ComponentIndexByPointer.Reserve(LandscapeComponents.Num());
	TMap<const UTexture2D*, FIMTextureMipChainSnapshot> TextureMipCache;
	TextureMipCache.Reserve(LandscapeComponents.Num());
	for (int32 Index = 0; Index < LandscapeComponents.Num(); ++Index)
	{
		if (!IsValid(LandscapeComponents[Index]))
		{
			AddIssue(Map, TEXT("The loaded world contains an invalid Landscape component."));
			continue;
		}
		CaptureComponentData(LandscapeComponents[Index], Index, Map,
			ComponentIndexByPointer, TextureMipCache);
	}

	TSet<const ULandscapeComponent*> CollisionPairedComponents;
	for (ULandscapeHeightfieldCollisionComponent* Collision : CollisionComponents)
	{
		if (IsValid(Collision))
		{
			ULandscapeComponent* RenderComponent = Collision->GetRenderComponent();
			if (IsValid(RenderComponent) && CollisionPairedComponents.Contains(RenderComponent))
			{
				AddIssue(Map, TEXT("Two collision components reference the same Landscape render component."));
				continue;
			}
			if (IsValid(RenderComponent))
			{
				CollisionPairedComponents.Add(RenderComponent);
			}
			CaptureCollisionData(Collision, ComponentIndexByPointer, Map);
		}
		else
		{
			AddIssue(Map, TEXT("The loaded world contains an invalid Landscape collision component."));
		}
	}

	for (FIMComponentSnapshot& Component : Map.Components)
	{
		if (!Component.bHasCollision)
		{
			AddComponentIssue(Map, Component, TEXT("No matching Landscape collision component was found."));
		}
		Component.WeightLayers.Sort([](const FIMWeightLayerSnapshot& A, const FIMWeightLayerSnapshot& B)
		{
			return A.LayerName < B.LayerName;
		});
		SortUniquePaths(Component.PhysicalMaterialPaths);
		SortUniquePaths(Component.RenderMaterialPaths);
		SortUniquePaths(Component.WeightTexturePaths);
	}

	Map.TextureSourceMipTextureCount = TextureMipCache.Num();
	for (const auto& MipEntry : TextureMipCache)
	{
		Map.TextureSourceMipCount += MipEntry.Value.Mips.Num();
		Map.TextureSourceMipBytesRead += MipEntry.Value.ByteCount;
		if (!MipEntry.Value.bReadable)
		{
			++Map.TextureSourceMipReadFailureCount;
		}
	}

	if (Map.CollisionComponentCount != Map.LandscapeComponentCount)
	{
		AddIssue(Map, FString::Printf(
			TEXT("Render/collision component counts differ: render=%d collision=%d."),
			Map.LandscapeComponentCount, Map.CollisionComponentCount));
	}
	if (Map.HeightCompleteComponentCount != Map.LandscapeComponentCount
		|| Map.WeightCompleteComponentCount != Map.LandscapeComponentCount
		|| Map.HeightMipCompleteComponentCount != Map.LandscapeComponentCount
		|| Map.WeightMipCompleteComponentCount != Map.LandscapeComponentCount
		|| Map.CollisionHeightCompleteComponentCount != Map.LandscapeComponentCount
		|| Map.PhysicalMaterialCellCompleteComponentCount != Map.LandscapeComponentCount)
	{
		AddIssue(Map, TEXT("At least one Landscape component did not provide complete height vertices, weight layers, all source mips, collision heights, or physical material cells."));
	}

	Map.Components.Sort([](const FIMComponentSnapshot& A, const FIMComponentSnapshot& B)
	{
		return A.IdentityKey < B.IdentityKey;
	});
	for (int32 Index = 1; Index < Map.Components.Num(); ++Index)
	{
		if (Map.Components[Index - 1].IdentityKey == Map.Components[Index].IdentityKey)
		{
			AddIssue(Map, FString::Printf(
				TEXT("Duplicate stable Landscape identity key: %s."), *Map.Components[Index].IdentityKey));
		}
	}

	SortUniquePaths(Map.LayerInfoAssetPaths);
	SortUniquePaths(Map.PhysicalMaterialPaths);
	SortUniquePaths(Map.RenderMaterialPaths);
	SortUniquePaths(Map.HeightmapTexturePaths);
	SortUniquePaths(Map.WeightTexturePaths);
	Map.bCoverageComplete = Map.bCoverageComplete && Map.bLevelCoverageComplete;
	return Map;
}

static void AddDifference(FIMComparison& Comparison, const FString& Difference)
{
	++Comparison.HardMismatchCount;
	if (Comparison.Differences.Num() < MaxDifferenceDetails)
	{
		Comparison.Differences.Add(Difference);
	}
}

static bool EqualUsedCollisionLayerNames(const FIMComponentSnapshot& A, const FIMComponentSnapshot& B)
{
	if (!A.DominantLayerData.bReadable || !B.DominantLayerData.bReadable) return false;
	if (!A.DominantLayerData.bPresent && !B.DominantLayerData.bPresent) return true;
	// Even the empty-source exception requires an internally valid rebuilt cache.
	// Every byte read from a present target DLD must resolve to an actual layer.
	if (B.DominantLayerData.bPresent)
	{
		if (B.UsedDominantLayerIndices.IsEmpty()) return false;
		for (const uint8 Index : B.UsedDominantLayerIndices)
		{
			if (!B.CollisionLayerNames.IsValidIndex(Index)
				|| B.CollisionLayerNames[Index].IsEmpty()
				|| B.CollisionLayerNames[Index] == TEXT("<INVALID_LAYER_INFO>")
				|| B.CollisionLayerNames[Index] == FName(NAME_None).ToString()) return false;
		}
	}
	// Preserve the existing narrowly defined empty-source PM-cache exception
	// after validating the target; this does not waive malformed DLD mappings.
	if (!A.DominantLayerData.bPresent && B.DominantLayerData.bPresent
		&& !A.bHasValidPhysicalMaterialInputs) return true;
	if (!A.DominantLayerData.bPresent || !B.DominantLayerData.bPresent
		|| A.UsedDominantLayerIndices.IsEmpty()
		|| A.UsedDominantLayerIndices != B.UsedDominantLayerIndices) return false;
	for (const uint8 Index : A.UsedDominantLayerIndices)
	{
		if (!A.CollisionLayerNames.IsValidIndex(Index) || !B.CollisionLayerNames.IsValidIndex(Index)
			|| A.CollisionLayerNames[Index] == TEXT("<INVALID_LAYER_INFO>")
			|| A.CollisionLayerNames[Index] != B.CollisionLayerNames[Index]) return false;
	}
	// UpdateCollisionLayerData can prune unreferenced candidate-layer entries.
	// Raw DLD equality remains a separate mandatory gate below; each used byte
	// must also resolve to the same named layer, never an unrelated array slot.
	return true;
}

static const FIMWeightLayerSnapshot* FindWeightLayer(const TArray<FIMWeightLayerSnapshot>& Layers, const FString& LayerName)
{
	return Layers.FindByPredicate([&LayerName](const FIMWeightLayerSnapshot& Layer)
	{
		return Layer.LayerName == LayerName;
	});
}

static bool AreTextureMipChainsEqual(
	const FIMTextureMipChainSnapshot& A,
	const FIMTextureMipChainSnapshot& B)
{
	if (!A.bReadable || !B.bReadable
		|| A.BlockCount != B.BlockCount || A.LayerCount != B.LayerCount || A.MipCount != B.MipCount
		|| A.ByteCount != B.ByteCount || A.Mips.Num() != B.Mips.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < A.Mips.Num(); ++Index)
	{
		const FIMTextureMipSnapshot& MipA = A.Mips[Index];
		const FIMTextureMipSnapshot& MipB = B.Mips[Index];
		if (MipA.BlockIndex != MipB.BlockIndex || MipA.LayerIndex != MipB.LayerIndex
			|| MipA.MipIndex != MipB.MipIndex || MipA.Width != MipB.Width || MipA.Height != MipB.Height
			|| MipA.NumSlices != MipB.NumSlices || MipA.SourceFormat != MipB.SourceFormat
			|| MipA.ByteCount != MipB.ByteCount || MipA.Sha1 != MipB.Sha1)
		{
			return false;
		}
	}
	return true;
}

template <typename SampleType>
static TSharedRef<FJsonObject> MakeSampleDeltaJson(
	const TArray<SampleType>& Source,
	const TArray<SampleType>& Preview,
	const int32 RowStride,
	const TCHAR* ValueUnits)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("source_count"), Source.Num());
	Json->SetNumberField(TEXT("preview_count"), Preview.Num());
	Json->SetNumberField(TEXT("row_stride"), RowStride);
	Json->SetStringField(TEXT("value_units"), ValueUnits);
	const bool bComparable = Source.Num() > 0 && Source.Num() == Preview.Num() && RowStride > 0;
	Json->SetBoolField(TEXT("comparable"), bComparable);
	if (!bComparable) return Json;

	int32 ChangedCount = 0;
	double MaxAbsDelta = 0.0;
	double SumAbsDelta = 0.0;
	FIntPoint MinSample(MAX_int32, MAX_int32);
	FIntPoint MaxSample(-1, -1);
	TArray<TSharedPtr<FJsonValue>> Examples;
	for (int32 Index = 0; Index < Source.Num(); ++Index)
	{
		const double SourceValue = static_cast<double>(Source[Index]);
		const double PreviewValue = static_cast<double>(Preview[Index]);
		const double Delta = PreviewValue - SourceValue;
		if (Delta == 0.0) continue;
		++ChangedCount;
		MaxAbsDelta = FMath::Max(MaxAbsDelta, FMath::Abs(Delta));
		SumAbsDelta += FMath::Abs(Delta);
		const FIntPoint Sample(Index % RowStride, Index / RowStride);
		MinSample.X = FMath::Min(MinSample.X, Sample.X);
		MinSample.Y = FMath::Min(MinSample.Y, Sample.Y);
		MaxSample.X = FMath::Max(MaxSample.X, Sample.X);
		MaxSample.Y = FMath::Max(MaxSample.Y, Sample.Y);
		if (Examples.Num() < 8)
		{
			TSharedRef<FJsonObject> Example = MakeShared<FJsonObject>();
			Example->SetNumberField(TEXT("index"), Index);
			Example->SetNumberField(TEXT("x"), Sample.X);
			Example->SetNumberField(TEXT("y"), Sample.Y);
			Example->SetNumberField(TEXT("source"), SourceValue);
			Example->SetNumberField(TEXT("preview"), PreviewValue);
			Example->SetNumberField(TEXT("preview_minus_source"), Delta);
			Examples.Add(MakeShared<FJsonValueObject>(Example));
		}
	}
	Json->SetNumberField(TEXT("changed_sample_count"), ChangedCount);
	Json->SetNumberField(TEXT("max_absolute_delta"), MaxAbsDelta);
	Json->SetNumberField(TEXT("mean_absolute_delta_over_all_samples"), SumAbsDelta / Source.Num());
	if (ChangedCount > 0)
	{
		Json->SetNumberField(TEXT("min_changed_x"), MinSample.X);
		Json->SetNumberField(TEXT("min_changed_y"), MinSample.Y);
		Json->SetNumberField(TEXT("max_changed_x"), MaxSample.X);
		Json->SetNumberField(TEXT("max_changed_y"), MaxSample.Y);
	}
	Json->SetArrayField(TEXT("first_changed_samples"), Examples);
	return Json;
}

static void CompareMatchedComponent(
	const FIMComponentSnapshot& Source,
	const FIMComponentSnapshot& Preview,
	FIMComparison& Comparison)
{
	++Comparison.MatchedComponentCount;
	TSharedRef<FJsonObject> SampleDelta = MakeShared<FJsonObject>();
	SampleDelta->SetStringField(TEXT("identity_key"), Source.IdentityKey);
	const int32 RenderRowStride = (Source.SubsectionSizeQuads + 1) * Source.NumSubsections;
	SampleDelta->SetObjectField(TEXT("raw_height"), MakeSampleDeltaJson(
		Source.HeightValues, Preview.HeightValues, RenderRowStride, TEXT("encoded_uint16")));
	SampleDelta->SetObjectField(TEXT("runtime_collision_height"), MakeSampleDeltaJson(
		Source.RuntimeCollisionHeightValues, Preview.RuntimeCollisionHeightValues,
		Source.CollisionSizeQuads + 1, TEXT("world_z_cm")));
	if (Source.ComponentSizeQuads != Preview.ComponentSizeQuads
		|| Source.NumSubsections != Preview.NumSubsections
		|| Source.SubsectionSizeQuads != Preview.SubsectionSizeQuads
		|| Source.CollisionSizeQuads != Preview.CollisionSizeQuads
		|| Source.SimpleCollisionSizeQuads != Preview.SimpleCollisionSizeQuads
		|| Source.CollisionScale != Preview.CollisionScale
		|| !EqualUsedCollisionLayerNames(Source, Preview))
	{
		++Comparison.TopologyMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Topology/layer mapping differs at %s."), *Source.IdentityKey));
	}

	if (Source.CollisionIdentityKey != Preview.CollisionIdentityKey)
	{
		++Comparison.CollisionTransformMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Collision transform or section base differs at %s."), *Source.IdentityKey));
	}

	if (!Source.bHeightReadable || !Preview.bHeightReadable
		|| Source.HeightSampleCount != Preview.HeightSampleCount
		|| Source.HeightValueSha1 != Preview.HeightValueSha1)
	{
		++Comparison.HeightMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Raw height values differ or are unreadable at %s."), *Source.IdentityKey));
	}

	if (!AreTextureMipChainsEqual(Source.HeightmapMipChain, Preview.HeightmapMipChain))
	{
		++Comparison.HeightMipMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Heightmap source mip chains differ or are unreadable at %s."), *Source.IdentityKey));
	}

	if (!Source.CollisionHeightData.bReadable || !Preview.CollisionHeightData.bReadable
		|| Source.CollisionHeightData.ElementCount != Preview.CollisionHeightData.ElementCount
		|| Source.CollisionHeightData.Sha1 != Preview.CollisionHeightData.Sha1)
	{
		++Comparison.CollisionHeightMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Raw collision heights differ or are unreadable at %s."), *Source.IdentityKey));
	}

	if (!Source.bRuntimeCollisionHeightReadable || !Preview.bRuntimeCollisionHeightReadable
		|| Source.RuntimeCollisionHeightSampleCount != Preview.RuntimeCollisionHeightSampleCount
		|| Source.RuntimeCollisionHeightSha1 != Preview.RuntimeCollisionHeightSha1)
	{
		++Comparison.RuntimeCollisionHeightMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Independent runtime collision heights differ or are unreadable at %s."), *Source.IdentityKey));
	}

	if (Source.CollisionQuadFlagsBytes != Preview.CollisionQuadFlagsBytes
		|| Source.CollisionQuadFlagsSha1 != Preview.CollisionQuadFlagsSha1)
	{
		++Comparison.CollisionFlagMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Collision quad topology flags differ at %s."), *Source.IdentityKey));
	}

	bool bWeightsEqual = Source.WeightLayers.Num() == Preview.WeightLayers.Num()
		&& Source.bWeightsReadable && Preview.bWeightsReadable;
	bool bWeightMipChainsEqual = Source.WeightLayers.Num() == Preview.WeightLayers.Num();
	TArray<TSharedPtr<FJsonValue>> WeightDeltaRows;
	for (const FIMWeightLayerSnapshot& SourceLayer : Source.WeightLayers)
	{
		const FIMWeightLayerSnapshot* PreviewLayer = FindWeightLayer(Preview.WeightLayers, SourceLayer.LayerName);
		if (!PreviewLayer)
		{
			bWeightsEqual = false;
			bWeightMipChainsEqual = false;
			continue;
		}

		TSharedRef<FJsonObject> WeightDelta = MakeSampleDeltaJson(
			SourceLayer.WeightValues, PreviewLayer->WeightValues, RenderRowStride, TEXT("uint8_weight"));
		WeightDelta->SetStringField(TEXT("layer_name"), SourceLayer.LayerName);
		WeightDeltaRows.Add(MakeShared<FJsonValueObject>(WeightDelta));

		if (SourceLayer.bAllocated != PreviewLayer->bAllocated
			|| !SourceLayer.bReadable || !PreviewLayer->bReadable
			|| SourceLayer.SampleCount != PreviewLayer->SampleCount
			|| SourceLayer.Sha1 != PreviewLayer->Sha1)
		{
			bWeightsEqual = false;
		}

		if (SourceLayer.bAllocated != PreviewLayer->bAllocated
			|| (SourceLayer.bAllocated
				&& !AreTextureMipChainsEqual(SourceLayer.TextureMipChain, PreviewLayer->TextureMipChain)))
		{
			bWeightMipChainsEqual = false;
		}
	}
	if (!bWeightsEqual)
	{
		++Comparison.WeightMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Layer weight names, allocation, counts, or raw bytes differ at %s."), *Source.IdentityKey));
	}
	if (!bWeightMipChainsEqual)
	{
		++Comparison.WeightMipMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Weightmap source mip chains differ or are unreadable at %s."), *Source.IdentityKey));
	}
	SampleDelta->SetArrayField(TEXT("weight_layers"), WeightDeltaRows);
	Comparison.SampleDeltaDetails.Add(MakeShared<FJsonValueObject>(SampleDelta));

	const bool bPhysicalMaterialCellTilesEqual = Source.bPhysicalMaterialCellsReadable
		&& Preview.bPhysicalMaterialCellsReadable
		&& Source.PhysicalMaterialCellCount == Preview.PhysicalMaterialCellCount
		&& Source.PhysicalMaterialCellSha1 == Preview.PhysicalMaterialCellSha1;
	if (!bPhysicalMaterialCellTilesEqual)
	{
		++Comparison.PhysicalMaterialCellMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Actual Landscape physical-material cell IDs differ or are unreadable at %s."), *Source.IdentityKey));
	}

	const bool bPhysicalMaterialHoleMasksEqual = Source.bPhysicalMaterialCellsReadable
		&& Preview.bPhysicalMaterialCellsReadable
		&& Source.PhysicalMaterialCellCount == Preview.PhysicalMaterialCellCount
		&& Source.PhysicalMaterialHoleCellCount == Preview.PhysicalMaterialHoleCellCount
		&& Source.PhysicalMaterialHoleMaskSha1 == Preview.PhysicalMaterialHoleMaskSha1;
	if (!bPhysicalMaterialHoleMasksEqual)
	{
		++Comparison.PhysicalMaterialHoleMaskMismatchCount;
		AddDifference(Comparison, FString::Printf(
			TEXT("Actual collision-cell hole masks differ or are unreadable at %s."), *Source.IdentityKey));
	}

	const bool bDominantLayerDataEqual = Source.DominantLayerData.bReadable
		&& Preview.DominantLayerData.bReadable
		&& Source.DominantLayerData.ElementCount == Preview.DominantLayerData.ElementCount
		&& Source.DominantLayerData.Sha1 == Preview.DominantLayerData.Sha1;
	if (!bDominantLayerDataEqual)
	{
		const bool bPmCacheRebuiltFromEmptySource = Source.DominantLayerData.bReadable
			&& Preview.DominantLayerData.bReadable
			&& !Source.DominantLayerData.bPresent
			&& Preview.DominantLayerData.bPresent
			&& !Source.bHasValidPhysicalMaterialInputs;
		if (bPmCacheRebuiltFromEmptySource)
		{
			++Comparison.DominantLayerPmCacheExceptionCount;
			Comparison.Differences.Add(FString::Printf(
				TEXT("DominantLayerData became non-empty at %s; source had no valid Landscape physical-material inputs, so this is reported as a derived PM cache rebuild."),
				*Source.IdentityKey));
		}
		else
		{
			++Comparison.DominantLayerMismatchCount;
			AddDifference(Comparison, FString::Printf(
				TEXT("DominantLayerData differs outside the empty-source PM-cache exception at %s."), *Source.IdentityKey));
		}
	}
}

static FIMComparison CompareSnapshots(const FIMMapSnapshot& Source, const FIMMapSnapshot& Preview)
{
	FIMComparison Comparison;
	TMap<FString, const FIMComponentSnapshot*> PreviewByKey;
	for (const FIMComponentSnapshot& Component : Preview.Components)
	{
		PreviewByKey.Add(Component.IdentityKey, &Component);
	}

	TSet<FString> MatchedKeys;
	for (const FIMComponentSnapshot& SourceComponent : Source.Components)
	{
		const FIMComponentSnapshot* const* PreviewComponent = PreviewByKey.Find(SourceComponent.IdentityKey);
		if (!PreviewComponent)
		{
			++Comparison.SourceOnlyComponentCount;
			AddDifference(Comparison, FString::Printf(
				TEXT("Source Landscape component is absent from preview: %s."), *SourceComponent.IdentityKey));
			continue;
		}

		MatchedKeys.Add(SourceComponent.IdentityKey);
		CompareMatchedComponent(SourceComponent, **PreviewComponent, Comparison);
	}

	for (const FIMComponentSnapshot& PreviewComponent : Preview.Components)
	{
		if (!MatchedKeys.Contains(PreviewComponent.IdentityKey))
		{
			++Comparison.PreviewOnlyComponentCount;
			AddDifference(Comparison, FString::Printf(
				TEXT("Preview Landscape component is absent from source: %s."), *PreviewComponent.IdentityKey));
		}
	}
	return Comparison;
}

static TArray<FString> GetOnlyPaths(const TArray<FString>& A, const TArray<FString>& B)
{
	TArray<FString> Result;
	for (const FString& Path : A)
	{
		if (!B.Contains(Path))
		{
			Result.Add(Path);
		}
	}
	return Result;
}

static TSharedRef<FJsonObject> MakeRawDataJson(const FIMRawDataSummary& Data)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("readable"), Data.bReadable);
	Json->SetBoolField(TEXT("present"), Data.bPresent);
	Json->SetNumberField(TEXT("element_count"), static_cast<double>(Data.ElementCount));
	Json->SetNumberField(TEXT("bytes_read"), static_cast<double>(Data.ByteCount));
	Json->SetStringField(TEXT("sha1"), Data.Sha1.IsEmpty() ? TEXT("UNAVAILABLE") : Data.Sha1);
	Json->SetStringField(TEXT("status"), Data.Status);
	return Json;
}

static TSharedRef<FJsonObject> MakeTextureMipChainJson(const FIMTextureMipChainSnapshot& Chain)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("readable"), Chain.bReadable);
	Json->SetNumberField(TEXT("block_count"), Chain.BlockCount);
	Json->SetNumberField(TEXT("layer_count"), Chain.LayerCount);
	Json->SetNumberField(TEXT("mip_count"), Chain.MipCount);
	Json->SetNumberField(TEXT("bytes_read"), static_cast<double>(Chain.ByteCount));
	Json->SetStringField(TEXT("status"), Chain.Status);
	Json->SetStringField(TEXT("error"), Chain.Error);

	TArray<TSharedPtr<FJsonValue>> MipJson;
	for (const FIMTextureMipSnapshot& Mip : Chain.Mips)
	{
		TSharedRef<FJsonObject> MipObject = MakeShared<FJsonObject>();
		MipObject->SetNumberField(TEXT("block_index"), Mip.BlockIndex);
		MipObject->SetNumberField(TEXT("layer_index"), Mip.LayerIndex);
		MipObject->SetNumberField(TEXT("mip_index"), Mip.MipIndex);
		MipObject->SetNumberField(TEXT("width"), Mip.Width);
		MipObject->SetNumberField(TEXT("height"), Mip.Height);
		MipObject->SetNumberField(TEXT("slice_count"), Mip.NumSlices);
		MipObject->SetNumberField(TEXT("source_format"), Mip.SourceFormat);
		MipObject->SetNumberField(TEXT("bytes_read"), static_cast<double>(Mip.ByteCount));
		MipObject->SetStringField(TEXT("sha1"), Mip.Sha1.IsEmpty() ? TEXT("UNAVAILABLE") : Mip.Sha1);
		MipJson.Add(MakeShared<FJsonValueObject>(MipObject));
	}
	Json->SetArrayField(TEXT("mips"), MipJson);
	return Json;
}

static TSharedRef<FJsonObject> MakeWeightLayerJson(const FIMWeightLayerSnapshot& Layer)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("layer_name"), Layer.LayerName);
	Json->SetStringField(TEXT("layer_info_asset_path"), Layer.LayerInfoAssetPath);
	Json->SetStringField(TEXT("weight_texture_path"), Layer.WeightTexturePath);
	Json->SetBoolField(TEXT("allocated"), Layer.bAllocated);
	Json->SetBoolField(TEXT("readable"), Layer.bReadable);
	Json->SetNumberField(TEXT("sample_count"), static_cast<double>(Layer.SampleCount));
	Json->SetStringField(TEXT("sample_count_unit"), TEXT("heightmap_source_texels"));
	Json->SetNumberField(TEXT("bytes_read"), static_cast<double>(Layer.ByteCount));
	Json->SetStringField(TEXT("sha1"), Layer.Sha1.IsEmpty() ? TEXT("UNAVAILABLE") : Layer.Sha1);
	Json->SetArrayField(TEXT("physical_material_paths"), ToJsonStringArray(Layer.PhysicalMaterialPaths));
	Json->SetObjectField(TEXT("texture_source_mip_chain"), MakeTextureMipChainJson(Layer.TextureMipChain));
	Json->SetStringField(TEXT("status"), Layer.Status);
	return Json;
}

static TSharedRef<FJsonObject> MakeComponentJson(const FIMComponentSnapshot& Component)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("identity_key_section_base_and_transform"), Component.IdentityKey);
	Json->SetNumberField(TEXT("section_base_x"), Component.SectionBase.X);
	Json->SetNumberField(TEXT("section_base_y"), Component.SectionBase.Y);
	Json->SetObjectField(TEXT("world_location"), MakeVectorJson(Component.Location));
	Json->SetObjectField(TEXT("world_rotation_quaternion"), MakeRotationJson(Component.Rotation));
	Json->SetObjectField(TEXT("world_scale"), MakeVectorJson(Component.Scale));
	Json->SetNumberField(TEXT("component_size_quads"), Component.ComponentSizeQuads);
	Json->SetNumberField(TEXT("num_subsections"), Component.NumSubsections);
	Json->SetNumberField(TEXT("subsection_size_quads"), Component.SubsectionSizeQuads);
	Json->SetStringField(TEXT("heightmap_texture_path"), Component.HeightmapTexturePath);
	Json->SetBoolField(TEXT("height_readable"), Component.bHeightReadable);
	Json->SetNumberField(TEXT("height_sample_count"), static_cast<double>(Component.HeightSampleCount));
	Json->SetStringField(TEXT("height_sample_count_unit"), TEXT("heightmap_source_texels"));
	Json->SetNumberField(TEXT("heightmap_texel_bytes_read"), static_cast<double>(Component.HeightTexelBytesRead));
	Json->SetNumberField(TEXT("height_value_bytes_hashed"), static_cast<double>(Component.HeightValueBytesHashed));
	Json->SetStringField(TEXT("heightmap_texel_sha1"), Component.HeightTexelSha1.IsEmpty() ? TEXT("UNAVAILABLE") : Component.HeightTexelSha1);
	Json->SetStringField(TEXT("height_value_sha1"), Component.HeightValueSha1.IsEmpty() ? TEXT("UNAVAILABLE") : Component.HeightValueSha1);
	Json->SetObjectField(TEXT("heightmap_source_mip_chain"), MakeTextureMipChainJson(Component.HeightmapMipChain));

	TArray<TSharedPtr<FJsonValue>> WeightLayerJson;
	for (const FIMWeightLayerSnapshot& Layer : Component.WeightLayers)
	{
		WeightLayerJson.Add(MakeShared<FJsonValueObject>(MakeWeightLayerJson(Layer)));
	}
	Json->SetArrayField(TEXT("weight_layers"), WeightLayerJson);
	Json->SetBoolField(TEXT("weights_readable"), Component.bWeightsReadable);
	Json->SetBoolField(TEXT("weight_source_mips_readable"), Component.bWeightMipsReadable);
	Json->SetNumberField(TEXT("weight_bytes_read"), static_cast<double>(Component.WeightBytesRead));

	Json->SetBoolField(TEXT("collision_component_present"), Component.bHasCollision);
	Json->SetStringField(TEXT("collision_identity_key_section_base_and_transform"), Component.CollisionIdentityKey);
	Json->SetNumberField(TEXT("collision_size_quads"), Component.CollisionSizeQuads);
	Json->SetNumberField(TEXT("simple_collision_size_quads"), Component.SimpleCollisionSizeQuads);
	Json->SetNumberField(TEXT("collision_scale"), Component.CollisionScale);
	Json->SetBoolField(TEXT("physical_material_cells_readable"), Component.bPhysicalMaterialCellsReadable);
	Json->SetNumberField(TEXT("stored_physical_material_index_count"), Component.StoredPhysicalMaterialIndexCount);
	Json->SetNumberField(TEXT("physical_material_cell_count"), static_cast<double>(Component.PhysicalMaterialCellCount));
	Json->SetStringField(TEXT("physical_material_cell_count_unit"), TEXT("collision_cells"));
	Json->SetNumberField(TEXT("physical_material_cell_bytes_read"), static_cast<double>(Component.PhysicalMaterialCellBytesRead));
	Json->SetStringField(TEXT("physical_material_cell_sha1"),
		Component.PhysicalMaterialCellSha1.IsEmpty() ? TEXT("UNAVAILABLE") : Component.PhysicalMaterialCellSha1);
	Json->SetNumberField(TEXT("physical_material_hole_cell_count"), static_cast<double>(Component.PhysicalMaterialHoleCellCount));
	Json->SetStringField(TEXT("physical_material_hole_cell_count_unit"), TEXT("collision_cells"));
	Json->SetStringField(TEXT("physical_material_cell_hole_mask_sha1"),
		Component.PhysicalMaterialHoleMaskSha1.IsEmpty() ? TEXT("UNAVAILABLE") : Component.PhysicalMaterialHoleMaskSha1);
	Json->SetStringField(TEXT("physical_material_cell_status"), Component.PhysicalMaterialCellStatus);
	Json->SetStringField(TEXT("physical_material_cell_error"), Component.PhysicalMaterialCellError);
	Json->SetObjectField(TEXT("collision_height_raw_bulk"), MakeRawDataJson(Component.CollisionHeightData));
	Json->SetStringField(TEXT("collision_height_raw_bulk_element_count_unit"), TEXT("collision_height_vertices"));
	Json->SetObjectField(TEXT("dominant_layer_raw_bulk"), MakeRawDataJson(Component.DominantLayerData));
	Json->SetStringField(TEXT("dominant_layer_raw_bulk_element_count_unit"), TEXT("collision_height_vertices"));
	Json->SetNumberField(TEXT("collision_quad_flags_bytes_read"), static_cast<double>(Component.CollisionQuadFlagsBytes));
	Json->SetStringField(TEXT("collision_quad_flags_sha1"), Component.CollisionQuadFlagsSha1);
	Json->SetBoolField(TEXT("runtime_collision_height_readback_available"), Component.bRuntimeCollisionHeightReadable);
	Json->SetNumberField(TEXT("runtime_collision_height_sample_count"), static_cast<double>(Component.RuntimeCollisionHeightSampleCount));
	Json->SetStringField(TEXT("runtime_collision_height_sample_count_unit"), TEXT("collision_height_vertices"));
	Json->SetNumberField(TEXT("runtime_collision_height_bytes_read"), static_cast<double>(Component.RuntimeCollisionHeightBytesRead));
	Json->SetStringField(TEXT("runtime_collision_height_sha1"),
		Component.RuntimeCollisionHeightSha1.IsEmpty() ? TEXT("UNAVAILABLE") : Component.RuntimeCollisionHeightSha1);
	Json->SetArrayField(TEXT("collision_layer_names"), ToJsonStringArray(Component.CollisionLayerNames));
	TArray<TSharedPtr<FJsonValue>> UsedIndices;
	for (const uint8 Index : Component.UsedDominantLayerIndices)
		UsedIndices.Add(MakeShared<FJsonValueNumber>(Index));
	Json->SetArrayField(TEXT("dominant_layer_used_indices"), UsedIndices);
	Json->SetArrayField(TEXT("collision_layer_info_asset_paths"), ToJsonStringArray(Component.CollisionLayerInfoPaths));
	Json->SetBoolField(TEXT("has_valid_physical_material_inputs"), Component.bHasValidPhysicalMaterialInputs);
	Json->SetArrayField(TEXT("physical_material_paths"), ToJsonStringArray(Component.PhysicalMaterialPaths));
	Json->SetArrayField(TEXT("render_material_paths"), ToJsonStringArray(Component.RenderMaterialPaths));
	Json->SetArrayField(TEXT("weight_texture_paths"), ToJsonStringArray(Component.WeightTexturePaths));
	Json->SetBoolField(TEXT("component_data_complete"), Component.bComplete);
	return Json;
}

static TSharedRef<FJsonObject> MakeMapJson(const FIMMapSnapshot& Map)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("capture_kind"), Map.CaptureKind);
	Json->SetStringField(TEXT("requested_map_package"), Map.RequestedPackage);
	Json->SetStringField(TEXT("loaded_world_package"), Map.LoadedPackage);
	Json->SetBoolField(TEXT("load_map_returned_success"), Map.bLoadMapReturned);
	Json->SetBoolField(TEXT("world_identity_matches"), Map.bWorldIdentityMatches);
	Json->SetBoolField(TEXT("non_world_partition"), Map.bNonWorldPartition);
	Json->SetBoolField(TEXT("level_coverage_complete"), Map.bLevelCoverageComplete);
	Json->SetBoolField(TEXT("world_readiness_confirmed"), Map.bWorldReadinessConfirmed);
	Json->SetNumberField(TEXT("world_readiness_wait_frame_count"), Map.WorldReadinessWaitFrameCount);
	Json->SetNumberField(TEXT("world_readiness_wait_seconds"), Map.WorldReadinessWaitSeconds);
	Json->SetNumberField(TEXT("world_readiness_landscape_component_count"), Map.WorldReadinessLandscapeComponentCount);
	Json->SetNumberField(TEXT("world_readiness_collision_component_count"), Map.WorldReadinessCollisionComponentCount);
	Json->SetNumberField(TEXT("world_readiness_streaming_level_count"), Map.WorldReadinessStreamingLevelCount);
	Json->SetNumberField(TEXT("world_readiness_loaded_streaming_level_count"), Map.WorldReadinessLoadedStreamingLevelCount);
	Json->SetNumberField(TEXT("streaming_level_count"), Map.StreamingLevelCount);
	Json->SetNumberField(TEXT("loaded_streaming_level_count"), Map.LoadedStreamingLevelCount);
	Json->SetBoolField(TEXT("has_landscape"), Map.bHasLandscape);
	Json->SetNumberField(TEXT("landscape_component_count"), Map.LandscapeComponentCount);
	Json->SetNumberField(TEXT("collision_component_count"), Map.CollisionComponentCount);
	Json->SetBoolField(TEXT("data_coverage_complete"), Map.bCoverageComplete);
	Json->SetStringField(TEXT("package_filename"), Map.PackageFilename);
	Json->SetStringField(TEXT("package_sha1_before_read"), Map.PackageSha1.IsEmpty() ? TEXT("UNAVAILABLE") : Map.PackageSha1);
	Json->SetNumberField(TEXT("package_bytes_read"), static_cast<double>(Map.PackageBytesRead));
	Json->SetStringField(TEXT("package_sha1_after_restore"), Map.PackageSha1AfterRestore.IsEmpty() ? TEXT("UNAVAILABLE") : Map.PackageSha1AfterRestore);
	Json->SetNumberField(TEXT("package_recheck_bytes_read"), static_cast<double>(Map.PackageRecheckBytesRead));
	Json->SetBoolField(TEXT("package_file_unchanged"), Map.bPackageUnchanged);

	TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetNumberField(TEXT("height_components_complete"), Map.HeightCompleteComponentCount);
	Coverage->SetNumberField(TEXT("weight_components_complete"), Map.WeightCompleteComponentCount);
	Coverage->SetNumberField(TEXT("height_source_mip_components_complete"), Map.HeightMipCompleteComponentCount);
	Coverage->SetNumberField(TEXT("weight_source_mip_components_complete"), Map.WeightMipCompleteComponentCount);
	Coverage->SetNumberField(TEXT("collision_height_components_complete"), Map.CollisionHeightCompleteComponentCount);
	Coverage->SetNumberField(TEXT("physical_material_cell_components_complete"), Map.PhysicalMaterialCellCompleteComponentCount);
	Coverage->SetNumberField(TEXT("dominant_layer_bulk_present_components"), Map.DominantLayerBulkPresentCount);
	Coverage->SetNumberField(TEXT("dominant_layer_bulk_empty_components"), Map.DominantLayerBulkEmptyCount);
	Coverage->SetNumberField(TEXT("heightmap_texel_bytes_read"), static_cast<double>(Map.HeightTexelBytesRead));
	Coverage->SetNumberField(TEXT("height_value_bytes_hashed"), static_cast<double>(Map.HeightValueBytesHashed));
	Coverage->SetNumberField(TEXT("weight_bytes_read"), static_cast<double>(Map.WeightBytesRead));
	Coverage->SetNumberField(TEXT("collision_height_bytes_read"), static_cast<double>(Map.CollisionHeightBytesRead));
	Coverage->SetNumberField(TEXT("dominant_layer_bytes_read"), static_cast<double>(Map.DominantLayerBytesRead));
	Coverage->SetNumberField(TEXT("collision_quad_flags_bytes_read"), static_cast<double>(Map.CollisionQuadFlagsBytesRead));
	Coverage->SetNumberField(TEXT("runtime_collision_height_bytes_read"), static_cast<double>(Map.RuntimeCollisionHeightBytesRead));
	Coverage->SetNumberField(TEXT("unique_texture_source_mip_texture_count"), Map.TextureSourceMipTextureCount);
	Coverage->SetNumberField(TEXT("texture_source_mip_count"), Map.TextureSourceMipCount);
	Coverage->SetNumberField(TEXT("texture_source_mip_cached_object_read_failure_count"), Map.TextureSourceMipReadFailureCount);
	Coverage->SetStringField(TEXT("texture_source_mip_failure_count_scope"),
		TEXT("Failed valid texture objects in the source-mip cache. Null/unavailable pointers are reported by component status and incomplete coverage."));
	Coverage->SetNumberField(TEXT("texture_source_mip_bytes_read"), static_cast<double>(Map.TextureSourceMipBytesRead));
	Coverage->SetNumberField(TEXT("physical_material_cell_count"), static_cast<double>(Map.PhysicalMaterialCellCount));
	Coverage->SetStringField(TEXT("physical_material_cell_count_unit"), TEXT("collision_cells"));
	Coverage->SetNumberField(TEXT("physical_material_cell_bytes_read"), static_cast<double>(Map.PhysicalMaterialCellBytesRead));
	Coverage->SetNumberField(TEXT("physical_material_hole_cell_count"), static_cast<double>(Map.PhysicalMaterialHoleCellCount));
	Json->SetObjectField(TEXT("read_coverage"), Coverage);

	TArray<TSharedPtr<FJsonValue>> ComponentsJson;
	ComponentsJson.Reserve(Map.Components.Num());
	for (const FIMComponentSnapshot& Component : Map.Components)
	{
		ComponentsJson.Add(MakeShared<FJsonValueObject>(MakeComponentJson(Component)));
	}
	Json->SetArrayField(TEXT("components"), ComponentsJson);
	Json->SetArrayField(TEXT("layer_info_asset_paths"), ToJsonStringArray(Map.LayerInfoAssetPaths));
	Json->SetArrayField(TEXT("physical_material_paths"), ToJsonStringArray(Map.PhysicalMaterialPaths));
	Json->SetArrayField(TEXT("render_material_paths"), ToJsonStringArray(Map.RenderMaterialPaths));
	Json->SetArrayField(TEXT("heightmap_texture_paths"), ToJsonStringArray(Map.HeightmapTexturePaths));
	Json->SetArrayField(TEXT("weight_texture_paths"), ToJsonStringArray(Map.WeightTexturePaths));
	Json->SetBoolField(TEXT("has_valid_physical_material_inputs"), Map.bHasValidPhysicalMaterialInputs);
	Json->SetArrayField(TEXT("coverage_issues"), ToJsonStringArray(Map.Issues));
	return Json;
}

static TSharedRef<FJsonObject> MakeComparisonJson(
	const FIMComparison& Comparison,
	const FIMMapSnapshot& Source,
	const FIMMapSnapshot& Preview)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("matched_component_count"), Comparison.MatchedComponentCount);
	Json->SetNumberField(TEXT("source_only_component_count"), Comparison.SourceOnlyComponentCount);
	Json->SetNumberField(TEXT("preview_only_component_count"), Comparison.PreviewOnlyComponentCount);
	Json->SetNumberField(TEXT("topology_mismatch_count"), Comparison.TopologyMismatchCount);
	Json->SetNumberField(TEXT("height_value_mismatch_count"), Comparison.HeightMismatchCount);
	Json->SetNumberField(TEXT("height_source_mip_mismatch_count"), Comparison.HeightMipMismatchCount);
	Json->SetNumberField(TEXT("collision_height_mismatch_count"), Comparison.CollisionHeightMismatchCount);
	Json->SetNumberField(TEXT("runtime_collision_height_mismatch_count"), Comparison.RuntimeCollisionHeightMismatchCount);
	Json->SetNumberField(TEXT("collision_quad_flag_mismatch_count"), Comparison.CollisionFlagMismatchCount);
	Json->SetNumberField(TEXT("weight_mismatch_count"), Comparison.WeightMismatchCount);
	Json->SetNumberField(TEXT("weight_source_mip_mismatch_count"), Comparison.WeightMipMismatchCount);
	Json->SetNumberField(TEXT("physical_material_cell_mismatch_count"), Comparison.PhysicalMaterialCellMismatchCount);
	Json->SetNumberField(TEXT("physical_material_hole_mask_mismatch_count"), Comparison.PhysicalMaterialHoleMaskMismatchCount);
	Json->SetNumberField(TEXT("dominant_layer_mismatch_count"), Comparison.DominantLayerMismatchCount);
	Json->SetNumberField(TEXT("dominant_layer_pm_cache_exception_count"), Comparison.DominantLayerPmCacheExceptionCount);
	Json->SetNumberField(TEXT("collision_transform_mismatch_count"), Comparison.CollisionTransformMismatchCount);
	Json->SetNumberField(TEXT("hard_mismatch_count"), Comparison.HardMismatchCount);
	Json->SetNumberField(TEXT("difference_detail_limit"), MaxDifferenceDetails);
	Json->SetArrayField(TEXT("differences"), ToJsonStringArray(Comparison.Differences));
	Json->SetArrayField(TEXT("sample_delta_details"), Comparison.SampleDeltaDetails);

	TSharedRef<FJsonObject> AllowedPathChanges = MakeShared<FJsonObject>();
	AllowedPathChanges->SetArrayField(TEXT("source_only_layer_info_asset_paths"),
		ToJsonStringArray(GetOnlyPaths(Source.LayerInfoAssetPaths, Preview.LayerInfoAssetPaths)));
	AllowedPathChanges->SetArrayField(TEXT("preview_only_layer_info_asset_paths"),
		ToJsonStringArray(GetOnlyPaths(Preview.LayerInfoAssetPaths, Source.LayerInfoAssetPaths)));
	AllowedPathChanges->SetArrayField(TEXT("source_only_physical_material_paths"),
		ToJsonStringArray(GetOnlyPaths(Source.PhysicalMaterialPaths, Preview.PhysicalMaterialPaths)));
	AllowedPathChanges->SetArrayField(TEXT("preview_only_physical_material_paths"),
		ToJsonStringArray(GetOnlyPaths(Preview.PhysicalMaterialPaths, Source.PhysicalMaterialPaths)));
	AllowedPathChanges->SetArrayField(TEXT("source_only_render_material_paths"),
		ToJsonStringArray(GetOnlyPaths(Source.RenderMaterialPaths, Preview.RenderMaterialPaths)));
	AllowedPathChanges->SetArrayField(TEXT("preview_only_render_material_paths"),
		ToJsonStringArray(GetOnlyPaths(Preview.RenderMaterialPaths, Source.RenderMaterialPaths)));
	AllowedPathChanges->SetArrayField(TEXT("source_only_heightmap_texture_paths"),
		ToJsonStringArray(GetOnlyPaths(Source.HeightmapTexturePaths, Preview.HeightmapTexturePaths)));
	AllowedPathChanges->SetArrayField(TEXT("preview_only_heightmap_texture_paths"),
		ToJsonStringArray(GetOnlyPaths(Preview.HeightmapTexturePaths, Source.HeightmapTexturePaths)));
	AllowedPathChanges->SetArrayField(TEXT("source_only_weight_texture_paths"),
		ToJsonStringArray(GetOnlyPaths(Source.WeightTexturePaths, Preview.WeightTexturePaths)));
	AllowedPathChanges->SetArrayField(TEXT("preview_only_weight_texture_paths"),
		ToJsonStringArray(GetOnlyPaths(Preview.WeightTexturePaths, Source.WeightTexturePaths)));
	AllowedPathChanges->SetBoolField(TEXT("layer_info_and_physical_material_paths_are_not_numeric_data_gates"), true);
	AllowedPathChanges->SetStringField(TEXT("physical_material_cell_correctness_owner"),
		TEXT("RuralPreviewPrepare validates cell IDs against material-source inputs; this test checks source-preview cell and hole-mask identity."));
	Json->SetObjectField(TEXT("asset_path_differences"), AllowedPathChanges);
	return Json;
}

static TArray<FString> GetDirtyPackagePaths()
{
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyPackages(DirtyPackages);
	TArray<FString> Paths;
	for (UPackage* Package : DirtyPackages)
	{
		if (IsValid(Package))
		{
			Paths.Add(Package->GetName());
		}
	}
	SortUniquePaths(Paths);
	return Paths;
}

static bool IsSavedStartupMapRestorable(const FString& StartupPackage, FString& OutFilename)
{
	if (StartupPackage.IsEmpty())
	{
		return false;
	}
	OutFilename = FPackageName::LongPackageNameToFilename(
		StartupPackage, FPackageName::GetMapPackageExtension());
	OutFilename = FPaths::ConvertRelativePathToFull(OutFilename);
	return FPaths::FileExists(OutFilename);
}

static void CollectLandscapeWorldComponents(
	UWorld* World,
	TArray<ULandscapeComponent*>& OutLandscapeComponents,
	TArray<ULandscapeHeightfieldCollisionComponent*>& OutCollisionComponents)
{
	OutLandscapeComponents.Reset();
	OutCollisionComponents.Reset();
	if (!IsValid(World))
	{
		return;
	}

	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!IsValid(Actor))
		{
			continue;
		}
		// GetComponents resets its output on every actor. Accumulate locally so
		// a later non-Landscape actor cannot erase earlier proxy components.
		TArray<ULandscapeComponent*> ActorLandscapeComponents;
		TArray<ULandscapeHeightfieldCollisionComponent*> ActorCollisionComponents;
		Actor->GetComponents(ActorLandscapeComponents);
		Actor->GetComponents(ActorCollisionComponents);
		OutLandscapeComponents.Append(ActorLandscapeComponents);
		OutCollisionComponents.Append(ActorCollisionComponents);
	}
}

struct FIMWorldReadinessSnapshot
{
	bool bWorldIdentityMatches = false;
	bool bNonWorldPartition = false;
	bool bAllStreamingLevelsLoaded = false;
	bool bReady = false;
	int32 StreamingLevelCount = 0;
	int32 LoadedStreamingLevelCount = 0;
	int32 LandscapeComponentCount = 0;
	int32 CollisionComponentCount = 0;
};

static FIMWorldReadinessSnapshot ReadWorldReadiness(const FString& RequestedPackage)
{
	FIMWorldReadinessSnapshot Readiness;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!IsValid(World) || !World->PersistentLevel)
	{
		return Readiness;
	}

	Readiness.bWorldIdentityMatches = World->GetOutermost()->GetName() == RequestedPackage;
	Readiness.bNonWorldPartition = !World->IsPartitionedWorld();
	Readiness.bAllStreamingLevelsLoaded = true;
	Readiness.StreamingLevelCount = World->GetStreamingLevels().Num();
	for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
	{
		if (!IsValid(StreamingLevel) || StreamingLevel->GetWorldAssetPackageFName().IsNone())
		{
			continue;
		}
		if (StreamingLevel->GetLoadedLevel())
		{
			++Readiness.LoadedStreamingLevelCount;
		}
		else
		{
			Readiness.bAllStreamingLevelsLoaded = false;
		}
	}

	TArray<ULandscapeComponent*> LandscapeComponents;
	TArray<ULandscapeHeightfieldCollisionComponent*> CollisionComponents;
	CollectLandscapeWorldComponents(World, LandscapeComponents, CollisionComponents);
	Readiness.LandscapeComponentCount = LandscapeComponents.Num();
	Readiness.CollisionComponentCount = CollisionComponents.Num();
	Readiness.bReady = Readiness.bWorldIdentityMatches
		&& Readiness.bNonWorldPartition
		&& Readiness.bAllStreamingLevelsLoaded
		&& Readiness.LandscapeComponentCount > 0
		&& Readiness.CollisionComponentCount > 0
		&& Readiness.LandscapeComponentCount == Readiness.CollisionComponentCount;
	return Readiness;
}

constexpr double MapReadinessTimeoutSeconds = 30.0;
constexpr double MapReadinessPollIntervalSeconds = 0.1;
constexpr int32 StableMapReadinessProbeCount = 3;

struct FIMMapReadinessWait
{
	double StartSeconds = 0.0;
	double LastProbeSeconds = 0.0;
	double ElapsedSeconds = 0.0;
	int32 FrameCount = 0;
	int32 ProbeCount = 0;
	int32 StableProbeCount = 0;
	int32 PreviousLandscapeComponentCount = INDEX_NONE;
	int32 PreviousCollisionComponentCount = INDEX_NONE;
	int32 PreviousStreamingLevelCount = INDEX_NONE;
	int32 PreviousLoadedStreamingLevelCount = INDEX_NONE;
	bool bConfirmed = false;
	FIMWorldReadinessSnapshot Latest;
};

// Family-lifetime storage is the public GT -> RT handoff. RT never dereferences
// the UWorld, and copied IDs are diagnostic tags only while that world is alive.
struct FIMRenderProducerFamilyData final : public ISceneViewFamilyExtentionData
{
	inline static const TCHAR* GSubclassIdentifier = TEXT("IMAcoustic.RenderProducerFamily.v1");
	virtual const TCHAR* GetSubclassIdentifier() const override { return GSubclassIdentifier; }
	FString RunId;
	FString WorldPackage;
	uint32 WorldId = 0;
	uint32 Step = 0;
	uint64 GTFrame = 0;
	uint32 FamilyFrameAtGT = MAX_uint32;
	bool bCaptured = false;
};

class FIMRenderProducerObserver final : public FSceneViewExtensionBase
{
public:
	FIMRenderProducerObserver(const FAutoRegister& AutoRegister, const FString& InRunId)
		: FSceneViewExtensionBase(AutoRegister), RunId(InRunId) {}
	virtual void SetupViewFamily(FSceneViewFamily&) override {}
	virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
	void SetStep(uint32 InStep) { check(IsInGameThread()); Step = InStep; }
	virtual void BeginRenderViewFamily(FSceneViewFamily& Family) override
	{
		check(IsInGameThread());
		++FamilyCount;
		if (FamilyCount > MaxCapturedFamilies) { ++OverflowFamilyCount; return; }
		FIMRenderProducerFamilyData* Data = Family.GetOrCreateExtentionData<FIMRenderProducerFamilyData>();
		UWorld* World = Family.Scene ? Family.Scene->GetWorld() : nullptr;
		Data->RunId = RunId;
		Data->WorldPackage = World ? World->GetOutermost()->GetName() : TEXT("<NO_WORLD>");
		Data->WorldId = World ? World->GetUniqueID() : 0;
		Data->Step = Step;
		Data->GTFrame = GFrameCounter;
		Data->FamilyFrameAtGT = Family.FrameNumber;
		Data->bCaptured = true;
	}
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& Graph, FSceneViewFamily& Family) override
	{
		LogFamily(TEXT("pre_family"), Graph, Family);
	}
	virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& Graph, FSceneView& View,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters>) override
	{
		const FIMRenderProducerFamilyData* Data = View.Family->GetExtentionData<FIMRenderProducerFamilyData>();
		if (!Data || !Data->bCaptured) return;
		++BasePassCallbackCount;
		bool bSubstrateTargetPresent = false;
		for (uint32 Slot = 0; Slot < RenderTargets.GetActiveCount(); ++Slot)
		{
			const FRDGTexture* Texture = RenderTargets[Slot].GetTexture();
			if (!Texture || FCString::Strcmp(Texture->Name, TEXT("Substrate.Material")) != 0) continue;
			bSubstrateTargetPresent = true;
			++SubstrateBindingCount;
			const bool bPriorGraphProduction = Texture->HasBeenProduced();
			if (!bPriorGraphProduction) ++UnproducedBindingCount;
			UE_LOG(LogTemp, Display, TEXT("[IM][RENDER_PRODUCER] phase=post_base_pass run=%s step=%u gt_frame=%llu rt_frame=%llu family_frame=%u family_frame_gt=%u family_frame_gt_valid=%d world=%s world_id=%u graph=%p scene=%p view_rect=%dx%d slot=%u texture=%p resource=%s extent=%dx%d array_size=%u format=%u prior_graph_production=%d materials=%d lighting=%d wireframe=%d"),
				*Data->RunId, Data->Step, Data->GTFrame, GFrameCounterRenderThread, View.Family->FrameNumber, Data->FamilyFrameAtGT,
				Data->FamilyFrameAtGT != MAX_uint32, *Data->WorldPackage, Data->WorldId,
				static_cast<const void*>(&Graph), static_cast<const void*>(View.Family->Scene),
				View.UnscaledViewRect.Width(), View.UnscaledViewRect.Height(), Slot,
				static_cast<const void*>(Texture), Texture->Name, Texture->Desc.Extent.X,
				Texture->Desc.Extent.Y, uint32(Texture->Desc.ArraySize), uint32(Texture->Desc.Format), bPriorGraphProduction,
				View.Family->EngineShowFlags.Materials, View.Family->EngineShowFlags.Lighting,
				View.Family->EngineShowFlags.Wireframe);
		}
		if (!bSubstrateTargetPresent)
			UE_LOG(LogTemp, Display, TEXT("[IM][RENDER_PRODUCER] phase=post_base_pass run=%s step=%u gt_frame=%llu rt_frame=%llu family_frame=%u world=%s world_id=%u graph=%p substrate_target_present=0 active_targets=%u"),
				*Data->RunId, Data->Step, Data->GTFrame, GFrameCounterRenderThread, View.Family->FrameNumber,
				*Data->WorldPackage, Data->WorldId, static_cast<const void*>(&Graph), RenderTargets.GetActiveCount());
	}
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& Graph, FSceneViewFamily& Family) override
	{
		LogFamily(TEXT("post_family"), Graph, Family);
	}
	TSharedRef<FJsonObject> StopAndMakeSummary()
	{
		check(IsInGameThread());
		bEnabled.Store(false);
		FlushRenderingCommands();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("family_count"), FamilyCount);
		Result->SetNumberField(TEXT("family_cap"), MaxCapturedFamilies);
		Result->SetNumberField(TEXT("overflow_family_count"), OverflowFamilyCount);
		Result->SetBoolField(TEXT("family_cap_not_exhausted"), OverflowFamilyCount == 0);
		Result->SetStringField(TEXT("coverage_requirement"), TEXT("Any overflow makes coverage incomplete. Even without overflow, claims require a matching world/step record; the cap includes pre-load families."));
		Result->SetNumberField(TEXT("post_base_pass_callback_count"), BasePassCallbackCount.Load());
		Result->SetNumberField(TEXT("substrate_binding_count"), SubstrateBindingCount.Load());
		Result->SetNumberField(TEXT("unproduced_substrate_binding_count"), UnproducedBindingCount.Load());
		Result->SetStringField(TEXT("state_semantics"), TEXT("HasBeenProduced is prior RDG registration, aggregate across earlier writers/subresources; not exact BasePass identity or GPU execution. Views without this extension are outside coverage."));
		Result->SetStringField(TEXT("step_legend"), TEXT("0 LoadSource;1 WaitForSource;2 CaptureSource;3 LoadPreview;4 RestorePristineFinal;5 WaitForPreview;6 CapturePreview;7 RestoreStartup;8 WaitForStartupRestore;9 Finalize;10 Done"));
		return Result;
	}
protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext&) const override { return bEnabled.Load(); }
private:
	static int32 ReadRenderCVar(const TCHAR* Name)
	{
		// FAutoConsoleVariableRef exposes GetInt but not typed variable data.
		// These four registered CVars are RenderThreadSafe; reject an unknown
		// non-safe registration rather than accessing it from this callback.
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable && Variable->TestFlags(ECVF_RenderThreadSafe) ? Variable->GetInt() : -1;
	}
	static void LogFamily(const TCHAR* Phase, FRDGBuilder& Graph, const FSceneViewFamily& Family)
	{
		const FIMRenderProducerFamilyData* Data = Family.GetExtentionData<FIMRenderProducerFamilyData>();
		if (!Data || !Data->bCaptured) return;
		UE_LOG(LogTemp, Display, TEXT("[IM][RENDER_PRODUCER] phase=%s run=%s step=%u gt_frame=%llu rt_frame=%llu family_frame=%u family_frame_gt=%u family_frame_gt_valid=%d world=%s world_id=%u graph=%p scene=%p materials=%d lighting=%d wireframe=%d dynamic_shadows=%d df_shadows=%d substrate=%d distance_field_shadowing=%d df_quality=%d substrate_clear=%d df_async_compute=%d df_full_resolution=%d"),
			Phase, *Data->RunId, Data->Step, Data->GTFrame, GFrameCounterRenderThread, Family.FrameNumber, Data->FamilyFrameAtGT,
			Data->FamilyFrameAtGT != MAX_uint32, *Data->WorldPackage, Data->WorldId,
			static_cast<const void*>(&Graph), static_cast<const void*>(Family.Scene),
			Family.EngineShowFlags.Materials, Family.EngineShowFlags.Lighting, Family.EngineShowFlags.Wireframe,
			Family.EngineShowFlags.DynamicShadows, Family.EngineShowFlags.RayTracedDistanceFieldShadows,
			ReadRenderCVar(TEXT("r.Substrate")), ReadRenderCVar(TEXT("r.DistanceFieldShadowing")),
			ReadRenderCVar(TEXT("r.DFShadowQuality")), ReadRenderCVar(TEXT("r.Substrate.Debug.ClearMaterialBuffer")),
			ReadRenderCVar(TEXT("r.DFShadowAsyncCompute")), ReadRenderCVar(TEXT("r.DFFullResolution")));
	}
	static constexpr uint32 MaxCapturedFamilies = 256;
	FString RunId;
	uint32 Step = 0;
	uint32 FamilyCount = 0;
	uint32 OverflowFamilyCount = 0;
	TAtomic<bool> bEnabled { true };
	TAtomic<uint32> BasePassCallbackCount { 0 };
	TAtomic<uint32> SubstrateBindingCount { 0 };
	TAtomic<uint32> UnproducedBindingCount { 0 };
};

// Descriptor mode inserts no resource readback pass, but DumpGPU serializes RDG
// setup/execute. This scope is diagnostic only and cannot certify natural timing.
class FIMRenderDescriptorDump final
{
public:
	~FIMRenderDescriptorDump() { Restore(); }
	bool Arm(const FString& RunId, const FString& ReportDirectory)
	{
		check(IsInGameThread());
		bAttempted = true;
		ArmedGTFrame = GFrameCounter;
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		ArmedWorld = World ? World->GetOutermost()->GetName() : TEXT("<NO_WORLD>");
#if RDG_DUMP_RESOURCES
		bCompiled = true;
		if (UE::RenderCore::DumpGPU::IsDumpingFrame())
		{
			Error = TEXT("Another DumpGPU capture is already active; no diagnostic CVar was changed.");
			return false;
		}
		// R12's first Rural family followed LoadMap by two GT frames. Capture
		// three frames, then require an actual RT-frame join in the raw evidence.
		const TPair<const TCHAR*, const TCHAR*> Requests[] = {
			{ TEXT("r.DumpGPU.Texture"), TEXT("1") },
			{ TEXT("r.DumpGPU.Buffer"), TEXT("1") },
			{ TEXT("r.DumpGPU.Root"), TEXT("*") },
			{ TEXT("r.DumpGPU.Delay"), TEXT("0") },
			{ TEXT("r.DumpGPU.FrameDelay"), TEXT("0") },
			{ TEXT("r.DumpGPU.FrameCount"), TEXT("3") },
			{ TEXT("r.DumpGPU.Stream"), TEXT("0") },
			{ TEXT("r.DumpGPU.Test.EnableDiskWrite"), TEXT("1") }
		};
		for (const auto& Request : Requests)
		{
			IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Request.Key);
			if (!Variable || Variable->TestFlags(ECVF_ReadOnly))
			{
				Error = FString::Printf(TEXT("DumpGPU CVar is missing or read-only: %s"), Request.Key);
				Restore();
				return false;
			}
			FIMSavedCVar& Saved = SavedCVars.AddDefaulted_GetRef();
			Saved.Name = Request.Key;
			Saved.Variable = Variable;
			Saved.Value = Variable->GetString();
			Saved.Flags = Variable->GetFlags();
			Saved.Requested = Request.Value;
			Variable->SetWithCurrentPriority(Request.Value);
			Saved.AppliedValue = Variable->GetString();
			if (Saved.AppliedValue != Saved.Requested)
			{
				Error = FString::Printf(TEXT("DumpGPU CVar request was not applied: %s"), Request.Key);
				Restore();
				return false;
			}
		}
		const FString RequestedDirectory = FPaths::ConvertRelativePathToFull(
			ReportDirectory / TEXT("RenderDescriptors") / RunId);
		const FString Command = FString::Printf(
			TEXT("-noexplorer -path=\"%s\" -unified_filter=\"*@Substrate.Material*\""), *RequestedDirectory);
		Directory = FRDGBuilder::BeginResourceDump(*Command, *RunId);
		bArmed = !Directory.IsEmpty();
		if (!bArmed)
		{
			Error = TEXT("BeginResourceDump did not return a disk-write directory; another dump may be pending.");
			Restore();
		}
		UE_LOG(LogTemp, Display, TEXT("[IM][RENDER_DESCRIPTOR_DUMP] run=%s armed=%d gt_frame=%llu world=%s directory=%s frames=3 texture=1 buffer=1"),
			*RunId, bArmed, ArmedGTFrame, *ArmedWorld, *Directory);
		return bArmed;
#else
		Error = TEXT("This Editor module was compiled without RDG_DUMP_RESOURCES.");
		return false;
#endif
	}
	void Tick()
	{
		check(IsInGameThread());
#if RDG_DUMP_RESOURCES
		if (!bArmed || bCVarRestorationComplete) return;
		if (UE::RenderCore::DumpGPU::IsDumpingFrame())
		{
			bObservedActiveDump = true;
			return;
		}
		if (bObservedActiveDump)
		{
			FlushRenderingCommands();
			Restore();
		}
#endif
	}
	void Restore()
	{
		check(IsInGameThread());
		if (bCVarRestorationComplete) return;
		bool bAllRestored = true;
		for (FIMSavedCVar& Saved : SavedCVars)
		{
			if (Saved.bRestored) continue;
			Saved.Variable->SetWithCurrentPriority(*Saved.Value);
			Saved.Variable->SetFlags(Saved.Flags);
			Saved.RestoredValue = Saved.Variable->GetString();
			Saved.RestoredFlags = Saved.Variable->GetFlags();
			Saved.bRestored = Saved.RestoredValue == Saved.Value && Saved.RestoredFlags == Saved.Flags;
			bAllRestored &= Saved.bRestored;
		}
		bCVarRestorationComplete = bAllRestored;
		RestoredGTFrame = GFrameCounter;
	}
	TSharedRef<FJsonObject> MakeSummary() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("attempted"), bAttempted);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("armed"), bArmed);
		Result->SetNumberField(TEXT("armed_gt_frame"), double(ArmedGTFrame));
		Result->SetStringField(TEXT("armed_world"), ArmedWorld);
		Result->SetStringField(TEXT("directory"), Directory);
		Result->SetStringField(TEXT("error"), Error);
		Result->SetBoolField(TEXT("observed_active_dump"), bObservedActiveDump);
		Result->SetNumberField(TEXT("restored_gt_frame"), double(RestoredGTFrame));
		Result->SetStringField(TEXT("scope"), TEXT("Three-frame descriptor-only DumpGPU; serializes RDG setup/execute. Require matching RT frame, first source family and exact resource ID before diagnosing a dependency. No performance or repair claim."));
		bool bAllRestored = true;
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FIMSavedCVar& Saved : SavedCVars)
		{
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("name"), Saved.Name);
			Value->SetStringField(TEXT("before"), Saved.Value);
			Value->SetStringField(TEXT("requested"), Saved.Requested);
			Value->SetStringField(TEXT("applied"), Saved.AppliedValue);
			Value->SetNumberField(TEXT("flags_before"), uint32(Saved.Flags));
			Value->SetStringField(TEXT("restored_value"), Saved.RestoredValue);
			Value->SetNumberField(TEXT("restored_flags"), uint32(Saved.RestoredFlags));
			Value->SetBoolField(TEXT("restored"), Saved.bRestored);
			Values.Add(MakeShared<FJsonValueObject>(Value));
			bAllRestored &= Saved.bRestored;
		}
		Result->SetArrayField(TEXT("cvars"), Values);
		Result->SetBoolField(TEXT("all_cvars_restored"), bAllRestored);
		for (const TCHAR* Name : { TEXT("Passes.json"), TEXT("ResourceDescs.json") })
		{
			const FString Filename = Directory / TEXT("Base") / Name;
			Result->SetNumberField(Name, Directory.IsEmpty() ? -1 : IFileManager::Get().FileSize(*Filename));
		}
		return Result;
	}
private:
	struct FIMSavedCVar
	{
		FString Name, Value, Requested, AppliedValue, RestoredValue;
		IConsoleVariable* Variable = nullptr;
		EConsoleVariableFlags Flags = ECVF_Default, RestoredFlags = ECVF_Default;
		bool bRestored = false;
	};
	TArray<FIMSavedCVar> SavedCVars;
	bool bAttempted = false, bCompiled = false, bArmed = false;
	bool bObservedActiveDump = false, bCVarRestorationComplete = false;
	uint64 ArmedGTFrame = 0, RestoredGTFrame = 0;
	FString ArmedWorld, Directory, Error;
};

struct FIMRunContext
{
	enum class EIMStep : uint8
	{
		LoadSource,
		WaitForSource,
		CaptureSource,
		LoadPreview,
		RestorePristineFinal,
		WaitForPreview,
		CapturePreview,
		RestoreStartup,
		WaitForStartupRestore,
		Finalize,
		Done
	};

	FAutomationTestBase* Test = nullptr;
	FString RunId;
	TSharedPtr<FIMRenderProducerObserver, ESPMode::ThreadSafe> RenderProducerObserver;
	TSharedPtr<FJsonObject> RenderProducerSummary;
	TUniquePtr<FIMRenderDescriptorDump> RenderDescriptorDump;
	FString ReportDirectory;
	FString ReportPath;
	FString StartupMapPackage;
	FString StartupFilename;
	FString TargetPreviewPackage;
	bool bPristineDuplicateProbe = false;
	bool bPristineProbeSaved = false;
	bool bRestoreFinalDataProbe = false;
	bool bFinalDataSnapshotCaptured = false;
	bool bFinalDataRestored = false;
	bool bRestoredMapSaved = false;
	int64 FinalDataTextureSourceBytes = 0;
	int64 FinalDataCollisionHeightBytes = 0;
	int32 FinalDataLandscapeComponents = 0;
	int32 FinalDataUniqueTextures = 0;
	uint64 FirstPristineSaveFrame = 0;
	uint64 RestoredPristineSaveFrame = 0;
	TStrongObjectPtr<UWorld> DuplicateProbeWorld;
	FIMAcousticLandscapeFinalSnapshot FinalDataSnapshot;
	FIMMapSnapshot SourceImmediatelyBeforeDuplicate;
	FIMMapSnapshot DuplicateBeforeSave;
	FIMMapSnapshot DuplicateImmediatelyAfterSave;
	FIMMapSnapshot DuplicateImmediatelyAfterRestore;
	FIMMapSnapshot DuplicateAfterRestoredSave;
	FIMMapSnapshot SourceImmediatelyAfterProbeSave;
	TArray<FString> InitialDirtyPackages;
	bool bInitialEditorWorldValid = false;
	bool bStartupMapRestorable = false;
	bool bMayLoadMaps = false;

	FIMMapSnapshot Source;
	FIMMapSnapshot Preview;
	FIMComparison Comparison;
	TArray<FString> InconclusiveReasons;
	TArray<FString> FailureReasons;
	bool bMapLoadAttempted = false;
	bool bStartupMapRestored = false;
	bool bDirtyPackagesClearAfterRestore = false;
	bool bSourcePackageHashStable = false;
	bool bPreviewPackageHashStable = false;

	FString SourcePackageFilenameBeforeLoad;
	FString SourcePackageSha1BeforeLoad;
	int64 SourcePackageBytesReadBeforeLoad = 0;
	bool bSourcePackageReadBeforeLoad = false;
	FString PreviewPackageFilenameBeforeLoad;
	FString PreviewPackageSha1BeforeLoad;
	int64 PreviewPackageBytesReadBeforeLoad = 0;
	bool bPreviewPackageReadBeforeLoad = false;

	EIMStep Step = EIMStep::Finalize;
	bool bSourceLoadReturned = false;
	bool bPreviewLoadReturned = false;
	bool bRestoreLoadReturned = false;
	FIMMapReadinessWait ActiveMapWait;
	double StartupRestoreStartSeconds = 0.0;
	double StartupRestoreLastProbeSeconds = 0.0;
	double StartupRestoreWaitSeconds = 0.0;
	int32 StartupRestoreWaitFrameCount = 0;
	int32 StartupRestoreProbeCount = 0;
	int32 StartupRestoreStableProbeCount = 0;
	bool bStartupRestoreIdentityStable = false;
};

static bool UpdateMapReadinessWait(FIMMapReadinessWait& Wait, const FString& RequestedPackage)
{
	++Wait.FrameCount;
	const double NowSeconds = FPlatformTime::Seconds();
	Wait.ElapsedSeconds = NowSeconds - Wait.StartSeconds;

	if (NowSeconds - Wait.LastProbeSeconds >= MapReadinessPollIntervalSeconds)
	{
		Wait.LastProbeSeconds = NowSeconds;
		++Wait.ProbeCount;
		Wait.Latest = ReadWorldReadiness(RequestedPackage);
		const bool bSameInventoryAsPreviousProbe =
			Wait.Latest.LandscapeComponentCount == Wait.PreviousLandscapeComponentCount
			&& Wait.Latest.CollisionComponentCount == Wait.PreviousCollisionComponentCount
			&& Wait.Latest.StreamingLevelCount == Wait.PreviousStreamingLevelCount
			&& Wait.Latest.LoadedStreamingLevelCount == Wait.PreviousLoadedStreamingLevelCount;
		if (Wait.Latest.bReady)
		{
			Wait.StableProbeCount = bSameInventoryAsPreviousProbe ? Wait.StableProbeCount + 1 : 1;
		}
		else
		{
			Wait.StableProbeCount = 0;
		}
		Wait.PreviousLandscapeComponentCount = Wait.Latest.LandscapeComponentCount;
		Wait.PreviousCollisionComponentCount = Wait.Latest.CollisionComponentCount;
		Wait.PreviousStreamingLevelCount = Wait.Latest.StreamingLevelCount;
		Wait.PreviousLoadedStreamingLevelCount = Wait.Latest.LoadedStreamingLevelCount;
		Wait.bConfirmed = Wait.StableProbeCount >= StableMapReadinessProbeCount;
	}

	return Wait.bConfirmed || Wait.ElapsedSeconds >= MapReadinessTimeoutSeconds;
}

static void ApplyReadinessWait(FIMMapSnapshot& Map, const FIMMapReadinessWait& Wait)
{
	Map.bWorldReadinessConfirmed = Wait.bConfirmed;
	Map.WorldReadinessWaitFrameCount = Wait.FrameCount;
	Map.WorldReadinessWaitSeconds = Wait.ElapsedSeconds;
	Map.WorldReadinessLandscapeComponentCount = Wait.Latest.LandscapeComponentCount;
	Map.WorldReadinessCollisionComponentCount = Wait.Latest.CollisionComponentCount;
	Map.WorldReadinessStreamingLevelCount = Wait.Latest.StreamingLevelCount;
	Map.WorldReadinessLoadedStreamingLevelCount = Wait.Latest.LoadedStreamingLevelCount;
}

static bool FinalizeRunTest(FIMRunContext& State)
{
	if (State.RenderProducerObserver)
	{
		State.RenderProducerSummary = State.RenderProducerObserver->StopAndMakeSummary();
		State.RenderProducerObserver.Reset();
	}
	if (State.RenderDescriptorDump) State.RenderDescriptorDump->Restore();
	FAutomationTestBase* Test = State.Test;
	const FString& RunId = State.RunId;
	const FString& ReportDirectory = State.ReportDirectory;
	const FString& ReportPath = State.ReportPath;
	const FString& StartupMapPackage = State.StartupMapPackage;
	const FString& StartupFilename = State.StartupFilename;
	const TArray<FString>& InitialDirtyPackages = State.InitialDirtyPackages;
	const bool bInitialEditorWorldValid = State.bInitialEditorWorldValid;
	const bool bStartupMapRestorable = State.bStartupMapRestorable;
	FIMMapSnapshot& Source = State.Source;
	FIMMapSnapshot& Preview = State.Preview;
	FIMComparison& Comparison = State.Comparison;
	TArray<FString>& InconclusiveReasons = State.InconclusiveReasons;
	TArray<FString>& FailureReasons = State.FailureReasons;
	TSharedPtr<FJsonObject> DescriptorSummary;
	if (State.RenderDescriptorDump)
	{
		DescriptorSummary = State.RenderDescriptorDump->MakeSummary();
		if (!DescriptorSummary->GetBoolField(TEXT("armed"))
			|| !DescriptorSummary->GetBoolField(TEXT("all_cvars_restored"))
			|| DescriptorSummary->GetNumberField(TEXT("Passes.json")) <= 0
			|| DescriptorSummary->GetNumberField(TEXT("ResourceDescs.json")) <= 0)
		{
			FailureReasons.Add(TEXT("Render descriptor dump was not armed, its metadata is missing, or its CVar restoration failed. Actual first-family coverage still requires a raw RT-frame/resource join."));
		}
	}
	const bool bMapLoadAttempted = State.bMapLoadAttempted;
	bool& bStartupMapRestored = State.bStartupMapRestored;
	bool& bDirtyPackagesClearAfterRestore = State.bDirtyPackagesClearAfterRestore;
	bool& bSourcePackageHashStable = State.bSourcePackageHashStable;
	bool& bPreviewPackageHashStable = State.bPreviewPackageHashStable;

	if (bMapLoadAttempted && bStartupMapRestorable)
	{
		UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		bStartupMapRestored = State.bRestoreLoadReturned && State.bStartupRestoreIdentityStable
			&& IsValid(RestoredWorld)
			&& RestoredWorld->GetOutermost()->GetName() == StartupMapPackage;
		if (!bStartupMapRestored)
		{
			FailureReasons.Add(FString::Printf(
				TEXT("Startup map restoration failed after waiting for the restored editor world: requested=%s loaded=%s returned=%d identity_stable=%d."),
				*StartupMapPackage,
				IsValid(RestoredWorld) ? *RestoredWorld->GetOutermost()->GetName() : TEXT("UNKNOWN"),
				State.bRestoreLoadReturned ? 1 : 0,
				State.bStartupRestoreIdentityStable ? 1 : 0));
		}
	}
	else
	{
		bStartupMapRestored = bInitialEditorWorldValid && bStartupMapRestorable;
	}

	FString SourceRecheckError;
	FString PreviewRecheckError;
	Source.bPackageUnchanged = Source.bPackageRead && ReadFileSha1(
		Source.PackageFilename, Source.PackageSha1AfterRestore, Source.PackageRecheckBytesRead, SourceRecheckError)
		&& Source.PackageSha1AfterRestore == Source.PackageSha1;
	Preview.bPackageUnchanged = Preview.bPackageRead && ReadFileSha1(
		Preview.PackageFilename, Preview.PackageSha1AfterRestore, Preview.PackageRecheckBytesRead, PreviewRecheckError)
		&& Preview.PackageSha1AfterRestore == Preview.PackageSha1;
	bSourcePackageHashStable = Source.bPackageUnchanged;
	bPreviewPackageHashStable = Preview.bPackageUnchanged;
	if (Source.bPackageRead && !Source.bPackageUnchanged)
	{
		FailureReasons.Add(FString::Printf(
			TEXT("Source map file changed during the read-only check or could not be re-read: %s"), *SourceRecheckError));
	}
	if (Preview.bPackageRead && !Preview.bPackageUnchanged)
	{
		FailureReasons.Add(FString::Printf(
			TEXT("Preview map file changed during the read-only check or could not be re-read: %s"), *PreviewRecheckError));
	}

	const TArray<FString> FinalDirtyPackages = GetDirtyPackagePaths();
	bDirtyPackagesClearAfterRestore = InitialDirtyPackages.IsEmpty() && FinalDirtyPackages.IsEmpty();
	if (!bDirtyPackagesClearAfterRestore)
	{
		FailureReasons.Add(FString::Printf(
			TEXT("Dirty package state is not clear after restoration: before=%d after=%d."),
			InitialDirtyPackages.Num(), FinalDirtyPackages.Num()));
	}

	const bool bBothMapsCaptured = bMapLoadAttempted
		&& Source.bLoadMapReturned && Source.bWorldIdentityMatches
		&& Preview.bLoadMapReturned && Preview.bWorldIdentityMatches;
	FIMComparison SourceDuringDuplicateComparison;
	if (State.bPristineProbeSaved)
	{
		SourceDuringDuplicateComparison = CompareSnapshots(
			State.SourceImmediatelyBeforeDuplicate, State.SourceImmediatelyAfterProbeSave);
		if (!State.SourceImmediatelyBeforeDuplicate.bCoverageComplete
			|| !State.SourceImmediatelyAfterProbeSave.bCoverageComplete
			|| SourceDuringDuplicateComparison.HardMismatchCount > 0)
		{
			FailureReasons.Add(TEXT("Source Landscape data changed or became unreadable during pristine duplication/save."));
		}
	}
	if (bBothMapsCaptured)
	{
		Comparison = CompareSnapshots(Source, Preview);
		if (Comparison.HardMismatchCount > 0)
		{
			FailureReasons.Add(TEXT("One or more Landscape identity, topology, height, source-mip, collision-height, weight, physical-material-cell, or hole-mask comparisons failed."));
		}
	}

	if (!Source.bPackageRead || !Preview.bPackageRead
		|| !Source.bPackageUnchanged || !Preview.bPackageUnchanged)
	{
		InconclusiveReasons.Add(TEXT("Map package SHA and byte coverage are incomplete or unstable."));
	}
	if (!Source.bCoverageComplete || !Preview.bCoverageComplete)
	{
		InconclusiveReasons.Add(TEXT("At least one map has incomplete Landscape source or collision data coverage."));
	}
	if (!bBothMapsCaptured)
	{
		InconclusiveReasons.Add(TEXT("Both target maps were not loaded and identified successfully."));
	}
	if (!bStartupMapRestored)
	{
		FailureReasons.Add(TEXT("The original startup map was not restored."));
	}

	const bool bDataMismatch = Comparison.HardMismatchCount > 0;
	const bool bStateFailure = !FailureReasons.IsEmpty();
	const bool bCoveragePass = bBothMapsCaptured && Source.bCoverageComplete && Preview.bCoverageComplete
		&& Source.bPackageRead && Preview.bPackageRead && bSourcePackageHashStable && bPreviewPackageHashStable
		&& bStartupMapRestored && bDirtyPackagesClearAfterRestore;
	const FString Status = bStateFailure || bDataMismatch
		? TEXT("FAIL")
		: bCoveragePass ? TEXT("PASS") : TEXT("INCONCLUSIVE");

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("im_acoustic_rural_landscape_copy_identity_v1"));
	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetStringField(TEXT("status"), Status);
	Report->SetStringField(TEXT("source_sha_algorithm"), TEXT("SHA-1"));
	Report->SetStringField(TEXT("preview_sha_algorithm"), TEXT("SHA-1"));
	Report->SetStringField(TEXT("report_path"), ReportPath);
	Report->SetBoolField(TEXT("passive_render_producer_observer_enabled"), State.RenderProducerSummary.IsValid());
	if (State.RenderProducerSummary)
		Report->SetObjectField(TEXT("passive_render_producer_observer"), State.RenderProducerSummary.ToSharedRef());
	Report->SetBoolField(TEXT("render_descriptor_dump_requested"), State.RenderDescriptorDump.IsValid());
	if (DescriptorSummary) Report->SetObjectField(TEXT("render_descriptor_dump"), DescriptorSummary.ToSharedRef());
	Report->SetBoolField(TEXT("map_load_attempted"), bMapLoadAttempted);
	Report->SetBoolField(TEXT("maps_or_assets_saved"), State.bPristineProbeSaved);
	Report->SetStringField(TEXT("test_mode"), State.bRestoreFinalDataProbe
		? TEXT("PRISTINE_DUPLICATE_EXACT_FINAL_RESTORE_SAVE_RELOAD_NO_PM_AUTHORING")
		: State.bPristineDuplicateProbe ? TEXT("PRISTINE_DUPLICATE_SAVE_RELOAD_NO_PM_AUTHORING") : TEXT("EXISTING_SOURCE_PREVIEW_READ_ONLY"));
	if (State.bPristineDuplicateProbe)
	{
		Report->SetStringField(TEXT("created_probe_map_package"), State.TargetPreviewPackage);
		Report->SetStringField(TEXT("probe_write_scope"), TEXT("Only a new plugin Tests map is saved. Original Rural and existing preview map packages are not saved or overwritten."));
		Report->SetObjectField(TEXT("source_immediately_before_duplicate"), MakeMapJson(State.SourceImmediatelyBeforeDuplicate));
		Report->SetObjectField(TEXT("duplicate_before_save"), MakeMapJson(State.DuplicateBeforeSave));
		Report->SetObjectField(TEXT("duplicate_immediately_after_save"), MakeMapJson(State.DuplicateImmediatelyAfterSave));
		Report->SetObjectField(TEXT("source_immediately_after_probe_save"), MakeMapJson(State.SourceImmediatelyAfterProbeSave));
		Report->SetObjectField(TEXT("source_during_duplicate_comparison"), MakeComparisonJson(
			SourceDuringDuplicateComparison, State.SourceImmediatelyBeforeDuplicate, State.SourceImmediatelyAfterProbeSave));
		Report->SetObjectField(TEXT("duplicate_before_save_comparison"), MakeComparisonJson(
			CompareSnapshots(State.SourceImmediatelyBeforeDuplicate, State.DuplicateBeforeSave),
			State.SourceImmediatelyBeforeDuplicate, State.DuplicateBeforeSave));
		Report->SetStringField(TEXT("before_save_comparison_scope"), TEXT("In-memory duplicate observation. Unregistered runtime collision may be unavailable; this provisional comparison is not a saved-map acceptance gate."));
		Report->SetObjectField(TEXT("duplicate_save_comparison"), MakeComparisonJson(
			CompareSnapshots(State.DuplicateBeforeSave, State.DuplicateImmediatelyAfterSave),
			State.DuplicateBeforeSave, State.DuplicateImmediatelyAfterSave));
		Report->SetObjectField(TEXT("duplicate_reload_comparison"), MakeComparisonJson(
			CompareSnapshots(State.bRestoredMapSaved ? State.DuplicateAfterRestoredSave : State.DuplicateImmediatelyAfterSave, State.Preview),
			State.bRestoredMapSaved ? State.DuplicateAfterRestoredSave : State.DuplicateImmediatelyAfterSave, State.Preview));
		Report->SetStringField(TEXT("save_reload_comparison_scope"), TEXT("The same duplicate is captured immediately after SaveLevel and before LoadMap. These stage comparisons locate source-array changes; unavailable unregistered collision is provisional and does not relax the final saved-map gate."));
		if (State.bRestoreFinalDataProbe)
		{
			Report->SetBoolField(TEXT("final_data_snapshot_captured"), State.bFinalDataSnapshotCaptured);
			Report->SetBoolField(TEXT("final_data_restored"), State.bFinalDataRestored);
			Report->SetBoolField(TEXT("restored_map_saved"), State.bRestoredMapSaved);
			Report->SetNumberField(TEXT("first_save_frame"), double(State.FirstPristineSaveFrame));
			Report->SetNumberField(TEXT("restored_save_frame"), double(State.RestoredPristineSaveFrame));
			Report->SetNumberField(TEXT("final_data_texture_source_bytes"), double(State.FinalDataTextureSourceBytes));
			Report->SetNumberField(TEXT("final_data_collision_height_bytes"), double(State.FinalDataCollisionHeightBytes));
			Report->SetNumberField(TEXT("final_data_landscape_components"), State.FinalDataLandscapeComponents);
			Report->SetNumberField(TEXT("final_data_unique_textures"), State.FinalDataUniqueTextures);
			Report->SetObjectField(TEXT("duplicate_immediately_after_restore"), MakeMapJson(State.DuplicateImmediatelyAfterRestore));
			Report->SetObjectField(TEXT("duplicate_after_restored_save"), MakeMapJson(State.DuplicateAfterRestoredSave));
			Report->SetObjectField(TEXT("restore_raw_comparison"), MakeComparisonJson(
				CompareSnapshots(State.DuplicateBeforeSave, State.DuplicateImmediatelyAfterRestore),
				State.DuplicateBeforeSave, State.DuplicateImmediatelyAfterRestore));
			Report->SetObjectField(TEXT("restored_save_comparison"), MakeComparisonJson(
				CompareSnapshots(State.DuplicateImmediatelyAfterRestore, State.DuplicateAfterRestoredSave),
				State.DuplicateImmediatelyAfterRestore, State.DuplicateAfterRestoredSave));
			Report->SetStringField(TEXT("restore_scope"), TEXT("Experimental exact final-texture/collision restoration in a new plugin Tests map only. Existing EditLayers/DLD/topology/PM are retained. The complete source-versus-normally-loaded-preview gate is unchanged; a successful Restore or second Save alone is not acceptance."));
		}
	}
	Report->SetBoolField(TEXT("startup_map_restored"), bStartupMapRestored);
	Report->SetBoolField(TEXT("startup_restore_identity_stable"), State.bStartupRestoreIdentityStable);
	Report->SetNumberField(TEXT("startup_restore_wait_frame_count"), State.StartupRestoreWaitFrameCount);
	Report->SetNumberField(TEXT("startup_restore_probe_count"), State.StartupRestoreProbeCount);
	Report->SetNumberField(TEXT("startup_restore_wait_seconds"), State.StartupRestoreWaitSeconds);
	Report->SetStringField(TEXT("startup_map_package"), StartupMapPackage);
	Report->SetStringField(TEXT("startup_map_filename"), StartupFilename);
	Report->SetStringField(TEXT("map_readiness_contract"),
		TEXT("After each map load, normal editor automation ticks must observe the target world identity, all referenced streaming levels loaded, and non-empty equal Landscape/collision component counts stable across three probes separated by at least 0.1 seconds. Timeout leaves data coverage incomplete."));
	Report->SetNumberField(TEXT("preexisting_dirty_package_count"), InitialDirtyPackages.Num());
	Report->SetArrayField(TEXT("preexisting_dirty_package_paths"), ToJsonStringArray(InitialDirtyPackages));
	Report->SetNumberField(TEXT("dirty_package_count_after_restore"), FinalDirtyPackages.Num());
	Report->SetArrayField(TEXT("dirty_package_paths_after_restore"), ToJsonStringArray(FinalDirtyPackages));
	Report->SetBoolField(TEXT("dirty_packages_clear_after_restore"), bDirtyPackagesClearAfterRestore);
	Report->SetObjectField(TEXT("source"), MakeMapJson(Source));
	Report->SetObjectField(TEXT("preview"), MakeMapJson(Preview));
	Report->SetObjectField(TEXT("comparison"), MakeComparisonJson(Comparison, Source, Preview));

	TSharedRef<FJsonObject> SampleUnits = MakeShared<FJsonObject>();
	SampleUnits->SetStringField(TEXT("landscape_height_sample_count"), TEXT("heightmap source texels carrying height vertex samples"));
	SampleUnits->SetStringField(TEXT("landscape_height_source_tile_shape"), TEXT("(SubsectionSizeQuads + 1) * NumSubsections per dimension"));
	SampleUnits->SetStringField(TEXT("collision_height_raw_bulk_element_count"), TEXT("collision height vertices, including optional simple-collision vertices"));
	SampleUnits->SetStringField(TEXT("runtime_collision_height_values"), TEXT("FillHeightTile world-space Z in Unreal centimeters; raw uint16 decoding is (sample - 32768) / 128 before component world scaling."));
	SampleUnits->SetStringField(TEXT("physical_material_cell_count"), TEXT("collision cells: CollisionSizeQuads x CollisionSizeQuads"));
	SampleUnits->SetStringField(TEXT("physical_material_hole_cell_count"), TEXT("collision cells whose registered face material index is 255"));
	SampleUnits->SetStringField(TEXT("texture_source_mip_bytes_read"), TEXT("unique raw source-mip payload bytes across heightmap and weightmap textures"));
	Report->SetObjectField(TEXT("sample_unit_contract"), SampleUnits);

	TSharedRef<FJsonObject> PmCachePolicy = MakeShared<FJsonObject>();
	PmCachePolicy->SetStringField(TEXT("dominant_layer_policy"),
		TEXT("DominantLayerData is read and reported. Empty source bulk becoming non-empty is allowed only when the source component has no valid Landscape physical-material inputs; otherwise a raw mismatch fails."));
	PmCachePolicy->SetStringField(TEXT("physical_material_correctness_owner"),
		TEXT("RuralPreviewPrepare validates cell IDs against material-source inputs; this test checks source-preview cell and hole-mask identity."));
	Report->SetObjectField(TEXT("physical_material_cache_policy"), PmCachePolicy);
	Report->SetArrayField(TEXT("inconclusive_reasons"), ToJsonStringArray(InconclusiveReasons));
	Report->SetArrayField(TEXT("failure_reasons"), ToJsonStringArray(FailureReasons));

	const bool bDirectoryReady = IFileManager::Get().MakeDirectory(*ReportDirectory, true);
	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
	Writer->Close();
	const bool bReportWritten = bDirectoryReady && bSerialized
		&& FFileHelper::SaveStringToFile(JsonText, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const FString FinalStatus = bReportWritten ? Status : TEXT("FAIL");
	const bool bPass = FinalStatus == TEXT("PASS");
	UE_LOG(LogTemp, Display,
		TEXT("IMLogs RuralLandscapeCopyIdentity status=%s source_components=%d preview_components=%d differences=%d report=%s"),
		*FinalStatus,
		Source.LandscapeComponentCount, Preview.LandscapeComponentCount,
		Comparison.HardMismatchCount, *ReportPath);
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));

	if (bPass)
	{
		Test->AddInfo(FString::Printf(
			TEXT("Landscape source-to-preview identity report: %s"), *ReportPath));
		return true;
	}

	if (!bReportWritten)
	{
		Test->AddError(TEXT("JSON report directory creation, serialization, or write failed."));
	}
	Test->AddError(FString::Printf(
		TEXT("Landscape source-to-preview identity did not PASS (status=%s). Report: %s"),
		*FinalStatus, *ReportPath));
	return false;
}

static void InitializeRunContext(FIMRunContext& State, FAutomationTestBase* Test, const bool bPristineDuplicateProbe, const bool bRestoreFinalDataProbe)
{
	State.Test = Test;
	State.RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	State.bPristineDuplicateProbe = bPristineDuplicateProbe;
	State.bRestoreFinalDataProbe = bRestoreFinalDataProbe;
	State.TargetPreviewPackage = bPristineDuplicateProbe
		? FString::Printf(TEXT("/IceMoonAcousticField/Tests/IM_RuralLandscapePristine_%s"), *State.RunId)
		: FString(PreviewMapPackage);
	State.ReportDirectory = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialDrive"));
	State.ReportPath = FPaths::Combine(
		State.ReportDirectory, FString::Printf(TEXT("landscape_copy_%s.json"), *State.RunId));
	State.StartupMapPackage = GEditor && GEditor->GetEditorWorldContext().World()
		? GEditor->GetEditorWorldContext().World()->GetOutermost()->GetName()
		: FString();

	State.InitialDirtyPackages = GetDirtyPackagePaths();
	State.bInitialEditorWorldValid = GEditor
		&& IsValid(GEditor->GetEditorWorldContext().World())
		&& GEditor->GetEditorWorldContext().World()->WorldType == EWorldType::Editor;
	State.bStartupMapRestorable = State.bInitialEditorWorldValid
		&& IsSavedStartupMapRestorable(State.StartupMapPackage, State.StartupFilename);

	State.Source.RequestedPackage = SourceMapPackage;
	State.Preview.RequestedPackage = State.TargetPreviewPackage;
	FString SourceFileHashError;
	State.Source.PackageFilename = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(SourceMapPackage, FPackageName::GetMapPackageExtension()));
	State.Source.bPackageRead = ReadFileSha1(
		State.Source.PackageFilename, State.Source.PackageSha1, State.Source.PackageBytesRead, SourceFileHashError);
	if (!State.Source.bPackageRead)
	{
		State.InconclusiveReasons.Add(FString::Printf(
			TEXT("Source map file SHA/readback is unavailable: %s"), *SourceFileHashError));
	}

	FString PreviewFileHashError;
	State.Preview.PackageFilename = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(State.TargetPreviewPackage, FPackageName::GetMapPackageExtension()));
	if (!bPristineDuplicateProbe)
	{
		State.Preview.bPackageRead = ReadFileSha1(
			State.Preview.PackageFilename, State.Preview.PackageSha1, State.Preview.PackageBytesRead, PreviewFileHashError);
		if (!State.Preview.bPackageRead)
		{
			State.InconclusiveReasons.Add(FString::Printf(
				TEXT("Preview map file SHA/readback is unavailable: %s"), *PreviewFileHashError));
		}
	}

	State.SourcePackageFilenameBeforeLoad = State.Source.PackageFilename;
	State.SourcePackageSha1BeforeLoad = State.Source.PackageSha1;
	State.SourcePackageBytesReadBeforeLoad = State.Source.PackageBytesRead;
	State.bSourcePackageReadBeforeLoad = State.Source.bPackageRead;
	State.PreviewPackageFilenameBeforeLoad = State.Preview.PackageFilename;
	State.PreviewPackageSha1BeforeLoad = State.Preview.PackageSha1;
	State.PreviewPackageBytesReadBeforeLoad = State.Preview.PackageBytesRead;
	State.bPreviewPackageReadBeforeLoad = State.Preview.bPackageRead;

	if (!State.bInitialEditorWorldValid)
	{
		State.InconclusiveReasons.Add(TEXT("No saved Editor world is available as a restorable startup map."));
	}
	if (!State.bStartupMapRestorable)
	{
		State.InconclusiveReasons.Add(TEXT("The startup map has no readable saved .umap file and cannot be safely restored."));
	}
	if (State.InitialDirtyPackages.Num() > 0)
	{
		State.InconclusiveReasons.Add(TEXT("Pre-existing dirty packages were detected; map switching was skipped to preserve unsaved user state."));
	}

	State.bMayLoadMaps = State.bInitialEditorWorldValid && State.bStartupMapRestorable
		&& State.InitialDirtyPackages.IsEmpty();
	if (State.bMayLoadMaps)
	{
		State.Step = FIMRunContext::EIMStep::LoadSource;
	}
	else
	{
		State.Source.Issues.Add(TEXT("Map load was not attempted because read-only restoration preconditions failed."));
		State.Preview.Issues.Add(TEXT("Map load was not attempted because read-only restoration preconditions failed."));
		State.Source.bCoverageComplete = false;
		State.Preview.bCoverageComplete = false;
		State.Step = FIMRunContext::EIMStep::Finalize;
	}
}

static void StartMapReadinessWait(FIMMapReadinessWait& Wait, const FString& RequestedPackage)
{
	Wait = FIMMapReadinessWait();
	Wait.StartSeconds = FPlatformTime::Seconds();
	Wait.LastProbeSeconds = Wait.StartSeconds;
	Wait.Latest = ReadWorldReadiness(RequestedPackage);
}

static void CaptureMapAfterReadiness(
	FIMRunContext& State,
	FIMMapSnapshot& OutMap,
	const FString& RequestedPackage,
	const bool bLoadMapReturned,
	const bool bSourceMap)
{
	OutMap = CaptureMapSnapshot(RequestedPackage, bLoadMapReturned);
	if (bSourceMap)
	{
		OutMap.PackageFilename = State.SourcePackageFilenameBeforeLoad;
		OutMap.PackageSha1 = State.SourcePackageSha1BeforeLoad;
		OutMap.PackageBytesRead = State.SourcePackageBytesReadBeforeLoad;
		OutMap.bPackageRead = State.bSourcePackageReadBeforeLoad;
	}
	else
	{
		OutMap.PackageFilename = State.PreviewPackageFilenameBeforeLoad;
		OutMap.PackageSha1 = State.PreviewPackageSha1BeforeLoad;
		OutMap.PackageBytesRead = State.PreviewPackageBytesReadBeforeLoad;
		OutMap.bPackageRead = State.bPreviewPackageReadBeforeLoad;
	}

	ApplyReadinessWait(OutMap, State.ActiveMapWait);
	const bool bCapturedInventoryMatchesReadiness =
		OutMap.LandscapeComponentCount == State.ActiveMapWait.Latest.LandscapeComponentCount
		&& OutMap.CollisionComponentCount == State.ActiveMapWait.Latest.CollisionComponentCount
		&& OutMap.StreamingLevelCount == State.ActiveMapWait.Latest.StreamingLevelCount
		&& OutMap.LoadedStreamingLevelCount == State.ActiveMapWait.Latest.LoadedStreamingLevelCount;
	if (State.ActiveMapWait.bConfirmed
		&& (!OutMap.bWorldIdentityMatches || !OutMap.bNonWorldPartition || !bCapturedInventoryMatchesReadiness))
	{
		OutMap.bWorldReadinessConfirmed = false;
		AddIssue(OutMap, TEXT("Landscape or streaming-level inventory changed between readiness confirmation and capture."));
	}
	else if (bLoadMapReturned && !State.ActiveMapWait.bConfirmed)
	{
		AddIssue(OutMap, FString::Printf(
			TEXT("World readiness was not confirmed within %.1f seconds (%d frames, %d probes; latest world_identity=%d non_world_partition=%d Landscape=%d collision=%d loaded streaming levels=%d/%d). Data coverage remains incomplete."),
			State.ActiveMapWait.ElapsedSeconds, State.ActiveMapWait.FrameCount, State.ActiveMapWait.ProbeCount,
			State.ActiveMapWait.Latest.bWorldIdentityMatches ? 1 : 0,
			State.ActiveMapWait.Latest.bNonWorldPartition ? 1 : 0,
			State.ActiveMapWait.Latest.LandscapeComponentCount,
			State.ActiveMapWait.Latest.CollisionComponentCount,
			State.ActiveMapWait.Latest.LoadedStreamingLevelCount,
			State.ActiveMapWait.Latest.StreamingLevelCount));
	}
}

static bool RecordPristinePackageBaseline(FIMRunContext& State, UWorld* SourceWorld, const FString& Filename)
{
	State.SourceImmediatelyAfterProbeSave = CaptureMapSnapshot(SourceMapPackage, true, SourceWorld);
	State.PreviewPackageFilenameBeforeLoad = FPaths::ConvertRelativePathToFull(Filename);
	FString HashError;
	State.bPreviewPackageReadBeforeLoad = ReadFileSha1(
		State.PreviewPackageFilenameBeforeLoad, State.PreviewPackageSha1BeforeLoad,
		State.PreviewPackageBytesReadBeforeLoad, HashError);
	if (!State.bPreviewPackageReadBeforeLoad)
	{
		State.FailureReasons.Add(TEXT("Saved pristine map byte baseline is unavailable: ") + HashError);
		return false;
	}
	return true;
}

static bool CreatePristineDuplicateProbe(FIMRunContext& State)
{
	UWorld* SourceWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!State.Source.bCoverageComplete || !State.Source.bWorldReadinessConfirmed
		|| !IsValid(SourceWorld) || SourceWorld->GetOutermost()->GetName() != SourceMapPackage
		|| !SourceWorld->PersistentLevel || SourceWorld->PersistentLevel->IsUsingExternalActors()
		|| SourceWorld->GetStreamingLevels().Num() != 0)
	{
		State.FailureReasons.Add(TEXT("Pristine duplication requires a ready, complete, non-external-actor source world."));
		return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(
		State.TargetPreviewPackage, FPackageName::GetMapPackageExtension());
	if (!State.TargetPreviewPackage.StartsWith(TEXT("/IceMoonAcousticField/Tests/IM_RuralLandscapePristine_"))
		|| FPackageName::DoesPackageExist(State.TargetPreviewPackage)
		|| FindPackage(nullptr, *State.TargetPreviewPackage)
		|| IFileManager::Get().FileExists(*Filename))
	{
		State.FailureReasons.Add(TEXT("Pristine probe destination is not a new task-owned plugin Tests map."));
		return false;
	}

	State.SourceImmediatelyBeforeDuplicate = CaptureMapSnapshot(SourceMapPackage, true, SourceWorld);
	ObjectTools::FPackageGroupName PackageName;
	PackageName.PackageName = State.TargetPreviewPackage;
	PackageName.ObjectName = FPackageName::GetLongPackageAssetName(State.TargetPreviewPackage);
	TSet<UPackage*> RefusedPackages;
	UWorld* Copy = Cast<UWorld>(ObjectTools::DuplicateSingleObject(SourceWorld, PackageName, RefusedPackages, false));
	if (!IsValid(Copy) || Copy == SourceWorld || !Copy->PersistentLevel
		|| Copy->GetOutermost()->GetName() != State.TargetPreviewPackage
		|| (SourceWorld->PersistentLevel->MapBuildData
			&& Copy->PersistentLevel->MapBuildData == SourceWorld->PersistentLevel->MapBuildData))
	{
		State.FailureReasons.Add(TEXT("Pristine duplicate failed its world/package/build-data ownership gate."));
		return false;
	}
	State.DuplicateBeforeSave = CaptureMapSnapshot(State.TargetPreviewPackage, false, Copy, true);
	FString RestoreError;
	if (State.bRestoreFinalDataProbe)
	{
		if (!State.FinalDataSnapshot.Capture(Copy, State.TargetPreviewPackage, RestoreError))
		{
			State.FailureReasons.Add(TEXT("Exact final-data snapshot did not pass its owned-object/layout gate: ") + RestoreError);
			return false;
		}
		State.bFinalDataSnapshotCaptured = true;
		State.FinalDataTextureSourceBytes = State.FinalDataSnapshot.GetTextureSourceBytes();
		State.FinalDataCollisionHeightBytes = State.FinalDataSnapshot.GetCollisionHeightBytes();
		State.FinalDataLandscapeComponents = State.FinalDataSnapshot.GetLandscapeComponentCount();
		State.FinalDataUniqueTextures = State.FinalDataSnapshot.GetUniqueTextureCount();
	}
	if (!FEditorFileUtils::SaveLevel(Copy->PersistentLevel, Filename))
	{
		State.FailureReasons.Add(TEXT("Pristine plugin probe map save failed."));
		return false;
	}
	State.bPristineProbeSaved = true;
	State.FirstPristineSaveFrame = GFrameCounter;
	// SaveLevel can run Landscape pre-save work. Observe this duplicate before any
	// LoadMap or editor tick, so save-time writes and load-time recomposition differ.
	State.DuplicateImmediatelyAfterSave = CaptureMapSnapshot(State.TargetPreviewPackage, false, Copy, true);
	if (State.bRestoreFinalDataProbe)
	{
		State.DuplicateProbeWorld.Reset(Copy);
		return true;
	}
	return RecordPristinePackageBaseline(State, SourceWorld, Filename);
}

static bool RestorePristineFinalDataProbe(FIMRunContext& State)
{
	UWorld* Copy = State.DuplicateProbeWorld.Get();
	UWorld* SourceWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!IsValid(Copy) || Copy->GetOutermost()->GetName() != State.TargetPreviewPackage
		|| !IsValid(SourceWorld) || SourceWorld->GetOutermost()->GetName() != SourceMapPackage
		|| GFrameCounter <= State.FirstPristineSaveFrame)
	{
		State.FailureReasons.Add(TEXT("Exact restoration lost its owned duplicate/source or next-frame save boundary."));
		return false;
	}
	// Run the second save on a later normal Editor frame: same-frame pre-save
	// flush suppression must not manufacture a final-data preservation pass.
	FString RestoreError;
	if (!State.FinalDataSnapshot.Restore(RestoreError))
	{
		State.FailureReasons.Add(TEXT("Exact final-data restoration stopped: ") + RestoreError);
		return false;
	}
	State.bFinalDataRestored = true;
	State.DuplicateImmediatelyAfterRestore = CaptureMapSnapshot(State.TargetPreviewPackage, false, Copy, true);
	const FString Filename = FPackageName::LongPackageNameToFilename(
		State.TargetPreviewPackage, FPackageName::GetMapPackageExtension());
	if (!FEditorFileUtils::SaveLevel(Copy->PersistentLevel, Filename))
	{
		State.FailureReasons.Add(TEXT("Restored pristine plugin probe map save failed."));
		return false;
	}
	State.bRestoredMapSaved = true;
	State.RestoredPristineSaveFrame = GFrameCounter;
	State.DuplicateAfterRestoredSave = CaptureMapSnapshot(State.TargetPreviewPackage, false, Copy, true);
	return RecordPristinePackageBaseline(State, SourceWorld, Filename);
}

static void StartStartupRestoreWait(FIMRunContext& State)
{
	State.StartupRestoreStartSeconds = FPlatformTime::Seconds();
	State.StartupRestoreLastProbeSeconds = State.StartupRestoreStartSeconds;
	State.StartupRestoreWaitSeconds = 0.0;
	State.StartupRestoreWaitFrameCount = 0;
	State.StartupRestoreProbeCount = 0;
	State.StartupRestoreStableProbeCount = 0;
	State.bStartupRestoreIdentityStable = false;
}

static bool UpdateRunContext(FIMRunContext& State)
{
	if (State.RenderDescriptorDump) State.RenderDescriptorDump->Tick();
	if (State.RenderProducerObserver) State.RenderProducerObserver->SetStep(uint32(State.Step));
	switch (State.Step)
	{
	case FIMRunContext::EIMStep::LoadSource:
		State.bMapLoadAttempted = true;
		State.bSourceLoadReturned = FEditorFileUtils::LoadMap(SourceMapPackage, false, false);
		if (State.bSourceLoadReturned && State.RenderDescriptorDump)
			State.RenderDescriptorDump->Arm(State.RunId, State.ReportDirectory);
		StartMapReadinessWait(State.ActiveMapWait, SourceMapPackage);
		State.Step = State.bSourceLoadReturned
			? FIMRunContext::EIMStep::WaitForSource
			: FIMRunContext::EIMStep::CaptureSource;
		return false;

	case FIMRunContext::EIMStep::WaitForSource:
		if (UpdateMapReadinessWait(State.ActiveMapWait, SourceMapPackage))
		{
			State.Step = FIMRunContext::EIMStep::CaptureSource;
		}
		return false;

	case FIMRunContext::EIMStep::CaptureSource:
		CaptureMapAfterReadiness(State, State.Source, SourceMapPackage, State.bSourceLoadReturned, true);
		State.Step = FIMRunContext::EIMStep::LoadPreview;
		return false;

	case FIMRunContext::EIMStep::LoadPreview:
		if (State.bPristineDuplicateProbe && !State.bPristineProbeSaved && !CreatePristineDuplicateProbe(State))
		{
			State.Step = FIMRunContext::EIMStep::RestoreStartup;
			return false;
		}
		if (State.bRestoreFinalDataProbe && !State.bRestoredMapSaved)
		{
			State.Step = FIMRunContext::EIMStep::RestorePristineFinal;
			return false;
		}
		// The raw observations above own no UObjects. Release the restore helper's
		// textures/components/world before LoadMap checks old-world GC reachability.
		State.FinalDataSnapshot.Reset();
		State.DuplicateProbeWorld.Reset();
		State.bPreviewLoadReturned = FEditorFileUtils::LoadMap(State.TargetPreviewPackage, false, false);
		StartMapReadinessWait(State.ActiveMapWait, State.TargetPreviewPackage);
		State.Step = State.bPreviewLoadReturned
			? FIMRunContext::EIMStep::WaitForPreview
			: FIMRunContext::EIMStep::CapturePreview;
		return false;

	case FIMRunContext::EIMStep::RestorePristineFinal:
		if (GFrameCounter <= State.FirstPristineSaveFrame) return false;
		State.Step = RestorePristineFinalDataProbe(State)
			? FIMRunContext::EIMStep::LoadPreview : FIMRunContext::EIMStep::RestoreStartup;
		return false;

	case FIMRunContext::EIMStep::WaitForPreview:
		if (UpdateMapReadinessWait(State.ActiveMapWait, State.TargetPreviewPackage))
		{
			State.Step = FIMRunContext::EIMStep::CapturePreview;
		}
		return false;

	case FIMRunContext::EIMStep::CapturePreview:
		CaptureMapAfterReadiness(State, State.Preview, State.TargetPreviewPackage, State.bPreviewLoadReturned, false);
		State.Step = FIMRunContext::EIMStep::RestoreStartup;
		return false;

	case FIMRunContext::EIMStep::RestoreStartup:
		// Also cover capture/restore/save failures before a preview load was attempted.
		State.FinalDataSnapshot.Reset();
		State.DuplicateProbeWorld.Reset();
		if (State.bMapLoadAttempted && State.bStartupMapRestorable)
		{
			State.bRestoreLoadReturned = FEditorFileUtils::LoadMap(State.StartupMapPackage, false, false);
			StartStartupRestoreWait(State);
			State.Step = State.bRestoreLoadReturned
				? FIMRunContext::EIMStep::WaitForStartupRestore
				: FIMRunContext::EIMStep::Finalize;
		}
		else
		{
			State.Step = FIMRunContext::EIMStep::Finalize;
		}
		return false;

	case FIMRunContext::EIMStep::WaitForStartupRestore:
	{
		++State.StartupRestoreWaitFrameCount;
		const double NowSeconds = FPlatformTime::Seconds();
		State.StartupRestoreWaitSeconds = NowSeconds - State.StartupRestoreStartSeconds;
		if (NowSeconds - State.StartupRestoreLastProbeSeconds >= MapReadinessPollIntervalSeconds)
		{
			State.StartupRestoreLastProbeSeconds = NowSeconds;
			++State.StartupRestoreProbeCount;
			UWorld* RestoredWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const bool bStartupIdentityMatches = IsValid(RestoredWorld)
				&& RestoredWorld->GetOutermost()->GetName() == State.StartupMapPackage;
			State.StartupRestoreStableProbeCount = bStartupIdentityMatches
				? State.StartupRestoreStableProbeCount + 1
				: 0;
			State.bStartupRestoreIdentityStable =
				State.StartupRestoreStableProbeCount >= StableMapReadinessProbeCount;
		}
		if (State.bStartupRestoreIdentityStable
			|| State.StartupRestoreWaitSeconds >= MapReadinessTimeoutSeconds)
		{
			State.Step = FIMRunContext::EIMStep::Finalize;
		}
		return false;
	}

	case FIMRunContext::EIMStep::Finalize:
		FinalizeRunTest(State);
		State.Step = FIMRunContext::EIMStep::Done;
		return true;

	case FIMRunContext::EIMStep::Done:
		return true;
	}

	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FIMAcousticRuralLandscapeCopyIdentityWorkflowCommand,
	TSharedRef<FIMRunContext>,
	Context);
bool FIMAcousticRuralLandscapeCopyIdentityWorkflowCommand::Update()
{
	return UpdateRunContext(*Context);
}

static bool RunTest(FAutomationTestBase* Test, const bool bPristineDuplicateProbe = false,
	const bool bRestoreFinalDataProbe = false, const bool bObserveRenderProducer = false,
	const bool bDumpRenderDescriptors = false)
{
	TSharedRef<FIMRunContext> Context = MakeShared<FIMRunContext>();
	InitializeRunContext(*Context, Test, bPristineDuplicateProbe, bRestoreFinalDataProbe);
	if (bObserveRenderProducer && Context->bMayLoadMaps)
		Context->RenderProducerObserver = FSceneViewExtensions::NewExtension<FIMRenderProducerObserver>(Context->RunId);
	if (bDumpRenderDescriptors && Context->bMayLoadMaps)
		Context->RenderDescriptorDump = MakeUnique<FIMRenderDescriptorDump>();
	ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticRuralLandscapeCopyIdentityWorkflowCommand(Context));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralLandscapeCopyIdentityTest,
	"IceMoon.AcousticField.Rural.LandscapeCopyIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralLandscapeCopyIdentityTest::RunTest(const FString&)
{
	return IMAcousticRuralLandscapeCopyTestPrivate::RunTest(this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralLandscapeRenderProducerContextTest,
	"IceMoon.AcousticField.Rural.LandscapeRenderProducerContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralLandscapeRenderProducerContextTest::RunTest(const FString&)
{
	return IMAcousticRuralLandscapeCopyTestPrivate::RunTest(this, false, false, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralLandscapeRenderProducerDescriptorDumpTest,
	"IceMoon.AcousticField.Rural.LandscapeRenderProducerDescriptorDump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralLandscapeRenderProducerDescriptorDumpTest::RunTest(const FString&)
{
	return IMAcousticRuralLandscapeCopyTestPrivate::RunTest(this, false, false, true, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralLandscapePristineDuplicateIdentityTest,
	"IceMoon.AcousticField.Rural.LandscapePristineDuplicateIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralLandscapePristineDuplicateIdentityTest::RunTest(const FString&)
{
	return IMAcousticRuralLandscapeCopyTestPrivate::RunTest(this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralLandscapeExactFinalRestoreIdentityTest,
	"IceMoon.AcousticField.Rural.LandscapeExactFinalRestoreIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticRuralLandscapeExactFinalRestoreIdentityTest::RunTest(const FString&)
{
	return IMAcousticRuralLandscapeCopyTestPrivate::RunTest(this, true, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticLandscapeCollisionLayerIdentityContractTest,
	"IceMoon.AcousticField.PhysicalMaterial.LandscapeCollisionLayerIdentityContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticLandscapeCollisionLayerIdentityContractTest::RunTest(const FString&)
{
	using namespace IMAcousticRuralLandscapeCopyTestPrivate;
	FIMComponentSnapshot Source;
	Source.DominantLayerData.bReadable = true;
	Source.DominantLayerData.bPresent = true;
	Source.UsedDominantLayerIndices = { 0, 1 };
	Source.CollisionLayerNames = { TEXT("Soil"), TEXT("Stone"), TEXT("UnusedGrass") };
	FIMComponentSnapshot Target = Source;
	Target.CollisionLayerNames.Pop();
	bool bOk = TestTrue(TEXT("Only unreferenced trailing cache entries may be pruned"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target.CollisionLayerNames[1] = TEXT("Wood");
	bOk &= TestFalse(TEXT("Changing the named layer at a used byte index fails"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target.CollisionLayerNames = { TEXT("Stone"), TEXT("Soil") };
	bOk &= TestFalse(TEXT("Permuting used layer slots without changing DLD fails"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target.CollisionLayerNames = { TEXT("Soil") };
	bOk &= TestFalse(TEXT("A used byte index outside the target table fails"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target = Source;
	Target.UsedDominantLayerIndices = { 0 };
	bOk &= TestFalse(TEXT("Different actual used-index sets fail"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target = Source;
	Target.DominantLayerData.bReadable = false;
	bOk &= TestFalse(TEXT("Unreadable dominant-layer data fails"),
		EqualUsedCollisionLayerNames(Source, Target));
	Source.DominantLayerData.bPresent = false;
	Source.UsedDominantLayerIndices.Reset();
	Source.bHasValidPhysicalMaterialInputs = true;
	Target.DominantLayerData.bReadable = true;
	bOk &= TestFalse(TEXT("Empty source with valid PM inputs cannot take the cache exception"),
		EqualUsedCollisionLayerNames(Source, Target));
	Source.bHasValidPhysicalMaterialInputs = false;
	bOk &= TestTrue(TEXT("The original no-PM-input empty-source cache exception is retained"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target.CollisionLayerNames = { TEXT("Soil") };
	bOk &= TestFalse(TEXT("The empty-source exception rejects an out-of-range target DLD index"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target.CollisionLayerNames = { TEXT("Soil"), TEXT("<INVALID_LAYER_INFO>") };
	bOk &= TestFalse(TEXT("The empty-source exception rejects an invalid target layer"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target.CollisionLayerNames = { TEXT("Soil"), FName(NAME_None).ToString() };
	bOk &= TestFalse(TEXT("The empty-source exception rejects an unnamed target layer"),
		EqualUsedCollisionLayerNames(Source, Target));
	Target = Source;
	Target.DominantLayerData.bPresent = true;
	bOk &= TestFalse(TEXT("Present target DLD without any observed byte indices fails"),
		EqualUsedCollisionLayerNames(Source, Target));
	AddInfo(TEXT("Collision layer-name contract covers pruning and ten wrong/missing-data controls; raw DLD equality remains an independent full-map gate."));
	return bOk;
}

#endif
