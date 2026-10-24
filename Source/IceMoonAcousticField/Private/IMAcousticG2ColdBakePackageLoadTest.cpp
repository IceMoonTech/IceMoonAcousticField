#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticOperationMemorySampler.h"
#include "IMAcousticSimulation.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticG2ColdBakePackageLoadTestPrivate
{
constexpr int64 IMExpectedPackageBytes = 5128266;
constexpr int32 IMExpectedPackedSceneBytes = 4903183;
constexpr int32 IMExpectedPackedProbeBytes = 214737;
constexpr int32 IMExpectedRawSceneBytes = 13724688;
constexpr int32 IMExpectedRawProbeBytes = 1250180;
constexpr int32 IMExpectedProbeCount = 121;
const TCHAR* IMEditorWorld = TEXT("/Engine/Maps/Entry");
const TCHAR* IMBakeWorld = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
const TCHAR* IMTargetPackage = TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_CE06F85C4DBFC6E0D63FF8A345E1C1A6");
const TCHAR* IMTargetAsset = TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_CE06F85C4DBFC6E0D63FF8A345E1C1A6.IM_IM_RuralCamera3Preview_10m_CE06F85C4DBFC6E0D63FF8A345E1C1A6");
const TCHAR* IMExpectedSceneFingerprint = TEXT("955D1C0A2195BAE4DEB1EEADD3ACC4F1FEDAE103");
const TCHAR* IMExpectedPackageSha256 = TEXT("31d412cc2a9b46b5308d0cbba866b3b184bf553018cafed6bdd52ce119660da5");

FString IMHashPayload(const UIMAcousticBakeAsset& Asset)
{
	FSHA1 Hash;
	auto UpdateSection = [&Hash](const uint8* Data, uint32 Count)
	{
		const uint8 Length[4] = {static_cast<uint8>(Count), static_cast<uint8>(Count >> 8),
			static_cast<uint8>(Count >> 16), static_cast<uint8>(Count >> 24)};
		Hash.Update(Length, UE_ARRAY_COUNT(Length));
		if (Count && Data) Hash.Update(Data, Count);
	};
	FTCHARToUTF8 Metadata(*Asset.MetadataJson);
	UpdateSection(reinterpret_cast<const uint8*>(Metadata.Get()), static_cast<uint32>(Metadata.Length()));
	UpdateSection(Asset.SceneData.GetData(), static_cast<uint32>(Asset.SceneData.Num()));
	UpdateSection(Asset.ProbeData.GetData(), static_cast<uint32>(Asset.ProbeData.Num()));
	Hash.Final();
	uint8 Digest[FSHA1::DigestSize];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

FString IMHashProbePreview(const TArray<FVector4>& Probes)
{
	FSHA1 Hash;
	for (const FVector4& Probe : Probes)
	{
		const FString Record = FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g;"),
			static_cast<double>(Probe.X), static_cast<double>(Probe.Y),
			static_cast<double>(Probe.Z), static_cast<double>(Probe.W));
		FTCHARToUTF8 Utf8(*Record);
		if (Utf8.Length()) Hash.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
	Hash.Final();
	uint8 Digest[FSHA1::DigestSize];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

struct FIMR49SamplerAssessment
{
	bool bFail = false;
	bool bShortWindow = false;
	FString Failure;
};

FIMR49SamplerAssessment IMAssessSampler(const FIMAcousticOperationMemorySamplerReport& Samples)
{
	FIMR49SamplerAssessment Result;
	auto Issue = [&Result](const TCHAR* Text)
	{
		Result.bFail = true;
		if (!Result.Failure.IsEmpty()) Result.Failure += TEXT(" | ");
		Result.Failure += Text;
	};
	if (!Samples.bStartAttempted || !Samples.bConfigurationValid || Samples.bConfigurationFailed
		|| !Samples.bWorkerStarted || Samples.bThreadStartFailed
		|| !Samples.bProcessIdentityCaptured || Samples.bProcessIdentityQueryFailed
		|| Samples.bProcessIdentityChanged || Samples.bMemoryQueryResultInvalid
		|| Samples.bWorkerExitedUnexpectedly || Samples.bSampleOverflowed
		|| Samples.bHasUnscopedInvalidSample || Samples.InvalidSampleCount != 0
		|| Samples.DroppedSampleCount != 0 || Samples.InvalidOperationCallCount != 0)
	{
		Issue(TEXT("Sampler process/configuration/worker health is invalid."));
	}
	if (Samples.AcceptedSampleAttemptCount > 0 && !Samples.bProcessIdentityVerifiedForAllSamples)
	{
		Issue(TEXT("Sampler process identity was not verified for accepted samples."));
	}
	if (Samples.Operations.Num() != 3)
	{
		Issue(TEXT("Sampler did not close exactly the three required operation windows."));
	}
	int32 NameMask = 0;
	for (const FIMAcousticOperationMemoryWindowResult& Window : Samples.Operations)
	{
		int32 Bit = 0;
		if (Window.Name == TEXT("cold_package_load_object")) Bit = 1;
		else if (Window.Name == TEXT("loaded_payload_validate_decode")) Bit = 2;
		else if (Window.Name == TEXT("new_sdk_load_48000_512")) Bit = 4;
		else Issue(TEXT("Sampler returned an unexpected operation window name."));
		if (Bit && (NameMask & Bit)) Issue(TEXT("Sampler returned a duplicate operation window name."));
		NameMask |= Bit;
		if (!Window.EndSeconds.IsSet() || !Window.ElapsedSeconds.IsSet()
			|| Window.ElapsedSeconds.GetValue() <= 0.0)
		{
			Issue(TEXT("Sampler operation window has no valid end or elapsed time."));
		}
		if (Window.InvalidSampleCount != 0 || Window.DroppedSampleCount != 0)
		{
			Issue(TEXT("Sampler operation window contains invalid or dropped samples."));
		}
		if (Window.ValidSampleCount < 2)
		{
			Result.bShortWindow = true;
		}
		else if (!Window.bMaximaValid)
		{
			Issue(TEXT("Sampler window has enough samples but invalid maxima."));
		}
	}
	if (NameMask != 7) Issue(TEXT("Sampler operation window set is incomplete."));
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticG2ColdBakePackageLoadTest,
	"IceMoon.AcousticField.G2.ColdBakePackageLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticG2ColdBakePackageLoadTest::RunTest(const FString&)
{
	check(IsInGameThread());
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || World->IsGameWorld() || (GEditor && GEditor->PlayWorld)
		|| World->GetPackage()->GetName() != IMAcousticG2ColdBakePackageLoadTestPrivate::IMEditorWorld)
	{
		AddError(TEXT("Cold package load requires the non-PIE /Engine/Maps/Entry editor world."));
		return false;
	}

	const FString PackageName(IMAcousticG2ColdBakePackageLoadTestPrivate::IMTargetPackage);
	const FString AssetPath(IMAcousticG2ColdBakePackageLoadTestPrivate::IMTargetAsset);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const int64 PackageBytes = IFileManager::Get().FileSize(*PackageFilename);
	if (PackageBytes != IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedPackageBytes)
	{
		AddError(FString::Printf(TEXT("CE06 package preflight size mismatch: expected %lld, got %lld (%s)."),
			static_cast<long long>(IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedPackageBytes), static_cast<long long>(PackageBytes), *PackageFilename));
		return false;
	}
	// Both checks are mandatory. A resident package or object is a failed cold
	// precondition; the test must never skip LoadObject and call that a pass.
	if (FindPackage(nullptr, *PackageName) || FindObject<UIMAcousticBakeAsset>(nullptr, *AssetPath))
	{
		AddError(TEXT("CE06 package or UIMAcousticBakeAsset is already resident; cold evidence is invalid."));
		return false;
	}

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Errors;
	bool bFailed = false;
	FIMAcousticOperationMemorySampler Memory(0.002);
	if (!Memory.Start())
	{
		AddError(TEXT("Cold package load sampler did not start."));
		return false;
	}
	auto Fail = [&](const FString& Message)
	{
		bFailed = true;
		Errors.Add(MakeShared<FJsonValueString>(Message));
		AddError(Message);
	};
	auto BeginWindow = [&](const TCHAR* Name) -> uint64
	{
		const uint64 Id = Memory.BeginOperation(Name);
		if (!Id) Fail(FString(TEXT("Memory operation begin failed: ")) + Name);
		return Id;
	};
	auto EndWindow = [&](uint64& Id) -> bool
	{
		const bool bEnded = Id && Memory.EndOperation(Id);
		if (!bEnded) Fail(TEXT("Memory operation end failed."));
		Id = 0;
		return bEnded;
	};

	Report->SetStringField(TEXT("run_id"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Report->SetStringField(TEXT("world"), World->GetPackage()->GetName());
	Report->SetStringField(TEXT("editor_world"), IMAcousticG2ColdBakePackageLoadTestPrivate::IMEditorWorld);
	Report->SetStringField(TEXT("bake_world"), IMAcousticG2ColdBakePackageLoadTestPrivate::IMBakeWorld);
	Report->SetStringField(TEXT("package_name"), PackageName);
	Report->SetStringField(TEXT("package_filename"), PackageFilename);
	Report->SetNumberField(TEXT("package_bytes"), static_cast<double>(PackageBytes));
	Report->SetStringField(TEXT("package_sha256_expected"), IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedPackageSha256);
	Report->SetStringField(TEXT("package_sha256_runtime"), TEXT("not_read_to_preserve_load_boundary"));
	Report->SetBoolField(TEXT("disk_cold_read_claim"), false);
	Report->SetBoolField(TEXT("level_save_requested"), false);
	Report->SetBoolField(TEXT("level_load_requested"), false);
	Report->SetBoolField(TEXT("pie_requested"), false);
	Report->SetBoolField(TEXT("world_tick_requested"), false);
	Report->SetStringField(TEXT("sampling_interval_seconds"), TEXT("0.002"));

	TStrongObjectPtr<UIMAcousticBakeAsset> Asset;
	UIMAcousticBakeAsset* LoadedAsset = nullptr;
	FString Failure;
	double LoadObjectMs = 0.0;
	double PayloadMs = 0.0;
	double SDKLoadMs = 0.0;
	uint64 LoadObjectId = BeginWindow(TEXT("cold_package_load_object"));
	if (LoadObjectId)
	{
		const double Started = FPlatformTime::Seconds();
		LoadedAsset = LoadObject<UIMAcousticBakeAsset>(nullptr, *AssetPath);
		LoadObjectMs = (FPlatformTime::Seconds() - Started) * 1000.0;
		EndWindow(LoadObjectId);
		if (!LoadedAsset)
		{
			Fail(TEXT("LoadObject did not return the CE06 bake asset."));
		}
		else
		{
			Asset.Reset(LoadedAsset);
			if (!Asset.IsValid() || Asset->GetPathName() != AssetPath
				|| !Asset->GetPackage() || Asset->GetPackage()->GetName() != PackageName)
			{
				Fail(TEXT("LoadObject returned an asset with an unexpected object or package path."));
			}
			if (Asset.IsValid() && Asset->GetPackage()->IsDirty())
			{
				Fail(TEXT("Cold package load dirtied the existing bake package."));
			}
		}
	}
	Report->SetNumberField(TEXT("load_object_ms"), LoadObjectMs);

	TArray<FVector4> ProbePreview;
	FVector ProbeOrigin = FVector::ZeroVector;
	TArray<uint8> RawScene;
	TArray<uint8> RawProbes;
	FIMAcousticBakeData Data;
	bool bPayloadOk = false;
	uint64 PayloadId = BeginWindow(TEXT("loaded_payload_validate_decode"));
	if (PayloadId)
	{
		const double Started = FPlatformTime::Seconds();
		if (!Asset.IsValid())
		{
			Fail(TEXT("Payload window could not retain the loaded CE06 asset."));
		}
		// CE06 is a frozen format-3 package. A newer writer format must keep
		// this cold-load compatibility proof without rewriting the source asset.
		else if (Asset->FormatVersion != 3
			|| Asset->SteamAudioVersion != UIMAcousticBakeAsset::SDKVersion
			|| Asset->WorldPackage != IMAcousticG2ColdBakePackageLoadTestPrivate::IMBakeWorld
			|| Asset->SceneFingerprint != IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedSceneFingerprint
			|| Asset->SceneData.Num() != IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedPackedSceneBytes
			|| Asset->ProbeData.Num() != IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedPackedProbeBytes)
		{
			Fail(TEXT("Loaded CE06 asset format, SDK, world, fingerprint, or packed byte counts differ."));
		}
		else if (!Asset->Validate(IMAcousticG2ColdBakePackageLoadTestPrivate::IMBakeWorld, IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedSceneFingerprint, Failure)
			|| !Asset->GetProbePreview(IMAcousticG2ColdBakePackageLoadTestPrivate::IMBakeWorld, IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedSceneFingerprint,
				ProbePreview, ProbeOrigin, Failure)
			|| !Asset->ReadPayload(IMAcousticG2ColdBakePackageLoadTestPrivate::IMBakeWorld, IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedSceneFingerprint,
				RawScene, RawProbes, Failure))
		{
			Fail(TEXT("Loaded CE06 payload validation, probe preview, or decode failed: ") + Failure);
		}
		else if (RawScene.Num() != IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedRawSceneBytes || RawProbes.Num() != IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedRawProbeBytes
			|| ProbePreview.Num() != IMAcousticG2ColdBakePackageLoadTestPrivate::IMExpectedProbeCount)
		{
			Fail(TEXT("Loaded CE06 decoded byte counts or probe count differ from the frozen target."));
		}
		else
		{
			const FString ComputedPayloadDigest = IMAcousticG2ColdBakePackageLoadTestPrivate::IMHashPayload(*Asset);
			if (Asset->PayloadDigest != ComputedPayloadDigest)
			{
				Fail(TEXT("Loaded CE06 payload digest does not match metadata and packed bytes."));
			}
			for (const FVector4& Probe : ProbePreview)
			{
				if (!FMath::IsFinite(Probe.X) || !FMath::IsFinite(Probe.Y)
					|| !FMath::IsFinite(Probe.Z) || !FMath::IsFinite(Probe.W) || Probe.W <= 0.0)
				{
					Fail(TEXT("Loaded CE06 probe preview contains a nonfinite coordinate or nonpositive radius."));
					break;
				}
			}
			if (!bFailed)
			{
				Data.Scene.assign(RawScene.GetData(), RawScene.GetData() + RawScene.Num());
				Data.ProbeBatch.assign(RawProbes.GetData(), RawProbes.GetData() + RawProbes.Num());
				Data.CoverageProbes.reserve(ProbePreview.Num());
				for (const FVector4& Probe : ProbePreview)
				{
					Data.CoverageProbes.push_back({{static_cast<float>(Probe.X), static_cast<float>(Probe.Y),
						static_cast<float>(Probe.Z)}, static_cast<float>(Probe.W)});
				}
				Report->SetStringField(TEXT("payload_digest"), Asset->PayloadDigest);
				Report->SetStringField(TEXT("payload_digest_recomputed"), ComputedPayloadDigest);
				Report->SetBoolField(TEXT("payload_digest_matches"), true);
				Report->SetStringField(TEXT("probe_preview_sha1"), IMAcousticG2ColdBakePackageLoadTestPrivate::IMHashProbePreview(ProbePreview));
				Report->SetNumberField(TEXT("probe_count"), ProbePreview.Num());
				Report->SetNumberField(TEXT("raw_scene_bytes"), RawScene.Num());
				Report->SetNumberField(TEXT("raw_probe_bytes"), RawProbes.Num());
				Report->SetNumberField(TEXT("packed_scene_bytes"), Asset->SceneData.Num());
				Report->SetNumberField(TEXT("packed_probe_bytes"), Asset->ProbeData.Num());
				Report->SetStringField(TEXT("scene_fingerprint"), Asset->SceneFingerprint);
				bPayloadOk = true;
			}
		}
		PayloadMs = (FPlatformTime::Seconds() - Started) * 1000.0;
		EndWindow(PayloadId);
	}
	Report->SetNumberField(TEXT("payload_validate_decode_ms"), PayloadMs);
	Report->SetBoolField(TEXT("payload_ok"), bPayloadOk);

	bool bSDKLoaded = false;
	// Keep the SDK object in this inner scope so it is destroyed before the
	// sampler is stopped and joined below.
	{
		FIMAcousticSimulation Simulation;
		std::string SDKError;
		if (bPayloadOk && !Simulation.SetPathingOptions(FIMAcousticPathingOptions::DefaultHybrid(), SDKError))
		{
			Fail(UTF8_TO_TCHAR(SDKError.c_str()));
		}
		uint64 SDKLoadId = BeginWindow(TEXT("new_sdk_load_48000_512"));
		if (SDKLoadId)
		{
			const double Started = FPlatformTime::Seconds();
			if (bPayloadOk && !bFailed)
			{
				bSDKLoaded = Simulation.Load(Data, 48000, 512, SDKError);
				if (!bSDKLoaded)
				{
					Fail(FString(TEXT("CE06 SDK load failed: ")) + UTF8_TO_TCHAR(SDKError.c_str()));
				}
			}
			else
			{
				Fail(TEXT("SDK load window was reached without a valid decoded payload."));
			}
			SDKLoadMs = (FPlatformTime::Seconds() - Started) * 1000.0;
			EndWindow(SDKLoadId);
		}
	}
	Report->SetNumberField(TEXT("sdk_load_ms"), SDKLoadMs);
	Report->SetBoolField(TEXT("sdk_load_ok"), bSDKLoaded);

	const bool bStagesPassedBeforeMemoryAssessment = !bFailed && bPayloadOk && bSDKLoaded;
	const FIMAcousticOperationMemorySamplerReport Samples = Memory.StopAndCollect();
	const IMAcousticG2ColdBakePackageLoadTestPrivate::FIMR49SamplerAssessment Assessment = IMAcousticG2ColdBakePackageLoadTestPrivate::IMAssessSampler(Samples);
	if (Assessment.bFail) Fail(Assessment.Failure);
	const FString MemoryStatus = Assessment.bFail
		? TEXT("FAIL") : (Assessment.bShortWindow ? TEXT("INCONCLUSIVE_SHORT_WINDOW") : TEXT("MEASURED"));
	Report->SetStringField(TEXT("memory_status"), MemoryStatus);
	Report->SetBoolField(TEXT("sampler_stop_and_join_called"), true);
	Report->SetBoolField(TEXT("operation_times_measured"), bStagesPassedBeforeMemoryAssessment);
	Report->SetBoolField(TEXT("behavior_success"), !bFailed && bPayloadOk && bSDKLoaded);
	Report->SetStringField(TEXT("status"), bFailed ? TEXT("FAIL")
		: (Assessment.bShortWindow ? TEXT("PASS_MEMORY_INCONCLUSIVE") : TEXT("PASS")));
	Report->SetStringField(TEXT("assurance"), bFailed || Assessment.bShortWindow ? TEXT("BLOCKED") : TEXT("ELIGIBLE"));
	Report->SetBoolField(TEXT("disk_cold_read_claim"), false);
	Report->SetArrayField(TEXT("errors"), Errors);
	Report->SetObjectField(TEXT("operation_memory"), IM_AcousticOperationMemoryReportToJson(Samples));

	const FString RunId = Report->GetStringField(TEXT("run_id"));
	const FString ReportDir = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AcousticV2/G2ColdPackageLoad"), RunId);
	FString Json;
	const bool bSaved = FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json))
		&& IFileManager::Get().MakeDirectory(*ReportDir, true)
		&& FFileHelper::SaveStringToFile(Json, *FPaths::Combine(ReportDir, TEXT("report.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!bSaved)
	{
		AddError(TEXT("Cold package load report could not be written."));
		return false;
	}
	AddInfo(FString::Printf(TEXT("G2 cold package load report: %s"), *ReportDir));
	UE_LOG(LogTemp, Display,
		TEXT("IMLogs AcousticG2ColdPackageLoad status=%s memory=%s load_object_ms=%.3f payload_ms=%.3f sdk_load_ms=%.3f report=%s"),
		bFailed ? TEXT("FAIL") : (Assessment.bShortWindow ? TEXT("PASS_MEMORY_INCONCLUSIVE") : TEXT("PASS")),
		*MemoryStatus, LoadObjectMs, PayloadMs, SDKLoadMs, *ReportDir);
	return !bFailed && bPayloadOk && bSDKLoaded;
}
#endif
