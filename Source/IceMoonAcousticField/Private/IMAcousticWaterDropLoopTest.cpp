#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSpatialization.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceRedirector.h"
#include "MetasoundSource.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Serialization/JsonWriter.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

namespace IMAcousticWaterDropLoopTestPrivate
{
constexpr const TCHAR* WaterDropMap = TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
constexpr const TCHAR* WaterDropSourceTag = TEXT("IMAcousticAuditionReferenceV1");
constexpr const TCHAR* WaterDropMetaSound = TEXT("/IceMoonAcousticField/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay");
constexpr double WaterDropLoopSeconds = 28.176;
constexpr double WaterDropCaptureSeconds = 62.5;
constexpr double WaterDropWarmupSeconds = 2.0;
constexpr double WaterDropTimeoutSeconds = 210.0;
constexpr int64 WaterDropMinimumWavBytes = 11 * 1000 * 1000;
constexpr double WaterDropFileStableSeconds = 1.0;

class FIMAcousticWaterDropLoopCommand final : public IAutomationLatentCommand
{
public:
    explicit FIMAcousticWaterDropLoopCommand(FAutomationTestBase* InTest)
        : Test(InTest), Created(FPlatformTime::Seconds())
    {
    }

    bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - Created > WaterDropTimeoutSeconds)
        {
            return Finish(false, TEXT("Water-drop Dry/Wet recording exceeded its bounded runtime."));
        }

        UWorld* PIE = nullptr;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::PIE && Context.World())
            {
                PIE = Context.World();
                break;
            }
        }
        if (!PIE)
        {
            return Stage == 0 ? false : Finish(false, TEXT("PIE ended during the water-drop recording."));
        }

        if (Stage == 0)
        {
            for (TActorIterator<AIMAcousticBakeVolume> It(PIE); It; ++It)
            {
                if (Volume.IsValid())
                {
                    return Finish(false, TEXT("Audition map contains multiple acoustic bake volumes."));
                }
                Volume = *It;
            }
            if (!Volume.IsValid())
            {
                return false;
            }

            for (TActorIterator<AActor> It(PIE); It; ++It)
            {
                if (!It->ActorHasTag(WaterDropSourceTag))
                {
                    continue;
                }
                if (SourceActor.IsValid())
                {
                    return Finish(false, TEXT("Audition map contains multiple tagged water-drop source actors."));
                }
                SourceActor = *It;
            }
            if (!SourceActor.IsValid())
            {
                return false;
            }

            TArray<UAudioComponent*> AudioComponents;
            SourceActor->GetComponents(AudioComponents);
            if (AudioComponents.Num() != 1)
            {
                return Finish(false, FString::Printf(TEXT("Expected one source AudioComponent, found %d."), AudioComponents.Num()));
            }
            Audio = AudioComponents[0];
            SourceMarker = SourceActor->FindComponentByClass<UIMAcousticSourceComponent>();
            MetaSound = Cast<UMetaSoundSource>(Audio->Sound);
            if (!SourceMarker.IsValid() || !MetaSound.IsValid() || !MetaSound->GetPathName().StartsWith(WaterDropMetaSound)
                || MetaSound->OutputFormat != EMetaSoundOutputAudioFormat::Mono || MetaSound->NumChannels != 1)
            {
                return Finish(false, TEXT("PIE source is not the authored mono water-drop MetaSound."));
            }
            FString SourceError;
            if (!SourceMarker->ValidateSource(SourceError))
            {
                return Finish(false, FString::Printf(TEXT("MetaSound source validation failed: %s"), *SourceError));
            }
            if (!FMath::IsNearlyEqual(Audio->VolumeMultiplier, 0.7f, 0.001f)
                || !FMath::IsNearlyEqual(Audio->PitchMultiplier, 1.0f, 0.001f))
            {
                return Finish(false, TEXT("PIE source gain/pitch differs from the shared Dry/Wet contract."));
            }

            Bridge = IMAcousticSpatialization::FindAcousticDevice(PIE->GetAudioDeviceRaw());
            if (!Bridge || !Bridge->Alive.load(std::memory_order_acquire))
            {
                return false;
            }

            PreviousUnfocusedVolume = FApp::GetUnfocusedVolumeMultiplier();
            FApp::SetUnfocusedVolumeMultiplier(1.0f);
            PreviousBackgroundAudio = GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
            GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
            PreviousThrottle = GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground;
            GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
            SettingsChanged = true;

            EvidenceDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
                FPaths::ProjectSavedDir(),
                TEXT("AcousticV2/ListeningPackage/20260918-hybrid-current/recordings"),
                FGuid::NewGuid().ToString(EGuidFormats::Digits)));
            if (!IFileManager::Get().MakeDirectory(*EvidenceDirectory, true))
            {
                return Finish(false, TEXT("Cannot create the water-drop recording evidence directory."));
            }

            ApplyRoutes(7);
            Audio->Stop();
            Audio->Play();
            Stage = 1;
            StageStarted = Now;
            return false;
        }

        if (!Volume.IsValid() || !Audio.IsValid() || !Bridge || !Bridge->Alive.load(std::memory_order_acquire))
        {
            return Finish(false, TEXT("Water-drop PIE source, volume, or audio bridge disappeared."));
        }

        if (Stage == 1)
        {
            if (Now - StageStarted < WaterDropWarmupSeconds)
            {
                return false;
            }
            BeginCapture(3, TEXT("Eryliaa_FullLoop_Dry_DirectPath"), PIE, Now);
            Stage = 2;
            StageStarted = Now;
            return false;
        }

        if (Stage == 2)
        {
            if (Now - StageStarted < WaterDropCaptureSeconds)
            {
                return false;
            }
            EndCapture(PIE);
            Stage = 3;
            StageStarted = Now;
            return false;
        }

        if (Stage == 3)
        {
            if (!CaptureFileReady(DryFile))
            {
                return false;
            }
            BeginCapture(4, TEXT("Eryliaa_FullLoop_Wet_ProjectSubmixReverb"), PIE, Now);
            Stage = 4;
            StageStarted = Now;
            return false;
        }

        if (Stage == 4)
        {
            if (Now - StageStarted < WaterDropCaptureSeconds)
            {
                return false;
            }
            EndCapture(PIE);
            Stage = 5;
            StageStarted = Now;
            return false;
        }

        if (Stage == 5)
        {
            if (!CaptureFileReady(WetFile))
            {
                return false;
            }
            return Finish(WriteReceipt(), TEXT("Water-drop full-source Dry/Wet recordings completed."));
        }

        return false;
    }

