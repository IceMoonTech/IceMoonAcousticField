#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "AudioDevice.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticCoordinates.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticReverbRenderer.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticSimulation.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace IMAcousticListenerResponseReplayTestPrivate
{
DEFINE_LOG_CATEGORY_STATIC(LogIMAcousticListenerResponse, Log, All);

constexpr int32 SampleRate = 48000;
constexpr int32 BlockFrames = 512;
constexpr float WetGain = 0.25f;
const TCHAR* OutputLeaf = TEXT("AcousticOrientationC31_20261012T023600Z");
const TCHAR* MapPath = TEXT("/IceMoonAcousticField/Tests/IM_FootstepAudition");

FString OutputDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CodexRuntime"), OutputLeaf));
}

bool SaveJson(const TCHAR* Name, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *FPaths::Combine(OutputDirectory(), Name),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool LoadJson(const TCHAR* Name, TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	return FFileHelper::LoadFileToString(Text, *FPaths::Combine(OutputDirectory(), Name))
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
}

TSharedRef<FJsonValue> VectorJson(const IPLVector3& Value)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Add(MakeShared<FJsonValueNumber>(Value.x));
	Values.Add(MakeShared<FJsonValueNumber>(Value.y));
	Values.Add(MakeShared<FJsonValueNumber>(Value.z));
	return MakeShared<FJsonValueArray>(Values);
}

TSharedRef<FJsonObject> SpaceJson(const IPLCoordinateSpace3& Space)
{
	auto Object = MakeShared<FJsonObject>();
	Object->SetField(TEXT("origin"), VectorJson(Space.origin));
	Object->SetField(TEXT("right"), VectorJson(Space.right));
	Object->SetField(TEXT("up"), VectorJson(Space.up));
	Object->SetField(TEXT("ahead"), VectorJson(Space.ahead));
	return Object;
}

bool ReadVector(const FJsonObject& Object, const TCHAR* Name, IPLVector3& Value)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.TryGetArrayField(Name, Values) || Values->Num() != 3) return false;
	Value = {float((*Values)[0]->AsNumber()), float((*Values)[1]->AsNumber()), float((*Values)[2]->AsNumber())};
	return FMath::IsFinite(Value.x) && FMath::IsFinite(Value.y) && FMath::IsFinite(Value.z);
}

bool ReadSpace(const FJsonObject& Object, IPLCoordinateSpace3& Space)
{
	return ReadVector(Object, TEXT("origin"), Space.origin) && ReadVector(Object, TEXT("right"), Space.right)
		&& ReadVector(Object, TEXT("up"), Space.up) && ReadVector(Object, TEXT("ahead"), Space.ahead);
}

bool SameVector(const IPLVector3& Left, const IPLVector3& Right)
{
	return FMath::Abs(Left.x - Right.x) < 0.00001f && FMath::Abs(Left.y - Right.y) < 0.00001f
		&& FMath::Abs(Left.z - Right.z) < 0.00001f;
}

bool SavePCM(const TCHAR* Name, const TArray<float>& Samples)
{
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Samples.Num() * int32(sizeof(float)));
	FMemory::Memcpy(Bytes.GetData(), Samples.GetData(), Bytes.Num());
	return FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(OutputDirectory(), Name));
}

bool LoadPCM(const TCHAR* Name, TArray<float>& Samples)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(OutputDirectory(), Name))
		|| Bytes.IsEmpty() || Bytes.Num() % int32(sizeof(float))) return false;
	Samples.SetNumUninitialized(Bytes.Num() / int32(sizeof(float)));
	FMemory::Memcpy(Samples.GetData(), Bytes.GetData(), Bytes.Num());
	for (float Sample : Samples) if (!FMath::IsFinite(Sample)) return false;
	return true;
}

