// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/InstancedStaticMeshComponent.h"
#include "EmojiComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace EmojiTestsPrivate
{
	constexpr float Frame = 1.0f / 60.0f;

	/** A stick leaning this far, 0 to 1, this many degrees clockwise from straight up. */
	FVector2D Lean(const double Degrees, const double Amount = 1.0)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		return Amount * FVector2D(FMath::Sin(Radians), FMath::Cos(Radians));
	}

	UEmojiComponent* SpawnEmoji(UWorld* World)
	{
		AActor* Drone = World->SpawnActor<AActor>();
		UEmojiComponent* Emoji = NewObject<UEmojiComponent>(Drone);
		Drone->SetRootComponent(Emoji);
		Emoji->RegisterComponent();
		return Emoji;
	}

	/** Runs the menu and everything it sends for Seconds, a frame at a time, as the tick does. */
	void Play(UEmojiComponent* Emoji, const bool bHeld, const FVector2D& Stick, const float Seconds)
	{
		for (float Time = 0.0f; Time < Seconds - 0.5f * Frame; Time += Frame)
		{
			Emoji->UpdateMenu(bHeld, Stick, Frame);
			Emoji->Advance(Frame);
		}
	}

	float MeanHeight(const FEmojiScreenState& Screen)
	{
		float Sum = 0.0f;
		for (const FEmojiScreenSprite& Sprite : Screen.Sprites)
		{
			Sum += Sprite.Centre.Y;
		}
		return Screen.Sprites.IsEmpty() ? 0.0f : Sum / Screen.Sprites.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEmojiPickTest, "WEM.Emoji.Pick", WEMRoomManagerTests::TestFlags)

bool FEmojiPickTest::RunTest(const FString& Parameters)
{
	using namespace EmojiTestsPrivate;

	// Nine round the ring, 40 degrees apart, the first straight up.
	for (int32 Index = 0; Index < 9; ++Index)
	{
		TestEqual(FString::Printf(TEXT("Leaning at %d degrees picks emoji %d"), Index * 40, Index), UEmojiComponent::PickEmoji(Lean(Index * 40.0), 9, 0.5f, INDEX_NONE), Index);
	}

	TestEqual(TEXT("Just past the edge of the first slice picks the second"), UEmojiComponent::PickEmoji(Lean(21.0), 9, 0.5f, INDEX_NONE), 1);
	TestEqual(TEXT("Just short of it picks the first"), UEmojiComponent::PickEmoji(Lean(19.0), 9, 0.5f, INDEX_NONE), 0);
	TestEqual(TEXT("Leaning a little left of up picks the first"), UEmojiComponent::PickEmoji(Lean(-15.0), 9, 0.5f, INDEX_NONE), 0);

	// Inside the dead zone the stick picks nothing new.
	TestEqual(TEXT("A slight lean picks nothing"), UEmojiComponent::PickEmoji(Lean(90.0, 0.3), 9, 0.5f, INDEX_NONE), INDEX_NONE);
	TestEqual(TEXT("A slight lean keeps the pick"), UEmojiComponent::PickEmoji(Lean(90.0, 0.3), 9, 0.5f, 4), 4);
	TestEqual(TEXT("A centred stick keeps the pick"), UEmojiComponent::PickEmoji(FVector2D::ZeroVector, 9, 0.5f, 6), 6);

	// A pick holds a little past the edge of its slice, either way round, across the top too.
	TestEqual(TEXT("24 degrees keeps the first"), UEmojiComponent::PickEmoji(Lean(24.0), 9, 0.5f, 0), 0);
	TestEqual(TEXT("28 degrees moves on to the second"), UEmojiComponent::PickEmoji(Lean(28.0), 9, 0.5f, 0), 1);
	TestEqual(TEXT("-24 degrees keeps the first"), UEmojiComponent::PickEmoji(Lean(-24.0), 9, 0.5f, 0), 0);
	TestEqual(TEXT("-28 degrees moves on to the last"), UEmojiComponent::PickEmoji(Lean(-28.0), 9, 0.5f, 0), 8);
	TestEqual(TEXT("Straight down from the first moves on"), UEmojiComponent::PickEmoji(Lean(170.0), 9, 0.5f, 0), 4);

	TestEqual(TEXT("No emoji, no pick"), UEmojiComponent::PickEmoji(Lean(0.0), 0, 0.5f, INDEX_NONE), INDEX_NONE);
	TestEqual(TEXT("One emoji is picked whichever way"), UEmojiComponent::PickEmoji(Lean(200.0), 1, 0.5f, INDEX_NONE), 0);
	TestEqual(TEXT("A pick past the end is not kept"), UEmojiComponent::PickEmoji(FVector2D::ZeroVector, 9, 0.5f, 12), INDEX_NONE);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEmojiMenuTest, "WEM.Emoji.Menu", WEMRoomManagerTests::TestFlags)

