#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IMAcousticBakeVolume.generated.h"

class UBoxComponent;
class UMaterialInterface;
class UPhysicalMaterial;
class UStaticMeshComponent;
class UIMAcousticBakeAsset;
class USoundSubmix;
class UIMAcousticReverbPreset;
struct FIMAcousticFieldRuntime;
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
class FIMAcousticOperationMemorySampler;
#endif

UENUM()
enum class EIMAcousticMaterialDrive : uint8
{
	PhysicalMaterial,
	LegacyRenderMaterial
};

USTRUCT()
struct FIMAcousticPhysicalMaterialMapping
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category="IM|Material")
	TObjectPtr<UPhysicalMaterial> PhysicalMaterial;
	// Steam Audio's band centers are 400 Hz, 2.5 kHz and 15 kHz (phonon.h).
	// Soil presets are adjustable estimates, not measurements inferred from color.
	UPROPERTY(EditAnywhere, Category="IM|Material", meta=(ClampMin="0", ClampMax="1", ToolTip="Energy absorption in [0,1]: X=400 Hz, Y=2.5 kHz, Z=15 kHz. Larger values absorb more reflected energy; rebake after editing."))
	FVector Absorption = FVector(0.1, 0.1, 0.1);
	UPROPERTY(EditAnywhere, Category="IM|Material", meta=(ClampMin="0", ClampMax="1"))
	float Scattering = 0.5f;
	// NonSolid vegetation is an explicit first-version approximation: its leaf
	// cards neither block nor reflect; trunks retain their solid wood sections.
	// Every placement still contributes to the scene identity and audit counts.
	UPROPERTY(EditAnywhere, Category="IM|Material")
	bool bSolidGeometry = true;
	UPROPERTY(EditAnywhere, Category="IM|Material", meta=(ToolTip="Only solid surfaces may support probes. Disable this for non-solid leaf and grass cards."))
	bool bSupportsProbes = true;
};

