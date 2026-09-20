#include "IMAcousticMetaSound.h"
#include "MetasoundSource.h"
#include "MetasoundFrontendDocument.h"
#include "Misc/ScopeLock.h"

namespace
{
FCriticalSection IMMetaSoundRegistryMutex;
TMap<uint32, TWeakPtr<IM_AcousticMetaSoundContext, ESPMode::ThreadSafe>> IMMetaSoundRegistry;
bool IMCaptureNextContext = false; // GT only, test sets before ordinary PIE startup.
}

void IM_EnableAcousticMetaSoundCaptureForTest(bool Enabled) { check(IsInGameThread()); IMCaptureNextContext = Enabled; }

IM_AcousticMetaSoundContextPtr IM_CreateAcousticMetaSoundContext(uint32 DeviceId, uint32 SampleRate, uint64 Epoch)
{
    check(IsInGameThread());
    FScopeLock Lock(&IMMetaSoundRegistryMutex);
    if (auto* Existing = IMMetaSoundRegistry.Find(DeviceId); Existing && Existing->IsValid()) return nullptr;
    auto Context = MakeShared<IM_AcousticMetaSoundContext, ESPMode::ThreadSafe>();
    Context->DeviceId = DeviceId;
    Context->Device = MakeShared<IM_AcousticDeviceBridge, ESPMode::ThreadSafe>();
    Context->Device->SampleRate = SampleRate;
    Context->Device->BlockFrames = IM_AcousticMetaSoundContext::Frames;
    Context->Device->InitProbes();
    for (uint32 I = 0; I < IM_AcousticMetaSoundContext::MaxVoices; ++I)
    {
        Context->Device->Voices.Add(MakeUnique<IM_AcousticVoiceBridge>());
    }
    Context->Device->Alive.store(true, std::memory_order_release);
    Context->Pool = MakeShared<IM_AcousticReverbPool, ESPMode::ThreadSafe>(Epoch);
    if (IMCaptureNextContext)
    {
        const int32 Capacity = SampleRate * 200;
        Context->CapturedSource.SetNumZeroed(Capacity);
        Context->CapturedDry.SetNumZeroed(Capacity * 2);
        Context->CapturedWet.SetNumZeroed(Capacity * 2);
        Context->CapturedBus.SetNumZeroed(Capacity);
        Context->CapturedSourceBlocks.SetNumZeroed(Capacity / IM_AcousticMetaSoundContext::Frames);
        Context->CapturedBlocks.SetNumZeroed(Capacity / IM_AcousticMetaSoundContext::Frames);
    }
    IMMetaSoundRegistry.Add(DeviceId, Context);
    return Context;
}

IM_AcousticMetaSoundContextPtr IM_FindAcousticMetaSoundContext(uint32 DeviceId)
{
    FScopeLock Lock(&IMMetaSoundRegistryMutex);
    const auto* Found = IMMetaSoundRegistry.Find(DeviceId);
    return Found ? Found->Pin() : nullptr;
}

void IM_StopAcousticMetaSoundContext(const IM_AcousticMetaSoundContextPtr& Context)
{
    check(IsInGameThread());
    if (!Context) return;
    Context->Stopped.store(true, std::memory_order_release);
    Context->Device->Alive.store(false, std::memory_order_release);
    Context->Pool->Stopped.store(true, std::memory_order_release);
    FScopeLock Lock(&IMMetaSoundRegistryMutex);
    auto* Found = IMMetaSoundRegistry.Find(Context->DeviceId);
    if (Found && Found->Pin() == Context) IMMetaSoundRegistry.Remove(Context->DeviceId);
}

int32 IM_RegisterAcousticMetaSoundSource(const IM_AcousticMetaSoundContextPtr& Context, uint64 AudioId)
{
    check(IsInGameThread());
    if (!Context || !AudioId || Context->Stopped.load(std::memory_order_acquire)) return INDEX_NONE;
    for (uint32 I = 0; I < IM_AcousticMetaSoundContext::MaxVoices; ++I)
    {
        if (Context->AudioIds[I].load(std::memory_order_acquire) == AudioId) return I;
    }
    // Slots are not recycled within a world. Old generators can therefore never
    // consume a new component's result. World shutdown releases the bounded set.
    for (uint32 I = 0; I < IM_AcousticMetaSoundContext::MaxVoices; ++I)
    {
        if (Context->AudioIds[I].load(std::memory_order_acquire) == 0)
        {
            Context->AudioIds[I].store(AudioId, std::memory_order_release);
            return I;
        }
    }
    return INDEX_NONE;
}

bool IM_IsAcousticMetaSound(const USoundBase* Sound)
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
