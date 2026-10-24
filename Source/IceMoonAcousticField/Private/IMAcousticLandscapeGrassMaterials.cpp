#include "IMAcousticLandscapeGrassMaterials.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "LandscapeComponent.h"
#include "LandscapeGrassType.h"
#include "LandscapeProxy.h"
#include "MaterialCachedData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeExit.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Serialization/ArchiveCrc32.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace IMAcousticLandscapeGrassMaterialsPrivate
{
	static const TCHAR* TaskWorldPackage = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
	static const TCHAR* PluginGrassRoot = TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials/GrassTypes/");
	static const TCHAR* PluginRoot = TEXT("/IceMoonAcousticField/");
	static const TCHAR* GrassComponentClassName = TEXT("GrassInstancedStaticMeshComponent");

	static uint32 ComputeOwnedGrassStateHash(ULandscapeGrassType* Type)
	{
		check(IsInGameThread() && IsValid(Type)
			&& Type->GetOutermost()->GetName().StartsWith(PluginGrassRoot));
		// UE5.8 LandscapeGrass.cpp:1620-1636 uses this exact serialized CRC.
		// ComputeStateHash is not exported by the MinimalAPI GrassType class.
		// Only the task-owned copy is temporarily changed, on the GT; source
		// types stay read-only. ScopeExit restores it even on early exit.
		const uint32 SavedHash = Type->StateHash;
		ON_SCOPE_EXIT { Type->StateHash = SavedHash; };
		Type->StateHash = 0;
		FArchiveCrc32 Archive;
		Type->Serialize(Archive);
		return Archive.GetCrc();
	}

	struct FIMGrassSourceHash
	{
		FString PackageName;
		FString Filename;
		FString Sha1Before;
		FString Sha1After;
		int64 ByteCount = 0;
		bool bDirtyBefore = false;
		bool bDirtyAfter = false;
	};

	struct FIMGrassTypeRecord
	{
		ULandscapeGrassType* Source = nullptr;
		ULandscapeGrassType* Copy = nullptr;
		FString SourceObjectPath;
		FString CopyObjectPath;
		FString CopyPackageName;
		FIMGrassSourceHash SourceHash;
		TArray<TSharedPtr<FJsonValue>> VarietyRows;
		bool bCreated = false;
	};

	struct FIMLandscapeGrassBinding
	{
		ALandscapeProxy* Proxy = nullptr;
		ULandscapeComponent* Component = nullptr;
		FName OutputName;
		ULandscapeGrassType* Source = nullptr;
		FString SourceObjectPath;
	};

	static bool HashPackageFile(const FString& PackageName, FString& OutFilename, FString& OutSha1, int64& OutByteCount, FString& OutError)
	{
		if (!FPackageName::DoesPackageExist(PackageName, &OutFilename))
		{
			OutError = TEXT("Source GrassType package is missing on disk: ") + PackageName;
			return false;
		}
		FPaths::NormalizeFilename(OutFilename);
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *OutFilename) || Bytes.IsEmpty())
		{
			OutError = TEXT("Could not read source GrassType package bytes: ") + OutFilename;
			return false;
		}
		uint8 Digest[20];
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
		OutSha1 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
		OutByteCount = Bytes.Num();
		return true;
	}

	static bool IsPluginPhysicalMaterialMIC(UMaterialInterface* Material, UMaterialInstanceConstant*& OutMIC, FString& OutError)
	{
		OutMIC = Cast<UMaterialInstanceConstant>(Material);
		if (!OutMIC || !OutMIC->GetOutermost()->GetName().StartsWith(PluginRoot))
		{
			OutError = TEXT("A grass material redirect must be a plugin-owned UMaterialInstanceConstant.");
			return false;
		}

		UPhysicalMaterial* ExplicitPhysicalMaterial = OutMIC->PhysMaterial.Get();
		if (!ExplicitPhysicalMaterial || !ExplicitPhysicalMaterial->GetOutermost()->GetName().StartsWith(PluginRoot)
			|| OutMIC->GetPhysicalMaterial() != ExplicitPhysicalMaterial)
		{
			OutError = TEXT("A grass material redirect must have an explicit plugin PhysicalMaterial override: ") + OutMIC->GetPathName();
			return false;
		}
		return true;
	}

	static bool ValidateRedirectTable(
		const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
		TSet<UMaterialInterface*>& OutPluginMICs,
		FString& OutError)
	{
		OutPluginMICs.Reset();
		if (SourceToPluginMIC.IsEmpty())
		{
			OutError = TEXT("The material redirect table is empty.");
			return false;
		}

		for (const TPair<UMaterialInterface*, UMaterialInterface*>& Pair : SourceToPluginMIC)
		{
			UMaterialInstanceConstant* MIC = nullptr;
			if (!IsValid(Pair.Key) || !IsPluginPhysicalMaterialMIC(Pair.Value, MIC, OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("The material redirect table contains a null source key.");
				}
				return false;
			}
			OutPluginMICs.Add(MIC);
		}
		return true;
	}

	static bool GetRedirectedMaterial(
		UMaterialInterface* SourceMaterial,
		const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
		UMaterialInstanceConstant*& OutMIC,
		FString& OutError)
	{
		UMaterialInterface* const* Redirect = SourceMaterial ? SourceToPluginMIC.Find(SourceMaterial) : nullptr;
		if (!Redirect || !IsPluginPhysicalMaterialMIC(*Redirect, OutMIC, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("No exact plugin MIC redirect exists for source grass material: ")
					+ (SourceMaterial ? SourceMaterial->GetPathName() : FString(TEXT("<null>")));
			}
			return false;
		}
		return true;
	}

	static int32 GetGrassMeshSlotCount(const FGrassVariety& Variety)
	{
		return IsValid(Variety.GrassMesh.Get()) ? Variety.GrassMesh->GetStaticMaterials().Num() : 0;
	}

	static UMaterialInterface* GetSourceMaterialForSlot(const FGrassVariety& Variety, int32 SlotIndex)
	{
		if (Variety.OverrideMaterials.IsValidIndex(SlotIndex) && Variety.OverrideMaterials[SlotIndex])
		{
			return Variety.OverrideMaterials[SlotIndex].Get();
		}
		return IsValid(Variety.GrassMesh.Get()) ? Variety.GrassMesh->GetMaterial(SlotIndex) : nullptr;
	}

	static bool CompareNonMaterialGrassProperties(const ULandscapeGrassType* Source, const ULandscapeGrassType* Copy, FString& OutDifference)
	{
		if (!Source || !Copy || Source->GetClass() != Copy->GetClass())
		{
			OutDifference = TEXT("Source and plugin GrassType class differ.");
			return false;
		}

		for (TFieldIterator<FProperty> It(ULandscapeGrassType::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const FName PropertyName = It->GetFName();
			if (PropertyName == GET_MEMBER_NAME_CHECKED(ULandscapeGrassType, GrassVarieties)
				|| PropertyName == GET_MEMBER_NAME_CHECKED(ULandscapeGrassType, StateHash))
			{
				continue;
			}
			if (!It->Identical_InContainer(Source, Copy, 0, PPF_None))
			{
				OutDifference = TEXT("GrassType property differs: ") + PropertyName.ToString();
				return false;
			}
		}

		if (Source->GrassVarieties.Num() != Copy->GrassVarieties.Num())
		{
			OutDifference = TEXT("GrassVarieties count or order differs.");
			return false;
		}

		for (int32 VarietyIndex = 0; VarietyIndex < Source->GrassVarieties.Num(); ++VarietyIndex)
		{
			const FGrassVariety& SourceVariety = Source->GrassVarieties[VarietyIndex];
			const FGrassVariety& CopyVariety = Copy->GrassVarieties[VarietyIndex];
			for (TFieldIterator<FProperty> It(FGrassVariety::StaticStruct()); It; ++It)
			{
				if (It->GetFName() == GET_MEMBER_NAME_CHECKED(FGrassVariety, OverrideMaterials))
				{
					continue;
				}
				if (!It->Identical_InContainer(&SourceVariety, &CopyVariety, 0, PPF_None))
				{
					OutDifference = FString::Printf(TEXT("GrassVarieties[%d].%s differs."), VarietyIndex, *It->GetName());
					return false;
				}
			}
		}
		return true;
	}

	static bool CompareMaterialOverrides(
		const ULandscapeGrassType* Source,
		const ULandscapeGrassType* Copy,
		const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
		TArray<TSharedPtr<FJsonValue>>& OutVarietyRows,
		FString& OutDifference)
	{
		for (int32 VarietyIndex = 0; VarietyIndex < Source->GrassVarieties.Num(); ++VarietyIndex)
		{
			const FGrassVariety& SourceVariety = Source->GrassVarieties[VarietyIndex];
			const FGrassVariety& CopyVariety = Copy->GrassVarieties[VarietyIndex];
			const int32 SlotCount = GetGrassMeshSlotCount(SourceVariety);
			if (SlotCount <= 0)
			{
				OutDifference = FString::Printf(TEXT("GrassVarieties[%d] has no valid static mesh material slots."), VarietyIndex);
				return false;
			}

			const int32 ExpectedOverrideCount = FMath::Max(SourceVariety.OverrideMaterials.Num(), SlotCount);
			if (CopyVariety.OverrideMaterials.Num() != ExpectedOverrideCount)
			{
				OutDifference = FString::Printf(TEXT("GrassVarieties[%d].OverrideMaterials count is %d, expected %d."),
					VarietyIndex, CopyVariety.OverrideMaterials.Num(), ExpectedOverrideCount);
				return false;
			}

			TSharedRef<FJsonObject> VarietyRow = MakeShared<FJsonObject>();
			VarietyRow->SetNumberField(TEXT("variety_index"), VarietyIndex);
			VarietyRow->SetStringField(TEXT("grass_mesh"), SourceVariety.GrassMesh->GetPathName());
			VarietyRow->SetBoolField(TEXT("non_material_properties_exact"), true);
			TArray<TSharedPtr<FJsonValue>> ComparedFieldNames;
			for (TFieldIterator<FProperty> PropertyIt(FGrassVariety::StaticStruct()); PropertyIt; ++PropertyIt)
			{
				if (PropertyIt->GetFName() != GET_MEMBER_NAME_CHECKED(FGrassVariety, OverrideMaterials))
				{
					ComparedFieldNames.Add(MakeShared<FJsonValueString>(PropertyIt->GetName()));
				}
			}
			VarietyRow->SetArrayField(TEXT("non_material_fields_compared"), ComparedFieldNames);
			TArray<TSharedPtr<FJsonValue>> SlotRows;
			for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
			{
				UMaterialInterface* SourceMaterial = GetSourceMaterialForSlot(SourceVariety, SlotIndex);
				UMaterialInstanceConstant* ExpectedMIC = nullptr;
				FString RedirectError;
				if (!GetRedirectedMaterial(SourceMaterial, SourceToPluginMIC, ExpectedMIC, RedirectError))
				{
					OutDifference = FString::Printf(TEXT("GrassVarieties[%d] slot %d: %s"), VarietyIndex, SlotIndex, *RedirectError);
					return false;
				}
				UMaterialInterface* ActualOverride = CopyVariety.OverrideMaterials[SlotIndex].Get();
				if (ActualOverride != ExpectedMIC)
				{
					OutDifference = FString::Printf(TEXT("GrassVarieties[%d] slot %d does not bind its exact plugin MIC."), VarietyIndex, SlotIndex);
					return false;
				}

				TSharedRef<FJsonObject> SlotRow = MakeShared<FJsonObject>();
				SlotRow->SetNumberField(TEXT("slot"), SlotIndex);
				SlotRow->SetStringField(TEXT("source_material"), SourceMaterial->GetPathName());
				SlotRow->SetStringField(TEXT("plugin_mic"), ExpectedMIC->GetPathName());
				SlotRow->SetStringField(TEXT("plugin_physical_material"), ExpectedMIC->GetPhysicalMaterial()->GetPathName());
				SlotRows.Add(MakeShared<FJsonValueObject>(SlotRow));
			}

			for (int32 SlotIndex = SlotCount; SlotIndex < SourceVariety.OverrideMaterials.Num(); ++SlotIndex)
			{
				if (CopyVariety.OverrideMaterials[SlotIndex] != SourceVariety.OverrideMaterials[SlotIndex])
				{
					OutDifference = FString::Printf(TEXT("GrassVarieties[%d] unused override slot %d changed."), VarietyIndex, SlotIndex);
					return false;
				}
			}
			VarietyRow->SetArrayField(TEXT("actual_mesh_slots"), SlotRows);
			OutVarietyRows.Add(MakeShared<FJsonValueObject>(VarietyRow));
		}
		return true;
	}

	static bool BuildExpectedOverrides(
		const ULandscapeGrassType* Source,
		const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
		TArray<TArray<UMaterialInterface*>>& OutOverrides,
		FString& OutError)
	{
		OutOverrides.Reset();
		if (!Source || Source->GrassVarieties.IsEmpty())
		{
			OutError = TEXT("The source GrassType has no varieties to bind to plugin materials.");
			return false;
		}
		OutOverrides.SetNum(Source->GrassVarieties.Num());
		for (int32 VarietyIndex = 0; VarietyIndex < Source->GrassVarieties.Num(); ++VarietyIndex)
		{
			const FGrassVariety& Variety = Source->GrassVarieties[VarietyIndex];
			const int32 SlotCount = GetGrassMeshSlotCount(Variety);
			if (!IsValid(Variety.GrassMesh.Get()) || SlotCount <= 0)
			{
				OutError = FString::Printf(TEXT("GrassVarieties[%d] has no valid mesh or material slots."), VarietyIndex);
				return false;
			}
			OutOverrides[VarietyIndex].SetNum(FMath::Max(Variety.OverrideMaterials.Num(), SlotCount));
			for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
			{
				UMaterialInstanceConstant* Redirect = nullptr;
				if (!GetRedirectedMaterial(GetSourceMaterialForSlot(Variety, SlotIndex), SourceToPluginMIC, Redirect, OutError))
				{
					OutError = FString::Printf(TEXT("GrassVarieties[%d] slot %d: %s"), VarietyIndex, SlotIndex, *OutError);
					return false;
				}
				OutOverrides[VarietyIndex][SlotIndex] = Redirect;
			}
			for (int32 SlotIndex = SlotCount; SlotIndex < Variety.OverrideMaterials.Num(); ++SlotIndex)
			{
				OutOverrides[VarietyIndex][SlotIndex] = Variety.OverrideMaterials[SlotIndex].Get();
			}
		}
		return true;
	}

	static bool ApplyPropertyChange(ULandscapeGrassType* GrassType, FString& OutError)
	{
		FProperty* GrassVarietiesProperty = FindFProperty<FProperty>(
			ULandscapeGrassType::StaticClass(), GET_MEMBER_NAME_CHECKED(ULandscapeGrassType, GrassVarieties));
		if (!GrassVarietiesProperty)
		{
			OutError = TEXT("Could not resolve ULandscapeGrassType.GrassVarieties for the editor property-change path.");
			return false;
		}
		FPropertyChangedEvent ChangeEvent(GrassVarietiesProperty, EPropertyChangeType::ValueSet);
		GrassType->PostEditChangeProperty(ChangeEvent);
		const uint32 ComputedHash = ComputeOwnedGrassStateHash(GrassType);
		if (GrassType->StateHash != ComputedHash)
		{
			OutError = TEXT("GrassType PostEditChangeProperty did not refresh StateHash to its serialized content.");
			return false;
		}
		return true;
	}

	static bool SaveGrassType(ULandscapeGrassType* GrassType, FString& OutError)
	{
		UPackage* Package = GrassType ? GrassType->GetOutermost() : nullptr;
		if (!Package)
		{
			OutError = TEXT("Cannot save a missing plugin GrassType package.");
			return false;
		}
		const FString PackageName = Package->GetName();
		if (!PackageName.StartsWith(PluginRoot))
		{
			OutError = TEXT("Refusing to save a non-plugin GrassType package: ") + PackageName;
			return false;
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		Package->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, GrassType, *Filename, SaveArgs))
		{
			OutError = TEXT("Saving plugin GrassType package failed: ") + Filename;
			return false;
		}
		return true;
	}

	static bool CreateOrLoadGrassCopy(
		FIMGrassTypeRecord& Record,
		const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
		bool bPrepare,
		FString& OutError)
	{
		const FString PackageName = Record.CopyPackageName;
		UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *Record.CopyObjectPath, nullptr, LOAD_NoWarn);
		if (ExistingObject)
		{
			Record.Copy = Cast<ULandscapeGrassType>(ExistingObject);
			if (!Record.Copy)
			{
				OutError = TEXT("The expected plugin GrassType path is occupied by another object class: ") + Record.CopyObjectPath;
				return false;
			}
		}
		else if (bPrepare)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				Package = CreatePackage(*PackageName);
			}
			if (!Package)
			{
				OutError = TEXT("Could not create the plugin GrassType package: ") + PackageName;
				return false;
			}
			Record.Copy = Cast<ULandscapeGrassType>(StaticDuplicateObject(Record.Source, Package, Record.Source->GetFName()));
			if (!Record.Copy)
			{
				OutError = TEXT("Could not duplicate the source GrassType into its plugin package: ") + Record.SourceObjectPath;
				return false;
			}
			Record.Copy->SetFlags(RF_Public | RF_Standalone);
			Record.bCreated = true;
			FAssetRegistryModule::AssetCreated(Record.Copy);
		}
		else
		{
			OutError = TEXT("The expected plugin GrassType copy is missing: ") + Record.CopyObjectPath;
			return false;
		}

		FString Difference;
		if (!CompareNonMaterialGrassProperties(Record.Source, Record.Copy, Difference))
		{
			OutError = TEXT("Existing plugin GrassType copy failed exact non-material comparison: ") + Difference;
			return false;
		}

		TArray<TArray<UMaterialInterface*>> ExpectedOverrides;
		if (!BuildExpectedOverrides(Record.Source, SourceToPluginMIC, ExpectedOverrides, OutError))
		{
			return false;
		}

		if (Record.bCreated)
		{
			for (int32 VarietyIndex = 0; VarietyIndex < Record.Copy->GrassVarieties.Num(); ++VarietyIndex)
			{
				FGrassVariety& CopyVariety = Record.Copy->GrassVarieties[VarietyIndex];
				CopyVariety.OverrideMaterials.SetNum(ExpectedOverrides[VarietyIndex].Num());
				for (int32 SlotIndex = 0; SlotIndex < ExpectedOverrides[VarietyIndex].Num(); ++SlotIndex)
				{
					CopyVariety.OverrideMaterials[SlotIndex] = ExpectedOverrides[VarietyIndex][SlotIndex];
				}
			}
			if (!ApplyPropertyChange(Record.Copy, OutError))
			{
				return false;
			}
		}
		else if (bPrepare && Record.Copy->StateHash != ComputeOwnedGrassStateHash(Record.Copy))
		{
			if (!ApplyPropertyChange(Record.Copy, OutError))
			{
				return false;
			}
		}

		TArray<TSharedPtr<FJsonValue>> VarietyRows;
		if (!CompareMaterialOverrides(Record.Source, Record.Copy, SourceToPluginMIC, VarietyRows, Difference))
		{
			OutError = TEXT("Plugin GrassType material slots failed comparison: ") + Difference;
			return false;
		}
		if (Record.Copy->GetName() != Record.Source->GetName())
		{
			OutError = TEXT("Plugin GrassType short UObject name differs from the source asset.");
			return false;
		}
		if (Record.Copy->GetPathName() != Record.CopyObjectPath)
		{
			OutError = TEXT("Plugin GrassType object path differs from its source-hash copy path.");
			return false;
		}
		if (Record.Copy->StateHash != ComputeOwnedGrassStateHash(Record.Copy))
		{
			OutError = TEXT("Plugin GrassType StateHash does not match its serialized content.");
			return false;
		}
		if (bPrepare && !SaveGrassType(Record.Copy, OutError))
		{
			return false;
		}
		Record.VarietyRows = MoveTemp(VarietyRows);
		return true;
	}

	static bool BuildSourceBindings(
		UWorld* World,
		TArray<FIMLandscapeGrassBinding>& OutBindings,
		TMap<ALandscapeProxy*, TMap<FName, ULandscapeGrassType*>>& OutExpectedByProxy,
		TMap<ULandscapeComponent*, TMap<FName, ULandscapeGrassType*>>& OutExpectedByComponent,
		TMap<FString, FIMGrassTypeRecord>& OutRecords,
		FString& OutError)
	{
		TArray<ALandscapeProxy*> Proxies;
		for (TObjectIterator<ALandscapeProxy> It; It; ++It)
		{
			ALandscapeProxy* Proxy = *It;
			if (IsValid(Proxy) && !Proxy->HasAnyFlags(RF_ClassDefaultObject) && Proxy->GetWorld() == World)
			{
				Proxies.Add(Proxy);
			}
		}
		if (Proxies.IsEmpty())
		{
			OutError = TEXT("The task world contains no loaded ALandscapeProxy.");
			return false;
		}

		for (ALandscapeProxy* Proxy : Proxies)
		{
			for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
			{
				if (!IsValid(Component))
				{
					continue;
				}
				UMaterialInterface* LandscapeMaterial = Component->GetLandscapeMaterial();
				UMaterial* BaseMaterial = LandscapeMaterial ? LandscapeMaterial->GetMaterial() : nullptr;
				if (!BaseMaterial)
				{
					OutError = TEXT("A task LandscapeComponent has no base Landscape material: ") + Component->GetPathName();
					return false;
				}

				const TMap<FName, TObjectPtr<ULandscapeGrassType>>& NamedTypes = BaseMaterial->GetCachedExpressionData().NamedGrassTypes;
				for (const TPair<FName, TObjectPtr<ULandscapeGrassType>>& Pair : NamedTypes)
				{
					ULandscapeGrassType* Source = Pair.Value.Get();
					if (Pair.Key.IsNone() || !IsValid(Source))
					{
						OutError = TEXT("The base Landscape material contains a missing or unnamed GrassType output.");
						return false;
					}
					const FString SourcePackageName = Source->GetOutermost()->GetName();
					if (!SourcePackageName.StartsWith(TEXT("/Game/")))
					{
						OutError = TEXT("Only source GrassType assets under /Game may be copied: ") + Source->GetPathName();
						return false;
					}

					FIMLandscapeGrassBinding Binding;
					Binding.Proxy = Proxy;
					Binding.Component = Component;
					Binding.OutputName = Pair.Key;
					Binding.Source = Source;
					Binding.SourceObjectPath = Source->GetPathName();
					OutBindings.Add(Binding);

					ULandscapeGrassType*& ProxyType = OutExpectedByProxy.FindOrAdd(Proxy).FindOrAdd(Pair.Key);
					if (ProxyType && ProxyType != Source)
					{
						OutError = TEXT("One Landscape proxy uses the same grass output name for different source GrassTypes: ") + Pair.Key.ToString();
						return false;
					}
					ProxyType = Source;
					OutExpectedByComponent.FindOrAdd(Component).Add(Pair.Key, Source);

					if (!OutRecords.Contains(Binding.SourceObjectPath))
					{
						FIMGrassTypeRecord Record;
						Record.Source = Source;
						Record.SourceObjectPath = Binding.SourceObjectPath;
						Record.SourceHash.PackageName = SourcePackageName;
						Record.SourceHash.bDirtyBefore = Source->GetOutermost()->IsDirty();
						if (!HashPackageFile(SourcePackageName, Record.SourceHash.Filename, Record.SourceHash.Sha1Before,
							Record.SourceHash.ByteCount, OutError))
						{
							return false;
						}
						if (Record.SourceHash.bDirtyBefore)
						{
							OutError = TEXT("Source GrassType package is dirty, so its on-disk SHA1 cannot identify the in-memory content: ") + SourcePackageName;
							return false;
						}
						Record.CopyPackageName = FString(PluginGrassRoot) + Record.SourceHash.Sha1Before + TEXT("/") + Source->GetName();
						Record.CopyObjectPath = Record.CopyPackageName + TEXT(".") + Source->GetName();
						OutRecords.Add(Binding.SourceObjectPath, MoveTemp(Record));
					}
				}
			}
		}

		if (OutBindings.IsEmpty())
		{
			OutError = TEXT("No named GrassTypes are declared by the task world's Landscape base materials.");
			return false;
		}

		for (const FIMLandscapeGrassBinding& Binding : OutBindings)
		{
			FIMGrassTypeRecord& Record = OutRecords.FindChecked(Binding.SourceObjectPath);
			const TObjectPtr<ULandscapeGrassType>* ExistingOverride = Binding.Proxy->GrassTypesOverrides.Find(FLandscapeGrassName(Binding.OutputName));
			if (ExistingOverride && ExistingOverride->Get() != Binding.Source
				&& (!ExistingOverride->Get() || ExistingOverride->Get()->GetPathName() != Record.CopyObjectPath))
			{
				OutError = TEXT("An existing Landscape GrassTypesOverrides entry is neither the source nor this helper's exact expected copy: ")
					+ Binding.Proxy->GetPathName() + TEXT(" / ") + Binding.OutputName.ToString();
				return false;
			}
		}

		return true;
	}

	static void BuildTypeSummary(const TMap<FString, FIMGrassTypeRecord>& Records, TSharedRef<FJsonObject> Summary)
	{
		TArray<FString> SourcePaths;
		Records.GetKeys(SourcePaths);
		SourcePaths.Sort();
		TArray<TSharedPtr<FJsonValue>> TypeRows;
		for (const FString& SourcePath : SourcePaths)
		{
			const FIMGrassTypeRecord& Record = Records.FindChecked(SourcePath);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("source_grass_type"), SourcePath);
			Row->SetStringField(TEXT("source_package"), Record.SourceHash.PackageName);
			Row->SetStringField(TEXT("source_package_filename"), Record.SourceHash.Filename);
			Row->SetStringField(TEXT("source_package_sha1_before"), Record.SourceHash.Sha1Before);
			Row->SetStringField(TEXT("source_package_sha1_after"), Record.SourceHash.Sha1After);
			Row->SetNumberField(TEXT("source_package_bytes"), Record.SourceHash.ByteCount);
			Row->SetBoolField(TEXT("source_package_dirty_before"), Record.SourceHash.bDirtyBefore);
			Row->SetBoolField(TEXT("source_package_dirty_after"), Record.SourceHash.bDirtyAfter);
			Row->SetStringField(TEXT("plugin_copy"), Record.CopyObjectPath);
			Row->SetStringField(TEXT("source_short_name"), Record.Source ? Record.Source->GetName() : FString());
			Row->SetStringField(TEXT("copy_short_name"), Record.Copy ? Record.Copy->GetName() : FString());
			Row->SetBoolField(TEXT("short_name_exact"), Record.Source && Record.Copy && Record.Source->GetName() == Record.Copy->GetName());
			Row->SetNumberField(TEXT("variety_count"), Record.Source ? Record.Source->GrassVarieties.Num() : 0);
			Row->SetNumberField(TEXT("source_state_hash"), Record.Source ? Record.Source->StateHash : 0);
			Row->SetNumberField(TEXT("copy_state_hash"), Record.Copy ? Record.Copy->StateHash : 0);
			Row->SetNumberField(TEXT("copy_computed_state_hash"), Record.Copy ? ComputeOwnedGrassStateHash(Record.Copy) : 0);
			FString Difference;
			Row->SetBoolField(TEXT("non_material_properties_exact"), CompareNonMaterialGrassProperties(Record.Source, Record.Copy, Difference));
			Row->SetStringField(TEXT("first_non_material_difference"), Difference);
			Row->SetArrayField(TEXT("variety_attributes"), Record.VarietyRows);
			TypeRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Summary->SetArrayField(TEXT("grass_types"), TypeRows);
	}

	static bool UpdateAndVerifyBindings(
		TMap<ALandscapeProxy*, TMap<FName, ULandscapeGrassType*>>& ExpectedByProxy,
		TMap<ULandscapeComponent*, TMap<FName, ULandscapeGrassType*>>& ExpectedByComponent,
		bool bPrepare,
		FString& OutError)
	{
		if (bPrepare)
		{
			FProperty* OverridesProperty = FindFProperty<FProperty>(
				ALandscapeProxy::StaticClass(), GET_MEMBER_NAME_CHECKED(ALandscapeProxy, GrassTypesOverrides));
			if (!OverridesProperty)
			{
				OutError = TEXT("Could not resolve ALandscapeProxy.GrassTypesOverrides editor property.");
				return false;
			}

			for (TPair<ALandscapeProxy*, TMap<FName, ULandscapeGrassType*>>& ProxyRow : ExpectedByProxy)
			{
				ALandscapeProxy* Proxy = ProxyRow.Key;
				bool bChanged = false;
				for (const TPair<FName, ULandscapeGrassType*>& TypeRow : ProxyRow.Value)
				{
					const FLandscapeGrassName Key(TypeRow.Key);
					const TObjectPtr<ULandscapeGrassType>* Existing = Proxy->GrassTypesOverrides.Find(Key);
					if (!Existing || Existing->Get() != TypeRow.Value)
					{
						Proxy->Modify();
						Proxy->GrassTypesOverrides.Add(Key, TypeRow.Value);
						bChanged = true;
					}
				}
				if (bChanged)
				{
					FPropertyChangedEvent ChangeEvent(OverridesProperty, EPropertyChangeType::ValueSet);
					Proxy->PostEditChangeProperty(ChangeEvent);
					Proxy->GetOutermost()->MarkPackageDirty();
				}
			}
		}

		for (TPair<ULandscapeComponent*, TMap<FName, ULandscapeGrassType*>>& ComponentRow : ExpectedByComponent)
		{
			ULandscapeComponent* Component = ComponentRow.Key;
			if (!IsValid(Component))
			{
				OutError = TEXT("A LandscapeComponent disappeared while refreshing NamedGrassTypes.");
				return false;
			}
			Component->UpdateGrassTypes(true);
			for (const TPair<FName, ULandscapeGrassType*>& TypeRow : ComponentRow.Value)
			{
				const TObjectPtr<ULandscapeGrassType>* Resolved = Component->GetNamedGrassTypes().Find(TypeRow.Key);
				if (!Resolved || Resolved->Get() != TypeRow.Value)
				{
					OutError = TEXT("LandscapeComponent.NamedGrassTypes did not resolve the exact plugin GrassType copy: ")
						+ Component->GetPathName() + TEXT(" / ") + TypeRow.Key.ToString();
					return false;
				}
			}
		}
		return true;
	}

	static bool ValidateGeneratedGrassSlots(
		UWorld* World,
		const TSet<UMaterialInterface*>& ExpectedMICs,
		const TMap<ALandscapeProxy*, TMap<FName, ULandscapeGrassType*>>& ExpectedByProxy,
		TSharedRef<FJsonObject> Summary,
		FString& OutError)
	{
		int32 GrassComponentCount = 0;
		int32 ComponentsWithInstances = 0;
		int64 TotalInstances = 0;
		TArray<TSharedPtr<FJsonValue>> ComponentRows;
		for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
		{
			UInstancedStaticMeshComponent* Component = *It;
			if (!IsValid(Component) || Component->GetWorld() != World || Component->GetClass()->GetName() != GrassComponentClassName)
			{
				continue;
			}

			++GrassComponentCount;
			const int32 InstanceCount = Component->GetInstanceCount();
			TotalInstances += InstanceCount;
			ComponentsWithInstances += InstanceCount > 0 ? 1 : 0;
			TSharedRef<FJsonObject> ComponentRow = MakeShared<FJsonObject>();
			ComponentRow->SetStringField(TEXT("component"), Component->GetPathName());
			ComponentRow->SetNumberField(TEXT("instance_count"), InstanceCount);
			// Membership in the global redirect table is insufficient: a known
			// Wood MIC on the wrong mesh/slot must still fail. Require the whole
			// material vector of a variety owned by this component's producer.
			ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Component->GetOwner());
			const TMap<FName, ULandscapeGrassType*>* Types = ExpectedByProxy.Find(Proxy);
			bool bExactProducerVariety = false;
			if (Types)
			{
				for (const auto& Pair : *Types)
				{
					for (int32 VarietyIndex = 0; VarietyIndex < Pair.Value->GrassVarieties.Num(); ++VarietyIndex)
					{
						const FGrassVariety& Variety = Pair.Value->GrassVarieties[VarietyIndex];
						if (Variety.GrassMesh != Component->GetStaticMesh()
							|| GetGrassMeshSlotCount(Variety) != Component->GetNumMaterials()) continue;
						bool bAllSlotsExact = true;
						for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
							bAllSlotsExact &= Component->GetMaterial(Slot) == GetSourceMaterialForSlot(Variety, Slot);
						if (bAllSlotsExact)
						{
							bExactProducerVariety = true;
							ComponentRow->SetStringField(TEXT("matching_output_key"), Pair.Key.ToString());
							ComponentRow->SetStringField(TEXT("matching_plugin_grass_type"), Pair.Value->GetPathName());
							ComponentRow->SetNumberField(TEXT("matching_variety_index"), VarietyIndex);
							break;
						}
					}
					if (bExactProducerVariety) break;
				}
			}
			ComponentRow->SetBoolField(TEXT("complete_material_vector_matches_producer_variety"), bExactProducerVariety);

			TArray<TSharedPtr<FJsonValue>> SlotRows;
			if (Component->GetNumMaterials() <= 0)
			{
				OutError = TEXT("An existing generated Grass component has no material slots to validate: ") + Component->GetPathName();
				Summary->SetNumberField(TEXT("generated_grass_component_count"), GrassComponentCount);
				Summary->SetNumberField(TEXT("generated_grass_components_with_instances"), ComponentsWithInstances);
				Summary->SetNumberField(TEXT("generated_grass_instance_count"), TotalInstances);
				Summary->SetBoolField(TEXT("actual_generation_not_observed"), ComponentsWithInstances == 0);
				Summary->SetStringField(TEXT("geometry_instance_gate"), TEXT("FAIL_NO_MATERIAL_SLOTS"));
				return false;
			}
			for (int32 SlotIndex = 0; SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
			{
				UMaterialInterface* ActualMaterial = Component->GetMaterial(SlotIndex);
				UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(ActualMaterial);
				const bool bIsExpectedMIC = MIC && ExpectedMICs.Contains(MIC);
				const bool bHasExactPhysicalMaterial = bIsExpectedMIC && MIC->PhysMaterial
					&& MIC->GetPhysicalMaterial() == MIC->PhysMaterial.Get()
					&& MIC->PhysMaterial->GetOutermost()->GetName().StartsWith(PluginRoot);
				TSharedRef<FJsonObject> SlotRow = MakeShared<FJsonObject>();
				SlotRow->SetNumberField(TEXT("slot"), SlotIndex);
				SlotRow->SetStringField(TEXT("material"), ActualMaterial ? ActualMaterial->GetPathName() : FString());
				SlotRow->SetStringField(TEXT("physical_material"), bIsExpectedMIC && MIC->GetPhysicalMaterial() ? MIC->GetPhysicalMaterial()->GetPathName() : FString());
				SlotRow->SetBoolField(TEXT("plugin_mic_exact"), bIsExpectedMIC);
				SlotRow->SetBoolField(TEXT("plugin_physical_material_exact"), bHasExactPhysicalMaterial);
				SlotRows.Add(MakeShared<FJsonValueObject>(SlotRow));
				if (!bIsExpectedMIC || !bHasExactPhysicalMaterial || !bExactProducerVariety)
				{
					OutError = TEXT("An existing generated Grass component slot does not resolve to an exact redirected plugin MIC/PhysicalMaterial: ")
						+ Component->GetPathName() + TEXT(" slot ") + FString::FromInt(SlotIndex);
					ComponentRow->SetArrayField(TEXT("slots"), SlotRows);
					ComponentRows.Add(MakeShared<FJsonValueObject>(ComponentRow));
					Summary->SetArrayField(TEXT("generated_grass_components"), ComponentRows);
					Summary->SetNumberField(TEXT("generated_grass_component_count"), GrassComponentCount);
					Summary->SetNumberField(TEXT("generated_grass_components_with_instances"), ComponentsWithInstances);
					Summary->SetNumberField(TEXT("generated_grass_instance_count"), TotalInstances);
					Summary->SetBoolField(TEXT("actual_generation_not_observed"), ComponentsWithInstances == 0);
					Summary->SetStringField(TEXT("geometry_instance_gate"), TEXT("FAIL_SLOT_MISMATCH"));
					return false;
				}
			}
			ComponentRow->SetArrayField(TEXT("slots"), SlotRows);
			ComponentRows.Add(MakeShared<FJsonValueObject>(ComponentRow));
		}

		Summary->SetArrayField(TEXT("generated_grass_components"), ComponentRows);
		Summary->SetNumberField(TEXT("generated_grass_component_count"), GrassComponentCount);
		Summary->SetNumberField(TEXT("generated_grass_components_with_instances"), ComponentsWithInstances);
		Summary->SetNumberField(TEXT("generated_grass_instance_count"), TotalInstances);
		const bool bObservedInstances = ComponentsWithInstances > 0 && TotalInstances > 0;
		Summary->SetBoolField(TEXT("actual_generation_not_observed"), !bObservedInstances);
		Summary->SetStringField(TEXT("geometry_instance_gate"), bObservedInstances ? TEXT("SLOTS_PASS_INSTANCES_PRESENT") : TEXT("NOT_OBSERVED"));
		return true;
	}

	static bool RunLandscapeGrassMaterialCopies(
		UWorld* World,
		const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
		bool bPrepare,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		OutError.Reset();
		OutSummary = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Summary = OutSummary.ToSharedRef();
		Summary->SetStringField(TEXT("status"), TEXT("IN_PROGRESS"));
		Summary->SetStringField(TEXT("mode"), bPrepare ? TEXT("prepare") : TEXT("validate"));
		Summary->SetStringField(TEXT("task_world_package"), IsValid(World) ? World->GetOutermost()->GetName() : FString());
		Summary->SetBoolField(TEXT("task_map_saved"), false);
		Summary->SetBoolField(TEXT("map_marked_dirty_by_prepare"), false);
		Summary->SetBoolField(TEXT("producer_binding_pass"), false);
		Summary->SetBoolField(TEXT("actual_generation_not_observed"), true);
		Summary->SetStringField(TEXT("geometry_instance_gate"), TEXT("NOT_OBSERVED"));

		auto Fail = [&Summary, &OutError](const FString& Error)
		{
			OutError = Error;
			Summary->SetStringField(TEXT("status"), TEXT("FAIL_CLOSED"));
			Summary->SetStringField(TEXT("error"), Error);
			return false;
		};

		if (!IsValid(World))
		{
			return Fail(TEXT("A live UWorld is required."));
		}
		if (!IsInGameThread()) return Fail(TEXT("Grass producer preparation and validation require the game thread."));
		if (World->GetOutermost()->GetName() != TaskWorldPackage)
		{
			return Fail(TEXT("Refusing to prepare or validate GrassType copies outside the exact task-owned preview world: ")
				+ World->GetOutermost()->GetName());
		}

		TSet<UMaterialInterface*> ExpectedMICs;
		if (!ValidateRedirectTable(SourceToPluginMIC, ExpectedMICs, OutError))
		{
			return Fail(OutError);
		}

		TArray<FIMLandscapeGrassBinding> Bindings;
		TMap<ALandscapeProxy*, TMap<FName, ULandscapeGrassType*>> ExpectedByProxy;
		TMap<ULandscapeComponent*, TMap<FName, ULandscapeGrassType*>> ExpectedByComponent;
		TMap<FString, FIMGrassTypeRecord> Records;
		if (!BuildSourceBindings(World, Bindings, ExpectedByProxy, ExpectedByComponent, Records, OutError))
		{
			return Fail(OutError);
		}

		// Preflight all source variety material slots before creating or changing any plugin assets.
		for (TPair<FString, FIMGrassTypeRecord>& RecordRow : Records)
		{
			TArray<TArray<UMaterialInterface*>> UnusedOverrides;
			if (!BuildExpectedOverrides(RecordRow.Value.Source, SourceToPluginMIC, UnusedOverrides, OutError))
			{
				return Fail(OutError);
			}
		}

		for (TPair<FString, FIMGrassTypeRecord>& RecordRow : Records)
		{
			if (!CreateOrLoadGrassCopy(RecordRow.Value, SourceToPluginMIC, bPrepare, OutError))
			{
				BuildTypeSummary(Records, Summary);
				return Fail(OutError);
			}
			for (TPair<ALandscapeProxy*, TMap<FName, ULandscapeGrassType*>>& ProxyRow : ExpectedByProxy)
			{
				for (TPair<FName, ULandscapeGrassType*>& TypeRow : ProxyRow.Value)
				{
					if (TypeRow.Value == RecordRow.Value.Source)
					{
						TypeRow.Value = RecordRow.Value.Copy;
					}
				}
			}
			for (TPair<ULandscapeComponent*, TMap<FName, ULandscapeGrassType*>>& ComponentRow : ExpectedByComponent)
			{
				for (TPair<FName, ULandscapeGrassType*>& TypeRow : ComponentRow.Value)
				{
					if (TypeRow.Value == RecordRow.Value.Source)
					{
						TypeRow.Value = RecordRow.Value.Copy;
					}
				}
			}
		}

		if (bPrepare)
		{
			if (!UpdateAndVerifyBindings(ExpectedByProxy, ExpectedByComponent, true, OutError))
			{
				BuildTypeSummary(Records, Summary);
				return Fail(OutError);
			}
			World->GetOutermost()->MarkPackageDirty();
			Summary->SetBoolField(TEXT("map_marked_dirty_by_prepare"), true);
		}
		else if (!UpdateAndVerifyBindings(ExpectedByProxy, ExpectedByComponent, false, OutError))
		{
			BuildTypeSummary(Records, Summary);
			return Fail(OutError);
		}

		for (TPair<FString, FIMGrassTypeRecord>& RecordRow : Records)
		{
			FIMGrassSourceHash& Hash = RecordRow.Value.SourceHash;
			if (!HashPackageFile(Hash.PackageName, Hash.Filename, Hash.Sha1After, Hash.ByteCount, OutError))
			{
				BuildTypeSummary(Records, Summary);
				return Fail(OutError);
			}
			Hash.bDirtyAfter = RecordRow.Value.Source->GetOutermost()->IsDirty();
			if (Hash.Sha1After != Hash.Sha1Before || Hash.bDirtyAfter != Hash.bDirtyBefore)
			{
				BuildTypeSummary(Records, Summary);
				return Fail(TEXT("A source /Game GrassType package changed while preparing or validating plugin copies: ") + Hash.PackageName);
			}
		}

		BuildTypeSummary(Records, Summary);
		TArray<TSharedPtr<FJsonValue>> BindingRows;
		for (const FIMLandscapeGrassBinding& Binding : Bindings)
		{
			const FIMGrassTypeRecord& Record = Records.FindChecked(Binding.SourceObjectPath);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("landscape_proxy"), Binding.Proxy->GetPathName());
			Row->SetStringField(TEXT("landscape_component"), Binding.Component->GetPathName());
			Row->SetStringField(TEXT("output_name"), Binding.OutputName.ToString());
			Row->SetStringField(TEXT("source_grass_type"), Binding.SourceObjectPath);
			Row->SetStringField(TEXT("resolved_plugin_grass_type"), Record.Copy ? Record.Copy->GetPathName() : FString());
			const TObjectPtr<ULandscapeGrassType>* Resolved = Binding.Component->GetNamedGrassTypes().Find(Binding.OutputName);
			Row->SetBoolField(TEXT("source_to_plugin_binding_exact"), Record.Copy && Resolved && Resolved->Get() == Record.Copy);
			BindingRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Summary->SetArrayField(TEXT("grass_output_bindings"), BindingRows);
		Summary->SetNumberField(TEXT("distinct_grass_type_count"), Records.Num());
		Summary->SetNumberField(TEXT("grass_output_binding_count"), Bindings.Num());
		Summary->SetBoolField(TEXT("producer_binding_pass"), true);
		Summary->SetBoolField(TEXT("source_grass_type_packages_unchanged"), true);
		if (!ValidateGeneratedGrassSlots(World, ExpectedMICs, ExpectedByProxy, Summary, OutError))
		{
			return Fail(OutError);
		}

		Summary->SetStringField(TEXT("status"), Summary->GetBoolField(TEXT("actual_generation_not_observed"))
			? TEXT("PRODUCER_PASS_GENERATION_NOT_OBSERVED") : TEXT("PRODUCER_AND_EXISTING_GRASS_SLOTS_PASS"));
		Summary->SetStringField(TEXT("interpretation"), TEXT("Producer material/type binding was checked. This status does not certify grass instance geometry or a preview goal."));
		return true;
	}
}

