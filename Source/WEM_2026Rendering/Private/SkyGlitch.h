// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RendererInterface.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

/**
 * The sky glitch, after the trail a hung window left across the Windows XP desktop: the sky is
 * never wiped, so wherever a platform was drawn against the sky, a copy of it stays there after the
 * camera moves on. As the camera moves, the platforms smear across the whole sky.
 *
 * Only the final image changes. It runs after tonemapping, so the sky, sun and clouds still render
 * and light the platforms as before, and the platforms themselves always draw live on top: the
 * trails only ever fill what would be sky. Where nothing has been painted yet, the live sky shows,
 * or black with wem.SkyGlitch.Black.
 *
 * What counts as sky is decided by depth: anything farther than wem.SkyGlitch.Distance. Each game
 * view keeps its own trail texture (the size of its view, RGBA16F) and, every
 * wem.SkyGlitch.Interval seconds, paints the platforms as they are drawn now into it. Two
 * full-screen passes on a frame that paints, one on a frame that does not. While it is off, one
 * draws the black sky with wem.SkyGlitch.Black, else none runs.
 * Editor viewports outside of play are left alone.
 *
 * It can run in a cycle: on for wem.SkyGlitch.ResetAfter seconds, then wiped and off for
 * wem.SkyGlitch.TurnOnAfter seconds, then on again over a clean sky. While it is off, a black sky
 * stays black.
 *
 * A title can be laid over the sky and burnt into the trails (SkyGlitchTitle.h): the start screen's.
 *
 * Console: wem.SkyGlitch 0/1, wem.SkyGlitch.Black 0/1, wem.SkyGlitch.Interval, wem.SkyGlitch.Distance,
 * wem.SkyGlitch.ResetAfter, wem.SkyGlitch.TurnOnAfter, wem.SkyGlitch.Clear.
 */
class FSkyGlitchViewExtension : public FSceneViewExtensionBase
{
public:
	FSkyGlitchViewExtension(const FAutoRegister& AutoRegister);

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	/** One view's painted sky. */
	struct FTrail
	{
		TRefCountPtr<IPooledRenderTarget> Texture;
		double LastStampSeconds = 0.0;
		uint64 LastUsedFrame = 0;
		uint32 ClearCount = 0;
		/** When the current on or off time began, in real seconds; below 0 until the view first draws. */
		double PhaseStartSeconds = -1.0;
		/** Wiped by wem.SkyGlitch.ResetAfter and waiting out wem.SkyGlitch.TurnOnAfter. */
		bool bOff = false;
		/** How many titles had been asked to burn in when this trail last looked (SkyGlitchTitle::BurnIn). */
		uint32 Burns = 0;
	};

	/** This view's trail, made on its first frame; also lets go of trails no view has drawn for a while. */
	FTrail& FindTrail_RenderThread(const FSceneView& View);

	FScreenPassTexture DrawAfterTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	/**
	 * By view key. Boxed so a trail stays where it is while the graph that paints it waits to run,
	 * even if another view adds itself to the map in between.
	 */
	TMap<uint32, TUniquePtr<FTrail>> Trails;
	FCriticalSection TrailsLock;
};
