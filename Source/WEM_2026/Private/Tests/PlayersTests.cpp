// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"
#include "WEMPlayers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayersFramesTest, "WEM.Players.Frames", WEMRoomManagerTests::TestFlags)

bool FPlayersFramesTest::RunTest(const FString& Parameters)
{
	constexpr float Ratio = 9.0f / 16.0f;

	// Wide, about square and narrow windows, and the editor's play view seen on 2026-10-19.
	const FVector2D Sizes[] = { FVector2D(1920, 1080), FVector2D(2560, 1080), FVector2D(2713, 1827), FVector2D(1200, 1000), FVector2D(700, 1000) };

	for (const FVector2D& Size : Sizes)
	{
		const TArray<FBox2D> Frames = WEMPlayers::GetScreenFrames(Size, 2, Ratio);
		const FString Window = FString::Printf(TEXT("%.0f x %.0f"), Size.X, Size.Y);

		if (!TestEqual(*(Window + TEXT(": two frames")), Frames.Num(), 2))
		{
			return false;
		}

		const FBox2D Left(Frames[0].Min * Size, Frames[0].Max * Size);
		const FBox2D Right(Frames[1].Min * Size, Frames[1].Max * Size);

		TestEqual(*(Window + TEXT(": the left one is 9:16")), Left.GetSize().X / Left.GetSize().Y, (double)Ratio, 1.0e-6);
		TestEqual(*(Window + TEXT(": the right one too")), Right.GetSize().X / Right.GetSize().Y, (double)Ratio, 1.0e-6);
		TestTrue(*(Window + TEXT(": the same size")), Left.GetSize().Equals(Right.GetSize(), 1.0e-6));
		TestTrue(*(Window + TEXT(": player 1 on the left")), Left.Max.X < Right.Min.X);

		// Even gaps: before, between and after.
		const double Before = Left.Min.X;
		const double Between = Right.Min.X - Left.Max.X;
		const double After = Size.X - Right.Max.X;
		TestEqual(*(Window + TEXT(": the gap between is the gap before")), Between, Before, 1.0e-6);
		TestEqual(*(Window + TEXT(": and the gap after")), After, Before, 1.0e-6);
		TestTrue(*(Window + TEXT(": the gaps are there")), Before >= WEMPlayers::MinGapShare * Size.X - 1.0e-6);

		// As tall as the window when there is room, else centred up and down.
		TestEqual(*(Window + TEXT(": centred up and down")), Left.Min.Y, Size.Y - Left.Max.Y, 1.0e-6);
		if (2.0 * Size.Y * Ratio + 3.0 * WEMPlayers::MinGapShare * Size.X <= Size.X)
		{
			TestEqual(*(Window + TEXT(": full height")), Left.GetSize().Y, Size.Y, 1.0e-6);
		}
		TestTrue(*(Window + TEXT(": inside the window")), Left.Min.Y >= -1.0e-6 && Left.Max.Y <= Size.Y + 1.0e-6);
	}

	// On a 16:9 window: frames about 32 % of the width, gaps about 12 %.
	const TArray<FBox2D> Wide = WEMPlayers::GetScreenFrames(FVector2D(1920, 1080), 2, Ratio);
	AddInfo(FString::Printf(TEXT("16:9: frames %.3f of the width, gaps %.3f"), Wide[0].GetSize().X, Wide[0].Min.X));
	TestEqual(TEXT("16:9 frame width"), Wide[0].GetSize().X, 1080.0 * Ratio / 1920.0, 1.0e-6);

	TestTrue(TEXT("No frames for nobody"), WEMPlayers::GetScreenFrames(FVector2D(1920, 1080), 0, Ratio).IsEmpty());
	TestTrue(TEXT("No frames for an empty window"), WEMPlayers::GetScreenFrames(FVector2D(0, 0), 2, Ratio).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayersStartsTest, "WEM.Players.Starts", WEMRoomManagerTests::TestFlags)

bool FPlayersStartsTest::RunTest(const FString& Parameters)
{
	// The cube in NewMap: 509 cells of 20 cm from the origin.
	const FBox Cube(FVector::ZeroVector, FVector(10180.0));
	const FVector Centre = Cube.GetCenter();
	constexpr double Distance = 1000.0;

	const FTransform First = WEMPlayers::GetStart(Cube, 0, 2, Distance, 0.5);
	const FTransform Second = WEMPlayers::GetStart(Cube, 1, 2, Distance, 0.5);

	TestEqual(TEXT("Player 1 starts Distance before the -X face"), First.GetLocation(), FVector(Cube.Min.X - Distance, Centre.Y, Centre.Z), 0.01);
	TestEqual(TEXT("Player 2 before the opposite face"), Second.GetLocation(), FVector(Cube.Max.X + Distance, Centre.Y, Centre.Z), 0.01);

	// Each looks level at the cube's middle.
	for (const FTransform& Start : { First, Second })
	{
		const FVector Forward = Start.GetRotation().GetForwardVector();
		const FVector ToCentre = (Centre - Start.GetLocation()).GetSafeNormal();
		TestEqual(TEXT("Facing the cube's middle"), FVector::DotProduct(Forward, ToCentre), 1.0, 1.0e-6);
		TestEqual(TEXT("Level"), Start.Rotator().Pitch, 0.0, 1.0e-6);
		TestEqual(TEXT("Upright"), Start.Rotator().Roll, 0.0, 1.0e-6);
	}

	// Height is a share of the cube's: 0 its bottom, 1 its top.
	TestEqual(TEXT("At the bottom"), WEMPlayers::GetStart(Cube, 0, 2, Distance, 0.0).GetLocation().Z, Cube.Min.Z, 0.01);
	TestEqual(TEXT("At the top"), WEMPlayers::GetStart(Cube, 1, 2, Distance, 1.0).GetLocation().Z, Cube.Max.Z, 0.01);

	// A negative distance starts them inside the cube, still facing the middle.
	TestEqual(TEXT("Inside"), WEMPlayers::GetStart(Cube, 0, 2, -2000.0, 0.5).GetLocation().X, Cube.Min.X + 2000.0, 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayersShowOnlyToTest, "WEM.Players.ShowOnlyTo", WEMRoomManagerTests::TestFlags)

bool FPlayersShowOnlyToTest::RunTest(const FString& Parameters)
{
	WEMRoomManagerTests::FScopedTestWorld TestWorld;

	APlayerController* Left = TestWorld.World->SpawnActor<APlayerController>();
	APlayerController* Right = TestWorld.World->SpawnActor<APlayerController>();
	AActor* Holder = TestWorld.World->SpawnActor<AActor>();

	if (!TestNotNull(TEXT("Players"), Left) || !TestNotNull(TEXT("Players"), Right) || !TestNotNull(TEXT("An actor"), Holder))
	{
		return false;
	}

	UStaticMeshComponent* ForLeft = NewObject<UStaticMeshComponent>(Holder);
	UStaticMeshComponent* ForRight = NewObject<UStaticMeshComponent>(Holder);

	TArray<FWEMViewer> Viewers;
	Viewers.AddDefaulted(2);
	Viewers[0].Player = Left;
	Viewers[1].Player = Right;

	WEMPlayers::ShowOnlyTo(ForLeft, Viewers, 0);
	WEMPlayers::ShowOnlyTo(ForRight, Viewers, 1);

	TestFalse(TEXT("The left player sees their own"), Left->HiddenPrimitiveComponents.Contains(ForLeft));
	TestTrue(TEXT("But not the right player's"), Left->HiddenPrimitiveComponents.Contains(ForRight));
	TestFalse(TEXT("The right player sees their own"), Right->HiddenPrimitiveComponents.Contains(ForRight));
	TestTrue(TEXT("But not the left player's"), Right->HiddenPrimitiveComponents.Contains(ForLeft));

	// Shown to the other one instead: the hiding follows, and nothing is listed twice.
	WEMPlayers::ShowOnlyTo(ForLeft, Viewers, 1);
	WEMPlayers::ShowOnlyTo(ForLeft, Viewers, 1);
	TestFalse(TEXT("Now the right player sees it"), Right->HiddenPrimitiveComponents.Contains(ForLeft));
	TestEqual(TEXT("And the left player does not, listed once"), Left->HiddenPrimitiveComponents.FilterByPredicate(
		[ForLeft](const TWeakObjectPtr<UPrimitiveComponent>& Each) { return Each.Get() == ForLeft; }).Num(), 1);

	// No players at all (Simulate): nothing to hide from.
	TArray<FWEMViewer> Editor;
	Editor.AddDefaulted();
	WEMPlayers::ShowOnlyTo(ForRight, Editor, 0);

	TestWorld.World->DestroyActor(Holder);
	TestWorld.World->DestroyActor(Left);
	TestWorld.World->DestroyActor(Right);
	return true;
}

#endif
