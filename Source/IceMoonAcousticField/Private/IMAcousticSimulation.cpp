#include "IMAcousticSimulation.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticSDKContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "HAL/PlatformTime.h"

// W1 fixed small-scale config. Units follow the SDK headers (meters, seconds,
// fractions in [0,1]). Each constant states its quality direction; none of
// them is a verified performance claim. Raise only with measured need.
namespace
{
// Although phonon.h calls this optional, the pinned Win64 4.8.1 path baker
// calls a null callback (native W1 fault RIP=0 at iplPathBakerBake; upstream
// issue #167 describes the same failure). Always supply a live callback.
void IPLCALL IMBakeProgress(IPLfloat32, void*) {}

bool IMSimulationFinite(float V) { return std::isfinite(V) != 0; }
bool IMSimulationFinite(const IPLVector3& V) { return IMSimulationFinite(V.x) && IMSimulationFinite(V.y) && IMSimulationFinite(V.z); }
bool IMFiniteSpace(const IPLCoordinateSpace3& S)
{
    return IMSimulationFinite(S.right) && IMSimulationFinite(S.up) && IMSimulationFinite(S.ahead) && IMSimulationFinite(S.origin);
}
bool IMFiniteMatrix(const IPLMatrix4x4& M)
{
    for (int R = 0; R < 4; ++R)
    {
        for (int C = 0; C < 4; ++C)
        {
            if (!IMSimulationFinite(M.elements[R][C])) { return false; }
        }
    }
    // The SDK consumes affine local-to-world matrices. Reject projective or
    // non-homogeneous payloads before they can enter the scene BVH.
    return std::abs(M.elements[3][0]) <= 1.e-4f && std::abs(M.elements[3][1]) <= 1.e-4f
        && std::abs(M.elements[3][2]) <= 1.e-4f && std::abs(M.elements[3][3] - 1.0f) <= 1.e-4f;
}
IPLVector3 IMTransformPoint(const IPLMatrix4x4& M, const IPLVector3& P)
{
    return {M.elements[0][0] * P.x + M.elements[0][1] * P.y + M.elements[0][2] * P.z + M.elements[0][3],
        M.elements[1][0] * P.x + M.elements[1][1] * P.y + M.elements[1][2] * P.z + M.elements[1][3],
        M.elements[2][0] * P.x + M.elements[2][1] * P.y + M.elements[2][2] * P.z + M.elements[2][3]};
}
bool IMFiniteMaterial(const IPLMaterial& M)
{
    for (int B = 0; B < IPL_NUM_BANDS; ++B)
    {
        if (!IMSimulationFinite(M.absorption[B]) || M.absorption[B] < 0.0f || M.absorption[B] > 1.0f) { return false; }
        if (!IMSimulationFinite(M.transmission[B]) || M.transmission[B] < 0.0f || M.transmission[B] > 1.0f) { return false; }
    }
    return IMSimulationFinite(M.scattering) && M.scattering >= 0.0f && M.scattering <= 1.0f;
}
// Geometry/material validation shared by Bake and GenerateProbes. Probes may
// be empty only when RequireProbes is false; Bake always requires them.
bool IMValidateBakeGeometry(const IM_AcousticSceneInput& In, bool RequireProbes,
    const char* What, std::string& OutError)
{
    static_assert(sizeof(int) == sizeof(IPLint32), "MaterialIndices must alias IPLint32");
    const auto TooBig = [](std::size_t N) {
        return N == 0 || N > static_cast<std::size_t>((std::numeric_limits<IPLint32>::max)());
    };
    const std::string Tag = std::string(What) + ": ";
    if (TooBig(In.Vertices.size())) { OutError = Tag + "Vertices empty or exceeds int32."; return false; }
    if (TooBig(In.Triangles.size())) { OutError = Tag + "Triangles empty or exceeds int32."; return false; }
    if (In.MaterialIndices.size() != In.Triangles.size()) { OutError = Tag + "MaterialIndices size mismatch."; return false; }
    if (TooBig(In.Materials.size())) { OutError = Tag + "Materials empty or exceeds int32."; return false; }
    if (RequireProbes && In.Probes.empty()) { OutError = Tag + "Probes empty."; return false; }
    for (const auto& V : In.Vertices)
    {
        if (!IMSimulationFinite(V)) { OutError = Tag + "non-finite vertex."; return false; }
    }
    const IPLint32 NumVerts = static_cast<IPLint32>(In.Vertices.size());
    for (const auto& T : In.Triangles)
    {
        for (int K = 0; K < 3; ++K)
        {
            if (T.indices[K] < 0 || T.indices[K] >= NumVerts) { OutError = Tag + "triangle index out of range."; return false; }
        }
    }
    const int NumMats = static_cast<int>(In.Materials.size());
    for (int MI : In.MaterialIndices)
    {
        if (MI < 0 || MI >= NumMats) { OutError = Tag + "material index out of range."; return false; }
    }
    for (const auto& M : In.Materials)
    {
        if (!IMFiniteMaterial(M)) { OutError = Tag + "material out of [0,1] or non-finite."; return false; }
    }
    for (const auto& P : In.Probes)
    {
        if (!IMSimulationFinite(P.center) || !IMSimulationFinite(P.radius) || P.radius <= 0.0f)
        {
            OutError = Tag + "invalid probe sphere."; return false;
        }
    }
    return true;
}

// Rejects bad input before any SDK call; never fabricates success.
// The empty-Probes rejection stays here: Bake never accepts probeless input.
bool IMValidateSceneInput(const IM_AcousticSceneInput& In, std::string& OutError)
{
    return IMValidateBakeGeometry(In, true, "Bake", OutError);
}

// Only UNIFORMFLOOR generation is supported. Spacing/height are meters.
bool IMValidateProbeGenParams(const IPLProbeGenerationParams& P, std::string& OutError)
{
    if (P.type != IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR)
    {
        OutError = "GenerateProbes: only UNIFORMFLOOR generation is supported."; return false;
    }
    if (!IMSimulationFinite(P.spacing) || P.spacing <= 0.0f)
    {
        OutError = "GenerateProbes: spacing must be finite and positive (m)."; return false;
    }
    if (!IMSimulationFinite(P.height) || P.height <= 0.0f)
    {
        OutError = "GenerateProbes: height must be finite and positive (m)."; return false;
    }
    for (int R = 0; R < 4; ++R)
    {
        for (int C = 0; C < 4; ++C)
        {
            if (!IMSimulationFinite(P.transform.elements[R][C]))
            {
                OutError = "GenerateProbes: non-finite transform element."; return false;
            }
        }
    }
    return true;
}

bool IMCopySerialized(IPLSerializedObject Obj, std::vector<std::uint8_t>& Out,
    std::string& OutError, const char* What)
{
    const IPLsize Size = iplSerializedObjectGetSize(Obj);
    const IPLbyte* Data = iplSerializedObjectGetData(Obj);
    if (Size == 0 || Data == nullptr)
    {
        OutError = std::string("Bake: serialized ") + What + " is empty.";
        return false;
    }
    Out.assign(Data, Data + Size);
    return true;
}
// RAII owner of one transient bake-time scene: a retained lease of the single
// shared SDK context, one DEFAULT CPU scene, and one committed static mesh.
// Shared by Bake and GenerateProbes. Destruction releases mesh, then scene,
// then the context lease, in that order. Borrowed input arrays must outlive
// this scope; no SDK handle escapes it.
struct IM_BakeSceneScope
{
    IPLContext Ctx = nullptr;
    IPLScene Scene = nullptr;
    IPLStaticMesh Mesh = nullptr;
    IM_BakeSceneScope() = default;
    IM_BakeSceneScope(const IM_BakeSceneScope&) = delete;
    IM_BakeSceneScope& operator=(const IM_BakeSceneScope&) = delete;
    ~IM_BakeSceneScope() { Reset(); }
    bool Build(const IM_AcousticSceneInput& Geo, const char* What, std::string& OutError)
    {
        Reset();
        const IPLContext Shared = IM_GetAcousticSDKContext();
        Ctx = Shared ? iplContextRetain(Shared) : nullptr;
        if (Ctx == nullptr) { OutError = std::string(What) + ": shared SDK context unavailable."; return false; }
        IPLSceneSettings SceneSettings{};
        SceneSettings.type = IPL_SCENETYPE_DEFAULT; // CPU; the only type iplSceneSave accepts.
        if (iplSceneCreate(Ctx, &SceneSettings, &Scene) != IPL_STATUS_SUCCESS || Scene == nullptr)
        {
            OutError = std::string(What) + ": iplSceneCreate failed."; return false;
        }
        IPLStaticMeshSettings MeshSettings{};
        MeshSettings.numVertices = static_cast<IPLint32>(Geo.Vertices.size());
        MeshSettings.numTriangles = static_cast<IPLint32>(Geo.Triangles.size());
        MeshSettings.numMaterials = static_cast<IPLint32>(Geo.Materials.size());
        // Geo outlives the scope user, so these borrowed pointers stay valid
        // through commit, bakes/generation, and saves.
        MeshSettings.vertices = const_cast<IPLVector3*>(Geo.Vertices.data());
        MeshSettings.triangles = const_cast<IPLTriangle*>(Geo.Triangles.data());
        MeshSettings.materialIndices = const_cast<IPLint32*>(
            reinterpret_cast<const IPLint32*>(Geo.MaterialIndices.data()));
        MeshSettings.materials = const_cast<IPLMaterial*>(Geo.Materials.data());
        if (iplStaticMeshCreate(Scene, &MeshSettings, &Mesh) != IPL_STATUS_SUCCESS || Mesh == nullptr)
        {
            OutError = std::string(What) + ": iplStaticMeshCreate failed."; return false;
        }
        iplStaticMeshAdd(Mesh, Scene);
        iplSceneCommit(Scene);
        return true;
    }
    void Reset()
    {
        if (Mesh != nullptr) { iplStaticMeshRelease(&Mesh); }
        if (Scene != nullptr) { iplSceneRelease(&Scene); }
        if (Ctx != nullptr) { iplContextRelease(&Ctx); }
    }
};
} // namespace

