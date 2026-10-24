#include "IMAcousticGrassWorldTransforms.h"

#include "Containers/StaticArray.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "InstanceDataSceneProxy.h"
#include "Misc/SecureHash.h"
#include "PrimitiveSceneProxy.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"

namespace IMAcousticGrassWorldTransformsPrivate
{
// Explicit scalars avoid SIMD padding and preserve every stored value. Grass
// buffer validation rejects nonfinite inputs before this comparator is used.
TStaticArray<double, 10> GrassTransformScalars(const FTransform& Transform)
{
	const FVector Location = Transform.GetLocation();
	const FQuat Rotation = Transform.GetRotation();
	const FVector Scale = Transform.GetScale3D();
	return {Location.X, Location.Y, Location.Z,
		Rotation.X, Rotation.Y, Rotation.Z, Rotation.W, Scale.X, Scale.Y, Scale.Z};
}

struct FIMGrassReadRequest
{
	FPrimitiveSceneProxy* Proxy = nullptr;
	FString ComponentPath;
	FTransform ComponentTransform;
	int32 Seed = 0;
};

struct FIMGrassReadBatch
{
	TArray<FIMGrassReadRequest> Requests;
	TArray<FIMAcousticGrassWorldTransformSnapshot> Snapshots;
	FString Error;
};

bool ReadGrassBuffer(const FIMGrassReadRequest& Request,
	FIMAcousticGrassWorldTransformSnapshot& Snapshot, FString& Error)
{
	if (Snapshot.RenderInstanceCount == 0) return true;
	const FInstanceSceneDataBuffers* Buffers = Request.Proxy->GetInstanceSceneDataBuffers(
		FPrimitiveSceneProxy::EInstanceBufferAccessFlags::SynchronizeUpdateTask);
	if (!Buffers)
	{
		Error = TEXT("Grass scene buffer is unavailable: ") + Request.ComponentPath;
		return false;
	}
	Snapshot.bSceneBufferObserved = true;
	Snapshot.bGPUOnly = Buffers->IsInstanceDataGPUOnly();
	Snapshot.BufferInstanceCount = Buffers->GetNumInstances();
	if (Snapshot.bGPUOnly || Snapshot.BufferInstanceCount != Snapshot.RenderInstanceCount)
	{
		Error = FString::Printf(TEXT("Grass transform export is incomplete: %s render=%d buffer=%d gpu_only=%d"),
			*Request.ComponentPath, Snapshot.RenderInstanceCount, Snapshot.BufferInstanceCount, Snapshot.bGPUOnly);
		return false;
	}
	Snapshot.PrimitiveWorldSpaceOffset = Buffers->GetPrimitiveWorldSpaceOffset();
	for (int32 Index = 0; Index < Snapshot.BufferInstanceCount; ++Index)
	{
		// UE's accessor reconstructs the high world offset. Reapplying the
		// component transform here would move every generated instance twice.
		const FMatrix WorldMatrix = Buffers->GetInstanceToWorld(Index);
		if (WorldMatrix.ContainsNaN() || !FMath::IsFinite(WorldMatrix.Determinant())
			|| WorldMatrix.Determinant() == 0.0)
		{
			Error = FString::Printf(TEXT("Grass renderer transform is nonfinite or singular: %s instance=%d"),
				*Request.ComponentPath, Index);
			return false;
		}
		const FTransform WorldTransform(WorldMatrix);
		if (WorldTransform.ContainsNaN())
		{
			Error = FString::Printf(TEXT("Grass world transform conversion failed: %s instance=%d"),
				*Request.ComponentPath, Index);
			return false;
		}
		Snapshot.WorldTransforms[Index] = WorldTransform;
	}
	return true;
}
}

bool IMCompareAcousticGrassWorldTransforms(const FTransform& A, const FTransform& B)
{
	const auto Left = IMAcousticGrassWorldTransformsPrivate::GrassTransformScalars(A);
	const auto Right = IMAcousticGrassWorldTransformsPrivate::GrassTransformScalars(B);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		if (Left[Index] < Right[Index]) return true;
		if (Left[Index] > Right[Index]) return false;
	}
	return false;
}

