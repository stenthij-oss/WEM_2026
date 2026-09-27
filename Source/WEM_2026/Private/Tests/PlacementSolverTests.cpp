// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMath.h"
#include "Misc/AutomationTest.h"
#include "PlacementSolver.h"
#include "PlacementTestHelpers.h"
#include "PlacementTestWorld.h"

namespace PlacementSolverTestsPrivate
{
	using namespace WEMPlacementTests;

	constexpr int32 SegmentSupport = 1 << static_cast<int32>(ESupportKind::Segment);
	constexpr int32 ObjectSurfaceSupport = 1 << static_cast<int32>(ESupportKind::ObjectSurface);

	FPlacementAttempt MakeAttempt(const int32 Signature, const FPlacementRule* Rule = nullptr, const int32 SupportMask = SegmentSupport)
	{
		FPlacementAttempt Attempt;
		Attempt.Signature = Signature;
		Attempt.Rule = Rule;
		Attempt.SupportMask = SupportMask;
		return Attempt;
	}

	/** A pose on a segment plane, turned so that Side looks along PlaneDirection, with local (0, 0) on Origin. */
	FPlacementPose MakePose(FPlacementSolver& Solver, const EGridFace Face, const FIntVector& SurfaceCell, const EObjectSide Side, const FIntPoint& PlaneDirection, const FIntPoint& Origin)
	{
		FPlacementPose Pose;
		Pose.Plane = Solver.GetSegmentPlaneId(SurfaceCell, Face);
		Pose.Rotation = WEMPlacement::FindRotationFacing(WEMPlacement::MakeSegmentPlaneAxes(Face), Side, PlaneDirection);
		Pose.Origin = Origin;
		return Pose;
	}

	/** The index in the catalog of the signature an archetype's first row went to. */
	int32 FindSignature(const FPlacementCatalogData& Catalog, const UPlacementArchetype* Archetype)
	{
		return Catalog.Signatures.IndexOfByPredicate([Archetype](const FPlacementSignature& Signature) { return Signature.Archetype == Archetype; });
	}

