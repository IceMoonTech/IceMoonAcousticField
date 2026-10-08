#pragma once
#include "CoreMinimal.h"
#include "Sound/SoundEffectSubmix.h"
#include "IMAcousticReverbPreset.generated.h"

struct FIMAcousticDeviceBridge;
struct FIMAcousticReverbPool;

// The bake-volume GT owner binds one device/world before adding this effect to
// its dedicated submix. Unbound presets output no wet audio.
UCLASS(BlueprintType,EditInlineNew)
class ICEMOONACOUSTICFIELD_API UIMAcousticReverbPreset : public USoundEffectSubmixPreset
{
    GENERATED_BODY()
public:
    void Bind(TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Device,
        TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> Pool,float WetGain);
    FText GetAssetActionName() const override{return FText::FromString(TEXT("IceMoon Baked Reverb"));}
    UClass* GetSupportedClass() const override{return StaticClass();}
    USoundEffectPreset* CreateNewPreset(UObject* Parent,FName Name,EObjectFlags Flags) const override;
    FSoundEffectBase* CreateNewEffect() const override;
    void Init() override{}
private:
    TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> BoundDevice;
    TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> BoundPool;
    float BoundWetGain=.25f;
};
