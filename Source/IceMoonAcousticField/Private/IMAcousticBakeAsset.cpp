#include "IMAcousticBakeAsset.h"
#include "Misc/SecureHash.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticAudioRenderer.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace IMAcousticBakeAssetPrivate
{
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
    if (FormatVersion != CurrentFormat || SteamAudioVersion != SDKVersion)
    { Failure = TEXT("Unsupported acoustic bake format or Steam Audio version."); }
    else if (ExpectedWorld.IsEmpty() || WorldPackage != ExpectedWorld)
    { Failure = TEXT("Acoustic bake belongs to a different map."); }
    else if (ExpectedFingerprint.IsEmpty() || SceneFingerprint != ExpectedFingerprint)
    { Failure = TEXT("Acoustic bake is stale: scene, material or probe settings changed."); }
    else if (SceneData.IsEmpty() || ProbeData.IsEmpty() || PayloadDigest != Digest(MetadataJson,SceneData, ProbeData))
    { Failure = TEXT("Acoustic bake payload is incomplete or corrupt."); }
    else {TSharedPtr<FJsonObject> Root;IMAcousticBakeAssetPrivate::ReadMetadata(MetadataJson,Root,Failure);}
    return Failure.IsEmpty();
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
    const FString NewDigest = Digest(InMetadata,InScene, InProbes);
    Modify();
    WorldPackage = InWorld;
    SceneFingerprint = InFingerprint;
    MetadataJson = InMetadata;
    SceneData = MoveTemp(InScene);
    ProbeData = MoveTemp(InProbes);
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
    const auto& Origin=Root->GetArrayField(TEXT("bounds_origin_cm"));
    OriginCm=FVector(Origin[0]->AsNumber(),Origin[1]->AsNumber(),Origin[2]->AsNumber());
    for(const auto& Value:Root->GetArrayField(TEXT("probes_m")))
    {const auto& P=Value->AsArray();Out.Add(FVector4(P[0]->AsNumber(),P[1]->AsNumber(),P[2]->AsNumber(),P[3]->AsNumber()));}
    return true;
}
