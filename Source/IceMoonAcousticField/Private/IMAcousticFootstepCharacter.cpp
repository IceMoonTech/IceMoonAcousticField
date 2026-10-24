#include "IMAcousticFootstepCharacter.h"

#include "IMAcousticPresenceParameters.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticSourceComponent.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Tests/IMAcousticOrdinaryRecorderBoundaryTest.h"
#endif
#include "HAL/IConsoleManager.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "ImageUtils.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Tests/IMAcousticAuditionCapture.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "UObject/ConstructorHelpers.h"
#include "UnrealClient.h"

namespace IMAcousticFootstepCharacterPrivate
{
// 演示角色高180 cm；完整左右步幅120 cm，120 cm/s对应每秒两次落脚。
constexpr float IMStrideCm = 120.f;
constexpr float IMWalkSpeedCm = 120.f;
constexpr float IMRunSpeedCm = 180.f;
constexpr float IMPatrolHalfLengthCm = 210.f;
constexpr float IMFootHeightCm = 4.f;
constexpr float IMFootLiftCm = 14.f;
constexpr float IMToeReachCm = 30.f;
constexpr float IMFootSeparationCm = 13.f;
// 演示跳跃约46/90cm高，低于现有关卡顶界；撞地响应曲线是可调整的声音设计值。
constexpr float IMLightJumpCmS = 300.f;
constexpr float IMStrongJumpCmS = 420.f;
constexpr float IMLandingReferenceCmS = 300.f;
constexpr double IMLandingContactGuardSeconds = .30;
// A two-footed landing is two ordinary footsteps of the floor material: the second sole touches down this long after the first.
// Design value, not a measurement: the tens-of-milliseconds spread between the two feet of a bilateral landing, kept short enough that
// the pair reads as one landing and long enough that the two hits do not phase against each other. Change it by ear if needed.
constexpr double IMLandingSecondFootDelaySeconds = .06;
constexpr double IMDemoJumpIntervalSeconds = 3.25;
// Waypoint reached when the capsule is within this planar distance. One tick moves at most 3 cm (180 cm/s at 60 Hz), so
// the radius is never stepped over, and it stays well inside the narrowest 120 cm door.
constexpr float IMRouteArriveCm = 25.f;
// Turn speed on the scripted route. The default 720 deg/s snaps a 90 degree corner in 4 video frames (the follow camera and the
// audio listener turn with the body); 180 deg/s takes 0.5 s, which reads as a walk.
constexpr float IMRouteTurnRateDegreesPerSecond = 180.f;
constexpr int32 IMMaxVideoWritesInFlight = 8;

bool IMFrameLandingObserver(AIMAcousticFootstepCharacter& Character)
{
	auto* Camera = Cast<ACameraActor>(Character.ObserverCamera.Get());
	if (!Camera || !Character.GetCharacterMovement())
	{
		UE_LOG(LogTemp, Warning, TEXT("IMLogs LandingDemo observer_frame_rejected reason=missing_camera_or_movement"));
		return false;
	}
	auto* CameraComponent = Camera->GetCameraComponent();
	const float GravityCmS2 = FMath::Abs(Character.GetCharacterMovement()->GetGravityZ());
	if (!CameraComponent || GravityCmS2 <= UE_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("IMLogs LandingDemo observer_frame_rejected reason=missing_camera_component_or_gravity gravity_cm_s2=%.3f"), GravityCmS2);
		return false;
	}
	TInlineComponentArray<UStaticMeshComponent*> Parts;
	Character.GetComponents(Parts);
	FBox VisibleBounds(ForceInit);
	int32 VisibleParts = 0;
	for (auto* Part : Parts)
	{
		if (!Part->IsRegistered() || !Part->IsVisible() || !Part->GetStaticMesh()) continue;
		VisibleBounds += Part->CalcBounds(Part->GetComponentTransform()).GetBox();
		++VisibleParts;
	}
	if (!VisibleBounds.IsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("IMLogs LandingDemo observer_frame_rejected reason=no_visible_mesh_bounds"));
		return false;
	}
	const FBox CachedActorBounds = Character.GetComponentsBoundingBox(true);
	FBox SweptBounds = VisibleBounds.ExpandBy(FVector(IMPatrolHalfLengthCm + IMToeReachCm, 0, 0));
	SweptBounds.Max.Z += FMath::Square(IMStrongJumpCmS) / (2.f * GravityCmS2) + IMFootLiftCm;
	const FVector Eye = Camera->GetActorLocation();
	const FRotator ViewRotation = (SweptBounds.GetCenter() - Eye).Rotation();
	float AspectRatio = CameraComponent->AspectRatio;
	if (!CameraComponent->bConstrainAspectRatio && GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		if (Size.X > 0 && Size.Y > 0) AspectRatio = float(Size.X) / float(Size.Y);
	}
	if (AspectRatio <= UE_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("IMLogs LandingDemo observer_frame_rejected reason=invalid_aspect aspect=%.6f"), AspectRatio);
		return false;
	}
	float RequiredHorizontalTan = 0.f;
	for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
	{
		const FVector Corner((CornerIndex & 1) ? SweptBounds.Max.X : SweptBounds.Min.X,
			(CornerIndex & 2) ? SweptBounds.Max.Y : SweptBounds.Min.Y,
			(CornerIndex & 4) ? SweptBounds.Max.Z : SweptBounds.Min.Z);
		const FVector Local = ViewRotation.UnrotateVector(Corner - Eye);
		if (Local.X <= UE_SMALL_NUMBER)
		{
			UE_LOG(LogTemp, Warning, TEXT("IMLogs LandingDemo observer_frame_rejected reason=visible_bounds_behind_camera corner=%d depth_cm=%.3f"), CornerIndex, Local.X);
			return false;
		}
		RequiredHorizontalTan = FMath::Max(RequiredHorizontalTan,
			FMath::Max(FMath::Abs(Local.Y) / Local.X, FMath::Abs(Local.Z) * AspectRatio / Local.X));
	}
	constexpr float BoundsFramingMargin = 1.1f;
	const float RequiredFOV = 2.f * FMath::RadiansToDegrees(FMath::Atan(RequiredHorizontalTan * BoundsFramingMargin));
	Camera->SetActorRotation(ViewRotation);
	CameraComponent->SetFieldOfView(FMath::Max(CameraComponent->FieldOfView, RequiredFOV));
	UE_LOG(LogTemp, Display,
		TEXT("IMLogs LandingDemo observer_frame bounds_source=visible_static_mesh_calc_bounds_swept_patrol_jump min_cm=%s max_cm=%s eye_cm=%s rotation=%s gravity_cm_s2=%.3f strong_jump_cm_s=%.3f horizontal_fov=%.3f aspect=%.6f visible_parts=%d cached_actor_min_cm=%s cached_actor_max_cm=%s listener_position_unchanged=1"),
		*SweptBounds.Min.ToString(), *SweptBounds.Max.ToString(), *Eye.ToString(), *ViewRotation.ToString(),
		GravityCmS2, IMStrongJumpCmS, CameraComponent->FieldOfView, AspectRatio, VisibleParts,
		*CachedActorBounds.Min.ToString(), *CachedActorBounds.Max.ToString());
	return true;
}
}

using namespace IMAcousticFootstepCharacterPrivate;

