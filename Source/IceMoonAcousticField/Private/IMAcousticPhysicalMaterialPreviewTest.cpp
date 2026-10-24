#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "IMAcousticBakeAsset.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticLandscapeCells.h"
#include "IMAcousticLandscapeFinalSnapshot.h"
#include "IMAcousticLandscapeGrassMaterials.h"
#include "IMAcousticRuralConstructionMaterials.h"
#include "Engine/Brush.h"
#include "GameFramework/PhysicsVolume.h"
#include "IMAcousticSourceComponent.h"

#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShowFlags.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealClient.h"
#include "ViewportClient.h"

namespace IMAcousticPhysicalMaterialPreviewTestPrivate
{
constexpr const TCHAR* RuralMap = TEXT("/IceMoonAcousticField/Tests/IM_RuralCamera3Preview_10m");
constexpr const TCHAR* RuralSourceMap = TEXT("/Game/RuralAustralia/Maps/RuralAustralia_Example_01");
constexpr const TCHAR* SourceTag = TEXT("IMAcousticRuralPreviewWaterDropV1");
constexpr const TCHAR* PlayerStartTag = TEXT("IMAcousticRuralPreviewPlayerStartV1");
constexpr const TCHAR* MutedBackgroundTag = TEXT("IMAcousticRuralPreviewMutedOriginalAmbient");
constexpr const TCHAR* IgnoreTag = TEXT("IMAcousticIgnore");
constexpr const TCHAR* BlockingTag = TEXT("IMAcousticRuralPreviewProvisionalBlockingVolume");
constexpr const TCHAR* WaterDropMetaSound = TEXT("/IceMoonAcousticField/Tests/Audio/MS_WaterDropEryliaa_FullLoopOnPlay");
constexpr const TCHAR* PhysicalMaterialRoot = TEXT("/IceMoonAcousticField/Tests/PhysicalMaterials");
// This is the historical local-region compatibility table, not the number of
// materials used by the whole Rural level. PhysicalMaterial mode does not solve
// acoustics through this render-material table.
constexpr int32 LegacyPreviewMaterialCount = 48;
constexpr const TCHAR* BakeAssetBefore =
	TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_83F5B0E44B9D02F8C2D01086FCA2A2BC.IM_IM_RuralCamera3Preview_10m_83F5B0E44B9D02F8C2D01086FCA2A2BC");
// Only this recorded partial candidate may bypass the validated-baseline gate.
// It passed full fence geometry and saved a bake, then failed regenerated Grass
// PM slots in run 5FB59178. Never generalize this exception to unknown assets.
constexpr const TCHAR* RejectedGrassBake =
	TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_949273DD4C1DE7F3FF2435A63DC498EF.IM_IM_RuralCamera3Preview_10m_949273DD4C1DE7F3FF2435A63DC498EF");
// Complete PM candidate saved before renderer-only Grass was added to capture.
// Its old payload remains valid; the new source geometry intentionally makes it stale.
constexpr const TCHAR* RecordedCPUOnlyGrassBake =
	TEXT("/IceMoonAcousticField/Bakes/IM_IM_RuralCamera3Preview_10m_59E06EE74320057D491302913A9CA7EC.IM_IM_RuralCamera3Preview_10m_59E06EE74320057D491302913A9CA7EC");
constexpr const TCHAR* RecordedCPUOnlyFingerprint = TEXT("4E9EC53A546E4ECFABFB8DD53FC063AADC221EA1");
constexpr const TCHAR* RecordedCPUOnlyPackageSHA1 = TEXT("F178E8D02083A53CD10017DCE885CC317445C218");
constexpr int64 RecordedCPUOnlyPackageBytes = 2137120;
constexpr const TCHAR* PristineTerrainMap =
	TEXT("/IceMoonAcousticField/Tests/IM_RuralLandscapePristine_D204C3D74C496F05F26278AA489A798D");
// This exact saved fixture was independently compared with all 16 registered
// source components: complete final textures/mips/collision heights matched.
constexpr const TCHAR* PristineTerrainSha1 = TEXT("B47D109F1F0CD04A07605F8806ABC40357D19674");
constexpr int64 PristineTerrainBytes = 30374499;
constexpr const TCHAR* ClassificationNotes =
	TEXT("Root inspected all 33 source Grass meshes from their saved package thumbnails (GrassSourceThumbnails, R9B) and joined exact producer materials and LOD0 section indices from inventory A05D0D9B. Scatter01 thin leaf/stem cards and Scatter03 leaf litter use Leaf/non-solid; Scatter02 twig/bark debris retain conservative Wood/Solid, including their mixed thin fragments rather than guessing a per-triangle split. Scatter04 stones use Stone/Solid; Scatter05 grass and its unused billboard slot use Grass/non-solid. GrassTree leaves use Grass/non-solid while actual LOD0 Base sections and logs retain Wood/Solid; mixed tree/GrassTree impostors remain Wood/Solid and are not substitutes for the LOD0 foliage sections. The static snake uses adjustable HardProp as an explicit nonvegetation preset, not a measured flesh model. Fence posts use Wood; wires, mixed signs/reflectors/delineators and the auxiliary cube use adjustable HardProp. Road and its line overlay use adjustable HardGround; terrain and dirt ridges use adjustable Soil. These are initial game presets, not measured material properties. Non-solid micro scattering is not simulated. Movable mesh PM bindings do not change static bake eligibility or register dynamic blockers. Exact tagged Sky/Horizon backdrops retain their existing exclusions. Runtime evidence records exact slots, mobility and LOD0 sections.");
constexpr double MaxSeconds = 900.0;

enum class EIMSurfaceProfile : uint8
{
	Wood,
	Leaf,
	Grass,
	Stone,
	Soil,
	HardProp,
	HardGround
};

struct FIMProfileSpec
{
	EIMSurfaceProfile Profile;
	const TCHAR* Name;
	FVector Absorption;
	float Scattering;
	bool bSolidGeometry;
	bool bSupportsProbes;
	const TCHAR* EvidenceLabel;
};

struct FIMMaterialRule
{
	const TCHAR* SourcePath;
	const TCHAR* CopyName;
	EIMSurfaceProfile Profile;
};

static const FIMProfileSpec ProfileSpecs[] =
{
	{ EIMSurfaceProfile::Wood,  TEXT("Wood"),  FVector(0.11, 0.07, 0.06), 0.05f, true,  false, TEXT("initial_profile") },
	{ EIMSurfaceProfile::Leaf,  TEXT("Leaf"),  FVector::ZeroVector,        0.00f, false, false, TEXT("non_solid_coefficients_unused") },
	{ EIMSurfaceProfile::Grass, TEXT("Grass"), FVector::ZeroVector,        0.00f, false, false, TEXT("non_solid_coefficients_unused") },
	{ EIMSurfaceProfile::Stone, TEXT("Stone"), FVector(0.13, 0.20, 0.24), 0.05f, true,  true,  TEXT("initial_profile") },
	{ EIMSurfaceProfile::Soil,  TEXT("Soil"),  FVector(0.35, 0.55, 0.70), 0.25f, true,  true,  TEXT("adjustable_initial_not_measured") },
	{ EIMSurfaceProfile::HardProp, TEXT("HardProp"), FVector(0.05, 0.07, 0.08), 0.05f, true, false, TEXT("adjustable_mixed_hard_prop_initial_not_measured") },
	{ EIMSurfaceProfile::HardGround, TEXT("HardGround"), FVector(0.05, 0.07, 0.08), 0.05f, true, true, TEXT("adjustable_hard_ground_initial_not_measured") }
};

// The first 48 paths retain the historical local-preview path ordering.
// Additional paths must come from a complete, explicit full-scene slot inventory.
// The category is an explicit per-asset decision; no wildcard classification is used.
// Billboard cards may contain trunk pixels, so they stay solid until visual evidence proves foliage-only.
// Scatter decisions use the saved source-mesh image/producer/section evidence,
// not a wildcard derived from mesh or material names.
static const FIMMaterialRule MaterialRules[] =
{
	{ TEXT("/Game/RuralAustralia/Landscape/MI_Landscape_Example_01.MI_Landscape_Example_01"), TEXT("PMI_Landscape_Example_01"), EIMSurfaceProfile::Soil },
	{ TEXT("/Game/RuralAustralia/Scatter/M_Scatter_02_LOD0.M_Scatter_02_LOD0"), TEXT("PMI_M_Scatter_02_LOD0"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_01.MI_Scatter_01"), TEXT("PMI_Scatter_01"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_02.MI_Scatter_02"), TEXT("PMI_Scatter_02"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Rocks/Rock_M_01/MI_Rock_M_01.MI_Rock_M_01"), TEXT("PMI_Rock_M_01"), EIMSurfaceProfile::Stone },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Rocks/Rock_M_02/MI_Rock_M_02.MI_Rock_M_02"), TEXT("PMI_Rock_M_02"), EIMSurfaceProfile::Stone },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Rocks/Rock_M_03/MI_Rock_M_03.MI_Rock_M_03"), TEXT("PMI_Rock_M_03"), EIMSurfaceProfile::Stone },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Rocks/Rock_S_01/MI_Rock_S_01.MI_Rock_S_01"), TEXT("PMI_Rock_S_01"), EIMSurfaceProfile::Stone },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Rocks/Rock_S_02/MI_Rock_S_02.MI_Rock_S_02"), TEXT("PMI_Rock_S_02"), EIMSurfaceProfile::Stone },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_01.MI_GrassTree_01"), TEXT("PMI_GrassTree_01"), EIMSurfaceProfile::Grass },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_01_Base.MI_GrassTree_01_Base"), TEXT("PMI_GrassTree_01_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_01_Billboard.MI_GrassTree_01_Billboard"), TEXT("PMI_GrassTree_01_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_02.MI_GrassTree_02"), TEXT("PMI_GrassTree_02"), EIMSurfaceProfile::Grass },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_02_Base.MI_GrassTree_02_Base"), TEXT("PMI_GrassTree_02_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_02_Billboard.MI_GrassTree_02_Billboard"), TEXT("PMI_GrassTree_02_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_03.MI_GrassTree_03"), TEXT("PMI_GrassTree_03"), EIMSurfaceProfile::Grass },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_03_Base.MI_GrassTree_03_Base"), TEXT("PMI_GrassTree_03_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_03_Billboard.MI_GrassTree_03_Billboard"), TEXT("PMI_GrassTree_03_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_04.MI_GrassTree_04"), TEXT("PMI_GrassTree_04"), EIMSurfaceProfile::Grass },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/GrassTree_01/MI_GrassTree_04_Billboard.MI_GrassTree_04_Billboard"), TEXT("PMI_GrassTree_04_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Log_L_01/MI_Log_L_01.MI_Log_L_01"), TEXT("PMI_Log_L_01"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Log_M_01/MI_Log_M_01.MI_Log_M_01"), TEXT("PMI_Log_M_01"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Log_S_01/MI_Log_S_01.MI_Log_S_01"), TEXT("PMI_Log_S_01"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Log_S_02/MI_Log_S_02.MI_Log_S_02"), TEXT("PMI_Log_S_02"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_L_01/MI_Tree_L_01_Base.MI_Tree_L_01_Base"), TEXT("PMI_Tree_L_01_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_L_01/MI_Tree_L_01_Billboard.MI_Tree_L_01_Billboard"), TEXT("PMI_Tree_L_01_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_L_01/MI_Tree_L_01_Leaves.MI_Tree_L_01_Leaves"), TEXT("PMI_Tree_L_01_Leaves"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_L_01/MI_Tree_L_01_Tile.MI_Tree_L_01_Tile"), TEXT("PMI_Tree_L_01_Tile"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_01/MI_Tree_M_01_Base.MI_Tree_M_01_Base"), TEXT("PMI_Tree_M_01_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_01/MI_Tree_M_01_Billboard.MI_Tree_M_01_Billboard"), TEXT("PMI_Tree_M_01_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_01/MI_Tree_M_01_Leaves.MI_Tree_M_01_Leaves"), TEXT("PMI_Tree_M_01_Leaves"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_01/MI_Tree_M_01_Tile.MI_Tree_M_01_Tile"), TEXT("PMI_Tree_M_01_Tile"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_02/MI_Tree_M_02_Base.MI_Tree_M_02_Base"), TEXT("PMI_Tree_M_02_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_02/MI_Tree_M_02_Billboard.MI_Tree_M_02_Billboard"), TEXT("PMI_Tree_M_02_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_02/MI_Tree_M_02_Leaves.MI_Tree_M_02_Leaves"), TEXT("PMI_Tree_M_02_Leaves"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_02/MI_Tree_M_02_Tile.MI_Tree_M_02_Tile"), TEXT("PMI_Tree_M_02_Tile"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_03/MI_Tree_M_03_Base.MI_Tree_M_03_Base"), TEXT("PMI_Tree_M_03_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_03/MI_Tree_M_03_Billboard.MI_Tree_M_03_Billboard"), TEXT("PMI_Tree_M_03_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_03/MI_Tree_M_03_Leaves.MI_Tree_M_03_Leaves"), TEXT("PMI_Tree_M_03_Leaves"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_03/MI_Tree_M_03_Tile.MI_Tree_M_03_Tile"), TEXT("PMI_Tree_M_03_Tile"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_04/MI_Tree_M_04_Base.MI_Tree_M_04_Base"), TEXT("PMI_Tree_M_04_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_04/MI_Tree_M_04_Billboard.MI_Tree_M_04_Billboard"), TEXT("PMI_Tree_M_04_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_04/MI_Tree_M_04_Leaves.MI_Tree_M_04_Leaves"), TEXT("PMI_Tree_M_04_Leaves"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_M_04/MI_Tree_M_04_Tile.MI_Tree_M_04_Tile"), TEXT("PMI_Tree_M_04_Tile"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_S_01/MI_Tree_S_01_Base.MI_Tree_S_01_Base"), TEXT("PMI_Tree_S_01_Base"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_S_01/MI_Tree_S_01_Billboard.MI_Tree_S_01_Billboard"), TEXT("PMI_Tree_S_01_Billboard"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_S_01/MI_Tree_S_01_Leaves.MI_Tree_S_01_Leaves"), TEXT("PMI_Tree_S_01_Leaves"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Vegetation/Tree_S_01/MI_Tree_S_01_Tile.MI_Tree_S_01_Tile"), TEXT("PMI_Tree_S_01_Tile"), EIMSurfaceProfile::Wood },
	// Full-scene source-geometry-identity.txt adds these ten model paths.
	// Sky/Horizon are already exact tagged background exclusions. The auxiliary
	// cube rule is a superset entry because its existing exclusion tags were not
	// recorded by that historical inventory; actual slots remain authoritative.
	{ TEXT("/Game/Level/C_Funection/ShadeupExamplePlugin/IMGridSimulationCases/IMSceneViewExtensionBuffer/M_ViewSceneExtension.M_ViewSceneExtension"), TEXT("PMI_ViewSceneExtension"), EIMSurfaceProfile::HardProp },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Delineator_01/M_Delineator_01.M_Delineator_01"), TEXT("PMI_Delineator_01"), EIMSurfaceProfile::HardProp },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/MI_Fence_01.MI_Fence_01"), TEXT("PMI_Fence_01"), EIMSurfaceProfile::Wood },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/M_Fence_Wires_01.M_Fence_Wires_01"), TEXT("PMI_Fence_Wires_01"), EIMSurfaceProfile::HardProp },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/M_Fence_Wires_01_LOD.M_Fence_Wires_01_LOD"), TEXT("PMI_Fence_Wires_01_LOD"), EIMSurfaceProfile::HardProp },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/KangarooSign_01/M_KangarooSign_01.M_KangarooSign_01"), TEXT("PMI_KangarooSign_01"), EIMSurfaceProfile::HardProp },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Reflector_01/M_Reflector_01.M_Reflector_01"), TEXT("PMI_Reflector_01"), EIMSurfaceProfile::HardProp },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Road_01/M_RoadLine_Decal_01.M_RoadLine_Decal_01"), TEXT("PMI_RoadLine_Decal_01"), EIMSurfaceProfile::HardGround },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Road_01/M_Road_01.M_Road_01"), TEXT("PMI_Road_01"), EIMSurfaceProfile::HardGround },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Rocks/Ridge_Dirt_01/MI_Ridge_Dirt_01.MI_Ridge_Dirt_01"), TEXT("PMI_Ridge_Dirt_01"), EIMSurfaceProfile::Soil },
	// A05D0D9B full GrassType producer inventory identifies these eleven
	// additional exact paths; R9B images establish the source mesh roles.
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_01_B.MI_Scatter_01_B"), TEXT("PMI_Scatter_01_B"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_01_C.MI_Scatter_01_C"), TEXT("PMI_Scatter_01_C"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_01_J.MI_Scatter_01_J"), TEXT("PMI_Scatter_01_J"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_01_K.MI_Scatter_01_K"), TEXT("PMI_Scatter_01_K"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_01_L.MI_Scatter_01_L"), TEXT("PMI_Scatter_01_L"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_03.MI_Scatter_03"), TEXT("PMI_Scatter_03"), EIMSurfaceProfile::Leaf },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_04.MI_Scatter_04"), TEXT("PMI_Scatter_04"), EIMSurfaceProfile::Stone },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_04_B.MI_Scatter_04_B"), TEXT("PMI_Scatter_04_B"), EIMSurfaceProfile::Stone },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_05.MI_Scatter_05"), TEXT("PMI_Scatter_05"), EIMSurfaceProfile::Grass },
	{ TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_05_Billboard.MI_Scatter_05_Billboard"), TEXT("PMI_Scatter_05_Billboard"), EIMSurfaceProfile::Grass },
	{ TEXT("/Game/RuralAustralia/StaticMeshes/Props/Snake_01/M_Snake_01.M_Snake_01"), TEXT("PMI_Snake_01"), EIMSurfaceProfile::HardProp }
};

