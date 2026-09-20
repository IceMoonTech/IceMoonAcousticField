// ***CROSSFLOOR-TEST-V1***
// E-leg: legal cross-floor traverse in an isolated two-storey stair fixture.
// Lower room y=0..3, slab y=3..3.2 with open stair void x=[0.5,1.5], upper
// room y=3.2..6.2. Source and listener both climb through the void.
// Gates (plan mapping): slab blocks direct (B-direct PCM16 energy==0 over a
// valid complete window; the d_direct block counter is diagnostic only per
// the approved O2 revision, user 2026-09-13),
// stair carries reverb (B-reverb wet>0, zero rejects), traverse stays
// connected (zero rejects, wet>0), ear-balance flips start->end at fixed
// yaw (direction follows relative position). No whitebox map touched.
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include <limits>
#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticTestSupport.h"
#include "IMAcousticSpatialization.h"
#include "Audio.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Sound/SoundWaveProcedural.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealEdGlobals.h"
#include <initializer_list>
namespace {
constexpr int32 IMCFRate = 48000;
constexpr double IMCFSettle = 2.0;
constexpr double IMCFShort = 2.0;
constexpr double IMCFReverb = 3.0;
constexpr double IMCFTraverse = 10.0;
// Recorder tail margin: StopRecordingOutput can land up to one mixer block
// (~16 ms observed) short of the requested window under load, so each capture
// keeps recording past its measured interval by this margin. The measured
// intervals (including the 10 s traverse movement) are unchanged; the extra
// tail records the static end pose / open scene.
constexpr double IMCFRecTail = 0.25;
struct IMCFV3 { double X, Y, Z; };
struct IMCFAabb { IMCFV3 Lo, Hi; };
struct IMCFInterval { double A = 0, B = 0; };
struct IMCFProbe { IMCFV3 P; double Radius = 0; };
struct IMCFPositionSample { double T = 0; IMCFV3 Source, Listener; };
struct IMCFGeometryResult { bool Pass = false; int32 BlockedSegments = 0; };
struct IMCFCoverageResult
{
  bool Pass = false;
  double Total = 0, MinNearest = TNumericLimits<double>::Max(), MaxNearest = 0;
  TArray<IMCFInterval> Uncovered;
  // Certified-visibility extension: intervals where probe spheres still
  // overlap the route but no visible (unoccluded) witness remains, plus the
  // margin values carried into the qualification report.
  TArray<IMCFInterval> BlockedOnly;
  int32 MinVisibleWitness = TNumericLimits<int32>::Max();
  int32 MaxBlocked = 0, MaxOverlaps = 0;
};
static FVector IMCFToUE(IMCFV3 P) { return FVector(-P.Z * 100, P.X * 100, P.Y * 100); }
static IMCFV3 IMCFRouteToSDK(IMCFV3 P, const FVector& OriginCm)
{
  const FVector UE = IMCFToUE(P);
  return { (UE.Y - OriginCm.Y) * 0.01, (UE.Z - OriginCm.Z) * 0.01, -(UE.X - OriginCm.X) * 0.01 };
}
static constexpr double IMCFQualificationSampleM = 0.05;
static constexpr double IMCFQualificationMarginM = 1e-3;
// Endpoints closer than this are treated as touching. It matches
// IMCFQualificationMarginM (both bound the same AABB/interval rounding) and is
// far below the 0.05 m route sample step, so it cannot hide a resolvable gap.
static constexpr double IMCFCoverageGapM = 1e-3;
// IMAcousticSimulation.cpp:687 caps the certified set at eight overlapping
// probes and the product fails closed once eight overlaps count as blocked.
static constexpr int32 IMCFMaxSelectedProbes = 8;
static constexpr double IMCFPositionToleranceM = 1e-6;
static IMCFV3 IMCFSwp[] = {
  { -2.0, 1.5, -0.5 }, { 0.0, 1.5, -1.5 }, { 1.0, 2.0, -1.5 },
  { 1.0, 3.8, -1.0 }, { 1.0, 4.5, -0.5 }, { 2.5, 4.7, 2.0 },
};
static IMCFV3 IMCFLwp[] = {
  { 0.0, 1.5, 0.5 }, { 1.0, 2.0, -1.5 }, { 1.0, 3.8, -1.0 }, { -1.5, 4.7, -0.5 },
};
static const IMCFV3 IMCFListenerRoute[] = {
  { 0.0, 1.5, 0.5 }, { 0.0, 1.5, 1.25 }, { 0.8, 1.5, 1.25 },
  { 0.8, 2.3, 0.75 }, { 0.8, 3.1, 0.25 }, { 0.8, 3.9, -0.25 },
  { 0.8, 4.7, -0.75 }, { 0.0, 4.7, -0.75 }, { -1.5, 4.7, -0.5 },
};
static const IMCFV3 IMCFSourceRoute[] = {
  { -2.0, 1.5, -0.5 }, { 1.2, 1.5, 1.25 }, { 1.2, 2.3, 0.75 },
  { 1.2, 3.1, 0.25 }, { 1.2, 3.9, -0.25 }, { 1.2, 4.7, -0.75 },
  { 2.0, 4.7, -0.75 }, { 2.5, 4.7, 2.0 },
};
static double IMCFDistanceSq(IMCFV3 A, IMCFV3 B)
{
  const double X = A.X - B.X, Y = A.Y - B.Y, Z = A.Z - B.Z;
  return X * X + Y * Y + Z * Z;
}
static double IMCFPathLength(const IMCFV3* W, int32 N)
{
  double Total = 0;
  for (int32 I = 1; I < N; ++I) Total += FMath::Sqrt(IMCFDistanceSq(W[I - 1], W[I]));
  return Total;
}
static IMCFV3 IMCFWpLerp(const IMCFV3* W, int32 N, double K)
{
  const double Total = IMCFPathLength(W, N);
  double Distance = FMath::Clamp(K, 0.0, 1.0) * Total;
  for (int32 I = 1; I < N; ++I)
  {
    const double Segment = FMath::Sqrt(IMCFDistanceSq(W[I - 1], W[I]));
    if (Distance <= Segment || I == N - 1)
    {
      const double F = Segment > 0 ? FMath::Clamp(Distance / Segment, 0.0, 1.0) : 0.0;
      return { W[I - 1].X + (W[I].X - W[I - 1].X) * F, W[I - 1].Y + (W[I].Y - W[I - 1].Y) * F, W[I - 1].Z + (W[I].Z - W[I - 1].Z) * F };
    }
    Distance -= Segment;
  }
  return W[N - 1];
}
static IMCFV3 IMCFPathPointAtDistance(const IMCFV3* W, int32 N, double Distance)
{
  const double Total = IMCFPathLength(W, N);
  return IMCFWpLerp(W, N, Total > 0 ? Distance / Total : 0.0);
}
static bool IMCFSegmentIntersectsAabb(IMCFV3 A, IMCFV3 B, const IMCFAabb& Box, double Margin)
{
  const double Lo[3] = { Box.Lo.X - Margin, Box.Lo.Y - Margin, Box.Lo.Z - Margin };
  const double Hi[3] = { Box.Hi.X + Margin, Box.Hi.Y + Margin, Box.Hi.Z + Margin };
  const double P[3] = { A.X, A.Y, A.Z }, D[3] = { B.X - A.X, B.Y - A.Y, B.Z - A.Z };
  double T0 = 0, T1 = 1;
  for (int32 I = 0; I < 3; ++I)
  {
    if (FMath::IsNearlyZero(D[I]))
    {
      if (P[I] < Lo[I] || P[I] > Hi[I]) return false;
      continue;
    }
    double A0 = (Lo[I] - P[I]) / D[I], A1 = (Hi[I] - P[I]) / D[I];
    if (A0 > A1) Swap(A0, A1);
    T0 = FMath::Max(T0, A0); T1 = FMath::Min(T1, A1);
    if (T0 > T1) return false;
  }
  return true;
}
static IMCFGeometryResult IMCFCheckGeometry(const IMCFV3* W, int32 N, const TArray<IMCFAabb>& Boxes)
{
  IMCFGeometryResult Result; Result.Pass = N >= 2 && !Boxes.IsEmpty();
  for (int32 I = 1; I < N; ++I)
  {
    const double Length = FMath::Sqrt(IMCFDistanceSq(W[I - 1], W[I]));
    const int32 Samples = FMath::Max(1, FMath::CeilToInt(Length / IMCFQualificationSampleM));
    for (int32 S = 0; S < Samples; ++S)
    {
      const double A = double(S) / Samples, B = double(S + 1) / Samples;
      const IMCFV3 P0 = { W[I - 1].X + (W[I].X - W[I - 1].X) * A, W[I - 1].Y + (W[I].Y - W[I - 1].Y) * A, W[I - 1].Z + (W[I].Z - W[I - 1].Z) * A };
      const IMCFV3 P1 = { W[I - 1].X + (W[I].X - W[I - 1].X) * B, W[I - 1].Y + (W[I].Y - W[I - 1].Y) * B, W[I - 1].Z + (W[I].Z - W[I - 1].Z) * B };
      for (const IMCFAabb& Box : Boxes)
      {
        if (IMCFSegmentIntersectsAabb(P0, P1, Box, IMCFQualificationMarginM))
        {
          ++Result.BlockedSegments; Result.Pass = false; break;
        }
      }
    }
  }
  return Result;
}
static bool IMCFCheckTreadRoute(const IMCFV3* W, int32 N, int32 FirstTread)
{
  static const double ZLo[4] = { 0.5, 0.0, -0.5, -1.0 };
  static const double ZHi[4] = { 1.0, 0.5, 0.0, -0.5 };
  static const double YTop[4] = { 0.8, 1.6, 2.4, 3.2 };
  if (N <= FirstTread + 3 || W[0].Y > 3.0 || W[N - 1].Y < 3.2) return false;
  for (int32 I = 0; I < 4; ++I)
  {
    const IMCFV3 P = W[FirstTread + I];
    if (P.X < 0.5 - IMCFQualificationMarginM || P.X > 1.5 + IMCFQualificationMarginM
      || P.Z < ZLo[I] - IMCFQualificationMarginM || P.Z > ZHi[I] + IMCFQualificationMarginM
      || FMath::Abs(P.Y - (YTop[I] + 1.5)) > IMCFQualificationMarginM) return false;
    if (I > 0 && P.Y <= W[FirstTread + I - 1].Y) return false;
  }
  return true;
}
static bool IMCFProbeSegmentInterval(IMCFV3 A, IMCFV3 B, const IMCFProbe& Probe, double Offset, double& OutA, double& OutB)
{
  const IMCFV3 V = { B.X - A.X, B.Y - A.Y, B.Z - A.Z };
  const IMCFV3 Q = { A.X - Probe.P.X, A.Y - Probe.P.Y, A.Z - Probe.P.Z };
  const double Segment = FMath::Sqrt(IMCFDistanceSq(A, B));
  if (Segment <= SMALL_NUMBER) return IMCFDistanceSq(A, Probe.P) <= Probe.Radius * Probe.Radius;
  const double AA = Segment * Segment;
  const double BB = 2.0 * (Q.X * V.X + Q.Y * V.Y + Q.Z * V.Z);
  const double CC = (Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z) - Probe.Radius * Probe.Radius;
  const double Discriminant = BB * BB - 4.0 * AA * CC;
  if (Discriminant < 0) return false;
  const double Root = FMath::Sqrt(FMath::Max(0.0, Discriminant));
  const double T0 = FMath::Max(0.0, (-BB - Root) / (2.0 * AA));
  const double T1 = FMath::Min(1.0, (-BB + Root) / (2.0 * AA));
  if (T0 > T1) return false;
  OutA = Offset + T0 * Segment; OutB = Offset + T1 * Segment; return true;
}
// True when the segment touches a fixture box. The fixture is a set of
// axis-aligned scaled engine cubes, so this matches the product's single
// occlusion raycast (IMAcousticSimulation.cpp:719-724) analytically. The
// shared margin keeps grazing hits on the blocked side (fail closed).
static bool IMCFSegmentBlockedByBoxes(IMCFV3 A, IMCFV3 B, const TArray<IMCFAabb>& Boxes)
{
  for (const IMCFAabb& Box : Boxes) if (IMCFSegmentIntersectsAabb(A, B, Box, IMCFQualificationMarginM)) return true;
  return false;
}
// Inverse of IMCFRouteToSDK. GetProbePreview reports probes_m in SDK metres
// while the routes and GeometryBoxes stay in fixture metres; mixing the two
// would offset the occlusion test from the real scene by the bake origin.
static IMCFV3 IMCFProbeToFixture(IMCFV3 P, const FVector& OriginCm)
{
  return { P.X + OriginCm.Y * 0.01, P.Y + OriginCm.Z * 0.01, P.Z - OriginCm.X * 0.01 };
}
// Mirrors IMAcousticSimulation.cpp:687-729: overlap candidates use the same
// float error bounds, boundary uncertainty counts as blocked, a blocked
// candidate can never supply the visible witness, and eight blocks fail closed.
static void IMCFEvaluateVisibility(IMCFV3 L, const TArray<IMCFProbe>& Probes, const TArray<IMCFAabb>& Boxes,
  int32& OutOverlaps, int32& OutBlocked, int32& OutVisible)
{
  OutOverlaps = OutBlocked = OutVisible = 0;
  // std::numeric_limits, not TNumericLimits: the UE specialization for float
  // exposes only Min/Max/Lowest, and these must be the IEEE float constants the
  // product rule (IMAcousticSimulation.cpp:687-729) uses.
  const double Eps = std::numeric_limits<float>::epsilon();
  const double MinNormal = std::numeric_limits<float>::min();
  if (!FMath::IsFinite(L.X) || !FMath::IsFinite(L.Y) || !FMath::IsFinite(L.Z)) { OutBlocked = IMCFMaxSelectedProbes; return; }
  for (const IMCFProbe& Probe : Probes)
  {
    if (!(Probe.Radius > 0) || !FMath::IsFinite(Probe.Radius)
      || !FMath::IsFinite(Probe.P.X) || !FMath::IsFinite(Probe.P.Y) || !FMath::IsFinite(Probe.P.Z))
    { OutBlocked = IMCFMaxSelectedProbes; break; }
    const double X = L.X - Probe.P.X, Y = L.Y - Probe.P.Y, Z = L.Z - Probe.P.Z;
    const double Distance2 = X * X + Y * Y + Z * Z, Radius2 = double(Probe.Radius) * Probe.Radius;
    const double ErrorBound = 16 * Eps * FMath::Max(Distance2, Radius2) + 16 * MinNormal;
    if (Distance2 - Radius2 > ErrorBound) continue;
    ++OutOverlaps;
    bool Uncertain = Radius2 - Distance2 <= ErrorBound;
    const double Positions[3] = { L.X, L.Y, L.Z };
    const double Centers[3] = { Probe.P.X, Probe.P.Y, Probe.P.Z };
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
      const double BoxError = 2 * Eps * (FMath::Abs(Centers[Axis]) + Probe.Radius) + 2 * MinNormal;
      if (Positions[Axis] - (Centers[Axis] - Probe.Radius) <= BoxError || (Centers[Axis] + Probe.Radius) - Positions[Axis] <= BoxError) Uncertain = true;
    }
    if (Uncertain) { if (++OutBlocked >= IMCFMaxSelectedProbes) break; continue; }
    if (IMCFSegmentBlockedByBoxes(L, Probe.P, Boxes)) { if (++OutBlocked >= IMCFMaxSelectedProbes) break; }
    else ++OutVisible;
  }
}
// Routes and boxes are fixture metres; probe centres arrive in SDK metres and
// are converted with the inverse of IMCFRouteToSDK before any distance test.
static IMCFCoverageResult IMCFCheckCoverage(const IMCFV3* W, int32 N, const TArray<IMCFProbe>& Probes,
  const TArray<IMCFAabb>& Boxes, const FVector& OriginCm)
{
  IMCFCoverageResult Result; Result.Total = IMCFPathLength(W, N);
  TArray<IMCFProbe> FixtureProbes;
  FixtureProbes.Reserve(Probes.Num());
  for (const IMCFProbe& Probe : Probes) FixtureProbes.Add({ IMCFProbeToFixture(Probe.P, OriginCm), Probe.Radius });
  TArray<IMCFInterval> Intervals; double Offset = 0;
  for (int32 I = 1; I < N; ++I)
  {
    const double Segment = FMath::Sqrt(IMCFDistanceSq(W[I - 1], W[I]));
    for (const IMCFProbe& Probe : FixtureProbes)
    {
      double A = 0, B = 0;
      if (IMCFProbeSegmentInterval(W[I - 1], W[I], Probe, Offset, A, B)) Intervals.Add({ A, B });
    }
    Offset += Segment;
  }
  Intervals.Sort([](const IMCFInterval& A, const IMCFInterval& B) { return A.A < B.A; });
  TArray<IMCFInterval> Merged;
  for (const IMCFInterval& Interval : Intervals)
  {
    if (Merged.IsEmpty() || Interval.A - Merged.Last().B > IMCFCoverageGapM) Merged.Add(Interval);
    else Merged.Last().B = FMath::Max(Merged.Last().B, Interval.B);
  }
  if (Merged.IsEmpty()) Result.Uncovered.Add({ 0, Result.Total });
  else
  {
    if (Merged[0].A > IMCFCoverageGapM) Result.Uncovered.Add({ 0, Merged[0].A });
    for (int32 I = 1; I < Merged.Num(); ++I) if (Merged[I].A - Merged[I - 1].B > IMCFCoverageGapM) Result.Uncovered.Add({ Merged[I - 1].B, Merged[I].A });
    if (Result.Total - Merged.Last().B > IMCFCoverageGapM) Result.Uncovered.Add({ Merged.Last().B, Result.Total });
  }
  // Sphere coverage alone is not certification: sample the product rule along
  // the route and record every sample where no visible witness survives.
  const int32 Samples = FMath::Max(1, FMath::CeilToInt(Result.Total / IMCFQualificationSampleM));
  const double Step = Result.Total / Samples;
  TArray<IMCFInterval> BlockedRuns; double RunStart = -1, RunEnd = -1;
  for (int32 I = 0; I <= Samples; ++I)
  {
    const double D = Result.Total * double(I) / Samples;
    const IMCFV3 P = IMCFPathPointAtDistance(W, N, D);
    double Nearest = TNumericLimits<double>::Max();
    for (const IMCFProbe& Probe : FixtureProbes) Nearest = FMath::Min(Nearest, FMath::Sqrt(IMCFDistanceSq(P, Probe.P)));
    Result.MinNearest = FMath::Min(Result.MinNearest, Nearest); Result.MaxNearest = FMath::Max(Result.MaxNearest, Nearest);
    int32 Overlaps = 0, Blocked = 0, Visible = 0;
    IMCFEvaluateVisibility(P, FixtureProbes, Boxes, Overlaps, Blocked, Visible);
    Result.MinVisibleWitness = FMath::Min(Result.MinVisibleWitness, Visible);
    Result.MaxBlocked = FMath::Max(Result.MaxBlocked, Blocked);
    Result.MaxOverlaps = FMath::Max(Result.MaxOverlaps, Overlaps);
    if (Visible > 0) { if (RunStart >= 0) { BlockedRuns.Add({ RunStart, RunEnd }); RunStart = -1; } }
    else { if (RunStart < 0) RunStart = D; RunEnd = D + Step; }
  }
  if (RunStart >= 0) BlockedRuns.Add({ RunStart, RunEnd });
  // Runs are sample aligned; merge runs separated by at most one sample step
  // so the report lists intervals instead of individual samples.
  for (const IMCFInterval& Run : BlockedRuns)
  {
    const IMCFInterval Clamped = { FMath::Min(Run.A, Result.Total), FMath::Min(Run.B, Result.Total) };
    if (Result.BlockedOnly.IsEmpty() || Clamped.A - Result.BlockedOnly.Last().B > Step + IMCFCoverageGapM) Result.BlockedOnly.Add(Clamped);
    else Result.BlockedOnly.Last().B = FMath::Max(Result.BlockedOnly.Last().B, Clamped.B);
  }
  if (Result.MinVisibleWitness == TNumericLimits<int32>::Max()) Result.MinVisibleWitness = 0;
  Result.Pass = !Probes.IsEmpty() && Result.Total > 0 && Result.Uncovered.IsEmpty() && Result.BlockedOnly.IsEmpty();
  return Result;
}
static TArray<TSharedPtr<FJsonValue>> IMCFJsonNumbers(std::initializer_list<double> Values)
{
  TArray<TSharedPtr<FJsonValue>> Result;
  for (double Value : Values) Result.Add(MakeShared<FJsonValueNumber>(Value));
  return Result;
}
static TArray<TSharedPtr<FJsonValue>> IMCFJsonIntervals(const TArray<IMCFInterval>& Intervals)
{
  TArray<TSharedPtr<FJsonValue>> Result;
  for (const IMCFInterval& Interval : Intervals)
  {
    auto O = MakeShared<FJsonObject>();
    O->SetNumberField(TEXT("start_m"), Interval.A); O->SetNumberField(TEXT("end_m"), Interval.B);
    Result.Add(MakeShared<FJsonValueObject>(O));
  }
  return Result;
}
bool IMCFBox(UWorld* W, UStaticMesh* C, IMCFV3 Lo, IMCFV3 Hi, FString& Id)
{
  auto* A = W->SpawnActor<AStaticMeshActor>();
  if (!A) return false;
  auto* M = A->GetStaticMeshComponent();
  M->SetMobility(EComponentMobility::Static); M->SetStaticMesh(C);
  A->SetActorLocation(FVector(-(Lo.Z + Hi.Z) * 50, (Lo.X + Hi.X) * 50, (Lo.Y + Hi.Y) * 50));
  // Fixture units are SDK metres; the engine Cube edge is 1m, so actor scale
  // is metres (same as the W1/Rooms fixtures). A *100 here inflates every
  // box 100x and buries the start pair inside solid geometry (SDK occlusion 0).
  A->SetActorScale3D(FVector(Hi.Z - Lo.Z, Hi.X - Lo.X, Hi.Y - Lo.Y));
  M->ComponentTags.Add(TEXT("IMAcousticRequired"));
  Id += C->GetPathName() + TEXT("|") + M->GetComponentTransform().ToString() + TEXT("\n");
  return true;
}
struct IMCFState
{
  FAutomationTestBase* Test;
  FString Directory, MountRoot, MapPath, Identity;
  TArray<IMCFAabb> GeometryBoxes;
  TArray<IMCFProbe> BakeProbes;
  FVector ProbeOriginCm = FVector::ZeroVector;
  TSharedPtr<FJsonObject> QualificationJson;
  bool QualificationPassed = false, SweepPassed = false;
  uint64 SweepNotFreshBlocks = 0, SweepReverbRejectedBlocks = 0, SweepInputNonzeroBlocks = 0, SweepRenderInputNonzeroBlocks = 0;
  uint64 SweepReverbNonzeroBlocks = 0, SweepDryBlocks = 0;
  int32 SweepDegradedEvents = 0;
  FString LastDegradedStatus, FirstDegradedStatus;
  TArray<IMCFPositionSample> TraversePositions;
  double LastTraverseSampleT = -1;
  bool TraversePositionsMatch = false;
  float BgVol = FApp::GetUnfocusedVolumeMultiplier();
  // bAllowBackgroundAudio twin of the Lifecycle/W3/W1 fix: a backgrounded
  // editor otherwise feeds zeros to a playing+active voice (frozen queue,
  // zero input); pinning only the unfocused multiplier is not enough because
  // EditorEngine::Tick forces the global volume multiplier to zero while the
  // editor is unfocused and background audio is not allowed.
  bool BgAllowAudio = GetDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio;
  bool PrevThrottle = GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground;
  bool Restored = false;
  explicit IMCFState(FAutomationTestBase* T) : Test(T)
  {
    const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/W3-CrossFloor"), Id));
    MountRoot = TEXT("/IceMoonAcousticField/Tests/CrossFloor/IM_") + Id;
    MapPath = MountRoot + TEXT("/IM_cross_floor");
    FApp::SetUnfocusedVolumeMultiplier(1);
    GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = true;
    GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
  }
  void Restore()
  {
    if (Restored) return;
    FApp::SetUnfocusedVolumeMultiplier(BgVol);
    GetMutableDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio = BgAllowAudio;
    GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = PrevThrottle;
    Restored = true;
  }
  ~IMCFState() { Restore(); }
};
bool IMCFJson(const FString& F, const TSharedRef<FJsonObject>& O)
{
  FString T; auto Wr = TJsonWriterFactory<>::Create(&T);
  return FJsonSerializer::Serialize(O, Wr) && FFileHelper::SaveStringToFile(T, *F, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
UWorld* IMCFPIE()
{
  for (const auto& C : GEngine->GetWorldContexts())
    if (C.WorldType == EWorldType::PIE) return C.World();
  return nullptr;
}
struct IMCFCount { uint64 D = 0, P = 0, R = 0, RR = 0, Drop = 0, W = 0, Calls = 0, Input = 0; };
class IM_AcousticCrossFloorCommand final : public IAutomationLatentCommand
{
public:
  explicit IM_AcousticCrossFloorCommand(TSharedRef<IMCFState> S, int32 St = 0) : State(S), Stage(St), T0(FPlatformTime::Seconds()) { SegT0 = T0; }
  bool Update() override
  {
    check(IsInGameThread());
    const double Now = FPlatformTime::Seconds();
    if (Now - T0 > 600 && Stage <= 1) return Fail(TEXT("Cross-floor bake timed out."));
    if (Stage > 1 && Now - SegT0 > 120) return Fail(TEXT("Cross-floor capture stage timed out."));
    FString E;
    if (Stage == 0)
    {
      if (IMCFPIE()) return false;
      if (!Fixture(E)) return Fail(E);
      Volume->GenerateProbes();
      if (Volume->GeneratedProbes <= 0 || Volume->ExportedTriangles <= 0) return Fail(TEXT("Probe generation failed: ") + Volume->Status);
      Volume->Bake(); Stage = 1; T0 = Now; return false;
    }
    if (Stage == 1)
    {
      if (!Volume.IsValid()) return Fail(TEXT("Bake owner disappeared."));
      if (!Volume->BakedField) { if (!Volume->Status.StartsWith(TEXT("Baking."))) return Fail(Volume->Status); return false; }
      if (!Volume->ValidateCurrentBake(E)) return Fail(E);
      const auto* A = Volume->BakedField.Get();
      if (!FFileHelper::SaveArrayToFile(A->SceneData, *Ev(TEXT("scene.bin"))) || !FFileHelper::SaveArrayToFile(A->ProbeData, *Ev(TEXT("probes.bin"))) || !FFileHelper::SaveStringToFile(A->MetadataJson, *Ev(TEXT("bake-metadata.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) || !FFileHelper::SaveStringToFile(State->Identity, *Ev(TEXT("geometry-identity.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return Fail(TEXT("Cannot persist bake evidence."));
      BakeJson = MakeShared<FJsonObject>();
      BakeJson->SetNumberField(TEXT("exported_triangles"), Volume->ExportedTriangles);
      BakeJson->SetNumberField(TEXT("generated_probes"), Volume->GeneratedProbes);
      BakeJson->SetStringField(TEXT("bake_asset"), A->GetPathName());
      BakeJson->SetStringField(TEXT("scene_fingerprint"), A->SceneFingerprint);
      if (!IMCFJson(Ev(TEXT("bake.json")), BakeJson.ToSharedRef())) return Fail(TEXT("Cannot save bake report."));
      if (!Qualify(E)) return Fail(E);
      if (!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel, FPackageName::LongPackageNameToFilename(State->MapPath, FPackageName::GetMapPackageExtension()))) return Fail(TEXT("Cannot save fixture map."));
      GUnrealEd->AutomationLoadMap(*State->MapPath, false, &E);
      if (!E.IsEmpty()) return Fail(E);
      auto N = MakeShared<IM_AcousticCrossFloorCommand>(State, 2);
      FAutomationTestFramework::Get().EnqueueLatentCommand(N);
      return true;
    }
    UWorld* W = IMCFPIE();
    if (!W) return Fail(TEXT("PIE ended before cross-floor capture."));
    Bridge = IM_AcousticTestSupport::FindBridge(W);
    if (Stage >= 2) Feed(false);
    // Bounded audio-pump health probe (test-side only, 1 line/s while the
    // fixture readies the pump). Distinguishes an engine-side silent voice
    // (playing, no callback) from an empty procedural wave (queue) and from a
    // plugin that is never reached, so a dead pump is diagnosable from one run.
    if (Stage >= 3 && Stage < 10 && Now - PumpDiagT > 1.0)
    {
      PumpDiagT = Now;
      const int32 Queue = Wave.IsValid() ? Wave->GetAvailableAudioByteCount() : -1;
      UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorPump stage=%d playing=%d active=%d queuebytes=%d voices=%d pushcalls=%llu rendered=%llu direct=%llu zerogain=%llu invaliddirect=%llu rejected=%llu bgvol=%.2f bga=%d"),
        Stage,
        SrcAudio.IsValid() ? int(SrcAudio->IsPlaying()) : -1,
        SrcAudio.IsValid() ? int(SrcAudio->IsActive()) : -1,
        Queue,
        Bridge.IsValid() ? Bridge->Voices.Num() : -1,
        Bridge.IsValid() ? Bridge->PushDryCalls.load() : 0,
        Bridge.IsValid() ? Bridge->RenderedBlocks.load() : 0,
        Bridge.IsValid() ? Bridge->DirectNonzeroBlocks.load() : 0,
        Bridge.IsValid() ? Bridge->PushDryZeroGain.load() : 0,
        Bridge.IsValid() ? Bridge->PushDryInvalidDirect.load() : 0,
        Bridge.IsValid() ? Bridge->RejectedBlocks.load() : 0,
        FApp::GetUnfocusedVolumeMultiplier(),
        GetDefault<ULevelEditorMiscSettings>()->bAllowBackgroundAudio ? 1 : 0);
    }
    // Bounded first-rejection diagnostic (Astra branch (b), test-side only):
    // on the first new rejection after stage 11 (max 8 lines per run) record
    // the counters needed to tell an input stall from a stale-result stall.
    // Read-only; no control flow, lease, refresh or verdict is touched.
    if (Stage >= 11 && Bridge.IsValid() && CFDiagLogs < 8)
    {
      const uint64 Rej = Bridge->RejectedBlocks.load() + Bridge->ReverbRejectedBlocks.load();
      if (!CFDiagInit) { CFDiagInit = true; CFDiagRej = Rej; }
      else if (Rej > CFDiagRej)
      {
        ++CFDiagLogs;
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorRejectDiag stage=%d newrej=%llu total=%llu rendered=%llu push=%llu inputnz=%llu rinputnz=%llu queue=%d gtgap=%llu workergap=%llu"),
          Stage, Rej - CFDiagRej, Rej,
          Bridge->RenderedBlocks.load(), Bridge->PushDryCalls.load(), Bridge->PushDryInputNonzero.load(),
          Bridge->RenderInputNonzero.load(),
          Wave.IsValid() ? Wave->GetAvailableAudioByteCount() : -1,
          Bridge->MaxSnapshotGapUs.load(), Bridge->MaxWorkerGapUs.load());
        CFDiagRej = Rej;
      }
    }
    // Spawn-race watchdog (W1-proven): a playing+active voice can sit on zero
    // dry input for the whole run; bounded recovery is destroy+respawn (2 max).
    // Test-side only; verdicts, thresholds and stage timeouts unchanged.
    if (Stage >= 3 && Bridge.IsValid())
    {
      // Same bounded destroy+respawn recovery, extended down to the sweep so a
      // silent spawn cannot burn the whole sweep window before recovery runs.
      if (!CFInit) { CFInit = true; CFIn0 = Bridge->PushDryInputNonzero.load(); CFT0 = Now; }
      if (CFRe < 2 && Bridge->PushDryInputNonzero.load() == CFIn0 && Now - CFT0 > 10)
      {
        ++CFRe; CFInit = false;
        if (auto* Old = SrcAudio.Get()) { if (auto* OA = Old->GetOwner()) OA->Destroy(); }
        SrcAudio.Reset(); Wave.Reset();
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorRespawn n=%d"), CFRe);
        if (!SpawnSource(W, E)) return Fail(E);
        PumpProbeBase = Bridge.IsValid() ? Bridge->PushDryInputNonzero.load() : 0;
        PumpProbeT = Now; PumpAlive = false;
        return false;
      }
    }
    if (Stage == 2)
    {
      for (TActorIterator<AIMAcousticBakeVolume> It(W); It; ++It)
      {
        if (Volume.IsValid()) return Fail(TEXT("Cross-floor PIE has multiple bake owners."));
        Volume = *It;
      }
      Listener = W->GetFirstPlayerController();
      Bridge = IM_AcousticTestSupport::FindBridge(W);
      if (!Volume.IsValid() || !Listener.IsValid() || !Bridge) return Fail(TEXT("Cross-floor PIE owner/listener/device unavailable."));
      if (UWorld::RemovePIEPrefix(W->GetOutermost()->GetName()) != State->MapPath) return Fail(TEXT("PIE opened a different map."));
      if (Bridge->SampleRate != IMCFRate) return Fail(TEXT("Cross-floor gate requires a 48 kHz device."));
      if (!SpawnSource(W, E)) return Fail(E);
      Bridge = IM_AcousticTestSupport::FindBridge(W);
      PumpProbeBase = Bridge->PushDryInputNonzero.load(); PumpProbeT = Now; PumpAlive = false;
      SetPose(0); SetRoute(false, false, true); SegT0 = Now; Stage = 3; return false;
    }
    if (Stage == 3)
    {
      if (Now - SegT0 < IMCFSettle) return false;
      // Fixture audio-pump readiness (test-side only). A voice can report
      // playing+active while producing nothing after its first block
      // (W1-proven spawn race), so "has ever produced" is not enough: the
      // sweep opens only after a full 1 s probe window still carries input
      // blocks. Recovery is the existing bounded respawn (2 max) above; a pump
      // that is still dead after both respawns fails as fixture-invalid
      // instead of being reported as a continuity failure or a bare timeout.
      if (Now - PumpProbeT >= 1.0)
      {
        const uint64 Pump = Bridge.IsValid() ? Bridge->PushDryInputNonzero.load() : 0;
        PumpAlive = Pump > PumpProbeBase;
        PumpProbeBase = Pump; PumpProbeT = Now;
        // A fresh respawn gets its own full probe window before the verdict.
        if (!PumpAlive && CFRe >= 2) return Fail(TEXT("fixture audio pump invalid; continuity not judged"));
      }
      if (!PumpAlive) return false;
      SetRoute(false, false, true); SweepT0 = Now;
      SweepNotFresh0 = Bridge->ReverbNotFreshBlocks.load(); SweepReverbRejected0 = Bridge->ReverbRejectedBlocks.load();
      SweepReverbNonzero0 = Bridge->ReverbNonzeroBlocks.load(); SweepDry0 = Bridge->ReverbDryBlocks.load();
      SweepInput0 = Bridge->PushDryInputNonzero.load(); SweepRenderInput0 = Bridge->RenderInputNonzero.load();
      State->SweepDegradedEvents = 0; State->LastDegradedStatus.Reset(); State->FirstDegradedStatus.Reset(); Stage = 4; return false;
    }
    if (Stage == 4)
    {
      const double T = Now - SweepT0;
      SubmitTraversePose(T, false); PollSweepDegraded();
      if (T < IMCFTraverse) return false;
      State->SweepNotFreshBlocks = Bridge->ReverbNotFreshBlocks.load() - SweepNotFresh0;
      State->SweepReverbRejectedBlocks = Bridge->ReverbRejectedBlocks.load() - SweepReverbRejected0;
      State->SweepInputNonzeroBlocks = Bridge->PushDryInputNonzero.load() - SweepInput0;
      State->SweepRenderInputNonzeroBlocks = Bridge->RenderInputNonzero.load() - SweepRenderInput0;
      State->SweepReverbNonzeroBlocks = Bridge->ReverbNonzeroBlocks.load() - SweepReverbNonzero0;
      State->SweepDryBlocks = Bridge->ReverbDryBlocks.load() - SweepDry0;
      State->SweepPassed = State->SweepNotFreshBlocks == 0 && State->SweepReverbRejectedBlocks == 0 && State->SweepInputNonzeroBlocks > 0
        && State->SweepRenderInputNonzeroBlocks > 0 && State->SweepReverbNonzeroBlocks > 0 && State->SweepDryBlocks > 0;
      auto Sweep = MakeShared<FJsonObject>();
      Sweep->SetNumberField(TEXT("duration_seconds"), IMCFTraverse);
      Sweep->SetNumberField(TEXT("reverb_not_fresh_blocks"), double(State->SweepNotFreshBlocks));
      Sweep->SetNumberField(TEXT("reverb_rejected_blocks"), double(State->SweepReverbRejectedBlocks));
      Sweep->SetNumberField(TEXT("push_dry_input_nonzero_blocks"), double(State->SweepInputNonzeroBlocks));
      Sweep->SetNumberField(TEXT("render_input_nonzero_blocks"), double(State->SweepRenderInputNonzeroBlocks));
      Sweep->SetNumberField(TEXT("sweep_push_input_nonzero_delta"), double(State->SweepInputNonzeroBlocks));
      Sweep->SetNumberField(TEXT("sweep_render_input_nonzero_delta"), double(State->SweepRenderInputNonzeroBlocks));
      Sweep->SetNumberField(TEXT("sweep_reverb_nonzero_delta"), double(State->SweepReverbNonzeroBlocks));
      Sweep->SetNumberField(TEXT("sweep_dry_delta"), double(State->SweepDryBlocks));
      Sweep->SetNumberField(TEXT("degraded_events"), State->SweepDegradedEvents);
      Sweep->SetStringField(TEXT("first_degraded_status"), State->FirstDegradedStatus);
      if (!State->SweepPassed)
      {
        Sweep->SetStringField(TEXT("qualification_failure"), State->SweepNotFreshBlocks > 0
          ? TEXT("fixture coverage invalid; continuity not judged")
          : TEXT("fixture audio pump invalid; continuity not judged"));
      }
      Sweep->SetBoolField(TEXT("pass"), State->SweepPassed);
      State->QualificationJson->SetObjectField(TEXT("coverage_sweep"), Sweep);
      if (!WriteQualification(E)) return Fail(E);
      if (!State->SweepPassed) return Fail(State->SweepNotFreshBlocks > 0
        ? TEXT("fixture coverage invalid; continuity not judged")
        : TEXT("fixture audio pump invalid; continuity not judged"));
      SetRoute(true, false, false);
      if (!ResetSource(W, E)) return Fail(E);
      SetPose(0); SegT0 = Now; Stage = 10; return false;
    }
    if (Stage == 10)
    {
      if (Now - SegT0 < IMCFSettle) return false;
      // Start-segment readiness (test-side only): do not open the a-direct
      // capture until the direct path has produced a nonzero direct block.
      // Bounded by the existing 120s stage timeout above; baselines (Snap)
      // are still taken after readiness, immediately before StartRecording.
      if (!Bridge.IsValid() || Bridge->DirectNonzeroBlocks.load() == 0 || Bridge->PushDryInputNonzero.load() == 0)
      {
      if (Now - CFDiagT > 5)
        {
          CFDiagT = Now;
          UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorDirectWait direct=%llu path=%llu rendered=%llu rejected=%llu push=%llu/inputnz=%llu/nodirect=%llu/zerogain=%llu/rinputnz=%llu routes=%u enabled=%d"),
            Bridge.IsValid() ? Bridge->DirectNonzeroBlocks.load() : 0,
            Bridge.IsValid() ? Bridge->PathNonzeroBlocks.load() : 0,
            Bridge.IsValid() ? Bridge->RenderedBlocks.load() : 0,
            Bridge.IsValid() ? Bridge->RejectedBlocks.load() : 0,
            Bridge.IsValid() ? Bridge->PushDryCalls.load() : 0,
            Bridge.IsValid() ? Bridge->PushDryInputNonzero.load() : 0,
            Bridge.IsValid() ? Bridge->PushDryInvalidDirect.load() : 0,
            Bridge.IsValid() ? Bridge->PushDryZeroGain.load() : 0,
            Bridge.IsValid() ? Bridge->RenderInputNonzero.load() : 0,
            Bridge.IsValid() ? Bridge->RenderRoutes.load(std::memory_order_relaxed) : 0,
            Bridge.IsValid() ? int(Bridge->Enabled.load(std::memory_order_relaxed)) : -1);
        }
        return false;
      }
      UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorGatePass direct=%llu rendered=%llu push=%llu/inputnz=%llu"),
        Bridge->DirectNonzeroBlocks.load(), Bridge->RenderedBlocks.load(), Bridge->PushDryCalls.load(), Bridge->PushDryInputNonzero.load());
      Snap(); UAudioMixerBlueprintLibrary::StartRecordingOutput(W, IMCFShort + 1); SegT0 = Now; Stage = 11; return false;
    }
    if (Stage == 11)
    {
      if (Now - SegT0 < IMCFShort + IMCFRecTail) return false;
      UAudioMixerBlueprintLibrary::StopRecordingOutput(W, EAudioRecordingExportType::WavFile, TEXT("a-direct"), State->Directory);
      WavWait = Ev(TEXT("a-direct.wav")); WavLast = -1; Stage = 12; return false;
    }
    if (Stage == 12)
    {
      if (!WavStable()) return false;
      double En, Bal; if (!Analyze(WavWait, En, Bal, E)) return Fail(E);
      double Dd = Delta(0); Rec(TEXT("a-direct"), En, Bal, E);
      if (!(En > 0 && Dd > 0)) return Fail(FString::Printf(TEXT("Start direct inaudible: energy=%.9g d_direct=%.0f."), En, Dd));
      BalA = Bal; SetPose(1); SetRoute(true, false, false); SegT0 = Now; Stage = 13; return false;
    }
    if (Stage == 13)
    {
      if (Now - SegT0 < IMCFSettle) return false;
      Snap(); UAudioMixerBlueprintLibrary::StartRecordingOutput(W, IMCFShort + 1); SegT0 = Now; Stage = 14; return false;
    }
    if (Stage == 14)
    {
      if (Now - SegT0 < IMCFShort + IMCFRecTail) return false;
      UAudioMixerBlueprintLibrary::StopRecordingOutput(W, EAudioRecordingExportType::WavFile, TEXT("b-direct"), State->Directory);
      WavWait = Ev(TEXT("b-direct.wav")); WavLast = -1; Stage = 15; return false;
    }
    if (Stage == 15)
    {
      if (!WavStable()) return false;
      // Approved O2 (user 2026-09-13): the slab-blocked direct gate is the
      // recorded PCM16 window itself -- a valid, non-empty, complete capture
      // with En==0 passes. d_direct stays recorded in b-direct.json as a
      // diagnostic of the float-level counter residual; nonzero PCM fails.
      int32 Frames = 0;
      double En, Bal; if (!Analyze(WavWait, En, Bal, E, &Frames)) return Fail(E);
      double Dd = Delta(0); Rec(TEXT("b-direct"), En, Bal, E);
      if (Frames < int32(IMCFShort * IMCFRate)) return Fail(TEXT("Slab direct capture window truncated."));
      if (En != 0)
      {
        return Fail(FString::Printf(TEXT("Illegal slab direct: energy=%.9g d_direct=%.0f."), En, Dd));
      }
      SetRoute(false, false, true); SegT0 = Now; Stage = 16; return false;
    }
    if (Stage == 16)
    {
      if (Now - SegT0 < IMCFSettle) return false;
      Snap(); UAudioMixerBlueprintLibrary::StartRecordingOutput(W, IMCFReverb + 1); SegT0 = Now; Stage = 17; return false;
    }
    if (Stage == 17)
    {
      if (Now - SegT0 < IMCFReverb + IMCFRecTail) return false;
      UAudioMixerBlueprintLibrary::StopRecordingOutput(W, EAudioRecordingExportType::WavFile, TEXT("b-reverb"), State->Directory);
      WavWait = Ev(TEXT("b-reverb.wav")); WavLast = -1; Stage = 18; return false;
    }
    if (Stage == 18)
    {
      if (!WavStable()) return false;
      double En, Bal; if (!Analyze(WavWait, En, Bal, E)) return Fail(E);
      IMCFCount C = Deltas(); Rec(TEXT("b-reverb"), En, Bal, E);
      if (!(En > 0 && C.R == 0 && C.RR == 0 && C.Drop == 0 && C.W > 0 && C.Calls >= 100 && C.Input >= 100)) return Fail(TEXT("Stair reverb path degraded/dry."));
      SetPose(0); SetRoute(false, false, true); TravT0 = Now; State->TraversePositions.Reset(); State->LastTraverseSampleT = -1; Snap();
      UAudioMixerBlueprintLibrary::StartRecordingOutput(W, IMCFTraverse + 1); Stage = 19; return false;
    }
    if (Stage == 19)
    {
      const double T = Now - TravT0;
      SubmitTraversePose(T, true);
      if (T < IMCFTraverse + IMCFRecTail) return false;
      UAudioMixerBlueprintLibrary::StopRecordingOutput(W, EAudioRecordingExportType::WavFile, TEXT("traverse"), State->Directory);
      WavWait = Ev(TEXT("traverse.wav")); WavLast = -1; Stage = 20; return false;
    }
    if (Stage == 20)
    {
      if (!WavStable()) return false;
      double En, Bal; if (!Analyze(WavWait, En, Bal, E)) return Fail(E);
      State->TraversePositionsMatch = CheckTraversePositions();
      IMCFCount C = Deltas(); Rec(TEXT("traverse"), En, Bal, E);
      if (!(En > 0 && C.R == 0 && C.RR == 0 && C.Drop == 0 && C.W > 0)) return Fail(TEXT("Traverse dropout/degraded."));
      if (!State->TraversePositionsMatch) return Fail(TEXT("Traverse submitted positions diverged from qualified route."));
      SetPose(2); SetRoute(true, false, false); SegT0 = Now; Stage = 21; return false;
    }
    if (Stage == 21)
    {
      if (Now - SegT0 < IMCFSettle) return false;
      Snap(); UAudioMixerBlueprintLibrary::StartRecordingOutput(W, IMCFShort + 1); SegT0 = Now; Stage = 22; return false;
    }
    if (Stage == 22)
    {
      if (Now - SegT0 < IMCFShort + IMCFRecTail) return false;
      UAudioMixerBlueprintLibrary::StopRecordingOutput(W, EAudioRecordingExportType::WavFile, TEXT("end-direct"), State->Directory);
      WavWait = Ev(TEXT("end-direct.wav")); WavLast = -1; Stage = 23; return false;
    }
    if (Stage == 23)
    {
      if (!WavStable()) return false;
      double En, Bal; if (!Analyze(WavWait, En, Bal, E)) return Fail(E);
      Rec(TEXT("end-direct"), En, Bal, E);
      auto Sum = MakeShared<FJsonObject>();
      Sum->SetStringField(TEXT("scope"), TEXT("UE-device-crossfloor-legal-move"));
      Sum->SetNumberField(TEXT("captures_completed"), Points.Num());
      Sum->SetNumberField(TEXT("start_balance"), BalA);
      Sum->SetNumberField(TEXT("end_balance"), Bal);
      Sum->SetBoolField(TEXT("balance_flipped"), BalA * Bal < 0);
      const bool P = En > 0 && FMath::Abs(BalA) > 0.02 && FMath::Abs(Bal) > 0.02 && BalA * Bal < 0;
      Sum->SetBoolField(TEXT("passed"), P);
      if (!IMCFJson(Ev(TEXT("summary.json")), Sum)) return Fail(TEXT("Cannot save cross-floor summary."));
      return Done(P, P ? TEXT("Cross-floor legal traverse complete; slab blocks direct, stair carries path, direction flips.") : TEXT("End direct cue failed."));
    }
    return false;
  }
private:
  FString Ev(const TCHAR* N) const { return FPaths::Combine(State->Directory, N); }
  void SetRoute(bool D, bool P, bool R)
  {
    if (Volume.IsValid()) { Volume->bDirectRoute = D; Volume->bPathRoute = P; Volume->bReverbRoute = R; }
  }
  void SetPose(int32 I)
  {
    IMCFV3 S, L;
    if (I == 0) { S = { -2.0, 1.5, -0.5 }; L = { 0.0, 1.5, 0.5 }; }
    else if (I == 1) { S = { -2.0, 1.5, 2.0 }; L = { -1.5, 4.7, -2.0 }; }
    else { S = IMCFSwp[5]; L = IMCFLwp[3]; }
    if (Listener.IsValid()) Listener->SetAudioListenerOverride(nullptr, IMCFToUE(L), FRotator(0, -90, 0));
    if (SrcAudio.IsValid()) SrcAudio->SetWorldLocation(IMCFToUE(S));
  }
  TSharedPtr<FJsonObject> CoverageJson(const IMCFCoverageResult& R) const
  {
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("pass"), R.Pass); O->SetNumberField(TEXT("total_length_m"), R.Total);
    O->SetNumberField(TEXT("min_nearest_probe_distance_m"), R.MinNearest);
    O->SetNumberField(TEXT("max_nearest_probe_distance_m"), R.MaxNearest);
    O->SetArrayField(TEXT("uncovered_intervals_m"), IMCFJsonIntervals(R.Uncovered));
    O->SetArrayField(TEXT("blocked_only_intervals_m"), IMCFJsonIntervals(R.BlockedOnly));
    O->SetNumberField(TEXT("visible_witness_min"), R.MinVisibleWitness);
    O->SetNumberField(TEXT("max_blocked_overlaps"), R.MaxBlocked);
    O->SetNumberField(TEXT("max_overlaps"), R.MaxOverlaps);
    return O;
  }
  bool WriteQualification(FString& E)
  {
    if (!State->QualificationJson.IsValid()) { E = TEXT("Qualification report was not initialized."); return false; }
    if (!IMCFJson(Ev(TEXT("qualification.json")), State->QualificationJson.ToSharedRef())) { E = TEXT("Cannot save qualification report."); return false; }
    return true;
  }
  bool Qualify(FString& E)
  {
    TArray<FVector4> Preview; FVector Origin;
    const auto* Asset = Volume->BakedField.Get();
    if (!Asset->GetProbePreview(State->MapPath, Asset->SceneFingerprint, Preview, Origin, E)) return false;
    State->BakeProbes.Reset(); State->ProbeOriginCm = Origin;
    // GetProbePreview's probes_m are SDK metres; route points use the same basis as IMToSDKPosition(P, Origin).
    for (const FVector4& P : Preview) State->BakeProbes.Add({ { P.X, P.Y, P.Z }, P.W });
    // Certified coverage runs in fixture metres (routes + boxes) and converts
    // the SDK-space probe centres; verify the inverse before trusting it.
    {
      static const IMCFV3 Guard[] = { { 0.0, 1.5, 0.5 }, { -2.0, 4.7, 2.0 }, { 1.25, 2.3, -0.75 } };
      for (const IMCFV3& G : Guard)
      {
        const IMCFV3 Back = IMCFProbeToFixture(IMCFRouteToSDK(G, Origin), Origin);
        if (FMath::Sqrt(IMCFDistanceSq(G, Back)) > IMCFPositionToleranceM)
        { E = TEXT("Probe-space inverse does not round-trip; coverage space is unverified."); return false; }
      }
    }
    const IMCFGeometryResult ListenerGeometry = IMCFCheckGeometry(IMCFListenerRoute, UE_ARRAY_COUNT(IMCFListenerRoute), State->GeometryBoxes);
    const IMCFGeometryResult SourceGeometry = IMCFCheckGeometry(IMCFSourceRoute, UE_ARRAY_COUNT(IMCFSourceRoute), State->GeometryBoxes);
    const bool ListenerTreads = IMCFCheckTreadRoute(IMCFListenerRoute, UE_ARRAY_COUNT(IMCFListenerRoute), 3);
    const bool SourceTreads = IMCFCheckTreadRoute(IMCFSourceRoute, UE_ARRAY_COUNT(IMCFSourceRoute), 2);
    const IMCFCoverageResult ListenerCoverage = IMCFCheckCoverage(IMCFListenerRoute, UE_ARRAY_COUNT(IMCFListenerRoute), State->BakeProbes, State->GeometryBoxes, Origin);
    const IMCFCoverageResult SourceCoverage = IMCFCheckCoverage(IMCFSourceRoute, UE_ARRAY_COUNT(IMCFSourceRoute), State->BakeProbes, State->GeometryBoxes, Origin);
    const IMCFCoverageResult OldListenerCoverage = IMCFCheckCoverage(IMCFLwp, UE_ARRAY_COUNT(IMCFLwp), State->BakeProbes, State->GeometryBoxes, Origin);
    const IMCFCoverageResult OldSourceCoverage = IMCFCheckCoverage(IMCFSwp, UE_ARRAY_COUNT(IMCFSwp), State->BakeProbes, State->GeometryBoxes, Origin);
    // Occlusion negative control. The bake keeps every probe inside the open
    // room, so no real probe pair is separated by the slab; both lanes use the
    // same open lower-room lane (>=1.4 m from every baked probe) and differ
    // only in whether two phantom probes of the baked radius sit behind the
    // upper slab (must be rejected) or in the open air (must be accepted).
    static const IMCFV3 OcclusionLane[] = { { -2.0, 2.9, 0.0 }, { -1.0, 2.9, 0.0 } };
    const double PhantomRadius = State->BakeProbes.IsEmpty() ? 1.0 : State->BakeProbes[0].Radius;
    TArray<IMCFProbe> OccludedProbes = State->BakeProbes, VisibleProbes = State->BakeProbes;
    OccludedProbes.Add({ IMCFRouteToSDK({ -2.0, 3.7, 0.0 }, Origin), PhantomRadius });
    OccludedProbes.Add({ IMCFRouteToSDK({ -1.0, 3.7, 0.0 }, Origin), PhantomRadius });
    VisibleProbes.Add({ IMCFRouteToSDK({ -2.0, 2.1, 0.0 }, Origin), PhantomRadius });
    VisibleProbes.Add({ IMCFRouteToSDK({ -1.0, 2.1, 0.0 }, Origin), PhantomRadius });
    const IMCFCoverageResult OccludedLaneCoverage = IMCFCheckCoverage(OcclusionLane, UE_ARRAY_COUNT(OcclusionLane), OccludedProbes, State->GeometryBoxes, Origin);
    const IMCFCoverageResult VisibleLaneCoverage = IMCFCheckCoverage(OcclusionLane, UE_ARRAY_COUNT(OcclusionLane), VisibleProbes, State->GeometryBoxes, Origin);
    const bool OccludedLaneRejected = !OccludedLaneCoverage.Pass && OccludedLaneCoverage.Uncovered.IsEmpty() && !OccludedLaneCoverage.BlockedOnly.IsEmpty();
    const bool VisibleLaneAccepted = VisibleLaneCoverage.Pass && VisibleLaneCoverage.MinVisibleWitness >= 1;
    static const IMCFV3 PenetratingRoute[] = { { 0.0, 1.5, 0.0 }, { 0.0, 4.7, 0.0 } };
    const IMCFGeometryResult PenetratingGeometry = IMCFCheckGeometry(PenetratingRoute, UE_ARRAY_COUNT(PenetratingRoute), State->GeometryBoxes);
    const bool NegativeControlsPass = !OldListenerCoverage.Uncovered.IsEmpty() && !OldSourceCoverage.Uncovered.IsEmpty() && !PenetratingGeometry.Pass
      && OccludedLaneRejected && VisibleLaneAccepted;
    State->QualificationPassed = ListenerGeometry.Pass && SourceGeometry.Pass && ListenerTreads && SourceTreads
      && ListenerCoverage.Pass && SourceCoverage.Pass && NegativeControlsPass;
    auto Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("coordinate_space"), TEXT("fixture metres for routes and geometry boxes; probes_m arrive in SDK metres and are converted with the inverse of IMToSDKPosition"));
    Root->SetStringField(TEXT("certification_rule"), TEXT("IMAcousticSimulation.cpp:687-729: overlaps>0 && blocked<overlaps && blocked<8; uncertain containment counts blocked; single-sample occlusion raycast against the fixture boxes"));
    Root->SetNumberField(TEXT("qualification_sample_step_m"), IMCFQualificationSampleM);
    Root->SetNumberField(TEXT("geometry_margin_m"), IMCFQualificationMarginM);
    Root->SetNumberField(TEXT("coverage_gap_tolerance_m"), IMCFCoverageGapM);
    Root->SetArrayField(TEXT("probe_origin_cm"), IMCFJsonNumbers({ Origin.X, Origin.Y, Origin.Z }));
    Root->SetNumberField(TEXT("probe_count"), State->BakeProbes.Num());
    auto Geometry = MakeShared<FJsonObject>();
    Geometry->SetBoolField(TEXT("pass"), ListenerGeometry.Pass && SourceGeometry.Pass && ListenerTreads && SourceTreads);
    Geometry->SetBoolField(TEXT("listener_route_no_geometry_intersection"), ListenerGeometry.Pass);
    Geometry->SetBoolField(TEXT("source_route_no_geometry_intersection"), SourceGeometry.Pass);
    Geometry->SetNumberField(TEXT("listener_blocked_segments"), ListenerGeometry.BlockedSegments);
    Geometry->SetNumberField(TEXT("source_blocked_segments"), SourceGeometry.BlockedSegments);
    Geometry->SetBoolField(TEXT("listener_all_treads_in_order"), ListenerTreads);
    Geometry->SetBoolField(TEXT("source_all_treads_in_order"), SourceTreads);
    Root->SetObjectField(TEXT("geometry"), Geometry);
    Root->SetObjectField(TEXT("listener_coverage"), CoverageJson(ListenerCoverage));
    Root->SetObjectField(TEXT("source_coverage"), CoverageJson(SourceCoverage));
    auto Negative = MakeShared<FJsonObject>();
    auto OldListener = CoverageJson(OldListenerCoverage); OldListener->SetBoolField(TEXT("expected_rejected"), !OldListenerCoverage.Uncovered.IsEmpty());
    auto OldSource = CoverageJson(OldSourceCoverage); OldSource->SetBoolField(TEXT("expected_rejected"), !OldSourceCoverage.Uncovered.IsEmpty());
    auto Penetrating = MakeShared<FJsonObject>();
    Penetrating->SetBoolField(TEXT("geometry_pass"), PenetratingGeometry.Pass);
    Penetrating->SetNumberField(TEXT("blocked_segments"), PenetratingGeometry.BlockedSegments);
    Penetrating->SetBoolField(TEXT("expected_rejected"), !PenetratingGeometry.Pass);
    auto OccludedLane = CoverageJson(OccludedLaneCoverage); OccludedLane->SetBoolField(TEXT("expected_rejected"), true);
    auto VisibleLane = CoverageJson(VisibleLaneCoverage); VisibleLane->SetBoolField(TEXT("expected_accepted"), true);
    Negative->SetObjectField(TEXT("old_listener_route_coverage"), OldListener);
    Negative->SetObjectField(TEXT("old_source_route_coverage"), OldSource);
    Negative->SetObjectField(TEXT("penetrating_geometry_route"), Penetrating);
    Negative->SetObjectField(TEXT("occluded_probe_lane_coverage"), OccludedLane);
    Negative->SetObjectField(TEXT("visible_probe_lane_coverage"), VisibleLane);
    Negative->SetBoolField(TEXT("all_negative_controls_pass"), NegativeControlsPass);
    Root->SetObjectField(TEXT("negative_controls"), Negative);
    Root->SetBoolField(TEXT("qualification_passed"), State->QualificationPassed);
    State->QualificationJson = Root;
    if (!WriteQualification(E)) return false;
    if (!State->QualificationPassed) { E = TEXT("Cross-floor fixture invalid: continuity not judged."); return false; }
    return true;
  }
  void SubmitTraversePose(double T, bool Record)
  {
    const double K = FMath::Clamp(T / IMCFTraverse, 0.0, 1.0);
    const IMCFV3 S = IMCFWpLerp(IMCFSourceRoute, UE_ARRAY_COUNT(IMCFSourceRoute), K);
    const IMCFV3 L = IMCFWpLerp(IMCFListenerRoute, UE_ARRAY_COUNT(IMCFListenerRoute), K);
    if (Listener.IsValid()) Listener->SetAudioListenerOverride(nullptr, IMCFToUE(L), FRotator(0, -90, 0));
    if (SrcAudio.IsValid()) SrcAudio->SetWorldLocation(IMCFToUE(S));
    if (Record && (State->LastTraverseSampleT < 0 || T - State->LastTraverseSampleT >= 0.5 || T >= IMCFTraverse))
    {
      State->TraversePositions.Add({ FMath::Clamp(T, 0.0, IMCFTraverse), S, L });
      State->LastTraverseSampleT = T;
    }
  }
  void PollSweepDegraded()
  {
    if (!Volume.IsValid()) return;
    const FString Status = Volume->Status;
    if (Status.StartsWith(TEXT("V2 degraded:"), ESearchCase::IgnoreCase))
    {
      if (Status != State->LastDegradedStatus)
      {
        ++State->SweepDegradedEvents; State->LastDegradedStatus = Status;
        if (State->FirstDegradedStatus.IsEmpty()) State->FirstDegradedStatus = Status;
        UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorQualificationDegraded event=%d status=%s"), State->SweepDegradedEvents, *Status);
      }
    }
    else State->LastDegradedStatus.Reset();
  }
  bool ResetSource(UWorld* W, FString& E)
  {
    if (auto* Old = SrcAudio.Get()) if (auto* Owner = Old->GetOwner()) Owner->Destroy();
    SrcAudio.Reset(); Wave.Reset(); FeedCursor = 0;
    UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorQualificationSourceReset"));
    return SpawnSource(W, E);
  }
  bool CheckTraversePositions() const
  {
    if (State->TraversePositions.IsEmpty()) return false;
    for (const IMCFPositionSample& Sample : State->TraversePositions)
    {
      const double K = Sample.T / IMCFTraverse;
      const IMCFV3 ExpectedS = IMCFWpLerp(IMCFSourceRoute, UE_ARRAY_COUNT(IMCFSourceRoute), K);
      const IMCFV3 ExpectedL = IMCFWpLerp(IMCFListenerRoute, UE_ARRAY_COUNT(IMCFListenerRoute), K);
      if (FMath::Sqrt(IMCFDistanceSq(Sample.Source, ExpectedS)) > IMCFPositionToleranceM
        || FMath::Sqrt(IMCFDistanceSq(Sample.Listener, ExpectedL)) > IMCFPositionToleranceM) return false;
    }
    return true;
  }
  bool Fixture(FString& E)
  {
    if (FPackageName::DoesPackageExist(State->MapPath)) { E = TEXT("Refusing to overwrite an existing cross-floor fixture."); return false; }
    UWorld* W = FAutomationEditorCommonUtils::CreateNewMap();
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!W || !Cube || !Cube->GetMaterial(0)) { E = TEXT("Cannot create cross-floor world/engine cube/material."); return false; }
    auto Box = [&](IMCFV3 Lo, IMCFV3 Hi)
    {
      if (!IMCFBox(W, Cube, Lo, Hi, State->Identity)) return false;
      State->GeometryBoxes.Add({ Lo, Hi });
      return true;
    };
    if (!Box({ -3.1, -0.1, -3.1 }, { 3.1, 0, 3.1 })
      || !Box({ -3.1, 6.0, -3.1 }, { 3.1, 6.2, 3.1 })
      || !Box({ -3.1, 0, -3.1 }, { -3.0, 3.0, 3.1 })
      || !Box({ 3.0, 0, -3.1 }, { 3.1, 3.0, 3.1 })
      || !Box({ -3.0, 0, -3.1 }, { 3.0, 3.0, -3.0 })
      || !Box({ -3.0, 0, 3.0 }, { 3.0, 3.0, 3.1 })
      || !Box({ -3.1, 3.2, -3.1 }, { -3.0, 6.0, 3.1 })
      || !Box({ 3.0, 3.2, -3.1 }, { 3.1, 6.0, 3.1 })
      || !Box({ -3.0, 3.2, -3.1 }, { 3.0, 6.0, -3.0 })
      || !Box({ -3.0, 3.2, 3.0 }, { 3.0, 6.0, 3.1 })
      || !Box({ -3.1, 3.0, -3.1 }, { 0.5, 3.2, 3.1 })
      || !Box({ 1.5, 3.0, -3.1 }, { 3.1, 3.2, 3.1 })
      || !Box({ 0.5, 0, 0.5 }, { 1.5, 0.8, 1.0 })
      || !Box({ 0.5, 0, 0.0 }, { 1.5, 1.6, 0.5 })
      || !Box({ 0.5, 0, -0.5 }, { 1.5, 2.4, 0.0 })
      || !Box({ 0.5, 0, -1.0 }, { 1.5, 3.2, -0.5 }))
    { E = TEXT("Cannot spawn cross-floor geometry."); return false; }
    Volume = W->SpawnActor<AIMAcousticBakeVolume>();
    if (!Volume.IsValid()) { E = TEXT("Cannot spawn cross-floor BakeVolume."); return false; }
    Volume->SetActorLocation(FVector(0, 0, 310));
    Volume->BakeBounds->SetBoxExtent(FVector(320, 320, 350));
    Volume->ProbeSpacingCm = 100; Volume->ProbeHeightCm = 150;
    FIMAcousticMaterialMapping M; M.Material = Cube->GetMaterial(0);
    M.Absorption = FVector(.25); M.Scattering = .5f; Volume->Materials.Add(M);
    Volume->bEnableV2 = true; Volume->bDirectRoute = true; Volume->bPathRoute = true;
    Volume->bReverbRoute = false;
    IFileManager::Get().MakeDirectory(*State->Directory, true);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FPackageName::LongPackageNameToFilename(State->MapPath, FPackageName::GetMapPackageExtension())), true);
    Rpt = MakeShared<FJsonObject>();
    Rpt->SetStringField(TEXT("scope"), TEXT("UE-device-crossfloor-fixture"));
    Rpt->SetStringField(TEXT("map"), State->MapPath);
    Rpt->SetNumberField(TEXT("mesh_boxes"), 16);
    if (!FFileHelper::SaveStringToFile(State->Identity, *Ev(TEXT("geometry-identity.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { E = TEXT("Cannot persist fixture identity."); return false; }
    if (!FEditorFileUtils::SaveLevel(W->PersistentLevel, FPackageName::LongPackageNameToFilename(State->MapPath, FPackageName::GetMapPackageExtension()))) { E = TEXT("Cannot save fixture map."); return false; }
    return true;
  }
  bool SpawnSource(UWorld* W, FString& E)
  {
    auto* A = W->SpawnActor<AActor>();
    if (!A) { E = TEXT("Cannot spawn source actor."); return false; }
    auto* C = NewObject<UAudioComponent>(A);
    A->SetRootComponent(C); A->AddInstanceComponent(C);
    C->bAutoActivate = false; C->RegisterComponent();
    C->SetWorldLocation(IMCFToUE({ -2.0, 1.5, -0.5 }));
    auto* Mk = NewObject<UIMAcousticSourceComponent>(A);
    A->AddInstanceComponent(Mk); Mk->AudioComponent = C; Mk->RegisterComponent();
    if (!IM_AcousticTestSupport::ConfigureGraphSource(C, E)) return false;
    SrcAudio = C; Wave.Reset(); FeedPCM.Reset();
    if (!Mk->ValidateSource(E)) return false;
    Bridge = IM_AcousticTestSupport::FindBridge(W);
    if (!Bridge.IsValid()) { E = TEXT("Cross-floor device unavailable."); return false; }
    if (Bridge->SampleRate != IMCFRate) { E = TEXT("Cross-floor gate requires a 48 kHz device."); return false; }
    C->Play(); return true;
  }
  void Feed(bool)
  {
    if (!Wave.IsValid() || FeedPCM.Num() == 0) return;
    static constexpr int32 ChunkSamples = IMCFRate / 2;
    int32 Fed = 0;
    while (Wave->GetAvailableAudioByteCount() < ChunkSamples * int32(sizeof(int16)) && Fed < ChunkSamples * 4)
    {
      TArray<int16> P; P.SetNumUninitialized(ChunkSamples);
      for (int32 I = 0; I < ChunkSamples; ++I) P[I] = FeedPCM[(FeedCursor + I) % FeedPCM.Num()];
      Wave->QueueAudio(reinterpret_cast<const uint8*>(P.GetData()), P.Num() * int32(sizeof(int16)));
      FeedCursor = (FeedCursor + ChunkSamples) % FeedPCM.Num(); Fed += ChunkSamples;
    }
  }
  void Snap()
  {
    if (!Bridge.IsValid()) return;
    B.D = Bridge->DirectNonzeroBlocks.load(); B.P = Bridge->PathNonzeroBlocks.load();
    B.R = Bridge->RejectedBlocks.load(); B.RR = Bridge->ReverbRejectedBlocks.load();
    B.Drop = Bridge->DryDroppedBlocks.load(); B.W = Bridge->ReverbNonzeroBlocks.load();
    B.Calls = Bridge->ReverbProcessedBlocks.load(); B.Input = Bridge->ReverbDryBlocks.load();
  }
  IMCFCount Deltas() const
  {
    IMCFCount C;
    if (!Bridge.IsValid()) return C;
    C.D = Bridge->DirectNonzeroBlocks.load() - B.D; C.P = Bridge->PathNonzeroBlocks.load() - B.P;
    C.R = Bridge->RejectedBlocks.load() - B.R; C.RR = Bridge->ReverbRejectedBlocks.load() - B.RR;
    C.Drop = Bridge->DryDroppedBlocks.load() - B.Drop; C.W = Bridge->ReverbNonzeroBlocks.load() - B.W;
    C.Calls = Bridge->ReverbProcessedBlocks.load() - B.Calls; C.Input = Bridge->ReverbDryBlocks.load() - B.Input;
    return C;
  }
  double Delta(int32 K) const
  {
    IMCFCount C = Deltas();
    return double(K == 0 ? C.D : K == 1 ? C.P : K == 2 ? C.R : C.W);
  }
  void Rec(const TCHAR* N, double En, double Bal, FString& E)
  {
    IMCFCount C = Deltas();
    auto O = MakeShared<FJsonObject>();
    O->SetNumberField(TEXT("energy"), En); O->SetNumberField(TEXT("ear_balance"), Bal);
    O->SetNumberField(TEXT("d_direct"), double(C.D)); O->SetNumberField(TEXT("d_path"), double(C.P));
    O->SetNumberField(TEXT("d_rejected"), double(C.R)); O->SetNumberField(TEXT("d_reverb_rejected"), double(C.RR));
    O->SetNumberField(TEXT("d_dry_dropped"), double(C.Drop)); O->SetNumberField(TEXT("d_wet"), double(C.W));
    O->SetNumberField(TEXT("d_processed"), double(C.Calls)); O->SetNumberField(TEXT("d_dry"), double(C.Input));
    O->SetNumberField(TEXT("max_gt_gap_us"), Bridge.IsValid() ? double(Bridge->MaxSnapshotGapUs.load()) : -1);
    O->SetNumberField(TEXT("max_worker_gap_us"), Bridge.IsValid() ? double(Bridge->MaxWorkerGapUs.load()) : -1);
    if (FCString::Strcmp(N, TEXT("traverse")) == 0)
    {
      O->SetNumberField(TEXT("duration_seconds"), IMCFTraverse);
      O->SetNumberField(TEXT("position_sample_count"), State->TraversePositions.Num());
      O->SetBoolField(TEXT("position_consistent"), State->TraversePositionsMatch);
      O->SetNumberField(TEXT("position_tolerance_m"), IMCFPositionToleranceM);
      TArray<TSharedPtr<FJsonValue>> Positions;
      for (const IMCFPositionSample& Sample : State->TraversePositions)
      {
        auto P = MakeShared<FJsonObject>();
        P->SetNumberField(TEXT("t_seconds"), Sample.T);
        P->SetArrayField(TEXT("source_fixture_m"), IMCFJsonNumbers({ Sample.Source.X, Sample.Source.Y, Sample.Source.Z }));
        P->SetArrayField(TEXT("listener_fixture_m"), IMCFJsonNumbers({ Sample.Listener.X, Sample.Listener.Y, Sample.Listener.Z }));
        Positions.Add(MakeShared<FJsonValueObject>(P));
      }
      O->SetArrayField(TEXT("positions"), Positions);
    }
    if (!IMCFJson(Ev(*(FString(N) + TEXT(".json"))), O)) E = TEXT("Cannot save point report.");
    Points.Add(FString(N));
  }
  bool Analyze(const FString& F, double& En, double& Bal, FString& E, int32* OutFrames = nullptr)
  {
    TArray<uint8> By; FWaveModInfo Info;
    if (!FFileHelper::LoadFileToArray(By, *F) || !Info.ReadWaveInfo(By.GetData(), By.Num())
      || !Info.pBitsPerSample || *Info.pBitsPerSample != 16 || !Info.pChannels || *Info.pChannels != 2
      || !Info.pSamplesPerSec || *Info.pSamplesPerSec != IMCFRate || Info.SampleDataSize % 4)
    { E = TEXT("Capture WAV must be 48 kHz stereo PCM16."); return false; }
    const int32 Fr = Info.SampleDataSize / 4;
    if (Fr < IMCFRate) { E = TEXT("Capture recording shorter than 1s."); return false; }
    if (OutFrames) *OutFrames = Fr;
    En = 0; double Le = 0, Ri = 0;
    for (int32 I = 0; I < Fr; ++I)
    {
      int16 LR[2]; FMemory::Memcpy(LR, Info.SampleDataStart + I * 4, sizeof(LR));
      Le += double(LR[0]) * LR[0]; Ri += double(LR[1]) * LR[1];
    }
    En = Le + Ri; Bal = En > 0 ? (Le - Ri) / En : 0;
    FString EJ = FString::Printf(TEXT("{\"energy\":%.17g,\"ear_balance\":%.9g,\"frames\":%d}"), En, Bal, Fr);
    if (!FFileHelper::SaveStringToFile(EJ, *FPaths::ChangeExtension(F, TEXT(".energy.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { E = TEXT("Cannot save energy report."); return false; }
    return true;
  }
  bool WavStable()
  {
    const int64 By = IFileManager::Get().FileSize(*WavWait);
    if (By <= 44 || By != WavLast) { WavLast = By; return false; }
    return true;
  }
  bool Fail(const FString& M) { return Done(false, M); }
  bool Done(bool Pass, const FString& M)
  {
    if (IMCFPIE()) GUnrealEd->RequestEndPlayMap();
    auto T = MakeShared<FJsonObject>();
    T->SetBoolField(TEXT("passed"), Pass); T->SetStringField(TEXT("message"), M);
    T->SetNumberField(TEXT("stage"), Stage); T->SetNumberField(TEXT("captures_completed"), Points.Num());
    if (!IMCFJson(FPaths::Combine(State->Directory, TEXT("terminal.json")), T)) Pass = false;
    if (!Pass) State->Test->AddError(M); else State->Test->AddInfo(M);
    State->Restore();
    UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloor %s %s evidence=%s"), Pass ? TEXT("PASS") : TEXT("FAIL"), *M, *State->Directory);
    UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticCrossFloorMove %s"), Pass ? TEXT("PASS") : TEXT("FAIL"));
    UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"), Pass ? TEXT("PASS") : TEXT("FAIL"));
    return true;
  }
  TSharedRef<IMCFState> State;
  int32 Stage; double T0, SegT0 = 0, TravT0 = 0, SweepT0 = 0;
  uint64 SweepNotFresh0 = 0, SweepReverbRejected0 = 0, SweepInput0 = 0, SweepRenderInput0 = 0;
  uint64 SweepReverbNonzero0 = 0, SweepDry0 = 0;
  TSharedPtr<FJsonObject> Rpt, BakeJson;
  TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
  TWeakObjectPtr<APlayerController> Listener;
  TWeakObjectPtr<UAudioComponent> SrcAudio;
  TWeakObjectPtr<USoundWaveProcedural> Wave;
  TSharedPtr<IM_AcousticDeviceBridge, ESPMode::ThreadSafe> Bridge;
  IMCFCount B;
  TArray<int16> FeedPCM; int32 FeedCursor = 0; int64 WavLast = -1;
  FString WavWait;
  TArray<FString> Points;
  double BalA = 0;
  // Spawn-race watchdog + direct-readiness diagnostics (test-side only).
  uint64 CFIn0 = 0; double CFT0 = 0; int32 CFRe = 0; bool CFInit = false;
  double CFDiagT = 0;
  // Rolling 1 s pump-activity probe for the sweep readiness gate (test-side only).
  uint64 PumpProbeBase = 0; double PumpProbeT = 0; bool PumpAlive = false;
  double PumpDiagT = 0;
  // Bounded first-rejection diagnostic counters (see the Stage >= 11 block).
  bool CFDiagInit = false; uint64 CFDiagRej = 0; int32 CFDiagLogs = 0;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticCrossFloorMove, "IceMoon.AcousticField.W3.CrossFloorMove",
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool IM_AcousticCrossFloorMove::RunTest(const FString&)
{
  if (IMCFPIE()) { AddError(TEXT("CrossFloorMove requires an owned idle Editor, not an existing PIE.")); return false; }
  auto S = MakeShared<IMCFState>(this);
  IFileManager::Get().MakeDirectory(*S->Directory, true);
  UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticCrossFloorMove START evidence=%s"), *S->Directory);
  FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<IM_AcousticCrossFloorCommand>(S)); return true;
}
#endif