AIMAcousticFootstepCharacter::AIMAcousticFootstepCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	GetCapsuleComponent()->InitCapsuleSize(26.f, 90.f);
	GetCapsuleComponent()->bReturnMaterialOnMove = true;
	GetCapsuleComponent()->ComponentTags.Add(TEXT("IMAcousticIgnore"));
	GetMesh()->ComponentTags.Add(TEXT("IMAcousticIgnore"));
	bUseControllerRotationYaw = false;
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0, 720, 0);
	GetCharacterMovement()->MaxWalkSpeed = IMWalkSpeedCm;
	GetCharacterMovement()->BrakingDecelerationWalking = 1200.f;
	AutoPossessPlayer = EAutoReceiveInput::Player0;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BodyMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	auto AddPart = [&](const TCHAR* Name, FVector SizeCm, FVector Position)
	{
		auto* Part = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Part->SetupAttachment(GetCapsuleComponent());
		Part->SetStaticMesh(Cube.Object);
		Part->SetMaterial(0, BodyMaterial.Object);
		Part->SetRelativeScale3D(SizeCm / 100.f);
		Part->SetRelativeLocation(Position);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		Part->ComponentTags.Add(TEXT("IMAcousticIgnore"));
		return Part;
	};
	AddPart(TEXT("IM_Torso"), FVector(28, 32, 50), FVector(0, 0, 4));
	AddPart(TEXT("IM_Head"), FVector(25, 25, 26), FVector(0, 0, 48));
	for (int32 Foot = 0; Foot < 2; ++Foot)
	{
		const float Side = Foot ? 1.f : -1.f;
		Feet.Add(AddPart(Foot ? TEXT("IM_RightFoot") : TEXT("IM_LeftFoot"),
			FVector(26, 12, 8), FVector(0, Side * IMFootSeparationCm, -86)));
		Legs.Add(AddPart(Foot ? TEXT("IM_RightLeg") : TEXT("IM_LeftLeg"),
			FVector(10, 10, 57), FVector(0, Side * IMFootSeparationCm, -56)));
		Arms.Add(AddPart(Foot ? TEXT("IM_RightArm") : TEXT("IM_LeftArm"),
			FVector(10, 10, 44), FVector(0, Side * 24, 0)));
	}
	ListenerCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("IM_ListenerCamera"));
	ListenerCamera->SetupAttachment(GetCapsuleComponent());
	ListenerCamera->SetUsingAbsoluteLocation(true);
	ListenerCamera->SetUsingAbsoluteRotation(true);
	ListenerCamera->bUsePawnControlRotation = false;
	ListenerCamera->SetFieldOfView(75.f);
	ListenerCamera->ComponentTags.Add(TEXT("IMAcousticIgnore"));
	FootstepAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("IM_FootstepAudio"));
	FootstepAudio->SetupAttachment(GetCapsuleComponent());
	FootstepAudio->bAutoActivate = false;
	FootstepAudio->bAllowSpatialization = false;
	FootstepAudio->bIsUISound = false;
	FootstepAudio->VolumeMultiplier = .7f;
	FootstepAudio->ComponentTags.Add(TEXT("IMAcousticIgnore"));
	AcousticSource = CreateDefaultSubobject<UIMAcousticSourceComponent>(TEXT("IM_FootstepSource"));
	AcousticSource->AudioComponent = FootstepAudio;
	Tags.Add(TEXT("IMAcousticFootstepCharacterR89"));
}

void AIMAcousticFootstepCharacter::BeginPlay()
{
	Super::BeginPlay();
	StartedAtSeconds = GetWorld()->GetTimeSeconds();
	NextDemoJumpSeconds = StartedAtSeconds + IMDemoJumpIntervalSeconds;
	PreviousLocation = PatrolOrigin = GetActorLocation();
	bObserverFrameApplied = ObserverCamera && !bFollowListenerCamera && IMFrameLandingObserver(*this);
	RouteIndex = 0;
	if (!RouteWaypoints.IsEmpty()) GetCharacterMovement()->RotationRate = FRotator(0, IMRouteTurnRateDegreesPerSecond, 0);
	if (auto* PresenceSound = LoadObject<USoundBase>(nullptr,
		TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/MS_IMFootstepPresenceNearfield.MS_IMFootstepPresenceNearfield")))
		FootstepAudio->SetSound(PresenceSound);
	else
		FootstepAudio->SetSound(FootstepSound);
	for (const TCHAR* Name : {TEXT("A"), TEXT("B")})
	{
		const FString WoodName = FString::Printf(TEXT("SW_IMFootstepPresenceWood%s"), Name);
		const FString StoneName = FString::Printf(TEXT("SW_IMFootstepPresenceStone%s"), Name);
		PresenceWoodSteps.Add(LoadObject<USoundWave>(nullptr, *(TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/") + WoodName + TEXT(".") + WoodName)));
		PresenceStoneSteps.Add(LoadObject<USoundWave>(nullptr, *(TEXT("/IceMoonAcousticField/Tests/Audio/Footsteps/") + StoneName + TEXT(".") + StoneName)));
	}
	FootstepAudio->SetFloatParameter(TEXT("Footstep Gain"), .65f);
	FootstepAudio->SetFloatParameter(TEXT("Footstep Pitch"), 0.f);
	if (!WoodSteps.IsEmpty()) FootstepAudio->SetWaveParameter(TEXT("Footstep Wave"), WoodSteps[0]);
	// 场景声场会绑定voice后第一次Play；这里不抢先构造没有绑定身份的generator。
	for (int32 Foot = 0; Foot < 2; ++Foot)
	{
		PlantedFeet[Foot] = Feet[Foot]->GetComponentLocation();
		FHitResult Hit;
		if (TraceSole(Foot, Hit)) PlantedFeet[Foot] = Hit.ImpactPoint + FVector(0, 0, IMFootHeightCm);
	}
	if (auto* PlayerController = GetWorld()->GetFirstPlayerController())
	{
		PlayerController->Possess(this);
		if (bFollowListenerCamera) PlayerController->SetViewTarget(this);
		else if (ObserverCamera) PlayerController->SetViewTarget(ObserverCamera);
	}
	SetExaggeratedPresence(bExaggeratedPresence);
	SetEqualLoudness(true);
	FString Failure;
	const bool bConfigured = FootstepSound && WoodPhysicalMaterial && StonePhysicalMaterial
		&& !WoodSteps.IsEmpty() && !StoneSteps.IsEmpty() && AcousticSource->ValidateSource(Failure);
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo begin configured=%d actor=%s graph=%s wood=%s stone=%s camera=%s reason=%s"),
		bConfigured, *GetPathName(), *GetPathNameSafe(FootstepSound), *GetPathNameSafe(WoodPhysicalMaterial),
		*GetPathNameSafe(StonePhysicalMaterial), *GetPathNameSafe(ObserverCamera), *Failure);
	UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo configured=%d auto_demo=%d keys=J,Shift+J,Space"), bConfigured, bAutoLandingDemo);
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepRoute waypoints=%d jumps=%d strong_jumps=%d follow_camera=%d"),
		RouteWaypoints.Num(), RouteJumpWaypoints.Num(), RouteStrongJumpWaypoints.Num(), bFollowListenerCamera);
	if (!bConfigured) SetAutoWalking(false);
}

void AIMAcousticFootstepCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
	if (bVideoCapturing) StopVideoCapture();
	FootstepAudio->Stop();
	if (auto* PlayerController = GetWorld()->GetFirstPlayerController()) PlayerController->ClearAudioListenerOverride();
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo end contacts=%d wood=%d stone=%d unknown=%d"),
		ContactCount, WoodContactCount, StoneContactCount, UnknownContactCount);
	UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo end landings=%d wood=%d stone=%d"), LandingCount, WoodLandingCount, StoneLandingCount);
	Super::EndPlay(Reason);
}

void AIMAcousticFootstepCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
	Super::SetupPlayerInputComponent(Input);
	Input->BindKey(EKeys::W, IE_Pressed, this, &AIMAcousticFootstepCharacter::ForwardDown);
	Input->BindKey(EKeys::W, IE_Released, this, &AIMAcousticFootstepCharacter::ForwardUp);
	Input->BindKey(EKeys::S, IE_Pressed, this, &AIMAcousticFootstepCharacter::BackwardDown);
	Input->BindKey(EKeys::S, IE_Released, this, &AIMAcousticFootstepCharacter::BackwardUp);
	Input->BindKey(EKeys::D, IE_Pressed, this, &AIMAcousticFootstepCharacter::RightDown);
	Input->BindKey(EKeys::D, IE_Released, this, &AIMAcousticFootstepCharacter::RightUp);
	Input->BindKey(EKeys::A, IE_Pressed, this, &AIMAcousticFootstepCharacter::LeftDown);
	Input->BindKey(EKeys::A, IE_Released, this, &AIMAcousticFootstepCharacter::LeftUp);
	Input->BindKey(EKeys::SpaceBar, IE_Pressed, this, &AIMAcousticFootstepCharacter::ToggleAutoWalking);
	Input->BindKey(EKeys::LeftShift, IE_Pressed, this, &AIMAcousticFootstepCharacter::BeginRunning);
	Input->BindKey(EKeys::LeftShift, IE_Released, this, &AIMAcousticFootstepCharacter::EndRunning);
	Input->BindKey(EKeys::J, IE_Pressed, this, &AIMAcousticFootstepCharacter::JumpDown);
	Input->BindKey(EKeys::J, IE_Released, this, &AIMAcousticFootstepCharacter::JumpUp);
	Input->BindKey(EKeys::B, IE_Pressed, this, &AIMAcousticFootstepCharacter::TogglePresence);
	Input->BindKey(EKeys::N, IE_Pressed, this, &AIMAcousticFootstepCharacter::ToggleEqualLoudness);
}

