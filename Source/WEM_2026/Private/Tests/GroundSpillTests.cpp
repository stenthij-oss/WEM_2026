// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace GroundSpillTestsPrivate
{
	using namespace WEMRoomManagerTests;

	FGridPlatform MakeFloor(const FIntVector& MinNode, UPlatformTileData* Tile, UPlatformStructureData* Top, UPlatformStructureData* Underside)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = EGridAxis::Z;
		Platform.Tile = Tile;
		Platform.Up = EGridFace::PosZ;
		Platform.PositiveStructure = Top;
		Platform.NegativeStructure = Underside;
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
	struct FSpillKit
	{
		UPlatformStructureData* GroundInterior = nullptr;
		UPlatformStructureData* GroundExterior = nullptr;
		UPlatformStructureData* GroundBoth = nullptr;
		UPlatformStructureData* RoofExterior = nullptr;
		UPlatformStructureData* CeilingInterior = nullptr;
		UPlatformStructureData* CeilingExterior = nullptr;
		UPlatformStructureData* WallInterior = nullptr;
		UPlatformStructureData* WallExterior = nullptr;

		explicit FSpillKit(TArray<UObject*>& OutRooted)
		{
			GroundInterior = MakeSided(TEXT("TestSpillGroundInt"), EStructureType::Ground, EStructureSide::Interior, OutRooted);
			GroundExterior = MakeSided(TEXT("TestSpillGroundExt"), EStructureType::Ground, EStructureSide::Exterior, OutRooted);
			GroundBoth = MakeSided(TEXT("TestSpillGroundBoth"), EStructureType::Ground, EStructureSide::Both, OutRooted);
			RoofExterior = MakeSided(TEXT("TestSpillRoofExt"), EStructureType::Roof, EStructureSide::Exterior, OutRooted);
			CeilingInterior = MakeSided(TEXT("TestSpillCeilingInt"), EStructureType::Ceiling, EStructureSide::Interior, OutRooted);
			CeilingExterior = MakeSided(TEXT("TestSpillCeilingExt"), EStructureType::Ceiling, EStructureSide::Exterior, OutRooted);
			WallInterior = MakeSided(TEXT("TestSpillWallInt"), EStructureType::Wall, EStructureSide::Interior, OutRooted);
			WallExterior = MakeSided(TEXT("TestSpillWallExt"), EStructureType::Wall, EStructureSide::Exterior, OutRooted);
		}

		TArray<TObjectPtr<UPlatformStructureData>> GetAll(const bool bWithBoth) const
		{
			TArray<TObjectPtr<UPlatformStructureData>> All = { GroundInterior, GroundExterior, RoofExterior, CeilingInterior, CeilingExterior, WallInterior, WallExterior };

			if (bWithBoth)
			{
				All.Add(GroundBoth);
			}

			return All;
		}
	};

	/** Whether two floors in one plane share a stretch of rim: touching along one axis, overlapping along the other. */
	bool SharesRim(const FGridPlatform& First, const FGridPlatform& Second)
	{
		const int32 FirstX = First.MinNode.X;
		const int32 FirstY = First.MinNode.Y;
		const int32 SecondX = Second.MinNode.X;
		const int32 SecondY = Second.MinNode.Y;
		const int32 FirstW = First.Tile->Width;
		const int32 FirstL = First.Tile->Length;
		const int32 SecondW = Second.Tile->Width;
		const int32 SecondL = Second.Tile->Length;

		const bool bTouchX = FirstX + FirstW == SecondX || SecondX + SecondW == FirstX;
		const bool bTouchY = FirstY + FirstL == SecondY || SecondY + SecondL == FirstY;
		const bool bOverlapX = FirstX < SecondX + SecondW && SecondX < FirstX + FirstW;
		const bool bOverlapY = FirstY < SecondY + SecondL && SecondY < FirstY + FirstL;

		return First.MinNode.Z == Second.MinNode.Z && ((bTouchX && bOverlapY) || (bTouchY && bOverlapX));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundSpillPatchTest, "WEM.Structures.GroundSpill.Patch", WEMRoomManagerTests::TestFlags)

bool FGroundSpillPatchTest::RunTest(const FString& Parameters)
{
	using namespace GroundSpillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FSpillKit Kit(Rooted);

	// Interior grounds always spill, to four pieces; behind an interior an exterior always, so the undersides show
	// they are drawn as any floor's.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll(/*bWithBoth=*/false));
	UPlatformTileData* Square = Manager->Tiles[0];
	Manager->InteriorGroundSpillChance = 1.0f;
	Manager->InteriorGroundSpillMin = 4;
	Manager->InteriorGroundSpillMax = 4;
	Manager->InteriorBacksExteriorChance = 1.0f;

	const FGridPlatform First = MakeFloor(FIntVector(32, 32, 32), Square, Kit.GroundInterior, Kit.CeilingExterior);
	FRoomManagerTestAccess::Place(*Manager, First);
	FRoomManagerTestAccess::QueueGroundSpill(*Manager, First);

	int32 Remaining = 0;
	TestEqual(TEXT("An interior ground starts a patch"), FRoomManagerTestAccess::GetGroundSpills(*Manager, Remaining), 1);
	TestEqual(TEXT("With three pieces to go"), Remaining, 3);

	// Each growth beat lays one piece of it, before anything else.
	TArray<FGridPlatform> Patch = { First };

	for (int32 Beat = 0; Beat < 3; ++Beat)
	{
		const int32 Before = Manager->GetPlatformCount();
		TestTrue(FString::Printf(TEXT("Beat %d places"), Beat), Manager->PlaceNextPlatform());
		TestEqual(FString::Printf(TEXT("Beat %d lays one piece"), Beat), Manager->GetPlatformCount(), Before + 1);

		const FGridPlatform Laid = FRoomManagerTestAccess::GetBuilt(*Manager).Last();
		TestTrue(TEXT("In the patch's plane, with its Up"), Laid.Normal == EGridAxis::Z && Laid.MinNode.Z == 32 && Laid.Up == EGridFace::PosZ);
		TestTrue(TEXT("Topped with an interior ground"), Laid.PositiveStructure == Kit.GroundInterior);
		TestTrue(TEXT("Its underside drawn as any floor's: an exterior behind the interior"),
			Laid.NegativeStructure && Laid.NegativeStructure->StandsFor(EStructureSide::Exterior) && !Laid.NegativeStructure->HasType(EStructureType::Roof));

		bool bBeside = false;
		for (const FGridPlatform& Piece : Patch)
		{
			bBeside = bBeside || SharesRim(Piece, Laid);
		}

		TestTrue(TEXT("Beside a piece the patch has laid"), bBeside);
		Patch.Add(Laid);

		TestEqual(FString::Printf(TEXT("After beat %d, spilling until it has four pieces"), Beat), FRoomManagerTestAccess::GetGroundSpills(*Manager, Remaining), Beat < 2 ? 1 : 0);
	}

	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// An exterior ground spills exteriors - of both sides too.
	ARoomManager* Outside = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll(/*bWithBoth=*/true));
	Outside->ExteriorGroundSpillChance = 1.0f;
	Outside->ExteriorGroundSpillMin = 6;
	Outside->ExteriorGroundSpillMax = 6;

	const FGridPlatform Plaza = MakeFloor(FIntVector(32, 32, 32), Outside->Tiles[0], Kit.GroundExterior, Kit.CeilingInterior);
	FRoomManagerTestAccess::Place(*Outside, Plaza);
	FRoomManagerTestAccess::QueueGroundSpill(*Outside, Plaza);

	int32 Exteriors = 0;
	for (int32 Beat = 0; Beat < 5; ++Beat)
	{
		Outside->PlaceNextPlatform();
		const UPlatformStructureData* Top = FRoomManagerTestAccess::GetBuilt(*Outside).Last().PositiveStructure;
		Exteriors += Top && Top->StandsFor(EStructureSide::Exterior) && Top->HasType(EStructureType::Ground) ? 1 : 0;
	}

	TestEqual(TEXT("Every piece of an exterior patch is an exterior ground"), Exteriors, 5);
	TestEqual(TEXT("And the patch is done"), FRoomManagerTestAccess::GetGroundSpills(*Outside, Remaining), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundSpillStartsTest, "WEM.Structures.GroundSpill.Starts", WEMRoomManagerTests::TestFlags)

bool FGroundSpillStartsTest::RunTest(const FString& Parameters)
{
	using namespace GroundSpillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FSpillKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll(/*bWithBoth=*/true));
	UPlatformTileData* Square = Manager->Tiles[0];
	Manager->InteriorGroundSpillChance = 1.0f;
	Manager->ExteriorGroundSpillChance = 1.0f;

	int32 Remaining = 0;
	auto Starts = [&](const FGridPlatform& Platform)
	{
		FRoomManagerTestAccess::Rebuild(*Manager, { Platform });
		FRoomManagerTestAccess::QueueGroundSpill(*Manager, Platform);
		const bool bStarted = FRoomManagerTestAccess::GetGroundSpills(*Manager, Remaining) > 0;
		Manager->ClearPlatforms();
		return bStarted;
	};

	TestTrue(TEXT("An interior ground starts a patch"), Starts(MakeFloor(FIntVector(32, 32, 32), Square, Kit.GroundInterior, Kit.CeilingExterior)));
	TestTrue(TEXT("An exterior ground starts a patch"), Starts(MakeFloor(FIntVector(32, 32, 32), Square, Kit.GroundExterior, Kit.CeilingInterior)));
	TestFalse(TEXT("One of both sides binds nothing, and starts none"), Starts(MakeFloor(FIntVector(32, 32, 32), Square, Kit.GroundBoth, Kit.CeilingInterior)));
	TestFalse(TEXT("A roof starts none"), Starts(MakeFloor(FIntVector(32, 32, 32), Square, Kit.RoofExterior, Kit.CeilingInterior)));

	FGridPlatform Wall = MakeFloor(FIntVector(32, 32, 32), Square, Kit.WallInterior, Kit.WallExterior);
	Wall.Normal = EGridAxis::X;
	TestFalse(TEXT("A wall starts none"), Starts(Wall));

	// Only the face looking along Up starts a patch: an interior ground hanging face down under an exterior one does not.
	Manager->ExteriorGroundSpillChance = 0.0f;
	TestFalse(TEXT("An interior ground looking down starts none"), Starts(MakeFloor(FIntVector(32, 32, 32), Square, Kit.GroundExterior, Kit.GroundInterior)));

	// At a chance of zero nothing is drawn, so growth draws as it did before there were spills.
	Manager->InteriorGroundSpillChance = 0.0f;
	TestFalse(TEXT("At no chance, none"), Starts(MakeFloor(FIntVector(32, 32, 32), Square, Kit.GroundInterior, Kit.CeilingExterior)));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundSpillHemmedTest, "WEM.Structures.GroundSpill.Hemmed", WEMRoomManagerTests::TestFlags)

