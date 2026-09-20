// See IMAcousticLiveTest.h. Phases: 0 build+probes, 1 settle, 2 table timing, 3 Smooth trajectory.
// Markers: IMACOUSTIC_LIVE_* parsed offline. All queries run on the GameThread in PIE.

#include "IMAcousticLiveTest.h"
#include "IMAcousticFieldActor.h"
#include "IMAcousticTestSceneBuilder.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

namespace
{
	// Deterministic stratified table: room tag per point (A/COR/B/Cwing/Maze/Yard counts).
	const int32 IMTableRooms[100] = {
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 30 A
		1,1,1,1,1,1,1,1,1,1, // 10 corridor
		2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // 30 B
		3,3,3,3,3,3,3,3,3,3, // 10 C wing
		4,4,4,4,4,4,4,4,4,4, // 10 maze
		5,5,5,5,5,5,5,5,5,5, // 10 yard
	};
	// Room boxes in cm: {Min, Max}.
	const FVector IMRoomMin[6] = {
		FVector(0, 0, 30), FVector(1000, 200, 30), FVector(1600, 0, 30),
		FVector(1000, 400, 30), FVector(-800, 0, 30), FVector(2400, 0, 30),
	};
	const FVector IMRoomMax[6] = {
		FVector(1000, 600, 270), FVector(1600, 400, 270), FVector(2400, 600, 270),
		FVector(1600, 1000, 270), FVector(0, 600, 270), FVector(3400, 600, 270),
	};
}

AIM_AcousticLiveTest::AIM_AcousticLiveTest()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AIM_AcousticLiveTest::BeginPlay()
{
	Super::BeginPlay();
	if (!bRunOnBeginPlay) { return; }
	UWorld* World = GetWorld();
	// 注意：IsEditorWorld() 对 PIE 也返回 true，此处必须用 IsGameWorld()（World.cpp:9318）。
	if (!World || !World->IsGameWorld()) { return; }

	UE_LOG(LogTemp, Log, TEXT("IMACOUSTIC_LIVE_BEGIN {\"map\":\"%s\"}"), *GetWorld()->GetMapName());

	// Build whitebox scene in this world (same process, no MCP round-trip).
	AIM_AcousticTestSceneBuilder* Builder = World->SpawnActor<AIM_AcousticTestSceneBuilder>();
	if (ensure(Builder))
	{
		Builder->BuildTestScene();
		Builder->Destroy();
	}

	// Ensure field actor exists, then fire probes at every table point.
	AIceMoonAcousticField* Field = AIceMoonAcousticField::GetAcousticFieldActor(this);
	if (!ensure(Field)) { return; }
	FRandomStream Rng(908);
	int32 Fired = 0;
	for (int32 i = 0; i < 100; ++i)
	{
		const int32 Room = IMTableRooms[i];
		const FVector P(
			Rng.FRandRange(IMRoomMin[Room].X, IMRoomMax[Room].X),
			Rng.FRandRange(IMRoomMin[Room].Y, IMRoomMax[Room].Y),
			Rng.FRandRange(IMRoomMin[Room].Z, IMRoomMax[Room].Z));
		Field->AsyncFireProbes(P, 16, 2000.0f);
		++Fired;
	}
	UE_LOG(LogTemp, Log, TEXT("IMACOUSTIC_LIVE_PROBES {\"fired\":%d}"), Fired);
	TestPhase = 1;
	PhaseTime = 0.0f;
}

void AIM_AcousticLiveTest::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (TestPhase < 1) { return; }
	PhaseTime += DeltaTime;

	if (TestPhase == 1)
	{
		// Settle: let async delegates drain through field Tick (5s).
		if (PhaseTime >= 5.0f)
		{
			RunTableTiming();
			TestPhase = 2;
			PhaseTime = 0.0f;
		}
		return;
	}
	if (TestPhase == 2)
	{
		// Smooth trajectory: one cross-room step per 0.5s, 10 steps A->B.
		if (PhaseTime >= 0.5f)
		{
			PhaseTime = 0.0f;
			RunSmoothStep();
			if (SmoothStep >= 25)
			{
				UE_LOG(LogTemp, Log, TEXT("IMACOUSTIC_LIVE_END {}"));
				TestPhase = 3;
			}
		}
	}
}

void AIM_AcousticLiveTest::RunTableTiming()
{
	AIceMoonAcousticField* Field = AIceMoonAcousticField::GetAcousticFieldActor(this);
	if (!Field) { return; }
	// Rebuild identical table (same seed) for measurement.
	FRandomStream Rng(908);
	TArray<double> Samples;
	Samples.Reserve(100);
	for (int32 i = 0; i < 100; ++i)
	{
		const int32 Room = IMTableRooms[i];
		const FVector P(
			Rng.FRandRange(IMRoomMin[Room].X, IMRoomMax[Room].X),
			Rng.FRandRange(IMRoomMin[Room].Y, IMRoomMax[Room].Y),
			Rng.FRandRange(IMRoomMin[Room].Z, IMRoomMax[Room].Z));
		FIM_AudioReverbParameters Ignored;
		const uint64 T0 = FPlatformTime::Cycles64();
		Field->QueryAcousticField(P, Ignored);
		const uint64 T1 = FPlatformTime::Cycles64();
		Samples.Add(static_cast<double>(T1 - T0) * FPlatformTime::GetSecondsPerCycle64() * 1e6);
	}
	Samples.Sort();
	const double P50 = Samples[50];
	const double P95 = Samples[95];
	UE_LOG(LogTemp, Log, TEXT("IMACOUSTIC_LIVE_TABLE {\"n\":100,\"p50_us\":%.2f,\"p95_us\":%.2f}"), P50, P95);
}

void AIM_AcousticLiveTest::RunSmoothStep()
{
	AIceMoonAcousticField* Field = AIceMoonAcousticField::GetAcousticFieldActor(this);
	if (!Field) { return; }
	// Fixed 25-point trajectory A(200,300,150) -> B(2200,300,150) in cm, then dwell:
	// steps 0-6 lerp, steps 7-24 repeat B endpoint (~9s ~= 3tau at tau=3s) so the
	// 5%-of-range steady gate is observable instead of extrapolated.
	FVector P;
	if (SmoothStep < 7)
	{
		const float T = static_cast<float>(SmoothStep) / 6.0f;
		P = FVector(200.0f, 300.0f, 150.0f) + (FVector(2200.0f, 300.0f, 150.0f) - FVector(200.0f, 300.0f, 150.0f)) * T;
	}
	else
	{
		P = FVector(2200.0f, 300.0f, 150.0f);
	}
	FIM_AudioReverbParameters Raw;
	Field->QueryAcousticField(P, Raw);
	FIM_AudioReverbParameters Smooth;
	Field->QueryAcousticFieldSmooth(this, FName(TEXT("LiveTrajectory")), P, Smooth, 3.0f);
	UE_LOG(LogTemp, Log, TEXT("IMACOUSTIC_LIVE_SMOOTH {\"step\":%d,\"raw_wet\":%.4f,\"smooth_wet\":%.4f}"),
		SmoothStep, Raw.Wet, Smooth.Wet);
	++SmoothStep;
}
