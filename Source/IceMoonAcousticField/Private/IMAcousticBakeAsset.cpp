#include "IMAcousticBakeAsset.h"
#include "Misc/SecureHash.h"
#include "Misc/Compression.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticAudioRenderer.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace IMAcousticBakeAssetPrivate
{
// Packed sections are local asset storage only. Steam Audio always receives
// the exact uncompressed bytes that its serializer produced.
bool IMUnpackSection(const TArray<uint8>& Packed, TArray<uint8>& Raw, FString& Failure)
{
	if (Packed.Num() <= 4)
	{
		Failure = TEXT("Acoustic compressed section is incomplete.");
		return false;
	}
	uint32 RawSize = 0;
	for (int32 I = 0; I < 4; ++I) RawSize |= uint32(Packed[I]) << (I * 8);
	if (RawSize == 0 || RawSize > MAX_int32)
	{
		Failure = TEXT("Acoustic compressed section has an invalid raw size.");
		return false;
	}
	Raw.SetNumUninitialized(static_cast<int32>(RawSize));
	if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), int64(RawSize),
		Packed.GetData() + 4, int64(Packed.Num() - 4)))
	{
		Failure = TEXT("Acoustic compressed section cannot be decoded.");
		Raw.Reset();
		return false;
	}
	return true;
}

constexpr uint32 IMV4ChunkBytes = 1024u * 1024u;
constexpr int32 IMV4HeaderBytes = 20;
constexpr int32 IMV4MagicBytes = 8;
constexpr uint8 IMV4Magic[] = {'I', 'M', 'A', 'C', 'P', 'K', '4', 0};

