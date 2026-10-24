#include "Tests/IMAcousticOrdinaryEnvironmentBoundaryTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "IMAcousticFootstepCharacter.h"
#include "IMAcousticMetaSound.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundBase.h"

namespace IMAcousticOrdinaryEnvironmentBoundaryTestPrivate
{
std::atomic<uint8> IMCoverageObservationState{0};
FIMAcousticOrdinaryCoverageFailure IMCoverageFailure;
}

bool IM_IsOrdinaryCoverageObservationArmedForTest()
{
	return IMAcousticOrdinaryEnvironmentBoundaryTestPrivate::IMCoverageObservationState.load(std::memory_order_acquire) == 1;
}

void IM_RecordOrdinaryCoverageFailureForTest(const FIMAcousticOrdinaryCoverageFailure& Probe)
{
	uint8 Expected = 1;
	if (!IMAcousticOrdinaryEnvironmentBoundaryTestPrivate::IMCoverageObservationState.compare_exchange_strong(Expected, 2, std::memory_order_acq_rel)) return;
	IMAcousticOrdinaryEnvironmentBoundaryTestPrivate::IMCoverageFailure = Probe;
	IMAcousticOrdinaryEnvironmentBoundaryTestPrivate::IMCoverageObservationState.store(3, std::memory_order_release);
}

bool FIMAcousticOrdinaryEnvironmentCaptureCounts::operator==(
	const FIMAcousticOrdinaryEnvironmentCaptureCounts& Other) const
{
	return SourceBlocks == Other.SourceBlocks && SourceFrames == Other.SourceFrames
		&& EnvironmentBlocks == Other.EnvironmentBlocks && EnvironmentFrames == Other.EnvironmentFrames
		&& NoIRBlocks == Other.NoIRBlocks && PushBlocks == Other.PushBlocks && PushFailures == Other.PushFailures
		&& SourceClaims == Other.SourceClaims && SourcePublished == Other.SourcePublished
		&& EnvironmentPublished == Other.EnvironmentPublished && BusPublished == Other.BusPublished
		&& SourcePCMFrames == Other.SourcePCMFrames && EnvironmentPCMFrames == Other.EnvironmentPCMFrames
		&& EnvironmentPCMBlocks == Other.EnvironmentPCMBlocks && SnapshotPushes == Other.SnapshotPushes
		&& SnapshotOverflows == Other.SnapshotOverflows && WorkerPushes == Other.WorkerPushes
		&& WorkerOverflows == Other.WorkerOverflows;
}

namespace IMAcousticOrdinaryEnvironmentBoundaryTestPrivate
{
constexpr uint32 IMTraceCapacity = FIMAcousticOrdinaryEnvironmentCaptureCounts::TraceCapacity;
const TCHAR* IMStockSource = TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/MS_IMFootstep.MS_IMFootstep");

struct FIMListenerInitializationProbe
{
	double Seconds = 0.0;
	uint64 Frame = 0;
	uint8 Phase = 0;
	bool ListenerValid = false;
	bool ListenerContainsNaN = false;
	bool CharacterBegunPlay = false;
	bool ControllerExists = false;
	bool ControllerBegunPlay = false;
	bool CameraBegunPlay = false;
	bool ViewTargetIsObserver = false;
	bool NativeTransformChanged = false;
	FVector DeviceListenerUE = FVector::ZeroVector;
	FVector ControllerListenerUE = FVector::ZeroVector;
	FVector ObserverUE = FVector::ZeroVector;
	FQuat DeviceListenerRotation = FQuat::Identity;
};

struct FIMEnvironmentBoundaryOwner
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AIMAcousticFootstepCharacter> Character;
	FAudioDeviceHandle Device;
	FIMAcousticMetaSoundContextPtr Context;
	FDelegateHandle Initialized;
	FDelegateHandle Cleanup;
	FDelegateHandle PreActorTick;
	FDelegateHandle PostActorTick;
	FTSTicker::FDelegateHandle Ticker;
	FString WorldPath;
	FString ActorPath;
	FString SourcePath;
	FString Failure;
	FString OutputDirectory;
	FString ObserverPath;
	FString ControllerPath;
	uint64 ComponentID = 0;
	uint64 PreparationID = 0;
	uint64 ContextGeneration = 0;
	uint32 DeviceID = 0;
	uint32 SampleRate = 0;
	double ArmedSeconds = 0.0;
	double InitializedSeconds = 0.0;
	double BoundSeconds = 0.0;
	double CleanupSeconds = 0.0;
	double ClosedSampleSeconds = 0.0;
	double CharacterObservedSeconds = 0.0;
	uint32 Contacts = 0;
	uint32 WoodContacts = 0;
	uint32 StoneContacts = 0;
	uint32 Landings = 0;
	uint32 WoodLandings = 0;
	uint32 StoneLandings = 0;
	bool CapturePCM = false;
	bool CaptureCoverageCause = false;
	bool CaptureQuietTail = false;
	bool CaptureVoiceMaster = false;
	bool CaptureNaturalAV = false;
	bool HaveInitialListenerTransform = false;
	bool ListenerTransitionObserved = false;
	FTransform InitialListenerTransform = FTransform::Identity;
	FIMListenerInitializationProbe ListenerInitialization[24]{};
	uint32 ListenerInitializationCount = 0;
	bool BoundWorld = false;
	bool Cleaned = false;
	bool CaptureSettingOwned = false;
	bool Prepared = false;
	bool PreparationConsumed = false;
	bool Finishing = false;
	bool HaveClosedSample = false;
	FIMAcousticOrdinaryEnvironmentCaptureCounts BoundCounts;
	FIMAcousticOrdinaryEnvironmentCaptureCounts ClosedCounts;
};

TUniquePtr<FIMEnvironmentBoundaryOwner> IMEnvironmentOwner;

FString IMEnvironmentOutput()
{
	if (IMEnvironmentOwner) return IMEnvironmentOwner->OutputDirectory;
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/EnvironmentR109Control_20261008"));
}

bool IMEnvironmentSave(const TCHAR* Name, const TSharedRef<FJsonObject>& Object, const FString& OutputDirectory = FString())
{
	FString Text;
	return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *FPaths::Combine(OutputDirectory.IsEmpty() ? IMEnvironmentOutput() : OutputDirectory, Name),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void IMEnvironmentFailure(FIMEnvironmentBoundaryOwner& State, const TCHAR* Reason)
{
	if (State.Failure.IsEmpty()) State.Failure = Reason;
}

FIMAcousticOrdinaryEnvironmentCaptureCounts IMEnvironmentCounts(const FIMAcousticMetaSoundContext& Context)
{
	FIMAcousticOrdinaryEnvironmentCaptureCounts Counts;
	Counts.SourceBlocks = Context.SourceBlocks.load(std::memory_order_acquire);
	Counts.SourceFrames = Context.SourceFrames.load(std::memory_order_acquire);
	Counts.EnvironmentBlocks = Context.EnvironmentBlocks.load(std::memory_order_acquire);
	Counts.EnvironmentFrames = Context.EnvironmentFrames.load(std::memory_order_acquire);
	Counts.NoIRBlocks = Context.NoIRBlocks.load(std::memory_order_acquire);
	Counts.PushBlocks = Context.SourceSendPushBlocks.load(std::memory_order_acquire);
	Counts.PushFailures = Context.SourceSendPushFailures.load(std::memory_order_acquire);
	Counts.SourceClaims = Context.CapturedSourceBlockClaimCount.load(std::memory_order_acquire);
	Counts.SourcePublished = Context.CapturedSourceBlockCount.load(std::memory_order_acquire);
	Counts.EnvironmentPublished = Context.CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
	Counts.BusPublished = Context.CapturedBusReadCount.load(std::memory_order_acquire);
	Counts.SourcePCMFrames = Context.CapturedSourceFrames.load(std::memory_order_acquire);
	Counts.EnvironmentPCMFrames = Context.CapturedEnvironmentFrames.load(std::memory_order_acquire);
	Counts.EnvironmentPCMBlocks = Context.CapturedBlockCount.load(std::memory_order_acquire);
	Counts.SnapshotPushes = Context.Device->SnapshotProbePushes.load(std::memory_order_acquire);
	Counts.SnapshotOverflows = Context.Device->SnapshotProbeOverflows.load(std::memory_order_acquire);
	Counts.WorkerPushes = Context.Device->WorkerProbePushes.load(std::memory_order_acquire);
	Counts.WorkerOverflows = Context.Device->WorkerProbeOverflows.load(std::memory_order_acquire);
	return Counts;
}

TSharedRef<FJsonObject> IMEnvironmentCountsJson(const FIMAcousticOrdinaryEnvironmentCaptureCounts& Counts)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("source_blocks"), double(Counts.SourceBlocks));
	Result->SetNumberField(TEXT("source_frames"), double(Counts.SourceFrames));
	Result->SetNumberField(TEXT("environment_blocks"), double(Counts.EnvironmentBlocks));
	Result->SetNumberField(TEXT("environment_frames"), double(Counts.EnvironmentFrames));
	Result->SetNumberField(TEXT("no_ir_blocks"), double(Counts.NoIRBlocks));
	Result->SetNumberField(TEXT("source_send_push_blocks"), double(Counts.PushBlocks));
	Result->SetNumberField(TEXT("source_send_push_failures"), double(Counts.PushFailures));
	Result->SetNumberField(TEXT("source_claims"), Counts.SourceClaims);
	Result->SetNumberField(TEXT("source_published"), Counts.SourcePublished);
	Result->SetNumberField(TEXT("environment_published"), Counts.EnvironmentPublished);
	Result->SetNumberField(TEXT("bus_published"), Counts.BusPublished);
	Result->SetNumberField(TEXT("source_pcm_frames"), Counts.SourcePCMFrames);
	Result->SetNumberField(TEXT("environment_pcm_frames"), Counts.EnvironmentPCMFrames);
	Result->SetNumberField(TEXT("environment_pcm_blocks"), Counts.EnvironmentPCMBlocks);
	Result->SetNumberField(TEXT("snapshot_pushes"), double(Counts.SnapshotPushes));
	Result->SetNumberField(TEXT("snapshot_overflows"), double(Counts.SnapshotOverflows));
	Result->SetNumberField(TEXT("worker_pushes"), double(Counts.WorkerPushes));
	Result->SetNumberField(TEXT("worker_overflows"), double(Counts.WorkerOverflows));
	return Result;
}

