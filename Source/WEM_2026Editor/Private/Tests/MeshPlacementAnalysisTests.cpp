// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MeshPlacementAnalysis.h"
#include "Misc/AutomationTest.h"
#include "PlacementMath.h"
#include "SyntheticMeshes.h"

namespace MeshPlacementAnalysisTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	int32 CountCells(const FDerivedSurfaceLayer& Layer)
	{
		int32 Count = 0;
		for (const uint8 Cell : Layer.CellMask)
		{
			Count += Cell != 0 ? 1 : 0;
		}

		return Count;
	}

	bool HasWarningContaining(const FMeshAnalysisResult& Result, const TCHAR* Text)
	{
		return Result.Warnings.ContainsByPredicate([Text](const FString& Warning) { return Warning.Contains(Text); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAnalysisBoxTest, "WEM.Placement.Analysis.Box", MeshPlacementAnalysisTestsPrivate::TestFlags)

bool FMeshAnalysisBoxTest::RunTest(const FString& Parameters)
{
	using namespace MeshPlacementAnalysisTestsPrivate;

	FMeshAnalysisSource Box;
	WEMSyntheticMeshes::AddBox(Box, FVector(-30.0, -30.0, 0.0), FVector(30.0, 30.0, 60.0));

	const FMeshAnalysisResult Result = WEMMeshAnalysis::Analyse(Box, FSurfacePolicy(), {}, FMeshAnalysisSettings());
	const FPlacementGeometry& Geometry = Result.Geometry;

	TestTrue(TEXT("The box is measured"), Result.bValid);
	TestEqual(TEXT("A 60 cm box is 3 x 3 x 3"), Geometry.SizeCells, FIntVector(3, 3, 3));

	if (TestEqual(TEXT("The box has one surface layer"), Geometry.Surfaces.Num(), 1))
	{
		TestEqual(TEXT("Its layer is at 60 cm"), Geometry.Surfaces[0].HeightCm, 60.0f, 0.01f);
		TestEqual(TEXT("Its layer has unlimited clearance"), Geometry.Surfaces[0].ClearanceCm, 0.0f);
		TestEqual(TEXT("Its layer covers all nine cells"), CountCells(Geometry.Surfaces[0]), 9);
	}

	bool bAllClosed = Geometry.OpenBelowCm.Num() == 9;
	for (const float OpenBelow : Geometry.OpenBelowCm)
	{
		bAllClosed = bAllClosed && OpenBelow == 0.0f;
	}

	TestTrue(TEXT("Nothing is open below a solid box"), bAllClosed);
	TestFalse(TEXT("A box wound the right way round raises no flipped-normals warning"), HasWarningContaining(Result, TEXT("flipped")));

	// The size rounds with a small tolerance, so a hair over a whole number of cells does not cost a cell.
	FMeshAnalysisSource Nearly;
	WEMSyntheticMeshes::AddBox(Nearly, FVector::ZeroVector, FVector(59.9, 60.0, 60.0001));

	TestEqual(TEXT("A 59.9 x 60 x 60.0001 box is 3 x 3 x 3"),
		WEMMeshAnalysis::Analyse(Nearly, FSurfacePolicy(), {}, FMeshAnalysisSettings()).Geometry.SizeCells, FIntVector(3, 3, 3));

	// Turned inside out, every line first meets a downward face.
	FMeshAnalysisSource Flipped = Box;
	WEMSyntheticMeshes::FlipWinding(Flipped);

	TestTrue(TEXT("A box wound inside out is warned about"),
		HasWarningContaining(WEMMeshAnalysis::Analyse(Flipped, FSurfacePolicy(), {}, FMeshAnalysisSettings()), TEXT("flipped")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAnalysisDeskTest, "WEM.Placement.Analysis.Desk", MeshPlacementAnalysisTestsPrivate::TestFlags)

bool FMeshAnalysisDeskTest::RunTest(const FString& Parameters)
{
	using namespace MeshPlacementAnalysisTestsPrivate;

	FSpecialEdgeTemplate KneeHole;
	KneeHole.Id = 1;
	KneeHole.Side = EObjectSide::Front;
	KneeHole.Mode = ESpecialEdgeMode::OpenBelow;
	KneeHole.MinOpenBelowCm = 60.0f;

	const FMeshAnalysisResult Result = WEMMeshAnalysis::Analyse(WEMSyntheticMeshes::MakeDesk(), FSurfacePolicy(), { KneeHole }, FMeshAnalysisSettings());
	const FPlacementGeometry& Geometry = Result.Geometry;

	TestEqual(TEXT("The desk is 3 x 6 x 4"), Geometry.SizeCells, FIntVector(3, 6, 4));

	if (TestEqual(TEXT("The desk has one surface layer"), Geometry.Surfaces.Num(), 1))
	{
		TestEqual(TEXT("Its layer is at 75 cm"), Geometry.Surfaces[0].HeightCm, 75.0f, 0.01f);
		TestEqual(TEXT("Its layer covers all 18 cells"), CountCells(Geometry.Surfaces[0]), 18);
	}

	const FIntVector Size = Geometry.SizeCells;
	bool bOpenBelowRight = Geometry.OpenBelowCm.Num() == 18;

	for (int32 X = 0; bOpenBelowRight && X < Size.X; ++X)
	{
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			const bool bLegCell = (X == 0 || X == Size.X - 1) && (Y == 0 || Y == Size.Y - 1);
			const float OpenBelow = Geometry.OpenBelowCm[WEMPlacement::GetFootprintIndex(Size, X, Y)];

			bOpenBelowRight = bOpenBelowRight && (bLegCell ? OpenBelow == 0.0f : FMath::IsNearlyEqual(OpenBelow, 71.0f, 0.01f));
		}
	}

	TestTrue(TEXT("About 71 cm is open below every cell but the four with legs, which have none"), bOpenBelowRight);

	if (TestEqual(TEXT("The knee-hole template resolves"), Geometry.SpecialEdges.Num(), 1))
	{
		TestEqual(TEXT("The knee-hole starts one face in"), Geometry.SpecialEdges[0].Start, 1);
		TestEqual(TEXT("The knee-hole is four faces long"), Geometry.SpecialEdges[0].Count, 4);
		TestTrue(TEXT("The knee-hole is on the front"), Geometry.SpecialEdges[0].Side == EObjectSide::Front);
	}

	// Asked for more open space than the desk has, the template finds nothing and says so.
	KneeHole.MinOpenBelowCm = 80.0f;
	const FMeshAnalysisResult TooLow = WEMMeshAnalysis::Analyse(WEMSyntheticMeshes::MakeDesk(), FSurfacePolicy(), { KneeHole }, FMeshAnalysisSettings());
	TestEqual(TEXT("No run is found with 80 cm open below"), TooLow.Geometry.SpecialEdges.Num(), 0);
	TestTrue(TEXT("A template that finds no run is warned about"), HasWarningContaining(TooLow, TEXT("found no run")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAnalysisShelfTest, "WEM.Placement.Analysis.Shelf", MeshPlacementAnalysisTestsPrivate::TestFlags)

bool FMeshAnalysisShelfTest::RunTest(const FString& Parameters)
{
	using namespace MeshPlacementAnalysisTestsPrivate;

	const FMeshAnalysisResult Result = WEMMeshAnalysis::Analyse(WEMSyntheticMeshes::MakeShelf(), FSurfacePolicy(), {}, FMeshAnalysisSettings());
	const FPlacementGeometry& Geometry = Result.Geometry;

	TestEqual(TEXT("The shelf is 2 x 4 x 6"), Geometry.SizeCells, FIntVector(2, 4, 6));

	if (TestEqual(TEXT("The shelf has two layers; the plinth is too low to be one"), Geometry.Surfaces.Num(), 2))
	{
		TestEqual(TEXT("The lower plank is at 60 cm"), Geometry.Surfaces[0].HeightCm, 60.0f, 0.01f);
		TestEqual(TEXT("The lower plank has 57 cm up to the plank above"), Geometry.Surfaces[0].ClearanceCm, 57.0f, 0.01f);
		TestEqual(TEXT("The top plank is at 120 cm"), Geometry.Surfaces[1].HeightCm, 120.0f, 0.01f);
		TestEqual(TEXT("The top plank has unlimited clearance"), Geometry.Surfaces[1].ClearanceCm, 0.0f);
		TestEqual(TEXT("The lower plank covers every cell"), CountCells(Geometry.Surfaces[0]), 8);
	}

	FSurfacePolicy TopOnly;
	TopOnly.bTopLayerOnly = true;
	const FMeshAnalysisResult Top = WEMMeshAnalysis::Analyse(WEMSyntheticMeshes::MakeShelf(), TopOnly, {}, FMeshAnalysisSettings());
	TestTrue(TEXT("Top layer only keeps just the top plank"), Top.Geometry.Surfaces.Num() == 1 && FMath::IsNearlyEqual(Top.Geometry.Surfaces[0].HeightCm, 120.0f, 0.01f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAnalysisCoverageTest, "WEM.Placement.Analysis.Coverage", MeshPlacementAnalysisTestsPrivate::TestFlags)

bool FMeshAnalysisCoverageTest::RunTest(const FString& Parameters)
{
	using namespace MeshPlacementAnalysisTestsPrivate;

	const FIntVector Size(3, 3, 4);

	auto IsCorner = [&Size](const int32 Cell)
	{
		const int32 X = Cell / Size.Y;
		const int32 Y = Cell % Size.Y;
		return (X == 0 || X == Size.X - 1) && (Y == 0 || Y == Size.Y - 1);
	};

	const FMeshAnalysisResult Result = WEMMeshAnalysis::Analyse(WEMSyntheticMeshes::MakeCutCornerTop(), FSurfacePolicy(), {}, FMeshAnalysisSettings());
	TestEqual(TEXT("The cut-corner table is 3 x 3 x 4"), Result.Geometry.SizeCells, Size);

	if (TestEqual(TEXT("The cut-corner table has one layer"), Result.Geometry.Surfaces.Num(), 1))
	{
		const FDerivedSurfaceLayer& Layer = Result.Geometry.Surfaces[0];
		bool bCornersOut = true;

		for (int32 Cell = 0; Cell < Layer.CellMask.Num(); ++Cell)
		{
			bCornersOut = bCornersOut && (Layer.CellMask[Cell] != 0) == !IsCorner(Cell);
		}

		TestTrue(TEXT("Its cut corners fall below coverage and are left out; every other cell is in"), bCornersOut);
	}

	// A PL_Surface socket pins the layer whatever the coverage, so the corners come back.
	FMeshAnalysisSource Pinned = WEMSyntheticMeshes::MakeCutCornerTop();
	Pinned.Sockets.Add({ FName(TEXT("PL_Surface_Top")), FVector(30.0, 30.0, 75.0) });

	const FMeshAnalysisResult PinnedResult = WEMMeshAnalysis::Analyse(Pinned, FSurfacePolicy(), {}, FMeshAnalysisSettings());

	if (TestEqual(TEXT("A pinned table still has one layer"), PinnedResult.Geometry.Surfaces.Num(), 1))
	{
		TestEqual(TEXT("A pinned layer takes every cell with a flat face at its height"), CountCells(PinnedResult.Geometry.Surfaces[0]), 9);
	}

	// PL_NoSurfaces turns detection off for the mesh.
	FMeshAnalysisSource Box;
	WEMSyntheticMeshes::AddBox(Box, FVector::ZeroVector, FVector(60.0));
	Box.Sockets.Add({ FName(TEXT("PL_NoSurfaces")), FVector::ZeroVector });

	const FMeshAnalysisResult NoSurfaces = WEMMeshAnalysis::Analyse(Box, FSurfacePolicy(), {}, FMeshAnalysisSettings());
	TestEqual(TEXT("PL_NoSurfaces leaves the box with no layers"), NoSurfaces.Geometry.Surfaces.Num(), 0);
	TestFalse(TEXT("PL_NoSurfaces is not warned about as a missing surface"), HasWarningContaining(NoSurfaces, TEXT("No surface")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAnalysisHashTest, "WEM.Placement.Analysis.Hash", MeshPlacementAnalysisTestsPrivate::TestFlags)

bool FMeshAnalysisHashTest::RunTest(const FString& Parameters)
{
	const FMeshAnalysisSource Desk = WEMSyntheticMeshes::MakeDesk();
	FMeshAnalysisSource Moved = Desk;
	Moved.Positions[0].Z += 1.0f;

	TestEqual(TEXT("The same mesh hashes the same"), WEMMeshAnalysis::HashSource(Desk), WEMMeshAnalysis::HashSource(WEMSyntheticMeshes::MakeDesk()));
	TestNotEqual(TEXT("A moved vertex changes the hash"), WEMMeshAnalysis::HashSource(Desk), WEMMeshAnalysis::HashSource(Moved));

	FSurfacePolicy Policy;
	const uint32 Before = WEMMeshAnalysis::HashSettings(Desk, Policy, {}, FMeshAnalysisSettings());
	Policy.MinHeightCm = 30.0f;

	TestNotEqual(TEXT("A changed surface policy changes the settings hash"), Before, WEMMeshAnalysis::HashSettings(Desk, Policy, {}, FMeshAnalysisSettings()));

	return true;
}

#endif