void AIMAcousticFootstepCharacter::SetExaggeratedPresence(bool bEnabled)
{
	bExaggeratedPresence = bEnabled;
	bPublishedPresence = bEnabled;
	FootstepAudio->SetFloatParameter(TEXT("Presence Enabled"), bEnabled ? 1.f : 0.f);
	FootstepAudio->SetFloatParameter(TEXT("Presence Near Field"), bEnabled && IsPlayerControlled() ? 1.f : 0.f);
	PublishPresenceFlags();
	PublishPresenceHint();
	UE_LOG(LogTemp, Display, TEXT("IMLogs Presence mode=%s"), bEnabled ? TEXT("exaggerated") : TEXT("physical"));
}

void AIMAcousticFootstepCharacter::TogglePresence() { SetExaggeratedPresence(!bExaggeratedPresence); }

void AIMAcousticFootstepCharacter::PublishPresenceFlags()
{
	// The bake volume reads this on the next tick and hands it to the audio-thread source slot.
	// Own footsteps keep the HRTF direction and only lose the listener-distance attenuation.
	AcousticSource->PresenceFlags = !bExaggeratedPresence ? 0
		: uint8(IMAcousticPresenceFlags::Exaggerated | (IsPlayerControlled() ? IMAcousticPresenceFlags::SelfNearField : 0));
}

void AIMAcousticFootstepCharacter::PublishPresenceLateral(const FVector& FootLocation)
{
	// How far the foot sits to the side of the body axis, as an azimuth seen from the listener. The listener is
	// behind the character, so this is the only left/right information a footstep has (about 2.3 degrees at 3 m
	// for the 13 cm half stride). Exaggerated presence enlarges exactly this part of the direct-sound direction.
	AcousticSource->PresenceLateralAzimuthRadians = 0.f;
	if (!bExaggeratedPresence) return;
	const APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (!PlayerController) return;
	FVector ListenerLocation, ListenerFront, ListenerRight;
	PlayerController->GetAudioListenerPosition(ListenerLocation, ListenerFront, ListenerRight);
	const double Ahead = FVector::DotProduct(FootLocation - ListenerLocation, ListenerFront);
	if (Ahead <= 1.0) return;
	const double Side = FVector::DotProduct(FootLocation - GetActorLocation(), ListenerRight);
	const float Lateral = float(FMath::Atan2(Side, Ahead));
	if (!FMath::IsFinite(Lateral)) return;
	AcousticSource->PresenceLateralAzimuthRadians = Lateral;
	for (TActorIterator<AIMAcousticBakeVolume> It(GetWorld()); It; ++It)
		It->PublishPresenceLateralNow(FootstepAudio->GetAudioComponentID(), Lateral);
	UE_LOG(LogTemp, Display, TEXT("IMLogs PresenceLateral foot_side_cm=%.3f ahead_cm=%.3f lateral_deg=%.4f"), Side, Ahead, FMath::RadiansToDegrees(Lateral));
}

void AIMAcousticFootstepCharacter::SetSynthesizedPresenceSamples(bool bEnabled)
{
	bSynthesizedPresenceSamples = bEnabled;
	SetEqualLoudness(bEqualLoudness);
}

void AIMAcousticFootstepCharacter::PublishPresenceHint()
{
	if (GEngine)
		GEngine->AddOnScreenDebugMessage(18010, 86400.f, bExaggeratedPresence ? FColor::Orange : FColor::Green,
			FString::Printf(TEXT("B: %s | STRENGTH %.2f (IM.AcousticField.PresenceStrength 0..2) | N: EQUAL %s (EXAG %.2f dB) | WASD move | J jump | Shift+J heavy | Space auto | SELF: NEAR / PHYSICAL: 3m"),
				bExaggeratedPresence ? TEXT("EXAGGERATED") : TEXT("PHYSICAL"), PresenceStrength, bEqualLoudness ? TEXT("ON") : TEXT("OFF"),
				20.f * FMath::LogX(10.f, PresenceCompensation)));
}

void AIMAcousticFootstepCharacter::SetEqualLoudness(bool bEnabled)
{
	FString Text;
	TSharedPtr<FJsonObject> Calibration;
	const FString File = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("IceMoonAcousticField/Content/Tests/Audio/IMGenericReverbCalibration.json"));
	bool bCalibrated = FFileHelper::LoadFileToString(Text, *File)
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Calibration) && Calibration.IsValid();
	// Each sample set has its own measured gain: recorded waves and synthesized waves differ in level.
	const TCHAR* GainField = bSynthesizedPresenceSamples ? TEXT("compensation_gain_synthesized") : TEXT("compensation_gain");
	bCalibrated = bCalibrated && Calibration->HasTypedField<EJson::Number>(GainField);
	if (bCalibrated)
	{
		const float Measured = float(Calibration->GetNumberField(GainField));
		bCalibrated = FMath::IsFinite(Measured) && Measured > 0.f;
		if (bCalibrated) PresenceCompensation = Measured;
	}
	// A route-fitted gain (SetPresenceCompensationOverride) replaces the file value for this session only; the file stays as it was.
	if (PresenceCompensationOverride > 0.f)
	{
		PresenceCompensation = PresenceCompensationOverride;
		bCalibrated = true;
	}
	bEqualLoudness = bEnabled && bCalibrated;
	FootstepAudio->SetFloatParameter(TEXT("Presence Compensation"), PresenceCompensation);
	FootstepAudio->SetFloatParameter(TEXT("Presence Equal Loudness"), bEqualLoudness ? 1.f : 0.f);
	PublishPresenceHint();
}

