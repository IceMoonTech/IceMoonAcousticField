#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "GameFramework/Character.h"
#include <atomic>
#include "IMAcousticFootstepCharacter.generated.h"

class IConsoleVariable;
class UAudioComponent;
class UCameraComponent;
class UIMAcousticSourceComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UPhysicalMaterial;
class USoundBase;
class USoundWave;
class UStaticMeshComponent;

struct FIMAcousticAuditionEvent
{
	double Seconds = 0.;
	FHitResult Hit;
	int32 Foot = 0;
	bool bLanding = false;
	float Gain = 0.f, Pitch = 0.f, Impact = 0.f;
	FString Wave;
	FString CanonicalWave;
	bool bStone = false, bSelfNearField = false;
	float Enclosure = 0.f;
};

struct FIMAcousticAuditionPose
{
	double Seconds = 0.;
	FTransform Transform;
	FVector Feet[2];
	FRotator Arms[2];
	float Enclosure = 0.f;
};

// 原生白模角色用于普通 Play 演示；脚步由可见脚掌的落地相位触发。
// UObject、碰撞材质和音频参数都由 GT 拥有，已有声场组件拥有传播/DSP。
UCLASS()
class ICEMOONACOUSTICFIELD_API AIMAcousticFootstepCharacter final : public ACharacter
{
	GENERATED_BODY()
public:
	AIMAcousticFootstepCharacter();
	virtual void Tick(float DeltaSeconds) override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	TObjectPtr<USoundBase> FootstepSound;
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	TObjectPtr<UPhysicalMaterial> WoodPhysicalMaterial;
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	TObjectPtr<UPhysicalMaterial> StonePhysicalMaterial;
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	TArray<TObjectPtr<USoundWave>> WoodSteps;
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	TArray<TObjectPtr<USoundWave>> StoneSteps;
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	TObjectPtr<AActor> ObserverCamera;
	// Scripted walk: world-space waypoints (cm, Z ignored) visited in order while auto walk is on. Empty = the old straight patrol.
	// The route is the only author of motion in the playable scene; WASD still takes over, Space resumes at the current waypoint.
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps|Route")
	TArray<FVector> RouteWaypoints;
	// Waypoint indices at which the character jumps on arrival; the strong subset uses the heavy jump.
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps|Route")
	TArray<int32> RouteJumpWaypoints;
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps|Route")
	TArray<int32> RouteStrongJumpWaypoints;
	// View from the audio listener pose (3 m behind, 0.6 m above), so what is seen is where it is heard from.
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps|Route")
	bool bFollowListenerCamera = false;
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	bool bAutoWalk = true;
	// 普通Play的演示行为；关闭后仍可用J/Shift+J触发实际跳跃。
	UPROPERTY(EditInstanceOnly, Category="IM|Footsteps")
	bool bAutoLandingDemo = true;
	UPROPERTY(EditAnywhere, Category="IM|Footsteps")
	bool bExaggeratedPresence = true;
	void SetExaggeratedPresence(bool bEnabled);
	// Exaggeration strength S of the exaggerated presence version: 0 = physical path, 1 = accepted exaggerated values (mapping in
	// IMAcousticPresenceParameters.h), up to 2. Drives the console variable IM.AcousticField.PresenceStrength, and a change made by that
	// variable is reflected back here. The 0..2 range is the UI/console range, not a safety guard. Physical (non-exaggerated) sources are unaffected.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="IM|Acoustic", meta=(ClampMin="0.0", ClampMax="2.0", UIMin="0.0", UIMax="2.0"))
	float PresenceStrength = 1.f;
	void SetEqualLoudness(bool bEnabled);
	// Listening comparison only: the exaggerated pass may use the synthesized SW_IMFootstepPresence* waves
	// instead of the recorded footstep waves. Default is the recorded waves, so A/B differs only by processing.
	void SetSynthesizedPresenceSamples(bool bEnabled);
	// Replaces the equal-loudness gain read from the calibration file; 0 restores the file value.
	void SetPresenceCompensationOverride(float Gain);
	// Route-fitted silent video of a replay: one PNG per fixed-step game tick (frame N shows the pose at replay time FrameSeconds[N]).
	bool StartVideoCapture(const FString& Directory, int32 Width, int32 Height, float FramesPerSecond, float FovDegrees, float ExposureBias);
	void StopVideoCapture();
	bool IsVideoCapturing() const { return bVideoCapturing; }
	void BeginAuditionPass(bool bRecord, const FString& Label);
	void PauseAuditionPass();
	void EndAuditionPass();

