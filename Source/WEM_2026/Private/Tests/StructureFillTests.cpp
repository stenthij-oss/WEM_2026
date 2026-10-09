// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMath.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"
#include "StructureCategory.h"
#include "StructurePlacement.h"

namespace StructureFillTestsPrivate
{
	using namespace WEMRoomManagerTests;

	constexpr EStructureType AllTypes = EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall;

	/** The tile sizes SpawnRoomManager lays: 16 x 16, 8 x 8, and 8 x 16 that may turn. */
	const FIntPoint TileSizes[] = { FIntPoint(16, 16), FIntPoint(8, 8), FIntPoint(8, 16) };

	/** Per size: a one-sided ground, ceiling and wall structure, the wall free to lie sideways. */
	TArray<TObjectPtr<UPlatformStructureData>> MakeOneSided(TArray<UObject*>& OutRooted, const bool bCeilingHoldsItems = true)
	{
		TArray<TObjectPtr<UPlatformStructureData>> Structures;

		for (const FIntPoint& Size : TileSizes)
		{
			UPlatformStructureData* Ground = MakeStructure(TEXT("TestGround"), Size.X, Size.Y, EStructureType::Ground, false);
			UPlatformStructureData* Ceiling = MakeStructure(TEXT("TestCeiling"), Size.X, Size.Y, EStructureType::Ceiling, false);
			UPlatformStructureData* Wall = MakeStructure(TEXT("TestWall"), Size.X, Size.Y, EStructureType::Wall, false, /*bMayLieSideways=*/true);
			Ceiling->bHoldsItems = bCeilingHoldsItems;

			Structures.Append({ Ground, Ceiling, Wall });
			OutRooted.Append({ Ground, Ceiling, Wall });
		}

		return Structures;
	}

	/** Per size: a two-sided slab of every type, and the one-sided set, all drawn together. */
	TArray<TObjectPtr<UPlatformStructureData>> MakeMixed(TArray<UObject*>& OutRooted)
	{
		TArray<TObjectPtr<UPlatformStructureData>> Structures = MakeOneSided(OutRooted);

		for (const FIntPoint& Size : TileSizes)
		{
			UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), Size.X, Size.Y, AllTypes, true, /*bMayLieSideways=*/true);
			Structures.Add(Slab);
			OutRooted.Add(Slab);
		}

		return Structures;
	}

	void GetSpans(const FGridPlatform& Platform, int32& OutFirst, int32& OutSecond)
	{
		OutFirst = Platform.bTurned ? Platform.Tile->Length : Platform.Tile->Width;
		OutSecond = Platform.bTurned ? Platform.Tile->Width : Platform.Tile->Length;
	}

	/**
	 * Why a platform's fill is not a complete one that fits, or an empty string when it is: a structure
	 * on each face that fits that face, the same one on both when it is two-sided.
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

		int32 SpanFirst, SpanSecond;
		GetSpans(Platform, SpanFirst, SpanSecond);

		const int32 NormalAxis = static_cast<int32>(Platform.Normal);
		bool bTurned = false;

		if (!WEMStructures::FitsFace(*Positive, NormalAxis, 1, SpanFirst, SpanSecond, bTurned))
		{
			return FString::Printf(TEXT("%s does not fit the positive face"), *Positive->GetName());
		}

		if (!WEMStructures::FitsFace(*Negative, NormalAxis, -1, SpanFirst, SpanSecond, bTurned))
		{
			return FString::Printf(TEXT("%s does not fit the negative face"), *Negative->GetName());
		}

		return FString();
	}

	FString Describe(const FGridPlatform& Platform)
	{
		return FString::Printf(TEXT("%s facing %d, %d x %d%s"), *Platform.MinNode.ToString(), static_cast<int32>(Platform.Normal),
			Platform.Tile->Width, Platform.Tile->Length, Platform.bTurned ? TEXT(" turned") : TEXT(""));
	}

	/**
	 * Gives the structures categories from three, in turn, and every third one a second, so the
	 * category pull, its streaks and its patches all come into play.
	 */
	void AddCategories(TConstArrayView<TObjectPtr<UPlatformStructureData>> Structures, TArray<UObject*>& OutRooted)
	{
		UStructureCategory* Categories[3];

		for (int32 Index = 0; Index < 3; ++Index)
		{
			Categories[Index] = NewObject<UStructureCategory>(GetTransientPackage(),
				MakeUniqueObjectName(GetTransientPackage(), UStructureCategory::StaticClass(), TEXT("TestCategory")));
			Categories[Index]->AddToRoot();
			OutRooted.Add(Categories[Index]);
		}

		for (int32 Index = 0; Index < Structures.Num(); ++Index)
		{
			Structures[Index]->Categories = { Categories[Index % 3] };

			if (Index % 3 == 0)
			{
				Structures[Index]->Categories.Add(Categories[(Index + 1) % 3]);
			}
		}
	}

	/** Grows Beats beats and returns how many platforms stand. */
	int32 Grow(ARoomManager& Manager, const int32 Beats)
	{
		for (int32 Beat = 0; Beat < Beats; ++Beat)
		{
			Manager.PlaceNextPlatform();
		}

		return Manager.GetPlatformCount();
	}

	/** Grows with VerifyEvery at 1 and returns how many checks drifted. */
	int32 GrowVerified(FAutomationTestBase& Test, ARoomManager& Manager, const int32 Placements)
	{
		IConsoleVariable* VerifyEvery = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.RoomManager.VerifyEvery"));

		if (!Test.TestNotNull(TEXT("The VerifyEvery console variable exists"), VerifyEvery))
		{
			return -1;
		}

		const int32 Previous = VerifyEvery->GetInt();
		VerifyEvery->Set(1, ECVF_SetByCode);

		for (int32 Beat = 0; Beat < Placements && Manager.PlaceNextPlatform(); ++Beat)
		{
		}

		VerifyEvery->Set(Previous, ECVF_SetByCode);

		Test.AddInfo(FString::Printf(TEXT("%d placement(s) checked against a full rebuild, %d platform(s) standing."),
			FRoomManagerTestAccess::GetVerifiedPlacements(Manager), Manager.GetPlatformCount()));

		return FRoomManagerTestAccess::GetFailedVerifications(Manager);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFillCompleteTest, "WEM.Structures.Fill.Complete", WEMRoomManagerTests::TestFlags)

