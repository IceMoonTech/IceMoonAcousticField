#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformTime.h"
#include "Dom/JsonObject.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealEdGlobals.h"
#include "UObject/Package.h"

namespace IMAcousticS4WorldTransitionTestPrivate
{
constexpr const TCHAR* IMS4WorldMap = TEXT("/IceMoonAcousticField/Tests/IM_W1Door");
constexpr double IMS4WorldTimeoutSeconds = 120.0;
constexpr double IMS4WorldPhaseTimeoutSeconds = 25.0;
constexpr double IMS4WorldTailTimeoutSeconds = 15.0;
constexpr double IMS4WorldReleaseQuietSeconds = 0.10;
constexpr uint32 IMS4WorldTailSilentFrames = 8u * FIMAcousticMetaSoundContext::Frames;
constexpr float IMS4WorldWetThreshold = 1.0e-9f;

struct FIMAcousticS4WorldIdentity
{
	int32 Cycle = 0;
	FString Map;
	FString SourceAsset = IMAcousticTestSupport::GraphSourceAssetPath;
	FString ContextPointer;
	uint32 DeviceId = 0;
	uint64 WorldGeneration = 0;
	uint64 AudioId = 0;
	uint64 VoiceGeneration = 0;
	uint64 SpatialVersion = 0;
	int32 Slot = INDEX_NONE;
	uint32 SourceTraceBegin = 0;
	uint32 SourceTraceEnd = 0;
	uint32 EnvironmentTraceBegin = 0;
	uint32 EnvironmentTraceEnd = 0;
	uint32 SourcePcmBegin = 0;
	uint32 SourcePcmEnd = 0;
	uint32 WetPcmBegin = 0;
	uint32 WetPcmEnd = 0;
	uint32 AcceptedBlocks = 0;
	uint32 PathBlocks = 0;
	uint32 OutputBlocks = 0;
	uint32 InputBlocks = 0;
	uint32 WetNonzeroFrames = 0;
	bool bContextReleased = false;
};

struct FIMAcousticS4WorldState
{
	FAutomationTestBase* Test = nullptr;
	FString RunId;
	FString EvidenceDirectory;
	FString OriginalMap;
	FString OriginalMapDiskPath;
	FString OriginalMapHashBefore;
	FString OriginalMapHashAfter;
	FString TestMapDiskPath;
	FString TestMapHashBefore;
	FString TestMapHashAfter;
	FString SourceDiskPath;
	FString SourceHashBefore;
	FString SourceHashAfter;
	FString BakeAssetPath;
	FString BakeAssetDiskPath;
	FString BakeAssetHashBefore;
	FString BakeAssetHashAfter;
	FString Failure;
	FString SourceBlocksCsv = TEXT("cycle,index,block,voice,callback_audio_id,result_audio_id,world_generation,voice_generation,spatial_version,sequence,consumed_seconds,routes,reject,input_delay_priming,input_energy,path_energy,output_energy\n");
	FString EnvironmentBlocksCsv = TEXT("cycle,index,frame,sequence,spatial_version,expected_spatial_version,seconds,listener_x,listener_y,listener_z,fresh,wet_nonzero_frames\n");
	FString ResultMessage;
	FString LastRejectDetail;
	float OriginalBackgroundVolume = 1.0f;
	bool bOriginalAllowBackgroundAudio = false;
	bool bGlobalsRestored = false;
	bool bMapRestored = false;
	bool bAssetsUnchanged = false;
	bool bPassed = true;
	bool bFinalized = false;
	bool bWorldMismatchRejected = false;
	bool bOldAudioMismatchRejected = false;
	bool bVoiceMismatchRejected = false;
	bool bOldAudioIdDistinct = false;
	bool bOldVoiceGenerationDistinct = false;
	FIMAcousticS4WorldIdentity First;
	FIMAcousticS4WorldIdentity Second;
	FIMAcousticMetaSoundContextPtr FirstContext;
	FIMAcousticMetaSoundContextPtr CurrentContext;
	uint32 FirstStopSourceFramesAtRelease = 0;
	uint32 FirstStopWetFramesAtRelease = 0;
	uint32 FirstStopTailEndFrame = 0;
	uint64 FirstStopLiveGeneration = 0;
	uint32 FirstSourceTraceAtRelease = 0;
	uint32 FirstEnvironmentTraceAtRelease = 0;
	uint64 FirstRenderedBlocksAtTeardown = 0;
	uint32 FirstSourceTraceAtTeardown = 0;
	uint32 FirstEnvironmentTraceAtTeardown = 0;
	uint32 FirstSourcePcmAtTeardown = 0;
	uint32 FirstWetPcmAtTeardown = 0;
	double FirstTeardownObservedAt = 0.0;
	uint64 FirstRenderedBlocksAtRelease = 0;
	uint64 SecondStopLiveGeneration = 0;
	uint64 SecondReplayVoiceGeneration = 0;
	uint32 SecondReplayTraceBegin = 0;
	uint32 SecondReplaySourcePcmBegin = 0;
	uint32 SecondReplayWetPcmBegin = 0;
	uint32 SecondReplayEnvironmentTraceBegin = 0;

	void AddFailure(const FString& Why)
	{
		bPassed = false;
		if (!Failure.IsEmpty()) Failure += TEXT(" | ");
		Failure += Why;
	}

	void RestoreGlobals()
	{
		if (bGlobalsRestored) return;
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = bOriginalAllowBackgroundAudio;
		bGlobalsRestored = true;
	}
};

struct FIMAcousticS4WorldStats
{
	uint32 Matching = 0;
	uint32 Accepted = 0;
	uint32 Path = 0;
	uint32 Output = 0;
	uint32 Input = 0;
	uint64 VoiceGeneration = 0;
	uint64 SpatialVersion = 0;
	bool bVoiceConsistent = true;
};

FString IMS4WorldHashFile(const FString& FilePath)
{
	return IFileManager::Get().FileSize(*FilePath) > 0
		? LexToString(FMD5Hash::HashFile(*FilePath)) : FString();
}

bool IMS4WorldSaveJson(const FString& FilePath, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	return FJsonSerializer::Serialize(Object, Writer)
		&& FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

UWorld* IMS4WorldPIE()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World()) return Context.World();
	}
	return nullptr;
}

int32 IMS4WorldFindSlot(const FIMAcousticMetaSoundContextPtr& Context, uint64 AudioId)
{
	if (!Context || !AudioId) return INDEX_NONE;
	for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
	{
		if (Context->AudioIds[Slot].load(std::memory_order_acquire) == AudioId) return int32(Slot);
	}
	return INDEX_NONE;
}

uint32 IMS4WorldSourceTraceCount(const FIMAcousticMetaSoundContextPtr& Context)
{
	return Context.IsValid()
		? FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire),
			uint32(Context->CapturedSourceBlocks.Num())) : 0;
}

uint32 IMS4WorldEnvironmentTraceCount(const FIMAcousticMetaSoundContextPtr& Context)
{
	return Context.IsValid()
		? FMath::Min<uint32>(Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire),
			uint32(Context->CapturedBlocks.Num())) : 0;
}

uint32 IMS4WorldSourcePcmFrames(const FIMAcousticMetaSoundContextPtr& Context)
{
	return Context.IsValid()
		? FMath::Min<uint32>(Context->CapturedSourceFrames.load(std::memory_order_acquire),
			uint32(Context->CapturedSource.Num())) : 0;
}

uint32 IMS4WorldWetPcmFrames(const FIMAcousticMetaSoundContextPtr& Context)
{
	return Context.IsValid()
		? FMath::Min<uint32>(Context->CapturedEnvironmentFrames.load(std::memory_order_acquire),
			uint32(Context->CapturedWet.Num() / 2)) : 0;
}

void IMS4WorldInspectSource(const FIMAcousticMetaSoundContextPtr& Context, uint32 Begin, uint32 End,
	uint64 AudioId, uint64 WorldGeneration, FIMAcousticS4WorldStats& Out)
{
	Out = {};
	if (!Context.IsValid()) return;
	const uint32 Limit = IMS4WorldSourceTraceCount(Context);
	const uint32 First = FMath::Min(Begin, Limit);
	const uint32 Last = FMath::Min(End, Limit);
	for (uint32 Index = First; Index < Last; ++Index)
	{
		const FIMAcousticBlockProbe& Block = Context->CapturedSourceBlocks[int32(Index)];
		if (Block.CallbackAudioComponentId != AudioId) continue;
		++Out.Matching;
		if (Block.InputEnergy > 1.0e-9) ++Out.Input;
		if (Block.Reject != EIMAcousticProbeReject::Accepted || Block.InputDelayPriming
			|| Block.ResultAudioComponentId != AudioId || Block.ResultWorldGeneration != WorldGeneration
			|| !Block.ResultVoiceGeneration || !Block.ResultSpatialVersion
			|| Block.ResultSpatialVersion != Block.OutputSpatialVersion || !Block.ResultSequence) continue;
		++Out.Accepted;
		if (Block.OutputEnergy > 1.0e-9) ++Out.Output;
		if ((Block.Routes & 2u) && Block.PathEnergy > 1.0e-9 && Block.OutputEnergy > 1.0e-9) ++Out.Path;
		if (!Out.VoiceGeneration) Out.VoiceGeneration = Block.ResultVoiceGeneration;
		else if (Out.VoiceGeneration != Block.ResultVoiceGeneration) Out.bVoiceConsistent = false;
		Out.SpatialVersion = Block.ResultSpatialVersion;
	}
}

bool IMS4WorldWetHasEnergy(const FIMAcousticMetaSoundContextPtr& Context, uint32 Begin, uint32 End,
	uint32* OutNonzeroFrames = nullptr, bool* OutFinite = nullptr)
{
	if (OutNonzeroFrames) *OutNonzeroFrames = 0;
	if (OutFinite) *OutFinite = true;
	if (!Context.IsValid()) return false;
	const uint32 First = FMath::Min(Begin, IMS4WorldWetPcmFrames(Context));
	const uint32 Last = FMath::Min(End, IMS4WorldWetPcmFrames(Context));
	uint32 Nonzero = 0;
	for (uint32 Frame = First; Frame < Last; ++Frame)
	{
		const float Left = Context->CapturedWet[int32(Frame * 2)];
		const float Right = Context->CapturedWet[int32(Frame * 2 + 1)];
		if (!FMath::IsFinite(Left) || !FMath::IsFinite(Right))
		{
			if (OutFinite) *OutFinite = false;
			continue;
		}
		if (FMath::Abs(Left) > IMS4WorldWetThreshold || FMath::Abs(Right) > IMS4WorldWetThreshold) ++Nonzero;
	}
	if (OutNonzeroFrames) *OutNonzeroFrames = Nonzero;
	return Nonzero > 0;
}

bool IMS4WorldSourcePcmHasEnergy(const FIMAcousticMetaSoundContextPtr& Context, uint32 Begin, uint32 End,
	bool& bFinite)
{
	bFinite = true;
	if (!Context.IsValid()) return false;
	const uint32 First = FMath::Min(Begin, IMS4WorldSourcePcmFrames(Context));
	const uint32 Last = FMath::Min(End, IMS4WorldSourcePcmFrames(Context));
	for (uint32 Frame = First; Frame < Last; ++Frame)
	{
		const float Sample = Context->CapturedSource[int32(Frame)];
		if (!FMath::IsFinite(Sample)) bFinite = false;
		else if (FMath::Abs(Sample) > IMS4WorldWetThreshold) return true;
	}
	return false;
}

bool IMS4WorldSavePcm(const FString& Directory, const TCHAR* Name, const TArray<float>& Samples,
	uint32 BeginFrame, uint32 EndFrame, uint32 Channels)
{
	if (EndFrame < BeginFrame || !Channels) return false;
	const uint64 BeginSample = uint64(BeginFrame) * Channels;
	const uint64 EndSample = uint64(EndFrame) * Channels;
	if (EndSample > uint64(Samples.Num())) return false;
	if (EndSample == BeginSample) return true;
	const TArrayView<const uint8> Bytes(
		reinterpret_cast<const uint8*>(Samples.GetData() + BeginSample),
		SIZE_T((EndSample - BeginSample) * sizeof(float)));
	return FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(Directory, Name));
}

bool IMS4WorldProbePassed(const FIMAcousticIdentityInjectionProbe& Probe, uint32 RequiredRejectBit,
	uint64 AudioId, uint64 WorldGeneration, uint64 VoiceGeneration)
{
	return Probe.Observed.load(std::memory_order_acquire)
		&& Probe.Rejected.load(std::memory_order_acquire)
		&& Probe.FiniteOutput.load(std::memory_order_acquire)
		&& (Probe.RejectDetail.load(std::memory_order_acquire) & RequiredRejectBit) == RequiredRejectBit
		&& Probe.RejectedDelta.load(std::memory_order_acquire) == 1
		&& Probe.RenderedDelta.load(std::memory_order_acquire) == 0
		&& Probe.ActualAudioId.load(std::memory_order_acquire) == AudioId
		&& Probe.ActualWorldGeneration.load(std::memory_order_acquire) == WorldGeneration
		&& Probe.ActualVoiceGeneration.load(std::memory_order_acquire) == VoiceGeneration;
}

void IMS4WorldResetProbe(FIMAcousticIdentityInjectionProbe& Probe)
{
	Probe.Observed.store(0, std::memory_order_relaxed);
	Probe.Rejected.store(0, std::memory_order_relaxed);
	Probe.FiniteOutput.store(0, std::memory_order_relaxed);
	Probe.RejectDetail.store(0, std::memory_order_relaxed);
	Probe.RenderedDelta.store(0, std::memory_order_relaxed);
	Probe.RejectedDelta.store(0, std::memory_order_relaxed);
	Probe.InjectedAudioId.store(0, std::memory_order_relaxed);
	Probe.InjectedWorldGeneration.store(0, std::memory_order_relaxed);
	Probe.InjectedVoiceGeneration.store(0, std::memory_order_relaxed);
	Probe.InjectedSequence.store(0, std::memory_order_relaxed);
	Probe.ActualAudioId.store(0, std::memory_order_relaxed);
	Probe.ActualWorldGeneration.store(0, std::memory_order_relaxed);
	Probe.ActualVoiceGeneration.store(0, std::memory_order_relaxed);
	Probe.ActualSpatialVersion.store(0, std::memory_order_relaxed);
}

void IMS4WorldAppendContextEvidence(const TSharedRef<FIMAcousticS4WorldState>& State,
	const FIMAcousticMetaSoundContextPtr& Context, const FIMAcousticS4WorldIdentity& Identity)
{
	const uint32 SourceEnd = FMath::Min(Identity.SourceTraceEnd, IMS4WorldSourceTraceCount(Context));
	for (uint32 Index = Identity.SourceTraceBegin; Index < SourceEnd; ++Index)
	{
		const FIMAcousticBlockProbe& B = Context->CapturedSourceBlocks[int32(Index)];
		State->SourceBlocksCsv += FString::Printf(
			TEXT("%d,%u,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%u,%u,%u,%.12g,%.12g,%.12g\n"),
			Identity.Cycle, Index, B.Block, B.Voice, B.CallbackAudioComponentId, B.ResultAudioComponentId,
			B.ResultWorldGeneration, B.ResultVoiceGeneration, B.ResultSpatialVersion, B.ResultSequence,
			B.ConsumedSeconds, B.Routes, uint32(B.Reject), uint32(B.InputDelayPriming),
			B.InputEnergy, B.PathEnergy, B.OutputEnergy);
	}
	const uint32 EnvironmentEnd = FMath::Min(Identity.EnvironmentTraceEnd, IMS4WorldEnvironmentTraceCount(Context));
	for (uint32 Index = Identity.EnvironmentTraceBegin; Index < EnvironmentEnd; ++Index)
	{
		const FIMAcousticMetaSoundBlock& B = Context->CapturedBlocks[int32(Index)];
		const uint32 Begin = uint32(B.Frame);
		const uint32 End = FMath::Min(Begin + FIMAcousticMetaSoundContext::Frames, IMS4WorldWetPcmFrames(Context));
		uint32 NonzeroFrames = 0;
		for (uint32 Frame = Begin; Frame < End; ++Frame)
		{
			const int32 Sample = int32(Frame * 2);
			if (FMath::Abs(Context->CapturedWet[Sample]) > IMS4WorldWetThreshold
				|| FMath::Abs(Context->CapturedWet[Sample + 1]) > IMS4WorldWetThreshold) ++NonzeroFrames;
		}
		State->EnvironmentBlocksCsv += FString::Printf(
			TEXT("%d,%u,%llu,%llu,%llu,%llu,%.9f,%.6f,%.6f,%.6f,%u,%u\n"),
			Identity.Cycle, Index, B.Frame, B.Sequence, B.SpatialVersion, B.ExpectedSpatialVersion,
			B.Seconds, B.ListenerX, B.ListenerY, B.ListenerZ, B.Fresh ? 1u : 0u, NonzeroFrames);
	}
}