bool FGroundSpillHemmedTest::RunTest(const FString& Parameters)
{
	using namespace GroundSpillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FSpillKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll(/*bWithBoth=*/false));
	UPlatformTileData* Square = Manager->Tiles[0];
	Manager->InteriorGroundSpillChance = 1.0f;
	Manager->InteriorGroundSpillMin = 4;
	Manager->InteriorGroundSpillMax = 4;

	// A floor in the middle of a ring of roofs has nowhere in its plane to spill to.
	TArray<FGridPlatform> Floors;
	for (int32 X = 16; X <= 48; X += 16)
	{
		for (int32 Y = 16; Y <= 48; Y += 16)
		{
			const bool bMiddle = X == 32 && Y == 32;
			Floors.Add(MakeFloor(FIntVector(X, Y, 32), Square, bMiddle ? Kit.GroundInterior : Kit.RoofExterior, Kit.CeilingInterior));
		}
	}

	FRoomManagerTestAccess::Rebuild(*Manager, Floors);
	FRoomManagerTestAccess::QueueGroundSpill(*Manager, Floors[4]);

	int32 Remaining = 0;
	TestEqual(TEXT("The middle floor starts a patch"), FRoomManagerTestAccess::GetGroundSpills(*Manager, Remaining), 1);
	TestFalse(TEXT("Which has nowhere to go"), FRoomManagerTestAccess::PlaceGroundSpillPiece(*Manager));
	TestEqual(TEXT("So it stops"), FRoomManagerTestAccess::GetGroundSpills(*Manager, Remaining), 0);
	TestEqual(TEXT("Having laid nothing"), Manager->GetPlatformCount(), 9);

	// A piece of a patch taken away is taken out of it, and a patch with none left goes.
	FRoomManagerTestAccess::QueueGroundSpill(*Manager, Floors[4]);
	FRoomManagerTestAccess::Remove(*Manager, { Floors[4] });
	TestEqual(TEXT("Its first piece struck, the patch goes"), FRoomManagerTestAccess::GetGroundSpills(*Manager, Remaining), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundSpillGrowthTest, "WEM.Structures.GroundSpill.Growth", WEMRoomManagerTests::TestFlags)

