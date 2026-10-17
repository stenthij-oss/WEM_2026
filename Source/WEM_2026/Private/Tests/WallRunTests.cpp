// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace WallRunTestsPrivate
{
	using namespace WEMRoomManagerTests;

	FGridPlatform MakeSegment(const FIntVector& MinNode, const EGridAxis Normal, UPlatformTileData* Tile, UPlatformStructureData* Positive, UPlatformStructureData* Negative)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = Normal;
		Platform.Tile = Tile;
		Platform.Up = EGridFace::PosZ;
		Platform.PositiveStructure = Positive;
		Platform.NegativeStructure = Negative;
		return Platform;
	}

	/** A one-sided 16 x 16 structure of one type, standing for one side of an enclosure. */
	UPlatformStructureData* MakeSided(const TCHAR* Name, const EStructureType Type, const EStructureSide Side, TArray<UObject*>& OutRooted)
	{
		UPlatformStructureData* Structure = MakeStructure(Name, 16, 16, Type, false);
		Structure->Side = Side;
		OutRooted.Add(Structure);
		return Structure;
	}

	/** The city's one-sided 16 x 16 structures, inside and out. Only 16 x 16 tiles can be filled. */
	struct FRunKit
	{
		UPlatformStructureData* GroundInterior = nullptr;
		UPlatformStructureData* GroundExterior = nullptr;
		UPlatformStructureData* RoofExterior = nullptr;
		UPlatformStructureData* CeilingInterior = nullptr;
		UPlatformStructureData* CeilingExterior = nullptr;
		UPlatformStructureData* WallInterior = nullptr;
		UPlatformStructureData* WallExterior = nullptr;
		UPlatformStructureData* WallBoth = nullptr;

		explicit FRunKit(TArray<UObject*>& OutRooted)
		{
			GroundInterior = MakeSided(TEXT("TestRunGroundInt"), EStructureType::Ground, EStructureSide::Interior, OutRooted);
			GroundExterior = MakeSided(TEXT("TestRunGroundExt"), EStructureType::Ground, EStructureSide::Exterior, OutRooted);
			RoofExterior = MakeSided(TEXT("TestRunRoofExt"), EStructureType::Roof, EStructureSide::Exterior, OutRooted);
			CeilingInterior = MakeSided(TEXT("TestRunCeilingInt"), EStructureType::Ceiling, EStructureSide::Interior, OutRooted);
			CeilingExterior = MakeSided(TEXT("TestRunCeilingExt"), EStructureType::Ceiling, EStructureSide::Exterior, OutRooted);
			WallInterior = MakeSided(TEXT("TestRunWallInt"), EStructureType::Wall, EStructureSide::Interior, OutRooted);
			WallExterior = MakeSided(TEXT("TestRunWallExt"), EStructureType::Wall, EStructureSide::Exterior, OutRooted);
			WallBoth = MakeSided(TEXT("TestRunWallBoth"), EStructureType::Wall, EStructureSide::Both, OutRooted);
		}

		TArray<TObjectPtr<UPlatformStructureData>> GetAll() const
		{
			return { GroundInterior, GroundExterior, RoofExterior, CeilingInterior, CeilingExterior, WallInterior, WallExterior };
		}
	};

	/** The face of a standing wall that looks over grounds of Side along their outline, or 0 when neither does. */
	int32 GetFaceOverGround(const ARoomManager& Manager, const FGridPlatform& Wall, const EStructureSide Side)
	{
		for (const int32 Sign : { 1, -1 })
		{
			if (FRoomManagerTestAccess::StandsOnGroundOutline(Manager, Wall, Sign, Side))
			{
				return Sign;
			}
		}

		return 0;
	}

	/** How many walls stand on the outline of grounds of Side. */
	int32 CountWallsOnOutline(const ARoomManager& Manager, const EStructureSide Side)
	{
		int32 Count = 0;
		for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(Manager))
		{
			Count += Platform.IsWall() && GetFaceOverGround(Manager, Platform, Side) != 0 ? 1 : 0;
		}

		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallRunRingTest, "WEM.Structures.WallRun.Ring", WEMRoomManagerTests::TestFlags)

