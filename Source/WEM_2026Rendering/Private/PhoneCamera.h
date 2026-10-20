// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

struct FMinimalViewInfo;
struct FPostProcessSettings;

/**
 * The phone camera look of the portrait frame (see FPortraitFrameViewExtension), exaggerated on purpose. Two halves:
 * - On the game thread, the view itself: a hand holding the phone, and the phone's picture processing done with the
 *   engine's own post process settings (flat "HDR" local exposure with crunchy detail, punchy colour, glowing
 *   lights without lens flare ghosts, quick auto exposure, little motion blur, everything in focus).
 * - On the render thread, a pass after tonemapping and after the sky glitch (PhoneCamera.usf): the stabiliser's crop,
 *   the rolling shutter, colour fringes, over-sharpening, a vignette and sensor noise.
 * Everything works inside the portrait frame: the engine's effects follow the frame's view rectangle, and the pass
 * measures from the frame's own edges and centre.
 *
 * Console: wem.Portrait.Effects, wem.Portrait.Sharpen, wem.Portrait.Fringe, wem.Portrait.Shake, wem.Portrait.RollingShutter.
 */
namespace PhoneCamera
{
	/** wem.Portrait.Effects, between 0 and 2: how strong the picture processing is. 0 leaves the picture alone. */
	float GetStrength();

	/** wem.Portrait.Shake, 0 or more: how much the hand holding the phone moves. */
	float GetShake();

	/**
	 * How much of the rendered frame the stabiliser keeps, across and down. The view is rendered wider by as much
	 * (AddStabilizerMargin), so the crop costs no field of view and the rolling shutter has a margin to move into.
	 */
	constexpr float CropScale = 0.9f;

	/** Widens the view by the stabiliser's margin, which the pass then crops away again. */
	void AddStabilizerMargin(FMinimalViewInfo& ViewInfo);

	/** A hand holding the phone at Seconds: a slow sway and a little tremor, in degrees, in the camera's own frame. */
	FRotator HandShake(double Seconds, float Strength);

	/** The phone's picture processing at Strength (0 as it was, 1 full, 2 twice as far), as camera overrides. */
	void ApplyLook(FPostProcessSettings& Settings, float Strength);

	/**
	 * How fast the camera turns between two of its rotations Seconds apart, in radians a second: x to the right,
	 * y upward, both in the camera's own frame.
	 */
	FVector2f TurnRate(const FQuat& From, const FQuat& To, double Seconds);

	/**
	 * How far the rolling shutter moves the picture, per unit of height away from the middle row, in frame widths
	 * (x) and heights (y). A phone reads its sensor from the top row down over ReadoutSeconds, so while the camera
	 * turns, each row is taken a moment after the one above: turning right leans uprights to the right at the top,
	 * and tilting up stretches the picture (down squashes it). Projection is the view's, to turn angles into the
	 * frame's units. Clamped to what the stabiliser's margin can hold.
	 */
	FVector2f ShutterSkew(const FVector2f& TurnRadiansPerSecond, float ReadoutSeconds, const FMatrix& Projection);

	/** The pass after tonemapping, and what it remembers of each view between frames to know how fast it turns. */
	class FPass
	{
	public:
		/** Adds the pass for this view if the look is on. Once a frame for each view, on the render thread. */
		void Subscribe_RenderThread(const FSceneView& View, FPostProcessingPassDelegateArray& InOutPassCallbacks);

	private:
		struct FViewState
		{
			FQuat LastRotation = FQuat::Identity;
			double LastSeconds = -1.0;
			FVector2f TurnRate = FVector2f::ZeroVector;
			uint64 LastUsedFrame = 0;
		};

		TMap<uint32, FViewState> Views;
		FCriticalSection ViewsLock;
	};
}