IM_AcousticSimulation::~IM_AcousticSimulation() { Shutdown(); }
bool IM_AcousticSimulation::SetPathingOptions(const IM_AcousticPathingOptions& Options, std::string& OutError)
{
    if (!Options.IsComplete())
    {
        OutError = std::string("SetPathingOptions: missing explicit field(s): ") + Options.MissingFields()
            + "; pass every field or IM_AcousticPathingOptions::DefaultHybrid().";
        return false;
    }
    PathingOptions = Options;
    PathingReadback.EnableValidationApplied = Options.EnableValidation;
    PathingReadback.FindAlternatePathsApplied = Options.FindAlternatePaths;
    PathingReadback.AppliedToSdk = false;
    PathingReadback.AppliedAt = "SetPathingOptions";
    PathingReadback.AppliedSources = 0;
    PathingReadback.LastSourceKey = 0;
    PathingReadback.LastGeneration = 0;
    return true;
}
IM_AcousticPathingOptions IM_AcousticSimulation::GetPathingOptions() const { return PathingOptions; }
IM_AcousticPathingReadback IM_AcousticSimulation::GetPathingReadback() const { return PathingReadback; }
bool IM_AcousticSimulation::SetApertureTransitPolicy(
    const IM_AcousticApertureTransitPolicy& Policy, std::string& OutError)
{
    OutError.clear();
    if (!Policy.Enabled)
    {
        ApertureTransitPolicy = {};
        return true;
    }
    if (!IMSimulationFinite(Policy.Center) || !IMSimulationFinite(Policy.HalfExtent)
        || Policy.HalfExtent.x <= 0.0f || Policy.HalfExtent.y <= 0.0f || Policy.HalfExtent.z <= 0.0f)
    {
        OutError = "SetApertureTransitPolicy: enabled policy needs finite positive half extents.";
        return false;
    }
    ApertureTransitPolicy = Policy;
    return true;
}
IM_AcousticApertureTransitPolicy IM_AcousticSimulation::GetApertureTransitPolicy() const
{
    return ApertureTransitPolicy;
}
bool IM_AcousticSimulation::RequirePathingOptions(const char* What, std::string& OutError) const
{
    if (PathingOptions.IsComplete()) { return true; }
    OutError = std::string(What) + ": pathing options incomplete, missing explicit field(s): "
        + PathingOptions.MissingFields() + "; call SetPathingOptions first.";
    return false;
}
bool IM_AcousticSimulation::Bake(const IM_AcousticSceneInput& Input, IM_AcousticBakeData& OutBake,
    std::string& OutError, const std::atomic<bool>* CancelRequested)
{
    // Cooperative cancellation only: a running SDK pass cannot be aborted
    // mid-flight (no cancel API by contract), so the editor waits for it.
    // The checks below run between passes; a cancel never modifies OutBake.
    const auto Cancelled = [&] { return CancelRequested != nullptr && CancelRequested->load(); };
    if (Cancelled()) { OutError = "Bake: cancelled before start."; return false; }
    if (!IMValidateSceneInput(Input, OutError)) { return false; }

    IM_BakeSceneScope Scope;
    IPLProbeBatch Batch = nullptr;
    IPLSerializedObject SceneObj = nullptr;
    IPLSerializedObject BatchObj = nullptr;
    std::vector<std::uint8_t> TmpScene;
    std::vector<std::uint8_t> TmpBatch;
    bool Ok = false;
    std::string Error;

    do
    {
        if (!Scope.Build(Input, "Bake", Error)) { break; }
        if (iplProbeBatchCreate(Scope.Ctx, &Batch) != IPL_STATUS_SUCCESS || Batch == nullptr)
        {
            Error = "Bake: iplProbeBatchCreate failed."; break;
        }
        for (const auto& P : Input.Probes) { iplProbeBatchAddProbe(Batch, P); }
        iplProbeBatchCommit(Batch);

        IPLBakedDataIdentifier PathId{};
        PathId.type = IPL_BAKEDDATATYPE_PATHING;
        PathId.variation = IPL_BAKEDDATAVARIATION_DYNAMIC; // every probe pair.
        // Dynamic pathing uses the canonical zero endpoint sphere. Nonzero
        // static-endpoint fields produce a different layer that runtime cannot find.
        IPLPathBakeParams PathParams{};
        PathParams.scene = Scope.Scene;
        PathParams.probeBatch = Batch;
        PathParams.identifier = PathId;
        PathParams.numSamples = IM_AcousticRecipe::PathNumSamples;
        PathParams.radius = IM_AcousticRecipe::PathRadiusM;
        PathParams.threshold = IM_AcousticRecipe::PathThreshold;
        PathParams.visRange = IM_AcousticRecipe::PathVisRangeM;
        PathParams.pathRange = IM_AcousticRecipe::PathRangeM;
        PathParams.numThreads = IM_AcousticRecipe::BakeThreads;
        iplPathBakerBake(Scope.Ctx, &PathParams, IMBakeProgress, nullptr);
        if (Cancelled()) { Error = "Bake: cancelled after pathing pass."; break; }
        if (iplProbeBatchGetDataSize(Batch, &PathId) == 0)
        {
            Error = "Bake: pathing layer empty after iplPathBakerBake."; break;
        }

        IPLBakedDataIdentifier ReverbId{};
        ReverbId.type = IPL_BAKEDDATATYPE_REFLECTIONS;
        ReverbId.variation = IPL_BAKEDDATAVARIATION_REVERB; // source and listener both at the probe: listener-position reverb.
        IPLReflectionsBakeParams ReverbParams{};
        ReverbParams.scene = Scope.Scene;
        ReverbParams.probeBatch = Batch;
        ReverbParams.sceneType = IPL_SCENETYPE_DEFAULT;
        ReverbParams.identifier = ReverbId;
        ReverbParams.bakeFlags = IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION; // convolution IR per probe; parametric would substitute the plan.
        ReverbParams.numRays = IM_AcousticRecipe::ReverbNumRays;
        ReverbParams.numDiffuseSamples = IM_AcousticRecipe::ReverbNumDiffuse;
        ReverbParams.numBounces = IM_AcousticRecipe::ReverbNumBounces;
        ReverbParams.simulatedDuration = IM_AcousticRecipe::ReverbSimDurationS;
        ReverbParams.savedDuration = IM_AcousticRecipe::ReverbSavedDurationS; // saved convolution IR length (s); cost scales with probe count.
        ReverbParams.order = IM_AcousticAudioFrame::Order;
        ReverbParams.numThreads = IM_AcousticRecipe::BakeThreads;
        ReverbParams.rayBatchSize = 1; // custom ray tracer unused.
        ReverbParams.irradianceMinDistance = IM_AcousticRecipe::IrradianceMinM;
        ReverbParams.bakeBatchSize = 1; // only used by STATICLISTENER; kept as a well-formed placeholder.
        ReverbParams.openCLDevice = nullptr;
        ReverbParams.radeonRaysDevice = nullptr;
        iplReflectionsBakerBake(Scope.Ctx, &ReverbParams, IMBakeProgress, nullptr);
        if (Cancelled()) { Error = "Bake: cancelled after reverb pass."; break; }
        if (iplProbeBatchGetDataSize(Batch, &ReverbId) == 0)
        {
            Error = "Bake: reverb layer empty after iplReflectionsBakerBake."; break;
        }

        IPLSerializedObjectSettings SerSettings{};
        if (iplSerializedObjectCreate(Scope.Ctx, &SerSettings, &SceneObj) != IPL_STATUS_SUCCESS || SceneObj == nullptr)
        {
            Error = "Bake: scene serialized-object create failed."; break;
        }
        iplSceneSave(Scope.Scene, SceneObj);
        if (!IMCopySerialized(SceneObj, TmpScene, Error, "scene")) { break; }
        if (iplSerializedObjectCreate(Scope.Ctx, &SerSettings, &BatchObj) != IPL_STATUS_SUCCESS || BatchObj == nullptr)
        {
            Error = "Bake: batch serialized-object create failed."; break;
        }
        iplProbeBatchSave(Batch, BatchObj);
        if (!IMCopySerialized(BatchObj, TmpBatch, Error, "probe batch")) { break; }
        Ok = true;
    } while (false);

    if (SceneObj != nullptr) { iplSerializedObjectRelease(&SceneObj); }
    if (BatchObj != nullptr) { iplSerializedObjectRelease(&BatchObj); }
    if (Batch != nullptr) { iplProbeBatchRelease(&Batch); }
    // Scope destruction releases mesh, scene, then the shared-context lease, in that order.

    if (!Ok) { OutError = Error; return false; }
    if (Cancelled()) { OutError = "Bake: cancelled before finalize."; return false; }
    // Fully built temporaries swap last: no partial payload ever escapes.
    auto TmpProbes=Input.Probes;
    OutBake.Scene.swap(TmpScene);
    OutBake.ProbeBatch.swap(TmpBatch);
    OutBake.CoverageProbes.swap(TmpProbes);
    return true;
}

