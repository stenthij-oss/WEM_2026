// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "RoomManager.generated.h"

class ULineBatchComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;

/** Defined in Components/LineBatchComponent.h; only the drawing code in the .cpp needs it. */
struct FBatchedLine;

/**
 * The two kinds of platform. Every platform is the same size and built the same way - the
 * kind only says which way it faces, which is what makes a floor, a wall and a ceiling one
 * object.
 */
UENUM(BlueprintType)
enum class EPlatformKind : uint8
{
	/** Lies in the XY plane and faces up and down: a floor from above, a ceiling from below. */
	SurfaceHorizontal UMETA(DisplayName = "surface_horizontal"),

	/** Stands in the XZ or YZ plane and faces sideways: a wall, from either side. */
	SurfaceVertical UMETA(DisplayName = "surface_vertical")
};

/** One of the grid's three axes, in the order FIntVector indexes them. */
UENUM(BlueprintType)
enum class EGridAxis : uint8
{
	X UMETA(DisplayName = "X"),
	Y UMETA(DisplayName = "Y"),
	Z UMETA(DisplayName = "Z")
};

/** One of a cell's six faces, named by the direction it looks out in. */
UENUM(BlueprintType)
enum class EGridFace : uint8
{
	PosX UMETA(DisplayName = "+X"),
	NegX UMETA(DisplayName = "-X"),
	PosY UMETA(DisplayName = "+Y"),
	NegY UMETA(DisplayName = "-Y"),
	PosZ UMETA(DisplayName = "+Z"),
	NegZ UMETA(DisplayName = "-Z")
};

/**
 * What a surface has been unlocked to carry.
 *
 * A surface is never "a floor" or "a wall" itself - it permits a class of things to stand on
 * it. Placement systems read this; nothing here spawns anything onto a surface.
 */
UENUM(BlueprintType)
enum class ESurfaceCapacity : uint8
{
	/** Not a surface: nothing built here faces this way. Carries nothing. */
	Empty UMETA(DisplayName = "Empty"),

	/** Rim surface: carries walls only. The purple in the reference drawings. */
	Wall UMETA(DisplayName = "Wall Surface"),

	/** Carries anything. The green field in the reference drawings. */
	AllObject UMETA(DisplayName = "All-Object Surface")
};

/**
 * One placed platform: a single face of the lattice, named by its min node and the axis it
 * faces along.
 *
 * Only the axis is stored. The kind follows from it - facing Z is horizontal, facing X or Y
 * is vertical - so storing the kind as well would only leave room for the two to disagree.
 */
USTRUCT(BlueprintType)
struct FGridPlatform
{
	GENERATED_BODY()

	/** The lattice node at the platform's min corner: the first cell of its rim. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	FIntVector MinNode = FIntVector::ZeroValue;

	/** The axis the platform faces along. Its two broad faces look out either way along it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	EGridAxis Normal = EGridAxis::Z;

	EPlatformKind GetKind() const
	{
		return Normal == EGridAxis::Z ? EPlatformKind::SurfaceHorizontal : EPlatformKind::SurfaceVertical;
	}
};

/** Addresses one surface: a cell, and which of its six faces. */
USTRUCT(BlueprintType)
struct FGridSurfaceRef
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface")
	FIntVector Cell = FIntVector::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface")
	EGridFace Face = EGridFace::PosZ;
};

/**
 * One kind of built piece - horizontal interior, vertical interior, edge, node - spread over a
 * run of small instanced components, "chunks", filled one at a time.
 *
 * Lumen throws away its surface cache for every instance of a component whose instance count
 * changes, then recaptures all of it over the frames that follow. With a kind of piece in one
 * component, every placement did that to the whole tower, and with the camera inside it - where
 * every surface is close and wants its cache at high resolution - that recapture was most of the
 * frame. In chunks, a placement only ever grows the chunk still being filled.
 */
USTRUCT()
struct FRoomPieceLayer
{
	GENERATED_BODY()

	/**
	 * Oldest first, and only the last is ever still being filled. The first is the actor's own
	 * component for this kind of piece; the rest are made as the structure grows and never saved.
	 * Not copied with the actor either: each layer property on ARoomManager is DuplicateTransient.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Chunks;

	/** Every piece drawn so far, keyed by what it stands for, with the sync that last found it. */
	TMap<uint64, uint32> PieceStamps;

	/** Bumped once per sync. Never zero after the first, which marks a piece as not yet drawn. */
	uint32 SyncStamp = 0;
};

/**
 * One colour of the surface overlay, spread over chunks the way FRoomPieceLayer spreads the pieces.
 *
 * Changing how many instances a component holds rebuilds its bounds and render data across every
 * instance it has. With a colour in one component, each placement did that to tens of thousands
 * of squares; in chunks it only touches the chunks a placement adds to or takes a square out of.
 */
