#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticMetaSound.h"
#include "MetasoundSource.h"
#include "Sound/AudioBus.h"
#include "AudioBusSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "IMAcousticSimulationWorker.h"
#include "IMAcousticReverbPreset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticCoordinates.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Sound/SoundSubmix.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Brush.h"
#include "GameFramework/PhysicsVolume.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Materials/MaterialInterface.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#if WITH_EDITOR
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#endif

namespace
{
std::atomic<uint64> IMNextWorldEpoch{1};
struct IM_BakeJob
{
    std::atomic<bool> Cancelled{false};
    std::atomic<bool> Done{false};
    bool Success=false;
    IM_AcousticBakeData Data;
    std::string Error;
    FString Fingerprint;
    FString WorldPackage;
    FString MetadataJson;
    TFuture<void> Future;
};
FString IMBakeMetadata(const AIMAcousticBakeVolume& Volume,const IM_AcousticSceneInput& Geometry,const FVector& Origin)
{
    FString Json;auto W=TJsonWriterFactory<>::Create(&Json);
    W->WriteObjectStart();W->WriteValue(TEXT("recipe_version"),IM_AcousticRecipe::Version);
    W->WriteValue(TEXT("sdk_version"),UIMAcousticBakeAsset::SDKVersion);W->WriteValue(TEXT("triangles"),int32(Geometry.Triangles.size()));
    W->WriteValue(TEXT("probe_count"),int32(Geometry.Probes.size()));
    W->WriteValue(TEXT("probe_spacing_cm"),Volume.ProbeSpacingCm);W->WriteValue(TEXT("probe_height_cm"),Volume.ProbeHeightCm);
    auto Vector=[&W](const TCHAR* Name,const FVector& V){W->WriteArrayStart(Name);for(int I=0;I<3;++I)W->WriteValue(V[I]);W->WriteArrayEnd();};
    Vector(TEXT("bounds_origin_cm"),Origin);Vector(TEXT("bounds_extent_cm"),Volume.BakeBounds->GetUnscaledBoxExtent());
    W->WriteArrayStart(TEXT("materials"));
    for(const auto& M:Volume.Materials)
    {
        W->WriteObjectStart();W->WriteValue(TEXT("asset_path"),M.Material->GetPathName());
        Vector(TEXT("absorption"),M.Absorption);W->WriteValue(TEXT("scattering"),M.Scattering);W->WriteObjectEnd();
    }
    W->WriteArrayEnd();W->WriteArrayStart(TEXT("probes_m"));
    for(const auto& P:Geometry.Probes)
    {W->WriteArrayStart();W->WriteValue(P.center.x);W->WriteValue(P.center.y);W->WriteValue(P.center.z);W->WriteValue(P.radius);W->WriteArrayEnd();}
    W->WriteArrayEnd();W->WriteObjectStart(TEXT("reflection"));
    W->WriteValue(TEXT("num_rays"),IM_AcousticRecipe::ReverbNumRays);W->WriteValue(TEXT("num_bounces"),IM_AcousticRecipe::ReverbNumBounces);
    W->WriteValue(TEXT("num_diffuse_samples"),IM_AcousticRecipe::ReverbNumDiffuse);
    W->WriteValue(TEXT("irradiance_min_m"),IM_AcousticRecipe::IrradianceMinM);
    W->WriteValue(TEXT("sim_duration_s"),IM_AcousticRecipe::ReverbSimDurationS);W->WriteValue(TEXT("saved_duration_s"),IM_AcousticRecipe::ReverbSavedDurationS);
    W->WriteValue(TEXT("order"),IM_AcousticAudioFrame::Order);W->WriteValue(TEXT("type"),TEXT("CONVOLUTION"));W->WriteObjectEnd();
    W->WriteObjectStart(TEXT("path"));W->WriteValue(TEXT("num_samples"),IM_AcousticRecipe::PathNumSamples);
    W->WriteValue(TEXT("radius_m"),IM_AcousticRecipe::PathRadiusM);W->WriteValue(TEXT("threshold"),IM_AcousticRecipe::PathThreshold);
    W->WriteValue(TEXT("vis_range_m"),IM_AcousticRecipe::PathVisRangeM);W->WriteValue(TEXT("path_range_m"),IM_AcousticRecipe::PathRangeM);
    W->WriteObjectEnd();W->WriteObjectEnd();W->Close();return Json;
}

// A bake-bound listener is not automatically an acoustically valid listener:
// the editor/PIE camera can be placed inside a closed wall slab while still
// remaining inside BakeBounds. In that state the SDK's nearest-probe lookup can
// select a probe on either side of the slab and produce a large, discontinuous
// reverb change. Test the exported acoustic triangles themselves so this guard
// follows the same geometry contract as the bake.
static bool IMPointInsideAcousticTriangleRange(const IM_AcousticSceneInput& Geometry, int32 StartTriangle, int32 EndTriangle, const IPLVector3& Point)
{
    const int32 TriangleCount = static_cast<int32>(Geometry.Triangles.size());
    const int32 Start = FMath::Clamp(StartTriangle, 0, TriangleCount);
    const int32 End = FMath::Clamp(EndTriangle, Start, TriangleCount);
    if (End <= Start) return false;

    const FVector P(Point.x, Point.y, Point.z);
    const FVector Directions[] = {
        FVector(1.0f, 0.371f, 0.173f).GetSafeNormal(),
        FVector(0.271f, 1.0f, 0.133f).GetSafeNormal(),
        FVector(0.163f, 0.239f, 1.0f).GetSafeNormal()};
    int32 InsideVotes = 0;
    for (const FVector& Direction : Directions)
    {
        int32 Hits = 0;
        for (int32 TriangleIndex = Start; TriangleIndex < End; ++TriangleIndex)
        {
            const IPLTriangle& Triangle = Geometry.Triangles[TriangleIndex];
            const int32 IA = Triangle.indices[0];
            const int32 IB = Triangle.indices[1];
            const int32 IC = Triangle.indices[2];
            if (IA < 0 || IB < 0 || IC < 0
                || size_t(IA) >= Geometry.Vertices.size()
                || size_t(IB) >= Geometry.Vertices.size()
                || size_t(IC) >= Geometry.Vertices.size())
            {
                continue;
            }
            const FVector A(Geometry.Vertices[IA].x, Geometry.Vertices[IA].y, Geometry.Vertices[IA].z);
            const FVector B(Geometry.Vertices[IB].x, Geometry.Vertices[IB].y, Geometry.Vertices[IB].z);
            const FVector C(Geometry.Vertices[IC].x, Geometry.Vertices[IC].y, Geometry.Vertices[IC].z);
            const FVector E1 = B - A;
            const FVector E2 = C - A;
            const FVector H = FVector::CrossProduct(Direction, E2);
            const double Det = FVector::DotProduct(E1, H);
            if (FMath::Abs(Det) < 1.e-9) continue;
            const double InvDet = 1.0 / Det;
            const FVector S = P - A;
            const double U = InvDet * FVector::DotProduct(S, H);
            if (U <= 1.e-7 || U >= 1.0 - 1.e-7) continue;
            const FVector Q = FVector::CrossProduct(S, E1);
            const double V = InvDet * FVector::DotProduct(Direction, Q);
            if (V <= 1.e-7 || U + V >= 1.0 - 1.e-7) continue;
            const double T = InvDet * FVector::DotProduct(E2, Q);
            if (T > 1.e-7) ++Hits;
        }
        if ((Hits & 1) != 0) ++InsideVotes;
    }
    return InsideVotes >= 2;
}

static bool IMPointInsideAcousticGeometry(const IM_AcousticSceneInput& Geometry, const TArray<TPair<int32, int32>>& TriangleRanges, const IPLVector3& Point)
{
    // Test each exported static-mesh instance independently. A whole-scene
    // parity pass is not valid when separate walls, floors, or open doorways
    // cancel one another's ray crossings. A hollow box remains hollow because
    // its own triangle range is tested as one topology, while the solid wall
    // shell around that opening still rejects a listener placed in the shell.
    for (const TPair<int32, int32>& Range : TriangleRanges)
    {
        if (IMPointInsideAcousticTriangleRange(Geometry, Range.Key, Range.Value, Point)) return true;
    }
    return false;
}

IPLMatrix4x4 IMToSDKDynamicTransform(const FTransform& Transform, const FVector& Origin)
{
    IPLMatrix4x4 Matrix{};
    // Instance visibility (validation raycasts) treats the instance matrix as
    // rigid: non-uniform component scale must live in the sub-scene vertices
    // (see IMBuildDynamicMeshSnapshot), never in this basis. Translation stays
    // in meters in column 3 (C API row-major Xeon order).
    const FQuat Rigid = Transform.GetRotation();
    const FVector UEAxes[3] = { FVector::RightVector, FVector::UpVector, -FVector::ForwardVector };
    for (int32 Column = 0; Column < 3; ++Column)
    {
        const IPLVector3 Axis = IMToSDKDirection(Rigid.RotateVector(UEAxes[Column]));
        Matrix.elements[0][Column] = Axis.x;
        Matrix.elements[1][Column] = Axis.y;
        Matrix.elements[2][Column] = Axis.z;
    }
    const IPLVector3 Translation = IMToSDKPosition(Transform.GetLocation(), Origin);
    Matrix.elements[0][3] = Translation.x;
    Matrix.elements[1][3] = Translation.y;
    Matrix.elements[2][3] = Translation.z;
    Matrix.elements[3][3] = 1.0f;
    return Matrix;
}

// Coplanar-triangle merge for dynamic snapshots: Steam Audio pathing conducts
// through internally-subdivided panels (a 2x2-split box face leaks bit-identical
// energy to no door at all, while the same extents as clean tris seal; evidence
// inc112/inc117/inc118), so each coplanar connected patch must reach the SDK as
// a minimal triangulation with no interior edges. Operates in place; any ambiguity
// keeps the original triangles (fail-safe: coverage never shrinks). GT context
// only (allocates); the static bake path is untouched (static walls seal today).
struct IMPlaneKey
{
    int32 Axis = 0;
    FIntVector Nq = FIntVector::ZeroValue;
    int32 Dq = 0;
    int32 Mat = 0;
    bool operator==(const IMPlaneKey& O) const { return Axis == O.Axis && Nq == O.Nq && Dq == O.Dq && Mat == O.Mat; }
};
static uint32 GetTypeHash(const IMPlaneKey& K)
{
    uint32 H = ::GetTypeHash(K.Axis);
    H = HashCombine(H, ::GetTypeHash(K.Nq.X));
    H = HashCombine(H, ::GetTypeHash(K.Nq.Y));
    H = HashCombine(H, ::GetTypeHash(K.Nq.Z));
    H = HashCombine(H, ::GetTypeHash(K.Mat));
    return HashCombine(H, ::GetTypeHash(K.Dq));
}
static double IMPolyArea2(const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
    return (double(B.X) - A.X) * (C.Y - A.Y) - (double(B.Y) - A.Y) * (C.X - A.X);
}
static bool IMPointStrictlyInTri(const FVector2D& Q, const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
    const double S1 = IMPolyArea2(Q, A, B);
    const double S2 = IMPolyArea2(Q, B, C);
    const double S3 = IMPolyArea2(Q, C, A);
    return S1 > 1e-9 && S2 > 1e-9 && S3 > 1e-9;
}
static bool IMEarClip(const TArray<FVector2D>& P, TArray<int32>& OutFlat)
{
    TArray<int32> V;
    for (int32 I = 0; I < P.Num(); ++I) { V.Add(I); }
    int32 Guard = V.Num() * V.Num() + 1;
    while (V.Num() > 3 && Guard-- > 0)
    {
        bool bClipped = false;
        for (int32 I = 0; I < V.Num(); ++I)
        {
            const int32 Ip = (I + V.Num() - 1) % V.Num();
            const int32 In = (I + 1) % V.Num();
            const FVector2D& A = P[V[Ip]];
            const FVector2D& B = P[V[I]];
            const FVector2D& C = P[V[In]];
            if (IMPolyArea2(A, B, C) <= 1e-9) { continue; }
            bool bInside = false;
            for (int32 J = 0; J < V.Num(); ++J)
            {
                if (J == I || J == Ip || J == In) { continue; }
                if (IMPointStrictlyInTri(P[V[J]], A, B, C)) { bInside = true; break; }
            }
            if (bInside) { continue; }
            OutFlat.Add(V[Ip]); OutFlat.Add(V[I]); OutFlat.Add(V[In]);
            V.RemoveAt(I); bClipped = true; break;
        }
        if (!bClipped) { return false; }
    }
    if (V.Num() != 3) { return false; }
    OutFlat.Add(V[0]); OutFlat.Add(V[1]); OutFlat.Add(V[2]);
    return true;
}
static void IMMergeCoplanarDynamicTris(TArray<IPLVector3>& Verts, TArray<IPLTriangle>& Tris, TArray<int32>& MatIds)
{
    const int32 TriCount = Tris.Num();
    if (TriCount < 2 || MatIds.Num() != TriCount || Verts.Num() <= 0) { return; }
    for (int32 I = 0; I < TriCount; ++I)
    {
        for (int32 K = 0; K < 3; ++K)
        {
            if (Tris[I].indices[K] < 0 || Tris[I].indices[K] >= Verts.Num()) { return; }
        }
    }
    TMap<FVector, int32> WeldId;
    TArray<IPLVector3> Welded;
    TArray<int32> Remap;
    Remap.SetNum(Verts.Num());
    for (int32 I = 0; I < Verts.Num(); ++I)
    {
        const FVector P(Verts[I].x, Verts[I].y, Verts[I].z);
        if (const int32* Found = WeldId.Find(P)) { Remap[I] = *Found; }
        else { Remap[I] = Welded.Num(); WeldId.Add(P, Welded.Num()); Welded.Add(Verts[I]); }
    }
    struct IMWTri { int32 V[3]; int32 Mat; FVector N; float D; };
    TArray<IMWTri> Work;
    Work.Reserve(TriCount);
    for (int32 I = 0; I < TriCount; ++I)
    {
        const int32 A = Remap[Tris[I].indices[0]];
        const int32 B = Remap[Tris[I].indices[1]];
        const int32 C = Remap[Tris[I].indices[2]];
        if (A == B || B == C || C == A) { continue; }
        const FVector VA(Welded[A].x, Welded[A].y, Welded[A].z);
        const FVector VB(Welded[B].x, Welded[B].y, Welded[B].z);
        const FVector VC(Welded[C].x, Welded[C].y, Welded[C].z);
        const FVector N = FVector::CrossProduct(VB - VA, VC - VA);
        if (N.SizeSquared() < 1e-12) { continue; }
        IMWTri T; T.V[0] = A; T.V[1] = B; T.V[2] = C; T.Mat = MatIds[I];
        T.N = N.GetSafeNormal(); T.D = -FVector::DotProduct(T.N, VA);
        Work.Add(T);
    }
    if (Work.Num() < 1) { return; }
    TMap<IMPlaneKey, TArray<int32>> Groups;
    for (int32 I = 0; I < Work.Num(); ++I)
    {
        const FVector& N = Work[I].N;
        const float AX = FMath::Abs(N.X), AY = FMath::Abs(N.Y), AZ = FMath::Abs(N.Z);
        IMPlaneKey K;
        K.Axis = (AX >= AY && AX >= AZ) ? 0 : ((AY >= AZ) ? 1 : 2);
        K.Nq = FIntVector(FMath::RoundToInt(N.X * 4096.0f), FMath::RoundToInt(N.Y * 4096.0f), FMath::RoundToInt(N.Z * 4096.0f));
        K.Dq = FMath::RoundToInt(Work[I].D * 10000.0f);
        K.Mat = Work[I].Mat;
        Groups.FindOrAdd(K).Add(I);
    }
    TArray<IPLTriangle> NewTris;
    TArray<int32> NewMats;
    NewTris.Reserve(TriCount);
    NewMats.Reserve(TriCount);
    auto EmitOriginal = [&](int32 WI)
    {
        IPLTriangle T{};
        T.indices[0] = Work[WI].V[0]; T.indices[1] = Work[WI].V[1]; T.indices[2] = Work[WI].V[2];
        NewTris.Add(T); NewMats.Add(Work[WI].Mat);
    };
    TArray<int32> VisitMark;
    VisitMark.SetNumZeroed(Work.Num());
    int32 VisitGen = 1;
    for (auto& KV : Groups)
    {
        const TArray<int32>& Members = KV.Value;
        TMap<int64, TArray<int32>> EdgeTris;
        for (int32 WI : Members)
        {
            for (int32 E = 0; E < 3; ++E)
            {
                const int32 U = Work[WI].V[E], W = Work[WI].V[(E + 1) % 3];
                const int64 Key = ((int64)FMath::Min(U, W) << 32) | (uint32)FMath::Max(U, W);
                EdgeTris.FindOrAdd(Key).Add(WI);
            }
        }
        for (int32 Seed : Members)
        {
            if (VisitMark[Seed] == VisitGen) { continue; }
            TArray<int32> Comp;
            TArray<int32> Stack; Stack.Add(Seed); VisitMark[Seed] = VisitGen;
            while (Stack.Num() > 0)
            {
                const int32 Cur = Stack.Pop();
                Comp.Add(Cur);
                for (int32 E = 0; E < 3; ++E)
                {
                    const int32 U = Work[Cur].V[E], W = Work[Cur].V[(E + 1) % 3];
                    const int64 Key = ((int64)FMath::Min(U, W) << 32) | (uint32)FMath::Max(U, W);
                    if (const TArray<int32>* Adj = EdgeTris.Find(Key))
                    {
                        for (int32 NX : *Adj)
                        {
                            if (VisitMark[NX] != VisitGen) { VisitMark[NX] = VisitGen; Stack.Add(NX); }
                        }
                    }
                }
            }
            if (Comp.Num() < 2) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
            TMap<int64, int32> Use;
            for (int32 WI : Comp)
            {
                for (int32 E = 0; E < 3; ++E)
                {
                    const int32 U = Work[WI].V[E], W = Work[WI].V[(E + 1) % 3];
                    const int64 Key = ((int64)FMath::Min(U, W) << 32) | (uint32)FMath::Max(U, W);
                    Use.FindOrAdd(Key)++;
                }
            }
            TMap<int32, TArray<int32>> Adj;
            bool bManifold = true;
            for (auto& EK : Use)
            {
                if (EK.Value != 1) { continue; }
                const int32 U = int32(EK.Key >> 32), W = int32(EK.Key & 0xFFFFFFFF);
                Adj.FindOrAdd(U).Add(W); Adj.FindOrAdd(W).Add(U);
            }
            for (auto& AK : Adj) { if (AK.Value.Num() != 2) { bManifold = false; break; } }
            if (!bManifold || Adj.Num() == 0) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
            TArray<int32> Loop;
            int32 Start = INT32_MAX;
            for (auto& AK : Adj) { Start = FMath::Min(Start, AK.Key); }
            int32 Prev = -1, Cur = Start;
            for (int32 Step = 0; Step <= Adj.Num(); ++Step)
            {
                Loop.Add(Cur);
                const TArray<int32>& NB = Adj.FindChecked(Cur);
                const int32 Next = (NB[0] != Prev) ? NB[0] : NB[1];
                Prev = Cur; Cur = Next;
                if (Cur == Start) { break; }
            }
            if (Cur != Start || Loop.Num() < 3) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
            for (int32 Pass = 0; Pass < 4 && Loop.Num() > 3; ++Pass)
            {
                bool bDropped = false;
                for (int32 I = 0; I < Loop.Num(); ++I)
                {
                    const int32 Ip = (I + Loop.Num() - 1) % Loop.Num();
                    const int32 In = (I + 1) % Loop.Num();
                    const FVector A(Welded[Loop[Ip]].x, Welded[Loop[Ip]].y, Welded[Loop[Ip]].z);
                    const FVector B(Welded[Loop[I]].x, Welded[Loop[I]].y, Welded[Loop[I]].z);
                    const FVector C(Welded[Loop[In]].x, Welded[Loop[In]].y, Welded[Loop[In]].z);
                    const FVector AB = B - A, BC = C - B;
                    const float L1 = AB.Size(), L2 = BC.Size();
                    if (L1 < 1e-9 || L2 < 1e-9) { continue; }
                    const float SinA = FVector::CrossProduct(AB, BC).Size() / (L1 * L2);
                    const float Dist = FVector::CrossProduct(B - A, C - A).Size() / (C - A).Size();
                    if (SinA < 1e-6 && Dist < 1e-6 && FVector::DotProduct(AB, BC) > 0.0f)
                    {
                        Loop.RemoveAt(I); bDropped = true; break;
                    }
                }
                if (!bDropped) { break; }
            }
            if (Loop.Num() < 3) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
            const int32 Axis = KV.Key.Axis;
            TArray<FVector2D> Pts;
            Pts.Reserve(Loop.Num());
            for (int32 VI : Loop)
            {
                const IPLVector3& P = Welded[VI];
                Pts.Add(Axis == 0 ? FVector2D(P.y, P.z) : (Axis == 1 ? FVector2D(P.x, P.z) : FVector2D(P.x, P.y)));
            }
            double LoopArea = 0.0;
            for (int32 I = 0; I < Pts.Num(); ++I) { LoopArea += IMPolyArea2(Pts[I], Pts[(I + 1) % Pts.Num()], Pts[(I + 2) % Pts.Num()]); }
            if (FMath::Abs(LoopArea) < 1e-9) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
            if (LoopArea < 0.0)
            {
                for (int32 LI = 0, LJ = Loop.Num() - 1; LI < LJ; ++LI, --LJ) { Loop.Swap(LI, LJ); }
                Pts.Reset();
                for (int32 VI : Loop)
                {
                    const IPLVector3& P = Welded[VI];
                    Pts.Add(Axis == 0 ? FVector2D(P.y, P.z) : (Axis == 1 ? FVector2D(P.x, P.z) : FVector2D(P.x, P.y)));
                }
            }
            TArray<int32> Flat;
            if (!IMEarClip(Pts, Flat)) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
            const int32 RefA = Work[Comp[0]].V[0], RefB = Work[Comp[0]].V[1], RefC = Work[Comp[0]].V[2];
            auto ProjOf = [&](int32 VI) -> FVector2D
            {
                const IPLVector3& P = Welded[VI];
                return Axis == 0 ? FVector2D(P.y, P.z) : (Axis == 1 ? FVector2D(P.x, P.z) : FVector2D(P.x, P.y));
            };
            const double RefSign = IMPolyArea2(ProjOf(RefA), ProjOf(RefB), ProjOf(RefC));
            const double OutSign = IMPolyArea2(Pts[Flat[0]], Pts[Flat[1]], Pts[Flat[2]]);
            const bool bSwap = (RefSign < 0.0) != (OutSign < 0.0);
            for (int32 I = 0; I + 2 < Flat.Num(); I += 3)
            {
                IPLTriangle T{};
                T.indices[0] = Loop[Flat[I]];
                T.indices[1] = Loop[bSwap ? Flat[I + 2] : Flat[I + 1]];
                T.indices[2] = Loop[bSwap ? Flat[I + 1] : Flat[I + 2]];
                NewTris.Add(T); NewMats.Add(Work[Comp[0]].Mat);
            }
        }
        ++VisitGen;
    }
    if (NewTris.Num() < 1) { return; }
    Verts = MoveTemp(Welded);
    Tris = MoveTemp(NewTris);
    MatIds = MoveTemp(NewMats);
}
bool IMBuildDynamicMeshSnapshot(UStaticMeshComponent* Component, int32 InstanceIndex,
    const FVector& Origin, const TMap<UMaterialInterface*, int32>& MaterialIndices,
    const TArray<IPLMaterial>& Materials, IM_AcousticDynamicMeshSnapshot& Out, FString& Failure)
{
    if (!IsValid(Component) || !Component->GetStaticMesh() || !Component->GetStaticMesh()->GetRenderData()
        || Component->GetStaticMesh()->GetRenderData()->LODResources.IsEmpty())
    {
        Failure = TEXT("Dynamic geometry has no CPU mesh data.");
        return false;
    }
    UStaticMesh* Mesh = Component->GetStaticMesh();
    const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
    const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
    const auto IndexView = LOD.IndexBuffer.GetArrayView();
    if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData() || IndexView.Num() == 0)
    {
        Failure = FString::Printf(TEXT("Dynamic mesh CPU geometry unavailable: %s"), *Mesh->GetPathName());
        return false;
    }
    FTransform Transform = Component->GetComponentTransform();
    if (UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Component))
    {
        if (!Instances->GetInstanceTransform(InstanceIndex, Transform, true))
        {
            Failure = TEXT("Dynamic instance transform unavailable.");
            return false;
        }
    }
    if (Transform.ContainsNaN() || FMath::IsNearlyZero(Transform.GetDeterminant()))
    {
        Failure = TEXT("Dynamic instance has invalid or singular transform.");
        return false;
    }
    Out = {};
    Out.Key = (static_cast<uint64>(Component->GetUniqueID()) << 32)
        | static_cast<uint64>(InstanceIndex + 1);
    // World scale lives in the vertices (meters), not in the instance basis:
    // visibility raycasts treat instances as rigid, so a scaled basis would
    // silently shrink the obstacle (W3 closed-door leak). Scale edits change
    // the hash so the simulator recreates instead of reusing stale geometry.
    const FVector MeshScale = Transform.GetScale3D();
    Out.GeometryHash = (static_cast<uint64>(Mesh->GetUniqueID()) << 32)
        ^ static_cast<uint64>(Positions.GetNumVertices())
        ^ (static_cast<uint64>(IndexView.Num()) << 1)
        ^ static_cast<uint64>(Materials.Num())
        ^ static_cast<uint64>(GetTypeHash(MeshScale));
    Out.Transform = IMToSDKDynamicTransform(Transform, Origin);
    Out.Materials.Append(Materials);
    Out.Vertices.Reserve(Positions.GetNumVertices());
    for (uint32 VertexIndex = 0; VertexIndex < Positions.GetNumVertices(); ++VertexIndex)
    {
        const FVector RawVertex(Positions.VertexPosition(VertexIndex));
        const FVector ScaledVertex(RawVertex.X * MeshScale.X, RawVertex.Y * MeshScale.Y, RawVertex.Z * MeshScale.Z);
        Out.Vertices.Add(IMToSDKDirection(ScaledVertex * 0.01f));
    }
    // UE faces wind CW; IMToSDKDirection (Y,Z,-X, det -1) already converts to
    // SDK-CCW-outward, so swap only when the instance itself mirrors (det<0).
    // Swapping det>0 wound the H1 door inward (VOL -0.594) and conducted path
    // audio through the sealed aperture (inc108; falsifiable by VOL sign).
    const bool Flip = Transform.GetDeterminant() < 0.0f;
    for (const FStaticMeshSection& Section : LOD.Sections)
    {
        const int32* MaterialIndex = MaterialIndices.Find(Component->GetMaterial(Section.MaterialIndex));
        if (!MaterialIndex)
        {
            Failure = FString::Printf(TEXT("Dynamic acoustic material missing: %s section %d"),
                *Component->GetPathName(), Section.MaterialIndex);
            return false;
        }
        for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
        {
            IPLTriangle Triangle{};
            for (uint32 Corner = 0; Corner < 3; ++Corner)
            {
                const uint32 Index = LOD.IndexBuffer.GetIndex(Section.FirstIndex + TriangleIndex * 3 + Corner);
                if (Index >= Positions.GetNumVertices())
                {
                    Failure = TEXT("Dynamic mesh index exceeds vertex buffer.");
                    return false;
                }
                Triangle.indices[Corner] = static_cast<IPLint32>(Index);
            }
            if (Flip) { std::swap(Triangle.indices[1], Triangle.indices[2]); }
            Out.Triangles.Add(Triangle);
            Out.MaterialIndices.Add(*MaterialIndex);
        }
    }
    {
        const int32 IMergeInTris = Out.Triangles.Num();
        const int32 IMergeInVerts = Out.Vertices.Num();
        IMMergeCoplanarDynamicTris(Out.Vertices, Out.Triangles, Out.MaterialIndices);
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMergeDynamic in_tris=%d out_tris=%d in_verts=%d out_verts=%d mesh=%s"),
            IMergeInTris, Out.Triangles.Num(), IMergeInVerts, Out.Vertices.Num(), *Mesh->GetPathName());
    }
    if (Out.Triangles.IsEmpty())
    {
        Failure = FString::Printf(TEXT("Dynamic mesh has no supported triangles: %s"), *Mesh->GetPathName());
        return false;
    }
    return true;
}
}

