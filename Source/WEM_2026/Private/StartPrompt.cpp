// Copyright Epic Games, Inc. All Rights Reserved.

#include "StartPrompt.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "StartScreen.h"

namespace StartPromptPrivate
{
	/** Slate sizes fonts in points at 96 DPI: a point is three quarters of a unit. */
	constexpr float PointsPerUnit = 0.75f;

	/** How far the shadow falls from a letter, as a share of the letter's height. */
	constexpr float ShadowOffset = 0.06f;

	/** The gap between what to press and what it does, as a share of a letter's height. */
	constexpr float ColumnGap = 1.2f;

	/** The most of the frame's width the controls may take before they are written smaller. */
	constexpr float ControlsWidth = 0.86f;
}

void SStartPrompt::Construct(const FArguments& InArgs)
{
	SetVisibility(EVisibility::HitTestInvisible);
	SetCanTick(false);

	ShownSince = FSlateApplication::Get().GetCurrentTime();
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SStartPrompt::KeepPainting));
}

void SStartPrompt::SetStyle(const FStartPromptStyle& InStyle)
{
	Style = InStyle;
}

EActiveTimerReturnType SStartPrompt::KeepPainting(double InCurrentTime, float InDeltaTime)
{
	return EActiveTimerReturnType::Continue;
}

FVector2D SStartPrompt::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// It takes whatever the viewport gives it.
	return FVector2D::ZeroVector;
}

FSlateRect SStartPrompt::GetFrame(const FVector2f& Size) const
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

FSlateFontInfo SStartPrompt::MakeFont(const float Height) const
{
	// Whole points, so resizing the window does not leave a trail of glyph caches behind.
	FSlateFontInfo Font = Style.Font;
	Font.Size = FMath::Max(1.0f, FMath::RoundToFloat(Height * StartPromptPrivate::PointsPerUnit));
	return Font;
}

int32 SStartPrompt::PaintControls(const FGeometry& AllottedGeometry, const FSlateRect& Frame, FSlateWindowElementList& OutDrawElements, const int32 LayerId) const
{
	using namespace StartPromptPrivate;

	if (Style.Controls.IsEmpty())
	{
		return LayerId;
	}

	const float FrameWidth = Frame.Right - Frame.Left;
	const float FrameHeight = Frame.Bottom - Frame.Top;
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

	// The widest of each column, at the height asked for; smaller where the two would not fit across the frame.
	float Height = Style.ControlsHeight * FrameHeight;
	FSlateFontInfo Font = MakeFont(Height);
	float LeftWidth = 0.0f;
	float RightWidth = 0.0f;

	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		LeftWidth = 0.0f;
		RightWidth = 0.0f;

		for (const TPair<FString, FString>& Line : Style.Controls)
		{
			LeftWidth = FMath::Max(LeftWidth, static_cast<float>(Measure->Measure(Line.Key, Font).X));
			RightWidth = FMath::Max(RightWidth, static_cast<float>(Measure->Measure(Line.Value, Font).X));
		}

		const float Width = LeftWidth + ColumnGap * Height + RightWidth;
		const float Room = ControlsWidth * FrameWidth;

		if (Width <= Room || Attempt > 0)
		{
			break;
		}

		Height *= Room / Width;
		Font = MakeFont(Height);
	}

	// Centred as a block, the gap between the columns where the two meet.
	const float Gap = ColumnGap * Height;
	const float Step = Style.ControlsLineSpacing * Height;
	const float BlockHeight = Step * (Style.Controls.Num() - 1) + Height;
	const float Seam = Frame.Left + 0.5f * (FrameWidth - (LeftWidth + Gap + RightWidth)) + LeftWidth;
	const float Top = Frame.Top + Style.ControlsPosition * FrameHeight - 0.5f * BlockHeight;
	const FVector2f Fall(ShadowOffset * Height, ShadowOffset * Height);

	auto Draw = [&](const FString& Text, const FVector2f& Position, const FVector2f& TextSize)
	{
		FSlateDrawElement::MakeText(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Position + Fall)),
			Text, Font, ESlateDrawEffect::None, Style.ShadowColor.CopyWithNewOpacity(Style.ShadowColor.A * Style.ControlsColor.A));
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Position)),
			Text, Font, ESlateDrawEffect::None, Style.ControlsColor);
	};

	for (int32 Index = 0; Index < Style.Controls.Num(); ++Index)
	{
		const TPair<FString, FString>& Line = Style.Controls[Index];
		const float Y = Top + Index * Step;
		const FVector2f LeftSize(Measure->Measure(Line.Key, Font));
		const FVector2f RightSize(Measure->Measure(Line.Value, Font));

		Draw(Line.Key, FVector2f(Seam - LeftSize.X, Y), LeftSize);
		Draw(Line.Value, FVector2f(Seam + Gap, Y), RightSize);
	}

	return LayerId + 2;
}

int32 SStartPrompt::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, const int32 LayerId, const FWidgetStyle& InWidgetStyle, const bool bParentEnabled) const
{
	using namespace StartPromptPrivate;

	const FVector2f Size = AllottedGeometry.GetLocalSize();

	if (Size.X <= 0.0f || Size.Y <= 0.0f)
	{
		return LayerId;
	}

	const FSlateRect Frame = GetFrame(Size);
	const int32 SignLayer = PaintControls(AllottedGeometry, Frame, OutDrawElements, LayerId);

	// The sign blinks; the controls stay.
	const double Since = FSlateApplication::Get().GetCurrentTime() - ShownSince;

	if (Style.Text.IsEmpty() || !WEMStartScreen::IsPromptShown(Since, Style.BlinkOnSeconds, Style.BlinkOffSeconds))
	{
		return SignLayer;
	}

	const float FrameWidth = Frame.Right - Frame.Left;
	const float FrameHeight = Frame.Bottom - Frame.Top;
	const float Height = Style.Height * FrameHeight;
	const FSlateFontInfo Font = MakeFont(Height);

	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FVector2f TextSize(Measure->Measure(Style.Text, Font));
	const FVector2f Position(Frame.Left + 0.5f * (FrameWidth - TextSize.X), Frame.Top + Style.Position * FrameHeight - 0.5f * TextSize.Y);
	const FVector2f Fall(ShadowOffset * Height, ShadowOffset * Height);

	FSlateDrawElement::MakeText(OutDrawElements, SignLayer,
		AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Position + Fall)),
		Style.Text, Font, ESlateDrawEffect::None, Style.ShadowColor);
	FSlateDrawElement::MakeText(OutDrawElements, SignLayer + 1,
		AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Position)),
		Style.Text, Font, ESlateDrawEffect::None, Style.Color);

	return SignLayer + 2;
}