static const FIMProfileSpec* ProfileFor(EIMSurfaceProfile Profile)
{
	for (const FIMProfileSpec& Spec : ProfileSpecs)
		if (Spec.Profile == Profile) return &Spec;
	return nullptr;
}

static FString ProfileObjectPath(EIMSurfaceProfile Profile)
{
	const FIMProfileSpec* Spec = ProfileFor(Profile);
	return Spec
		? FString::Printf(TEXT("%s/PM_Acoustic_%s.PM_Acoustic_%s"), PhysicalMaterialRoot, Spec->Name, Spec->Name)
		: FString();
}

static FString RuleCopyPackage(const FIMMaterialRule& Rule)
{
	return FString::Printf(TEXT("%s/%s"), PhysicalMaterialRoot, Rule.CopyName);
}

static FString RuleCopyObjectPath(const FIMMaterialRule& Rule)
{
	return FString::Printf(TEXT("%s.%s"), *RuleCopyPackage(Rule), Rule.CopyName);
}

static FString ProfileName(EIMSurfaceProfile Profile)
{
	const FIMProfileSpec* Spec = ProfileFor(Profile);
	return Spec ? FString(Spec->Name) : TEXT("Unknown");
}

static const FIMMaterialRule* FindRule(const FString& MaterialPath)
{
	for (const FIMMaterialRule& Rule : MaterialRules)
	{
		if (MaterialPath == Rule.SourcePath || MaterialPath == RuleCopyObjectPath(Rule))
			return &Rule;
	}
	return nullptr;
}

static FString SafeAssetPart(const FString& Input)
{
	FString Result;
	for (TCHAR Character : Input)
		Result.AppendChar(FChar::IsAlnum(Character) || Character == TEXT('_') ? Character : TEXT('_'));
	return Result;
}

static bool HashFile(const FString& Filename, FString& OutHash, int64& OutBytes, FString& Error)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
	{
		Error = TEXT("Cannot read source package for immutability hash: ") + Filename;
		return false;
	}
	uint8 Digest[20];
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
	OutHash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	OutBytes = Bytes.Num();
	return true;
}

static bool SaveAsset(UObject* Asset, FString& Error)
{
	if (!IsValid(Asset) || Asset->HasAnyFlags(RF_Transient))
	{
		Error = TEXT("Asset save received an invalid or transient object.");
		return false;
	}
	UPackage* Package = Asset->GetOutermost();
	const FString ExpectedFile = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExpectedFile), true);
	Package->SetDirtyFlag(true);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Asset, *ExpectedFile, Args))
	{
		Error = TEXT("Asset package save failed: ") + Package->GetName();
		return false;
	}
	FString ActualFile;
	if (!FPackageName::DoesPackageExist(Package->GetName(), &ActualFile))
	{
		Error = TEXT("Saved asset did not resolve to its exact plugin package file: ") + Package->GetName();
		return false;
	}
	FPaths::NormalizeFilename(ActualFile);
	FString NormalizedExpectedFile = ExpectedFile;
	FPaths::NormalizeFilename(NormalizedExpectedFile);
	if (!ActualFile.Equals(NormalizedExpectedFile, ESearchCase::IgnoreCase))
	{
		Error = TEXT("Saved asset did not resolve to its exact plugin package file: ") + Package->GetName();
		return false;
	}
	return true;
}

static UPhysicalMaterial* LoadExistingProfilePM(EIMSurfaceProfile Profile)
{
	const FString Path = ProfileObjectPath(Profile);
	return Path.IsEmpty() ? nullptr : LoadObject<UPhysicalMaterial>(nullptr, *Path);
}

static bool IsKnownScatterLeafReclassification(const FIMMaterialRule& Rule,
	UMaterialInterface* Source, UMaterialInstanceConstant* Copy, UPhysicalMaterial* DesiredPM)
{
	// Root's actual-image decision replaces only the prior task-authored Wood
	// assignment of this exact copy. The R10 failure receipt pins its bytes;
	// another parent/profile, dirty WIP, or on-disk drift is never authorized.
	if (FString(Rule.SourcePath) != TEXT("/Game/RuralAustralia/Scatter/MI_Scatter_01.MI_Scatter_01")
		|| FString(Rule.CopyName) != TEXT("PMI_Scatter_01") || Rule.Profile != EIMSurfaceProfile::Leaf
		|| !Source || Source->GetPathName() != Rule.SourcePath || !Copy || !DesiredPM
		|| Copy->GetPathName() != RuleCopyObjectPath(Rule)
		|| Copy->GetOutermost()->IsDirty()) return false;
	UMaterialInstanceConstant* SourceInstance = Cast<UMaterialInstanceConstant>(Source);
	UMaterialInterface* ExpectedParent = SourceInstance ? SourceInstance->Parent.Get() : Source;
	UPhysicalMaterial* PriorWood = LoadExistingProfilePM(EIMSurfaceProfile::Wood);
	if (!ExpectedParent || !PriorWood || DesiredPM != LoadExistingProfilePM(EIMSurfaceProfile::Leaf)
		|| Copy->Parent.Get() != ExpectedParent || !Copy->bOverridePhysMaterial
		|| Copy->PhysMaterial.Get() != PriorWood || Copy->GetPhysicalMaterial() != PriorWood) return false;
	FString Filename, ActualSHA1, ReadError;
	int64 ActualBytes = -1;
	return FPackageName::DoesPackageExist(RuleCopyPackage(Rule), &Filename)
		&& HashFile(Filename, ActualSHA1, ActualBytes, ReadError)
		&& ActualBytes == 10175
		&& ActualSHA1 == TEXT("2924FD995746238AE5632065D16B5BA2C54DFA75");
}

static UPhysicalMaterial* CreateOrLoadProfilePM(EIMSurfaceProfile Profile, FString& Error)
{
	const FIMProfileSpec* Spec = ProfileFor(Profile);
	if (!Spec)
	{
		Error = TEXT("Unknown profile requested for PhysicalMaterial creation.");
		return nullptr;
	}
	const FString ObjectPath = ProfileObjectPath(Profile);
	if (UPhysicalMaterial* Existing = LoadObject<UPhysicalMaterial>(nullptr, *ObjectPath))
		return Existing;

	const FString PackagePath = FString::Printf(TEXT("%s/PM_Acoustic_%s"), PhysicalMaterialRoot, Spec->Name);
	if (FPackageName::DoesPackageExist(PackagePath))
	{
		Error = TEXT("Profile package exists with an unexpected asset class: ") + PackagePath;
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		Error = TEXT("Could not create profile package: ") + PackagePath;
		return nullptr;
	}
	UPhysicalMaterial* PhysicalMaterial = NewObject<UPhysicalMaterial>(
		Package, FName(*FString::Printf(TEXT("PM_Acoustic_%s"), Spec->Name)), RF_Public | RF_Standalone);
	if (!PhysicalMaterial)
	{
		Error = TEXT("Could not create profile PhysicalMaterial: ") + ObjectPath;
		return nullptr;
	}
	PhysicalMaterial->PostEditChange();
	if (!SaveAsset(PhysicalMaterial, Error)) return nullptr;
	return PhysicalMaterial;
}

static UMaterialInstanceConstant* LoadOrCreateMaterialCopy(
	const FIMMaterialRule& Rule, UMaterialInterface* Source, UPhysicalMaterial* PhysicalMaterial, FString& Error)
{
	const FString ObjectPath = RuleCopyObjectPath(Rule);
	if (UMaterialInstanceConstant* Existing = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath))
	{
		const FIMProfileSpec* Spec = ProfileFor(Rule.Profile);
		UMaterialInstanceConstant* SourceInstance = Cast<UMaterialInstanceConstant>(Source);
		UMaterialInterface* ExpectedParent = SourceInstance ? SourceInstance->Parent.Get() : Source;
		if (IsKnownScatterLeafReclassification(Rule, Source, Existing, PhysicalMaterial))
		{
			Existing->Modify();
			Existing->PhysMaterial = PhysicalMaterial;
			Existing->bOverridePhysMaterial = true;
			Existing->PostEditChange();
			if (!SaveAsset(Existing, Error)) return nullptr;
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticKnownProfileTransition copy=%s from=Wood to=Leaf pinned_prior_sha1=2924FD995746238AE5632065D16B5BA2C54DFA75"), *ObjectPath);
		}
		if (!Spec || Existing->Parent.Get() != ExpectedParent
			|| !Existing->bOverridePhysMaterial || Existing->PhysMaterial.Get() != PhysicalMaterial
			|| Existing->GetPhysicalMaterial() != PhysicalMaterial)
		{
			Error = TEXT("Existing plugin MI copy does not match its exact source parent and assigned profile: ") + ObjectPath;
			return nullptr;
		}
		return Existing;
	}
	const FString PackagePath = RuleCopyPackage(Rule);
	if (FPackageName::DoesPackageExist(PackagePath))
	{
		Error = TEXT("MI copy package exists with an unexpected asset class: ") + PackagePath;
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		Error = TEXT("Could not create MI copy package: ") + PackagePath;
		return nullptr;
	}

	UMaterialInstanceConstant* Copy = nullptr;
	if (UMaterialInstanceConstant* SourceInstance = Cast<UMaterialInstanceConstant>(Source))
	{
		Copy = Cast<UMaterialInstanceConstant>(
			StaticDuplicateObject(SourceInstance, Package, FName(Rule.CopyName)));
	}
	else if (Source->IsA<UMaterial>())
	{
		Copy = NewObject<UMaterialInstanceConstant>(Package, FName(Rule.CopyName), RF_Public | RF_Standalone);
		if (Copy) Copy->SetParentEditorOnly(Source, false);
	}
	if (!Copy)
	{
		Error = TEXT("Source was neither a duplicable MIC nor a UMaterial master: ") + Source->GetPathName();
		return nullptr;
	}
	Copy->SetFlags(RF_Public | RF_Standalone);
	Copy->Modify();
	Copy->PhysMaterial = PhysicalMaterial;
	Copy->bOverridePhysMaterial = true;
	Copy->PostEditChange();
	if (!SaveAsset(Copy, Error)) return nullptr;
	if (Copy->GetPathName() != ObjectPath || Copy->GetPhysicalMaterial() != PhysicalMaterial)
	{
		Error = TEXT("New MI copy failed post-save identity or physical-material verification: ") + ObjectPath;
		return nullptr;
	}
	return Copy;
}

static void AddVector(TSharedRef<FJsonObject> Json, const TCHAR* Name, const FVector& Value)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Add(MakeShared<FJsonValueNumber>(Value.X));
	Values.Add(MakeShared<FJsonValueNumber>(Value.Y));
	Values.Add(MakeShared<FJsonValueNumber>(Value.Z));
	Json->SetArrayField(Name, Values);
}

static void AddPathArray(TSharedRef<FJsonObject> Json, const TCHAR* Name, const TArray<FString>& Paths)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Paths.Num());
	for (const FString& Path : Paths) Values.Add(MakeShared<FJsonValueString>(Path));
	Json->SetArrayField(Name, Values);
}

struct FIMFixedSceneState
{
	FVector BoundsCenter = FVector::ZeroVector;
	FVector BoundsExtent = FVector::ZeroVector;
	FVector CameraLocation = FVector::ZeroVector;
	FVector PlayerLocation = FVector::ZeroVector;
	FVector SourceLocation = FVector::ZeroVector;
	FString BakeAssetPath;
	FString WaterDropPath;
	FString AmbientPath;
	float ProbeSpacingCm = 0.0f;
	float ProbeHeightCm = 0.0f;
	float ReverbWetGain = 0.0f;
	float SourceGain = 0.0f;
	float SourcePitch = 0.0f;
	int32 AudioCount = 0;
	int32 MarkerCount = 0;
	bool bV2 = false;
	bool bDirect = false;
	bool bPath = false;
	bool bReverb = false;
};

static TSharedRef<FJsonObject> FixedStateJson(const FIMFixedSceneState& State)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	AddVector(Json, TEXT("bounds_center_cm"), State.BoundsCenter);
	AddVector(Json, TEXT("bounds_extent_cm"), State.BoundsExtent);
	AddVector(Json, TEXT("camera_location_cm"), State.CameraLocation);
	AddVector(Json, TEXT("player_start_location_cm"), State.PlayerLocation);
	AddVector(Json, TEXT("water_drop_source_location_cm"), State.SourceLocation);
	Json->SetStringField(TEXT("bake_asset_path"), State.BakeAssetPath);
	Json->SetStringField(TEXT("water_drop_metasound"), State.WaterDropPath);
	Json->SetStringField(TEXT("ambient_audio_component"), State.AmbientPath);
	Json->SetNumberField(TEXT("probe_spacing_cm"), State.ProbeSpacingCm);
	Json->SetNumberField(TEXT("probe_height_cm"), State.ProbeHeightCm);
	Json->SetNumberField(TEXT("reverb_wet_gain"), State.ReverbWetGain);
	Json->SetNumberField(TEXT("water_drop_gain"), State.SourceGain);
	Json->SetNumberField(TEXT("water_drop_pitch"), State.SourcePitch);
	Json->SetNumberField(TEXT("audio_component_count"), State.AudioCount);
	Json->SetNumberField(TEXT("source_marker_count"), State.MarkerCount);
	Json->SetBoolField(TEXT("v2_enabled"), State.bV2);
	Json->SetBoolField(TEXT("direct_route"), State.bDirect);
	Json->SetBoolField(TEXT("path_route"), State.bPath);
	Json->SetBoolField(TEXT("reverb_route"), State.bReverb);
	return Json;
}

class FIMPhysicalMaterialPreviewPrepareCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMPhysicalMaterialPreviewPrepareCommand(FAutomationTestBase* InTest, bool bInTerrainOnly = false)
		: Test(InTest), StartedAt(FPlatformTime::Seconds()), bTerrainOnly(bInTerrainOnly)
	{
		EvidenceDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialDrive"));
		EvidenceFile = FPaths::Combine(EvidenceDirectory,
			TEXT("authoring_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"));
		SampleMemory();
	}

	bool Update() override
	{
		SampleMemory();
		if (bFinished) return true;
		if (!GEditor || GEditor->PlayWorld)
			return Finish(false, TEXT("PhysicalMaterial preview requires an idle owned Editor without PIE."));
		if (FPlatformTime::Seconds() - StartedAt > MaxSeconds && Stage != 2)
			return Finish(false, TEXT("PhysicalMaterial preview exceeded the bounded 900 second authoring guard."));
		if (LastLoggedStage != Stage)
		{
			LogRenderContext(TEXT("stage_enter"));
			LastLoggedStage = Stage;
		}
		if (Stage == 0)
		{
			if (!bInitialMapLoaded)
			{
				FString Error;
				const double LoadStart = FPlatformTime::Seconds();
				if (!LoadRuralMap(Error)) return Finish(false, Error);
				LogRenderContext(TEXT("initial_map_loaded"));
				InitialLoadMs = (FPlatformTime::Seconds() - LoadStart) * 1000.0;
				InitialLoadFrame = GFrameCounter;
				InitialReadyStartedAt = FPlatformTime::Seconds();
				bInitialMapLoaded = true;
				return false;
			}
			if (GFrameCounter <= InitialLoadFrame) return false;
			InitialPostLoadFrameDelta = GFrameCounter - InitialLoadFrame;
			const double Start = FPlatformTime::Seconds();
			const bool Done = AuthorAndSave();
			AuthorSaveMs += (FPlatformTime::Seconds() - Start) * 1000.0;
			SampleMemory();
			return Done;
		}
		if (Stage == 1) return ReloadAndStartBake();
		if (Stage == 2) return PollBake();
		if (Stage == 3) return ReloadAndValidate();
		if (Stage == 4) return ObserveEditorValidation();
		if (Stage == 10 || Stage == 11) return RestoreTerrainAndSave();
		if (Stage == 12) return ReloadTerrainOnly();
		return Finish(false, TEXT("Unexpected PhysicalMaterial preview stage."));
	}