struct IM_AcousticFieldRuntime
{
    IM_AcousticSceneInput Geometry;
    TArray<TPair<int32, int32>> GeometryTriangleRanges;
    IPLProbeGenerationParams ProbeParams{};
    FString Fingerprint;
    FString ProbeFingerprint;
    FVector Origin=FVector::ZeroVector;
    TSharedPtr<IM_BakeJob,ESPMode::ThreadSafe> Job;
    TUniquePtr<IM_AcousticSimulationWorker> Worker;
    TSharedPtr<IM_AcousticDeviceBridge,ESPMode::ThreadSafe> Bridge;
    TSharedPtr<IM_AcousticReverbPool,ESPMode::ThreadSafe> Reverb;
    IM_AcousticMetaSoundContextPtr MetaSoundContext;
    TStrongObjectPtr<UAudioBus> MetaSoundBus;
    TStrongObjectPtr<UAudioComponent> MetaSoundEnvironment;
    TSet<uint64> MetaSoundRegisteredSources;
    bool MetaSoundAudibleReported = false;
    FAudioDeviceHandle DeviceHandle;
    FTSTicker::FDelegateHandle BakeTicker;
    FTSTicker::FDelegateHandle ValidationTicker;
    uint64 Epoch=0;
    double NextSnapshot=0;
    double PreviousSnapshot=0;
    bool SceneValid=false;
    bool ApertureTransitEnabled=false;
    FVector ApertureTransitCenterUE=FVector::ZeroVector;
    FVector ApertureTransitHalfExtentUE=FVector::ZeroVector;
};

