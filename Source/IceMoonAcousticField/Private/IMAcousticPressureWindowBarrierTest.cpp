#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "IMAcousticSpatialization.h"

namespace IMAcousticPressureWindowBarrierTestPrivate
{
class FIMAcousticPressureWindowBarrierWorker final : public FRunnable
{
public:
	FIMAcousticPressureWindowBarrierWorker(
		FIMAcousticDeviceBridge& InBridge,
		FEvent& InAdmitted,
		FEvent& InRelease,
		FEvent& InFinished)
		: Bridge(InBridge), Admitted(InAdmitted), Release(InRelease), Finished(InFinished)
	{
	}

	uint32 Run() override
	{
		const bool bDidAdmit = Bridge.TryBeginPressureWindowPublication();
		bAdmitted.store(bDidAdmit, std::memory_order_release);
		Admitted.Trigger();

		if (bDidAdmit)
		{
			// Hold the admitted lease while the game thread closes the window.
			// The write then completes under that old lease before End releases it.
			if (Release.Wait(30000))
			{
				FIMAcousticWorkerProbe Entry;
				Bridge.PressureWorkers.Publish(Entry);
				bPublished.store(true, std::memory_order_release);
			}
			Bridge.EndPressureWindowPublication();
		}

		Finished.Trigger();
		return 0;
	}

