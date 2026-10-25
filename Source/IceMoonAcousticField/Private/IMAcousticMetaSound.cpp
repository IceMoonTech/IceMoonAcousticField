#include "IMAcousticMetaSound.h"
#include "MetasoundSource.h"
#include "MetasoundFrontendDocument.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include <utility>

namespace IMAcousticMetaSoundPrivate
{
FCriticalSection MetaSoundRegistryMutex;
TMap<uint32, TWeakPtr<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>> MetaSoundRegistry;
bool CaptureNextContext = false; // GT only, test sets before ordinary PIE startup.
uint32 IMCaptureNextSourceTraceCapacity = 0;
uint32 IMCaptureNextEnvironmentTraceCapacity = 0;
bool IMCaptureNextPCM = true;
bool IMCaptureNextBusReads = false;
bool IMCaptureNextTransportOrder = false;

// GT-only settings and storage. A prepared object owns no world, bridge, pool,
// voice or generator; registry publication still follows ordinary source input.
struct FIMAcousticCaptureSettings
{
	uint64 Revision = 0;
	uint32 SampleRate = 0;
	uint32 SourceOverride = 0, EnvironmentOverride = 0;
	uint32 SourceTraceCapacity = 0, EnvironmentTraceCapacity = 0;
	int32 PcmCapacity = 0;
	bool CapturePCM = false, CaptureBusReads = false, CaptureTransportOrder = false;

	bool Matches(const FIMAcousticCaptureSettings& Other) const
	{
		return Revision == Other.Revision && SampleRate == Other.SampleRate
			&& SourceOverride == Other.SourceOverride && EnvironmentOverride == Other.EnvironmentOverride
			&& SourceTraceCapacity == Other.SourceTraceCapacity && EnvironmentTraceCapacity == Other.EnvironmentTraceCapacity
			&& PcmCapacity == Other.PcmCapacity && CapturePCM == Other.CapturePCM
			&& CaptureBusReads == Other.CaptureBusReads && CaptureTransportOrder == Other.CaptureTransportOrder;
	}
};

struct FIMAcousticCaptureStorage
{
	TArray<float> Source, Dry, Wet, Bus;
	TArray<FIMAcousticBlockProbe> SourceBlocks;
	TArray<FIMAcousticMetaSoundBlock> EnvironmentBlocks;
	TArray<FIMAcousticBusReadBlock> BusReads;
	TArray<FIMAcousticTransportOrderEvent> TransportOrder;
	uint32 TransportReadyWordCount = 0;
	std::unique_ptr<std::atomic<uint64>[]> TransportReady;
	uint32 ReadyWordCount = 0;
	std::unique_ptr<std::atomic<uint64>[]> Ready;
};

struct FIMAcousticPreparedCapture
{
	uint64 Id = 0;
	uint32 DeviceId = 0;
	FIMAcousticCaptureSettings Settings;
	FIMAcousticCaptureStorage Storage;
};

uint64 IMCaptureSettingsRevision = 0;
uint64 IMNextCapturePreparationId = 0;
TUniquePtr<FIMAcousticPreparedCapture> IMPreparedCapture; // GT owns until one context consumes it.

// Q5 upstream timing is an explicit process flag. Ordinary runtime and tests
// execute the exact old path and do not emit these diagnostics.
bool IMMetaSoundUpstreamTimingEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("IMAcousticUpstreamTiming"));
	return bEnabled;
}

uint64 IMMetaSoundUpstreamPhysicalBytes()
{
	return IMMetaSoundUpstreamTimingEnabled() ? FPlatformMemory::GetStats().UsedPhysical : 0;
}

