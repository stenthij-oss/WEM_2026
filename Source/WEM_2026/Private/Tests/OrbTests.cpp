// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/PrimitiveComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "ObjectPlacer.h"
#include "Orb.h"
#include "OrbSnippets.h"
#include "OrbSpot.h"
#include "PlacementTestWorld.h"
#include "RoomManagerTestHelpers.h"

/** What the orb tests read of the placer's private state. Befriended by AObjectPlacer. */
struct FObjectPlacerTestAccess
{
	static void ResetOrbs(AObjectPlacer& Placer)
	{
		Placer.ResetOrbs();
	}

	static void AdvanceOrbs(AObjectPlacer& Placer)
	{
		Placer.AdvanceOrbs();
	}

	static void StandOrbs(AObjectPlacer& Placer)
	{
		Placer.StandOrbs();
	}

	static FGridPlatform GetPlatform(const AObjectPlacer& Placer, const int32 Zone)
	{
		return Placer.ZoneOrbs[Zone].Platform;
	}

	static FOrbSpot GetSpot(const AObjectPlacer& Placer, const int32 Zone)
	{
		return Placer.ZoneOrbs[Zone].Spot;
	}

	static int32 GetMoved(const AObjectPlacer& Placer)
	{
		return Placer.OrbsMoved;
	}
};

namespace OrbTestsPrivate
{
	using namespace WEMRoomManagerTests;

	bool IsSamePlatform(const FGridPlatform& A, const FGridPlatform& B)
	{
		return A.MinNode == B.MinNode && A.Normal == B.Normal;
	}

