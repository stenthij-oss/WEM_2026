// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "PlacementTypes.generated.h"

class UPlacementArchetype;

/**
 * A side of a placed object, named in its own frame as authored in the mesh, standing up: front
 * along local +X, right along +Y, top along +Z. Left and right are the object's own, not a
 * viewer's facing it.
 *
 * Which local direction each side is lives in WEMPlacement::GetSideDirection alone, so the
 * convention can be flipped there without touching anything that reads it.
 */
UENUM(BlueprintType, meta = (Bitflags))
enum class EObjectSide : uint8
{
	Front UMETA(DisplayName = "Front"),
	Back UMETA(DisplayName = "Back"),
	Right UMETA(DisplayName = "Right"),
	Left UMETA(DisplayName = "Left"),
	Top UMETA(DisplayName = "Top"),
	Bottom UMETA(DisplayName = "Bottom")
};

/** What an object can stand on: a segment's face, or a surface on top of another object. */
UENUM(BlueprintType, meta = (Bitflags))
enum class ESupportKind : uint8
{
	Segment UMETA(DisplayName = "Segment"),
	ObjectSurface UMETA(DisplayName = "Object Surface")
};

/** How an archetype chooses among its rules. */
UENUM(BlueprintType)
enum class ERuleSelection : uint8
{
	/** In order: the first rule that finds a place wins. */
	Priority UMETA(DisplayName = "Priority"),

	/** By weight, among the rules not yet tried; a rule that finds nothing drops out and the roll is made again. */
	Weighted UMETA(DisplayName = "Weighted")
};

UENUM(BlueprintType)
enum class ERuleKind : uint8
{
	/** Anywhere the object fits. Nothing has to line up, though nothing stops it touching. */
	Surface UMETA(DisplayName = "Surface"),

	/** Every contact has to hold. A corner is two of them, on adjacent sides. */
	Edge UMETA(DisplayName = "Edge")
};

/** What an edge of an object has to touch. */
UENUM(BlueprintType)
enum class EContactTarget : uint8
{
	/** Solid structure across the face. Only on a segment: an object's top has no structure beside it. */
	Wall UMETA(DisplayName = "Wall"),

	/** Another placed object standing on the same support. */
	Object UMETA(DisplayName = "Object"),

	/**
	 * The edge of the support itself: across the face is no part of it. On a segment that is a
	 * cell carrying no all-object surface on this plane; on an object's surface, a cell outside
	 * the layer.
	 */
	SupportBoundary UMETA(DisplayName = "Support Boundary")
};

/** Which edge of a target object a contact has to meet. */
UENUM(BlueprintType)
enum class ETargetEdge : uint8
{
	Any UMETA(DisplayName = "Any"),
	Front UMETA(DisplayName = "Front"),
	Back UMETA(DisplayName = "Back"),
	Left UMETA(DisplayName = "Left"),
	Right UMETA(DisplayName = "Right"),

	/** One of the target's special edges, by id. */
	Special UMETA(DisplayName = "Special Edge")
};

/** How much of an edge a contact needs. Always counted over the edge of the object being placed, never the target's. */
UENUM(BlueprintType)
enum class EOverlapMode : uint8
{
	/** Every face of the edge touches a valid target face. */
	Flush UMETA(DisplayName = "Flush"),

	/** At least MinFaces of them do. */
	AtLeast UMETA(DisplayName = "At Least")
};

/** How a special edge template finds its run on a mesh. */
UENUM(BlueprintType)
enum class ESpecialEdgeMode : uint8
{
	/** The side's run, less InsetStart faces at its start and InsetEnd at its end. */
	Inset UMETA(DisplayName = "Inset"),

	/** The longest run of faces on the side with at least MinOpenBelowCm of open space under their cells: a knee-hole. */
	OpenBelow UMETA(DisplayName = "Open Below")
};

UENUM(BlueprintType)
enum class EZoneMode : uint8
{
	/** Keeps out whatever is listed. */
	BlockListed UMETA(DisplayName = "Block Listed"),

	/** Keeps out everything but what is listed. With nothing listed, it keeps the zone clear. */
	AllowOnlyListed UMETA(DisplayName = "Allow Only Listed")
};

/** What an object's surfaces take on top of them. */
UENUM(BlueprintType)
enum class EAcceptMode : uint8
{
	All UMETA(DisplayName = "All"),
	OnlyListed UMETA(DisplayName = "Only Listed"),
	None UMETA(DisplayName = "None")
};

