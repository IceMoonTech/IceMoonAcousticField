#include "Tests/IMAcousticAuditionCapture.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include <atomic>

namespace IMAcousticAuditionCapturePrivate
{
struct FIMAcousticAuditionBuffer
{
	TArray<float> Samples;
	std::atomic<uint32> Frames{0};
	std::atomic<int32> SampleRate{0};
	int32 Channels = 1;
};

struct FIMAcousticAuditionListener
{
	double Seconds = 0.0;
	FVector Desired = FVector::ZeroVector, Actual = FVector::ZeroVector;
	float SweepRadiusCm = 0.f;
	FName BlockingActor;
};

FIMAcousticAuditionBuffer Buffers[uint8(EIMAcousticAuditionStage::Count)];
TArray<FIMAcousticAuditionListener> ListenerProbes;
std::atomic<bool> Enabled{false};
std::atomic<int32> Writers{0};
std::atomic<int32> Contacts{0};
std::atomic<uint32> Generation{0};
uint32 ContactFrames[256]{};
double ContactSeconds[256]{};
double BeginSeconds = 0.0, EndSeconds = 0.0;
uint32 RenderedBlocks = 0, FallbackBlocks = 0;
// Every fallback block with its wall-clock time, block index within the pass and reject reason (the pass has far fewer than this many).
struct FIMAcousticAuditionFallback
{
	double Seconds = 0.0;
	uint32 Block = 0;
	uint8 Reject = 0;
};
constexpr int32 MaximumFallbackRows = 512;
FIMAcousticAuditionFallback FallbackRows[MaximumFallbackRows];
float MinimumDistanceCm = MAX_flt, MaximumDistanceCm = 0.f, MinimumDistanceGain = MAX_flt, MaximumDistanceGain = 0.f;
float MinimumAir[3]{1.f, 1.f, 1.f};
float MinimumOcclusion = 1.f;
FString CurrentLabel;
// The longest pass the buffers hold. An audio pass runs in real time (about 40 s); the fixed-step video pass renders 1148 frames and measured
// 89.9 s of wall clock on the Mini while the audio engine keeps running (job IMAcousticR2-20261021-02, end_qpc - begin_qpc). The buffers
// overflowed at the former 75 s, so End() reported a failed capture. 120 s holds the measured pass with a third of margin.
constexpr int32 MaximumPassSeconds = 120;
const TCHAR* Names[] = {TEXT("wave_pitch"), TEXT("wave_gain"), TEXT("legacy_presence"), TEXT("presence"), TEXT("uncompensated_presence"), TEXT("spatial"), TEXT("wet"), TEXT("nearfield")};
}

FString IMAcousticAuditionCapture::OutputDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ClaudeRuntime/AcousticV2Footstep_20261021"));
}

void IMAcousticAuditionCapture::Push(EIMAcousticAuditionStage Stage, const float* Left, const float* Right, int32 Frames, int32 SampleRate)
{
#if WITH_EDITOR
	using namespace IMAcousticAuditionCapturePrivate;
	const uint32 ObservedGeneration = Generation.load(std::memory_order_acquire);
	if (!Enabled.load(std::memory_order_acquire)) return;
	Writers.fetch_add(1, std::memory_order_acq_rel);
	if (Enabled.load(std::memory_order_acquire) && ObservedGeneration == Generation.load(std::memory_order_acquire))
	{
		auto& Buffer = Buffers[uint8(Stage)];
		const uint32 Offset = Buffer.Frames.fetch_add(Frames, std::memory_order_relaxed);
		Buffer.SampleRate.store(SampleRate, std::memory_order_relaxed);
		if (uint64(Offset + Frames) * Buffer.Channels <= uint64(Buffer.Samples.Num()))
			for (int32 Frame = 0; Frame < Frames; ++Frame)
			{
				Buffer.Samples[(Offset + Frame) * Buffer.Channels] = Left[Frame];
				if (Buffer.Channels == 2) Buffer.Samples[(Offset + Frame) * 2 + 1] = Right[Frame];
			}
	}
	Writers.fetch_sub(1, std::memory_order_release);
#endif
}

