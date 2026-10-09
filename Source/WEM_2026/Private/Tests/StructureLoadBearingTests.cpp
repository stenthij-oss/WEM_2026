// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace StructureLoadBearingTestsPrivate
{
	using namespace WEMRoomManagerTests;

	constexpr EStructureType AllTypes = EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall;

	FGridPlatform MakeFilled(const FIntVector& MinNode, const EGridAxis Normal, UPlatformTileData* Tile, UPlatformStructureData* Structure)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = Normal;
		Platform.Tile = Tile;
		Platform.PositiveStructure = Structure;
		Platform.NegativeStructure = Structure;
		return Platform;
	}

	FGridPlatform MakeOpen(const FIntVector& MinNode, const EGridAxis Normal, UPlatformTileData* Tile)
	{
		return MakeFilled(MinNode, Normal, Tile, nullptr);
	}

	/** A two-sided 16 x 16 slab of every type, load-bearing or not. */
	UPlatformStructureData* MakeSlab(const bool bLoadBearing, TArray<UObject*>& OutRooted, const float Weight = 1.0f, const int32 Width = 16, const int32 Length = 16)
	{
		UPlatformStructureData* Slab = MakeStructure(bLoadBearing ? TEXT("TestBearingSlab") : TEXT("TestLightSlab"), Width, Length, AllTypes, true, /*bMayLieSideways=*/true);
		Slab->bLoadBearing = bLoadBearing;
		Slab->Weight = Weight;
		OutRooted.Add(Slab);
		return Slab;
	}

	/**
	 * Per tile size, a load-bearing and a light two-sided slab, the light ones taking LightShare of
	 * each size's weight.
	 */
	TArray<TObjectPtr<UPlatformStructureData>> MakeWeightedSlabs(const float LightShare, TArray<UObject*>& OutRooted)
	{
		TArray<TObjectPtr<UPlatformStructureData>> Structures;

		for (const FIntPoint Size : { FIntPoint(16, 16), FIntPoint(8, 8), FIntPoint(8, 16) })
		{
			Structures.Add(MakeSlab(true, OutRooted, 1.0f - LightShare, Size.X, Size.Y));

			if (LightShare > 0.0f)
			{
				Structures.Add(MakeSlab(false, OutRooted, LightShare, Size.X, Size.Y));
			}
		}

		return Structures;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureLoadBearingWallTest, "WEM.Structures.LoadBearing.Wall", WEMRoomManagerTests::TestFlags)

