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
	// Without a path payload, only the direct checks above apply.
	if (!Frame.PathValid) { return true; }
	if (!Finite(Frame.Listener.origin) || !Finite(Frame.Listener.right)
		|| !Finite(Frame.Listener.up) || !Finite(Frame.Listener.ahead)) { return false; }
	for (float V : Frame.PathEQ) { if (!Finite(V) || V < 0.0f) { return false; } }
	for (float V : Frame.PathSH) { if (!Finite(V)) { return false; } }
	return true;
}


void ClearStereoOutputs(float* Stereo, int Frames, float* DirectStereo, float* PathStereo)
{
	std::fill_n(Stereo, Frames * 2, 0.0f);
	if (DirectStereo) { std::fill_n(DirectStereo, Frames * 2, 0.0f); }
	if (PathStereo) { std::fill_n(PathStereo, Frames * 2, 0.0f); }
}
}

IPLVector3 FIMAcousticDirectShaping::ScaleAzimuth(const IPLVector3& Direction, float Scale, float LateralRadians, float LimitRadians)
{
	using namespace IMAcousticAudioRendererPrivate;
	const float Length = std::sqrt(Direction.x * Direction.x + Direction.y * Direction.y + Direction.z * Direction.z);
	const float Ahead = -Direction.z;
	if (!Finite(Direction) || !Finite(Scale) || !Finite(LateralRadians) || !Finite(LimitRadians) || Length < 1.e-6f || Ahead <= 0.0f) { return Direction; }
	// Scale 1 (strength 0) or no lateral offset adds nothing: return the solved direction itself so the physical-range azimuth is neither
	// recomputed through trigonometry nor touched by the limit.
	if ((Scale - 1.0f) * LateralRadians == 0.0f) { return Direction; }
	const float Elevation = std::asin(std::fmax(-1.0f, std::fmin(1.0f, Direction.y / Length)));
	const float Azimuth = std::atan2(Direction.x, Ahead);
	// The limit never pulls an already wider direction back toward the front.
	const float Reach = std::fmax(LimitRadians, std::fabs(Azimuth));
	const float NewAzimuth = std::fmax(-Reach, std::fmin(Reach, Azimuth + (Scale - 1.0f) * LateralRadians));
	const float Horizontal = std::cos(Elevation);
	return IPLVector3{Horizontal * std::sin(NewAzimuth), std::sin(Elevation), -Horizontal * std::cos(NewAzimuth)};
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
	FIMAcousticAudioMetrics* Metrics, std::uint32_t AudibleRoutes, const FIMAcousticDirectShaping* Shaping)
{
	if (Metrics) { *Metrics = {}; }
	if (!Stereo || Frames <= 0) { return false; }
	IMAcousticAudioRendererPrivate::ClearStereoOutputs(Stereo, Frames, DirectStereo, PathStereo);
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
	for (int FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
	{
		if (!IMAcousticAudioRendererPrivate::Finite(DryMono[FrameIndex])) { if(Metrics)Metrics->Failure=EIMAcousticRenderFailure::NonfiniteInput;Reset(); return false; }
		Input[FrameIndex] = DryMono[FrameIndex];
	}
	for (int ChannelIndex = 0; ChannelIndex < 2; ++ChannelIndex)
	{
		std::fill(DirectOutput[ChannelIndex].begin(), DirectOutput[ChannelIndex].end(), 0.0f);
		std::fill(PathOutput[ChannelIndex].begin(), PathOutput[ChannelIndex].end(), 0.0f);
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
		IPLVector3 BinauralDirection = Frame.ListenerLocalDirection;
		if (Shaping)
		{
			if (Shaping->bUnitDistanceAttenuation) { Params.distanceAttenuation = 1.0f; }
			BinauralDirection = FIMAcousticDirectShaping::ScaleAzimuth(BinauralDirection, Shaping->AzimuthScale, Shaping->LateralAzimuthRadians, Shaping->AzimuthLimitRadians);
		}
		iplDirectEffectApply(DirectEffect, &Params, &Dry, &Direct);
		IPLBinauralEffectParams Binaural{};
		Binaural.direction = BinauralDirection;
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
	for (int FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
	{
		for (int ChannelIndex = 0; ChannelIndex < 2; ++ChannelIndex)
		{
			const int InterleavedSampleIndex = 2 * FrameIndex + ChannelIndex;
			// Audition mutes only the output; both valid DSP histories continue
			// advancing so re-enabling a route does not restart its filters.
			const float DirectSample = (AudibleRoutes & 1) ? DirectOutput[ChannelIndex][FrameIndex] : 0.f;
			const float PathSample = (AudibleRoutes & 2) ? PathOutput[ChannelIndex][FrameIndex] : 0.f;
			const float MixedSample = DirectSample + PathSample;
			if (!IMAcousticAudioRendererPrivate::Finite(DirectOutput[ChannelIndex][FrameIndex]) || !IMAcousticAudioRendererPrivate::Finite(PathOutput[ChannelIndex][FrameIndex]) || !IMAcousticAudioRendererPrivate::Finite(MixedSample))
			{
				if(Metrics)Metrics->Failure=!IMAcousticAudioRendererPrivate::Finite(DirectOutput[ChannelIndex][FrameIndex])?EIMAcousticRenderFailure::NonfiniteDirect:EIMAcousticRenderFailure::NonfinitePath;
				IMAcousticAudioRendererPrivate::ClearStereoOutputs(Stereo, Frames, DirectStereo, PathStereo);
				Reset();
				return false;
			}
			Stereo[InterleavedSampleIndex] = MixedSample;
			if (Metrics) { Metrics->DirectEnergy += double(DirectSample)*DirectSample; Metrics->PathEnergy += double(PathSample)*PathSample; }
			if (DirectStereo) { DirectStereo[InterleavedSampleIndex] = DirectSample; }
			if (PathStereo) { PathStereo[InterleavedSampleIndex] = PathSample; }
		}
	}
	return true;
}
