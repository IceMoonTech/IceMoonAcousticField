#include "IMAcousticBlockCostLedger.h"
#include "AudioDevice.h"
#include "HAL/PlatformTime.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace IMAcousticBlockCostLedgerPrivate
{
// User+kernel cycles of the calling thread (QueryThreadCycleTime); 0 on failure or other platforms. Not convertible to wall time on its own.
uint64 CurrentThreadCpuCycles()
{
#if PLATFORM_WINDOWS
	ULONG64 Cycles = 0;
	return ::QueryThreadCycleTime(::GetCurrentThread(), &Cycles) != 0 ? uint64(Cycles) : 0;
#else
	return 0;
#endif
}
}

bool FIMAcousticBlockCostLedger::Prepare(const FAudioDevice* InClockDevice, double InCallbackSeconds, uint32 InBlocksPerCallback,
	const TArray<int32>& SourceSlots, uint64 InCapacityBlocks)
{
	bArmed.store(false, std::memory_order_release);
	if (!InClockDevice || InCallbackSeconds <= 0.0 || InBlocksPerCallback == 0 || InCapacityBlocks == 0) return false;
	for (int8& Participant : SlotParticipant) Participant = -1;
	SourceParticipantCount = 0;
	for (const int32 Slot : SourceSlots)
	{
		if (Slot < 0 || Slot >= MaxSourceSlots || SlotParticipant[Slot] >= 0) return false;
		SlotParticipant[Slot] = int8(SourceParticipantCount++);
	}
	ClockDevice = InClockDevice;
	InverseCallbackSeconds = 1.0 / InCallbackSeconds;
	GraphBlocksPerCallback = InBlocksPerCallback;
	Capacity = InCapacityBlocks;
	const uint64 Cells = Capacity * uint64(ParticipantCount());
	Positions.reset(new FIMLedgerParticipantPosition[ParticipantCount()]);
	CostCycles.reset(new std::atomic<uint64>[Cells]);
	CpuCycles.reset(new std::atomic<uint64>[Cells]);
	BlockExecutes.reset(new std::atomic<uint32>[Capacity]);
	FirstStart.reset(new std::atomic<uint64>[Capacity]);
	for (uint64 I = 0; I < Cells; ++I)
	{
		CostCycles[I].store(0, std::memory_order_relaxed);
		CpuCycles[I].store(0, std::memory_order_relaxed);
	}
	TotalCpuCycles.store(0, std::memory_order_relaxed);
	TotalWallCyclesWithCpu.store(0, std::memory_order_relaxed);
	for (uint64 I = 0; I < Capacity; ++I)
	{
		BlockExecutes[I].store(0, std::memory_order_relaxed);
		FirstStart[I].store(MAX_uint64, std::memory_order_relaxed);
	}
	bOverflow.store(false, std::memory_order_relaxed);
	BeforeBase.store(0, std::memory_order_relaxed);
	ExtraExecutes.store(0, std::memory_order_relaxed);
	Unmapped.store(0, std::memory_order_relaxed);
	// The callback in progress has already executed some participants, so the first complete callback is the next one.
	BaseCallback = uint64(ClockDevice->GetAudioClock() * InverseCallbackSeconds + 0.5) + 1;
	ArmCycles = FPlatformTime::Cycles64();
	bArmed.store(true, std::memory_order_release);
	return true;
}

int32 FIMAcousticBlockCostLedger::SourceParticipant(int32 Slot) const
{
	return Slot >= 0 && Slot < MaxSourceSlots ? int32(SlotParticipant[Slot]) : -1;
}

uint64 FIMAcousticBlockCostLedger::BeginCpuCycles() const
{
	return bArmed.load(std::memory_order_acquire) ? IMAcousticBlockCostLedgerPrivate::CurrentThreadCpuCycles() : 0;
}