bool FStructureLoadBearingWallTest::RunTest(const FString& Parameters)
{
	using namespace StructureLoadBearingTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Bearing = MakeSlab(true, Rooted);
	UPlatformStructureData* Light = MakeSlab(false, Rooted);

	// A floor from X 16 to 32, and on its east edge a wall that is not load-bearing, Y 16 to 32 and
	// Z 32 to 48.
	const FGridPlatform Floor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Bearing);
	const FGridPlatform Wall = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, Light);
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor, Wall });

	// Nothing is built onto its top: no wall carrying it on upward, and no floor or ceiling either side.
	TestFalse(TEXT("No wall stands on its top"), Manager->CanPlacePlatform(MakeOpen(FIntVector(32, 16, 48), EGridAxis::X, Square)));
	TestFalse(TEXT("No floor runs east off its top"), Manager->CanPlacePlatform(MakeOpen(FIntVector(32, 16, 48), EGridAxis::Z, Square)));
	TestFalse(TEXT("No ceiling runs west off its top"), Manager->CanPlacePlatform(MakeOpen(FIntVector(16, 16, 48), EGridAxis::Z, Square)));

	// But a panel can join beside it, sharing its side beam and the post at its top corner.
	TestTrue(TEXT("A panel can join beside it"), Manager->CanPlacePlatform(MakeOpen(FIntVector(32, 32, 32), EGridAxis::X, Square)));

	// Its top beam is not built; its side beams are, to full height, and so are the posts at the top
	// corners they end at.
	TestFalse(TEXT("Its top beam is not built"), Manager->IsCellSolid(FIntVector(32, 18, 48)));
	TestTrue(TEXT("Its south side beam is built to the top"), Manager->IsCellSolid(FIntVector(32, 16, 46)));
	TestTrue(TEXT("Its north side beam is built to the top"), Manager->IsCellSolid(FIntVector(32, 32, 46)));
	TestTrue(TEXT("The post at its south top corner is built"), Manager->IsCellSolid(FIntVector(32, 16, 48)));
	TestTrue(TEXT("The post at its north top corner is built"), Manager->IsCellSolid(FIntVector(32, 32, 48)));
	TestEqual(TEXT("A beam that is not built carries nothing"), Manager->GetSurfaceCapacity(FIntVector(32, 18, 48), EGridFace::PosX), ESurfaceCapacity::Empty);

	// With the panel beside it in place, the post it shares stands, and the wall's top still holds nothing.
	const FGridPlatform Beside = MakeFilled(FIntVector(32, 32, 32), EGridAxis::X, Square, Bearing);
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor, Wall, Beside });
	TestEqual(TEXT("All three stand"), Manager->GetPlatformCount(), 3);
	TestTrue(TEXT("The shared top corner post is built"), Manager->IsCellSolid(FIntVector(32, 32, 48)));
	TestTrue(TEXT("The panel's own top beam is built"), Manager->IsCellSolid(FIntVector(32, 34, 48)));
	TestFalse(TEXT("The wall's top beam still is not"), Manager->IsCellSolid(FIntVector(32, 18, 48)));

	// A wall hanging from a ceiling: its top beam is the ceiling's edge, which the ceiling uses unblocked.
	const FGridPlatform Ceiling = MakeFilled(FIntVector(16, 16, 48), EGridAxis::Z, Square, Bearing);
	FRoomManagerTestAccess::Rebuild(*Manager, { Ceiling, Wall });
	TestEqual(TEXT("The ceiling and the wall hanging from it stand"), Manager->GetPlatformCount(), 2);
	TestTrue(TEXT("The top beam it shares with the ceiling is built"), Manager->IsCellSolid(FIntVector(32, 18, 48)));
	TestFalse(TEXT("Still nothing more is built onto it"), Manager->CanPlacePlatform(MakeOpen(FIntVector(32, 16, 48), EGridAxis::X, Square)));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureLoadBearingFloorTest, "WEM.Structures.LoadBearing.Floor", WEMRoomManagerTests::TestFlags)

bool FStructureLoadBearingFloorTest::RunTest(const FString& Parameters)
{
	using namespace StructureLoadBearingTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Bearing = MakeSlab(true, Rooted);
	UPlatformStructureData* Light = MakeSlab(false, Rooted);

	// A floor that is not load-bearing grown east off one that is: it blocks the beams it brought, and
	// leaves open the one it grew from.
	const FGridPlatform West = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Bearing);
	const FGridPlatform East = MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, Light);
	FRoomManagerTestAccess::Rebuild(*Manager, { West, East });

	TestTrue(TEXT("The beam it grew from is built"), Manager->IsCellSolid(FIntVector(32, 18, 32)));
	TestFalse(TEXT("Its far beam is not"), Manager->IsCellSolid(FIntVector(48, 18, 32)));
	TestFalse(TEXT("Nor its north beam"), Manager->IsCellSolid(FIntVector(34, 32, 32)));
	TestFalse(TEXT("Nor the post at its far corner, where only blocked beams end"), Manager->IsCellSolid(FIntVector(48, 16, 32)));
	TestTrue(TEXT("The post where the open beam ends is built"), Manager->IsCellSolid(FIntVector(32, 16, 32)));

	TestFalse(TEXT("No wall stands on its far edge"), Manager->CanPlacePlatform(MakeOpen(FIntVector(48, 16, 32), EGridAxis::X, Square)));
	TestFalse(TEXT("No floor carries it on east"), Manager->CanPlacePlatform(MakeOpen(FIntVector(48, 16, 32), EGridAxis::Z, Square)));
	TestFalse(TEXT("No wall hangs from its north edge"), Manager->CanPlacePlatform(MakeOpen(FIntVector(32, 32, 16), EGridAxis::Y, Square)));

	// The same two the other way round: the light one first can only be the first platform in a list
	// written by hand, and then it blocks every beam it has - so the second is refused by the rebuild.
	FRoomManagerTestAccess::Rebuild(*Manager, { East, West });
	TestEqual(TEXT("A platform taking in a blocked beam is left out of a rebuild"), Manager->GetPlatformCount(), 1);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureLoadBearingGrowthTest, "WEM.Structures.LoadBearing.Growth", WEMRoomManagerTests::TestFlags)