/**
 * One thing an edge of the object has to touch for a rule to hold.
 *
 * Tested one cell off the support, in the first layer of air the object stands in: each face of
 * the edge is checked against the cell it looks into there.
 */
USTRUCT(BlueprintType)
struct FContactRequirement
{
	GENERATED_BODY()

	/** The object's own edge: Front, Back, Left or Right. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact")
	EObjectSide Side = EObjectSide::Back;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact")
	EContactTarget Target = EContactTarget::Wall;

	/** Objects of these archetypes count as a target. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (EditCondition = "Target == EContactTarget::Object", EditConditionHides))
	TArray<TObjectPtr<UPlacementArchetype>> TargetArchetypes;

	/** And so do objects whose archetype's classification matches this. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (EditCondition = "Target == EContactTarget::Object", EditConditionHides))
	FGameplayTagQuery TargetClasses;

	/** Which of the target's edges has to be met. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (EditCondition = "Target == EContactTarget::Object", EditConditionHides))
	ETargetEdge TargetEdge = ETargetEdge::Any;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (ClampMin = "1", EditCondition = "Target == EContactTarget::Object && TargetEdge == ETargetEdge::Special", EditConditionHides))
	int32 TargetSpecialEdgeId = 1;

	/**
	 * A target face at either end of its edge does not count, and under Flush it fails the
	 * requirement outright. This is what keeps a chair off the corners of the desk it pulls up to.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (EditCondition = "Target == EContactTarget::Object", EditConditionHides))
	bool bExcludeTargetCorners = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact")
	EOverlapMode Overlap = EOverlapMode::Flush;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact", meta = (ClampMin = "1", EditCondition = "Overlap == EOverlapMode::AtLeast", EditConditionHides))
	int32 MinFaces = 1;
};

/** One way an archetype may be placed. */
USTRUCT(BlueprintType)
struct FPlacementRule
{
	GENERATED_BODY()

	/** Shown in the placement log. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rule")
	FName Label;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rule")
	ERuleKind Kind = ERuleKind::Surface;

	/** Only read when the archetype selects its rules by weight. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rule", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	/** Narrows the archetype's AllowedSupports for this rule alone. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rule", meta = (Bitmask, BitmaskEnum = "/Script/WEM_2026.ESupportKind"))
	int32 SupportMask = 0b11;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rule", meta = (EditCondition = "Kind == ERuleKind::Edge", EditConditionHides))
	TArray<FContactRequirement> Contacts;
};

/**
 * A special run along one side of an archetype's objects - a desk's knee-hole, a counter's
 * serving side - written once on the archetype and found on each mesh by the catalog builder.
 */
USTRUCT(BlueprintType)
struct FSpecialEdgeTemplate
{
	GENERATED_BODY()

	/** What contacts name it by. Unique per archetype. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge", meta = (ClampMin = "1"))
	int32 Id = 1;

	/** Front, Back, Left or Right. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge")
	EObjectSide Side = EObjectSide::Front;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge")
	ESpecialEdgeMode Mode = ESpecialEdgeMode::Inset;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge", meta = (ClampMin = "0", EditCondition = "Mode == ESpecialEdgeMode::Inset", EditConditionHides))
	int32 InsetStart = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge", meta = (ClampMin = "0", EditCondition = "Mode == ESpecialEdgeMode::Inset", EditConditionHides))
	int32 InsetEnd = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge", meta = (ClampMin = "0.0", EditCondition = "Mode == ESpecialEdgeMode::OpenBelow", EditConditionHides))
	float MinOpenBelowCm = 60.0f;
};

/** A special edge as found on one mesh: a run of faces along one side, counted from the edge's start. */
USTRUCT(BlueprintType)
struct FSpecialEdge
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge", meta = (ClampMin = "1"))
	int32 Id = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge")
	EObjectSide Side = EObjectSide::Front;

	/** Index of the run's first face along the edge. Front and Back count from Left to Right, Left and Right from Back to Front. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge", meta = (ClampMin = "0"))
	int32 Start = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Special Edge", meta = (ClampMin = "1"))
	int32 Count = 1;

	bool operator==(const FSpecialEdge& Other) const
	{
		return Id == Other.Id && Side == Other.Side && Start == Other.Start && Count == Other.Count;
	}
};

/**
 * Cells around an object that other objects are kept out of, or kept to, on the support the
 * object stands on. Each chosen side reaches DepthCells out from the object, as wide as that side.
 */
USTRUCT(BlueprintType)
struct FExclusionZone
{
	GENERATED_BODY()