	int32 GetLandingCount() const { return LandingCount; }
	int32 GetWoodLandingCount() const { return WoodLandingCount; }
	int32 GetStoneLandingCount() const { return StoneLandingCount; }
	float GetLastLandingImpactCmS() const { return LastLandingImpactCmS; }
	float GetLastLandingGain() const { return LastLandingGain; }
	float GetLastLandingPitch() const { return LastLandingPitch; }
	double GetLastLandingSeconds() const { return LastLandingSeconds; }
	int32 GetManualJumpPressCount() const { return ManualJumpPressCount; }
	int32 GetManualJumpReleaseCount() const { return ManualJumpReleaseCount; }
	int32 GetManualJumpStartCount() const { return ManualJumpStartCount; }
	bool WasLastManualJumpStrong() const { return bLastManualJumpStrong; }
	float GetLastManualJumpStartSpeedCmS() const { return LastManualJumpStartSpeedCmS; }
	bool WasObserverFrameApplied() const { return bObserverFrameApplied; }
	int32 GetContactCount() const { return ContactCount; }
	int32 GetWoodContactCount() const { return WoodContactCount; }
	int32 GetStoneContactCount() const { return StoneContactCount; }
	int32 GetUnknownContactCount() const { return UnknownContactCount; }
	UAudioComponent* GetFootstepAudio() const { return FootstepAudio.Get(); }
	void SetAutoWalking(bool bEnabled);

protected:
	virtual void BeginPlay() override;
	virtual void OnJumped_Implementation() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void ForwardDown();
	void ForwardUp();
	void BackwardDown();
	void BackwardUp();
	void RightDown();
	void RightUp();
	void LeftDown();
	void LeftUp();
	void ToggleAutoWalking();
	void TogglePresence();
	void ToggleEqualLoudness();
	void PublishPresenceHint();
	void PublishPresenceFlags();
	void PublishPresenceLateral(const FVector& FootLocation);
	void AdvanceAuditionReplay();
	void AdvanceRoute();
	void CaptureVideoFrame();
	void UpdateAuditionListener();
	void UpdatePresenceParameters();
	void SyncPresenceStrength();
	void BeginRunning();
	void EndRunning();
	void JumpDown();
	void JumpUp();
	void RequestVisibleJump(bool bStrong);
	void AdvanceWalking(float DeltaSeconds);
	void UpdateVisiblePose();
	void EmitSoleContact(int32 FootIndex);
	void EmitPendingLandingFoot();
	bool EmitSurfaceSound(const FHitResult& Hit, int32 Foot, bool bLanding, float Gain, float Pitch);
	bool TraceSole(int32 FootIndex, FHitResult& Hit) const;
	void UpdateLeg(int32 FootIndex, const FVector& FootPosition);

	UPROPERTY()
	TObjectPtr<UAudioComponent> FootstepAudio;
	UPROPERTY()
	TObjectPtr<UIMAcousticSourceComponent> AcousticSource;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundWave>> PresenceWoodSteps;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundWave>> PresenceStoneSteps;
	UPROPERTY()
	TObjectPtr<UCameraComponent> ListenerCamera;
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> VideoCapture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> VideoTarget;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Feet;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Legs;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Arms;

	FVector PreviousLocation = FVector::ZeroVector;
	FVector PatrolOrigin = FVector::ZeroVector;
	FVector PlantedFeet[2];
	double GaitPhase = 0.0;
	double StartedAtSeconds = 0.0;
	float PatrolDirection = 1.0f;
	bool bRunning = false;
	bool bForwardHeld = false;
	bool bBackwardHeld = false;
	bool bRightHeld = false;
	bool bLeftHeld = false;
	bool bGroundedMoving = false;
	double NextDemoJumpSeconds = 0.0;
	double LastLandingSeconds = -1000.0;
	bool bNextDemoStrong = false;
	float LastLandingImpactCmS = 0.f, LastLandingGain = 0.f, LastLandingPitch = 0.f;
	int32 LandingCount = 0, WoodLandingCount = 0, StoneLandingCount = 0;
	// Landing footsteps emitted so far (two per landing); picks the sample like ContactCount does for walking steps.
	int32 LandingStepCount = 0;
	// World time at which the second foot of the current landing touches down (negative: none pending) and the gain it shares with the first foot.
	double PendingSecondFootSeconds = -1.0;
	float PendingSecondFootGain = 0.f;
	int32 ManualJumpPressCount = 0, ManualJumpReleaseCount = 0, ManualJumpStartCount = 0;
	bool bManualJumpPending = false, bLastManualJumpStrong = false;
	bool bObserverFrameApplied = false;
	float LastManualJumpStartSpeedCmS = 0.f;
	int32 ContactCount = 0;
	int32 WoodContactCount = 0;
	int32 StoneContactCount = 0;
	int32 UnknownContactCount = 0;
	double NextPresenceProbeSeconds = 0.;
	bool bPublishedPresence = false;
	// Last strength this actor wrote to or read from the console variable (negative until the first sync).
	float PublishedPresenceStrength = -1.f;
	IConsoleVariable* PresenceStrengthVariable = nullptr;
	bool bEqualLoudness = false;
	bool bSynthesizedPresenceSamples = false;
	float PresenceCompensation = 1.f;
	float LastPresenceEnclosure = 0.f;
	bool bAuditionRecording = false, bAuditionReplaying = false;
	double AuditionStartedSeconds = 0.;
	int32 AuditionPoseIndex = 0, AuditionEventIndex = 0;
	float AuditionImpactOverride = -1.f;
	FString AuditionLabel;
	TArray<FIMAcousticAuditionPose> AuditionPoses;
	TArray<FIMAcousticAuditionEvent> AuditionEvents;
	TArray<FIMAcousticAuditionEvent> PlayedAuditionEvents;
	// Index of the waypoint being walked to (== waypoint count once the route is complete); read by the capture script to end the recording.
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category="IM|Footsteps|Route", meta=(AllowPrivateAccess="true"))
	int32 RouteIndex = 0;
	float PresenceCompensationOverride = 0.f;
	// Video capture state; all of it belongs to the game thread except the PNG encode tasks, which own their pixel copies.
	bool bVideoCapturing = false;
	FString VideoDirectory;
	int32 VideoWidth = 0, VideoHeight = 0, VideoFrameIndex = 0;
	double VideoPreviousFixedDelta = 0.;
	bool bVideoPreviousFixed = false;
	TArray<int32> TickEmittedEvents;
	TArray<FString> VideoFrameRows;
	TSharedPtr<std::atomic<int32>> VideoWritesInFlight;
};
