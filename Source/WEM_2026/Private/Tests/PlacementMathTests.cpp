// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMath.h"
#include "Misc/AutomationTest.h"
#include "PlacementCatalog.h"
#include "PlacementMath.h"
#include "PlacementTestWorld.h"

namespace PlacementMathTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** How far two positions in centimetres may disagree and still be the same place. */
	constexpr double Tolerance = 1.0e-3;

	int32 Dot(const FVector& A, const FIntVector& B)
	{
		return FMath::RoundToInt32(A.X * B.X + A.Y * B.Y + A.Z * B.Z);
	}

	FIntVector RoundToDirection(const FVector& Vector)
	{
		return FIntVector(FMath::RoundToInt32(Vector.X), FMath::RoundToInt32(Vector.Y), FMath::RoundToInt32(Vector.Z));
	}

	FSpecialEdge MakeSpecialEdge(const int32 Id, const EObjectSide Side, const int32 Start, const int32 Count)
	{
		FSpecialEdge Edge;
		Edge.Id = Id;
		Edge.Side = Side;
		Edge.Start = Start;
		Edge.Count = Count;
		return Edge;
	}

	void GetBoxCorners(const FBox& Box, FVector (&OutCorners)[8])
	{
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			OutCorners[Corner] = FVector(
				(Corner & 1) ? Box.Max.X : Box.Min.X,
				(Corner & 2) ? Box.Max.Y : Box.Min.Y,
				(Corner & 4) ? Box.Max.Z : Box.Min.Z);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementMathSizeCellsTest, "WEM.Placement.Math.SizeCells", PlacementMathTestsPrivate::TestFlags)

bool FPlacementMathSizeCellsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("A 60 cm cube is three cells a side"), WEMPlacement::ComputeSizeCells(FVector(60.0), 20.0), FIntVector(3, 3, 3));
	TestEqual(TEXT("59.9 x 60 x 60.0001 is still three cells a side"), WEMPlacement::ComputeSizeCells(FVector(59.9, 60.0, 60.0001), 20.0), FIntVector(3, 3, 3));
	TestEqual(TEXT("A 60 x 120 x 75 desk is 3 x 6 x 4"), WEMPlacement::ComputeSizeCells(FVector(60.0, 120.0, 75.0), 20.0), FIntVector(3, 6, 4));
	TestEqual(TEXT("Nothing is smaller than one cell"), WEMPlacement::ComputeSizeCells(FVector(0.0, 1.0, 5.0), 20.0), FIntVector(1, 1, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementMathEdgeFacesTest, "WEM.Placement.Math.EdgeFaces", PlacementMathTestsPrivate::TestFlags)

bool FPlacementMathEdgeFacesTest::RunTest(const FString& Parameters)
{
	using namespace WEMPlacement;

	// A single cell has a face on every side, each the whole of its edge and so a corner of it.
	{
		TArray<FEdgeFace> Faces;
		GatherEdgeFaces(FIntVector(1, 1, 1), {}, Faces);

		TestEqual(TEXT("A 1x1 footprint has four edge faces"), Faces.Num(), 4);

		TSet<EObjectSide> Sides;
		for (const FEdgeFace& Face : Faces)
		{
			Sides.Add(Face.Side);
			TestTrue(TEXT("Each of a 1x1's faces is a corner"), Face.bCorner);
			TestEqual(TEXT("Each of a 1x1's faces is first along its edge"), Face.Index, 0);
		}

		TestEqual(TEXT("A 1x1's four faces are on four distinct sides"), Sides.Num(), 4);
	}

	// The desk from the brief: 3 deep, 6 wide.
	{
		const FIntVector Size(3, 6, 4);

		TArray<FEdgeFace> Faces;
		GatherEdgeFaces(Size, {}, Faces);

		TestEqual(TEXT("A 3x6 footprint has 18 edge faces"), Faces.Num(), 18);

		TMap<EObjectSide, int32> Counts;
		for (const FEdgeFace& Face : Faces)
		{
			++Counts.FindOrAdd(Face.Side);

			const int32 Length = GetEdgeLength(Size, Face.Side);
			TestEqual(TEXT("A face is a corner exactly at either end of its edge"), Face.bCorner, Face.Index == 0 || Face.Index == Length - 1);

			const FIntVector Direction = GetSideDirection(Face.Side);
			TestEqual(TEXT("A face looks out the way its side does"), Face.Direction, FIntPoint(Direction.X, Direction.Y));

			switch (Face.Side)
			{
			case EObjectSide::Front:
				TestEqual(TEXT("Front faces are on the last row of x"), Face.Cell.X, 2);
				TestEqual(TEXT("Front runs along y, left to right"), Face.Index, Face.Cell.Y);
				break;
			case EObjectSide::Back:
				TestEqual(TEXT("Back faces are on the first row of x"), Face.Cell.X, 0);
				TestEqual(TEXT("Back runs along y, left to right"), Face.Index, Face.Cell.Y);
				break;
			case EObjectSide::Left:
				TestEqual(TEXT("Left faces are at y = 0"), Face.Cell.Y, 0);
				TestEqual(TEXT("Left runs along x, back to front"), Face.Index, Face.Cell.X);
				break;
			case EObjectSide::Right:
				TestEqual(TEXT("Right faces are at y = 5"), Face.Cell.Y, 5);
				TestEqual(TEXT("Right runs along x, back to front"), Face.Index, Face.Cell.X);
				break;
			default:
				AddError(TEXT("An edge face was given the top or bottom"));
				break;
			}
		}

		TestEqual(TEXT("Front has six faces"), Counts.FindRef(EObjectSide::Front), 6);
		TestEqual(TEXT("Back has six faces"), Counts.FindRef(EObjectSide::Back), 6);
		TestEqual(TEXT("Left has three faces"), Counts.FindRef(EObjectSide::Left), 3);
		TestEqual(TEXT("Right has three faces"), Counts.FindRef(EObjectSide::Right), 3);

		const int32 CornerCellFaces = Faces.FilterByPredicate([](const FEdgeFace& Face) { return Face.Cell == FIntPoint(0, 0); }).Num();
		TestEqual(TEXT("A corner cell gives one face to each of two edges"), CornerCellFaces, 2);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementMathFramesTest, "WEM.Placement.Math.Frames", PlacementMathTestsPrivate::TestFlags)

bool FPlacementMathFramesTest::RunTest(const FString& Parameters)
{
	using namespace WEMPlacement;
	using namespace PlacementMathTestsPrivate;

	FPlacementTestWorld World;
	const double CellSize = World.CellSize;
	const FIntVector SurfaceCell(24, 24, 24);

	// Odd sizes on purpose, and a mesh whose pivot sits nowhere near its bounds, so neither can
	// hide a mistake in the other.
	const FIntVector Sizes[] = { FIntVector(1, 1, 1), FIntVector(3, 6, 4), FIntVector(2, 5, 3), FIntVector(4, 3, 7) };

	for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
	{
		const EGridFace Face = static_cast<EGridFace>(FaceIndex);
		const FPlaneAxes Plane = MakeSegmentPlaneAxes(Face);
		const int32 Layer = SurfaceCell[WEMGrid::FaceAxis(Face)];
		const FIntPoint Origin = WorldToSegmentPlaneCell(Face, SurfaceCell);
		const FVector Normal(Plane.Normal);

		TestEqual(TEXT("A segment plane's cell maps back to the world cell it came from"), SegmentPlaneCellToWorld(Face, Layer, Origin), SurfaceCell);

		for (const FIntVector& Size : Sizes)
		{
			const FVector BoundsSize = FVector(Size) * CellSize - FVector(3.0, 5.0, 1.0);
			const FBox Bounds(FVector(-13.0, 7.0, -2.0), FVector(-13.0, 7.0, -2.0) + BoundsSize);

			for (int32 Rotation = 0; Rotation < 4; ++Rotation)
			{
				const FPoseFrame Frame = MakePoseFrame(Plane, Rotation);
				const FString Context = FString::Printf(TEXT("%s, size %s, rotation %d"), *DescribeDirection(Plane.Normal), *Size.ToString(), Rotation);

				auto ToWorldCell = [&](const FIntPoint& LocalCell)
				{
					return SegmentPlaneCellToWorld(Face, Layer, LocalToPlaneCell(Frame, Origin, LocalCell));
				};

				const FVector FirstCentre = World.GetFaceCentre(ToWorldCell(FIntPoint(0, 0)), Face);
				const FVector LastCentre = World.GetFaceCentre(ToWorldCell(FIntPoint(Size.X - 1, Size.Y - 1)), Face);
				const FVector FootprintCentre = 0.5 * (FirstCentre + LastCentre);

				const FTransform Transform = MakeObjectWorldTransform(Frame, FootprintCentre, Bounds, Size, CellSize, 0);

				TestEqual(*(Context + TEXT(": the mesh's X is the pose's front")), RoundToDirection(Transform.GetRotation().GetAxisX()), Frame.Forward);
				TestEqual(*(Context + TEXT(": the mesh's Y is the pose's right")), RoundToDirection(Transform.GetRotation().GetAxisY()), Frame.Right);
				TestEqual(*(Context + TEXT(": the mesh's Z is the face's normal")), RoundToDirection(Transform.GetRotation().GetAxisZ()), Plane.Normal);

				// The pose's volume: the footprint's cells, carried up off the face one layer at a time.
				FBox Volume(ForceInit);
				for (int32 X = 0; X < Size.X; ++X)
				{
					for (int32 Y = 0; Y < Size.Y; ++Y)
					{
						for (int32 Height = 1; Height <= Size.Z; ++Height)
						{
							Volume += World.GetCellBox(ToWorldCell(FIntPoint(X, Y)) + Plane.Normal * Height);
						}
					}
				}

				FVector Corners[8];
				GetBoxCorners(Bounds, Corners);

				double Lowest = MAX_dbl;
				bool bInside = true;

				for (const FVector& Corner : Corners)
				{
					const FVector WorldCorner = Transform.TransformPosition(Corner);
					bInside = bInside && Volume.ExpandBy(Tolerance).IsInsideOrOn(WorldCorner);
					Lowest = FMath::Min(Lowest, FVector::DotProduct(WorldCorner, Normal));
				}

				TestTrue(*(Context + TEXT(": the rotated bounds land inside the pose's volume cells")), bInside);
				TestEqual(*(Context + TEXT(": the bottom lies on the support face")), Lowest, FVector::DotProduct(FirstCentre, Normal), Tolerance);

				// Every footprint cell of the mesh, as the catalog measured it, sits over the plane cell
				// the pose maps it to: the footprint mapping and the mesh rotation agree.
				bool bCellsAgree = true;
				for (int32 X = 0; X < Size.X; ++X)
				{
					for (int32 Y = 0; Y < Size.Y; ++Y)
					{
						const FVector Measured = Transform.TransformPosition(GetFootprintCellLocalCentre(Bounds, Size, CellSize, FIntPoint(X, Y), 0.0));
						const FVector Mapped = World.GetFaceCentre(ToWorldCell(FIntPoint(X, Y)), Face);
						bCellsAgree = bCellsAgree && Measured.Equals(Mapped, Tolerance);
					}
				}

				TestTrue(*(Context + TEXT(": each footprint cell of the mesh sits over the plane cell the pose gives it")), bCellsAgree);

				// The faces labelled Right look out along the mesh's own +Y, and those labelled Front along its +X.
				TArray<FEdgeFace> EdgeFaces;
				GatherEdgeFaces(Size, {}, EdgeFaces);

				for (const FEdgeFace& EdgeFace : EdgeFaces)
				{
					const FIntVector WorldDirection = PlaneToWorldDirection(Plane, LocalToPlaneDirection(Frame, EdgeFace.Direction));

					if (EdgeFace.Side == EObjectSide::Right)
					{
						TestEqual(*(Context + TEXT(": Right faces look along the mesh's +Y")), WorldDirection, RoundToDirection(Transform.GetRotation().GetAxisY()));
					}
					else if (EdgeFace.Side == EObjectSide::Front)
					{
						TestEqual(*(Context + TEXT(": Front faces look along the mesh's +X")), WorldDirection, RoundToDirection(Transform.GetRotation().GetAxisX()));
					}
				}

				// Every side can be turned to face every way across the plane.
				for (const FIntPoint& Direction : PlaneDirections)
				{
					const int32 Facing = FindRotationFacing(Plane, EObjectSide::Back, Direction);
					const FPoseFrame Turned = MakePoseFrame(Plane, Facing);
					TestEqual(*(Context + TEXT(": the rotation found turns Back the asked way")), LocalToPlaneDirection(Turned, FIntPoint(-1, 0)), Direction);
				}

				// Slid against a back wall and a left wall, the mesh's back and left sit on the footprint's.
				const FTransform Slid = MakeObjectWorldTransform(
					Frame, FootprintCentre, Bounds, Size, CellSize, SideBit(EObjectSide::Back) | SideBit(EObjectSide::Left));

				double Backmost = MAX_dbl;
				double Leftmost = MAX_dbl;

				for (const FVector& Corner : Corners)
				{
					const FVector WorldCorner = Slid.TransformPosition(Corner);
					Backmost = FMath::Min(Backmost, FVector::DotProduct(WorldCorner, FVector(Frame.Forward)));
					Leftmost = FMath::Min(Leftmost, FVector::DotProduct(WorldCorner, FVector(Frame.Right)));
				}

				TestEqual(*(Context + TEXT(": slid back, the mesh's back is on the footprint's")),
					Backmost, FVector::DotProduct(FirstCentre, FVector(Frame.Forward)) - 0.5 * CellSize, Tolerance);
				TestEqual(*(Context + TEXT(": slid left, the mesh's left is on the footprint's")),
					Leftmost, FVector::DotProduct(FirstCentre, FVector(Frame.Right)) - 0.5 * CellSize, Tolerance);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementCatalogEffectiveGeometryTest, "WEM.Placement.Catalog.EffectiveGeometry", PlacementMathTestsPrivate::TestFlags)

bool FPlacementCatalogEffectiveGeometryTest::RunTest(const FString& Parameters)
{
	FPlacementCatalogRow Row;
	Row.Geometry.SizeCells = FIntVector(3, 6, 4);
	Row.Geometry.OpenBelowCm.Init(71.0f, 18);
	Row.Geometry.FootprintMask.Init(1, 18);
	Row.Geometry.SpecialEdges.Add(PlacementMathTestsPrivate::MakeSpecialEdge(1, EObjectSide::Front, 1, 4));

	FDerivedSurfaceLayer& Top = Row.Geometry.Surfaces.AddDefaulted_GetRef();
	Top.HeightCm = 75.0f;
	Top.CellMask.Init(1, 18);

	{
		const FPlacementGeometry Effective = Row.GetEffectiveGeometry();
		TestEqual(TEXT("Without overrides the derived geometry stands"), Effective.Surfaces.Num(), 1);
		TestEqual(TEXT("Without overrides the special edge stands"), Effective.SpecialEdges.Num(), 1);
	}

	Row.bOverrideSpecialEdges = true;
	Row.OverrideSpecialEdges.Add(PlacementMathTestsPrivate::MakeSpecialEdge(2, EObjectSide::Back, 0, 6));

	{
		const FPlacementGeometry Effective = Row.GetEffectiveGeometry();
		TestTrue(TEXT("An override replaces the derived special edges"), Effective.SpecialEdges.Num() == 1 && Effective.SpecialEdges[0].Id == 2);
	}

	Row.bOverrideSize = true;
	Row.OverrideSizeCells = FIntVector(3, 4, 4);

	{
		const FPlacementGeometry Effective = Row.GetEffectiveGeometry();
		TestEqual(TEXT("A size override sets the size"), Effective.SizeCells, FIntVector(3, 4, 4));
		TestEqual(TEXT("Surfaces measured on the old footprint no longer fit, and are dropped"), Effective.Surfaces.Num(), 0);
		TestEqual(TEXT("A special edge longer than its new edge is dropped"), Effective.SpecialEdges.Num(), 0);
		TestEqual(TEXT("Per-cell data is resized to the new footprint"), Effective.OpenBelowCm.Num(), 12);
	}

	Row.bOverrideSet = true;
	Row.SetName = TEXT("Oak");
	Row.OverrideSetName = TEXT("Walnut");
	TestEqual(TEXT("A set override wins over the derived set"), Row.GetEffectiveSetName(), FName(TEXT("Walnut")));

	return true;
}

#endif
