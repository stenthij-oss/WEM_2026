// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Camera/CameraTypes.h"
#include "Engine/Scene.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "PhoneCamera.h"
#include "PortraitFrame.h"
#include "SceneView.h"

namespace PortraitFrameTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** The projection the engine builds for this view in a ViewSize view, through the same function the game uses. */
	FMatrix Project(FMinimalViewInfo View, const FIntPoint ViewSize)
	{
		FSceneViewProjectionData Data;
		const FIntRect Rect(FIntPoint::ZeroValue, ViewSize);
		Data.SetViewRectangle(Rect);
		FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle(View, AspectRatio_MaintainXFOV, Rect, Data);
		return Data.ProjectionMatrix;
	}

	double TanHalf(const float FOVDegrees)
	{
		return FMath::Tan(FMath::DegreesToRadians((double)FOVDegrees) / 2.0);
	}

	struct FCase
	{
		const TCHAR* Name;
		bool bConstrain;
		float AspectRatio;
		float FOV;
		/** tan(half the field of view) along the lens's wide side. */
		double TanHalfWide;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPortraitFrameFitTest, "WEM.Portrait.Fit", PortraitFrameTestsPrivate::TestFlags)

bool FPortraitFrameFitTest::RunTest(const FString& Parameters)
{
	using namespace PortraitFrameTestsPrivate;

	// The lens turns upright: the frame is as tall as the lens was wide (shorter for a longer focal length), and
	// Ratio as wide as it is tall. Checked through the engine's own projection in a landscape window.
	const FCase Cases[] = {
		{ TEXT("drone's 8 mm lens"), false, 16.0f / 9.0f, 112.0873f, TanHalf(112.0873f) },
		{ TEXT("drone zoomed to 55 mm"), false, 16.0f / 9.0f, 24.4f, TanHalf(24.4f) },
		{ TEXT("CineCamera constraining 16:9"), true, 16.0f / 9.0f, 90.0f, TanHalf(90.0f) },
		{ TEXT("camera constraining a tall 3:4"), true, 0.75f, 60.0f, TanHalf(60.0f) / 0.75 },
	};

	for (const float Ratio : { 9.0f / 16.0f, 9.0f / 19.5f })
	{
		for (const float Scale : { 1.0f, 0.8f, 1.25f })
		{
			for (const FCase& Case : Cases)
			{
				FMinimalViewInfo View;
				View.bConstrainAspectRatio = Case.bConstrain;
				View.AspectRatio = Case.AspectRatio;
				View.FOV = Case.FOV;

				PortraitFrame::Fit(View, Ratio, Scale);
				const FMatrix Portrait = Project(View, FIntPoint(2713, 1827));

				const FString Name = FString::Printf(TEXT("%s, ratio %.4f, focal length x%.2f"), Case.Name, Ratio, Scale);
				TestTrue(Name + TEXT(": constrained"), View.bConstrainAspectRatio);
				TestEqual(Name + TEXT(": height is the lens's width"), 1.0 / Portrait.M[1][1], Case.TanHalfWide / Scale, 1e-4);
				TestEqual(Name + TEXT(": width over height"), Portrait.M[1][1] / Portrait.M[0][0], (double)Ratio, 1e-4);
			}
		}
	}

	// Left alone: orthographic views, and a ratio of 0. A focal length scale of 0 or less counts as 1.
	FMinimalViewInfo Ortho;
	Ortho.ProjectionMode = ECameraProjectionMode::Orthographic;
	PortraitFrame::Fit(Ortho, 9.0f / 16.0f, 1.0f);
	TestFalse(TEXT("orthographic left alone"), Ortho.bConstrainAspectRatio);

	FMinimalViewInfo Zero;
	Zero.FOV = 90.0f;
	PortraitFrame::Fit(Zero, 0.0f, 1.0f);
	TestEqual(TEXT("ratio 0 left alone"), Zero.FOV, 90.0f);

	FMinimalViewInfo Unscaled;
	Unscaled.FOV = 90.0f;
	PortraitFrame::Fit(Unscaled, 9.0f / 16.0f, 0.0f);
	TestEqual(TEXT("focal length scale 0 counts as 1"), TanHalf(Unscaled.FOV), 9.0 / 16.0, 1e-4);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPortraitFrameToggleTest, "WEM.Portrait.Toggle", PortraitFrameTestsPrivate::TestFlags)