bool IMPrepareLandscapeGrassMaterialCopies(
	UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
	TSharedPtr<FJsonObject>& OutSummary,
	FString& OutError)
{
	return IMAcousticLandscapeGrassMaterialsPrivate::RunLandscapeGrassMaterialCopies(World, SourceToPluginMIC, true, OutSummary, OutError);
}

bool IMValidateLandscapeGrassMaterialCopies(
	UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
	TSharedPtr<FJsonObject>& OutSummary,
	FString& OutError)
{
	return IMAcousticLandscapeGrassMaterialsPrivate::RunLandscapeGrassMaterialCopies(World, SourceToPluginMIC, false, OutSummary, OutError);
}

namespace IMAcousticLandscapeGrassMaterialsPrivate
{
	static constexpr int32 MaxMaterialParentDepth = 64;

	struct FIMGrassInventoryType
	{
		ULandscapeGrassType* Source = nullptr;
		TArray<TSharedPtr<FJsonValue>> Bindings;
	};

	static bool InspectMaterialParentChain(
		UMaterialInterface* ProducerMaterial,
		TArray<TSharedPtr<FJsonValue>>& OutChain,
		FString& OutBaseMaterialPath,
		FString& OutError,
		TMap<UPackage*, bool>& InOutPackageDirtyBefore)
	{
		TSet<const UMaterialInterface*> Visited;
		UMaterialInterface* Current = ProducerMaterial;
		for (int32 Depth = 0; Depth < MaxMaterialParentDepth; ++Depth)
		{
			if (!IsValid(Current))
			{
				OutError = TEXT("Material parent chain ended at a null or invalid UMaterialInterface.");
				return false;
			}
			if (Visited.Contains(Current))
			{
				OutError = TEXT("Material parent chain contains a cycle at ") + Current->GetPathName();
				return false;
			}
			Visited.Add(Current);
			UPackage* Package = Current->GetOutermost();
			if (Package && !InOutPackageDirtyBefore.Contains(Package))
			{
				InOutPackageDirtyBefore.Add(Package, Package->IsDirty());
			}
			OutChain.Add(MakeShared<FJsonValueString>(Current->GetPathName()));

			if (Cast<UMaterial>(Current))
			{
				OutBaseMaterialPath = Current->GetPathName();
				return true;
			}
			UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
			if (!Instance)
			{
				OutError = TEXT("Material parent chain reached an unsupported UMaterialInterface subclass: ") + Current->GetPathName();
				return false;
			}
			Current = Instance->Parent.Get();
		}

		OutError = FString::Printf(TEXT("Material parent chain exceeded the bounded depth of %d."), MaxMaterialParentDepth);
		return false;
	}