void IMS4WorldWriteIdentity(TSharedRef<FJsonObject> Json, const TCHAR* Prefix,
	const FIMAcousticS4WorldIdentity& Identity)
{
	TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetNumberField(TEXT("cycle"), Identity.Cycle);
	Item->SetStringField(TEXT("map"), Identity.Map);
	Item->SetStringField(TEXT("source_asset"), Identity.SourceAsset);
	Item->SetStringField(TEXT("context_pointer"), Identity.ContextPointer);
	Item->SetNumberField(TEXT("device_id"), Identity.DeviceId);
	Item->SetStringField(TEXT("world_generation"), LexToString(Identity.WorldGeneration));
	Item->SetStringField(TEXT("audio_component_id"), LexToString(Identity.AudioId));
	Item->SetStringField(TEXT("voice_generation"), LexToString(Identity.VoiceGeneration));
	Item->SetStringField(TEXT("spatial_version"), LexToString(Identity.SpatialVersion));
	Item->SetNumberField(TEXT("voice_slot"), Identity.Slot);
	Item->SetNumberField(TEXT("source_trace_begin"), Identity.SourceTraceBegin);
	Item->SetNumberField(TEXT("source_trace_end"), Identity.SourceTraceEnd);
	Item->SetNumberField(TEXT("environment_trace_begin"), Identity.EnvironmentTraceBegin);
	Item->SetNumberField(TEXT("environment_trace_end"), Identity.EnvironmentTraceEnd);
	Item->SetNumberField(TEXT("source_pcm_begin_frame"), Identity.SourcePcmBegin);
	Item->SetNumberField(TEXT("source_pcm_end_frame"), Identity.SourcePcmEnd);
	Item->SetNumberField(TEXT("wet_pcm_begin_frame"), Identity.WetPcmBegin);
	Item->SetNumberField(TEXT("wet_pcm_end_frame"), Identity.WetPcmEnd);
	Item->SetNumberField(TEXT("accepted_blocks"), Identity.AcceptedBlocks);
	Item->SetNumberField(TEXT("path_nonzero_blocks"), Identity.PathBlocks);
	Item->SetNumberField(TEXT("output_nonzero_blocks"), Identity.OutputBlocks);
	Item->SetNumberField(TEXT("input_nonzero_blocks"), Identity.InputBlocks);
	Item->SetNumberField(TEXT("wet_nonzero_frames"), Identity.WetNonzeroFrames);
	Item->SetBoolField(TEXT("context_released"), Identity.bContextReleased);
	Json->SetObjectField(Prefix, Item);
}

class FIMAcousticS4WorldTransitionCommand final : public IAutomationLatentCommand
{
public:
	FIMAcousticS4WorldTransitionCommand(TSharedRef<FIMAcousticS4WorldState> InState, int32 InCycle)
		: State(InState), StartedAt(FPlatformTime::Seconds()), StageAt(StartedAt), Cycle(InCycle) {}

	bool Update() override
	{
		check(IsInGameThread());
		const double Now = FPlatformTime::Seconds();
		if (Stage != 90 && Now - StartedAt > IMS4WorldTimeoutSeconds)
			return BeginFailure(TEXT("World-transition automation exceeded its 120 second cap."));

		if (Stage == 90) return CleanupAndRestore(Now);
		if (Stage == 12) return WaitFirstWorldEnd(Now);
		if (Stage == 13) return VerifyFirstWorldRelease(Now);
		if (Stage == 14) return WaitSecondWorldEnd(Now);
		if (Stage == 15) return VerifySecondWorldReleaseAndRestore(Now);

		UWorld* World = IMS4WorldPIE();
		if (!World)
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Expected PIE world did not become available."));
			return false;
		}
		if (Stage == 0)
		{
			if (!SetupWorld(World, Now)) return false;
			Stage = 1;
			StageAt = Now;
			return false;
		}
		if (!SourceAudio.IsValid() || (Stage != 1 && !CurrentContext.IsValid()))
			return BeginFailure(TEXT("Ordinary MetaSound source or its context disappeared during the active window."));

		if (Stage == 1) return WaitForBaseline(Now);
		if (Stage == 2 || Stage == 6) return WaitForStoppedTail(Now);
		if (Stage == 3) return WaitForOldWorldReject(Now);
		if (Stage == 4) return WaitForFreshAfterWorldReject(Now);
		if (Stage == 5) return WaitForOldAudioReject(Now);
		if (Stage == 7) return WaitForFreshAfterAudioReject(Now);
		if (Stage == 8) return WaitForReplayFreshGeneration(Now);
		if (Stage == 9) return WaitForOldVoiceReject(Now);
		if (Stage == 10) return WaitForFreshAfterVoiceReject(Now);
		return BeginFailure(TEXT("World-transition command reached an unknown stage."));
	}