bool FPortraitFrameToggleTest::RunTest(const FString& Parameters)
{
	// wem.Portrait.Toggle (the P key) flips wem.Portrait each time, and a second press goes back to normal.
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.Portrait"));
	IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(TEXT("wem.Portrait.Toggle"));
	IConsoleCommand* Command = Object ? Object->AsCommand() : nullptr;
	if (!TestNotNull(TEXT("wem.Portrait is registered"), Variable) || !TestNotNull(TEXT("wem.Portrait.Toggle is registered"), Command))
	{
		return false;
	}

	const int32 Before = Variable->GetInt();
	Command->Execute({}, nullptr, *GLog);
	TestEqual(TEXT("first press"), Variable->GetInt(), Before != 0 ? 0 : 1);
	Command->Execute({}, nullptr, *GLog);
	TestEqual(TEXT("second press"), Variable->GetInt(), Before);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhoneCameraLookTest, "WEM.Portrait.PhoneLook", PortraitFrameTestsPrivate::TestFlags)

bool FPhoneCameraLookTest::RunTest(const FString& Parameters)
{
	// Strength 0 changes nothing.
	FPostProcessSettings Untouched;
	PhoneCamera::ApplyLook(Untouched, 0.0f);
	TestFalse(TEXT("strength 0: saturation not overridden"), (bool)Untouched.bOverride_ColorSaturation);
	TestFalse(TEXT("strength 0: depth of field not overridden"), (bool)Untouched.bOverride_DepthOfFieldFocalDistance);

	// Strength 1 reaches the phone's values; everything ends up in focus. The engine's vignette, colour fringes and
	// sharpening stay as they were: the pass draws its own, after the sky glitch.
	FPostProcessSettings Full;
	PhoneCamera::ApplyLook(Full, 1.0f);
	TestFalse(TEXT("full: engine vignette left alone"), (bool)Full.bOverride_VignetteIntensity);
	TestFalse(TEXT("full: engine colour fringes left alone"), (bool)Full.bOverride_SceneFringeIntensity);
	TestFalse(TEXT("full: engine sharpening left alone"), (bool)Full.bOverride_Sharpen);
	TestTrue(TEXT("full: saturation overridden"), (bool)Full.bOverride_ColorSaturation);
	TestEqual(TEXT("full: saturation"), (float)Full.ColorSaturation.X, 1.3f);
	TestEqual(TEXT("full: highlights pressed down"), Full.LocalExposureHighlightContrastScale, 0.45f);
	TestEqual(TEXT("full: shadows lifted"), Full.LocalExposureShadowContrastScale, 0.5f);
	TestTrue(TEXT("full: no lens flare ghosts"), Full.bOverride_LensFlareIntensity && Full.LensFlareIntensity == 0.0f);
	TestEqual(TEXT("full: bloom"), Full.BloomIntensity, 1.5f);
	TestTrue(TEXT("full: depth of field off"), Full.bOverride_DepthOfFieldFocalDistance && Full.DepthOfFieldFocalDistance == 0.0f);

	// Strength 0.5 goes halfway from where the camera had it.
	FPostProcessSettings Half;
	const float BloomBefore = Half.BloomIntensity;
	PhoneCamera::ApplyLook(Half, 0.5f);
	TestEqual(TEXT("half: bloom halfway"), Half.BloomIntensity, FMath::Lerp(BloomBefore, 1.5f, 0.5f));
	TestEqual(TEXT("half: still no lens flare ghosts"), Half.LensFlareIntensity, 0.0f);

	// Strength 2 goes further but stays usable.
	FPostProcessSettings Double;
	PhoneCamera::ApplyLook(Double, 2.0f);
	TestTrue(TEXT("double: highlight contrast stays above 0"), Double.LocalExposureHighlightContrastScale >= 0.05f);
	TestTrue(TEXT("double: motion blur stays at 0 or more"), Double.MotionBlurAmount >= 0.0f);
	TestTrue(TEXT("double: more bloom than full"), Double.BloomIntensity > Full.BloomIntensity);

	// The stabiliser's margin: rendered wider by exactly what the pass crops away.
	FMinimalViewInfo View;
	View.FOV = 80.0f;
	PhoneCamera::AddStabilizerMargin(View);
	TestEqual(TEXT("margin widens by 1 / CropScale"), PortraitFrameTestsPrivate::TanHalf(View.FOV),
		PortraitFrameTestsPrivate::TanHalf(80.0f) / PhoneCamera::CropScale, 1e-6);

	// The hand: still at strength 0, within about a degree at 1, the same at the same moment, moving over time.
	TestTrue(TEXT("no shake at strength 0"), PhoneCamera::HandShake(12.3, 0.0f).IsNearlyZero());
	bool bMoves = false;
	for (double Seconds = 0.0; Seconds < 30.0; Seconds += 0.37)
	{
		const FRotator Shake = PhoneCamera::HandShake(Seconds, 1.0f);
		if (!TestTrue(FString::Printf(TEXT("shake within reach at %.2f s"), Seconds),
			FMath::Abs(Shake.Pitch) <= 0.8f && FMath::Abs(Shake.Yaw) <= 0.9f && FMath::Abs(Shake.Roll) <= 1.1f))
		{
			break;
		}
		bMoves |= !Shake.Equals(PhoneCamera::HandShake(0.0, 1.0f), 0.05f);
	}
	TestTrue(TEXT("shake moves over time"), bMoves);
	TestTrue(TEXT("shake is the same at the same moment"), PhoneCamera::HandShake(4.2, 1.0f).Equals(PhoneCamera::HandShake(4.2, 1.0f)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhoneCameraRollingShutterTest, "WEM.Portrait.RollingShutter", PortraitFrameTestsPrivate::TestFlags)

bool FPhoneCameraRollingShutterTest::RunTest(const FString& Parameters)
{
	// Turn rates are in the camera's own frame, wherever it points: a camera looking down and to the side that turns
	// 1 degree to its own right in 0.01 s turns at 100 degrees a second to the right, and not up.
	const FQuat Looking = FRotator(-30.0, 45.0, 10.0).Quaternion();
	const FVector2f Right = PhoneCamera::TurnRate(Looking, Looking * FRotator(0.0, 1.0, 0.0).Quaternion(), 0.01);
	TestEqual(TEXT("turning right: x"), Right.X, FMath::DegreesToRadians(100.0f), 1e-3f);
	TestEqual(TEXT("turning right: y"), Right.Y, 0.0f, 1e-3f);
	const FVector2f Up = PhoneCamera::TurnRate(Looking, Looking * FRotator(1.0, 0.0, 0.0).Quaternion(), 0.01);
	TestEqual(TEXT("tilting up: x"), Up.X, 0.0f, 1e-3f);
	TestEqual(TEXT("tilting up: y"), Up.Y, FMath::DegreesToRadians(100.0f), 1e-3f);
	TestTrue(TEXT("no time, no turn"), PhoneCamera::TurnRate(Looking, Looking * FRotator(0.0, 5.0, 0.0).Quaternion(), 0.0).IsNearlyZero());

	// Turning at 1 radian a second through a 0.08 s readout, with a projection scale of 1, moves rows by 0.04 frame
	// widths per frame height: positive to the right and up, negative the other way.
	FMatrix Projection = FMatrix::Identity;
	TestEqual(TEXT("lean"), PhoneCamera::ShutterSkew(FVector2f(1.0f, 0.0f), 0.08f, Projection).X, 0.04f, 1e-5f);
	TestEqual(TEXT("lean the other way"), PhoneCamera::ShutterSkew(FVector2f(-1.0f, 0.0f), 0.08f, Projection).X, -0.04f, 1e-5f);
	TestEqual(TEXT("stretch"), PhoneCamera::ShutterSkew(FVector2f(0.0f, 1.0f), 0.08f, Projection).Y, 0.04f, 1e-5f);
	TestTrue(TEXT("no readout, no lean"), PhoneCamera::ShutterSkew(FVector2f(3.0f, -2.0f), 0.0f, Projection).IsNearlyZero());

	// A wider projection scale (a longer lens) leans more for the same turn.
	Projection.M[0][0] = 2.0;
	TestEqual(TEXT("longer lens leans more"), PhoneCamera::ShutterSkew(FVector2f(1.0f, 0.0f), 0.08f, Projection).X, 0.08f, 1e-5f);

	// Never further than the stabiliser's margin holds, except stretching, which only moves inward.
	const float Margin = 1.0f / PhoneCamera::CropScale - 1.0f;
	TestEqual(TEXT("lean held by the margin"), PhoneCamera::ShutterSkew(FVector2f(50.0f, 0.0f), 0.08f, Projection).X, Margin, 1e-5f);
	TestEqual(TEXT("squash held by the margin"), PhoneCamera::ShutterSkew(FVector2f(0.0f, -50.0f), 0.08f, Projection).Y, -Margin, 1e-5f);
	TestEqual(TEXT("stretch held at half"), PhoneCamera::ShutterSkew(FVector2f(0.0f, 50.0f), 0.08f, Projection).Y, 0.5f, 1e-5f);

	return true;
}

#endif