USTRUCT()
struct FSurfaceMarkLayer
{
	GENERATED_BODY()

	/**
	 * Oldest first. New squares only ever go into the last; a square that changes colour leaves a
	 * hole in whichever chunk held it, filled from that chunk's own last square. The first is the
	 * actor's own component for this colour; the rest are made at runtime and never saved.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Chunks;

	/** Per chunk, the key of the surface each instance stands for, in instance order. */
	TArray<TArray<uint64>> ChunkKeys;
};

/**
 * Directs the simulation's grid: a cube of cells, and the structure that grows through it one
 * platform at a time.
 *
 * The cube's corner is this actor's own world-space location, at the resolved ground height,
 * and it extends into +X/+Y/+Z from there. Actor rotation and scale are ignored - the cube
 * stays axis-aligned to the world.
 *
 * The ground lattice and the cube's outline are drawn from OnConstruction, not from Tick, so
 * they are visible in the editor without entering Play.
 *
 * Cell addressing: every cell has a unique FIntVector coordinate, each component in
 * [0, GridSize). Nothing is stored per cell - a cube at the cap holds 134 million of them - so
 * the state below is kept sparse, and whatever is not held there is worked out from the
 * coordinate itself.
 *
 * The lattice
 * -----------
 * Everything placed is a platform: a PlatformSize x PlatformSize interior ringed by a one-cell
 * rim of beams, one cell thick. Neighbouring platforms share their rims rather than doubling
 * them up, so they sit at a pitch of PlatformSize + 1 and the whole structure lies on one
 * lattice. Its nodes, one every pitch along each axis, are the posts where beams meet; its
 * edges are the PlatformSize cells of beam between two neighbouring nodes; and each of its
 * faces is one slot a platform can fill - interior, four edges and four nodes. A platform is
 * named by its min node and the axis it faces along, and two platforms can share edges and
 * nodes but never interior cells.
 *
 * The lattice is pinned to the cube rather than to whatever happens to be placed first. Nodes
 * sit at every multiple of the pitch from the cube's corner, and the cube is sized in whole
 * platforms - GridPlatformsPerSide of them - so a node lands on its far face as well. A
 * structure that grows out to the edge therefore finishes flush with the cube, its outer beams
 * lying on the cube's faces on every side.
 *
 * Growth
 * ------
 * The first platform is horizontal and in the sky: on the node layer at the cube's centre, at a
 * random lattice position across it. Every one after grows off an edge of a platform already
 * placed. An edge is shared by up to four slots, two in each of the two planes that contain it,
 * so each edge offers three more: the coplanar slot that carries its platform on past it, and
 * the two that fold off it - up and down from a floor, sideways from a wall.
 *
 * A slot is free when it is empty, lies wholly inside the cube, and none of its interior has
 * been built. A fold asks more: the edge it folds off must still be offering itself on the side
 * the fold goes toward, which means every cell of the edge's run has to present a face there
 * that is wall surface and not yet built against. The posts at the run's ends are not asked,
 * since a post is shared by whatever meets at it. That is what keeps walls on the outside of the
 * floorspace: the moment two floors conjoin, the edge between them opens into all-object surface
 * and no longer offers itself.
 *
 * A slot is judged against every platform it touches, not only the one it was found from - it
 * has to fold legally off each perpendicular platform it shares an edge with. So a wall that
 * would carry on from one already standing still cannot be raised over a seam that has opened,
 * and a surface that has opened is never closed again afterwards.
 *
 * Surfaces
 * --------
 * A surface is one face of one cell. Only a platform's two broad faces count - the two faces of
 * each of its cells along its normal - and the thin outer edge of a rim does not. A cell that
 * platforms facing two ways both run through carries surfaces along both.
 *
 * What a surface carries follows from the cell: interior surfaces carry anything, rim surfaces
 * walls only. A surface is occupied once the cell it looks into has been built, and occupancy
 * outranks what it carries.
 *
 * A rim surface caught between two fields stops being rim and joins them: a wall surface
 * flanked, within its own plane and on its own side, by all-object surfaces on opposite sides is
 * promoted to all-object. Occupied surfaces never promote and never count as all-object for the
 * ones beside them. The outer rim can never satisfy the rule, since it always has emptiness on
 * one side, so a field grows while staying enclosed. Promotions feed each other - the node at
 * the centre of a 2x2 only opens once the seams around it have - so the rule is resolved to a
 * fixpoint.
 *
 * Because promotion goes side by side, a wall divides only the side it stands on. A wall raised
 * on a free edge builds against that edge's top faces, which stay closed; a floor conjoining
 * across the edge afterwards still opens the seam's underside, since the ceiling below runs on
 * unbroken, while the floor above stays divided. The posts close the corners, so two fields
 * never find each other around the end of a wall. And a wall, being a platform like any other,
 * carries all-object surface on both of its faces.
 *
 * Platforms is the whole of the saved state. Everything else is rebuilt wholesale from it, and
 * the promotion rule is order-independent, so the same set of platforms always yields the same
 * surfaces whatever order they were placed in.
 *
 * A placement does not rebuild, though - that grows with the structure and soon costs more than a
 * frame. Growth only ever adds, and a surface only ever moves from wall to all-object or to
 * occupied, never back, so a placement takes in just the platform it adds: what that platform
 * touches, and whatever promotions spread out from there. The wholesale rebuild is kept for
 * loading, editing and the wem.RoomManager.VerifyEvery check, which holds the two against each
 * other.
 */