void FIMAcousticBlockCostLedger::Record(int32 Participant, uint64 StartCycles, uint64 Cycles, uint64 BeginCpu)
{
	if (!bArmed.load(std::memory_order_acquire)) return;
	const uint64 EndCpu = BeginCpu ? IMAcousticBlockCostLedgerPrivate::CurrentThreadCpuCycles() : 0;
	const uint64 CpuDelta = EndCpu >= BeginCpu && BeginCpu ? EndCpu - BeginCpu : 0;
	if (Participant < 0 || Participant >= ParticipantCount())
	{
		Unmapped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const uint64 Callback = uint64(ClockDevice->GetAudioClock() * InverseCallbackSeconds + 0.5);
	FIMLedgerParticipantPosition& Position = Positions[Participant];
	uint32 InCallback = 0;
	if (Position.Callback.load(std::memory_order_relaxed) == Callback)
		InCallback = Position.Index.load(std::memory_order_relaxed) + 1;
	Position.Callback.store(Callback, std::memory_order_relaxed);
	Position.Index.store(InCallback, std::memory_order_relaxed);
	if (Callback < BaseCallback)
	{
		BeforeBase.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	uint32 SubBlock = InCallback;
	if (SubBlock >= GraphBlocksPerCallback)
	{
		// More Executes than graph blocks in one callback: keep the cost in the callback's last block and count it, never drop it.
		ExtraExecutes.fetch_add(1, std::memory_order_relaxed);
		SubBlock = GraphBlocksPerCallback - 1;
	}
	const uint64 Block = (Callback - BaseCallback) * GraphBlocksPerCallback + SubBlock;
	if (Block >= Capacity)
	{
		bOverflow.store(true, std::memory_order_release);
		return;
	}
	CostCycles[Block * uint64(ParticipantCount()) + uint64(Participant)].fetch_add(Cycles, std::memory_order_relaxed);
	if (CpuDelta)
	{
		CpuCycles[Block * uint64(ParticipantCount()) + uint64(Participant)].fetch_add(CpuDelta, std::memory_order_relaxed);
		TotalCpuCycles.fetch_add(CpuDelta, std::memory_order_relaxed);
		TotalWallCyclesWithCpu.fetch_add(Cycles, std::memory_order_relaxed);
	}
	BlockExecutes[Block].fetch_add(1, std::memory_order_relaxed);
	uint64 Earliest = FirstStart[Block].load(std::memory_order_relaxed);
	while (StartCycles < Earliest && !FirstStart[Block].compare_exchange_weak(Earliest, StartCycles, std::memory_order_relaxed)) {}
}

uint64 FIMAcousticBlockCostLedger::ParticipantCycles(uint64 Block, int32 Participant) const
{
	return Block < Capacity && Participant >= 0 && Participant < ParticipantCount()
		? CostCycles[Block * uint64(ParticipantCount()) + uint64(Participant)].load(std::memory_order_relaxed) : 0;
}

uint64 FIMAcousticBlockCostLedger::ParticipantCpuCycles(uint64 Block, int32 Participant) const
{
	return Block < Capacity && Participant >= 0 && Participant < ParticipantCount()
		? CpuCycles[Block * uint64(ParticipantCount()) + uint64(Participant)].load(std::memory_order_relaxed) : 0;
}

double FIMAcousticBlockCostLedger::CpuCyclesPerWallUs() const
{
	const double WallUs = FPlatformTime::ToSeconds64(TotalWallCyclesWithCpu.load(std::memory_order_relaxed)) * 1.e6;
	return WallUs > 0.0 ? double(TotalCpuCycles.load(std::memory_order_relaxed)) / WallUs : 0.0;
}

uint32 FIMAcousticBlockCostLedger::BlockParticipantCount(uint64 Block) const
{
	return Block < Capacity ? BlockExecutes[Block].load(std::memory_order_relaxed) : 0;
}

uint64 FIMAcousticBlockCostLedger::BlockFirstStartCycles(uint64 Block) const
{
	return Block < Capacity ? FirstStart[Block].load(std::memory_order_relaxed) : MAX_uint64;
}