void AIMAcousticFootstepCharacter::ToggleEqualLoudness() { SetEqualLoudness(!bEqualLoudness); }

void AIMAcousticFootstepCharacter::SetPresenceCompensationOverride(float Gain)
{
	PresenceCompensationOverride = FMath::IsFinite(Gain) && Gain > 0.f ? Gain : 0.f;
	SetEqualLoudness(bEqualLoudness);
}

void AIMAcousticFootstepCharacter::UpdateAuditionListener()
{
	if (auto* PlayerController = GetWorld()->GetFirstPlayerController())
	{
		const FVector Origin = GetActorLocation();
		const FVector Desired = Origin - GetActorForwardVector() * 300.f + FVector(0, 0, 60.f);
		const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Query(SCENE_QUERY_STAT(IMAcousticAuditionListener), false, this);
		FHitResult Obstruction;
		const bool bBlocked = GetWorld()->SweepSingleByChannel(Obstruction, Origin, Desired,
			FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(Radius), Query);
		const FVector Location = bBlocked ? Obstruction.Location : Desired;
		const FRotator Rotation = (Origin - Location).Rotation();
		PlayerController->SetAudioListenerOverride(nullptr, Location, Rotation);
		if (ListenerCamera) ListenerCamera->SetWorldLocationAndRotation(Location, Rotation);
		IMAcousticAuditionCapture::Listener(Desired, Location, Radius,
			bBlocked && Obstruction.GetActor() ? Obstruction.GetActor()->GetFName() : NAME_None);
	}
}

void AIMAcousticFootstepCharacter::BeginAuditionPass(bool bRecord, const FString& Label)
{
	AuditionLabel = Label;
	AuditionStartedSeconds = GetWorld()->GetTimeSeconds();
	AuditionPoseIndex = AuditionEventIndex = 0;
	ContactCount = LandingCount = WoodContactCount = StoneContactCount = WoodLandingCount = StoneLandingCount = LandingStepCount = 0;
	PendingSecondFootSeconds = -1.0;
	PlayedAuditionEvents.Reset();
	GetCharacterMovement()->StopMovementImmediately();
	bAuditionRecording = bRecord;
	bAuditionReplaying = !bRecord;
	if (bRecord)
	{
		AuditionPoses.Reset();
		AuditionEvents.Reset();
		bAutoWalk = bAutoLandingDemo = true;
		RouteIndex = 0;
		NextDemoJumpSeconds = AuditionStartedSeconds + IMDemoJumpIntervalSeconds;
		bNextDemoStrong = false;
	}
	else
	{
		check(!AuditionPoses.IsEmpty() && !AuditionEvents.IsEmpty());
		bAutoWalk = false;
		GetCharacterMovement()->DisableMovement();
		SetActorTransform(AuditionPoses[0].Transform, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AIMAcousticFootstepCharacter::PauseAuditionPass()
{
	bAuditionRecording = bAuditionReplaying = false;
	bAutoWalk = false;
	GetCharacterMovement()->DisableMovement();
}

void AIMAcousticFootstepCharacter::EndAuditionPass()
{
	PauseAuditionPass();
	auto Report = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const auto& Event : PlayedAuditionEvents)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("seconds"), Event.Seconds);
		Row->SetStringField(TEXT("wave"), Event.Wave);
		Row->SetStringField(TEXT("canonical_wave"), Event.CanonicalWave);
		Row->SetStringField(TEXT("material"), Event.bStone ? TEXT("stone") : TEXT("wood"));
		Row->SetBoolField(TEXT("self_near_field"), Event.bSelfNearField);
		Row->SetNumberField(TEXT("enclosure"), Event.Enclosure);
		Row->SetBoolField(TEXT("landing"), Event.bLanding);
		Row->SetNumberField(TEXT("foot"), Event.Foot);
		Row->SetNumberField(TEXT("gain"), Event.Gain);
		Row->SetNumberField(TEXT("pitch"), Event.Pitch);
		Row->SetNumberField(TEXT("impact"), Event.Impact);
		Row->SetStringField(TEXT("position_cm"), Event.Hit.ImpactPoint.ToString());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Report->SetArrayField(TEXT("events"), Rows);
	Report->SetNumberField(TEXT("canonical_poses"), AuditionPoses.Num());
	Report->SetNumberField(TEXT("canonical_events"), AuditionEvents.Num());
	Report->SetNumberField(TEXT("replayed_events"), AuditionEventIndex);
	FString Text;
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Text));
	const FString File = FPaths::Combine(IMAcousticAuditionCapture::OutputDirectory(), AuditionLabel + TEXT("_events.json"));
	FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void AIMAcousticFootstepCharacter::AdvanceAuditionReplay()
{
	TickEmittedEvents.Reset();
	const double Seconds = GetWorld()->GetTimeSeconds() - AuditionStartedSeconds;
	while (AuditionPoseIndex + 1 < AuditionPoses.Num() && AuditionPoses[AuditionPoseIndex + 1].Seconds <= Seconds) ++AuditionPoseIndex;
	const auto& Pose = AuditionPoses[AuditionPoseIndex];
	SetActorTransform(Pose.Transform, false, nullptr, ETeleportType::TeleportPhysics);
	LastPresenceEnclosure = Pose.Enclosure;
	FootstepAudio->SetFloatParameter(TEXT("Presence Enclosure"), Pose.Enclosure);
	for (int32 Foot = 0; Foot < 2; ++Foot)
	{
		UpdateLeg(Foot, Pose.Feet[Foot]);
		Arms[Foot]->SetRelativeRotation(Pose.Arms[Foot]);
	}
	while (AuditionEventIndex < AuditionEvents.Num() && AuditionEvents[AuditionEventIndex].Seconds <= Seconds)
	{
		const auto& Event = AuditionEvents[AuditionEventIndex];
		AuditionImpactOverride = Event.Impact;
		EmitSurfaceSound(Event.Hit, Event.Foot, Event.bLanding, Event.Gain, Event.Pitch);
		AuditionImpactOverride = -1.f;
		if (!Event.bLanding) ++ContactCount;
		TickEmittedEvents.Add(AuditionEventIndex);
		++AuditionEventIndex;
	}
	UpdateAuditionListener();
}

void AIMAcousticFootstepCharacter::SyncPresenceStrength()
{
	if (!PresenceStrengthVariable) PresenceStrengthVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("IM.AcousticField.PresenceStrength"));
	if (!PresenceStrengthVariable) return;
	const float FromProperty = FIMAcousticPresenceParameters::ClampStrength(PresenceStrength);
	const float FromConsole = FIMAcousticPresenceParameters::ClampStrength(PresenceStrengthVariable->GetFloat());
	if (PublishedPresenceStrength < 0.f)
	{
		// First sync: an authored (non-default) property value drives the variable; otherwise a value already set on the console is kept.
		if (FromProperty != 1.f) PresenceStrengthVariable->Set(FromProperty, ECVF_SetByConsole);
		else PresenceStrength = FromConsole;
	}
	else if (FromProperty != PublishedPresenceStrength) PresenceStrengthVariable->Set(FromProperty, ECVF_SetByConsole);
	else if (FromConsole != PublishedPresenceStrength) PresenceStrength = FromConsole;
	else return;
	PresenceStrength = FIMAcousticPresenceParameters::ClampStrength(PresenceStrength);
	PublishedPresenceStrength = PresenceStrength;
	PublishPresenceHint();
	UE_LOG(LogTemp, Display, TEXT("IMLogs PresenceStrength strength=%.3f"), PresenceStrength);
}

