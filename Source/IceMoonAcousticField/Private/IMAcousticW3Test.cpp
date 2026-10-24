#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticSpatialization.h"
#include "AudioMixerBlueprintLibrary.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioDevice.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Sound/SoundWaveProcedural.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticW3TestPrivate
{
constexpr int32 W3Voices=16,W3Rate=48000;
constexpr double W3MotionPeriod=0.05; // Match the product's 20 Hz GT snapshot cadence.
constexpr double W3Duration=600;
constexpr double W3RareTailNearBudgetFraction=0.90;
constexpr uint32 W3RareTailNearTailUs=20000;
constexpr uint32 W3RareTailSelfTestUs=10000;
struct FIMW3MemorySample
{
	uint64 UsedPhysical=0,UsedVirtual=0,AvailablePhysical=0,AvailableVirtual=0;
	uint64 PeakUsedPhysical=0,PeakUsedVirtual=0;
};
FIMW3MemorySample IMW3CaptureMemorySample()
{
	const FPlatformMemoryStats Stats=FPlatformMemory::GetStats();
	FIMW3MemorySample Sample;
	Sample.UsedPhysical=Stats.UsedPhysical;Sample.UsedVirtual=Stats.UsedVirtual;
	Sample.AvailablePhysical=Stats.AvailablePhysical;Sample.AvailableVirtual=Stats.AvailableVirtual;
	Sample.PeakUsedPhysical=Stats.PeakUsedPhysical;Sample.PeakUsedVirtual=Stats.PeakUsedVirtual;
	return Sample;
}
struct FIMW3VoiceBinding
{
	uint64 AudioComponentId=0;
	uint32 Slot=MAX_uint32;
	uint64 LiveGeneration=0;
};
struct FIMW3VoiceBudget
{
	FIMW3VoiceBinding Binding;
	uint64 ExecuteCount=0,MaxCycles=0;
};
bool IMW3BuildVoiceBindings(
	const FIMAcousticMetaSoundContextPtr& Context,
	const TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe>& Bridge,
	const TArray<uint64>& ExpectedAudioIds,
	TArray<FIMW3VoiceBinding>& OutBindings,
	FString& OutError)
{
	OutBindings.Reset();
	if(!Context.IsValid()||!Context->Device||Context->Device.Get()!=Bridge.Get())
	{OutError=TEXT("MetaSound context is unavailable or belongs to a different device bridge.");return false;}
	if(ExpectedAudioIds.Num()!=W3Voices)
	{OutError=FString::Printf(TEXT("Expected %d graph sources, found %d AudioComponentIDs."),W3Voices,ExpectedAudioIds.Num());return false;}
	TSet<uint64> UniqueIds;
	TSet<uint32> UniqueSlots;
	for(const uint64 AudioId:ExpectedAudioIds)
	{
		if(!AudioId||UniqueIds.Contains(AudioId))
		{OutError=TEXT("Graph sources do not have sixteen unique nonzero AudioComponentIDs.");return false;}
		UniqueIds.Add(AudioId);
		uint32 MatchedSlot=MAX_uint32;
		uint32 Matches=0;
		for(uint32 Slot=0;Slot<FIMAcousticMetaSoundContext::MaxVoices;++Slot)
		{
			if(Context->AudioIds[Slot].load(std::memory_order_acquire)==AudioId)
			{MatchedSlot=Slot;++Matches;}
		}
		if(Matches!=1||!Bridge->Voices.IsValidIndex(int32(MatchedSlot))||!Bridge->Voices[int32(MatchedSlot)])
		{OutError=FString::Printf(TEXT("AudioComponentID %llu maps to %u MetaSound slots or has no voice bridge."),AudioId,Matches);return false;}
		if(!Context->SourceConsumers[MatchedSlot].load(std::memory_order_acquire))
		{OutError=FString::Printf(TEXT("AudioComponentID %llu has no active source consumer in slot %u."),AudioId,MatchedSlot);return false;}
		if(UniqueSlots.Contains(MatchedSlot))
		{OutError=FString::Printf(TEXT("Multiple graph sources map to MetaSound slot %u."),MatchedSlot);return false;}
		UniqueSlots.Add(MatchedSlot);
		const uint64 Generation=Bridge->Voices[int32(MatchedSlot)]->LiveGeneration.load(std::memory_order_acquire);
		if(!Generation)
		{OutError=FString::Printf(TEXT("AudioComponentID %llu has no live voice generation in slot %u."),AudioId,MatchedSlot);return false;}
		OutBindings.Add({AudioId,MatchedSlot,Generation});
	}
	return OutBindings.Num()==W3Voices&&UniqueSlots.Num()==W3Voices;
}
bool IMW3VoiceBindingsStillCurrent(
	const FIMAcousticMetaSoundContextPtr& Context,
	const TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe>& Bridge,
	const TArray<FIMW3VoiceBinding>& Bindings,
	FString& OutError)
{
	if(!Context.IsValid()||!Context->Device||Context->Device.Get()!=Bridge.Get())
	{OutError=TEXT("MetaSound context or device bridge changed during the measured window.");return false;}
	for(const FIMW3VoiceBinding& Binding:Bindings)
	{
		if(Binding.Slot>=FIMAcousticMetaSoundContext::MaxVoices||!Bridge->Voices.IsValidIndex(int32(Binding.Slot))
			||!Bridge->Voices[int32(Binding.Slot)]
			||Context->AudioIds[Binding.Slot].load(std::memory_order_acquire)!=Binding.AudioComponentId
			||!Context->SourceConsumers[Binding.Slot].load(std::memory_order_acquire)
			||Bridge->Voices[int32(Binding.Slot)]->LiveGeneration.load(std::memory_order_acquire)!=Binding.LiveGeneration)
		{
			OutError=FString::Printf(TEXT("AudioComponentID %llu no longer owns slot %u generation %llu."),
				Binding.AudioComponentId,Binding.Slot,Binding.LiveGeneration);
			return false;
		}
	}
	return Bindings.Num()==W3Voices;
}
bool IMW3TimingsAreFresh(
	const FIMAcousticDeviceBridge& Bridge,
	const TArray<FIMW3VoiceBinding>& Bindings,
	FString& OutError)
{
	auto CheckTiming=[&OutError](const TCHAR* Name,const FIMAcousticTiming& Timing)
	{
		const uint64 Count=Timing.Count.load(std::memory_order_acquire);
		const uint64 MaxCycles=Timing.MaxCycles.load(std::memory_order_acquire);
		if(Count||MaxCycles)
		{
			OutError=FString::Printf(TEXT("Fresh-window timing %s was already nonzero (count=%llu max_cycles=%llu)."),Name,Count,MaxCycles);
			return false;
		}
		return true;
	};
	if(!CheckTiming(TEXT("source_callback"),Bridge.SourceTiming)
		||!CheckTiming(TEXT("single_source_execute_legacy_alias"),Bridge.SourceBlockTiming)
		||!CheckTiming(TEXT("reverb_callback"),Bridge.ReverbTiming)
		||!CheckTiming(TEXT("worker_update"),Bridge.WorkerTiming)
		||!CheckTiming(TEXT("gt_snapshot"),Bridge.GameThreadTiming)
		||!CheckTiming(TEXT("worker_snapshot"),Bridge.WorkerSnapshotTiming)
		||!CheckTiming(TEXT("worker_solve"),Bridge.WorkerSolveTiming)
		||!CheckTiming(TEXT("worker_reverb"),Bridge.WorkerReverbTiming)
		||!CheckTiming(TEXT("worker_publish"),Bridge.WorkerPublishTiming)
		||!CheckTiming(TEXT("gt_traverse"),Bridge.GTTraverseTiming)
		||!CheckTiming(TEXT("gt_submit"),Bridge.GTSubmitTiming))return false;
	for(const FIMW3VoiceBinding& Binding:Bindings)
	{
		if(!Bridge.Voices.IsValidIndex(int32(Binding.Slot))||!Bridge.Voices[int32(Binding.Slot)])
		{OutError=FString::Printf(TEXT("Fresh-window source slot %u has no voice bridge."),Binding.Slot);return false;}
		const auto& Voice=*Bridge.Voices[int32(Binding.Slot)];
		const uint64 Count=Voice.SourceExecuteCount.load(std::memory_order_acquire);
		const uint64 MaxCycles=Voice.SourceExecuteMaxCycles.load(std::memory_order_acquire);
		if(Count||MaxCycles)
		{
			OutError=FString::Printf(TEXT("Fresh-window source slot %u AudioComponentID %llu generation %llu was already nonzero (count=%llu max_cycles=%llu)."),
				Binding.Slot,Binding.AudioComponentId,Binding.LiveGeneration,Count,MaxCycles);
			return false;
		}
	}
	return true;
}
bool IMW3ReadVoiceBudgets(
	const TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe>& Bridge,
	const TArray<FIMW3VoiceBinding>& Bindings,
	TArray<FIMW3VoiceBudget>& OutBudgets,
	uint64& OutSourceMaxCycles,
	FString& OutError)
{
	OutBudgets.Reset();OutSourceMaxCycles=0;
	if(!Bridge){OutError=TEXT("Device bridge is unavailable while reading per-voice budgets.");return false;}
	for(const FIMW3VoiceBinding& Binding:Bindings)
	{
		if(!Bridge->Voices.IsValidIndex(int32(Binding.Slot))||!Bridge->Voices[int32(Binding.Slot)])
		{OutError=FString::Printf(TEXT("Source slot %u has no voice bridge while reading budgets."),Binding.Slot);return false;}
		const auto& Voice=*Bridge->Voices[int32(Binding.Slot)];
		FIMW3VoiceBudget Budget;Budget.Binding=Binding;
		Budget.ExecuteCount=Voice.SourceExecuteCount.load(std::memory_order_acquire);
		Budget.MaxCycles=Voice.SourceExecuteMaxCycles.load(std::memory_order_acquire);
		OutSourceMaxCycles+=Budget.MaxCycles;OutBudgets.Add(Budget);
	}
	return OutBudgets.Num()==W3Voices;
}
struct FIMW3WindowLogSummary
{
	bool Allocated=false,Complete=false,NoOverflow=false,CoversStart=false,CoversEnd=false,FileSaved=false;
	uint64 Pushes=0,Exported=0,Overflows=0;
	double FirstWallClockSeconds=-1.0,LastWallClockSeconds=-1.0;
};
struct FIMW3PressureWindowSummary
{
	FIMW3WindowLogSummary Snapshots,Workers;
	bool FilesSaved=false;
	uint64 AllocatedBytes=0;
};
uint64 W3ProbeComplete(const std::atomic<uint64>* Done,uint32 Capacity,uint64 Pushes);
bool IMW3ExportPressureWindowLogs(
	FIMAcousticDeviceBridge* Bridge,const FString& EvidenceDir,double WindowStart,double WindowEnd,
	FIMW3PressureWindowSummary& Out)
{
	if(!Bridge||!Bridge->PressureWindowPublicationsQuiescent())return false;
	using FSnapshotLog=decltype(Bridge->PressureSnapshots);
	using FWorkerLog=decltype(Bridge->PressureWorkers);
	auto& SnapshotLog=Bridge->PressureSnapshots;
	auto& WorkerLog=Bridge->PressureWorkers;
	const bool bAllocated=bool(SnapshotLog.Entries)&&bool(SnapshotLog.Done)&&bool(WorkerLog.Entries)&&bool(WorkerLog.Done);
	Out.Snapshots.Allocated=bAllocated;
	Out.Workers.Allocated=bAllocated;
	if(bAllocated)Out.AllocatedBytes=FSnapshotLog::StorageBytes+FWorkerLog::StorageBytes;
	FString Snapshots=TEXT("block,world,spatial_version,captured_s,submit_s,submit_span_us,num_sources,submitted,fail_code\n");
	FString Workers=TEXT("block,world,loop_start_s,snap_captured_s,eval_start_s,eval_end_s,reverb_start_s,reverb_end_s,push_s,wait_end_s,num_inputs,published,eval_ok,push_ok,reverb_attempt,reverb_ok,reverb_sequence,reverb_spatial_version,reverb_slot,reverb_ready_before_s,reverb_ready_after_s\n");
	if(!bAllocated)return false;
	auto& S=Out.Snapshots;
	S.Pushes=SnapshotLog.Pushes.load(std::memory_order_acquire);
	S.Overflows=SnapshotLog.Overflows.load(std::memory_order_acquire);
	S.Exported=W3ProbeComplete(SnapshotLog.Done.get(),FSnapshotLog::MaxEntries,S.Pushes);
	S.Complete=S.Pushes<=FSnapshotLog::MaxEntries&&S.Exported==S.Pushes;
	S.NoOverflow=S.Overflows==0;
	for(uint64 I=0;I<S.Exported;++I)
	{
		const auto& P=SnapshotLog.Entries[I];
		if(I==0)S.FirstWallClockSeconds=P.CapturedSeconds;
		S.LastWallClockSeconds=P.CapturedSeconds;
		Snapshots+=FString::Printf(TEXT("%llu,%llu,%llu,%.9f,%.9f,%.3f,%u,%u,%u\n"),
			P.Block,P.WorldGeneration,P.SpatialVersion,P.CapturedSeconds,P.SubmitSeconds,
			(P.SubmitSeconds-P.CapturedSeconds)*1.e6,P.NumSources,P.Submitted,P.FailCode);
	}
	S.CoversStart=S.Exported>0&&S.FirstWallClockSeconds<=WindowStart+1.0;
	S.CoversEnd=S.Exported>0&&S.LastWallClockSeconds>=WindowEnd-1.0;
	auto& W=Out.Workers;
	W.Pushes=WorkerLog.Pushes.load(std::memory_order_acquire);
	W.Overflows=WorkerLog.Overflows.load(std::memory_order_acquire);
	W.Exported=W3ProbeComplete(WorkerLog.Done.get(),FWorkerLog::MaxEntries,W.Pushes);
	W.Complete=W.Pushes<=FWorkerLog::MaxEntries&&W.Exported==W.Pushes;
	W.NoOverflow=W.Overflows==0;
	for(uint64 I=0;I<W.Exported;++I)
	{
		const auto& P=WorkerLog.Entries[I];
		if(I==0)W.FirstWallClockSeconds=P.LoopStartSeconds;
		W.LastWallClockSeconds=P.WaitEndSeconds;
		Workers+=FString::Printf(TEXT("%llu,%llu,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%.9f,%.9f\n"),
			P.Block,P.WorldGeneration,P.LoopStartSeconds,P.SnapshotCaptured,P.EvalStartSeconds,P.EvalEndSeconds,
			P.ReverbStartSeconds,P.ReverbEndSeconds,P.PushSeconds,P.WaitEndSeconds,P.NumInputs,
			P.ResultsPublished,P.EvalOk,uint32(P.PushOk),P.ReverbAttempt,P.ReverbOk,
			P.ReverbSequence,P.ReverbSpatialVersion,P.ReverbSlot,P.ReverbReadyBeforeSeconds,P.ReverbReadyAfterSeconds);
	}
	W.CoversStart=W.Exported>0&&W.FirstWallClockSeconds<=WindowStart+1.0;
	W.CoversEnd=W.Exported>0&&W.LastWallClockSeconds>=WindowEnd-1.0;
	S.FileSaved=FFileHelper::SaveStringToFile(Snapshots,*FPaths::Combine(EvidenceDir,TEXT("pressure-gt-snapshots.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	W.FileSaved=FFileHelper::SaveStringToFile(Workers,*FPaths::Combine(EvidenceDir,TEXT("pressure-worker-loops.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	Out.FilesSaved=S.FileSaved&&W.FileSaved;
	return Out.FilesSaved&&S.Complete&&S.NoOverflow&&S.Pushes>0&&S.CoversStart&&S.CoversEnd
		&&W.Complete&&W.NoOverflow&&W.Pushes>0&&W.CoversStart&&W.CoversEnd;
}
bool IMW3WritePreflightFailureReport(
	const FString& EvidenceDir,const TCHAR* Gate,const FString& Message,
	int32 SourceCount,int32 AudioIdCount,uint32 GraphFrames,float GraphRate,
	const TArray<FIMW3VoiceBinding>& Bindings)
{
	if(EvidenceDir.IsEmpty())return false;
	FString Json;
	auto W=TJsonWriterFactory<>::Create(&Json);
	W->WriteObjectStart();
	W->WriteValue(TEXT("status"),TEXT("FAIL"));
	W->WriteValue(TEXT("gate"),Gate);
	W->WriteValue(TEXT("reason"),Message);
	W->WriteValue(TEXT("failure_wall_clock_s"),FPlatformTime::Seconds());
	W->WriteValue(TEXT("source_components"),SourceCount);
	W->WriteValue(TEXT("unique_audio_component_ids"),AudioIdCount);
	W->WriteValue(TEXT("graph_block_frames"),GraphFrames);
	W->WriteValue(TEXT("graph_sample_rate"),GraphRate);
	W->WriteArrayStart(TEXT("voice_bindings"));
	for(const FIMW3VoiceBinding& Binding:Bindings)
	{
		W->WriteObjectStart();W->WriteValue(TEXT("audio_component_id"),LexToString(Binding.AudioComponentId));
		W->WriteValue(TEXT("slot"),Binding.Slot);W->WriteValue(TEXT("live_generation"),double(Binding.LiveGeneration));W->WriteObjectEnd();
	}
	W->WriteArrayEnd();W->WriteObjectEnd();W->Close();
	return FFileHelper::SaveStringToFile(Json,*FPaths::Combine(EvidenceDir,TEXT("pressure-preflight-failure.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
class FIMAcousticUnderrunObserver final:public FOutputDevice
{
public:
	std::atomic<uint64> Count{0};std::atomic<bool> Enabled{false};
	void Serialize(const TCHAR* Text,ELogVerbosity::Type,const FName&) override
	{if(Enabled.load(std::memory_order_relaxed)&&FCString::Strifind(Text,TEXT("Audio Buffer Underrun (starvation) detected")))Count.fetch_add(1,std::memory_order_relaxed);}
	bool CanBeUsedOnAnyThread() const override{return true;}
};
uint64 W3ProbeComplete(const std::atomic<uint64>* Done,uint32 Capacity,uint64 Pushes)
{
	const uint64 Limit=Pushes<Capacity?Pushes:Capacity;
	for(uint64 I=0;I<Limit;++I)if(Done[I].load(std::memory_order_acquire)!=I+1)return I;
	return Limit;
}
bool W3ExportCallbackCorrelation(FIMAcousticDeviceBridge* Bridge,const FString& EvidenceDir)
{
	if(!Bridge)return false;
	const uint64 SourcePushes=Bridge->SourceBlockProbePushes.load(std::memory_order_acquire);
	const uint64 SourceLimit=SourcePushes<FIMAcousticDeviceBridge::CallbackBlockProbeCapacity?SourcePushes:FIMAcousticDeviceBridge::CallbackBlockProbeCapacity;
	const uint64 SourceComplete=W3ProbeComplete(Bridge->SourceBlockDone.data(),FIMAcousticDeviceBridge::CallbackBlockProbeCapacity,SourcePushes);
	const uint64 ReverbPushes=Bridge->ReverbBlockProbePushes.load(std::memory_order_acquire);
	const uint64 ReverbLimit=ReverbPushes<FIMAcousticDeviceBridge::CallbackBlockProbeCapacity?ReverbPushes:FIMAcousticDeviceBridge::CallbackBlockProbeCapacity;
	const uint64 ReverbComplete=W3ProbeComplete(Bridge->ReverbBlockDone.data(),FIMAcousticDeviceBridge::CallbackBlockProbeCapacity,ReverbPushes);
	FString Sources=TEXT("audio_block,callback_count,valid_source_count,source_sum_us,start_s,end_s\n");
	for(uint64 I=0;I<SourceComplete;++I)
	{
		const auto& P=Bridge->SourceBlockProbes[I];
		Sources+=FString::Printf(TEXT("%llu,%u,%u,%.3f,%.9f,%.9f\n"),P.AudioBlock,P.CallbackCount,P.ValidSourceCount,P.SourceSumUs,P.StartSeconds,P.EndSeconds);
	}
	FString Reverbs=TEXT("audio_block,outcome,fresh,rendered,start_s,end_s,duration_us\n");
	for(uint64 I=0;I<ReverbComplete;++I)
	{
		const auto& P=Bridge->ReverbBlockProbes[I];
		Reverbs+=FString::Printf(TEXT("%llu,%u,%u,%u,%.9f,%.9f,%.3f\n"),P.AudioBlock,P.Outcome,P.Fresh,P.Rendered,P.StartSeconds,P.EndSeconds,(P.EndSeconds-P.StartSeconds)*1.e6);
	}
	FString Snapshots=TEXT("block,world,spatial_version,captured_s,submit_s,submit_span_us,num_sources,submitted,fail_code\n");
	const uint64 SnapshotPushes=Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
	const uint64 SnapshotComplete=W3ProbeComplete(Bridge->SnapshotDone.data(),FIMAcousticDeviceBridge::ProbeSnapshotCapacity,SnapshotPushes);
	for(uint64 I=0;I<SnapshotComplete;++I)
	{
		const auto& P=Bridge->SnapshotProbes[I];
		Snapshots+=FString::Printf(TEXT("%llu,%llu,%llu,%.9f,%.9f,%.3f,%u,%u,%u\n"),P.Block,P.WorldGeneration,P.SpatialVersion,P.CapturedSeconds,P.SubmitSeconds,(P.SubmitSeconds-P.CapturedSeconds)*1.e6,P.NumSources,P.Submitted,P.FailCode);
	}
	FString Workers=TEXT("block,world,loop_start_s,snap_captured_s,eval_start_s,eval_end_s,reverb_start_s,reverb_end_s,push_s,wait_end_s,num_inputs,published,eval_ok,push_ok,reverb_attempt,reverb_ok,reverb_sequence,reverb_spatial_version,reverb_slot,reverb_ready_before_s,reverb_ready_after_s\n");
	const uint64 WorkerPushes=Bridge->WorkerProbePushes.load(std::memory_order_acquire);
	const uint64 WorkerComplete=W3ProbeComplete(Bridge->WorkerDone.data(),FIMAcousticDeviceBridge::ProbeWorkerCapacity,WorkerPushes);
	for(uint64 I=0;I<WorkerComplete;++I)
	{
		const auto& P=Bridge->WorkerProbes[I];
		Workers+=FString::Printf(TEXT("%llu,%llu,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%.9f,%.9f\n"),P.Block,P.WorldGeneration,P.LoopStartSeconds,P.SnapshotCaptured,P.EvalStartSeconds,P.EvalEndSeconds,P.ReverbStartSeconds,P.ReverbEndSeconds,P.PushSeconds,P.WaitEndSeconds,P.NumInputs,P.ResultsPublished,P.EvalOk,uint32(P.PushOk),P.ReverbAttempt,P.ReverbOk,P.ReverbSequence,P.ReverbSpatialVersion,P.ReverbSlot,P.ReverbReadyBeforeSeconds,P.ReverbReadyAfterSeconds);
	}
	const bool SourceOk=Bridge->SourceBlockProbeOverflows.load(std::memory_order_acquire)==0&&SourceComplete==SourceLimit;
	const bool ReverbOk=Bridge->ReverbBlockProbeOverflows.load(std::memory_order_acquire)==0&&ReverbComplete==ReverbLimit;
	const bool SnapshotOk=Bridge->SnapshotProbeOverflows.load(std::memory_order_acquire)==0&&SnapshotComplete==(SnapshotPushes<FIMAcousticDeviceBridge::ProbeSnapshotCapacity?SnapshotPushes:FIMAcousticDeviceBridge::ProbeSnapshotCapacity);
	const bool WorkerOk=Bridge->WorkerProbeOverflows.load(std::memory_order_acquire)==0&&WorkerComplete==(WorkerPushes<FIMAcousticDeviceBridge::ProbeWorkerCapacity?WorkerPushes:FIMAcousticDeviceBridge::ProbeWorkerCapacity);
	const FString Summary=FString::Printf(TEXT("{\"complete\":%s,\"source_blocks\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu},\"reverb_blocks\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu},\"snapshots\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu},\"workers\":{\"complete\":%s,\"pushed\":%llu,\"exported\":%llu,\"overflow\":%llu}}"),
		(SourceOk&&ReverbOk&&SnapshotOk&&WorkerOk)?TEXT("true"):TEXT("false"),SourceOk?TEXT("true"):TEXT("false"),SourcePushes,SourceComplete,Bridge->SourceBlockProbeOverflows.load(),ReverbOk?TEXT("true"):TEXT("false"),ReverbPushes,ReverbComplete,Bridge->ReverbBlockProbeOverflows.load(),SnapshotOk?TEXT("true"):TEXT("false"),SnapshotPushes,SnapshotComplete,Bridge->SnapshotProbeOverflows.load(),WorkerOk?TEXT("true"):TEXT("false"),WorkerPushes,WorkerComplete,Bridge->WorkerProbeOverflows.load());
	const bool SavedSources=FFileHelper::SaveStringToFile(Sources,*FPaths::Combine(EvidenceDir,TEXT("callback-source-blocks.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedReverbs=FFileHelper::SaveStringToFile(Reverbs,*FPaths::Combine(EvidenceDir,TEXT("callback-reverb-blocks.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSnapshots=FFileHelper::SaveStringToFile(Snapshots,*FPaths::Combine(EvidenceDir,TEXT("callback-gt-snapshots.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedWorkers=FFileHelper::SaveStringToFile(Workers,*FPaths::Combine(EvidenceDir,TEXT("callback-worker-loops.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSummary=FFileHelper::SaveStringToFile(Summary,*FPaths::Combine(EvidenceDir,TEXT("callback-correlation-summary.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3CallbackCorrelation complete=%d source=%llu/%llu reverb=%llu/%llu snapshots=%llu workers=%llu evidence=%s"),SourceOk&&ReverbOk&&SnapshotOk&&WorkerOk,SourceComplete,SourcePushes,ReverbComplete,ReverbPushes,SnapshotComplete,WorkerComplete,*EvidenceDir);
	return SavedSources&&SavedReverbs&&SavedSnapshots&&SavedWorkers&&SavedSummary&&SourceOk&&ReverbOk&&SnapshotOk&&WorkerOk;
}
bool W3ExportMetaSoundCorrelation(const FIMAcousticMetaSoundContextPtr& Context,const FString& EvidenceDir)
{
	if (!Context.IsValid()) return false;
	const uint32 SourceCount = FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire), uint32(Context->CapturedSourceBlocks.Num()));
	const uint32 SourceClaimed = Context->CapturedSourceBlockClaimCount.load(std::memory_order_acquire);
	const uint32 EnvironmentCount = FMath::Min<uint32>(Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire), uint32(Context->CapturedBlocks.Num()));
	const uint32 EnvironmentPcmBlocks = FMath::Min<uint32>(Context->CapturedBlockCount.load(std::memory_order_acquire), uint32(Context->CapturedBlocks.Num()));
	const uint64 EnvironmentClaimed = Context->EnvironmentBlocks.load(std::memory_order_acquire);
	const bool SourceComplete = SourceCount == SourceClaimed;
	const bool EnvironmentComplete = uint64(EnvironmentCount) == EnvironmentClaimed;
	FString Sources = TEXT("block,voice,input_spatial_version,output_spatial_version,input_delay_priming,callback_audio_id,result_audio_id,world_generation,result_spatial_version,voice_generation,sequence,result_snapshot_captured_s,consumed_s,age_ms,routes,direct_valid,path_valid,reject,reject_detail,fallback,input_energy,output_energy\n");
	for (uint32 I = 0; I < SourceCount; ++I)
	{
		const auto& P = Context->CapturedSourceBlocks[int32(I)];
		Sources += FString::Printf(TEXT("%llu,%u,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%.9f,%.6f,%u,%u,%u,%u,%u,%u,%.9g,%.9g\n"),
			P.Block, P.Voice, P.InputSpatialVersion, P.OutputSpatialVersion, uint32(P.InputDelayPriming),
			P.CallbackAudioComponentId, P.ResultAudioComponentId, P.ResultWorldGeneration, P.ResultSpatialVersion,
			P.ResultVoiceGeneration, P.ResultSequence, P.SnapshotCaptured, P.ConsumedSeconds, P.AgeMs,
			P.Routes, P.DirectValid, P.PathValid, uint32(P.Reject), P.RejectDetail, P.Fallback,
			P.InputEnergy, P.OutputEnergy);
	}
	FString Environments = TEXT("block,frame,sequence,seconds,fresh,listener_x,listener_y,listener_z,expected_spatial_version,current_spatial_version\n");
	for (uint32 I = 0; I < EnvironmentCount; ++I)
	{
		const auto& E = Context->CapturedBlocks[int32(I)];
		Environments += FString::Printf(TEXT("%u,%llu,%llu,%.9f,%u,%.6f,%.6f,%.6f,%llu,%llu\n"),
			I, E.Frame, E.Sequence, E.Seconds, E.Fresh ? 1u : 0u, E.ListenerX, E.ListenerY, E.ListenerZ,E.ExpectedSpatialVersion,E.SpatialVersion);
	}
	const FString Summary = FString::Printf(TEXT("{\"source_blocks\":%u,\"source_claimed\":%u,\"environment_blocks\":%u,\"environment_trace_claimed\":%llu,\"environment_pcm_blocks\":%u,\"source_frames\":%u,\"environment_frames\":%u,\"source_overflow\":%s,\"environment_overflow\":%s}"),
		SourceCount, SourceClaimed, EnvironmentCount, EnvironmentClaimed, EnvironmentPcmBlocks, Context->CapturedSourceFrames.load(std::memory_order_acquire),
		Context->CapturedEnvironmentFrames.load(std::memory_order_acquire),
		SourceComplete ? TEXT("false") : TEXT("true"),
		EnvironmentComplete ? TEXT("false") : TEXT("true"));
	const bool SavedSources = FFileHelper::SaveStringToFile(Sources, *FPaths::Combine(EvidenceDir, TEXT("metasound-source-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedEnvironments = FFileHelper::SaveStringToFile(Environments, *FPaths::Combine(EvidenceDir, TEXT("metasound-environment-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSummary = FFileHelper::SaveStringToFile(Summary, *FPaths::Combine(EvidenceDir, TEXT("metasound-correlation-summary.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticW3MetaSoundCorrelation source=%u environment=%u evidence=%s"), SourceCount, EnvironmentCount, *EvidenceDir);
	return SavedSources && SavedEnvironments && SavedSummary && SourceComplete && EnvironmentComplete && SourceCount > 0 && EnvironmentCount > 0;
}
struct FIMW3DiagnosticTraceSummary
{
	bool ContextAvailable=false,TraceComplete=false,FormalWindowValid=false,ExpectedSourcesPresent=false;
	uint32 SourceRows=0,SourceClaimed=0,EnvironmentRows=0,FormalRows=0;
	uint64 EnvironmentClaimed=0;
	uint32 UniqueVoices=0,UniqueAudioIds=0,IdentityMismatchRows=0,SlotMismatchRows=0,GenerationMismatchRows=0,RejectedRows=0,PrimingRows=0;
	uint32 RouteMaskOr=0;
};
FIMW3DiagnosticTraceSummary IMW3ValidateDiagnosticTrace(
	const FIMAcousticMetaSoundContextPtr& Context,double WindowStart,double WindowEnd,
	double ExpectedFormalRows,const TArray<FIMW3VoiceBinding>& ExpectedBindings)
{
	FIMW3DiagnosticTraceSummary Summary;
	Summary.ContextAvailable=Context.IsValid();
	if(!Context.IsValid())return Summary;

	Summary.SourceRows=FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire),uint32(Context->CapturedSourceBlocks.Num()));
	Summary.SourceClaimed=Context->CapturedSourceBlockClaimCount.load(std::memory_order_acquire);
	Summary.EnvironmentRows=FMath::Min<uint32>(Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire),uint32(Context->CapturedBlocks.Num()));
	Summary.EnvironmentClaimed=Context->EnvironmentBlocks.load(std::memory_order_acquire);
	Summary.TraceComplete=Summary.SourceRows==Summary.SourceClaimed&&uint64(Summary.EnvironmentRows)==Summary.EnvironmentClaimed;

	TSet<uint32> Voices;
	TSet<uint64> AudioIds;
	TMap<uint32,uint64> VoiceToAudioId;
	TMap<uint64,uint32> AudioIdToVoice;
	TMap<uint64,FIMW3VoiceBinding> BindingByAudioId;
	TMap<uint32,uint64> BindingBySlot;
	for(const FIMW3VoiceBinding& Binding:ExpectedBindings)
	{
		BindingByAudioId.Add(Binding.AudioComponentId,Binding);
		BindingBySlot.Add(Binding.Slot,Binding.AudioComponentId);
	}
	for(uint32 I=0;I<Summary.SourceRows;++I)
	{
		const FIMAcousticBlockProbe& P=Context->CapturedSourceBlocks[int32(I)];
		if(P.ConsumedSeconds<WindowStart||P.ConsumedSeconds>WindowEnd)continue;
		++Summary.FormalRows;
		Summary.RouteMaskOr|=P.Routes;
		const FIMW3VoiceBinding* Binding=BindingByAudioId.Find(P.CallbackAudioComponentId);
		const uint64* SlotAudioId=BindingBySlot.Find(P.Voice);
		const bool bSlotMismatch=!Binding||Binding->Slot!=P.Voice||!SlotAudioId||*SlotAudioId!=P.CallbackAudioComponentId;
		const bool bGenerationMismatch=Binding&&P.ResultVoiceGeneration!=Binding->LiveGeneration;
		if(bSlotMismatch)++Summary.SlotMismatchRows;
		if(bGenerationMismatch)++Summary.GenerationMismatchRows;
		if(P.Voice>=FIMAcousticMetaSoundContext::MaxVoices||P.CallbackAudioComponentId==0
			||P.CallbackAudioComponentId!=P.ResultAudioComponentId||!Binding)
		{
			++Summary.IdentityMismatchRows;
		}
		else
		{
			Voices.Add(P.Voice);AudioIds.Add(P.CallbackAudioComponentId);
			if(bSlotMismatch||bGenerationMismatch)++Summary.IdentityMismatchRows;
			if(const uint64* ExistingId=VoiceToAudioId.Find(P.Voice);ExistingId&&*ExistingId!=P.CallbackAudioComponentId)
				++Summary.IdentityMismatchRows;
			else VoiceToAudioId.Add(P.Voice,P.CallbackAudioComponentId);
			if(const uint32* ExistingVoice=AudioIdToVoice.Find(P.CallbackAudioComponentId);ExistingVoice&&*ExistingVoice!=P.Voice)
				++Summary.IdentityMismatchRows;
			else AudioIdToVoice.Add(P.CallbackAudioComponentId,P.Voice);
		}
		if(P.Reject!=EIMAcousticProbeReject::Accepted)++Summary.RejectedRows;
		if(P.InputDelayPriming)++Summary.PrimingRows;
	}
	Summary.UniqueVoices=Voices.Num();Summary.UniqueAudioIds=AudioIds.Num();
	TSet<uint32> ExpectedSlots;
	Summary.ExpectedSourcesPresent=ExpectedBindings.Num()==W3Voices&&BindingByAudioId.Num()==W3Voices&&BindingBySlot.Num()==W3Voices;
	for(const FIMW3VoiceBinding& Binding:ExpectedBindings)
	{
		ExpectedSlots.Add(Binding.Slot);
		Summary.ExpectedSourcesPresent=Summary.ExpectedSourcesPresent&&AudioIds.Contains(Binding.AudioComponentId)
			&&Voices.Contains(Binding.Slot);
	}
	Summary.ExpectedSourcesPresent=Summary.ExpectedSourcesPresent&&ExpectedSlots.Num()==W3Voices;
	Summary.FormalWindowValid=Summary.FormalRows>=uint32(FMath::CeilToDouble(ExpectedFormalRows*.9))
		&&Summary.UniqueVoices==W3Voices&&Summary.UniqueAudioIds==W3Voices
		&&Summary.IdentityMismatchRows==0&&Summary.SlotMismatchRows==0&&Summary.GenerationMismatchRows==0
		&&Summary.RejectedRows==0&&Summary.PrimingRows==0
		&&Summary.ExpectedSourcesPresent;
	return Summary;
}
struct FIMW3WavSummary
{
	bool FileExists=false,Complete=false,Pcm16=false;
	uint64 FileBytes=0,DataBytes=0,NonzeroSamples=0;
	uint32 SampleRate=0,Channels=0,BitsPerSample=0;
	double DurationSeconds=0.0;
};
FIMW3WavSummary IMW3InspectRecordedWav(const FString& WavPath)
{
	FIMW3WavSummary Summary;
	Summary.FileExists=IFileManager::Get().FileExists(*WavPath);
	if(!Summary.FileExists)return Summary;
	TArray<uint8> Bytes;
	if(!FFileHelper::LoadFileToArray(Bytes,*WavPath)||Bytes.Num()<44)return Summary;
	Summary.FileBytes=uint64(Bytes.Num());
	auto Read16=[&Bytes](int32 Offset)->uint16{return uint16(Bytes[Offset])|(uint16(Bytes[Offset+1])<<8);};
	auto Read32=[&Bytes](int32 Offset)->uint32{return uint32(Bytes[Offset])|(uint32(Bytes[Offset+1])<<8)|(uint32(Bytes[Offset+2])<<16)|(uint32(Bytes[Offset+3])<<24);};
	const bool HeaderOk=FMemory::Memcmp(Bytes.GetData(),"RIFF",4)==0&&FMemory::Memcmp(Bytes.GetData()+8,"WAVE",4)==0
		&&FMemory::Memcmp(Bytes.GetData()+12,"fmt ",4)==0&&FMemory::Memcmp(Bytes.GetData()+36,"data",4)==0;
	const uint32 RiffBytes=Read32(4),Format=Read16(20),Channels=Read16(22),SampleRate=Read32(24),ByteRate=Read32(28);
	const uint16 BlockAlign=Read16(32),Bits=Read16(34);const uint32 DataBytes=Read32(40);
	Summary.Channels=Channels;Summary.SampleRate=SampleRate;Summary.BitsPerSample=Bits;Summary.DataBytes=DataBytes;
	Summary.Pcm16=HeaderOk&&Format==1&&Bits==16&&Channels>0&&SampleRate>0
		&&BlockAlign==Channels*2&&ByteRate==SampleRate*BlockAlign;
	Summary.Complete=Summary.Pcm16&&uint64(RiffBytes)+8==Summary.FileBytes&&uint64(DataBytes)+44==Summary.FileBytes
		&&DataBytes>0&&DataBytes%BlockAlign==0;
	if(!Summary.Complete)return Summary;
	Summary.DurationSeconds=double(DataBytes)/double(ByteRate);
	for(int32 Offset=44;Offset+1<int32(Summary.FileBytes);Offset+=2)
	{
		const uint16 Sample=Read16(Offset);
		if(Sample!=0)++Summary.NonzeroSamples;
	}
	return Summary;
}
bool W3ExportRareTailCorrelation(FIMAcousticDeviceBridge* Bridge,const FString& EvidenceDir,bool Requested,bool SelfTest)
{
	if(!Bridge)return false;
	const uint64 Pushes=Bridge->RareTailProbePushes.load(std::memory_order_acquire);
	const uint64 Capacity=FIMAcousticDeviceBridge::RareTailProbeCapacity;
	const uint64 Begin=Pushes>Capacity?Pushes-Capacity:0;
	const uint64 Retained=Pushes<Capacity?Pushes:Capacity;
	bool Complete=Bridge->RareTailProbeOverflows.load(std::memory_order_acquire)==0;
	FString CSV=TEXT("sequence,audio_block,source_block,trigger_mask,source_join_valid,source_sum_us,reverb_us,combined_us,outcome,fresh,rendered,self_test,reverb_start_s,reverb_end_s,max_snapshot_gap_us,max_worker_gap_us\n");
	uint64 Exported=0;
	for(uint64 N=Begin;N<Pushes;++N)
	{
		const uint32 Slot=static_cast<uint32>(N%Capacity);
		if(Bridge->RareTailDone[Slot].load(std::memory_order_acquire)!=N+1){Complete=false;continue;}
		const auto& P=Bridge->RareTailProbes[Slot];
		CSV+=FString::Printf(TEXT("%llu,%llu,%llu,%u,%u,%.3f,%.3f,%.3f,%u,%u,%u,%u,%.9f,%.9f,%llu,%llu\n"),
			N,P.AudioBlock,P.SourceBlock,P.TriggerMask,P.SourceJoinValid,P.SourceSumUs,P.ReverbUs,P.CombinedUs,P.Outcome,P.Fresh,P.Rendered,P.SelfTest,
			P.ReverbStartSeconds,P.ReverbEndSeconds,P.MaxSnapshotGapUs,P.MaxWorkerGapUs);
		++Exported;
	}
	const uint32 DeviceBudgetUs=Bridge->RareTailDeviceBudgetUs.load(std::memory_order_relaxed);
	const uint32 NearBudgetUs=Bridge->RareTailNearBudgetUs.load(std::memory_order_relaxed);
	const uint32 NearTailUs=Bridge->RareTailNearTailUs.load(std::memory_order_relaxed);
	const uint64 Overflow=Bridge->RareTailProbeOverflows.load(std::memory_order_acquire);
	const FString Summary=FString::Printf(TEXT("{\"requested\":%s,\"self_test\":%s,\"negative_control\":%s,\"device_budget_us\":%u,\"near_budget_us\":%u,\"near_tail_us\":%u,\"capacity\":%llu,\"pushed\":%llu,\"retained\":%llu,\"exported\":%llu,\"overflow\":%llu,\"complete\":%s,\"closing_condition\":\"flags_disabled_plus_0.3s_drain\"}"),
		Requested?TEXT("true"):TEXT("false"),SelfTest?TEXT("true"):TEXT("false"),(!Requested)?TEXT("true"):TEXT("false"),DeviceBudgetUs,NearBudgetUs,NearTailUs,Capacity,Pushes,Retained,Exported,Overflow,Complete?TEXT("true"):TEXT("false"));
	const bool SavedCSV=FFileHelper::SaveStringToFile(CSV,*FPaths::Combine(EvidenceDir,TEXT("callback-rare-tail.csv")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool SavedSummary=FFileHelper::SaveStringToFile(Summary,*FPaths::Combine(EvidenceDir,TEXT("callback-rare-tail-contract.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3RareTail requested=%d self_test=%d complete=%d pushed=%llu exported=%llu overflow=%llu thresholds=%u/%u/%u evidence=%s"),
		Requested,SelfTest,Complete,Pushes,Exported,Overflow,DeviceBudgetUs,NearBudgetUs,NearTailUs,*EvidenceDir);
	return SavedCSV&&SavedSummary&&Complete;
}
class FIMAcousticW3PressureCommand final:public IAutomationLatentCommand
{
public:
	explicit FIMAcousticW3PressureCommand(FAutomationTestBase* In,double InDuration=W3Duration,bool InDropWet=false,bool InStaleProbe=false,
		bool InRequireDiagnosticEvidence=false,FIMW3MemorySample InPreCaptureMemory=FIMW3MemorySample())
		:Test(In),Created(FPlatformTime::Seconds()),Duration(InDuration),DropWet(InDropWet),StaleProbe(InStaleProbe),
		RequireDiagnosticEvidence(InRequireDiagnosticEvidence),RequireNormalEvidence(!InDropWet&&!InStaleProbe),
		PreCaptureMemory(InPreCaptureMemory){}
	~FIMAcousticW3PressureCommand() override {Cleanup();}
	bool Update() override
	{
		const double Now=FPlatformTime::Seconds();
		if(Now-Created>Duration+90)return Finish(false,TEXT("Pressure test exceeded its bounded runtime."));
		UWorld* World=nullptr;for(const auto& C:GEngine->GetWorldContexts())if(C.WorldType==EWorldType::PIE){World=C.World();break;}
		if(!World)return Stage==0?false:Finish(false,TEXT("PIE ended during pressure test."));
		if(Stage==0)
		{
			TestWorld=World;
			for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It)Volume=*It;
			Listener=World->GetFirstPlayerController();if(!Volume.IsValid()||!Listener.IsValid())return false;
			Bridge=IMAcousticTestSupport::FindBridge(World);if(!Bridge)return false;
			BackgroundVolume=FApp::GetUnfocusedVolumeMultiplier();FApp::SetUnfocusedVolumeMultiplier(1);
			bAllowBackgroundAudioOrig=GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=true;SettingsChanged=true;
			auto* Performance=GetMutableDefault<UEditorPerformanceSettings>();PreviousThrottle=Performance->bThrottleCPUWhenNotForeground;
			// An unattended editor must continue GT snapshots when another app
			// has focus. This is test-local and restored without saving config.
			Performance->bThrottleCPUWhenNotForeground=false;
			LogInterval=IConsoleManager::Get().FindConsoleVariable(TEXT("au.MinLogTimeBetweenUnderrunWarnings"));
			if(LogInterval){PreviousLogInterval=LogInterval->GetFloat();LogInterval->Set(0.f,ECVF_SetByCode);}
			GLog->AddOutputDevice(&Underruns);ObserverRegistered=true;
			PCM.SetNumUninitialized(W3Rate*2);
			// Two seconds of low-level periodic input; all voices together remain
			// below clipping. Queue replenishment is GT-only and measured separately.
			for(int32 I=0;I<PCM.Num();++I)PCM[I]=int16(300*FMath::Sin(2*PI*440*I/W3Rate));
			for(int32 I=0;I<W3Voices;++I)
			{
				auto* Actor=World->SpawnActor<AActor>();auto* Audio=NewObject<UAudioComponent>(Actor);
				Actor->SetRootComponent(Audio);Actor->AddInstanceComponent(Audio);Audio->bAutoActivate=false;Audio->RegisterComponent();
				auto* Source=NewObject<UIMAcousticSourceComponent>(Actor);Actor->AddInstanceComponent(Source);Source->AudioComponent=Audio;Source->RegisterComponent();
				FString SourceError;
				if(!IMAcousticTestSupport::ConfigureGraphSource(Audio,SourceError))return Finish(false,SourceError);
				FString ValidationError;
				if(!Source->ValidateSource(ValidationError))return Finish(false,ValidationError);
				Audio->Play();Sources.Add(Audio);Sounds.Add(Audio->Sound);
			}
			if(RequireNormalEvidence)
			{
				GraphSourceAsset=LoadObject<USoundBase>(nullptr,IMAcousticTestSupport::GraphSourceAssetPath);
				GraphSourceIdentityValid=GraphSourceAsset.IsValid()&&Sources.Num()==W3Voices&&Sounds.Num()==W3Voices;
				for(int32 I=0;I<Sources.Num();++I)
					GraphSourceIdentityValid=GraphSourceIdentityValid&&Sources[I].IsValid()&&Sounds[I].Get()==GraphSourceAsset.Get()
						&&Sources[I]->GetSound()==GraphSourceAsset.Get();
			}
			ExpectedAudioComponentIds.Reset();ExpectedAudioComponentIdOrder.Reset();
			for(const auto& Source:Sources)
			{
				const uint64 AudioId=Source.IsValid()?Source->GetAudioComponentID():0;
				if(!AudioId||ExpectedAudioComponentIds.Contains(AudioId))
					return Finish(false,TEXT("Graph setup requires sixteen unique nonzero AudioComponentIDs."));
				ExpectedAudioComponentIds.Add(AudioId);ExpectedAudioComponentIdOrder.Add(AudioId);
			}
			Volume->bEnableV2=true;Volume->bDirectRoute=true;Volume->bPathRoute=true;Volume->bReverbRoute=true;
			Directory=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/W3-UE"),FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*Directory,true);WavPath=FPaths::Combine(Directory,TEXT("IM_complex_16voices.wav"));
			CSV=TEXT("elapsed_s,rendered,rejected,reverb_rejected,dry_dropped,used_physical_process_bytes,reverb_calls,reverb_dry,reverb_raw,reverb_audible,max_snapshot_gap_us,max_worker_gap_us,used_virtual_commit_process_bytes,available_physical_system_bytes,available_virtual_system_bytes,peak_used_physical_process_lifetime_bytes,peak_used_virtual_process_lifetime_bytes\n");
			// The volume detects graph sources on its next tick and may replace
			// its initial legacy bridge. Setup precedes the stable warmup baseline.
			Bridge.Reset();MetaContext.Reset();
			Stage=-1;StageStarted=FPlatformTime::Seconds();
		}
		const TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> CurrentBridge=IMAcousticTestSupport::FindBridge(World);
		if(Stage==-1)
		{
			FIMAcousticMetaSoundContextPtr CandidateContext;
			FAudioDevice* AudioDevice=World->GetAudioDeviceRaw();
			if(AudioDevice)CandidateContext=IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
			TArray<FIMW3VoiceBinding> CandidateBindings;
			FString BindingError=TEXT("The graph device, environment and source consumers are not ready.");
			const bool bGraphOwnerReady=AudioDevice&&CandidateContext.IsValid()&&CurrentBridge
				&&CandidateContext->Device.Get()==CurrentBridge.Get()
				&&CandidateContext->DeviceId==AudioDevice->DeviceID
				&&!CandidateContext->Stopped.load(std::memory_order_acquire)
				&&CurrentBridge->Alive.load(std::memory_order_acquire)
				&&CurrentBridge->WorldGeneration.load(std::memory_order_acquire)!=0
				&&CandidateContext->EnvironmentConsumer.load(std::memory_order_acquire)
				&&CandidateContext->BusReaderConsumer.load(std::memory_order_acquire);
			if(bGraphOwnerReady&&IMW3BuildVoiceBindings(CandidateContext,CurrentBridge,
					ExpectedAudioComponentIdOrder,CandidateBindings,BindingError))
			{
				Bridge=CurrentBridge;MetaContext=CandidateContext;VoiceBindings=MoveTemp(CandidateBindings);
				GraphSetupBindingWaitSeconds=Now-StageStarted;
				GraphDeviceId=AudioDevice->DeviceID;
				GraphWorldGeneration=Bridge->WorldGeneration.load(std::memory_order_acquire);
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3GraphWarmupBaseline device=%u world_generation=%llu voices=%d setup_wait_s=%g"),
					GraphDeviceId,GraphWorldGeneration,VoiceBindings.Num(),GraphSetupBindingWaitSeconds);
				Stage=1;StageStarted=Now;
			}
			else
			{
				if(Now-StageStarted>10.0)
					return FailGate(TEXT("graph_setup_binding"),BindingError);
				return false;
			}
		}
		if(Stage==1&&Bridge.Get()!=CurrentBridge.Get())
			return FailGate(TEXT("stable_device_bridge"),TEXT("The device bridge changed during the W3 warmup."));
		if(Stage==2&&Bridge.Get()!=CurrentBridge.Get())
			return FailGate(TEXT("stable_device_bridge"),TEXT("The device bridge changed during the measured window."));
		if (!MetaContext.IsValid())
		{
			if (FAudioDevice* AudioDevice = World->GetAudioDeviceRaw())
				MetaContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
		}
		if(Stage==1||Stage==2)
		{
			FIMAcousticMetaSoundContextPtr CurrentMetaContext;
			if(FAudioDevice* AudioDevice=World->GetAudioDeviceRaw())
				CurrentMetaContext=IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
			if(CurrentMetaContext.Get()!=MetaContext.Get())
				return FailGate(TEXT("stable_metasound_context"),TEXT("The MetaSound context changed after the graph warmup baseline."));
			if(Bridge->WorldGeneration.load(std::memory_order_acquire)!=GraphWorldGeneration)
				return FailGate(TEXT("stable_graph_world_generation"),TEXT("The graph world generation changed after the warmup baseline."));
			if(!MetaContext->EnvironmentConsumer.load(std::memory_order_acquire)
				||!MetaContext->BusReaderConsumer.load(std::memory_order_acquire))
				return FailGate(TEXT("stable_environment_consumer"),TEXT("The graph environment/bus consumer disappeared after the warmup baseline."));
			FString IdentityError;
			if(!IMW3VoiceBindingsStillCurrent(MetaContext,Bridge,VoiceBindings,IdentityError))
				return FailGate(TEXT("stable_source_voice_identity"),IdentityError);
		}
		if(!Volume.IsValid()||!Listener.IsValid()||!Bridge||!Bridge->Alive.load())return Finish(false,TEXT("Pressure world/device disappeared."));
		const double Motion=Now-Created;
		if(Now>=NextMotionUpdate)
		{
			NextMotionUpdate=Now+W3MotionPeriod;
			Listener->SetAudioListenerOverride(nullptr,FVector(900+350*FMath::Sin(Motion*.23),300,150),FRotator(0,20*FMath::Sin(Motion*.17),0));
			if(FParse::Param(FCommandLine::Get(),TEXT("nullrhi")))
			{
				// NullRHI omits GameViewportClient::Draw's listener update. Mirror
				// that GT update for this diagnostic only; normal acceptance draws
				// the real viewport and uses the engine's listener publication.
				FVector Location,Front,Right;Listener->GetAudioListenerPosition(Location,Front,Right);
				FTransform Transform(FRotationMatrix::MakeFromXY(Front,Right));Transform.SetTranslation(Location);Transform.NormalizeRotation();
				World->GetAudioDeviceRaw()->SetListener(World,0,Transform,World->GetDeltaSeconds());
			}
			for(int32 I=0;I<Sources.Num();++I)
			{
				if(!Sources[I].IsValid()||!Sounds[I].IsValid())return Finish(false,TEXT("Managed pressure source destroyed unexpectedly."));
				Sources[I]->SetWorldLocation(FVector(900+400*FMath::Sin(Motion*.21+I*.4),300+25*FMath::Sin(Motion*.31+I),150));
			}
		}
		for(int32 I=0;I<Sources.Num();++I)
		{
			if(!Sources[I].IsValid()||!Sounds[I].IsValid())return Finish(false,TEXT("Managed pressure source destroyed unexpectedly."));
		}
		// Whitebox first publication measured at ~5.5s after Stage1 under load
		// (2s GT hitches during heavy-map startup). Warmup lets the system reach
		// steady state; all acceptance thresholds below are unchanged.
		if(Stage==1&&Now-StageStarted>=30)
		{
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3RenderFaultCounts frame=%llu input=%llu direct=%llu path=%llu"),
				Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::InvalidFrame)].load(),Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::NonfiniteInput)].load(),
				Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::NonfiniteDirect)].load(),Bridge->RenderFailures[size_t(EIMAcousticRenderFailure::NonfinitePath)].load());
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3Warmup elapsed_s=%g rendered=%llu reverb=%llu rejected=%llu stale=%llu missing=%llu reverb_calls=%llu dry=%llu raw=%llu snapshot_gap_us=%llu worker_gap_us=%llu status=%s"),
				Now-StageStarted,Bridge->RenderedBlocks.load(),Bridge->ReverbNonzeroBlocks.load(),Bridge->RejectedBlocks.load(),
				Bridge->StaleResultBlocks.load(),Bridge->MissingResultBlocks.load(),Bridge->ReverbProcessedBlocks.load(),
				Bridge->ReverbDryBlocks.load(),Bridge->ReverbRawNonzeroBlocks.load(),Bridge->MaxSnapshotGapUs.load(),Bridge->MaxWorkerGapUs.load(),*Volume->Status);
			if(Bridge->RenderedBlocks.load()<W3Voices*50||Bridge->ReverbNonzeroBlocks.load()<10)
				return Finish(false,TEXT("Actual complex-scene sound routes did not become audible."));
			DiagnosticMetaContextAtMeasurementStart=MetaContext.IsValid()&&MetaContext->Device.Get()==Bridge.Get();
			ExpectedAudioComponentIds.Reset();ExpectedAudioComponentIdOrder.Reset();
			for(const auto& Source:Sources)
			{
				const uint64 AudioId=Source.IsValid()?Source->GetAudioComponentID():0;
				if(!AudioId||ExpectedAudioComponentIds.Contains(AudioId))GraphSourceIdentityValid=false;
				else
				{
					ExpectedAudioComponentIds.Add(AudioId);
					ExpectedAudioComponentIdOrder.Add(AudioId);
				}
			}
			GraphSourceIdentityValid=GraphSourceIdentityValid&&ExpectedAudioComponentIds.Num()==W3Voices
				&&ExpectedAudioComponentIdOrder.Num()==W3Voices;
			if(RequireNormalEvidence&&(!GraphSourceIdentityValid||!DiagnosticMetaContextAtMeasurementStart))
				return FailGate(TEXT("graph_source_identity"),TEXT("Normal W3 measurement requires sixteen unique graph source IDs in the current MetaSound context."));
			FString BindingError;
			if(!IMW3BuildVoiceBindings(MetaContext,Bridge,ExpectedAudioComponentIdOrder,VoiceBindings,BindingError))
				return FailGate(TEXT("graph_voice_slot_binding"),BindingError);
			GraphBlockFrames=Bridge->BlockFrames;GraphSampleRate=Bridge->SampleRate;
			Audio::FMixerDevice* Mixer=FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(World);
			if(Mixer)
			{
				MixerBlockFrames=Mixer->GetNumOutputFrames();MixerSampleRate=Mixer->GetSampleRate();
				MixerDeadlineAvailable=MixerBlockFrames>0&&MixerSampleRate>0.0f;
			}
			const bool GraphFramesAvailable=GraphBlockFrames==FIMAcousticMetaSoundContext::Frames&&GraphSampleRate>0.0f;
			MixerGeometryGateOk=MixerDeadlineAvailable&&GraphFramesAvailable&&MixerSampleRate==GraphSampleRate
				&&MixerBlockFrames%GraphBlockFrames==0;
			GraphBlocksPerMixer=MixerGeometryGateOk?MixerBlockFrames/GraphBlockFrames:0;
			if(RequireNormalEvidence&&(!GraphFramesAvailable||!MixerGeometryGateOk))
				return FailGate(TEXT("graph_mixer_geometry"),FString::Printf(TEXT("Graph/mixer cadence is invalid (graph=%u@%.1f mixer=%u@%.1f)."),
					GraphBlockFrames,GraphSampleRate,MixerBlockFrames,MixerSampleRate));
			if(RequireNormalEvidence&&!IMW3TimingsAreFresh(*Bridge,VoiceBindings,BindingError))
				return FailGate(TEXT("fresh_timing_preflight"),BindingError);
			FreshTimingPreflightPassed=RequireNormalEvidence;
			if (RequireNormalEvidence)
			{
				if (!MetaContext.IsValid()
					|| MetaContext->EnvironmentExecuteMaximum.PublicationSequence.load(std::memory_order_acquire)
					|| MetaContext->BusExecuteMaximum.PublicationSequence.load(std::memory_order_acquire)
					|| MetaContext->BusExecuteMaxCycles.load(std::memory_order_acquire)
					|| MetaContext->BusExecuteCount.load(std::memory_order_acquire))
					return FailGate(TEXT("execute_maximum_freshness"), TEXT("Environment or bus timing observation is not empty on the fresh context."));
				for (const FIMW3VoiceBinding& Binding : VoiceBindings)
				{
					if (MetaContext->SourceExecuteMaximums[Binding.Slot].PublicationSequence.load(std::memory_order_acquire))
						return FailGate(TEXT("execute_maximum_freshness"), TEXT("Source maximum observation is not empty on the fresh context."));
				}
				MetaContext->CaptureExecuteMaximums.store(true, std::memory_order_release);
			}
			Bridge->ResetPressureDiagnostics();
			Bridge->MaxSnapshotGapUs.store(0);Bridge->MaxWorkerGapUs.store(0);
			PressureWindowCaptureRequested=RequireNormalEvidence;
			if(PressureWindowCaptureRequested)
			{
				if(!Bridge->PreparePressureWindowProbes())
					return FailGate(TEXT("pressure_window_probe_allocation"),TEXT("Optional full-window snapshot/worker probe arrays could not be allocated once on the game thread."));
				if(Bridge->PressureSnapshots.Pushes.load(std::memory_order_acquire)!=0
					||Bridge->PressureSnapshots.Overflows.load(std::memory_order_acquire)!=0
					||Bridge->PressureWorkers.Pushes.load(std::memory_order_acquire)!=0
					||Bridge->PressureWorkers.Overflows.load(std::memory_order_acquire)!=0)
					return FailGate(TEXT("pressure_window_probe_freshness"),TEXT("Optional full-window probe counters were nonzero before the measured window."));
			}
			CallbackTraceRequested=Duration<=30.0;
			// Full S5 exports the preallocated MetaSound identity trace. Keep the
			// fixed-size callback probes limited to short diagnostics.
			MetaSoundCorrelationRequested=CallbackTraceRequested||(!DropWet&&!StaleProbe&&Duration>=W3Duration);
			RareTailRequested=!DropWet&&!StaleProbe;RareTailSelfTest=RareTailRequested&&Duration<W3Duration;
			if(!Bridge->BlockFrames||!Bridge->SampleRate)
				return FailGate(TEXT("graph_timing_configuration"),TEXT("Device bridge has a zero graph block size or sample rate."));
			const uint32 DeviceBudgetUs=static_cast<uint32>(FMath::CeilToDouble(double(Bridge->BlockFrames)/Bridge->SampleRate*1.e6));
			const uint32 NearBudgetUs=RareTailSelfTest?W3RareTailSelfTestUs:static_cast<uint32>(FMath::CeilToDouble(double(DeviceBudgetUs)*W3RareTailNearBudgetFraction));
			const uint32 NearTailUs=RareTailSelfTest?W3RareTailSelfTestUs:W3RareTailNearTailUs;
			Bridge->ConfigureRareTailDiagnostics(RareTailRequested,RareTailSelfTest,DeviceBudgetUs,NearBudgetUs,NearTailUs);
			UAudioMixerBlueprintLibrary::StartRecordingOutput(World,12,nullptr);Recording=true;RecordingPaused=false;
			WavRecordingStartSeconds=FPlatformTime::Seconds();
			const FIMW3MemorySample MemoryAtMeasurementStart=IMW3CaptureMemorySample();
			MeasurementStartMemorySample=MemoryAtMeasurementStart;
			StartMemory=MemoryAtMeasurementStart.UsedPhysical;StartVirtualMemory=MemoryAtMeasurementStart.UsedVirtual;
			StartAvailablePhysical=MemoryAtMeasurementStart.AvailablePhysical;StartAvailableVirtual=MemoryAtMeasurementStart.AvailableVirtual;
			PeakMemory=MemoryAtMeasurementStart.UsedPhysical;PeakVirtualMemory=MemoryAtMeasurementStart.UsedVirtual;
			MinimumAvailablePhysical=MemoryAtMeasurementStart.AvailablePhysical;MinimumAvailableVirtual=MemoryAtMeasurementStart.AvailableVirtual;
			PeakProcessPhysical=MemoryAtMeasurementStart.PeakUsedPhysical;PeakProcessVirtual=MemoryAtMeasurementStart.PeakUsedVirtual;
			LastReverbCalls=Bridge->ReverbProcessedBlocks.load();LastReverbDry=Bridge->ReverbDryBlocks.load();
			LastReverbRaw=Bridge->ReverbRawNonzeroBlocks.load();LastReverbAudible=Bridge->ReverbNonzeroBlocks.load();
			RejectedStart=Bridge->RejectedBlocks.load(std::memory_order_acquire);
			ReverbRejectedStart=Bridge->ReverbRejectedBlocks.load(std::memory_order_acquire);
			ReverbNotFreshStart=Bridge->ReverbNotFreshBlocks.load(std::memory_order_acquire);
			GraphBusUnderrunsStart=MetaContext ? MetaContext->BusUnderruns.load(std::memory_order_acquire) : 0;
			SourceSendFailuresStart=MetaContext ? MetaContext->SourceSendPushFailures.load(std::memory_order_acquire) : 0;
			if (MetaContext)
				for (const FIMW3VoiceBinding& Binding : VoiceBindings)
					SourceSendCountsStart[Binding.Slot]=MetaContext->SourceSendBlockCounts[Binding.Slot].load(std::memory_order_acquire);
			DryDroppedStart=Bridge->DryDroppedBlocks.load(std::memory_order_acquire);
			RenderedStart=Bridge->RenderedBlocks.load(std::memory_order_acquire);
			if(PressureWindowCaptureRequested&&!Bridge->OpenPressureWindowProbes())
				return FailGate(TEXT("pressure_window_open"),TEXT("Prepared pressure logs could not open exactly once on the fresh device."));
			Bridge->CallbackDiagnosticsEnabled.store(CallbackTraceRequested,std::memory_order_release);
			Underruns.Enabled.store(true,std::memory_order_release);
			Bridge->ProfilingEnabled.store(true,std::memory_order_release);
			const double WindowStart=FPlatformTime::Seconds();
			Stage=2;StageStarted=WindowStart;MeasurementStarted=WindowStart;NextSample=WindowStart+1.0;
			return false;
		}
		if(Stage==2)
		{
			const double Elapsed=Now-StageStarted;
			// Separate negative-control test: the normal pressure command never
			// injects an outage and still requires continuous wet output.
			if(DropWet)Volume->bReverbRoute=!(Elapsed>=10&&Elapsed<13);
			// Stale-drain probe: halt GT once so snapshots age past the 250ms
			// lease while audio callbacks keep pushing dry sends. This forces a
			// genuine not-fresh IR window with zero product-path changes; the
			// verdict checks the drain fix and only the drain fix.
			if(StaleProbe&&!StaleInjected&&Elapsed>=2)
			{
				StaleN0=Bridge->ReverbNotFreshBlocks.load();StaleD0=Bridge->DryDroppedBlocks.load();StaleR0=Bridge->ReverbNonzeroBlocks.load();
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3StaleInject notfresh=%llu drydropped=%llu reverbnonzero=%llu"),StaleN0,StaleD0,StaleR0);
				FPlatformProcess::Sleep(0.7f);StaleInjected=true;
			}
			if(StaleProbe&&StaleInjected&&Elapsed>=8)
			{
				const uint64 N1=Bridge->ReverbNotFreshBlocks.load(),D1=Bridge->DryDroppedBlocks.load(),R1=Bridge->ReverbNonzeroBlocks.load();
				UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3StaleVerdict notfresh=%llu(+%llu) drydropped=%llu(+%llu) reverbnonzero=%llu(+%llu)"),N1,N1-StaleN0,D1,D1-StaleD0,R1,R1-StaleR0);
				AppendTimeseriesSample(Elapsed);
				const bool TimeseriesSaved=FFileHelper::SaveStringToFile(CSV,*FPaths::Combine(Directory,TEXT("pressure-timeseries.csv")));
				const FString JS=FString::Printf(TEXT("{\"notfresh_before\":%llu,\"notfresh_after\":%llu,\"drydropped_before\":%llu,\"drydropped_after\":%llu,\"reverbnonzero_before\":%llu,\"reverbnonzero_after\":%llu,\"memory_timeseries_saved\":%s}"),
					StaleN0,N1,StaleD0,D1,StaleR0,R1,TimeseriesSaved?TEXT("true"):TEXT("false"));
				const bool StaleReportSaved=FFileHelper::SaveStringToFile(JS,*FPaths::Combine(Directory,TEXT("stale-probe.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				if(!TimeseriesSaved||!StaleReportSaved)return Finish(false,TEXT("Stale-drain probe evidence could not be saved."));
				if(N1==StaleN0)return Finish(false,TEXT("Stale-drain probe INCONCLUSIVE: injection produced no not-fresh window."));
				if(D1!=StaleD0)return Finish(false,TEXT("Stale-drain probe FAIL: dry sends dropped during not-fresh window."));
				if(R1<=StaleR0)return Finish(false,TEXT("Stale-drain probe FAIL: reverb did not recover after stale window."));
				return Finish(true,TEXT("Stale-drain probe PASS: not-fresh window drained without dry drops and reverb recovered."));
			}
			if(Recording&&!RecordingPaused&&Elapsed>=10)
			{
				// Keep the 10-second master capture, but defer WAV conversion
				// and file export until after the measured pressure window.
				UAudioMixerBlueprintLibrary::PauseRecordingOutput(World,nullptr);
				RecordingPaused=true;WavRecordingStopSeconds=FPlatformTime::Seconds();
			}
			if(Now>=NextSample)
			{
				NextSample=Now+1;AppendTimeseriesSample(Elapsed);
			}
			if(Elapsed<Duration)return false;
			Bridge->ProfilingEnabled.store(false,std::memory_order_release);
			Bridge->CallbackDiagnosticsEnabled.store(false,std::memory_order_release);
			Bridge->RareTailDiagnosticsEnabled.store(false,std::memory_order_release);
			Bridge->ClosePressureWindowProbes();
			Underruns.Enabled.store(false,std::memory_order_release);
			MeasurementEnded=FPlatformTime::Seconds();
			MeasuredSeconds=MeasurementEnded-MeasurementStarted;
			FString EndIdentityError;
			VoiceIdentityStableAtWindowEnd=IMW3VoiceBindingsStillCurrent(MetaContext,Bridge,VoiceBindings,EndIdentityError);
			if(!VoiceIdentityStableAtWindowEnd)VoiceIdentityFailure=EndIdentityError;
			// End the measured source load before either WAV or trace export.
			for(const auto& Source:Sources)if(Source.IsValid())Source->Stop();
			if(Recording)
			{
				if(!RecordingPaused)WavRecordingStopSeconds=FPlatformTime::Seconds();
				UAudioMixerBlueprintLibrary::StopRecordingOutput(World,EAudioRecordingExportType::WavFile,TEXT("IM_complex_16voices"),Directory,nullptr,nullptr);
				Recording=false;
			}
			Stage=3;StageStarted=MeasurementEnded;
			return false;
		}
		if(Stage==3&&Now-StageStarted>.3)
		{
			if(PressureWindowCaptureRequested&&!Bridge->PressureWindowPublicationsQuiescent())
			{
				if(Now-StageStarted>10.0)
					return FailGate(TEXT("pressure_window_publication_barrier"),TEXT("An admitted pressure-log writer has not left the closed window."));
				return false;
			}
			if(RequireNormalEvidence&&!WavExportResolved)
			{
				WavSummary=IMW3InspectRecordedWav(WavPath);
				if(!WavSummary.Complete&&Now-StageStarted<15.0)return false;
				WavExportResolved=true;
			}
			// Profiling is disabled before serialization; allow in-flight callback
			// scopes to retire so histogram totals are stable without audio locks.
			const uint64 RejectedAfterDrain=Bridge->RejectedBlocks.load(std::memory_order_acquire);
			const uint64 ReverbRejectedAfterDrain=Bridge->ReverbRejectedBlocks.load(std::memory_order_acquire);
			const uint64 ReverbNotFreshAfterDrain=Bridge->ReverbNotFreshBlocks.load(std::memory_order_acquire);
			const uint64 DryDroppedAfterDrain=Bridge->DryDroppedBlocks.load(std::memory_order_acquire);
			const double GraphDeadlineUs=GraphSampleRate>0.0f?double(GraphBlockFrames)/GraphSampleRate*1.e6:0.0;
			const double MixerDeadlineUs=MixerDeadlineAvailable?double(MixerBlockFrames)/MixerSampleRate*1.e6:0.0;
			const double ExpectedPerVoice=GraphBlockFrames&&GraphSampleRate>0.0f?MeasuredSeconds*GraphSampleRate/GraphBlockFrames:0.0;
			const double Expected=ExpectedPerVoice*W3Voices;
			uint64 SourceMaxCycles=0;
			FString VoiceBudgetError;
			const bool VoiceBudgetsRead=IMW3ReadVoiceBudgets(Bridge,VoiceBindings,VoiceBudgets,SourceMaxCycles,VoiceBudgetError);
			const uint64 ReverbMaxCycles=Bridge->ReverbTiming.MaxCycles.load(std::memory_order_acquire);
			const uint64 BusMaxCycles=MetaContext.IsValid()?MetaContext->BusExecuteMaxCycles.load(std::memory_order_acquire):0;
			const uint64 BusExecuteCount=MetaContext.IsValid()?MetaContext->BusExecuteCount.load(std::memory_order_acquire):0;
			// The mono bus reader is a separate graph operator. Its complete
			// Execute must be included rather than hidden inside the IR budget.
			const bool BusExecuteBudgetComplete=!RequireNormalEvidence
				||(BusMaxCycles>0&&double(BusExecuteCount)>=ExpectedPerVoice*.9);
			TArray<FIMAcousticExecuteMaximumSample> SourceMaximumSamples;
			FIMAcousticExecuteMaximumSample EnvironmentMaximumSample;
			FIMAcousticExecuteMaximumSample BusMaximumSample;
			bool ExecuteMaximumObservationComplete = !RequireNormalEvidence;
			FString ExecuteMaximumObservationError;
			if (RequireNormalEvidence && MetaContext.IsValid() && VoiceBudgetsRead)
			{
				ExecuteMaximumObservationComplete = true;
				for (const FIMW3VoiceBudget& Budget : VoiceBudgets)
				{
					FIMAcousticExecuteMaximumSample Sample;
					const bool bRead = MetaContext->SourceExecuteMaximums[Budget.Binding.Slot].ReadComplete(Sample);
					const bool bMatches = bRead && Sample.WholeCycles == Budget.MaxCycles
						&& Sample.AudioComponentId == Budget.Binding.AudioComponentId
						&& Sample.Slot == Budget.Binding.Slot
						&& Sample.VoiceGeneration == Budget.Binding.LiveGeneration
						&& Sample.WorldGeneration == GraphWorldGeneration && Sample.ThreadId;
					ExecuteMaximumObservationComplete &= bMatches;
					if (!bMatches && ExecuteMaximumObservationError.IsEmpty())
						ExecuteMaximumObservationError = FString::Printf(TEXT("Source maximum sample is incomplete or differs from budget/identity at slot %u."), Budget.Binding.Slot);
					SourceMaximumSamples.Add(Sample);
				}
				const bool bEnvironmentRead = MetaContext->EnvironmentExecuteMaximum.ReadComplete(EnvironmentMaximumSample);
				const bool bEnvironmentMatches = bEnvironmentRead
					&& EnvironmentMaximumSample.WholeCycles == ReverbMaxCycles
					&& EnvironmentMaximumSample.WorldGeneration == GraphWorldGeneration
					&& EnvironmentMaximumSample.ThreadId;
				ExecuteMaximumObservationComplete &= bEnvironmentMatches;
				if (!bEnvironmentMatches && ExecuteMaximumObservationError.IsEmpty())
					ExecuteMaximumObservationError = TEXT("Environment maximum sample is incomplete or differs from the whole-Execute budget maximum.");
				const bool bBusRead = MetaContext->BusExecuteMaximum.ReadComplete(BusMaximumSample);
				const bool bBusMatches = bBusRead && BusExecuteBudgetComplete
					&& BusMaximumSample.WholeCycles == BusMaxCycles
					&& BusMaximumSample.WorldGeneration == GraphWorldGeneration
					&& BusMaximumSample.ThreadId;
				ExecuteMaximumObservationComplete &= bBusMatches;
				if (!bBusMatches && ExecuteMaximumObservationError.IsEmpty())
					ExecuteMaximumObservationError = TEXT("Bus maximum sample is incomplete or differs from the whole-Execute budget maximum/count.");
			}
			else if (RequireNormalEvidence)
			{
				ExecuteMaximumObservationError = TEXT("Maximum observation has no current graph context or voice budgets.");
			}
			const double ConservativeGraphBoundUs=VoiceBudgetsRead
				?FPlatformTime::ToSeconds64(SourceMaxCycles+ReverbMaxCycles+BusMaxCycles)*1.e6:0.0;
			const double ConservativeMixerBoundUs=ConservativeGraphBoundUs*GraphBlocksPerMixer;
			bool VoiceCountsGateOk=VoiceBudgetsRead&&VoiceBudgets.Num()==W3Voices;
			for(const FIMW3VoiceBudget& V:VoiceBudgets)
				VoiceCountsGateOk=VoiceCountsGateOk&&double(V.ExecuteCount)>=ExpectedPerVoice*.9;
			const bool VoiceIdentityGateOk=VoiceIdentityStableAtWindowEnd&&VoiceIdentityFailure.IsEmpty();
			FIMW3DiagnosticTraceSummary DiagnosticTrace;
			bool DiagnosticMetaSoundCorrelationSaved=false;
			if(RequireNormalEvidence)
			{
				DiagnosticTrace=IMW3ValidateDiagnosticTrace(MetaContext,MeasurementStarted,MeasurementEnded,Expected,VoiceBindings);
				DiagnosticMetaSoundCorrelationSaved=MetaContext.IsValid()&&W3ExportMetaSoundCorrelation(MetaContext,Directory);
			}
			PressureWindowRawSaved=!PressureWindowCaptureRequested
				||IMW3ExportPressureWindowLogs(Bridge.Get(),Directory,MeasurementStarted,MeasurementEnded,PressureWindowSummary);
			const FIMW3MemorySample FinalMemorySample=IMW3CaptureMemorySample();
			FString Json;auto W=TJsonWriterFactory<>::Create(&Json);W->WriteObjectStart();
			W->WriteValue(TEXT("duration_s"),MeasuredSeconds);W->WriteValue(TEXT("measurement_started_s"),MeasurementStarted);W->WriteValue(TEXT("measurement_ended_s"),MeasurementEnded);W->WriteValue(TEXT("voices"),W3Voices);
			const bool HeadlessDiagnostic=FParse::Param(FCommandLine::Get(),TEXT("nullrhi"));
			W->WriteValue(TEXT("nullrhi_diagnostic_only"),HeadlessDiagnostic);
			W->WriteValue(TEXT("sample_rate"),Bridge->SampleRate);W->WriteValue(TEXT("block_frames"),Bridge->BlockFrames);
			W->WriteValue(TEXT("graph_block_frames"),GraphBlockFrames);W->WriteValue(TEXT("graph_sample_rate"),GraphSampleRate);
			W->WriteValue(TEXT("graph_block_deadline_us"),GraphDeadlineUs);
			W->WriteValue(TEXT("mixer_block_frames"),MixerBlockFrames);W->WriteValue(TEXT("mixer_sample_rate"),MixerSampleRate);
			W->WriteValue(TEXT("mixer_block_deadline_us"),MixerDeadlineUs);
			W->WriteValue(TEXT("graph_blocks_per_mixer_callback"),GraphBlocksPerMixer);
			W->WriteValue(TEXT("graph_frame_contract_valid"),GraphBlockFrames==FIMAcousticMetaSoundContext::Frames&&GraphSampleRate>0.0f);
			W->WriteValue(TEXT("mixer_deadline_available"),MixerDeadlineAvailable);
			W->WriteValue(TEXT("mixer_geometry_contract_valid"),MixerGeometryGateOk);
			W->WriteValue(TEXT("graph_deadline_origin"),TEXT("MetaSound bridge BlockFrames / SampleRate."));
			W->WriteValue(TEXT("mixer_deadline_origin"),TEXT("FAudioDeviceManager world mixer GetNumOutputFrames() / GetSampleRate()."));
			W->WriteValue(TEXT("memory_sampling_semantics"),TEXT("FPlatformMemory::GetStats: Used/Peak fields describe this process; Available fields describe system headroom. Formal window samples are approximately 1 Hz; process peaks are lifetime counters observed at each sample."));
			W->WriteValue(TEXT("memory_sample_interval_s"),1.0);
			auto WriteMemory=[&W](const TCHAR* Name,const FIMW3MemorySample& M)
			{
				W->WriteObjectStart(Name);W->WriteValue(TEXT("used_physical_process_bytes"),double(M.UsedPhysical));
				W->WriteValue(TEXT("used_virtual_commit_process_bytes"),double(M.UsedVirtual));
				W->WriteValue(TEXT("available_physical_system_bytes"),double(M.AvailablePhysical));
				W->WriteValue(TEXT("available_virtual_system_bytes"),double(M.AvailableVirtual));
				W->WriteValue(TEXT("peak_used_physical_process_lifetime_bytes"),double(M.PeakUsedPhysical));
				W->WriteValue(TEXT("peak_used_virtual_process_lifetime_bytes"),double(M.PeakUsedVirtual));W->WriteObjectEnd();
			};
			WriteMemory(TEXT("memory_before_capture"),PreCaptureMemory);
			WriteMemory(TEXT("memory_formal_window_start"),MeasurementStartMemorySample);
			WriteMemory(TEXT("memory_after_metasound_trace_export_before_report_serialization"),FinalMemorySample);
			W->WriteValue(TEXT("formal_window_sample_peak_used_physical_process_bytes"),double(PeakMemory));
			W->WriteValue(TEXT("formal_window_sample_peak_used_virtual_commit_process_bytes"),double(PeakVirtualMemory));
			W->WriteValue(TEXT("formal_window_min_available_physical_system_bytes"),double(MinimumAvailablePhysical));
			W->WriteValue(TEXT("formal_window_min_available_virtual_system_bytes"),double(MinimumAvailableVirtual));
			W->WriteValue(TEXT("formal_window_max_observed_process_lifetime_peak_physical_bytes"),double(PeakProcessPhysical));
			W->WriteValue(TEXT("formal_window_max_observed_process_lifetime_peak_virtual_bytes"),double(PeakProcessVirtual));
			W->WriteValue(TEXT("fresh_timing_preflight_passed"),FreshTimingPreflightPassed);
			W->WriteValue(TEXT("graph_setup_binding_wait_seconds"),GraphSetupBindingWaitSeconds);
			W->WriteValue(TEXT("graph_warmup_audio_device_id"),double(GraphDeviceId));
			W->WriteValue(TEXT("graph_warmup_world_generation"),double(GraphWorldGeneration));
			W->WriteValue(TEXT("graph_warmup_baseline_voice_count"),W3Voices);
			W->WriteValue(TEXT("voice_identity_stable_through_measurement"),VoiceIdentityGateOk);
			W->WriteValue(TEXT("voice_identity_failure"),VoiceIdentityFailure);
			W->WriteArrayStart(TEXT("voice_slot_bindings"));
			for(const FIMW3VoiceBinding& Binding:VoiceBindings)
			{
				W->WriteObjectStart();W->WriteValue(TEXT("audio_component_id"),LexToString(Binding.AudioComponentId));
				W->WriteValue(TEXT("slot"),Binding.Slot);W->WriteValue(TEXT("live_generation"),LexToString(Binding.LiveGeneration));W->WriteObjectEnd();
			}
			W->WriteArrayEnd();
			W->WriteArrayStart(TEXT("voice_source_budget"));
			for(const FIMW3VoiceBudget& Budget:VoiceBudgets)
			{
				W->WriteObjectStart();W->WriteValue(TEXT("audio_component_id"),LexToString(Budget.Binding.AudioComponentId));
				W->WriteValue(TEXT("slot"),Budget.Binding.Slot);W->WriteValue(TEXT("live_generation"),LexToString(Budget.Binding.LiveGeneration));
				W->WriteValue(TEXT("execute_count"),double(Budget.ExecuteCount));W->WriteValue(TEXT("max_cycles"),double(Budget.MaxCycles));
				W->WriteObjectEnd();
			}
			W->WriteArrayEnd();
			W->WriteValue(TEXT("expected_source_executes_per_voice"),ExpectedPerVoice);
			W->WriteValue(TEXT("minimum_source_executes_per_voice"),ExpectedPerVoice*.9);
			W->WriteValue(TEXT("all_voice_execute_count_gate"),VoiceCountsGateOk);
			W->WriteValue(TEXT("voice_source_max_cycles_sum"),double(SourceMaxCycles));
			W->WriteValue(TEXT("environment_reverb_max_cycles"),double(ReverbMaxCycles));
			W->WriteValue(TEXT("bus_execute_max_cycles"),double(BusMaxCycles));
			W->WriteValue(TEXT("bus_execute_count"),double(BusExecuteCount));
			W->WriteValue(TEXT("bus_execute_budget_complete"),BusExecuteBudgetComplete);
			W->WriteValue(TEXT("execute_maximum_observation_requested"),RequireNormalEvidence);
			W->WriteValue(TEXT("execute_maximum_observation_complete"),ExecuteMaximumObservationComplete);
			W->WriteValue(TEXT("execute_maximum_observation_error"),ExecuteMaximumObservationError);
			W->WriteValue(TEXT("execute_maximum_observation_semantics"),TEXT("One bounded event per source, environment and bus reader, at that owner's largest whole Execute in this fresh window. Stage durations include wall-clock scheduling delay and the optional timing reads. Source render includes send/fallback; environment render includes gain/fallback; bus render includes the complete patch read. The budget includes all whole-Execute maxima and output/PCM/trace tails. This does not measure a complete mixer callback or establish preemption."));
			const auto WriteMaximumSample = [&](const FIMAcousticExecuteMaximumSample& Sample)
			{
				W->WriteValue(TEXT("audio_component_id"),FString::Printf(TEXT("%llu"),Sample.AudioComponentId));
				W->WriteValue(TEXT("slot"),double(Sample.Slot));
				W->WriteValue(TEXT("voice_generation"),FString::Printf(TEXT("%llu"),Sample.VoiceGeneration));
				W->WriteValue(TEXT("world_generation"),FString::Printf(TEXT("%llu"),Sample.WorldGeneration));
				W->WriteValue(TEXT("spatial_version"),FString::Printf(TEXT("%llu"),Sample.SpatialVersion));
				W->WriteValue(TEXT("result_sequence"),FString::Printf(TEXT("%llu"),Sample.Sequence));
				W->WriteValue(TEXT("thread_id"),double(Sample.ThreadId));
				W->WriteValue(TEXT("begin_seconds"),Sample.BeginSeconds);
				W->WriteValue(TEXT("fresh"),Sample.Fresh!=0);
				W->WriteValue(TEXT("ordered_stages"),Sample.HasOrderedStages());
				W->WriteValue(TEXT("whole_cycles"),double(Sample.WholeCycles));
				W->WriteValue(TEXT("whole_us"),FPlatformTime::ToSeconds64(Sample.WholeCycles)*1.e6);
				W->WriteValue(TEXT("setup_and_identity_us"),Sample.HasOrderedStages()?FPlatformTime::ToSeconds64(Sample.BeforeRenderCycles-Sample.StartCycles)*1.e6:0.0);
				W->WriteValue(TEXT("render_send_or_gain_us"),Sample.HasOrderedStages()?FPlatformTime::ToSeconds64(Sample.AfterRenderCycles-Sample.BeforeRenderCycles)*1.e6:0.0);
				W->WriteValue(TEXT("output_and_trace_us"),Sample.HasOrderedStages()?FPlatformTime::ToSeconds64(Sample.WholeCycles-(Sample.AfterRenderCycles-Sample.StartCycles))*1.e6:0.0);
			};
			W->WriteArrayStart(TEXT("source_execute_maximum_events"));
			for (const FIMAcousticExecuteMaximumSample& Sample : SourceMaximumSamples)
			{
				W->WriteObjectStart(); WriteMaximumSample(Sample); W->WriteObjectEnd();
			}
			W->WriteArrayEnd();
			W->WriteObjectStart(TEXT("environment_execute_maximum_event"));
			WriteMaximumSample(EnvironmentMaximumSample);
			W->WriteObjectEnd();
			W->WriteObjectStart(TEXT("bus_execute_maximum_event"));
			WriteMaximumSample(BusMaximumSample);
			W->WriteObjectEnd();
			W->WriteValue(TEXT("conservative_graph_feature_bound_us"),ConservativeGraphBoundUs);
			W->WriteValue(TEXT("conservative_mixer_feature_bound_us"),ConservativeMixerBoundUs);
			W->WriteValue(TEXT("conservative_bound_semantics"),TEXT("Graph bound = sum of the 16 participating voices' maximum whole Execute cycles + environment ReverbTiming.MaxCycles + bus-reader maximum whole Execute cycles. Mixer bound = graph bound multiplied by the integer graph-blocks-per-mixer callback ratio. A bound below its deadline with complete costs is a sufficient conservative budget pass. If a bound is at or above its deadline, only the conservative budget gate is unmet; that does not establish an actual mixer callback overrun or identify a DSP bottleneck. This is a feature CPU upper bound, not a measured whole mixer callback, whole-engine cost, or per-device epoch aggregate."));
			W->WriteValue(TEXT("conservative_graph_bound_below_graph_deadline"),VoiceBudgetsRead&&BusExecuteBudgetComplete&&ConservativeGraphBoundUs<GraphDeadlineUs);
			W->WriteValue(TEXT("conservative_mixer_bound_below_mixer_deadline"),MixerGeometryGateOk&&BusExecuteBudgetComplete&&ConservativeMixerBoundUs<MixerDeadlineUs);
			W->WriteValue(TEXT("graph_source_asset"),FString(IMAcousticTestSupport::GraphSourceAssetPath));
			W->WriteValue(TEXT("graph_source_assets_match"),GraphSourceIdentityValid);
			W->WriteValue(TEXT("graph_source_component_count"),Sources.Num());
			W->WriteValue(TEXT("graph_source_unique_audio_component_ids"),ExpectedAudioComponentIds.Num());
			W->WriteValue(TEXT("metasound_context_available_at_window_start"),DiagnosticMetaContextAtMeasurementStart);
			W->WriteValue(TEXT("metasound_context_available_at_export"),DiagnosticTrace.ContextAvailable);
			W->WriteValue(TEXT("metasound_trace_complete_no_overflow"),DiagnosticTrace.TraceComplete);
			W->WriteValue(TEXT("metasound_trace_source_rows"),DiagnosticTrace.SourceRows);
			W->WriteValue(TEXT("metasound_trace_source_claimed"),DiagnosticTrace.SourceClaimed);
			W->WriteValue(TEXT("metasound_trace_environment_rows"),DiagnosticTrace.EnvironmentRows);
			W->WriteValue(TEXT("metasound_trace_environment_claimed"),double(DiagnosticTrace.EnvironmentClaimed));
			W->WriteValue(TEXT("formal_trace_window_start_s"),MeasurementStarted);
			W->WriteValue(TEXT("formal_trace_window_end_s"),MeasurementEnded);
			W->WriteValue(TEXT("formal_trace_rows"),DiagnosticTrace.FormalRows);
			W->WriteValue(TEXT("formal_trace_unique_voices"),DiagnosticTrace.UniqueVoices);
			W->WriteValue(TEXT("formal_trace_unique_audio_component_ids"),DiagnosticTrace.UniqueAudioIds);
			W->WriteValue(TEXT("formal_trace_identity_mismatch_rows"),DiagnosticTrace.IdentityMismatchRows);
			W->WriteValue(TEXT("formal_trace_slot_mismatch_rows"),DiagnosticTrace.SlotMismatchRows);
			W->WriteValue(TEXT("formal_trace_generation_mismatch_rows"),DiagnosticTrace.GenerationMismatchRows);
			W->WriteValue(TEXT("formal_trace_rejected_rows"),DiagnosticTrace.RejectedRows);
			W->WriteValue(TEXT("formal_trace_delay_priming_rows"),DiagnosticTrace.PrimingRows);
			W->WriteValue(TEXT("formal_trace_route_mask_or"),DiagnosticTrace.RouteMaskOr);
			W->WriteValue(TEXT("formal_trace_window_valid"),DiagnosticTrace.FormalWindowValid);
			W->WriteValue(TEXT("formal_trace_expected_component_ids_present"),DiagnosticTrace.ExpectedSourcesPresent);
			W->WriteValue(TEXT("metasound_correlation_files_saved"),DiagnosticMetaSoundCorrelationSaved);
			W->WriteValue(TEXT("pressure_window_probes_requested"),PressureWindowCaptureRequested);
			W->WriteValue(TEXT("pressure_window_probe_gate_passed"),PressureWindowRawSaved);
			W->WriteValue(TEXT("pressure_window_publication_barrier_closed"),Bridge->PressureWindowPublicationsQuiescent());
			W->WriteValue(TEXT("pressure_window_probe_files_saved"),PressureWindowSummary.FilesSaved);
			W->WriteValue(TEXT("pressure_window_probe_allocated_bytes"),double(PressureWindowSummary.AllocatedBytes));
			W->WriteValue(TEXT("pressure_window_capture_start_s"),MeasurementStarted);
			W->WriteValue(TEXT("pressure_window_capture_end_s"),MeasurementEnded);
			W->WriteValue(TEXT("pressure_window_worker_boundary_note"),TEXT("A worker-loop row can begin before capture is enabled or before the measured end and publish after that boundary. Its LoopStartSeconds/WaitEndSeconds interval is retained; start coverage uses the first complete row's LoopStartSeconds and end coverage uses the last complete row's WaitEndSeconds."));
			auto WriteWindowLog=[&](const TCHAR* Name,const FIMW3WindowLogSummary& Log)
			{
				W->WriteObjectStart(Name);W->WriteValue(TEXT("allocated"),Log.Allocated);W->WriteValue(TEXT("complete_prefix"),Log.Complete);
				W->WriteValue(TEXT("no_overflow"),Log.NoOverflow);W->WriteValue(TEXT("pushes"),double(Log.Pushes));
				W->WriteValue(TEXT("exported_complete_rows"),double(Log.Exported));W->WriteValue(TEXT("overflows"),double(Log.Overflows));
				W->WriteValue(TEXT("capture_start_s"),MeasurementStarted);W->WriteValue(TEXT("capture_end_s"),MeasurementEnded);
				W->WriteValue(TEXT("first_wall_clock_s"),Log.FirstWallClockSeconds);W->WriteValue(TEXT("last_wall_clock_s"),Log.LastWallClockSeconds);
				W->WriteValue(TEXT("covers_window_start_within_1s"),Log.CoversStart);W->WriteValue(TEXT("covers_window_end_within_1s"),Log.CoversEnd);
				W->WriteValue(TEXT("file_saved"),Log.FileSaved);W->WriteObjectEnd();
			};
			WriteWindowLog(TEXT("pressure_gt_snapshots"),PressureWindowSummary.Snapshots);
			WriteWindowLog(TEXT("pressure_worker_loops"),PressureWindowSummary.Workers);
			if(RequireNormalEvidence)
			{
				W->WriteValue(TEXT("wav_recording_path"),WavPath);
				W->WriteValue(TEXT("wav_recording_window_start_relative_s"),WavRecordingStartSeconds-MeasurementStarted);
				W->WriteValue(TEXT("wav_recording_window_end_relative_s"),WavRecordingStopSeconds-MeasurementStarted);
				W->WriteValue(TEXT("wav_file_exists"),WavSummary.FileExists);W->WriteValue(TEXT("wav_file_complete_pcm16"),WavSummary.Complete);
				W->WriteValue(TEXT("wav_file_bytes"),double(WavSummary.FileBytes));W->WriteValue(TEXT("wav_data_bytes"),double(WavSummary.DataBytes));
				W->WriteValue(TEXT("wav_sample_rate"),WavSummary.SampleRate);W->WriteValue(TEXT("wav_channels"),WavSummary.Channels);
				W->WriteValue(TEXT("wav_duration_s"),WavSummary.DurationSeconds);W->WriteValue(TEXT("wav_nonzero_pcm16_samples"),double(WavSummary.NonzeroSamples));
				W->WriteValue(TEXT("wav_capture_scope"),TEXT("Master-output WAV starts immediately before the formal window and is paused once elapsed time reaches 10 seconds; reported relative start/end carry the exact measured offsets. It is an auxiliary capture for the 30-second diagnostic and the 600-second pressure run."));
			}
			auto Stats=[&W](const TCHAR* Name,const FIMAcousticTiming& T)
			{
				const uint64 Count=T.Count.load();uint64 Cumulative=0;double P99=-1;
				W->WriteObjectStart(Name);W->WriteValue(TEXT("count"),double(Count));W->WriteValue(TEXT("bin_width_us"),T.BinMicroseconds);
				W->WriteArrayStart(TEXT("histogram_counts"));
				for(uint32 I=0;I<T.BinCount;++I){const uint64 N=T.Bins[I].load();W->WriteValue(double(N));Cumulative+=N;if(P99<0&&Count&&Cumulative>=uint64(FMath::CeilToDouble(Count*.99)))P99=I==T.BinCount-1?-2:(I+1)*T.BinMicroseconds;}
				W->WriteArrayEnd();W->WriteValue(TEXT("p99_upper_us"),P99);
				W->WriteValue(TEXT("max_us"),FPlatformTime::ToSeconds64(T.MaxCycles.load())*1.e6);
				W->WriteValue(TEXT("mean_us"),Count?FPlatformTime::ToSeconds64(T.TotalCycles.load())*1.e6/Count:0);W->WriteObjectEnd();
			};
			Stats(TEXT("source_callback"),Bridge->SourceTiming);Stats(TEXT("single_source_execute_legacy_alias"),Bridge->SourceBlockTiming);
			Stats(TEXT("reverb_callback"),Bridge->ReverbTiming);Stats(TEXT("worker_update"),Bridge->WorkerTiming);Stats(TEXT("gt_snapshot"),Bridge->GameThreadTiming);
			// Sub-phase diagnostics (zero behavior; histograms only, no gate input).
			Stats(TEXT("worker_snapshot"),Bridge->WorkerSnapshotTiming);Stats(TEXT("worker_solve"),Bridge->WorkerSolveTiming);
			Stats(TEXT("worker_reverb"),Bridge->WorkerReverbTiming);Stats(TEXT("worker_publish"),Bridge->WorkerPublishTiming);
			Stats(TEXT("gt_traverse"),Bridge->GTTraverseTiming);Stats(TEXT("gt_submit"),Bridge->GTSubmitTiming);
			W->WriteValue(TEXT("underrun_log_events"),double(Underruns.Count.load()));
			W->WriteValue(TEXT("graph_bus_underruns_total"),MetaContext ? double(MetaContext->BusUnderruns.load(std::memory_order_acquire)) : -1.0);
			W->WriteValue(TEXT("graph_bus_underruns_after_warmup"),MetaContext ? double(MetaContext->BusUnderruns.load(std::memory_order_acquire)-GraphBusUnderrunsStart) : -1.0);
			W->WriteValue(TEXT("source_send_push_failures_total"),MetaContext ? double(MetaContext->SourceSendPushFailures.load(std::memory_order_acquire)) : -1.0);
			W->WriteValue(TEXT("source_send_push_failures_after_warmup"),MetaContext ? double(MetaContext->SourceSendPushFailures.load(std::memory_order_acquire)-SourceSendFailuresStart) : -1.0);
			W->WriteValue(TEXT("source_send_transport_failed"),!MetaContext || MetaContext->SourceSendTransportFailed.load(std::memory_order_acquire));
			W->WriteValue(TEXT("source_send_contribution_scope"),TEXT("Warmup baseline through post-window drain; real existing voice bindings, not exact formal-window sample counters."));
			W->WriteArrayStart(TEXT("source_send_contributions"));
			if (MetaContext)
				for (const FIMW3VoiceBinding& Binding : VoiceBindings)
				{
					W->WriteObjectStart();
					W->WriteValue(TEXT("voice"),Binding.Slot);
					W->WriteValue(TEXT("audio_component_id"),double(Binding.AudioComponentId));
					W->WriteValue(TEXT("pushed_blocks_after_warmup"),double(MetaContext->SourceSendBlockCounts[Binding.Slot].load(std::memory_order_acquire)-SourceSendCountsStart[Binding.Slot]));
					W->WriteObjectEnd();
				}
			W->WriteArrayEnd();
			W->WriteValue(TEXT("callback_trace_requested"),CallbackTraceRequested);W->WriteValue(TEXT("metasound_correlation_requested"),MetaSoundCorrelationRequested);W->WriteValue(TEXT("callback_trace_records"),double(Bridge->CallbackProbePushes.load()));W->WriteValue(TEXT("callback_trace_overflows"),double(Bridge->CallbackProbeOverflows.load()));
			W->WriteValue(TEXT("rare_tail_requested"),RareTailRequested);W->WriteValue(TEXT("rare_tail_self_test"),RareTailSelfTest);
			W->WriteValue(TEXT("rare_tail_device_budget_us"),double(Bridge->RareTailDeviceBudgetUs.load()));W->WriteValue(TEXT("rare_tail_near_budget_us"),double(Bridge->RareTailNearBudgetUs.load()));W->WriteValue(TEXT("rare_tail_near_tail_us"),double(Bridge->RareTailNearTailUs.load()));
			W->WriteValue(TEXT("rare_tail_probe_records"),double(Bridge->RareTailProbePushes.load()));W->WriteValue(TEXT("rare_tail_probe_overflows"),double(Bridge->RareTailProbeOverflows.load()));
			W->WriteValue(TEXT("rejected_blocks"),double(RejectedAfterDrain-RejectedStart));
			W->WriteValue(TEXT("reverb_rejected_blocks"),double(ReverbRejectedAfterDrain-ReverbRejectedStart));
			W->WriteValue(TEXT("reverb_not_fresh_blocks"),double(ReverbNotFreshAfterDrain-ReverbNotFreshStart));
			W->WriteValue(TEXT("environment_block_continuity_gate"),!RequireNormalEvidence||ReverbNotFreshAfterDrain==ReverbNotFreshStart);
			W->WriteValue(TEXT("dry_dropped_blocks"),double(DryDroppedAfterDrain-DryDroppedStart));
			W->WriteValue(TEXT("counter_window_semantics"),TEXT("Rejected, reverb-rejected and dry-drop deltas use the saved pre-window counters and are read after the existing 0.3-second drain; they can include in-flight callback completion after measurement_ended_s. Trace rows, execute counts and conservative timing maxima are bounded to the profiling window plus callback retirement."));
			W->WriteValue(TEXT("max_gt_snapshot_gap_us"),double(Bridge->MaxSnapshotGapUs.load()));W->WriteValue(TEXT("max_worker_gap_us"),double(Bridge->MaxWorkerGapUs.load()));
			const uint64 FirstRejectUs=Bridge->FirstPressureRejectTimeUs.load(std::memory_order_acquire);
			const uint64 FirstReverbRejectUs=Bridge->FirstPressureReverbRejectTimeUs.load(std::memory_order_acquire);
			W->WriteValue(TEXT("first_reject_time_s"),FirstRejectUs?double(FirstRejectUs)*1.e-6-MeasurementStarted:-1.0);
			W->WriteValue(TEXT("first_reject_detail"),double(Bridge->FirstPressureRejectDetail.load(std::memory_order_relaxed)));
			W->WriteValue(TEXT("first_reject_voice"),double(Bridge->FirstPressureRejectVoice.load(std::memory_order_relaxed)));
			W->WriteValue(TEXT("first_reverb_reject_time_s"),FirstReverbRejectUs?double(FirstReverbRejectUs)*1.e-6-MeasurementStarted:-1.0);
			W->WriteValue(TEXT("first_reverb_reject_reason"),double(Bridge->FirstPressureReverbRejectReason.load(std::memory_order_relaxed)));
			W->WriteObjectStart(TEXT("pressure_reject_detail_counts"));
			for(uint32 I=0;I<FIMAcousticDeviceBridge::PressureRejectDetailCapacity;++I)
			{
				const uint64 Count=Bridge->PressureRejectDetails[I].load(std::memory_order_relaxed);
				if(Count)W->WriteValue(FString::Printf(TEXT("%u"),I),double(Count));
			}
			W->WriteObjectEnd();
			W->WriteArrayStart(TEXT("pressure_reverb_reject_reason_counts"));
			for(const auto& Count:Bridge->PressureReverbRejectReasons)W->WriteValue(double(Count.load(std::memory_order_relaxed)));
			W->WriteArrayEnd();
			W->WriteValue(TEXT("stale_result_blocks_including_warmup"),double(Bridge->StaleResultBlocks.load()));
			W->WriteValue(TEXT("missing_result_blocks_including_warmup"),double(Bridge->MissingResultBlocks.load()));
			W->WriteValue(TEXT("invalid_result_blocks_including_warmup"),double(Bridge->InvalidResultBlocks.load()));
			W->WriteValue(TEXT("renderer_failed_blocks_including_warmup"),double(Bridge->RendererFailedBlocks.load()));
			W->WriteArrayStart(TEXT("render_failure_counts_none_frame_input_direct_path"));
			for(const auto& Count:Bridge->RenderFailures)W->WriteValue(double(Count.load()));
			W->WriteArrayEnd();
			W->WriteValue(TEXT("previous_background_throttle"),PreviousThrottle);
			W->WriteValue(TEXT("continuous_reverb_each_sample"),ContinuousReverb);
			W->WriteValue(TEXT("injected_wet_outage_control"),DropWet);
			W->WriteValue(TEXT("wet_outage_detected"),WetDropDetected);W->WriteValue(TEXT("wet_recovery_observed"),WetRecoveryObserved);
			W->WriteValue(TEXT("start_physical_bytes"),double(StartMemory));W->WriteValue(TEXT("peak_physical_bytes"),double(PeakMemory));
			W->WriteObjectEnd();W->Close();
			FString CallbackTraceCSV;const bool CallbackTraceComplete=!CallbackTraceRequested||Bridge->ExportCallbackProbes(CallbackTraceCSV);
			const bool CallbackTraceSaved=!CallbackTraceRequested||(
				CallbackTraceComplete&&FFileHelper::SaveStringToFile(CallbackTraceCSV,*FPaths::Combine(Directory,TEXT("callback-trace.csv"))));
			const bool CallbackCorrelationSaved=!CallbackTraceRequested
				||W3ExportCallbackCorrelation(Bridge.Get(),Directory);
			const bool MetaSoundCorrelationSaved=RequireNormalEvidence
				? DiagnosticMetaSoundCorrelationSaved
				: (!MetaSoundCorrelationRequested
					||(MetaContext.IsValid()?W3ExportMetaSoundCorrelation(MetaContext,Directory):CallbackTraceRequested));
			const bool RareTailCorrelationSaved=W3ExportRareTailCorrelation(Bridge.Get(),Directory,RareTailRequested,RareTailSelfTest);
			const bool PressureJsonSaved=FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Directory,TEXT("pressure.json")));
			const bool PressureTimeseriesSaved=PressureJsonSaved
				&&FFileHelper::SaveStringToFile(CSV,*FPaths::Combine(Directory,TEXT("pressure-timeseries.csv")));
			const bool Saved=PressureTimeseriesSaved&&CallbackTraceSaved&&CallbackCorrelationSaved&&MetaSoundCorrelationSaved
				&&RareTailCorrelationSaved&&PressureWindowRawSaved;
			const bool WetOracle=DropWet?(!ContinuousReverb&&WetDropDetected&&WetRecoveryObserved):ContinuousReverb;
			const uint64 SourceTimingCount=Bridge->SourceTiming.Count.load();
			const uint64 SourceBlockTimingCount=Bridge->SourceBlockTiming.Count.load();
			const uint64 UnderrunCount=Underruns.Count.load();
			const bool RuntimeGateOk=!HeadlessDiagnostic||Duration<W3Duration;
			const bool SourceTimingGateOk=SourceTimingCount>=Expected*.9;
			const bool SourceBlockTimingGateOk=SourceBlockTimingCount>0;
			const bool CallbackBudgetGateOk=VoiceBudgetsRead&&BusExecuteBudgetComplete&&ConservativeGraphBoundUs<GraphDeadlineUs;
			const bool MixerCallbackBudgetGateOk=!RequireNormalEvidence||(MixerGeometryGateOk&&BusExecuteBudgetComplete&&ConservativeMixerBoundUs<MixerDeadlineUs);
			const bool DiagnosticTraceGateOk=!RequireNormalEvidence||(DiagnosticMetaContextAtMeasurementStart&&DiagnosticTrace.TraceComplete
				&&DiagnosticTrace.FormalWindowValid&&DiagnosticTrace.ExpectedSourcesPresent
				&&DiagnosticTrace.UniqueVoices==W3Voices&&DiagnosticTrace.UniqueAudioIds==W3Voices
				&&DiagnosticTrace.RejectedRows==0&&GraphSourceIdentityValid&&DiagnosticMetaSoundCorrelationSaved
				&&VoiceCountsGateOk&&VoiceIdentityGateOk&&PressureWindowRawSaved&&ExecuteMaximumObservationComplete);
			const bool DiagnosticWavGateOk=!RequireDiagnosticEvidence||(WavSummary.Complete&&WavSummary.DurationSeconds>0.0&&WavSummary.NonzeroSamples>0);
			const bool UnderrunGateOk=UnderrunCount==0;
			bool SourceSendContributorsOk=MetaContext.IsValid() && VoiceBindings.Num()==W3Voices;
			if (MetaContext)
				for (const FIMW3VoiceBinding& Binding : VoiceBindings)
					SourceSendContributorsOk &= MetaContext->SourceSendBlockCounts[Binding.Slot].load(std::memory_order_acquire)>SourceSendCountsStart[Binding.Slot];
			const bool GraphTransportGateOk=!RequireNormalEvidence || (MetaContext.IsValid()
				&& MetaContext->SourceSendTransportEnabled && !MetaContext->SourceSendTransportFailed.load(std::memory_order_acquire)
				&& MetaContext->BusUnderruns.load(std::memory_order_acquire)==GraphBusUnderrunsStart
				&& MetaContext->SourceSendPushFailures.load(std::memory_order_acquire)==SourceSendFailuresStart
				&& SourceSendContributorsOk);
			const bool RejectedGateOk=RejectedAfterDrain==RejectedStart;
			const bool ReverbRejectedGateOk=ReverbRejectedAfterDrain==ReverbRejectedStart;
			const bool EnvironmentContinuityGateOk=!RequireNormalEvidence||ReverbNotFreshAfterDrain==ReverbNotFreshStart;
			const bool DryDroppedGateOk=DryDroppedAfterDrain==DryDroppedStart;
			const bool DiagnosticGraphFramesGateOk=!RequireNormalEvidence
				||(GraphBlockFrames==FIMAcousticMetaSoundContext::Frames&&GraphSampleRate>0.0f);
			const bool Success=Saved&&RuntimeGateOk&&WetOracle&&SourceTimingGateOk&&SourceBlockTimingGateOk
				&&CallbackBudgetGateOk&&MixerCallbackBudgetGateOk&&DiagnosticTraceGateOk&&DiagnosticWavGateOk
				&&DiagnosticGraphFramesGateOk&&VoiceIdentityGateOk&&VoiceCountsGateOk
				&&UnderrunGateOk&&GraphTransportGateOk&&RejectedGateOk&&ReverbRejectedGateOk&&DryDroppedGateOk&&EnvironmentContinuityGateOk;
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3GraphTransport gate=%d all_contributors=%d bus_underrun_delta=%llu send_failure_delta=%llu failed=%d"),
				GraphTransportGateOk,SourceSendContributorsOk,
				MetaContext ? MetaContext->BusUnderruns.load(std::memory_order_acquire)-GraphBusUnderrunsStart : MAX_uint64,
				MetaContext ? MetaContext->SourceSendPushFailures.load(std::memory_order_acquire)-SourceSendFailuresStart : MAX_uint64,
				!MetaContext || MetaContext->SourceSendTransportFailed.load(std::memory_order_acquire));
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3MaximumAndContinuity maximum_complete=%d environment_continuity=%d reverb_not_fresh=%llu observation_error=%s"),
				ExecuteMaximumObservationComplete,EnvironmentContinuityGateOk,ReverbNotFreshAfterDrain-ReverbNotFreshStart,*ExecuteMaximumObservationError);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3Gate saved=%d pressure_json=%d timeseries=%d callback_trace=%d correlation=%d callback_correlation=%d metasound_correlation=%d raw_pressure=%d rare_tail=%d runtime=%d wet=%d source_timing=%d voice_counts=%d voice_identity=%d alias_count=%d graph_bound=%d mixer_bound=%d diagnostic_trace=%d diagnostic_wav=%d underrun=%d rejected=%d reverb_rejected=%d dry_dropped=%d observed=%llu expected_min=%.1f graph_bound_us=%.1f mixer_bound_us=%.1f graph_deadline_us=%.1f mixer_deadline_us=%.1f"),
				Saved,PressureJsonSaved,PressureTimeseriesSaved,CallbackTraceSaved,(CallbackCorrelationSaved&&MetaSoundCorrelationSaved),CallbackCorrelationSaved,MetaSoundCorrelationSaved,
				PressureWindowRawSaved,RareTailCorrelationSaved,RuntimeGateOk,WetOracle,SourceTimingGateOk,VoiceCountsGateOk,VoiceIdentityGateOk,
				SourceBlockTimingGateOk,CallbackBudgetGateOk,MixerCallbackBudgetGateOk,DiagnosticTraceGateOk,DiagnosticWavGateOk,UnderrunGateOk,
				RejectedGateOk,ReverbRejectedGateOk,DryDroppedGateOk,SourceTimingCount,Expected*.9,ConservativeGraphBoundUs,ConservativeMixerBoundUs,GraphDeadlineUs,MixerDeadlineUs);
			if(DropWet)
			{
				const FString Message=Success
					? TEXT("Wet-outage negative control detected silence and recovery.")
					: FString::Printf(TEXT("Wet-outage control gate failed: saved=%d runtime=%d wet=%d source_timing=%d source_block_timing=%d callback_budget=%d underrun=%d rejected=%d reverb_rejected=%d dry_dropped=%d."),
						Saved,RuntimeGateOk,WetOracle,SourceTimingGateOk,SourceBlockTimingGateOk,CallbackBudgetGateOk,
						UnderrunGateOk,RejectedGateOk,ReverbRejectedGateOk,DryDroppedGateOk);
				return Finish(Success,Message);
			}
			return Finish(Success,Success?FString::Printf(TEXT("16 voices / %.1f seconds actual audio path passed."),MeasuredSeconds):TEXT("Pressure gate failed; retain raw histogram/time series and fix before acceptance."));
		}
		return false;
	}
private:
	void AppendTimeseriesSample(double Elapsed)
	{
		const FIMW3MemorySample MemorySample=IMW3CaptureMemorySample();
		const uint64 Memory=MemorySample.UsedPhysical;PeakMemory=FMath::Max(PeakMemory,Memory);
		PeakVirtualMemory=FMath::Max(PeakVirtualMemory,MemorySample.UsedVirtual);
		MinimumAvailablePhysical=FMath::Min(MinimumAvailablePhysical,MemorySample.AvailablePhysical);
		MinimumAvailableVirtual=FMath::Min(MinimumAvailableVirtual,MemorySample.AvailableVirtual);
		PeakProcessPhysical=FMath::Max(PeakProcessPhysical,MemorySample.PeakUsedPhysical);
		PeakProcessVirtual=FMath::Max(PeakProcessVirtual,MemorySample.PeakUsedVirtual);
		const uint64 Calls=Bridge->ReverbProcessedBlocks.load(),Dry=Bridge->ReverbDryBlocks.load();
		const uint64 Raw=Bridge->ReverbRawNonzeroBlocks.load(),Audible=Bridge->ReverbNonzeroBlocks.load();
		if(DropWet&&Elapsed>=11&&Elapsed<13&&Audible==LastReverbAudible)WetDropDetected=true;
		if(DropWet&&Elapsed>=14&&Audible>LastReverbAudible)WetRecoveryObserved=true;
		// Continuous periodic source input must reach the environmental
		// output in every sampled second, not only during warmup.
		ContinuousReverb=ContinuousReverb&&Calls>LastReverbCalls&&Dry>LastReverbDry&&Raw>LastReverbRaw&&Audible>LastReverbAudible;
		LastReverbCalls=Calls;LastReverbDry=Dry;LastReverbRaw=Raw;LastReverbAudible=Audible;
		CSV+=FString::Printf(TEXT("%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n"),Elapsed,Bridge->RenderedBlocks.load()-RenderedStart,
			Bridge->RejectedBlocks.load()-RejectedStart,Bridge->ReverbRejectedBlocks.load()-ReverbRejectedStart,
			Bridge->DryDroppedBlocks.load()-DryDroppedStart,Memory,Calls,Dry,Raw,Audible,
			Bridge->MaxSnapshotGapUs.load(),Bridge->MaxWorkerGapUs.load(),MemorySample.UsedVirtual,
			MemorySample.AvailablePhysical,MemorySample.AvailableVirtual,MemorySample.PeakUsedPhysical,MemorySample.PeakUsedVirtual);
		LastTimeseriesSampleElapsed=Elapsed;
	}
	bool FailGate(const TCHAR* Gate,const FString& Reason)
	{
		const bool bReportSaved=IMW3WritePreflightFailureReport(Directory,Gate,Reason,Sources.Num(),ExpectedAudioComponentIds.Num(),
			GraphBlockFrames,GraphSampleRate,VoiceBindings);
		UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticW3PreflightGate gate=%s passed=0 report_saved=%d reason=%s evidence=%s"),
			Gate,bReportSaved,*Reason,*Directory);
		return Finish(false,FString::Printf(TEXT("W3 preflight gate %s failed: %s"),Gate,*Reason));
	}
	void Cleanup()
	{
		if(Recording)
		{
			if(UWorld* World=TestWorld.Get())
			{
				UAudioMixerBlueprintLibrary::StopRecordingOutput(World,EAudioRecordingExportType::WavFile,TEXT("IM_complex_16voices"),Directory,nullptr,nullptr);
				WavRecordingStopSeconds=FPlatformTime::Seconds();
			}
			Recording=false;
		}
		if(Bridge){Bridge->ClosePressureWindowProbes();Bridge->ProfilingEnabled.store(false);Bridge->CallbackDiagnosticsEnabled.store(false);Bridge->RareTailDiagnosticsEnabled.store(false,std::memory_order_release);}
		Underruns.Enabled.store(false);if(ObserverRegistered){GLog->RemoveOutputDevice(&Underruns);ObserverRegistered=false;}
		if(SettingsChanged){FApp::SetUnfocusedVolumeMultiplier(BackgroundVolume);GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio=bAllowBackgroundAudioOrig;GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground=PreviousThrottle;if(LogInterval)LogInterval->Set(PreviousLogInterval,ECVF_SetByCode);SettingsChanged=false;}
	}
	bool Finish(bool Success,const FString& Message)
	{
		Cleanup();if(!Success)Test->AddError(Message);
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticW3Pressure %s evidence=%s"),*Message,*Directory);
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticW3Pressure %s"),Success?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),Success?TEXT("PASS"):TEXT("FAIL"));GEditor->RequestEndPlayMap();return true;
	}
	FAutomationTestBase* Test;double Created,Duration;const bool DropWet,StaleProbe,RequireDiagnosticEvidence,RequireNormalEvidence;
	FIMW3MemorySample PreCaptureMemory;
	FIMW3MemorySample MeasurementStartMemorySample;
	double StageStarted=0,MeasurementStarted=0,MeasurementEnded=0,NextSample=0,NextMotionUpdate=0,MeasuredSeconds=0,LastTimeseriesSampleElapsed=-1;int32 Stage=0;
	double GraphSetupBindingWaitSeconds=0;
	uint32 GraphDeviceId=0;
	uint64 GraphWorldGeneration=0;
	bool ContinuousReverb=true,CallbackTraceRequested=false,MetaSoundCorrelationRequested=false,RareTailRequested=false,RareTailSelfTest=false;
	bool PressureWindowCaptureRequested=false,VoiceIdentityStableAtWindowEnd=false,FreshTimingPreflightPassed=false;
	bool WetDropDetected=false,WetRecoveryObserved=false,StaleInjected=false;
	uint64 StaleN0=0,StaleD0=0,StaleR0=0;
	uint64 LastReverbCalls=0,LastReverbDry=0,LastReverbRaw=0,LastReverbAudible=0;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;TWeakObjectPtr<APlayerController> Listener;TWeakObjectPtr<UWorld> TestWorld;
	TArray<TWeakObjectPtr<UAudioComponent>> Sources;TArray<TWeakObjectPtr<USoundBase>> Sounds;TArray<int16> PCM;
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Bridge;FIMAcousticMetaSoundContextPtr MetaContext;FIMAcousticUnderrunObserver Underruns;
	TWeakObjectPtr<USoundBase> GraphSourceAsset;TSet<uint64> ExpectedAudioComponentIds;TArray<uint64> ExpectedAudioComponentIdOrder;
	TArray<FIMW3VoiceBinding> VoiceBindings;TArray<FIMW3VoiceBudget> VoiceBudgets;FString VoiceIdentityFailure;
	FIMW3PressureWindowSummary PressureWindowSummary;bool PressureWindowRawSaved=false;
	FString Directory,CSV,WavPath;FIMW3WavSummary WavSummary;bool WavExportResolved=false,GraphSourceIdentityValid=false,DiagnosticMetaContextAtMeasurementStart=false;
	bool MixerDeadlineAvailable=false,MixerGeometryGateOk=false,ObserverRegistered=false,SettingsChanged=false,Recording=false,RecordingPaused=false,PreviousThrottle=false,bAllowBackgroundAudioOrig=false;
	float BackgroundVolume=1,PreviousLogInterval=0,MixerSampleRate=0,GraphSampleRate=0;
	uint32 MixerBlockFrames=0,GraphBlockFrames=0,GraphBlocksPerMixer=0;
	IConsoleVariable* LogInterval=nullptr;
	uint64 RejectedStart=0,ReverbRejectedStart=0,ReverbNotFreshStart=0,DryDroppedStart=0,RenderedStart=0,StartMemory=0,PeakMemory=0;
	uint64 GraphBusUnderrunsStart=0,SourceSendFailuresStart=0;
	std::array<uint64,FIMAcousticMetaSoundContext::MaxVoices> SourceSendCountsStart{};
	uint64 StartVirtualMemory=0,StartAvailablePhysical=0,StartAvailableVirtual=0,PeakVirtualMemory=0;
	uint64 MinimumAvailablePhysical=0,MinimumAvailableVirtual=0,PeakProcessPhysical=0,PeakProcessVirtual=0;
	double WavRecordingStartSeconds=0,WavRecordingStopSeconds=0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3Pressure,"IceMoon.AcousticField.W3.Pressure16x600",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3Pressure::RunTest(const FString&)
{
	const IMAcousticW3TestPrivate::FIMW3MemorySample MemoryBeforeTraceCapture=IMAcousticW3TestPrivate::IMW3CaptureMemorySample();
	// S5 needs full-window identity traces; its WAVE recording is handled by the mixer.
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(
		true,
		FIMAcousticMetaSoundContext::CapturedSourceBlockTraceMaxCapacity,
		FIMAcousticMetaSoundContext::CapturedEnvironmentBlockTraceMaxCapacity,
		false);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this,IMAcousticW3TestPrivate::W3Duration,false,false,false,MemoryBeforeTraceCapture));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3Diagnostic,"IceMoon.AcousticField.W3.Diagnostic16x30",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3Diagnostic::RunTest(const FString&)
{
	const IMAcousticW3TestPrivate::FIMW3MemorySample MemoryBeforeTraceCapture=IMAcousticW3TestPrivate::IMW3CaptureMemorySample();
	// Identity rows plus the master WAV are the Diagnostic evidence; avoid also
	// allocating the unrelated 200-second PCM capture arrays.
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true,0,0,false);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this,30,false,false,true,MemoryBeforeTraceCapture));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3WetDrop,"IceMoon.AcousticField.W3.WetDropCounterexample",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3WetDrop::RunTest(const FString&)
{
	const IMAcousticW3TestPrivate::FIMW3MemorySample MemoryBeforeTraceCapture=IMAcousticW3TestPrivate::IMW3CaptureMemorySample();
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this,30,true,false,false,MemoryBeforeTraceCapture));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticW3StaleDrain,"IceMoon.AcousticField.W3.StaleDrainProbe",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIMAcousticW3StaleDrain::RunTest(const FString&)
{
	const IMAcousticW3TestPrivate::FIMW3MemorySample MemoryBeforeTraceCapture=IMAcousticW3TestPrivate::IMW3CaptureMemorySample();
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/L_IceMoonAcousticField"),false,&Error);
	if(!Error.IsEmpty()){IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticW3TestPrivate::FIMAcousticW3PressureCommand(this,45,false,true,false,MemoryBeforeTraceCapture));return true;
}
#endif