bool FGroundSpillGrowthTest::RunTest(const FString& Parameters)
{
	using namespace GroundSpillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FSpillKit Kit(Rooted);

	// Two room managers on the same seed, grounds spilling now and then as they grow.
	auto Grow = [&](TArray<FGridPlatform>& OutPlatforms, int32& OutSpillBeats) -> ARoomManager*
	{
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll(/*bWithBoth=*/true));
		Manager->GridSize = 129;
		Manager->InteriorGroundSpillChance = 0.3f;
		Manager->ExteriorGroundSpillChance = 0.3f;
		Manager->ClearPlatforms();

		OutSpillBeats = 0;
		int32 Remaining = 0;

		for (int32 Beat = 0; Beat < 400; ++Beat)
		{
			OutSpillBeats += FRoomManagerTestAccess::GetGroundSpills(*Manager, Remaining) > 0 ? 1 : 0;
			Manager->PlaceNextPlatform();
		}

		OutPlatforms = FRoomManagerTestAccess::GetBuilt(*Manager);
		return Manager;
	};

	TArray<FGridPlatform> First;
	TArray<FGridPlatform> Second;
	int32 SpillBeats = 0;
	int32 SecondSpillBeats = 0;
	ARoomManager* Manager = Grow(First, SpillBeats);
	Grow(Second, SecondSpillBeats);

	AddInfo(FString::Printf(TEXT("%d platforms, %d beats taken by a patch spilling."), Manager->GetPlatformCount(), SpillBeats));
	TestTrue(TEXT("Growth goes on"), Manager->GetPlatformCount() > 200);
	TestTrue(TEXT("Patches spill"), SpillBeats > 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	bool bSame = First.Num() == Second.Num() && SpillBeats == SecondSpillBeats;
	for (int32 Index = 0; bSame && Index < First.Num(); ++Index)
	{
		bSame = First[Index].MinNode == Second[Index].MinNode && First[Index].Normal == Second[Index].Normal
			&& First[Index].PositiveStructure == Second[Index].PositiveStructure && First[Index].NegativeStructure == Second[Index].NegativeStructure;
	}

	TestTrue(TEXT("The same seed grows the same city, patches and all"), bSame);

	ReleaseRooted(Rooted);
	return true;
}

#endif
