#include "IMAcousticSourceComponent.h"
#include "IMAcousticMetaSound.h"
#include "Components/AudioComponent.h"
#if WITH_EDITORONLY_DATA
#include "Components/BillboardComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/SoftObjectPath.h"
#endif
#include "MetasoundSource.h"
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "Sound/SoundWave.h"
#include "UObject/ICookInfo.h"

namespace IMAcousticSourceComponentPrivate
{
class FIMAcousticSourceMarkerSceneProxy final : public FPrimitiveSceneProxy
{
public:
	explicit FIMAcousticSourceMarkerSceneProxy(const UIMAcousticSourceMarkerComponent* Component)
		: FPrimitiveSceneProxy(Component)
	{
		bWillEverBeLit = false;
	}

	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		const FMatrix& WorldTransform = GetLocalToWorld();
		const FLinearColor DrawColor = IsSelected()
			? FLinearColor(1.0f, 1.0f, 0.1f, 1.0f)
			: FLinearColor(1.0f, 0.9f, 0.05f, 1.0f);

		auto WorldPoint = [&WorldTransform](float Y, float Z)
		{
			return WorldTransform.TransformPosition(FVector(0.0f, Y, Z));
		};

		auto DrawLine = [&](FPrimitiveDrawInterface* PDI, const FVector& A, const FVector& B)
		{
			PDI->DrawLine(A, B, DrawColor, SDPG_Foreground, 10.0f, 0.0f, false);
		};

		auto DrawRectangle = [&](FPrimitiveDrawInterface* PDI, float MinY, float MaxY, float MinZ, float MaxZ)
		{
			const FVector A = WorldPoint(MinY, MinZ);
			const FVector B = WorldPoint(MaxY, MinZ);
			const FVector C = WorldPoint(MaxY, MaxZ);
			const FVector D = WorldPoint(MinY, MaxZ);
			DrawLine(PDI, A, B);
			DrawLine(PDI, B, C);
			DrawLine(PDI, C, D);
			DrawLine(PDI, D, A);
		};

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1u << ViewIndex)) == 0)
			{
				continue;
			}

			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);

			// Outer sign frame.
			DrawRectangle(PDI, -110.0f, 110.0f, -70.0f, 70.0f);

			// Speaker cabinet and cone.
			DrawRectangle(PDI, -72.0f, -28.0f, -42.0f, 42.0f);
			DrawLine(PDI, WorldPoint(-28.0f, -42.0f), WorldPoint(12.0f, -58.0f));
			DrawLine(PDI, WorldPoint(-28.0f, 42.0f), WorldPoint(12.0f, 58.0f));
			DrawLine(PDI, WorldPoint(12.0f, -58.0f), WorldPoint(12.0f, 58.0f));

			// Two sound-wave strokes make the marker identifiable as an audio
			// source without relying on a texture, font, or overlay show flag.
			DrawLine(PDI, WorldPoint(32.0f, -38.0f), WorldPoint(54.0f, -24.0f));
			DrawLine(PDI, WorldPoint(32.0f, 38.0f), WorldPoint(54.0f, 24.0f));
			DrawLine(PDI, WorldPoint(58.0f, -58.0f), WorldPoint(88.0f, -34.0f));
			DrawLine(PDI, WorldPoint(58.0f, 58.0f), WorldPoint(88.0f, 34.0f));
		}
	}

	FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bDynamicRelevance = true;
		Result.bEditorPrimitiveRelevance = true;
		Result.bEditorNoDepthTestPrimitiveRelevance = true;
		return Result;
	}

	bool CanBeOccluded() const override
	{
		return false;
	}

	uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}

	uint32 GetAllocatedSize() const
	{
		return FPrimitiveSceneProxy::GetAllocatedSize();
	}
};
}

UIMAcousticSourceMarkerComponent::UIMAcousticSourceMarkerComponent(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bUseEditorCompositing = true;
	bIsEditorOnly = true;
	SetVisibility(true);
	SetGenerateOverlapEvents(false);
}

