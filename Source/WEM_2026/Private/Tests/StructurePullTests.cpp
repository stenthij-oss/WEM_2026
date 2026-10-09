// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMath.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"
#include "StructureCategory.h"
#include "StructurePlacement.h"

namespace StructurePullTestsPrivate
{
	using namespace WEMRoomManagerTests;

	WEMStructures::FDrawOption MakeOption(const float Weight, std::initializer_list<int32> Categories)
	{
		WEMStructures::FDrawOption Option;
		Option.Weight = Weight;
		Option.Categories.Append(Categories);
		return Option;
	}

	WEMStructures::FNeighbourOffer MakeOffer(std::initializer_list<TPair<int32, float>> Categories)
	{
		WEMStructures::FNeighbourOffer Offer;
		Offer.Categories.Append(Categories);
		return Offer;
	}

	/** How often each option is drawn, over Draws draws from one fixed seed. */
	TArray<double> Measure(TConstArrayView<WEMStructures::FDrawOption> Options, TConstArrayView<WEMStructures::FNeighbourOffer> Offers, const int32 Draws = 40000)
	{
		FRandomStream Stream(20261008);
		TArray<double> Shares;
		Shares.SetNumZeroed(Options.Num());

		for (int32 Draw = 0; Draw < Draws; ++Draw)
		{
			Shares[WEMStructures::DrawWithPull(Stream, Options, Offers)] += 1.0 / Draws;
		}

		return Shares;
	}

	FGridPlatform MakeTestPlatform(const FIntVector& MinNode, const EGridAxis Normal, UPlatformTileData* Tile)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = Normal;
		Platform.Tile = Tile;
		return Platform;
	}

	UStructureCategory* MakeCategory(const TCHAR* Name, TArray<UObject*>& OutRooted)
	{
		UStructureCategory* Category = NewObject<UStructureCategory>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStructureCategory::StaticClass(), Name));
		Category->DisplayName = FText::FromString(Name);
		Category->AddToRoot();
		OutRooted.Add(Category);
		return Category;
	}

	bool CheckShares(FAutomationTestBase& Test, const TCHAR* Name, TConstArrayView<double> Measured, std::initializer_list<double> Expected)
	{
		bool bPassed = true;
		int32 Index = 0;

		for (const double Share : Expected)
		{
			bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: option %d is drawn %.3f of the time (expected %.3f)"), Name, Index, Measured[Index], Share),
				FMath::Abs(Measured[Index] - Share) < 0.012);
			++Index;
		}

		return bPassed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructurePullChanceTest, "WEM.Structures.Pull.ContinueChance", WEMRoomManagerTests::TestFlags)

