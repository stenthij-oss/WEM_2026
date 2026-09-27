// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PlacementTypes.h"
#include "Widgets/SLeafWidget.h"

/** What the footprint view draws: a mesh's geometry and the zones its archetype lays round it. */
struct FPlacementFootprintView
{
	FPlacementGeometry Geometry;
	TArray<FExclusionZone> Zones;
	double CellSize = 20.0;

	/** The geometry is a catalog row's with its overrides laid over it, not what the builder derived. */
	bool bOverridesApplied = false;
};

/**
 * A mesh's footprint as the placer sees it, seen from above: front (+X) to the right and the
 * object's own right (+Y) downward. Edge faces are bars on the cell borders - back black, front
 * yellow, left and right blue, the ends of each run faded - with special runs and their ids beside
 * them. Surface layers shade their cells green, the open space under each cell shades it blue, and
 * the archetype's zones lie round it in red. Hovering a cell says what is known about it.
 *
 * Read-only. It asks for its data on every paint, so it follows the row as it is edited.
 */
class SPlacementFootprint : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPlacementFootprint) {}
		/** Fills in what to draw. False when there is nothing to draw. */
		SLATE_ARGUMENT(TFunction<bool(FPlacementFootprintView&)>, OnGetView)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;

private:
	/** Where things go on the widget for one view. */
	struct FLayout
	{
		float CellPixels = 20.0f;

		/** Cells of room left round the footprint for its zones. */
		int32 Margin = 1;

		/** Top left of the footprint's local cell (0, 0). */
		FVector2f FootprintOrigin = FVector2f::ZeroVector;

		FVector2f GridSize = FVector2f::ZeroVector;
		FVector2f GridOrigin = FVector2f::ZeroVector;

		/** Lines of text under the grid. */
		TArray<TPair<FString, FLinearColor>> Legend;

		FVector2f DesiredSize = FVector2f(300.0f, 40.0f);
	};

	bool GetView(FPlacementFootprintView& OutView) const;
	FLayout MakeLayout(const FPlacementFootprintView& View) const;
	FText GetHoverText() const;

	TFunction<bool(FPlacementFootprintView&)> OnGetView;

	/** The footprint cell under the mouse, or INDEX_NONE on both when there is none. */
	FIntPoint HoveredCell = FIntPoint(INDEX_NONE, INDEX_NONE);
};