void IMAppendU32LE(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

bool IMReadU32LE(const TArray<uint8>& Bytes, int32& Cursor, uint32& Value)
{
	if (Cursor < 0 || Cursor > Bytes.Num() || Bytes.Num() - Cursor < 4) return false;
	Value = uint32(Bytes[Cursor]) | (uint32(Bytes[Cursor + 1]) << 8)
		| (uint32(Bytes[Cursor + 2]) << 16) | (uint32(Bytes[Cursor + 3]) << 24);
	Cursor += 4;
	return true;
}

void IMShuffleFourBytePlanes(const uint8* Source, int32 NumBytes, TArray<uint8>& Shuffled)
{
	const int32 Groups = NumBytes / 4;
	const int32 PlaneBytes = Groups;
	const int32 FullBytes = Groups * 4;
	Shuffled.SetNumUninitialized(NumBytes);
	for (int32 Group = 0; Group < Groups; ++Group)
	{
		for (int32 Lane = 0; Lane < 4; ++Lane)
		{
			Shuffled[Lane * PlaneBytes + Group] = Source[Group * 4 + Lane];
		}
	}
	if (FullBytes < NumBytes)
	{
		FMemory::Memcpy(Shuffled.GetData() + FullBytes, Source + FullBytes, NumBytes - FullBytes);
	}
}

void IMUnshuffleFourBytePlanes(const uint8* Shuffled, int32 NumBytes, uint8* Destination)
{
	const int32 Groups = NumBytes / 4;
	const int32 PlaneBytes = Groups;
	const int32 FullBytes = Groups * 4;
	for (int32 Group = 0; Group < Groups; ++Group)
	{
		for (int32 Lane = 0; Lane < 4; ++Lane)
		{
			Destination[Group * 4 + Lane] = Shuffled[Lane * PlaneBytes + Group];
		}
	}
	if (FullBytes < NumBytes)
	{
		FMemory::Memcpy(Destination + FullBytes, Shuffled + FullBytes, NumBytes - FullBytes);
	}
}

bool IMReadV4Header(const TArray<uint8>& Packed, int32& Cursor,
	uint32& RawBytes, uint32& ChunkCount, FString& Failure)
{
	if (Packed.Num() < IMV4HeaderBytes)
	{
		Failure = TEXT("Acoustic format 4 header is truncated.");
		return false;
	}
	for (int32 Index = 0; Index < IMV4MagicBytes; ++Index)
	{
		if (Packed[Index] != IMV4Magic[Index])
		{
			Failure = TEXT("Acoustic format 4 magic is invalid.");
			return false;
		}
	}
	Cursor = IMV4MagicBytes;
	uint32 ChunkBytes = 0;
	if (!IMReadU32LE(Packed, Cursor, RawBytes)
		|| !IMReadU32LE(Packed, Cursor, ChunkBytes)
		|| !IMReadU32LE(Packed, Cursor, ChunkCount))
	{
		Failure = TEXT("Acoustic format 4 header is truncated.");
		return false;
	}
	if (RawBytes == 0 || RawBytes > static_cast<uint32>(MAX_int32)
		|| ChunkBytes != IMV4ChunkBytes)
	{
		Failure = TEXT("Acoustic format 4 header has invalid section or chunk dimensions.");
		return false;
	}
	const uint64 ExpectedCount = (static_cast<uint64>(RawBytes) + IMV4ChunkBytes - 1) / IMV4ChunkBytes;
	if (ChunkCount == 0 || ChunkCount != ExpectedCount)
	{
		Failure = TEXT("Acoustic format 4 chunk count does not match the section length.");
		return false;
	}
	return true;
}

bool IMValidateV4Structure(const TArray<uint8>& Packed, FString& Failure)
{
	int32 Cursor = 0;
	uint32 TotalRawBytes = 0;
	uint32 ChunkCount = 0;
	if (!IMReadV4Header(Packed, Cursor, TotalRawBytes, ChunkCount, Failure)) return false;

	uint64 RawOffset = 0;
	for (uint32 Index = 0; Index < ChunkCount; ++Index)
	{
		uint32 ChunkRawBytes = 0;
		uint32 ChunkCompressedBytes = 0;
		if (!IMReadU32LE(Packed, Cursor, ChunkRawBytes)
			|| !IMReadU32LE(Packed, Cursor, ChunkCompressedBytes))
		{
			Failure = TEXT("Acoustic format 4 chunk header is truncated.");
			return false;
		}
		const uint32 ExpectedRawBytes = static_cast<uint32>(FMath::Min<uint64>(
			IMV4ChunkBytes, static_cast<uint64>(TotalRawBytes) - RawOffset));
		int64 CompressedBound = 0;
		if (ChunkRawBytes != ExpectedRawBytes || ChunkCompressedBytes == 0
			|| ChunkCompressedBytes > static_cast<uint32>(Packed.Num() - Cursor)
			|| !FCompression::CompressMemoryBound(NAME_Zlib, CompressedBound, ChunkRawBytes)
			|| CompressedBound <= 0 || ChunkCompressedBytes > static_cast<uint64>(CompressedBound))
		{
			Failure = TEXT("Acoustic format 4 chunk lengths are invalid or truncated.");
			return false;
		}
		Cursor += static_cast<int32>(ChunkCompressedBytes);
		RawOffset += ChunkRawBytes;
	}
	// Full consumption here is container framing. Core FCompression exposes
	// no consumed-input count for suffix bytes inside a declared zlib chunk.
	// The digest protects stored bytes; it is not a producer trust signature.
	if (RawOffset != TotalRawBytes || Cursor != Packed.Num())
	{
		Failure = TEXT("Acoustic format 4 section has inconsistent length or trailing bytes.");
		return false;
	}
	return true;
}

bool IMUnpackV4Section(const TArray<uint8>& Packed, TArray<uint8>& Raw, FString& Failure)
{
	if (!IMValidateV4Structure(Packed, Failure)) return false;

	int32 Cursor = 0;
	uint32 TotalRawBytes = 0;
	uint32 ChunkCount = 0;
	if (!IMReadV4Header(Packed, Cursor, TotalRawBytes, ChunkCount, Failure)) return false;
	Raw.SetNumUninitialized(static_cast<int32>(TotalRawBytes));
	uint64 RawOffset = 0;
	for (uint32 Index = 0; Index < ChunkCount; ++Index)
	{
		uint32 ChunkRawBytes = 0;
		uint32 ChunkCompressedBytes = 0;
		if (!IMReadU32LE(Packed, Cursor, ChunkRawBytes)
			|| !IMReadU32LE(Packed, Cursor, ChunkCompressedBytes))
		{
			Failure = TEXT("Acoustic format 4 chunk header is truncated.");
			Raw.Reset();
			return false;
		}
		TArray<uint8> Shuffled;
		Shuffled.SetNumUninitialized(static_cast<int32>(ChunkRawBytes));
		if (!FCompression::UncompressMemory(NAME_Zlib, Shuffled.GetData(), ChunkRawBytes,
			Packed.GetData() + Cursor, ChunkCompressedBytes))
		{
			Failure = TEXT("Acoustic format 4 chunk cannot be decompressed.");
			Raw.Reset();
			return false;
		}
		IMUnshuffleFourBytePlanes(Shuffled.GetData(), static_cast<int32>(ChunkRawBytes),
			Raw.GetData() + RawOffset);
		Cursor += static_cast<int32>(ChunkCompressedBytes);
		RawOffset += ChunkRawBytes;
	}
	return true;
}

bool IMPackSectionV4(const TArray<uint8>& Raw, TArray<uint8>& Packed, FString& Failure)
{
	if (Raw.IsEmpty())
	{
		Failure = TEXT("Acoustic section cannot be empty.");
		return false;
	}
	const uint32 RawBytes = static_cast<uint32>(Raw.Num());
	const uint32 ChunkCount = static_cast<uint32>((static_cast<uint64>(RawBytes)
		+ IMV4ChunkBytes - 1) / IMV4ChunkBytes);
	TArray<uint8> Result;
	Result.Append(IMV4Magic, IMV4MagicBytes);
	IMAppendU32LE(Result, RawBytes);
	IMAppendU32LE(Result, IMV4ChunkBytes);
	IMAppendU32LE(Result, ChunkCount);

	uint64 RawOffset = 0;
	for (uint32 Index = 0; Index < ChunkCount; ++Index)
	{
		const uint32 ChunkRawBytes = static_cast<uint32>(FMath::Min<uint64>(
			IMV4ChunkBytes, static_cast<uint64>(RawBytes) - RawOffset));
		TArray<uint8> Shuffled;
		IMShuffleFourBytePlanes(Raw.GetData() + RawOffset, static_cast<int32>(ChunkRawBytes), Shuffled);

		int64 CompressedBound = 0;
		if (!FCompression::CompressMemoryBound(NAME_Zlib, CompressedBound, Shuffled.Num())
			|| CompressedBound <= 0 || CompressedBound > MAX_int32)
		{
			Failure = TEXT("Acoustic format 4 compressed chunk exceeds the supported size.");
			return false;
		}
		TArray<uint8> Compressed;
		Compressed.SetNumUninitialized(static_cast<int32>(CompressedBound));
		int64 CompressedBytes = CompressedBound;
		if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedBytes,
			Shuffled.GetData(), Shuffled.Num()) || CompressedBytes <= 0
			|| CompressedBytes > CompressedBound || CompressedBytes > MAX_int32
			|| static_cast<uint64>(Result.Num()) + 8 + static_cast<uint64>(CompressedBytes) > MAX_int32)
		{
			Failure = TEXT("Acoustic format 4 chunk compression failed or exceeded the asset size limit.");
			return false;
		}
		IMAppendU32LE(Result, ChunkRawBytes);
		IMAppendU32LE(Result, static_cast<uint32>(CompressedBytes));
		Result.Append(Compressed.GetData(), static_cast<int32>(CompressedBytes));
		RawOffset += ChunkRawBytes;
	}
	if (RawOffset != RawBytes)
	{
		Failure = TEXT("Acoustic format 4 packing did not consume the complete section.");
		return false;
	}
	Packed = MoveTemp(Result);
	return true;
}

