#include "IMAcousticMetaSound.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticPresenceParameters.h"
#include "Tests/IMAcousticAuditionCapture.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundParamHelper.h"
#include "MetasoundPrimitives.h"
#include "MetasoundTrigger.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "Misc/ScopeExit.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

// Raw CPU observation is a separate private opt-in. Order-only journals
// keep both cycles and query-attempt flags at zero.
// Each admitted endpoint queries its own calling thread once. The pseudo handle
// is neither cached nor owned; failure has no fallback. API latency/granularity
// are unknown, and these user+kernel cycles must not be converted to wall time.
namespace IMAcousticMetaSoundNodesPrivate
{
uint32 IM_QueryCurrentThreadCpuCycles(uint64& OutCycles, bool bObserveRawCpu = false)
{
	OutCycles = 0;
	if (!bObserveRawCpu)
	{
		return 0;
	}
#if PLATFORM_WINDOWS
	ULONG64 Cycles = 0;
	const bool Succeeded = ::QueryThreadCycleTime(::GetCurrentThread(), &Cycles) != 0;
	if (Succeeded) OutCycles = uint64(Cycles);
	return uint32(EIMAcousticTransportWorkFlag::ThreadCpuQueryAttempted)
		| (Succeeded ? uint32(EIMAcousticTransportWorkFlag::ThreadCpuQuerySucceeded) : 0u);
#else
	return 0;
#endif
}


// The Steam Audio reverb tail carries more energy below 60 Hz than the dry sound it was made from: the offline convolution of
// the Kenney footsteps with the baked tail adds about 4.5 dB to 20-60 Hz relative to 60-150 Hz (the 0-800 Hz reconstruction band is flat
// down to DC), and in the engine the wet stage held 30-34% of its energy below 60 Hz against 6-15% in the dry presence stage.
// A 2nd-order Butterworth high-pass at 60 Hz (the upper edge of that band) brings the wet share to 9-12%, measured offline on the
// recorded wet stage of job IMAcousticR2-20261021-01 (Saved/ClaudeRuntime/AcousticV2Footstep_20261021/IMHpfProto.py).
constexpr double IMWetHighPassCornerHz = 60.0;

struct FIMWetHighPass
{
	// RBJ cookbook high-pass, Q = 1/sqrt(2) (Butterworth). Transposed direct form II, one state pair per channel.
	void Configure(double SampleRate)
	{
		const double Omega = 2.0 * PI * IMWetHighPassCornerHz / SampleRate;
		const double Alpha = FMath::Sin(Omega) * 0.5 * FMath::Sqrt(2.0); // sin(w) / (2Q) with Q = 1/sqrt(2)
		const double CosOmega = FMath::Cos(Omega), A0 = 1.0 + Alpha;
		B0 = float((1.0 + CosOmega) * 0.5 / A0);
		B1 = float(-(1.0 + CosOmega) / A0);
		A1 = float(-2.0 * CosOmega / A0);
		A2 = float((1.0 - Alpha) / A0);
		Reset();
	}
	void Reset() { for (float& Value : State) Value = 0.f; }
	float Process(int32 Channel, float Input)
	{
		float* Z = State + 2 * Channel;
		const float Output = B0 * Input + Z[0];
		Z[0] = B1 * Input - A1 * Output + Z[1];
		Z[1] = B0 * Input - A2 * Output;
		return Output;
	}
	float B0 = 1.f, B1 = 0.f, A1 = 0.f, A2 = 0.f;
	float State[4]{};
};
}

#define LOCTEXT_NAMESPACE "IMAcousticMetaSound"

