#include "Tests/IMAcousticOrdinaryRecorderBoundaryTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticFootstepCharacter.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioThread.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "ISubmixBufferListener.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectIterator.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace IMAcousticOrdinaryRecorderBoundaryTestPrivate
{
constexpr int32 IMBoundaryCapacitySeconds = 32;
constexpr int32 IMBoundaryCapacityBlocks = 2048;
constexpr int32 IMBoundaryCapacityEvents = 256;
constexpr int32 IMBoundaryCapacityGeneratorRows = 32;
constexpr double IMBoundaryClockToleranceSeconds = 1.e-6;

struct FIMOrdinaryMasterBlock
{
	int32 FrameBegin = 0, Frames = 0, Channels = 0, Rate = 0;
	uint32 ThreadId = 0;
	uint64 CyclesBeforeCopy = 0, CyclesAfterCopy = 0;
	double AudioClock = 0., SecondsBeforeCopy = 0., SecondsAfterCopy = 0.;
};

struct FIMOrdinaryGeneratorRow
{
	uint64 ComponentID = 0, InstanceID = 0, Cycles = 0;
	const void* Pointer = nullptr;
	double Seconds = 0.;
	uint32 ThreadID = 0;
	bool Created = false;
};

class FIMOrdinaryGeneratorObserver
{
public:
	explicit FIMOrdinaryGeneratorObserver(uint64 InComponentID) : ComponentID(InComponentID)
	{
		Rows.SetNum(IMBoundaryCapacityGeneratorRows);
	}

	void Observe(const Metasound::FGeneratorInstanceInfo& Info, bool Created)
	{
		if (Info.AudioComponentID != ComponentID) return;
		if (InFlight.fetch_add(1, std::memory_order_acquire) != 0)
		{
			Failure.store(1, std::memory_order_release);
			InFlight.fetch_sub(1, std::memory_order_release);
			return;
		}
		const int32 ActiveAfter = Active.fetch_add(Created ? 1 : -1, std::memory_order_acq_rel) + (Created ? 1 : -1);
		const int32 Index = Published.load(std::memory_order_relaxed);
		if (Index >= Rows.Num() || ActiveAfter < 0)
		{
			Failure.store(2, std::memory_order_release);
		}
		else
		{
			const auto Generator = Info.Generator.Pin();
			FIMOrdinaryGeneratorRow& Row = Rows[Index];
			Row.ComponentID = Info.AudioComponentID;
			Row.InstanceID = Info.InstanceID;
			Row.Pointer = Generator.Get();
			Row.Cycles = FPlatformTime::Cycles64();
			Row.Seconds = FPlatformTime::Seconds();
			Row.ThreadID = FPlatformTLS::GetCurrentThreadId();
			Row.Created = Created;
			Published.store(Index + 1, std::memory_order_release);
		}
		InFlight.fetch_sub(1, std::memory_order_release);
	}

	const uint64 ComponentID;
	TArray<FIMOrdinaryGeneratorRow> Rows;
	std::atomic<int32> Published{0}, Active{0}, InFlight{0}, Failure{0};
};

class FIMOrdinaryMasterRecorder final : public ISubmixBufferListener
{
public:
	FIMOrdinaryMasterRecorder(const USoundSubmix* InMaster, int32 InRate, int32 InFrames)
		: Master(InMaster), Rate(InRate), FramesPerBlock(InFrames)
	{
		Blocks.SetNum(IMBoundaryCapacityBlocks);
		PCM.SetNumUninitialized(IMBoundaryCapacitySeconds * Rate * 2);
	}

	const FString& GetListenerName() const override { return Name; }
	bool IsRenderingAudio() const override { return false; }
	void Fail(int32 Code)
	{
		int32 Expected = 0;
		Failure.compare_exchange_strong(Expected, Code, std::memory_order_release);
	}
	int32 GetCount() const { return Published.load(std::memory_order_acquire); }
	int32 GetFrames(int32 Count) const
	{
		return Count ? Blocks[Count - 1].FrameBegin + Blocks[Count - 1].Frames : 0;
	}

	void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData,
		int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override
	{
		if (InFlight.fetch_add(1, std::memory_order_acquire) != 0)
		{
			Fail(1);
			InFlight.fetch_sub(1, std::memory_order_release);
			return;
		}
		const int32 Index = Published.load(std::memory_order_relaxed);
		int32 Reject = 0;
		if (OwningSubmix != Master) Reject = 2;
		else if (!AudioData || NumChannels != 2 || SampleRate != Rate || NumSamples != FramesPerBlock * 2) Reject = 3;
		else if (Index >= Blocks.Num() || RenderFrames * 2 + NumSamples > PCM.Num()) Reject = 4;
		else if (!FMath::IsFinite(AudioClock)) Reject = 5;
		else if (Index && FMath::Abs(AudioClock - Blocks[Index - 1].AudioClock
			- double(Blocks[Index - 1].Frames) / Rate) > IMBoundaryClockToleranceSeconds) Reject = 6;
		if (!Reject)
		{
			for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
			{
				if (!FMath::IsFinite(AudioData[SampleIndex])) { Reject = 5; break; }
			}
		}
		if (Reject || Failure.load(std::memory_order_acquire))
		{
			if (Reject) Fail(Reject);
			InFlight.fetch_sub(1, std::memory_order_release);
			return;
		}
		FIMOrdinaryMasterBlock& Block = Blocks[Index];
		Block.FrameBegin = RenderFrames;
		Block.Frames = NumSamples / NumChannels;
		Block.Channels = NumChannels;
		Block.Rate = SampleRate;
		Block.ThreadId = FPlatformTLS::GetCurrentThreadId();
		Block.AudioClock = AudioClock;
		Block.SecondsBeforeCopy = FPlatformTime::Seconds();
		Block.CyclesBeforeCopy = FPlatformTime::Cycles64();
		FMemory::Memcpy(PCM.GetData() + RenderFrames * 2, AudioData, NumSamples * sizeof(float));
		Block.CyclesAfterCopy = FPlatformTime::Cycles64();
		Block.SecondsAfterCopy = FPlatformTime::Seconds();
		RenderFrames += Block.Frames;
		Published.store(Index + 1, std::memory_order_release);
		InFlight.fetch_sub(1, std::memory_order_release);
	}

	TArray<FIMOrdinaryMasterBlock> Blocks;
	TArray<float> PCM;
	std::atomic<int32> Failure{0}, InFlight{0}, Published{0};
	std::atomic<bool> RegisteredRenderAck{false}, UnregisteredRenderAck{false};
	double DrainAudioClock = 0., DrainPlatformSeconds = 0.;
	uint64 DrainCycles = 0;
private:
	const USoundSubmix* Master;
	const int32 Rate, FramesPerBlock;
	const FString Name = TEXT("IMOrdinaryNativeMasterBoundaryR105");
	int32 RenderFrames = 0;
};

struct FIMOrdinaryRecorderOwner
{
	FAudioDeviceHandle Device;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<USoundSubmix> Master;
	TSharedPtr<FIMOrdinaryMasterRecorder, ESPMode::ThreadSafe> Listener;
	TStrongObjectPtr<UMetaSoundSource> Source;
	TSharedPtr<FIMOrdinaryGeneratorObserver, ESPMode::ThreadSafe> GeneratorObserver;
	FDelegateHandle CreatedHandle, DestroyedHandle;
	TArray<TSharedPtr<FJsonValue>> Events, SourceWaves;
	FString Output, WorldPath, MasterPath, MasterPointer;
	FString ActorPath, ComponentPath, SourcePath;
	uint64 Epoch = 0;
	int32 Rate = 0, MixerFrames = 0;
	int32 LastEventPublishedFrames = 0;
	double StartSeconds = 0., StopSeconds = 0., LongestWaveSeconds = 0., LastEventSeconds = 0.;
	bool bStopping = false, bUnregisterSubmitted = false, bWorldCleanupRequestedStop = false;
	bool bGeneratorDelegatesRemoved = false;
};

TUniquePtr<FIMOrdinaryRecorderOwner> IMBoundaryOwner;
FDelegateHandle IMBoundaryWorldCleanupHandle;
FDelegateHandle IMBoundaryPreBeginHandle;
uint64 IMBoundaryNextEpoch = 0;

void IMBoundarySaveJson(const FString& File, const TSharedRef<FJsonObject>& Report)
{
	FString Serialized;
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Serialized));
	if (!FFileHelper::SaveStringToFile(Serialized, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		if (IMBoundaryOwner) IMBoundaryOwner->Listener->Fail(9);
		UE_LOG(LogTemp, Error, TEXT("IMLogs R105_NATIVE json_write_failed path=%s"), *File);
	}
}

