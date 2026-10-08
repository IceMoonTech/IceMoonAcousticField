#include "IMAcousticAudioRenderer.h"
#include <algorithm>
#include <cmath>

namespace IMAcousticAudioRendererPrivate
{
bool Finite(float Value) { return std::isfinite(Value); }
bool Finite(const IPLVector3& V) { return Finite(V.x) && Finite(V.y) && Finite(V.z); }
bool ValidFrame(const FIMAcousticAudioFrame& Frame)
{
    if (Frame.Generation == 0) { return false; }
    if (Frame.DirectValid)
    {
        const auto& D = Frame.Direct;
        if (!Finite(Frame.ListenerLocalDirection) || !Finite(D.distanceAttenuation)
            || !Finite(D.occlusion) || !Finite(D.directivity)) { return false; }
        for (int B = 0; B < IPL_NUM_BANDS; ++B)
        {
            if (!Finite(D.airAbsorption[B]) || !Finite(D.transmission[B])) { return false; }
        }
    }
    if (Frame.PathValid)
    {
        if (!Finite(Frame.Listener.origin) || !Finite(Frame.Listener.right)
            || !Finite(Frame.Listener.up) || !Finite(Frame.Listener.ahead)) { return false; }
        for (float V : Frame.PathEQ) { if (!Finite(V) || V < 0.0f) { return false; } }
        for (float V : Frame.PathSH) { if (!Finite(V)) { return false; } }
    }
    return true;
}
}

FIMAcousticAudioRenderer::~FIMAcousticAudioRenderer() { Shutdown(); }

bool FIMAcousticAudioRenderer::Initialize(IPLContext InContext, IPLHRTF InHRTF,
    int SampleRate, int InBlockFrames)
{
    Shutdown();
    if (!InContext || !InHRTF || SampleRate <= 0 || InBlockFrames <= 0) { return false; }
    Context = iplContextRetain(InContext);
    HRTF = iplHRTFRetain(InHRTF);
    IPLAudioSettings Audio{SampleRate, InBlockFrames};
    IPLDirectEffectSettings DirectSettings{1};
    IPLBinauralEffectSettings BinauralSettings{HRTF};
    IPLPathEffectSettings PathSettings{};
    PathSettings.maxOrder = FIMAcousticAudioFrame::Order;
    PathSettings.spatialize = IPL_TRUE;
    PathSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    PathSettings.hrtf = HRTF;
    if (iplDirectEffectCreate(Context, &Audio, &DirectSettings, &DirectEffect) != IPL_STATUS_SUCCESS
        || iplBinauralEffectCreate(Context, &Audio, &BinauralSettings, &BinauralEffect) != IPL_STATUS_SUCCESS
        || iplPathEffectCreate(Context, &Audio, &PathSettings, &PathEffect) != IPL_STATUS_SUCCESS)
    {
        Shutdown();
        return false;
    }
    BlockFrames = InBlockFrames;
    Input.resize(BlockFrames);
    DirectMono.resize(BlockFrames);
    for (int C = 0; C < 2; ++C)
    {
        DirectOutput[C].resize(BlockFrames);
        PathOutput[C].resize(BlockFrames);
    }
    return true;
}

void FIMAcousticAudioRenderer::Reset()
{
    if (DirectEffect) { iplDirectEffectReset(DirectEffect); }
    if (BinauralEffect) { iplBinauralEffectReset(BinauralEffect); }
    if (PathEffect) { iplPathEffectReset(PathEffect); }
    WasDirectValid = WasPathValid = false;
    ActiveGeneration = 0;
}

void FIMAcousticAudioRenderer::Shutdown()
{
    if (PathEffect) { iplPathEffectRelease(&PathEffect); }
    if (BinauralEffect) { iplBinauralEffectRelease(&BinauralEffect); }
    if (DirectEffect) { iplDirectEffectRelease(&DirectEffect); }
    if (HRTF) { iplHRTFRelease(&HRTF); }
    if (Context) { iplContextRelease(&Context); }
    BlockFrames = 0;
    ActiveGeneration = 0;
    WasDirectValid = WasPathValid = false;
}