UCLASS()
class WEM_2026_API ARoomManager : public AActor
{
	GENERATED_BODY()

public:
	ARoomManager();

	/** Hard cap on cells along each side of the cube. Re-applied in code, since the cube is sized in platforms. */
	static constexpr int32 MaxGridDimension = 512;

	/**
	 * Size of the cube, counted in platforms along each side.
	 *
	 * Counted in platforms rather than cells so the lattice always fits it exactly: a cube of
	 * whole platforms has a node on both of its faces along every axis, which is what lets a
	 * structure's outer beams lie flush with it. Clamped in code so the cube stays within
	 * MaxGridDimension cells.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1"))
	int32 GridPlatformsPerSide = 6;

	/** Cells along each side of the cube: a pitch per platform, plus the node that closes the far side. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid")
	int32 GridSize = 0;

	/** Edge length of one cell, in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1.0"))
	float CellSize = 20.0f;

	/** Manual Z nudge applied on top of the resolved base height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Placement")
	float GridHeightOffset = 0.0f;

	/** If true, a single downward trace at the cube's origin corner sets the base Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Placement")
	bool bSnapToLandscapeHeight = true;

	/** Collision channel used by that height trace. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Placement")
	TEnumAsByte<ECollisionChannel> LandscapeTraceChannel = ECC_WorldStatic;

	/** Cells per side of one platform's interior, before its beam rim. The lattice pitch is one more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms", meta = (ClampMin = "1"))
	int32 PlatformSize = 8;

	/** Seconds between automatic placements. One platform, rim and all, per beat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms", meta = (ClampMin = "0.0"))
	float PlatformPlacementInterval = 3.0f;

	/** Whether the placement beat runs on its own once play begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms")
	bool bAutoPlacePlatforms = true;

	/** Discards platforms placed in the editor at BeginPlay, so a run starts from an empty cube. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms")
	bool bClearPlatformsOnBeginPlay = true;

	/** Seed for the placement stream. Zero draws a fresh seed each run. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms")
	int32 PlatformRandomSeed = 0;

	/**
	 * Chance that a beat places a vertical platform rather than a horizontal one.
	 *
	 * The roll only decides which kind is tried first: when the chosen kind has nowhere to go
	 * the other one takes the beat, so a beat is lost only when the structure has room for
	 * neither. That also covers the opening beat, which only a horizontal can take.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VerticalPlacementChance = 0.5f;

	/**
	 * Every platform placed, in the order it was placed. This list alone is the simulation's
	 * state; everything else is derived from it.
	 *
	 * Deliberately kept out of the Details panel. Every placement changes it, and the panel
	 * answers a changed array by rebuilding its whole property tree - with this actor selected
	 * during a run that cost over a second a frame. The counts below stand in for it there.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Grid|Platforms")
	TArray<FGridPlatform> Platforms;

	/** Platforms standing in the cube. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Platforms")
	int32 PlatformCount = 0;

	/** Of those, the surface_horizontal ones. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Platforms")
	int32 HorizontalPlatformCount = 0;

	/** Of those, the surface_vertical ones. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Platforms")
	int32 VerticalPlatformCount = 0;

	/** Master toggle for the built geometry: every interior, edge and node. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	bool bShowPieces = true;

	/**
	 * Unit box every piece is built from: the engine's 100cm cube, centred on its own origin.
	 * Anything with those conventions can be swapped in.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	TObjectPtr<UStaticMesh> PieceMesh;

	/**
	 * Material the pieces are built from. Lit, unlike the surface overlay laid over them: the
	 * overlay is notation and reads best flat, while a platform is a thing standing in the world
	 * and needs light falling across it to read as one. Tinted per layer, so one material covers
	 * every colour. It is expected to expose PieceColorParameterName.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	TObjectPtr<UMaterialInterface> PieceMaterial;

	/** Vector parameter on PieceMaterial that the colours below drive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	FName PieceColorParameterName = FName(TEXT("Color"));

	/**
	 * The interiors of horizontal platforms - floor and ceiling at once.
	 *
	 * These are straight linear albedos and not pitched to suit an exposure: the pieces are
	 * lit, so what lands on screen is whatever the level's exposure makes of them. A level left
	 * on auto-exposure renormalises the whole frame and will wash these out however they are
	 * set here - that is fixed in the level, with a post-process volume on manual exposure, not
	 * by dimming the colour.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	FLinearColor HorizontalInteriorColor = FLinearColor(0.35f, 0.34f, 0.32f);

	/** The interiors of vertical platforms. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	FLinearColor VerticalInteriorColor = FLinearColor(0.25f, 0.10f, 0.03f);

	/** The beams: one run per lattice edge, however many platforms share it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	FLinearColor EdgeColor = FLinearColor(0.21f, 0.21f, 0.21f);

	/** The posts: one per lattice node, however many beams meet there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	FLinearColor NodeColor = FLinearColor(0.21f, 0.21f, 0.21f);

	/** Master toggle for the debug lattice and the cube's outline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	bool bShowDebugGrid = true;

	/**
	 * Overlays what each surface is unlocked to carry, one inset square per surface, in green,
	 * purple and white. This is the only view that shows which rim has been promoted - the
	 * pieces cannot, since a promoted beam is still a beam - so it reads on top of them rather
	 * than instead of them, and both are on by default.
	 *
	 * The squares are instanced in chunks, and a placement only touches the surfaces it changed
	 * and the chunks holding them, so the overlay costs little however far the structure has grown.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	bool bShowSurfaces = true;

	/**
	 * Unit plane every overlay square is built from: the engine's 100x100cm plane, centred on
	 * its own origin and facing +Z. Anything with those conventions can be swapped in.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	TObjectPtr<UStaticMesh> PlaneMesh;

	/**
	 * Base material for the overlay. It is not used directly - a dynamic instance per colour is
	 * made from it and tinted, so one material covers all three.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	TObjectPtr<UMaterialInterface> PlaneMaterial;

	/** Vector parameter on PlaneMaterial that the overlay colours drive. M_WEMDebug calls it "Color". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FName PlaneColorParameterName = FName(TEXT("Color"));

	/** Colour of the ground lattice's interior lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor GridLineColor = FColor(200, 200, 200);

	/** Colour of the cube's twelve edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor BoundaryLineColor = FColor::Blue;

	/** Surfaces that carry anything. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor AllObjectSurfaceColor = FColor(0, 230, 0);

	/** Surfaces that carry walls only, and are still free to. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor WallSurfaceColor = FColor(170, 0, 230);

	/** Surfaces already built against. Drawn over whatever the surface carries, since occupancy outranks it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor OccupiedSurfaceColor = FColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug", meta = (ClampMin = "0.1"))
	float GridLineThickness = 1.5f;

	/** Drawn thicker than the ground lattice so the cube reads as surrounded by a boundary line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug", meta = (ClampMin = "0.1"))
	float BoundaryLineThickness = 5.0f;

	/** Marks DebugCell in the viewport so a single cell can be located by coordinate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	bool bHighlightDebugCell = false;

	/** Cell to mark. Nothing is drawn when it falls outside the cube. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	FIntVector DebugCell = FIntVector::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	FColor DebugCellColor = FColor::Green;

	/** Base Z resolved at the last rebuild: where cell layer zero starts. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	double GridBaseZ = 0.0;

	/** World-space centre of DebugCell, or zero when it is outside the cube. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	FVector DebugCellCenter = FVector::ZeroVector;

	/** World location fed through WorldToCell on each rebuild, for the inverse lookup. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	FVector DebugProbeLocation = FVector::ZeroVector;

	/** Cell containing DebugProbeLocation, or (INDEX_NONE, INDEX_NONE, INDEX_NONE) when outside the cube. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	FIntVector DebugProbeCell = FIntVector(INDEX_NONE, INDEX_NONE, INDEX_NONE);

	/**
	 * The surface nearest DebugProbeLocation, among the probe's own cell and the 26 around it.
	 * Its cell is (INDEX_NONE, INDEX_NONE, INDEX_NONE) when there is none that close. Readout only.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	FGridSurfaceRef DebugProbeSurface;

	/** What that surface carries. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	ESurfaceCapacity DebugProbeSurfaceCapacity = ESurfaceCapacity::Empty;

	/** Whether that surface has been built against. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	bool bDebugProbeSurfaceOccupied = false;

	/** True when the coordinate addresses a cell inside the cube. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	bool IsValidCell(const FIntVector& Cell) const;

	/** World-space corner of a cell nearest the cube's origin corner. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	FVector GetCellMinCorner(const FIntVector& Cell) const;

	/** World-space centre of a cell. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	FVector GetCellCenter(const FIntVector& Cell) const;

	/** World-space box a cell fills. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	FBox GetCellBounds(const FIntVector& Cell) const;

	/**
	 * Cell containing a world location.
	 * Returns false and sets OutCell to (INDEX_NONE, INDEX_NONE, INDEX_NONE) when the location
	 * is outside the cube.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	bool WorldToCell(const FVector& WorldLocation, FIntVector& OutCell) const;

	/** What one face of a cell has been unlocked to carry. Anything that is not a surface carries nothing. */
	UFUNCTION(BlueprintPure, Category = "Grid|Surfaces")
	ESurfaceCapacity GetSurfaceCapacity(const FIntVector& Cell, EGridFace Face) const;