void ObserveListener(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() != 1 || (Args[0] != TEXT("A") && Args[0] != TEXT("B")) || !World
		|| World->WorldType != EWorldType::PIE || UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) != MapPath) return;
	FAudioDevice* Device = World->GetAudioDeviceRaw();
	FTransform Listener, Extra;
	if (!Device || !Device->GetListenerTransform(0, Listener) || Device->GetListenerTransform(1, Extra)) return;
	AIMAcousticBakeVolume* Volume = nullptr;
	for (TActorIterator<AIMAcousticBakeVolume> Iterator(World); Iterator; ++Iterator)
	{
		if (Volume) return;
		Volume = *Iterator;
	}
	if (!Volume || !Volume->BakedField) return;
	TArray<FVector4> Probes;
	FVector Origin;
	FString Error;
	if (!Volume->BakedField->GetProbePreview(Volume->BakedField->WorldPackage,
		Volume->BakedField->SceneFingerprint, Probes, Origin, Error)) return;
	const auto Context = IMAcousticMetaSound::FindAcousticMetaSoundContext(Device->DeviceID);
	if (!Context || Context->Stopped.load(std::memory_order_acquire)) return;
	const uint64 ListenerVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
	if (!ListenerVersion) return;
	const FString Name = FString::Printf(TEXT("IMC31Listener%s.json"), *Args[0]);
	if (FPaths::FileExists(FPaths::Combine(OutputDirectory(), Name))) return;
	auto Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("phase"), Args[0]);
	Report->SetStringField(TEXT("world"), World->GetPathName());
	Report->SetNumberField(TEXT("native_platform_seconds"), FPlatformTime::Seconds());
	Report->SetNumberField(TEXT("audio_device_id"), Device->DeviceID);
	Report->SetStringField(TEXT("listener_spatial_version"), LexToString(ListenerVersion));
	Report->SetBoolField(TEXT("native_pose_version_stable_during_observation"),
		ListenerVersion == Context->ListenerSpatialVersion.load(std::memory_order_acquire));
	Report->SetStringField(TEXT("bake"), Volume->BakedField->GetPathName());
	Report->SetStringField(TEXT("bake_payload_digest"), Volume->BakedField->PayloadDigest);
	Report->SetObjectField(TEXT("listener_sdk"), SpaceJson(ToSDKSpace(Listener, Origin)));
	Report->SetField(TEXT("listener_ue_cm"), VectorJson({float(Listener.GetLocation().X),
		float(Listener.GetLocation().Y), float(Listener.GetLocation().Z)}));
	Report->SetField(TEXT("bake_origin_ue_cm"), VectorJson({float(Origin.X), float(Origin.Y), float(Origin.Z)}));
	SaveJson(*Name, Report);
}

struct FIMAcousticImpulseOwner
{
	IPLReflectionEffect Effect = nullptr;
	~FIMAcousticImpulseOwner() { if (Effect) iplReflectionEffectRelease(&Effect); }
};

bool ExportImpulse(FIMAcousticSimulation& Simulation, const IPLCoordinateSpace3& Listener,
	const TCHAR* Name, TArray<float>& Samples, FString& Error)
{
	FIMAcousticReverbSlot Slot;
	Slot.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
	std::string SDKError;
	if (!Simulation.EvaluateReverb(Slot, Listener, SDKError))
	{
		Error = ANSI_TO_TCHAR(SDKError.c_str());
		return false;
	}
	FIMAcousticImpulseOwner Owner;
	IPLAudioSettings Audio{SampleRate, BlockFrames};
	IPLReflectionEffectSettings Settings{};
	Settings.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
	Settings.irSize = Slot.Params.irSize;
	Settings.numChannels = 4;
	if (Slot.Params.numChannels != 4 || iplReflectionEffectCreate(IMAcousticSDKContext::GetAcousticSDKContext(), &Audio,
		&Settings, &Owner.Effect) != IPL_STATUS_SUCCESS)
	{
		Error = TEXT("independent impulse effect creation failed");
		return false;
	}
	TArray<float> Mono;
	Mono.SetNumZeroed(BlockFrames);
	TArray<float> Channels[4];
	float* ChannelPointers[4];
	for (int32 Channel = 0; Channel < 4; ++Channel)
	{
		Channels[Channel].SetNumZeroed(BlockFrames);
		ChannelPointers[Channel] = Channels[Channel].GetData();
	}
	float* InputPointers[1] = {Mono.GetData()};
	IPLAudioBuffer Input{1, BlockFrames, InputPointers};
	IPLAudioBuffer Output{4, BlockFrames, ChannelPointers};
	iplReflectionEffectApply(Owner.Effect, &Slot.Params, &Input, &Output, nullptr);
	Slot.Applied = true;
	Mono[0] = 1.0f;
	IPLAudioEffectState Tail = iplReflectionEffectApply(Owner.Effect, &Slot.Params, &Input, &Output, nullptr);
	int32 Blocks = 0;
	do
	{
		for (int32 Frame = 0; Frame < BlockFrames; ++Frame)
		{
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				const float Sample = Channels[Channel][Frame];
				if (!FMath::IsFinite(Sample)) { Error = TEXT("nonfinite independent impulse response"); return false; }
				Samples.Add(Sample);
			}
		}
		++Blocks;
		if (Tail == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE) break;
		if (Blocks > FMath::DivideAndRoundUp(Slot.Params.irSize, BlockFrames) + 2)
		{
			Error = TEXT("independent impulse GetTail did not complete within IR length");
			return false;
		}
		Tail = iplReflectionEffectGetTail(Owner.Effect, &Output, nullptr);
	} while (true);
	return SavePCM(Name, Samples);
}

