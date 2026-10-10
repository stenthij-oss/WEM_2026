// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "PlatformStructureData.h"

enum class EGridFace : uint8;

/**
 * How a structure is laid onto a segment: which face it fits, which way round, and where its mesh
 * goes. Pure geometry, with nothing read from a room manager, so every case can be checked on its own.
 *
 * A segment is named here the way the room manager names a platform: the axis it faces along, its
 * spans beam to beam along its first and second in-plane axes (see WEMGrid::GetInPlaneAxes), and
 * its Up - the way up in its own frame, which need not be the world's. Up along its normal makes it
 * a floor, its Ground face the one looking along Up and its Ceiling face the other; Up in its plane
 * makes it a wall, whose structures stand with their tops along Up.
 *
 * The category pull is here too, with categories as the room manager numbers them: which structure
 * a face is filled with, given what the faces around it were filled with.
 */
namespace WEMStructures
{
	/** One structure a face could be filled with, as the draw sees it. */
	struct FDrawOption
	{
		float Weight = 1.0f;

		/** Its categories, each once. */
		TArray<int32, TInlineAllocator<4>> Categories;
	};

	/**
	 * What one filled face beside the face being drawn offers: each of its categories, with the
	 * chance its streak continues.
	 */
	struct FNeighbourOffer
	{
		TArray<TPair<int32, float>, TInlineAllocator<4>> Categories;
	};

	/**
	 * Where the importer puts one of Blender's axes (0, 1, 2 for X, Y, Z) in an imported mesh's own
	 * frame. Measured with the calibration structure rather than assumed: a glTF file carries its
	 * axes fixed, and the importer turns them into Unreal's.
	 */
	WEM_2026_API FVector GetMeshAxisForBlenderAxis(int32 BlenderAxis);

	/** Whether a segment facing along NormalAxis is a wall in its own frame: its Up lies in its plane. */
	WEM_2026_API bool IsWall(int32 NormalAxis, EGridFace Up);

	/** The type of the face a segment facing along NormalAxis, with its Up, shows on the side FaceSign looks out from. */
	WEM_2026_API EStructureType GetFaceType(int32 NormalAxis, int32 FaceSign, EGridFace Up);

	/**
	 * Whether a structure of Width x Length cells fits a segment's opening. Across a floor or ceiling
	 * it may lie either way round; up a wall it stands with its length along the wall's Up, or lies on
	 * its side only when it may. Turned means its length runs along the segment's first axis.
	 */
	WEM_2026_API bool FitsShape(int32 Width, int32 Length, bool bMayLieSideways, int32 NormalAxis, EGridFace Up, int32 SpanFirst, int32 SpanSecond, bool& bOutTurned);

	/**
	 * Whether a structure may fill one face of a segment: its types take in the face's, and the other
	 * face's too when it is two-sided, and its shape fits the opening.
	 */
	WEM_2026_API bool FitsFace(const UPlatformStructureData& Structure, int32 NormalAxis, int32 FaceSign, EGridFace Up, int32 SpanFirst, int32 SpanSecond, bool& bOutTurned);

	/**
	 * Where a structure's mesh goes on one face of a segment whose min node's cell starts at MinCorner.
	 *
	 * The mesh's face - Blender's +Z - looks out of the segment on the side FaceSign says. Its top -
	 * Blender's +Y - points along the axis its length runs: up a wall that is the wall's Up, or the
	 * wall's level axis when it lies on its side; across a floor or ceiling it is either in-plane axis. Its X follows as a proper
	 * rotation, never a mirror, so it reads the same from the side it faces as it did in Blender.
	 * It stands at the centre of the opening on the segment's mid-plane, its back there; a two-sided
	 * structure was modelled lying on the ground, so it goes back half a cell to fill both halves.
	 */
	WEM_2026_API FTransform MakeStructureTransform(
		const FVector& MinCorner,
		double CellSize,
		int32 NormalAxis,
		EGridFace Up,
		int32 SpanFirst,
		int32 SpanSecond,
		int32 FaceSign,
		bool bTurned,
		bool bTwoSided);

	/**
	 * The chance a category's streak of Streak faces carries on into one more: ContinueChance for a
	 * streak of one, rising evenly to MaxContinueChance at StreakForMaxChance and staying there.
	 */
	WEM_2026_API float GetContinueChance(int32 Streak, float ContinueChance, float MaxContinueChance, int32 StreakForMaxChance);

	/**
	 * Draws which of Options fills a face, from Stream, pulled toward the categories of the faces
	 * around it.
	 *
	 * Only the categories some option carries are live. Each neighbour with a live category makes one
	 * offer, as strong as its strongest live category - a neighbour of several categories pulls no
	 * harder than its strongest, it only spreads that pull across them. With the chance that any
	 * offer is taken, one is: a neighbour in proportion to its offer, then one of its live categories
	 * in proportion to its chance, then an option carrying that category by weight. Otherwise, or
	 * when nothing is offered, an option is drawn by weight from all of them. Weight is per option,
	 * however many categories it carries.
	 *
	 * Returns the option's index, or INDEX_NONE when there is no option with any weight.
	 */
	WEM_2026_API int32 DrawWithPull(FRandomStream& Stream, TConstArrayView<FDrawOption> Options, TConstArrayView<FNeighbourOffer> Offers);
}
