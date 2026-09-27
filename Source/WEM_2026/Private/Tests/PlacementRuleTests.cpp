// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PlacementSolver.h"
#include "PlacementTestHelpers.h"
#include "PlacementTestWorld.h"

namespace PlacementRuleTestsPrivate
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

	/** A pose on the floor at z = 10 with Side looking along PlaneDirection. */
	FPlacementPose MakeFloorPose(FPlacementSolver& Solver, const EObjectSide Side, const FIntPoint& PlaneDirection, const FIntPoint& Origin)
	{
		FPlacementPose Pose;
		Pose.Plane = Solver.GetSegmentPlaneId(FIntVector(Origin.X, Origin.Y, 10), EGridFace::PosZ);
		Pose.Rotation = WEMPlacement::FindRotationFacing(WEMPlacement::MakeSegmentPlaneAxes(EGridFace::PosZ), Side, PlaneDirection);
		Pose.Origin = Origin;
		return Pose;
	}

	FPlacementTestWorld MakeFloorWorld(const int32 Size = 30)
	{
		FPlacementTestWorld World;
		World.AddSegment(2, FIntVector(0, 0, 10), Size, Size);
		return World;
	}

	FDerivedSurfaceLayer MakeLayer(const FIntVector& Size, const float HeightCm, const float ClearanceCm)
	{
		FDerivedSurfaceLayer Layer;
		Layer.HeightCm = HeightCm;
		Layer.ClearanceCm = ClearanceCm;
		Layer.CellMask.Init(1, WEMPlacement::GetFootprintCellCount(Size));
		return Layer;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementChairAtDeskTest, "WEM.Placement.Contacts.ChairAtDesk", WEMPlacementTests::TestFlags)

