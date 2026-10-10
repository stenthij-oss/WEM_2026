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

bool WEMStructures::IsWall(const int32 NormalAxis, const EGridFace Up)
{
	return FaceAxis(Up) != NormalAxis;
}

EStructureType WEMStructures::GetFaceType(const int32 NormalAxis, const int32 FaceSign, const EGridFace Up)
{
	if (IsWall(NormalAxis, Up))
	{
		return EStructureType::Wall;
	}

	return FaceSign == WEMGrid::FaceSign(Up) ? EStructureType::Ground : EStructureType::Ceiling;
}

bool WEMStructures::FitsShape(
	const int32 Width,
	const int32 Length,
	const bool bMayLieSideways,
	const int32 NormalAxis,
	const EGridFace Up,
	const int32 SpanFirst,
	const int32 SpanSecond,
	bool& bOutTurned)
{
	bOutTurned = false;

	const bool bFitsAsAuthored = Width == SpanFirst && Length == SpanSecond;
	const bool bFitsTurned = Width == SpanSecond && Length == SpanFirst;

	// A floor or ceiling has no up across it, so a structure lies across it either way - as authored
	// first, so a square structure on a square opening is never turned for nothing.
	if (!IsWall(NormalAxis, Up))
	{
		bOutTurned = !bFitsAsAuthored && bFitsTurned;
		return bFitsAsAuthored || bFitsTurned;
	}

	// A wall does. Its structure stands with its length along Up, which is the way round that leaves
	// its length on whichever in-plane axis Up lies along - and only one made to lie on its side may lie
	// the other way: a facade turned over is no longer a facade.
	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	const bool bUprightTurned = FaceAxis(Up) == U;
	const bool bFitsUpright = bUprightTurned ? bFitsTurned : bFitsAsAuthored;
	const bool bFitsLying = bUprightTurned ? bFitsAsAuthored : bFitsTurned;

	if (bFitsUpright)
	{
		bOutTurned = bUprightTurned;
		return true;
	}

	if (bFitsLying && bMayLieSideways)
	{
		bOutTurned = !bUprightTurned;
		return true;
	}

	return false;
}

bool WEMStructures::FitsFace(
	const UPlatformStructureData& Structure,
	const int32 NormalAxis,
	const int32 FaceSign,
	const EGridFace Up,
	const int32 SpanFirst,
	const int32 SpanSecond,
	bool& bOutTurned)
{
	bOutTurned = false;

	if (!Structure.HasType(GetFaceType(NormalAxis, FaceSign, Up)))
	{
		return false;
	}

	// A two-sided structure is both faces at once, so it has to be allowed on the other one as well.
	if (Structure.bTwoSided && !Structure.HasType(GetFaceType(NormalAxis, -FaceSign, Up)))
	{
		return false;
	}

	return FitsShape(Structure.Width, Structure.Length, Structure.bMayLieSideways, NormalAxis, Up, SpanFirst, SpanSecond, bOutTurned);
}

namespace
{
	/** Draws an index by weight. Returns INDEX_NONE when no weight is above zero. */
	template <typename WeightFunctorType>
	int32 DrawByWeight(FRandomStream& Stream, const int32 Count, WeightFunctorType&& GetWeight)
	{
		double TotalWeight = 0.0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			TotalWeight += FMath::Max(GetWeight(Index), 0.0);
		}

		if (TotalWeight <= 0.0)
		{
			return INDEX_NONE;
		}

		double Roll = Stream.FRand() * TotalWeight;
		int32 Chosen = INDEX_NONE;

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Weight = FMath::Max(GetWeight(Index), 0.0);

			if (Weight <= 0.0)
			{
				continue;
			}

			// Taken whenever it is in the running, so rounding at the very top of the roll still lands
			// on the last one that could have been drawn.
			Chosen = Index;

			if (Roll < Weight)
			{
				break;
			}

			Roll -= Weight;
		}

		return Chosen;
	}
}

