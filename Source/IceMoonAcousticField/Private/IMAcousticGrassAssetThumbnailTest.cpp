#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"

namespace IMAcousticGrassAssetThumbnailTestPrivate
{
	static constexpr TCHAR ReceiptRelativePath[] = TEXT("AcousticV2/PhysicalMaterialDrive/pm_grass_inventory_r8_receipt_20261001.json");
	static constexpr TCHAR InventoryRelativePath[] = TEXT("AcousticV2/PhysicalMaterialDrive/grass_source_inventory_A05D0D9B46189BD77CE6F39DA640030E.json");
	static constexpr TCHAR OutputRelativePath[] = TEXT("AcousticV2/PhysicalMaterialDrive/GrassSourceThumbnails");
	static constexpr TCHAR ManifestFilename[] = TEXT("grass_source_saved_thumbnails_manifest.json");

	// SHA-256 is used only to fingerprint package bytes read by this collector.
	// The implementation is local because UE5.8's generic platform SHA-256
	// signature helper is not implemented for all targets.
	class FIMStreamingSHA256
	{
	public:
		FIMStreamingSHA256()
			: State{ 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u }
		{
		}

		void Update(const uint8* Data, int32 Length)
		{
			if (Data == nullptr || Length <= 0)
			{
				return;
			}

			TotalBytes += static_cast<uint64>(Length);
			while (Length > 0)
			{
				const int32 CopyLength = FMath::Min<int32>(static_cast<int32>(UE_ARRAY_COUNT(Block)) - BufferedBytes, Length);
				FMemory::Memcpy(Block + BufferedBytes, Data, CopyLength);
				BufferedBytes += CopyLength;
				Data += CopyLength;
				Length -= CopyLength;
				if (BufferedBytes == static_cast<int32>(UE_ARRAY_COUNT(Block)))
				{
					Transform(Block);
					BufferedBytes = 0;
				}
			}
		}

		FString FinalizeHex()
		{
			const uint64 BitLength = TotalBytes * 8u;
			Block[BufferedBytes++] = 0x80u;
			if (BufferedBytes > 56)
			{
				while (BufferedBytes < static_cast<int32>(UE_ARRAY_COUNT(Block)))
				{
					Block[BufferedBytes++] = 0;
				}
				Transform(Block);
				BufferedBytes = 0;
			}
			while (BufferedBytes < 56)
			{
				Block[BufferedBytes++] = 0;
			}
			for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
			{
				Block[63 - ByteIndex] = static_cast<uint8>(BitLength >> (ByteIndex * 8));
			}
			Transform(Block);

			return FString::Printf(TEXT("%08x%08x%08x%08x%08x%08x%08x%08x"),
				State[0], State[1], State[2], State[3], State[4], State[5], State[6], State[7]);
		}

	private:
		static uint32 RotateRight(uint32 Value, uint32 Amount)
		{
			return (Value >> Amount) | (Value << (32u - Amount));
		}

		void Transform(const uint8* Input)
		{
			static constexpr uint32 RoundConstants[64] =
			{
				0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
				0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
				0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
				0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
				0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
				0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
				0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
				0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
			};

			uint32 Words[64];
			for (uint32 WordIndex = 0; WordIndex < 16; ++WordIndex)
			{
				const uint32 ByteIndex = WordIndex * 4u;
				Words[WordIndex] = (static_cast<uint32>(Input[ByteIndex]) << 24)
					| (static_cast<uint32>(Input[ByteIndex + 1]) << 16)
					| (static_cast<uint32>(Input[ByteIndex + 2]) << 8)
					| static_cast<uint32>(Input[ByteIndex + 3]);
			}
			for (uint32 WordIndex = 16; WordIndex < 64; ++WordIndex)
			{
				const uint32 S0 = RotateRight(Words[WordIndex - 15], 7) ^ RotateRight(Words[WordIndex - 15], 18) ^ (Words[WordIndex - 15] >> 3);
				const uint32 S1 = RotateRight(Words[WordIndex - 2], 17) ^ RotateRight(Words[WordIndex - 2], 19) ^ (Words[WordIndex - 2] >> 10);
				Words[WordIndex] = Words[WordIndex - 16] + S0 + Words[WordIndex - 7] + S1;
			}

			uint32 A = State[0];
			uint32 B = State[1];
			uint32 C = State[2];
			uint32 D = State[3];
			uint32 E = State[4];
			uint32 F = State[5];
			uint32 G = State[6];
			uint32 H = State[7];
			for (uint32 Round = 0; Round < 64; ++Round)
			{
				const uint32 Sum1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32 Choose = (E & F) ^ (~E & G);
				const uint32 Temp1 = H + Sum1 + Choose + RoundConstants[Round] + Words[Round];
				const uint32 Sum0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32 Temp2 = Sum0 + Majority;
				H = G;
				G = F;
				F = E;
				E = D + Temp1;
				D = C;
				C = B;
				B = A;
				A = Temp1 + Temp2;
			}

			State[0] += A;
			State[1] += B;
			State[2] += C;
			State[3] += D;
			State[4] += E;
			State[5] += F;
			State[6] += G;
			State[7] += H;
		}

