#pragma once
#include "HAL/PlatformTime.h"
#include <array>
#include <atomic>

namespace IMAcousticTiming
{
inline void AcousticRecordMaximum(std::atomic<uint64>& Target,uint64 Value)
{
	uint64 Previous=Target.load(std::memory_order_relaxed);
	while(Previous<Value&&!Target.compare_exchange_weak(Previous,Value,std::memory_order_relaxed)){}
}
}

// Opt-in diagnostic histogram. Fixed 10 us bins make p99 an explicit upper
// bound; the final bin is overflow, never silently reported as a valid p99.
// Multiple UE source jobs may publish concurrently. No audio-side allocation.
struct FIMAcousticTiming
{
	static constexpr uint32 BinCount=4096;
	static constexpr double BinMicroseconds=10.0;
	std::array<std::atomic<uint64>,BinCount> Bins{};
	std::atomic<uint64> Count{0},TotalCycles{0},MaxCycles{0};
	uint64 Record(uint64 Start)
	{
		const uint64 Cycles=FPlatformTime::Cycles64()-Start;
		RecordCycles(Cycles);return Cycles;
	}
	void RecordCycles(uint64 Cycles)
	{
		const double Us=FPlatformTime::ToSeconds64(Cycles)*1.e6;
		const uint32 Bin=Us>=double(BinCount-1)*BinMicroseconds?BinCount-1:uint32(Us/BinMicroseconds);
		Bins[Bin].fetch_add(1,std::memory_order_relaxed);TotalCycles.fetch_add(Cycles,std::memory_order_relaxed);
		uint64 Maximum=MaxCycles.load(std::memory_order_relaxed);
		while(Maximum<Cycles&&!MaxCycles.compare_exchange_weak(Maximum,Cycles,std::memory_order_relaxed)){}
		Count.fetch_add(1,std::memory_order_release);
	}
};
struct FIMAcousticTimingScope
{
	FIMAcousticTiming* Timing;uint64 Started;std::atomic<uint64>* Accumulate;
	explicit FIMAcousticTimingScope(FIMAcousticTiming* In,std::atomic<uint64>* InAccumulate=nullptr)
		:Timing(In),Started(In?FPlatformTime::Cycles64():0),Accumulate(InAccumulate){}
	~FIMAcousticTimingScope(){if(Timing){const uint64 Cycles=Timing->Record(Started);if(Accumulate)Accumulate->fetch_add(Cycles,std::memory_order_relaxed);}}
};
