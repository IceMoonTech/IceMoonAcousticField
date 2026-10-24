#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSpatialization.h"
#include "Audio.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioThread.h"
#include "ISubmixBufferListener.h"
#include "Sound/SoundSubmix.h"
#include "AudioMixerBlueprintLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "MetasoundSource.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Sound/SoundWave.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealEdGlobals.h"
#include "UObject/Package.h"

namespace IMAcousticQ5RoomGraphTestPrivate
{
constexpr int32 IMQ5Rate = 48000;
constexpr double IMQ5CaptureSeconds = 6.0;
constexpr double IMQ5ExpectedTriggerSeconds = 0.5;
constexpr double IMQ5TriggerToleranceSeconds = 0.2;
constexpr double IMQ5ClosedRelativeGap = 0.10;
constexpr double IMQ5FloatSilenceEnergy = 1.0e-20;
constexpr TCHAR IMQ5SourcePath[] = TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticQ5Impulse.MS_AcousticQ5Impulse");
constexpr TCHAR IMQ5WavePath[] = TEXT("/IceMoonAcousticField/Tests/Audio/SW_AcousticQ5Impulse.SW_AcousticQ5Impulse");
constexpr TCHAR IMQ5EnvironmentPath[] = TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticEnvironment.MS_AcousticEnvironment");
constexpr TCHAR IMQ5RoomGraphExpectedSourceSha1[] = TEXT("6dd1c837a04230771ad19b2b6802d38834247da7");
constexpr TCHAR IMQ5RoomGraphExpectedWaveSha1[] = TEXT("3f720a2536e69ddf7599c4c7413b693ff1300882");
constexpr TCHAR IMQ5RoomGraphExpectedEnvironmentSha1[] = TEXT("560d3f3c7b5d3306338e778479ee314aee877d89");

struct FIMQ5RoomCase
{
	const TCHAR* Name;
	const TCHAR* Geometry;
	double HalfX;
	double HalfZ;
	double Absorption;
	double ListenerX;
	bool bCeiling;
};

constexpr FIMQ5RoomCase IMQ5Cases[] = {
	{TEXT("closed-low"), TEXT("sealed-room-8x3x6"), 4.0, 3.0, 0.1, 2.0, true},
	{TEXT("closed-high"), TEXT("sealed-room-8x3x6"), 4.0, 3.0, 0.8, 2.0, true},
	{TEXT("corridor"), TEXT("sealed-duct-20x3x2"), 10.0, 1.0, 0.1, 6.0, true},
	{TEXT("outdoor-notop"), TEXT("walled-floor-20x20-notop"), 10.0, 10.0, 0.1, 2.0, false}
};

struct FIMQ5Decay
{
	bool bUsable = false;
	int32 Frames = 0;
	int32 FirstNonzeroFrame = INDEX_NONE;
	int32 FitStartFrame = INDEX_NONE;
	int32 FitEndFrame = INDEX_NONE;
	int32 FitSamples = 0;
	double TotalEnergy = 0.0;
	double Peak = 0.0;
	double LeadingEnergy = 0.0;
	double TrailingEnergy = 0.0;
	double SlopeDbPerSecond = 0.0;
	double T20Seconds = 0.0;
	FString Error;
};

struct FIMQ5DeviceCounters
{
	uint64 Direct = 0;
	uint64 Path = 0;
	uint64 Rejected = 0;
	uint64 ReverbRejected = 0;
	uint64 DryDropped = 0;
	uint64 WetNonzero = 0;
	uint64 ReverbProcessed = 0;
	uint64 ReverbDry = 0;
};

// The GT reads separate atomic publications within this explicit bracket.
// It is a conservative graph cutoff beside synchronous recorder Stop, not an
// asserted sample-perfect alignment with the asynchronously exported WAV.
struct FIMQ5GraphCaptureStop
{
	uint32 EnvironmentPcmEnd = 0, EnvironmentTraceEnd = 0;
	uint32 SourcePcmEnd = 0, SourceTraceEnd = 0, BusTraceEnd = 0;
	uint64 BusUnderrunsBeforeRecorderStop = 0, BusUnderruns = 0;
	uint64 PushBlocks = 0, PushFailures = 0;
	double BeginSeconds = 0.0, EndSeconds = 0.0;
	FIMQ5DeviceCounters CounterDelta;
	bool TransportFailed = false, Frozen = false;
};

struct FIMQ5SavedAsset
{
	FString ObjectPath;
	FString DiskPath;
	FString Sha1Before;
	FString Sha1After;
	FString ExpectedSha1;
	UObject* Object = nullptr;
	UPackage* Package = nullptr;
	bool bDirtyBefore = false;
	bool bDirtyAfter = false;
};

struct FIMQ5GeneratedPackage
{
	FString PackageName;
	FString DiskPath;
	FString Sha1BeforeCleanup;
	FString Sha1AfterRestore;
	bool bIsMap = false;
	bool bRemoved = false;
};

bool IMQ5WriteJson(const FString& File, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const auto Writer = TJsonWriterFactory<>::Create(&Text);
	return FJsonSerializer::Serialize(Object, Writer)
		&& FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool IMQ5HashFile(const FString& File, FString& OutSha1)
{
	TArray<uint8> Bytes;
	OutSha1.Reset();
	if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.IsEmpty()) return false;
	OutSha1 = FSHA1::HashBuffer(Bytes.GetData(), uint64(Bytes.Num())).ToString().ToLower();
	return true;
}

bool IMQ5WriteFloatWindow(const FString& File, const TArray<float>& Pcm, uint32 BeginFrame, uint32 EndFrame, int32 Channels)
{
	if (Channels <= 0 || EndFrame < BeginFrame || uint64(EndFrame) * Channels > uint64(Pcm.Num())) return false;
	const uint64 ByteCount = uint64(EndFrame - BeginFrame) * uint64(Channels) * sizeof(float);
	if (ByteCount > uint64(MAX_int32)) return false;
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(int32(ByteCount));
	if (ByteCount > 0)
	{
		FMemory::Memcpy(Bytes.GetData(), Pcm.GetData() + uint64(BeginFrame) * Channels, SIZE_T(ByteCount));
	}
	return FFileHelper::SaveArrayToFile(Bytes, *File);
}

bool IMQ5AnalyzeStereoFloat(
	const float* Samples,
	int32 Frames,
	double SilenceEnergyThreshold,
	const FString& CsvFile,
	FIMQ5Decay& Out,
	bool bRequireDevicePreTriggerWindow)
{
	Out = {};
	Out.Frames = Frames;
	const double MinimumWindowSeconds = bRequireDevicePreTriggerWindow
		? IMQ5CaptureSeconds - 0.5
		: IMQ5CaptureSeconds - IMQ5ExpectedTriggerSeconds - IMQ5TriggerToleranceSeconds;
	const int32 MinimumWindowFrames = int32(MinimumWindowSeconds * IMQ5Rate);
	if (!Samples || Frames < MinimumWindowFrames)
	{
		Out.Error = FString::Printf(TEXT("Stereo wet window is absent or shorter than its %.3fs capture contract."), MinimumWindowSeconds);
		return false;
	}

	TArray<double> FrameEnergy;
	FrameEnergy.SetNumUninitialized(Frames);
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		const double Left = Samples[2 * Frame];
		const double Right = Samples[2 * Frame + 1];
		const double Energy = Left * Left + Right * Right;
		FrameEnergy[Frame] = Energy;
		Out.TotalEnergy += Energy;
		Out.Peak = FMath::Max(Out.Peak, FMath::Max(FMath::Abs(Left), FMath::Abs(Right)));
		if (Energy > SilenceEnergyThreshold && Out.FirstNonzeroFrame == INDEX_NONE) Out.FirstNonzeroFrame = Frame;
		if (Frame < IMQ5Rate / 4) Out.LeadingEnergy += Energy;
		if (Frame >= Frames - IMQ5Rate / 2) Out.TrailingEnergy += Energy;
	}

	const int32 MinimumFirstNonzeroFrame = bRequireDevicePreTriggerWindow ? IMQ5Rate / 4 : 0;
	if (!(Out.TotalEnergy > SilenceEnergyThreshold)
		|| Out.Peak >= (32760.0 / 32768.0)
		|| (bRequireDevicePreTriggerWindow && Out.LeadingEnergy > SilenceEnergyThreshold)
		|| Out.TrailingEnergy > SilenceEnergyThreshold
		|| Out.FirstNonzeroFrame < MinimumFirstNonzeroFrame
		|| Out.FirstNonzeroFrame > IMQ5Rate * 1.5
		|| Frames - Out.FirstNonzeroFrame < int32((IMAcousticRecipe::ReverbSavedDurationS + 0.5) * IMQ5Rate))
	{
		Out.Error = bRequireDevicePreTriggerWindow
			? TEXT("Device wet impulse is missing, clipped, pre-trigger-contaminated, late, or has insufficient recorded decay tail.")
			: TEXT("Graph wet impulse is missing, clipped, late from context start, or has insufficient recorded decay tail.");
		return false;
	}

	TArray<double> EdcDb;
	EdcDb.SetNumUninitialized(Frames);
	double SuffixEnergy = 0.0;
	for (int32 Frame = Frames - 1; Frame >= 0; --Frame)
	{
		SuffixEnergy += FrameEnergy[Frame];
		EdcDb[Frame] = 10.0 * FMath::LogX(10.0, FMath::Max(SuffixEnergy / Out.TotalEnergy, 1.0e-30));
	}

	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		if (Out.FitStartFrame == INDEX_NONE && EdcDb[Frame] <= -5.0) Out.FitStartFrame = Frame;
		if (Out.FitEndFrame == INDEX_NONE && EdcDb[Frame] <= -25.0) Out.FitEndFrame = Frame;
	}
	FString Csv = TEXT("time_s,frame_energy,edc_db\n");
	for (int32 Frame = 0; Frame < Frames; Frame += IMQ5Rate / 100)
	{
		Csv += FString::Printf(TEXT("%.9g,%.17g,%.17g\n"), double(Frame) / IMQ5Rate, FrameEnergy[Frame], EdcDb[Frame]);
	}
	if (!FFileHelper::SaveStringToFile(Csv, *CsvFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Out.Error = TEXT("Could not save the 10 ms wet energy/EDC trace.");
		return false;
	}
	if (Out.FitStartFrame == INDEX_NONE || Out.FitEndFrame <= Out.FitStartFrame)
	{
		Out.Error = TEXT("No usable -5 to -25 dB EDC fitting interval.");
		return false;
	}

	double SumT = 0.0, SumD = 0.0, SumTT = 0.0, SumTD = 0.0;
	for (int32 Frame = Out.FitStartFrame; Frame <= Out.FitEndFrame; ++Frame)
	{
		const double Time = double(Frame - Out.FitStartFrame) / IMQ5Rate;
		const double Db = EdcDb[Frame];
		SumT += Time;
		SumD += Db;
		SumTT += Time * Time;
		SumTD += Time * Db;
	}
	Out.FitSamples = Out.FitEndFrame - Out.FitStartFrame + 1;
	const double Denominator = Out.FitSamples * SumTT - SumT * SumT;
	if (!(Denominator > 0.0))
	{
		Out.Error = TEXT("Degenerate -5 to -25 dB EDC fit.");
		return false;
	}
	Out.SlopeDbPerSecond = (Out.FitSamples * SumTD - SumT * SumD) / Denominator;
	Out.bUsable = FMath::IsFinite(Out.SlopeDbPerSecond) && Out.SlopeDbPerSecond < 0.0;
	if (!Out.bUsable)
	{
		Out.Error = TEXT("Wet EDC did not decay through the declared fit interval.");
		return false;
	}
	Out.T20Seconds = -60.0 / Out.SlopeDbPerSecond;
	return FMath::IsFinite(Out.T20Seconds) && Out.T20Seconds > 0.0;
}

bool IMQ5AnalyzeDeviceWave(const FString& WavFile, const FString& CsvFile, FIMQ5Decay& Out)
{
	TArray<uint8> Bytes;
	FWaveModInfo Info;
	if (!FFileHelper::LoadFileToArray(Bytes, *WavFile)
		|| !Info.ReadWaveInfo(Bytes.GetData(), Bytes.Num())
		|| !Info.pBitsPerSample || *Info.pBitsPerSample != 16
		|| !Info.pChannels || *Info.pChannels != 2
		|| !Info.pSamplesPerSec || *Info.pSamplesPerSec != IMQ5Rate
		|| Info.SampleDataSize % 4 != 0)
	{
		Out.Error = TEXT("Device recording must be 48 kHz stereo PCM16 WAV.");
		return false;
	}
	const int32 Frames = Info.SampleDataSize / 4;
	TArray<float> Samples;
	Samples.SetNumUninitialized(Frames * 2);
	for (int32 Sample = 0; Sample < Frames * 2; ++Sample)
	{
		int16 Pcm = 0;
		FMemory::Memcpy(&Pcm, Info.SampleDataStart + Sample * sizeof(int16), sizeof(int16));
		Samples[Sample] = float(Pcm) / 32768.0f;
	}
	return IMQ5AnalyzeStereoFloat(Samples.GetData(), Frames, 0.0, CsvFile, Out, true);
}

// Private diagnostic scope: completion acknowledges metadata and cleanup only.
// Null product fields prevent this single window from impersonating Q5 T20 acceptance.
void IMQ5SetOrderOnlyScope(FJsonObject& Report, bool bCompleted)
{
	Report.SetStringField(TEXT("observation_scope"), TEXT("closed-low-order-only"));
	Report.SetBoolField(TEXT("observation_completed"), bCompleted);
	Report.SetStringField(TEXT("original_product_status"), TEXT("NOT_EVALUATED"));
	Report.SetStringField(TEXT("observation_causal_status"), TEXT("INCONCLUSIVE_CAUSE_NOT_ESTABLISHED"));
	Report.SetField(TEXT("passed"), MakeShared<FJsonValueNull>());
	Report.SetField(TEXT("original_native_case_pass"), MakeShared<FJsonValueNull>());
	Report.SetField(TEXT("device_and_graph_wet_fits_pass"), MakeShared<FJsonValueNull>());
}

TSharedRef<FJsonObject> IMQ5DecayJson(const FIMQ5Decay& D)
{
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("usable"), D.bUsable);
	J->SetNumberField(TEXT("frames"), D.Frames);
	J->SetNumberField(TEXT("total_energy"), D.TotalEnergy);
	J->SetNumberField(TEXT("peak_abs_normalized"), D.Peak);
	J->SetNumberField(TEXT("leading_quarter_second_energy"), D.LeadingEnergy);
	J->SetNumberField(TEXT("trailing_half_second_energy"), D.TrailingEnergy);
	J->SetNumberField(TEXT("first_nonzero_frame"), D.FirstNonzeroFrame);
	J->SetNumberField(TEXT("fit_start_minus5_db_frame"), D.FitStartFrame);
	J->SetNumberField(TEXT("fit_end_minus25_db_frame"), D.FitEndFrame);
	J->SetNumberField(TEXT("fit_samples"), D.FitSamples);
	J->SetNumberField(TEXT("slope_db_per_second"), D.SlopeDbPerSecond);
	J->SetNumberField(TEXT("t20_seconds"), D.T20Seconds);
	J->SetStringField(TEXT("error"), D.Error);
	return J;
}

FString IMQ5PIEMapPath(UWorld* World)
{
	return World ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) : FString();
}

UWorld* IMQ5FindPIEWorld()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE) return Context.World();
	}
	return nullptr;
}

bool IMQ5DeletePackageFiles(const FString& DiskPath)
{
	const FString Files[] = {
		DiskPath,
		FPaths::ChangeExtension(DiskPath, TEXT("uexp")),
		FPaths::ChangeExtension(DiskPath, TEXT("ubulk")),
		FPaths::ChangeExtension(DiskPath, TEXT("uptnl"))
	};
	bool bOk = true;
	for (const FString& File : Files)
	{
		if (IFileManager::Get().FileExists(*File))
		{
			bOk = IFileManager::Get().Delete(*File, false, true, true) && bOk;
		}
		bOk = !IFileManager::Get().FileExists(*File) && bOk;
	}
	return bOk;
}

struct FIMQ5MasterBlock
{
	uint64 FrameBegin = 0;
	int32 Frames = 0;
	int32 Channels = 0;
	int32 Rate = 0;
	uint8 RequestPhase = 0;
	double AudioClock = 0.0;
	double WallSeconds = 0.0;
	double Energy = 0.0;
	float Peak = 0.0f;
};

// GT allocates the ledger. One mixer render producer publishes immutable rows;
// GT only reads the acquire-published prefix. No callback changes AudioData.
class FIMQ5MasterRecorderListener final : public ISubmixBufferListener
{
public:
	explicit FIMQ5MasterRecorderListener(const USoundSubmix* InSubmix) : Submix(InSubmix)
	{
		Blocks.SetNum(2048);
	}

	bool IsRenderingAudio() const override { return bKeepAlive.load(std::memory_order_acquire); }
	void SetKeepAlive(bool Value) { bKeepAlive.store(Value, std::memory_order_release); }
	void SetRequestPhase(uint8 Value) { RequestPhase.store(Value, std::memory_order_release); }
	bool IsValidLedger() const { return !bInvalid.load(std::memory_order_acquire); }
	uint32 GetBlockCount() const { return Published.load(std::memory_order_acquire); }

	uint64 GetFrameCount() const
	{
		const uint32 Count = GetBlockCount();
		return Count ? Blocks[Count - 1].FrameBegin + uint64(Blocks[Count - 1].Frames) : 0;
	}

	TArray<FIMQ5MasterBlock> Snapshot() const
	{
		const uint32 Count = GetBlockCount();
		TArray<FIMQ5MasterBlock> Result;
		Result.Append(Blocks.GetData(), int32(Count));
		return Result;
	}

	void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData,
		int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override
	{
		const uint32 Index = Published.load(std::memory_order_relaxed);
		if (OwningSubmix != Submix || !AudioData || NumChannels != 2 || SampleRate != IMQ5Rate
			|| NumSamples <= 0 || NumSamples % NumChannels != 0 || Index >= uint32(Blocks.Num()))
		{
			bInvalid.store(true, std::memory_order_release);
			return;
		}
		FIMQ5MasterBlock& Row = Blocks[Index];
		Row.FrameBegin = RenderFrames;
		Row.Frames = NumSamples / NumChannels;
		Row.Channels = NumChannels;
		Row.Rate = SampleRate;
		Row.RequestPhase = RequestPhase.load(std::memory_order_acquire);
		Row.AudioClock = AudioClock;
		Row.WallSeconds = FPlatformTime::Seconds();
		for (int32 Sample = 0; Sample < NumSamples; ++Sample)
		{
			const float Value = AudioData[Sample];
			Row.Energy += double(Value) * Value;
			Row.Peak = FMath::Max(Row.Peak, FMath::Abs(Value));
		}
		RenderFrames += uint64(Row.Frames);
		Published.store(Index + 1, std::memory_order_release);
	}

private:
	const USoundSubmix* const Submix;
	TArray<FIMQ5MasterBlock> Blocks;
	uint64 RenderFrames = 0;
	std::atomic<uint32> Published{0};
	std::atomic<uint8> RequestPhase{0};
	std::atomic<bool> bKeepAlive{true};
	std::atomic<bool> bInvalid{false};
};