void AIMAcousticFootstepCharacter::UpdatePresenceParameters()
{
	SyncPresenceStrength();
	if (bPublishedPresence != bExaggeratedPresence) SetExaggeratedPresence(bExaggeratedPresence);
	if (GetWorld()->GetTimeSeconds() < NextPresenceProbeSeconds) return;
	NextPresenceProbeSeconds = GetWorld()->GetTimeSeconds() + .1;
	const FVector Origin = GetActorLocation() + FVector(0, 0, 30);
	const FVector Directions[] = {FVector::ForwardVector, -FVector::ForwardVector,
		FVector::RightVector, -FVector::RightVector, FVector::UpVector};
	FCollisionQueryParams Query(SCENE_QUERY_STAT(IMAcousticPresenceWalls), false, this);
	float Enclosure = 0.f;
	for (const FVector& Direction : Directions)
	{
		FHitResult Wall;
		if (GetWorld()->LineTraceSingleByChannel(Wall, Origin, Origin + Direction * 600.f, ECC_Visibility, Query))
			Enclosure += 1.f - FMath::Clamp(Wall.Distance / 600.f, 0.f, 1.f);
	}
	LastPresenceEnclosure = Enclosure / UE_ARRAY_COUNT(Directions);
	FootstepAudio->SetFloatParameter(TEXT("Presence Enclosure"), LastPresenceEnclosure);
}

void AIMAcousticFootstepCharacter::ForwardDown() { bForwardHeld = true; bAutoWalk = false; }
void AIMAcousticFootstepCharacter::ForwardUp() { bForwardHeld = false; }
void AIMAcousticFootstepCharacter::BackwardDown() { bBackwardHeld = true; bAutoWalk = false; }
void AIMAcousticFootstepCharacter::BackwardUp() { bBackwardHeld = false; }
void AIMAcousticFootstepCharacter::RightDown() { bRightHeld = true; bAutoWalk = false; }
void AIMAcousticFootstepCharacter::RightUp() { bRightHeld = false; }
void AIMAcousticFootstepCharacter::LeftDown() { bLeftHeld = true; bAutoWalk = false; }
void AIMAcousticFootstepCharacter::LeftUp() { bLeftHeld = false; }

void AIMAcousticFootstepCharacter::SetAutoWalking(bool bEnabled)
{
	bAutoWalk = bEnabled;
	if (!bEnabled) GetCharacterMovement()->StopMovementImmediately();
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo auto_walk=%d contacts=%d"), bAutoWalk, ContactCount);
}

void AIMAcousticFootstepCharacter::ToggleAutoWalking() { SetAutoWalking(!bAutoWalk); }
void AIMAcousticFootstepCharacter::BeginRunning() { bRunning = true; }
void AIMAcousticFootstepCharacter::EndRunning() { bRunning = false; }
void AIMAcousticFootstepCharacter::JumpDown()
{
	++ManualJumpPressCount;
	bLastManualJumpStrong = bRunning;
	bManualJumpPending = CanJump();
	RequestVisibleJump(bRunning);
	UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo native_J_callback press_count=%d strong=%d jump_pending=%d"),
		ManualJumpPressCount, bLastManualJumpStrong, bManualJumpPending);
}

void AIMAcousticFootstepCharacter::JumpUp()
{
	++ManualJumpReleaseCount;
	StopJumping();
	UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo native_J_callback release_count=%d"), ManualJumpReleaseCount);
}

void AIMAcousticFootstepCharacter::RequestVisibleJump(bool bStrong)
{
	if (!CanJump()) return;
	GetCharacterMovement()->JumpZVelocity = bStrong ? IMStrongJumpCmS : IMLightJumpCmS;
	Jump();
}

void AIMAcousticFootstepCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();
	if (bManualJumpPending)
	{
		++ManualJumpStartCount;
		LastManualJumpStartSpeedCmS = GetVelocity().Z;
		bManualJumpPending = false;
		UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo native_J_physical_jump starts=%d strong=%d velocity_z_cm_s=%.3f"),
			ManualJumpStartCount, bLastManualJumpStrong, LastManualJumpStartSpeedCmS);
	}
	UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo jump time=%.6f frame=%llu velocity_z_cm_s=%.3f position=%s"),
		GetWorld()->GetTimeSeconds(), GFrameCounter, GetVelocity().Z, *GetActorLocation().ToString());
}

void AIMAcousticFootstepCharacter::Landed(const FHitResult& Hit)
{
	// ProcessLanded在SetPostLandedPhysics之前回调，保留碰撞前速度，不能读清零后的Walking速度。
	const float ImpactCmS = FMath::Max(0.f, -FVector::DotProduct(GetVelocity(), Hit.ImpactNormal));
	Super::Landed(Hit);
	bGroundedMoving = false;
	FHitResult SoleHit = Hit;
	for (int32 Foot = 0; Foot < 2; ++Foot)
	{
		FHitResult Ground;
		if (TraceSole(Foot, Ground))
		{
			PlantedFeet[Foot] = Ground.ImpactPoint + FVector(0, 0, IMFootHeightCm);
			UpdateLeg(Foot, PlantedFeet[Foot]);
			if (Foot == 0) SoleHit = Ground;
		}
	}
	// 初始摆放的2cm落差不发音；实际跳跃/跌落碰撞才驱动落地声音。
	if (ImpactCmS < 100.f) return;
	// Both feet play the floor material's own footstep samples, unshifted and at the existing impact gain (impact speed over the 300 cm/s
	// reference, 0.4 to 1.5 times a walking step). The first sole sounds now, the second IMLandingSecondFootDelaySeconds later.
	const float Gain = FMath::Clamp(ImpactCmS / IMLandingReferenceCmS, .4f, 1.5f);
	const float Pitch = 0.f;
	LastLandingImpactCmS = ImpactCmS;
	if (!EmitSurfaceSound(SoleHit, 0, true, Gain, Pitch)) return;
	LastLandingSeconds = GetWorld()->GetTimeSeconds();
	PendingSecondFootSeconds = LastLandingSeconds + IMLandingSecondFootDelaySeconds;
	PendingSecondFootGain = Gain;
	LastLandingGain = Gain;
	LastLandingPitch = Pitch;
	++LandingCount;
	if (SoleHit.PhysMaterial.Get() == WoodPhysicalMaterial) ++WoodLandingCount; else ++StoneLandingCount;
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	IM_RecordOrdinaryFootstepSourceEvent(*this, true, LandingCount, SoleHit.PhysMaterial.Get(),
		FootstepAudio->GetAudioComponentID(), Gain, Pitch);
#endif
	UE_LOG(LogTemp, Display, TEXT("IMLogs LandingDemo contact=%d time=%.6f frame=%llu material=%s surface=%s impact_cm_s=%.3f gain=%.6f pitch_st=%.6f sole=%s source=%s audio=%llu"),
		LandingCount, LastLandingSeconds, GFrameCounter, *GetPathNameSafe(SoleHit.PhysMaterial.Get()),
		SoleHit.PhysMaterial.Get() == WoodPhysicalMaterial ? TEXT("wood") : TEXT("stone"),
		ImpactCmS, Gain, Pitch, *SoleHit.ImpactPoint.ToString(), *FootstepAudio->GetComponentLocation().ToString(), FootstepAudio->GetAudioComponentID());
}