	/**
	 * Whether a surface has been built against: the cell it looks into is part of a platform.
	 *
	 * Occupancy is not the same as what a surface carries. An occupied rim surface is still a
	 * wall surface, it has only stopped being a free one - nothing more may fold off there, and
	 * the promotion that opens seams into floorspace steps over it. Always false for a face that
	 * is not a surface.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Surfaces")
	bool IsSurfaceOccupied(const FIntVector& Cell, EGridFace Face) const;

	/**
	 * World transform of one face of a cell: at the face's centre, turned so its up vector is
	 * the face's outward normal. Pure geometry, so it answers for any face, surface or not.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Surfaces")
	FTransform GetSurfaceWorldTransform(const FIntVector& Cell, EGridFace Face) const;

	/**
	 * Every surface that carries Filter, occupied ones included only when asked for.
	 * Occupied surfaces always carry walls only, and asking for Empty finds nothing.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Surfaces")
	void GatherSurfaces(ESurfaceCapacity Filter, bool bIncludeOccupied, TArray<FGridSurfaceRef>& OutSurfaces) const;

	/** Number of platforms standing in the cube. */
	UFUNCTION(BlueprintPure, Category = "Grid|Platforms")
	int32 GetPlatformCount() const;

	/**
	 * Whether a platform could be placed at this min node, facing along this axis.
	 *
	 * It has to sit on the lattice with its whole footprint inside the cube, in a slot that is
	 * empty and whose interior is unbuilt, sharing at least one edge with a platform already
	 * placed. For every platform it would fold off - one it shares an edge with but faces
	 * across - every cell of that edge's run must still offer a free wall surface on the side it
	 * would fold toward. Before anything is placed, only a horizontal platform on the cube's
	 * centre layer passes.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Platforms")
	bool CanPlacePlatform(const FIntVector& MinNode, EGridAxis Normal) const;

	/**
	 * One beat of growth: rolls which kind to try first, places one platform - the other kind
	 * if the first has nowhere to go - and brings everything derived up to date. The platform is
	 * drawn from the placement stream out of every free slot across the whole structure at once.
	 * Returns false when neither kind fits anywhere.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Platforms")
	bool PlaceNextPlatform();

	/** Drops every platform and re-seeds the placement stream. Back to an empty cube. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Platforms")
	void ClearPlatforms();

	//~ Begin AActor Interface
	virtual void OnConstruction(const FTransform& Transform) override;
	//~ End AActor Interface

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

private:
	/** How many colours the surface overlay has: all-object, wall, and occupied. A chunked layer each. */
	static constexpr int32 SurfaceMarkLayerCount = 3;