	/** The zones holding an orb that has not been caught. */
	TArray<int32> GetZonesWithOrbs(const AObjectPlacer& Placer)
	{
		TArray<int32> Zones;

		for (int32 Zone = 0; Zone < ARoomManager::GetZoneCount(); ++Zone)
		{
			if (const AOrb* Orb = Placer.GetZoneOrb(Zone); IsValid(Orb) && !Orb->IsCaught())
			{
				Zones.Add(Zone);
			}
		}

		return Zones;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrbSnippetsTest, "WEM.Orbs.Snippets", WEMRoomManagerTests::TestFlags)

bool FOrbSnippetsTest::RunTest(const FString& Parameters)
{
	// Paragraphs are snippets; notes, spare blank lines and the line endings of any editor are not.
	TArray<FString> Parsed;
	WEMOrbSnippets::Parse(TEXT("# a note\r\n\r\nFirst line\r\n  and its second  \r\n\r\n\r\n# another note\nSecond\n   \nThird"), Parsed);

	TestEqual(TEXT("Three snippets"), Parsed.Num(), 3);

	if (Parsed.Num() == 3)
	{
		TestEqual(TEXT("A line break inside a snippet is kept, and each line trimmed"), Parsed[0], FString(TEXT("First line\nand its second")));
		TestEqual(TEXT("A note between snippets is dropped"), Parsed[1], FString(TEXT("Second")));
		TestEqual(TEXT("A line of spaces ends a snippet"), Parsed[2], FString(TEXT("Third")));
	}

	// The folder the game reads: the snippets written for it are there.
	TArray<FString> Written;
	WEMOrbSnippets::LoadFolder(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Orbs/Snippets")), Written);
	AddInfo(FString::Printf(TEXT("%d snippets in Content/Orbs/Snippets."), Written.Num()));
	TestTrue(TEXT("The snippet folder holds snippets"), Written.Num() > 0);

	// Each drawn once a round, every round, and never twice running across a reroll.
	TArray<FString> Five = { TEXT("a"), TEXT("b"), TEXT("c"), TEXT("d"), TEXT("e") };
	FOrbSnippetBag Bag;
	Bag.Reset(Five, 7);

	bool bEachOnceARound = true;
	bool bNeverTwiceRunning = true;
	FString Previous;

	for (int32 Round = 0; Round < 40; ++Round)
	{
		TSet<FString> Seen;

		for (int32 Draw = 0; Draw < Five.Num(); ++Draw)
		{
			const FString Snippet = Bag.Draw();
			bool bAlreadySeen = false;
			Seen.Add(Snippet, &bAlreadySeen);
			bEachOnceARound &= !bAlreadySeen;
			bNeverTwiceRunning &= Snippet != Previous;
			Previous = Snippet;
		}
	}

	TestTrue(TEXT("Every snippet is shown once before any is shown again"), bEachOnceARound);
	TestTrue(TEXT("The first of a new round is never the one just shown"), bNeverTwiceRunning);
	TestEqual(TEXT("The bag is shuffled anew after every round"), Bag.GetRerolls(), 39);

	// One snippet comes round every time; none gives nothing.
	FOrbSnippetBag One;
	One.Reset({ TEXT("only") }, 1);
	TestEqual(TEXT("A single snippet is drawn"), One.Draw(), FString(TEXT("only")));
	TestEqual(TEXT("And drawn again"), One.Draw(), FString(TEXT("only")));

	FOrbSnippetBag None;
	None.Reset({}, 1);
	TestTrue(TEXT("No snippets draws nothing"), None.Draw().IsEmpty());

	// A snippet that fits below where text starts stays there; a longer one starts higher; one too long for that does
	// not fit (the HUD then writes it smaller). A frame 1000 tall, text starting at 540, ending above 940, its top below 120.
	float Start = 0.0f;
	TestTrue(TEXT("A short snippet fits"), WEMOrbSnippets::PlaceBlock(540.0f, 120.0f, 20.0f, 120.0f, 940.0f, Start));
	TestEqual(TEXT("Where text starts"), Start, 540.0f);
	TestTrue(TEXT("A longer one fits higher"), WEMOrbSnippets::PlaceBlock(540.0f, 600.0f, 20.0f, 120.0f, 940.0f, Start));
	TestEqual(TEXT("Just high enough to end at the bottom"), Start, 340.0f);
	TestFalse(TEXT("One too long does not fit"), WEMOrbSnippets::PlaceBlock(540.0f, 820.0f, 20.0f, 120.0f, 940.0f, Start));
	TestEqual(TEXT("And starts as high as it may"), Start, 140.0f);

	// Short snippets are written at the set speed; a long one faster, so it is written out within the longest time.
	TestEqual(TEXT("Short at the set speed"), WEMOrbSnippets::GetWritingSpeed(60, 10.0f, 15.0f), 10.0f);
	TestEqual(TEXT("The longest snippet within 15 s"), WEMOrbSnippets::GetWritingSpeed(639, 10.0f, 15.0f), 639.0f / 15.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrbSpotTest, "WEM.Orbs.Spot", WEMRoomManagerTests::TestFlags)

bool FOrbSpotTest::RunTest(const FString& Parameters)
{
	// A horizontal segment 13 cells square at height 10, carrying anything on both faces.
	FPlacementTestWorld World;
	World.AddSegment(2, FIntVector(10, 10, 10), 13, 13);

	const FIntVector PlatformMin(10, 10, 10);
	const FIntVector PlatformMax(22, 22, 10);

	FOrbSpot Spot;
	TestTrue(TEXT("Room in the middle of an empty segment"), WEMOrbSpot::Find(World, PlatformMin, PlatformMax, 5, 10, 4, Spot));
	TestEqual(TEXT("A square of five in the middle"), Spot.FootMin, FIntVector(14, 14, 10));
	TestEqual(TEXT("Its far corner"), Spot.FootMax, FIntVector(18, 18, 10));
	TestEqual(TEXT("Its air starts on the cell above the surface"), Spot.AirMin, FIntVector(14, 14, 11));
	TestEqual(TEXT("And runs ten cells up"), Spot.AirMax, FIntVector(18, 18, 20));
	TestEqual(TEXT("It hovers over the middle of the square's top faces"), WEMOrbSpot::GetFootCentre(World, Spot), FVector(16.5 * 20.0, 16.5 * 20.0, 11.0 * 20.0));

	// Something built in the middle's air: the nearest square clear of it, in a fixed order, three cells over.
	World.AddSolidBlock(FIntVector(16, 16, 15), FIntVector(16, 16, 15));
	TestFalse(TEXT("The middle is no longer free"), WEMOrbSpot::IsFree(World, Spot));
	TestTrue(TEXT("Room off the middle"), WEMOrbSpot::Find(World, PlatformMin, PlatformMax, 5, 10, 4, Spot));
	TestEqual(TEXT("The first square clear of it"), Spot.FootMin, FIntVector(14, 11, 10));

	TestFalse(TEXT("No room when it may not move off the middle"), WEMOrbSpot::Find(World, PlatformMin, PlatformMax, 5, 10, 0, Spot));

	// A surface that carries walls only will not do either.
	FPlacementTestWorld Walled;
	Walled.AddSegment(2, FIntVector(10, 10, 10), 13, 13);
	Walled.SetCapacity(FIntVector(16, 16, 10), EGridFace::PosZ, ESurfaceCapacity::Wall);
	TestTrue(TEXT("Room beside a wall surface"), WEMOrbSpot::Find(Walled, PlatformMin, PlatformMax, 5, 10, 4, Spot));
	TestTrue(TEXT("Clear of it"), Spot.FootMin.X > 16 || Spot.FootMax.X < 16 || Spot.FootMin.Y > 16 || Spot.FootMax.Y < 16);

	// Nothing over a segment that is not horizontal, or too small, or under the cube's top.
	TestFalse(TEXT("Not over a wall"), WEMOrbSpot::Find(World, FIntVector(10, 10, 10), FIntVector(22, 10, 22), 5, 10, 4, Spot));
	TestFalse(TEXT("Not over a segment narrower than the orb"), WEMOrbSpot::Find(World, FIntVector(10, 10, 10), FIntVector(12, 22, 10), 5, 10, 4, Spot));

	FPlacementTestWorld High;
	High.AddSegment(2, FIntVector(10, 10, 58), 13, 13);
	TestFalse(TEXT("Not where its air would leave the cube"), WEMOrbSpot::Find(High, FIntVector(10, 10, 58), FIntVector(22, 22, 58), 5, 10, 4, Spot));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrbPlatformHealthTest, "WEM.Orbs.PlatformHealth", WEMRoomManagerTests::TestFlags)

bool FOrbPlatformHealthTest::RunTest(const FString& Parameters)
{
	using namespace OrbTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->ClearPlatforms();

	for (int32 Beat = 0; Beat < 150; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	const TArray<FGridPlatform> Built = FRoomManagerTestAccess::GetBuilt(*Manager);
	TestEqual(TEXT("The standing platforms are the built ones"), Manager->GetStandingPlatforms().Num(), Built.Num());

	bool bAllStanding = true;
	bool bZonesAgree = true;

	for (const FGridPlatform& Platform : Built)
	{
		bAllStanding &= Manager->IsPlatformStanding(Platform);
		bZonesAgree &= Manager->GetPlatformZone(Platform) == FRoomManagerTestAccess::GetZoneOf(*Manager, Platform);
	}

	TestTrue(TEXT("Every built platform stands"), bAllStanding);
	TestTrue(TEXT("Its zone is the deletion's zone"), bZonesAgree);

	// A boost adds to what it has.
	const FGridPlatform First = Built[0];
	const float Before = FRoomManagerTestAccess::GetHealth(*Manager, First);
	TestEqual(TEXT("A boost adds to its health"), Manager->AddPlatformHealth(First, 300.0f), Before + 300.0f);
	TestEqual(TEXT("And keeps it"), FRoomManagerTestAccess::GetHealth(*Manager, First), Before + 300.0f);

	// Struck down to nothing, all of them wait to go; a boost spares the one it is given to.
	Manager->StrikeFraction = 1.0f;
	Manager->MinDamage = 1000.0f;
	Manager->MaxDamage = 1000.0f;
	FRoomManagerTestAccess::Strike(*Manager);
	TestEqual(TEXT("All wait to go"), FRoomManagerTestAccess::GetDyingCount(*Manager), Built.Num());

	const FGridPlatform Spared = Built.Last();
	TestTrue(TEXT("A boost lifts it above nothing"), Manager->AddPlatformHealth(Spared, 2000.0f) > 0.0f);
	TestEqual(TEXT("And takes it out of the queue"), FRoomManagerTestAccess::GetDyingCount(*Manager), Built.Num() - 1);

	FRoomManagerTestAccess::EnsureCandidates(*Manager);
	FRoomManagerTestAccess::RemoveDying(*Manager, MAX_int32);
	TestTrue(TEXT("The spared one still stands"), Manager->IsPlatformStanding(Spared));
	TestFalse(TEXT("The rest went"), Manager->IsPlatformStanding(First));
	TestEqual(TEXT("Nothing is given to a platform gone"), Manager->AddPlatformHealth(First, 300.0f), 0.0f);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrbZonesTest, "WEM.Orbs.Zones", WEMRoomManagerTests::TestFlags)

bool FOrbZonesTest::RunTest(const FString& Parameters)
{
	using namespace OrbTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Every zone seeded, and grown, so there are horizontal segments in all of them.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->bSeedEveryZone = true;
	Manager->ClearPlatforms();

	for (int32 Beat = 0; Beat < 400; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	AObjectPlacer* Placer = TestWorld.World->SpawnActor<AObjectPlacer>();
	Placer->RoomManager = Manager;
	Placer->RandomSeed = 99;
	Placer->bLogPlacements = false;
	Placer->bDrawDebug = false;
	FObjectPlacerTestAccess::ResetOrbs(*Placer);
	FObjectPlacerTestAccess::AdvanceOrbs(*Placer);

	const FRoomManagerPlacementWorld Grid(Manager);
	const TArray<int32> Zones = GetZonesWithOrbs(*Placer);
	AddInfo(FString::Printf(TEXT("%d platforms; orbs in %d of %d zones. %s"), Manager->GetPlatformCount(), Zones.Num(), ARoomManager::GetZoneCount(), *Placer->OrbSummary));
	TestTrue(TEXT("Orbs stand in most zones"), Zones.Num() >= ARoomManager::GetZoneCount() / 2);

	bool bOverHorizontal = true;
	bool bInItsZone = true;
	bool bOnFreeSpots = true;
	bool bAtHoverHeight = true;

	for (const int32 Zone : Zones)
	{
		const FGridPlatform Platform = FObjectPlacerTestAccess::GetPlatform(*Placer, Zone);
		const FOrbSpot Spot = FObjectPlacerTestAccess::GetSpot(*Placer, Zone);
		const FVector Expected = WEMOrbSpot::GetFootCentre(Grid, Spot) + FVector(0.0, 0.0, Placer->OrbHoverHeight);

		bOverHorizontal &= Platform.Normal == EGridAxis::Z && Spot.FootMin.Z == Platform.MinNode.Z;
		bInItsZone &= Manager->GetPlatformZone(Platform) == Zone;
		bOnFreeSpots &= WEMOrbSpot::IsFree(Grid, Spot);
		bAtHoverHeight &= Placer->GetZoneOrb(Zone)->GetActorLocation().Equals(Expected, 0.01);
	}

	// The AI overlay paints only the pieces' custom stencil value; an orb writes one of its own over them.
	if (const AOrb* Orb = Placer->GetZoneOrb(Zones.IsEmpty() ? 0 : Zones[0]))
	{
		const UPrimitiveComponent* Quad = Orb->FindComponentByClass<UPrimitiveComponent>();
		const IConsoleVariable* PieceStencil = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.AIOverlay.Stencil"));
		TestTrue(TEXT("An orb writes custom stencil"), Quad && Quad->bRenderCustomDepth);
		TestTrue(TEXT("Not the pieces' value, so the AI overlay leaves it alone"), Quad && Quad->CustomDepthStencilValue != (PieceStencil ? PieceStencil->GetInt() : 1));
	}

	TestTrue(TEXT("Every orb hovers over a horizontal segment"), bOverHorizontal);
	TestTrue(TEXT("Of its own zone"), bInItsZone);
	TestTrue(TEXT("Where the surface carries anything and the air is open"), bOnFreeSpots);
	TestTrue(TEXT("At its hover height"), bAtHoverHeight);

	if (Zones.IsEmpty())
	{
		ReleaseRooted(Rooted);
		return false;
	}

	// Its segment taken away, the orb stands again on another segment of the zone in the same frame. Tried in the zone
	// with the most horizontal segments, which has the most room to move to.
	TArray<int32> HorizontalPerZone;
	HorizontalPerZone.Init(0, ARoomManager::GetZoneCount());

	for (const FGridPlatform& Platform : Manager->GetStandingPlatforms())
	{
		if (Platform.Normal == EGridAxis::Z)
		{
			++HorizontalPerZone[Manager->GetPlatformZone(Platform)];
		}
	}

	int32 Zone = Zones[0];

	for (const int32 Each : Zones)
	{
		Zone = HorizontalPerZone[Each] > HorizontalPerZone[Zone] ? Each : Zone;
	}

	AOrb* OldOrb = Placer->GetZoneOrb(Zone);
	const FGridPlatform OldPlatform = FObjectPlacerTestAccess::GetPlatform(*Placer, Zone);

	FRoomManagerTestAccess::EnsureCandidates(*Manager);
	FRoomManagerTestAccess::Remove(*Manager, { OldPlatform });
	FObjectPlacerTestAccess::AdvanceOrbs(*Placer);

	AOrb* NewOrb = Placer->GetZoneOrb(Zone);
	TestEqual(TEXT("One orb had to move"), FObjectPlacerTestAccess::GetMoved(*Placer), 1);
	TestTrue(TEXT("The old orb is gone"), !IsValid(OldOrb));
	TestTrue(TEXT("A new one stands in the zone"), IsValid(NewOrb));
	TestFalse(TEXT("On another segment"), IsSamePlatform(FObjectPlacerTestAccess::GetPlatform(*Placer, Zone), OldPlatform));
	TestEqual(TEXT("Of the same zone"), Manager->GetPlatformZone(FObjectPlacerTestAccess::GetPlatform(*Placer, Zone)), Zone);

	// Nobody near: nothing is caught.
	TestFalse(TEXT("Nothing caught from afar"), Placer->CatchNearestOrb());

	// The view right at the orb: it is caught, its segment is given health, and the zone rests.
	TestWorld.World->ViewLocationsRenderedLastFrame = { NewOrb->GetActorLocation() };

	for (const int32 Each : Zones)
	{
		if (AOrb* Orb = Placer->GetZoneOrb(Each))
		{
			Orb->Tick(0.01f);
		}
	}

	const FGridPlatform CaughtPlatform = FObjectPlacerTestAccess::GetPlatform(*Placer, Zone);
	const float HealthBefore = FRoomManagerTestAccess::GetHealth(*Manager, CaughtPlatform);

	TestTrue(TEXT("The orb in view is near enough to catch"), NewOrb->CanBeCaught());
	TestTrue(TEXT("It is caught"), Placer->CatchNearestOrb());
	TestTrue(TEXT("It fades"), NewOrb->IsCaught());
	TestEqual(TEXT("The count goes up"), Placer->OrbsCaught, 1);
	TestEqual(TEXT("Its segment is given health"), FRoomManagerTestAccess::GetHealth(*Manager, CaughtPlatform), HealthBefore + Placer->CatchHealthBoost);
	TestFalse(TEXT("A fading orb is not caught again"), NewOrb->CanBeCaught());

	// Faded away, and the zone rests: no orb stands there however often the zones look.
	NewOrb->Tick(NewOrb->FadeSeconds + 0.1f);
	TestTrue(TEXT("It takes itself away once faded"), !IsValid(NewOrb));

	FObjectPlacerTestAccess::StandOrbs(*Placer);
	TestTrue(TEXT("The zone rests"), Placer->GetZoneOrb(Zone) == nullptr);

	const TArray<int32> After = GetZonesWithOrbs(*Placer);
	TestTrue(TEXT("The other zones keep theirs"), Zones.FilterByPredicate([Zone, &After](const int32 Each) { return Each != Zone && !After.Contains(Each); }).IsEmpty());

	ReleaseRooted(Rooted);
	return true;
}

#endif
