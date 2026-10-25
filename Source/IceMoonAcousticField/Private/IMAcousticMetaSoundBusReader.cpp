#include "IMAcousticMetaSound.h"
#include "IMAcousticTiming.h"
#include "MetasoundAudioBus.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "Misc/ScopeExit.h"
#include "Sound/AudioBus.h"
#include "Algo/AllOf.h"

// No allocation, logging, queue mutation or UObject lookup. The optional
// bracket samples are diagnostic; Finish's reason identifies the actual branch.
namespace IMAcousticMetaSoundBusReaderPrivate
{
class FIMAcousticBusReadObservation
{
public:
	FIMAcousticBusReadObservation(FIMAcousticMetaSoundContext& InContext, int32 Frames, bool& InPrimed)
		: C(InContext), Primed(InPrimed)
	{
		if (C.CapturedBusReads.IsEmpty()) return;
		Index = C.CapturedBusReadCount.load(std::memory_order_relaxed);
		if (Index >= uint32(C.CapturedBusReads.Num()))
		{
			C.CapturedBusReadOverflow.store(true, std::memory_order_release);
			return;
		}
		Row = &C.CapturedBusReads[int32(Index)];
		*Row = {};
		Row->Ordinal = Index;
		Row->Frames = Frames;
		Row->PrimeFrames = C.BusPrimeFrames;
		Row->CapacityFrames = C.SourceSendCapacityFrames;
		Row->BeginSeconds = FPlatformTime::Seconds();
		Row->PrimedBefore = Primed;
		Row->TransportFailedBefore = C.SourceSendTransportFailed.load(std::memory_order_acquire);
		Row->UnderrunsBefore = C.BusUnderruns.load(std::memory_order_relaxed);
		if (!C.CapturedTransportOrder.IsEmpty())
		{
			FIMAcousticTransportOrderEvent Event;
			Event.Phase = EIMAcousticTransportOrderPhase::ReaderBegin; Event.ReaderOrdinal = Index;
			Event.Frames = Frames; Event.Seconds = Row->BeginSeconds;
			TransportGroup = IM_RecordAcousticTransportOrder(C, Event);
		}
	}

	~FIMAcousticBusReadObservation()
	{
		if (!Row) return;
		Row->PrimedAfter = Primed;
		Row->TransportFailedAfter = C.SourceSendTransportFailed.load(std::memory_order_acquire);
		Row->UnderrunsAfter = C.BusUnderruns.load(std::memory_order_relaxed);
		Row->ReadBlocks = C.BusReadBlocks.load(std::memory_order_relaxed);
		Row->PrimingBlocks = C.BusPrimingBlocks.load(std::memory_order_relaxed);
		Row->SilentBlocks = C.BusSilentBlocks.load(std::memory_order_relaxed);
		Row->PushBlocks = C.SourceSendPushBlocks.load(std::memory_order_acquire);
		Row->PushFailures = C.SourceSendPushFailures.load(std::memory_order_acquire);
		if (Row->BlockingSlot != INDEX_NONE)
		{
			const uint32 Slot = uint32(Row->BlockingSlot);
			Row->BlockingActiveAfter = C.SourceSendActive[Slot].load(std::memory_order_acquire);
			Row->BlockingPushBlocks = C.SourceSendBlockCounts[Slot].load(std::memory_order_acquire);
			Row->BlockingQueueAfter = C.SourceSendPatches[Slot]->GetNumSamplesAvailable();
		}
		Row->EndSeconds = FPlatformTime::Seconds();
		C.CapturedBusReadCount.store(Index + 1u, std::memory_order_release);
		Record(EIMAcousticTransportOrderPhase::ReaderEnd, MAX_uint32, 0, -1, int32(Row->Result), -1,
			EIMAcousticTransportOrderReason::None, Row->ReturnedFresh, Row->EndSeconds);
	}

