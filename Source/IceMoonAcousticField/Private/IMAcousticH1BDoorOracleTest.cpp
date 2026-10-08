#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

namespace IMAcousticH1BDoorOracleTestPrivate
{
constexpr const TCHAR* Run =
	TEXT("IMCF_W3_CBCDDEBC4908CEC9D1D357B9171DC061");
constexpr double Src[3] = {100.0, -200.0, 150.0};
constexpr double Half[3] = {110.0, 5.0, 135.0};
constexpr double AX[2] = {-250.0, -50.0};
constexpr double AZ[2] = {0.0, 250.0};

struct FIMOBox { double C[3]; double H[3]; };
struct FIMOSeg { double A[3]; double B[3]; };

static bool OSegBox(const FIMOSeg& S, const FIMOBox& B, double& T)
{
	double T0 = 0.0, T1 = 1.0;
	for (int I = 0; I < 3; ++I)
	{
		const double D = S.B[I] - S.A[I];
		if (FMath::Abs(D) < 1e-12)
		{
			const double Lo = B.C[I] - B.H[I];
			const double Hi = B.C[I] + B.H[I];
			if (S.A[I] < Lo || S.A[I] > Hi) return false;
		}
		else
		{
			double U0 = ((B.C[I] - B.H[I]) - S.A[I]) / D;
			double U1 = ((B.C[I] + B.H[I]) - S.A[I]) / D;
			if (U0 > U1) Swap(U0, U1);
			T0 = FMath::Max(T0, U0);
			T1 = FMath::Min(T1, U1);
			if (T0 > T1) return false;
		}
	}
	T = T0;
	return true;
}

static FString OClass(const FIMOSeg& S, const FIMOBox& Door)
{
	double L2 = 0.0;
	for (int I = 0; I < 3; ++I)
		L2 += (S.B[I] - S.A[I]) * (S.B[I] - S.A[I]);
	if (L2 < 1e-12) return TEXT("INVALID");
	double THit = 0.0;
	if (OSegBox(S, Door, THit)) return TEXT("DOOR");
	const double Dy = S.B[1] - S.A[1];
	if (FMath::Abs(Dy) < 1e-12) return TEXT("CLEAR");
	const double T = (0.0 - S.A[1]) / Dy;
	if (T < 0.0 || T > 1.0) return TEXT("CLEAR");
	const double X = S.A[0] + (S.B[0] - S.A[0]) * T;
	const double Z = S.A[2] + (S.B[2] - S.A[2]) * T;
	const bool InX = (X >= AX[0] && X <= AX[1]);
	const bool InZ = (Z >= AZ[0] && Z <= AZ[1]);
	if (InX && InZ) return TEXT("CLEAR");
	return TEXT("WALL");
}

static void OMakeBox(FIMOBox& B, double x, double y, double z,
	double hx, double hy, double hz)
{
	B.C[0] = x; B.C[1] = y; B.C[2] = z;
	B.H[0] = hx; B.H[1] = hy; B.H[2] = hz;
}

static void OMakeSeg(FIMOSeg& S,
	double ax, double ay, double az,
	double bx, double by, double bz)
{
	S.A[0] = ax; S.A[1] = ay; S.A[2] = az;
	S.B[0] = bx; S.B[1] = by; S.B[2] = bz;
}

constexpr double ApC[3] = {-150.0, 0.0, 125.0};

static FString OAp(const double Sx[3], const double Lx[3], const FIMOBox& Door)
{
	FIMOSeg T1, T2;
	OMakeSeg(T1, Sx[0], Sx[1], Sx[2], ApC[0], ApC[1], ApC[2]);
	OMakeSeg(T2, ApC[0], ApC[1], ApC[2], Lx[0], Lx[1], Lx[2]);
	const FString C1 = OClass(T1, Door);
	const FString C2 = OClass(T2, Door);
	if (C1 == TEXT("DOOR") || C2 == TEXT("DOOR")) return TEXT("DOOR");
	if (C1 == TEXT("WALL") || C2 == TEXT("WALL")) return TEXT("WALL");
	if (C1 == TEXT("INVALID") || C2 == TEXT("INVALID")) return TEXT("INVALID");
	return TEXT("CLEAR");
}

class FIMOracleCmd final : public IAutomationLatentCommand
{
public:
	explicit FIMOracleCmd(FAutomationTestBase* T) : Test(T) {}
	bool Update() override
	{
		if (!GEditor || GEditor->PlayWorld)
			return Done(false, TEXT("Need idle Editor."));
		FIMOBox Wall, Shut, Open;
		OMakeBox(Wall, 0, 0, 125, 410, 5, 200);
		OMakeBox(Shut, -150, 0, 125, 110, 5, 135);
		OMakeBox(Open, 150, 0, 125, 110, 5, 135);
		FIMOSeg S1, S2, S3, S4;
		OMakeSeg(S1, -400, -400, 150, -400, 400, 150);
		OMakeSeg(S2, -150, -400, 125, -150, 400, 125);
		OMakeSeg(S3, 100, -200, 150, -150, 0, 125);
		OMakeSeg(S4, 1, 2, 3, 1, 2, 3);
		if (OClass(S1, Open) != TEXT("WALL"))
			return Done(false, TEXT("Control WALL failed."));
		if (OClass(S2, Open) != TEXT("CLEAR"))
			return Done(false, TEXT("Control CLEAR failed."));
		if (OClass(S3, Shut) != TEXT("DOOR"))
			return Done(false, TEXT("Control DOOR failed."));
		if (OClass(S4, Shut) != TEXT("INVALID"))
			return Done(false, TEXT("Control INVALID failed."));
		FIMOBox Parked;
		OMakeBox(Parked, 0, 0, 600, 110, 5, 135);
		FIMOSeg SAW, SAC;
		OMakeSeg(SAW, 100, -200, 150, 100, 0, 150);
		OMakeSeg(SAC, 100, -200, 150, -150, 0, 125);
		if (OClass(SAW, Parked) != TEXT("WALL"))
			return Done(false, TEXT("Control N1 wall-aperture failed."));
		if (OClass(SAC, Shut) != TEXT("DOOR"))
			return Done(false, TEXT("Control N2 closed-aperture failed."));
		const FString Saved = FPaths::ProjectSavedDir();
		const FString H1UE = FPaths::Combine(Saved, TEXT("AcousticV2/H1-UE"));
		const FString RunDir = FPaths::Combine(H1UE, Run);
		FString SumRaw;
		const FString SumPath = FPaths::Combine(RunDir,
			TEXT("IM_probe_summary.json"));
		if (!FFileHelper::LoadFileToString(SumRaw, *SumPath))
			return Done(false, TEXT("Baseline summary missing."));
		TSharedPtr<FJsonObject> Sum;
		TSharedRef<TJsonReader<>> Rd =
			TJsonReaderFactory<>::Create(SumRaw);
		const bool OkJ = FJsonSerializer::Deserialize(Rd, Sum);
		if (!OkJ || !Sum.IsValid())
			return Done(false, TEXT("Summary unparsable."));
		bool bComplete = false;
		if (!Sum->TryGetBoolField(TEXT("complete"), bComplete))
			return Done(false, TEXT("Summary has no flag."));
		if (!bComplete)
			return Done(false, TEXT("Export incomplete; never PASS."));
		const TArray<TSharedPtr<FJsonValue>>* Wins = nullptr;
		if (!Sum->TryGetArrayField(TEXT("route_windows"), Wins))
			return Done(false, TEXT("No windows array."));
		if (Wins->Num() == 0)
			return Done(false, TEXT("Zero windows; refusing."));
		FString CsvRaw;
		const FString CsvPath = FPaths::Combine(RunDir,
			TEXT("IM_probe_snapshots.csv"));
		if (!FFileHelper::LoadFileToString(CsvRaw, *CsvPath))
			return Done(false, TEXT("Snapshots missing."));
		TArray<FString> Lines;
		CsvRaw.ParseIntoArrayLines(Lines);
		if (Lines.Num() < 2)
			return Done(false, TEXT("Snapshots empty."));
		TArray<FString> Head;
		Lines[0].ParseIntoArray(Head, TEXT(","));
		int32 cT = -1, cX = -1, cY = -1, cZ = -1;
		for (int32 H = 0; H < Head.Num(); ++H)
		{
			if (Head[H] == TEXT("captured")) cT = H;
			else if (Head[H] == TEXT("lis_ue_x")) cX = H;
			else if (Head[H] == TEXT("lis_ue_y")) cY = H;
			else if (Head[H] == TEXT("lis_ue_z")) cZ = H;
		}
		if (cT < 0 || cX < 0 || cY < 0 || cZ < 0)
			return Done(false, TEXT("Columns changed."));
		// ---- SPEC output contract (JSON verdict) ----
		FString Json = TEXT("{\n");
		Json += FString::Printf(TEXT("  \"baseline\": \"%s\",\n"), Run);
		Json += TEXT("  \"spec\": \"OracleDoorDiag_20260917/SPEC.md\",\n");
		Json += TEXT("  \"oracle_geometry\": \"aperture-two-segment S->A->L, A=(-150,0,125)UE\",\n");
		Json += TEXT("  \"controls\": {\"wall\": \"WALL\", \"clear\": \"CLEAR\", \"door\": \"DOOR\", \"invalid\": \"INVALID\", \"n1_wall_aperture\": \"WALL\", \"n2_closed_aperture\": \"DOOR\", \"all_pass\": true},\n");
		Json += TEXT("  \"windows\": [\n");
		bool bAllMatch = true;
		double WinEndMax = 0.0;
		int32 N = 0;
		for (const auto& WV : *Wins)
		{
			const TSharedPtr<FJsonObject>* WO = nullptr;
			if (!WV->TryGetObject(WO) || !(*WO).IsValid())
				return Done(false, TEXT("Window malformed."));
			const FString SN = (*WO)->GetStringField(TEXT("state"));
			const double T0 = (*WO)->GetNumberField(TEXT("win_start"));
			const double T1 = (*WO)->GetNumberField(TEXT("win_end"));
			const double DP = (*WO)->GetNumberField(TEXT("d_path"));
			const double EN = (*WO)->GetNumberField(TEXT("energy"));
			const TArray<TSharedPtr<FJsonValue>>* DU = nullptr;
			const bool HasDU = (*WO)->TryGetArrayField(TEXT("door_ue"), DU);
			if (!HasDU || DU->Num() < 3)
				return Done(false, TEXT("door_ue missing."));
			TArray<double> LX, LY, LZ;
			for (int32 L = 1; L < Lines.Num(); ++L)
			{
				TArray<FString> F;
				Lines[L].ParseIntoArray(F, TEXT(","));
				if (F.Num() <= cT || F.Num() <= cX) continue;
				if (F.Num() <= cY || F.Num() <= cZ) continue;
				const double T = FCString::Atod(*F[cT]);
				if (T < T0 || T > T1) continue;
				LX.Add(FCString::Atod(*F[cX]));
				LY.Add(FCString::Atod(*F[cY]));
				LZ.Add(FCString::Atod(*F[cZ]));
			}
			if (LX.Num() == 0)
				return Done(false, TEXT("Window has no samples."));
			LX.Sort(); LY.Sort(); LZ.Sort();
			const int32 M = LX.Num() / 2;
			FIMOSeg S;
			OMakeSeg(S, Src[0], Src[1], Src[2],
				LX[M], LY[M], LZ[M]);
			FIMOBox D;
			OMakeBox(D, (*DU)[0]->AsNumber(),
				(*DU)[1]->AsNumber(), (*DU)[2]->AsNumber(),
				Half[0], Half[1], Half[2]);
			const double Lv[3] = {LX[M], LY[M], LZ[M]};
			const FString Cls = OAp(Src, Lv, D);
			FIMOSeg DS;
			OMakeSeg(DS, Src[0], Src[1], Src[2], Lv[0], Lv[1], Lv[2]);
			const FString DirectCls = OClass(DS, D);
			const bool bShut = SN.Contains(TEXT("closed"));
			const FString Exp = bShut ? TEXT("DOOR") : TEXT("CLEAR");
			const bool bM = Cls.Equals(Exp);
			if (!bM) bAllMatch = false;
			if (T1 > WinEndMax) WinEndMax = T1;
			if (N > 0) Json += TEXT(",\n");
			Json += FString::Printf(TEXT("    {\"state\": \"%s\", \"class\": \"%s\", \"direct_class\": \"%s\", \"expected\": \"%s\", \"match\": %s, \"d_path\": %.0f, \"energy\": %.6g, \"door_ue\": [%.1f, %.1f, %.1f], \"n\": %d}"),
				*SN, *Cls, *DirectCls, *Exp, bM ? TEXT("true") : TEXT("false"), DP, EN,
				(*DU)[0]->AsNumber(), (*DU)[1]->AsNumber(), (*DU)[2]->AsNumber(), LX.Num());
			++N;
		}
		Json += TEXT("\n  ],\n");
		// ---- Fallback: post-window snapshot clusters by door0 (SPEC) ----
		int32 cD0X = -1, cD0Y = -1, cD0Z = -1;
		for (int32 H = 0; H < Head.Num(); ++H)
		{
			if (Head[H] == TEXT("door0_x")) cD0X = H;
			else if (Head[H] == TEXT("door0_y")) cD0Y = H;
			else if (Head[H] == TEXT("door0_z")) cD0Z = H;
		}
		if (cD0X < 0 || cD0Y < 0 || cD0Z < 0)
			return Done(false, TEXT("door0 columns changed."));
		// Group post-window rows by rounded door0 triple.
		TMap<FString, int32> ClusIdx;
		struct FIMClus { FString Key; double D0[3]; TArray<double> LX, LY, LZ; };
		TArray<FIMClus> Clus;
		for (int32 L = 1; L < Lines.Num(); ++L)
		{
			TArray<FString> F;
			Lines[L].ParseIntoArray(F, TEXT(","));
			if (F.Num() <= cT || F.Num() <= cD0Z) continue;
			const double T = FCString::Atod(*F[cT]);
			if (T <= WinEndMax) continue;
			const double X0 = FCString::Atod(*F[cD0X]);
			const double Y0 = FCString::Atod(*F[cD0Y]);
			const double Z0 = FCString::Atod(*F[cD0Z]);
			const FString Key = FString::Printf(TEXT("%.4g,%.4g,%.4g"), X0, Y0, Z0);
			int32* P = ClusIdx.Find(Key);
			int32 C = 0;
			if (P == nullptr) { C = Clus.Num(); ClusIdx.Add(Key, C); FIMClus NC; NC.Key = Key; NC.D0[0] = X0; NC.D0[1] = Y0; NC.D0[2] = Z0; Clus.Add(NC); }
			else C = *P;
			if (F.Num() <= cX || F.Num() <= cY || F.Num() <= cZ) continue;
			Clus[C].LX.Add(FCString::Atod(*F[cX]));
			Clus[C].LY.Add(FCString::Atod(*F[cY]));
			Clus[C].LZ.Add(FCString::Atod(*F[cZ]));
		}
		Json += TEXT("  \"fallback_clusters\": [\n");
		bool bFbMatch = true;
		bool bHaveClosedFb = false;
		for (int32 C = 0; C < Clus.Num(); ++C)
		{
			FIMClus& K = Clus[C];
			if (K.LX.Num() < 3) continue;
			K.LX.Sort(); K.LY.Sort(); K.LZ.Sort();
			const int32 M = K.LX.Num() / 2;
			// door0 SDK -> UE fixture mapping (SPEC fixture frame).
			double UEx = 0, UEy = 0, UEz = 0;
			bool bMapped = false;
			bool bClosedLike = false;
			if (FMath::Abs(K.D0[0] - 0.0) < 1e-6 && FMath::Abs(K.D0[1] + 0.25) < 1e-6 && FMath::Abs(K.D0[2] - 1.5) < 1e-6)
			{ UEx = -150; UEy = 0; UEz = 125; bMapped = true; bClosedLike = true; }
			else if (FMath::Abs(K.D0[0] - 0.0) < 1e-6 && FMath::Abs(K.D0[1] - 4.5) < 1e-6 && FMath::Abs(K.D0[2] - 0.0) < 1e-6)
			{ UEx = 0; UEy = 0; UEz = 600; bMapped = true; bClosedLike = false; }
			FString Cls = TEXT("UNMAPPED");
			FString Exp = bClosedLike ? TEXT("DOOR") : TEXT("CLEAR");
			bool bM = false;
			FString DirectCls = TEXT("-");
			if (bMapped)
			{
				const double Lv[3] = {K.LX[M], K.LY[M], K.LZ[M]};
				FIMOBox D;
				OMakeBox(D, UEx, UEy, UEz, Half[0], Half[1], Half[2]);
				FIMOSeg DS;
				OMakeSeg(DS, Src[0], Src[1], Src[2], Lv[0], Lv[1], Lv[2]);
				DirectCls = OClass(DS, D);
				Cls = OAp(Src, Lv, D);
				bM = Cls.Equals(Exp);
			}
			if (!bM) bFbMatch = false;
			if (bClosedLike) bHaveClosedFb = true;
			if (C > 0) Json += TEXT(",\n");
			Json += FString::Printf(TEXT("    {\"id\": \"fb%d\", \"door0_sdk\": [%s], \"door_ue\": [%.1f, %.1f, %.1f], \"mapped\": %s, \"class\": \"%s\", \"direct_class\": \"%s\", \"expected\": \"%s\", \"match\": %s, \"n\": %d}"),
				C, *K.Key, UEx, UEy, UEz, bMapped ? TEXT("true") : TEXT("false"),
				*Cls, *DirectCls, *Exp, bM ? TEXT("true") : TEXT("false"), K.LX.Num());
		}
		Json += TEXT("\n  ],\n");
		// Product closed-state record (ledger, non-window evidence).
		Json += TEXT("  \"product_closed_record\": {\"d_path\": 107, \"energy\": 3.57823e10, \"source\": \"progress-ledger-20260917-CBCDDEBC\"},\n");
		const bool bOverall = bAllMatch && bFbMatch;
		Json += FString::Printf(TEXT("  \"overall\": \"%s\",\n"), bOverall ? TEXT("PASS") : TEXT("FAIL"));
		if (bOverall)
			Json += TEXT("  \"note\": \"All states matched.\"\n}");
		else if (!bHaveClosedFb && N <= 1)
			Json += TEXT("  \"falsifier\": \"Closed states absent from windows and fallback; W3 closed question unanswered; STOP, no product changes.\"\n}");
		else
			Json += TEXT("  \"falsifier\": \"Aperture-segment expectation refuted for this baseline; negative result recorded; STOP per amend-A stop rule, no product changes.\"\n}");
		Evidence = FPaths::Combine(Saved, TEXT("AcousticV2/H1BOracle"));
		IFileManager::Get().MakeDirectory(*Evidence, true);
		const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Evidence = FPaths::Combine(Evidence, Guid);
		IFileManager::Get().MakeDirectory(*Evidence, true);
		FFileHelper::SaveStringToFile(Json,
			*FPaths::Combine(Evidence, TEXT("oracle_verdict.json")));
		FString Msg;
		if (bOverall) Msg = TEXT("Oracle matched all states.");
		else Msg = TEXT("Oracle aperture MISMATCH: prediction refuted; negative recorded; STOP, no product changes.");
		return Done(bOverall, Msg);
	}
private:
	bool Done(bool bOk, const FString& Msg)
	{
		if (!bOk) Test->AddError(Msg);
		UE_LOG(LogTemp, Display, TEXT("IMLogs Oracle %s"), *Msg);
		UE_LOG(LogTemp, Display, TEXT("Oracle %s"),
			bOk ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] Oracle %s"),
			bOk ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s"),
			bOk ? TEXT("PASS") : TEXT("FAIL"));
		return true;
	}
	FAutomationTestBase* Test;
	FString Evidence;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMOracle,
	"IceMoon.AcousticField.H1B.DoorOracle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FIMOracle::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(IMAcousticH1BDoorOracleTestPrivate::FIMOracleCmd(this));
	return true;
}
#endif
