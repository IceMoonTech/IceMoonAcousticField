#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticTestSupport.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticCoordinates.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "CollisionQueryParams.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "LandscapeProxy.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AmbientSound.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

namespace IMAcousticRuralPIEOperationalTestPrivate
{
constexpr const TCHAR* RuralOperationalTargetMap = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* RuralSourceTag = TEXT("IMAcousticRuralPreviewWaterDropV1");
constexpr double MovementCm = 100.0;
constexpr double ListenerToleranceCm = 2.0;
constexpr double SourceSnapshotToleranceM = 0.03;
constexpr double PhaseTimeoutSeconds = 20.0;
constexpr double StartupTimeoutSeconds = 60.0;
constexpr double CleanupTimeoutSeconds = 5.0;
constexpr double MaxPlayerStartDistanceCm = 300.0;

TSharedPtr<FJsonObject> IMRuralPIEOperationalVectorToJson(const FVector& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	Object->SetNumberField(TEXT("z"), Value.Z);
	return Object;
}

TArray<TSharedPtr<FJsonValue>> IMRuralPIEOperationalStringsToJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Strings.Num());
	for (const FString& Value : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(Value));
	}
	return Values;
}

FString IMRuralPIEOperationalStripPIEPackagePrefix(FString PackageName)
{
	const int32 Slash = PackageName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const FString Directory = Slash == INDEX_NONE ? FString() : PackageName.Left(Slash + 1);
	FString Leaf = Slash == INDEX_NONE ? PackageName : PackageName.Mid(Slash + 1);
	if (Leaf.StartsWith(TEXT("UEDPIE_"), ESearchCase::CaseSensitive))
	{
		const int32 NumberEnd = Leaf.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 7);
		if (NumberEnd > 7 && Leaf.Mid(7, NumberEnd - 7).IsNumeric())
		{
			Leaf = Leaf.Mid(NumberEnd + 1);
		}
	}
	return Directory + Leaf;
}

FVector IMRuralPIEOperationalSnapshotListenerUE(const FIMAcousticSnapshotProbe& Snapshot)
{
	return FVector(Snapshot.ListenerUEX, Snapshot.ListenerUEY, Snapshot.ListenerUEZ);
}

FVector IMRuralPIEOperationalSnapshotSourceSDK(const FIMAcousticSnapshotProbe& Snapshot)
{
	return FVector(Snapshot.Source0X, Snapshot.Source0Y, Snapshot.Source0Z);
}

bool IMRuralPIEOperationalIsPointInsideBakeBounds(const UBoxComponent* Bounds, const FVector& WorldPoint)
{
	if (!IsValid(Bounds) || !Bounds->IsRegistered())
	{
		return false;
	}
	const FVector LocalPoint = Bounds->GetComponentTransform().InverseTransformPosition(WorldPoint);
	const FVector Extent = Bounds->GetUnscaledBoxExtent() + FVector(1.0);
	return FMath::Abs(LocalPoint.X) <= Extent.X
		&& FMath::Abs(LocalPoint.Y) <= Extent.Y
		&& FMath::Abs(LocalPoint.Z) <= Extent.Z;
}

class FIMAcousticRuralPIEOperationalCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMAcousticRuralPIEOperationalCommand(FAutomationTestBase* InTest, const int32 InStage = StageLoadMap,
		const bool bInObserveReverb = false, const bool bInFixedAudition = false, const bool bInMovementObservation = false)
		: Test(InTest), Stage(InStage), bObserveReverb(bInObserveReverb), bFixedAudition(bInFixedAudition), bMovementObservation(bInMovementObservation)
	{
		if (bFixedAudition)
		{
			TargetMapPackage = TEXT("/IceMoonAcousticField/Tests/IM_V2Audition");
			TargetSourceTag = TEXT("IMAcousticAuditionReferenceV1");
		}
		// AutomationLoadMap hands startup to a new latent command instance.
		// That command must own its own bounded deadline before its first poll;
		// the previous instance's deadline is not inherited.
		if (InStage == StageWaitForPIE)
		{
			StartupDeadline = FPlatformTime::Seconds() + StartupTimeoutSeconds;
		}
		if (bObserveReverb)
		{
			ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), bMovementObservation
				? TEXT("AcousticV2/AuditionMovementGraph") : (bFixedAudition
				? TEXT("AcousticV2/AuditionGraphReverb") : TEXT("AcousticV2/RuralGraphReverb")), RunId);
			ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
		}
	}

	bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		if (bFinishing)
		{
			return UpdateCleanup(Now);
		}

		if (Stage == StageLoadMap)
		{
			if (!GUnrealEd)
			{
				RequestFinish(false, TEXT("GUnrealEd is unavailable; the saved Rural map cannot be opened in the owned PIE test."), Now);
				return false;
			}
			FString Error;
			GUnrealEd->AutomationLoadMap(TargetMapPackage, false, &Error);
			if (!Error.IsEmpty())
			{
				RequestFinish(false, FString::Printf(TEXT("AutomationLoadMap failed: %s"), *Error), Now);
				return false;
			}

			// AutomationLoadMap queues PIE startup. Yield this command completely,
			// then poll from a new latent command after the queued startup work.
			Stage = StageWaitForPIE;
			StartupDeadline = Now + StartupTimeoutSeconds;
			ADD_LATENT_AUTOMATION_COMMAND(FIMAcousticRuralPIEOperationalCommand(Test, StageWaitForPIE, bObserveReverb, bFixedAudition, bMovementObservation));
			return true;
		}

		UWorld* World = FindTargetPIEWorld(Now);
		if (bFinishing)
		{
			return false;
		}
		if (!World)
		{
			return false;
		}

		if (Stage == StageWaitForPIE)
		{
			PIEWorld = World;
			Stage = StageWaitForOwners;
			StartupDeadline = Now + StartupTimeoutSeconds;
		}

		if (Stage == StageWaitForOwners)
		{
			if (!bSceneOwnersResolved && !ResolveSceneOwners(World, Now))
			{
				return false;
			}
			if (bFinishing)
			{
				return false;
			}
			if (!ResolvePlayerAndRuntime(World, Now))
			{
				return false;
			}
			if (bFinishing)
			{
				return false;
			}
			if (bObserveReverb)
			{
				GraphContext = IMAcousticMetaSound::FindAcousticMetaSoundContext(World->GetAudioDevice().GetDeviceID());
				if (!GraphContext || GraphContext->Device != Bridge || GraphContext->CapturedBus.IsEmpty()
					|| GraphContext->AudioIds[0].load(std::memory_order_acquire) != OriginalSourceAudioId)
				{
					RequestFinish(false, TEXT("Rural graph capture was not preallocated on the exact ordinary source/device context."), Now);
					return false;
				}
				if (bMovementObservation)
				{
					if (!Bridge.IsValid() || !Bridge->PreparePressureWindowProbes())
					{
						RequestFinish(false, TEXT("Optional movement pressure-window probe allocation failed on the exact PIE device."), Now);
						return false;
					}
					bPressureWindowPrepared = true;
					PressureWindowSnapshotCapacityUsed = Bridge->PressureSnapshots.MaxEntries;
					PressureWindowWorkerCapacityUsed = Bridge->PressureWorkers.MaxEntries;
					if (Bridge->PressureSnapshots.Pushes.load(std::memory_order_acquire) != 0
						|| Bridge->PressureSnapshots.Overflows.load(std::memory_order_acquire) != 0
						|| Bridge->PressureWorkers.Pushes.load(std::memory_order_acquire) != 0
						|| Bridge->PressureWorkers.Overflows.load(std::memory_order_acquire) != 0)
					{
						RequestFinish(false, TEXT("Fresh movement pressure-window logs were not empty before opening."), Now);
						return false;
					}
					if (!Bridge->OpenPressureWindowProbes())
					{
						RequestFinish(false, TEXT("Optional movement pressure-window probes could not be opened on the exact PIE device."), Now);
						return false;
					}
					bPressureWindowOpened = true;
					PressureWindowOpenedSeconds = FPlatformTime::Seconds();
				}
				BeginReverbObservation(0, Now);
			}
			else BeginListenerMoveOut(Now);
			return false;
		}
		if (bObserveReverb && Stage == StageReverbObservation) return UpdateReverbObservation(Now);

		if (Stage >= StageListenerMoveOut && Stage <= StageSourceMoveBack)
		{
			return UpdateMovementPhase(World, Now);
		}

		RequestFinish(false, TEXT("Rural PIE operational test reached an invalid state."), Now);
		return false;
	}

