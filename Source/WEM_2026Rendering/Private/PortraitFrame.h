// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PhoneCamera.h"
#include "SceneViewExtension.h"

struct FMinimalViewInfo;

namespace PortraitFrame
{
	/**
	 * Narrows a view to a portrait frame, Ratio wide for every 1 tall (9:16 is 0.5625), the way a phone turned upright
	 * frames the world: the camera's lens stays, and its wide side now runs from the top of the frame to the bottom.
	 * Nothing turns, stretches or crops; the frame shows more above and below and less to the sides. The frame fills
	 * the viewport's height, with black bars at the sides.
	 *
	 * FocalLengthScale multiplies the lens's focal length: below 1 is wider, above 1 narrower. The camera's own zoom
	 * still works on top. Orthographic views are left as they are.
	 */
	void Fit(FMinimalViewInfo& ViewInfo, float Ratio, float FocalLengthScale);
}

/**
 * Portrait mode, like a phone held upright and filming: game and play-in-editor views draw in a tall, narrow frame in
 * the middle of the window, with the camera's lens turned upright (PortraitFrame::Fit) and the look of a phone camera
 * (PhoneCamera). Editor viewports and Simulate are left alone: they have no player. With more than one player it is
 * always on: the window shows a portrait frame for each (UWEMGameViewportClient), and P does nothing.
 *
 * Console: wem.Portrait 0/1, wem.Portrait.Toggle (also the P key while playing), wem.Portrait.Ratio,
 * wem.Portrait.FocalLengthScale, and the phone look's wem.Portrait.Effects, wem.Portrait.Sharpen, wem.Portrait.Fringe,
 * wem.Portrait.Shake and wem.Portrait.RollingShutter.
 */
class FPortraitFrameViewExtension : public FSceneViewExtensionBase
{
public:
	FPortraitFrameViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupViewPoint(APlayerController* Player, FMinimalViewInfo& InViewInfo) override;

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	/** Below the sky glitch's, so the phone's pass runs after the glitch has painted the sky. */
	virtual int32 GetPriority() const override { return -1; }

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	PhoneCamera::FPass PhonePass;
};