FPrimitiveSceneProxy* UIMAcousticSourceMarkerComponent::CreateSceneProxy()
{
	return new IMAcousticSourceComponentPrivate::FIMAcousticSourceMarkerSceneProxy(this);
}

FBoxSphereBounds UIMAcousticSourceMarkerComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return FBoxSphereBounds(
		FBox(FVector(-4.0f, -76.0f, -46.0f), FVector(4.0f, 76.0f, 46.0f)))
		.TransformBy(LocalToWorld);
}

namespace IMAcousticSourceComponentPrivate
{
	// Both pipelines own these acoustic effects.
	// Spatialization is checked separately because the pipeline requirements differ.
	bool HasCompetingNativeProcessing(const FSoundAttenuationSettings& Settings)
	{
		return Settings.bAttenuate
			|| Settings.bAttenuateWithLPF
			|| Settings.bEnableOcclusion
			|| Settings.bEnableListenerFocus
			|| Settings.bEnableReverbSend;
	}
}

#if WITH_EDITORONLY_DATA
void UIMAcousticSourceComponent::OnRegister()
{
	Super::OnRegister();
	if (!GIsEditor || !GetOwner()) return;
	const UWorld* World = GetWorld();
	if (!World || World->IsGameWorld()) return;
	CreateEditorBillboard();
}

void UIMAcousticSourceComponent::OnUnregister()
{
	if (AActor* Owner = GetOwner())
	{
		for (UStaticMeshComponent* WaveMesh : EditorMarkerWaveMeshes)
		{
			if (WaveMesh)
			{
				Owner->RemoveInstanceComponent(WaveMesh);
				WaveMesh->DestroyComponent();
			}
		}
	}
	EditorMarkerWaveMeshes.Empty();
	if (EditorMarkerMesh)
	{
		if (AActor* Owner = GetOwner())
		{
			Owner->RemoveInstanceComponent(EditorMarkerMesh);
		}
		EditorMarkerMesh->DestroyComponent();
		EditorMarkerMesh = nullptr;
	}
	if (EditorMarker)
	{
		if (AActor* Owner = GetOwner())
		{
			Owner->RemoveInstanceComponent(EditorMarker);
		}
		EditorMarker->DestroyComponent();
		EditorMarker = nullptr;
	}
	if (EditorBillboard)
	{
		if (AActor* Owner = GetOwner())
		{
			// The marker is an editor-only instance component. Remove it from
			// the actor list before destroying it so the Components panel and
			// subsequent re-registers cannot retain a stale entry.
			Owner->RemoveInstanceComponent(EditorBillboard);
		}
		EditorBillboard->DestroyComponent();
		EditorBillboard = nullptr;
	}
	Super::OnUnregister();
}

