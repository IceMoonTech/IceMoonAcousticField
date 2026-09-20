#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"

namespace
{
constexpr const TCHAR* IMAuditionRebakeMap = TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");

FString IMAuditionRebakeGeometryIdentity(UWorld* World)
{
    TArray<FString> Items;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        TArray<UStaticMeshComponent*> Components;
        It->GetComponents(Components);
        for (auto* M : Components)
        {
            Items.Add(M->GetPathName() + TEXT("|") + GetPathNameSafe(M->GetStaticMesh()) + TEXT("|") + M->GetComponentTransform().ToString());
        }
    }
    Items.Sort();
    FSHA1 Hash;
    for (const FString& Item : Items)
    {
        FTCHARToUTF8 Bytes(*Item);
        Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    }
    Hash.Final();
    uint8 Digest[20];
    Hash.GetHash(Digest);
    return BytesToHex(Digest, 20);
}

class IM_AuditionRebakeCommand final : public IAutomationLatentCommand
{
public:
    explicit IM_AuditionRebakeCommand(FAutomationTestBase* InTest)
        : Test(InTest), Started(FPlatformTime::Seconds()) {}
    bool Update() override
    {
        if (FPlatformTime::Seconds() - Started > 900.0)
        {
            return Finish(false, TEXT("Audition rebake timed out; previous asset preserved."));
        }
        if (Stage == 0)
        {
            if (!GEditor || GEditor->PlayWorld)
            {
                return Finish(false, TEXT("AuditionRebake requires an idle owned Editor; PIE must be stopped by root first."));
            }
            if (!FEditorFileUtils::LoadMap(IMAuditionRebakeMap, false, true))
            {
                return Finish(false, TEXT("Existing audition map missing; refusing to manufacture a replacement."));
            }
            UWorld* World = GEditor->GetEditorWorldContext().World();
            if (!World)
            {
                return Finish(false, TEXT("No editor world after loading the audition map."));
            }
            for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
            {
                if (Volume.IsValid())
                {
                    return Finish(false, TEXT("Ambiguous multiple acoustic bake volumes on the audition map."));
                }
                Volume = *It;
            }
            if (!Volume.IsValid())
            {
                return Finish(false, TEXT("Audition map has no AIMAcousticBakeVolume."));
            }
            if (!Volume->BakedField)
            {
                return Finish(false, TEXT("Audition bake volume has no bound bake asset; refusing unbound recovery."));
            }
            OldBakePath = GetPathNameSafe(Volume->BakedField.Get());
            OldPackage = Volume->BakedField->GetOutermost()->GetName();
            OldFingerprint = Volume->BakedField->SceneFingerprint;
            OldAssetFile = FPackageName::LongPackageNameToFilename(OldPackage, FPackageName::GetAssetPackageExtension());
            GeometryBefore = IMAuditionRebakeGeometryIdentity(World);
            Volume->GenerateProbes();
            if (Volume->GeneratedProbes <= 0)
            {
                return Finish(false, FString::Printf(TEXT("Audition probe generation failed: %s"), *Volume->Status));
            }
            TrianglesBefore = Volume->ExportedTriangles;
            ProbesBefore = Volume->GeneratedProbes;
            Previous = Volume->BakedField.Get();
            Volume->Bake();
            if (!Volume->Status.StartsWith(TEXT("Baking.")))
            {
                return Finish(false, FString::Printf(TEXT("Audition bake did not start: %s"), *Volume->Status));
            }
            Stage = 1;
            return false;
        }
        if (!Volume.IsValid())
        {
            return Finish(false, TEXT("Bake volume destroyed during its async bake."));
        }
        if (!Volume->BakedField || Volume->BakedField == Previous.Get())
        {
            if (Volume->Status.Contains(TEXT("failed"), ESearchCase::IgnoreCase) || Volume->Status.Contains(TEXT("discarded"), ESearchCase::IgnoreCase))
            {
                return Finish(false, FString::Printf(TEXT("Audition bake failed: %s"), *Volume->Status));
            }
            return false;
        }
        const FString NewPath = GetPathNameSafe(Volume->BakedField.Get());
        const FString NewPackage = Volume->BakedField->GetOutermost()->GetName();
        if (NewPath == OldBakePath)
        {
            return Finish(false, TEXT("Bake did not produce a new asset; refusing to claim recovery."));
        }
        if (!IFileManager::Get().FileExists(*OldAssetFile))
        {
            return Finish(false, TEXT("Previous bake asset file missing after bake; recovery is not reversible."));
        }
        if (Volume->BakedField->WorldPackage != IMAuditionRebakeMap)
        {
            return Finish(false, TEXT("New bake belongs to a different map; refusing binding."));
        }
        FString Error;
        if (!Volume->ValidateCurrentBake(Error))
        {
            return Finish(false, FString::Printf(TEXT("Rebaked asset failed current-scene validation: %s"), *Error));
        }
        TArray<FVector4> Preview;
        FVector Origin = FVector::ZeroVector;
        if (!Volume->BakedField->GetProbePreview(IMAuditionRebakeMap, Volume->BakedField->SceneFingerprint, Preview, Origin, Error) || Preview.Num() == 0)
        {
            return Finish(false, FString::Printf(TEXT("Rebaked probe preview unavailable: %s"), *Error));
        }
        UWorld* World = Volume->GetWorld();
        if (!World || IMAuditionRebakeGeometryIdentity(World) != GeometryBefore)
        {
            return Finish(false, TEXT("Bake changed audition mesh geometry; refusing map save."));
        }
        const FString MapFile = FPackageName::LongPackageNameToFilename(IMAuditionRebakeMap, FPackageName::GetMapPackageExtension());
        if (!FEditorFileUtils::SaveLevel(World->PersistentLevel, MapFile))
        {
            return Finish(false, TEXT("Cannot save audition map with the new bake binding; new asset retained, map unchanged on disk."));
        }
        Evidence = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/AuditionRebake"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
        IFileManager::Get().MakeDirectory(*Evidence, true);
        FString Json;
        auto Writer = TJsonWriterFactory<>::Create(&Json);
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("scope"), TEXT("audition fixture rebake only; no threshold or product-semantic change"));
        Writer->WriteValue(TEXT("map"), IMAuditionRebakeMap);
        Writer->WriteValue(TEXT("old_bake_path"), OldBakePath);
        Writer->WriteValue(TEXT("old_bake_package"), OldPackage);
        Writer->WriteValue(TEXT("old_scene_fingerprint"), OldFingerprint);
        Writer->WriteValue(TEXT("new_bake_path"), NewPath);
        Writer->WriteValue(TEXT("new_bake_package"), NewPackage);
        Writer->WriteValue(TEXT("new_scene_fingerprint"), Volume->BakedField->SceneFingerprint);
        Writer->WriteValue(TEXT("exported_triangles"), TrianglesBefore);
        Writer->WriteValue(TEXT("generated_probes"), ProbesBefore);
        Writer->WriteValue(TEXT("preview_probes"), Preview.Num());
        Writer->WriteValue(TEXT("geometry_sha1"), GeometryBefore);
        Writer->WriteValue(TEXT("map_file"), MapFile);
        Writer->WriteObjectEnd();
        Writer->Close();
        FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Evidence, TEXT("rebake-result.json")));
        return Finish(true, FString::Printf(TEXT("Audition rebaked: %s -> %s, probes=%d, geometry unchanged."), *OldBakePath, *NewPath, Preview.Num()));
    }

private:
    bool Finish(bool bSuccess, const FString& Message)
    {
        if (!bSuccess)
        {
            Test->AddError(Message);
        }
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticAuditionRebake %s evidence=%s"), *Message, *Evidence);
        UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticAuditionRebake %s"), bSuccess ? TEXT("PASS") : TEXT("FAIL"));
        UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), bSuccess ? TEXT("PASS") : TEXT("FAIL"));
        return true;
    }
    FAutomationTestBase* Test;
    double Started;
    int32 Stage = 0;
    int32 TrianglesBefore = 0;
    int32 ProbesBefore = 0;
    FString OldBakePath;
    FString OldPackage;
    FString OldFingerprint;
    FString OldAssetFile;
    FString GeometryBefore;
    FString Evidence;
    TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
    TWeakObjectPtr<UIMAcousticBakeAsset> Previous;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticAuditionRebakeTest, "IceMoon.AcousticField.W3.AuditionRebake", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticAuditionRebakeTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(IM_AuditionRebakeCommand(this));
    return true;
}
#endif
