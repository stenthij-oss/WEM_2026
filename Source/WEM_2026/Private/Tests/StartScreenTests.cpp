// Copyright Epic Games, Inc. All Rights Reserved.

#include "RoomManagerTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "SkyGlitchTitle.h"
#include "StartScreen.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStartScreenTitleTest, "WEM.StartScreen.Title", WEMRoomManagerTests::TestFlags)

bool FStartScreenTitleTest::RunTest(const FString& Parameters)
{
	// Lit evenly over the seconds asked, fully lit from then on, dark before the music.
	TestEqual(TEXT("dark at the start"), WEMStartScreen::GetTitleOpacity(0.0, 0.79f), 0.0f);
	TestEqual(TEXT("half lit half way"), WEMStartScreen::GetTitleOpacity(0.395, 0.79f), 0.5f, 1e-4f);
	TestEqual(TEXT("fully lit at 0.79 s"), WEMStartScreen::GetTitleOpacity(0.79, 0.79f), 1.0f);
	TestEqual(TEXT("stays lit"), WEMStartScreen::GetTitleOpacity(5.0, 0.79f), 1.0f);
	TestEqual(TEXT("dark before the music"), WEMStartScreen::GetTitleOpacity(-0.1, 0.79f), 0.0f);
	TestEqual(TEXT("lit at once with no seconds"), WEMStartScreen::GetTitleOpacity(0.0, 0.0f), 1.0f);

	// The title as wide as asked where that leaves it short enough, and held to its tallest where not, centred either way.
	const FBox2f Wide = SkyGlitchTitle::Fit(FIntPoint(1920, 1080), FIntPoint(4096, 1024), 0.8f, 0.5f);
	TestEqual(TEXT("wide: width"), Wide.GetSize().X, 1536.0f, 0.01f);
	TestEqual(TEXT("wide: height"), Wide.GetSize().Y, 384.0f, 0.01f);
	TestEqual(TEXT("wide: centred"), Wide.GetCenter(), FVector2f(960.0f, 540.0f));

	const FBox2f Capped = SkyGlitchTitle::Fit(FIntPoint(1920, 1080), FIntPoint(4096, 1024), 0.8f, 0.3f);
	TestEqual(TEXT("capped: height"), Capped.GetSize().Y, 324.0f, 0.01f);
	TestEqual(TEXT("capped: width"), Capped.GetSize().X, 1296.0f, 0.01f);

	const FBox2f Tall = SkyGlitchTitle::Fit(FIntPoint(1000, 1000), FIntPoint(1000, 1000), 0.8f, 0.3f);
	TestEqual(TEXT("held to its tallest: height"), Tall.GetSize().Y, 300.0f, 0.01f);
	TestEqual(TEXT("held to its tallest: width"), Tall.GetSize().X, 300.0f, 0.01f);
	TestEqual(TEXT("held to its tallest: centred"), Tall.GetCenter(), FVector2f(500.0f, 500.0f));

	TestFalse(TEXT("nothing to fit"), SkyGlitchTitle::Fit(FIntPoint(1920, 1080), FIntPoint::ZeroValue, 0.8f, 0.3f).bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStartScreenPromptTest, "WEM.StartScreen.Prompt", WEMRoomManagerTests::TestFlags)

bool FStartScreenPromptTest::RunTest(const FString& Parameters)
{
	// On for 0.6 s, off for 0.4 s, over and over.
	TestTrue(TEXT("on at first"), WEMStartScreen::IsPromptShown(0.0, 0.6f, 0.4f));
	TestTrue(TEXT("still on"), WEMStartScreen::IsPromptShown(0.59, 0.6f, 0.4f));
	TestFalse(TEXT("off"), WEMStartScreen::IsPromptShown(0.61, 0.6f, 0.4f));
	TestFalse(TEXT("still off"), WEMStartScreen::IsPromptShown(0.99, 0.6f, 0.4f));
	TestTrue(TEXT("on again"), WEMStartScreen::IsPromptShown(1.01, 0.6f, 0.4f));
	TestTrue(TEXT("on again much later"), WEMStartScreen::IsPromptShown(100.3, 0.6f, 0.4f));
	TestFalse(TEXT("off again much later"), WEMStartScreen::IsPromptShown(100.7, 0.6f, 0.4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStartScreenHoldTest, "WEM.StartScreen.Hold", WEMRoomManagerTests::TestFlags)

bool FStartScreenHoldTest::RunTest(const FString& Parameters)
{
	using namespace WEMRoomManagerTests;

	TArray<UObject*> Rooted;

	// Held, it begins play without growing; started, it grows; started again, nothing more happens.
	{
		FScopedTestWorld TestWorld;
		ARoomManager* Held = SpawnRoomManager(TestWorld.World, Rooted);
		Held->HoldGrowth();
		Held->DispatchBeginPlay();
		TestFalse(TEXT("held: not growing after BeginPlay"), Held->HasGrowthStarted());

		Held->StartGrowth();
		TestTrue(TEXT("held: growing once started"), Held->HasGrowthStarted());
		Held->StartGrowth();
		TestTrue(TEXT("held: starting twice is harmless"), Held->HasGrowthStarted());
	}

	// Begun after a start screen - loaded later, with its part of the world - it finds the start screen and waits all the same.
	{
		FScopedTestWorld TestWorld;
		AStartScreen* Screen = TestWorld.World->SpawnActor<AStartScreen>();
		ARoomManager* Late = SpawnRoomManager(TestWorld.World, Rooted);
		TestTrue(TEXT("late: the start screen holds it"), Screen && Screen->HoldsGrowth(Late));
		Late->DispatchBeginPlay();
		TestFalse(TEXT("late: not growing after BeginPlay"), Late->HasGrowthStarted());
	}

	// Not held, BeginPlay starts it as it always did. A world of its own, as a room manager expects to be alone.
	{
		FScopedTestWorld TestWorld;
		ARoomManager* Free = SpawnRoomManager(TestWorld.World, Rooted);
		Free->DispatchBeginPlay();
		TestTrue(TEXT("not held: growing from BeginPlay"), Free->HasGrowthStarted());
	}

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStartScreenUnseenTest, "WEM.StartScreen.Unseen", WEMRoomManagerTests::TestFlags)

bool FStartScreenUnseenTest::RunTest(const FString& Parameters)
{
	using namespace WEMRoomManagerTests;

	// A test has no music: the start screen says so and keeps to the clock.
	AddExpectedError(TEXT("no music to play"), EAutomationExpectedErrorFlags::Contains, 1);

	// From BeginPlay until the title is fully lit, every player's drone is unseen, one that comes later too.
	{
		FScopedTestWorld TestWorld;
		UWorld* World = TestWorld.World;
		AStartScreen* Screen = World->SpawnActor<AStartScreen>();
		Screen->TitleSeconds = 0.0f;

		// A test world's actors are never initialised for play, so a controller is listed with the world by hand.
		APlayerController* First = World->SpawnActor<APlayerController>();
		World->AddController(First);
		APawn* FirstDrone = World->SpawnActor<APawn>();
		First->Possess(FirstDrone);

		Screen->DispatchBeginPlay();
		TestTrue(TEXT("waiting: unseen"), FirstDrone->IsHidden());

		APlayerController* Second = World->SpawnActor<APlayerController>();
		World->AddController(Second);
		APawn* SecondDrone = World->SpawnActor<APawn>();
		Second->Possess(SecondDrone);
		Screen->Tick(0.0f);
		TestTrue(TEXT("waiting: a drone that came later is unseen too"), SecondDrone->IsHidden());

		Screen->StartRun();
		TestTrue(TEXT("title lighting up: still unseen"), FirstDrone->IsHidden());

		// With no seconds to light up over, the first tick of the title lights it fully.
		Screen->Tick(0.0f);
		TestTrue(TEXT("title lit: playing"), Screen->Stage == EStartScreenStage::Playing);
		TestFalse(TEXT("title lit: seen"), FirstDrone->IsHidden());
		TestFalse(TEXT("title lit: the later one seen too"), SecondDrone->IsHidden());
	}

	// With the start screen off, the drones are seen from the start.
	{
		FScopedTestWorld TestWorld;
		AStartScreen* Screen = TestWorld.World->SpawnActor<AStartScreen>();
		Screen->bShowStartScreen = false;

		APlayerController* Player = TestWorld.World->SpawnActor<APlayerController>();
		TestWorld.World->AddController(Player);
		APawn* Drone = TestWorld.World->SpawnActor<APawn>();
		Player->Possess(Drone);

		Screen->DispatchBeginPlay();
		TestFalse(TEXT("off: seen"), Drone->IsHidden());
	}

	// The title the test lit and burnt in is no game's.
	SkyGlitchTitle::Reset();
	return true;
}

#endif
