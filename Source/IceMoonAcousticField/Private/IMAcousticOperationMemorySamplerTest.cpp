#include "IMAcousticOperationMemorySampler.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticOperationMemorySamplerSemanticTest,
	"IceMoon.AcousticField.OperationMemorySampler.Semantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticOperationMemorySamplerSemanticTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	AddInfo(TEXT("Synthetic semantic cases use injected intervals and byte counters; they are not runtime measurements."));

	using FSampler = FIMAcousticOperationMemorySampler;

	const auto InitializeSyntheticSampler = [](FSampler& Sampler)
	{
		Sampler.bStartAttempted = true;
		Sampler.bConfigurationValid = true;
		Sampler.bWorkerStarted = true;
		Sampler.ProcessId = 1234;
		Sampler.ProcessCreationTimeFileTime = 987654321;
		Sampler.bProcessIdentityCaptured = true;
	};

	const auto AddWindow = [](FSampler& Sampler, uint64 Id, double BeginSeconds,
		double EndSeconds, bool bCloseWindow)
	{
		FSampler::FIMOperationState Operation;
		Operation.Id = Id;
		Operation.Name = TEXT("synthetic-window");
		Operation.BeginSeconds = BeginSeconds;
		Operation.bBeginClockValid = true;
		if (bCloseWindow)
		{
			Operation.EndSeconds = EndSeconds;
			Operation.bHasEnd = true;
			Operation.bEndClockValid = true;
		}
		Sampler.Operations.Add(Id, MoveTemp(Operation));
		Sampler.NextOperationId = FMath::Max(Sampler.NextOperationId, Id);
	};

	const auto PublishTwoSamples = [](FSampler& Sampler)
	{
		Sampler.PublishSampleAttempt(10.010, 10.011, 100, 200, 150, 300, true);
		Sampler.PublishSampleAttempt(10.030, 10.031, 140, 280, 180, 380, true);
	};

	const auto CheckNoValidMaxima = [this](const FString& CaseName, FSampler& Sampler)
	{
		const FIMAcousticOperationMemorySamplerReport Report = Sampler.StopAndCollect();
		TestEqual(CaseName + TEXT(" produces one result"), Report.Operations.Num(), 1);
		if (Report.Operations.Num() == 1)
		{
			const FIMAcousticOperationMemoryWindowResult& Result = Report.Operations[0];
			TestFalse(CaseName + TEXT(" cannot mark sampled maxima valid"), Result.bMaximaValid);
			TestFalse(CaseName + TEXT(" has no working-set maximum"),
				Result.SampledMaxWorkingSetBytes.IsSet());
			TestFalse(CaseName + TEXT(" has no commit maximum"),
				Result.SampledMaxCommitBytes.IsSet());
		}
	};

	// A valid sample pair must be contained by both operation windows. The
	// exact maxima remain sampled maxima, and edge-inclusive gap values are kept.
	FSampler TwoWindows(0.025);
	InitializeSyntheticSampler(TwoWindows);
	AddWindow(TwoWindows, 1, 10.0, 10.05, true);
	AddWindow(TwoWindows, 2, 10.005, 10.045, true);
	PublishTwoSamples(TwoWindows);
	const FIMAcousticOperationMemorySamplerReport TwoWindowReport = TwoWindows.StopAndCollect();
	TestEqual(TEXT("Two valid windows are returned"), TwoWindowReport.Operations.Num(), 2);
	if (TwoWindowReport.Operations.Num() == 2)
	{
		for (const FIMAcousticOperationMemoryWindowResult& Result : TwoWindowReport.Operations)
		{
			TestTrue(TEXT("Fully contained two-sample window is valid"), Result.bMaximaValid);
			TestTrue(TEXT("Working-set sampled maximum is present"),
				Result.SampledMaxWorkingSetBytes.IsSet());
			TestTrue(TEXT("Commit sampled maximum is present"), Result.SampledMaxCommitBytes.IsSet());
			if (Result.SampledMaxWorkingSetBytes.IsSet())
			{
				TestEqual(TEXT("Exact sampled working-set maximum is retained"),
					Result.SampledMaxWorkingSetBytes.GetValue(), uint64(140));
			}
			if (Result.SampledMaxCommitBytes.IsSet())
			{
				TestEqual(TEXT("Exact sampled commit maximum is retained"),
					Result.SampledMaxCommitBytes.GetValue(), uint64(280));
			}
			TestTrue(TEXT("Maximum sample gap is present"), Result.MaxSampleGapSeconds.IsSet());
			if (Result.MaxSampleGapSeconds.IsSet())
			{
				TestTrue(TEXT("Maximum sample gap includes the 20 ms internal gap"),
					FMath::IsNearlyEqual(Result.MaxSampleGapSeconds.GetValue(), 0.020, 1.0e-9));
			}
		}
	}

	const TSharedRef<FJsonObject> Json = IM_AcousticOperationMemoryReportToJson(TwoWindowReport);
	TestEqual(TEXT("JSON method identifies sampled process counters"),
		Json->GetStringField(TEXT("method")), FString(TEXT("sampled process counters")));
	TestFalse(TEXT("JSON does not claim an exact peak"), Json->GetBoolField(TEXT("exact_peak")));
	TestEqual(TEXT("JSON keeps FILETIME as a decimal string"),
		Json->GetStringField(TEXT("process_creation_time_filetime_100ns")),
		FString(TEXT("987654321")));
	const TArray<TSharedPtr<FJsonValue>>& JsonOperations = Json->GetArrayField(TEXT("operations"));
	TestEqual(TEXT("JSON includes both operation windows"), JsonOperations.Num(), 2);
	if (JsonOperations.Num() == 2 && JsonOperations[0].IsValid())
	{
		const TSharedPtr<FJsonObject> FirstWindowJson = JsonOperations[0]->AsObject();
		TestTrue(TEXT("First operation JSON is an object"), FirstWindowJson.IsValid());
		if (FirstWindowJson.IsValid())
		{
			TestEqual(TEXT("JSON keeps operation identifiers as decimal strings"),
				FirstWindowJson->GetStringField(TEXT("id")), FString(TEXT("1")));
			const TSharedPtr<FJsonValue>* WorkingSetValue =
				FirstWindowJson->Values.Find(TEXT("sampled_max_working_set_bytes"));
			TestTrue(TEXT("JSON emits byte maxima as numbers"), WorkingSetValue
				&& WorkingSetValue->IsValid() && (*WorkingSetValue)->Type == EJson::Number);
		}
	}

	FSampler OpenWindow(0.025);
	InitializeSyntheticSampler(OpenWindow);
	AddWindow(OpenWindow, 1, 10.0, 10.05, false);
	PublishTwoSamples(OpenWindow);
	CheckNoValidMaxima(TEXT("Open window"), OpenWindow);

	FSampler ZeroSampleWindow(0.025);
	InitializeSyntheticSampler(ZeroSampleWindow);
	AddWindow(ZeroSampleWindow, 1, 10.0, 10.05, true);
	CheckNoValidMaxima(TEXT("Zero-sample window"), ZeroSampleWindow);
	const FIMAcousticOperationMemorySamplerReport ZeroSampleReport = ZeroSampleWindow.StopAndCollect();
	const TSharedRef<FJsonObject> ZeroSampleJson = IM_AcousticOperationMemoryReportToJson(ZeroSampleReport);
	const TArray<TSharedPtr<FJsonValue>>& ZeroSampleOperations =
		ZeroSampleJson->GetArrayField(TEXT("operations"));
	TestEqual(TEXT("Zero-sample JSON includes its operation"), ZeroSampleOperations.Num(), 1);
	if (ZeroSampleOperations.Num() == 1 && ZeroSampleOperations[0].IsValid())
	{
		const TSharedPtr<FJsonObject> ZeroSampleWindowJson = ZeroSampleOperations[0]->AsObject();
		TestTrue(TEXT("Zero-sample JSON operation is an object"), ZeroSampleWindowJson.IsValid());
		if (ZeroSampleWindowJson.IsValid())
		{
			const TSharedPtr<FJsonValue>* NullMaximum =
				ZeroSampleWindowJson->Values.Find(TEXT("sampled_max_working_set_bytes"));
			TestTrue(TEXT("Missing optional maxima serialize as JSON null"), NullMaximum
				&& NullMaximum->IsValid() && (*NullMaximum)->Type == EJson::Null);
		}
	}

	FSampler SingleSampleWindow(0.025);
	InitializeSyntheticSampler(SingleSampleWindow);
	AddWindow(SingleSampleWindow, 1, 10.0, 10.05, true);
	SingleSampleWindow.PublishSampleAttempt(10.010, 10.011, 100, 200, 150, 300, true);
	CheckNoValidMaxima(TEXT("Single-sample window"), SingleSampleWindow);

	FSampler InvalidSampleWindow(0.025);
	InitializeSyntheticSampler(InvalidSampleWindow);
	AddWindow(InvalidSampleWindow, 1, 10.0, 10.05, true);
	InvalidSampleWindow.PublishSampleAttempt(10.012, 10.013, 0, 0, 0, 0, false);
	PublishTwoSamples(InvalidSampleWindow);
	CheckNoValidMaxima(TEXT("Invalid-sample window"), InvalidSampleWindow);

	FSampler OverflowedWindow(0.025);
	InitializeSyntheticSampler(OverflowedWindow);
	AddWindow(OverflowedWindow, 1, 10.0, 10.05, true);
	PublishTwoSamples(OverflowedWindow);
	OverflowedWindow.AcceptedSampleAttemptCount = FSampler::MaxAcceptedSampleAttempts;
	OverflowedWindow.PublishSampleAttempt(10.040, 10.041, 160, 320, 200, 420, true);
	CheckNoValidMaxima(TEXT("Overflowed window"), OverflowedWindow);

	FSampler ChangedIdentityWindow(0.025);
	InitializeSyntheticSampler(ChangedIdentityWindow);
	AddWindow(ChangedIdentityWindow, 1, 10.0, 10.05, true);
	PublishTwoSamples(ChangedIdentityWindow);
	ChangedIdentityWindow.bProcessIdentityChanged = true;
	CheckNoValidMaxima(TEXT("Identity-changed window"), ChangedIdentityWindow);

	const double InvalidIntervals[] = {
		std::numeric_limits<double>::quiet_NaN(),
		0.0009,
		1.0001,
	};
	const TCHAR* InvalidIntervalCases[] = {
		TEXT("Non-finite interval"),
		TEXT("Below-minimum interval"),
		TEXT("Above-maximum interval"),
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(InvalidIntervals); ++Index)
	{
		FSampler InvalidConfiguration(InvalidIntervals[Index]);
		TestFalse(FString(InvalidIntervalCases[Index]) + TEXT(" is rejected by Start"),
			InvalidConfiguration.Start());
		const FIMAcousticOperationMemorySamplerReport Report = InvalidConfiguration.StopAndCollect();
		TestTrue(FString(InvalidIntervalCases[Index]) + TEXT(" is reported as a configuration failure"),
			Report.bConfigurationFailed);
		TestFalse(FString(InvalidIntervalCases[Index]) + TEXT(" is not marked valid"),
			Report.bConfigurationValid);
		TestFalse(FString(InvalidIntervalCases[Index]) + TEXT(" has an explicit failure reason"),
			Report.ConfigurationFailureReason.IsEmpty());
		TestFalse(FString(InvalidIntervalCases[Index]) + TEXT(" starts no worker"), Report.bWorkerStarted);
	}

	return true;
}