	std::atomic<bool> bAdmitted{false};
	std::atomic<bool> bPublished{false};

private:
	FIMAcousticDeviceBridge& Bridge;
	FEvent& Admitted;
	FEvent& Release;
	FEvent& Finished;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticPressureWindowBarrierTest,
	"IceMoon.AcousticField.PhysicalMaterial.PressureWindowPublicationBarrier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticPressureWindowBarrierTest::RunTest(const FString&)
{
	// The bridge contains large bounded probe arrays; keep it off the test stack.
	TUniquePtr<FIMAcousticDeviceBridge> Bridge = MakeUnique<FIMAcousticDeviceBridge>();
	TestTrue(TEXT("The automation test runs on the game thread required by probe allocation."), IsInGameThread());

	TestFalse(TEXT("An unallocated pressure window cannot open."), Bridge->OpenPressureWindowProbes());
	const bool bPrepared = Bridge->PreparePressureWindowProbes();
	TestTrue(TEXT("The first game-thread preparation allocates both pressure logs."), bPrepared);
	TestFalse(TEXT("A device pressure window can only be prepared once."), Bridge->PreparePressureWindowProbes());

	const bool bOpened = bPrepared && Bridge->OpenPressureWindowProbes();
	TestTrue(TEXT("The allocated pressure window opens once."), bOpened);
	if (bOpened)
	{
		TestFalse(TEXT("An open pressure window cannot be opened a second time."), Bridge->OpenPressureWindowProbes());

		FIMAcousticSnapshotProbe Snapshot;
		FIMAcousticWorkerProbe Worker;
		Bridge->TraceSnapshot(Snapshot);
		Bridge->TraceWorker(Worker);
		TestTrue(TEXT("TraceSnapshot publishes one complete real pressure snapshot."),
			Bridge->PressureSnapshots.Pushes.load(std::memory_order_acquire) == 1
				&& Bridge->PressureSnapshots.Done[0].load(std::memory_order_acquire) == 1
				&& Bridge->PressureSnapshots.Overflows.load(std::memory_order_acquire) == 0);
		TestTrue(TEXT("TraceWorker publishes one complete real pressure worker row."),
			Bridge->PressureWorkers.Pushes.load(std::memory_order_acquire) == 1
				&& Bridge->PressureWorkers.Done[0].load(std::memory_order_acquire) == 1
				&& Bridge->PressureWorkers.Overflows.load(std::memory_order_acquire) == 0);
	}

	FEvent* AdmittedEvent = FPlatformProcess::GetSynchEventFromPool(false);
	FEvent* ReleaseEvent = FPlatformProcess::GetSynchEventFromPool(false);
	FEvent* FinishedEvent = FPlatformProcess::GetSynchEventFromPool(false);
	const bool bEventsReady = AdmittedEvent && ReleaseEvent && FinishedEvent;
	TestTrue(TEXT("Three pooled events are available for deterministic worker ordering."), bEventsReady);

	FRunnableThread* Thread = nullptr;
	IMAcousticPressureWindowBarrierTestPrivate::FIMAcousticPressureWindowBarrierWorker* Worker = nullptr;
	const bool bMultithreadingSupported = FPlatformProcess::SupportsMultithreading();
	TestTrue(TEXT("This barrier test requires a real worker thread."), bMultithreadingSupported);
	if (bOpened && bEventsReady && bMultithreadingSupported)
	{
		Worker = new IMAcousticPressureWindowBarrierTestPrivate::FIMAcousticPressureWindowBarrierWorker(
			*Bridge, *AdmittedEvent, *ReleaseEvent, *FinishedEvent);
		Thread = FRunnableThread::Create(Worker, TEXT("IMPressureWindowBarrierTest"), 0, TPri_Normal);
		TestTrue(TEXT("UE creates the barrier test worker thread."), Thread != nullptr);
		if (Thread)
		{
			TestTrue(TEXT("The publisher runs on an actual thread."),
				Thread->GetThreadType() == FRunnableThread::ThreadType::Real);

			const bool bAdmissionSignalled = AdmittedEvent->Wait(10000);
			TestTrue(TEXT("The worker reports its admission attempt."), bAdmissionSignalled);
			TestTrue(TEXT("The worker was admitted while the pressure window was open."),
				Worker->bAdmitted.load(std::memory_order_acquire));

			Bridge->ClosePressureWindowProbes();
			TestFalse(TEXT("Close cannot report quiescence while an admitted worker is held before Publish."),
				Bridge->PressureWindowPublicationsQuiescent());
			TestFalse(TEXT("A new publisher cannot enter after the game thread closes the window."),
				Bridge->TryBeginPressureWindowPublication());
		}
	}

	// Every path signals the worker before waiting for its real thread to finish.
	Bridge->ClosePressureWindowProbes();
	if (ReleaseEvent) ReleaseEvent->Trigger();
	if (Thread)
	{
		if (FinishedEvent)
			TestTrue(TEXT("The admitted publisher exits after the release event."), FinishedEvent->Wait(10000));
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}

	if (Worker)
	{
		TestTrue(TEXT("The already-admitted worker completed its publication after Close."),
			Worker->bPublished.load(std::memory_order_acquire));
		delete Worker;
		Worker = nullptr;
	}

	TestTrue(TEXT("Both real trace rows and the admitted in-flight row have complete Done markers."),
		bPrepared
			&& Bridge->PressureSnapshots.Pushes.load(std::memory_order_acquire) == 1
			&& Bridge->PressureSnapshots.Done[0].load(std::memory_order_acquire) == 1
			&& Bridge->PressureWorkers.Pushes.load(std::memory_order_acquire) == 2
			&& Bridge->PressureWorkers.Done[0].load(std::memory_order_acquire) == 1
			&& Bridge->PressureWorkers.Done[1].load(std::memory_order_acquire) == 2);
	TestTrue(TEXT("After all admitted writes leave, the closed window is quiescent."),
		Bridge->PressureWindowPublicationsQuiescent());
	TestFalse(TEXT("A closed pressure window cannot be reopened."), Bridge->OpenPressureWindowProbes());

	if (bPrepared && bOpened)
	{
		FIMAcousticSnapshotProbe SnapshotAfterClose;
		FIMAcousticWorkerProbe WorkerAfterClose;
		Bridge->TraceSnapshot(SnapshotAfterClose);
		Bridge->TraceWorker(WorkerAfterClose);
		TestTrue(TEXT("Normal trace calls after Close cannot add optional pressure rows."),
			Bridge->PressureSnapshots.Pushes.load(std::memory_order_acquire) == 1
				&& Bridge->PressureWorkers.Pushes.load(std::memory_order_acquire) == 2);
	}

	if (AdmittedEvent)
	{
		AdmittedEvent->Reset();
		FPlatformProcess::ReturnSynchEventToPool(AdmittedEvent);
	}
	if (ReleaseEvent)
	{
		ReleaseEvent->Reset();
		FPlatformProcess::ReturnSynchEventToPool(ReleaseEvent);
	}
	if (FinishedEvent)
	{
		FinishedEvent->Reset();
		FPlatformProcess::ReturnSynchEventToPool(FinishedEvent);
	}
	const bool bPassed = !HasAnyErrors();
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] %s PressureWindowPublicationBarrier"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s PressureWindowPublicationBarrier"), bPassed ? TEXT("PASS") : TEXT("FAIL"));
	return bPassed;
}

#endif