void AIMAcousticFootstepCharacter::Tick(float DeltaSeconds)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_IMAcousticFootstepGameTick);
	Super::Tick(DeltaSeconds);
	if (bAuditionReplaying)
	{
		AdvanceAuditionReplay();
		if (bVideoCapturing) CaptureVideoFrame();
		return;
	}
	GetCharacterMovement()->MaxWalkSpeed = bRunning ? IMRunSpeedCm : IMWalkSpeedCm;
	EmitPendingLandingFoot();
	if (!bAutoWalk)
	{
		AddMovementInput(FVector::ForwardVector, float(bForwardHeld) - float(bBackwardHeld));
		AddMovementInput(FVector::RightVector, float(bRightHeld) - float(bLeftHeld));
	}
	// 留两秒让普通开图/声场注册完成；不是静音预播，不触发任何脚步。
	if (bAutoWalk && !RouteWaypoints.IsEmpty() && GetWorld()->GetTimeSeconds() - StartedAtSeconds > 2.f) AdvanceRoute();
	else if (bAutoWalk && GetWorld()->GetTimeSeconds() - StartedAtSeconds > 2.f)
	{
		const float Offset = GetActorLocation().X - PatrolOrigin.X;
		if (Offset > IMPatrolHalfLengthCm) PatrolDirection = -1.f;
		if (Offset < -IMPatrolHalfLengthCm) PatrolDirection = 1.f;
		AddMovementInput(FVector::ForwardVector, PatrolDirection);
	}
	if (bAutoWalk && bAutoLandingDemo && RouteWaypoints.IsEmpty() && GetWorld()->GetTimeSeconds() >= NextDemoJumpSeconds
		&& GetCharacterMovement()->IsMovingOnGround())
	{
		RequestVisibleJump(bNextDemoStrong);
		bNextDemoStrong = !bNextDemoStrong;
		NextDemoJumpSeconds = GetWorld()->GetTimeSeconds() + IMDemoJumpIntervalSeconds;
	}
	AdvanceWalking(DeltaSeconds);
	UpdatePresenceParameters();
	UpdateAuditionListener();
	if (bAuditionRecording)
	{
		FIMAcousticAuditionPose Pose;
		Pose.Seconds = GetWorld()->GetTimeSeconds() - AuditionStartedSeconds;
		Pose.Transform = GetActorTransform();
		Pose.Enclosure = LastPresenceEnclosure;
		for (int32 Foot = 0; Foot < 2; ++Foot)
		{
			Pose.Feet[Foot] = Feet[Foot]->GetComponentLocation();
			Pose.Arms[Foot] = Arms[Foot]->GetRelativeRotation();
		}
		AuditionPoses.Add(Pose);
	}
}

void AIMAcousticFootstepCharacter::AdvanceRoute()
{
	if (RouteIndex >= RouteWaypoints.Num()) return;
	FVector ToTarget = RouteWaypoints[RouteIndex] - GetActorLocation();
	ToTarget.Z = 0.0;
	if (ToTarget.Size2D() <= IMRouteArriveCm)
	{
		const bool bJump = RouteJumpWaypoints.Contains(RouteIndex);
		const bool bStrong = RouteStrongJumpWaypoints.Contains(RouteIndex);
		UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepRoute reached=%d/%d time=%.6f jump=%d strong=%d position=%s"),
			RouteIndex, RouteWaypoints.Num(), GetWorld()->GetTimeSeconds(), bJump, bStrong, *GetActorLocation().ToString());
		if (bJump) RequestVisibleJump(bStrong);
		++RouteIndex;
		if (RouteIndex >= RouteWaypoints.Num()) UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepRoute complete time=%.6f"), GetWorld()->GetTimeSeconds());
		return;
	}
	AddMovementInput(ToTarget.GetSafeNormal(), 1.f);
}

bool AIMAcousticFootstepCharacter::StartVideoCapture(const FString& Directory, int32 Width, int32 Height, float FramesPerSecond, float FovDegrees, float ExposureBias)
{
	if (bVideoCapturing || Width < 16 || Height < 16 || FramesPerSecond < 1.f || !ListenerCamera) return false;
	IFileManager::Get().MakeDirectory(*Directory, true);
	VideoDirectory = Directory;
	VideoWidth = Width;
	VideoHeight = Height;
	VideoFrameIndex = 0;
	VideoFrameRows.Reset();
	VideoWritesInFlight = MakeShared<std::atomic<int32>>(0);
	VideoTarget = NewObject<UTextureRenderTarget2D>(this);
	VideoTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
	VideoTarget->ClearColor = FLinearColor::Black;
	VideoTarget->InitAutoFormat(Width, Height);
	VideoTarget->UpdateResourceImmediate(true);
	VideoCapture = NewObject<USceneCaptureComponent2D>(this, TEXT("IM_VideoCapture"));
	VideoCapture->SetupAttachment(GetRootComponent());
	VideoCapture->TextureTarget = VideoTarget;
	VideoCapture->CaptureSource = SCS_FinalColorLDR;
	VideoCapture->bCaptureEveryFrame = false;
	VideoCapture->bCaptureOnMovement = false;
	VideoCapture->bAlwaysPersistRenderingState = true;
	VideoCapture->FOVAngle = FovDegrees;
	// One locked exposure for the whole route, so frames of different spaces are comparable and consecutive frames never pump.
	// Min = Max brightness (EV100) pins the histogram exposure to that value.
	VideoCapture->PostProcessBlendWeight = 1.f;
	VideoCapture->PostProcessSettings.bOverride_AutoExposureMethod = true;
	VideoCapture->PostProcessSettings.AutoExposureMethod = AEM_Histogram;
	VideoCapture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
	VideoCapture->PostProcessSettings.AutoExposureMinBrightness = ExposureBias;
	VideoCapture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
	VideoCapture->PostProcessSettings.AutoExposureMaxBrightness = ExposureBias;
	VideoCapture->RegisterComponent();
	// Fixed step: every game tick is exactly 1/fps of game time, so frame N is replay time N/fps and no frame is dropped or repeated.
	bVideoPreviousFixed = FApp::UseFixedTimeStep();
	VideoPreviousFixedDelta = FApp::GetFixedDeltaTime();
	FApp::SetFixedDeltaTime(1.0 / FramesPerSecond);
	FApp::SetUseFixedTimeStep(true);
	bVideoCapturing = true;
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepVideo start dir=%s size=%dx%d fps=%.3f fov=%.3f ev100=%.3f"), *Directory, Width, Height, FramesPerSecond, FovDegrees, ExposureBias);
	return true;
}