USTRUCT()
struct FIMAcousticMaterialMapping
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category="IM|Material")
	TObjectPtr<UMaterialInterface> Material;
	// SDK three-band energy absorption, ordered low/mid/high, each in [0,1].
	UPROPERTY(EditAnywhere, Category="IM|Material")
	FVector Absorption = FVector(0.1, 0.1, 0.1);
	UPROPERTY(EditAnywhere, Category="IM|Material", meta=(ClampMin="0", ClampMax="1"))
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
	UPROPERTY(VisibleAnywhere, Category="IM|Acoustics")
	TObjectPtr<UBoxComponent> BakeBounds;
	UPROPERTY(EditAnywhere, Category="IM|Acoustics|Materials")
	EIMAcousticMaterialDrive MaterialDrive = EIMAcousticMaterialDrive::PhysicalMaterial;
	UPROPERTY(EditAnywhere, Category="IM|Acoustics|Materials", meta=(EditCondition="MaterialDrive == EIMAcousticMaterialDrive::PhysicalMaterial"))
	TArray<FIMAcousticPhysicalMaterialMapping> PhysicalMaterials;
	// Explicit compatibility mode for historical fixtures and authored volumes.
	UPROPERTY(EditAnywhere, Category="IM|Acoustics")
	TArray<FIMAcousticMaterialMapping> Materials;
	UPROPERTY(VisibleAnywhere, Category="IM|Acoustics|Materials")
	int32 NonSolidTriangles = 0;
	UPROPERTY(VisibleAnywhere, Category="IM|Acoustics|Materials")
	int32 NonSolidSections = 0;
	UPROPERTY(EditAnywhere, Category="IM|Acoustics", meta=(ClampMin="25", Units="cm"))
	float ProbeSpacingCm = 100.0f;
	UPROPERTY(EditAnywhere, Category="IM|Acoustics", meta=(ClampMin="25", Units="cm"))
	float ProbeHeightCm = 150.0f;
	// H1 W2: dynamic rigid bodies (doors/boxes) synced into instanced meshes
	// on every snapshot tick. Validity is checked per tick; destroyed or
	// invalid entries are omitted, and the worker then removes them from the
	// scene. Omission degrades to unoccluded audio (pre-H1 behavior), never
	// to stale geometry: removal is explicit in SyncDynamicMeshes.
	UPROPERTY(EditInstanceOnly, Category="IM|Acoustics")
	TArray<TObjectPtr<UStaticMeshComponent>> DynamicBlockers;
	UPROPERTY(EditInstanceOnly, Category="IM|Acoustics")
	TObjectPtr<UIMAcousticBakeAsset> BakedField;
	UPROPERTY(VisibleAnywhere, Category="IM|Acoustics")
	FString Status;
	UPROPERTY(VisibleAnywhere, Category="IM|Acoustics")
	TArray<FString> SceneIssues;
	UPROPERTY(VisibleAnywhere, Category="IM|Acoustics")
	int32 ExportedTriangles = 0;
	UPROPERTY(VisibleAnywhere, Category="IM|Acoustics")
	int32 GeneratedProbes = 0;
	UPROPERTY(EditAnywhere,Category="IM|Acoustics",meta=(ClampMin="0",ClampMax="1"))
	float ReverbWetGain=.25f;
	UPROPERTY(EditAnywhere,Category="IM|Acoustics|Audition")
	bool bEnableV2=true;
	UPROPERTY(EditAnywhere,Category="IM|Acoustics|Audition")
	bool bDirectRoute=true;
	UPROPERTY(EditAnywhere,Category="IM|Acoustics|Audition")
	bool bPathRoute=true;
	UPROPERTY(EditAnywhere,Category="IM|Acoustics|Audition")
	bool bReverbRoute=true;
	UPROPERTY(VisibleInstanceOnly,Transient,Category="IM|Acoustics")
	TObjectPtr<USoundSubmix> ReverbSubmix;

	UFUNCTION(CallInEditor, Category="IM|Acoustics")
	void InspectScene();
	UFUNCTION(CallInEditor, Category="IM|Acoustics")
	void GenerateProbes();
	UFUNCTION(CallInEditor, Category="IM|Acoustics")
	void Bake();
	UFUNCTION(CallInEditor, Category="IM|Acoustics")
	void CancelBake();
	UFUNCTION(CallInEditor, Category="IM|Acoustics")
	void ShowProbeCoverage();
	// Persistent editor overlay. While this actor is selected (or the flag below
	// is on) the viewport draws the requested bake region, the probes that
	// actually exist, their combined extent and a status label, so "which areas
	// are baked / not baked" is answerable at a glance. Editor-only drawing;
	// no runtime effect, nothing is saved by the flag (Transient).
	UPROPERTY(EditAnywhere, Transient, Category="IM|Acoustics|Debug")
	bool bShowBakedFieldOverlay=false;
	// Live overlay readout (probe count, spacing, bake state), refreshed only
	// while the overlay draws. Kept separate from Status so bake errors are not
	// overwritten by viewport bookkeeping.
	UPROPERTY(VisibleAnywhere, Transient, Category="IM|Acoustics|Debug")
	FString OverlaySummary;
	bool ValidateCurrentBake(FString& Failure);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	// Test-only readback of the exact scene submitted to the bake path.
	bool IM_CaptureSceneGeometryForTest(TArray<FVector>& OutVertices,
		TArray<FIntVector>& OutTriangles, TArray<int32>& OutMaterialIndices,
		FString& OutFingerprint);
	bool IM_CaptureDynamicGeometryForTest(UStaticMeshComponent* Component,
		int32& OutVertexCount, int32& OutTriangleCount, FString& Failure);
	void IM_GetGeneratedProbeCentersForTest(TArray<FVector>& OutSDKCenters) const;
	// GT-only evidence cleanup: cancel/join the owned bake and detach its
	// publication ticker before restoring an earlier test binding.
	bool IM_CancelAndDrainBakeForTest();
	// Measures the actual one-second Editor ticker body for automation only.
	void IM_SetEditorValidationProfilingForTest(bool bEnable,
		FIMAcousticOperationMemorySampler* MemorySampler = nullptr);
	bool IM_GetEditorValidationProfileForTest(int32& OutTicks, double& OutCaptureMs,
		double& OutAssetValidationMs, double& OutMaxTickMs) const;
	UIMAcousticReverbPreset* IM_GetEnvironmentPresetForTest() const { return ReverbPreset.Get(); }
#endif
	// H1 W3 negative control only (test-only): request a pathing-validation
	// flip on the running worker; poll IM_GetAppliedPathingValidationForTest
	// until it matches. Returns false when no worker is running.
	bool SetPathingValidationForTest(bool bOn);
	int GetAppliedPathingValidationForTest() const;
	bool SetApertureTransitForTest(bool bOn, const FVector& CenterUE,
		const FVector& HalfExtentUE);

	// GT only. Stores the lateral azimuth of a footstep into the audio-thread slot of this audio component at the
	// trigger moment; the per-frame Tick copy would otherwise reach a short sample after its onset (previous foot's value).
	void PublishPresenceLateralNow(uint64 AudioComponentId, float LateralAzimuthRadians);
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