namespace IMAcousticMetaSoundNodesPrivate
{
	using namespace Metasound;

DEFINE_METASOUND_PARAM(Mono, "Mono", "完整单声道输入；不预加遮挡或混响。");
DEFINE_METASOUND_PARAM(Voice, "Voice", "由声场普通入口绑定的 voice slot。");
DEFINE_METASOUND_PARAM(Finished, "Finished", "覆盖输入延迟和声学尾部后的源图完成事件；保留最后发送块。");
DEFINE_METASOUND_PARAM(Left, "Left", "左声道。");
DEFINE_METASOUND_PARAM(Right, "Right", "右声道。");
DEFINE_METASOUND_PARAM(Send, "Send", "独立混响发送距离曲线后的原始输入，送共享环境总线。");


template<typename ParamsType>
FIMAcousticMetaSoundContextPtr ContextFromBuild(const ParamsType& Params)
{
		const FName Key = Frontend::SourceInterface::Environment::DeviceID;
		return Params.Environment.template Contains<uint32>(Key)
				? IMAcousticMetaSound::FindAcousticMetaSoundContext(Params.Environment.template GetValue<uint32>(Key)) : nullptr;
}

IPLHRTF CreateOperatorHRTF(const FOperatorSettings& Settings)
{
	IPLContext SDK = IMAcousticSDKContext::GetAcousticSDKContext();
	if (!SDK) return nullptr;
	IPLAudioSettings Audio{int(Settings.GetSampleRate()), Settings.GetNumFramesPerBlock()};
	IPLHRTFSettings HRTFSettings{};
	HRTFSettings.type = IPL_HRTFTYPE_DEFAULT;
	HRTFSettings.volume = 1.0f;
	IPLHRTF HRTF = nullptr;
	return iplHRTFCreate(SDK, &Audio, &HRTFSettings, &HRTF) == IPL_STATUS_SUCCESS ? HRTF : nullptr;
}

void IMPublishCapturedSourceBlock(const FIMAcousticMetaSoundContextPtr& Context, uint32 Block)
{
	const uint32 Word = Block >> 6;
	if (!Context->CapturedSourceBlockReady || Word >= Context->CapturedSourceBlockReadyWordCount) return;
	const uint64 Mask = uint64(1) << (Block & 63u);
	Context->CapturedSourceBlockReady[Word].fetch_or(Mask, std::memory_order_release);

	uint32 Published = Context->CapturedSourceBlockCount.load(std::memory_order_acquire);
	const uint32 Capacity = uint32(Context->CapturedSourceBlocks.Num());
	for (;;)
	{
		uint32 Next = Published;
		while (Next < Capacity)
		{
			const uint32 NextWord = Next >> 6;
			const uint64 NextMask = uint64(1) << (Next & 63u);
			if ((Context->CapturedSourceBlockReady[NextWord].load(std::memory_order_acquire) & NextMask) == 0) break;
			++Next;
		}
		if (Next == Published) return;
		if (Context->CapturedSourceBlockCount.compare_exchange_weak(
			Published, Next, std::memory_order_release, std::memory_order_acquire)) return;
	}
}

class FIMAcousticSourceOperator final : public TExecutableOperator<FIMAcousticSourceOperator>
{
public:
	static const FVertexInterface& GetVertexInterface()
	{
		using namespace IMAcousticMetaSoundNodesPrivate;
		static const FVertexInterface Interface(
			FInputVertexInterface(TInputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Mono)),
				TInputDataVertex<int32>(METASOUND_GET_PARAM_NAME_AND_METADATA(Voice), -1),
				TInputDataVertex<FTrigger>(METASOUND_GET_PARAM_NAME_AND_METADATA(Finished))),
			FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Left)),
				TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Right)),
				TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Send))));
		return Interface;
	}
	static const FNodeClassMetadata& GetNodeInfo()
	{
		static const FNodeClassMetadata Info = []
		{
			FNodeClassMetadata M;
			M.ClassName = {TEXT("IM"), TEXT("Acoustic Source"), TEXT("Stereo")};
			M.MajorVersion = 1; M.MinorVersion = 1;
			M.DisplayName = LOCTEXT("SourceTitle", "Acoustic Source");
			M.Description = LOCTEXT("SourceDescription", "图内直达声和绕射处理，共享环境发送。");
			M.Author = TEXT("IceMoon"); M.DefaultInterface = GetVertexInterface();
			return M;
		}();
		return Info;
	}
	static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& P, FBuildResults&)
	{
		return MakeUnique<FIMAcousticSourceOperator>(P,
			P.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Mono"), P.OperatorSettings),
			P.InputData.GetOrCreateDefaultDataReadReference<int32>(TEXT("Voice"), P.OperatorSettings),
			P.InputData.GetOrCreateDefaultDataReadReference<FTrigger>(TEXT("Finished"), P.OperatorSettings));
	}
	FIMAcousticSourceOperator(const FBuildOperatorParams& P, FAudioBufferReadRef InMono,
		FInt32ReadRef InVoice, FTriggerReadRef InFinished)
		: Mono(InMono), Voice(InVoice), Finished(InFinished), Left(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)),
			Right(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)), Send(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)),
			Context(ContextFromBuild(P)), Frames(P.OperatorSettings.GetNumFramesPerBlock())
	{
		Reset(P);
	}
	void Reset(const IOperator::FResetParams& P)
	{
		ReleaseSource(EIMAcousticTransportOrderReason::Reset);
		Context = ContextFromBuild(P);
		Frames = P.OperatorSettings.GetNumFramesPerBlock();
		Stereo.SetNumZeroed(Frames * 2);
		if (!Context || Frames != int32(FIMAcousticMetaSoundContext::Frames)
			|| P.OperatorSettings.GetSampleRate() != Context->Device->SampleRate) return;
		// Test capture allocates immutable trace arrays before generator construction.
		// Observe construction separately from Execute: startup lateness must not be
		// mistaken for a gap after the audio callback has begun. Ordinary audio emits no log.
		const bool bObserveBuild = !Context->CapturedSourceBlocks.IsEmpty();
		const double BuildStartSeconds = bObserveBuild ? FPlatformTime::Seconds() : 0.0;
		IPLHRTF HRTF = CreateOperatorHRTF(P.OperatorSettings);
		const double HRTFReadySeconds = bObserveBuild ? FPlatformTime::Seconds() : 0.0;
		if (HRTF)
		{
			Ready = Renderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTF, int(P.OperatorSettings.GetSampleRate()), Frames);
			iplHRTFRelease(&HRTF);
			if (Ready)
			{
				Context->Device->SourceRendererInstances.fetch_add(1, std::memory_order_relaxed);
				bRendererCounted = true;
			}
		}
		if (bObserveBuild)
		{
			const double BuildEndSeconds = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticOperatorBuild kind=source device=%u begin_s=%.9f end_s=%.9f hrtf_ms=%.6f renderer_ms=%.6f ready=%d game_thread=%d"),
				Context->DeviceId, BuildStartSeconds, BuildEndSeconds,
				(HRTFReadySeconds - BuildStartSeconds) * 1000.0,
				(BuildEndSeconds - HRTFReadySeconds) * 1000.0, Ready ? 1 : 0, IsInGameThread() ? 1 : 0);
		}
	}
	~FIMAcousticSourceOperator() override { ReleaseSource(EIMAcousticTransportOrderReason::Destruction); }
	void ReleaseSource(EIMAcousticTransportOrderReason Reason = EIMAcousticTransportOrderReason::Explicit)
	{
		const uint32 ReleaseGroup = ObserveTransport(EIMAcousticTransportOrderPhase::ReleaseBegin, MAX_uint32, false, -1, -1, Reason);
		Renderer.Shutdown();
		if (bRendererCounted)
		{
			Context->Device->SourceRendererInstances.fetch_sub(1, std::memory_order_relaxed);
			bRendererCounted = false;
		}
		if (Context && Slot != INDEX_NONE)
		{
			Context->SourceSendActive[Slot].store(false, std::memory_order_release);
			ObserveTransport(EIMAcousticTransportOrderPhase::ReleaseInactive, ReleaseGroup, false, -1, 0, Reason);
			Context->Device->Voices[Slot]->LiveGeneration.fetch_add(1, std::memory_order_acq_rel);
			Context->SourceConsumers[Slot].store(false, std::memory_order_release);
		}
		Slot = INDEX_NONE;
		Latest = {};
		Future = {};
		DelayedMono.fill(0.0f);
		DelayedInputBlock.fill(0.0f);
		DelayedSpatialVersions.fill(0);
		DelayWriteIndex = 0;
		DelayBlocksWritten = 0;
		HasAcceptedSourceResult = false;
		bSourceSendCompleted = false;
		Generation = 0;
		AudioId = 0;
		Ready = false;
	}
	void BindInputs(FInputVertexInterfaceData& D) override
	{
		D.BindReadVertex(TEXT("Mono"), Mono);
		D.BindReadVertex(TEXT("Voice"), Voice);
		D.BindReadVertex(TEXT("Finished"), Finished);
	}
	void BindOutputs(FOutputVertexInterfaceData& D) override
	{ D.BindReadVertex(TEXT("Left"), Left); D.BindReadVertex(TEXT("Right"), Right); D.BindReadVertex(TEXT("Send"), Send); }

	void Execute()
	{
		const uint64 ExecuteCandidateStart = Context && Context->Device
			&& Context->Device->ProfilingEnabled.load(std::memory_order_relaxed)
			? FPlatformTime::Cycles64() : 0;
		// In the optional formal observation, admission closes before export.
		// Hold the nonblocking publication lease through the timing/probe tail;
		// a Renderer still in flight must keep the GT closure gate non-quiescent.
		const bool bMaximumWindow = Context && Context->Device
			&& Context->CaptureExecuteMaximums.load(std::memory_order_relaxed);
		const bool bPressureScope = bMaximumWindow
			&& Context->Device->TryBeginPressureWindowPublication();
		ON_SCOPE_EXIT
		{
			if (bPressureScope) Context->Device->EndPressureWindowPublication();
		};
		const uint64 ExecuteStart = (!bMaximumWindow || bPressureScope) ? ExecuteCandidateStart : 0;
		const uint64 ExecuteStartCpu = ExecuteStart && Context ? Context->BlockCosts.BeginCpuCycles() : 0;
		Left->Zero(); Right->Zero(); Send->Zero();
		if (!Context || Context->Stopped.load(std::memory_order_acquire) || bSourceSendCompleted) return;
		if (!Ready || Mono->Num() != Frames || Frames != int32(FIMAcousticMetaSoundContext::Frames))
		{ Context->InvalidBlocks.fetch_add(1, std::memory_order_relaxed); return; }
		if (Slot == INDEX_NONE)
		{
			if (*Voice < 0 || *Voice >= int32(FIMAcousticMetaSoundContext::MaxVoices)) return;
			const uint64 Id = Context->AudioIds[*Voice].load(std::memory_order_acquire);
			if (!Id) return;
			bool Unowned = false;
			if (!Context->SourceConsumers[*Voice].compare_exchange_strong(Unowned, true, std::memory_order_acq_rel))
			{ Context->DuplicateConsumers.fetch_add(1, std::memory_order_relaxed); return; }
			Slot = *Voice; AudioId = Id;
			Generation = Context->Device->Voices[Slot]->LiveGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		}
		auto& Device = *Context->Device;
		auto& Bridge = *Device.Voices[Slot];
		const bool bCaptureMaximum = ExecuteStart
			&& Context->CaptureExecuteMaximums.load(std::memory_order_relaxed);
		FIMAcousticExecuteMaximumSample MaximumSample;
		if (bCaptureMaximum)
		{
			MaximumSample.StartCycles = ExecuteStart;
			MaximumSample.ThreadId = FPlatformTLS::GetCurrentThreadId();
			MaximumSample.Slot = uint32(Slot);
			MaximumSample.AudioComponentId = AudioId;
			MaximumSample.WorldGeneration = Context->Pool->WorldGeneration;
			MaximumSample.VoiceGeneration = Generation;
		}
		// Scope exit includes the PCM/identity trace tail and every return after
		// a source has a valid device/voice. Start precedes output clearing.
		ON_SCOPE_EXIT
		{
			if (ExecuteStart)
			{
				const uint64 Cycles = FPlatformTime::Cycles64() - ExecuteStart;
				Device.SourceTiming.RecordCycles(Cycles);
				Device.SourceBlockTiming.RecordCycles(Cycles); // Legacy per-source alias.
				IMAcousticTiming::AcousticRecordMaximum(Bridge.SourceExecuteMaxCycles, Cycles);
				Bridge.SourceExecuteCount.fetch_add(1, std::memory_order_release);
				if (bCaptureMaximum)
				{
					MaximumSample.WholeCycles = Cycles;
					Context->SourceExecuteMaximums[Slot].RecordMaximum(MaximumSample);
				}
				Context->BlockCosts.Record(Context->BlockCosts.SourceParticipant(Slot), ExecuteStart, Cycles, ExecuteStartCpu);
			}
		};
		// This scope runs before the timing scope above closes, so the source
		// Execute budget includes the handoff. No allocation or mixer round trip
		// occurs here; the exact existing Send samples (including zeros) enter
		// this voice's GT-preallocated SPSC queue. A short write fails closed.
		// 接入已覆盖输入延迟和声学尾部的图 OnFinished。算子销毁可能晚于
		// 最后 PCM 块，不能单靠析构通知生产结束。先推送最后完整块再发布
		// inactive；保留队列、consumer 和 renderer 到正常销毁。
		const bool bCompleteAfterThisBlock = bool(*Finished);
		const uint32 TransportGroup = ObserveTransport(EIMAcousticTransportOrderPhase::SourceEnter, MAX_uint32, bCompleteAfterThisBlock);
		ON_SCOPE_EXIT
		{
			if (Context->SourceSendTransportEnabled && !Context->Stopped.load(std::memory_order_acquire))
			{
				auto* Producer = Context->SourceSendInputs[Slot].Get();
				ObserveTransport(EIMAcousticTransportOrderPhase::PushBegin, TransportGroup, bCompleteAfterThisBlock);
				const int32 Pushed = Producer ? Producer->PushAudio(Send->GetData(), Frames) : INDEX_NONE;
				ObserveTransport(EIMAcousticTransportOrderPhase::PushReturn, TransportGroup, bCompleteAfterThisBlock, Pushed, -1,
					Producer ? EIMAcousticTransportOrderReason::None : EIMAcousticTransportOrderReason::MissingProducer);
				if (Pushed == Frames)
				{
					Context->SourceSendPushBlocks.fetch_add(1, std::memory_order_relaxed);
					Context->SourceSendBlockCounts[Slot].fetch_add(1, std::memory_order_relaxed);
					Context->SourceSendActive[Slot].store(!bCompleteAfterThisBlock, std::memory_order_release);
					ObserveTransport(EIMAcousticTransportOrderPhase::ActiveStore, TransportGroup, bCompleteAfterThisBlock, -1, !bCompleteAfterThisBlock,
						bCompleteAfterThisBlock ? EIMAcousticTransportOrderReason::Finished : EIMAcousticTransportOrderReason::None);
				}
				else
				{
					Context->SourceSendPushFailures.fetch_add(1, std::memory_order_relaxed);
					Context->SourceSendTransportFailed.store(true, std::memory_order_release);
					Context->InvalidBlocks.fetch_add(1, std::memory_order_relaxed);
				}
			}
			else
			{
				ObserveTransport(EIMAcousticTransportOrderPhase::PushSkipped, TransportGroup, bCompleteAfterThisBlock, -1, -1,
					Context->SourceSendTransportEnabled ? EIMAcousticTransportOrderReason::StoppedContext : EIMAcousticTransportOrderReason::DisabledTransport);
			}
			if (bCompleteAfterThisBlock)
			{
				bSourceSendCompleted = true;
				ObserveTransport(EIMAcousticTransportOrderPhase::SourceCompleted, TransportGroup, true);
				Context->SourceSendActive[Slot].store(false, std::memory_order_release);
				ObserveTransport(EIMAcousticTransportOrderPhase::ActiveStore, TransportGroup, true, -1, 0, EIMAcousticTransportOrderReason::Finished);
			}
		};
		const double Now = FPlatformTime::Seconds();
		const uint64 InputSpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
		const bool bInputDelayPriming = DelayBlocksWritten < FIMAcousticMetaSoundContext::SourceInputDelayBlocks;
		const uint32 DelayOffset = DelayWriteIndex * uint32(Frames);
		const uint64 ExpectedSpatialVersion = bInputDelayPriming ? 0 : DelayedSpatialVersions[DelayWriteIndex];
		FMemory::Memcpy(DelayedInputBlock.data(), DelayedMono.data() + DelayOffset, Frames * sizeof(float));
		FMemory::Memcpy(DelayedMono.data() + DelayOffset, Mono->GetData(), Frames * sizeof(float));
		DelayedSpatialVersions[DelayWriteIndex] = InputSpatialVersion;
		DelayWriteIndex = (DelayWriteIndex + 1) % FIMAcousticMetaSoundContext::SourceInputDelayBlocks;
		if (DelayBlocksWritten < FIMAcousticMetaSoundContext::SourceInputDelayBlocks) ++DelayBlocksWritten;
		// Source operator is the sole request producer and result consumer.
		// Requests renew the existing 250 ms worker lease; a full ring simply
		// already contains newer work than the serial worker can drain.
		Bridge.Requests.Push({uint32(Slot), Generation, AudioId, true});
		const uint32 IdentityInjectionKind = bInputDelayPriming
			? 0 : Context->IdentityInjection.PendingKind.exchange(0, std::memory_order_acq_rel);
		FIMAcousticIdentityInjectionProbe* IdentityProbe =
			IdentityInjectionKind >= 1 && IdentityInjectionKind <= 3
			? &Context->IdentityInjection.Probes[IdentityInjectionKind - 1] : nullptr;
		const uint64 IdentityRenderedBefore = IdentityProbe
			? Device.RenderedBlocks.load(std::memory_order_acquire) : 0;
		const uint64 IdentityRejectedBefore = IdentityProbe
			? Device.RejectedBlocks.load(std::memory_order_acquire) : 0;
		uint32 IdentityRejectDetail = 0;
		bool IdentityInjectionRejected = false;
		if (!bInputDelayPriming)
		{
			AlignResultToSpatialVersion(ExpectedSpatialVersion, IdentityRejectDetail);
			if (IdentityProbe)
			{
				FIMAcousticVoiceResult FrozenA;
				FrozenA.Frame.Generation = Context->IdentityInjection.VoiceGeneration.load(std::memory_order_acquire);
				FrozenA.Frame.Sequence = Context->IdentityInjection.Sequence.load(std::memory_order_acquire);
				FrozenA.Frame.Listener.origin = {
					Context->IdentityInjection.ListenerX.load(std::memory_order_relaxed),
					Context->IdentityInjection.ListenerY.load(std::memory_order_relaxed),
					Context->IdentityInjection.ListenerZ.load(std::memory_order_relaxed)};
				FrozenA.Frame.ListenerLocalDirection = {0.0f, 0.0f, -1.0f};
				FrozenA.AudioComponentId = Context->IdentityInjection.AudioId.load(std::memory_order_acquire);
				FrozenA.WorldGeneration = Context->IdentityInjection.WorldGeneration.load(std::memory_order_acquire);
				FrozenA.PublishedSeconds = Now;
				IdentityProbe->InjectedAudioId.store(FrozenA.AudioComponentId, std::memory_order_relaxed);
				IdentityProbe->InjectedWorldGeneration.store(FrozenA.WorldGeneration, std::memory_order_relaxed);
				IdentityProbe->InjectedVoiceGeneration.store(FrozenA.Frame.Generation, std::memory_order_relaxed);
				IdentityProbe->InjectedSequence.store(FrozenA.Frame.Sequence, std::memory_order_relaxed);
				FrozenA.SpatialVersion = Context->IdentityInjection.SpatialVersion.load(std::memory_order_acquire);
				IdentityProbe->InjectedSpatialVersion.store(FrozenA.SpatialVersion, std::memory_order_relaxed);
				AcceptResult(FrozenA, ExpectedSpatialVersion, true, IdentityRejectDetail, IdentityInjectionRejected);
			}
			FIMAcousticVoiceResult Next;
			for (uint32 Drained = 0; !IdentityInjectionRejected && Drained < 8 && Bridge.Results.Pop(Next); ++Drained)
			{
				AcceptResult(Next, ExpectedSpatialVersion, false, IdentityRejectDetail, IdentityInjectionRejected);
			}
			if (!IdentityInjectionRejected && Latest.Frame.Sequence
				&& Latest.SpatialVersion == ExpectedSpatialVersion)
			{
				IdentityRejectDetail = 0;
			}
		}
		const uint32 Routes = Device.RenderRoutes.load(std::memory_order_relaxed);
		// Results are checked against the spatial version traveling with this
		// delayed source block; the live GT version can be newer by two blocks.
		const uint64 ExpectedWorldGeneration = Context->Pool->WorldGeneration;
		const bool IdentityFresh = Latest.WorldGeneration == ExpectedWorldGeneration
			&& Latest.Frame.Generation == Generation
			&& Latest.AudioComponentId == AudioId
			&& ExpectedSpatialVersion != 0
			&& Latest.SpatialVersion == ExpectedSpatialVersion;
		const bool Fresh = !IdentityInjectionRejected && IdentityFresh && Latest.Frame.Sequence
			&& Now >= Latest.PublishedSeconds && Now - Latest.PublishedSeconds <= .25
			&& Device.Enabled.load(std::memory_order_acquire);
		EIMAcousticProbeReject ProbeReject = EIMAcousticProbeReject::Accepted;
		if (!Fresh)
		{
			if (!Device.Enabled.load(std::memory_order_acquire)) ProbeReject = EIMAcousticProbeReject::Bypassed;
			else if (!Latest.Frame.Sequence || Now < Latest.PublishedSeconds) ProbeReject = EIMAcousticProbeReject::MissingResult;
			else ProbeReject = EIMAcousticProbeReject::StaleResult;
		}
		bool Rendered = false;
		FIMAcousticAudioMetrics Metrics;
		double InputEnergy = 0.0;
		for (int32 I = 0; I < Frames; ++I)
		{
			const float Input = DelayedInputBlock[I];
			if (FMath::IsFinite(Input)) InputEnergy += double(Input) * double(Input);
		}
		if (bCaptureMaximum)
		{
			MaximumSample.BeginSeconds = Now;
			MaximumSample.SpatialVersion = ExpectedSpatialVersion;
			MaximumSample.Sequence = Latest.Frame.Sequence;
			MaximumSample.Fresh = Fresh ? 1u : 0u;
			MaximumSample.BeforeRenderCycles = FPlatformTime::Cycles64();
		}
		if (Fresh)
		{
			const uint32 RendererWorkFlags = CaptureRendererWorkFlags(Routes);
			HasAcceptedSourceResult = true;
			if (RendererWorkFlags)
			{
				uint64 BeforeThreadCpuCycles = 0;
				const uint32 BeforeCpuFlags = IM_QueryCurrentThreadCpuCycles(BeforeThreadCpuCycles);
				ObserveTransport(EIMAcousticTransportOrderPhase::SourceBeforeRenderer, TransportGroup, bCompleteAfterThisBlock,
					-1, -1, EIMAcousticTransportOrderReason::None, MAX_uint32, FPlatformTime::Cycles64(),
					RendererWorkFlags | BeforeCpuFlags, BeforeThreadCpuCycles);
			}
			// Exaggerated presence only reshapes the direct path of slots flagged by the GT.
			// Flag 0 passes nullptr, so the physical render is unchanged.
			const uint8 PresenceFlags = Context->PresenceFlags[Slot].load(std::memory_order_relaxed);
			FIMAcousticDirectShaping Shaping;
			if (PresenceFlags & IMAcousticPresenceFlags::Exaggerated)
			{
				const float PresenceStrength = Context->PresenceStrength.load(std::memory_order_relaxed);
				Shaping.AzimuthScale = FIMAcousticPresenceParameters::DirectAzimuthScaleAt(PresenceStrength);
				Shaping.AzimuthLimitRadians = FMath::DegreesToRadians(FIMAcousticPresenceParameters::DirectAzimuthLimitDegreesAt(PresenceStrength));
				Shaping.LateralAzimuthRadians = Context->PresenceLateralAzimuth[Slot].load(std::memory_order_relaxed);
			}
			Shaping.bUnitDistanceAttenuation = (PresenceFlags & IMAcousticPresenceFlags::SelfNearField) != 0;
			Rendered = Renderer.Render(DelayedInputBlock.data(), Frames, Latest.Frame, Stereo.GetData(), nullptr, nullptr, &Metrics, Routes,
				PresenceFlags ? &Shaping : nullptr);
			if (RendererWorkFlags)
			{
				const uint64 AfterCycles = FPlatformTime::Cycles64();
				uint64 AfterThreadCpuCycles = 0;
				const uint32 AfterCpuFlags = IM_QueryCurrentThreadCpuCycles(AfterThreadCpuCycles);
				const uint32 AfterWorkFlags = RendererWorkFlags | AfterCpuFlags | (Rendered ? uint32(EIMAcousticTransportWorkFlag::Rendered) : 0u);
				ObserveTransport(EIMAcousticTransportOrderPhase::SourceAfterRenderer, TransportGroup, bCompleteAfterThisBlock,
					-1, -1, EIMAcousticTransportOrderReason::None, MAX_uint32, AfterCycles, AfterWorkFlags, AfterThreadCpuCycles);
			}
			const float Gain = Context->ReverbSendGains[Slot].load(std::memory_order_relaxed);
			if (FMath::IsFinite(Gain))
				for (int32 I = 0; I < Frames; ++I)
					Send->GetData()[I] = FMath::IsFinite(DelayedInputBlock[I]) ? DelayedInputBlock[I] * Gain : 0.f;
			else Send->Zero();
		}
		if (!Rendered)
		{
			Renderer.Reset();
			const bool PrimingMissing = bInputDelayPriming
				|| (Device.Enabled.load(std::memory_order_acquire)
					&& !HasAcceptedSourceResult
					&& !IdentityInjectionRejected
					&& IdentityRejectDetail == 0
					&& !Latest.Frame.Sequence
					&& !Future.Frame.Sequence
					&& ProbeReject == EIMAcousticProbeReject::MissingResult);
			if (PrimingMissing)
			{
				Device.PrimingMissingBlocks.fetch_add(1, std::memory_order_relaxed);
				if (bInputDelayPriming)
					Context->SourceDelayPrimingBlocks.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				Device.RejectedBlocks.fetch_add(1, std::memory_order_relaxed);
				if (!Fresh)
				{
					// The encoded bucket identifies the graph source slot and
					// ProbeReject reason for this full measurement window;
					// the helper also preserves IdentityRejectDetail as a
					// separate raw-mask bucket when identity failed.
					Device.RecordGraphSourceReject(static_cast<uint32>(Slot), ProbeReject, IdentityRejectDetail, Now);
				}
				else
				{
					// A fresh result that the renderer still refused used to leave no trace: W3.Pressure16x600 counted
					// rejected blocks while the detail buckets and the renderer failure counters stayed empty.
					Device.RendererFailedBlocks.fetch_add(1, std::memory_order_relaxed);
					Device.RenderFailures[size_t(Metrics.Failure)].fetch_add(1, std::memory_order_relaxed);
					Device.RecordGraphSourceReject(static_cast<uint32>(Slot), EIMAcousticProbeReject::RendererFailed, 0, Now);
				}
			}
			if (Fresh) ProbeReject = EIMAcousticProbeReject::RendererFailed;
			const float Gain = (Routes & 1u) ? 0.7071067811865475f * Context->DistanceGains[Slot].load(std::memory_order_relaxed) : 0.f;
			for (int32 I = 0; I < Frames; ++I)
			{
				const float Value = FMath::IsFinite(DelayedInputBlock[I]) ? DelayedInputBlock[I] * Gain : 0.f;
				Stereo[2 * I] = Value; Stereo[2 * I + 1] = Value;
			}
			if (!Fresh) Send->Zero();
		}
		else Device.RenderedBlocks.fetch_add(1, std::memory_order_relaxed);
		if (bCaptureMaximum) MaximumSample.AfterRenderCycles = FPlatformTime::Cycles64();
		double OutputEnergy = 0.0;
		bool OutputFinite = true;
		for (int32 I = 0; I < Frames; ++I)
		{
			Left->GetData()[I] = Stereo[2 * I]; Right->GetData()[I] = Stereo[2 * I + 1];
			const float L = Left->GetData()[I], R = Right->GetData()[I];
			OutputFinite = OutputFinite && FMath::IsFinite(L) && FMath::IsFinite(R);
			if (FMath::IsFinite(L)) OutputEnergy += double(L) * double(L);
			if (FMath::IsFinite(R)) OutputEnergy += double(R) * double(R);
		}
		if (IdentityProbe)
		{
			IdentityProbe->ActualAudioId.store(AudioId, std::memory_order_relaxed);
			IdentityProbe->ActualWorldGeneration.store(Context->Pool->WorldGeneration, std::memory_order_relaxed);
			IdentityProbe->ActualVoiceGeneration.store(Generation, std::memory_order_relaxed);
			IdentityProbe->ActualSpatialVersion.store(ExpectedSpatialVersion, std::memory_order_relaxed);
			IdentityProbe->FrozenListenerX.store(Context->IdentityInjection.ListenerX.load(std::memory_order_relaxed), std::memory_order_relaxed);
			IdentityProbe->FrozenListenerY.store(Context->IdentityInjection.ListenerY.load(std::memory_order_relaxed), std::memory_order_relaxed);
			IdentityProbe->FrozenListenerZ.store(Context->IdentityInjection.ListenerZ.load(std::memory_order_relaxed), std::memory_order_relaxed);
			IdentityProbe->RejectDetail.store(IdentityRejectDetail, std::memory_order_relaxed);
			const uint64 RenderedDelta = Device.RenderedBlocks.load(std::memory_order_acquire) - IdentityRenderedBefore;
			const uint64 RejectedDelta = Device.RejectedBlocks.load(std::memory_order_acquire) - IdentityRejectedBefore;
			IdentityProbe->RenderedDelta.store(RenderedDelta, std::memory_order_relaxed);
			IdentityProbe->RejectedDelta.store(RejectedDelta, std::memory_order_relaxed);
			IdentityProbe->FiniteOutput.store(OutputFinite ? 1u : 0u, std::memory_order_relaxed);
			IdentityProbe->Rejected.store(IdentityInjectionRejected && RejectedDelta == 1 && RenderedDelta == 0 ? 1u : 0u, std::memory_order_relaxed);
			IdentityProbe->Observed.store(1, std::memory_order_release);
		}
		Context->LastRejectDetail.store(IdentityRejectDetail, std::memory_order_release);
		if (Slot == 0)
		{
			IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::Spatial, Left->GetData(), Right->GetData(), Frames, Device.SampleRate);
			IMAcousticAuditionCapture::Spatial(Rendered, Context->DistanceCm[Slot].load(std::memory_order_relaxed),
				Latest.Frame.Direct.distanceAttenuation, Latest.Frame.Direct.airAbsorption, Latest.Frame.Direct.occlusion, static_cast<uint8>(ProbeReject));
		}
		// The graph is now the production source consumer. Publish the same
		// device-level observability used by the legacy adapter so existing
		// pressure/cross-floor gates measure actual graph blocks rather than
		// mistaking an empty legacy callback ring for silence.
		Device.PushDryCalls.fetch_add(1, std::memory_order_relaxed);
		if (InputEnergy > 1.e-12)
		{
			Device.PushDryInputNonzero.fetch_add(1, std::memory_order_relaxed);
			Device.RenderInputNonzero.fetch_add(1, std::memory_order_relaxed);
		}
		if (Rendered)
		{
			if ((Routes & 1u) && Metrics.DirectEnergy > 1.e-12)
				Device.DirectNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
			if ((Routes & 2u) && Metrics.PathEnergy > 1.e-12)
				Device.PathNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
		}
		Context->SourceBlocks.fetch_add(1, std::memory_order_relaxed);
		Context->SourceFrames.fetch_add(Frames, std::memory_order_relaxed);
		// PCM remains the ordinary source window used by the listening and
		// continuity checks.  The graph trace below deliberately covers every
		// bound slot so a multi-source run cannot look like a slot-zero run.
		if (Slot == 0)
		{
			const uint32 Offset = Context->CapturedSourceFrames.load(std::memory_order_relaxed);
			if (Offset + Frames <= uint32(Context->CapturedSource.Num()))
			{
				FMemory::Memcpy(Context->CapturedSource.GetData() + Offset, Mono->GetData(), Frames * sizeof(float));
				FMemory::Memcpy(Context->CapturedDry.GetData() + 2 * Offset, Stereo.GetData(), Frames * 2 * sizeof(float));
				Context->CapturedSourceFrames.store(Offset + Frames, std::memory_order_release);
			}
		}
		if (Context->CapturedSourceBlocks.Num() > 0)
		{
			const uint32 Block = Context->CapturedSourceBlockClaimCount.fetch_add(1, std::memory_order_relaxed);
			if (Context->CapturedSourceBlocks.IsValidIndex(Block))
			{
				FIMAcousticBlockProbe& Probe = Context->CapturedSourceBlocks[Block];
				Probe = {};
				Probe.Block = Block;
				Probe.Voice = uint32(Slot);
				Probe.InputSpatialVersion = InputSpatialVersion;
				Probe.OutputSpatialVersion = ExpectedSpatialVersion;
				Probe.InputDelayPriming = bInputDelayPriming ? 1 : 0;
				Probe.CallbackAudioComponentId = AudioId;
				Probe.ResultAudioComponentId = Latest.AudioComponentId;
				Probe.ResultWorldGeneration = Latest.WorldGeneration;
				Probe.ResultVoiceGeneration = Latest.Frame.Generation;
				Probe.ResultSpatialVersion = Latest.SpatialVersion;
				Probe.ResultSequence = Latest.Frame.Sequence;
				Probe.SnapshotCaptured = Latest.PublishedSeconds;
				Probe.ConsumedSeconds = Now;
				Probe.AgeMs = Latest.Frame.Sequence ? (Now - Latest.PublishedSeconds) * 1000.0 : 0.0;
				Probe.ListenerX = Latest.Frame.Listener.origin.x;
				Probe.ListenerY = Latest.Frame.Listener.origin.y;
				Probe.ListenerZ = Latest.Frame.Listener.origin.z;
				Probe.DirX = Latest.Frame.ListenerLocalDirection.x;
				Probe.DirY = Latest.Frame.ListenerLocalDirection.y;
				Probe.DirZ = Latest.Frame.ListenerLocalDirection.z;
				Probe.Occlusion = Latest.Frame.Direct.occlusion;
				Probe.DistanceGain = Latest.Frame.Direct.distanceAttenuation;
				Probe.Routes = Routes;
				Probe.DirectValid = Latest.Frame.DirectValid ? 1 : 0;
				Probe.PathValid = Latest.Frame.PathValid ? 1 : 0;
				Probe.Reject = ProbeReject;
				Probe.RenderFailure = uint8(Metrics.Failure);
				Probe.Fallback = Rendered ? 0 : 1;
				Probe.RejectDetail = IdentityRejectDetail;
				Probe.InputEnergy = InputEnergy;
				double ReverbSendEnergy = 0.0;
				for (int32 I = 0; I < Frames; ++I)
				{
					const double Sample = Send->GetData()[I];
					if (FMath::IsFinite(Sample)) ReverbSendEnergy += Sample * Sample;
				}
				Probe.ReverbSendEnergy = ReverbSendEnergy;
				Probe.DirectEnergy = Rendered ? Metrics.DirectEnergy : -1.0;
				Probe.PathEnergy = Rendered ? Metrics.PathEnergy : -1.0;
				Probe.OutputEnergy = OutputEnergy;
				Probe.RenderedAt = Device.RenderedBlocks.load(std::memory_order_relaxed);
				Probe.RejectedAt = Device.RejectedBlocks.load(std::memory_order_relaxed);
				Probe.CallbackDistanceCm = Context->DistanceCm[Slot].load(std::memory_order_relaxed);
				Probe.DegradedGain = (ProbeReject == EIMAcousticProbeReject::StaleResult
					|| ProbeReject == EIMAcousticProbeReject::MissingResult)
					? Context->DistanceGains[Slot].load(std::memory_order_relaxed) : 1.f;
				// Publish the completed POD only after every field is written;
				// the bitset lets concurrent operators expose a contiguous
				// prefix to GT without a lock or an audio-thread allocation.
				IMPublishCapturedSourceBlock(Context, Block);
				ObserveTransport(EIMAcousticTransportOrderPhase::SourceProbeLink, TransportGroup, bCompleteAfterThisBlock, -1, -1,
					EIMAcousticTransportOrderReason::None, Block);
			}
		}
	}
