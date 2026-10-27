#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include <memory>

class FAudioDevice;

// Optional W3 observation of what one device block costs in total.
//
// The per-participant maxima kept elsewhere (IMAcousticExecuteTimingProbe.h) each come from a different block over the whole run, so their
// sum is not the cost of any real block. This ledger instead adds up the whole-Execute cycles of every participating source operator, the
// environment operator and the bus reader that ran in the same graph block, and keeps one row per block so the worst real block can be
// compared with the block deadline and its neighbours inspected.
//
// Block identity: the mixer device advances its audio clock once per mixer callback, after every generator of that callback has rendered
// (FMixerDevice::OnProcessAudioStream -> UpdateAudioClock), so all Executes of one callback read the same clock value. The callback index is
// that clock divided by the callback period. A callback holds BlocksPerCallback graph blocks and each participant executes once per graph
// block, so the n-th Execute of a participant inside one callback belongs to the n-th graph block of that callback. A participant is only
// ever executed by one thread at a time, which is why its (callback, n) pair needs no lock.
//
// Allocated once on the game thread before the capture flag is raised; the audio threads only do relaxed atomic adds into preallocated rows.
struct FIMAcousticBlockCostLedger
{
	static constexpr int32 MaxSourceSlots = 64; // Same bound as FIMAcousticMetaSoundContext::MaxVoices.

	// Game thread. Clears any earlier observation. Slots are the voice slots whose Execute cost is attributed; environment and bus follow
	// them as the last two participants. Callbacks that began before this call are ignored (their blocks would be partial).
	bool Prepare(const FAudioDevice* InClockDevice, double InCallbackSeconds, uint32 InBlocksPerCallback,
		const TArray<int32>& SourceSlots, uint64 InCapacityBlocks);
	// Audio threads. Safe to call when the ledger is not armed (does nothing).
	// BeginCpuCycles() is read at the start of an Execute and handed back to Record() at its end; the difference is the calling thread's
	// own user+kernel cycles over the Execute. They are processor cycles, not wall time (see EstimatedCpuUs); 0 when unarmed or unsupported.
	uint64 BeginCpuCycles() const;
	void Record(int32 Participant, uint64 StartCycles, uint64 Cycles, uint64 BeginCpu);

	int32 SourceParticipant(int32 Slot) const;
	int32 EnvironmentParticipant() const { return SourceParticipantCount; }
	int32 BusParticipant() const { return SourceParticipantCount + 1; }
	int32 ParticipantCount() const { return SourceParticipantCount + 2; }
	bool IsArmed() const { return bArmed.load(std::memory_order_acquire); }

	// Game thread, after the writers have left the closed window.
	uint64 CapacityBlocks() const { return Capacity; }
	uint64 FirstCallback() const { return BaseCallback; }
	uint32 BlocksPerCallback() const { return GraphBlocksPerCallback; }
	uint64 ParticipantCycles(uint64 Block, int32 Participant) const;
	uint64 ParticipantCpuCycles(uint64 Block, int32 Participant) const;
	// Processor cycles per wall microsecond over every recorded Execute (their wall time is dominated by running, not waiting), used
	// to turn one Execute's CPU cycles into an estimated CPU time. 0 when nothing was recorded or CPU cycles are unavailable.
	double CpuCyclesPerWallUs() const;
	uint32 BlockParticipantCount(uint64 Block) const;
	uint64 BlockFirstStartCycles(uint64 Block) const;
	uint64 ArmedCycles() const { return ArmCycles; }
	bool Overflowed() const { return bOverflow.load(std::memory_order_acquire); }
	uint64 RecordsBeforeFirstCallback() const { return BeforeBase.load(std::memory_order_acquire); }
	uint64 RecordsBeyondCallbackBlocks() const { return ExtraExecutes.load(std::memory_order_acquire); }
	uint64 RecordsUnmapped() const { return Unmapped.load(std::memory_order_acquire); }

private:
	struct FIMLedgerParticipantPosition
	{
		std::atomic<uint64> Callback{MAX_uint64};
		std::atomic<uint32> Index{0};
	};

	const FAudioDevice* ClockDevice = nullptr;
	double InverseCallbackSeconds = 0.0;
	uint32 GraphBlocksPerCallback = 1;
	uint64 BaseCallback = 0;
	uint64 Capacity = 0;
	uint64 ArmCycles = 0;
	int32 SourceParticipantCount = 0;
	int8 SlotParticipant[MaxSourceSlots]{};
	std::unique_ptr<FIMLedgerParticipantPosition[]> Positions;
	std::unique_ptr<std::atomic<uint64>[]> CostCycles;     // [Capacity * ParticipantCount()]
	std::unique_ptr<std::atomic<uint64>[]> CpuCycles;      // [Capacity * ParticipantCount()], thread cycles over the same Executes
	std::atomic<uint64> TotalCpuCycles{0}, TotalWallCyclesWithCpu{0};
	std::unique_ptr<std::atomic<uint32>[]> BlockExecutes;  // [Capacity]
	std::unique_ptr<std::atomic<uint64>[]> FirstStart;     // [Capacity], smallest Execute start (Cycles64), MAX_uint64 when empty
	std::atomic<bool> bArmed{false};
	std::atomic<bool> bOverflow{false};
	std::atomic<uint64> BeforeBase{0}, ExtraExecutes{0}, Unmapped{0};
};
