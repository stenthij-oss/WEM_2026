// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UTexture;

/**
 * A title laid over the sky of every game view by the sky glitch, and burnt into its trails.
 *
 * While shown, the title covers the sky only - whatever the glitch counts as platform draws over it - and
 * fades with its opacity. BurnIn paints it, fully covering, into each view's trail, where it stays as the
 * glitch's own: fixed on the screen, painted over only by the platforms' copies as the camera moves, and
 * wiped with the rest of the trail. With the glitch off it is neither shown nor burnt in.
 *
 * Game thread only. The picture's red channel is how much of each pixel it covers; Colour is what it covers
 * it with.
 */
namespace SkyGlitchTitle
{
	/**
	 * Shows the title, centred in each view, Width of the view's width wide - or as wide as MaxHeight of its
	 * height allows, whichever is less - at Opacity. A null texture or an opacity of 0 hides it.
	 */
	WEM_2026RENDERING_API void Show(UTexture* Texture, float Opacity, const FLinearColor& Colour, float Width, float MaxHeight);

	/** Takes the shown title away. Whatever has been burnt in stays. */
	WEM_2026RENDERING_API void Hide();

	/** Paints the title as last shown, fully covering, into every view's trail on the frame drawn next. */
	WEM_2026RENDERING_API void BurnIn();

	/** Hides the title and lets go of its picture, burnt in or not yet. What a trail already holds stays. */
	WEM_2026RENDERING_API void Reset();

	/** Where the title lies in a view of ViewSize pixels, for a picture of TextureSize: its min corner and its size. */
	WEM_2026RENDERING_API FBox2f Fit(const FIntPoint& ViewSize, const FIntPoint& TextureSize, float Width, float MaxHeight);
}
