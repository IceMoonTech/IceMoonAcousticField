#if WITH_EDITOR
#include "IMAcousticMetaSound.h"
#include "IMAcousticBakeVolume.h"
#include "Tests/IMAcousticAuditionCapture.h"
#include "AudioDevice.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace IMAcousticContinuityTracePrivate
{
FIMAcousticMetaSoundContextPtr Context;
TArray<TSharedPtr<FJsonValue>> Marks;

TSharedRef<FJsonObject> BoxJson(const FBox& Box)
{
	auto Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("min_x"), Box.Min.X);
	Row->SetNumberField(TEXT("min_y"), Box.Min.Y);
	Row->SetNumberField(TEXT("min_z"), Box.Min.Z);
	Row->SetNumberField(TEXT("max_x"), Box.Max.X);
	Row->SetNumberField(TEXT("max_y"), Box.Max.Y);
	Row->SetNumberField(TEXT("max_z"), Box.Max.Z);
	return Row;
}

void Export(UWorld* World)
{
	if (!Context || !Context->Device) return;
	auto Report = MakeShared<FJsonObject>();
	auto& Device = *Context->Device;
	Report->SetNumberField(TEXT("sample_rate"), Device.SampleRate);
	Report->SetNumberField(TEXT("frames_per_block"), FIMAcousticMetaSoundContext::Frames);
	Report->SetNumberField(TEXT("export_qpc_seconds"), FPlatformTime::Seconds());
	Report->SetArrayField(TEXT("marks"), Marks);
	TArray<TSharedPtr<FJsonValue>> Volumes, Geometry;
	for (TActorIterator<AIMAcousticBakeVolume> Actor(World); Actor; ++Actor)
	{
		auto Row = BoxJson(Actor->BakeBounds->Bounds.GetBox());
		Row->SetStringField(TEXT("actor"), Actor->GetName());
		Volumes.Add(MakeShared<FJsonValueObject>(Row));
	}
	for (TActorIterator<AStaticMeshActor> Actor(World); Actor; ++Actor)
	{
		auto* Mesh = Actor->GetStaticMeshComponent();
		if (!Mesh || !Mesh->GetStaticMesh() || Mesh->ComponentHasTag(TEXT("IMAcousticIgnore"))) continue;
		auto Row = BoxJson(Mesh->Bounds.GetBox());
		Row->SetStringField(TEXT("actor"), Actor->GetName());
		Row->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Row->SetStringField(TEXT("mesh"), Mesh->GetStaticMesh()->GetPathName());
		Row->SetBoolField(TEXT("identity_rotation"), Mesh->GetComponentQuat().Equals(FQuat::Identity));
		Row->SetNumberField(TEXT("collision_enabled"), int32(Mesh->GetCollisionEnabled()));
		Geometry.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("scene_volumes"), Volumes);
	Report->SetArrayField(TEXT("scene_geometry"), Geometry);
	TArray<TSharedPtr<FJsonValue>> Sources, Environments, Snapshots, Workers, Bus;
#define IM_TRACE_FIELD(Field) Row->SetNumberField(TEXT(#Field), double(Probe.Field))
	const uint32 SourceCount = Context->CapturedSourceBlockCount.load(std::memory_order_acquire);
	for (uint32 Index = 0; Index < FMath::Min(SourceCount, uint32(Context->CapturedSourceBlocks.Num())); ++Index)
	{
		const auto& Probe = Context->CapturedSourceBlocks[Index];
		auto Row = MakeShared<FJsonObject>();
		IM_TRACE_FIELD(Block); IM_TRACE_FIELD(Voice); IM_TRACE_FIELD(InputSpatialVersion); IM_TRACE_FIELD(OutputSpatialVersion);
		IM_TRACE_FIELD(CallbackAudioComponentId); IM_TRACE_FIELD(ResultAudioComponentId); IM_TRACE_FIELD(ResultWorldGeneration);
		IM_TRACE_FIELD(ResultSpatialVersion); IM_TRACE_FIELD(ResultVoiceGeneration); IM_TRACE_FIELD(ResultSequence);
		IM_TRACE_FIELD(SnapshotCaptured); IM_TRACE_FIELD(ConsumedSeconds); IM_TRACE_FIELD(AgeMs);
		IM_TRACE_FIELD(ListenerX); IM_TRACE_FIELD(ListenerY); IM_TRACE_FIELD(ListenerZ); IM_TRACE_FIELD(DistanceGain);
		IM_TRACE_FIELD(Routes); IM_TRACE_FIELD(DirectValid); IM_TRACE_FIELD(PathValid); IM_TRACE_FIELD(Reject);
		IM_TRACE_FIELD(RenderFailure); IM_TRACE_FIELD(Fallback); IM_TRACE_FIELD(InputDelayPriming); IM_TRACE_FIELD(RejectDetail);
		IM_TRACE_FIELD(InputEnergy); IM_TRACE_FIELD(ReverbSendEnergy); IM_TRACE_FIELD(DirectEnergy); IM_TRACE_FIELD(PathEnergy); IM_TRACE_FIELD(OutputEnergy);
		IM_TRACE_FIELD(CallbackDistanceCm); IM_TRACE_FIELD(DegradedGain);
		Sources.Add(MakeShared<FJsonValueObject>(Row));
	}
	const uint32 EnvironmentCount = Context->CapturedEnvironmentTraceBlockCount.load(std::memory_order_acquire);
	for (uint32 Index = 0; Index < FMath::Min(EnvironmentCount, uint32(Context->CapturedBlocks.Num())); ++Index)
	{
		const auto& Probe = Context->CapturedBlocks[Index];
		auto Row = MakeShared<FJsonObject>();
		IM_TRACE_FIELD(Frame); IM_TRACE_FIELD(Sequence); IM_TRACE_FIELD(SpatialVersion); IM_TRACE_FIELD(ExpectedSpatialVersion);
		IM_TRACE_FIELD(Seconds); IM_TRACE_FIELD(CapturedSeconds); IM_TRACE_FIELD(InputSpatialVersion); IM_TRACE_FIELD(DeviceWorldGeneration); IM_TRACE_FIELD(PoolWorldGeneration);
		IM_TRACE_FIELD(Fresh); IM_TRACE_FIELD(RenderRoutes); IM_TRACE_FIELD(HasCurrent); IM_TRACE_FIELD(DelayPriming);
		IM_TRACE_FIELD(Enabled); IM_TRACE_FIELD(RenderAttempted); IM_TRACE_FIELD(Rendered);
		Environments.Add(MakeShared<FJsonValueObject>(Row));
	}
	for (uint64 Index = 0; Index < FMath::Min(Device.SnapshotProbePushes.load(std::memory_order_acquire), uint64(Device.ProbeSnapshotCapacity)); ++Index)
	{
		if (Device.SnapshotDone[Index].load(std::memory_order_acquire) != Index + 1) break;
		const auto& Probe = Device.SnapshotProbes[Index];
		auto Row = MakeShared<FJsonObject>();
		IM_TRACE_FIELD(Block); IM_TRACE_FIELD(WorldGeneration); IM_TRACE_FIELD(SpatialVersion); IM_TRACE_FIELD(CapturedSeconds); IM_TRACE_FIELD(SubmitSeconds);
		IM_TRACE_FIELD(ListenerUEX); IM_TRACE_FIELD(ListenerUEY); IM_TRACE_FIELD(ListenerUEZ);
		IM_TRACE_FIELD(Source0X); IM_TRACE_FIELD(Source0Y); IM_TRACE_FIELD(Source0Z); IM_TRACE_FIELD(Source0AudioId);
		IM_TRACE_FIELD(NumSources); IM_TRACE_FIELD(Submitted); IM_TRACE_FIELD(FailCode);
		Snapshots.Add(MakeShared<FJsonValueObject>(Row));
	}
	for (uint64 Index = 0; Index < FMath::Min(Device.WorkerProbePushes.load(std::memory_order_acquire), uint64(Device.ProbeWorkerCapacity)); ++Index)
	{
		if (Device.WorkerDone[Index].load(std::memory_order_acquire) != Index + 1) break;
		const auto& Probe = Device.WorkerProbes[Index];
		auto Row = MakeShared<FJsonObject>();
		IM_TRACE_FIELD(Block); IM_TRACE_FIELD(WorldGeneration); IM_TRACE_FIELD(LoopStartSeconds); IM_TRACE_FIELD(SnapshotCaptured);
		IM_TRACE_FIELD(SnapValid); IM_TRACE_FIELD(SnapReason); IM_TRACE_FIELD(NumInputs); IM_TRACE_FIELD(ResultsPublished);
		IM_TRACE_FIELD(EvalStartSeconds); IM_TRACE_FIELD(EvalEndSeconds); IM_TRACE_FIELD(EvalOk); IM_TRACE_FIELD(PushSeconds); IM_TRACE_FIELD(PushOk);
		IM_TRACE_FIELD(ReverbAttempt); IM_TRACE_FIELD(ReverbOk); IM_TRACE_FIELD(ReverbStartSeconds); IM_TRACE_FIELD(ReverbEndSeconds);
		IM_TRACE_FIELD(ReverbSequence); IM_TRACE_FIELD(ReverbCaptured); IM_TRACE_FIELD(ReverbSpatialVersion); IM_TRACE_FIELD(ReverbSlot);
		IM_TRACE_FIELD(ReverbReadyBeforeSeconds); IM_TRACE_FIELD(ReverbReadyAfterSeconds); IM_TRACE_FIELD(WaitEndSeconds);
		IM_TRACE_FIELD(FirstVoice); IM_TRACE_FIELD(FirstAudioId); IM_TRACE_FIELD(FirstVoiceGen); IM_TRACE_FIELD(FirstSeq);
		Workers.Add(MakeShared<FJsonValueObject>(Row));
	}
	const uint32 BusCount = Context->CapturedBusReadCount.load(std::memory_order_acquire);
	for (uint32 Index = 0; Index < FMath::Min(BusCount, uint32(Context->CapturedBusReads.Num())); ++Index)
	{
		const auto& Probe = Context->CapturedBusReads[Index];
		auto Row = MakeShared<FJsonObject>();
		IM_TRACE_FIELD(Ordinal); IM_TRACE_FIELD(Frames); IM_TRACE_FIELD(InputCount); IM_TRACE_FIELD(ActiveCount); IM_TRACE_FIELD(MinAvailable); IM_TRACE_FIELD(MaxAvailable);
		IM_TRACE_FIELD(BlockingSlot); IM_TRACE_FIELD(BlockingAudioId); IM_TRACE_FIELD(UnderrunsBefore); IM_TRACE_FIELD(UnderrunsAfter);
		IM_TRACE_FIELD(PushBlocks); IM_TRACE_FIELD(PushFailures); IM_TRACE_FIELD(BeginSeconds); IM_TRACE_FIELD(EndSeconds);
		IM_TRACE_FIELD(Result); IM_TRACE_FIELD(ReturnedFresh); IM_TRACE_FIELD(TransportFailedBefore); IM_TRACE_FIELD(TransportFailedAfter);
		Bus.Add(MakeShared<FJsonValueObject>(Row));
	}
#undef IM_TRACE_FIELD
	Report->SetArrayField(TEXT("source"), Sources);
	Report->SetArrayField(TEXT("environment"), Environments);
	Report->SetArrayField(TEXT("snapshots"), Snapshots);
	Report->SetArrayField(TEXT("workers"), Workers);
	Report->SetArrayField(TEXT("bus"), Bus);
	Report->SetNumberField(TEXT("source_claims"), Context->CapturedSourceBlockClaimCount.load());
	Report->SetNumberField(TEXT("source_capacity"), Context->CapturedSourceBlocks.Num());
	Report->SetNumberField(TEXT("environment_capacity"), Context->CapturedBlocks.Num());
	Report->SetNumberField(TEXT("snapshot_overflows"), Device.SnapshotProbeOverflows.load());
	Report->SetNumberField(TEXT("worker_overflows"), Device.WorkerProbeOverflows.load());
	Report->SetBoolField(TEXT("bus_overflow"), Context->CapturedBusReadOverflow.load());
	FString Text;
	const FString File = FPaths::Combine(IMAcousticAuditionCapture::OutputDirectory(), TEXT("IMContinuityTrace.json"));
	if (!FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Text))
		|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		UE_LOG(LogTemp, Error, TEXT("Spatial continuity trace export failed."));
}

