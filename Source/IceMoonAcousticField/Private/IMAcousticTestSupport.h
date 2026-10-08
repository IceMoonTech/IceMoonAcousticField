#pragma once

#include "CoreMinimal.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticSpatialization.h"
#include "Components/AudioComponent.h"
#include "AudioDevice.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"

namespace IMAcousticTestSupport
{
inline constexpr const TCHAR* GraphSourceAssetPath =
	TEXT("/IceMoonAcousticField/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay");

inline TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> FindBridge(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	FAudioDevice* Device = World->GetAudioDeviceRaw();
	if (!Device)
	{
		return nullptr;
	}
	if (const FIMAcousticMetaSoundContextPtr Context = IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID))
	{
		return Context->Device;
	}
	return IMAcousticSpatialization::FindAcousticDevice(Device);
}

inline bool ConfigureGraphSource(UAudioComponent* Audio, FString& Failure)
{
	if (!Audio)
	{
		Failure = TEXT("Graph source test audio component is null.");
		return false;
	}
	USoundBase* Sound = LoadObject<USoundBase>(nullptr, GraphSourceAssetPath);
	if (!Sound || !IMAcousticMetaSound::IsAcousticMetaSound(Sound))
	{
		Failure = FString::Printf(TEXT("Missing or invalid graph source asset: %s"), GraphSourceAssetPath);
		return false;
	}
	// The graph owns stereo spatialization, distance, occlusion, path and wet
	// processing. Leaving native spatialization or attenuation enabled would
	// make this fixture exercise a second consumer instead of the new chain.
	Audio->bAutoActivate = false;
	Audio->bAllowSpatialization = false;
	Audio->SetOverrideAttenuation(false);
	Audio->SetSound(Sound);
	return true;
}
}
