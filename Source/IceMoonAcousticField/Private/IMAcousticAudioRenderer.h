#pragma once

// SDK-only audio kernel. The UE adapter and independent executable use the same
// implementation; no UObject, simulator, allocation or lock is needed by Render.
#include <phonon.h>
#include <array>
#include <cstdint>
#include <vector>

struct FIMAcousticAudioFrame
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

// Optional direct-path shaping for the exaggerated presence mode. nullptr (default)
// keeps the physical render bit-identical. Only the direct effect/HRTF direction
// are shaped; the path effect and every other part of the frame are untouched.
struct FIMAcousticDirectShaping
{
	// Replace the SDK inverse-distance gain by 1 (own footsteps stay near).
	bool bUnitDistanceAttenuation = false;
	// Multiplier for the source's own lateral azimuth (below); 1 keeps the HRTF direction as solved.
	float AzimuthScale = 1.0f;
	// The source's lateral offset from its owner's body axis as an azimuth (radians, right positive), published by the GT.
	// Only this part is scaled, so a listener swing or snapshot lag that shifts the whole direction is not amplified with it.
	float LateralAzimuthRadians = 0.0f;
	// Largest azimuth (radians) a scaled direction may reach.
	float AzimuthLimitRadians = 0.0f;

	// Adds (Scale - 1) * Lateral to the azimuth of a listener-local direction (x right, y up, -z ahead),
	// keeping elevation. Sources behind the listener and invalid input stay unchanged.
	static IPLVector3 ScaleAzimuth(const IPLVector3& Direction, float Scale, float LateralRadians, float LimitRadians);
};

enum class EIMAcousticRenderFailure : unsigned char { None, InvalidFrame, NonfiniteInput, NonfiniteDirect, NonfinitePath, Count };
struct FIMAcousticAudioMetrics
{
	double DirectEnergy = 0.0;
	double PathEnergy = 0.0;
	EIMAcousticRenderFailure Failure=EIMAcousticRenderFailure::None;
};

class FIMAcousticAudioRenderer final
{
public:
	FIMAcousticAudioRenderer() = default;
	~FIMAcousticAudioRenderer();
	FIMAcousticAudioRenderer(const FIMAcousticAudioRenderer&) = delete;
	FIMAcousticAudioRenderer& operator=(const FIMAcousticAudioRenderer&) = delete;

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
	bool Render(const float* DryMono, int Frames, const FIMAcousticAudioFrame& Frame,
		float* Stereo, float* DirectStereo = nullptr, float* PathStereo = nullptr,
		FIMAcousticAudioMetrics* Metrics = nullptr, std::uint32_t AudibleRoutes = 3,
		const FIMAcousticDirectShaping* Shaping = nullptr);

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
