#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

// Explicit stall diagnostics only, called on GT or the dedicated worker. Never
// called by audio RT. Wall seconds use UE QPC; CPU time is cumulative kernel +
// user in 100ns units. Raw thread cycles are not converted using QPC frequency.
struct FIMAcousticStallThreadClock
{
	double Seconds = 0.0;
	uint64 Cpu100ns = 0, CpuCycles = 0;
	uint32 ThreadId = 0;
	uint8 Valid = 0; // bit 0: GetThreadTimes; bit 1: QueryThreadCycleTime
};

inline FIMAcousticStallThreadClock IM_AcousticReadStallThreadClock()
{
	FIMAcousticStallThreadClock Result;
#if PLATFORM_WINDOWS
	Result.ThreadId = ::GetCurrentThreadId();
	FILETIME Created{}, Exited{}, Kernel{}, User{};
	if (::GetThreadTimes(::GetCurrentThread(), &Created, &Exited, &Kernel, &User))
	{
		Result.Cpu100ns = (uint64(Kernel.dwHighDateTime) << 32 | Kernel.dwLowDateTime)
			+ (uint64(User.dwHighDateTime) << 32 | User.dwLowDateTime);
		Result.Valid |= 1;
	}
	ULONG64 Cycles = 0;
	if (::QueryThreadCycleTime(::GetCurrentThread(), &Cycles))
	{
		Result.CpuCycles = Cycles;
		Result.Valid |= 2;
	}
#endif
	Result.Seconds = FPlatformTime::Seconds();
	return Result;
}

inline bool IM_AcousticStallClockPairValid(const FIMAcousticStallThreadClock& Before,
	const FIMAcousticStallThreadClock& After)
{
	// GetThreadTimes is coarse; never reject a short interval because its CPU
	// delta exceeds wall time by one accounting tick. Only validate the pair.
	return Before.Valid == 3 && After.Valid == 3 && Before.ThreadId != 0
		&& Before.ThreadId == After.ThreadId && Before.Seconds > 0.0
		&& Before.Seconds <= After.Seconds && Before.Cpu100ns <= After.Cpu100ns
		&& Before.CpuCycles <= After.CpuCycles;
}