TSharedRef<FJsonObject> IMBoundaryBlockJson(const FIMOrdinaryMasterBlock& Block)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("frame_begin"), Block.FrameBegin);
	Row->SetNumberField(TEXT("frames"), Block.Frames);
	Row->SetNumberField(TEXT("channels"), Block.Channels);
	Row->SetNumberField(TEXT("rate"), Block.Rate);
	Row->SetNumberField(TEXT("audio_clock"), Block.AudioClock);
	Row->SetNumberField(TEXT("platform_seconds_before_copy"), Block.SecondsBeforeCopy);
	Row->SetNumberField(TEXT("platform_seconds_after_copy"), Block.SecondsAfterCopy);
	Row->SetStringField(TEXT("cycles64_before_copy"), LexToString(Block.CyclesBeforeCopy));
	Row->SetStringField(TEXT("cycles64_after_copy"), LexToString(Block.CyclesAfterCopy));
	Row->SetNumberField(TEXT("thread_id"), Block.ThreadId);
	return Row;
}

TSharedRef<FJsonObject> IMBoundaryStatus()
{
	check(IsInGameThread());
	FIMOrdinaryRecorderOwner& State = *IMBoundaryOwner;
	FIMOrdinaryMasterRecorder& Listener = *State.Listener;
	const int32 Count = Listener.GetCount();
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("fixture"), TEXT("private native ordinary master recorder R105"));
	Report->SetNumberField(TEXT("device_id"), State.Device.GetDeviceID());
	Report->SetStringField(TEXT("record_epoch"), LexToString(State.Epoch));
	Report->SetStringField(TEXT("record_epoch_semantics"), TEXT("private listener start ordinal; not a hardware or World generation"));
	Report->SetStringField(TEXT("world"), State.WorldPath);
	Report->SetStringField(TEXT("master"), State.MasterPath);
	Report->SetStringField(TEXT("master_pointer"), State.MasterPointer);
	Report->SetStringField(TEXT("pcm_file"), FPaths::Combine(State.Output, TEXT("native_master.f32")));
	Report->SetStringField(TEXT("pcm_codec"), TEXT("float32le interleaved stereo, actual copied listener buffers"));
	Report->SetNumberField(TEXT("sample_rate"), State.Rate);
	Report->SetNumberField(TEXT("mixer_frames"), State.MixerFrames);
	Report->SetNumberField(TEXT("block_count"), Count);
	Report->SetNumberField(TEXT("frames"), Listener.GetFrames(Count));
	Report->SetNumberField(TEXT("source_event_count"), State.Events.Num());
	Report->SetNumberField(TEXT("failure_code"), Listener.Failure.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("active_callbacks"), Listener.InFlight.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("gt_platform_seconds"), FPlatformTime::Seconds());
	Report->SetNumberField(TEXT("gt_start_platform_seconds"), State.StartSeconds);
	Report->SetNumberField(TEXT("gt_stop_request_platform_seconds"), State.StopSeconds);
	Report->SetNumberField(TEXT("last_source_event_platform_seconds"), State.LastEventSeconds);
	Report->SetNumberField(TEXT("longest_source_wave_seconds"), State.LongestWaveSeconds);
	Report->SetNumberField(TEXT("minimum_source_pitch_semitones"), -2.);
	Report->SetNumberField(TEXT("saved_ir_seconds"), IMAcousticRecipe::ReverbSavedDurationS);
	Report->SetNumberField(TEXT("tail_observation_margin_seconds"), .5);
	Report->SetNumberField(TEXT("tail_window_seconds"), State.LongestWaveSeconds * FMath::Pow(2., 2. / 12.)
		+ IMAcousticRecipe::ReverbSavedDurationS + .5);
	Report->SetNumberField(TEXT("seconds_per_cycle64"), FPlatformTime::GetSecondsPerCycle64());
	Report->SetBoolField(TEXT("registration_render_ack"), Listener.RegisteredRenderAck.load(std::memory_order_acquire));
	Report->SetBoolField(TEXT("stop_requested"), State.bStopping);
	Report->SetBoolField(TEXT("unregister_submitted"), State.bUnregisterSubmitted);
	Report->SetBoolField(TEXT("world_cleanup_requested_stop"), State.bWorldCleanupRequestedStop);
	Report->SetBoolField(TEXT("unregistration_render_ack"), Listener.UnregisteredRenderAck.load(std::memory_order_acquire));
	Report->SetBoolField(TEXT("complete"), false);
	Report->SetBoolField(TEXT("forces_audio_rendering"), false);
	Report->SetBoolField(TEXT("public_recorder_used"), false);
	Report->SetField(TEXT("public_pcm_origin"), MakeShared<FJsonValueNull>());
	Report->SetField(TEXT("native_trigger_consumption"), MakeShared<FJsonValueNull>());
	Report->SetField(TEXT("sound_identity"), MakeShared<FJsonValueNull>());
	Report->SetStringField(TEXT("master_sampling_stage"), TEXT("actual main submix after its effects/output volume; before device post-submix mute, endpoint additions and platform processing"));
	Report->SetStringField(TEXT("pcm_frame_origin"), TEXT("frame zero is the first received native main-submix callback; not a trigger, graph, hardware or speaker frame"));
	Report->SetStringField(TEXT("audio_clock_semantics"), TEXT("FMixerDevice::GetAudioTime at submix callback; device render-block clock, not World or platform time"));
	Report->SetField(TEXT("hardware_final_output"), MakeShared<FJsonValueNull>());
	if (State.GeneratorObserver)
	{
		Report->SetStringField(TEXT("work_id"), TEXT("MON-AC-NORMAL-EVENT-NATIVE-VOICE-MASTER-MAPPING-AND-PLAY-20261008-0543"));
		Report->SetStringField(TEXT("actor"), State.ActorPath);
		Report->SetStringField(TEXT("audio_component"), State.ComponentPath);
		Report->SetStringField(TEXT("audio_component_id"), LexToString(State.GeneratorObserver->ComponentID));
		Report->SetStringField(TEXT("saved_metasound_source"), State.SourcePath);
		Report->SetBoolField(TEXT("observer_bound_before_beginplay"), true);
		Report->SetNumberField(TEXT("generator_callback_rows"), State.GeneratorObserver->Published.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("active_generators"), State.GeneratorObserver->Active.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("generator_callbacks_in_flight"), State.GeneratorObserver->InFlight.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("generator_observer_failure"), State.GeneratorObserver->Failure.load(std::memory_order_acquire));
		Report->SetBoolField(TEXT("own_generator_delegates_removed"), State.bGeneratorDelegatesRemoved);
		Report->SetNumberField(TEXT("published_frames_at_last_gt_post_submit_observation"), State.LastEventPublishedFrames);
		Report->SetNumberField(TEXT("frames_since_last_gt_post_submit_observation"), Listener.GetFrames(Count) - State.LastEventPublishedFrames);
		Report->SetStringField(TEXT("event_to_master_clock_boundary"), TEXT("GT post-submit observes an acquired published PCM prefix; neither parameter-consumption frame nor isolated source contribution"));
		Report->SetField(TEXT("mixer_source_id_from_generator"), MakeShared<FJsonValueNull>());
	}
	if (Count)
	{
		Report->SetObjectField(TEXT("first_block"), IMBoundaryBlockJson(Listener.Blocks[0]));
		Report->SetObjectField(TEXT("last_block"), IMBoundaryBlockJson(Listener.Blocks[Count - 1]));
	}
	return Report;
}

