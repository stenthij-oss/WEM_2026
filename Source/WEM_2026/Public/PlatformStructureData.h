// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PlatformStructureData.generated.h"

class UStaticMesh;
class UStructureCategory;

/**
 * What a structure may be on a segment, in the segment's own frame (see FGridPlatform::Up), not by the
 * way it looks in the world. Any number may be set.
 *
 * A floor is a Roof or a Ground on the face looking along its Up, over a Ceiling on the other - or a
 * Ground over a Ground, the world carrying on upside down beneath it. Never a roof over a ground. A
 * wall is Wall on both faces.
 */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EStructureType : uint8
{
	None = 0 UMETA(Hidden),

	/** A floor's face that is walked on: the one looking along its Up, over a ceiling or a ground, or the one under a ground, looking the other way. */
	Ground = 1 << 0,

	/** A floor's face looking against its Up, under a roof or a ground. */
	Ceiling = 1 << 1,

	/** Either face of a wall: a segment whose Up lies in its plane. */
	Wall = 1 << 2,

	/** A floor's face looking along its Up, over a ceiling. */
	Roof = 1 << 3
};
ENUM_CLASS_FLAGS(EStructureType);

/** Which side of an enclosure a structure stands for. */
UENUM(BlueprintType)
enum class EStructureSide : uint8
{
	/** Outside: a facade, a street, a roof. Anything may stand behind it. */
	Exterior,

	/** Inside a room. Behind it, growth reaches for an exterior (see ARoomManager::bInteriorBacksExterior). */
	Interior,

	/** Either: it fills a face asked to be an interior or an exterior, and binds nothing behind it or beside it. */
	Both
};

/**
 * A mesh that fills the opening of a segment between its beams, and what it means for growth.
 *
 * A segment is one cell thick. A two-sided structure fills all of it, covering both of its faces; a
 * one-sided one fills half, from the segment's mid-plane out to one face, so a segment filled that
 * way takes two of them, one per face.
 *
 * Modelled in Blender at real size, lying on the ground face up, centred on the origin with its
 * transforms applied: its back on Z = 0, its width along X and its length along Y. On a wall X runs
 * across the wall's Up and +Y points along it. Width and length are counted beam to beam, like a tile's, so a
 * structure of 8 x 16 cells is 1.40 m by 3.00 m - the opening between the beams. Its edges are
 * closed, since they show wherever a beam is not built.
 */
UCLASS(BlueprintType)
class WEM_2026_API UPlatformStructureData : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The mesh built across the segment's opening, with its own materials. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Structure")
	TObjectPtr<UStaticMesh> Mesh;

	/** Cells from beam to beam along the structure's width (its X), as authored. The importer measures it from the mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Structure", meta = (ClampMin = "2"))
	int32 Width = 8;

	/** Cells from beam to beam along the structure's length (its Y), as authored. The importer measures it from the mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Structure", meta = (ClampMin = "2"))
	int32 Length = 8;

	/**
	 * Fills the whole thickness of the segment, both of its faces at once. Otherwise it fills one
	 * face, from the segment's mid-plane outward, and another one-sided structure fills the other.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Structure")
	bool bTwoSided = false;

	/**
	 * The categories the structure belongs to. A face is pulled toward the categories of the faces
	 * around it, so these decide what it tends to grow beside. A structure with none takes no part
	 * in that pull. Repeats count once.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Structure")
	TArray<TObjectPtr<UStructureCategory>> Categories;

	/**
	 * What the structure may be on a segment. A one-sided structure is backed by another that pairs with
	 * it: a roof by a ceiling, a ceiling by a roof or a ground, a ground by a ceiling or a ground, a wall
	 * by a wall. A two-sided structure is both faces of a segment at once, so it needs a pair of its
	 * own: Ground for a floor, or Roof and Ceiling; Wall for a wall.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Structure", meta = (Bitmask, BitmaskEnum = "/Script/WEM_2026.EStructureType"))
	int32 Types = static_cast<int32>(EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall);

	/**
	 * Which side of an enclosure it stands for. Behind an interior, growth reaches for an exterior, and a
	 * wall standing on an interior ground is an interior on that side (see ARoomManager's
	 * bInteriorBacksExterior and bWallsFaceInteriorGrounds). Both serves either and binds nothing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Structure")
	EStructureSide Side = EStructureSide::Exterior;

	/**
	 * Whether anything may be built onto it. Off, the segment it fills blocks some of its beams: a
	 * wall its top, so nothing stands on it while more panels can still join beside it, and a floor
	 * or ceiling every beam no segment used before it. A blocked beam is not built unless another
	 * segment needs it. The first segment is always load-bearing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules", meta = (DisplayName = "Load-bearing"))
	bool bLoadBearing = true;

	/**
	 * Whether its face is a surface that objects can stand on. Off, the face carries nothing: no
	 * objects, and no floorspace opening across it. The beams beside it stay wall surface, so walls
	 * can still fold up from a seam next to a face that holds nothing. A two-sided structure answers
	 * for both of its faces.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules", meta = (DisplayName = "Holds items"))
	bool bHoldsItems = true;

	/**
	 * Tends to continue as a straight run of copies of itself: the same structure, on a segment of the
	 * same tile laid the same way round, beside the last one in the same plane. A wall fence runs
	 * along the ground and can turn corners; a floor or ceiling fence runs straight along its width.
	 * Fences are always two-sided.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules", meta = (DisplayName = "Fence", EditCondition = "bTwoSided"))
	bool bFence = false;

	/**
	 * Lets a wall structure be turned on its side to fill a wall of the other shape: an upright 8 x 16
	 * filling a lying 16 x 8. Without it, an upright and a lying wall each need a structure of their
	 * own. Floors and ceilings turn freely regardless.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules")
	bool bMayLieSideways = false;

	/** How often the structure is drawn, relative to the others that fit the same face. Zero leaves it out. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

#if WITH_EDITORONLY_DATA
	/** The .blend this structure was exported from, as its settings file says, so it can be found again. */
	UPROPERTY(VisibleAnywhere, Category = "Source")
	FString SourceBlend;

	/** The object in that .blend. */
	UPROPERTY(VisibleAnywhere, Category = "Source")
	FString SourceObject;

	/** A hash of the .glb and the settings file it was last imported from. An import skips a structure whose files have not changed. */
	UPROPERTY(VisibleAnywhere, Category = "Source")
	FString ImportHash;
#endif

	/** Whether it may fill a face asked to be Wanted: one of that side, or of both. */
	bool StandsFor(const EStructureSide Wanted) const
	{
		return Side == Wanted || Side == EStructureSide::Both;
	}

	bool HasType(const EStructureType Type) const
	{
		return (Types & static_cast<int32>(Type)) != 0;
	}
};