void UIMAcousticSourceComponent::CreateEditorBillboard()
{
	if (EditorBillboard || !GetOwner()) return;

	USceneComponent* Parent = AudioComponent.Get();
	if (!Parent) Parent = GetOwner()->GetRootComponent();
	if (!Parent) return;

	const EObjectFlags TransactionalFlag = GetFlags() & RF_Transactional;
	const FName BillboardName = MakeUniqueObjectName(
		GetOwner(), UBillboardComponent::StaticClass(), TEXT("IM_AcousticSourceBillboard"));
	EditorBillboard = NewObject<UBillboardComponent>(
		GetOwner(), BillboardName, TransactionalFlag | RF_Transient | RF_TextExportTransient);
	{
		// Engine audio sprites are editor-only references. Match
		// UAudioComponent::UpdateSpriteTexture so a dynamically created marker
		// does not receive a null texture while the editor is in cook-load mode.
		FCookLoadScope EditorOnlyScope(ECookLoadType::EditorOnly);
		EditorBillboard->SetSprite(LoadObject<UTexture2D>(
			nullptr, TEXT("/Engine/EditorResources/AudioIcons/S_AudioComponent.S_AudioComponent")));
	}
	// Keep the marker visibly separate from UAudioComponent's native sprite.
	// The native sprite sits at the source origin; this marker floats above it
	// so the author can find the acoustic source even in a dense whitebox.
	EditorBillboard->SetRelativeLocation(FVector(-60.0f, 0.0f, 80.0f));
	EditorBillboard->SetRelativeScale3D_Direct(FVector(2.0f));
	EditorBillboard->Mobility = EComponentMobility::Movable;
	EditorBillboard->AlwaysLoadOnClient = false;
	// This is intentionally a regular instance component, not a UE
	// Visualization Component. The editor's Components tree filters
	// IsVisualizationComponent() entries, which made the marker exist at
	// runtime but disappear from the author's Details/Components view.
	EditorBillboard->SetIsVisualizationComponent(false);
	EditorBillboard->bIsEditorOnly = true;
	// Keep the source marker readable when the editor camera is in a wall seam
	// or the source is behind whitebox geometry; this is editor compositing only.
	EditorBillboard->bUseEditorCompositing = true;
	EditorBillboard->SetVisibility(true);
	EditorBillboard->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));
	EditorBillboard->CreationMethod = EComponentCreationMethod::Instance;
	EditorBillboard->bIsScreenSizeScaled = true;
	EditorBillboard->ScreenSize = 0.02f;
	EditorBillboard->bUseInEditorScaling = true;
	EditorBillboard->OpacityMaskRefVal = 1.0f;
	// Do not put this transient marker behind a user's per-category Sounds
	// visibility toggle. The global BillboardSprites show flag remains the
	// normal editor control for this explicit source marker.
	EditorBillboard->SpriteInfo.Category = NAME_None;
	EditorBillboard->SpriteInfo.DisplayName = NSLOCTEXT(
		"SpriteCategory", "AcousticSource", "Acoustic Source");
	// Register the transient marker as an actor instance component as well as
	// a scene component. Without this, runtime enumeration can find it but the
	// editor Components tree/Details panel cannot expose it to the author.
	GetOwner()->AddInstanceComponent(EditorBillboard);
	EditorBillboard->SetupAttachment(Parent);
	EditorBillboard->RegisterComponent();

	// BillboardSprites is viewport-show-flag controlled. Add a small ordinary
	// foreground-line sign as the always-visible locator so a source cannot
	// become an invisible point merely because that optional overlay is off.
	const FName MarkerName = MakeUniqueObjectName(
		GetOwner(), UIMAcousticSourceMarkerComponent::StaticClass(), TEXT("IM_AcousticSourceMarker"));
	EditorMarker = NewObject<UIMAcousticSourceMarkerComponent>(
		GetOwner(), MarkerName, TransactionalFlag | RF_Transient | RF_TextExportTransient);
	EditorMarker->SetRelativeLocation(FVector(-60.0f, 0.0f, 80.0f));
	EditorMarker->Mobility = EComponentMobility::Movable;
	EditorMarker->bIsEditorOnly = true;
	EditorMarker->bUseEditorCompositing = true;
	EditorMarker->SetVisibility(true);
	EditorMarker->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));
	EditorMarker->CreationMethod = EComponentCreationMethod::Instance;
	GetOwner()->AddInstanceComponent(EditorMarker);
	EditorMarker->SetupAttachment(Parent);
	EditorMarker->RegisterComponent();

	// Keep a small solid icon in the main viewport pass as well. The editor
	// overlay line proxy is useful in the normal viewport, but a raw viewport
	// readback does not include every editor-composited primitive.
	const FName MarkerMeshName = MakeUniqueObjectName(
		GetOwner(), UStaticMeshComponent::StaticClass(), TEXT("IM_AcousticSourceIcon"));
	EditorMarkerMesh = NewObject<UStaticMeshComponent>(
		GetOwner(), MarkerMeshName, TransactionalFlag | RF_Transient | RF_TextExportTransient);
	{
		FCookLoadScope EditorOnlyScope(ECookLoadType::EditorOnly);
		EditorMarkerMesh->SetStaticMesh(LoadObject<UStaticMesh>(
			nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		if (UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			if (UMaterialInstanceDynamic* MarkerMaterial = UMaterialInstanceDynamic::Create(
				BaseMaterial, EditorMarkerMesh))
			{
				const FLinearColor MarkerColor(1.0f, 0.75f, 0.02f, 1.0f);
				MarkerMaterial->SetVectorParameterValue(TEXT("Color"), MarkerColor);
				MarkerMaterial->SetVectorParameterValue(TEXT("BaseColor"), MarkerColor);
				MarkerMaterial->SetVectorParameterValue(TEXT("EmissiveColor"), MarkerColor);
				MarkerMaterial->SetScalarParameterValue(TEXT("Emissive"), 4.0f);
				MarkerMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"), 4.0f);
				EditorMarkerMesh->SetMaterial(0, MarkerMaterial);
			}
		}
	}
	EditorMarkerMesh->SetRelativeLocation(FVector(-60.0f, -50.0f, 80.0f));
	EditorMarkerMesh->SetRelativeScale3D(FVector(0.25f, 0.45f, 0.8f));
	EditorMarkerMesh->Mobility = EComponentMobility::Movable;
	EditorMarkerMesh->bIsEditorOnly = true;
	EditorMarkerMesh->bUseEditorCompositing = true;
	EditorMarkerMesh->SetVisibility(true);
	// PIE duplicates editor instance meshes; editor-only cooking flags do not hide them in Play.
	EditorMarkerMesh->SetHiddenInGame(true);
	EditorMarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	EditorMarkerMesh->SetCastShadow(false);
	EditorMarkerMesh->bReceivesDecals = false;
	EditorMarkerMesh->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));
	EditorMarkerMesh->CreationMethod = EComponentCreationMethod::Instance;
	GetOwner()->AddInstanceComponent(EditorMarkerMesh);
	EditorMarkerMesh->SetupAttachment(Parent);
	EditorMarkerMesh->RegisterComponent();

	auto AddWaveMesh = [this, Owner = GetOwner(), Parent, TransactionalFlag](
		const TCHAR* Suffix,
		const FVector& RelativeLocation,
		const FVector& RelativeScale,
		const FRotator& RelativeRotation)
	{
		if (!Owner || !EditorMarkerMesh)
		{
			return;
		}

		const FName WaveName = MakeUniqueObjectName(
			Owner,
			UStaticMeshComponent::StaticClass(),
			*FString::Printf(TEXT("IM_AcousticSourceWave_%s"), Suffix));
		UStaticMeshComponent* WaveMesh = NewObject<UStaticMeshComponent>(
			Owner, WaveName, TransactionalFlag | RF_Transient | RF_TextExportTransient);
		WaveMesh->SetStaticMesh(EditorMarkerMesh->GetStaticMesh());
		WaveMesh->SetMaterial(0, EditorMarkerMesh->GetMaterial(0));
		WaveMesh->SetRelativeLocation(RelativeLocation);
		WaveMesh->SetRelativeScale3D(RelativeScale);
		WaveMesh->SetRelativeRotation(RelativeRotation);
		WaveMesh->Mobility = EComponentMobility::Movable;
		WaveMesh->bIsEditorOnly = true;
		WaveMesh->bUseEditorCompositing = true;
		WaveMesh->SetVisibility(true);
		WaveMesh->SetHiddenInGame(true);
		WaveMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WaveMesh->SetCastShadow(false);
		WaveMesh->bReceivesDecals = false;
		WaveMesh->ComponentTags.AddUnique(TEXT("IMAcousticIgnore"));
		WaveMesh->CreationMethod = EComponentCreationMethod::Instance;
		Owner->AddInstanceComponent(WaveMesh);
		WaveMesh->SetupAttachment(Parent);
		WaveMesh->RegisterComponent();
		EditorMarkerWaveMeshes.Add(WaveMesh);
	};

	AddWaveMesh(TEXT("A"), FVector(-60.0f, 8.0f, 80.0f), FVector(0.25f, 0.10f, 0.35f), FRotator(0.0f, 0.0f, -22.0f));
	AddWaveMesh(TEXT("B"), FVector(-60.0f, 38.0f, 80.0f), FVector(0.25f, 0.10f, 0.55f), FRotator::ZeroRotator);
	AddWaveMesh(TEXT("C"), FVector(-60.0f, 72.0f, 80.0f), FVector(0.25f, 0.10f, 0.75f), FRotator(0.0f, 0.0f, 22.0f));
}
#endif