FString IMHashSortedAcousticGrassWorldTransforms(TConstArrayView<FTransform> Transforms)
{
	FSHA1 Hash;
	// This diagnostic digest has a new binary encoding, not the old rounded
	// UTF-16 sort-key encoding. Only same-encoding source/copy hashes compare.
	const ANSICHAR Encoding[] = "IMAcousticGrassExactTransform.v1";
	Hash.Update(reinterpret_cast<const uint8*>(Encoding), sizeof(Encoding));
	const int32 Count = Transforms.Num();
	Hash.Update(reinterpret_cast<const uint8*>(&Count), sizeof(Count));
	for (const FTransform& Transform : Transforms)
	{
		const auto Scalars = IMAcousticGrassWorldTransformsPrivate::GrassTransformScalars(Transform);
		Hash.Update(reinterpret_cast<const uint8*>(Scalars.GetData()), 10 * sizeof(double));
	}
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool IMReadLandscapeGrassWorldTransforms(
	TConstArrayView<UInstancedStaticMeshComponent*> Components,
	TArray<FIMAcousticGrassWorldTransformSnapshot>& OutSnapshots,
	FString& OutError)
{
	OutSnapshots.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Grass transform export requires the game thread.");
		return false;
	}
	TArray<TStrongObjectPtr<UInstancedStaticMeshComponent>> Owners;
	TSet<UWorld*> Worlds;
	TSet<UInstancedStaticMeshComponent*> Seen;
	for (UInstancedStaticMeshComponent* Component : Components)
	{
		UWorld* World = IsValid(Component) ? Component->GetWorld() : nullptr;
		if (!World || (World->WorldType != EWorldType::Editor && !World->IsGameWorld()) || !Component->IsRegistered()
			|| Component->GetClass()->GetPathName() != TEXT("/Script/Foliage.GrassInstancedStaticMeshComponent")
			|| Seen.Contains(Component))
		{
			OutError = TEXT("Grass export requires unique registered generated Grass components in an Editor or game world.");
			return false;
		}
		Owners.Emplace(Component);
		Seen.Add(Component);
		Worlds.Add(World);
	}
	if (Components.IsEmpty()) return true;

	// Drain pending render-state replacement before capturing raw non-UObject
	// proxies. GT then remains blocked until the RT command completes: no map
	// switch/unregister is admitted during this one-shot ownership interval.
	for (UWorld* World : Worlds) World->SendAllEndOfFrameUpdates();
	FlushRenderingCommands();
	const TSharedRef<IMAcousticGrassWorldTransformsPrivate::FIMGrassReadBatch, ESPMode::ThreadSafe> Batch =
		MakeShared<IMAcousticGrassWorldTransformsPrivate::FIMGrassReadBatch, ESPMode::ThreadSafe>();
	Batch->Requests.SetNum(Components.Num());
	Batch->Snapshots.SetNum(Components.Num());
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		UInstancedStaticMeshComponent* Component = Components[Index];
		IMAcousticGrassWorldTransformsPrivate::FIMGrassReadRequest& Request = Batch->Requests[Index];
		FIMAcousticGrassWorldTransformSnapshot& Snapshot = Batch->Snapshots[Index];
		Request.Proxy = Component->GetSceneProxy();
		Request.ComponentPath = Component->GetPathName();
		Request.ComponentTransform = Component->GetComponentTransform();
		Request.Seed = Component->InstancingRandomSeed;
		Snapshot.CPUInstanceCount = Component->GetInstanceCount();
		Snapshot.RenderInstanceCount = Component->GetNumRenderInstances();
		Snapshot.bSceneProxyPresent = Request.Proxy != nullptr;
		if (Snapshot.RenderInstanceCount < 0 || (Snapshot.RenderInstanceCount > 0 && !Request.Proxy))
		{
			OutError = TEXT("Grass render count or proxy is unavailable: ") + Request.ComponentPath;
			return false;
		}
		// GT preallocates every output; RT performs no per-instance growth.
		Snapshot.WorldTransforms.SetNum(Snapshot.RenderInstanceCount);
	}
	ENQUEUE_RENDER_COMMAND(IMCopyAcousticGrassWorldTransforms)(
		[Batch](FRHICommandListImmediate&)
		{
			for (int32 Index = 0; Index < Batch->Requests.Num(); ++Index)
				if (!IMAcousticGrassWorldTransformsPrivate::ReadGrassBuffer(Batch->Requests[Index], Batch->Snapshots[Index], Batch->Error)) return;
		});
	FlushRenderingCommands();
	if (!Batch->Error.IsEmpty())
	{
		OutError = Batch->Error;
		return false;
	}
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		UInstancedStaticMeshComponent* Component = Components[Index];
		const IMAcousticGrassWorldTransformsPrivate::FIMGrassReadRequest& Request = Batch->Requests[Index];
		const FIMAcousticGrassWorldTransformSnapshot& Snapshot = Batch->Snapshots[Index];
		if (!IsValid(Component) || !Component->IsRegistered() || Component->GetSceneProxy() != Request.Proxy
			|| Component->InstancingRandomSeed != Request.Seed
			|| Component->GetNumRenderInstances() != Snapshot.RenderInstanceCount
			|| !Component->GetComponentTransform().Equals(Request.ComponentTransform, 0.0))
		{
			OutError = TEXT("Grass owner changed during the synchronized read: ") + Request.ComponentPath;
			return false;
		}
	}
	OutSnapshots = MoveTemp(Batch->Snapshots);
	return true;
}
