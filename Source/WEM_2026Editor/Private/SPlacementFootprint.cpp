// Copyright Epic Games, Inc. All Rights Reserved.

#include "SPlacementFootprint.h"

#include "Fonts/SlateFontInfo.h"
#include "PlacementMath.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "SPlacementFootprint"

namespace PlacementFootprintPrivate
{
	constexpr float LeftPadding = 56.0f;
	constexpr float TopPadding = 22.0f;
	constexpr float LineHeight = 15.0f;
	constexpr float GridTarget = 320.0f;

	/** Tallest an object can be, in cells, and still fit between neighbouring parallel segments. */
	constexpr int32 SingleGapCells = 7;

	const FLinearColor BackColour(0.0f, 0.0f, 0.0f);
	const FLinearColor FrontColour(1.0f, 0.85f, 0.0f);
	const FLinearColor SideColour(0.15f, 0.4f, 1.0f);
	const FLinearColor SpecialColour(1.0f, 0.45f, 0.0f);
	const FLinearColor ZoneColour(1.0f, 0.1f, 0.1f, 0.25f);
	const FLinearColor SurfaceColour(0.1f, 0.85f, 0.2f, 0.35f);
	const FLinearColor OpenBelowColour(0.3f, 0.6f, 1.0f);
	const FLinearColor GridColour(0.35f, 0.35f, 0.35f);
	const FLinearColor TextColour(0.85f, 0.85f, 0.85f);
	const FLinearColor WarningColour(1.0f, 0.6f, 0.2f);

	FLinearColor GetSideColour(const EObjectSide Side)
	{
		switch (Side)
		{
		case EObjectSide::Back: return BackColour;
		case EObjectSide::Front: return FrontColour;
		default: return SideColour;
		}
	}

	FString DescribeClearance(const float ClearanceCm)
	{
		return ClearanceCm <= 0.0f ? TEXT("unlimited") : FString::Printf(TEXT("%.0f cm"), ClearanceCm);
	}
}

void SPlacementFootprint::Construct(const FArguments& InArgs)
{
	OnGetView = InArgs._OnGetView;
	SetToolTipText(MakeAttributeSP(this, &SPlacementFootprint::GetHoverText));
}

bool SPlacementFootprint::GetView(FPlacementFootprintView& OutView) const
{
	return OnGetView && OnGetView(OutView) && WEMPlacement::GetFootprintCellCount(OutView.Geometry.SizeCells) > 0;
}

SPlacementFootprint::FLayout SPlacementFootprint::MakeLayout(const FPlacementFootprintView& View) const
{
	using namespace PlacementFootprintPrivate;

	FLayout Layout;
	const FIntVector& Size = View.Geometry.SizeCells;

	for (const FExclusionZone& Zone : View.Zones)
	{
		Layout.Margin = FMath::Max(Layout.Margin, Zone.DepthCells);
	}

	const int32 CellsAcross = Size.X + 2 * Layout.Margin;
	const int32 CellsDown = Size.Y + 2 * Layout.Margin;

	Layout.CellPixels = FMath::Clamp(FMath::FloorToFloat(GridTarget / FMath::Max(CellsAcross, CellsDown)), 6.0f, 36.0f);
	Layout.GridOrigin = FVector2f(LeftPadding, TopPadding);
	Layout.GridSize = FVector2f(CellsAcross, CellsDown) * Layout.CellPixels;
	Layout.FootprintOrigin = Layout.GridOrigin + FVector2f(Layout.Margin, Layout.Margin) * Layout.CellPixels;

	const FVector BoundsSize = View.Geometry.LocalBoundsCm.IsValid ? View.Geometry.LocalBoundsCm.GetSize() : FVector::ZeroVector;

	Layout.Legend.Emplace(FString::Printf(TEXT("%d x %d x %d cells, bounds %.1f x %.1f x %.1f cm"),
		Size.X, Size.Y, Size.Z, BoundsSize.X, BoundsSize.Y, BoundsSize.Z), TextColour);

	if (Size.Z > SingleGapCells)
	{
		Layout.Legend.Emplace(FString::Printf(TEXT("%d cells tall: only fits where parallel segments are 16 or more cells apart"), Size.Z), WarningColour);
	}

	for (int32 LayerIndex = 0; LayerIndex < View.Geometry.Surfaces.Num(); ++LayerIndex)
	{
		const FDerivedSurfaceLayer& Layer = View.Geometry.Surfaces[LayerIndex];
		const int32 Cells = Layer.CellMask.FilterByPredicate([](const uint8 Cell) { return Cell != 0; }).Num();

		Layout.Legend.Emplace(FString::Printf(TEXT("surface %d: %.1f cm up, clearance %s, %d cells"),
			LayerIndex, Layer.HeightCm, *DescribeClearance(Layer.ClearanceCm), Cells), SurfaceColour.CopyWithNewOpacity(1.0f));
	}

	for (const FSpecialEdge& Special : View.Geometry.SpecialEdges)
	{
		Layout.Legend.Emplace(FString::Printf(TEXT("special %d: %s faces %d-%d"),
			Special.Id, WEMPlacement::GetSideName(Special.Side), Special.Start, Special.Start + Special.Count - 1), SpecialColour);
	}

	if (!View.Zones.IsEmpty())
	{
		Layout.Legend.Emplace(FString::Printf(TEXT("%d exclusion zone(s) from the archetype"), View.Zones.Num()), ZoneColour.CopyWithNewOpacity(1.0f));
	}

	if (View.bOverridesApplied)
	{
		Layout.Legend.Emplace(TEXT("the row's overrides are applied"), WarningColour);
	}

	Layout.DesiredSize = FVector2f(
		FMath::Max(Layout.GridOrigin.X + Layout.GridSize.X + 16.0f, 360.0f),
		Layout.GridOrigin.Y + Layout.GridSize.Y + 8.0f + Layout.Legend.Num() * LineHeight);

	return Layout;
}