bool FPlacementChairAtDeskTest::RunTest(const FString& Parameters)
{
	using namespace PlacementRuleTestsPrivate;

	UPlacementArchetype* Desk = MakeArchetype(TEXT("DA_Desk"));
	UPlacementArchetype* Chair = MakeArchetype(TEXT("DA_OfficeChair"));

	FContactRequirement AtDesk = MakeContact(EObjectSide::Front, EContactTarget::Object);
	AtDesk.TargetArchetypes = { Desk };
	AtDesk.TargetEdge = ETargetEdge::Special;
	AtDesk.TargetSpecialEdgeId = 1;
	AtDesk.bExcludeTargetCorners = true;
	Chair->Rules.Add(MakeEdgeRule(TEXT("At the desk"), { AtDesk }));

	FPlacementCatalogRow DeskRow = MakeRow(Desk, FIntVector(3, 6, 4));
	DeskRow.Geometry.SpecialEdges.Add(MakeSpecialEdge(1, EObjectSide::Front, 1, 4));

	FPlacementTestWorld World = MakeFloorWorld();
	FPlacementCatalogData Catalog;
	Catalog.AddRow(TEXT("SM_Desk"), DeskRow);
	Catalog.AddRow(TEXT("SM_Chair"), MakeRow(Chair, FIntVector(2, 2, 2)));

	FRandomStream Stream(5);
	FPlacementSolver Solver(World, Catalog, Stream);

	// The desk faces +X from x = 5..7, y = 5..10; its front faces are indexed by y - 5, and its
	// knee-hole runs faces 1 to 4.
	const int32 DeskId = Solver.TryPlaceAt(MakeAttempt(0), MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(5, 5)), 0);
	TestTrue(TEXT("The desk is placed"), DeskId != INDEX_NONE);

	// The chair faces the desk from x = 8..9. Its local (0, 0) is its back left, which for a chair
	// facing -X is at the far x and the high y.
	const FPlacementAttempt Attempt = MakeAttempt(1, &Chair->Rules[0]);
	FContactRequirement& Contact = Chair->Rules[0].Contacts[0];
	FPoseCheck Check;

	auto ChairAt = [&](const int32 HighY)
	{
		return Solver.TestPose(Attempt, MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(-1, 0), FIntPoint(9, HighY)), Check);
	};

	TestTrue(TEXT("Across the middle of the knee-hole, the chair is allowed"), ChairAt(7));
	TestTrue(TEXT("Across the far end of the knee-hole, the chair is allowed"), ChairAt(9));
	TestFalse(TEXT("Overlapping the desk's corner face, it is rejected"), ChairAt(6));
	TestFalse(TEXT("Overlapping the desk's other corner face, it is rejected"), ChairAt(10));

	// Against the desk's plain front, it is the corner exclusion alone that keeps it off the corner.
	Contact.TargetEdge = ETargetEdge::Front;
	TestTrue(TEXT("Against the front, away from the corners, it is allowed"), ChairAt(7));
	TestFalse(TEXT("Against the front, touching a corner face, it is rejected"), ChairAt(6));

	Contact.bExcludeTargetCorners = false;
	TestTrue(TEXT("With corners allowed, touching a corner face is fine"), ChairAt(6));

	// Searched for, a chair only ever pulls up to the knee-hole.
	Contact.TargetEdge = ETargetEdge::Special;
	Contact.bExcludeTargetCorners = true;

	for (const int32 Seed : { 1, 2, 3, 4, 5, 6 })
	{
		FRandomStream SearchStream(Seed);
		FPlacementSolver Search(World, Catalog, SearchStream);
		Search.TryPlaceAt(MakeAttempt(0), MakeFloorPose(Search, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(5, 5)), 0);

		FPlacementPose Found;
		if (TestTrue(TEXT("A place at the desk is found"), Search.FindPose(Attempt, Found, Check)))
		{
			const int32 ChairId = Search.TryPlaceAt(Attempt, Found, 1);
			bool bInKneeHole = true;

			for (const FIntPoint& Cell : Search.FindObject(ChairId)->Footprint)
			{
				bInKneeHole = bInKneeHole && Cell.X >= 8 && Cell.X <= 9 && Cell.Y >= 6 && Cell.Y <= 9;
			}

			TestTrue(TEXT("The chair found stands at the knee-hole"), bInKneeHole);
		}
	}

	Release({ Desk, Chair });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementZonesTest, "WEM.Placement.Zones", WEMPlacementTests::TestFlags)