	void Record(EIMAcousticTransportOrderPhase Phase, uint32 Slot = MAX_uint32, uint64 AudioId = 0,
		int32 Available = -1, int32 Returned = -1, int32 Active = -1,
		EIMAcousticTransportOrderReason Reason = EIMAcousticTransportOrderReason::None,
		bool Fresh = false, double Seconds = 0.0)
	{
		if (TransportGroup == MAX_uint32) return;
		FIMAcousticTransportOrderEvent Event;
		Event.Phase = Phase; Event.Group = TransportGroup; Event.ReaderOrdinal = Index;
		Event.Slot = Slot == MAX_uint32 ? INDEX_NONE : int32(Slot); Event.AudioId = AudioId;
		Event.Frames = Row ? Row->Frames : 0; Event.Available = Available;
		Event.Returned = Returned; Event.Active = Active; Event.Reason = Reason;
		Event.Fresh = Fresh; Event.Seconds = Seconds;
		Event.Underruns = C.BusUnderruns.load(std::memory_order_relaxed);
		if (Slot != MAX_uint32) Event.PushBlocks = C.SourceSendBlockCounts[Slot].load(std::memory_order_relaxed);
		IM_RecordAcousticTransportOrder(C, Event);
	}

	void NoteInput(uint32 Slot, uint64 AudioId, int32 Queued, bool Active)
	{
		if (!Row) return;
		++Row->InputCount;
		if (Active) ++Row->ActiveCount;
		Row->MaxAvailable = FMath::Max(Row->MaxAvailable, Queued);
		if (Row->MinAvailable == -1 || Queued < Row->MinAvailable)
		{
			Row->MinAvailable = Row->BlockingAvailable = Queued;
			Row->BlockingSlot = int32(Slot);
			Row->BlockingAudioId = AudioId;
			Row->BlockingActive = Active;
		}
	}

	void NotePop(Audio::FPatchOutput* Patch, int32 Popped)
	{
		if (TransportGroup != MAX_uint32)
		{
			for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
			{
				const uint64 AudioId = C.AudioIds[Slot].load(std::memory_order_acquire);
				if (AudioId && C.SourceSendPatches[Slot].Get() == Patch)
				{ Record(EIMAcousticTransportOrderPhase::PopReturn, Slot, AudioId, -1, Popped); break; }
			}
		}
		if (!Row) return;
		Row->MinPopped = Row->MinPopped == -1 ? Popped : FMath::Min(Row->MinPopped, Popped);
		if (Popped == Row->Frames) return;
		for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
		{
			const uint64 AudioId = C.AudioIds[Slot].load(std::memory_order_acquire);
			if (!AudioId || C.SourceSendPatches[Slot].Get() != Patch) continue;
			if (Row->BlockingSlot == int32(Slot)) return;
			Row->BlockingSlot = int32(Slot);
			Row->BlockingAudioId = AudioId;
			Row->BlockingActive = C.SourceSendActive[Slot].load(std::memory_order_acquire);
			Row->BlockingAvailable = -1; // The minimum belonged to another queue.
			return;
		}
	}