struct FIMQ5RunState
{
	explicit FIMQ5RunState(FAutomationTestBase* InTest)
		: Test(InTest),
			RunId(FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()),
			EvidenceDirectory(FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/Q5RoomGraph"), RunId))),
			MountRoot(TEXT("/IceMoonAcousticField/Tests/Rooms/IM_Q5_" ) + RunId),
			BackgroundVolume(FApp::GetUnfocusedVolumeMultiplier()),
			PreviousThrottle(GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground),
			PreviousAllowBackgroundAudio(GetDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio)
	{
	}

	void ReleaseCapturePreparation()
	{
		IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(CapturePreparationId);
		CapturePreparationId = 0;
	}

	void RestoreGlobals()
	{
		ReleaseCapturePreparation();
		if (bGlobalsRestored) return;
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		FApp::SetUnfocusedVolumeMultiplier(BackgroundVolume);
		GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = PreviousThrottle;
		GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = PreviousAllowBackgroundAudio;
		bGlobalsRestored = true;
	}

	void ReleaseMasterRecorderListener()
	{
		if (!bMasterRecorderRegistered || !MasterRecorder.IsValid()) return;
		MasterRecorder->SetKeepAlive(false);
		if (UWorld* PIE = IMQ5FindPIEWorld())
		{
			if (FAudioDevice* Device = PIE->GetAudioDeviceRaw(); Device && Device->DeviceID == AudioDeviceId)
			{
				Device->UnregisterSubmixBufferListener(MasterRecorder.ToSharedRef(), Device->GetMainSubmixObject());
				RecorderFence.BeginFence();
			}
		}
		bMasterRecorderRegistered = false;
	}

	~FIMQ5RunState() { ReleaseMasterRecorderListener(); RestoreGlobals(); }

	FAutomationTestBase* Test = nullptr;
	FString RunId;
	FString EvidenceDirectory;
	FString MountRoot;
	FString OriginalMapPath;
	FString OriginalMapDiskPath;
	FString OriginalMapSha1Before;
	FString OriginalMapSha1After;
	FString ClosedGeometryIdentity;
	FString CurrentMapPath;
	FString CurrentMapDiskPath;
	FString CurrentMapSha1;
	FString CurrentBakePackage;
	FString CurrentBakeDiskPath;
	FString CurrentBakeSha1;
	FString Failure;
	FString FinalMessage;
	FString SourceAssetPath;
	FString GraphTriggerWallClockStart;
	float BackgroundVolume = 1.0f;
	bool PreviousThrottle = false;
	bool PreviousAllowBackgroundAudio = false;
	int32 CaseIndex = 0;
	int32 CasesCompleted = 0;
	bool bFinalPass = false;
	bool bRecording = false;
	bool bCaptureTransportOrder = false, bTransportOrderValid = false;
	// These flags belong to the GT latent command, never the audio payload.
	bool bOrderOnlyClosedLow = false;
	bool bOrderOnlyMetadataComplete = false, bOrderOnlyObservationComplete = false;
	uint32 TransportClaimsAtStop = 0;
	double TransportCutBeginSeconds = 0.0, TransportCutEndSeconds = 0.0;
	bool bMasterRecorderRegistered = false;
	uint64 MasterStartRequestFrame = 0;
	uint64 MasterTriggerRenderFrame = 0;
	uint64 MasterStopBeforeFrame = 0;
	uint64 MasterStopAfterFrame = 0;
	FAudioCommandFence RecorderFence;
	TSharedPtr<FIMQ5MasterRecorderListener, ESPMode::ThreadSafe> MasterRecorder;
	bool bSourceSpawned = false;
	bool bRestoreAttempted = false;
	bool bOriginalMapRestored = false;
	bool bAssetsRestored = false;
	bool bGeneratedContentRemoved = false;
	bool bGlobalsRestored = false;
	bool bTerminalWritten = false;
	bool bSourceWorldWarm = false;
	uint64 CapturePreparationId = 0;
	double CapturePreparationStartedSeconds = 0.0;
	double CapturePreparationEndedSeconds = 0.0;
	double CaptureStartedSeconds = 0.0;
	double TriggerRequestedSeconds = 0.0;
	double GraphContextObservedSeconds = 0.0;
	int64 LastWavBytes = -1;
	uint32 AudioDeviceId = 0;
	uint32 EnvironmentPcmBegin = 0;
	uint32 EnvironmentTraceBegin = 0;
	uint32 SourcePcmBegin = 0;
	uint32 SourceTraceBegin = 0;
	uint32 EnvironmentFramesAtFirstObservation = 0;
	uint32 SourceFramesAtFirstObservation = 0;
	uint32 EnvironmentTraceRowsAtFirstObservation = 0;
	uint32 SourceTraceRowsAtFirstObservation = 0;
	uint32 GraphWetFramesRecorded = 0;
	FIMQ5DeviceCounters CountersBegin;
	FIMQ5GraphCaptureStop GraphStop;
	uint64 BusUnderrunsBegin = 0;
	uint64 WarmEnvironmentBlocks = 0;
	uint64 WarmReverbDryBlocks = 0;
	uint64 WarmReverbProcessedBlocks = 0;
	uint64 WarmReverbWetBlocks = 0;
	uint64 WarmBusReadBlocks = 0;
	bool bMapDirtyBefore = false;
	bool bPreTriggerContextAbsent = false;
	bool bGraphCaptureWindowInitialized = false;
	TArray<FIMQ5SavedAsset> TestAssets;
	TArray<FIMQ5GeneratedPackage> GeneratedPackages;
	TArray<double> DeviceT20;
	TArray<double> GraphWetT20;
	TArray<FString> CaseReports;
	TSharedPtr<FJsonObject> CurrentReport;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<AActor> SourceActor;
	TWeakObjectPtr<UAudioComponent> Audio;
	TWeakObjectPtr<UIMAcousticSourceComponent> SourceMarker;
	FIMAcousticMetaSoundContextPtr MetaContext;
	TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
};

bool IMQ5CaptureOriginalState(FIMQ5RunState& S, FString& Error)
{
	if (!GEditor || !GEngine || IMQ5FindPIEWorld())
	{
		Error = TEXT("Q5 room graph test requires an idle Editor world with no active PIE.");
		return false;
	}
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	UPackage* Package = EditorWorld ? EditorWorld->GetPackage() : nullptr;
	if (!EditorWorld || !Package || Package->IsDirty())
	{
		Error = TEXT("Original editor map is unavailable or dirty; refusing to replace unsaved map state.");
		return false;
	}
	S.OriginalMapPath = Package->GetName();
	S.OriginalMapDiskPath = FPackageName::LongPackageNameToFilename(S.OriginalMapPath, FPackageName::GetMapPackageExtension());
	S.bMapDirtyBefore = Package->IsDirty();
	if (!IMQ5HashFile(S.OriginalMapDiskPath, S.OriginalMapSha1Before))
	{
		Error = TEXT("Could not fingerprint the original editor map package.");
		return false;
	}

	struct FIMExpectedAsset { const TCHAR* ObjectPath; const TCHAR* Sha1; };
	const FIMExpectedAsset Expected[] = {
		{IMQ5SourcePath, IMQ5RoomGraphExpectedSourceSha1},
		{IMQ5WavePath, IMQ5RoomGraphExpectedWaveSha1},
		{IMQ5EnvironmentPath, IMQ5RoomGraphExpectedEnvironmentSha1}
	};
	for (const FIMExpectedAsset& Entry : Expected)
	{
		UObject* Object = LoadObject<UObject>(nullptr, Entry.ObjectPath);
		if (!Object || !Object->GetOutermost())
		{
			Error = FString::Printf(TEXT("Required pinned Q5 asset did not load: %s"), Entry.ObjectPath);
			return false;
		}
		FIMQ5SavedAsset Asset;
		Asset.ObjectPath = Object->GetPathName();
		Asset.Package = Object->GetOutermost();
		Asset.DiskPath = FPackageName::LongPackageNameToFilename(Asset.Package->GetName(), FPackageName::GetAssetPackageExtension());
		Asset.ExpectedSha1 = Entry.Sha1;
		Asset.Object = Object;
		Asset.bDirtyBefore = Asset.Package->IsDirty();
		if (Asset.bDirtyBefore || !IMQ5HashFile(Asset.DiskPath, Asset.Sha1Before) || Asset.Sha1Before != Asset.ExpectedSha1)
		{
			Error = FString::Printf(TEXT("Pinned Q5 asset identity/hash is dirty or differs from its reviewed baseline: %s"), Entry.ObjectPath);
			return false;
		}
		S.TestAssets.Add(MoveTemp(Asset));
	}

	auto* Q5Source = Cast<UMetaSoundSource>(S.TestAssets[0].Object);
	auto* ImpulseWave = Cast<USoundWave>(S.TestAssets[1].Object);
	auto* Environment = Cast<UMetaSoundSource>(S.TestAssets[2].Object);
	bool bEnvironmentGraph = false;
	if (Environment && Environment->OutputFormat == EMetaSoundOutputAudioFormat::Stereo && Environment->NumChannels == 2)
	{
		for (const auto& Dependency : Environment->GetConstDocument().Dependencies)
		{
			const auto& Name = Dependency.Metadata.GetClassName();
			if (Name.Namespace == TEXT("IM") && Name.Name == TEXT("Acoustic Environment") && Name.Variant == TEXT("Stereo"))
			{
				bEnvironmentGraph = true;
				break;
			}
		}
	}
	if (!Q5Source || !ImpulseWave || !Environment
		|| !IMAcousticMetaSound::IsAcousticMetaSound(Q5Source)
		|| !bEnvironmentGraph
		|| ImpulseWave->NumChannels != 1
		|| !FMath::IsNearlyEqual(double(ImpulseWave->Duration), 0.010, 0.001))
	{
		Error = FString::Printf(TEXT("Pinned Q5 shape mismatch: source=%d acoustic_source=%d wave=%d channels=%d duration_s=%.6f environment=%d acoustic_environment=%d."),
			Q5Source ? 1 : 0, Q5Source && IMAcousticMetaSound::IsAcousticMetaSound(Q5Source) ? 1 : 0,
			ImpulseWave ? 1 : 0, ImpulseWave ? ImpulseWave->NumChannels : -1,
			ImpulseWave ? double(ImpulseWave->Duration) : -1.0,
			Environment ? 1 : 0, bEnvironmentGraph ? 1 : 0);
		return false;
	}
	return true;
}

bool IMQ5SaveLevel(UWorld* World, const FString& MapPath, FString& OutDiskPath)
{
	if (!World || !World->PersistentLevel) return false;
	OutDiskPath = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutDiskPath), true);
	return FEditorFileUtils::SaveLevel(World->PersistentLevel, OutDiskPath);
}

class FIMQ5RoomGraphCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMQ5RoomGraphCommand(TSharedRef<FIMQ5RunState> InState, int32 InStage = 0)
		: State(InState), Stage(InStage), StageStarted(FPlatformTime::Seconds())
	{
	}

	bool Update() override
	{
		check(IsInGameThread());
		const double Now = FPlatformTime::Seconds();
		if (Stage == 90) return CleanupAndFinish(Now);
		if (Stage == 91) return WaitForPIEExit(Now);
		const double Timeout = Stage == 1 ? 600.0 : Stage == 7 ? 30.0 : 120.0;
		if (Now - StageStarted > Timeout)
		{
			return Fail(FString::Printf(TEXT("Q5 room stage %d timed out."), Stage));
		}

		if (Stage == 0) return CreateAndStartBake();
		if (Stage == 1) return PollBakeAndEnterPIE();

		UWorld* PIE = IMQ5FindPIEWorld();
		if (Stage == 7) return FinishCase(Now, PIE);
		if (!PIE) return Stage == 2 ? false : Fail(TEXT("PIE ended before Q5 wet recording completed."));
		if (Stage == 2) return ResolvePIEAndStartDeviceRecording(PIE, Now);
		if (Stage == 3) return WaitForImpulseTime(PIE, Now);
		if (Stage == 4) return WaitForGraphReadiness(PIE, Now);
		if (Stage == 5) return CaptureRoomWet(PIE, Now);
		if (Stage == 6) return WaitForWavAndAnalyze(PIE, Now);
		return Fail(FString::Printf(TEXT("Unknown Q5 room test stage %d."), Stage));
	}

