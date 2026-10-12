// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMath.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"
#include "StructureCategory.h"
#include "StructurePlacement.h"

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

	/** A one-sided pair of wall structures for a 16 x 16 wall, whose negative face holds nothing. */
	void MakeHalfHoldingWall(UPlatformStructureData*& OutHolds, UPlatformStructureData*& OutHoldsNothing, TArray<UObject*>& OutRooted)
	{
		OutHolds = MakeStructure(TEXT("TestRoleWallHolds"), 16, 16, EStructureType::Wall, false);
		OutHoldsNothing = MakeStructure(TEXT("TestRoleWallBare"), 16, 16, EStructureType::Wall, false);
		OutHoldsNothing->bHoldsItems = false;
		OutRooted.Append({ OutHolds, OutHoldsNothing });
	}

	/**
	 * Why a platform's fill does not fit it as it stands, its Up included, or an empty string when it
	 * does: a structure on each face fitting that face, the two pairing back to back, the same one on
	 * both when it is two-sided.
	 */
	FString CheckFill(const FGridPlatform& Platform)
	{
		const UPlatformStructureData* Positive = Platform.PositiveStructure;
		const UPlatformStructureData* Negative = Platform.NegativeStructure;

		if (!Positive || !Negative)
		{
			return TEXT("a face is empty");
		}

		if (Positive->bTwoSided != Negative->bTwoSided || (Positive->bTwoSided && Positive != Negative))
		{
			return TEXT("a two-sided structure fills one face but not the other");
		}

		const int32 SpanFirst = Platform.bTurned ? Platform.Tile->Length : Platform.Tile->Width;
		const int32 SpanSecond = Platform.bTurned ? Platform.Tile->Width : Platform.Tile->Length;
		const int32 NormalAxis = static_cast<int32>(Platform.Normal);
		bool bTurned = false;

		if (!WEMStructures::FitsFace(*Positive, NormalAxis, 1, Platform.Up, SpanFirst, SpanSecond, bTurned))
		{
			return FString::Printf(TEXT("%s does not fit the positive face"), *Positive->GetName());
		}

		if (!WEMStructures::FitsFace(*Negative, NormalAxis, -1, Platform.Up, SpanFirst, SpanSecond, bTurned))
		{
			return FString::Printf(TEXT("%s does not fit the negative face"), *Negative->GetName());
		}

		if (!WEMStructures::PairsBackToBack(*Positive, *Negative, NormalAxis, 1, Platform.Up))
		{
			return FString::Printf(TEXT("%s and %s do not pair back to back"), *Positive->GetName(), *Negative->GetName());
		}

		return FString();
	}

	/** A one-sided structure of one type, standing for one side of an enclosure. */
	UPlatformStructureData* MakeSided(const TCHAR* Name, const EStructureType Type, const EStructureSide Side, TArray<UObject*>& OutRooted)
	{
		UPlatformStructureData* Structure = MakeStructure(Name, 16, 16, Type, false);
		Structure->Side = Side;
		OutRooted.Add(Structure);
		return Structure;
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
	UPlatformStructureData* Roof = MakeStructure(TEXT("TestRoleRoof"), 16, 16, EStructureType::Roof, false);
	UPlatformStructureData* Ceiling = MakeStructure(TEXT("TestRoleCeiling"), 16, 16, EStructureType::Ceiling, false);
	UPlatformStructureData* Wall = MakeStructure(TEXT("TestRoleWall"), 16, 16, EStructureType::Wall, false);
	Rooted.Append({ Ground, Roof, Ceiling, Wall });

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, TArray<TObjectPtr<UPlatformStructureData>>{ Ground, Roof, Ceiling, Wall });
	UPlatformTileData* Square = Manager->Tiles[0];

	// Filled again and again, a floor is a roof or a ground over a ceiling, or a ground over a ground,
	// the roof on the face looking along its Up: upside down, on its underside.
	auto TallyFloors = [&](const EGridFace Up, int32& OutRoofs, int32& OutGroundsOverCeilings, int32& OutGrounds)
	{
		OutRoofs = 0;
		OutGroundsOverCeilings = 0;
		OutGrounds = 0;

		for (int32 Fill = 0; Fill < 64; ++Fill)
		{
			FGridPlatform Floor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr, Up);

			if (!FRoomManagerTestAccess::DrawFill(*Manager, Floor))
			{
				AddError(TEXT("A floor is not filled"));
				return;
			}

			const bool bUpPositive = WEMGrid::FaceSign(Up) > 0;
			const UPlatformStructureData* AlongUp = bUpPositive ? Floor.PositiveStructure.Get() : Floor.NegativeStructure.Get();
			const UPlatformStructureData* AgainstUp = bUpPositive ? Floor.NegativeStructure.Get() : Floor.PositiveStructure.Get();

			OutRoofs += AlongUp == Roof && AgainstUp == Ceiling ? 1 : 0;
			OutGroundsOverCeilings += AlongUp == Ground && AgainstUp == Ceiling ? 1 : 0;
			OutGrounds += AlongUp == Ground && AgainstUp == Ground ? 1 : 0;
		}
	};

	int32 Roofs, GroundsOverCeilings, Grounds;
	TallyFloors(EGridFace::NegZ, Roofs, GroundsOverCeilings, Grounds);
	AddInfo(FString::Printf(TEXT("Upside down: %d of 64 roof over ceiling, %d ground over ceiling, %d ground over ground."), Roofs, GroundsOverCeilings, Grounds));
	TestEqual(TEXT("Upside down, every floor is a roof or a ground over a ceiling, or a ground over a ground, the top on its underside"),
		Roofs + GroundsOverCeilings + Grounds, 64);
	TestTrue(TEXT("All three turn up"), Roofs > 0 && GroundsOverCeilings > 0 && Grounds > 0);

	TallyFloors(EGridFace::PosZ, Roofs, GroundsOverCeilings, Grounds);
	TestEqual(TEXT("The right way up, the top is on its top"), Roofs + GroundsOverCeilings + Grounds, 64);

	// A floor in the world whose Up lies in its plane is a wall, both faces.
	FGridPlatform LyingWall = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr, EGridFace::NegX);
	TestTrue(TEXT("A wall lying flat in the world is filled"), FRoomManagerTestAccess::DrawFill(*Manager, LyingWall));
	TestTrue(TEXT("With wall on both faces"), LyingWall.PositiveStructure == Wall && LyingWall.NegativeStructure == Wall);

	// And a wall in the world whose Up runs along its normal is a floor: ground along Up.
	FGridPlatform StandingFloor = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr, EGridFace::NegX);
	TestTrue(TEXT("A floor standing up in the world is filled"), FRoomManagerTestAccess::DrawFill(*Manager, StandingFloor));
	TestTrue(TEXT("Its face looking along -X takes the roof or the ground"),
		StandingFloor.NegativeStructure.Get() == Roof || StandingFloor.NegativeStructure.Get() == Ground);
	TestTrue(TEXT("And its face looking along +X what pairs with that"), CheckFill(StandingFloor).IsEmpty());

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleHangTest, "WEM.Structures.Roles.HangFromCeilings", WEMRoomManagerTests::TestFlags)

