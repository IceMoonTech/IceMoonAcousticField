#pragma once

// Standard-C++ Steam Audio SDK 4.8.1 bake/simulate adapter.
// No UE, no UObject, no worker threads, no file I/O. Every SDK object is
// owned by one serial caller. Detached reverb sources are retained in IR slots.
// Coordinate contract: all positions in FIMAcousticSceneInput and Evaluate
// are already SDK meters (GT converted); this adapter never rescales.
//
// Serial discipline: the owner calls every method serially. No locks are
// created here. Direct/path outputs copy scalar data; convolution uses the
// explicit slot ownership protocol in IMAcousticReverbData.h because the SDK IR
// is an opaque source-owned buffer. Fixed quality lives in IMAcousticBakeRecipe.h.

#include <phonon.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "IMAcousticAudioRenderer.h"
#include "IMAcousticReverbData.h"

// Frozen scene description. Vertices/Triangles/MaterialIndices follow the
// IPLStaticMeshSettings layout; each MaterialIndices entry addresses Materials.
struct FIMAcousticSceneInput
{
	std::vector<IPLVector3> Vertices;
	std::vector<IPLTriangle> Triangles;
	std::vector<int> MaterialIndices;
	std::vector<IPLMaterial> Materials;
	std::vector<IPLSphere> Probes;
};

// Frozen bake payload. Scene holds the serialized DEFAULT scene (geometry
// included); ProbeBatch holds the probes plus baked pathing (DYNAMIC) and
// listener-position baked convolution reverb (REVERB) layers. Opaque bytes;
// persistence is the owner's job, not this adapter's.
struct FIMAcousticBakeData
{
	std::vector<std::uint8_t> Scene;
	std::vector<std::uint8_t> ProbeBatch;
	// Validated metadata, in SDK meters and identical order to ProbeBatch.
	// Public C API has no runtime probe-sphere getter. Reverb fails closed if
	// a loader omits this coverage information; direct/path remain available.
	std::vector<IPLSphere> CoverageProbes;
};

struct FIMAcousticSourceInput
{
	std::uint64_t SourceKey;
	std::uint64_t Generation;
	IPLCoordinateSpace3 Source;
};

// H1 W2: value-only description of one movable rigid body. Geometry is kept
// in the instance sub-scene's local SDK meters; Transform places it in the
// loaded parent scene. The worker owns every SDK handle created from this
// payload, so no UObject or borrowed engine memory crosses the thread edge.
struct FIMAcousticDynamicMeshInput
{
	std::uint64_t Key = 0;
	std::uint64_t GeometryHash = 0;
	FIMAcousticSceneInput Geometry;
	IPLMatrix4x4 Transform{};
};

struct FIMAcousticDynamicMeshReadback
{
	std::uint64_t Key = 0;
	std::uint64_t GeometryHash = 0;
	IPLMatrix4x4 Transform{};
};

// H1 W1: runtime-settable Hybrid pathing inputs. Has* tracks explicit caller
// intent: a missing field fails closed instead of silently taking a default.
// DefaultHybrid is the prescribed Hybrid payload (validation on, alternates
// on); the adapter itself never invents it. SDK note: findAlternatePaths is
// honored only when enableValidation is on.
struct FIMAcousticPathingOptions
{
	bool EnableValidation = true;
	bool FindAlternatePaths = true;
	bool HasEnableValidation = false;
	bool HasFindAlternatePaths = false;
	static FIMAcousticPathingOptions DefaultHybrid()
	{
		FIMAcousticPathingOptions O;
		O.EnableValidation = true;
		O.FindAlternatePaths = true;
		O.HasEnableValidation = true;
		O.HasFindAlternatePaths = true;
		return O;
	}
	bool IsComplete() const { return HasEnableValidation && HasFindAlternatePaths; }
	std::string MissingFields() const
	{
		std::string M;
		if (!HasEnableValidation) { M += "enableValidation"; }
		if (!HasFindAlternatePaths) { if (!M.empty()) { M += ", "; } M += "findAlternatePaths"; }
		return M;
	}
};

// H1 W1 readback: the exact option values consumed at each creation/change
// point. The C API has no source-input getter, so the write-time record of
// the IPLSimulationInputs fields handed to iplSourceSetInputs (plus the
// creation-time resolution at Load) IS the readback.
struct FIMAcousticPathingReadback
{
	bool EnableValidationApplied = false;
	bool FindAlternatePathsApplied = false;
	bool AppliedToSdk = false;
	std::string AppliedAt;
	std::uint64_t ApplyCount = 0;
	std::uint64_t AppliedSources = 0;
	std::uint64_t LastSourceKey = 0;
	std::uint64_t LastGeneration = 0;
};

// Optional runtime aperture contract. Disabled by default so ordinary SDK
// pathing and no-door/open scenes keep their existing behavior. Coordinates
// are SDK meters; the half extent is a conservative axis-aligned bound.
struct FIMAcousticApertureTransitPolicy
{
	bool Enabled = false;
	IPLVector3 Center{0.0f, 0.0f, 0.0f};
	IPLVector3 HalfExtent{0.0f, 0.0f, 0.0f};
};

class FIMAcousticSimulation final
{
public:
	FIMAcousticSimulation() = default;
	~FIMAcousticSimulation();
	FIMAcousticSimulation(const FIMAcousticSimulation&) = delete;
	FIMAcousticSimulation& operator=(const FIMAcousticSimulation&) = delete;

