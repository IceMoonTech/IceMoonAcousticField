#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "IMAcousticSimulation.h"

// H1 W2 mechanism proof (not an acoustic claim): SyncDynamicMeshes accepts,
// moves and destroys an instanced door panel with exact readback, and every
// malformed payload fails closed with the cause named. Mirrors the W1 sync
// test shape: real Bake + Load, one JSON receipt, controller log lines.
namespace IMAcousticW2SyncTestPrivate
{
IPLCoordinateSpace3 H2DMFrame(float OX, float OY, float OZ)
{
	IPLCoordinateSpace3 F{};
	F.right = {1.0f, 0.0f, 0.0f};
	F.up = {0.0f, 0.0f, 1.0f};
	F.ahead = {0.0f, 1.0f, 0.0f};
	F.origin = {OX, OY, OZ};
	return F;
}
void H2DMBoxRoom(FIMAcousticSceneInput& Out)
{
	// Closed box x in [-3,3], y in [-2,2], z in [0,3], outward winding, meters.
	Out.Vertices = {{-3,-2,0},{3,-2,0},{3,2,0},{-3,2,0},{-3,-2,3},{3,-2,3},{3,2,3},{-3,2,3}};
	const int T[][3] = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,5,4},{0,1,5},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
	for (const auto& R : T) { IPLTriangle Tr{}; Tr.indices[0]=R[0]; Tr.indices[1]=R[1]; Tr.indices[2]=R[2]; Out.Triangles.push_back(Tr); }
	IPLMaterial M{};
	M.absorption[0]=0.2f; M.absorption[1]=0.25f; M.absorption[2]=0.3f;
	M.scattering=0.1f;
	Out.Materials.push_back(M);
	Out.MaterialIndices.assign(Out.Triangles.size(), 0);
	Out.Probes = {{{-1.0f,0.0f,1.5f},0.6f},{{1.0f,0.0f,1.5f},0.6f}};
}
void H2DMPanel(FIMAcousticSceneInput& Out)
{
	// Thin door panel x in [-0.05,0.05], y in [-1,1], z in [0,2], meters.
	Out.Vertices = {{-0.05f,-1,0},{0.05f,-1,0},{0.05f,1,0},{-0.05f,1,0},{-0.05f,-1,2},{0.05f,-1,2},{0.05f,1,2},{-0.05f,1,2}};
	const int T[][3] = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,5,4},{0,1,5},{2,3,7},{2,7,6},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
	for (const auto& R : T) { IPLTriangle Tr{}; Tr.indices[0]=R[0]; Tr.indices[1]=R[1]; Tr.indices[2]=R[2]; Out.Triangles.push_back(Tr); }
	IPLMaterial M{};
	M.absorption[0]=0.1f; M.absorption[1]=0.1f; M.absorption[2]=0.1f;
	M.scattering=0.5f;
	Out.Materials.push_back(M);
	Out.MaterialIndices.assign(Out.Triangles.size(), 0);
}
IPLMatrix4x4 H2DMTranslate(float X, float Y, float Z)
{
	// Affine only: last row (0,0,0,1), translation in column 3 (C API order).
	IPLMatrix4x4 M{};
	M.elements[0][0]=1.0f; M.elements[1][1]=1.0f; M.elements[2][2]=1.0f; M.elements[3][3]=1.0f;
	M.elements[0][3]=X; M.elements[1][3]=Y; M.elements[2][3]=Z;
	return M;
}
FString H2DMDynJson(uint64 Key, uint64 Hash, float TX, float TY, float TZ)
{
	return FString::Printf(TEXT("{\"key\":%llu,\"hash\":%llu,\"tx\":%.6g,\"ty\":%.6g,\"tz\":%.6g}"), Key, Hash, TX, TY, TZ);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticDynamicSyncTest, "IceMoon.AcousticField.H1.DynamicSync", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FIMAcousticDynamicSyncTest::RunTest(const FString&)
{
	std::string Error;
	FIMAcousticSimulation Sim;
	// 1. Unloaded runtime fails closed and names Load.
	FIMAcousticDynamicMeshInput Early;
	Early.Key = 11; Early.GeometryHash = 1001; IMAcousticW2SyncTestPrivate::H2DMPanel(Early.Geometry); Early.Transform = IMAcousticW2SyncTestPrivate::H2DMTranslate(0.0f, 0.0f, 0.0f);
	if (Sim.SyncDynamicMeshes({Early}, Error)) { AddError(TEXT("Sync without Load must fail.")); return false; }
	const FString UnloadedErr = ANSI_TO_TCHAR(Error.c_str());
	if (!UnloadedErr.Contains(TEXT("Load"))) { AddError(FString::Printf(TEXT("Unloaded gate must name Load, got: %s"), *UnloadedErr)); return false; }
	// 2. Hybrid options + real Bake + Load (same room as W1).
	if (!Sim.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), Error)) { AddError(ANSI_TO_TCHAR(Error.c_str())); return false; }
	FIMAcousticSceneInput Scene; IMAcousticW2SyncTestPrivate::H2DMBoxRoom(Scene);
	FIMAcousticBakeData Bake;
	if (!Sim.Bake(Scene, Bake, Error)) { AddError(FString::Printf(TEXT("Synthetic bake failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	if (!Sim.Load(Bake, 48000, 512, Error)) { AddError(FString::Printf(TEXT("Load failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	// 3. Malformed payloads fail closed; state unchanged (readback empty).
	FIMAcousticDynamicMeshInput Zero = Early; Zero.Key = 0;
	if (Sim.SyncDynamicMeshes({Zero}, Error)) { AddError(TEXT("Zero key must be rejected.")); return false; }
	const FString ZeroErr = ANSI_TO_TCHAR(Error.c_str());
	if (!ZeroErr.Contains(TEXT("nonzero"))) { AddError(FString::Printf(TEXT("Zero-key gate must say nonzero, got: %s"), *ZeroErr)); return false; }
	FIMAcousticDynamicMeshInput Dup = Early;
	if (Sim.SyncDynamicMeshes({Early, Dup}, Error)) { AddError(TEXT("Duplicate keys must be rejected.")); return false; }
	const FString DupErr = ANSI_TO_TCHAR(Error.c_str());
	if (!DupErr.Contains(TEXT("duplicate"))) { AddError(FString::Printf(TEXT("Duplicate gate must say duplicate, got: %s"), *DupErr)); return false; }
	FIMAcousticDynamicMeshInput Empty = Early; Empty.Geometry = FIMAcousticSceneInput{};
	if (Sim.SyncDynamicMeshes({Empty}, Error)) { AddError(TEXT("Empty geometry must be rejected.")); return false; }
	const FString EmptyErr = ANSI_TO_TCHAR(Error.c_str());
	if (!Sim.GetDynamicMeshReadback().empty()) { AddError(TEXT("Failed syncs must not change state.")); return false; }
	// 4. Create: readback echoes key/hash/transform exactly.
	if (!Sim.SyncDynamicMeshes({Early}, Error)) { AddError(FString::Printf(TEXT("Initial sync failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	auto Rb = Sim.GetDynamicMeshReadback();
	if (Rb.size() != 1 || Rb[0].Key != 11 || Rb[0].GeometryHash != 1001
		|| Rb[0].Transform.elements[0][3] != 0.0f || Rb[0].Transform.elements[1][3] != 0.0f || Rb[0].Transform.elements[2][3] != 0.0f)
	{ AddError(TEXT("Create readback must echo key/hash/transform.")); return false; }
	const FString InitJson = IMAcousticW2SyncTestPrivate::H2DMDynJson(Rb[0].Key, Rb[0].GeometryHash, Rb[0].Transform.elements[0][3], Rb[0].Transform.elements[1][3], Rb[0].Transform.elements[2][3]);
	// 5. Move: 20 frames across x in [-1,1]; readback follows every frame.
	// Per-frame time is recorded only (no perf gate in H1).
	FString FramesJson;
	for (int Frame = 0; Frame < 20; ++Frame)
	{
		const float X = -1.0f + 2.0f * float(Frame) / 19.0f;
		FIMAcousticDynamicMeshInput Moved = Early; Moved.Transform = IMAcousticW2SyncTestPrivate::H2DMTranslate(X, 0.0f, 0.0f);
		const uint64 T0 = FPlatformTime::Cycles64();
		if (!Sim.SyncDynamicMeshes({Moved}, Error)) { AddError(FString::Printf(TEXT("Move frame %d failed: %s"), Frame, ANSI_TO_TCHAR(Error.c_str()))); return false; }
		const double Us = double(FPlatformTime::Cycles64() - T0) * FPlatformTime::GetSecondsPerCycle() * 1.0e6;
		Rb = Sim.GetDynamicMeshReadback();
		if (Rb.size() != 1 || Rb[0].Transform.elements[0][3] != X) { AddError(FString::Printf(TEXT("Move frame %d readback must follow transform."), Frame)); return false; }
		FramesJson += FString::Printf(TEXT("%s{\"x\":%.6g,\"us\":%.6g}"), Frame ? TEXT(",") : TEXT(""), X, Us);
	}
	// 6. Same key, new hash recreates with the new hash in readback.
	FIMAcousticDynamicMeshInput Rehashed = Early; Rehashed.GeometryHash = 2002;
	if (!Sim.SyncDynamicMeshes({Rehashed}, Error)) { AddError(FString::Printf(TEXT("Rehash sync failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	Rb = Sim.GetDynamicMeshReadback();
	if (Rb.size() != 1 || Rb[0].GeometryHash != 2002) { AddError(TEXT("Rehash readback must show the new hash.")); return false; }
	// 7. Batch still evaluates with the door present (mechanism only; acoustic verdicts belong to W3).
	std::vector<FIMAcousticSourceInput> In(1);
	In[0].SourceKey = 7; In[0].Generation = 1; In[0].Source = IMAcousticW2SyncTestPrivate::H2DMFrame(2.0f, 0.0f, 1.5f);
	std::vector<FIMAcousticAudioFrame> OutFrames;
	if (!Sim.EvaluateBatch(In, IMAcousticW2SyncTestPrivate::H2DMFrame(-2.0f, 0.0f, 1.5f), OutFrames, Error)) { AddError(FString::Printf(TEXT("EvaluateBatch with door failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	const int WithDoorDirect = OutFrames[0].DirectValid ? 1 : 0;
	const int WithDoorPath = OutFrames[0].PathValid ? 1 : 0;
	// 8. Destroy: empty sync clears readback; batch still evaluates (mesh gone).
	if (!Sim.SyncDynamicMeshes({}, Error)) { AddError(FString::Printf(TEXT("Destroy sync failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	if (!Sim.GetDynamicMeshReadback().empty()) { AddError(TEXT("Destroy must clear readback.")); return false; }
	std::vector<FIMAcousticAudioFrame> OutFrames2;
	if (!Sim.EvaluateBatch(In, IMAcousticW2SyncTestPrivate::H2DMFrame(-2.0f, 0.0f, 1.5f), OutFrames2, Error)) { AddError(FString::Printf(TEXT("EvaluateBatch after destroy failed: %s"), ANSI_TO_TCHAR(Error.c_str()))); return false; }
	// 9. Evidence receipt.
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/H1-UE"), FString::Printf(TEXT("IMCF_W2_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString Json = FString::Printf(TEXT("{\"gate_unloaded\":\"%s\",\"gate_zero_key\":\"%s\",\"gate_duplicate\":\"%s\",\"gate_empty_geometry\":\"%s\",\"create\":%s,\"move_frames\":[%s],\"rehash\":2002,\"destroy_empty\":true,\"batch_with_door\":{\"direct\":%d,\"path\":%d},\"batch_after_destroy\":true}"),
		*UnloadedErr, *ZeroErr, *DupErr, *EmptyErr, *InitJson, *FramesJson, WithDoorDirect, WithDoorPath);
	FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Dir, TEXT("IM_dynamic_readback.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticW2Dynamic create=%s frames=20 destroy_empty=1 dir=%s"), *InitJson, *Dir);
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] AcousticH1DynamicSync PASS"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS"));
	AddInfo(FString::Printf(TEXT("Dynamic sync evidence: %s"), *Dir));
	return true;
}
#endif