	static void TrackPackageDirtyState(const UObject* Object, TMap<UPackage*, bool>& InOutPackageDirtyBefore)
	{
		UPackage* Package = Object ? Object->GetOutermost() : nullptr;
		if (Package && !InOutPackageDirtyBefore.Contains(Package))
		{
			InOutPackageDirtyBefore.Add(Package, Package->IsDirty());
		}
	}

	static bool InspectLandscapeGrassMaterialSources(
		UWorld* World,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		OutError.Reset();
		OutSummary = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Summary = OutSummary.ToSharedRef();
		Summary->SetStringField(TEXT("status"), TEXT("IN_PROGRESS"));
		Summary->SetStringField(TEXT("task_world_package"), IsValid(World) ? World->GetOutermost()->GetName() : FString());
		Summary->SetBoolField(TEXT("read_only"), true);
		Summary->SetBoolField(TEXT("asset_creation_invoked"), false);
		Summary->SetBoolField(TEXT("asset_save_invoked"), false);
		Summary->SetBoolField(TEXT("map_save_invoked"), false);
		Summary->SetBoolField(TEXT("update_grass_types_invoked"), false);
		Summary->SetBoolField(TEXT("post_edit_change_invoked"), false);
		Summary->SetBoolField(TEXT("state_hash_write_invoked"), false);
		Summary->SetBoolField(TEXT("redirect_applied"), false);
		Summary->SetBoolField(TEXT("dirty_state_check_complete"), false);

		auto Fail = [&Summary, &OutError](const FString& Error)
		{
			OutError = Error;
			Summary->SetStringField(TEXT("status"), TEXT("FAIL_CLOSED"));
			Summary->SetStringField(TEXT("error"), Error);
			return false;
		};
		if (!IsInGameThread())
		{
			return Fail(TEXT("Grass source inventory requires the Game Thread; it reads editor-owned UObject graphs."));
		}
		if (!IsValid(World))
		{
			return Fail(TEXT("A live task UWorld is required for Grass source inventory."));
		}
		if (World->GetOutermost()->GetName() != TaskWorldPackage)
		{
			return Fail(TEXT("Refusing to inventory Grass sources outside the exact task-owned preview world: ")
				+ World->GetOutermost()->GetName());
		}

		TMap<UPackage*, bool> PackageDirtyBefore;
		TrackPackageDirtyState(World, PackageDirtyBefore);
		TMap<FString, FIMGrassInventoryType> TypesByPath;
		TSet<FString> UniqueProxyPaths;
		TSet<FString> UniqueComponentPaths;
		TSet<FString> UniqueBaseMaterialPaths;
		TSet<FString> UniqueProducerMaterialPaths;
		TSet<FString> UniqueBindings;
		TArray<FString> Errors;

		for (TObjectIterator<ALandscapeProxy> ProxyIt; ProxyIt; ++ProxyIt)
		{
			ALandscapeProxy* Proxy = *ProxyIt;
			if (!IsValid(Proxy) || Proxy->HasAnyFlags(RF_ClassDefaultObject) || Proxy->GetWorld() != World)
			{
				continue;
			}
			TrackPackageDirtyState(Proxy, PackageDirtyBefore);
			UniqueProxyPaths.Add(Proxy->GetPathName());
			for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
			{
				if (!IsValid(Component))
				{
					Errors.Add(TEXT("The task LandscapeProxy contains an invalid LandscapeComponent reference."));
					continue;
				}
				TrackPackageDirtyState(Component, PackageDirtyBefore);
				UniqueComponentPaths.Add(Component->GetPathName());
				UMaterialInterface* LandscapeMaterial = Component->GetLandscapeMaterial();
				if (!IsValid(LandscapeMaterial))
				{
					Errors.Add(TEXT("A task LandscapeComponent has no readable Landscape material: ") + Component->GetPathName());
					continue;
				}
				TrackPackageDirtyState(LandscapeMaterial, PackageDirtyBefore);
				UMaterial* BaseMaterial = LandscapeMaterial->GetMaterial();
				if (!IsValid(BaseMaterial))
				{
					Errors.Add(TEXT("A task Landscape material did not resolve to a base UMaterial: ") + LandscapeMaterial->GetPathName());
					continue;
				}
				TrackPackageDirtyState(BaseMaterial, PackageDirtyBefore);
				UniqueBaseMaterialPaths.Add(BaseMaterial->GetPathName());

				const TMap<FName, TObjectPtr<ULandscapeGrassType>>& NamedTypes = BaseMaterial->GetCachedExpressionData().NamedGrassTypes;
				for (const TPair<FName, TObjectPtr<ULandscapeGrassType>>& TypePair : NamedTypes)
				{
					ULandscapeGrassType* Source = TypePair.Value.Get();
					if (TypePair.Key.IsNone() || !IsValid(Source))
					{
						Errors.Add(TEXT("A cached base-material grass output has a None name or invalid GrassType: ") + BaseMaterial->GetPathName());
						continue;
					}
					TrackPackageDirtyState(Source, PackageDirtyBefore);
					const FString SourcePath = Source->GetPathName();
					FIMGrassInventoryType& TypeRecord = TypesByPath.FindOrAdd(SourcePath);
					TypeRecord.Source = Source;

					const FString BindingIdentity = Proxy->GetPathName() + TEXT("|") + Component->GetPathName() + TEXT("|") + TypePair.Key.ToString();
					if (!UniqueBindings.Contains(BindingIdentity))
					{
						UniqueBindings.Add(BindingIdentity);
						TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
						Binding->SetStringField(TEXT("output_name"), TypePair.Key.ToString());
						Binding->SetStringField(TEXT("landscape_proxy"), Proxy->GetPathName());
						Binding->SetStringField(TEXT("landscape_component"), Component->GetPathName());
						Binding->SetStringField(TEXT("landscape_material_interface"), LandscapeMaterial->GetPathName());
						Binding->SetStringField(TEXT("base_material"), BaseMaterial->GetPathName());
						const TObjectPtr<ULandscapeGrassType>* ExistingOverride = Proxy->GrassTypesOverrides.Find(FLandscapeGrassName(TypePair.Key));
						Binding->SetStringField(TEXT("current_proxy_grass_override"), ExistingOverride && ExistingOverride->Get() ? ExistingOverride->Get()->GetPathName() : FString());
						TypeRecord.Bindings.Add(MakeShared<FJsonValueObject>(Binding));
					}
				}
			}
		}

		if (UniqueProxyPaths.IsEmpty() || UniqueComponentPaths.IsEmpty() || TypesByPath.IsEmpty())
		{
			Errors.Add(TEXT("The exact task world did not expose any complete Landscape Grass source bindings."));
		}

		TArray<FString> TypePaths;
		TypesByPath.GetKeys(TypePaths);
		TypePaths.Sort();
		TArray<TSharedPtr<FJsonValue>> TypeRows;
		for (const FString& TypePath : TypePaths)
		{
			FIMGrassInventoryType& TypeRecord = TypesByPath.FindChecked(TypePath);
			ULandscapeGrassType* Source = TypeRecord.Source;
			TrackPackageDirtyState(Source, PackageDirtyBefore);
			TSharedRef<FJsonObject> TypeRow = MakeShared<FJsonObject>();
			TypeRow->SetStringField(TEXT("source_grass_type"), TypePath);
			TypeRow->SetStringField(TEXT("source_package"), Source->GetOutermost()->GetName());
			TypeRow->SetNumberField(TEXT("source_state_hash_read_only"), Source->StateHash);
			TypeRow->SetNumberField(TEXT("variety_count"), Source->GrassVarieties.Num());
			TypeRow->SetArrayField(TEXT("output_bindings"), TypeRecord.Bindings);

			TArray<TSharedPtr<FJsonValue>> VarietyRows;
			for (int32 VarietyIndex = 0; VarietyIndex < Source->GrassVarieties.Num(); ++VarietyIndex)
			{
				const FGrassVariety& Variety = Source->GrassVarieties[VarietyIndex];
				TSharedRef<FJsonObject> VarietyRow = MakeShared<FJsonObject>();
				VarietyRow->SetNumberField(TEXT("variety_index"), VarietyIndex);
				UStaticMesh* Mesh = Variety.GrassMesh.Get();
				if (!IsValid(Mesh))
				{
					VarietyRow->SetStringField(TEXT("mesh_status"), TEXT("NOT_OBSERVED_MISSING_GRASS_MESH"));
					VarietyRow->SetStringField(TEXT("grass_mesh"), FString());
					VarietyRow->SetNumberField(TEXT("actual_mesh_slot_count"), 0);
					VarietyRow->SetStringField(TEXT("lod0_status"), TEXT("NOT_OBSERVED_NO_MESH"));
					Errors.Add(FString::Printf(TEXT("GrassType %s variety %d has no valid GrassMesh."), *TypePath, VarietyIndex));
					VarietyRows.Add(MakeShared<FJsonValueObject>(VarietyRow));
					continue;
				}

				TrackPackageDirtyState(Mesh, PackageDirtyBefore);
				const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
				VarietyRow->SetStringField(TEXT("mesh_status"), TEXT("OBSERVED"));
				VarietyRow->SetStringField(TEXT("grass_mesh"), Mesh->GetPathName());
				VarietyRow->SetNumberField(TEXT("actual_mesh_slot_count"), StaticMaterials.Num());
				TArray<TSharedPtr<FJsonValue>> SlotRows;
				for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
				{
					const FStaticMaterial& StaticMaterial = StaticMaterials[SlotIndex];
					UMaterialInterface* ProducerMaterial = Variety.OverrideMaterials.IsValidIndex(SlotIndex)
						? Variety.OverrideMaterials[SlotIndex].Get() : nullptr;
					if (!ProducerMaterial)
					{
						ProducerMaterial = Mesh->GetMaterial(SlotIndex);
					}
					TSharedRef<FJsonObject> SlotRow = MakeShared<FJsonObject>();
					SlotRow->SetNumberField(TEXT("slot_index"), SlotIndex);
					SlotRow->SetStringField(TEXT("mesh_material_slot_name"), StaticMaterial.MaterialSlotName.ToString());
					if (!IsValid(ProducerMaterial))
					{
						SlotRow->SetStringField(TEXT("producer_material_status"), TEXT("NOT_OBSERVED_NULL_SLOT_MATERIAL"));
						SlotRow->SetStringField(TEXT("producer_material"), FString());
						SlotRow->SetStringField(TEXT("producer_physical_material"), FString());
						SlotRow->SetStringField(TEXT("material_parent_chain_status"), TEXT("NOT_OBSERVED"));
						Errors.Add(FString::Printf(TEXT("GrassType %s variety %d mesh slot %d has no producer material."), *TypePath, VarietyIndex, SlotIndex));
						SlotRows.Add(MakeShared<FJsonValueObject>(SlotRow));
						continue;
					}

					TrackPackageDirtyState(ProducerMaterial, PackageDirtyBefore);
					const FString ProducerPath = ProducerMaterial->GetPathName();
					UniqueProducerMaterialPaths.Add(ProducerPath);
					SlotRow->SetStringField(TEXT("producer_material_status"), TEXT("OBSERVED"));
					SlotRow->SetStringField(TEXT("producer_material"), ProducerPath);
					UPhysicalMaterial* PhysicalMaterial = ProducerMaterial->GetPhysicalMaterial();
					TrackPackageDirtyState(PhysicalMaterial, PackageDirtyBefore);
					SlotRow->SetStringField(TEXT("producer_physical_material"), IsValid(PhysicalMaterial) ? PhysicalMaterial->GetPathName() : FString());
					SlotRow->SetStringField(TEXT("producer_physical_material_status"), IsValid(PhysicalMaterial) ? TEXT("OBSERVED") : TEXT("NOT_SET"));

					TArray<TSharedPtr<FJsonValue>> ParentChain;
					FString FinalBaseMaterialPath;
					FString ChainError;
					const bool bChainComplete = InspectMaterialParentChain(ProducerMaterial, ParentChain, FinalBaseMaterialPath, ChainError, PackageDirtyBefore);
					SlotRow->SetArrayField(TEXT("material_instance_parent_chain"), ParentChain);
					SlotRow->SetStringField(TEXT("material_parent_chain_status"), bChainComplete ? TEXT("OBSERVED") : TEXT("FAIL_CLOSED"));
					SlotRow->SetStringField(TEXT("final_base_material"), FinalBaseMaterialPath);
					if (!bChainComplete)
					{
						SlotRow->SetStringField(TEXT("material_parent_chain_error"), ChainError);
						Errors.Add(FString::Printf(TEXT("GrassType %s variety %d slot %d: %s"), *TypePath, VarietyIndex, SlotIndex, *ChainError));
					}
					SlotRows.Add(MakeShared<FJsonValueObject>(SlotRow));
				}
				VarietyRow->SetArrayField(TEXT("actual_mesh_slots"), SlotRows);

				const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
				TArray<TSharedPtr<FJsonValue>> LOD0Sections;
				if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
				{
					VarietyRow->SetStringField(TEXT("lod0_status"), TEXT("NOT_OBSERVED_RENDER_DATA_UNAVAILABLE"));
				}
				else if (RenderData->LODResources[0].Sections.IsEmpty())
				{
					VarietyRow->SetStringField(TEXT("lod0_status"), TEXT("NOT_OBSERVED_NO_SECTIONS"));
				}
				else
				{
					VarietyRow->SetStringField(TEXT("lod0_status"), TEXT("OBSERVED"));
					const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
					for (int32 SectionIndex = 0; SectionIndex < LOD0.Sections.Num(); ++SectionIndex)
					{
						const FStaticMeshSection& Section = LOD0.Sections[SectionIndex];
						TSharedRef<FJsonObject> SectionRow = MakeShared<FJsonObject>();
						SectionRow->SetNumberField(TEXT("section_index"), SectionIndex);
						SectionRow->SetNumberField(TEXT("material_index"), Section.MaterialIndex);
						SectionRow->SetNumberField(TEXT("triangle_count"), Section.NumTriangles);
						SectionRow->SetStringField(TEXT("material_slot_name"), StaticMaterials.IsValidIndex(Section.MaterialIndex)
							? StaticMaterials[Section.MaterialIndex].MaterialSlotName.ToString() : FString());
						SectionRow->SetStringField(TEXT("material_slot_resolution"), StaticMaterials.IsValidIndex(Section.MaterialIndex)
							? TEXT("OBSERVED") : TEXT("NOT_OBSERVED_OUT_OF_RANGE"));
						LOD0Sections.Add(MakeShared<FJsonValueObject>(SectionRow));
					}
				}
				VarietyRow->SetArrayField(TEXT("lod0_sections"), LOD0Sections);
				VarietyRows.Add(MakeShared<FJsonValueObject>(VarietyRow));
			}
			TypeRow->SetArrayField(TEXT("varieties"), VarietyRows);
			TypeRows.Add(MakeShared<FJsonValueObject>(TypeRow));
		}

		Summary->SetArrayField(TEXT("grass_types"), TypeRows);
		Summary->SetNumberField(TEXT("landscape_proxy_count"), UniqueProxyPaths.Num());
		Summary->SetNumberField(TEXT("landscape_component_count"), UniqueComponentPaths.Num());
		Summary->SetNumberField(TEXT("grass_type_count"), TypesByPath.Num());
		Summary->SetNumberField(TEXT("unique_base_material_count"), UniqueBaseMaterialPaths.Num());
		Summary->SetNumberField(TEXT("unique_source_material_count"), UniqueProducerMaterialPaths.Num());
		TArray<FString> BaseMaterialPaths = UniqueBaseMaterialPaths.Array();
		BaseMaterialPaths.Sort();
		TArray<TSharedPtr<FJsonValue>> BaseMaterialValues;
		for (const FString& Path : BaseMaterialPaths)
		{
			BaseMaterialValues.Add(MakeShared<FJsonValueString>(Path));
		}
		Summary->SetArrayField(TEXT("base_material_paths"), BaseMaterialValues);
		TArray<FString> ProducerMaterialPaths = UniqueProducerMaterialPaths.Array();
		ProducerMaterialPaths.Sort();
		TArray<TSharedPtr<FJsonValue>> ProducerMaterialValues;
		for (const FString& Path : ProducerMaterialPaths)
		{
			ProducerMaterialValues.Add(MakeShared<FJsonValueString>(Path));
		}
		Summary->SetArrayField(TEXT("unique_source_material_paths"), ProducerMaterialValues);

		TArray<UPackage*> Packages;
		PackageDirtyBefore.GetKeys(Packages);
		// TArray::Sort dereferences raw pointer elements before invoking its predicate.
		Packages.Sort([](const UPackage& A, const UPackage& B) { return A.GetName() < B.GetName(); });
		TArray<TSharedPtr<FJsonValue>> PackageRows;
		bool bDirtyStateUnchanged = true;
		for (UPackage* Package : Packages)
		{
			const bool bDirtyBefore = PackageDirtyBefore.FindChecked(Package);
			const bool bDirtyAfter = Package->IsDirty();
			bDirtyStateUnchanged &= bDirtyBefore == bDirtyAfter;
			TSharedRef<FJsonObject> PackageRow = MakeShared<FJsonObject>();
			PackageRow->SetStringField(TEXT("package"), Package->GetName());
			PackageRow->SetBoolField(TEXT("dirty_before"), bDirtyBefore);
			PackageRow->SetBoolField(TEXT("dirty_after"), bDirtyAfter);
			PackageRows.Add(MakeShared<FJsonValueObject>(PackageRow));
		}
		Summary->SetArrayField(TEXT("observed_package_dirty_states"), PackageRows);
		Summary->SetBoolField(TEXT("dirty_state_check_complete"), true);
		Summary->SetBoolField(TEXT("observed_package_dirty_state_unchanged"), bDirtyStateUnchanged);
		if (!bDirtyStateUnchanged)
		{
			Errors.Add(TEXT("A package dirtiness flag changed during the read-only source inventory."));
		}

		if (!Errors.IsEmpty())
		{
			OutError = Errors[0];
			Summary->SetStringField(TEXT("status"), TEXT("FAIL_CLOSED"));
			Summary->SetStringField(TEXT("error"), OutError);
			TArray<TSharedPtr<FJsonValue>> ErrorRows;
			for (const FString& Error : Errors)
			{
				ErrorRows.Add(MakeShared<FJsonValueString>(Error));
			}
			Summary->SetArrayField(TEXT("errors"), ErrorRows);
			return false;
		}

		Summary->SetStringField(TEXT("status"), TEXT("PASS_READ_ONLY_SOURCE_INVENTORY"));
		Summary->SetStringField(TEXT("scope"), TEXT("Base-material cached NamedGrassTypes and source GrassType variety/material data; no redirects or writes were applied."));
		return true;
	}
}