	// Builds a transient SDK scene, bakes pathing + REVERB convolution reverb
	// with real SDK bakers, and swaps the serialized bytes into OutBake.
	// Never touches loaded runtime state. OutBake is swapped only on success.
	// CancelRequested is cooperative: checked before start, after each SDK bake
	// pass, and before the final swap (which it then skips). A cancel never
	// modifies OutBake. No SDK cancel API is used; the running pass completes
	// first, so the editor waits for it.
	bool Bake(const FIMAcousticSceneInput& Input, FIMAcousticBakeData& OutBake,
		std::string& OutError, const std::atomic<bool>* CancelRequested = nullptr);

	// Generates UNIFORMFLOOR probes with the real SDK generator on a transient
	// scene built from Geometry (whose Probes may be empty; geometry/material
	// validation matches Bake). Empty or invalid generation fails; a complete
	// temporary swaps into OutProbes only on success.
	bool GenerateProbes(const FIMAcousticSceneInput& Geometry,
		const IPLProbeGenerationParams& Params, std::vector<IPLSphere>& OutProbes,
		std::string& OutError);

	// Replaces any loaded runtime: deserializes Bake, creates the simulator,
	// binds scene + probe batch. Rates in Hz; frames in samples.
	bool Load(const FIMAcousticBakeData& Bake, int SampleRateHz, int BlockFrames,
		std::string& OutError);

	// H1 W2: synchronizes the complete current rigid-body set. Missing keys
	// are removed, new keys create an instance sub-scene, and retained keys
	// update the local-to-world matrix before one parent-scene commit.
	bool SyncDynamicMeshes(const std::vector<FIMAcousticDynamicMeshInput>& Input,
		std::string& OutError);
	std::vector<FIMAcousticDynamicMeshReadback> GetDynamicMeshReadback() const;

	// Batch path: sets every source's inputs, then commits and runs once.
	// Output order matches input order. Generation 0, empty batches, and
	// duplicate keys are rejected; a changed generation removes and recreates
	// the source; more than 32 live sources are rejected. PathValid is detected
	// from real output (audible EQ energy plus nonzero SH energy), never
	// defaulted true. Generation passes through; Sequence increments per
	// source on success. Output entries are overwritten on every return.
	bool EvaluateBatch(const std::vector<FIMAcousticSourceInput>& Input,
		const IPLCoordinateSpace3& Listener, std::vector<FIMAcousticAudioFrame>& Output,
		std::string& OutError);

	// H1 W1: explicit Hybrid pathing inputs. Incomplete options fail closed
	// (missing field named in OutError); state is unchanged on failure.
	// Every successful call refreshes the change readback (AppliedToSdk=false
	// until the values reach the SDK at Load/EvaluateBatch).
	bool SetPathingOptions(const FIMAcousticPathingOptions& Options, std::string& OutError);
	FIMAcousticPathingOptions GetPathingOptions() const;
	FIMAcousticPathingReadback GetPathingReadback() const;
	bool SetApertureTransitPolicy(const FIMAcousticApertureTransitPolicy& Policy,
		std::string& OutError);
	FIMAcousticApertureTransitPolicy GetApertureTransitPolicy() const;
	bool WasApertureTransitBlocked() const { return LastApertureTransitBlocked; }
	std::uint64_t GetLastApertureTransitCycles() const { return LastApertureTransitCycles; }

	// Single-source test convenience over EvaluateBatch. Kept for tests only.
	bool Evaluate(std::uint64_t SourceKey, std::uint64_t Generation,
		const IPLCoordinateSpace3& Source, const IPLCoordinateSpace3& Listener,
		FIMAcousticAudioFrame& OutFrame, std::string& OutError);

	void Remove(std::uint64_t SourceKey);
	// Worker only, with exclusive Writing ownership of Slot. Source is removed
	// and committed before return, freezing the IR until that slot is recycled.
	bool EvaluateReverb(FIMAcousticReverbSlot& Slot,const IPLCoordinateSpace3& Listener,
		std::string& OutError);
	void Shutdown();
	// Serial-owner diagnostic hook. SDK invokes it synchronously during pathing;
	// caller keeps UserData alive until the next hook change. Audio never calls it.
	void SetPathVisualization(IPLPathingVisualizationCallback Callback,void* UserData)
	{PathCallback=Callback;PathUserData=UserData;}

private:
	struct FIMSourceRecord
	{
		IPLSource Source = nullptr;
		std::uint64_t Generation = 0;
		std::uint64_t Sequence = 0;
	};

	struct FIMDynamicMeshRecord
	{
		IPLScene SubScene = nullptr;
		IPLStaticMesh Mesh = nullptr;
		IPLInstancedMesh Instance = nullptr;
		std::uint64_t GeometryHash = 0;
		IPLMatrix4x4 Transform{};
		IPLVector3 LocalMin{0.0f, 0.0f, 0.0f};
		IPLVector3 LocalMax{0.0f, 0.0f, 0.0f};
	};

	IPLContext Context = nullptr;
	IPLScene Scene = nullptr;
	IPLProbeBatch ProbeBatch = nullptr;
	IPLSimulator Simulator = nullptr;
	FIMAcousticPathingOptions PathingOptions;
	FIMAcousticPathingReadback PathingReadback;
	FIMAcousticApertureTransitPolicy ApertureTransitPolicy;
	bool LastApertureTransitBlocked = false;
	std::uint64_t LastApertureTransitCycles = 0;
	bool RequirePathingOptions(const char* What, std::string& OutError) const;
	std::vector<IPLSphere> CoverageProbes;
	std::map<std::uint64_t, FIMSourceRecord> Sources;
	std::map<std::uint64_t, FIMDynamicMeshRecord> DynamicMeshes;
	int SampleRateHz = 0;
	int BlockFrames = 0;
	bool Loaded = false;
	IPLPathingVisualizationCallback PathCallback=nullptr;
	void* PathUserData=nullptr;
};