		uint32 State[8];
		uint8 Block[64] = {};
		int32 BufferedBytes = 0;
		uint64 TotalBytes = 0;
	};

	static bool ComputeFileSHA256(const FString& Filename, FString& OutSHA256, int64& OutSize)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
		if (!Reader || Reader->IsError())
		{
			return false;
		}

		OutSize = Reader->TotalSize();
		if (OutSize < 0)
		{
			return false;
		}

		FIMStreamingSHA256 Hasher;
		TArray<uint8> Scratch;
		Scratch.SetNumUninitialized(1024 * 1024);
		int64 BytesRead = 0;
		while (BytesRead < OutSize)
		{
			const int32 ChunkSize = static_cast<int32>(FMath::Min<int64>(Scratch.Num(), OutSize - BytesRead));
			const int64 PreviousPosition = Reader->Tell();
			Reader->Serialize(Scratch.GetData(), ChunkSize);
			if (Reader->IsError() || Reader->Tell() != PreviousPosition + ChunkSize)
			{
				return false;
			}
			Hasher.Update(Scratch.GetData(), ChunkSize);
			BytesRead += ChunkSize;
		}

		OutSHA256 = Hasher.FinalizeHex();
		return !Reader->IsError() && Reader->Tell() == OutSize;
	}

	static bool SplitMeshObjectPath(const FString& ObjectPath, FString& OutPackageName, FString& OutAssetName)
	{
		int32 DotIndex = INDEX_NONE;
		for (int32 Index = ObjectPath.Len() - 1; Index >= 0; --Index)
		{
			if (ObjectPath[Index] == TEXT('.'))
			{
				DotIndex = Index;
				break;
			}
		}
		if (DotIndex <= 0 || DotIndex >= ObjectPath.Len() - 1)
		{
			return false;
		}

		OutPackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		OutAssetName = ObjectPath.Mid(DotIndex + 1);
		return !OutPackageName.IsEmpty() && !OutAssetName.IsEmpty();
	}

	static FString BytesToHexPrefix(const TArray<uint8>& Bytes, int32 MaxBytes)
	{
		static constexpr TCHAR HexDigits[] = TEXT("0123456789ABCDEF");
		FString Hex;
		const int32 Count = FMath::Min(Bytes.Num(), MaxBytes);
		Hex.Reserve(Count * 2);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Hex.AppendChar(HexDigits[Bytes[Index] >> 4]);
			Hex.AppendChar(HexDigits[Bytes[Index] & 0x0f]);
		}
		return Hex;
	}

	static FString DetectImageMagic(const TArray<uint8>& Bytes)
	{
		static constexpr uint8 PngSignature[8] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
		if (Bytes.Num() >= static_cast<int32>(UE_ARRAY_COUNT(PngSignature))
			&& FMemory::Memcmp(Bytes.GetData(), PngSignature, UE_ARRAY_COUNT(PngSignature)) == 0)
		{
			return TEXT("PNG");
		}
		if (Bytes.Num() >= 3 && Bytes[0] == 0xff && Bytes[1] == 0xd8 && Bytes[2] == 0xff)
		{
			return TEXT("JPEG");
		}
		return Bytes.IsEmpty() ? TEXT("EMPTY") : FString::Printf(TEXT("UNSUPPORTED_HEX_PREFIX_%s"), *BytesToHexPrefix(Bytes, 8));
	}

	static bool WriteRawImageWithoutReplacingDifferentBytes(const FString& Filename, const TArray<uint8>& Bytes, FString& OutWriteState)
	{
		if (IFileManager::Get().FileExists(*Filename))
		{
			TArray<uint8> ExistingBytes;
			if (FFileHelper::LoadFileToArray(ExistingBytes, *Filename) && ExistingBytes == Bytes)
			{
				OutWriteState = TEXT("EXISTING_IDENTICAL_BYTES");
				return true;
			}
			OutWriteState = TEXT("OUTPUT_PATH_CONFLICT_PRESERVED");
			return false;
		}

		if (FFileHelper::SaveArrayToFile(Bytes, *Filename))
		{
			OutWriteState = TEXT("WRITTEN_RAW_BYTES");
			return true;
		}

		IFileManager::Get().Delete(*Filename, false, true);
		OutWriteState = TEXT("WRITE_FAILED");
		return false;
	}

	static TSharedPtr<FJsonObject> MakeMeshRecord(const FString& ObjectPath)
	{
		TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("sourceObjectPath"), ObjectPath);
		Record->SetStringField(TEXT("package"), TEXT("NOT_OBSERVED"));
		Record->SetStringField(TEXT("packageFile"), TEXT("NOT_OBSERVED"));
		Record->SetStringField(TEXT("sourceSHA256"), TEXT("NOT_OBSERVED"));
		Record->SetNumberField(TEXT("sizeBytes"), -1);
		Record->SetNumberField(TEXT("thumbW"), 0);
		Record->SetNumberField(TEXT("thumbH"), 0);
		Record->SetStringField(TEXT("compressionMagic"), TEXT("NOT_OBSERVED"));
		Record->SetStringField(TEXT("thumbnailMapKey"), TEXT("NOT_OBSERVED"));
		Record->SetStringField(TEXT("export"), TEXT(""));
		Record->SetStringField(TEXT("status"), TEXT("NOT_OBSERVED"));
		Record->SetStringField(TEXT("directLoaderResult"), TEXT("NOT_CALLED"));
		return Record;
	}

	static void SetRecordStatus(const TSharedPtr<FJsonObject>& Record, const TCHAR* Status)
	{
		Record->SetStringField(TEXT("status"), Status);
	}

	static bool CollectSavedThumbnail(
		const FString& ObjectPath,
		const FString& OutputDirectory,
		const TArray<TSharedPtr<FJsonValue>>& InventoryAssociations,
		const TArray<TSharedPtr<FJsonValue>>& MissingSlotAssociations,
		TSharedPtr<FJsonObject>& OutRecord)
	{
		OutRecord = MakeMeshRecord(ObjectPath);
		OutRecord->SetArrayField(TEXT("grassTypeInventoryAssociations"), InventoryAssociations);
		OutRecord->SetArrayField(TEXT("missingSlotRows"), MissingSlotAssociations);
		OutRecord->SetNumberField(TEXT("inventoryAssociationCount"), InventoryAssociations.Num());
		OutRecord->SetNumberField(TEXT("missingSlotAssociationCount"), MissingSlotAssociations.Num());
		OutRecord->SetBoolField(TEXT("presentInGrassSourceInventory"), !InventoryAssociations.IsEmpty());
		OutRecord->SetBoolField(TEXT("presentInMissingSlotReceipt"), !MissingSlotAssociations.IsEmpty());
		FString PackageName;
		FString AssetName;
		if (!SplitMeshObjectPath(ObjectPath, PackageName, AssetName))
		{
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_INVALID_OBJECT_PATH"));
			return false;
		}

		OutRecord->SetStringField(TEXT("package"), PackageName);
		const FString ObjectFullName = FString::Printf(TEXT("StaticMesh %s"), *ObjectPath);
		const FName ThumbnailMapKey(*ObjectFullName);
		OutRecord->SetStringField(TEXT("thumbnailMapKey"), ThumbnailMapKey.ToString());

		FString PackageFilename;
		if (!FPackageName::DoesPackageExist(PackageName, &PackageFilename))
		{
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_PACKAGE_MISSING"));
			return true;
		}
		OutRecord->SetStringField(TEXT("packageFile"), PackageFilename);

		FString BeforeSHA256;
		int64 BeforeSize = -1;
		if (!ComputeFileSHA256(PackageFilename, BeforeSHA256, BeforeSize))
		{
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_PACKAGE_SHA256_FAILED"));
			return false;
		}
		OutRecord->SetStringField(TEXT("sourceSHA256"), BeforeSHA256);
		OutRecord->SetNumberField(TEXT("sizeBytes"), static_cast<double>(BeforeSize));

		TSet<FName> RequestedNames;
		RequestedNames.Add(ThumbnailMapKey);
		FThumbnailMap Thumbnails;
		const bool bLoadedFromSourcePackage = ThumbnailTools::LoadThumbnailsFromPackageDirectly(
			PackageFilename, RequestedNames, Thumbnails, PackageFilename);
		OutRecord->SetStringField(TEXT("directLoaderResult"), bLoadedFromSourcePackage ? TEXT("TRUE") : TEXT("FALSE"));

		FString AfterSHA256;
		int64 AfterSize = -1;
		if (!ComputeFileSHA256(PackageFilename, AfterSHA256, AfterSize))
		{
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_POST_READ_SHA256_FAILED"));
			return false;
		}
		if (BeforeSHA256 != AfterSHA256 || BeforeSize != AfterSize)
		{
			OutRecord->SetStringField(TEXT("sourceSHA256AfterRead"), AfterSHA256);
			OutRecord->SetNumberField(TEXT("sizeBytesAfterRead"), static_cast<double>(AfterSize));
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_SOURCE_PACKAGE_CHANGED_DURING_READ"));
			return false;
		}
		OutRecord->SetStringField(TEXT("sourceSHA256AfterRead"), AfterSHA256);
		OutRecord->SetNumberField(TEXT("sizeBytesAfterRead"), static_cast<double>(AfterSize));

		if (!bLoadedFromSourcePackage)
		{
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_NO_SOURCE_THUMBNAIL_TABLE_OR_READ_FAILURE"));
			return true;
		}

		FObjectThumbnail* Thumbnail = Thumbnails.Find(ThumbnailMapKey);
		if (Thumbnail == nullptr)
		{
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_THUMBNAIL_KEY_MISSING"));
			return true;
		}

		OutRecord->SetNumberField(TEXT("thumbW"), Thumbnail->GetImageWidth());
		OutRecord->SetNumberField(TEXT("thumbH"), Thumbnail->GetImageHeight());
		// UE exposes this saved-byte accessor only as non-const. This private
		// thumbnail copy is read through a const array reference without edits.
		const TArray<uint8>& CompressedBytes = Thumbnail->AccessCompressedImageData();
		const FString Magic = DetectImageMagic(CompressedBytes);
		OutRecord->SetStringField(TEXT("compressionMagic"), Magic);
		OutRecord->SetNumberField(TEXT("compressedSizeBytes"), CompressedBytes.Num());
		if (Magic != TEXT("PNG") && Magic != TEXT("JPEG"))
		{
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_UNSUPPORTED_COMPRESSION_MAGIC"));
			return true;
		}

		const FString SafeShortName = FPaths::MakeValidFileName(AssetName);
		const FString Extension = Magic == TEXT("PNG") ? TEXT("png") : TEXT("jpg");
		const FString ImageFilename = FPaths::Combine(OutputDirectory, SafeShortName + TEXT(".") + Extension);
		FString WriteState;
		if (!WriteRawImageWithoutReplacingDifferentBytes(ImageFilename, CompressedBytes, WriteState))
		{
			OutRecord->SetStringField(TEXT("outputWriteState"), WriteState);
			SetRecordStatus(OutRecord, TEXT("NOT_OBSERVED_OUTPUT_PATH_CONFLICT_OR_WRITE_FAILURE"));
			return true;
		}

		OutRecord->SetStringField(TEXT("outputWriteState"), WriteState);
		OutRecord->SetStringField(TEXT("export"), FPaths::ConvertRelativePathToFull(ImageFilename));
		SetRecordStatus(OutRecord, Magic == TEXT("PNG") ? TEXT("EXPORTED_PNG_RAW_SAVED_BYTES") : TEXT("EXPORTED_JPEG_RAW_SAVED_BYTES"));
		return true;
	}

	static bool WriteManifest(const TSharedPtr<FJsonObject>& Manifest, const FString& Filename)
	{
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		if (!FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer))
		{
			return false;
		}
		return FFileHelper::SaveStringToFile(Json, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIMAcousticGrassAssetThumbnailTest,
	"IceMoon.AcousticField.Rural.GrassSourceSavedThumbnails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticGrassAssetThumbnailTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString SavedDirectory = FPaths::ProjectSavedDir();
	const FString MissingSlotReceiptFilename = FPaths::Combine(SavedDirectory, IMAcousticGrassAssetThumbnailTestPrivate::ReceiptRelativePath);
	const FString InventoryFilename = FPaths::Combine(SavedDirectory, IMAcousticGrassAssetThumbnailTestPrivate::InventoryRelativePath);
	const FString OutputDirectory = FPaths::Combine(SavedDirectory, IMAcousticGrassAssetThumbnailTestPrivate::OutputRelativePath);
	const FString ManifestFilenameFull = FPaths::Combine(OutputDirectory, IMAcousticGrassAssetThumbnailTestPrivate::ManifestFilename);
	const bool bOutputDirectoryReady = IFileManager::Get().MakeDirectory(*OutputDirectory, true);

	TArray<TSharedPtr<FJsonValue>> RecordValues;
	TArray<TSharedPtr<FJsonValue>> MissingMeshesWithoutInventoryValues;
	TArray<FString> UniqueMeshPaths;
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> InventoryAssociationsByMesh;
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> MissingSlotRowsByMesh;
	FString FailureReason;
	FString InventoryReceiptStatus = TEXT("NOT_OBSERVED");
	FString InventoryAutomationStatus = TEXT("NOT_OBSERVED");
	FString MissingSlotReceiptStatus = TEXT("NOT_OBSERVED");
	FString InventoryReceiptSHA256 = TEXT("NOT_OBSERVED");
	FString MissingSlotReceiptSHA256 = TEXT("NOT_OBSERVED");
	int64 InventoryReceiptSizeBytes = -1;
	int64 MissingSlotReceiptSizeBytes = -1;
	int32 InventoryGrassTypeCount = 0;
	int32 InventoryVarietyAssociationCount = 0;
	int32 MissingSlotRowCount = 0;
	bool bInputsValid = false;
	if (!bOutputDirectoryReady)
	{
		FailureReason = TEXT("Could not create the task Saved output directory.");
	}
	else
	{
		FString MissingSlotReceiptJson;
		TSharedPtr<FJsonObject> MissingSlotReceipt;
		if (!FFileHelper::LoadFileToString(MissingSlotReceiptJson, *MissingSlotReceiptFilename))
		{
			FailureReason = FString::Printf(TEXT("Could not read the required missing-slot receipt: %s"), *MissingSlotReceiptFilename);
		}
		else if (!IMAcousticGrassAssetThumbnailTestPrivate::ComputeFileSHA256(MissingSlotReceiptFilename, MissingSlotReceiptSHA256, MissingSlotReceiptSizeBytes))
		{
			FailureReason = TEXT("Could not compute SHA-256 for the required missing-slot receipt.");
		}
		else
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MissingSlotReceiptJson);
			if (!FJsonSerializer::Deserialize(Reader, MissingSlotReceipt) || !MissingSlotReceipt.IsValid())
			{
				FailureReason = TEXT("The required missing-slot receipt is not valid JSON.");
			}
			else
			{
				MissingSlotReceipt->TryGetStringField(TEXT("status"), MissingSlotReceiptStatus);
				const TArray<TSharedPtr<FJsonValue>>* MissingSlotRows = nullptr;
				if (!MissingSlotReceipt->TryGetArrayField(TEXT("missing_slot_rows"), MissingSlotRows) || MissingSlotRows == nullptr)
				{
					FailureReason = TEXT("The missing-slot receipt lacks its missing_slot_rows array.");
				}
				else
				{
					MissingSlotRowCount = MissingSlotRows->Num();
					for (const TSharedPtr<FJsonValue>& RowValue : *MissingSlotRows)
					{
						const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
						FString MeshObjectPath;
						if (!Row.IsValid() || !Row->TryGetStringField(TEXT("mesh"), MeshObjectPath) || MeshObjectPath.IsEmpty())
						{
							FailureReason = TEXT("A missing_slot_rows entry lacks an exact mesh object path.");
							break;
						}
						MissingSlotRowsByMesh.FindOrAdd(MeshObjectPath).Add(RowValue);
					}
				}
			}
		}

		if (FailureReason.IsEmpty())
		{
			FString InventoryJson;
			TSharedPtr<FJsonObject> Inventory;
			if (!FFileHelper::LoadFileToString(InventoryJson, *InventoryFilename))
			{
				FailureReason = FString::Printf(TEXT("Could not read the required full grass-source inventory: %s"), *InventoryFilename);
			}
			else if (!IMAcousticGrassAssetThumbnailTestPrivate::ComputeFileSHA256(InventoryFilename, InventoryReceiptSHA256, InventoryReceiptSizeBytes))
			{
				FailureReason = TEXT("Could not compute SHA-256 for the required full grass-source inventory.");
			}
			else
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InventoryJson);
				if (!FJsonSerializer::Deserialize(Reader, Inventory) || !Inventory.IsValid())
				{
					FailureReason = TEXT("The required full grass-source inventory is not valid JSON.");
				}
				else
				{
					Inventory->TryGetStringField(TEXT("status"), InventoryReceiptStatus);
					Inventory->TryGetStringField(TEXT("automation_status"), InventoryAutomationStatus);
					const TArray<TSharedPtr<FJsonValue>>* GrassTypes = nullptr;
					if (!Inventory->TryGetArrayField(TEXT("grass_types"), GrassTypes) || GrassTypes == nullptr)
					{
						FailureReason = TEXT("The full grass-source inventory lacks its grass_types array.");
					}
					else
					{
						InventoryGrassTypeCount = GrassTypes->Num();
						for (const TSharedPtr<FJsonValue>& GrassTypeValue : *GrassTypes)
						{
							const TSharedPtr<FJsonObject> GrassType = GrassTypeValue.IsValid() ? GrassTypeValue->AsObject() : nullptr;
							FString SourceGrassType;
							FString SourcePackage;
							const TArray<TSharedPtr<FJsonValue>>* Varieties = nullptr;
							if (!GrassType.IsValid()
								|| !GrassType->TryGetStringField(TEXT("source_grass_type"), SourceGrassType)
								|| SourceGrassType.IsEmpty()
								|| !GrassType->TryGetStringField(TEXT("source_package"), SourcePackage)
								|| SourcePackage.IsEmpty()
								|| !GrassType->TryGetArrayField(TEXT("varieties"), Varieties)
								|| Varieties == nullptr)
							{
								FailureReason = TEXT("A grass_types entry lacks its exact source path, package, or varieties array.");
								break;
							}

							const TSharedPtr<FJsonValue> SourceStateHashValue = GrassType->TryGetField(TEXT("source_state_hash_read_only"));
							for (const TSharedPtr<FJsonValue>& VarietyValue : *Varieties)
							{
								const TSharedPtr<FJsonObject> Variety = VarietyValue.IsValid() ? VarietyValue->AsObject() : nullptr;
								FString MeshObjectPath;
								if (!Variety.IsValid() || !Variety->TryGetStringField(TEXT("grass_mesh"), MeshObjectPath) || MeshObjectPath.IsEmpty())
								{
									FailureReason = TEXT("A grass variety lacks its exact grass_mesh object path.");
									break;
								}

								TSharedPtr<FJsonObject> Association = MakeShared<FJsonObject>();
								Association->SetStringField(TEXT("source_grass_type"), SourceGrassType);
								Association->SetStringField(TEXT("source_package"), SourcePackage);
								if (SourceStateHashValue.IsValid())
								{
									Association->SetField(TEXT("source_state_hash_read_only"), SourceStateHashValue);
								}
								else
								{
									Association->SetStringField(TEXT("source_state_hash_read_only"), TEXT("NOT_OBSERVED"));
								}
								Association->SetObjectField(TEXT("variety"), Variety);
								InventoryAssociationsByMesh.FindOrAdd(MeshObjectPath).Add(MakeShared<FJsonValueObject>(Association));
								++InventoryVarietyAssociationCount;
							}
							if (!FailureReason.IsEmpty())
							{
								break;
							}
						}
					}
				}
			}
		}

		if (FailureReason.IsEmpty())
		{
			TSet<FString> UniqueSet;
			for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Entry : InventoryAssociationsByMesh)
			{
				UniqueSet.Add(Entry.Key);
			}
			for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Entry : MissingSlotRowsByMesh)
			{
				UniqueSet.Add(Entry.Key);
			}
			UniqueMeshPaths = UniqueSet.Array();
			UniqueMeshPaths.Sort();
			TArray<FString> MissingMeshesWithoutInventory;
			for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Entry : MissingSlotRowsByMesh)
			{
				if (!InventoryAssociationsByMesh.Contains(Entry.Key))
				{
					MissingMeshesWithoutInventory.Add(Entry.Key);
				}
			}
			MissingMeshesWithoutInventory.Sort();
			for (const FString& MissingMesh : MissingMeshesWithoutInventory)
			{
				MissingMeshesWithoutInventoryValues.Add(MakeShared<FJsonValueString>(MissingMesh));
			}
			bInputsValid = !UniqueMeshPaths.IsEmpty() && InventoryVarietyAssociationCount > 0 && MissingSlotRowCount > 0;
			if (!bInputsValid)
			{
				FailureReason = TEXT("The two source receipts did not yield a non-empty mesh union and association sets.");
			}
		}
	}

	int32 ExportedCount = 0;
	int32 NotObservedCount = 0;
	int32 SourcePackageIntegrityFailureCount = 0;
	bool bAllMeshRecordsProcessed = true;
	const TArray<TSharedPtr<FJsonValue>> EmptyAssociations;
	if (bInputsValid)
	{
		for (const FString& MeshObjectPath : UniqueMeshPaths)
		{
			TSharedPtr<FJsonObject> Record;
			const TArray<TSharedPtr<FJsonValue>>* InventoryAssociations = InventoryAssociationsByMesh.Find(MeshObjectPath);
			const TArray<TSharedPtr<FJsonValue>>* MissingSlotAssociations = MissingSlotRowsByMesh.Find(MeshObjectPath);
			const bool bMeshProcessed = IMAcousticGrassAssetThumbnailTestPrivate::CollectSavedThumbnail(
				MeshObjectPath,
				OutputDirectory,
				InventoryAssociations != nullptr ? *InventoryAssociations : EmptyAssociations,
				MissingSlotAssociations != nullptr ? *MissingSlotAssociations : EmptyAssociations,
				Record);
			const FString Status = Record->GetStringField(TEXT("status"));
			bAllMeshRecordsProcessed &= bMeshProcessed;
			if (Status == TEXT("NOT_OBSERVED_PACKAGE_SHA256_FAILED")
				|| Status == TEXT("NOT_OBSERVED_POST_READ_SHA256_FAILED")
				|| Status == TEXT("NOT_OBSERVED_SOURCE_PACKAGE_CHANGED_DURING_READ"))
			{
				++SourcePackageIntegrityFailureCount;
				if (FailureReason.IsEmpty())
				{
					FailureReason = FString::Printf(TEXT("Source package integrity check failed for %s (status=%s)."), *MeshObjectPath, *Status);
				}
			}
			if (Status.StartsWith(TEXT("EXPORTED_")))
			{
				++ExportedCount;
			}
			else
			{
				++NotObservedCount;
			}
			RecordValues.Add(MakeShared<FJsonValueObject>(Record));
		}
	}
	if (bInputsValid && !bAllMeshRecordsProcessed && FailureReason.IsEmpty())
	{
		FailureReason = TEXT("One or more source mesh paths could not be resolved as exact asset object paths.");
	}

	const bool bCollectorComplete = bInputsValid && bAllMeshRecordsProcessed && bOutputDirectoryReady;
	const TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("automationTest"), TEXT("IceMoon.AcousticField.Rural.GrassSourceSavedThumbnails"));
	Manifest->SetStringField(TEXT("collectorStatus"), bCollectorComplete ? TEXT("PASS") : TEXT("FAIL"));
	Manifest->SetStringField(TEXT("terminalReason"), FailureReason.IsEmpty() ? TEXT("All unique inventory and missing-slot mesh paths were enumerated; source associations and thumbnail outcomes were recorded.") : FailureReason);
	Manifest->SetStringField(TEXT("missingSlotReceipt"), MissingSlotReceiptFilename);
	Manifest->SetStringField(TEXT("missingSlotReceiptStatus"), MissingSlotReceiptStatus);
	Manifest->SetStringField(TEXT("missingSlotReceiptSHA256"), MissingSlotReceiptSHA256);
	Manifest->SetNumberField(TEXT("missingSlotReceiptSizeBytes"), static_cast<double>(MissingSlotReceiptSizeBytes));
	Manifest->SetStringField(TEXT("grassSourceInventoryReceipt"), InventoryFilename);
	Manifest->SetStringField(TEXT("grassSourceInventoryStatus"), InventoryReceiptStatus);
	Manifest->SetStringField(TEXT("grassSourceInventoryAutomationStatus"), InventoryAutomationStatus);
	Manifest->SetStringField(TEXT("grassSourceInventoryReceiptSHA256"), InventoryReceiptSHA256);
	Manifest->SetNumberField(TEXT("grassSourceInventoryReceiptSizeBytes"), static_cast<double>(InventoryReceiptSizeBytes));
	Manifest->SetStringField(TEXT("outputDirectory"), OutputDirectory);
	Manifest->SetStringField(TEXT("scope"), TEXT("Read-only saved source-package thumbnail byte extraction for all grass_types varieties, retaining full inventory associations and exact missing_slot_rows associations. No thumbnail generation, UObject asset load/save, CVar change, or visual classification."));
	Manifest->SetStringField(TEXT("visualClassification"), TEXT("NOT_PERFORMED"));
	Manifest->SetNumberField(TEXT("grassTypeCount"), InventoryGrassTypeCount);
	Manifest->SetNumberField(TEXT("inventoryVarietyAssociationCount"), InventoryVarietyAssociationCount);
	Manifest->SetNumberField(TEXT("inventoryUniqueMeshCount"), InventoryAssociationsByMesh.Num());
	Manifest->SetNumberField(TEXT("missingSlotRowCount"), MissingSlotRowCount);
	Manifest->SetNumberField(TEXT("missingSlotUniqueMeshCount"), MissingSlotRowsByMesh.Num());
	Manifest->SetNumberField(TEXT("uniqueMeshUnionCount"), UniqueMeshPaths.Num());
	Manifest->SetArrayField(TEXT("missingSlotMeshesWithoutInventoryAssociation"), MissingMeshesWithoutInventoryValues);
	Manifest->SetNumberField(TEXT("exportedCount"), ExportedCount);
	Manifest->SetNumberField(TEXT("notObservedCount"), NotObservedCount);
	Manifest->SetNumberField(TEXT("sourcePackageIntegrityFailureCount"), SourcePackageIntegrityFailureCount);
	Manifest->SetStringField(TEXT("thumbnailObservationStatus"), NotObservedCount == 0 ? TEXT("OBSERVED_ALL") : TEXT("INCONCLUSIVE_SOME_NOT_OBSERVED"));
	Manifest->SetArrayField(TEXT("meshes"), RecordValues);

	const bool bManifestWritten = bOutputDirectoryReady && IMAcousticGrassAssetThumbnailTestPrivate::WriteManifest(Manifest, ManifestFilenameFull);
	const bool bPassed = bCollectorComplete && bManifestWritten;
	if (!bManifestWritten)
	{
		FailureReason = FString::Printf(TEXT("Could not write the collector manifest: %s"), *ManifestFilenameFull);
		AddError(FailureReason);
	}
	else if (!bCollectorComplete)
	{
		AddError(FailureReason.IsEmpty() ? TEXT("Collector input or output precondition failed.") : FailureReason);
	}

	if (bPassed)
	{
		UE_LOG(LogTemp, Display,
			TEXT("[IM][PIE_TEST] PASS GrassSourceSavedThumbnails inventoryMeshes=%d missingSlotMeshes=%d exported=%d notObserved=%d visualClassification=NOT_PERFORMED manifest=%s"),
			InventoryAssociationsByMesh.Num(), MissingSlotRowsByMesh.Num(), ExportedCount, NotObservedCount, *ManifestFilenameFull);
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor PASS GrassSourceSavedThumbnails"));
	}
	else
	{
		UE_LOG(LogTemp, Error,
			TEXT("[IM][PIE_TEST] FAIL GrassSourceSavedThumbnails exported=%d notObserved=%d reason=%s manifest=%s"),
			ExportedCount, NotObservedCount, *FailureReason, *ManifestFilenameFull);
		UE_LOG(LogTemp, Error, TEXT("IMExitEditor FAIL GrassSourceSavedThumbnails"));
	}
	return bPassed;
}

#endif // WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