	bool Finish(EIMAcousticBusReadResult Result, bool Fresh)
	{
		if (Row) { Row->Result = Result; Row->ReturnedFresh = Fresh; }
		Record(EIMAcousticTransportOrderPhase::ReaderDecision, MAX_uint32, 0, -1, int32(Result), -1,
			EIMAcousticTransportOrderReason::None, Fresh);
		return Fresh;
	}

private:
	FIMAcousticMetaSoundContext& C;
	bool& Primed;
	FIMAcousticBusReadBlock* Row = nullptr;
	uint32 Index = 0, TransportGroup = MAX_uint32;
};

}
// Never pop a partial block: a short read would discard timing information.
// After startup, exhaustion is an explicit failed continuity condition, not a
// reason to hide a gap with latest-audio reads or an unbounded realtime wait.
namespace IMAcousticMetaSoundBusReaderPrivate
{
bool ReadAcousticBus(FIMAcousticMetaSoundContext& C, float* Out, int32 Frames, bool& Primed)
{
	FIMAcousticBusReadObservation Observation(C, Frames, Primed);
	FMemory::Memzero(Out, Frames * sizeof(float));
	if (C.Stopped.load(std::memory_order_acquire)) return Observation.Finish(EIMAcousticBusReadResult::Stopped, false);
	if (Frames != int32(FIMAcousticMetaSoundContext::Frames))
	{ C.InvalidBlocks.fetch_add(1, std::memory_order_relaxed); return Observation.Finish(EIMAcousticBusReadResult::InvalidFrames, false); }
	if (C.SourceSendTransportFailed.load(std::memory_order_acquire))
	{ C.BusUnderruns.fetch_add(1, std::memory_order_relaxed); return Observation.Finish(EIMAcousticBusReadResult::LatchedFault, false); }
	std::array<Audio::FPatchOutput*, FIMAcousticMetaSoundContext::MaxVoices> Inputs{};
	int32 InputCount = 0;
	int32 MinimumQueuedSamples = MAX_int32;
	for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
	{
		// AudioId publishes the immutable queue handle. An empty stopped voice
		// cannot hold up other sources; a stopped voice with queued tail drains.
		const uint64 AudioId = C.AudioIds[Slot].load(std::memory_order_acquire);
		if (!AudioId) continue;
		auto* Patch = C.SourceSendPatches[Slot].Get();
		if (!Patch) continue;
		bool Active = C.SourceSendActive[Slot].load(std::memory_order_acquire);
		Observation.Record(EIMAcousticTransportOrderPhase::ActiveLoad, Slot, AudioId, -1, -1, Active);
		Observation.Record(EIMAcousticTransportOrderPhase::QueueReadBegin, Slot, AudioId, -1, -1, Active, EIMAcousticTransportOrderReason::InitialQueue);
		int32 Queued = Patch->GetNumSamplesAvailable();
		Observation.Record(EIMAcousticTransportOrderPhase::QueueReadReturn, Slot, AudioId, Queued, -1, Active, EIMAcousticTransportOrderReason::InitialQueue);
		if (Active && Queued < Frames)
		{
			const bool StillActive = C.SourceSendActive[Slot].load(std::memory_order_acquire);
			Observation.Record(EIMAcousticTransportOrderPhase::ActiveRecheck, Slot, AudioId, Queued, -1, StillActive);
			if (!StillActive)
			{
				Active = false;
				// Acquire inactive after the final push, then re-read its tail.
				Observation.Record(EIMAcousticTransportOrderPhase::QueueReadBegin, Slot, AudioId, -1, -1, Active, EIMAcousticTransportOrderReason::InactiveQueue);
				Queued = Patch->GetNumSamplesAvailable();
				Observation.Record(EIMAcousticTransportOrderPhase::QueueReadReturn, Slot, AudioId, Queued, -1, Active, EIMAcousticTransportOrderReason::InactiveQueue);
			}
		}
		if (!Active && Queued == 0) continue;
		Observation.NoteInput(Slot, AudioId, Queued, Active);
		Inputs[InputCount++] = Patch;
		MinimumQueuedSamples = FMath::Min(MinimumQueuedSamples, Queued);
	}
	if (InputCount == 0)
	{
		if (!Primed)
		{ C.BusPrimingBlocks.fetch_add(1, std::memory_order_relaxed); return Observation.Finish(EIMAcousticBusReadResult::NoInputPriming, false); }
		// Continue feeding silence to the convolution after the last queue
		// drains. Never reset its history or count a stopped source as starvation.
		C.BusSilentBlocks.fetch_add(1, std::memory_order_relaxed);
		C.BusReadBlocks.fetch_add(1, std::memory_order_relaxed);
		return Observation.Finish(EIMAcousticBusReadResult::NoInputSilence, true);
	}
	if (!Primed)
	{
		if (MinimumQueuedSamples < C.BusPrimeFrames)
		{ C.BusPrimingBlocks.fetch_add(1, std::memory_order_relaxed); return Observation.Finish(EIMAcousticBusReadResult::ReservePriming, false); }
		Primed = true;
	}
	C.BusMinAvailable.store(FMath::Min(C.BusMinAvailable.load(), MinimumQueuedSamples), std::memory_order_relaxed);
	C.BusMaxAvailable.store(FMath::Max(C.BusMaxAvailable.load(), MinimumQueuedSamples), std::memory_order_relaxed);
	// Check every producer before consuming any source. No partial pop, latest
	// seek, blocking wait or omitted active voice is used to hide a short block.
	if (MinimumQueuedSamples < Frames)
	{
		FMemory::Memzero(Out, Frames * sizeof(float));
		C.BusUnderruns.fetch_add(1, std::memory_order_relaxed);
		return Observation.Finish(EIMAcousticBusReadResult::ShortAvailable, false);
	}
	std::array<float, FIMAcousticMetaSoundContext::Frames> Source{};
	for (int32 Index = 0; Index < InputCount; ++Index)
	{
		const int32 Popped = Inputs[Index]->PopAudio(Source.data(), Frames, false);
		Observation.NotePop(Inputs[Index], Popped);
		if (Popped != Frames)
		{
			FMemory::Memzero(Out, Frames * sizeof(float));
			C.SourceSendTransportFailed.store(true, std::memory_order_release);
			C.BusUnderruns.fetch_add(1, std::memory_order_relaxed);
			return Observation.Finish(EIMAcousticBusReadResult::ShortPop, false);
		}
		for (int32 Frame = 0; Frame < Frames; ++Frame) Out[Frame] += Source[Frame];
	}
	C.BusReadBlocks.fetch_add(1, std::memory_order_relaxed);
	return Observation.Finish(EIMAcousticBusReadResult::Complete, true);
}

}


