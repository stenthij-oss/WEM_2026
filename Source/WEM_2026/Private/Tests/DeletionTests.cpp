// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Count.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace DeletionTestsPrivate
{
	using namespace WEMRoomManagerTests;

	/** Grows Beats beats. */
	void Grow(ARoomManager& Manager, const int32 Beats)
	{
		for (int32 Beat = 0; Beat < Beats; ++Beat)
		{
			Manager.PlaceNextPlatform();
		}
	}

	/** Takes Count platforms standing away, drawn from Stream, Batch at a time. */
	void RemoveAtRandom(ARoomManager& Manager, FRandomStream& Stream, const int32 Count, const int32 Batch)
	{
		for (int32 Removed = 0; Removed < Count;)
		{
			TArray<FGridPlatform> Built = FRoomManagerTestAccess::GetBuilt(Manager);
			TArray<FGridPlatform> Chosen;

			for (int32 Pick = 0; Pick < Batch && !Built.IsEmpty(); ++Pick)
			{
				Chosen.Add(Built[Stream.RandRange(0, Built.Num() - 1)]);
				Built.RemoveAt(Built.IndexOfByPredicate([&Chosen](const FGridPlatform& Platform) { return Platform.MinNode == Chosen.Last().MinNode && Platform.Normal == Chosen.Last().Normal; }));
			}

			if (Chosen.IsEmpty())
			{
				return;
			}

			FRoomManagerTestAccess::Remove(Manager, Chosen);
			Removed += Chosen.Num();
		}
	}

	/** How many platforms standing cross air held back. */
	int32 CountVulnerable(const ARoomManager& Manager)
	{
		return Algo::CountIf(FRoomManagerTestAccess::GetBuilt(Manager), [&Manager](const FGridPlatform& Platform)
		{
			return FRoomManagerTestAccess::IsVulnerable(Manager, Platform);
		});
	}

	/** Whether a platform still stands. */
	bool IsStanding(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return FRoomManagerTestAccess::GetBuilt(Manager).ContainsByPredicate([&Platform](const FGridPlatform& Built)
		{
			return Built.MinNode == Platform.MinNode && Built.Normal == Platform.Normal;
		});
	}

	/** The structure as a plain graph: every platform standing, the ones sharing a beam with each, and the ground tiles. */
	struct FLinks
	{
		TArray<FGridPlatform> Built;
		TArray<TArray<int32>> Linked;
		TArray<int32> Ground;
	};

	FLinks GetLinks(const ARoomManager& Manager)
	{
		FLinks Links;
		Links.Built = FRoomManagerTestAccess::GetBuilt(Manager);

		for (int32 Index = 0; Index < Links.Built.Num(); ++Index)
		{
			Links.Linked.Add(FRoomManagerTestAccess::GetLinked(Manager, Index));

			if (FRoomManagerTestAccess::IsGroundTile(Manager, Links.Built[Index]))
			{
				Links.Ground.Add(Index);
			}
		}

		return Links;
	}

	/** What would be left with no link to Ground once Gone were taken away, worked out the plain way: all not reached from them. */
	TSet<int32> GetUnreached(const FLinks& Links, const TArray<int32>& Ground, const TSet<int32>& Gone)
	{
		TArray<bool> bReached;
		bReached.Init(false, Links.Built.Num());
		TArray<int32> Frontier;

		for (const int32 Index : Ground)
		{
			if (!Gone.Contains(Index) && !bReached[Index])
			{
				bReached[Index] = true;
				Frontier.Add(Index);
			}
		}

		for (int32 Next = 0; Next < Frontier.Num(); ++Next)
		{
			for (const int32 Index : Links.Linked[Frontier[Next]])
			{
				if (!Gone.Contains(Index) && !bReached[Index])
				{
					bReached[Index] = true;
					Frontier.Add(Index);
				}
			}
		}

		TSet<int32> Unreached;

		for (int32 Index = 0; Index < Links.Built.Num(); ++Index)
		{
			if (!bReached[Index] && !Gone.Contains(Index))
			{
				Unreached.Add(Index);
			}
		}

		return Unreached;
	}

	/** The platform, not a ground tile, whose going would cut the most loose, and what it would cut. */
	int32 FindWeakest(const FLinks& Links, TSet<int32>& OutCut)
	{
		int32 Weakest = INDEX_NONE;
		OutCut.Reset();

		for (int32 Index = 0; Index < Links.Built.Num(); ++Index)
		{
			if (Links.Ground.Contains(Index))
			{
				continue;
			}

			TSet<int32> Cut = GetUnreached(Links, Links.Ground, { Index });

			if (Weakest == INDEX_NONE || Cut.Num() > OutCut.Num())
			{
				Weakest = Index;
				OutCut = MoveTemp(Cut);
			}
		}

		return Weakest;
	}

	/** How many of these still stand. */
	int32 CountStanding(const ARoomManager& Manager, const FLinks& Links, const TSet<int32>& Indices)
	{
		return Algo::CountIf(Indices, [&](const int32 Index) { return IsStanding(Manager, Links.Built[Index]); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionRemoveTest, "WEM.Deletion.Remove", WEMRoomManagerTests::TestFlags)

bool FDeletionRemoveTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// The slabs, and a second set that is not load-bearing, so blocked beams come and go with the platforms.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;

	TArray<TObjectPtr<UPlatformStructureData>> Structures = Manager->Structures;

	for (const UPlatformTileData* Tile : TArray<TObjectPtr<UPlatformTileData>>(Manager->Tiles))
	{
		UPlatformStructureData* Light = MakeStructure(TEXT("TestLightSlab"), Tile->Width, Tile->Length,
			EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall, /*bTwoSided=*/true, /*bMayLieSideways=*/true);
		Light->bLoadBearing = false;
		Structures.Add(Light);
		Rooted.Add(Light);
	}

	Manager->Structures = Structures;
	Manager->ClearPlatforms();

	Grow(*Manager, 400);
	const int32 Grown = Manager->GetPlatformCount();
	AddInfo(FString::Printf(TEXT("%d platforms grown."), Grown));
	TestTrue(TEXT("Growth goes on"), Grown > 250);

	// Taken away a few at a time, the state kept piece by piece has to come out as a full rebuild makes it - the
	// candidate lists among it, so the lists have to be in hand first.
	FRoomManagerTestAccess::EnsureCandidates(*Manager);
	FRandomStream Stream(77);

	for (int32 Round = 0; Round < 6; ++Round)
	{
		RemoveAtRandom(*Manager, Stream, 20, Round % 2 == 0 ? 1 : 5);
		TestFalse(TEXT("A removal keeps the lists"), FRoomManagerTestAccess::IsStale(*Manager));
		TestEqual(TEXT("And everything agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);
	}

	TestEqual(TEXT("They are gone"), Manager->GetPlatformCount(), Grown - 120);
	TestEqual(TEXT("And gone from the list"), Manager->Platforms.Num(), Grown - 120);

	// Growth carries on into the room that came free, and still agrees.
	Grow(*Manager, 200);
	TestTrue(TEXT("Growth fills in"), Manager->GetPlatformCount() > Grown - 120);
	TestEqual(TEXT("Growing again agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// The rebuild path, kept as a fallback, lands in the same place.
	IConsoleVariable* ByRebuild = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.RoomManager.RemoveByRebuild"));
	ByRebuild->Set(1);
	RemoveAtRandom(*Manager, Stream, 10, 5);
	ByRebuild->Set(0);
	TestEqual(TEXT("Removed by rebuild, it agrees too"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// Taking every platform away leaves the empty cube, and growth starts over from the middle.
	FRoomManagerTestAccess::Remove(*Manager, TArray<FGridPlatform>(FRoomManagerTestAccess::GetBuilt(*Manager)));
	TestEqual(TEXT("All gone"), Manager->GetPlatformCount(), 0);
	TestTrue(TEXT("Growth starts again"), Manager->PlaceNextPlatform() && Manager->GetPlatformCount() == 1);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionVisualsTest, "WEM.Deletion.Visuals", WEMRoomManagerTests::TestFlags)

bool FDeletionVisualsTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// With the pieces and the overlay drawn: what removal takes out of the chunks has to match what a full sync would draw.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->bShowPieces = true;
	Manager->bShowSurfaces = true;
	Manager->ClearPlatforms();

	Grow(*Manager, 250);
	FRandomStream Stream(5);
	RemoveAtRandom(*Manager, Stream, 40, 4);

	TestEqual(TEXT("Pieces and squares agree with a full sync"), FRoomManagerTestAccess::Verify(*Manager), 0);

	Grow(*Manager, 100);
	TestEqual(TEXT("And after growing again"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionStrikeTest, "WEM.Deletion.Strike", WEMRoomManagerTests::TestFlags)

bool FDeletionStrikeTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->ClearPlatforms();
	Grow(*Manager, 300);

	// Every platform is laid with health between the least and the most.
	bool bHealthInRange = true;

	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		const float Health = FRoomManagerTestAccess::GetHealth(*Manager, Platform);
		bHealthInRange = bHealthInRange && Health >= Manager->MinHealth && Health <= Manager->MaxHealth;
	}

	TestTrue(TEXT("Health between 80 and 120"), bHealthInRange);

	// About a tenth hit by a strike, for 10 to 20 each: nothing goes in one.
	FRoomManagerTestAccess::Strike(*Manager);
	AddInfo(Manager->DeletionSummary);
	TestEqual(TEXT("Nothing dies of one strike"), FRoomManagerTestAccess::GetDyingCount(*Manager), 0);

	// Every platform at 100, every one hit for 40, and paths drawn through what stands, making those platforms vulnerable:
	// two strikes leave the rest at 20, and the vulnerable at -28 - gone.
	Manager->MinHealth = 100.0f;
	Manager->MaxHealth = 100.0f;
	Manager->ClearPlatforms();
	Grow(*Manager, 300);

	Manager->PathCount = 30;
	Manager->PathWidth = 4;
	Manager->RerollPaths();

	Manager->StrikeFraction = 1.0f;
	Manager->MinDamage = 40.0f;
	Manager->MaxDamage = 40.0f;

	const int32 Before = Manager->GetPlatformCount();
	const int32 VulnerableBefore = CountVulnerable(*Manager);
	AddInfo(FString::Printf(TEXT("%d of %d platforms cross a path."), VulnerableBefore, Before));
	TestTrue(TEXT("Some cross a path"), VulnerableBefore > 0 && VulnerableBefore < Before);

	FRoomManagerTestAccess::Strike(*Manager);
	TestEqual(TEXT("One strike leaves them all"), FRoomManagerTestAccess::GetDyingCount(*Manager), 0);

	FRoomManagerTestAccess::Strike(*Manager);
	AddInfo(Manager->DeletionSummary);
	TestEqual(TEXT("Two strikes take the vulnerable ones, and only them"), FRoomManagerTestAccess::GetDyingCount(*Manager), VulnerableBefore);

	// Taken away a few at a time, as the clock takes them, and the rest stand.
	FRoomManagerTestAccess::EnsureCandidates(*Manager);

	while (FRoomManagerTestAccess::GetDyingCount(*Manager) > 0)
	{
		FRoomManagerTestAccess::RemoveDying(*Manager, 4);
	}

	TestEqual(TEXT("They are taken away"), Manager->GetPlatformCount(), Before - VulnerableBefore);
	TestEqual(TEXT("None crosses a path now"), CountVulnerable(*Manager), 0);
	TestEqual(TEXT("And the rest agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// A third strike takes the rest.
	Manager->StrikeSegments();
	TestEqual(TEXT("A third strike takes the rest"), Manager->GetPlatformCount(), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionSeedsTest, "WEM.Deletion.Seeds", WEMRoomManagerTests::TestFlags)

bool FDeletionSeedsTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->bSeedEveryZone = true;
	Manager->ClearPlatforms();

	// The zones tile the cube, three by three by three, on lattice lines.
	int32 Covered = 0;

	for (int32 Zone = 0; Zone < 27; ++Zone)
	{
		const TPair<FIntVector, FIntVector> Box = FRoomManagerTestAccess::GetZoneBox(*Manager, Zone);
		TestTrue(TEXT("Zones lie on lattice lines"), Box.Key.X % 4 == 0 && Box.Key.Y % 4 == 0 && Box.Key.Z % 4 == 0
			&& Box.Value.X % 4 == 0 && Box.Value.Y % 4 == 0 && Box.Value.Z % 4 == 0);
		Covered += (Box.Value.X - Box.Key.X) * (Box.Value.Y - Box.Key.Y) * (Box.Value.Z - Box.Key.Z);
	}

	const TPair<FIntVector, FIntVector> Top = FRoomManagerTestAccess::GetZoneBox(*Manager, 26);
	TestEqual(TEXT("And cover it between them"), Covered, (Manager->GridSize - 1) * (Manager->GridSize - 1) * (Manager->GridSize - 1));
	TestTrue(TEXT("The last at the cube's far corner, up top"), Top.Value == FIntVector(Manager->GridSize - 1));
	TestEqual(TEXT("26 zones wait for a first tile"), FRoomManagerTestAccess::GetWaitingSeeds(*Manager), 26);

	// The middle one first, at the heart of the cube, then one in every other zone, a beat each.
	Grow(*Manager, 27);
	const TArray<FGridPlatform>& Built = FRoomManagerTestAccess::GetBuilt(*Manager);
	TestEqual(TEXT("27 first tiles"), Built.Num(), 27);

	TSet<int32> Zones;
	bool bInStep = true;
	bool bApart = true;

	for (const FGridPlatform& Seed : Built)
	{
		Zones.Add(FRoomManagerTestAccess::GetZoneOf(*Manager, Seed));

		// In step with the middle one, so their growth lines up where it meets: whole tiles apart across, and whole sides up.
		const FIntVector Step = Seed.MinNode - Built[0].MinNode;
		bInStep = bInStep && Seed.Normal == EGridAxis::Z && Step.X % 16 == 0 && Step.Y % 16 == 0 && Step.Z % 16 == 0;
	}

	for (int32 Index = 1; Index < Built.Num(); ++Index)
	{
		const FIntVector Step = Built[Index].MinNode - Built[0].MinNode;
		bApart = bApart && (FMath::Abs(Step.X) > 16 || FMath::Abs(Step.Y) > 16 || Step.Z != 0);
	}

	TestEqual(TEXT("The first in the middle zone"), FRoomManagerTestAccess::GetZoneOf(*Manager, Built[0]), 13);
	TestEqual(TEXT("One in every zone"), Zones.Num(), 27);
	TestTrue(TEXT("All flat, in step with the middle one"), bInStep);
	TestTrue(TEXT("None touching the middle one"), bApart);

	// Growth from all of them agrees with a full rebuild, which keeps every first tile.
	Grow(*Manager, 400);
	AddInfo(FString::Printf(TEXT("%d platforms; per zone %s."), Manager->GetPlatformCount(), *Manager->ZoneSummary));
	TestEqual(TEXT("Growth from 27 first tiles agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	FRoomManagerTestAccess::Rebuild(*Manager, Manager->Platforms);
	TestEqual(TEXT("The rebuild keeps them all"), FRoomManagerTestAccess::GetBuilt(*Manager).Num(), Manager->Platforms.Num());

	// Three a zone: laid a round at a time, every zone its first before any its second, none touching another.
	Manager->SeedsPerZone = 3;
	Manager->ClearPlatforms();
	TestEqual(TEXT("80 more wait: three a zone, the middle one's first aside"), FRoomManagerTestAccess::GetWaitingSeeds(*Manager), 80);

	Grow(*Manager, 27);
	TSet<int32> FirstRound;

	for (const FGridPlatform& Seed : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		FirstRound.Add(FRoomManagerTestAccess::GetZoneOf(*Manager, Seed));
	}

	TestEqual(TEXT("The first round reaches every zone"), FirstRound.Num(), 27);

	Grow(*Manager, 54);
	int32 PerZone[27] = {};

	for (const FGridPlatform& Seed : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		++PerZone[FRoomManagerTestAccess::GetZoneOf(*Manager, Seed)];
	}

	int32 Fewest = MAX_int32;
	int32 Most = 0;

	for (const int32 Count : PerZone)
	{
		Fewest = FMath::Min(Fewest, Count);
		Most = FMath::Max(Most, Count);
	}

	AddInfo(FString::Printf(TEXT("Three a zone: %d first tiles, %d to %d a zone."), Manager->GetPlatformCount(), Fewest, Most));
	TestEqual(TEXT("Three in every zone"), Fewest == 3 && Most == 3, true);
	// Flat, so two touch only lying in one layer with their rims meeting.
	const TArray<FGridPlatform>& Seeds = FRoomManagerTestAccess::GetBuilt(*Manager);
	int32 Touching = 0;

	for (int32 First = 0; First < Seeds.Num(); ++First)
	{
		for (int32 Second = First + 1; Second < Seeds.Num(); ++Second)
		{
			const FIntVector Step = Seeds[Second].MinNode - Seeds[First].MinNode;
			Touching += Step.Z == 0 && FMath::Abs(Step.X) <= 16 && FMath::Abs(Step.Y) <= 16 ? 1 : 0;
		}
	}

	TestEqual(TEXT("None touches another: each starts growth of its own"), Touching, 0);
	TestEqual(TEXT("Growth from them agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// Off, the middle one alone.
	Manager->SeedsPerZone = 1;
	Manager->bSeedEveryZone = false;
	Manager->ClearPlatforms();
	Grow(*Manager, 27);
	TestEqual(TEXT("Off, growth starts from the middle alone"), FRoomManagerTestAccess::GetWaitingSeeds(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionReshuffleTest, "WEM.Deletion.Reshuffle", WEMRoomManagerTests::TestFlags)

bool FDeletionReshuffleTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->bSeedEveryZone = true;
	Manager->ClearPlatforms();
	Grow(*Manager, 500);

	// An object on every other floor, standing on its face.
	TArray<FGridPlatform> Holding;

	for (int32 Index = 0; Index < FRoomManagerTestAccess::GetBuilt(*Manager).Num(); Index += 2)
	{
		const FGridPlatform Platform = FRoomManagerTestAccess::GetBuilt(*Manager)[Index];

		if (Platform.Normal == EGridAxis::Z)
		{
			Manager->AddObstacle(Platform.MinNode + FIntVector(2, 2, 1), Platform.MinNode + FIntVector(3, 3, 2));
			Holding.Add(Platform);
		}
	}

	FRoomManagerTestAccess::EnsureCandidates(*Manager);

	for (int32 Round = 0; Round < 4; ++Round)
	{
		const int32 Before = Manager->GetPlatformCount();

		FRoomManagerTestAccess::BeginReshuffle(*Manager);

		int32 Waiting = 0;
		const int32 Zone = FRoomManagerTestAccess::GetReshuffleZone(*Manager, Waiting);
		TestTrue(TEXT("A zone is drawn, and between 4 and 12 to move"), Zone >= 0 && Zone < 27 && Waiting >= 0 && Waiting <= 12);

		const TArray<FGridPlatform> StoodBefore = FRoomManagerTestAccess::GetBuilt(*Manager);

		while (FRoomManagerTestAccess::StepReshuffle(*Manager))
		{
		}

		const TArray<FGridPlatform>& After = FRoomManagerTestAccess::GetBuilt(*Manager);
		AddInfo(FString::Printf(TEXT("Zone %d: %d to move, %d -> %d platforms. %s"), Zone, Waiting, Before, After.Num(), *Manager->DeletionSummary));

		// What it laid lies in its zone: every platform new to the list is in it.
		const int32 NewOutside = Algo::CountIf(After, [&](const FGridPlatform& Platform)
		{
			const bool bStoodBefore = StoodBefore.ContainsByPredicate([&Platform](const FGridPlatform& Old)
			{
				return Old.MinNode == Platform.MinNode && Old.Normal == Platform.Normal;
			});

			return !bStoodBefore && FRoomManagerTestAccess::GetZoneOf(*Manager, Platform) != Zone;
		});

		TestEqual(TEXT("Nothing laid outside it"), NewOutside, 0);
		TestTrue(TEXT("One laid for each taken, where there was room"), After.Num() <= Before && After.Num() >= Before - Waiting);
	}

	// Every platform holding an object still stands.
	const int32 HoldingGone = Algo::CountIf(Holding, [&Manager](const FGridPlatform& Platform)
	{
		return !FRoomManagerTestAccess::GetBuilt(*Manager).ContainsByPredicate([&Platform](const FGridPlatform& Built)
		{
			return Built.MinNode == Platform.MinNode && Built.Normal == Platform.Normal;
		});
	});

	TestEqual(TEXT("Platforms holding objects are never reshuffled"), HoldingGone, 0);
	TestEqual(TEXT("And the reshuffles agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionGroundTilesTest, "WEM.Deletion.GroundTiles", WEMRoomManagerTests::TestFlags)

bool FDeletionGroundTilesTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->bSeedEveryZone = true;
	Manager->bLinkToGroundTiles = true;
	Manager->ClearPlatforms();

	// The first tiles are the ground tiles, one a zone. Growth lays no more, since every zone has its own, and everything
	// it lays links to one.
	Grow(*Manager, 27);
	TestEqual(TEXT("27 first tiles, all ground tiles"), FRoomManagerTestAccess::GetGroundTiles(*Manager).Num(), 27);

	Grow(*Manager, 400);
	TestEqual(TEXT("Growth lays no more"), FRoomManagerTestAccess::GetGroundTiles(*Manager).Num(), 27);
	TestEqual(TEXT("Everything growth lays links to one"), FRoomManagerTestAccess::CountLoose(*Manager), 0);
	TestEqual(TEXT("Agreeing with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	FRoomManagerTestAccess::EnsureCandidates(*Manager);

	// Taking away the platform that most hangs on cuts loose exactly what linked through it alone, as worked out the plain
	// way from the beams shared, all in the one removal.
	{
		const FLinks Links = GetLinks(*Manager);
		TSet<int32> Cut;
		const int32 Weakest = FindWeakest(Links, Cut);
		const int32 Before = Manager->GetPlatformCount();
		AddInfo(FString::Printf(TEXT("%d platforms; taking away the weakest cuts %d loose."), Before, Cut.Num()));

		if (!TestTrue(TEXT("Something hangs on one platform alone"), Weakest != INDEX_NONE && Cut.Num() > 0))
		{
			ReleaseRooted(Rooted);
			return false;
		}

		FRoomManagerTestAccess::Remove(*Manager, { Links.Built[Weakest] });

		TestEqual(TEXT("It and all that hung on it go at once"), Manager->GetPlatformCount(), Before - 1 - Cut.Num());
		TestEqual(TEXT("Exactly what hung on it"), CountStanding(*Manager, Links, Cut), 0);
		TestEqual(TEXT("Counted as cut loose"), FRoomManagerTestAccess::GetCutLoose(*Manager), Cut.Num());
		TestEqual(TEXT("Everything left links"), FRoomManagerTestAccess::CountLoose(*Manager), 0);
		TestFalse(TEXT("The lists are kept"), FRoomManagerTestAccess::IsStale(*Manager));
		TestEqual(TEXT("And agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);
	}

	// A platform left with nothing around it - every one it shared a beam with taken away - goes at once.
	{
		const FLinks Links = GetLinks(*Manager);
		int32 Lonely = INDEX_NONE;

		for (int32 Index = 0; Index < Links.Built.Num() && Lonely == INDEX_NONE; ++Index)
		{
			const bool bNextToGround = Links.Linked[Index].ContainsByPredicate([&Links](const int32 Linked) { return Links.Ground.Contains(Linked); });
			Lonely = !Links.Ground.Contains(Index) && Links.Linked[Index].Num() >= 2 && !bNextToGround ? Index : INDEX_NONE;
		}

		if (TestTrue(TEXT("A platform with two or more around it, none a ground tile"), Lonely != INDEX_NONE))
		{
			TArray<FGridPlatform> Around;

			for (const int32 Linked : Links.Linked[Lonely])
			{
				Around.Add(Links.Built[Linked]);
			}

			FRoomManagerTestAccess::Remove(*Manager, Around);
			TestFalse(TEXT("A platform with nothing left to link through goes at once"), IsStanding(*Manager, Links.Built[Lonely]));
			TestEqual(TEXT("Everything left links"), FRoomManagerTestAccess::CountLoose(*Manager), 0);
		}
	}

	// A ground tile taken away is passed on to another platform of its zone, and what links neither to that one nor to any
	// other ground tile goes.
	{
		const FLinks Links = GetLinks(*Manager);
		int32 Ground = INDEX_NONE;
		int32 Zone = INDEX_NONE;

		for (const int32 Index : Links.Ground)
		{
			const int32 ItsZone = FRoomManagerTestAccess::GetZoneOf(*Manager, Links.Built[Index]);
			const int32 InZone = Algo::CountIf(Links.Built, [&](const FGridPlatform& Platform) { return FRoomManagerTestAccess::GetZoneOf(*Manager, Platform) == ItsZone; });

			if (InZone >= 8)
			{
				Ground = Index;
				Zone = ItsZone;
				break;
			}
		}

		if (!TestTrue(TEXT("A ground tile with company in its zone"), Ground != INDEX_NONE))
		{
			ReleaseRooted(Rooted);
			return false;
		}

		const int32 Before = Manager->GetPlatformCount();
		const int32 PassedBefore = FRoomManagerTestAccess::GetGroundTilesPassedOn(*Manager);
		FRoomManagerTestAccess::Remove(*Manager, { Links.Built[Ground] });

		const TArray<FGridPlatform> ZoneGround = FRoomManagerTestAccess::GetGroundTiles(*Manager).FilterByPredicate([&](const FGridPlatform& Platform)
		{
			return FRoomManagerTestAccess::GetZoneOf(*Manager, Platform) == Zone;
		});

		TestEqual(TEXT("Passed on"), FRoomManagerTestAccess::GetGroundTilesPassedOn(*Manager) - PassedBefore, 1);
		TestEqual(TEXT("Its zone has one again"), ZoneGround.Num(), 1);

		if (ZoneGround.Num() == 1)
		{
			const int32 Heir = Links.Built.IndexOfByPredicate([&](const FGridPlatform& Platform)
			{
				return Platform.MinNode == ZoneGround[0].MinNode && Platform.Normal == ZoneGround[0].Normal;
			});

			TArray<int32> Grounds = Links.Ground;
			Grounds.Remove(Ground);
			Grounds.Add(Heir);

			const TSet<int32> Cut = GetUnreached(Links, Grounds, { Ground });
			AddInfo(FString::Printf(TEXT("Zone %d's ground tile passed on to the %d-th platform; %d cut loose."), Zone, Heir, Cut.Num()));
			TestTrue(TEXT("To another platform that stood in its zone"), Heir != INDEX_NONE && Heir != Ground);
			TestEqual(TEXT("What links to no ground tile goes, and only that"), Manager->GetPlatformCount(), Before - 1 - Cut.Num());
			TestEqual(TEXT("All of it"), CountStanding(*Manager, Links, Cut), 0);
		}

		TestEqual(TEXT("Everything left links"), FRoomManagerTestAccess::CountLoose(*Manager), 0);
		TestEqual(TEXT("And agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);
	}

	// Worn down at random, ground tiles among the rest, with growth going on: nothing is ever left standing unlinked, and
	// no zone with platforms is ever without a ground tile.
	FRandomStream Stream(31);
	int32 Unlinked = 0;
	int32 ZonesWithout = 0;

	for (int32 Round = 0; Round < 8; ++Round)
	{
		RemoveAtRandom(*Manager, Stream, 12, Round % 2 == 0 ? 1 : 4);
		Grow(*Manager, 40);
		Unlinked += FRoomManagerTestAccess::CountLoose(*Manager);

		for (int32 Zone = 0; Zone < 27; ++Zone)
		{
			const bool bHasPlatforms = FRoomManagerTestAccess::GetBuilt(*Manager).ContainsByPredicate([&](const FGridPlatform& Platform)
			{
				return FRoomManagerTestAccess::GetZoneOf(*Manager, Platform) == Zone;
			});

			ZonesWithout += bHasPlatforms && FRoomManagerTestAccess::GetGroundTileCount(*Manager, Zone) == 0 ? 1 : 0;
		}
	}

	AddInfo(FString::Printf(TEXT("Worn at random: %d platforms. %s"), Manager->GetPlatformCount(), *Manager->DeletionSummary));
	TestEqual(TEXT("Nothing ever stands unlinked"), Unlinked, 0);
	TestEqual(TEXT("No zone with platforms goes without a ground tile"), ZonesWithout, 0);
	TestEqual(TEXT("And it all agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// A zone emptied has no ground tile until growth reaches it again, and the first platform laid there is its ground tile.
	{
		constexpr int32 Zone = 13;
		const TArray<FGridPlatform> InZone = FRoomManagerTestAccess::GetBuilt(*Manager).FilterByPredicate([&](const FGridPlatform& Platform)
		{
			return FRoomManagerTestAccess::GetZoneOf(*Manager, Platform) == Zone;
		});

		FRoomManagerTestAccess::Remove(*Manager, InZone);
		TestEqual(TEXT("An emptied zone has no ground tile"), FRoomManagerTestAccess::GetGroundTileCount(*Manager, Zone), 0);

		auto FindFirstInZone = [&]() -> const FGridPlatform*
		{
			return Manager->Platforms.FindByPredicate([&](const FGridPlatform& Platform) { return FRoomManagerTestAccess::GetZoneOf(*Manager, Platform) == Zone; });
		};

		int32 Beats = 0;

		while (!FindFirstInZone() && Beats < 2000 && Manager->PlaceNextPlatform())
		{
			++Beats;
		}

		const FGridPlatform* First = FindFirstInZone();
		AddInfo(FString::Printf(TEXT("Growth reached the emptied zone after %d beats."), Beats));
		TestTrue(TEXT("Growth reaches it again"), First != nullptr);
		TestTrue(TEXT("And the first laid there is its ground tile"), First && FRoomManagerTestAccess::IsGroundTile(*Manager, *First));
		TestEqual(TEXT("Its only one"), FRoomManagerTestAccess::GetGroundTileCount(*Manager, Zone), 1);
	}

	// Strikes cut loose too: every platform hit dies, and what hung on it goes with it, on the clock's few at a time.
	{
		Manager->StrikeFraction = 0.2f;
		Manager->MinDamage = 1000.0f;
		Manager->MaxDamage = 1000.0f;

		const int32 CutBefore = FRoomManagerTestAccess::GetCutLoose(*Manager);
		FRoomManagerTestAccess::Strike(*Manager);

		int32 Ticks = 0;

		while (FRoomManagerTestAccess::GetDyingCount(*Manager) > 0)
		{
			FRoomManagerTestAccess::RemoveDying(*Manager, 4);
			Unlinked += FRoomManagerTestAccess::CountLoose(*Manager);
			++Ticks;
		}

		AddInfo(FString::Printf(TEXT("A strike killing a fifth: %d ticks, %d cut loose with them; %d platforms left."),
			Ticks, FRoomManagerTestAccess::GetCutLoose(*Manager) - CutBefore, Manager->GetPlatformCount()));
		TestEqual(TEXT("Nothing stands unlinked after any tick"), Unlinked, 0);
		TestEqual(TEXT("And it agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);
	}

	// Off, a platform taken away takes nothing with it, and what hung on it stands unlinked.
	{
		Manager->bLinkToGroundTiles = false;
		Grow(*Manager, 200);

		const FLinks Links = GetLinks(*Manager);
		TSet<int32> Cut;
		const int32 Weakest = FindWeakest(Links, Cut);
		const int32 Before = Manager->GetPlatformCount();
		AddInfo(FString::Printf(TEXT("Off: taking away the weakest of %d would cut %d loose."), Before, Cut.Num()));
		TestTrue(TEXT("Something hangs on one platform alone"), Weakest != INDEX_NONE && Cut.Num() > 0);
		FRoomManagerTestAccess::Remove(*Manager, { Links.Built[FMath::Max(Weakest, 0)] });

		TestEqual(TEXT("Off, only the platform taken away goes"), Manager->GetPlatformCount(), Before - 1);
		TestEqual(TEXT("And what hung on it stands unlinked"), FRoomManagerTestAccess::CountLoose(*Manager), Cut.Num());
	}

	// Every seed is a ground tile: three a zone, three ground tiles a zone.
	Manager->bLinkToGroundTiles = true;
	Manager->SeedsPerZone = 3;
	Manager->ClearPlatforms();
	Grow(*Manager, 81);
	TestEqual(TEXT("Three seeds a zone, 81 ground tiles"), FRoomManagerTestAccess::GetGroundTiles(*Manager).Num(), 81);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionPocketsTest, "WEM.Deletion.Pockets", WEMRoomManagerTests::TestFlags)

bool FDeletionPocketsTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;
	using FPocket = FRoomManagerTestAccess::FPocket;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->AirPocketCount = 6;
	Manager->MinAirPocketSize = 16;
	Manager->MaxAirPocketSize = 40;
	Manager->MoldCount = 1;
	Manager->MinMoldSize = 48;
	Manager->MaxMoldSize = 64;
	Manager->ClearPlatforms();

	Grow(*Manager, 500);
	FRoomManagerTestAccess::EnsureCandidates(*Manager);

	auto CountCrossingPockets = [&Manager]() -> int32
	{
		return Algo::CountIf(FRoomManagerTestAccess::GetBuilt(*Manager), [&Manager](const FGridPlatform& Platform)
		{
			return !FRoomManagerTestAccess::IsClearOfPockets(*Manager, Platform);
		});
	};

	TestEqual(TEXT("Nothing grows into a pocket"), CountCrossingPockets(), 0);

	// Each moves on its own, the same shape, the others staying where they are, and the lists keep up.
	const TArray<FPocket> Before = FRoomManagerTestAccess::GetAirPockets(*Manager);
	int32 Moved = 0;

	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		FRoomManagerTestAccess::MovePocket(*Manager, Index);
		const TArray<FPocket> After = FRoomManagerTestAccess::GetAirPockets(*Manager);

		Moved += After[Index].Centre.Equals(Before[Index].Centre) ? 0 : 1;
		TestTrue(TEXT("It keeps its shape"), After[Index].Shape == Before[Index].Shape && After[Index].HalfSize.Equals(Before[Index].HalfSize) && After[Index].bMold == Before[Index].bMold);
		TestFalse(TEXT("A move keeps the lists"), FRoomManagerTestAccess::IsStale(*Manager));

		for (int32 Other = Index + 1; Other < Before.Num(); ++Other)
		{
			TestTrue(TEXT("The ones still to move stay"), After[Other].Centre.Equals(Before[Other].Centre));
		}
	}

	const int32 Crossing = CountCrossingPockets();
	AddInfo(FString::Printf(TEXT("%d of %d moved; %d platforms now stand in pockets. %s"), Moved, Before.Num(), Crossing, *Manager->DeletionSummary));
	TestTrue(TEXT("They move"), Moved > Before.Num() / 2);
	TestEqual(TEXT("The lists agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// What stands where one formed stays - through a rebuild too - and is vulnerable.
	const int32 Standing = Manager->GetPlatformCount();
	FRoomManagerTestAccess::Rebuild(*Manager, Manager->Platforms);
	TestEqual(TEXT("The rebuild keeps what stands in a pocket"), Manager->GetPlatformCount(), Standing);
	TestTrue(TEXT("Standing in a pocket is vulnerable"), CountVulnerable(*Manager) >= Crossing);

	// Growth from here keeps out of where they now are.
	Grow(*Manager, 200);
	TestEqual(TEXT("Nothing new grows into them"), CountCrossingPockets(), Crossing);
	TestEqual(TEXT("Growing agrees with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// As play moves one - a slab a tick, breaking down and then forming, with growth going on between - the lists keep up
	// at every step, and it ends up held whole.
	FRoomManagerTestAccess::EnsureCandidates(*Manager);
	FRoomManagerTestAccess::StartPocketMove(*Manager, 0);

	int32 Steps = 0;
	int32 Drift = 0;

	while (FRoomManagerTestAccess::IsPocketMoving(*Manager) && Steps < 1000)
	{
		FRoomManagerTestAccess::StepPocketMove(*Manager);
		Manager->PlaceNextPlatform();

		if (++Steps % 5 == 0)
		{
			Drift = FRoomManagerTestAccess::Verify(*Manager);
			FRoomManagerTestAccess::EnsureCandidates(*Manager);
		}
	}

	AddInfo(FString::Printf(TEXT("A move a slab at a time took %d ticks."), Steps));
	TestTrue(TEXT("It takes several ticks"), Steps > 2 && Steps < 1000);
	TestEqual(TEXT("And agrees with a full rebuild at every step"), Drift, 0);
	TestEqual(TEXT("It ends up held whole"), FRoomManagerTestAccess::GetAirPockets(*Manager)[0].CutPieces > 0, true);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionMoldPullTest, "WEM.Deletion.MoldPull", WEMRoomManagerTests::TestFlags)

bool FDeletionMoldPullTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// The same cube and molds grown with molds drawn alike and drawn heavily: the heavy one has more in its molds.
	auto CountInMolds = [&](const float Weight) -> int32
	{
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
		Manager->GridSize = 129;
		Manager->MoldCount = 2;
		Manager->MinMoldSize = 48;
		Manager->MaxMoldSize = 64;
		Manager->MoldSpread = 0.2f;
		Manager->MoldGrowthWeight = Weight;
		Manager->ClearPlatforms();
		Grow(*Manager, 600);

		return Algo::CountIf(FRoomManagerTestAccess::GetBuilt(*Manager), [&Manager](const FGridPlatform& Platform)
		{
			return FRoomManagerTestAccess::IsInMoldBox(*Manager, Platform);
		});
	};

	const int32 Alike = CountInMolds(1.0f);
	const int32 Pulled = CountInMolds(8.0f);
	AddInfo(FString::Printf(TEXT("After 600 beats, %d platforms in the molds drawn alike, %d drawn eight times as likely."), Alike, Pulled));
	TestTrue(TEXT("A mold drawn heavily fills sooner"), Pulled > Alike);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBeatsPerFrameTest, "WEM.Platforms.BeatsPerFrame", WEMRoomManagerTests::TestFlags)

bool FBeatsPerFrameTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->PlatformPlacementInterval = 0.01f;
	Manager->ClearPlatforms();
	Grow(*Manager, 20);

	// A frame that owes eight beats - 85 ms at a 10 ms interval - runs one by default, as before.
	int32 Before = Manager->GetPlatformCount();
	FRoomManagerTestAccess::AdvancePlacement(*Manager, 0.085);
	TestEqual(TEXT("One beat a frame by default"), Manager->GetPlatformCount() - Before, 1);

	// Allowed eight, with time to spare, it runs all eight; owed a hundred, still eight, and the rest are dropped.
	Manager->MaxBeatsPerFrame = 8;
	Manager->BeatBudgetMilliseconds = 1000.0f;
	Before = Manager->GetPlatformCount();
	const double Start = FPlatformTime::Seconds();
	FRoomManagerTestAccess::AdvancePlacement(*Manager, 0.085);
	AddInfo(FString::Printf(TEXT("Eight beats took %.2f ms."), 1000.0 * (FPlatformTime::Seconds() - Start)));
	TestEqual(TEXT("Eight owed, eight run"), Manager->GetPlatformCount() - Before, 8);

	Before = Manager->GetPlatformCount();
	FRoomManagerTestAccess::AdvancePlacement(*Manager, 1.0);
	TestEqual(TEXT("A hundred owed, eight run"), Manager->GetPlatformCount() - Before, 8);

	// With no time to spare, only the frame's first runs.
	Manager->BeatBudgetMilliseconds = 0.0f;
	Before = Manager->GetPlatformCount();
	FRoomManagerTestAccess::AdvancePlacement(*Manager, 0.085);
	TestEqual(TEXT("Out of budget, the first beat only"), Manager->GetPlatformCount() - Before, 1);

	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeletionPerfTest, "WEM.Deletion.Perf", WEMRoomManagerTests::SlowTestFlags)

bool FDeletionPerfTest::RunTest(const FString& Parameters)
{
	using namespace DeletionTestsPrivate;

	// A NewMap-sized cube, 27 first tiles, six pockets and two molds, grown to 3000 platforms. Then play's deletion as the
	// clock does it, timed: strikes, the removals a few a tick, reshuffle steps, pocket moves - with growth going on.
	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 509;
	Manager->PlatformRandomSeed = 4242;
	Manager->bSeedEveryZone = true;
	Manager->AirPocketCount = 6;
	Manager->MoldCount = 2;
	Manager->bLinkToGroundTiles = true;
	Manager->ClearPlatforms();

	while (Manager->GetPlatformCount() < 3000 && Manager->PlaceNextPlatform())
	{
	}

	FRoomManagerTestAccess::EnsureCandidates(*Manager);

	struct FTimes
	{
		double Total = 0.0;
		double Longest = 0.0;
		int32 Count = 0;

		void Add(const double Seconds)
		{
			Total += 1000.0 * Seconds;
			Longest = FMath::Max(Longest, 1000.0 * Seconds);
			++Count;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("%d, mean %.2f ms, longest %.2f ms"), Count, Count > 0 ? Total / Count : 0.0, Longest);
		}
	};

	FTimes Beats, Strikes, Removals, CuttingRemovals, Reshuffles, Moves;
	int32 MostCut = 0;

	auto Time = [](FTimes& Times, TFunctionRef<void()> Work)
	{
		const double Start = FPlatformTime::Seconds();
		Work();
		Times.Add(FPlatformTime::Seconds() - Start);
	};

	// A removal tick, kept apart by whether it cut anything loose from the ground tiles.
	auto TimeRemoval = [&]()
	{
		const int32 CutBefore = FRoomManagerTestAccess::GetCutLoose(*Manager);
		const double Start = FPlatformTime::Seconds();
		FRoomManagerTestAccess::RemoveDying(*Manager, 4);
		const double Seconds = FPlatformTime::Seconds() - Start;
		const int32 Cut = FRoomManagerTestAccess::GetCutLoose(*Manager) - CutBefore;

		(Cut > 0 ? CuttingRemovals : Removals).Add(Seconds);
		MostCut = FMath::Max(MostCut, Cut);
	};

	// About 60 beats a second, as NewMap's 0.0107 s beat comes out at 60 frames a second, a clock tick every three
	// beats, a strike every two seconds, a reshuffle every ten, a pocket move every fifteen: 40 simulated seconds.
	for (int32 Beat = 0; Beat < 2400; ++Beat)
	{
		Time(Beats, [&] { Manager->PlaceNextPlatform(); });

		if (Beat % 3 == 0 && FRoomManagerTestAccess::GetDyingCount(*Manager) > 0)
		{
			TimeRemoval();
		}

		if (Beat % 120 == 60)
		{
			Time(Strikes, [&] { FRoomManagerTestAccess::Strike(*Manager); });
		}

		if (Beat % 600 == 300)
		{
			FRoomManagerTestAccess::BeginReshuffle(*Manager);
		}

		if (Beat % 15 == 0)
		{
			int32 Waiting = 0;

			if (FRoomManagerTestAccess::GetReshuffleZone(*Manager, Waiting) != INDEX_NONE)
			{
				Time(Reshuffles, [&] { FRoomManagerTestAccess::StepReshuffle(*Manager); });
			}
		}

		// A pocket move every fifteen seconds, stepped a slab a clock tick.
		if (Beat % 900 == 450 && !FRoomManagerTestAccess::GetAirPockets(*Manager).IsEmpty())
		{
			FRoomManagerTestAccess::StartPocketMove(*Manager, (Beat / 900) % FRoomManagerTestAccess::GetAirPockets(*Manager).Num());
		}

		if (Beat % 3 == 0 && FRoomManagerTestAccess::IsPocketMoving(*Manager))
		{
			Time(Moves, [&] { FRoomManagerTestAccess::StepPocketMove(*Manager); });
		}
	}

	TestFalse(TEXT("Deletion never leaves the lists to be gathered afresh"), FRoomManagerTestAccess::IsStale(*Manager));

	AddInfo(FString::Printf(TEXT("%d platforms standing. %s"), Manager->GetPlatformCount(), *Manager->DeletionSummary));
	AddInfo(FString::Printf(TEXT("Beats: %s. Strikes: %s. Removal ticks (4 at most): %s. Reshuffle steps: %s. Pocket move steps (a slab each): %s."),
		*Beats.Describe(), *Strikes.Describe(), *Removals.Describe(), *Reshuffles.Describe(), *Moves.Describe()));

	// 40 seconds kill too few to cut much loose. A strike that kills one in twenty, taken away as the clock takes them -
	// 4 a tick, three beats of growth between ticks - shows what cutting loose costs at a high rate of deaths.
	Manager->StrikeFraction = 0.05f;
	Manager->MinDamage = 1000.0f;
	Manager->MaxDamage = 1000.0f;

	const int32 BeforeHeavy = Manager->GetPlatformCount();
	const int32 CutBeforeHeavy = FRoomManagerTestAccess::GetCutLoose(*Manager);
	FRoomManagerTestAccess::Strike(*Manager);
	const int32 Killed = FRoomManagerTestAccess::GetDyingCount(*Manager);

	while (FRoomManagerTestAccess::GetDyingCount(*Manager) > 0)
	{
		TimeRemoval();

		for (int32 Beat = 0; Beat < 3; ++Beat)
		{
			Time(Beats, [&] { Manager->PlaceNextPlatform(); });
		}
	}

	AddInfo(FString::Printf(TEXT("A strike killing %d of %d: %d more cut loose with them, %d at most in one tick. Removal ticks cutting nothing: %s. Ticks cutting some loose: %s."),
		Killed, BeforeHeavy, FRoomManagerTestAccess::GetCutLoose(*Manager) - CutBeforeHeavy, MostCut, *Removals.Describe(), *CuttingRemovals.Describe()));
	TestEqual(TEXT("Nothing is left standing unlinked"), FRoomManagerTestAccess::CountLoose(*Manager), 0);
	TestFalse(TEXT("Nor are the lists left to be gathered afresh"), FRoomManagerTestAccess::IsStale(*Manager));

	ReleaseRooted(Rooted);
	return true;
}

#endif
