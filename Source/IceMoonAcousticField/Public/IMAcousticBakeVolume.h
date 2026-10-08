#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IMAcousticBakeVolume.generated.h"

class UBoxComponent;
class UMaterialInterface;
class UStaticMeshComponent;
class UIMAcousticBakeAsset;
class USoundSubmix;
class UIMAcousticReverbPreset;
struct FIMAcousticFieldRuntime;

USTRUCT()
struct FIMAcousticMaterialMapping
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category="Material")
	TObjectPtr<UMaterialInterface> Material;
	// SDK three-band energy absorption, ordered low/mid/high, each in [0,1].
	UPROPERTY(EditAnywhere, Category="Material")
	FVector Absorption = FVector(0.1, 0.1, 0.1);
	UPROPERTY(EditAnywhere, Category="Material", meta=(ClampMin="0", ClampMax="1"))
	float Scattering = 0.5f;
};

// A single explicit bake region and runtime owner. This actor inspects existing
// geometry and never creates, deletes, or rearranges level actors.
UCLASS()
class ICEMOONACOUSTICFIELD_API AIMAcousticBakeVolume final : public AActor
{
	GENERATED_BODY()
public:
	AIMAcousticBakeVolume();
	~AIMAcousticBakeVolume() override;
	UPROPERTY(VisibleAnywhere, Category="Acoustics")
	TObjectPtr<UBoxComponent> BakeBounds;
	UPROPERTY(EditAnywhere, Category="Acoustics")
	TArray<FIMAcousticMaterialMapping> Materials;
	UPROPERTY(EditAnywhere, Category="Acoustics", meta=(ClampMin="25", Units="cm"))
	float ProbeSpacingCm = 100.0f;
	UPROPERTY(EditAnywhere, Category="Acoustics", meta=(ClampMin="25", Units="cm"))
	float ProbeHeightCm = 150.0f;
	// H1 W2: dynamic rigid bodies (doors/boxes) synced into instanced meshes
	// on every snapshot tick. Validity is checked per tick; destroyed or
	// invalid entries are omitted, and the worker then removes them from the
	// scene. Omission degrades to unoccluded audio (pre-H1 behavior), never
	// to stale geometry: removal is explicit in SyncDynamicMeshes.
	UPROPERTY(EditInstanceOnly, Category="Acoustics")
	TArray<TObjectPtr<UStaticMeshComponent>> DynamicBlockers;
	UPROPERTY(EditInstanceOnly, Category="Acoustics")
	TObjectPtr<UIMAcousticBakeAsset> BakedField;
	UPROPERTY(VisibleAnywhere, Category="Acoustics")
	FString Status;
	UPROPERTY(VisibleAnywhere, Category="Acoustics")
	TArray<FString> SceneIssues;
	UPROPERTY(VisibleAnywhere, Category="Acoustics")
	int32 ExportedTriangles = 0;
	UPROPERTY(VisibleAnywhere, Category="Acoustics")
	int32 GeneratedProbes = 0;
	UPROPERTY(EditAnywhere,Category="Acoustics",meta=(ClampMin="0",ClampMax="1"))
	float ReverbWetGain=.25f;
	UPROPERTY(EditAnywhere,Category="Acoustics|Audition")
	bool bEnableV2=true;
	UPROPERTY(EditAnywhere,Category="Acoustics|Audition")
	bool bDirectRoute=true;
	UPROPERTY(EditAnywhere,Category="Acoustics|Audition")
	bool bPathRoute=true;
	UPROPERTY(EditAnywhere,Category="Acoustics|Audition")
	bool bReverbRoute=true;
	UPROPERTY(VisibleInstanceOnly,Transient,Category="Acoustics")
	TObjectPtr<USoundSubmix> ReverbSubmix;

	UFUNCTION(CallInEditor, Category="Acoustics")
	void InspectScene();
	UFUNCTION(CallInEditor, Category="Acoustics")
	void GenerateProbes();
	UFUNCTION(CallInEditor, Category="Acoustics")
	void Bake();
	UFUNCTION(CallInEditor, Category="Acoustics")
	void CancelBake();
	UFUNCTION(CallInEditor, Category="Acoustics")
	void ShowProbeCoverage();
	// Persistent editor overlay. While this actor is selected (or the flag below
	// is on) the viewport draws the requested bake region, the probes that
	// actually exist, their combined extent and a status label, so "which areas
	// are baked / not baked" is answerable at a glance. Editor-only drawing;
	// no runtime effect, nothing is saved by the flag (Transient).
	UPROPERTY(EditAnywhere, Transient, Category="Acoustics|Debug")
	bool bShowBakedFieldOverlay=false;
	// Live overlay readout (probe count, spacing, bake state), refreshed only
	// while the overlay draws. Kept separate from Status so bake errors are not
	// overwritten by viewport bookkeeping.
	UPROPERTY(VisibleAnywhere, Transient, Category="Acoustics|Debug")
	FString OverlaySummary;
	bool ValidateCurrentBake(FString& Failure);
	// H1 W3 negative control only (test-only): request a pathing-validation
	// flip on the running worker; poll IM_GetAppliedPathingValidationForTest
	// until it matches. Returns false when no worker is running.
	bool SetPathingValidationForTest(bool bOn);
	int ReadAppliedPathingValidationForTest() const;
	bool SetApertureTransitForTest(bool bOn, const FVector& CenterUE,
		const FVector& HalfExtentUE);

	void Tick(float DeltaSeconds) override;
	void PostRegisterAllComponents() override;
	bool ShouldTickIfViewportsOnly() const override { return true; }
protected:
	void BeginPlay() override;
	void EndPlay(const EEndPlayReason::Type Reason) override;
	void BeginDestroy() override;
private:
	UPROPERTY(Transient)
	TObjectPtr<UIMAcousticReverbPreset> ReverbPreset;
	TUniquePtr<FIMAcousticFieldRuntime> Runtime;
	// Overlay probe cache: refilled only when the bound bake asset changes, so
	// per-frame drawing never re-parses the asset metadata.
	TWeakObjectPtr<UIMAcousticBakeAsset> OverlayAsset;
	TArray<FVector4> OverlayAssetProbes;
	FVector OverlayAssetOrigin=FVector::ZeroVector;
	bool CaptureScene();
	void PollBakeCompletion();
	void ShutdownOwnedWork();
	void DrawEditorFieldOverlay();
};
