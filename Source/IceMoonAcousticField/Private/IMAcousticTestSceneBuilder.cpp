// See IMAcousticTestSceneBuilder.h. Frozen P0 map, meters. UE conversion (x100) at spawn.

#include "IMAcousticTestSceneBuilder.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Tag shared by every spawned whitebox; the only cleanup key.
	const FName IMWhiteboxTag(TEXT("IMAcousticWhitebox"));
	// Editor folder for spawned actors.
	const FName IMWhiteboxFolder(TEXT("AcousticTest"));
	// Wall thickness in meters.
	constexpr float IMWallT = 0.2f;
}

AIM_AcousticTestSceneBuilder::AIM_AcousticTestSceneBuilder()
{
	PrimaryActorTick.bCanEverTick = false;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeRef(TEXT("/Engine/BasicShapes/Cube.Cube"));
	WhiteboxCube = CubeRef.Object;
}

void AIM_AcousticTestSceneBuilder::BuildTestScene()
{
#if WITH_EDITOR
	ClearTestScene();
	if (!ensure(WhiteboxCube)) { return; }

	// --- A room x[0,10] y[0,6] h3, wood floor. D1 in east wall x=10, gap y[2.4,3.6]. ---
	AddBox(FVector(5.0f, 3.0f, -0.1f), FVector(10.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Wood); // floor
	AddBox(FVector(5.0f, 3.0f, 3.1f), FVector(10.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Wood); // ceiling
	AddWallY(0.0f, 0.0f, 10.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Wood);
	AddWallY(6.0f, 0.0f, 10.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Wood);
	AddWallX(0.0f, 0.0f, 6.0f, 0.0f, 3.0f, { FVector2D(2.4f, 3.6f) }, EIM_WhiteboxZone::Wood); // 迷宫门
	AddWallX(10.0f, 0.0f, 6.0f, 0.0f, 3.0f, { FVector2D(2.4f, 3.6f) }, EIM_WhiteboxZone::Wood);

	// --- Corridor x[10,16] y[2,4] h3, concrete. D2 in east wall x=16, gap y[2.4,3.6]. ---
	AddBox(FVector(13.0f, 3.0f, -0.1f), FVector(6.0f, 2.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddBox(FVector(13.0f, 3.0f, 3.1f), FVector(6.0f, 2.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddWallY(2.0f, 10.0f, 16.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(4.0f, 10.0f, 16.0f, 0.0f, 3.0f, {{12.4f, 13.6f}}, EIM_WhiteboxZone::Concrete); // C-wing door
	AddWallX(16.0f, 2.0f, 4.0f, 0.0f, 3.0f, { FVector2D(2.4f, 3.6f) }, EIM_WhiteboxZone::Concrete);

	// --- B room x[16,24] y[0,6] h3, carpet. Wide opening east x=24, gap y[2,4] to yard. ---
	AddBox(FVector(20.0f, 3.0f, -0.1f), FVector(8.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Carpet);
	AddBox(FVector(20.0f, 3.0f, 3.1f), FVector(8.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Carpet);
	AddWallY(0.0f, 16.0f, 24.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Carpet);
	AddWallY(6.0f, 16.0f, 24.0f, 0.0f, 3.0f, { FVector2D(17.4f, 18.6f) }, EIM_WhiteboxZone::Carpet); // 吸声房门
	AddWallX(24.0f, 0.0f, 6.0f, 0.0f, 3.0f, { FVector2D(2.0f, 4.0f) }, EIM_WhiteboxZone::Carpet);
	// West wall x=16 of B room is shared with corridor mouth; fill y[0,2] and y[4,6].
	AddWallX(16.0f, 0.0f, 2.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Carpet);
	AddWallX(16.0f, 4.0f, 6.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Carpet);
	// Pillars in B (fragmentation test).
	AddBox(FVector(19.0f, 2.0f, 1.5f), FVector(0.4f, 0.4f, 3.0f), EIM_WhiteboxZone::Concrete);
	AddBox(FVector(21.0f, 4.0f, 1.5f), FVector(0.4f, 0.4f, 3.0f), EIM_WhiteboxZone::Concrete);

	// --- C wing x[10,16] y[4,10] h3, concrete. ---
	AddBox(FVector(13.0f, 7.0f, -0.1f), FVector(6.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddBox(FVector(13.0f, 7.0f, 3.1f), FVector(6.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddWallX(10.0f, 4.0f, 10.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallX(16.0f, 4.0f, 10.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(10.0f, 10.0f, 16.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	// South wall y=4 shared with corridor north wall (already built with C door gap above).

	// --- Furniture in A (wood). ---
	AddBox(FVector(3.0f, 4.5f, 0.45f), FVector(1.5f, 0.8f, 0.9f), EIM_WhiteboxZone::Wood);

	// --- Open yard x[24,34] y[0,6], concrete floor, no ceiling (open-space fallback test). ---
	AddBox(FVector(29.0f, 3.0f, -0.1f), FVector(10.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Concrete);

	// --- Maze annex x[-8,0] y[0,6] h3, concrete (pathological diffraction test). ---
	AddBox(FVector(-4.0f, 3.0f, -0.1f), FVector(8.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddBox(FVector(-4.0f, 3.0f, 3.1f), FVector(8.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddWallX(-8.0f, 0.0f, 6.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(0.0f, -8.0f, 0.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(6.0f, -8.0f, 0.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	// Serpentine baffles (east wall x=0 shared with A room, door gap already cut above).
	AddWallY(2.0f, -8.0f, -3.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(4.0f, -5.0f, 0.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallX(-3.0f, 2.0f, 4.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);

	// --- Coupled pair north of B: absorb x[16,20] y[6,10] (carpet) + reflect x[20,24] y[6,10] (concrete). ---
	AddBox(FVector(18.0f, 8.0f, -0.1f), FVector(4.0f, 4.0f, 0.2f), EIM_WhiteboxZone::Carpet);
	AddBox(FVector(18.0f, 8.0f, 3.1f), FVector(4.0f, 4.0f, 0.2f), EIM_WhiteboxZone::Carpet);
	AddBox(FVector(22.0f, 8.0f, -0.1f), FVector(4.0f, 4.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddBox(FVector(22.0f, 8.0f, 3.1f), FVector(4.0f, 4.0f, 0.2f), EIM_WhiteboxZone::Concrete);
	AddWallX(16.0f, 6.0f, 10.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Carpet);
	AddWallX(20.0f, 6.0f, 10.0f, 0.0f, 3.0f, { FVector2D(7.4f, 8.6f) }, EIM_WhiteboxZone::Carpet); // 耦合门
	AddWallX(24.0f, 6.0f, 10.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(10.0f, 16.0f, 20.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Carpet);
	AddWallY(10.0f, 20.0f, 24.0f, 0.0f, 3.0f, {}, EIM_WhiteboxZone::Concrete);

	// --- Upper floor over B: slab z[3.2,3.4] with stair hole x[17,19] y[2.5,3.5] (Z-contamination test). ---
	AddBox(FVector(16.5f, 3.0f, 3.3f), FVector(1.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Wood);
	AddBox(FVector(21.5f, 3.0f, 3.3f), FVector(5.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Wood);
	AddBox(FVector(18.0f, 1.25f, 3.3f), FVector(2.0f, 2.5f, 0.2f), EIM_WhiteboxZone::Wood);
	AddBox(FVector(18.0f, 4.75f, 3.3f), FVector(2.0f, 2.5f, 0.2f), EIM_WhiteboxZone::Wood);
	AddWallX(16.0f, 0.0f, 6.0f, 3.4f, 6.4f, {}, EIM_WhiteboxZone::Concrete);
	AddWallX(24.0f, 0.0f, 6.0f, 3.4f, 6.4f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(0.0f, 16.0f, 24.0f, 3.4f, 6.4f, {}, EIM_WhiteboxZone::Concrete);
	AddWallY(6.0f, 16.0f, 24.0f, 3.4f, 6.4f, {}, EIM_WhiteboxZone::Concrete);
	AddBox(FVector(20.0f, 3.0f, 6.5f), FVector(8.0f, 6.0f, 0.2f), EIM_WhiteboxZone::Concrete); // upper ceiling

	UE_LOG(LogTemp, Log, TEXT("IMAcousticField: 白模测试场景已生成 (tag=%s)"), *IMWhiteboxTag.ToString());
#else
	UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: BuildTestScene 仅编辑器可用"));
#endif
}

void AIM_AcousticTestSceneBuilder::ClearTestScene()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World) { return; }
	TArray<AActor*> ToDestroy;
	for (AStaticMeshActor* Candidate : TActorRange<AStaticMeshActor>(World))
	{
		if (IsValid(Candidate) && Candidate->ActorHasTag(IMWhiteboxTag))
		{
			ToDestroy.Add(Candidate);
		}
	}
	for (AActor* Victim : ToDestroy)
	{
		if (IsValid(Victim)) { Victim->Destroy(); }
	}
#endif
}

#if WITH_EDITOR
void AIM_AcousticTestSceneBuilder::AddBox(const FVector& CenterM, const FVector& SizeM, EIM_WhiteboxZone Zone)
{
	UWorld* World = GetWorld();
	if (!World || !ensure(WhiteboxCube)) { return; }
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Box = World->SpawnActor<AStaticMeshActor>(CenterM * 100.0f, FRotator::ZeroRotator, Params);
	if (!ensure(Box)) { return; }
	Box->Tags.Add(IMWhiteboxTag);
	Box->SetFolderPath(IMWhiteboxFolder);
	Box->SetMobility(EComponentMobility::Static);
	UStaticMeshComponent* Mesh = Box->GetStaticMeshComponent();
	check(Mesh);
	Mesh->SetStaticMesh(WhiteboxCube);
	// Base cube assumed 1m (verify in manual test); scale maps meters directly.
	Mesh->SetWorldScale3D(SizeM);
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	if (UPhysicalMaterial* PM = ResolvePM(Zone))
	{
		Mesh->SetPhysMaterialOverride(PM);
	}
	Box->SetActorLabel(FString::Printf(TEXT("IMWhitebox_%s"), Zone == EIM_WhiteboxZone::Concrete ? TEXT("C") : (Zone == EIM_WhiteboxZone::Wood ? TEXT("W") : TEXT("K"))));
}

void AIM_AcousticTestSceneBuilder::AddWallX(float X, float Y0, float Y1, float Z0, float Z1, const TArray<FVector2D>& Gaps, EIM_WhiteboxZone Zone)
{
	// Solid y-intervals = [Y0,Y1] minus gaps (gaps span full height in P0 spec).
	float Cursor = Y0;
	for (const FVector2D& Gap : Gaps)
	{
		if (Gap.X > Cursor) { AddBox(FVector(X, (Cursor + Gap.X) * 0.5f, (Z0 + Z1) * 0.5f), FVector(IMWallT, Gap.X - Cursor, Z1 - Z0), Zone); }
		Cursor = FMath::Max(Cursor, Gap.Y);
	}
	if (Y1 > Cursor) { AddBox(FVector(X, (Cursor + Y1) * 0.5f, (Z0 + Z1) * 0.5f), FVector(IMWallT, Y1 - Cursor, Z1 - Z0), Zone); }
}

void AIM_AcousticTestSceneBuilder::AddWallY(float Y, float X0, float X1, float Z0, float Z1, const TArray<FVector2D>& Gaps, EIM_WhiteboxZone Zone)
{
	float Cursor = X0;
	for (const FVector2D& Gap : Gaps)
	{
		if (Gap.X > Cursor) { AddBox(FVector((Cursor + Gap.X) * 0.5f, Y, (Z0 + Z1) * 0.5f), FVector(Gap.X - Cursor, IMWallT, Z1 - Z0), Zone); }
		Cursor = FMath::Max(Cursor, Gap.Y);
	}
	if (X1 > Cursor) { AddBox(FVector((Cursor + X1) * 0.5f, Y, (Z0 + Z1) * 0.5f), FVector(X1 - Cursor, IMWallT, Z1 - Z0), Zone); }
}

UPhysicalMaterial* AIM_AcousticTestSceneBuilder::ResolvePM(EIM_WhiteboxZone Zone) const
{
	switch (Zone)
	{
	case EIM_WhiteboxZone::Wood: return PM_Wood;
	case EIM_WhiteboxZone::Carpet: return PM_Carpet;
	case EIM_WhiteboxZone::Concrete:
	default: return PM_Concrete;
	}
}
#endif