namespace IMAcousticMetaSoundBusReaderPrivate
{
	using namespace Metasound;
class FIMAcousticBusReaderOperator final : public TExecutableOperator<FIMAcousticBusReaderOperator>
{
public:
	static const FVertexInterface& GetVertexInterface()
	{
		static const FVertexInterface V(FInputVertexInterface(TInputDataVertex<FAudioBusAsset>(FName(TEXT("Audio Bus")), FDataVertexMetadata{})),
			FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(FName(TEXT("Out 0")), FDataVertexMetadata{})));
		return V;
	}
	static const FNodeClassMetadata& GetNodeInfo()
	{
		static const FNodeClassMetadata Info = []
		{
			FNodeClassMetadata M; M.ClassName = {TEXT("IM"), TEXT("Acoustic Bus Reader"), TEXT("Mono")};
			M.MajorVersion = 1; M.MinorVersion = 0;
			M.DisplayName = FText::FromString(TEXT("Acoustic Bus Reader"));
			M.Description = FText::FromString(TEXT("Complete graph-native source-send blocks with bounded FIFO summing, stop-tail drain and explicit fault accounting."));
			M.Author = TEXT("IceMoon"); M.DefaultInterface = GetVertexInterface(); return M;
		}();
		return Info;
	}
	static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& P, FBuildResults&)
	{
		return MakeUnique<FIMAcousticBusReaderOperator>(P,
			P.InputData.GetOrCreateDefaultDataReadReference<FAudioBusAsset>(TEXT("Audio Bus"), P.OperatorSettings));
	}
	FIMAcousticBusReaderOperator(const FBuildOperatorParams& P, FAudioBusAssetReadRef InBus)
		: Bus(InBus), Output(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)) { Reset(P); }
	~FIMAcousticBusReaderOperator() override { Release(); }
	void Release()
	{
		if (Owned && Context) Context->BusReaderConsumer.store(false, std::memory_order_release);
		Owned = false; Primed = false; Context.Reset();
	}
	void Reset(const IOperator::FResetParams& P)
	{
		Release(); Output->Zero();
		const FName Key = Frontend::SourceInterface::Environment::DeviceID;
		if (!P.Environment.Contains<uint32>(Key)) return;
		Context = IMAcousticMetaSound::FindAcousticMetaSoundContext(P.Environment.GetValue<uint32>(Key));
		if (!Context) return;
		bool Expected = false;
		Owned = Context->BusReaderConsumer.compare_exchange_strong(Expected, true, std::memory_order_acq_rel);
		if (!Owned) Context->DuplicateConsumers.fetch_add(1, std::memory_order_relaxed);
	}
	void BindInputs(FInputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Audio Bus"), Bus); }
	void BindOutputs(FOutputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Out 0"), Output); }
	void Execute()
	{
		const uint64 ExecuteCandidateStart = Context && Owned && Context->Device
			&& Context->Device->ProfilingEnabled.load(std::memory_order_relaxed)
			? FPlatformTime::Cycles64() : 0;
		const bool bMaximumWindow = Context && Owned && Context->Device
			&& Context->CaptureExecuteMaximums.load(std::memory_order_relaxed);
		const bool bPressureScope = bMaximumWindow
			&& Context->Device->TryBeginPressureWindowPublication();
		ON_SCOPE_EXIT
		{
			if (bPressureScope) Context->Device->EndPressureWindowPublication();
		};
		const uint64 ExecuteStart = (!bMaximumWindow || bPressureScope) ? ExecuteCandidateStart : 0;
		const bool bCaptureMaximum = ExecuteStart
			&& Context->CaptureExecuteMaximums.load(std::memory_order_relaxed);
		FIMAcousticExecuteMaximumSample MaximumSample;
		if (bCaptureMaximum)
		{
			MaximumSample.StartCycles = ExecuteStart;
			MaximumSample.BeforeRenderCycles = ExecuteStart;
			MaximumSample.AfterRenderCycles = ExecuteStart;
			MaximumSample.ThreadId = FPlatformTLS::GetCurrentThreadId();
			MaximumSample.Slot = MAX_uint32;
			MaximumSample.WorldGeneration = Context->Pool->WorldGeneration;
			MaximumSample.BeginSeconds = FPlatformTime::Seconds();
		}
		ON_SCOPE_EXIT
		{
			if (ExecuteStart)
			{
				const uint64 Cycles = FPlatformTime::Cycles64() - ExecuteStart;
				IMAcousticTiming::AcousticRecordMaximum(Context->BusExecuteMaxCycles, Cycles);
				Context->BusExecuteCount.fetch_add(1, std::memory_order_release);
				if (bCaptureMaximum)
				{
					MaximumSample.WholeCycles = Cycles;
					Context->BusExecuteMaximum.RecordMaximum(MaximumSample);
				}
			}
		};
		Output->Zero();
		if (!Context || !Owned) return;
		const auto& Proxy = Bus->GetAudioBusProxy();
		if (!Proxy.IsValid() || Proxy->AudioBusId != Context->BusId || Proxy->NumChannels != 1) return;
		if (bCaptureMaximum) MaximumSample.BeforeRenderCycles = FPlatformTime::Cycles64();
		const bool bFresh = IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(*Context, Output->GetData(), Output->Num(), Primed);
		if (bCaptureMaximum)
		{
			MaximumSample.AfterRenderCycles = FPlatformTime::Cycles64();
			MaximumSample.Fresh = bFresh ? 1u : 0u;
		}
	}
private:
	FAudioBusAssetReadRef Bus;
	FAudioBufferWriteRef Output;
	FIMAcousticMetaSoundContextPtr Context;
	bool Owned = false, Primed = false;
};
using FIMAcousticBusReaderNode = TNodeFacade<FIMAcousticBusReaderOperator>;
METASOUND_REGISTER_NODE(FIMAcousticBusReaderNode)
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticBusBufferBoundaries, "IceMoon.AcousticField.MetaSound.BusBufferBoundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticBusBufferBoundaries::RunTest(const FString&)
{
	FIMAcousticMetaSoundContext C;
	C.SourceSendPatches[0] = MakeShared<Audio::FPatchOutput, ESPMode::ThreadSafe>(16384);
	C.AudioIds[0].store(1); C.SourceSendActive[0].store(true); C.BusPrimeFrames = 4096;
	C.SourceSendCapacityFrames = 16384;
	C.CapturedBusReads.SetNumZeroed(32);
	Audio::FPatchInput Producer(C.SourceSendPatches[0]);
	TArray<float> Input, Output; Input.SetNumUninitialized(4096); Output.SetNumZeroed(512);
	for (int32 I=0; I<Input.Num(); ++I) Input[I] = float(I) / 4096.f;
	bool Primed = false;
	Producer.PushAudio(Input.GetData(), 3072);
	TestFalse(TEXT("Short startup reserve does not consume input"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestEqual(TEXT("Startup samples retained"), C.SourceSendPatches[0]->GetNumSamplesAvailable(), 3072);
	TestEqual(TEXT("Reserve priming observation"), uint32(C.CapturedBusReads[0].Result), uint32(EIMAcousticBusReadResult::ReservePriming));
	TestEqual(TEXT("Exact startup queue observed"), C.CapturedBusReads[0].MinAvailable, 3072);
	TestEqual(TEXT("Priming does not become an underrun"), C.CapturedBusReads[0].UnderrunsAfter, uint64(0));
	Producer.PushAudio(Input.GetData()+3072, 1024);
	for (int32 Block=0; Block<8; ++Block)
	{
		TestTrue(TEXT("Complete buffered block"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
		for (int32 I=0; I<512; ++I)
			if (Output[I] != Input[Block*512+I]) { AddError(TEXT("Bus block order or samples changed")); break; }
	}
	TestFalse(TEXT("Exhaustion is rejected"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestEqual(TEXT("Underrun is accounted"), C.BusUnderruns.load(), uint64(1));
	TestEqual(TEXT("Empty active queue fault located"), uint32(C.CapturedBusReads[9].Result), uint32(EIMAcousticBusReadResult::ShortAvailable));
	TestEqual(TEXT("Empty active queue observed"), C.CapturedBusReads[9].MinAvailable, 0);
	TestEqual(TEXT("Underrun row counter published"), C.CapturedBusReads[9].UnderrunsAfter, uint64(1));
	Producer.PushAudio(Input.GetData(), 256);
	IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed);
	TestEqual(TEXT("Partial block is not discarded"), C.SourceSendPatches[0]->GetNumSamplesAvailable(), 256);
	// A producer short-write latches the context. The consumer must then expose
	// silence and failure without consuming the remaining partial queue.
	C.SourceSendTransportFailed.store(true);
	const uint64 UnderrunsBeforeFault = C.BusUnderruns.load();
	Output.Init(1.f, 512);
	TestFalse(TEXT("Latched producer fault rejects the reader"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestTrue(TEXT("Latched fault clears every output sample"), Algo::AllOf(Output, [](float Sample) { return Sample == 0.f; }));
	TestEqual(TEXT("Latched fault preserves queued input"), C.SourceSendPatches[0]->GetNumSamplesAvailable(), 256);
	TestEqual(TEXT("Latched fault is accounted"), C.BusUnderruns.load(), UnderrunsBeforeFault + 1);
	TestTrue(TEXT("Reader cannot clear a producer fault"), C.SourceSendTransportFailed.load());
	C.SourceSendTransportFailed.store(false); // Isolate the existing stop negative control.
	C.Stopped.store(true);
	TestFalse(TEXT("Stopped context cannot consume"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestEqual(TEXT("Every bounded reader call published once"), C.CapturedBusReadCount.load(std::memory_order_acquire), uint32(13));
	TestEqual(TEXT("Latched fault observed separately"), uint32(C.CapturedBusReads[11].Result), uint32(EIMAcousticBusReadResult::LatchedFault));
	TestEqual(TEXT("Stopped context observed separately"), uint32(C.CapturedBusReads[12].Result), uint32(EIMAcousticBusReadResult::Stopped));
	TestFalse(TEXT("Complete trace did not overflow"), C.CapturedBusReadOverflow.load());
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound bus buffer boundaries"), HasAnyErrors()?TEXT("FAIL"):TEXT("PASS"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSound bus buffer boundaries %s"), HasAnyErrors()?TEXT("FAIL"):TEXT("PASS"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticSourceSendAggregation, "IceMoon.AcousticField.MetaSound.SourceSendAggregation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticSourceSendAggregation::RunTest(const FString&)
{
	FIMAcousticMetaSoundContext C;
	constexpr uint32 Voices = 16;
	C.SourceSendCapacityFrames = 2048;
	C.CapturedBusReads.SetNumZeroed(2);
	TArray<float> Input, Output; Input.SetNumUninitialized(512); Output.SetNumZeroed(512);
	for (uint32 Slot = 0; Slot < Voices; ++Slot)
	{
		C.SourceSendPatches[Slot] = MakeShared<Audio::FPatchOutput, ESPMode::ThreadSafe>(2048);
		C.SourceSendInputs[Slot] = MakeUnique<Audio::FPatchInput>(C.SourceSendPatches[Slot]);
		C.AudioIds[Slot].store(Slot + 1); C.SourceSendActive[Slot].store(true);
		for (int32 Frame = 0; Frame < 512; ++Frame) Input[Frame] = float(Slot + 1) + float(Frame) / 512.f;
		TestEqual(TEXT("Every source queues its full block"), C.SourceSendInputs[Slot]->PushAudio(Input.GetData(), 512), 512);
	}
	bool Primed = false;
	TestTrue(TEXT("All sixteen sources contribute"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	for (int32 Frame = 0; Frame < 512; ++Frame)
		if (Output[Frame] != 136.f + 16.f * float(Frame) / 512.f) { AddError(TEXT("Sixteen-source sum omitted or reordered samples")); break; }

	// One partial producer must not consume any of the other fifteen blocks.
	Input.Init(1.f, 512);
	for (uint32 Slot = 0; Slot < Voices; ++Slot)
		C.SourceSendInputs[Slot]->PushAudio(Input.GetData(), Slot == 0 ? 256 : 512);
	TestFalse(TEXT("Partial producer rejects the aggregate"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestEqual(TEXT("All sixteen queues observed"), C.CapturedBusReads[1].InputCount, int32(16));
	TestEqual(TEXT("Partial blocking source located"), C.CapturedBusReads[1].BlockingSlot, int32(0));
	TestEqual(TEXT("Partial queue length observed"), C.CapturedBusReads[1].MinAvailable, int32(256));
	TestEqual(TEXT("Rejected aggregate pops no queue"), C.CapturedBusReads[1].MinPopped, int32(-1));
	for (uint32 Slot = 0; Slot < Voices; ++Slot)
		TestEqual(TEXT("Rejected aggregate retains every queue"), C.SourceSendPatches[Slot]->GetNumSamplesAvailable(), Slot == 0 ? 256 : 512);
	C.SourceSendInputs[0]->PushAudio(Input.GetData(), 256);
	TestTrue(TEXT("Completed partial block is consumed once"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestTrue(TEXT("Completed aggregate contains all voices"), Algo::AllOf(Output, [](float Sample) { return Sample == 16.f; }));
	TestEqual(TEXT("Observation capacity stays fixed"), C.CapturedBusReadCount.load(), uint32(2));
	TestTrue(TEXT("Observation overflow is explicit"), C.CapturedBusReadOverflow.load());
	TestEqual(TEXT("Published rows are not reused after overflow"), C.CapturedBusReads[1].MinAvailable, int32(256));

	// A stopped generator leaves its Context-owned input alive, preserving its
	// queued tail. After draining, the registered empty source does not block.
	for (uint32 Slot = 0; Slot < Voices; ++Slot) C.SourceSendActive[Slot].store(false);
	C.SourceSendInputs[0]->PushAudio(Input.GetData(), 512);
	TestTrue(TEXT("Stopped source queued tail drains"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestTrue(TEXT("Tail samples preserved"), Algo::AllOf(Output, [](float Sample) { return Sample == 1.f; }));
	TestTrue(TEXT("No active inputs feeds silence without starvation"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestTrue(TEXT("Drained output is silent"), Algo::AllOf(Output, [](float Sample) { return Sample == 0.f; }));
	C.SourceSendActive[1].store(true); C.SourceSendInputs[1]->PushAudio(Input.GetData(), 512);
	TestTrue(TEXT("Stopped empty source cannot stall live source"), IMAcousticMetaSoundBusReaderPrivate::ReadAcousticBus(C, Output.GetData(), 512, Primed));
	TestTrue(TEXT("Only live source contributes once"), Algo::AllOf(Output, [](float Sample) { return Sample == 1.f; }));
	TestEqual(TEXT("Only deliberate partial negative counted as underrun"), C.BusUnderruns.load(), uint64(1));
	TestFalse(TEXT("Negative preflight did not corrupt the transport"), C.SourceSendTransportFailed.load());
	return !HasAnyErrors();
}
#endif
