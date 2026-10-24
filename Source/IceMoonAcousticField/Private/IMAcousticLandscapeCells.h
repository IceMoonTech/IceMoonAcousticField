#pragma once

#include "CoreMinimal.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Chaos/HeightField.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

// The collision face API handles both one default index and per-cell indices.
// FillMaterialIndexTile instead reads the vertex-sized grid via GetMaterialIndex
// (x,y): its last row/column are not cells, and its scalar overload does not
// expand a single default. Those 255 values must not become invented terrain holes.
inline bool IMReadHeightfieldMaterialCells(const Chaos::FHeightField& Geometry, int32 Quads,
	TConstArrayView<TObjectPtr<UPhysicalMaterial>> Materials, TArray<uint8>& OutCells,
	FString& Error, int32* StoredIndexCount = nullptr)
{
	OutCells.Reset();
	Error.Reset();
	const int64 CellCount = int64(Quads) * Quads;
	const int32 Indices = Geometry.GeomData.MaterialIndices.Num();
	if (StoredIndexCount) *StoredIndexCount = Indices;
	if (Quads <= 0 || Quads > 4096 || CellCount > MAX_int32
		|| Geometry.GetNumRows() != Quads + 1 || Geometry.GetNumCols() != Quads + 1)
	{
		Error = TEXT("Landscape physical heightfield dimensions do not match the full-resolution collision cells.");
		return false;
	}
	if ((Indices != 1 && Indices != CellCount) || Materials.IsEmpty() || Materials.Num() > 255)
	{
		Error = TEXT("Landscape physical heightfield has an invalid default/per-cell material index buffer or material table.");
		return false;
	}
	TArray<uint8> Cells;
	Cells.SetNumUninitialized(int32(CellCount));
	for (int32 Cell = 0; Cell < int32(CellCount); ++Cell)
	{
		const uint16 Material = Geometry.GetMaterialIndex(uint32(Cell) * 2u);
		if (Material != 255 && (Material >= Materials.Num() || !Materials[Material]))
		{
			Error = FString::Printf(TEXT("Landscape physical face material index %u at cell %d has no valid material table entry."),
				uint32(Material), Cell);
			return false;
		}
		Cells[Cell] = uint8(Material);
	}
	OutCells = MoveTemp(Cells);
	return true;
}

inline bool IMReadLandscapeMaterialCells(const ULandscapeHeightfieldCollisionComponent& Collision,
	TArray<uint8>& OutCells, FString& Error, int32* StoredIndexCount = nullptr)
{
	OutCells.Reset();
	if (!IsInGameThread())
	{
		Error = TEXT("Landscape physical material cells must be captured on the game thread.");
		return false;
	}
	// Keep the immutable registered geometry alive throughout the GT read.
	const auto GeometryRef = Collision.HeightfieldRef;
	if (!IsValidRef(GeometryRef) || !GeometryRef->HeightfieldGeometry)
	{
		Error = TEXT("Landscape registered physical heightfield geometry is unavailable.");
		return false;
	}
	return IMReadHeightfieldMaterialCells(*GeometryRef->HeightfieldGeometry,
		Collision.CollisionSizeQuads, MakeArrayView(Collision.CookedPhysicalMaterials),
		OutCells, Error, StoredIndexCount);
}
