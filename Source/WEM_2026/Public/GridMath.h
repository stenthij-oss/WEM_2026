// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RoomManager.h"

/**
 * The room manager's small grid helpers: packing a cell into a key, and naming axes, faces and
 * the steps between cells. Shared by ARoomManager and the object placement code, so that both
 * address the grid the same way.
 */
namespace WEMGrid
{
	/**
	 * Bits per coordinate in a packed key. Inside the cube a coordinate is never negative and
	 * never past the 512 cap, so 20 bits is ample, and the top four bits of the key are left to
	 * say which face or axis it means.
	 */
	constexpr int32 KeyCoordinateBits = 20;
	constexpr uint64 KeyCoordinateMask = (uint64(1) << KeyCoordinateBits) - 1;
	constexpr int32 KeyTagShift = 3 * KeyCoordinateBits;

	/**
	 * One key per cell, packed from the cell's own coordinate. Never from a lattice index: those
	 * count from wherever a platform happens to be and run negative below it.
	 */
	FORCEINLINE uint64 MakeCellKey(const FIntVector& Cell)
	{
		return (static_cast<uint64>(Cell.X) & KeyCoordinateMask)
			| ((static_cast<uint64>(Cell.Y) & KeyCoordinateMask) << KeyCoordinateBits)
			| ((static_cast<uint64>(Cell.Z) & KeyCoordinateMask) << (2 * KeyCoordinateBits));
	}

	FORCEINLINE FIntVector CellFromKey(const uint64 Key)
	{
		return FIntVector(
			static_cast<int32>(Key & KeyCoordinateMask),
			static_cast<int32>((Key >> KeyCoordinateBits) & KeyCoordinateMask),
			static_cast<int32>((Key >> (2 * KeyCoordinateBits)) & KeyCoordinateMask));
	}

	/** A cell plus a small tag - an axis or a face - in the top bits. Offset by one, so no tagged key is ever a bare cell key. */
	FORCEINLINE uint64 MakeTaggedKey(const FIntVector& Cell, const int32 Tag)
	{
		return MakeCellKey(Cell) | (static_cast<uint64>(Tag + 1) << KeyTagShift);
	}

	/** A step of Length cells along one axis. */
	FORCEINLINE FIntVector AxisStep(const int32 Axis, const int32 Length = 1)
	{
		FIntVector Step = FIntVector::ZeroValue;
		Step[Axis] = Length;
		return Step;
	}

	/**
	 * The two axes a platform facing along NormalAxis lies across, in the order its tile's width
	 * and length run: the first is always level, and up a wall the second is height. Across a
	 * floor they are X and Y; along a wall facing X, Y and Z; along one facing Y, X and Z.
	 */
	FORCEINLINE void GetInPlaneAxes(const int32 NormalAxis, int32& OutFirst, int32& OutSecond)
	{
		OutFirst = NormalAxis == 0 ? 1 : 0;
		OutSecond = NormalAxis == 2 ? 1 : 2;
	}

	/** EGridFace runs +X, -X, +Y, -Y, +Z, -Z, so the axis and the sign fall straight out of it. */
	FORCEINLINE int32 FaceAxis(const EGridFace Face)
	{
		return static_cast<int32>(Face) / 2;
	}

	FORCEINLINE int32 FaceSign(const EGridFace Face)
	{
		return (static_cast<int32>(Face) % 2 == 0) ? 1 : -1;
	}

	FORCEINLINE EGridFace MakeFace(const int32 Axis, const int32 Sign)
	{
		return static_cast<EGridFace>(Axis * 2 + (Sign > 0 ? 0 : 1));
	}

	/** The step from a cell to the one its face looks into. */
	FORCEINLINE FIntVector FaceStep(const EGridFace Face)
	{
		return AxisStep(FaceAxis(Face), FaceSign(Face));
	}

	FORCEINLINE uint64 MakeFaceKey(const FIntVector& Cell, const EGridFace Face)
	{
		return MakeTaggedKey(Cell, static_cast<int32>(Face));
	}
}