bool UIMAcousticSourceComponent::ValidateSource(FString& Failure) const
{
	check(IsInGameThread());
	// BakeVolume shares Failure across sources. A successful validation must
	// preserve an earlier source rejection so the final status reports it.
	const UAudioComponent* Audio = AudioComponent.Get();
	if (!IsValid(Audio) || Audio->GetWorld() != GetWorld())
	{
		Failure = TEXT("AudioComponent is missing or belongs to another world.");
		return false;
	}
	const USoundWave* Wave = Cast<USoundWave>(Audio->Sound);
	if (IMAcousticMetaSound::IsAcousticMetaSound(Audio->Sound))
	{
		const FSoundAttenuationSettings* Settings = Audio->GetAttenuationSettingsToApply();
		if (Audio->bAllowSpatialization || (Settings && (Settings->bSpatialize
			|| IMAcousticSourceComponentPrivate::HasCompetingNativeProcessing(*Settings))))
		{
			Failure = TEXT("Acoustic MetaSound owns stereo spatialization and attenuation; disable native processing.");
			return false;
		}
		return true;
	}
	const UMetaSoundSource* MetaSound = Cast<UMetaSoundSource>(Audio->Sound);
	const bool bMonoWave = Wave && Wave->NumChannels == 1;
	const bool bMonoMetaSound = MetaSound && MetaSound->OutputFormat == EMetaSoundOutputAudioFormat::Mono
		&& MetaSound->NumChannels == 1;
	if (!bMonoWave && !bMonoMetaSound)
	{
		// Both accepted source types expose a fixed, verifiable mono voice to the
		// spatialization plugin. Other graph types may change channel layout.
		Failure = TEXT("The V2 delivery requires a mono SoundWave or mono MetaSoundSource.");
		return false;
	}
	const FSoundAttenuationSettings* Settings = Audio->GetAttenuationSettingsToApply();
	if (!Settings || !Audio->bAllowSpatialization || !Settings->bSpatialize
		|| Settings->SpatializationAlgorithm != SPATIALIZATION_HRTF)
	{
		Failure = TEXT("Enable plugin spatialization on the AudioComponent attenuation settings.");
		return false;
	}
	if (IMAcousticSourceComponentPrivate::HasCompetingNativeProcessing(*Settings))
	{
		Failure = TEXT("Disable native distance, air absorption, occlusion, listener focus and reverb sends; V2 owns these paths.");
		return false;
	}
	const auto& Plugins = Settings->PluginSettings;
	if (Plugins.SpatializationPluginSettingsArray.Num() != 1
		|| !IsValid(Plugins.SpatializationPluginSettingsArray[0])
		|| !Plugins.SpatializationPluginSettingsArray[0]->IsA<UIMAcousticSpatializationSettings>()
		|| !Plugins.OcclusionPluginSettingsArray.IsEmpty()
		|| !Plugins.ReverbPluginSettingsArray.IsEmpty()
		|| !Plugins.SourceDataOverridePluginSettingsArray.IsEmpty())
	{
		Failure = TEXT("Select only IceMoon spatialization settings and remove conflicting audio plugin settings.");
		return false;
	}
	return true;
}
