#include "IMAcousticReverbPreset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticReverbData.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticSpatialization.h"
#include "HAL/PlatformTime.h"
#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "AudioParameter.h"
#include "IMAcousticMetaSound.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "MetasoundGenerator.h"
#include "MetasoundAssetManager.h"
#include "MetasoundGraph.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundSource.h"
#include "Misc/ScopeExit.h"
#include "Sound/AudioBus.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#endif

namespace IMAcousticReverbPresetPrivate
{
struct FIMAcousticEnvironmentHostBlock
{
	double AudioClock = 0.0, PreAudioClock = 0.0, PreSeconds = 0.0, BeginSeconds = 0.0, EndSeconds = 0.0;
	uint64 PushesBeforeSources = 0, PushesBeforeReaders = 0, PushesAfterReaders = 0;
	uint64 UnderrunsBefore = 0, UnderrunsAfter = 0;
	uint32 BusBegin = 0, BusEnd = 0;
	int32 NativeFrames = 0, PreNativeFrames = 0, GeneratedBlocks = 0, ShortReturn = 0;
	uint32 Fault = 0;
};

}

namespace IMAcousticReverbPresetPrivate
{
enum class EIMAcousticEnvironmentHostFault : uint32
{
	None, NativeIdentity, OutputShape, ShortReturn, ConcurrentCallback, ProducerDuringReaders
};

}

struct FIMAcousticEnvironmentHost final
{
	static constexpr uint32 ActiveCallback = 1, ClosedCallbacks = 2;
	ISoundGeneratorPtr Generator;
	FIMAcousticMetaSoundContextPtr Context;
	FAudioDeviceHandle DeviceHandle;
	FDelegateHandle PreRenderHandle;
	FEvent* Idle = nullptr;
	uint64 GeneratorInstanceId = 0, WorldGeneration = 0;
	uint32 DeviceId = 0, SampleRate = 0;
	int32 NativeFrames = 0;
	std::atomic<uint32> CallbackGate{0}, Fault{0}, PublishedBlocks{0};
	std::atomic<uint64> ProcessedBlocks{0}, GeneratedBlocks{0};
	std::atomic<bool> CaptureOverflow{false}, GeneratorReleased{false}, PreRenderRemoved{false};
	TArray<IMAcousticReverbPresetPrivate::FIMAcousticEnvironmentHostBlock> Capture;
	double PreAudioClock = -1.0, PreSeconds = 0.0;
	uint64 PrePushes = 0;
	int32 PreNativeFrames = 0;
	bool BeganGenerate = false, OwnershipReleased = false;

	FIMAcousticEnvironmentHost(ISoundGeneratorPtr InGenerator, FIMAcousticMetaSoundContextPtr InContext,
		uint64 InInstance, uint32 InRate, int32 InFrames)
		: Generator(MoveTemp(InGenerator)), Context(MoveTemp(InContext)), GeneratorInstanceId(InInstance),
			WorldGeneration(Context->Pool ? Context->Pool->WorldGeneration : 0), DeviceId(Context->DeviceId),
			SampleRate(InRate), NativeFrames(InFrames)
	{
		check(IsInGameThread());
		Idle = FPlatformProcess::GetSynchEventFromPool(false);
		if (!Context->CapturedBusReads.IsEmpty()) Capture.SetNum(4096);
	}

	~FIMAcousticEnvironmentHost()
	{
		check(!Generator && !PreRenderHandle.IsValid());
		FPlatformProcess::ReturnSynchEventToPool(Idle);
	}

	void RecordPre(double Clock, int32 Frames)
	{
		if (CallbackGate.load(std::memory_order_acquire) & ClosedCallbacks) return;
		PreAudioClock = Clock;
		PreNativeFrames = Frames;
		PreSeconds = FPlatformTime::Seconds();
		PrePushes = Context->SourceSendPushBlocks.load(std::memory_order_acquire);
	}

	void Publish(IMAcousticReverbPresetPrivate::FIMAcousticEnvironmentHostBlock& Row)
	{
		Row.EndSeconds = FPlatformTime::Seconds();
		Row.BusEnd = Context->CapturedBusReadCount.load(std::memory_order_acquire);
		Row.PushesAfterReaders = Context->SourceSendPushBlocks.load(std::memory_order_acquire);
		Row.UnderrunsAfter = Context->BusUnderruns.load(std::memory_order_acquire);
		Row.Fault = Fault.load(std::memory_order_acquire);
		ProcessedBlocks.fetch_add(1, std::memory_order_relaxed);
		if (Capture.IsEmpty()) return;
		const uint32 Index = PublishedBlocks.load(std::memory_order_relaxed);
		if (Index < uint32(Capture.Num()))
		{
			Capture[int32(Index)] = Row;
			PublishedBlocks.store(Index + 1, std::memory_order_release);
		}
		else CaptureOverflow.store(true, std::memory_order_release);
	}

