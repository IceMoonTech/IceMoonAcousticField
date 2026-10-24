#include "IMAcousticOperationMemorySampler.h"

#if WITH_EDITOR
#include "Dom/JsonObject.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Containers/UnrealString.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#include <cmath>
#include <limits>

namespace IMAcousticOperationMemorySamplerPrivate
{
constexpr TCHAR IM_AcousticOperationMemorySamplerThreadName[] = TEXT("IM_AcousticOperationMemorySampler");

void IM_SetJsonNull(FJsonObject& Object, const TCHAR* FieldName)
{
	Object.SetField(FieldName, MakeShared<FJsonValueNull>());
}

void IM_SetFiniteJsonNumber(FJsonObject& Object, const TCHAR* FieldName, double Value)
{
	if (std::isfinite(Value))
	{
		Object.SetNumberField(FieldName, Value);
	}
	else
	{
		IM_SetJsonNull(Object, FieldName);
	}
}

void IM_SetOptionalJsonNumber(FJsonObject& Object, const TCHAR* FieldName,
	const TOptional<double>& Value)
{
	if (Value.IsSet())
	{
		IM_SetFiniteJsonNumber(Object, FieldName, Value.GetValue());
	}
	else
	{
		IM_SetJsonNull(Object, FieldName);
	}
}

void IM_SetOptionalJsonBytes(FJsonObject& Object, const TCHAR* FieldName,
	const TOptional<uint64>& Value)
{
	if (Value.IsSet())
	{
		Object.SetNumberField(FieldName, static_cast<double>(Value.GetValue()));
	}
	else
	{
		IM_SetJsonNull(Object, FieldName);
	}
}

bool IM_AcousticQueryCurrentProcessIdentity(uint32& OutProcessId, uint64& OutCreationTimeFileTime)
{
	OutProcessId = FPlatformProcess::GetCurrentProcessId();
	OutCreationTimeFileTime = 0;
#if PLATFORM_WINDOWS
	FILETIME CreationTime = { 0 };
	FILETIME ExitTime = { 0 };
	FILETIME KernelTime = { 0 };
	FILETIME UserTime = { 0 };
	if (::GetProcessTimes(::GetCurrentProcess(), &CreationTime, &ExitTime, &KernelTime, &UserTime) == 0)
	{
		return false;
	}

	OutCreationTimeFileTime = (uint64(CreationTime.dwHighDateTime) << 32)
		| uint64(CreationTime.dwLowDateTime);
	return OutProcessId != 0 && OutCreationTimeFileTime != 0;
#else
	return false;
#endif
}
}

FIMAcousticOperationMemorySampler::FIMAcousticOperationMemorySampler(
	double InRequestedIntervalSeconds)
	: RequestedSampleIntervalSeconds(InRequestedIntervalSeconds)
{
}

FIMAcousticOperationMemorySampler::~FIMAcousticOperationMemorySampler()
{
	Stop();
	if (Thread)
	{
		// The runnable may still be inside GetStats or publishing its final result.
		// Never destroy either the thread handle or this owner before the join.
		Thread->WaitForCompletion();
		Thread.Reset();
	}
}

