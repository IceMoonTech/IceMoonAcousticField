#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "Audio.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioMixerBlueprintLibrary.h"
#include "AudioMixerDevice.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "UnrealEdGlobals.h"

namespace IMAcousticS4ReplayTestPrivate
{
constexpr const TCHAR* IMS4ReplayMap = TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
constexpr uint32 IMS4TailSilentBlocks = 8;
constexpr double IMS4TailTimeoutSeconds = 15.0;
constexpr double IMS4PlaybackTimeoutSeconds = 25.0;
constexpr float IMS4WetSilenceThreshold = 1.0e-9f;
constexpr int32 IMS4MasterSampleRate = 48000;
constexpr int32 IMS4MasterPcm16SilenceThreshold = 1;
constexpr int32 IMS4MasterMinimumTailSilenceMs = 100;
constexpr double IMS4MasterReplayActiveCaptureSeconds = 0.15;
constexpr double IMS4MasterWaveSettleTimeoutSeconds = 30.0;
constexpr const TCHAR* IMS4MasterAlignmentNote = TEXT("WAV frame offsets are measured within each exported file only. Game-thread event timestamps do not identify exact audio frames, and graph PCM frames are not mapped to master WAV frames.");

struct FIMAcousticS4MasterWaveSummary
{
	bool bParsed = false;
	FString Error;
	int64 FileBytes = -1;
	int32 SampleRate = 0;
	int32 Channels = 0;
	int32 Frames = 0;
	int32 FirstNonSilentFrame = INDEX_NONE;
	int32 LastNonSilentFrame = INDEX_NONE;
	int32 LeadingSilentFrames = 0;
	int32 TrailingSilentFrames = 0;
	int32 ActiveFrames = 0;
	int32 PeakAbsPcm16 = 0;
	int64 NonSilentFrames = 0;
	int64 OverloadSamples = 0;
	double Energy = 0.0;
};

bool IM_S4ReadMasterWave(const FString& WavPath, int32 ExpectedSampleRate, FIMAcousticS4MasterWaveSummary& Out)
{
	Out = {};
	Out.FileBytes = IFileManager::Get().FileSize(*WavPath);
	TArray<uint8> Bytes;
	FWaveModInfo Info;
	if (!FFileHelper::LoadFileToArray(Bytes, *WavPath)
		|| !Info.ReadWaveInfo(Bytes.GetData(), Bytes.Num())
		|| !Info.pBitsPerSample || *Info.pBitsPerSample != 16
		|| !Info.pChannels || *Info.pChannels != 2
		|| !Info.pSamplesPerSec || *Info.pSamplesPerSec != ExpectedSampleRate
		|| !Info.pFormatTag || *Info.pFormatTag != FWaveModInfo::WAVE_INFO_FORMAT_PCM
		|| !Info.SampleDataStart || Info.SampleDataSize == 0 || Info.SampleDataSize % 4 != 0)
	{
		Out.Error = TEXT("Master recording must be a readable 48 kHz stereo PCM16 WAV with complete frames.");
		return false;
	}

	Out.SampleRate = *Info.pSamplesPerSec;
	Out.Channels = *Info.pChannels;
	Out.Frames = Info.SampleDataSize / 4;
	for (int32 Frame = 0; Frame < Out.Frames; ++Frame)
	{
		int16 Left = 0;
		int16 Right = 0;
		const int64 SampleByteOffset = int64(Frame) * 4;
		FMemory::Memcpy(&Left, Info.SampleDataStart + SampleByteOffset, sizeof(int16));
		FMemory::Memcpy(&Right, Info.SampleDataStart + SampleByteOffset + sizeof(int16), sizeof(int16));
		const int32 AbsLeft = FMath::Abs(int32(Left));
		const int32 AbsRight = FMath::Abs(int32(Right));
		Out.PeakAbsPcm16 = FMath::Max(Out.PeakAbsPcm16, FMath::Max(AbsLeft, AbsRight));
		Out.Energy += double(Left) * Left + double(Right) * Right;
		Out.OverloadSamples += (AbsLeft >= 32760 ? 1 : 0) + (AbsRight >= 32760 ? 1 : 0);
		if (AbsLeft > IMS4MasterPcm16SilenceThreshold || AbsRight > IMS4MasterPcm16SilenceThreshold)
		{
			if (Out.FirstNonSilentFrame == INDEX_NONE) Out.FirstNonSilentFrame = Frame;
			Out.LastNonSilentFrame = Frame;
			++Out.NonSilentFrames;
		}
	}

	Out.LeadingSilentFrames = Out.FirstNonSilentFrame == INDEX_NONE ? Out.Frames : Out.FirstNonSilentFrame;
	Out.TrailingSilentFrames = Out.LastNonSilentFrame == INDEX_NONE
		? Out.Frames : Out.Frames - Out.LastNonSilentFrame - 1;
	Out.ActiveFrames = Out.FirstNonSilentFrame == INDEX_NONE
		? 0 : Out.LastNonSilentFrame - Out.FirstNonSilentFrame + 1;
	Out.bParsed = true;
	return true;
}

void IM_S4WriteMasterWaveSummary(
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer,
	const FIMAcousticS4MasterWaveSummary& Wave)
{
	Writer->WriteValue(TEXT("parsed"), Wave.bParsed);
	Writer->WriteValue(TEXT("parse_error"), Wave.Error);
	Writer->WriteValue(TEXT("file_bytes"), double(Wave.FileBytes));
	Writer->WriteValue(TEXT("sample_rate_hz"), Wave.SampleRate);
	Writer->WriteValue(TEXT("channels"), Wave.Channels);
	Writer->WriteValue(TEXT("frames"), double(Wave.Frames));
	Writer->WriteValue(TEXT("first_non_silent_frame"), Wave.FirstNonSilentFrame);
	Writer->WriteValue(TEXT("last_non_silent_frame"), Wave.LastNonSilentFrame);
	Writer->WriteValue(TEXT("leading_silent_frames"), Wave.LeadingSilentFrames);
	Writer->WriteValue(TEXT("trailing_silent_frames"), Wave.TrailingSilentFrames);
	Writer->WriteValue(TEXT("active_frames"), Wave.ActiveFrames);
	Writer->WriteValue(TEXT("non_silent_frames"), double(Wave.NonSilentFrames));
	Writer->WriteValue(TEXT("peak_abs_pcm16"), Wave.PeakAbsPcm16);
	Writer->WriteValue(TEXT("overload_samples_at_32760"), double(Wave.OverloadSamples));
	Writer->WriteValue(TEXT("energy_pcm16_squared"), Wave.Energy);
}

struct FIMAcousticS4ReplayState
{
	FAutomationTestBase* Test = nullptr;
	FString Directory;
	double Started = 0.0;
	float OriginalBackgroundVolume = 1.0f;
	bool bOriginalBackgroundAudio = false;
};

struct FIMAcousticS4ReplayWindow
{
	uint64 AudioId = 0;
	uint64 WorldGeneration = 0;
	uint64 VoiceGeneration = 0;
	uint32 SourceTraceBegin = 0;
	uint32 SourceTraceEnd = 0;
	uint32 EnvironmentTraceBegin = 0;
	uint32 EnvironmentTraceEnd = 0;
	uint32 SourcePcmBegin = 0;
	uint32 SourcePcmEnd = 0;
	uint32 EnvironmentPcmBegin = 0;
	uint32 EnvironmentPcmEnd = 0;
	int32 Slot = INDEX_NONE;
	uint64 MatchingSourceBlocks = 0;
	uint64 AcceptedIdentityBlocks = 0;
	uint64 PathNonzeroBlocks = 0;
	uint64 OutputNonzeroBlocks = 0;
	uint64 NonzeroInputBlocks = 0;
	bool bVoiceGenerationConsistent = true;
};

struct FIMAcousticS4WindowStats
{
	uint64 Matching = 0;
	uint64 Accepted = 0;
	uint64 PathNonzero = 0;
	uint64 OutputNonzero = 0;
	uint64 InputNonzero = 0;
	uint64 VoiceGeneration = 0;
	uint64 SpatialVersion = 0;
	bool bVoiceGenerationConsistent = true;
};

UWorld* IM_FindS4PIEWorld()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

int32 IM_FindS4Slot(const FIMAcousticMetaSoundContextPtr& Context, uint64 AudioId)
{
	if (!Context || !AudioId) return INDEX_NONE;
	for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
	{
		if (Context->AudioIds[Slot].load(std::memory_order_acquire) == AudioId)
		{
			return int32(Slot);
		}
	}
	return INDEX_NONE;
}

bool IM_S4IdentityProbePass(const FIMAcousticIdentityInjectionProbe& Probe, uint32 RequiredDetail)
{
	return Probe.Observed.load(std::memory_order_acquire) != 0
		&& Probe.Rejected.load(std::memory_order_acquire) != 0
		&& Probe.FiniteOutput.load(std::memory_order_acquire) != 0
		&& (Probe.RejectDetail.load(std::memory_order_acquire) & RequiredDetail) == RequiredDetail
		&& Probe.RejectedDelta.load(std::memory_order_acquire) == 1
		&& Probe.RenderedDelta.load(std::memory_order_acquire) == 0;
}

class FIMAcousticS4ReplayCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticS4ReplayCommand(TSharedRef<FIMAcousticS4ReplayState> InState)
		: State(InState)
	{
	}

	bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - State->Started > 105.0)
		{
			return Finish(false, FString::Printf(TEXT("S4 replay timed out at stage %d."), Stage));
		}

		UWorld* World = IM_FindS4PIEWorld();
		if (!World)
		{
			if (Stage == 0) return false;
			return Finish(false, TEXT("PIE world ended before S4 replay evidence completed."));
		}

		if (Stage == 0)
		{
			Listener = World->GetFirstPlayerController();
			if (!Listener.IsValid()) return false;
			for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It) Volume = *It;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (!It->ActorHasTag(TEXT("IMAcousticAuditionReferenceV1"))) continue;
				if (UIMAcousticSourceComponent* Marker = It->FindComponentByClass<UIMAcousticSourceComponent>())
				{
					Source = Marker->AudioComponent;
					break;
				}
			}
			if (!Volume.IsValid() || !Source.IsValid())
			{
				if (Now - State->Started > 20.0)
					return Finish(false, TEXT("IM_V2Audition did not expose its acoustic volume and tagged ordinary source."));
				return false;
			}

			FAudioDevice* AudioDevice = World->GetAudioDeviceRaw();
			if (!AudioDevice)
			{
				if (Now - State->Started > 20.0) return Finish(false, TEXT("PIE world has no audio device."));
				return false;
			}
			Context = IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
			if (!Context || !Context->Pool || !Context->Device || Context->Stopped.load(std::memory_order_acquire))
			{
				if (Now - State->Started > 20.0) return Finish(false, TEXT("Ordinary MetaSound context was not created for the audition world."));
				return false;
			}
			FString MixerError;
			if (!InitializeMasterMixer(World, MixerError))
				return Finish(false, MixerError);
			if (!IMAcousticMetaSound::IsAcousticMetaSound(Source->Sound) || Source->bAllowSpatialization)
				return Finish(false, TEXT("S4 source is not the ordinary isolated production MetaSound path."));

			Volume->bEnableV2 = true;
			// The audition map's volume authors legacy render-material mappings; the PhysicalMaterial default would leave its surfaces unmapped.
			// The volume lives in the PIE world copy, so the map asset itself is not changed.
			Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
			// Keep the ordinary audition route set for this lifecycle case.
			Volume->bDirectRoute = true;
			Volume->bPathRoute = true;
			Volume->bReverbRoute = true;
			Listener->SetAudioListenerOverride(nullptr, FVector(750, 300, 150), FRotator::ZeroRotator);
			if (!FApp::CanEverRender())
			{
				AudioDevice->SetListener(World, 0,
					FTransform(FRotator::ZeroRotator, FVector(750, 300, 150)), 0.0f);
			}

			if (!Source->IsPlaying()) Source->Play();
			Play1.AudioId = Source->GetAudioComponentID();
			Play1.WorldGeneration = Context->Pool->WorldGeneration;
			Play1.SourceTraceBegin = SourceTraceCount();
			Play1.EnvironmentTraceBegin = EnvironmentTraceCount();
			Play1.SourcePcmBegin = SourcePcmFrames();
			Play1.EnvironmentPcmBegin = EnvironmentPcmFrames();
			Play1.Slot = IM_FindS4Slot(Context, Play1.AudioId);
			Play1StartSeconds = Now;
			Stage = 1;
		}

		if (!Source.IsValid() || !Volume.IsValid() || !Context.IsValid())
			return Finish(false, TEXT("S4 source, volume, or graph context was destroyed during replay."));

		FAudioDevice* AudioDevice = World->GetAudioDeviceRaw();
		if (!AudioDevice || AudioDevice->DeviceID != Context->DeviceId)
			return Finish(false, TEXT("S4 replay changed the owning audio device inside the same PIE world."));

		if (Stage == 1)
		{
			const uint64 CurrentAudioId = Source->GetAudioComponentID();
			if (!Play1.AudioId && CurrentAudioId)
			{
				Play1.AudioId = CurrentAudioId;
				Play1.Slot = IM_FindS4Slot(Context, CurrentAudioId);
			}
			if (!Play1.AudioId || Play1.Slot == INDEX_NONE)
			{
				if (Now - Play1StartSeconds > 15.0)
					return Finish(false, TEXT("First play did not acquire its production AudioComponentID slot."));
				return false;
			}
			if (Play1.Slot != 0)
				return Finish(false, TEXT("Audition source is not slot zero; the production context exposes PCM only for slot zero."));

			FIMAcousticS4WindowStats Stats;
			InspectWindow(Play1.SourceTraceBegin, SourceTraceCount(), Play1.AudioId, Play1.WorldGeneration, Stats);
			const bool bWetObserved = HasWetEnergy(Play1.EnvironmentPcmBegin, EnvironmentPcmFrames());
			if (!Source->IsPlaying() || Stats.OutputNonzero < 8 || Stats.Accepted < 8
				|| Stats.InputNonzero == 0 || !Stats.VoiceGeneration || !bWetObserved)
			{
				if (Now - Play1StartSeconds > IMS4PlaybackTimeoutSeconds)
					return Finish(false, FString::Printf(
						TEXT("First play gate: playing=%d matching=%llu accepted=%llu output=%llu path=%llu input=%llu generation=%llu wet=%d source_frames=%u wet_frames=%u."),
						Source->IsPlaying() ? 1 : 0, Stats.Matching, Stats.Accepted, Stats.OutputNonzero, Stats.PathNonzero,
						Stats.InputNonzero, Stats.VoiceGeneration, bWetObserved ? 1 : 0,
						SourcePcmFrames(), EnvironmentPcmFrames()));
				return false;
			}

			Play1.VoiceGeneration = Stats.VoiceGeneration;
			Play1.SourceTraceEnd = SourceTraceCount();
			Play1.EnvironmentTraceEnd = EnvironmentTraceCount();
			Play1.SourcePcmEnd = SourcePcmFrames();
			Play1.EnvironmentPcmEnd = EnvironmentPcmFrames();
			Play1.MatchingSourceBlocks = Stats.Matching;
			Play1.AcceptedIdentityBlocks = Stats.Accepted;
			Play1.PathNonzeroBlocks = Stats.PathNonzero;
			Play1.OutputNonzeroBlocks = Stats.OutputNonzero;
			Play1.NonzeroInputBlocks = Stats.InputNonzero;
			Play1.bVoiceGenerationConsistent = Stats.bVoiceGenerationConsistent;
			if (!Play1.bVoiceGenerationConsistent)
				return Finish(false, TEXT("First play changed voice generation while the same graph voice was running."));

			StopSourceTraceAtRequest = SourceTraceCount();
			StopSourceFramesAtRequest = SourcePcmFrames();
			StopRequestSeconds = FPlatformTime::Seconds();
			Source->Stop();
			StopReturnSeconds = FPlatformTime::Seconds();
			StopSourceTraceAtReturn = SourceTraceCount();
			StopSourceFramesAtReturn = SourcePcmFrames();
			StopSeconds = StopReturnSeconds;
			StopWetFrameBegin = EnvironmentPcmFrames();
			TailScanFrame = StopWetFrameBegin;
			TailSilentRunFrames = 0;
			TailSawPostStopWet = false;
			StopSourceReleased = false;
			StopSourceFramesAtRelease = 0;
			LastSourceTraceScan = SourceTraceCount();
			Stage = 2;
			return false;
		}

		if (Stage == 2)
		{
			ScanStoppedSourceBlocks();
			ScanPostStopWet();

			if (!Context->SourceConsumers[Play1.Slot].load(std::memory_order_acquire)
				&& !StopSourceReleased)
			{
				StopSourceReleased = true;
				StopSourceReleaseSeconds = FPlatformTime::Seconds();
				StopSourceFramesAtRelease = SourcePcmFrames();
				StopSourceTraceAtRelease = SourceTraceCount();
				StopEnvironmentFramesAtRelease = EnvironmentPcmFrames();
				StopLiveGeneration = Context->Device->Voices[Play1.Slot]->LiveGeneration.load(std::memory_order_acquire);
			}
			if (StopSourceReleased && SourcePcmFrames() != StopSourceFramesAtRelease)
				return Finish(false, TEXT("Source PCM continued after the stopped MetaSound voice released its consumer slot."));
			if (StoppedSourceBlockObserved)
				return Finish(false, TEXT("A source block with the stopped AudioComponentID appeared after voice release."));

			if (Now - StopSeconds > IMS4TailTimeoutSeconds)
				return Finish(false, TEXT("Stopped source wet tail did not reach a stable silent PCM window."));

			if (StopSourceReleased && Now - StopSourceReleaseSeconds >= 0.10 && !bTailMasterRecordingStarted)
			{
				if (!StartMasterOutputRecording(World, TEXT("s4-stop-tail-master"), float(IMS4TailTimeoutSeconds + 1.0)))
					return Finish(false, TEXT("Could not start the owned stop-tail master-output recording."));
				bTailMasterRecordingStarted = true;
				TailFrameAtMasterStart = TailScanFrame;
				TailMasterStartSeconds = MasterOutputStartSeconds;
				TailMasterReleaseGuardSeconds = TailMasterStartSeconds - StopSourceReleaseSeconds;
			}

			const bool bTailGraphGateNow = StopSourceReleased && !Source->IsPlaying() && TailSawPostStopWet
				&& TailSilentRunFrames >= IMS4TailSilentBlocks * FIMAcousticMetaSoundContext::Frames
				&& Now - StopSourceReleaseSeconds >= 0.10;
			if (bTailGraphGateNow)
			{
				if (StopLiveGeneration <= Play1.VoiceGeneration)
					return Finish(false, TEXT("Stopping the first graph voice did not advance its live generation."));
				if (!HasWetEnergy(StopEnvironmentFramesAtRelease, TailScanFrame))
					return Finish(false, TEXT("The environment wet tail ended before the stopped source consumer was released."));
				if (!bTailGraphSilenceGatePassed)
				{
					bTailGraphSilenceGatePassed = true;
					TailGraphSilenceGateSeconds = Now;
				}
			}
			else
			{
				bTailGraphSilenceGatePassed = false;
				TailGraphSilenceGateSeconds = 0.0;
			}

			const double MixerBlockSeconds = double(MasterMixerBlockFrames) / MasterMixerSampleRateHz;
			if (bTailMasterRecordingStarted && bTailGraphSilenceGatePassed
				&& Now - TailMasterStartSeconds >= 0.10
				&& Now - TailGraphSilenceGateSeconds >= 3.0 * MixerBlockSeconds)
			{
				TailDrainFrameEnd = TailScanFrame;
				Play1.EnvironmentTraceEnd = EnvironmentTraceCount();
				Play1.EnvironmentPcmEnd = StopWetFrameBegin;
				if (!StopMasterOutputRecording(TailMasterWavPath))
					return Finish(false, TEXT("Could not stop the owned stop-tail master-output recording."));
				TailMasterStopSeconds = MasterOutputStopSeconds;
				TailMasterPostGateGuardSeconds = TailMasterStopSeconds - TailGraphSilenceGateSeconds;
				Stage = 6;
			}
			return false;
		}

		if (Stage == 6)
		{
			bool bStable = false;
			if (!PollMasterWaveStable(TailMasterWavPath, Now, bStable))
				return Finish(false, TEXT("The stop-tail master WAV did not settle within 30 seconds."));
			if (!bStable) return false;
			if (!IM_S4ReadMasterWave(TailMasterWavPath, MasterSampleRate, TailMasterWave))
				return Finish(false, FString::Printf(TEXT("Could not parse stop-tail master WAV: %s"), *TailMasterWave.Error));
			const int32 RequiredTailSilenceFrames = FMath::CeilToInt(
				double(MasterSampleRate) * IMS4MasterMinimumTailSilenceMs / 1000.0);
			// The master WAV is 16-bit and its silence test needs a sample above one PCM16 step (IMS4MasterPcm16SilenceThreshold), so a wet tail whose
			// graph peak inside the recording window is below two steps cannot appear in it whatever the route does. The non-silence proof therefore
			// applies only when the graph tail is observable at that resolution; the trailing-silence proof always applies.
			TailGraphPeakInMasterWindow = PeakWetAmplitude(TailFrameAtMasterStart, TailDrainFrameEnd);
			bTailObservableInMaster = TailGraphPeakInMasterWindow >= float(IMS4MasterPcm16SilenceThreshold + 1) / 32768.0f;
			bTailMasterGatePassed = (TailMasterWave.NonSilentFrames > 0 || !bTailObservableInMaster)
				&& TailMasterWave.TrailingSilentFrames >= RequiredTailSilenceFrames;
			if (!bTailMasterGatePassed)
				return Finish(false, FString::Printf(
					TEXT("Stop-tail master WAV gate failed: non_silent_frames=%lld trailing_silent_frames=%d required=%d graph_wet_peak_during_master_window=%g graph_wet_peak_before_master_window=%g pcm16_step=%g."),
					TailMasterWave.NonSilentFrames, TailMasterWave.TrailingSilentFrames, RequiredTailSilenceFrames,
					PeakWetAmplitude(TailFrameAtMasterStart, TailDrainFrameEnd), PeakWetAmplitude(StopWetFrameBegin, TailFrameAtMasterStart),
					1.0 / 32768.0));

			if (!StartMasterOutputRecording(World, TEXT("s4-replay-master"), float(IMS4PlaybackTimeoutSeconds + 1.0)))
				return Finish(false, TEXT("Could not start the owned replay master-output recording after the tail WAV settled."));
			bReplayMasterRecordingStarted = true;
			ReplayMasterStartSeconds = MasterOutputStartSeconds;
			ReplayMasterPreRollGuardSeconds = FMath::Max(
				0.10, 4.0 * double(MasterMixerBlockFrames) / MasterMixerSampleRateHz);
			Stage = 7;
			return false;
		}

		if (Stage == 7)
		{
			if (!bMasterOutputRecording || !bReplayMasterRecordingStarted)
				return Finish(false, TEXT("Replay master recording ended before the same component could be replayed."));
			if (!IsMasterMixerUnchanged(World))
				return Finish(false, TEXT("Audio device or actual mixer block size changed during replay master pre-roll."));
			if (Now - ReplayMasterStartSeconds < ReplayMasterPreRollGuardSeconds)
				return false;

			Play2.AudioId = Source->GetAudioComponentID();
			if (Play2.AudioId != Play1.AudioId)
				return Finish(false, TEXT("AudioComponentID changed on the same component before replay."));
			Play2.WorldGeneration = Context->Pool->WorldGeneration;
			Play2.SourceTraceBegin = SourceTraceCount();
			Play2.EnvironmentTraceBegin = EnvironmentTraceCount();
			Play2.SourcePcmBegin = SourcePcmFrames();
			Play2.EnvironmentPcmBegin = EnvironmentPcmFrames();
			Play2.Slot = IM_FindS4Slot(Context, Play2.AudioId);
			if (Play2.Slot != Play1.Slot)
				return Finish(false, TEXT("The retained AudioComponentID moved to a different voice slot."));

			Source->Play();
			Play2.AudioId = Source->GetAudioComponentID();
			Play2StartSeconds = FPlatformTime::Seconds();
			Stage = 3;
			return false;
		}

		if (Stage == 3)
		{
			const uint64 CurrentAudioId = Source->GetAudioComponentID();
			if (CurrentAudioId != Play1.AudioId || Play2.WorldGeneration != Context->Pool->WorldGeneration)
				return Finish(false, TEXT("Replay changed AudioComponentID or WorldGeneration on the retained source."));

			FIMAcousticS4WindowStats Stats;
			InspectWindow(Play2.SourceTraceBegin, SourceTraceCount(), Play2.AudioId, Play2.WorldGeneration, Stats);
			const bool bWetObserved = HasWetEnergy(Play2.EnvironmentPcmBegin, EnvironmentPcmFrames());
			if (!Source->IsPlaying() || Stats.OutputNonzero < 8 || Stats.Accepted < 8
				|| Stats.InputNonzero == 0 || !Stats.VoiceGeneration || !bWetObserved)
			{
				if (bReplayFreshGatePassed && !Source->IsPlaying())
					return Finish(false, TEXT("Replay stopped before the post-acceptance master-output capture window completed."));
				if (Now - Play2StartSeconds > IMS4PlaybackTimeoutSeconds)
					return Finish(false, FString::Printf(
						TEXT("Replay gate: playing=%d matching=%llu accepted=%llu output=%llu path=%llu input=%llu generation=%llu wet=%d source_frames=%u wet_frames=%u."),
						Source->IsPlaying() ? 1 : 0, Stats.Matching, Stats.Accepted, Stats.OutputNonzero, Stats.PathNonzero,
						Stats.InputNonzero, Stats.VoiceGeneration, bWetObserved ? 1 : 0,
						SourcePcmFrames(), EnvironmentPcmFrames()));
				return false;
			}
			if (!Stats.bVoiceGenerationConsistent || Stats.VoiceGeneration <= StopLiveGeneration
				|| Stats.VoiceGeneration <= Play1.VoiceGeneration)
				return Finish(false, TEXT("Replay did not publish accepted source blocks from a fresh voice generation."));

			Play2.VoiceGeneration = Stats.VoiceGeneration;
			Play2.MatchingSourceBlocks = Stats.Matching;
			Play2.AcceptedIdentityBlocks = Stats.Accepted;
			Play2.PathNonzeroBlocks = Stats.PathNonzero;
			Play2.OutputNonzeroBlocks = Stats.OutputNonzero;
			Play2.NonzeroInputBlocks = Stats.InputNonzero;
			Play2.bVoiceGenerationConsistent = Stats.bVoiceGenerationConsistent;
			ReplaySpatialVersion = Stats.SpatialVersion;
			if (!ReplaySpatialVersion)
				return Finish(false, TEXT("Replay had no accepted spatial version for identity negative controls."));

			if (!bReplayFreshGatePassed)
			{
				// The audition source starts with about one second of sub-LSB samples before its audible part, and the master WAV is 16-bit,
				// so the active master window may only start once the graph wet reaches an amplitude the WAV can represent. The master gate
				// itself (non-silent frames, headroom, pre-roll silence) is unchanged.
				const float ObservableWetPeak = float(IMS4MasterPcm16SilenceThreshold + 1) / 32768.0f;
				const float ReplayWetPeak = PeakWetAmplitude(Play2.EnvironmentPcmBegin, EnvironmentPcmFrames());
				if (ReplayWetPeak < ObservableWetPeak)
				{
					if (Now - Play2StartSeconds > IMS4PlaybackTimeoutSeconds)
						return Finish(false, FString::Printf(
							TEXT("Replay graph wet never reached the PCM16-observable level: peak=%g required=%g."),
							ReplayWetPeak, ObservableWetPeak));
					return false;
				}
				bReplayFreshGatePassed = true;
				ReplayFreshGateSeconds = Now;
			}
			if (Now - ReplayFreshGateSeconds < IMS4MasterReplayActiveCaptureSeconds)
				return false;

			Play2.SourcePcmEnd = SourcePcmFrames();
			Play2.EnvironmentPcmEnd = EnvironmentPcmFrames();
			Play2.SourceTraceEnd = SourceTraceCount();
			Play2.EnvironmentTraceEnd = EnvironmentTraceCount();
			if (!StopMasterOutputRecording(ReplayMasterWavPath))
				return Finish(false, TEXT("Could not stop the owned replay master-output recording."));
			ReplayMasterStopSeconds = MasterOutputStopSeconds;
			Stage = 8;
			return false;
		}

		if (Stage == 8)
		{
			bool bStable = false;
			if (!PollMasterWaveStable(ReplayMasterWavPath, Now, bStable))
				return Finish(false, TEXT("The replay master WAV did not settle within 30 seconds."));
			if (!bStable) return false;
			if (!IM_S4ReadMasterWave(ReplayMasterWavPath, MasterSampleRate, ReplayMasterWave))
				return Finish(false, FString::Printf(TEXT("Could not parse replay master WAV: %s"), *ReplayMasterWave.Error));
			const int32 RequiredReplayPreRollFrames = int32(2 * MasterMixerBlockFrames);
			bReplayMasterGatePassed = ReplayMasterWave.NonSilentFrames > 0
				&& ReplayMasterWave.ActiveFrames > 0
				&& ReplayMasterWave.LeadingSilentFrames >= RequiredReplayPreRollFrames
				&& ReplayMasterWave.PeakAbsPcm16 < 32760;
			if (!bReplayMasterGatePassed)
				return Finish(false, FString::Printf(
					TEXT("Replay master WAV gate failed: leading_silent_frames=%d required=%d non_silent_frames=%lld active_frames=%d peak_abs_pcm16=%d overload_limit=32760 graph_wet_peak_replay_window=%g graph_wet_frames_replay_window=%u pcm16_step=%g."),
					ReplayMasterWave.LeadingSilentFrames, RequiredReplayPreRollFrames, ReplayMasterWave.NonSilentFrames,
					ReplayMasterWave.ActiveFrames, ReplayMasterWave.PeakAbsPcm16,
					PeakWetAmplitude(Play2.EnvironmentPcmBegin, Play2.EnvironmentPcmEnd),
					Play2.EnvironmentPcmEnd - Play2.EnvironmentPcmBegin, 1.0f / 32768.0f));

			Context->IdentityInjection.Request(
				1,
				Play2.AudioId + 1,
				Play2.WorldGeneration,
				Play2.VoiceGeneration,
				900101,
				0.0f, 0.0f, 0.0f,
				ReplaySpatialVersion);
			Stage = 4;
			return false;
		}

		if (Stage == 4)
		{
			const auto& Probe = Context->IdentityInjection.Probes[0];
			if (!Probe.Observed.load(std::memory_order_acquire)) return false;
			if (!IM_S4IdentityProbePass(Probe, 128u)
				|| Probe.InjectedAudioId.load(std::memory_order_acquire) == Probe.ActualAudioId.load(std::memory_order_acquire)
				|| Probe.ActualAudioId.load(std::memory_order_acquire) != Play2.AudioId
				|| Probe.ActualWorldGeneration.load(std::memory_order_acquire) != Play2.WorldGeneration
				|| Probe.ActualVoiceGeneration.load(std::memory_order_acquire) != Play2.VoiceGeneration)
				return Finish(false, TEXT("Bad AudioComponentID injection was not rejected without publication."));

			const uint64 SpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
			Context->IdentityInjection.Request(2, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, SpatialVersion);
			Stage = 5;
			return false;
		}

		if (Stage == 5)
		{
			const auto& Probe = Context->IdentityInjection.Probes[1];
			if (!Probe.Observed.load(std::memory_order_acquire)) return false;
			constexpr uint32 MissingFieldsMask = 16u | 64u | 128u;
			if (!IM_S4IdentityProbePass(Probe, MissingFieldsMask)
				|| Probe.InjectedAudioId.load(std::memory_order_acquire) != 0
				|| Probe.InjectedWorldGeneration.load(std::memory_order_acquire) != 0
				|| Probe.InjectedVoiceGeneration.load(std::memory_order_acquire) != 0
				|| Probe.ActualAudioId.load(std::memory_order_acquire) != Play2.AudioId
				|| Probe.ActualWorldGeneration.load(std::memory_order_acquire) != Play2.WorldGeneration
				|| Probe.ActualVoiceGeneration.load(std::memory_order_acquire) != Play2.VoiceGeneration)
				return Finish(false, TEXT("Missing identity fields were not rejected without publication."));

			Play2.SourcePcmEnd = SourcePcmFrames();
			Play2.EnvironmentPcmEnd = EnvironmentPcmFrames();
			Play2.SourceTraceEnd = SourceTraceCount();
			Play2.EnvironmentTraceEnd = EnvironmentTraceCount();
			const bool bSaved = SaveEvidence();
			Source->Stop();
			return Finish(bSaved, bSaved
				? TEXT("Same ordinary MetaSound component stopped, drained wet PCM, replayed with a fresh voice generation, and rejected bad/missing identity.")
				: TEXT("S4 replay predicates passed but evidence files could not be saved."));
		}

		return Finish(false, TEXT("S4 replay entered an unknown stage."));
	}