bool IM_AcousticSimulation::GenerateProbes(const IM_AcousticSceneInput& Geometry,
    const IPLProbeGenerationParams& Params, std::vector<IPLSphere>& OutProbes, std::string& OutError)
{
    if (!IMValidateBakeGeometry(Geometry, false, "GenerateProbes", OutError)) { return false; }
    if (!IMValidateProbeGenParams(Params, OutError)) { return false; }
    IM_BakeSceneScope Scope;
    if (!Scope.Build(Geometry, "GenerateProbes", OutError)) { return false; }

    IPLProbeArray Array = nullptr;
    if (iplProbeArrayCreate(Scope.Ctx, &Array) != IPL_STATUS_SUCCESS || Array == nullptr)
    {
        OutError = "GenerateProbes: iplProbeArrayCreate failed."; return false;
    }
    // The SDK keeps no reference to Params after the call returns.
    iplProbeArrayGenerateProbes(Array, Scope.Scene, const_cast<IPLProbeGenerationParams*>(&Params));
    const IPLint32 Count = iplProbeArrayGetNumProbes(Array);
    std::vector<IPLSphere> Tmp;
    bool Ok = false;
    std::string Error;
    if (Count <= 0)
    {
        Error = "GenerateProbes: SDK generated no probes for these params/volume.";
    }
    else
    {
        Tmp.reserve(static_cast<std::size_t>(Count));
        Ok = true;
        for (IPLint32 I = 0; I < Count; ++I)
        {
            const IPLSphere P = iplProbeArrayGetProbe(Array, I);
            if (!IMSimulationFinite(P.center) || !IMSimulationFinite(P.radius) || P.radius < 0.0f)
            {
                Ok = false; Error = "GenerateProbes: SDK returned a non-finite probe."; break;
            }
            IPLSphere Kept = P;
            if (!(Kept.radius > 0.0f))
            {
                Ok = false; Error = "GenerateProbes: SDK returned a non-positive influence radius."; break;
            }
            Tmp.push_back(Kept);
        }
    }
    iplProbeArrayRelease(&Array);
    if (!Ok) { OutError = Error; return false; }
    // Complete temporary swaps last: no partial probe set ever escapes.
    OutProbes.swap(Tmp);
    return true;
}

