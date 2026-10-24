#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "IAudioExtensionPlugin.h"
#include "IMAcousticSourceComponent.generated.h"

class UAudioComponent;
class FPrimitiveSceneProxy;
#if WITH_EDITORONLY_DATA
class UBillboardComponent;
class UStaticMeshComponent;
#endif

// A positive routing marker. Selecting the device plugin alone must not silently
// enroll every sound in the world into baked acoustic processing.
UCLASS(EditInlineNew)
class ICEMOONACOUSTICFIELD_API UIMAcousticSpatializationSettings final : public USpatializationPluginSourceSettingsBase
{
	GENERATED_BODY()
};

// Editor locator rendered as ordinary foreground lines. This intentionally
// does not use BillboardSprites, TextRender, collision, or a mesh material, so
// the marker remains visible when viewport overlay show flags are disabled.
UCLASS()
class ICEMOONACOUSTICFIELD_API UIMAcousticSourceMarkerComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UIMAcousticSourceMarkerComponent(const FObjectInitializer& ObjectInitializer);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
};

UCLASS(ClassGroup=(Audio), meta=(BlueprintSpawnableComponent))
class ICEMOONACOUSTICFIELD_API UIMAcousticSourceComponent final : public UActorComponent
{
	GENERATED_BODY()
public:
	// Explicit reference avoids choosing an arbitrary component on multi-voice actors.
	UPROPERTY(EditInstanceOnly, Category="IM|Acoustics")
	TObjectPtr<UAudioComponent> AudioComponent;

	// GT only. Validation never changes playback or the user's attenuation asset.
	bool ValidateSource(FString& Failure) const;

	// GT-only runtime state, not serialized: IMAcousticPresenceFlags bits that the
	// bake volume publishes to this source's audio slot. 0 is the physical render.
	uint8 PresenceFlags = 0;
	// GT-only: signed azimuth (radians, right positive) of this source's own lateral offset from its
	// owner's body axis, as seen from the listener. Only exaggerated presence reads it.
	float PresenceLateralAzimuthRadians = 0.f;

protected:
#if WITH_EDITORONLY_DATA
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	void CreateEditorBillboard();

	// Transient visualization only. The source marker remains the sole runtime owner.
	UPROPERTY(Transient)
	TObjectPtr<UBillboardComponent> EditorBillboard;

	// A regular editor primitive is the fail-safe overlay marker. It is paired
	// with the small editor-only mesh icon used by raw viewport captures.
	UPROPERTY(Transient)
	TObjectPtr<UIMAcousticSourceMarkerComponent> EditorMarker;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> EditorMarkerMesh;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> EditorMarkerWaveMeshes;

#endif
};