private:
	bool InitializeMasterMixer(UWorld* World, FString& OutError)
	{
		FAudioDevice* AudioDevice = World ? World->GetAudioDeviceRaw() : nullptr;
		Audio::FMixerDevice* Mixer = World
			? FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(World) : nullptr;
		if (!AudioDevice || !Mixer || AudioDevice->DeviceID != Context->DeviceId)
		{
			OutError = TEXT("S4 master-output evidence requires the same PIE audio device and an active AudioMixer device.");
			return false;
		}
		const float MixerSampleRate = Mixer->GetSampleRate();
		const uint32 MixerBlockFrames = Mixer->GetNumOutputFrames();
		const int32 DeviceSampleRate = FMath::RoundToInt(AudioDevice->GetSampleRate());
		if (!FMath::IsFinite(MixerSampleRate) || FMath::RoundToInt(MixerSampleRate) != IMS4MasterSampleRate
			|| DeviceSampleRate != IMS4MasterSampleRate || MixerBlockFrames == 0)
		{
			OutError = FString::Printf(
				TEXT("S4 master WAV evidence requires a 48 kHz device with a nonzero actual mixer block; device_rate=%d mixer_rate=%g mixer_block_frames=%u."),
				DeviceSampleRate, MixerSampleRate, MixerBlockFrames);
			return false;
		}
		MasterAudioDeviceId = AudioDevice->DeviceID;
		MasterSampleRate = DeviceSampleRate;
		MasterMixerSampleRateHz = MixerSampleRate;
		MasterMixerBlockFrames = MixerBlockFrames;
		return true;
	}

	bool IsMasterMixerUnchanged(UWorld* World) const
	{
		FAudioDevice* AudioDevice = World ? World->GetAudioDeviceRaw() : nullptr;
		Audio::FMixerDevice* Mixer = World
			? FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(World) : nullptr;
		return AudioDevice && Mixer
			&& AudioDevice->DeviceID == MasterAudioDeviceId
			&& FMath::RoundToInt(AudioDevice->GetSampleRate()) == MasterSampleRate
			&& FMath::IsNearlyEqual(Mixer->GetSampleRate(), MasterMixerSampleRateHz)
			&& Mixer->GetNumOutputFrames() == MasterMixerBlockFrames;
	}

	bool StartMasterOutputRecording(UWorld* World, const TCHAR* Stem, float ExpectedDurationSeconds)
	{
		if (bMasterOutputRecording || !World || !IsMasterMixerUnchanged(World)) return false;
		UAudioMixerBlueprintLibrary::StartRecordingOutput(World, ExpectedDurationSeconds, nullptr);
		bMasterOutputRecording = true;
		MasterRecordingWorld = World;
		MasterRecordingStem = Stem;
		MasterOutputStartSeconds = FPlatformTime::Seconds();
		MasterOutputStopSeconds = 0.0;
		MasterWaveLastBytes = -1;
		MasterWaveStablePolls = 0;
		return true;
	}

	bool StopMasterOutputRecording(FString& OutWavPath)
	{
		if (!bMasterOutputRecording || MasterRecordingStem.IsEmpty()) return false;
		UWorld* World = MasterRecordingWorld.Get();
		if (!World) return false;
		UAudioMixerBlueprintLibrary::StopRecordingOutput(
			World, EAudioRecordingExportType::WavFile, MasterRecordingStem, State->Directory);
		bMasterOutputRecording = false;
		MasterOutputStopSeconds = FPlatformTime::Seconds();
		OutWavPath = FPaths::Combine(State->Directory, MasterRecordingStem + TEXT(".wav"));
		MasterWaveLastBytes = -1;
		MasterWaveStablePolls = 0;
		return true;
	}

	bool PollMasterWaveStable(const FString& WavPath, double Now, bool& OutStable)
	{
		OutStable = false;
		if (Now - MasterOutputStopSeconds > IMS4MasterWaveSettleTimeoutSeconds) return false;
		const int64 Bytes = IFileManager::Get().FileSize(*WavPath);
		if (Bytes <= 44)
		{
			MasterWaveLastBytes = -1;
			MasterWaveStablePolls = 0;
			return true;
		}
		if (Bytes == MasterWaveLastBytes)
		{
			++MasterWaveStablePolls;
		}
		else
		{
			MasterWaveLastBytes = Bytes;
			MasterWaveStablePolls = 0;
		}
		OutStable = MasterWaveStablePolls >= 1;
		return true;
	}

	void StopOwnedMasterRecordingForFailure()
	{
		bMasterRecorderActiveWhenFinish = bMasterOutputRecording;
		if (!bMasterOutputRecording) return;
		UWorld* World = MasterRecordingWorld.Get();
		if (!World)
		{
			return;
		}
		const FString IncompleteStem = MasterRecordingStem + TEXT("-incomplete");
		bMasterRecorderStopAttemptedAtFinish = true;
		UAudioMixerBlueprintLibrary::StopRecordingOutput(
			World, EAudioRecordingExportType::WavFile, IncompleteStem, State->Directory);
		MasterRecorderFailureWavPath = FPaths::Combine(State->Directory, IncompleteStem + TEXT(".wav"));
		bMasterOutputRecording = false;
	}

	bool SaveFailureEvidence(const FString& Message) const
	{
		FString JSON;
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JSON);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("status"), TEXT("FAIL_S4_REPLAY_CAPTURE"));
		Writer->WriteValue(TEXT("test"), TEXT("IceMoon.AcousticField.S4.ReplaySameComponent"));
		Writer->WriteValue(TEXT("stage"), Stage);
		Writer->WriteValue(TEXT("failure"), Message);
		Writer->WriteValue(TEXT("audio_device_id"), double(MasterAudioDeviceId));
		Writer->WriteValue(TEXT("master_sample_rate_hz"), MasterSampleRate);
		Writer->WriteValue(TEXT("actual_mixer_block_frames"), double(MasterMixerBlockFrames));
		Writer->WriteValue(TEXT("actual_mixer_sample_rate_hz"), double(MasterMixerSampleRateHz));
		Writer->WriteValue(TEXT("master_recording_active_when_finish_started"), bMasterRecorderActiveWhenFinish);
		Writer->WriteValue(TEXT("master_recording_stop_request_issued_by_finish"), bMasterRecorderStopAttemptedAtFinish);
		Writer->WriteValue(TEXT("incomplete_wav_path"), MasterRecorderFailureWavPath);
		Writer->WriteValue(TEXT("timestamp_and_alignment_note"), IMS4MasterAlignmentNote);
		Writer->WriteObjectStart(TEXT("stop_tail_master"));
		Writer->WriteValue(TEXT("wav_path"), TailMasterWavPath);
		Writer->WriteValue(TEXT("record_start_seconds"), TailMasterStartSeconds);
		Writer->WriteValue(TEXT("record_stop_seconds"), TailMasterStopSeconds);
		Writer->WriteValue(TEXT("release_to_start_wall_seconds"), TailMasterReleaseGuardSeconds);
		Writer->WriteValue(TEXT("graph_silence_gate_passed"), bTailGraphSilenceGatePassed);
		Writer->WriteValue(TEXT("graph_gate_to_stop_wall_seconds"), TailMasterPostGateGuardSeconds);
		Writer->WriteValue(TEXT("master_gate_passed"), bTailMasterGatePassed);
		IM_S4WriteMasterWaveSummary(Writer, TailMasterWave);
		Writer->WriteObjectEnd();
		Writer->WriteObjectStart(TEXT("replay_master"));
		Writer->WriteValue(TEXT("wav_path"), ReplayMasterWavPath);
		Writer->WriteValue(TEXT("record_start_seconds"), ReplayMasterStartSeconds);
		Writer->WriteValue(TEXT("record_stop_seconds"), ReplayMasterStopSeconds);
		Writer->WriteValue(TEXT("pre_roll_guard_seconds"), ReplayMasterPreRollGuardSeconds);
		Writer->WriteValue(TEXT("fresh_gate_seconds"), ReplayFreshGateSeconds);
		Writer->WriteValue(TEXT("master_gate_passed"), bReplayMasterGatePassed);
		IM_S4WriteMasterWaveSummary(Writer, ReplayMasterWave);
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();
		Writer->Close();
		return FFileHelper::SaveStringToFile(JSON, *FPaths::Combine(State->Directory, TEXT("failure.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	uint32 SourceTraceCount() const
	{
		if (!Context.IsValid()) return 0;
		return FMath::Min<uint32>(
			Context->CapturedSourceBlockCount.load(std::memory_order_acquire),
			uint32(Context->CapturedSourceBlocks.Num()));
	}

	uint32 EnvironmentTraceCount() const
	{
		if (!Context.IsValid()) return 0;
		return FMath::Min<uint32>(
			Context->CapturedBlockCount.load(std::memory_order_acquire),
			uint32(Context->CapturedBlocks.Num()));
	}

	uint32 SourcePcmFrames() const
	{
		return Context.IsValid()
			? FMath::Min<uint32>(
				Context->CapturedSourceFrames.load(std::memory_order_acquire),
				uint32(Context->CapturedSource.Num()))
			: 0;
	}

	uint32 EnvironmentPcmFrames() const
	{
		return Context.IsValid()
			? FMath::Min<uint32>(
				Context->CapturedEnvironmentFrames.load(std::memory_order_acquire),
				uint32(Context->CapturedWet.Num() / 2))
			: 0;
	}

	void InspectWindow(uint32 Begin, uint32 End, uint64 AudioId, uint64 WorldGeneration,
		FIMAcousticS4WindowStats& Out) const
	{
		Out = {};
		if (!Context.IsValid()) return;
		const uint32 Limit = SourceTraceCount();
		const uint32 First = FMath::Min(Begin, Limit);
		const uint32 Last = FMath::Min(End, Limit);
		for (uint32 I = First; I < Last; ++I)
		{
			const FIMAcousticBlockProbe& Block = Context->CapturedSourceBlocks[int32(I)];
			if (Block.CallbackAudioComponentId != AudioId) continue;
			++Out.Matching;
			if (Block.InputEnergy > 1.0e-9) ++Out.InputNonzero;

			const bool bAcceptedIdentity = Block.Reject == EIMAcousticProbeReject::Accepted
				&& !Block.InputDelayPriming
				&& Block.ResultAudioComponentId == AudioId
				&& Block.ResultWorldGeneration == WorldGeneration
				&& Block.ResultVoiceGeneration != 0
				&& Block.ResultSpatialVersion != 0
				&& Block.ResultSpatialVersion == Block.OutputSpatialVersion
				&& Block.ResultSequence != 0;
			if (!bAcceptedIdentity) continue;

			++Out.Accepted;
			if (Block.OutputEnergy > 1.0e-9) ++Out.OutputNonzero;
			if (!Out.VoiceGeneration) Out.VoiceGeneration = Block.ResultVoiceGeneration;
			else if (Out.VoiceGeneration != Block.ResultVoiceGeneration) Out.bVoiceGenerationConsistent = false;
			Out.SpatialVersion = Block.ResultSpatialVersion;
			if ((Block.Routes & 2u) != 0 && Block.PathEnergy > 1.0e-9 && Block.OutputEnergy > 1.0e-9)
				++Out.PathNonzero;
		}
	}

	// Peak graph wet amplitude over a frame window, reported next to the master WAV verdict: the WAV is 16-bit, so a graph tail whose peak is
	// below one PCM16 step can never reach the master gate no matter how the route behaves.
	float PeakWetAmplitude(uint32 BeginFrame, uint32 EndFrame) const
	{
		if (!Context.IsValid()) return 0.0f;
		const uint32 First = FMath::Min(BeginFrame, EnvironmentPcmFrames());
		const uint32 Last = FMath::Min(EndFrame, EnvironmentPcmFrames());
		float Peak = 0.0f;
		for (uint32 Frame = First; Frame < Last; ++Frame)
			Peak = FMath::Max(Peak, FMath::Max(FMath::Abs(Context->CapturedWet[int32(Frame * 2)]), FMath::Abs(Context->CapturedWet[int32(Frame * 2 + 1)])));
		return Peak;
	}

	bool HasWetEnergy(uint32 BeginFrame, uint32 EndFrame) const
	{
		if (!Context.IsValid()) return false;
		const uint32 First = FMath::Min(BeginFrame, EnvironmentPcmFrames());
		const uint32 Last = FMath::Min(EndFrame, EnvironmentPcmFrames());
		for (uint32 Frame = First; Frame < Last; ++Frame)
		{
			const uint32 Sample = Frame * 2;
			if (FMath::Abs(Context->CapturedWet[int32(Sample)]) > IMS4WetSilenceThreshold
				|| FMath::Abs(Context->CapturedWet[int32(Sample + 1)]) > IMS4WetSilenceThreshold)
				return true;
		}
		return false;
	}

	void ScanPostStopWet()
	{
		const uint32 EndFrame = EnvironmentPcmFrames();
		const uint32 FirstFrame = FMath::Max(StopWetFrameBegin, TailScanFrame);
		for (uint32 Frame = FirstFrame; Frame < EndFrame; ++Frame)
		{
			const uint32 Sample = Frame * 2;
			const bool bNonzero = FMath::Abs(Context->CapturedWet[int32(Sample)]) > IMS4WetSilenceThreshold
				|| FMath::Abs(Context->CapturedWet[int32(Sample + 1)]) > IMS4WetSilenceThreshold;
			if (bNonzero)
			{
				TailSawPostStopWet = true;
				TailSilentRunFrames = 0;
			}
			else
			{
				++TailSilentRunFrames;
			}
		}
		TailScanFrame = EndFrame;
	}

	void ScanStoppedSourceBlocks()
	{
		const uint32 End = SourceTraceCount();
		const uint32 First = FMath::Min(LastSourceTraceScan, End);
		for (uint32 I = First; I < End; ++I)
		{
			const FIMAcousticBlockProbe& Block = Context->CapturedSourceBlocks[int32(I)];
			if (StopSourceReleased && Block.CallbackAudioComponentId == Play1.AudioId)
			{
				StoppedSourceBlockObserved = true;
			}
		}
		LastSourceTraceScan = End;
	}

	bool SavePcmWindow(const TCHAR* FileName, const TArray<float>& Samples,
		uint32 BeginFrame, uint32 EndFrame, uint32 Channels) const
	{
		if (EndFrame < BeginFrame || Channels == 0) return false;
		const uint64 BeginSample = uint64(BeginFrame) * Channels;
		const uint64 EndSample = uint64(EndFrame) * Channels;
		if (EndSample > uint64(Samples.Num())) return false;
		if (EndSample == BeginSample) return true;
		const TArrayView<const uint8> Bytes(
			reinterpret_cast<const uint8*>(Samples.GetData() + BeginSample),
			SIZE_T((EndSample - BeginSample) * sizeof(float)));
		return FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(State->Directory, FileName));
	}

	bool SaveSourceTrace() const
	{
		if (!Context.IsValid()) return false;
		FString CSV = TEXT("phase,trace_index,block,voice,callback_audio_id,result_audio_id,world_generation,voice_generation,spatial_version,sequence,snapshot_captured_s,consumed_s,routes,reject,input_delay_priming,input_energy,path_energy,output_energy\n");
		const uint32 Count = SourceTraceCount();
		const uint32 Begin = FMath::Min(Play1.SourceTraceBegin, Count);
		const uint32 End = FMath::Min(Play2.SourceTraceEnd, Count);
		for (uint32 I = Begin; I < End; ++I)
		{
			const FIMAcousticBlockProbe& B = Context->CapturedSourceBlocks[int32(I)];
			if (B.CallbackAudioComponentId != Play1.AudioId) continue;
			const TCHAR* Phase = I < StopSourceTraceAtRequest ? TEXT("play1")
				: I < StopSourceTraceAtRelease ? TEXT("stop_to_release")
				: I < Play2.SourceTraceBegin ? TEXT("after_release_before_replay") : TEXT("play2");
			CSV += FString::Printf(TEXT("%s,%u,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%.9f,%u,%u,%u,%.12g,%.12g,%.12g\n"),
				Phase, I, B.Block, B.Voice, B.CallbackAudioComponentId, B.ResultAudioComponentId,
				B.ResultWorldGeneration, B.ResultVoiceGeneration, B.ResultSpatialVersion, B.ResultSequence,
				B.SnapshotCaptured, B.ConsumedSeconds, B.Routes, uint32(B.Reject), uint32(B.InputDelayPriming),
				B.InputEnergy, B.PathEnergy, B.OutputEnergy);
		}
		return FFileHelper::SaveStringToFile(CSV,
			*FPaths::Combine(State->Directory, TEXT("source-blocks.csv")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	void WriteWindow(TSharedRef<TJsonWriter< TCHAR, TCondensedJsonPrintPolicy<TCHAR> >> Writer,
		const TCHAR* Name, const FIMAcousticS4ReplayWindow& Window) const
	{
		Writer->WriteObjectStart(Name);
		Writer->WriteValue(TEXT("audio_component_id"), double(Window.AudioId));
		Writer->WriteValue(TEXT("world_generation"), double(Window.WorldGeneration));
		Writer->WriteValue(TEXT("voice_generation"), double(Window.VoiceGeneration));
		Writer->WriteValue(TEXT("voice_slot"), Window.Slot);
		Writer->WriteValue(TEXT("source_trace_begin"), double(Window.SourceTraceBegin));
		Writer->WriteValue(TEXT("source_trace_end"), double(Window.SourceTraceEnd));
		Writer->WriteValue(TEXT("matching_source_blocks"), double(Window.MatchingSourceBlocks));
		Writer->WriteValue(TEXT("accepted_identity_blocks"), double(Window.AcceptedIdentityBlocks));
		Writer->WriteValue(TEXT("path_nonzero_blocks"), double(Window.PathNonzeroBlocks));
		Writer->WriteValue(TEXT("output_nonzero_blocks"), double(Window.OutputNonzeroBlocks));
		Writer->WriteValue(TEXT("nonzero_input_blocks"), double(Window.NonzeroInputBlocks));
		Writer->WriteValue(TEXT("voice_generation_consistent"), Window.bVoiceGenerationConsistent);
		Writer->WriteValue(TEXT("environment_trace_begin"), double(Window.EnvironmentTraceBegin));
		Writer->WriteValue(TEXT("environment_trace_end"), double(Window.EnvironmentTraceEnd));
		Writer->WriteValue(TEXT("source_pcm_begin_frame"), double(Window.SourcePcmBegin));
		Writer->WriteValue(TEXT("source_pcm_end_frame"), double(Window.SourcePcmEnd));
		Writer->WriteValue(TEXT("environment_pcm_begin_frame"), double(Window.EnvironmentPcmBegin));
		Writer->WriteValue(TEXT("environment_pcm_end_frame"), double(Window.EnvironmentPcmEnd));
		Writer->WriteObjectEnd();
	}

	void WriteIdentityProbe(TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer,
		const TCHAR* Name, const FIMAcousticIdentityInjectionProbe& Probe) const
	{
		Writer->WriteObjectStart(Name);
		Writer->WriteValue(TEXT("observed"), Probe.Observed.load(std::memory_order_acquire) != 0);
		Writer->WriteValue(TEXT("rejected"), Probe.Rejected.load(std::memory_order_acquire) != 0);
		Writer->WriteValue(TEXT("finite_output"), Probe.FiniteOutput.load(std::memory_order_acquire) != 0);
		Writer->WriteValue(TEXT("reject_detail"), double(Probe.RejectDetail.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("rendered_delta"), double(Probe.RenderedDelta.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("rejected_delta"), double(Probe.RejectedDelta.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("injected_audio_id"), double(Probe.InjectedAudioId.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("injected_world_generation"), double(Probe.InjectedWorldGeneration.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("injected_voice_generation"), double(Probe.InjectedVoiceGeneration.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("actual_audio_id"), double(Probe.ActualAudioId.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("actual_world_generation"), double(Probe.ActualWorldGeneration.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("actual_voice_generation"), double(Probe.ActualVoiceGeneration.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("actual_spatial_version"), double(Probe.ActualSpatialVersion.load(std::memory_order_acquire)));
		Writer->WriteObjectEnd();
	}

	bool SaveEvidence() const
	{
		IFileManager::Get().MakeDirectory(*State->Directory, true);
		bool bSaved = SavePcmWindow(TEXT("play1-source-mono.f32"), Context->CapturedSource,
				Play1.SourcePcmBegin, Play1.SourcePcmEnd, 1)
			&& SavePcmWindow(TEXT("play1-direct-path-stereo.f32"), Context->CapturedDry,
				Play1.SourcePcmBegin, Play1.SourcePcmEnd, 2)
			&& SavePcmWindow(TEXT("play1-wet-stereo.f32"), Context->CapturedWet,
				Play1.EnvironmentPcmBegin, Play1.EnvironmentPcmEnd, 2)
			&& SavePcmWindow(TEXT("stop-tail-wet-stereo.f32"), Context->CapturedWet,
				StopWetFrameBegin, TailDrainFrameEnd, 2)
			&& SavePcmWindow(TEXT("stop-to-release-direct-path-stereo.f32"), Context->CapturedDry,
				StopSourceFramesAtReturn, StopSourceFramesAtRelease, 2)
			&& SavePcmWindow(TEXT("release-tail-wet-stereo.f32"), Context->CapturedWet,
				StopEnvironmentFramesAtRelease, TailDrainFrameEnd, 2)
			&& SavePcmWindow(TEXT("play2-source-mono.f32"), Context->CapturedSource,
				Play2.SourcePcmBegin, Play2.SourcePcmEnd, 1)
			&& SavePcmWindow(TEXT("play2-direct-path-stereo.f32"), Context->CapturedDry,
				Play2.SourcePcmBegin, Play2.SourcePcmEnd, 2)
			&& SavePcmWindow(TEXT("play2-wet-stereo.f32"), Context->CapturedWet,
				Play2.EnvironmentPcmBegin, Play2.EnvironmentPcmEnd, 2)
			&& SaveSourceTrace();

		FString JSON;
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JSON);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("status"), bSaved ? TEXT("CAPTURED_ANALYSIS_PENDING") : TEXT("FAIL_EVIDENCE_WRITE"));
		Writer->WriteValue(TEXT("test"), TEXT("IceMoon.AcousticField.S4.ReplaySameComponent"));
		Writer->WriteValue(TEXT("map"), IMS4ReplayMap);
		Writer->WriteValue(TEXT("source_asset"), IMAcousticTestSupport::GraphSourceAssetPath);
		Writer->WriteValue(TEXT("context_device_id"), int32(Context->DeviceId));
		Writer->WriteValue(TEXT("context_pointer"), FString::Printf(TEXT("%p"), Context.Get()));
		Writer->WriteValue(TEXT("world_generation"), double(Context->Pool->WorldGeneration));
		Writer->WriteValue(TEXT("context_same_across_plays"), true);
		Writer->WriteValue(TEXT("route_mask"), int32(Context->Device->RenderRoutes.load(std::memory_order_acquire)));
		Writer->WriteValue(TEXT("map_direct_route"), false);
		Writer->WriteValue(TEXT("map_path_route"), true);
		Writer->WriteValue(TEXT("map_reverb_route"), true);
		Writer->WriteValue(TEXT("stop_world_transition_covered"), false);
		Writer->WriteValue(TEXT("audio_component_id_reuse_forced"), false);
		Writer->WriteValue(TEXT("audio_component_id_reuse_claim"), TEXT("not_tested_same_component_retains_id"));
		Writer->WriteValue(TEXT("same_component_id_across_play_stop_play"), Play1.AudioId == Play2.AudioId);
		Writer->WriteValue(TEXT("stop_source_consumer_released"), StopSourceReleased);
		Writer->WriteValue(TEXT("stop_request_seconds"), StopRequestSeconds);
		Writer->WriteValue(TEXT("stop_return_seconds"), StopReturnSeconds);
		Writer->WriteValue(TEXT("stop_source_release_seconds"), StopSourceReleaseSeconds);
		Writer->WriteValue(TEXT("stop_source_trace_at_request"), double(StopSourceTraceAtRequest));
		Writer->WriteValue(TEXT("stop_source_trace_at_return"), double(StopSourceTraceAtReturn));
		Writer->WriteValue(TEXT("stop_source_trace_at_release"), double(StopSourceTraceAtRelease));
		Writer->WriteValue(TEXT("stop_source_pcm_frames_at_request"), double(StopSourceFramesAtRequest));
		Writer->WriteValue(TEXT("stop_source_pcm_frames_at_return"), double(StopSourceFramesAtReturn));
		Writer->WriteValue(TEXT("stop_source_pcm_frames_at_release"), double(StopSourceFramesAtRelease));
		Writer->WriteValue(TEXT("stop_environment_pcm_frames_at_release"), double(StopEnvironmentFramesAtRelease));
		Writer->WriteValue(TEXT("wet_energy_after_release"), HasWetEnergy(StopEnvironmentFramesAtRelease, TailDrainFrameEnd));
		Writer->WriteValue(TEXT("stop_live_generation"), double(StopLiveGeneration));
		Writer->WriteValue(TEXT("post_stop_wet_observed"), TailSawPostStopWet);
		Writer->WriteValue(TEXT("post_stop_silent_run_frames"), double(TailSilentRunFrames));
		Writer->WriteValue(TEXT("tail_drain_begin_frame"), double(StopWetFrameBegin));
		Writer->WriteValue(TEXT("tail_drain_end_frame"), double(TailDrainFrameEnd));
		Writer->WriteValue(TEXT("old_source_block_after_release"), StoppedSourceBlockObserved);
		Writer->WriteValue(TEXT("normal_identity_accepted"), Play2.AcceptedIdentityBlocks > 0);
		Writer->WriteObjectStart(TEXT("play1"));
		Writer->WriteValue(TEXT("audio_component_id"), double(Play1.AudioId));
		Writer->WriteValue(TEXT("world_generation"), double(Play1.WorldGeneration));
		Writer->WriteValue(TEXT("voice_generation"), double(Play1.VoiceGeneration));
		Writer->WriteValue(TEXT("voice_slot"), Play1.Slot);
		Writer->WriteValue(TEXT("matching_source_blocks"), double(Play1.MatchingSourceBlocks));
		Writer->WriteValue(TEXT("accepted_identity_blocks"), double(Play1.AcceptedIdentityBlocks));
		Writer->WriteValue(TEXT("path_nonzero_blocks"), double(Play1.PathNonzeroBlocks));
		Writer->WriteValue(TEXT("output_nonzero_blocks"), double(Play1.OutputNonzeroBlocks));
		Writer->WriteValue(TEXT("nonzero_input_blocks"), double(Play1.NonzeroInputBlocks));
		Writer->WriteValue(TEXT("source_trace_begin"), double(Play1.SourceTraceBegin));
		Writer->WriteValue(TEXT("source_trace_end"), double(Play1.SourceTraceEnd));
		Writer->WriteValue(TEXT("source_pcm_begin_frame"), double(Play1.SourcePcmBegin));
		Writer->WriteValue(TEXT("source_pcm_end_frame"), double(Play1.SourcePcmEnd));
		Writer->WriteValue(TEXT("environment_pcm_begin_frame"), double(Play1.EnvironmentPcmBegin));
		Writer->WriteValue(TEXT("environment_pcm_end_frame"), double(Play1.EnvironmentPcmEnd));
		Writer->WriteObjectEnd();
		Writer->WriteObjectStart(TEXT("play2"));
		Writer->WriteValue(TEXT("audio_component_id"), double(Play2.AudioId));
		Writer->WriteValue(TEXT("world_generation"), double(Play2.WorldGeneration));
		Writer->WriteValue(TEXT("voice_generation"), double(Play2.VoiceGeneration));
		Writer->WriteValue(TEXT("voice_slot"), Play2.Slot);
		Writer->WriteValue(TEXT("matching_source_blocks"), double(Play2.MatchingSourceBlocks));
		Writer->WriteValue(TEXT("accepted_identity_blocks"), double(Play2.AcceptedIdentityBlocks));
		Writer->WriteValue(TEXT("path_nonzero_blocks"), double(Play2.PathNonzeroBlocks));
		Writer->WriteValue(TEXT("output_nonzero_blocks"), double(Play2.OutputNonzeroBlocks));
		Writer->WriteValue(TEXT("nonzero_input_blocks"), double(Play2.NonzeroInputBlocks));
		Writer->WriteValue(TEXT("source_trace_begin"), double(Play2.SourceTraceBegin));
		Writer->WriteValue(TEXT("source_trace_end"), double(Play2.SourceTraceEnd));
		Writer->WriteValue(TEXT("source_pcm_begin_frame"), double(Play2.SourcePcmBegin));
		Writer->WriteValue(TEXT("source_pcm_end_frame"), double(Play2.SourcePcmEnd));
		Writer->WriteValue(TEXT("environment_pcm_begin_frame"), double(Play2.EnvironmentPcmBegin));
		Writer->WriteValue(TEXT("environment_pcm_end_frame"), double(Play2.EnvironmentPcmEnd));
		Writer->WriteObjectEnd();
		Writer->WriteObjectStart(TEXT("identity_negative"));
		Writer->WriteValue(TEXT("status"), TEXT("PASS_FOCUSED_FAIL_CLOSED"));
		WriteIdentityProbe(Writer, TEXT("bad_audio_id"), Context->IdentityInjection.Probes[0]);
		WriteIdentityProbe(Writer, TEXT("missing_fields"), Context->IdentityInjection.Probes[1]);
		Writer->WriteObjectEnd();
		Writer->WriteObjectStart(TEXT("master_output"));
		Writer->WriteValue(TEXT("device_id"), double(MasterAudioDeviceId));
		Writer->WriteValue(TEXT("sample_rate_hz"), MasterSampleRate);
		Writer->WriteValue(TEXT("actual_mixer_block_frames"), double(MasterMixerBlockFrames));
		Writer->WriteValue(TEXT("actual_mixer_sample_rate_hz"), double(MasterMixerSampleRateHz));
		Writer->WriteValue(TEXT("timestamp_and_alignment_note"), IMS4MasterAlignmentNote);
		Writer->WriteValue(TEXT("pcm16_silence_threshold_abs_lsb"), IMS4MasterPcm16SilenceThreshold);
		Writer->WriteValue(TEXT("overload_peak_limit_abs_pcm16"), 32760);
		Writer->WriteObjectStart(TEXT("stop_tail"));
		Writer->WriteValue(TEXT("wav_path"), TailMasterWavPath);
		Writer->WriteValue(TEXT("record_start_seconds"), TailMasterStartSeconds);
		Writer->WriteValue(TEXT("record_stop_seconds"), TailMasterStopSeconds);
		Writer->WriteValue(TEXT("release_to_start_required_seconds"), 0.10);
		Writer->WriteValue(TEXT("release_to_start_wall_seconds"), TailMasterReleaseGuardSeconds);
		Writer->WriteValue(TEXT("release_guard_passed"), TailMasterReleaseGuardSeconds >= 0.10);
		Writer->WriteValue(TEXT("graph_silence_gate_passed_before_stop"), bTailGraphSilenceGatePassed);
		Writer->WriteValue(TEXT("graph_gate_to_stop_required_mixer_blocks"), 3);
		Writer->WriteValue(TEXT("graph_gate_to_stop_required_seconds"),
			3.0 * double(MasterMixerBlockFrames) / MasterMixerSampleRateHz);
		Writer->WriteValue(TEXT("graph_gate_to_stop_wall_seconds"), TailMasterPostGateGuardSeconds);
		Writer->WriteValue(TEXT("capture_wall_seconds"), TailMasterStopSeconds - TailMasterStartSeconds);
		Writer->WriteValue(TEXT("required_trailing_silent_frames"),
			FMath::CeilToInt(double(MasterSampleRate) * IMS4MasterMinimumTailSilenceMs / 1000.0));
		Writer->WriteValue(TEXT("master_tail_gate_passed"), bTailMasterGatePassed);
		Writer->WriteValue(TEXT("graph_wet_peak_in_master_window"), TailGraphPeakInMasterWindow);
		Writer->WriteValue(TEXT("graph_tail_observable_in_pcm16_master"), bTailObservableInMaster);
		IM_S4WriteMasterWaveSummary(Writer, TailMasterWave);
		Writer->WriteObjectEnd();
		Writer->WriteObjectStart(TEXT("replay"));
		Writer->WriteValue(TEXT("wav_path"), ReplayMasterWavPath);
		Writer->WriteValue(TEXT("record_start_seconds"), ReplayMasterStartSeconds);
		Writer->WriteValue(TEXT("play2_start_seconds"), Play2StartSeconds);
		Writer->WriteValue(TEXT("fresh_gate_seconds"), ReplayFreshGateSeconds);
		Writer->WriteValue(TEXT("record_stop_seconds"), ReplayMasterStopSeconds);
		Writer->WriteValue(TEXT("pre_roll_wall_guard_required_seconds"), ReplayMasterPreRollGuardSeconds);
		Writer->WriteValue(TEXT("pre_roll_wall_guard_actual_seconds"), Play2StartSeconds - ReplayMasterStartSeconds);
		Writer->WriteValue(TEXT("fresh_gate_to_stop_required_seconds"), IMS4MasterReplayActiveCaptureSeconds);
		Writer->WriteValue(TEXT("fresh_gate_to_stop_wall_seconds"), ReplayMasterStopSeconds - ReplayFreshGateSeconds);
		Writer->WriteValue(TEXT("required_leading_silent_frames"), double(2 * MasterMixerBlockFrames));
		Writer->WriteValue(TEXT("master_replay_gate_passed"), bReplayMasterGatePassed);
		IM_S4WriteMasterWaveSummary(Writer, ReplayMasterWave);
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();
		Writer->Close();
		bSaved = FFileHelper::SaveStringToFile(JSON, *FPaths::Combine(State->Directory, TEXT("capture.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && bSaved;
		return bSaved;
	}

	bool Finish(bool Success, const FString& Message)
	{
		if (Finished) return true;
		Finished = true;
		if (!Success)
		{
			StopOwnedMasterRecordingForFailure();
			if (!SaveFailureEvidence(Message))
				UE_LOG(LogTemp, Error, TEXT("IMLogs AcousticS4Replay could not persist failure.json evidence at %s."), *State->Directory);
			if (State->Test) State->Test->AddError(Message);
		}
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(State->OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = State->bOriginalBackgroundAudio;
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS4Replay %s evidence=%s"), *Message, *State->Directory);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticS4Replay %s"), Success ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), Success ? TEXT("PASS") : TEXT("FAIL"));
		if (GEditor && GEditor->PlayWorld) GEditor->RequestEndPlayMap();
		return true;
	}

	TSharedRef<FIMAcousticS4ReplayState> State;
	int32 Stage = 0;
	bool Finished = false;
	bool bTailMasterRecordingStarted = false;
	bool bTailGraphSilenceGatePassed = false;
	bool bTailMasterGatePassed = false;
	bool bTailObservableInMaster = false;
	float TailGraphPeakInMasterWindow = 0.0f;
	bool bReplayMasterRecordingStarted = false;
	bool bReplayFreshGatePassed = false;
	bool bReplayMasterGatePassed = false;
	bool bMasterOutputRecording = false;
	bool bMasterRecorderActiveWhenFinish = false;
	bool bMasterRecorderStopAttemptedAtFinish = false;
	bool StopSourceReleased = false;
	bool StoppedSourceBlockObserved = false;
	bool TailSawPostStopWet = false;
	uint64 StopLiveGeneration = 0;
	uint32 StopSourceTraceAtRequest = 0;
	uint32 StopSourceTraceAtReturn = 0;
	uint32 StopSourceTraceAtRelease = 0;
	uint32 StopSourceFramesAtRequest = 0;
	uint32 StopSourceFramesAtReturn = 0;
	uint64 StopSourceFramesAtRelease = 0;
	uint32 StopEnvironmentFramesAtRelease = 0;
	uint32 StopWetFrameBegin = 0;
	uint32 TailDrainFrameEnd = 0;
	uint32 TailFrameAtMasterStart = 0;
	uint32 TailScanFrame = 0;
	uint32 TailSilentRunFrames = 0;
	uint32 LastSourceTraceScan = 0;
	uint32 MasterAudioDeviceId = 0;
	uint32 MasterMixerBlockFrames = 0;
	uint32 MasterWaveStablePolls = 0;
	int32 MasterSampleRate = 0;
	int64 MasterWaveLastBytes = -1;
	float MasterMixerSampleRateHz = 0.0f;
	double Play1StartSeconds = 0.0;
	double StopSeconds = 0.0;
	double StopRequestSeconds = 0.0;
	double StopReturnSeconds = 0.0;
	double StopSourceReleaseSeconds = 0.0;
	double Play2StartSeconds = 0.0;
	double TailMasterStartSeconds = 0.0;
	double TailMasterStopSeconds = 0.0;
	double TailMasterReleaseGuardSeconds = 0.0;
	double TailGraphSilenceGateSeconds = 0.0;
	double TailMasterPostGateGuardSeconds = 0.0;
	double ReplayMasterStartSeconds = 0.0;
	double ReplayMasterStopSeconds = 0.0;
	double ReplayMasterPreRollGuardSeconds = 0.0;
	double ReplayFreshGateSeconds = 0.0;
	double MasterOutputStartSeconds = 0.0;
	double MasterOutputStopSeconds = 0.0;
	uint64 ReplaySpatialVersion = 0;
	FString MasterRecordingStem;
	FString TailMasterWavPath;
	FString ReplayMasterWavPath;
	FString MasterRecorderFailureWavPath;
	TWeakObjectPtr<UWorld> MasterRecordingWorld;
	FIMAcousticS4MasterWaveSummary TailMasterWave;
	FIMAcousticS4MasterWaveSummary ReplayMasterWave;
	FIMAcousticMetaSoundContextPtr Context;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<UAudioComponent> Source;
	FIMAcousticS4ReplayWindow Play1;
	FIMAcousticS4ReplayWindow Play2;
};

class FIMAcousticS4IRLeaseReacquireCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticS4IRLeaseReacquireCommand(TSharedRef<FIMAcousticS4ReplayState> InState)
		: State(InState)
	{
	}

	bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - State->Started > 90.0)
			return Finish(false, FString::Printf(TEXT("IR lease reacquire timed out at stage %d."), Stage));

		UWorld* World = IM_FindS4PIEWorld();
		if (!World)
		{
			if (Stage == 0) return false;
			return Finish(false, TEXT("PIE world ended before the IR lease reacquire evidence completed."));
		}

		if (Stage == 0)
		{
			Listener = World->GetFirstPlayerController();
			if (!Listener.IsValid())
			{
				if (Now - State->Started > 15.0) return Finish(false, TEXT("IM_V2Audition has no player listener."));
				return false;
			}
			for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It) Volume = *It;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (!It->ActorHasTag(TEXT("IMAcousticAuditionReferenceV1"))) continue;
				if (UIMAcousticSourceComponent* Marker = It->FindComponentByClass<UIMAcousticSourceComponent>())
				{
					Source = Marker->AudioComponent;
					break;
				}
			}
			if (!Volume.IsValid() || !Source.IsValid())
			{
				if (Now - State->Started > 15.0)
					return Finish(false, TEXT("IM_V2Audition did not expose its acoustic volume and tagged ordinary source."));
				return false;
			}

			FAudioDevice* AudioDevice = World->GetAudioDeviceRaw();
			if (!AudioDevice)
			{
				if (Now - State->Started > 15.0) return Finish(false, TEXT("PIE world has no audio device."));
				return false;
			}
			Context = IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID);
			if (!Context || !Context->Pool || !Context->Device || Context->Stopped.load(std::memory_order_acquire))
			{
				if (Now - State->Started > 15.0) return Finish(false, TEXT("Ordinary MetaSound context was not created."));
				return false;
			}
			if (!IMAcousticMetaSound::IsAcousticMetaSound(Source->Sound) || Source->bAllowSpatialization)
				return Finish(false, TEXT("IR lease reacquire requires the ordinary isolated MetaSound source."));
			if (!Volume->bEnableV2 || !Volume->bReverbRoute || (!Volume->bDirectRoute && !Volume->bPathRoute)
				|| Volume->ReverbWetGain <= 0.0f || Context->WetGain.load(std::memory_order_acquire) <= 0.0f)
				return Finish(false, TEXT("IM_V2Audition must begin with V2, reverb, and at least one dry route enabled with nonzero wet gain."));
			FString MixerError;
			if (!InitializeMasterMixer(World, MixerError)) return Finish(false, MixerError);

			// Same fixture contract as the replay case above: legacy render-material mappings drive the audition map.
			Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
			OriginalEnableV2 = Volume->bEnableV2;
			OriginalDirectRoute = Volume->bDirectRoute;
			OriginalPathRoute = Volume->bPathRoute;
			OriginalReverbRoute = Volume->bReverbRoute;
			OriginalVolumeWetGain = Volume->ReverbWetGain;
			OriginalContextWetGain = Context->WetGain.load(std::memory_order_acquire);
			bSettingsCaptured = true;
			ExpectedOnRouteMask = (OriginalDirectRoute ? 1u : 0u) | (OriginalPathRoute ? 2u : 0u) | 4u;
			WorldIdentity = World;
			AudioDeviceId = AudioDevice->DeviceID;
			WorldGeneration = Context->Pool->WorldGeneration;

			if (!Source->IsPlaying())
			{
				Source->Play();
				bStartedSourceForTest = true;
			}
			BaselineSourceBegin = SourceTraceCount();
			BaselineEnvironmentBegin = EnvironmentTraceCount();
			BaselineWetBegin = WetFrames();
			Stage = 1;
			return false;
		}

		if (!OwnerAndSettingsStable(World, Stage == 2 || Stage == 3 ? false : true))
			return Finish(false, TEXT("World, listener, source, AudioId, voice context, dry routes, or gain changed across the IR lease phases."));

		const uint32 ActualRoutes = Context->Device->RenderRoutes.load(std::memory_order_acquire);
		const uint32 ExpectedRoutes = Stage == 2 || Stage == 3
			? (ExpectedOnRouteMask & ~4u) : ExpectedOnRouteMask;

		if (Stage == 1)
		{
			if (ActualRoutes != ExpectedOnRouteMask) return false;
			const uint64 CurrentAudioId = Source->GetAudioComponentID();
			const int32 CurrentSlot = IM_FindS4Slot(Context, CurrentAudioId);
			if (!CurrentAudioId || CurrentSlot == INDEX_NONE)
			{
				if (Now - State->Started > 20.0) return Finish(false, TEXT("Ordinary source did not acquire its AudioId lease."));
				return false;
			}
			if (!AudioId) { AudioId = CurrentAudioId; VoiceSlot = CurrentSlot; }
			if (CurrentAudioId != AudioId || CurrentSlot != VoiceSlot)
				return Finish(false, TEXT("Baseline source identity changed before the route experiment."));

			AnalyzeSource(BaselineSourceBegin, SourceTraceCount(), ExpectedOnRouteMask,
				BaselineAccepted, BaselineOutput, BaselineVoice, bBaselineVoiceConsistent);
			FindFreshEnvironment(BaselineEnvironmentBegin, EnvironmentTraceCount(),
				BaselineSequence, BaselineSpatialVersion, BaselineExpectedSpatialVersion);
			if (BaselineAccepted < 4 || BaselineOutput == 0 || !BaselineVoice || !bBaselineVoiceConsistent
				|| !BaselineSequence || BaselineSpatialVersion != BaselineExpectedSpatialVersion
				|| BaselineSpatialVersion != Context->ListenerSpatialVersion.load(std::memory_order_acquire)
				|| Context->LastIRSequence.load(std::memory_order_acquire) < BaselineSequence
				|| !HasWetEnergy(BaselineWetBegin, WetFrames()))
			{
				if (Now - State->Started > 25.0)
					return Finish(false, TEXT("Baseline did not provide accepted dry output, wet PCM, and a fresh identity-matched IR lease."));
				return false;
			}

			BaselineSourceEnd = SourceTraceCount();
			BaselineEnvironmentEnd = EnvironmentTraceCount();
			BaselineWetEnd = WetFrames();
			BaselineGateSeconds = Now;
			OffRequestSeconds = Now;
			OffRouteObservedSeconds = 0.0;
			OffSourceBegin = SourceTraceCount();
			OffEnvironmentBegin = EnvironmentTraceCount();
			OffWetBegin = WetFrames();
			Volume->bReverbRoute = false;
			Stage = 2;
			return false;
		}

		if (Stage == 2)
		{
			if ((ActualRoutes & 4u) != 0) return false;
			if (!OffRouteObservedSeconds) OffRouteObservedSeconds = Now;
			const double RouteDrainGuard = FMath::Max(
				0.10, 3.0 * double(MasterMixerBlockFrames) / MasterMixerSampleRateHz);
			if (Now - OffRouteObservedSeconds < RouteDrainGuard) return false;
			if (!bOffCaptureStarted)
			{
				OffSourceBegin = SourceTraceCount();
				OffEnvironmentBegin = EnvironmentTraceCount();
				OffWetBegin = WetFrames();
				bOffCaptureStarted = true;
			}
			ObserveOffPhase();
			if (OffFreshBlocks != 0)
				return Finish(false, TEXT("Fresh IR output continued after the reverb route was disabled."));
			if (OffStableFreePolls < 3 || !OffSawReleasedTrace || OffEnvironmentEnd - OffEnvironmentBegin < 8
				|| OffWetEnd - OffWetBegin < 8 * FIMAcousticMetaSoundContext::Frames
				|| OffWetNonzeroSamples != 0 || OffAccepted < 4 || OffOutput == 0
				|| OffVoice != BaselineVoice || !bOffVoiceConsistent)
			{
				if (Now - OffRouteObservedSeconds > 20.0)
					return Finish(false, FString::Printf(
						TEXT("Reverb-off gate failed: stable_free=%u released_trace=%d fresh=%llu wet_nonzero_samples=%llu dry_accepted=%llu dry_output=%llu."),
						OffStableFreePolls, OffSawReleasedTrace ? 1 : 0, OffFreshBlocks, OffWetNonzeroSamples, OffAccepted, OffOutput));
				return false;
			}

			OffEnvironmentEnd = EnvironmentTraceCount();
			OffSourceEnd = SourceTraceCount();
			OffWetEnd = WetFrames();
			if (!StartMasterOutputRecording(World))
				return Finish(false, TEXT("Could not start the owned master-output recording for the reverb-on phase."));
			Stage = 3;
			return false;
		}

		if (Stage == 3)
		{
			ObserveOffPhase();
			if ((ActualRoutes & 4u) != 0 || OffStableFreePolls < 3 || OffWetNonzeroSamples != 0
				|| OffOutput == 0 || OffVoice != BaselineVoice || !bOffVoiceConsistent)
				return Finish(false, TEXT("Dry output or the released-lease state changed during master pre-roll."));
			const double PreRollGuard = FMath::Max(
				0.10, 4.0 * double(MasterMixerBlockFrames) / MasterMixerSampleRateHz);
			if (Now - MasterStartSeconds < PreRollGuard) return false;

			OffEnvironmentEnd = EnvironmentTraceCount();
			OffSourceEnd = SourceTraceCount();
			OffWetEnd = WetFrames();
			OnSourceBegin = SourceTraceCount();
			OnEnvironmentBegin = EnvironmentTraceCount();
			OnWetBegin = WetFrames();
			Volume->bReverbRoute = true;
			OnRequestSeconds = Now;
			Stage = 4;
			return false;
		}

		if (Stage == 4)
		{
			if ((ActualRoutes & 4u) == 0) return false;
			AnalyzeSource(OnSourceBegin, SourceTraceCount(), ExpectedOnRouteMask,
				OnAccepted, OnOutput, OnVoice, bOnVoiceConsistent);
			FindFreshEnvironment(OnEnvironmentBegin, EnvironmentTraceCount(),
				OnSequence, OnSpatialVersion, OnExpectedSpatialVersion);
			const bool bOnWet = HasWetEnergy(OnWetBegin, WetFrames());
			const bool bFreshIdentity = OnSequence > BaselineSequence
				&& OnSpatialVersion == BaselineSpatialVersion
				&& OnExpectedSpatialVersion == BaselineSpatialVersion
				&& Context->LastIRSequence.load(std::memory_order_acquire) >= OnSequence;
			const bool bDryContinues = OnAccepted > 0 && OnOutput > 0 && OnVoice == BaselineVoice
				&& bOnVoiceConsistent;
			if (!bFreshIdentity || !bOnWet || !bDryContinues)
			{
				if (Now - OnRequestSeconds > 30.0)
					return Finish(false, TEXT("Reverb-on did not reacquire a newer fresh IR with matching spatial identity, wet PCM, and stable dry voice output."));
				return false;
			}
			if (!OnFreshGateSeconds) OnFreshGateSeconds = Now;
			if (Now - OnFreshGateSeconds < IMS4MasterReplayActiveCaptureSeconds) return false;

			OnSourceEnd = SourceTraceCount();
			OnEnvironmentEnd = EnvironmentTraceCount();
			OnWetEnd = WetFrames();
			if (!StopMasterOutputRecording(MasterWavPath))
				return Finish(false, TEXT("Could not stop the owned master-output recording."));
			Stage = 5;
			return false;
		}

		if (Stage == 5)
		{
			bool bStable = false;
			if (!PollMasterWaveStable(Now, bStable))
				return Finish(false, TEXT("The IR reacquire master WAV did not settle within 30 seconds."));
			if (!bStable) return false;
			if (!IM_S4ReadMasterWave(MasterWavPath, MasterSampleRate, MasterWave))
				return Finish(false, FString::Printf(TEXT("Could not parse IR reacquire master WAV: %s"), *MasterWave.Error));
			bMasterOutputGate = MasterWave.NonSilentFrames > 0 && MasterWave.ActiveFrames > 0;
			if (!bMasterOutputGate)
				return Finish(false, TEXT("The reverb-on master WAV had no nonzero device-output PCM."));
			RestoreTestChanges();
			return Finish(SaveEvidence(), TEXT("Same-world reverb route release and reacquire produced a newer fresh IR, wet PCM, stable dry output, and nonzero master WAV PCM."));
		}

		return Finish(false, TEXT("IR lease reacquire entered an unknown stage."));
	}

private:
	uint32 SourceTraceCount() const
	{
		return Context.IsValid()
			? FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire),
				uint32(Context->CapturedSourceBlocks.Num())) : 0;
	}
	uint32 EnvironmentTraceCount() const
	{
		return Context.IsValid()
			? FMath::Min<uint32>(Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire),
				uint32(Context->CapturedBlocks.Num())) : 0;
	}
	uint32 WetFrames() const
	{
		return Context.IsValid()
			? FMath::Min<uint32>(Context->CapturedEnvironmentFrames.load(std::memory_order_acquire),
				uint32(Context->CapturedWet.Num() / 2)) : 0;
	}
	bool HasWetEnergy(uint32 Begin, uint32 End) const
	{
		if (!Context.IsValid()) return false;
		const uint32 Limit = FMath::Min(End, WetFrames());
		for (uint32 Frame = FMath::Min(Begin, Limit); Frame < Limit; ++Frame)
		{
			const uint32 Sample = Frame * 2;
			if (FMath::Abs(Context->CapturedWet[int32(Sample)]) > IMS4WetSilenceThreshold
				|| FMath::Abs(Context->CapturedWet[int32(Sample + 1)]) > IMS4WetSilenceThreshold) return true;
		}
		return false;
	}
	uint64 CountWetNonzero(uint32 Begin, uint32 End) const
	{
		if (!Context.IsValid()) return 0;
		uint64 Count = 0;
		const uint32 Limit = FMath::Min(End, WetFrames());
		for (uint32 Frame = FMath::Min(Begin, Limit); Frame < Limit; ++Frame)
		{
			const uint32 Sample = Frame * 2;
			Count += FMath::Abs(Context->CapturedWet[int32(Sample)]) > IMS4WetSilenceThreshold ? 1 : 0;
			Count += FMath::Abs(Context->CapturedWet[int32(Sample + 1)]) > IMS4WetSilenceThreshold ? 1 : 0;
		}
		return Count;
	}
	void AnalyzeSource(uint32 Begin, uint32 End, uint32 ExpectedRoutes,
		uint64& OutAccepted, uint64& OutOutput, uint64& OutVoice, bool& OutConsistent) const
	{
		OutAccepted = OutOutput = OutVoice = 0;
		OutConsistent = true;
		if (!Context.IsValid() || !AudioId) return;
		const uint32 Limit = FMath::Min(End, SourceTraceCount());
		for (uint32 Index = FMath::Min(Begin, Limit); Index < Limit; ++Index)
		{
			const FIMAcousticBlockProbe& B = Context->CapturedSourceBlocks[int32(Index)];
			if (B.CallbackAudioComponentId != AudioId) continue;
			if (B.Reject != EIMAcousticProbeReject::Accepted || B.InputDelayPriming
				|| B.ResultAudioComponentId != AudioId || B.ResultWorldGeneration != WorldGeneration
				|| !B.ResultVoiceGeneration || !B.ResultSpatialVersion
				|| B.ResultSpatialVersion != B.OutputSpatialVersion || !B.ResultSequence
				|| B.Routes != ExpectedRoutes) continue;
			++OutAccepted;
			if (B.OutputEnergy > 1.0e-9) ++OutOutput;
			if (!OutVoice) OutVoice = B.ResultVoiceGeneration;
			else if (OutVoice != B.ResultVoiceGeneration) OutConsistent = false;
		}
	}
	void FindFreshEnvironment(uint32 Begin, uint32 End,
		uint64& OutSequence, uint64& OutSpatial, uint64& OutExpected) const
	{
		OutSequence = OutSpatial = OutExpected = 0;
		if (!Context.IsValid()) return;
		const uint32 Limit = FMath::Min(End, EnvironmentTraceCount());
		for (uint32 Index = FMath::Min(Begin, Limit); Index < Limit; ++Index)
		{
			const FIMAcousticMetaSoundBlock& B = Context->CapturedBlocks[int32(Index)];
			if (B.Fresh && B.Sequence >= OutSequence && B.Sequence)
			{
				OutSequence = B.Sequence;
				OutSpatial = B.SpatialVersion;
				OutExpected = B.ExpectedSpatialVersion;
			}
		}
	}
	bool SawReleasedEnvironmentTrace(uint32 Begin, uint32 End) const
	{
		if (!Context.IsValid()) return false;
		const uint32 Limit = FMath::Min(End, EnvironmentTraceCount());
		for (uint32 Index = FMath::Min(Begin, Limit); Index < Limit; ++Index)
		{
			const FIMAcousticMetaSoundBlock& B = Context->CapturedBlocks[int32(Index)];
			if (!B.Fresh && !B.Sequence && !B.SpatialVersion) return true;
		}
		return false;
	}
	bool AllLeaseSlotsFree() const
	{
		if (!Context.IsValid() || !Context->Pool) return false;
		for (const FIMAcousticReverbSlot& Slot : Context->Pool->Slots)
			if (Slot.State.load(std::memory_order_acquire) != EIMAcousticIRState::Free) return false;
		return true;
	}
	bool OwnerAndSettingsStable(UWorld* World, bool bExpectedReverb) const
	{
		FAudioDevice* AudioDevice = World ? World->GetAudioDeviceRaw() : nullptr;
		return World && World == WorldIdentity.Get() && Volume.IsValid() && Source.IsValid()
			&& Listener.IsValid() && World->GetFirstPlayerController() == Listener.Get()
			&& AudioDevice && AudioDevice->DeviceID == AudioDeviceId
			&& Context.IsValid() && Context->Pool && Context->Device
			&& Context->Pool->WorldGeneration == WorldGeneration
			&& !Context->Stopped.load(std::memory_order_acquire)
			&& Volume->bEnableV2 == OriginalEnableV2
			&& Volume->bDirectRoute == OriginalDirectRoute
			&& Volume->bPathRoute == OriginalPathRoute
			&& Volume->bReverbRoute == bExpectedReverb
			&& FMath::IsNearlyEqual(Volume->ReverbWetGain, OriginalVolumeWetGain, 1.0e-6f)
			&& FMath::IsNearlyEqual(Context->WetGain.load(std::memory_order_acquire), OriginalContextWetGain, 1.0e-6f)
			&& Source->IsPlaying()
			&& (!AudioId || (Source->GetAudioComponentID() == AudioId
				&& IM_FindS4Slot(Context, AudioId) == VoiceSlot))
			&& (!BaselineSpatialVersion
				|| Context->ListenerSpatialVersion.load(std::memory_order_acquire) == BaselineSpatialVersion);
	}
	void ObserveOffPhase()
	{
		AnalyzeSource(OffSourceBegin, SourceTraceCount(), ExpectedOnRouteMask & ~4u,
			OffAccepted, OffOutput, OffVoice, bOffVoiceConsistent);
		OffEnvironmentEnd = EnvironmentTraceCount();
		OffSourceEnd = SourceTraceCount();
		OffWetEnd = WetFrames();
		OffWetNonzeroSamples = CountWetNonzero(OffWetBegin, OffWetEnd);
		OffFreshBlocks = 0;
		for (uint32 I = OffEnvironmentBegin; I < OffEnvironmentEnd; ++I)
			if (Context->CapturedBlocks[int32(I)].Fresh) ++OffFreshBlocks;
		OffSawReleasedTrace = SawReleasedEnvironmentTrace(OffEnvironmentBegin, OffEnvironmentEnd);
		if (AllLeaseSlotsFree()) ++OffStableFreePolls; else OffStableFreePolls = 0;
	}
	bool InitializeMasterMixer(UWorld* World, FString& OutError)
	{
		FAudioDevice* AudioDevice = World ? World->GetAudioDeviceRaw() : nullptr;
		Audio::FMixerDevice* Mixer = World ? FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(World) : nullptr;
		if (!AudioDevice || !Mixer || AudioDevice->DeviceID != Context->DeviceId)
		{
			OutError = TEXT("Master WAV evidence requires the same PIE AudioMixer device.");
			return false;
		}
		const float Rate = Mixer->GetSampleRate();
		const uint32 BlockFrames = Mixer->GetNumOutputFrames();
		if (FMath::RoundToInt(Rate) != IMS4MasterSampleRate
			|| FMath::RoundToInt(AudioDevice->GetSampleRate()) != IMS4MasterSampleRate || !BlockFrames)
		{
			OutError = FString::Printf(TEXT("Master WAV requires 48 kHz and a nonzero actual mixer block; mixer_rate=%g block=%u."),
				Rate, BlockFrames);
			return false;
		}
		MasterSampleRate = IMS4MasterSampleRate;
		MasterMixerSampleRateHz = Rate;
		MasterMixerBlockFrames = BlockFrames;
		return true;
	}
	bool StartMasterOutputRecording(UWorld* World)
	{
		if (bMasterRecording || !World) return false;
		UAudioMixerBlueprintLibrary::StartRecordingOutput(World, 35.0f, nullptr);
		MasterWorld = World;
		MasterStartSeconds = FPlatformTime::Seconds();
		MasterStem = TEXT("s4-ir-lease-on-master");
		bMasterRecording = true;
		MasterLastBytes = -1;
		MasterStablePolls = 0;
		return true;
	}
	bool StopMasterOutputRecording(FString& OutPath)
	{
		UWorld* World = MasterWorld.Get();
		if (!bMasterRecording || !World) return false;
		UAudioMixerBlueprintLibrary::StopRecordingOutput(
			World, EAudioRecordingExportType::WavFile, MasterStem, State->Directory);
		bMasterRecording = false;
		MasterStopSeconds = FPlatformTime::Seconds();
		OutPath = FPaths::Combine(State->Directory, MasterStem + TEXT(".wav"));
		MasterLastBytes = -1;
		MasterStablePolls = 0;
		return true;
	}
	bool PollMasterWaveStable(double Now, bool& OutStable)
	{
		OutStable = false;
		if (Now - MasterStopSeconds > IMS4MasterWaveSettleTimeoutSeconds) return false;
		const int64 Bytes = IFileManager::Get().FileSize(*MasterWavPath);
		if (Bytes <= 44) { MasterLastBytes = -1; MasterStablePolls = 0; return true; }
		if (Bytes == MasterLastBytes) ++MasterStablePolls;
		else { MasterLastBytes = Bytes; MasterStablePolls = 0; }
		OutStable = MasterStablePolls >= 1;
		return true;
	}
	bool SaveTraceFiles() const
	{
		if (!Context.IsValid()) return false;
		const uint32 SrcEnd = FMath::Max(OnSourceEnd, SourceTraceCount());
		const uint32 EnvEnd = FMath::Max(OnEnvironmentEnd, EnvironmentTraceCount());
		FString Env = TEXT("phase,index,frame,seconds,fresh,sequence,spatial_version,expected_spatial_version\n");
		for (uint32 I = BaselineEnvironmentBegin; I < EnvEnd; ++I)
		{
			const FIMAcousticMetaSoundBlock& B = Context->CapturedBlocks[int32(I)];
			const TCHAR* PhaseName = I < BaselineEnvironmentEnd ? TEXT("baseline")
				: (OnEnvironmentBegin && I >= OnEnvironmentBegin ? TEXT("reverb_on") : TEXT("reverb_off"));
			Env += FString::Printf(TEXT("%s,%u,%llu,%.9f,%d,%llu,%llu,%llu\n"), PhaseName, I,
				B.Frame, B.Seconds, B.Fresh ? 1 : 0, B.Sequence, B.SpatialVersion, B.ExpectedSpatialVersion);
		}
		FString Src = TEXT("phase,index,block,callback_audio_id,result_audio_id,world_generation,voice_generation,spatial_version,sequence,routes,reject,output_energy\n");
		for (uint32 I = BaselineSourceBegin; I < SrcEnd; ++I)
		{
			const FIMAcousticBlockProbe& B = Context->CapturedSourceBlocks[int32(I)];
			if (B.CallbackAudioComponentId != AudioId) continue;
			const TCHAR* PhaseName = I < BaselineSourceEnd ? TEXT("baseline")
				: (OnSourceBegin && I >= OnSourceBegin ? TEXT("reverb_on") : TEXT("reverb_off"));
			Src += FString::Printf(TEXT("%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%.12g\n"),
				PhaseName, I, B.Block, B.CallbackAudioComponentId, B.ResultAudioComponentId,
				B.ResultWorldGeneration, B.ResultVoiceGeneration, B.ResultSpatialVersion, B.ResultSequence,
				B.Routes, uint32(B.Reject), B.OutputEnergy);
		}
		return FFileHelper::SaveStringToFile(Env, *FPaths::Combine(State->Directory, TEXT("environment-lease-trace.csv")),
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& FFileHelper::SaveStringToFile(Src, *FPaths::Combine(State->Directory, TEXT("source-route-trace.csv")),
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	bool SaveReport(const TCHAR* FileName, const TCHAR* Status, const FString& Failure) const
	{
		FString JSON;
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JSON);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("status"), Status);
		Writer->WriteValue(TEXT("test"), TEXT("IceMoon.AcousticField.S4.IRLeaseReleaseReacquireSameWorld"));
		Writer->WriteValue(TEXT("map"), IMS4ReplayMap);
		Writer->WriteValue(TEXT("world_generation"), double(WorldGeneration));
		Writer->WriteValue(TEXT("audio_device_id"), double(AudioDeviceId));
		Writer->WriteValue(TEXT("audio_component_id"), double(AudioId));
		Writer->WriteValue(TEXT("voice_slot"), VoiceSlot);
		Writer->WriteValue(TEXT("voice_generation"), double(BaselineVoice));
		Writer->WriteValue(TEXT("listener_spatial_version"), double(BaselineSpatialVersion));
		Writer->WriteValue(TEXT("route_variable"), TEXT("bReverbRoute on -> off -> on; direct/path flags and gains held fixed."));
		Writer->WriteValue(TEXT("dry_routes_unchanged"), Volume.IsValid()
			&& Volume->bDirectRoute == OriginalDirectRoute && Volume->bPathRoute == OriginalPathRoute);
		Writer->WriteValue(TEXT("volume_wet_gain_unchanged"), Volume.IsValid()
			&& FMath::IsNearlyEqual(Volume->ReverbWetGain, OriginalVolumeWetGain, 1.0e-6f));
		Writer->WriteValue(TEXT("baseline_old_ir_sequence"), double(BaselineSequence));
		Writer->WriteValue(TEXT("baseline_ir_spatial_version"), double(BaselineSpatialVersion));
		Writer->WriteValue(TEXT("off_route_request_seconds"), OffRequestSeconds);
		Writer->WriteValue(TEXT("off_route_observed_seconds"), OffRouteObservedSeconds);
		Writer->WriteValue(TEXT("off_all_lease_slots_free"), OffStableFreePolls >= 3);
		Writer->WriteValue(TEXT("off_released_environment_trace_seen"), OffSawReleasedTrace);
		Writer->WriteValue(TEXT("off_fresh_blocks"), double(OffFreshBlocks));
		Writer->WriteValue(TEXT("off_wet_nonzero_samples"), double(OffWetNonzeroSamples));
		Writer->WriteValue(TEXT("off_dry_accepted_blocks"), double(OffAccepted));
		Writer->WriteValue(TEXT("off_dry_output_blocks"), double(OffOutput));
		Writer->WriteValue(TEXT("on_route_request_seconds"), OnRequestSeconds);
		Writer->WriteValue(TEXT("on_fresh_gate_seconds"), OnFreshGateSeconds);
		Writer->WriteValue(TEXT("on_new_ir_sequence"), double(OnSequence));
		Writer->WriteValue(TEXT("on_ir_spatial_version"), double(OnSpatialVersion));
		Writer->WriteValue(TEXT("on_ir_expected_spatial_version"), double(OnExpectedSpatialVersion));
		Writer->WriteValue(TEXT("on_sequence_advanced"), OnSequence > BaselineSequence);
		Writer->WriteValue(TEXT("on_wet_pcm_nonzero"), HasWetEnergy(OnWetBegin, OnWetEnd));
		Writer->WriteValue(TEXT("on_dry_accepted_blocks"), double(OnAccepted));
		Writer->WriteValue(TEXT("on_dry_output_blocks"), double(OnOutput));
		Writer->WriteValue(TEXT("master_wav_path"), MasterWavPath);
		Writer->WriteValue(TEXT("master_output_gate_passed"), bMasterOutputGate);
		Writer->WriteValue(TEXT("master_record_start_seconds"), MasterStartSeconds);
		Writer->WriteValue(TEXT("master_record_stop_seconds"), MasterStopSeconds);
		Writer->WriteValue(TEXT("master_wav_proves_device_output_only"), true);
		Writer->WriteValue(TEXT("alignment_note"), IMS4MasterAlignmentNote);
		IM_S4WriteMasterWaveSummary(Writer, MasterWave);
		Writer->WriteValue(TEXT("failure"), Failure);
		Writer->WriteValue(TEXT("master_recorder_active_at_failure"), bRecorderActiveAtFinish || bMasterRecording);
		Writer->WriteValue(TEXT("master_recorder_stop_attempted_at_failure"), bRecorderStopAttemptedAtFinish);
		Writer->WriteValue(TEXT("incomplete_wav_path"), IncompleteWavPath);
		Writer->WriteValue(TEXT("original_reverb_route_restored"), bOriginalRouteRestored);
		Writer->WriteValue(TEXT("test_started_source_stopped"), bStartedSourceForTest ? bStartedSourceStopped : true);
		Writer->WriteObjectEnd();
		Writer->Close();
		return FFileHelper::SaveStringToFile(JSON, *FPaths::Combine(State->Directory, FileName),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	bool SaveEvidence()
	{
		const bool bTraceSaved = SaveTraceFiles();
		return SaveReport(TEXT("capture.json"), TEXT("CAPTURED_IR_LEASE_REACQUIRE_PASS"), TEXT("")) && bTraceSaved;
	}
	void StopOwnedRecorderForFailure()
	{
		bRecorderActiveAtFinish = bMasterRecording;
		if (!bMasterRecording) return;
		UWorld* World = MasterWorld.Get();
		if (!World) return;
		bRecorderStopAttemptedAtFinish = true;
		UAudioMixerBlueprintLibrary::StopRecordingOutput(
			World, EAudioRecordingExportType::WavFile, TEXT("s4-ir-lease-on-incomplete"), State->Directory);
		IncompleteWavPath = FPaths::Combine(State->Directory, TEXT("s4-ir-lease-on-incomplete.wav"));
		bMasterRecording = false;
	}
	void RestoreTestChanges()
	{
		if (Volume.IsValid() && bSettingsCaptured)
		{
			Volume->bReverbRoute = OriginalReverbRoute;
			bOriginalRouteRestored = Volume->bReverbRoute == OriginalReverbRoute;
		}
		if (bStartedSourceForTest && Source.IsValid() && Source->IsPlaying())
		{
			Source->Stop();
			bStartedSourceStopped = true;
		}
	}
	bool Finish(bool Success, const FString& Message)
	{
		if (Finished) return true;
		Finished = true;
		if (!Success)
		{
			StopOwnedRecorderForFailure();
			RestoreTestChanges();
			if (!SaveTraceFiles() || !SaveReport(TEXT("failure.json"), TEXT("FAIL_IR_LEASE_REACQUIRE"), Message))
				UE_LOG(LogTemp, Error, TEXT("IMLogs AcousticS4IRLease could not persist failure evidence at %s."), *State->Directory);
			if (State->Test) State->Test->AddError(Message);
		}
		else RestoreTestChanges();
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(State->OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = State->bOriginalBackgroundAudio;
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticS4IRLease %s evidence=%s"), *Message, *State->Directory);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticS4IRLease %s"), Success ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), Success ? TEXT("PASS") : TEXT("FAIL"));
		if (GEditor && GEditor->PlayWorld) GEditor->RequestEndPlayMap();
		return true;
	}

	TSharedRef<FIMAcousticS4ReplayState> State;
	int32 Stage = 0;
	bool Finished = false;
	bool bSettingsCaptured = false;
	bool bStartedSourceForTest = false;
	bool bStartedSourceStopped = false;
	bool bOffCaptureStarted = false;
	bool bMasterRecording = false;
	bool bMasterOutputGate = false;
	bool bRecorderActiveAtFinish = false;
	bool bRecorderStopAttemptedAtFinish = false;
	bool bOriginalRouteRestored = false;
	bool OriginalEnableV2 = false, OriginalDirectRoute = false, OriginalPathRoute = false, OriginalReverbRoute = false;
	bool bBaselineVoiceConsistent = true, bOffVoiceConsistent = true, bOnVoiceConsistent = true;
	bool OffSawReleasedTrace = false;
	uint32 AudioDeviceId = 0, MasterMixerBlockFrames = 0, ExpectedOnRouteMask = 0;
	int32 MasterSampleRate = 0;
	uint32 MasterStablePolls = 0, OffStableFreePolls = 0, OffFreshBlocks = 0;
	uint32 BaselineSourceBegin = 0, BaselineSourceEnd = 0, BaselineEnvironmentBegin = 0, BaselineEnvironmentEnd = 0;
	uint32 BaselineWetBegin = 0, BaselineWetEnd = 0;
	uint32 OffSourceBegin = 0, OffSourceEnd = 0, OffEnvironmentBegin = 0, OffEnvironmentEnd = 0, OffWetBegin = 0, OffWetEnd = 0;
	uint32 OnSourceBegin = 0, OnSourceEnd = 0, OnEnvironmentBegin = 0, OnEnvironmentEnd = 0, OnWetBegin = 0, OnWetEnd = 0;
	int32 VoiceSlot = INDEX_NONE;
	uint64 WorldGeneration = 0, AudioId = 0, BaselineVoice = 0, BaselineSequence = 0;
	uint64 BaselineSpatialVersion = 0, BaselineExpectedSpatialVersion = 0;
	uint64 BaselineAccepted = 0, BaselineOutput = 0, OffAccepted = 0, OffOutput = 0, OffVoice = 0;
	uint64 OnAccepted = 0, OnOutput = 0, OnVoice = 0, OnSequence = 0, OnSpatialVersion = 0, OnExpectedSpatialVersion = 0;
	uint64 OffWetNonzeroSamples = 0;
	int64 MasterLastBytes = -1;
	float OriginalVolumeWetGain = 0.0f, OriginalContextWetGain = 0.0f, MasterMixerSampleRateHz = 0.0f;
	double BaselineGateSeconds = 0.0, OffRequestSeconds = 0.0, OffRouteObservedSeconds = 0.0;
	double OnRequestSeconds = 0.0, OnFreshGateSeconds = 0.0, MasterStartSeconds = 0.0, MasterStopSeconds = 0.0;
	FString MasterStem, MasterWavPath, IncompleteWavPath;
	TWeakObjectPtr<UWorld> WorldIdentity, MasterWorld;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<UAudioComponent> Source;
	FIMAcousticMetaSoundContextPtr Context;
	FIMAcousticS4MasterWaveSummary MasterWave;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticS4ReplaySameComponent,
	"IceMoon.AcousticField.S4.ReplaySameComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticS4ReplaySameComponent::RunTest(const FString&)
{
	auto State = MakeShared<IMAcousticS4ReplayTestPrivate::FIMAcousticS4ReplayState>();
	State->Test = this;
	State->Started = FPlatformTime::Seconds();
	State->OriginalBackgroundVolume = FApp::GetUnfocusedVolumeMultiplier();
	State->bOriginalBackgroundAudio = GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	State->Directory = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/S4-ReplaySameComponent"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*State->Directory, true);

	FApp::SetUnfocusedVolumeMultiplier(1.0f);
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FString Error;
	GUnrealEd->AutomationLoadMap(IMAcousticS4ReplayTestPrivate::IMS4ReplayMap, false, &Error);
	if (!Error.IsEmpty())
	{
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(State->OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = State->bOriginalBackgroundAudio;
		AddError(Error);
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticS4ReplayTestPrivate::FIMAcousticS4ReplayCommand(State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticS4IRLeaseReleaseReacquireSameWorld,
	"IceMoon.AcousticField.S4.IRLeaseReleaseReacquireSameWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticS4IRLeaseReleaseReacquireSameWorld::RunTest(const FString&)
{
	auto State = MakeShared<IMAcousticS4ReplayTestPrivate::FIMAcousticS4ReplayState>();
	State->Test = this;
	State->Started = FPlatformTime::Seconds();
	State->OriginalBackgroundVolume = FApp::GetUnfocusedVolumeMultiplier();
	State->bOriginalBackgroundAudio = GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
	State->Directory = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/S4-IRLeaseReleaseReacquireSameWorld"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*State->Directory, true);

	FApp::SetUnfocusedVolumeMultiplier(1.0f);
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 16384, 16384, true);
	FString Error;
	GUnrealEd->AutomationLoadMap(IMAcousticS4ReplayTestPrivate::IMS4ReplayMap, false, &Error);
	if (!Error.IsEmpty())
	{
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(State->OriginalBackgroundVolume);
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = State->bOriginalBackgroundAudio;
		AddError(Error);
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticS4ReplayTestPrivate::FIMAcousticS4IRLeaseReacquireCommand(State));
	return true;
}
#endif