bool FEmojiMenuTest::RunTest(const FString& Parameters)
{
	using namespace EmojiTestsPrivate;

	WEMRoomManagerTests::FScopedTestWorld TestWorld;
	UEmojiComponent* Emoji = SpawnEmoji(TestWorld.World);

	if (!TestEqual(TEXT("It starts with the nine emoji in /Game/Emoji"), Emoji->Emoji.Num(), 9))
	{
		return false;
	}

	for (int32 Index = 0; Index < Emoji->Emoji.Num(); ++Index)
	{
		TestNotNull(*FString::Printf(TEXT("Emoji %d has its texture"), Index), Emoji->Emoji[Index].Get());
	}

	// Held with the stick resting: open, nothing picked.
	Play(Emoji, true, FVector2D::ZeroVector, 0.5f * Emoji->MenuOpenSeconds);
	TestTrue(TEXT("The menu is open while held"), Emoji->IsMenuOpen());
	TestTrue(TEXT("It is still opening"), Emoji->GetScreenState().MenuOpen > 0.3f && Emoji->GetScreenState().MenuOpen < 0.7f);
	TestEqual(TEXT("Nothing is picked yet"), Emoji->GetScreenState().Picked, INDEX_NONE);

	Play(Emoji, true, Lean(0.0), 0.3f);
	const FEmojiScreenState& Screen = Emoji->GetScreenState();
	TestEqual(TEXT("It has opened all the way"), Screen.MenuOpen, 1.0f);
	TestEqual(TEXT("Leaning up picks the first"), Screen.Picked, 0);
	TestTrue(TEXT("The first has grown"), Screen.Highlight[0] > 0.95f);
	TestTrue(TEXT("The rest have not"), Screen.Highlight[1] < 0.05f && Screen.Highlight[8] < 0.05f);

	// Turning the stick round picks another; the first shrinks back.
	Play(Emoji, true, Lean(80.0), 0.3f);
	TestEqual(TEXT("Leaning right of up picks the third"), Screen.Picked, 2);
	TestTrue(TEXT("The third has grown"), Screen.Highlight[2] > 0.95f);
	TestTrue(TEXT("The first has shrunk back"), Screen.Highlight[0] < 0.05f);

	// Letting the stick spring back keeps the pick.
	Play(Emoji, true, FVector2D::ZeroVector, 0.2f);
	TestEqual(TEXT("The pick holds with the stick centred"), Screen.Picked, 2);
	TestEqual(TEXT("Nothing is sent while the menu is held"), Emoji->GetNumParticles(), 0);
	TestTrue(TEXT("Nothing rises while the menu is held"), Screen.Sprites.IsEmpty());

	// Let go: the third is sent, and the menu shuts.
	Play(Emoji, false, FVector2D::ZeroVector, Frame);
	TestFalse(TEXT("The menu shuts when let go"), Emoji->IsMenuOpen());
	TestTrue(TEXT("The drone gives off emoji once let go"), Emoji->GetNumParticles() > 0);
	TestNotNull(TEXT("The third's squares are drawn"), Emoji->GetCloud(2));
	TestNull(TEXT("No other emoji's squares are"), Emoji->GetCloud(0));

	Play(Emoji, false, FVector2D::ZeroVector, Emoji->MenuCloseSeconds + 0.3f);
	TestEqual(TEXT("It has shut all the way"), Screen.MenuOpen, 0.0f);
	TestEqual(TEXT("Shut, nothing is picked"), Screen.Picked, INDEX_NONE);
	TestFalse(TEXT("Emoji rise over the screen"), Screen.Sprites.IsEmpty());

	for (const FEmojiScreenSprite& Sprite : Screen.Sprites)
	{
		TestEqual(TEXT("They are the one sent"), Sprite.Emoji, 2);
	}

	// Opening again and letting go without leaning sends nothing more.
	UEmojiComponent* Quiet = SpawnEmoji(TestWorld.World);
	Play(Quiet, true, Lean(130.0, 0.3), 0.4f);
	Play(Quiet, false, FVector2D::ZeroVector, 0.5f);
	TestEqual(TEXT("Let go with nothing picked, nothing is given off"), Quiet->GetNumParticles(), 0);
	TestTrue(TEXT("Let go with nothing picked, nothing rises"), Quiet->GetScreenState().Sprites.IsEmpty());

	// A new opening starts with nothing picked, even after a pick last time.
	Play(Emoji, true, FVector2D::ZeroVector, 0.2f);
	TestEqual(TEXT("Each opening starts with nothing picked"), Screen.Picked, INDEX_NONE);

	TestWorld.World->DestroyActor(Emoji->GetOwner());
	TestWorld.World->DestroyActor(Quiet->GetOwner());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEmojiBurstTest, "WEM.Emoji.Burst", WEMRoomManagerTests::TestFlags)

