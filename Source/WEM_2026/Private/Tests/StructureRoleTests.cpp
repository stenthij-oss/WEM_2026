// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace StructureRoleTestsPrivate
{
	using namespace WEMRoomManagerTests;

	constexpr EStructureType AllTypes = EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall;

	FGridPlatform MakeFilled(
		const FIntVector& MinNode,
		const EGridAxis Normal,
		UPlatformTileData* Tile,
		UPlatformStructureData* Structure,
		const EGridFace Up = EGridFace::PosZ,
		const bool bTurned = false)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = Normal;
		Platform.Tile = Tile;
		Platform.bTurned = bTurned;
		Platform.Up = Up;
		Platform.PositiveStructure = Structure;
		Platform.NegativeStructure = Structure;
		return Platform;
	}

	/** A two-sided slab of every type, free to lie sideways, load-bearing or not. */
	UPlatformStructureData* MakeSlab(const int32 Width, const int32 Length, const bool bLoadBearing, TArray<UObject*>& OutRooted)
	{
		UPlatformStructureData* Slab = MakeStructure(TEXT("TestRoleSlab"), Width, Length, AllTypes, /*bTwoSided=*/true, /*bMayLieSideways=*/true);
		Slab->bLoadBearing = bLoadBearing;
		OutRooted.Add(Slab);
		return Slab;
	}

	FString DescribeUp(const EGridFace Up)
	{
		return StaticEnum<EGridFace>()->GetDisplayNameTextByValue(static_cast<int64>(Up)).ToString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleInheritTest, "WEM.Structures.Roles.Inherit", WEMRoomManagerTests::TestFlags)

bool FStructureRoleInheritTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Slab = MakeSlab(16, 16, true, Rooted);

	// An upside-down floor: anything growing off it takes its Up, whichever way it grows.
	const FGridPlatform Floor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab, EGridFace::NegZ);
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor });

	TestEqual(TEXT("A wall folding off it takes its Up"),
		FRoomManagerTestAccess::ChooseUp(*Manager, MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr)), EGridFace::NegZ);
	TestEqual(TEXT("A floor carrying it on takes its Up"),
		FRoomManagerTestAccess::ChooseUp(*Manager, MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, nullptr)), EGridFace::NegZ);

	// A placement touching two platforms takes the Up of one of them, drawn at random: both turn up.
	const FGridPlatform Wall = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, Slab, EGridFace::PosY);
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor, Wall });
	TestEqual(TEXT("Both stand"), Manager->GetPlatformCount(), 2);

	const FGridPlatform Between = MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, nullptr);
	int32 FromFloor = 0;
	int32 FromWall = 0;

	for (int32 Draw = 0; Draw < 64; ++Draw)
	{
		const EGridFace Up = FRoomManagerTestAccess::ChooseUp(*Manager, Between);
		FromFloor += Up == EGridFace::NegZ ? 1 : 0;
		FromWall += Up == EGridFace::PosY ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("Of 64 draws, %d took the floor's Up and %d the wall's."), FromFloor, FromWall));
	TestEqual(TEXT("Every draw takes the Up of one of them"), FromFloor + FromWall, 64);
	TestTrue(TEXT("Both are drawn"), FromFloor > 0 && FromWall > 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleFallbackTest, "WEM.Structures.Roles.Fallback", WEMRoomManagerTests::TestFlags)