FVector2D SPlacementFootprint::ComputeDesiredSize(float) const
{
	FPlacementFootprintView View;
	return GetView(View) ? FVector2D(MakeLayout(View).DesiredSize) : FVector2D(300.0, 20.0);
}

int32 SPlacementFootprint::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	using namespace PlacementFootprintPrivate;
	using namespace WEMPlacement;

	const FSlateBrush* White = FAppStyle::GetBrush(TEXT("WhiteBrush"));
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 8);

	auto Box = [&](const int32 Layer, const FVector2f& Position, const FVector2f& Size, const FLinearColor& Colour)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(Position)), White, ESlateDrawEffect::None, Colour);
	};

	auto Line = [&](const int32 Layer, const FVector2f& From, const FVector2f& To, const FLinearColor& Colour, const float Thickness)
	{
		FSlateDrawElement::MakeLines(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(), TArray<FVector2f>{ From, To }, ESlateDrawEffect::None, Colour, true, Thickness);
	};

	auto Text = [&](const int32 Layer, const FVector2f& Position, const FString& String, const FLinearColor& Colour)
	{
		FSlateDrawElement::MakeText(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(FVector2f(400.0f, LineHeight), FSlateLayoutTransform(Position)), String, Font, ESlateDrawEffect::None, Colour);
	};

	FPlacementFootprintView View;

	if (!GetView(View))
	{
		Text(LayerId, FVector2f(4.0f, 2.0f), TEXT("No footprint to show."), TextColour);
		return LayerId;
	}

	const FLayout Layout = MakeLayout(View);
	const FPlacementGeometry& Geometry = View.Geometry;
	const FIntVector& Size = Geometry.SizeCells;
	const float Cell = Layout.CellPixels;

	auto CellPosition = [&](const FIntPoint& LocalCell)
	{
		return Layout.FootprintOrigin + FVector2f(LocalCell.X, LocalCell.Y) * Cell;
	};

	// Background.
	Box(LayerId, Layout.GridOrigin, Layout.GridSize, FLinearColor(0.02f, 0.02f, 0.02f));

	// The archetype's zones, round the footprint.
	TArray<FIntPoint> ZoneCells;
	for (const FExclusionZone& Zone : View.Zones)
	{
		GatherZoneCells(Size, Zone, ZoneCells);

		for (const FIntPoint& ZoneCell : ZoneCells)
		{
			Box(LayerId + 1, CellPosition(ZoneCell), FVector2f(Cell), ZoneColour);
		}
	}

	// Each cell: shaded by the space open under it, darker where the mesh has nothing at all.
	const double Height = FMath::Max(1.0, Geometry.LocalBoundsCm.IsValid ? Geometry.LocalBoundsCm.GetSize().Z : Size.Z * View.CellSize);

	for (int32 X = 0; X < Size.X; ++X)
	{
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			const int32 Index = GetFootprintIndex(Size, X, Y);
			const bool bHasGeometry = !Geometry.FootprintMask.IsValidIndex(Index) || Geometry.FootprintMask[Index] != 0;
			const float Open = Geometry.OpenBelowCm.IsValidIndex(Index) ? static_cast<float>(FMath::Clamp(Geometry.OpenBelowCm[Index] / Height, 0.0, 1.0)) : 0.0f;

			const FLinearColor Base = bHasGeometry ? FLinearColor(0.16f, 0.16f, 0.16f) : FLinearColor(0.06f, 0.06f, 0.06f);
			Box(LayerId + 2, CellPosition(FIntPoint(X, Y)), FVector2f(Cell), FMath::Lerp(Base, OpenBelowColour, 0.6f * Open));

			// Surfaces, each layer inset a little further so several can be told apart.
			for (int32 LayerIndex = 0; LayerIndex < Geometry.Surfaces.Num(); ++LayerIndex)
			{
				const FDerivedSurfaceLayer& Layer = Geometry.Surfaces[LayerIndex];

				if (Layer.CellMask.IsValidIndex(Index) && Layer.CellMask[Index] != 0)
				{
					const float Inset = FMath::Min(2.0f + 3.0f * LayerIndex, 0.4f * Cell);
					Box(LayerId + 3, CellPosition(FIntPoint(X, Y)) + FVector2f(Inset), FVector2f(Cell - 2.0f * Inset), SurfaceColour);
				}
			}
		}
	}

	// The grid over the footprint.
	for (int32 X = 0; X <= Size.X; ++X)
	{
		Line(LayerId + 4, CellPosition(FIntPoint(X, 0)), CellPosition(FIntPoint(X, Size.Y)), GridColour, 1.0f);
	}

	for (int32 Y = 0; Y <= Size.Y; ++Y)
	{
		Line(LayerId + 4, CellPosition(FIntPoint(0, Y)), CellPosition(FIntPoint(Size.X, Y)), GridColour, 1.0f);
	}

	// Edge faces as bars on the cell borders, the ends of each run faded.
	TArray<FEdgeFace> Faces;
	GatherEdgeFaces(Size, {}, Faces);

	auto GetFaceEnds = [&](const FEdgeFace& Face, const float Outset, FVector2f& OutFrom, FVector2f& OutTo)
	{
		const FVector2f TopLeft = CellPosition(Face.Cell);
		const FVector2f Direction(Face.Direction.X, Face.Direction.Y);
		const FVector2f Middle = TopLeft + FVector2f(0.5f * Cell) + Direction * (0.5f * Cell + Outset);
		const FVector2f Along(-Direction.Y, Direction.X);

		OutFrom = Middle - Along * (0.5f * Cell - 1.0f);
		OutTo = Middle + Along * (0.5f * Cell - 1.0f);
	};

	for (const FEdgeFace& Face : Faces)
	{
		FVector2f From, To;
		GetFaceEnds(Face, 0.0f, From, To);

		const FLinearColor Colour = GetSideColour(Face.Side);
		Line(LayerId + 5, From, To, Face.bCorner ? Colour.CopyWithNewOpacity(0.4f) : Colour, 3.0f);
	}

	// Special runs, beside their edge, with their id.
	for (const FSpecialEdge& Special : Geometry.SpecialEdges)
	{
		const FEdgeFace* First = Faces.FindByPredicate([&Special](const FEdgeFace& Face) { return Face.Side == Special.Side && Face.Index == Special.Start; });
		const FEdgeFace* Last = Faces.FindByPredicate([&Special](const FEdgeFace& Face) { return Face.Side == Special.Side && Face.Index == Special.Start + Special.Count - 1; });

		if (!First || !Last)
		{
			continue;
		}

		FVector2f FirstFrom, FirstTo, LastFrom, LastTo;
		GetFaceEnds(*First, 5.0f, FirstFrom, FirstTo);
		GetFaceEnds(*Last, 5.0f, LastFrom, LastTo);

		const FVector2f Start = (FirstFrom - LastTo).SizeSquared() > (FirstTo - LastFrom).SizeSquared() ? FirstFrom : FirstTo;
		const FVector2f End = Start == FirstFrom ? LastTo : LastFrom;
		Line(LayerId + 5, Start, End, SpecialColour, 2.0f);

		const FVector2f Outward(First->Direction.X, First->Direction.Y);
		Text(LayerId + 6, 0.5f * (Start + End) + Outward * 6.0f - FVector2f(3.0f, 7.0f), FString::FromInt(Special.Id), SpecialColour);
	}

	// Which way the object faces, and which way is its right.
	const FVector2f FrontLabel(Layout.GridOrigin.X + Layout.GridSize.X - 70.0f, 4.0f);
	Text(LayerId + 6, FrontLabel, TEXT("front +X \u2192"), FrontColour);
	Text(LayerId + 6, FVector2f(4.0f, Layout.GridOrigin.Y), TEXT("right"), SideColour);
	Text(LayerId + 6, FVector2f(4.0f, Layout.GridOrigin.Y + LineHeight), TEXT("+Y \u2193"), SideColour);

	// What it all adds up to, under the grid.
	float LegendY = Layout.GridOrigin.Y + Layout.GridSize.Y + 6.0f;
	for (const TPair<FString, FLinearColor>& Entry : Layout.Legend)
	{
		Text(LayerId + 6, FVector2f(4.0f, LegendY), Entry.Key, Entry.Value);
		LegendY += LineHeight;
	}

	// The hovered cell, outlined.
	if (HoveredCell.X != INDEX_NONE)
	{
		const FVector2f TopLeft = CellPosition(HoveredCell);
		const FVector2f Corners[4] = { TopLeft, TopLeft + FVector2f(Cell, 0.0f), TopLeft + FVector2f(Cell), TopLeft + FVector2f(0.0f, Cell) };

		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Line(LayerId + 6, Corners[Corner], Corners[(Corner + 1) % 4], FLinearColor::White, 1.5f);
		}
	}

	return LayerId + 6;
}