void IMLogMetaSoundUpstreamTiming(const TCHAR* Event, double StartedAt, uint64 Bytes,
	uint32 DeviceId, uint64 Epoch, uint64 Detail = 0, uint64 Auxiliary = 0,
	uint64 PhysicalBefore = 0)
{
	if (!IMMetaSoundUpstreamTimingEnabled()) return;
	const double Now = FPlatformTime::Seconds();
	const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
	const uint64 PhysicalDelta = PhysicalBefore > 0 && Memory.UsedPhysical >= PhysicalBefore
		? Memory.UsedPhysical - PhysicalBefore : 0;
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticUpstreamTiming event=%s wall_s=%.9f elapsed_ms=%.3f bytes=%llu physical_delta_bytes=%llu device=%u epoch=%llu gt_frame=%llu detail=%llu auxiliary=%llu"),
		Event, Now, StartedAt > 0.0 ? (Now - StartedAt) * 1000.0 : 0.0,
		static_cast<unsigned long long>(Bytes), static_cast<unsigned long long>(PhysicalDelta),
		DeviceId, static_cast<unsigned long long>(Epoch),
		static_cast<unsigned long long>(GFrameCounter),
		static_cast<unsigned long long>(Detail), static_cast<unsigned long long>(Auxiliary));
}

bool IM_GetCaptureSettings(uint32 SampleRate, FIMAcousticCaptureSettings& Settings, FString& Error)
{
	if (!CaptureNextContext)
	{
		Error = TEXT("MetaSound test capture is not enabled.");
		return false;
	}
	// The existing 200-second stereo arrays use int32 lengths. Validate before
	// multiplying; this is a test-storage bound, not a new device rate default.
	if (SampleRate == 0 || SampleRate > uint32(MAX_int32 / 400))
	{
		Error = TEXT("The actual device sample rate cannot size the 200-second test capture arrays safely.");
		return false;
	}
	Settings.Revision = IMCaptureSettingsRevision;
	Settings.SampleRate = SampleRate;
	Settings.SourceOverride = IMCaptureNextSourceTraceCapacity;
	Settings.EnvironmentOverride = IMCaptureNextEnvironmentTraceCapacity;
	Settings.PcmCapacity = int32(SampleRate * 200u);
	Settings.CapturePCM = IMCaptureNextPCM;
	Settings.CaptureBusReads = IMCaptureNextBusReads;
	Settings.CaptureTransportOrder = IMCaptureNextTransportOrder;
	if (Settings.CaptureTransportOrder && !Settings.CaptureBusReads)
	{ Error = TEXT("Joint transport order capture requires the existing bus-reader observation."); return false; }
	const uint32 DefaultSourceTraceCapacity = FMath::Min<uint32>(
		uint32(Settings.PcmCapacity / FIMAcousticMetaSoundContext::Frames) * FIMAcousticMetaSoundContext::MaxVoices,
		FIMAcousticMetaSoundContext::CapturedSourceBlockTraceDefaultCapacity);
	Settings.SourceTraceCapacity = FMath::Min<uint32>(
		Settings.SourceOverride != 0 ? Settings.SourceOverride : DefaultSourceTraceCapacity,
		FIMAcousticMetaSoundContext::CapturedSourceBlockTraceMaxCapacity);
	const uint32 DefaultEnvironmentTraceCapacity = uint32(Settings.PcmCapacity / FIMAcousticMetaSoundContext::Frames);
	Settings.EnvironmentTraceCapacity = Settings.EnvironmentOverride != 0
		? FMath::Min<uint32>(Settings.EnvironmentOverride, FIMAcousticMetaSoundContext::CapturedEnvironmentBlockTraceMaxCapacity)
		: DefaultEnvironmentTraceCapacity;
	return true;
}

