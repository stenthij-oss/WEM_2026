// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EmojiComponent.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SLeafWidget.h"

/** How the emoji menu is drawn. Sizes are shares of the frame's height, so they hold at any resolution. */
struct FEmojiMenuStyle
{
	/** One per emoji, round the ring clockwise from the top. Whoever sets them keeps their textures alive. */
	TArray<FSlateBrush> Icons;

	float Radius = 0.12f;
	float IconSize = 0.065f;
	float PickedScale = 1.45f;

	/** How much the emoji not picked fade back while one is. */
	float UnpickedOpacity = 0.6f;

	FLinearColor BackdropColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.35f);
	FLinearColor RimColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.18f);
	FLinearColor HighlightColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.22f);
};

/**
 * The emoji menu in the middle of the frame and the emoji rising over it once one is sent, as
 * UEmojiComponent says they are this frame. Laid out in the frame the player sees - the portrait
 * frame when wem.Portrait is on - and drawn over the finished picture, so neither the sky glitch nor
 * the phone camera ever takes it in.
 */
class SEmojiOverlay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SEmojiOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetStyle(const FEmojiMenuStyle& InStyle);

	void SetState(const FEmojiScreenState& InState);

	/** The frame the player sees inside a widget this size: all of it, or the portrait frame in its middle. */
	static FSlateRect GetFrame(const FVector2f& Size);

	//~ Begin SWidget Interface
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	//~ End SWidget Interface

private:
	FEmojiMenuStyle Style;
	FEmojiScreenState State;

	/** A round patch, whatever size it is drawn: the backdrop and the glow behind the picked emoji. */
	FSlateBrush Disc;
};
