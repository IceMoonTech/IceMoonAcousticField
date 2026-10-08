#include "IMAcousticReverbPreset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticReverbData.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticSpatialization.h"
#include "HAL/PlatformTime.h"

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
			FMemory::Memzero(Dry.GetData(),Dry.Num()*sizeof(float));
			for(auto& Voice:Device->Voices)Voice->MixDry(Dry.GetData(),Device->BlockFrames,Pool->WorldGeneration,Now);
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
			FMemory::Memzero(Dry.GetData(),Dry.Num()*sizeof(float));
			for(auto& Voice:Device->Voices)Voice->MixDry(Dry.GetData(),Device->BlockFrames,Pool->WorldGeneration,Now);
		}
		// SDK Apply no longer receives the retired TripleBuffer shell. Its old
		// convolution history is owned by the effect; the worker may reuse source.
		if(Retired)Retired->State.store(EIMAcousticIRState::Free,std::memory_order_release);
	}
private:
	void ReleaseCurrent(){if(Current){Current->State.store(EIMAcousticIRState::Free,std::memory_order_release);Current=nullptr;}}
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Device;
	TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> Pool;
	TUniquePtr<FIMAcousticReverbRenderer> Renderer;
	TArray<float> Dry,Wet;
	FIMAcousticReverbSlot* Current=nullptr;
	float Gain;
};

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
	check(IsInGameThread());return new FIMAcousticSubmixEffect(BoundDevice,BoundPool,BoundWetGain);
}