void IMAcousticAuditionCapture::Contact(int32 FrameInBlock)
{
#if WITH_EDITOR
	using namespace IMAcousticAuditionCapturePrivate;
	const uint32 ObservedGeneration = Generation.load(std::memory_order_acquire);
	if (!Enabled.load(std::memory_order_acquire)) return;
	Writers.fetch_add(1, std::memory_order_acq_rel);
	if (Enabled.load(std::memory_order_acquire) && ObservedGeneration == Generation.load(std::memory_order_acquire))
	{
		const int32 Index = Contacts.fetch_add(1, std::memory_order_relaxed);
		if (Index < UE_ARRAY_COUNT(ContactFrames))
		{
			ContactFrames[Index] = Buffers[uint8(EIMAcousticAuditionStage::WaveGain)].Frames.load(std::memory_order_relaxed) + FrameInBlock;
			ContactSeconds[Index] = FPlatformTime::Seconds();
		}
	}
	Writers.fetch_sub(1, std::memory_order_release);
#endif
}

void IMAcousticAuditionCapture::Spatial(bool bRendered, float DistanceCm, float DistanceGain, const float* AirAbsorption, float Occlusion, uint8 Reject)
{
#if WITH_EDITOR
	using namespace IMAcousticAuditionCapturePrivate;
	const uint32 ObservedGeneration = Generation.load(std::memory_order_acquire);
	if (!Enabled.load(std::memory_order_acquire)) return;
	Writers.fetch_add(1, std::memory_order_acq_rel);
	if (Enabled.load(std::memory_order_acquire) && ObservedGeneration == Generation.load(std::memory_order_acquire))
	{
		MinimumDistanceCm = FMath::Min(MinimumDistanceCm, DistanceCm);
		MaximumDistanceCm = FMath::Max(MaximumDistanceCm, DistanceCm);
		if (bRendered)
		{
			++RenderedBlocks;
			MinimumDistanceGain = FMath::Min(MinimumDistanceGain, DistanceGain);
			MaximumDistanceGain = FMath::Max(MaximumDistanceGain, DistanceGain);
			for (int32 Band = 0; Band < 3; ++Band) MinimumAir[Band] = FMath::Min(MinimumAir[Band], AirAbsorption[Band]);
			MinimumOcclusion = FMath::Min(MinimumOcclusion, Occlusion);
		}
		else
		{
			if (FallbackBlocks < uint32(MaximumFallbackRows)) FallbackRows[FallbackBlocks] = {FPlatformTime::Seconds(), RenderedBlocks + FallbackBlocks, Reject};
			++FallbackBlocks;
		}
	}
	Writers.fetch_sub(1, std::memory_order_release);
#endif
}

void IMAcousticAuditionCapture::Listener(const FVector& Desired, const FVector& Actual, float SweepRadiusCm, FName BlockingActor)
{
#if WITH_EDITOR
	using namespace IMAcousticAuditionCapturePrivate;
	check(IsInGameThread());
	if (Enabled.load(std::memory_order_acquire))
		ListenerProbes.Add({FPlatformTime::Seconds(), Desired, Actual, SweepRadiusCm, BlockingActor});
#endif
}

bool IMAcousticAuditionCapture::Begin(const FString& Label)
{
	using namespace IMAcousticAuditionCapturePrivate;
	check(IsInGameThread());
	if (Enabled.load() || Label != FPaths::GetCleanFilename(Label)) return false;
	Generation.fetch_add(1, std::memory_order_acq_rel);
	CurrentLabel = Label;
	RenderedBlocks = FallbackBlocks = 0;
	MinimumDistanceCm = MinimumDistanceGain = MAX_flt;
	MaximumDistanceCm = MaximumDistanceGain = 0.f;
	MinimumOcclusion = 1.f;
	for (float& Value : MinimumAir) Value = 1.f;
	for (uint8 Index = 0; Index < uint8(EIMAcousticAuditionStage::Count); ++Index)
	{
		auto& Buffer = Buffers[Index];
		Buffer.Channels = Index >= uint8(EIMAcousticAuditionStage::Spatial) ? 2 : 1;
		Buffer.Samples.SetNumZeroed(MaximumPassSeconds * 48000 * Buffer.Channels);
		Buffer.Frames.store(0);
		Buffer.SampleRate.store(0);
	}
	Contacts.store(0);
	ListenerProbes.Reset();
	BeginSeconds = FPlatformTime::Seconds();
	Enabled.store(true, std::memory_order_release);
	return true;
}

