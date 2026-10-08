#include "IMAcousticMetaSound.h"
#include "MetasoundSource.h"
#include "MetasoundFrontendDocument.h"
#include "Misc/ScopeLock.h"

namespace IMAcousticMetaSoundPrivate
{
FCriticalSection MetaSoundRegistryMutex;
TMap<uint32, TWeakPtr<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>> MetaSoundRegistry;
bool CaptureNextContext = false; // GT only, test sets before ordinary PIE startup.
}

void IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(bool Enabled) { check(IsInGameThread()); IMAcousticMetaSoundPrivate::CaptureNextContext = Enabled; }

FIMAcousticMetaSoundContextPtr IMAcousticMetaSound::CreateAcousticMetaSoundContext(uint32 DeviceId, uint32 SampleRate, uint64 Epoch)
{
	check(IsInGameThread());
	FScopeLock Lock(&IMAcousticMetaSoundPrivate::MetaSoundRegistryMutex);
	if (auto* Existing = IMAcousticMetaSoundPrivate::MetaSoundRegistry.Find(DeviceId); Existing && Existing->IsValid()) return nullptr;
	auto Context = MakeShared<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>();
	Context->DeviceId = DeviceId;
	Context->Device = MakeShared<FIMAcousticDeviceBridge, ESPMode::ThreadSafe>();
	Context->Device->SampleRate = SampleRate;
	Context->Device->BlockFrames = FIMAcousticMetaSoundContext::Frames;
	Context->Device->InitProbes();
	for (uint32 I = 0; I < FIMAcousticMetaSoundContext::MaxVoices; ++I)
	{
		Context->Device->Voices.Add(MakeUnique<FIMAcousticVoiceBridge>());
	}
	Context->Device->Alive.store(true, std::memory_order_release);
	Context->Pool = MakeShared<FIMAcousticReverbPool, ESPMode::ThreadSafe>(Epoch);
	if (IMAcousticMetaSoundPrivate::CaptureNextContext)
	{
		const int32 Capacity = SampleRate * 200;
		Context->CapturedSource.SetNumZeroed(Capacity);
		Context->CapturedDry.SetNumZeroed(Capacity * 2);
		Context->CapturedWet.SetNumZeroed(Capacity * 2);
		Context->CapturedBus.SetNumZeroed(Capacity);
		Context->CapturedSourceBlocks.SetNumZeroed(Capacity / FIMAcousticMetaSoundContext::Frames);
		Context->CapturedBlocks.SetNumZeroed(Capacity / FIMAcousticMetaSoundContext::Frames);
	}
	IMAcousticMetaSoundPrivate::MetaSoundRegistry.Add(DeviceId, Context);
	return Context;
}

FIMAcousticMetaSoundContextPtr IMAcousticMetaSound::FindAcousticMetaSoundContext(uint32 DeviceId)
{
	FScopeLock Lock(&IMAcousticMetaSoundPrivate::MetaSoundRegistryMutex);
	const auto* Found = IMAcousticMetaSoundPrivate::MetaSoundRegistry.Find(DeviceId);
	return Found ? Found->Pin() : nullptr;
}

void IMAcousticMetaSound::StopAcousticMetaSoundContext(const FIMAcousticMetaSoundContextPtr& Context)
{
	check(IsInGameThread());
	if (!Context) return;
	Context->Stopped.store(true, std::memory_order_release);
	Context->Device->Alive.store(false, std::memory_order_release);
	Context->Pool->Stopped.store(true, std::memory_order_release);
	FScopeLock Lock(&IMAcousticMetaSoundPrivate::MetaSoundRegistryMutex);
	auto* Found = IMAcousticMetaSoundPrivate::MetaSoundRegistry.Find(Context->DeviceId);
	if (Found && Found->Pin() == Context) IMAcousticMetaSoundPrivate::MetaSoundRegistry.Remove(Context->DeviceId);
}

int32 IMAcousticMetaSound::RegisterAcousticMetaSoundSource(const FIMAcousticMetaSoundContextPtr& Context, uint64 AudioId)
{
	check(IsInGameThread());
	if (!Context || !AudioId || Context->Stopped.load(std::memory_order_acquire)) return INDEX_NONE;
	for (uint32 I = 0; I < FIMAcousticMetaSoundContext::MaxVoices; ++I)
	{
		if (Context->AudioIds[I].load(std::memory_order_acquire) == AudioId) return I;
	}
	// Slots are not recycled within a world. Old generators can therefore never
	// consume a new component's result. World shutdown releases the bounded set.
	for (uint32 I = 0; I < FIMAcousticMetaSoundContext::MaxVoices; ++I)
	{
		if (Context->AudioIds[I].load(std::memory_order_acquire) == 0)
		{
			Context->AudioIds[I].store(AudioId, std::memory_order_release);
			return I;
		}
	}
	return INDEX_NONE;
}

bool IMAcousticMetaSound::IsAcousticMetaSound(const USoundBase* Sound)
{
	const auto* Source = Cast<UMetaSoundSource>(Sound);
	if (!Source || Source->OutputFormat != EMetaSoundOutputAudioFormat::Stereo || Source->NumChannels != 2) return false;
	for (const auto& Dependency : Source->GetConstDocument().Dependencies)
	{
		const auto& Name = Dependency.Metadata.GetClassName();
		if (Name.Namespace == TEXT("IM") && Name.Name == TEXT("Acoustic Source")) return true;
	}
	return false;
}