bool FStructureFillCompleteTest::RunTest(const FString& Parameters)
{
	using namespace StructureFillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, MakeMixed(Rooted));

	TestTrue(TEXT("Growth takes off"), Grow(*Manager, 300) > 100);

	// Every platform stands with a complete fill that fits it, and both kinds of fill turn up.
	int32 TwoSided = 0;
	int32 OneSided = 0;

	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		const FString Problem = CheckFill(Platform);

		if (!Problem.IsEmpty())
		{
			AddError(FString::Printf(TEXT("The platform at %s has no complete fill: %s"), *Describe(Platform), *Problem));
			break;
		}

		(Platform.PositiveStructure->bTwoSided ? TwoSided : OneSided) += 1;
	}

	TestTrue(TEXT("Some platforms are filled two-sided"), TwoSided > 0);
	TestTrue(TEXT("Some platforms are filled with a one-sided pair"), OneSided > 0);

	// The first holds everything else up.
	const FGridPlatform& First = Manager->Platforms[0];
	TestTrue(TEXT("The first platform is filled with load-bearing structures"),
		First.PositiveStructure->bLoadBearing && First.NegativeStructure->bLoadBearing);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFillOneSidedTest, "WEM.Structures.Fill.OneSidedPairs", WEMRoomManagerTests::TestFlags)

bool FStructureFillOneSidedTest::RunTest(const FString& Parameters)
{
	using namespace StructureFillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, MakeOneSided(Rooted));

	TestTrue(TEXT("Growth takes off on one-sided structures alone"), Grow(*Manager, 200) > 80);

	// With nothing two-sided to draw, every platform takes a pair: ground over ceiling, or wall back to wall.
	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		const FString Problem = CheckFill(Platform);
		const bool bPair = Platform.PositiveStructure && Platform.NegativeStructure
			&& !Platform.PositiveStructure->bTwoSided && !Platform.NegativeStructure->bTwoSided;

		if (!Problem.IsEmpty() || !bPair)
		{
			AddError(FString::Printf(TEXT("The platform at %s is not covered by a one-sided pair: %s"), *Describe(Platform), *Problem));
			break;
		}

		if (Platform.GetKind() == EPlatformKind::SurfaceHorizontal
			&& (!Platform.PositiveStructure->HasType(EStructureType::Ground) || !Platform.NegativeStructure->HasType(EStructureType::Ceiling)))
		{
			AddError(FString::Printf(TEXT("The floor at %s is not ground over ceiling"), *Describe(Platform)));
			break;
		}
	}

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFillHoldsItemsTest, "WEM.Structures.Fill.HoldsItems", WEMRoomManagerTests::TestFlags)