bool LoadBake(const FJsonObject& Observation, FIMAcousticBakeData& Bake, FString& Error)
{
	UIMAcousticBakeAsset* Asset = LoadObject<UIMAcousticBakeAsset>(nullptr, *Observation.GetStringField(TEXT("bake")));
	if (!Asset || Asset->PayloadDigest != Observation.GetStringField(TEXT("bake_payload_digest")))
	{
		Error = TEXT("actual ordinary bake identity/digest changed");
		return false;
	}
	TArray<uint8> Scene, Probes;
	TArray<FVector4> Preview;
	FVector Origin;
	if (!Asset->ReadPayload(Asset->WorldPackage, Asset->SceneFingerprint, Scene, Probes, Error)
		|| !Asset->GetProbePreview(Asset->WorldPackage, Asset->SceneFingerprint, Preview, Origin, Error)) return false;
	Bake.Scene.assign(Scene.GetData(), Scene.GetData() + Scene.Num());
	Bake.ProbeBatch.assign(Probes.GetData(), Probes.GetData() + Probes.Num());
	for (const FVector4& Probe : Preview)
		Bake.CoverageProbes.push_back({{float(Probe.X), float(Probe.Y), float(Probe.Z)}, float(Probe.W)});
	return !Bake.Scene.empty() && !Bake.ProbeBatch.empty() && !Bake.CoverageProbes.empty();
}

struct FIMAcousticReplayHRTFOwner
{
	IPLHRTF HRTF = nullptr;
	~FIMAcousticReplayHRTFOwner() { if (HRTF) iplHRTFRelease(&HRTF); }
};