bool IM_AcousticSimulation::Load(const IM_AcousticBakeData& Bake, int SampleRateHzIn, int BlockFramesIn,
    std::string& OutError)
{
    // H1 W1: creation binds an explicit options identity; unset fields fail here, not silently below.
    if (!RequirePathingOptions("Load", OutError)) { return false; }
    if (Bake.Scene.empty() || Bake.ProbeBatch.empty()) { OutError = "Load: bake payload empty."; return false; }
    if (SampleRateHzIn <= 0 || BlockFramesIn <= 0)
    {
        OutError = "Load: sample rate and block frames must be positive."; return false;
    }
    Shutdown();

    IPLSerializedObject SceneObj = nullptr;
    IPLSerializedObject BatchObj = nullptr;
    bool Ok = false;
    std::string Error;
    do
    {
        const IPLContext SharedContext = IM_GetAcousticSDKContext();
        Context = SharedContext ? iplContextRetain(SharedContext) : nullptr;
        if (Context == nullptr)
        {
            Context = nullptr; Error = "Load: iplContextCreate failed."; break;
        }
        IPLSceneSettings SceneSettings{};
        SceneSettings.type = IPL_SCENETYPE_DEFAULT;
        IPLSerializedObjectSettings SceneSer{};
        SceneSer.data = const_cast<IPLbyte*>(Bake.Scene.data()); // wrapped read-only for this call.
        SceneSer.size = Bake.Scene.size();
        if (iplSerializedObjectCreate(Context, &SceneSer, &SceneObj) != IPL_STATUS_SUCCESS || SceneObj == nullptr)
        {
            SceneObj = nullptr; Error = "Load: scene serialized-object create failed."; break;
        }
        if (iplSceneLoad(Context, &SceneSettings, SceneObj, nullptr, nullptr, &Scene) != IPL_STATUS_SUCCESS
            || Scene == nullptr)
        {
            Scene = nullptr; Error = "Load: iplSceneLoad failed."; break;
        }
        IPLSerializedObjectSettings BatchSer{};
        BatchSer.data = const_cast<IPLbyte*>(Bake.ProbeBatch.data());
        BatchSer.size = Bake.ProbeBatch.size();
        if (iplSerializedObjectCreate(Context, &BatchSer, &BatchObj) != IPL_STATUS_SUCCESS || BatchObj == nullptr)
        {
            BatchObj = nullptr; Error = "Load: batch serialized-object create failed."; break;
        }
        if (iplProbeBatchLoad(Context, BatchObj, &ProbeBatch) != IPL_STATUS_SUCCESS || ProbeBatch == nullptr)
        {
            ProbeBatch = nullptr; Error = "Load: iplProbeBatchLoad failed."; break;
        }
        IPLBakedDataIdentifier RuntimePathId{};
        RuntimePathId.type = IPL_BAKEDDATATYPE_PATHING;
        RuntimePathId.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
        if (iplProbeBatchGetDataSize(ProbeBatch, &RuntimePathId) == 0)
        {
            Error = "Load: probe batch has no canonical dynamic pathing layer."; break;
        }
        // ProbeBatch deserialization restores probes/data, not its lookup tree.
        // In 4.8.1 only commit builds mProbeTree; simulatePathing dereferences it.
        // The cold-load native fixture reproduced the null tree without this call.
        iplProbeBatchCommit(ProbeBatch);
        IPLSimulationSettings SimSettings{};
        SimSettings.flags = static_cast<IPLSimulationFlags>(
            IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING | IPL_SIMULATIONFLAGS_REFLECTIONS);
        SimSettings.sceneType = IPL_SCENETYPE_DEFAULT;
        SimSettings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        SimSettings.maxNumOcclusionSamples = 1; // RAYCAST ignores samples; nonzero bound.
        SimSettings.maxNumRays = 1;             // reflections never run; nonzero bound.
        SimSettings.numDiffuseSamples = 1;
        SimSettings.maxDuration = IM_AcousticRecipe::ReverbSavedDurationS;
        SimSettings.maxOrder = IM_AcousticAudioFrame::Order;
        SimSettings.maxNumSources = IM_AcousticRecipe::MaxSources+1; // one attached listener reverb source at a time.
        SimSettings.numThreads = 1;             // serial simulation; no worker pool here.
        SimSettings.rayBatchSize = 1;
        SimSettings.numVisSamples = 1;          // nonzero bound; Hybrid pathing validation itself is runtime-settable (W1).
        SimSettings.samplingRate = SampleRateHzIn;
        SimSettings.frameSize = BlockFramesIn;
        SimSettings.openCLDevice = nullptr;
        SimSettings.radeonRaysDevice = nullptr;
        SimSettings.tanDevice = nullptr;
        if (iplSimulatorCreate(Context, &SimSettings, &Simulator) != IPL_STATUS_SUCCESS || Simulator == nullptr)
        {
            Simulator = nullptr; Error = "Load: iplSimulatorCreate failed."; break;
        }
        iplSimulatorSetScene(Simulator, Scene);
        iplSimulatorAddProbeBatch(Simulator, ProbeBatch);
        // Loaded scenes must commit before later-added instanced meshes take
        // part in validation raycasts; without this, dynamic doors validate
        // against the load-time static snapshot only (W3 closed-door leak).
        iplSceneCommit(Scene);
        iplSimulatorCommit(Simulator);
        SampleRateHz = SampleRateHzIn;
        BlockFrames = BlockFramesIn;
        CoverageProbes=Bake.CoverageProbes;
        // H1 W1 readback: creation-time resolution of the explicit options (struct -> creation -> readback).
        PathingReadback.EnableValidationApplied = PathingOptions.EnableValidation;
        PathingReadback.FindAlternatePathsApplied = PathingOptions.FindAlternatePaths;
        PathingReadback.AppliedToSdk = true;
        PathingReadback.AppliedAt = "Load";
        PathingReadback.AppliedSources = 0;
        PathingReadback.LastSourceKey = 0;
        PathingReadback.LastGeneration = 0;
        ++PathingReadback.ApplyCount;
        Loaded = true;
        Ok = true;
    } while (false);

    if (SceneObj != nullptr) { iplSerializedObjectRelease(&SceneObj); }
    if (BatchObj != nullptr) { iplSerializedObjectRelease(&BatchObj); }
    if (!Ok)
    {
        const std::string Kept = Error;
        Shutdown();
        OutError = Kept;
        return false;
    }
    return true;
}

