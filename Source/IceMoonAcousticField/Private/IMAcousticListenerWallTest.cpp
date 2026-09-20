#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticMetaSound.h"
#include "AudioDevice.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Editor/EditorPerformanceSettings.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

namespace
{
constexpr const TCHAR* IMWallMap = TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
const FVector IMValidListener(750.0f, 300.0f, 150.0f);
// The front wall of the first whitebox room is centered at Y=0 and is 20 cm
// thick. This point is 5 cm inside the slab, not merely on its surface.
const FVector IMWallInteriorListener(550.0f, 5.0f, 150.0f);

class IM_AcousticListenerWallCommand final : public IAutomationLatentCommand
{
public:
    explicit IM_AcousticListenerWallCommand(FAutomationTestBase* InTest)
        : Test(InTest), Started(FPlatformTime::Seconds()) {}

    bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - Started > 45.0)
        {
            return Finish(false, TEXT("listener wall coverage probe timed out"));
        }

        UWorld* World = nullptr;
        for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
        {
            if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
            {
                World = WorldContext.World();
                break;
            }
        }
        if (!World) return false;

        if (!Listener.IsValid())
        {
            Listener = World->GetFirstPlayerController();
            if (!Listener.IsValid()) return false;
            AudioDevice = World->GetAudioDeviceRaw();
            if (!AudioDevice) return false;
            for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
            {
                Volume = *It;
                break;
            }
            if (!Volume.IsValid()) return Finish(false, TEXT("listener wall coverage map has no bake volume"));
            Context = IM_FindAcousticMetaSoundContext(AudioDevice->DeviceID);
            if (!Context) return false;
            SetListener(World, IMValidListener);
            BaselineIR = Context->LastIRSequence.load(std::memory_order_acquire);
            BaselineWet = Context->Device->ReverbNonzeroBlocks.load(std::memory_order_acquire);
            BaselineReject = Context->Device->ReverbRejectedBlocks.load(std::memory_order_acquire);
            BaselineNoIR = Context->NoIRBlocks.load(std::memory_order_acquire);
            StageStarted = Now;
            return false;
        }

        if (Stage == 0)
        {
            if (!Context.IsValid() || !Context->Device.IsValid())
            {
                return Finish(false, TEXT("acoustic MetaSound device bridge was released during listener probe"));
            }
            const uint64 IR = Context->LastIRSequence.load(std::memory_order_acquire);
            const uint64 Wet = Context->Device->ReverbNonzeroBlocks.load(std::memory_order_acquire);
            if (IR <= BaselineIR || Wet <= BaselineWet)
            {
                if (Now - StageStarted > 20.0)
                {
                    return Finish(false, TEXT("valid listener never produced a fresh audible IR"));
                }
                return false;
            }
            ValidIR = IR;
            ValidWet = Wet;
            SetListener(World, IMWallInteriorListener);
            Stage = 1;
            StageStarted = Now;
            UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticListenerWall valid_ready ir=%llu wet=%llu wall_ue=(%g,%g,%g)"),
                ValidIR, ValidWet, IMWallInteriorListener.X, IMWallInteriorListener.Y, IMWallInteriorListener.Z);
            return false;
        }

        if (Stage == 1)
        {
            // Wait longer than the 250 ms IR freshness lease. Any wet blocks
            // after this point must come from a newly accepted wall position.
            if (Now - StageStarted < 1.25) return false;
            WallIR = Context->LastIRSequence.load(std::memory_order_acquire);
            WallWet = Context->Device->ReverbNonzeroBlocks.load(std::memory_order_acquire);
            WallReject = Context->Device->ReverbRejectedBlocks.load(std::memory_order_acquire);
            WallNoIR = Context->NoIRBlocks.load(std::memory_order_acquire);
            WallStatus = Volume.IsValid() ? Volume->Status : TEXT("volume released");
            SetListener(World, IMValidListener);
            Stage = 2;
            StageStarted = Now;
            return false;
        }

        if (Stage == 2 && Now - StageStarted >= 0.75)
        {
            const uint64 WetDelta = WallWet - ValidWet;
            const uint64 IRDelta = WallIR - ValidIR;
            const uint64 RejectDelta = WallReject - BaselineReject;
            const uint64 NoIRDelta = WallNoIR - BaselineNoIR;
            const bool Safe = WetDelta == 0 && IRDelta == 0 && NoIRDelta > 0
                && WallStatus.Contains(TEXT("listener inside baked acoustic geometry"));
            FString Message = FString::Printf(
                TEXT("wall_listener safe=%d valid_ir=%llu wall_ir=%llu valid_wet=%llu wall_wet=%llu wall_reject=%llu wall_no_ir=%llu status=%s"),
                Safe ? 1 : 0, ValidIR, WallIR, ValidWet, WallWet, RejectDelta, NoIRDelta, *WallStatus);
            WriteEvidence(Safe, Message);
            return Finish(Safe, Message);
        }
        return false;
    }