	/** How many kinds of platform there are, which is how many candidate lists are kept. */
	static constexpr int32 PlatformKindCount = 2;

	/** Where one surface's overlay square stands: which colour, which of its chunks, and which instance there. */
	struct FSurfaceMarkSlot
	{
		int32 Layer = INDEX_NONE;

		/** INDEX_NONE, like Instance, while the square is still waiting to be added. */
		int32 Chunk = INDEX_NONE;

		/** INDEX_NONE while the square is still waiting to be added. */
		int32 Instance = INDEX_NONE;

		/** The sync that last found this surface. Anything older is no longer a surface at all. */
		uint32 SeenStamp = 0;
	};

	/** Squares waiting to be added, per colour, gathered over one sync and added a batch per chunk. */
	struct FPendingSurfaceMarks
	{
		/** Every square is the same size: one cell, less the inset. */
		FVector Scale = FVector::OneVector;

		TArray<FTransform> Transforms[SurfaceMarkLayerCount];
		TArray<uint64> Keys[SurfaceMarkLayerCount];
	};

	/** Every visual from scratch: the pieces, the overlay and the debug lattice. */
	void RebuildVisuals();

	/**
	 * What has to follow one placement. A placement only ever adds to the structure, so the
	 * lattice stays as it is, the pieces take just the new platform's, and the overlay just the
	 * surfaces the placement changed.
	 */
	void UpdateVisualsAfterPlacement(const FGridPlatform& Platform, TConstArrayView<FGridSurfaceRef> ChangedSurfaces);

	/** Flushes and redraws the debug lattice. Owned batcher, so no other system's lines are touched. */
	void RebuildDebugGrid();

	/** Drops every overlay square and the record of them, so the next sync draws the lot. */
	void ResetSurfaceMarks();