	int32 FindEntry(const FPlacementCatalogData& Catalog, const UPlacementArchetype* Archetype)
	{
		return Catalog.Entries.IndexOfByPredicate([Archetype](const FPlacementEntry& Entry) { return Entry.Archetype == Archetype; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementSelectionShareTest, "WEM.Placement.Selection.ArchetypeShare", WEMPlacementTests::TestFlags)

bool FPlacementSelectionShareTest::RunTest(const FString& Parameters)
{
	using namespace PlacementSolverTestsPrivate;

	UPlacementArchetype* Chair = MakeArchetype(TEXT("DA_Chair"));
	UPlacementArchetype* Lamp = MakeArchetype(TEXT("DA_Lamp"));

	FPlacementTestWorld World;
	FPlacementCatalogData Catalog;
	FRandomStream Stream(7);

	// Forty chairs of a few shapes against one lamp.
	for (int32 Index = 0; Index < 40; ++Index)
	{
		Catalog.AddRow(FName(*FString::Printf(TEXT("SM_Chair_%d"), Index)), MakeRow(Chair, FIntVector(1 + Index % 3, 1 + (Index / 3) % 3, 2)));
	}

	Catalog.AddRow(TEXT("SM_Lamp"), MakeRow(Lamp, FIntVector(1, 1, 3)));

	FPlacementSolver Solver(World, Catalog, Stream);

	TArray<float> Weights;
	Solver.GetArchetypeDrawWeights(Weights);

	if (TestEqual(TEXT("Two archetypes are drawn from"), Weights.Num(), 2))
	{
		const float Total = Weights[0] + Weights[1];
		TestEqual(TEXT("Forty chairs get half the picks"), Weights[0] / Total, 0.5f);
		TestEqual(TEXT("One lamp gets the other half"), Weights[1] / Total, 0.5f);
	}

	float ChairSignatureWeight = 0.0f;
	for (const int32 Signature : Catalog.Kinds[0].Signatures)
	{
		ChairSignatureWeight += Solver.GetSignatureDrawWeight(Signature);
	}

	TestEqual(TEXT("Within its share, each chair mesh weighs its own weight"), ChairSignatureWeight, 40.0f);

	Release({ Chair, Lamp });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementSelectionSignatureTest, "WEM.Placement.Selection.Signatures", WEMPlacementTests::TestFlags)

bool FPlacementSelectionSignatureTest::RunTest(const FString& Parameters)
{
	using namespace PlacementSolverTestsPrivate;

	UPlacementArchetype* Desk = MakeArchetype(TEXT("DA_Desk"));

	FPlacementTestWorld World;
	FPlacementCatalogData Catalog;
	FRandomStream Stream(7);

	FPlacementCatalogRow Oak = MakeRow(Desk, FIntVector(3, 6, 4));
	FPlacementCatalogRow Pine = MakeRow(Desk, FIntVector(3, 6, 4));
	Pine.Geometry.LocalBoundsCm = FBox(FVector::ZeroVector, FVector(57.0, 118.0, 74.0));

	Catalog.AddRow(TEXT("SM_Desk_Oak"), Oak);
	Catalog.AddRow(TEXT("SM_Desk_Pine"), Pine);

	TestEqual(TEXT("Two desks of one archetype and shape share one signature"), Catalog.Signatures.Num(), 1);

	// With nothing to stand on, the signature fails once, and that one failure stands for both meshes.
	FPlacementSolver Solver(World, Catalog, Stream);
	TArray<int32> Placed;

	TestFalse(TEXT("Nothing is placed with nowhere to stand"), Solver.PlaceNext(4, Placed));
	TestEqual(TEXT("The shared signature is marked infeasible at the current revision"), Catalog.Signatures[0].InfeasibleAtRevision, Solver.GetRevision());

	TArray<float> Weights;
	Solver.GetArchetypeDrawWeights(Weights);
	TestEqual(TEXT("An archetype with no signature left drops out of the draw"), Weights[0], 0.0f);

	// Once the structure changes, it is worth trying again.
	World.AddSegment(2, FIntVector(0, 0, 10), 10, 10);
	Solver.GetArchetypeDrawWeights(Weights);
	TestEqual(TEXT("A new revision brings it back"), Weights[0], 1.0f);
	TestTrue(TEXT("And it can be placed now"), Solver.PlaceNext(4, Placed));

	// A catalog built at another cell size is refused outright.
	UDataTable* Table = NewObject<UDataTable>(GetTransientPackage());
	Table->RowStruct = FPlacementCatalogRow::StaticStruct();
	FPlacementCatalogRow Wrong = MakeRow(Desk, FIntVector(3, 6, 4), 10.0);
	Table->AddRow(TEXT("SM_Desk_Small"), Wrong);

	FPlacementCatalogData Refused;
	FString Error;
	TArray<FString> Warnings;
	TestFalse(TEXT("A row built at another cell size refuses the catalog"), Refused.AddTables({ Table }, 20.0, Error, Warnings));
	TestTrue(TEXT("The refusal names the cell size"), Error.Contains(TEXT("cell size")));
	TestEqual(TEXT("Nothing is taken from a refused catalog"), Refused.Entries.Num(), 0);

	Release({ Desk });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementWallContactTest, "WEM.Placement.Contacts.BackToWall", WEMPlacementTests::TestFlags)

bool FPlacementWallContactTest::RunTest(const FString& Parameters)
{
	using namespace PlacementSolverTestsPrivate;

	// A floor, and a wall three cells long standing on it at x = 5.
	FPlacementTestWorld World;
	World.AddSegment(2, FIntVector(0, 0, 10), 20, 20);
	World.AddSegment(0, FIntVector(5, 0, 11), 3, 4);

	UPlacementArchetype* Bench = MakeArchetype(TEXT("DA_Bench"));
	Bench->Rules.Add(MakeEdgeRule(TEXT("Back to wall"), { MakeContact(EObjectSide::Back, EContactTarget::Wall) }));

	FPlacementCatalogData Catalog;
	Catalog.AddRow(TEXT("SM_Bench"), MakeRow(Bench, FIntVector(1, 4, 2)));

	FRandomStream Stream(3);
	FPlacementSolver Solver(World, Catalog, Stream);
	Solver.RefreshSegmentPlanes();

	// Four wide with its back to the wall, one face past the wall's end.
	const FPlacementPose Pose = MakePose(Solver, EGridFace::PosZ, FIntVector(6, 0, 10), EObjectSide::Back, FIntPoint(-1, 0), FIntPoint(6, 0));
	const FPlacementAttempt Attempt = MakeAttempt(0, &Bench->Rules[0]);
	FContactRequirement& Contact = Bench->Rules[0].Contacts[0];

	FPoseCheck Check;
	TestFalse(TEXT("Flush fails with one face of the back uncovered"), Solver.TestPose(Attempt, Pose, Check));

	Contact.Overlap = EOverlapMode::AtLeast;
	Contact.MinFaces = 3;
	TestTrue(TEXT("At least 3 passes with exactly 3 faces on the wall"), Solver.TestPose(Attempt, Pose, Check));
	TestTrue(TEXT("The contact's side is recorded"), (Check.ContactSideMask & WEMPlacement::SideBit(EObjectSide::Back)) != 0);
	TestEqual(TEXT("The three touching faces are recorded"), Check.ContactFaces.Num(), 3);

	Contact.MinFaces = 4;
	TestFalse(TEXT("At least 4 fails with 3"), Solver.TestPose(Attempt, Pose, Check));

	// Placed, it slides its short mesh back against the wall.
	Contact.MinFaces = 3;
	const int32 Id = Solver.TryPlaceAt(Attempt, Pose, 0);

	if (TestTrue(TEXT("The bench is placed"), Id != INDEX_NONE))
	{
		const FPlacedObject& Object = *Solver.FindObject(Id);
		const FBox Bounds = Catalog.Entries[0].Geometry.LocalBoundsCm.TransformBy(Object.Transform);
		TestEqual(TEXT("Its back stands on the wall's face"), Bounds.Min.X, 6.0 * World.CellSize, 0.01);
		TestEqual(TEXT("Its bottom stands on the floor"), Bounds.Min.Z, 11.0 * World.CellSize, 0.01);
		TestEqual(TEXT("Its volume starts one layer off the floor"), Object.VolumeMin.Z, 11);
		TestEqual(TEXT("Its volume is two cells tall"), Object.VolumeMax.Z, 12);
	}

	Release({ Bench });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementVerticalSupportTest, "WEM.Placement.Contacts.VerticalSupport", WEMPlacementTests::TestFlags)

bool FPlacementVerticalSupportTest::RunTest(const FString& Parameters)
{
	using namespace PlacementSolverTestsPrivate;

	// A segment standing at x = 10, and one meeting it at a right angle at z = 5.
	FPlacementTestWorld World;
	World.AddSegment(0, FIntVector(10, 0, 0), 20, 20);
	World.AddSegment(2, FIntVector(11, 0, 5), 9, 20);

	UPlacementArchetype* Bench = MakeArchetype(TEXT("DA_Bench"));
	Bench->Rules.Add(MakeEdgeRule(TEXT("Back to wall"), { MakeContact(EObjectSide::Back, EContactTarget::Wall) }));

	FPlacementCatalogData Catalog;
	Catalog.AddRow(TEXT("SM_Bench"), MakeRow(Bench, FIntVector(1, 3, 2)));

	FRandomStream Stream(3);
	FPlacementSolver Solver(World, Catalog, Stream);
	Solver.RefreshSegmentPlanes();

	// Standing on the vertical segment's +X face, its up is +X, and the segment at z = 5 is a wall
	// to it. Plane cells there are (y, z).
	const FPlacementPose Pose = MakePose(Solver, EGridFace::PosX, FIntVector(10, 10, 6), EObjectSide::Back, FIntPoint(0, -1), FIntPoint(10, 6));
	const FPlacementAttempt Attempt = MakeAttempt(0, &Bench->Rules[0]);

	FPoseCheck Check;
	TestTrue(TEXT("Back to wall holds on a vertical segment, against the segment meeting it"), Solver.TestPose(Attempt, Pose, Check));

	const int32 Id = Solver.TryPlaceAt(Attempt, Pose, 0);

	if (TestTrue(TEXT("The bench is placed on the wall"), Id != INDEX_NONE))
	{
		const FPlacedObject& Object = *Solver.FindObject(Id);
		TestEqual(TEXT("Its up is the vertical segment's normal"), Object.Frame.Up, FIntVector(1, 0, 0));
		TestEqual(TEXT("Its volume starts one cell off the segment"), Object.VolumeMin.X, 11);

		const FBox Bounds = Catalog.Entries[0].Geometry.LocalBoundsCm.TransformBy(Object.Transform);
		TestEqual(TEXT("Its back lies against the segment it treats as a wall"), Bounds.Min.Z, 6.0 * World.CellSize, 0.01);
	}

	Release({ Bench });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementCornerTest, "WEM.Placement.Contacts.Corner", WEMPlacementTests::TestFlags)

bool FPlacementCornerTest::RunTest(const FString& Parameters)
{
	using namespace PlacementSolverTestsPrivate;

	// A floor, crossed by a wall at x = 5 and another at y = 5. The walls offer no surfaces of their
	// own here, so every corner found is on the floor; on segments, their faces would be floors too.
	FPlacementTestWorld World;
	World.AddSegment(2, FIntVector(0, 0, 10), 12, 12);
	World.AddSolidBlock(FIntVector(5, 0, 11), FIntVector(5, 11, 13));
	World.AddSolidBlock(FIntVector(0, 5, 11), FIntVector(11, 5, 13));

	UPlacementArchetype* Cabinet = MakeArchetype(TEXT("DA_CornerCabinet"));
	Cabinet->Rules.Add(MakeEdgeRule(TEXT("Corner"), { MakeContact(EObjectSide::Back, EContactTarget::Wall), MakeContact(EObjectSide::Left, EContactTarget::Wall) }));

	FPlacementCatalogData Catalog;
	Catalog.AddRow(TEXT("SM_CornerCabinet"), MakeRow(Cabinet, FIntVector(2, 2, 1)));

	for (const int32 Seed : { 1, 2, 3, 4, 5 })
	{
		FRandomStream Stream(Seed);
		FPlacementSolver Solver(World, Catalog, Stream);
		Solver.RefreshSegmentPlanes();

		FPlacementPose Pose;
		FPoseCheck Check;
		const FPlacementAttempt Attempt = MakeAttempt(0, &Cabinet->Rules[0]);

		if (!TestTrue(TEXT("A corner is found"), Solver.FindPose(Attempt, Pose, Check)))
		{
			continue;
		}

		TestTrue(TEXT("Its back and left both hold"),
			(Check.ContactSideMask & WEMPlacement::SideBit(EObjectSide::Back)) && (Check.ContactSideMask & WEMPlacement::SideBit(EObjectSide::Left)));

		const int32 Id = Solver.TryPlaceAt(Attempt, Pose, 0);
		const FPlacedObject& Object = *Solver.FindObject(Id);

		int32 NearestX = MAX_int32;
		int32 NearestY = MAX_int32;
		for (const FIntPoint& Cell : Object.Footprint)
		{
			NearestX = FMath::Min(NearestX, FMath::Abs(Cell.X - 5));
			NearestY = FMath::Min(NearestY, FMath::Abs(Cell.Y - 5));
		}

		TestTrue(TEXT("It sits in the corner the two walls make"), NearestX == 1 && NearestY == 1);
	}

	Release({ Cabinet });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementClearanceTest, "WEM.Placement.Clearance", WEMPlacementTests::TestFlags)

bool FPlacementClearanceTest::RunTest(const FString& Parameters)
{
	using namespace PlacementSolverTestsPrivate;

	// Parallel segments eight cells apart: seven cells of air between them.
	FPlacementTestWorld World;
	World.AddSegment(2, FIntVector(0, 0, 10), 10, 10);
	World.AddSegment(2, FIntVector(0, 0, 18), 10, 10);

	UPlacementArchetype* Column = MakeArchetype(TEXT("DA_Column"));

	FPlacementCatalogData Catalog;
	Catalog.AddRow(TEXT("SM_Column_7"), MakeRow(Column, FIntVector(1, 1, 7)));
	Catalog.AddRow(TEXT("SM_Column_8"), MakeRow(Column, FIntVector(1, 1, 8)));

	FRandomStream Stream(3);
	FPlacementSolver Solver(World, Catalog, Stream);
	Solver.RefreshSegmentPlanes();

	const FPlacementPose Pose = MakePose(Solver, EGridFace::PosZ, FIntVector(3, 3, 10), EObjectSide::Back, FIntPoint(-1, 0), FIntPoint(3, 3));

	FPoseCheck Check;
	TestTrue(TEXT("A 7-cell object fits in 7 cells of air"), Solver.TestPose(MakeAttempt(Catalog.Entries[0].Signature), Pose, Check));
	TestFalse(TEXT("An 8-cell object does not"), Solver.TestPose(MakeAttempt(Catalog.Entries[1].Signature), Pose, Check));

	// Hung from the ceiling, it is the same seven cells, the other way up.
	const FPlacementPose Hanging = MakePose(Solver, EGridFace::NegZ, FIntVector(3, 3, 18), EObjectSide::Back, FIntPoint(-1, 0), FIntPoint(3, 3));
	TestTrue(TEXT("A 7-cell object fits under the ceiling too"), Solver.TestPose(MakeAttempt(Catalog.Entries[0].Signature), Hanging, Check));

	// Placed on the floor, it fills the gap, and nothing hangs from the ceiling above it.
	Solver.TryPlaceAt(MakeAttempt(Catalog.Entries[0].Signature), Pose, 0);
	TestFalse(TEXT("Objects cannot overlap across the gap"), Solver.TestPose(MakeAttempt(Catalog.Entries[0].Signature), Hanging, Check));

	Release({ Column });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementSolverDeterminismTest, "WEM.Placement.Solver.Determinism", WEMPlacementTests::TestFlags)

bool FPlacementSolverDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace PlacementSolverTestsPrivate;

	FPlacementTestWorld World;
	World.AddSegment(2, FIntVector(0, 0, 10), 16, 16);
	World.AddSegment(0, FIntVector(8, 0, 0), 16, 16);

	UPlacementArchetype* Box = MakeArchetype(TEXT("DA_Box"));
	UPlacementArchetype* Bench = MakeArchetype(TEXT("DA_Bench"));
	Bench->Rules.Add(MakeEdgeRule(TEXT("Back to wall"), { MakeContact(EObjectSide::Back, EContactTarget::Wall) }));

	auto Run = [&](const int32 Seed)
	{
		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Box"), MakeRow(Box, FIntVector(2, 2, 2)));
		Catalog.AddRow(TEXT("SM_Bench"), MakeRow(Bench, FIntVector(1, 3, 2)));

		FRandomStream Stream(Seed);
		FPlacementSolver Solver(World, Catalog, Stream);

		TArray<FString> Log;
		TArray<int32> Placed;

		for (int32 Beat = 0; Beat < 12 && Solver.PlaceNext(4, Placed); ++Beat)
		{
			Log.Add(Solver.FindObject(Placed[0])->Description);
		}

		// No two objects share a cell.
		TSet<FIntVector> Filled;
		bool bOverlap = false;

		for (const FPlacedObject& Object : Solver.GetObjects())
		{
			for (int32 X = Object.VolumeMin.X; X <= Object.VolumeMax.X; ++X)
			{
				for (int32 Y = Object.VolumeMin.Y; Y <= Object.VolumeMax.Y; ++Y)
				{
					for (int32 Z = Object.VolumeMin.Z; Z <= Object.VolumeMax.Z; ++Z)
					{
						bool bAlready = false;
						Filled.Add(FIntVector(X, Y, Z), &bAlready);
						bOverlap = bOverlap || bAlready;
					}
				}
			}
		}

		TestFalse(TEXT("No two placed objects fill the same cell"), bOverlap);
		return Log;
	};

	const TArray<FString> First = Run(42);
	const TArray<FString> Second = Run(42);

	TestTrue(TEXT("Objects are placed"), First.Num() > 0);
	TestTrue(TEXT("The same seed places the same objects in the same places"), First == Second);

	if (!First.IsEmpty())
	{
		AddInfo(First[0]);
	}

	Release({ Box, Bench });
	return true;
}

#endif