bool FStructurePullChanceTest::RunTest(const FString& Parameters)
{
	// The defaults: 35% for a streak of one, rising evenly to 70% at four, and no further.
	TestEqual(TEXT("A streak of one"), WEMStructures::GetContinueChance(1, 0.35f, 0.70f, 4), 0.35f, 1.e-5f);
	TestEqual(TEXT("A streak of two"), WEMStructures::GetContinueChance(2, 0.35f, 0.70f, 4), 0.35f + 0.35f / 3.0f, 1.e-5f);
	TestEqual(TEXT("A streak of four"), WEMStructures::GetContinueChance(4, 0.35f, 0.70f, 4), 0.70f, 1.e-5f);
	TestEqual(TEXT("A streak of nine"), WEMStructures::GetContinueChance(9, 0.35f, 0.70f, 4), 0.70f, 1.e-5f);
	TestEqual(TEXT("The most a chance rises to is the maximum, reached at two"), WEMStructures::GetContinueChance(2, 0.2f, 0.9f, 2), 0.9f, 1.e-5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructurePullDrawTest, "WEM.Structures.Pull.Draw", WEMRoomManagerTests::TestFlags)

bool FStructurePullDrawTest::RunTest(const FString& Parameters)
{
	using namespace StructurePullTestsPrivate;
	using WEMStructures::FDrawOption;
	using WEMStructures::FNeighbourOffer;

	const TArray<FDrawOption> TwoOptions = { MakeOption(1.0f, { 0 }), MakeOption(1.0f, { 1 }) };

	// Nothing offered: by weight alone.
	CheckShares(*this, TEXT("No neighbours"), Measure(TwoOptions, {}), { 0.5, 0.5 });

	// One neighbour of category 0, continuing with 0.5: taken half the time, and drawn by weight the rest.
	CheckShares(*this, TEXT("One neighbour"), Measure(TwoOptions, { MakeOffer({ { 0, 0.5f } }) }), { 0.75, 0.25 });

	// A neighbour of a category nothing here carries offers nothing.
	CheckShares(*this, TEXT("A dead category"), Measure(TwoOptions, { MakeOffer({ { 5, 0.9f } }) }), { 0.5, 0.5 });

	// Two neighbours, offering 0.5 and 0.25: one of them is taken with 1 - 0.5 x 0.75, and then the
	// first two times in three.
	CheckShares(*this, TEXT("Two neighbours"), Measure(TwoOptions, { MakeOffer({ { 0, 0.5f } }), MakeOffer({ { 1, 0.25f } }) }),
		{ 0.625 * 2.0 / 3.0 + 0.375 * 0.5, 0.625 / 3.0 + 0.375 * 0.5 });

	// A neighbour of two categories pulls as hard as its stronger one, 0.6, and splits that between
	// them 0.6 to 0.3.
	const TArray<FDrawOption> ThreeOptions = { MakeOption(1.0f, { 0 }), MakeOption(1.0f, { 1 }), MakeOption(1.0f, { 2 }) };
	CheckShares(*this, TEXT("A neighbour of two categories"), Measure(ThreeOptions, { MakeOffer({ { 0, 0.6f }, { 1, 0.3f } }) }),
		{ 0.6 * 2.0 / 3.0 + 0.4 / 3.0, 0.6 / 3.0 + 0.4 / 3.0, 0.4 / 3.0 });

	// Weight counts once per structure, however many categories it carries.
	const TArray<FDrawOption> Broad = { MakeOption(1.0f, { 0, 1, 2 }), MakeOption(1.0f, { 3 }) };
	CheckShares(*this, TEXT("Three categories, one weight"), Measure(Broad, {}), { 0.5, 0.5 });
	CheckShares(*this, TEXT("Three categories, pulled by one of them"), Measure(Broad, { MakeOffer({ { 1, 0.5f } }) }), { 0.75, 0.25 });

	// Weights still divide the pulled category between the options carrying it.
	const TArray<FDrawOption> Weighted = { MakeOption(3.0f, { 0 }), MakeOption(1.0f, { 0 }), MakeOption(4.0f, { 1 }) };
	CheckShares(*this, TEXT("Weights within a category"), Measure(Weighted, { MakeOffer({ { 0, 1.0f } }) }), { 0.75, 0.25, 0.0 });

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructurePullAdjacencyTest, "WEM.Structures.Pull.Adjacency", WEMRoomManagerTests::TestFlags)

bool FStructurePullAdjacencyTest::RunTest(const FString& Parameters)
{
	using namespace StructurePullTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UPlatformTileData* Square = Manager->Tiles[0];

	// Floors either side of the line X = 32, Z = 32 (it runs along Y), a wall standing on it and one
	// hanging below it.
	const FGridPlatform West = MakeTestPlatform(FIntVector(16, 16, 32), EGridAxis::Z, Square);
	const FGridPlatform East = MakeTestPlatform(FIntVector(32, 16, 32), EGridAxis::Z, Square);
	const FGridPlatform Up = MakeTestPlatform(FIntVector(32, 16, 32), EGridAxis::X, Square);
	const FGridPlatform Down = MakeTestPlatform(FIntVector(32, 16, 16), EGridAxis::X, Square);

	// Faces are named by platform index and side: the faces beside one face, as a sorted list.
	auto Beside = [Manager](const int32 PlatformIndex, const int32 FaceSign)
	{
		return FRoomManagerTestAccess::GetFaceNeighbours(*Manager, PlatformIndex, FaceSign);
	};

	auto Faces = [](std::initializer_list<TPair<int32, int32>> Named)
	{
		TArray<int32> Indices;
		for (const TPair<int32, int32>& Face : Named)
		{
			Indices.Add(Face.Key * 2 + (Face.Value > 0 ? 0 : 1));
		}

		Indices.Sort();
		return Indices;
	};

	// Two floors side by side: top with top, underside with underside.
	FRoomManagerTestAccess::Rebuild(*Manager, { West, East });
	TestEqual(TEXT("Coplanar: the west floor's top meets the east floor's top"), Beside(0, 1), Faces({ { 1, 1 } }));
	TestEqual(TEXT("Coplanar: the undersides meet"), Beside(0, -1), Faces({ { 1, -1 } }));

	// A room corner: the floor's top with the wall's inner face, toward the floor; its underside with
	// the wall's outer face, round the outside.
	FRoomManagerTestAccess::Rebuild(*Manager, { West, Up });
	TestEqual(TEXT("Corner: the floor's top meets the wall's face looking back over it"), Beside(0, 1), Faces({ { 1, -1 } }));
	TestEqual(TEXT("Corner: the floor's underside meets the wall's outer face"), Beside(0, -1), Faces({ { 1, 1 } }));
	TestEqual(TEXT("Corner: the wall's inner face meets the floor's top"), Beside(1, -1), Faces({ { 0, 1 } }));

	// A wall on the seam between two floors: one meeting per room, three in all.
	FRoomManagerTestAccess::Rebuild(*Manager, { West, East, Up });
	TestEqual(TEXT("T: the west top meets the wall's west face"), Beside(0, 1), Faces({ { 2, -1 } }));
	TestEqual(TEXT("T: the east top meets the wall's east face"), Beside(1, 1), Faces({ { 2, 1 } }));
	TestEqual(TEXT("T: the undersides still meet beneath the wall"), Beside(0, -1), Faces({ { 1, -1 } }));
	TestEqual(TEXT("T: the wall's two faces meet the two tops"), Beside(2, 1), Faces({ { 1, 1 } }));

	// A cross of four: each face meets the one round the corner from it, four meetings in all.
	FRoomManagerTestAccess::Rebuild(*Manager, { West, East, Up, Down });
	TestEqual(TEXT("Cross: west top and the upper wall's west face"), Beside(0, 1), Faces({ { 2, -1 } }));
	TestEqual(TEXT("Cross: east top and the upper wall's east face"), Beside(1, 1), Faces({ { 2, 1 } }));
	TestEqual(TEXT("Cross: east underside and the lower wall's east face"), Beside(1, -1), Faces({ { 3, 1 } }));
	TestEqual(TEXT("Cross: west underside and the lower wall's west face"), Beside(0, -1), Faces({ { 3, -1 } }));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructurePullStreakTest, "WEM.Structures.Pull.Streaks", WEMRoomManagerTests::TestFlags)

bool FStructurePullStreakTest::RunTest(const FString& Parameters)
{
	using namespace StructurePullTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	UStructureCategory* Urban = MakeCategory(TEXT("TestUrban"), Rooted);
	UStructureCategory* Rural = MakeCategory(TEXT("TestRural"), Rooted);

	constexpr EStructureType AllTypes = EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall;
	UPlatformStructureData* UrbanSlab = MakeStructure(TEXT("TestUrbanSlab"), 8, 8, AllTypes, true);
	UPlatformStructureData* RuralSlab = MakeStructure(TEXT("TestRuralSlab"), 8, 8, AllTypes, true);
	UPlatformStructureData* BothSlab = MakeStructure(TEXT("TestBothSlab"), 8, 8, AllTypes, true);
	UrbanSlab->Categories = { Urban };
	RuralSlab->Categories = { Rural };
	BothSlab->Categories = { Urban, Rural };
	Rooted.Append({ UrbanSlab, RuralSlab, BothSlab });

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->Structures.Append({ UrbanSlab, RuralSlab, BothSlab });
	UPlatformTileData* Small = Manager->Tiles[1];

	// Three small floors in a row, each sharing an edge with the next only.
	auto Row = [Small](UPlatformStructureData* First, UPlatformStructureData* Second, UPlatformStructureData* Third)
	{
		TArray<FGridPlatform> Platforms;
		int32 X = 16;

		for (UPlatformStructureData* Structure : { First, Second, Third })
		{
			FGridPlatform& Platform = Platforms.Add_GetRef(MakeTestPlatform(FIntVector(X, 16, 32), EGridAxis::Z, Small));
			Platform.PositiveStructure = Structure;
			Platform.NegativeStructure = Structure;
			X += 8;
		}

		return Platforms;
	};

	auto Streak = [Manager](const int32 PlatformIndex, const int32 FaceSign, const UStructureCategory* Category)
	{
		return FRoomManagerTestAccess::GetStreak(*Manager, PlatformIndex, FaceSign, Category);
	};

	// A face without the category breaks its chain: the third floor starts urban afresh.
	FRoomManagerTestAccess::Rebuild(*Manager, Row(UrbanSlab, RuralSlab, UrbanSlab));
	TestEqual(TEXT("The first urban floor starts a streak of one"), Streak(0, 1, Urban), 1);
	TestEqual(TEXT("The rural floor beside it starts a rural streak"), Streak(1, 1, Rural), 1);
	TestEqual(TEXT("The third, past a floor that is not urban, starts over"), Streak(2, 1, Urban), 1);
	TestEqual(TEXT("A face has no streak in a category it does not carry"), Streak(1, 1, Urban), 0);

	// A structure of two categories carries both chains on.
	FRoomManagerTestAccess::Rebuild(*Manager, Row(UrbanSlab, BothSlab, BothSlab));
	TestEqual(TEXT("The two-category floor continues the urban chain"), Streak(1, 1, Urban), 2);
	TestEqual(TEXT("And starts a rural one"), Streak(1, 1, Rural), 1);
	TestEqual(TEXT("The next continues both"), Streak(2, 1, Urban), 3);
	TestEqual(TEXT("Both of them"), Streak(2, 1, Rural), 2);
	TestEqual(TEXT("Undersides keep streaks of their own, the same here"), Streak(2, -1, Urban), 3);

	// Patches: urban tops 3 and undersides 3, rural tops 2 and undersides 2 - sizes squared over sizes.
	TestEqual(TEXT("The category patch size weighs each patch by its size"), Manager->CategoryPatchSize, 26.0 / 10.0, 1.e-9);

	ReleaseRooted(Rooted);
	return true;
}

#endif