bool ReadMetadata(const FString& Text,TSharedPtr<FJsonObject>& Root,FString& Failure)
{
	auto Reader=TJsonReaderFactory<>::Create(Text);
	if(!FJsonSerializer::Deserialize(Reader,Root)||!Root.IsValid())
	{Failure=TEXT("Invalid acoustic metadata JSON.");return false;}
	auto Number=[](const FJsonObject& O,const TCHAR* Key,double Min,double Max,bool Integer=false)
	{
		double V=0;return O.TryGetNumberField(Key,V)&&FMath::IsFinite(V)&&V>=Min&&V<=Max&&(!Integer||FMath::FloorToDouble(V)==V);
	};
	auto Array=[](const FJsonObject& O,const TCHAR* Key,int32 Count,double Min,double Max)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
		if(!O.TryGetArrayField(Key,Values)||Values->Num()!=Count)return false;
		for(const auto& Item:*Values){double V=0;if(!Item||!Item->TryGetNumber(V)||!FMath::IsFinite(V)||V<Min||V>Max)return false;}
		return true;
	};
	const TArray<TSharedPtr<FJsonValue>> *Materials=nullptr,*Probes=nullptr;
	const TSharedPtr<FJsonObject> *Reflection=nullptr,*Path=nullptr;
	bool Valid=Number(*Root,TEXT("recipe_version"),IMAcousticRecipe::Version,IMAcousticRecipe::Version,true)
		&&Number(*Root,TEXT("sdk_version"),UIMAcousticBakeAsset::SDKVersion,UIMAcousticBakeAsset::SDKVersion,true)
		&&Number(*Root,TEXT("triangles"),1,MAX_int32,true)&&Number(*Root,TEXT("probe_count"),1,MAX_int32,true)
		&&Number(*Root,TEXT("probe_spacing_cm"),25,MAX_flt)&&Number(*Root,TEXT("probe_height_cm"),25,MAX_flt)
		&&Array(*Root,TEXT("bounds_origin_cm"),3,-MAX_dbl,MAX_dbl)&&Array(*Root,TEXT("bounds_extent_cm"),3,SMALL_NUMBER,MAX_flt)
		&&Root->TryGetArrayField(TEXT("materials"),Materials)&&!Materials->IsEmpty()
		&&Root->TryGetArrayField(TEXT("probes_m"),Probes)
		&&Root->TryGetObjectField(TEXT("reflection"),Reflection)&&Root->TryGetObjectField(TEXT("path"),Path);
	if(Valid)
	{
		Valid=Probes->Num()==Root->GetNumberField(TEXT("probe_count"));
		for(const auto& Item:*Materials)
		{
			const TSharedPtr<FJsonObject>* M=nullptr;FString Asset;
			if(!Item||!Item->TryGetObject(M)||!M->IsValid()||!(*M)->TryGetStringField(TEXT("asset_path"),Asset)||Asset.IsEmpty()
				||!Array(**M,TEXT("absorption"),3,0,1)||!Number(**M,TEXT("scattering"),0,1)){Valid=false;break;}
		}
		for(const auto& Item:*Probes)
		{
			const TArray<TSharedPtr<FJsonValue>>* P=nullptr;
			if(!Item||!Item->TryGetArray(P)||P->Num()!=4){Valid=false;break;}
			for(int32 I=0;I<4;++I){double V=0;if(!(*P)[I]||!(*P)[I]->TryGetNumber(V)||!FMath::IsFinite(V)||FMath::Abs(V)>MAX_flt/100.0||(I==3&&V<=0))Valid=false;}
		}
		// Persisted settings must describe the exact pinned recipe, not merely
		// contain plausible numbers; changing quality requires a recipe revision.
		FString Type;
		Valid=Valid&&(*Reflection)->TryGetStringField(TEXT("type"),Type)&&Type==TEXT("CONVOLUTION")
			&&Number(**Reflection,TEXT("num_rays"),IMAcousticRecipe::ReverbNumRays,IMAcousticRecipe::ReverbNumRays)
			&&Number(**Reflection,TEXT("num_bounces"),IMAcousticRecipe::ReverbNumBounces,IMAcousticRecipe::ReverbNumBounces)
			&&Number(**Reflection,TEXT("num_diffuse_samples"),IMAcousticRecipe::ReverbNumDiffuse,IMAcousticRecipe::ReverbNumDiffuse)
			&&Number(**Reflection,TEXT("sim_duration_s"),IMAcousticRecipe::ReverbSimDurationS,IMAcousticRecipe::ReverbSimDurationS)
			&&Number(**Reflection,TEXT("saved_duration_s"),IMAcousticRecipe::ReverbSavedDurationS,IMAcousticRecipe::ReverbSavedDurationS)
			&&Number(**Reflection,TEXT("irradiance_min_m"),IMAcousticRecipe::IrradianceMinM-1.e-8,IMAcousticRecipe::IrradianceMinM+1.e-8)
			&&Number(**Reflection,TEXT("order"),FIMAcousticAudioFrame::Order,FIMAcousticAudioFrame::Order)
			&&Number(**Path,TEXT("num_samples"),IMAcousticRecipe::PathNumSamples,IMAcousticRecipe::PathNumSamples)
			&&Number(**Path,TEXT("radius_m"),IMAcousticRecipe::PathRadiusM,IMAcousticRecipe::PathRadiusM)
			&&Number(**Path,TEXT("threshold"),IMAcousticRecipe::PathThreshold,IMAcousticRecipe::PathThreshold)
			&&Number(**Path,TEXT("vis_range_m"),IMAcousticRecipe::PathVisRangeM,IMAcousticRecipe::PathVisRangeM)
			&&Number(**Path,TEXT("path_range_m"),IMAcousticRecipe::PathRangeM,IMAcousticRecipe::PathRangeM);
	}
	if(!Valid)Failure=TEXT("Acoustic metadata is incomplete or does not match the bake recipe.");
	return Valid;
}
}