bool FStructureLoadBearingGrowthTest::RunTest(const FString& Parameters)
{
	using namespace StructureLoadBearingTestsPrivate;

	IConsoleVariable* VerifyEvery = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.RoomManager.VerifyEvery"));

	if (!TestNotNull(TEXT("The VerifyEvery console variable exists"), VerifyEvery))
	{
		return false;
	}

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Mostly light structures, so blocking comes up all the time.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, MakeWeightedSlabs(0.6f, Rooted));
	Manager->bShowPieces = true;
	Manager->ClearPlatforms();

	const int32 Previous = VerifyEvery->GetInt();
	VerifyEvery->Set(1, ECVF_SetByCode);

	int32 Placed = 0;
	while (Placed < 200 && Manager->PlaceNextPlatform())
	{
		++Placed;
	}

	VerifyEvery->Set(Previous, ECVF_SetByCode);

	TestTrue(TEXT("Growth takes off"), Placed > 10);
	TestEqual(TEXT("Every placement matched a full rebuild"), FRoomManagerTestAccess::GetFailedVerifications(*Manager), 0);

	const FGridPlatform& First = Manager->Platforms[0];
	TestTrue(TEXT("The first platform is always load-bearing"), First.PositiveStructure->bLoadBearing && First.NegativeStructure->bLoadBearing);

	int32 Light = 0;
	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		Light += Platform.PositiveStructure->bLoadBearing ? 0 : 1;
	}

	TestTrue(TEXT("Light structures were placed"), Light > 0);
	AddInfo(FString::Printf(TEXT("%d placement(s), %d not load-bearing, before growth %s."), Placed, Light, Placed < 200 ? TEXT("stopped") : TEXT("was ended")));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureLoadBearingStallTest, "WEM.Structures.LoadBearing.Stall", WEMRoomManagerTests::SlowTestFlags)

bool FStructureLoadBearingStallTest::RunTest(const FString& Parameters)
{
	using namespace StructureLoadBearingTestsPrivate;

	// Growth to its end, with and without 5% of the fill weight on structures that are not load-
	// bearing, over several seeds: how many platforms stand when growth stops, and why it stopped.
	for (const float LightShare : { 0.0f, 0.05f })
	{
		TArray<int32> Counts;
		TArray<int32> LightCounts;

		for (int32 Seed = 1; Seed <= 6; ++Seed)
		{
			FScopedTestWorld TestWorld;
			TArray<UObject*> Rooted;
			ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, MakeWeightedSlabs(LightShare, Rooted));
			Manager->GridSize = 129;
			Manager->PlatformRandomSeed = Seed;
			Manager->ClearPlatforms();

			while (Manager->PlaceNextPlatform())
			{
			}

			int32 Light = 0;
			for (const FGridPlatform& Platform : Manager->Platforms)
			{
				Light += Platform.PositiveStructure->bLoadBearing ? 0 : 1;
			}

			Counts.Add(Manager->GetPlatformCount());
			LightCounts.Add(Light);
			ReleaseRooted(Rooted);
		}

		Counts.Sort();
		AddInfo(FString::Printf(TEXT("%.0f%% light: growth stopped at %s platforms (median %d); light ones placed: %s."),
			LightShare * 100.0f,
			*FString::JoinBy(Counts, TEXT(", "), [](const int32 Count) { return FString::FromInt(Count); }),
			Counts[Counts.Num() / 2],
			*FString::JoinBy(LightCounts, TEXT(", "), [](const int32 Count) { return FString::FromInt(Count); })));
	}

	return true;
}

#endif