void AIMAcousticFootstepCharacter::CaptureVideoFrame()
{
	check(IsInGameThread());
	if (!bVideoCapturing || !VideoCapture || !VideoTarget || !ListenerCamera) return;
	VideoCapture->SetWorldLocationAndRotation(ListenerCamera->GetComponentLocation(), ListenerCamera->GetComponentRotation());
	VideoCapture->CaptureScene();
	TArray<FColor> Pixels;
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
	ReadFlags.SetLinearToGamma(false);
	FTextureRenderTargetResource* Resource = VideoTarget->GameThread_GetRenderTargetResource();
	if (!Resource || !Resource->ReadPixels(Pixels, ReadFlags) || Pixels.Num() != VideoWidth * VideoHeight)
	{
		UE_LOG(LogTemp, Error, TEXT("IMLogs FootstepVideo read_failed frame=%d pixels=%d"), VideoFrameIndex, Pixels.Num());
		return;
	}
	for (FColor& Pixel : Pixels) Pixel.A = 255;
	// Bounded queue: the game thread waits when eight encodes are pending, so memory stays at a few frames.
	while (VideoWritesInFlight->load() >= IMMaxVideoWritesInFlight) FPlatformProcess::Sleep(.002f);
	VideoWritesInFlight->fetch_add(1);
	const FString File = FPaths::Combine(VideoDirectory, FString::Printf(TEXT("frame_%05d.png"), VideoFrameIndex));
	Async(EAsyncExecution::ThreadPool, [Pixels = MoveTemp(Pixels), File, Width = VideoWidth, Height = VideoHeight, InFlight = VideoWritesInFlight]()
	{
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		if (!FFileHelper::SaveArrayToFile(Png, *File)) UE_LOG(LogTemp, Error, TEXT("IMLogs FootstepVideo write_failed file=%s"), *File);
		InFlight->fetch_sub(1);
	});
	// One CSV row per frame: game time, replay time, body, both soles (component position is what is drawn), camera, events emitted on this tick.
	FString Emitted;
	for (const int32 Index : TickEmittedEvents)
	{
		if (!AuditionEvents.IsValidIndex(Index)) continue;
		if (!Emitted.IsEmpty()) Emitted += TEXT(";");
		Emitted += FString::Printf(TEXT("%d:%d:%d"), Index, AuditionEvents[Index].Foot, AuditionEvents[Index].bLanding ? 1 : 0);
	}
	const FVector Body = GetActorLocation(), LeftSole = Feet[0]->GetComponentLocation(), RightSole = Feet[1]->GetComponentLocation();
	const FVector CameraLocation = ListenerCamera->GetComponentLocation();
	VideoFrameRows.Add(FString::Printf(TEXT("%d,%.9f,%.9f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s"),
		VideoFrameIndex, GetWorld()->GetTimeSeconds(), GetWorld()->GetTimeSeconds() - AuditionStartedSeconds, Body.X, Body.Y, Body.Z,
		LeftSole.X, LeftSole.Y, LeftSole.Z, RightSole.X, RightSole.Y, RightSole.Z, CameraLocation.X, CameraLocation.Y, *Emitted));
	++VideoFrameIndex;
}

void AIMAcousticFootstepCharacter::StopVideoCapture()
{
	if (!bVideoCapturing) return;
	bVideoCapturing = false;
	FApp::SetUseFixedTimeStep(bVideoPreviousFixed);
	FApp::SetFixedDeltaTime(VideoPreviousFixedDelta);
	const double Waiting = FPlatformTime::Seconds();
	while (VideoWritesInFlight->load() > 0 && FPlatformTime::Seconds() - Waiting < 120.) FPlatformProcess::Sleep(.01f);
	const bool bWritten = VideoWritesInFlight->load() == 0;
	FString Csv = TEXT("frame,world_seconds,replay_seconds,body_x,body_y,body_z,left_sole_x,left_sole_y,left_sole_z,right_sole_x,right_sole_y,right_sole_z,camera_x,camera_y,events\n");
	Csv += FString::Join(VideoFrameRows, TEXT("\n")) + TEXT("\n");
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(VideoDirectory, TEXT("video_frames.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepVideo stop frames=%d writes_complete=%d"), VideoFrameIndex, bWritten);
	if (VideoCapture) VideoCapture->DestroyComponent();
	VideoCapture = nullptr;
	VideoTarget = nullptr;
}

void AIMAcousticFootstepCharacter::AdvanceWalking(float DeltaSeconds)
{
	const FVector Location = GetActorLocation();
	const double Distance = FVector::Dist2D(Location, PreviousLocation);
	PreviousLocation = Location;
	bGroundedMoving = GetCharacterMovement()->IsMovingOnGround() && GetVelocity().Size2D() > 2.f
		&& Distance > .001 && DeltaSeconds > 0;
	if (!bGroundedMoving)
	{
		for (int32 Foot = 0; Foot < 2; ++Foot)
		{
			FHitResult Hit;
			if (TraceSole(Foot, Hit)) PlantedFeet[Foot] = Hit.ImpactPoint + FVector(0, 0, IMFootHeightCm);
		}
		UpdateVisiblePose();
		return;
	}
	const double Before = GaitPhase;
	GaitPhase += Distance / IMStrideCm;
	const int32 PreviousHalfStep = FMath::FloorToInt(Before * 2.);
	const int32 NewHalfStep = FMath::FloorToInt(GaitPhase * 2.);
	UpdateVisiblePose();
	// 一个实际画面最多一次可见落脚；卡顿不补播一串没有对应画面的历史脚步。
	if (NewHalfStep != PreviousHalfStep)
	{
		const int32 Foot = (NewHalfStep & 1) ? 0 : 1;
		// 接触后让落地瞬态完整发出，避免同帧的普通步伐重触发切掉它。
		if (GetWorld()->GetTimeSeconds() - LastLandingSeconds >= IMLandingContactGuardSeconds)
			EmitSoleContact(Foot);
		if (NewHalfStep - PreviousHalfStep > 1)
			UE_LOG(LogTemp, Warning, TEXT("IMLogs FootstepDemo skipped_hitch_contacts=%d"), NewHalfStep-PreviousHalfStep-1);
	}
}

bool AIMAcousticFootstepCharacter::TraceSole(int32 Foot, FHitResult& Hit) const
{
	const float Side = Foot ? 1.f : -1.f;
	const FVector Local(IMToeReachCm, Side * IMFootSeparationCm, -90.f);
	const FVector Sole = GetActorTransform().TransformPositionNoScale(Local);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(IMAcousticFootstepSole), false, this);
	Query.bReturnPhysicalMaterial = true;
	return GetWorld()->LineTraceSingleByChannel(Hit, Sole + FVector(0,0,35), Sole - FVector(0,0,55), ECC_Visibility, Query)
		&& Hit.IsValidBlockingHit() && Hit.ImpactNormal.Z >= GetCharacterMovement()->GetWalkableFloorZ();
}

void AIMAcousticFootstepCharacter::UpdateLeg(int32 Foot, const FVector& Position)
{
	Feet[Foot]->SetWorldLocation(Position);
	Feet[Foot]->SetWorldRotation(GetActorRotation());
	const FVector Hip = GetActorTransform().TransformPositionNoScale(FVector(0, (Foot?1.f:-1.f)*IMFootSeparationCm, -27.f));
	const FVector Axis = Hip - Position;
	Legs[Foot]->SetWorldLocation((Hip + Position) * .5);
	Legs[Foot]->SetWorldRotation(FRotationMatrix::MakeFromZ(Axis).Rotator());
	Legs[Foot]->SetWorldScale3D(FVector(.10f, .10f, Axis.Size()/100.f));
}

void AIMAcousticFootstepCharacter::UpdateVisiblePose()
{
	if (GetCharacterMovement()->IsFalling())
	{
		for (int32 Foot = 0; Foot < 2; ++Foot)
		{
			const FVector Position = GetActorTransform().TransformPositionNoScale(
				FVector(0, (Foot ? 1.f : -1.f) * IMFootSeparationCm, -82.f));
			UpdateLeg(Foot, Position);
			Arms[Foot]->SetRelativeRotation(FRotator(-12.f, 0, 0));
		}
		return;
	}
	for (int32 Foot = 0; Foot < 2; ++Foot)
	{
		const double Phase = FMath::Frac(GaitPhase + (Foot ? .5 : 0.));
		FVector Position = PlantedFeet[Foot];
		if (bGroundedMoving && Phase < .5)
		{
			FHitResult Hit;
			if (TraceSole(Foot, Hit))
			{
				const FVector Target = Hit.ImpactPoint + FVector(0,0,IMFootHeightCm);
				Position = FMath::Lerp(PlantedFeet[Foot], Target, float(Phase*2.));
				Position.Z += IMFootLiftCm * FMath::Sin(float(Phase*2.*PI));
			}
		}
		UpdateLeg(Foot, Position);
		Arms[Foot]->SetRelativeRotation(FRotator(bGroundedMoving ? 20.f * FMath::Sin(float((GaitPhase+(Foot?.5:0.))*2.*PI)) : 0,0,0));
	}
}

