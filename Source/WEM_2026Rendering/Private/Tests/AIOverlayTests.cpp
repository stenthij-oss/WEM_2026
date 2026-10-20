// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AIOverlay.h"
#include "AIOverlaySettings.h"
#include "HAL/IConsoleManager.h"
#include "Math/InverseRotationMatrix.h"
#include "Math/PerspectiveMatrix.h"
#include "Misc/AutomationTest.h"
#include "SceneView.h"

namespace AIOverlayTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	AIOverlay::FTiming Timing()
	{
		AIOverlay::FTiming Result;
		Result.GapMin = 20.0f;
		Result.GapMax = 30.0f;
		Result.RampUp = 8.0f;
		Result.Hold = 5.0f;
		Result.RampDown = 8.0f;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIOverlayCycleTest, "WEM.AIOverlay.Cycle", AIOverlayTestsPrivate::TestFlags)

bool FAIOverlayCycleTest::RunTest(const FString& Parameters)
{
	using namespace AIOverlayTestsPrivate;

	// The first cycle waits out a gap of 20 to 30 seconds, ramps in over 8, holds 5 and ramps out over 8.
	for (int32 Seed = 0; Seed < 20; ++Seed)
	{
		AIOverlay::FCycle Cycle(Seed);
		double Start = -1.0;
		for (double Now = 0.0; Now < 31.0; Now += 0.05)
		{
			const float Intensity = Cycle.Advance(Now, Timing());
			if (Cycle.GetCycleCount() > 0)
			{
				Start = Now;
				TestEqual(TEXT("A cycle starts from nothing"), Intensity, 0.0f);
				break;
			}
			TestEqual(TEXT("Nothing during the gap"), Intensity, 0.0f);
		}
		TestTrue(FString::Printf(TEXT("Seed %d: the first cycle starts 20 to 30 s in (%.2f)"), Seed, Start), Start >= 20.0 && Start <= 30.05);

		TestEqual(TEXT("Halfway up the ramp is halfway in"), Cycle.Advance(Start + 4.0, Timing()), 0.5f, 1.0e-4f);
		TestTrue(TEXT("The ramp rises"), Cycle.Advance(Start + 6.0, Timing()) > 0.5f);
		TestEqual(TEXT("Fully in once the ramp is up"), Cycle.Advance(Start + 8.0, Timing()), 1.0f);
		TestEqual(TEXT("Holds"), Cycle.Advance(Start + 12.9, Timing()), 1.0f);
		TestEqual(TEXT("Halfway down the ramp is halfway out"), Cycle.Advance(Start + 17.0, Timing()), 0.5f, 1.0e-4f);
		TestEqual(TEXT("Out once the ramp is down"), Cycle.Advance(Start + 21.0, Timing()), 0.0f);
		TestEqual(TEXT("And stays out for the next gap"), Cycle.Advance(Start + 21.0 + 19.9, Timing()), 0.0f);
		TestEqual(TEXT("One cycle so far"), (int32)Cycle.GetCycleCount(), 1);
	}

	// wem.AIOverlay.Start ends the gap at once; asked again during a cycle, it changes nothing.
	AIOverlay::FCycle Cycle(1);
	TestEqual(TEXT("Gap"), Cycle.Advance(0.0, Timing()), 0.0f);
	Cycle.StartNow();
	TestEqual(TEXT("Starts at once, from nothing"), Cycle.Advance(1.0, Timing()), 0.0f);
	TestEqual(TEXT("Started"), (int32)Cycle.GetCycleCount(), 1);
	Cycle.StartNow();
	TestEqual(TEXT("Asked again mid-ramp: the ramp goes on"), Cycle.Advance(5.0, Timing()), 0.5f, 1.0e-4f);
	TestEqual(TEXT("Still the one cycle"), (int32)Cycle.GetCycleCount(), 1);

	// A restart drops a cycle halfway and waits out a whole new gap from then.
	AIOverlay::FCycle Restarted(3);
	Restarted.StartNow();
	Restarted.Advance(100.0, Timing());
	TestEqual(TEXT("Mid-ramp before the restart"), Restarted.Advance(104.0, Timing()), 0.5f, 1.0e-4f);
	Restarted.Restart();
	TestEqual(TEXT("Out at once after the restart"), Restarted.Advance(104.0, Timing()), 0.0f);
	TestEqual(TEXT("And for the whole of the next gap"), Restarted.Advance(104.0 + 19.9, Timing()), 0.0f);
	TestEqual(TEXT("A new cycle once that gap is over"), Restarted.Advance(104.0 + 30.0, Timing()), 0.0f);
	TestEqual(TEXT("Which is the second"), (int32)Restarted.GetCycleCount(), 2);

	// No ramps: straight in and straight out.
	AIOverlay::FTiming Sharp = Timing();
	Sharp.RampUp = 0.0f;
	Sharp.RampDown = 0.0f;
	AIOverlay::FCycle SharpCycle(2);
	SharpCycle.StartNow();
	TestEqual(TEXT("Straight in"), SharpCycle.Advance(0.0, Sharp), 1.0f);
	TestEqual(TEXT("Holding"), SharpCycle.Advance(4.9, Sharp), 1.0f);
	TestEqual(TEXT("Straight out"), SharpCycle.Advance(5.0, Sharp), 0.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIOverlayConstantTest, "WEM.AIOverlay.Constant", AIOverlayTestsPrivate::TestFlags)

bool FAIOverlayConstantTest::RunTest(const FString& Parameters)
{
	using namespace AIOverlayTestsPrivate;

	AIOverlay::FTiming Constant = Timing();
	Constant.bConstant = true;

	// Constant from the start: no gap, a ramp in, and then full for good.
	AIOverlay::FCycle Cycle(4);
	TestEqual(TEXT("Starts at once, from nothing"), Cycle.Advance(0.0, Constant), 0.0f);
	TestEqual(TEXT("Started"), (int32)Cycle.GetCycleCount(), 1);
	TestEqual(TEXT("Halfway up the ramp"), Cycle.Advance(4.0, Constant), 0.5f, 1.0e-4f);
	TestEqual(TEXT("Full once up"), Cycle.Advance(8.0, Constant), 1.0f);
	for (double Now = 8.0; Now <= 600.0; Now += 0.5)
	{
		if (!TestEqual(TEXT("Stays full"), Cycle.Advance(Now, Constant), 1.0f))
		{
			break;
		}
	}
	TestEqual(TEXT("Still the one cycle"), (int32)Cycle.GetCycleCount(), 1);

	// Turned off: the hold runs out, it ramps out, and a gap follows.
	TestEqual(TEXT("Held after turning off"), Cycle.Advance(604.0, Timing()), 1.0f);
	TestEqual(TEXT("Halfway out"), Cycle.Advance(609.0, Timing()), 0.5f, 1.0e-4f);
	TestEqual(TEXT("Out"), Cycle.Advance(613.0, Timing()), 0.0f);
	TestEqual(TEXT("Then a gap"), Cycle.Advance(613.0 + 19.9, Timing()), 0.0f);

	// Turned on during a gap: it comes in at once.
	TestEqual(TEXT("From the gap, at once"), Cycle.Advance(640.0, Constant), 0.0f);
	TestEqual(TEXT("A second cycle"), (int32)Cycle.GetCycleCount(), 2);
	TestEqual(TEXT("Coming up"), Cycle.Advance(644.0, Constant), 0.5f, 1.0e-4f);

	// Turned on while a normal cycle ramps out: it turns back up from where it is, with no jump.
	AIOverlay::FCycle Falling(5);
	Falling.StartNow();
	Falling.Advance(0.0, Timing());
	const float AtTurn = Falling.Advance(17.0, Timing());
	TestEqual(TEXT("Halfway out before turning on"), AtTurn, 0.5f, 1.0e-4f);
	TestEqual(TEXT("Turned on: the same value"), Falling.Advance(17.0, Constant), AtTurn, 1.0e-4f);
	TestTrue(TEXT("And rising"), Falling.Advance(19.0, Constant) > AtTurn);
	TestEqual(TEXT("Full again"), Falling.Advance(21.0, Constant), 1.0f);
	TestEqual(TEXT("Still the one cycle"), (int32)Falling.GetCycleCount(), 1);

	// No ramps and no hold: full the whole time, still one cycle.
	AIOverlay::FTiming Sharp = Constant;
	Sharp.RampUp = 0.0f;
	Sharp.Hold = 0.0f;
	Sharp.RampDown = 0.0f;
	AIOverlay::FCycle SharpCycle(6);
	for (double Now = 0.0; Now < 10.0; Now += 0.25)
	{
		TestEqual(TEXT("Full at once and after"), SharpCycle.Advance(Now, Sharp), 1.0f);
	}
	TestEqual(TEXT("One cycle"), (int32)SharpCycle.GetCycleCount(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIOverlaySharedLayoutTest, "WEM.AIOverlay.SharedLayout", AIOverlayTestsPrivate::TestFlags)

bool FAIOverlaySharedLayoutTest::RunTest(const FString& Parameters)
{
	// Tools/AIOverlay/bridge.py reads and writes these offsets.
	TestEqual(TEXT("Sequence"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Sequence), 8);
	TestEqual(TEXT("Width"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Width), 12);
	TestEqual(TEXT("FrameId"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, FrameId), 20);
	TestEqual(TEXT("Strength"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Strength), 28);
	TestEqual(TEXT("Heartbeat"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Heartbeat), 32);
	TestEqual(TEXT("DepthWeight"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, DepthWeight), 40);
	TestEqual(TEXT("Controls"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Controls), 44);
	TestEqual(TEXT("MaxRate"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, MaxRate), 48);
	TestEqual(TEXT("Size"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Size), 52);
	TestEqual(TEXT("Rate"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Rate), 56);
	TestEqual(TEXT("Prompt"), (int32)STRUCT_OFFSET(AIOverlay::FSharedHeader, Prompt), 64);
	TestEqual(TEXT("Pixels"), (int64)AIOverlay::PixelOffset, (int64)2048);
	TestEqual(TEXT("Block size"), (int64)AIOverlay::SharedSize, (int64)(2048 + 1024 * 1024 * 4));

	// Channels, as bridge.py's shared_name names them.
	TestEqual(TEXT("Channel 0 keeps the name"), AIOverlay::GetSharedName(AIOverlay::InputName, 0), FString(AIOverlay::InputName));
	TestEqual(TEXT("Channel 9 adds to the GUID's second group"), AIOverlay::GetSharedName(AIOverlay::OutputName, 9),
		FString(TEXT("{5E1A7C3B-2D4D-4F0E-9B6A-41A0C3D2E702}")));
	FGuid Parsed;
	TestTrue(TEXT("And is still a GUID, which Windows shares without administrator rights"),
		FGuid::ParseExact(AIOverlay::GetSharedName(AIOverlay::ControlName, 3), EGuidFormats::DigitsWithHyphensInBraces, Parsed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIOverlayReprojectTest, "WEM.AIOverlay.Reproject", AIOverlayTestsPrivate::TestFlags)

bool FAIOverlayReprojectTest::RunTest(const FString& Parameters)
{
	// Two views of the same spot, the second moved and turned, both far from the origin, as a camera a few frames apart.
	const auto MakeView = [](const FVector& Origin, const FRotator& Rotation)
	{
		FViewMatrices::FMinimalInitializer Initializer;
		Initializer.ViewOrigin = Origin;
		// Unreal's view space looks down +Z, with X right and Y up, from its world's X forward, Y right, Z up.
		Initializer.ViewRotationMatrix = FInverseRotationMatrix(Rotation) * FMatrix(
			FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
		Initializer.ProjectionMatrix = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(45.0f), 16.0f, 9.0f, 10.0f);
		Initializer.ConstrainedViewRect = FIntRect(0, 0, 1600, 900);
		return FViewMatrices(Initializer);
	};
	const FViewMatrices Earlier = MakeView(FVector(250000.0, -120000.0, 3000.0), FRotator(-10.0f, 30.0f, 0.0f));
	const FViewMatrices Now = MakeView(FVector(250040.0, -119990.0, 3005.0), FRotator(-9.0f, 31.5f, 0.0f));

	const FMatrix EarlierToClip = AIOverlay::GetTranslatedWorldToClipNoAA(Earlier);
	const FMatrix NowToClip = AIOverlay::GetTranslatedWorldToClipNoAA(Now);
	const FMatrix NowToEarlierClip = AIOverlay::MakeTranslatedWorldToEarlierClip(Now.GetPreViewTranslation(),
		Earlier.GetPreViewTranslation(), EarlierToClip);
	const FMatrix NowClipToTranslatedWorld = Now.ComputeInvProjectionNoAAMatrix() * Now.GetInvTranslatedViewMatrix();

	// Spots in front of both cameras: seen from now, taken back through the screen and depth (as the shader does), they
	// land where the earlier view saw them.
	const FVector Spots[] = {FVector(254000.0, -118000.0, 2500.0), FVector(251500.0, -119200.0, 3300.0), FVector(256000.0, -115000.0, 1000.0)};
	for (const FVector& Spot : Spots)
	{
		const FVector4 SeenNow = NowToClip.TransformFVector4(FVector4(Spot + Now.GetPreViewTranslation(), 1.0));
		const FVector4 SeenEarlier = EarlierToClip.TransformFVector4(FVector4(Spot + Earlier.GetPreViewTranslation(), 1.0));
		if (!TestTrue(TEXT("In front of both"), SeenNow.W > 0.0 && SeenEarlier.W > 0.0))
		{
			continue;
		}

		// The screen position and device depth the shader starts from, and its sums, in floats as on the GPU.
		const FVector4f Screen((float)(SeenNow.X / SeenNow.W), (float)(SeenNow.Y / SeenNow.W), (float)(SeenNow.Z / SeenNow.W), 1.0f);
		const FVector4f Translated = FMatrix44f(NowClipToTranslatedWorld).TransformFVector4(Screen);
		const FVector4f Point(Translated.X / Translated.W, Translated.Y / Translated.W, Translated.Z / Translated.W, 1.0f);
		const FVector4f Back = FMatrix44f(NowToEarlierClip).TransformFVector4(Point);

		TestEqual(TEXT("Same spot across, in the earlier view"), (double)(Back.X / Back.W), SeenEarlier.X / SeenEarlier.W, 1.0e-3);
		TestEqual(TEXT("Same spot up and down"), (double)(Back.Y / Back.W), SeenEarlier.Y / SeenEarlier.W, 1.0e-3);
		TestEqual(TEXT("Its depth then, as the earlier view saw it"), (double)Back.W, SeenEarlier.W, SeenEarlier.W * 1.0e-3);
	}

	// Held where 85 % of the frames get a picture of their own, within bounds.
	TestEqual(TEXT("Held where 85 % of the frames get a picture"), AIOverlay::GetMatchedFrameRate(34.0f), 40.0f, 1.0e-3f);
	TestEqual(TEXT("Never below 20"), AIOverlay::GetMatchedFrameRate(5.0f), 20.0f);
	TestEqual(TEXT("Never above 240"), AIOverlay::GetMatchedFrameRate(500.0f), 240.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIOverlayTurnsTest, "WEM.AIOverlay.Turns", AIOverlayTestsPrivate::TestFlags)

bool FAIOverlayTurnsTest::RunTest(const FString& Parameters)
{
	// One player: always theirs, whatever is asked.
	for (uint32 Cycle = 0; Cycle < 5; ++Cycle)
	{
		TestEqual(TEXT("One player, taking turns"), AIOverlay::PickTurn(Cycle, 1, 0), 0);
		TestEqual(TEXT("One player, player 2 asked"), AIOverlay::PickTurn(Cycle, 1, 2), 0);
	}

	// Two players taking turns: the first cycle on the left screen, then right, left, right.
	TestEqual(TEXT("Before any cycle"), AIOverlay::PickTurn(0, 2, 0), 0);
	TestEqual(TEXT("Cycle 1 on player 1"), AIOverlay::PickTurn(1, 2, 0), 0);
	TestEqual(TEXT("Cycle 2 on player 2"), AIOverlay::PickTurn(2, 2, 0), 1);
	TestEqual(TEXT("Cycle 3 on player 1"), AIOverlay::PickTurn(3, 2, 0), 0);
	TestEqual(TEXT("Cycle 4 on player 2"), AIOverlay::PickTurn(4, 2, 0), 1);

	// Kept on one player.
	TestEqual(TEXT("Player 1 only"), AIOverlay::PickTurn(2, 2, 1), 0);
	TestEqual(TEXT("Player 2 only"), AIOverlay::PickTurn(3, 2, 2), 1);
	TestEqual(TEXT("A player who is not there gets nothing"), AIOverlay::PickTurn(3, 2, 3), (int32)INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIOverlaySettingsTest, "WEM.AIOverlay.Settings", AIOverlayTestsPrivate::TestFlags)

bool FAIOverlaySettingsTest::RunTest(const FString& Parameters)
{
	IConsoleManager& Console = IConsoleManager::Get();
	const auto Variable = [&Console](const TCHAR* Name) { return Console.FindConsoleVariable(Name); };

	// Every console variable the settings page drives, put back as it was at the end.
	const TCHAR* Names[] = {
		TEXT("wem.AIOverlay"), TEXT("wem.AIOverlay.GapMin"), TEXT("wem.AIOverlay.GapMax"), TEXT("wem.AIOverlay.RampUp"),
		TEXT("wem.AIOverlay.Hold"), TEXT("wem.AIOverlay.RampDown"), TEXT("wem.AIOverlay.Prompt"), TEXT("wem.AIOverlay.Strength"),
		TEXT("wem.AIOverlay.Opacity"), TEXT("wem.AIOverlay.ClipToMask"), TEXT("wem.AIOverlay.Constant"), TEXT("wem.AIOverlay.SegmentPercent"),
		TEXT("wem.AIOverlay.Depth"), TEXT("wem.AIOverlay.DepthWeight"), TEXT("wem.AIOverlay.DepthFit"), TEXT("wem.AIOverlay.DepthNear"),
		TEXT("wem.AIOverlay.DepthFar"),
		TEXT("wem.AIOverlay.Show"), TEXT("wem.AIOverlay.Player"), TEXT("wem.AIOverlay.MaxRate"), TEXT("wem.AIOverlay.Reproject"),
		TEXT("wem.AIOverlay.Blend"), TEXT("wem.AIOverlay.Size"), TEXT("wem.AIOverlay.MatchFrameRate")};
	TMap<FString, FString> Before;
	for (const TCHAR* Name : Names)
	{
		if (!TestNotNull(FString::Printf(TEXT("%s exists"), Name), Variable(Name)))
		{
			return false;
		}
		Before.Add(Name, Variable(Name)->GetString());
	}

	UAIOverlaySettings* Settings = NewObject<UAIOverlaySettings>();
	Settings->bEnabled = false;
	Settings->GapMin = 3.0f;
	Settings->GapMax = 4.0f;
	Settings->RampUp = 1.0f;
	Settings->Hold = 2.0f;
	Settings->RampDown = 1.5f;
	Settings->Prompt = TEXT("a test prompt");
	Settings->Strength = 0.25f;
	Settings->Opacity = 0.5f;
	Settings->bClipToMask = false;
	Settings->bConstant = true;
	Settings->SegmentPercent = 40.0f;
	Settings->bDepth = false;
	Settings->DepthWeight = 1.25f;
	Settings->bDepthFit = false;
	Settings->DepthNear = 2.0f;
	Settings->DepthFar = 50.0f;
	Settings->Show = EAIOverlayShow::SentDepth;
	Settings->Player = EAIOverlayPlayer::Player2;
	Settings->MaxRate = 15.0f;
	Settings->bReproject = false;
	Settings->bBlend = false;
	Settings->Size = 384;
	Settings->bMatchFrameRate = true;

	// A change on the page sets only its own variable.
	Settings->Apply(ECVF_SetByConsole, GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Hold));
	TestEqual(TEXT("Hold is set"), Variable(TEXT("wem.AIOverlay.Hold"))->GetFloat(), 2.0f);
	TestEqual(TEXT("The others are not"), Variable(TEXT("wem.AIOverlay.GapMin"))->GetString(), Before[TEXT("wem.AIOverlay.GapMin")]);

	Settings->Apply(ECVF_SetByConsole);
	TestEqual(TEXT("On"), Variable(TEXT("wem.AIOverlay"))->GetInt(), 0);
	TestEqual(TEXT("GapMin"), Variable(TEXT("wem.AIOverlay.GapMin"))->GetFloat(), 3.0f);
	TestEqual(TEXT("GapMax"), Variable(TEXT("wem.AIOverlay.GapMax"))->GetFloat(), 4.0f);
	TestEqual(TEXT("RampUp"), Variable(TEXT("wem.AIOverlay.RampUp"))->GetFloat(), 1.0f);
	TestEqual(TEXT("RampDown"), Variable(TEXT("wem.AIOverlay.RampDown"))->GetFloat(), 1.5f);
	TestEqual(TEXT("Prompt"), Variable(TEXT("wem.AIOverlay.Prompt"))->GetString(), FString(TEXT("a test prompt")));
	TestEqual(TEXT("Strength"), Variable(TEXT("wem.AIOverlay.Strength"))->GetFloat(), 0.25f);
	TestEqual(TEXT("Opacity"), Variable(TEXT("wem.AIOverlay.Opacity"))->GetFloat(), 0.5f);
	TestEqual(TEXT("ClipToMask"), Variable(TEXT("wem.AIOverlay.ClipToMask"))->GetInt(), 0);
	TestEqual(TEXT("Constant"), Variable(TEXT("wem.AIOverlay.Constant"))->GetInt(), 1);
	TestEqual(TEXT("SegmentPercent"), Variable(TEXT("wem.AIOverlay.SegmentPercent"))->GetFloat(), 40.0f);
	TestEqual(TEXT("Depth"), Variable(TEXT("wem.AIOverlay.Depth"))->GetInt(), 0);
	TestEqual(TEXT("DepthWeight"), Variable(TEXT("wem.AIOverlay.DepthWeight"))->GetFloat(), 1.25f);
	TestEqual(TEXT("DepthFit"), Variable(TEXT("wem.AIOverlay.DepthFit"))->GetInt(), 0);
	TestEqual(TEXT("DepthNear"), Variable(TEXT("wem.AIOverlay.DepthNear"))->GetFloat(), 2.0f);
	TestEqual(TEXT("DepthFar"), Variable(TEXT("wem.AIOverlay.DepthFar"))->GetFloat(), 50.0f);
	TestEqual(TEXT("Show"), Variable(TEXT("wem.AIOverlay.Show"))->GetInt(), 2);
	TestEqual(TEXT("Player"), Variable(TEXT("wem.AIOverlay.Player"))->GetInt(), 2);
	TestEqual(TEXT("MaxRate"), Variable(TEXT("wem.AIOverlay.MaxRate"))->GetFloat(), 15.0f);
	TestEqual(TEXT("Reproject"), Variable(TEXT("wem.AIOverlay.Reproject"))->GetInt(), 0);
	TestEqual(TEXT("Blend"), Variable(TEXT("wem.AIOverlay.Blend"))->GetInt(), 0);
	TestEqual(TEXT("Size"), Variable(TEXT("wem.AIOverlay.Size"))->GetInt(), 384);
	TestEqual(TEXT("MatchFrameRate"), Variable(TEXT("wem.AIOverlay.MatchFrameRate"))->GetInt(), 1);

	for (const TPair<FString, FString>& Pair : Before)
	{
		Variable(*Pair.Key)->Set(*Pair.Value, ECVF_SetByConsole);
	}
	return true;
}

#endif
