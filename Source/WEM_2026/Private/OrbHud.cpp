// Copyright Epic Games, Inc. All Rights Reserved.

#include "OrbHud.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "OrbSnippets.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"

namespace OrbHudPrivate
{
	/** Slate sizes fonts in points at 96 DPI: a point is three quarters of a unit. */
	constexpr float PointsPerUnit = 0.75f;

	/** The gap above the counter and to its right, and between it and its orb, as shares of the frame's height and of a letter. */
	constexpr float CounterMargin = 0.035f;
	constexpr float CounterIconGap = 0.35f;

	/** How far the shadow falls from a letter, as a share of the letter's height. */
	constexpr float ShadowOffset = 0.06f;

	/** The space between the lines of a snippet, as a share of the font's line height. */
	constexpr float LineSpacing = 1.05f;

	/** How much smaller a snippet that does not fit is tried again at, each time, as a share of TextHeight. */
	constexpr float ScaleStep = 0.05f;

	TSharedRef<FSlateFontMeasure> GetFontMeasure()
	{
		return FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	}

	bool IsSameRect(const FSlateRect& A, const FSlateRect& B)
	{
		return A.Left == B.Left && A.Top == B.Top && A.Right == B.Right && A.Bottom == B.Bottom;
	}
}

void SOrbHud::Construct(const FArguments& InArgs)
{
	SetVisibility(EVisibility::HitTestInvisible);
	SetCanTick(false);
}

void SOrbHud::SetStyle(const FOrbHudStyle& InStyle)
{
	// The layout only has to be worked out again when what it is measured with, or where it has to fit, changes.
	if (!(InStyle.Font == Style.Font) || InStyle.LetterSpacing != Style.LetterSpacing || InStyle.TextHeight != Style.TextHeight
		|| InStyle.TiltDegrees != Style.TiltDegrees || InStyle.TextPosition != Style.TextPosition || InStyle.PortraitTextPosition != Style.PortraitTextPosition
		|| InStyle.TextTop != Style.TextTop || InStyle.TextBottom != Style.TextBottom || InStyle.SmallestTextScale != Style.SmallestTextScale)
	{
		LaidOutWidth = -1.0f;
	}

	Style = InStyle;
}

void SOrbHud::SetCount(const int32 InCaught, const int32 InTotal)
{
	Caught = InCaught;
	Total = InTotal;
}

void SOrbHud::ShowSnippet(const FString& Text)
{
	Snippet = Text;
	SnippetLetters = Text.Len();
	SnippetStart = FSlateApplication::Get().GetCurrentTime();
	Lines.Reset();
	LaidOutWidth = -1.0f;

	// Each snippet leans a little differently, as handwriting would.
	SnippetTilt = FMath::FRandRange(-0.6f, 0.6f);

	if (!bTimerRunning)
	{
		bTimerRunning = true;
		RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SOrbHud::KeepPainting));
	}
}

bool SOrbHud::IsShowingSnippet() const
{
	float Elapsed, Opacity;
	return GetSnippetTime(FSlateApplication::Get().GetCurrentTime(), Elapsed, Opacity);
}

EActiveTimerReturnType SOrbHud::KeepPainting(double InCurrentTime, float InDeltaTime)
{
	if (IsShowingSnippet())
	{
		return EActiveTimerReturnType::Continue;
	}

	bTimerRunning = false;
	return EActiveTimerReturnType::Stop;
}

FVector2D SOrbHud::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// It takes whatever the viewport gives it.
	return FVector2D::ZeroVector;
}

FSlateRect SOrbHud::GetFrame(const FVector2f& Size) const
{
	static IConsoleVariable* Portrait = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.Portrait"));
	static IConsoleVariable* Ratio = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.Portrait.Ratio"));

	if (Portrait && Ratio && Portrait->GetInt() != 0 && Size.Y > 0.0f && Ratio->GetFloat() > 0.0f && Size.X / Size.Y > Ratio->GetFloat())
	{
		// The frame fills the height, black at the sides.
		const float Width = Size.Y * Ratio->GetFloat();
		const float Left = 0.5f * (Size.X - Width);
		return FSlateRect(Left, 0.0f, Left + Width, Size.Y);
	}

	return FSlateRect(0.0f, 0.0f, Size.X, Size.Y);
}

FSlateFontInfo SOrbHud::MakeFont(const float Height) const
{
	FSlateFontInfo Font = Style.Font;

	// Whole points, so resizing the window does not leave a trail of glyph caches behind.
	Font.Size = FMath::Max(1.0f, FMath::RoundToFloat(Height * OrbHudPrivate::PointsPerUnit));
	Font.LetterSpacing = FMath::RoundToInt32(Style.LetterSpacing * 1000.0f);
	return Font;
}