bool FPlacementZonesTest::RunTest(const FString& Parameters)
{
	using namespace PlacementRuleTestsPrivate;

	UPlacementArchetype* Couch = MakeArchetype(TEXT("DA_Couch"));
	UPlacementArchetype* Dining = MakeArchetype(TEXT("DA_DiningTable"));
	UPlacementArchetype* Coffee = MakeArchetype(TEXT("DA_CoffeeTable"));

	FExclusionZone& Zone = Couch->ExclusionZones.AddDefaulted_GetRef();
	Zone.SideMask = WEMPlacement::SideBit(EObjectSide::Front);
	Zone.DepthCells = 4;
	Zone.Mode = EZoneMode::BlockListed;
	Zone.Archetypes = { Dining };

	const FPlacementTestWorld World = MakeFloorWorld();

	auto MakeCatalog = [&]()
	{
		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Couch"), MakeRow(Couch, FIntVector(2, 4, 2)));
		Catalog.AddRow(TEXT("SM_DiningTable"), MakeRow(Dining, FIntVector(2, 2, 2)));
		Catalog.AddRow(TEXT("SM_CoffeeTable"), MakeRow(Coffee, FIntVector(2, 2, 2)));
		return Catalog;
	};

	// The couch faces +X from x = 5..6, y = 5..8, so its zone runs x = 7..10 across the same y.
	// A table at x = 8..9, y = 6..7 stands in it.
	auto CouchPose = [](FPlacementSolver& Solver) { return MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(5, 5)); };
	auto TablePose = [](FPlacementSolver& Solver) { return MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(8, 6)); };

	FPoseCheck Check;

	// The couch first, then a table in front of it.
	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(1);
		FPlacementSolver Solver(World, Catalog, Stream);

		Solver.TryPlaceAt(MakeAttempt(0), CouchPose(Solver), 0);
		TestFalse(TEXT("A dining table may not stand in front of a couch placed before it"), Solver.TestPose(MakeAttempt(1), TablePose(Solver), Check));
		TestTrue(TEXT("A coffee table may"), Solver.TestPose(MakeAttempt(2), TablePose(Solver), Check));
	}

	// The table first, then the couch facing it.
	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(1);
		FPlacementSolver Solver(World, Catalog, Stream);

		Solver.TryPlaceAt(MakeAttempt(1), TablePose(Solver), 1);
		TestFalse(TEXT("A couch may not face a dining table placed before it"), Solver.TestPose(MakeAttempt(0), CouchPose(Solver), Check));
	}

	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(1);
		FPlacementSolver Solver(World, Catalog, Stream);

		Solver.TryPlaceAt(MakeAttempt(2), TablePose(Solver), 2);
		TestTrue(TEXT("A couch may face a coffee table placed before it"), Solver.TestPose(MakeAttempt(0), CouchPose(Solver), Check));
	}

	// Allow-only with nothing listed keeps the zone clear of everything.
	Zone.Mode = EZoneMode::AllowOnlyListed;
	Zone.Archetypes.Reset();

	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(1);
		FPlacementSolver Solver(World, Catalog, Stream);

		Solver.TryPlaceAt(MakeAttempt(0), CouchPose(Solver), 0);
		TestFalse(TEXT("A keep-clear zone refuses even a coffee table"), Solver.TestPose(MakeAttempt(2), TablePose(Solver), Check));
	}

	// Corners fill in between two chosen sides.
	Zone.SideMask = WEMPlacement::SideBit(EObjectSide::Front) | WEMPlacement::SideBit(EObjectSide::Right);
	Zone.DepthCells = 2;

	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(1);
		FPlacementSolver Solver(World, Catalog, Stream);

		const int32 CouchId = Solver.TryPlaceAt(MakeAttempt(0), CouchPose(Solver), 0);
		const FPlacedObject* Placed = Solver.FindObject(CouchId);

		// Front band 2 x 4, right band 2 x 2, and the 2 x 2 corner square between them.
		TestEqual(TEXT("Front, right and the corner between them make 16 zone cells"), Placed ? Placed->ZoneCells.Num() : 0, 16);
		TestTrue(TEXT("The corner square is part of the zone"),
			Placed && Placed->ZoneCells.ContainsByPredicate([](const TPair<FIntPoint, int32>& Cell) { return Cell.Key == FIntPoint(8, 10); }));
	}

	Release({ Couch, Dining, Coffee });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementRuleSelectionTest, "WEM.Placement.Rules.Selection", WEMPlacementTests::TestFlags)

