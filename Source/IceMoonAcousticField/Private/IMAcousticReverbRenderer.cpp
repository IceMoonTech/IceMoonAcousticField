#include "IMAcousticReverbRenderer.h"
#include <algorithm>
#include <cmath>

namespace IMAcousticReverbRendererPrivate
{
bool ReverbFinite(float Value) { return std::isfinite(Value) != 0; }
bool ReverbFinite(const IPLVector3& V)
{
    return ReverbFinite(V.x) && ReverbFinite(V.y) && ReverbFinite(V.z);
}
bool ReverbFinite(const IPLCoordinateSpace3& Space)
{
    return ReverbFinite(Space.origin) && ReverbFinite(Space.right)
        && ReverbFinite(Space.up) && ReverbFinite(Space.ahead);
}
void ReverbClear(float* Stereo, int Frames)
{
    if (Stereo && Frames > 0) { std::fill_n(Stereo, Frames * 2, 0.0f); }
}
constexpr int ReverbOrder = 1;
constexpr int ReverbAmbiChannels = (ReverbOrder + 1) * (ReverbOrder + 1);
constexpr int ReverbStereoChannels = 2;
}

FIMAcousticReverbRenderer::~FIMAcousticReverbRenderer() { Cleanup(); }

bool FIMAcousticReverbRenderer::Initialize(IPLContext InContext, IPLHRTF InHRTF,
    int SampleRateHz, int InBlockFrames, int InIRSizeSamples)
{
    Cleanup();
    if (!InContext || !InHRTF || SampleRateHz <= 0 || InBlockFrames <= 0
        || InIRSizeSamples <= 0) { return false; }
    Context = iplContextRetain(InContext);
    HRTF = iplHRTFRetain(InHRTF);
    if (!Context || !HRTF) { Cleanup(); return false; }
    IPLAudioSettings Audio{};
    Audio.samplingRate = SampleRateHz;
    Audio.frameSize = InBlockFrames;
    IPLReflectionEffectSettings ReflectionSettings{};
    ReflectionSettings.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    ReflectionSettings.irSize = InIRSizeSamples;
    ReflectionSettings.numChannels = IMAcousticReverbRendererPrivate::ReverbAmbiChannels;
    IPLAmbisonicsDecodeEffectSettings DecodeSettings{};
    DecodeSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    DecodeSettings.speakerLayout.numSpeakers = 0;
    DecodeSettings.speakerLayout.speakers = nullptr;
    DecodeSettings.hrtf = HRTF;
    DecodeSettings.maxOrder = IMAcousticReverbRendererPrivate::ReverbOrder;
    if (iplReflectionEffectCreate(Context, &Audio, &ReflectionSettings,
            &ReflectionEffect) != IPL_STATUS_SUCCESS
        || iplAmbisonicsDecodeEffectCreate(Context, &Audio, &DecodeSettings,
            &DecodeEffect) != IPL_STATUS_SUCCESS)
    {
        Cleanup();
        return false;
    }
    BlockFrames = InBlockFrames;
    IRSizeSamples = InIRSizeSamples;
    Input.resize(static_cast<std::size_t>(BlockFrames), 0.0f);
    for (auto& Channel : Ambi) { Channel.resize(static_cast<std::size_t>(BlockFrames), 0.0f); }
    for (auto& Channel : Decoded) { Channel.resize(static_cast<std::size_t>(BlockFrames), 0.0f); }
    return true;
}

void FIMAcousticReverbRenderer::Reset()
{
    if (ReflectionEffect) { iplReflectionEffectReset(ReflectionEffect); }
    if (DecodeEffect) { iplAmbisonicsDecodeEffectReset(DecodeEffect); }
}

void FIMAcousticReverbRenderer::Cleanup()
{
    if (DecodeEffect) { iplAmbisonicsDecodeEffectRelease(&DecodeEffect); }
    if (ReflectionEffect) { iplReflectionEffectRelease(&ReflectionEffect); }
    if (HRTF) { iplHRTFRelease(&HRTF); }
    if (Context) { iplContextRelease(&Context); }
    DecodeEffect = nullptr;
    ReflectionEffect = nullptr;
    HRTF = nullptr;
    Context = nullptr;
    BlockFrames = 0;
    IRSizeSamples = 0;
    Input.clear();
    for (auto& Channel : Ambi) { Channel.clear(); }
    for (auto& Channel : Decoded) { Channel.clear(); }
}

