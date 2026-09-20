#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "IMAcousticSpatialization.h"
#include "IMAcousticSimulation.h"

class FRunnableThread;

struct IM_AcousticSourceSnapshot
{
    uint64 AudioComponentId = 0;
    IPLCoordinateSpace3 Source{};
};

// The GT freezes all geometry and transforms before publication. The worker
// owns the SDK instances; no UObject, component, or world pointer crosses.
struct IM_AcousticDynamicMeshSnapshot
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
struct IM_AcousticWorldSnapshot
{
    uint64 WorldGeneration = 0;
    double CapturedSeconds = 0.0;
    IPLCoordinateSpace3 Listener{};
    TArray<IM_AcousticSourceSnapshot> Sources;
    TArray<IM_AcousticDynamicMeshSnapshot> DynamicMeshes;
    bool ApertureTransitEnabled = false;
    IPLVector3 ApertureTransitCenter{};
    IPLVector3 ApertureTransitHalfExtent{};
};

class IM_AcousticSimulationWorker final : private FRunnable
{
public:
    IM_AcousticSimulationWorker();
    ~IM_AcousticSimulationWorker();
    bool Start(TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> InBridge,
        uint64 InWorldGeneration, IM_AcousticBakeData&& InBake,
        TSharedPtr<IM_AcousticReverbPool,ESPMode::ThreadSafe> InReverb={});
    bool Submit(TSharedPtr<const IM_AcousticWorldSnapshot, ESPMode::ThreadSafe> Snapshot);
    void StopAndJoin();

    // Read on GT for explicit degradation reporting, never logged in audio callbacks.
    enum class State : uint8 { Stopped, Loading, Running, LoadFailed };
    State GetState() const { return WorkerState.load(std::memory_order_acquire); }
    uint64 GetFailedUpdates() const { return FailedUpdates.load(std::memory_order_relaxed); }
    // H1 W3 negative control only (test-only): GT requests a validation flip;
    // the worker thread applies it serially on its next iteration. -1 = none.
    void RequestPathingValidationForTest(bool bOn) { ValidationRequest.store(bOn ? 1 : 0, std::memory_order_release); }
    int GetAppliedPathingValidationForTest() const { return AppliedValidation.load(std::memory_order_acquire); }

private:
    uint32 Run() override;
    void Stop() override;
    TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
    IM_AcousticSpscRing<TSharedPtr<const IM_AcousticWorldSnapshot, ESPMode::ThreadSafe>> Snapshots;
    IM_AcousticBakeData Bake;
    TSharedPtr<IM_AcousticReverbPool,ESPMode::ThreadSafe> Reverb;
    TUniquePtr<FRunnableThread> Thread;
    uint64 WorldGeneration = 0;
    std::atomic<bool> StopRequested{false};
    std::atomic<State> WorkerState{State::Stopped};
    std::atomic<uint64> FailedUpdates{0};
    std::atomic<int> ValidationRequest{-1};
    std::atomic<int> AppliedValidation{-1};
};