bool IM_CaptureStorageHasExpectedCapacity(const FIMAcousticCaptureSettings& Settings,
	const FIMAcousticCaptureStorage& Storage)
{
	const int32 Pcm = Settings.CapturePCM ? Settings.PcmCapacity : 0;
	const uint32 Words = (Settings.SourceTraceCapacity + 63u) / 64u;
	return Storage.Source.Num() == Pcm && Storage.Bus.Num() == Pcm
		&& Storage.Dry.Num() == Pcm * 2 && Storage.Wet.Num() == Pcm * 2
		&& Storage.SourceBlocks.Num() == int32(Settings.SourceTraceCapacity)
		&& Storage.EnvironmentBlocks.Num() == int32(Settings.EnvironmentTraceCapacity)
		&& Storage.BusReads.Num() == (Settings.CaptureBusReads ? int32(Settings.EnvironmentTraceCapacity) : 0)
		&& Storage.ReadyWordCount == Words && bool(Storage.Ready) == (Words > 0)
		&& Storage.TransportOrder.Num() == (Settings.CaptureTransportOrder ? int32(FIMAcousticMetaSoundContext::TransportOrderEventCapacity) : 0)
		&& Storage.TransportReadyWordCount == (Settings.CaptureTransportOrder ? FIMAcousticMetaSoundContext::TransportOrderEventCapacity / 64u : 0)
		&& bool(Storage.TransportReady) == Settings.CaptureTransportOrder;
}

void IM_AllocateCaptureStorage(const FIMAcousticCaptureSettings& Settings,
	FIMAcousticCaptureStorage& Storage, uint32 DeviceId, uint64 Epoch, bool bPrepared)
{
	check(IsInGameThread());
	const bool bTiming = IMMetaSoundUpstreamTimingEnabled();
	if (Settings.CapturePCM)
	{
		const double StartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
		const uint64 PhysicalBefore = bTiming ? IMMetaSoundUpstreamPhysicalBytes() : 0;
		Storage.Source.SetNumZeroed(Settings.PcmCapacity);
		Storage.Dry.SetNumZeroed(Settings.PcmCapacity * 2);
		Storage.Wet.SetNumZeroed(Settings.PcmCapacity * 2);
		Storage.Bus.SetNumZeroed(Settings.PcmCapacity);
		IMLogMetaSoundUpstreamTiming(bPrepared ? TEXT("metasound_capture_prepare_pcm_zero") : TEXT("metasound_context_captured_pcm_zero"),
			StartedAt, uint64(Settings.PcmCapacity) * 6ull * sizeof(float), DeviceId, Epoch,
			uint64(Settings.PcmCapacity), 6, PhysicalBefore);
	}
	const double ReadyStartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
	const uint64 ReadyPhysicalBefore = bTiming ? IMMetaSoundUpstreamPhysicalBytes() : 0;
	Storage.ReadyWordCount = (Settings.SourceTraceCapacity + 63u) / 64u;
	if (Storage.ReadyWordCount > 0)
	{
		Storage.Ready = std::make_unique<std::atomic<uint64>[]>(Storage.ReadyWordCount);
		for (uint32 Word = 0; Word < Storage.ReadyWordCount; ++Word)
			Storage.Ready[Word].store(0, std::memory_order_relaxed);
	}
	IMLogMetaSoundUpstreamTiming(bPrepared ? TEXT("metasound_capture_prepare_source_trace_ready") : TEXT("metasound_context_source_trace_ready"),
		ReadyStartedAt, uint64(Storage.ReadyWordCount) * sizeof(std::atomic<uint64>),
		DeviceId, Epoch, Settings.SourceTraceCapacity, Storage.ReadyWordCount, ReadyPhysicalBefore);
	const double TraceStartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
	const uint64 TracePhysicalBefore = bTiming ? IMMetaSoundUpstreamPhysicalBytes() : 0;
	Storage.SourceBlocks.SetNumZeroed(int32(Settings.SourceTraceCapacity));
	Storage.EnvironmentBlocks.SetNumZeroed(int32(Settings.EnvironmentTraceCapacity));
	if (Settings.CaptureBusReads) Storage.BusReads.SetNumZeroed(int32(Settings.EnvironmentTraceCapacity));
	if (Settings.CaptureTransportOrder)
	{
		Storage.TransportOrder.SetNumZeroed(int32(FIMAcousticMetaSoundContext::TransportOrderEventCapacity));
		Storage.TransportReadyWordCount = FIMAcousticMetaSoundContext::TransportOrderEventCapacity / 64u;
		Storage.TransportReady = std::make_unique<std::atomic<uint64>[]>(Storage.TransportReadyWordCount);
		for (uint32 Word = 0; Word < Storage.TransportReadyWordCount; ++Word)
			Storage.TransportReady[Word].store(0, std::memory_order_relaxed);
	}
	const uint64 TraceBytes = uint64(Settings.SourceTraceCapacity) * sizeof(FIMAcousticBlockProbe)
		+ uint64(Settings.EnvironmentTraceCapacity) * sizeof(FIMAcousticMetaSoundBlock)
		+ uint64(Storage.BusReads.Num()) * sizeof(FIMAcousticBusReadBlock)
		+ uint64(Storage.TransportOrder.Num()) * sizeof(FIMAcousticTransportOrderEvent)
		+ uint64(Storage.TransportReadyWordCount) * sizeof(std::atomic<uint64>);
	IMLogMetaSoundUpstreamTiming(bPrepared ? TEXT("metasound_capture_prepare_trace_allocation") : TEXT("metasound_context_trace_allocation"),
		TraceStartedAt, TraceBytes, DeviceId, Epoch, Settings.SourceTraceCapacity, Settings.EnvironmentTraceCapacity, TracePhysicalBefore);
}