bool FPlacementRuleSelectionTest::RunTest(const FString& Parameters)
{
	using namespace PlacementRuleTestsPrivate;

	const FPlacementTestWorld World = MakeFloorWorld(10);

	FPlacementRule Anywhere;
	Anywhere.Label = TEXT("Anywhere");
	Anywhere.Kind = ERuleKind::Surface;

	// There are no walls on this floor, so a rule asking for one never places.
	const FPlacementRule AgainstWall = MakeEdgeRule(TEXT("Against a wall"), { MakeContact(EObjectSide::Back, EContactTarget::Wall) });

	auto PlaceOne = [&](UPlacementArchetype* Archetype, int32& OutRule)
	{
		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Thing"), MakeRow(Archetype, FIntVector(1, 1, 1)));

		FRandomStream Stream(11);
		FPlacementSolver Solver(World, Catalog, Stream);
		TArray<int32> Placed;

		const bool bPlaced = Solver.PlaceNext(1, Placed);
		OutRule = bPlaced ? Solver.FindObject(Placed[0])->Rule : -2;
		return bPlaced;
	};

	int32 Rule = INDEX_NONE;

	UPlacementArchetype* ByPriority = MakeArchetype(TEXT("DA_ByPriority"));
	ByPriority->Rules = { AgainstWall, Anywhere };
	TestTrue(TEXT("Priority falls through a rule that finds nothing to the next"), PlaceOne(ByPriority, Rule) && Rule == 1);

	UPlacementArchetype* ByWeight = MakeArchetype(TEXT("DA_ByWeight"));
	ByWeight->RuleSelection = ERuleSelection::Weighted;
	ByWeight->Rules = { AgainstWall, Anywhere };
	ByWeight->Rules[0].Weight = 1000.0f;
	ByWeight->Rules[1].Weight = 0.001f;
	TestTrue(TEXT("A weighted rule that finds nothing drops out, however heavy, and the roll goes again"), PlaceOne(ByWeight, Rule) && Rule == 1);

	UPlacementArchetype* WithFallback = MakeArchetype(TEXT("DA_WithFallback"));
	WithFallback->Rules = { AgainstWall };
	WithFallback->FallbackChance = 1.0f;
	TestTrue(TEXT("When every rule fails, the fallback places it on any surface"), PlaceOne(WithFallback, Rule) && Rule == INDEX_NONE);

	UPlacementArchetype* WithoutFallback = MakeArchetype(TEXT("DA_WithoutFallback"));
	WithoutFallback->Rules = { AgainstWall };
	TestFalse(TEXT("Without a fallback, it is not placed"), PlaceOne(WithoutFallback, Rule));

	// Instance limits, on the archetype and on a row.
	UPlacementArchetype* Limited = MakeArchetype(TEXT("DA_Limited"));
	Limited->MaxInstances = 2;

	{
		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Limited"), MakeRow(Limited, FIntVector(1, 1, 1)));

		FRandomStream Stream(11);
		FPlacementSolver Solver(World, Catalog, Stream);
		TArray<int32> Placed;

		TestTrue(TEXT("The first of two is placed"), Solver.PlaceNext(1, Placed));
		TestTrue(TEXT("The second of two is placed"), Solver.PlaceNext(1, Placed));
		TestFalse(TEXT("An archetype at its limit is placed no more"), Solver.PlaceNext(1, Placed));
	}

	UPlacementArchetype* Unlimited = MakeArchetype(TEXT("DA_Unlimited"));

	{
		FPlacementCatalogRow Rare = MakeRow(Unlimited, FIntVector(1, 1, 1));
		Rare.MaxInstances = 1;

		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Rare"), Rare);

		FRandomStream Stream(11);
		FPlacementSolver Solver(World, Catalog, Stream);
		TArray<int32> Placed;

		TestTrue(TEXT("The one allowed of a row is placed"), Solver.PlaceNext(1, Placed));
		TestFalse(TEXT("A row at its limit is placed no more"), Solver.PlaceNext(1, Placed));
	}

	Release({ ByPriority, ByWeight, WithFallback, WithoutFallback, Limited, Unlimited });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementStackingTest, "WEM.Placement.Stacking", WEMPlacementTests::TestFlags)

