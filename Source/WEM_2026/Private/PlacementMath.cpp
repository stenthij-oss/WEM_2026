// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementMath.h"

#include "GridMath.h"
#include "PlacementCatalog.h"

namespace WEMPlacement
{
	int32 GetPlaneDirectionIndex(const FIntPoint& Direction)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (PlaneDirections[Index] == Direction)
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	FIntVector GetSideDirection(const EObjectSide Side)
	{
		switch (Side)
		{
		case EObjectSide::Front: return FIntVector(1, 0, 0);
		case EObjectSide::Back: return FIntVector(-1, 0, 0);
		case EObjectSide::Right: return FIntVector(0, 1, 0);
		case EObjectSide::Left: return FIntVector(0, -1, 0);
		case EObjectSide::Top: return FIntVector(0, 0, 1);
		default: return FIntVector(0, 0, -1);
		}
	}

	bool IsEdgeSide(const EObjectSide Side)
	{
		return GetSideDirection(Side).Z == 0;
	}

	bool FindEdgeSide(const FIntPoint& LocalDirection, EObjectSide& OutSide)
	{
		// Read back from GetSideDirection, so the sides are only ever named in one place.
		for (const EObjectSide Side : { EObjectSide::Front, EObjectSide::Back, EObjectSide::Right, EObjectSide::Left })
		{
			const FIntVector Direction = GetSideDirection(Side);

			if (Direction.X == LocalDirection.X && Direction.Y == LocalDirection.Y)
			{
				OutSide = Side;
				return true;
			}
		}

		return false;
	}

	FIntVector ComputeSizeCells(const FVector& SizeCm, const double CellSize)
	{
		FIntVector Cells(1, 1, 1);

		if (CellSize <= 0.0)
		{
			return Cells;
		}

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Cells[Axis] = FMath::Max(1, FMath::CeilToInt32(SizeCm[Axis] / CellSize - 0.001));
		}