private:
    void ApplyRoutes(uint32 Routes)
    {
        Volume->bEnableV2 = true;
        Volume->bDirectRoute = (Routes & 1u) != 0;
        Volume->bPathRoute = (Routes & 2u) != 0;
        Volume->bReverbRoute = (Routes & 4u) != 0;
        Bridge->RenderRoutes.store(Routes, std::memory_order_relaxed);
    }

    void BeginCapture(uint32 Routes, const TCHAR* Name, UWorld* PIE, double Now)
    {
        ApplyRoutes(Routes);
        CurrentFile = FPaths::Combine(EvidenceDirectory, FString(Name) + TEXT(".wav"));
        if (Routes == 3u)
        {
            DryFile = CurrentFile;
            DryRoutes = Routes;
        }
        else
        {
            WetFile = CurrentFile;
            WetRoutes = Routes;
        }
        Audio->Stop();
        Audio->SetVolumeMultiplier(0.7f);
        UAudioMixerBlueprintLibrary::StartRecordingOutput(PIE, static_cast<float>(WaterDropCaptureSeconds + 2.0), nullptr);
        Audio->Play();
        Recording = true;
        CaptureStarted = Now;
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticWaterDropCapture start routes=%u file=%s expected_s=%.3f source=%s"), Routes, *CurrentFile, WaterDropCaptureSeconds, *MetaSound->GetPathName());
    }

    void EndCapture(UWorld* PIE)
    {
        if (!Recording)
        {
            return;
        }
        UAudioMixerBlueprintLibrary::StopRecordingOutput(
            PIE,
            EAudioRecordingExportType::WavFile,
            FPaths::GetBaseFilename(CurrentFile),
            EvidenceDirectory,
            nullptr,
            nullptr);
        Recording = false;
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticWaterDropCapture stop file=%s elapsed_s=%.3f"), *CurrentFile, FPlatformTime::Seconds() - CaptureStarted);
    }

    bool CaptureFileReady(const FString& File)
    {
        // StopRecordingOutput finalizes asynchronously; reject the early partial file before writing the receipt.
        const int64 Size = File.IsEmpty() ? -1 : IFileManager::Get().FileSize(*File);
        if (Size < WaterDropMinimumWavBytes)
        {
            ReadyProbeFile.Reset();
            ReadyProbeBytes = -1;
            ReadyProbeSince = 0.0;
            return false;
        }
        if (ReadyProbeFile != File || ReadyProbeBytes != Size)
        {
            ReadyProbeFile = File;
            ReadyProbeBytes = Size;
            ReadyProbeSince = FPlatformTime::Seconds();
            return false;
        }
        return FPlatformTime::Seconds() - ReadyProbeSince >= WaterDropFileStableSeconds;
    }

    bool WriteReceipt() const
    {
        FString Json;
        auto Writer = TJsonWriterFactory<>::Create(&Json);
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("schema"), TEXT("IMAcousticWaterDropFullLoopRecording.v1"));
        Writer->WriteValue(TEXT("status"), TEXT("PASS_RECORDING_COMPLETE_ANALYSIS_PENDING"));
        Writer->WriteValue(TEXT("map"), WaterDropMap);
        Writer->WriteValue(TEXT("source_metasound"), WaterDropMetaSound);
        Writer->WriteValue(TEXT("source_duration_seconds"), WaterDropLoopSeconds);
        Writer->WriteValue(TEXT("recording_contract"), TEXT("record at least 62.5 seconds so both 28.176s and 56.352s full-asset boundaries are audible"));
        Writer->WriteValue(TEXT("same_source_and_start"), true);
        Writer->WriteValue(TEXT("audio_component_gain"), 0.7);
        Writer->WriteValue(TEXT("offline_reverb"), false);
        Writer->WriteValue(TEXT("dry_route_mask"), DryRoutes);
        Writer->WriteValue(TEXT("wet_route_mask"), WetRoutes);
        Writer->WriteValue(TEXT("dry_file"), DryFile);
        Writer->WriteValue(TEXT("wet_file"), WetFile);
        Writer->WriteValue(TEXT("dry_file_bytes"), static_cast<double>(IFileManager::Get().FileSize(*DryFile)));
        Writer->WriteValue(TEXT("wet_file_bytes"), static_cast<double>(IFileManager::Get().FileSize(*WetFile)));
        Writer->WriteValue(TEXT("raw_source_sha256"), TEXT("8284c59901421d002db751a1e082d2551a74cb7789ddf97e782baf95221e3159"));
        Writer->WriteValue(TEXT("loop_boundary_seconds_1"), WaterDropLoopSeconds);
        Writer->WriteValue(TEXT("loop_boundary_seconds_2"), WaterDropLoopSeconds * 2.0);
        Writer->WriteObjectEnd();
        Writer->Close();
        const FString Receipt = FPaths::Combine(EvidenceDirectory, TEXT("runtime-recording-receipt.json"));
        const bool bSaved = FFileHelper::SaveStringToFile(Json, *Receipt, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticWaterDropRecordingReceipt saved=%d path=%s dry_bytes=%lld wet_bytes=%lld"), bSaved, *Receipt, IFileManager::Get().FileSize(*DryFile), IFileManager::Get().FileSize(*WetFile));
        return bSaved;
    }

    bool Finish(bool bSuccess, const FString& Message)
    {
        if (Recording)
        {
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
            {
                if (Context.WorldType == EWorldType::PIE && Context.World())
                {
                    EndCapture(Context.World());
                    break;
                }
            }
        }
        if (Bridge)
        {
            Bridge->RenderRoutes.store(7, std::memory_order_relaxed);
        }
        if (SettingsChanged)
        {
            FApp::SetUnfocusedVolumeMultiplier(PreviousUnfocusedVolume);
            GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = PreviousBackgroundAudio;
            GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = PreviousThrottle;
            SettingsChanged = false;
        }
        if (!bSuccess)
        {
            Test->AddError(Message);
        }
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticWaterDropRecording %s evidence=%s message=%s"), bSuccess ? TEXT("PASS") : TEXT("FAIL"), *EvidenceDirectory, *Message);
        UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticWaterDropRecording %s"), bSuccess ? TEXT("PASS") : TEXT("FAIL"));
        UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s WaterDrop full-loop Dry/Wet recording"), bSuccess ? TEXT("PASS") : TEXT("FAIL"));
        if (GEditor && GEditor->PlayWorld)
        {
            GEditor->RequestEndPlayMap();
        }
        return true;
    }

    FAutomationTestBase* Test = nullptr;
    double Created = 0.0;
    double StageStarted = 0.0;
    double CaptureStarted = 0.0;
    int32 Stage = 0;
    uint32 DryRoutes = 0;
    uint32 WetRoutes = 0;
    bool Recording = false;
    bool SettingsChanged = false;
    bool PreviousBackgroundAudio = false;
    bool PreviousThrottle = false;
    float PreviousUnfocusedVolume = 1.0f;
    TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
    TWeakObjectPtr<AActor> SourceActor;
    TWeakObjectPtr<UIMAcousticSourceComponent> SourceMarker;
    TWeakObjectPtr<UAudioComponent> Audio;
    TWeakObjectPtr<UMetaSoundSource> MetaSound;
    TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
    FString EvidenceDirectory;
    FString CurrentFile;
    FString DryFile;
    FString WetFile;
    FString ReadyProbeFile;
    int64 ReadyProbeBytes = -1;
    double ReadyProbeSince = 0.0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticWaterDropLoop, "IceMoon.AcousticField.Audition.WaterDropMetaSoundFullLoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticWaterDropLoop::RunTest(const FString&)
{
    FString Error;
    GUnrealEd->AutomationLoadMap(IMAcousticWaterDropLoopTestPrivate::WaterDropMap, false, &Error);
    if (!Error.IsEmpty())
    {
        AddError(FString::Printf(TEXT("Cannot load audition map: %s"), *Error));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(IMAcousticWaterDropLoopTestPrivate::FIMAcousticWaterDropLoopCommand(this));
    return true;
}
#endif