void AIMAcousticFootstepCharacter::EmitPendingLandingFoot()
{
	if (PendingSecondFootSeconds < 0.0 || GetWorld()->GetTimeSeconds() < PendingSecondFootSeconds) return;
	PendingSecondFootSeconds = -1.0;
	FHitResult Hit;
	if (!TraceSole(1, Hit)) return;
	PlantedFeet[1] = Hit.ImpactPoint + FVector(0, 0, IMFootHeightCm);
	UpdateLeg(1, PlantedFeet[1]);
	EmitSurfaceSound(Hit, 1, true, PendingSecondFootGain, 0.f);
}

void AIMAcousticFootstepCharacter::EmitSoleContact(int32 Foot)
{
	FHitResult Hit;
	if (!TraceSole(Foot, Hit)) return;
	PlantedFeet[Foot] = Hit.ImpactPoint + FVector(0, 0, IMFootHeightCm);
	UpdateLeg(Foot, PlantedFeet[Foot]);
	const float Speed = GetVelocity().Size2D();
	const float Strength = FMath::Clamp(Speed / IMWalkSpeedCm, .35f, 1.15f);
	const float Pitch = FMath::Clamp((Speed - IMWalkSpeedCm) / IMWalkSpeedCm, -.25f, .5f);
	if (!EmitSurfaceSound(Hit, Foot, false, Strength, Pitch)) return;
	++ContactCount;
	const bool bWood = Hit.PhysMaterial.Get() == WoodPhysicalMaterial;
	if (bWood) ++WoodContactCount; else ++StoneContactCount;
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	IM_RecordOrdinaryFootstepSourceEvent(*this, false, ContactCount, Hit.PhysMaterial.Get(),
		FootstepAudio->GetAudioComponentID(), Strength, Pitch);
#endif
	UE_LOG(LogTemp, Display, TEXT("IMLogs FootstepDemo contact=%d time=%.6f foot=%d material=%s surface=%s clip=%s speed_cm_s=%.3f gain=%.6f pitch_st=%.6f sole=%s source=%s audio=%llu"),
		ContactCount, GetWorld()->GetTimeSeconds(), Foot, *GetPathNameSafe(Hit.PhysMaterial.Get()), bWood ? TEXT("wood") : TEXT("stone"),
		*((bWood ? WoodSteps : StoneSteps)[(ContactCount - 1) % (bWood ? WoodSteps.Num() : StoneSteps.Num())])->GetPathName(), Speed, Strength, Pitch, *Hit.ImpactPoint.ToString(),
		*FootstepAudio->GetComponentLocation().ToString(), FootstepAudio->GetAudioComponentID());
}

bool AIMAcousticFootstepCharacter::EmitSurfaceSound(const FHitResult& Hit, int32 Foot, bool bLanding, float Gain, float Pitch)
{
	const UPhysicalMaterial* PhysicalMaterial = Hit.PhysMaterial.Get();
	const bool bWood = PhysicalMaterial && PhysicalMaterial == WoodPhysicalMaterial;
	const bool bStone = PhysicalMaterial && PhysicalMaterial == StonePhysicalMaterial;
	if (!bWood && !bStone)
	{
		++UnknownContactCount;
		UE_LOG(LogTemp, Warning, TEXT("IMLogs FootstepDemo unmapped_contact material=%s component=%s"),
			*GetPathNameSafe(PhysicalMaterial), *GetPathNameSafe(Hit.Component.Get()));
		return false;
	}
	const auto& Clips = bWood ? WoodSteps : StoneSteps;
	if (Clips.IsEmpty() || !FootstepAudio->IsPlaying()) return false;
	// 落地是两只脚各一次该材质的普通脚步：与普通脚步一样轮换素材，落地脚步另计数以免打乱步行序列。
	USoundWave* CanonicalWave = Clips[(ContactCount + (bLanding ? LandingStepCount : 0)) % Clips.Num()];
	if (bLanding) ++LandingStepCount;
	const auto& PresenceClips = bWood ? PresenceWoodSteps : PresenceStoneSteps;
	USoundWave* Wave = bExaggeratedPresence && bSynthesizedPresenceSamples && PresenceClips.Num() == 2
		? PresenceClips[bLanding ? 1 : ContactCount % PresenceClips.Num()].Get() : CanonicalWave;
	if (!Wave || Wave->NumChannels != 1) return false;
	// 同一GT事务先发布素材/实时参数/位置，再触发；既有MetaSound和环境卷积Owner不变。
	FootstepAudio->AttachToComponent(Feet[Foot], FAttachmentTransformRules::KeepWorldTransform);
	FootstepAudio->SetWorldLocation(Hit.ImpactPoint + FVector(0, 0, 14));
	FootstepAudio->SetWaveParameter(TEXT("Footstep Wave"), Wave);
	FootstepAudio->SetFloatParameter(TEXT("Footstep Gain"), Gain);
	FootstepAudio->SetFloatParameter(TEXT("Footstep Pitch"), Pitch);
	FootstepAudio->SetFloatParameter(TEXT("Presence Material"), bStone ? 1.f : 0.f);
	const bool SelfNearField = bExaggeratedPresence && IsPlayerControlled();
	FootstepAudio->SetFloatParameter(TEXT("Presence Near Field"), SelfNearField ? 1.f : 0.f);
	PublishPresenceFlags();
	PublishPresenceLateral(Hit.ImpactPoint);
	FootstepAudio->SetFloatParameter(TEXT("Presence Pan"), Foot ? .12f : -.12f);
	const float Impact = AuditionImpactOverride >= 0.f ? AuditionImpactOverride : (bLanding
		? FMath::Clamp(LastLandingImpactCmS / 420.f, 0.f, 1.5f)
		: .35f * FMath::Clamp(GetVelocity().Size2D() / 180.f, 0.f, 1.f));
	FootstepAudio->SetFloatParameter(TEXT("Presence Impact"), Impact);
	FootstepAudio->SetTriggerParameter(TEXT("Footstep Contact"));
	if (bAuditionRecording || bAuditionReplaying)
	{
		FIMAcousticAuditionEvent Event;
		Event.Seconds = bAuditionReplaying ? AuditionEvents[AuditionEventIndex].Seconds : GetWorld()->GetTimeSeconds() - AuditionStartedSeconds;
		Event.Hit = Hit;
		Event.Foot = Foot;
		Event.bLanding = bLanding;
		Event.Gain = Gain;
		Event.Pitch = Pitch;
		Event.Impact = Impact;
		Event.Wave = Wave->GetPathName();
		Event.CanonicalWave = GetPathNameSafe(CanonicalWave);
		Event.bStone = bStone;
		Event.bSelfNearField = SelfNearField;
		Event.Enclosure = LastPresenceEnclosure;
		PlayedAuditionEvents.Add(Event);
		if (bAuditionRecording) AuditionEvents.Add(Event);
	}
	return true;
}