	/**
	 * Brings the overlay in line with every surface, adding, recolouring or dropping only what
	 * differs. Walks the whole structure. Returns how many squares it had to add or recolour.
	 */
	int32 SyncSurfaceMarks();

	/**
	 * Brings just these surfaces' squares in line - the ones a placement changed. Falls back on
	 * the full sync whenever the overlay is not already complete and in step with its record.
	 */
	void UpdateSurfaceMarks(TConstArrayView<FGridSurfaceRef> Surfaces);

	/** Whether there is anything for the overlay to draw, and anything to draw it with. */
	bool CanDrawSurfaceMarks() const;

	/** Whether every colour's chunks still match the record of them. A loaded level or a PIE copy does not. */
	bool AreSurfaceMarksIntact() const;

	/**
	 * Resolves one surface and moves its square to the colour it should be, queueing a new square
	 * in Pending when it needs one. Returns true when the square had to be added or recoloured.
	 */
	bool SyncSurfaceMark(const FIntVector& Cell, EGridFace Face, uint64 Key, FSurfaceMarkSlot& Slot, FPendingSurfaceMarks& Pending);

	/** Adds the queued squares, filling each colour's last chunk and starting a new one whenever that is full. */
	void AddPendingSurfaceMarks(const FPendingSurfaceMarks& Pending);

	/** Re-tints the overlay's materials and puts the plane and its colour on every chunk. */
	void RefreshSurfaceMarkMaterials();

	/** Takes one square out of its chunk without moving any square but that chunk's last. */
	void RemoveSurfaceMark(int32 Layer, int32 Chunk, int32 Instance);

	/** A colour's first chunk: the actor's own component for it. */
	UInstancedStaticMeshComponent* GetSurfaceMarkComponent(int32 Layer) const;

	/** A colour's chunks and the record of what each holds. */
	FSurfaceMarkLayer& GetSurfaceMarkChunks(int32 Layer);
	const FSurfaceMarkLayer& GetSurfaceMarkChunks(int32 Layer) const;

	/** One piece as its layer sees it: what it stands for, and where it goes. */
	struct FPlacedPiece
	{
		uint64 Key = 0;
		FTransform Transform;
	};

	/**
	 * Brings the built geometry in line with the platforms - one box per interior, per edge and
	 * per node - adding only what is new. Walks the whole structure. Returns how many pieces it
	 * had to add.
	 */
	int32 SyncPieces();

	/**
	 * Adds one new platform's pieces, skipping any edge or node a neighbour already built. Falls
	 * back on the full sync whenever the pieces are not already complete and in step.
	 */
	void AppendPlatformPieces(const FGridPlatform& Platform);

	/** One platform's pieces, appended to the list for each kind. */
	void GatherPlatformPieces(
		const FGridPlatform& Platform,
		TArray<FPlacedPiece>& OutHorizontalInteriors,
		TArray<FPlacedPiece>& OutVerticalInteriors,
		TArray<FPlacedPiece>& OutEdges,
		TArray<FPlacedPiece>& OutNodes) const;

	/** Drops every piece of every kind, so the next sync builds the lot. */
	void ResetPieces();

	/**
	 * Adds the pieces a layer has not drawn yet. If a piece it did draw is missing from Pieces,
	 * the layer is rebuilt from scratch instead - growth never takes a piece away, so that only
	 * follows a reset. Returns how many pieces it added.
	 */
	int32 SyncPieceLayer(
		FRoomPieceLayer& Layer,
		UInstancedStaticMeshComponent* FirstChunk,
		TConstArrayView<FPlacedPiece> Pieces,
		UStaticMesh* Mesh,
		UMaterialInterface* Material);

	/** Whether a layer's chunks still hold exactly the pieces its record says they do. */
	bool IsPieceLayerIntact(const FRoomPieceLayer& Layer, const UInstancedStaticMeshComponent* FirstChunk) const;

	/** Adds pieces to a layer, filling its open chunk and starting a new one whenever that is full. */
	void AddPiecesToLayer(FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk, TConstArrayView<FTransform> NewPieces);

	/** Empties a layer back to its first chunk, cleared. */
	void ResetPieceLayer(FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk);

	/** Destroys every chunk but the first, clears that, and leaves it as the only one. */
	void ResetChunks(TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Chunks, UInstancedStaticMeshComponent* FirstChunk);

	/**
	 * Starts a new chunk and appends it to Chunks. Set up like the first - mesh and material
	 * included - so the first has to be brought up to date before this is called.
	 */
	UInstancedStaticMeshComponent* AddInstancedChunk(
		TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Chunks,
		UInstancedStaticMeshComponent* FirstChunk);

	/**
	 * Dynamic instance of a base material tinted to Color, made once and re-tinted after that.
	 * Returns null when no base material is set, which leaves the mesh's own material in place.
	 */
	UMaterialInstanceDynamic* ResolveTintedMaterial(
		TObjectPtr<UMaterialInstanceDynamic>& CachedMaterial,
		UMaterialInterface* BaseMaterial,
		FName ParameterName,
		const FLinearColor& Color);

