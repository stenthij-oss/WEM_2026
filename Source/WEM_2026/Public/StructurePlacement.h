// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PlatformStructureData.h"

/**
 * How a structure is laid onto a segment: which face it fits, which way round, and where its mesh
 * goes. Pure geometry, with nothing read from a room manager, so every case can be checked on its own.
 *
 * A segment is named here the way the room manager names a platform: the axis it faces along, and
 * its spans beam to beam along its first and second in-plane axes (see WEMGrid::GetInPlaneAxes) -
 * level and level across a floor, level and up a wall.
 */
namespace WEMStructures
{
	/**
	 * Where the importer puts one of Blender's axes (0, 1, 2 for X, Y, Z) in an imported mesh's own
	 * frame. Measured with the calibration structure rather than assumed: a glTF file carries its
	 * axes fixed, and the importer turns them into Unreal's.
	 */
	WEM_2026_API FVector GetMeshAxisForBlenderAxis(int32 BlenderAxis);

	/** The type of the face a segment facing along NormalAxis shows on the side FaceSign looks out from. */
	WEM_2026_API EStructureType GetFaceType(int32 NormalAxis, int32 FaceSign);

	/**
	 * Whether a structure of Width x Length cells fits a segment's opening. Across a floor or ceiling
	 * it may lie either way round; up a wall it stands with its length up, or lies on its side only
	 * when it may. Turned means its length runs along the segment's first axis.
	 */
	WEM_2026_API bool FitsShape(int32 Width, int32 Length, bool bMayLieSideways, int32 NormalAxis, int32 SpanFirst, int32 SpanSecond, bool& bOutTurned);

	/**
	 * Whether a structure may fill one face of a segment: its types take in the face's, and the other
	 * face's too when it is two-sided, and its shape fits the opening.
	 */
	WEM_2026_API bool FitsFace(const UPlatformStructureData& Structure, int32 NormalAxis, int32 FaceSign, int32 SpanFirst, int32 SpanSecond, bool& bOutTurned);

	/**
	 * Where a structure's mesh goes on one face of a segment whose min node's cell starts at MinCorner.
	 *
	 * The mesh's face - Blender's +Z - looks out of the segment on the side FaceSign says. Up a wall
	 * its top - Blender's +Y - points up, or along the wall's level axis when it lies on its side;
	 * across a floor or ceiling it points along the axis its length runs. Its X follows as a proper
	 * rotation, never a mirror, so it reads the same from the side it faces as it did in Blender.
	 * It stands at the centre of the opening on the segment's mid-plane, its back there; a two-sided
	 * structure was modelled lying on the ground, so it goes back half a cell to fill both halves.
	 */
	WEM_2026_API FTransform MakeStructureTransform(
		const FVector& MinCorner,
		double CellSize,
		int32 NormalAxis,
		int32 SpanFirst,
		int32 SpanSecond,
		int32 FaceSign,
		bool bTurned,
		bool bTwoSided);
}