FString UIMAcousticBakeAsset::Digest(const FString& Metadata,const TArray<uint8>& Scene, const TArray<uint8>& Probes)
{
	// Length-prefix each section so moving bytes across its boundary changes the
	// digest. SHA1 detects corruption here; it is not a signature/trust mechanism.
	FSHA1 Hash;
	FTCHARToUTF8 Utf8(*Metadata);TArray<uint8> MetadataBytes;
	MetadataBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
	const TArray<uint8>* Sections[] = { &MetadataBytes, &Scene, &Probes };
	for (const TArray<uint8>* Data : Sections)
	{
		const uint32 Count = static_cast<uint32>(Data->Num());
		const uint8 Length[4] = {static_cast<uint8>(Count), static_cast<uint8>(Count >> 8),
			static_cast<uint8>(Count >> 16), static_cast<uint8>(Count >> 24)};
		Hash.Update(Length, sizeof(Length));
		if (Count) { Hash.Update(Data->GetData(), Count); }
	}
	Hash.Final();
	uint8 Bytes[20];
	Hash.GetHash(Bytes);
	return BytesToHex(Bytes, UE_ARRAY_COUNT(Bytes));
}

bool UIMAcousticBakeAsset::Validate(const FString& ExpectedWorld,
	const FString& ExpectedFingerprint, FString& Failure) const
{
	check(IsInGameThread());
	Failure.Reset();
	if ((FormatVersion != 2 && FormatVersion != 3 && FormatVersion != CurrentFormat) || SteamAudioVersion != SDKVersion)
	{ Failure = TEXT("Unsupported acoustic bake format or Steam Audio version."); }
	else if (ExpectedWorld.IsEmpty() || WorldPackage != ExpectedWorld)
	{ Failure = TEXT("Acoustic bake belongs to a different map."); }
	else if (ExpectedFingerprint.IsEmpty() || SceneFingerprint != ExpectedFingerprint)
	{ Failure = TEXT("Acoustic bake is stale: scene, material or probe settings changed."); }
	else if (SceneData.IsEmpty() || ProbeData.IsEmpty()
		|| (FormatVersion == 3 && (SceneData.Num() <= 4 || ProbeData.Num() <= 4))
		|| PayloadDigest != Digest(MetadataJson,SceneData, ProbeData))
	{ Failure = TEXT("Acoustic bake payload is incomplete or corrupt."); }
	else if (FormatVersion == 4
		&& (!IMAcousticBakeAssetPrivate::IMValidateV4Structure(SceneData, Failure) || !IMAcousticBakeAssetPrivate::IMValidateV4Structure(ProbeData, Failure)))
	{ return false; }
	else {TSharedPtr<FJsonObject> Root;IMAcousticBakeAssetPrivate::ReadMetadata(MetadataJson,Root,Failure);}
	return Failure.IsEmpty();
}