bool FStructureRoleFallbackTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Every size has a slab, but the 8 x 16 one only stands upright: it cannot fill an 8 x 16 wall whose
	// Up runs along its short side.
	UPlatformStructureData* Square = MakeSlab(16, 16, true, Rooted);
	UPlatformStructureData* Small = MakeSlab(8, 8, true, Rooted);
	UPlatformStructureData* Upright = MakeSlab(8, 16, true, Rooted);
	Upright->bMayLieSideways = false;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, TArray<TObjectPtr<UPlatformStructureData>>{ Square, Small, Upright });
	UPlatformTileData* SquareTile = Manager->Tiles[0];
	UPlatformTileData* LongTile = Manager->Tiles[2];

	// A floor in the world that is a wall in its own frame, its Up along Y.
	const FGridPlatform Sideways = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, SquareTile, Square, EGridFace::PosY);
	FRoomManagerTestAccess::Rebuild(*Manager, { Sideways });

	// An 8 x 16 wall facing X off its edge would take that Up, standing 8 along it - which nothing fills.
	// It falls back on the world's.
	const FGridPlatform Long = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, LongTile, nullptr);
	TestEqual(TEXT("A way up nothing can fill gives way to the world's"), FRoomManagerTestAccess::ChooseUp(*Manager, Long), EGridFace::PosZ);

	// A square one there can be filled either way up, and takes the floor's.
	const FGridPlatform SquareWall = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, SquareTile, nullptr);
	TestEqual(TEXT("One that can be filled takes it"), FRoomManagerTestAccess::ChooseUp(*Manager, SquareWall), EGridFace::PosY);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleFillTest, "WEM.Structures.Roles.Fill", WEMRoomManagerTests::TestFlags)

bool FStructureRoleFillTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	UPlatformStructureData* Ground = MakeStructure(TEXT("TestRoleGround"), 16, 16, EStructureType::Ground, false);
	UPlatformStructureData* Ceiling = MakeStructure(TEXT("TestRoleCeiling"), 16, 16, EStructureType::Ceiling, false);
	UPlatformStructureData* Wall = MakeStructure(TEXT("TestRoleWall"), 16, 16, EStructureType::Wall, false);
	Rooted.Append({ Ground, Ceiling, Wall });

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, TArray<TObjectPtr<UPlatformStructureData>>{ Ground, Ceiling, Wall });
	UPlatformTileData* Square = Manager->Tiles[0];

	// Upside down, a floor's ground goes on its underside and its ceiling on its top.
	FGridPlatform UpsideDown = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr, EGridFace::NegZ);
	TestTrue(TEXT("An upside-down floor is filled"), FRoomManagerTestAccess::DrawFill(*Manager, UpsideDown));
	TestEqual(TEXT("Its underside takes the ground"), UpsideDown.NegativeStructure.Get(), Ground);
	TestEqual(TEXT("Its top takes the ceiling"), UpsideDown.PositiveStructure.Get(), Ceiling);

	FGridPlatform RightWayUp = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr, EGridFace::PosZ);
	TestTrue(TEXT("The right way up it is filled"), FRoomManagerTestAccess::DrawFill(*Manager, RightWayUp));
	TestEqual(TEXT("Its top takes the ground"), RightWayUp.PositiveStructure.Get(), Ground);

	// A floor in the world whose Up lies in its plane is a wall, both faces.
	FGridPlatform LyingWall = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr, EGridFace::NegX);
	TestTrue(TEXT("A wall lying flat in the world is filled"), FRoomManagerTestAccess::DrawFill(*Manager, LyingWall));
	TestTrue(TEXT("With wall on both faces"), LyingWall.PositiveStructure == Wall && LyingWall.NegativeStructure == Wall);

	// And a wall in the world whose Up runs along its normal is a floor: ground along Up.
	FGridPlatform StandingFloor = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr, EGridFace::NegX);
	TestTrue(TEXT("A floor standing up in the world is filled"), FRoomManagerTestAccess::DrawFill(*Manager, StandingFloor));
	TestEqual(TEXT("Its face looking along -X takes the ground"), StandingFloor.NegativeStructure.Get(), Ground);
	TestEqual(TEXT("And its face looking along +X the ceiling"), StandingFloor.PositiveStructure.Get(), Ceiling);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleLoadBearingTest, "WEM.Structures.Roles.LoadBearing", WEMRoomManagerTests::TestFlags)

bool FStructureRoleLoadBearingTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Bearing = MakeSlab(16, 16, true, Rooted);
	UPlatformStructureData* Light = MakeSlab(16, 16, false, Rooted);

	// An upside-down floor at Z 32, and below its east edge an upside-down wall that is not load-bearing,
	// Z 16 to 32. In its own frame it stands on the floor, and its top is its lowest beam in the world.
	const FGridPlatform Floor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Bearing, EGridFace::NegZ);
	const FGridPlatform Wall = MakeFilled(FIntVector(32, 16, 16), EGridAxis::X, Square, Light, EGridFace::NegZ);
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor, Wall });
	TestEqual(TEXT("Both stand"), Manager->GetPlatformCount(), 2);

	TestFalse(TEXT("Its top beam, lowest in the world, is not built"), Manager->IsCellSolid(FIntVector(32, 18, 16)));
	TestTrue(TEXT("The beam it stands on, shared with the floor, is"), Manager->IsCellSolid(FIntVector(32, 18, 32)));
	TestTrue(TEXT("Its side beams are"), Manager->IsCellSolid(FIntVector(32, 16, 18)) && Manager->IsCellSolid(FIntVector(32, 32, 18)));
	TestFalse(TEXT("Nothing is built onto its top"), Manager->CanPlacePlatform(MakeFilled(FIntVector(32, 16, 0), EGridAxis::X, Square, nullptr)));
	TestTrue(TEXT("A panel can still join beside it"), Manager->CanPlacePlatform(MakeFilled(FIntVector(32, 32, 16), EGridAxis::X, Square, nullptr)));

	// The right way up, the same wall's top is the beam it shares with the floor, which the floor keeps.
	const FGridPlatform Hanging = MakeFilled(FIntVector(32, 16, 16), EGridAxis::X, Square, Light, EGridFace::PosZ);
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor, Hanging });
	TestTrue(TEXT("Hanging the right way up, its lowest beam is built"), Manager->IsCellSolid(FIntVector(32, 18, 16)));

	// A wall lying flat in the world, Up along +X: its top is its far edge along X.
	const FGridPlatform Lying = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Light, EGridFace::PosX);
	FRoomManagerTestAccess::Rebuild(*Manager, { Lying });
	TestFalse(TEXT("A lying wall's top, along +X, is not built"), Manager->IsCellSolid(FIntVector(32, 18, 32)));
	TestTrue(TEXT("Its foot is"), Manager->IsCellSolid(FIntVector(16, 18, 32)));
	TestTrue(TEXT("And so are its sides"), Manager->IsCellSolid(FIntVector(18, 16, 32)) && Manager->IsCellSolid(FIntVector(18, 32, 32)));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleFenceTest, "WEM.Structures.Roles.Fence", WEMRoomManagerTests::TestFlags)

