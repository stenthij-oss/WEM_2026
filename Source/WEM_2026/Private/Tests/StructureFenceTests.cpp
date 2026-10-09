// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace StructureFenceTestsPrivate
{
	using namespace WEMRoomManagerTests;

	constexpr EStructureType AllTypes = EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall;
	constexpr EStructureType FloorTypes = EStructureType::Ground | EStructureType::Ceiling;

	UPlatformStructureData* MakeFence(const TCHAR* Name, const int32 Width, const int32 Length, const EStructureType Types, const bool bLoadBearing, TArray<UObject*>& OutRooted)
	{
		UPlatformStructureData* Fence = MakeStructure(Name, Width, Length, Types, /*bTwoSided=*/true);
		Fence->bFence = true;
		Fence->bLoadBearing = bLoadBearing;
		OutRooted.Add(Fence);
		return Fence;
	}

	FGridPlatform MakeFilled(const FIntVector& MinNode, const EGridAxis Normal, UPlatformTileData* Tile, UPlatformStructureData* Structure, const bool bTurned = false)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = Normal;
		Platform.Tile = Tile;
		Platform.bTurned = bTurned;
		Platform.PositiveStructure = Structure;
		Platform.NegativeStructure = Structure;
		return Platform;
	}

	/**
	 * A room manager with nothing to grow but fences: no tiles to draw, so a beat with no fence end
	 * waiting places nothing. Every end carries on, and Structures holds the fences so they are usable.
	 */
	ARoomManager* SpawnFenceManager(UWorld* World, TArray<UObject*>& OutRooted, TArray<TObjectPtr<UPlatformStructureData>> Fences)
	{
		ARoomManager* Manager = SpawnRoomManager(World, OutRooted);
		Manager->Structures.Append(Fences);
		Manager->Tiles.Reset();
		Manager->FenceContinueChance = 1.0f;
		return Manager;
	}

	/** Places nothing but fence panels until no end has anywhere left to go. Returns how many beats placed something. */
	int32 GrowFences(ARoomManager& Manager, const int32 MaxBeats = 100)
	{
		int32 Beats = 0;
		while (Beats < MaxBeats && Manager.PlaceNextPlatform())
		{
			++Beats;
		}

		return Beats;
	}

	bool HasPanel(const ARoomManager& Manager, const FIntVector& MinNode, const EGridAxis Normal, const UPlatformStructureData* Fence)
	{
		return Manager.Platforms.ContainsByPredicate([&](const FGridPlatform& Platform)
		{
			return Platform.MinNode == MinNode && Platform.Normal == Normal && Platform.PositiveStructure == Fence && Platform.NegativeStructure == Fence;
		});
	}

	/** A 2 x 2 roof of 16 x 16 platforms from X, Y 16 to 48, at height Z. */
	TArray<FGridPlatform> MakeRoof(UPlatformTileData* Square, UPlatformStructureData* Slab, const int32 Z)
	{
		return {
			MakeFilled(FIntVector(16, 16, Z), EGridAxis::Z, Square, Slab),
			MakeFilled(FIntVector(32, 16, Z), EGridAxis::Z, Square, Slab),
			MakeFilled(FIntVector(16, 32, Z), EGridAxis::Z, Square, Slab),
			MakeFilled(FIntVector(32, 32, Z), EGridAxis::Z, Square, Slab),
		};
	}

	/** The eight wall positions round that roof's edge, all with their foot at Z 32. */
	struct FPerimeterPanel
	{
		FIntVector MinNode;
		EGridAxis Normal;
	};

	const FPerimeterPanel Perimeter[] =
	{
		{ FIntVector(16, 16, 32), EGridAxis::Y }, { FIntVector(32, 16, 32), EGridAxis::Y },
		{ FIntVector(48, 16, 32), EGridAxis::X }, { FIntVector(48, 32, 32), EGridAxis::X },
		{ FIntVector(16, 48, 32), EGridAxis::Y }, { FIntVector(32, 48, 32), EGridAxis::Y },
		{ FIntVector(16, 16, 32), EGridAxis::X }, { FIntVector(16, 32, 32), EGridAxis::X },
	};

	/** Checks that a fence went once round a roof's edge and nowhere else. */
	void CheckRoundTheRoof(FAutomationTestBase& Test, const ARoomManager& Manager, const UPlatformStructureData* Fence, const TCHAR* Name)
	{
		for (const FPerimeterPanel& Panel : Perimeter)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: a panel stands at %s facing %d"), Name, *Panel.MinNode.ToString(), static_cast<int32>(Panel.Normal)),
				HasPanel(Manager, Panel.MinNode, Panel.Normal, Fence));
		}

		const int32 Panels = Manager.Platforms.FilterByPredicate([Fence](const FGridPlatform& Platform) { return Platform.PositiveStructure == Fence; }).Num();
		Test.TestEqual(FString::Printf(TEXT("%s: the fence is exactly the roof's edge, closed and nowhere else"), Name), Panels, 8);
		Test.TestEqual(FString::Printf(TEXT("%s: no end is left waiting"), Name), FRoomManagerTestAccess::GetWaitingFenceEnds(Manager), 0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFenceParapetTest, "WEM.Structures.Fence.Parapet", WEMRoomManagerTests::TestFlags)