FAutoConsoleCommandWithWorldAndArgs Command(TEXT("IM.AcousticField.SpatialTrace"), TEXT("prepare before Play, mark label during Play, export after recording, disable after Stop."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Arguments, UWorld* World)
	{
		check(IsInGameThread());
		if (Arguments.IsEmpty()) return;
		if (Arguments[0] == TEXT("prepare"))
		{
			Context.Reset();
			Marks.Reset();
			const uint32 Capacity = FMath::DivideAndRoundUp(300u * 48000u, FIMAcousticMetaSoundContext::Frames);
			IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(true, Capacity, Capacity, false, true, false);
			return;
		}
		if (Arguments[0] == TEXT("disable"))
		{
			IMAcousticMetaSound::EnableAcousticMetaSoundCaptureForTest(false);
			Context.Reset();
			return;
		}
		if (!World || !World->GetAudioDeviceRaw()) return;
		if (!Context) Context = IMAcousticMetaSound::FindAcousticMetaSoundContext(World->GetAudioDeviceRaw()->DeviceID);
		if (!Context || !Context->Device) return;
		if (Arguments[0] == TEXT("export")) { Export(World); return; }
		if (Arguments[0] != TEXT("mark") || Arguments.Num() != 2) return;
		Context->Device->ProfilingEnabled.store(true, std::memory_order_release);
		auto Mark = MakeShared<FJsonObject>();
		Mark->SetStringField(TEXT("label"), Arguments[1]);
		Mark->SetNumberField(TEXT("qpc_seconds"), FPlatformTime::Seconds());
		Mark->SetNumberField(TEXT("world_seconds"), World->GetTimeSeconds());
		Mark->SetNumberField(TEXT("source_blocks"), Context->SourceBlocks.load());
		Mark->SetNumberField(TEXT("environment_blocks"), Context->EnvironmentBlocks.load());
		Mark->SetNumberField(TEXT("listener_version"), Context->ListenerSpatialVersion.load());
		Marks.Add(MakeShared<FJsonValueObject>(Mark));
	}));
}
#endif