private:
	FString CaseDirectory() const
	{
		return FPaths::Combine(State->EvidenceDirectory, IMQ5Cases[State->CaseIndex].Name);
	}

	FString CaseFile(const TCHAR* Name) const { return FPaths::Combine(CaseDirectory(), Name); }

	bool SaveCurrentReport()
	{
		return State->CurrentReport.IsValid() && IMQ5WriteJson(CaseFile(TEXT("case.json")), State->CurrentReport.ToSharedRef());
	}

	void SetStage(int32 Next)
	{
		Stage = Next;
		StageStarted = FPlatformTime::Seconds();
	}

	bool Fail(const FString& Message)
	{
		if (State->Failure.IsEmpty()) State->Failure = Message;
		if (State->CurrentReport.IsValid())
		{
			if (State->bOrderOnlyClosedLow) IMQ5SetOrderOnlyScope(*State->CurrentReport, false);
			else State->CurrentReport->SetBoolField(TEXT("passed"), false);
			State->CurrentReport->SetStringField(TEXT("failure"), State->Failure);
			State->CurrentReport->SetNumberField(TEXT("failed_stage"), Stage);
			SaveCurrentReport();
		}
		BeginFinish(false, Message);
		return false;
	}

	void BeginFinish(bool bPass, const FString& Message)
	{
		State->bFinalPass = bPass;
		if (State->bOrderOnlyClosedLow) State->bFinalPass = false;
		State->FinalMessage = Message;
		if (State->bRecording)
		{
			if (UWorld* PIE = IMQ5FindPIEWorld())
			{
				UAudioMixerBlueprintLibrary::StopRecordingOutput(
					PIE, EAudioRecordingExportType::WavFile, TEXT("wet-incomplete"), State->EvidenceDirectory);
			}
			State->bRecording = false;
		}
		State->ReleaseMasterRecorderListener();
		State->ReleaseCapturePreparation();
		if (State->Audio.IsValid()) State->Audio->Stop();
		if (State->Volume.IsValid()) State->Volume->CancelBake();
		if (GUnrealEd && IMQ5FindPIEWorld())
		{
			// A diagnostic failure also honors the existing recorder fence and 30s PIE exit bound.
			if (!State->bOrderOnlyClosedLow || State->RecorderFence.IsFenceComplete())
				GUnrealEd->RequestEndPlayMap();
			SetStage(91);
		}
		else
		{
			SetStage(90);
		}
	}

	bool CreateAndStartBake()
	{
		if (State->bOrderOnlyClosedLow && (State->CaseIndex != 0 || !State->bCaptureTransportOrder))
			return Fail(TEXT("Order-only mode requires the original first closed-low case and transport-order flag."));
		const FIMQ5RoomCase& C = IMQ5Cases[State->CaseIndex];
		IFileManager::Get().MakeDirectory(*CaseDirectory(), true);
		State->CurrentReport = MakeShared<FJsonObject>();
		State->CurrentReport->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5RoomGraphCase.v1"));
		State->CurrentReport->SetStringField(TEXT("case"), C.Name);
		State->CurrentReport->SetStringField(TEXT("geometry"), C.Geometry);
		State->CurrentReport->SetNumberField(TEXT("absorption_each_band"), C.Absorption);
		State->CurrentReport->SetNumberField(TEXT("scattering_each_band"), 0.5);
		State->CurrentReport->SetNumberField(TEXT("capture_rate_hz"), IMQ5Rate);
		State->CurrentReport->SetNumberField(TEXT("capture_seconds"), IMQ5CaptureSeconds);
		State->CurrentReport->SetNumberField(TEXT("expected_trigger_delay_s"), IMQ5ExpectedTriggerSeconds);
		State->CurrentReport->SetNumberField(TEXT("trigger_tolerance_s"), IMQ5TriggerToleranceSeconds);
		State->CurrentReport->SetNumberField(TEXT("impulse_wave_channels"), 1);
		State->CurrentReport->SetNumberField(TEXT("impulse_wave_duration_s"), 0.010);
		State->CurrentReport->SetStringField(TEXT("source_metasound"), IMQ5SourcePath);
		State->CurrentReport->SetStringField(TEXT("source_asset_sha1"), State->TestAssets[0].Sha1Before);
		State->CurrentReport->SetStringField(TEXT("impulse_wave"), IMQ5WavePath);
		State->CurrentReport->SetStringField(TEXT("impulse_asset_sha1"), State->TestAssets[1].Sha1Before);
		State->CurrentReport->SetStringField(TEXT("environment_metasound"), IMQ5EnvironmentPath);
		State->CurrentReport->SetStringField(TEXT("environment_asset_sha1"), State->TestAssets[2].Sha1Before);
		State->CurrentReport->SetBoolField(TEXT("direct_route_enabled"), false);
		State->CurrentReport->SetBoolField(TEXT("path_route_enabled"), false);
		State->CurrentReport->SetBoolField(TEXT("reverb_route_enabled"), true);
		if (State->bOrderOnlyClosedLow) IMQ5SetOrderOnlyScope(*State->CurrentReport, false);

		if (IMQ5FindPIEWorld()) return Fail(TEXT("Cannot start a room fixture while another PIE world is active."));
		State->CurrentMapPath = State->MountRoot + TEXT("/IM_") + FString(C.Name).Replace(TEXT("-"), TEXT("_"));
		State->CurrentMapDiskPath = FPackageName::LongPackageNameToFilename(State->CurrentMapPath, FPackageName::GetMapPackageExtension());
		if (FPackageName::DoesPackageExist(State->CurrentMapPath)
			|| IFileManager::Get().FileExists(*State->CurrentMapDiskPath))
		{
			return Fail(TEXT("Refusing to overwrite an existing Q5 fixture map package."));
		}

		UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!World || !Cube || !Cube->GetMaterial(0)) return Fail(TEXT("Cannot create the room fixture world, engine Cube, or Cube material."));

		FString GeometryIdentity;
		int32 BoxCount = 0;
		auto AddBox = [&](const FVector& Lo, const FVector& Hi) -> bool
		{
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
			if (!Actor) return false;
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			Mesh->SetMobility(EComponentMobility::Static);
			Mesh->SetStaticMesh(Cube);
			const FVector Center = (Lo + Hi) * 0.5;
			const FVector Size = Hi - Lo;
			// Test fixture conversion follows the production SDK coordinate contract:
			// SDK metres (x,y,z; y up) -> UE centimetres (-z,x,y).
			Actor->SetActorLocation(FVector(-Center.Z, Center.X, Center.Y) * 100.0);
			Actor->SetActorScale3D(FVector(Size.Z, Size.X, Size.Y));
			Actor->SetActorLabel(FString::Printf(TEXT("IMQ5RoomBox_%02d"), BoxCount++));
			Mesh->ComponentTags.Add(TEXT("IMAcousticRequired"));
			GeometryIdentity += Cube->GetPathName() + TEXT("|") + Mesh->GetComponentTransform().ToString() + TEXT("\n");
			return true;
		};
		const double X = C.HalfX;
		const double Z = C.HalfZ;
		if (!AddBox({-X - 0.1, -0.1, -Z - 0.1}, {X + 0.1, 0.0, Z + 0.1})
			|| (C.bCeiling && !AddBox({-X - 0.1, 3.0, -Z - 0.1}, {X + 0.1, 3.1, Z + 0.1}))
			|| !AddBox({-X - 0.1, 0.0, -Z - 0.1}, {-X, 3.0, Z + 0.1})
			|| !AddBox({X, 0.0, -Z - 0.1}, {X + 0.1, 3.0, Z + 0.1})
			|| !AddBox({-X, 0.0, -Z - 0.1}, {X, 3.0, -Z})
			|| !AddBox({-X, 0.0, Z}, {X, 3.0, Z + 0.1}))
		{
			return Fail(TEXT("Could not create the complete sealed room/corridor/outdoor fixture."));
		}
		if (State->CaseIndex == 0) State->ClosedGeometryIdentity = GeometryIdentity;
		if (State->CaseIndex == 1 && State->ClosedGeometryIdentity != GeometryIdentity)
		{
			return Fail(TEXT("closed-low and closed-high do not have identical actual static-mesh transforms."));
		}
		State->CurrentReport->SetNumberField(TEXT("static_mesh_wall_actors"), BoxCount);
		State->CurrentReport->SetStringField(TEXT("closed_geometry_identity_sha1"),
			FSHA1::HashBuffer(reinterpret_cast<const uint8*>(GeometryIdentity.GetCharArray().GetData()),
				uint64(GeometryIdentity.Len()) * sizeof(TCHAR)).ToString().ToLower());

		State->Volume = World->SpawnActor<AIMAcousticBakeVolume>();
		if (!State->Volume.IsValid()) return Fail(TEXT("Could not spawn the production acoustic BakeVolume."));
		State->Volume->MaterialDrive = EIMAcousticMaterialDrive::LegacyRenderMaterial;
		State->Volume->SetActorLocation(FVector(0.0, 0.0, 150.0));
		State->Volume->BakeBounds->SetBoxExtent(FVector(Z * 100.0, X * 100.0, 150.0));
		State->Volume->ProbeSpacingCm = 100.0f;
		State->Volume->ProbeHeightCm = 150.0f;
		FIMAcousticMaterialMapping Material;
		Material.Material = Cube->GetMaterial(0);
		Material.Absorption = FVector(C.Absorption);
		Material.Scattering = 0.5f;
		State->Volume->Materials.Add(Material);
		State->Volume->bEnableV2 = true;
		State->Volume->bDirectRoute = false;
		State->Volume->bPathRoute = false;
		State->Volume->bReverbRoute = true;
		State->Volume->ReverbWetGain = 1.0f;

		FIMQ5GeneratedPackage GeneratedMap;
		GeneratedMap.PackageName = State->CurrentMapPath;
		GeneratedMap.DiskPath = State->CurrentMapDiskPath;
		GeneratedMap.bIsMap = true;
		State->GeneratedPackages.Add(GeneratedMap);
		if (!IMQ5SaveLevel(World, State->CurrentMapPath, State->CurrentMapDiskPath)
			|| !IMQ5HashFile(State->CurrentMapDiskPath, State->GeneratedPackages.Last().Sha1BeforeCleanup))
		{
			return Fail(TEXT("Could not save/fingerprint the unique Q5 room map fixture."));
		}
		State->CurrentReport->SetStringField(TEXT("map_package"), State->CurrentMapPath);
		State->CurrentReport->SetStringField(TEXT("map_sha1_after_fixture_save"), State->GeneratedPackages.Last().Sha1BeforeCleanup);
		State->CurrentReport->SetNumberField(TEXT("recipe_version"), IMAcousticRecipe::Version);
		State->CurrentReport->SetNumberField(TEXT("saved_ir_seconds"), IMAcousticRecipe::ReverbSavedDurationS);
		State->CurrentReport->SetNumberField(TEXT("probe_spacing_cm"), 100.0);
		State->CurrentReport->SetNumberField(TEXT("probe_height_cm"), 150.0);
		State->CurrentReport->SetNumberField(TEXT("listener_sdk_x_m"), C.ListenerX);
		State->CurrentReport->SetNumberField(TEXT("listener_sdk_y_m"), 1.5);
		State->CurrentReport->SetNumberField(TEXT("source_sdk_x_m"), C.ListenerX - 1.0);
		State->CurrentReport->SetNumberField(TEXT("source_listener_distance_cm"), 100.0);
		if (!FFileHelper::SaveStringToFile(GeometryIdentity, *CaseFile(TEXT("geometry-identity.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !SaveCurrentReport())
		{
			return Fail(TEXT("Could not save Q5 fixture geometry identity or initial report."));
		}

		State->Volume->GenerateProbes();
		if (State->Volume->GeneratedProbes <= 0 || State->Volume->ExportedTriangles <= 0)
		{
			return Fail(TEXT("Production probe generation failed: ") + State->Volume->Status);
		}
		State->Volume->Bake();
		SetStage(1);
		return false;
	}

	bool PollBakeAndEnterPIE()
	{
		if (!State->Volume.IsValid()) return Fail(TEXT("BakeVolume owner disappeared during the production room bake."));
		if (!State->Volume->BakedField)
		{
			if (!State->Volume->Status.StartsWith(TEXT("Baking."))) return Fail(State->Volume->Status);
			return false;
		}
		FString BakeError;
		if (!State->Volume->ValidateCurrentBake(BakeError)) return Fail(BakeError);

		UIMAcousticBakeAsset* Asset = State->Volume->BakedField.Get();
		State->CurrentBakePackage = Asset->GetOutermost()->GetName();
		State->CurrentBakeDiskPath = FPackageName::LongPackageNameToFilename(State->CurrentBakePackage, FPackageName::GetAssetPackageExtension());
		if (State->CurrentBakePackage.IsEmpty() || State->GeneratedPackages.IsEmpty()
			|| State->GeneratedPackages.Last().PackageName != State->CurrentMapPath)
		{
			return Fail(TEXT("Unexpected generated bake package or fixture-map identity."));
		}
		const int32 MapPackageIndex = State->GeneratedPackages.Num() - 1;
		if (!IMQ5SaveLevel(State->Volume->GetWorld(), State->CurrentMapPath, State->CurrentMapDiskPath))
		{
			return Fail(TEXT("Could not persist the generated bake reference into its room map."));
		}
		FString MapHashAfterBake;
		if (!IMQ5HashFile(State->CurrentMapDiskPath, MapHashAfterBake))
		{
			return Fail(TEXT("Could not fingerprint the Q5 room map after writing its bake reference."));
		}
		State->GeneratedPackages[MapPackageIndex].Sha1BeforeCleanup = MapHashAfterBake;
		State->CurrentReport->SetStringField(TEXT("map_sha1_with_bake_binding"), MapHashAfterBake);

		FIMQ5GeneratedPackage GeneratedBake;
		GeneratedBake.PackageName = State->CurrentBakePackage;
		GeneratedBake.DiskPath = State->CurrentBakeDiskPath;
		if (!IMQ5HashFile(GeneratedBake.DiskPath, GeneratedBake.Sha1BeforeCleanup))
		{
			return Fail(TEXT("Could not fingerprint the production bake asset package."));
		}
		State->GeneratedPackages.Add(GeneratedBake);
		State->CurrentBakeSha1 = State->GeneratedPackages.Last().Sha1BeforeCleanup;

		State->CurrentReport->SetNumberField(TEXT("exported_triangles"), State->Volume->ExportedTriangles);
		State->CurrentReport->SetNumberField(TEXT("generated_probes"), State->Volume->GeneratedProbes);
		State->CurrentReport->SetStringField(TEXT("bake_asset_path"), Asset->GetPathName());
		State->CurrentReport->SetStringField(TEXT("bake_asset_sha1_before_pie"), State->CurrentBakeSha1);
		State->CurrentReport->SetStringField(TEXT("scene_fingerprint"), Asset->SceneFingerprint);
		State->CurrentReport->SetNumberField(TEXT("sdk_version"), Asset->SDKVersion);
		if (!SaveCurrentReport()) return Fail(TEXT("Could not update the bake-binding report."));

		FString LoadError;
		GUnrealEd->AutomationLoadMap(State->CurrentMapPath, false, &LoadError);
		if (!LoadError.IsEmpty()) return Fail(LoadError);
		// AutomationLoadMap enqueues PIE startup; continue from a later latent command.
		FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FIMQ5RoomGraphCommand>(State, 2));
		return true;
	}

	bool ResolvePIEAndStartDeviceRecording(UWorld* PIE, double)
	{
		if (!PIE) return false;
		if (IMQ5PIEMapPath(PIE) != State->CurrentMapPath) return Fail(TEXT("PIE opened a map other than this Q5 room case."));
		AIMAcousticBakeVolume* PIEVolume = nullptr;
		for (TActorIterator<AIMAcousticBakeVolume> It(PIE); It; ++It)
		{
			if (PIEVolume) return Fail(TEXT("Q5 room PIE has multiple BakeVolume owners."));
			PIEVolume = *It;
		}
		State->Volume = PIEVolume;
		State->Listener = PIE->GetFirstPlayerController();
		FAudioDevice* Device = PIE->GetAudioDeviceRaw();
		if (!State->Volume.IsValid() || !State->Listener.IsValid() || !Device)
		{
			return Fail(TEXT("Q5 room PIE is missing its BakeVolume, player listener, or audio device."));
		}
		if (Device->GetSampleRate() != IMQ5Rate) return Fail(TEXT("Q5 wet gate requires a 48 kHz PIE audio device."));
		if (State->Volume->bDirectRoute || State->Volume->bPathRoute || !State->Volume->bReverbRoute)
		{
			return Fail(TEXT("Q5 BakeVolume route settings are not direct/path-off and reverb-on before recording."));
		}
		const FIMAcousticMetaSoundContextPtr PreexistingContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID);
		State->bPreTriggerContextAbsent = !PreexistingContext.IsValid();
		if (!State->bPreTriggerContextAbsent)
		{
			return Fail(TEXT("A MetaSound environment context already existed before the Q5 source trigger; stale context would invalidate the causal fixture."));
		}
		State->AudioDeviceId = Device->DeviceID;
		const FIMQ5RoomCase& C = IMQ5Cases[State->CaseIndex];
		State->Listener->SetAudioListenerOverride(nullptr, FVector(0.0, C.ListenerX * 100.0, 150.0), FRotator(0.0, -90.0, 0.0));

		// Recording alone does not keep an idle master submix rendering. Arm a
		// read-only test listener first, then require actual silent callbacks.
		if (!State->MasterRecorder.IsValid())
		{
			State->MasterRecorder = MakeShared<FIMQ5MasterRecorderListener, ESPMode::ThreadSafe>(&Device->GetMainSubmixObject());
			Device->RegisterSubmixBufferListener(State->MasterRecorder.ToSharedRef(), Device->GetMainSubmixObject());
			State->bMasterRecorderRegistered = true;
			State->RecorderFence.BeginFence();
			return false;
		}
		if (!State->MasterRecorder->IsValidLedger()) return Fail(TEXT("Master recorder callback identity/rate/capacity is invalid."));
		if (!State->RecorderFence.IsFenceComplete() || State->MasterRecorder->GetBlockCount() < 2) return false;
		// Allocate only on a prerecord tick. Returning once prevents this GT
		// allocation from carrying the caller's old Now into recording/trigger.
		const bool bFirstPreparation = State->CapturePreparationId == 0;
		FString PreparationError;
		const double PrepareStartedAt = bFirstPreparation ? FPlatformTime::Seconds() : 0.0;
		if (!IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(Device->DeviceID,
			uint32(Device->GetSampleRate()), State->CapturePreparationId, PreparationError))
		{
			return Fail(TEXT("Q5 prerecord capture preparation failed: ") + PreparationError);
		}
		if (bFirstPreparation)
		{
			State->CapturePreparationStartedSeconds = PrepareStartedAt;
			State->CapturePreparationEndedSeconds = FPlatformTime::Seconds();
			if (IMAcousticMetaSound::FindAcousticMetaSoundContext(State->AudioDeviceId).IsValid())
				return Fail(TEXT("Capture storage preparation created a pre-trigger context."));
			State->CurrentReport->SetStringField(TEXT("capture_preparation_id"),
				FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(State->CapturePreparationId)));
			State->CurrentReport->SetNumberField(TEXT("capture_preparation_device_id"), Device->DeviceID);
			State->CurrentReport->SetNumberField(TEXT("capture_preparation_sample_rate"), Device->GetSampleRate());
			State->CurrentReport->SetNumberField(TEXT("capture_preparation_started_wall_seconds"), State->CapturePreparationStartedSeconds);
			State->CurrentReport->SetNumberField(TEXT("capture_preparation_ended_wall_seconds"), State->CapturePreparationEndedSeconds);
			State->CurrentReport->SetBoolField(TEXT("capture_preparation_context_absent"), true);
			if (!SaveCurrentReport()) return Fail(TEXT("Could not persist prerecord capture preparation evidence."));
			return false;
		}
		State->MasterStartRequestFrame = State->MasterRecorder->GetFrameCount();
		State->MasterRecorder->SetRequestPhase(1);
		UAudioMixerBlueprintLibrary::StartRecordingOutput(PIE, float(IMQ5CaptureSeconds + 1.0), nullptr);
		State->RecorderFence.BeginFence();
		State->bRecording = true;
		State->CaptureStartedSeconds = FPlatformTime::Seconds();
		if (State->CapturePreparationEndedSeconds <= 0.0
			|| State->CapturePreparationEndedSeconds > State->CaptureStartedSeconds)
			return Fail(TEXT("Q5 capture preparation did not complete before the original recording request."));
		State->CurrentReport->SetBoolField(TEXT("capture_preparation_precedes_recording"), true);
		State->CurrentReport->SetNumberField(TEXT("device_id"), State->AudioDeviceId);
		State->CurrentReport->SetBoolField(TEXT("master_readonly_keepalive_armed"), true);
		State->CurrentReport->SetNumberField(TEXT("master_prerecord_render_blocks"), State->MasterRecorder->GetBlockCount());
		State->CurrentReport->SetNumberField(TEXT("master_start_request_render_frame"), State->MasterStartRequestFrame);
		State->CurrentReport->SetBoolField(TEXT("graph_context_present_before_source_trigger"), false);
		State->CurrentReport->SetBoolField(TEXT("pretrigger_device_recording_started"), true);
		State->CurrentReport->SetStringField(TEXT("graph_pcm_origin_contract"), TEXT("Graph PCM begins at frame zero of the context created after the ordinary Q5 source registration; the 48 kHz device WAV begins before that trigger and retains pre-trigger silence."));
		State->CurrentReport->SetNumberField(TEXT("capture_started_wall_seconds"), State->CaptureStartedSeconds);
		if (!SaveCurrentReport()) return Fail(TEXT("Could not persist pre-trigger recording and absent-context evidence."));
		SetStage(3);
		return false;
	}

	bool WaitForGraphReadiness(UWorld* PIE, double Now)
	{
		if (!PIE || !State->Volume.IsValid() || !State->bRecording || !State->bSourceSpawned)
		{
			return Fail(TEXT("PIE BakeVolume/recording or the one-shot Q5 source ended before graph preparation."));
		}
		FAudioDevice* Device = PIE->GetAudioDeviceRaw();
		if (!Device || Device->DeviceID != State->AudioDeviceId || Device->GetSampleRate() != IMQ5Rate)
		{
			return Fail(TEXT("PIE audio device identity or 48 kHz rate changed after the source trigger."));
		}
		if (!State->MetaContext.IsValid())
		{
			State->MetaContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(State->AudioDeviceId);
			if (!State->MetaContext.IsValid())
			{
				if (Now - State->TriggerRequestedSeconds > 20.0)
				{
					return Fail(TEXT("The production MetaSound environment context did not initialize within 20 seconds after the ordinary Q5 source registration."));
				}
				return false;
			}
			State->Bridge = State->MetaContext->Device;
			if (!State->Bridge.IsValid()) return Fail(TEXT("The post-trigger MetaSound environment context has no audio-device bridge."));
			if (State->MetaContext->DeviceId != State->AudioDeviceId || !State->MetaContext->Pool
				|| State->MetaContext->CapturedSource.IsEmpty() || State->MetaContext->CapturedWet.IsEmpty()
				|| State->MetaContext->CapturedSourceBlocks.IsEmpty() || State->MetaContext->CapturedBlocks.IsEmpty())
			{
				return Fail(TEXT("The post-trigger context has the wrong device identity or was created without the required source/wet PCM and graph trace capture buffers."));
			}
			const int32 PcmCapacity = int32(Device->GetSampleRate()) * 200;
			const int32 EnvironmentRows = PcmCapacity / FIMAcousticMetaSoundContext::Frames;
			const int32 SourceRows = int32(FIMAcousticMetaSoundContext::CapturedSourceBlockTraceDefaultCapacity);
			if (State->CapturePreparationId == 0
				|| State->MetaContext->TestCapturePreparationId != State->CapturePreparationId
				|| State->Bridge->SampleRate != uint32(Device->GetSampleRate())
				|| State->MetaContext->CapturedSource.Num() != PcmCapacity
				|| State->MetaContext->CapturedBus.Num() != PcmCapacity
				|| State->MetaContext->CapturedDry.Num() != PcmCapacity * 2
				|| State->MetaContext->CapturedWet.Num() != PcmCapacity * 2
				|| State->MetaContext->CapturedSourceBlocks.Num() != SourceRows
				|| State->MetaContext->CapturedBlocks.Num() != EnvironmentRows
				|| State->MetaContext->CapturedBusReads.Num() != EnvironmentRows
				|| State->MetaContext->CapturedSourceBlockReadyWordCount != uint32((SourceRows + 63) / 64)
				|| !State->MetaContext->CapturedSourceBlockReady)
			{
				return Fail(TEXT("The fresh Q5 context did not consume the exact prepared ID/rate/full-capacity storage."));
			}
			State->CurrentReport->SetStringField(TEXT("context_capture_preparation_id"),
				FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(State->MetaContext->TestCapturePreparationId)));
			State->CurrentReport->SetNumberField(TEXT("context_capture_pcm_capacity_frames"), PcmCapacity);
			State->CurrentReport->SetNumberField(TEXT("context_capture_source_trace_capacity"), SourceRows);
			State->CurrentReport->SetNumberField(TEXT("context_capture_environment_trace_capacity"), EnvironmentRows);
			State->CurrentReport->SetNumberField(TEXT("context_capture_bus_trace_capacity"), State->MetaContext->CapturedBusReads.Num());
			State->CurrentReport->SetNumberField(TEXT("context_capture_source_ready_words"), State->MetaContext->CapturedSourceBlockReadyWordCount);
			State->CurrentReport->SetBoolField(TEXT("context_consumed_prepared_capture"), true);
			const uint32 OrderCapacity = State->bCaptureTransportOrder ? FIMAcousticMetaSoundContext::TransportOrderEventCapacity : 0;
			if (uint32(State->MetaContext->CapturedTransportOrder.Num()) != OrderCapacity
				|| State->MetaContext->CapturedTransportOrderReadyWordCount != OrderCapacity / 64u
				|| bool(State->MetaContext->CapturedTransportOrderReady) != State->bCaptureTransportOrder)
				return Fail(TEXT("The optional joint order trace did not consume its prepared storage."));
			State->CurrentReport->SetBoolField(TEXT("transport_order_opt_in"), State->bCaptureTransportOrder);
			State->GraphContextObservedSeconds = Now;
			State->EnvironmentFramesAtFirstObservation = State->MetaContext->CapturedEnvironmentFrames.load(std::memory_order_acquire);
			State->SourceFramesAtFirstObservation = State->MetaContext->CapturedSourceFrames.load(std::memory_order_acquire);
			State->EnvironmentTraceRowsAtFirstObservation = State->MetaContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
			State->SourceTraceRowsAtFirstObservation = State->MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire);

			// The context is born only after the source registration. Preserve its
			// complete post-trigger PCM and trace prefix; resetting to the first
			// observation would discard the one-shot that caused the context.
			State->EnvironmentPcmBegin = 0;
			State->EnvironmentTraceBegin = 0;
			State->SourcePcmBegin = 0;
			State->SourceTraceBegin = 0;
			State->WarmEnvironmentBlocks = State->MetaContext->EnvironmentBlocks.load(std::memory_order_acquire);
			State->WarmReverbDryBlocks = State->Bridge->ReverbDryBlocks.load(std::memory_order_acquire);
			State->WarmReverbProcessedBlocks = State->Bridge->ReverbProcessedBlocks.load(std::memory_order_acquire);
			State->WarmReverbWetBlocks = State->Bridge->ReverbNonzeroBlocks.load(std::memory_order_acquire);
			State->WarmBusReadBlocks = State->MetaContext->BusReadBlocks.load(std::memory_order_acquire);
			// This context and bridge did not exist at recorder start; their
			// counters are zero-origin and include any blocks consumed before
			// the latent test command observes the new context.
			State->CountersBegin = {};
			State->BusUnderrunsBegin = 0;
			State->bGraphCaptureWindowInitialized = true;
			State->CurrentReport->SetNumberField(TEXT("graph_context_observed_wall_seconds"), State->GraphContextObservedSeconds);
			State->CurrentReport->SetNumberField(TEXT("graph_context_observed_delay_after_trigger_s"), State->GraphContextObservedSeconds - State->TriggerRequestedSeconds);
			State->CurrentReport->SetNumberField(TEXT("graph_source_frames_at_first_observation"), State->SourceFramesAtFirstObservation);
			State->CurrentReport->SetNumberField(TEXT("graph_wet_frames_at_first_observation"), State->EnvironmentFramesAtFirstObservation);
			State->CurrentReport->SetNumberField(TEXT("graph_source_trace_rows_at_first_observation"), State->SourceTraceRowsAtFirstObservation);
			State->CurrentReport->SetNumberField(TEXT("graph_environment_trace_rows_at_first_observation"), State->EnvironmentTraceRowsAtFirstObservation);
			State->CurrentReport->SetNumberField(TEXT("graph_environment_blocks_at_first_observation"), State->WarmEnvironmentBlocks);
			State->CurrentReport->SetNumberField(TEXT("graph_reverb_dry_blocks_at_first_observation"), State->WarmReverbDryBlocks);
			State->CurrentReport->SetNumberField(TEXT("graph_reverb_processed_blocks_at_first_observation"), State->WarmReverbProcessedBlocks);
			State->CurrentReport->SetNumberField(TEXT("graph_wet_nonzero_blocks_at_first_observation"), State->WarmReverbWetBlocks);
			State->CurrentReport->SetNumberField(TEXT("graph_bus_read_blocks_at_first_observation"), State->WarmBusReadBlocks);
			State->CurrentReport->SetNumberField(TEXT("graph_bus_underruns_at_first_observation"), State->MetaContext->BusUnderruns.load(std::memory_order_acquire));
			State->CurrentReport->SetNumberField(TEXT("graph_source_pcm_begin_frame"), State->SourcePcmBegin);
			State->CurrentReport->SetNumberField(TEXT("graph_wet_pcm_begin_frame"), State->EnvironmentPcmBegin);
			if (!SaveCurrentReport()) return Fail(TEXT("Could not persist post-trigger graph-context identity and capture origin."));
		}
		if (!State->Bridge.IsValid() || !State->bGraphCaptureWindowInitialized)
		{
			return Fail(TEXT("The production MetaSound context/bridge was not retained after first observation."));
		}

		const bool bReady = State->MetaContext->EnvironmentConsumer.load(std::memory_order_acquire)
			&& State->MetaContext->BusReaderConsumer.load(std::memory_order_acquire)
			&& State->MetaContext->SourceSendPatches[0].IsValid()
			&& State->MetaContext->CapturedWet.Num() >= IMQ5Rate * 2
			&& State->MetaContext->CapturedSource.Num() >= IMQ5Rate
			&& State->MetaContext->CapturedSourceBlocks.Num() > 0
			&& State->MetaContext->CapturedSourceFrames.load(std::memory_order_acquire) > 0
			&& State->MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire) > 0
			&& State->MetaContext->EnvironmentBlocks.load(std::memory_order_acquire) >= State->WarmEnvironmentBlocks + 4
			// A 10 ms impulse can contribute just one nonzero dry block; the
			// graph must keep rendering the later wet tail, not repeat the impulse.
			&& State->Bridge->ReverbDryBlocks.load(std::memory_order_acquire) >= 1
			&& State->Bridge->ReverbProcessedBlocks.load(std::memory_order_acquire) >= State->WarmReverbProcessedBlocks + 4
			&& State->MetaContext->BusReadBlocks.load(std::memory_order_acquire) >= State->WarmBusReadBlocks + 4
			&& State->MetaContext->Pool->WorldGeneration != 0
			&& State->Bridge->WorldGeneration.load(std::memory_order_acquire) == State->MetaContext->Pool->WorldGeneration
			&& State->MetaContext->CapturedEnvironmentFrames.load(std::memory_order_acquire) >= IMQ5Rate / 10;
		if (!bReady)
		{
			if (Now - State->TriggerRequestedSeconds > 30.0)
			{
				const auto& Graph = *State->MetaContext;
				const uint64 SourceFrames = Graph.CapturedSourceFrames.load(std::memory_order_acquire);
				const uint32 SourceBlocks = Graph.CapturedSourceBlockCount.load(std::memory_order_acquire);
				const uint64 WetFrames = Graph.CapturedEnvironmentFrames.load(std::memory_order_acquire);
				const uint64 EnvironmentBlocks = Graph.EnvironmentBlocks.load(std::memory_order_acquire);
				const uint64 ReverbDryBlocks = State->Bridge->ReverbDryBlocks.load(std::memory_order_acquire);
				const uint64 ReverbProcessedBlocks = State->Bridge->ReverbProcessedBlocks.load(std::memory_order_acquire);
				const uint64 BusReads = Graph.BusReadBlocks.load(std::memory_order_acquire);
				const bool bEnvironmentConsumer = Graph.EnvironmentConsumer.load(std::memory_order_acquire);
				const bool bBusReaderConsumer = Graph.BusReaderConsumer.load(std::memory_order_acquire);
				const bool bSourceSendPatch = Graph.SourceSendPatches[0].IsValid();
				const bool bSourcePlaying = State->Audio.IsValid() && State->Audio->IsPlaying();
				double SourcePcmEnergy = 0.0;
				for (uint32 Frame = 0; Frame < FMath::Min<uint32>(uint32(SourceFrames), uint32(Graph.CapturedSource.Num())); ++Frame)
				{
					const double Sample = Graph.CapturedSource[int32(Frame)];
					SourcePcmEnergy += Sample * Sample;
				}
				const FIMAcousticBlockProbe* FirstSourceBlock = SourceBlocks > 0 && !Graph.CapturedSourceBlocks.IsEmpty()
					? &Graph.CapturedSourceBlocks[0] : nullptr;
				double DelayedInputEnergy = 0.0;
				double ReverbSendEnergy = 0.0;
				uint32 NonzeroSendBlocks = 0;
				FString ReadinessTrace = TEXT("index,consumed_s,input_energy,send_energy,reject,priming,sequence\n");
				const uint32 SourceTraceRows = FMath::Min<uint32>(SourceBlocks, uint32(Graph.CapturedSourceBlocks.Num()));
				for (uint32 Index = 0; Index < SourceTraceRows; ++Index)
				{
					const FIMAcousticBlockProbe& Block = Graph.CapturedSourceBlocks[int32(Index)];
					DelayedInputEnergy += FMath::Max(0.0, Block.InputEnergy);
					ReverbSendEnergy += FMath::Max(0.0, Block.ReverbSendEnergy);
					if (Block.ReverbSendEnergy > 1e-12) ++NonzeroSendBlocks;
					ReadinessTrace += FString::Printf(TEXT("%u,%.9f,%.17g,%.17g,%u,%u,%llu\n"),
						Index, Block.ConsumedSeconds, Block.InputEnergy, Block.ReverbSendEnergy,
						uint32(Block.Reject), Block.InputDelayPriming, Block.ResultSequence);
				}
				FFileHelper::SaveStringToFile(ReadinessTrace, *CaseFile(TEXT("readiness-source-trace.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_source_frames"), double(SourceFrames));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_source_blocks"), SourceBlocks);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_source_pcm_energy"), SourcePcmEnergy);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_delayed_input_energy"), DelayedInputEnergy);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_reverb_send_energy"), ReverbSendEnergy);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_nonzero_send_blocks"), NonzeroSendBlocks);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_reverb_send_gain"), Graph.ReverbSendGains[0].load(std::memory_order_acquire));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_bus_id"), Graph.BusId);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_bus_underruns"), Graph.BusUnderruns.load(std::memory_order_acquire));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_last_ir_sequence"), Graph.LastIRSequence.load(std::memory_order_acquire));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_reverb_raw_nonzero_blocks"), State->Bridge->ReverbRawNonzeroBlocks.load(std::memory_order_acquire));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_first_source_delayed_input_energy"), FirstSourceBlock ? FirstSourceBlock->InputEnergy : -1.0);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_first_source_input_delay_priming"), FirstSourceBlock ? FirstSourceBlock->InputDelayPriming : -1);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_first_source_reject"), FirstSourceBlock ? uint32(FirstSourceBlock->Reject) : -1);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_source_input_delay_blocks"), FIMAcousticMetaSoundContext::SourceInputDelayBlocks);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_wet_frames"), double(WetFrames));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_environment_blocks"), double(EnvironmentBlocks));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_reverb_dry_blocks"), double(ReverbDryBlocks));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_reverb_processed_blocks"), double(ReverbProcessedBlocks));
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_bus_read_blocks"), double(BusReads));
				State->CurrentReport->SetBoolField(TEXT("readiness_timeout_environment_consumer"), bEnvironmentConsumer);
				State->CurrentReport->SetBoolField(TEXT("readiness_timeout_bus_reader_consumer"), bBusReaderConsumer);
				State->CurrentReport->SetBoolField(TEXT("readiness_timeout_source_send_patch"), bSourceSendPatch);
				State->CurrentReport->SetBoolField(TEXT("readiness_timeout_source_playing"), bSourcePlaying);
				State->CurrentReport->SetNumberField(TEXT("readiness_timeout_render_routes"), State->Bridge->RenderRoutes.load(std::memory_order_acquire));
				State->CurrentReport->SetStringField(TEXT("readiness_timeout_volume_status"), State->Volume->Status);
				return Fail(FString::Printf(TEXT("Post-trigger graph not ready: env=%d bus_reader=%d patch=%d source_playing=%d source_frames=%llu source_blocks=%u delayed_e=%.9g send_e=%.9g gain=%.6g bus_id=%u wet_frames=%llu env_blocks=%llu dry=%llu processed=%llu bus_reads=%llu status=%s"),
					bEnvironmentConsumer ? 1 : 0, bBusReaderConsumer ? 1 : 0, bSourceSendPatch ? 1 : 0,
					bSourcePlaying ? 1 : 0, SourceFrames, SourceBlocks, DelayedInputEnergy, ReverbSendEnergy,
					Graph.ReverbSendGains[0].load(std::memory_order_acquire), Graph.BusId, WetFrames, EnvironmentBlocks,
					ReverbDryBlocks, ReverbProcessedBlocks, BusReads, *State->Volume->Status));
			}
			return false;
		}
		if (State->Volume->bDirectRoute || State->Volume->bPathRoute || !State->Volume->bReverbRoute
			|| State->Bridge->RenderRoutes.load(std::memory_order_acquire) != 4u)
		{
			return Fail(TEXT("Room routes are not isolated to wet reverb after the Q5 source graph became ready."));
		}
		State->CurrentReport->SetNumberField(TEXT("world_generation_begin"), State->Bridge->WorldGeneration.load(std::memory_order_acquire));
		State->CurrentReport->SetNumberField(TEXT("graph_context_pool_world_generation"), State->MetaContext->Pool->WorldGeneration);
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_begin"), State->BusUnderrunsBegin);
		State->CurrentReport->SetNumberField(TEXT("graph_capture_ready_wall_seconds"), Now);
		State->CurrentReport->SetNumberField(TEXT("graph_capture_ready_delay_after_trigger_s"), Now - State->TriggerRequestedSeconds);
		if (!SaveCurrentReport()) return Fail(TEXT("Could not persist post-trigger graph readiness evidence."));
		SetStage(5);
		return false;
	}

	uint32 DeviceId() const { return State->AudioDeviceId; }

	bool SpawnDelayedGraphImpulse(UWorld* PIE, FString& Error)
	{
		if (State->bSourceSpawned)
		{
			Error = TEXT("The Q5 one-shot source trigger was requested more than once.");
			return false;
		}
		UMetaSoundSource* Sound = LoadObject<UMetaSoundSource>(nullptr, IMQ5SourcePath);
		if (!Sound || Sound->GetPathName() != IMQ5SourcePath || !IMAcousticMetaSound::IsAcousticMetaSound(Sound))
		{
			Error = TEXT("The delayed trigger did not load the pinned ordinary Q5 acoustic MetaSound graph.");
			return false;
		}
		AActor* Actor = PIE->SpawnActor<AActor>();
		if (!Actor)
		{
			Error = TEXT("Could not spawn the single Q5 impulse source actor.");
			return false;
		}
		UAudioComponent* Component = NewObject<UAudioComponent>(Actor, TEXT("IMQ5ImpulseAudio"));
		if (!Component)
		{
			Actor->Destroy();
			Error = TEXT("Could not create the Q5 impulse AudioComponent.");
			return false;
		}
		Actor->SetRootComponent(Component);
		Actor->AddInstanceComponent(Component);
		Component->bAutoActivate = false;
		Component->bAllowSpatialization = false;
		Component->SetOverrideAttenuation(false);
		Component->SetSound(Sound);
		Component->RegisterComponent();
		const FIMQ5RoomCase& C = IMQ5Cases[State->CaseIndex];
		Component->SetWorldLocation(FVector(0.0, (C.ListenerX - 1.0) * 100.0, 150.0));

		UIMAcousticSourceComponent* Marker = NewObject<UIMAcousticSourceComponent>(Actor, TEXT("IMQ5AcousticSource"));
		if (!Marker)
		{
			Actor->Destroy();
			Error = TEXT("Could not create the Q5 acoustic source marker.");
			return false;
		}
		Actor->AddInstanceComponent(Marker);
		Marker->AudioComponent = Component;
		Marker->RegisterComponent();
		if (!Marker->ValidateSource(Error))
		{
			Actor->Destroy();
			return false;
		}
		State->SourceActor = Actor;
		State->Audio = Component;
		State->SourceMarker = Marker;
		State->bSourceSpawned = true;
		State->TriggerRequestedSeconds = FPlatformTime::Seconds();
		State->CurrentReport->SetStringField(TEXT("trigger_source"), TEXT("single UIMAcousticSourceComponent registration; BakeVolume binds voice+bus and invokes the ordinary component Play/OnPlay path"));
		State->CurrentReport->SetNumberField(TEXT("trigger_requested_wall_seconds"), State->TriggerRequestedSeconds);
		State->CurrentReport->SetNumberField(TEXT("trigger_requested_delay_s"), State->TriggerRequestedSeconds - State->CaptureStartedSeconds);
		State->CurrentReport->SetStringField(TEXT("audio_component_id_before_graph_callback"), LexToString(Component->GetAudioComponentID()));
		return SaveCurrentReport();
	}

	bool WaitForImpulseTime(UWorld* PIE, double Now)
	{
		if (!PIE || !State->bRecording || !State->bPreTriggerContextAbsent)
		{
			return Fail(TEXT("PIE device recording or the pre-trigger context-absence invariant ended before the delayed impulse."));
		}
		if (!State->bSourceSpawned && IMAcousticMetaSound::FindAcousticMetaSoundContext(State->AudioDeviceId).IsValid())
		{
			return Fail(TEXT("A MetaSound context appeared before the scheduled Q5 source registration."));
		}
		if (!State->bSourceSpawned && Now - State->CaptureStartedSeconds >= IMQ5ExpectedTriggerSeconds)
		{
			FAudioDevice* TriggerDevice = PIE->GetAudioDeviceRaw();
			if (!State->CapturePreparationId || !TriggerDevice || TriggerDevice->DeviceID != State->AudioDeviceId
				|| TriggerDevice->GetSampleRate() != IMQ5Rate)
				return Fail(TEXT("The prepared Q5 device/rate/ID changed before the original source trigger."));
			FString PreparationError;
			if (!IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(TriggerDevice->DeviceID,
				uint32(TriggerDevice->GetSampleRate()), State->CapturePreparationId, PreparationError))
				return Fail(TEXT("Q5 pre-trigger capture validation failed: ") + PreparationError);
			if (!State->MasterRecorder.IsValid() || !State->MasterRecorder->IsValidLedger()
				|| !State->RecorderFence.IsFenceComplete())
				return Fail(TEXT("Master recording start command or callback ledger was not ready at the original 0.5s trigger."));
			uint64 SilentFrames = 0;
			for (const FIMQ5MasterBlock& Row : State->MasterRecorder->Snapshot())
			{
				if (Row.RequestPhase == 1 && Row.Energy <= IMQ5FloatSilenceEnergy) SilentFrames += uint64(Row.Frames);
			}
			if (SilentFrames < uint64(IMQ5Rate / 4))
				return Fail(TEXT("Master recorder did not render a quarter second of pre-trigger silence."));
			State->MasterTriggerRenderFrame = State->MasterRecorder->GetFrameCount();
			State->CurrentReport->SetBoolField(TEXT("master_start_audio_fence_completed_before_trigger"), true);
			State->CurrentReport->SetNumberField(TEXT("master_pretrigger_silent_render_frames"), SilentFrames);
			State->CurrentReport->SetNumberField(TEXT("source_trigger_master_render_frame"), State->MasterTriggerRenderFrame);
			FString Error;
			if (!SpawnDelayedGraphImpulse(PIE, Error)) return Fail(Error.IsEmpty() ? TEXT("Could not persist delayed source-trigger evidence.") : Error);
		}
		if (!State->bSourceSpawned) return false;
		SetStage(4);
		return false;
	}

	bool CaptureRoomWet(UWorld* PIE, double Now)
	{
		if (!PIE || !State->bRecording || !State->MetaContext.IsValid() || !State->Bridge.IsValid())
		{
			return Fail(TEXT("PIE audio capture or graph context disappeared during the wet window."));
		}
		if (Now - State->CaptureStartedSeconds < IMQ5CaptureSeconds) return false;

		FIMQ5GraphCaptureStop& Cut = State->GraphStop;
		Cut.BeginSeconds = FPlatformTime::Seconds();
		Cut.BusUnderrunsBeforeRecorderStop = State->MetaContext->BusUnderruns.load(std::memory_order_acquire);
		State->MasterStopBeforeFrame = State->MasterRecorder->GetFrameCount();
		State->MasterRecorder->SetRequestPhase(2);
		UAudioMixerBlueprintLibrary::StopRecordingOutput(PIE, EAudioRecordingExportType::WavFile, TEXT("wet"), CaseDirectory());
		State->MasterStopAfterFrame = State->MasterRecorder->GetFrameCount();
		// Freeze the completed graph prefix and acceptance counters before any
		// JSON/CSV I/O, source Stop or WAV settle can extend the measured window.
		Cut.EnvironmentPcmEnd = State->MetaContext->CapturedEnvironmentFrames.load(std::memory_order_acquire);
		Cut.SourcePcmEnd = State->MetaContext->CapturedSourceFrames.load(std::memory_order_acquire);
		Cut.SourceTraceEnd = State->MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire);
		Cut.EnvironmentTraceEnd = State->MetaContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
		Cut.BusTraceEnd = State->MetaContext->CapturedBusReadCount.load(std::memory_order_acquire);
		Cut.CounterDelta = CaptureCounterDelta();
		Cut.BusUnderruns = State->MetaContext->BusUnderruns.load(std::memory_order_acquire);
		Cut.PushBlocks = State->MetaContext->SourceSendPushBlocks.load(std::memory_order_acquire);
		Cut.PushFailures = State->MetaContext->SourceSendPushFailures.load(std::memory_order_acquire);
		Cut.TransportFailed = State->MetaContext->SourceSendTransportFailed.load(std::memory_order_acquire);
		Cut.EndSeconds = FPlatformTime::Seconds();
		Cut.Frozen = true;
		if (State->bCaptureTransportOrder)
		{
			// Separate diagnostic cut after all original clocks/counters freeze.
			State->TransportCutBeginSeconds = FPlatformTime::Seconds();
			State->TransportClaimsAtStop = State->MetaContext->CapturedTransportOrderClaims.load(std::memory_order_acquire);
			State->TransportCutEndSeconds = FPlatformTime::Seconds();
			State->bTransportOrderValid = false;
		}
		State->bRecording = false;
		State->CurrentReport->SetNumberField(TEXT("capture_stopped_wall_seconds"), Cut.EndSeconds);
		State->CurrentReport->SetNumberField(TEXT("capture_wall_seconds"), Cut.EndSeconds - State->CaptureStartedSeconds);
		State->CurrentReport->SetNumberField(TEXT("graph_context_active_wall_seconds"), Cut.EndSeconds - State->GraphContextObservedSeconds);
		State->CurrentReport->SetBoolField(TEXT("graph_capture_stop_snapshot_frozen"), true);
		State->CurrentReport->SetNumberField(TEXT("graph_stop_snapshot_before_wall_seconds"), Cut.BeginSeconds);
		State->CurrentReport->SetNumberField(TEXT("graph_stop_snapshot_after_wall_seconds"), Cut.EndSeconds);
		State->CurrentReport->SetStringField(TEXT("graph_stop_snapshot_contract"), TEXT("Separate atomic completed-prefix/counter reads after synchronous recorder Stop and before source Stop; bounded by these GT wall times. Graph capture origin and device WAV origin remain distinct."));
		State->CurrentReport->SetNumberField(TEXT("environment_pcm_end_frame"), Cut.EnvironmentPcmEnd);
		State->CurrentReport->SetNumberField(TEXT("source_pcm_end_frame"), Cut.SourcePcmEnd);
		State->CurrentReport->SetNumberField(TEXT("source_trace_end_block"), Cut.SourceTraceEnd);
		State->CurrentReport->SetNumberField(TEXT("environment_trace_end_block"), Cut.EnvironmentTraceEnd);
		State->CurrentReport->SetNumberField(TEXT("frozen_source_trace_end_block"), Cut.SourceTraceEnd);
		State->CurrentReport->SetNumberField(TEXT("frozen_environment_trace_end_block"), Cut.EnvironmentTraceEnd);
		State->CurrentReport->SetNumberField(TEXT("bus_trace_stop_rows"), Cut.BusTraceEnd);
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_before_stop_recording"), Cut.BusUnderrunsBeforeRecorderStop);
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_at_stop"), Cut.BusUnderruns);
		State->CurrentReport->SetNumberField(TEXT("bus_push_blocks_at_stop"), Cut.PushBlocks);
		State->CurrentReport->SetNumberField(TEXT("bus_push_failures_at_stop"), Cut.PushFailures);
		State->CurrentReport->SetBoolField(TEXT("bus_transport_failed_at_stop"), Cut.TransportFailed);
		State->ReleaseMasterRecorderListener();
		if (!WriteMasterRecorderLedger()) return Fail(TEXT("Could not persist the complete master recorder callback ledger."));
		if (!SaveCurrentReport()) return Fail(TEXT("Could not persist Q5 audio-window end counters."));
		if (State->Audio.IsValid()) State->Audio->Stop();
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_after_source_stop_request"), State->MetaContext->BusUnderruns.load(std::memory_order_acquire));
		SetStage(6);
		return false;
	}

	bool WriteMasterRecorderLedger()
	{
		if (!State->MasterRecorder.IsValid() || !State->MasterRecorder->IsValidLedger()) return false;
		const TArray<FIMQ5MasterBlock> Rows = State->MasterRecorder->Snapshot();
		if (Rows.IsEmpty()) return false;
		FString Csv = TEXT("index,frame_begin,frames,channels,rate,request_phase,audio_clock,wall_seconds,energy,peak\n");
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			const FIMQ5MasterBlock& Row = Rows[Index];
			Csv += FString::Printf(TEXT("%d,%llu,%d,%d,%d,%u,%.12f,%.12f,%.12g,%.9g\n"), Index,
				Row.FrameBegin, Row.Frames, Row.Channels, Row.Rate, uint32(Row.RequestPhase),
				Row.AudioClock, Row.WallSeconds, Row.Energy, double(Row.Peak));
		}
		State->CurrentReport->SetStringField(TEXT("master_recorder_trace_file"), CaseFile(TEXT("master-recorder-blocks.csv")));
		State->CurrentReport->SetNumberField(TEXT("master_recorder_trace_rows"), Rows.Num());
		State->CurrentReport->SetBoolField(TEXT("master_recorder_trace_overflow"), false);
		State->CurrentReport->SetNumberField(TEXT("master_stop_before_render_frame"), State->MasterStopBeforeFrame);
		State->CurrentReport->SetNumberField(TEXT("master_stop_after_render_frame"), State->MasterStopAfterFrame);
		State->CurrentReport->SetBoolField(TEXT("master_listener_unregister_requested_before_pie_end"), !State->bMasterRecorderRegistered);
		State->CurrentReport->SetStringField(TEXT("master_recorder_alignment"), TEXT("Callback frame zero is listener registration, before recording. Request-phase rows bracket asynchronous Start and synchronous Stop; they are not exact recorder frame indices. WAV native duration/pretrigger/decay gates remain authoritative."));
		return FFileHelper::SaveStringToFile(Csv, *CaseFile(TEXT("master-recorder-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FIMQ5DeviceCounters CaptureCounterDelta() const
	{
		FIMQ5DeviceCounters D;
		if (!State->Bridge.IsValid()) return D;
		D.Direct = State->Bridge->DirectNonzeroBlocks.load(std::memory_order_acquire) - State->CountersBegin.Direct;
		D.Path = State->Bridge->PathNonzeroBlocks.load(std::memory_order_acquire) - State->CountersBegin.Path;
		D.Rejected = State->Bridge->RejectedBlocks.load(std::memory_order_acquire) - State->CountersBegin.Rejected;
		D.ReverbRejected = State->Bridge->ReverbRejectedBlocks.load(std::memory_order_acquire) - State->CountersBegin.ReverbRejected;
		D.DryDropped = State->Bridge->DryDroppedBlocks.load(std::memory_order_acquire) - State->CountersBegin.DryDropped;
		D.WetNonzero = State->Bridge->ReverbNonzeroBlocks.load(std::memory_order_acquire) - State->CountersBegin.WetNonzero;
		D.ReverbProcessed = State->Bridge->ReverbProcessedBlocks.load(std::memory_order_acquire) - State->CountersBegin.ReverbProcessed;
		D.ReverbDry = State->Bridge->ReverbDryBlocks.load(std::memory_order_acquire) - State->CountersBegin.ReverbDry;
		return D;
	}

	void WriteTransportOrderTrace()
	{
		if (!State->bCaptureTransportOrder) return;
		const auto& Graph = *State->MetaContext;
		// Order-only uses recorded timestamps as-is, without the optional GT calibration probe.
		const uint64 ClockBefore = State->bOrderOnlyClosedLow ? 0 : FPlatformTime::Cycles64();
		const double ClockSeconds = State->bOrderOnlyClosedLow ? 0.0 : FPlatformTime::Seconds();
		const uint64 ClockAfter = State->bOrderOnlyClosedLow ? 0 : FPlatformTime::Cycles64();
		const double SecondsPerTick = State->bOrderOnlyClosedLow ? 0.0 : FPlatformTime::GetSecondsPerCycle64();
		const double SecondsOffset = State->bOrderOnlyClosedLow ? 0.0 : FPlatformTime::GetSecondsTimeOffset();
		const bool ClockValid = ClockBefore > 0 && ClockAfter >= ClockBefore
			&& FMath::IsFinite(ClockSeconds) && FMath::IsFinite(SecondsPerTick) && SecondsPerTick > 0.0
			&& FMath::IsFinite(SecondsOffset);
		IConsoleVariable* ParallelCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.DisableParallelSourceProcessing"));
		const int32 ParallelValue = ParallelCVar ? ParallelCVar->GetInt() : 0;
		UWorld* PIE = IMQ5FindPIEWorld();
		Audio::FMixerDevice* Mixer = PIE ? FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(PIE) : nullptr;
		const bool MixerPresent = Mixer && Mixer->DeviceID == Graph.DeviceId;
		const int32 MixerFrames = MixerPresent ? Mixer->GetNumOutputFrames() : 0;
		const uint32 SourcePublished = Graph.CapturedSourceBlockCount.load(std::memory_order_acquire);
		const uint32 Claimed = Graph.CapturedTransportOrderClaims.load(std::memory_order_acquire);
		const uint32 Capacity = uint32(Graph.CapturedTransportOrder.Num());
		const bool Overflow = Graph.CapturedTransportOrderOverflow.load(std::memory_order_acquire);
		uint32 End = 0;
		if (Graph.CapturedTransportOrderReady)
		{
			const uint32 Limit = FMath::Min(Claimed, Capacity);
			// Acquire ready before reading a row. An in-flight writer is never
			// waited on, nor does export read or repair its unpublished payload.
			for (; End < Limit; ++End)
			{
				const uint32 Word = End / 64u;
				if (Word >= Graph.CapturedTransportOrderReadyWordCount
					|| !(Graph.CapturedTransportOrderReady[Word].load(std::memory_order_acquire)
						& (uint64(1) << (End % 64u)))) break;
			}
		}
		bool Valid = !Overflow && Capacity == FIMAcousticMetaSoundContext::TransportOrderEventCapacity
			&& State->GraphStop.Frozen && State->TransportClaimsAtStop > 0 && End >= State->TransportClaimsAtStop;
		// Rows are immutable and Context stays strongly owned through this GT
		// export. New stage completeness is separate from original native gates.
		struct FIMRendererPair
		{
			const FIMAcousticTransportOrderEvent* Enter = nullptr;
			const FIMAcousticTransportOrderEvent* Before = nullptr;
			const FIMAcousticTransportOrderEvent* After = nullptr;
			const FIMAcousticTransportOrderEvent* Probe = nullptr;
		};
		TMap<uint32, FIMRendererPair> RendererPairs;
		bool StageFieldsValid = true;
		bool ThreadCpuFieldsValid = true;
		// CPU query validity is independent of the original Renderer flags/gates.
		constexpr uint32 RendererFlagMask = 127u, ThreadCpuFlagMask = 384u, KnownFlagMask = 511u;
		const uint32 CpuAttemptedBit = uint32(EIMAcousticTransportWorkFlag::ThreadCpuQueryAttempted);
		const uint32 CpuSucceededBit = uint32(EIMAcousticTransportWorkFlag::ThreadCpuQuerySucceeded);
		FString Csv = TEXT("index,group,phase,seconds,thread,device,rate,slot,audio,world,source_generation,reader_ordinal,source_trace_index,frames,available,returned,active,finished,fresh,reason,push_blocks,underruns,work_cycles,work_flags,thread_cpu_cycles\n");
		for (uint32 Index = 0; Index < End; ++Index)
		{
			const FIMAcousticTransportOrderEvent& E = Graph.CapturedTransportOrder[int32(Index)];
			Valid = Valid && E.Index == Index && E.Group <= Index && FMath::IsFinite(E.Seconds) && E.Seconds > 0.0
				&& E.DeviceId == Graph.DeviceId && E.SampleRate == Graph.Device->SampleRate
				&& E.WorldGeneration == Graph.Pool->WorldGeneration
				&& uint32(E.Phase) < uint32(EIMAcousticTransportOrderPhase::Count);
			if (State->bOrderOnlyClosedLow)
			{
				const bool bReader = E.Phase >= EIMAcousticTransportOrderPhase::ReaderBegin
					&& E.Phase <= EIMAcousticTransportOrderPhase::ReaderEnd;
				Valid = Valid && E.ThreadId != 0 && (bReader ? E.SourceGeneration == 0 : E.SourceGeneration != 0);
				ThreadCpuFieldsValid = ThreadCpuFieldsValid && E.ThreadCpuCycles == 0 && (E.WorkFlags & ThreadCpuFlagMask) == 0;
			}
			const bool Before = E.Phase == EIMAcousticTransportOrderPhase::SourceBeforeRenderer;
			const bool After = E.Phase == EIMAcousticTransportOrderPhase::SourceAfterRenderer;
			if (Before || After)
			{
				StageFieldsValid = StageFieldsValid && E.WorkCycles > 0 && E.Fresh
					&& (E.WorkFlags & uint32(EIMAcousticTransportWorkFlag::Fresh)) != 0
					&& (E.WorkFlags & ~KnownFlagMask) == 0
					&& (!Before || (E.WorkFlags & uint32(EIMAcousticTransportWorkFlag::Rendered)) == 0);
				const bool CpuAttempted = (E.WorkFlags & CpuAttemptedBit) != 0;
				const bool CpuSucceeded = (E.WorkFlags & CpuSucceededBit) != 0;
				ThreadCpuFieldsValid = ThreadCpuFieldsValid && (!CpuSucceeded || CpuAttempted)
					&& (CpuSucceeded || E.ThreadCpuCycles == 0);
				FIMRendererPair& Pair = RendererPairs.FindOrAdd(E.Group);
				const FIMAcousticTransportOrderEvent*& Target = Before ? Pair.Before : Pair.After;
				StageFieldsValid = StageFieldsValid && !Target;
				Target = &E;
			}
			else
			{
				StageFieldsValid = StageFieldsValid && E.WorkCycles == 0 && E.WorkFlags == 0;
				ThreadCpuFieldsValid = ThreadCpuFieldsValid && E.ThreadCpuCycles == 0 && (E.WorkFlags & ThreadCpuFlagMask) == 0;
				if (E.Phase == EIMAcousticTransportOrderPhase::SourceEnter)
					RendererPairs.FindOrAdd(E.Group).Enter = &E;
				else if (E.Phase == EIMAcousticTransportOrderPhase::SourceProbeLink)
					RendererPairs.FindOrAdd(E.Group).Probe = &E;
			}
			Csv += FString::Printf(TEXT("%u,%u,%u,%.12f,%u,%u,%u,%d,%llu,%llu,%llu,%u,%u,%d,%d,%d,%d,%u,%u,%u,%llu,%llu,%llu,%u,%llu\n"),
				E.Index, E.Group, uint32(E.Phase), E.Seconds, E.ThreadId, E.DeviceId, E.SampleRate, E.Slot,
				E.AudioId, E.WorldGeneration, E.SourceGeneration, E.ReaderOrdinal, E.SourceTraceIndex,
				E.Frames, E.Available, E.Returned, E.Active, uint32(E.Finished), uint32(E.Fresh), uint32(E.Reason),
				E.PushBlocks, E.Underruns, E.WorkCycles, E.WorkFlags, E.ThreadCpuCycles);
		}
		bool StagePairsValid = StageFieldsValid;
		uint32 RequiredGroups = 0, CompleteGroups = 0;
		uint32 ThreadCpuValidGroups = 0, ThreadCpuFailedQueryGroups = 0, ThreadCpuWrongThreadGroups = 0;
		uint32 ThreadCpuMissingPairGroups = 0, ThreadCpuNonmonotonicGroups = 0, ThreadCpuZeroDeltaGroups = 0;
		uint32 ThreadCpuNotAttemptedGroups = 0, ThreadCpuInvalidStructureGroups = 0;
		for (const auto& Entry : RendererPairs)
		{
			const FIMRendererPair& P = Entry.Value;
			if (!P.Enter) { StagePairsValid = false; continue; }
			if (P.Enter->Index >= State->TransportClaimsAtStop) continue; // diagnostic only after original cut
			if (!P.Probe || P.Probe->SourceTraceIndex >= SourcePublished
				|| !Graph.CapturedSourceBlocks.IsValidIndex(P.Probe->SourceTraceIndex))
			{ StagePairsValid = false; continue; }
			const FIMAcousticBlockProbe& Probe = Graph.CapturedSourceBlocks[P.Probe->SourceTraceIndex];
			const bool Attempted = Probe.Reject == EIMAcousticProbeReject::Accepted || Probe.Reject == EIMAcousticProbeReject::RendererFailed;
			if (!Attempted)
			{
				StagePairsValid = StagePairsValid && !P.Before && !P.After;
				continue;
			}
			++RequiredGroups;
			const uint32 RenderedBit = uint32(EIMAcousticTransportWorkFlag::Rendered);
			const bool PairValid = P.Before && P.After && P.Enter->Index < P.Before->Index
				&& P.Before->Index < P.After->Index && P.After->Index < P.Probe->Index
				&& P.Before->ThreadId == P.Enter->ThreadId && P.After->ThreadId == P.Enter->ThreadId
				&& P.Before->Slot == P.Enter->Slot && P.After->Slot == P.Enter->Slot
				&& P.Before->AudioId == P.Enter->AudioId && P.After->AudioId == P.Enter->AudioId
				&& P.Before->SourceGeneration == P.Enter->SourceGeneration && P.After->SourceGeneration == P.Enter->SourceGeneration
				&& P.Before->WorkCycles <= P.After->WorkCycles
				&& P.Before->Seconds >= P.Enter->Seconds && P.After->Seconds >= P.Before->Seconds && P.Probe->Seconds >= P.After->Seconds
				&& (P.Before->WorkFlags & RendererFlagMask) == ((P.After->WorkFlags & RendererFlagMask) & ~RenderedBit);
			StagePairsValid = StagePairsValid && PairValid;
			CompleteGroups += PairValid ? 1u : 0u;
			if (!P.Before || !P.After) ++ThreadCpuMissingPairGroups;
			else if (!P.Enter->ThreadId || P.Before->ThreadId != P.Enter->ThreadId || P.After->ThreadId != P.Enter->ThreadId)
				++ThreadCpuWrongThreadGroups;
			else if (!PairValid) ++ThreadCpuInvalidStructureGroups;
			else if ((P.Before->WorkFlags & CpuAttemptedBit) == 0 || (P.After->WorkFlags & CpuAttemptedBit) == 0)
				++ThreadCpuNotAttemptedGroups;
			else if ((P.Before->WorkFlags & CpuSucceededBit) == 0 || (P.After->WorkFlags & CpuSucceededBit) == 0)
				++ThreadCpuFailedQueryGroups;
			else if (P.After->ThreadCpuCycles < P.Before->ThreadCpuCycles) ++ThreadCpuNonmonotonicGroups;
			else
			{
				++ThreadCpuValidGroups;
				ThreadCpuZeroDeltaGroups += P.After->ThreadCpuCycles == P.Before->ThreadCpuCycles ? 1u : 0u;
			}
		}
		if (State->bOrderOnlyClosedLow)
		{
			// One ready-prefix read only: reject any hole, missing stage/identity, or CPU query bits.
			Valid = Valid && StagePairsValid && ThreadCpuFieldsValid && End == FMath::Min(Claimed, Capacity)
				&& sizeof(FIMAcousticTransportOrderEvent) == 120 && Graph.CapturedTransportOrderReadyWordCount == 128;
		}
		TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
		Header->SetStringField(TEXT("schema"), TEXT("IMAcousticTransportOrderObservation.v3"));
		Header->SetStringField(TEXT("run_id"), State->RunId);
		Header->SetStringField(TEXT("case"), IMQ5Cases[State->CaseIndex].Name);
		Header->SetNumberField(TEXT("device_id"), Graph.DeviceId);
		Header->SetNumberField(TEXT("sample_rate"), Graph.Device->SampleRate);
		Header->SetNumberField(TEXT("world_generation"), Graph.Pool->WorldGeneration);
		Header->SetStringField(TEXT("preparation_id"), FString::Printf(TEXT("%llu"), Graph.TestCapturePreparationId));
		Header->SetNumberField(TEXT("capacity_events"), Capacity);
		Header->SetNumberField(TEXT("event_bytes"), sizeof(FIMAcousticTransportOrderEvent));
		Header->SetNumberField(TEXT("ready_words"), Graph.CapturedTransportOrderReadyWordCount);
		const uint32 EventPayloadBytes = uint32(sizeof(FIMAcousticTransportOrderEvent)) * Capacity;
		const uint32 ReadyBytes = uint32(sizeof(std::atomic<uint64>)) * Graph.CapturedTransportOrderReadyWordCount;
		Header->SetNumberField(TEXT("event_payload_bytes"), EventPayloadBytes);
		Header->SetNumberField(TEXT("ready_bytes"), ReadyBytes);
		Header->SetNumberField(TEXT("payload_plus_ready_bytes"), EventPayloadBytes + ReadyBytes);
		Header->SetNumberField(TEXT("claimed_at_stop"), State->TransportClaimsAtStop);
		Header->SetNumberField(TEXT("claimed_at_export"), Claimed);
		Header->SetNumberField(TEXT("published_prefix"), End);
		Header->SetBoolField(TEXT("overflow"), Overflow);
		Header->SetBoolField(TEXT("ready_hole_at_export"), End < FMath::Min(Claimed, Capacity));
		Header->SetNumberField(TEXT("diagnostic_cut_begin_s"), State->TransportCutBeginSeconds);
		Header->SetNumberField(TEXT("diagnostic_cut_end_s"), State->TransportCutEndSeconds);
		Header->SetNumberField(TEXT("original_stop_begin_s"), State->GraphStop.BeginSeconds);
		Header->SetNumberField(TEXT("original_stop_end_s"), State->GraphStop.EndSeconds);
		Header->SetNumberField(TEXT("original_bus_stop_rows"), State->GraphStop.BusTraceEnd);
		Header->SetStringField(TEXT("order_contract"), TEXT("Claim indices order observer claims only. Times bracket original API calls, not internal FIFO commit or device PCM frames. Release ready protects immutable rows; no new transport happens-before is inferred. Rows after original bus_stop_rows are diagnostic only."));
		Header->SetStringField(TEXT("work_cycles_domain"), TEXT("FPlatformTime::Cycles64/WindowsQPC"));
		Header->SetNumberField(TEXT("work_seconds_per_tick"), SecondsPerTick);
		Header->SetNumberField(TEXT("work_seconds_offset"), SecondsOffset);
		Header->SetStringField(TEXT("work_clock_calibration_cycles_before"), FString::Printf(TEXT("%llu"), ClockBefore));
		Header->SetNumberField(TEXT("work_clock_calibration_seconds"), ClockSeconds);
		Header->SetStringField(TEXT("work_clock_calibration_cycles_after"), FString::Printf(TEXT("%llu"), ClockAfter));
		Header->SetBoolField(TEXT("work_clock_calibration_valid"), ClockValid);
		Header->SetStringField(TEXT("work_clock_calibration_status"), State->bOrderOnlyClosedLow ? TEXT("NOT_ATTEMPTED_ORDER_ONLY") : (ClockValid ? TEXT("CALIBRATED_GT_EXPORT") : TEXT("UNKNOWN_QPC_CALIBRATION")));
		Header->SetStringField(TEXT("work_flags_bits"), TEXT("1=Fresh,2=FirstAcceptedAttempt,4=DirectValid,8=PathValid,16=RenderedAfter,32=DirectAudibleRoute,64=PathAudibleRoute,128=ThreadCpuQueryAttempted,256=ThreadCpuQuerySucceeded"));
		Header->SetBoolField(TEXT("renderer_stage_fields_valid"), StageFieldsValid);
		Header->SetBoolField(TEXT("renderer_stage_pairs_valid"), StagePairsValid);
		Header->SetNumberField(TEXT("renderer_required_groups"), RequiredGroups);
		Header->SetNumberField(TEXT("renderer_complete_groups"), CompleteGroups);
		Header->SetStringField(TEXT("thread_cpu_method"), TEXT("QueryThreadCycleTime(GetCurrentThread())"));
		Header->SetStringField(TEXT("thread_cpu_unit"), TEXT("raw_cpu_cycle_count"));
		Header->SetBoolField(TEXT("thread_cpu_supported_platform"), PLATFORM_WINDOWS != 0);
		Header->SetStringField(TEXT("thread_cpu_flag_bits"), TEXT("128=Attempted,256=Succeeded"));
		Header->SetBoolField(TEXT("thread_cpu_fields_valid"), ThreadCpuFieldsValid);
		Header->SetNumberField(TEXT("thread_cpu_required_groups"), RequiredGroups);
		Header->SetNumberField(TEXT("thread_cpu_valid_groups"), ThreadCpuValidGroups);
		Header->SetNumberField(TEXT("thread_cpu_failed_query_groups"), ThreadCpuFailedQueryGroups);
		Header->SetNumberField(TEXT("thread_cpu_wrong_thread_groups"), ThreadCpuWrongThreadGroups);
		Header->SetNumberField(TEXT("thread_cpu_missing_pair_groups"), ThreadCpuMissingPairGroups);
		Header->SetNumberField(TEXT("thread_cpu_nonmonotonic_groups"), ThreadCpuNonmonotonicGroups);
		Header->SetNumberField(TEXT("thread_cpu_zero_delta_groups"), ThreadCpuZeroDeltaGroups);
		Header->SetNumberField(TEXT("thread_cpu_not_attempted_groups"), ThreadCpuNotAttemptedGroups);
		Header->SetNumberField(TEXT("thread_cpu_invalid_structure_groups"), ThreadCpuInvalidStructureGroups);
		Header->SetStringField(TEXT("thread_cpu_interpretation"), TEXT("Same calling Source thread's reported user+kernel raw CPU cycles. CPU endpoints surround the QPC bracket and include observer/query work. No conversion to elapsed time, subtraction from QPC, SDK-exclusive CPU or OS residency/preemption inference. Zero delta does not prove no execution. API precision/latency and auxiliary threads are unmeasured; opt-in queries may perturb this run."));
		Header->SetBoolField(TEXT("parallel_source_processing_cvar_present"), ParallelCVar != nullptr);
		Header->SetNumberField(TEXT("parallel_source_processing_cvar_value"), ParallelValue);
		Header->SetBoolField(TEXT("mixer_frames_present"), MixerPresent);
		Header->SetNumberField(TEXT("mixer_frames"), MixerFrames);
		Header->SetStringField(TEXT("scheduling_metadata_status"), ParallelCVar && MixerPresent ? TEXT("ACTUAL_GT_EXPORT") : TEXT("UNKNOWN_CVAR_OR_MIXER"));
		const TCHAR* Names[] = { TEXT("SourceEnter"), TEXT("SourceProbeLink"), TEXT("PushBegin"), TEXT("PushReturn"),
			TEXT("PushSkipped"), TEXT("ActiveStore"), TEXT("SourceCompleted"), TEXT("ReleaseBegin"), TEXT("ReleaseInactive"),
			TEXT("ReaderBegin"), TEXT("ActiveLoad"), TEXT("QueueReadBegin"), TEXT("QueueReadReturn"), TEXT("ActiveRecheck"),
			TEXT("PopReturn"), TEXT("ReaderDecision"), TEXT("ReaderEnd"), TEXT("SourceBeforeRenderer"), TEXT("SourceAfterRenderer") };
		TSharedRef<FJsonObject> Phases = MakeShared<FJsonObject>();
		for (uint32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
			Phases->SetStringField(FString::FromInt(int32(Index)), Names[Index]);
		Header->SetObjectField(TEXT("phase_names"), Phases);
		Header->SetStringField(TEXT("reason_names"), TEXT("0=None,1=MissingProducer,2=DisabledTransport,3=StoppedContext,4=Finished,5=Reset,6=Destruction,7=Explicit,8=InitialQueue,9=InactiveQueue"));
		const bool CsvSaved = FFileHelper::SaveStringToFile(Csv, *CaseFile(TEXT("transport-order-events.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		Valid = Valid && CsvSaved;
		Header->SetStringField(TEXT("renderer_stage_status"), Valid && StagePairsValid ? TEXT("COMPLETE") : TEXT("MISSING"));
		Header->SetBoolField(TEXT("metadata_and_ready_prefix_through_stop_valid"), Valid);
		const TCHAR* ThreadCpuStatus = TEXT("COMPLETE_RAW_PAIRS");
		if (!RequiredGroups) ThreadCpuStatus = TEXT("UNKNOWN_NO_REQUIRED_GROUPS");
		else if (ThreadCpuMissingPairGroups) ThreadCpuStatus = TEXT("UNKNOWN_MISSING_PAIR");
		else if (ThreadCpuWrongThreadGroups) ThreadCpuStatus = TEXT("UNKNOWN_WRONG_THREAD");
		else if (ThreadCpuNotAttemptedGroups) ThreadCpuStatus = TEXT("UNKNOWN_UNSUPPORTED_OR_NOT_ATTEMPTED");
		else if (ThreadCpuFailedQueryGroups) ThreadCpuStatus = TEXT("UNKNOWN_QUERY_FAILED");
		else if (ThreadCpuNonmonotonicGroups) ThreadCpuStatus = TEXT("UNKNOWN_NONMONOTONIC");
		else if (!ThreadCpuFieldsValid) ThreadCpuStatus = TEXT("UNKNOWN_INVALID_CPU_FIELDS");
		else if (!Valid || !StagePairsValid || ThreadCpuValidGroups != RequiredGroups)
			ThreadCpuStatus = TEXT("UNKNOWN_INVALID_STRUCTURE_OR_PREFIX");
		if (State->bOrderOnlyClosedLow)
			ThreadCpuStatus = ThreadCpuFieldsValid ? TEXT("NOT_ATTEMPTED_IN_READY_PREFIX") : TEXT("REJECTED_NONZERO_CPU_FIELDS");
		Header->SetStringField(TEXT("thread_cpu_status"), ThreadCpuStatus);
		const bool HeaderSaved = IMQ5WriteJson(CaseFile(TEXT("transport-order-observation.json")), Header);
		State->bTransportOrderValid = Valid && HeaderSaved;
		State->CurrentReport->SetBoolField(TEXT("transport_order_opt_in"), true);
		State->CurrentReport->SetBoolField(TEXT("transport_order_metadata_valid"), State->bTransportOrderValid);
		State->CurrentReport->SetStringField(TEXT("transport_order_renderer_stage_status"), Valid && StagePairsValid ? TEXT("COMPLETE") : TEXT("MISSING"));
		State->CurrentReport->SetStringField(TEXT("transport_order_thread_cpu_status"), ThreadCpuStatus);
		State->CurrentReport->SetStringField(TEXT("transport_order_clock_calibration_status"), State->bOrderOnlyClosedLow ? TEXT("NOT_ATTEMPTED_ORDER_ONLY") : (ClockValid ? TEXT("CALIBRATED_GT_EXPORT") : TEXT("UNKNOWN_QPC_CALIBRATION")));
		State->CurrentReport->SetStringField(TEXT("transport_order_trace_file"), CaseFile(TEXT("transport-order-events.csv")));
		State->CurrentReport->SetStringField(TEXT("transport_order_header_file"), CaseFile(TEXT("transport-order-observation.json")));
	}

	bool WriteBusReadTrace(FString& Error)
	{
		const auto& Graph = *State->MetaContext;
		const uint32 End = Graph.CapturedBusReadCount.load(std::memory_order_acquire);
		const bool Overflow = Graph.CapturedBusReadOverflow.load(std::memory_order_acquire);
		if (!State->GraphStop.Frozen || Overflow || End == 0 || End >= uint32(Graph.CapturedBusReads.Num())
			|| State->GraphStop.BusTraceEnd == 0 || State->GraphStop.BusTraceEnd > End)
		{
			Error = TEXT("Q5 bus-reader observation is absent, at capacity, overflowed or missing the frozen Stop prefix.");
			return false;
		}
		FString Csv = TEXT("index,ordinal,begin_s,end_s,frames,prime_frames,capacity_frames,result,fresh,primed_before,primed_after,transport_failed_before,transport_failed_after,input_count,active_count,min_available,max_available,blocking_slot,blocking_audio_component_id,blocking_available,blocking_active,blocking_active_after,blocking_queue_after,blocking_push_blocks,min_popped,underruns_before,underruns_after,read_blocks,priming_blocks,silent_blocks,push_blocks,push_failures\n");
		uint64 PreviousUnderruns = 0;
		double PreviousEndSeconds = 0.0;
		for (uint32 Index = 0; Index < End; ++Index)
		{
			const FIMAcousticBusReadBlock& B = Graph.CapturedBusReads[int32(Index)];
			const bool Fault = B.Result == EIMAcousticBusReadResult::LatchedFault
				|| B.Result == EIMAcousticBusReadResult::ShortAvailable || B.Result == EIMAcousticBusReadResult::ShortPop;
			if (B.Ordinal != Index || !FMath::IsFinite(B.BeginSeconds) || !FMath::IsFinite(B.EndSeconds)
				|| B.BeginSeconds < PreviousEndSeconds || B.EndSeconds < B.BeginSeconds
				|| B.UnderrunsBefore != PreviousUnderruns || B.UnderrunsAfter != B.UnderrunsBefore + uint64(Fault))
			{
				Error = TEXT("Q5 bus-reader rows are incomplete, reordered or disagree with the actual fault counter branches.");
				return false;
			}
			PreviousUnderruns = B.UnderrunsAfter;
			PreviousEndSeconds = B.EndSeconds;
			Csv += FString::Printf(TEXT("%u,%u,%.12f,%.12f,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%llu,%d,%u,%u,%d,%llu,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n"),
				Index, B.Ordinal, B.BeginSeconds, B.EndSeconds, B.Frames, B.PrimeFrames, B.CapacityFrames,
				uint32(B.Result), uint32(B.ReturnedFresh), uint32(B.PrimedBefore), uint32(B.PrimedAfter),
				uint32(B.TransportFailedBefore), uint32(B.TransportFailedAfter), B.InputCount, B.ActiveCount,
				B.MinAvailable, B.MaxAvailable, B.BlockingSlot, B.BlockingAudioId, B.BlockingAvailable,
				uint32(B.BlockingActive), uint32(B.BlockingActiveAfter), B.BlockingQueueAfter, B.BlockingPushBlocks,
				B.MinPopped, B.UnderrunsBefore, B.UnderrunsAfter, B.ReadBlocks, B.PrimingBlocks, B.SilentBlocks,
				B.PushBlocks, B.PushFailures);
		}
		State->CurrentReport->SetStringField(TEXT("bus_reader_trace_file"), CaseFile(TEXT("graph-bus-reader-blocks.csv")));
		State->CurrentReport->SetStringField(TEXT("bus_reader_trace_origin"), TEXT("Single reader call ordinal and FPlatformTime brackets, beginning at context construction; not device PCM frames. Rows after frozen bus_trace_stop_rows are diagnostic only. Separate atomics may bracket one in-flight row at Stop."));
		State->CurrentReport->SetNumberField(TEXT("bus_trace_rows"), End);
		State->CurrentReport->SetNumberField(TEXT("bus_trace_capacity"), Graph.CapturedBusReads.Num());
		State->CurrentReport->SetBoolField(TEXT("bus_trace_overflow"), Overflow);
		State->CurrentReport->SetBoolField(TEXT("bus_trace_complete_through_stop"), true);
		State->CurrentReport->SetNumberField(TEXT("bus_trace_post_stop_rows"), End - State->GraphStop.BusTraceEnd);
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_last_published_row"), PreviousUnderruns);
		const uint64 LiveUnderruns = Graph.BusUnderruns.load(std::memory_order_acquire);
		if (LiveUnderruns < State->GraphStop.BusUnderruns)
		{
			Error = TEXT("The graph bus counter decreased after its frozen Stop sample.");
			return false;
		}
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_at_analysis"), LiveUnderruns);
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_after_stop_delta"), LiveUnderruns - State->GraphStop.BusUnderruns);
		return FFileHelper::SaveStringToFile(Csv, *CaseFile(TEXT("graph-bus-reader-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WriteSourceTraceAndValidate(uint32 End, FIMQ5Decay& GraphWetDecay, FString& Error, double& OutTriggerDelay)
	{
		if (!State->MetaContext.IsValid() || !State->Bridge.IsValid() || !State->Audio.IsValid())
		{
			Error = TEXT("Graph source, environment context, bridge, or AudioComponent disappeared before trace export.");
			return false;
		}
		const uint32 Published = FMath::Min<uint32>(
			State->MetaContext->CapturedSourceBlockCount.load(std::memory_order_acquire),
			uint32(State->MetaContext->CapturedSourceBlocks.Num()));
		if (!State->GraphStop.Frozen || End >= uint32(State->MetaContext->CapturedSourceBlocks.Num())
			|| End > Published || End < State->SourceTraceBegin)
		{
			Error = TEXT("Graph source trace did not publish a complete bounded prefix.");
			return false;
		}
		const uint64 ExpectedAudioId = State->Audio->GetAudioComponentID();
		const uint64 ExpectedWorldGeneration = State->Bridge->WorldGeneration.load(std::memory_order_acquire);
		TSet<uint64> CallbackIds;
		uint32 AcceptedMatchingRows = 0;
		uint32 AcceptedImpulseRows = 0;
		uint32 RejectRows = 0;
		uint32 PrimingRows = 0;
		uint32 SilentStartupRows = 0;
		uint32 RouteContaminationRows = 0;
		double FirstConsumedSeconds = DBL_MAX;
		double FirstAcceptedImpulseSeconds = DBL_MAX;
		double SourceBlockInputEnergy = 0.0;
		bool bAcceptedSourceResult = false;
		FString Csv = TEXT("index,block,voice,audio_component_id,result_audio_component_id,world_generation,voice_generation,spatial_version,sequence,consumed_s,snapshot_s,age_ms,listener_x_m,listener_y_m,listener_z_m,direction_x,direction_y,direction_z,occlusion,distance_gain,routes,direct_valid,path_valid,reject,fallback,input_delay_priming,reject_detail,input_energy,direct_energy,path_energy,output_energy\n");
		for (uint32 Index = State->SourceTraceBegin; Index < End; ++Index)
		{
			const FIMAcousticBlockProbe& B = State->MetaContext->CapturedSourceBlocks[int32(Index)];
			CallbackIds.Add(B.CallbackAudioComponentId);
			if (B.InputDelayPriming)
			{
				if (B.Reject == EIMAcousticProbeReject::Accepted || B.InputEnergy > 1e-12)
				{
					Error = TEXT("A source input-delay priming block carried delayed audio or rendered unexpectedly.");
					return false;
				}
				++PrimingRows;
			}
			else if (B.Reject != EIMAcousticProbeReject::Accepted)
			{
				// A newly bound graph may execute silent blocks before its
				// first worker result. Only nonzero or post-accept failures
				// indicate lost acoustic input in this impulse measurement.
				if (!bAcceptedSourceResult && B.Reject == EIMAcousticProbeReject::MissingResult
					&& B.InputEnergy <= 1e-12) ++SilentStartupRows;
				else ++RejectRows;
			}
			else bAcceptedSourceResult = true;
			if ((B.Routes & 3u) != 0u) ++RouteContaminationRows;
			SourceBlockInputEnergy += FMath::Max(0.0, B.InputEnergy);
			if (B.CallbackAudioComponentId == ExpectedAudioId
				&& B.ResultAudioComponentId == ExpectedAudioId
				&& B.ResultWorldGeneration == ExpectedWorldGeneration
				&& B.ResultVoiceGeneration != 0
				&& B.ResultSequence != 0
				&& B.Reject == EIMAcousticProbeReject::Accepted)
			{
				++AcceptedMatchingRows;
				FirstConsumedSeconds = FMath::Min(FirstConsumedSeconds, B.ConsumedSeconds);
				if (B.InputEnergy > 1e-12)
				{
					++AcceptedImpulseRows;
					FirstAcceptedImpulseSeconds = FMath::Min(FirstAcceptedImpulseSeconds, B.ConsumedSeconds);
				}
			}
			Csv += FString::Printf(TEXT("%u,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%.9f,%.6f,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%.17g,%.17g,%.17g,%.17g\n"),
				Index, B.Block, B.Voice, B.CallbackAudioComponentId, B.ResultAudioComponentId,
				B.ResultWorldGeneration, B.ResultVoiceGeneration, B.ResultSpatialVersion, B.ResultSequence,
				B.ConsumedSeconds, B.SnapshotCaptured, B.AgeMs, B.ListenerX, B.ListenerY, B.ListenerZ,
				B.DirX, B.DirY, B.DirZ, B.Occlusion, B.DistanceGain, B.Routes, B.DirectValid, B.PathValid,
				uint32(B.Reject), B.Fallback, B.InputDelayPriming, B.RejectDetail,
				B.InputEnergy, B.DirectEnergy, B.PathEnergy, B.OutputEnergy);
		}
		if (!FFileHelper::SaveStringToFile(Csv, *CaseFile(TEXT("graph-source-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Error = TEXT("Could not save the graph-native source block identity/direction/occlusion/path trace.");
			return false;
		}

		const uint32 EnvironmentTraceEnd = State->GraphStop.EnvironmentTraceEnd;
		const uint32 EnvironmentTracePublished = State->MetaContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
		if (EnvironmentTraceEnd >= uint32(State->MetaContext->CapturedBlocks.Num())
			|| EnvironmentTraceEnd > EnvironmentTracePublished || EnvironmentTraceEnd <= State->EnvironmentTraceBegin)
		{
			Error = TEXT("The production MetaSound environment graph did not publish block identity rows during recording.");
			return false;
		}
		uint32 FreshEnvironmentRows = 0;
		FString EnvironmentCsv = TEXT("index,frame,sequence,spatial_version,expected_spatial_version,seconds,listener_x_m,listener_y_m,listener_z_m,fresh\n");
		for (uint32 Index = State->EnvironmentTraceBegin; Index < EnvironmentTraceEnd; ++Index)
		{
			const FIMAcousticMetaSoundBlock& B = State->MetaContext->CapturedBlocks[int32(Index)];
			if (B.Fresh) ++FreshEnvironmentRows;
			EnvironmentCsv += FString::Printf(TEXT("%u,%llu,%llu,%llu,%llu,%.9f,%.9g,%.9g,%.9g,%u\n"),
				Index, B.Frame, B.Sequence, B.SpatialVersion, B.ExpectedSpatialVersion,
				B.Seconds, B.ListenerX, B.ListenerY, B.ListenerZ, uint32(B.Fresh));
		}
		if (!FFileHelper::SaveStringToFile(EnvironmentCsv, *CaseFile(TEXT("graph-environment-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Error = TEXT("Could not save the production environment graph block/IR identity trace.");
			return false;
		}

		const uint32 SourceFrames = State->GraphStop.SourcePcmEnd;
		if (SourceFrames >= uint32(State->MetaContext->CapturedSource.Num())
			|| SourceFrames > State->MetaContext->CapturedSourceFrames.load(std::memory_order_acquire)
			|| SourceFrames <= State->SourcePcmBegin || !IMQ5WriteFloatWindow(
			CaseFile(TEXT("graph-source-mono-f32le.bin")), State->MetaContext->CapturedSource,
			State->SourcePcmBegin, SourceFrames, 1))
		{
			Error = TEXT("Graph-native mono source PCM was empty or could not be saved.");
			return false;
		}
		double SourcePcmEnergy = 0.0;
		for (uint32 I = State->SourcePcmBegin; I < SourceFrames; ++I)
		{
			const double V = State->MetaContext->CapturedSource[int32(I)];
			SourcePcmEnergy += V * V;
		}
		if (!(SourcePcmEnergy > 0.0) || !(SourceBlockInputEnergy > 0.0))
		{
			Error = TEXT("The real Q5 MetaSound source graph produced no nonzero impulse PCM/input energy.");
			return false;
		}

		const uint32 WetFramesPublished = State->GraphStop.EnvironmentPcmEnd;
		if (WetFramesPublished >= uint32(State->MetaContext->CapturedWet.Num() / 2)
			|| WetFramesPublished > State->MetaContext->CapturedEnvironmentFrames.load(std::memory_order_acquire))
		{
			Error = TEXT("Frozen graph PCM reaches capacity or exceeds the completed prefix.");
			return false;
		}
		const uint32 WetFrames = WetFramesPublished > State->EnvironmentPcmBegin
			? WetFramesPublished - State->EnvironmentPcmBegin : 0;
		State->GraphWetFramesRecorded = WetFrames;
		const uint32 MinimumGraphWetFrames = uint32(IMQ5Rate
			* (IMQ5CaptureSeconds - IMQ5ExpectedTriggerSeconds - IMQ5TriggerToleranceSeconds));
		if (WetFrames < MinimumGraphWetFrames
			&& FParse::Param(FCommandLine::Get(), TEXT("IMAcousticQ5LengthFailurePCM")))
		{
			// GT-only, default-off failure evidence. Use the original frozen cut;
			// never wait, recapture, append post-Stop samples or change acceptance.
			const auto& Graph = *State->MetaContext;
			const uint32 BeginFrame = State->EnvironmentPcmBegin, EndFrame = State->GraphStop.EnvironmentPcmEnd;
			constexpr int32 Channels = 2;
			const uint64 BeginSample = uint64(BeginFrame) * Channels, EndSample = uint64(EndFrame) * Channels;
			const uint64 ByteCount = (EndFrame >= BeginFrame ? EndSample - BeginSample : 0u) * sizeof(float);
			FString Status = TEXT("INVALID_FROZEN_RANGE_OR_FORMAT");
			bool PayloadSaved = false;
			const bool EvidenceSaved = [&]()
			{
				if (!IsInGameThread() || sizeof(float) != 4 || !PLATFORM_LITTLE_ENDIAN || Channels != 2
					|| EndFrame <= BeginFrame || EndSample > uint64(Graph.CapturedWet.Num())
					|| ByteCount == 0 || ByteCount > uint64(MAX_int32)) return false;
				Status = TEXT("INVALID_RUN_CASE_OR_STOP_BINDING");
				FGuid ParsedRun;
				FString ReportCase;
				bool ReportFrozen = false;
				double ReportDevice = 0.0, ReportRate = 0.0, ReportEnd = 0.0, ReportStopBegin = 0.0, ReportStopEnd = 0.0;
				double ReportWorldBegin = 0.0, ReportPoolWorld = 0.0;
				if (!FGuid::ParseExact(State->RunId, EGuidFormats::Digits, ParsedRun) || !ParsedRun.IsValid()
					|| State->RunId != ParsedRun.ToString(EGuidFormats::Digits).ToLower()
					|| State->CaseIndex < 0 || State->CaseIndex >= int32(UE_ARRAY_COUNT(IMQ5Cases))
					|| !Graph.Device || !Graph.Pool || Graph.DeviceId != State->AudioDeviceId || Graph.Device->SampleRate != IMQ5Rate
					|| Graph.Pool->WorldGeneration == 0 || Graph.Pool->WorldGeneration != ExpectedWorldGeneration
					|| Graph.TestCapturePreparationId == 0 || Graph.TestCapturePreparationId != State->CapturePreparationId
					|| !State->GraphStop.Frozen || !FMath::IsFinite(State->GraphStop.BeginSeconds) || !FMath::IsFinite(State->GraphStop.EndSeconds)
					|| State->GraphStop.BeginSeconds <= 0.0 || State->GraphStop.EndSeconds < State->GraphStop.BeginSeconds
					|| !State->CurrentReport.IsValid()
					|| !State->CurrentReport->TryGetStringField(TEXT("case"), ReportCase) || ReportCase != IMQ5Cases[State->CaseIndex].Name
					|| !State->CurrentReport->TryGetBoolField(TEXT("graph_capture_stop_snapshot_frozen"), ReportFrozen) || !ReportFrozen
					|| !State->CurrentReport->TryGetNumberField(TEXT("device_id"), ReportDevice) || ReportDevice != Graph.DeviceId
					|| !State->CurrentReport->TryGetNumberField(TEXT("world_generation_begin"), ReportWorldBegin) || ReportWorldBegin != double(Graph.Pool->WorldGeneration)
					|| !State->CurrentReport->TryGetNumberField(TEXT("graph_context_pool_world_generation"), ReportPoolWorld) || ReportPoolWorld != double(Graph.Pool->WorldGeneration)
					|| !State->CurrentReport->TryGetNumberField(TEXT("capture_rate_hz"), ReportRate) || ReportRate != IMQ5Rate
					|| !State->CurrentReport->TryGetNumberField(TEXT("environment_pcm_end_frame"), ReportEnd) || ReportEnd != EndFrame
					|| !State->CurrentReport->TryGetNumberField(TEXT("graph_stop_snapshot_before_wall_seconds"), ReportStopBegin) || ReportStopBegin != State->GraphStop.BeginSeconds
					|| !State->CurrentReport->TryGetNumberField(TEXT("graph_stop_snapshot_after_wall_seconds"), ReportStopEnd) || ReportStopEnd != State->GraphStop.EndSeconds)
					return false;
				auto CanonicalPath = [](FString Path)
				{
					Path = FPaths::ConvertRelativePathToFull(Path);
					FPaths::NormalizeFilename(Path);
					FPaths::CollapseRelativeDirectories(Path);
					return Path;
				};
				const FString ExpectedRun = CanonicalPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/Q5RoomGraph"), State->RunId));
				const FString ExpectedCase = CanonicalPath(FPaths::Combine(ExpectedRun, ReportCase));
				const FString OutputCase = CanonicalPath(CaseDirectory());
				const FString PcmFile = CanonicalPath(CaseFile(TEXT("graph-wet-frozen-length-failure.f32le.bin")));
				const FString MetadataFile = CanonicalPath(CaseFile(TEXT("graph-wet-frozen-length-failure.json")));
				Status = TEXT("INVALID_OR_ESCAPING_OUTPUT_PATH");
				if (!CanonicalPath(State->EvidenceDirectory).Equals(ExpectedRun, ESearchCase::IgnoreCase)
					|| !OutputCase.Equals(ExpectedCase, ESearchCase::IgnoreCase)
					|| !FPaths::IsUnderDirectory(PcmFile, ExpectedCase) || !FPaths::IsUnderDirectory(MetadataFile, ExpectedCase)
					|| !IFileManager::Get().DirectoryExists(*ExpectedCase)) return false;
				Status = TEXT("OUTPUT_OCCUPIED_NO_OVERWRITE");
				if (IFileManager::Get().FileExists(*PcmFile) || IFileManager::Get().DirectoryExists(*PcmFile)
					|| IFileManager::Get().FileExists(*MetadataFile) || IFileManager::Get().DirectoryExists(*MetadataFile)) return false;
				// Exclusive-create both files; a racing/partial output is retained
				// as invalid evidence, never overwritten or promoted to a PASS.
				const auto SaveExclusive = [](const FString& File, const void* Bytes, int64 Size)
				{
					TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*File, FILEWRITE_NoReplaceExisting));
					if (!Writer) return false;
					Writer->Serialize(const_cast<void*>(Bytes), Size);
					const bool Closed = Writer->Close();
					return Closed && !Writer->IsError() && IFileManager::Get().FileSize(*File) == Size;
				};
				const float* FrozenSamples = Graph.CapturedWet.GetData() + BeginSample;
				const FString FrozenSha1 = FSHA1::HashBuffer(FrozenSamples, ByteCount).ToString().ToLower();
				PayloadSaved = SaveExclusive(PcmFile, FrozenSamples, int64(ByteCount));
				TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
				Metadata->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5FrozenLengthFailurePCM.v1"));
				Metadata->SetStringField(TEXT("run_id"), State->RunId);
				Metadata->SetStringField(TEXT("case"), ReportCase);
				Metadata->SetNumberField(TEXT("device_id"), Graph.DeviceId);
				Metadata->SetNumberField(TEXT("sample_rate"), IMQ5Rate);
				Metadata->SetStringField(TEXT("world_generation"), FString::Printf(TEXT("%llu"), Graph.Pool->WorldGeneration));
				Metadata->SetStringField(TEXT("preparation_id"), FString::Printf(TEXT("%llu"), Graph.TestCapturePreparationId));
				Metadata->SetNumberField(TEXT("channels"), Channels);
				Metadata->SetStringField(TEXT("format"), TEXT("stereo-f32le"));
				Metadata->SetNumberField(TEXT("begin_frame"), BeginFrame);
				Metadata->SetNumberField(TEXT("end_frame_exclusive"), EndFrame);
				Metadata->SetNumberField(TEXT("minimum_original_frames"), MinimumGraphWetFrames);
				Metadata->SetStringField(TEXT("payload_bytes"), FString::Printf(TEXT("%llu"), ByteCount));
				Metadata->SetStringField(TEXT("payload_file"), PcmFile);
				Metadata->SetStringField(TEXT("payload_sha1_expected_from_frozen_memory"), FrozenSha1);
				Metadata->SetBoolField(TEXT("payload_saved"), PayloadSaved);
				Metadata->SetBoolField(TEXT("original_stop_frozen"), true);
				Metadata->SetNumberField(TEXT("original_stop_begin_s"), State->GraphStop.BeginSeconds);
				Metadata->SetNumberField(TEXT("original_stop_end_s"), State->GraphStop.EndSeconds);
				Metadata->SetNumberField(TEXT("original_bus_stop_rows"), State->GraphStop.BusTraceEnd);
				Metadata->SetStringField(TEXT("scope"), TEXT("Existing original frozen graph prefix only; evidence validity is independent of the unchanged native length failure. No recapture, added frames or post-Stop samples."));
				FString JsonText;
				const auto JsonWriter = TJsonWriterFactory<>::Create(&JsonText);
				Status = TEXT("METADATA_SERIALIZATION_OR_SAVE_FAILED");
				if (!FJsonSerializer::Serialize(Metadata, JsonWriter)) return false;
				JsonText += TEXT("\n");
				const FTCHARToUTF8 Utf8(*JsonText);
				if (!SaveExclusive(MetadataFile, Utf8.Get(), Utf8.Length())) return false;
				Status = PayloadSaved ? TEXT("SAVED_FROZEN_PCM_AND_BINDING") : TEXT("PAYLOAD_SAVE_FAILED_METADATA_RETAINED");
				State->CurrentReport->SetStringField(TEXT("length_failure_pcm_metadata_file"), MetadataFile);
				State->CurrentReport->SetStringField(TEXT("length_failure_pcm_file"), PcmFile);
				return PayloadSaved;
			}();
			if (State->CurrentReport.IsValid())
			{
				State->CurrentReport->SetBoolField(TEXT("length_failure_pcm_opt_in"), true);
				State->CurrentReport->SetBoolField(TEXT("length_failure_pcm_saved"), EvidenceSaved);
				State->CurrentReport->SetBoolField(TEXT("length_failure_pcm_payload_saved"), PayloadSaved);
				State->CurrentReport->SetStringField(TEXT("length_failure_pcm_status"), Status);
			}
		}
		if (WetFrames < MinimumGraphWetFrames
			|| !IMQ5WriteFloatWindow(CaseFile(TEXT("graph-wet-stereo-f32le.bin")), State->MetaContext->CapturedWet,
				State->EnvironmentPcmBegin, WetFramesPublished, 2))
		{
			Error = FString::Printf(TEXT("The post-trigger wet environment graph published %u frames; at least %u frames (%.3fs, allowing the declared 0.5s trigger +/- 0.2s) are required."),
				WetFrames, MinimumGraphWetFrames, double(MinimumGraphWetFrames) / IMQ5Rate);
			return false;
		}
		if (!IMQ5AnalyzeStereoFloat(
			State->MetaContext->CapturedWet.GetData() + uint64(State->EnvironmentPcmBegin) * 2u,
			int32(WetFrames), IMQ5FloatSilenceEnergy,
			CaseFile(TEXT("graph-wet-energy-10ms.csv")), GraphWetDecay, false))
		{
			Error = TEXT("Graph-internal wet PCM decay is unusable: ") + GraphWetDecay.Error;
			return false;
		}
		if (FreshEnvironmentRows == 0 || State->MetaContext->LastIRSequence.load(std::memory_order_acquire) == 0)
		{
			Error = TEXT("The wet tail did not include a fresh production environment-graph IR sequence.");
			return false;
		}

		if (CallbackIds.Num() != 1 || !CallbackIds.Contains(ExpectedAudioId) || ExpectedAudioId == 0
			|| AcceptedMatchingRows == 0 || AcceptedImpulseRows == 0 || RejectRows != 0 || RouteContaminationRows != 0
			|| SourceBlockInputEnergy <= 0.0 || FirstConsumedSeconds == DBL_MAX || FirstAcceptedImpulseSeconds == DBL_MAX)
		{
			Error = TEXT("Graph trace did not prove one ordinary Q5 source identity, accepted world/voice result, and reverb-only routes.");
			return false;
		}
		// The 0.5 s contract controls when the ordinary source is triggered.
		// The first graph callback additionally includes audio-device startup,
		// context registration, and the two-block spatial input delay.
		OutTriggerDelay = State->TriggerRequestedSeconds - State->CaptureStartedSeconds;
		if (FMath::Abs(OutTriggerDelay - IMQ5ExpectedTriggerSeconds) > IMQ5TriggerToleranceSeconds)
		{
			Error = FString::Printf(TEXT("Observed ordinary source trigger delay %.6fs is outside the declared 0.5s +/- 0.2s window."), OutTriggerDelay);
			return false;
		}
		const double FirstGraphCallbackDelay = FirstConsumedSeconds - State->TriggerRequestedSeconds;
		if (FirstGraphCallbackDelay < 0.0 || FirstGraphCallbackDelay > 0.35)
		{
			Error = FString::Printf(TEXT("First accepted graph callback lagged the ordinary source trigger by %.6fs, outside the declared 0.35s startup window."), FirstGraphCallbackDelay);
			return false;
		}

		State->CurrentReport->SetStringField(TEXT("audio_component_id"), LexToString(ExpectedAudioId));
		State->CurrentReport->SetNumberField(TEXT("source_pcm_begin_frame"), State->SourcePcmBegin);
		State->CurrentReport->SetNumberField(TEXT("source_pcm_end_frame"), SourceFrames);
		State->CurrentReport->SetNumberField(TEXT("source_pcm_energy"), SourcePcmEnergy);
		State->CurrentReport->SetNumberField(TEXT("source_trace_begin_block"), State->SourceTraceBegin);
		State->CurrentReport->SetNumberField(TEXT("source_trace_end_block"), End);
		State->CurrentReport->SetNumberField(TEXT("source_trace_rows"), End - State->SourceTraceBegin);
		State->CurrentReport->SetNumberField(TEXT("source_trace_accepted_matching_rows"), AcceptedMatchingRows);
		State->CurrentReport->SetNumberField(TEXT("source_trace_accepted_impulse_rows"), AcceptedImpulseRows);
		State->CurrentReport->SetNumberField(TEXT("source_trace_nonaccepted_rows"), RejectRows);
		State->CurrentReport->SetNumberField(TEXT("source_trace_expected_priming_rows"), PrimingRows);
		State->CurrentReport->SetNumberField(TEXT("source_trace_silent_startup_rows"), SilentStartupRows);
		State->CurrentReport->SetNumberField(TEXT("source_trace_direct_path_route_rows"), RouteContaminationRows);
		State->CurrentReport->SetNumberField(TEXT("source_trace_input_energy"), SourceBlockInputEnergy);
		State->CurrentReport->SetNumberField(TEXT("environment_trace_begin_block"), State->EnvironmentTraceBegin);
		State->CurrentReport->SetNumberField(TEXT("environment_trace_end_block"), EnvironmentTraceEnd);
		State->CurrentReport->SetNumberField(TEXT("environment_trace_rows"), EnvironmentTraceEnd - State->EnvironmentTraceBegin);
		State->CurrentReport->SetNumberField(TEXT("environment_trace_fresh_rows"), FreshEnvironmentRows);
		State->CurrentReport->SetNumberField(TEXT("observed_ordinary_trigger_delay_s"), OutTriggerDelay);
		State->CurrentReport->SetNumberField(TEXT("first_accepted_graph_callback_delay_after_trigger_s"), FirstGraphCallbackDelay);
		State->CurrentReport->SetNumberField(TEXT("first_accepted_impulse_delay_after_trigger_s"), FirstAcceptedImpulseSeconds - State->TriggerRequestedSeconds);
		State->CurrentReport->SetNumberField(TEXT("world_generation"), ExpectedWorldGeneration);
		State->CurrentReport->SetNumberField(TEXT("environment_wet_pcm_begin_frame"), State->EnvironmentPcmBegin);
		State->CurrentReport->SetNumberField(TEXT("environment_wet_pcm_end_frame"), WetFramesPublished);
		State->CurrentReport->SetNumberField(TEXT("environment_wet_pcm_frames"), WetFrames);
		State->CurrentReport->SetNumberField(TEXT("graph_wet_first_nonzero_seconds_from_context_start"),
			double(GraphWetDecay.FirstNonzeroFrame) / IMQ5Rate);
		State->CurrentReport->SetStringField(TEXT("graph_wet_onset_alignment"), TEXT("First nonzero graph wet PCM is measured relative to captured context frame zero; context creation follows source registration. Device WAV remains aligned to recorder start and independently verifies pre-trigger silence and the approximately 0.5s one-shot delay."));
		State->CurrentReport->SetObjectField(TEXT("graph_wet_decay"), IMQ5DecayJson(GraphWetDecay));
		return true;
	}

	bool CompleteOrderOnlyMetadata(UWorld* PIE)
	{
		if (!PIE || !State->MetaContext.IsValid() || !State->Bridge.IsValid() || !State->CurrentReport.IsValid()
			|| !State->bCaptureTransportOrder || State->CaseIndex != 0 || !State->GraphStop.Frozen)
			return Fail(TEXT("Order-only observation lacks its original closed-low context or frozen Stop."));
		const auto& Graph = *State->MetaContext;
		double RecordedWorld = 0.0;
		if (!Graph.Device || !Graph.Pool || Graph.DeviceId != State->AudioDeviceId || Graph.Device->SampleRate != IMQ5Rate
			|| Graph.Pool->WorldGeneration == 0 || Graph.TestCapturePreparationId == 0
			|| Graph.TestCapturePreparationId != State->CapturePreparationId
			|| !State->CurrentReport->TryGetNumberField(TEXT("graph_context_pool_world_generation"), RecordedWorld)
			|| RecordedWorld != double(Graph.Pool->WorldGeneration)
			|| State->GraphStop.BusUnderruns < State->BusUnderrunsBegin)
			return Fail(TEXT("Order-only device/rate/world/preparation or frozen counter identity is incomplete."));

		// CaptureRoomWet already stopped recording and froze the original counters before source Stop.
		// These writers read metadata only. Never wait for a ready hole or enter audio fit/export helpers.
		WriteTransportOrderTrace();
		FString BusError;
		if (!WriteBusReadTrace(BusError)) return Fail(BusError);
		const FIMQ5GraphCaptureStop& Cut = State->GraphStop;
		const FIMQ5DeviceCounters& Counters = Cut.CounterDelta;
		IMQ5SetOrderOnlyScope(*State->CurrentReport, false);
		State->CurrentReport->SetBoolField(TEXT("observation_metadata_complete"), State->bTransportOrderValid);
		State->CurrentReport->SetNumberField(TEXT("observation_stage_at_metadata_export"), Stage);
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_delta"), Cut.BusUnderruns - State->BusUnderrunsBegin);
		State->CurrentReport->SetNumberField(TEXT("bus_push_blocks_at_stop"), Cut.PushBlocks);
		State->CurrentReport->SetNumberField(TEXT("bus_push_failures_at_stop"), Cut.PushFailures);
		State->CurrentReport->SetBoolField(TEXT("bus_transport_failed_at_stop"), Cut.TransportFailed);
		State->CurrentReport->SetNumberField(TEXT("direct_nonzero_blocks"), Counters.Direct);
		State->CurrentReport->SetNumberField(TEXT("path_nonzero_blocks"), Counters.Path);
		State->CurrentReport->SetNumberField(TEXT("degraded_source_blocks"), Counters.Rejected);
		State->CurrentReport->SetNumberField(TEXT("reverb_rejected_blocks"), Counters.ReverbRejected);
		State->CurrentReport->SetNumberField(TEXT("dry_dropped_blocks"), Counters.DryDropped);
		State->CurrentReport->SetNumberField(TEXT("wet_nonzero_blocks"), Counters.WetNonzero);
		State->CurrentReport->SetNumberField(TEXT("reverb_processed_blocks"), Counters.ReverbProcessed);
		State->CurrentReport->SetNumberField(TEXT("reverb_dry_blocks"), Counters.ReverbDry);
		State->CurrentReport->SetStringField(TEXT("observation_audio_analysis_status"), TEXT("NOT_ATTEMPTED"));
		if (!SaveCurrentReport()) return Fail(TEXT("Could not save order-only metadata."));
		if (!State->bTransportOrderValid)
			return Fail(TEXT("Order-only metadata is INCONCLUSIVE: prefix, stage pairs, identity, overflow/hole or CPU fields rejected."));
		State->bOrderOnlyMetadataComplete = true;
		// The unchanged Stage7 budget/fence/end-PIE prefix supplies the normal drain.
		SetStage(7);
		return false;
	}

	bool WaitForWavAndAnalyze(UWorld* PIE, double Now)
	{
		if (State->bOrderOnlyClosedLow) return CompleteOrderOnlyMetadata(PIE);
		if (!PIE || !State->MetaContext.IsValid() || !State->Bridge.IsValid())
		{
			return Fail(TEXT("Graph/device context disappeared before WAV export completed."));
		}
		const FString WavFile = CaseFile(TEXT("wet.wav"));
		const int64 Bytes = IFileManager::Get().FileSize(*WavFile);
		if (Bytes <= 44 || Bytes != State->LastWavBytes)
		{
			State->LastWavBytes = Bytes;
			if (Now - StageStarted > 30.0) return Fail(TEXT("The six-second master-output WAV export did not settle."));
			return false;
		}

		WriteTransportOrderTrace();
		FString BusTraceError;
		if (!WriteBusReadTrace(BusTraceError)) return Fail(BusTraceError);
		FIMQ5Decay DeviceDecay;
		if (!IMQ5AnalyzeDeviceWave(WavFile, CaseFile(TEXT("device-wet-energy-10ms.csv")), DeviceDecay))
		{
			State->CurrentReport->SetObjectField(TEXT("device_wav_decay"), IMQ5DecayJson(DeviceDecay));
			return Fail(TEXT("Device WAV wet decay is unusable: ") + DeviceDecay.Error);
		}
		FIMQ5Decay GraphWetDecay;
		FString TraceError;
		double TriggerDelay = 0.0;
		const uint32 SourceEnd = State->GraphStop.SourceTraceEnd;
		if (!WriteSourceTraceAndValidate(SourceEnd, GraphWetDecay, TraceError, TriggerDelay))
		{
			State->CurrentReport->SetObjectField(TEXT("device_wav_decay"), IMQ5DecayJson(DeviceDecay));
			return Fail(TraceError);
		}

		const FIMQ5DeviceCounters D = State->GraphStop.CounterDelta;
		const uint64 BusUnderruns = State->GraphStop.BusUnderruns - State->BusUnderrunsBegin;
		const double ExpectedBlocks = double(State->GraphWetFramesRecorded)
			/ FMath::Max(1u, State->Bridge->BlockFrames);
		const bool bCounterPass = D.Direct == 0 && D.Path == 0 && D.Rejected == 0 && D.ReverbRejected == 0
			&& D.DryDropped == 0 && D.WetNonzero > 0
			&& D.ReverbProcessed >= ExpectedBlocks * 0.90 && D.ReverbDry > 0
			&& BusUnderruns == 0 && State->GraphStop.PushFailures == 0 && !State->GraphStop.TransportFailed;
		State->CurrentReport->SetNumberField(TEXT("device_wav_t20_s"), DeviceDecay.T20Seconds);
		State->CurrentReport->SetObjectField(TEXT("device_wav_decay"), IMQ5DecayJson(DeviceDecay));
		State->CurrentReport->SetNumberField(TEXT("ordinary_source_trigger_delay_s"), TriggerDelay);
		State->CurrentReport->SetNumberField(TEXT("direct_nonzero_blocks"), D.Direct);
		State->CurrentReport->SetNumberField(TEXT("path_nonzero_blocks"), D.Path);
		State->CurrentReport->SetNumberField(TEXT("degraded_source_blocks"), D.Rejected);
		State->CurrentReport->SetNumberField(TEXT("reverb_rejected_blocks"), D.ReverbRejected);
		State->CurrentReport->SetNumberField(TEXT("dry_dropped_blocks"), D.DryDropped);
		State->CurrentReport->SetNumberField(TEXT("wet_nonzero_blocks"), D.WetNonzero);
		State->CurrentReport->SetNumberField(TEXT("reverb_processed_blocks"), D.ReverbProcessed);
		State->CurrentReport->SetNumberField(TEXT("reverb_dry_blocks"), D.ReverbDry);
		State->CurrentReport->SetNumberField(TEXT("expected_graph_environment_blocks"), ExpectedBlocks);
		State->CurrentReport->SetNumberField(TEXT("device_block_frames"), State->Bridge->BlockFrames);
		State->CurrentReport->SetNumberField(TEXT("bus_underruns_delta"), BusUnderruns);
		State->CurrentReport->SetBoolField(TEXT("direct_path_wet_isolation_pass"), bCounterPass);
		State->CurrentReport->SetBoolField(TEXT("source_graph_identity_and_wet_pcm_pass"), true);
		State->CurrentReport->SetBoolField(TEXT("device_and_graph_wet_fits_pass"), DeviceDecay.bUsable && GraphWetDecay.bUsable);
		State->CurrentReport->SetBoolField(TEXT("passed"), bCounterPass && DeviceDecay.bUsable && GraphWetDecay.bUsable);
		State->CurrentReport->SetBoolField(TEXT("original_native_case_pass"), bCounterPass && DeviceDecay.bUsable && GraphWetDecay.bUsable);
		State->CurrentReport->SetStringField(TEXT("wav_file"), WavFile);
		State->CurrentReport->SetStringField(TEXT("graph_source_pcm_file"), CaseFile(TEXT("graph-source-mono-f32le.bin")));
		State->CurrentReport->SetStringField(TEXT("graph_wet_pcm_file"), CaseFile(TEXT("graph-wet-stereo-f32le.bin")));
		State->CurrentReport->SetStringField(TEXT("graph_source_block_trace_file"), CaseFile(TEXT("graph-source-blocks.csv")));
		State->CurrentReport->SetStringField(TEXT("graph_environment_block_trace_file"), CaseFile(TEXT("graph-environment-blocks.csv")));
		if (!SaveCurrentReport()) return Fail(TEXT("Could not persist complete Q5 graph/device wet evidence."));
		if (!bCounterPass) return Fail(TEXT("Q5 wet recording contains direct/path/degraded/drop contamination, bus underruns, or insufficient device processing."));
		if (State->bCaptureTransportOrder && !State->bTransportOrderValid)
			return Fail(TEXT("Private joint order observation is incomplete; original native gates remain recorded separately."));

		State->DeviceT20.Add(DeviceDecay.T20Seconds);
		State->GraphWetT20.Add(GraphWetDecay.T20Seconds);
		++State->CasesCompleted;
		State->CaseReports.Add(CaseFile(TEXT("case.json")));
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticQ5RoomGraph case=%s device_t20_s=%.9g graph_wet_t20_s=%.9g identity=%s evidence=%s"),
			IMQ5Cases[State->CaseIndex].Name, DeviceDecay.T20Seconds, GraphWetDecay.T20Seconds,
			State->Audio.IsValid() ? *LexToString(State->Audio->GetAudioComponentID()) : TEXT("DESTROYED"), *CaseDirectory());
		SetStage(7);
		return false;
	}

	bool FinishCase(double, UWorld* PIE)
	{
		if (PIE)
		{
			if (!State->RecorderFence.IsFenceComplete()) return false;
			if (GUnrealEd) GUnrealEd->RequestEndPlayMap();
			return false;
		}
		if (State->bOrderOnlyClosedLow)
		{
			if (!State->bOrderOnlyMetadataComplete)
				return Fail(TEXT("Order-only case reached drain without complete metadata."));
			BeginFinish(false, TEXT("Closed-low order-only metadata completed; original Q5 product gates were not evaluated."));
			return false;
		}
		if (State->CaseIndex + 1 < UE_ARRAY_COUNT(IMQ5Cases))
		{
			State->ReleaseCapturePreparation();
			++State->CaseIndex;
			State->Volume.Reset();
			State->Listener.Reset();
			State->SourceActor.Reset();
			State->Audio.Reset();
			State->SourceMarker.Reset();
			State->MetaContext.Reset();
			State->Bridge.Reset();
			State->MasterRecorder.Reset();
			State->MasterStartRequestFrame = 0;
			State->MasterTriggerRenderFrame = 0;
			State->MasterStopBeforeFrame = 0;
			State->MasterStopAfterFrame = 0;
			State->bSourceSpawned = false;
			State->bPreTriggerContextAbsent = false;
			State->bGraphCaptureWindowInitialized = false;
			State->AudioDeviceId = 0;
			State->CapturePreparationStartedSeconds = 0.0;
			State->CapturePreparationEndedSeconds = 0.0;
			State->CaptureStartedSeconds = 0.0;
			State->TriggerRequestedSeconds = 0.0;
			State->GraphContextObservedSeconds = 0.0;
			State->EnvironmentPcmBegin = 0;
			State->EnvironmentTraceBegin = 0;
			State->SourcePcmBegin = 0;
			State->SourceTraceBegin = 0;
			State->EnvironmentFramesAtFirstObservation = 0;
			State->SourceFramesAtFirstObservation = 0;
			State->EnvironmentTraceRowsAtFirstObservation = 0;
			State->SourceTraceRowsAtFirstObservation = 0;
			State->GraphWetFramesRecorded = 0;
			State->WarmEnvironmentBlocks = 0;
			State->WarmReverbDryBlocks = 0;
			State->WarmReverbProcessedBlocks = 0;
			State->WarmReverbWetBlocks = 0;
			State->WarmBusReadBlocks = 0;
			State->BusUnderrunsBegin = 0;
			State->CountersBegin = {};
			State->GraphStop = {};
			State->LastWavBytes = -1;
			SetStage(0);
			return false;
		}

		const double DeviceGap = (State->DeviceT20[0] - State->DeviceT20[1]) / State->DeviceT20[1];
		const double GraphGap = (State->GraphWetT20[0] - State->GraphWetT20[1]) / State->GraphWetT20[1];
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5RoomGraphSummary.v1"));
		Summary->SetStringField(TEXT("source_map_before"), State->OriginalMapPath);
		Summary->SetStringField(TEXT("source_map_sha1_before"), State->OriginalMapSha1Before);
		Summary->SetNumberField(TEXT("cases_completed"), State->CasesCompleted);
		Summary->SetNumberField(TEXT("closed_low_device_t20_s"), State->DeviceT20[0]);
		Summary->SetNumberField(TEXT("closed_high_device_t20_s"), State->DeviceT20[1]);
		Summary->SetNumberField(TEXT("closed_device_relative_gap"), DeviceGap);
		Summary->SetNumberField(TEXT("closed_low_graph_wet_t20_s"), State->GraphWetT20[0]);
		Summary->SetNumberField(TEXT("closed_high_graph_wet_t20_s"), State->GraphWetT20[1]);
		Summary->SetNumberField(TEXT("closed_graph_wet_relative_gap"), GraphGap);
		Summary->SetNumberField(TEXT("required_relative_gap"), IMQ5ClosedRelativeGap);
		Summary->SetBoolField(TEXT("closed_geometry_same_except_absorption"), State->ClosedGeometryIdentity.Len() > 0);
		Summary->SetBoolField(TEXT("all_four_scenes_have_device_and_graph_wet_fits"), State->CasesCompleted == UE_ARRAY_COUNT(IMQ5Cases));
		Summary->SetBoolField(TEXT("passed"), DeviceGap >= IMQ5ClosedRelativeGap && GraphGap >= IMQ5ClosedRelativeGap
			&& State->CasesCompleted == UE_ARRAY_COUNT(IMQ5Cases));
		if (!IMQ5WriteJson(FPaths::Combine(State->EvidenceDirectory, TEXT("summary.json")), Summary))
		{
			return Fail(TEXT("Could not save the four-room T20 comparison summary."));
		}
		if (DeviceGap < IMQ5ClosedRelativeGap || GraphGap < IMQ5ClosedRelativeGap)
		{
			return Fail(TEXT("closed-low T20 must exceed closed-high by at least 10% in both device WAV and graph-internal wet PCM."));
		}
		BeginFinish(true, TEXT("Four ordinary MetaSound Q5 wet recordings and both closed-room T20 gaps passed."));
		return false;
	}

	bool WaitForPIEExit(double Now)
	{
		if (IMQ5FindPIEWorld())
		{
			if (State->bOrderOnlyClosedLow && State->RecorderFence.IsFenceComplete() && GUnrealEd)
				GUnrealEd->RequestEndPlayMap();
			if (Now - StageStarted > 30.0)
			{
				State->Failure = State->Failure.IsEmpty() ? TEXT("PIE did not stop within 30 seconds after RequestEndPlayMap.") : State->Failure;
				SetStage(90);
				return false;
			}
			return false;
		}
		SetStage(90);
		return false;
	}

	bool RestoreOriginalMap()
	{
		if (IMQ5FindPIEWorld() || State->OriginalMapPath.IsEmpty()) return false;
		UWorld* Current = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const bool bLoaded = Current && Current->GetOutermost()->GetName() == State->OriginalMapPath
			? true : FEditorFileUtils::LoadMap(State->OriginalMapPath, false, true);
		UWorld* Restored = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		State->bOriginalMapRestored = bLoaded && Restored && Restored->GetOutermost()->GetName() == State->OriginalMapPath
			&& Restored->GetPackage() && Restored->GetPackage()->IsDirty() == State->bMapDirtyBefore
			&& IMQ5HashFile(State->OriginalMapDiskPath, State->OriginalMapSha1After)
			&& State->OriginalMapSha1After == State->OriginalMapSha1Before;
		return State->bOriginalMapRestored;
	}

	bool CleanupGeneratedPackages()
	{
		bool bAllRemoved = true;
		for (FIMQ5GeneratedPackage& Generated : State->GeneratedPackages)
		{
			FString CurrentHash;
			const bool bHashUnchanged = IMQ5HashFile(Generated.DiskPath, CurrentHash)
				&& !Generated.Sha1BeforeCleanup.IsEmpty() && CurrentHash == Generated.Sha1BeforeCleanup;
			Generated.Sha1AfterRestore = CurrentHash;
			if (!bHashUnchanged)
			{
				bAllRemoved = false;
				continue;
			}

			if (UPackage* Package = FindPackage(nullptr, *Generated.PackageName))
			{
				UObject* Asset = FindObject<UObject>(Package, *FPackageName::GetShortName(Generated.PackageName));
				if (Asset)
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					Asset->ClearFlags(RF_Public | RF_Standalone);
					Asset->MarkAsGarbage();
				}
				Package->SetDirtyFlag(false);
				Package->ClearFlags(RF_Public | RF_Standalone);
				Package->MarkAsGarbage();
			}
			Generated.bRemoved = IMQ5DeletePackageFiles(Generated.DiskPath);
			bAllRemoved = Generated.bRemoved && bAllRemoved;
		}
		CollectGarbage(RF_NoFlags);
		return bAllRemoved;
	}

	bool VerifyTestAssetsUnchanged()
	{
		bool bAll = true;
		for (FIMQ5SavedAsset& Asset : State->TestAssets)
		{
			Asset.bDirtyAfter = Asset.Package && Asset.Package->IsDirty();
			const bool bHash = IMQ5HashFile(Asset.DiskPath, Asset.Sha1After);
			bAll = bHash && Asset.Sha1After == Asset.Sha1Before && Asset.bDirtyBefore == Asset.bDirtyAfter && bAll;
		}
		State->bAssetsRestored = bAll;
		return bAll;
	}

	bool CleanupAndFinish(double)
	{
		if (!State->bRestoreAttempted)
		{
			State->bRestoreAttempted = true;
			if (!RestoreOriginalMap() && State->Failure.IsEmpty()) State->Failure = TEXT("Original editor map identity/bytes/dirty state could not be restored.");
			if (State->bOriginalMapRestored)
			{
				State->bGeneratedContentRemoved = CleanupGeneratedPackages();
				if (!State->bGeneratedContentRemoved && State->Failure.IsEmpty()) State->Failure = TEXT("Generated Q5 fixture map or bake package changed before cleanup or could not be removed.");
			}
			if (!VerifyTestAssetsUnchanged() && State->Failure.IsEmpty()) State->Failure = TEXT("A pinned Q5 source, impulse-wave, or environment package changed during recording.");
			State->RestoreGlobals();
			if (State->bOrderOnlyClosedLow)
			{
				State->bOrderOnlyObservationComplete = State->bOrderOnlyMetadataComplete && State->Failure.IsEmpty()
					&& State->bOriginalMapRestored && State->bAssetsRestored && State->bGeneratedContentRemoved;
				State->bFinalPass = false;
				if (!State->bOrderOnlyObservationComplete && State->Failure.IsEmpty())
					State->Failure = TEXT("Order-only metadata or cleanup/restore is incomplete.");
				if (State->CurrentReport.IsValid())
				{
					IMQ5SetOrderOnlyScope(*State->CurrentReport, State->bOrderOnlyObservationComplete);
					if (!SaveCurrentReport())
					{
						State->bOrderOnlyObservationComplete = false;
						if (State->Failure.IsEmpty()) State->Failure = TEXT("Could not save final order-only case scope.");
					}
				}
			}
			else
			{
				State->bFinalPass = State->bFinalPass && State->Failure.IsEmpty()
					&& State->bOriginalMapRestored && State->bAssetsRestored && State->bGeneratedContentRemoved;
				if (!State->bFinalPass && State->FinalMessage.IsEmpty()) State->FinalMessage = State->Failure.IsEmpty()
					? TEXT("Q5 room graph test failed its cleanup/restore gate.") : State->Failure;
				if (!State->bFinalPass && State->Failure.IsEmpty()) State->Failure = State->FinalMessage;
			}
		}

		TSharedRef<FJsonObject> Terminal = MakeShared<FJsonObject>();
		Terminal->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5RoomGraphTerminal.v1"));
		Terminal->SetStringField(TEXT("run_id"), State->RunId);
		Terminal->SetStringField(TEXT("evidence_directory"), State->EvidenceDirectory);
		Terminal->SetBoolField(TEXT("passed"), State->bFinalPass);
		Terminal->SetStringField(TEXT("message"), State->FinalMessage);
		Terminal->SetStringField(TEXT("failure"), State->Failure);
		Terminal->SetNumberField(TEXT("cases_completed"), State->CasesCompleted);
		Terminal->SetStringField(TEXT("original_map_path"), State->OriginalMapPath);
		Terminal->SetStringField(TEXT("original_map_sha1_before"), State->OriginalMapSha1Before);
		Terminal->SetStringField(TEXT("original_map_sha1_after"), State->OriginalMapSha1After);
		Terminal->SetBoolField(TEXT("original_map_restored"), State->bOriginalMapRestored);
		Terminal->SetBoolField(TEXT("pinned_assets_unchanged"), State->bAssetsRestored);
		Terminal->SetBoolField(TEXT("generated_packages_removed"), State->bGeneratedContentRemoved);
		TArray<TSharedPtr<FJsonValue>> PackageRows;
		for (const FIMQ5GeneratedPackage& Package : State->GeneratedPackages)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package_name"), Package.PackageName);
			Row->SetStringField(TEXT("disk_path"), Package.DiskPath);
			Row->SetStringField(TEXT("sha1_before_cleanup"), Package.Sha1BeforeCleanup);
			Row->SetStringField(TEXT("sha1_after_restore"), Package.Sha1AfterRestore);
			Row->SetBoolField(TEXT("is_map"), Package.bIsMap);
			Row->SetBoolField(TEXT("removed"), Package.bRemoved);
			PackageRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Terminal->SetArrayField(TEXT("generated_packages"), PackageRows);
		TArray<TSharedPtr<FJsonValue>> AssetRows;
		for (const FIMQ5SavedAsset& Asset : State->TestAssets)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("object_path"), Asset.ObjectPath);
			Row->SetStringField(TEXT("sha1_before"), Asset.Sha1Before);
			Row->SetStringField(TEXT("sha1_after"), Asset.Sha1After);
			Row->SetStringField(TEXT("expected_sha1"), Asset.ExpectedSha1);
			Row->SetBoolField(TEXT("package_dirty_before"), Asset.bDirtyBefore);
			Row->SetBoolField(TEXT("package_dirty_after"), Asset.bDirtyAfter);
			AssetRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Terminal->SetArrayField(TEXT("pinned_assets"), AssetRows);
		if (State->bOrderOnlyClosedLow)
		{
			Terminal->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5OrderOnlyTerminal.v1"));
			IMQ5SetOrderOnlyScope(*Terminal, State->bOrderOnlyObservationComplete);
			Terminal->SetStringField(TEXT("case"), IMQ5Cases[0].Name);
			Terminal->SetBoolField(TEXT("observation_metadata_complete"), State->bOrderOnlyMetadataComplete);
			Terminal->SetNumberField(TEXT("observation_cases_completed"), State->bOrderOnlyObservationComplete ? 1 : 0);
		}
		State->bTerminalWritten = IMQ5WriteJson(FPaths::Combine(State->EvidenceDirectory, TEXT("terminal.json")), Terminal);
		if (!State->bTerminalWritten)
		{
			State->bFinalPass = false;
			if (State->bOrderOnlyClosedLow) State->bOrderOnlyObservationComplete = false;
			State->Failure = TEXT("Could not write fail-closed Q5 terminal evidence.");
		}
		if (State->bOrderOnlyClosedLow)
		{
			if (!State->bOrderOnlyObservationComplete && State->Test)
				State->Test->AddError(State->Failure.IsEmpty() ? State->FinalMessage : State->Failure);
			if (State->bOrderOnlyObservationComplete && State->Test) State->Test->AddInfo(State->FinalMessage);
			const TCHAR* Status = State->bOrderOnlyObservationComplete ? TEXT("OBSERVATION_COMPLETE") : TEXT("OBSERVATION_INCONCLUSIVE");
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticQ5OrderOnly %s message=%s evidence=%s"), Status, *State->FinalMessage, *State->EvidenceDirectory);
			// Shell completion is explicitly scoped to metadata/restore, never original Q5 product acceptance.
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticQ5OrderOnly %s observation_scope=closed-low-order-only original_product_status=NOT_EVALUATED"),
				State->bOrderOnlyObservationComplete ? TEXT("SCOPE_PASS") : TEXT("SCOPE_FAIL"));
			UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), Status);
			return true;
		}
		if (!State->bFinalPass && State->Test) State->Test->AddError(State->Failure.IsEmpty() ? State->FinalMessage : State->Failure);
		if (State->bFinalPass && State->Test) State->Test->AddInfo(State->FinalMessage);
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticQ5RoomGraph %s message=%s evidence=%s"),
			State->bFinalPass ? TEXT("PASS") : TEXT("FAIL"), *State->FinalMessage, *State->EvidenceDirectory);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticQ5RoomGraph %s"), State->bFinalPass ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), State->bFinalPass ? TEXT("PASS") : TEXT("FAIL"));
		return true;
	}

	TSharedRef<FIMQ5RunState> State;
	int32 Stage = 0;
	double StageStarted = 0.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	IM_AcousticQ5CapturePreparationOwnership,
	"IceMoon.AcousticField.Q5.CapturePreparationOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool IM_AcousticQ5CapturePreparationOwnership::RunTest(const FString&)
{
	check(IsInGameThread());
	constexpr uint32 DeviceA = 0xF1AC0001u, DeviceB = 0xF1AC0002u, DeviceC = 0xF1AC0003u;
	if (!TestFalse(TEXT("Synthetic device A is unowned"), IMAcousticMetaSound::FindAcousticMetaSoundContext(DeviceA).IsValid())
		|| !TestFalse(TEXT("Synthetic device B is unowned"), IMAcousticMetaSound::FindAcousticMetaSoundContext(DeviceB).IsValid())
		|| !TestFalse(TEXT("Synthetic device C is unowned"), IMAcousticMetaSound::FindAcousticMetaSoundContext(DeviceC).IsValid())) return false;
	struct FIMCapturePreparationCleanup
	{
		uint64 PendingId = 0;
		TArray<FIMAcousticMetaSoundContextPtr> Contexts;
		~FIMCapturePreparationCleanup()
		{
			IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(PendingId);
			for (const auto& Context : Contexts) IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
			IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		}
	} Cleanup;
	FString Error;
	uint64 Id = 0;
	// Trace-only controls exercise ownership cheaply. The final positive case
	// below retains Q5's actual 48kHz/200s PCM and default trace capacities.
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 64, 8, false, true);
	if (!TestTrue(TEXT("Prepare trace-only storage"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, Id, Error))) return false;
	Cleanup.PendingId = Id;
	TestTrue(TEXT("Preparation has a nonzero ID"), Id != 0);
	TestFalse(TEXT("Preparation does not create a context"), IMAcousticMetaSound::FindAcousticMetaSoundContext(DeviceA).IsValid());
	TestTrue(TEXT("Nonzero ID validates without preparing again"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, Id, Error));
	uint64 DuplicateId = 0;
	TestFalse(TEXT("Second zero-ID request cannot replace pending owner"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, DuplicateId, Error));
	TestEqual(TEXT("Failed prepare leaves caller ID zero"), DuplicateId, uint64(0));
	TestFalse(TEXT("Wrong device is rejected by validate"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceB, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, Id, Error));
	TestFalse(TEXT("Wrong rate is rejected by validate"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate + 512, Id, Error));
	TestFalse(TEXT("Wrong ID cannot cancel pending owner"), IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(Id + 1));
	TestTrue(TEXT("Pending owner survives failed identity checks"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, Id, Error));

	// An unrelated device keeps the existing unprepared fixture path and
	// cannot steal the prepared target's ID.
	auto Other = IMAcousticMetaSound::CreateAcousticMetaSoundContext(DeviceB, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, 100);
	if (!TestTrue(TEXT("Unprepared other device keeps existing context path"), Other.IsValid())) return false;
	Cleanup.Contexts.Add(Other);
	TestEqual(TEXT("Unprepared context ID is zero"), Other->TestCapturePreparationId, uint64(0));
	TestEqual(TEXT("Unprepared trace override preserved"), Other->CapturedSourceBlocks.Num(), 64);
	TestTrue(TEXT("Other device did not consume pending target"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, Id, Error));
	TestFalse(TEXT("Create with target rate mismatch cannot fallback"), IMAcousticMetaSound::CreateAcousticMetaSoundContext(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate + 512, 101).IsValid());
	TestTrue(TEXT("Rejected create preserves pending target"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, Id, Error));
	auto Context = IMAcousticMetaSound::CreateAcousticMetaSoundContext(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, 102);
	if (!TestTrue(TEXT("Fresh context consumes prepared storage"), Context.IsValid())) return false;
	Cleanup.Contexts.Add(Context);
	Cleanup.PendingId = 0;
	TestEqual(TEXT("Context received the exact ID"), Context->TestCapturePreparationId, Id);
	TestEqual(TEXT("Source trace capacity preserved"), Context->CapturedSourceBlocks.Num(), 64);
	TestEqual(TEXT("Environment trace capacity preserved"), Context->CapturedBlocks.Num(), 8);
	TestEqual(TEXT("Bus trace opt-in preserved"), Context->CapturedBusReads.Num(), 8);
	TestTrue(TEXT("PCM opt-out preserved"), Context->CapturedSource.IsEmpty() && Context->CapturedWet.IsEmpty());
	TestEqual(TEXT("Source ready words moved"), Context->CapturedSourceBlockReadyWordCount, uint32(1));
	if (!TestTrue(TEXT("Ready storage exists"), bool(Context->CapturedSourceBlockReady))
		|| Context->CapturedSourceBlockReadyWordCount != 1) return false;
	TestEqual(TEXT("Ready word starts zero"), Context->CapturedSourceBlockReady[0].load(std::memory_order_relaxed), uint64(0));
	TestFalse(TEXT("Consumed ID cannot validate again"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, Id, Error));
	TestFalse(TEXT("Duplicate context cannot consume a second time"), IMAcousticMetaSound::CreateAcousticMetaSoundContext(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, 103).IsValid());
	uint64 ExistingId = 0;
	TestFalse(TEXT("Preexisting context rejects new prepare"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, ExistingId, Error));

	// Cancellation of a consumed ID must not touch this active synthetic
	// context's counters or the new pending owner on another device.
	const auto* SourceRows = Context->CapturedSourceBlocks.GetData();
	Context->CapturedSourceFrames.store(17, std::memory_order_relaxed);
	uint64 NewId = 0;
	if (!TestTrue(TEXT("New pending owner can prepare"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceC, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, NewId, Error))) return false;
	Cleanup.PendingId = NewId;
	TestTrue(TEXT("IDs are not recycled"), NewId != Id);
	TestFalse(TEXT("Consumed old ID cannot cancel new owner"), IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(Id));
	TestTrue(TEXT("New owner remains ready"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceC, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, NewId, Error));
	TestEqual(TEXT("Active context prefix is not reset"), Context->CapturedSourceFrames.load(std::memory_order_relaxed), uint32(17));
	TestTrue(TEXT("Active context array address is unchanged"), Context->CapturedSourceBlocks.GetData() == SourceRows);
	TestTrue(TEXT("Unconsumed cancellation releases pending owner"), IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(NewId));
	Cleanup.PendingId = 0;
	TestFalse(TEXT("Cancelled ID cannot validate"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceC, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, NewId, Error));
	TestFalse(TEXT("Repeated cancellation is a no-op"), IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(NewId));

	uint64 SessionId = 0;
	if (!TestTrue(TEXT("Prepare another enable session"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceC, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, SessionId, Error))) return false;
	Cleanup.PendingId = SessionId;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 64, 8, false, true);
	TestFalse(TEXT("Even same settings in a new enable session invalidate old ID"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceC, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, SessionId, Error));
	TestFalse(TEXT("Invalidated session ID cannot cancel"), IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(SessionId));
	Cleanup.PendingId = 0;
	uint64 DisabledId = 0;
	if (!TestTrue(TEXT("Prepare before disable"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceC, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, DisabledId, Error))) return false;
	Cleanup.PendingId = DisabledId;
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
	TestFalse(TEXT("Disable rejects pending validation"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceC, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, DisabledId, Error));
	TestFalse(TEXT("Disable already released unconsumed pending storage"), IMAcousticMetaSound::IM_CancelPreparedAcousticMetaSoundCaptureForTest(DisabledId));
	Cleanup.PendingId = 0;

	// One full-capacity positive control. No recorder, source or operator is
	// started on these synthetic IDs; this does not sign Q5 audio behavior.
	IMAcousticMetaSound::StopAcousticMetaSoundContext(Context);
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 0, 0, true, true);
	uint64 FullId = 0;
	if (!TestTrue(TEXT("Prepare full original Q5 storage"), IMAcousticMetaSound::IM_PrepareAcousticMetaSoundCaptureForTest(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, FullId, Error))) return false;
	Cleanup.PendingId = FullId;
	auto Full = IMAcousticMetaSound::CreateAcousticMetaSoundContext(DeviceA, IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate, 104);
	if (!TestTrue(TEXT("Transfer full original Q5 storage"), Full.IsValid())) return false;
	Cleanup.Contexts.Add(Full);
	Cleanup.PendingId = 0;
	TestEqual(TEXT("Full context received the exact ID"), Full->TestCapturePreparationId, FullId);
	TestEqual(TEXT("200s source PCM frames"), Full->CapturedSource.Num(), IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate * 200);
	TestEqual(TEXT("200s bus PCM frames"), Full->CapturedBus.Num(), IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate * 200);
	TestEqual(TEXT("200s stereo dry samples"), Full->CapturedDry.Num(), IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate * 400);
	TestEqual(TEXT("200s stereo wet samples"), Full->CapturedWet.Num(), IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate * 400);
	TestEqual(TEXT("Default source rows"), Full->CapturedSourceBlocks.Num(), 131072);
	TestEqual(TEXT("Default environment rows"), Full->CapturedBlocks.Num(), 18750);
	TestEqual(TEXT("Default bus rows"), Full->CapturedBusReads.Num(), 18750);
	TestEqual(TEXT("Default ready words"), Full->CapturedSourceBlockReadyWordCount, uint32(2048));
	if (Full->CapturedSource.Num() != IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate * 200 || Full->CapturedWet.Num() != IMAcousticQ5RoomGraphTestPrivate::IMQ5Rate * 400
		|| !TestTrue(TEXT("Full ready storage exists"), bool(Full->CapturedSourceBlockReady))
		|| Full->CapturedSourceBlockReadyWordCount != 2048) return false;
	TestEqual(TEXT("First source sample zero"), Full->CapturedSource[0], 0.0f);
	TestEqual(TEXT("Last wet sample zero"), Full->CapturedWet.Last(), 0.0f);
	TestEqual(TEXT("Last ready word zero"), Full->CapturedSourceBlockReady[2047].load(std::memory_order_relaxed), uint64(0));
	TestEqual(TEXT("Fresh source prefix zero"), Full->CapturedSourceFrames.load(std::memory_order_relaxed), uint32(0));
	TestEqual(TEXT("Fresh environment prefix zero"), Full->CapturedEnvironmentFrames.load(std::memory_order_relaxed), uint32(0));
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	IM_AcousticQ5RoomGraph,
	"IceMoon.AcousticField.Q5.RoomGraphWetT20",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool IM_AcousticQ5RoomGraph::RunTest(const FString&)
{
	const bool bOrderOnlyClosedLow = FParse::Param(FCommandLine::Get(), TEXT("IMAcousticQ5OrderOnlyClosedLow"));
	if (bOrderOnlyClosedLow && !FParse::Param(FCommandLine::Get(), TEXT("IMAcousticQ5TransportOrder")))
	{
		AddError(TEXT("Order-only closed-low requires IMAcousticQ5TransportOrder before any case or capture is prepared."));
		return false;
	}
	auto State = MakeShared<IMAcousticQ5RoomGraphTestPrivate::FIMQ5RunState>(this);
	State->bOrderOnlyClosedLow = bOrderOnlyClosedLow;
	IFileManager::Get().MakeDirectory(*State->EvidenceDirectory, true);
	FString Error;
	if (!IMAcousticQ5RoomGraphTestPrivate::IMQ5CaptureOriginalState(*State, Error))
	{
		TSharedRef<FJsonObject> Terminal = MakeShared<FJsonObject>();
		Terminal->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5RoomGraphTerminal.v1"));
		Terminal->SetStringField(TEXT("run_id"), State->RunId);
		Terminal->SetBoolField(TEXT("passed"), false);
		Terminal->SetStringField(TEXT("failure"), Error);
		if (State->bOrderOnlyClosedLow)
		{
			Terminal->SetStringField(TEXT("schema"), TEXT("IMAcousticQ5OrderOnlyTerminal.v1"));
			IMAcousticQ5RoomGraphTestPrivate::IMQ5SetOrderOnlyScope(*Terminal, false);
		}
		IMAcousticQ5RoomGraphTestPrivate::IMQ5WriteJson(FPaths::Combine(State->EvidenceDirectory, TEXT("terminal.json")), Terminal);
		AddError(Error);
		return false;
	}
	FApp::SetUnfocusedVolumeMultiplier(1.0f);
	GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
	State->bCaptureTransportOrder = FParse::Param(FCommandLine::Get(), TEXT("IMAcousticQ5TransportOrder"));
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 0, 0, true, true, State->bCaptureTransportOrder);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticQ5RoomGraph START run_id=%s map=%s source=%s evidence=%s"),
		*State->RunId, *State->OriginalMapPath, IMAcousticQ5RoomGraphTestPrivate::IMQ5SourcePath, *State->EvidenceDirectory);
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<IMAcousticQ5RoomGraphTestPrivate::FIMQ5RoomGraphCommand>(State));
	return true;
}

#endif