void IMBoundaryStop()
{
	check(IsInGameThread());
	if (!IMBoundaryOwner || IMBoundaryOwner->bStopping) return;
	FIMOrdinaryRecorderOwner& State = *IMBoundaryOwner;
	State.bStopping = true;
	State.StopSeconds = FPlatformTime::Seconds();
	Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(State.Device.GetAudioDevice());
	if (USoundSubmix* Master = State.Master.Get())
	{
		Mixer->UnregisterSubmixBufferListener(State.Listener.ToSharedRef(), *Master);
		State.bUnregisterSubmitted = true;
	}
	else State.Listener->Fail(7);
	FAudioThread::RunCommandOnAudioThread([Handle = State.Device, Recorder = State.Listener, Mixer]()
	{
		Mixer->AudioRenderThreadCommand([Handle, Recorder, Mixer]()
		{
			Recorder->DrainAudioClock = Mixer->GetAudioTime();
			Recorder->DrainPlatformSeconds = FPlatformTime::Seconds();
			Recorder->DrainCycles = FPlatformTime::Cycles64();
			Recorder->UnregisteredRenderAck.store(true, std::memory_order_release);
		});
	});
	UE_LOG(LogTemp, Display, TEXT("IMLogs R105_NATIVE stop_requested device=%u epoch=%llu"), State.Device.GetDeviceID(), State.Epoch);
}

void IMBoundaryWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	check(IsInGameThread());
	if (IMBoundaryOwner && IMBoundaryOwner->World.Get() == World)
	{
		IMBoundaryOwner->bWorldCleanupRequestedStop = true;
		IMBoundaryStop();
	}
}

bool IMBoundaryTick(float DeltaSeconds)
{
	check(IsInGameThread());
	if (!IMBoundaryOwner) return false;
	FIMOrdinaryRecorderOwner& State = *IMBoundaryOwner;
	FIMOrdinaryMasterRecorder& Listener = *State.Listener;
	if (State.GeneratorObserver && State.GeneratorObserver->Failure.load(std::memory_order_acquire)) Listener.Fail(11);
	if (!State.bStopping)
	{
		UWorld* World = State.World.Get();
		if (!World || World->GetAudioDevice().GetAudioDevice() != State.Device.GetAudioDevice()
			|| World->GetAudioDevice().GetDeviceID() != State.Device.GetDeviceID()
			|| &static_cast<Audio::FMixerDevice*>(State.Device.GetAudioDevice())->GetMainSubmixObject() != State.Master.Get())
		{
			Listener.Fail(7);
			IMBoundaryStop();
		}
		else if (FPlatformTime::Seconds() - State.StartSeconds > 28.)
		{
			Listener.Fail(8);
			IMBoundaryStop();
		}
	}
	TSharedRef<FJsonObject> Report = IMBoundaryStatus();
	if (!State.bStopping || !Listener.UnregisteredRenderAck.load(std::memory_order_acquire)
		|| Listener.InFlight.load(std::memory_order_acquire) || !State.Listener.IsUnique())
	{
		IMBoundarySaveJson(FPaths::Combine(State.Output, TEXT("native_status.json")), Report);
		return true;
	}
	if (State.GeneratorObserver)
	{
		if (State.GeneratorObserver->Active.load(std::memory_order_acquire)
			|| State.GeneratorObserver->InFlight.load(std::memory_order_acquire))
		{
			IMBoundarySaveJson(FPaths::Combine(State.Output, TEXT("native_status.json")), Report);
			return true;
		}
		if (!State.bGeneratorDelegatesRemoved)
		{
			State.Source->OnGeneratorInstanceInfoCreated.Remove(State.CreatedHandle);
			State.Source->OnGeneratorInstanceInfoDestroyed.Remove(State.DestroyedHandle);
			State.CreatedHandle.Reset();
			State.DestroyedHandle.Reset();
			State.bGeneratorDelegatesRemoved = true;
		}
		if (!State.GeneratorObserver.IsUnique()) return true;
		TArray<TSharedPtr<FJsonValue>> Generators;
		const int32 GeneratorRows = State.GeneratorObserver->Published.load(std::memory_order_acquire);
		if (!GeneratorRows) Listener.Fail(11);
		for (int32 Index = 0; Index < GeneratorRows; ++Index)
		{
			const FIMOrdinaryGeneratorRow& Row = State.GeneratorObserver->Rows[Index];
			auto Generator = MakeShared<FJsonObject>();
			Generator->SetStringField(TEXT("audio_component_id"), LexToString(Row.ComponentID));
			Generator->SetStringField(TEXT("metasound_instance_id"), LexToString(Row.InstanceID));
			Generator->SetStringField(TEXT("generator_pointer_identity_only"), FString::Printf(TEXT("%p"), Row.Pointer));
			Generator->SetStringField(TEXT("cycles64"), LexToString(Row.Cycles));
			Generator->SetNumberField(TEXT("platform_seconds"), Row.Seconds);
			Generator->SetNumberField(TEXT("thread_id"), Row.ThreadID);
			Generator->SetBoolField(TEXT("created"), Row.Created);
			Generators.Add(MakeShared<FJsonValueObject>(Generator));
		}
		Report->SetArrayField(TEXT("generator_lifecycle"), Generators);
		Report->SetBoolField(TEXT("own_generator_delegates_removed"), true);
		Report->SetBoolField(TEXT("generator_observer_unique_before_release"), true);
	}
	const int32 Count = Listener.GetCount();
	const int32 Frames = Listener.GetFrames(Count);
	if (!Count || !Listener.RegisteredRenderAck.load(std::memory_order_acquire) || !State.bUnregisterSubmitted) Listener.Fail(8);
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Frames * 2 * sizeof(float));
	FMemory::Memcpy(Bytes.GetData(), Listener.PCM.GetData(), Bytes.Num());
	const bool bSaved = FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(State.Output, TEXT("native_master.f32")));
	if (!bSaved) Listener.Fail(9);
	TArray<TSharedPtr<FJsonValue>> Blocks;
	Blocks.Reserve(Count);
	for (int32 BlockIndex = 0; BlockIndex < Count; ++BlockIndex)
		Blocks.Add(MakeShared<FJsonValueObject>(IMBoundaryBlockJson(Listener.Blocks[BlockIndex])));
	Report->SetArrayField(TEXT("blocks"), Blocks);
	Report->SetArrayField(TEXT("source_events"), State.Events);
	Report->SetArrayField(TEXT("source_waves"), State.SourceWaves);
	Report->SetNumberField(TEXT("failure_code"), Listener.Failure.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("drain_audio_clock"), Listener.DrainAudioClock);
	Report->SetNumberField(TEXT("drain_platform_seconds"), Listener.DrainPlatformSeconds);
	Report->SetStringField(TEXT("drain_cycles64"), LexToString(Listener.DrainCycles));
	Report->SetBoolField(TEXT("listener_unique_before_release"), State.Listener.IsUnique());
	Report->SetBoolField(TEXT("native_drain_complete"), true);
	Report->SetBoolField(TEXT("pcm_exported"), bSaved);
	const FString Output = State.Output;
	const uint64 Epoch = State.Epoch;
	FWorldDelegates::OnWorldCleanup.Remove(IMBoundaryWorldCleanupHandle);
	IMBoundaryWorldCleanupHandle.Reset();
	IMBoundaryOwner.Reset();
	Report->SetBoolField(TEXT("owner_released_after_native_drain"), true);
	Report->SetBoolField(TEXT("complete"), bSaved && Report->GetNumberField(TEXT("failure_code")) == 0.);
	IMBoundarySaveJson(FPaths::Combine(Output, TEXT("native_report.json")), Report);
	IMBoundarySaveJson(FPaths::Combine(Output, TEXT("native_status.json")), Report);
	UE_LOG(LogTemp, Display, TEXT("IMLogs R105_NATIVE drained_and_released epoch=%llu blocks=%d frames=%d failure=%d"),
		Epoch, Count, Frames, int32(Report->GetNumberField(TEXT("failure_code"))));
	return false;
}