void SOrbHud::BreakLines(const FSlateFontInfo& Font, const float Width) const
{
	const TSharedRef<FSlateFontMeasure> Measure = OrbHudPrivate::GetFontMeasure();

	Lines.Reset();
	SnippetLetters = 0;

	auto AddLine = [this](const FString& Text)
	{
		FLine& Line = Lines.AddDefaulted_GetRef();
		Line.Text = Text;
		Line.FirstLetter = SnippetLetters;
		SnippetLetters += Text.Len();
	};

	// Each line written in the snippet, wrapped a word at a time where it runs wider than the frame allows.
	TArray<FString> Written;
	Snippet.ParseIntoArray(Written, TEXT("\n"), /*InCullEmpty=*/false);

	for (const FString& Paragraph : Written)
	{
		TArray<FString> Words;
		Paragraph.ParseIntoArrayWS(Words);

		FString Current;

		for (const FString& Word : Words)
		{
			const FString Longer = Current.IsEmpty() ? Word : Current + TEXT(" ") + Word;

			if (!Current.IsEmpty() && Measure->Measure(Longer, Font).X > Width)
			{
				AddLine(Current);
				Current = Word;
			}
			else
			{
				Current = Longer;
			}
		}

		AddLine(Current);
	}
}

void SOrbHud::Fit(const FSlateRect& Frame, const float Start, const float Width) const
{
	using namespace OrbHudPrivate;

	const TSharedRef<FSlateFontMeasure> Measure = GetFontMeasure();
	const float FrameHeight = Frame.Bottom - Frame.Top;
	const float Top = Frame.Top + Style.TextTop * FrameHeight;
	const float Bottom = Frame.Top + Style.TextBottom * FrameHeight;
	const float Smallest = FMath::Clamp(Style.SmallestTextScale, 0.1f, 1.0f);

	// The lines lean up to the right, so the end of the first one stands this much above where it starts.
	const float Rise = Width * FMath::Abs(FMath::Sin(FMath::DegreesToRadians(Style.TiltDegrees + SnippetTilt)));

	// At its own size where it starts, or higher; and only when it fits nowhere at that size, a little smaller.
	for (float Scale = 1.0f;; Scale = FMath::Max(Scale - ScaleStep, Smallest))
	{
		FitLetterHeight = Style.TextHeight * FrameHeight * Scale;
		FitFont = MakeFont(FitLetterHeight);
		BreakLines(FitFont, Width);
		FitLineStep = LineSpacing * Measure->GetMaxCharacterHeight(FitFont);

		if (WEMOrbSnippets::PlaceBlock(Start, Lines.Num() * FitLineStep, Rise, Top, Bottom, FitStart) || Scale <= Smallest + KINDA_SMALL_NUMBER)
		{
			break;
		}
	}

	// Each letter's place along its line, worked out once, for the size settled on.
	for (FLine& Line : Lines)
	{
		Line.LetterOffsets.SetNum(Line.Text.Len());

		for (int32 Index = 0; Index < Line.Text.Len(); ++Index)
		{
			Line.LetterOffsets[Index] = Index == 0 ? 0.0f : static_cast<float>(Measure->Measure(Line.Text.Left(Index), FitFont).X);
		}
	}
}

bool SOrbHud::GetSnippetTime(const double Now, float& OutElapsed, float& OutOpacity) const
{
	if (SnippetStart < 0.0 || Snippet.IsEmpty())
	{
		return false;
	}

	OutElapsed = static_cast<float>(Now - SnippetStart);

	const float Speed = WEMOrbSnippets::GetWritingSpeed(SnippetLetters, Style.LettersPerSecond, Style.LongestWriteSeconds);
	const float Written = FMath::Max(SnippetLetters - 1, 0) / Speed + Style.LetterFadeSeconds;
	const float FadeStart = Written + Style.HoldSeconds + Style.HoldSecondsPerLetter * SnippetLetters;
	const float FadeLength = FMath::Max(Style.FadeOutSeconds, 0.01f);

	OutOpacity = 1.0f - FMath::Clamp((OutElapsed - FadeStart) / FadeLength, 0.0f, 1.0f);
	return OutElapsed < FadeStart + FadeLength;
}