private:
    void SetListener(UWorld* World, const FVector& Position)
    {
        Listener->SetAudioListenerOverride(nullptr, Position, FRotator::ZeroRotator);
        // NullRHI has no view update; publish the same public listener input so
        // this test still exercises the exact AudioDevice -> worker path.
        if (!FApp::CanEverRender() && AudioDevice)
        {
            AudioDevice->SetListener(World, 0, FTransform(FRotator::ZeroRotator, Position), 0.0f);
        }
    }

    void WriteEvidence(bool bSafe, const FString& Message)
    {
        const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("AcousticV2/ListenerWall"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        IFileManager::Get().MakeDirectory(*Dir, true);
        FString Json;
        auto Writer = TJsonWriterFactory<>::Create(&Json);
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("status"), bSafe ? TEXT("PASS_LISTENER_WALL_FAIL_CLOSED") : TEXT("FAIL_LISTENER_WALL_WET_CHANGED"));
        Writer->WriteValue(TEXT("map"), IMWallMap);
        Writer->WriteValue(TEXT("message"), Message);
        Writer->WriteValue(TEXT("valid_listener_ue_cm"), TEXT("(750,300,150)"));
        Writer->WriteValue(TEXT("wall_listener_ue_cm"), TEXT("(550,5,150)"));
        Writer->WriteValue(TEXT("valid_ir_sequence"), double(ValidIR));
        Writer->WriteValue(TEXT("wall_ir_sequence"), double(WallIR));
        Writer->WriteValue(TEXT("valid_wet_blocks"), double(ValidWet));
        Writer->WriteValue(TEXT("wall_wet_blocks"), double(WallWet));
        Writer->WriteValue(TEXT("wall_rejected_delta"), double(WallReject - BaselineReject));
        Writer->WriteValue(TEXT("wall_no_ir_delta"), double(WallNoIR - BaselineNoIR));
        Writer->WriteValue(TEXT("wall_status"), WallStatus);
        Writer->WriteObjectEnd();
        Writer->Close();
        FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Dir, TEXT("listener-wall.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticListenerWall evidence=%s"), *Dir);
    }

    bool Finish(bool bPass, const FString& Message)
    {
        if (!bPass) Test->AddError(Message);
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticListenerWall %s %s"), bPass ? TEXT("PASS") : TEXT("FAIL"), *Message);
        UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticListenerWall %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
        if (GEditor && GEditor->PlayWorld) GEditor->RequestEndPlayMap();
        return true;
    }

    FAutomationTestBase* Test = nullptr;
    double Started = 0.0, StageStarted = 0.0;
    int32 Stage = 0;
    TWeakObjectPtr<APlayerController> Listener;
    TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
    FAudioDevice* AudioDevice = nullptr;
    IM_AcousticMetaSoundContextPtr Context;
    uint64 BaselineIR = 0, BaselineWet = 0, BaselineReject = 0, BaselineNoIR = 0;
    uint64 ValidIR = 0, ValidWet = 0, WallIR = 0, WallWet = 0, WallReject = 0, WallNoIR = 0;
    FString WallStatus;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticListenerWallTest,
    "IceMoon.AcousticField.MetaSound.ListenerWallFailClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FIMAcousticListenerWallTest::RunTest(const FString&)
{
    FApp::SetUnfocusedVolumeMultiplier(1.0f);
    GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
    GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
    FString Error;
    GUnrealEd->AutomationLoadMap(IMWallMap, false, &Error);
    if (!Error.IsEmpty())
    {
        AddError(Error);
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(IM_AcousticListenerWallCommand(this));
    return true;
}

#endif
