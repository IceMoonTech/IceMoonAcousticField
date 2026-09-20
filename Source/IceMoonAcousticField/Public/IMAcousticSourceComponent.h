#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "IAudioExtensionPlugin.h"
#include "IMAcousticSourceComponent.generated.h"

class UAudioComponent;
#if WITH_EDITORONLY_DATA
class UBillboardComponent;
#endif

// A positive routing marker. Selecting the device plugin alone must not silently
// enroll every sound in the world into baked acoustic processing.
UCLASS(EditInlineNew)
class ICEMOONACOUSTICFIELD_API UIMAcousticSpatializationSettings final : public USpatializationPluginSourceSettingsBase
{
    GENERATED_BODY()
};

UCLASS(ClassGroup=(Audio), meta=(BlueprintSpawnableComponent))
class ICEMOONACOUSTICFIELD_API UIMAcousticSourceComponent final : public UActorComponent
{
    GENERATED_BODY()
public:
    // Explicit reference avoids choosing an arbitrary component on multi-voice actors.
    UPROPERTY(EditInstanceOnly, Category="Acoustics")
    TObjectPtr<UAudioComponent> AudioComponent;

    // GT only. Validation never changes playback or the user's attenuation asset.
    bool ValidateSource(FString& Failure) const;

protected:
#if WITH_EDITORONLY_DATA
    virtual void OnRegister() override;
    virtual void OnUnregister() override;

    void CreateEditorBillboard();

    // Transient visualization only. The source marker remains the sole runtime owner.
    UPROPERTY(Transient)
    TObjectPtr<UBillboardComponent> EditorBillboard;
#endif
};