	bool Render(const FSoundEffectSubmixInputData& In, FSoundEffectSubmixOutputData& Out,
		IMAcousticReverbPresetPrivate::FIMAcousticEnvironmentHostBlock& Row)
	{
		if (In.NumFrames != NativeFrames || In.NumFrames != PreNativeFrames || In.AudioClock != PreAudioClock
			|| PreSeconds <= 0.0 || PreSeconds > Row.BeginSeconds)
		{
			Fault.store(uint32(IMAcousticReverbPresetPrivate::EIMAcousticEnvironmentHostFault::NativeIdentity), std::memory_order_release);
			return false;
		}
		if (Out.NumChannels != 2 || Out.AudioBuffer->Num() != In.NumFrames * 2)
		{
			Fault.store(uint32(IMAcousticReverbPresetPrivate::EIMAcousticEnvironmentHostFault::OutputShape), std::memory_order_release);
			return false;
		}
		if (!BeganGenerate) { Generator->OnBeginGenerate(); BeganGenerate = true; }
		constexpr int32 BlockSamples = FIMAcousticMetaSoundContext::Frames * 2;
		for (int32 Offset = 0; Offset < Out.AudioBuffer->Num(); Offset += BlockSamples)
		{
			const int32 Returned = Generator->GetNextBuffer(Out.AudioBuffer->GetData() + Offset, BlockSamples, false);
			if (Returned != BlockSamples)
			{
				Row.ShortReturn = Returned;
				Fault.store(uint32(IMAcousticReverbPresetPrivate::EIMAcousticEnvironmentHostFault::ShortReturn), std::memory_order_release);
				return false;
			}
			++Row.GeneratedBlocks;
			GeneratedBlocks.fetch_add(1, std::memory_order_relaxed);
		}
		if (Context->SourceSendPushBlocks.load(std::memory_order_acquire) != Row.PushesBeforeReaders)
		{
			Fault.store(uint32(IMAcousticReverbPresetPrivate::EIMAcousticEnvironmentHostFault::ProducerDuringReaders), std::memory_order_release);
			return false;
		}
		return true;
	}

	void Process(const FSoundEffectSubmixInputData& In, FSoundEffectSubmixOutputData& Out, uint32 InDeviceId)
	{
		if (!Out.AudioBuffer) return;
		uint32 Expected = 0;
		if (!CallbackGate.compare_exchange_strong(Expected, ActiveCallback, std::memory_order_acq_rel))
		{
			if (!(Expected & ClosedCallbacks)) Fault.store(uint32(IMAcousticReverbPresetPrivate::EIMAcousticEnvironmentHostFault::ConcurrentCallback), std::memory_order_release);
			FMemory::Memzero(Out.AudioBuffer->GetData(), Out.AudioBuffer->Num() * sizeof(float));
			return;
		}
		ON_SCOPE_EXIT
		{
			if (CallbackGate.fetch_sub(ActiveCallback, std::memory_order_acq_rel) & ClosedCallbacks) Idle->Trigger();
		};
		if (!Generator || Context->Stopped.load(std::memory_order_acquire))
		{
			FMemory::Memzero(Out.AudioBuffer->GetData(), Out.AudioBuffer->Num() * sizeof(float));
			return;
		}
		IMAcousticReverbPresetPrivate::FIMAcousticEnvironmentHostBlock Row;
		Row.AudioClock = In.AudioClock; Row.NativeFrames = In.NumFrames;
		Row.PreAudioClock = PreAudioClock; Row.PreNativeFrames = PreNativeFrames;
		Row.PreSeconds = PreSeconds; Row.BeginSeconds = FPlatformTime::Seconds();
		Row.PushesBeforeSources = PrePushes;
		Row.PushesBeforeReaders = Context->SourceSendPushBlocks.load(std::memory_order_acquire);
		Row.BusBegin = Context->CapturedBusReadCount.load(std::memory_order_acquire);
		Row.UnderrunsBefore = Context->BusUnderruns.load(std::memory_order_acquire);
		const bool IdentityValid = InDeviceId == DeviceId;
		if (!IdentityValid) Fault.store(uint32(IMAcousticReverbPresetPrivate::EIMAcousticEnvironmentHostFault::NativeIdentity), std::memory_order_release);
		const bool Complete = IdentityValid && Fault.load(std::memory_order_acquire) == 0 && Render(In, Out, Row);
		if (!Complete)
		{
			Context->InvalidBlocks.fetch_add(1, std::memory_order_relaxed);
			FMemory::Memzero(Out.AudioBuffer->GetData(), Out.AudioBuffer->Num() * sizeof(float));
		}
		Publish(Row);
	}

	void Close()
	{
		check(IsInGameThread());
		const uint32 Previous = CallbackGate.fetch_or(ClosedCallbacks, std::memory_order_acq_rel);
		if (PreRenderHandle.IsValid())
		{
			check(DeviceHandle.IsValid());
			PreRenderRemoved.store(DeviceHandle->RemovePreRenderDelegate(PreRenderHandle), std::memory_order_release);
			PreRenderHandle.Reset();
		}
		else PreRenderRemoved.store(true, std::memory_order_release);
		if (Previous & ActiveCallback) Idle->Wait();
	}

