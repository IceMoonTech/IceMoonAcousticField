#pragma once

// Standard-C++ Steam Audio SDK 4.8.1 convolution reverb renderer.
// No UE, no UObject, no threads, no file I/O. The owner calls every method
// serially; no lock is created here and no SDK pointer leaves this object.
// Units: SampleRateHz in Hz; BlockFrames in samples; IRSizeSamples in samples
// per channel. Fixed order 1, so the Ambisonics field is always 4 channels.
//
// Chain: IPLReflectionEffect (CONVOLUTION) renders mono dry into an order-1
// Ambisonics buffer, then a single IPLAmbisonicsDecodeEffect in binaural mode
// renders that field to stereo. Decode-in-binaural is used instead of
// IPLAmbisonicsBinauralEffect because only the decode params carry a listener
// orientation (phonon.h: IPLAmbisonicsBinauralEffectParams holds hrtf+order
// only, while IPLAmbisonicsDecodeEffectParams holds order+hrtf+orientation+
// binaural), so Listener is consumed without any custom rotation math.
//
// IR ownership: the caller (root) guarantees EffectParams.ir stays alive for
// the duration of Render. Render borrows that IR handle only inside the
// iplReflectionEffectApply call and never retains or releases it. History
// state (overlap-save FIR buffers, decode state) is owned by the SDK effect
// instances and persists until Reset or destruction, which is why a failed
// Render resets the effects before returning.
#include <phonon.h>

#include <array>
#include <vector>

struct IM_AcousticReverbMetrics { double InputEnergy=0,AmbisonicsEnergy=0; };
class IM_AcousticReverbRenderer final
{
public:
    IM_AcousticReverbRenderer() = default;
    ~IM_AcousticReverbRenderer();
    IM_AcousticReverbRenderer(const IM_AcousticReverbRenderer&) = delete;
    IM_AcousticReverbRenderer& operator=(const IM_AcousticReverbRenderer&) = delete;

    // Retains Context/HRTF, creates the convolution reflection effect
    // (type CONVOLUTION, numChannels 4, irSize IRSizeSamples) and the order-1
    // binaural decode effect, and preallocates all Render buffers. Returns
    // false without partial state when any handle/rate/size is invalid or any
    // SDK create fails. Lifecycle only; never called from steady audio.
    bool Initialize(IPLContext Context, IPLHRTF HRTF, int SampleRateHz,
        int BlockFrames, int IRSizeSamples);

    // Clears SDK history state. Safe on an uninitialized renderer.
    void Reset();
    // Lifecycle teardown for MetaSound operator-cache reuse across worlds.
    void Shutdown() { Cleanup(); }

    // Renders one block: Mono[Frames] dry into interleaved Stereo[2*Frames].
    // EffectParams must be type CONVOLUTION with ir != null, numChannels 4,
    // and irSize equal to the Initialize IRSizeSamples; the IR handle is
    // borrowed only for this call. Listener provides the decode orientation.
    // Frames must equal the Initialize BlockFrames. Rejects null pointers and
    // non-finite dry samples; on any failure Stereo is cleared (when the
    // pointer and frame count allow it), effects are reset, and false returns.
    // No allocation, lock, file, or UObject access inside.
    bool Render(const float* Mono, int Frames,
        const IPLReflectionEffectParams& EffectParams,
        const IPLCoordinateSpace3& Listener, float* Stereo, IM_AcousticReverbMetrics* Metrics=nullptr);

private:
    void Cleanup();

    IPLContext Context = nullptr;
    IPLHRTF HRTF = nullptr;
    IPLReflectionEffect ReflectionEffect = nullptr;
    IPLAmbisonicsDecodeEffect DecodeEffect = nullptr;
    int BlockFrames = 0;
    int IRSizeSamples = 0;
    std::vector<float> Input;
    std::array<std::vector<float>, 4> Ambi;
    std::array<std::vector<float>, 2> Decoded;
};
