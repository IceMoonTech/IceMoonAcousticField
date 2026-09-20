#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticW2Inventory,"IceMoon.AcousticField.W2.Inventory",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticW2Inventory::RunTest(const FString&)
{
    const bool Loaded=FEditorFileUtils::LoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,true);
    if(!Loaded){AddError(TEXT("Existing whitebox map missing."));return false;}
    UWorld* World=GEditor->GetEditorWorldContext().World();
    FString Json;auto W=TJsonWriterFactory<>::Create(&Json);W->WriteArrayStart();int32 MeshCount=0;
    for(TActorIterator<AActor> It(World);It;++It)
    {
        TArray<UPrimitiveComponent*> Components;It->GetComponents(Components);
        for(auto* Component:Components)
        {
            W->WriteObjectStart();W->WriteValue(TEXT("actor"),It->GetActorLabel());W->WriteValue(TEXT("component"),Component->GetPathName());
            W->WriteValue(TEXT("class"),Component->GetClass()->GetName());W->WriteValue(TEXT("mobility"),int32(Component->Mobility));
            W->WriteValue(TEXT("transform"),Component->GetComponentTransform().ToString());
            W->WriteValue(TEXT("bounds"),Component->Bounds.GetBox().ToString());
            if(auto* Mesh=Cast<UStaticMeshComponent>(Component))
            {
                ++MeshCount;W->WriteValue(TEXT("mesh"),GetPathNameSafe(Mesh->GetStaticMesh()));
                W->WriteArrayStart(TEXT("materials"));for(int32 I=0;I<Mesh->GetNumMaterials();++I)W->WriteValue(GetPathNameSafe(Mesh->GetMaterial(I)));W->WriteArrayEnd();
                W->WriteValue(TEXT("physical_material"),GetPathNameSafe(Mesh->BodyInstance.GetSimplePhysicalMaterial()));
                if(auto* Instances=Cast<UInstancedStaticMeshComponent>(Mesh))W->WriteValue(TEXT("instances"),Instances->GetInstanceCount());
            }
            W->WriteObjectEnd();
        }
    }
    W->WriteArrayEnd();W->Close();
    const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W2-UE"));IFileManager::Get().MakeDirectory(*Directory,true);
    const bool Success=MeshCount>0&&FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Directory,TEXT("existing-whitebox-inventory.json")));
    UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW2Inventory meshes=%d"),MeshCount);
    UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW2Inventory %s"),Success?TEXT("PASS"):TEXT("FAIL"));
    UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));
    return Success;
}
namespace
{
constexpr const TCHAR* IMW2Map=TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
FString IMW2GeometryIdentity(UWorld* World)
{
    TArray<FString> Items;
    for(TActorIterator<AActor> It(World);It;++It)
    {
        TArray<UStaticMeshComponent*> Components;It->GetComponents(Components);
        for(auto* M:Components)Items.Add(M->GetPathName()+TEXT("|")+GetPathNameSafe(M->GetStaticMesh())+TEXT("|")+M->GetComponentTransform().ToString());
    }
    Items.Sort();FSHA1 Hash;for(const FString& Item:Items){FTCHARToUTF8 Bytes(*Item);Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());}
    Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);return BytesToHex(Digest,20);
}
class IM_AcousticW2BakeCommand final:public IAutomationLatentCommand
{
public:
    explicit IM_AcousticW2BakeCommand(FAutomationTestBase* InTest):Test(InTest),Started(FPlatformTime::Seconds()){}
    bool Update() override
    {
        if(FPlatformTime::Seconds()-Started>900)return Finish(false,TEXT("Actual whitebox bake timed out."));
        if(Stage==0)
        {
            if(!FEditorFileUtils::LoadMap(IMW2Map,false,true))return Finish(false,TEXT("Existing whitebox map missing."));
            UWorld* World=GEditor->GetEditorWorldContext().World();GeometryBefore=IMW2GeometryIdentity(World);
            FBox Bounds(ForceInit);int32 WhiteboxCount=0;UMaterialInterface* Material=nullptr;
            for(TActorIterator<AActor> It(World);It;++It)
            {
                if(auto* Existing=Cast<AIMAcousticBakeVolume>(*It))
                {if(Volume.IsValid())return Finish(false,TEXT("Ambiguous multiple acoustic bake volumes."));Volume=Existing;}
                TArray<UPrimitiveComponent*> Components;It->GetComponents(Components);
                for(auto* C:Components)
                {
                    // This fixture's visual sky and spawn marker are not surfaces.
                    // Persist explicit exclusions instead of weakening the exporter.
                    if(It->GetActorLabel()==TEXT("SM_SkySphere")||It->IsA<APlayerStart>())
                    {C->Modify();C->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));}
                    if(!It->ActorHasTag(TEXT("IMAcousticWhitebox")))continue;
                    if(auto* M=Cast<UStaticMeshComponent>(C))
                    {
                        ++WhiteboxCount;Bounds+=M->Bounds.GetBox();
                        if(Material&&Material!=M->GetMaterial(0))return Finish(false,TEXT("Whitebox material inventory changed; explicit mapping needs review."));
                        Material=M->GetMaterial(0);
                    }
                }
            }
            if(WhiteboxCount!=60||!Bounds.IsValid||!Material)return Finish(false,TEXT("Existing whitebox geometry inventory differs from 60 verified meshes."));
            if(!Volume.IsValid())
            {
                Volume=World->SpawnActor<AIMAcousticBakeVolume>();Volume->SetActorLabel(TEXT("IM_V2Bake"));
                Volume->SetActorLocation(Bounds.GetCenter());Volume->BakeBounds->SetBoxExtent(Bounds.GetExtent()+FVector(25));
                FIMAcousticMaterialMapping Mapping;Mapping.Material=Material;Mapping.Absorption=FVector(.25);Mapping.Scattering=.5f;
                Volume->Materials.Add(Mapping);
            }
            Volume->GenerateProbes();
            if(Volume->GeneratedProbes<=0)
            {for(const auto& Issue:Volume->SceneIssues)UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticW2Scene %s"),*Issue);return Finish(false,Volume->Status);}
            Previous=Volume->BakedField;Volume->Bake();Stage=1;return false;
        }
        if(!Volume.IsValid())return Finish(false,TEXT("Bake owner destroyed."));
        if(Stage==1)
        {
            if(Volume->Status.Contains(TEXT("failed"),ESearchCase::IgnoreCase)||Volume->Status.Contains(TEXT("discarded")))return Finish(false,Volume->Status);
            if(!Volume->BakedField||Volume->BakedField==Previous.Get())return false;
            FString Error;if(!Volume->ValidateCurrentBake(Error))return Finish(false,Error);
            if(IMW2GeometryIdentity(Volume->GetWorld())!=GeometryBefore)return Finish(false,TEXT("Test changed existing scene geometry."));
            auto* Asset=Volume->BakedField.Get();
            auto* Corrupt=DuplicateObject<UIMAcousticBakeAsset>(Asset,GetTransientPackage());
            Corrupt->ProbeData[0]^=1;
            if(Corrupt->Validate(Asset->WorldPackage,Asset->SceneFingerprint,Error))return Finish(false,TEXT("Corrupt payload accepted."));
            if(Asset->Validate(TEXT("/IceMoonAcousticField/Tests/IM_W1Door"),Asset->SceneFingerprint,Error))return Finish(false,TEXT("Wrong map accepted."));
            const FString OldDigest=Corrupt->PayloadDigest,OldMetadata=Corrupt->MetadataJson;
            TArray<uint8> Scene=Asset->SceneData,Probes=Asset->ProbeData;
            if(Corrupt->CommitCompleteBake(Asset->WorldPackage,Asset->SceneFingerprint,TEXT("{}"),MoveTemp(Scene),MoveTemp(Probes),Error)
                ||Corrupt->PayloadDigest!=OldDigest||Corrupt->MetadataJson!=OldMetadata)return Finish(false,TEXT("Invalid commit mutated previous asset."));
            const FVector Absorption=Volume->Materials[0].Absorption;Volume->Materials[0].Absorption=FVector(.8);
            const bool StaleAccepted=Volume->ValidateCurrentBake(Error);Volume->Materials[0].Absorption=Absorption;
            if(StaleAccepted||!Volume->ValidateCurrentBake(Error))return Finish(false,TEXT("Material stale/restore validation failed."));
            const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W2-UE"));
            FFileHelper::SaveStringToFile(Asset->MetadataJson,*FPaths::Combine(Directory,TEXT("bake-metadata.json")));
            FFileHelper::SaveArrayToFile(Asset->SceneData,*FPaths::Combine(Directory,TEXT("scene.bin")));
            FFileHelper::SaveArrayToFile(Asset->ProbeData,*FPaths::Combine(Directory,TEXT("probes.bin")));
            FFileHelper::SaveStringToFile(GeometryBefore,*FPaths::Combine(Directory,TEXT("geometry-identity.txt")));
            const FString File=FPackageName::LongPackageNameToFilename(IMW2Map,FPackageName::GetMapPackageExtension());
            if(!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel,File))return Finish(false,TEXT("Cannot save whitebox bake binding."));
            Previous=Asset;Volume->GenerateProbes();Volume->Bake();Volume->CancelBake();Stage=2;return false;
        }
        if(!Volume->Status.StartsWith(TEXT("Bake cancelled")))return false;
        FString Error;
        if(Volume->BakedField!=Previous.Get()||!Volume->ValidateCurrentBake(Error))return Finish(false,TEXT("Cancelled bake replaced valid asset."));
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW2Bake triangles=%d probes=%d elapsed_s=%g geometry_preserved=1 negatives=4"),Volume->ExportedTriangles,Volume->GeneratedProbes,FPlatformTime::Seconds()-Started);
        return Finish(true,TEXT("Actual whitebox bake, metadata, stale/corrupt/wrong-map/cancel gates passed."));
    }
