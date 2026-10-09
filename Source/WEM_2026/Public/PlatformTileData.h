// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PlatformTileData.generated.h"

/** Which ways growth may lay a tile: flat as a floor and ceiling, stood up as a wall, or either. */
UENUM(BlueprintType)
enum class ETileOrientation : uint8
{
	/** Laid whichever way growth finds room for it, as any platform is. */
	Any UMETA(DisplayName = "Horizontal and Vertical"),

	/** Only ever laid flat: a floor and a ceiling, never a wall. */
	HorizontalOnly UMETA(DisplayName = "Horizontal Only"),

	/** Only ever stood up as a wall, never laid flat. */
	VerticalOnly UMETA(DisplayName = "Vertical Only")
};

/**
 * One kind of tile a platform can be laid from: its size, how it may be turned, and how often it
 * is chosen. What fills it is the room manager's structures' business, not the tile's.
 *
 * Sizes are counted beam to beam - from the centre line of one rim to the centre line of the
 * next - since neighbouring platforms share their rims. A tile of 8 has seven cells of interior
 * between its beams, and two of them side by side cover exactly the ground of one tile of 16.
 * Both sides have to be whole multiples of the room manager's lattice module, or the tile is left
 * out of growth.
 *
 * A tile is laid as a floor and a ceiling at once, or stood up as a wall, whichever way growth
 * finds room for it - unless its Orientation keeps it to one of the two. Its width runs along the
 * ground either way - along X across a floor, or along the foot of a wall - and its length runs
 * along Y across a floor, or up a wall.
 */
UCLASS(BlueprintType)
class WEM_2026_API UPlatformTileData : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Cells from beam to beam along the tile's width. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile", meta = (ClampMin = "2"))
	int32 Width = 8;

	/** Cells from beam to beam along the tile's length. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile", meta = (ClampMin = "2"))
	int32 Length = 8;

	/**
	 * Whether the tile may also be laid the other way round, its length running where its width
	 * would. Makes no difference to a square tile.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile")
	bool bCanTurn = false;

	/**
	 * Which ways the tile may be laid. The first tile is always laid flat, so a tile kept to
	 * walls cannot start growth.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile")
	ETileOrientation Orientation = ETileOrientation::Any;

	/**
	 * How often a beat lays this tile, relative to the other tiles that have somewhere to go.
	 * Zero leaves it out of growth, though it can still start it as the first tile.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	/** Whether Orientation lets the tile be stood up as a wall (bVertical) or laid flat (not). */
	bool AllowsOrientation(const bool bVertical) const
	{
		return Orientation == ETileOrientation::Any
			|| Orientation == (bVertical ? ETileOrientation::VerticalOnly : ETileOrientation::HorizontalOnly);
	}
};
