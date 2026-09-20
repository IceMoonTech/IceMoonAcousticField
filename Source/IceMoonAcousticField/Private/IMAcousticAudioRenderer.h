#pragma once

// SDK-only audio kernel. The UE adapter and independent executable use the same
// implementation; no UObject, simulator, allocation or lock is needed by Render.
#include <phonon.h>
#include <array>
#include <cstdint>
#include <vector>

struct IM_AcousticAudioFrame
{
    static constexpr int Order = 1;
    static constexpr int Coefficients = (Order + 1) * (Order + 1);
    std::uint64_t Generation = 0;
    std::uint64_t Sequence = 0;
    bool DirectValid = false;
    bool PathValid = false;
    IPLDirectEffectParams Direct{};
    IPLVector3 ListenerLocalDirection{0.0f, 0.0f, -1.0f};
    IPLCoordinateSpace3 Listener{};
    std::array<float, IPL_NUM_BANDS> PathEQ{};
    std::array<float, Coefficients> PathSH{};
};

enum class IM_AcousticRenderFailure : unsigned char { None, InvalidFrame, NonfiniteInput, NonfiniteDirect, NonfinitePath, Count };
struct IM_AcousticAudioMetrics
{
    double DirectEnergy = 0.0;
    double PathEnergy = 0.0;
    IM_AcousticRenderFailure Failure=IM_AcousticRenderFailure::None;
};

class IM_AcousticAudioRenderer final
{
public:
    IM_AcousticAudioRenderer() = default;
    ~IM_AcousticAudioRenderer();
    IM_AcousticAudioRenderer(const IM_AcousticAudioRenderer&) = delete;
    IM_AcousticAudioRenderer& operator=(const IM_AcousticAudioRenderer&) = delete;

    // Lifecycle operations are outside the steady audio callback. Context/HRTF
    // references are retained, so the parent device cannot invalidate live effects.
    // HRTF must be exclusive to this concurrent render owner: SDK 4.8.1 bilinear
    // interpolation mutates HRTF scratch, despite receiving a retained handle.
    bool Initialize(IPLContext Context, IPLHRTF HRTF, int SampleRate, int BlockFrames);
    void Reset();
    void Shutdown();

    // DryMono is immutable. Direct occlusion never contaminates the path input.
    // Every buffer length must match BlockFrames (stereo outputs: 2*BlockFrames).
    // Optional stems are for capturing the production signal, not a separate oracle.
    bool Render(const float* DryMono, int Frames, const IM_AcousticAudioFrame& Frame,
        float* Stereo, float* DirectStereo = nullptr, float* PathStereo = nullptr,
        IM_AcousticAudioMetrics* Metrics = nullptr, std::uint32_t AudibleRoutes = 3);

private:
    IPLContext Context = nullptr;
    IPLHRTF HRTF = nullptr;
    IPLDirectEffect DirectEffect = nullptr;
    IPLBinauralEffect BinauralEffect = nullptr;
    IPLPathEffect PathEffect = nullptr;
    int BlockFrames = 0;
    bool WasDirectValid = false;
    bool WasPathValid = false;
    std::uint64_t ActiveGeneration = 0;
    std::vector<float> Input;
    std::vector<float> DirectMono;
    std::array<std::vector<float>, 2> DirectOutput;
    std::array<std::vector<float>, 2> PathOutput;
};
