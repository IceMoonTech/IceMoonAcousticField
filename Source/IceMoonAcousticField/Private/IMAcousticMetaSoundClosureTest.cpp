#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSDKContext.h"
#include "IMAcousticReverbRenderer.h"
#include "Components/AudioComponent.h"
#include "GameFramework/PlayerController.h"
#include "AudioDevice.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticMetaSoundClosureTestPrivate
{
class FIMAcousticMetaSoundClosureCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticMetaSoundClosureCommand(FAutomationTestBase* InTest)
		: Test(InTest), Started(FPlatformTime::Seconds()) {}
	bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - Started > 195) return Finish(false, TEXT("Ordinary closure timed out before sufficient source samples."));
		UWorld* World = nullptr;
		for (const FWorldContext& C : GEngine->GetWorldContexts())
			if (C.WorldType == EWorldType::PIE && C.World()) { World = C.World(); break; }
		if (!World) return false;
		if (!Listener.IsValid())
		{
			Listener = World->GetFirstPlayerController();
			if (!Listener.IsValid()) return false;
			Listener->SetAudioListenerOverride(nullptr, FVector(750,300,150), FRotator::ZeroRotator);
			for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It) Volume = *It;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (!It->ActorHasTag(TEXT("IMAcousticAuditionReferenceV1"))) continue;
				if (auto* Marker = It->FindComponentByClass<UIMAcousticSourceComponent>()) Source = Marker->AudioComponent;
			}
		}
		if (!Context && World->GetAudioDeviceRaw()) Context = IMAcousticMetaSound::FindAcousticMetaSoundContext(World->GetAudioDeviceRaw()->DeviceID);
		// NullRHI omits the view update that normally forwards the controller's
		// listener override. Supply that same public device input explicitly;
		// all voice registration, simulation, bus and DSP remain product-owned.
		if (!FApp::CanEverRender() && World->GetAudioDeviceRaw())
			World->GetAudioDeviceRaw()->SetListener(World, 0,
				FTransform(FRotator::ZeroRotator, Stage == 0 ? FVector(750,300,150) : FVector(1350,300,150)), 0.f);
		if (!Context || !Source.IsValid() || !Volume.IsValid())
		{
			if (Now - Started > 20) return Finish(false, Volume.IsValid() ? Volume->Status : TEXT("Ordinary scene did not expose its volume/source/context."));
			return false;
		}
		if (Now - Started > 20 && Context->LastIRSequence.load() == 0)
			return Finish(false, TEXT("Ordinary graph has not consumed any IR: ") + Volume->Status);
		if (Volume->ReverbSubmix) return Finish(false, TEXT("Ordinary graph entry created legacy convolution submix."));
		if (!IMAcousticMetaSound::IsAcousticMetaSound(Source->Sound) || Source->bAllowSpatialization) return Finish(false, TEXT("Ordinary source is not the isolated graph DSP path."));
		const uint64 Frames = Context->SourceFrames.load(std::memory_order_acquire);
		if (Stage == 0 && Frames >= 3ull * 1352448)
		{
			if (!Source->IsPlaying()) return Finish(false, TEXT("Ordinary entry did not keep the full source playing."));
			AEnd = Frames;
			Listener->SetAudioListenerOverride(nullptr, FVector(1350,300,150), FRotator::ZeroRotator);
			const uint64 AudioId = Source->GetAudioComponentID();
			const uint64 WorldGeneration = Context->Pool->WorldGeneration;
			const uint64 VoiceGeneration = Context->Device->Voices[0]->LiveGeneration.load(std::memory_order_acquire);
			// The frozen payload carries the real A listener coordinates from
			// this fixture, but its source identity is intentionally invalid.
			Context->IdentityInjection.Request(1, AudioId + 1, WorldGeneration,
				VoiceGeneration, 900001, -2.0f, -1.7f, 5.45f);
			Stage = 1;
			UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundClosure move_B source_frame=%llu ir=%llu"), Frames, Context->LastIRSequence.load());
		}
		if (Stage == 1 && !IdentityMissingRequested
			&& Context->IdentityInjection.Probes[0].Observed.load(std::memory_order_acquire))
		{
			Context->IdentityInjection.Request(2, 0, 0, 0, 0, -2.0f, -1.7f, 5.45f);
			IdentityMissingRequested = true;
		}
		if (Stage == 1 && Frames >= 6ull * 1352448 + 48000)
		{
			StopFrame = Frames; StopSeconds = Now;
			Source->Stop(); // ordinary stop: environment remains alive to drain.
			Stage = 2;
		}
		if (Stage == 2 && Now - StopSeconds >= 4)
		{
			const bool IdentityPass = IdentityNegativePass();
			const bool Valid = IdentityPass && Context->Device->ReverbNonzeroBlocks.load() > 100
				&& Context->LastIRSequence.load() > 1 && Context->InvalidBlocks.load() == 0
				&& Context->DuplicateConsumers.load() == 0;
			if (Context->BusUnderruns.load() != 0) return Finish(false, TEXT("Bounded acoustic bus reader underrun."));
			return Finish(Valid, Valid ? TEXT("Raw graph PCM captured; alignment/position/continuity analysis required.")
				: IdentityPass ? TEXT("Graph output, IR, block or exclusive-consumer predicate failed.")
				: TEXT("Focused ordinary freeze-A-to-move-B identity negative control failed."));
		}
		return false;
	}