	void Release()
	{
		check(IsInGameThread());
		check(CallbackGate.load(std::memory_order_acquire) == ClosedCallbacks);
		check(!Context || Context->Stopped.load(std::memory_order_acquire));
		if (Generator && BeganGenerate) Generator->OnEndGenerate();
		Generator.Reset();
		OwnershipReleased = !Context || (!Context->EnvironmentConsumer.load(std::memory_order_acquire)
			&& !Context->BusReaderConsumer.load(std::memory_order_acquire));
		GeneratorReleased.store(true, std::memory_order_release);
		Context.Reset();
		DeviceHandle.Reset();
	}
};

namespace IMAcousticReverbPresetPrivate
{
ISoundGeneratorPtr IM_CreateEnvironmentGenerator(UMetaSoundSource* Source, UAudioBus* Bus,
	const FIMAcousticMetaSoundContextPtr& Context, uint64 InstanceId, int32 NativeFrames, FString& OutError)
{
	check(IsInGameThread());
	if (!Source || !Bus || !Context || Source->IsDynamic() || Context->Stopped.load(std::memory_order_acquire))
	{ OutError = TEXT("Environment requires an original constant source and a live owned context."); return nullptr; }
	const auto Settings = Source->GetOperatorSettings(float(Context->Device->SampleRate));
	const auto OutputNames = Source->GetOutputAudioChannelOrder();
	if (Settings.GetNumFramesPerBlock() != FIMAcousticMetaSoundContext::Frames || OutputNames.Num() != 2
		|| NativeFrames <= 0 || NativeFrames % FIMAcousticMetaSoundContext::Frames != 0)
	{ OutError = TEXT("Environment graph/native shape does not preserve 512-frame stereo blocks."); return nullptr; }
	Metasound::Frontend::FMetaSoundAssetRegistrationOptions Registration;
	Registration.bForceReregister = false;
	Registration.bAutoUpdate = false;
#if WITH_EDITOR
	Registration.bForceViewSynchronization = false;
#endif
	Source->UpdateAndRegisterForExecution(Registration);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound environment_resources_before_parameters source=%s instance=%llu on_gt=%d"),
		*Source->GetPathName(), InstanceId, IsInGameThread());
	TArray<FAudioParameter> Parameters{FAudioParameter(TEXT("Acoustic Bus"), Bus)};
	Source->InitParameters(Parameters, TEXT("IM_AcousticEnvironmentHost"));
	if (Parameters.Num() != 1 || Parameters[0].ObjectProxies.Num() != 1 || !Parameters[0].ObjectProxies[0])
	{ OutError = TEXT("Original Acoustic Bus parameter did not produce its initialized proxy."); return nullptr; }
	const auto Graph = Metasound::Frontend::INodeClassRegistry::Get()->GetGraph(Source->GetGraphRegistryKey());
	if (!Graph)
	{ OutError = TEXT("Original registered environment graph is unavailable on GT."); return nullptr; }
	FSoundGeneratorInitParams Sound;
	Sound.AudioDeviceID = Context->DeviceId; Sound.InstanceID = InstanceId;
	Sound.SampleRate = float(Context->Device->SampleRate); Sound.AudioMixerNumOutputFrames = NativeFrames;
	Sound.NumChannels = 2; Sound.NumFramesPerCallback = FIMAcousticMetaSoundContext::Frames;
	Metasound::FGeneratorInitParams Init{Settings};
	Init.BuilderSettings = Metasound::FOperatorBuilderSettings::GetDefaultSettings();
	Init.Graph = Graph; Init.Environment = Source->CreateEnvironment(Sound);
	Init.AudioOutputNames = OutputNames; Init.DefaultParameters = MoveTemp(Parameters);
	Init.bBuildSynchronous = true; Init.ClassName = Source->GetClass()->GetFName();
	Init.AssetPath = FTopLevelAssetPath(Source);
	TSharedPtr<Metasound::FMetasoundConstGraphGenerator, ESPMode::ThreadSafe> Generator =
		MakeShared<Metasound::FMetasoundConstGraphGenerator, ESPMode::ThreadSafe>(MoveTemp(Init));
	if (Generator->GetNumChannels() != 2 || Generator->GetDesiredNumSamplesToRenderPerCallback() != 1024
		|| !Context->EnvironmentConsumer.load(std::memory_order_acquire) || !Context->BusReaderConsumer.load(std::memory_order_acquire))
	{
		OutError = TEXT("Synchronous original graph did not become ready with its unique environment/BusReader owners.");
		IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
		Generator.Reset();
		return nullptr;
	}
	return Generator;
}

}

namespace IMAcousticReverbPresetPrivate
{
class FIMAcousticEnvironmentSubmixEffect final : public FSoundEffectSubmix
{
public:
	explicit FIMAcousticEnvironmentSubmixEffect(TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe> InHost)
		: Host(MoveTemp(InHost)) {}
	void Init(const FSoundEffectSubmixInitData& In) override
	{
		check(IsInGameThread());
		DeviceId = In.DeviceID;
		if (DeviceId != Host->DeviceId || In.SampleRate != Host->SampleRate)
			Host->Fault.store(uint32(EIMAcousticEnvironmentHostFault::NativeIdentity), std::memory_order_release);
	}
	void OnPresetChanged() override {}
	uint32 GetDesiredInputChannelCountOverride() const override { return 2; }
	void OnProcessAudio(const FSoundEffectSubmixInputData& In, FSoundEffectSubmixOutputData& Out) override
	{ Host->Process(In, Out, DeviceId); }
private:
	TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe> Host;
	uint32 DeviceId = 0;
};

}