private:
	uint32 CaptureRendererWorkFlags(uint32 Routes) const
	{
		// Default-off returns before reading or computing any new snapshots.
		if (!Context || Context->CapturedTransportOrder.IsEmpty()) return 0;
		uint32 Flags = uint32(EIMAcousticTransportWorkFlag::Fresh);
		if (!HasAcceptedSourceResult) Flags |= uint32(EIMAcousticTransportWorkFlag::FirstAcceptedAttempt);
		if (Latest.Frame.DirectValid) Flags |= uint32(EIMAcousticTransportWorkFlag::DirectValid);
		if (Latest.Frame.PathValid) Flags |= uint32(EIMAcousticTransportWorkFlag::PathValid);
		if (Routes & 1u) Flags |= uint32(EIMAcousticTransportWorkFlag::DirectAudible);
		if (Routes & 2u) Flags |= uint32(EIMAcousticTransportWorkFlag::PathAudible);
		return Flags;
	}

	uint32 ObserveTransport(EIMAcousticTransportOrderPhase Phase, uint32 Group = MAX_uint32,
		bool FinishedBlock = false, int32 Returned = -1, int32 Active = -1,
		EIMAcousticTransportOrderReason Reason = EIMAcousticTransportOrderReason::None,
		uint32 SourceTraceIndex = MAX_uint32, uint64 WorkCycles = 0, uint32 WorkFlags = 0, uint64 ThreadCpuCycles = 0) const
	{
		if (!Context || Slot == INDEX_NONE || Context->CapturedTransportOrder.IsEmpty()) return MAX_uint32;
		FIMAcousticTransportOrderEvent Event;
		Event.Phase = Phase; Event.Group = Group; Event.Slot = Slot;
		Event.AudioId = AudioId; Event.SourceGeneration = Generation;
		Event.Frames = Frames; Event.Finished = FinishedBlock;
		Event.Returned = Returned; Event.Active = Active; Event.Reason = Reason;
		Event.SourceTraceIndex = SourceTraceIndex;
		Event.WorkCycles = WorkCycles; Event.WorkFlags = WorkFlags; Event.ThreadCpuCycles = ThreadCpuCycles;
		Event.Fresh = (WorkFlags & uint32(EIMAcousticTransportWorkFlag::Fresh)) != 0;
		Event.PushBlocks = Context->SourceSendBlockCounts[Slot].load(std::memory_order_relaxed);
		return IM_RecordAcousticTransportOrder(*Context, Event);
	}

	uint32 GetNonSpatialIdentityDetail(const FIMAcousticVoiceResult& Candidate) const
	{
		uint32 Detail = 0;
		if (!Candidate.WorldGeneration) Detail |= 16u;
		else if (!Context || !Context->Pool || Candidate.WorldGeneration != Context->Pool->WorldGeneration) Detail |= 32u;
		if (!Candidate.Frame.Generation || Candidate.Frame.Generation != Generation) Detail |= 64u;
		if (!Candidate.AudioComponentId || Candidate.AudioComponentId != AudioId) Detail |= 128u;
		return Detail;
	}

	void AlignResultToSpatialVersion(uint64 ExpectedSpatialVersion, uint32& RejectDetail)
	{
		if (!ExpectedSpatialVersion)
		{
			if (Latest.Frame.Sequence)
			{
				Latest = {};
				RejectDetail |= 2048u;
			}
			return;
		}

		if (Future.Frame.Sequence && Future.SpatialVersion == ExpectedSpatialVersion)
		{
			if (Future.Frame.Sequence > Latest.Frame.Sequence) Latest = Future;
			Future = {};
		}
		else if (Future.Frame.Sequence && Future.SpatialVersion < ExpectedSpatialVersion)
		{
			Future = {};
			if (!Latest.Frame.Sequence || Latest.SpatialVersion != ExpectedSpatialVersion)
				RejectDetail |= 2048u;
		}

		if (Latest.Frame.Sequence && Latest.SpatialVersion != ExpectedSpatialVersion)
		{
			Latest = {};
			RejectDetail |= 2048u;
		}
	}

	void AcceptResult(const FIMAcousticVoiceResult& Candidate, uint64 ExpectedSpatialVersion,
		bool bTestInjection, uint32& RejectDetail, bool& InjectionRejected)
	{
		uint32 Detail = GetNonSpatialIdentityDetail(Candidate);
		const bool bSpatialMismatch = !ExpectedSpatialVersion
			|| !Candidate.SpatialVersion || Candidate.SpatialVersion != ExpectedSpatialVersion;
		if (bTestInjection && bSpatialMismatch) Detail |= 2048u;
		if (Detail)
		{
			Latest = {};
			Future = {};
			RejectDetail |= Detail;
			if (bTestInjection) InjectionRejected = true;
			return;
		}

		if (Candidate.Frame.Sequence > Latest.Frame.Sequence
			&& Candidate.SpatialVersion == ExpectedSpatialVersion)
		{
			Latest = Candidate;
			if (Future.Frame.Sequence && Future.SpatialVersion == ExpectedSpatialVersion) Future = {};
			return;
		}

		if (Candidate.SpatialVersion > ExpectedSpatialVersion)
		{
			// Keep the nearest future pose so one in-flight worker result can
			// wait for its source block to reach the output side of the delay.
			if (!Future.Frame.Sequence || Candidate.SpatialVersion < Future.SpatialVersion
				|| (Candidate.SpatialVersion == Future.SpatialVersion
					&& Candidate.Frame.Sequence > Future.Frame.Sequence))
			{
				Future = Candidate;
			}
			return;
		}

		if (!Latest.Frame.Sequence || Latest.SpatialVersion != ExpectedSpatialVersion)
			RejectDetail |= 2048u;
	}

	FAudioBufferReadRef Mono;
	FInt32ReadRef Voice;
	FTriggerReadRef Finished;
	FAudioBufferWriteRef Left, Right, Send;
	FIMAcousticMetaSoundContextPtr Context;
	FIMAcousticAudioRenderer Renderer;
	TArray<float> Stereo;
	std::array<float, FIMAcousticMetaSoundContext::SourceInputDelayFrames> DelayedMono{};
	std::array<float, FIMAcousticMetaSoundContext::Frames> DelayedInputBlock{};
	std::array<uint64, FIMAcousticMetaSoundContext::SourceInputDelayBlocks> DelayedSpatialVersions{};
	FIMAcousticVoiceResult Latest, Future;
	int32 Frames, Slot = INDEX_NONE;
	uint64 AudioId = 0, Generation = 0;
	uint32 DelayWriteIndex = 0, DelayBlocksWritten = 0;
	bool Ready = false, HasAcceptedSourceResult = false, bRendererCounted = false;
	bool bSourceSendCompleted = false;
};