bool ReplayBlocks(const FJsonObject& Environment, const IPLCoordinateSpace3& ListenerA,
	const IPLCoordinateSpace3& ListenerB, uint64 ListenerVersionA, uint64 ListenerVersionB, FIMAcousticSimulation& Simulation,
	TSharedRef<FJsonObject>& Report, FString& Error)
{
	TArray<float> Bus, ActualWet;
	if (!LoadPCM(TEXT("Environment/bus_native_f32le.pcm"), Bus)
		|| !LoadPCM(TEXT("Environment/wet_native_f32le.pcm"), ActualWet))
	{
		Error = TEXT("original native Bus/Wet PCM unavailable or invalid");
		return false;
	}
	const auto& Rows = Environment.GetArrayField(TEXT("environment_rows"));
	if (Bus.Num() != Rows.Num() * BlockFrames || ActualWet.Num() != Bus.Num() * 2)
	{
		Error = TEXT("original PCM/block origin lengths differ");
		return false;
	}
	FIMAcousticReverbSlot CurrentSlot, FixedSlot;
	FIMAcousticReplayHRTFOwner HRTFOwner;
	IPLAudioSettings Audio{SampleRate, BlockFrames};
	IPLHRTFSettings HRTFSettings{};
	HRTFSettings.type = IPL_HRTFTYPE_DEFAULT;
	HRTFSettings.volume = 1.0f;
	if (iplHRTFCreate(IMAcousticSDKContext::GetAcousticSDKContext(), &Audio, &HRTFSettings, &HRTFOwner.HRTF) != IPL_STATUS_SUCCESS)
	{
		Error = TEXT("independent replay HRTF creation failed");
		return false;
	}
	FIMAcousticReverbRenderer CurrentRenderer, FixedRenderer;
	if (!CurrentRenderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTFOwner.HRTF, SampleRate, BlockFrames, SampleRate * 2)
		|| !FixedRenderer.Initialize(IMAcousticSDKContext::GetAcousticSDKContext(), HRTFOwner.HRTF, SampleRate, BlockFrames, SampleRate * 2))
	{
		Error = TEXT("independent replay renderer initialization failed");
		return false;
	}
	TArray<float> CurrentWet, FixedWet;
	CurrentWet.SetNumZeroed(ActualWet.Num());
	FixedWet.SetNumZeroed(ActualWet.Num());
	TArray<TSharedPtr<FJsonValue>> Updates;
	uint64 CurrentSequence = 0;
	int32 RenderedBlocks = 0, ChangedListenerBlocks = 0;
	double ActualEnergy = 0.0, CurrentErrorEnergy = 0.0, FixedErrorEnergy = 0.0;
	double CurrentMaximumError = 0.0, FixedMaximumError = 0.0;
	for (int32 Ordinal = 0; Ordinal < Rows.Num(); ++Ordinal)
	{
		const auto Row = Rows[Ordinal]->AsObject();
		if (!Row.IsValid() || int32(Row->GetNumberField(TEXT("ordinal"))) != Ordinal)
		{
			Error = TEXT("native environment ordinal is not the original PCM origin");
			return false;
		}
		const bool Rendered = Row->GetBoolField(TEXT("rendered"));
		if (Rendered)
		{
			IPLCoordinateSpace3 Listener = ListenerA;
			Listener.origin = {float(Row->GetNumberField(TEXT("listener_x_m"))),
				float(Row->GetNumberField(TEXT("listener_y_m"))), float(Row->GetNumberField(TEXT("listener_z_m")))};
			const uint64 SpatialVersion = FCString::Strtoui64(*Row->GetStringField(TEXT("spatial_version")), nullptr, 10);
			const bool Changed = SpatialVersion == ListenerVersionB;
			if ((!Changed && SpatialVersion != ListenerVersionA)
				|| !SameVector(Listener.origin, Changed ? ListenerB.origin : ListenerA.origin))
			{
				Error = FString::Printf(TEXT("unobserved actual listener orientation at native block %d"), Ordinal);
				return false;
			}
			Listener = Changed ? ListenerB : ListenerA;
			const uint64 Sequence = FCString::Strtoui64(*Row->GetStringField(TEXT("sequence")), nullptr, 10);
			if (!Sequence || !Row->GetBoolField(TEXT("fresh")))
			{
				Error = TEXT("rendered native block has no fresh actual IR identity");
				return false;
			}
			if (Sequence != CurrentSequence)
			{
				std::string SDKError;
				CurrentSlot.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
				FixedSlot.State.store(EIMAcousticIRState::Writing, std::memory_order_release);
				if (!Simulation.EvaluateReverb(CurrentSlot, Listener, SDKError)
					|| !Simulation.EvaluateReverb(FixedSlot, ListenerA, SDKError))
				{
					Error = FString::Printf(TEXT("independent evaluation at block %d: %s"), Ordinal, ANSI_TO_TCHAR(SDKError.c_str()));
					return false;
				}
				CurrentSequence = Sequence;
				auto Update = MakeShared<FJsonObject>();
				Update->SetNumberField(TEXT("native_ordinal"), Ordinal);
				Update->SetStringField(TEXT("native_sequence"), Row->GetStringField(TEXT("sequence")));
				Update->SetStringField(TEXT("native_spatial_version"), Row->GetStringField(TEXT("spatial_version")));
				Update->SetNumberField(TEXT("native_captured_seconds"), Row->GetNumberField(TEXT("captured_seconds")));
				Update->SetObjectField(TEXT("recomputed_listener"), SpaceJson(Listener));
				Update->SetNumberField(TEXT("ir_channels"), CurrentSlot.Params.numChannels);
				Update->SetNumberField(TEXT("ir_frames"), CurrentSlot.Params.irSize);
				Updates.Add(MakeShared<FJsonValueObject>(Update));
			}
			if (!CurrentRenderer.Render(Bus.GetData() + Ordinal * BlockFrames, BlockFrames,
				CurrentSlot.Params, Listener, CurrentWet.GetData() + Ordinal * BlockFrames * 2)
				|| !FixedRenderer.Render(Bus.GetData() + Ordinal * BlockFrames, BlockFrames,
					FixedSlot.Params, ListenerA, FixedWet.GetData() + Ordinal * BlockFrames * 2))
			{
				Error = TEXT("independent replay Render rejected a native successful block");
				return false;
			}
			CurrentSlot.Applied = true;
			FixedSlot.Applied = true;
			++RenderedBlocks;
			if (Changed) ++ChangedListenerBlocks;
			for (int32 Sample = Ordinal * BlockFrames * 2; Sample < (Ordinal + 1) * BlockFrames * 2; ++Sample)
			{
				CurrentWet[Sample] *= WetGain;
				FixedWet[Sample] *= WetGain;
			}
		}
		else if (!Row->GetBoolField(TEXT("delay_priming")))
		{
			CurrentRenderer.Reset();
			FixedRenderer.Reset();
		}
		for (int32 Sample = Ordinal * BlockFrames * 2; Sample < (Ordinal + 1) * BlockFrames * 2; ++Sample)
		{
			const double CurrentDifference = double(CurrentWet[Sample]) - ActualWet[Sample];
			const double FixedDifference = double(FixedWet[Sample]) - ActualWet[Sample];
			ActualEnergy += double(ActualWet[Sample]) * ActualWet[Sample];
			CurrentErrorEnergy += CurrentDifference * CurrentDifference;
			FixedErrorEnergy += FixedDifference * FixedDifference;
			CurrentMaximumError = FMath::Max(CurrentMaximumError, FMath::Abs(CurrentDifference));
			FixedMaximumError = FMath::Max(FixedMaximumError, FMath::Abs(FixedDifference));
		}
	}
	Report->SetNumberField(TEXT("native_blocks"), Rows.Num());
	Report->SetNumberField(TEXT("rendered_blocks"), RenderedBlocks);
	Report->SetNumberField(TEXT("changed_listener_rendered_blocks"), ChangedListenerBlocks);
	Report->SetNumberField(TEXT("actual_wet_energy"), ActualEnergy);
	Report->SetNumberField(TEXT("matching_replay_error_energy"), CurrentErrorEnergy);
	Report->SetNumberField(TEXT("fixed_a_replay_error_energy"), FixedErrorEnergy);
	Report->SetNumberField(TEXT("matching_replay_max_absolute_error"), CurrentMaximumError);
	Report->SetNumberField(TEXT("fixed_a_replay_max_absolute_error"), FixedMaximumError);
	Report->SetArrayField(TEXT("independent_ir_updates"), Updates);
	return SavePCM(TEXT("IMC31MatchingReplayWet_f32le.pcm"), CurrentWet)
		&& SavePCM(TEXT("IMC31FixedAReplayWet_f32le.pcm"), FixedWet);
}