namespace IMAcousticReverbPresetPrivate
{
struct FIMAcousticReverbTraceScope final
{
	FIMAcousticDeviceBridge* Bridge = nullptr;
	uint64 AudioBlock = 0;
	double StartSeconds = 0.0;
	bool CallbackTrace = false;
	bool RareTail = false;
	uint32 Outcome = 0;
	uint8 Fresh = 0;
	uint8 Rendered = 0;

	~FIMAcousticReverbTraceScope()
	{
		if (!Bridge) return;
		const double EndSeconds = FPlatformTime::Seconds();
		if (RareTail) Bridge->RecordRareTailReverb(AudioBlock, Outcome, Fresh, Rendered, StartSeconds, EndSeconds);
		if (CallbackTrace)
		{
			FIMAcousticReverbBlockProbe Probe{};
			Probe.AudioBlock = AudioBlock;
			Probe.Outcome = Outcome;
			Probe.Fresh = Fresh;
			Probe.Rendered = Rendered;
			Probe.StartSeconds = StartSeconds;
			Probe.EndSeconds = EndSeconds;
			Bridge->TraceReverbBlock(Probe);
		}
	}
};

}

namespace IMAcousticReverbPresetPrivate
{
class FIMAcousticSubmixEffect final : public FSoundEffectSubmix
{
public:
	FIMAcousticSubmixEffect(TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> InDevice,
		TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> InPool,float InGain)
		:Device(MoveTemp(InDevice)),Pool(MoveTemp(InPool)),Gain(InGain)
	{if(Device)Device->ReverbEffectInstances.fetch_add(1,std::memory_order_relaxed);}
	~FIMAcousticSubmixEffect() override
	{
		Renderer.Reset();ReleaseCurrent();
		if(Device)Device->ReverbEffectInstances.fetch_sub(1,std::memory_order_relaxed);
	}
	void Init(const FSoundEffectSubmixInitData& In) override
	{
		check(IsInGameThread());
		if(!Device||!Pool||In.SampleRate!=Device->SampleRate)return;
		IPLContext Context=IMAcousticSDKContext::GetAcousticSDKContext();if(!Context)return;
		IPLAudioSettings Audio{int(Device->SampleRate),int(Device->BlockFrames)};
		IPLHRTFSettings Settings{};Settings.type=IPL_HRTFTYPE_DEFAULT;Settings.volume=1;
		IPLHRTF HRTF=nullptr;
		if(iplHRTFCreate(Context,&Audio,&Settings,&HRTF)!=IPL_STATUS_SUCCESS)return;
		Renderer=MakeUnique<FIMAcousticReverbRenderer>();
		if(!Renderer->Initialize(Context,HRTF,Device->SampleRate,Device->BlockFrames,int(Device->SampleRate*IMAcousticRecipe::ReverbSavedDurationS)))Renderer.Reset();
		iplHRTFRelease(&HRTF);
		Dry.SetNumZeroed(Device->BlockFrames);Wet.SetNumZeroed(Device->BlockFrames*2);
	}
	void OnPresetChanged() override{} // All UObject data was captured by CreateNewEffect on GT.
	uint32 GetDesiredInputChannelCountOverride() const override{return 2;}
	void OnProcessAudio(const FSoundEffectSubmixInputData& In,FSoundEffectSubmixOutputData& Out) override
	{
		if(!Out.AudioBuffer)return;
		FMemory::Memzero(Out.AudioBuffer->GetData(),Out.AudioBuffer->Num()*sizeof(float));
		if(!Device||!Pool||!Renderer)return;
		FIMAcousticReverbTraceScope Trace;
		Trace.CallbackTrace=Device->CallbackDiagnosticsEnabled.load(std::memory_order_relaxed);
		Trace.RareTail=Device->RareTailDiagnosticsEnabled.load(std::memory_order_relaxed);
		if(Trace.CallbackTrace||Trace.RareTail)
		{
			Trace.Bridge=Device.Get();
			Trace.AudioBlock=Device->CallbackAudioBlock.load(std::memory_order_relaxed);
			Trace.StartSeconds=FPlatformTime::Seconds();
		}
		Device->ReverbProcessedBlocks.fetch_add(1,std::memory_order_relaxed);
		FIMAcousticTimingScope Timing(Device->ProfilingEnabled.load(std::memory_order_relaxed)?&Device->ReverbTiming:nullptr);
		const double Now=FPlatformTime::Seconds();
		if(!Device->Enabled.load(std::memory_order_acquire))
		{
			Trace.Outcome=1;
			Renderer->Reset();ReleaseCurrent();
			for(auto& Slot:Pool->Slots)
			{auto Expected=EIMAcousticIRState::Ready;Slot.State.compare_exchange_strong(Expected,EIMAcousticIRState::Free,std::memory_order_acq_rel);}
			DiscardDrySends(Now);
			return;
		}
		if(!Device->Alive.load(std::memory_order_acquire)||Pool->Stopped.load(std::memory_order_acquire)
			||Device->WorldGeneration.load(std::memory_order_acquire)!=Pool->WorldGeneration
			||In.NumFrames!=int32(Device->BlockFrames)||Out.NumChannels!=2
			||Out.AudioBuffer->Num()!=int32(Device->BlockFrames*2))
		{Trace.Outcome=2;Renderer->Reset();ReleaseCurrent();Device->RecordPressureReverbReject(1, FPlatformTime::Seconds());Device->ReverbRejectedBlocks.fetch_add(1);return;}
		FIMAcousticReverbSlot* Next=nullptr;
		for(auto& Slot:Pool->Slots)
		{
			auto Expected=EIMAcousticIRState::Ready;
			if(!Slot.State.compare_exchange_strong(Expected,EIMAcousticIRState::Reading,std::memory_order_acq_rel))continue;
			if((Current&&Slot.Sequence<=Current->Sequence)||(Next&&Slot.Sequence<=Next->Sequence))
			{Slot.State.store(EIMAcousticIRState::Free,std::memory_order_release);continue;}
			if(Next)Next->State.store(EIMAcousticIRState::Free,std::memory_order_release);
			Next=&Slot;
		}
		FIMAcousticReverbSlot* Retired=nullptr;
		if(Next){Retired=Current;Current=Next;}
		const bool Fresh=Current&&Now>=Current->CapturedSeconds&&Now-Current->CapturedSeconds<=.25;
		if(Fresh)
		{
			Trace.Fresh=1;
			FMemory::Memzero(Dry.GetData(),Dry.Num()*sizeof(float));
			bool DryAccepted=false;
			for(auto& Voice:Device->Voices)DryAccepted=Voice->MixDry(Dry.GetData(),Device->BlockFrames,Pool->WorldGeneration,Now)||DryAccepted;
			if(DryAccepted)Device->ReverbDryBlocks.fetch_add(1,std::memory_order_relaxed);
			FIMAcousticReverbMetrics Metrics;
			if(Renderer->Render(Dry.GetData(),Device->BlockFrames,Current->Params,Current->Listener,Wet.GetData(),&Metrics))
			{
				Trace.Outcome=3;Trace.Rendered=1;
				Current->Applied=true;
				if(Metrics.InputEnergy>0)Device->ReverbDryNonzeroBlocks.fetch_add(1,std::memory_order_relaxed);
				if(Metrics.AmbisonicsEnergy>0)Device->ReverbAmbisonicsNonzeroBlocks.fetch_add(1,std::memory_order_relaxed);
				double Energy=0,RawEnergy=0;
				// Audition mute retains the same convolution history for A/B
				// comparisons; it deliberately is not a CPU bypass switch.
				const float AudibleGain=(Device->RenderRoutes.load(std::memory_order_relaxed)&4)?Gain:0;
				for(int32 I=0;I<Wet.Num();++I){const float Sample=Wet[I]*AudibleGain;(*Out.AudioBuffer)[I]=Sample;Energy+=double(Sample)*Sample;RawEnergy+=double(Wet[I])*Wet[I];}
				if(RawEnergy>0)Device->ReverbRawNonzeroBlocks.fetch_add(1,std::memory_order_relaxed);
				if(Energy>0)Device->ReverbNonzeroBlocks.fetch_add(1,std::memory_order_relaxed);
			}
			else{Trace.Outcome=4;Renderer->Reset();ReleaseCurrent();Device->RecordPressureReverbReject(2, Now);Device->ReverbRejectedBlocks.fetch_add(1,std::memory_order_relaxed);}
		}
		else
		{
			Trace.Outcome=4;
			Renderer->Reset();ReleaseCurrent();Device->RecordPressureReverbReject(3, Now);Device->ReverbRejectedBlocks.fetch_add(1,std::memory_order_relaxed);Device->ReverbNotFreshBlocks.fetch_add(1,std::memory_order_relaxed);
			// Stale-IR drain: the voice dry queue is still pushed every block
			// while no wet output is produced. MixDry always advances the read
			// cursor (even when its 100ms slot check fails), so draining here
			// keeps the 2-slot ring from filling and cascading into
			// DryDroppedBlocks. Discarded sends never reach the wet bus: the
			// output buffer stays zeroed and no ReverbDryBlocks credit is taken.
			DiscardDrySends(Now);
		}
		// SDK Apply no longer receives the retired TripleBuffer shell. Its old
		// convolution history is owned by the effect; the worker may reuse source.
		if(Retired)Retired->State.store(EIMAcousticIRState::Free,std::memory_order_release);
	}
private:
	void DiscardDrySends(double Now)
	{
		FMemory::Memzero(Dry.GetData(),Dry.Num()*sizeof(float));
		for(auto& Voice:Device->Voices)Voice->MixDry(Dry.GetData(),Device->BlockFrames,Pool->WorldGeneration,Now);
	}
	void ReleaseCurrent(){if(Current){Current->State.store(EIMAcousticIRState::Free,std::memory_order_release);Current=nullptr;}}
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Device;
	TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> Pool;
	TUniquePtr<FIMAcousticReverbRenderer> Renderer;
	TArray<float> Dry,Wet;
	FIMAcousticReverbSlot* Current=nullptr;
	float Gain;
};

}

