// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "ObjectPlacer.h"
#include "PlacementTestHelpers.h"
#include "PlatformTileData.h"
#include "RoomManager.h"

/** What the tests read of the room manager's private state. Befriended by ARoomManager. */
struct FRoomManagerTestAccess
{
	static void EnsureCandidates(ARoomManager& Manager)
	{
		Manager.EnsurePlatformCandidates();
	}

	static bool IsListed(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		for (const ARoomManager::FTileCandidates& Candidates : Manager.TileCandidates)
		{
			if (Candidates.Tile != Platform.Tile)
			{
				continue;
			}

			for (const TArray<FGridPlatform>& ByKind : Candidates.ByKind)
			{
				for (const FGridPlatform& Listed : ByKind)
				{
					if (Listed.MinNode == Platform.MinNode && Listed.Normal == Platform.Normal && Listed.bTurned == Platform.bTurned)
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	static TArray<FGridPlatform> GetListed(const ARoomManager& Manager)
	{
		TArray<FGridPlatform> Listed;
		for (const ARoomManager::FTileCandidates& Candidates : Manager.TileCandidates)
		{
			for (const TArray<FGridPlatform>& ByKind : Candidates.ByKind)
			{
				Listed.Append(ByKind);
			}
		}

		return Listed;
	}

	/** Runs the wholesale check and returns how many checks have drifted so far. */
	static int32 Verify(ARoomManager& Manager)
	{
		Manager.VerifyIncrementalState();
		return Manager.FailedVerifications;
	}

	static int32 GetFailedVerifications(const ARoomManager& Manager)
	{
		return Manager.FailedVerifications;
	}

	static int32 GetVerifiedPlacements(const ARoomManager& Manager)
	{
		return Manager.VerifiedPlacements;
	}

	static bool IsStale(const ARoomManager& Manager)
	{
		return Manager.bPlatformCandidatesStale;
	}
};

namespace PlacementIntegrationTestsPrivate
{
	using namespace WEMPlacementTests;

	/** A game world with nothing in it, torn down again when this goes out of scope. */
	struct FScopedTestWorld
	{
		FScopedTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false, TEXT("WEMPlacementTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
		}

		~FScopedTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		UWorld* World = nullptr;
	};

	UPlatformTileData* MakeTile(const int32 Width, const int32 Length, const bool bCanTurn = false)
	{
		UPlatformTileData* Tile = NewObject<UPlatformTileData>(GetTransientPackage());
		Tile->Width = Width;
		Tile->Length = Length;
		Tile->bCanTurn = bCanTurn;
		Tile->AddToRoot();
		return Tile;
	}

	/** A room manager with the designer's tile sizes, overlay and pieces off, on a fixed seed. */
	ARoomManager* SpawnRoomManager(UWorld* World, TArray<UObject*>& OutRooted)
	{
		// Deferred, so its construction sees the tiles rather than warning that there are none.
		ARoomManager* Manager = World->SpawnActorDeferred<ARoomManager>(ARoomManager::StaticClass(), FTransform::Identity);
		Manager->bShowSurfaces = false;
		Manager->bShowPieces = false;
		Manager->bShowDebugGrid = false;
		Manager->bSnapToLandscapeHeight = false;
		Manager->PlatformRandomSeed = 1234;

		UPlatformTileData* Square = MakeTile(16, 16);
		UPlatformTileData* Small = MakeTile(8, 8);
		UPlatformTileData* Long = MakeTile(8, 16, true);
		OutRooted.Append({ Square, Small, Long });

		Manager->FirstTile = Square;
		Manager->Tiles = { Square, Small, Long };
		Manager->FinishSpawning(FTransform::Identity);
		Manager->ClearPlatforms();
		return Manager;
	}

	UDataTable* MakeCatalog(TArray<UObject*>& OutRooted, const double CellSize = 20.0)
	{
		UPlacementArchetype* Box = MakeArchetype(TEXT("DA_TestBox"));
		UPlacementArchetype* Bench = MakeArchetype(TEXT("DA_TestBench"));
		Bench->Rules.Add(MakeEdgeRule(TEXT("Back to wall"), { MakeContact(EObjectSide::Back, EContactTarget::Wall) }));
		Bench->SpawnWeight = 2.0f;

		UDataTable* Table = NewObject<UDataTable>(GetTransientPackage());
		Table->RowStruct = FPlacementCatalogRow::StaticStruct();
		Table->AddRow(TEXT("SM_TestBox"), MakeRow(Box, FIntVector(2, 2, 2), CellSize));
		Table->AddRow(TEXT("SM_TestBox_Tall"), MakeRow(Box, FIntVector(1, 1, 5), CellSize));
		Table->AddRow(TEXT("SM_TestBench"), MakeRow(Bench, FIntVector(1, 3, 2), CellSize));
		Table->AddToRoot();

		OutRooted.Append({ Box, Bench, Table });
		return Table;
	}

	AObjectPlacer* SpawnPlacer(UWorld* World, ARoomManager* Manager, UDataTable* Catalog)
	{
		AObjectPlacer* Placer = World->SpawnActor<AObjectPlacer>();
		Placer->RoomManager = Manager;
		Placer->Catalogs = { Catalog };
		Placer->bAutoPlace = false;
		Placer->bLogPlacements = false;
		Placer->RandomSeed = 99;
		Placer->ClearObjects();
		return Placer;
	}

	/** Every cell of a platform that is not already built, on its interior, a module edge's run, or a node. */
	void GatherOpenCells(const ARoomManager& Manager, const FGridPlatform& Platform, TArray<FIntVector>& OutInterior, TArray<FIntVector>& OutRun, TArray<FIntVector>& OutNodes)
	{
		const int32 Module = Manager.LatticeModule;
		const int32 Span = Platform.Tile->Width;
		const int32 SpanOther = Platform.Tile->Length;
		const int32 SpanU = Platform.bTurned ? SpanOther : Span;
		const int32 SpanV = Platform.bTurned ? Span : SpanOther;

		const int32 Normal = static_cast<int32>(Platform.Normal);
		const int32 U = Normal == 0 ? 1 : 0;
		const int32 V = Normal == 2 ? 1 : 2;

		for (int32 StepU = 0; StepU <= SpanU; ++StepU)
		{
			for (int32 StepV = 0; StepV <= SpanV; ++StepV)
			{
				FIntVector Cell = Platform.MinNode;
				Cell[U] += StepU;
				Cell[V] += StepV;

				if (Manager.IsCellSolid(Cell))
				{
					continue;
				}

				const int32 OnLines = (StepU % Module == 0 ? 1 : 0) + (StepV % Module == 0 ? 1 : 0);
				(OnLines == 0 ? OutInterior : OnLines == 1 ? OutRun : OutNodes).Add(Cell);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementObstacleCandidatesTest, "WEM.Placement.Obstacles.Candidates", WEMPlacementTests::TestFlags)

bool FPlacementObstacleCandidatesTest::RunTest(const FString& Parameters)
{
	using namespace PlacementIntegrationTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);

	for (int32 Beat = 0; Beat < 30; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	FRoomManagerTestAccess::EnsureCandidates(*Manager);
	const TArray<FGridPlatform> Listed = FRoomManagerTestAccess::GetListed(*Manager);

	if (!TestTrue(TEXT("Growth leaves candidates to cut"), Listed.Num() > 10))
	{
		Release(Rooted);
		return false;
	}

	// One obstacle into the inside of a module face, one onto a module edge's run, one onto a node,
	// each of a different listed candidate. Each of those candidates has to drop out.
	int32 Cut = 0;
	TSet<int32> Used;

	for (int32 Kind = 0; Kind < 3; ++Kind)
	{
		for (int32 Index = 0; Index < Listed.Num(); ++Index)
		{
			if (Used.Contains(Index))
			{
				continue;
			}

			TArray<FIntVector> Interior, Run, Nodes;
			GatherOpenCells(*Manager, Listed[Index], Interior, Run, Nodes);
			const TArray<FIntVector>& Cells = Kind == 0 ? Interior : Kind == 1 ? Run : Nodes;

			if (Cells.IsEmpty() || !FRoomManagerTestAccess::IsListed(*Manager, Listed[Index]))
			{
				continue;
			}

			Used.Add(Index);
			Manager->AddObstacle(Cells[0], Cells[0]);

			static const TCHAR* Names[3] = { TEXT("face"), TEXT("edge"), TEXT("node") };
			TestFalse(*FString::Printf(TEXT("A candidate cutting a reserved %s is no longer listed"), Names[Kind]), FRoomManagerTestAccess::IsListed(*Manager, Listed[Index]));
			TestFalse(*FString::Printf(TEXT("A candidate cutting a reserved %s can no longer be placed"), Names[Kind]), Manager->CanPlacePlatform(Listed[Index]));
			++Cut;
			break;
		}
	}

	TestEqual(TEXT("A face, an edge and a node were each reserved"), Cut, 3);

	// A box that only touches a candidate - the cell just past its rim in its own plane - leaves it listed.
	for (int32 Index = 0; Index < Listed.Num(); ++Index)
	{
		const FGridPlatform& Candidate = Listed[Index];

		if (Used.Contains(Index) || !FRoomManagerTestAccess::IsListed(*Manager, Candidate))
		{
			continue;
		}

		const int32 Normal = static_cast<int32>(Candidate.Normal);
		const int32 U = Normal == 0 ? 1 : 0;
		const int32 V = Normal == 2 ? 1 : 2;

		FIntVector Beside = Candidate.MinNode;
		Beside[U] -= 1;
		Beside[V] += 2;

		if (Beside[U] < 0 || Manager->IsCellSolid(Beside))
		{
			continue;
		}

		Manager->AddObstacle(Beside, Beside);
		TestTrue(TEXT("A candidate the obstacle only touches stays listed"), FRoomManagerTestAccess::IsListed(*Manager, Candidate));
		break;
	}

	// The lists kept one obstacle at a time match a full rebuild of them.
	TestEqual(TEXT("After adding obstacles, the incremental candidates match a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// Growth carries on around them and the two still agree.
	for (int32 Beat = 0; Beat < 20; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	TestEqual(TEXT("Growing around obstacles keeps the incremental state in step"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// Clearing them gives the room back through a fresh gather, which matches a rebuild as well.
	Manager->ClearObstacles();
	TestTrue(TEXT("Clearing obstacles leaves the candidates to be gathered afresh"), FRoomManagerTestAccess::IsStale(*Manager));
	FRoomManagerTestAccess::EnsureCandidates(*Manager);
	TestEqual(TEXT("After clearing obstacles, the regathered candidates match a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	Release(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementObstacleDriftTest, "WEM.Placement.Obstacles.VerifyEvery", WEMPlacementTests::TestFlags)

bool FPlacementObstacleDriftTest::RunTest(const FString& Parameters)
{
	using namespace PlacementIntegrationTestsPrivate;

	IConsoleVariable* VerifyEvery = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.RoomManager.VerifyEvery"));

	if (!TestNotNull(TEXT("The VerifyEvery console variable exists"), VerifyEvery))
	{
		return false;
	}

	const int32 Previous = VerifyEvery->GetInt();
	VerifyEvery->Set(1, ECVF_SetByCode);

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	AObjectPlacer* Placer = SpawnPlacer(TestWorld.World, Manager, MakeCatalog(Rooted));

	// Growth and furnishing interleaved, with every placement checked against a full rebuild.
	int32 PlacedObjects = 0;

	for (int32 Beat = 0; Beat < 150; ++Beat)
	{
		Manager->PlaceNextPlatform();

		if (Beat >= 5 && Placer->PlaceNextObject())
		{
			++PlacedObjects;
		}
	}

	VerifyEvery->Set(Previous, ECVF_SetByCode);

	TestTrue(TEXT("Objects were placed along the way"), PlacedObjects > 10);
	TestTrue(TEXT("Every placement was checked"), FRoomManagerTestAccess::GetVerifiedPlacements(*Manager) >= 140);
	TestEqual(TEXT("No check found drift"), FRoomManagerTestAccess::GetFailedVerifications(*Manager), 0);
	TestEqual(TEXT("Every object holds an obstacle"), Manager->Obstacles.Num(), PlacedObjects);

	// No platform grew through an object.
	bool bThrough = false;
	for (const FPlacedObject& Object : Placer->GetSolver()->GetObjects())
	{
		for (int32 X = Object.VolumeMin.X; X <= Object.VolumeMax.X; ++X)
		{
			for (int32 Y = Object.VolumeMin.Y; Y <= Object.VolumeMax.Y; ++Y)
			{
				for (int32 Z = Object.VolumeMin.Z; Z <= Object.VolumeMax.Z; ++Z)
				{
					bThrough = bThrough || Manager->IsCellSolid(FIntVector(X, Y, Z));
				}
			}
		}
	}

	TestFalse(TEXT("No segment grew through an object"), bThrough);

	// Starting the structure over takes the objects and their obstacles with it.
	Manager->ClearPlatforms();
	TestEqual(TEXT("A new platform list clears the obstacles"), Manager->Obstacles.Num(), 0);
	TestFalse(TEXT("Nothing is placed on an empty cube"), Placer->PlaceNextObject());
	TestTrue(TEXT("The placer drops its objects when the structure starts over"), Placer->GetSolver() && Placer->GetSolver()->GetObjects().IsEmpty());
	TestEqual(TEXT("And its log with them"), Placer->GetPlacementLog().Num(), 0);

	Release(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementDeterminismTest, "WEM.Placement.Determinism", WEMPlacementTests::TestFlags)

bool FPlacementDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace PlacementIntegrationTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	AObjectPlacer* Placer = SpawnPlacer(TestWorld.World, Manager, MakeCatalog(Rooted));

	// The same seeds for both, run twice: once loading every mesh on the spot, once in the background.
	auto Run = [&](const bool bLoadSynchronously)
	{
		Manager->ClearPlatforms();
		Placer->bLoadMeshesSynchronously = bLoadSynchronously;
		Placer->ClearObjects();

		for (int32 Beat = 0; Beat < 40; ++Beat)
		{
			Manager->PlaceNextPlatform();

			if (Beat >= 5)
			{
				Placer->PlaceNextObject();
			}
		}

		return Placer->GetPlacementLog();
	};

	const TArray<FString> Instant = Run(true);
	const TArray<FString> Background = Run(false);

	TestTrue(TEXT("Objects are placed"), Instant.Num() > 5);
	TestTrue(TEXT("The same seeds give the same placement log, however the meshes load"), Instant == Background);

	if (!Instant.IsEmpty())
	{
		AddInfo(Instant.Last());
	}

	Release(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementCellSizeTest, "WEM.Placement.Catalog.CellSizeMismatch", WEMPlacementTests::TestFlags)

bool FPlacementCellSizeTest::RunTest(const FString& Parameters)
{
	using namespace PlacementIntegrationTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);

	for (int32 Beat = 0; Beat < 10; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	AddExpectedError(TEXT("built at a cell size other than"), EAutomationExpectedErrorFlags::Contains, 1);

	AObjectPlacer* Placer = SpawnPlacer(TestWorld.World, Manager, MakeCatalog(Rooted, 10.0));
	TestFalse(TEXT("A catalog built at another cell size places nothing"), Placer->PlaceNextObject());
	TestFalse(TEXT("And is asked again without saying so again"), Placer->PlaceNextObject());
	TestNull(TEXT("The placer never starts"), Placer->GetSolver());

	Release(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementIncrementalPlanesTest, "WEM.Placement.Solver.IncrementalPlanes", WEMPlacementTests::TestFlags)

bool FPlacementIncrementalPlanesTest::RunTest(const FString& Parameters)
{
	using namespace PlacementIntegrationTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	UDataTable* Catalog = MakeCatalog(Rooted);

	// The bench keeps its back to a wall; this keeps its back to the edge of its support, so the
	// boundary seeds are followed as well as the wall ones.
	UPlacementArchetype* Ledge = MakeArchetype(TEXT("DA_TestLedge"));
	Ledge->Rules.Add(MakeEdgeRule(TEXT("Back to the edge"), { MakeContact(EObjectSide::Back, EContactTarget::SupportBoundary) }));
	Catalog->AddRow(TEXT("SM_TestLedge"), MakeRow(Ledge, FIntVector(1, 2, 1)));
	Rooted.Add(Ledge);

	AObjectPlacer* Placer = SpawnPlacer(TestWorld.World, Manager, Catalog);

	// Both beats in turn, as they run in a level, with the planes the placer has followed the
	// structure with checked every tenth against every surface gathered afresh.
	int32 Checks = 0;

	for (int32 Beat = 1; Beat <= 200; ++Beat)
	{
		Manager->PlaceNextPlatform();
		Placer->PlaceNextObject();

		const FPlacementSolver* Solver = Placer->GetSolver();

		if (Beat % 10 != 0 || !TestNotNull(TEXT("The placer has started"), Solver))
		{
			continue;
		}

		++Checks;
		TArray<FString> Problems;

		if (!Solver->VerifySegmentPlanes(Problems))
		{
			AddError(FString::Printf(TEXT("At %d platform(s) and %d object(s) the planes had drifted: %s"),
				Manager->GetPlatformCount(), Solver->GetObjects().Num(), *FString::Join(Problems, TEXT("; "))));
			break;
		}
	}

	TestEqual(TEXT("Every check ran"), Checks, 20);
	TestTrue(TEXT("The structure grew"), Manager->GetPlatformCount() > 50);
	TestTrue(TEXT("Objects were placed along the way"), Placer->GetSolver() && Placer->GetSolver()->GetObjects().Num() > 20);

	// Otherwise every refresh gathered the lot, and the checks above were of that alone.
	TArray<FGridSurfaceRef> LastChanges;
	TestTrue(TEXT("The room manager can say what its last placement changed"),
		Manager->GatherSurfaceChangesSince(Manager->GetStructureRevision() - 1, LastChanges) && !LastChanges.IsEmpty());

	Release(Rooted);
	return true;
}

#endif