FReply SPlacementFootprint::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	HoveredCell = FIntPoint(INDEX_NONE, INDEX_NONE);

	FPlacementFootprintView View;

	if (GetView(View))
	{
		const FLayout Layout = MakeLayout(View);
		const FVector2f Local = FVector2f(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));
		const FVector2f InFootprint = (Local - Layout.FootprintOrigin) / Layout.CellPixels;
		const FIntPoint Cell(FMath::FloorToInt32(InFootprint.X), FMath::FloorToInt32(InFootprint.Y));

		if (Cell.X >= 0 && Cell.Y >= 0 && Cell.X < View.Geometry.SizeCells.X && Cell.Y < View.Geometry.SizeCells.Y)
		{
			HoveredCell = Cell;
		}
	}

	return FReply::Unhandled();
}

void SPlacementFootprint::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	SLeafWidget::OnMouseLeave(MouseEvent);
	HoveredCell = FIntPoint(INDEX_NONE, INDEX_NONE);
}

FText SPlacementFootprint::GetHoverText() const
{
	using namespace WEMPlacement;

	FPlacementFootprintView View;

	if (HoveredCell.X == INDEX_NONE || !GetView(View))
	{
		return FText::GetEmpty();
	}

	const FPlacementGeometry& Geometry = View.Geometry;
	const int32 Index = GetFootprintIndex(Geometry.SizeCells, HoveredCell.X, HoveredCell.Y);

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("cell (%d, %d)"), HoveredCell.X, HoveredCell.Y));

	if (Geometry.OpenBelowCm.IsValidIndex(Index))
	{
		Lines.Add(FString::Printf(TEXT("open below: %.1f cm"), Geometry.OpenBelowCm[Index]));
	}

	TArray<FEdgeFace> Faces;
	GatherEdgeFaces(Geometry.SizeCells, {}, Faces);

	for (const FEdgeFace& Face : Faces)
	{
		if (Face.Cell == HoveredCell)
		{
			Lines.Add(FString::Printf(TEXT("%s edge, face %d%s"), GetSideName(Face.Side), Face.Index, Face.bCorner ? TEXT(" (corner)") : TEXT("")));
		}
	}

	for (int32 LayerIndex = 0; LayerIndex < Geometry.Surfaces.Num(); ++LayerIndex)
	{
		const FDerivedSurfaceLayer& Layer = Geometry.Surfaces[LayerIndex];

		if (Layer.CellMask.IsValidIndex(Index) && Layer.CellMask[Index] != 0)
		{
			Lines.Add(FString::Printf(TEXT("surface %d at %.1f cm"), LayerIndex, Layer.HeightCm));
		}
	}

	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

#undef LOCTEXT_NAMESPACE