void IM_TransferCaptureStorage(FIMAcousticMetaSoundContext& Context,
	FIMAcousticCaptureStorage& Storage, uint64 PreparationId)
{
	check(IsInGameThread());
	// This helper can only initialize a fresh, unpublished context. In
	// particular, cancellation must never reuse it to clear a live prefix.
	check(Context.CapturedSource.IsEmpty() && Context.CapturedDry.IsEmpty()
		&& Context.CapturedWet.IsEmpty() && Context.CapturedBus.IsEmpty()
		&& Context.CapturedSourceBlocks.IsEmpty() && Context.CapturedBlocks.IsEmpty()
		&& Context.CapturedBusReads.IsEmpty() && !Context.CapturedSourceBlockReady
		&& Context.CapturedTransportOrder.IsEmpty() && !Context.CapturedTransportOrderReady);
	check(Context.CapturedSourceFrames.load(std::memory_order_relaxed) == 0
		&& Context.CapturedSourceBlockCount.load(std::memory_order_relaxed) == 0
		&& Context.CapturedSourceBlockClaimCount.load(std::memory_order_relaxed) == 0
		&& Context.CapturedEnvironmentFrames.load(std::memory_order_relaxed) == 0
		&& Context.CapturedBlockCount.load(std::memory_order_relaxed) == 0
		&& Context.CapturedEnvironmentTraceBlockCount.load(std::memory_order_relaxed) == 0
		&& Context.CapturedBusReadCount.load(std::memory_order_relaxed) == 0
		&& Context.CapturedTransportOrderClaims.load(std::memory_order_relaxed) == 0);
	const float* Source = Storage.Source.GetData();
	const float* Dry = Storage.Dry.GetData();
	const float* Wet = Storage.Wet.GetData();
	const float* Bus = Storage.Bus.GetData();
	const FIMAcousticBlockProbe* SourceBlocks = Storage.SourceBlocks.GetData();
	const FIMAcousticMetaSoundBlock* EnvironmentBlocks = Storage.EnvironmentBlocks.GetData();
	const FIMAcousticBusReadBlock* BusReads = Storage.BusReads.GetData();
	const std::atomic<uint64>* Ready = Storage.Ready.get();
	const FIMAcousticTransportOrderEvent* TransportOrder = Storage.TransportOrder.GetData();
	const std::atomic<uint64>* TransportReady = Storage.TransportReady.get();
	Context.CapturedSource = MoveTemp(Storage.Source);
	Context.CapturedDry = MoveTemp(Storage.Dry);
	Context.CapturedWet = MoveTemp(Storage.Wet);
	Context.CapturedBus = MoveTemp(Storage.Bus);
	Context.CapturedSourceBlocks = MoveTemp(Storage.SourceBlocks);
	Context.CapturedBlocks = MoveTemp(Storage.EnvironmentBlocks);
	Context.CapturedBusReads = MoveTemp(Storage.BusReads);
	Context.CapturedSourceBlockReadyWordCount = Storage.ReadyWordCount;
	Context.CapturedSourceBlockReady = std::move(Storage.Ready);
	Context.CapturedTransportOrder = MoveTemp(Storage.TransportOrder);
	Context.CapturedTransportOrderReadyWordCount = Storage.TransportReadyWordCount;
	Context.CapturedTransportOrderReady = std::move(Storage.TransportReady);
	Context.TestCapturePreparationId = PreparationId;
	// Prove storage was moved rather than copied/reallocated, without exposing
	// addresses in evidence. Operators acquire these immutable handles later.
	check(Source == Context.CapturedSource.GetData() && Dry == Context.CapturedDry.GetData()
		&& Wet == Context.CapturedWet.GetData() && Bus == Context.CapturedBus.GetData()
		&& SourceBlocks == Context.CapturedSourceBlocks.GetData()
		&& EnvironmentBlocks == Context.CapturedBlocks.GetData()
		&& BusReads == Context.CapturedBusReads.GetData() && Ready == Context.CapturedSourceBlockReady.get()
		&& TransportOrder == Context.CapturedTransportOrder.GetData() && TransportReady == Context.CapturedTransportOrderReady.get());
}
}