private:
	bool IdentityProbePass(const FIMAcousticIdentityInjectionProbe& Probe, uint32 ExpectedDetail) const
	{
		return Probe.Observed.load(std::memory_order_acquire)
			&& Probe.Rejected.load(std::memory_order_acquire)
			&& Probe.FiniteOutput.load(std::memory_order_acquire)
			&& Probe.RejectDetail.load(std::memory_order_acquire) == ExpectedDetail
			&& Probe.RejectedDelta.load(std::memory_order_acquire) == 1
			&& Probe.RenderedDelta.load(std::memory_order_acquire) == 0;
	}

	bool IdentityNegativePass() const
	{
		return Context.IsValid()
			&& IdentityProbePass(Context->IdentityInjection.Probes[0], 128u)
			&& IdentityProbePass(Context->IdentityInjection.Probes[1], 16u | 64u | 128u);
	}

	bool SaveTiming(double EndSeconds)
	{
		const auto& D = *Context->Device;
		FString Snapshots = TEXT("captured_s,submit_s,submitted,fail_code\n");
		const uint64 SN = FMath::Min<uint64>(D.SnapshotProbePushes.load(std::memory_order_acquire), FIMAcousticDeviceBridge::ProbeSnapshotCapacity);
		for (uint64 I = 0; I < SN && D.SnapshotDone[I].load(std::memory_order_acquire); ++I)
		{
			const auto& P = D.SnapshotProbes[I];
			if (P.CapturedSeconds > EndSeconds) break;
			Snapshots += FString::Printf(TEXT("%.9f,%.9f,%u,%u\n"), P.CapturedSeconds, P.SubmitSeconds, P.Submitted, P.FailCode);
		}
		FString Workers = TEXT("loop_s,snapshot_s,valid,reason,eval_start_s,eval_end_s,reverb_start_s,reverb_end_s,sequence,reverb_snapshot_s,wait_end_s\n");
		const uint64 WN = FMath::Min<uint64>(D.WorkerProbePushes.load(std::memory_order_acquire), FIMAcousticDeviceBridge::ProbeWorkerCapacity);
		for (uint64 I = 0; I < WN && D.WorkerDone[I].load(std::memory_order_acquire); ++I)
		{
			const auto& P = D.WorkerProbes[I];
			if (P.LoopStartSeconds > EndSeconds) break;
			Workers += FString::Printf(TEXT("%.9f,%.9f,%u,%u,%.9f,%.9f,%.9f,%.9f,%llu,%.9f,%.9f\n"),
				P.LoopStartSeconds, P.SnapshotCaptured, P.SnapValid, P.SnapReason, P.EvalStartSeconds, P.EvalEndSeconds,
				P.ReverbStartSeconds, P.ReverbEndSeconds, P.ReverbSequence, P.ReverbCaptured, P.WaitEndSeconds);
		}
		return FFileHelper::SaveStringToFile(Snapshots, *FPaths::Combine(Directory, TEXT("snapshots.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& FFileHelper::SaveStringToFile(Workers, *FPaths::Combine(Directory, TEXT("workers.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	bool SavePCM(const TCHAR* Name, const TArray<float>& Samples, uint32 Count)
	{
		if (!Count || Count > uint32(Samples.Num())) return false;
		return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(reinterpret_cast<const uint8*>(Samples.GetData()), Count * sizeof(float)),
			*FPaths::Combine(Directory, Name));
	}
	bool Finish(bool Pass, const FString& Reason)
	{
		Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/MetaSoundClosure/runtime")));
		IFileManager::Get().MakeDirectory(*Directory, true);
		FString EffectiveReason = Reason;
		if (Context)
		{
			const bool IdentityPass = IdentityNegativePass();
			Pass = IdentityPass && Pass;
			if (!IdentityPass) EffectiveReason += TEXT(" Focused identity negative control was not fail-closed.");
			const uint32 S = Context->CapturedSourceFrames.load(std::memory_order_acquire);
			const uint32 E = Context->CapturedEnvironmentFrames.load(std::memory_order_acquire);
			const uint32 N = Context->CapturedBlockCount.load(std::memory_order_acquire);
			Pass = SaveTiming(FPlatformTime::Seconds()) && Pass;
			Pass = SavePCM(TEXT("source.f32"), Context->CapturedSource, S) && Pass;
			Pass = SavePCM(TEXT("direct-path.f32"), Context->CapturedDry, S * 2) && Pass;
			Pass = SavePCM(TEXT("bus.f32"), Context->CapturedBus, E) && Pass;
			Pass = SavePCM(TEXT("wet.f32"), Context->CapturedWet, E * 2) && Pass;
			FString CSV = TEXT("frame,sequence,seconds,listener_x_m,listener_y_m,listener_z_m,fresh\n");
			for (uint32 I = 0; I < N; ++I)
			{
				const auto& B = Context->CapturedBlocks[I];
				// Only publish trace matching the captured PCM prefix. File IO
				// may stall the GT worker heartbeat after recording has ended.
				if (B.Frame >= E) break;
				CSV += FString::Printf(TEXT("%llu,%llu,%.9f,%.9g,%.9g,%.9g,%d\n"), B.Frame, B.Sequence, B.Seconds, B.ListenerX, B.ListenerY, B.ListenerZ, B.Fresh);
			}
			Pass = FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(Directory, TEXT("blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && Pass;
			FString SourceBlocks = TEXT("block,voice,callback_audio_id,result_audio_id,result_world_generation,result_voice_generation,result_sequence,listener_x_m,listener_y_m,listener_z_m,reject,reject_detail,fallback,input_energy,output_energy,rendered_at,rejected_at\n");
			const uint32 SourceBlockCount = FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire), Context->CapturedSourceBlocks.Num());
			for (uint32 I = 0; I < SourceBlockCount; ++I)
			{
				const auto& B = Context->CapturedSourceBlocks[I];
				SourceBlocks += FString::Printf(TEXT("%llu,%u,%llu,%llu,%llu,%llu,%llu,%.9g,%.9g,%.9g,%u,%u,%u,%.9g,%.9g,%llu,%llu\n"),
					B.Block, B.Voice, B.CallbackAudioComponentId, B.ResultAudioComponentId,
					B.ResultWorldGeneration, B.ResultVoiceGeneration, B.ResultSequence,
					B.ListenerX, B.ListenerY, B.ListenerZ, uint32(B.Reject), B.RejectDetail,
					B.Fallback, B.InputEnergy, B.OutputEnergy, B.RenderedAt, B.RejectedAt);
			}
			Pass = FFileHelper::SaveStringToFile(SourceBlocks, *FPaths::Combine(Directory, TEXT("source-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && Pass;
			FString JSON; auto W = TJsonWriterFactory<>::Create(&JSON); W->WriteObjectStart();
			W->WriteValue(TEXT("status"), Pass ? TEXT("CAPTURED_ANALYSIS_PENDING") : TEXT("FAIL"));
			W->WriteValue(TEXT("reason"), EffectiveReason); W->WriteValue(TEXT("sample_rate"), int32(Context->Device->SampleRate));
			W->WriteValue(TEXT("graph_frames"), int32(FIMAcousticMetaSoundContext::Frames));
			W->WriteValue(TEXT("headless_listener_adapter"), !FApp::CanEverRender());
			W->WriteValue(TEXT("source_frames"), double(S)); W->WriteValue(TEXT("environment_frames"), double(E));
			W->WriteValue(TEXT("a_end_source_frame"), double(AEnd)); W->WriteValue(TEXT("stop_source_frame"), double(StopFrame));
			W->WriteValue(TEXT("source_audio_id"), double(Source.IsValid() ? Source->GetAudioComponentID() : 0));
			W->WriteValue(TEXT("world_generation"), double(Context->Pool->WorldGeneration));
			W->WriteValue(TEXT("rendered_source_blocks"), double(Context->Device->RenderedBlocks.load()));
			W->WriteValue(TEXT("degraded_source_blocks"), double(Context->Device->RejectedBlocks.load()));
			W->WriteValue(TEXT("wet_nonzero_blocks"), double(Context->Device->ReverbNonzeroBlocks.load()));
			W->WriteValue(TEXT("no_ir_blocks"), double(Context->NoIRBlocks.load()));
			W->WriteValue(TEXT("invalid_blocks"), double(Context->InvalidBlocks.load()));
			W->WriteValue(TEXT("duplicate_consumers"), double(Context->DuplicateConsumers.load()));
			W->WriteValue(TEXT("last_ir_sequence"), double(Context->LastIRSequence.load()));
			W->WriteValue(TEXT("bus_prime_frames"), Context->BusPrimeFrames);
			W->WriteValue(TEXT("bus_underruns"), double(Context->BusUnderruns.load()));
			W->WriteValue(TEXT("bus_read_blocks"), double(Context->BusReadBlocks.load()));
			W->WriteValue(TEXT("bus_min_available"), Context->BusMinAvailable.load());
			W->WriteValue(TEXT("bus_max_available"), Context->BusMaxAvailable.load());
			W->WriteValue(TEXT("legacy_submix_created"), Volume.IsValid() && Volume->ReverbSubmix != nullptr);
			W->WriteValue(TEXT("dry_capture_component_gain_not_applied"), 0.7);
			W->WriteObjectStart(TEXT("identity_negative"));
			W->WriteValue(TEXT("status"), IdentityPass ? TEXT("PASS_FOCUSED_FAIL_CLOSED") : TEXT("FAIL_FOCUSED_FAIL_CLOSED"));
			auto WriteIdentityProbe = [&](const TCHAR* Name, const FIMAcousticIdentityInjectionProbe& Probe, uint32 ExpectedDetail)
			{
				W->WriteObjectStart(Name);
				W->WriteValue(TEXT("observed"), Probe.Observed.load(std::memory_order_acquire) != 0);
				W->WriteValue(TEXT("rejected"), Probe.Rejected.load(std::memory_order_acquire) != 0);
				W->WriteValue(TEXT("finite_output"), Probe.FiniteOutput.load(std::memory_order_acquire) != 0);
				W->WriteValue(TEXT("expected_reject_detail"), double(ExpectedDetail));
				W->WriteValue(TEXT("reject_detail"), double(Probe.RejectDetail.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("rendered_delta"), double(Probe.RenderedDelta.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("rejected_delta"), double(Probe.RejectedDelta.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("injected_audio_id"), double(Probe.InjectedAudioId.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("injected_world_generation"), double(Probe.InjectedWorldGeneration.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("injected_voice_generation"), double(Probe.InjectedVoiceGeneration.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("injected_sequence"), double(Probe.InjectedSequence.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("actual_audio_id"), double(Probe.ActualAudioId.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("actual_world_generation"), double(Probe.ActualWorldGeneration.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("actual_voice_generation"), double(Probe.ActualVoiceGeneration.load(std::memory_order_acquire)));
				W->WriteObjectStart(TEXT("frozen_listener_sdk_m"));
				W->WriteValue(TEXT("x"), Probe.FrozenListenerX.load(std::memory_order_acquire));
				W->WriteValue(TEXT("y"), Probe.FrozenListenerY.load(std::memory_order_acquire));
				W->WriteValue(TEXT("z"), Probe.FrozenListenerZ.load(std::memory_order_acquire));
				W->WriteObjectEnd();
				W->WriteObjectEnd();
			};
			WriteIdentityProbe(TEXT("bad_audio_id"), Context->IdentityInjection.Probes[0], 128u);
			WriteIdentityProbe(TEXT("missing_fields"), Context->IdentityInjection.Probes[1], 16u | 64u | 128u);
			W->WriteObjectEnd();
			W->WriteObjectEnd(); W->Close();
			FFileHelper::SaveStringToFile(JSON, *FPaths::Combine(Directory, TEXT("capture.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		if (!Pass) Test->AddError(EffectiveReason);
		IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s MetaSoundClosure capture %s"), Pass ? TEXT("PASS") : TEXT("FAIL"), *EffectiveReason);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] MetaSoundClosure capture %s"), Pass ? TEXT("PASS") : TEXT("FAIL"));
		if (GEditor && GEditor->PlayWorld) GEditor->RequestEndPlayMap();
		return true;
	}
	FAutomationTestBase* Test;
	double Started = 0, StopSeconds = 0;
	uint64 AEnd = 0, StopFrame = 0;
	int32 Stage = 0;
	FString Directory;
	FIMAcousticMetaSoundContextPtr Context;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<UAudioComponent> Source;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	bool IdentityMissingRequested = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticMetaSoundClosure, "IceMoon.AcousticField.MetaSound.OrdinaryClosure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMAcousticMetaSoundClosure::RunTest(const FString&)
{
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true);
	FApp::SetUnfocusedVolumeMultiplier(1.f);
	GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
	GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
	FString Error;
	GUnrealEd->AutomationLoadMap(TEXT("/IceMoonAcousticField/Tests/IM_V2Audition"), false, &Error);
	if (!Error.IsEmpty()) { AddError(Error); IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false); return false; }
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticMetaSoundClosureTestPrivate::FIMAcousticMetaSoundClosureCommand(this));
	return true;
}
#endif