bool FStructureFillHoldsItemsTest::RunTest(const FString& Parameters)
{
	using namespace StructureFillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Every ceiling holds nothing, and a two-sided wall slab that holds nothing either.
	TArray<TObjectPtr<UPlatformStructureData>> Structures = MakeOneSided(Rooted, /*bCeilingHoldsItems=*/false);
	UPlatformStructureData* BareWall = MakeStructure(TEXT("TestBareWall"), 16, 16, EStructureType::Wall, true);
	BareWall->bHoldsItems = false;
	BareWall->Weight = 5.0f;
	Structures.Add(BareWall);
	Rooted.Add(BareWall);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
	Grow(*Manager, 200);

	int32 CheckedFloors = 0;
	int32 CheckedWalls = 0;

	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		// A cell inside the opening, off every lattice line, which only this platform covers.
		const int32 NormalAxis = static_cast<int32>(Platform.Normal);
		int32 U, V;
		WEMGrid::GetInPlaneAxes(NormalAxis, U, V);
		const FIntVector Inside = Platform.MinNode + WEMGrid::AxisStep(U, 1) + WEMGrid::AxisStep(V, 1);

		const ESurfaceCapacity Positive = Manager->GetSurfaceCapacity(Inside, WEMGrid::MakeFace(NormalAxis, 1));
		const ESurfaceCapacity Negative = Manager->GetSurfaceCapacity(Inside, WEMGrid::MakeFace(NormalAxis, -1));

		if (Platform.GetKind() == EPlatformKind::SurfaceHorizontal)
		{
			++CheckedFloors;
			TestEqual(*FString::Printf(TEXT("The underside of %s, under a ceiling that holds nothing, is not a surface"), *Describe(Platform)), Negative, ESurfaceCapacity::Empty);
			TestEqual(*FString::Printf(TEXT("The top of %s still carries anything"), *Describe(Platform)), Positive, ESurfaceCapacity::AllObject);
		}
		else if (Platform.PositiveStructure == BareWall)
		{
			++CheckedWalls;
			TestTrue(*FString::Printf(TEXT("Both faces of %s, filled two-sided with a structure that holds nothing, are not surfaces"), *Describe(Platform)),
				Positive == ESurfaceCapacity::Empty && Negative == ESurfaceCapacity::Empty);
		}
	}

	TestTrue(TEXT("Floors were checked"), CheckedFloors > 10);
	TestTrue(TEXT("Bare walls were checked"), CheckedWalls > 0);

	// Nothing about it drifts from a full rebuild.
	TestEqual(TEXT("The incremental state matches a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFillNoFillTest, "WEM.Structures.Fill.NoFill", WEMRoomManagerTests::TestFlags)

bool FStructureFillNoFillTest::RunTest(const FString& Parameters)
{
	using namespace StructureFillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Floors of every size, but walls only for 16 x 16 and for 8 x 16 stood upright: 8 x 8 has no
	// wall fill at all, and 8 x 16 none lying on its side.
	TArray<TObjectPtr<UPlatformStructureData>> Structures;

	for (const FIntPoint& Size : TileSizes)
	{
		Structures.Add(MakeStructure(TEXT("TestFloor"), Size.X, Size.Y, EStructureType::Ground | EStructureType::Ceiling, true));
	}

	Structures.Add(MakeStructure(TEXT("TestSquareWall"), 16, 16, EStructureType::Wall, true));
	Structures.Add(MakeStructure(TEXT("TestUprightWall"), 8, 16, EStructureType::Wall, true));
	Rooted.Append(Structures);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
	Grow(*Manager, 300);

	int32 UprightWalls = 0;

	for (const FGridPlatform& Platform : Manager->Platforms)
	{
		if (Platform.GetKind() != EPlatformKind::SurfaceVertical)
		{
			continue;
		}

		TestFalse(*FString::Printf(TEXT("No 8 x 8 is stood up as a wall (%s)"), *Describe(Platform)), Platform.Tile->Width == 8 && Platform.Tile->Length == 8);
		TestFalse(*FString::Printf(TEXT("No 8 x 16 wall lies on its side (%s)"), *Describe(Platform)), Platform.Tile->Width == 8 && Platform.Tile->Length == 16 && Platform.bTurned);
		UprightWalls += Platform.Tile->Width == 8 && Platform.Tile->Length == 16 ? 1 : 0;
	}

	TestTrue(TEXT("Upright 8 x 16 walls still grow"), UprightWalls > 0);
	TestTrue(TEXT("Floors still grow"), Manager->HorizontalPlatformCount > 20);

	// With nothing load-bearing to fill the first tile, nothing grows at all.
	for (UPlatformStructureData* Structure : Structures)
	{
		Structure->bLoadBearing = false;
	}

	AddExpectedError(TEXT("no load-bearing structures can fill FirstTile"), EAutomationExpectedErrorFlags::Contains, 0);
	Manager->ClearPlatforms();

	TestFalse(TEXT("Without a load-bearing fill for the first tile, no beat places anything"), Manager->PlaceNextPlatform());
	TestEqual(TEXT("And nothing stands"), Manager->GetPlatformCount(), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFillSameGrowthTest, "WEM.Structures.Fill.SameGrowth", WEMRoomManagerTests::TestFlags)

bool FStructureFillSameGrowthTest::RunTest(const FString& Parameters)
{
	using namespace StructureFillTestsPrivate;

	// The same seed, filled from two different sets of structures that each fill every tile every
	// way: the fills differ, the platforms do not, since the fills are drawn from a stream apart.
	auto GrowPlatforms = [this](const bool bOneSided)
	{
		FScopedTestWorld TestWorld;
		TArray<UObject*> Rooted;
		ARoomManager* Manager = bOneSided
			? SpawnRoomManager(TestWorld.World, Rooted, MakeOneSided(Rooted))
			: SpawnRoomManager(TestWorld.World, Rooted);

		Grow(*Manager, 250);
		TArray<FGridPlatform> Grown = Manager->Platforms;
		ReleaseRooted(Rooted);
		return Grown;
	};

	const TArray<FGridPlatform> WithSlabs = GrowPlatforms(false);
	const TArray<FGridPlatform> WithPairs = GrowPlatforms(true);

	bool bSame = WithSlabs.Num() == WithPairs.Num();

	for (int32 Index = 0; bSame && Index < WithSlabs.Num(); ++Index)
	{
		const FGridPlatform& A = WithSlabs[Index];
		const FGridPlatform& B = WithPairs[Index];

		bSame = A.MinNode == B.MinNode && A.Normal == B.Normal && A.bTurned == B.bTurned
			&& A.Tile->Width == B.Tile->Width && A.Tile->Length == B.Tile->Length;
	}

	TestTrue(FString::Printf(TEXT("The same seed lays the same %d platforms whatever fills them"), WithSlabs.Num()), bSame && WithSlabs.Num() > 100);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFillVerifyTest, "WEM.Structures.Fill.VerifyEvery", WEMRoomManagerTests::TestFlags)

bool FStructureFillVerifyTest::RunTest(const FString& Parameters)
{
	using namespace StructureFillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	const TArray<TObjectPtr<UPlatformStructureData>> Structures = MakeOneSided(Rooted, /*bCeilingHoldsItems=*/false);
	AddCategories(Structures, Rooted);
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);

	// The pieces on as well, so the structures' layers are held to a full rebuild too.
	Manager->bShowPieces = true;
	Manager->ClearPlatforms();

	TestEqual(TEXT("Every placement matches a full rebuild"), GrowVerified(*this, *Manager, 200), 0);
	TestTrue(TEXT("Categories gathered into patches"), Manager->CategoryPatchSize > 1.0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFillVerifyLongTest, "WEM.Structures.Fill.VerifyEvery1000", WEMRoomManagerTests::SlowTestFlags)

bool FStructureFillVerifyLongTest::RunTest(const FString& Parameters)
{
	using namespace StructureFillTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	// Everything at once: two- and one-sided fills, categories, structures that are not load-bearing,
	// and fences on walls and floors.
	TArray<TObjectPtr<UPlatformStructureData>> Structures = MakeMixed(Rooted);
	AddCategories(Structures, Rooted);

	UPlatformStructureData* LightSlab = MakeStructure(TEXT("TestLightSlab"), 16, 16, AllTypes, true, /*bMayLieSideways=*/true);
	UPlatformStructureData* Railing = MakeStructure(TEXT("TestRailing"), 8, 8, EStructureType::Wall, true);
	UPlatformStructureData* Parapet = MakeStructure(TEXT("TestParapet"), 16, 8, EStructureType::Wall, true);
	UPlatformStructureData* Boardwalk = MakeStructure(TEXT("TestBoardwalk"), 16, 16, EStructureType::Ground | EStructureType::Ceiling, true);
	LightSlab->bLoadBearing = false;
	LightSlab->Weight = 0.3f;
	Railing->bFence = true;
	Parapet->bFence = true;
	Parapet->bLoadBearing = false;
	Boardwalk->bFence = true;
	Boardwalk->bLoadBearing = false;
	Structures.Append({ LightSlab, Railing, Parapet, Boardwalk });
	Rooted.Append({ LightSlab, Railing, Parapet, Boardwalk });

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
	Manager->bShowPieces = true;
	Manager->GridSize = 257;
	Manager->ClearPlatforms();

	TestEqual(TEXT("A thousand placements, each matching a full rebuild"), GrowVerified(*this, *Manager, 1000), 0);
	TestEqual(TEXT("A thousand placements were made"), FRoomManagerTestAccess::GetVerifiedPlacements(*Manager), 1000);

	ReleaseRooted(Rooted);
	return true;
}

#endif
