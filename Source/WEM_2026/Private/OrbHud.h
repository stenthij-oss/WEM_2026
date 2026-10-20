// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SLeafWidget.h"

/** How the orb counter and the caught orbs' snippets are drawn. Sizes and places are shares of the frame, so they hold at any resolution. */
struct FOrbHudStyle
{
	/** The typeface; its size is set from TextHeight and CounterHeight. */
	FSlateFontInfo Font;

	FLinearColor TextColor = FLinearColor::Yellow;
	FLinearColor ShadowColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.6f);

	/** A letter's height, as a share of the frame's height. */
	float TextHeight = 0.03f;
	float CounterHeight = 0.032f;

	/** Room added after every letter, as a share of the letter's height. */
	float LetterSpacing = 0.12f;

	float LettersPerSecond = 10.0f;
	float LetterFadeSeconds = 0.3f;

	/** The longest a snippet takes to be written out: a longer one is written faster. */
	float LongestWriteSeconds = 15.0f;

	/** How long a snippet stays once written out: HoldSeconds, and HoldSecondsPerLetter more for every letter. */
	float HoldSeconds = 2.5f;
	float HoldSecondsPerLetter = 0.05f;
	float FadeOutSeconds = 1.5f;

	/** How far each line leans, its end higher than its start, in degrees. */
	float TiltDegrees = 3.0f;

	/** Where the first line starts and how wide the lines run, as shares of the frame, in a wide frame and in a portrait one. */
	FVector2D TextPosition = FVector2D(0.28, 0.54);
	float TextWidth = 0.36f;
	FVector2D PortraitTextPosition = FVector2D(0.1, 0.56);
	float PortraitTextWidth = 0.8f;

	/**
	 * How high a long snippet may start and how low it may end, as shares of the frame's height. One that would run past
	 * TextBottom starts higher, up to TextTop, and only then is written smaller, down to SmallestTextScale of TextHeight.
	 */
	float TextTop = 0.12f;
	float TextBottom = 0.94f;
	float SmallestTextScale = 0.6f;

	/** The picture beside the count. Whoever sets it keeps its texture alive. */
	FSlateBrush CounterIcon;
};

/**
 * The orb count in the frame's top right corner, with an orb beside it, and the snippet a caught
 * orb shows, written out a letter at a time on lines that lean a little, as in The Path. A snippet
 * too long to fit below where the text starts starts higher, and if it still does not fit is
 * written smaller; a long one is also written faster, so none takes longer than LongestWriteSeconds.
 * Laid out in the frame the player sees - the portrait frame when wem.Portrait is on - and drawn
 * over the finished picture, so neither the sky glitch nor any other pass of the image ever takes
 * it in.
 */
class SOrbHud : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SOrbHud) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetStyle(const FOrbHudStyle& InStyle);

	void SetCount(int32 InCaught, int32 InTotal);

	/** Starts writing out a snippet, in place of any still showing. */
	void ShowSnippet(const FString& Text);

	/** Whether a snippet is still being written out or showing. */
	bool IsShowingSnippet() const;

	//~ Begin SWidget Interface
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	//~ End SWidget Interface

private:
	/** One line of a snippet laid out: its letters, where each starts along the line, and the index of its first letter in the snippet. */
	struct FLine
	{
		FString Text;
		TArray<float> LetterOffsets;
		int32 FirstLetter = 0;
	};

	/** The frame the player sees inside the widget: all of it, or the portrait frame in its middle. */
	FSlateRect GetFrame(const FVector2f& Size) const;

	/** The style's font at a size, in the widget's own units: Height is a letter's height. */
	FSlateFontInfo MakeFont(float Height) const;

	/** Breaks the snippet into lines no wider than Width, for the font given, a word at a time and at the line breaks written in it. */
	void BreakLines(const FSlateFontInfo& Font, float Width) const;

	/**
	 * Fits the snippet in the frame: at its size where it starts, if it fits there; else starting higher; else smaller.
	 * Leaves the lines laid out, their letters placed, and where and how large they are drawn.
	 */
	void Fit(const FSlateRect& Frame, float Start, float Width) const;

	/** Seconds since the snippet started, and how far it has faded from 1 to 0 overall. False once it has gone. */
	bool GetSnippetTime(double Now, float& OutElapsed, float& OutOpacity) const;

	/** Keeps the widget painting while a snippet changes, should the viewport be caching what it draws. */
	EActiveTimerReturnType KeepPainting(double InCurrentTime, float InDeltaTime);

	FOrbHudStyle Style;
	int32 Caught = 0;
	int32 Total = 0;

	FString Snippet;
	double SnippetStart = -1.0;
	float SnippetTilt = 0.0f;
	bool bTimerRunning = false;

	/** Letters to write out: the snippet's, then those laid out, once spaces at line breaks are gone. */
	mutable int32 SnippetLetters = 0;

	/** The layout, kept until the snippet, the frame or the style changes, and where it is drawn and how large. */
	mutable TArray<FLine> Lines;
	mutable FSlateFontInfo FitFont;
	mutable float FitLetterHeight = 0.0f;
	mutable float FitStart = 0.0f;
	mutable float FitLineStep = 0.0f;
	mutable FSlateRect LaidOutFrame = FSlateRect(-1.0f, -1.0f, -1.0f, -1.0f);
	mutable float LaidOutWidth = -1.0f;
};