int32 SOrbHud::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, const int32 LayerId, const FWidgetStyle& InWidgetStyle, const bool bParentEnabled) const
{
	using namespace OrbHudPrivate;

	const FVector2f Size = AllottedGeometry.GetLocalSize();

	if (Size.X <= 0.0f || Size.Y <= 0.0f)
	{
		return LayerId;
	}

	const FSlateRect Frame = GetFrame(Size);
	const float FrameWidth = Frame.Right - Frame.Left;
	const float FrameHeight = Frame.Bottom - Frame.Top;
	const TSharedRef<FSlateFontMeasure> Measure = GetFontMeasure();
	const FLinearColor Shadow = Style.ShadowColor;

	auto DrawLetters = [&](const FString& Text, const FVector2f& Position, const float Angle, const FSlateFontInfo& Font, const FLinearColor& Colour, const float Height)
	{
		const FVector2f Fall(ShadowOffset * Height, ShadowOffset * Height);
		const FVector2f TextSize(Measure->Measure(Text, Font));

		FSlateDrawElement::MakeText(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Position + Fall), FSlateRenderTransform(FQuat2f(Angle)), FVector2f::ZeroVector),
			Text, Font, ESlateDrawEffect::None, Shadow.CopyWithNewOpacity(Shadow.A * Colour.A));
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Position), FSlateRenderTransform(FQuat2f(Angle)), FVector2f::ZeroVector),
			Text, Font, ESlateDrawEffect::None, Colour);
	};

	// The count, right-aligned against its orb in the frame's top right corner.
	if (Total > 0)
	{
		const float Height = Style.CounterHeight * FrameHeight;
		const FSlateFontInfo Font = MakeFont(Height);
		const FString Count = FString::Printf(TEXT("%d/%d"), Caught, Total);
		const FVector2f CountSize(Measure->Measure(Count, Font));
		const float IconSize = 1.25f * CountSize.Y;
		const float Margin = CounterMargin * FrameHeight;
		const FVector2f IconPosition(Frame.Right - Margin - IconSize, Frame.Top + Margin);

		if (Style.CounterIcon.GetResourceObject())
		{
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
				AllottedGeometry.ToPaintGeometry(FVector2f(IconSize, IconSize), FSlateLayoutTransform(IconPosition)),
				&Style.CounterIcon, ESlateDrawEffect::None, FLinearColor::White);
		}

		const FVector2f CountPosition(IconPosition.X - CounterIconGap * Height - CountSize.X, IconPosition.Y + 0.5f * (IconSize - CountSize.Y));
		DrawLetters(Count, CountPosition, 0.0f, Font, Style.TextColor, Height);
	}

	// The snippet, a letter at a time, each letter fading in as it is written.
	float Elapsed, Opacity;

	if (GetSnippetTime(FSlateApplication::Get().GetCurrentTime(), Elapsed, Opacity))
	{
		const bool bPortrait = FrameWidth < FrameHeight;
		const FVector2D Place = bPortrait ? Style.PortraitTextPosition : Style.TextPosition;
		const float Width = (bPortrait ? Style.PortraitTextWidth : Style.TextWidth) * FrameWidth;

		if (LaidOutWidth != Width || !IsSameRect(LaidOutFrame, Frame))
		{
			Fit(Frame, Frame.Top + static_cast<float>(Place.Y) * FrameHeight, Width);
			LaidOutFrame = Frame;
			LaidOutWidth = Width;

			// The time is reckoned by the letters laid out, which may be fewer than written.
			GetSnippetTime(FSlateApplication::Get().GetCurrentTime(), Elapsed, Opacity);
		}

		// Screen y runs down, so a line rising to the right turns the other way round.
		const float Angle = -FMath::DegreesToRadians(Style.TiltDegrees + SnippetTilt);
		const FQuat2f Turn(Angle);
		const FVector2f Origin(Frame.Left + static_cast<float>(Place.X) * FrameWidth, FitStart);
		const float PerLetter = 1.0f / WEMOrbSnippets::GetWritingSpeed(SnippetLetters, Style.LettersPerSecond, Style.LongestWriteSeconds);
		const float LetterFade = FMath::Max(Style.LetterFadeSeconds, 0.01f);

		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FLine& Line = Lines[LineIndex];
			const FVector2f LineStart = Origin + Turn.TransformVector(FVector2f(0.0f, LineIndex * FitLineStep));

			for (int32 Index = 0; Index < Line.Text.Len() && Index < Line.LetterOffsets.Num(); ++Index)
			{
				const float Shown = FMath::Clamp((Elapsed - (Line.FirstLetter + Index) * PerLetter) / LetterFade, 0.0f, 1.0f) * Opacity;

				if (Shown <= 0.0f || FChar::IsWhitespace(Line.Text[Index]))
				{
					continue;
				}

				const FVector2f Position = LineStart + Turn.TransformVector(FVector2f(Line.LetterOffsets[Index], 0.0f));
				DrawLetters(Line.Text.Mid(Index, 1), Position, Angle, FitFont, Style.TextColor.CopyWithNewOpacity(Style.TextColor.A * Shown), FitLetterHeight);
			}
		}
	}

	return LayerId + 2;
}