private:
	bool SetupWorld(UWorld* World, double Now)
	{
		const FString Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
		if (Map != IMS4WorldMap) return BeginFailure(TEXT("PIE opened a map other than the read-only W1 doorway fixture."));

		AIMAcousticBakeVolume* FoundVolume = nullptr;
		int32 VolumeCount = 0;
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
		{
			FoundVolume = *It;
			++VolumeCount;
		}
		if (VolumeCount != 1 || !FoundVolume || !FoundVolume->BakedField)
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("W1 PIE must contain exactly one already-baked acoustic volume; no bake or save is attempted."));
			return false;
		}

		APlayerController* FoundListener = World->GetFirstPlayerController();
		FAudioDevice* AudioDevice = World->GetAudioDeviceRaw();
		if (!FoundListener || !AudioDevice)
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("PIE listener or audio device did not initialize."));
			return false;
		}
		Volume = FoundVolume;
		Listener = FoundListener;
		Volume->bEnableV2 = true;
		Volume->bDirectRoute = false;
		Volume->bPathRoute = true;
		Volume->bReverbRoute = true;
		Listener->SetAudioListenerOverride(nullptr, FVector(0.0, 200.0, 150.0), FRotator(0.0, -90.0, 0.0));

		if (Cycle == 1)
		{
			const FString BakePackage = FoundVolume->BakedField->GetOutermost()->GetName();
			if (FoundVolume->BakedField->GetOutermost()->IsDirty())
				return BeginFailure(TEXT("Referenced bake asset is dirty; refusing to proceed."));
			State->BakeAssetPath = FoundVolume->BakedField->GetPathName();
			State->BakeAssetDiskPath = FPackageName::LongPackageNameToFilename(
				BakePackage, FPackageName::GetAssetPackageExtension());
			State->BakeAssetHashBefore = IMS4WorldHashFile(State->BakeAssetDiskPath);
			if (State->BakeAssetHashBefore.IsEmpty())
				return BeginFailure(TEXT("Could not fingerprint the read-only bake asset."));
		}

		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor) return BeginFailure(TEXT("Could not spawn the transient PIE source actor."));
		Actor->SetActorLocation(FVector(0.0, -200.0, 150.0));
		UAudioComponent* Audio = NewObject<UAudioComponent>(Actor);
		if (!Audio)
		{
			Actor->Destroy();
			return BeginFailure(TEXT("Could not create the transient ordinary AudioComponent."));
		}
		Actor->SetRootComponent(Audio);
		Actor->AddInstanceComponent(Audio);
		Audio->bAutoActivate = false;
		Audio->RegisterComponent();
		Audio->SetWorldLocation(FVector(0.0, -200.0, 150.0));
		UIMAcousticSourceComponent* Marker = NewObject<UIMAcousticSourceComponent>(Actor);
		if (!Marker)
		{
			Actor->Destroy();
			return BeginFailure(TEXT("Could not create the transient IceMoon source marker."));
		}
		Actor->AddInstanceComponent(Marker);
		Marker->AudioComponent = Audio;
		Marker->RegisterComponent();
		FString Error;
		if (!IMAcousticTestSupport::ConfigureGraphSource(Audio, Error) || !Marker->ValidateSource(Error))
		{
			Actor->Destroy();
			return BeginFailure(Error.IsEmpty() ? TEXT("Ordinary MetaSound source validation failed.") : Error);
		}
		SourceActor = Actor;
		SourceAudio = Audio;
		Audio->Play();
		StageAt = Now;
		return true;
	}

	bool BindContextAfterSourcePlay(double Now)
	{
		UWorld* World = IMS4WorldPIE();
		FAudioDevice* AudioDevice = World ? World->GetAudioDeviceRaw() : nullptr;
		FIMAcousticMetaSoundContextPtr NewContext = AudioDevice
			? IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID) : nullptr;
		if (!NewContext.IsValid() || !NewContext->Pool.IsValid() || !NewContext->Device.IsValid())
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Ordinary MetaSound environment context did not initialize after the graph source began playing."));
			return false;
		}
		if (NewContext->Stopped.load(std::memory_order_acquire)
			|| !NewContext->Device->Alive.load(std::memory_order_acquire)
			|| NewContext->Pool->Stopped.load(std::memory_order_acquire))
			return BeginFailure(TEXT("New PIE MetaSound context is already stopped or not alive."));
		if (Cycle == 2)
		{
			if (!State->FirstContext.IsValid() || NewContext.Get() == State->FirstContext.Get())
				return BeginFailure(TEXT("Second PIE reused the retained old MetaSound context pointer."));
			if (NewContext->Pool->WorldGeneration == State->First.WorldGeneration)
				return BeginFailure(TEXT("Second PIE reused the previous WorldGeneration."));
		}

		CurrentContext = NewContext;
		State->CurrentContext = NewContext;
		FIMAcousticS4WorldIdentity& Identity = Cycle == 1 ? State->First : State->Second;
		Identity = {};
		Identity.Cycle = Cycle;
		Identity.Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
		Identity.ContextPointer = FString::Printf(TEXT("%p"), static_cast<void*>(NewContext.Get()));
		Identity.DeviceId = AudioDevice->DeviceID;
		Identity.WorldGeneration = NewContext->Pool->WorldGeneration;
		Identity.SourceTraceBegin = IMS4WorldSourceTraceCount(NewContext);
		Identity.EnvironmentTraceBegin = IMS4WorldEnvironmentTraceCount(NewContext);
		Identity.SourcePcmBegin = IMS4WorldSourcePcmFrames(NewContext);
		Identity.WetPcmBegin = IMS4WorldWetPcmFrames(NewContext);
		return true;
	}

	bool WaitForBaseline(double Now)
	{
		if (!CurrentContext.IsValid() && !BindContextAfterSourcePlay(Now)) return false;
		FIMAcousticS4WorldIdentity& Identity = Cycle == 1 ? State->First : State->Second;
		Identity.AudioId = SourceAudio->GetAudioComponentID();
		if (!Identity.AudioId)
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Ordinary AudioComponent did not publish a nonzero AudioComponentID."));
			return false;
		}
		Identity.Slot = IMS4WorldFindSlot(CurrentContext, Identity.AudioId);
		if (Identity.Slot == INDEX_NONE)
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Ordinary MetaSound source did not acquire a voice slot."));
			return false;
		}
		if (Identity.Slot != 0)
			return BeginFailure(TEXT("The W1 fixture source did not acquire slot zero required by the bounded source PCM capture."));

		FIMAcousticS4WorldStats Stats;
		IMS4WorldInspectSource(CurrentContext, Identity.SourceTraceBegin,
			IMS4WorldSourceTraceCount(CurrentContext), Identity.AudioId, Identity.WorldGeneration, Stats);
		uint32 WetNonzero = 0;
		bool bWetFinite = true;
		const bool bWet = IMS4WorldWetHasEnergy(CurrentContext, Identity.WetPcmBegin,
			IMS4WorldWetPcmFrames(CurrentContext), &WetNonzero, &bWetFinite);
		bool bSourceFinite = true;
		const bool bSourcePcm = IMS4WorldSourcePcmHasEnergy(CurrentContext, Identity.SourcePcmBegin,
			IMS4WorldSourcePcmFrames(CurrentContext), bSourceFinite);
		if (!bWetFinite || !bSourceFinite)
			return BeginFailure(TEXT("Captured ordinary MetaSound source or wet PCM contains non-finite samples."));
		if (!SourceAudio->IsPlaying() || Stats.Accepted < 8 || Stats.Path < 8 || Stats.Output < 8
			|| Stats.Input == 0 || !bWet || !bSourcePcm
			|| !CurrentContext->EnvironmentConsumer.load(std::memory_order_acquire)
			|| CurrentContext->Device->ReverbEffectInstances.load(std::memory_order_acquire) <= 0)
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(FString::Printf(
					TEXT("Ordinary source/environment baseline timed out: playing=%d accepted=%u path=%u output=%u input=%u source_pcm=%d wet=%d wet_frames=%u env_consumer=%d reverb_instances=%d."),
					SourceAudio->IsPlaying() ? 1 : 0, Stats.Accepted, Stats.Path, Stats.Output, Stats.Input,
					bSourcePcm ? 1 : 0, bWet ? 1 : 0, WetNonzero,
					CurrentContext->EnvironmentConsumer.load(std::memory_order_acquire) ? 1 : 0,
					CurrentContext->Device->ReverbEffectInstances.load(std::memory_order_acquire)));
			return false;
		}
		Identity.VoiceGeneration = Stats.VoiceGeneration;
		Identity.SpatialVersion = Stats.SpatialVersion;
		Identity.AcceptedBlocks = Stats.Accepted;
		Identity.PathBlocks = Stats.Path;
		Identity.OutputBlocks = Stats.Output;
		Identity.InputBlocks = Stats.Input;
		Identity.WetNonzeroFrames = WetNonzero;
		Identity.SourcePcmEnd = IMS4WorldSourcePcmFrames(CurrentContext);
		Identity.WetPcmEnd = IMS4WorldWetPcmFrames(CurrentContext);
		Identity.SourceTraceEnd = IMS4WorldSourceTraceCount(CurrentContext);
		Identity.EnvironmentTraceEnd = IMS4WorldEnvironmentTraceCount(CurrentContext);

		if (Cycle == 1)
		{
			State->FirstContext = CurrentContext;
			BeginStopAndTail(Now, 2);
			return false;
		}
		if (Identity.AudioId == State->First.AudioId)
		{
			State->bOldAudioIdDistinct = false;
		}
		else
		{
			State->bOldAudioIdDistinct = true;
		}
		State->bOldVoiceGenerationDistinct = Identity.VoiceGeneration != State->First.VoiceGeneration;
		RequestIdentity(3, Identity.AudioId, State->First.WorldGeneration,
			Identity.VoiceGeneration, Identity.SpatialVersion, 930101, 2);
		Stage = 3;
		StageAt = Now;
		return false;
	}

	void RequestIdentity(uint32 Kind, uint64 AudioId, uint64 WorldGeneration,
		uint64 VoiceGeneration, uint64 SpatialVersion, uint64 Sequence, int32 ProbeIndex)
	{
		IMS4WorldResetProbe(CurrentContext->IdentityInjection.Probes[ProbeIndex]);
		CurrentContext->IdentityInjection.Request(Kind, AudioId, WorldGeneration,
			VoiceGeneration, Sequence, 0.0f, 0.0f, 0.0f, SpatialVersion);
		ProbeTraceBegin = IMS4WorldSourceTraceCount(CurrentContext);
		ActiveProbeIndex = ProbeIndex;
		ActiveProbeRejectBit = Kind == 1 ? 128u : (VoiceGeneration != State->Second.VoiceGeneration ? 64u : 32u);
	}

	bool WaitForOldWorldReject(double Now)
	{
		const auto& Probe = CurrentContext->IdentityInjection.Probes[2];
		if (!Probe.Observed.load(std::memory_order_acquire))
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Old WorldGeneration negative control was not consumed by the source operator."));
			return false;
		}
		State->LastRejectDetail = LexToString(Probe.RejectDetail.load(std::memory_order_acquire));
		if (!IMS4WorldProbePassed(Probe, 32u, State->Second.AudioId,
			State->Second.WorldGeneration, State->Second.VoiceGeneration)
			|| Probe.InjectedAudioId.load(std::memory_order_acquire) != State->Second.AudioId
			|| Probe.InjectedWorldGeneration.load(std::memory_order_acquire) != State->First.WorldGeneration
			|| Probe.InjectedVoiceGeneration.load(std::memory_order_acquire) != State->Second.VoiceGeneration)
			return BeginFailure(TEXT("Old WorldGeneration was not rejected with current AudioId/VoiceGeneration and no render publication."));
		State->bWorldMismatchRejected = true;
		Stage = 4;
		StageAt = Now;
		return false;
	}

	bool WaitForFreshAfterWorldReject(double Now)
	{
		if (!HasFreshCurrentSourceBlock(ProbeTraceBegin, State->Second))
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Current source identity did not recover after the old-world rejection."));
			return false;
		}
		if (State->bOldAudioIdDistinct)
		{
			RequestIdentity(1, State->First.AudioId, State->Second.WorldGeneration,
				State->Second.VoiceGeneration, State->Second.SpatialVersion, 930102, 0);
			Stage = 5;
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS4WorldTransition old_audio_id_axis=not_distinguishable old_id=%llu new_id=%llu; no forced reuse was attempted."),
				State->First.AudioId, State->Second.AudioId);
			BeginSecondStop(Now);
		}
		StageAt = Now;
		return false;
	}

	bool WaitForOldAudioReject(double Now)
	{
		const auto& Probe = CurrentContext->IdentityInjection.Probes[0];
		if (!Probe.Observed.load(std::memory_order_acquire))
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Old AudioComponentID negative control was not consumed by the source operator."));
			return false;
		}
		State->LastRejectDetail = LexToString(Probe.RejectDetail.load(std::memory_order_acquire));
		if (!IMS4WorldProbePassed(Probe, 128u, State->Second.AudioId,
			State->Second.WorldGeneration, State->Second.VoiceGeneration)
			|| Probe.InjectedAudioId.load(std::memory_order_acquire) != State->First.AudioId
			|| Probe.InjectedWorldGeneration.load(std::memory_order_acquire) != State->Second.WorldGeneration
			|| Probe.InjectedVoiceGeneration.load(std::memory_order_acquire) != State->Second.VoiceGeneration)
			return BeginFailure(TEXT("Old AudioComponentID was not rejected independently of current WorldGeneration/VoiceGeneration."));
		State->bOldAudioMismatchRejected = true;
		Stage = 7;
		StageAt = Now;
		return false;
	}

	bool WaitForFreshAfterAudioReject(double Now)
	{
		if (!HasFreshCurrentSourceBlock(ProbeTraceBegin, State->Second))
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Current source identity did not recover after the old-audio rejection."));
			return false;
		}
		BeginSecondStop(Now);
		return false;
	}

	void BeginSecondStop(double Now)
	{
		BeginStopAndTail(Now, 6);
	}

	void BeginStopAndTail(double Now, int32 NextStage)
	{
		if (!SourceAudio.IsValid() || !CurrentContext.IsValid())
		{
			BeginFailure(TEXT("Cannot begin stop/tail phase without the active source and context."));
			return;
		}
		StopRequestSeconds = Now;
		StopTraceBegin = IMS4WorldSourceTraceCount(CurrentContext);
		StopPcmFramesAtRequest = IMS4WorldSourcePcmFrames(CurrentContext);
		StopWetFrameBegin = IMS4WorldWetPcmFrames(CurrentContext);
		TailScanFrame = StopWetFrameBegin;
		TailSilentRunFrames = 0;
		TailSawWetAfterRelease = false;
		TailPcmFinite = true;
		StopSourceReleased = false;
		StoppedSourceBlockObserved = false;
		StopDryPcmAdvanced = false;
		StopEnvironmentAlive = true;
		SourceTraceScan = StopTraceBegin;
		SourceAudio->Stop();
		Stage = NextStage;
		StageAt = Now;
	}

	void ScanStoppedWet(double Now)
	{
		const uint32 EndFrame = IMS4WorldWetPcmFrames(CurrentContext);
		const uint32 FirstFrame = FMath::Min(TailScanFrame, EndFrame);
		for (uint32 Frame = FirstFrame; Frame < EndFrame; ++Frame)
		{
			const float Left = CurrentContext->CapturedWet[int32(Frame * 2)];
			const float Right = CurrentContext->CapturedWet[int32(Frame * 2 + 1)];
			if (!FMath::IsFinite(Left) || !FMath::IsFinite(Right))
			{
				TailPcmFinite = false;
				continue;
			}
			const bool bNonzero = FMath::Abs(Left) > IMS4WorldWetThreshold
				|| FMath::Abs(Right) > IMS4WorldWetThreshold;
			if (StopSourceReleased && Frame >= WetFramesAtRelease && bNonzero)
				TailSawWetAfterRelease = true;
			if (bNonzero) TailSilentRunFrames = 0;
			else if (StopSourceReleased && Frame >= WetFramesAtRelease) ++TailSilentRunFrames;
		}
		TailScanFrame = EndFrame;

		const uint32 TraceEnd = IMS4WorldSourceTraceCount(CurrentContext);
		const uint32 TraceFirst = FMath::Min(SourceTraceScan, TraceEnd);
		for (uint32 Index = TraceFirst; Index < TraceEnd; ++Index)
		{
			if (!StopSourceReleased) break;
			const FIMAcousticBlockProbe& Block = CurrentContext->CapturedSourceBlocks[int32(Index)];
			if (Block.CallbackAudioComponentId == (Cycle == 1 ? State->First.AudioId : State->Second.AudioId))
				StoppedSourceBlockObserved = true;
		}
		SourceTraceScan = TraceEnd;

		if (!StopSourceReleased
			&& !CurrentContext->SourceConsumers[Cycle == 1 ? State->First.Slot : State->Second.Slot].load(std::memory_order_acquire))
		{
			StopSourceReleased = true;
			SourceReleaseSeconds = Now;
			SourcePcmFramesAtRelease = IMS4WorldSourcePcmFrames(CurrentContext);
			SourceTraceAtRelease = IMS4WorldSourceTraceCount(CurrentContext);
			EnvironmentTraceAtRelease = IMS4WorldEnvironmentTraceCount(CurrentContext);
			WetFramesAtRelease = IMS4WorldWetPcmFrames(CurrentContext);
			StopLiveGeneration = CurrentContext->Device->Voices[Cycle == 1 ? State->First.Slot : State->Second.Slot]
				->LiveGeneration.load(std::memory_order_acquire);
			if (!CurrentContext->EnvironmentConsumer.load(std::memory_order_acquire)
				|| CurrentContext->Device->ReverbEffectInstances.load(std::memory_order_acquire) <= 0)
				StopEnvironmentAlive = false;
		}
		if (StopSourceReleased && IMS4WorldSourcePcmFrames(CurrentContext) != SourcePcmFramesAtRelease)
			StopDryPcmAdvanced = true;
		if (StopSourceReleased && (IMS4WorldSourceTraceCount(CurrentContext) > SourceTraceAtRelease)
			&& StoppedSourceBlockObserved)
			StopDryPcmAdvanced = true;
	}

	bool WaitForStoppedTail(double Now)
	{
		ScanStoppedWet(Now);
		if (!TailPcmFinite) return BeginFailure(TEXT("Stop-tail wet PCM contains non-finite samples."));
		if (StopDryPcmAdvanced) return BeginFailure(TEXT("Source PCM or source trace advanced after source consumer release."));
		if (Now - StopRequestSeconds > IMS4WorldTailTimeoutSeconds)
			return BeginFailure(TEXT("Stopped source wet tail did not reach a stable silent PCM window."));
		if (!StopSourceReleased || !SourceAudio.IsValid() || SourceAudio->IsPlaying()
			|| !TailSawWetAfterRelease || TailSilentRunFrames < IMS4WorldTailSilentFrames
			|| Now - SourceReleaseSeconds < IMS4WorldReleaseQuietSeconds)
			return false;
		if (!StopEnvironmentAlive)
			return BeginFailure(TEXT("Environment consumer or reverb effect released before the wet tail finished."));
		if (!CurrentContext->EnvironmentConsumer.load(std::memory_order_acquire)
			|| CurrentContext->Device->ReverbEffectInstances.load(std::memory_order_acquire) <= 0)
			return BeginFailure(TEXT("Environment consumer or reverb effect did not remain alive through the complete wet tail."));
		if (StopLiveGeneration <= (Cycle == 1 ? State->First.VoiceGeneration : State->Second.VoiceGeneration))
			return BeginFailure(TEXT("Stop did not invalidate the previous voice generation."));
		uint32 TailWetFrames = 0;
		bool bFinite = true;
		if (!IMS4WorldWetHasEnergy(CurrentContext, WetFramesAtRelease, TailScanFrame, &TailWetFrames, &bFinite)
			|| !bFinite || !TailWetFrames)
			return BeginFailure(TEXT("No finite nonzero environment wet PCM was captured after source-consumer release."));

		FIMAcousticS4WorldIdentity& Identity = Cycle == 1 ? State->First : State->Second;
		Identity.SourceTraceEnd = IMS4WorldSourceTraceCount(CurrentContext);
		Identity.EnvironmentTraceEnd = IMS4WorldEnvironmentTraceCount(CurrentContext);
		Identity.SourcePcmEnd = IMS4WorldSourcePcmFrames(CurrentContext);
		Identity.WetPcmEnd = TailScanFrame;
		Identity.WetNonzeroFrames = TailWetFrames;
		if (Cycle == 1)
		{
			State->FirstStopSourceFramesAtRelease = SourcePcmFramesAtRelease;
			State->FirstStopWetFramesAtRelease = WetFramesAtRelease;
			State->FirstStopTailEndFrame = TailScanFrame;
			State->FirstStopLiveGeneration = StopLiveGeneration;
			if (!IMS4WorldSavePcm(State->EvidenceDirectory, TEXT("world1-source-mono.f32le"),
				CurrentContext->CapturedSource, Identity.SourcePcmBegin, Identity.SourcePcmEnd, 1)
				|| !IMS4WorldSavePcm(State->EvidenceDirectory, TEXT("world1-post-release-wet-stereo.f32le"),
					CurrentContext->CapturedWet, WetFramesAtRelease, TailScanFrame, 2))
				return BeginFailure(TEXT("Could not save world-1 source/wet PCM evidence."));
			GEditor->RequestEndPlayMap();
			Stage = 12;
			StageAt = Now;
			return false;
		}

		State->SecondStopLiveGeneration = StopLiveGeneration;
		State->SecondReplayTraceBegin = IMS4WorldSourceTraceCount(CurrentContext);
		State->SecondReplayEnvironmentTraceBegin = IMS4WorldEnvironmentTraceCount(CurrentContext);
		State->SecondReplaySourcePcmBegin = IMS4WorldSourcePcmFrames(CurrentContext);
		State->SecondReplayWetPcmBegin = IMS4WorldWetPcmFrames(CurrentContext);
		SourceAudio->Play();
		Stage = 8;
		StageAt = Now;
		return false;
	}

	bool WaitForOldVoiceReject(double Now)
	{
		const auto& Probe = CurrentContext->IdentityInjection.Probes[2];
		if (!Probe.Observed.load(std::memory_order_acquire))
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Old VoiceGeneration negative control was not consumed by the source operator."));
			return false;
		}
		State->LastRejectDetail = LexToString(Probe.RejectDetail.load(std::memory_order_acquire));
		if (!IMS4WorldProbePassed(Probe, 64u, State->Second.AudioId,
			State->Second.WorldGeneration, State->SecondReplayVoiceGeneration)
			|| Probe.InjectedAudioId.load(std::memory_order_acquire) != State->Second.AudioId
			|| Probe.InjectedWorldGeneration.load(std::memory_order_acquire) != State->Second.WorldGeneration
			|| Probe.InjectedVoiceGeneration.load(std::memory_order_acquire) != State->First.VoiceGeneration)
			return BeginFailure(TEXT("Old VoiceGeneration was not rejected independently of current WorldGeneration/AudioId."));
		State->bVoiceMismatchRejected = true;
		Stage = 10;
		StageAt = Now;
		return false;
	}

	bool WaitForFreshAfterVoiceReject(double Now)
	{
		if (!HasFreshCurrentSourceBlock(ProbeTraceBegin, State->Second))
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Current source identity did not recover after the old-voice rejection."));
			return false;
		}
		GEditor->RequestEndPlayMap();
		Stage = 14;
		StageAt = Now;
		return false;
	}

	bool WaitForReplayFreshGeneration(double Now)
	{
		if (SourceAudio->GetAudioComponentID() != State->Second.AudioId
			|| CurrentContext->Pool->WorldGeneration != State->Second.WorldGeneration)
			return BeginFailure(TEXT("Same-component replay changed AudioComponentID or WorldGeneration."));
		FIMAcousticS4WorldStats Stats;
		IMS4WorldInspectSource(CurrentContext, State->SecondReplayTraceBegin,
			IMS4WorldSourceTraceCount(CurrentContext), State->Second.AudioId, State->Second.WorldGeneration, Stats);
		uint32 WetNonzero = 0;
		bool bFinite = true;
		const bool bWet = IMS4WorldWetHasEnergy(CurrentContext, State->SecondReplayWetPcmBegin,
			IMS4WorldWetPcmFrames(CurrentContext), &WetNonzero, &bFinite);
		if (!bFinite) return BeginFailure(TEXT("World-2 replay wet PCM contains non-finite samples."));
		bool bSourceFinite = true;
		const bool bSourcePcm = IMS4WorldSourcePcmHasEnergy(CurrentContext, State->SecondReplaySourcePcmBegin,
			IMS4WorldSourcePcmFrames(CurrentContext), bSourceFinite);
		if (!bSourceFinite) return BeginFailure(TEXT("World-2 replay source PCM contains non-finite samples."));
		if (!SourceAudio->IsPlaying() || Stats.Accepted < 8 || Stats.Path < 8 || Stats.Output < 8
			|| Stats.Input == 0 || !Stats.VoiceGeneration || !bWet || !bSourcePcm)
		{
			if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
				return BeginFailure(TEXT("Same-component fresh-generation replay did not produce accepted path and finite wet/source PCM."));
			return false;
		}
		if (!Stats.bVoiceConsistent || Stats.VoiceGeneration <= State->SecondStopLiveGeneration
			|| Stats.VoiceGeneration <= State->First.VoiceGeneration)
			return BeginFailure(TEXT("Replay did not advance beyond stop and the previous world's VoiceGeneration."));
		State->SecondReplayVoiceGeneration = Stats.VoiceGeneration;
		State->bOldVoiceGenerationDistinct = State->First.VoiceGeneration != Stats.VoiceGeneration;
		if (!State->bOldVoiceGenerationDistinct)
			return BeginFailure(TEXT("A normal stop/replay did not make the prior VoiceGeneration independently distinguishable."));
		State->Second.VoiceGeneration = Stats.VoiceGeneration;
		State->Second.AcceptedBlocks += Stats.Accepted;
		State->Second.PathBlocks += Stats.Path;
		State->Second.OutputBlocks += Stats.Output;
		State->Second.InputBlocks += Stats.Input;
		State->Second.SpatialVersion = Stats.SpatialVersion;
		State->Second.WetNonzeroFrames += WetNonzero;
		RequestIdentity(3, State->Second.AudioId, State->Second.WorldGeneration,
			State->First.VoiceGeneration, Stats.SpatialVersion, 930103, 2);
		Stage = 9;
		StageAt = Now;
		return false;
	}

	bool WaitForFreshAfterProbe(double Now, const FIMAcousticS4WorldIdentity& Identity)
	{
		if (HasFreshCurrentSourceBlock(ProbeTraceBegin, Identity)) return true;
		if (Now - StageAt > IMS4WorldPhaseTimeoutSeconds)
			return BeginFailure(TEXT("Current source did not publish a fresh accepted block after identity rejection."));
		return false;
	}

	bool HasFreshCurrentSourceBlock(uint32 Begin, const FIMAcousticS4WorldIdentity& Identity) const
	{
		if (!CurrentContext.IsValid()) return false;
		const uint32 End = IMS4WorldSourceTraceCount(CurrentContext);
		const uint32 First = FMath::Min(Begin, End);
		for (uint32 Index = First; Index < End; ++Index)
		{
			const FIMAcousticBlockProbe& B = CurrentContext->CapturedSourceBlocks[int32(Index)];
			if (B.CallbackAudioComponentId == Identity.AudioId
				&& B.Reject == EIMAcousticProbeReject::Accepted
				&& !B.InputDelayPriming
				&& B.ResultAudioComponentId == Identity.AudioId
				&& B.ResultWorldGeneration == Identity.WorldGeneration
				&& B.ResultVoiceGeneration != 0
				&& (!Identity.VoiceGeneration || B.ResultVoiceGeneration == Identity.VoiceGeneration)
				&& B.ResultSpatialVersion == B.OutputSpatialVersion
				&& B.ResultSequence && B.OutputEnergy > 1.0e-9)
				return true;
		}
		return false;
	}

	bool WaitFirstWorldEnd(double Now)
	{
		if (IMS4WorldPIE())
		{
			if (Now - StageAt > 30.0) return BeginFailure(TEXT("PIE did not end after the first world was stopped."));
			return false;
		}
		State->FirstTeardownObservedAt = Now;
		State->FirstRenderedBlocksAtTeardown = State->FirstContext->Device->RenderedBlocks.load(std::memory_order_acquire);
		State->FirstSourceTraceAtTeardown = IMS4WorldSourceTraceCount(State->FirstContext);
		State->FirstEnvironmentTraceAtTeardown = IMS4WorldEnvironmentTraceCount(State->FirstContext);
		State->FirstSourcePcmAtTeardown = IMS4WorldSourcePcmFrames(State->FirstContext);
		State->FirstWetPcmAtTeardown = IMS4WorldWetPcmFrames(State->FirstContext);
		Stage = 13;
		StageAt = Now;
		return false;
	}

	bool VerifyFirstWorldRelease(double Now)
	{
		if (!WorldResourcesReleased(State->FirstContext, State->First, Now, true)) return false;
		State->First.bContextReleased = true;
		if (!ReleaseCountersStable(State->FirstContext, State->FirstRenderedBlocksAtTeardown,
			State->FirstSourceTraceAtTeardown, State->FirstEnvironmentTraceAtTeardown,
			State->FirstSourcePcmAtTeardown, State->FirstWetPcmAtTeardown))
			return BeginFailure(TEXT("First PIE context continued publishing source/environment work after teardown."));
		State->First.SourceTraceEnd = IMS4WorldSourceTraceCount(State->FirstContext);
		State->First.EnvironmentTraceEnd = IMS4WorldEnvironmentTraceCount(State->FirstContext);
		State->First.SourcePcmEnd = IMS4WorldSourcePcmFrames(State->FirstContext);
		State->First.WetPcmEnd = IMS4WorldWetPcmFrames(State->FirstContext);
		if (State->FirstContext->CapturedSourceBlockClaimCount.load(std::memory_order_acquire)
				> uint32(State->FirstContext->CapturedSourceBlocks.Num())
			|| State->FirstContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire)
				> uint32(State->FirstContext->CapturedBlocks.Num()))
			return BeginFailure(TEXT("First PIE bounded source/environment trace overflowed; evidence is incomplete."));
		IMS4WorldAppendContextEvidence(State, State->FirstContext, State->First);
		State->FirstContext->CapturedSource.Empty();
		State->FirstContext->CapturedDry.Empty();
		State->FirstContext->CapturedWet.Empty();
		State->FirstContext->CapturedBus.Empty();
		State->FirstContext->CapturedSourceBlocks.Empty();
		State->FirstContext->CapturedBlocks.Empty();
		State->FirstContext->CapturedSourceBlockReady.reset();

		FString Error;
		GUnrealEd->AutomationLoadMap(IMS4WorldMap, false, &Error);
		if (!Error.IsEmpty())
			return BeginFailure(Error.IsEmpty() ? TEXT("Could not queue the second W1 PIE world.") : Error);
		ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticS4WorldTransitionCommand(State, 2));
		return true;
	}

	bool WaitSecondWorldEnd(double Now)
	{
		if (IMS4WorldPIE())
		{
			if (Now - StageAt > 30.0) return BeginFailure(TEXT("PIE did not end after the second world was stopped."));
			return false;
		}
		SecondTeardownObservedAt = Now;
		SecondRenderedBlocksAtTeardown = CurrentContext->Device->RenderedBlocks.load(std::memory_order_acquire);
		SecondSourceTraceAtTeardown = IMS4WorldSourceTraceCount(CurrentContext);
		SecondEnvironmentTraceAtTeardown = IMS4WorldEnvironmentTraceCount(CurrentContext);
		SecondSourcePcmAtTeardown = IMS4WorldSourcePcmFrames(CurrentContext);
		SecondWetPcmAtTeardown = IMS4WorldWetPcmFrames(CurrentContext);
		Stage = 15;
		StageAt = Now;
		return false;
	}

	bool VerifySecondWorldReleaseAndRestore(double Now)
	{
		if (!WorldResourcesReleased(CurrentContext, State->Second, Now, false)) return false;
		State->Second.bContextReleased = true;
		if (!ReleaseCountersStable(CurrentContext, SecondRenderedBlocksAtTeardown,
			SecondSourceTraceAtTeardown, SecondEnvironmentTraceAtTeardown,
			SecondSourcePcmAtTeardown, SecondWetPcmAtTeardown))
			return BeginFailure(TEXT("Second PIE context continued publishing source/environment work after teardown."));
		if (CurrentContext->CapturedSourceBlockClaimCount.load(std::memory_order_acquire)
				> uint32(CurrentContext->CapturedSourceBlocks.Num())
			|| CurrentContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire)
				> uint32(CurrentContext->CapturedBlocks.Num()))
			return BeginFailure(TEXT("Second PIE bounded source/environment trace overflowed; evidence is incomplete."));
		State->Second.SourceTraceEnd = IMS4WorldSourceTraceCount(CurrentContext);
		State->Second.EnvironmentTraceEnd = IMS4WorldEnvironmentTraceCount(CurrentContext);
		State->Second.SourcePcmEnd = IMS4WorldSourcePcmFrames(CurrentContext);
		State->Second.WetPcmEnd = IMS4WorldWetPcmFrames(CurrentContext);
		IMS4WorldAppendContextEvidence(State, CurrentContext, State->Second);
		if (!IMS4WorldSavePcm(State->EvidenceDirectory, TEXT("world2-source-mono.f32le"),
			CurrentContext->CapturedSource, State->Second.SourcePcmBegin, State->Second.SourcePcmEnd, 1)
			|| !IMS4WorldSavePcm(State->EvidenceDirectory, TEXT("world2-wet-stereo.f32le"),
				CurrentContext->CapturedWet, State->Second.WetPcmBegin, State->Second.WetPcmEnd, 2))
			State->AddFailure(TEXT("Could not save world-2 source/wet PCM evidence."));
		if (!RestoreOriginalMap()) State->AddFailure(TEXT("Original editor map or persistent source/map assets did not restore unchanged."));
		State->ResultMessage = TEXT("Two ordinary MetaSound PIE worlds produced source/environment evidence, a complete post-release wet tail, old-identity rejection, teardown, and original-map restoration.");
		return Finalize();
	}

	bool WorldResourcesReleased(const FIMAcousticMetaSoundContextPtr& Context,
		const FIMAcousticS4WorldIdentity& Identity, double Now, bool bFirst)
	{
		if (!Context.IsValid() || !Context->Device.IsValid() || !Context->Pool.IsValid())
			return BeginFailure(TEXT("Retained PIE context lost its observable resource owner before release checks."));
		const bool bSourceAlive = [&Context]()
		{
			for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
				if (Context->SourceConsumers[Slot].load(std::memory_order_acquire)) return true;
			return false;
		}();
		if (!Context->Stopped.load(std::memory_order_acquire)
			|| Context->Device->Alive.load(std::memory_order_acquire)
			|| !Context->Pool->Stopped.load(std::memory_order_acquire)
			|| bSourceAlive
			|| Context->EnvironmentConsumer.load(std::memory_order_acquire)
			|| Context->BusReaderConsumer.load(std::memory_order_acquire)
			|| Context->Device->ReverbEffectInstances.load(std::memory_order_acquire) != 0)
		{
			if (Now - StageAt > 15.0)
				return BeginFailure(TEXT("PIE teardown left a live source/environment consumer, reverb effect, device, pool, or context."));
			return false;
		}
		const FIMAcousticMetaSoundContextPtr Registered = IMAcousticMetaSound::FindAcousticMetaSoundContext(Identity.DeviceId);
		if (Registered.IsValid() && Registered.Get() == Context.Get())
		{
			if (Now - StageAt > 15.0) return BeginFailure(TEXT("Stopped PIE context remained registered for its audio device."));
			return false;
		}
		const double TearDownStart = bFirst ? State->FirstTeardownObservedAt : SecondTeardownObservedAt;
		if (Now - TearDownStart < 0.20) return false;
		return true;
	}

	bool ReleaseCountersStable(const FIMAcousticMetaSoundContextPtr& Context,
		uint64 Rendered, uint32 SourceTrace, uint32 EnvironmentTrace, uint32 SourcePcm, uint32 WetPcm) const
	{
		return Context.IsValid()
			&& Context->Device->RenderedBlocks.load(std::memory_order_acquire) == Rendered
			&& IMS4WorldSourceTraceCount(Context) == SourceTrace
			&& IMS4WorldEnvironmentTraceCount(Context) == EnvironmentTrace
			&& IMS4WorldSourcePcmFrames(Context) == SourcePcm
			&& IMS4WorldWetPcmFrames(Context) == WetPcm;
	}

	bool RestoreOriginalMap()
	{
		if (IMS4WorldPIE()) return false;
		UWorld* CurrentEditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!CurrentEditorWorld || !CurrentEditorWorld->GetOutermost()) return false;
		UPackage* CurrentPackage = CurrentEditorWorld->GetOutermost();
		if (CurrentPackage->IsDirty())
		{
			State->AddFailure(TEXT("Current editor map became dirty; refusing to discard it during original-map restoration."));
			return false;
		}
		if (CurrentPackage->GetName() != State->OriginalMap)
		{
			if (!FEditorFileUtils::LoadMap(*State->OriginalMap, false, true)) return false;
		}
		UWorld* Restored = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		State->bMapRestored = Restored && Restored->GetOutermost()->GetName() == State->OriginalMap
			&& !Restored->GetOutermost()->IsDirty();
		State->OriginalMapHashAfter = IMS4WorldHashFile(State->OriginalMapDiskPath);
		State->TestMapHashAfter = IMS4WorldHashFile(State->TestMapDiskPath);
		State->SourceHashAfter = IMS4WorldHashFile(State->SourceDiskPath);
		State->BakeAssetHashAfter = IMS4WorldHashFile(State->BakeAssetDiskPath);
		UPackage* SourcePackage = FindPackage(nullptr, IMAcousticTestSupport::GraphSourceAssetPath);
		UPackage* TestMapPackage = FindPackage(nullptr, IMS4WorldMap);
		State->bAssetsUnchanged = State->OriginalMapHashBefore == State->OriginalMapHashAfter
			&& !State->OriginalMapHashBefore.IsEmpty()
			&& State->TestMapHashBefore == State->TestMapHashAfter
			&& !State->TestMapHashBefore.IsEmpty()
			&& State->SourceHashBefore == State->SourceHashAfter
			&& !State->SourceHashBefore.IsEmpty()
			&& State->BakeAssetHashBefore == State->BakeAssetHashAfter
			&& !State->BakeAssetHashBefore.IsEmpty()
			&& (!SourcePackage || !SourcePackage->IsDirty())
			&& (!TestMapPackage || !TestMapPackage->IsDirty());
		return State->bMapRestored && State->bAssetsUnchanged;
	}

	bool BeginFailure(const FString& Why)
	{
		if (!State->bPassed && Stage == 90) return false;
		State->AddFailure(Why);
		State->ResultMessage = Why;
		State->RestoreGlobals();
		if (GUnrealEd && IMS4WorldPIE()) GUnrealEd->RequestEndPlayMap();
		Stage = 90;
		StageAt = FPlatformTime::Seconds();
		return false;
	}

	bool CleanupAndRestore(double Now)
	{
		if (IMS4WorldPIE())
		{
			if (Now - StageAt > 30.0)
			{
				State->AddFailure(TEXT("PIE did not end during fail-closed cleanup; original map was not loaded."));
				return Finalize();
			}
			return false;
		}
		if (!State->bMapRestored && !RestoreOriginalMap())
			State->AddFailure(TEXT("Original editor map/asset restoration failed during cleanup."));
		return Finalize();
	}

	bool Finalize()
	{
		if (State->bFinalized) return true;
		State->bFinalized = true;
		State->RestoreGlobals();
		if (!State->bMapRestored) State->AddFailure(TEXT("Original editor map identity was not verified after cleanup."));
		if (!State->bAssetsUnchanged) State->AddFailure(TEXT("Persistent map/source/bake asset hashes or dirty-state did not remain unchanged."));
		if (!State->bWorldMismatchRejected) State->AddFailure(TEXT("Old WorldGeneration negative control was not verified."));
		if (State->bOldAudioIdDistinct && !State->bOldAudioMismatchRejected)
			State->AddFailure(TEXT("Distinct old AudioComponentID negative control was not verified."));
		if (!State->bVoiceMismatchRejected) State->AddFailure(TEXT("Old VoiceGeneration negative control was not verified."));
		State->bPassed = State->Failure.IsEmpty();
		if (!SaveEvidence())
		{
			State->AddFailure(TEXT("Could not persist complete S4 world-transition evidence."));
			State->bPassed = false;
			SaveEvidence();
		}
		if (State->Test)
		{
			if (State->bPassed) State->Test->AddInfo(FString::Printf(TEXT("S4 world transition complete; evidence=%s"), *State->EvidenceDirectory));
			else State->Test->AddError(FString::Printf(TEXT("S4 world transition failed: %s; evidence=%s"), *State->Failure, *State->EvidenceDirectory));
		}
		if (State->bPassed)
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS4WorldTransition PASS old_id_distinct=%d old_voice_distinct=%d evidence=%s"),
				State->bOldAudioIdDistinct ? 1 : 0, State->bOldVoiceGenerationDistinct ? 1 : 0, *State->EvidenceDirectory);
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS4WorldTransition FAIL reason=%s old_id_distinct=%d old_voice_distinct=%d evidence=%s"),
				*State->Failure, State->bOldAudioIdDistinct ? 1 : 0,
				State->bOldVoiceGenerationDistinct ? 1 : 0, *State->EvidenceDirectory);
		}
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticS4WorldTransition %s evidence=%s"),
			State->bPassed ? TEXT("PASS") : TEXT("FAIL"), *State->EvidenceDirectory);
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), State->bPassed ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] IMExitEditor %s"), State->bPassed ? TEXT("PASS") : TEXT("FAIL"));
		return true;
	}

	bool SaveEvidence() const
	{
		const bool bSourceSaved = FFileHelper::SaveStringToFile(State->SourceBlocksCsv,
			*FPaths::Combine(State->EvidenceDirectory, TEXT("source-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const bool bEnvironmentSaved = FFileHelper::SaveStringToFile(State->EnvironmentBlocksCsv,
			*FPaths::Combine(State->EvidenceDirectory, TEXT("environment-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("IMAcousticS4WorldTransition.v1"));
		Root->SetStringField(TEXT("run_id"), State->RunId);
		Root->SetStringField(TEXT("outcome"), State->bPassed ? TEXT("PASS") : TEXT("FAIL"));
		Root->SetStringField(TEXT("message"), State->ResultMessage);
		Root->SetStringField(TEXT("failure"), State->Failure);
		Root->SetStringField(TEXT("map"), IMS4WorldMap);
		Root->SetStringField(TEXT("original_editor_map"), State->OriginalMap);
		Root->SetStringField(TEXT("source_asset"), IMAcousticTestSupport::GraphSourceAssetPath);
		Root->SetStringField(TEXT("bake_asset"), State->BakeAssetPath);
		Root->SetStringField(TEXT("original_map_md5_before"), State->OriginalMapHashBefore);
		Root->SetStringField(TEXT("original_map_md5_after"), State->OriginalMapHashAfter);
		Root->SetStringField(TEXT("test_map_md5_before"), State->TestMapHashBefore);
		Root->SetStringField(TEXT("test_map_md5_after"), State->TestMapHashAfter);
		Root->SetStringField(TEXT("source_asset_md5_before"), State->SourceHashBefore);
		Root->SetStringField(TEXT("source_asset_md5_after"), State->SourceHashAfter);
		Root->SetStringField(TEXT("bake_asset_md5_before"), State->BakeAssetHashBefore);
		Root->SetStringField(TEXT("bake_asset_md5_after"), State->BakeAssetHashAfter);
		Root->SetBoolField(TEXT("original_map_restored_clean"), State->bMapRestored);
		Root->SetBoolField(TEXT("persistent_assets_unchanged"), State->bAssetsUnchanged);
		IMS4WorldWriteIdentity(Root, TEXT("world1"), State->First);
		IMS4WorldWriteIdentity(Root, TEXT("world2"), State->Second);
		Root->SetStringField(TEXT("first_stop_live_generation"), LexToString(State->FirstStopLiveGeneration));
		Root->SetNumberField(TEXT("first_stop_source_pcm_frames_at_release"), State->FirstStopSourceFramesAtRelease);
		Root->SetNumberField(TEXT("first_stop_wet_frames_at_release"), State->FirstStopWetFramesAtRelease);
		Root->SetNumberField(TEXT("first_stop_tail_end_frame"), State->FirstStopTailEndFrame);
		Root->SetBoolField(TEXT("old_world_generation_rejected"), State->bWorldMismatchRejected);
		Root->SetBoolField(TEXT("old_audio_id_distinct_from_current"), State->bOldAudioIdDistinct);
		Root->SetBoolField(TEXT("old_audio_id_rejected"), State->bOldAudioMismatchRejected);
		Root->SetBoolField(TEXT("old_voice_generation_distinct_after_normal_replay"), State->bOldVoiceGenerationDistinct);
		Root->SetBoolField(TEXT("old_voice_generation_rejected"), State->bVoiceMismatchRejected);
		Root->SetStringField(TEXT("last_identity_reject_detail_mask"), State->LastRejectDetail);
		Root->SetStringField(TEXT("second_stop_live_generation"), LexToString(State->SecondStopLiveGeneration));
		Root->SetStringField(TEXT("second_replay_voice_generation"), LexToString(State->SecondReplayVoiceGeneration));
		Root->SetBoolField(TEXT("world1_context_stopped"), State->FirstContext.IsValid()
			&& State->FirstContext->Stopped.load(std::memory_order_acquire));
		Root->SetBoolField(TEXT("world2_context_stopped"), State->CurrentContext.IsValid()
			&& State->CurrentContext->Stopped.load(std::memory_order_acquire));
		Root->SetStringField(TEXT("source_trace_csv"), TEXT("source-blocks.csv"));
		Root->SetStringField(TEXT("environment_trace_csv"), TEXT("environment-blocks.csv"));
		Root->SetStringField(TEXT("world1_source_pcm"), TEXT("world1-source-mono.f32le"));
		Root->SetStringField(TEXT("world1_post_release_wet_pcm"), TEXT("world1-post-release-wet-stereo.f32le"));
		Root->SetStringField(TEXT("world2_source_pcm"), TEXT("world2-source-mono.f32le"));
		Root->SetStringField(TEXT("world2_wet_pcm"), TEXT("world2-wet-stereo.f32le"));
		return bSourceSaved && bEnvironmentSaved
			&& IMS4WorldSaveJson(FPaths::Combine(State->EvidenceDirectory, TEXT("capture.json")), Root);
	}

	TSharedRef<FIMAcousticS4WorldState> State;
	FIMAcousticMetaSoundContextPtr CurrentContext;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<AActor> SourceActor;
	TWeakObjectPtr<UAudioComponent> SourceAudio;
	double StartedAt = 0.0;
	double StageAt = 0.0;
	double StopRequestSeconds = 0.0;
	double SourceReleaseSeconds = 0.0;
	double SecondTeardownObservedAt = 0.0;
	uint32 StopTraceBegin = 0;
	uint32 StopPcmFramesAtRequest = 0;
	uint32 StopWetFrameBegin = 0;
	uint32 TailScanFrame = 0;
	uint32 TailSilentRunFrames = 0;
	uint32 SourcePcmFramesAtRelease = 0;
	uint32 SourceTraceAtRelease = 0;
	uint32 EnvironmentTraceAtRelease = 0;
	uint32 WetFramesAtRelease = 0;
	uint32 SourceTraceScan = 0;
	uint32 ProbeTraceBegin = 0;
	uint32 ActiveProbeRejectBit = 0;
	uint64 SecondRenderedBlocksAtTeardown = 0;
	uint32 SecondSourceTraceAtTeardown = 0;
	uint32 SecondEnvironmentTraceAtTeardown = 0;
	uint32 SecondSourcePcmAtTeardown = 0;
	uint32 SecondWetPcmAtTeardown = 0;
	uint64 StopLiveGeneration = 0;
	int32 Cycle = 1;
	int32 Stage = 0;
	int32 ActiveProbeIndex = INDEX_NONE;
	bool StopSourceReleased = false;
	bool StoppedSourceBlockObserved = false;
	bool StopDryPcmAdvanced = false;
	bool StopEnvironmentAlive = true;
	bool TailSawWetAfterRelease = false;
	bool TailPcmFinite = true;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticS4WorldTransitionTest,
	"IceMoon.AcousticField.S4.WorldTransition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIMAcousticS4WorldTransitionTest::RunTest(const FString&)
{
	const auto FailBeforePIE = [this](const FString& Reason)
	{
		AddError(Reason);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] AcousticS4WorldTransition INCONCLUSIVE %s"), *Reason);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] IMExitEditor INCONCLUSIVE"));
		return false;
	};
	if (!GEditor || !GUnrealEd) return FailBeforePIE(TEXT("World-transition automation requires an active Unreal Editor."));
	if (IMAcousticS4WorldTransitionTestPrivate::IMS4WorldPIE()) return FailBeforePIE(TEXT("Refusing to replace an already-running PIE world."));
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld || !EditorWorld->GetOutermost() || EditorWorld->GetOutermost()->IsDirty())
		return FailBeforePIE(TEXT("Current editor map is missing or dirty; refusing to switch maps."));
	const FString OriginalMap = EditorWorld->GetOutermost()->GetName();
	if (!FPackageName::IsValidLongPackageName(OriginalMap) || !FPackageName::DoesPackageExist(OriginalMap))
		return FailBeforePIE(TEXT("Current editor map cannot be restored from a saved package."));
	if (!FPackageName::DoesPackageExist(IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap))
		return FailBeforePIE(TEXT("Existing read-only W1 doorway map is missing; test will not manufacture or save it."));
	if (UPackage* LoadedTestMap = FindPackage(nullptr, IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap); LoadedTestMap && LoadedTestMap->IsDirty())
		return FailBeforePIE(TEXT("W1 test map is already dirty; refusing to discard map changes."));

	USoundBase* GraphSource = LoadObject<USoundBase>(nullptr, IMAcousticTestSupport::GraphSourceAssetPath);
	if (!GraphSource || !IMAcousticMetaSound::IsAcousticMetaSound(GraphSource) || GraphSource->GetOutermost()->IsDirty())
		return FailBeforePIE(TEXT("Ordinary MetaSound source asset is missing, invalid, or dirty."));
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("IceMoonAcousticField"));
	if (!Plugin.IsValid()) return FailBeforePIE(TEXT("Could not resolve the acoustic plugin for read-only asset fingerprinting."));

	auto State = MakeShared<IMAcousticS4WorldTransitionTestPrivate::FIMAcousticS4WorldState>();
	State->Test = this;
	State->RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	State->OriginalMap = OriginalMap;
	State->OriginalMapDiskPath = FPackageName::LongPackageNameToFilename(OriginalMap, FPackageName::GetMapPackageExtension());
	State->TestMapDiskPath = FPackageName::LongPackageNameToFilename(IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap, FPackageName::GetMapPackageExtension());
	State->SourceDiskPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Content/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay.uasset"));
	State->OriginalMapHashBefore = IMAcousticS4WorldTransitionTestPrivate::IMS4WorldHashFile(State->OriginalMapDiskPath);
	State->TestMapHashBefore = IMAcousticS4WorldTransitionTestPrivate::IMS4WorldHashFile(State->TestMapDiskPath);
	State->SourceHashBefore = IMAcousticS4WorldTransitionTestPrivate::IMS4WorldHashFile(State->SourceDiskPath);
	if (State->OriginalMapHashBefore.IsEmpty() || State->TestMapHashBefore.IsEmpty() || State->SourceHashBefore.IsEmpty())
		return FailBeforePIE(TEXT("Could not fingerprint the original map, read-only W1 map, or ordinary MetaSound asset."));

	State->EvidenceDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/S4-WorldTransition"), State->RunId);
	if (!IFileManager::Get().MakeDirectory(*State->EvidenceDirectory, true))
		return FailBeforePIE(TEXT("Could not create the S4 world-transition evidence directory."));
	State->OriginalBackgroundVolume = FApp::GetUnfocusedVolumeMultiplier();
	State->bOriginalAllowBackgroundAudio = GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	FApp::SetUnfocusedVolumeMultiplier(1.0f);
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 8192, 4096, true);

	FString Error;
	GUnrealEd->AutomationLoadMap(IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap, false, &Error);
	if (!Error.IsEmpty())
	{
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(State->OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = State->bOriginalAllowBackgroundAudio;
		AddError(Error.IsEmpty() ? TEXT("Could not queue the W1 doorway PIE world.") : Error);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] AcousticS4WorldTransition FAIL %s"), Error.IsEmpty() ? TEXT("map load failed") : *Error);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] IMExitEditor FAIL"));
		return false;
	}
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<IMAcousticS4WorldTransitionTestPrivate::FIMAcousticS4WorldTransitionCommand>(State, 1));
	return true;
}