AIMAcousticBakeVolume::AIMAcousticBakeVolume()
{
    PrimaryActorTick.bCanEverTick=true;
    BakeBounds=CreateDefaultSubobject<UBoxComponent>(TEXT("BakeBounds"));
    SetRootComponent(BakeBounds);
    BakeBounds->SetBoxExtent(FVector(500,500,250));
    BakeBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Runtime=MakeUnique<IM_AcousticFieldRuntime>();
}
AIMAcousticBakeVolume::~AIMAcousticBakeVolume() = default;

void AIMAcousticBakeVolume::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
#if WITH_EDITOR
    if(!Runtime||Runtime->ValidationTicker.IsValid()||!GetWorld()||GetWorld()->IsGameWorld()||IsTemplate())return;
    // Asset/instance edits and actor insertion can bypass this actor's property
    // callbacks. Re-export in the editor at a bounded cadence so all supported
    // geometry changes invalidate the same fingerprint, even without realtime.
    TWeakObjectPtr<AIMAcousticBakeVolume> WeakOwner(this);
    Runtime->ValidationTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakOwner](float)
    {
        auto* Owner=WeakOwner.Get();if(!Owner||!Owner->GetWorld())return false;
        if(Owner->GetWorld()->IsGameWorld()||(GEditor&&GEditor->PlayWorld)||Owner->Runtime->Job||!Owner->BakedField)return true;
        const FString PreviousStatus=Owner->Status;FString Error;
        if(!Owner->CaptureScene())Owner->Status=TEXT("Bake stale: scene export is incomplete. See Scene Issues.");
        else if(!Owner->BakedField->Validate(Owner->GetWorld()->GetOutermost()->GetName(),Owner->Runtime->Fingerprint,Error))Owner->Status=Error;
        else Owner->Status=PreviousStatus.StartsWith(TEXT("Acoustic bake is stale"))?TEXT("Bake valid for current scene."):PreviousStatus;
        return true;
    }),1.0f);