	/** Resolves the world-space Z the whole cube stands at. Cached into GridBaseZ each rebuild. */
	double ResolveGridBaseZ() const;

	/** Lattice pitch in cells: a platform's interior plus the one rim line neighbours share. */
	int32 GetPitch() const;

	/** GridPlatformsPerSide, clamped so the cube stays within MaxGridDimension cells. */
	int32 GetPlatformsPerSide() const;

	/** Cells along each side of the cube, worked out afresh rather than read from the readout. */
	int32 ResolveGridSize() const;

	/** Z of the node layer the first platform is laid on: the one at the cube's centre, or just above it. */
	int32 GetFirstPlatformLayer() const;

	/** Whether a platform sits on the lattice with its whole footprint inside the cube. */
	bool IsPlatformOnGrid(const FGridPlatform& Platform) const;

	/**
	 * Rebuilds everything derived from Platforms. Wholesale, so it cannot drift out of step -
	 * which is also what makes it the reference AddPlacedPlatform is checked against.
	 */
	void RebuildPlatformState();

	/**
	 * Takes one newly placed platform into the derived state without rebuilding it: its slot and
	 * rim, the surfaces it builds against, and the promotions that spread out from it. Collects
	 * every surface whose capacity or occupancy may have changed, for the overlay.
	 */
	void AddPlacedPlatform(const FGridPlatform& Platform, TArray<FGridSurfaceRef>& OutChangedSurfaces);

	/** Records a platform's rim cells, each with the axis the platform faces along. */
	void RecordRimCells(const FGridPlatform& Platform);

	/**
	 * The promotion rule: flanked, within its own plane and on its own side, by open all-object
	 * surfaces on opposite sides along either axis of that plane.
	 */
	bool IsFlankedByOpenFields(const FIntVector& Cell, EGridFace Face) const;

	/**
	 * The axes along which a cell carries broad faces, one bit each, or zero when nothing is
	 * built there. An interior cell carries one; a cell on an edge or a node can carry up to three.
	 */
	uint8 GetCellSurfaceAxes(const FIntVector& Cell, bool& bOutInterior) const;

	/** Whether any platform covers this cell. */
	bool IsSolidCell(const FIntVector& Cell) const;

	/** What one face carries and whether it is built against, resolved together. */
	void ResolveSurface(const FIntVector& Cell, EGridFace Face, ESurfaceCapacity& OutCapacity, bool& bOutOccupied) const;

	/** Every lattice position the first platform could take. */
	void GatherFirstPlatformCandidates(TArray<FGridPlatform>& OutCandidates) const;

	/** Every free slot of one kind around the structure, in key order. Walks the whole structure. */
	void GatherPlatformCandidates(EPlatformKind Kind, TArray<FGridPlatform>& OutCandidates) const;

	/** Gathers both candidate lists afresh if a rebuild has left them stale. */
	void EnsurePlatformCandidates();

	/** Re-judges one slot, adding it to or dropping it from its kind's candidates to match. */
	void RefreshPlatformCandidate(const FGridPlatform& Slot);

	/** Draws one platform of a kind from the placement stream and places it. False when that kind has nowhere to go. */
	bool PlacePlatformOfKind(EPlatformKind Kind);

	/**
	 * Rebuilds the derived state and the visuals from scratch and reports anything the placements
	 * had built up differently. Driven by wem.RoomManager.VerifyEvery; the rebuild is kept either way.
	 */
	void VerifyIncrementalState();

	/** The placement beat. Stops its own timer once the structure has no room left. */
	void AdvancePlacement();

	void ResetPlacementStream();

	/** Refreshes the readouts under Grid|Coordinates, which follow both the probe and the structure. */
	void UpdateDebugReadouts();

	void AppendLatticeLines(TArray<FBatchedLine>& Lines) const;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> HorizontalInteriorPieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> VerticalInteriorPieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> EdgePieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> NodePieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> AllObjectMarkMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> WallMarkMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> OccupiedMarkMaterial;

	/*
	 * Everything derived from Platforms. None of it is reflected, so none of it is saved or
	 * copied with the actor - a loaded level or a PIE copy arrives with the list alone and
	 * rebuilds the rest. All of it is sparse: keyed by packed cell coordinates, and holding only
	 * what cannot be worked out from a coordinate on the spot. RebuildPlatformState makes it from
	 * scratch; AddPlacedPlatform keeps it up to date one placement at a time.
	 */

	/** The platforms that fit the lattice as it stands, each once, in list order. */
	TArray<FGridPlatform> BuiltPlatforms;

	/** Every slot a platform fills, keyed by min node and normal axis. */
	TSet<uint64> PlatformKeys;