bool FPlacementStackingTest::RunTest(const FString& Parameters)
{
	using namespace PlacementRuleTestsPrivate;

	UPlacementArchetype* Desk = MakeArchetype(TEXT("DA_Desk"));
	UPlacementArchetype* Shelf = MakeArchetype(TEXT("DA_Shelf"));
	UPlacementArchetype* Monitor = MakeArchetype(TEXT("DA_Monitor"));
	UPlacementArchetype* Book = MakeArchetype(TEXT("DA_Book"));

	// A monitor only ever stands on something, with its back to the edge of whatever it stands on.
	Monitor->AllowedSupports = ObjectSurfaceSupport;
	Monitor->Rules.Add(MakeEdgeRule(TEXT("Back of the desk"), { MakeContact(EObjectSide::Back, EContactTarget::SupportBoundary) }));
	Book->AllowedSupports = ObjectSurfaceSupport;

	const FIntVector DeskSize(3, 6, 4);
	FPlacementCatalogRow DeskRow = MakeRow(Desk, DeskSize);
	DeskRow.Geometry.Surfaces.Add(MakeLayer(DeskSize, 75.0f, 0.0f));

	const FIntVector ShelfSize(2, 4, 6);
	FPlacementCatalogRow ShelfRow = MakeRow(Shelf, ShelfSize);
	ShelfRow.Geometry.Surfaces.Add(MakeLayer(ShelfSize, 60.0f, 57.0f));

	const FPlacementTestWorld World = MakeFloorWorld();

	auto MakeCatalog = [&]()
	{
		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Desk"), DeskRow);
		Catalog.AddRow(TEXT("SM_Shelf"), ShelfRow);
		Catalog.AddRow(TEXT("SM_Monitor"), MakeRow(Monitor, FIntVector(1, 2, 2)));
		Catalog.AddRow(TEXT("SM_Book_Short"), MakeRow(Book, FIntVector(1, 1, 2)));
		Catalog.AddRow(TEXT("SM_Book_Tall"), MakeRow(Book, FIntVector(1, 1, 3)));
		return Catalog;
	};

	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(3);
		FPlacementSolver Solver(World, Catalog, Stream);

		const int32 DeskId = Solver.TryPlaceAt(MakeAttempt(0), MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(5, 5)), 0);
		const FPlacedObject* PlacedDesk = Solver.FindObject(DeskId);

		if (!TestTrue(TEXT("The desk has a surface to stand things on"), PlacedDesk && PlacedDesk->HostedPlanes.Num() == 1))
		{
			Release({ Desk, Shelf, Monitor, Book });
			return false;
		}

		const FPlacementAttempt OnDesk = MakeAttempt(2, &Monitor->Rules[0], ObjectSurfaceSupport);
		FPlacementPose Pose;
		FPoseCheck Check;

		if (TestTrue(TEXT("A monitor finds a place on the desk"), Solver.FindPose(OnDesk, Pose, Check)))
		{
			TestEqual(TEXT("It stands on the desk's surface"), Pose.Plane, PlacedDesk->HostedPlanes[0]);

			const int32 MonitorId = Solver.TryPlaceAt(OnDesk, Pose, 2);
			const FPlacedObject* PlacedMonitor = Solver.FindObject(MonitorId);

			TestEqual(TEXT("Its host is the desk"), PlacedMonitor ? PlacedMonitor->Host : INDEX_NONE, DeskId);

			if (PlacedMonitor)
			{
				const FBox DeskBounds = Catalog.Entries[0].Geometry.LocalBoundsCm.TransformBy(PlacedDesk->Transform);
				const FBox MonitorBounds = Catalog.Entries[2].Geometry.LocalBoundsCm.TransformBy(PlacedMonitor->Transform);
				TestEqual(TEXT("Its bottom is on the desk top, 75 cm up"), MonitorBounds.Min.Z, DeskBounds.Min.Z + 75.0, 0.01);

				// Back to the support's edge: its back faces look off the desk's surface.
				TestTrue(TEXT("Its back holds against the edge of the desk's surface"),
					(PlacedMonitor->Contacts.ContactSideMask & WEMPlacement::SideBit(EObjectSide::Back)) != 0);
			}
		}

		// A book is short enough for the desk, and has nowhere to stand on the floor at all.
		FPlacementPose BookPose;
		TestTrue(TEXT("A book finds a place on the desk"), Solver.FindPose(MakeAttempt(3, nullptr, ObjectSurfaceSupport), BookPose, Check));
		TestFalse(TEXT("A stacked-only archetype never stands on a segment"), Solver.FindPose(MakeAttempt(3, nullptr, SegmentSupport & Book->AllowedSupports), BookPose, Check));
	}

	// Clearance on a shelf: 57 cm under the plank above takes a 40 cm book, not a 60 cm one.
	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(3);
		FPlacementSolver Solver(World, Catalog, Stream);

		Solver.TryPlaceAt(MakeAttempt(1), MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(15, 15)), 1);

		FPlacementPose Pose;
		FPoseCheck Check;
		TestTrue(TEXT("A 2-cell book fits under the shelf's 57 cm"), Solver.FindPose(MakeAttempt(3, nullptr, ObjectSurfaceSupport), Pose, Check));
		TestFalse(TEXT("A 3-cell book does not"), Solver.FindPose(MakeAttempt(4, nullptr, ObjectSurfaceSupport), Pose, Check));
	}

	// A surface that accepts nothing takes nothing.
	Desk->Surfaces.Accept = EAcceptMode::None;

	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(3);
		FPlacementSolver Solver(World, Catalog, Stream);

		Solver.TryPlaceAt(MakeAttempt(0), MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(5, 5)), 0);

		FPlacementPose Pose;
		FPoseCheck Check;
		TestFalse(TEXT("Nothing stands on a desk whose surfaces accept nothing"), Solver.FindPose(MakeAttempt(3, nullptr, ObjectSurfaceSupport), Pose, Check));
	}

	// Or only what it lists.
	Desk->Surfaces.Accept = EAcceptMode::OnlyListed;
	Desk->Surfaces.AcceptedArchetypes = { Monitor };

	{
		FPlacementCatalogData Catalog = MakeCatalog();
		FRandomStream Stream(3);
		FPlacementSolver Solver(World, Catalog, Stream);

		Solver.TryPlaceAt(MakeAttempt(0), MakeFloorPose(Solver, EObjectSide::Front, FIntPoint(1, 0), FIntPoint(5, 5)), 0);

		FPlacementPose Pose;
		FPoseCheck Check;
		TestFalse(TEXT("A book is not among what the desk accepts"), Solver.FindPose(MakeAttempt(3, nullptr, ObjectSurfaceSupport), Pose, Check));
		TestTrue(TEXT("A monitor is"), Solver.FindPose(MakeAttempt(2, &Monitor->Rules[0], ObjectSurfaceSupport), Pose, Check));
	}

	Release({ Desk, Shelf, Monitor, Book });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementCompanionsTest, "WEM.Placement.Companions", WEMPlacementTests::TestFlags)