class FIMAcousticEnvironmentOperator final : public TExecutableOperator<FIMAcousticEnvironmentOperator>
{
public:
	static const FVertexInterface& GetVertexInterface()
	{
		using namespace IMAcousticMetaSoundNodesPrivate;
		static const FVertexInterface Interface(
			FInputVertexInterface(TInputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Mono))),
			FOutputVertexInterface(TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Left)),
				TOutputDataVertex<FAudioBuffer>(METASOUND_GET_PARAM_NAME_AND_METADATA(Right))));
		return Interface;
	}
	static const FNodeClassMetadata& GetNodeInfo()
	{
		static const FNodeClassMetadata Info = []
		{
			FNodeClassMetadata M;
			M.ClassName = {TEXT("IM"), TEXT("Acoustic Environment"), TEXT("Stereo")};
			M.MajorVersion = 1; M.MinorVersion = 0;
			M.DisplayName = LOCTEXT("EnvironmentTitle", "Acoustic Environment");
			M.Description = LOCTEXT("EnvironmentDescription", "唯一环境 IR 消费者：卷积与双耳解码在图内执行。");
			M.Author = TEXT("IceMoon"); M.DefaultInterface = GetVertexInterface();
			return M;
		}();
		return Info;
	}
	static TUniquePtr<IOperator> CreateOperator(const FBuildOperatorParams& P, FBuildResults&)
	{
		return MakeUnique<FIMAcousticEnvironmentOperator>(P,
			P.InputData.GetOrCreateDefaultDataReadReference<FAudioBuffer>(TEXT("Mono"), P.OperatorSettings));
	}
	FIMAcousticEnvironmentOperator(const FBuildOperatorParams& P, FAudioBufferReadRef InMono)
		: Mono(InMono), Left(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)),
			Right(FAudioBufferWriteRef::CreateNew(P.OperatorSettings)), Context(ContextFromBuild(P)),
			Frames(P.OperatorSettings.GetNumFramesPerBlock())
	{
		Reset(P);
	}
	void Reset(const IOperator::FResetParams& P)
	{
		ReleaseEnvironment();
		Context = ContextFromBuild(P);
		Frames = P.OperatorSettings.GetNumFramesPerBlock();
		Stereo.SetNumZeroed(Frames * 2);
		if (!Context || Frames != int32(FIMAcousticMetaSoundContext::Frames)
			|| P.OperatorSettings.GetSampleRate() != Context->Device->SampleRate) return;
		bool Expected = false;
		Owned = Context->EnvironmentConsumer.compare_exchange_strong(Expected, true, std::memory_order_acq_rel);
		if (!Owned) { Context->DuplicateConsumers.fetch_add(1, std::memory_order_relaxed); return; }
		// Match the source's test-only construction observation without changing
		// HRTF/IR ownership, processing order, or the ordinary audio path.
		const bool bObserveBuild = !Context->CapturedBlocks.IsEmpty();
		const double BuildStartSeconds = bObserveBuild ? FPlatformTime::Seconds() : 0.0;
		IPLHRTF HRTF = CreateOperatorHRTF(P.OperatorSettings);
		const double HRTFReadySeconds = bObserveBuild ? FPlatformTime::Seconds() : 0.0;
		if (HRTF)
		{
			Ready = Renderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTF, int(P.OperatorSettings.GetSampleRate()), Frames,
				int(P.OperatorSettings.GetSampleRate() * IMAcousticRecipe::ReverbSavedDurationS));
			iplHRTFRelease(&HRTF);
		}
		WetHighPass.Configure(P.OperatorSettings.GetSampleRate());
		Context->Device->ReverbEffectInstances.fetch_add(1, std::memory_order_relaxed);
		if (bObserveBuild)
		{
			const double BuildEndSeconds = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticOperatorBuild kind=environment device=%u begin_s=%.9f end_s=%.9f hrtf_ms=%.6f renderer_ms=%.6f ready=%d game_thread=%d"),
				Context->DeviceId, BuildStartSeconds, BuildEndSeconds,
				(HRTFReadySeconds - BuildStartSeconds) * 1000.0,
				(BuildEndSeconds - HRTFReadySeconds) * 1000.0, Ready ? 1 : 0, IsInGameThread() ? 1 : 0);
		}
	}
	~FIMAcousticEnvironmentOperator() override { ReleaseEnvironment(); }
	void ReleaseEnvironment()
	{
		Renderer.Shutdown(); ReleaseCurrent(); ReleaseFuture();
		DelayedSpatialVersions.fill(0);
		DelayWriteIndex = 0;
		DelayBlocksWritten = 0;
		if (Owned)
		{
			Context->Device->ReverbEffectInstances.fetch_sub(1, std::memory_order_relaxed);
			Context->EnvironmentConsumer.store(false, std::memory_order_release);
		}
		Owned = false; Ready = false;
	}
	void BindInputs(FInputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Mono"), Mono); }
	void BindOutputs(FOutputVertexInterfaceData& D) override { D.BindReadVertex(TEXT("Left"), Left); D.BindReadVertex(TEXT("Right"), Right); }
	void Execute()
	{
		const uint64 ExecuteCandidateStart = Context && Context->Device
			&& Context->Device->ProfilingEnabled.load(std::memory_order_relaxed)
			? FPlatformTime::Cycles64() : 0;
		const bool bMaximumWindow = Context && Context->Device
			&& Context->CaptureExecuteMaximums.load(std::memory_order_relaxed);
		const bool bPressureScope = bMaximumWindow
			&& Context->Device->TryBeginPressureWindowPublication();
		ON_SCOPE_EXIT
		{
			if (bPressureScope) Context->Device->EndPressureWindowPublication();
		};
		const uint64 ExecuteStart = (!bMaximumWindow || bPressureScope) ? ExecuteCandidateStart : 0;
		const uint64 ExecuteStartCpu = ExecuteStart && Context ? Context->BlockCosts.BeginCpuCycles() : 0;
		Left->Zero(); Right->Zero();
		if (!Context || !Owned) return;
		if (Context->Stopped.load(std::memory_order_acquire) || Context->Pool->Stopped.load(std::memory_order_acquire))
		{ Renderer.Reset(); ReleaseCurrent(); ReleaseFuture(); return; }
		if (!Ready || Mono->Num() != Frames || Frames != int32(FIMAcousticMetaSoundContext::Frames))
		{ Context->InvalidBlocks.fetch_add(1, std::memory_order_relaxed); return; }
		auto& Device = *Context->Device;
		const bool bCaptureMaximum = ExecuteStart
			&& Context->CaptureExecuteMaximums.load(std::memory_order_relaxed);
		FIMAcousticExecuteMaximumSample MaximumSample;
		if (bCaptureMaximum)
		{
			MaximumSample.StartCycles = ExecuteStart;
			MaximumSample.ThreadId = FPlatformTLS::GetCurrentThreadId();
			MaximumSample.WorldGeneration = Context->Pool->WorldGeneration;
		}
		// Match the bound source budget: include output clearing and the complete
		// environment execution, including identity/PCM capture after the DSP.
		ON_SCOPE_EXIT
		{
			if (ExecuteStart)
			{
				const uint64 Cycles = FPlatformTime::Cycles64() - ExecuteStart;
				Device.ReverbTiming.RecordCycles(Cycles);
				if (bCaptureMaximum)
				{
					MaximumSample.WholeCycles = Cycles;
					Context->EnvironmentExecuteMaximum.RecordMaximum(MaximumSample);
				}
				Context->BlockCosts.Record(Context->BlockCosts.EnvironmentParticipant(), ExecuteStart, Cycles, ExecuteStartCpu);
			}
		};
		const double Now = FPlatformTime::Seconds();
		const uint64 InputSpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
		const bool bDelayPriming = DelayBlocksWritten < FIMAcousticMetaSoundContext::SourceInputDelayBlocks;
		const uint64 ExpectedSpatialVersion = bDelayPriming ? 0 : DelayedSpatialVersions[DelayWriteIndex];
		DelayedSpatialVersions[DelayWriteIndex] = InputSpatialVersion;
		DelayWriteIndex = (DelayWriteIndex + 1) % FIMAcousticMetaSoundContext::SourceInputDelayBlocks;
		if (DelayBlocksWritten < FIMAcousticMetaSoundContext::SourceInputDelayBlocks) ++DelayBlocksWritten;
		FIMAcousticReverbSlot* Retired = SelectReverbForVersion(ExpectedSpatialVersion, Now);
		const bool SpatialFresh = Current && ExpectedSpatialVersion && Current->SpatialVersion
			&& Current->SpatialVersion == ExpectedSpatialVersion;
		// One immutable decision tuple feeds both the production predicate and
		// its optional trace; reloading after ReleaseCurrent loses the evidence.
		const uint64 DeviceWorldGeneration = Device.WorldGeneration.load(std::memory_order_acquire);
		const bool bDeviceEnabled = Device.Enabled.load(std::memory_order_acquire);
		const uint32 RenderRoutes = Device.RenderRoutes.load(std::memory_order_relaxed);
		const bool Fresh = Current && SpatialFresh && Now >= Current->CapturedSeconds && Now - Current->CapturedSeconds <= .25
			&& DeviceWorldGeneration == Context->Pool->WorldGeneration && bDeviceEnabled && (RenderRoutes & 4u);
		const uint32 TraceBlock = Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_relaxed);
		FIMAcousticMetaSoundBlock* Trace = Context->CapturedBlocks.IsValidIndex(int32(TraceBlock))
			? &Context->CapturedBlocks[int32(TraceBlock)] : nullptr;
		if (Trace)
		{
			*Trace = FIMAcousticMetaSoundBlock{};
			Trace->Frame = uint64(TraceBlock) * Frames;
			Trace->Seconds = Now; Trace->Fresh = Fresh;
			Trace->InputSpatialVersion = InputSpatialVersion;
			Trace->ExpectedSpatialVersion = ExpectedSpatialVersion;
			Trace->DeviceWorldGeneration = DeviceWorldGeneration;
			Trace->PoolWorldGeneration = Context->Pool->WorldGeneration;
			Trace->Enabled = bDeviceEnabled; Trace->RenderRoutes = RenderRoutes;
			Trace->HasCurrent = Current != nullptr; Trace->DelayPriming = bDelayPriming;
			if (Current)
			{
				Trace->CapturedSeconds = Current->CapturedSeconds;
				Trace->Sequence = Current->Sequence; Trace->SpatialVersion = Current->SpatialVersion;
				Trace->ListenerX = Current->Listener.origin.x;
				Trace->ListenerY = Current->Listener.origin.y;
				Trace->ListenerZ = Current->Listener.origin.z;
			}
		}
		FIMAcousticReverbMetrics Metrics;
		if (bCaptureMaximum)
		{
			MaximumSample.BeginSeconds = Now;
			MaximumSample.SpatialVersion = ExpectedSpatialVersion;
			MaximumSample.Sequence = Current ? Current->Sequence : 0;
			MaximumSample.Fresh = Fresh ? 1u : 0u;
			MaximumSample.BeforeRenderCycles = FPlatformTime::Cycles64();
		}
		const bool bCanHoldIR = Current && Current->Applied && !bDelayPriming
			&& DeviceWorldGeneration == Context->Pool->WorldGeneration && bDeviceEnabled && (RenderRoutes & 4u);
		const bool bRenderAttempted = Fresh || bCanHoldIR;
		if (!Fresh && bCanHoldIR)
		{
			Device.ReverbNotFreshBlocks.fetch_add(1, std::memory_order_relaxed);
			Context->NoIRBlocks.fetch_add(1, std::memory_order_relaxed);
		}
		const bool bRendered = bRenderAttempted && Renderer.Render(Mono->GetData(), Frames, Current->Params, Current->Listener, Stereo.GetData(), &Metrics);
		if (bRendered)
		{
			Current->Applied = true;
			Context->LastIRSequence.store(Current->Sequence, std::memory_order_release);
			Device.ReverbProcessedBlocks.fetch_add(1, std::memory_order_relaxed);
			const float Gain = (Device.RenderRoutes.load(std::memory_order_relaxed) & 4u) ? Context->WetGain.load(std::memory_order_relaxed) : 0.f;
			const float TargetContinuityGain = Fresh ? 1.f : .65f;
			double Energy = 0;
			// Presence widening is mid/side on the wet output only. 1 reproduces L/R exactly.
			const float SideGain = Context->PresenceWetSideGain.load(std::memory_order_relaxed);
			const bool bWiden = FMath::IsFinite(SideGain) && SideGain != 1.f;
			for (int32 I = 0; I < Frames; ++I)
			{
				ContinuityGain += FMath::Clamp(TargetContinuityGain - ContinuityGain,
					-1.f / (.1f * Device.SampleRate), 1.f / (.1f * Device.SampleRate));
				float WetLeft = WetHighPass.Process(0, Stereo[2 * I]), WetRight = WetHighPass.Process(1, Stereo[2 * I + 1]);
				if (bWiden)
				{
					const float Mid = .5f * (WetLeft + WetRight), Side = .5f * (WetLeft - WetRight) * SideGain;
					WetLeft = Mid + Side;
					WetRight = Mid - Side;
				}
				Left->GetData()[I] = WetLeft * Gain * ContinuityGain;
				Right->GetData()[I] = WetRight * Gain * ContinuityGain;
				Energy += double(Left->GetData()[I]) * Left->GetData()[I] + double(Right->GetData()[I]) * Right->GetData()[I];
			}
			if (Metrics.InputEnergy > 0)
			{
				Device.ReverbDryNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
				Device.ReverbDryBlocks.fetch_add(1, std::memory_order_relaxed);
			}
			if (Metrics.AmbisonicsEnergy > 0) Device.ReverbRawNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
			if (Energy > 0) Device.ReverbNonzeroBlocks.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			if (!Fresh) Device.ReverbNotFreshBlocks.fetch_add(1, std::memory_order_relaxed);
			else Device.ReverbRejectedBlocks.fetch_add(1, std::memory_order_relaxed);
			Renderer.Reset();
			WetHighPass.Reset();
			const bool bWetBypassed = !Device.Enabled.load(std::memory_order_acquire)
				|| !(Device.RenderRoutes.load(std::memory_order_relaxed) & 4u)
				|| DeviceWorldGeneration != Context->Pool->WorldGeneration;
			if (bWetBypassed)
			{
				ReleaseCurrent();
				ContinuityGain = 0.f;
				ReleaseFuture();
			}
			Context->NoIRBlocks.fetch_add(1, std::memory_order_relaxed);
		}
		// Retire only after Apply, preserving SDK's mutable triple-buffer lease.
		if (bCaptureMaximum) MaximumSample.AfterRenderCycles = FPlatformTime::Cycles64();
		if (Retired) Retired->State.store(EIMAcousticIRState::Free, std::memory_order_release);
		IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage::Wet, Left->GetData(), Right->GetData(), Frames, Device.SampleRate);
		Context->EnvironmentBlocks.fetch_add(1, std::memory_order_relaxed);
		Context->EnvironmentFrames.fetch_add(Frames, std::memory_order_relaxed);
		if (Trace)
		{
			Trace->RenderAttempted = bRenderAttempted; Trace->Rendered = bRendered;
			Context->CapturedEnvironmentTraceBlockCount.store(TraceBlock + 1, std::memory_order_release);
		}

		// The environment identity trace can outlive the bounded PCM window used
		// by short listening tests. Keep the two capture limits independent.
		const uint32 PcmOffset = Context->CapturedEnvironmentFrames.load(std::memory_order_relaxed);
		const uint32 PcmBlock = Context->CapturedBlockCount.load(std::memory_order_relaxed);
		const uint64 PcmFrameEnd = uint64(PcmOffset) + Frames;
		const uint64 PcmStereoSampleEnd = PcmFrameEnd * 2u;
		if (PcmFrameEnd <= uint32(Context->CapturedBus.Num())
			&& PcmStereoSampleEnd <= uint32(Context->CapturedWet.Num()))
		{
			FMemory::Memcpy(Context->CapturedBus.GetData() + PcmOffset, Mono->GetData(), Frames * sizeof(float));
			for (int32 I = 0; I < Frames; ++I)
			{
				Context->CapturedWet[2 * (PcmOffset + I)] = Left->GetData()[I];
				Context->CapturedWet[2 * (PcmOffset + I) + 1] = Right->GetData()[I];
			}
			Context->CapturedEnvironmentFrames.store(uint32(PcmFrameEnd), std::memory_order_release);
			Context->CapturedBlockCount.store(PcmBlock + 1, std::memory_order_release);
		}
	}
private:
	void ReleaseSlot(FIMAcousticReverbSlot*& Slot)
	{
		if (Slot)
		{
			Slot->State.store(EIMAcousticIRState::Free, std::memory_order_release);
			Slot = nullptr;
		}
	}
	void ReleaseCurrent() { ReleaseSlot(Current); }
	void ReleaseFuture() { ReleaseSlot(Future); }
	FIMAcousticReverbSlot* SelectReverbForVersion(uint64 ExpectedSpatialVersion, double Now)
	{
		FIMAcousticReverbSlot* Next = nullptr;
		if (Future && (Now < Future->CapturedSeconds || Now - Future->CapturedSeconds > .25
			|| (ExpectedSpatialVersion && Future->SpatialVersion < ExpectedSpatialVersion)))
		{
			ReleaseFuture();
		}
		if (Future && ExpectedSpatialVersion && Future->SpatialVersion == ExpectedSpatialVersion)
		{
			if (!Current || Current->SpatialVersion != ExpectedSpatialVersion
				|| Future->Sequence > Current->Sequence)
			{
				Next = Future;
				Future = nullptr;
			}
			else
			{
				ReleaseFuture();
			}
		}

		for (auto& Candidate : Context->Pool->Slots)
		{
			auto ExpectedState = EIMAcousticIRState::Ready;
			if (!Candidate.State.compare_exchange_strong(ExpectedState, EIMAcousticIRState::Reading,
				std::memory_order_acq_rel))
			{
				continue;
			}

			const bool CandidateExpired = Now < Candidate.CapturedSeconds
				|| Now - Candidate.CapturedSeconds > .25;
			if (CandidateExpired || !Candidate.SpatialVersion
				|| (ExpectedSpatialVersion && Candidate.SpatialVersion < ExpectedSpatialVersion))
			{
				Candidate.State.store(EIMAcousticIRState::Free, std::memory_order_release);
				continue;
			}

			if (ExpectedSpatialVersion && Candidate.SpatialVersion == ExpectedSpatialVersion)
			{
				if (!Next && Current && Current->SpatialVersion == ExpectedSpatialVersion
					&& Candidate.Sequence <= Current->Sequence)
				{
					Candidate.State.store(EIMAcousticIRState::Free, std::memory_order_release);
					continue;
				}
				if (Next && Candidate.Sequence <= Next->Sequence)
				{
					Candidate.State.store(EIMAcousticIRState::Free, std::memory_order_release);
					continue;
				}
				if (Next) Next->State.store(EIMAcousticIRState::Free, std::memory_order_release);
				Next = &Candidate;
				continue;
			}

			// Keep only the nearest future pose. A newer listener result must
			// not evict the IR still paired with source PCM in the delay line.
			if (Current && Current->SpatialVersion == Candidate.SpatialVersion
				&& Candidate.Sequence <= Current->Sequence)
			{
				Candidate.State.store(EIMAcousticIRState::Free, std::memory_order_release);
				continue;
			}
			if (!Future || Candidate.SpatialVersion < Future->SpatialVersion
				|| (Candidate.SpatialVersion == Future->SpatialVersion
					&& Candidate.Sequence > Future->Sequence))
			{
				ReleaseFuture();
				Future = &Candidate;
			}
			else
			{
				Candidate.State.store(EIMAcousticIRState::Free, std::memory_order_release);
			}
		}

		if (!Next) return nullptr;
		FIMAcousticReverbSlot* Retired = Current;
		Current = Next;
		return Retired;
	}
	FAudioBufferReadRef Mono;
	FAudioBufferWriteRef Left, Right;
	FIMAcousticMetaSoundContextPtr Context;
	FIMAcousticReverbRenderer Renderer;
	TArray<float> Stereo;
	std::array<uint64, FIMAcousticMetaSoundContext::SourceInputDelayBlocks> DelayedSpatialVersions{};
	FIMAcousticReverbSlot* Current = nullptr;
	FIMAcousticReverbSlot* Future = nullptr;
	int32 Frames;
		float ContinuityGain = 1.f;
	FIMWetHighPass WetHighPass;
	uint32 DelayWriteIndex = 0, DelayBlocksWritten = 0;
	bool Owned = false, Ready = false;
};

