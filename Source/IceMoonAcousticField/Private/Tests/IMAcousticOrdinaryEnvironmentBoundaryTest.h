#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
struct FIMAcousticOrdinaryCoverageFailure
{
	double Seconds = 0.0;
	float ListenerX = 0.f;
	float ListenerY = 0.f;
	float ListenerZ = 0.f;
	uint32 TotalProbes = 0;
	uint32 VisitedProbes = 0;
	uint32 Overlaps = 0;
	uint32 Blocked = 0;
	uint32 Uncertain = 0;
	uint32 RaycastBlocked = 0;
	uint32 UnsafeProbes = 0;
	uint32 SelectedProbeCap = 0;
	uint32 RejectionMask = 0;
};

bool IM_IsOrdinaryCoverageObservationArmedForTest();
void IM_RecordOrdinaryCoverageFailureForTest(const FIMAcousticOrdinaryCoverageFailure& Probe);

struct FIMAcousticOrdinaryEnvironmentCaptureCounts
{
	static constexpr uint32 TraceCapacity = 4096;

	uint64 SourceBlocks = 0;
	uint64 SourceFrames = 0;
	uint64 EnvironmentBlocks = 0;
	uint64 EnvironmentFrames = 0;
	uint64 NoIRBlocks = 0;
	uint64 PushBlocks = 0;
	uint64 PushFailures = 0;
	uint32 SourceClaims = 0;
	uint32 SourcePublished = 0;
	uint32 EnvironmentPublished = 0;
	uint32 BusPublished = 0;
	uint32 SourcePCMFrames = 0;
	uint32 EnvironmentPCMFrames = 0;
	uint32 EnvironmentPCMBlocks = 0;
	uint64 SnapshotPushes = 0;
	uint64 SnapshotOverflows = 0;
	uint64 WorkerPushes = 0;
	uint64 WorkerOverflows = 0;

	bool operator==(const FIMAcousticOrdinaryEnvironmentCaptureCounts& Other) const;
};
#endif
