#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/UniquePtr.h"

#include <atomic>

class FRunnableThread;
class FJsonObject;

// Per-operation values use monotonic seconds and bytes. The sampled maxima are
// unset unless the complete operation window has enough valid evidence.
struct FIMAcousticOperationMemoryWindowResult
{
	uint64 Id = 0;
	FString Name;
	double BeginSeconds = 0.0;
	TOptional<double> EndSeconds;
	TOptional<double> ElapsedSeconds;
	double RequestedIntervalSeconds = 0.0;

	uint64 ValidSampleCount = 0;
	uint64 InvalidSampleCount = 0;
	uint64 DroppedSampleCount = 0;

	// Sample times are midpoints, reported relative to BeginSeconds. Internal
	// gap is between adjacent in-window sample midpoints; edge gaps are separate.
	TOptional<double> FirstSampleOffsetSeconds;
	TOptional<double> LastSampleOffsetSeconds;
	TOptional<double> SampleCoverageSpanSeconds;
	TOptional<double> MaxInternalSampleGapSeconds;
	TOptional<double> FirstSampleGapSeconds;
	TOptional<double> LastToEndSampleGapSeconds;
	TOptional<double> MaxSampleGapSeconds;
	TOptional<double> MaxQueryDurationSeconds;

	// Latest valid query whose full interval ended strictly before BeginSeconds.
	// It is diagnostic context only; the sampler never computes an allocation delta.
	TOptional<uint64> BaselineWorkingSetBytes;
	TOptional<uint64> BaselineCommitBytes;
	TOptional<double> BaselineAgeSeconds;

	// On Win64 these maxima are based on FPlatformMemoryStats::UsedPhysical
	// (working set) and UsedVirtual (commit charge), both in bytes.
	bool bMaximaValid = false;
	TOptional<uint64> SampledMaxWorkingSetBytes;
	TOptional<uint64> SampledMaxCommitBytes;
};

struct FIMAcousticOperationMemorySamplerReport
{
	bool bStartAttempted = false;
	bool bConfigurationValid = false;
	bool bConfigurationFailed = false;
	FString ConfigurationFailureReason;
	double RequestedSampleIntervalSeconds = 0.025;
	bool bWorkerStarted = false;
	bool bThreadStartFailed = false;
	uint32 ProcessId = 0;
	// Raw FILETIME creation timestamp in 100 ns units since 1601-01-01 UTC.
	uint64 ProcessCreationTimeFileTime = 0;
	bool bProcessIdentityCaptured = false;
	bool bProcessIdentityVerifiedForAllSamples = false;
	bool bProcessIdentityQueryFailed = false;
	bool bProcessIdentityChanged = false;
	bool bMemoryQueryResultInvalid = false;
	bool bWorkerExitedUnexpectedly = false;
	bool bSampleOverflowed = false;
	bool bHasUnscopedInvalidSample = false;
	// Includes invalid attempts; the 100000 cap applies to attempts incorporated.
	uint64 AcceptedSampleAttemptCount = 0;
	uint64 InvalidSampleCount = 0;
	uint64 DroppedSampleCount = 0;
	uint64 InvalidOperationCallCount = 0;

	// Independent OS process-lifetime diagnostics. These are never substituted
	// for operation-window maxima. On the audited Win64 UE 5.8 path they map to
	// PeakWorkingSetSize and PeakPagefileUsage respectively.
	TOptional<uint64> OsLifetimePeakWorkingSetBytes;
	TOptional<uint64> OsLifetimePeakCommitBytes;

	TArray<FIMAcousticOperationMemoryWindowResult> Operations;
};

// Owner-held sampler. Start and StopAndCollect are serialized lifecycle calls
// from the owner, never from this sampler's worker. BeginOperation/EndOperation
// are safe from any thread; each pair identifies an independent window.
class FIMAcousticOperationMemorySampler final : private FRunnable
{
public:
	static constexpr double DefaultRequestedSampleIntervalSeconds = 0.025;
	static constexpr double MinRequestedSampleIntervalSeconds = 0.001;
	static constexpr double MaxRequestedSampleIntervalSeconds = 1.0;
	static constexpr uint64 MaxAcceptedSampleAttempts = 100000;

	explicit FIMAcousticOperationMemorySampler(
		double InRequestedIntervalSeconds = DefaultRequestedSampleIntervalSeconds);
	~FIMAcousticOperationMemorySampler() override;

	FIMAcousticOperationMemorySampler(const FIMAcousticOperationMemorySampler&) = delete;
	FIMAcousticOperationMemorySampler& operator=(const FIMAcousticOperationMemorySampler&) = delete;