bool IM_AcousticSimulation::SyncDynamicMeshes(const std::vector<IM_AcousticDynamicMeshInput>& Input,
    std::string& OutError)
{
    OutError.clear();
    if (!Loaded || Context == nullptr || Scene == nullptr || Simulator == nullptr)
    {
        OutError = "SyncDynamicMeshes: no loaded runtime; Load first.";
        return false;
    }

    std::map<std::uint64_t, const IM_AcousticDynamicMeshInput*> Desired;
    for (const auto& Item : Input)
    {
        if (Item.Key == 0)
        {
            OutError = "SyncDynamicMeshes: dynamic mesh key must be nonzero.";
            return false;
        }
        if (!Desired.emplace(Item.Key, &Item).second)
        {
            OutError = "SyncDynamicMeshes: duplicate dynamic mesh key.";
            return false;
        }
        if (!IMValidateBakeGeometry(Item.Geometry, false, "SyncDynamicMeshes", OutError)) { return false; }
        if (!IMFiniteMatrix(Item.Transform))
        {
            OutError = "SyncDynamicMeshes: transform must be a finite affine matrix.";
            return false;
        }
    }

    auto ReleaseRecord = [this](IM_DynamicMeshRecord& Record, bool RemoveFromScene)
    {
        if (Record.Instance != nullptr)
        {
            if (RemoveFromScene && Scene != nullptr) { iplInstancedMeshRemove(Record.Instance, Scene); }
            iplInstancedMeshRelease(&Record.Instance);
        }
        if (Record.Mesh != nullptr) { iplStaticMeshRelease(&Record.Mesh); }
        if (Record.SubScene != nullptr) { iplSceneRelease(&Record.SubScene); }
        Record = {};
    };

    auto CreateRecord = [this](const IM_AcousticDynamicMeshInput& Item,
        IM_DynamicMeshRecord& OutRecord, std::string& Error) -> bool
    {
        OutRecord = {};
        IPLSceneSettings SubSceneSettings{};
        SubSceneSettings.type = IPL_SCENETYPE_DEFAULT;
        if (iplSceneCreate(Context, &SubSceneSettings, &OutRecord.SubScene) != IPL_STATUS_SUCCESS
            || OutRecord.SubScene == nullptr)
        {
            Error = "SyncDynamicMeshes: iplSceneCreate failed.";
            return false;
        }
        const IM_AcousticSceneInput& Geometry = Item.Geometry;
        IPLStaticMeshSettings MeshSettings{};
        MeshSettings.numVertices = static_cast<IPLint32>(Geometry.Vertices.size());
        MeshSettings.numTriangles = static_cast<IPLint32>(Geometry.Triangles.size());
        MeshSettings.numMaterials = static_cast<IPLint32>(Geometry.Materials.size());
        MeshSettings.vertices = const_cast<IPLVector3*>(Geometry.Vertices.data());
        MeshSettings.triangles = const_cast<IPLTriangle*>(Geometry.Triangles.data());
        MeshSettings.materialIndices = const_cast<IPLint32*>(
            reinterpret_cast<const IPLint32*>(Geometry.MaterialIndices.data()));
        MeshSettings.materials = const_cast<IPLMaterial*>(Geometry.Materials.data());
        if (iplStaticMeshCreate(OutRecord.SubScene, &MeshSettings, &OutRecord.Mesh) != IPL_STATUS_SUCCESS
            || OutRecord.Mesh == nullptr)
        {
            Error = "SyncDynamicMeshes: iplStaticMeshCreate failed.";
            if (OutRecord.SubScene != nullptr) { iplSceneRelease(&OutRecord.SubScene); }
            return false;
        }
        iplStaticMeshAdd(OutRecord.Mesh, OutRecord.SubScene);
        iplSceneCommit(OutRecord.SubScene);
        IPLInstancedMeshSettings InstanceSettings{};
        InstanceSettings.subScene = OutRecord.SubScene;
        InstanceSettings.transform = Item.Transform;
        if (iplInstancedMeshCreate(Scene, &InstanceSettings, &OutRecord.Instance) != IPL_STATUS_SUCCESS
            || OutRecord.Instance == nullptr)
        {
            Error = "SyncDynamicMeshes: iplInstancedMeshCreate failed.";
            if (OutRecord.Mesh != nullptr) { iplStaticMeshRelease(&OutRecord.Mesh); }
            if (OutRecord.SubScene != nullptr) { iplSceneRelease(&OutRecord.SubScene); }
            return false;
        }
        iplInstancedMeshAdd(OutRecord.Instance, Scene);
        OutRecord.GeometryHash = Item.GeometryHash;
        OutRecord.Transform = Item.Transform;
        OutRecord.LocalMin = Item.Geometry.Vertices.front();
        OutRecord.LocalMax = Item.Geometry.Vertices.front();
        for (const IPLVector3& V : Item.Geometry.Vertices)
        {
            OutRecord.LocalMin.x = (std::min)(OutRecord.LocalMin.x, V.x);
            OutRecord.LocalMin.y = (std::min)(OutRecord.LocalMin.y, V.y);
            OutRecord.LocalMin.z = (std::min)(OutRecord.LocalMin.z, V.z);
            OutRecord.LocalMax.x = (std::max)(OutRecord.LocalMax.x, V.x);
            OutRecord.LocalMax.y = (std::max)(OutRecord.LocalMax.y, V.y);
            OutRecord.LocalMax.z = (std::max)(OutRecord.LocalMax.z, V.z);
        }
        return true;
    };

    bool Changed = false;
    for (auto It = DynamicMeshes.begin(); It != DynamicMeshes.end();)
    {
        if (Desired.find(It->first) == Desired.end())
        {
            ReleaseRecord(It->second, true);
            It = DynamicMeshes.erase(It);
            Changed = true;
        }
        else { ++It; }
    }
    for (const auto& DesiredEntry : Desired)
    {
        const IM_AcousticDynamicMeshInput& Item = *DesiredEntry.second;
        auto It = DynamicMeshes.find(Item.Key);
        if (It != DynamicMeshes.end() && It->second.GeometryHash == Item.GeometryHash)
        {
            iplInstancedMeshUpdateTransform(It->second.Instance, Scene, Item.Transform);
            It->second.Transform = Item.Transform;
            Changed = true;
            continue;
        }
        IM_DynamicMeshRecord Replacement;
        if (!CreateRecord(Item, Replacement, OutError))
        {
            ReleaseRecord(Replacement, false);
            return false;
        }
        if (It != DynamicMeshes.end())
        {
            ReleaseRecord(It->second, true);
            It->second = Replacement;
        }
        else { DynamicMeshes.emplace(Item.Key, Replacement); }
        Changed = true;
    }
    if (Changed)
    {
        // The SDK's scene commit is the visibility boundary for both direct
        // occlusion and validated pathing. The simulator commit keeps the
        // parent-scene attachment explicit for the loaded runtime.
        iplSceneCommit(Scene);
        iplSimulatorCommit(Simulator);
    }
    return true;
}

std::vector<IM_AcousticDynamicMeshReadback> IM_AcousticSimulation::GetDynamicMeshReadback() const
{
    std::vector<IM_AcousticDynamicMeshReadback> Readback;
    Readback.reserve(DynamicMeshes.size());
    for (const auto& Entry : DynamicMeshes)
    {
        IM_AcousticDynamicMeshReadback Item;
        Item.Key = Entry.first;
        Item.GeometryHash = Entry.second.GeometryHash;
        Item.Transform = Entry.second.Transform;
        Readback.push_back(Item);
    }
    return Readback;
}