bool FIMAcousticReverbRenderer::Render(const float* Mono, int Frames,
    const IPLReflectionEffectParams& EffectParams,
    const IPLCoordinateSpace3& Listener, float* Stereo, FIMAcousticReverbMetrics* Metrics)
{
    if(Metrics)*Metrics={};
    if (!Stereo || Frames <= 0) { Reset(); return false; }
    if (!ReflectionEffect || !DecodeEffect || BlockFrames <= 0 || IRSizeSamples <= 0
        || !Mono || Frames != BlockFrames)
    {
        IMAcousticReverbRendererPrivate::ReverbClear(Stereo, Frames);
        Reset();
        return false;
    }
    if (EffectParams.type != IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
        || EffectParams.ir == nullptr
        || EffectParams.numChannels != IMAcousticReverbRendererPrivate::ReverbAmbiChannels
        || EffectParams.irSize != IRSizeSamples)
    {
        IMAcousticReverbRendererPrivate::ReverbClear(Stereo, Frames);
        Reset();
        return false;
    }
    if (!IMAcousticReverbRendererPrivate::ReverbFinite(Listener))
    {
        IMAcousticReverbRendererPrivate::ReverbClear(Stereo, Frames);
        Reset();
        return false;
    }
    for (int I = 0; I < Frames; ++I)
    {
        if (!IMAcousticReverbRendererPrivate::ReverbFinite(Mono[I]))
        {
            IMAcousticReverbRendererPrivate::ReverbClear(Stereo, Frames);
            Reset();
            return false;
        }
        Input[static_cast<std::size_t>(I)] = Mono[I];
        if(Metrics)Metrics->InputEnergy+=double(Mono[I])*Mono[I];
    }
    for (auto& Channel : Ambi) { std::fill(Channel.begin(), Channel.end(), 0.0f); }
    for (auto& Channel : Decoded) { std::fill(Channel.begin(), Channel.end(), 0.0f); }
    float* InChannels[1] = { Input.data() };
    float* AmbiChannels[IMAcousticReverbRendererPrivate::ReverbAmbiChannels] =
        { Ambi[0].data(), Ambi[1].data(), Ambi[2].data(), Ambi[3].data() };
    float* StereoChannels[IMAcousticReverbRendererPrivate::ReverbStereoChannels] =
        { Decoded[0].data(), Decoded[1].data() };
    IPLAudioBuffer InBuffer{1, Frames, InChannels};
    IPLAudioBuffer AmbiBuffer{IMAcousticReverbRendererPrivate::ReverbAmbiChannels, Frames, AmbiChannels};
    IPLAudioBuffer StereoBuffer{IMAcousticReverbRendererPrivate::ReverbStereoChannels, Frames, StereoChannels};
    // Borrowed IR: copied handle is read only inside this apply; this object
    // never retains or releases the source IR. Root owns the IR lifetime and
    // keeps it alive until Render returns. Effect history state stays inside
    // the SDK effects and is cleared only by Reset/destruction above.
    IPLReflectionEffectParams ActiveParams = EffectParams;
    iplReflectionEffectApply(ReflectionEffect, &ActiveParams, &InBuffer, &AmbiBuffer, nullptr);
    if(Metrics)for(const auto& Channel:Ambi)for(float V:Channel)Metrics->AmbisonicsEnergy+=double(V)*V;
    IPLAmbisonicsDecodeEffectParams DecodeParams{};
    DecodeParams.order = IMAcousticReverbRendererPrivate::ReverbOrder;
    DecodeParams.hrtf = HRTF;
    DecodeParams.orientation = Listener;
    DecodeParams.binaural = IPL_TRUE;
    iplAmbisonicsDecodeEffectApply(DecodeEffect, &DecodeParams, &AmbiBuffer, &StereoBuffer);
    for (int I = 0; I < Frames; ++I)
    {
        for (int C = 0; C < IMAcousticReverbRendererPrivate::ReverbStereoChannels; ++C)
        {
            const float Sample = Decoded[static_cast<std::size_t>(C)][static_cast<std::size_t>(I)];
            if (!IMAcousticReverbRendererPrivate::ReverbFinite(Sample))
            {
                IMAcousticReverbRendererPrivate::ReverbClear(Stereo, Frames);
                Reset();
                return false;
            }
            Stereo[2 * I + C] = Sample;
        }
    }
    return true;
}