void IMBoundaryStart(const TArray<FString>& Args, UWorld* World)
{
	check(IsInGameThread());
	if (IMBoundaryOwner || !World || World->WorldType != EWorldType::PIE || Args.Num() > 1)
	{
		UE_LOG(LogTemp, Error, TEXT("IMLogs R105_NATIVE start_rejected reason=not_idle_pie_or_unexpected_args"));
		return;
	}
	FAudioDeviceHandle Device = World->GetAudioDevice();
	Audio::FMixerDevice* Mixer = FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(World);
	if (!Device.IsValid() || !Mixer || Mixer != Device.GetAudioDevice() || Mixer->GetNumDeviceChannels() != 2)
	{
		UE_LOG(LogTemp, Error, TEXT("IMLogs R105_NATIVE start_rejected reason=invalid_actual_pie_mixer"));
		return;
	}
	FString Output = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/OrdinaryPlayR105_20261008"));
	bool bVoiceMaster = false;
	if (!Args.IsEmpty())
	{
		const bool bPrimary = Args[0] == TEXT("OutputDir=OrdinaryPlayR108_20261008");
		const bool bRetry = Args[0] == TEXT("OutputDir=OrdinaryPlayR108_20261008_retry1");
		bVoiceMaster = Args[0] == TEXT("OutputDir=OrdinaryPlayR113Master_20261008");
		if (!bPrimary && !bRetry && !bVoiceMaster)
		{
			UE_LOG(LogTemp, Error, TEXT("IMLogs R105_NATIVE start_rejected reason=private_output_argument_not_allowlisted"));
			return;
		}
		Output = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2"),
			bVoiceMaster ? TEXT("OrdinaryPlayR113Master_20261008") : bPrimary ? TEXT("OrdinaryPlayR108_20261008") : TEXT("OrdinaryPlayR108_20261008_retry1"));
		if (IFileManager::Get().DirectoryExists(*Output) || IFileManager::Get().FileExists(*Output))
		{
			UE_LOG(LogTemp, Error, TEXT("IMLogs R105_NATIVE start_rejected reason=private_output_not_fresh"));
			return;
		}
	}
	if (IFileManager::Get().FileExists(*FPaths::Combine(Output, TEXT("native_report.json"))))
	{
		UE_LOG(LogTemp, Error, TEXT("IMLogs R105_NATIVE start_rejected reason=immutable_completed_output_exists"));
		return;
	}
	AIMAcousticFootstepCharacter* ObservedCharacter = nullptr;
	if (bVoiceMaster)
	{
		int32 Characters = 0;
		for (TObjectIterator<AIMAcousticFootstepCharacter> Character; Character; ++Character)
		{
			if (Character->GetWorld() != World) continue;
			ObservedCharacter = *Character;
			++Characters;
		}
		if (Characters != 1 || ObservedCharacter->HasActorBegunPlay() || !ObservedCharacter->GetFootstepAudio()
			|| !Cast<UMetaSoundSource>(ObservedCharacter->FootstepSound)
			|| !IFileManager::Get().MakeDirectory(*Output, true))
		{
			UE_LOG(LogTemp, Error, TEXT("IMLogs R113_NATIVE start_rejected reason=actual_pre_beginplay_source_component_boundary"));
			return;
		}
	}
	IMBoundaryOwner = MakeUnique<FIMOrdinaryRecorderOwner>();
	FIMOrdinaryRecorderOwner& State = *IMBoundaryOwner;
	State.Device = MoveTemp(Device);
	State.World = World;
	State.Master = &Mixer->GetMainSubmixObject();
	State.WorldPath = World->GetPathName();
	State.MasterPath = State.Master->GetPathName();
	State.MasterPointer = FString::Printf(TEXT("%p"), State.Master.Get());
	State.Output = Output;
	State.Epoch = ++IMBoundaryNextEpoch;
	State.Rate = Mixer->GetSampleRate();
	State.MixerFrames = Mixer->GetNumOutputFrames();
	State.StartSeconds = FPlatformTime::Seconds();
	State.Events.Reserve(IMBoundaryCapacityEvents);
	if (bVoiceMaster)
	{
		State.Source = TStrongObjectPtr<UMetaSoundSource>(Cast<UMetaSoundSource>(ObservedCharacter->FootstepSound));
		State.ActorPath = ObservedCharacter->GetPathName();
		State.ComponentPath = ObservedCharacter->GetFootstepAudio()->GetPathName();
		State.SourcePath = State.Source->GetPathName();
		State.GeneratorObserver = MakeShared<FIMOrdinaryGeneratorObserver, ESPMode::ThreadSafe>(ObservedCharacter->GetFootstepAudio()->GetAudioComponentID());
		auto Observer = State.GeneratorObserver.ToSharedRef();
		State.CreatedHandle = State.Source->OnGeneratorInstanceInfoCreated.AddLambda([Observer](const Metasound::FGeneratorInstanceInfo& Info)
		{
			Observer->Observe(Info, true);
		});
		State.DestroyedHandle = State.Source->OnGeneratorInstanceInfoDestroyed.AddLambda([Observer](const Metasound::FGeneratorInstanceInfo& Info)
		{
			Observer->Observe(Info, false);
		});
	}
	for (TObjectIterator<AIMAcousticFootstepCharacter> Character; Character; ++Character)
	{
		if (Character->GetWorld() != World) continue;
		for (const TArray<TObjectPtr<USoundWave>>* Waves : {&Character->StoneSteps, &Character->WoodSteps})
		{
			for (USoundWave* Wave : *Waves)
			{
				if (!Wave) continue;
				const double Duration = Wave->GetDuration();
				State.LongestWaveSeconds = FMath::Max(State.LongestWaveSeconds, Duration);
				TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
				Source->SetStringField(TEXT("wave"), Wave->GetPathName());
				Source->SetNumberField(TEXT("seconds"), Duration);
				State.SourceWaves.Add(MakeShared<FJsonValueObject>(Source));
			}
		}
	}
	State.Listener = MakeShared<FIMOrdinaryMasterRecorder, ESPMode::ThreadSafe>(State.Master.Get(), State.Rate, State.MixerFrames);
	IMBoundaryWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&IMBoundaryWorldCleanup);
	Mixer->RegisterSubmixBufferListener(State.Listener.ToSharedRef(), *State.Master.Get());
	FAudioThread::RunCommandOnAudioThread([Handle = State.Device, Recorder = State.Listener, Mixer]()
	{
		Mixer->AudioRenderThreadCommand([Handle, Recorder]()
		{
			Recorder->RegisteredRenderAck.store(true, std::memory_order_release);
		});
	});
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&IMBoundaryTick), .05f);
	IMBoundarySaveJson(FPaths::Combine(Output, TEXT("native_status.json")), IMBoundaryStatus());
	UE_LOG(LogTemp, Display, TEXT("IMLogs R105_NATIVE start device=%u epoch=%llu world=%s master=%s rate=%d mixer_frames=%d"),
		State.Device.GetDeviceID(), State.Epoch, *State.WorldPath, *State.MasterPath, State.Rate, State.MixerFrames);
}

