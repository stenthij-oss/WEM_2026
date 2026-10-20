// Copyright Epic Games, Inc. All Rights Reserved.

#include "EmojiOverlay.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/DrawElements.h"

namespace EmojiOverlayPrivate
{
	/** How far the backdrop reaches past the emoji round the ring, and the glow past the picked one, as shares of an emoji's size. */
	constexpr float BackdropMargin = 0.8f;
	constexpr float HighlightMargin = 1.3f;

	/** The rim round the backdrop, in units. */
	constexpr float RimWidth = 1.5f;

	/** How small the ring starts as it opens, as a share of its full size. */
	constexpr float ClosedScale = 0.55f;
}

void SEmojiOverlay::Construct(const FArguments& InArgs)
{
	SetVisibility(EVisibility::HitTestInvisible);
	SetCanTick(false);

	Disc = FSlateRoundedBoxBrush(FLinearColor::White, 0.0f, FLinearColor::White, EmojiOverlayPrivate::RimWidth);
	Disc.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
}

void SEmojiOverlay::SetStyle(const FEmojiMenuStyle& InStyle)
{
	Style = InStyle;
	Invalidate(EInvalidateWidgetReason::Paint);
}

void SEmojiOverlay::SetState(const FEmojiScreenState& InState)
{
	// Nothing showing before or after: nothing to draw again.
	if (InState.MenuOpen <= 0.0f && InState.Sprites.IsEmpty() && State.MenuOpen <= 0.0f && State.Sprites.IsEmpty())
	{
		return;
	}

	State = InState;
	Invalidate(EInvalidateWidgetReason::Paint);
}

FVector2D SEmojiOverlay::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// It takes whatever the viewport gives it.
	return FVector2D::ZeroVector;
}

FSlateRect SEmojiOverlay::GetFrame(const FVector2f& Size)
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

int32 SEmojiOverlay::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, const int32 LayerId, const FWidgetStyle& InWidgetStyle, const bool bParentEnabled) const
{
	using namespace EmojiOverlayPrivate;

	const FVector2f Size = AllottedGeometry.GetLocalSize();

	if (Size.X <= 0.0f || Size.Y <= 0.0f || (State.MenuOpen <= 0.0f && State.Sprites.IsEmpty()))
	{
		return LayerId;
	}

	const FSlateRect Frame = GetFrame(Size);
	const FVector2f FrameTopLeft(Frame.Left, Frame.Top);
	const FVector2f FrameSize(Frame.Right - Frame.Left, Frame.Bottom - Frame.Top);
	const float Height = FrameSize.Y;

	auto DrawDisc = [&](const int32 Layer, const FVector2f& Centre, const float Diameter, const FLinearColor& Fill, const FLinearColor& Rim)
	{
		FSlateBrush Brush = Disc;
		Brush.OutlineSettings.Color = Rim;
		Brush.OutlineSettings.Width = Rim.A > 0.0f ? RimWidth : 0.0f;
		FSlateDrawElement::MakeBox(OutDrawElements, Layer,
			AllottedGeometry.ToPaintGeometry(FVector2f(Diameter, Diameter), FSlateLayoutTransform(Centre - 0.5f * FVector2f(Diameter, Diameter))),
			&Brush, ESlateDrawEffect::None, Fill);
	};

	auto DrawIcon = [&](const int32 Layer, const int32 Emoji, const FVector2f& Centre, const float IconSize, const float Angle, const float Opacity)
	{
		if (!Style.Icons.IsValidIndex(Emoji) || Opacity <= 0.0f || IconSize <= 0.0f)
		{
			return;
		}

		FSlateDrawElement::MakeBox(OutDrawElements, Layer,
			AllottedGeometry.ToPaintGeometry(FVector2f(IconSize, IconSize), FSlateLayoutTransform(Centre - 0.5f * FVector2f(IconSize, IconSize)),
				FSlateRenderTransform(FQuat2f(Angle)), FVector2f(0.5f, 0.5f)),
			&Style.Icons[Emoji], ESlateDrawEffect::None, FLinearColor(1.0f, 1.0f, 1.0f, Opacity));
	};

	OutDrawElements.PushClip(FSlateClippingZone(AllottedGeometry.ToPaintGeometry(FrameSize, FSlateLayoutTransform(FrameTopLeft))));

	// The emoji rising over the screen, under the menu should it open again.
	for (const FEmojiScreenSprite& Sprite : State.Sprites)
	{
		const FVector2f Centre = FrameTopLeft + Sprite.Centre * FrameSize;
		DrawIcon(LayerId, Sprite.Emoji, Centre, Sprite.Size * Height, Sprite.Angle, Sprite.Opacity);
	}

	// The menu: a ring of emoji on a dark round backdrop in the middle of the frame, growing from smaller as it opens.
	const int32 NumEmoji = Style.Icons.Num();

	if (State.MenuOpen > 0.0f && NumEmoji > 0)
	{
		const float Opacity = FMath::Clamp(State.MenuOpen, 0.0f, 1.0f);
		const float Grown = FMath::Lerp(ClosedScale, 1.0f, 1.0f - FMath::Cube(1.0f - Opacity));
		const FVector2f Centre = FrameTopLeft + 0.5f * FrameSize;
		const float Radius = Style.Radius * Height * Grown;
		const float IconSize = Style.IconSize * Height * Grown;

		DrawDisc(LayerId + 1, Centre, 2.0f * Radius + (1.0f + 2.0f * BackdropMargin) * IconSize,
			Style.BackdropColor.CopyWithNewOpacity(Style.BackdropColor.A * Opacity), Style.RimColor.CopyWithNewOpacity(Style.RimColor.A * Opacity));

		for (int32 Emoji = 0; Emoji < NumEmoji; ++Emoji)
		{
			const float Highlight = State.Highlight.IsValidIndex(Emoji) ? State.Highlight[Emoji] : 0.0f;
			const float Angle = UE_TWO_PI * Emoji / NumEmoji;

			// Clockwise from the top: screen y runs down.
			const FVector2f Place = Centre + Radius * FVector2f(FMath::Sin(Angle), -FMath::Cos(Angle));
			const float Scale = FMath::Lerp(1.0f, Style.PickedScale, Highlight);

			if (Highlight > 0.0f)
			{
				DrawDisc(LayerId + 2, Place, HighlightMargin * IconSize * Scale,
					Style.HighlightColor.CopyWithNewOpacity(Style.HighlightColor.A * Highlight * Opacity), FLinearColor::Transparent);
			}

			const float Shown = State.Picked == INDEX_NONE ? 1.0f : FMath::Lerp(Style.UnpickedOpacity, 1.0f, Highlight);
			DrawIcon(LayerId + 3, Emoji, Place, IconSize * Scale, 0.0f, Opacity * Shown);
		}
	}

	OutDrawElements.PopClip();

	return LayerId + 4;
}