bool IMInspectLandscapeGrassMaterialSources(UWorld* World, TSharedPtr<FJsonObject>& OutSummary, FString& OutError)
{
	return IMAcousticLandscapeGrassMaterialsPrivate::InspectLandscapeGrassMaterialSources(World, OutSummary, OutError);
}

#else

bool IMPrepareLandscapeGrassMaterialCopies(
	UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
	TSharedPtr<FJsonObject>& OutSummary,
	FString& OutError)
{
	(void)World;
	(void)SourceToPluginMIC;
	OutSummary.Reset();
	OutError = TEXT("Landscape Grass material copies require WITH_EDITOR and WITH_DEV_AUTOMATION_TESTS.");
	return false;
}

bool IMValidateLandscapeGrassMaterialCopies(
	UWorld* World,
	const TMap<UMaterialInterface*, UMaterialInterface*>& SourceToPluginMIC,
	TSharedPtr<FJsonObject>& OutSummary,
	FString& OutError)
{
	(void)World;
	(void)SourceToPluginMIC;
	OutSummary.Reset();
	OutError = TEXT("Landscape Grass material validation requires WITH_EDITOR and WITH_DEV_AUTOMATION_TESTS.");
	return false;
}

bool IMInspectLandscapeGrassMaterialSources(UWorld* World, TSharedPtr<FJsonObject>& OutSummary, FString& OutError)
{
	(void)World;
	OutSummary.Reset();
	OutError = TEXT("Landscape Grass source inventory requires WITH_EDITOR and WITH_DEV_AUTOMATION_TESTS.");
	return false;
}

#endif