void IMBoundaryStopCommand(const TArray<FString>& Args, UWorld* World)
{
	if (IMBoundaryOwner && IMBoundaryOwner->World.Get() == World) IMBoundaryStop();
}

void IMBoundaryArmVoiceMaster(const TArray<FString>& Args, UWorld* World)
{
	check(IsInGameThread());
	if (!World || World->WorldType != EWorldType::Editor || IMBoundaryOwner || IMBoundaryPreBeginHandle.IsValid()
		|| Args.Num() != 1 || Args[0] != TEXT("OutputDir=OrdinaryPlayR113Master_20261008")) return;
	IMBoundaryPreBeginHandle = FWorldDelegates::OnWorldInitializedActors.AddLambda([](const UWorld::FActorsInitializedParams& Params)
	{
		if (!Params.World || Params.World->WorldType != EWorldType::PIE) return;
		FWorldDelegates::OnWorldInitializedActors.Remove(IMBoundaryPreBeginHandle);
		IMBoundaryPreBeginHandle.Reset();
		IMBoundaryStart({TEXT("OutputDir=OrdinaryPlayR113Master_20261008")}, Params.World);
	});
	UE_LOG(LogTemp, Display, TEXT("IMLogs R113_NATIVE pre_beginplay_observer_armed default_off_saved_source_unchanged=1"));
}