bool IMAcousticAuditionCapture::End()
{
	using namespace IMAcousticAuditionCapturePrivate;
	check(IsInGameThread());
	Enabled.store(false, std::memory_order_release);
	EndSeconds = FPlatformTime::Seconds();
	while (Writers.load(std::memory_order_acquire)) FPlatformProcess::SleepNoStats(.001f);
	const FString Directory = FPaths::Combine(OutputDirectory(), CurrentLabel);
	IFileManager::Get().MakeDirectory(*Directory, true);
	auto Report = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Stages;
	bool Good = Contacts.load() < int32(UE_ARRAY_COUNT(ContactFrames));
	for (uint8 Index = 0; Index < uint8(EIMAcousticAuditionStage::Count); ++Index)
	{
		const auto& Buffer = Buffers[Index];
		const uint32 Frames = Buffer.Frames.load();
		const uint64 Samples = uint64(Frames) * Buffer.Channels;
		if (Samples > uint64(Buffer.Samples.Num())) { Good = false; continue; }
		const FString File = FPaths::Combine(Directory, FString(Names[Index]) + TEXT(".f32"));
		Good &= FFileHelper::SaveArrayToFile(TArrayView<const uint8>(reinterpret_cast<const uint8*>(Buffer.Samples.GetData()), int32(Samples * sizeof(float))), *File);
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Names[Index]);
		Row->SetStringField(TEXT("file"), File);
		Row->SetNumberField(TEXT("frames"), Frames);
		Row->SetNumberField(TEXT("channels"), Buffer.Channels);
		Row->SetNumberField(TEXT("sample_rate"), Buffer.SampleRate.load());
		Stages.Add(MakeShared<FJsonValueObject>(Row));
	}
	TArray<TSharedPtr<FJsonValue>> Marks;
	TArray<TSharedPtr<FJsonValue>> ContactTimes;
	for (int32 Index = 0; Index < FMath::Min(Contacts.load(), int32(UE_ARRAY_COUNT(ContactFrames))); ++Index)
	{
		Marks.Add(MakeShared<FJsonValueNumber>(ContactFrames[Index]));
		ContactTimes.Add(MakeShared<FJsonValueNumber>(ContactSeconds[Index]));
	}
	Report->SetArrayField(TEXT("stages"), Stages);
	Report->SetArrayField(TEXT("contact_frames"), Marks);
	Report->SetArrayField(TEXT("contact_qpc_seconds"), ContactTimes);
	Report->SetNumberField(TEXT("begin_qpc_seconds"), BeginSeconds);
	Report->SetNumberField(TEXT("end_qpc_seconds"), EndSeconds);
	TArray<TSharedPtr<FJsonValue>> ListenerRows;
	for (const auto& Probe : ListenerProbes)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("qpc_seconds"), Probe.Seconds);
		Row->SetNumberField(TEXT("desired_x"), Probe.Desired.X);
		Row->SetNumberField(TEXT("desired_y"), Probe.Desired.Y);
		Row->SetNumberField(TEXT("desired_z"), Probe.Desired.Z);
		Row->SetNumberField(TEXT("actual_x"), Probe.Actual.X);
		Row->SetNumberField(TEXT("actual_y"), Probe.Actual.Y);
		Row->SetNumberField(TEXT("actual_z"), Probe.Actual.Z);
		Row->SetNumberField(TEXT("sweep_radius_cm"), Probe.SweepRadiusCm);
		Row->SetStringField(TEXT("blocking_actor"), Probe.BlockingActor.ToString());
		ListenerRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("listener_probes"), ListenerRows);
	Report->SetNumberField(TEXT("rendered_blocks"), RenderedBlocks);
	Report->SetNumberField(TEXT("fallback_blocks"), FallbackBlocks);
	TArray<TSharedPtr<FJsonValue>> FallbackJson;
	for (uint32 Index = 0; Index < FMath::Min(FallbackBlocks, uint32(MaximumFallbackRows)); ++Index)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("qpc_seconds"), FallbackRows[Index].Seconds);
		Row->SetNumberField(TEXT("block"), FallbackRows[Index].Block);
		Row->SetNumberField(TEXT("reject"), FallbackRows[Index].Reject);
		FallbackJson.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("fallback_rows"), FallbackJson);
	Report->SetNumberField(TEXT("minimum_distance_cm"), MinimumDistanceCm);
	Report->SetNumberField(TEXT("maximum_distance_cm"), MaximumDistanceCm);
	Report->SetNumberField(TEXT("minimum_distance_gain"), MinimumDistanceGain);
	Report->SetNumberField(TEXT("maximum_distance_gain"), MaximumDistanceGain);
	Report->SetNumberField(TEXT("minimum_air_low"), MinimumAir[0]);
	Report->SetNumberField(TEXT("minimum_air_mid"), MinimumAir[1]);
	Report->SetNumberField(TEXT("minimum_air_high"), MinimumAir[2]);
	Report->SetNumberField(TEXT("minimum_occlusion"), MinimumOcclusion);
	Report->SetBoolField(TEXT("complete"), Good);
	FString Text;
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Text));
	return FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Directory, TEXT("IMStages.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && Good;
}