bool FIMAcousticOperationMemorySampler::Start()
{
	const bool bIntervalFinite = std::isfinite(RequestedSampleIntervalSeconds);
	const bool bIntervalInRange = bIntervalFinite
		&& RequestedSampleIntervalSeconds >= MinRequestedSampleIntervalSeconds
		&& RequestedSampleIntervalSeconds <= MaxRequestedSampleIntervalSeconds;
	FString IntervalFailureReason;
	if (!bIntervalFinite)
	{
		IntervalFailureReason = TEXT("Requested sample interval must be finite.");
	}
	else if (RequestedSampleIntervalSeconds < MinRequestedSampleIntervalSeconds)
	{
		IntervalFailureReason = TEXT("Requested sample interval is below the 0.001 second minimum.");
	}
	else if (RequestedSampleIntervalSeconds > MaxRequestedSampleIntervalSeconds)
	{
		IntervalFailureReason = TEXT("Requested sample interval exceeds the 1.0 second maximum.");
	}

	{
		FScopeLock Lock(&DataGuard);
		if (bStartAttempted || bStopAndCollectCalled)
		{
			return false;
		}
		bStartAttempted = true;
		bConfigurationValid = bIntervalInRange;
		bConfigurationFailed = !bIntervalInRange;
		ConfigurationFailureReason = MoveTemp(IntervalFailureReason);
		if (!bConfigurationValid)
		{
			return false;
		}
		StopRequested.store(false, std::memory_order_release);
	}

	uint32 StartingProcessId = 0;
	uint64 StartingCreationTimeFileTime = 0;
	const bool bIdentityCaptured = IMAcousticOperationMemorySamplerPrivate::IM_AcousticQueryCurrentProcessIdentity(
		StartingProcessId, StartingCreationTimeFileTime);
	{
		FScopeLock Lock(&DataGuard);
		ProcessId = StartingProcessId;
		ProcessCreationTimeFileTime = StartingCreationTimeFileTime;
		bProcessIdentityCaptured = bIdentityCaptured;
		bProcessIdentityQueryFailed = !bIdentityCaptured;
	}
	if (!bIdentityCaptured)
	{
		return false;
	}

	// Do not hold DataGuard while creating the thread: Run may immediately call
	// GetStats and then publish under that lock.
	Thread.Reset(FRunnableThread::Create(this, IMAcousticOperationMemorySamplerPrivate::IM_AcousticOperationMemorySamplerThreadName,
		0, TPri_BelowNormal));

	{
		FScopeLock Lock(&DataGuard);
		bWorkerStarted = Thread ? true : false;
		bThreadStartFailed = !bWorkerStarted;
	}
	return bWorkerStarted;
}

uint64 FIMAcousticOperationMemorySampler::BeginOperation(const FString& Name)
{
	FScopeLock Lock(&DataGuard);
	if (bStopAndCollectCalled)
	{
		return 0;
	}
	if (StopRequested.load(std::memory_order_acquire)
		|| NextOperationId == std::numeric_limits<uint64>::max())
	{
		++InvalidOperationCallCount;
		return 0;
	}

	FIMOperationState Operation;
	Operation.Id = ++NextOperationId;
	Operation.Name = Name;
	const uint64 Id = Operation.Id;
	Operations.Add(Id, MoveTemp(Operation));

	FIMOperationState& StartedOperation = Operations.FindChecked(Id);
	StartedOperation.BeginSeconds = FPlatformTime::Seconds();
	StartedOperation.bBeginClockValid = std::isfinite(StartedOperation.BeginSeconds);
	if (StartedOperation.bBeginClockValid && LastValidSample.bSet
		&& LastValidSample.QueryEndSeconds < StartedOperation.BeginSeconds)
	{
		StartedOperation.bHasBaseline = true;
		StartedOperation.BaselineQueryEndSeconds = LastValidSample.QueryEndSeconds;
		StartedOperation.BaselineWorkingSetBytes = LastValidSample.WorkingSetBytes;
		StartedOperation.BaselineCommitBytes = LastValidSample.CommitBytes;
	}
	return Id;
}

bool FIMAcousticOperationMemorySampler::EndOperation(uint64 Id)
{
	FScopeLock Lock(&DataGuard);
	if (bStopAndCollectCalled)
	{
		return false;
	}
	if (StopRequested.load(std::memory_order_acquire))
	{
		++InvalidOperationCallCount;
		return false;
	}

	FIMOperationState* Operation = Operations.Find(Id);
	if (Operation == nullptr || Operation->bHasEnd)
	{
		++InvalidOperationCallCount;
		return false;
	}

	Operation->EndSeconds = FPlatformTime::Seconds();
	Operation->bHasEnd = true;
	Operation->bEndClockValid = std::isfinite(Operation->EndSeconds)
		&& Operation->bBeginClockValid
		&& Operation->EndSeconds >= Operation->BeginSeconds;
	return Operation->bEndClockValid;
}