#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticOperationMemorySamplerThreadSmokeTest,
	"IceMoon.AcousticField.OperationMemorySampler.ThreadLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticOperationMemorySamplerThreadSmokeTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);

	FIMAcousticOperationMemorySampler Sampler(0.002);
	const bool bStarted = Sampler.Start();
	TestTrue(TEXT("Sampler worker starts"), bStarted);
	if (!bStarted)
	{
		Sampler.StopAndCollect();
		return false;
	}

	const uint64 OperationId = Sampler.BeginOperation(TEXT("thread-lifecycle-smoke"));
	TestTrue(TEXT("Operation begins with a nonzero identity"), OperationId != 0);
	FPlatformProcess::Sleep(0.064f);
	TestTrue(TEXT("Operation end is accepted"), Sampler.EndOperation(OperationId));

	const FIMAcousticOperationMemorySamplerReport Report = Sampler.StopAndCollect();
	TestTrue(TEXT("Requested interval is accepted"), Report.bConfigurationValid);
	TestTrue(TEXT("Report uses the requested 2 ms interval"),
		FMath::IsNearlyEqual(Report.RequestedSampleIntervalSeconds, 0.002, 1.0e-12));
	TestTrue(TEXT("Process identity was captured"), Report.bProcessIdentityCaptured);
	TestTrue(TEXT("Process identity matched each sample"),
		Report.bProcessIdentityVerifiedForAllSamples);
	TestFalse(TEXT("Process identity did not change"), Report.bProcessIdentityChanged);
	TestTrue(TEXT("At least two sampling attempts were accepted"), Report.AcceptedSampleAttemptCount >= 2);
	TestFalse(TEXT("Worker thread was joined and released"), Sampler.Thread.IsValid());
	TestEqual(TEXT("One operation result is returned"), Report.Operations.Num(), 1);
	if (Report.Operations.Num() == 1)
	{
		const FIMAcousticOperationMemoryWindowResult& Result = Report.Operations[0];
		TestTrue(TEXT("At least two valid samples belong to the operation"), Result.ValidSampleCount >= 2);
		TestTrue(TEXT("Window report uses the requested 2 ms interval"),
			FMath::IsNearlyEqual(Result.RequestedIntervalSeconds, 0.002, 1.0e-12));
	}
	return true;
}
#endif // PLATFORM_WINDOWS

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