uint32 IMEnvironmentSourceConsumers(const FIMAcousticMetaSoundContext& Context)
{
	uint32 Count = 0;
	for (const auto& Consumer : Context.SourceConsumers)
		if (Consumer.load(std::memory_order_acquire)) ++Count;
	return Count;
}

uint32 IMEnvironmentSourceActive(const FIMAcousticMetaSoundContext& Context)
{
	uint32 Count = 0;
	for (const auto& Active : Context.SourceSendActive)
		if (Active.load(std::memory_order_acquire)) ++Count;
	return Count;
}

bool IMEnvironmentClosed(const FIMEnvironmentBoundaryOwner& State)
{
	if (State.BoundWorld && !State.Cleaned) return false;
	if (!State.Context) return !State.BoundWorld || State.Cleaned;
	const auto& Context = *State.Context;
	if (!Context.Stopped.load(std::memory_order_acquire)
		|| IMAcousticMetaSound::FindAcousticMetaSoundContext(State.DeviceID).IsValid()
		|| !Context.Pool->Stopped.load(std::memory_order_acquire)
		|| Context.Device->Alive.load(std::memory_order_acquire)
		|| Context.Device->WorldGeneration.load(std::memory_order_acquire) != 0
		|| Context.EnvironmentConsumer.load(std::memory_order_acquire)
		|| Context.BusReaderConsumer.load(std::memory_order_acquire)
		|| IMEnvironmentSourceConsumers(Context) || IMEnvironmentSourceActive(Context)
		|| Context.Device->SourceRendererInstances.load(std::memory_order_acquire)
		|| Context.Device->ReverbEffectInstances.load(std::memory_order_acquire)) return false;
	for (const auto& Slot : Context.Pool->Slots)
	{
		const auto SlotState = Slot.State.load(std::memory_order_acquire);
		if (SlotState == EIMAcousticIRState::Writing || SlotState == EIMAcousticIRState::Reading) return false;
	}
	return true;
}

void IMEnvironmentReleaseCaptureSetting(FIMEnvironmentBoundaryOwner& State)
{
	if (!State.CaptureSettingOwned) return;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
	State.CaptureSettingOwned = false;
}

void IMEnvironmentObserveContext(FIMEnvironmentBoundaryOwner& State)
{
	if (!State.BoundWorld || State.Cleaned || !State.Device.IsValid()) return;
	auto Current = IMAcousticMetaSound::FindAcousticMetaSoundContext(State.DeviceID);
	if (State.Context)
	{
		if (Current && Current != State.Context)
			IMEnvironmentFailure(State, TEXT("same_device_context_replaced_no_cross_epoch_merge"));
		return;
	}
	if (!Current) return;
	State.Context = MoveTemp(Current);
	State.BoundSeconds = FPlatformTime::Seconds();
	State.ContextGeneration = State.Context->Pool->WorldGeneration;
	State.BoundCounts = IMEnvironmentCounts(*State.Context);
	State.PreparationConsumed = State.Prepared && State.Context->TestCapturePreparationId == State.PreparationID;
	const int32 PCMCapacity = State.Context->CapturedSource.Num();
	const bool PCMCapacityMatches = State.CapturePCM
		? PCMCapacity > 0 && State.Context->CapturedBus.Num() == PCMCapacity
			&& State.Context->CapturedDry.Num() == 2 * PCMCapacity && State.Context->CapturedWet.Num() == 2 * PCMCapacity
		: State.Context->CapturedSource.IsEmpty() && State.Context->CapturedDry.IsEmpty()
			&& State.Context->CapturedWet.IsEmpty() && State.Context->CapturedBus.IsEmpty();
	if (State.Context->DeviceId != State.DeviceID || State.Context->Device->SampleRate != State.SampleRate
		|| !State.ContextGeneration || State.Context->CapturedSourceBlocks.Num() != int32(IMTraceCapacity)
		|| State.Context->CapturedBlocks.Num() != int32(IMTraceCapacity)
		|| State.Context->CapturedBusReads.Num() != int32(IMTraceCapacity)
		|| State.Context->CapturedSourceBlockReadyWordCount != (IMTraceCapacity + 63u) / 64u
		|| !State.Context->CapturedSourceBlockReady || !PCMCapacityMatches
		|| !State.Context->CapturedTransportOrder.IsEmpty()
		|| State.Context->Device->ProfilingEnabled.load(std::memory_order_acquire)
		|| (State.Prepared && !State.PreparationConsumed))
		IMEnvironmentFailure(State, TEXT("actual_context_capture_identity_capacity_or_preparation_mismatch"));
	IMEnvironmentReleaseCaptureSetting(State);
}

void IMEnvironmentObserveCharacter(FIMEnvironmentBoundaryOwner& State)
{
	const auto* Character = State.Character.Get();
	if (!Character) return;
	State.Contacts = Character->GetContactCount();
	State.WoodContacts = Character->GetWoodContactCount();
	State.StoneContacts = Character->GetStoneContactCount();
	State.Landings = Character->GetLandingCount();
	State.WoodLandings = Character->GetWoodLandingCount();
	State.StoneLandings = Character->GetStoneLandingCount();
	State.CharacterObservedSeconds = FPlatformTime::Seconds();
}

void IMEnvironmentObserveListenerInitialization(FIMEnvironmentBoundaryOwner& State, uint8 Phase)
{
	check(IsInGameThread());
	auto* World = State.World.Get();
	const auto* Character = State.Character.Get();
	if (!State.CaptureCoverageCause || State.Cleaned || !World || !Character || !State.Device.IsValid()
		|| State.ListenerTransitionObserved || State.ListenerInitializationCount >= 24) return;
	auto& Row = State.ListenerInitialization[State.ListenerInitializationCount++];
	Row.Seconds = FPlatformTime::Seconds();
	Row.Frame = GFrameCounter;
	Row.Phase = Phase;
	Row.CharacterBegunPlay = Character->HasActorBegunPlay();
	FTransform Listener;
	Row.ListenerValid = State.Device.GetAudioDevice()->GetListenerTransform(0, Listener);
	Row.ListenerContainsNaN = Listener.ContainsNaN();
	Row.DeviceListenerUE = Listener.GetLocation();
	Row.DeviceListenerRotation = Listener.GetRotation();
	if (Row.ListenerValid && !Row.ListenerContainsNaN)
	{
		if (!State.HaveInitialListenerTransform)
		{
			State.InitialListenerTransform = Listener;
			State.HaveInitialListenerTransform = true;
		}
		else Row.NativeTransformChanged = !Listener.Equals(State.InitialListenerTransform, 1.e-6f);
	}
	auto* Observer = Character->ObserverCamera.Get();
	if (Observer)
	{
		Row.ObserverUE = Observer->GetActorLocation();
		Row.CameraBegunPlay = Observer->HasActorBegunPlay();
		State.ObserverPath = Observer->GetPathName();
	}
	auto* Controller = World->GetFirstPlayerController();
	Row.ControllerExists = Controller != nullptr;
	if (Controller)
	{
		FVector Front, Right;
		Controller->GetAudioListenerPosition(Row.ControllerListenerUE, Front, Right);
		Row.ControllerBegunPlay = Controller->HasActorBegunPlay();
		Row.ViewTargetIsObserver = Observer && Controller->GetViewTarget() == Observer;
		State.ControllerPath = Controller->GetPathName();
	}
	State.ListenerTransitionObserved = Row.NativeTransformChanged;
}