private:
	enum EIMStage : int32
	{
		StageLoadMap = 0,
		StageWaitForPIE = 1,
		StageWaitForOwners = 2,
		StageListenerMoveOut = 3,
			StageListenerMoveBack = 4,
			StageSourceMoveOut = 5,
			StageSourceMoveBack = 6,
			StageFinalize = 7,
			StageReverbObservation = 8,
		};

		void BeginReverbObservation(const int32 Index, const double Now)
	{
		ReverbPhase = Index;
		PhaseName = Index == 1 ? TEXT("neighbor_minus_100cm_x")
			: (Index == 0 ? TEXT("original_playerstart") : TEXT("return_original_playerstart"));
		if (bMovementObservation)
		{
			static const TCHAR* Labels[] = { TEXT("stationary_start"), TEXT("rotate_yaw_5deg"),
				TEXT("restore_rotation"), TEXT("translate_plus_100cm_x"), TEXT("restore_location"), TEXT("distance_16m_x") };
			check(Index >= 0 && Index < UE_ARRAY_COUNT(Labels));
			PhaseName = Labels[Index];
			if (Index > 0)
			{
				// Drive the real PIE camera via its controller/pawn. An audio-listener
				// override would bypass the startup/update path reported by the user.
				if (Index <= 2)
					Listener->SetControlRotation(MovementOriginalControlRotation + (Index == 1 ? FRotator(0, 5, 0) : FRotator::ZeroRotator));
				else
					PlayerPawn->SetActorLocation(MovementOriginalPawnLocation + MovementTargetLocation(Index) - OriginalListenerLocation, false);
				bCameraPoseTouched = true;
			}
		}
		else if (Index > 0)
		{
			Listener->SetAudioListenerOverride(nullptr, Index == 1 ? ListenerMoveTarget : OriginalListenerLocation,
				OriginalListenerRotation);
			bListenerOverrideTouched = true;
		}
		Stage = StageReverbObservation;
		bReverbPhaseArmed = false;
		// Fixed-room capture spans two complete 28.176 s loops without changing
		// listener/source pose, gains, graph or bake. Rural still observes 31 s per pose.
		PhaseDeadline = Now + (bMovementObservation ? 45.0 : (bFixedAudition ? 100.0 : 65.0));
	}

	bool UpdateReverbObservation(const double Now)
	{
		if (!SourceAudio.IsValid() || !SourceAudio->IsPlaying() || !GraphContext
			|| GraphContext->Stopped.load(std::memory_order_acquire))
		{
			RequestFinish(false, TEXT("Ordinary Rural graph/source stopped before the full-loop observation completed."), Now);
			return false;
		}
		FIMAcousticSnapshotProbe Snapshot{};
		uint64 SnapshotIndex = 0;
		const FVector Target = bMovementObservation ? MovementTargetLocation(ReverbPhase)
			: (ReverbPhase == 1 ? ListenerMoveTarget : OriginalListenerLocation);
		FTransform ActualAudioListener;
		const FRotator TargetRotation = OriginalListenerRotation + (bMovementObservation && ReverbPhase == 1 ? FRotator(0, 5, 0) : FRotator::ZeroRotator);
		const bool bCameraPoseMatches = !bMovementObservation || (PIEWorld.IsValid() && PIEWorld->GetAudioDeviceRaw()
			&& PIEWorld->GetAudioDeviceRaw()->GetListenerTransform(0, ActualAudioListener)
			&& FVector::Distance(ActualAudioListener.GetLocation(), Target) <= ListenerToleranceCm
			&& ActualAudioListener.GetRotation().Equals(TargetRotation.Quaternion(), 0.01));
		if (!bReverbPhaseArmed && bCameraPoseMatches && ReadLatestSnapshot(Snapshot, SnapshotIndex) && Snapshot.Submitted
			&& Snapshot.Source0AudioId == OriginalSourceAudioId
			&& FVector::Distance(IMRuralPIEOperationalSnapshotListenerUE(Snapshot), Target) <= ListenerToleranceCm
			&& FVector::Distance(IMRuralPIEOperationalSnapshotSourceSDK(Snapshot), OriginalSourceSDK) <= SourceSnapshotToleranceM
			&& (ReverbPhase == 0 || SnapshotIndex > ReverbPreviousSnapshotIndex))
		{
			ReverbPhaseObservation = MakeShared<FJsonObject>();
			ReverbPhaseObservation->SetStringField(TEXT("phase"), PhaseName);
			ReverbPhaseObservation->SetObjectField(TEXT("listener_ue_cm"), IMRuralPIEOperationalVectorToJson(Target));
			ReverbPhaseObservation->SetNumberField(TEXT("observed_spatial_version"), double(Snapshot.SpatialVersion));
			ReverbPhaseObservation->SetNumberField(TEXT("requested_observation_seconds"), bFixedAudition && !bMovementObservation ? 62.0 : 31.0);
			if (bMovementObservation)
			{
				ReverbPhaseObservation->SetStringField(TEXT("camera_control"), TEXT("real PIE controller/pawn; no audio listener override"));
				ReverbPhaseObservation->SetNumberField(TEXT("actual_listener_yaw"), ActualAudioListener.Rotator().Yaw);
				ReverbPhaseObservation->SetNumberField(TEXT("source_listener_distance_m"), FVector::Distance(OriginalSourceLocation, Target) * 0.01);
			}

			ReverbPhaseEnvironmentBegin = GraphContext->CapturedEnvironmentFrames.load(std::memory_order_acquire);
			ReverbPhaseSourceBegin = GraphContext->CapturedSourceFrames.load(std::memory_order_acquire);
			ReverbPhaseTraceBegin = GraphContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
			ReverbPhaseNoIRBegin = GraphContext->NoIRBlocks.load(std::memory_order_acquire);
			ReverbPhaseUnderrunBegin = GraphContext->BusUnderruns.load(std::memory_order_acquire);
			ReverbPhaseRejectedBegin = Bridge->ReverbRejectedBlocks.load(std::memory_order_acquire);
			ReverbPhaseIRBegin = GraphContext->LastIRSequence.load(std::memory_order_acquire);
			ReverbPreviousSnapshotIndex = SnapshotIndex;
			bReverbPhaseArmed = true;
		}
		const uint32 EnvironmentEnd = GraphContext->CapturedEnvironmentFrames.load(std::memory_order_acquire);
		const uint32 SourceEnd = GraphContext->CapturedSourceFrames.load(std::memory_order_acquire);
		const uint32 RequiredFrames = Bridge->SampleRate * (bFixedAudition && !bMovementObservation ? 62u : 31u);
		if (bReverbPhaseArmed && EnvironmentEnd - ReverbPhaseEnvironmentBegin >= RequiredFrames
			&& SourceEnd - ReverbPhaseSourceBegin >= RequiredFrames)
		{
			auto& Observation = ReverbPhaseObservation;
			Observation->SetNumberField(TEXT("environment_pcm_begin_frame"), ReverbPhaseEnvironmentBegin);
			Observation->SetNumberField(TEXT("environment_pcm_end_frame"), EnvironmentEnd);
			Observation->SetNumberField(TEXT("source_pcm_begin_frame"), ReverbPhaseSourceBegin);
			Observation->SetNumberField(TEXT("source_pcm_end_frame"), SourceEnd);
			Observation->SetNumberField(TEXT("environment_trace_begin_row"), ReverbPhaseTraceBegin);
			Observation->SetNumberField(TEXT("environment_trace_end_row"), GraphContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire));
			Observation->SetNumberField(TEXT("no_ir_blocks_delta"), double(GraphContext->NoIRBlocks.load(std::memory_order_acquire) - ReverbPhaseNoIRBegin));
			Observation->SetNumberField(TEXT("bus_underruns_delta"), double(GraphContext->BusUnderruns.load(std::memory_order_acquire) - ReverbPhaseUnderrunBegin));
			Observation->SetNumberField(TEXT("reverb_rejected_blocks_delta"), double(Bridge->ReverbRejectedBlocks.load(std::memory_order_acquire) - ReverbPhaseRejectedBegin));
			Observation->SetNumberField(TEXT("ir_sequence_begin"), double(ReverbPhaseIRBegin));
			Observation->SetNumberField(TEXT("ir_sequence_end"), double(GraphContext->LastIRSequence.load(std::memory_order_acquire)));
			ReverbPhaseObservations.Add(MakeShared<FJsonValueObject>(Observation));
			UE_LOG(LogTemp, Display, TEXT("IMLogs RuralGraphReverb phase=%s captured_env_frames=%u no_ir_delta=%llu"),
				*PhaseName, EnvironmentEnd - ReverbPhaseEnvironmentBegin,
				GraphContext->NoIRBlocks.load(std::memory_order_acquire) - ReverbPhaseNoIRBegin);
			if ((bMovementObservation && ReverbPhase < 5) || (!bFixedAudition && ReverbPhase < 2)) BeginReverbObservation(ReverbPhase + 1, Now);
			else RequestFinish(true, TEXT("Full-loop graph observations captured; acoustic acceptance remains separate."), Now);
		}
		else if (Now > PhaseDeadline)
		{
			RequestFinish(false, FString::Printf(TEXT("Rural graph observation timed out: phase=%s armed=%d env=%u source=%u."),
				*PhaseName, bReverbPhaseArmed ? 1 : 0, EnvironmentEnd, SourceEnd), Now);
		}
		return false;
	}

	bool ExportReverbObservation(TSharedRef<FJsonObject> Report)
	{
		if (!GraphContext || !IFileManager::Get().MakeDirectory(*ReportDirectory, true)) return false;
		const uint32 EnvironmentFrames = GraphContext->CapturedEnvironmentFrames.load(std::memory_order_acquire);
		const uint32 SourceFrames = GraphContext->CapturedSourceFrames.load(std::memory_order_acquire);
		const uint32 TraceCount = GraphContext->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
		const uint32 SourceTraceCount = GraphContext->CapturedSourceBlockCount.load(std::memory_order_acquire);
		// Each acquire count publishes an append-only prefix after the audio writer's memcpy.
		// Later callbacks write only beyond that prefix; Stopped alone is not a join.
		if (EnvironmentFrames >= uint32(GraphContext->CapturedBus.Num())
			|| SourceFrames >= uint32(GraphContext->CapturedSource.Num())
			|| TraceCount >= uint32(GraphContext->CapturedBlocks.Num())
			|| SourceTraceCount >= uint32(GraphContext->CapturedSourceBlocks.Num())) return false;
		Report->SetStringField(TEXT("scope"), bMovementObservation
			? TEXT("Saved IM_V2Audition ordinary source; real PIE camera stationary/rotation/translation/return/16m observations, 31 audio seconds each. Source/gains/graph/bake unchanged; CAPTURE_ONLY, no user-hearing verdict.") : bFixedAudition
			? TEXT("Ordinary saved IM_V2Audition source; 62 audio seconds at the actual default PIE listener, no pose/gain/graph/bake change. CAPTURE_ONLY; user hearing is separate.")
			: TEXT("Ordinary saved MetaSound source, one listener variable, 31 seconds at each pose. CAPTURE_ONLY; wet validity and user hearing are separate."));
		Report->SetStringField(TEXT("pcm_format"), TEXT("IEEE float32 little-endian; source/bus mono, source-output/wet interleaved stereo"));
		Report->SetStringField(TEXT("source_output_semantics"), TEXT("CapturedDry is the source graph output including approved direct/path routes; source input is pre-delay. Bus/wet share environment frame-zero; source frame-zero is independent."));
		Report->SetStringField(TEXT("no_ir_blocks_semantics"), TEXT("Counts both missing/not-fresh IR and failed reverb rendering; inspect Fresh trace plus reverb_rejected_blocks_delta and last successful IR sequence."));
		Report->SetNumberField(TEXT("sample_rate_hz"), Bridge->SampleRate);
		Report->SetNumberField(TEXT("environment_pcm_frames"), EnvironmentFrames);
		Report->SetNumberField(TEXT("source_pcm_frames"), SourceFrames);
		Report->SetNumberField(TEXT("environment_trace_rows"), TraceCount);
		Report->SetNumberField(TEXT("source_trace_rows"), SourceTraceCount);
		Report->SetNumberField(TEXT("bus_prime_frames"), GraphContext->BusPrimeFrames);
		Report->SetStringField(TEXT("bus_transport"), TEXT("graph_source_send_fifo_sum"));
		Report->SetNumberField(TEXT("source_send_capacity_frames"), GraphContext->SourceSendCapacityFrames);
		Report->SetBoolField(TEXT("source_send_transport_enabled"), GraphContext->SourceSendTransportEnabled);
		Report->SetBoolField(TEXT("source_send_transport_failed"), GraphContext->SourceSendTransportFailed.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("source_send_push_blocks_total"), double(GraphContext->SourceSendPushBlocks.load(std::memory_order_acquire)));
		Report->SetNumberField(TEXT("source_send_push_failures_total"), double(GraphContext->SourceSendPushFailures.load(std::memory_order_acquire)));
		Report->SetNumberField(TEXT("bus_underruns_total"), double(GraphContext->BusUnderruns.load(std::memory_order_acquire)));
		Report->SetNumberField(TEXT("bus_silent_blocks_total"), double(GraphContext->BusSilentBlocks.load(std::memory_order_acquire)));
		TArray<TSharedPtr<FJsonValue>> SourceSendContributions;
		for (uint32 Slot = 0; Slot < FIMAcousticMetaSoundContext::MaxVoices; ++Slot)
		{
			const uint64 AudioId = GraphContext->AudioIds[Slot].load(std::memory_order_acquire);
			if (!AudioId) continue;
			TSharedRef<FJsonObject> Contribution = MakeShared<FJsonObject>();
			Contribution->SetNumberField(TEXT("voice"), Slot);
			Contribution->SetNumberField(TEXT("audio_component_id"), double(AudioId));
			Contribution->SetNumberField(TEXT("pushed_blocks_total"), double(GraphContext->SourceSendBlockCounts[Slot].load(std::memory_order_acquire)));
			SourceSendContributions.Add(MakeShared<FJsonValueObject>(Contribution));
		}
		Report->SetArrayField(TEXT("source_send_contributions"), SourceSendContributions);
		Report->SetNumberField(TEXT("bus_min_available_frames"), GraphContext->BusMinAvailable.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("bus_max_available_frames"), GraphContext->BusMaxAvailable.load(std::memory_order_acquire));
		Report->SetNumberField(TEXT("bus_read_blocks"), double(GraphContext->BusReadBlocks.load(std::memory_order_acquire)));
		Report->SetNumberField(TEXT("bus_priming_blocks"), double(GraphContext->BusPrimingBlocks.load(std::memory_order_acquire)));
		Report->SetNumberField(TEXT("wet_gain_at_report"), GraphContext->WetGain.load(std::memory_order_acquire));
		Report->SetArrayField(TEXT("phases"), ReverbPhaseObservations);
		auto SavePCM = [&](const TCHAR* Name, const TArray<float>& Samples, const uint32 Count)
		{
			if (Count > uint32(Samples.Num())) return false;
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Samples.GetData()), int32(Count * sizeof(float)));
			FSHAHash Hash;
			FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
			Report->SetStringField(FString(Name) + TEXT("_sha1"), Hash.ToString());
			return FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(ReportDirectory, FString(Name) + TEXT(".bin")));
		};
		const bool bPCM = SavePCM(TEXT("source-mono-f32"), GraphContext->CapturedSource, SourceFrames)
			&& SavePCM(TEXT("source-output-stereo-f32"), GraphContext->CapturedDry, SourceFrames * 2u)
			&& SavePCM(TEXT("bus-mono-f32"), GraphContext->CapturedBus, EnvironmentFrames)
			&& SavePCM(TEXT("wet-stereo-f32"), GraphContext->CapturedWet, EnvironmentFrames * 2u);
		FString CSV = TEXT("frame,seconds,sequence,spatial_version,expected_spatial_version,listener_x,listener_y,listener_z,fresh\n");
		for (uint32 I = 0; I < TraceCount; ++I)
		{
			const auto& Row = GraphContext->CapturedBlocks[int32(I)];
			CSV += FString::Printf(TEXT("%llu,%.9f,%llu,%llu,%llu,%.9g,%.9g,%.9g,%d\n"), Row.Frame, Row.Seconds,
				Row.Sequence, Row.SpatialVersion, Row.ExpectedSpatialVersion, Row.ListenerX, Row.ListenerY, Row.ListenerZ, Row.Fresh ? 1 : 0);
		}
		const bool bEnvironmentCSV = FFileHelper::SaveStringToFile(CSV,
			*FPaths::Combine(ReportDirectory, TEXT("environment.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		CSV = TEXT("block,voice,seconds,audio_component_id,input_spatial_version,output_spatial_version,result_spatial_version,sequence,occlusion,distance_gain,direct_valid,path_valid,reject,reject_detail,input_energy,send_energy,direct_energy,path_energy,output_energy\n");
		for (uint32 I = 0; I < SourceTraceCount; ++I)
		{
			const auto& Row = GraphContext->CapturedSourceBlocks[int32(I)];
			CSV += FString::Printf(TEXT("%llu,%u,%.9f,%llu,%llu,%llu,%llu,%llu,%.9g,%.9g,%u,%u,%u,%u,%.17g,%.17g,%.17g,%.17g,%.17g\n"),
				Row.Block, Row.Voice, Row.ConsumedSeconds, Row.CallbackAudioComponentId, Row.InputSpatialVersion,
				Row.OutputSpatialVersion, Row.ResultSpatialVersion, Row.ResultSequence, Row.Occlusion, Row.DistanceGain,
				uint32(Row.DirectValid), uint32(Row.PathValid), uint32(Row.Reject), Row.RejectDetail,
				Row.InputEnergy, Row.ReverbSendEnergy, Row.DirectEnergy, Row.PathEnergy, Row.OutputEnergy);
		}
			const bool bSourceCSV = FFileHelper::SaveStringToFile(CSV,
				*FPaths::Combine(ReportDirectory, TEXT("source.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				// Geometry witnesses are a Rural-specific auxiliary replay. The room
				// diagnostic uses the existing preparation/fingerprint gate instead.
				Report->SetBoolField(TEXT("rural_geometry_witnesses_applicable"), !bFixedAudition);
				return bPCM && bEnvironmentCSV && bSourceCSV && ExportLeaseObservation(Report, SourceTraceCount)
					&& (bFixedAudition || RecordGeometryWitnesses(Report));
		}

		bool ExportLeaseObservation(TSharedRef<FJsonObject> Report, uint32 SourceTraceCount)
		{
			if (!Bridge || !GraphContext) return false;
			// Each producer release-publishes its own append-only prefix. Local
			// block ordinals are not an atomic GT/worker/audio join; use clocks and
			// world/audio-id/voice-generation/sequence identities in these files.
			const bool bUsePressureWindowWorkers = bMovementObservation;
			const uint64 SnapshotPushes = Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
			const uint64 SnapshotCapacity = Bridge->ProbeSnapshotCapacity;
			const uint64 SnapshotClaims = FMath::Min<uint64>(SnapshotPushes, SnapshotCapacity);
			const uint64 DefaultWorkerPushes = Bridge->WorkerProbePushes.load(std::memory_order_acquire);
			const uint64 DefaultWorkerCapacity = Bridge->ProbeWorkerCapacity;
			const uint64 DefaultWorkerClaims = FMath::Min<uint64>(DefaultWorkerPushes, DefaultWorkerCapacity);
			const uint64 PressureSnapshotCapacity = bPressureWindowPrepared ? PressureWindowSnapshotCapacityUsed : 0u;
			const uint64 PressureWorkerCapacity = bPressureWindowPrepared ? PressureWindowWorkerCapacityUsed : 0u;
			const uint64 PressureSnapshotPushes = bPressureWindowPrepared ? Bridge->PressureSnapshots.Pushes.load(std::memory_order_acquire) : 0u;
			const uint64 PressureWorkerPushes = bPressureWindowPrepared ? Bridge->PressureWorkers.Pushes.load(std::memory_order_acquire) : 0u;
			const uint64 PressureSnapshotClaims = FMath::Min<uint64>(PressureSnapshotPushes, PressureSnapshotCapacity);
			const uint64 PressureWorkerClaims = FMath::Min<uint64>(PressureWorkerPushes, PressureWorkerCapacity);
			uint64 SnapshotRows = 0, DefaultWorkerRows = 0, PressureSnapshotRows = 0, PressureWorkerRows = 0;
			while (SnapshotRows < SnapshotClaims && Bridge->SnapshotDone[SnapshotRows].load(std::memory_order_acquire) == SnapshotRows + 1) ++SnapshotRows;
			while (DefaultWorkerRows < DefaultWorkerClaims && Bridge->WorkerDone[DefaultWorkerRows].load(std::memory_order_acquire) == DefaultWorkerRows + 1) ++DefaultWorkerRows;
			while (bPressureWindowPrepared && PressureSnapshotRows < PressureSnapshotClaims
				&& Bridge->PressureSnapshots.Done[PressureSnapshotRows].load(std::memory_order_acquire) == PressureSnapshotRows + 1) ++PressureSnapshotRows;
			while (bPressureWindowPrepared && PressureWorkerRows < PressureWorkerClaims
				&& Bridge->PressureWorkers.Done[PressureWorkerRows].load(std::memory_order_acquire) == PressureWorkerRows + 1) ++PressureWorkerRows;
			const uint64 WorkerRows = bUsePressureWindowWorkers ? PressureWorkerRows : DefaultWorkerRows;
			const uint64 WorkerPushes = bUsePressureWindowWorkers ? PressureWorkerPushes : DefaultWorkerPushes;
			const uint64 WorkerCapacity = bUsePressureWindowWorkers ? PressureWorkerCapacity : DefaultWorkerCapacity;
			const uint64 WorkerClaims = bUsePressureWindowWorkers ? PressureWorkerClaims : DefaultWorkerClaims;
			const uint64 SnapshotOverflows = Bridge->SnapshotProbeOverflows.load(std::memory_order_acquire);
			const uint64 DefaultWorkerOverflows = Bridge->WorkerProbeOverflows.load(std::memory_order_acquire);
			const uint64 PressureSnapshotOverflows = bPressureWindowPrepared ? Bridge->PressureSnapshots.Overflows.load(std::memory_order_acquire) : 0u;
			const uint64 PressureWorkerOverflows = bPressureWindowPrepared ? Bridge->PressureWorkers.Overflows.load(std::memory_order_acquire) : 0u;
			const uint64 WorkerOverflows = bUsePressureWindowWorkers ? PressureWorkerOverflows : DefaultWorkerOverflows;
			const bool bSnapshotPrefixComplete = SnapshotRows == SnapshotClaims && SnapshotClaims == SnapshotPushes;
			const bool bDefaultWorkerPrefixComplete = DefaultWorkerRows == DefaultWorkerClaims && DefaultWorkerClaims == DefaultWorkerPushes;
			const bool bPressureSnapshotPrefixComplete = bPressureWindowPrepared
				&& PressureSnapshotRows == PressureSnapshotClaims && PressureSnapshotClaims == PressureSnapshotPushes;
			const bool bPressureWorkerPrefixComplete = bPressureWindowPrepared
				&& PressureWorkerRows == PressureWorkerClaims && PressureWorkerClaims == PressureWorkerPushes;
			const bool bPressureWindowLogPrefixesComplete = bPressureSnapshotPrefixComplete && bPressureWorkerPrefixComplete;
			const bool bPressureWindowLogsOverflowFree = PressureSnapshotOverflows == 0 && PressureWorkerOverflows == 0;
			const bool bWorkerPrefixComplete = WorkerRows == WorkerClaims && WorkerClaims == WorkerPushes;
			const bool bPressureWindowComplete = !bUsePressureWindowWorkers
				|| (bPressureWindowPrepared && bPressureWindowOpened && bPressureWindowClosed && bPressureWindowQuiescent
					&& bPressureWindowLogPrefixesComplete && bPressureWindowLogsOverflowFree);
			const bool bMovementCaptureComplete = !bMovementObservation
				|| (bSnapshotPrefixComplete && bWorkerPrefixComplete && bPressureWindowComplete);
			const bool bLeaseObservationAvailable = SnapshotRows > 0 && WorkerRows > 0 && SourceTraceCount > 0
				&& SnapshotOverflows == 0 && WorkerOverflows == 0 && bMovementCaptureComplete;
			Report->SetStringField(TEXT("lease_observation_schema"), TEXT("gt-worker-source-lease-v1"));
			Report->SetStringField(TEXT("lease_observation_join"), TEXT("GT to worker: (world_generation,snapshot_captured); worker first voice to source: (audio_component_id,voice_generation,sequence) plus world/spatial checks. Per-producer prefixes are acquired independently. published/captured age is not worker push time."));
			Report->SetStringField(TEXT("active_worker_log"), bUsePressureWindowWorkers ? TEXT("pressure-window-worker") : TEXT("default-worker"));
			Report->SetNumberField(TEXT("gt_snapshot_rows"), double(SnapshotRows));
			Report->SetNumberField(TEXT("gt_snapshot_pushes"), double(SnapshotPushes));
			Report->SetNumberField(TEXT("gt_snapshot_capacity"), double(SnapshotCapacity));
			Report->SetBoolField(TEXT("gt_snapshot_prefix_complete"), bSnapshotPrefixComplete);
			Report->SetNumberField(TEXT("worker_rows"), double(WorkerRows));
			Report->SetNumberField(TEXT("worker_pushes"), double(WorkerPushes));
			Report->SetNumberField(TEXT("worker_capacity"), double(WorkerCapacity));
			Report->SetBoolField(TEXT("worker_prefix_complete"), bWorkerPrefixComplete);
			Report->SetNumberField(TEXT("worker_prefix_gap"), double(WorkerPushes - WorkerRows));
			Report->SetNumberField(TEXT("gt_snapshot_overflows"), double(SnapshotOverflows));
			Report->SetNumberField(TEXT("worker_overflows"), double(WorkerOverflows));
			Report->SetNumberField(TEXT("default_worker_probe_rows"), double(DefaultWorkerRows));
			Report->SetNumberField(TEXT("default_worker_probe_pushes"), double(DefaultWorkerPushes));
			Report->SetNumberField(TEXT("default_worker_probe_capacity"), double(DefaultWorkerCapacity));
			Report->SetNumberField(TEXT("default_worker_probe_overflows"), double(DefaultWorkerOverflows));
			Report->SetBoolField(TEXT("default_worker_probe_prefix_complete"), bDefaultWorkerPrefixComplete);
			Report->SetNumberField(TEXT("default_worker_probe_prefix_gap"), double(DefaultWorkerPushes - DefaultWorkerRows));
			Report->SetBoolField(TEXT("pressure_window_prepared"), bPressureWindowPrepared);
			Report->SetBoolField(TEXT("pressure_window_opened"), bPressureWindowOpened);
			Report->SetBoolField(TEXT("pressure_window_closed"), bPressureWindowClosed);
			Report->SetBoolField(TEXT("pressure_window_publications_quiescent"), bPressureWindowQuiescent);
			Report->SetNumberField(TEXT("pressure_window_opened_seconds_monotonic"), PressureWindowOpenedSeconds);
			Report->SetNumberField(TEXT("pressure_window_closed_seconds_monotonic"), PressureWindowClosedSeconds);
			Report->SetNumberField(TEXT("pressure_window_duration_seconds"), PressureWindowClosedSeconds >= PressureWindowOpenedSeconds
				? PressureWindowClosedSeconds - PressureWindowOpenedSeconds : 0.0);
			Report->SetStringField(TEXT("pressure_window_scope"), TEXT("Source and worker startup completed before opening; only publications inside the opened movement observation interval are included."));
			Report->SetNumberField(TEXT("pressure_window_snapshot_rows"), double(PressureSnapshotRows));
			Report->SetNumberField(TEXT("pressure_window_snapshot_pushes"), double(PressureSnapshotPushes));
			Report->SetNumberField(TEXT("pressure_window_snapshot_capacity"), double(PressureSnapshotCapacity));
			Report->SetNumberField(TEXT("pressure_window_snapshot_overflows"), double(PressureSnapshotOverflows));
			Report->SetBoolField(TEXT("pressure_window_snapshot_prefix_complete"), bPressureSnapshotPrefixComplete);
			Report->SetNumberField(TEXT("pressure_window_snapshot_prefix_gap"), double(PressureSnapshotPushes - PressureSnapshotRows));
			Report->SetNumberField(TEXT("pressure_window_worker_rows"), double(PressureWorkerRows));
			Report->SetNumberField(TEXT("pressure_window_worker_pushes"), double(PressureWorkerPushes));
			Report->SetNumberField(TEXT("pressure_window_worker_capacity"), double(PressureWorkerCapacity));
			Report->SetNumberField(TEXT("pressure_window_worker_overflows"), double(PressureWorkerOverflows));
			Report->SetBoolField(TEXT("pressure_window_worker_prefix_complete"), bPressureWorkerPrefixComplete);
			Report->SetNumberField(TEXT("pressure_window_worker_prefix_gap"), double(PressureWorkerPushes - PressureWorkerRows));
			Report->SetBoolField(TEXT("pressure_window_logs_prefix_complete"), bPressureWindowLogPrefixesComplete);
			Report->SetBoolField(TEXT("pressure_window_logs_overflow_free"), bPressureWindowLogsOverflowFree);
			Report->SetBoolField(TEXT("lease_observation_nonempty_without_overflow"), bLeaseObservationAvailable);
			FString CSV = TEXT("block,world_generation,spatial_version,captured_seconds,submit_seconds,source_audio_id,num_sources,submitted,fail_code\n");
			for (uint64 I = 0; I < SnapshotRows; ++I)
			{
				const auto& Row = Bridge->SnapshotProbes[I];
				CSV += FString::Printf(TEXT("%llu,%llu,%llu,%.9f,%.9f,%llu,%u,%u,%u\n"), Row.Block, Row.WorldGeneration, Row.SpatialVersion,
					Row.CapturedSeconds, Row.SubmitSeconds, Row.Source0AudioId, Row.NumSources, uint32(Row.Submitted), uint32(Row.FailCode));
			}
			const bool bGT = FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(ReportDirectory, TEXT("gt-snapshots.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			CSV = TEXT("block,world_generation,loop_start,snapshot_captured,snap_valid,snap_reason,num_inputs,eval_start,eval_end,eval_ok,results_published,push_seconds,push_ok,first_audio_id,first_voice_generation,first_sequence,reverb_attempt,reverb_start,reverb_end,reverb_ok,reverb_sequence,reverb_captured,wait_end\n");
			for (uint64 I = 0; I < WorkerRows; ++I)
			{
				const auto& Row = bUsePressureWindowWorkers ? Bridge->PressureWorkers.Entries[I] : Bridge->WorkerProbes[I];
				CSV += FString::Printf(TEXT("%llu,%llu,%.9f,%.9f,%u,%u,%u,%.9f,%.9f,%u,%u,%.9f,%u,%llu,%llu,%llu,%u,%.9f,%.9f,%u,%llu,%.9f,%.9f\n"),
					Row.Block, Row.WorldGeneration, Row.LoopStartSeconds, Row.SnapshotCaptured, uint32(Row.SnapValid), uint32(Row.SnapReason), uint32(Row.NumInputs),
					Row.EvalStartSeconds, Row.EvalEndSeconds, uint32(Row.EvalOk), uint32(Row.ResultsPublished), Row.PushSeconds, uint32(Row.PushOk),
					Row.FirstAudioId, Row.FirstVoiceGen, Row.FirstSeq, uint32(Row.ReverbAttempt), Row.ReverbStartSeconds, Row.ReverbEndSeconds,
					uint32(Row.ReverbOk), Row.ReverbSequence, Row.ReverbCaptured, Row.WaitEndSeconds);
			}
			const bool bWorker = FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(ReportDirectory, TEXT("worker-phases.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			CSV = TEXT("block,voice,callback_audio_id,result_audio_id,world_generation,voice_generation,spatial_version,sequence,snapshot_captured,consumed_seconds,age_ms,reject,reject_detail,fallback\n");
			for (uint32 I = 0; I < SourceTraceCount; ++I)
			{
				const auto& Row = GraphContext->CapturedSourceBlocks[int32(I)];
				CSV += FString::Printf(TEXT("%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%.9f,%.9f,%u,%u,%u\n"),
					Row.Block, Row.Voice, Row.CallbackAudioComponentId, Row.ResultAudioComponentId, Row.ResultWorldGeneration,
					Row.ResultVoiceGeneration, Row.ResultSpatialVersion, Row.ResultSequence, Row.SnapshotCaptured, Row.ConsumedSeconds,
					Row.AgeMs, uint32(Row.Reject), Row.RejectDetail, uint32(Row.Fallback));
			}
			const bool bSourceLease = FFileHelper::SaveStringToFile(CSV, *FPaths::Combine(ReportDirectory, TEXT("source-lease.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return bGT && bWorker && bSourceLease && bLeaseObservationAvailable;
		}

	bool RecordGeometryWitnesses(TSharedRef<FJsonObject> Report)
	{
		if (!BakeVolume.IsValid() || !BakeAsset.IsValid() || !BakeBounds.IsValid()) return false;
		TArray<FVector> Vertices;
		TArray<FIntVector> Triangles;
		TArray<int32> MaterialIndices;
		FString Fingerprint, Error;
		TArray<FVector4> Probes;
		FVector Origin;
			if (!BakeVolume->IM_CaptureSceneGeometryForTest(Vertices, Triangles, MaterialIndices, Fingerprint)
				|| Fingerprint != BakeAssetSceneFingerprint || MaterialIndices.Num() != Triangles.Num()
				|| !BakeAsset->GetProbePreview(RuralOperationalTargetMap, Fingerprint, Probes, Origin, Error)) return false;
			const int32 MappingCount = BakeVolume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial
				? BakeVolume->PhysicalMaterials.Num() : BakeVolume->Materials.Num();
			for (const int32 MaterialIndex : MaterialIndices)
				if (MaterialIndex < 0 || MaterialIndex >= MappingCount) return false;
		auto SDKPoint = [&](const FVector& UEPoint)
		{
			const IPLVector3 P = ToSDKPosition(UEPoint, Origin);
			return FVector(P.x, P.y, P.z);
		};
		auto Trace = [&](const TCHAR* Name, const FVector& From, const FVector& To)
		{
			struct FIMHit { double T; int32 Triangle; };
			TArray<FIMHit> Hits;
			const FVector Direction = To - From;
			for (int32 I = 0; I < Triangles.Num(); ++I)
			{
				const FIntVector Ids = Triangles[I];
				if (!Vertices.IsValidIndex(Ids.X) || !Vertices.IsValidIndex(Ids.Y) || !Vertices.IsValidIndex(Ids.Z)) continue;
				const FVector A = Vertices[Ids.X], E1 = Vertices[Ids.Y] - A, E2 = Vertices[Ids.Z] - A;
				const FVector P = FVector::CrossProduct(Direction, E2);
				const double Det = FVector::DotProduct(E1, P);
				if (FMath::Abs(Det) < 1.e-12) continue;
				const FVector Offset = From - A;
				const double U = FVector::DotProduct(Offset, P) / Det;
				if (U < 0.0 || U > 1.0) continue;
				const FVector Q = FVector::CrossProduct(Offset, E1);
				const double V = FVector::DotProduct(Direction, Q) / Det;
				if (V < 0.0 || U + V > 1.0) continue;
				const double T = FVector::DotProduct(E2, Q) / Det;
				if (T >= 0.0 && T <= 1.0) Hits.Add({T, I});
			}
			Hits.Sort([](const FIMHit& A, const FIMHit& B) { return A.T < B.T; });
			TSharedRef<FJsonObject> Witness = MakeShared<FJsonObject>();
			Witness->SetObjectField(TEXT("from_sdk_m"), IMRuralPIEOperationalVectorToJson(From));
			Witness->SetObjectField(TEXT("to_sdk_m"), IMRuralPIEOperationalVectorToJson(To));
			Witness->SetNumberField(TEXT("triangle_hit_count"), Hits.Num());
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (int32 I = 0; I < FMath::Min(16, Hits.Num()); ++I)
			{
				const FIMHit& Hit = Hits[I];
				const FVector Position = From + Direction * Hit.T;
				const int32 MaterialIndex = MaterialIndices[Hit.Triangle];
				UMaterialInterface* Material = BakeVolume->Materials.IsValidIndex(MaterialIndex)
					? BakeVolume->Materials[MaterialIndex].Material.Get() : nullptr;
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("triangle_index"), Hit.Triangle);
				Row->SetNumberField(TEXT("distance_m"), Direction.Size() * Hit.T);
				Row->SetObjectField(TEXT("position_sdk_m"), IMRuralPIEOperationalVectorToJson(Position));
					Row->SetNumberField(TEXT("material_index"), MaterialIndex);
					if (BakeVolume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial)
					{
						const auto& Profile = BakeVolume->PhysicalMaterials[MaterialIndex];
						Row->SetStringField(TEXT("material_drive"), TEXT("physical_material"));
						Row->SetStringField(TEXT("material_path"), GetPathNameSafe(Profile.PhysicalMaterial.Get()));
						Row->SetBoolField(TEXT("solid_geometry"), Profile.bSolidGeometry);
						Row->SetBoolField(TEXT("supports_probes"), Profile.bSupportsProbes);
						Row->SetField(TEXT("masked"), MakeShared<FJsonValueNull>());
					}
					else
					{
						Row->SetStringField(TEXT("material_drive"), TEXT("legacy_render_material"));
						Row->SetStringField(TEXT("material_path"), GetPathNameSafe(Material));
						Row->SetBoolField(TEXT("masked"), Material && Material->GetBlendMode() == BLEND_Masked);
					}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Witness->SetArrayField(TEXT("first_16_intersections"), Rows);
			Report->SetObjectField(Name, Witness);
		};
			Report->SetStringField(TEXT("geometry_witness_scope"), TEXT("Independent double-precision two-sided triangle intersections in the exact fingerprint-matching exported SDK mesh; not SDK selected-hit identity, opacity sampling or a visibility certificate. Material order is CaptureScene's explicit mapping order."));
			Report->SetStringField(TEXT("material_drive"), BakeVolume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial ? TEXT("physical_material") : TEXT("legacy_render_material"));
			Report->SetNumberField(TEXT("non_solid_sections"), BakeVolume->NonSolidSections);
			Report->SetNumberField(TEXT("non_solid_triangles"), BakeVolume->NonSolidTriangles);
		Report->SetNumberField(TEXT("probe_height_m"), BakeVolume->ProbeHeightCm * 0.01);
		TArray<TSharedPtr<FJsonValue>> ColumnProbes;
		const FVector ListenerSDK = SDKPoint(OriginalListenerLocation);
		for (int32 I = 0; I < Probes.Num(); ++I)
		{
			const FVector4& Sphere = Probes[I];
			if (FMath::Abs(Sphere.X - ListenerSDK.X) > 1.e-4 || FMath::Abs(Sphere.Z - ListenerSDK.Z) > 1.e-4) continue;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("probe_index"), I);
			Row->SetObjectField(TEXT("center_sdk_m"), IMRuralPIEOperationalVectorToJson(FVector(Sphere.X, Sphere.Y, Sphere.Z)));
			Row->SetNumberField(TEXT("radius_m"), Sphere.W);
			ColumnProbes.Add(MakeShared<FJsonValueObject>(Row));
		}
		Report->SetArrayField(TEXT("probes_at_original_horizontal_column"), ColumnProbes);
		const FVector SourceSDK = SDKPoint(OriginalSourceLocation);
		Trace(TEXT("source_to_original_listener"), SourceSDK, SDKPoint(OriginalListenerLocation));
		Trace(TEXT("source_to_neighbor_listener"), SourceSDK, SDKPoint(ListenerMoveTarget));
		const FBox Region = BakeBounds->Bounds.GetBox();
		Trace(TEXT("vertical_original_listener_column"), SDKPoint(FVector(OriginalListenerLocation.X, OriginalListenerLocation.Y, Region.Max.Z)),
			SDKPoint(FVector(OriginalListenerLocation.X, OriginalListenerLocation.Y, Region.Min.Z - BakeVolume->ProbeHeightCm)));
		return true;
	}

	UWorld* FindTargetPIEWorld(const double Now)
	{
		if (!GEngine)
		{
			RequestFinish(false, TEXT("GEngine is unavailable while waiting for the target PIE world."), Now);
			return nullptr;
		}

		UWorld* FoundWorld = nullptr;
		int32 PIEWorldCount = 0;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				++PIEWorldCount;
				FoundWorld = Context.World();
			}
		}
		ObservedPIEWorldCount = PIEWorldCount;
		if (PIEWorldCount == 0)
		{
			if (Now > StartupDeadline)
			{
				RequestFinish(false, TEXT("Timed out waiting for AutomationLoadMap to create a PIE world."), Now);
			}
			return nullptr;
		}
		if (PIEWorldCount != 1)
		{
			RequestFinish(false, FString::Printf(TEXT("Expected one PIE world for the saved map; found %d."), PIEWorldCount), Now);
			return nullptr;
		}

		LoadedWorldPackage = FoundWorld->GetOutermost() ? FoundWorld->GetOutermost()->GetName() : FString();
		const FString CanonicalPackage = IMRuralPIEOperationalStripPIEPackagePrefix(LoadedWorldPackage);
		if (CanonicalPackage != TargetMapPackage)
		{
			RequestFinish(false, FString::Printf(TEXT("PIE world identity mismatch: expected %s, found %s."), TargetMapPackage, *LoadedWorldPackage), Now);
			return nullptr;
		}
		return FoundWorld;
	}

	bool ResolveSceneOwners(UWorld* World, const double Now)
	{
		TArray<AIMAcousticBakeVolume*> Volumes;
		for (TActorIterator<AIMAcousticBakeVolume> It(World); It; ++It)
		{
			if (IsValid(*It)) Volumes.Add(*It);
		}
		BakeVolumeCount = Volumes.Num();
		if (Volumes.Num() != 1)
		{
			RequestFinish(false, FString::Printf(TEXT("Expected exactly one BakeVolume in the PIE world; found %d."), Volumes.Num()), Now);
			return false;
		}
		BakeVolume = Volumes[0];
		BakeBounds = BakeVolume->BakeBounds;
		if (!BakeBounds.IsValid() || !BakeBounds->IsRegistered() || BakeBounds->GetWorld() != World)
		{
			RequestFinish(false, TEXT("The unique BakeVolume has no registered BakeBounds in the target PIE world."), Now);
			return false;
		}
		BakeBoundsBox = BakeBounds->Bounds.GetBox();
		if (!BakeBoundsBox.IsValid)
		{
			RequestFinish(false, TEXT("The unique BakeVolume has invalid runtime bounds."), Now);
			return false;
		}
		BakeAsset = BakeVolume->BakedField;
		if (!BakeAsset.IsValid())
		{
			RequestFinish(false, TEXT("The unique BakeVolume has no valid bound BakedField asset."), Now);
			return false;
		}
		BakeAssetPath = BakeAsset->GetPathName();
		BakeAssetWorldPackage = BakeAsset->WorldPackage;
		BakeAssetFormatVersion = BakeAsset->FormatVersion;
		BakeAssetSceneFingerprint = BakeAsset->SceneFingerprint;
		BakeAssetPayloadDigest = BakeAsset->PayloadDigest;
		bBakeAssetValid = BakeAsset->Validate(TargetMapPackage, BakeAsset->SceneFingerprint, BakeValidationError);
		if (!bBakeAssetValid)
		{
			RequestFinish(false, FString::Printf(TEXT("The bound BakeField failed its saved-world/payload validation: %s"), *BakeValidationError), Now);
			return false;
		}

		const FName Tag(TargetSourceTag);
		TArray<AActor*> TaggedActors;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && (*It)->ActorHasTag(Tag)) TaggedActors.Add(*It);
		}
		TaggedSourceActorCount = TaggedActors.Num();
		if (TaggedActors.Num() != 1)
		{
			RequestFinish(false, FString::Printf(TEXT("Expected exactly one actor tagged %s in PIE; found %d."), TargetSourceTag, TaggedActors.Num()), Now);
			return false;
		}
		TaggedSourceActor = Cast<AAmbientSound>(TaggedActors[0]);
		if (!TaggedSourceActor.IsValid())
		{
			RequestFinish(false, TEXT("The unique Rural water-drop tagged actor is not an AAmbientSound."), Now);
			return false;
		}
		TaggedActorPath = TaggedSourceActor->GetPathName();

		TArray<UIMAcousticSourceComponent*> Markers;
		TaggedSourceActor->GetComponents(Markers);
		SourceMarkerCount = Markers.Num();
		if (Markers.Num() != 1 || !IsValid(Markers[0]))
		{
			RequestFinish(false, FString::Printf(TEXT("Expected exactly one UIMAcousticSourceComponent on the tagged source; found %d."), Markers.Num()), Now);
			return false;
		}
		SourceMarker = Markers[0];
		SourceAudio = SourceMarker->AudioComponent;
		if (!SourceAudio.IsValid()
			|| SourceMarker->GetOwner() != TaggedSourceActor
			|| SourceMarker->GetWorld() != World
			|| SourceAudio->GetOwner() != TaggedSourceActor
			|| SourceAudio->GetWorld() != World
			|| TaggedSourceActor->GetAudioComponent() != SourceAudio)
		{
			RequestFinish(false, TEXT("The tagged source marker and AmbientSound AudioComponent ownership/world binding do not match."), Now);
			return false;
		}
		SourceMarkerPath = SourceMarker->GetPathName();
		SourceAudioPath = SourceAudio->GetPathName();
		if (!SourceMarker->ValidateSource(SourceValidationError))
		{
			RequestFinish(false, FString::Printf(TEXT("The tagged source failed UIMAcousticSourceComponent::ValidateSource: %s"), *SourceValidationError), Now);
			return false;
		}

		OriginalSourceTransform = SourceAudio->GetComponentTransform();
		OriginalSourceLocation = SourceAudio->GetComponentLocation();
		bSceneOwnersResolved = true;
		return true;
	}

	bool ResolvePlayerAndRuntime(UWorld* World, const double Now)
	{
		if (Now > StartupDeadline)
		{
			RequestFinish(false, FString::Printf(TEXT("Timed out waiting for ordinary PIE player/audio runtime. controller=%d pawn=%d playing=%d bridge_alive=%d rendered=%llu status=%s"),
				Listener.IsValid() ? 1 : 0, PlayerPawn.IsValid() ? 1 : 0, SourceAudio.IsValid() && SourceAudio->IsPlaying() ? 1 : 0,
				Bridge.IsValid() && Bridge->Alive.load(std::memory_order_acquire) ? 1 : 0,
				Bridge.IsValid() ? Bridge->RenderedBlocks.load(std::memory_order_acquire) : 0ull,
				BakeVolume.IsValid() ? *BakeVolume->Status : TEXT("no volume")), Now);
			return false;
		}

		if (!Listener.IsValid()) Listener = World->GetFirstPlayerController();
		if (Listener.IsValid() && !PlayerPawn.IsValid()) PlayerPawn = Listener->GetPawn();
		if (!Listener.IsValid() || !PlayerPawn.IsValid()) return false;
		if (!bPlayerStartResolved)
		{
			TArray<APlayerStart*> Starts;
			for (TActorIterator<APlayerStart> It(World); It; ++It)
			{
				if (IsValid(*It)) Starts.Add(*It);
			}
			PlayerStartCount = Starts.Num();
			if (Starts.IsEmpty())
			{
				RequestFinish(false, TEXT("The target PIE world has no APlayerStart."), Now);
				return false;
			}
			PlayerStart = Starts[0];
			double NearestDistanceSquared = FVector::DistSquared(PlayerPawn->GetActorLocation(), PlayerStart->GetActorLocation());
			for (APlayerStart* Candidate : Starts)
			{
				const double DistanceSquared = FVector::DistSquared(PlayerPawn->GetActorLocation(), Candidate->GetActorLocation());
				if (DistanceSquared < NearestDistanceSquared)
				{
					NearestDistanceSquared = DistanceSquared;
					PlayerStart = Candidate;
				}
			}
			PlayerStartPath = PlayerStart->GetPathName();
			PlayerStartLocation = PlayerStart->GetActorLocation();
			InitialPawnLocation = PlayerPawn->GetActorLocation();
			PawnStartDistanceCm = FVector::Distance(InitialPawnLocation, PlayerStartLocation);
			bPawnCloseToPlayerStart = PawnStartDistanceCm <= MaxPlayerStartDistanceCm;
			if (!bPawnCloseToPlayerStart)
			{
				Errors.Add(FString::Printf(TEXT("The PIE pawn spawned %.1f cm from the nearest PlayerStart; the declared closeness limit is %.1f cm."),
					PawnStartDistanceCm, MaxPlayerStartDistanceCm));
			}
			if (!bFixedAudition) CheckLandscapeGround(World);
			bPlayerStartResolved = true;
		}

		if (!SourceAudio->IsPlaying()) return false;
		bSourceWasObservedPlaying = true;
		// This fixture is the saved MetaSound water source. Before the volume's
		// first graph tick, the generic test helper may return the legacy device
		// bridge. Caching that live but unused object makes startup wait forever
		// while the actual graph renders. Resolve the current graph every poll;
		// a context replacement must also replace this observation identity.
		FAudioDevice* AudioDevice = World->GetAudioDeviceRaw();
		const FIMAcousticMetaSoundContextPtr CurrentGraph = AudioDevice
			? IMAcousticMetaSound::FindAcousticMetaSoundContext(AudioDevice->DeviceID) : nullptr;
		if (!CurrentGraph || CurrentGraph->Stopped.load(std::memory_order_acquire)
			|| !CurrentGraph->Device.IsValid())
		{
			if (!bWaitedForGraphContext)
			{
				bWaitedForGraphContext = true;
				UE_LOG(LogTemp, Display, TEXT("IMLogs RuralPIEGraphBridge awaiting_current_context world_id=%u device=%u"),
					World->GetUniqueID(), AudioDevice ? AudioDevice->DeviceID : MAX_uint32);
			}
			return false;
		}
		if (Bridge != CurrentGraph->Device)
		{
			UE_LOG(LogTemp, Display, TEXT("IMLogs RuralPIEGraphBridge acquired_current_context world_id=%u device=%u context=%llu previous_bridge=%llu bridge=%llu previous_rendered=%llu current_rendered=%llu generation=%llu"),
				World->GetUniqueID(), CurrentGraph->DeviceId,
				uint64(reinterpret_cast<UPTRINT>(CurrentGraph.Get())),
				uint64(reinterpret_cast<UPTRINT>(Bridge.Get())),
				uint64(reinterpret_cast<UPTRINT>(CurrentGraph->Device.Get())),
				Bridge ? Bridge->RenderedBlocks.load(std::memory_order_acquire) : 0ull,
				CurrentGraph->Device->RenderedBlocks.load(std::memory_order_acquire),
				CurrentGraph->Device->WorldGeneration.load(std::memory_order_acquire));
			Bridge = CurrentGraph->Device;
			++GraphBridgeAcquisitions;
		}
		if (!Bridge.IsValid() || !Bridge->Alive.load(std::memory_order_acquire)) return false;
		BridgeWorldGeneration = Bridge->WorldGeneration.load(std::memory_order_acquire);
		bBridgeEnabled = Bridge->Enabled.load(std::memory_order_acquire);
		BridgeRenderRoutes = Bridge->RenderRoutes.load(std::memory_order_acquire);
		InitialRenderedBlocks = Bridge->RenderedBlocks.load(std::memory_order_acquire);
		if (!bBridgeEnabled)
		{
			RequestFinish(false, TEXT("The V2 acoustic device bridge is alive but disabled in the ordinary PIE world."), Now);
			return false;
		}
		if (InitialRenderedBlocks == 0) return false;
			uint64 InitialSnapshotIndex = 0;
			if (!ReadLatestSnapshot(InitialSnapshot, InitialSnapshotIndex)) return false;
		if (!InitialSnapshot.Submitted || InitialSnapshot.NumSources != 1 || InitialSnapshot.Source0AudioId == 0)
		{
			return false;
		}
		if (InitialSnapshot.SpatialVersion == 0)
		{
			RequestFinish(false, TEXT("The first submitted Rural snapshot has no listener spatial version."), Now);
			return false;
		}

		FVector ViewLocation;
		Listener->GetPlayerViewPoint(ViewLocation, OriginalListenerRotation);
		OriginalListenerLocation = IMRuralPIEOperationalSnapshotListenerUE(InitialSnapshot);
		if (bMovementObservation)
		{
			MovementOriginalPawnLocation = PlayerPawn->GetActorLocation();
			MovementOriginalControlRotation = Listener->GetControlRotation();
		}

		OriginalSourceSDK = IMRuralPIEOperationalSnapshotSourceSDK(InitialSnapshot);
		OriginalSourceAudioId = InitialSnapshot.Source0AudioId;
		OriginalSnapshotIndex = InitialSnapshotIndex;
		OriginalSpatialVersion = InitialSnapshot.SpatialVersion;
		OriginalSourceWorldGeneration = InitialSnapshot.WorldGeneration;
		SourceOriginalInBakeBounds = IMRuralPIEOperationalIsPointInsideBakeBounds(BakeBounds.Get(), OriginalSourceLocation);
		ListenerOriginalInBakeBounds = IMRuralPIEOperationalIsPointInsideBakeBounds(BakeBounds.Get(), OriginalListenerLocation);
		ListenerMoveTarget = OriginalListenerLocation - FVector(MovementCm, 0.0, 0.0);
		SourceMoveTarget = OriginalSourceLocation + FVector(MovementCm, 0.0, 0.0);
		ListenerTargetInBakeBounds = IMRuralPIEOperationalIsPointInsideBakeBounds(BakeBounds.Get(), ListenerMoveTarget);
		SourceTargetInBakeBounds = IMRuralPIEOperationalIsPointInsideBakeBounds(BakeBounds.Get(), SourceMoveTarget);
		if (!SourceOriginalInBakeBounds || !ListenerOriginalInBakeBounds || !ListenerTargetInBakeBounds || !SourceTargetInBakeBounds)
		{
			RequestFinish(false, FString::Printf(TEXT("Required baseline/+100 cm positions are outside BakeBounds: listener=%d listener_plus=%d source=%d source_plus=%d."),
				ListenerOriginalInBakeBounds ? 1 : 0, ListenerTargetInBakeBounds ? 1 : 0,
				SourceOriginalInBakeBounds ? 1 : 0, SourceTargetInBakeBounds ? 1 : 0), Now);
			return false;
		}
		bRuntimeReady = true;
		return true;
	}

	void CheckLandscapeGround(UWorld* World)
	{
		GroundTraceStart = PlayerStartLocation + FVector(0.0, 0.0, 5000.0);
		GroundTraceEnd = PlayerStartLocation - FVector(0.0, 0.0, 50000.0);
		FCollisionQueryParams QueryParams(FName(TEXT("IMRuralPIEOperationalGround")), false);
		QueryParams.AddIgnoredActor(PlayerStart.Get());
		if (PlayerPawn.IsValid()) QueryParams.AddIgnoredActor(PlayerPawn.Get());
		TArray<FHitResult> Hits;
		bWorldStaticTraceHit = World->LineTraceMultiByChannel(Hits, GroundTraceStart, GroundTraceEnd, ECC_WorldStatic, QueryParams);
		WorldStaticHitCount = Hits.Num();
		if (!Hits.IsEmpty())
		{
			FirstWorldStaticHitActorClass = Hits[0].GetActor() ? Hits[0].GetActor()->GetClass()->GetPathName() : TEXT("no actor");
			FirstWorldStaticHitComponent = Hits[0].GetComponent() ? Hits[0].GetComponent()->GetPathName() : TEXT("no component");
		}

		const UCharacterMovementComponent* CharacterMovement = PlayerPawn->FindComponentByClass<UCharacterMovementComponent>();
		if (CharacterMovement)
		{
			WalkableFloorAngle = CharacterMovement->GetWalkableFloorAngle();
			WalkableFloorNormalZ = CharacterMovement->GetWalkableFloorZ();
			bWalkabilityOracleAvailable = true;
		}

		float NearestLandscapeDistance = TNumericLimits<float>::Max();
		for (const FHitResult& Hit : Hits)
		{
			if (!Cast<ALandscapeProxy>(Hit.GetActor())) continue;
			++LandscapeHitCount;
			if (Hit.Distance < NearestLandscapeDistance)
			{
				NearestLandscapeDistance = Hit.Distance;
				LandscapeHitActorPath = Hit.GetActor()->GetPathName();
				LandscapeHitComponentPath = Hit.GetComponent() ? Hit.GetComponent()->GetPathName() : TEXT("no component");
				LandscapeImpactPoint = Hit.ImpactPoint;
				LandscapeImpactNormal = Hit.ImpactNormal;
				LandscapeImpactDistance = Hit.Distance;
			}
		}
		bLandscapeHit = LandscapeHitCount > 0;
		bLandscapeWalkable = bLandscapeHit && bWalkabilityOracleAvailable
			&& LandscapeImpactNormal.Z >= WalkableFloorNormalZ;
		bGroundSafetyProven = bLandscapeWalkable;
		if (!bLandscapeHit)
		{
			Errors.Add(FString::Printf(TEXT("ECC_WorldStatic trace beneath PlayerStart returned no LandscapeProxy collision hit (world_static_hits=%d, first_actor=%s); safe landing is not asserted."),
				WorldStaticHitCount, *FirstWorldStaticHitActorClass));
		}
		else if (bWalkabilityOracleAvailable && !bLandscapeWalkable)
		{
			Errors.Add(FString::Printf(TEXT("Landscape collision at %s is steeper than the pawn walkable floor threshold: normal_z=%.4f required_z=%.4f angle_deg=%.2f."),
				*LandscapeHitActorPath, LandscapeImpactNormal.Z, WalkableFloorNormalZ, WalkableFloorAngle));
		}
	}

	bool ReadLatestSnapshot(FIMAcousticSnapshotProbe& OutSnapshot, uint64& OutIndex) const
	{
		if (!Bridge.IsValid()) return false;
		const uint64 Pushes = Bridge->SnapshotProbePushes.load(std::memory_order_acquire);
		const uint64 Limit = FMath::Min<uint64>(Pushes, FIMAcousticDeviceBridge::ProbeSnapshotCapacity);
		for (uint64 I = Limit; I > 0; --I)
		{
			const uint64 Index = I - 1;
			if (Bridge->SnapshotDone[Index].load(std::memory_order_acquire) != Index + 1) continue;
			OutSnapshot = Bridge->SnapshotProbes[Index];
			OutIndex = Index;
			return true;
		}
		return false;
	}

	void ArmPhase(const int32 NewStage, const TCHAR* Name, const FIMAcousticSnapshotProbe& Snapshot,
		const uint64 SnapshotIndex, const double Now)
	{
		Stage = NewStage;
		PhaseName = Name;
		PhaseStartSnapshot = Snapshot;
		PhaseStartSnapshotIndex = SnapshotIndex;
		PhaseStartRenderedBlocks = Bridge->RenderedBlocks.load(std::memory_order_acquire);
		PhaseStartSpatialVersion = Snapshot.SpatialVersion;
		PhaseDeadline = Now + PhaseTimeoutSeconds;
	}

	void BeginListenerMoveOut(const double Now)
	{
		Listener->SetAudioListenerOverride(nullptr, ListenerMoveTarget, OriginalListenerRotation);
		bListenerOverrideTouched = true;
		ArmPhase(StageListenerMoveOut, TEXT("listener_minus_100cm_x"), InitialSnapshot, OriginalSnapshotIndex, Now);
	}

	bool UpdateMovementPhase(UWorld* World, const double Now)
	{
		if (!SourceAudio.IsValid() || !Listener.IsValid() || !PlayerPawn.IsValid() || !Bridge.IsValid())
		{
			RequestFinish(false, TEXT("An owned Rural PIE source, controller, pawn, or V2 bridge disappeared during movement."), Now);
			return false;
		}
		if (!SourceAudio->IsPlaying())
		{
			bPlaybackContinuous = false;
			RequestFinish(false, TEXT("The tagged water-drop AudioComponent stopped playing during the bounded movement phases."), Now);
			return false;
		}
		if (!Bridge->Alive.load(std::memory_order_acquire))
		{
			RequestFinish(false, TEXT("The V2 acoustic device bridge stopped being alive during the movement phases."), Now);
			return false;
		}

		FIMAcousticSnapshotProbe Snapshot{};
		uint64 SnapshotIndex = 0;
		const uint64 Rendered = Bridge->RenderedBlocks.load(std::memory_order_acquire);
		const bool bHasSnapshot = ReadLatestSnapshot(Snapshot, SnapshotIndex);
		if (bHasSnapshot && SnapshotIndex > PhaseStartSnapshotIndex
			&& Rendered > PhaseStartRenderedBlocks
			&& Snapshot.Submitted
			&& Snapshot.NumSources == 1
			&& Snapshot.Source0AudioId == OriginalSourceAudioId
			&& PhaseOracleMatches(Snapshot))
		{
			RecordPhaseObservation(PhaseName, Snapshot, SnapshotIndex, Rendered);
			AdvancePhase(Snapshot, SnapshotIndex, Now);
			return false;
		}
		if (Now > PhaseDeadline)
		{
			RequestFinish(false, FString::Printf(TEXT("Timed out in %s: snapshot_index=%llu start_index=%llu spatial_version=%llu start_version=%llu rendered=%llu start_rendered=%llu playing=%d status=%s."),
				*PhaseName, SnapshotIndex, PhaseStartSnapshotIndex, bHasSnapshot ? Snapshot.SpatialVersion : 0ull,
				PhaseStartSpatialVersion, Rendered, PhaseStartRenderedBlocks, SourceAudio->IsPlaying() ? 1 : 0,
				BakeVolume.IsValid() ? *BakeVolume->Status : TEXT("no volume")), Now);
		}
		return false;
	}

	bool PhaseOracleMatches(const FIMAcousticSnapshotProbe& Snapshot) const
	{
		const FVector ListenerPosition = IMRuralPIEOperationalSnapshotListenerUE(Snapshot);
		const FVector SourcePosition = IMRuralPIEOperationalSnapshotSourceSDK(Snapshot);
		const FVector BaselineSource = IMRuralPIEOperationalSnapshotSourceSDK(InitialSnapshot);
		const bool bListenerIsOriginal = FVector::Distance(ListenerPosition, OriginalListenerLocation) <= ListenerToleranceCm;
		const bool bSourceIsOriginal = FVector::Distance(SourcePosition, BaselineSource) <= SourceSnapshotToleranceM;
		switch (Stage)
		{
		case StageListenerMoveOut:
			return Snapshot.SpatialVersion > PhaseStartSpatialVersion
				&& FVector::Distance(ListenerPosition, ListenerMoveTarget) <= ListenerToleranceCm
				&& bSourceIsOriginal;
		case StageListenerMoveBack:
			return Snapshot.SpatialVersion > PhaseStartSpatialVersion
				&& bListenerIsOriginal
				&& bSourceIsOriginal;
		case StageSourceMoveOut:
			{
				const double DeltaMeters = FVector::Distance(SourcePosition, BaselineSource);
				return Snapshot.SpatialVersion == PhaseStartSpatialVersion
					&& bListenerIsOriginal
					&& FMath::IsNearlyEqual(DeltaMeters, MovementCm * 0.01, 0.03);
			}
		case StageSourceMoveBack:
			return Snapshot.SpatialVersion == PhaseStartSpatialVersion
				&& bListenerIsOriginal
				&& bSourceIsOriginal;
		default:
			return false;
		}
	}

	void AdvancePhase(const FIMAcousticSnapshotProbe& Snapshot, const uint64 SnapshotIndex, const double Now)
	{
		switch (Stage)
		{
		case StageListenerMoveOut:
			Listener->SetAudioListenerOverride(nullptr, OriginalListenerLocation, OriginalListenerRotation);
			ArmPhase(StageListenerMoveBack, TEXT("listener_return_to_origin"), Snapshot, SnapshotIndex, Now);
			break;
		case StageListenerMoveBack:
			ListenerReturnedSpatialVersion = Snapshot.SpatialVersion;
			SourceAudio->SetWorldLocation(SourceMoveTarget, false, nullptr, ETeleportType::TeleportPhysics);
			bSourceTransformTouched = true;
			ArmPhase(StageSourceMoveOut, TEXT("source_plus_100cm_x"), Snapshot, SnapshotIndex, Now);
			break;
		case StageSourceMoveOut:
			SourceAudio->SetWorldTransform(OriginalSourceTransform, false, nullptr, ETeleportType::TeleportPhysics);
			ArmPhase(StageSourceMoveBack, TEXT("source_return_to_origin"), Snapshot, SnapshotIndex, Now);
			break;
		case StageSourceMoveBack:
			RequestFinish(true, TEXT("Listener moved -100 cm and source moved +100 cm on X independently, then both returned; PIE bridge snapshots and rendered blocks confirmed each phase."), Now);
			break;
		default:
			RequestFinish(false, TEXT("Rural PIE movement phase transition was invalid."), Now);
			break;
		}
	}

	void RecordPhaseObservation(const FString& Name, const FIMAcousticSnapshotProbe& Snapshot,
		const uint64 SnapshotIndex, const uint64 RenderedBlocks)
	{
		TSharedRef<FJsonObject> Observation = MakeShared<FJsonObject>();
		Observation->SetStringField(TEXT("phase"), Name);
		Observation->SetNumberField(TEXT("snapshot_index"), static_cast<double>(SnapshotIndex));
		Observation->SetNumberField(TEXT("snapshot_world_generation"), static_cast<double>(Snapshot.WorldGeneration));
		Observation->SetNumberField(TEXT("listener_spatial_version"), static_cast<double>(Snapshot.SpatialVersion));
		Observation->SetObjectField(TEXT("listener_ue_cm"), IMRuralPIEOperationalVectorToJson(IMRuralPIEOperationalSnapshotListenerUE(Snapshot)));
		Observation->SetObjectField(TEXT("source0_sdk_m"), IMRuralPIEOperationalVectorToJson(IMRuralPIEOperationalSnapshotSourceSDK(Snapshot)));
		Observation->SetNumberField(TEXT("source_count"), Snapshot.NumSources);
		Observation->SetNumberField(TEXT("source0_audio_component_id"), static_cast<double>(Snapshot.Source0AudioId));
		Observation->SetNumberField(TEXT("rendered_blocks"), static_cast<double>(RenderedBlocks));
		Observation->SetNumberField(TEXT("rejected_blocks"), static_cast<double>(Bridge->RejectedBlocks.load(std::memory_order_acquire)));
		Observation->SetBoolField(TEXT("source_playing"), SourceAudio->IsPlaying());
		PhaseObservations.Add(MakeShared<FJsonValueObject>(Observation));
	}

	void RequestFinish(const bool bPass, const FString& Message, const double Now)
	{
		if (bFinishing)
		{
			if (!bPass && !Message.IsEmpty()) Errors.AddUnique(Message);
			return;
		}
		bFinishing = true;
		bool bPressureWindowCloseSucceeded = true;
		if (bMovementObservation && bPressureWindowOpened && !bPressureWindowClosed)
		{
			if (Bridge.IsValid())
			{
				Bridge->ClosePressureWindowProbes();
				bPressureWindowClosed = true;
				PressureWindowClosedSeconds = FPlatformTime::Seconds();
			}
			else
			{
				bPressureWindowCloseSucceeded = false;
				Errors.AddUnique(TEXT("Movement pressure-window could not be closed because its PIE device bridge disappeared."));
			}
		}
		bRequestedPass = bPass && bPressureWindowCloseSucceeded;
		if (!bPass && !Message.IsEmpty()) Errors.AddUnique(Message);
		if (!bPass && Errors.IsEmpty()) Errors.Add(TEXT("The Rural PIE operational test failed without a more specific error."));

		if (bSourceTransformTouched && SourceAudio.IsValid())
		{
			SourceAudio->SetWorldTransform(OriginalSourceTransform, false, nullptr, ETeleportType::TeleportPhysics);
			bSourceTransformRestored = OriginalSourceTransform.Equals(SourceAudio->GetComponentTransform(), 0.1f);
			if (!bSourceTransformRestored) Errors.AddUnique(TEXT("The source AudioComponent transform could not be restored before PIE exit."));
		}
		else
		{
			bSourceTransformRestored = true;
		}

		if (bCameraPoseTouched && Listener.IsValid() && PlayerPawn.IsValid())
		{
			Listener->SetControlRotation(MovementOriginalControlRotation);
			PlayerPawn->SetActorLocation(MovementOriginalPawnLocation, false);
		}
		if (bListenerOverrideTouched && Listener.IsValid())
		{
			Listener->SetAudioListenerOverride(nullptr, OriginalListenerLocation, OriginalListenerRotation);
			bListenerRestoreRequested = true;
		}
		else
		{
			bListenerRestoreRequested = true;
		}

		bNeedsCleanupObservation = bSourceTransformTouched || bListenerOverrideTouched || bCameraPoseTouched;
		FIMAcousticSnapshotProbe Snapshot{};
		if (ReadLatestSnapshot(Snapshot, CleanupStartSnapshotIndex))
		{
			CleanupStartSnapshot = Snapshot;
		}
		CleanupDeadline = Now + CleanupTimeoutSeconds;
		if (!bNeedsCleanupObservation) bCleanupObserved = true;
		Stage = StageFinalize;
	}

	bool UpdateCleanup(const double Now)
	{
		if (bMovementObservation && bPressureWindowOpened && !bPressureWindowQuiescent)
		{
			if (Bridge.IsValid() && Bridge->PressureWindowPublicationsQuiescent())
			{
				bPressureWindowQuiescent = true;
			}
			else if (Now > CleanupDeadline)
			{
				Errors.AddUnique(FString::Printf(TEXT("Movement pressure-window publications did not quiesce within %.1f seconds: bridge_valid=%d closed=%d."),
					CleanupTimeoutSeconds, Bridge.IsValid() ? 1 : 0, bPressureWindowClosed ? 1 : 0));
				return Finalize(false);
			}
			else return false;
		}
		if (bNeedsCleanupObservation && !bCleanupObserved)
		{
			if (!SourceAudio.IsValid() || !Listener.IsValid() || !Bridge.IsValid())
			{
				Errors.AddUnique(TEXT("PIE owners disappeared before restored listener/source state could be observed."));
				return Finalize(false);
			}
			if (!SourceAudio->IsPlaying())
			{
				bPlaybackContinuous = false;
				Errors.AddUnique(TEXT("The source was not playing while the test verified cleanup."));
			}
			FIMAcousticSnapshotProbe Snapshot{};
			uint64 SnapshotIndex = 0;
			const bool bHasSnapshot = ReadLatestSnapshot(Snapshot, SnapshotIndex);
			const bool bListenerRestored = bHasSnapshot
				&& FVector::Distance(IMRuralPIEOperationalSnapshotListenerUE(Snapshot), OriginalListenerLocation) <= ListenerToleranceCm;
			const bool bSourceRestored = bHasSnapshot
				&& FVector::Distance(IMRuralPIEOperationalSnapshotSourceSDK(Snapshot), IMRuralPIEOperationalSnapshotSourceSDK(InitialSnapshot)) <= SourceSnapshotToleranceM;
			const bool bFreshAfterRestore = bHasSnapshot && SnapshotIndex > CleanupStartSnapshotIndex;
			bSourceTransformRestored = OriginalSourceTransform.Equals(SourceAudio->GetComponentTransform(), 0.1f);
			if (bFreshAfterRestore && bListenerRestored && bSourceRestored && bSourceTransformRestored && SourceAudio->IsPlaying())
			{
				bCleanupObserved = true;
			}
			else if (Now > CleanupDeadline)
			{
				Errors.AddUnique(FString::Printf(TEXT("Cleanup restore was not observed within %.1f seconds: fresh_snapshot=%d listener_restored=%d source_restored=%d source_transform_restored=%d."),
					CleanupTimeoutSeconds, bFreshAfterRestore ? 1 : 0, bListenerRestored ? 1 : 0,
					bSourceRestored ? 1 : 0, bSourceTransformRestored ? 1 : 0));
			}
			if (!bCleanupObserved && Now <= CleanupDeadline) return false;
		}
		return Finalize(bRequestedPass);
	}

	bool Finalize(const bool bRequestedSuccess)
	{
		if (bFinalized) return true;
		bFinalized = true;
		if (bObserveReverb) IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
		if (!bPlaybackContinuous && bSourceWasObservedPlaying)
		{
			Errors.AddUnique(TEXT("Audio playback continuity failed after the source had been observed playing."));
		}
		if (!bSourceTransformRestored) Errors.AddUnique(TEXT("The source AudioComponent did not return to its saved world transform."));
		if (!bListenerRestoreRequested) Errors.AddUnique(TEXT("The test did not request restoration of its listener override."));

		bool bReportSaved = WriteReport(bRequestedSuccess && Errors.IsEmpty());
		if (!bReportSaved)
		{
			Errors.AddUnique(FString::Printf(TEXT("Capture evidence incomplete or report serialization/write failed: %s"), *ReportPath));
		}
		const bool bPass = bRequestedSuccess && Errors.IsEmpty() && bReportSaved && bCleanupObserved;
		const FString Message = bPass
			? (bObserveReverb
				? FString::Printf(TEXT("Ordinary Rural graph observations captured; this is not wet/listening acceptance; report=%s."), *ReportPath)
				: FString::Printf(TEXT("Ordinary Rural PIE spatial state transitions passed; report=%s."), *ReportPath))
			: FString::Join(Errors, TEXT(" | "));
		if (bPass) Test->AddInfo(Message); else Test->AddError(Message);
		UE_LOG(LogTemp, Display, TEXT("IMLogs RuralPIEOperational %s %s report=%s"), bPass ? TEXT("PASS") : TEXT("FAIL"), *Message, *ReportPath);
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] RuralPIEOperational %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
		if (GUnrealEd) GUnrealEd->RequestEndPlayMap();
		return true;
	}

	bool WriteReport(const bool bPreliminaryPass)
	{
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		bool bCaptureSaved = true;
		if (bObserveReverb)
		{
			TSharedRef<FJsonObject> Observation = MakeShared<FJsonObject>();
			bCaptureSaved = ExportReverbObservation(Observation);
			Observation->SetBoolField(TEXT("all_capture_files_saved"), bCaptureSaved);
			Report->SetObjectField(TEXT("reverb_graph_observation"), Observation);
		}
		Report->SetStringField(TEXT("schema"), bFixedAudition
			? TEXT("im_acoustic_audition_graph_observation_v1") : TEXT("im_acoustic_rural_pie_operational_v1"));
		Report->SetBoolField(TEXT("fixed_default_listener_capture"), bFixedAudition && !bMovementObservation);
		Report->SetBoolField(TEXT("startup_movement_observation"), bMovementObservation);
		Report->SetBoolField(TEXT("listener_or_source_pose_modified"), bListenerOverrideTouched || bSourceTransformTouched || bCameraPoseTouched);
		Report->SetStringField(TEXT("bridge_binding_policy"), TEXT("Current MetaSound context for this PIE audio device, resolved on every startup poll; no cached legacy fallback."));
		Report->SetBoolField(TEXT("waited_for_graph_context"), bWaitedForGraphContext);
		Report->SetNumberField(TEXT("graph_bridge_acquisitions"), GraphBridgeAcquisitions);
		Report->SetStringField(TEXT("run_id"), RunId);
		Report->SetStringField(TEXT("target_map_package"), TargetMapPackage);
		Report->SetStringField(TEXT("loaded_world_package"), LoadedWorldPackage);
		Report->SetNumberField(TEXT("pie_world_count"), ObservedPIEWorldCount);
		Report->SetBoolField(TEXT("scene_owners_resolved"), bSceneOwnersResolved);
		Report->SetNumberField(TEXT("bake_volume_count"), BakeVolumeCount);
		Report->SetStringField(TEXT("bake_volume_path"), GetPathNameSafe(BakeVolume.Get()));
		Report->SetStringField(TEXT("bake_bounds_path"), GetPathNameSafe(BakeBounds.Get()));
		Report->SetObjectField(TEXT("bake_bounds_world_aabb_min_cm"), IMRuralPIEOperationalVectorToJson(BakeBoundsBox.Min));
		Report->SetObjectField(TEXT("bake_bounds_world_aabb_max_cm"), IMRuralPIEOperationalVectorToJson(BakeBoundsBox.Max));
		Report->SetStringField(TEXT("bake_asset_path"), BakeAssetPath);
		Report->SetStringField(TEXT("bake_asset_world_package"), BakeAssetWorldPackage);
		Report->SetNumberField(TEXT("bake_asset_format_version"), BakeAssetFormatVersion);
		Report->SetStringField(TEXT("bake_asset_scene_fingerprint"), BakeAssetSceneFingerprint);
		Report->SetStringField(TEXT("bake_asset_payload_digest"), BakeAssetPayloadDigest);
		Report->SetBoolField(TEXT("bake_asset_payload_and_world_validation"), bBakeAssetValid);
		Report->SetStringField(TEXT("bake_asset_validation_error"), BakeValidationError);
		Report->SetStringField(TEXT("bake_validation_scope"), TEXT("Saved asset format, digest, metadata and target world package; the PIE test does not rebake or recapture current scene geometry."));

		Report->SetNumberField(TEXT("player_start_count"), PlayerStartCount);
		Report->SetStringField(TEXT("selected_player_start_path"), PlayerStartPath);
		Report->SetObjectField(TEXT("player_start_location_cm"), IMRuralPIEOperationalVectorToJson(PlayerStartLocation));
		Report->SetStringField(TEXT("player_controller_class"), GetPathNameSafe(Listener.Get() ? Listener->GetClass() : nullptr));
		Report->SetStringField(TEXT("pawn_class"), GetPathNameSafe(PlayerPawn.Get() ? PlayerPawn->GetClass() : nullptr));
		Report->SetObjectField(TEXT("pawn_initial_location_cm"), IMRuralPIEOperationalVectorToJson(InitialPawnLocation));
		Report->SetNumberField(TEXT("pawn_to_player_start_distance_cm"), PawnStartDistanceCm);
		Report->SetNumberField(TEXT("pawn_to_player_start_limit_cm"), MaxPlayerStartDistanceCm);
		Report->SetBoolField(TEXT("pawn_close_to_player_start"), bPawnCloseToPlayerStart);

		TSharedRef<FJsonObject> Ground = MakeShared<FJsonObject>();
		Ground->SetStringField(TEXT("trace_channel"), TEXT("ECC_WorldStatic"));
		Ground->SetObjectField(TEXT("trace_start_cm"), IMRuralPIEOperationalVectorToJson(GroundTraceStart));
		Ground->SetObjectField(TEXT("trace_end_cm"), IMRuralPIEOperationalVectorToJson(GroundTraceEnd));
		Ground->SetBoolField(TEXT("world_static_trace_hit"), bWorldStaticTraceHit);
		Ground->SetNumberField(TEXT("world_static_hit_count"), WorldStaticHitCount);
		Ground->SetStringField(TEXT("first_world_static_actor_class"), FirstWorldStaticHitActorClass);
		Ground->SetStringField(TEXT("first_world_static_component"), FirstWorldStaticHitComponent);
		Ground->SetNumberField(TEXT("landscape_hit_count"), LandscapeHitCount);
		Ground->SetBoolField(TEXT("landscape_collision_hit"), bLandscapeHit);
		Ground->SetStringField(TEXT("landscape_hit_actor"), LandscapeHitActorPath);
		Ground->SetStringField(TEXT("landscape_hit_component"), LandscapeHitComponentPath);
		Ground->SetObjectField(TEXT("landscape_impact_point_cm"), IMRuralPIEOperationalVectorToJson(LandscapeImpactPoint));
		Ground->SetObjectField(TEXT("landscape_impact_normal"), IMRuralPIEOperationalVectorToJson(LandscapeImpactNormal));
		Ground->SetNumberField(TEXT("landscape_hit_distance_cm"), LandscapeImpactDistance);
		Ground->SetBoolField(TEXT("walkability_oracle_available"), bWalkabilityOracleAvailable);
		Ground->SetNumberField(TEXT("pawn_walkable_floor_angle_deg"), WalkableFloorAngle);
		Ground->SetNumberField(TEXT("pawn_walkable_floor_normal_z"), WalkableFloorNormalZ);
		Ground->SetBoolField(TEXT("landscape_slope_walkable"), bLandscapeWalkable);
		Ground->SetBoolField(TEXT("safe_landing_proven"), bGroundSafetyProven);
		Ground->SetBoolField(TEXT("rural_landscape_check_applicable"), !bFixedAudition);
		Ground->SetStringField(TEXT("no_ground_claim"), bFixedAudition
			? TEXT("Rural Landscape landing test is not applicable to this saved room graph observation.") : bGroundSafetyProven
			? TEXT("Landscape collision and pawn walkable slope were observed under the selected PlayerStart.")
			: TEXT("Safe landing is not asserted; see the recorded WorldStatic trace limitation."));
		Report->SetObjectField(TEXT("ground_collision"), Ground);

		Report->SetStringField(TEXT("source_tag"), TargetSourceTag);
		Report->SetStringField(TEXT("source_sound_path"), GetPathNameSafe(SourceAudio.IsValid() ? SourceAudio->Sound.Get() : nullptr));
		Report->SetNumberField(TEXT("tagged_source_actor_count"), TaggedSourceActorCount);
		Report->SetStringField(TEXT("tagged_source_actor_path"), TaggedActorPath);
		Report->SetNumberField(TEXT("source_marker_count"), SourceMarkerCount);
		Report->SetStringField(TEXT("source_marker_path"), SourceMarkerPath);
		Report->SetStringField(TEXT("audio_component_path"), SourceAudioPath);
		Report->SetStringField(TEXT("source_validation_error"), SourceValidationError);
		Report->SetBoolField(TEXT("source_audio_was_observed_playing"), bSourceWasObservedPlaying);
		Report->SetBoolField(TEXT("source_audio_playback_continuous"), bPlaybackContinuous);
		Report->SetBoolField(TEXT("source_audio_playing_at_report"), SourceAudio.IsValid() && SourceAudio->IsPlaying());
		Report->SetObjectField(TEXT("source_initial_world_location_cm"), IMRuralPIEOperationalVectorToJson(OriginalSourceLocation));
		Report->SetObjectField(TEXT("source_plus_100cm_world_location_cm"), IMRuralPIEOperationalVectorToJson(SourceMoveTarget));
		Report->SetBoolField(TEXT("source_initial_inside_bake_bounds"), SourceOriginalInBakeBounds);
		Report->SetBoolField(TEXT("source_plus_100cm_inside_bake_bounds"), SourceTargetInBakeBounds);
		Report->SetBoolField(TEXT("source_transform_restored"), bSourceTransformRestored);

		Report->SetBoolField(TEXT("bridge_found"), Bridge.IsValid());
		Report->SetBoolField(TEXT("bridge_alive"), Bridge.IsValid() && Bridge->Alive.load(std::memory_order_acquire));
		Report->SetBoolField(TEXT("bridge_enabled"), bBridgeEnabled);
		Report->SetNumberField(TEXT("bridge_world_generation"), static_cast<double>(BridgeWorldGeneration));
		Report->SetNumberField(TEXT("bridge_render_routes"), BridgeRenderRoutes);
		Report->SetNumberField(TEXT("rendered_blocks_at_ready"), static_cast<double>(InitialRenderedBlocks));
		Report->SetNumberField(TEXT("rendered_blocks_at_report"), Bridge.IsValid() ? static_cast<double>(Bridge->RenderedBlocks.load(std::memory_order_acquire)) : 0.0);
		Report->SetNumberField(TEXT("snapshot_pushes_at_report"), Bridge.IsValid() ? static_cast<double>(Bridge->SnapshotProbePushes.load(std::memory_order_acquire)) : 0.0);
		Report->SetNumberField(TEXT("initial_snapshot_index"), static_cast<double>(OriginalSnapshotIndex));
		Report->SetNumberField(TEXT("initial_listener_spatial_version"), static_cast<double>(OriginalSpatialVersion));
		Report->SetNumberField(TEXT("listener_restored_spatial_version"), static_cast<double>(ListenerReturnedSpatialVersion));
		Report->SetNumberField(TEXT("initial_source_audio_component_id"), static_cast<double>(OriginalSourceAudioId));
		Report->SetObjectField(TEXT("listener_initial_ue_cm"), IMRuralPIEOperationalVectorToJson(OriginalListenerLocation));
		Report->SetObjectField(TEXT("listener_minus_100cm_ue_cm"), IMRuralPIEOperationalVectorToJson(ListenerMoveTarget));
		Report->SetBoolField(TEXT("listener_initial_inside_bake_bounds"), ListenerOriginalInBakeBounds);
		Report->SetBoolField(TEXT("listener_minus_100cm_inside_bake_bounds"), ListenerTargetInBakeBounds);
		Report->SetBoolField(TEXT("listener_restore_requested"), bListenerRestoreRequested);
		Report->SetBoolField(TEXT("cleanup_state_observed"), bCleanupObserved);
		Report->SetArrayField(TEXT("phase_observations"), PhaseObservations);
		Report->SetArrayField(TEXT("failure_reasons"), IMRuralPIEOperationalStringsToJson(Errors));
		Report->SetBoolField(TEXT("recording_started"), false);
		Report->SetBoolField(TEXT("asset_or_map_modified"), false);
		Report->SetStringField(TEXT("terminal_status"), bPreliminaryPass && bCaptureSaved
			? (bObserveReverb ? TEXT("CAPTURED") : TEXT("PASS")) : TEXT("FAIL"));
		Report->SetStringField(TEXT("report_path"), ReportPath);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
		Writer->Close();
		return bSerialized
			&& IFileManager::Get().MakeDirectory(*ReportDirectory, true)
			&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& bCaptureSaved;
	}

	FAutomationTestBase* Test = nullptr;
	int32 Stage = StageLoadMap;
	bool bFinishing = false;
		bool bObserveReverb = false;
		bool bFixedAudition = false;
		bool bMovementObservation = false;
		bool bPressureWindowPrepared = false;
		bool bPressureWindowOpened = false;
		bool bPressureWindowClosed = false;
		bool bPressureWindowQuiescent = false;
		uint32 PressureWindowSnapshotCapacityUsed = 0;
		uint32 PressureWindowWorkerCapacityUsed = 0;
		bool bCameraPoseTouched = false;
		FVector MovementOriginalPawnLocation = FVector::ZeroVector;
		FRotator MovementOriginalControlRotation = FRotator::ZeroRotator;
		FVector MovementTargetLocation(const int32 Index) const
		{
			if (Index == 3) return OriginalListenerLocation + FVector(100, 0, 0);
			if (Index == 5) return OriginalSourceLocation + FVector(1600, 0, 0);
			return OriginalListenerLocation;
		}
		const TCHAR* TargetMapPackage = RuralOperationalTargetMap;
		const TCHAR* TargetSourceTag = RuralSourceTag;
	bool bReverbPhaseArmed = false;
	int32 ReverbPhase = 0;
	uint32 ReverbPhaseEnvironmentBegin = 0, ReverbPhaseSourceBegin = 0, ReverbPhaseTraceBegin = 0;
	uint64 ReverbPhaseNoIRBegin = 0, ReverbPhaseUnderrunBegin = 0, ReverbPhaseRejectedBegin = 0, ReverbPhaseIRBegin = 0;
	uint64 ReverbPreviousSnapshotIndex = 0;
	FIMAcousticMetaSoundContextPtr GraphContext;
	TSharedPtr<FJsonObject> ReverbPhaseObservation;
	TArray<TSharedPtr<FJsonValue>> ReverbPhaseObservations;
	bool bFinalized = false;
	bool bRequestedPass = false;
		bool bRuntimeReady = false;
		bool bWaitedForGraphContext = false;
		int32 GraphBridgeAcquisitions = 0;
	bool bSceneOwnersResolved = false;
	bool bPlayerStartResolved = false;
	bool bSourceWasObservedPlaying = false;
	bool bPlaybackContinuous = true;
	bool bListenerOverrideTouched = false;
	bool bSourceTransformTouched = false;
	bool bSourceTransformRestored = true;
	bool bListenerRestoreRequested = false;
	bool bNeedsCleanupObservation = false;
	bool bCleanupObserved = false;
	bool bPawnCloseToPlayerStart = false;
	bool bWorldStaticTraceHit = false;
	bool bLandscapeHit = false;
	bool bWalkabilityOracleAvailable = false;
	bool bLandscapeWalkable = false;
	bool bGroundSafetyProven = false;
	bool bBakeAssetValid = false;
	bool bBridgeEnabled = false;
	bool SourceOriginalInBakeBounds = false;
	bool SourceTargetInBakeBounds = false;
	bool ListenerOriginalInBakeBounds = false;
	bool ListenerTargetInBakeBounds = false;

	double StartupDeadline = 0.0;
	double PhaseDeadline = 0.0;
	double CleanupDeadline = 0.0;
	double PressureWindowOpenedSeconds = 0.0;
	double PressureWindowClosedSeconds = 0.0;
	double PawnStartDistanceCm = 0.0;
	double LandscapeImpactDistance = -1.0;
	float WalkableFloorAngle = -1.0f;
	float WalkableFloorNormalZ = -1.0f;
	float LandscapeImpactNormalZ = 0.0f;

	uint64 PhaseStartSnapshotIndex = 0;
	uint64 PhaseStartRenderedBlocks = 0;
	uint64 PhaseStartSpatialVersion = 0;
	uint64 OriginalSnapshotIndex = 0;
	uint64 OriginalSpatialVersion = 0;
	uint64 OriginalSourceAudioId = 0;
	uint64 OriginalSourceWorldGeneration = 0;
	uint64 ListenerReturnedSpatialVersion = 0;
	uint64 CleanupStartSnapshotIndex = 0;
	uint64 BridgeWorldGeneration = 0;
	uint64 InitialRenderedBlocks = 0;
	uint64 WorldStaticHitCount = 0;
	uint32 BridgeRenderRoutes = 0;
	int32 ObservedPIEWorldCount = 0;
	int32 BakeVolumeCount = 0;
	int32 PlayerStartCount = 0;
	int32 TaggedSourceActorCount = 0;
	int32 SourceMarkerCount = 0;
	int32 LandscapeHitCount = 0;
	int32 BakeAssetFormatVersion = 0;

	TWeakObjectPtr<UWorld> PIEWorld;
	TWeakObjectPtr<AIMAcousticBakeVolume> BakeVolume;
	TWeakObjectPtr<UBoxComponent> BakeBounds;
	TWeakObjectPtr<UIMAcousticBakeAsset> BakeAsset;
	TWeakObjectPtr<AAmbientSound> TaggedSourceActor;
	TWeakObjectPtr<UIMAcousticSourceComponent> SourceMarker;
	TWeakObjectPtr<UAudioComponent> SourceAudio;
	TWeakObjectPtr<APlayerController> Listener;
	TWeakObjectPtr<APawn> PlayerPawn;
	TWeakObjectPtr<APlayerStart> PlayerStart;

	FBox BakeBoundsBox = FBox(ForceInit);
	FTransform OriginalSourceTransform = FTransform::Identity;
	FVector OriginalSourceLocation = FVector::ZeroVector;
	FVector SourceMoveTarget = FVector::ZeroVector;
	FVector OriginalListenerLocation = FVector::ZeroVector;
	FVector ListenerMoveTarget = FVector::ZeroVector;
	FVector OriginalSourceSDK = FVector::ZeroVector;
	FVector InitialPawnLocation = FVector::ZeroVector;
	FVector PlayerStartLocation = FVector::ZeroVector;
	FVector GroundTraceStart = FVector::ZeroVector;
	FVector GroundTraceEnd = FVector::ZeroVector;
	FVector LandscapeImpactPoint = FVector::ZeroVector;
	FVector LandscapeImpactNormal = FVector::ZeroVector;
	FRotator OriginalListenerRotation = FRotator::ZeroRotator;
	FString LoadedWorldPackage;
	FString BakeAssetPath;
	FString BakeAssetWorldPackage;
	FString BakeAssetSceneFingerprint;
	FString BakeAssetPayloadDigest;
	FString BakeValidationError;
	FString TaggedActorPath;
	FString SourceMarkerPath;
	FString SourceAudioPath;
	FString SourceValidationError;
	FString PlayerStartPath;
	FString FirstWorldStaticHitActorClass;
	FString FirstWorldStaticHitComponent;
	FString LandscapeHitActorPath;
	FString LandscapeHitComponentPath;
	FString PhaseName;
	FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/RuralPIEOperational"), RunId);
	FString ReportPath = FPaths::Combine(ReportDirectory, TEXT("report.json"));
	FIMAcousticSnapshotProbe InitialSnapshot{};
	FIMAcousticSnapshotProbe PhaseStartSnapshot{};
	FIMAcousticSnapshotProbe CleanupStartSnapshot{};
	TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> PhaseObservations;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralPIEOperationalTest,
	"IceMoon.AcousticField.Rural.PersistentCamera3OrdinaryPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FIMAcousticRuralPIEOperationalTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticRuralPIEOperationalTestPrivate::FIMAcousticRuralPIEOperationalCommand(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticRuralGraphReverbTest,
	"IceMoon.AcousticField.Rural.PersistentCamera3GraphReverbObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FIMAcousticRuralGraphReverbTest::RunTest(const FString&)
{
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 16384, 16384);
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticRuralPIEOperationalTestPrivate::FIMAcousticRuralPIEOperationalCommand(this, 0, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticAuditionGraphReverbTest,
	"IceMoon.AcousticField.Audition.FixedListenerGraphReverbObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FIMAcousticAuditionGraphReverbTest::RunTest(const FString&)
{
	// The test owns PIE startup and uses the same preallocated audio-thread
	// capture as the Rural diagnostic; no replacement source or impulse is injected.
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 16384, 16384);
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticRuralPIEOperationalTestPrivate::FIMAcousticRuralPIEOperationalCommand(this, 0, true, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticAuditionStartupMovementObservation,
	"IceMoon.AcousticField.Audition.StartupMovementReverbObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FIMAcousticAuditionStartupMovementObservation::RunTest(const FString&)
{
	// Six 31-second phases fit within the existing 200-second PCM window.
	// Trace capacity is separate and bounded; source PCM is neither reset nor replaced.
	IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, 32768, 32768);
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticRuralPIEOperationalTestPrivate::FIMAcousticRuralPIEOperationalCommand(this, 0, true, true, true));
	return true;
}

#endif