	/**
	 * Every cell on a built edge or node, with the axes it carries broad faces along. Interior
	 * cells are not listed: whether one is built follows from the slot it lies in.
	 */
	TMap<uint64, uint8> RimCellNormals;

	/** Rim surfaces promoted to all-object, keyed by cell and face. */
	TSet<uint64> PromotedFaces;

	/**
	 * Every free slot, per kind (indexed by EPlatformKind), in ascending key order - the order
	 * the placement stream draws from. Kept up to date placement by placement: only slots around an
	 * edge that a placement touched or opened can have changed, so only those are judged again.
	 */
	TArray<FGridPlatform> PlatformCandidates[PlatformKindCount];

	/** Set by every rebuild, and by the first placement; the next draw then gathers the lists afresh. */
	bool bPlatformCandidatesStale = true;

	/** Platforms in the list that the last rebuild left out, so the warning is given once rather than every beat. */
	int32 ReportedSkippedPlatforms = 0;

	/** Placements since wem.RoomManager.VerifyEvery last checked one, and the tally of those checks. */
	int32 PlacementsSinceVerify = 0;
	int32 VerifiedPlacements = 0;
	int32 FailedVerifications = 0;

	/**
	 * Every surface the overlay has drawn, keyed by cell and face. Mirrors the mark chunks and is
	 * neither saved nor duplicated with them, so a sync that finds the two out of step starts
	 * again from scratch rather than trusting it.
	 */
	TMap<uint64, FSurfaceMarkSlot> SurfaceMarkSlots;

	/** Bumped once per sync, to tell the surfaces it found from ones it did not. */
	uint32 SurfaceMarkSyncStamp = 0;

	/**
	 * Whether the last full sync drew every surface, so a placement may add to it. False after
	 * a reset or while the overlay is off; the next update then takes the full sync instead.
	 */
	bool bSurfaceMarksComplete = false;

	/** Whether the last full sync built every platform's pieces, so a placement may add to them. */
	bool bPiecesComplete = false;

	/** The overlay's green squares. Starts from AllObjectSurfaceMarks. */
	UPROPERTY(Transient, DuplicateTransient)
	FSurfaceMarkLayer AllObjectMarkChunks;

	/** The overlay's purple squares. Starts from WallSurfaceMarks. */
	UPROPERTY(Transient, DuplicateTransient)
	FSurfaceMarkLayer WallMarkChunks;

	/** The overlay's white squares. Starts from OccupiedSurfaceMarks. */
	UPROPERTY(Transient, DuplicateTransient)
	FSurfaceMarkLayer OccupiedMarkChunks;

	/** Interiors of horizontal platforms. Starts from HorizontalInteriorPieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer HorizontalInteriorLayer;

	/** Interiors of vertical platforms. Starts from VerticalInteriorPieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer VerticalInteriorLayer;

	/** Lattice edges, the beams. Starts from EdgePieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer EdgeLayer;

	/** Lattice nodes, the posts. Starts from NodePieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer NodeLayer;

	FRandomStream PlacementStream;

	FTimerHandle PlacementTimerHandle;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;

	/*
	 * The instanced components below are not VisibleAnywhere. The Details panel shows a visible
	 * component's properties inline, its per-instance array included, and every placement changes
	 * those arrays - so with this actor selected during a run the panel rebuilt thousands of rows
	 * a beat. BlueprintReadOnly keeps them reachable from Blueprint and MCP.
	 *
	 * Each of these is only the first chunk of its kind (see FRoomPieceLayer and FSurfaceMarkLayer).
	 * Their settings - shadows, collision, mobility, lighting - are copied onto every chunk made
	 * after them.
	 */

	/**
	 * One instance per horizontal platform, spanning its whole interior.
	 *
	 * A platform's interior is drawn as one box because it was placed as one thing; its rim is
	 * drawn apart, as edges and nodes, because neighbours share it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> HorizontalInteriorPieces;

	/** One instance per vertical platform, spanning its whole interior. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> VerticalInteriorPieces;

	/** One instance per lattice edge in use, spanning its run. Shared edges are built once. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> EdgePieces;

	/** One instance per lattice node in use. Shared nodes are built once. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> NodePieces;

	/** The overlay's green squares: one per all-object surface. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> AllObjectSurfaceMarks;

	/** The overlay's purple squares: one per wall surface still free to take a wall. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> WallSurfaceMarks;

	/** The overlay's white squares: one per surface already built against. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> OccupiedSurfaceMarks;

	/**
	 * Draws the ground lattice and the cube. An actor-owned batcher rather than
	 * DrawDebugLine/FlushPersistentDebugLines: the global persistent-line buffer is world-wide, so
	 * flushing it to redraw this grid would also wipe any other system's persistent debug lines,
	 * and vice versa.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<ULineBatchComponent> GridLineBatcher;
};