bool IM_AcousticSimulation::EvaluateBatch(const std::vector<IM_AcousticSourceInput>& Input,
    const IPLCoordinateSpace3& Listener, std::vector<IM_AcousticAudioFrame>& Output, std::string& OutError)
{
    // H1 W1: application requires explicit options; a missing field fails here, never silently below.
    if (!RequirePathingOptions("EvaluateBatch", OutError)) { return false; }
    // Fail closed: fixed-size invalid frames keep positional correspondence, never stale data.
    Output.assign(Input.size(), IM_AcousticAudioFrame{});
    if (!Loaded || Context == nullptr || Scene == nullptr || ProbeBatch == nullptr || Simulator == nullptr)
    {
        OutError = "EvaluateBatch: no loaded runtime; Load first."; return false;
    }
    if (Input.empty()) { OutError = "EvaluateBatch: empty batch."; return false; }
    if (!IMFiniteSpace(Listener)) { OutError = "EvaluateBatch: non-finite listener frame."; return false; }
    for (std::size_t I = 0; I < Input.size(); ++I)
    {
        if (Input[I].Generation == 0) { OutError = "EvaluateBatch: generation 0 rejected."; return false; }
        if (!IMFiniteSpace(Input[I].Source)) { OutError = "EvaluateBatch: non-finite source frame."; return false; }
        for (std::size_t J = I + 1; J < Input.size(); ++J)
        {
            if (Input[J].SourceKey == Input[I].SourceKey)
            {
                OutError = "EvaluateBatch: duplicate source key in one batch."; return false;
            }
        }
    }

    std::vector<IM_SourceRecord*> Recs;
    Recs.reserve(Input.size());
    for (const auto& Item : Input)
    {
        auto It = Sources.find(Item.SourceKey);
        if (It != Sources.end() && It->second.Generation != Item.Generation)
        {
            // New voice generation: remove the old source (with its path cache)
            // and create fresh below; the sequence restarts.
            Remove(Item.SourceKey);
            It = Sources.end();
        }
        if (It == Sources.end())
        {
            if (Sources.size() >= static_cast<std::size_t>(IM_AcousticRecipe::MaxSources))
            {
                // Records created earlier in this batch stay valid runtime state;
                // only the frames are withheld. Caller must Remove first.
                OutError = "EvaluateBatch: source cap (32) reached; Remove first."; return false;
            }
            IPLSourceSettings SrcSettings{};
            SrcSettings.flags = static_cast<IPLSimulationFlags>(
                IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING);
            IPLSource NewSource = nullptr;
            if (iplSourceCreate(Simulator, &SrcSettings, &NewSource) != IPL_STATUS_SUCCESS || NewSource == nullptr)
            {
                OutError = "EvaluateBatch: iplSourceCreate failed."; return false;
            }
            iplSourceAdd(NewSource, Simulator);
            IM_SourceRecord Rec;
            Rec.Source = NewSource;
            Rec.Generation = Item.Generation;
            Rec.Sequence = 0;
            It = Sources.emplace(Item.SourceKey, Rec).first;
        }
        Recs.push_back(&It->second);
    }

    // Aperture transit is an explicit, caller-owned runtime contract. It is
    // intentionally a conservative AABB coverage test over synchronized
    // dynamic bodies: a closed body covering the authored aperture blocks
    // path output, while a parked/open body or an empty set does not.
    LastApertureTransitBlocked = false;
    LastApertureTransitCycles = 0;
    if (ApertureTransitPolicy.Enabled)
    {
        const uint64 TransitStart = FPlatformTime::Cycles64();
        const IPLVector3& C = ApertureTransitPolicy.Center;
        const IPLVector3& E = ApertureTransitPolicy.HalfExtent;
        const auto Contains = [&C, &E](const IM_DynamicMeshRecord& Record)
        {
            const float Inf = (std::numeric_limits<float>::max)();
            IPLVector3 Min{Inf, Inf, Inf};
            IPLVector3 Max{-Inf, -Inf, -Inf};
            for (int X = 0; X < 2; ++X)
                for (int Y = 0; Y < 2; ++Y)
                    for (int Z = 0; Z < 2; ++Z)
                    {
                        const IPLVector3 P{X ? Record.LocalMax.x : Record.LocalMin.x,
                            Y ? Record.LocalMax.y : Record.LocalMin.y,
                            Z ? Record.LocalMax.z : Record.LocalMin.z};
                        const IPLVector3 W = IMTransformPoint(Record.Transform, P);
                        Min.x = (std::min)(Min.x, W.x); Min.y = (std::min)(Min.y, W.y); Min.z = (std::min)(Min.z, W.z);
                        Max.x = (std::max)(Max.x, W.x); Max.y = (std::max)(Max.y, W.y); Max.z = (std::max)(Max.z, W.z);
                    }
            return C.x >= Min.x - E.x && C.x <= Max.x + E.x
                && C.y >= Min.y - E.y && C.y <= Max.y + E.y
                && C.z >= Min.z - E.z && C.z <= Max.z + E.z;
        };
        for (const auto& Entry : DynamicMeshes)
        {
            if (Contains(Entry.second)) { LastApertureTransitBlocked = true; break; }
        }
        LastApertureTransitCycles = FPlatformTime::Cycles64() - TransitStart;
    }

    IPLSimulationSharedInputs Shared{};
    Shared.listener = Listener;
    Shared.numRays = 1;          // This pass runs direct/path only; reverb has separate shared inputs.
    Shared.numBounces = 1;
    Shared.duration = 0.1f;      // seconds; reflections never run.
    Shared.order = IM_AcousticAudioFrame::Order;
    Shared.irradianceMinDistance = IM_AcousticRecipe::IrradianceMinM;
    Shared.pathingVisCallback = PathCallback;
    Shared.pathingUserData = PathUserData;
    iplSimulatorSetSharedInputs(Simulator, static_cast<IPLSimulationFlags>(
        IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING), &Shared);

    for (std::size_t I = 0; I < Input.size(); ++I)
    {
        IPLSimulationInputs Inputs{};
        Inputs.flags = static_cast<IPLSimulationFlags>(
            IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING);
        // W1: wall-blocked direct sound is occlusion only; transmission stays
        // off so no through-wall path is modeled.
        Inputs.directFlags = static_cast<IPLDirectSimulationFlags>(
            IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION
            | IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
        Inputs.source = Input[I].Source;
        Inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT; // inverse falloff, flat within 1 m.
        Inputs.distanceAttenuationModel.minDistance = 1.0f;                        // meters; INVERSEDISTANCE only.
        Inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;             // physical exponential falloff.
        Inputs.directivity.dipoleWeight = 0.0f; // unitless; 0 = pure omnidirectional.
        Inputs.directivity.dipolePower = 1.0f;
        Inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST; // one listener->source ray; cheapest exact occlusion.
        Inputs.occlusionRadius = 0.0f;                    // meters; volumetric only.
        Inputs.numOcclusionSamples = 1;
        Inputs.reverbScale[0] = Inputs.reverbScale[1] = Inputs.reverbScale[2] = 1.0f; // unitless; sim values as-is.
        Inputs.hybridReverbTransitionTime = 0.0f; // seconds; baked = false so unused.
        Inputs.hybridReverbOverlapPercent = 0.0f;
        Inputs.baked = IPL_FALSE; // Direct/path source; listener reverb uses a separate reflection-only source.
        Inputs.pathingProbes = ProbeBatch; // borrowed; consumed synchronously, never stored or shared.
        Inputs.visRadius = IM_AcousticRecipe::VisRadiusM;
        Inputs.visThreshold = IM_AcousticRecipe::VisThreshold;
        Inputs.visRange = IM_AcousticRecipe::VisRangeM;
        Inputs.pathingOrder = IM_AcousticAudioFrame::Order;
        // H1 W1: runtime-settable Hybrid pathing (Hybrid default: validation on, alternate paths on).
        Inputs.enableValidation = PathingOptions.EnableValidation ? IPL_TRUE : IPL_FALSE;
        Inputs.findAlternatePaths = PathingOptions.FindAlternatePaths ? IPL_TRUE : IPL_FALSE;
        Inputs.numTransmissionRays = 0; // transmission disabled; unused.
        Inputs.deviationModel = nullptr; // default physics deviation model.
        iplSourceSetInputs(Recs[I]->Source, Inputs.flags, &Inputs);
    }

    // One commit and one run for the whole batch: per-source run calls would
    // recompute every source each time (N^2).
    // H1 W1 readback: exact IPL values consumed by iplSourceSetInputs above.
    // The C API offers no source-input getter, so this write-time record is the readback.
    PathingReadback.EnableValidationApplied = PathingOptions.EnableValidation;
    PathingReadback.FindAlternatePathsApplied = PathingOptions.FindAlternatePaths;
    PathingReadback.AppliedToSdk = true;
    PathingReadback.AppliedAt = "EvaluateBatch";
    PathingReadback.AppliedSources = static_cast<std::uint64_t>(Input.size());
    PathingReadback.LastSourceKey = Input.back().SourceKey;
    PathingReadback.LastGeneration = Input.back().Generation;
    ++PathingReadback.ApplyCount;
    iplSimulatorCommit(Simulator);
    iplSimulatorRunDirect(Simulator);
    iplSimulatorRunPathing(Simulator);

    std::vector<IM_AcousticAudioFrame> Tmp;
    Tmp.reserve(Input.size());
    for (std::size_t I = 0; I < Input.size(); ++I)
    {
        IPLSimulationOutputs Outputs{};
        Outputs.direct.directivity = 1.0f; // neutral omni; overwritten below because DIRECTIVITY is enabled.
        Outputs.direct.transmission[0] = Outputs.direct.transmission[1] = Outputs.direct.transmission[2] = 1.0f; // neutral; flag off, never applied.
        Outputs.direct.transmissionType = IPL_TRANSMISSIONTYPE_FREQINDEPENDENT; // deterministic placeholder; flag off.
        iplSourceGetOutputs(Recs[I]->Source, static_cast<IPLSimulationFlags>(
            IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING), &Outputs);
        // SDK-owned pointers (notably pathing.shCoeffs) live only until the
        // next simulation call: copy now, store nothing.
        // outputs.direct.flags is NOT a trusted SDK output: configure it here
        // to mirror the enabled input flags exactly (no transmission).
        Outputs.direct.flags = static_cast<IPLDirectEffectFlags>(
            IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION
            | IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION);

        const IPLDirectEffectParams& D = Outputs.direct;
        bool DirectOk = IMSimulationFinite(D.distanceAttenuation) && IMSimulationFinite(D.directivity) && IMSimulationFinite(D.occlusion);
        for (int B = 0; DirectOk && B < IPL_NUM_BANDS; ++B)
        {
            DirectOk = IMSimulationFinite(D.airAbsorption[B]) && IMSimulationFinite(D.transmission[B]);
        }

        // CSource::getOutputs in 4.8.1 writes EQ and SH only, NOT pathing.order.
        // Buffer size is fixed by maxOrder/pathingOrder above. Also suppress the
        // SDK's unobstructed direct-path result here: our direct branch owns it.
        bool PathOk = Outputs.pathing.shCoeffs != nullptr && DirectOk && D.occlusion < 1.0f;
        float MaxEQ = 0.0f;
        for (int B = 0; PathOk && B < IPL_NUM_BANDS; ++B)
        {
            const float EQ = Outputs.pathing.eqCoeffs[B];
            if (!IMSimulationFinite(EQ) || EQ < 0.0f) { PathOk = false; break; }
            MaxEQ = (std::max)(MaxEQ, EQ);
        }
        float SHEnergy = 0.0f;
        for (int C = 0; PathOk && C < IM_AcousticAudioFrame::Coefficients; ++C)
        {
            const float SH = Outputs.pathing.shCoeffs[C];
            if (!IMSimulationFinite(SH)) { PathOk = false; break; }
            SHEnergy += SH * SH;
        }
        // Real detection: audible EQ energy AND nonzero SH energy. A zero-SH
        // path is never reported valid. This gate is never defaulted true.
        if (!(MaxEQ > 0.0f) || !(SHEnergy > 0.0f)) { PathOk = false; }
        // The OFF negative control deliberately disables runtime validation;
        // preserve the SDK-only result there so the control can expose the
        // difference between validated aperture transit and raw pathing.
        if (PathOk && PathingOptions.EnableValidation && LastApertureTransitBlocked) { PathOk = false; }

        IM_AcousticAudioFrame Frame;
        Frame.Generation = Input[I].Generation;
        Frame.Sequence = Recs[I]->Sequence + 1;
        Frame.DirectValid = DirectOk;
        Frame.Direct = D;
        Frame.PathValid = PathOk;
        if (PathOk)
        {
            for (int B = 0; B < IPL_NUM_BANDS; ++B) { Frame.PathEQ[B] = Outputs.pathing.eqCoeffs[B]; }
            for (int C = 0; C < IM_AcousticAudioFrame::Coefficients; ++C)
            {
                Frame.PathSH[C] = Outputs.pathing.shCoeffs[C];
            }
        }
        IPLVector3 Rel = iplCalculateRelativeDirection(Context, Input[I].Source.origin,
            Listener.origin, Listener.ahead, Listener.up);
        if (!IMSimulationFinite(Rel)) { Rel = IPLVector3{0.0f, 0.0f, -1.0f}; } // degenerate input; render hint only.
        Frame.ListenerLocalDirection = Rel;
        Frame.Listener = Listener;

        Recs[I]->Sequence = Frame.Sequence;
        Tmp.push_back(Frame);
    }
    Output.swap(Tmp);
    return true;
}

bool IM_AcousticSimulation::Evaluate(std::uint64_t SourceKey, std::uint64_t Generation,
    const IPLCoordinateSpace3& Source, const IPLCoordinateSpace3& Listener,
    IM_AcousticAudioFrame& OutFrame, std::string& OutError)
{
    // Single-source test convenience over the batch path; the batch never calls back here.
    OutFrame = IM_AcousticAudioFrame{};
    IM_AcousticSourceInput Item{SourceKey, Generation, Source};
    const std::vector<IM_AcousticSourceInput> BatchInput(1, Item);
    std::vector<IM_AcousticAudioFrame> BatchOutput;
    if (!EvaluateBatch(BatchInput, Listener, BatchOutput, OutError)) { return false; }
    OutFrame = BatchOutput[0];
    return true;
}

bool IM_AcousticSimulation::EvaluateReverb(IM_AcousticReverbSlot& Slot,
    const IPLCoordinateSpace3& Listener,std::string& OutError)
{
    OutError.clear();
    if(!Loaded||!Simulator||Slot.State.load(std::memory_order_acquire)!=IM_AcousticIRState::Writing
        ||!IMSimulationFinite(Listener.origin)||!IMSimulationFinite(Listener.ahead)
        ||!IMSimulationFinite(Listener.up)||!IMSimulationFinite(Listener.right))
    {OutError="Reverb: invalid simulator, listener or slot ownership.";return false;}
    IPLBakedDataIdentifier Id{};Id.type=IPL_BAKEDDATATYPE_REFLECTIONS;Id.variation=IPL_BAKEDDATAVARIATION_REVERB;
    if(iplProbeBatchGetDataSize(ProbeBatch,&Id)==0)
    {OutError="Reverb: canonical baked convolution layer missing.";return false;}
    if(CoverageProbes.empty()||CoverageProbes.size()!=size_t(iplProbeBatchGetNumProbes(ProbeBatch)))
    {OutError="Reverb: validated probe coverage is missing or inconsistent.";return false;}
    // Keep every SDK float squared-distance intermediate below FLT_MAX:
    // three squared differences of coordinates bounded by M use at most
    // 12*M*M, with 64 providing headroom for rounding. Finite alone is weaker.
    const double MaxCoverageCoordinate=std::sqrt(double(std::numeric_limits<float>::max())/64);
    const auto SafeCoveragePoint=[MaxCoverageCoordinate](const IPLVector3& P)
    {return IMSimulationFinite(P)&&std::abs(double(P.x))<=MaxCoverageCoordinate&&std::abs(double(P.y))<=MaxCoverageCoordinate&&std::abs(double(P.z))<=MaxCoverageCoordinate;};
    if(!SafeCoveragePoint(Listener.origin))
    {OutError="Reverb: listener exceeds safe SDK coverage arithmetic range.";return false;}
    // A Ready slot may have been discarded during bypass/newer-result selection
    // without SDK Apply. Reusing that source would leave its old pending IR in
    // the TripleBuffer and silently reject the new commit. Writing owns it;
    // it is detached from the simulator and no audio reader can still use it.
    if(Slot.Source&&!Slot.Applied){iplSourceRelease(&Slot.Source);Slot.Params={};}
    Slot.Applied=false;
    if(!Slot.Source)
    {
        IPLSourceSettings Settings{};Settings.flags=static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT|IPL_SIMULATIONFLAGS_REFLECTIONS);
        if(iplSourceCreate(Simulator,&Settings,&Slot.Source)!=IPL_STATUS_SUCCESS||!Slot.Source)
        {OutError="Reverb: source creation failed.";return false;}
    }
    // SDK 4.8.1 ProbeTree selects at most eight overlapping spheres, then
    // rejects occluded probes (probe_batch.h / probe_manager.cpp). The C API
    // does not expose that selected set. Certify coverage without reimplementing
    // its tree: when fewer than eight overlaps are blocked, any eight selected
    // overlaps must include a visible probe. Ambiguous dense coverage fails
    // closed rather than refreshing a potentially stale SDK energy field.
    constexpr int MaxSelectedProbes=8;
    int Overlaps=0,Blocked=0;
    IPLSimulationSharedInputs VisibilityShared{};VisibilityShared.listener=Listener;
    iplSimulatorSetSharedInputs(Simulator,IPL_SIMULATIONFLAGS_DIRECT,&VisibilityShared);
    iplSourceAdd(Slot.Source,Simulator);iplSimulatorCommit(Simulator);
    for(const auto& Probe:CoverageProbes)
    {
        if(!SafeCoveragePoint(Probe.center)||!IMSimulationFinite(Probe.radius)||Probe.radius<=0||Probe.radius>MaxCoverageCoordinate)
        {Blocked=MaxSelectedProbes;break;}
        const double X=double(Listener.origin.x)-Probe.center.x,Y=double(Listener.origin.y)-Probe.center.y,Z=double(Listener.origin.z)-Probe.center.z;
        const double Distance2=X*X+Y*Y+Z*Z,Radius2=double(Probe.radius)*Probe.radius;
        // SDK containment uses float subtract/multiply/add and float AABBs.
        // Bound rounding rather than silently omitting boundary candidates:
        // 16 eps exceeds gamma_8 for those operations (including squared
        // subtraction errors); min-normal also covers flush-to-zero behavior.
        constexpr double Eps=std::numeric_limits<float>::epsilon();
        const double ErrorBound=16*Eps*std::max(Distance2,Radius2)+16*std::numeric_limits<float>::min();
        if(Distance2-Radius2>ErrorBound)continue;
        ++Overlaps;
        bool Uncertain=Radius2-Distance2<=ErrorBound;
        const double Positions[]{Listener.origin.x,Listener.origin.y,Listener.origin.z};
        const double Centers[]{Probe.center.x,Probe.center.y,Probe.center.z};
        for(int Axis=0;Axis<3;++Axis)
        {
            const double BoxError=2*Eps*(std::abs(Centers[Axis])+Probe.radius)+2*std::numeric_limits<float>::min();
            if(Positions[Axis]-(Centers[Axis]-Probe.radius)<=BoxError||(Centers[Axis]+Probe.radius)-Positions[Axis]<=BoxError)Uncertain=true;
        }
        // Counting uncertainty as blocked is a conservative upper bound even
        // if SDK omits it. It can never supply the required visible witness.
        if(Uncertain){if(++Blocked>=MaxSelectedProbes)break;continue;}
        IPLSimulationInputs Visibility{};Visibility.flags=IPL_SIMULATIONFLAGS_DIRECT;
        Visibility.directFlags=IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
        Visibility.source=Listener;Visibility.source.origin=Probe.center;
        Visibility.occlusionType=IPL_OCCLUSIONTYPE_RAYCAST;Visibility.numOcclusionSamples=1;
        iplSourceSetInputs(Slot.Source,IPL_SIMULATIONFLAGS_DIRECT,&Visibility);
        iplSimulatorRunDirect(Simulator);
        IPLSimulationOutputs Result{};iplSourceGetOutputs(Slot.Source,IPL_SIMULATIONFLAGS_DIRECT,&Result);
        if(Result.direct.occlusion!=1.f)++Blocked;
        if(Blocked>=MaxSelectedProbes)break;
    }
    iplSourceRemove(Slot.Source,Simulator);iplSimulatorCommit(Simulator);
    if(Overlaps==0||Blocked==Overlaps||Blocked>=MaxSelectedProbes)
    {OutError="Reverb: no certified visible probe coverage at listener.";return false;}
    IPLSimulationInputs Inputs{};Inputs.flags=IPL_SIMULATIONFLAGS_REFLECTIONS;
    Inputs.source=Listener;Inputs.baked=IPL_TRUE;Inputs.bakedDataIdentifier=Id;
    Inputs.reverbScale[0]=Inputs.reverbScale[1]=Inputs.reverbScale[2]=1;
    Inputs.distanceAttenuationModel.type=IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
    Inputs.airAbsorptionModel.type=IPL_AIRABSORPTIONTYPE_DEFAULT;
    Inputs.directivity.dipolePower=1;
    IPLSimulationSharedInputs Shared{};Shared.listener=Listener;
    Shared.numRays=1;Shared.numBounces=1;Shared.duration=IM_AcousticRecipe::ReverbSavedDurationS;
    Shared.order=IM_AcousticAudioFrame::Order;Shared.irradianceMinDistance=IM_AcousticRecipe::IrradianceMinM;
    iplSourceSetInputs(Slot.Source,IPL_SIMULATIONFLAGS_REFLECTIONS,&Inputs);
    iplSimulatorSetSharedInputs(Simulator,IPL_SIMULATIONFLAGS_REFLECTIONS,&Shared);
    iplSourceAdd(Slot.Source,Simulator);iplSimulatorCommit(Simulator);
    iplSimulatorRunReflections(Simulator);
    IPLSimulationOutputs Outputs{};
    iplSourceGetOutputs(Slot.Source,IPL_SIMULATIONFLAGS_REFLECTIONS,&Outputs);
    // Each 4.8.1 source owns its TripleBuffer<OverlapSaveFIR>. Removing and
    // committing excludes it from all later simulation writes. Audio may swap
    // its read buffer, hence a slot has exactly one consumer until returned Free.
    iplSourceRemove(Slot.Source,Simulator);iplSimulatorCommit(Simulator);
    Outputs.reflections.type=IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    if(!Outputs.reflections.ir||Outputs.reflections.numChannels!=IM_AcousticAudioFrame::Coefficients
        ||Outputs.reflections.irSize!=int(SampleRateHz*IM_AcousticRecipe::ReverbSavedDurationS))
    {OutError="Reverb: invalid convolution output dimensions.";return false;}
    Slot.Params=Outputs.reflections;Slot.Listener=Listener;
    return true;
}

