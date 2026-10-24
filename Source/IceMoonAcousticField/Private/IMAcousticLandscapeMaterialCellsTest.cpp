#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Chaos/HeightField.h"
#include "HAL/FileManager.h"
#include "IMAcousticLandscapeCells.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/UniquePtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace IMAcousticLandscapeMaterialCellsTestPrivate
{
constexpr int32 HeightfieldRows = 3;
constexpr int32 HeightfieldCols = 3;
constexpr int32 HeightfieldQuads = 2;

struct FIMCellContractCase
{
	FString Name;
	FString InputDescription;
	int32 Quads = HeightfieldQuads;
	TArray<uint8> InputMaterialIndices;
	TArray<uint8> ConstructorMaterialIndices;
	TArray<FString> MaterialTablePaths;
	TArray<int32> NullMaterialSlots;
	TArray<uint8> ExpectedCells;
	TArray<uint8> OutputCells;
	FString ExpectedErrorSubstring;
	FString ActualError;
	int32 StoredIndexCount = INDEX_NONE;
	bool bExpectedSuccess = false;
	bool bReturnedSuccess = false;
	bool bOutputMatches = false;
	bool bErrorMatches = false;
	bool bPassed = false;
};

static FString MakeReportPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialDrive"),
		FString::Printf(TEXT("landscape_cell_contract_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

static bool WriteReport(const FString& Path, const TSharedRef<FJsonObject>& Report, FString& Error)
{
	const FString Directory = FPaths::GetPath(Path);
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		Error = TEXT("Could not create the Landscape material-cell evidence directory: ") + Directory;
		return false;
	}

	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Report, Writer))
	{
		Writer->Close();
		Error = TEXT("Could not serialize the Landscape material-cell contract report.");
		return false;
	}
	Writer->Close();
	if (!FFileHelper::SaveStringToFile(
			JsonText, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Error = TEXT("Could not write the Landscape material-cell contract report: ") + Path;
		return false;
	}
	return true;
}

static TUniquePtr<Chaos::FHeightField> MakeHeightfield(const TArray<uint8>& ConstructorMaterialIndices)
{
	TArray<Chaos::FReal> Heights;
	Heights.Reserve(HeightfieldRows * HeightfieldCols);
	Heights.Add(0.0);
	Heights.Add(1.0);
	Heights.Add(2.0);
	Heights.Add(3.0);
	Heights.Add(4.0);
	Heights.Add(5.0);
	Heights.Add(6.0);
	Heights.Add(7.0);
	Heights.Add(8.0);

	TArray<uint8> MaterialIndices = ConstructorMaterialIndices;
	return MakeUnique<Chaos::FHeightField>(MoveTemp(Heights), MoveTemp(MaterialIndices),
		HeightfieldRows, HeightfieldCols, Chaos::FVec3(1.0, 1.0, 1.0));
}

static TArray<TSharedPtr<FJsonValue>> BytesToJson(const TArray<uint8>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const uint8 Value : Values)
	{
		Result.Add(MakeShared<FJsonValueNumber>(static_cast<int32>(Value)));
	}
	return Result;
}

