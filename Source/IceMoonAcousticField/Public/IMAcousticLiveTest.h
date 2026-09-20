// Live self-test: drives probes + timing + Smooth trajectory in PIE and logs markers.
// Inert unless bRunOnBeginPlay is true (set on the placed instance). Test-only tooling.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IMAcousticLiveTest.generated.h"

UCLASS(Blueprintable, BlueprintType)
class ICEMOONACOUSTICFIELD_API AIM_AcousticLiveTest : public AActor
{
	GENERATED_BODY()

public:
	AIM_AcousticLiveTest();

	// Gate to run: default false so manual maps are unaffected.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	bool bRunOnBeginPlay = false;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	int32 TestPhase = -1;
	float PhaseTime = 0.0f;
	int32 SmoothStep = 0;
	void RunTableTiming();
	void RunSmoothStep();
};
