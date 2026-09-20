// P0 test scene builder: assembles the frozen acoustic whitebox map from engine BasicShapes cubes.
// No binary assets: all geometry is spawned in code. Owner: IceMoonAcousticField (test tooling).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IMAcousticTestSceneBuilder.generated.h"

// Material zone per whitebox; maps to physical-material slots assigned by the user.
UENUM(BlueprintType)
enum class EIM_WhiteboxZone : uint8
{
	Concrete UMETA(DisplayName = "Concrete"),
	Wood UMETA(DisplayName = "Wood"),
	Carpet UMETA(DisplayName = "Carpet")
};

/**
 * Builds the frozen P0 acoustic test scene (A 10x6x3 + corridor 6x2x3 + B 8x6x3 + C wing,
 * maze annex, coupled absorb/reflect pair, upper floor with stair void, open yard) from
 * /Engine/BasicShapes/Cube. All spawned actors are Static mobility
 * (v1 probes only hit Static/Stationary) and tagged for one-click cleanup.
 * Assumption (to verify in manual test): BasicShapes Cube is a 1m box centered at origin.
 */
UCLASS(Blueprintable, BlueprintType)
class ICEMOONACOUSTICFIELD_API AIM_AcousticTestSceneBuilder : public AActor
{
	GENERATED_BODY()

public:
	AIM_AcousticTestSceneBuilder();

	// Whitebox cube mesh (defaults to engine BasicShapes Cube).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Scene")
	TObjectPtr<class UStaticMesh> WhiteboxCube;

	// Physical materials per zone; null entries are skipped (mesh default applies).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Scene")
	TObjectPtr<class UPhysicalMaterial> PM_Concrete;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Scene")
	TObjectPtr<class UPhysicalMaterial> PM_Wood;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Scene")
	TObjectPtr<class UPhysicalMaterial> PM_Carpet;

	// Destroys previously built whiteboxes (tag match) and rebuilds the frozen scene.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Scene")
	void BuildTestScene();

	// Destroys previously built whiteboxes without rebuilding.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Scene")
	void ClearTestScene();

private:
#if WITH_EDITOR
	void AddBox(const FVector& CenterM, const FVector& SizeM, EIM_WhiteboxZone Zone);
	void AddWallX(float X, float Y0, float Y1, float Z0, float Z1, const TArray<FVector2D>& Gaps, EIM_WhiteboxZone Zone);
	void AddWallY(float Y, float X0, float X1, float Z0, float Z1, const TArray<FVector2D>& Gaps, EIM_WhiteboxZone Zone);
	UPhysicalMaterial* ResolvePM(EIM_WhiteboxZone Zone) const;
#endif
};