FIMAcousticOperationMemorySamplerReport FIMAcousticOperationMemorySampler::StopAndCollect()
{
	Stop();
	if (Thread)
	{
		// Do not hold DataGuard while waiting; the worker must publish its final
		// query before WaitForCompletion returns.
		Thread->WaitForCompletion();
		Thread.Reset();
	}

	FScopeLock Lock(&DataGuard);
	bStopAndCollectCalled = true;

	FIMAcousticOperationMemorySamplerReport Report;
	Report.bStartAttempted = bStartAttempted;
	Report.bConfigurationValid = bConfigurationValid;
	Report.bConfigurationFailed = bConfigurationFailed;
	Report.ConfigurationFailureReason = ConfigurationFailureReason;
	Report.RequestedSampleIntervalSeconds = RequestedSampleIntervalSeconds;
	Report.bWorkerStarted = bWorkerStarted;
	Report.bThreadStartFailed = bThreadStartFailed;
	Report.ProcessId = ProcessId;
	Report.ProcessCreationTimeFileTime = ProcessCreationTimeFileTime;
	Report.bProcessIdentityCaptured = bProcessIdentityCaptured;
	Report.bProcessIdentityVerifiedForAllSamples = bProcessIdentityCaptured
		&& !bProcessIdentityQueryFailed && !bProcessIdentityChanged
		&& !bWorkerExitedUnexpectedly
		&& AcceptedSampleAttemptCount > 0;
	Report.bProcessIdentityQueryFailed = bProcessIdentityQueryFailed;
	Report.bProcessIdentityChanged = bProcessIdentityChanged;
	Report.bMemoryQueryResultInvalid = bMemoryQueryResultInvalid;
	Report.bWorkerExitedUnexpectedly = bWorkerExitedUnexpectedly;
	Report.bSampleOverflowed = bSampleOverflowed;
	Report.bHasUnscopedInvalidSample = bHasUnscopedInvalidSample;
	Report.AcceptedSampleAttemptCount = AcceptedSampleAttemptCount;
	Report.InvalidSampleCount = InvalidSampleCount;
	Report.DroppedSampleCount = DroppedSampleCount;
	Report.InvalidOperationCallCount = InvalidOperationCallCount;

	if (bHasOsLifetimeDiagnostics)
	{
		Report.OsLifetimePeakWorkingSetBytes = OsLifetimePeakWorkingSetBytes;
		Report.OsLifetimePeakCommitBytes = OsLifetimePeakCommitBytes;
	}

	TArray<uint64> OperationIds;
	Operations.GetKeys(OperationIds);
	OperationIds.Sort();
	Report.Operations.Reserve(OperationIds.Num());
	for (const uint64 Id : OperationIds)
	{
		const FIMOperationState& Operation = Operations.FindChecked(Id);
		FIMAcousticOperationMemoryWindowResult Result;
		Result.Id = Operation.Id;
		Result.Name = Operation.Name;
		Result.BeginSeconds = Operation.BeginSeconds;
		Result.RequestedIntervalSeconds = RequestedSampleIntervalSeconds;

		const bool bHasValidBounds = Operation.bBeginClockValid
			&& Operation.bHasEnd && Operation.bEndClockValid;
		if (Operation.bHasEnd && std::isfinite(Operation.EndSeconds))
		{
			Result.EndSeconds = Operation.EndSeconds;
		}
		if (bHasValidBounds)
		{
			Result.ElapsedSeconds = Operation.EndSeconds - Operation.BeginSeconds;
			Result.ValidSampleCount = Operation.ValidSampleCount;
			Result.InvalidSampleCount = Operation.InvalidSampleCount;
			Result.DroppedSampleCount = Operation.DroppedSampleCount;
			if (Operation.bHasValidSample)
			{
				Result.FirstSampleOffsetSeconds = Operation.FirstSampleOffsetSeconds;
				Result.LastSampleOffsetSeconds = Operation.LastSampleOffsetSeconds;
				// Include both window edges so the reported maximum gap covers
				// the full phase rather than only the sampled interior.
				Result.FirstSampleGapSeconds = Operation.FirstSampleOffsetSeconds;
				Result.LastToEndSampleGapSeconds =
					Operation.EndSeconds - Operation.BeginSeconds - Operation.LastSampleOffsetSeconds;
				Result.MaxSampleGapSeconds = FMath::Max(Operation.FirstSampleOffsetSeconds,
					FMath::Max(Result.LastToEndSampleGapSeconds.GetValue(), Operation.MaxInternalSampleGapSeconds));
				if (Operation.ValidSampleCount > 1)
				{
					const double CoverageSpanSeconds = Operation.LastSampleOffsetSeconds
						- Operation.FirstSampleOffsetSeconds;
					if (std::isfinite(CoverageSpanSeconds) && CoverageSpanSeconds > 0.0)
					{
						Result.SampleCoverageSpanSeconds = CoverageSpanSeconds;
					}
					Result.MaxInternalSampleGapSeconds = Operation.MaxInternalSampleGapSeconds;
				}
			}
			if (Operation.bHasTimedQuery)
			{
				Result.MaxQueryDurationSeconds = Operation.MaxQueryDurationSeconds;
			}
		}
		if (Operation.bBeginClockValid && Operation.bHasBaseline)
		{
			Result.BaselineWorkingSetBytes = Operation.BaselineWorkingSetBytes;
			Result.BaselineCommitBytes = Operation.BaselineCommitBytes;
			Result.BaselineAgeSeconds = Operation.BeginSeconds - Operation.BaselineQueryEndSeconds;
		}

		Result.bMaximaValid = bHasValidBounds && bConfigurationValid && !bConfigurationFailed
			&& bWorkerStarted && !bThreadStartFailed
			&& bProcessIdentityCaptured && !bProcessIdentityQueryFailed && !bProcessIdentityChanged
			&& !bWorkerExitedUnexpectedly
			&& !bSampleOverflowed && !bHasUnscopedInvalidSample
			&& Operation.ValidSampleCount >= 2
			&& Result.SampleCoverageSpanSeconds.IsSet()
			&& Result.MaxSampleGapSeconds.IsSet()
			&& Operation.InvalidSampleCount == 0
			&& Operation.DroppedSampleCount == 0;
		if (Result.bMaximaValid)
		{
			Result.SampledMaxWorkingSetBytes = Operation.MaxWorkingSetBytes;
			Result.SampledMaxCommitBytes = Operation.MaxCommitBytes;
		}
		// TOptional maxima remain unset for zero-sample, open, failed, overflowed,
		// or otherwise invalid windows; callers should serialize those as null.
		Report.Operations.Add(MoveTemp(Result));
	}
	return Report;
}