bool FStructureRoleHangTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Slab = MakeSlab(16, 16, true, Rooted);

	const FGridPlatform Floor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab);
	const FGridPlatform Below = MakeFilled(FIntVector(32, 16, 16), EGridAxis::X, Square, nullptr);
	const FGridPlatform Above = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr);
	FRoomManagerTestAccess::Rebuild(*Manager, { Floor });

	TestEqual(TEXT("Off, a wall below a floor stands the right way up"), FRoomManagerTestAccess::ChooseUp(*Manager, Below), EGridFace::PosZ);

	Manager->bHangFromCeilings = true;
	TestEqual(TEXT("On, it hangs from the floor's ceiling, upside down"), FRoomManagerTestAccess::ChooseUp(*Manager, Below), EGridFace::NegZ);
	TestEqual(TEXT("A wall standing on the floor's ground does not"), FRoomManagerTestAccess::ChooseUp(*Manager, Above), EGridFace::PosZ);

	// An upside-down floor's ceiling is its top: a wall standing on it in the world hangs from it, and so
	// comes out the right way up.
	const FGridPlatform UpsideDown = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab, EGridFace::NegZ);
	FRoomManagerTestAccess::Rebuild(*Manager, { UpsideDown });
	TestEqual(TEXT("Above an upside-down floor, a wall hangs the right way up"), FRoomManagerTestAccess::ChooseUp(*Manager, Above), EGridFace::PosZ);
	TestEqual(TEXT("Below it, one stands on its ground, upside down like it"), FRoomManagerTestAccess::ChooseUp(*Manager, Below), EGridFace::NegZ);

	// A ceiling that holds nothing has nothing to hang from.
	UPlatformStructureData* Roof = MakeStructure(TEXT("TestRoleRoof"), 16, 16, EStructureType::Roof, false);
	UPlatformStructureData* BareCeiling = MakeStructure(TEXT("TestRoleBareCeiling"), 16, 16, EStructureType::Ceiling, false);
	BareCeiling->bHoldsItems = false;
	Rooted.Append({ Roof, BareCeiling });

	FGridPlatform BareFloor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr);
	BareFloor.PositiveStructure = Roof;
	BareFloor.NegativeStructure = BareCeiling;
	FRoomManagerTestAccess::Rebuild(*Manager, { BareFloor });
	TestEqual(TEXT("Below a ceiling that holds nothing, a wall stands the right way up"), FRoomManagerTestAccess::ChooseUp(*Manager, Below), EGridFace::PosZ);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleFlipTest, "WEM.Structures.Roles.Flip", WEMRoomManagerTests::TestFlags)

