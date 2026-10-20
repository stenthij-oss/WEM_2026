// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Widgets/SLeafWidget.h"

/** How the start screen's sign is drawn. Sizes and places are shares of the frame, so they hold at any resolution. */
struct FStartPromptStyle
{
	FString Text;

	/** The typeface; its size is set from Height. */
	FSlateFontInfo Font;

	FLinearColor Color = FLinearColor::White;
	FLinearColor ShadowColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.6f);

	/** A letter's height, as a share of the frame's height. */
	float Height = 0.04f;

	/** How far down the frame the line's middle sits. */
	float Position = 0.75f;

	float BlinkOnSeconds = 0.6f;
	float BlinkOffSeconds = 0.4f;

	/** The controls, what to press and what it does, a line each; drawn steadily, in two columns, above the sign. */
	TArray<TPair<FString, FString>> Controls;
	FLinearColor ControlsColor = FLinearColor(1.0f, 1.0f, 1.0f, 0.85f);
	float ControlsHeight = 0.024f;
	float ControlsPosition = 0.44f;
	float ControlsLineSpacing = 1.7f;
};

/**
 * The start screen: the controls in two columns - what to press, right-aligned, beside what it does - and below them
 * the sign, one line, blinking on and off. Laid out in the frame the player sees - the portrait frame when wem.Portrait
 * is on - and drawn over the finished picture, so the sky glitch never takes it in.
 */
class SStartPrompt : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SStartPrompt) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetStyle(const FStartPromptStyle& InStyle);

	//~ Begin SWidget Interface
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	//~ End SWidget Interface

private:
	/** The frame the player sees inside the widget: all of it, or the portrait frame in its middle. */
	FSlateRect GetFrame(const FVector2f& Size) const;

	/** The style's font with letters Height high, in the widget's own units. */
	FSlateFontInfo MakeFont(float Height) const;

	/** Draws the controls in the frame; returns the layer after them. */
	int32 PaintControls(const FGeometry& AllottedGeometry, const FSlateRect& Frame, FSlateWindowElementList& OutDrawElements, int32 LayerId) const;

	/** Keeps the widget painting, so it blinks should the viewport be caching what it draws. */
	EActiveTimerReturnType KeepPainting(double InCurrentTime, float InDeltaTime);

	FStartPromptStyle Style;

	/** When the sign first showed, in Slate's time. */
	double ShownSince = 0.0;
};