TSharedRef<FJsonObject> IMEnvironmentStatus()
{
	auto Report = MakeShared<FJsonObject>();
	if (!IMEnvironmentOwner)
	{
		Report->SetStringField(TEXT("state"), TEXT("OFF"));
		Report->SetBoolField(TEXT("own_capture_setting_released"), true);
		Report->SetBoolField(TEXT("own_references_released"), true);
		return Report;
	}
	const auto& State = *IMEnvironmentOwner;
	Report->SetStringField(TEXT("state"), State.Finishing ? TEXT("FINISHING") : State.Context ? TEXT("CAPTURING") : TEXT("ARMED"));
	Report->SetStringField(TEXT("failure"), State.Failure);
	Report->SetStringField(TEXT("world"), State.WorldPath);
	Report->SetStringField(TEXT("actor"), State.ActorPath);
	Report->SetStringField(TEXT("source"), State.SourcePath);
	Report->SetStringField(TEXT("component_id"), LexToString(State.ComponentID));
	Report->SetStringField(TEXT("context_generation"), LexToString(State.ContextGeneration));
	Report->SetStringField(TEXT("preparation_id"), LexToString(State.PreparationID));
	Report->SetNumberField(TEXT("device_id"), State.DeviceID);
	Report->SetNumberField(TEXT("sample_rate"), State.SampleRate);
	Report->SetNumberField(TEXT("armed_seconds"), State.ArmedSeconds);
	Report->SetNumberField(TEXT("world_initialized_seconds"), State.InitializedSeconds);
	Report->SetNumberField(TEXT("context_observed_seconds"), State.BoundSeconds);
	Report->SetNumberField(TEXT("world_cleanup_seconds"), State.CleanupSeconds);
	Report->SetBoolField(TEXT("bound_before_begin_play"), State.BoundWorld);
	Report->SetBoolField(TEXT("context_bound"), State.Context.IsValid());
	Report->SetBoolField(TEXT("prepared_actual_world_device_before_context"), State.Prepared);
	Report->SetBoolField(TEXT("preparation_consumed"), State.PreparationConsumed);
	Report->SetBoolField(TEXT("world_cleanup_observed"), State.Cleaned);
	Report->SetBoolField(TEXT("own_capture_setting_released"), !State.CaptureSettingOwned);
	Report->SetBoolField(TEXT("native_closure_ready"), IMEnvironmentClosed(State));
	Report->SetNumberField(TEXT("trace_capacity_rows"), IMTraceCapacity);
	Report->SetBoolField(TEXT("capture_pcm"), State.CapturePCM);
	Report->SetBoolField(TEXT("capture_initial_coverage_cause"), State.CaptureCoverageCause);
	Report->SetBoolField(TEXT("capture_quiet_tail"), State.CaptureQuietTail);
	Report->SetNumberField(TEXT("listener_initialization_observations"), State.ListenerInitializationCount);
	Report->SetBoolField(TEXT("native_listener_transition_observed"), State.ListenerTransitionObserved);
	Report->SetNumberField(TEXT("gt_platform_seconds"), FPlatformTime::Seconds());
	Report->SetNumberField(TEXT("character_observed_seconds"), State.CharacterObservedSeconds);
	Report->SetStringField(TEXT("event_clock_semantics"), TEXT("GT counter polling observation, not trigger submission or audio/master frame"));
	Report->SetNumberField(TEXT("contacts"), State.Contacts);
	Report->SetNumberField(TEXT("wood_contacts"), State.WoodContacts);
	Report->SetNumberField(TEXT("stone_contacts"), State.StoneContacts);
	Report->SetNumberField(TEXT("landings"), State.Landings);
	Report->SetNumberField(TEXT("wood_landings"), State.WoodLandings);
	Report->SetNumberField(TEXT("stone_landings"), State.StoneLandings);
	Report->SetBoolField(TEXT("capture_bus_reads"), true);
	Report->SetBoolField(TEXT("capture_transport_order"), false);
	Report->SetBoolField(TEXT("product_stop_called_by_fixture"), false);
	Report->SetBoolField(TEXT("product_counters_or_arrays_reset_by_fixture"), false);
	Report->SetBoolField(TEXT("retained_context_delays_destructor_until_release"), State.Context.IsValid());
	Report->SetField(TEXT("master_consumed_frame_mapping"), MakeShared<FJsonValueNull>());
	Report->SetObjectField(TEXT("counts_at_first_context_observation"), IMEnvironmentCountsJson(State.BoundCounts));
	if (State.Context)
	{
		const auto& Context = *State.Context;
		Report->SetBoolField(TEXT("native_profiling_enabled"), Context.Device->ProfilingEnabled.load(std::memory_order_acquire));
		Report->SetObjectField(TEXT("counters"), IMEnvironmentCountsJson(IMEnvironmentCounts(Context)));
		Report->SetBoolField(TEXT("context_stopped"), Context.Stopped.load(std::memory_order_acquire));
		Report->SetBoolField(TEXT("registry_removed"), !IMAcousticMetaSound::FindAcousticMetaSoundContext(State.DeviceID));
		Report->SetBoolField(TEXT("pool_stopped"), Context.Pool->Stopped.load(std::memory_order_acquire));
		Report->SetBoolField(TEXT("device_alive"), Context.Device->Alive.load(std::memory_order_acquire));
		Report->SetStringField(TEXT("device_world_generation"), LexToString(Context.Device->WorldGeneration.load(std::memory_order_acquire)));
		Report->SetBoolField(TEXT("environment_consumer"), Context.EnvironmentConsumer.load(std::memory_order_acquire));
		Report->SetBoolField(TEXT("bus_reader_consumer"), Context.BusReaderConsumer.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("source_consumer_count"), IMEnvironmentSourceConsumers(Context));
		Report->SetNumberField(TEXT("source_send_active_count"), IMEnvironmentSourceActive(Context));
		Report->SetNumberField(TEXT("source_renderer_instances"), Context.Device->SourceRendererInstances.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("reverb_effect_instances"), Context.Device->ReverbEffectInstances.load(std::memory_order_acquire));
	}
	return Report;
}

TSharedRef<FJsonObject> IMEnvironmentRowJson(const FIMAcousticMetaSoundBlock& Row, uint32 Ordinal)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("ordinal"), Ordinal);
	Result->SetNumberField(TEXT("frame"), double(Row.Frame));
	Result->SetNumberField(TEXT("seconds"), Row.Seconds);
	Result->SetNumberField(TEXT("captured_seconds"), Row.CapturedSeconds);
	Result->SetStringField(TEXT("sequence"), LexToString(Row.Sequence));
	Result->SetStringField(TEXT("input_spatial_version"), LexToString(Row.InputSpatialVersion));
	Result->SetStringField(TEXT("expected_spatial_version"), LexToString(Row.ExpectedSpatialVersion));
	Result->SetStringField(TEXT("spatial_version"), LexToString(Row.SpatialVersion));
	Result->SetStringField(TEXT("device_world_generation"), LexToString(Row.DeviceWorldGeneration));
	Result->SetStringField(TEXT("pool_world_generation"), LexToString(Row.PoolWorldGeneration));
	Result->SetNumberField(TEXT("listener_x_m"), Row.ListenerX);
	Result->SetNumberField(TEXT("listener_y_m"), Row.ListenerY);
	Result->SetNumberField(TEXT("listener_z_m"), Row.ListenerZ);
	Result->SetNumberField(TEXT("render_routes"), Row.RenderRoutes);
	Result->SetBoolField(TEXT("has_current"), Row.HasCurrent);
	Result->SetBoolField(TEXT("delay_priming"), Row.DelayPriming);
	Result->SetBoolField(TEXT("enabled"), Row.Enabled);
	Result->SetBoolField(TEXT("fresh"), Row.Fresh);
	Result->SetBoolField(TEXT("render_attempted"), Row.RenderAttempted);
	Result->SetBoolField(TEXT("rendered"), Row.Rendered);
	return Result;
}

const TCHAR* IMEnvironmentBusResult(EIMAcousticBusReadResult Result)
{
	switch (Result)
	{
	case EIMAcousticBusReadResult::Stopped: return TEXT("Stopped");
	case EIMAcousticBusReadResult::InvalidFrames: return TEXT("InvalidFrames");
	case EIMAcousticBusReadResult::LatchedFault: return TEXT("LatchedFault");
	case EIMAcousticBusReadResult::NoInputPriming: return TEXT("NoInputPriming");
	case EIMAcousticBusReadResult::NoInputSilence: return TEXT("NoInputSilence");
	case EIMAcousticBusReadResult::ReservePriming: return TEXT("ReservePriming");
	case EIMAcousticBusReadResult::Complete: return TEXT("Complete");
	case EIMAcousticBusReadResult::ShortAvailable: return TEXT("ShortAvailable");
	case EIMAcousticBusReadResult::ShortPop: return TEXT("ShortPop");
	}
	return TEXT("Unknown");
}

TSharedRef<FJsonObject> IMEnvironmentBusRowJson(const FIMAcousticBusReadBlock& Row)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("ordinal"), Row.Ordinal);
	Result->SetNumberField(TEXT("frames"), Row.Frames);
	Result->SetNumberField(TEXT("prime_frames"), Row.PrimeFrames);
	Result->SetNumberField(TEXT("capacity_frames"), Row.CapacityFrames);
	Result->SetNumberField(TEXT("input_count"), Row.InputCount);
	Result->SetNumberField(TEXT("active_count"), Row.ActiveCount);
	Result->SetNumberField(TEXT("min_available"), Row.MinAvailable);
	Result->SetNumberField(TEXT("max_available"), Row.MaxAvailable);
	Result->SetNumberField(TEXT("blocking_slot"), Row.BlockingSlot);
	Result->SetNumberField(TEXT("blocking_available"), Row.BlockingAvailable);
	Result->SetNumberField(TEXT("blocking_queue_after"), Row.BlockingQueueAfter);
	Result->SetNumberField(TEXT("min_popped"), Row.MinPopped);
	Result->SetStringField(TEXT("blocking_audio_id"), LexToString(Row.BlockingAudioId));
	Result->SetNumberField(TEXT("blocking_push_blocks"), double(Row.BlockingPushBlocks));
	Result->SetNumberField(TEXT("underruns_before"), double(Row.UnderrunsBefore));
	Result->SetNumberField(TEXT("underruns_after"), double(Row.UnderrunsAfter));
	Result->SetNumberField(TEXT("read_blocks"), double(Row.ReadBlocks));
	Result->SetNumberField(TEXT("priming_blocks"), double(Row.PrimingBlocks));
	Result->SetNumberField(TEXT("silent_blocks"), double(Row.SilentBlocks));
	Result->SetNumberField(TEXT("push_blocks"), double(Row.PushBlocks));
	Result->SetNumberField(TEXT("push_failures"), double(Row.PushFailures));
	Result->SetNumberField(TEXT("begin_seconds"), Row.BeginSeconds);
	Result->SetNumberField(TEXT("end_seconds"), Row.EndSeconds);
	Result->SetStringField(TEXT("result"), IMEnvironmentBusResult(Row.Result));
	Result->SetBoolField(TEXT("primed_before"), Row.PrimedBefore);
	Result->SetBoolField(TEXT("primed_after"), Row.PrimedAfter);
	Result->SetBoolField(TEXT("transport_failed_before"), Row.TransportFailedBefore);
	Result->SetBoolField(TEXT("transport_failed_after"), Row.TransportFailedAfter);
	Result->SetBoolField(TEXT("blocking_active"), Row.BlockingActive);
	Result->SetBoolField(TEXT("blocking_active_after"), Row.BlockingActiveAfter);
	Result->SetBoolField(TEXT("returned_fresh"), Row.ReturnedFresh);
	return Result;
}