void FIMAcousticOperationMemorySampler::Stop()
{
	StopRequested.store(true, std::memory_order_release);
}

uint32 FIMAcousticOperationMemorySampler::Run()
{
	bool bRunFailed = false;
	while (!StopRequested.load(std::memory_order_acquire))
	{
		uint32 SampledProcessId = 0;
		uint64 SampledCreationTimeFileTime = 0;
		const bool bIdentityRead = IMAcousticOperationMemorySamplerPrivate::IM_AcousticQueryCurrentProcessIdentity(
			SampledProcessId, SampledCreationTimeFileTime);
		if (!bIdentityRead || SampledProcessId != ProcessId
			|| SampledCreationTimeFileTime != ProcessCreationTimeFileTime)
		{
			FScopeLock Lock(&DataGuard);
			if (!bIdentityRead)
			{
				bProcessIdentityQueryFailed = true;
			}
			else
			{
				bProcessIdentityChanged = true;
			}
			bRunFailed = true;
			break;
		}

		const double QueryBeginSeconds = FPlatformTime::Seconds();
		const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
		const double QueryEndSeconds = FPlatformTime::Seconds();

		// GetStats has no success flag. On the audited Win64 implementation,
		// failed process-counter reads leave zeroed counters and peaks cannot be
		// below current usage. Reject those values instead of using them as maxima.
		const bool bStatsPlausible = MemoryStats.UsedPhysical > 0
			&& MemoryStats.UsedVirtual > 0
			&& MemoryStats.PeakUsedPhysical >= MemoryStats.UsedPhysical
			&& MemoryStats.PeakUsedVirtual >= MemoryStats.UsedVirtual;

		PublishSampleAttempt(QueryBeginSeconds, QueryEndSeconds,
			MemoryStats.UsedPhysical, MemoryStats.UsedVirtual,
			MemoryStats.PeakUsedPhysical, MemoryStats.PeakUsedVirtual,
			bStatsPlausible);

		if (StopRequested.load(std::memory_order_acquire))
		{
			break;
		}

		// Requested interval is start-to-start. If GetStats itself overruns it,
		// the next query starts immediately and the actual gap exposes the overrun.
		const double RemainingSeconds = QueryBeginSeconds + RequestedSampleIntervalSeconds
			- FPlatformTime::Seconds();
		if (!std::isfinite(RemainingSeconds))
		{
			FPlatformProcess::SleepNoStats(static_cast<float>(RequestedSampleIntervalSeconds));
		}
		else if (RemainingSeconds > 0.0)
		{
			FPlatformProcess::SleepNoStats(static_cast<float>(RemainingSeconds));
		}
	}

	{
		FScopeLock Lock(&DataGuard);
		if (bRunFailed || !StopRequested.load(std::memory_order_acquire))
		{
			bWorkerExitedUnexpectedly = true;
		}
	}
	return bRunFailed ? 1 : 0;
}

