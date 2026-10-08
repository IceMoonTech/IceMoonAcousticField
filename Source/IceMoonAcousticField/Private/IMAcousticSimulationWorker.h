#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticSimulation.h"

class FRunnableThread;

struct FIMAcousticSourceSnapshot
{
    uint64 AudioComponentId = 0;
    IPLCoordinateSpace3 Source{};
};

// The GT freezes all geometry and transforms before publication. The worker
// owns the SDK instances; no UObject, component, or world pointer crosses.
struct FIMAcousticDynamicMeshSnapshot
{
    uint64 Key = 0;
    uint64 GeometryHash = 0;
    TArray<IPLVector3> Vertices;
    TArray<IPLTriangle> Triangles;
    TArray<int32> MaterialIndices;
    TArray<IPLMaterial> Materials;
    IPLMatrix4x4 Transform{};
};

// The GT freezes transforms after checking UObject/world/asset validity. Only
// value data crosses into the worker; no raw component or world pointer escapes.
struct FIMAcousticWorldSnapshot
{
    uint64 WorldGeneration = 0;
    double CapturedSeconds = 0.0;
    IPLCoordinateSpace3 Listener{};
    TArray<FIMAcousticSourceSnapshot> Sources;
    TArray<FIMAcousticDynamicMeshSnapshot> DynamicMeshes;
    bool ApertureTransitEnabled = false;
    IPLVector3 ApertureTransitCenter{};
    IPLVector3 ApertureTransitHalfExtent{};
};

class FIMAcousticSimulationWorker final : private FRunnable
{
public:
    FIMAcousticSimulationWorker();
    ~FIMAcousticSimulationWorker();
    bool Start(TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> InBridge,
        uint64 InWorldGeneration, FIMAcousticBakeData&& InBake,
        TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> InReverb={});
    bool Submit(TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe> Snapshot);
    void StopAndJoin();

    // Read on GT for explicit degradation reporting, never logged in audio callbacks.
    enum class EIMWorkerState : uint8 { Stopped, Loading, Running, LoadFailed };
    EIMWorkerState GetState() const { return WorkerState.load(std::memory_order_acquire); }
    uint64 GetFailedUpdates() const { return FailedUpdates.load(std::memory_order_relaxed); }
    // H1 W3 negative control only (test-only): GT requests a validation flip;
    // the worker thread applies it serially on its next iteration. -1 = none.
    void RequestPathingValidationForTest(bool bOn) { ValidationRequest.store(bOn ? 1 : 0, std::memory_order_release); }
    int GetAppliedPathingValidationForTest() const { return AppliedValidation.load(std::memory_order_acquire); }

private:
    uint32 Run() override;
    void Stop() override;
    TSharedPtr<FIMAcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
    FIMAcousticSpscRing<TSharedPtr<const FIMAcousticWorldSnapshot, ESPMode::ThreadSafe>> Snapshots;
    FIMAcousticBakeData Bake;
    TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> Reverb;
    TUniquePtr<FRunnableThread> Thread;
    uint64 WorldGeneration = 0;
    std::atomic<bool> StopRequested{false};
    std::atomic<EIMWorkerState> WorkerState{EIMWorkerState::Stopped};
    std::atomic<uint64> FailedUpdates{0};
    std::atomic<int> ValidationRequest{-1};
    std::atomic<int> AppliedValidation{-1};
};