void IM_AcousticSimulation::Remove(std::uint64_t SourceKey)
{
    const auto It = Sources.find(SourceKey);
    if (It == Sources.end()) { return; }
    if (It->second.Source != nullptr)
    {
        if (Simulator != nullptr) { iplSourceRemove(It->second.Source, Simulator); }
        iplSourceRelease(&It->second.Source);
    }
    Sources.erase(It);
}

void IM_AcousticSimulation::Shutdown()
{
    for (auto& Entry : Sources)
    {
        if (Entry.second.Source != nullptr)
        {
            if (Simulator != nullptr) { iplSourceRemove(Entry.second.Source, Simulator); }
            iplSourceRelease(&Entry.second.Source);
        }
    }
    Sources.clear();
    for (auto& Entry : DynamicMeshes)
    {
        auto& Record = Entry.second;
        if (Record.Instance != nullptr)
        {
            if (Scene != nullptr) { iplInstancedMeshRemove(Record.Instance, Scene); }
            iplInstancedMeshRelease(&Record.Instance);
        }
        if (Record.Mesh != nullptr) { iplStaticMeshRelease(&Record.Mesh); }
        if (Record.SubScene != nullptr) { iplSceneRelease(&Record.SubScene); }
    }
    DynamicMeshes.clear();
    if (Simulator != nullptr) { iplSimulatorRelease(&Simulator); }
    if (ProbeBatch != nullptr) { iplProbeBatchRelease(&ProbeBatch); }
    if (Scene != nullptr) { iplSceneRelease(&Scene); }
    if (Context != nullptr) { iplContextRelease(&Context); }
    SampleRateHz = 0;
    BlockFrames = 0;
    CoverageProbes.clear();
    Loaded = false;
}