#endif
}

bool AIMAcousticBakeVolume::CaptureScene()
{
    check(IsInGameThread());
    Runtime->SceneValid=false;
    auto PreviousProbes=MoveTemp(Runtime->Geometry.Probes);
    GeneratedProbes=0;
    Runtime->Geometry={};
    Runtime->GeometryTriangleRanges.Reset();
    SceneIssues.Reset();ExportedTriangles=0;
    if (!GetWorld() || !BakeBounds || !FMath::IsFinite(ProbeSpacingCm) || ProbeSpacingCm<25
        || !FMath::IsFinite(ProbeHeightCm) || ProbeHeightCm<25)
    { Status=TEXT("Invalid world, bounds or probe configuration.");return false; }
    const FTransform BoundsTransform=BakeBounds->GetComponentTransform();
    if(!BoundsTransform.GetRotation().Equals(FQuat::Identity) || !BoundsTransform.GetScale3D().Equals(FVector::OneVector))
    { Status=TEXT("Bake bounds must be axis-aligned with unit scale; edit Box Extent for size.");return false; }
    Runtime->Origin=BoundsTransform.GetLocation();
    const FBox Region=BakeBounds->Bounds.GetBox();
    bool Failed=false;
    TMap<UMaterialInterface*,int32> MaterialIndices;
    for(const auto& Mapping:Materials)
    {
        if(!Mapping.Material || MaterialIndices.Contains(Mapping.Material))
        { SceneIssues.Add(TEXT("Missing or duplicate explicit acoustic material mapping."));Failed=true;continue; }
        IPLMaterial Material{};
        for(int B=0;B<3;++B)
        {
            const double V=Mapping.Absorption[B];
            if(!FMath::IsFinite(V)||V<0||V>1)Failed=true;
            Material.absorption[B]=float(V);Material.transmission[B]=0;
        }
        if(!FMath::IsFinite(Mapping.Scattering)||Mapping.Scattering<0||Mapping.Scattering>1)Failed=true;
        Material.scattering=Mapping.Scattering;
        MaterialIndices.Add(Mapping.Material,static_cast<int32>(Runtime->Geometry.Materials.size()));
        Runtime->Geometry.Materials.push_back(Material);
    }
    TArray<UPrimitiveComponent*> Components;
    for(TActorIterator<AActor> It(GetWorld());It;++It)
    {
        if(*It==this)continue;
        // These world-owned helpers describe editor construction / default physics,
        // not acoustic surfaces. Do not generalize this to authored BSP or volumes.
        if(*It==GetWorld()->GetDefaultBrush() || *It==GetWorld()->GetDefaultPhysicsVolume())
        {
            SceneIssues.Add(FString::Printf(TEXT("Excluded engine world helper: %s"),*It->GetPathName()));
            continue;
        }
        TArray<UPrimitiveComponent*> ActorComponents;It->GetComponents(ActorComponents);
        Components.Append(ActorComponents);
    }
    Components.Sort([](const UPrimitiveComponent& A,const UPrimitiveComponent& B){return A.GetPathName()<B.GetPathName();});
    TMap<FString,int32> IMActorTris;
    TArray<TPair<int32,FString>> IMActorRanges;
    for(UPrimitiveComponent* Primitive:Components)
    {
        if(!IsValid(Primitive)||!Region.Intersect(Primitive->Bounds.GetBox()))continue;
        const bool Required=Primitive->ComponentHasTag(TEXT("IMAcousticRequired"));
#if WITH_EDITORONLY_DATA
        if(Primitive->IsVisualizationComponent()||Primitive->IsEditorOnly())
        {
            SceneIssues.Add(FString::Printf(TEXT("Excluded editor-only component: %s"),*Primitive->GetPathName()));
            if(Required)Failed=true;
            continue;
        }
#endif
        if(Primitive->ComponentHasTag(TEXT("IMAcousticIgnore")))
        {
            SceneIssues.Add(FString::Printf(TEXT("Explicitly excluded: %s"),*Primitive->GetPathName()));
            if(Required)Failed=true;
            continue;
        }
        if(Primitive->Mobility!=EComponentMobility::Static)
        {
            SceneIssues.Add(FString::Printf(TEXT("Not baked (movable): %s"),*Primitive->GetPathName()));
            if(Required)Failed=true;
            continue;
        }
        UStaticMeshComponent* MeshComponent=Cast<UStaticMeshComponent>(Primitive);
        // A static non-mesh component (PIE pawn capsule, sprite, arrow) can
        // never contribute triangles. List and skip it; only an explicit
        // IMAcousticRequired tag still fails. Meshes with missing data keep
        // the hard failure so required geometry can never pass silently.
        if(!MeshComponent)
        { SceneIssues.Add(FString::Printf(TEXT("Excluded non-mesh component: %s"),*Primitive->GetPathName()));if(Required)Failed=true;continue; }
        UStaticMesh* Mesh=MeshComponent->GetStaticMesh();
        if(!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
        { SceneIssues.Add(FString::Printf(TEXT("REQUIRED geometry unsupported: %s"),*Primitive->GetPathName()));Failed=true;continue; }
        const auto& LOD=Mesh->GetRenderData()->LODResources[0];
        const auto& Positions=LOD.VertexBuffers.PositionVertexBuffer;
        if(Positions.GetNumVertices()==0||!Positions.GetVertexData()||LOD.IndexBuffer.GetArrayView().Num()==0)
        { SceneIssues.Add(FString::Printf(TEXT("REQUIRED mesh CPU geometry unavailable: %s"),*Mesh->GetPathName()));Failed=true;continue; }
        UInstancedStaticMeshComponent* Instances=Cast<UInstancedStaticMeshComponent>(MeshComponent);
        const int32 Count=Instances?Instances->GetInstanceCount():1;
        const int32 IMActorStart=static_cast<int32>(Runtime->Geometry.Triangles.size());
        for(int32 Instance=0;Instance<Count;++Instance)
        {
            FTransform Transform=MeshComponent->GetComponentTransform();
            if(Instances&&!Instances->GetInstanceTransform(Instance,Transform,true))
            { SceneIssues.Add(TEXT("REQUIRED instance transform unavailable."));Failed=true;continue; }
            if(Transform.ContainsNaN()||FMath::IsNearlyZero(Transform.GetDeterminant()))
            { SceneIssues.Add(TEXT("REQUIRED instance has invalid/singular transform."));Failed=true;continue; }
            const int32 IMInstanceStart=static_cast<int32>(Runtime->Geometry.Triangles.size());
            const int32 Base=static_cast<int32>(Runtime->Geometry.Vertices.size());
            for(uint32 V=0;V<Positions.GetNumVertices();++V)
                Runtime->Geometry.Vertices.push_back(IMToSDKPosition(Transform.TransformPosition(FVector(Positions.VertexPosition(V))),Runtime->Origin));
            // UE faces wind CW; IMToSDKPosition (Y,Z,-X, det -1) already converts
            // to SDK-CCW-outward, so swap only when the instance itself mirrors
            // (det<0). Swapping det>0 wound static geometry inward and conducted
            // path audio through sealed walls (inc109; wall-ray pv verdict).
            const bool Flip=Transform.GetDeterminant()<0;
            for(const auto& Section:LOD.Sections)
            {
                const int32* Material=MaterialIndices.Find(MeshComponent->GetMaterial(Section.MaterialIndex));
                if(!Material)
                { SceneIssues.Add(FString::Printf(TEXT("REQUIRED acoustic material missing: %s section %d"),*Primitive->GetPathName(),Section.MaterialIndex));Failed=true;continue; }
                for(uint32 T=0;T<Section.NumTriangles;++T)
                {
                    IPLTriangle Triangle{};
                    for(uint32 K=0;K<3;++K)Triangle.indices[K]=Base+int32(LOD.IndexBuffer.GetIndex(Section.FirstIndex+T*3+K));
                    if(Flip)std::swap(Triangle.indices[1],Triangle.indices[2]);
                    Runtime->Geometry.Triangles.push_back(Triangle);Runtime->Geometry.MaterialIndices.push_back(*Material);
                }
            }
            const int32 IMInstanceEnd=static_cast<int32>(Runtime->Geometry.Triangles.size());
            if(IMInstanceEnd>IMInstanceStart)
            {
                Runtime->GeometryTriangleRanges.Add(TPair<int32,int32>(IMInstanceStart,IMInstanceEnd));
                IMActorRanges.Add(TPair<int32,FString>(IMInstanceStart,Primitive->GetPathName()));
            }
            {
                IPLVector3 IMMn{1e30f,1e30f,1e30f},IMMx{-1e30f,-1e30f,-1e30f};
                for(int32 TI=IMInstanceStart;TI<IMInstanceEnd;++TI)
                {
                    const IPLTriangle& IMTr=Runtime->Geometry.Triangles[TI];
                    const int32 IMIdx[3]={IMTr.indices[0],IMTr.indices[1],IMTr.indices[2]};
                    for(int32 KI=0;KI<3;++KI)
                    {
                        const IPLVector3& IMV=Runtime->Geometry.Vertices[IMIdx[KI]];
                        IMMn.x=FMath::Min(IMMn.x,IMV.x);IMMn.y=FMath::Min(IMMn.y,IMV.y);IMMn.z=FMath::Min(IMMn.z,IMV.z);
                        IMMx.x=FMath::Max(IMMx.x,IMV.x);IMMx.y=FMath::Max(IMMx.y,IMV.y);IMMx.z=FMath::Max(IMMx.z,IMV.z);
                    }
                }
                UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeBounds sdkmin=(%.3f,%.3f,%.3f) sdkmax=(%.3f,%.3f,%.3f) %s"),IMMn.x,IMMn.y,IMMn.z,IMMx.x,IMMx.y,IMMx.z,*Primitive->GetPathName());
            }
        }
        IMActorTris.Add(Primitive->GetPathName(),static_cast<int32>(Runtime->Geometry.Triangles.size())-IMActorStart);
    }
    ExportedTriangles=static_cast<int32>(Runtime->Geometry.Triangles.size());
    for(const auto& IMKV:IMActorTris)UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeActor tris=%d %s"),IMKV.Value,*IMKV.Key);
    for(UPrimitiveComponent* IMPrim:Components)if(IMActorTris.Contains(IMPrim->GetPathName()))UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakePlace loc=%s scale=%s mob=%d %s"),*IMPrim->GetComponentLocation().ToString(),*IMPrim->GetComponentScale().ToString(),int(IMPrim->Mobility),*IMPrim->GetPathName());
    {
        // Segment-piercing test: the exact hole-axis segment the sweep measures.
        // Lists baked tris the ray must cross (or total=0 = baked data truly open).
        const FVector IMSegA(-2.0f,0.0f,1.5f),IMSegB(2.0f,0.0f,1.5f);
        int32 IMSegHits=0;
        for(int32 RI=0;RI<IMActorRanges.Num()&&IMSegHits<20;++RI)
        {
            const int32 IMStart=IMActorRanges[RI].Key;
            const int32 IMEnd=(RI+1<IMActorRanges.Num())?IMActorRanges[RI+1].Key:Runtime->Geometry.Triangles.size();
            for(int32 TI=IMStart;TI<IMEnd;++TI)
            {
                const IPLTriangle& IMPT=Runtime->Geometry.Triangles[TI];
                const IPLVector3& PA=Runtime->Geometry.Vertices[IMPT.indices[0]];
                const IPLVector3& PB=Runtime->Geometry.Vertices[IMPT.indices[1]];
                const IPLVector3& PC=Runtime->Geometry.Vertices[IMPT.indices[2]];
                const FVector VA(PA.x,PA.y,PA.z),VB(PB.x,PB.y,PB.z),VC(PC.x,PC.y,PC.z);
                const FVector IMD=IMSegB-IMSegA,IME1=VB-VA,IME2=VC-VA;
                const FVector IMP=FVector::CrossProduct(IMD,IME2);
                const double IMDet=FVector::DotProduct(IME1,IMP);
                bool IMPierce=false;FVector IMHit(0,0,0);
                if(!FMath::IsNearlyZero(IMDet))
                {
                    const double IMInv=1.0/IMDet;
                    const FVector IMT=IMSegA-VA;
                    const double IMU=FVector::DotProduct(IMT,IMP)*IMInv;
                    if(IMU>=-1e-6&&IMU<=1.0+1e-6)
                    {
                        const FVector IMQ=FVector::CrossProduct(IMT,IME1);
                        const double IMV=FVector::DotProduct(IMD,IMQ)*IMInv;
                        if(IMV>=-1e-6&&IMU+IMV<=1.0+1e-6)
                        {
                            const double IMTp=FVector::DotProduct(IME2,IMQ)*IMInv;
                            if(IMTp>=-1e-6&&IMTp<=1.0+1e-6){IMPierce=true;IMHit=IMSegA+IMD*float(IMTp);}
                        }
                    }
                }
                if(IMPierce)
                {
                    ++IMSegHits;
                    UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakePierce tri=%d hit=(%.3f,%.3f,%.3f) %s"),TI,IMHit.X,IMHit.Y,IMHit.Z,*IMActorRanges[RI].Value);
                }
            }
        }
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakePierce totalhits=%d"),IMSegHits);
        int32 IMHoleDumped=0,IMHoleTotal=0;
        for(int32 RI=0;RI<IMActorRanges.Num()&&IMHoleDumped<60;++RI)
        {
            const int32 IMStart=IMActorRanges[RI].Key;
            const int32 IMEnd=(RI+1<IMActorRanges.Num())?IMActorRanges[RI+1].Key:Runtime->Geometry.Triangles.size();
            for(int32 TI=IMStart;TI<IMEnd;++TI)
            {
                const IPLTriangle& IMT=Runtime->Geometry.Triangles[TI];
                const IPLVector3& A=Runtime->Geometry.Vertices[IMT.indices[0]];
                const IPLVector3& B=Runtime->Geometry.Vertices[IMT.indices[1]];
                const IPLVector3& C=Runtime->Geometry.Vertices[IMT.indices[2]];
                const float CX=(A.x+B.x+C.x)/3, CY=(A.y+B.y+C.y)/3, CZ=(A.z+B.z+C.z)/3;
                if(CX>-0.6f&&CX<0.6f&&CY>-0.6f&&CY<0.6f&&CZ>0.5f&&CZ<2.5f)
                {
                    ++IMHoleTotal;
                    if(IMHoleDumped<60)
                    {
                        ++IMHoleDumped;
                        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeHole tri=%d c=(%.3f,%.3f,%.3f) v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f) %s"),TI,CX,CY,CZ,A.x,A.y,A.z,B.x,B.y,B.z,C.x,C.y,C.z,*IMActorRanges[RI].Value);
                    }
                }
            }
        }
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeHole total=%d"),IMHoleTotal);
    }
    if(Failed||ExportedTriangles==0)
    { Status=TEXT("Scene check failed: required geometry/material incomplete. See Scene Issues.");return false; }
    const IPLVector3 Min=IMToSDKPosition(Region.Min,Runtime->Origin),Max=IMToSDKPosition(Region.Max,Runtime->Origin);
    auto& Params=Runtime->ProbeParams;Params={};Params.type=IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    Params.spacing=ProbeSpacingCm*.01f;Params.height=ProbeHeightCm*.01f;
    const float Lo[3]={FMath::Min(Min.x,Max.x),FMath::Min(Min.y,Max.y),FMath::Min(Min.z,Max.z)};
    const float Hi[3]={FMath::Max(Min.x,Max.x),FMath::Max(Min.y,Max.y),FMath::Max(Min.z,Max.z)};
    // Pinned SDK 4.8.1 probe_generator.cpp samples [-.5,.5], despite phonon.h
    // documenting [0,1]. Translation must be the center: using Min generated
    // only the negative half-room and left the listener without a path.
    for(int I=0;I<3;++I){Params.transform.elements[I][I]=Hi[I]-Lo[I];Params.transform.elements[I][3]=(Lo[I]+Hi[I])*.5f;}
    Params.transform.elements[3][3]=1;
    FSHA1 Hash;
    auto Feed=[&Hash](const auto& Values)
    {
        const uint64 Count=Values.size();Hash.Update(reinterpret_cast<const uint8*>(&Count),sizeof(Count));
        if(Count)Hash.Update(reinterpret_cast<const uint8*>(Values.data()),uint32(Count*sizeof(Values[0])));
    };
    Feed(Runtime->Geometry.Vertices);Feed(Runtime->Geometry.Triangles);Feed(Runtime->Geometry.MaterialIndices);Feed(Runtime->Geometry.Materials);
    Hash.Update(reinterpret_cast<const uint8*>(&Params),sizeof(Params));
    const int RecipeVersion=IM_AcousticRecipe::Version;Hash.Update(reinterpret_cast<const uint8*>(&RecipeVersion),sizeof(RecipeVersion));
    for(int I=0;I<3;++I){const double V=Runtime->Origin[I];Hash.Update(reinterpret_cast<const uint8*>(&V),sizeof(V));}
    Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);Runtime->Fingerprint=BytesToHex(Digest,20);
    if(Runtime->ProbeFingerprint==Runtime->Fingerprint)Runtime->Geometry.Probes=MoveTemp(PreviousProbes);
    else Runtime->ProbeFingerprint.Empty();
    GeneratedProbes=static_cast<int32>(Runtime->Geometry.Probes.size());
    Runtime->SceneValid=true;Status=TEXT("Scene check passed; exclusions are listed explicitly.");return true;
}