namespace IMAcousticS4WorldTransitionTestPrivate
{
constexpr double IMS4MultiSourceTimeoutSeconds = 120.0;
constexpr double IMS4MultiSourcePhaseTimeoutSeconds = 25.0;
constexpr double IMS4MultiSourceTeardownTimeoutSeconds = 30.0;
constexpr double IMS4MultiSourceReleaseQuietSeconds = 0.20;
constexpr uint32 IMS4MultiSourceTraceCapacity = 32768;
constexpr uint32 IMS4MultiSourceEnvironmentTraceCapacity = 4096;

struct FIMAcousticS4MultiSourceEntry
{
	FString Name;
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UAudioComponent> Audio;
	uint64 AudioId = 0;
	int32 Slot = INDEX_NONE;
	uint64 VoiceGeneration = 0;
	uint64 StartLiveGeneration = 0;
	uint64 StopLiveGeneration = 0;
	uint32 TraceBegin = 0;
};

struct FIMAcousticS4MultiSourceState final : FIMAcousticS4WorldState
{
	TWeakObjectPtr<UWorld> PieWorld;
	FIMAcousticMetaSoundContextPtr Context;
	FIMAcousticMetaSoundContextPtr PreSourceContext;
	TArray<FIMAcousticS4MultiSourceEntry> Sources;
	FString PhaseCsv = TEXT("phase,seconds,world,device_id,world_generation,reverb_baseline,reverb_expected,reverb_observed,environment_consumer,bus_reader,duplicate_baseline,duplicate_observed,source_consumer_count,source_renderer_baseline,source_renderer_observed,rendered_blocks,rejected_blocks,source_blocks,environment_blocks,source_trace_count,environment_trace_count,source,audio_id,slot,audio_id_in_slot,voice_generation,live_generation,source_consumer,matching,accepted,path,output,input,post_a_release_accepted,post_a_release_path,post_a_release_output,post_a_release_input,voice_consistent,failure_reason\n");
	FString TestMapDiskPath;
	FString SourceDiskPath;
	int32 DeviceId = 0;
	int32 ReverbBaseline = 0;
	int32 ReverbExpected = 1;
	int32 SourceRendererBaseline = 0;
	uint64 DuplicateBaseline = 0;
	uint64 WorldGeneration = 0;
	uint32 AReleaseTraceBegin = 0;
	uint32 CrossProbeTraceBegin = 0;
	uint32 PhaseIndex = 0;
	uint64 TeardownRenderedBlocks = 0;
	uint64 TeardownRejectedBlocks = 0;
	uint64 TeardownSourceBlocks = 0;
	uint64 TeardownEnvironmentBlocks = 0;
	uint32 TeardownSourceTraceCount = 0;
	uint32 TeardownEnvironmentTraceCount = 0;
	double TeardownObservedAt = 0.0;
	bool bHadPreSourceContext = false;
	bool bCapturedAudioIdsBeforeC = false;
	bool bAReleased = false;
	bool bCrossProbeRequested = false;
	bool bTeardownObserved = false;
	bool bWorldResourcesReleased = false;
	std::array<uint64, FIMAcousticMetaSoundContext::MaxVoices> AudioIdsBeforeC{};

