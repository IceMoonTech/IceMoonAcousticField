#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
// Real UE room-decay gate. The shell supplies geometry and one PCM impulse;
// BakeVolume owns export/baking and the device owns every recorded wet sample.
// No SDK renderer, synthetic IR, or native-fixture WAV is used in this test.
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSpatialization.h"
#include "Audio.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Sound/SoundWaveProcedural.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealEdGlobals.h"

namespace
{
constexpr int32 IMRoomRate = 48000;
constexpr double IMRoomCaptureSeconds = 6.0;
constexpr double IMRoomDecayGap = .10; // Same robust relative gap as native decay.
constexpr int16 IMRoomImpulse = 16384; // 0.5 full-scale, identical for all rooms.

struct IM_RoomCase
{
    const TCHAR* Name;
    const TCHAR* Geometry;
    double HalfX, HalfZ, Absorption, ListenerX; // SDK metres; Y is up.
    bool Ceiling;
};
const IM_RoomCase IMRoomCases[] = {
    {TEXT("closed-low"), TEXT("sealed-room-8x3x6"), 4, 3, .1, 2, true},
    {TEXT("closed-high"), TEXT("sealed-room-8x3x6"), 4, 3, .8, 2, true},
    {TEXT("corridor"), TEXT("sealed-duct-20x3x2"), 10, 1, .1, 6, true},
    {TEXT("outdoor-notop"), TEXT("walled-floor-20x20-notop"), 10, 10, .1, 2, false}
};

bool IMRoomSaveJson(const FString& File, const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    auto Writer = TJsonWriterFactory<>::Create(&Text);
    return FJsonSerializer::Serialize(Object, Writer)
        && FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

UWorld* IMRoomPIE()
{
    for (const auto& Context : GEngine->GetWorldContexts())
        if (Context.WorldType == EWorldType::PIE) return Context.World();
    return nullptr;
}

struct IM_RoomDecay
{
    bool Usable = false;
    double T20 = 0, Energy = 0, Slope = 0;
};

// PCM16 analysis intentionally lives outside the feature. Python recomputes
// the same oracle from WAV bytes, independently of these reported numbers.
bool IMRoomAnalyze(const FString& File, TSharedRef<FJsonObject> Report, IM_RoomDecay& Result, FString& Error)
{
    TArray<uint8> Bytes;
    FWaveModInfo Info;
    if (!FFileHelper::LoadFileToArray(Bytes, *File) || !Info.ReadWaveInfo(Bytes.GetData(), Bytes.Num())
        || !Info.pBitsPerSample || *Info.pBitsPerSample != 16 || !Info.pChannels || *Info.pChannels != 2
        || !Info.pSamplesPerSec || *Info.pSamplesPerSec != IMRoomRate || Info.SampleDataSize % 4)
    { Error = TEXT("Device WAV must be 48 kHz stereo PCM16."); return false; }
    const int32 Frames = Info.SampleDataSize / 4;
    if (Frames < IMRoomRate * (IMRoomCaptureSeconds - .5))
    { Error = TEXT("Device recording is shorter than the capture contract."); return false; }
    TArray<double> Energy, EDC;
    Energy.SetNumZeroed(Frames); EDC.SetNumZeroed(Frames);
    int32 First = INDEX_NONE, Peak = 0;
    double LeadingEnergy = 0, TrailingEnergy = 0;
    for (int32 I = 0; I < Frames; ++I)
    {
        int16 LR[2]; FMemory::Memcpy(LR, Info.SampleDataStart + I * 4, sizeof(LR));
        Peak = FMath::Max(Peak, FMath::Max(FMath::Abs(int32(LR[0])), FMath::Abs(int32(LR[1]))));
        Energy[I] = (double(LR[0]) * LR[0] + double(LR[1]) * LR[1]) / (32768.0 * 32768.0);
        Result.Energy += Energy[I];
        if (Energy[I] > 0 && First == INDEX_NONE) First = I;
        if (I < IMRoomRate / 4) LeadingEnergy += Energy[I];
        if (I >= Frames - IMRoomRate / 2) TrailingEnergy += Energy[I];
    }
    Report->SetNumberField(TEXT("sample_rate_hz"), IMRoomRate);
    Report->SetNumberField(TEXT("channels"), 2);
    Report->SetNumberField(TEXT("frames"), Frames);
    Report->SetNumberField(TEXT("total_energy"), Result.Energy);
    Report->SetNumberField(TEXT("peak_abs_pcm16"), Peak);
    Report->SetNumberField(TEXT("leading_quarter_second_energy"), LeadingEnergy);
    Report->SetNumberField(TEXT("trailing_half_second_energy"), TrailingEnergy);
    Report->SetNumberField(TEXT("first_nonzero_frame"), First);
    Report->SetField(TEXT("t20_s"), MakeShared<FJsonValueNull>());
    Report->SetBoolField(TEXT("decay_fit_usable"), false);
    if (!(Result.Energy > 0) || Peak >= 32760 || LeadingEnergy != 0 || TrailingEnergy != 0
        || First < IMRoomRate / 4 || First > IMRoomRate * 1.5
        || Frames - First < (IM_AcousticRecipe::ReverbSavedDurationS + .5) * IMRoomRate)
    { Error = TEXT("Wet impulse is missing, clipped, contaminated, late, or its full tail was not recorded."); return false; }

    double Suffix = 0;
    for (int32 I = Frames - 1; I >= 0; --I)
    {
        Suffix += Energy[I];
        EDC[I] = 10 * FMath::LogX(10.0, FMath::Max(Suffix / Result.Energy, 1e-30));
    }
    int32 Start = INDEX_NONE, End = INDEX_NONE;
    FString CSV = TEXT("time_s,energy,edc_db\n");
    for (int32 I = 0; I < Frames; ++I)
    {
        if (Start == INDEX_NONE && EDC[I] <= -5) Start = I;
        if (End == INDEX_NONE && EDC[I] <= -25) End = I;
        if (I % 480 == 0)
        {
            double Window = 0;
            for (int32 J = I; J < FMath::Min(I + 480, Frames); ++J) Window += Energy[J];
            CSV += FString::Printf(TEXT("%.9g,%.17g,%.17g\n"), double(I) / IMRoomRate, Window, EDC[I]);
        }
    }
    if (!FFileHelper::SaveStringToFile(CSV, *FPaths::ChangeExtension(File, TEXT("energy-10ms.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { Error = TEXT("Cannot save EDC evidence."); return false; }
    if (Start == INDEX_NONE || End <= Start)
    { Error = TEXT("No usable -5..-25 dB decay interval; T20 remains null."); return false; }
    double SumT = 0, SumD = 0, SumTT = 0, SumTD = 0;
    for (int32 I = Start; I <= End; ++I)
    {
        // Centering time at the fit start avoids cancellation from capture latency.
        const double T = double(I - Start) / IMRoomRate, D = EDC[I];
        SumT += T; SumD += D; SumTT += T * T; SumTD += T * D;
    }
    const int32 Count = End - Start + 1;
    const double Den = Count * SumTT - SumT * SumT;
    if (!(Den > 0)) { Error = TEXT("Degenerate decay fit."); return false; }
    Result.Slope = (Count * SumTD - SumT * SumD) / Den;
    Result.Usable = FMath::IsFinite(Result.Slope) && Result.Slope < 0;
    if (!Result.Usable) { Error = TEXT("Non-decaying fit."); return false; }
    Result.T20 = -60 / Result.Slope; // RT60 extrapolated from the T20 fitting interval.
    Report->SetNumberField(TEXT("t20_s"), Result.T20);
    Report->SetNumberField(TEXT("slope_db_per_s"), Result.Slope);
    Report->SetNumberField(TEXT("fit_samples"), Count);
    Report->SetNumberField(TEXT("edc_minus5_frame"), Start);
    Report->SetNumberField(TEXT("edc_minus25_frame"), End);
    Report->SetBoolField(TEXT("decay_fit_usable"), true);
    return true;
}

struct IM_RoomState
{
    FAutomationTestBase* Test;
    FString Directory, MountRoot, ClosedGeometryIdentity;
    int32 CaseIndex = 0;
    TArray<IM_RoomDecay> Decays;
    float BackgroundVolume = FApp::GetUnfocusedVolumeMultiplier();
    bool PreviousThrottle = GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground;
    bool PreviousAllowBackgroundAudio = GetDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
    bool Restored = false;
    explicit IM_RoomState(FAutomationTestBase* InTest) : Test(InTest)
    {
        const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/W3-Rooms"), Id));
        MountRoot = TEXT("/IceMoonAcousticField/Tests/Rooms/IM_") + Id;
        FApp::SetUnfocusedVolumeMultiplier(1);
        GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
        // bAllowBackgroundAudio twin of the CrossFloor/Lifecycle/W3/W1 fix: an
        // unfocused unattended editor zero-feeds playing voices when this is
        // false (EditorEngine SetVolumeMultiplier(0) path), starving capture.
        GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
    }
    void Restore()
    {
        if (Restored) return;
        FApp::SetUnfocusedVolumeMultiplier(BackgroundVolume);
        GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = PreviousThrottle;
        GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = PreviousAllowBackgroundAudio;
        Restored = true; // Transient settings only; never SaveConfig.
    }
    ~IM_RoomState() { Restore(); }
};

class IM_AcousticRoomCommand final : public IAutomationLatentCommand
{
public:
    explicit IM_AcousticRoomCommand(TSharedRef<IM_RoomState> InState, int32 InStage = 0)
        : State(InState), Stage(InStage), StageStarted(FPlatformTime::Seconds()) {}
    bool Update() override
    {
        check(IsInGameThread()); // All UObjects/test state remain on the GT.
        const double Now = FPlatformTime::Seconds();
        // Stage 3 is a readiness wait (ReverbNonzero/ReverbDry +4); single-voice
        // sessions need longer than 20s to warm up, so the wait is bounded at
        // 120s. Readiness condition, verdicts and all other stages unchanged.
        const double Limit = Stage == 1 ? 600 : (Stage == 3 ? 120 : 30);
        if (Now - StageStarted > Limit) return Finish(false, FString::Printf(TEXT("Room stage %d timed out."), Stage));
        FString Error;
        if (Stage == 0)
        {
            if (IMRoomPIE()) return false;
            if (!CreateFixture(Error)) return Finish(false, Error);
            Volume->GenerateProbes();
            if (Volume->GeneratedProbes <= 0 || Volume->ExportedTriangles <= 0)
                return Finish(false, TEXT("Production probe generation failed: ") + Volume->Status);
            Volume->Bake(); SetStage(1); return false;
        }
        if (Stage == 1)
        {
            if (!Volume.IsValid()) return Finish(false, TEXT("Bake owner disappeared."));
            if (!Volume->BakedField)
            {
                if (!Volume->Status.StartsWith(TEXT("Baking."))) return Finish(false, Volume->Status);
                return false;
            }
            if (!Volume->ValidateCurrentBake(Error)) return Finish(false, Error);
            const auto* Asset = Volume->BakedField.Get();
            Report->SetNumberField(TEXT("exported_triangles"), Volume->ExportedTriangles);
            Report->SetNumberField(TEXT("generated_probes"), Volume->GeneratedProbes);
            Report->SetNumberField(TEXT("sdk_version"), Asset->SDKVersion);
            Report->SetStringField(TEXT("bake_asset"), Asset->GetPathName());
            Report->SetStringField(TEXT("scene_fingerprint"), Asset->SceneFingerprint);
            Report->SetNumberField(TEXT("bake_wall_seconds"), Now - StageStarted);
            // Bake() owns package naming under the plugin's Bakes mount. Do not
            // redirect mounts or rename production output just for this shell.
            if (!FFileHelper::SaveArrayToFile(Asset->SceneData, *CaseFile(TEXT("scene.bin")))
                || !FFileHelper::SaveArrayToFile(Asset->ProbeData, *CaseFile(TEXT("probes.bin")))
                || !FFileHelper::SaveStringToFile(Asset->MetadataJson, *CaseFile(TEXT("bake-metadata.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
                || !SaveMap() || !SaveReport()) return Finish(false, TEXT("Cannot persist real bake evidence/binding."));
            GUnrealEd->AutomationLoadMap(*MapPath, false, &Error);
            if (!Error.IsEmpty()) return Finish(false, Error);
            // AutomationLoadMap enqueues PIE. A new command must run behind it;
            // waiting inside this one prevents the queued startup from executing.
            auto Next = MakeShared<IM_AcousticRoomCommand>(State, 2);
            Next->MapPath = MapPath; Next->Report = Report;
            FAutomationTestFramework::Get().EnqueueLatentCommand(Next);
            return true;
        }
        UWorld* World = IMRoomPIE();
        if (Stage == 8)
        {
            if (World) return false;
            if (++State->CaseIndex == UE_ARRAY_COUNT(IMRoomCases))
            {
                const double Gap = (State->Decays[0].T20 - State->Decays[1].T20) / State->Decays[1].T20;
                auto Summary = MakeShared<FJsonObject>();
                Summary->SetStringField(TEXT("scope"), TEXT("UE-device-room-wet-decay"));
                Summary->SetNumberField(TEXT("cases_completed"), State->Decays.Num());
                Summary->SetNumberField(TEXT("closed_low_t20_s"), State->Decays[0].T20);
                Summary->SetNumberField(TEXT("closed_high_t20_s"), State->Decays[1].T20);
                Summary->SetNumberField(TEXT("relative_gap"), Gap);
                Summary->SetNumberField(TEXT("required_relative_gap"), IMRoomDecayGap);
                Summary->SetBoolField(TEXT("same_closed_geometry"), true);
                Summary->SetBoolField(TEXT("passed"), Gap >= IMRoomDecayGap);
                if (!IMRoomSaveJson(FPaths::Combine(State->Directory, TEXT("summary.json")), Summary))
                    return Finish(false, TEXT("Cannot save room comparison."));
                return Finish(Gap >= IMRoomDecayGap, TEXT("Four UE wet recordings complete; closed-low must decay at least 10% slower than closed-high."));
            }
            FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<IM_AcousticRoomCommand>(State)); return true;
        }
        if (!World) return Stage == 2 ? false : Finish(false, TEXT("PIE ended before room recording."));
        if (Stage == 2)
        {
            for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
            {
                if (Volume.IsValid()) return Finish(false, TEXT("Room PIE has multiple bake owners."));
                Volume = *It;
            }
            Listener = World->GetFirstPlayerController();
            Bridge = IM_FindAcousticDevice(World->GetAudioDeviceRaw());
            if (!Volume.IsValid() || !Listener.IsValid() || !Bridge) return Finish(false, TEXT("Room PIE owner/listener/device unavailable."));
            if (UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) != MapPath)
                return Finish(false, TEXT("PIE opened a different map."));
            if (Bridge->SampleRate != IMRoomRate) return Finish(false, TEXT("Room gate requires a 48 kHz device."));
            Listener->SetAudioListenerOverride(nullptr, FVector(0, IMRoomCases[State->CaseIndex].ListenerX * 100, 150), FRotator(0, -90, 0));
            if (!SpawnSource(World, Error)) return Finish(false, Error);
            WarmWet = Bridge->ReverbNonzeroBlocks.load();
            WarmDry = Bridge->ReverbDryBlocks.load();
            SetStage(3); return false;
        }
        if (!Volume.IsValid() || !Audio.IsValid() || !Wave.IsValid() || !Bridge)
            return Finish(false, TEXT("Room audio owner disappeared."));
        if (Stage == 3)
        {
            Feed(true);
            // Spawn-race watchdog (W1-proven): a playing+active voice can sit on
            // zero dry input for the whole run; bounded recovery is
            // destroy+respawn (2 max). Test-side only; readiness condition,
            // verdicts and counter baselines unchanged.
            if (!RoomInit) { RoomInit = true; RoomIn0 = Bridge->PushDryInputNonzero.load(); RoomT0 = Now; }
            if (RoomRe < 2 && Bridge->PushDryInputNonzero.load() == RoomIn0 && Now - RoomT0 > 10)
            {
                ++RoomRe; RoomInit = false;
                if (auto* Old = Audio.Get()) { if (auto* OA = Old->GetOwner()) OA->Destroy(); }
                Audio.Reset(); Wave.Reset();
                UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticRoomRespawn n=%d case=%s"), RoomRe, IMRoomCases[State->CaseIndex].Name);
                if (!SpawnSource(World, Error)) return Finish(false, Error);
                return false;
            }
            if (Now - RoomDiagT > 15)
            {
                RoomDiagT = Now;
                UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticRoomWaiting case=%s reverb=%llu dry=%llu rendered=%llu rejected=%llu push=%llu/inputnz=%llu"),
                    IMRoomCases[State->CaseIndex].Name, Bridge->ReverbNonzeroBlocks.load(), Bridge->ReverbDryBlocks.load(),
                    Bridge->RenderedBlocks.load(), Bridge->RejectedBlocks.load(),
                    Bridge->PushDryCalls.load(), Bridge->PushDryInputNonzero.load());
            }
            if (Bridge->ReverbNonzeroBlocks.load() < WarmWet + 4 || Bridge->ReverbDryBlocks.load() < WarmDry + 4) return false;
            SetStage(4); return false;
        }
        if (Stage == 4)
        {
            Feed(false);
            // At most 0.35 s of warmup input is queued. Wait the saved IR length
            // plus 0.75 s before the first recorded silence control.
            if (Now - StageStarted < IM_AcousticRecipe::ReverbSavedDurationS + .75) return false;
            SnapshotCounters();
            UAudioMixerBlueprintLibrary::StartRecordingOutput(World, IMRoomCaptureSeconds + 1);
            Recording = true;
            TArray<int16> Input;
            Input.SetNumZeroed(FMath::CeilToInt((.5 + IM_AcousticRecipe::ReverbSavedDurationS + .75) * IMRoomRate));
            Input[IMRoomRate / 2] = IMRoomImpulse;
            TArray<uint8> Raw;
            Raw.Append(reinterpret_cast<const uint8*>(Input.GetData()), Input.Num() * sizeof(int16));
            if (!FFileHelper::SaveArrayToFile(Raw, *CaseFile(TEXT("input-pcm16.bin"))))
                return Finish(false, TEXT("Cannot preserve exact queued input."));
            Wave->QueueAudio(Raw.GetData(), Raw.Num());
            SetStage(5); return false;
        }
        if (Stage == 5)
        {
            Feed(false); // Append zeros only after the impulse payload is consumed.
            if (Now - StageStarted < IMRoomCaptureSeconds) return false;
            UAudioMixerBlueprintLibrary::StopRecordingOutput(World, EAudioRecordingExportType::WavFile, TEXT("wet"), CaseDirectory());
            Recording = false;
            bool CountersPass = WriteCounters();
            Report->SetNumberField(TEXT("capture_wall_seconds"), Now - StageStarted);
            Report->SetBoolField(TEXT("capture_counters_pass"), CountersPass);
            if (!SaveReport()) return Finish(false, TEXT("Cannot save audio counters."));
            // Allow the asynchronous WAV writer to finish even when counters fail.
            SetStage(6); return false;
        }
        if (Stage == 6)
        {
            const int64 Bytes = IFileManager::Get().FileSize(*CaseFile(TEXT("wet.wav")));
            if (Bytes <= 44 || Bytes != LastWavBytes) { LastWavBytes = Bytes; return false; }
            IM_RoomDecay Decay;
            const bool DecayPass = IMRoomAnalyze(CaseFile(TEXT("wet.wav")), Report.ToSharedRef(), Decay, Error);
            const bool Pass = DecayPass && Report->GetBoolField(TEXT("capture_counters_pass"));
            Report->SetBoolField(TEXT("passed"), Pass);
            Report->SetStringField(TEXT("analysis_error"), Error);
            if (!SaveReport()) return Finish(false, TEXT("Cannot save decay result."));
            UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticRoom name=%s t20_s=%.9g wet_energy=%.9g passed=%d evidence=%s"),
                IMRoomCases[State->CaseIndex].Name, Decay.T20, Decay.Energy, int(Pass), *CaseDirectory());
            if (!Pass) return Finish(false, Error.IsEmpty() ? TEXT("Wet capture included degraded/dry/path output or missed processing.") : Error);
            State->Decays.Add(Decay);
            Audio->Stop(); GUnrealEd->RequestEndPlayMap(); SetStage(8); return false;
        }
        return false;
    }
private:
    FString CaseDirectory() const { return FPaths::Combine(State->Directory, IMRoomCases[State->CaseIndex].Name); }
    FString CaseFile(const TCHAR* Name) const { return FPaths::Combine(CaseDirectory(), Name); }
    bool SaveReport() const { return IMRoomSaveJson(CaseFile(TEXT("room.json")), Report.ToSharedRef()); }
    void SetStage(int32 Next) { Stage = Next; StageStarted = FPlatformTime::Seconds(); }
    bool SaveMap() const
    {
        return FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel,
            FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension()));
    }
    bool CreateFixture(FString& Error)
    {
        const auto& C = IMRoomCases[State->CaseIndex];
        MapPath = State->MountRoot + TEXT("/IM_") + FString(C.Name).Replace(TEXT("-"), TEXT("_"));
        if (FPackageName::DoesPackageExist(MapPath)) { Error = TEXT("Refusing to overwrite an existing room fixture."); return false; }
        UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
        auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!World || !Cube || !Cube->GetMaterial(0)) { Error = TEXT("Cannot create room world/engine cube/material."); return false; }
        FString Identity;
        int32 Boxes = 0;
        auto Box = [World, Cube, &Identity, &Boxes](FVector Lo, FVector Hi)
        {
            // The same nativeDecay box bounds, represented by real UE static
            // mesh actors. Conversion: SDK metres (x,y,z) -> UE cm (-z,x,y).
            auto* Actor = World->SpawnActor<AStaticMeshActor>();
            if (!Actor) return false;
            auto* Mesh = Actor->GetStaticMeshComponent();
            Mesh->SetMobility(EComponentMobility::Static); Mesh->SetStaticMesh(Cube);
            const FVector Center = (Lo + Hi) * .5, Size = Hi - Lo;
            Actor->SetActorLocation(FVector(-Center.Z, Center.X, Center.Y) * 100);
            Actor->SetActorScale3D(FVector(Size.Z, Size.X, Size.Y));
            Actor->SetActorLabel(FString::Printf(TEXT("IMRoomBox_%02d"), Boxes++));
            Mesh->ComponentTags.Add(TEXT("IMAcousticRequired"));
            // Read back the actual mesh/transforms, excluding materials and map
            // package names so the low/high geometry comparison is meaningful.
            Identity += Cube->GetPathName() + TEXT("|") + Mesh->GetComponentTransform().ToString() + TEXT("\n");
            return true;
        };
        const double X = C.HalfX, Z = C.HalfZ;
        if (!Box({-X-.1,-.1,-Z-.1}, {X+.1,0,Z+.1})
            || (C.Ceiling && !Box({-X-.1,3,-Z-.1}, {X+.1,3.1,Z+.1}))
            || !Box({-X-.1,0,-Z-.1}, {-X,3,Z+.1}) || !Box({X,0,-Z-.1}, {X+.1,3,Z+.1})
            || !Box({-X,0,-Z-.1}, {X,3,-Z}) || !Box({-X,0,Z}, {X,3,Z+.1}))
        { Error = TEXT("Cannot spawn complete room geometry."); return false; }
        if (State->CaseIndex == 0) State->ClosedGeometryIdentity = Identity;
        if (State->CaseIndex == 1 && State->ClosedGeometryIdentity != Identity)
        { Error = TEXT("Low/high actual geometry differs."); return false; }
        Volume = World->SpawnActor<AIMAcousticBakeVolume>();
        if (!Volume.IsValid()) { Error = TEXT("Cannot spawn production BakeVolume."); return false; }
        Volume->SetActorLocation(FVector(0, 0, 150));
        Volume->BakeBounds->SetBoxExtent(FVector(Z * 100, X * 100, 150));
        Volume->ProbeSpacingCm = 100; Volume->ProbeHeightCm = 150;
        FIMAcousticMaterialMapping Material; Material.Material = Cube->GetMaterial(0);
        Material.Absorption = FVector(C.Absorption); Material.Scattering = .5f; Volume->Materials.Add(Material);
        Volume->bEnableV2 = true; Volume->bDirectRoute = false; Volume->bPathRoute = false;
        Volume->bReverbRoute = true; Volume->ReverbWetGain = 1;
        IFileManager::Get().MakeDirectory(*CaseDirectory(), true);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension())), true);
        Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("scope"), TEXT("UE-device-room-wet-decay"));
        Report->SetStringField(TEXT("name"), C.Name); Report->SetStringField(TEXT("geometry"), C.Geometry);
        Report->SetStringField(TEXT("map"), MapPath);
        Report->SetNumberField(TEXT("absorption_each_band"), C.Absorption);
        Report->SetNumberField(TEXT("scattering"), .5); Report->SetNumberField(TEXT("transmission_each_band"), 0);
        Report->SetNumberField(TEXT("mesh_actors"), Boxes);
        Report->SetNumberField(TEXT("recipe_version"), IM_AcousticRecipe::Version);
        Report->SetNumberField(TEXT("saved_ir_s"), IM_AcousticRecipe::ReverbSavedDurationS);
        Report->SetNumberField(TEXT("probe_spacing_cm"), 100); Report->SetNumberField(TEXT("probe_height_cm"), 150);
        Report->SetNumberField(TEXT("impulse_pcm16"), IMRoomImpulse); Report->SetNumberField(TEXT("wet_gain"), 1);
        Report->SetNumberField(TEXT("source_listener_distance_cm"), 100);
        Report->SetStringField(TEXT("listener_sdk_m"), FString::Printf(TEXT("%.9g,1.5,0"), C.ListenerX));
        Report->SetStringField(TEXT("source_sdk_m"), FString::Printf(TEXT("%.9g,1.5,0"), C.ListenerX - 1));
        if (!FFileHelper::SaveStringToFile(Identity, *CaseFile(TEXT("geometry-identity.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            || !SaveMap() || !SaveReport()) { Error = TEXT("Cannot persist isolated fixture."); return false; }
        return true;
    }
    bool SpawnSource(UWorld* World, FString& Error)
    {
        auto* Actor = World->SpawnActor<AActor>();
        if (!Actor) { Error = TEXT("Cannot spawn source actor."); return false; }
        auto* Component = NewObject<UAudioComponent>(Actor);
        Actor->SetRootComponent(Component); Actor->AddInstanceComponent(Component);
        Component->bAutoActivate = false; Component->RegisterComponent();
        Component->SetWorldLocation(FVector(0, (IMRoomCases[State->CaseIndex].ListenerX - 1) * 100, 150));
        auto* Marker = NewObject<UIMAcousticSourceComponent>(Actor);
        Actor->AddInstanceComponent(Marker); Marker->AudioComponent = Component; Marker->RegisterComponent();
        FSoundAttenuationSettings Attenuation;
        Attenuation.bAttenuate = false; Attenuation.bAttenuateWithLPF = false;
        Attenuation.bEnableOcclusion = false; Attenuation.bEnableListenerFocus = false; Attenuation.bEnableReverbSend = false;
        Attenuation.bSpatialize = true; Attenuation.SpatializationAlgorithm = SPATIALIZATION_HRTF;
        Attenuation.PluginSettings.SpatializationPluginSettingsArray.Add(NewObject<UIMAcousticSpatializationSettings>(Component));
        Component->bAllowSpatialization = true; Component->SetOverrideAttenuation(true); Component->SetAttenuationOverrides(Attenuation);
        auto* Procedural = NewObject<USoundWaveProcedural>(Component);
        Procedural->NumChannels = 1; Procedural->SetSampleRate(IMRoomRate); Procedural->Duration = INDEFINITELY_LOOPING_DURATION;
        Component->SetSound(Procedural); Audio = Component; Wave = Procedural;
        if (!Marker->ValidateSource(Error)) return false;
        Feed(true); Component->Play(); return true;
    }
    void Feed(bool Warmup)
    {
        if (Wave->GetAvailableAudioByteCount() >= IMRoomRate / 10 * sizeof(int16)) return;
        TArray<int16> PCM; PCM.SetNumZeroed(IMRoomRate / 4);
        if (Warmup)
            for (int32 I = 0; I < PCM.Num(); ++I)
            {
                const double T = double(WarmSample++) / IMRoomRate;
                PCM[I] = int16(2000 * (FMath::Sin(2 * PI * 233 * T) + FMath::Sin(2 * PI * 997 * T) + FMath::Sin(2 * PI * 3109 * T)));
            }
        Wave->QueueAudio(reinterpret_cast<const uint8*>(PCM.GetData()), PCM.Num() * sizeof(int16));
    }
    void SnapshotCounters()
    {
        Direct = Bridge->DirectNonzeroBlocks.load(); Path = Bridge->PathNonzeroBlocks.load();
        Rejected = Bridge->RejectedBlocks.load(); ReverbRejected = Bridge->ReverbRejectedBlocks.load();
        DryDropped = Bridge->DryDroppedBlocks.load(); Wet = Bridge->ReverbNonzeroBlocks.load();
        Processed = Bridge->ReverbProcessedBlocks.load(); Dry = Bridge->ReverbDryBlocks.load();
    }
    bool WriteCounters()
    {
        const uint64 D = Bridge->DirectNonzeroBlocks.load() - Direct, P = Bridge->PathNonzeroBlocks.load() - Path;
        const uint64 R = Bridge->RejectedBlocks.load() - Rejected, RR = Bridge->ReverbRejectedBlocks.load() - ReverbRejected;
        const uint64 Drop = Bridge->DryDroppedBlocks.load() - DryDropped, W = Bridge->ReverbNonzeroBlocks.load() - Wet;
        const uint64 Calls = Bridge->ReverbProcessedBlocks.load() - Processed, Input = Bridge->ReverbDryBlocks.load() - Dry;
        Report->SetNumberField(TEXT("direct_nonzero_blocks"), double(D)); Report->SetNumberField(TEXT("path_nonzero_blocks"), double(P));
        Report->SetNumberField(TEXT("degraded_blocks"), double(R)); Report->SetNumberField(TEXT("reverb_rejected_blocks"), double(RR));
        Report->SetNumberField(TEXT("dry_dropped_blocks"), double(Drop)); Report->SetNumberField(TEXT("wet_nonzero_blocks"), double(W));
        Report->SetNumberField(TEXT("reverb_processed_blocks"), double(Calls)); Report->SetNumberField(TEXT("reverb_dry_blocks"), double(Input));
        Report->SetNumberField(TEXT("device_block_frames"), Bridge->BlockFrames);
        Report->SetNumberField(TEXT("world_epoch"), double(Bridge->WorldGeneration.load()));
        Report->SetNumberField(TEXT("max_gt_gap_us"), double(Bridge->MaxSnapshotGapUs.load()));
        Report->SetNumberField(TEXT("max_worker_gap_us"), double(Bridge->MaxWorkerGapUs.load()));
        const double Expected = IMRoomCaptureSeconds * IMRoomRate / Bridge->BlockFrames;
        return D == 0 && P == 0 && R == 0 && RR == 0 && Drop == 0 && W > 0 && Calls >= Expected * .9 && Input >= Expected * .9;
    }
    bool Finish(bool Pass, const FString& Message)
    {
        if (Recording && IMRoomPIE())
            UAudioMixerBlueprintLibrary::StopRecordingOutput(IMRoomPIE(), EAudioRecordingExportType::WavFile, TEXT("wet-incomplete"), CaseDirectory());
        if (!Pass && Volume.IsValid())
        {
            Volume->CancelBake();
            for (const FString& Issue : Volume->SceneIssues) State->Test->AddInfo(Issue);
        }
        auto Terminal = MakeShared<FJsonObject>();
        Terminal->SetBoolField(TEXT("passed"), Pass); Terminal->SetStringField(TEXT("message"), Message);
        Terminal->SetNumberField(TEXT("stage"), Stage); Terminal->SetNumberField(TEXT("cases_completed"), State->Decays.Num());
        if (!IMRoomSaveJson(FPaths::Combine(State->Directory, TEXT("terminal.json")), Terminal)) Pass = false;
        if (!Pass) State->Test->AddError(Message); else State->Test->AddInfo(Message);
        State->Restore();
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticRoomRecordings %s %s evidence=%s"), Pass ? TEXT("PASS") : TEXT("FAIL"), *Message, *State->Directory);
        UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticRoomRecordings %s"), Pass ? TEXT("PASS") : TEXT("FAIL"));
        UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), Pass ? TEXT("PASS") : TEXT("FAIL"));
        if (GUnrealEd && IMRoomPIE()) GUnrealEd->RequestEndPlayMap();
        return true;
    }
    TSharedRef<IM_RoomState> State;
    int32 Stage; double StageStarted;
    FString MapPath; TSharedPtr<FJsonObject> Report;
    TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
    TWeakObjectPtr<APlayerController> Listener;
    TWeakObjectPtr<UAudioComponent> Audio;
    TWeakObjectPtr<USoundWaveProcedural> Wave;
    TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
    bool Recording = false; int64 WarmSample = 0, LastWavBytes = -1;
    uint64 WarmWet = 0, WarmDry = 0, Direct = 0, Path = 0, Rejected = 0, ReverbRejected = 0;
    uint64 DryDropped = 0, Wet = 0, Processed = 0, Dry = 0;
    // Spawn-race watchdog + readiness diagnostics (test-side only).
    uint64 RoomIn0 = 0; double RoomT0 = 0; int32 RoomRe = 0; bool RoomInit = false;
    double RoomDiagT = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticRoomRecordings, "IceMoon.AcousticField.W3.RoomRecordings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool IM_AcousticRoomRecordings::RunTest(const FString&)
{
    if (IMRoomPIE()) { AddError(TEXT("RoomRecordings requires an owned idle Editor, not an existing PIE.")); return false; }
    auto State = MakeShared<IM_RoomState>(this);
    IFileManager::Get().MakeDirectory(*State->Directory, true);
    UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticRoomRecordings START evidence=%s"), *State->Directory);
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<IM_AcousticRoomCommand>(State)); return true;
}
#endif