bool FStructureFenceParapetTest::RunTest(const FString& Parameters)
{
	using namespace StructureFenceTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	UPlatformStructureData* Railing = MakeFence(TEXT("TestRailing"), 16, 16, EStructureType::Wall, true, Rooted);
	ARoomManager* Manager = SpawnFenceManager(TestWorld.World, Rooted, { Railing });
	UPlatformTileData* Square = Manager->FirstTile;
	UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), 16, 16, AllTypes, true);
	Rooted.Add(Slab);

	// A railing started on the south edge of a roof runs round it, both ways, turning at each corner
	// toward the side that has roof under it, and closes where its two ends meet.
	FRoomManagerTestAccess::Rebuild(*Manager, MakeRoof(Square, Slab, 32));
	FRoomManagerTestAccess::Place(*Manager, MakeFilled(FIntVector(16, 16, 32), EGridAxis::Y, Square, Railing));
	TestEqual(TEXT("Both ends of the first panel wait to grow"), FRoomManagerTestAccess::GetWaitingFenceEnds(*Manager), 2);

	GrowFences(*Manager);
	CheckRoundTheRoof(*this, *Manager, Railing, TEXT("Standing on a roof"));

	// Every panel is the same fence on the same tile, laid the same way round, and none of them is
	// up a level: a run keeps to the ground.
	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		if (Platform.PositiveStructure == Railing)
		{
			TestTrue(TEXT("Every panel is the first's tile laid the same way, at the same height"),
				Platform.Tile == Square && !Platform.bTurned && Platform.MinNode.Z == 32);
		}
	}

	// The same hanging from a ceiling's edge: the walls hang below it and follow it round.
	FRoomManagerTestAccess::Rebuild(*Manager, MakeRoof(Square, Slab, 48));
	FRoomManagerTestAccess::Place(*Manager, MakeFilled(FIntVector(16, 16, 32), EGridAxis::Y, Square, Railing));
	GrowFences(*Manager);
	CheckRoundTheRoof(*this, *Manager, Railing, TEXT("Hanging from a ceiling"));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFenceTurnTest, "WEM.Structures.Fence.TurnChance", WEMRoomManagerTests::TestFlags)

bool FStructureFenceTurnTest::RunTest(const FString& Parameters)
{
	using namespace StructureFenceTestsPrivate;

	// A railing on the south edge of floor A (X 16 to 32). East of its end at X 32 it could go
	// straight on along floor B's south edge, or turn south under ceiling G's west edge; turning
	// north would take it onto the seam between A and B, which no wall may stand on. West of its
	// other end only the turn north along A's west edge has anything under it.
	auto Run = [this](const float TurnChance, const bool bExpectTurn)
	{
		FScopedTestWorld TestWorld;
		TArray<UObject*> Rooted;

		UPlatformStructureData* Railing = MakeFence(TEXT("TestRailing"), 16, 16, EStructureType::Wall, true, Rooted);
		ARoomManager* Manager = SpawnFenceManager(TestWorld.World, Rooted, { Railing });
		Manager->FenceTurnChance = TurnChance;
		UPlatformTileData* Square = Manager->FirstTile;
		UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), 16, 16, AllTypes, true);
		Rooted.Add(Slab);

		FRoomManagerTestAccess::Rebuild(*Manager, {
			MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab),
			MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, Slab),
			MakeFilled(FIntVector(32, 0, 48), EGridAxis::Z, Square, Slab) });

		FRoomManagerTestAccess::Place(*Manager, MakeFilled(FIntVector(16, 16, 32), EGridAxis::Y, Square, Railing));

		// The west end goes first, and can only turn; then the east end decides.
		TestTrue(TEXT("Two fence beats"), Manager->PlaceNextPlatform() && Manager->PlaceNextPlatform());
		TestTrue(FString::Printf(TEXT("Turn chance %.0f: the west end turns north, where it cannot go straight"), TurnChance),
			HasPanel(*Manager, FIntVector(16, 16, 32), EGridAxis::X, Railing));

		const bool bStraight = HasPanel(*Manager, FIntVector(32, 16, 32), EGridAxis::Y, Railing);
		const bool bTurned = HasPanel(*Manager, FIntVector(32, 0, 32), EGridAxis::X, Railing);

		TestTrue(FString::Printf(TEXT("Turn chance %.0f: the east end %s"), TurnChance, bExpectTurn ? TEXT("turns south") : TEXT("goes straight on")),
			bExpectTurn ? (bTurned && !bStraight) : (bStraight && !bTurned));

		ReleaseRooted(Rooted);
	};

	Run(0.0f, false);
	Run(1.0f, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFenceFloorTest, "WEM.Structures.Fence.Floor", WEMRoomManagerTests::TestFlags)