private:
	void LogRenderContext(const TCHAR* Phase)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		FViewport* Viewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
		FViewportClient* Client = Viewport ? Viewport->GetClient() : nullptr;
		FEngineShowFlags* Flags = Client ? Client->GetEngineShowFlags() : nullptr;
		const FIntPoint Size = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
		auto ReadCVar = [](const TCHAR* Name)
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			return Variable ? Variable->GetInt() : -1;
		};
		UE_LOG(LogTemp, Display, TEXT("[IM][PM_RENDER_CONTEXT] phase=%s stage=%d frame=%llu time=%.9f world=%s world_id=%u view_world=%s viewport=%dx%d rendering=%d materials=%d dynamic_shadows=%d substrate=%d distance_field_shadowing=%d"),
			Phase, Stage, GFrameCounter, FPlatformTime::Seconds(), *GetPathNameSafe(World), World ? World->GetUniqueID() : 0,
			*GetPathNameSafe(Client ? Client->GetWorld() : nullptr), Size.X, Size.Y,
			Flags ? int32(Flags->Rendering) : -1, Flags ? int32(Flags->Materials) : -1,
			Flags ? int32(Flags->DynamicShadows) : -1, ReadCVar(TEXT("r.Substrate")), ReadCVar(TEXT("r.DistanceFieldShadowing")));
	}

	bool Finish(bool bSuccess, const FString& Message)
	{
		if (bFinished) return true;
		bFinished = true;
		if (AIMAcousticBakeVolume* Volume = VolumeWeak.Get()) Volume->IM_SetEditorValidationProfilingForTest(false);
		TerrainSnapshot.Reset();
		SampleMemory();
		Status = bSuccess ? TEXT("PASS_PREPARATION_ONLY") : TEXT("FAIL_CLOSED");
		ResultMessage = Message;
		const bool bReportSaved = WriteReport();
		if (!bReportSaved && bSuccess)
		{
			bSuccess = false;
			Status = TEXT("FAIL_CLOSED");
			ResultMessage = TEXT("Authoring completed but the required Saved evidence JSON could not be written.");
			WriteReport();
		}
		if (bSuccess)
		{
			Test->AddInfo(FString::Printf(TEXT("PhysicalMaterial preview preparation completed; evidence: %s"), *EvidenceFile));
			UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] PASS PhysicalMaterialPreviewPrepare (preparation only; no listening acceptance) evidence=%s"), *EvidenceFile);
		}
		else
		{
			Test->AddError(ResultMessage);
			UE_LOG(LogTemp, Error, TEXT("[IM][PIE_TEST] FAIL PhysicalMaterialPreviewPrepare reason=%s evidence=%s"), *ResultMessage, *EvidenceFile);
		}
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s PhysicalMaterialPreviewPrepare"), bSuccess ? TEXT("PASS") : TEXT("FAIL"));
		return true;
	}

	bool AuthorAndSave()
	{
		FString Error;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		AIMAcousticBakeVolume* Volume = nullptr;
		if (!FindBakeVolume(World, Volume, Error)) return Finish(false, Error);
		// LoadMap may return before HISM bounds settle. Do not use zero cached
		// boxes to select material bindings or silently miss instance families.
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UStaticMeshComponent*> Meshes;
			It->GetComponents(Meshes);
			for (UStaticMeshComponent* Mesh : Meshes)
			{
				if (IsValid(Mesh) && Mesh->IsRegistered() && Mesh->Bounds.BoxExtent.IsNearlyZero()
					&& !Mesh->CalcBounds(Mesh->GetComponentTransform()).GetBox().GetExtent().IsNearlyZero())
				{
					if (FPlatformTime::Seconds() - InitialReadyStartedAt <= 5.0) return false;
					return Finish(false, TEXT("Initial Rural mesh bounds did not become ready: ") + Mesh->GetPathName());
				}
			}
		}
		if (!CaptureFixedState(World, Volume, StateBefore, Error)) return Finish(false, Error);
		OldBakeAssetPath = StateBefore.BakeAssetPath;
		if (!ValidateFixedContract(StateBefore, Error)) return Finish(false, Error);
		if (bTerrainOnly)
		{
			if (StateBefore.BakeAssetPath != RejectedGrassBake || !Volume->BakedField
				|| Volume->BakedField->SceneFingerprint != TEXT("4E9EC53A546E4ECFABFB8DD53FC063AADC221EA1"))
				return Finish(false, TEXT("Terrain-only restoration requires the exact recorded partial candidate."));
			if (!HashSourcePackages(World, Error) || !SeedPristineTerrain(World, Error)) return Finish(false, Error);
			if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
			if (!FEditorFileUtils::SaveLevel(World->PersistentLevel, TargetMapFile))
				return Finish(false, TEXT("Terrain-only first SaveLevel failed."));
			bAuthorMapSaved = true;
			if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
			TerrainFirstSaveFrame = GFrameCounter;
			Stage = 10;
			return false;
		}

		FBox Region = Volume->BakeBounds->Bounds.GetBox();
		if (!CollectSourceMaterials(Volume, Error)) return Finish(false, Error);
		if (!CollectSlots(World, Region, false, SlotRowsBefore, Error)) return Finish(false, Error);
		if (!ValidateMaterialCoverage(Volume, Error)) return Finish(false, Error);
		if (!PreflightExistingVolumeProfiles(Volume, Error)) return Finish(false, Error);
		if (!PreflightLandscape(World, Error)) return Finish(false, Error);
		if (StateBefore.BakeAssetPath != BakeAssetBefore)
		{
			bRepairRecordedGrassCandidate = StateBefore.BakeAssetPath == RejectedGrassBake
				&& Volume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial
				&& Volume->BakedField && Volume->BakedField->WorldPackage == RuralMap
				&& Volume->BakedField->SceneFingerprint == TEXT("4E9EC53A546E4ECFABFB8DD53FC063AADC221EA1");
			if (StateBefore.BakeAssetPath == RecordedCPUOnlyGrassBake)
			{
				if (!ValidateRecordedCPUOnlyCandidate(World, Volume, Error))
					return Finish(false, TEXT("The exact recorded CPU-only PM candidate failed refresh preflight; no overwrite was made: ") + Error);
				bRefreshRecordedCPUOnlyGrassCandidate = true;
			}
			if (!bRepairRecordedGrassCandidate && !bRefreshRecordedCPUOnlyGrassCandidate)
			{
				bStartedFromValidatedPhysicalCandidate = Volume->MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial
					&& ValidateAuthoredMaterialState(World, Volume, Error) && Volume->ValidateCurrentBake(Error);
				if (!bStartedFromValidatedPhysicalCandidate)
				{
					if (Error.IsEmpty()) Error = TEXT("A non-historical BakeField may be reused as the baseline only when the saved PhysicalMaterial preview and bake validate exactly.");
					return Finish(false, TEXT("The non-historical BakeField is not a validated PhysicalMaterial candidate; no profile overwrite was made: ") + Error);
				}
			}
		}
		if (!HashSourcePackages(World, Error)) return Finish(false, Error);
		if (!SeedPristineTerrain(World, Error)) return Finish(false, Error);

		if (!EnsureProfileAssets(Error)) return Finish(false, Error);
		if (!EnsureMaterialCopies(Error)) return Finish(false, Error);
		for (const FIMMaterialRule& Rule : MaterialRules)
		{
			UMaterialInterface* Source = LoadObject<UMaterialInterface>(nullptr, Rule.SourcePath);
			UMaterialInstanceConstant* Copy = MaterialCopies.FindRef(Rule.SourcePath);
			if (!Source || !Copy) return Finish(false, TEXT("The full producer material redirect table is incomplete."));
			ProducerMaterialRedirects.Add(Source, Copy);
		}
		TMap<UMaterialInterface*, UMaterialInterface*> ConstructionRedirects;
		const TCHAR* FenceMaterialPaths[] = {
			TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/M_Fence_Wires_01.M_Fence_Wires_01"),
			TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/MI_Fence_01.MI_Fence_01"),
			TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/M_Fence_Wires_01_LOD.M_Fence_Wires_01_LOD")
		};
		for (const TCHAR* Path : FenceMaterialPaths)
		{
			UMaterialInterface* Source = LoadObject<UMaterialInterface>(nullptr, Path);
			UMaterialInstanceConstant* Copy = MaterialCopies.FindRef(Path);
			if (!Source || !Copy)
				return Finish(false, TEXT("Construction material redirect is incomplete: ") + FString(Path));
			ConstructionRedirects.Add(Source, Copy);
		}
		if (!IMPrepareRuralConstructionMaterialCopies(World, ConstructionRedirects,
			ConstructionMaterialSummary, Error)) return Finish(false, Error);
		if (!ApplyMaterialCopies(World, Volume, Error)) return Finish(false, Error);
		if (!IMPrepareLandscapeGrassMaterialCopies(World, ProducerMaterialRedirects,
			GrassPreparationSummary, Error)) return Finish(false, Error);
		if (!SetLandscapePhysicalMaterials(World, Error)) return Finish(false, Error);
		if (!ApplyVolumeProfile(Volume, Error)) return Finish(false, Error);

		if (!CollectSlots(World, Region, true, SlotRowsAfter, Error)) return Finish(false, Error);
		if (!ValidateLandscapeCollisionInventory(World, Region, Error)) return Finish(false, Error);
		if (!VerifySourcePackageHashes(Error)) return Finish(false, Error);
		if (!SetProbeHeight(Volume, Error)) return Finish(false, Error);
		if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
		if (!FEditorFileUtils::SaveLevel(World->PersistentLevel, TargetMapFile))
			return Finish(false, TEXT("Saving the authored Rural preview map failed; incomplete authoring and generated assets are preserved."));
		bAuthorMapSaved = true;
		if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
		if (!CaptureFixedState(World, Volume, StateAuthored, Error)) return Finish(false, Error);
		if (!SameFixedState(StateBefore, StateAuthored, false, Error))
			return Finish(false, TEXT("Rural camera, bounds, player/source, source gain/graph, routing, or reverb settings changed during PhysicalMaterial authoring: ") + Error);
		TerrainFirstSaveFrame = GFrameCounter;
		Stage = 10;
		Status = TEXT("AUTHORING_SAVED");
		ResultMessage = TEXT("Exact Rural section overrides, PM profiles, Landscape defaults/layer copies, and recooked collision inventory were saved. Bake remains pending.");
		WriteReport();
		return false;
	}

	bool SeedPristineTerrain(UWorld* World, FString& Error)
	{
		const FString File = FPackageName::LongPackageNameToFilename(PristineTerrainMap, FPackageName::GetMapPackageExtension());
		FString Hash;
		int64 Bytes = 0;
		if (!HashFile(File, Hash, Bytes, Error)) return false;
		if (Hash != PristineTerrainSha1 || Bytes != PristineTerrainBytes)
		{
			Error = TEXT("The proven exact terrain fixture changed; no terrain seed was applied.");
			return false;
		}
		const FString ObjectPath = FString(PristineTerrainMap) + TEXT(".") + FPackageName::GetShortName(PristineTerrainMap);
		UWorld* Reference = LoadObject<UWorld>(nullptr, *ObjectPath);
		if (!Reference || !TerrainSnapshot.Capture(Reference, PristineTerrainMap, Error)) return false;
		const bool bRetargeted = TerrainSnapshot.RetargetMatchingOwnedWorld(World, RuralMap, Error, true);
		TerrainReferenceHash = Hash;
		TerrainCaptureSummary = TerrainSnapshot.GetCaptureSummary();
		return bRetargeted;
	}

	bool RestoreTerrainAndSave()
	{
		if (GFrameCounter <= TerrainFirstSaveFrame) return false;
		FString Error;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!TerrainSnapshot.Restore(Error)) return Finish(false, Error);
		const FString Summary = TerrainSnapshot.GetRestoreSummary();
		if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
		if (!FEditorFileUtils::SaveLevel(World->PersistentLevel, TargetMapFile))
			return Finish(false, TEXT("Saving exact restored terrain failed: ") + Summary);
		if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
		if (!VerifySourcePackageHashes(Error)) return Finish(false, Error);
		if (Stage == 10)
		{
			AuthorTerrainRestoreSummary = Summary;
			bAuthorTerrainRestoredSave = true;
			Stage = bTerrainOnly ? 12 : 1;
		}
		else
		{
			FinalTerrainRestoreSummary = Summary;
			bFinalTerrainRestoredSave = true;
			Stage = 3;
		}
		// LoadMap performs old-world GC checks. The restored-byte helper holds
		// strong world/component/texture refs, so release all of them first.
		TerrainSnapshot.Reset();
		SampleMemory();
		WriteReport();
		return false;
	}

	bool ReloadTerrainOnly()
	{
		if (!bAuthorReloadPerformed)
		{
			if (!FEditorFileUtils::LoadMap(RuralMap, false, true))
				return Finish(false, TEXT("Terrain-only restored map did not reload."));
			bAuthorReloadPerformed = true;
			AuthorReloadFrame = GFrameCounter;
			LogRenderContext(TEXT("terrain_only_map_reloaded"));
			return false;
		}
		if (GFrameCounter <= AuthorReloadFrame) return false;
		FString Error;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		AIMAcousticBakeVolume* Volume = nullptr;
		if (!FindBakeVolume(World, Volume, Error)
			|| !CaptureFixedState(World, Volume, StateReloaded, Error)
			|| !SameFixedState(StateBefore, StateReloaded, false, Error)
			|| !VerifySourcePackageHashes(Error)) return Finish(false, Error);
		return Finish(true, TEXT("Terrain-only exact-byte restoration saved and normally reloaded without changing materials or BakeField. Full independent 16-component identity comparison remains required; old BakeField is not accepted."));
	}

	bool ReloadAndStartBake()
	{
		FString Error;
		if (!bAuthorReloadPerformed)
		{
			const double ReloadStart = FPlatformTime::Seconds();
			if (!FEditorFileUtils::LoadMap(RuralMap, false, true))
				return Finish(false, TEXT("The saved Rural PhysicalMaterial map could not be reloaded before baking."));
			LogRenderContext(TEXT("author_map_reloaded"));
			AuthorReloadMs = (FPlatformTime::Seconds() - ReloadStart) * 1000.0;
			AuthorReloadFrame = GFrameCounter;
			AuthorReloadReadyStartedAt = FPlatformTime::Seconds();
			bAuthorReloadPerformed = true;
			SampleMemory();
			// Registered HISM cached bounds may complete on the next normal
			// Editor frame. Loading repeatedly would never let that work finish.
			return false;
		}
		if (GFrameCounter <= AuthorReloadFrame) return false;
		AuthorPostLoadFrameDelta = GFrameCounter - AuthorReloadFrame;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		AIMAcousticBakeVolume* Volume = nullptr;
		if (!FindBakeVolume(World, Volume, Error)) return Finish(false, Error);
		if (!CaptureFixedState(World, Volume, StateReloaded, Error)) return Finish(false, Error);
		if (!SameFixedState(StateBefore, StateReloaded, false, Error))
			return Finish(false, TEXT("Reloaded Rural settings differ before Bake: ") + Error);
		if (!ValidateAuthoredMaterialState(World, Volume, Error)) return Finish(false, Error);
		if (!IMValidateLandscapeGrassMaterialCopies(World, ProducerMaterialRedirects,
			GrassAuthorReloadSummary, Error)) return Finish(false, Error);
		VolumeWeak = Volume;
		{
			TArray<FVector> Vertices;
			TArray<FIntVector> Triangles;
			TArray<int32> Materials;
			FString Fingerprint;
			const double CaptureStart = FPlatformTime::Seconds();
			if (!Volume->IM_CaptureSceneGeometryForTest(Vertices, Triangles, Materials, Fingerprint))
			{
				if (Volume->Status.StartsWith(TEXT("Scene components are still updating bounds"))
					&& FPlatformTime::Seconds() - AuthorReloadReadyStartedAt <= 5.0) return false;
				return Finish(false, TEXT("Measured physical-material scene capture failed: ") + Volume->Status);
			}
			CaptureMs = (FPlatformTime::Seconds() - CaptureStart) * 1000.0;
			CapturedVertices = Vertices.Num();
			CapturedTriangles = Triangles.Num();
			CapturedNonSolidTriangles = Volume->NonSolidTriangles;
			CapturedNonSolidSections = Volume->NonSolidSections;
			SampleMemory();
		}
		const double GenerateStart = FPlatformTime::Seconds();
		Volume->GenerateProbes();
		GenerateProbesMs = (FPlatformTime::Seconds() - GenerateStart) * 1000.0;
		SampleMemory();
		if (Volume->GeneratedProbes <= 0)
			return Finish(false, TEXT("PhysicalMaterial GenerateProbes failed: ") + Volume->Status);
		ProbeCount = Volume->GeneratedProbes;
		BakeStartedAt = FPlatformTime::Seconds();
		Volume->Bake();
		BakeSubmitMs = (FPlatformTime::Seconds() - BakeStartedAt) * 1000.0;
		SampleMemory();
		if (!Volume->Status.StartsWith(TEXT("Baking")))
			return Finish(false, TEXT("PhysicalMaterial Bake did not start: ") + Volume->Status);
		Stage = 2;
		Status = TEXT("BAKING");
		ResultMessage = TEXT("Authoring was saved and reloaded; asynchronous Bake is running.");
		WriteReport();
		return false;
	}

	bool PollBake()
	{
		FString Error;
		AIMAcousticBakeVolume* Volume = VolumeWeak.Get();
		if (!IsValid(Volume))
			return Finish(false, TEXT("Rural BakeVolume was destroyed during the asynchronous PhysicalMaterial bake."));
		const double Elapsed = FPlatformTime::Seconds() - StartedAt;
		if (Elapsed > MaxSeconds)
		{
			if (Volume->Status.StartsWith(TEXT("Baking")) || Volume->Status.StartsWith(TEXT("Cancellation requested")))
			{
				if (!Volume->Status.StartsWith(TEXT("Cancellation requested"))) Volume->CancelBake();
				return false;
			}
			return Finish(false, TEXT("PhysicalMaterial Rural bake exceeded its 900 second guard: ") + Volume->Status);
		}
		// Bake deliberately retains the previous valid asset pointer until the
		// asynchronous job commits its new GUID asset. Never validate that stale
		// pointer while the producer still reports Baking.
		if (Volume->Status.StartsWith(TEXT("Baking"))) return false;
		BakeReadyElapsedMs = (FPlatformTime::Seconds() - BakeStartedAt) * 1000.0;
		if (!Volume->BakedField)
			return Finish(false, TEXT("PhysicalMaterial Rural bake finished without a new BakeField: ") + Volume->Status);
		if (Volume->BakedField->GetPathName() == OldBakeAssetPath)
			return Finish(false, TEXT("PhysicalMaterial Bake finished without replacing the stale historical BakeField; status=") + Volume->Status);
		if (!Volume->ValidateCurrentBake(Error))
			return Finish(false, TEXT("PhysicalMaterial Rural ValidateCurrentBake failed: ") + Error);
		NewBakeAssetPath = Volume->BakedField->GetPathName();
		NewBakeFingerprint = Volume->BakedField->SceneFingerprint;
		NewBakeWorldPackage = Volume->BakedField->WorldPackage;
		if (NewBakeAssetPath.IsEmpty() || NewBakeAssetPath == OldBakeAssetPath
			|| NewBakeWorldPackage != RuralMap || NewBakeFingerprint.IsEmpty())
			return Finish(false, TEXT("Bake did not produce a fresh complete GUID asset bound to the Rural preview world."));
		if (!ValidateLandscapeCollisionInventory(Volume->GetWorld(),
				Volume->BakeBounds->Bounds.GetBox(), Error))
			return Finish(false, TEXT("Post-bake Landscape collision table verification failed: ") + Error);
		if (!VerifySourcePackageHashes(Error)) return Finish(false, Error);
		if (!TerrainSnapshot.Capture(Volume->GetWorld(), RuralMap, Error)) return Finish(false, Error);
		if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
		if (!FEditorFileUtils::SaveLevel(Volume->GetWorld()->PersistentLevel, TargetMapFile))
			return Finish(false, TEXT("Saving the Rural map's new BakeField binding failed; generated assets are preserved."));
		bFinalMapSaved = true;
		if (!TerrainSnapshot.ValidateCurrentLayout(Error)) return Finish(false, Error);
		TerrainFirstSaveFrame = GFrameCounter;
		Stage = 11;
		Status = TEXT("BAKED_SAVED");
		ResultMessage = TEXT("Bake created a new complete GUID asset. Final persistent map reload is pending.");
		return false;
	}

	bool ReloadAndValidate()
	{
		FString Error;
		if (!bFinalReloadPerformed)
		{
			const double ReloadStart = FPlatformTime::Seconds();
			if (!FEditorFileUtils::LoadMap(RuralMap, false, true))
				return Finish(false, TEXT("The Rural preview failed to reload after saving the new BakeField binding."));
			LogRenderContext(TEXT("final_map_reloaded"));
			FinalReloadMs = (FPlatformTime::Seconds() - ReloadStart) * 1000.0;
			FinalReloadFrame = GFrameCounter;
			FinalReloadReadyStartedAt = FPlatformTime::Seconds();
			bFinalReloadPerformed = true;
			SampleMemory();
			return false;
		}
		if (GFrameCounter <= FinalReloadFrame) return false;
		FinalPostLoadFrameDelta = GFrameCounter - FinalReloadFrame;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		AIMAcousticBakeVolume* Volume = nullptr;
		if (!FindBakeVolume(World, Volume, Error)) return Finish(false, Error);
		FIMFixedSceneState FinalState;
		if (!CaptureFixedState(World, Volume, FinalState, Error)) return Finish(false, Error);
		if (!SameFixedState(StateBefore, FinalState, true, Error))
			return Finish(false, TEXT("Final reload changed the protected Rural Camera3/source/gain/graph/reverb/region contract: ") + Error);
		if (FinalState.BakeAssetPath != NewBakeAssetPath
			|| !Volume->BakedField
			|| Volume->BakedField->SceneFingerprint != NewBakeFingerprint
			|| Volume->BakedField->WorldPackage != RuralMap
			|| Volume->ProbeHeightCm != 100.0f
			|| Volume->ProbeSpacingCm != 100.0f)
			return Finish(false, TEXT("Final reload did not retain this run's new BakeField, fingerprint, 100 cm probe height, and 100 cm spacing."));
		const double ValidateStart = FPlatformTime::Seconds();
		if (!Volume->ValidateCurrentBake(Error))
		{
			if (Error.StartsWith(TEXT("Scene components are still updating bounds"))
				&& FPlatformTime::Seconds() - FinalReloadReadyStartedAt <= 5.0) return false;
			return Finish(false, TEXT("Final reloaded PhysicalMaterial BakeField is invalid: ") + Error);
		}
		FinalValidateMs = (FPlatformTime::Seconds() - ValidateStart) * 1000.0;
		if (!ValidateAuthoredMaterialState(World, Volume, Error)) return Finish(false, Error);
		if (!IMValidateLandscapeGrassMaterialCopies(World, ProducerMaterialRedirects,
			GrassFinalReloadSummary, Error)) return Finish(false, Error);
		if (!ValidateLandscapeCollisionInventory(World, Volume->BakeBounds->Bounds.GetBox(), Error))
			return Finish(false, Error);
		if (!VerifySourcePackageHashes(Error)) return Finish(false, Error);
		// CaptureScene after a normal reload has no in-memory generated probe
		// list. The saved Bake owns the validated persistent list; compare it
		// to the count measured at this run's GenerateProbes boundary.
		FinalRuntimeProbeCount = Volume->GeneratedProbes;
		TArray<FVector4> ReloadedProbes;
		FVector ReloadedOriginCm;
		if (!Volume->BakedField->GetProbePreview(NewBakeWorldPackage,
				NewBakeFingerprint, ReloadedProbes, ReloadedOriginCm, Error))
			return Finish(false, TEXT("Final reloaded Bake probe metadata is invalid: ") + Error);
		ReloadedBakedProbeCount = ReloadedProbes.Num();
		if (ProbeCount <= 0 || ReloadedBakedProbeCount != ProbeCount)
			return Finish(false, FString::Printf(TEXT("Final saved Bake probe count differs from GenerateProbes: generated=%d saved=%d runtime_list=%d."),
				ProbeCount, ReloadedBakedProbeCount, FinalRuntimeProbeCount));
		bFinalReloadValidated = true;
		BakePackageBytes = IFileManager::Get().FileSize(*FPackageName::LongPackageNameToFilename(
			Volume->BakedField->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()));
		if (BakePackageBytes <= 0) return Finish(false, TEXT("Fresh baked asset package is missing or empty after map reload."));
		VolumeWeak = Volume;
		Volume->IM_SetEditorValidationProfilingForTest(true);
		EditorObservationStartedAt = FPlatformTime::Seconds();
		Stage = 4;
		Status = TEXT("OBSERVING_EDITOR_VALIDATION");
		return false;
	}

	bool ObserveEditorValidation()
	{
		AIMAcousticBakeVolume* Volume = VolumeWeak.Get();
		if (!Volume) return Finish(false, TEXT("Reloaded volume disappeared during Editor validation observation."));
		const bool Observed = Volume->IM_GetEditorValidationProfileForTest(EditorTicks,
			EditorCaptureTotalMs, EditorAssetValidateTotalMs, EditorMaxTickMs);
		if (Observed && EditorTicks >= 6)
		{
			FString Error;
			if (!Volume->ValidateCurrentBake(Error)) return Finish(false, TEXT("Idle Editor validation invalidated the fresh bake: ") + Error);
			return Finish(true, TEXT("Persistent PhysicalMaterial preview preparation, reload and six actual Editor validation ticks complete. This is not listening/product acceptance."));
		}
		if (FPlatformTime::Seconds() - EditorObservationStartedAt > 30.0)
			return Finish(false, TEXT("Six actual Editor validation ticker samples were not observed within 30 seconds."));
		return false;
	}

	void SampleMemory()
	{
		const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
		if (MemorySamples == 0)
		{
			BaselinePhysical = Stats.UsedPhysical;
			BaselineVirtual = Stats.UsedVirtual;
		}
		++MemorySamples;
		SampledPeakPhysical = FMath::Max(SampledPeakPhysical, Stats.UsedPhysical);
		SampledPeakVirtual = FMath::Max(SampledPeakVirtual, Stats.UsedVirtual);
		ProcessLifetimePeakPhysical = FMath::Max(ProcessLifetimePeakPhysical, Stats.PeakUsedPhysical);
		ProcessLifetimePeakVirtual = FMath::Max(ProcessLifetimePeakVirtual, Stats.PeakUsedVirtual);
	}

	bool LoadRuralMap(FString& Error)
	{
		if (!GEditor || GEditor->PlayWorld)
		{
			Error = TEXT("No idle Editor world is available.");
			return false;
		}
		UWorld* Current = GEditor->GetEditorWorldContext().World();
		if (Current && Current->GetOutermost()->IsDirty())
		{
			Error = TEXT("Current Editor map has unsaved changes; refusing to discard or overwrite them.");
			return false;
		}
		if (!FPackageName::DoesPackageExist(RuralMap, &TargetMapFile))
		{
			Error = TEXT("The required existing Rural preview map package is missing.");
			return false;
		}
		if (!FPackageName::DoesPackageExist(RuralSourceMap, &SourceMapFile))
		{
			Error = TEXT("The original Rural source map package is missing.");
			return false;
		}
		if (Current && Current->GetOutermost()->GetName() == RuralMap && Current->PersistentLevel)
		{
			UWorld* World = Current;
			if (World->GetWorldPartition() || World->GetStreamingLevels().Num() != 0
				|| World->PersistentLevel->IsUsingExternalActors())
			{
				Error = TEXT("Rural target map uses WorldPartition, streaming levels, or external actors; safe persistent rewrite is unsupported.");
				return false;
			}
			return HashFile(SourceMapFile, SourceMapHashBefore, SourceMapBytes, Error);
		}
		if (!FEditorFileUtils::LoadMap(RuralMap, false, true))
		{
			Error = TEXT("Could not load the exact existing Rural preview map.");
			return false;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World || World->GetOutermost()->GetName() != RuralMap || !World->PersistentLevel)
		{
			Error = TEXT("Loaded Editor world does not match the exact Rural preview package.");
			return false;
		}
		if (World->GetWorldPartition() || World->GetStreamingLevels().Num() != 0
			|| World->PersistentLevel->IsUsingExternalActors())
		{
			Error = TEXT("Rural target map uses WorldPartition, streaming levels, or external actors; safe persistent rewrite is unsupported.");
			return false;
		}
		return HashFile(SourceMapFile, SourceMapHashBefore, SourceMapBytes, Error);
	}

	bool FindBakeVolume(UWorld* InWorld, AIMAcousticBakeVolume*& OutVolume, FString& Error) const
	{
		OutVolume = nullptr;
		int32 Count = 0;
		for (TActorIterator<AIMAcousticBakeVolume> It(InWorld); It; ++It)
		{
			OutVolume = *It;
			++Count;
		}
		if (Count != 1 || !IsValid(OutVolume) || !IsValid(OutVolume->BakeBounds))
		{
			Error = FString::Printf(TEXT("Expected exactly one persistent Rural BakeVolume with bounds; found %d."), Count);
			return false;
		}
		return true;
	}

	bool FindTaggedActor(UWorld* InWorld, const FName Tag, AActor*& OutActor, FString& Error) const
	{
		OutActor = nullptr;
		int32 Count = 0;
		for (TActorIterator<AActor> It(InWorld); It; ++It)
		{
			if ((*It)->ActorHasTag(Tag))
			{
				OutActor = *It;
				++Count;
			}
		}
		if (Count != 1)
		{
			Error = FString::Printf(TEXT("Expected exactly one actor tagged %s; found %d."), *Tag.ToString(), Count);
			return false;
		}
		return true;
	}

	bool CaptureFixedState(UWorld* InWorld, AIMAcousticBakeVolume* Volume, FIMFixedSceneState& Out, FString& Error) const
	{
		if (!InWorld || !Volume || !Volume->BakeBounds)
		{
			Error = TEXT("Cannot capture Rural preview fixed settings from invalid world/volume.");
			return false;
		}
		AActor* Camera = nullptr;
		int32 CameraCount = 0;
		for (TActorIterator<AActor> It(InWorld); It; ++It)
		{
			if ((*It)->GetFName() == FName(TEXT("Camera3"))
				&& (*It)->GetClass()->GetPathName() == TEXT("/Script/CinematicCamera.CineCameraActor"))
			{
				Camera = *It;
				++CameraCount;
			}
		}
		if (CameraCount != 1 || !Camera)
		{
			Error = FString::Printf(TEXT("Expected one persistent CineCameraActor named Camera3; found %d."), CameraCount);
			return false;
		}
		AActor* SourceActor = nullptr;
		if (!FindTaggedActor(InWorld, FName(SourceTag), SourceActor, Error)) return false;
		UIMAcousticSourceComponent* Marker = nullptr;
		TArray<UIMAcousticSourceComponent*> Markers;
		SourceActor->GetComponents(Markers);
		if (Markers.Num() != 1 || !IsValid(Markers[0]))
		{
			Error = FString::Printf(TEXT("Tagged water-drop source must own exactly one source marker; found %d."), Markers.Num());
			return false;
		}
		Marker = Markers[0];
		if (!Marker->ValidateSource(Error)) return false;
		UAudioComponent* Audio = Marker->AudioComponent;
		UMetaSoundSource* MetaSound = Audio ? Cast<UMetaSoundSource>(Audio->Sound) : nullptr;
		if (!Audio || !MetaSound || MetaSound->GetPathName() != FString(WaterDropMetaSound) + TEXT(".MS_WaterDropEryliaa_FullLoopOnPlay")
			|| MetaSound->OutputFormat != EMetaSoundOutputAudioFormat::Stereo || MetaSound->NumChannels != 2
			|| !Audio->bAutoActivate || Audio->bAutoDestroy || !Audio->bStopWhenOwnerDestroyed
			|| Audio->bAllowSpatialization || Audio->bIsUISound || Audio->bOverrideAttenuation
			|| !FMath::IsNearlyEqual(Audio->VolumeMultiplier, 0.7f)
			|| !FMath::IsNearlyEqual(Audio->PitchMultiplier, 1.0f))
		{
			Error = TEXT("Existing Rural full water-drop MetaSound, stereo, autoplay, gain, or native-processing settings differ from the approved preview.");
			return false;
		}

		APlayerStart* PlayerStart = nullptr;
		int32 PlayerStartCount = 0;
		for (TActorIterator<APlayerStart> It(InWorld); It; ++It)
		{
			if ((*It)->PlayerStartTag == FName(PlayerStartTag))
			{
				PlayerStart = *It;
				++PlayerStartCount;
			}
		}
		if (PlayerStartCount != 1 || !PlayerStart)
		{
			Error = FString::Printf(TEXT("Expected one tagged Rural PlayerStart; found %d."), PlayerStartCount);
			return false;
		}

		UAudioComponent* Ambient = nullptr;
		int32 AudioCount = 0;
		int32 MarkerCount = 0;
		for (TActorIterator<AActor> It(InWorld); It; ++It)
		{
			TArray<UAudioComponent*> AudioComponents;
			It->GetComponents(AudioComponents);
			for (UAudioComponent* Component : AudioComponents)
			{
				++AudioCount;
				if (Component != Audio && Component->ComponentHasTag(FName(MutedBackgroundTag)))
					Ambient = Component;
			}
			TArray<UIMAcousticSourceComponent*> ActorMarkers;
			It->GetComponents(ActorMarkers);
			MarkerCount += ActorMarkers.Num();
		}
		if (AudioCount != 2 || MarkerCount != 1 || !Ambient
			|| Ambient->bAutoActivate || !FMath::IsNearlyZero(Ambient->VolumeMultiplier)
			|| !Ambient->ComponentHasTag(FName(MutedBackgroundTag)))
		{
			Error = FString::Printf(TEXT("Rural preview must retain exactly the muted original bed plus one managed water-drop source; audio=%d markers=%d ambient=%s auto_activate=%d gain=%.9g."),
				AudioCount, MarkerCount, *GetPathNameSafe(Ambient),
				Ambient ? static_cast<int32>(Ambient->bAutoActivate) : -1,
				Ambient ? Ambient->VolumeMultiplier : -1.0f);
			return false;
		}
		if (FPackageName::ObjectPathToPackageName(MetaSound->GetPathName()) != WaterDropMetaSound)
		{
			Error = TEXT("Water-drop graph package path differs from the full-loop preview asset.");
			return false;
		}
		if (!Volume->BakedField)
		{
			Error = TEXT("The existing Rural preview has no historical BakeField binding to preserve as the old evidence reference.");
			return false;
		}

		Out.BoundsCenter = Volume->BakeBounds->Bounds.GetBox().GetCenter();
		Out.BoundsExtent = Volume->BakeBounds->Bounds.GetBox().GetExtent();
		Out.CameraLocation = Camera->GetActorLocation();
		Out.PlayerLocation = PlayerStart->GetActorLocation();
		Out.SourceLocation = SourceActor->GetActorLocation();
		Out.BakeAssetPath = Volume->BakedField->GetPathName();
		Out.WaterDropPath = MetaSound->GetPathName();
		Out.AmbientPath = Ambient->GetPathName();
		Out.ProbeSpacingCm = Volume->ProbeSpacingCm;
		Out.ProbeHeightCm = Volume->ProbeHeightCm;
		Out.ReverbWetGain = Volume->ReverbWetGain;
		Out.SourceGain = Audio->VolumeMultiplier;
		Out.SourcePitch = Audio->PitchMultiplier;
		Out.AudioCount = AudioCount;
		Out.MarkerCount = MarkerCount;
		Out.bV2 = Volume->bEnableV2;
		Out.bDirect = Volume->bDirectRoute;
		Out.bPath = Volume->bPathRoute;
		Out.bReverb = Volume->bReverbRoute;
		return true;
	}

	bool ValidateFixedContract(const FIMFixedSceneState& State, FString& Error) const
	{
		const FVector ExpectedCamera(-6597.062988, 14720.866211, 395.261597);
		const FVector ExpectedPlayer(-6597.062988, 14720.866211, 188.891165);
		const FVector ExpectedSource(-6347.062988, 14720.866211, 188.891165);
		if (!State.CameraLocation.Equals(ExpectedCamera, 0.02)
			|| !State.PlayerLocation.Equals(ExpectedPlayer, 0.02)
			|| !State.SourceLocation.Equals(ExpectedSource, 0.02)
			|| !FMath::IsNearlyEqual(State.BoundsExtent.X, 500.0, 0.02)
			|| !FMath::IsNearlyEqual(State.BoundsExtent.Y, 500.0, 0.02)
			|| !State.bV2 || !State.bDirect || !State.bPath || !State.bReverb
			|| !FMath::IsNearlyEqual(State.ReverbWetGain, 0.25f)
			|| !FMath::IsNearlyEqual(State.ProbeSpacingCm, 100.0f)
			|| (State.ProbeHeightCm != 150.0f && State.ProbeHeightCm != 100.0f)
			|| State.WaterDropPath != FString(WaterDropMetaSound) + TEXT(".MS_WaterDropEryliaa_FullLoopOnPlay")
			|| !FMath::IsNearlyEqual(State.SourceGain, 0.7f)
			|| !FMath::IsNearlyEqual(State.SourcePitch, 1.0f))
		{
			Error = TEXT("Rural Camera3 coordinates, 10 m XY bounds, probe spacing, source, full water-drop graph, gain, or V2/Reverb contract is outside the frozen preview.");
			return false;
		}
		return true;
	}

	bool SameFixedState(const FIMFixedSceneState& A, const FIMFixedSceneState& B, bool bAllowNewBakePath, FString& Error) const
	{
		const bool bBakeMatches = bAllowNewBakePath
			? B.BakeAssetPath == NewBakeAssetPath
			: B.BakeAssetPath == A.BakeAssetPath;
		if (!bBakeMatches
			|| !A.BoundsCenter.Equals(B.BoundsCenter, 0.02)
			|| !A.BoundsExtent.Equals(B.BoundsExtent, 0.02)
			|| !A.CameraLocation.Equals(B.CameraLocation, 0.02)
			|| !A.PlayerLocation.Equals(B.PlayerLocation, 0.02)
			|| !A.SourceLocation.Equals(B.SourceLocation, 0.02)
			|| A.WaterDropPath != B.WaterDropPath || A.AmbientPath != B.AmbientPath
			|| A.ProbeSpacingCm != B.ProbeSpacingCm || A.ReverbWetGain != B.ReverbWetGain
			|| A.SourceGain != B.SourceGain || A.SourcePitch != B.SourcePitch
			|| A.AudioCount != B.AudioCount || A.MarkerCount != B.MarkerCount
			|| A.bV2 != B.bV2 || A.bDirect != B.bDirect || A.bPath != B.bPath || A.bReverb != B.bReverb)
		{
			Error = TEXT("At least one protected Rural value changed.");
			return false;
		}
		if (!bAllowNewBakePath && B.ProbeHeightCm != 100.0f)
		{
			Error = TEXT("ProbeHeightCm was not authored to the requested 100 cm ear height.");
			return false;
		}
		return true;
	}

	bool CollectSourceMaterials(AIMAcousticBakeVolume* Volume, FString& Error)
	{
		if (!Volume || Volume->Materials.Num() != LegacyPreviewMaterialCount)
		{
			Error = FString::Printf(TEXT("Expected the historical 48-entry Rural compatibility mapping, found %d."),
				Volume ? Volume->Materials.Num() : 0);
			return false;
		}
		TSet<FString> Seen;
		TSet<FString> ExpectedLegacyPaths;
		static_assert(UE_ARRAY_COUNT(MaterialRules) >= LegacyPreviewMaterialCount);
		for (int32 RuleIndex = 0; RuleIndex < LegacyPreviewMaterialCount; ++RuleIndex)
			ExpectedLegacyPaths.Add(MaterialRules[RuleIndex].SourcePath);
		for (const FIMMaterialRule& Rule : MaterialRules)
		{
			UMaterialInterface* Source = LoadObject<UMaterialInterface>(nullptr, Rule.SourcePath);
			if (!IsValid(Source))
			{
				Error = TEXT("Explicit material path table asset did not load: ") + FString(Rule.SourcePath);
				return false;
			}
			if (Source->GetPhysicalMaterial() && GEngine
				&& Source->GetPhysicalMaterial() != GEngine->DefaultPhysMaterial
				&& Source->GetPathName() != RuleCopyObjectPath(Rule))
			{
				Error = TEXT("Unknown non-default source PhysicalMaterial would be overwritten by the proposed profile: ")
					+ Source->GetPathName() + TEXT(" -> ") + Source->GetPhysicalMaterial()->GetPathName();
				return false;
			}
			FString SourceHash;
			int64 SourceBytes = 0;
			const FString SourceFile = FPackageName::LongPackageNameToFilename(
				Source->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
			if (Source->GetOutermost()->IsDirty() || !HashFile(SourceFile, SourceHash, SourceBytes, Error))
			{
				if (Error.IsEmpty()) Error = TEXT("Original Rural material package is already dirty: ") + Source->GetPathName();
				return false;
			}
			AddPackageHash(Source->GetOutermost(), SourceHash, SourceBytes);
		}
		for (const FIMAcousticMaterialMapping& Mapping : Volume->Materials)
		{
			if (!IsValid(Mapping.Material))
			{
				Error = TEXT("Rural BakeVolume contains a null material mapping.");
				return false;
			}
			const FIMMaterialRule* Rule = FindRule(Mapping.Material->GetPathName());
			if (!Rule || !ExpectedLegacyPaths.Contains(Rule->SourcePath) || Seen.Contains(Rule->SourcePath))
			{
				Error = TEXT("Rural BakeVolume material mapping is unknown or duplicated: ") + Mapping.Material->GetPathName();
				return false;
			}
			Seen.Add(Rule->SourcePath);
		}
		if (Seen.Num() != ExpectedLegacyPaths.Num())
		{
			Error = FString::Printf(TEXT("Rural BakeVolume compatibility mapping covers %d of the expected 48 historical paths."), Seen.Num());
			return false;
		}
		return true;
	}

	void AddPackageHash(UPackage* Package, const FString& Hash, int64 Bytes)
	{
		if (!Package || HashedPackages.Contains(Package->GetName())) return;
		HashedPackages.Add(Package->GetName());
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), Package->GetName());
		Row->SetStringField(TEXT("sha1_before"), Hash);
		Row->SetNumberField(TEXT("bytes"), double(Bytes));
		SourcePackageRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	bool HashSourcePackages(UWorld* InWorld, FString& Error)
	{
		if (!InWorld) return false;
		// The preview child may inherit from this Blueprint, but its templates
		// must never be authored into the original Rural package.
		UObject* SourceBlueprint = LoadObject<UObject>(nullptr,
			TEXT("/Game/RuralAustralia/StaticMeshes/Props/Fence_01/Spline_Fence.Spline_Fence"));
		if (!SourceBlueprint)
		{
			Error = TEXT("Original Rural fence Blueprint did not load for byte verification.");
			return false;
		}
		UPackage* BlueprintPackage = SourceBlueprint->GetOutermost();
		FString BlueprintHash;
		int64 BlueprintBytes = 0;
		const FString BlueprintFilename = FPackageName::LongPackageNameToFilename(
			BlueprintPackage->GetName(), FPackageName::GetAssetPackageExtension());
		if (BlueprintPackage->IsDirty() || !HashFile(BlueprintFilename, BlueprintHash, BlueprintBytes, Error))
		{
			if (Error.IsEmpty()) Error = TEXT("Original Rural fence Blueprint package is already dirty.");
			return false;
		}
		AddPackageHash(BlueprintPackage, BlueprintHash, BlueprintBytes);
		for (TActorIterator<AActor> It(InWorld); It; ++It)
		{
			TArray<UPrimitiveComponent*> Primitives;
			It->GetComponents(Primitives);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				if (!IsValid(Primitive)
					|| Primitive->ComponentHasTag(FName(IgnoreTag))
					|| Primitive->ComponentHasTag(FName(BlockingTag))
					|| Primitive->IsEditorOnly())
					continue;
				UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive);
				if (!Mesh || !Mesh->GetStaticMesh()) continue;
				UPackage* Package = Mesh->GetStaticMesh()->GetOutermost();
				FString Hash;
				int64 Bytes = 0;
				const FString Filename = FPackageName::LongPackageNameToFilename(
					Package->GetName(), FPackageName::GetAssetPackageExtension());
				if (Package->IsDirty() || !HashFile(Filename, Hash, Bytes, Error))
				{
					if (Error.IsEmpty()) Error = TEXT("Original Rural StaticMesh package is already dirty: ") + Package->GetName();
					return false;
				}
				AddPackageHash(Package, Hash, Bytes);
			}
		}
		return true;
	}

	bool PreflightLandscape(UWorld* InWorld, FString& Error)
	{
		if (!GEngine || !GEngine->DefaultPhysMaterial)
		{
			Error = TEXT("Engine default PhysicalMaterial is unavailable for fail-closed comparisons.");
			return false;
		}
		for (TActorIterator<ALandscapeProxy> It(InWorld); It; ++It)
		{
			ALandscapeProxy* Landscape = *It;
			UPhysicalMaterial* CurrentDefault = Landscape->DefaultPhysMaterial;
			UPhysicalMaterial* ExpectedSoil = LoadExistingProfilePM(EIMSurfaceProfile::Soil);
			if (CurrentDefault && CurrentDefault != GEngine->DefaultPhysMaterial
				&& CurrentDefault != ExpectedSoil)
			{
				Error = TEXT("Landscape has an unknown non-default DefaultPhysMaterial; preserving it and stopping: ")
					+ Landscape->GetPathName() + TEXT(" -> ") + CurrentDefault->GetPathName();
				return false;
			}
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			if (!Info)
			{
				Error = TEXT("Landscape proxy has no resolved LandscapeInfo: ") + Landscape->GetPathName();
				return false;
			}
			for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
			{
				ULandscapeLayerInfoObject* LayerInfo = Layer.LayerInfoObj;
				if (!IsValid(LayerInfo))
				{
					Error = TEXT("LandscapeInfo contains a layer with no LayerInfo asset; per-layer PM repair cannot be applied safely.");
					return false;
				}
				UPhysicalMaterial* LayerPM = LayerInfo->GetPhysicalMaterial();
				UPhysicalMaterial* ExpectedSoilPM = LoadExistingProfilePM(EIMSurfaceProfile::Soil);
				const bool bIsPluginLayerCopy = LayerInfo->GetOutermost()->GetName().StartsWith(
					FString(PhysicalMaterialRoot) + TEXT("/Layer_"), ESearchCase::CaseSensitive);
				if (LayerPM && LayerPM != GEngine->DefaultPhysMaterial
					&& !(bIsPluginLayerCopy && ExpectedSoilPM && LayerPM == ExpectedSoilPM))
				{
					Error = TEXT("Landscape layer has an unknown non-default PhysicalMaterial; preserving it and stopping: ")
						+ LayerInfo->GetPathName() + TEXT(" -> ") + LayerPM->GetPathName();
					return false;
				}
			}
		}
		return true;
	}

	bool CollectSlots(UWorld* InWorld, const FBox& Region, bool bExpectCopies,
		TArray<TSharedPtr<FJsonValue>>& OutRows, FString& Error)
	{
		OutRows.Reset();
		TSet<FString> UniqueSourcePaths;
		TSet<FString> UnknownSourcePaths;
		if (bExpectCopies) bAfterSlotInventoryComplete = false;
		else bBeforeSlotInventoryComplete = false;
		for (TActorIterator<AActor> It(InWorld); It; ++It)
		{
			if (*It == InWorld->GetDefaultBrush() || *It == InWorld->GetDefaultPhysicsVolume()
				|| It->IsA<AIMAcousticBakeVolume>())
				continue;
			TArray<UPrimitiveComponent*> Primitives;
			It->GetComponents(Primitives);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				if (!IsValid(Primitive)
					|| Primitive->ComponentHasTag(FName(IgnoreTag))
					|| Primitive->ComponentHasTag(FName(BlockingTag))
					|| Primitive->IsEditorOnly())
					continue;
				if (Primitive->IsA<ULandscapeHeightfieldCollisionComponent>())
					continue;

				ULandscapeComponent* LandscapeComponent = Cast<ULandscapeComponent>(Primitive);
				UStaticMeshComponent* Mesh = LandscapeComponent ? nullptr : Cast<UStaticMeshComponent>(Primitive);
				if (!LandscapeComponent && !Mesh)
				{
					// Bind model PMs for both static and movable mesh families.
					// Other movable primitives retain the existing explicit
					// dynamic-registration contract and are not authored here.
					if (Primitive->Mobility != EComponentMobility::Static) continue;
					const FString ComponentClass = Primitive->GetClass()->GetPathName();
					// This is the Landscape authoring container, not the actual
					// USplineMeshComponents captured through the mesh path below.
					const FString ActorClass = It->GetClass()->GetPathName();
					if (ComponentClass == TEXT("/Script/Landscape.LandscapeSplinesComponent")
						|| (ComponentClass == TEXT("/Script/Engine.BrushComponent")
							&& (ActorClass == TEXT("/Script/Engine.LightmassImportanceVolume")
								|| ActorClass == TEXT("/Script/Engine.BlockingVolume"))))
						continue;
					Error = TEXT("Unsupported static primitive overlaps the Rural PM bake region: ") + Primitive->GetPathName();
					return false;
				}

				int32 NumSlots = Primitive->GetNumMaterials();
				if (!NumSlots || (LandscapeComponent && NumSlots != 1))
				{
					Error = TEXT("In-region Rural surface has no explicit material slots: ") + Primitive->GetPathName();
					return false;
				}
				UStaticMesh* StaticMesh = Mesh ? Mesh->GetStaticMesh() : nullptr;
				if (Mesh && (!StaticMesh || !StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.IsEmpty()))
				{
					Error = TEXT("In-region Rural StaticMesh has no readable LOD0 section table: ") + Mesh->GetPathName();
					return false;
				}

				for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
				{
					UMaterialInterface* Material = Primitive->GetMaterial(SlotIndex);
					if (!IsValid(Material))
					{
						Error = TEXT("In-region Rural surface has a null actual material slot: ") + Primitive->GetPathName();
						return false;
					}
					const FString MaterialPath = Material->GetPathName();
					const FIMMaterialRule* Rule = FindRule(MaterialPath);
					if (!Rule)
					{
						// Finish the read-only inventory before rejecting unknown
						// materials. A first-slot failure only exposes a prefix and
						// would require one Editor run per missing material.
						UnknownSourcePaths.Add(MaterialPath);
						TSharedRef<FJsonObject> UnknownRow = MakeShared<FJsonObject>();
						UnknownRow->SetStringField(TEXT("component_path"), Primitive->GetPathName());
						UnknownRow->SetStringField(TEXT("component_class"), Primitive->GetClass()->GetPathName());
						UnknownRow->SetNumberField(TEXT("component_mobility"), int32(Primitive->Mobility));
						UnknownRow->SetBoolField(TEXT("component_bounds_intersects_bake_region"), Region.Intersect(Primitive->Bounds.GetBox()));
						if (UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Mesh))
							UnknownRow->SetNumberField(TEXT("component_instance_count"), Instances->GetInstanceCount());
						UnknownRow->SetStringField(TEXT("mesh_path"), StaticMesh ? StaticMesh->GetPathName() : TEXT("LandscapeHeightfield"));
						UnknownRow->SetNumberField(TEXT("slot_index"), SlotIndex);
						TArray<TSharedPtr<FJsonValue>> UnknownSections;
						if (Mesh)
						{
							const FStaticMeshLODResources& LOD0 = StaticMesh->GetRenderData()->LODResources[0];
							for (int32 SectionIndex = 0; SectionIndex < LOD0.Sections.Num(); ++SectionIndex)
								if (LOD0.Sections[SectionIndex].MaterialIndex == SlotIndex)
									UnknownSections.Add(MakeShared<FJsonValueNumber>(SectionIndex));
						}
						else UnknownSections.Add(MakeShared<FJsonValueNumber>(0));
						UnknownRow->SetArrayField(TEXT("lod0_sections"), UnknownSections);
						UnknownRow->SetStringField(TEXT("material_path"), MaterialPath);
						UnknownRow->SetStringField(TEXT("source_material_path"), MaterialPath);
						UnknownRow->SetStringField(TEXT("profile"), TEXT("UNCLASSIFIED"));
						UnknownRow->SetStringField(TEXT("effective_physical_material"), GetPathNameSafe(Material->GetPhysicalMaterial()));
						UnknownRow->SetBoolField(TEXT("plugin_mi_copy"), false);
						OutRows.Add(MakeShared<FJsonValueObject>(UnknownRow));
						continue;
					}
					UniqueSourcePaths.Add(Rule->SourcePath);
					UMaterialInterface* Source = LoadObject<UMaterialInterface>(nullptr, Rule->SourcePath);
					UMaterialInstanceConstant* Copy = LoadObject<UMaterialInstanceConstant>(nullptr, *RuleCopyObjectPath(*Rule));
					const FIMProfileSpec* Profile = ProfileFor(Rule->Profile);
					UPhysicalMaterial* ExpectedPM = LoadExistingProfilePM(Rule->Profile);
					const bool bCopyPath = MaterialPath == RuleCopyObjectPath(*Rule);
					if (!Source || !Profile)
					{
						Error = TEXT("Rural slot rule could not resolve its source or explicit category: ") + FString(Rule->SourcePath);
						return false;
					}
					if (bExpectCopies)
					{
						if (!bCopyPath || !Copy || !ExpectedPM || Material != Copy || Material->GetPhysicalMaterial() != ExpectedPM)
						{
							Error = TEXT("Post-authoring Rural slot does not resolve to its plugin MI and expected PhysicalMaterial: ")
								+ Primitive->GetPathName() + TEXT(" slot=") + FString::FromInt(SlotIndex);
							return false;
						}
					}
					else if (bCopyPath)
					{
						if (!Copy || !ExpectedPM || Material->GetPhysicalMaterial() != ExpectedPM)
						{
							if (!IsKnownScatterLeafReclassification(*Rule, Source, Copy, ExpectedPM))
							{
								Error = TEXT("Existing plugin MI copy has an unexpected current PhysicalMaterial: ") + MaterialPath;
								return false;
							}
							UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticKnownProfileTransition preflight_allowed=1 copy=%s from=Wood to=Leaf"), *MaterialPath);
						}
					}
					else
					{
						UPhysicalMaterial* CurrentPM = Material->GetPhysicalMaterial();
						if (CurrentPM && GEngine && CurrentPM != GEngine->DefaultPhysMaterial)
						{
							Error = TEXT("Unknown non-default effective PhysicalMaterial on an original Rural slot; no profile overwrite was made: ")
								+ MaterialPath + TEXT(" -> ") + CurrentPM->GetPathName();
							return false;
						}
					}

					TArray<TSharedPtr<FJsonValue>> SectionIndices;
					if (Mesh)
					{
						const FStaticMeshLODResources& LOD0 = StaticMesh->GetRenderData()->LODResources[0];
						for (int32 SectionIndex = 0; SectionIndex < LOD0.Sections.Num(); ++SectionIndex)
						{
							if (LOD0.Sections[SectionIndex].MaterialIndex == SlotIndex)
								SectionIndices.Add(MakeShared<FJsonValueNumber>(SectionIndex));
						}
					}
					else
					{
						SectionIndices.Add(MakeShared<FJsonValueNumber>(0));
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("component_path"), Primitive->GetPathName());
					Row->SetStringField(TEXT("component_class"), Primitive->GetClass()->GetPathName());
					Row->SetNumberField(TEXT("component_mobility"), int32(Primitive->Mobility));
					Row->SetBoolField(TEXT("component_bounds_intersects_bake_region"), Region.Intersect(Primitive->Bounds.GetBox()));
					if (UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Mesh))
						Row->SetNumberField(TEXT("component_instance_count"), Instances->GetInstanceCount());
					Row->SetStringField(TEXT("mesh_path"), StaticMesh ? StaticMesh->GetPathName() : TEXT("LandscapeHeightfield"));
					Row->SetNumberField(TEXT("slot_index"), SlotIndex);
					Row->SetArrayField(TEXT("lod0_sections"), SectionIndices);
					Row->SetStringField(TEXT("material_path"), MaterialPath);
					Row->SetStringField(TEXT("source_material_path"), Rule->SourcePath);
					Row->SetStringField(TEXT("profile"), ProfileName(Rule->Profile));
					Row->SetStringField(TEXT("effective_physical_material"), GetPathNameSafe(Material->GetPhysicalMaterial()));
					Row->SetBoolField(TEXT("plugin_mi_copy"), bCopyPath);
					OutRows.Add(MakeShared<FJsonValueObject>(Row));
				}

				if (Mesh)
				{
					FBodyInstance* Body = Mesh->GetBodyInstance();
					const FString OverridePath = Body ? GetPathNameSafe(Body->GetPhysMaterialOverride()) : TEXT("");
					TSharedRef<FJsonObject> OverrideRow = MakeShared<FJsonObject>();
					OverrideRow->SetStringField(TEXT("component_path"), Mesh->GetPathName());
					OverrideRow->SetStringField(TEXT("mesh_path"), StaticMesh->GetPathName());
					OverrideRow->SetStringField(TEXT("body_phys_material_override"), OverridePath);
					OverrideRow->SetBoolField(TEXT("body_instance_available"), Body != nullptr);
					if (Body && Body->GetPhysMaterialOverride() && GEngine
						&& Body->GetPhysMaterialOverride() != GEngine->DefaultPhysMaterial)
					{
						Error = TEXT("Unknown non-default component body PhysMaterialOverride in the Rural scene: ")
							+ Mesh->GetPathName() + TEXT(" -> ") + OverridePath;
						return false;
					}
					if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Mesh))
					{
						TArray<TSharedPtr<FJsonValue>> InstanceOverrides;
						for (int32 InstanceIndex = 0; InstanceIndex < ISM->GetInstanceCount(); ++InstanceIndex)
						{
							FBodyInstance* InstanceBody = ISM->GetBodyInstance(NAME_None, true, InstanceIndex);
							const FString InstanceOverride = InstanceBody
								? GetPathNameSafe(InstanceBody->GetPhysMaterialOverride()) : FString();
							if (InstanceBody && InstanceBody->GetPhysMaterialOverride() && GEngine
								&& InstanceBody->GetPhysMaterialOverride() != GEngine->DefaultPhysMaterial)
							{
								Error = FString::Printf(TEXT("Unknown non-default ISM instance PhysMaterialOverride: %s instance=%d -> %s"),
									*ISM->GetPathName(), InstanceIndex, *InstanceOverride);
								return false;
							}
							if (InstanceBody && InstanceBody->GetPhysMaterialOverride())
							{
								TSharedRef<FJsonObject> InstanceRow = MakeShared<FJsonObject>();
								InstanceRow->SetNumberField(TEXT("instance_index"), InstanceIndex);
								InstanceRow->SetStringField(TEXT("override"), InstanceOverride);
								InstanceOverrides.Add(MakeShared<FJsonValueObject>(InstanceRow));
							}
						}
						OverrideRow->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());
						OverrideRow->SetArrayField(TEXT("instance_overrides"), InstanceOverrides);
					}
					BodyOverrideRows.Add(MakeShared<FJsonValueObject>(OverrideRow));
				}
			}
		}
		if (bExpectCopies) bAfterSlotInventoryComplete = true;
		else bBeforeSlotInventoryComplete = true;
		if (!UnknownSourcePaths.IsEmpty())
		{
			TArray<FString> UnknownPaths = UnknownSourcePaths.Array();
			UnknownPaths.Sort();
			Error = FString::Printf(TEXT("Full-scene inventory completed before asset writes; %d actual material paths lack explicit rules: %s"),
				UnknownPaths.Num(), *FString::Join(UnknownPaths, TEXT("; ")));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>>& UnusedRules = bExpectCopies ? AfterUnusedRules : BeforeUnusedRules;
		UnusedRules.Reset();
		for (const FIMMaterialRule& Rule : MaterialRules)
		{
			if (!UniqueSourcePaths.Contains(Rule.SourcePath))
				UnusedRules.Add(MakeShared<FJsonValueString>(Rule.SourcePath));
		}
		// Rules may explicitly cover a known model excluded by a current tag.
		// Every observed slot must resolve; unused superset rules are reported,
		// not treated as evidence that an excluded object was captured.
		return true;
	}

	bool ValidateMaterialCoverage(AIMAcousticBakeVolume* Volume, FString& Error) const
	{
		TSet<FString> Actual;
		for (const TSharedPtr<FJsonValue>& Value : SlotRowsBefore)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			Actual.Add(Row->GetStringField(TEXT("source_material_path")));
		}
		for (const FIMAcousticMaterialMapping& Mapping : Volume->Materials)
		{
			const FIMMaterialRule* Rule = Mapping.Material ? FindRule(Mapping.Material->GetPathName()) : nullptr;
			if (!Rule || !Actual.Contains(Rule->SourcePath))
			{
				Error = TEXT("A Rural BakeVolume compatibility mapping lacks a recorded full-scene component/slot: ")
					+ GetPathNameSafe(Mapping.Material);
				return false;
			}
		}
		return true;
	}

	bool PreflightExistingVolumeProfiles(AIMAcousticBakeVolume* Volume, FString& Error) const
	{
		if (!Volume || Volume->PhysicalMaterials.IsEmpty()) return true;
		if (Volume->PhysicalMaterials.Num() != UE_ARRAY_COUNT(ProfileSpecs))
		{
			Error = FString::Printf(TEXT("Existing PhysicalMaterial profile table has %d entries; expected %d explicit profiles. No overwrite was made."),
				Volume->PhysicalMaterials.Num(), int32(UE_ARRAY_COUNT(ProfileSpecs)));
			return false;
		}
		TSet<FString> SeenProfiles;
		for (const FIMAcousticPhysicalMaterialMapping& Mapping : Volume->PhysicalMaterials)
		{
			bool bMatched = false;
			for (const FIMProfileSpec& Spec : ProfileSpecs)
			{
				UPhysicalMaterial* ExpectedPM = LoadExistingProfilePM(Spec.Profile);
				if (!ExpectedPM || Mapping.PhysicalMaterial != ExpectedPM) continue;
				const FString Name = Spec.Name;
				if (SeenProfiles.Contains(Name)
					|| !Mapping.Absorption.Equals(Spec.Absorption, 0.000001)
					|| !FMath::IsNearlyEqual(Mapping.Scattering, Spec.Scattering, 0.000001f)
					|| Mapping.bSolidGeometry != Spec.bSolidGeometry
					|| Mapping.bSupportsProbes != Spec.bSupportsProbes)
				{
					Error = TEXT("Existing PhysicalMaterial profile values differ from the exact explicit table; no overwrite was made: ") + Name;
					return false;
				}
				SeenProfiles.Add(Name);
				bMatched = true;
				break;
			}
			if (!bMatched)
			{
				Error = TEXT("Existing PhysicalMaterial table contains an unknown or missing profile asset; no overwrite was made: ")
					+ GetPathNameSafe(Mapping.PhysicalMaterial);
				return false;
			}
		}
		if (SeenProfiles.Num() != UE_ARRAY_COUNT(ProfileSpecs))
		{
			Error = TEXT("Existing PhysicalMaterial table does not cover each explicit profile exactly once; no overwrite was made.");
			return false;
		}
		return true;
	}

	bool ValidateRecordedCPUOnlyCandidate(UWorld* InWorld, AIMAcousticBakeVolume* Volume, FString& Error)
	{
		if (!Volume || !Volume->BakedField || Volume->BakedField->GetPathName() != RecordedCPUOnlyGrassBake
			|| Volume->MaterialDrive != EIMAcousticMaterialDrive::PhysicalMaterial
			|| Volume->BakedField->WorldPackage != RuralMap
			|| Volume->BakedField->SceneFingerprint != RecordedCPUOnlyFingerprint
			|| Volume->BakedField->GetOutermost()->IsDirty())
		{
			Error = TEXT("Recorded CPU-only candidate identity, PM mode, world, fingerprint or clean package differs.");
			return false;
		}
		if (!ValidateAuthoredMaterialState(InWorld, Volume, Error)
			|| !Volume->BakedField->Validate(RuralMap, RecordedCPUOnlyFingerprint, Error)) return false;
		FString Filename, PackageSHA1;
		int64 PackageBytes = -1;
		if (!FPackageName::DoesPackageExist(Volume->BakedField->GetOutermost()->GetName(), &Filename)
			|| !HashFile(Filename, PackageSHA1, PackageBytes, Error))
		{
			if (Error.IsEmpty()) Error = TEXT("Recorded CPU-only candidate package could not be resolved.");
			return false;
		}
		if (PackageSHA1 != RecordedCPUOnlyPackageSHA1 || PackageBytes != RecordedCPUOnlyPackageBytes)
		{
			Error = TEXT("Recorded CPU-only candidate package bytes changed; unknown work cannot be refreshed.");
			return false;
		}
		return true;
	}

	bool EnsureProfileAssets(FString& Error)
	{
		for (const FIMProfileSpec& Spec : ProfileSpecs)
		{
			UPhysicalMaterial* PhysicalMaterial = CreateOrLoadProfilePM(Spec.Profile, Error);
			if (!PhysicalMaterial) return false;
			ProfileAssets.Add(Spec.Profile, PhysicalMaterial);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("profile"), Spec.Name);
			Row->SetStringField(TEXT("physical_material"), PhysicalMaterial->GetPathName());
			AddVector(Row, TEXT("absorption"), Spec.Absorption);
			Row->SetNumberField(TEXT("scattering"), Spec.Scattering);
			Row->SetBoolField(TEXT("bSolidGeometry"), Spec.bSolidGeometry);
			Row->SetBoolField(TEXT("bSupportsProbes"), Spec.bSupportsProbes);
			Row->SetStringField(TEXT("value_status"), Spec.EvidenceLabel);
			ProfileRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return true;
	}

	bool EnsureMaterialCopies(FString& Error)
	{
		for (const FIMMaterialRule& Rule : MaterialRules)
		{
			UMaterialInterface* Source = LoadObject<UMaterialInterface>(nullptr, Rule.SourcePath);
			UPhysicalMaterial* PhysicalMaterial = ProfileAssets.FindRef(Rule.Profile);
			if (!Source || !PhysicalMaterial)
			{
				Error = TEXT("Material copy source or profile asset was not loaded: ") + FString(Rule.SourcePath);
				return false;
			}
			UMaterialInstanceConstant* Copy = LoadOrCreateMaterialCopy(Rule, Source, PhysicalMaterial, Error);
			if (!Copy) return false;
			MaterialCopies.Add(Rule.SourcePath, Copy);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("source_material"), Source->GetPathName());
			Row->SetStringField(TEXT("plugin_mi_copy"), Copy->GetPathName());
			Row->SetStringField(TEXT("parent"), GetPathNameSafe(Copy->Parent));
			Row->SetStringField(TEXT("profile"), ProfileName(Rule.Profile));
			Row->SetStringField(TEXT("physical_material"), GetPathNameSafe(Copy->GetPhysicalMaterial()));
			MaterialRuleRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return MaterialCopies.Num() == UE_ARRAY_COUNT(MaterialRules);
	}

	bool ApplyMaterialCopies(UWorld* InWorld, AIMAcousticBakeVolume* Volume, FString& Error)
	{
		for (TActorIterator<AActor> It(InWorld); It; ++It)
		{
			TArray<UPrimitiveComponent*> Primitives;
			It->GetComponents(Primitives);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				if (!IsValid(Primitive)
					|| Primitive->ComponentHasTag(FName(IgnoreTag))
					|| Primitive->ComponentHasTag(FName(BlockingTag))
					|| Primitive->IsEditorOnly())
					continue;
				if (Primitive->IsA<ULandscapeHeightfieldCollisionComponent>())
					continue;
				if (!Cast<ULandscapeComponent>(Primitive) && !Cast<UStaticMeshComponent>(Primitive))
					continue;
				Primitive->Modify();
				for (int32 SlotIndex = 0; SlotIndex < Primitive->GetNumMaterials(); ++SlotIndex)
				{
					UMaterialInterface* Current = Primitive->GetMaterial(SlotIndex);
					const FIMMaterialRule* Rule = Current ? FindRule(Current->GetPathName()) : nullptr;
					if (!Rule)
					{
						Error = TEXT("Rural component material changed after preflight: ") + Primitive->GetPathName();
						return false;
					}
					UMaterialInstanceConstant* Copy = MaterialCopies.FindRef(Rule->SourcePath);
					if (!Copy)
					{
						Error = TEXT("No plugin MI copy exists for Rural slot: ") + FString(Rule->SourcePath);
						return false;
					}
					Primitive->SetMaterial(SlotIndex, Copy);
				}
			}
		}
		Volume->Modify();
		for (FIMAcousticMaterialMapping& Mapping : Volume->Materials)
		{
			const FIMMaterialRule* Rule = Mapping.Material ? FindRule(Mapping.Material->GetPathName()) : nullptr;
			UMaterialInstanceConstant* Copy = Rule ? MaterialCopies.FindRef(Rule->SourcePath) : nullptr;
			if (!Copy)
			{
				Error = TEXT("Rural BakeVolume mapping could not be redirected to the exact plugin MI copy.");
				return false;
			}
			Mapping.Material = Copy;
		}
		return true;
	}

	bool SetLandscapePhysicalMaterials(UWorld* InWorld, FString& Error)
	{
		UPhysicalMaterial* SoilPM = ProfileAssets.FindRef(EIMSurfaceProfile::Soil);
		if (!SoilPM)
		{
			Error = TEXT("Soil PhysicalMaterial profile is unavailable for Landscape fallback/layers.");
			return false;
		}
		TSet<ULandscapeInfo*> ProcessedInfos;
		for (TActorIterator<ALandscapeProxy> It(InWorld); It; ++It)
		{
			ALandscapeProxy* Landscape = *It;
			Landscape->Modify();
			Landscape->DefaultPhysMaterial = SoilPM;
			TSharedRef<FJsonObject> ProxyRow = MakeShared<FJsonObject>();
			ProxyRow->SetStringField(TEXT("landscape_proxy"), Landscape->GetPathName());
			ProxyRow->SetStringField(TEXT("default_physical_material"), GetPathNameSafe(Landscape->DefaultPhysMaterial));
			LandscapeRows.Add(MakeShared<FJsonValueObject>(ProxyRow));

			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			if (!Info)
			{
				Error = TEXT("LandscapeInfo disappeared during per-layer PM authoring: ") + Landscape->GetPathName();
				return false;
			}
			if (ProcessedInfos.Contains(Info)) continue;
			ProcessedInfos.Add(Info);
			TArray<ULandscapeLayerInfoObject*> Layers;
			for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
			{
				if (IsValid(Layer.LayerInfoObj)) Layers.AddUnique(Layer.LayerInfoObj);
			}
			for (ULandscapeLayerInfoObject* OriginalLayer : Layers)
			{
				ULandscapeLayerInfoObject* LayerCopy = CreateOrLoadLayerCopy(OriginalLayer, SoilPM, Error);
				if (!LayerCopy) return false;
				if (OriginalLayer != LayerCopy)
				{
					Info->ReplaceLayer(OriginalLayer, LayerCopy);
					Info->UpdateLayerInfoMap(nullptr, true);
				}
				TSharedRef<FJsonObject> LayerRow = MakeShared<FJsonObject>();
				LayerRow->SetStringField(TEXT("source_layer_info"), OriginalLayer->GetPathName());
				LayerRow->SetStringField(TEXT("plugin_layer_copy"), LayerCopy->GetPathName());
				LayerRow->SetStringField(TEXT("physical_material"), GetPathNameSafe(LayerCopy->GetPhysicalMaterial()));
				LandscapeLayerRows.Add(MakeShared<FJsonValueObject>(LayerRow));
			}
		}
		// Refresh the registered collision material/DLD from current layer allocations.
		// Proxy-wide RecreateCollisionComponents also flushes generated grass HISM;
		// this PM-only edit must retain those instances and never hand-write cooked PMs.
		for (TActorIterator<ALandscapeProxy> It(InWorld); It; ++It)
		{
			It->ChangedPhysMaterial();
		}
		return !ProcessedInfos.IsEmpty();
	}

	ULandscapeLayerInfoObject* CreateOrLoadLayerCopy(
		ULandscapeLayerInfoObject* OriginalLayer, UPhysicalMaterial* SoilPM, FString& Error)
	{
		if (OriginalLayer->GetOutermost()->GetName().StartsWith(
				FString(PhysicalMaterialRoot) + TEXT("/Layer_"), ESearchCase::CaseSensitive))
		{
			if (OriginalLayer->GetPhysicalMaterial() != SoilPM)
			{
				Error = TEXT("Existing plugin LayerInfo copy has an unexpected PhysicalMaterial: ") + OriginalLayer->GetPathName();
				return nullptr;
			}
			return OriginalLayer;
		}
		const FString SourcePackage = OriginalLayer->GetOutermost()->GetName();
		const FString SourceObject = OriginalLayer->GetName();
		uint8 Digest[20];
		FSHA1::HashBuffer(reinterpret_cast<const uint8*>(*SourcePackage),
			SourcePackage.Len() * sizeof(TCHAR), Digest);
		const FString Suffix = BytesToHex(Digest, 4);
		const FString CopyName = TEXT("Layer_") + SafeAssetPart(SourceObject) + TEXT("_") + Suffix;
		const FString PackagePath = FString(PhysicalMaterialRoot) + TEXT("/") + CopyName;
		const FString ObjectPath = PackagePath + TEXT(".") + CopyName;
		if (ULandscapeLayerInfoObject* Existing = LoadObject<ULandscapeLayerInfoObject>(nullptr, *ObjectPath))
		{
			if (Existing->GetPhysicalMaterial() != SoilPM)
			{
				Error = TEXT("Existing plugin LayerInfo copy has an unexpected PhysicalMaterial: ") + ObjectPath;
				return nullptr;
			}
			return Existing;
		}
		if (FPackageName::DoesPackageExist(PackagePath))
		{
			Error = TEXT("LayerInfo copy package exists with an unexpected asset class: ") + PackagePath;
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackagePath);
		ULandscapeLayerInfoObject* Copy = Package
			? Cast<ULandscapeLayerInfoObject>(StaticDuplicateObject(OriginalLayer, Package, FName(*CopyName)))
			: nullptr;
		if (!Copy)
		{
			Error = TEXT("Could not duplicate Landscape LayerInfo into the plugin copy folder: ") + OriginalLayer->GetPathName();
			return nullptr;
		}
		Copy->SetFlags(RF_Public | RF_Standalone);
		Copy->Modify();
		Copy->SetPhysicalMaterial(SoilPM, true);
		Copy->PostEditChange();
		if (!SaveAsset(Copy, Error)) return nullptr;
		return Copy;
	}

	bool ApplyVolumeProfile(AIMAcousticBakeVolume* Volume, FString& Error)
	{
		if (!Volume || Volume->Materials.Num() != LegacyPreviewMaterialCount)
		{
			Error = TEXT("Rural BakeVolume lost its exact 48 material mappings during authoring.");
			return false;
		}
		Volume->Modify();
		Volume->MaterialDrive = EIMAcousticMaterialDrive::PhysicalMaterial;
		Volume->PhysicalMaterials.Reset();
		for (const FIMProfileSpec& Spec : ProfileSpecs)
		{
			UPhysicalMaterial* PhysicalMaterial = ProfileAssets.FindRef(Spec.Profile);
			if (!PhysicalMaterial)
			{
				Error = TEXT("A required profile PhysicalMaterial disappeared before volume save.");
				return false;
			}
			FIMAcousticPhysicalMaterialMapping& Mapping = Volume->PhysicalMaterials.AddDefaulted_GetRef();
			Mapping.PhysicalMaterial = PhysicalMaterial;
			Mapping.Absorption = Spec.Absorption;
			Mapping.Scattering = Spec.Scattering;
			Mapping.bSolidGeometry = Spec.bSolidGeometry;
			Mapping.bSupportsProbes = Spec.bSupportsProbes;
		}
		return Volume->PhysicalMaterials.Num() == UE_ARRAY_COUNT(ProfileSpecs);
	}

	bool SetProbeHeight(AIMAcousticBakeVolume* Volume, FString& Error)
	{
		if (!Volume || !FMath::IsNearlyEqual(Volume->ProbeSpacingCm, 100.0f))
		{
			Error = TEXT("Rural probe spacing changed from the frozen 100 cm setting; refusing to guess.");
			return false;
		}
		if (Volume->ProbeHeightCm != 150.0f && Volume->ProbeHeightCm != 100.0f)
		{
			Error = TEXT("Rural probe height is neither the previously authored 150 cm nor the requested 100 cm value.");
			return false;
		}
		Volume->ProbeHeightCm = 100.0f;
		Volume->MarkPackageDirty();
		return true;
	}

	bool ValidateAuthoredMaterialState(UWorld* InWorld, AIMAcousticBakeVolume* Volume, FString& Error)
	{
		if (Volume->MaterialDrive != EIMAcousticMaterialDrive::PhysicalMaterial
			|| Volume->Materials.Num() != LegacyPreviewMaterialCount
			|| Volume->PhysicalMaterials.Num() != UE_ARRAY_COUNT(ProfileSpecs))
		{
			Error = TEXT("Rural BakeVolume did not retain PhysicalMaterial drive, 48 historical render-slot redirects, and the explicit profile mappings.");
			return false;
		}
		if (!CollectSlots(InWorld, Volume->BakeBounds->Bounds.GetBox(), true, SlotRowsAfter, Error))
			return false;
		return ValidateLandscapeCollisionInventory(InWorld, Volume->BakeBounds->Bounds.GetBox(), Error);
	}

	bool ValidateLandscapeCollisionInventory(UWorld* InWorld, const FBox& Region, FString& Error)
	{
		int32 Audited = 0;
		for (TActorIterator<AActor> It(InWorld); It; ++It)
		{
			TArray<ULandscapeComponent*> Components;
			It->GetComponents(Components);
			for (ULandscapeComponent* Component : Components)
			{
				ULandscapeHeightfieldCollisionComponent* Collision = Component->GetCollisionComponent();
				if (!IsValid(Collision))
				{
					Error = TEXT("A Landscape render component has no collision after physical material refresh: ") + Component->GetPathName();
					return false;
				}
				if (!Region.Intersect(Collision->Bounds.GetBox())) continue;
				if (Collision->CookedPhysicalMaterials.IsEmpty()
					|| Collision->CookedPhysicalMaterials.Contains(nullptr))
				{
					Error = TEXT("A bake-region Landscape collision has an empty/null actual CookedPhysicalMaterials table: ")
						+ Collision->GetPathName();
					return false;
				}
				const int32 Side = Collision->CollisionSizeQuads + 1;
				if (Side <= 1 || Side > 4097)
				{
					Error = TEXT("Landscape collision sample dimensions are invalid for bounded PM-ID inspection: ")
						+ Collision->GetPathName();
					return false;
				}
				TArray<uint8> Ids;
				int32 StoredIndexCount = 0;
				if (!IMReadLandscapeMaterialCells(*Collision, Ids, Error, &StoredIndexCount))
				{
					Error = TEXT("Physical collision face material read failed after Landscape recreation: ")
						+ Collision->GetPathName() + TEXT(" ") + Error;
					return false;
				}
				TMap<int32, int64> IdCounts;
				for (uint8 Id : Ids) ++IdCounts.FindOrAdd(int32(Id));
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("collision_component"), Collision->GetPathName());
				Row->SetStringField(TEXT("render_component"), Component->GetPathName());
				Row->SetBoolField(TEXT("intersects_bake_region"), true);
				Row->SetNumberField(TEXT("collision_size_quads"), Collision->CollisionSizeQuads);
				Row->SetNumberField(TEXT("sample_count"), Ids.Num());
				Row->SetNumberField(TEXT("stored_physics_material_index_count"), StoredIndexCount);
				Row->SetStringField(TEXT("material_index_api"), TEXT("Chaos::FHeightField::GetMaterialIndex(uint32 face); two faces per cell; actual collision cells only."));
				TArray<TSharedPtr<FJsonValue>> TableRows;
				for (int32 Index = 0; Index < Collision->CookedPhysicalMaterials.Num(); ++Index)
				{
					TSharedRef<FJsonObject> TableEntry = MakeShared<FJsonObject>();
					TableEntry->SetNumberField(TEXT("id"), Index);
					TableEntry->SetStringField(TEXT("physical_material"),
						GetPathNameSafe(Collision->CookedPhysicalMaterials[Index]));
					TableRows.Add(MakeShared<FJsonValueObject>(TableEntry));
				}
				Row->SetArrayField(TEXT("cooked_physical_materials"), TableRows);
				TArray<TSharedPtr<FJsonValue>> CountRows;
				UPhysicalMaterial* SoilPM = ProfileAssets.FindRef(EIMSurfaceProfile::Soil);
				if (!SoilPM) SoilPM = LoadExistingProfilePM(EIMSurfaceProfile::Soil);
				for (const TPair<int32, int64>& Pair : IdCounts)
				{
					TSharedRef<FJsonObject> CountEntry = MakeShared<FJsonObject>();
					CountEntry->SetNumberField(TEXT("id"), Pair.Key);
					CountEntry->SetNumberField(TEXT("count"), double(Pair.Value));
					if (Pair.Key == 255)
					{
						CountEntry->SetStringField(TEXT("kind"), TEXT("actual_collision_hole"));
						CountEntry->SetStringField(TEXT("physical_material"), TEXT("<hole, no acoustic triangles>"));
						CountEntry->SetBoolField(TEXT("table_index_valid"), false);
						CountRows.Add(MakeShared<FJsonValueObject>(CountEntry));
						continue;
					}
					const bool bIdValid = Pair.Key < Collision->CookedPhysicalMaterials.Num();
					CountEntry->SetBoolField(TEXT("table_index_valid"), bIdValid);
					if (bIdValid)
					{
						UPhysicalMaterial* UsedPM = Collision->CookedPhysicalMaterials[Pair.Key];
						CountEntry->SetStringField(TEXT("physical_material"), GetPathNameSafe(UsedPM));
						if (Pair.Value > 0 && UsedPM != SoilPM)
						{
							Error = TEXT("Landscape's used collision PM ID resolves outside the explicit Soil layer/default profile: ")
								+ Collision->GetPathName() + TEXT(" id=") + FString::FromInt(Pair.Key);
							return false;
						}
					}
					else
					{
						CountEntry->SetStringField(TEXT("physical_material"), TEXT("<no table entry>"));
						if (Pair.Value > 0)
						{
							Error = TEXT("A Landscape collision material ID is outside CookedPhysicalMaterials: ")
								+ Collision->GetPathName() + TEXT(" id=") + FString::FromInt(Pair.Key);
							return false;
						}
					}
					CountRows.Add(MakeShared<FJsonValueObject>(CountEntry));
				}
				Row->SetArrayField(TEXT("material_id_counts"), CountRows);
				LandscapeCollisionRows.Add(MakeShared<FJsonValueObject>(Row));
				++Audited;
			}
		}
		if (Audited == 0)
		{
			Error = TEXT("No Landscape collision components intersect the Rural 10 m bake region.");
			return false;
		}
		return true;
	}

	bool VerifySourcePackageHashes(FString& Error)
	{
		bSourcePackagesUnchangedVerified = false;
		FString Hash;
		int64 Bytes = 0;
		if (!TerrainReferenceHash.IsEmpty())
		{
			if (!HashFile(FPackageName::LongPackageNameToFilename(PristineTerrainMap,
					FPackageName::GetMapPackageExtension()), Hash, Bytes, Error)) return false;
			if (Hash != TerrainReferenceHash || Bytes != PristineTerrainBytes)
			{
				Error = TEXT("The read-only proven terrain reference changed during preview authoring.");
				return false;
			}
		}
		if (!HashFile(SourceMapFile, Hash, Bytes, Error)) return false;
		if (Hash != SourceMapHashBefore || Bytes != SourceMapBytes)
		{
			Error = TEXT("The original Rural source map package changed during target-map authoring.");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : SourcePackageRows)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			const FString PackageName = Row->GetStringField(TEXT("package"));
			FString CurrentHash;
			int64 CurrentBytes = 0;
			if (!HashFile(FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()),
					CurrentHash, CurrentBytes, Error))
				return false;
			if (CurrentHash != Row->GetStringField(TEXT("sha1_before"))
				|| double(CurrentBytes) != Row->GetNumberField(TEXT("bytes")))
			{
				Error = TEXT("An original Rural mesh/material package changed; only plugin copies may be authored: ") + PackageName;
				return false;
			}
		}
		bSourcePackagesUnchangedVerified = true;
		return true;
	}

	bool WriteReport()
	{
		IFileManager::Get().MakeDirectory(*EvidenceDirectory, true);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("IMAcousticPhysicalMaterialPreviewAuthoring.v1"));
		Root->SetBoolField(TEXT("terrain_only_operation"), bTerrainOnly);
		Root->SetStringField(TEXT("status"), Status);
		Root->SetStringField(TEXT("message"), ResultMessage);
		Root->SetStringField(TEXT("target_map"), RuralMap);
		Root->SetStringField(TEXT("original_source_map"), RuralSourceMap);
		Root->SetStringField(TEXT("source_map_sha1_before"), SourceMapHashBefore);
		Root->SetBoolField(TEXT("source_map_unchanged"), bSourcePackagesUnchangedVerified);
		Root->SetStringField(TEXT("source_package_verification"), bSourcePackagesUnchangedVerified ? TEXT("BYTE_HASHES_VERIFIED") : TEXT("NOT_VERIFIED"));
		Root->SetStringField(TEXT("evidence_scope"), TEXT("Persistent preparation and Bake only; listening/product acceptance is not asserted."));
		Root->SetStringField(TEXT("material_classification_policy"), TEXT("Explicit full-scene source object paths with profile values; historical 48-entry local compatibility mapping is checked separately. Actual component slots and LOD0 section indices are inventoried before authoring."));
		Root->SetNumberField(TEXT("explicit_material_rule_count"), int32(UE_ARRAY_COUNT(MaterialRules)));
		Root->SetNumberField(TEXT("legacy_preview_material_count"), LegacyPreviewMaterialCount);
		Root->SetBoolField(TEXT("component_slot_inventory_before_complete"), bBeforeSlotInventoryComplete);
		Root->SetBoolField(TEXT("component_slot_inventory_after_complete"), bAfterSlotInventoryComplete);
		Root->SetStringField(TEXT("classification_notes"), ClassificationNotes);
		Root->SetStringField(TEXT("construction_material_persistence"), ConstructionMaterialSummary);
		Root->SetStringField(TEXT("old_bake_asset_path"), OldBakeAssetPath);
		Root->SetStringField(TEXT("new_bake_asset_path"), NewBakeAssetPath);
		Root->SetBoolField(TEXT("started_from_validated_physical_material_candidate"),
			bStartedFromValidatedPhysicalCandidate);
		Root->SetBoolField(TEXT("repairing_exact_recorded_rejected_grass_candidate"), bRepairRecordedGrassCandidate);
		Root->SetBoolField(TEXT("refreshing_exact_recorded_cpu_only_grass_candidate"), bRefreshRecordedCPUOnlyGrassCandidate);
		Root->SetStringField(TEXT("terrain_pristine_reference"), PristineTerrainMap);
		Root->SetStringField(TEXT("terrain_pristine_reference_sha1"), TerrainReferenceHash);
		Root->SetStringField(TEXT("terrain_capture_summary"), TerrainCaptureSummary);
		Root->SetStringField(TEXT("author_terrain_restore_summary"), AuthorTerrainRestoreSummary);
		Root->SetStringField(TEXT("final_terrain_restore_summary"), FinalTerrainRestoreSummary);
		Root->SetBoolField(TEXT("author_exact_terrain_restored_save"), bAuthorTerrainRestoredSave);
		Root->SetBoolField(TEXT("final_exact_terrain_restored_save"), bFinalTerrainRestoredSave);
		if (GrassPreparationSummary) Root->SetObjectField(TEXT("grass_producer_preparation"), GrassPreparationSummary.ToSharedRef());
		if (GrassAuthorReloadSummary) Root->SetObjectField(TEXT("grass_author_reload"), GrassAuthorReloadSummary.ToSharedRef());
		if (GrassFinalReloadSummary) Root->SetObjectField(TEXT("grass_final_reload"), GrassFinalReloadSummary.ToSharedRef());
		Root->SetStringField(TEXT("new_bake_world_package"), NewBakeWorldPackage);
		Root->SetStringField(TEXT("new_bake_fingerprint"), NewBakeFingerprint);
		Root->SetNumberField(TEXT("probe_count"), ProbeCount);
		Root->SetNumberField(TEXT("generated_probe_count_after_final_scene_validation"), FinalRuntimeProbeCount);
		Root->SetNumberField(TEXT("reloaded_baked_probe_count"), ReloadedBakedProbeCount);
		Root->SetNumberField(TEXT("probe_spacing_cm"), 100.0);
		Root->SetNumberField(TEXT("probe_height_cm"), 100.0);
		Root->SetNumberField(TEXT("capture_ms"), CaptureMs);
		Root->SetNumberField(TEXT("generate_probes_ms"), GenerateProbesMs);
		Root->SetNumberField(TEXT("bake_submit_ms"), BakeSubmitMs);
		Root->SetNumberField(TEXT("bake_ready_wall_ms_including_submit_capture_and_asset_save"), BakeReadyElapsedMs);
		Root->SetNumberField(TEXT("author_save_ms"), AuthorSaveMs);
		Root->SetNumberField(TEXT("author_map_reload_ms"), AuthorReloadMs);
		Root->SetNumberField(TEXT("final_map_reload_ms_warm_process"), FinalReloadMs);
		Root->SetNumberField(TEXT("final_validate_ms"), FinalValidateMs);
		Root->SetNumberField(TEXT("author_post_load_frame_delta"), double(AuthorPostLoadFrameDelta));
		Root->SetNumberField(TEXT("final_post_load_frame_delta"), double(FinalPostLoadFrameDelta));
		Root->SetNumberField(TEXT("captured_vertices"), CapturedVertices);
		Root->SetNumberField(TEXT("captured_solid_triangles"), CapturedTriangles);
		Root->SetNumberField(TEXT("non_solid_triangles"), CapturedNonSolidTriangles);
		Root->SetNumberField(TEXT("non_solid_sections"), CapturedNonSolidSections);
		Root->SetNumberField(TEXT("bake_package_bytes"), double(BakePackageBytes));
		Root->SetNumberField(TEXT("actual_editor_validation_ticks"), EditorTicks);
		Root->SetNumberField(TEXT("editor_capture_total_ms"), EditorCaptureTotalMs);
		Root->SetNumberField(TEXT("editor_asset_validate_total_ms"), EditorAssetValidateTotalMs);
		Root->SetNumberField(TEXT("editor_max_tick_ms"), EditorMaxTickMs);
		Root->SetNumberField(TEXT("memory_samples"), MemorySamples);
		Root->SetNumberField(TEXT("baseline_process_used_physical_bytes"), double(BaselinePhysical));
		Root->SetNumberField(TEXT("baseline_process_used_virtual_bytes"), double(BaselineVirtual));
		Root->SetNumberField(TEXT("sampled_interval_peak_physical_bytes"), double(SampledPeakPhysical));
		Root->SetNumberField(TEXT("sampled_interval_peak_virtual_bytes"), double(SampledPeakVirtual));
		Root->SetNumberField(TEXT("os_process_lifetime_peak_physical_bytes"), double(ProcessLifetimePeakPhysical));
		Root->SetNumberField(TEXT("os_process_lifetime_peak_virtual_bytes"), double(ProcessLifetimePeakVirtual));
		Root->SetStringField(TEXT("memory_scope"), TEXT("Process-wide sampled interval and OS lifetime peaks; baseline is automation entry. Neither is a strict operation-only incremental peak."));
		Root->SetBoolField(TEXT("landscape_collision_recreated"), !LandscapeCollisionRows.IsEmpty());
		Root->SetBoolField(TEXT("landscape_cooked_tables_were_handwritten"), false);
		Root->SetBoolField(TEXT("author_map_saved"), bAuthorMapSaved);
		Root->SetBoolField(TEXT("final_map_saved"), bFinalMapSaved);
		Root->SetBoolField(TEXT("final_reload_validated"), bFinalReloadValidated);
		Root->SetStringField(TEXT("settings_before_summary"), FString::Printf(
			TEXT("Camera3=%.6f,%.6f,%.6f; BoundsExtent=%.6f,%.6f,%.6f; waterdrop_gain=%.6f; reverb_wet_gain=%.6f; old_probe_height=%.6f"),
			StateBefore.CameraLocation.X, StateBefore.CameraLocation.Y, StateBefore.CameraLocation.Z,
			StateBefore.BoundsExtent.X, StateBefore.BoundsExtent.Y, StateBefore.BoundsExtent.Z,
			StateBefore.SourceGain, StateBefore.ReverbWetGain, StateBefore.ProbeHeightCm));
		Root->SetObjectField(TEXT("settings_before"), FixedStateJson(StateBefore));
		Root->SetObjectField(TEXT("settings_after_authoring"), FixedStateJson(StateAuthored));
		Root->SetObjectField(TEXT("settings_after_authoring_reload"), FixedStateJson(StateReloaded));
		Root->SetArrayField(TEXT("profile_mappings"), ProfileRows);
		Root->SetArrayField(TEXT("explicit_material_path_table"), MaterialRuleRows);
		Root->SetArrayField(TEXT("component_slots_before"), SlotRowsBefore);
		Root->SetArrayField(TEXT("component_slots_after"), SlotRowsAfter);
		Root->SetStringField(TEXT("component_slot_scope"), TEXT("FULL_SCENE_SM_FAMILY_AND_LANDSCAPE_PM_BINDINGS; native static bake remains selected by Static mobility, the fixed 10m region and every intersecting instance. Movable binding does not register dynamic blockers."));
		Root->SetArrayField(TEXT("unused_explicit_material_rules_before"), BeforeUnusedRules);
		Root->SetArrayField(TEXT("unused_explicit_material_rules_after"), AfterUnusedRules);
		Root->SetNumberField(TEXT("initial_load_ms"), InitialLoadMs);
		Root->SetNumberField(TEXT("initial_post_load_frame_delta"), double(InitialPostLoadFrameDelta));
		Root->SetArrayField(TEXT("component_body_pm_overrides"), BodyOverrideRows);
		Root->SetArrayField(TEXT("landscape_defaults"), LandscapeRows);
		Root->SetArrayField(TEXT("landscape_layer_copy_map"), LandscapeLayerRows);
		Root->SetArrayField(TEXT("landscape_collision_cooked_tables_and_ids"), LandscapeCollisionRows);
		Root->SetArrayField(TEXT("original_source_package_hashes"), SourcePackageRows);

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root, Writer)) return false;
		return FFileHelper::SaveStringToFile(JsonText, *EvidenceFile,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

private:
	FAutomationTestBase* Test = nullptr;
	double StartedAt = 0.0;
	double CaptureMs=0, GenerateProbesMs=0, BakeSubmitMs=0, BakeReadyElapsedMs=0, BakeStartedAt=0;
	double InitialLoadMs=0, InitialReadyStartedAt=0;
	uint64 InitialLoadFrame=0, InitialPostLoadFrameDelta=0;
	double AuthorSaveMs=0, AuthorReloadMs=0, FinalReloadMs=0, FinalValidateMs=0, EditorObservationStartedAt=0;
	double AuthorReloadReadyStartedAt=0, FinalReloadReadyStartedAt=0;
	uint64 AuthorReloadFrame=0, FinalReloadFrame=0, AuthorPostLoadFrameDelta=0, FinalPostLoadFrameDelta=0;
	double EditorCaptureTotalMs=0, EditorAssetValidateTotalMs=0, EditorMaxTickMs=0;
	int32 CapturedVertices=0, CapturedTriangles=0, CapturedNonSolidTriangles=0, CapturedNonSolidSections=0, EditorTicks=0;
	int64 BakePackageBytes=-1;
	uint64 BaselinePhysical=0, BaselineVirtual=0, SampledPeakPhysical=0, SampledPeakVirtual=0;
	uint64 ProcessLifetimePeakPhysical=0, ProcessLifetimePeakVirtual=0;
	int32 MemorySamples=0;
	int32 Stage = 0;
	int32 LastLoggedStage = INDEX_NONE;
	bool bTerrainOnly = false;
	bool bFinished = false;
	bool bAuthorMapSaved = false;
	bool bFinalMapSaved = false;
	bool bFinalReloadValidated = false;
	bool bAuthorReloadPerformed = false;
	bool bInitialMapLoaded = false;
	bool bFinalReloadPerformed = false;
	bool bSourcePackagesUnchangedVerified = false;
	bool bBeforeSlotInventoryComplete = false;
	bool bAfterSlotInventoryComplete = false;
	bool bRepairRecordedGrassCandidate = false;
	bool bRefreshRecordedCPUOnlyGrassCandidate = false;
	bool bStartedFromValidatedPhysicalCandidate = false;
	bool bAuthorTerrainRestoredSave = false;
	bool bFinalTerrainRestoredSave = false;
	uint64 TerrainFirstSaveFrame = 0;
	FIMAcousticLandscapeFinalSnapshot TerrainSnapshot;
	FString TerrainReferenceHash, TerrainCaptureSummary, AuthorTerrainRestoreSummary, FinalTerrainRestoreSummary;
	TMap<UMaterialInterface*, UMaterialInterface*> ProducerMaterialRedirects;
	TSharedPtr<FJsonObject> GrassPreparationSummary, GrassAuthorReloadSummary, GrassFinalReloadSummary;
	FString Status = TEXT("STARTED");
	FString ResultMessage;
	FString ConstructionMaterialSummary;
	FString EvidenceDirectory;
	FString EvidenceFile;
	FString TargetMapFile;
	FString SourceMapFile;
	FString SourceMapHashBefore;
	FString OldBakeAssetPath;
	FString NewBakeAssetPath;
	FString NewBakeWorldPackage;
	FString NewBakeFingerprint;
	int64 SourceMapBytes = 0;
	int32 ProbeCount = 0;
	int32 FinalRuntimeProbeCount = INDEX_NONE;
	int32 ReloadedBakedProbeCount = INDEX_NONE;
	TWeakObjectPtr<AIMAcousticBakeVolume> VolumeWeak;
	FIMFixedSceneState StateBefore;
	FIMFixedSceneState StateAuthored;
	FIMFixedSceneState StateReloaded;
	TMap<EIMSurfaceProfile, UPhysicalMaterial*> ProfileAssets;
	TMap<FString, UMaterialInstanceConstant*> MaterialCopies;
	TSet<FString> HashedPackages;
	TArray<TSharedPtr<FJsonValue>> ProfileRows;
	TArray<TSharedPtr<FJsonValue>> MaterialRuleRows;
	TArray<TSharedPtr<FJsonValue>> SlotRowsBefore;
	TArray<TSharedPtr<FJsonValue>> SlotRowsAfter;
	TArray<TSharedPtr<FJsonValue>> BeforeUnusedRules;
	TArray<TSharedPtr<FJsonValue>> AfterUnusedRules;
	TArray<TSharedPtr<FJsonValue>> BodyOverrideRows;
	TArray<TSharedPtr<FJsonValue>> LandscapeRows;
	TArray<TSharedPtr<FJsonValue>> LandscapeLayerRows;
	TArray<TSharedPtr<FJsonValue>> LandscapeCollisionRows;
	TArray<TSharedPtr<FJsonValue>> SourcePackageRows;
};