TSharedRef<FJsonObject> IMEnvironmentSourceRowJson(const FIMAcousticBlockProbe& Row)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("block"), double(Row.Block));
	Result->SetNumberField(TEXT("voice"), Row.Voice);
	Result->SetStringField(TEXT("audio_component_id"), LexToString(Row.CallbackAudioComponentId));
	Result->SetStringField(TEXT("result_audio_component_id"), LexToString(Row.ResultAudioComponentId));
	Result->SetStringField(TEXT("result_world_generation"), LexToString(Row.ResultWorldGeneration));
	Result->SetStringField(TEXT("result_voice_generation"), LexToString(Row.ResultVoiceGeneration));
	Result->SetStringField(TEXT("result_sequence"), LexToString(Row.ResultSequence));
	Result->SetStringField(TEXT("input_spatial_version"), LexToString(Row.InputSpatialVersion));
	Result->SetStringField(TEXT("output_spatial_version"), LexToString(Row.OutputSpatialVersion));
	Result->SetStringField(TEXT("result_spatial_version"), LexToString(Row.ResultSpatialVersion));
	Result->SetNumberField(TEXT("snapshot_captured_seconds"), Row.SnapshotCaptured);
	Result->SetNumberField(TEXT("consumed_seconds"), Row.ConsumedSeconds);
	Result->SetNumberField(TEXT("age_ms"), Row.AgeMs);
	Result->SetNumberField(TEXT("render_routes"), Row.Routes);
	Result->SetNumberField(TEXT("reject"), uint32(Row.Reject));
	Result->SetNumberField(TEXT("reject_detail"), Row.RejectDetail);
	Result->SetNumberField(TEXT("fallback"), Row.Fallback);
	Result->SetNumberField(TEXT("input_delay_priming"), Row.InputDelayPriming);
	Result->SetNumberField(TEXT("direct_valid"), Row.DirectValid);
	Result->SetNumberField(TEXT("path_valid"), Row.PathValid);
	Result->SetNumberField(TEXT("input_energy"), Row.InputEnergy);
	Result->SetNumberField(TEXT("reverb_send_energy"), Row.ReverbSendEnergy);
	Result->SetNumberField(TEXT("output_energy"), Row.OutputEnergy);
	Result->SetNumberField(TEXT("callback_distance_cm"), Row.CallbackDistanceCm);
	Result->SetNumberField(TEXT("degraded_gain"), Row.DegradedGain);
	return Result;
}

TSharedRef<FJsonObject> IMEnvironmentSnapshotJson(const FIMAcousticSnapshotProbe& Row, uint64 Done)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("block"), double(Row.Block));
	Result->SetStringField(TEXT("done_acquire"), LexToString(Done));
	Result->SetStringField(TEXT("world_generation"), LexToString(Row.WorldGeneration));
	Result->SetStringField(TEXT("spatial_version"), LexToString(Row.SpatialVersion));
	Result->SetNumberField(TEXT("captured_seconds"), Row.CapturedSeconds);
	Result->SetNumberField(TEXT("submit_seconds"), Row.SubmitSeconds);
	Result->SetNumberField(TEXT("listener_ue_x_cm"), Row.ListenerUEX);
	Result->SetNumberField(TEXT("listener_ue_y_cm"), Row.ListenerUEY);
	Result->SetNumberField(TEXT("listener_ue_z_cm"), Row.ListenerUEZ);
	Result->SetNumberField(TEXT("listener_sdk_x_m"), Row.ListenerSDKX);
	Result->SetNumberField(TEXT("listener_sdk_y_m"), Row.ListenerSDKY);
	Result->SetNumberField(TEXT("listener_sdk_z_m"), Row.ListenerSDKZ);
	Result->SetNumberField(TEXT("source0_x_m"), Row.Source0X);
	Result->SetNumberField(TEXT("source0_y_m"), Row.Source0Y);
	Result->SetNumberField(TEXT("source0_z_m"), Row.Source0Z);
	Result->SetStringField(TEXT("source0_audio_id"), LexToString(Row.Source0AudioId));
	Result->SetNumberField(TEXT("num_sources"), Row.NumSources);
	Result->SetNumberField(TEXT("submitted"), Row.Submitted);
	Result->SetNumberField(TEXT("fail_code"), Row.FailCode);
	Result->SetNumberField(TEXT("num_dynamic_meshes"), Row.NumDynamicMeshes);
	Result->SetNumberField(TEXT("num_dynamic_skipped"), Row.NumDynamicSkipped);
	Result->SetNumberField(TEXT("door0_x_m"), Row.Door0TX);
	Result->SetNumberField(TEXT("door0_y_m"), Row.Door0TY);
	Result->SetNumberField(TEXT("door0_z_m"), Row.Door0TZ);
	return Result;
}

TSharedRef<FJsonObject> IMEnvironmentWorkerJson(const FIMAcousticWorkerProbe& Row, uint64 Done)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("block"), double(Row.Block));
	Result->SetStringField(TEXT("done_acquire"), LexToString(Done));
	Result->SetStringField(TEXT("world_generation"), LexToString(Row.WorldGeneration));
	Result->SetNumberField(TEXT("loop_start_seconds"), Row.LoopStartSeconds);
	Result->SetNumberField(TEXT("snapshot_captured_seconds"), Row.SnapshotCaptured);
	Result->SetNumberField(TEXT("snap_valid"), Row.SnapValid);
	Result->SetNumberField(TEXT("snap_reason"), Row.SnapReason);
	Result->SetNumberField(TEXT("num_inputs"), Row.NumInputs);
	Result->SetNumberField(TEXT("results_published"), Row.ResultsPublished);
	Result->SetNumberField(TEXT("eval_start_seconds"), Row.EvalStartSeconds);
	Result->SetNumberField(TEXT("eval_end_seconds"), Row.EvalEndSeconds);
	Result->SetNumberField(TEXT("eval_ok"), Row.EvalOk);
	Result->SetNumberField(TEXT("reverb_attempt"), Row.ReverbAttempt);
	Result->SetNumberField(TEXT("reverb_ok"), Row.ReverbOk);
	Result->SetNumberField(TEXT("reverb_start_seconds"), Row.ReverbStartSeconds);
	Result->SetNumberField(TEXT("reverb_end_seconds"), Row.ReverbEndSeconds);
	Result->SetStringField(TEXT("reverb_sequence"), LexToString(Row.ReverbSequence));
	Result->SetNumberField(TEXT("reverb_captured_seconds"), Row.ReverbCaptured);
	Result->SetStringField(TEXT("reverb_spatial_version"), LexToString(Row.ReverbSpatialVersion));
	Result->SetNumberField(TEXT("reverb_slot"), Row.ReverbSlot);
	Result->SetNumberField(TEXT("reverb_ready_before_seconds"), Row.ReverbReadyBeforeSeconds);
	Result->SetNumberField(TEXT("reverb_ready_after_seconds"), Row.ReverbReadyAfterSeconds);
	Result->SetNumberField(TEXT("push_seconds"), Row.PushSeconds);
	Result->SetNumberField(TEXT("push_ok"), Row.PushOk);
	Result->SetNumberField(TEXT("wait_end_seconds"), Row.WaitEndSeconds);
	Result->SetNumberField(TEXT("first_voice"), Row.FirstVoice);
	Result->SetStringField(TEXT("first_audio_id"), LexToString(Row.FirstAudioId));
	Result->SetStringField(TEXT("first_voice_generation"), LexToString(Row.FirstVoiceGen));
	Result->SetStringField(TEXT("first_sequence"), LexToString(Row.FirstSeq));
	Result->SetNumberField(TEXT("num_dynamic_synced"), Row.NumDynamicSynced);
	Result->SetNumberField(TEXT("applied_validation_snapshot"), Row.AppliedValidationSnap);
	return Result;
}