	static FString Csv(const FString& Value)
	{
		return FString::Printf(TEXT("\"%s\""), *Value.Replace(TEXT("\""), TEXT("\"\"")));
	}

	bool SaveSummary() const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("IMAcousticS4MultiSourceWorldLifecycle.v1"));
		Root->SetStringField(TEXT("run_id"), RunId);
		Root->SetStringField(TEXT("outcome"), Failure.IsEmpty()
			? (bFinalized ? TEXT("PASS") : TEXT("IN_PROGRESS")) : TEXT("FAIL"));
		Root->SetStringField(TEXT("message"), ResultMessage);
		Root->SetStringField(TEXT("failure_reason"), Failure);
		Root->SetStringField(TEXT("map"), IMS4WorldMap);
		Root->SetStringField(TEXT("original_editor_map"), OriginalMap);
		Root->SetNumberField(TEXT("device_id"), DeviceId);
		Root->SetStringField(TEXT("world_generation"), LexToString(WorldGeneration));
		Root->SetNumberField(TEXT("reverb_effect_baseline"), ReverbBaseline);
		Root->SetNumberField(TEXT("reverb_effect_expected"), ReverbExpected);
		Root->SetNumberField(TEXT("source_renderer_baseline"), SourceRendererBaseline);
		Root->SetBoolField(TEXT("pre_source_context_present"), bHadPreSourceContext);
		Root->SetBoolField(TEXT("a_released"), bAReleased);
		Root->SetBoolField(TEXT("cross_source_a_to_b_rejected"), bCrossProbeRequested
			&& Context.IsValid() && Context->IdentityInjection.Probes[0].Observed.load(std::memory_order_acquire)
			&& Context->IdentityInjection.Probes[0].Rejected.load(std::memory_order_acquire));
		Root->SetBoolField(TEXT("world_resources_released"), bWorldResourcesReleased);
		Root->SetBoolField(TEXT("original_map_restored_clean"), bMapRestored);
		Root->SetBoolField(TEXT("persistent_assets_unchanged"), bAssetsUnchanged);
		Root->SetNumberField(TEXT("phase_count"), PhaseIndex);
		Root->SetStringField(TEXT("evidence_directory"), EvidenceDirectory);
		return IMS4WorldSaveJson(FPaths::Combine(EvidenceDirectory, TEXT("summary.json")), Root);
	}

	bool RecordPhase(const FString& Phase)
	{
		const double Now = FPlatformTime::Seconds();
		const uint32 SourceTraceEnd = IMS4WorldSourceTraceCount(Context);
		const uint32 EnvironmentTraceEnd = IMS4WorldEnvironmentTraceCount(Context);
		const int32 ReverbObserved = Context.IsValid() && Context->Device.IsValid()
			? Context->Device->ReverbEffectInstances.load(std::memory_order_acquire) : 0;
		const bool bEnvironmentConsumer = Context.IsValid()
			&& Context->EnvironmentConsumer.load(std::memory_order_acquire);
		const bool bBusReaderConsumer = Context.IsValid()
			&& Context->BusReaderConsumer.load(std::memory_order_acquire);
		const uint64 DuplicateObserved = Context.IsValid()
			? Context->DuplicateConsumers.load(std::memory_order_acquire) : 0;
		uint32 SourceConsumerCount = 0;
		uint32 UsedSlotCount = 0;
		if (Context.IsValid())
		{
			for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
			{
				if (Context->SourceConsumers[Slot].load(std::memory_order_acquire)) ++SourceConsumerCount;
				if (Context->AudioIds[Slot].load(std::memory_order_acquire)) ++UsedSlotCount;
			}
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("IMAcousticS4MultiSourceWorldLifecycle.phase.v1"));
		Root->SetNumberField(TEXT("phase_index"), PhaseIndex);
		Root->SetStringField(TEXT("phase"), Phase);
		Root->SetNumberField(TEXT("seconds"), Now);
		Root->SetStringField(TEXT("world"), PieWorld.IsValid() && PieWorld->GetOutermost()
			? UWorld::RemovePIEPrefix(PieWorld->GetOutermost()->GetName()) : FString());
		Root->SetNumberField(TEXT("device_id"), DeviceId);
		Root->SetStringField(TEXT("world_generation"), LexToString(WorldGeneration));
		Root->SetNumberField(TEXT("reverb_effect_baseline"), ReverbBaseline);
		Root->SetNumberField(TEXT("reverb_effect_expected"), ReverbExpected);
		Root->SetNumberField(TEXT("reverb_effect_observed"), ReverbObserved);
		Root->SetBoolField(TEXT("pre_source_context_present"), bHadPreSourceContext);
		Root->SetBoolField(TEXT("environment_consumer"), bEnvironmentConsumer);
		Root->SetBoolField(TEXT("bus_reader_consumer"), bBusReaderConsumer);
		Root->SetStringField(TEXT("duplicate_consumers_baseline"), LexToString(DuplicateBaseline));
		Root->SetStringField(TEXT("duplicate_consumers_observed"), LexToString(DuplicateObserved));
		Root->SetNumberField(TEXT("source_consumer_count"), SourceConsumerCount);
		Root->SetNumberField(TEXT("source_renderer_baseline"), SourceRendererBaseline);
		Root->SetNumberField(TEXT("source_renderer_observed"), Context.IsValid() && Context->Device.IsValid()
			? Context->Device->SourceRendererInstances.load(std::memory_order_acquire) : 0);
		Root->SetNumberField(TEXT("used_source_slot_count"), UsedSlotCount);
		Root->SetStringField(TEXT("rendered_blocks"), Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->Device->RenderedBlocks.load(std::memory_order_acquire)) : TEXT("0"));
		Root->SetStringField(TEXT("rejected_blocks"), Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->Device->RejectedBlocks.load(std::memory_order_acquire)) : TEXT("0"));
		Root->SetStringField(TEXT("source_blocks"), Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->SourceBlocks.load(std::memory_order_acquire)) : TEXT("0"));
		Root->SetStringField(TEXT("environment_blocks"), Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->EnvironmentBlocks.load(std::memory_order_acquire)) : TEXT("0"));
		Root->SetNumberField(TEXT("source_trace_count"), SourceTraceEnd);
		Root->SetNumberField(TEXT("environment_trace_count"), EnvironmentTraceEnd);
		Root->SetStringField(TEXT("failure_reason"), Failure);
		Root->SetBoolField(TEXT("a_released"), bAReleased);
		Root->SetBoolField(TEXT("audio_ids_snapshotted_before_c"), bCapturedAudioIdsBeforeC);

		TArray<FString> CommonColumns;
		CommonColumns.Add(Csv(Phase));
		CommonColumns.Add(Csv(LexToString(Now)));
		CommonColumns.Add(Csv(PieWorld.IsValid() && PieWorld->GetOutermost()
			? UWorld::RemovePIEPrefix(PieWorld->GetOutermost()->GetName()) : FString()));
		CommonColumns.Add(Csv(LexToString(DeviceId)));
		CommonColumns.Add(Csv(LexToString(WorldGeneration)));
		CommonColumns.Add(Csv(LexToString(ReverbBaseline)));
		CommonColumns.Add(Csv(LexToString(ReverbExpected)));
		CommonColumns.Add(Csv(LexToString(ReverbObserved)));
		CommonColumns.Add(Csv(bEnvironmentConsumer ? TEXT("1") : TEXT("0")));
		CommonColumns.Add(Csv(bBusReaderConsumer ? TEXT("1") : TEXT("0")));
		CommonColumns.Add(Csv(LexToString(DuplicateBaseline)));
		CommonColumns.Add(Csv(LexToString(DuplicateObserved)));
		CommonColumns.Add(Csv(LexToString(SourceConsumerCount)));
		CommonColumns.Add(Csv(LexToString(SourceRendererBaseline)));
		CommonColumns.Add(Csv(Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->Device->SourceRendererInstances.load(std::memory_order_acquire)) : TEXT("0")));
		CommonColumns.Add(Csv(Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->Device->RenderedBlocks.load(std::memory_order_acquire)) : TEXT("0")));
		CommonColumns.Add(Csv(Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->Device->RejectedBlocks.load(std::memory_order_acquire)) : TEXT("0")));
		CommonColumns.Add(Csv(Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->SourceBlocks.load(std::memory_order_acquire)) : TEXT("0")));
		CommonColumns.Add(Csv(Context.IsValid() && Context->Device.IsValid()
			? LexToString(Context->EnvironmentBlocks.load(std::memory_order_acquire)) : TEXT("0")));
		CommonColumns.Add(Csv(LexToString(SourceTraceEnd)));
		CommonColumns.Add(Csv(LexToString(EnvironmentTraceEnd)));

		for (int32 Index = 0; Index < Sources.Num(); ++Index)
		{
			const FIMAcousticS4MultiSourceEntry& Entry = Sources[Index];
			const uint64 AudioIdAtSlot = Context.IsValid() && Entry.Slot >= 0
				&& Entry.Slot < int32(FIMAcousticMetaSoundContext::MaxVoices)
				? Context->AudioIds[uint32(Entry.Slot)].load(std::memory_order_acquire) : 0;
			const bool bSourceConsumer = Context.IsValid() && Entry.Slot >= 0
				&& Entry.Slot < int32(FIMAcousticMetaSoundContext::MaxVoices)
				&& Context->SourceConsumers[uint32(Entry.Slot)].load(std::memory_order_acquire);
			const uint64 LiveGeneration = Context.IsValid() && Context->Device.IsValid() && Entry.Slot >= 0
				&& Entry.Slot < int32(Context->Device->Voices.Num())
				? Context->Device->Voices[Entry.Slot]->LiveGeneration.load(std::memory_order_acquire) : 0;
			FIMAcousticS4WorldStats Stats;
			if (Context.IsValid() && Entry.AudioId)
				IMS4WorldInspectSource(Context, Entry.TraceBegin, SourceTraceEnd, Entry.AudioId, WorldGeneration, Stats);
			TSharedRef<FJsonObject> SourceJson = MakeShared<FJsonObject>();
			SourceJson->SetStringField(TEXT("name"), Entry.Name);
			SourceJson->SetStringField(TEXT("audio_component_id"), LexToString(Entry.AudioId));
			SourceJson->SetNumberField(TEXT("slot"), Entry.Slot);
			SourceJson->SetStringField(TEXT("audio_id_retained_in_slot"), LexToString(AudioIdAtSlot));
			SourceJson->SetStringField(TEXT("voice_generation"), LexToString(Entry.VoiceGeneration));
			SourceJson->SetStringField(TEXT("live_generation"), LexToString(LiveGeneration));
			SourceJson->SetBoolField(TEXT("source_consumer"), bSourceConsumer);
			SourceJson->SetNumberField(TEXT("trace_begin"), Entry.TraceBegin);
			SourceJson->SetNumberField(TEXT("matching_blocks"), Stats.Matching);
			SourceJson->SetNumberField(TEXT("accepted_blocks"), Stats.Accepted);
			SourceJson->SetNumberField(TEXT("path_nonzero_blocks"), Stats.Path);
			SourceJson->SetNumberField(TEXT("output_nonzero_blocks"), Stats.Output);
			SourceJson->SetNumberField(TEXT("input_nonzero_blocks"), Stats.Input);
			SourceJson->SetBoolField(TEXT("voice_consistent"), Stats.bVoiceConsistent);
			Root->SetObjectField(FString::Printf(TEXT("source_%s"), *Entry.Name), SourceJson);

			FIMAcousticS4WorldStats PostAReleaseStats;
			if (Index == 1 && AReleaseTraceBegin && Context.IsValid() && Entry.AudioId)
				IMS4WorldInspectSource(Context, AReleaseTraceBegin, SourceTraceEnd,
					Entry.AudioId, WorldGeneration, PostAReleaseStats);
			TArray<FString> Columns = CommonColumns;
			Columns.Add(Csv(Entry.Name));
			Columns.Add(Csv(LexToString(Entry.AudioId)));
			Columns.Add(Csv(LexToString(Entry.Slot)));
			Columns.Add(Csv(LexToString(AudioIdAtSlot)));
			Columns.Add(Csv(LexToString(Entry.VoiceGeneration)));
			Columns.Add(Csv(LexToString(LiveGeneration)));
			Columns.Add(Csv(bSourceConsumer ? TEXT("1") : TEXT("0")));
			Columns.Add(Csv(LexToString(Stats.Matching)));
			Columns.Add(Csv(LexToString(Stats.Accepted)));
			Columns.Add(Csv(LexToString(Stats.Path)));
			Columns.Add(Csv(LexToString(Stats.Output)));
			Columns.Add(Csv(LexToString(Stats.Input)));
			Columns.Add(Csv(LexToString(PostAReleaseStats.Accepted)));
			Columns.Add(Csv(LexToString(PostAReleaseStats.Path)));
			Columns.Add(Csv(LexToString(PostAReleaseStats.Output)));
			Columns.Add(Csv(LexToString(PostAReleaseStats.Input)));
			Columns.Add(Csv(Stats.bVoiceConsistent ? TEXT("1") : TEXT("0")));
			Columns.Add(Csv(Failure));
			for (int32 Column = 0; Column < Columns.Num(); ++Column)
			{
				if (Column) PhaseCsv += TEXT(",");
				PhaseCsv += Columns[Column];
			}
			PhaseCsv += TEXT("\n");
		}

		const FString PhasePath = FPaths::Combine(EvidenceDirectory,
			FString::Printf(TEXT("phase-%03u-%s.json"), PhaseIndex, *Phase));
		++PhaseIndex;
		const bool bPhaseSaved = IMS4WorldSaveJson(PhasePath, Root);
		const bool bCsvSaved = FFileHelper::SaveStringToFile(PhaseCsv,
			*FPaths::Combine(EvidenceDirectory, TEXT("phases.csv")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return bPhaseSaved && bCsvSaved && SaveSummary();
	}
};