void UIMAcousticReverbPreset::Bind(TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Device,
	TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> Pool,float WetGain)
{
	check(IsInGameThread());BoundDevice=MoveTemp(Device);BoundPool=MoveTemp(Pool);
	BoundWetGain=FMath::IsFinite(WetGain)?FMath::Clamp(WetGain,0.f,1.f):0;
}
USoundEffectPreset* UIMAcousticReverbPreset::CreateNewPreset(UObject* Parent,FName Name,EObjectFlags Flags) const
{return NewObject<UIMAcousticReverbPreset>(Parent,Name,Flags);}
FSoundEffectBase* UIMAcousticReverbPreset::CreateNewEffect() const
{
	// UAudioMixerBlueprintLibrary::AddSubmixEffect creates the instance on GT
	// before enqueueing it to the mixer. No preset lookup occurs in processing.
	check(IsInGameThread());
	if (BoundHost) return new IMAcousticReverbPresetPrivate::FIMAcousticEnvironmentSubmixEffect(BoundHost);
	return new IMAcousticReverbPresetPrivate::FIMAcousticSubmixEffect(BoundDevice,BoundPool,BoundWetGain);
}

bool UIMAcousticReverbPreset::BindMetaSound(UMetaSoundSource* Source, UAudioBus* Bus,
	const FIMAcousticMetaSoundContextPtr& Context, const FAudioDeviceHandle& Device, FString& OutError)
{
	check(IsInGameThread());
	if (BoundHost || !Device.IsValid() || !Context || Device->DeviceID != Context->DeviceId)
	{ OutError = TEXT("Environment host ownership/device binding is invalid."); return false; }
	static uint64 LastEnvironmentGeneratorInstance = 0;
	if (LastEnvironmentGeneratorInstance == MAX_uint64)
	{ OutError = TEXT("Environment generator identity space is exhausted."); return false; }
	const uint64 InstanceId = ++LastEnvironmentGeneratorInstance;
	auto* Mixer = static_cast<Audio::FMixerDevice*>(Device.GetAudioDevice());
	auto Generator = IMAcousticReverbPresetPrivate::IM_CreateEnvironmentGenerator(Source, Bus, Context, InstanceId, Mixer->GetNumOutputFrames(), OutError);
	if (!Generator) return false;
	auto Host = MakeShared<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe>(MoveTemp(Generator), Context,
		InstanceId, Context->Device->SampleRate, Mixer->GetNumOutputFrames());
	Host->DeviceHandle = Device;
	Host->PreRenderHandle = Host->DeviceHandle->AddPreRenderDelegate(FOnAudioDevicePreRender::FDelegate::CreateLambda(
		[Host, Mixer](const FAudioDeviceRenderInfo& Info) { Host->RecordPre(Mixer->GetAudioClock(), Info.NumFrames); }));
	BoundHost = MoveTemp(Host);
	return true;
}