bool UIMAcousticBakeAsset::ReadPayload(const FString& ExpectedWorld,
	const FString& ExpectedFingerprint, TArray<uint8>& OutScene,
	TArray<uint8>& OutProbes, FString& Failure) const
{
	check(IsInGameThread());
	OutScene.Reset();
	OutProbes.Reset();
	if (!Validate(ExpectedWorld, ExpectedFingerprint, Failure)) return false;
	if (FormatVersion == 2)
	{
		OutScene = SceneData;
		OutProbes = ProbeData;
		return true;
	}
	if (FormatVersion == 4)
	{
		// Validate has checked both complete containers before either decoded
		// allocation. Decode each chunk once; publish both sections only after
		// both succeed, so a corrupt second section leaves the caller empty.
		TArray<uint8> Scene, Probes;
		if (!IMAcousticBakeAssetPrivate::IMUnpackV4Section(SceneData, Scene, Failure)
			|| !IMAcousticBakeAssetPrivate::IMUnpackV4Section(ProbeData, Probes, Failure)) return false;
		OutScene = MoveTemp(Scene);
		OutProbes = MoveTemp(Probes);
		return true;
	}
	TArray<uint8> Scene, Probes;
	if (!IMAcousticBakeAssetPrivate::IMUnpackSection(SceneData, Scene, Failure)
		|| !IMAcousticBakeAssetPrivate::IMUnpackSection(ProbeData, Probes, Failure)) return false;
	OutScene = MoveTemp(Scene);
	OutProbes = MoveTemp(Probes);
	return true;
}

