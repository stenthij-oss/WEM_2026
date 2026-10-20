// Copyright Epic Games, Inc. All Rights Reserved.

#include "OrbSpot.h"

#include "PlacementWorld.h"

namespace OrbSpotPrivate
{
	FOrbSpot MakeSpot(const FIntVector& FootMin, const int32 FootCells, const int32 AirCells)
	{
		FOrbSpot Spot;
		Spot.FootMin = FootMin;
		Spot.FootMax = FootMin + FIntVector(FootCells - 1, FootCells - 1, 0);
		Spot.AirMin = FootMin + FIntVector(0, 0, 1);
		Spot.AirMax = Spot.FootMax + FIntVector(0, 0, AirCells);
		return Spot;
	}
}

bool WEMOrbSpot::Find(const IPlacementWorld& World, const FIntVector& PlatformMin, const FIntVector& PlatformMax,
	const int32 FootCells, const int32 AirCells, const int32 MaxShift, FOrbSpot& OutSpot)
{
	using namespace OrbSpotPrivate;

	if (PlatformMin.Z != PlatformMax.Z || FootCells < 1 || AirCells < 1)
	{
		return false;
	}

	// The square that stands in the middle, rounding towards the min corner when it cannot stand exactly there.
	const FIntVector Middle(
		PlatformMin.X + (PlatformMax.X - PlatformMin.X + 1 - FootCells) / 2,
		PlatformMin.Y + (PlatformMax.Y - PlatformMin.Y + 1 - FootCells) / 2,
		PlatformMin.Z);

	// Nearest the middle first, in a fixed order, so the same structure always gives the same spot.
	TArray<FIntPoint> Shifts;
	const int32 Reach = FMath::Max(MaxShift, 0);

	for (int32 Y = -Reach; Y <= Reach; ++Y)
	{
		for (int32 X = -Reach; X <= Reach; ++X)
		{
			Shifts.Emplace(X, Y);
		}
	}

	Shifts.StableSort([](const FIntPoint& A, const FIntPoint& B) { return A.SizeSquared() < B.SizeSquared(); });

	for (const FIntPoint& Shift : Shifts)
	{
		const FIntVector FootMin = Middle + FIntVector(Shift.X, Shift.Y, 0);

		if (FootMin.X < PlatformMin.X || FootMin.Y < PlatformMin.Y
			|| FootMin.X + FootCells - 1 > PlatformMax.X || FootMin.Y + FootCells - 1 > PlatformMax.Y)
		{
			continue;
		}

		const FOrbSpot Spot = MakeSpot(FootMin, FootCells, AirCells);

		if (IsFree(World, Spot))
		{
			OutSpot = Spot;
			return true;
		}
	}

	return false;
}

bool WEMOrbSpot::IsFree(const IPlacementWorld& World, const FOrbSpot& Spot)
{
	// The surfaces first: a handful of lookups, and the commonest reason a spot will not do.
	for (int32 Y = Spot.FootMin.Y; Y <= Spot.FootMax.Y; ++Y)
	{
		for (int32 X = Spot.FootMin.X; X <= Spot.FootMax.X; ++X)
		{
			if (World.GetSurfaceCapacity(FIntVector(X, Y, Spot.FootMin.Z), EGridFace::PosZ) != ESurfaceCapacity::AllObject)
			{
				return false;
			}
		}
	}

	// Then the air, from the bottom up, where anything built is likeliest to be met first.
	for (int32 Z = Spot.AirMin.Z; Z <= Spot.AirMax.Z; ++Z)
	{
		for (int32 Y = Spot.AirMin.Y; Y <= Spot.AirMax.Y; ++Y)
		{
			for (int32 X = Spot.AirMin.X; X <= Spot.AirMax.X; ++X)
			{
				const FIntVector Cell(X, Y, Z);

				if (!World.IsValidCell(Cell) || World.IsCellSolid(Cell))
				{
					return false;
				}
			}
		}
	}

	return true;
}

FVector WEMOrbSpot::GetFootCentre(const IPlacementWorld& World, const FOrbSpot& Spot)
{
	return 0.5 * (World.GetFaceCentre(Spot.FootMin, EGridFace::PosZ) + World.GetFaceCentre(Spot.FootMax, EGridFace::PosZ));
}