static TSharedRef<FJsonObject> CaseToJson(const FIMCellContractCase& Case)
{
	TArray<TSharedPtr<FJsonValue>> ConstructorIndices;
	for (const uint8 Value : Case.ConstructorMaterialIndices)
	{
		ConstructorIndices.Add(MakeShared<FJsonValueNumber>(static_cast<int32>(Value)));
	}
	TArray<TSharedPtr<FJsonValue>> MaterialPaths;
	for (const FString& Path : Case.MaterialTablePaths)
	{
		MaterialPaths.Add(MakeShared<FJsonValueString>(Path));
	}
	TArray<TSharedPtr<FJsonValue>> NullSlots;
	for (const int32 Slot : Case.NullMaterialSlots)
	{
		NullSlots.Add(MakeShared<FJsonValueNumber>(Slot));
	}

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("case"), Case.Name);
	Json->SetStringField(TEXT("explicit_input"), Case.InputDescription);
	Json->SetNumberField(TEXT("geometry_rows"), HeightfieldRows);
	Json->SetNumberField(TEXT("geometry_cols"), HeightfieldCols);
	Json->SetNumberField(TEXT("quads_argument"), Case.Quads);
	Json->SetArrayField(TEXT("constructor_material_indices"), ConstructorIndices);
	Json->SetArrayField(TEXT("material_indices_under_test"), BytesToJson(Case.InputMaterialIndices));
	Json->SetNumberField(TEXT("stored_index_count_observed"), Case.StoredIndexCount);
	Json->SetArrayField(TEXT("material_table_paths"), MaterialPaths);
	Json->SetArrayField(TEXT("null_material_slots"), NullSlots);
	Json->SetBoolField(TEXT("expected_success"), Case.bExpectedSuccess);
	Json->SetArrayField(TEXT("expected_cells"), BytesToJson(Case.ExpectedCells));
	Json->SetBoolField(TEXT("returned_success"), Case.bReturnedSuccess);
	Json->SetArrayField(TEXT("output_cells"), BytesToJson(Case.OutputCells));
	Json->SetStringField(TEXT("expected_error_substring"), Case.ExpectedErrorSubstring);
	Json->SetStringField(TEXT("actual_error"), Case.ActualError);
	Json->SetBoolField(TEXT("output_matches"), Case.bOutputMatches);
	Json->SetBoolField(TEXT("error_matches"), Case.bErrorMatches);
	Json->SetBoolField(TEXT("passed"), Case.bPassed);
	return Json;
}