	bool Start();
	uint64 BeginOperation(const FString& Name);
	bool EndOperation(uint64 Id);

	// Requests stop, joins the worker, then snapshots all results. Calling this
	// repeatedly does not restart the worker or change completed window results.
	FIMAcousticOperationMemorySamplerReport StopAndCollect();

private:
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	friend class FIMAcousticOperationMemorySamplerSemanticTest;
	friend class FIMAcousticOperationMemorySamplerThreadSmokeTest;
#endif

	struct FIMOperationState
	{
		uint64 Id = 0;
		FString Name;
		double BeginSeconds = 0.0;
		double EndSeconds = 0.0;
		bool bBeginClockValid = false;
		bool bHasEnd = false;
		bool bEndClockValid = false;

		uint64 ValidSampleCount = 0;
		uint64 InvalidSampleCount = 0;
		uint64 DroppedSampleCount = 0;
		bool bHasValidSample = false;
		double FirstSampleOffsetSeconds = 0.0;
		double LastSampleOffsetSeconds = 0.0;
		double PreviousSampleMidpointSeconds = 0.0;
		double MaxInternalSampleGapSeconds = 0.0;
		bool bHasTimedQuery = false;
		double MaxQueryDurationSeconds = 0.0;
		uint64 MaxWorkingSetBytes = 0;
		uint64 MaxCommitBytes = 0;

		bool bHasBaseline = false;
		double BaselineQueryEndSeconds = 0.0;
		uint64 BaselineWorkingSetBytes = 0;
		uint64 BaselineCommitBytes = 0;
	};

	struct FIMLastValidSample
	{
		bool bSet = false;
		double QueryEndSeconds = 0.0;
		uint64 WorkingSetBytes = 0;
		uint64 CommitBytes = 0;
	};

	uint32 Run() override;
	void Stop() override;
	void PublishSampleAttempt(double QueryBeginSeconds, double QueryEndSeconds,
		uint64 WorkingSetBytes, uint64 CommitBytes,
		uint64 InOsLifetimePeakWorkingSetBytes, uint64 InOsLifetimePeakCommitBytes,
		bool bStatsPlausible);
	static bool IsFullyContained(const FIMOperationState& Operation,
		double QueryBeginSeconds, double QueryEndSeconds);
	static void AccumulateValidSample(FIMOperationState& Operation,
		double QueryBeginSeconds, double QueryEndSeconds,
		uint64 WorkingSetBytes, uint64 CommitBytes);
	void UpdateBaselineIfEarlier(FIMOperationState& Operation,
		double QueryEndSeconds, uint64 WorkingSetBytes, uint64 CommitBytes);

	// DataGuard protects operation bounds and aggregates. GetStats and thread
	// joins are always outside this lock.
	FCriticalSection DataGuard;
	TMap<uint64, FIMOperationState> Operations;
	uint64 NextOperationId = 0;
	uint64 AcceptedSampleAttemptCount = 0;
	uint64 InvalidSampleCount = 0;
	uint64 DroppedSampleCount = 0;
	uint64 InvalidOperationCallCount = 0;
	bool bStartAttempted = false;
	bool bConfigurationValid = false;
	bool bConfigurationFailed = false;
	FString ConfigurationFailureReason;
	double RequestedSampleIntervalSeconds = DefaultRequestedSampleIntervalSeconds;
	bool bWorkerStarted = false;
	bool bThreadStartFailed = false;
	uint32 ProcessId = 0;
	uint64 ProcessCreationTimeFileTime = 0;
	bool bProcessIdentityCaptured = false;
	bool bProcessIdentityQueryFailed = false;
	bool bProcessIdentityChanged = false;
	bool bMemoryQueryResultInvalid = false;
	bool bWorkerExitedUnexpectedly = false;
	bool bSampleOverflowed = false;
	bool bHasUnscopedInvalidSample = false;
	bool bStopAndCollectCalled = false;
	bool bHasOsLifetimeDiagnostics = false;
	uint64 OsLifetimePeakWorkingSetBytes = 0;
	uint64 OsLifetimePeakCommitBytes = 0;
	FIMLastValidSample LastValidSample;

	std::atomic<bool> StopRequested{false};
	TUniquePtr<FRunnableThread> Thread;
};

TSharedRef<FJsonObject> IM_AcousticOperationMemoryReportToJson(
	const FIMAcousticOperationMemorySamplerReport& Report);
#endif // WITH_EDITOR
