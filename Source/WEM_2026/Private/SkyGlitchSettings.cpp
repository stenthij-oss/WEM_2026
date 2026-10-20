// Copyright Epic Games, Inc. All Rights Reserved.

#include "SkyGlitchSettings.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDeviceNull.h"

namespace SkyGlitchSettingsPrivate
{
	// The variables live in WEM_2026Rendering and are found by name, so this module needs nothing of it.
	// Each is set only when it differs: setting wem.SkyGlitch at all wipes the trails. Set as the console
	// sets them, so the console and this actor can both still change them and the last one wins.

	void SetInt(const TCHAR* Name, const int32 Value)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (Variable && Variable->GetInt() != Value)
		{
			Variable->Set(Value, ECVF_SetByConsole);
		}
	}

	void SetFloat(const TCHAR* Name, const float Value)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (Variable && Variable->GetFloat() != Value)
		{
			Variable->Set(Value, ECVF_SetByConsole);
		}
	}
}

void ASkyGlitchSettings::Apply() const
{
	using namespace SkyGlitchSettingsPrivate;

	SetInt(TEXT("wem.SkyGlitch"), bEnabled ? 1 : 0);
	SetInt(TEXT("wem.SkyGlitch.Black"), bBlackSky ? 1 : 0);
	SetFloat(TEXT("wem.SkyGlitch.Interval"), CopyInterval);
	SetFloat(TEXT("wem.SkyGlitch.Distance"), SkyDistance);
	SetFloat(TEXT("wem.SkyGlitch.ResetAfter"), ResetAfter);
	SetFloat(TEXT("wem.SkyGlitch.TurnOnAfter"), TurnOnAfter);

	// The portrait frame's variables, found the same way. Whether the frame is on (wem.Portrait) is left to P and the
	// console, so editing a slider here never switches it off mid-play.
	SetFloat(TEXT("wem.Portrait.Ratio"), FrameRatio);
	SetFloat(TEXT("wem.Portrait.FocalLengthScale"), FocalLengthScale);
	SetFloat(TEXT("wem.Portrait.Effects"), PhoneLook);
	SetFloat(TEXT("wem.Portrait.Sharpen"), Sharpen);
	SetFloat(TEXT("wem.Portrait.Fringe"), Fringes);
	SetFloat(TEXT("wem.Portrait.Shake"), HandShake);
	SetFloat(TEXT("wem.Portrait.RollingShutter"), RollingShutter);
}

void ASkyGlitchSettings::ClearTrails()
{
	IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(TEXT("wem.SkyGlitch.Clear"));
	if (IConsoleCommand* Command = Object ? Object->AsCommand() : nullptr)
	{
		FOutputDeviceNull Ignored;
		Command->Execute({}, GetWorld(), Ignored);
	}
}

void ASkyGlitchSettings::BeginPlay()
{
	Super::BeginPlay();

	Apply();
}

#if WITH_EDITOR
void ASkyGlitchSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	Apply();
}
#endif
