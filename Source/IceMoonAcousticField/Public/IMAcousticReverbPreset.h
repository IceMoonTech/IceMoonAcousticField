#pragma once
#include "CoreMinimal.h"
#include "Sound/SoundEffectSubmix.h"
#include "IMAcousticReverbPreset.generated.h"

struct FIMAcousticDeviceBridge;
struct FIMAcousticReverbPool;
struct FIMAcousticMetaSoundContext;
struct FIMAcousticEnvironmentHost;
class UMetaSoundSource;
class UAudioBus;
class FAudioDeviceHandle;
class FJsonObject;

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
TSharedPtr<FJsonObject> IM_ExportAcousticEnvironmentHostForTest(
	const TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe>& Host, const FString& Directory);
#endif

// The bake-volume GT owner binds one device/world before adding this effect to
// its dedicated submix. Unbound presets output no wet audio.
UCLASS(BlueprintType,EditInlineNew)
class ICEMOONACOUSTICFIELD_API UIMAcousticReverbPreset : public USoundEffectSubmixPreset
{
	GENERATED_BODY()
public:
	void Bind(TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Device,
		TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> Pool,float WetGain);
	bool BindMetaSound(UMetaSoundSource* Source, UAudioBus* Bus,
		const TSharedPtr<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>& Context,
		const FAudioDeviceHandle& Device, FString& OutError);
	void StopMetaSoundHost();
	void ReleaseMetaSoundHost();
	void BeginDestroy() override;
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe> GetEnvironmentHostForTest() const { return BoundHost; }
#endif
	FText GetAssetActionName() const override{return FText::FromString(TEXT("IceMoon Baked Reverb"));}
	UClass* GetSupportedClass() const override{return StaticClass();}
	USoundEffectPreset* CreateNewPreset(UObject* Parent,FName Name,EObjectFlags Flags) const override;
	FSoundEffectBase* CreateNewEffect() const override;
	void Init() override{}
private:
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> BoundDevice;
	TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> BoundPool;
	float BoundWetGain=.25f;
	TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe> BoundHost;
};