void FIMAcousticOperationMemorySampler::PublishSampleAttempt(double QueryBeginSeconds,
	double QueryEndSeconds, uint64 WorkingSetBytes, uint64 CommitBytes,
	uint64 InOsLifetimePeakWorkingSetBytes, uint64 InOsLifetimePeakCommitBytes,
	bool bStatsPlausible)
{
	// Consume each timestamp/value tuple online instead of retaining a raw sample
	// array, so the sampler does not grow memory while measuring process memory.
	const bool bTimeValid = std::isfinite(QueryBeginSeconds)
		&& std::isfinite(QueryEndSeconds)
		&& QueryEndSeconds >= QueryBeginSeconds;
	const bool bSampleValid = bTimeValid && bStatsPlausible;

	FScopeLock Lock(&DataGuard);

	if (!bStatsPlausible)
	{
		// FPlatformMemory::GetStats has no success result; this records an
		// implausible returned sample as an explicit query-result failure.
		bMemoryQueryResultInvalid = true;
	}

	if (bStatsPlausible)
	{
		// Keep process-lifetime OS peaks separate from all operation maxima.
		this->OsLifetimePeakWorkingSetBytes = bHasOsLifetimeDiagnostics
			? FMath::Max(this->OsLifetimePeakWorkingSetBytes, InOsLifetimePeakWorkingSetBytes)
			: InOsLifetimePeakWorkingSetBytes;
		this->OsLifetimePeakCommitBytes = bHasOsLifetimeDiagnostics
			? FMath::Max(this->OsLifetimePeakCommitBytes, InOsLifetimePeakCommitBytes)
			: InOsLifetimePeakCommitBytes;
		bHasOsLifetimeDiagnostics = true;
	}

	const bool bWithinSampleBudget = AcceptedSampleAttemptCount < MaxAcceptedSampleAttempts;
	if (bWithinSampleBudget)
	{
		++AcceptedSampleAttemptCount;
	}
	else
	{
		++DroppedSampleCount;
		bSampleOverflowed = true;
	}

	if (!bSampleValid)
	{
		++InvalidSampleCount;
		if (!bTimeValid)
		{
			// Its interval cannot be assigned safely to any operation window.
			bHasUnscopedInvalidSample = true;
		}
	}

	if (bSampleValid && bWithinSampleBudget)
	{
		LastValidSample.bSet = true;
		LastValidSample.QueryEndSeconds = QueryEndSeconds;
		LastValidSample.WorkingSetBytes = WorkingSetBytes;
		LastValidSample.CommitBytes = CommitBytes;
	}

	for (TPair<uint64, FIMOperationState>& Pair : Operations)
	{
		FIMOperationState& Operation = Pair.Value;
		if (!bTimeValid || !Operation.bBeginClockValid)
		{
			continue;
		}

		if (bSampleValid && bWithinSampleBudget && QueryEndSeconds < Operation.BeginSeconds)
		{
			UpdateBaselineIfEarlier(Operation, QueryEndSeconds, WorkingSetBytes, CommitBytes);
			continue;
		}

		if (!IsFullyContained(Operation, QueryBeginSeconds, QueryEndSeconds))
		{
			continue;
		}

		Operation.bHasTimedQuery = true;
		Operation.MaxQueryDurationSeconds = FMath::Max(
			Operation.MaxQueryDurationSeconds, QueryEndSeconds - QueryBeginSeconds);

		if (!bWithinSampleBudget)
		{
			++Operation.DroppedSampleCount;
		}
		if (!bStatsPlausible)
		{
			++Operation.InvalidSampleCount;
		}
		if (bSampleValid && bWithinSampleBudget)
		{
			AccumulateValidSample(Operation, QueryBeginSeconds, QueryEndSeconds,
				WorkingSetBytes, CommitBytes);
		}
	}
}