bool FStructureRoleFlipTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Slab = MakeSlab(16, 16, true, Rooted);

	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab) });

	const FGridPlatform Fold = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr);
	const FGridPlatform Along = MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, nullptr);

	Manager->bFlipOnFold = true;
	Manager->FlipChance = 1.0f;
	TestEqual(TEXT("Sure to flip, a wall folding off the floor is upside down"), FRoomManagerTestAccess::ChooseUp(*Manager, Fold), EGridFace::NegZ);
	TestEqual(TEXT("A floor carrying it on in its plane is not flipped"), FRoomManagerTestAccess::ChooseUp(*Manager, Along), EGridFace::PosZ);

	Manager->FlipChance = 0.0f;
	TestEqual(TEXT("Never flipping, it is not"), FRoomManagerTestAccess::ChooseUp(*Manager, Fold), EGridFace::PosZ);

	// At a chance between, some folds flip and some do not.
	Manager->FlipChance = 0.5f;
	int32 Flipped = 0;

	for (int32 Draw = 0; Draw < 200; ++Draw)
	{
		Flipped += FRoomManagerTestAccess::ChooseUp(*Manager, Fold) == EGridFace::NegZ ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("At 0.5, %d of 200 folds flipped."), Flipped));
	TestTrue(TEXT("At 0.5, about half the folds flip"), Flipped > 70 && Flipped < 130);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleQuarterTurnTest, "WEM.Structures.Roles.QuarterTurn", WEMRoomManagerTests::TestFlags)

bool FStructureRoleQuarterTurnTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Slab = MakeSlab(16, 16, true, Rooted);

	// A wall facing X, holding items on both faces.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, Slab) });

	const FGridPlatform Along = MakeFilled(FIntVector(32, 32, 32), EGridAxis::X, Square, nullptr);
	const FGridPlatform Fold = MakeFilled(FIntVector(32, 16, 48), EGridAxis::Z, Square, nullptr);

	Manager->bQuarterTurn = true;
	Manager->QuarterTurnChance = 1.0f;

	int32 TowardPositive = 0;
	int32 TowardNegative = 0;

	for (int32 Draw = 0; Draw < 64; ++Draw)
	{
		const EGridFace Up = FRoomManagerTestAccess::ChooseUp(*Manager, Along);
		TowardPositive += Up == EGridFace::PosX ? 1 : 0;
		TowardNegative += Up == EGridFace::NegX ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("Carrying the wall on, %d of 64 turned toward +X and %d toward -X."), TowardPositive, TowardNegative));
	TestEqual(TEXT("Sure to turn, carrying the wall on always makes a floor looking out of it"), TowardPositive + TowardNegative, 64);
	TestTrue(TEXT("Out of either face, since both hold items"), TowardPositive > 0 && TowardNegative > 0);
	TestEqual(TEXT("A floor folding off the wall's top is not turned"), FRoomManagerTestAccess::ChooseUp(*Manager, Fold), EGridFace::PosZ);

	// Only a face that holds items is looked out of.
	UPlatformStructureData* Holds = nullptr;
	UPlatformStructureData* HoldsNothing = nullptr;
	MakeHalfHoldingWall(Holds, HoldsNothing, Rooted);

	FGridPlatform HalfWall = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr);
	HalfWall.PositiveStructure = Holds;
	HalfWall.NegativeStructure = HoldsNothing;
	FRoomManagerTestAccess::Rebuild(*Manager, { HalfWall });

	bool bAlwaysPositive = true;

	for (int32 Draw = 0; Draw < 16; ++Draw)
	{
		bAlwaysPositive = bAlwaysPositive && FRoomManagerTestAccess::ChooseUp(*Manager, Along) == EGridFace::PosX;
	}

	TestTrue(TEXT("Off a wall holding items on its +X face only, the floor's ground looks along +X"), bAlwaysPositive);

	// A wall holding nothing either side is never turned.
	FGridPlatform BareWall = HalfWall;
	BareWall.PositiveStructure = HoldsNothing;
	FRoomManagerTestAccess::Rebuild(*Manager, { BareWall });
	TestEqual(TEXT("Off a wall holding nothing, it stays a wall"), FRoomManagerTestAccess::ChooseUp(*Manager, Along), EGridFace::PosZ);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleWallsStandTest, "WEM.Structures.Roles.WallsStand", WEMRoomManagerTests::TestFlags)

bool FStructureRoleWallsStandTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformStructureData* Slab = MakeSlab(16, 16, true, Rooted);

	const FGridPlatform Below = MakeFilled(FIntVector(32, 16, 16), EGridAxis::X, Square, nullptr);
	const FGridPlatform Above = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr);
	const FGridPlatform Along = MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, nullptr);

	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab) });
	TestEqual(TEXT("A wall about to hang below a ground is drawn again with the rule's chance"),
		FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Below), Manager->WallsStandChance);
	TestEqual(TEXT("One standing on it is not"), FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Above), 0.0f);
	TestEqual(TEXT("Nor is a floor carrying it on"), FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Along), 0.0f);

	Manager->bWallsStandOnGrounds = false;
	TestEqual(TEXT("With the rule off, hanging is not redrawn"), FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Below), 0.0f);
	Manager->bWallsStandOnGrounds = true;

	// Upside down, the ground looks down: standing on it is hanging in the world.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, Slab, EGridFace::NegZ) });
	TestEqual(TEXT("Off an upside-down ground, the wall above it is the one redrawn"),
		FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Above), Manager->WallsStandChance);
	TestEqual(TEXT("And the one below it stands"), FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Below), 0.0f);

	// A ground that holds nothing asks nothing.
	UPlatformStructureData* BareGround = MakeStructure(TEXT("TestRoleBareGround"), 16, 16, EStructureType::Ground, false);
	UPlatformStructureData* UnderGround = MakeStructure(TEXT("TestRoleUnderGround"), 16, 16, EStructureType::Ground, false);
	BareGround->bHoldsItems = false;
	Rooted.Append({ BareGround, UnderGround });

	FGridPlatform BareFloor = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr);
	BareFloor.PositiveStructure = BareGround;
	BareFloor.NegativeStructure = UnderGround;
	FRoomManagerTestAccess::Rebuild(*Manager, { BareFloor });
	TestEqual(TEXT("Below a ground that holds nothing, hanging is not redrawn"), FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Below), 0.0f);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleFloorsCloseRoomsTest, "WEM.Structures.Roles.FloorsCloseRooms", WEMRoomManagerTests::TestFlags)

bool FStructureRoleFloorsCloseRoomsTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];
	UPlatformTileData* Small = Manager->Tiles[1];
	UPlatformStructureData* Slab = MakeSlab(16, 16, true, Rooted);
	UPlatformStructureData* SmallSlab = MakeSlab(8, 8, true, Rooted);
	const float Chance = Manager->FloorsCloseRoomsChance;

	// A wall facing X at X 32, Y 16 to 32 and Z 32 to 48, holding items on both faces. Floors fold off it
	// at its foot (Z 32) or its top (Z 48), toward +X or toward -X.
	const FGridPlatform Wall = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, Slab);
	const FGridPlatform FootPlus = MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Square, nullptr);
	const FGridPlatform TopPlus = MakeFilled(FIntVector(32, 16, 48), EGridAxis::Z, Square, nullptr);
	const FGridPlatform FootMinus = MakeFilled(FIntVector(16, 16, 32), EGridAxis::Z, Square, nullptr);
	const FGridPlatform TopMinus = MakeFilled(FIntVector(16, 16, 48), EGridAxis::Z, Square, nullptr);
	const FGridPlatform Corner = MakeFilled(FIntVector(32, 32, 32), EGridAxis::Y, Square, nullptr);

	auto RedrawChance = [Manager](const FGridPlatform& Candidate)
	{
		return FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Candidate);
	};

	// With neither end closed, a ground at the foot is wanted on either side.
	FRoomManagerTestAccess::Rebuild(*Manager, { Wall });
	TestEqual(TEXT("Open both ends: a floor at the foot is kept"), RedrawChance(FootPlus), 0.0f);
	TestEqual(TEXT("And one at the top is drawn again"), RedrawChance(TopPlus), Chance);
	TestEqual(TEXT("On the other side too"), RedrawChance(TopMinus), Chance);
	TestEqual(TEXT("A wall turning its corner asks nothing"), RedrawChance(Corner), 0.0f);

	// A ceiling at the top on +X: the room there wants its ground. The -X side is still open both ends.
	FRoomManagerTestAccess::Rebuild(*Manager, { Wall, MakeFilled(FIntVector(32, 16, 48), EGridAxis::Z, Square, Slab) });
	TestEqual(TEXT("Under a ceiling, a ground at the foot is kept"), RedrawChance(FootPlus), 0.0f);
	TestEqual(TEXT("Across the wall, the top is still drawn again"), RedrawChance(TopMinus), Chance);

	// Part of a ground at the foot on +X: the room there wants its ceiling, so the rest of the foot is
	// drawn again and the top is kept.
	const FGridPlatform PartGround = MakeFilled(FIntVector(32, 16, 32), EGridAxis::Z, Small, SmallSlab);
	const FGridPlatform RestOfFoot = MakeFilled(FIntVector(32, 24, 32), EGridAxis::Z, Small, nullptr);
	FRoomManagerTestAccess::Rebuild(*Manager, { Wall, PartGround });
	TestEqual(TEXT("With a ground at its foot, more of the foot is drawn again"), RedrawChance(RestOfFoot), Chance);
	TestEqual(TEXT("And a ceiling at the top is kept"), RedrawChance(TopPlus), 0.0f);

	// Closed at both ends, the room wants neither.
	FRoomManagerTestAccess::Rebuild(*Manager, { Wall, PartGround, MakeFilled(FIntVector(32, 16, 48), EGridAxis::Z, Square, Slab) });
	TestEqual(TEXT("Closed both ends, nothing is drawn again"), RedrawChance(RestOfFoot), 0.0f);

	// A face that holds nothing faces no room.
	UPlatformStructureData* Holds = nullptr;
	UPlatformStructureData* HoldsNothing = nullptr;
	MakeHalfHoldingWall(Holds, HoldsNothing, Rooted);

	FGridPlatform HalfWall = Wall;
	HalfWall.PositiveStructure = HoldsNothing;
	HalfWall.NegativeStructure = Holds;
	FRoomManagerTestAccess::Rebuild(*Manager, { HalfWall });
	TestEqual(TEXT("Off a face holding nothing, the top is kept"), RedrawChance(TopPlus), 0.0f);
	TestEqual(TEXT("Off the face that holds items, it is drawn again"), RedrawChance(TopMinus), Chance);

	// Upside down, the wall's foot is its highest edge in the world.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, Slab, EGridFace::NegZ) });
	TestEqual(TEXT("Upside down, a floor at Z 48, its foot, is kept"), RedrawChance(TopPlus), 0.0f);
	TestEqual(TEXT("And one at Z 32, its top, is drawn again"), RedrawChance(FootPlus), Chance);

	Manager->bFloorsCloseRooms = false;
	TestEqual(TEXT("With the rule off, nothing is drawn again"), RedrawChance(FootPlus), 0.0f);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleRuleGrowthTest, "WEM.Structures.Roles.RuleGrowth", WEMRoomManagerTests::TestFlags)

bool FStructureRoleRuleGrowthTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	// The same seed grown with the rules and without. Every placement is then asked, against the
	// platforms placed before it, whether it went against either rule: with the rules on, far fewer do.
	struct FTally
	{
		int32 Placements = 0;
		int32 AgainstWallsStand = 0;
		int32 AgainstFloorsCloseRooms = 0;
	};

	auto Grow = [](const bool bRules)
	{
		FScopedTestWorld TestWorld;
		TArray<UObject*> Rooted;
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
		Manager->bWallsStandOnGrounds = bRules;
		Manager->bFloorsCloseRooms = bRules;

		for (int32 Beat = 0; Beat < 300; ++Beat)
		{
			Manager->PlaceNextPlatform();
		}

		const TArray<FGridPlatform> Grown = Manager->Platforms;

		FTally Tally;
		Tally.Placements = Grown.Num();

		// Replayed a placement at a time, each asked against those before it, one rule at a time.
		TArray<FGridPlatform> Before;

		for (const FGridPlatform& Platform : Grown)
		{
			if (!Before.IsEmpty())
			{
				FRoomManagerTestAccess::Rebuild(*Manager, Before);

				Manager->bWallsStandOnGrounds = true;
				Manager->bFloorsCloseRooms = false;
				Tally.AgainstWallsStand += FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Platform) > 0.0f ? 1 : 0;

				Manager->bWallsStandOnGrounds = false;
				Manager->bFloorsCloseRooms = true;
				Tally.AgainstFloorsCloseRooms += FRoomManagerTestAccess::GetRuleRedrawChance(*Manager, Platform) > 0.0f ? 1 : 0;
			}

			Before.Add(Platform);
		}

		ReleaseRooted(Rooted);
		return Tally;
	};

	const FTally Without = Grow(false);
	const FTally With = Grow(true);

	AddInfo(FString::Printf(TEXT("Without the rules: of %d placements, %d hung below a ground and %d went to the end of a wall its room did not need."),
		Without.Placements, Without.AgainstWallsStand, Without.AgainstFloorsCloseRooms));
	AddInfo(FString::Printf(TEXT("With the rules: of %d placements, %d hung below a ground and %d went to the end of a wall its room did not need."),
		With.Placements, With.AgainstWallsStand, With.AgainstFloorsCloseRooms));

	TestTrue(TEXT("Both grow"), Without.Placements > 100 && With.Placements > 100);
	TestTrue(TEXT("Without the rules, some walls hang below grounds"), Without.AgainstWallsStand > 10);
	TestTrue(TEXT("Without the rules, some floors go where their room did not need them"), Without.AgainstFloorsCloseRooms > 10);
	TestTrue(TEXT("With them, at most two thirds as many walls hang"), 3 * With.AgainstWallsStand <= 2 * Without.AgainstWallsStand);
	TestTrue(TEXT("And at most two thirds as many floors go astray"), 3 * With.AgainstFloorsCloseRooms <= 2 * Without.AgainstFloorsCloseRooms);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleInteriorBacksExteriorTest, "WEM.Structures.Roles.InteriorBacksExterior", WEMRoomManagerTests::TestFlags)

bool FStructureRoleInteriorBacksExteriorTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// One-sided walls standing for an interior and two exteriors, equally weighted. The categories say
	// nothing: a category named for an interior does not make one.
	UStructureCategory* Office = NewObject<UStructureCategory>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStructureCategory::StaticClass(), TEXT("TestCategory_InteriorOffice")));
	Office->AddToRoot();
	Rooted.Add(Office);

	UPlatformStructureData* InsideWall = MakeSided(TEXT("TestInsideWall"), EStructureType::Wall, EStructureSide::Interior, Rooted);
	UPlatformStructureData* OutsideWall = MakeSided(TEXT("TestOutsideWall"), EStructureType::Wall, EStructureSide::Exterior, Rooted);
	UPlatformStructureData* OfficeWall = MakeSided(TEXT("TestOfficeWall"), EStructureType::Wall, EStructureSide::Exterior, Rooted);
	UPlatformStructureData* EitherWall = MakeSided(TEXT("TestEitherWall"), EStructureType::Wall, EStructureSide::Both, Rooted);
	OfficeWall->Categories = { Office };

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted,
		TArray<TObjectPtr<UPlatformStructureData>>{ InsideWall, OutsideWall, OfficeWall, EitherWall, MakeSlab(16, 16, true, Rooted) });
	UPlatformTileData* Square = Manager->Tiles[0];

	// Filled again and again, a wall with nothing beside it: tally what goes behind each kind of face.
	struct FTally
	{
		int32 InsideFaces = 0;
		int32 InsideBehindInside = 0;
		int32 OutsideBehindInside = 0;
		int32 InsideBehindOutside = 0;
		int32 OutsideBehindOutside = 0;
		int32 EitherBehindInside = 0;
		int32 InsideBehindEither = 0;
	};

	auto Tally = [&]()
	{
		FTally Result;

		for (int32 Fill = 0; Fill < 300; ++Fill)
		{
			FGridPlatform Wall = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr);

			if (!FRoomManagerTestAccess::DrawFill(*Manager, Wall) || Wall.PositiveStructure->bTwoSided)
			{
				continue;
			}

			for (const TPair<UPlatformStructureData*, UPlatformStructureData*>& Pair :
				{ MakeTuple(Wall.PositiveStructure.Get(), Wall.NegativeStructure.Get()), MakeTuple(Wall.NegativeStructure.Get(), Wall.PositiveStructure.Get()) })
			{
				const bool bInside = Pair.Key->Side == EStructureSide::Interior;
				const bool bBehindInside = Pair.Value->Side == EStructureSide::Interior;

				Result.InsideFaces += bInside ? 1 : 0;
				Result.InsideBehindInside += bInside && bBehindInside ? 1 : 0;
				Result.OutsideBehindInside += bInside && !bBehindInside ? 1 : 0;
				Result.InsideBehindOutside += !bInside && bBehindInside ? 1 : 0;
				Result.OutsideBehindOutside += !bInside && !bBehindInside ? 1 : 0;
				Result.EitherBehindInside += bInside && Pair.Value == EitherWall ? 1 : 0;
				Result.InsideBehindEither += Pair.Key == EitherWall && bBehindInside ? 1 : 0;
			}
		}

		return Result;
	};

	Manager->InteriorBacksExteriorChance = 1.0f;
	FTally Always = Tally();
	AddInfo(FString::Printf(TEXT("Always: %d interior faces, %d with an interior behind, %d with an exterior behind; behind exteriors, %d interiors and %d exteriors."),
		Always.InsideFaces, Always.InsideBehindInside, Always.OutsideBehindInside, Always.InsideBehindOutside, Always.OutsideBehindOutside));
	TestTrue(TEXT("Interiors are drawn"), Always.InsideFaces > 20);
	TestEqual(TEXT("Sure to, an interior never has an interior behind it"), Always.InsideBehindInside, 0);
	TestTrue(TEXT("It has an exterior"), Always.OutsideBehindInside > 0);
	TestTrue(TEXT("Exteriors meet back to back, since nothing binds them"), Always.OutsideBehindOutside > 0);
	TestTrue(TEXT("A wall of both sides may go behind an interior"), Always.EitherBehindInside > 0);
	TestTrue(TEXT("And binds nothing behind itself: interiors follow it too"), Always.InsideBehindEither > 0);

	// Off, the back is drawn like any other face, and interiors turn up back to back.
	Manager->bInteriorBacksExterior = false;
	FTally Off = Tally();
	AddInfo(FString::Printf(TEXT("Off: %d interior faces, %d with an interior behind, %d with an exterior behind."),
		Off.InsideFaces, Off.InsideBehindInside, Off.OutsideBehindInside));
	TestTrue(TEXT("Off, interiors turn up back to back"), Off.InsideBehindInside > 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleWallsFaceInteriorGroundsTest, "WEM.Structures.Roles.WallsFaceInteriorGrounds", WEMRoomManagerTests::TestFlags)