class FIMAcousticS4MultiSourceLifecycleCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticS4MultiSourceLifecycleCommand(
		TSharedRef<FIMAcousticS4MultiSourceState> InState)
		: State(InState), StartedAt(FPlatformTime::Seconds()), StageAt(StartedAt)
	{
	}

	bool Update() override
	{
		check(IsInGameThread());
		const double Now = FPlatformTime::Seconds();
		if (Stage != 90 && Now - StartedAt > IMS4MultiSourceTimeoutSeconds)
			return BeginFailure(TEXT("Multi-source same-world lifecycle exceeded its 120 second cap."));
		if (Stage == 90) return CleanupAndRestore(Now);
		if (Stage == 7) return WaitForWorldTeardown(Now);

		UWorld* World = IMS4WorldPIE();
		if (!World)
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("The requested W1 PIE world did not become available."));
			return false;
		}
		if (Stage == 0) return SetupWorld(World, Now);
		if (!State->Context.IsValid() && !BindContext(Now)) return false;
		if (State->Context.IsValid() && !NoSourceTraceOverflow())
			return BeginFailure(TEXT("Bounded source/environment graph trace overflowed; lifecycle evidence is incomplete."));
		if (Stage == 1) return WaitForSourceA(Now);
		if (Stage == 2) return WaitForSourceB(Now);
		if (Stage == 3) return WaitForSourceAReleaseAndBContinuation(Now);
		if (Stage == 4) return WaitForCrossSourceReject(Now);
		if (Stage == 5) return WaitForBRecoveryAndSpawnC(Now);
		if (Stage == 6) return WaitForSourceCAndTeardown(Now);
		return BeginFailure(TEXT("Multi-source lifecycle command reached an unknown stage."));
	}