uint32 IM_RecordAcousticTransportOrder(FIMAcousticMetaSoundContext& Context, FIMAcousticTransportOrderEvent Event)
{
	if (Context.CapturedTransportOrder.IsEmpty()) return MAX_uint32;
	const double Seconds = Event.Seconds > 0.0 ? Event.Seconds : FPlatformTime::Seconds();
	const uint32 Index = Context.CapturedTransportOrderClaims.fetch_add(1, std::memory_order_relaxed);
	const uint32 Word = Index / 64u;
	if (Index >= uint32(Context.CapturedTransportOrder.Num()) || !Context.CapturedTransportOrderReady
		|| Word >= Context.CapturedTransportOrderReadyWordCount)
	{
		Context.CapturedTransportOrderOverflow.store(true, std::memory_order_release);
		return MAX_uint32; // Diagnostics never change transport, counters or PCM.
	}
	Event.Index = Index;
	if (Event.Group == MAX_uint32) Event.Group = Index;
	Event.Seconds = Seconds;
	Event.ThreadId = FPlatformTLS::GetCurrentThreadId();
	Event.DeviceId = Context.DeviceId;
	Event.SampleRate = Context.Device ? Context.Device->SampleRate : 0;
	Event.WorldGeneration = Context.Pool ? Context.Pool->WorldGeneration : 0;
	Context.CapturedTransportOrder[int32(Index)] = Event;
	Context.CapturedTransportOrderReady[Word].fetch_or(uint64(1) << (Index % 64u), std::memory_order_release);
	// Only GT scans the ready prefix; callback work is bounded to this row.
	return Index;
}

void IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(
	bool Enabled,
	uint32 SourceTraceCapacityOverride,
	uint32 EnvironmentTraceCapacityOverride,
	bool bCapturePCM,
	bool bCaptureBusReads,
	bool bCaptureTransportOrder)
{
	check(IsInGameThread());
	// Every call is a new test-settings session. Invalidate only unconsumed
	// GT storage; existing graph contexts retain their arrays and prefixes.
	IMAcousticMetaSoundPrivate::IMPreparedCapture.Reset();
	++IMAcousticMetaSoundPrivate::IMCaptureSettingsRevision;
	IMAcousticMetaSoundPrivate::CaptureNextContext = Enabled;
	IMAcousticMetaSoundPrivate::IMCaptureNextSourceTraceCapacity = Enabled ? SourceTraceCapacityOverride : 0;
	IMAcousticMetaSoundPrivate::IMCaptureNextEnvironmentTraceCapacity = Enabled ? EnvironmentTraceCapacityOverride : 0;
	IMAcousticMetaSoundPrivate::IMCaptureNextPCM = Enabled && bCapturePCM;
	IMAcousticMetaSoundPrivate::IMCaptureNextBusReads = Enabled && bCaptureBusReads;
	IMAcousticMetaSoundPrivate::IMCaptureNextTransportOrder = Enabled && bCaptureTransportOrder;
}

bool IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(
	uint32 DeviceId, uint32 SampleRate, uint64& InOutPreparationId, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	IMAcousticMetaSoundPrivate::FIMAcousticCaptureSettings Settings;
	if (!IMAcousticMetaSoundPrivate::IM_GetCaptureSettings(SampleRate, Settings, OutError)) return false;
	if (InOutPreparationId != 0)
	{
		// Nonzero IDs are validate-only. Never reallocate or silently replace
		// a consumed/cancelled request between recorder start and source Play.
		if (!IMAcousticMetaSoundPrivate::IMPreparedCapture || IMAcousticMetaSoundPrivate::IMPreparedCapture->Id != InOutPreparationId)
		{
			OutError = TEXT("The capture preparation ID is stale, cancelled or already consumed.");
			return false;
		}
		if (IMAcousticMetaSoundPrivate::IMPreparedCapture->DeviceId != DeviceId || !IMAcousticMetaSoundPrivate::IMPreparedCapture->Settings.Matches(Settings)
			|| !IMAcousticMetaSoundPrivate::IM_CaptureStorageHasExpectedCapacity(Settings, IMAcousticMetaSoundPrivate::IMPreparedCapture->Storage))
		{
			OutError = TEXT("The prepared capture device, sample rate or enable-session settings changed.");
			return false;
		}
		return true;
	}
	if (IMAcousticMetaSoundPrivate::IMPreparedCapture)
	{
		OutError = TEXT("Another unconsumed capture preparation already owns the pending storage.");
		return false;
	}
	if (IMAcousticMetaSound::FindAcousticMetaSoundContext(DeviceId).IsValid())
	{
		OutError = TEXT("A context already exists on the requested capture device.");
		return false;
	}
	if (IMAcousticMetaSoundPrivate::IMNextCapturePreparationId == MAX_uint64)
	{
		OutError = TEXT("Capture preparation IDs are exhausted; IDs must not be recycled.");
		return false;
	}
	const bool bTiming = IMAcousticMetaSoundPrivate::IMMetaSoundUpstreamTimingEnabled();
	const double StartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
	const uint64 PhysicalBefore = bTiming ? IMAcousticMetaSoundPrivate::IMMetaSoundUpstreamPhysicalBytes() : 0;
	auto Prepared = MakeUnique<IMAcousticMetaSoundPrivate::FIMAcousticPreparedCapture>();
	Prepared->DeviceId = DeviceId;
	Prepared->Settings = Settings;
	Prepared->Id = ++IMAcousticMetaSoundPrivate::IMNextCapturePreparationId;
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_capture_prepare_begin"), 0.0, 0,
		DeviceId, 0, Prepared->Id, Settings.Revision);
	IMAcousticMetaSoundPrivate::IM_AllocateCaptureStorage(Settings, Prepared->Storage, DeviceId, 0, true);
	if (!IMAcousticMetaSoundPrivate::IM_CaptureStorageHasExpectedCapacity(Settings, Prepared->Storage))
	{
		OutError = TEXT("The constructed capture storage does not match the requested capacities.");
		return false;
	}
	InOutPreparationId = Prepared->Id;
	IMAcousticMetaSoundPrivate::IMPreparedCapture = MoveTemp(Prepared);
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_capture_prepare_end"), StartedAt, 0,
		DeviceId, 0, InOutPreparationId, Settings.Revision, PhysicalBefore);
	return true;
}

