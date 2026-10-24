#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AudioDevice.h"
#include "IMAcousticAudioRenderer.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticCoordinates.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticSimulation.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AmbientSound.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

namespace IMAcousticRuralReverbDiagnosticTestPrivate
{
constexpr int32 Rate = 48000;
constexpr int32 Block = 512;
constexpr int32 ImpulseFrame = Rate / 4;
constexpr int32 CaptureBlocks = (Rate * 3 + Block - 1) / Block;
constexpr float ImpulseAmplitude = 0.5f;

// Serial SDK ownership, including an exclusive HRTF scratch buffer. Effects die
// before this lease; Simulation separately retains the context for its IR source.
struct FIMSDKLease
{
	IPLContext Context = nullptr;
	IPLHRTF HRTF = nullptr;
	~FIMSDKLease()
	{
		if (HRTF) iplHRTFRelease(&HRTF);
		if (Context) iplContextRelease(&Context);
	}
	bool Initialize()
	{
		if (!IMAcousticSDKContext::GetAcousticSDKContext()) return false;
		Context = iplContextRetain(IMAcousticSDKContext::GetAcousticSDKContext());
		IPLAudioSettings Audio{Rate, Block};
		IPLHRTFSettings Settings{};
		Settings.type = IPL_HRTFTYPE_DEFAULT;
		Settings.volume = 1.0f;
		return iplHRTFCreate(Context, &Audio, &Settings, &HRTF) == IPL_STATUS_SUCCESS;
	}
};

FString FileDigest(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return FString();
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash);
	return BytesToHex(Hash, FSHA1::DigestSize);
}

TSharedRef<FJsonObject> Analyze(const TArray<float>& Stereo)
{
	auto Result = MakeShared<FJsonObject>();
	const int32 Frames = Stereo.Num() / 2;
	TArray<double> Energy;
	Energy.SetNumZeroed(Frames);
	double Total = 0.0, Late50 = 0.0, Late100 = 0.0, Late250 = 0.0, Peak = 0.0;
	int32 First = INDEX_NONE, Last = INDEX_NONE;
	for (int32 I = 0; I < Frames; ++I)
	{
		const double L = Stereo[2 * I], R = Stereo[2 * I + 1];
		Energy[I] = L * L + R * R;
		Total += Energy[I];
		Peak = FMath::Max(Peak, FMath::Max(FMath::Abs(L), FMath::Abs(R)));
		if (Energy[I] > 1.e-20)
		{
			if (First == INDEX_NONE) First = I;
			Last = I;
		}
		if (I >= ImpulseFrame + Rate / 20) Late50 += Energy[I];
		if (I >= ImpulseFrame + Rate / 10) Late100 += Energy[I];
		if (I >= ImpulseFrame + Rate / 4) Late250 += Energy[I];
	}
	double Accumulated = 0.0;
	int32 Q90 = INDEX_NONE, Q99 = INDEX_NONE;
	for (int32 I = 0; I < Frames && Total > 0.0; ++I)
	{
		Accumulated += Energy[I];
		if (Q90 == INDEX_NONE && Accumulated >= Total * .90) Q90 = I;
		if (Q99 == INDEX_NONE && Accumulated >= Total * .99) Q99 = I;
	}
	const auto RelativeSeconds = [](int32 Frame) { return Frame == INDEX_NONE ? -1.0 : double(Frame - ImpulseFrame) / Rate; };
	Result->SetNumberField(TEXT("stereo_integrated_energy"), Total);
	Result->SetNumberField(TEXT("sample_peak"), Peak);
	Result->SetNumberField(TEXT("first_nonzero_after_input_s"), RelativeSeconds(First));
	Result->SetNumberField(TEXT("last_above_energy_1e_minus20_after_input_s"), RelativeSeconds(Last));
	Result->SetNumberField(TEXT("energy90_arrival_after_input_s"), RelativeSeconds(Q90));
	Result->SetNumberField(TEXT("energy99_arrival_after_input_s"), RelativeSeconds(Q99));
	Result->SetNumberField(TEXT("energy_after_50ms_fraction"), Total > 0.0 ? Late50 / Total : 0.0);
	Result->SetNumberField(TEXT("energy_after_100ms_fraction"), Total > 0.0 ? Late100 / Total : 0.0);
	Result->SetNumberField(TEXT("energy_after_250ms_fraction"), Total > 0.0 ? Late250 / Total : 0.0);
	return Result;
}

