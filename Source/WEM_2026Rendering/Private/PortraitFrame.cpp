// Copyright Epic Games, Inc. All Rights Reserved.

#include "PortraitFrame.h"

#include "Camera/CameraTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "SceneView.h"
#include "UnrealClient.h"

namespace PortraitFramePrivate
{
	TAutoConsoleVariable<int32> CVarPortrait(
		TEXT("wem.Portrait"),
		0,
		TEXT("1 films the game like a phone held upright: a tall, narrow frame with black bars at the sides, the camera's\n")
		TEXT("lens turned upright, and the look of a phone camera (wem.Portrait.Effects). 0 is the normal camera.\n")
		TEXT("P toggles it while playing."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarPortraitRatio(
		TEXT("wem.Portrait.Ratio"),
		9.0f / 16.0f,
		TEXT("Width of the portrait frame for a height of 1. 0.5625 is 9:16; a taller phone, 9:19.5, is 0.4615."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarPortraitFocalLengthScale(
		TEXT("wem.Portrait.FocalLengthScale"),
		1.0f,
		TEXT("Multiplies the camera's focal length in the portrait frame. 1 keeps the lens (the drone's 8 mm is as wide as a\n")
		TEXT("phone's 0.5x camera); below 1 is wider, above 1 narrower. The zoom still works on top."),
		ECVF_Default);

	/** Which local player a controller is, 0 for the first; 0 too when it is none. */
	int32 GetPlayerIndex(const APlayerController* Player)
	{
		const ULocalPlayer* Local = Player ? Player->GetLocalPlayer() : nullptr;
		const UGameInstance* GameInstance = Local ? Local->GetGameInstance() : nullptr;
		return GameInstance ? FMath::Max(GameInstance->GetLocalPlayers().IndexOfByKey(Local), 0) : 0;
	}

	/** More than one plays in World: the window is split into a portrait frame for each (UWEMGameViewportClient). */
	bool HasSeveralPlayers(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance && GameInstance->GetNumLocalPlayers() > 1;
	}

	void Toggle()
	{
		IConsoleVariable* Variable = CVarPortrait.AsVariable();
		Variable->Set(Variable->GetInt() != 0 ? 0 : 1, ECVF_SetByConsole);
	}

	FAutoConsoleCommand CmdPortraitToggle(
		TEXT("wem.Portrait.Toggle"),
		TEXT("Switches between the portrait frame and the normal camera (wem.Portrait 1 and 0)."),
		FConsoleCommandDelegate::CreateStatic(&Toggle));
}

void PortraitFrame::Fit(FMinimalViewInfo& ViewInfo, const float Ratio, const float FocalLengthScale)
{
	if (Ratio <= 0.0f || ViewInfo.ProjectionMode != ECameraProjectionMode::Perspective)
	{
		return;
	}

	// The lens's wide side, as tan(half its field of view). ViewInfo.FOV is horizontal; a camera that constrains its
	// own aspect ratio to a tall one has its wide side up and down instead.
	const double TanHalfX = FMath::Tan(FMath::DegreesToRadians(FMath::Max(0.001, (double)ViewInfo.FOV)) / 2.0);
	const double TanHalfWide = ViewInfo.bConstrainAspectRatio && ViewInfo.AspectRatio > 0.0f && ViewInfo.AspectRatio < 1.0f
		? TanHalfX / ViewInfo.AspectRatio
		: TanHalfX;

	// That side now runs from the top of the frame to the bottom; a longer focal length shrinks it.
	const double TanHalfY = TanHalfWide / (FocalLengthScale > 0.0f ? FocalLengthScale : 1.0f);

	// A constrained view's FOV is horizontal over the frame's own width.
	ViewInfo.bConstrainAspectRatio = true;
	ViewInfo.AspectRatio = Ratio;
	ViewInfo.FOV = FMath::RadiansToDegrees(2.0 * FMath::Atan(TanHalfY * Ratio));
}

FPortraitFrameViewExtension::FPortraitFrameViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FPortraitFrameViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	// Players' viewports only: in the editor that is a play-in-editor viewport, never the level viewport (which
	// Simulate draws in); outside the editor, every viewport belongs to the game.
	// With more than one player, every screen is a portrait frame, whatever wem.Portrait says.
	const bool bOn = PortraitFramePrivate::CVarPortrait.GetValueOnAnyThread() != 0
		|| (IsInGameThread() && PortraitFramePrivate::HasSeveralPlayers(Context.GetWorld()));

	return bOn && (!GIsEditor || (Context.Viewport && Context.Viewport->IsPlayInEditorViewport()));
}

void FPortraitFrameViewExtension::SetupViewPoint(APlayerController* Player, FMinimalViewInfo& InViewInfo)
{
	using namespace PortraitFramePrivate;

	// Only players' views come here, every time one asks where it looks from, before the engine shapes the picture.
	// So everything below is done to the camera itself, and the engine draws the frame, the bars and its effects
	// inside the frame.
	PortraitFrame::Fit(InViewInfo, CVarPortraitRatio.GetValueOnGameThread(), CVarPortraitFocalLengthScale.GetValueOnGameThread());

	const UWorld* World = Player ? Player->GetWorld() : nullptr;
	const float Shake = PhoneCamera::GetShake();
	if (World && Shake > 0.0f)
	{
		// Each player's hand shakes its own way.
		const double Seconds = World->GetTimeSeconds() + 41.0 * GetPlayerIndex(Player);
		InViewInfo.Rotation = (InViewInfo.Rotation.Quaternion() * PhoneCamera::HandShake(Seconds, Shake).Quaternion()).Rotator();
	}

	const float Strength = PhoneCamera::GetStrength();
	if (Strength > 0.0f)
	{
		PhoneCamera::AddStabilizerMargin(InViewInfo);
		PhoneCamera::ApplyLook(InViewInfo.PostProcessSettings, Strength);
		InViewInfo.PostProcessBlendWeight = 1.0f;
	}
}

void FPortraitFrameViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// After tonemapping, like the sky glitch, and after it (see GetPriority).
	if (Pass == EPostProcessingPass::Tonemap && bIsPassEnabled && View.Family && View.Family->EngineShowFlags.PostProcessing
		&& !View.bIsSceneCapture && !View.bIsReflectionCapture && !View.bIsPlanarReflection && !View.bIsVirtualTexture)
	{
		PhonePass.Subscribe_RenderThread(View, InOutPassCallbacks);
	}
}
