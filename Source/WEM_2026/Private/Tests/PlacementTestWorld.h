// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMath.h"
#include "PlacementWorld.h"

/**
 * A stand-in grid for the placement tests: a cube of cells, some of them solid, with whichever
 * faces the test lays down as surfaces. Segments are laid the way the room manager lays them -
 * one cell thick, both broad faces carrying anything - without any of the growth around them.
 */
class FPlacementTestWorld : public IPlacementWorld
{
public:
	double CellSize = 20.0;

	/** Cells along each side of the cube, which starts at the world origin. */
	int32 CubeCells = 64;

	int32 Revision = 1;

	/**
	 * Lays a segment facing along NormalAxis whose min cell is MinCell, SpanA cells along the
	 * plane's first axis and SpanB along its second, in the room manager's in-plane axis order.
	 * Both broad faces of every cell are all-object surfaces.
	 */
	void AddSegment(const int32 NormalAxis, const FIntVector& MinCell, const int32 SpanA, const int32 SpanB)
	{
		int32 AxisA, AxisB;
		WEMGrid::GetInPlaneAxes(NormalAxis, AxisA, AxisB);

		for (int32 StepA = 0; StepA < SpanA; ++StepA)
		{
			for (int32 StepB = 0; StepB < SpanB; ++StepB)
			{
				const FIntVector Cell = MinCell + WEMGrid::AxisStep(AxisA, StepA) + WEMGrid::AxisStep(AxisB, StepB);
				SolidCells.Add(Cell);

				for (const int32 Sign : { 1, -1 })
				{
					SetCapacity(Cell, WEMGrid::MakeFace(NormalAxis, Sign), ESurfaceCapacity::AllObject);
				}
			}
		}

		++Revision;
	}

	/** Fills an inclusive box of cells with structure that offers no surface of its own: a wall and nothing more. */
	void AddSolidBlock(const FIntVector& Min, const FIntVector& Max)
	{
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
				{
					SolidCells.Add(FIntVector(X, Y, Z));
				}
			}
		}

		++Revision;
	}

	void SetCapacity(const FIntVector& Cell, const EGridFace Face, const ESurfaceCapacity Capacity)
	{
		const uint64 Key = WEMGrid::MakeFaceKey(Cell, Face);

		if (!Capacities.Contains(Key))
		{
			SurfaceOrder.Add({ Cell, Face });
		}

		Capacities.Add(Key, Capacity);
		++Revision;
	}

	virtual double GetCellSize() const override
	{
		return CellSize;
	}

	virtual bool IsValidCell(const FIntVector& Cell) const override
	{
		return Cell.X >= 0 && Cell.Y >= 0 && Cell.Z >= 0 && Cell.X < CubeCells && Cell.Y < CubeCells && Cell.Z < CubeCells;
	}

	virtual bool IsCellSolid(const FIntVector& Cell) const override
	{
		return SolidCells.Contains(Cell);
	}

	virtual ESurfaceCapacity GetSurfaceCapacity(const FIntVector& Cell, const EGridFace Face) const override
	{
		const ESurfaceCapacity* Capacity = Capacities.Find(WEMGrid::MakeFaceKey(Cell, Face));
		return Capacity ? *Capacity : ESurfaceCapacity::Empty;
	}

	virtual void GatherOpenAllObjectSurfaces(TArray<FGridSurfaceRef>& OutSurfaces) const override
	{
		OutSurfaces.Reset();

		for (const TPair<FIntVector, EGridFace>& Surface : SurfaceOrder)
		{
			if (GetSurfaceCapacity(Surface.Key, Surface.Value) == ESurfaceCapacity::AllObject
				&& !IsCellSolid(Surface.Key + WEMGrid::FaceStep(Surface.Value)))
			{
				FGridSurfaceRef& Ref = OutSurfaces.AddDefaulted_GetRef();
				Ref.Cell = Surface.Key;
				Ref.Face = Surface.Value;
			}
		}
	}

	virtual FVector GetFaceCentre(const FIntVector& Cell, const EGridFace Face) const override
	{
		return (FVector(Cell) + FVector(0.5)) * CellSize + FVector(WEMGrid::FaceStep(Face)) * (0.5 * CellSize);
	}

	virtual int32 GetStructureRevision() const override
	{
		return Revision;
	}

	/** Keeps no record of what changed, so the solver gathers every surface again whenever anything does. */
	virtual bool GatherSurfaceChangesSince(const int32 FromRevision, TArray<FGridSurfaceRef>& OutSurfaces) const override
	{
		OutSurfaces.Reset();
		return false;
	}

	/** The world box one cell fills. */
	FBox GetCellBox(const FIntVector& Cell) const
	{
		return FBox(FVector(Cell) * CellSize, (FVector(Cell) + FVector(1.0)) * CellSize);
	}

	TSet<FIntVector> SolidCells;
	TMap<uint64, ESurfaceCapacity> Capacities;

private:
	/** Surfaces in the order they were laid, which is the order they are gathered in. */
	TArray<TPair<FIntVector, EGridFace>> SurfaceOrder;
};

#endif