bool FEmojiBurstTest::RunTest(const FString& Parameters)
{
	using namespace EmojiTestsPrivate;

	WEMRoomManagerTests::FScopedTestWorld TestWorld;
	UEmojiComponent* Emoji = SpawnEmoji(TestWorld.World);
	Emoji->SetWorldLocation(FVector(5000.0f, 5000.0f, 5000.0f));

	// Big at the drone, small far from it, and never bigger farther out.
	TestEqual(TEXT("At the drone they are NearSize"), Emoji->GetSizeAtDistance(0.0f), Emoji->NearSize);
	TestEqual(TEXT("At ShrinkDistance they are FarSize"), Emoji->GetSizeAtDistance(Emoji->ShrinkDistance), Emoji->FarSize);
	TestEqual(TEXT("Beyond it they stay FarSize"), Emoji->GetSizeAtDistance(3.0f * Emoji->ShrinkDistance), Emoji->FarSize);

	bool bShrinking = true;
	for (float Distance = 10.0f; Distance <= Emoji->ShrinkDistance; Distance += 10.0f)
	{
		bShrinking &= Emoji->GetSizeAtDistance(Distance) < Emoji->GetSizeAtDistance(Distance - 10.0f);
	}
	TestTrue(TEXT("They shrink all the way out"), bShrinking);
	TestTrue(TEXT("They shrink quickest near the drone"),
		Emoji->GetSizeAtDistance(0.0f) - Emoji->GetSizeAtDistance(100.0f) > Emoji->GetSizeAtDistance(500.0f) - Emoji->GetSizeAtDistance(600.0f));

	// A third come at once, the rest over EmitSeconds.
	Emoji->SendEmoji(3);
	Emoji->Advance(Frame);
	const int32 First = Emoji->GetNumParticles();
	TestTrue(*FString::Printf(TEXT("About a third come at once (%d of %d)"), First, Emoji->BurstCount), First >= Emoji->BurstCount / 3 && First <= Emoji->BurstCount / 2);

	UInstancedStaticMeshComponent* Cloud = Emoji->GetCloud(3);

	if (!TestNotNull(TEXT("The emoji's squares are drawn"), Cloud))
	{
		return false;
	}

	TestEqual(TEXT("A square for every one"), Cloud->GetInstanceCount(), First);
	TestNull(TEXT("No squares for an emoji not sent"), Emoji->GetCloud(4));

	auto MeanDistance = [Emoji]()
	{
		const UInstancedStaticMeshComponent* Squares = Emoji->GetCloud(3);
		double Sum = 0.0;
		for (int32 Index = 0; Index < Squares->GetInstanceCount(); ++Index)
		{
			FTransform Transform;
			Squares->GetInstanceTransform(Index, Transform, /*bWorldSpace=*/true);
			Sum += FVector::Distance(Transform.GetLocation(), Emoji->GetComponentLocation());
		}
		return Squares->GetInstanceCount() > 0 ? Sum / Squares->GetInstanceCount() : 0.0;
	};

	Play(Emoji, false, FVector2D::ZeroVector, 0.1f);
	const double Near = MeanDistance();

	Play(Emoji, false, FVector2D::ZeroVector, Emoji->EmitSeconds);
	TestEqual(TEXT("The whole burst is out"), Emoji->GetNumParticles(), Emoji->BurstCount);
	TestEqual(TEXT("And drawn"), Cloud->GetInstanceCount(), Emoji->BurstCount);
	TestTrue(*FString::Printf(TEXT("They fly out from the drone (%.0f cm on average, then %.0f)"), Near, MeanDistance()), MeanDistance() > Near + 50.0);

	// None is bigger than NearSize, and those far out are smaller than those near.
	float NearestSize = 0.0f;
	float FarthestSize = 0.0f;
	double Nearest = UE_BIG_NUMBER;
	double Farthest = 0.0;
	bool bNoneTooBig = true;

	for (int32 Index = 0; Index < Cloud->GetInstanceCount(); ++Index)
	{
		FTransform Transform;
		Cloud->GetInstanceTransform(Index, Transform, /*bWorldSpace=*/true);
		const float Size = static_cast<float>(Transform.GetScale3D().X) * 100.0f;
		const double Distance = FVector::Distance(Transform.GetLocation(), Emoji->GetComponentLocation());
		bNoneTooBig &= Size <= Emoji->NearSize + 0.01f;

		if (Distance < Nearest)
		{
			Nearest = Distance;
			NearestSize = Size;
		}
		if (Distance > Farthest)
		{
			Farthest = Distance;
			FarthestSize = Size;
		}
	}

	TestTrue(TEXT("None is bigger than NearSize"), bNoneTooBig);
	TestTrue(*FString::Printf(TEXT("The farthest (%.0f cm, %.1f cm wide) is smaller than the nearest (%.0f cm, %.1f cm wide)"), Farthest, FarthestSize, Nearest, NearestSize),
		FarthestSize < NearestSize);

	// Gone soon after.
	Play(Emoji, false, FVector2D::ZeroVector, Emoji->Lifetime);
	TestEqual(TEXT("They are all gone within Lifetime"), Emoji->GetNumParticles(), 0);
	TestEqual(TEXT("And no longer drawn"), Cloud->GetInstanceCount(), 0);

	// Many sends at once are held to a limit.
	for (int32 Send = 0; Send < 40; ++Send)
	{
		Emoji->SendEmoji(Send % 9);
	}
	Play(Emoji, false, FVector2D::ZeroVector, Emoji->EmitSeconds + 0.1f);
	TestTrue(*FString::Printf(TEXT("A pile of sends is held to 600 in flight (%d)"), Emoji->GetNumParticles()), Emoji->GetNumParticles() > 0 && Emoji->GetNumParticles() <= 600);

	Play(Emoji, false, FVector2D::ZeroVector, Emoji->Lifetime);
	TestEqual(TEXT("They all go in the end"), Emoji->GetNumParticles(), 0);

	Emoji->SendEmoji(-1);
	Emoji->SendEmoji(9);
	Emoji->Advance(Frame);
	TestEqual(TEXT("An emoji that is not there sends nothing"), Emoji->GetNumParticles(), 0);

	TestWorld.World->DestroyActor(Emoji->GetOwner());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEmojiScreenTest, "WEM.Emoji.Screen", WEMRoomManagerTests::TestFlags)