// Inspect the persisted producer before material authoring: generated components
// alone omit varieties that were not spawned at the current camera position.
class FIMGrassSourceInventoryCommand final : public IAutomationLatentCommand
{
public:
	explicit FIMGrassSourceInventoryCommand(FAutomationTestBase* InTest)
		: Test(InTest), StartedAt(FPlatformTime::Seconds()) {}

	virtual bool Update() override
	{
		if (!IsInGameThread() || !GEditor) return Finish(false, TEXT("Grass inventory requires the owned Editor game thread."));
		if (FPlatformTime::Seconds() - StartedAt > 120.0) return Finish(false, TEXT("Grass source inventory timed out."));
		if (!bLoaded)
		{
			if (!FEditorFileUtils::LoadMap(RuralMap, false, true))
				return Finish(false, TEXT("The owned Rural preview could not be loaded for Grass inventory."));
			LoadFrame = GFrameCounter;
			bLoaded = true;
			return false;
		}
		if (GFrameCounter <= LoadFrame) return false;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World || World->GetOutermost()->GetName() != RuralMap)
			return Finish(false, TEXT("Grass inventory world does not match the owned Rural preview."));
		const bool bDirtyBefore = World->GetOutermost()->IsDirty();
		FString Error;
		const bool bInspected = IMInspectLandscapeGrassMaterialSources(World, Summary, Error);
		const bool bDirtyAfter = World->GetOutermost()->IsDirty();
		if (!Summary) Summary = MakeShared<FJsonObject>();
		Summary->SetBoolField(TEXT("map_dirty_before_inspect"), bDirtyBefore);
		Summary->SetBoolField(TEXT("map_dirty_after_inspect"), bDirtyAfter);
		Summary->SetBoolField(TEXT("maps_or_assets_saved"), false);
		Summary->SetNumberField(TEXT("post_load_frame_delta"), double(GFrameCounter - LoadFrame));
		if (bDirtyBefore != bDirtyAfter) return Finish(false, TEXT("The read-only Grass inspection changed the map dirty state."));
		return Finish(bInspected, Error);
	}