void IMBoundaryFinishVoiceMaster()
{
	check(IsInGameThread());
	FWorldDelegates::OnWorldInitializedActors.Remove(IMBoundaryPreBeginHandle);
	IMBoundaryPreBeginHandle.Reset();
}

FAutoConsoleCommandWithWorldAndArgs IMBoundaryStartCommand(TEXT("IM.Acoustic.NativeBoundary.Start"),
	TEXT("Start the default-off private R105 ordinary PIE master boundary fixture."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&IMBoundaryStart));
FAutoConsoleCommandWithWorldAndArgs IMBoundaryStopConsole(TEXT("IM.Acoustic.NativeBoundary.Stop"),
	TEXT("Unregister the private R105 listener and export only after native drain."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&IMBoundaryStopCommand));
FAutoConsoleCommandWithWorldAndArgs IMBoundaryArmVoiceMasterConsole(TEXT("IM.Acoustic.NativeBoundary.ArmVoiceMaster"),
	TEXT("Default-OFF private saved-source generator lifetime and main-submix PCM observation before ordinary BeginPlay."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&IMBoundaryArmVoiceMaster));
FAutoConsoleCommand IMBoundaryFinishVoiceMasterConsole(TEXT("IM.Acoustic.NativeBoundary.FinishVoiceMaster"),
	TEXT("Remove only this private observer's pending pre-BeginPlay delegate."), FConsoleCommandDelegate::CreateStatic(&IMBoundaryFinishVoiceMaster));
}