using FIMAcousticSourceNode = TNodeFacade<FIMAcousticSourceOperator>;
using FIMAcousticEnvironmentNode = TNodeFacade<FIMAcousticEnvironmentOperator>;
METASOUND_REGISTER_NODE(FIMAcousticSourceNode)
METASOUND_REGISTER_NODE(FIMAcousticEnvironmentNode)
}
#undef LOCTEXT_NAMESPACE

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticAudioRenderer.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticCoordinates.h"
#include "IMAcousticSimulation.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticMetaSoundBoundaries, "IceMoon.AcousticField.MetaSound.OperatorBoundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticMetaSoundBoundaries::RunTest(const FString&)
{
	using namespace Metasound;
	constexpr uint32 TestDevice = MAX_uint32 - 1;
	auto Context = IMAcousticMetaSound::CreateAcousticMetaSoundContext(TestDevice, 48000, 123);
	if (!TestNotNull(TEXT("Isolated operator context"), Context.Get())) return false;
	Context->Device->WorldGeneration.store(123);
	Context->Device->Enabled.store(true);
	Context->Device->RenderRoutes.store(7);
	const int32 Slot = IMAcousticMetaSound::RegisterAcousticMetaSoundSource(Context, 42);
	TestEqual(TEXT("First voice slot"), Slot, 0);
	Context->SendGains[0].store(1.f); Context->ReverbSendGains[0].store(1.f); Context->DistanceGains[0].store(.5f);
	Context->CapturedSource.SetNumZeroed(3 * 512);
	Context->CapturedDry.SetNumZeroed(3 * 1024);
	Context->CapturedWet.SetNumZeroed(4096);
	Context->CapturedBus.SetNumZeroed(2048);
	Context->CapturedBlocks.SetNumZeroed(4);
	const FOperatorSettings Settings(48000, 93.75f);
	FMetasoundEnvironment Environment;
	Environment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
	::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceNode Node(TEXT("BoundarySource"), FGuid::NewGuid());
	::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentNode EnvNode(TEXT("BoundaryEnvironment"), FGuid::NewGuid());
	FInputVertexInterfaceData Inputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetInputInterface());
	FInputVertexInterfaceData EnvInputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentOperator::GetVertexInterface().GetInputInterface());
	auto Mono = FAudioBufferWriteRef::CreateNew(Settings);
	for (int32 I = 0; I < Mono->Num(); ++I) Mono->GetData()[I] = 1.f;
	auto Voice = FInt32WriteRef::CreateNew(0);
	const FBuildOperatorParams Params(Node, Settings, Inputs, Environment);
	const FBuildOperatorParams EnvParams(EnvNode, Settings, EnvInputs, Environment);
	{
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator Source(Params, Mono, Voice, FTriggerReadRef::CreateNew(Params.OperatorSettings));
		Source.Execute();
		Source.Execute();
		TestEqual(TEXT("Two source blocks prime the fixed lookahead"),
			Context->SourceDelayPrimingBlocks.load(), uint64(FIMAcousticMetaSoundContext::SourceInputDelayBlocks));
		Source.Execute();
		TestEqual(TEXT("The source output begins after the fixed lookahead"), Context->SourceFrames.load(), uint64(3 * 512));
		TestEqual(TEXT("The two priming output blocks are silent"), Context->CapturedDry[0], 0.0f);
		TestTrue(TEXT("No IR preserves route-aware distance-scaled fallback after the lookahead"),
			FMath::IsNearlyEqual(Context->CapturedDry[2 * 512 * 2], .35355339f, 1.e-6f));
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator DuplicateSource(Params, Mono, Voice, FTriggerReadRef::CreateNew(Params.OperatorSettings));
		DuplicateSource.Execute();
		TestEqual(TEXT("Duplicate source consumer rejected"), Context->DuplicateConsumers.load(), uint64(1));
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentOperator Wet(EnvParams, Mono);
		Wet.Execute();
		TestEqual(TEXT("No-IR environment explicitly degrades"), Context->NoIRBlocks.load(), uint64(1));
		TestEqual(TEXT("No-IR wet is silent"), Context->CapturedWet[0], 0.f);
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentOperator DuplicateWet(EnvParams, Mono);
		DuplicateWet.Execute();
		TestEqual(TEXT("Duplicate environment consumer rejected"), Context->DuplicateConsumers.load(), uint64(2));
		auto& InvalidIR = Context->Pool->Slots[0];
		InvalidIR.Sequence = 1; InvalidIR.CapturedSeconds = FPlatformTime::Seconds();
		InvalidIR.State.store(EIMAcousticIRState::Ready, std::memory_order_release);
		Wet.Execute();
		TestTrue(TEXT("Invalid IR lease released without acknowledgement"), InvalidIR.State.load() == EIMAcousticIRState::Free && !InvalidIR.Applied);
		const FOperatorSettings BadSettings(48000, 100.f);
		const FBuildOperatorParams BadParams(Node, BadSettings, Inputs, Environment);
		auto BadMono = FAudioBufferWriteRef::CreateNew(BadSettings);
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator BadBlock(BadParams, BadMono, Voice, FTriggerReadRef::CreateNew(BadParams.OperatorSettings));
		BadBlock.Execute();
		TestEqual(TEXT("480-frame graph rejects incompatible 512-frame SDK contract"), Context->InvalidBlocks.load(), uint64(1));
	}
	TestEqual(TEXT("Environment resources returned"), Context->Device->ReverbEffectInstances.load(), int32(0));
	TestFalse(TEXT("Environment lease returned"), Context->EnvironmentConsumer.load());
	TestFalse(TEXT("Source lease returned"), Context->SourceConsumers[0].load());
	IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound operator boundaries"), HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSound operator boundaries %s"), HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticSourceSendCompletion,
	"IceMoon.AcousticField.MetaSound.SourceSendCompletion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticSourceSendCompletion::RunTest(const FString&)
{
	using namespace Metasound;
	constexpr uint32 TestDevice = MAX_uint32 - 4;
	TSharedPtr<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe> Context =
		IMAcousticMetaSound::CreateAcousticMetaSoundContext(TestDevice, 48000, 125);
	if (!TestNotNull(TEXT("Isolated completion-test context"), Context.Get())) return false;

	bool bContextStopped = false;
	ON_SCOPE_EXIT
	{
		if (Context.IsValid() && !bContextStopped)
			IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
	};

	Context->Device->Enabled.store(true);
	Context->Device->RenderRoutes.store(7);
	Context->SourceSendTransportEnabled = true;
	// Four produced blocks leave headroom, so the completion assertion cannot be
	// confused with a capacity short write.
	Context->SourceSendCapacityFrames = int32(FIMAcousticMetaSoundContext::Frames * 8);

	const int32 Slot = IMAcousticMetaSound::RegisterAcousticMetaSoundSource(Context, 43);
	if (!TestTrue(TEXT("Completion-test voice is registered"), Slot == 0)) return false;
	Context->SendGains[Slot].store(1.f);
	Context->ReverbSendGains[Slot].store(1.f);
	Context->DistanceGains[Slot].store(.5f);

	const FOperatorSettings Settings(48000, 93.75f);
	FMetasoundEnvironment Environment;
	Environment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
	::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceNode Node(TEXT("SourceSendCompletion"), FGuid::NewGuid());
	FInputVertexInterfaceData Inputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetInputInterface());
	auto Mono = FAudioBufferWriteRef::CreateNew(Settings);
	for (int32 Frame = 0; Frame < Mono->Num(); ++Frame)
		Mono->GetData()[Frame] = 1.f;
	auto Voice = FInt32WriteRef::CreateNew(Slot);
	const FBuildOperatorParams Params(Node, Settings, Inputs, Environment);
	FTriggerWriteRef FinishedWrite = FTriggerWriteRef::CreateNew(Settings);
	const FTriggerReadRef FinishedRead(FinishedWrite);

	const int32 Frames = int32(FIMAcousticMetaSoundContext::Frames);
	auto* Producer = Context->SourceSendInputs[Slot].Get();
	if (!TestNotNull(TEXT("Context-owned source producer remains live"), Producer)) return false;
	TArray<float> SentinelPrefix;
	SentinelPrefix.SetNumUninitialized(Frames);
	for (int32 Frame = 0; Frame < Frames; ++Frame)
		SentinelPrefix[Frame] = 2.f + float(Frame) / float(Frames);
	if (!TestEqual(TEXT("Distinctive non-zero PCM sentinel enters the queue before the source blocks"),
		Producer->PushAudio(SentinelPrefix.GetData(), Frames), Frames)) return false;

	{
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator Source(Params, Mono, Voice, FinishedRead);
		FOutputVertexInterfaceData Outputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetOutputInterface());
		Source.BindOutputs(Outputs);
		const FAudioBufferReadRef Send = Outputs.GetDataReadReference<FAudioBuffer>(TEXT("Send"));

		// A live source with Finished unset establishes the active control.
		Source.Execute();
		TestFalse(TEXT("Finished input starts untriggered"), FinishedRead->IsTriggeredInBlock());
		TestTrue(TEXT("Successful non-final Source block keeps the source send active"),
			Context->SourceSendActive[Slot].load(std::memory_order_acquire));
		TestEqual(TEXT("Non-final control queues one complete PCM block"),
			Context->SourceSendBlockCounts[Slot].load(std::memory_order_acquire), uint64(1));
		TestEqual(TEXT("Non-final control has no push failure"),
			Context->SourceSendPushFailures.load(std::memory_order_acquire), uint64(0));

		// Fill to three complete blocks, trigger completion for the fourth, and
		// compare the consumer's final block against the operator's emitted PCM.
		Source.Execute();
		Source.Execute();
		FinishedWrite->TriggerFrame(0);
		Source.Execute();
		// This source block may be silent during priming or without a fresh spatial
		// frame; the distinct non-zero sentinel below independently checks PCM fidelity.
		TArray<float> ExpectedFinalPcm;
		ExpectedFinalPcm.Append(Send->GetData(), Frames);

		TestEqual(TEXT("Completion enqueues its final complete source block"),
			Context->SourceSendBlockCounts[Slot].load(std::memory_order_acquire), uint64(4));
		TestEqual(TEXT("Completion does not count a failed enqueue"),
			Context->SourceSendPushFailures.load(std::memory_order_acquire), uint64(0));
		TestFalse(TEXT("Final enqueue marks the source send inactive before destruction"),
			Context->SourceSendActive[Slot].load(std::memory_order_acquire));
		TestTrue(TEXT("Completion retains the source consumer lease until destruction"),
			Context->SourceConsumers[Slot].load(std::memory_order_acquire));
		TestEqual(TEXT("Completion retains the renderer lease until destruction"),
			Context->Device->SourceRendererInstances.load(std::memory_order_acquire), int32(1));

		// Direct Execute calls do not run MetaSound's post-execute trigger advance.
		// Advance explicitly so the next call proves the operator's local completion latch.
		FinishedWrite->AdvanceBlock();
		TestFalse(TEXT("Finished event has advanced beyond the completion block"),
			FinishedRead->IsTriggeredInBlock());

		auto* Patch = Context->SourceSendPatches[Slot].Get();
		if (TestNotNull(TEXT("Context retains the source-send consumer patch"), Patch))
		{
			TestEqual(TEXT("Completion leaves its four source blocks plus the PCM sentinel queued"),
				Patch->GetNumSamplesAvailable(), 5 * Frames);

			const uint64 PushBlocksBeforePostFinishExecute = Context->SourceSendBlockCounts[Slot].load();
			const uint64 PushFailuresBeforePostFinishExecute = Context->SourceSendPushFailures.load();
			Source.Execute();
			TestEqual(TEXT("Post-finish Execute does not push another PCM block"),
				Context->SourceSendBlockCounts[Slot].load(), PushBlocksBeforePostFinishExecute);
			TestEqual(TEXT("Post-finish Execute does not introduce a push failure"),
				Context->SourceSendPushFailures.load(), PushFailuresBeforePostFinishExecute);
			TestEqual(TEXT("Post-finish Execute preserves the queued PCM tail"),
				Patch->GetNumSamplesAvailable(), 5 * Frames);

			TArray<float> RetainedSentinel;
			RetainedSentinel.SetNumUninitialized(Frames);
			if (TestEqual(TEXT("Non-zero PCM sentinel remains a complete queue prefix"),
				Patch->PopAudio(RetainedSentinel.GetData(), RetainedSentinel.Num(), false), Frames))
			{
				bool bSentinelMatches = true;
				for (int32 Frame = 0; Frame < Frames; ++Frame)
					if (RetainedSentinel[Frame] != SentinelPrefix[Frame])
						bSentinelMatches = false;
				TestTrue(TEXT("Every non-zero sentinel PCM sample is unchanged"), bSentinelMatches);
			}

			TArray<float> EarlierPcm;
			EarlierPcm.SetNumUninitialized(3 * Frames);
			TestEqual(TEXT("Earlier queued PCM drains as three complete blocks"),
			Patch->PopAudio(EarlierPcm.GetData(), EarlierPcm.Num(), false), EarlierPcm.Num());
			TArray<float> RetainedFinalPcm;
			RetainedFinalPcm.SetNumUninitialized(Frames);
			if (TestEqual(TEXT("Final PCM block remains complete and poppable after source completion"),
				Patch->PopAudio(RetainedFinalPcm.GetData(), RetainedFinalPcm.Num(), false), Frames))
			{
				bool bFinalPcmMatches = true;
				for (int32 Frame = 0; Frame < Frames; ++Frame)
					if (RetainedFinalPcm[Frame] != ExpectedFinalPcm[Frame])
						bFinalPcmMatches = false;
				TestTrue(TEXT("Every final PCM sample matches the completion output"), bFinalPcmMatches);
			}
			TestEqual(TEXT("All completed source PCM has drained"), Patch->GetNumSamplesAvailable(), 0);

			// Reset 开启新一轮播放；完成 latch 不能让新一轮一直静音。
			const uint64 GenerationBeforeReset = Context->Device->Voices[Slot]->LiveGeneration.load();
			Source.Reset(Params);
			TestFalse(TEXT("Reset releases the old consumer before reacquisition"),
				Context->SourceConsumers[Slot].load(std::memory_order_acquire));
			TestEqual(TEXT("Reset retains exactly one reinitialized renderer"),
				Context->Device->SourceRendererInstances.load(), int32(1));
			Source.Execute();
			TestTrue(TEXT("Reset permits a new non-final send block"),
				Context->SourceSendActive[Slot].load(std::memory_order_acquire));
			TestTrue(TEXT("Reset reacquires its single consumer"), Context->SourceConsumers[Slot].load());
			TestTrue(TEXT("Reset playback uses a newer source generation"),
				Context->Device->Voices[Slot]->LiveGeneration.load() > GenerationBeforeReset);
			TestEqual(TEXT("Reset keeps the original AudioComponent identity"), Context->AudioIds[Slot].load(), uint64(43));
			TestEqual(TEXT("Reset produces exactly one additional complete block"),
				Context->SourceSendBlockCounts[Slot].load(), uint64(5));
			TestEqual(TEXT("Reset block is fully queued"), Patch->GetNumSamplesAvailable(), Frames);
			TestEqual(TEXT("Reset introduced no duplicate consumer"), Context->DuplicateConsumers.load(), uint64(0));
		}

		TestTrue(TEXT("Consumer lease remains held until operator destruction"),
			Context->SourceConsumers[Slot].load(std::memory_order_acquire));
		TestEqual(TEXT("Renderer lease remains held until operator destruction"),
			Context->Device->SourceRendererInstances.load(std::memory_order_acquire), int32(1));
	}

	TestFalse(TEXT("Operator destruction releases the source consumer lease"),
		Context->SourceConsumers[Slot].load(std::memory_order_acquire));
	TestEqual(TEXT("Operator destruction releases its renderer lease"),
		Context->Device->SourceRendererInstances.load(std::memory_order_acquire), int32(0));
	TestTrue(TEXT("Context retains its producer handle until context shutdown"),
		Context->SourceSendInputs[Slot].IsValid());

	IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
	bContextStopped = true;
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticSourceSendOverflow,
	"IceMoon.AcousticField.MetaSound.SourceSendOverflow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticSourceSendOverflow::RunTest(const FString&)
{
	using namespace Metasound;
	constexpr uint32 TestDevice = MAX_uint32 - 2;
	auto Context = IMAcousticMetaSound::CreateAcousticMetaSoundContext(TestDevice, 48000, 124);
	if (!TestNotNull(TEXT("Isolated overflow-test context"), Context.Get())) return false;

	bool bContextStopped = false;
	ON_SCOPE_EXIT
	{
		if (Context.IsValid() && !bContextStopped)
			IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
	};

	Context->Device->WorldGeneration.store(124);
	Context->Device->Enabled.store(true);
	Context->Device->RenderRoutes.store(7);
	Context->SourceSendTransportEnabled = true;
	Context->SourceSendCapacityFrames = int32(FIMAcousticMetaSoundContext::Frames * 4);

	const int32 Slot = IMAcousticMetaSound::RegisterAcousticMetaSoundSource(Context, 43);
	if (!TestTrue(TEXT("Overflow-test voice is registered"), Slot == 0)) return false;
	Context->SendGains[Slot].store(1.f);
	Context->ReverbSendGains[Slot].store(1.f);
	Context->DistanceGains[Slot].store(.5f);

	const FOperatorSettings Settings(48000, 93.75f);
	FMetasoundEnvironment Environment;
	Environment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
	::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceNode Node(TEXT("SourceSendOverflow"), FGuid::NewGuid());
	FInputVertexInterfaceData Inputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetInputInterface());
	auto Mono = FAudioBufferWriteRef::CreateNew(Settings);
	for (int32 Frame = 0; Frame < Mono->Num(); ++Frame)
		Mono->GetData()[Frame] = 1.f;
	auto Voice = FInt32WriteRef::CreateNew(Slot);
	const FBuildOperatorParams Params(Node, Settings, Inputs, Environment);

	bool bQueueShortWriteObserved = false;
	{
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator Source(Params, Mono, Voice, FTriggerReadRef::CreateNew(Params.OperatorSettings));
		Source.Execute();
		TestEqual(TEXT("Actual Source Execute successfully pushes its first complete block"),
			Context->SourceSendBlockCounts[Slot].load(), uint64(1));
		TestEqual(TEXT("Successful first block has no push failure"),
			Context->SourceSendPushFailures.load(), uint64(0));
		TestFalse(TEXT("Successful first block leaves transport healthy"),
			Context->SourceSendTransportFailed.load());
		TestEqual(TEXT("Successful first block has no invalid-block count"),
			Context->InvalidBlocks.load(), uint64(0));

		auto* Producer = Context->SourceSendInputs[Slot].Get();
		if (!TestNotNull(TEXT("Context-owned source producer remains live"), Producer)) return false;
		TArray<float> FillBlock;
		FillBlock.Init(0.f, int32(FIMAcousticMetaSoundContext::Frames));
		const int32 MaxFillAttempts = FMath::DivideAndRoundUp(
			Context->SourceSendCapacityFrames, int32(FIMAcousticMetaSoundContext::Frames)) + 2;
		for (int32 Attempt = 0; Attempt < MaxFillAttempts; ++Attempt)
		{
			const int32 SamplesPushed = Producer->PushAudio(FillBlock.GetData(), FillBlock.Num());
			if (SamplesPushed < 0)
			{
				AddError(TEXT("Context-owned source producer became invalid while its operator lease was held"));
				break;
			}
			if (SamplesPushed != FillBlock.Num())
			{
				bQueueShortWriteObserved = true;
				break;
			}
		}
		TestTrue(TEXT("Bounded producer fill observes the actual queue short write"), bQueueShortWriteObserved);

		const uint64 PushFailuresBeforeExecute = Context->SourceSendPushFailures.load();
		const uint64 InvalidBlocksBeforeExecute = Context->InvalidBlocks.load();
		const uint64 SuccessfulBlocksBeforeExecute = Context->SourceSendBlockCounts[Slot].load();
		Source.Execute();
		TestEqual(TEXT("Actual Source Execute latches exactly one short-push failure"),
			Context->SourceSendPushFailures.load(), PushFailuresBeforeExecute + 1);
		TestTrue(TEXT("Actual Source Execute fails transport closed"),
			Context->SourceSendTransportFailed.load());
		TestEqual(TEXT("Actual Source Execute counts the rejected block"),
			Context->InvalidBlocks.load(), InvalidBlocksBeforeExecute + 1);
		TestEqual(TEXT("Short push is not counted as a successful source block"),
			Context->SourceSendBlockCounts[Slot].load(), SuccessfulBlocksBeforeExecute);
	}

	TestFalse(TEXT("Source consumer lease is released by operator destruction"),
		Context->SourceConsumers[Slot].load(std::memory_order_acquire));
	TestEqual(TEXT("Source renderer lease is released by operator destruction"),
		Context->Device->SourceRendererInstances.load(), int32(0));
	TestTrue(TEXT("Context retains its producer handle until context shutdown"),
		Context->SourceSendInputs[Slot].IsValid());

	IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
	bContextStopped = true;
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound source-send overflow"), HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSound source-send overflow %s"), HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticReverbSendDistance,
	"IceMoon.AcousticField.MetaSound.ReverbSendDistance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticReverbSendDistance::RunTest(const FString&)
{
	const float Reference = IMAcousticRecipe::ReverbSendDistanceGain(1.0f);
	const float Near = IMAcousticRecipe::ReverbSendDistanceGain(2.0f);
	const float Mid = IMAcousticRecipe::ReverbSendDistanceGain(16.0f);
	const float Far = IMAcousticRecipe::ReverbSendDistanceGain(IMAcousticRecipe::ReverbSendFarDistanceM);
	TestTrue(TEXT("Reverb send preserves the 1 m reference"), FMath::IsNearlyEqual(Reference, 1.0f));
	TestTrue(TEXT("Reverb send halves at 2 m before the far taper"), FMath::IsNearlyEqual(Near, 0.5f));
	TestTrue(TEXT("Reverb send cannot outgrow direct inverse distance at 16 m"), Mid > 0.0f && Mid < (1.0f / 16.0f));
	TestTrue(TEXT("Reverb send reaches zero at its explicit far boundary"), FMath::IsNearlyZero(Far));
	TestTrue(TEXT("Reverb send remains silent beyond the far boundary"),
		FMath::IsNearlyZero(IMAcousticRecipe::ReverbSendDistanceGain(64.0f)));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound distance-weighted reverb send"),
		HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSoundReverbSendDistance %s"),
		HasAnyErrors() ? TEXT("FAIL") : TEXT("PASS"));
	return !HasAnyErrors();
}