	/** Which edges the zone reaches out from: any of Front, Back, Left and Right. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Zone", meta = (Bitmask, BitmaskEnum = "/Script/WEM_2026.EObjectSide"))
	int32 SideMask = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Zone", meta = (ClampMin = "1"))
	int32 DepthCells = 1;

	/** Fills the square between two neighbouring chosen sides, so the zone wraps round the corner. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Zone")
	bool bIncludeCorners = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Zone")
	EZoneMode Mode = EZoneMode::BlockListed;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Zone")
	TArray<TObjectPtr<UPlacementArchetype>> Archetypes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Zone")
	FGameplayTagQuery Classes;
};

/** How the catalog builder finds an archetype's surfaces on its meshes, and what those surfaces take. */
USTRUCT(BlueprintType)
struct FSurfacePolicy
{
	GENERATED_BODY()

	/** Off for things nothing should stand on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	bool bDetectSurfaces = true;

	/** Anything lower - a plinth, a foot - is not a surface. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces", meta = (ClampMin = "0.0"))
	float MinHeightCm = 10.0f;

	/** A cell with less room than this above it is left out of its layer. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces", meta = (ClampMin = "0.0"))
	float MinClearanceCm = 20.0f;

	/** The share of a cell's sample lines that have to agree on a surface for the cell to join it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CoverageFraction = 0.75f;

	/** Keeps only the highest layer found. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	bool bTopLayerOnly = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces")
	EAcceptMode Accept = EAcceptMode::All;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces", meta = (EditCondition = "Accept == EAcceptMode::OnlyListed", EditConditionHides))
	TArray<TObjectPtr<UPlacementArchetype>> AcceptedArchetypes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surfaces", meta = (EditCondition = "Accept == EAcceptMode::OnlyListed", EditConditionHides))
	FGameplayTagQuery AcceptedClasses;
};

/** Something placed alongside an object of this archetype, right after it. */
USTRUCT(BlueprintType)
struct FCompanion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Companion")
	TObjectPtr<UPlacementArchetype> Archetype;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Companion", meta = (ClampMin = "0"))
	int32 MinCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Companion", meta = (ClampMin = "0"))
	int32 MaxCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Companion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Chance = 1.0f;

	/** Looks first among meshes from the same set as the one placed, then anywhere. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Companion")
	bool bPreferSameSet = true;
};

/**
 * One level of an object that other objects can stand on: a desk's top, one shelf of a bookcase.
 * One height for the whole layer - a shelf with several heights has several layers.
 */
USTRUCT(BlueprintType)
struct FDerivedSurfaceLayer
{
	GENERATED_BODY()

	/** Height above the object's bottom. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surface", meta = (ClampMin = "0.0"))
	float HeightCm = 0.0f;

	/** Room above it before the object's own geometry closes in. Zero means unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surface", meta = (ClampMin = "0.0"))
	float ClearanceCm = 0.0f;

	/** One entry per footprint cell, 1 where the layer covers it. Indexed as WEMPlacement::GetFootprintIndex does. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surface")
	TArray<uint8> CellMask;
};

/**
 * Everything the catalog builder works out from one mesh.
 *
 * The footprint is a grid of cells centred on the mesh bounds' centre in X and Y and starting at
 * the bounds' bottom in Z. Per-cell arrays hold SizeCells.X * SizeCells.Y entries, row-major by
 * local x and then y.
 */
USTRUCT(BlueprintType)
struct FPlacementGeometry
{
	GENERATED_BODY()

	/** Depth along local X, back to front; width along Y, left to right; height along Z. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Geometry")
	FIntVector SizeCells = FIntVector(1, 1, 1);

	/** Exact, from the mesh's source vertices, so extended culling bounds and a stray pivot count for nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Geometry")
	FBox LocalBoundsCm = FBox(ForceInit);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Geometry")
	TArray<FDerivedSurfaceLayer> Surfaces;

	/** The archetype's templates, resolved against this mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Geometry")
	TArray<FSpecialEdge> SpecialEdges;

	/** Per footprint cell, the open height under the object's lowest geometry there. A leg anywhere in the cell makes it zero. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Geometry")
	TArray<float> OpenBelowCm;

	/** Per footprint cell, 1 where the mesh has any geometry above it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Geometry")
	TArray<uint8> FootprintMask;
};