private:
	bool SetupWorld(UWorld* World, double Now)
	{
		const FString Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
		if (Map != IMS4WorldMap)
			return BeginFailure(TEXT("PIE opened a map other than the existing read-only W1 doorway fixture."));

		AIMAcousticBakeVolume* FoundVolume = nullptr;
		int32 VolumeCount = 0;
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
		{
			FoundVolume = *It;
			++VolumeCount;
		}
		if (VolumeCount != 1 || !FoundVolume || !FoundVolume->BakedField)
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("W1 PIE must contain exactly one already-baked acoustic volume."));
			return false;
		}
		if (FoundVolume->BakedField->GetOutermost()->IsDirty())
			return BeginFailure(TEXT("The referenced bake asset is dirty; refusing to continue."));

		APlayerController* Listener = World->GetFirstPlayerController();
		FAudioDevice* AudioDevice = World->GetAudioDeviceRaw();
		if (!Listener || !AudioDevice)
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("PIE listener or audio device did not initialize."));
			return false;
		}
		State->PieWorld = World;
		State->DeviceId = int32(AudioDevice->DeviceID);
		State->BakeAssetPath = FoundVolume->BakedField->GetPathName();
		State->BakeAssetDiskPath = FPackageName::LongPackageNameToFilename(
			FoundVolume->BakedField->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
		State->BakeAssetHashBefore = IMS4WorldHashFile(State->BakeAssetDiskPath);
		if (State->BakeAssetHashBefore.IsEmpty())
			return BeginFailure(TEXT("Could not fingerprint the W1 read-only bake asset."));

		FoundVolume->bEnableV2 = true;
		FoundVolume->bDirectRoute = false;
		FoundVolume->bPathRoute = true;
		FoundVolume->bReverbRoute = true;
		Listener->SetAudioListenerOverride(nullptr, FVector(0.0, 200.0, 150.0), FRotator(0.0, -90.0, 0.0));
		State->PreSourceContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
		State->bHadPreSourceContext = State->PreSourceContext.IsValid();
		if (State->bHadPreSourceContext)
		{
			if (!State->PreSourceContext->Device.IsValid() || !State->PreSourceContext->Pool.IsValid())
				return BeginFailure(TEXT("A pre-existing W1 context has no observable device or pool."));
			State->Context = State->PreSourceContext;
			State->WorldGeneration = State->Context->Pool->WorldGeneration;
			State->ReverbBaseline = State->Context->Device->ReverbEffectInstances.load(std::memory_order_acquire);
			State->SourceRendererBaseline = State->Context->Device->SourceRendererInstances.load(std::memory_order_acquire);
			State->DuplicateBaseline = State->Context->DuplicateConsumers.load(std::memory_order_acquire);
		}
		State->ReverbExpected = State->ReverbBaseline + 1;
		if (FIMAcousticMetaSoundContext::MaxVoices != 64)
			return BeginFailure(TEXT("The source-slot contract changed from the required 64 entries."));

		if (!SpawnSource(World, 0))
			return BeginFailure(TEXT("Could not create and start ordinary MetaSound source A."));
		Stage = 1;
		StageAt = Now;
		if (!State->RecordPhase(TEXT("source_a_requested")))
			return BeginFailure(TEXT("Could not persist the source-A request phase."));
		return false;
	}

	bool SpawnSource(UWorld* World, int32 Index)
	{
		if (!World || !State->Sources.IsValidIndex(Index)) return false;
		FIMAcousticS4MultiSourceEntry& Entry = State->Sources[Index];
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor) return false;
		Actor->SetActorLocation(FVector(0.0, -200.0, 150.0));
		UAudioComponent* Audio = NewObject<UAudioComponent>(Actor);
		if (!Audio)
		{
			Actor->Destroy();
			return false;
		}
		Actor->AddInstanceComponent(Audio);
		Audio->RegisterComponent();
		Audio->SetWorldLocation(FVector(0.0, -200.0, 150.0));
		UIMAcousticSourceComponent* Marker = NewObject<UIMAcousticSourceComponent>(Actor);
		if (!Marker)
		{
			Actor->Destroy();
			return false;
		}
		Actor->AddInstanceComponent(Marker);
		Marker->AudioComponent = Audio;
		Marker->RegisterComponent();
		FString Error;
		if (!IMAcousticTestSupport::ConfigureGraphSource(Audio, Error) || !Marker->ValidateSource(Error))
		{
			Actor->Destroy();
			State->AddFailure(Error.IsEmpty()
				? FString::Printf(TEXT("Ordinary MetaSound source %s failed validation."), *Entry.Name) : Error);
			return false;
		}
		Entry.TraceBegin = IMS4WorldSourceTraceCount(State->Context);
		Entry.Actor = Actor;
		Entry.Audio = Audio;
		Audio->Play();
		return true;
	}

	bool BindContext(double Now)
	{
		UWorld* World = IMS4WorldPIE();
		FAudioDevice* AudioDevice = World ? World->GetAudioDeviceRaw() : nullptr;
		if (!AudioDevice || int32(AudioDevice->DeviceID) != State->DeviceId)
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("The W1 audio device changed or disappeared during source startup."));
			return false;
		}
		FIMAcousticMetaSoundContextPtr NewContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
		if (!NewContext.IsValid() || !NewContext->Pool.IsValid() || !NewContext->Device.IsValid())
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("The ordinary MetaSound world context did not initialize."));
			return false;
		}
		if (State->PreSourceContext.IsValid() && NewContext.Get() != State->PreSourceContext.Get())
			return BeginFailure(TEXT("Source startup replaced the pre-existing context for the same W1 device."));
		if (NewContext->Stopped.load(std::memory_order_acquire)
			|| !NewContext->Device->Alive.load(std::memory_order_acquire)
			|| NewContext->Pool->Stopped.load(std::memory_order_acquire))
			return BeginFailure(TEXT("The bound W1 context is already stopped or not alive."));
		State->Context = NewContext;
		State->WorldGeneration = NewContext->Pool->WorldGeneration;
		return true;
	}

	bool ReadSourceStats(int32 Index, uint32 Begin, FIMAcousticS4WorldStats& OutStats)
	{
		if (!State->Context.IsValid() || !State->Sources.IsValidIndex(Index)) return false;
		FIMAcousticS4MultiSourceEntry& Entry = State->Sources[Index];
		UAudioComponent* Audio = Entry.Audio.Get();
		if (!Audio) return false;
		const uint64 CurrentAudioId = Audio->GetAudioComponentID();
		if (!CurrentAudioId) return false;
		if (Entry.AudioId && Entry.AudioId != CurrentAudioId)
		{
			BeginFailure(FString::Printf(TEXT("Source %s changed AudioComponentID in one World."), *Entry.Name));
			return false;
		}
		Entry.AudioId = CurrentAudioId;
		const int32 CurrentSlot = IMS4WorldFindSlot(State->Context, CurrentAudioId);
		if (CurrentSlot == INDEX_NONE) return false;
		if (Entry.Slot != INDEX_NONE && Entry.Slot != CurrentSlot)
		{
			BeginFailure(FString::Printf(TEXT("Source %s changed its voice slot in one World."), *Entry.Name));
			return false;
		}
		Entry.Slot = CurrentSlot;
		if (State->Context->AudioIds[uint32(CurrentSlot)].load(std::memory_order_acquire) != CurrentAudioId)
		{
			BeginFailure(FString::Printf(TEXT("Source %s slot did not retain its AudioComponentID."), *Entry.Name));
			return false;
		}
		IMS4WorldInspectSource(State->Context, Begin, IMS4WorldSourceTraceCount(State->Context),
			CurrentAudioId, State->WorldGeneration, OutStats);
		if (!OutStats.bVoiceConsistent)
		{
			BeginFailure(FString::Printf(TEXT("Source %s emitted inconsistent VoiceGeneration values."), *Entry.Name));
			return false;
		}
		if (OutStats.VoiceGeneration)
		{
			const uint64 Live = State->Context->Device->Voices[CurrentSlot]->LiveGeneration.load(std::memory_order_acquire);
			if (Entry.VoiceGeneration && Entry.VoiceGeneration != OutStats.VoiceGeneration)
			{
				BeginFailure(FString::Printf(TEXT("Source %s changed its own VoiceGeneration while active."), *Entry.Name));
				return false;
			}
			if (Entry.StartLiveGeneration && Entry.StartLiveGeneration != Live)
			{
				BeginFailure(FString::Printf(TEXT("Source %s changed its own LiveGeneration while active."), *Entry.Name));
				return false;
			}
			Entry.VoiceGeneration = OutStats.VoiceGeneration;
			Entry.StartLiveGeneration = Live;
		}
		return true;
	}

	bool SharedConsumersReady(double Now)
	{
		if (!State->Context.IsValid() || !State->Context->Device.IsValid()) return false;
		const uint64 Duplicates = State->Context->DuplicateConsumers.load(std::memory_order_acquire);
		if (Duplicates != State->DuplicateBaseline)
			return BeginFailure(FString::Printf(TEXT("DuplicateConsumers changed from baseline %llu to %llu."),
				DuplicateBaselineForLog(), Duplicates));
		const int32 Effects = State->Context->Device->ReverbEffectInstances.load(std::memory_order_acquire);
		if (Effects > State->ReverbExpected)
			return BeginFailure(FString::Printf(TEXT("ReverbEffectInstances exceeded baseline+1: baseline=%d observed=%d."),
				State->ReverbBaseline, Effects));
		const bool bReady = State->Context->EnvironmentConsumer.load(std::memory_order_acquire)
			&& State->Context->BusReaderConsumer.load(std::memory_order_acquire)
			&& Effects == State->ReverbExpected;
		if (!bReady && Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
			return BeginFailure(FString::Printf(TEXT("Shared consumer baseline+1 did not settle: env=%d bus=%d effects=%d expected=%d."),
				State->Context->EnvironmentConsumer.load(std::memory_order_acquire) ? 1 : 0,
				State->Context->BusReaderConsumer.load(std::memory_order_acquire) ? 1 : 0,
				Effects, State->ReverbExpected));
		return bReady;
	}

	uint64 DuplicateBaselineForLog() const { return State->DuplicateBaseline; }

	bool SourceRendererCountIs(int32 Expected, const TCHAR* Phase)
	{
		const int32 Observed = State->Context->Device->SourceRendererInstances.load(std::memory_order_acquire);
		if (Observed == Expected) return true;
		return BeginFailure(FString::Printf(
			TEXT("Source renderer bundle count at %s: expected=%d observed=%d baseline=%d."),
			Phase, Expected, Observed, State->SourceRendererBaseline));
	}

	bool SourceReady(int32 Index, uint32 Begin, double Now, uint32 MinimumAccepted,
		FIMAcousticS4WorldStats& OutStats)
	{
		if (!ReadSourceStats(Index, Begin, OutStats)) return Stage != 90 && Now - StageAt <= IMS4MultiSourcePhaseTimeoutSeconds
			? false : BeginFailure(FString::Printf(TEXT("Source %s did not publish an identity-bound graph trace."),
				*State->Sources[Index].Name));
		const FIMAcousticS4MultiSourceEntry& Entry = State->Sources[Index];
		const bool bConsumer = State->Context->SourceConsumers[uint32(Entry.Slot)].load(std::memory_order_acquire);
		const UAudioComponent* Audio = Entry.Audio.Get();
		const bool bReady = Audio && Audio->IsPlaying() && bConsumer && OutStats.Matching >= MinimumAccepted
			&& OutStats.Accepted >= MinimumAccepted && OutStats.Input > 0 && OutStats.Path > 0 && OutStats.Output > 0
			&& OutStats.VoiceGeneration != 0
			&& State->Context->Device->Voices[Entry.Slot]->LiveGeneration.load(std::memory_order_acquire)
				== Entry.VoiceGeneration;
		if (!bReady && Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
			return BeginFailure(FString::Printf(
				TEXT("Source %s did not produce accepted nonzero graph output: playing=%d consumer=%d matching=%u accepted=%u path=%u output=%u input=%u voice=%llu."),
				*Entry.Name, Audio && Audio->IsPlaying() ? 1 : 0, bConsumer ? 1 : 0,
				OutStats.Matching, OutStats.Accepted, OutStats.Path, OutStats.Output, OutStats.Input,
				OutStats.VoiceGeneration));
		return bReady;
	}

	bool WaitForSourceA(double Now)
	{
		FIMAcousticS4WorldStats Stats;
		if (!SourceReady(0, State->Sources[0].TraceBegin, Now, 8, Stats)) return false;
		if (!SourceRendererCountIs(State->SourceRendererBaseline + 1, TEXT("A active"))) return false;
		if (!SharedConsumersReady(Now)) return false;
		if (!State->RecordPhase(TEXT("source_a_active")))
			return BeginFailure(TEXT("Could not persist the source-A active phase."));
		UWorld* World = IMS4WorldPIE();
		if (!SpawnSource(World, 1))
			return BeginFailure(TEXT("Could not create and start ordinary MetaSound source B."));
		Stage = 2;
		StageAt = Now;
		if (!State->RecordPhase(TEXT("source_b_requested")))
			return BeginFailure(TEXT("Could not persist the source-B request phase."));
		return false;
	}

	bool WaitForSourceB(double Now)
	{
		FIMAcousticS4WorldStats AStats;
		FIMAcousticS4WorldStats BStats;
		if (!SourceReady(0, State->Sources[0].TraceBegin, Now, 8, AStats)) return false;
		if (!SourceReady(1, State->Sources[1].TraceBegin, Now, 8, BStats)) return false;
		if (State->Sources[0].AudioId == State->Sources[1].AudioId
			|| State->Sources[0].Slot == State->Sources[1].Slot)
			return BeginFailure(TEXT("Sources A and B did not acquire distinct AudioComponentIDs and voice slots."));
		if (!SourceRendererCountIs(State->SourceRendererBaseline + 2, TEXT("A/B active"))) return false;
		if (!SharedConsumersReady(Now)) return false;
		if (!State->RecordPhase(TEXT("sources_a_b_active")))
			return BeginFailure(TEXT("Could not persist the A/B active phase."));

		FIMAcousticS4MultiSourceEntry& A = State->Sources[0];
		A.StopLiveGeneration = State->Context->Device->Voices[A.Slot]->LiveGeneration.load(std::memory_order_acquire);
		if (UAudioComponent* Audio = A.Audio.Get()) Audio->Stop();
		if (A.Actor.IsValid()) A.Actor->Destroy();
		Stage = 3;
		StageAt = Now;
		if (!State->RecordPhase(TEXT("source_a_stop_destroy_requested")))
			return BeginFailure(TEXT("Could not persist the A stop/destroy phase."));
		return false;
	}

	bool NoSourceABlockAfterRelease() const
	{
		if (!State->Context.IsValid() || !State->bAReleased) return false;
		const uint32 End = IMS4WorldSourceTraceCount(State->Context);
		if (End < State->AReleaseTraceBegin) return false;
		const uint64 AudioIdA = State->Sources[0].AudioId;
		for (uint32 Index = State->AReleaseTraceBegin; Index < End; ++Index)
		{
			if (State->Context->CapturedSourceBlocks[int32(Index)].CallbackAudioComponentId == AudioIdA)
				return false;
		}
		return true;
	}

	bool WaitForSourceAReleaseAndBContinuation(double Now)
	{
		FIMAcousticS4MultiSourceEntry& A = State->Sources[0];
		FIMAcousticS4MultiSourceEntry& B = State->Sources[1];
		const uint64 LiveA = State->Context->Device->Voices[A.Slot]->LiveGeneration.load(std::memory_order_acquire);
		if (!State->bAReleased)
		{
			const bool bAConsumer = State->Context->SourceConsumers[uint32(A.Slot)].load(std::memory_order_acquire);
			if (bAConsumer || LiveA <= A.VoiceGeneration)
			{
				if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
					return BeginFailure(TEXT("Stopping/destroying A did not release its consumer and advance its LiveGeneration."));
				return false;
			}
			if (State->Context->AudioIds[uint32(A.Slot)].load(std::memory_order_acquire) != A.AudioId)
				return BeginFailure(TEXT("A's AudioID was cleared or replaced after source-consumer release."));
			A.StopLiveGeneration = LiveA;
			State->AReleaseTraceBegin = IMS4WorldSourceTraceCount(State->Context);
			State->bAReleased = true;
			if (!SourceRendererCountIs(State->SourceRendererBaseline + 1, TEXT("A released, B active"))) return false;
			if (!State->RecordPhase(TEXT("source_a_released")))
				return BeginFailure(TEXT("Could not persist the source-A release phase."));
		}
		if (!NoSourceABlockAfterRelease())
			return BeginFailure(TEXT("An old A source block appeared after A's consumer release."));

		FIMAcousticS4WorldStats BAfterRelease;
		const uint32 TraceEnd = IMS4WorldSourceTraceCount(State->Context);
		IMS4WorldInspectSource(State->Context, State->AReleaseTraceBegin, TraceEnd,
			B.AudioId, State->WorldGeneration, BAfterRelease);
		const bool bBConsumer = State->Context->SourceConsumers[uint32(B.Slot)].load(std::memory_order_acquire);
		const UAudioComponent* BAudio = B.Audio.Get();
		if (!SharedConsumersReady(Now)) return false;
		if (!BAudio || !BAudio->IsPlaying() || !bBConsumer
			|| BAfterRelease.Accepted < 8 || BAfterRelease.Path == 0 || BAfterRelease.Output == 0
			|| !BAfterRelease.bVoiceConsistent || BAfterRelease.VoiceGeneration != B.VoiceGeneration
			|| State->Context->Device->Voices[B.Slot]->LiveGeneration.load(std::memory_order_acquire)
				!= B.StartLiveGeneration)
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(FString::Printf(
					TEXT("B did not continue with accepted nonzero output after A release: playing=%d consumer=%d accepted=%u path=%u output=%u voice=%llu."),
					BAudio && BAudio->IsPlaying() ? 1 : 0, bBConsumer ? 1 : 0,
					BAfterRelease.Accepted, BAfterRelease.Path, BAfterRelease.Output,
					BAfterRelease.VoiceGeneration));
			return false;
		}

		IMS4WorldResetProbe(State->Context->IdentityInjection.Probes[0]);
		const uint64 Sequence = State->Context->SourceBlocks.load(std::memory_order_acquire) + 1;
		State->CrossProbeTraceBegin = TraceEnd;
		State->Context->IdentityInjection.Request(1, A.AudioId, State->WorldGeneration,
			A.VoiceGeneration, Sequence, 0.0f, 0.0f, 0.0f, BAfterRelease.SpatialVersion);
		State->bCrossProbeRequested = true;
		Stage = 4;
		StageAt = Now;
		if (!State->RecordPhase(TEXT("cross_source_a_to_b_requested")))
			return BeginFailure(TEXT("Could not persist the A-to-B identity-negative-control request."));
		return false;
	}

	bool WaitForCrossSourceReject(double Now)
	{
		if (!NoSourceABlockAfterRelease())
			return BeginFailure(TEXT("An old A source block appeared after A's consumer release during the identity negative control."));
		const FIMAcousticIdentityInjectionProbe& Probe = State->Context->IdentityInjection.Probes[0];
		if (!Probe.Observed.load(std::memory_order_acquire))
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("The active B source operator did not consume the A-to-B identity negative control."));
			return false;
		}
		const FIMAcousticS4MultiSourceEntry& A = State->Sources[0];
		const FIMAcousticS4MultiSourceEntry& B = State->Sources[1];
		if (!IMS4WorldProbePassed(Probe, 128u, B.AudioId, State->WorldGeneration, B.VoiceGeneration)
			|| Probe.InjectedAudioId.load(std::memory_order_acquire) != A.AudioId
			|| Probe.InjectedWorldGeneration.load(std::memory_order_acquire) != State->WorldGeneration
			|| Probe.InjectedVoiceGeneration.load(std::memory_order_acquire) != A.VoiceGeneration)
			return BeginFailure(TEXT("A's captured source identity was not rejected by B without rendering."));
		if (!State->RecordPhase(TEXT("cross_source_a_to_b_rejected")))
			return BeginFailure(TEXT("Could not persist the A-to-B rejection phase."));
		Stage = 5;
		StageAt = Now;
		return false;
	}

	bool WaitForBRecoveryAndSpawnC(double Now)
	{
		if (!NoSourceABlockAfterRelease())
			return BeginFailure(TEXT("An old A source block appeared after release while B recovered."));
		FIMAcousticS4MultiSourceEntry& B = State->Sources[1];
		FIMAcousticS4WorldStats BAfterProbe;
		IMS4WorldInspectSource(State->Context, State->CrossProbeTraceBegin,
			IMS4WorldSourceTraceCount(State->Context), B.AudioId, State->WorldGeneration, BAfterProbe);
		const UAudioComponent* BAudio = B.Audio.Get();
		const bool bBConsumer = State->Context->SourceConsumers[uint32(B.Slot)].load(std::memory_order_acquire);
		if (!SharedConsumersReady(Now)) return false;
		if (!BAudio || !BAudio->IsPlaying() || !bBConsumer
			|| BAfterProbe.Accepted == 0 || BAfterProbe.Path == 0 || BAfterProbe.Output == 0
			|| !BAfterProbe.bVoiceConsistent || BAfterProbe.VoiceGeneration != B.VoiceGeneration
			|| State->Context->Device->Voices[B.Slot]->LiveGeneration.load(std::memory_order_acquire)
				!= B.StartLiveGeneration)
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("B did not recover with fresh accepted nonzero output after rejecting A's identity."));
			return false;
		}

		for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
			State->AudioIdsBeforeC[Slot] = State->Context->AudioIds[Slot].load(std::memory_order_acquire);
		State->bCapturedAudioIdsBeforeC = true;
		if (!State->RecordPhase(TEXT("source_b_recovered_before_c")))
			return BeginFailure(TEXT("Could not persist B's post-rejection recovery phase."));
		UWorld* World = IMS4WorldPIE();
		if (!SpawnSource(World, 2))
			return BeginFailure(TEXT("Could not create and start ordinary MetaSound source C."));
		Stage = 6;
		StageAt = Now;
		if (!State->RecordPhase(TEXT("source_c_requested")))
			return BeginFailure(TEXT("Could not persist the source-C request phase."));
		return false;
	}

	bool ValidateCAllocation() const
	{
		if (!State->bCapturedAudioIdsBeforeC || !State->Context.IsValid()) return false;
		const FIMAcousticS4MultiSourceEntry& A = State->Sources[0];
		const FIMAcousticS4MultiSourceEntry& B = State->Sources[1];
		const FIMAcousticS4MultiSourceEntry& C = State->Sources[2];
		if (C.Slot < 0 || C.Slot >= int32(FIMAcousticMetaSoundContext::MaxVoices)
			|| C.Slot == A.Slot || C.Slot == B.Slot || !C.AudioId
			|| C.AudioId == A.AudioId || C.AudioId == B.AudioId
			|| State->AudioIdsBeforeC[uint32(C.Slot)] != 0
			|| State->Context->AudioIds[uint32(A.Slot)].load(std::memory_order_acquire) != A.AudioId
			|| State->Context->AudioIds[uint32(B.Slot)].load(std::memory_order_acquire) != B.AudioId
			|| State->Context->AudioIds[uint32(C.Slot)].load(std::memory_order_acquire) != C.AudioId)
			return false;
		for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
		{
			const uint64 Current = State->Context->AudioIds[Slot].load(std::memory_order_acquire);
			if (Slot == uint32(C.Slot))
			{
				if (State->AudioIdsBeforeC[Slot] != 0 || Current != C.AudioId) return false;
			}
			else if (Current != State->AudioIdsBeforeC[Slot])
			{
				return false;
			}
		}
		return true;
	}

	bool WaitForSourceCAndTeardown(double Now)
	{
		if (!NoSourceABlockAfterRelease())
			return BeginFailure(TEXT("An old A source block appeared after A release while C started."));
		FIMAcousticS4WorldStats CStats;
		FIMAcousticS4WorldStats BAfterRelease;
		if (!SourceReady(2, State->Sources[2].TraceBegin, Now, 8, CStats)) return false;
		if (!ReadSourceStats(1, State->AReleaseTraceBegin, BAfterRelease)) return false;
		const FIMAcousticS4MultiSourceEntry& B = State->Sources[1];
		const UAudioComponent* BAudio = B.Audio.Get();
		const bool bBConsumer = State->Context->SourceConsumers[uint32(B.Slot)].load(std::memory_order_acquire);
		if (!BAudio || !BAudio->IsPlaying() || !bBConsumer
			|| BAfterRelease.Accepted < 8 || BAfterRelease.Path == 0 || BAfterRelease.Output == 0
			|| !BAfterRelease.bVoiceConsistent || BAfterRelease.VoiceGeneration != B.VoiceGeneration
			|| State->Context->Device->Voices[B.Slot]->LiveGeneration.load(std::memory_order_acquire)
				!= B.StartLiveGeneration)
		{
			if (Now - StageAt > IMS4MultiSourcePhaseTimeoutSeconds)
				return BeginFailure(TEXT("B stopped or lost accepted nonzero graph output after C joined."));
			return false;
		}
		if (!ValidateCAllocation())
			return BeginFailure(TEXT("C did not receive a previously empty source slot while A's slot identity remained reserved."));
		if (!SourceRendererCountIs(State->SourceRendererBaseline + 2, TEXT("A released, B/C active"))) return false;
		if (!SharedConsumersReady(Now)) return false;
		if (!State->RecordPhase(TEXT("source_a_released_b_c_active")))
			return BeginFailure(TEXT("Could not persist the final active multi-source phase."));
		if (GUnrealEd) GUnrealEd->RequestEndPlayMap();
		Stage = 7;
		StageAt = Now;
		if (!State->RecordPhase(TEXT("pie_teardown_requested")))
			return BeginFailure(TEXT("Could not persist the PIE teardown request phase."));
		return false;
	}

	bool WorldResourcesReleased(double Now)
	{
		const FIMAcousticMetaSoundContextPtr& Context = State->Context;
		if (!Context.IsValid() || !Context->Device.IsValid() || !Context->Pool.IsValid())
			return BeginFailure(TEXT("Retained W1 context lost its device or pool before teardown verification."));
		bool bSourceAlive = false;
		for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
			bSourceAlive |= Context->SourceConsumers[Slot].load(std::memory_order_acquire);
		const bool bReleased = Context->Stopped.load(std::memory_order_acquire)
			&& !Context->Device->Alive.load(std::memory_order_acquire)
			&& Context->Pool->Stopped.load(std::memory_order_acquire)
			&& !bSourceAlive
			&& !Context->EnvironmentConsumer.load(std::memory_order_acquire)
			&& !Context->BusReaderConsumer.load(std::memory_order_acquire)
			&& Context->Device->ReverbEffectInstances.load(std::memory_order_acquire) == 0
			&& Context->Device->SourceRendererInstances.load(std::memory_order_acquire) == 0;
		if (!bReleased)
		{
			if (Now - State->TeardownObservedAt > 15.0)
				return BeginFailure(TEXT("World teardown left a source/environment consumer, reverb effect, device, pool, or context live."));
			return false;
		}
		const FIMAcousticMetaSoundContextPtr Registered = IMAcousticMetaSound::FindAcousticMetaSoundContext(uint32(State->DeviceId));
		if (Registered.IsValid() && Registered.Get() == Context.Get())
		{
			if (Now - State->TeardownObservedAt > 15.0)
				return BeginFailure(TEXT("The stopped W1 context remained registered for its audio device."));
			return false;
		}
		if (Now - State->TeardownObservedAt < IMS4MultiSourceReleaseQuietSeconds) return false;
		if (Context->Device->RenderedBlocks.load(std::memory_order_acquire) != State->TeardownRenderedBlocks
			|| Context->Device->RejectedBlocks.load(std::memory_order_acquire) != State->TeardownRejectedBlocks
			|| Context->SourceBlocks.load(std::memory_order_acquire) != State->TeardownSourceBlocks
			|| Context->EnvironmentBlocks.load(std::memory_order_acquire) != State->TeardownEnvironmentBlocks
			|| IMS4WorldSourceTraceCount(Context) != State->TeardownSourceTraceCount
			|| IMS4WorldEnvironmentTraceCount(Context) != State->TeardownEnvironmentTraceCount)
			return BeginFailure(TEXT("A stopped W1 graph continued publishing counters or trace entries after teardown."));
		State->bWorldResourcesReleased = true;
		return true;
	}

	bool WaitForWorldTeardown(double Now)
	{
		if (IMS4WorldPIE())
		{
			if (Now - StageAt > IMS4MultiSourceTeardownTimeoutSeconds)
				return BeginFailure(TEXT("PIE did not end after the multi-source teardown request."));
			return false;
		}
		if (!State->bTeardownObserved)
		{
			State->bTeardownObserved = true;
			State->TeardownObservedAt = Now;
			if (State->Context.IsValid() && State->Context->Device.IsValid())
			{
				State->TeardownRenderedBlocks = State->Context->Device->RenderedBlocks.load(std::memory_order_acquire);
				State->TeardownRejectedBlocks = State->Context->Device->RejectedBlocks.load(std::memory_order_acquire);
				State->TeardownSourceBlocks = State->Context->SourceBlocks.load(std::memory_order_acquire);
				State->TeardownEnvironmentBlocks = State->Context->EnvironmentBlocks.load(std::memory_order_acquire);
				State->TeardownSourceTraceCount = IMS4WorldSourceTraceCount(State->Context);
				State->TeardownEnvironmentTraceCount = IMS4WorldEnvironmentTraceCount(State->Context);
			}
			if (!State->RecordPhase(TEXT("pie_ended_release_pending")))
				return BeginFailure(TEXT("Could not persist the observed PIE end phase."));
		}
		if (!WorldResourcesReleased(Now)) return false;
		if (!State->RecordPhase(TEXT("world_resources_released")))
			return BeginFailure(TEXT("Could not persist world resource-release evidence."));
		if (!RestoreOriginalMap())
			State->AddFailure(TEXT("Original editor map or persistent map/source/bake assets did not restore unchanged."));
		State->ResultMessage = TEXT("Three ordinary MetaSound sources used distinct monotonic slots; A was released without stale blocks, B continued, C used a previously empty slot, and W1 teardown released its shared resources.");
		return Finalize();
	}

	bool NoSourceTraceOverflow() const
	{
		return State->Context.IsValid()
			&& State->Context->CapturedSourceBlockClaimCount.load(std::memory_order_acquire)
				<= uint32(State->Context->CapturedSourceBlocks.Num())
			&& State->Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire)
				<= uint32(State->Context->CapturedBlocks.Num());
	}

	bool BeginFailure(const FString& Why)
	{
		if (Stage == 90) return false;
		State->AddFailure(Why);
		State->ResultMessage = Why;
		if (!State->RecordPhase(TEXT("failure")))
			State->AddFailure(TEXT("Could not persist the failure phase."));
		for (FIMAcousticS4MultiSourceEntry& Entry : State->Sources)
		{
			if (UAudioComponent* Audio = Entry.Audio.Get()) Audio->Stop();
			if (Entry.Actor.IsValid()) Entry.Actor->Destroy();
		}
		State->RestoreGlobals();
		if (GUnrealEd && IMS4WorldPIE()) GUnrealEd->RequestEndPlayMap();
		Stage = 90;
		StageAt = FPlatformTime::Seconds();
		return false;
	}

	bool CleanupAndRestore(double Now)
	{
		if (IMS4WorldPIE())
		{
			if (Now - StageAt > IMS4MultiSourceTeardownTimeoutSeconds)
			{
				State->AddFailure(TEXT("PIE did not end during fail-closed cleanup; original map restoration was skipped."));
				State->RestoreGlobals();
				State->bFinalized = true;
				State->bPassed = false;
				State->ResultMessage = State->Failure;
				State->RecordPhase(TEXT("cleanup_timeout"));
				ReportFinal();
				return true;
			}
			return false;
		}
		if (State->Context.IsValid() && !State->bWorldResourcesReleased)
		{
			if (!State->Context->Device.IsValid() || !State->Context->Pool.IsValid())
			{
				State->AddFailure(TEXT("W1 context lost its device or pool during cleanup."));
			}
			else if (State->Context->Stopped.load(std::memory_order_acquire)
				&& !State->Context->Device->Alive.load(std::memory_order_acquire)
				&& State->Context->Pool->Stopped.load(std::memory_order_acquire))
			{
				if (!State->bTeardownObserved)
				{
					State->bTeardownObserved = true;
					State->TeardownObservedAt = Now;
					State->TeardownRenderedBlocks = State->Context->Device->RenderedBlocks.load(std::memory_order_acquire);
					State->TeardownRejectedBlocks = State->Context->Device->RejectedBlocks.load(std::memory_order_acquire);
					State->TeardownSourceBlocks = State->Context->SourceBlocks.load(std::memory_order_acquire);
					State->TeardownEnvironmentBlocks = State->Context->EnvironmentBlocks.load(std::memory_order_acquire);
					State->TeardownSourceTraceCount = IMS4WorldSourceTraceCount(State->Context);
					State->TeardownEnvironmentTraceCount = IMS4WorldEnvironmentTraceCount(State->Context);
				}
				if (!WorldResourcesReleased(Now) && Now - State->TeardownObservedAt <= 15.0) return false;
				if (!State->bWorldResourcesReleased)
					State->AddFailure(TEXT("W1 resources did not satisfy the teardown release contract during cleanup."));
			}
			else if (Now - StageAt <= 15.0)
			{
				return false;
			}
			else
			{
				State->AddFailure(TEXT("W1 context/device/pool remained live during cleanup."));
			}
		}
		if (!State->bMapRestored && !RestoreOriginalMap())
			State->AddFailure(TEXT("Original editor map or persistent assets could not be restored during cleanup."));
		State->ResultMessage = State->Failure;
		return Finalize();
	}

	bool RestoreOriginalMap()
	{
		if (IMS4WorldPIE() || !GEditor) return false;
		UWorld* Current = GEditor->GetEditorWorldContext().World();
		if (!Current || !Current->GetOutermost()) return false;
		UPackage* CurrentPackage = Current->GetOutermost();
		if (CurrentPackage->IsDirty())
		{
			State->AddFailure(TEXT("Current editor map became dirty; refusing to discard it during restoration."));
			return false;
		}
		if (CurrentPackage->GetName() != State->OriginalMap
			&& !FEditorFileUtils::LoadMap(*State->OriginalMap, false, true))
			return false;
		UWorld* Restored = GEditor->GetEditorWorldContext().World();
		State->bMapRestored = Restored && Restored->GetOutermost()
			&& Restored->GetOutermost()->GetName() == State->OriginalMap
			&& !Restored->GetOutermost()->IsDirty();
		State->OriginalMapHashAfter = IMS4WorldHashFile(State->OriginalMapDiskPath);
		State->TestMapHashAfter = IMS4WorldHashFile(State->TestMapDiskPath);
		State->SourceHashAfter = IMS4WorldHashFile(State->SourceDiskPath);
		State->BakeAssetHashAfter = IMS4WorldHashFile(State->BakeAssetDiskPath);
		UPackage* SourcePackage = FindPackage(nullptr, IMAcousticTestSupport::GraphSourceAssetPath);
		UPackage* TestMapPackage = FindPackage(nullptr, IMS4WorldMap);
		const FString BakePackageName = FPackageName::ObjectPathToPackageName(State->BakeAssetPath);
		UPackage* BakePackage = BakePackageName.IsEmpty() ? nullptr : FindPackage(nullptr, *BakePackageName);
		State->bAssetsUnchanged = State->OriginalMapHashBefore == State->OriginalMapHashAfter
			&& !State->OriginalMapHashBefore.IsEmpty()
			&& State->TestMapHashBefore == State->TestMapHashAfter
			&& !State->TestMapHashBefore.IsEmpty()
			&& State->SourceHashBefore == State->SourceHashAfter
			&& !State->SourceHashBefore.IsEmpty()
			&& State->BakeAssetHashBefore == State->BakeAssetHashAfter
			&& !State->BakeAssetHashBefore.IsEmpty()
			&& (!SourcePackage || !SourcePackage->IsDirty())
			&& (!TestMapPackage || !TestMapPackage->IsDirty())
			&& (!BakePackage || !BakePackage->IsDirty());
		return State->bMapRestored && State->bAssetsUnchanged;
	}

	bool Finalize()
	{
		if (State->bFinalized) return true;
		State->RestoreGlobals();
		if (!State->bWorldResourcesReleased && State->Context.IsValid())
			State->AddFailure(TEXT("W1 resource teardown was not verified."));
		if (!State->bMapRestored) State->AddFailure(TEXT("Original editor map was not restored cleanly."));
		if (!State->bAssetsUnchanged) State->AddFailure(TEXT("Persistent map/source/bake asset hashes or dirty state changed."));
		State->bPassed = State->Failure.IsEmpty();
		State->bFinalized = true;
		if (!State->RecordPhase(State->bPassed ? TEXT("completed") : TEXT("final_failure")))
		{
			State->AddFailure(TEXT("Could not persist final multi-source lifecycle evidence."));
			State->bPassed = false;
			State->SaveSummary();
		}
		ReportFinal();
		return true;
	}

	void ReportFinal() const
	{
		if (State->Test)
		{
			if (State->bPassed)
				State->Test->AddInfo(FString::Printf(TEXT("S4 multi-source lifecycle complete; evidence=%s"), *State->EvidenceDirectory));
			else
				State->Test->AddError(FString::Printf(TEXT("S4 multi-source lifecycle failed: %s; evidence=%s"),
					*State->Failure, *State->EvidenceDirectory));
		}
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS4MultiSourceWorldLifecycle %s reason=%s evidence=%s"),
			State->bPassed ? TEXT("PASS") : TEXT("FAIL"), *State->Failure, *State->EvidenceDirectory);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticS4MultiSourceWorldLifecycle %s evidence=%s"),
			State->bPassed ? TEXT("PASS") : TEXT("FAIL"), *State->EvidenceDirectory);
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), State->bPassed ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] IMExitEditor %s"), State->bPassed ? TEXT("PASS") : TEXT("FAIL"));
	}

	TSharedRef<FIMAcousticS4MultiSourceState> State;
	double StartedAt = 0.0;
	double StageAt = 0.0;
	int32 Stage = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticS4MultiSourceLifecycleTest,
	"IceMoon.AcousticField.S4.MultiSourceWorldLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIMAcousticS4MultiSourceLifecycleTest::RunTest(const FString&)
{
	TSharedRef<IMAcousticS4WorldTransitionTestPrivate::FIMAcousticS4MultiSourceState> State = MakeShared<IMAcousticS4WorldTransitionTestPrivate::FIMAcousticS4MultiSourceState>();
	State->Test = this;
	State->RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	State->EvidenceDirectory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AcousticV2/S4-MultiSourceWorldLifecycle"), State->RunId);
	State->bGlobalsRestored = true;
	State->Sources.SetNum(3);
	State->Sources[0].Name = TEXT("A");
	State->Sources[1].Name = TEXT("B");
	State->Sources[2].Name = TEXT("C");
	if (!IFileManager::Get().MakeDirectory(*State->EvidenceDirectory, true))
	{
		AddError(TEXT("Could not create the S4 multi-source evidence directory."));
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] AcousticS4MultiSourceWorldLifecycle INCONCLUSIVE evidence directory unavailable."));
		return false;
	}
	const auto FailBeforePIE = [this, State](const FString& Reason)
	{
		State->AddFailure(Reason);
		State->ResultMessage = Reason;
		State->bPassed = false;
		State->bFinalized = true;
		State->RecordPhase(TEXT("preflight_failed"));
		State->SaveSummary();
		AddError(FString::Printf(TEXT("%s; evidence=%s"), *Reason, *State->EvidenceDirectory));
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] AcousticS4MultiSourceWorldLifecycle INCONCLUSIVE %s evidence=%s"),
			*Reason, *State->EvidenceDirectory);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] IMExitEditor INCONCLUSIVE"));
		return false;
	};

	if (!GEditor || !GUnrealEd)
		return FailBeforePIE(TEXT("Multi-source W1 lifecycle automation requires an active Unreal Editor."));
	if (IMAcousticS4WorldTransitionTestPrivate::IMS4WorldPIE())
		return FailBeforePIE(TEXT("Refusing to replace an already-running PIE world."));
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld || !EditorWorld->GetOutermost() || EditorWorld->GetOutermost()->IsDirty())
		return FailBeforePIE(TEXT("Current editor map is missing or dirty; refusing to switch maps."));
	State->OriginalMap = EditorWorld->GetOutermost()->GetName();
	if (!FPackageName::IsValidLongPackageName(State->OriginalMap)
		|| !FPackageName::DoesPackageExist(State->OriginalMap))
		return FailBeforePIE(TEXT("Current editor map cannot be restored from a saved package."));
	if (!FPackageName::DoesPackageExist(IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap))
		return FailBeforePIE(TEXT("Existing read-only W1 doorway map is missing."));
	if (UPackage* LoadedTestMap = FindPackage(nullptr, IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap); LoadedTestMap && LoadedTestMap->IsDirty())
		return FailBeforePIE(TEXT("W1 test map is already dirty; refusing to discard map changes."));

	USoundBase* GraphSource = LoadObject<USoundBase>(nullptr, IMAcousticTestSupport::GraphSourceAssetPath);
	if (!GraphSource || !IMAcousticMetaSound::IsAcousticMetaSound(GraphSource) || GraphSource->GetOutermost()->IsDirty())
		return FailBeforePIE(TEXT("Ordinary MetaSound graph source is missing, invalid, or dirty."));
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("IceMoonAcousticField"));
	if (!Plugin.IsValid())
		return FailBeforePIE(TEXT("Could not resolve the acoustic plugin for read-only source fingerprinting."));

	State->OriginalMapDiskPath = FPackageName::LongPackageNameToFilename(
		State->OriginalMap, FPackageName::GetMapPackageExtension());
	State->TestMapDiskPath = FPackageName::LongPackageNameToFilename(
		IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap, FPackageName::GetMapPackageExtension());
	State->SourceDiskPath = FPaths::Combine(Plugin->GetBaseDir(),
		TEXT("Content/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay.uasset"));
	State->OriginalMapHashBefore = IMAcousticS4WorldTransitionTestPrivate::IMS4WorldHashFile(State->OriginalMapDiskPath);
	State->TestMapHashBefore = IMAcousticS4WorldTransitionTestPrivate::IMS4WorldHashFile(State->TestMapDiskPath);
	State->SourceHashBefore = IMAcousticS4WorldTransitionTestPrivate::IMS4WorldHashFile(State->SourceDiskPath);
	if (State->OriginalMapHashBefore.IsEmpty() || State->TestMapHashBefore.IsEmpty() || State->SourceHashBefore.IsEmpty())
		return FailBeforePIE(TEXT("Could not fingerprint the original map, read-only W1 map, or ordinary MetaSound asset."));
	if (!State->RecordPhase(TEXT("preflight_passed")))
		return FailBeforePIE(TEXT("Could not persist the multi-source preflight phase."));

	State->OriginalBackgroundVolume = FApp::GetUnfocusedVolumeMultiplier();
	State->bOriginalAllowBackgroundAudio = GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	State->bGlobalsRestored = false;
	FApp::SetUnfocusedVolumeMultiplier(1.0f);
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true,
		IMAcousticS4WorldTransitionTestPrivate::IMS4MultiSourceTraceCapacity, IMAcousticS4WorldTransitionTestPrivate::IMS4MultiSourceEnvironmentTraceCapacity, false);

	FString Error;
	GUnrealEd->AutomationLoadMap(IMAcousticS4WorldTransitionTestPrivate::IMS4WorldMap, false, &Error);
	if (!Error.IsEmpty())
	{
		State->AddFailure(Error);
		State->ResultMessage = Error;
		State->RestoreGlobals();
		State->bPassed = false;
		State->bFinalized = true;
		State->RecordPhase(TEXT("map_queue_failed"));
		State->SaveSummary();
		AddError(FString::Printf(TEXT("%s; evidence=%s"), *Error, *State->EvidenceDirectory));
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] AcousticS4MultiSourceWorldLifecycle FAIL %s evidence=%s"),
			*Error, *State->EvidenceDirectory);
		UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] IMExitEditor FAIL"));
		return false;
	}
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<IMAcousticS4WorldTransitionTestPrivate::FIMAcousticS4MultiSourceLifecycleCommand>(State));
	return true;
}

#endif