bool IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(uint64 PreparationId)
{
	check(IsInGameThread());
	if (!PreparationId || !IMAcousticMetaSoundPrivate::IMPreparedCapture || IMAcousticMetaSoundPrivate::IMPreparedCapture->Id != PreparationId) return false;
	const uint32 DeviceId = IMAcousticMetaSoundPrivate::IMPreparedCapture->DeviceId;
	const uint64 Revision = IMAcousticMetaSoundPrivate::IMPreparedCapture->Settings.Revision;
	IMAcousticMetaSoundPrivate::IMPreparedCapture.Reset(); // No operator has ever owned pending storage.
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_capture_prepare_cancel"), 0.0, 0,
		DeviceId, 0, PreparationId, Revision);
	return true;
}
FIMAcousticMetaSoundContextPtr IMAcousticMetaSound::CreateAcousticMetaSoundContext(uint32 DeviceId, uint32 SampleRate, uint64 Epoch)
{
	check(IsInGameThread());
	const bool bTiming = IMAcousticMetaSoundPrivate::IMMetaSoundUpstreamTimingEnabled();
	const double ContextStartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
	const uint64 ContextPhysicalBefore = bTiming ? IMAcousticMetaSoundPrivate::IMMetaSoundUpstreamPhysicalBytes() : 0;
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_context_begin"), 0.0, 0, DeviceId, Epoch);
	FScopeLock Lock(&IMAcousticMetaSoundPrivate::MetaSoundRegistryMutex);
	if (auto* Existing = IMAcousticMetaSoundPrivate::MetaSoundRegistry.Find(DeviceId); Existing && Existing->IsValid()) return nullptr;
	IMAcousticMetaSoundPrivate::FIMAcousticCaptureSettings CaptureSettings;
	FString CaptureError;
	if (IMAcousticMetaSoundPrivate::CaptureNextContext && !IMAcousticMetaSoundPrivate::IM_GetCaptureSettings(SampleRate, CaptureSettings, CaptureError))
	{
		UE_LOG(LogTemp, Warning, TEXT("IMLogs AcousticCapturePreparation context_rejected device=%u reason=%s"), DeviceId, *CaptureError);
		return nullptr;
	}
	const bool bConsumePrepared = IMAcousticMetaSoundPrivate::CaptureNextContext && IMAcousticMetaSoundPrivate::IMPreparedCapture && IMAcousticMetaSoundPrivate::IMPreparedCapture->DeviceId == DeviceId;
	if (bConsumePrepared && (!IMAcousticMetaSoundPrivate::IMPreparedCapture->Settings.Matches(CaptureSettings)
		|| !IMAcousticMetaSoundPrivate::IM_CaptureStorageHasExpectedCapacity(CaptureSettings, IMAcousticMetaSoundPrivate::IMPreparedCapture->Storage)))
	{
		// Retain pending ownership for the fixture's explicit failure cleanup;
		// recreating storage here would hide an invalid prerecord preparation.
		UE_LOG(LogTemp, Warning, TEXT("IMLogs AcousticCapturePreparation context_rejected device=%u id=%llu reason=rate_or_settings_or_capacity_mismatch"),
			DeviceId, static_cast<unsigned long long>(IMAcousticMetaSoundPrivate::IMPreparedCapture->Id));
		return nullptr;
	}
	auto Context = MakeShared<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>();
	Context->DeviceId = DeviceId;
	Context->Device = MakeShared<FIMAcousticDeviceBridge, ESPMode::ThreadSafe>();
	Context->Device->SampleRate = SampleRate;
	Context->Device->BlockFrames = FIMAcousticMetaSoundContext::Frames;
	const double ProbeBufferStartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
	const uint64 ProbeBufferPhysicalBefore = bTiming ? IMAcousticMetaSoundPrivate::IMMetaSoundUpstreamPhysicalBytes() : 0;
	Context->Device->InitProbes();
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_context_init_block_probe_buffer"), ProbeBufferStartedAt, 0,
		DeviceId, Epoch, FIMAcousticDeviceBridge::ProbeSnapshotCapacity, 0, ProbeBufferPhysicalBefore);
	const double VoicesStartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
	const uint64 VoicesPhysicalBefore = bTiming ? IMAcousticMetaSoundPrivate::IMMetaSoundUpstreamPhysicalBytes() : 0;
	for (uint32 I = 0; I < FIMAcousticMetaSoundContext::MaxVoices; ++I)
	{
		Context->Device->Voices.Add(MakeUnique<FIMAcousticVoiceBridge>());
	}
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_context_voice_allocation"), VoicesStartedAt, 0,
		DeviceId, Epoch, FIMAcousticMetaSoundContext::MaxVoices,
		Context->Device->Voices.GetAllocatedSize(), VoicesPhysicalBefore);
	Context->Device->Alive.store(true, std::memory_order_release);
	const double PoolStartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
	const uint64 PoolPhysicalBefore = bTiming ? IMAcousticMetaSoundPrivate::IMMetaSoundUpstreamPhysicalBytes() : 0;
	Context->Pool = MakeShared<FIMAcousticReverbPool, ESPMode::ThreadSafe>(Epoch);
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_context_pool_allocation"), PoolStartedAt, 0,
		DeviceId, Epoch, 1, 0, PoolPhysicalBefore);
	if (IMAcousticMetaSoundPrivate::CaptureNextContext)
	{
		if (bConsumePrepared)
		{
			const double TransferStartedAt = bTiming ? FPlatformTime::Seconds() : 0.0;
			const uint64 PreparationId = IMAcousticMetaSoundPrivate::IMPreparedCapture->Id;
			IMAcousticMetaSoundPrivate::IM_TransferCaptureStorage(*Context, IMAcousticMetaSoundPrivate::IMPreparedCapture->Storage, PreparationId);
			IMAcousticMetaSoundPrivate::IMPreparedCapture.Reset();
			IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_context_capture_transfer"), TransferStartedAt, 0,
				DeviceId, Epoch, PreparationId, CaptureSettings.Revision);
		}
		else
		{
			// Existing fixtures that do not opt into prepare keep the same
			// rate/override/PCM/bus allocation semantics and zero-origin data.
			IMAcousticMetaSoundPrivate::FIMAcousticCaptureStorage Storage;
			IMAcousticMetaSoundPrivate::IM_AllocateCaptureStorage(CaptureSettings, Storage, DeviceId, Epoch, false);
			IMAcousticMetaSoundPrivate::IM_TransferCaptureStorage(*Context, Storage, 0);
		}
	}
	IMAcousticMetaSoundPrivate::MetaSoundRegistry.Add(DeviceId, Context);
	IMAcousticMetaSoundPrivate::IMLogMetaSoundUpstreamTiming(TEXT("metasound_context_end"), ContextStartedAt, 0, DeviceId, Epoch,
		Context->Device->Voices.Num(), Context->CapturedSourceBlocks.Num(), ContextPhysicalBefore);
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
		if (Context->AudioIds[I].load(std::memory_order_acquire) != 0) continue;
		if (Context->SourceSendTransportEnabled)
		{
			Context->SourceSendPatches[I] = MakeShared<Audio::FPatchOutput, ESPMode::ThreadSafe>(
				Context->SourceSendCapacityFrames);
			Context->SourceSendInputs[I] = MakeUnique<Audio::FPatchInput>(Context->SourceSendPatches[I]);
		}
		// Operators and the environment reader acquire this identity before
		// reading the immutable queue handles above. Slots are never reused.
		Context->AudioIds[I].store(AudioId, std::memory_order_release);
		return I;
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