		return Cells;
	}

	void GatherEdgeFaces(const FIntVector& SizeCells, const TConstArrayView<uint8> Mask, TArray<FEdgeFace>& OutFaces)
	{
		OutFaces.Reset();

		const bool bMasked = Mask.Num() == GetFootprintCellCount(SizeCells);

		auto IsInFootprint = [&](const int32 X, const int32 Y)
		{
			if (X < 0 || Y < 0 || X >= SizeCells.X || Y >= SizeCells.Y)
			{
				return false;
			}

			return !bMasked || Mask[GetFootprintIndex(SizeCells, X, Y)] != 0;
		};

		for (int32 X = 0; X < SizeCells.X; ++X)
		{
			for (int32 Y = 0; Y < SizeCells.Y; ++Y)
			{
				if (!IsInFootprint(X, Y))
				{
					continue;
				}

				for (const FIntPoint& Direction : PlaneDirections)
				{
					if (IsInFootprint(X + Direction.X, Y + Direction.Y))
					{
						continue;
					}

					FEdgeFace& Face = OutFaces.AddDefaulted_GetRef();
					Face.Cell = FIntPoint(X, Y);
					Face.Direction = Direction;
					FindEdgeSide(Direction, Face.Side);

					// An edge runs across the direction its faces look out along, so its index is the
					// other coordinate.
					Face.Index = Direction.X != 0 ? Y : X;
				}
			}
		}

		// A face is a corner when it is the first or last of its side's run. Worked out over what was
		// gathered rather than from the rectangle, so it holds for a footprint of any shape.
		for (FEdgeFace& Face : OutFaces)
		{
			int32 First = MAX_int32;
			int32 Last = MIN_int32;

			for (const FEdgeFace& Other : OutFaces)
			{
				const bool bSameRun = Other.Side == Face.Side
					&& (Face.Direction.X != 0 ? Other.Cell.X == Face.Cell.X : Other.Cell.Y == Face.Cell.Y);

				if (bSameRun)
				{
					First = FMath::Min(First, Other.Index);
					Last = FMath::Max(Last, Other.Index);
				}
			}

			Face.bCorner = Face.Index == First || Face.Index == Last;
		}
	}

	void GatherZoneCells(const FIntVector& SizeCells, const FExclusionZone& Zone, TArray<FIntPoint>& OutCells)
	{
		OutCells.Reset();

		const int32 Depth = FMath::Max(1, Zone.DepthCells);

		TArray<FEdgeFace> Faces;
		GatherEdgeFaces(SizeCells, {}, Faces);

		for (const FEdgeFace& Face : Faces)
		{
			if ((Zone.SideMask & SideBit(Face.Side)) != 0)
			{
				for (int32 Step = 1; Step <= Depth; ++Step)
				{
					OutCells.Add(Face.Cell + Face.Direction * Step);
				}
			}
		}

		if (!Zone.bIncludeCorners)
		{
			return;
		}

		for (const EObjectSide Along : { EObjectSide::Front, EObjectSide::Back })
		{
			for (const EObjectSide Across : { EObjectSide::Right, EObjectSide::Left })
			{
				if ((Zone.SideMask & SideBit(Along)) == 0 || (Zone.SideMask & SideBit(Across)) == 0)
				{
					continue;
				}

				const FIntVector AlongDirection = GetSideDirection(Along);
				const FIntVector AcrossDirection = GetSideDirection(Across);
				const FIntPoint Corner(AlongDirection.X > 0 ? SizeCells.X - 1 : 0, AcrossDirection.Y > 0 ? SizeCells.Y - 1 : 0);

				for (int32 StepAlong = 1; StepAlong <= Depth; ++StepAlong)
				{
					for (int32 StepAcross = 1; StepAcross <= Depth; ++StepAcross)
					{
						OutCells.Add(Corner + FIntPoint(AlongDirection.X * StepAlong, AcrossDirection.Y * StepAcross));
					}
				}
			}
		}
	}

	int32 GetEdgeLength(const FIntVector& SizeCells, const EObjectSide Side)
	{
		const FIntVector Direction = GetSideDirection(Side);

		if (Direction.Z != 0)
		{
			return 0;
		}

		return Direction.X != 0 ? SizeCells.Y : SizeCells.X;
	}

	FPlaneAxes MakeSegmentPlaneAxes(const EGridFace Face)
	{
		int32 AxisA, AxisB;
		WEMGrid::GetInPlaneAxes(WEMGrid::FaceAxis(Face), AxisA, AxisB);

		FPlaneAxes Plane;
		Plane.Normal = WEMGrid::FaceStep(Face);
		Plane.AxisA = WEMGrid::AxisStep(AxisA);
		Plane.AxisB = WEMGrid::AxisStep(AxisB);
		return Plane;
	}

	FIntVector SegmentPlaneCellToWorld(const EGridFace Face, const int32 Layer, const FIntPoint& PlaneCell)
	{
		int32 AxisA, AxisB;
		WEMGrid::GetInPlaneAxes(WEMGrid::FaceAxis(Face), AxisA, AxisB);

		FIntVector Cell;
		Cell[WEMGrid::FaceAxis(Face)] = Layer;
		Cell[AxisA] = PlaneCell.X;
		Cell[AxisB] = PlaneCell.Y;
		return Cell;
	}

	FIntPoint WorldToSegmentPlaneCell(const EGridFace Face, const FIntVector& Cell)
	{
		int32 AxisA, AxisB;
		WEMGrid::GetInPlaneAxes(WEMGrid::FaceAxis(Face), AxisA, AxisB);

		return FIntPoint(Cell[AxisA], Cell[AxisB]);
	}

	FIntVector PlaneToWorldDirection(const FPlaneAxes& Plane, const FIntPoint& PlaneDirection)
	{
		return Plane.AxisA * PlaneDirection.X + Plane.AxisB * PlaneDirection.Y;
	}

	FIntPoint WorldToPlaneDirection(const FPlaneAxes& Plane, const FIntVector& WorldDirection)
	{
		auto Dot = [](const FIntVector& A, const FIntVector& B)
		{
			return A.X * B.X + A.Y * B.Y + A.Z * B.Z;
		};

		return FIntPoint(Dot(WorldDirection, Plane.AxisA), Dot(WorldDirection, Plane.AxisB));
	}

	FIntVector CrossProduct(const FIntVector& A, const FIntVector& B)
	{
		// Written out the way FVector's operator^ is, so the two can never disagree.
		return FIntVector(
			A.Y * B.Z - A.Z * B.Y,
			A.Z * B.X - A.X * B.Z,
			A.X * B.Y - A.Y * B.X);
	}

	FPoseFrame MakePoseFrame(const FPlaneAxes& Plane, const int32 Rotation)
	{
		FPoseFrame Frame;
		Frame.PlaneForward = PlaneDirections[Rotation & 3];
		Frame.Forward = PlaneToWorldDirection(Plane, Frame.PlaneForward);
		Frame.Up = Plane.Normal;

		// A plane's two axes can lie either way round its normal - a ceiling's are mirrored from the
		// floor above it - so right is never taken as a fixed turn of front in plane coordinates. It
		// is crossed in the world, as the mesh rotation will cross it, and read back onto the plane.
		Frame.Right = CrossProduct(Frame.Up, Frame.Forward);
		Frame.PlaneRight = WorldToPlaneDirection(Plane, Frame.Right);
		return Frame;
	}

	int32 FindRotationFacing(const FPlaneAxes& Plane, const EObjectSide Side, const FIntPoint& PlaneDirection)
	{
		const FIntVector SideDirection = GetSideDirection(Side);

		if (SideDirection.Z != 0)
		{
			return INDEX_NONE;
		}

		for (int32 Rotation = 0; Rotation < 4; ++Rotation)
		{
			const FPoseFrame Frame = MakePoseFrame(Plane, Rotation);

			if (LocalToPlaneDirection(Frame, FIntPoint(SideDirection.X, SideDirection.Y)) == PlaneDirection)
			{
				return Rotation;
			}
		}

		return INDEX_NONE;
	}

	FQuat MakeWorldRotation(const FPoseFrame& Frame)
	{
		return FRotationMatrix::MakeFromXZ(FVector(Frame.Forward), FVector(Frame.Up)).ToQuat();
	}

	FVector GetFootprintCellLocalCentre(
		const FBox& LocalBounds,
		const FIntVector& SizeCells,
		const double CellSize,
		const FIntPoint& LocalCell,
		const double HeightCm)
	{
		const FVector Centre = LocalBounds.GetCenter();

		return FVector(
			Centre.X + (LocalCell.X + 0.5 - 0.5 * SizeCells.X) * CellSize,
			Centre.Y + (LocalCell.Y + 0.5 - 0.5 * SizeCells.Y) * CellSize,
			LocalBounds.Min.Z + HeightCm);
	}

	FTransform MakeObjectWorldTransform(
		const FPoseFrame& Frame,
		const FVector& FootprintCentreOnFace,
		const FBox& LocalBounds,
		const FIntVector& SizeCells,
		const double CellSize,
		const int32 ContactSideMask)
	{
		const FQuat Rotation = MakeWorldRotation(Frame);
		const FVector BoundsSize = LocalBounds.GetSize();
		const FVector Up(Frame.Up);

		const FVector Target = FootprintCentreOnFace + Up * (0.5 * BoundsSize.Z);
		FVector Location = Target - Rotation.RotateVector(LocalBounds.GetCenter());

		// The footprint rounds the bounds up to whole cells, which leaves a little slack round the
		// mesh. Centred, that slack would stand a mesh off whatever it was placed against.
		const double SlackX = FMath::Max(0.0, SizeCells.X * CellSize - BoundsSize.X);
		const double SlackY = FMath::Max(0.0, SizeCells.Y * CellSize - BoundsSize.Y);

		for (const EObjectSide Side : { EObjectSide::Front, EObjectSide::Back, EObjectSide::Right, EObjectSide::Left })
		{
			if ((ContactSideMask & SideBit(Side)) == 0)
			{
				continue;
			}

			const FIntVector Local = GetSideDirection(Side);
			const double Slack = Local.X != 0 ? SlackX : SlackY;
			const FVector World = FVector(Frame.Forward) * Local.X + FVector(Frame.Right) * Local.Y;

			// A contact on both opposite sides slides both ways at once and so stays centred.
			Location += World * (0.5 * Slack);
		}

		return FTransform(Rotation, Location);
	}

	FString DescribeDirection(const FIntVector& Direction)
	{
		static const TCHAR* AxisNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Direction[Axis] != 0)
			{
				return FString::Printf(TEXT("%s%s"), Direction[Axis] > 0 ? TEXT("+") : TEXT("-"), AxisNames[Axis]);
			}
		}

		return TEXT("0");
	}

	FString DescribeSegmentPlane(const EGridFace Face, const int32 Layer)
	{
		return FString::Printf(TEXT("%s@%d"), *DescribeDirection(WEMGrid::FaceStep(Face)), Layer);
	}

	const TCHAR* GetSideName(const EObjectSide Side)
	{
		switch (Side)
		{
		case EObjectSide::Front: return TEXT("Front");
		case EObjectSide::Back: return TEXT("Back");
		case EObjectSide::Right: return TEXT("Right");
		case EObjectSide::Left: return TEXT("Left");
		case EObjectSide::Top: return TEXT("Top");
		default: return TEXT("Bottom");
		}
	}
}