bool FStructureRoleFenceTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* SquareTile = Manager->Tiles[0];
	UPlatformTileData* LongTile = Manager->Tiles[2];

	UPlatformStructureData* Fence = MakeStructure(TEXT("TestRoleFence"), 8, 16, AllTypes, /*bTwoSided=*/true, /*bMayLieSideways=*/true);
	Fence->bFence = true;
	Rooted.Add(Fence);

	// A wall fence runs across its Up: along the ground in its own frame.
	const FGridPlatform Upright = MakeFilled(FIntVector(32, 16, 16), EGridAxis::X, LongTile, Fence, EGridFace::PosZ);
	const FGridPlatform Sideways = MakeFilled(FIntVector(32, 16, 16), EGridAxis::X, LongTile, Fence, EGridFace::PosY);
	const FGridPlatform Lying = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, SquareTile, Fence, EGridFace::NegX);
	TestEqual(TEXT("Upright, it runs along Y"), FRoomManagerTestAccess::GetRunAxis(*Manager, Upright), 1);
	TestEqual(TEXT("With its Up along Y, it runs along Z"), FRoomManagerTestAccess::GetRunAxis(*Manager, Sideways), 2);
	TestEqual(TEXT("Lying flat with its Up along X, it runs along Y"), FRoomManagerTestAccess::GetRunAxis(*Manager, Lying), 1);

	// Round a corner, the upright panel's turns are laid as before: the same way round, reaching back as
	// far as it ran.
	FGridPlatform Straight;
	TArray<FGridPlatform> Turns;
	FRoomManagerTestAccess::GatherFenceContinuations(*Manager, Upright, 1, Straight, Turns);
	TestEqual(TEXT("An upright panel has two turns"), Turns.Num(), 2);

	if (Turns.Num() == 2)
	{
		TestTrue(TEXT("Facing along its run, laid as it is, keeping its Up"),
			Turns[0].Normal == EGridAxis::Y && !Turns[0].bTurned && Turns[0].Up == EGridFace::PosZ);
		TestTrue(TEXT("At the run's end, reaching either way"),
			Turns[0].MinNode == FIntVector(32, 24, 16) && Turns[1].MinNode == FIntVector(24, 24, 16));
	}

	// The sideways panel stands 8 along its Up and runs 16 along Z. Its turns face Z and have to be laid
	// turned to stand as tall - 16 along X, away from the wall, and 8 along Y.
	FRoomManagerTestAccess::GatherFenceContinuations(*Manager, Sideways, 1, Straight, Turns);
	TestEqual(TEXT("Straight on, a sideways panel goes along Z"), Straight.MinNode, FIntVector(32, 16, 32));
	TestEqual(TEXT("A sideways panel has two turns"), Turns.Num(), 2);

	if (Turns.Num() == 2)
	{
		TestTrue(TEXT("Facing Z, turned, keeping its Up"),
			Turns[0].Normal == EGridAxis::Z && Turns[0].bTurned && Turns[0].Up == EGridFace::PosY);
		TestTrue(TEXT("At the run's end, reaching 16 either way along X"),
			Turns[0].MinNode == FIntVector(32, 16, 32) && Turns[1].MinNode == FIntVector(16, 16, 32));
	}

	// A tile that may not turn has no corner to turn there.
	UPlatformTileData* Fixed = MakeTile(8, 16, /*bCanTurn=*/false);
	Rooted.Add(Fixed);
	FGridPlatform FixedSideways = Sideways;
	FixedSideways.Tile = Fixed;
	FRoomManagerTestAccess::GatherFenceContinuations(*Manager, FixedSideways, 1, Straight, Turns);
	TestEqual(TEXT("A tile that cannot turn has no corner to turn"), Turns.Num(), 0);

	// A wall stands on something along its foot or its top, which run across its Up. A wall facing Y
	// along the sideways panel's edge at Y 16 stands it; it does nothing for the upright one, whose
	// foot and top run along Y.
	UPlatformStructureData* Slab = MakeSlab(16, 16, true, Rooted);
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFilled(FIntVector(16, 16, 16), EGridAxis::Y, SquareTile, Slab) });
	TestTrue(TEXT("The sideways panel stands on the wall at its foot"), FRoomManagerTestAccess::StandsOnSomething(*Manager, Sideways));
	TestFalse(TEXT("The upright one does not"), FRoomManagerTestAccess::StandsOnSomething(*Manager, Upright));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleUnturnedGrowthTest, "WEM.Structures.Roles.UnturnedGrowth", WEMRoomManagerTests::TestFlags)

bool FStructureRoleUnturnedGrowthTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);

	for (int32 Beat = 0; Beat < 300; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	// With nothing turning it, every platform takes the world's Up from the first.
	int32 Turned = 0;

	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		if (Platform.Up != EGridFace::PosZ)
		{
			AddError(FString::Printf(TEXT("The platform at %s is up %s"), *Platform.MinNode.ToString(), *DescribeUp(Platform.Up)));
			++Turned;
		}
	}

	TestTrue(TEXT("Growth takes off"), Manager->GetPlatformCount() > 100);
	TestEqual(TEXT("No platform is turned"), Turned, 0);

	ReleaseRooted(Rooted);
	return true;
}

#endif