void UIMAcousticReverbPreset::StopMetaSoundHost()
{
	check(IsInGameThread());
	if (BoundHost) BoundHost->Close();
}

void UIMAcousticReverbPreset::ReleaseMetaSoundHost()
{
	check(IsInGameThread());
	if (BoundHost) BoundHost->Release();
}

void UIMAcousticReverbPreset::BeginDestroy()
{
	if (BoundHost && !BoundHost->GeneratorReleased.load(std::memory_order_acquire))
	{
		BoundHost->Close();
		IMAcousticMetaSound::StopAcousticMetaSoundContext(BoundHost->Context);
		BoundHost->Release();
	}
	Super::BeginDestroy();
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
TSharedPtr<FJsonObject> IM_ExportAcousticEnvironmentHostForTest(
	const TSharedPtr<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe>& Host, const FString& Directory)
{
	check(IsInGameThread());
	auto Result = MakeShared<FJsonObject>();
	const bool Closed = Host && Host->CallbackGate.load(std::memory_order_acquire) == FIMAcousticEnvironmentHost::ClosedCallbacks;
	Result->SetBoolField(TEXT("closed_and_quiesced"), Closed);
	if (!Closed) { Result->SetBoolField(TEXT("passed"), false); return Result; }
	const uint32 Count = Host->PublishedBlocks.load(std::memory_order_acquire);
	FString CSV = TEXT("ordinal,device_id,sample_rate,world_generation,generator_instance_id,audio_clock,pre_audio_clock,native_frames,pre_native_frames,pre_seconds,begin_seconds,end_seconds,pushes_before_sources,pushes_before_readers,pushes_after_readers,bus_begin,bus_end,underruns_before,underruns_after,generated_blocks,short_return,fault\n");
	bool Valid = Count > 0 && Count <= uint32(Host->Capture.Num()) && !Host->CaptureOverflow.load(std::memory_order_acquire);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const auto& Row = Host->Capture[int32(Index)];
		CSV += FString::Printf(TEXT("%u,%u,%u,%llu,%llu,%.17g,%.17g,%d,%d,%.17g,%.17g,%.17g,%llu,%llu,%llu,%u,%u,%llu,%llu,%d,%d,%u\n"),
			Index, Host->DeviceId, Host->SampleRate, Host->WorldGeneration, Host->GeneratorInstanceId,
			Row.AudioClock, Row.PreAudioClock, Row.NativeFrames, Row.PreNativeFrames, Row.PreSeconds, Row.BeginSeconds,
			Row.EndSeconds, Row.PushesBeforeSources, Row.PushesBeforeReaders, Row.PushesAfterReaders,
			Row.BusBegin, Row.BusEnd, Row.UnderrunsBefore, Row.UnderrunsAfter, Row.GeneratedBlocks, Row.ShortReturn, Row.Fault);
		Valid &= Row.AudioClock == Row.PreAudioClock && Row.NativeFrames == 1024 && Row.PreNativeFrames == 1024
			&& Row.GeneratedBlocks == 2 && Row.Fault == 0 && Row.ShortReturn == 0
			&& Row.PreSeconds <= Row.BeginSeconds && Row.BeginSeconds <= Row.EndSeconds
			&& Row.PushesBeforeSources <= Row.PushesBeforeReaders && Row.PushesBeforeReaders == Row.PushesAfterReaders
			&& Row.BusEnd == Row.BusBegin + 2 && Row.UnderrunsBefore == 0 && Row.UnderrunsAfter == 0;
	}
	const bool Saved = FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(Directory, TEXT("native_host_blocks.csv")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool Released = Host->GeneratorReleased.load(std::memory_order_acquire);
	const bool Removed = Host->PreRenderRemoved.load(std::memory_order_acquire);
	Result->SetNumberField(TEXT("device_id"), Host->DeviceId);
	Result->SetNumberField(TEXT("world_generation"), double(Host->WorldGeneration));
	Result->SetNumberField(TEXT("generator_instance_id"), double(Host->GeneratorInstanceId));
	Result->SetNumberField(TEXT("native_blocks"), Count);
	Result->SetNumberField(TEXT("processed_blocks"), double(Host->ProcessedBlocks.load(std::memory_order_acquire)));
	Result->SetNumberField(TEXT("generated_graph_blocks"), double(Host->GeneratedBlocks.load(std::memory_order_acquire)));
	Result->SetNumberField(TEXT("fault"), Host->Fault.load(std::memory_order_acquire));
	Result->SetBoolField(TEXT("all_native_rows_valid"), Valid);
	Result->SetBoolField(TEXT("generator_released"), Released);
	Result->SetBoolField(TEXT("owned_pre_render_delegate_removed"), Removed);
	Result->SetBoolField(TEXT("environment_bus_reader_ownership_released"), Host->OwnershipReleased);
	Result->SetBoolField(TEXT("csv_saved"), Saved);
	Result->SetStringField(TEXT("native_identity"), TEXT("actual device/AudioClock/NumFrames; ordinal is only capture row index"));
	Result->SetBoolField(TEXT("passed"), Valid && Saved && Released && Removed && Host->OwnershipReleased);
	return Result;
}

namespace IMAcousticReverbPresetPrivate
{
class FIMAcousticHostControlGenerator final : public ISoundGenerator
{
public:
	int32 Calls = 0, EndCalls = 0;
	bool Short = false;
	FEvent* Entered = nullptr;
	FEvent* Continue = nullptr;
	int32 GetNumChannels() const override { return 2; }
	int32 OnGenerateAudio(float* Output, int32 Samples) override
	{
		if (Entered && Calls == 0) { Entered->Trigger(); Continue->Wait(); }
		const int32 Written = Short ? Samples - 2 : Samples;
		for (int32 Index = 0; Index < Written; ++Index) Output[Index] = float(Calls * Samples + Index + 1) / 4096.0f;
		++Calls;
		return Written;
	}
	void OnEndGenerate() override { ++EndCalls; }
};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticEnvironmentHostControls, "IceMoon.AcousticField.EnvironmentHost.Controls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticEnvironmentHostControls::RunTest(const FString&)
{
	auto Context = MakeShared<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>();
	Context->DeviceId = 4294967200u;
	auto Generator = MakeShared<IMAcousticReverbPresetPrivate::FIMAcousticHostControlGenerator, ESPMode::ThreadSafe>();
	auto Host = MakeShared<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe>(Generator, Context, 1, 48000, 1024);
	Audio::FAlignedFloatBuffer Buffer; Buffer.SetNumUninitialized(2048);
	FSoundEffectSubmixInputData Input; Input.NumFrames = 1024; Input.NumChannels = 2; Input.AudioClock = 0.5;
	FSoundEffectSubmixOutputData Output; Output.NumChannels = 2; Output.AudioBuffer = &Buffer;
	Host->RecordPre(Input.AudioClock, Input.NumFrames);
	Host->Process(Input, Output, Context->DeviceId);
	bool Identical = Generator->Calls == 2;
	for (int32 Index = 0; Index < Buffer.Num(); ++Index) Identical &= Buffer[Index] == float(Index + 1) / 4096.0f;
	TestTrue(TEXT("Host forwards exactly two 512-frame stereo blocks without gain/delay/channel conversion"), Identical);
	Generator->Short = true; Input.AudioClock += 0.1;
	Host->RecordPre(Input.AudioClock, Input.NumFrames); Host->Process(Input, Output, Context->DeviceId);
	TestEqual(TEXT("Short generator return is an explicit fault, not padded success"), Host->Fault.load(), uint32(IMAcousticReverbPresetPrivate::EIMAcousticEnvironmentHostFault::ShortReturn));
	const int32 BeforeClose = Generator->Calls;
	Host->Close(); Context->Stopped.store(true, std::memory_order_release); Host->Release();
	Host->Process(Input, Output, Context->DeviceId);
	TestEqual(TEXT("Closed host cannot render or reuse generator"), Generator->Calls, BeforeClose);
	TestEqual(TEXT("Generator ends exactly once after quiescence"), Generator->EndCalls, 1);
	Host->Release();
	TestEqual(TEXT("Repeated release is idempotent"), Generator->EndCalls, 1);
	auto ClosingContext = MakeShared<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe>();
	ClosingContext->DeviceId = 4294967200u;
	auto ClosingGenerator = MakeShared<IMAcousticReverbPresetPrivate::FIMAcousticHostControlGenerator, ESPMode::ThreadSafe>();
	ClosingGenerator->Entered = FPlatformProcess::GetSynchEventFromPool(false);
	ClosingGenerator->Continue = FPlatformProcess::GetSynchEventFromPool(false);
	auto ClosingHost = MakeShared<FIMAcousticEnvironmentHost, ESPMode::ThreadSafe>(ClosingGenerator, ClosingContext, 2, 48000, 1024);
	ClosingHost->RecordPre(Input.AudioClock, Input.NumFrames);
	auto Active = Async(EAsyncExecution::Thread, [ClosingHost, &Input, &Output, ClosingContext]()
		{ ClosingHost->Process(Input, Output, ClosingContext->DeviceId); });
	const bool Entered = ClosingGenerator->Entered->Wait(5000);
	TestTrue(TEXT("In-flight control entered the exact processing gate"), Entered);
	auto Resume = Async(EAsyncExecution::Thread, [ClosingGenerator]()
		{ FPlatformProcess::Sleep(0.01f); ClosingGenerator->Continue->Trigger(); });
	ClosingHost->Close(); Active.Wait(); Resume.Wait();
	TestEqual(TEXT("GT close waits for both complete graph blocks before release"), ClosingGenerator->Calls, 2);
	ClosingContext->Stopped.store(true, std::memory_order_release); ClosingHost->Release();
	TestEqual(TEXT("In-flight graph ends only after its callback quiesces"), ClosingGenerator->EndCalls, 1);
	FPlatformProcess::ReturnSynchEventToPool(ClosingGenerator->Entered);
	FPlatformProcess::ReturnSynchEventToPool(ClosingGenerator->Continue);
	ClosingGenerator->Entered = nullptr; ClosingGenerator->Continue = nullptr;
	TStrongObjectPtr<UMetaSoundSource> Source(LoadObject<UMetaSoundSource>(nullptr, TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticEnvironment")));
	for (uint32 World = 1; World <= 2; ++World)
	{
		auto GraphContext = IMAcousticMetaSound::CreateAcousticMetaSoundContext(4294967199u, 48000, World);
		TStrongObjectPtr<UAudioBus> Bus(NewObject<UAudioBus>());
		Bus->AudioBusChannels = EAudioBusChannels::Mono;
		GraphContext->BusId = Bus->GetUniqueID(); GraphContext->SourceSendTransportEnabled = true;
		FString Error;
		auto Graph = IMAcousticReverbPresetPrivate::IM_CreateEnvironmentGenerator(Source.Get(), Bus.Get(), GraphContext, Bus->GetUniqueID(), 1024, Error);
		TestTrue(TEXT("Original constant graph becomes synchronously ready before any Execute"), Graph.IsValid());
		TestEqual(TEXT("Construction does not consume/prime a graph block"), GraphContext->EnvironmentBlocks.load(), uint64(0));
		if (!Graph) AddError(Error);
		IMAcousticMetaSound::StopAcousticMetaSoundContext(GraphContext); Graph.Reset();
		TestFalse(TEXT("Graph destruction releases unique environment owner"), GraphContext->EnvironmentConsumer.load());
		TestFalse(TEXT("Graph destruction releases unique BusReader owner"), GraphContext->BusReaderConsumer.load());
		TestFalse(TEXT("Stopped world registry entry is not recycled"), IMAcousticMetaSound::FindAcousticMetaSoundContext(4294967199u).IsValid());
	}
	const bool Passed = !HasAnyErrors();
	UE_LOG(LogTemp, Display, TEXT("IMLogs EnvironmentHost Controls status=%s original_graph_ready=1 host_output_bitwise_checked=1 lifecycle_checked=1; native order and hearing still require actual Play"), Passed ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] EnvironmentHost.Controls %s"), Passed ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor EnvironmentHost.Controls"));
	return Passed;
}
#endif