bool IMEnvironmentExportUpstream(const FIMEnvironmentBoundaryOwner& State, const TSharedRef<FJsonObject>& Report)
{
	const auto& Device = *State.Context->Device;
	const auto Counts = IMEnvironmentCounts(*State.Context);
	TArray<TSharedPtr<FJsonValue>> Snapshots;
	TArray<TSharedPtr<FJsonValue>> Workers;
	bool Valid = Counts.SnapshotPushes > 0 && Counts.WorkerPushes > 0
		&& Counts.SnapshotPushes <= Device.ProbeSnapshotCapacity && Counts.WorkerPushes <= Device.ProbeWorkerCapacity
		&& Counts.SnapshotOverflows == 0 && Counts.WorkerOverflows == 0;
	for (uint64 Index = 0; Index < FMath::Min(Counts.SnapshotPushes, uint64(Device.ProbeSnapshotCapacity)); ++Index)
	{
		const uint64 Done = Device.SnapshotDone[Index].load(std::memory_order_acquire);
		Valid &= Done == Index + 1;
		if (Done != Index + 1) continue;
		const auto& Row = Device.SnapshotProbes[Index];
		Valid &= Row.Block == Index && Row.WorldGeneration == State.ContextGeneration;
		if (!State.CaptureQuietTail && (!State.CaptureCoverageCause || Index < 4))
			Snapshots.Add(MakeShared<FJsonValueObject>(IMEnvironmentSnapshotJson(Row, Done)));
	}
	for (uint64 Index = 0; Index < FMath::Min(Counts.WorkerPushes, uint64(Device.ProbeWorkerCapacity)); ++Index)
	{
		const uint64 Done = Device.WorkerDone[Index].load(std::memory_order_acquire);
		Valid &= Done == Index + 1;
		if (Done != Index + 1) continue;
		const auto& Row = Device.WorkerProbes[Index];
		Valid &= Row.Block == Index && Row.WorldGeneration == State.ContextGeneration;
		if (!State.CaptureQuietTail && (!State.CaptureCoverageCause || Index < 12))
			Workers.Add(MakeShared<FJsonValueObject>(IMEnvironmentWorkerJson(Row, Done)));
	}
	Report->SetNumberField(TEXT("snapshot_prefix_capacity"), Device.ProbeSnapshotCapacity);
	Report->SetNumberField(TEXT("worker_prefix_capacity"), Device.ProbeWorkerCapacity);
	Report->SetBoolField(TEXT("upstream_done_prefix_valid"), Valid);
	Report->SetBoolField(TEXT("upstream_full_prefix_exported"), !State.CaptureCoverageCause && !State.CaptureQuietTail);
	Report->SetStringField(TEXT("upstream_export_scope"), State.CaptureQuietTail
		? TEXT("no upstream rows exported for quiet-tail batch; Done validated without replaying initial-cause task") : State.CaptureCoverageCause
		? TEXT("only first four GT snapshots and first twelve worker rows for initial failure key; Done validated without whole-prefix export")
		: TEXT("complete fixed native published prefix"));
	Report->SetStringField(TEXT("upstream_publication_barrier"), TEXT("each Done acquire must equal native release index+1; export after native worker join/world cleanup"));
	Report->SetStringField(TEXT("native_clock_domain"), TEXT("FPlatformTime::Seconds, seconds; not World game time or Python monotonic time"));
	Report->SetStringField(TEXT("ready_clock_semantics"), TEXT("profiling unchanged OFF; zero Ready bracket fields are unavailable, successful native publication lies after ReverbEnd and before WaitEnd, not at ReverbEnd"));
	Report->SetArrayField(TEXT("snapshot_rows"), Snapshots);
	Report->SetArrayField(TEXT("worker_rows"), Workers);
	return Valid;
}

TSharedRef<FJsonValueArray> IMEnvironmentVectorJson(const FVector& Value)
{
	TArray<TSharedPtr<FJsonValue>> Coordinates;
	Coordinates.Add(MakeShared<FJsonValueNumber>(Value.X));
	Coordinates.Add(MakeShared<FJsonValueNumber>(Value.Y));
	Coordinates.Add(MakeShared<FJsonValueNumber>(Value.Z));
	return MakeShared<FJsonValueArray>(Coordinates);
}

void IMEnvironmentExportInitialCause(const FIMEnvironmentBoundaryOwner& State, const TSharedRef<FJsonObject>& Report)
{
	if (!State.CaptureCoverageCause) return;
	Report->SetStringField(TEXT("observer_path"), State.ObserverPath);
	Report->SetStringField(TEXT("controller_path"), State.ControllerPath);
	TArray<TSharedPtr<FJsonValue>> Initialization;
	for (uint32 Index = 0; Index < State.ListenerInitializationCount; ++Index)
	{
		const auto& Probe = State.ListenerInitialization[Index];
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("ordinal"), Index);
		Row->SetNumberField(TEXT("seconds"), Probe.Seconds);
		Row->SetNumberField(TEXT("gt_frame"), double(Probe.Frame));
		Row->SetNumberField(TEXT("phase"), Probe.Phase);
		Row->SetStringField(TEXT("phase_semantics"), Probe.Phase == 1 ? TEXT("WorldInitializedActors before BeginPlay")
			: Probe.Phase == 2 ? TEXT("PreActorTick") : Probe.Phase == 3 ? TEXT("PostActorTick") : TEXT("CoreTicker GT observation"));
		Row->SetBoolField(TEXT("device_listener_valid"), Probe.ListenerValid);
		Row->SetBoolField(TEXT("device_listener_contains_nan"), Probe.ListenerContainsNaN);
		Row->SetBoolField(TEXT("character_begun_play"), Probe.CharacterBegunPlay);
		Row->SetBoolField(TEXT("controller_exists"), Probe.ControllerExists);
		Row->SetBoolField(TEXT("controller_begun_play"), Probe.ControllerBegunPlay);
		Row->SetBoolField(TEXT("observer_begun_play"), Probe.CameraBegunPlay);
		Row->SetBoolField(TEXT("view_target_is_observer"), Probe.ViewTargetIsObserver);
		Row->SetBoolField(TEXT("native_transform_changed_from_first_observed"), Probe.NativeTransformChanged);
		Row->SetField(TEXT("device_listener_ue_cm"), IMEnvironmentVectorJson(Probe.DeviceListenerUE));
		Row->SetField(TEXT("controller_listener_ue_cm"), IMEnvironmentVectorJson(Probe.ControllerListenerUE));
		Row->SetField(TEXT("observer_ue_cm"), IMEnvironmentVectorJson(Probe.ObserverUE));
		TArray<TSharedPtr<FJsonValue>> Rotation;
		Rotation.Add(MakeShared<FJsonValueNumber>(Probe.DeviceListenerRotation.X));
		Rotation.Add(MakeShared<FJsonValueNumber>(Probe.DeviceListenerRotation.Y));
		Rotation.Add(MakeShared<FJsonValueNumber>(Probe.DeviceListenerRotation.Z));
		Rotation.Add(MakeShared<FJsonValueNumber>(Probe.DeviceListenerRotation.W));
		Row->SetArrayField(TEXT("device_listener_rotation_xyzw"), Rotation);
		Initialization.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("listener_initialization_rows"), Initialization);
	const uint8 Publication = IMCoverageObservationState.load(std::memory_order_acquire);
	Report->SetNumberField(TEXT("coverage_observation_publication_acquire"), Publication);
	Report->SetBoolField(TEXT("first_coverage_failure_published"), Publication == 3);
	Report->SetStringField(TEXT("coverage_observation_semantics"), TEXT("explicit private opt-in, one worker first-failure POD, release publication, GT export after native worker join; no product reset or semantic change"));
	if (Publication != 3) return;
	const auto& Probe = IMCoverageFailure;
	auto Failure = MakeShared<FJsonObject>();
	Failure->SetNumberField(TEXT("seconds"), Probe.Seconds);
	Failure->SetNumberField(TEXT("listener_x_m"), Probe.ListenerX);
	Failure->SetNumberField(TEXT("listener_y_m"), Probe.ListenerY);
	Failure->SetNumberField(TEXT("listener_z_m"), Probe.ListenerZ);
	Failure->SetNumberField(TEXT("total_probes"), Probe.TotalProbes);
	Failure->SetNumberField(TEXT("visited_probes"), Probe.VisitedProbes);
	Failure->SetNumberField(TEXT("overlaps"), Probe.Overlaps);
	Failure->SetNumberField(TEXT("blocked_conservative"), Probe.Blocked);
	Failure->SetNumberField(TEXT("uncertain_boundary_probes"), Probe.Uncertain);
	Failure->SetNumberField(TEXT("raycast_blocked_probes"), Probe.RaycastBlocked);
	Failure->SetNumberField(TEXT("unsafe_probe_encountered"), Probe.UnsafeProbes);
	Failure->SetNumberField(TEXT("selected_probe_cap"), Probe.SelectedProbeCap);
	Failure->SetNumberField(TEXT("rejection_mask"), Probe.RejectionMask);
	Failure->SetStringField(TEXT("rejection_mask_semantics"), TEXT("bit1 zero overlap; bit2 blocked==overlaps; bit4 blocked>=unchanged selected cap"));
	Report->SetObjectField(TEXT("first_coverage_failure"), Failure);
}

bool IMEnvironmentSavePCM(const FIMEnvironmentBoundaryOwner& State, const TCHAR* Name,
	const TArray<float>& Storage, uint32 Frames, uint32 Channels)
{
	const uint64 Samples = uint64(Frames) * Channels;
	if (!Frames || Samples > uint64(Storage.Num()) || Samples * sizeof(float) > uint64(MAX_int32)) return false;
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(int32(Samples * sizeof(float)));
	FMemory::Memcpy(Bytes.GetData(), Storage.GetData(), Bytes.Num());
	return FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(State.OutputDirectory, Name));
}