FTransform WEMStructures::MakeStructureTransform(
	const FVector& MinCorner,
	const double CellSize,
	const int32 NormalAxis,
	const EGridFace Up,
	const int32 SpanFirst,
	const int32 SpanSecond,
	const int32 FaceSign,
	const bool bTurned,
	const bool bTwoSided)
{
	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// Where Blender's three axes go in the world. Z looks out of the face. Y runs along the axis the
	// structure's length does - the second unless it is turned, when it is the first. Standing on a
	// wall that is the wall's Up, and Y points the way Up does, so a wall upside down in the world
	// carries its structures upside down too; anywhere else it points along the axis.
	const int32 TopAxis = bTurned ? U : V;
	const int32 TopSign = IsWall(NormalAxis, Up) && FaceAxis(Up) == TopAxis ? WEMGrid::FaceSign(Up) : 1;

	const FVector Out(AxisStep(NormalAxis, FaceSign > 0 ? 1 : -1));
	const FVector Top(AxisStep(TopAxis, TopSign));

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

float WEMStructures::GetContinueChance(const int32 Streak, const float ContinueChance, const float MaxContinueChance, const int32 StreakForMaxChance)
{
	const int32 Steps = FMath::Max(StreakForMaxChance, 2) - 1;
	const float Progress = static_cast<float>(FMath::Clamp(Streak - 1, 0, Steps)) / static_cast<float>(Steps);

	return FMath::Clamp(ContinueChance + (MaxContinueChance - ContinueChance) * Progress, 0.0f, 1.0f);
}

int32 WEMStructures::DrawWithPull(FRandomStream& Stream, const TConstArrayView<FDrawOption> Options, const TConstArrayView<FNeighbourOffer> Offers)
{
	// A category no option carries could never be drawn, so it pulls toward nothing.
	TSet<int32> LiveCategories;
	for (const FDrawOption& Option : Options)
	{
		if (Option.Weight > 0.0f)
		{
			LiveCategories.Append(Option.Categories);
		}
	}

	// Each neighbour's offer: its live categories, and the strongest of their chances.
	struct FLiveOffer
	{
		TArray<TPair<int32, float>, TInlineAllocator<4>> Categories;
		float Chance = 0.0f;
	};

	TArray<FLiveOffer, TInlineAllocator<16>> LiveOffers;
	double Stay = 1.0;

	for (const FNeighbourOffer& Offer : Offers)
	{
		FLiveOffer Live;

		for (const TPair<int32, float>& Category : Offer.Categories)
		{
			if (LiveCategories.Contains(Category.Key) && Category.Value > 0.0f)
			{
				Live.Categories.Add(Category);
				Live.Chance = FMath::Max(Live.Chance, Category.Value);
			}
		}

		if (Live.Categories.IsEmpty())
		{
			continue;
		}

		Stay *= 1.0 - FMath::Min(Live.Chance, 1.0f);
		LiveOffers.Add(MoveTemp(Live));
	}

	// The pull is only rolled for when something pulls, so a face with no filled neighbour draws as it
	// would with no categories at all.
	if (!LiveOffers.IsEmpty() && Stream.FRand() < 1.0 - Stay)
	{
		const int32 Neighbour = DrawByWeight(Stream, LiveOffers.Num(), [&LiveOffers](const int32 Index)
		{
			return static_cast<double>(LiveOffers[Index].Chance);
		});

		const FLiveOffer& Offer = LiveOffers[Neighbour];
		const int32 CategoryIndex = DrawByWeight(Stream, Offer.Categories.Num(), [&Offer](const int32 Index)
		{
			return static_cast<double>(Offer.Categories[Index].Value);
		});

		const int32 Category = Offer.Categories[CategoryIndex].Key;
		const int32 Chosen = DrawByWeight(Stream, Options.Num(), [&Options, Category](const int32 Index)
		{
			return Options[Index].Categories.Contains(Category) ? static_cast<double>(Options[Index].Weight) : 0.0;
		});

		if (Chosen != INDEX_NONE)
		{
			return Chosen;
		}
	}

	return DrawByWeight(Stream, Options.Num(), [&Options](const int32 Index)
	{
		return static_cast<double>(Options[Index].Weight);
	});
}
