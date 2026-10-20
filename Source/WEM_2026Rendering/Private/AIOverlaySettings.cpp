// Copyright Epic Games, Inc. All Rights Reserved.

#include "AIOverlaySettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AIOverlaySettings)

void UAIOverlaySettings::Apply(const EConsoleVariableFlags SetBy, const FName OnlyProperty) const
{
	const auto Set = [SetBy, OnlyProperty](const FName Property, const TCHAR* Name, const auto Value)
	{
		if (OnlyProperty != NAME_None && OnlyProperty != Property)
		{
			return;
		}
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Variable->Set(Value, SetBy);
		}
	};

	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bEnabled), TEXT("wem.AIOverlay"), bEnabled ? 1 : 0);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, GapMin), TEXT("wem.AIOverlay.GapMin"), GapMin);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, GapMax), TEXT("wem.AIOverlay.GapMax"), GapMax);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, RampUp), TEXT("wem.AIOverlay.RampUp"), RampUp);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Hold), TEXT("wem.AIOverlay.Hold"), Hold);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, RampDown), TEXT("wem.AIOverlay.RampDown"), RampDown);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bConstant), TEXT("wem.AIOverlay.Constant"), bConstant ? 1 : 0);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Player), TEXT("wem.AIOverlay.Player"), (int32)Player);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Prompt), TEXT("wem.AIOverlay.Prompt"), *Prompt);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Strength), TEXT("wem.AIOverlay.Strength"), Strength);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Opacity), TEXT("wem.AIOverlay.Opacity"), Opacity);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bClipToMask), TEXT("wem.AIOverlay.ClipToMask"), bClipToMask ? 1 : 0);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, SegmentPercent), TEXT("wem.AIOverlay.SegmentPercent"), SegmentPercent);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bDepth), TEXT("wem.AIOverlay.Depth"), bDepth ? 1 : 0);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, DepthWeight), TEXT("wem.AIOverlay.DepthWeight"), DepthWeight);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bDepthFit), TEXT("wem.AIOverlay.DepthFit"), bDepthFit ? 1 : 0);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, DepthNear), TEXT("wem.AIOverlay.DepthNear"), DepthNear);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, DepthFar), TEXT("wem.AIOverlay.DepthFar"), DepthFar);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Show), TEXT("wem.AIOverlay.Show"), (int32)Show);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, MaxRate), TEXT("wem.AIOverlay.MaxRate"), MaxRate);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bReproject), TEXT("wem.AIOverlay.Reproject"), bReproject ? 1 : 0);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bBlend), TEXT("wem.AIOverlay.Blend"), bBlend ? 1 : 0);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Size), TEXT("wem.AIOverlay.Size"), Size);
	Set(GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, bMatchFrameRate), TEXT("wem.AIOverlay.MatchFrameRate"), bMatchFrameRate ? 1 : 0);
}

void UAIOverlaySettings::PostInitProperties()
{
	Super::PostInitProperties();

	// The saved values are loaded by now. They count as project settings, below anything typed or passed with
	// -ExecCmds, and below [SystemSettings] in DefaultEngine.ini.
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		Apply(ECVF_SetByProjectSetting);
	}
}

#if WITH_EDITOR
void UAIOverlaySettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// The share of segments only once a drag ends: each change moves pieces between the room manager's chunks. The picture
	// size too: each change has the bridge load its model again.
	const FName Property = PropertyChangedEvent.GetMemberPropertyName();
	if ((Property == GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, SegmentPercent) || Property == GET_MEMBER_NAME_CHECKED(UAIOverlaySettings, Size))
		&& PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	// As if typed into the console, so a change on the page also wins over a command typed before it.
	if (PropertyChangedEvent.Property)
	{
		Apply(ECVF_SetByConsole, Property);
	}
}
#endif