bool IMEnvironmentExportPCM(const FIMEnvironmentBoundaryOwner& State, const TSharedRef<FJsonObject>& Report)
{
	const auto& Context = *State.Context;
	const auto Counts = IMEnvironmentCounts(Context);
	Report->SetNumberField(TEXT("allocated_pcm_payload_bytes"),
		double((int64(Context.CapturedSource.Num()) + Context.CapturedDry.Num()
			+ Context.CapturedWet.Num() + Context.CapturedBus.Num()) * sizeof(float)));
	Report->SetStringField(TEXT("pcm_semantics"), TEXT("native Source mono/Dry stereo share source origin; native Bus mono/Wet stereo share environment origin; separate graph stages, NOT master mix; no offline summing"));
	if (!State.CapturePCM) return Counts.SourcePCMFrames == 0 && Counts.EnvironmentPCMFrames == 0;
	const bool Valid = Counts.SourcePCMFrames == Counts.SourceFrames
		&& Counts.EnvironmentPCMFrames == Counts.EnvironmentFrames && Counts.EnvironmentPCMBlocks == Counts.EnvironmentBlocks
		&& IMEnvironmentSavePCM(State, TEXT("source_native_f32le.pcm"), Context.CapturedSource, Counts.SourcePCMFrames, 1)
		&& IMEnvironmentSavePCM(State, TEXT("dry_native_f32le.pcm"), Context.CapturedDry, Counts.SourcePCMFrames, 2)
		&& IMEnvironmentSavePCM(State, TEXT("bus_native_f32le.pcm"), Context.CapturedBus, Counts.EnvironmentPCMFrames, 1)
		&& IMEnvironmentSavePCM(State, TEXT("wet_native_f32le.pcm"), Context.CapturedWet, Counts.EnvironmentPCMFrames, 2);
	Report->SetBoolField(TEXT("native_stage_pcm_prefix_saved"), Valid);
	return Valid;
}

