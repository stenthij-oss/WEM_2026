// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PlatformTileData.generated.h"

class UStaticMesh;

/** Which of a tile's two broad faces, if either, is closed off from being a surface. */
UENUM(BlueprintType)
enum class ETileBlockedFace : uint8
{
	/** Both faces are surfaces, as on any platform. */
	None UMETA(DisplayName = "None"),

	/** The face the tile's top looks out from: up from a floor, out along +X or +Y from a wall. */
	Top UMETA(DisplayName = "Top"),

	/** The face its underside looks out from: down from a ceiling, out along -X or -Y from a wall. */
	Bottom UMETA(DisplayName = "Bottom")
};

/**
 * One kind of tile a platform can be laid from: its size, how it may be turned, how often it is
 * chosen, and what it is built with.
 *
 * Sizes are counted beam to beam - from the centre line of one rim to the centre line of the
 * next - since neighbouring platforms share their rims. A tile of 8 has seven cells of interior
 * between its beams, and two of them side by side cover exactly the ground of one tile of 16.
 * Both sides have to be whole multiples of the room manager's lattice module, or the tile is left
 * out of growth.
 *
 * A tile is laid as a floor and a ceiling at once, or stood up as a wall, whichever way growth
 * finds room for it. Its width runs along the ground either way - along X across a floor, or
 * along the foot of a wall - and its length runs along Y across a floor, or up a wall.
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
	 * How often a beat lays this tile, relative to the other tiles that have somewhere to go.
	 * Zero leaves it out of growth, though it can still start it as the first tile.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	/**
	 * A broad face of the tile that is not a surface: across its interior that side carries
	 * nothing, and never opens into floorspace with the tiles beside it. The beams around it are
	 * shared with its neighbours and stay surfaces as they are.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile|Surfaces")
	ETileBlockedFace BlockedFace = ETileBlockedFace::None;

	/**
	 * Built in place of the plain box across the tile's interior, with its own materials. Left
	 * empty, the box is drawn.
	 *
	 * Modelled in centimetres at the size of the interior - Width - 1 by Length - 1 cells, one
	 * cell thick - around its own centre, with its width along X, its length along Y and its top
	 * facing +Z. The beams around it are built apart, since neighbouring platforms share them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tile|Appearance")
	TObjectPtr<UStaticMesh> Mesh;
};