bool FIMAcousticOperationMemorySampler::IsFullyContained(const FIMOperationState& Operation,
	double QueryBeginSeconds, double QueryEndSeconds)
{
	if (QueryBeginSeconds < Operation.BeginSeconds)
	{
		return false;
	}
	if (!Operation.bHasEnd)
	{
		// While DataGuard is held, EndOperation cannot stamp an end time until
		// after this query's already-recorded QueryEndSeconds.
		return true;
	}
	return Operation.bEndClockValid && QueryEndSeconds <= Operation.EndSeconds;
}

void FIMAcousticOperationMemorySampler::AccumulateValidSample(FIMOperationState& Operation,
	double QueryBeginSeconds, double QueryEndSeconds,
	uint64 WorkingSetBytes, uint64 CommitBytes)
{
	const double MidpointSeconds = QueryBeginSeconds + (QueryEndSeconds - QueryBeginSeconds) * 0.5;
	const double OffsetSeconds = MidpointSeconds - Operation.BeginSeconds;

	if (!Operation.bHasValidSample)
	{
		Operation.bHasValidSample = true;
		Operation.FirstSampleOffsetSeconds = OffsetSeconds;
		Operation.MaxInternalSampleGapSeconds = 0.0;
	}
	else
	{
		Operation.MaxInternalSampleGapSeconds = FMath::Max(
			Operation.MaxInternalSampleGapSeconds,
			MidpointSeconds - Operation.PreviousSampleMidpointSeconds);
	}

	Operation.LastSampleOffsetSeconds = OffsetSeconds;
	Operation.PreviousSampleMidpointSeconds = MidpointSeconds;
	Operation.MaxWorkingSetBytes = FMath::Max(Operation.MaxWorkingSetBytes, WorkingSetBytes);
	Operation.MaxCommitBytes = FMath::Max(Operation.MaxCommitBytes, CommitBytes);
	++Operation.ValidSampleCount;
}

void FIMAcousticOperationMemorySampler::UpdateBaselineIfEarlier(FIMOperationState& Operation,
	double QueryEndSeconds, uint64 WorkingSetBytes, uint64 CommitBytes)
{
	if (!Operation.bHasBaseline || QueryEndSeconds > Operation.BaselineQueryEndSeconds)
	{
		Operation.bHasBaseline = true;
		Operation.BaselineQueryEndSeconds = QueryEndSeconds;
		Operation.BaselineWorkingSetBytes = WorkingSetBytes;
		Operation.BaselineCommitBytes = CommitBytes;
	}
}