bool FIMAcousticAudioRenderer::Render(const float* DryMono, int Frames,
    const FIMAcousticAudioFrame& Frame, float* Stereo, float* DirectStereo, float* PathStereo,
    FIMAcousticAudioMetrics* Metrics, std::uint32_t AudibleRoutes)
{
    if (Metrics) { *Metrics = {}; }
    if (!Stereo || Frames <= 0) { return false; }
    std::fill_n(Stereo, Frames * 2, 0.0f);
    if (DirectStereo) { std::fill_n(DirectStereo, Frames * 2, 0.0f); }
    if (PathStereo) { std::fill_n(PathStereo, Frames * 2, 0.0f); }
    if (!DryMono || Frames != BlockFrames || !DirectEffect || !BinauralEffect || !PathEffect
        || !IMAcousticAudioRendererPrivate::ValidFrame(Frame))
    {
        if(Metrics)Metrics->Failure=EIMAcousticRenderFailure::InvalidFrame;
        Reset();
        return false;
    }
    if (Frame.Generation != ActiveGeneration)
    {
        // Voice reuse/world replacement must not play the prior voice's filter tail.
        Reset();
        ActiveGeneration = Frame.Generation;
    }
    for (int I = 0; I < Frames; ++I)
    {
        if (!IMAcousticAudioRendererPrivate::Finite(DryMono[I])) { if(Metrics)Metrics->Failure=EIMAcousticRenderFailure::NonfiniteInput;Reset(); return false; }
        Input[I] = DryMono[I];
    }
    for (int C = 0; C < 2; ++C)
    {
        std::fill(DirectOutput[C].begin(), DirectOutput[C].end(), 0.0f);
        std::fill(PathOutput[C].begin(), PathOutput[C].end(), 0.0f);
    }
    float* DryChannels[]{Input.data()};
    float* DirectChannels[]{DirectMono.data()};
    float* DirectStereoChannels[]{DirectOutput[0].data(), DirectOutput[1].data()};
    float* PathStereoChannels[]{PathOutput[0].data(), PathOutput[1].data()};
    IPLAudioBuffer Dry{1, Frames, DryChannels};
    IPLAudioBuffer Direct{1, Frames, DirectChannels};
    IPLAudioBuffer DirectOut{2, Frames, DirectStereoChannels};
    IPLAudioBuffer PathOut{2, Frames, PathStereoChannels};
    if (Frame.DirectValid)
    {
        auto Params = Frame.Direct;
        iplDirectEffectApply(DirectEffect, &Params, &Dry, &Direct);
        IPLBinauralEffectParams Binaural{};
        Binaural.direction = Frame.ListenerLocalDirection;
        Binaural.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        Binaural.spatialBlend = 1.0f;
        Binaural.hrtf = HRTF;
        iplBinauralEffectApply(BinauralEffect, &Binaural, &Direct, &DirectOut);
    }
    else if (WasDirectValid)
    {
        iplDirectEffectReset(DirectEffect);
        iplBinauralEffectReset(BinauralEffect);
    }
    if (Frame.PathValid)
    {
        IPLPathEffectParams Path{};
        std::copy(Frame.PathEQ.begin(), Frame.PathEQ.end(), Path.eqCoeffs);
        // SDK apply consumes the coefficients during this call; no simulator-owned
        // pointer escapes a worker update. The local copy also honors its mutable ABI.
        auto Coefficients = Frame.PathSH;
        Path.shCoeffs = Coefficients.data();
        Path.order = FIMAcousticAudioFrame::Order;
        Path.binaural = IPL_TRUE;
        Path.hrtf = HRTF;
        Path.listener = Frame.Listener;
        Path.normalizeEQ = IPL_FALSE;
        iplPathEffectApply(PathEffect, &Path, &Dry, &PathOut);
    }
    else if (WasPathValid) { iplPathEffectReset(PathEffect); }
    WasDirectValid = Frame.DirectValid;
    WasPathValid = Frame.PathValid;
    for (int I = 0; I < Frames; ++I)
    {
        for (int C = 0; C < 2; ++C)
        {
            const int OutIndex = 2 * I + C;
            // Audition mutes only the output; both valid DSP histories continue
            // advancing so re-enabling a route does not restart its filters.
            const float D = (AudibleRoutes & 1) ? DirectOutput[C][I] : 0.f;
            const float P = (AudibleRoutes & 2) ? PathOutput[C][I] : 0.f;
            const float Sum = D + P;
            if (!IMAcousticAudioRendererPrivate::Finite(DirectOutput[C][I]) || !IMAcousticAudioRendererPrivate::Finite(PathOutput[C][I]) || !IMAcousticAudioRendererPrivate::Finite(Sum))
            {
                if(Metrics)Metrics->Failure=!IMAcousticAudioRendererPrivate::Finite(DirectOutput[C][I])?EIMAcousticRenderFailure::NonfiniteDirect:EIMAcousticRenderFailure::NonfinitePath;
                std::fill_n(Stereo, Frames * 2, 0.0f);
                if (DirectStereo) { std::fill_n(DirectStereo, Frames * 2, 0.0f); }
                if (PathStereo) { std::fill_n(PathStereo, Frames * 2, 0.0f); }
                Reset();
                return false;
            }
            Stereo[OutIndex] = Sum;
            if (Metrics) { Metrics->DirectEnergy += double(D)*D; Metrics->PathEnergy += double(P)*P; }
            if (DirectStereo) { DirectStereo[OutIndex] = D; }
            if (PathStereo) { PathStereo[OutIndex] = P; }
        }
    }
    return true;
}