bool FStructureFenceFloorTest::RunTest(const FString& Parameters)
{
	using namespace StructureFenceTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// A floor fence that is not load-bearing: it blocks the edges it brings, its run's own included.
	UPlatformStructureData* Boardwalk = MakeFence(TEXT("TestBoardwalk"), 16, 16, FloorTypes, false, Rooted);
	ARoomManager* Manager = SpawnFenceManager(TestWorld.World, Rooted, { Boardwalk });
	UPlatformTileData* Square = Manager->FirstTile;
	UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), 16, 16, AllTypes, true);
	Rooted.Add(Slab);

	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab) });
	FRoomManagerTestAccess::Place(*Manager, MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, Boardwalk));

	// Its far end waits; the end it grew from has no room.
	TestEqual(TEXT("Only the far end waits"), FRoomManagerTestAccess::GetWaitingFenceEnds(*Manager), 1);

	// Its blocked far edge refuses anything but the next panel of its run.
	FGridPlatform Plain = MakeFilled(FIntVector(48, 16, 32), EGridAxis::Z, Square, Slab);
	TestFalse(TEXT("A plain floor may not cross its blocked far edge"), Manager->CanPlacePlatform(Plain));
	TestFalse(TEXT("Nor may a wall stand on it"), Manager->CanPlacePlatform(MakeFilled(FIntVector(48, 16, 32), EGridAxis::X, Square, Slab)));
	TestTrue(TEXT("The run's next panel may"), Manager->CanPlacePlatform(MakeFilled(FIntVector(48, 16, 32), EGridAxis::Z, Square, Boardwalk)));

	GrowFences(*Manager);

	TestTrue(TEXT("The run carried on across its own blocked edge"), HasPanel(*Manager, FIntVector(48, 16, 32), EGridAxis::Z, Boardwalk));
	TestEqual(TEXT("And stopped at the cube's side, straight all the way"), Manager->GetPlatformCount(), 3);
	TestTrue(TEXT("The beam its two panels share is built"), Manager->IsCellSolid(FIntVector(48, 18, 32)));
	TestFalse(TEXT("Its other edges stay blocked"), Manager->CanPlacePlatform(MakeFilled(FIntVector(48, 32, 32), EGridAxis::Z, Square, Slab)));

	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		TestTrue(TEXT("A floor fence never turns: every panel lies in the same row"), Platform.MinNode.Y == 16 && Platform.MinNode.Z == 32 && Platform.Normal == EGridAxis::Z);
	}

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFenceEndsTest, "WEM.Structures.Fence.Ends", WEMRoomManagerTests::TestFlags)