void AIMAcousticBakeVolume::InspectScene(){CaptureScene();}
bool AIMAcousticBakeVolume::ValidateCurrentBake(FString& Failure)
{
    if(!CaptureScene()){Failure=Status;return false;}
    if(!BakedField){Failure=TEXT("Missing acoustic bake asset.");return false;}
    return BakedField->Validate(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),Runtime->Fingerprint,Failure);
}
void AIMAcousticBakeVolume::GenerateProbes()
{
    if(Runtime->Job){Status=TEXT("Wait for the current bake or cancel it.");return;}
    if(!CaptureScene())return;
    IM_AcousticSimulation Simulation;std::string Error;
    if(!Simulation.GenerateProbes(Runtime->Geometry,Runtime->ProbeParams,Runtime->Geometry.Probes,Error))
    {Status=UTF8_TO_TCHAR(Error.c_str());return;}
    GeneratedProbes=static_cast<int32>(Runtime->Geometry.Probes.size());
    Runtime->ProbeFingerprint=Runtime->Fingerprint;Status=TEXT("Probes generated; inspect coverage before baking.");
    ShowProbeCoverage();
}
void AIMAcousticBakeVolume::ShowProbeCoverage()
{
    // Enables the persistent selection overlay below. The previous one-shot draw
    // used radius-1m spheres for 10 seconds; 632 of them overlap into one blob and
    // expire before they can be inspected, which is the "cannot tell what is
    // baked" problem this replaces.
    bShowBakedFieldOverlay=true;
    if(Runtime->Geometry.Probes.empty()&&BakedField)
    {
        FString Error;
        if(!CaptureScene()||!BakedField->Validate(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),Runtime->Fingerprint,Error))
        {Status=Error.IsEmpty()?TEXT("Cannot show coverage: scene is invalid."):Error;return;}
        Status=TEXT("Baked-field overlay on; the bound bake is valid for the current scene.");return;
    }
    Status=FString::Printf(TEXT("Baked-field overlay on: %d generated probes%s."),int32(Runtime->Geometry.Probes.size()),
        BakedField?TEXT(" (a bake asset is bound; the overlay shows the bake)"):TEXT(" (not baked yet)"));
}
// Editor-only overlay: while this actor is selected (or bShowBakedFieldOverlay is
// on) the viewport shows the configured bake bounds (green), the probe extent with
// one probe radius included (orange) and every probe as a dot (cyan = read from the
// bound bake asset, yellow = generated this session but not baked). Answers
// "which areas are covered" without guessing; no runtime effect.
void AIMAcousticBakeVolume::DrawEditorFieldOverlay()
{
#if WITH_EDITOR
    if(!bShowBakedFieldOverlay&&!IsSelectedInEditor())return;
    UWorld* World=GetWorld();if(!World)return;
    const FVector BoundsCenter=BakeBounds?BakeBounds->GetComponentLocation():GetActorLocation();
    const FVector BoundsExtent=BakeBounds?BakeBounds->GetScaledBoxExtent():FVector(500,500,250);
    if(BakeBounds)
        DrawDebugBox(World,BoundsCenter,BoundsExtent,BakeBounds->GetComponentQuat(),FColor(120,255,140),false,-1.f,SDPG_Foreground,2.f);

    // Precedence mirrors ShowProbeCoverage: probes generated in this session win
    // over the bound asset's list. The asset branch reads the asset's own recorded
    // world/fingerprint, so the overlay shows what WAS baked and reports staleness
    // separately instead of hiding the data.
    TArray<FVector> Points;bool bFromAsset=false;
    if(!Runtime->Geometry.Probes.empty())
    {
        Points.Reserve(int32(Runtime->Geometry.Probes.size()));
        for(const auto& P:Runtime->Geometry.Probes)Points.Add(IMFromSDKPosition(P.center,Runtime->Origin));
    }
    else if(BakedField)
    {
        if(OverlayAsset.Get()!=BakedField)
        {
            OverlayAsset=BakedField;OverlayAssetProbes.Reset();OverlayAssetOrigin=FVector::ZeroVector;
            FString Error;
            if(!BakedField->GetProbePreview(BakedField->WorldPackage,BakedField->SceneFingerprint,OverlayAssetProbes,OverlayAssetOrigin,Error))
                UE_LOG(LogTemp,Warning,TEXT("IMLogs AcousticOverlay probe preview unavailable: %s"),*Error);
        }
        Points.Reserve(OverlayAssetProbes.Num());
        for(const FVector4& P:OverlayAssetProbes)Points.Add(OverlayAssetOrigin+FVector(-P.Z,P.X,P.Y)*100.0);
        bFromAsset=true;
    }
    if(!Points.IsEmpty())
    {
        FBox Covered(ForceInit);
        for(const FVector& P:Points)Covered+=P;
        Covered=Covered.ExpandBy(FVector(100)); // one probe radius, cm
        DrawDebugBox(World,Covered.GetCenter(),Covered.GetExtent(),FColor(255,170,60),false,-1.f,SDPG_Foreground,1.f);
        const FColor ProbeColor=bFromAsset?FColor(0,220,255):FColor(255,220,0);
        for(const FVector& P:Points)DrawDebugPoint(World,P,12.f,ProbeColor,false,-1.f,SDPG_Foreground);
    }
    const bool bStale=BakedField&&!Runtime->Fingerprint.IsEmpty()&&BakedField->SceneFingerprint!=Runtime->Fingerprint;
    const FString Summary=FString::Printf(TEXT("Overlay: %d probes, spacing %.0fcm, height %.0fcm, %s%s. green=bake bounds, orange=probe extent (+1m radius), %s=probes."),
        Points.Num(),ProbeSpacingCm,ProbeHeightCm,
        bFromAsset?TEXT("read from the bound bake asset"):(Points.IsEmpty()?TEXT("no probes yet: run GenerateProbes / Bake"):TEXT("generated this session, NOT baked yet")),
        bStale?TEXT("; STALE: scene changed since the bake, rebake required"):TEXT(""),
        bFromAsset?TEXT("cyan"):TEXT("yellow"));
    if(OverlaySummary!=Summary){OverlaySummary=Summary;UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticOverlay %s"),*Summary);}
#endif
}
void AIMAcousticBakeVolume::Bake()
{
#if WITH_EDITOR
    if(GetWorld()->IsGameWorld()||Runtime->Job)return;
    if(!CaptureScene())return;
    if(Runtime->Geometry.Probes.empty()||Runtime->ProbeFingerprint!=Runtime->Fingerprint)
    {Status=TEXT("Generate probes again: scene or probe configuration changed.");return;}
    auto Job=MakeShared<IM_BakeJob,ESPMode::ThreadSafe>();Runtime->Job=Job;
    Job->Fingerprint=Runtime->Fingerprint;Job->WorldPackage=UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName());
    Job->MetadataJson=IMBakeMetadata(*this,Runtime->Geometry,Runtime->Origin);
    auto Input=Runtime->Geometry;
    Job->Future=Async(EAsyncExecution::Thread,[Job,Input=MoveTemp(Input)]()
    {
        IM_AcousticSimulation Simulation;
        Job->Success=Simulation.Bake(Input,Job->Data,Job->Error,&Job->Cancelled);
        Job->Done.store(true,std::memory_order_release);
    });
    // Editor actor ticks depend on viewport realtime. Completion must also work
    // with a paused viewport, so a weak GT ticker owns only the pending bake.
    TWeakObjectPtr<AIMAcousticBakeVolume> WeakOwner(this);
    TWeakPtr<IM_BakeJob,ESPMode::ThreadSafe> WeakJob(Job);
    Runtime->BakeTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [WeakOwner,WeakJob](float)
        {
            auto* Owner=WeakOwner.Get();auto Expected=WeakJob.Pin();
            if(!Owner||!Expected||Owner->Runtime->Job!=Expected)return false;
            Owner->PollBakeCompletion();return Owner->Runtime->Job==Expected;
        }),.05f);
    Status=TEXT("Baking. Cancellation waits for the current SDK pass; previous asset is preserved.");