private:
    bool Finish(bool Success,const FString& Message)
    {
        if(!Success)Test->AddError(Message);
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW2 %s"),*Message);
        UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW2 %s"),Success?TEXT("PASS"):TEXT("FAIL"));
        UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));return true;
    }
    FAutomationTestBase* Test;double Started;int32 Stage=0;FString GeometryBefore;
    TWeakObjectPtr<AIMAcousticBakeVolume> Volume;TWeakObjectPtr<UIMAcousticBakeAsset> Previous;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticW2Bake,"IceMoon.AcousticField.W2.BakeExisting",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticW2Bake::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(IM_AcousticW2BakeCommand(this));return true;}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticW2Cold,"IceMoon.AcousticField.W2.ColdLoad",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticW2Cold::RunTest(const FString&)
{
    bool Success=FEditorFileUtils::LoadMap(IMW2Map,false,true);FString Error;int32 Found=0;
    if(Success)for(TActorIterator<AIMAcousticBakeVolume> It(GEditor->GetEditorWorldContext().World());It;++It)
    {
        ++Found;Success=It->ValidateCurrentBake(Error)&&Success;
        if(Success){TArray<FVector4> Probes;FVector Origin;Success=It->BakedField->GetProbePreview(IMW2Map,It->BakedField->SceneFingerprint,Probes,Origin,Error)&&!Probes.IsEmpty();}
    }
    Success=Success&&Found==1;if(!Success)AddError(TEXT("Cold-loaded bake invalid: ")+Error);
    UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW2ColdLoad %s"),Success?TEXT("PASS"):TEXT("FAIL"));
    UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));return Success;
}
#endif