bool FPlacementCompanionsTest::RunTest(const FString& Parameters)
{
	using namespace PlacementRuleTestsPrivate;

	UPlacementArchetype* Desk = MakeArchetype(TEXT("DA_Desk"));
	UPlacementArchetype* Chair = MakeArchetype(TEXT("DA_OfficeChair"));
	UPlacementArchetype* Monitor = MakeArchetype(TEXT("DA_Monitor"));

	// The chair pulls up to a desk's knee-hole; the monitor stands at the back of a desk's top.
	// Neither is ever drawn on its own: they only come with a desk.
	FContactRequirement AtDesk = MakeContact(EObjectSide::Front, EContactTarget::Object);
	AtDesk.TargetArchetypes = { Desk };
	AtDesk.TargetEdge = ETargetEdge::Special;
	AtDesk.TargetSpecialEdgeId = 1;
	AtDesk.bExcludeTargetCorners = true;
	Chair->Rules.Add(MakeEdgeRule(TEXT("At the desk"), { AtDesk }));
	Chair->SpawnWeight = 0.0f;

	Monitor->AllowedSupports = ObjectSurfaceSupport;
	Monitor->Rules.Add(MakeEdgeRule(TEXT("Back of the desk"), { MakeContact(EObjectSide::Back, EContactTarget::SupportBoundary) }));
	Monitor->SpawnWeight = 0.0f;

	// The desk stands with its back to the floor's edge, so there is always room in front of it.
	Desk->Rules.Add(MakeEdgeRule(TEXT("Back to the edge"), { MakeContact(EObjectSide::Back, EContactTarget::SupportBoundary) }));

	FCompanion& WithChair = Desk->Companions.AddDefaulted_GetRef();
	WithChair.Archetype = Chair;

	FCompanion& WithMonitor = Desk->Companions.AddDefaulted_GetRef();
	WithMonitor.Archetype = Monitor;

	const FIntVector DeskSize(3, 6, 4);
	FPlacementCatalogRow DeskRow = MakeRow(Desk, DeskSize);
	DeskRow.SetName = TEXT("Oak");
	DeskRow.Geometry.SpecialEdges.Add(MakeSpecialEdge(1, EObjectSide::Front, 1, 4));
	DeskRow.Geometry.Surfaces.Add(MakeLayer(DeskSize, 75.0f, 0.0f));

	FPlacementCatalogRow OakChair = MakeRow(Chair, FIntVector(2, 2, 2));
	OakChair.SetName = TEXT("Oak");
	FPlacementCatalogRow PineChair = MakeRow(Chair, FIntVector(2, 2, 2));
	PineChair.SetName = TEXT("Pine");
	PineChair.Weight = 100.0f;

	const FPlacementTestWorld World = MakeFloorWorld();

	for (const int32 Seed : { 1, 2, 3 })
	{
		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Desk_Oak"), DeskRow);
		Catalog.AddRow(TEXT("SM_Chair_Oak"), OakChair);
		Catalog.AddRow(TEXT("SM_Chair_Pine"), PineChair);
		Catalog.AddRow(TEXT("SM_Monitor"), MakeRow(Monitor, FIntVector(1, 2, 2)));

		FRandomStream Stream(Seed);
		FPlacementSolver Solver(World, Catalog, Stream);
		TArray<int32> Placed;

		if (!TestTrue(TEXT("A desk is placed"), Solver.PlaceNext(4, Placed)))
		{
			continue;
		}

		TestEqual(TEXT("The desk comes with its chair and its monitor"), Placed.Num(), 3);

		const FPlacedObject* PlacedDesk = Solver.FindObject(Placed[0]);
		TestTrue(TEXT("The desk comes first"), PlacedDesk && PlacedDesk->Archetype == Desk);

		for (int32 Index = 1; Index < Placed.Num(); ++Index)
		{
			const FPlacedObject* Companion = Solver.FindObject(Placed[Index]);

			if (!Companion)
			{
				continue;
			}

			TestEqual(TEXT("A companion knows its host"), Companion->CompanionOf, Placed[0]);

			if (Companion->Archetype == Chair)
			{
				// However heavy the pine chair, the oak desk takes the oak one.
				TestEqual(TEXT("The chair comes from the desk's set"), Catalog.Entries[Companion->Entry].SetName, FName(TEXT("Oak")));
				TestTrue(TEXT("The chair pulls up to that desk"), (Companion->Contacts.ContactSideMask & WEMPlacement::SideBit(EObjectSide::Front)) != 0);
			}
			else if (Companion->Archetype == Monitor)
			{
				TestEqual(TEXT("The monitor stands on that desk"), Companion->Host, Placed[0]);
			}
		}
	}

	// With no chair of the desk's set, any chair will do.
	DeskRow.SetName = TEXT("Walnut");

	{
		FPlacementCatalogData Catalog;
		Catalog.AddRow(TEXT("SM_Desk_Walnut"), DeskRow);
		Catalog.AddRow(TEXT("SM_Chair_Oak"), OakChair);
		Catalog.AddRow(TEXT("SM_Chair_Pine"), PineChair);

		FRandomStream Stream(1);
		FPlacementSolver Solver(World, Catalog, Stream);
		TArray<int32> Placed;

		TestTrue(TEXT("A desk of another set is placed"), Solver.PlaceNext(4, Placed));
		TestEqual(TEXT("It still comes with a chair"), Placed.Num(), 2);
	}

	Release({ Desk, Chair, Monitor });
	return true;
}

#endif
