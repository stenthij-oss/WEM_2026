// Copyright Epic Games, Inc. All Rights Reserved.

#include "StructurePlacement.h"

#include "GridMath.h"

using namespace WEMGrid;

namespace
{
	/**
	 * Blender's X, Y and Z as the importer lays them in a mesh's own frame, measured from the
	 * calibration F: a glTF exported from Blender with +Y up comes in with X kept, Y turned round and
	 * Z kept, so Blender's right-handed axes land in Unreal's left-handed ones without the mesh
	 * itself changing - an F that reads correctly from above in Blender still does in Unreal.
	 */
	const FVector MeshAxesForBlenderAxes[3] =
	{
		FVector(1.0, 0.0, 0.0),
		FVector(0.0, -1.0, 0.0),
		FVector(0.0, 0.0, 1.0)
	};
}

FVector WEMStructures::GetMeshAxisForBlenderAxis(const int32 BlenderAxis)
{
	return MeshAxesForBlenderAxes[FMath::Clamp(BlenderAxis, 0, 2)];
}

EStructureType WEMStructures::GetFaceType(const int32 NormalAxis, const int32 FaceSign)
{
	if (NormalAxis != 2)
	{
		return EStructureType::Wall;
	}

	return FaceSign > 0 ? EStructureType::Ground : EStructureType::Ceiling;
}

bool WEMStructures::FitsShape(
	const int32 Width,
	const int32 Length,
	const bool bMayLieSideways,
	const int32 NormalAxis,
	const int32 SpanFirst,
	const int32 SpanSecond,
	bool& bOutTurned)
{
	bOutTurned = false;

	// As authored first, so a square structure on a square opening is never turned for nothing.
	if (Width == SpanFirst && Length == SpanSecond)
	{
		return true;
	}

	// A floor or ceiling has no up, so a structure lies across it either way. A wall does, and only a
	// structure made to lie on its side may: a facade turned over is no longer a facade.
	if (Width == SpanSecond && Length == SpanFirst && (NormalAxis == 2 || bMayLieSideways))
	{
		bOutTurned = true;
		return true;
	}

	return false;
}

bool WEMStructures::FitsFace(
	const UPlatformStructureData& Structure,
	const int32 NormalAxis,
	const int32 FaceSign,
	const int32 SpanFirst,
	const int32 SpanSecond,
	bool& bOutTurned)
{
	bOutTurned = false;

	if (!Structure.HasType(GetFaceType(NormalAxis, FaceSign)))
	{
		return false;
	}

	// A two-sided structure is both faces at once, so it has to be allowed on the other one as well.
	if (Structure.bTwoSided && !Structure.HasType(GetFaceType(NormalAxis, -FaceSign)))
	{
		return false;
	}

	return FitsShape(Structure.Width, Structure.Length, Structure.bMayLieSideways, NormalAxis, SpanFirst, SpanSecond, bOutTurned);
}

FTransform WEMStructures::MakeStructureTransform(
	const FVector& MinCorner,
	const double CellSize,
	const int32 NormalAxis,
	const int32 SpanFirst,
	const int32 SpanSecond,
	const int32 FaceSign,
	const bool bTurned,
	const bool bTwoSided)
{
	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// Where Blender's three axes go in the world. Z looks out of the face. Y runs along the second
	// axis - up a wall, along the length across a floor - unless the structure is turned, when its
	// length runs along the first: the level axis of a wall it lies on its side across.
	const FVector Out(AxisStep(NormalAxis, FaceSign > 0 ? 1 : -1));
	const FVector Top(AxisStep(bTurned ? U : V));

	// X completes the frame so that the mesh's own frame is turned and never mirrored. The importer's
	// frame is itself a mirror of Blender's, so Blender's X lands on whichever side keeps the whole a
	// proper rotation.
	const FVector MeshX = GetMeshAxisForBlenderAxis(0);
	const FVector MeshY = GetMeshAxisForBlenderAxis(1);
	const FVector MeshZ = GetMeshAxisForBlenderAxis(2);
	const double ImportHandedness = FVector::DotProduct(MeshX, FVector::CrossProduct(MeshY, MeshZ)) > 0.0 ? 1.0 : -1.0;

	const FVector BlenderAxesInWorld[3] = { ImportHandedness * FVector::CrossProduct(Top, Out), Top, Out };
	const FVector MeshAxesForBlender[3] = { MeshX, MeshY, MeshZ };

	// The mesh's own axes in the world: each is made of Blender's, as far as the importer laid it along them.
	FVector MeshAxesInWorld[3];
	for (int32 MeshAxis = 0; MeshAxis < 3; ++MeshAxis)
	{
		MeshAxesInWorld[MeshAxis] = FVector::ZeroVector;

		for (int32 BlenderAxis = 0; BlenderAxis < 3; ++BlenderAxis)
		{
			MeshAxesInWorld[MeshAxis] += MeshAxesForBlender[BlenderAxis][MeshAxis] * BlenderAxesInWorld[BlenderAxis];
		}
	}

	const FMatrix Rotation(MeshAxesInWorld[0], MeshAxesInWorld[1], MeshAxesInWorld[2], FVector::ZeroVector);

	// The opening's centre, halfway between the beams' centre lines, on the segment's mid-plane.
	FVector Centre = MinCorner;
	Centre[U] += 0.5 * static_cast<double>(SpanFirst + 1) * CellSize;
	Centre[V] += 0.5 * static_cast<double>(SpanSecond + 1) * CellSize;
	Centre[NormalAxis] += 0.5 * CellSize;

	if (bTwoSided)
	{
		Centre -= Out * (0.5 * CellSize);
	}

	return FTransform(Rotation.ToQuat(), Centre);
}
