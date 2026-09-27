// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PlacementTypes.h"
#include "RoomManager.h"

/**
 * The arithmetic of placing an object on the grid, with no world attached: which cells a pose
 * covers, which way its sides face, and where its mesh goes. Everything here works in whole grid
 * directions, so a footprint is carried onto a support by stepping along integer vectors rather
 * than by turning arrays, and every face direction goes through the same code.
 */
namespace WEMPlacement
{
	/** The four in-plane directions of a support, in plane coordinates, in the order a rotation index names them. */
	inline const FIntPoint PlaneDirections[4] = { FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1) };

	/** Which of PlaneDirections a direction is, or INDEX_NONE for anything that is not one of them. */
	WEM_2026_API int32 GetPlaneDirectionIndex(const FIntPoint& Direction);

	/**
	 * The one place a side is given its local direction. Front is +X and Top is +Z; Right is +Y,
	 * the object's own right in UE's left-handed frame, where an object facing +X has its right at
	 * +Y. Flipping the convention for right and left means changing this and nothing else.
	 */
	WEM_2026_API FIntVector GetSideDirection(EObjectSide Side);

	/** Front, Back, Left and Right: the sides that have an edge on the footprint. */
	WEM_2026_API bool IsEdgeSide(EObjectSide Side);

	/** The edge side whose local direction is this in-plane one. False for anything that is not a unit step along local X or Y. */
	WEM_2026_API bool FindEdgeSide(const FIntPoint& LocalDirection, EObjectSide& OutSide);

	FORCEINLINE int32 SideBit(const EObjectSide Side)
	{
		return 1 << static_cast<int32>(Side);
	}

	FORCEINLINE int32 SupportBit(const ESupportKind Kind)
	{
		return 1 << static_cast<int32>(Kind);
	}

	/** Where a footprint cell's entry sits in the per-cell arrays: row-major by local x, then y. */
	FORCEINLINE int32 GetFootprintIndex(const FIntVector& SizeCells, const int32 LocalX, const int32 LocalY)
	{
		return LocalX * SizeCells.Y + LocalY;
	}

	FORCEINLINE int32 GetFootprintCellCount(const FIntVector& SizeCells)
	{
		return FMath::Max(SizeCells.X, 0) * FMath::Max(SizeCells.Y, 0);
	}

	/**
	 * Cells a size in centimetres takes up along each axis. A small tolerance keeps a size a hair
	 * over a whole number of cells from costing a whole extra cell: 60.0001 cm is three cells of 20.
	 */
	WEM_2026_API FIntVector ComputeSizeCells(const FVector& SizeCm, double CellSize);

	/** One face on a footprint's perimeter, in the object's own frame. */
	struct FEdgeFace
	{
		/** The footprint cell the face belongs to, local x and y. */
		FIntPoint Cell = FIntPoint::ZeroValue;

		/** The local in-plane direction the face looks out along. */
		FIntPoint Direction = FIntPoint::ZeroValue;

		EObjectSide Side = EObjectSide::Front;

		/** Along its edge from the edge's lower local coordinate: y for Front and Back, x for Left and Right. */
		int32 Index = 0;

		/** The first or last face along its edge. */
		bool bCorner = false;
	};

	/**
	 * Every face on a footprint's perimeter: for each cell, each in-plane direction whose
	 * neighbour lies outside the footprint gives one face, labelled with the side it looks out
	 * from. A corner cell therefore gives two faces, one to each of two edges, and a one-cell
	 * footprint gives four. Mask, when given, says which of the SizeCells.X by SizeCells.Y cells
	 * are in the footprint; without one every cell is.
	 */
	WEM_2026_API void GatherEdgeFaces(const FIntVector& SizeCells, TConstArrayView<uint8> Mask, TArray<FEdgeFace>& OutFaces);

	/**
	 * The cells an exclusion zone reaches, in the object's own frame: a band DepthCells deep out from
	 * each chosen side, as wide as the side, and with bIncludeCorners the square between two
	 * neighbouring chosen sides. They lie outside the footprint, so some coordinates run negative.
	 */
	WEM_2026_API void GatherZoneCells(const FIntVector& SizeCells, const FExclusionZone& Zone, TArray<FIntPoint>& OutCells);

	/** How many faces run along one side of a full rectangular footprint. */
	WEM_2026_API int32 GetEdgeLength(const FIntVector& SizeCells, EObjectSide Side);

	/** A support plane's directions in whole grid steps: its normal, and the axes its cell coordinates run along. */
	struct FPlaneAxes
	{
		FIntVector Normal = FIntVector(0, 0, 1);
		FIntVector AxisA = FIntVector(1, 0, 0);
		FIntVector AxisB = FIntVector(0, 1, 0);
	};

	/** A segment's plane facing out of Face, with its cell axes in the order the room manager's GetInPlaneAxes gives them. */
	WEM_2026_API FPlaneAxes MakeSegmentPlaneAxes(EGridFace Face);

	/** The world surface cell at a segment plane's cell: the plane's layer along the face's axis, and the cell's coordinates along the other two. */
	WEM_2026_API FIntVector SegmentPlaneCellToWorld(EGridFace Face, int32 Layer, const FIntPoint& PlaneCell);

	/** A world cell's coordinates on a segment plane facing Face; its coordinate along the face's axis is the plane's layer. */
	WEM_2026_API FIntPoint WorldToSegmentPlaneCell(EGridFace Face, const FIntVector& Cell);

	/** A plane direction as a world direction. */
	WEM_2026_API FIntVector PlaneToWorldDirection(const FPlaneAxes& Plane, const FIntPoint& PlaneDirection);

	/** A world direction lying in the plane as a plane direction. Anything off the plane is dropped. */
	WEM_2026_API FIntPoint WorldToPlaneDirection(const FPlaneAxes& Plane, const FIntVector& WorldDirection);

	/** The same product FRotationMatrix::MakeFromXZ takes Y from, Y = Z ^ X, in whole numbers. */
	WEM_2026_API FIntVector CrossProduct(const FIntVector& A, const FIntVector& B);

	/** A posed object's axes: world directions of its front, right and up, and its front and right in plane coordinates. */
	struct FPoseFrame
	{
		FIntVector Forward = FIntVector(1, 0, 0);
		FIntVector Right = FIntVector(0, 1, 0);
		FIntVector Up = FIntVector(0, 0, 1);

		FIntPoint PlaneForward = FIntPoint(1, 0);
		FIntPoint PlaneRight = FIntPoint(0, 1);
	};

	/**
	 * How an object stands on a support at one of its four quarter turns: bottom on the face, up
	 * along the face's normal - that support's gravity - and front along the plane direction the
	 * rotation names. Right is up crossed with front, exactly as the mesh rotation will have it.
	 */
	WEM_2026_API FPoseFrame MakePoseFrame(const FPlaneAxes& Plane, int32 Rotation);

	/** The plane cell a local footprint cell lands on, given the plane cell local (0, 0) lands on. */
	FORCEINLINE FIntPoint LocalToPlaneCell(const FPoseFrame& Frame, const FIntPoint& Origin, const FIntPoint& LocalCell)
	{
		return Origin + Frame.PlaneForward * LocalCell.X + Frame.PlaneRight * LocalCell.Y;
	}

	/** A local in-plane direction as a plane direction. */
	FORCEINLINE FIntPoint LocalToPlaneDirection(const FPoseFrame& Frame, const FIntPoint& LocalDirection)
	{
		return Frame.PlaneForward * LocalDirection.X + Frame.PlaneRight * LocalDirection.Y;
	}

	/** The rotation that turns Side to look along PlaneDirection, or INDEX_NONE when none does. */
	WEM_2026_API int32 FindRotationFacing(const FPlaneAxes& Plane, EObjectSide Side, const FIntPoint& PlaneDirection);

	/** The mesh's world rotation for a pose: front along Forward, up along Up. */
	WEM_2026_API FQuat MakeWorldRotation(const FPoseFrame& Frame);

	/**
	 * Local position, in the mesh's own space, of a footprint cell's centre at HeightCm above the
	 * object's bottom. The footprint grid is centred on the bounds' centre in X and Y and starts
	 * at their bottom in Z, the same grid the catalog builder measured the mesh on.
	 */
	WEM_2026_API FVector GetFootprintCellLocalCentre(const FBox& LocalBounds, const FIntVector& SizeCells, double CellSize, const FIntPoint& LocalCell, double HeightCm);

	/**
	 * Where a posed mesh goes, from its measured bounds rather than its pivot, so no mesh has to be
	 * loaded to place it. Its bounds' centre lands over the footprint's centre, its bottom on the
	 * support face. Then, for each side a contact held on, it slides toward that side by half of
	 * its footprint's slack, so a 55 cm chair in a 60 cm footprint stands against its wall rather
	 * than 2.5 cm off it. ContactSideMask holds a SideBit for each such side.
	 */
	WEM_2026_API FTransform MakeObjectWorldTransform(
		const FPoseFrame& Frame,
		const FVector& FootprintCentreOnFace,
		const FBox& LocalBounds,
		const FIntVector& SizeCells,
		double CellSize,
		int32 ContactSideMask);

	/** Plane name as the log writes it: the face and the layer along its axis, as in +Z@32. */
	WEM_2026_API FString DescribeSegmentPlane(EGridFace Face, int32 Layer);

	/** A world direction as the log writes it: +X, -Z and so on. */
	WEM_2026_API FString DescribeDirection(const FIntVector& Direction);

	WEM_2026_API const TCHAR* GetSideName(EObjectSide Side);
}
