// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"
#include "SkyGlitchSettings.h"

namespace SkyGlitchSettingsTestsPrivate
{
	const TCHAR* const Names[] = {
		TEXT("wem.SkyGlitch"), TEXT("wem.SkyGlitch.Black"), TEXT("wem.SkyGlitch.Interval"),
		TEXT("wem.SkyGlitch.Distance"), TEXT("wem.SkyGlitch.ResetAfter"), TEXT("wem.SkyGlitch.TurnOnAfter"),
		TEXT("wem.Portrait"), TEXT("wem.Portrait.Ratio"), TEXT("wem.Portrait.FocalLengthScale"), TEXT("wem.Portrait.Effects"),
		TEXT("wem.Portrait.Sharpen"), TEXT("wem.Portrait.Fringe"), TEXT("wem.Portrait.Shake"), TEXT("wem.Portrait.RollingShutter")
	};

	float Get(const TCHAR* Name)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable ? Variable->GetFloat() : -1.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkyGlitchSettingsTest, "WEM.SkyGlitch.Settings", WEMRoomManagerTests::TestFlags)

bool FSkyGlitchSettingsTest::RunTest(const FString& Parameters)
{
	using namespace SkyGlitchSettingsTestsPrivate;

	// Every setting reaches its console variable, and the variables are put back as they were afterwards.
	TMap<FString, FString> Before;
	for (const TCHAR* Name : Names)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!TestNotNull(FString::Printf(TEXT("%s is registered"), Name), Variable))
		{
			return false;
		}
		Before.Add(Name, Variable->GetString());
	}

	WEMRoomManagerTests::FScopedTestWorld TestWorld;
	ASkyGlitchSettings* Settings = TestWorld.World->SpawnActor<ASkyGlitchSettings>();
	TestNotNull(TEXT("settings spawn"), Settings);

	if (Settings)
	{
		Settings->bEnabled = false;
		Settings->bBlackSky = true;
		Settings->CopyInterval = 0.1f;
		Settings->SkyDistance = 5000.0f;
		Settings->ResetAfter = 4.0f;
		Settings->TurnOnAfter = 3.0f;
		Settings->Apply();

		TestEqual(TEXT("Enabled off"), Get(TEXT("wem.SkyGlitch")), 0.0f);
		TestEqual(TEXT("Black Sky on"), Get(TEXT("wem.SkyGlitch.Black")), 1.0f);
		TestEqual(TEXT("Copy Interval"), Get(TEXT("wem.SkyGlitch.Interval")), 0.1f);
		TestEqual(TEXT("Sky Distance"), Get(TEXT("wem.SkyGlitch.Distance")), 5000.0f);
		TestEqual(TEXT("Reset After"), Get(TEXT("wem.SkyGlitch.ResetAfter")), 4.0f);
		TestEqual(TEXT("Turn On After"), Get(TEXT("wem.SkyGlitch.TurnOnAfter")), 3.0f);

		Settings->bEnabled = true;
		Settings->Apply();
		TestEqual(TEXT("Enabled on again"), Get(TEXT("wem.SkyGlitch")), 1.0f);

		// The portrait sliders reach theirs too, and switching the frame on or off is left to P and the console.
		IConsoleManager::Get().FindConsoleVariable(TEXT("wem.Portrait"))->Set(1, ECVF_SetByConsole);
		Settings->FrameRatio = 0.4615f;
		Settings->FocalLengthScale = 0.8f;
		Settings->PhoneLook = 0.5f;
		Settings->Sharpen = 0.25f;
		Settings->Fringes = 0.0f;
		Settings->HandShake = 2.0f;
		Settings->RollingShutter = 0.02f;
		Settings->Apply();

		TestEqual(TEXT("Frame Ratio"), Get(TEXT("wem.Portrait.Ratio")), 0.4615f);
		TestEqual(TEXT("Focal Length Scale"), Get(TEXT("wem.Portrait.FocalLengthScale")), 0.8f);
		TestEqual(TEXT("Phone Look"), Get(TEXT("wem.Portrait.Effects")), 0.5f);
		TestEqual(TEXT("Sharpen"), Get(TEXT("wem.Portrait.Sharpen")), 0.25f);
		TestEqual(TEXT("Colour Fringes"), Get(TEXT("wem.Portrait.Fringe")), 0.0f);
		TestEqual(TEXT("Hand Shake"), Get(TEXT("wem.Portrait.Shake")), 2.0f);
		TestEqual(TEXT("Rolling Shutter"), Get(TEXT("wem.Portrait.RollingShutter")), 0.02f);
		TestEqual(TEXT("portrait left on"), Get(TEXT("wem.Portrait")), 1.0f);
	}

	for (const TPair<FString, FString>& Pair : Before)
	{
		IConsoleManager::Get().FindConsoleVariable(*Pair.Key)->Set(*Pair.Value, ECVF_SetByConsole);
	}

	return true;
}

#endif
