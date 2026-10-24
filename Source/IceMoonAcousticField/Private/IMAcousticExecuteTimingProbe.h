#pragma once

#include <atomic>

#include "CoreMinimal.h"

static_assert(std::atomic<uint64>::is_always_lock_free
	&& std::atomic<uint32>::is_always_lock_free && std::atomic<double>::is_always_lock_free,
	"Execute maximum observation requires lock-free scalar atomics on the audio thread.");

// A single graph consumer owns each probe. GT reads only atomics, so even a
// read overlapping a new maximum is race-free and returns inconclusive.
// This locates a whole-Execute maximum; it is not a callback budget substitute
// and wall-clock cycle time does not distinguish DSP work from preemption.
struct FIMAcousticExecuteMaximumSample
{
	uint64 WholeCycles = 0, StartCycles = 0, BeforeRenderCycles = 0, AfterRenderCycles = 0;
	uint64 AudioComponentId = 0, WorldGeneration = 0, VoiceGeneration = 0;
	uint64 SpatialVersion = 0, Sequence = 0;
	uint32 ThreadId = 0, Slot = MAX_uint32, Fresh = 0;
	double BeginSeconds = 0;

	bool HasOrderedStages() const
	{
		return WholeCycles && StartCycles && BeforeRenderCycles >= StartCycles
			&& AfterRenderCycles >= BeforeRenderCycles
			&& WholeCycles >= AfterRenderCycles - StartCycles;
	}
};

struct FIMAcousticExecuteMaximumProbe
{
	// Sequence and payload share the sequentially consistent atomic order.
	// A reader cannot accept an unchanged even sequence across a new payload
	// write; no non-atomic data or architecture-specific load ordering is used.
	std::atomic<uint64> PublicationSequence{0};
	std::atomic<uint64> WholeCycles{0}, StartCycles{0}, BeforeRenderCycles{0}, AfterRenderCycles{0};
	std::atomic<uint64> AudioComponentId{0}, WorldGeneration{0}, VoiceGeneration{0};
	std::atomic<uint64> SpatialVersion{0}, Sequence{0};
	std::atomic<uint32> ThreadId{0}, Slot{MAX_uint32}, Fresh{0};
	std::atomic<double> BeginSeconds{0};

	void RecordMaximum(const FIMAcousticExecuteMaximumSample& Value)
	{
		// The source consumer or environment consumer is the sole writer.
		// No per-block array, allocation, lock, or retry loop is required.
		if (Value.WholeCycles <= WholeCycles.load(std::memory_order_seq_cst)) return;
		PublicationSequence.fetch_add(1, std::memory_order_seq_cst);
		WholeCycles.store(Value.WholeCycles, std::memory_order_seq_cst);
		StartCycles.store(Value.StartCycles, std::memory_order_seq_cst);
		BeforeRenderCycles.store(Value.BeforeRenderCycles, std::memory_order_seq_cst);
		AfterRenderCycles.store(Value.AfterRenderCycles, std::memory_order_seq_cst);
		AudioComponentId.store(Value.AudioComponentId, std::memory_order_seq_cst);
		WorldGeneration.store(Value.WorldGeneration, std::memory_order_seq_cst);
		VoiceGeneration.store(Value.VoiceGeneration, std::memory_order_seq_cst);
		SpatialVersion.store(Value.SpatialVersion, std::memory_order_seq_cst);
		Sequence.store(Value.Sequence, std::memory_order_seq_cst);
		ThreadId.store(Value.ThreadId, std::memory_order_seq_cst);
		Slot.store(Value.Slot, std::memory_order_seq_cst);
		Fresh.store(Value.Fresh, std::memory_order_seq_cst);
		BeginSeconds.store(Value.BeginSeconds, std::memory_order_seq_cst);
		PublicationSequence.fetch_add(1, std::memory_order_seq_cst);
	}

	bool ReadComplete(FIMAcousticExecuteMaximumSample& Out) const
	{
		const uint64 Before = PublicationSequence.load(std::memory_order_seq_cst);
		if (!Before || (Before & 1u)) return false;
		Out.WholeCycles = WholeCycles.load(std::memory_order_seq_cst);
		Out.StartCycles = StartCycles.load(std::memory_order_seq_cst);
		Out.BeforeRenderCycles = BeforeRenderCycles.load(std::memory_order_seq_cst);
		Out.AfterRenderCycles = AfterRenderCycles.load(std::memory_order_seq_cst);
		Out.AudioComponentId = AudioComponentId.load(std::memory_order_seq_cst);
		Out.WorldGeneration = WorldGeneration.load(std::memory_order_seq_cst);
		Out.VoiceGeneration = VoiceGeneration.load(std::memory_order_seq_cst);
		Out.SpatialVersion = SpatialVersion.load(std::memory_order_seq_cst);
		Out.Sequence = Sequence.load(std::memory_order_seq_cst);
		Out.ThreadId = ThreadId.load(std::memory_order_seq_cst);
		Out.Slot = Slot.load(std::memory_order_seq_cst);
		Out.Fresh = Fresh.load(std::memory_order_seq_cst);
		Out.BeginSeconds = BeginSeconds.load(std::memory_order_seq_cst);
		const uint64 After = PublicationSequence.load(std::memory_order_seq_cst);
		return Before == After && !(After & 1u) && Out.HasOrderedStages();
	}
};