namespace IMAcousticMetaSoundNodesPrivate
{
constexpr int32 ReferenceSampleRate = 48000;
constexpr int32 ReferenceGraphFrames = 512;
constexpr uint32 ReferenceSourceToEnvironmentLagFrames = 4608;
constexpr int32 ReferenceWarmupBlocks = ReferenceSourceToEnvironmentLagFrames / ReferenceGraphFrames;
constexpr int32 IMReferenceGraphWarmupBlocks = ReferenceWarmupBlocks
	+ int32(FIMAcousticMetaSoundContext::SourceInputDelayBlocks);
constexpr float ReferenceInputGain = 0.7f;
constexpr float ReferenceWetGain = 0.25f;
// The comparison runs the same Steam Audio SDK with independent effect
// instances. The absolute term covers float accumulation and the one
// interleaved stereo sum; the relative term scales with the actual quiet
// signal. A scale floor of 1 would hide the known low-level gain control.
constexpr float ReferenceAbsoluteTolerance = 1.0e-7f;
constexpr float ReferenceRelativeTolerance = 2.0e-4f;

struct FIMReferenceComparison
{
	bool Equal = false;
	float MaxAbsError = 0.0f;
	float MaxAllowedError = 0.0f;
	int32 MaxErrorIndex = INDEX_NONE;
	FString Summary;
};

FIMReferenceComparison CompareStereo(const TArray<float>& Actual, const TArray<float>& Expected)
{
	FIMReferenceComparison Result;
	if (Actual.Num() != Expected.Num() || Actual.Num() == 0 || (Actual.Num() & 1) != 0)
	{
		Result.Summary = FString::Printf(TEXT("shape actual=%d expected=%d"), Actual.Num(), Expected.Num());
		return Result;
	}
	Result.Equal = true;
	for (int32 I = 0; I < Actual.Num(); ++I)
	{
		const float Difference = FMath::Abs(Actual[I] - Expected[I]);
		const float Scale = FMath::Max(FMath::Abs(Actual[I]), FMath::Abs(Expected[I]));
		const float Allowed = ReferenceAbsoluteTolerance + ReferenceRelativeTolerance * Scale;
		if (Difference > Result.MaxAbsError)
		{
			Result.MaxAbsError = Difference;
			Result.MaxAllowedError = Allowed;
			Result.MaxErrorIndex = I;
		}
		if (Difference > Allowed) Result.Equal = false;
	}
	Result.Summary = FString::Printf(TEXT("max_abs=%g max_allowed_at_max=%g index=%d"),
		Result.MaxAbsError, Result.MaxAllowedError, Result.MaxErrorIndex);
	return Result;
}

// Independent model of the environment wet stage's 60 Hz 2nd-order Butterworth high-pass (the V2 merge added it after the SDK reverb;
// the SDK-only reference has to apply it too or it overstates the sub-60 Hz wet energy). The coefficients are derived separately from the
// product's (RBJ high-pass written out here, in double, then rounded to float), so a wrong corner or Q is still caught. The state update runs
// in float on purpose: with poles this close to z=1 the float round-off of the recursion showed up as 2.6e-7 max absolute difference
// against a double-precision form on the frozen wet stem (sdkref9), which is the same floor the product itself has and which the unchanged 1e-7 absolute
// tolerance is not meant to cover. A double-precision recursion here would make the product's own float round-off look like a mismatch.
struct FIMReferenceWetHighPass
{
	void Configure(double SampleRate, double CornerHz)
	{
		const double Omega = 2.0 * PI * CornerHz / SampleRate;
		const double Alpha = FMath::Sin(Omega) / (2.0 * 0.70710678118654752440);
		const double CosOmega = FMath::Cos(Omega);
		const double A0 = 1.0 + Alpha;
		B0 = float((1.0 + CosOmega) * 0.5 / A0);
		B1 = float(-(1.0 + CosOmega) / A0);
		A1 = float(-2.0 * CosOmega / A0);
		A2 = float((1.0 - Alpha) / A0);
		for (float& Value : State) Value = 0.0f;
	}
	// Transposed direct form II, one state pair per channel.
	float Process(int32 Channel, float Input)
	{
		float* Z = State + 2 * Channel;
		const float Output = B0 * Input + Z[0];
		Z[0] = B1 * Input - A1 * Output + Z[1];
		Z[1] = B0 * Input - A2 * Output;
		return Output;
	}
	float B0 = 1.0f, B1 = 0.0f, A1 = 0.0f, A2 = 0.0f;
	float State[4]{};
};

float StereoEnergy(const TArray<float>& Samples)
{
	double Energy = 0.0;
	for (const float Sample : Samples) Energy += double(Sample) * double(Sample);
	return float(Energy);
}

float StereoChannelAsymmetry(const TArray<float>& Samples)
{
	float MaxDifference = 0.0f;
	for (int32 I = 0; I + 1 < Samples.Num(); I += 2)
		MaxDifference = FMath::Max(MaxDifference, FMath::Abs(Samples[I] - Samples[I + 1]));
	return MaxDifference;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticMetaSoundSDKReference,
	"IceMoon.AcousticField.MetaSound.SDKReference", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticMetaSoundSDKReference::RunTest(const FString&)
{
	using namespace Metasound;
	constexpr uint32 TestDevice = MAX_uint32 - 2;
	constexpr uint64 TestAudioId = 42;
	constexpr uint64 TestEpoch = 0x5344325245464552ull; // frozen S2 reference epoch
	const FOperatorSettings Settings(::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate, 93.75f);
	const FString BakePath = TEXT("/IceMoonAcousticField/Bakes/IM_IM_V2Audition_51EB95704ADA37C752DC909BA5B90830.IM_IM_V2Audition_51EB95704ADA37C752DC909BA5B90830");
	const FString PCMPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/MetaSoundClosure/runtime/source.f32"));

	auto Fail = [this](const FString& Reason)
	{
		AddError(Reason);
		UE_LOG(LogTemp, Error, TEXT("IMLogs MetaSoundSDKReference FAIL %s"), *Reason);
		return false;
	};

	UIMAcousticBakeAsset* Bake = LoadObject<UIMAcousticBakeAsset>(nullptr, *BakePath);
	if (!Bake || Bake->SceneData.IsEmpty() || Bake->ProbeData.IsEmpty())
		return Fail(TEXT("Frozen current bake asset could not be loaded with scene and probe payloads."));

	TArray<FVector4> ProbePreview;
	FVector ProbeOrigin = FVector::ZeroVector;
	FString PreviewError;
	if (!Bake->GetProbePreview(Bake->WorldPackage, Bake->SceneFingerprint, ProbePreview, ProbeOrigin, PreviewError)
		|| ProbePreview.IsEmpty())
		return Fail(FString::Printf(TEXT("Frozen bake probe coverage readback failed: %s"), *PreviewError));

	TArray<uint8> RawScene;
	TArray<uint8> RawProbes;
	FString PayloadFailure;
	if (!Bake->ReadPayload(Bake->WorldPackage, Bake->SceneFingerprint, RawScene, RawProbes, PayloadFailure))
		return Fail(FString::Printf(TEXT("Frozen bake raw SDK payload read failed: %s"), *PayloadFailure));

	FIMAcousticBakeData BakeData;
	BakeData.Scene.assign(RawScene.GetData(), RawScene.GetData() + RawScene.Num());
	BakeData.ProbeBatch.assign(RawProbes.GetData(), RawProbes.GetData() + RawProbes.Num());
	BakeData.CoverageProbes.reserve(ProbePreview.Num());
	for (const FVector4& Probe : ProbePreview)
		BakeData.CoverageProbes.push_back({{float(Probe.X), float(Probe.Y), float(Probe.Z)}, float(Probe.W)});

	FIMAcousticSimulation Simulation;
	std::string SimulationError;
	if (!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), SimulationError))
		return Fail(FString::Printf(TEXT("SDK reference pathing options failed: %s"), ANSI_TO_TCHAR(SimulationError.c_str())));
	if (!Simulation.Load(BakeData, ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate, ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames, SimulationError))
		return Fail(FString::Printf(TEXT("SDK reference current bake load failed: %s"), ANSI_TO_TCHAR(SimulationError.c_str())));

	const FVector SourceUE(550.0f, 300.0f, 150.0f);
	const FVector RequestedA(750.0f, 300.0f, 150.0f);
	const FVector RequestedB(1350.0f, 300.0f, 150.0f);
	struct FIMReferenceListenerCandidate
	{
		FString Name;
		FVector Position = FVector::ZeroVector;
	};
	TArray<FIMReferenceListenerCandidate> ListenerCandidates;
	auto AddListenerCandidate = [&ListenerCandidates](const TCHAR* Name, const FVector& Position)
	{
		FIMReferenceListenerCandidate Candidate;
		Candidate.Name = Name;
		Candidate.Position = Position;
		ListenerCandidates.Add(MoveTemp(Candidate));
	};
	AddListenerCandidate(TEXT("A"), RequestedA);
	AddListenerCandidate(TEXT("B"), RequestedB);
	TArray<FVector> CoverageCandidates;
	CoverageCandidates.Reserve(ProbePreview.Num());
	for (const FVector4& Probe : ProbePreview)
	{
		CoverageCandidates.Add(FromSDKPosition(IPLVector3{float(Probe.X), float(Probe.Y), float(Probe.Z)}, ProbeOrigin));
	}
	CoverageCandidates.Sort([&](const FVector& Left, const FVector& Right)
	{
		const float LeftDistance = FMath::Min((Left - RequestedA).SizeSquared(), (Left - RequestedB).SizeSquared());
		const float RightDistance = FMath::Min((Right - RequestedA).SizeSquared(), (Right - RequestedB).SizeSquared());
		return LeftDistance < RightDistance;
	});
	for (const FVector& Candidate : CoverageCandidates)
	{
		if (ListenerCandidates.Num() >= 34) break;
		AddListenerCandidate(TEXT("coverage_probe"), Candidate);
	}

	FIMAcousticAudioFrame FrozenFrame;
	FIMAcousticAudioFrame FrozenAFrame;
	FIMAcousticAudioFrame FrozenBFrame;
	IPLCoordinateSpace3 FrozenASpace{};
	IPLCoordinateSpace3 FrozenBSpace{};
	FVector FrozenAUE = FVector::ZeroVector;
	FVector FrozenBUE = FVector::ZeroVector;
	bool HasFrozenA = false;
	bool HasFrozenB = false;
	IPLCoordinateSpace3 SourceSpace = ToSDKSpace(FTransform(FQuat::Identity, SourceUE), ProbeOrigin);
	IPLCoordinateSpace3 ListenerSpace{};
	FVector ListenerUE = FVector::ZeroVector;
	FString SelectedListenerName;
	FString CandidateDiagnostics;
	for (int32 CandidateIndex = 0; CandidateIndex < ListenerCandidates.Num(); ++CandidateIndex)
	{
		const FIMReferenceListenerCandidate& Candidate = ListenerCandidates[CandidateIndex];
		const IPLCoordinateSpace3 CandidateSpace = ToSDKSpace(FTransform(FQuat::Identity, Candidate.Position), ProbeOrigin);
		FIMAcousticAudioFrame CandidateFrame;
		std::string CandidateError;
		const uint64 CandidateKey = TestAudioId + 1000ull + uint64(CandidateIndex);
		const bool Evaluated = Simulation.Evaluate(CandidateKey, 1, SourceSpace, CandidateSpace, CandidateFrame, CandidateError);
		Simulation.Remove(CandidateKey);
		const bool Valid = Evaluated && CandidateFrame.DirectValid && CandidateFrame.PathValid;
		if (CandidateDiagnostics.Len() < 4000)
		{
			CandidateDiagnostics += FString::Printf(TEXT("%s[%s,d=%d,p=%d,occ=%g];"), *Candidate.Name,
				Valid ? TEXT("valid") : TEXT("reject"), CandidateFrame.DirectValid ? 1 : 0,
				CandidateFrame.PathValid ? 1 : 0, CandidateFrame.Direct.occlusion);
		}
		if (Valid)
		{
			FrozenFrame = CandidateFrame;
			ListenerSpace = CandidateSpace;
			ListenerUE = Candidate.Position;
			SelectedListenerName = Candidate.Name;
			break;
		}
	}
	if (SelectedListenerName.IsEmpty())
		return Fail(FString::Printf(TEXT("Frozen current snapshot did not produce both valid direct and path stems after A/B and coverage-probe candidates: %s origin_cm=(%g,%g,%g)"),
			*CandidateDiagnostics, ProbeOrigin.X, ProbeOrigin.Y, ProbeOrigin.Z));
	TArray<uint8> RawPCM;
	if (!FFileHelper::LoadFileToArray(RawPCM, *PCMPath)
		|| RawPCM.Num() < (::IMAcousticMetaSoundNodesPrivate::ReferenceSourceToEnvironmentLagFrames + ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames) * int32(sizeof(float)))
		return Fail(FString::Printf(TEXT("Frozen PCM input is missing or shorter than the declared 4608-frame adaptation offset plus one 512-frame block: %s"), *PCMPath));
	TArray<float> Mono;
	Mono.SetNumUninitialized(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames);
	// The declared window starts at the 4608-frame adaptation offset. The captured audition source plays exact zeros and then sub-LSB
	// samples for its first second, and where its audible part begins moves with the host's audio start-up, so a fixed 4608-frame window
	// can hold no usable signal (the stereo negative control below needs a real stereo mix). The reference therefore uses the loudest
	// 512-frame block at or after the declared offset that follows a block carrying signal (so the SDK reference is not compared across
	// an onset step). The comparison tolerances and the non-silent requirement are unchanged.
	const int32 PcmFrameCount = RawPCM.Num() / int32(sizeof(float));
	const int32 BlockFrames = ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames;
	const int32 DeclaredStartFrame = int32(::IMAcousticMetaSoundNodesPrivate::ReferenceSourceToEnvironmentLagFrames);
	int32 WindowStartFrame = INDEX_NONE;
	{
		const float* Pcm = reinterpret_cast<const float*>(RawPCM.GetData());
		double PreviousEnergy = 0.0;
		double BestEnergy = 0.0;
		for (int32 StartFrame = DeclaredStartFrame - BlockFrames; StartFrame + BlockFrames <= PcmFrameCount; StartFrame += BlockFrames)
		{
			double Energy = 0.0;
			for (int32 I = 0; I < BlockFrames; ++I)
			{
				float Sample = 0.0f;
				FMemory::Memcpy(&Sample, Pcm + StartFrame + I, sizeof(float));
				if (!FMath::IsFinite(Sample)) return Fail(TEXT("Frozen PCM input contains a non-finite sample."));
				Energy += double(Sample) * double(Sample);
			}
			if (StartFrame >= DeclaredStartFrame && PreviousEnergy > 0.0 && Energy > BestEnergy)
			{
				BestEnergy = Energy;
				WindowStartFrame = StartFrame;
			}
			PreviousEnergy = Energy;
		}
	}
	if (WindowStartFrame == INDEX_NONE)
		return Fail(TEXT("Frozen PCM input is silent in the declared phase window and in every later block."));
	for (int32 I = 0; I < BlockFrames; ++I)
	{
		float Sample = 0.0f;
		FMemory::Memcpy(&Sample, RawPCM.GetData() + (WindowStartFrame + I) * sizeof(float), sizeof(float));
		Mono[I] = Sample * ::IMAcousticMetaSoundNodesPrivate::ReferenceInputGain;
	}

	auto MeasureCandidateEnergy = [&](const FIMAcousticAudioFrame& CandidateFrame, const FVector& CandidatePosition,
		double& OutDirectEnergy, double& OutPathEnergy, double& OutWetEnergy) -> bool
	{
		OutDirectEnergy = 0.0;
		OutPathEnergy = 0.0;
		OutWetEnergy = 0.0;
		auto RenderSourceRoute = [&](uint32 Routes, double& OutEnergy) -> bool
		{
			IPLHRTF HRTF = ::IMAcousticMetaSoundNodesPrivate::CreateOperatorHRTF(Settings);
			if (!HRTF) return false;
			FIMAcousticAudioRenderer Renderer;
			const bool Initialized = Renderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTF,
				::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate, ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames);
			iplHRTFRelease(&HRTF);
			if (!Initialized) return false;
			TArray<float> Stereo;
			Stereo.SetNumZeroed(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames * 2);
			for (int32 Warmup = 0; Warmup < ::IMAcousticMetaSoundNodesPrivate::ReferenceWarmupBlocks; ++Warmup)
			{
				if (!Renderer.Render(Mono.GetData(), ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames, CandidateFrame,
					Stereo.GetData(), nullptr, nullptr, nullptr, Routes)) return false;
			}
			OutEnergy = double(::IMAcousticMetaSoundNodesPrivate::StereoEnergy(Stereo));
			return true;
		};
		if (!RenderSourceRoute(1, OutDirectEnergy) || !RenderSourceRoute(2, OutPathEnergy)) return false;