#endif
}
void AIMAcousticBakeVolume::CancelBake()
{
    if(Runtime->Job){Runtime->Job->Cancelled.store(true);Status=TEXT("Cancellation requested; waiting for current SDK pass.");}
}

void AIMAcousticBakeVolume::BeginPlay()
{
    Super::BeginPlay();Runtime->Epoch=IMNextWorldEpoch.fetch_add(1);
    CaptureScene();
    if(!Runtime->SceneValid||!BakedField)
    {
        FString Issues;
        for(int32 I=0;I<FMath::Min(8,SceneIssues.Num());++I){if(!Issues.IsEmpty())Issues+=TEXT(" | ");Issues+=SceneIssues[I];}
        Issues.ReplaceInline(TEXT("%"),TEXT("%%"));
        UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeDegraded scenevalid=%d bakedfield=%d issues=%d status=%s detail=%s"),int(Runtime->SceneValid),int(BakedField!=nullptr),SceneIssues.Num(),*Status,*Issues);
    }
}
void AIMAcousticBakeVolume::ShutdownOwnedWork()
{
    if(!Runtime)return;
    FTSTicker::GetCoreTicker().RemoveTicker(Runtime->BakeTicker);Runtime->BakeTicker.Reset();
    FTSTicker::GetCoreTicker().RemoveTicker(Runtime->ValidationTicker);Runtime->ValidationTicker.Reset();
    if(Runtime->Bridge)
    {
        // A volume that lost device enrollment must never revoke another
        // volume's reverb lease, including during repeated EndPlay/BeginDestroy.
        auto Expected=Runtime->Epoch;
        Runtime->Bridge->ReverbWorldGeneration.compare_exchange_strong(Expected,0,std::memory_order_acq_rel);
    }
    if(Runtime->Job){Runtime->Job->Cancelled.store(true);Runtime->Job->Future.Wait();Runtime->Job.Reset();}
    if(Runtime->MetaSoundContext) IM_StopAcousticMetaSoundContext(Runtime->MetaSoundContext);
    if(Runtime->MetaSoundEnvironment.IsValid())
    {
        Runtime->MetaSoundEnvironment->Stop();
        Runtime->MetaSoundEnvironment->DestroyComponent();
        Runtime->MetaSoundEnvironment.Reset();
    }
    if(Runtime->Worker){Runtime->Worker->StopAndJoin();Runtime->Worker.Reset();}
    if (Runtime->DeviceHandle.IsValid() && Runtime->MetaSoundBus.IsValid())
        Runtime->DeviceHandle.GetAudioDevice()->GetSubsystem<UAudioBusSubsystem>()->StopAudioBus(Audio::FAudioBusKey(Runtime->MetaSoundBus->GetUniqueID()));
    Runtime->MetaSoundBus.Reset();
    Runtime->MetaSoundContext.Reset();
    Runtime->MetaSoundRegisteredSources.Reset();
    Runtime->MetaSoundAudibleReported = false;
    if(Runtime->DeviceHandle.IsValid()&&ReverbSubmix)
    {
        auto* Mixer=static_cast<Audio::FMixerDevice*>(Runtime->DeviceHandle.GetAudioDevice());
        if(ReverbPreset)Mixer->RemoveSubmixEffect(ReverbSubmix,ReverbPreset->GetUniqueID());
        Mixer->UnregisterSoundSubmix(ReverbSubmix,false);
    }
    ReverbPreset=nullptr;ReverbSubmix=nullptr;Runtime->Reverb.Reset();Runtime->DeviceHandle.Reset();
    Runtime->Bridge.Reset();
}
void AIMAcousticBakeVolume::EndPlay(const EEndPlayReason::Type Reason){ShutdownOwnedWork();Super::EndPlay(Reason);}
void AIMAcousticBakeVolume::BeginDestroy(){ShutdownOwnedWork();Super::BeginDestroy();}

