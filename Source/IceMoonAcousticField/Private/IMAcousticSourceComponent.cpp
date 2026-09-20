#include "IMAcousticSourceComponent.h"
#include "IMAcousticMetaSound.h"
#include "Components/AudioComponent.h"
#if WITH_EDITORONLY_DATA
#include "Components/BillboardComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/SoftObjectPath.h"
#endif
#include "MetasoundSource.h"
#include "Sound/SoundWave.h"

#if WITH_EDITORONLY_DATA
void UIMAcousticSourceComponent::OnRegister()
{
    Super::OnRegister();
    if (!GIsEditor || !GetOwner()) return;
    const UWorld* World = GetWorld();
    if (!World || World->IsGameWorld()) return;
    CreateEditorBillboard();
}

void UIMAcousticSourceComponent::OnUnregister()
{
    if (EditorBillboard)
    {
        if (AActor* Owner = GetOwner())
        {
            // The marker is an editor-only instance component. Remove it from
            // the actor list before destroying it so the Components panel and
            // subsequent re-registers cannot retain a stale entry.
            Owner->RemoveInstanceComponent(EditorBillboard);
        }
        EditorBillboard->DestroyComponent();
        EditorBillboard = nullptr;
    }
    Super::OnUnregister();
}

void UIMAcousticSourceComponent::CreateEditorBillboard()
{
    if (EditorBillboard || !GetOwner()) return;

    USceneComponent* Parent = AudioComponent.Get();
    if (!Parent) Parent = GetOwner()->GetRootComponent();
    if (!Parent) return;

    const EObjectFlags TransactionalFlag = GetFlags() & RF_Transactional;
    const FName BillboardName = MakeUniqueObjectName(
        GetOwner(), UBillboardComponent::StaticClass(), TEXT("IM_AcousticSourceBillboard"));
    EditorBillboard = NewObject<UBillboardComponent>(
        GetOwner(), BillboardName, TransactionalFlag | RF_Transient | RF_TextExportTransient);
    EditorBillboard->SetSprite(LoadObject<UTexture2D>(
        nullptr, TEXT("/Engine/EditorResources/AudioIcons/S_AudioComponent.S_AudioComponent")));
    // Keep the marker visibly separate from UAudioComponent's native sprite.
    // The native sprite sits at the source origin; this marker floats above it
    // so the author can find the acoustic source even in a dense whitebox.
    EditorBillboard->SetRelativeLocation(FVector(0.0f, 0.0f, 100.0f));
    EditorBillboard->SetRelativeScale3D_Direct(FVector(1.0f));
    EditorBillboard->Mobility = EComponentMobility::Movable;
    EditorBillboard->AlwaysLoadOnClient = false;
    // This is intentionally a regular instance component, not a UE
    // Visualization Component. The editor's Components tree filters
    // IsVisualizationComponent() entries, which made the marker exist at
    // runtime but disappear from the author's Details/Components view.
    EditorBillboard->SetIsVisualizationComponent(false);
    EditorBillboard->bIsEditorOnly = false;
    // Keep the source marker readable when the editor camera is in a wall seam
    // or the source is behind whitebox geometry; this is editor compositing only.
    EditorBillboard->bUseEditorCompositing = true;
    EditorBillboard->SetHiddenInGame(true);
    EditorBillboard->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));
    EditorBillboard->CreationMethod = EComponentCreationMethod::Instance;
    EditorBillboard->bIsScreenSizeScaled = true;
    EditorBillboard->ScreenSize = 0.01f;
    EditorBillboard->bUseInEditorScaling = true;
    EditorBillboard->OpacityMaskRefVal = 1.0f;
    // Reuse the engine's always-visible Sounds category; a transient custom
    // category is not guaranteed to be present in an already-open viewport's
    // sprite visibility array.
    EditorBillboard->SpriteInfo.Category = TEXT("Sounds");
    EditorBillboard->SpriteInfo.DisplayName = NSLOCTEXT(
        "SpriteCategory", "Sounds", "Sounds");
    // Register the transient marker as an actor instance component as well as
    // a scene component. Without this, runtime enumeration can find it but the
    // editor Components tree/Details panel cannot expose it to the author.
    GetOwner()->AddInstanceComponent(EditorBillboard);
    EditorBillboard->SetupAttachment(Parent);
    EditorBillboard->RegisterComponent();
}
#endif

bool UIMAcousticSourceComponent::ValidateSource(FString& Failure) const
{
    check(IsInGameThread());
    // BakeVolume shares Failure across sources. A successful validation must
    // preserve an earlier source rejection so the final status reports it.
    const UAudioComponent* Audio = AudioComponent.Get();
    if (!IsValid(Audio) || Audio->GetWorld() != GetWorld())
    {
        Failure = TEXT("AudioComponent is missing or belongs to another world.");
        return false;
    }
    const USoundWave* Wave = Cast<USoundWave>(Audio->Sound);
    if (IM_IsAcousticMetaSound(Audio->Sound))
    {
        const FSoundAttenuationSettings* Settings = Audio->GetAttenuationSettingsToApply();
        if (Audio->bAllowSpatialization || (Settings && (Settings->bSpatialize || Settings->bAttenuate
            || Settings->bAttenuateWithLPF || Settings->bEnableOcclusion || Settings->bEnableListenerFocus
            || Settings->bEnableReverbSend)))
        {
            Failure = TEXT("Acoustic MetaSound owns stereo spatialization and attenuation; disable native processing.");
            return false;
        }
        return true;
    }
    const UMetaSoundSource* MetaSound = Cast<UMetaSoundSource>(Audio->Sound);
    const bool bMonoWave = Wave && Wave->NumChannels == 1;
    const bool bMonoMetaSound = MetaSound && MetaSound->OutputFormat == EMetaSoundOutputAudioFormat::Mono
        && MetaSound->NumChannels == 1;
    if (!bMonoWave && !bMonoMetaSound)
    {
        // Both accepted source types expose a fixed, verifiable mono voice to the
        // spatialization plugin. Other graph types may change channel layout.
        Failure = TEXT("The V2 delivery requires a mono SoundWave or mono MetaSoundSource.");
        return false;
    }
    const FSoundAttenuationSettings* Settings = Audio->GetAttenuationSettingsToApply();
    if (!Settings || !Audio->bAllowSpatialization || !Settings->bSpatialize
        || Settings->SpatializationAlgorithm != SPATIALIZATION_HRTF)
    {
        Failure = TEXT("Enable plugin spatialization on the AudioComponent attenuation settings.");
        return false;
    }
    if (Settings->bAttenuate || Settings->bAttenuateWithLPF || Settings->bEnableOcclusion
        || Settings->bEnableListenerFocus || Settings->bEnableReverbSend)
    {
        Failure = TEXT("Disable native distance, air absorption, occlusion, listener focus and reverb sends; V2 owns these paths.");
        return false;
    }
    const auto& Plugins = Settings->PluginSettings;
    if (Plugins.SpatializationPluginSettingsArray.Num() != 1
        || !IsValid(Plugins.SpatializationPluginSettingsArray[0])
        || !Plugins.SpatializationPluginSettingsArray[0]->IsA<UIMAcousticSpatializationSettings>()
        || !Plugins.OcclusionPluginSettingsArray.IsEmpty()
        || !Plugins.ReverbPluginSettingsArray.IsEmpty()
        || !Plugins.SourceDataOverridePluginSettingsArray.IsEmpty())
    {
        Failure = TEXT("Select only IceMoon spatialization settings and remove conflicting audio plugin settings.");
        return false;
    }
    return true;
}
