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
constexpr uint64 IMWaterDropLoopFrames = 1352448ull; // 28.176 s at 48 kHz.

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
				FTransform(FRotator::ZeroRotator, (Stage == 0 || Stage == 2 || ReturnListenerRequested)
					? FVector(750,300,150) : FVector(1350,300,150)), 0.f);
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
		if (Stage == 0 && Frames >= IMWaterDropLoopFrames)
		{
			if (!Source->IsPlaying()) return Finish(false, TEXT("Ordinary entry did not keep the full source playing."));
			ASpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
			BTransitionProbeStart = Context->CapturedSourceBlockCount.load(std::memory_order_acquire);
			Listener->SetAudioListenerOverride(nullptr, FVector(1350,300,150), FRotator::ZeroRotator);
			Stage = 1;
			UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundClosure move_B source_frame=%llu ir=%llu"), Frames, Context->LastIRSequence.load());
		}
		if (Stage == 1)
		{
			const uint64 LiveSpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
			if (!BSpatialVersion && LiveSpatialVersion > ASpatialVersion)
				BSpatialVersion = LiveSpatialVersion;
			if (BSpatialVersion && !BTransitionRecorded)
			{
				uint64 BSourceFrame = 0;
				if (FindAcceptedSourceFrameForVersion(BSpatialVersion, BTransitionProbeStart, BSourceFrame))
				{
					AEnd = BStart = BSourceFrame;
					BTransitionRecorded = true;
				}
			}
		}
		if (Stage == 1 && BTransitionRecorded && !SpatialStaleRequested)
		{
			const uint64 AudioId = Source->GetAudioComponentID();
			const uint64 WorldGeneration = Context->Pool->WorldGeneration;
			const uint64 VoiceGeneration = Context->Device->Voices[0]->LiveGeneration.load(std::memory_order_acquire);
			// Freeze the exact A version and inject it only after a valid B
			// result has reached the delayed output timeline.
			Context->IdentityInjection.Request(3, AudioId, WorldGeneration,
				VoiceGeneration, 900002, -2.0f, -1.7f, 5.45f, ASpatialVersion);
			SpatialStaleRequested = true;
		}
		if (Stage == 1 && SpatialStaleRequested && !IdentityAudioIdRequested
			&& Context->IdentityInjection.Probes[2].Observed.load(std::memory_order_acquire))
		{
			const uint64 SpatialVersion = BSpatialVersion;
			Context->IdentityInjection.Request(1, Source->GetAudioComponentID() + 1, Context->Pool->WorldGeneration,
				Context->Device->Voices[0]->LiveGeneration.load(std::memory_order_acquire), 900003,
				-2.0f, -1.7f, 5.45f, SpatialVersion);
			IdentityAudioIdRequested = true;
		}
		if (Stage == 1 && IdentityAudioIdRequested && !IdentityMissingRequested
			&& Context->IdentityInjection.Probes[0].Observed.load(std::memory_order_acquire))
		{
			const uint64 SpatialVersion = BSpatialVersion;
			Context->IdentityInjection.Request(2, 0, 0, 0, 0, -2.0f, -1.7f, 5.45f, SpatialVersion);
			IdentityMissingRequested = true;
		}
		const bool IdentityControlsComplete = SpatialStaleRequested && IdentityAudioIdRequested && IdentityMissingRequested
			&& Context->IdentityInjection.Probes[2].Observed.load(std::memory_order_acquire)
			&& Context->IdentityInjection.Probes[0].Observed.load(std::memory_order_acquire)
			&& Context->IdentityInjection.Probes[1].Observed.load(std::memory_order_acquire);
		if (Stage == 1 && IdentityControlsComplete && Frames >= 2ull * IMWaterDropLoopFrames
			&& !ReturnListenerRequested)
		{
			ReturnProbeStart = Context->CapturedSourceBlockCount.load(std::memory_order_acquire);
			Listener->SetAudioListenerOverride(nullptr, FVector(750,300,150), FRotator::ZeroRotator);
			ReturnListenerRequested = true;
		}
		if (Stage == 1 && ReturnListenerRequested)
		{
			const uint64 LiveSpatialVersion = Context->ListenerSpatialVersion.load(std::memory_order_acquire);
			if (!ReturnSpatialVersion && LiveSpatialVersion != BSpatialVersion)
				ReturnSpatialVersion = LiveSpatialVersion;
			if (ReturnSpatialVersion)
			{
				uint64 ReturnSourceFrame = 0;
				if (FindAcceptedSourceFrameForVersion(ReturnSpatialVersion, ReturnProbeStart, ReturnSourceFrame))
				{
					BEnd = AReturn = ReturnSourceFrame;
					Stage = 2;
					UE_LOG(LogTemp, Display, TEXT("IMLogs MetaSoundClosure move_A_return source_frame=%llu spatial_version=%llu"),
						AReturn, ReturnSpatialVersion);
				}
			}
		}
		if (Stage == 2 && Frames >= 3ull * IMWaterDropLoopFrames + FIMAcousticMetaSoundContext::SourceInputDelayFrames)
		{
			StopFrame = Frames; StopSeconds = Now;
			Source->Stop(); // ordinary stop: environment remains alive to drain.
			Stage = 3;
		}
		if (Stage == 3 && Now - StopSeconds >= 4)
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
	bool FindAcceptedSourceFrameForVersion(uint64 SpatialVersion, uint32 StartBlock, uint64& OutSourceFrame) const
	{
		if (!Context.IsValid() || !Source.IsValid()) return false;
		const uint64 AudioId = Source->GetAudioComponentID();
		const uint32 Count = FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire),
			uint32(Context->CapturedSourceBlocks.Num()));
		for (uint32 I = StartBlock; I < Count; ++I)
		{
			const FIMAcousticBlockProbe& Block = Context->CapturedSourceBlocks[I];
			if (Block.CallbackAudioComponentId != AudioId || Block.InputDelayPriming
				|| Block.Reject != EIMAcousticProbeReject::Accepted
				|| Block.OutputSpatialVersion != SpatialVersion
				|| Block.ResultSpatialVersion != SpatialVersion || !Block.ResultSequence)
			{
				continue;
			}
			const uint64 OutputFrameEnd = Block.Block * FIMAcousticMetaSoundContext::Frames;
			if (OutputFrameEnd < FIMAcousticMetaSoundContext::SourceInputDelayFrames) continue;
			OutSourceFrame = OutputFrameEnd - FIMAcousticMetaSoundContext::SourceInputDelayFrames;
			return true;
		}
		return false;
	}

	bool IdentityProbePass(const FIMAcousticIdentityInjectionProbe& Probe, uint32 ExpectedDetail,
		bool AllowAdditionalSpatialMismatch = false) const
	{
		const uint32 ActualDetail = Probe.RejectDetail.load(std::memory_order_acquire);
		const bool DetailPass = AllowAdditionalSpatialMismatch
			? (ActualDetail & ExpectedDetail) == ExpectedDetail
			: ActualDetail == ExpectedDetail;
		return Probe.Observed.load(std::memory_order_acquire)
			&& Probe.Rejected.load(std::memory_order_acquire)
			&& Probe.FiniteOutput.load(std::memory_order_acquire)
			&& DetailPass
			&& Probe.RejectedDelta.load(std::memory_order_acquire) == 1
			&& Probe.RenderedDelta.load(std::memory_order_acquire) == 0;
	}

	bool IdentityNegativePass() const
	{
		return Context.IsValid()
			&& IdentityProbePass(Context->IdentityInjection.Probes[0], 128u, true)
			&& IdentityProbePass(Context->IdentityInjection.Probes[1], 16u | 64u | 128u, true)
			&& IdentityProbePass(Context->IdentityInjection.Probes[2], 2048u);
	}

	bool SaveTiming(double EndSeconds)
	{
		const auto& D = *Context->Device;
		FString Snapshots = TEXT("captured_s,submit_s,submitted,fail_code,block,world_generation,spatial_version\n");
		const uint64 SN = FMath::Min<uint64>(D.SnapshotProbePushes.load(std::memory_order_acquire), FIMAcousticDeviceBridge::ProbeSnapshotCapacity);
		for (uint64 I = 0; I < SN && D.SnapshotDone[I].load(std::memory_order_acquire); ++I)
		{
			const auto& P = D.SnapshotProbes[I];
			if (P.CapturedSeconds > EndSeconds) break;
			Snapshots += FString::Printf(TEXT("%.9f,%.9f,%u,%u,%llu,%llu,%llu\n"), P.CapturedSeconds, P.SubmitSeconds, P.Submitted, P.FailCode, P.Block, P.WorldGeneration, P.SpatialVersion);
		}
		FString Workers = TEXT("loop_s,snapshot_s,valid,reason,eval_start_s,eval_end_s,reverb_start_s,reverb_end_s,sequence,reverb_snapshot_s,wait_end_s,block,world_generation,num_inputs,results_published,eval_ok,push_s,push_ok,first_voice,first_audio_id,first_voice_generation,first_sequence\n");
		const uint64 WN = FMath::Min<uint64>(D.WorkerProbePushes.load(std::memory_order_acquire), FIMAcousticDeviceBridge::ProbeWorkerCapacity);
		for (uint64 I = 0; I < WN && D.WorkerDone[I].load(std::memory_order_acquire); ++I)
		{
			const auto& P = D.WorkerProbes[I];
			if (P.LoopStartSeconds > EndSeconds) break;
			Workers += FString::Printf(TEXT("%.9f,%.9f,%u,%u,%.9f,%.9f,%.9f,%.9f,%llu,%.9f,%.9f,%llu,%llu,%u,%u,%u,%.9f,%u,%u,%llu,%llu,%llu\n"),
				P.LoopStartSeconds, P.SnapshotCaptured, P.SnapValid, P.SnapReason, P.EvalStartSeconds, P.EvalEndSeconds,
				P.ReverbStartSeconds, P.ReverbEndSeconds, P.ReverbSequence, P.ReverbCaptured, P.WaitEndSeconds,
				P.Block, P.WorldGeneration, P.NumInputs, P.ResultsPublished, P.EvalOk, P.PushSeconds, P.PushOk,
				P.FirstVoice, P.FirstAudioId, P.FirstVoiceGen, P.FirstSeq);
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
			FString CSV = TEXT("frame,sequence,spatial_version,seconds,listener_x_m,listener_y_m,listener_z_m,fresh,expected_spatial_version\n");
			for (uint32 I = 0; I < N; ++I)
			{
				const auto& B = Context->CapturedBlocks[I];
				// Only publish trace matching the captured PCM prefix. File IO
				// may stall the GT worker heartbeat after recording has ended.
				if (B.Frame >= E) break;
				CSV += FString::Printf(TEXT("%llu,%llu,%llu,%.9f,%.9g,%.9g,%.9g,%d,%llu\n"),
					B.Frame, B.Sequence, B.SpatialVersion, B.Seconds, B.ListenerX, B.ListenerY, B.ListenerZ,
					B.Fresh, B.ExpectedSpatialVersion);
			}
			Pass = FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(Directory, TEXT("blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && Pass;
			FString SourceBlocks = TEXT("block,voice,input_spatial_version,output_spatial_version,input_delay_priming,callback_audio_id,result_audio_id,result_world_generation,result_voice_generation,result_spatial_version,result_sequence,listener_x_m,listener_y_m,listener_z_m,reject,reject_detail,fallback,input_energy,output_energy,rendered_at,rejected_at,snapshot_captured_s,consumed_s,age_ms\n");
			const uint32 SourceBlockCount = FMath::Min<uint32>(Context->CapturedSourceBlockCount.load(std::memory_order_acquire), Context->CapturedSourceBlocks.Num());
			for (uint32 I = 0; I < SourceBlockCount; ++I)
			{
				const auto& B = Context->CapturedSourceBlocks[I];
				SourceBlocks += FString::Printf(TEXT("%llu,%u,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%.9g,%.9g,%.9g,%u,%u,%u,%.9g,%.9g,%llu,%llu,%.9f,%.9f,%.9f\n"),
					B.Block, B.Voice, B.InputSpatialVersion, B.OutputSpatialVersion, uint32(B.InputDelayPriming),
					B.CallbackAudioComponentId, B.ResultAudioComponentId,
					B.ResultWorldGeneration, B.ResultVoiceGeneration, B.ResultSpatialVersion, B.ResultSequence,
					B.ListenerX, B.ListenerY, B.ListenerZ, uint32(B.Reject), B.RejectDetail,
					B.Fallback, B.InputEnergy, B.OutputEnergy, B.RenderedAt, B.RejectedAt,
					B.SnapshotCaptured, B.ConsumedSeconds, B.AgeMs);
			}
			Pass = FFileHelper::SaveStringToFile(SourceBlocks, *FPaths::Combine(Directory, TEXT("source-blocks.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && Pass;
			FString JSON; auto W = TJsonWriterFactory<>::Create(&JSON); W->WriteObjectStart();
			W->WriteValue(TEXT("status"), Pass ? TEXT("CAPTURED_ANALYSIS_PENDING") : TEXT("FAIL"));
			W->WriteValue(TEXT("reason"), EffectiveReason); W->WriteValue(TEXT("sample_rate"), int32(Context->Device->SampleRate));
			W->WriteValue(TEXT("graph_frames"), int32(FIMAcousticMetaSoundContext::Frames));
			W->WriteValue(TEXT("source_input_delay_blocks"), int32(FIMAcousticMetaSoundContext::SourceInputDelayBlocks));
			W->WriteValue(TEXT("source_input_delay_frames"), int32(FIMAcousticMetaSoundContext::SourceInputDelayFrames));
			W->WriteValue(TEXT("source_input_delay_ms"), 1000.0 * FIMAcousticMetaSoundContext::SourceInputDelayFrames
				/ double(Context->Device->SampleRate));
			W->WriteValue(TEXT("source_delay_priming_blocks"), double(Context->SourceDelayPrimingBlocks.load()));
			W->WriteValue(TEXT("headless_listener_adapter"), !FApp::CanEverRender());
			W->WriteValue(TEXT("source_frames"), double(S)); W->WriteValue(TEXT("environment_frames"), double(E));
			W->WriteValue(TEXT("a_end_source_frame"), double(AEnd)); W->WriteValue(TEXT("stop_source_frame"), double(StopFrame));
			W->WriteValue(TEXT("source_stop_tail_drain_frames"), double(StopFrame > 3ull * IMWaterDropLoopFrames
				? StopFrame - 3ull * IMWaterDropLoopFrames : 0));
			W->WriteValue(TEXT("source_loop_frames"), double(IMWaterDropLoopFrames));
			W->WriteValue(TEXT("source_loop_seconds"), double(IMWaterDropLoopFrames) / double(Context->Device->SampleRate));
			W->WriteValue(TEXT("b_start_source_frame"), double(BStart));
			W->WriteValue(TEXT("b_end_source_frame"), double(BEnd));
			W->WriteValue(TEXT("a_return_source_frame"), double(AReturn));
			W->WriteValue(TEXT("a_b_a_complete"), BStart >= AEnd && BEnd >= BStart && AReturn == BEnd);
			W->WriteValue(TEXT("source_audio_id"), double(Source.IsValid() ? Source->GetAudioComponentID() : 0));
			W->WriteValue(TEXT("world_generation"), double(Context->Pool->WorldGeneration));
			W->WriteValue(TEXT("rendered_source_blocks"), double(Context->Device->RenderedBlocks.load()));
			W->WriteValue(TEXT("degraded_source_blocks"), double(Context->Device->RejectedBlocks.load()));
			W->WriteValue(TEXT("priming_missing_source_blocks"), double(Context->Device->PrimingMissingBlocks.load()));
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
				W->WriteValue(TEXT("injected_spatial_version"), double(Probe.InjectedSpatialVersion.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("actual_audio_id"), double(Probe.ActualAudioId.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("actual_world_generation"), double(Probe.ActualWorldGeneration.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("actual_voice_generation"), double(Probe.ActualVoiceGeneration.load(std::memory_order_acquire)));
				W->WriteValue(TEXT("actual_spatial_version"), double(Probe.ActualSpatialVersion.load(std::memory_order_acquire)));
				W->WriteObjectStart(TEXT("frozen_listener_sdk_m"));
				W->WriteValue(TEXT("x"), Probe.FrozenListenerX.load(std::memory_order_acquire));
				W->WriteValue(TEXT("y"), Probe.FrozenListenerY.load(std::memory_order_acquire));
				W->WriteValue(TEXT("z"), Probe.FrozenListenerZ.load(std::memory_order_acquire));
				W->WriteObjectEnd();
				W->WriteObjectEnd();
			};
			WriteIdentityProbe(TEXT("bad_audio_id"), Context->IdentityInjection.Probes[0], 128u);
			WriteIdentityProbe(TEXT("missing_fields"), Context->IdentityInjection.Probes[1], 16u | 64u | 128u);
			WriteIdentityProbe(TEXT("frozen_a_spatial_version"), Context->IdentityInjection.Probes[2], 2048u);
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
	uint64 AEnd = 0, BStart = 0, BEnd = 0, AReturn = 0, StopFrame = 0;
	uint64 ASpatialVersion = 0, BSpatialVersion = 0, ReturnSpatialVersion = 0;
	uint32 BTransitionProbeStart = 0, ReturnProbeStart = 0;
	int32 Stage = 0;
	FString Directory;
	FIMAcousticMetaSoundContextPtr Context;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<UAudioComponent> Source;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	bool SpatialStaleRequested = false, BTransitionRecorded = false, ReturnListenerRequested = false;
	bool IdentityAudioIdRequested = false;
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