bool SaveFloatStereo(const FString& Path, const TArray<float>& Samples)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Samples.GetData()), Samples.Num() * sizeof(float));
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

bool IMCaptureBakedReverb(FAutomationTestBase& Test, bool bLiveAuditionListener)
{
	const TCHAR* TargetMap = bLiveAuditionListener ? TEXT("/IceMoonAcousticField/Tests/IM_V2Audition")
		: TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || (bLiveAuditionListener ? !GEditor->PlayWorld : GEditor->PlayWorld != nullptr)
		|| World->GetOutermost()->GetName() != TargetMap)
	{
		Test.AddError(TEXT("The read-only diagnostic requires its saved Editor map and the requested idle/live-listener state."));
		return false;
	}
	AIMAcousticBakeVolume* Volume = nullptr;
	APlayerStart* Start = nullptr;
	AAmbientSound* Ambient = nullptr;
	int32 Volumes = 0, Starts = 0, Sources = 0;
	for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It) { Volume = *It; ++Volumes; }
	for (TActorIterator<APlayerStart> It(World); It; ++It) { Start = *It; ++Starts; }
	for (TActorIterator<AAmbientSound> It(World); It; ++It)
		if (It->ActorHasTag(bLiveAuditionListener ? TEXT("IMAcousticAuditionReferenceV1")
			: TEXT("IMAcousticRuralPreviewWaterDropV1"))) { Ambient = *It; ++Sources; }
	if (Volumes != 1 || Starts != 1 || Sources != 1 || !Volume->BakedField || !Ambient->GetAudioComponent())
	{
		Test.AddError(TEXT("The saved preview must have exactly one bound field, PlayerStart and tagged water-drop source."));
		return false;
	}
	auto* Asset = Volume->BakedField.Get();
	const FString MapFile = FPackageName::LongPackageNameToFilename(TargetMap, FPackageName::GetMapPackageExtension());
	const FString BakeFile = FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	const FString MapBefore = FileDigest(MapFile), BakeBefore = FileDigest(BakeFile);
	FString Failure;
	TArray<uint8> RawScene, RawProbes;
	TArray<FVector4> Probes;
	FVector Origin;
	if (!Volume->ValidateCurrentBake(Failure)
		|| !Asset->ReadPayload(TargetMap, Asset->SceneFingerprint, RawScene, RawProbes, Failure)
		|| !Asset->GetProbePreview(TargetMap, Asset->SceneFingerprint, Probes, Origin, Failure))
	{
		Test.AddError(Failure);
		return false;
	}
	FIMAcousticBakeData Bake;
	Bake.Scene.assign(RawScene.GetData(), RawScene.GetData() + RawScene.Num());
	Bake.ProbeBatch.assign(RawProbes.GetData(), RawProbes.GetData() + RawProbes.Num());
	for (const auto& P : Probes) Bake.CoverageProbes.push_back({{float(P.X), float(P.Y), float(P.Z)}, float(P.W)});
	FTransform ListenerTransform = Start->GetActorTransform();
	if (bLiveAuditionListener)
	{
		UWorld* PlayWorld = GEditor->PlayWorld;
		FAudioDevice* Device = PlayWorld ? PlayWorld->GetAudioDeviceRaw() : nullptr;
		FTransform Extra;
		if (!Device || UWorld::RemovePIEPrefix(PlayWorld->GetOutermost()->GetName()) != TargetMap
			|| !Device->GetListenerTransform(0, ListenerTransform) || Device->GetListenerTransform(1, Extra))
		{ Test.AddError(TEXT("Exactly one actual PIE audio-device listener is required.")); return false; }
		int32 LiveSources = 0;
		for (TActorIterator<AAmbientSound> It(PlayWorld); It; ++It)
			if (It->ActorHasTag(TEXT("IMAcousticAuditionReferenceV1")))
			{
				++LiveSources;
				auto* Audio = It->GetAudioComponent();
				if (!Audio || !Audio->IsPlaying() || Audio->Sound != Ambient->GetAudioComponent()->Sound
					|| !Audio->GetComponentTransform().Equals(Ambient->GetAudioComponent()->GetComponentTransform(), .001))
				{ Test.AddError(TEXT("The held live source must match the saved source for this kernel observation.")); return false; }
			}
		if (LiveSources != 1) { Test.AddError(TEXT("The held live source was not unique.")); return false; }
		// Read only the live listener pose. Capture/validation uses the saved
		// Editor field, so this diagnostic never mutates the active PIE worker.
	}
	else
	{
		// Preserve R31's explicit neighboring-probe coverage control.
		ListenerTransform.AddToTranslation(FVector(-100.0, 0.0, 0.0));
	}
	const IPLCoordinateSpace3 Listener = ToSDKSpace(ListenerTransform, Origin);
	const IPLCoordinateSpace3 Source = ToSDKSpace(Ambient->GetAudioComponent()->GetComponentTransform(), Origin);
	const float DistanceM = float(FVector::Distance(ListenerTransform.GetLocation(), Ambient->GetAudioComponent()->GetComponentLocation()) * .01);
	const float SendGain = Ambient->GetAudioComponent()->VolumeMultiplier * IMAcousticRecipe::ReverbSendDistanceGain(DistanceM);
	std::string Error;
	FIMAcousticSimulation Simulation;
	// Match the product worker's explicit hybrid options before loading its Bake.
	if (!Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), Error))
	{ Test.AddError(UTF8_TO_TCHAR(Error.c_str())); return false; }
	if (!Simulation.Load(Bake, Rate, Block, Error)) { Test.AddError(UTF8_TO_TCHAR(Error.c_str())); return false; }
	FIMAcousticReverbSlot Slot;
	// This stack slot has one serial writer and one serial consumer; there is no
	// worker or audio-thread handoff in this auxiliary kernel diagnostic.
	Slot.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
	if (!Simulation.EvaluateReverb(Slot, Listener, Error)) { Test.AddError(UTF8_TO_TCHAR(Error.c_str())); return false; }
	Slot.State.store(EIMAcousticIRState::Ready, std::memory_order_release);
	Slot.State.store(EIMAcousticIRState::Reading, std::memory_order_release);
	FIMAcousticAudioFrame DirectFrame;
	if (!Simulation.Evaluate(1, 1, Source, Listener, DirectFrame, Error)) { Test.AddError(UTF8_TO_TCHAR(Error.c_str())); return false; }
	FIMSDKLease SDK;
	if (!SDK.Initialize()) { Test.AddError(TEXT("Exclusive diagnostic SDK/HRTF initialization failed.")); return false; }
	FIMAcousticReverbRenderer Wet;
	FIMAcousticAudioRenderer Direct;
	FIMAcousticAudioRenderer DirectAndPath;
	if (!Wet.Initialize(SDK.Context, SDK.HRTF, Rate, Block, Rate * int32(IMAcousticRecipe::ReverbSavedDurationS))
		|| !Direct.Initialize(SDK.Context, SDK.HRTF, Rate, Block)
		|| (bLiveAuditionListener && !DirectAndPath.Initialize(SDK.Context, SDK.HRTF, Rate, Block)))
	{
		Test.AddError(TEXT("Production render-kernel initialization failed.")); return false;
	}
	TArray<float> WetPcm, DirectPcm, PathPcm;
	WetPcm.SetNumZeroed(CaptureBlocks * Block * 2);
	DirectPcm.SetNumZeroed(CaptureBlocks * Block * 2);
	if(bLiveAuditionListener)PathPcm.SetNumZeroed(CaptureBlocks * Block * 2);
	float Input[Block]{};
	float WetInput[Block]{};
	for (int32 B = 0; B < CaptureBlocks; ++B)
	{
		FMemory::Memzero(Input, sizeof(Input));
		const int32 LocalImpulse = ImpulseFrame - B * Block;
		if (LocalImpulse >= 0 && LocalImpulse < Block) Input[LocalImpulse] = ImpulseAmplitude;
		for (int32 I = 0; I < Block; ++I) WetInput[I] = Input[I] * SendGain;
		if (!Wet.Render(WetInput, Block, Slot.Params, Listener, WetPcm.GetData() + B * Block * 2)
			|| !Direct.Render(Input, Block, DirectFrame, DirectPcm.GetData() + B * Block * 2, nullptr, nullptr, nullptr, 1u))
		{
			Test.AddError(TEXT("The production render kernels rejected the current baked IR or direct frame.")); return false;
		}
		if(bLiveAuditionListener && !DirectAndPath.Render(Input, Block, DirectFrame,
			PathPcm.GetData() + B * Block * 2, nullptr, nullptr, nullptr, 3u))
		{ Test.AddError(TEXT("The direct/path control renderer rejected the current frame.")); return false; }
		Slot.Applied = true;
	}
	// Keep this one-shot slot Reading through scope exit. The stack effects die
	// before Slot releases its source; this slot is never offered for reuse.
	for (float& Sample : WetPcm) Sample *= Volume->ReverbWetGain;
	// Independent effects receive identical input. Subtract their direct-only
	// output before gain to expose the path contribution without changing DSP.
	if(bLiveAuditionListener)for(int32 I=0;I<PathPcm.Num();++I)
		PathPcm[I]=(PathPcm[I]-DirectPcm[I])*Ambient->GetAudioComponent()->VolumeMultiplier;
	for (float& Sample : DirectPcm) Sample *= Ambient->GetAudioComponent()->VolumeMultiplier;
	const auto WetMetrics = Analyze(WetPcm), DirectMetrics = Analyze(DirectPcm);
	auto Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("purpose"), bLiveAuditionListener
		? TEXT("Saved-bake production-kernel impulse at the actual held PIE audio-device listener 0; separate SDK effects, not live MetaSound output or hearing acceptance.")
		: TEXT("Diagnose saved IR at PlayerStart minus 100 cm X with current gains and fixed source; auxiliary SDK-kernel evidence, not original-spawn, live MetaSound or listening acceptance."));
	Report->SetBoolField(TEXT("uses_actual_pie_audio_listener"), bLiveAuditionListener);
	Report->SetStringField(TEXT("world_package"), TargetMap);
	Report->SetStringField(TEXT("scene_fingerprint"), Asset->SceneFingerprint);
	Report->SetStringField(TEXT("map_sha1_before"), MapBefore);
	Report->SetStringField(TEXT("bake_sha1_before"), BakeBefore);
	Report->SetNumberField(TEXT("sample_rate_hz"), Rate);
	Report->SetNumberField(TEXT("block_frames"), Block);
	Report->SetNumberField(TEXT("impulse_frame"), ImpulseFrame);
	Report->SetNumberField(TEXT("impulse_amplitude"), ImpulseAmplitude);
	Report->SetNumberField(TEXT("capture_frames"), CaptureBlocks * Block);
	Report->SetNumberField(TEXT("capture_seconds"), double(CaptureBlocks * Block) / Rate);
	Report->SetStringField(TEXT("pcm_format"), TEXT("interleaved stereo IEEE float32 little-endian, current Win64 host"));
	Report->SetStringField(TEXT("numerical_tail_threshold"), TEXT("Per-frame stereo energy > 1e-20; this is not an audibility threshold."));
	Report->SetStringField(TEXT("listener_ue_cm"), ListenerTransform.GetLocation().ToString());
	Report->SetStringField(TEXT("original_playerstart_ue_cm"), Start->GetActorLocation().ToString());
	Report->SetStringField(TEXT("listener_control_delta_ue_cm"), (ListenerTransform.GetLocation()-Start->GetActorLocation()).ToString());
	Report->SetStringField(TEXT("source_audio_component_ue_cm"), Ambient->GetAudioComponent()->GetComponentLocation().ToString());
	Report->SetNumberField(TEXT("probe_count"), Probes.Num());
	Report->SetNumberField(TEXT("source_listener_distance_m"), DistanceM);
	Report->SetNumberField(TEXT("volume_multiplier"), Ambient->GetAudioComponent()->VolumeMultiplier);
	Report->SetNumberField(TEXT("reverb_send_gain"), SendGain);
	Report->SetNumberField(TEXT("reverb_wet_gain"), Volume->ReverbWetGain);
	Report->SetBoolField(TEXT("direct_frame_valid"), DirectFrame.DirectValid);
	Report->SetBoolField(TEXT("path_frame_valid"), DirectFrame.PathValid);
	Report->SetNumberField(TEXT("direct_distance_attenuation"), DirectFrame.Direct.distanceAttenuation);
	Report->SetNumberField(TEXT("direct_occlusion"), DirectFrame.Direct.occlusion);
	TArray<TSharedPtr<FJsonValue>> MaterialAudit;
	int32 MaskedMappings = 0;
	for (const auto& Mapping : Volume->Materials)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("material_path"), GetPathNameSafe(Mapping.Material.Get()));
		Entry->SetStringField(TEXT("three_band_absorption"), Mapping.Absorption.ToString());
		Entry->SetNumberField(TEXT("scattering"), Mapping.Scattering);
		if (Mapping.Material)
		{
			const bool Masked = Mapping.Material->GetBlendMode() == BLEND_Masked;
			Entry->SetNumberField(TEXT("blend_mode_enum"), int32(Mapping.Material->GetBlendMode()));
			Entry->SetBoolField(TEXT("masked"), Masked);
			Entry->SetBoolField(TEXT("two_sided"), Mapping.Material->IsTwoSided());
			MaskedMappings += Masked ? 1 : 0;
		}
		MaterialAudit.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Report->SetArrayField(TEXT("material_mappings_audit"), MaterialAudit);
	Report->SetNumberField(TEXT("masked_material_mapping_count"), MaskedMappings);
	Report->SetStringField(TEXT("geometry_mask_policy"), TEXT("Current export uses complete LOD0 triangles; it does not sample opacity masks. Material audit does not measure effective acoustic opacity."));
	Report->SetObjectField(TEXT("wet"), WetMetrics);
	Report->SetObjectField(TEXT("direct"), DirectMetrics);
	if(bLiveAuditionListener)Report->SetObjectField(TEXT("path"), Analyze(PathPcm));
	const double DirectEnergy = DirectMetrics->GetNumberField(TEXT("stereo_integrated_energy"));
	Report->SetNumberField(TEXT("wet_to_direct_energy_ratio"), DirectEnergy > 0.0 ? WetMetrics->GetNumberField(TEXT("stereo_integrated_energy")) / DirectEnergy : -1.0);
	const bool Stable = !MapBefore.IsEmpty() && !BakeBefore.IsEmpty()
		&& MapBefore == FileDigest(MapFile) && BakeBefore == FileDigest(BakeFile);
	Report->SetBoolField(TEXT("persistent_map_and_bake_unchanged"), Stable);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), bLiveAuditionListener
		? TEXT("AcousticV2/AuditionReverbDiagnostic") : TEXT("AcousticV2/RuralReverbDiagnostic"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString WetFile = FPaths::Combine(Directory, TEXT("wet-stereo-f32.bin"));
	const FString DirectFile = FPaths::Combine(Directory, TEXT("direct-stereo-f32.bin"));
	const bool PcmSaved = SaveFloatStereo(WetFile, WetPcm) && SaveFloatStereo(DirectFile, DirectPcm)
		&& (!bLiveAuditionListener || SaveFloatStereo(FPaths::Combine(Directory,TEXT("path-stereo-f32.bin")),PathPcm));
	Report->SetStringField(TEXT("wet_pcm_sha1"), FileDigest(WetFile));
	Report->SetStringField(TEXT("direct_pcm_sha1"), FileDigest(DirectFile));
	FString Text;
	const auto Writer = TJsonWriterFactory<>::Create(&Text);
	const bool Saved = PcmSaved && FJsonSerializer::Serialize(Report, Writer)
		&& FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Directory, TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!Stable || !Saved) { Test.AddError(TEXT("Diagnostic evidence save or persistent-identity check failed.")); return false; }
	UE_LOG(LogTemp, Display, TEXT("IMLogs RuralReverbDiagnostic captured=1 ratio=%.9g wet99_s=%.9g evidence=%s"),
		Report->GetNumberField(TEXT("wet_to_direct_energy_ratio")), WetMetrics->GetNumberField(TEXT("energy99_arrival_after_input_s")), *Directory);
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] PASS RuralReverbDiagnosticCapture"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS"));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralReverbDiagnosticTest,
	"IceMoon.AcousticField.Rural.DiagnoseBakedReverb",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticRuralReverbDiagnosticTest::RunTest(const FString&)
{
	return IMAcousticRuralReverbDiagnosticTestPrivate::IMCaptureBakedReverb(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticAuditionLiveListenerReverbDiagnosticTest,
	"IceMoon.AcousticField.Audition.DiagnoseBakedReverbAtLiveListener",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
class FIMAuditionLiveListenerCaptureCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAuditionLiveListenerCaptureCommand(FAutomationTestBase* InTest)
		: Test(InTest),Started(FPlatformTime::Seconds()) {}
	bool Update() override
	{
		const double Now=FPlatformTime::Seconds();
		if(bCaptured)
		{
			if(!GEditor->PlayWorld)return true;
			if(Now-CleanupStarted>5.){Test->AddError(TEXT("Owned diagnostic PIE cleanup timed out."));return true;}
			return false;
		}
		if(Now-Started>60.)
		{Test->AddError(TEXT("Owned diagnostic PIE startup timed out."));GUnrealEd->RequestEndPlayMap();return true;}
		if(!GEditor->PlayWorld)return false;
		if(PIEStarted==0.)PIEStarted=Now;
		if(Now-PIEStarted<5.)return false;
		IMAcousticRuralReverbDiagnosticTestPrivate::IMCaptureBakedReverb(*Test,true);
		GUnrealEd->RequestEndPlayMap();bCaptured=true;CleanupStarted=FPlatformTime::Seconds();
		return false;
	}
private:
	FAutomationTestBase* Test;
	double Started=0.,PIEStarted=0.,CleanupStarted=0.;
	bool bCaptured=false;
};
bool FIMAcousticAuditionLiveListenerReverbDiagnosticTest::RunTest(const FString&)
{
	if(!GUnrealEd){AddError(TEXT("The owned Editor is unavailable."));return false;}
	FString Error;
	// Automation stops any existing PIE before RunTest. Its supported map-load
	// entry queues ordinary PIE startup; the next latent command owns capture
	// and teardown after that queued work completes.
	GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/Tests/IM_V2Audition"),false,&Error);
	if(!Error.IsEmpty()){AddError(Error);return false;}
	ADD_LATENT_AUTOMATION_COMMAND(FIMAuditionLiveListenerCaptureCommand(this));
	return true;
}

#endif