bool FWallRunRingTest::RunTest(const FString& Parameters)
{
	using namespace WallRunTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FRunKit Kit(Rooted);

	// Every interior wall on the outline of interior grounds starts a run, and every end carries on.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	UPlatformTileData* Square = Manager->Tiles[0];
	Manager->InteriorWallRunChance = 1.0f;
	Manager->WallRunContinueChance = 1.0f;

	// An interior floor, X and Y 32 to 48 at Z 32, and a wall standing on its north edge, its interior over the floor.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeSegment(FIntVector(32, 32, 32), EGridAxis::Z, Square, Kit.GroundInterior, Kit.CeilingExterior) });

	const FGridPlatform First = MakeSegment(FIntVector(32, 48, 32), EGridAxis::Y, Square, Kit.WallExterior, Kit.WallInterior);
	TestEqual(TEXT("The wall's negative face stands over the floor's outline"), GetFaceOverGround(*Manager, First, EStructureSide::Interior), -1);

	FRoomManagerTestAccess::Place(*Manager, First);
	FRoomManagerTestAccess::QueueWallRun(*Manager, First);
	TestEqual(TEXT("Both its ends wait to carry it on"), FRoomManagerTestAccess::GetWaitingWallRunEnds(*Manager), 2);

	// A wall a beat, turning each corner, until the room is closed.
	for (int32 Beat = 0; Beat < 3; ++Beat)
	{
		const int32 Before = Manager->GetPlatformCount();
		TestTrue(FString::Printf(TEXT("Beat %d places"), Beat), Manager->PlaceNextPlatform());
		TestEqual(FString::Printf(TEXT("Beat %d lays one wall"), Beat), Manager->GetPlatformCount(), Before + 1);

		const FGridPlatform Laid = FRoomManagerTestAccess::GetBuilt(*Manager).Last();
		TestTrue(TEXT("Standing at the run's height, with its Up"), Laid.IsWall() && Laid.MinNode.Z == 32 && Laid.Up == EGridFace::PosZ);

		const int32 FaceSign = GetFaceOverGround(*Manager, Laid, EStructureSide::Interior);
		const UPlatformStructureData* OverGround = FaceSign > 0 ? Laid.PositiveStructure.Get() : Laid.NegativeStructure.Get();
		TestTrue(TEXT("On the floor's outline, an interior over it"), FaceSign != 0 && OverGround && OverGround->StandsFor(EStructureSide::Interior));
	}

	TestEqual(TEXT("A wall on each of the floor's four edges"), CountWallsOnOutline(*Manager, EStructureSide::Interior), 4);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// An L of three floors: the run turns in at the inner corner too, and never stands on a seam between two floors.
	ARoomManager* Ell = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	Ell->InteriorWallRunChance = 1.0f;
	Ell->WallRunContinueChance = 1.0f;

	TArray<FGridPlatform> Floors;
	for (const FIntVector& MinNode : { FIntVector(32, 32, 32), FIntVector(48, 32, 32), FIntVector(32, 48, 32) })
	{
		Floors.Add(MakeSegment(MinNode, EGridAxis::Z, Ell->Tiles[0], Kit.GroundInterior, Kit.CeilingExterior));
	}

	FRoomManagerTestAccess::Rebuild(*Ell, Floors);

	const FGridPlatform South = MakeSegment(FIntVector(32, 32, 32), EGridAxis::Y, Ell->Tiles[0], Kit.WallInterior, Kit.WallExterior);
	TestEqual(TEXT("The L's south wall stands on its outline, its positive face over it"), GetFaceOverGround(*Ell, South, EStructureSide::Interior), 1);

	FRoomManagerTestAccess::Place(*Ell, South);
	FRoomManagerTestAccess::QueueWallRun(*Ell, South);

	for (int32 Beat = 0; Beat < 7; ++Beat)
	{
		Ell->PlaceNextPlatform();
	}

	TestEqual(TEXT("A wall on each of the L's eight edges"), CountWallsOnOutline(*Ell, EStructureSide::Interior), 8);
	TestEqual(TEXT("Round the L, the placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Ell), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallRunStartsTest, "WEM.Structures.WallRun.Starts", WEMRoomManagerTests::TestFlags)

bool FWallRunStartsTest::RunTest(const FString& Parameters)
{
	using namespace WallRunTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FRunKit Kit(Rooted);

	TArray<TObjectPtr<UPlatformStructureData>> Structures = Kit.GetAll();
	Structures.Add(Kit.WallBoth);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
	UPlatformTileData* Square = Manager->Tiles[0];

	// Whether a wall starts a run beside the given floors, every chance certain.
	auto Starts = [&](const TArray<FGridPlatform>& Floors, const FGridPlatform& Wall)
	{
		Manager->InteriorWallRunChance = 1.0f;
		Manager->ExteriorWallRunChance = 1.0f;
		Manager->WallRunContinueChance = 1.0f;

		TArray<FGridPlatform> Platforms = Floors;
		Platforms.Add(Wall);
		FRoomManagerTestAccess::Rebuild(*Manager, Platforms);
		FRoomManagerTestAccess::QueueWallRun(*Manager, Wall);

		const bool bStarted = FRoomManagerTestAccess::GetWaitingWallRunEnds(*Manager) > 0;
		Manager->ClearPlatforms();
		return bStarted;
	};

	const FGridPlatform InsideFloor = MakeSegment(FIntVector(32, 32, 32), EGridAxis::Z, Square, Kit.GroundInterior, Kit.CeilingExterior);
	const FGridPlatform OutsideFloor = MakeSegment(FIntVector(32, 32, 32), EGridAxis::Z, Square, Kit.GroundExterior, Kit.CeilingInterior);
	const FGridPlatform NorthFloor = MakeSegment(FIntVector(32, 48, 32), EGridAxis::Z, Square, Kit.GroundInterior, Kit.CeilingExterior);

	// The north edge, its negative face over the floor.
	auto MakeNorthWall = [&](UPlatformStructureData* OverFloor, UPlatformStructureData* Away, const int32 Z = 32)
	{
		return MakeSegment(FIntVector(32, 48, Z), EGridAxis::Y, Square, Away, OverFloor);
	};

	TestTrue(TEXT("An interior over interior ground starts a run"), Starts({ InsideFloor }, MakeNorthWall(Kit.WallInterior, Kit.WallExterior)));
	TestTrue(TEXT("An exterior over exterior ground starts a run"), Starts({ OutsideFloor }, MakeNorthWall(Kit.WallExterior, Kit.WallInterior)));
	TestFalse(TEXT("An interior over exterior ground starts none"), Starts({ OutsideFloor }, MakeNorthWall(Kit.WallInterior, Kit.WallInterior)));
	TestFalse(TEXT("One of both sides over the ground starts none"), Starts({ InsideFloor }, MakeNorthWall(Kit.WallBoth, Kit.WallBoth)));
	TestFalse(TEXT("A wall on the seam between two floors stands on no outline"), Starts({ InsideFloor, NorthFloor }, MakeNorthWall(Kit.WallInterior, Kit.WallInterior)));
	TestFalse(TEXT("A wall hanging below the floor's edge looks over its ceiling, not its ground"), Starts({ InsideFloor }, MakeNorthWall(Kit.WallInterior, Kit.WallExterior, 16)));

	FRoomManagerTestAccess::Rebuild(*Manager, { InsideFloor });
	TestTrue(TEXT("Wholly on the floor's edge is its outline"),
		FRoomManagerTestAccess::StandsOnGroundOutline(*Manager, MakeSegment(FIntVector(32, 48, 32), EGridAxis::Y, Square, nullptr, nullptr), -1, EStructureSide::Interior));
	TestFalse(TEXT("Half off it is not"),
		FRoomManagerTestAccess::StandsOnGroundOutline(*Manager, MakeSegment(FIntVector(40, 48, 32), EGridAxis::Y, Square, nullptr, nullptr), -1, EStructureSide::Interior));

	// At a chance of zero nothing is drawn, so growth draws as it did before there were runs.
	Manager->InteriorWallRunChance = 0.0f;
	FRoomManagerTestAccess::Rebuild(*Manager, { InsideFloor, MakeNorthWall(Kit.WallInterior, Kit.WallExterior) });
	FRoomManagerTestAccess::QueueWallRun(*Manager, MakeNorthWall(Kit.WallInterior, Kit.WallExterior));
	TestEqual(TEXT("At no chance, none"), FRoomManagerTestAccess::GetWaitingWallRunEnds(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallRunFloorLaidTest, "WEM.Structures.WallRun.FloorLaid", WEMRoomManagerTests::TestFlags)

bool FWallRunFloorLaidTest::RunTest(const FString& Parameters)
{
	using namespace WallRunTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FRunKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	UPlatformTileData* Square = Manager->Tiles[0];
	Manager->InteriorWallRunChance = 1.0f;
	Manager->WallRunContinueChance = 1.0f;

	// A wall standing first, its interior looking south, and nothing under that side yet.
	const FGridPlatform Wall = MakeSegment(FIntVector(32, 48, 32), EGridAxis::Y, Square, Kit.WallExterior, Kit.WallInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Wall });

	// A roof laid beside it gives it no ground to stand on.
	const FGridPlatform Roof = MakeSegment(FIntVector(32, 32, 32), EGridAxis::Z, Square, Kit.RoofExterior, Kit.CeilingInterior);
	FRoomManagerTestAccess::Place(*Manager, Roof);
	FRoomManagerTestAccess::QueueWallRun(*Manager, Roof);
	TestEqual(TEXT("A roof laid beside the wall starts nothing"), FRoomManagerTestAccess::GetWaitingWallRunEnds(*Manager), 0);

	// An interior ground laid there puts the wall on its outline, and the wall starts running round it.
	FRoomManagerTestAccess::Rebuild(*Manager, { Wall });
	const FGridPlatform Floor = MakeSegment(FIntVector(32, 32, 32), EGridAxis::Z, Square, Kit.GroundInterior, Kit.CeilingExterior);
	FRoomManagerTestAccess::Place(*Manager, Floor);
	FRoomManagerTestAccess::QueueWallRun(*Manager, Floor);
	TestEqual(TEXT("A ground laid beside the wall starts it running from both ends"), FRoomManagerTestAccess::GetWaitingWallRunEnds(*Manager), 2);

	FRoomManagerTestAccess::QueueWallRun(*Manager, Floor);
	TestEqual(TEXT("Not twice while its run waits"), FRoomManagerTestAccess::GetWaitingWallRunEnds(*Manager), 2);

	for (int32 Beat = 0; Beat < 3; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	TestEqual(TEXT("The run closes round the floor"), CountWallsOnOutline(*Manager, EStructureSide::Interior), 4);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// The wall's back over an interior ground already makes the new floor's edge a seam: no outline, no run.
	ARoomManager* Seam = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	Seam->InteriorWallRunChance = 1.0f;
	FRoomManagerTestAccess::Rebuild(*Seam, { Wall, MakeSegment(FIntVector(32, 48, 32), EGridAxis::Z, Seam->Tiles[0], Kit.GroundInterior, Kit.CeilingExterior) });
	const FGridPlatform Second = MakeSegment(FIntVector(32, 32, 32), EGridAxis::Z, Seam->Tiles[0], Kit.GroundInterior, Kit.CeilingExterior);
	FRoomManagerTestAccess::Place(*Seam, Second);
	FRoomManagerTestAccess::QueueWallRun(*Seam, Second);
	TestEqual(TEXT("A wall between two interior grounds starts nothing"), FRoomManagerTestAccess::GetWaitingWallRunEnds(*Seam), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallRunGrowthTest, "WEM.Structures.WallRun.Growth", WEMRoomManagerTests::TestFlags)

bool FWallRunGrowthTest::RunTest(const FString& Parameters)
{
	using namespace WallRunTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FRunKit Kit(Rooted);

	// Two room managers on the same seed, grounds spilling and walls running round them as they grow.
	auto Grow = [&](TArray<FGridPlatform>& OutPlatforms, int32& OutRunBeats) -> ARoomManager*
	{
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
		Manager->GridSize = 129;
		Manager->InteriorGroundSpillChance = 0.3f;
		Manager->ExteriorGroundSpillChance = 0.3f;
		Manager->InteriorWallRunChance = 0.5f;
		Manager->ExteriorWallRunChance = 0.5f;
		Manager->ClearPlatforms();

		OutRunBeats = 0;

		for (int32 Beat = 0; Beat < 400; ++Beat)
		{
			OutRunBeats += FRoomManagerTestAccess::GetWaitingWallRunEnds(*Manager) > 0 ? 1 : 0;
			Manager->PlaceNextPlatform();
		}

		OutPlatforms = FRoomManagerTestAccess::GetBuilt(*Manager);
		return Manager;
	};

	TArray<FGridPlatform> First;
	TArray<FGridPlatform> Second;
	int32 RunBeats = 0;
	int32 SecondRunBeats = 0;
	ARoomManager* Manager = Grow(First, RunBeats);
	Grow(Second, SecondRunBeats);

	AddInfo(FString::Printf(TEXT("%d platforms, %d beats with a run of walls waiting, %d walls on an interior outline, %d on an exterior one."),
		Manager->GetPlatformCount(), RunBeats, CountWallsOnOutline(*Manager, EStructureSide::Interior), CountWallsOnOutline(*Manager, EStructureSide::Exterior)));
	TestTrue(TEXT("Growth goes on"), Manager->GetPlatformCount() > 200);
	TestTrue(TEXT("Walls run"), RunBeats > 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	bool bSame = First.Num() == Second.Num() && RunBeats == SecondRunBeats;
	for (int32 Index = 0; bSame && Index < First.Num(); ++Index)
	{
		bSame = First[Index].MinNode == Second[Index].MinNode && First[Index].Normal == Second[Index].Normal
			&& First[Index].PositiveStructure == Second[Index].PositiveStructure && First[Index].NegativeStructure == Second[Index].NegativeStructure;
	}

	TestTrue(TEXT("The same seed grows the same city, runs and all"), bSame);

	ReleaseRooted(Rooted);
	return true;
}

#endif