bool UIMAcousticBakeAsset::CommitCompleteBake(const FString& InWorld,
	const FString& InFingerprint,const FString& InMetadata, TArray<uint8>&& InScene, TArray<uint8>&& InProbes, FString& Failure)
{
	check(IsInGameThread());
	Failure.Reset();
	if (InWorld.IsEmpty() || InFingerprint.IsEmpty() || InScene.IsEmpty() || InProbes.IsEmpty())
	{
		Failure = TEXT("Incomplete bake cannot replace the previous asset.");
		return false;
	}
	TSharedPtr<FJsonObject> Root;if(!IMAcousticBakeAssetPrivate::ReadMetadata(InMetadata,Root,Failure))return false;
	TArray<uint8> PackedScene, PackedProbes;
	if (!IMAcousticBakeAssetPrivate::IMPackSectionV4(InScene, PackedScene, Failure)
		|| !IMAcousticBakeAssetPrivate::IMPackSectionV4(InProbes, PackedProbes, Failure)) return false;
	const FString NewDigest = Digest(InMetadata,PackedScene, PackedProbes);
	Modify();
	WorldPackage = InWorld;
	SceneFingerprint = InFingerprint;
	MetadataJson = InMetadata;
	SceneData = MoveTemp(PackedScene);
	ProbeData = MoveTemp(PackedProbes);
	PayloadDigest = NewDigest;
	FormatVersion = CurrentFormat;
	SteamAudioVersion = SDKVersion;
	MarkPackageDirty();
	return true;
}

bool UIMAcousticBakeAsset::GetProbePreview(const FString& ExpectedWorld,const FString& ExpectedFingerprint,TArray<FVector4>& Out,FVector& OriginCm,FString& Failure) const
{
	check(IsInGameThread());Out.Reset();Failure.Reset();
	if(!Validate(ExpectedWorld,ExpectedFingerprint,Failure))return false;
	TSharedPtr<FJsonObject> Root;if(!IMAcousticBakeAssetPrivate::ReadMetadata(MetadataJson,Root,Failure))return false;
	const auto& OriginCmValues=Root->GetArrayField(TEXT("bounds_origin_cm"));
	OriginCm=FVector(OriginCmValues[0]->AsNumber(),OriginCmValues[1]->AsNumber(),OriginCmValues[2]->AsNumber());
	for(const auto& Value:Root->GetArrayField(TEXT("probes_m")))
	{const auto& SdkProbeMeters=Value->AsArray();Out.Add(FVector4(SdkProbeMeters[0]->AsNumber(),SdkProbeMeters[1]->AsNumber(),SdkProbeMeters[2]->AsNumber(),SdkProbeMeters[3]->AsNumber()));}
	return true;
}