private:
	bool Finish(bool bSuccess, const FString& Error)
	{
		if (!Summary) Summary = MakeShared<FJsonObject>();
		const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AcousticV2/PhysicalMaterialDrive"));
		const FString Filename = FPaths::Combine(Directory, FString::Printf(TEXT("grass_source_inventory_%s.json"), *RunId));
		Summary->SetStringField(TEXT("run_id"), RunId);
		Summary->SetStringField(TEXT("automation_status"), bSuccess ? TEXT("PASS_READ_ONLY_INVENTORY") : TEXT("FAIL_CLOSED"));
		Summary->SetStringField(TEXT("automation_error"), Error);
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		const bool bSaved = IFileManager::Get().MakeDirectory(*Directory, true)
			&& FJsonSerializer::Serialize(Summary.ToSharedRef(), Writer)
			&& FFileHelper::SaveStringToFile(Json, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		bSuccess &= bSaved;
		if (bSuccess) Test->AddInfo(FString::Printf(TEXT("Read-only Grass producer inventory: %s"), *Filename));
		else Test->AddError(bSaved ? Error : TEXT("The required Grass inventory evidence could not be written."));
		UE_LOG(LogTemp, Display, TEXT("[IM][PIE_TEST] %s GrassSourceInventory report=%s"), bSuccess ? TEXT("PASS") : TEXT("FAIL"), *Filename);
		UE_LOG(LogTemp, Display, TEXT("IMExitEditor %s GrassSourceInventory report=%s"), bSuccess ? TEXT("PASS") : TEXT("FAIL"), *Filename);
		return true;
	}

	FAutomationTestBase* Test = nullptr;
	double StartedAt = 0.0;
	uint64 LoadFrame = 0;
	bool bLoaded = false;
	TSharedPtr<FJsonObject> Summary;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticLandscapeGrassSourceInventory,
	"IceMoon.AcousticField.Rural.LandscapeGrassSourceInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticLandscapeGrassSourceInventory::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(
		IMAcousticPhysicalMaterialPreviewTestPrivate::FIMGrassSourceInventoryCommand(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPhysicalMaterialPreviewPrepare,
	"IceMoon.AcousticField.Rural.PhysicalMaterialPreviewPrepare",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticPhysicalMaterialPreviewPrepare::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(
		IMAcousticPhysicalMaterialPreviewTestPrivate::FIMPhysicalMaterialPreviewPrepareCommand(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIMAcousticPhysicalMaterialTerrainRestore,
	"IceMoon.AcousticField.Rural.PhysicalMaterialTerrainRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIMAcousticPhysicalMaterialTerrainRestore::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(
		IMAcousticPhysicalMaterialPreviewTestPrivate::FIMPhysicalMaterialPreviewPrepareCommand(this, true));
	return true;
}

#endif