void IM_RecordOrdinaryFootstepSourceEvent(AIMAcousticFootstepCharacter& Character,
	bool bLanding, int32 EventIndex, const UPhysicalMaterial* Material,
	uint64 AudioComponentId, float Gain, float Pitch)
{
	check(IsInGameThread());
	if (!IMAcousticOrdinaryRecorderBoundaryTestPrivate::IMBoundaryOwner || IMAcousticOrdinaryRecorderBoundaryTestPrivate::IMBoundaryOwner->bStopping || IMAcousticOrdinaryRecorderBoundaryTestPrivate::IMBoundaryOwner->World.Get() != Character.GetWorld()) return;
	const double Seconds = FPlatformTime::Seconds();
	const uint64 Cycles = FPlatformTime::Cycles64();
	IMAcousticOrdinaryRecorderBoundaryTestPrivate::FIMOrdinaryRecorderOwner& State = *IMAcousticOrdinaryRecorderBoundaryTestPrivate::IMBoundaryOwner;
	if (State.Events.Num() >= IMAcousticOrdinaryRecorderBoundaryTestPrivate::IMBoundaryCapacityEvents) { State.Listener->Fail(10); return; }
	const bool bWood = Material == Character.WoodPhysicalMaterial;
	const auto& Clips = bWood ? Character.WoodSteps : Character.StoneSteps;
	const USoundWave* Wave = Clips.IsEmpty() ? nullptr : Clips[bLanding ? 0 : (EventIndex - 1) % Clips.Num()].Get();
	TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
	Event->SetNumberField(TEXT("ordinal"), State.Events.Num() + 1);
	Event->SetStringField(TEXT("kind"), bLanding ? TEXT("landing") : TEXT("step"));
	Event->SetNumberField(TEXT("kind_index"), EventIndex);
	Event->SetNumberField(TEXT("gt_platform_seconds_post_trigger_submit"), Seconds);
	Event->SetStringField(TEXT("gt_cycles64_post_trigger_submit"), LexToString(Cycles));
	Event->SetNumberField(TEXT("world_seconds"), Character.GetWorld()->GetTimeSeconds());
	Event->SetStringField(TEXT("frame_counter"), LexToString(GFrameCounter));
	Event->SetStringField(TEXT("audio_component_id"), LexToString(AudioComponentId));
	Event->SetStringField(TEXT("material"), GetPathNameSafe(Material));
	Event->SetStringField(TEXT("surface"), bWood ? TEXT("wood") : TEXT("stone"));
	Event->SetStringField(TEXT("wave_gt_selected"), GetPathNameSafe(Wave));
	Event->SetNumberField(TEXT("gain"), Gain);
	Event->SetNumberField(TEXT("pitch_semitones"), Pitch);
	const int32 PublishedBlocks = State.Listener->GetCount();
	State.LastEventPublishedFrames = State.Listener->GetFrames(PublishedBlocks);
	Event->SetNumberField(TEXT("device_id"), State.Device.GetDeviceID());
	Event->SetStringField(TEXT("world"), State.WorldPath);
	Event->SetStringField(TEXT("record_epoch"), LexToString(State.Epoch));
	Event->SetNumberField(TEXT("published_master_blocks_at_gt_post_submit_observation"), PublishedBlocks);
	Event->SetNumberField(TEXT("published_master_frames_at_gt_post_submit_observation"), State.LastEventPublishedFrames);
	if (PublishedBlocks) Event->SetNumberField(TEXT("last_published_master_audio_clock_at_gt_observation"), State.Listener->Blocks[PublishedBlocks - 1].AudioClock);
	Event->SetField(TEXT("native_trigger_consumed_frame"), MakeShared<FJsonValueNull>());
	State.Events.Add(MakeShared<FJsonValueObject>(Event));
	State.LastEventSeconds = Seconds;
}
#endif
