#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include <atomic>

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

enum class EIMWaveConsumerRowKind : uint8 { Accept, Render };

struct FIMWaveConsumerRow
{
	EIMWaveConsumerRowKind Kind = EIMWaveConsumerRowKind::Accept;
	uint64 InstanceID = 0, OperatorID = 0, GraphEpoch = 0, Block = 0, TriggerOrdinal = 0;
	uint64 Cycles = 0, ProxyPointer = 0, DataPointer = 0, OutputReference = 0;
	uint32 DeviceID = MAX_uint32;
	int32 Start = 0, End = 0, SourcePCMOffset = 0, SourcePCMFrames = 0, SourceFrames = 0;
	double Seconds = 0.;
	float SourceRate = 0.f, Pitch = 0.f;
	uint32 SourceChannels = 0;
	FGuid DataGUID;
	FName WaveName, Package, Format;
	bool SetSoundWaveReturned = false, PlayerValid = false, Playing = false, FirstRender = false;
};

struct FIMWaveConsumerGeneratorRow
{
	uint64 ComponentID = 0, InstanceID = 0, GeneratorPointer = 0, Cycles = 0;
	double Seconds = 0.;
	bool Created = false;
};

struct FIMWaveConsumerSink : TSharedFromThis<FIMWaveConsumerSink, ESPMode::ThreadSafe>
{
	static constexpr int32 CapacityRows = 4096;
	static constexpr int32 CapacityPCM = 48000 * 32;
	static constexpr int32 CapacityGenerators = 16;
	TArray<FIMWaveConsumerRow> Rows;
	TArray<float> PCM;
	FIMWaveConsumerGeneratorRow Generators[CapacityGenerators];
	std::atomic<bool> GeneratorReady[CapacityGenerators];
	std::atomic<int32> Published{0}, Failure{0}, InFlight{0}, Operators{0}, ActiveGenerators{0}, GeneratorCount{0};
	std::atomic<uint64> ProducerOperator{0};
	int32 PCMFrames = 0;
	uint64 RecordEpoch = 0;

	explicit FIMWaveConsumerSink(uint64 InEpoch) : RecordEpoch(InEpoch)
	{
		check(IsInGameThread());
		Rows.SetNum(CapacityRows);
		PCM.SetNumUninitialized(CapacityPCM);
		for (int32 Index = 0; Index < CapacityGenerators; ++Index) GeneratorReady[Index].store(false);
	}

	void Fail(int32 Code)
	{
		int32 Expected = 0;
		Failure.compare_exchange_strong(Expected, Code);
	}

	void Append(FIMWaveConsumerRow Row, const float* Samples = nullptr, int32 Frames = 0)
	{
		if (InFlight.fetch_add(1, std::memory_order_acq_rel) != 0)
		{
			Fail(1);
			InFlight.fetch_sub(1, std::memory_order_release);
			return;
		}
		uint64 Expected = 0;
		ProducerOperator.compare_exchange_strong(Expected, Row.OperatorID);
		const int32 Count = Published.load(std::memory_order_relaxed);
		if (ProducerOperator.load() != Row.OperatorID || !Row.InstanceID || Row.DeviceID == MAX_uint32)
			Fail(2);
		else if (Count >= CapacityRows || Frames < 0 || PCMFrames + Frames > CapacityPCM || (Frames && !Samples))
			Fail(3);
		else
		{
			Row.SourcePCMOffset = PCMFrames;
			Row.SourcePCMFrames = Frames;
			if (Frames) FMemory::Memcpy(PCM.GetData() + PCMFrames, Samples, Frames * sizeof(float));
			PCMFrames += Frames;
			Rows[Count] = Row;
			Published.store(Count + 1, std::memory_order_release);
		}
		InFlight.fetch_sub(1, std::memory_order_release);
	}

	void AddGenerator(FIMWaveConsumerGeneratorRow Row)
	{
		const int32 Slot = GeneratorCount.fetch_add(1);
		if (Slot >= CapacityGenerators) { Fail(4); return; }
		Generators[Slot] = Row;
		GeneratorReady[Slot].store(true, std::memory_order_release);
		ActiveGenerators.fetch_add(Row.Created ? 1 : -1);
	}
};

TSharedPtr<FIMWaveConsumerSink, ESPMode::ThreadSafe> IM_AcquireWaveConsumerSink();
void IM_SetWaveConsumerSink(FIMWaveConsumerSink* Sink);
bool IM_WaveConsumerAcquisitionDrained();

#endif