bool IMEnvironmentExport(const FIMEnvironmentBoundaryOwner& State, const TSharedRef<FJsonObject>& Report)
{
	if (!State.Context) return false;
	const auto& Context = *State.Context;
	const auto Counts = IMEnvironmentCounts(Context);
	TArray<TSharedPtr<FJsonValue>> EnvironmentRows;
	TArray<TSharedPtr<FJsonValue>> BusRows;
	TArray<TSharedPtr<FJsonValue>> SourceRows;
	TArray<TSharedPtr<FJsonValue>> Slots;
	bool PrefixValid = Counts.EnvironmentPublished > 0 && Counts.EnvironmentPublished < IMTraceCapacity
		&& Counts.EnvironmentBlocks == Counts.EnvironmentPublished
		&& Counts.EnvironmentFrames == Counts.EnvironmentBlocks * FIMAcousticMetaSoundContext::Frames
		&& Counts.SourceClaims > 0 && Counts.SourceClaims < IMTraceCapacity
		&& Counts.SourceClaims == Counts.SourcePublished && Counts.SourceBlocks == Counts.SourceClaims
		&& Counts.SourceFrames == Counts.SourceBlocks * FIMAcousticMetaSoundContext::Frames
		&& Counts.BusPublished > 0 && Counts.BusPublished < IMTraceCapacity
		&& !Context.CapturedBusReadOverflow.load(std::memory_order_acquire);
	uint64 NoIRRows = 0;
	uint64 FreshRows = 0;
	uint64 RenderedRows = 0;
	for (uint32 Index = 0; Index < FMath::Min(Counts.EnvironmentPublished, uint32(Context.CapturedBlocks.Num())); ++Index)
	{
		const auto& Row = Context.CapturedBlocks[Index];
		PrefixValid &= Row.Frame == uint64(Index) * FIMAcousticMetaSoundContext::Frames
			&& Row.PoolWorldGeneration == State.ContextGeneration && Row.Seconds >= State.ArmedSeconds;
		NoIRRows += Row.Rendered ? 0u : 1u;
		FreshRows += Row.Fresh ? 1u : 0u;
		RenderedRows += Row.Rendered ? 1u : 0u;
		EnvironmentRows.Add(MakeShared<FJsonValueObject>(IMEnvironmentRowJson(Row, Index)));
	}
	for (uint32 Index = 0; Index < FMath::Min(Counts.BusPublished, uint32(Context.CapturedBusReads.Num())); ++Index)
	{
		const auto& Row = Context.CapturedBusReads[Index];
		PrefixValid &= Row.Ordinal == Index && Row.Frames == int32(FIMAcousticMetaSoundContext::Frames)
			&& Row.BeginSeconds >= State.ArmedSeconds && Row.EndSeconds >= Row.BeginSeconds;
		BusRows.Add(MakeShared<FJsonValueObject>(IMEnvironmentBusRowJson(Row)));
	}
	for (uint32 Index = 0; Index < FMath::Min(Counts.SourcePublished, uint32(Context.CapturedSourceBlocks.Num())); ++Index)
	{
		const uint32 Word = Index >> 6;
		const bool Ready = Context.CapturedSourceBlockReady && Word < Context.CapturedSourceBlockReadyWordCount
			&& (Context.CapturedSourceBlockReady[Word].load(std::memory_order_acquire) & (uint64(1) << (Index & 63u)));
		PrefixValid &= Ready;
		if (!Ready) continue;
		const auto& Row = Context.CapturedSourceBlocks[Index];
		PrefixValid &= Row.Block == Index && Row.CallbackAudioComponentId == State.ComponentID
			&& Row.ConsumedSeconds >= State.ArmedSeconds;
		SourceRows.Add(MakeShared<FJsonValueObject>(IMEnvironmentSourceRowJson(Row)));
	}
	PrefixValid &= NoIRRows == Counts.NoIRBlocks;
	PrefixValid &= RenderedRows == Context.Device->ReverbProcessedBlocks.load(std::memory_order_acquire);
	PrefixValid &= Counts.EnvironmentPublished - FreshRows == Context.Device->ReverbNotFreshBlocks.load(std::memory_order_acquire);
	PrefixValid &= FreshRows - RenderedRows == Context.Device->ReverbRejectedBlocks.load(std::memory_order_acquire);
	Report->SetNumberField(TEXT("graph_block_frames"), FIMAcousticMetaSoundContext::Frames);
	Report->SetStringField(TEXT("graph_frame_origin"), TEXT("native environment trace ordinal zero; not master PCM zero"));
	Report->SetBoolField(TEXT("full_native_prefix_valid"), PrefixValid);
	Report->SetBoolField(TEXT("source_ready_prefix_complete"), SourceRows.Num() == int32(Counts.SourceClaims));
	Report->SetBoolField(TEXT("bus_capture_overflow"), Context.CapturedBusReadOverflow.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("environment_no_ir_rows"), double(NoIRRows));
	Report->SetNumberField(TEXT("environment_fresh_rows"), double(FreshRows));
	Report->SetNumberField(TEXT("environment_rendered_rows"), double(RenderedRows));
	Report->SetNumberField(TEXT("invalid_blocks"), double(Context.InvalidBlocks.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("duplicate_consumers"), double(Context.DuplicateConsumers.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("reverb_processed_blocks"), double(Context.Device->ReverbProcessedBlocks.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("reverb_not_fresh_blocks"), double(Context.Device->ReverbNotFreshBlocks.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("reverb_rejected_blocks"), double(Context.Device->ReverbRejectedBlocks.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("bus_read_blocks"), double(Context.BusReadBlocks.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("bus_priming_blocks"), double(Context.BusPrimingBlocks.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("bus_silent_blocks"), double(Context.BusSilentBlocks.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("bus_underruns"), double(Context.BusUnderruns.load(std::memory_order_acquire)));
	Report->SetNumberField(TEXT("bus_min_available"), Context.BusMinAvailable.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("bus_max_available"), Context.BusMaxAvailable.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("source_delay_priming_blocks"), double(Context.SourceDelayPrimingBlocks.load(std::memory_order_acquire)));
	Report->SetBoolField(TEXT("source_send_transport_failed"), Context.SourceSendTransportFailed.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("source_pcm_frames"), Context.CapturedSourceFrames.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("environment_pcm_frames"), Context.CapturedEnvironmentFrames.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("environment_pcm_blocks"), Context.CapturedBlockCount.load(std::memory_order_acquire));
	Report->SetNumberField(TEXT("allocated_metadata_payload_bytes"),
		double(Context.CapturedSourceBlocks.Num() * sizeof(FIMAcousticBlockProbe)
			+ Context.CapturedBlocks.Num() * sizeof(FIMAcousticMetaSoundBlock)
			+ Context.CapturedBusReads.Num() * sizeof(FIMAcousticBusReadBlock)
			+ Context.CapturedSourceBlockReadyWordCount * sizeof(std::atomic<uint64>)));
	for (uint32 Index = 0; Index < FIMAcousticMetaSoundContext::MaxVoices; ++Index)
	{
		const uint64 AudioID = Context.AudioIds[Index].load(std::memory_order_acquire);
		if (!AudioID) continue;
		auto Slot = MakeShared<FJsonObject>();
		Slot->SetNumberField(TEXT("slot"), Index);
		Slot->SetStringField(TEXT("audio_component_id"), LexToString(AudioID));
		Slot->SetNumberField(TEXT("push_blocks"), double(Context.SourceSendBlockCounts[Index].load(std::memory_order_acquire)));
		Slot->SetBoolField(TEXT("consumer"), Context.SourceConsumers[Index].load(std::memory_order_acquire));
		Slot->SetBoolField(TEXT("active"), Context.SourceSendActive[Index].load(std::memory_order_acquire));
		Slot->SetNumberField(TEXT("queue_samples_at_export"), Context.SourceSendPatches[Index]
			? Context.SourceSendPatches[Index]->GetNumSamplesAvailable() : -1);
		Slots.Add(MakeShared<FJsonValueObject>(Slot));
	}
	TArray<TSharedPtr<FJsonValue>> PoolStates;
	for (const auto& Slot : Context.Pool->Slots)
		PoolStates.Add(MakeShared<FJsonValueNumber>(uint32(Slot.State.load(std::memory_order_acquire))));
	Report->SetArrayField(TEXT("pool_slot_states_at_export"), PoolStates);
	Report->SetArrayField(TEXT("source_slots"), Slots);
	Report->SetArrayField(TEXT("environment_rows"), EnvironmentRows);
	Report->SetArrayField(TEXT("bus_rows"), BusRows);
	Report->SetArrayField(TEXT("source_rows"), SourceRows);
	PrefixValid &= IMEnvironmentExportUpstream(State, Report);
	PrefixValid &= IMEnvironmentExportPCM(State, Report);
	IMEnvironmentExportInitialCause(State, Report);
	Report->SetBoolField(TEXT("full_native_prefix_valid"), PrefixValid);
	return PrefixValid;
}

void IMEnvironmentInitialized(const UWorld::FActorsInitializedParams& Params)
{
	if (!IMEnvironmentOwner || !Params.World || Params.World->WorldType != EWorldType::PIE) return;
	auto& State = *IMEnvironmentOwner;
	if (State.BoundWorld)
	{
		if (State.World.Get() != Params.World)
			IMEnvironmentFailure(State, TEXT("second_pie_world_rejected_no_cross_world_merge"));
		return;
	}
	TArray<AIMAcousticFootstepCharacter*> Actors;
	for (TActorIterator<AIMAcousticFootstepCharacter> Actor(Params.World); Actor; ++Actor) Actors.Add(*Actor);
	if (Actors.Num() != 1 || Actors[0]->HasActorBegunPlay() || !Actors[0]->FootstepSound
		|| (Actors[0]->FootstepSound->GetPathName() != IMStockSource
			&& !(State.CaptureNaturalAV && Actors[0]->FootstepSound->HasAnyFlags(RF_Transient)
				&& IMAcousticMetaSound::IsAcousticMetaSound(Actors[0]->FootstepSound))) || !Actors[0]->GetFootstepAudio())
	{
		IMEnvironmentFailure(State, TEXT("pre_beginplay_stock_source_or_character_boundary_mismatch"));
		IMEnvironmentReleaseCaptureSetting(State);
		return;
	}
	State.World = Params.World;
	State.Character = Actors[0];
	State.WorldPath = Params.World->GetPathName();
	State.ActorPath = Actors[0]->GetPathName();
	State.SourcePath = Actors[0]->FootstepSound->GetPathName();
	State.ComponentID = Actors[0]->GetFootstepAudio()->GetAudioComponentID();
	State.InitializedSeconds = FPlatformTime::Seconds();
	State.Device = Params.World->GetAudioDevice();
	State.BoundWorld = true;
	auto* Mixer = FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(Params.World);
	if (!State.Device.IsValid() || !Mixer || Mixer != State.Device.GetAudioDevice())
	{
		IMEnvironmentFailure(State, TEXT("actual_world_audio_device_unavailable_before_context"));
		IMEnvironmentReleaseCaptureSetting(State);
		return;
	}
	State.DeviceID = State.Device.GetDeviceID();
	State.SampleRate = Mixer->GetSampleRate();
	IMEnvironmentObserveListenerInitialization(State, 1);
	if (!IMAcousticMetaSound::FindAcousticMetaSoundContext(State.DeviceID))
	{
		FString Error;
		State.Prepared = IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(State.DeviceID, State.SampleRate, State.PreparationID, Error);
		if (!State.Prepared)
		{
			IMEnvironmentFailure(State, TEXT("actual_pre_context_preparation_failed"));
			UE_LOG(LogTemp, Error, TEXT("IMLogs R109_ENV preparation_failed device=%u reason=%s"), State.DeviceID, *Error);
		}
	}
	IMEnvironmentObserveContext(State);
}

void IMEnvironmentArm(const TArray<FString>& Args, UWorld* World)
{
	check(IsInGameThread());
	const bool CaptureNaturalAV = Args.Num() == 2 && (Args[0] == TEXT("OutputDir=AcousticNaturalC24_20261011T053352Z")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC25_20261011T063628Z")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC25_20261011T063628Z_R2")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC25_20261011T063628Z_R3")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC26_20261011T082612Z")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC26_20261011T082612Z_R2")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC27_20261011T101233Z")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC27_20261011T101233Z_R2")
		|| Args[0] == TEXT("OutputDir=AcousticNaturalC27_20261011T101233Z_R3")
		|| Args[0] == TEXT("OutputDir=AcousticListenerC29_20261011T135339Z")
		|| Args[0] == TEXT("OutputDir=AcousticListenerC30_20261011T155840Z")
		|| Args[0] == TEXT("OutputDir=AcousticOrientationC31_20261012T023600Z")
		|| Args[0] == TEXT("OutputDir=AcousticVisibleC32Native_20261016")
		|| Args[0] == TEXT("OutputDir=AcousticCrossC32_20261016"))
		&& Args[1] == TEXT("CapturePCM=1");
	const bool CaptureCoverageCause = Args.Num() == 2 && Args[0] == TEXT("OutputDir=EnvironmentR111Control_20261008")
		&& Args[1] == TEXT("CapturePCM=1");
	const bool CaptureVoiceMaster = Args.Num() == 2 && Args[0] == TEXT("OutputDir=EnvironmentR113Control_20261008")
		&& Args[1] == TEXT("CapturePCM=1");
	const bool CaptureQuietTail = CaptureVoiceMaster || (Args.Num() == 2 && Args[0] == TEXT("OutputDir=EnvironmentR112Control_20261008")
		&& Args[1] == TEXT("CapturePCM=1"));
	const bool CaptureCurrentWork = Args.Num() == 2 && Args[0] == TEXT("OutputDir=AcousticProduct_20261010_132653")
		&& Args[1] == TEXT("CapturePCM=1");
	const bool CapturePCM = CaptureNaturalAV || CaptureCurrentWork || CaptureCoverageCause || CaptureQuietTail || (Args.Num() == 2 && Args[0] == TEXT("OutputDir=EnvironmentR110Control_20261008")
		&& Args[1] == TEXT("CapturePCM=1"));
	if ((!Args.IsEmpty() && !CapturePCM) || IMEnvironmentOwner || !World || World->WorldType != EWorldType::Editor || !GEngine) return;
	const FString OutputDirectory = CaptureNaturalAV
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexRuntime"), Args[0].RightChop(10), TEXT("Environment")) : CaptureCurrentWork
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexRuntime/AcousticProduct_20261010_132653")) : CaptureVoiceMaster
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/EnvironmentR113Control_20261008")) : CaptureQuietTail
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/EnvironmentR112Control_20261008")) : CaptureCoverageCause
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/EnvironmentR111Control_20261008")) : CapturePCM
		? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/EnvironmentR110Control_20261008")) : IMEnvironmentOutput();
	const FString ReportPath = FPaths::Combine(OutputDirectory, TEXT("environment_report.json"));
	const FString StatusPath = FPaths::Combine(OutputDirectory, TEXT("environment_status.json"));
	if (IFileManager::Get().FileExists(*ReportPath) || IFileManager::Get().FileExists(*StatusPath)) return;
	for (const auto& WorldContext : GEngine->GetWorldContexts())
	{
		auto* ExistingWorld = WorldContext.World();
		if (!ExistingWorld) continue;
		if (ExistingWorld->WorldType == EWorldType::PIE) return;
		const auto ExistingDevice = ExistingWorld->GetAudioDevice();
		if (ExistingDevice.IsValid() && IMAcousticMetaSound::FindAcousticMetaSoundContext(ExistingDevice.GetDeviceID())) return;
	}
	if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true)) return;
	auto State = MakeUnique<FIMEnvironmentBoundaryOwner>();
	State->OutputDirectory = OutputDirectory;
	State->CapturePCM = CapturePCM;
	State->CaptureCoverageCause = CaptureCoverageCause;
	State->CaptureQuietTail = CaptureQuietTail;
	State->CaptureVoiceMaster = CaptureVoiceMaster;
	State->CaptureNaturalAV = CaptureNaturalAV;
	State->ArmedSeconds = FPlatformTime::Seconds();
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, IMTraceCapacity, IMTraceCapacity, CapturePCM, true, false);
	State->CaptureSettingOwned = true;
	IMCoverageFailure = FIMAcousticOrdinaryCoverageFailure();
	IMCoverageObservationState.store(CaptureCoverageCause ? 1u : 0u, std::memory_order_release);
	IMEnvironmentOwner = MoveTemp(State);
	IMEnvironmentOwner->Initialized = FWorldDelegates::OnWorldInitializedActors.AddStatic(&IMEnvironmentInitialized);
	if (CaptureCoverageCause)
	{
		IMEnvironmentOwner->PreActorTick = FWorldDelegates::OnWorldPreActorTick.AddLambda([](UWorld* TickWorld, ELevelTick, float)
		{
			if (IMEnvironmentOwner && IMEnvironmentOwner->World.Get() == TickWorld)
				IMEnvironmentObserveListenerInitialization(*IMEnvironmentOwner, 2);
		});
		IMEnvironmentOwner->PostActorTick = FWorldDelegates::OnWorldPostActorTick.AddLambda([](UWorld* TickWorld, ELevelTick, float)
		{
			if (IMEnvironmentOwner && IMEnvironmentOwner->World.Get() == TickWorld)
				IMEnvironmentObserveListenerInitialization(*IMEnvironmentOwner, 3);
		});
	}
	IMEnvironmentOwner->Cleanup = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* CleanWorld, bool, bool)
	{
		if (IMEnvironmentOwner && IMEnvironmentOwner->World.Get() == CleanWorld)
		{
			IMEnvironmentObserveCharacter(*IMEnvironmentOwner);
			IMEnvironmentOwner->Cleaned = true;
			IMEnvironmentOwner->CleanupSeconds = FPlatformTime::Seconds();
			if (!IMEnvironmentOwner->Context)
				IMEnvironmentFailure(*IMEnvironmentOwner, TEXT("world_ended_without_observed_native_context"));
		}
	});
	IMEnvironmentOwner->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		if (!IMEnvironmentOwner) return false;
		IMEnvironmentObserveContext(*IMEnvironmentOwner);
		IMEnvironmentObserveCharacter(*IMEnvironmentOwner);
		IMEnvironmentObserveListenerInitialization(*IMEnvironmentOwner, 4);
		IMEnvironmentSave(TEXT("environment_status.json"), IMEnvironmentStatus());
		return true;
	}), .1f);
	IMEnvironmentSave(TEXT("environment_status.json"), IMEnvironmentStatus());
}