bool FEmojiScreenTest::RunTest(const FString& Parameters)
{
	using namespace EmojiTestsPrivate;

	WEMRoomManagerTests::FScopedTestWorld TestWorld;
	UEmojiComponent* Emoji = SpawnEmoji(TestWorld.World);
	const FEmojiScreenState& Screen = Emoji->GetScreenState();

	Emoji->SendEmoji(5);

	// They set off one after another over ScreenSpreadSeconds, and all are up before the first is gone.
	int32 MostAtOnce = 0;
	bool bInFrame = true;
	bool bShown = true;
	const float Biggest = 1.25f * Emoji->ScreenSize;

	for (float Time = 0.0f; Time < Emoji->ScreenSpreadSeconds + 0.1f; Time += Frame)
	{
		Emoji->Advance(Frame);
		MostAtOnce = FMath::Max(MostAtOnce, Screen.Sprites.Num());

		for (const FEmojiScreenSprite& Sprite : Screen.Sprites)
		{
			bInFrame &= Sprite.Centre.X > 0.0f && Sprite.Centre.X < 1.0f && Sprite.Centre.Y <= 1.0f + Biggest && Sprite.Centre.Y >= -Biggest;
			bShown &= Sprite.Emoji == 5 && Sprite.Opacity >= 0.0f && Sprite.Opacity <= 1.0f && Sprite.Size > 0.0f && Sprite.Size <= Biggest;
		}
	}

	TestEqual(TEXT("All of them set off within ScreenSpreadSeconds"), MostAtOnce, Emoji->ScreenCount);

	// They rise.
	const float Before = MeanHeight(Screen);
	Play(Emoji, false, FVector2D::ZeroVector, 0.8f);
	TestEqual(TEXT("None has gone yet"), Screen.Sprites.Num(), Emoji->ScreenCount);
	TestTrue(*FString::Printf(TEXT("They rise (from %.2f down the frame to %.2f)"), Before, MeanHeight(Screen)), MeanHeight(Screen) < Before - 0.15f);

	float Left = 1.0f;
	float Right = 0.0f;
	for (const FEmojiScreenSprite& Sprite : Screen.Sprites)
	{
		Left = FMath::Min(Left, Sprite.Centre.X);
		Right = FMath::Max(Right, Sprite.Centre.X);
	}
	TestTrue(*FString::Printf(TEXT("They spread across the frame (%.2f to %.2f)"), Left, Right), Left < 0.3f && Right > 0.7f);

	for (float Time = 0.0f; Time < 1.2f * Emoji->ScreenSeconds && !Screen.Sprites.IsEmpty(); Time += Frame)
	{
		Emoji->Advance(Frame);

		for (const FEmojiScreenSprite& Sprite : Screen.Sprites)
		{
			bInFrame &= Sprite.Centre.X > 0.0f && Sprite.Centre.X < 1.0f && Sprite.Centre.Y <= 1.0f + Biggest && Sprite.Centre.Y >= -Biggest;
			bShown &= Sprite.Emoji == 5 && Sprite.Opacity >= 0.0f && Sprite.Opacity <= 1.0f && Sprite.Size > 0.0f && Sprite.Size <= Biggest;
		}
	}

	TestTrue(TEXT("They stay across the frame, from just below it to just above it"), bInFrame);
	TestTrue(TEXT("They show the emoji sent, sized and faded within bounds"), bShown);
	TestTrue(TEXT("They are all gone within ScreenSpreadSeconds and the longest rise"), Screen.Sprites.IsEmpty());

	TestWorld.World->DestroyActor(Emoji->GetOwner());
	return true;
}

#endif