bool FStructureRoleWallsFaceInteriorGroundsTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	UPlatformStructureData* InsideGround = MakeSided(TEXT("TestInsideGround"), EStructureType::Ground, EStructureSide::Interior, Rooted);
	UPlatformStructureData* OutsideGround = MakeSided(TEXT("TestOutsideGround"), EStructureType::Ground, EStructureSide::Exterior, Rooted);
	UPlatformStructureData* EitherGround = MakeSided(TEXT("TestEitherGround"), EStructureType::Ground, EStructureSide::Both, Rooted);
	UPlatformStructureData* InsideWall = MakeSided(TEXT("TestInsideWall"), EStructureType::Wall, EStructureSide::Interior, Rooted);
	UPlatformStructureData* OutsideWall = MakeSided(TEXT("TestOutsideWall"), EStructureType::Wall, EStructureSide::Exterior, Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted,
		TArray<TObjectPtr<UPlatformStructureData>>{ InsideGround, OutsideGround, InsideWall, OutsideWall });
	UPlatformTileData* Square = Manager->Tiles[0];

	auto MakeFloor = [&](const FIntVector& MinNode, UPlatformStructureData* AlongUp, UPlatformStructureData* AgainstUp, const EGridFace Up)
	{
		FGridPlatform Floor = MakeFilled(MinNode, EGridAxis::Z, Square, nullptr, Up);
		Floor.PositiveStructure = WEMGrid::FaceSign(Up) > 0 ? AlongUp : AgainstUp;
		Floor.NegativeStructure = WEMGrid::FaceSign(Up) > 0 ? AgainstUp : AlongUp;
		return Floor;
	};

	auto Faces = [Manager](const FGridPlatform& Wall)
	{
		return static_cast<int32>(FRoomManagerTestAccess::GetFacesOverInteriorGround(*Manager, Wall));
	};

	// A floor X 16 to 32 at Z 32, its walked face interior. A wall standing on its east edge looks over
	// it with its -X face; one hanging below it, or folding off its far side, does not.
	const FGridPlatform Above = MakeFilled(FIntVector(32, 16, 32), EGridAxis::X, Square, nullptr);
	const FGridPlatform Below = MakeFilled(FIntVector(32, 16, 16), EGridAxis::X, Square, nullptr);
	const FGridPlatform Side = MakeFilled(FIntVector(16, 32, 32), EGridAxis::Y, Square, nullptr);

	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFloor(FIntVector(16, 16, 32), InsideGround, OutsideGround, EGridFace::PosZ) });
	TestEqual(TEXT("A wall standing on an interior ground looks over it with the face toward it"), Faces(Above), 2);
	TestEqual(TEXT("One hanging below it does not"), Faces(Below), 0);
	TestEqual(TEXT("A wall along its north edge looks over it with its -Y face"), Faces(Side), 2);

	// Upside down, its walked face looks down: the wall below stands on it.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFloor(FIntVector(16, 16, 32), InsideGround, OutsideGround, EGridFace::NegZ) });
	TestEqual(TEXT("Below an upside-down interior ground, a wall stands on it"), Faces(Below), 2);
	TestEqual(TEXT("And above it, none does"), Faces(Above), 0);

	// An exterior ground asks nothing, nor does the interior underneath it.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFloor(FIntVector(16, 16, 32), OutsideGround, InsideGround, EGridFace::PosZ) });
	TestEqual(TEXT("On an exterior ground, nothing"), Faces(Above), 0);
	TestEqual(TEXT("Below it, by an interior ground facing down that is not its walked face, nothing"), Faces(Below), 0);

	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFloor(FIntVector(16, 16, 32), EitherGround, OutsideGround, EGridFace::PosZ) });
	TestEqual(TEXT("On a ground of both sides, nothing"), Faces(Above), 0);

	// Between two rooms, both faces.
	FRoomManagerTestAccess::Rebuild(*Manager, {
		MakeFloor(FIntVector(16, 16, 32), InsideGround, OutsideGround, EGridFace::PosZ),
		MakeFloor(FIntVector(32, 16, 32), InsideGround, OutsideGround, EGridFace::PosZ) });
	TestEqual(TEXT("Between two interior grounds, both faces"), Faces(Above), 3);

	// Drawn, with the rule sure to apply: inside over the room, and behind it, also sure, outside.
	Manager->WallsFaceInteriorGroundsChance = 1.0f;
	Manager->InteriorBacksExteriorChance = 1.0f;

	auto Tally = [&](int32& OutInsideOverRoom, int32& OutOutsideBehind, int32& OutFills)
	{
		OutInsideOverRoom = 0;
		OutOutsideBehind = 0;
		OutFills = 0;

		for (int32 Fill = 0; Fill < 100; ++Fill)
		{
			FGridPlatform Wall = Above;

			if (FRoomManagerTestAccess::DrawFill(*Manager, Wall))
			{
				++OutFills;
				OutInsideOverRoom += Wall.NegativeStructure == InsideWall ? 1 : 0;
				OutOutsideBehind += Wall.PositiveStructure == OutsideWall ? 1 : 0;
			}
		}
	};

	int32 InsideOverRoom, OutsideBehind, Fills;
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFloor(FIntVector(16, 16, 32), InsideGround, OutsideGround, EGridFace::PosZ) });
	Tally(InsideOverRoom, OutsideBehind, Fills);
	TestEqual(TEXT("Every wall on the room's edge is filled"), Fills, 100);
	TestEqual(TEXT("Sure to, its face over the room is always an interior"), InsideOverRoom, 100);
	TestEqual(TEXT("And its back always an exterior"), OutsideBehind, 100);

	// Between two rooms both faces are inside, the rule outranking what goes behind an interior.
	FRoomManagerTestAccess::Rebuild(*Manager, {
		MakeFloor(FIntVector(16, 16, 32), InsideGround, OutsideGround, EGridFace::PosZ),
		MakeFloor(FIntVector(32, 16, 32), InsideGround, OutsideGround, EGridFace::PosZ) });
	int32 BothInside = 0;

	for (int32 Fill = 0; Fill < 100; ++Fill)
	{
		FGridPlatform Wall = Above;
		BothInside += FRoomManagerTestAccess::DrawFill(*Manager, Wall) && Wall.PositiveStructure == InsideWall && Wall.NegativeStructure == InsideWall ? 1 : 0;
	}

	TestEqual(TEXT("Between two rooms, a wall is inside on both faces"), BothInside, 100);

	// Off, the face over the room is drawn like any other.
	Manager->bWallsFaceInteriorGrounds = false;
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeFloor(FIntVector(16, 16, 32), InsideGround, OutsideGround, EGridFace::PosZ) });
	Tally(InsideOverRoom, OutsideBehind, Fills);
	AddInfo(FString::Printf(TEXT("Off: of %d walls, %d inside over the room."), Fills, InsideOverRoom));
	TestTrue(TEXT("Off, some walls are outside over the room"), InsideOverRoom < Fills && InsideOverRoom > 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureRoleTurnedGrowthTest, "WEM.Structures.Roles.TurnedGrowth", WEMRoomManagerTests::TestFlags)