TSharedRef<FJsonObject> IM_AcousticOperationMemoryReportToJson(
	const FIMAcousticOperationMemorySamplerReport& Report)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("method"), TEXT("sampled process counters"));
	Json->SetBoolField(TEXT("exact_peak"), false);
	Json->SetBoolField(TEXT("start_attempted"), Report.bStartAttempted);
	Json->SetBoolField(TEXT("configuration_valid"), Report.bConfigurationValid);
	Json->SetBoolField(TEXT("configuration_failed"), Report.bConfigurationFailed);
	Json->SetStringField(TEXT("configuration_failure_reason"), Report.ConfigurationFailureReason);
	IMAcousticOperationMemorySamplerPrivate::IM_SetFiniteJsonNumber(*Json, TEXT("requested_sample_interval_seconds"),
		Report.RequestedSampleIntervalSeconds);
	Json->SetBoolField(TEXT("worker_started"), Report.bWorkerStarted);
	Json->SetBoolField(TEXT("thread_start_failed"), Report.bThreadStartFailed);
	Json->SetNumberField(TEXT("process_id"), static_cast<double>(Report.ProcessId));
	if (Report.bProcessIdentityCaptured)
	{
		Json->SetStringField(TEXT("process_creation_time_filetime_100ns"),
			LexToString(Report.ProcessCreationTimeFileTime));
	}
	else
	{
		IMAcousticOperationMemorySamplerPrivate::IM_SetJsonNull(*Json, TEXT("process_creation_time_filetime_100ns"));
	}
	Json->SetBoolField(TEXT("process_identity_captured"), Report.bProcessIdentityCaptured);
	Json->SetBoolField(TEXT("process_identity_verified_for_all_samples"),
		Report.bProcessIdentityVerifiedForAllSamples);
	Json->SetBoolField(TEXT("process_identity_query_failed"), Report.bProcessIdentityQueryFailed);
	Json->SetBoolField(TEXT("process_identity_changed"), Report.bProcessIdentityChanged);
	Json->SetBoolField(TEXT("memory_query_result_invalid"), Report.bMemoryQueryResultInvalid);
	Json->SetBoolField(TEXT("worker_exited_unexpectedly"), Report.bWorkerExitedUnexpectedly);
	Json->SetBoolField(TEXT("sample_overflowed"), Report.bSampleOverflowed);
	Json->SetBoolField(TEXT("has_unscoped_invalid_sample"), Report.bHasUnscopedInvalidSample);
	Json->SetNumberField(TEXT("accepted_sample_attempt_count"),
		static_cast<double>(Report.AcceptedSampleAttemptCount));
	Json->SetNumberField(TEXT("invalid_sample_count"), static_cast<double>(Report.InvalidSampleCount));
	Json->SetNumberField(TEXT("dropped_sample_count"), static_cast<double>(Report.DroppedSampleCount));
	Json->SetNumberField(TEXT("invalid_operation_call_count"),
		static_cast<double>(Report.InvalidOperationCallCount));
	IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonBytes(*Json, TEXT("os_lifetime_peak_working_set_bytes"),
		Report.OsLifetimePeakWorkingSetBytes);
	IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonBytes(*Json, TEXT("os_lifetime_peak_commit_bytes"),
		Report.OsLifetimePeakCommitBytes);

	TArray<TSharedPtr<FJsonValue>> OperationValues;
	OperationValues.Reserve(Report.Operations.Num());
	for (const FIMAcousticOperationMemoryWindowResult& Operation : Report.Operations)
	{
		TSharedRef<FJsonObject> OperationJson = MakeShared<FJsonObject>();
		OperationJson->SetStringField(TEXT("id"), LexToString(Operation.Id));
		OperationJson->SetStringField(TEXT("name"), Operation.Name);
		IMAcousticOperationMemorySamplerPrivate::IM_SetFiniteJsonNumber(*OperationJson, TEXT("begin_seconds"), Operation.BeginSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("end_seconds"), Operation.EndSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("elapsed_seconds"), Operation.ElapsedSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetFiniteJsonNumber(*OperationJson, TEXT("requested_interval_seconds"),
			Operation.RequestedIntervalSeconds);
		OperationJson->SetNumberField(TEXT("valid_sample_count"),
			static_cast<double>(Operation.ValidSampleCount));
		OperationJson->SetNumberField(TEXT("invalid_sample_count"),
			static_cast<double>(Operation.InvalidSampleCount));
		OperationJson->SetNumberField(TEXT("dropped_sample_count"),
			static_cast<double>(Operation.DroppedSampleCount));
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("first_sample_offset_seconds"),
			Operation.FirstSampleOffsetSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("last_sample_offset_seconds"),
			Operation.LastSampleOffsetSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("sample_coverage_span_seconds"),
			Operation.SampleCoverageSpanSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("max_internal_sample_gap_seconds"),
			Operation.MaxInternalSampleGapSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("first_sample_gap_seconds"),
			Operation.FirstSampleGapSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("last_to_end_sample_gap_seconds"),
			Operation.LastToEndSampleGapSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("max_sample_gap_seconds"),
			Operation.MaxSampleGapSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("max_query_duration_seconds"),
			Operation.MaxQueryDurationSeconds);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonBytes(*OperationJson, TEXT("baseline_working_set_bytes"),
			Operation.BaselineWorkingSetBytes);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonBytes(*OperationJson, TEXT("baseline_commit_bytes"),
			Operation.BaselineCommitBytes);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonNumber(*OperationJson, TEXT("baseline_age_seconds"),
			Operation.BaselineAgeSeconds);
		OperationJson->SetBoolField(TEXT("sampled_maxima_valid"), Operation.bMaximaValid);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonBytes(*OperationJson, TEXT("sampled_max_working_set_bytes"),
			Operation.SampledMaxWorkingSetBytes);
		IMAcousticOperationMemorySamplerPrivate::IM_SetOptionalJsonBytes(*OperationJson, TEXT("sampled_max_commit_bytes"),
			Operation.SampledMaxCommitBytes);
		OperationValues.Add(MakeShared<FJsonValueObject>(OperationJson));
	}
	Json->SetArrayField(TEXT("operations"), OperationValues);
	return Json;
}
#endif // WITH_EDITOR