void ReplayAfterStop(const TArray<FString>& Args, UWorld* World)
{
	if (!Args.IsEmpty() || !World || World->WorldType != EWorldType::Editor
		|| World->GetOutermost()->GetName() != MapPath || FPaths::FileExists(FPaths::Combine(OutputDirectory(), TEXT("IMC31Replay.json")))) return;
	auto Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("work_id"), TEXT("MON-AC-CURRENT-UNFINISHED-ORDINARY-ACCEPTANCE-CONTINUATION-C31-20261012-0236"));
	Report->SetNumberField(TEXT("sample_rate"), SampleRate);
	Report->SetNumberField(TEXT("block_frames"), BlockFrames);
	Report->SetNumberField(TEXT("wet_gain"), WetGain);
	Report->SetBoolField(TEXT("production_ir_handle_borrowed"), false);
	Report->SetBoolField(TEXT("test_impulse_entered_master"), false);
	Report->SetBoolField(TEXT("same_original_bus_and_block_history"), false);
	Report->SetBoolField(TEXT("opaque_ir_layout_interpreted"), false);
	const double Started = FPlatformTime::Seconds();
	FString Error;
	TSharedPtr<FJsonObject> ObservationA, ObservationB, Environment;
	IPLCoordinateSpace3 ListenerA{}, ListenerB{};
	uint64 ListenerVersionA = 0, ListenerVersionB = 0;
	bool Complete = LoadJson(TEXT("IMC31ListenerA.json"), ObservationA)
		&& LoadJson(TEXT("IMC31ListenerB.json"), ObservationB)
		&& LoadJson(TEXT("Environment/environment_report.json"), Environment);
	if (Complete)
	{
		ListenerVersionA = FCString::Strtoui64(*ObservationA->GetStringField(TEXT("listener_spatial_version")), nullptr, 10);
		ListenerVersionB = FCString::Strtoui64(*ObservationB->GetStringField(TEXT("listener_spatial_version")), nullptr, 10);
		Complete = Environment->GetBoolField(TEXT("complete")) && Environment->GetBoolField(TEXT("context_stopped"))
			&& !Environment->GetBoolField(TEXT("environment_consumer"))
			&& ReadSpace(*ObservationA->GetObjectField(TEXT("listener_sdk")), ListenerA)
			&& ReadSpace(*ObservationB->GetObjectField(TEXT("listener_sdk")), ListenerB)
			&& ListenerVersionA && ListenerVersionB && ListenerVersionA != ListenerVersionB
			&& ObservationA->GetBoolField(TEXT("native_pose_version_stable_during_observation"))
			&& ObservationB->GetBoolField(TEXT("native_pose_version_stable_during_observation"))
			&& SameVector(ListenerA.origin, ListenerB.origin)
			&& (!SameVector(ListenerA.right, ListenerB.right) || !SameVector(ListenerA.ahead, ListenerB.ahead))
			&& ObservationA->GetStringField(TEXT("bake_payload_digest")) == ObservationB->GetStringField(TEXT("bake_payload_digest"));
	}
	if (!Complete) Error = TEXT("ordinary Stop/actual Listener A-B/changed orientation/stable position and pose version/PCM precondition missing");
	FIMAcousticBakeData Bake;
	FIMAcousticSimulation Simulation;
	std::string SDKError;
	if (Complete) Complete = LoadBake(*ObservationA, Bake, Error);
	if (Complete)
	{
		Complete = Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), SDKError)
			&& Simulation.Load(Bake, SampleRate, BlockFrames, SDKError);
		if (!Complete) Error = ANSI_TO_TCHAR(SDKError.c_str());
	}
	TArray<float> ImpulseA1, ImpulseA2, ImpulseB;
	if (Complete)
	{
		Complete = ExportImpulse(Simulation, ListenerA, TEXT("IMC31ImpulseA1_ambi_f32le.pcm"), ImpulseA1, Error)
			&& ExportImpulse(Simulation, ListenerA, TEXT("IMC31ImpulseA2_ambi_f32le.pcm"), ImpulseA2, Error)
			&& ExportImpulse(Simulation, ListenerB, TEXT("IMC31ImpulseB_ambi_f32le.pcm"), ImpulseB, Error);
	}
	Report->SetNumberField(TEXT("impulse_channels"), 4);
	Report->SetNumberField(TEXT("impulse_a1_frames"), ImpulseA1.Num() / 4);
	Report->SetNumberField(TEXT("impulse_a2_frames"), ImpulseA2.Num() / 4);
	Report->SetNumberField(TEXT("impulse_b_frames"), ImpulseB.Num() / 4);
	const bool ImpulsesExported = Complete && !ImpulseA1.IsEmpty() && !ImpulseA2.IsEmpty() && !ImpulseB.IsEmpty();
	Report->SetBoolField(TEXT("impulse_initial_crossfade_consumed_with_silent_block"), ImpulsesExported);
	Report->SetBoolField(TEXT("impulse_get_tail_public_api_used"), ImpulsesExported);
	if (Complete) Complete = ReplayBlocks(*Environment, ListenerA, ListenerB, ListenerVersionA, ListenerVersionB, Simulation, Report, Error);
	Report->SetBoolField(TEXT("same_original_bus_and_block_history"), Complete);
	Report->SetBoolField(TEXT("complete"), Complete);
	Report->SetStringField(TEXT("error"), Error);
	Report->SetNumberField(TEXT("offline_elapsed_seconds"), FPlatformTime::Seconds() - Started);
	SaveJson(TEXT("IMC31Replay.json"), Report);
	UE_LOG(LogIMAcousticListenerResponse, Log, TEXT("IMLogs ListenerResponseReplay complete=%d error=%s"), Complete ? 1 : 0, *Error);
}

FAutoConsoleCommandWithWorldAndArgs ObserveCommand(TEXT("IM.Acoustic.ListenerResponse.Observe"),
	TEXT("Record actual ordinary PIE listener A or B without overriding it."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ObserveListener));
FAutoConsoleCommandWithWorldAndArgs ReplayCommand(TEXT("IM.Acoustic.ListenerResponse.Replay"),
	TEXT("Replay original C31 native Bus after ordinary Stop using independent SDK-owned IRs and observed pose versions."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ReplayAfterStop));
}

#endif
