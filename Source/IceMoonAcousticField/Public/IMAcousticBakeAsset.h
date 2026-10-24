#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "IMAcousticBakeAsset.generated.h"

// The asset is one transaction: scene and probe data are never published separately.
// SDK pointers belong to the worker and must never be serialized into this UObject.
UCLASS()
class ICEMOONACOUSTICFIELD_API UIMAcousticBakeAsset final : public UDataAsset
{
	GENERATED_BODY()
public:
	static constexpr int32 CurrentFormat = 4;
	static constexpr int32 SDKVersion = 0x040801;

	UPROPERTY(VisibleAnywhere, Category="IM|Bake")
	int32 FormatVersion = 0;

	UPROPERTY(VisibleAnywhere, Category="IM|Bake")
	int32 SteamAudioVersion = 0;

	UPROPERTY(VisibleAnywhere, Category="IM|Bake")
	FString WorldPackage;

	// Includes exported geometry, transforms, explicit material coefficients and
	// probe settings. The exporter recomputes it; file timestamps are not an oracle.
	UPROPERTY(VisibleAnywhere, Category="IM|Bake")
	FString SceneFingerprint;

	UPROPERTY(VisibleAnywhere, Category="IM|Bake")
	FString PayloadDigest;

	UPROPERTY(VisibleAnywhere, Category="IM|Bake")
	FString MetadataJson;

	// Format 2 stores raw SDK bytes. Format 3 stores each section as a
	// little-endian uint32 raw length followed by lossless Zlib bytes. Format 4
	// stores fixed-size independently-compressed chunks after a reversible
	// four-byte-plane shuffle. The format version keeps old assets readable
	// without changing the pinned acoustic recipe or bytes passed to Steam Audio.
	UPROPERTY()
	TArray<uint8> SceneData;

	UPROPERTY()
	TArray<uint8> ProbeData;

	bool Validate(const FString& ExpectedWorld, const FString& ExpectedFingerprint, FString& Failure) const;
	bool ReadPayload(const FString& ExpectedWorld, const FString& ExpectedFingerprint,
		TArray<uint8>& OutScene, TArray<uint8>& OutProbes, FString& Failure) const;
	bool CommitCompleteBake(const FString& InWorld, const FString& InFingerprint,
		const FString& InMetadata, TArray<uint8>&& InScene, TArray<uint8>&& InProbes, FString& Failure);
	bool GetProbePreview(const FString& ExpectedWorld,const FString& ExpectedFingerprint,
		TArray<FVector4>& Out, FVector& OriginCm, FString& Failure) const;

private:
	static FString Digest(const FString& Metadata, const TArray<uint8>& Scene, const TArray<uint8>& Probes);
};