static bool RunContract(FAutomationTestBase* Test)
{
	const FString ReportPath = MakeReportPath();
	TStrongObjectPtr<UPhysicalMaterial> PM0(NewObject<UPhysicalMaterial>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UPhysicalMaterial::StaticClass(), TEXT("IMLandscapeCellPM0")), RF_Transient));
	TStrongObjectPtr<UPhysicalMaterial> PM1(NewObject<UPhysicalMaterial>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UPhysicalMaterial::StaticClass(), TEXT("IMLandscapeCellPM1")), RF_Transient));

	const bool bProfilesValid = IsValid(PM0.Get()) && IsValid(PM1.Get()) && PM0.Get() != PM1.Get();
	if (!bProfilesValid)
	{
		Test->AddError(TEXT("Could not create two distinct transient PhysicalMaterial profiles for the Landscape cell contract."));
	}

	TArray<TObjectPtr<UPhysicalMaterial>> ValidMaterialTable;
	ValidMaterialTable.Add(PM0.Get());
	ValidMaterialTable.Add(PM1.Get());
	TArray<TObjectPtr<UPhysicalMaterial>> NullSlotMaterialTable;
	NullSlotMaterialTable.Add(PM0.Get());
	NullSlotMaterialTable.Add(nullptr);
	TArray<TObjectPtr<UPhysicalMaterial>> EmptyMaterialTable;

	TArray<FIMCellContractCase> Cases;
	Cases.Reserve(7);
	auto RunCase = [&](const TCHAR* Name, const TCHAR* Description, const int32 Quads,
		const TArray<uint8>& InputIndices, const TArray<TObjectPtr<UPhysicalMaterial>>& Materials,
		const bool bExpectedSuccess, const TArray<uint8>& ExpectedCells,
		const TCHAR* ExpectedErrorSubstring, const bool bTruncateAfterValidConstruction)
	{
		FIMCellContractCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = Name;
		Case.InputDescription = Description;
		Case.Quads = Quads;
		Case.InputMaterialIndices = InputIndices;
		Case.ConstructorMaterialIndices = bTruncateAfterValidConstruction
			? TArray<uint8>{0, 1, 2, 3}
			: InputIndices;
		Case.ExpectedCells = ExpectedCells;
		Case.ExpectedErrorSubstring = ExpectedErrorSubstring;
		Case.bExpectedSuccess = bExpectedSuccess;
		for (const TObjectPtr<UPhysicalMaterial>& Material : Materials)
		{
			Case.MaterialTablePaths.Add(IsValid(Material.Get()) ? Material->GetPathName() : TEXT("<null>"));
			if (!IsValid(Material.Get()))
			{
				Case.NullMaterialSlots.Add(Case.MaterialTablePaths.Num() - 1);
			}
		}

		TUniquePtr<Chaos::FHeightField> Geometry = MakeHeightfield(Case.ConstructorMaterialIndices);
		if (bTruncateAfterValidConstruction)
		{
			// GeomData is public in UE 5.8. Use a valid constructed 3x3 field,
			// then set its exposed stored buffer to the exact malformed 2-entry input.
			// This exercises the reader without provoking Chaos' constructor ensure.
			Geometry->GeomData.MaterialIndices = InputIndices;
		}

		TArray<uint8> OutputCells;
		OutputCells.Add(99); // Prove every call clears prior output before success or failure.
		FString Error;
		int32 StoredIndexCount = INDEX_NONE;
		Case.bReturnedSuccess = IMReadHeightfieldMaterialCells(*Geometry, Quads,
			MakeArrayView(Materials), OutputCells, Error, &StoredIndexCount);
		Case.StoredIndexCount = StoredIndexCount;
		Case.OutputCells = MoveTemp(OutputCells);
		Case.ActualError = MoveTemp(Error);

		const bool bResultMatches = Case.bReturnedSuccess == Case.bExpectedSuccess;
		Case.bOutputMatches = Case.bExpectedSuccess
			? Case.OutputCells == Case.ExpectedCells
			: Case.OutputCells.IsEmpty();
		Case.bErrorMatches = Case.bExpectedSuccess
			? Case.ActualError.IsEmpty()
			: !Case.ExpectedErrorSubstring.IsEmpty()
				&& Case.ActualError.Contains(Case.ExpectedErrorSubstring, ESearchCase::CaseSensitive);
		Case.bPassed = bProfilesValid && bResultMatches && Case.bOutputMatches && Case.bErrorMatches
			&& Case.StoredIndexCount == InputIndices.Num();
		Test->TestTrue(FString::Printf(TEXT("Landscape cell material contract: %s"), Name), Case.bPassed);
	};

	const TArray<uint8> UniformInput{0};
	const TArray<uint8> UniformExpected{0, 0, 0, 0};
	RunCase(TEXT("uniform_default_index_expands_to_four_cells"),
		TEXT("3x3 geometry, stored material index [0], two valid profiles, expected four zeros."),
		HeightfieldQuads, UniformInput, ValidMaterialTable, true, UniformExpected, TEXT(""), false);

	const TArray<uint8> PerCellInput{0, 1, 255, 0};
	const TArray<uint8> PerCellExpected{0, 1, 255, 0};
	RunCase(TEXT("per_cell_indices_preserve_hole"),
		TEXT("3x3 geometry, explicit per-cell indices [0,1,255,0], expected exact cells and retained hole."),
		HeightfieldQuads, PerCellInput, ValidMaterialTable, true, PerCellExpected, TEXT(""), false);

	RunCase(TEXT("quad_count_must_match_geometry_rows"),
		TEXT("3x3 geometry has two quads per axis, but the helper receives Quads=1."),
		1, PerCellInput, ValidMaterialTable, false, TArray<uint8>(),
		TEXT("dimensions do not match"), false);

	const TArray<uint8> TwoEntryInput{0, 1};
	RunCase(TEXT("two_entry_material_buffer_is_rejected"),
		TEXT("3x3 geometry, explicit malformed stored material buffer [0,1] instead of one or four entries."),
		HeightfieldQuads, TwoEntryInput, ValidMaterialTable, false, TArray<uint8>(),
		TEXT("invalid default/per-cell material index buffer or material table"), true);

	const TArray<uint8> OutOfRangeInput{0, 1, 2, 0};
	RunCase(TEXT("used_material_id_two_exceeds_two_entry_table"),
		TEXT("3x3 geometry, per-cell ID 2 is used while the explicit valid table has only slots 0 and 1."),
		HeightfieldQuads, OutOfRangeInput, ValidMaterialTable, false, TArray<uint8>(),
		TEXT("has no valid material table entry"), false);

	RunCase(TEXT("used_null_physical_material_slot_is_rejected"),
		TEXT("3x3 geometry uses cell ID 1; explicit table slot 1 is null."),
		HeightfieldQuads, PerCellInput, NullSlotMaterialTable, false, TArray<uint8>(),
		TEXT("has no valid material table entry"), false);

	const TArray<uint8> EmptyInput;
	RunCase(TEXT("empty_material_buffer_and_table_are_rejected"),
		TEXT("3x3 geometry has no stored material indices and the explicit material table is empty."),
		HeightfieldQuads, EmptyInput, EmptyMaterialTable, false, TArray<uint8>(),
		TEXT("invalid default/per-cell material index buffer or material table"), false);

	bool bAllCasesPassed = bProfilesValid && Cases.Num() == 7;
	TArray<TSharedPtr<FJsonValue>> CaseRows;
	CaseRows.Reserve(Cases.Num());
	for (const FIMCellContractCase& Case : Cases)
	{
		bAllCasesPassed = bAllCasesPassed && Case.bPassed;
		CaseRows.Add(MakeShared<FJsonValueObject>(CaseToJson(Case)));
	}

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("IMAcousticLandscapeMaterialCellsContract.v1"));
	Report->SetStringField(TEXT("test"), TEXT("IceMoon.AcousticField.PhysicalMaterial.LandscapeCellMaterialContract"));
	Report->SetStringField(TEXT("status"), bAllCasesPassed ? TEXT("PASS") : TEXT("FAIL"));
	Report->SetNumberField(TEXT("geometry_rows"), HeightfieldRows);
	Report->SetNumberField(TEXT("geometry_cols"), HeightfieldCols);
	Report->SetNumberField(TEXT("quads"), HeightfieldQuads);
	Report->SetStringField(TEXT("geometry_source"), TEXT("Constructed Chaos::FHeightField with nine explicit non-uniform heights."));
	Report->SetStringField(TEXT("profile_0"), IsValid(PM0.Get()) ? PM0->GetPathName() : FString());
	Report->SetStringField(TEXT("profile_1"), IsValid(PM1.Get()) ? PM1->GetPathName() : FString());
	Report->SetNumberField(TEXT("case_count"), Cases.Num());
	Report->SetArrayField(TEXT("cases"), CaseRows);

	FString ReportError;
	const bool bReportWritten = WriteReport(ReportPath, Report, ReportError);
	if (bReportWritten)
	{
		Test->AddInfo(FString::Printf(TEXT("Landscape cell material contract report: %s"), *ReportPath));
	}
	else
	{
		Test->AddError(ReportError);
	}
	const bool bFinalPassed = bAllCasesPassed && bReportWritten;
	UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] LandscapeCellMaterialContract %s"),
		bFinalPassed ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s LandscapeCellMaterialContract"),
		bFinalPassed ? TEXT("PASS") : TEXT("FAIL"));
	return bFinalPassed;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticLandscapeMaterialCellsContractTest,
	"IceMoon.AcousticField.PhysicalMaterial.LandscapeCellMaterialContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticLandscapeMaterialCellsContractTest::RunTest(const FString&)
{
	return IMAcousticLandscapeMaterialCellsTestPrivate::RunContract(this);
}

#endif