void AIMAcousticBakeVolume::PollBakeCompletion()
{
    check(IsInGameThread());
#if WITH_EDITOR
    if(Runtime->Job && Runtime->Job->Done.load(std::memory_order_acquire))
    {
        auto Job=Runtime->Job;Job->Future.Wait();Runtime->Job.Reset();
        if(Job->Cancelled.load()||!Job->Success){Status=Job->Cancelled.load()?TEXT("Bake cancelled; previous asset preserved."):UTF8_TO_TCHAR(Job->Error.c_str());return;}
        if(!CaptureScene()||Runtime->Fingerprint!=Job->Fingerprint){Status=TEXT("Bake discarded: scene changed during bake.");return;}
        if(Job->Data.Scene.size()>MAX_int32||Job->Data.ProbeBatch.size()>MAX_int32)
        {Status=TEXT("Bake exceeds the supported 2 GiB asset section limit; previous asset preserved.");return;}
        // Save a new complete asset first. A failed save never mutates the previous
        // complete UObject or its package; assign BakedField only after success.
        const FString Name=TEXT("IM_")+FPackageName::GetShortName(Job->WorldPackage)+TEXT("_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString PackageName=TEXT("/IceMoonAcousticField/Bakes/")+Name;
        UPackage* Package=CreatePackage(*PackageName);
        auto* Asset=NewObject<UIMAcousticBakeAsset>(Package,*Name,RF_Public|RF_Standalone);
        TArray<uint8> Scene,Probes;Scene.Append(Job->Data.Scene.data(),int32(Job->Data.Scene.size()));Probes.Append(Job->Data.ProbeBatch.data(),int32(Job->Data.ProbeBatch.size()));
        FString Error;
        if(!Asset->CommitCompleteBake(Job->WorldPackage,Job->Fingerprint,Job->MetadataJson,MoveTemp(Scene),MoveTemp(Probes),Error)){Status=Error;return;}
        FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
        const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
        if(!UPackage::SavePackage(Package,Asset,*Filename,Args)){Status=TEXT("Bake asset save failed; previous asset preserved.");return;}
        FAssetRegistryModule::AssetCreated(Asset);Modify();BakedField=Asset;MarkPackageDirty();Status=TEXT("Bake saved. Save the level to persist its asset binding.");
    }
#endif
}
bool AIMAcousticBakeVolume::IM_SetPathingValidationForTest(bool bOn)
{
    if (!Runtime || !Runtime->Worker) { return false; }
    Runtime->Worker->RequestPathingValidationForTest(bOn);
    return true;
}
int AIMAcousticBakeVolume::IM_GetAppliedPathingValidationForTest() const
{
    if (!Runtime || !Runtime->Worker) { return -2; }
    return Runtime->Worker->GetAppliedPathingValidationForTest();
}
bool AIMAcousticBakeVolume::IM_SetApertureTransitForTest(bool bOn, const FVector& CenterUE,
    const FVector& HalfExtentUE)
{
    if(!Runtime)return false;
    if(!bOn)
    {
        Runtime->ApertureTransitEnabled=false;
        return true;
    }
    const FVector E=HalfExtentUE.GetAbs();
    if(CenterUE.ContainsNaN()||E.ContainsNaN()||E.GetMin()<=0.0f)return false;
    Runtime->ApertureTransitCenterUE=CenterUE;
    Runtime->ApertureTransitHalfExtentUE=E;
    Runtime->ApertureTransitEnabled=true;
    return true;
}
void AIMAcousticBakeVolume::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    PollBakeCompletion();
    if(!GetWorld())return;
    if(!GetWorld()->IsGameWorld())
    {
#if WITH_EDITOR
        DrawEditorFieldOverlay();
#endif
        return;
    }
    const double Now=FPlatformTime::Seconds();if(Now<Runtime->NextSnapshot)return;Runtime->NextSnapshot=Now+.05;
    if(!Runtime->SceneValid||!BakedField){Status=TEXT("V2 degraded: missing/stale scene or bake asset.");return;}
    FAudioDevice* Device=GetWorld()->GetAudioDeviceRaw();if(!Device)return;
    auto* Mixer=FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(this);
    if(!Mixer||Mixer->GetNumDeviceChannels()!=2){Status=TEXT("V2 degraded: a stereo mixer device is required.");return;}
    // A source can be created after this volume's first game tick (ordinary
    // delayed binding and several lifecycle fixtures do exactly that).  The
    // initial owner must not permanently lock the world to the legacy
    // spatialization bridge before the graph source exists.  Rebuild the
    // owner when the discovered consumer kind changes; shutdown joins the old
    // worker and stops the old environment before the new generation starts.
    bool bGraphPipeline = false;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TArray<UIMAcousticSourceComponent*> Sources; It->GetComponents(Sources);
        for (auto* Source : Sources)
        {
            bGraphPipeline |= IsValid(Source->AudioComponent) && IM_IsAcousticMetaSound(Source->AudioComponent->Sound);
        }
    }
    if (Runtime->Worker && (Runtime->MetaSoundContext.IsValid() != bGraphPipeline))
    {
        const bool bWasGraph = Runtime->MetaSoundContext.IsValid();
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound pipeline_rebind from=%s to=%s epoch=%llu"),
            bWasGraph ? TEXT("graph") : TEXT("legacy"),
            bGraphPipeline ? TEXT("graph") : TEXT("legacy"), Runtime->Epoch);
        ShutdownOwnedWork();
        Runtime->Epoch = IMNextWorldEpoch.fetch_add(1, std::memory_order_relaxed);
        Status = TEXT("V2 rebind: acoustic consumer pipeline changed; rebuilding.");
        return;
    }
    if(!Runtime->Worker)
    {
        FString Error;
        if(!BakedField->Validate(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),Runtime->Fingerprint,Error)){Status=Error;return;}
        if (bGraphPipeline)
        {
            Runtime->MetaSoundContext = IM_CreateAcousticMetaSoundContext(Device->DeviceID, Mixer->SampleRate, Runtime->Epoch);
            if (!Runtime->MetaSoundContext) { Status = TEXT("MetaSound environment already owned on this device."); return; }
            Runtime->Bridge = Runtime->MetaSoundContext->Device;
        }
        else Runtime->Bridge=IM_FindAcousticDevice(Device);
        if(!Runtime->Bridge){Status=TEXT("V2 degraded: select IceMoon Acoustic Field as Windows spatialization plugin.");return;}
        IM_AcousticBakeData Data;Data.Scene.assign(BakedField->SceneData.GetData(),BakedField->SceneData.GetData()+BakedField->SceneData.Num());Data.ProbeBatch.assign(BakedField->ProbeData.GetData(),BakedField->ProbeData.GetData()+BakedField->ProbeData.Num());
        TArray<FVector4> ProbePreview;FVector ProbeOrigin;
        if(!BakedField->GetProbePreview(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),Runtime->Fingerprint,ProbePreview,ProbeOrigin,Error))
        {ShutdownOwnedWork();Status=Error;return;}
        for(const auto& P:ProbePreview)Data.CoverageProbes.push_back({{float(P.X),float(P.Y),float(P.Z)},float(P.W)});
        Runtime->Worker=MakeUnique<IM_AcousticSimulationWorker>();
        Runtime->Reverb=Runtime->MetaSoundContext ? Runtime->MetaSoundContext->Pool
            : MakeShared<IM_AcousticReverbPool,ESPMode::ThreadSafe>(Runtime->Epoch);
        if(!Runtime->Worker->Start(Runtime->Bridge,Runtime->Epoch,MoveTemp(Data),Runtime->Reverb)){ShutdownOwnedWork();Status=TEXT("V2 degraded: audio device already belongs to another acoustic world.");return;}
        Runtime->DeviceHandle=GetWorld()->GetAudioDevice();
        if (Runtime->MetaSoundContext)
        {
            UMetaSoundSource* Environment = LoadObject<UMetaSoundSource>(nullptr,
                TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticEnvironment"));
            if (!Environment) { Status = TEXT("Missing acoustic environment MetaSound asset."); ShutdownOwnedWork(); return; }
            Runtime->MetaSoundBus.Reset(NewObject<UAudioBus>(this));
            Runtime->MetaSoundBus->AudioBusChannels = EAudioBusChannels::Mono;
            auto* BusSubsystem = Device->GetSubsystem<UAudioBusSubsystem>();
            auto& Graph = *Runtime->MetaSoundContext;
            Graph.BusId = Runtime->MetaSoundBus->GetUniqueID();
            // Native reader's three 512-frame priming blocks underrun in the
            // measured 1024-frame mixer / 512-frame graph combination. Keep
            // four mixer callbacks of observed input before beginning reads;
            // extra capacity absorbs bounded producer/consumer scheduling skew.
            Graph.BusPrimeFrames = FMath::DivideAndRoundUp(Mixer->GetNumOutputFrames() * 4, 512) * 512;
            BusSubsystem->StartAudioBus(Audio::FAudioBusKey(Graph.BusId), TEXT("IM_AcousticEnvironment"), 1, false);
            Graph.BusPatch = BusSubsystem->AddPatchOutputForAudioBus(Audio::FAudioBusKey(Graph.BusId), Graph.BusPrimeFrames * 2, 1);
            if (!Graph.BusPatch) { Status = TEXT("Cannot allocate acoustic bus patch."); ShutdownOwnedWork(); return; }
            Runtime->MetaSoundEnvironment.Reset(NewObject<UAudioComponent>(this));
            auto* Audio = Runtime->MetaSoundEnvironment.Get();
            Audio->bAutoActivate = false;
            Audio->bAllowSpatialization = false;
            Audio->bIsUISound = true;
            Audio->SetSound(Environment);
            Audio->RegisterComponent();
            Audio->SetObjectParameter(TEXT("Acoustic Bus"), Runtime->MetaSoundBus.Get());
            Runtime->MetaSoundContext->WetGain.store(ReverbWetGain, std::memory_order_relaxed);
            Audio->Play();
            UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound ordinary_environment device=%u epoch=%llu graph_frames=512 mixer_frames=%d"),
                Device->DeviceID, Runtime->Epoch, Mixer->GetNumOutputFrames());
        }
        else
        {
        ReverbSubmix=NewObject<USoundSubmix>(this,TEXT("IM_EnvironmentReverb"));
        ReverbSubmix->bAutoDisable=false;ReverbSubmix->bMuteWhenBackgrounded=false;
        Mixer->RegisterSoundSubmix(ReverbSubmix);
        ReverbPreset=NewObject<UIMAcousticReverbPreset>(this);
        ReverbPreset->Bind(Runtime->Bridge,Runtime->Reverb,ReverbWetGain);
        UAudioMixerBlueprintLibrary::AddSubmixEffect(this,ReverbSubmix,ReverbPreset);
        }
        Runtime->Bridge->ReverbWorldGeneration.store(Runtime->Epoch,std::memory_order_release);
    }
    if(Runtime->Worker->GetState()==IM_AcousticSimulationWorker::State::LoadFailed){Status=TEXT("V2 degraded: SDK bake load failed.");return;}
    IM_AcousticTimingScope Timing(Runtime->Bridge->ProfilingEnabled.load(std::memory_order_relaxed)?&Runtime->Bridge->GameThreadTiming:nullptr);
    if(Runtime->PreviousSnapshot)IM_AcousticRecordMaximum(Runtime->Bridge->MaxSnapshotGapUs,uint64((Now-Runtime->PreviousSnapshot)*1.e6));
    Runtime->PreviousSnapshot=Now;
    Runtime->Bridge->Enabled.store(bEnableV2,std::memory_order_release);
    Runtime->Bridge->RenderRoutes.store((bDirectRoute?1u:0u)|(bPathRoute?2u:0u)|(bReverbRoute?4u:0u),std::memory_order_relaxed);
    FTransform Listener,Extra;
    if(!Device->GetListenerTransform(0,Listener)||Device->GetListenerTransform(1,Extra))
    {Status=TEXT("V2 degraded: exactly one listener is required.");return;}
    const FBox Region=BakeBounds->Bounds.GetBox();
    // Keep the graph source's dry and independent room-send distance curves
    // live even when the listener cannot publish a bake snapshot. The previous
    // ordering returned before this update, leaving a wall-interior/out-of-bounds
    // listener on the last valid point's gain and making the degraded window
    // report a false distance.
    if (Runtime->MetaSoundContext)
    {
        FString SourceFailure;
        for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        {
            TArray<UIMAcousticSourceComponent*> Sources; It->GetComponents(Sources);
            for (auto* Source : Sources)
            {
                if (!Source) continue;
                if (!Source->ValidateSource(SourceFailure)) continue;
                UAudioComponent* Audio = Source->AudioComponent;
                if (!Audio || !IM_IsAcousticMetaSound(Audio->Sound)) continue;
                const uint64 Id = Audio->GetAudioComponentID();
                const int32 Slot = IM_RegisterAcousticMetaSoundSource(Runtime->MetaSoundContext, Id);
                if (Slot == INDEX_NONE) continue;
                Runtime->MetaSoundContext->SendGains[Slot].store(Audio->VolumeMultiplier, std::memory_order_relaxed);
                const float SourceListenerDistanceM = float(FVector::Distance(Audio->GetComponentLocation(), Listener.GetLocation()) * .01);
                const float ReverbSendDistanceGain = IM_AcousticRecipe::ReverbSendDistanceGain(SourceListenerDistanceM);
                Runtime->MetaSoundContext->ReverbSendGains[Slot].store(
                    Audio->VolumeMultiplier * ReverbSendDistanceGain, std::memory_order_relaxed);
                Runtime->MetaSoundContext->DistanceGains[Slot].store(
                    1.f / FMath::Max(1.f, SourceListenerDistanceM), std::memory_order_relaxed);
                Runtime->MetaSoundContext->DistanceCm[Slot].store(
                    SourceListenerDistanceM * 100.f, std::memory_order_relaxed);
                Runtime->MetaSoundContext->WetGain.store(FMath::Clamp(ReverbWetGain, 0.f, 1.f), std::memory_order_relaxed);
                if (!Runtime->MetaSoundRegisteredSources.Contains(Id))
                {
                    // Registration precedes the first generator construction;
                    // this must also work when the first listener snapshot is
                    // intentionally rejected by bounds/geometry fail-closed.
                    Audio->Stop();
                    Audio->SetIntParameter(TEXT("Acoustic Voice"), Slot);
                    Audio->SetObjectParameter(TEXT("Acoustic Bus"), Runtime->MetaSoundBus.Get());
                    Runtime->MetaSoundRegisteredSources.Add(Id);
                    Audio->Play();
                    UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound ordinary_source audio=%llu slot=%d epoch=%llu"), Id, Slot, Runtime->Epoch);
                }
            }
        }
    }
    if(!Region.IsInsideOrOn(Listener.GetLocation()))
    {
        Runtime->Bridge->RenderRoutes.store((bDirectRoute ? 1u : 0u) | (bPathRoute ? 2u : 0u), std::memory_order_relaxed);
        Status=TEXT("V2 degraded: listener outside bake bounds.");return;
    }
    const IPLCoordinateSpace3 ListenerSpace = IMToSDKSpace(Listener, Runtime->Origin);
    if (IMPointInsideAcousticGeometry(Runtime->Geometry, Runtime->GeometryTriangleRanges, ListenerSpace.origin))
    {
        // Do not let an invalid wall-interior position keep selecting a
        // nearest baked probe. Stop publishing the snapshot and immediately
        // remove only the reverb route; direct/path dry audio remains enabled.
        // The environment operator also treats this route bit as part of its
        // freshness contract, so an already queued IR cannot produce wet
        // output during the 250 ms lease transition.
        Runtime->Bridge->RenderRoutes.store((bDirectRoute ? 1u : 0u) | (bPathRoute ? 2u : 0u), std::memory_order_relaxed);
        Status = TEXT("V2 degraded: listener inside baked acoustic geometry.");
        return;
    }
    auto Snapshot=MakeShared<IM_AcousticWorldSnapshot,ESPMode::ThreadSafe>();
    Snapshot->WorldGeneration=Runtime->Epoch;Snapshot->CapturedSeconds=Now;Snapshot->Listener=ListenerSpace;
    Snapshot->ApertureTransitEnabled=Runtime->ApertureTransitEnabled;
    if(Runtime->ApertureTransitEnabled)
    {
        Snapshot->ApertureTransitCenter=IMToSDKPosition(Runtime->ApertureTransitCenterUE,Runtime->Origin);
        const FVector E=Runtime->ApertureTransitHalfExtentUE.GetAbs()*0.01f;
        Snapshot->ApertureTransitHalfExtent={float(E.Y),float(E.Z),float(E.X)};
    }
    FString Failure;
    TMap<UMaterialInterface*,int32> DynamicMaterialIndices;
    TArray<IPLMaterial> DynamicMaterials;
    for (const FIMAcousticMaterialMapping& Mapping : Materials)
    {
        UMaterialInterface* MaterialAsset = Mapping.Material.Get();
        if (!MaterialAsset || DynamicMaterialIndices.Contains(MaterialAsset)) { continue; }
        IPLMaterial Material{};
        for (int32 Band = 0; Band < 3; ++Band)
        {
            Material.absorption[Band] = FMath::Clamp(static_cast<float>(Mapping.Absorption[Band]), 0.0f, 1.0f);
            Material.transmission[Band] = 0.0f;
        }
        Material.scattering = FMath::Clamp(Mapping.Scattering, 0.0f, 1.0f);
        DynamicMaterialIndices.Add(MaterialAsset, DynamicMaterials.Num());
        DynamicMaterials.Add(Material);
    }
    // Sub-phase diagnostic: world traversal isolated from worker submit.
    const uint64 IMTravStart=FPlatformTime::Cycles64();
    for(TActorIterator<AActor> It(GetWorld());It;++It)
    {
        TArray<UIMAcousticSourceComponent*> Sources;It->GetComponents(Sources);
        for(auto* Source:Sources)
        {
            if(!Source->ValidateSource(Failure))continue;
            UAudioComponent* Audio=Source->AudioComponent;
            if(!Region.IsInsideOrOn(Audio->GetComponentLocation())){Failure=TEXT("Source outside bake bounds.");continue;}
            Snapshot->Sources.Add({Audio->GetAudioComponentID(),IMToSDKSpace(Audio->GetComponentTransform(),Runtime->Origin)});
        }
    }
    if (Snapshot->Sources.Num() > 1)
    {
        static double IM_SrcCountLast = 0;
        if (Now - IM_SrcCountLast > 5.0)
        {
            IM_SrcCountLast = Now;
            const uint64 IM_FirstId = Snapshot->Sources.Num() ? Snapshot->Sources[0].AudioComponentId : 0;
            UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticSourceCount n=%d first_id=%llu"), Snapshot->Sources.Num(), IM_FirstId);
        }
    }
    // H1 W2: collect registered dynamic blockers into this snapshot. Validity
    // is checked here on the GT (no UObject crosses to the worker); missing or
    // failed entries are omitted so the worker removes them, degrading to
    // unoccluded audio rather than stale geometry. Skips are observable via
    // the snapshot probe counters and verbose log, not via the source status.
    int32 IMDynamicSkipped = 0;
    for (const TObjectPtr<UStaticMeshComponent>& Blocker : DynamicBlockers)
    {
        if (!IsValid(Blocker)) { ++IMDynamicSkipped; continue; }
        IM_AcousticDynamicMeshSnapshot DynamicEntry;
        FString DynamicFailure;
        if (!IMBuildDynamicMeshSnapshot(Blocker.Get(), 0, Runtime->Origin, DynamicMaterialIndices, DynamicMaterials, DynamicEntry, DynamicFailure))
        {
            ++IMDynamicSkipped;
            UE_LOG(LogTemp, Verbose, TEXT("[IM] Acoustic dynamic blocker skipped: %s (%s)"), *Blocker->GetPathName(), *DynamicFailure);
            continue;
        }
        Snapshot->DynamicMeshes.Add(MoveTemp(DynamicEntry));
    }
    if(Runtime->Bridge->ProfilingEnabled.load(std::memory_order_relaxed))Runtime->Bridge->GTTraverseTiming.Record(IMTravStart);
    // Sub-phase diagnostic: worker submit isolated from traversal.
    const uint64 IMSubStart=FPlatformTime::Cycles64();
    const bool IMProbeHasWorker = (Runtime->Worker != nullptr);
    const bool IMProbeSubmitted = IMProbeHasWorker && Runtime->Worker->Submit(Snapshot);
    if(Runtime->Bridge->ProfilingEnabled.load(std::memory_order_relaxed))Runtime->Bridge->GTSubmitTiming.Record(IMSubStart);
    const double IMProbeSubmitDone = FPlatformTime::Seconds();
    // W1 causal probe (GT side): actual AudioDevice listener snapshot that the
    // worker will consume. Joins to worker results and audio blocks by
    // (WorldGeneration, CapturedSeconds). Preallocated write-once log; zero behavior.
    // Times are FPlatformTime::Seconds() wall clock, seconds.
    IM_AcousticSnapshotProbe IMProbeSnap{};
    IMProbeSnap.WorldGeneration = Snapshot->WorldGeneration;
    IMProbeSnap.CapturedSeconds = Snapshot->CapturedSeconds;
    IMProbeSnap.SubmitSeconds = IMProbeHasWorker ? IMProbeSubmitDone : 0.0;
    IMProbeSnap.ListenerUEX = float(Listener.GetLocation().X);
    IMProbeSnap.ListenerUEY = float(Listener.GetLocation().Y);
    IMProbeSnap.ListenerUEZ = float(Listener.GetLocation().Z);
    IMProbeSnap.ListenerSDKX = Snapshot->Listener.origin.x;
    IMProbeSnap.ListenerSDKY = Snapshot->Listener.origin.y;
    IMProbeSnap.ListenerSDKZ = Snapshot->Listener.origin.z;
    if (Snapshot->Sources.Num() > 0)
    {
        IMProbeSnap.Source0X = Snapshot->Sources[0].Source.origin.x;
        IMProbeSnap.Source0Y = Snapshot->Sources[0].Source.origin.y;
        IMProbeSnap.Source0Z = Snapshot->Sources[0].Source.origin.z;
        IMProbeSnap.Source0AudioId = Snapshot->Sources[0].AudioComponentId;
    }
    IMProbeSnap.NumSources = uint32(Snapshot->Sources.Num());
    IMProbeSnap.NumDynamicMeshes = uint32(Snapshot->DynamicMeshes.Num());
    IMProbeSnap.NumDynamicSkipped = uint32(IMDynamicSkipped);
    if (Snapshot->DynamicMeshes.Num() > 0)
    {
        IMProbeSnap.Door0TX = Snapshot->DynamicMeshes[0].Transform.elements[0][3];
        IMProbeSnap.Door0TY = Snapshot->DynamicMeshes[0].Transform.elements[1][3];
        IMProbeSnap.Door0TZ = Snapshot->DynamicMeshes[0].Transform.elements[2][3];
    }
    IMProbeSnap.Submitted = IMProbeSubmitted ? 1 : 0;
    IMProbeSnap.FailCode = IMProbeSubmitted ? 0 : (IMProbeHasWorker ? 1 : 2);
    Runtime->Bridge->TraceSnapshot(IMProbeSnap);
    Status=Failure.IsEmpty()?FString::Printf(TEXT("V2 routing %d sources; rendered %llu, degraded %llu, reverb audible %llu blocks."),Snapshot->Sources.Num(),Runtime->Bridge->RenderedBlocks.load(),Runtime->Bridge->RejectedBlocks.load(),Runtime->Bridge->ReverbNonzeroBlocks.load()):TEXT("V2 source rejected: ")+Failure;
    if (Runtime->MetaSoundContext && !Runtime->MetaSoundAudibleReported && Runtime->Bridge->ReverbNonzeroBlocks.load() >= 50)
    {
        Runtime->MetaSoundAudibleReported = true;
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound ordinary_wet_active epoch=%llu source_blocks=%llu wet_blocks=%llu ir=%llu legacy_submix=%d"),
            Runtime->Epoch, Runtime->MetaSoundContext->SourceBlocks.load(), Runtime->Bridge->ReverbNonzeroBlocks.load(),
            Runtime->MetaSoundContext->LastIRSequence.load(), ReverbSubmix != nullptr);
    }
}