void IMEnvironmentFinish(const TArray<FString>& Args)
{
	check(IsInGameThread());
	if (!Args.IsEmpty() || !IMEnvironmentOwner) return;
	auto& State = *IMEnvironmentOwner;
	State.Finishing = true;
	if (!IMEnvironmentClosed(State))
	{
		IMEnvironmentSave(TEXT("environment_status.json"), IMEnvironmentStatus());
		return;
	}
	const auto Counts = State.Context ? IMEnvironmentCounts(*State.Context) : FIMAcousticOrdinaryEnvironmentCaptureCounts();
	const double Now = FPlatformTime::Seconds();
	if (!State.HaveClosedSample || !(Counts == State.ClosedCounts))
	{
		State.HaveClosedSample = true;
		State.ClosedCounts = Counts;
		State.ClosedSampleSeconds = Now;
		return;
	}
	if (Now - State.ClosedSampleSeconds < .1) return;
	auto Report = IMEnvironmentStatus();
	Report->SetNumberField(TEXT("schema"), 1);
	Report->SetStringField(TEXT("work_id"), State.CaptureNaturalAV
		? ((State.OutputDirectory.Contains(TEXT("AcousticVisibleC32Native_20261016"))
			|| State.OutputDirectory.Contains(TEXT("AcousticCrossC32_20261016")))
			? TEXT("MON-AC-UNFINISHED-ROTATION-VISIBLE-AV-AND-INDEPENDENT-COST-C32-20261012-045022")
			: State.OutputDirectory.Contains(TEXT("AcousticOrientationC31_20261012T023600Z"))
			? TEXT("MON-AC-CURRENT-UNFINISHED-ORDINARY-ACCEPTANCE-CONTINUATION-C31-20261012-0236")
			: State.OutputDirectory.Contains(TEXT("AcousticListenerC30_20261011T155840Z"))
			? TEXT("MON-AC-REPAIRED-NUMERIC-ROTATION-LISTENER-IR-WET-REAL-VALIDATION-STAGE-C30-20261011-155840")
			: State.OutputDirectory.Contains(TEXT("AcousticListenerC29_20261011T135339Z"))
			? TEXT("MON-AC-LISTENER-CENTRED-IR-COEFFICIENT-TO-REAL-WET-RESPONSE-STAGE-C29-20261011-135339")
			: State.OutputDirectory.Contains(TEXT("AcousticNaturalC27_20261011T101233Z"))
			? TEXT("MON-AC-NORMAL-RENDERER-SAVED-STOCK-SOURCE-NATURAL-AUDIBLE-STAGE-C27-20261011-101233")
			: State.OutputDirectory.Contains(TEXT("AcousticNaturalC26_20261011T082612Z"))
			? TEXT("MON-AC-AUDIBLE-NOIR-SNAPSHOT-CAUSAL-CONTINUITY-AND-PLAY-STAGE-C26-20261011-081331")
			: State.OutputDirectory.Contains(TEXT("AcousticNaturalC25_20261011T063628Z"))
			? TEXT("MON-AC-NONBLOCKING-NATIVE-CAPTURE-AND-NATURAL-AUDIO-PLAY-STAGE-C25-20261011-060859")
			: TEXT("MON-AC-NATURAL-WAVEPLAYER-DSP-IDENTITY-AV-AND-PLAY-C24-20261011-050729")) : State.CaptureVoiceMaster
		? TEXT("MON-AC-NORMAL-EVENT-NATIVE-VOICE-MASTER-MAPPING-AND-PLAY-20261008-0543") : State.CaptureQuietTail
		? TEXT("MON-AC-SAVED-ORDINARY-PLAY-NATIVE-QUIET-TAIL-20261008-0516") : State.CaptureCoverageCause
		? TEXT("MON-AC-CURRENT-INITIAL-LISTENER-COVERAGE-CAUSE-ORDINARY-PLAY-20261008-0408") : State.CapturePCM
		? TEXT("MON-AC-GT-SNAPSHOT-WORKER-IR-READY-SELECT-CAUSAL-JOIN-ORDINARY-PLAY-20261008-0223")
		: TEXT("MON-AC-ORDINARY-STARTUP-IR-FULL-NOIR-BUS-COLD-PLAY-20261008-0110"));
	Report->SetBoolField(TEXT("consumer_release_and_worker_join_native_closure"), State.Context.IsValid());
	Report->SetBoolField(TEXT("closed_counts_unchanged"), true);
	Report->SetNumberField(TEXT("closed_counter_observation_seconds"), Now - State.ClosedSampleSeconds);
	Report->SetBoolField(TEXT("retained_context_is_observation_only_no_stop_or_reset"), true);
	const bool PrefixValid = IMEnvironmentExport(State, Report);
	bool PendingCancelled = false;
	if (State.Prepared && !State.PreparationConsumed)
		PendingCancelled = IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(State.PreparationID);
	Report->SetBoolField(TEXT("unconsumed_preparation_cancelled"), PendingCancelled);
	IMEnvironmentReleaseCaptureSetting(State);
	FTSTicker::GetCoreTicker().RemoveTicker(State.Ticker);
	FWorldDelegates::OnWorldInitializedActors.Remove(State.Initialized);
	FWorldDelegates::OnWorldCleanup.Remove(State.Cleanup);
	FWorldDelegates::OnWorldPreActorTick.Remove(State.PreActorTick);
	FWorldDelegates::OnWorldPostActorTick.Remove(State.PostActorTick);
	IMCoverageObservationState.store(0, std::memory_order_release);
	Report->SetBoolField(TEXT("own_first_coverage_observation_disabled"), true);
	State.Device = FAudioDeviceHandle();
	TWeakPtr<FIMAcousticMetaSoundContext, ESPMode::ThreadSafe> ReleasedContext = State.Context;
	State.Context.Reset();
	Report->SetBoolField(TEXT("context_destructor_observed_after_own_release"), !ReleasedContext.IsValid());
	Report->SetBoolField(TEXT("own_capture_setting_released"), !State.CaptureSettingOwned);
	Report->SetBoolField(TEXT("own_delegates_removed"), true);
	Report->SetBoolField(TEXT("own_device_reference_released"), !State.Device.IsValid());
	Report->SetBoolField(TEXT("own_context_reference_released"), !State.Context);
	Report->SetBoolField(TEXT("complete"), PrefixValid && State.Failure.IsEmpty());
	Report->SetBoolField(TEXT("product_accepted"), false);
	Report->SetBoolField(TEXT("preview_ready"), false);
	IMEnvironmentSave(TEXT("environment_report.json"), Report);
	const FString OutputDirectory = State.OutputDirectory;
	IMEnvironmentOwner.Reset();
	IMEnvironmentSave(TEXT("environment_status.json"), IMEnvironmentStatus(), OutputDirectory);
}

FAutoConsoleCommandWithWorldAndArgs IMEnvironmentArmCommand(TEXT("IM.Acoustic.OrdinaryEnvironment.Arm"),
	TEXT("Default-OFF private pre-Context capture; explicit owned leaf opt-in exports existing native graph PCM."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&IMEnvironmentArm));
FAutoConsoleCommand IMEnvironmentFinishCommand(TEXT("IM.Acoustic.OrdinaryEnvironment.Finish"),
	TEXT("Export private published prefixes only after native consumers and worker end."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&IMEnvironmentFinish));
}
#endif