bool FStructureFenceEndsTest::RunTest(const FString& Parameters)
{
	using namespace StructureFenceTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	UPlatformStructureData* Railing = MakeFence(TEXT("TestRailing"), 16, 16, EStructureType::Wall, true, Rooted);
	ARoomManager* Manager = SpawnFenceManager(TestWorld.World, Rooted, { Railing });
	UPlatformTileData* Square = Manager->FirstTile;
	UPlatformTileData* Small = MakeTile(8, 8);
	Rooted.Add(Small);
	UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), 16, 16, AllTypes, true);
	UPlatformStructureData* SmallSlab = MakeStructure(TEXT("TestSmallSlab"), 8, 8, AllTypes, true);
	Rooted.Append({ Slab, SmallSlab });

	const FGridPlatform Floor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab);
	const FGridPlatform First = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Y, Square, Railing);

	// A stopped end never continues.
	Manager->FenceContinueChance = 0.0f;
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor });
	FRoomManagerTestAccess::Place(*Manager, First);
	TestEqual(TEXT("With no chance to carry on, no end waits"), FRoomManagerTestAccess::GetWaitingFenceEnds(*Manager), 0);
	TestFalse(TEXT("And no beat places anything"), Manager->PlaceNextPlatform());
	TestEqual(TEXT("The panel stands alone"), Manager->GetPlatformCount(), 2);

	// A blocked end is dropped: both ends wait, then every spot either could go to is taken by small
	// walls of another tile, which carry no run on.
	Manager->FenceContinueChance = 1.0f;
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor });
	FRoomManagerTestAccess::Place(*Manager, First);
	TestEqual(TEXT("Both ends wait"), FRoomManagerTestAccess::GetWaitingFenceEnds(*Manager), 2);

	for (const FGridPlatform& Blocker : {
		MakeFilled(FIntVector(0, 16, 32), EGridAxis::Y, Small, SmallSlab),
		MakeFilled(FIntVector(16, 16, 32), EGridAxis::X, Small, SmallSlab),
		MakeFilled(FIntVector(16, 8, 32), EGridAxis::X, Small, SmallSlab),
		MakeFilled(FIntVector(32, 16, 32), EGridAxis::Y, Small, SmallSlab),
		MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Small, SmallSlab),
		MakeFilled(FIntVector(32, 8, 32), EGridAxis::X, Small, SmallSlab) })
	{
		FRoomManagerTestAccess::Place(*Manager, Blocker);
	}

	TestEqual(TEXT("Every blocker stands"), Manager->GetPlatformCount(), 8);
	TestFalse(TEXT("With nowhere to go, both ends are dropped and nothing is placed"), Manager->PlaceNextPlatform());
	TestEqual(TEXT("Nothing waits any more"), FRoomManagerTestAccess::GetWaitingFenceEnds(*Manager), 0);

	// The queue is beat state: a rebuild keeps it, and starting over empties it.
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor });
	FRoomManagerTestAccess::Place(*Manager, First);
	FRoomManagerTestAccess::Rebuild(*Manager, Manager->Platforms);
	TestEqual(TEXT("A rebuild keeps the waiting ends"), FRoomManagerTestAccess::GetWaitingFenceEnds(*Manager), 2);
	Manager->ClearPlatforms();
	TestEqual(TEXT("Starting over empties the queue"), FRoomManagerTestAccess::GetWaitingFenceEnds(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFenceLightWallTest, "WEM.Structures.Fence.LightWall", WEMRoomManagerTests::TestFlags)

bool FStructureFenceLightWallTest::RunTest(const FString& Parameters)
{
	using namespace StructureFenceTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// A parapet that is not load-bearing runs on round the roof along its side beams, while nothing
	// lands on its top.
	UPlatformStructureData* Parapet = MakeFence(TEXT("TestParapet"), 16, 16, EStructureType::Wall, false, Rooted);
	ARoomManager* Manager = SpawnFenceManager(TestWorld.World, Rooted, { Parapet });
	UPlatformTileData* Square = Manager->FirstTile;
	UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), 16, 16, AllTypes, true);
	Rooted.Add(Slab);

	FRoomManagerTestAccess::Rebuild(*Manager, MakeRoof(Square, Slab, 32));
	FRoomManagerTestAccess::Place(*Manager, MakeFilled(FIntVector(16, 16, 32), EGridAxis::Y, Square, Parapet));
	GrowFences(*Manager);
	CheckRoundTheRoof(*this, *Manager, Parapet, TEXT("A light parapet"));

	TestFalse(TEXT("No floor lands on its top"), Manager->CanPlacePlatform(MakeFilled(FIntVector(16, 16, 48), EGridAxis::Z, Square, Slab)));
	TestFalse(TEXT("No wall stands on its top"), Manager->CanPlacePlatform(MakeFilled(FIntVector(16, 16, 48), EGridAxis::Y, Square, Slab)));
	TestFalse(TEXT("Its top beam is not built"), Manager->IsCellSolid(FIntVector(18, 16, 48)));
	TestTrue(TEXT("The corner post its side beams run up to is"), Manager->IsCellSolid(FIntVector(16, 16, 48)));

	TestEqual(TEXT("The incremental state matches a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFenceGrowthTest, "WEM.Structures.Fence.Growth", WEMRoomManagerTests::TestFlags)

bool FStructureFenceGrowthTest::RunTest(const FString& Parameters)
{
	using namespace StructureFenceTestsPrivate;

	IConsoleVariable* VerifyEvery = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.RoomManager.VerifyEvery"));

	if (!TestNotNull(TEXT("The VerifyEvery console variable exists"), VerifyEvery))
	{
		return false;
	}

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Ordinary growth with fences among the structures - a railing, a light parapet lying on its side,
	// a light floor fence - and a one-sided fence, which growth leaves out with a warning.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformStructureData* Railing = MakeFence(TEXT("TestRailing"), 16, 16, EStructureType::Wall, true, Rooted);
	UPlatformStructureData* Parapet = MakeFence(TEXT("TestParapet"), 16, 8, EStructureType::Wall, false, Rooted);
	UPlatformStructureData* Boardwalk = MakeFence(TEXT("TestBoardwalk"), 16, 16, FloorTypes, false, Rooted);
	UPlatformStructureData* OneSided = MakeFence(TEXT("TestOneSidedFence"), 16, 16, EStructureType::Wall, true, Rooted);
	OneSided->bTwoSided = false;

	AddExpectedError(TEXT("is a fence but not two-sided"), EAutomationExpectedErrorFlags::Contains, 0);
	Manager->Structures.Append({ Railing, Parapet, Boardwalk, OneSided });
	Manager->bShowPieces = true;
	Manager->ClearPlatforms();

	const int32 Previous = VerifyEvery->GetInt();
	VerifyEvery->Set(1, ECVF_SetByCode);

	int32 Placed = 0;
	while (Placed < 250 && Manager->PlaceNextPlatform())
	{
		++Placed;
	}

	VerifyEvery->Set(Previous, ECVF_SetByCode);

	int32 FencePanels = 0;
	bool bOneSidedUsed = false;

	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		FencePanels += Platform.PositiveStructure && Platform.PositiveStructure->bFence ? 1 : 0;
		bOneSidedUsed = bOneSidedUsed || Platform.PositiveStructure == OneSided || Platform.NegativeStructure == OneSided;
	}

	TestTrue(TEXT("Fence panels were placed"), FencePanels > 10);
	TestFalse(TEXT("The one-sided fence never was"), bOneSidedUsed);
	TestEqual(TEXT("Every placement matched a full rebuild, fences and all"), FRoomManagerTestAccess::GetFailedVerifications(*Manager), 0);
	AddInfo(FString::Printf(TEXT("%d placement(s), %d of them fence panels."), Placed, FencePanels));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFenceRunLengthTest, "WEM.Structures.Fence.RunLength", WEMRoomManagerTests::SlowTestFlags)

bool FStructureFenceRunLengthTest::RunTest(const FString& Parameters)
{
	using namespace StructureFenceTestsPrivate;

	// A floor fence started alone in an open cube, over and over: each end carries on a geometric
	// number of panels, so a run should be about 1 + 2p / (1 - p) long - 9 at the default 0.8.
	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	UPlatformStructureData* Boardwalk = MakeFence(TEXT("TestBoardwalk"), 8, 8, FloorTypes, true, Rooted);
	ARoomManager* Manager = SpawnFenceManager(TestWorld.World, Rooted, { Boardwalk });
	Manager->GridSize = 512;
	Manager->FenceContinueChance = 0.8f;
	UPlatformTileData* Small = MakeTile(8, 8);
	Rooted.Add(Small);

	TMap<int32, int32> Lengths;
	double Total = 0.0;
	constexpr int32 Runs = 3000;

	for (int32 Run = 0; Run < Runs; ++Run)
	{
		FRoomManagerTestAccess::Rebuild(*Manager, {});
		FRoomManagerTestAccess::Place(*Manager, MakeFilled(FIntVector(252, 252, 252), EGridAxis::Z, Small, Boardwalk));
		GrowFences(*Manager, 1000);

		const int32 Length = Manager->GetPlatformCount();
		++Lengths.FindOrAdd(Length);
		Total += Length;
	}

	const double Mean = Total / Runs;
	TestTrue(FString::Printf(TEXT("The mean run is %.2f panels against 9 expected"), Mean), FMath::Abs(Mean - 9.0) < 0.6);

	Lengths.KeySort(TLess<int32>());
	AddInfo(FString::Printf(TEXT("Mean run %.2f panels over %d runs (1 + 2p / (1 - p) = 9). Lengths: %s"), Mean, Runs,
		*FString::JoinBy(Lengths, TEXT(", "), [](const TPair<int32, int32>& Entry) { return FString::Printf(TEXT("%d x%d"), Entry.Key, Entry.Value); })));

	ReleaseRooted(Rooted);
	return true;
}

#endif
