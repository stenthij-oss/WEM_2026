// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Count.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace PathTestsPrivate
{
	using namespace WEMRoomManagerTests;
	using FPath = FRoomManagerTestAccess::FPath;

	ARoomManager* SpawnWithPaths(UWorld* World, TArray<UObject*>& Rooted, const int32 Count, const int32 Width)
	{
		ARoomManager* Manager = SpawnRoomManager(World, Rooted);
		Manager->GridSize = 129;
		Manager->PathCount = Count;
		Manager->PathWidth = Width;
		Manager->ClearPlatforms();
		return Manager;
	}

	bool SameCorners(const TArray<FPath>& A, const TArray<FPath>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}

		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index].Corners != B[Index].Corners)
			{
				return false;
			}
		}

		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathsDrawTest, "WEM.Paths.Draw", WEMRoomManagerTests::TestFlags)

bool FPathsDrawTest::RunTest(const FString& Parameters)
{
	using namespace PathTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnWithPaths(TestWorld.World, Rooted, 10, 2);
	Manager->MinPathLegs = 3;
	Manager->MaxPathLegs = 5;
	Manager->ClearPlatforms();

	const TArray<FPath> Paths = FRoomManagerTestAccess::GetPaths(*Manager);
	AddInfo(Manager->PathSummary);

	TestEqual(TEXT("Ten paths are drawn"), Paths.Num(), 10);

	const int32 LastCell = Manager->GridSize - 1;
	const int32 Module = Manager->LatticeModule;

	for (const FPath& Path : Paths)
	{
		TestTrue(TEXT("Every path has a leg"), Path.LegMins.Num() >= 1 && Path.LegMins.Num() <= 5);
		TestEqual(TEXT("A corner closes every leg"), Path.Corners.Num(), Path.LegMins.Num() + 1);

		int32 PreviousAxis = INDEX_NONE;

		for (int32 Leg = 0; Leg + 1 < Path.Corners.Num(); ++Leg)
		{
			const FIntVector Step = Path.Corners[Leg + 1] - Path.Corners[Leg];
			const int32 Moved = (Step.X != 0) + (Step.Y != 0) + (Step.Z != 0);
			TestEqual(TEXT("A leg runs straight along one axis"), Moved, 1);

			const int32 Axis = Step.X != 0 ? 0 : (Step.Y != 0 ? 1 : 2);
			TestTrue(TEXT("Every corner turns"), Axis != PreviousAxis);
			TestEqual(TEXT("Legs are whole modules long"), FMath::Abs(Step[Axis]) % Module, 0);
			PreviousAxis = Axis;

			for (int32 Each = 0; Each < 3; ++Each)
			{
				TestTrue(TEXT("Corners sit at a module's middle"), Path.Corners[Leg][Each] % Module == Module / 2);
				TestTrue(TEXT("Legs stay in the cube"), Path.LegMins[Leg][Each] >= 0 && Path.LegMaxes[Leg][Each] <= LastCell);
			}
		}
	}

	// Thinner than a module, a path only reaches the planes it crosses: faces across its legs, no edges or nodes.
	int32 Faces, Edges, Nodes;
	FRoomManagerTestAccess::GetPathPieceCounts(*Manager, Faces, Edges, Nodes);
	TestTrue(TEXT("A thin path holds faces back"), Faces > 0);
	TestEqual(TEXT("But no edges"), Edges, 0);
	TestEqual(TEXT("And no nodes"), Nodes, 0);

	// Its routes follow the seed and the draw: drawn again the same, they come out the same; thicker, on the same routes.
	Manager->PathWidth = 4;
	Manager->OnConstruction(Manager->GetActorTransform());
	TestTrue(TEXT("A change of width keeps the routes"), SameCorners(Paths, FRoomManagerTestAccess::GetPaths(*Manager)));

	FRoomManagerTestAccess::GetPathPieceCounts(*Manager, Faces, Edges, Nodes);
	TestTrue(TEXT("A module thick, a path reaches the planes along it too"), Edges > 0);

	Manager->RerollPaths();
	TestFalse(TEXT("A redraw moves them"), SameCorners(Paths, FRoomManagerTestAccess::GetPaths(*Manager)));

	// Play moves them one at a time: one path drawn afresh leaves every other where it is.
	const TArray<FPath> BeforeOne = FRoomManagerTestAccess::GetPaths(*Manager);
	FRoomManagerTestAccess::RedrawPath(*Manager, 3);
	const TArray<FPath> AfterOne = FRoomManagerTestAccess::GetPaths(*Manager);

	TestEqual(TEXT("A single redraw keeps the count"), AfterOne.Num(), BeforeOne.Num());
	TestTrue(TEXT("It moves the one path"), AfterOne[3].Corners != BeforeOne[3].Corners);

	bool bOthersStay = true;
	for (int32 Index = 0; Index < AfterOne.Num(); ++Index)
	{
		bOthersStay = bOthersStay && (Index == 3 || AfterOne[Index].Corners == BeforeOne[Index].Corners);
	}

	TestTrue(TEXT("And no other"), bOthersStay);

	// They fall due one after another across the interval, never together.
	const TArray<double> Due = FRoomManagerTestAccess::GetPathRedrawTimes(*Manager);
	TArray<double> Sorted = Due;
	Sorted.RemoveAt(3);
	Sorted.Sort();

	bool bSpread = true;
	for (int32 Index = 1; Index < Sorted.Num(); ++Index)
	{
		bSpread = bSpread && Sorted[Index] > Sorted[Index - 1];
	}

	TestTrue(TEXT("Each falls due at its own time"), bSpread);
	TestTrue(TEXT("All within the interval"), Sorted.Last() - Sorted[0] <= Manager->PathRerollInterval);

	// Switched off, none are drawn - by a redraw or by any edit - and switched on, they come back.
	Manager->bEnablePaths = false;
	Manager->RerollPaths();
	TestEqual(TEXT("None while switched off"), FRoomManagerTestAccess::GetPaths(*Manager).Num(), 0);

	Manager->OnConstruction(Manager->GetActorTransform());
	FRoomManagerTestAccess::GetPathPieceCounts(*Manager, Faces, Edges, Nodes);
	TestEqual(TEXT("Nor after an edit, nor anything held back"), Faces + Edges + Nodes, 0);

	Manager->bEnablePaths = true;
	Manager->RerollPaths();
	TestEqual(TEXT("Back when switched on"), FRoomManagerTestAccess::GetPaths(*Manager).Num(), 10);

	Manager->PathCount = 0;
	Manager->RerollPaths();
	TestEqual(TEXT("None at zero"), FRoomManagerTestAccess::GetPaths(*Manager).Num(), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathsHoldBackTest, "WEM.Paths.HoldBack", WEMRoomManagerTests::TestFlags)

bool FPathsHoldBackTest::RunTest(const FString& Parameters)
{
	using namespace PathTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	// Wide paths, many of them, so growth keeps running into them.
	ARoomManager* Manager = SpawnWithPaths(TestWorld.World, Rooted, 40, 4);

	auto CountInPaths = [&Manager]()
	{
		const TArray<FGridPlatform>& Built = FRoomManagerTestAccess::GetBuilt(*Manager);
		int32 Count = 0;

		// The first platform starts growth wherever paths run.
		for (int32 Index = 1; Index < Built.Num(); ++Index)
		{
			Count += FRoomManagerTestAccess::IsClearOfPaths(*Manager, Built[Index]) ? 0 : 1;
		}

		return Count;
	};

	for (int32 Beat = 0; Beat < 400; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	const int32 Grown = Manager->GetPlatformCount();
	AddInfo(FString::Printf(TEXT("%d platforms; %s"), Grown, *Manager->PathSummary));
	TestTrue(TEXT("Growth goes on round the paths"), Grown > 200);
	TestEqual(TEXT("Nothing is placed across a path"), CountInPaths(), 0);

	// Redrawn, the paths run through what stands, and every platform stays - through a rebuild too.
	int32 RunThrough = 0;

	// The lists are refreshed where paths were and are, not gathered afresh: they have to come out as a full gather would.
	FRoomManagerTestAccess::EnsureCandidates(*Manager);

	for (int32 Draw = 0; Draw < 8 && RunThrough == 0; ++Draw)
	{
		Manager->RerollPaths();
		TestFalse(TEXT("A redraw keeps the lists"), FRoomManagerTestAccess::IsStale(*Manager));
		TestEqual(TEXT("And they match a full gather"), FRoomManagerTestAccess::Verify(*Manager), 0);
		RunThrough = CountInPaths();
	}

	AddInfo(Manager->PathSummary);
	TestTrue(TEXT("A redrawn path runs through platforms standing"), RunThrough > 0);
	TestEqual(TEXT("They stand"), Manager->GetPlatformCount(), Grown);

	FRoomManagerTestAccess::Rebuild(*Manager, Manager->Platforms);
	TestEqual(TEXT("And the rebuild keeps them"), Manager->GetPlatformCount(), Grown);
	TestEqual(TEXT("With the same paths"), CountInPaths(), RunThrough);

	// What grows from here keeps clear of the new paths.
	for (int32 Beat = 0; Beat < 200; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	TestTrue(TEXT("Growth carries on"), Manager->GetPlatformCount() > Grown);
	TestEqual(TEXT("Only what stood before is in a path"), CountInPaths(), RunThrough);

	// Each path drawn afresh on its own, as play does, keeps the lists as a full gather would have them, and the
	// paths find the platforms standing in them as the placements are asked.
	FRoomManagerTestAccess::EnsureCandidates(*Manager);

	for (int32 Index = 0; Index < 40; Index += 4)
	{
		FRoomManagerTestAccess::RedrawPath(*Manager, Index);
		TestFalse(TEXT("A single redraw keeps the lists"), FRoomManagerTestAccess::IsStale(*Manager));
	}

	TestEqual(TEXT("And they match a full gather"), FRoomManagerTestAccess::Verify(*Manager), 0);

	const TArray<FGridPlatform>& Built = FRoomManagerTestAccess::GetBuilt(*Manager);
	const int32 StandingAll = Algo::CountIf(Built, [&Manager](const FGridPlatform& Platform)
	{
		return !FRoomManagerTestAccess::IsClearOfPaths(*Manager, Platform);
	});

	TestEqual(TEXT("The paths find every platform standing in them"), FRoomManagerTestAccess::GetPathStandingCount(*Manager), StandingAll);

	FRoomManagerTestAccess::EnsureCandidates(*Manager);
	const TArray<FGridPlatform> Listed = FRoomManagerTestAccess::GetListed(*Manager);
	TestFalse(TEXT("No listed placement crosses a path"), Listed.ContainsByPredicate([&Manager](const FGridPlatform& Platform)
	{
		return !FRoomManagerTestAccess::IsClearOfPaths(*Manager, Platform);
	}));

	// Switched off mid-growth, the placements the paths held back come free, as a full gather would have them.
	Manager->bEnablePaths = false;
	Manager->RerollPaths();
	TestEqual(TEXT("Switching off matches a full gather"), FRoomManagerTestAccess::Verify(*Manager), 0);
	Manager->bEnablePaths = true;

	// And thin paths, refreshed the same way.
	Manager->PathWidth = 2;
	Manager->RerollPaths();
	TestEqual(TEXT("A thin redraw matches a full gather too"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPathsPerfTest, "WEM.Paths.Perf", WEMRoomManagerTests::SlowTestFlags)

bool FPathsPerfTest::RunTest(const FString& Parameters)
{
	using namespace PathTestsPrivate;

	// A NewMap-sized cube grown to 3000 platforms with 162 paths, as NewMap was calibrated on 2026-10-15. Play moves
	// one path at a time: 162 over 30 s is one every 0.19 s, about every 17th beat of NewMap's 0.0107 s city. Each
	// such redraw is timed, and at the end every path at once, as the paths moved before.
	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 509;
	Manager->PlatformRandomSeed = 4242;
	Manager->PathCount = 162;
	Manager->ClearPlatforms();

	double LongestBeat = 0.0;
	double BeatTotal = 0.0;
	int32 Beats = 0;

	double LongestRedraw = 0.0;
	double RedrawTotal = 0.0;
	int32 Redraws = 0;
	int32 NextPath = 0;

	while (Manager->GetPlatformCount() < 3000)
	{
		const double Start = FPlatformTime::Seconds();

		if (!Manager->PlaceNextPlatform())
		{
			break;
		}

		const double Beat = FPlatformTime::Seconds() - Start;
		LongestBeat = FMath::Max(LongestBeat, Beat);
		BeatTotal += Beat;
		++Beats;

		if (Beats % 17 == 0)
		{
			const double RedrawStart = FPlatformTime::Seconds();
			FRoomManagerTestAccess::RedrawPath(*Manager, NextPath);
			const double Redraw = FPlatformTime::Seconds() - RedrawStart;

			NextPath = (NextPath + 1) % Manager->PathCount;

			if (Manager->GetPlatformCount() >= 1000)
			{
				LongestRedraw = FMath::Max(LongestRedraw, Redraw);
				RedrawTotal += Redraw;
				++Redraws;
			}
		}
	}

	TestFalse(TEXT("Single redraws never leave the lists to be gathered afresh"), FRoomManagerTestAccess::IsStale(*Manager));

	const double AllStart = FPlatformTime::Seconds();
	Manager->RerollPaths();
	const double All = FPlatformTime::Seconds() - AllStart;

	AddInfo(FString::Printf(TEXT("%d platforms, beats %.2f ms mean, %.1f ms longest. %s"),
		Manager->GetPlatformCount(), 1000.0 * BeatTotal / FMath::Max(Beats, 1), 1000.0 * LongestBeat, *Manager->PathSummary));
	AddInfo(FString::Printf(TEXT("One path at a time, after 1000 platforms: %d redraws, %.2f ms mean, %.1f ms longest. Every path at once: %.1f ms."),
		Redraws, 1000.0 * RedrawTotal / FMath::Max(Redraws, 1), 1000.0 * LongestRedraw, 1000.0 * All));

	ReleaseRooted(Rooted);
	return true;
}

#endif