		FIMAcousticReverbSlot CandidateIR;
		CandidateIR.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
		std::string CandidateReverbError;
		if (!Simulation.EvaluateReverb(CandidateIR, CandidateFrame.Listener, CandidateReverbError)) return false;
		if (!CandidateIR.Params.ir || CandidateIR.Params.type != IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
			|| CandidateIR.Params.numChannels != FIMAcousticAudioFrame::Coefficients) return false;
		IPLHRTF HRTF = ::IMAcousticMetaSoundNodesPrivate::CreateOperatorHRTF(Settings);
		if (!HRTF) return false;
		FIMAcousticReverbRenderer Renderer;
		const bool Initialized = Renderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTF,
			::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate, ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames, CandidateIR.Params.irSize);
		iplHRTFRelease(&HRTF);
		if (!Initialized) return false;
		TArray<float> Send;
		Send.SetNumUninitialized(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames);
		const float ReverbSendGain = IMAcousticRecipe::ReverbSendDistanceGain(
			FVector::Distance(SourceUE, CandidatePosition) * 0.01f);
		for (int32 I = 0; I < ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames; ++I)
			Send[I] = Mono[I] * ReverbSendGain;
		TArray<float> Wet;
		Wet.SetNumZeroed(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames * 2);
		for (int32 Warmup = 0; Warmup < ::IMAcousticMetaSoundNodesPrivate::ReferenceWarmupBlocks; ++Warmup)
		{
			if (!Renderer.Render(Send.GetData(), ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames, CandidateIR.Params,
				CandidateIR.Listener, Wet.GetData())) return false;
		}
		OutWetEnergy = double(::IMAcousticMetaSoundNodesPrivate::StereoEnergy(Wet)) * double(::IMAcousticMetaSoundNodesPrivate::ReferenceWetGain) * double(::IMAcousticMetaSoundNodesPrivate::ReferenceWetGain);
		// The current V2 fixture intentionally exercises the blocked-direct
		// path: Steam Audio's occlusion gain is zero while the validated
		// alternate path and listener IR remain audible. Direct is still
		// compared and must stay zero for this frozen physical snapshot.
		return OutPathEnergy > 1.0e-12 && OutWetEnergy > 1.0e-12;
	};

	FString AudibleCandidateDiagnostics;
	bool AudibleCandidateFound = false;
	for (int32 CandidateIndex = 0; CandidateIndex < ListenerCandidates.Num(); ++CandidateIndex)
	{
		const FIMReferenceListenerCandidate& Candidate = ListenerCandidates[CandidateIndex];
		const IPLCoordinateSpace3 CandidateSpace = ToSDKSpace(FTransform(FQuat::Identity, Candidate.Position), ProbeOrigin);
		FIMAcousticAudioFrame CandidateFrame;
		std::string CandidateError;
		const uint64 CandidateKey = TestAudioId + 2000ull + uint64(CandidateIndex);
		const bool Evaluated = Simulation.Evaluate(CandidateKey, 1, SourceSpace, CandidateSpace, CandidateFrame, CandidateError);
		Simulation.Remove(CandidateKey);
		// Keep the requested A/B frames even when the current bake reports a
		// missing direct/path stem at those exact points. The identity test is
		// about rejecting a stale spatial snapshot, not about turning the
		// scene's coverage gap into a false audio PASS.
		if (Candidate.Name == TEXT("A") && Evaluated)
		{
			FrozenAFrame = CandidateFrame;
			FrozenASpace = CandidateSpace;
			FrozenAUE = Candidate.Position;
			HasFrozenA = true;
		}
		else if (Candidate.Name == TEXT("B") && Evaluated)
		{
			FrozenBFrame = CandidateFrame;
			FrozenBSpace = CandidateSpace;
			FrozenBUE = Candidate.Position;
			HasFrozenB = true;
		}
		if (!Evaluated || !CandidateFrame.DirectValid || !CandidateFrame.PathValid)
		{
			AudibleCandidateDiagnostics += FString::Printf(TEXT("%s[invalid];"), *Candidate.Name);
			continue;
		}
		double DirectEnergy = 0.0, PathEnergy = 0.0, WetEnergy = 0.0;
		const bool Audible = MeasureCandidateEnergy(CandidateFrame, Candidate.Position, DirectEnergy, PathEnergy, WetEnergy);
		AudibleCandidateDiagnostics += FString::Printf(TEXT("%s[audible=%d,d=%g,p=%g,w=%g];"), *Candidate.Name,
			Audible ? 1 : 0, DirectEnergy, PathEnergy, WetEnergy);
		if (Audible)
		{
			if (Candidate.Name == TEXT("A"))
			{
				FrozenAFrame = CandidateFrame;
				FrozenASpace = CandidateSpace;
				FrozenAUE = Candidate.Position;
				HasFrozenA = true;
			}
			else if (Candidate.Name == TEXT("B"))
			{
				FrozenBFrame = CandidateFrame;
				FrozenBSpace = CandidateSpace;
				FrozenBUE = Candidate.Position;
				HasFrozenB = true;
			}
			if (!AudibleCandidateFound)
			{
				FrozenFrame = CandidateFrame;
				ListenerSpace = CandidateSpace;
				ListenerUE = Candidate.Position;
				SelectedListenerName = Candidate.Name;
				AudibleCandidateFound = true;
				UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKReference listener_candidate=%s listener_ue=(%g,%g,%g) listener_sdk=(%g,%g,%g) candidate_energy direct=%g path=%g wet=%g"),
					*SelectedListenerName, ListenerUE.X, ListenerUE.Y, ListenerUE.Z,
					ListenerSpace.origin.x, ListenerSpace.origin.y, ListenerSpace.origin.z,
					DirectEnergy, PathEnergy, WetEnergy);
			}
			if (HasFrozenA && HasFrozenB) break;
		}
	}
	if (!AudibleCandidateFound)
		return Fail(FString::Printf(TEXT("Current bake has no candidate with audible direct/path/wet reference energy after A/B and coverage-probe scan: %s"),
			*AudibleCandidateDiagnostics));
	if (!HasFrozenA)
	{
		// A/B may be outside the current bake's valid direct/path coverage.
		// Use the first valid frozen candidate only as a bounded identity
		// fallback and expose that fact in evidence; it is not an A/B audio
		// coverage PASS.
		FrozenAFrame = FrozenFrame;
		FrozenASpace = ListenerSpace;
		FrozenAUE = ListenerUE;
	}
	FrozenFrame.Sequence = 1;

	auto Context = IMAcousticMetaSound::CreateAcousticMetaSoundContext(TestDevice, ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate, TestEpoch);
	if (!Context) return Fail(TEXT("SDK reference MetaSound context could not be created."));
	const int32 Slot = IMAcousticMetaSound::RegisterAcousticMetaSoundSource(Context, TestAudioId);
	if (Slot != 0) { IMAcousticMetaSound::StopAcousticMetaSoundContext(Context); return Fail(TEXT("SDK reference did not acquire frozen voice slot zero.")); }
	Context->Device->WorldGeneration.store(TestEpoch, std::memory_order_release);
	Context->Device->Enabled.store(true, std::memory_order_release);
	Context->Device->RenderRoutes.store(7, std::memory_order_release);
	Context->ListenerSpatialVersion.store(1, std::memory_order_release);
	Context->SendGains[0].store(1.0f, std::memory_order_release);
	const float FrozenReverbSendDistanceGain = IMAcousticRecipe::ReverbSendDistanceGain(
		FVector::Distance(SourceUE, ListenerUE) * 0.01f);
	Context->ReverbSendGains[0].store(FrozenReverbSendDistanceGain, std::memory_order_release);
	Context->DistanceGains[0].store(1.0f, std::memory_order_release);
	Context->WetGain.store(::IMAcousticMetaSoundNodesPrivate::ReferenceWetGain, std::memory_order_release);

	FIMAcousticReverbSlot& FrozenIR = Context->Pool->Slots[0];
	FrozenIR.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
	if (!Simulation.EvaluateReverb(FrozenIR, FrozenFrame.Listener, SimulationError))
	{
		IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
		return Fail(FString::Printf(TEXT("Frozen current IR evaluation failed: %s"), ANSI_TO_TCHAR(SimulationError.c_str())));
	}
	FrozenIR.Sequence = 1;
	FrozenIR.SpatialVersion = 1;
	FrozenIR.Applied = false;
	FrozenIR.CapturedSeconds = FPlatformTime::Seconds();
	FrozenIR.State.store(EIMAcousticIRState::Ready, std::memory_order_release);
	if (!FrozenIR.Params.ir || FrozenIR.Params.type != IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
		|| FrozenIR.Params.numChannels != FIMAcousticAudioFrame::Coefficients)
	{
		IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
		return Fail(TEXT("Frozen current IR does not satisfy the order-1 four-channel convolution contract."));
	}

	TArray<float> ExpectedSend;
	ExpectedSend.SetNumUninitialized(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames);
	const float ExpectedSendGain = Context->ReverbSendGains[0].load(std::memory_order_acquire);
	if (!FMath::IsFinite(ExpectedSendGain) || ExpectedSendGain <= 0.0f)
	{
		IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
		return Fail(TEXT("Frozen current snapshot has no positive finite environmental send gain."));
	}
	for (int32 I = 0; I < ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames; ++I) ExpectedSend[I] = Mono[I] * ExpectedSendGain;

	auto RenderReferenceSource = [&](const FIMAcousticAudioFrame& Frame, uint32 Routes, TArray<float>& Out) -> bool
	{
		IPLHRTF HRTF = ::IMAcousticMetaSoundNodesPrivate::CreateOperatorHRTF(Settings);
		if (!HRTF) return false;
		FIMAcousticAudioRenderer Renderer;
		const bool Initialized = Renderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTF, ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate, ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames);
		iplHRTFRelease(&HRTF);
		if (!Initialized) return false;
		Out.SetNumZeroed(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames * 2);
		for (int32 Warmup = 0; Warmup < ::IMAcousticMetaSoundNodesPrivate::ReferenceWarmupBlocks; ++Warmup)
		{
			if (!Renderer.Render(Mono.GetData(), ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames, Frame, Out.GetData(), nullptr, nullptr, nullptr, Routes))
				return false;
		}
		return true;
	};

	auto RenderReferenceWet = [&](TArray<float>& Out) -> bool
	{
		FIMAcousticReverbSlot ReferenceIR;
		ReferenceIR.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
		std::string ReferenceReverbError;
		if (!Simulation.EvaluateReverb(ReferenceIR, FrozenFrame.Listener, ReferenceReverbError)) return false;
		if (!ReferenceIR.Params.ir || ReferenceIR.Params.type != IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
			|| ReferenceIR.Params.numChannels != FIMAcousticAudioFrame::Coefficients) return false;
		IPLHRTF HRTF = ::IMAcousticMetaSoundNodesPrivate::CreateOperatorHRTF(Settings);
		if (!HRTF) return false;
		FIMAcousticReverbRenderer Renderer;
		const bool Initialized = Renderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTF, ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate,
			::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames, ReferenceIR.Params.irSize);
		iplHRTFRelease(&HRTF);
		if (!Initialized) return false;
		TArray<float> RawWet;
		RawWet.SetNumZeroed(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames * 2);
		Out.SetNumUninitialized(RawWet.Num());
		// The graph's high-pass sees every warm-up block in order, so the reference filters every rendered block with persistent state.
		::IMAcousticMetaSoundNodesPrivate::FIMReferenceWetHighPass HighPass;
		HighPass.Configure(::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate, ::IMAcousticMetaSoundNodesPrivate::IMWetHighPassCornerHz);
		auto RenderAndFilter = [&]() -> bool
		{
			if (!Renderer.Render(ExpectedSend.GetData(), ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames, ReferenceIR.Params,
				ReferenceIR.Listener, RawWet.GetData())) return false;
			for (int32 Frame = 0; Frame < ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames; ++Frame)
			{
				Out[2 * Frame] = HighPass.Process(0, RawWet[2 * Frame]);
				Out[2 * Frame + 1] = HighPass.Process(1, RawWet[2 * Frame + 1]);
			}
			return true;
		};
		if (!RenderAndFilter()) return false;
		for (int32 Warmup = 1; Warmup < ::IMAcousticMetaSoundNodesPrivate::ReferenceWarmupBlocks; ++Warmup)
		{
			if (!RenderAndFilter()) return false;
		}
		for (int32 I = 0; I < Out.Num(); ++I) Out[I] *= ::IMAcousticMetaSoundNodesPrivate::ReferenceWetGain;
		return true;
	};

	auto RunGraph = [&](uint32 Routes, TArray<float>& OutSource, TArray<float>& OutWet,
		TArray<float>& OutSend, FString& OutError) -> bool
	{
		OutError.Reset();
		Context->Device->RenderRoutes.store(Routes, std::memory_order_release);
		FrozenIR.Applied = false;
		FrozenIR.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
		std::string GraphReverbError;
		if (!Simulation.EvaluateReverb(FrozenIR, FrozenFrame.Listener, GraphReverbError))
		{
			OutError = FString::Printf(TEXT("fresh frozen IR refresh failed: %s"), ANSI_TO_TCHAR(GraphReverbError.c_str()));
			return false;
		}
		FrozenIR.Sequence = 1;
		FrozenIR.SpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
		FrozenIR.CapturedSeconds = FPlatformTime::Seconds();
		FrozenIR.State.store(EIMAcousticIRState::Ready, std::memory_order_release);
		FIMAcousticAudioFrame Frame = FrozenFrame;
		Frame.Generation = Context->Device->Voices[0]->LiveGeneration.load(std::memory_order_acquire) + 1;
		Frame.Sequence = 1;
		FIMAcousticVoiceResult Result;
		Result.Frame = Frame;
		Result.AudioComponentId = TestAudioId;
		Result.WorldGeneration = TestEpoch;
		Result.SpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
		Result.PublishedSeconds = FPlatformTime::Seconds();
		if (!Context->Device->Voices[0]->Results.Push(Result))
		{
			OutError = TEXT("frozen source result ring was not empty");
			return false;
		}

		FMetasoundEnvironment MetaEnvironment;
		MetaEnvironment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceNode SourceNode(TEXT("SDKReferenceSource"), FGuid::NewGuid());
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentNode EnvironmentNode(TEXT("SDKReferenceEnvironment"), FGuid::NewGuid());
		FInputVertexInterfaceData Inputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetInputInterface());
		FInputVertexInterfaceData EnvironmentInputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentOperator::GetVertexInterface().GetInputInterface());
		auto MonoReference = FAudioBufferWriteRef::CreateNew(Settings);
		for (int32 I = 0; I < ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames; ++I) MonoReference->GetData()[I] = Mono[I];
		auto Voice = FInt32WriteRef::CreateNew(0);
		const FBuildOperatorParams SourceParams(SourceNode, Settings, Inputs, MetaEnvironment);
		const FBuildOperatorParams EnvironmentParams(EnvironmentNode, Settings, EnvironmentInputs, MetaEnvironment);
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator Source(SourceParams, MonoReference, Voice, FTriggerReadRef::CreateNew(SourceParams.OperatorSettings));
		FOutputVertexInterfaceData SourceOutputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetOutputInterface());
		Source.BindOutputs(SourceOutputs);
		const FAudioBufferReadRef Send = SourceOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Send"));
		const FAudioBufferReadRef SourceLeft = SourceOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Left"));
		const FAudioBufferReadRef SourceRight = SourceOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Right"));
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentOperator Environment(EnvironmentParams, Send);
		FOutputVertexInterfaceData EnvironmentOutputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticEnvironmentOperator::GetVertexInterface().GetOutputInterface());
		Environment.BindOutputs(EnvironmentOutputs);
		for (int32 Warmup = 0; Warmup < ::IMAcousticMetaSoundNodesPrivate::IMReferenceGraphWarmupBlocks; ++Warmup)
		{
			const uint64 RejectedBefore = Context->Device->RejectedBlocks.load(std::memory_order_acquire);
			Source.Execute();
			if (Context->Device->RejectedBlocks.load(std::memory_order_acquire) != RejectedBefore)
			{
				OutError = TEXT("valid frozen source frame was rejected by the graph operator");
				return false;
			}
			Environment.Execute();
		}
		const FAudioBufferReadRef WetLeft = EnvironmentOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Left"));
		const FAudioBufferReadRef WetRight = EnvironmentOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Right"));
		OutSource.SetNumUninitialized(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames * 2);
		OutWet.SetNumUninitialized(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames * 2);
		OutSend.SetNumUninitialized(::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames);
		for (int32 I = 0; I < ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames; ++I)
		{
			OutSource[2 * I] = SourceLeft->GetData()[I];
			OutSource[2 * I + 1] = SourceRight->GetData()[I];
			OutWet[2 * I] = WetLeft->GetData()[I];
			OutWet[2 * I + 1] = WetRight->GetData()[I];
			OutSend[I] = Send->GetData()[I];
		}
		return true;
	};

	// Identity negative control: a frozen response from A must not be accepted
	// after the source/world/voice identity has changed on the path to B.  The
	// source operator is the consumer-side authority, so inject one malformed
	// result at a time and require a degraded block without a rendered block.
	auto RunIdentityNegative = [&](const TCHAR* Label, uint64 ResultAudioId,
		uint64 ResultWorldGeneration, bool bWrongVoiceGeneration,
		const FIMAcousticAudioFrame& InjectedFrame, uint64 ResultSpatialVersion,
		uint32 ExpectedRejectDetail, FString& OutError) -> bool
	{
		OutError.Reset();
		Context->Device->RenderRoutes.store(7, std::memory_order_release);
		const uint64 RejectedBefore = Context->Device->RejectedBlocks.load(std::memory_order_acquire);
		const uint64 RenderedBefore = Context->Device->RenderedBlocks.load(std::memory_order_acquire);
		Context->LastRejectDetail.store(0, std::memory_order_release);
		const uint64 ExpectedGeneration = Context->Device->Voices[0]->LiveGeneration.load(std::memory_order_acquire) + 1;

		FIMAcousticVoiceResult Mismatched;
		Mismatched.Frame = InjectedFrame;
		Mismatched.Frame.Generation = bWrongVoiceGeneration ? ExpectedGeneration + 1 : ExpectedGeneration;
		Mismatched.Frame.Sequence = 9000 + ExpectedGeneration;
		Mismatched.AudioComponentId = ResultAudioId;
		Mismatched.WorldGeneration = ResultWorldGeneration;
		Mismatched.SpatialVersion = ResultSpatialVersion;
		Mismatched.PublishedSeconds = FPlatformTime::Seconds();
		if (!Context->Device->Voices[0]->Results.Push(Mismatched))
		{
			OutError = FString::Printf(TEXT("%s result ring was not empty before identity injection"), Label);
			return false;
		}

		FMetasoundEnvironment IdentityEnvironment;
		IdentityEnvironment.SetValue<uint32>(Frontend::SourceInterface::Environment::DeviceID, TestDevice);
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceNode IdentityNode(FName(*FString::Printf(TEXT("SDKIdentityNegative_%s"), Label)), FGuid::NewGuid());
		FInputVertexInterfaceData IdentityInputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetInputInterface());
		auto IdentityMono = FAudioBufferWriteRef::CreateNew(Settings);
		for (int32 I = 0; I < ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames; ++I) IdentityMono->GetData()[I] = Mono[I];
		auto IdentityVoice = FInt32WriteRef::CreateNew(0);
		const FBuildOperatorParams IdentityParams(IdentityNode, Settings, IdentityInputs, IdentityEnvironment);
		::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator IdentitySource(IdentityParams, IdentityMono, IdentityVoice, FTriggerReadRef::CreateNew(IdentityParams.OperatorSettings));
		FOutputVertexInterfaceData IdentityOutputs(::IMAcousticMetaSoundNodesPrivate::FIMAcousticSourceOperator::GetVertexInterface().GetOutputInterface());
		IdentitySource.BindOutputs(IdentityOutputs);
		const FAudioBufferReadRef IdentityLeft = IdentityOutputs.GetDataReadReference<FAudioBuffer>(TEXT("Left"));
		for (uint32 DelayBlock = 0; DelayBlock < FIMAcousticMetaSoundContext::SourceInputDelayBlocks; ++DelayBlock)
			IdentitySource.Execute();
		IdentitySource.Execute();

		const uint64 RejectedAfter = Context->Device->RejectedBlocks.load(std::memory_order_acquire);
		const uint64 RenderedAfter = Context->Device->RenderedBlocks.load(std::memory_order_acquire);
		const uint32 ObservedRejectDetail = Context->LastRejectDetail.load(std::memory_order_acquire);
		const bool RejectDetailMatches = ObservedRejectDetail == ExpectedRejectDetail;
		const bool Rejected = RejectedAfter == RejectedBefore + 1 && RenderedAfter == RenderedBefore && RejectDetailMatches;
		bool FiniteOutput = true;
		if (FiniteOutput)
		{
			for (int32 I = 0; I < IdentityLeft->Num(); ++I)
			{
				if (!FMath::IsFinite(IdentityLeft->GetData()[I])) { FiniteOutput = false; break; }
			}
		}
		UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKIdentityNegative label=%s rejected=%d rendered_delta=%llu finite_output=%d reject_detail=%u expected_detail=%u expected_generation=%llu"),
			Label, Rejected ? 1 : 0, RenderedAfter - RenderedBefore, FiniteOutput ? 1 : 0,
			ObservedRejectDetail,
			ExpectedRejectDetail, ExpectedGeneration);
		if (!Rejected || !FiniteOutput)
		{
			OutError = FString::Printf(TEXT("%s identity mismatch was not fail-closed (rejected_delta=%llu rendered_delta=%llu finite_output=%d reject_detail=%u expected_detail=%u)"),
				Label, RejectedAfter - RejectedBefore, RenderedAfter - RenderedBefore, FiniteOutput ? 1 : 0,
				ObservedRejectDetail,
				ExpectedRejectDetail);
			return false;
		}
		return true;
	};

	TArray<float> GraphDirect, GraphPath, GraphSourceFull, GraphWet, GraphSend;
	TArray<float> IgnoredWet, IgnoredSend;
	FString GraphError;
	if (!RunGraph(1, GraphDirect, IgnoredWet, IgnoredSend, GraphError)
		|| !RunGraph(2, GraphPath, IgnoredWet, IgnoredSend, GraphError)
		|| !RunGraph(7, GraphSourceFull, GraphWet, GraphSend, GraphError))
	{
		IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
		return Fail(FString::Printf(TEXT("Current graph reference fixture failed: %s"), *GraphError));
	}

	FIMAcousticAudioFrame ReferenceFrame = FrozenFrame;
	ReferenceFrame.Generation = 1;
	TArray<float> ReferenceDirect, ReferencePath, ReferenceSourceFull, ReferenceWet;
	if (!RenderReferenceSource(ReferenceFrame, 1, ReferenceDirect)
		|| !RenderReferenceSource(ReferenceFrame, 2, ReferencePath)
		|| !RenderReferenceSource(ReferenceFrame, 7, ReferenceSourceFull)
		|| !RenderReferenceWet(ReferenceWet))
	{
		IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
		return Fail(TEXT("SDK-only reference renderer could not render all frozen stems."));
	}

	const ::IMAcousticMetaSoundNodesPrivate::FIMReferenceComparison DirectComparison = ::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphDirect, ReferenceDirect);
	const ::IMAcousticMetaSoundNodesPrivate::FIMReferenceComparison PathComparison = ::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphPath, ReferencePath);
	const ::IMAcousticMetaSoundNodesPrivate::FIMReferenceComparison WetComparison = ::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphWet, ReferenceWet);
	TArray<float> GraphFullMix = GraphSourceFull;
	TArray<float> ReferenceFullMix = ReferenceSourceFull;
	for (int32 I = 0; I < GraphFullMix.Num(); ++I)
	{
		GraphFullMix[I] += GraphWet[I];
		ReferenceFullMix[I] += ReferenceWet[I];
	}
	const ::IMAcousticMetaSoundNodesPrivate::FIMReferenceComparison FullComparison = ::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphFullMix, ReferenceFullMix);
	const ::IMAcousticMetaSoundNodesPrivate::FIMReferenceComparison SendComparison = ::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphSend, ExpectedSend);
	UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKReference energy graph_direct=%g graph_path=%g graph_wet=%g graph_full=%g ref_direct=%g ref_path=%g ref_wet=%g ref_full=%g stereo_asym=%g"),
		::IMAcousticMetaSoundNodesPrivate::StereoEnergy(GraphDirect), ::IMAcousticMetaSoundNodesPrivate::StereoEnergy(GraphPath), ::IMAcousticMetaSoundNodesPrivate::StereoEnergy(GraphWet), ::IMAcousticMetaSoundNodesPrivate::StereoEnergy(GraphFullMix),
		::IMAcousticMetaSoundNodesPrivate::StereoEnergy(ReferenceDirect), ::IMAcousticMetaSoundNodesPrivate::StereoEnergy(ReferencePath), ::IMAcousticMetaSoundNodesPrivate::StereoEnergy(ReferenceWet), ::IMAcousticMetaSoundNodesPrivate::StereoEnergy(ReferenceFullMix),
		::IMAcousticMetaSoundNodesPrivate::StereoChannelAsymmetry(ReferenceFullMix));
	const bool PositivePass = DirectComparison.Equal && PathComparison.Equal && WetComparison.Equal
		&& FullComparison.Equal && SendComparison.Equal;
	TestTrue(TEXT("Frozen direct stem matches SDK-only renderer"), DirectComparison.Equal);
	TestTrue(TEXT("Frozen path stem matches SDK-only renderer"), PathComparison.Equal);
	TestTrue(TEXT("Frozen wet stem matches SDK-only renderer"), WetComparison.Equal);
	TestTrue(TEXT("Frozen full mix matches SDK-only renderer"), FullComparison.Equal);
	TestTrue(TEXT("Frozen environmental send uses the declared gain"), SendComparison.Equal);
	UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundSDKReference stems direct[%s] path[%s] wet[%s] full[%s] send[%s]"),
		*DirectComparison.Summary, *PathComparison.Summary, *WetComparison.Summary,
		*FullComparison.Summary, *SendComparison.Summary);

	TArray<float> WrongGainMix = ReferenceFullMix;
	TArray<float> SwappedMix = ReferenceFullMix;
	TArray<float> DuplicateWetMix = ReferenceSourceFull;
	for (int32 I = 0; I < ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames; ++I)
	{
		WrongGainMix[2 * I] *= 0.5f; WrongGainMix[2 * I + 1] *= 0.5f;
		SwappedMix[2 * I] = ReferenceFullMix[2 * I + 1];
		SwappedMix[2 * I + 1] = ReferenceFullMix[2 * I];
		DuplicateWetMix[2 * I] += 2.0f * ReferenceWet[2 * I];
		DuplicateWetMix[2 * I + 1] += 2.0f * ReferenceWet[2 * I + 1];
	}
	const bool WrongGainRejected = !::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphFullMix, WrongGainMix).Equal;
	const bool ChannelsRejected = !::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphFullMix, SwappedMix).Equal;
	const bool DuplicateWetRejected = !::IMAcousticMetaSoundNodesPrivate::CompareStereo(GraphFullMix, DuplicateWetMix).Equal;
	TestTrue(TEXT("Known wrong gain is rejected"), WrongGainRejected);
	TestTrue(TEXT("Known left/right swap is rejected"), ChannelsRejected);
	TestTrue(TEXT("Known duplicated wet is rejected"), DuplicateWetRejected);
	TestTrue(TEXT("Wet negative control is discriminating"), ::IMAcousticMetaSoundNodesPrivate::StereoEnergy(ReferenceWet) > 1.0e-10f);
	TestTrue(TEXT("Stereo negative control is discriminating"), ::IMAcousticMetaSoundNodesPrivate::StereoChannelAsymmetry(ReferenceFullMix) > 1.0e-5f);

	FString IdentityError;
	const bool AudioIdMismatchRejected = RunIdentityNegative(TEXT("audio_id"), TestAudioId + 1, TestEpoch, false,
		FrozenFrame, 1, 128u, IdentityError);
	const bool WorldMismatchRejected = RunIdentityNegative(TEXT("world_generation"), TestAudioId, TestEpoch + 1, false,
		FrozenFrame, 1, 32u, IdentityError);
	const bool VoiceGenerationMismatchRejected = RunIdentityNegative(TEXT("voice_generation"), TestAudioId, TestEpoch, true,
		FrozenFrame, 1, 64u, IdentityError);
	Context->ListenerSpatialVersion.store(2, std::memory_order_release);
	const bool FrozenASpatialMismatchRejected = RunIdentityNegative(TEXT("frozen_a_spatial_version"), TestAudioId, TestEpoch, false,
		FrozenAFrame, 1, 2048u, IdentityError);
	Context->ListenerSpatialVersion.store(1, std::memory_order_release);
	const bool IdentityNegativePass = AudioIdMismatchRejected && WorldMismatchRejected
		&& VoiceGenerationMismatchRejected && FrozenASpatialMismatchRejected;
	TestTrue(TEXT("Mismatched audio-component identity is rejected"), AudioIdMismatchRejected);
	TestTrue(TEXT("Mismatched world identity is rejected"), WorldMismatchRejected);
	TestTrue(TEXT("Mismatched voice generation is rejected"), VoiceGenerationMismatchRejected);
	TestTrue(TEXT("Same-valid-identity frozen-A spatial response is rejected at B"), FrozenASpatialMismatchRejected);
	if (!IdentityNegativePass && !IdentityError.IsEmpty()) AddError(IdentityError);

	FString EvidenceJSON;
	auto Writer = TJsonWriterFactory<>::Create(&EvidenceJSON);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("status"), PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass
		? TEXT("PASS_S2_SDK_REFERENCE") : TEXT("FAIL_S2_SDK_REFERENCE"));
	Writer->WriteValue(TEXT("sample_rate_hz"), ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate);
	Writer->WriteValue(TEXT("graph_block_frames"), ::IMAcousticMetaSoundNodesPrivate::ReferenceGraphFrames);
	Writer->WriteValue(TEXT("source_to_environment_lag_frames"), int32(::IMAcousticMetaSoundNodesPrivate::ReferenceSourceToEnvironmentLagFrames));
	Writer->WriteValue(TEXT("source_to_environment_lag_ms"), 1000.0 * ::IMAcousticMetaSoundNodesPrivate::ReferenceSourceToEnvironmentLagFrames / ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate);
	Writer->WriteValue(TEXT("source_operator_input_delay_blocks"), int32(FIMAcousticMetaSoundContext::SourceInputDelayBlocks));
	Writer->WriteValue(TEXT("source_operator_input_delay_frames"), int32(FIMAcousticMetaSoundContext::SourceInputDelayFrames));
	Writer->WriteValue(TEXT("source_operator_input_delay_ms"),
		1000.0 * FIMAcousticMetaSoundContext::SourceInputDelayFrames / ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate);
	Writer->WriteValue(TEXT("environment_expected_spatial_version_delay_blocks"),
		int32(FIMAcousticMetaSoundContext::SourceInputDelayBlocks));
	Writer->WriteValue(TEXT("source_input_to_environment_total_delay_frames"),
		int32(::IMAcousticMetaSoundNodesPrivate::ReferenceSourceToEnvironmentLagFrames + FIMAcousticMetaSoundContext::SourceInputDelayFrames));
	Writer->WriteValue(TEXT("source_input_to_environment_total_delay_ms"),
		1000.0 * (::IMAcousticMetaSoundNodesPrivate::ReferenceSourceToEnvironmentLagFrames + FIMAcousticMetaSoundContext::SourceInputDelayFrames)
			/ ::IMAcousticMetaSoundNodesPrivate::ReferenceSampleRate);
	Writer->WriteValue(TEXT("channels"), TEXT("stereo_interleaved_LR"));
	Writer->WriteValue(TEXT("input_gain"), ::IMAcousticMetaSoundNodesPrivate::ReferenceInputGain);
	Writer->WriteValue(TEXT("send_gain"), ExpectedSendGain);
	Writer->WriteValue(TEXT("reverb_send_distance_gain"), FrozenReverbSendDistanceGain);
	Writer->WriteValue(TEXT("reverb_send_near_distance_m"), IMAcousticRecipe::ReverbSendNearDistanceM);
	Writer->WriteValue(TEXT("reverb_send_far_distance_m"), IMAcousticRecipe::ReverbSendFarDistanceM);
	Writer->WriteValue(TEXT("wet_gain"), ::IMAcousticMetaSoundNodesPrivate::ReferenceWetGain);
	Writer->WriteValue(TEXT("absolute_tolerance"), ::IMAcousticMetaSoundNodesPrivate::ReferenceAbsoluteTolerance);
	Writer->WriteValue(TEXT("relative_tolerance"), ::IMAcousticMetaSoundNodesPrivate::ReferenceRelativeTolerance);
	Writer->WriteValue(TEXT("bake_asset"), BakePath);
	Writer->WriteValue(TEXT("pcm_input"), PCMPath);
	Writer->WriteValue(TEXT("pcm_offset_frames"), WindowStartFrame);
	Writer->WriteValue(TEXT("pcm_declared_offset_frames"), int32(::IMAcousticMetaSoundNodesPrivate::ReferenceSourceToEnvironmentLagFrames));
	Writer->WriteValue(TEXT("listener_candidate"), SelectedListenerName);
	Writer->WriteObjectStart(TEXT("source_ue_cm"));
	Writer->WriteValue(TEXT("x"), SourceUE.X); Writer->WriteValue(TEXT("y"), SourceUE.Y); Writer->WriteValue(TEXT("z"), SourceUE.Z);
	Writer->WriteObjectEnd();
	Writer->WriteObjectStart(TEXT("listener_ue_cm"));
	Writer->WriteValue(TEXT("x"), ListenerUE.X); Writer->WriteValue(TEXT("y"), ListenerUE.Y); Writer->WriteValue(TEXT("z"), ListenerUE.Z);
	Writer->WriteObjectEnd();
	Writer->WriteObjectStart(TEXT("listener_sdk_m"));
	Writer->WriteValue(TEXT("x"), ListenerSpace.origin.x); Writer->WriteValue(TEXT("y"), ListenerSpace.origin.y); Writer->WriteValue(TEXT("z"), ListenerSpace.origin.z);
	Writer->WriteObjectEnd();
	Writer->WriteObjectStart(TEXT("positive_controls"));
	Writer->WriteValue(TEXT("direct"), DirectComparison.Equal);
	Writer->WriteValue(TEXT("path"), PathComparison.Equal);
	Writer->WriteValue(TEXT("wet"), WetComparison.Equal);
	Writer->WriteValue(TEXT("full_mix"), FullComparison.Equal);
	Writer->WriteValue(TEXT("send"), SendComparison.Equal);
	Writer->WriteObjectEnd();
	Writer->WriteObjectStart(TEXT("negative_controls"));
	Writer->WriteValue(TEXT("wrong_gain_rejected"), WrongGainRejected);
	Writer->WriteValue(TEXT("left_right_swap_rejected"), ChannelsRejected);
	Writer->WriteValue(TEXT("duplicated_wet_rejected"), DuplicateWetRejected);
	Writer->WriteObjectStart(TEXT("identity_mismatch"));
	Writer->WriteValue(TEXT("audio_component_id_rejected"), AudioIdMismatchRejected);
	Writer->WriteValue(TEXT("world_generation_rejected"), WorldMismatchRejected);
	Writer->WriteValue(TEXT("voice_generation_rejected"), VoiceGenerationMismatchRejected);
	Writer->WriteValue(TEXT("frozen_a_spatial_version_rejected"), FrozenASpatialMismatchRejected);
	Writer->WriteValue(TEXT("frozen_a_spatial_version_expected"), 2.0);
	Writer->WriteValue(TEXT("frozen_a_spatial_version_injected"), 1.0);
	Writer->WriteObjectStart(TEXT("frozen_a_listener_sdk_m"));
	Writer->WriteValue(TEXT("x"), FrozenASpace.origin.x); Writer->WriteValue(TEXT("y"), FrozenASpace.origin.y); Writer->WriteValue(TEXT("z"), FrozenASpace.origin.z);
	Writer->WriteObjectEnd();
	Writer->WriteObjectStart(TEXT("b_listener_sdk_m"));
	Writer->WriteValue(TEXT("x"), FrozenBSpace.origin.x); Writer->WriteValue(TEXT("y"), FrozenBSpace.origin.y); Writer->WriteValue(TEXT("z"), FrozenBSpace.origin.z);
	Writer->WriteObjectEnd();
	Writer->WriteValue(TEXT("frozen_response_fail_closed"), IdentityNegativePass);
	Writer->WriteObjectEnd();
	Writer->WriteObjectEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	const FString EvidencePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/MetaSoundClosure/sdk-reference-comparison.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(EvidencePath), true);
	if (!FFileHelper::SaveStringToFile(EvidenceJSON, *EvidencePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		AddError(FString::Printf(TEXT("Could not save SDK reference evidence: %s"), *EvidencePath));

	IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSound SDK reference comparison"),
		(PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass) ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSoundSDKReference %s"),
		(PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass) ? TEXT("PASS") : TEXT("FAIL"));
	return !HasAnyErrors() && PositivePass && WrongGainRejected && ChannelsRejected && DuplicateWetRejected && IdentityNegativePass;
}
#endif