bool FStructureRoleTurnedGrowthTest::RunTest(const FString& Parameters)
{
	using namespace StructureRoleTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Per size, a slab and a one-sided ground, roof, ceiling and wall, so the fill has to follow each
	// platform's Up to be right.
	TArray<TObjectPtr<UPlatformStructureData>> Structures;

	for (const FIntPoint Size : { FIntPoint(16, 16), FIntPoint(8, 8), FIntPoint(8, 16) })
	{
		UPlatformStructureData* Ground = MakeStructure(TEXT("TestRoleGround"), Size.X, Size.Y, EStructureType::Ground, false);
		UPlatformStructureData* Roof = MakeStructure(TEXT("TestRoleRoof"), Size.X, Size.Y, EStructureType::Roof, false);
		UPlatformStructureData* Ceiling = MakeStructure(TEXT("TestRoleCeiling"), Size.X, Size.Y, EStructureType::Ceiling, false);
		UPlatformStructureData* Wall = MakeStructure(TEXT("TestRoleWall"), Size.X, Size.Y, EStructureType::Wall, false, /*bMayLieSideways=*/true);
		Rooted.Append({ Ground, Roof, Ceiling, Wall });
		Structures.Append({ Ground, Roof, Ceiling, Wall, MakeSlab(Size.X, Size.Y, true, Rooted) });
	}

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
	Manager->bFlipOnFold = true;
	Manager->FlipChance = 0.3f;
	Manager->bHangFromCeilings = true;
	Manager->bQuarterTurn = true;
	Manager->QuarterTurnChance = 0.3f;

	// Every placement checked against a full rebuild from the list, Ups and all.
	IConsoleVariable* VerifyEvery = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.RoomManager.VerifyEvery"));

	if (!TestNotNull(TEXT("The VerifyEvery console variable exists"), VerifyEvery))
	{
		ReleaseRooted(Rooted);
		return false;
	}

	const int32 Previous = VerifyEvery->GetInt();
	VerifyEvery->Set(1, ECVF_SetByCode);

	for (int32 Beat = 0; Beat < 400 && Manager->PlaceNextPlatform(); ++Beat)
	{
	}

	VerifyEvery->Set(Previous, ECVF_SetByCode);

	TestEqual(TEXT("No placement drifts from a full rebuild"), FRoomManagerTestAccess::GetFailedVerifications(*Manager), 0);
	TestTrue(TEXT("Growth takes off"), Manager->GetPlatformCount() > 100);

	// Every fill fits its platform as it is, its Up included, and the turns have turned the world round.
	int32 NotWorldUp = 0;
	int32 UpsideDown = 0;
	int32 LyingWalls = 0;
	int32 StandingFloors = 0;

	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		const FString Problem = CheckFill(Platform);

		if (!Problem.IsEmpty())
		{
			AddError(FString::Printf(TEXT("The platform at %s facing %d, up %s, has no fill that fits: %s"),
				*Platform.MinNode.ToString(), static_cast<int32>(Platform.Normal), *DescribeUp(Platform.Up), *Problem));
			break;
		}

		NotWorldUp += Platform.Up != EGridFace::PosZ ? 1 : 0;
		UpsideDown += Platform.Up == EGridFace::NegZ ? 1 : 0;
		LyingWalls += Platform.Normal == EGridAxis::Z && Platform.IsWall() ? 1 : 0;
		StandingFloors += Platform.Normal != EGridAxis::Z && !Platform.IsWall() ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("%d platform(s): %d not up the world's way, %d upside down, %d walls lying flat, %d floors standing up."),
		Manager->Platforms.Num(), NotWorldUp, UpsideDown, LyingWalls, StandingFloors));
	TestTrue(TEXT("Some platforms are upside down"), UpsideDown > 0);
	TestTrue(TEXT("Some floors stand up in the world"), StandingFloors > 0);

	ReleaseRooted(Rooted);
	return true;
}

#endif
