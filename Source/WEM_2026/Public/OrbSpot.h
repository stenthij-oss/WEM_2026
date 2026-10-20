// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class IPlacementWorld;

/** Where an orb hovers over a horizontal segment: the surfaces under it and the air it hovers in, inclusive boxes of cells. */
struct FOrbSpot
{
	FIntVector FootMin = FIntVector::ZeroValue;
	FIntVector FootMax = FIntVector::ZeroValue;
	FIntVector AirMin = FIntVector::ZeroValue;
	FIntVector AirMax = FIntVector::ZeroValue;
};

/**
 * Where an orb can stand, asked of the grid the way the object placer asks it: an orb hovers over
 * the top face of a horizontal segment, over a square of surfaces that carry anything - the
 * placer's all-object surfaces, inside the segment's beams - with open air above it for as high as
 * it hovers. Pure, so the tests can ask it of a fake grid.
 */
namespace WEMOrbSpot
{
	/**
	 * Room for an orb over the top face of the horizontal segment covering PlatformMin to PlatformMax: a square of
	 * FootCells by FootCells all-object surfaces, with AirCells of open air above each, as near the segment's middle as
	 * there is one and no more than MaxShift cells from it either way. False when there is none.
	 */
	WEM_2026_API bool Find(const IPlacementWorld& World, const FIntVector& PlatformMin, const FIntVector& PlatformMax,
		int32 FootCells, int32 AirCells, int32 MaxShift, FOrbSpot& OutSpot);

	/** Whether a spot is still free: its surfaces still carry anything and nothing has been built in its air. */
	WEM_2026_API bool IsFree(const IPlacementWorld& World, const FOrbSpot& Spot);

	/** The middle of the spot's surfaces, in the world: what the orb hovers above. */
	WEM_2026_API FVector GetFootCentre(const IPlacementWorld& World, const FOrbSpot& Spot);
}