FPlacementGeometry FPlacementCatalogRow::GetEffectiveGeometry() const
{
	FPlacementGeometry Effective = Geometry;

	if (bOverrideSize)
	{
		Effective.SizeCells = FIntVector(
			FMath::Max(1, OverrideSizeCells.X),
			FMath::Max(1, OverrideSizeCells.Y),
			FMath::Max(1, OverrideSizeCells.Z));
	}

	const int32 CellCount = WEMPlacement::GetFootprintCellCount(Effective.SizeCells);

	// A new size leaves the derived per-cell data describing a footprint that no longer exists.
	// There is no telling how the old cells map onto the new, so anything that no longer fits is
	// dropped rather than guessed at.
	if (Effective.OpenBelowCm.Num() != CellCount)
	{
		Effective.OpenBelowCm.Init(0.0f, CellCount);
	}

	if (Effective.FootprintMask.Num() != CellCount)
	{
		Effective.FootprintMask.Init(1, CellCount);
	}

	if (bOverrideSurfaces)
	{
		Effective.Surfaces = OverrideSurfaces;
	}

	Effective.Surfaces.RemoveAll([CellCount](const FDerivedSurfaceLayer& Layer)
	{
		return Layer.CellMask.Num() != CellCount;
	});

	if (bOverrideSpecialEdges)
	{
		Effective.SpecialEdges = OverrideSpecialEdges;
	}

	Effective.SpecialEdges.RemoveAll([&Effective](const FSpecialEdge& Edge)
	{
		const int32 Length = WEMPlacement::GetEdgeLength(Effective.SizeCells, Edge.Side);
		return Edge.Count < 1 || Edge.Start < 0 || Edge.Start + Edge.Count > Length;
	});

	return Effective;
}
