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
class UPlatformTileData;
class UStaticMesh;

/** Defined in Components/LineBatchComponent.h; only the drawing code in the .cpp needs it. */
struct FBatchedLine;

/**
 * The two kinds of platform. Every platform is built the same way, whatever tile it is laid
 * from - the kind only says which way it faces, which is what makes a floor, a wall and a
 * ceiling one object.
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
 * One placed platform: a rectangle of the lattice's module faces, named by its min node, the axis
 * it faces along, and the tile it was laid from.
 *
 * Only the axis is stored. The kind follows from it - facing Z is horizontal, facing X or Y
 * is vertical - so storing the kind as well would only leave room for the two to disagree. Its
 * size follows from the tile the same way, so changing a tile reshapes every platform laid from it.
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

	/** The tile it was laid from, which gives it its size and what it is built with. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	TObjectPtr<UPlatformTileData> Tile;

	/** Laid the other way round: the tile's length runs where its width would, and its width where its length would. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	bool bTurned = false;

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
 * One kind of built piece - horizontal interior, vertical interior, a tile's own mesh, edge,
 * node - spread over a run of small instanced components, "chunks", filled one at a time.
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
	 * component for this kind of piece, or for a tile's mesh one made alongside the layer; the
	 * rest are made as the structure grows. None but the actor's own are saved, and none are
	 * copied with the actor: each layer property on ARoomManager is DuplicateTransient.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Chunks;

	/** Every piece drawn so far, keyed by what it stands for, with the sync that last found it. */
	TMap<uint64, uint32> PieceStamps;

	/** Bumped once per sync. Never zero after the first, which marks a piece as not yet drawn. */
	uint32 SyncStamp = 0;
};

/** The interiors of platforms whose tile brings a mesh of its own: one piece layer per tile. */
USTRUCT()
struct FTileMeshLayer
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UPlatformTileData> Tile;

	/** Its first chunk is made with the layer, set up like HorizontalInteriorPieces, rather than being one of the actor's own. */
	UPROPERTY(Transient)
	FRoomPieceLayer Pieces;
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
 * Everything placed is a platform, laid from one of the Tiles: an interior ringed by a one-cell
 * rim of beams, one cell thick. Neighbouring platforms share their rims rather than doubling
 * them up, so a tile is measured beam to beam, and every size is a whole number of lattice
 * modules - LatticeModule cells, the step from one lattice node to the next along each axis. The
 * nodes are where beams can meet; a module edge is the LatticeModule - 1 cells of beam between
 * two neighbouring nodes; a module face is the square four module edges ring. A platform covers a
 * rectangle of module faces in one plane. Every edge and node around it is its rim, and every one
 * inside it is interior, bridged without a beam. It is named by its min node and the axis it
 * faces along, and two platforms can share rim but never interior.
 *
 * The lattice is pinned to the cube rather than to whatever happens to be placed first. Nodes
 * sit at every multiple of the module from the cube's corner, and the cube is rounded down to
 * whole modules, so a node lands on its far face as well. A structure that grows out to the edge
 * therefore finishes flush with the cube, its outer beams lying on the cube's faces on every side.
 *
 * Growth
 * ------
 * The first platform is always laid from FirstTile, horizontal and in the sky, at the heart of
 * the cube: on the node layer at its centre, and centred on that layer as nearly as the lattice
 * allows - where the centre falls between two positions along an axis, the placement stream
 * picks one. From there the structure has as far to grow on every side before it reaches the
 * cube's faces. Every one after grows off the structure, sharing at least one module edge of rim
 * with a platform already placed: either carrying it on in its own plane, or folding off it - up
 * and down from a floor, sideways from a wall.
 *
 * It lies flush with every platform it shares rim with. Along the line the two share, their sides
 * start or end together - one corner at least lines up, so tiles never stagger - and wherever the
 * other end does not, the step between them is at least the shortest side of any tile. A step any
 * shorter would leave a notch no tile could ever be laid into flush.
 *
 * A placement is free when it lies wholly inside the cube and on nothing already built but rim
 * it can share: none of its module faces is taken, nothing runs across its interior, and its own
 * rim runs across no other platform's interior. A fold asks more: every module edge it shares
 * with a platform it folds off must still be offering itself on the side the fold goes toward,
 * which means every cell of that edge's run has to present a face there that is wall surface and
 * not yet built against. The nodes are not asked, since a node is shared by whatever meets at it.
 * That is what keeps walls on the outside of the floorspace: the moment two floors conjoin, the
 * seam between them opens into all-object surface and no longer offers itself.
 *
 * A placement is judged against every platform it touches, not only the one it was found from -
 * it has to fold legally off each perpendicular platform it shares an edge with. So a wall that
 * would carry on from one already standing still cannot be raised over a seam that has opened,
 * and a surface that has opened is never closed again afterwards.
 *
 * Each beat rolls which kind to try first, then which tile, by the tiles' weights among those
 * with somewhere to go, then draws one of that tile's free placements.
 *
 * Surfaces
 * --------
 * A surface is one face of one cell. Only a platform's two broad faces count - the two faces of
 * each of its cells along its normal - and the thin outer edge of a rim does not. A cell that
 * platforms facing two ways both run through carries surfaces along both.
 *
 * What a surface carries follows from the cell: interior surfaces carry anything, rim surfaces
 * walls only. A tile can close off one side of its interior (UPlatformTileData::BlockedFace), and
 * there it carries nothing and is not a surface at all. A surface is occupied once the cell it
 * looks into has been built, and occupancy outranks what it carries.
 *
 * A rim surface caught between two fields stops being rim and joins them: a wall surface
 * flanked, within its own plane and on its own side, by all-object surfaces on opposite sides is
 * promoted to all-object. Occupied surfaces never promote and never count as all-object for the
 * ones beside them, and neither does a blocked side. The outer rim can never satisfy the rule,
 * since it always has emptiness on one side, so a field grows while staying enclosed. Promotions
 * feed each other - the node at the centre of a 2x2 only opens once the seams around it have -
 * so the rule is resolved to a fixpoint.
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

	/** Hard cap on cells along each side of the cube. Re-applied in code. */
	static constexpr int32 MaxGridDimension = 512;

	/**
	 * Cells along each side of the cube.
	 *
	 * Rounded down in code to a whole number of lattice modules and the node that closes the far
	 * side, so the lattice fits the cube exactly: a node lands on both of its faces along every
	 * axis, which is what lets a structure's outer beams lie flush with it. Counted in cells rather
	 * than in platforms, since platforms now come in more than one size.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "3", ClampMax = "512"))
	int32 GridSize = 65;

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

	/**
	 * Cells from one lattice node to the next along each axis: the step every tile's size, and
	 * every platform's position, is a whole number of. A tile whose sides are not is left out of
	 * growth, with a warning.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms", meta = (ClampMin = "2"))
	int32 LatticeModule = 4;

	/** The tiles growth lays platforms from, each chosen by its weight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms")
	TArray<TObjectPtr<UPlatformTileData>> Tiles;

	/**
	 * The tile the first platform is always laid from, whatever its weight. It need not be among
	 * Tiles as well; if it is not, it is only ever laid first. Nothing grows without it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms")
	TObjectPtr<UPlatformTileData> FirstTile;

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
	 * neither. That also covers the opening beat, which only a horizontal can take. The tile is
	 * rolled after the kind.
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
	 * Anything with those conventions can be swapped in. Across the interior of a tile with a
	 * mesh of its own, that mesh is built instead.
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
	 * The interiors of horizontal platforms - floor and ceiling at once. A tile's own mesh keeps
	 * its own materials rather than taking this or the colour below.
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

	/** The beams: one run per module edge of rim, however many platforms share it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	FLinearColor EdgeColor = FLinearColor(0.21f, 0.21f, 0.21f);

	/** The posts: one per lattice node of rim, however many beams meet there. */
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
	 * Whether a platform could be placed: at its min node, facing along its axis, laid from its
	 * tile the way round it says.
	 *
	 * It has to sit on the lattice with its whole footprint inside the cube, taking no module
	 * face already taken, with nothing built across its interior and no other interior under its
	 * rim, and share at least one module edge with a platform already placed. It has to lie flush
	 * with every platform it shares an edge with, in its plane or across it. For every platform
	 * it would fold off - one it shares an edge with but faces across - every cell of each shared
	 * edge's run must still offer a free wall surface on the side it would fold toward. Before
	 * anything is placed, only FirstTile, horizontal and at the middle of the cube's centre
	 * layer, passes.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Platforms")
	bool CanPlacePlatform(const FGridPlatform& Platform) const;

	/**
	 * One beat of growth: rolls which kind to try first, then which tile, places one platform -
	 * the other kind if the first has nowhere to go - and brings everything derived up to date.
	 * The platform is drawn from the placement stream out of every free placement of that tile
	 * across the whole structure at once. Returns false when neither kind fits anywhere.
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

	/** How many kinds of platform there are, which is how many candidate lists each tile keeps. */
	static constexpr int32 PlatformKindCount = 2;

	/** How a module edge is taken in: around a platform as rim, or across one as interior. */
	static constexpr uint8 RimEdgeUse = 1 << 0;
	static constexpr uint8 InteriorEdgeUse = 1 << 1;

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
	 * The colour one surface's square should be, or INDEX_NONE when the face is not a surface at
	 * all - nothing built faces that way, or a tile has blocked that side off.
	 */
	int32 ResolveSurfaceMarkLayer(const FIntVector& Cell, EGridFace Face) const;

	/**
	 * Moves one surface's square to the colour Layer, queueing a new square in Pending when it
	 * needs one. Returns true when the square had to be added or recoloured.
	 */
	bool SyncSurfaceMark(const FIntVector& Cell, EGridFace Face, uint64 Key, int32 Layer, FSurfaceMarkSlot& Slot, FPendingSurfaceMarks& Pending);

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

	/** Pieces sorted by the layer each goes to: one platform's worth, or the whole structure's. */
	struct FGatheredPieces
	{
		TArray<FPlacedPiece> HorizontalInteriors;
		TArray<FPlacedPiece> VerticalInteriors;
		TArray<FPlacedPiece> Edges;
		TArray<FPlacedPiece> Nodes;

		/** Interiors of tiles that bring a mesh of their own, per tile. */
		TMap<UPlatformTileData*, TArray<FPlacedPiece>> MeshInteriors;
	};

	/**
	 * Brings the built geometry in line with the platforms - one piece per interior, per module
	 * edge and per node of rim - adding only what is new. Walks the whole structure. Returns how
	 * many pieces it had to add.
	 */
	int32 SyncPieces();

	/**
	 * Adds one new platform's pieces, skipping any edge or node a neighbour already built. Falls
	 * back on the full sync whenever the pieces are not already complete and in step.
	 */
	void AppendPlatformPieces(const FGridPlatform& Platform);

	/** One platform's pieces, appended to the list for each kind. */
	void GatherPlatformPieces(const FGridPlatform& Platform, FGatheredPieces& OutPieces) const;

	/** Drops every piece of every kind, so the next sync builds the lot. */
	void ResetPieces();

	/** The layer a tile's mesh is built in, made the first time the tile needs one. */
	FTileMeshLayer& FindOrAddTileMeshLayer(UPlatformTileData* Tile);

	/** Takes down every tile's mesh layer, first chunk and all. */
	void ResetTileMeshLayers();

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

	/** Destroys every chunk but the first, clears that, and leaves it as the only one. With no first, destroys the lot. */
	void ResetChunks(TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Chunks, UInstancedStaticMeshComponent* FirstChunk);

	/**
	 * Starts a new chunk and appends it to Chunks. Set up like FirstChunk - mesh and material
	 * included - so that has to be brought up to date before this is called. Named after it
	 * unless given a name of its own.
	 */
	UInstancedStaticMeshComponent* AddInstancedChunk(
		TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Chunks,
		UInstancedStaticMeshComponent* FirstChunk,
		FName BaseName = NAME_None);

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

	/** LatticeModule, clamped so that at least one module, and the node closing it, fit the cube. */
	int32 GetModule() const;

	/** Cells along each side of the cube: GridSize, clamped and rounded down to whole modules and the closing node. */
	int32 ResolveGridSize() const;

	/** Z of the node layer the first platform is laid on: the one at the cube's centre, or just above it. */
	int32 GetFirstPlatformLayer() const;

	/** Whether a tile can be laid on the lattice as it stands: both of its sides whole multiples of the module. */
	bool IsTileUsable(const UPlatformTileData* Tile) const;

	/**
	 * A platform's size in cells, beam to beam, along its first and second in-plane axes - its
	 * tile's width and length, swapped when it is turned. False when its tile is not usable.
	 */
	bool GetPlatformSpans(const FGridPlatform& Platform, int32& OutSpanFirst, int32& OutSpanSecond) const;

	/** Whether a platform sits on the lattice, laid from a usable tile, with its whole footprint inside the cube. */
	bool IsPlatformOnGrid(const FGridPlatform& Platform) const;

	/**
	 * Whether a platform on the grid would stand on nothing already built but rim it can share:
	 * none of its module faces taken, nothing built along any module edge across its interior,
	 * and no other platform's interior along the module edges around it. The half of
	 * CanPlacePlatform that asks only what is where, which the rebuild holds the list to as well.
	 */
	bool IsSpaceFree(const FGridPlatform& Platform) const;

	/**
	 * Whether a platform lies flush with a neighbour it shares rim with, along the axis of the
	 * line they share: their sides there start or end together, and wherever they do not, the
	 * step between them is at least SmallestTileSide.
	 */
	bool IsFlushWith(const FGridPlatform& Platform, const FGridPlatform& Neighbour, int32 SharedAxis) const;

	/** Brings the usable tiles and the warnings about the rest up to date with Tiles, FirstTile and the module. */
	void ResolveUsableTiles();

	/**
	 * Rebuilds everything derived from Platforms. Wholesale, so it cannot drift out of step -
	 * which is also what makes it the reference AddPlacedPlatform is checked against.
	 */
	void RebuildPlatformState();

	/**
	 * Takes one newly placed platform into the derived state without rebuilding it: its module
	 * faces, edges and rim, the surfaces it builds against, and the promotions that spread out
	 * from it. Collects every surface whose capacity or occupancy may have changed, for the overlay.
	 */
	void AddPlacedPlatform(const FGridPlatform& Platform, TArray<FGridSurfaceRef>& OutChangedSurfaces);

	/**
	 * Adds a platform to BuiltPlatforms and records what it takes in: its module faces, its
	 * module edges - rim or interior - and its rim cells, each with the axis it faces along.
	 */
	void RecordPlatform(const FGridPlatform& Platform);

	/**
	 * The promotion rule: flanked, within its own plane and on its own side, by open all-object
	 * surfaces on opposite sides along either axis of that plane.
	 */
	bool IsFlankedByOpenFields(const FIntVector& Cell, EGridFace Face) const;

	/**
	 * The axes along which a cell carries broad faces, one bit each, or zero when nothing is
	 * built there. An interior cell carries one, and names the platform it is interior to in
	 * OutInteriorOwner - an index into BuiltPlatforms, or INDEX_NONE for anything else. A cell of
	 * rim, on a module edge or a node, can carry up to three.
	 */
	uint8 GetCellSurfaceAxes(const FIntVector& Cell, int32& OutInteriorOwner) const;

	/**
	 * The platform whose interior takes in a cell lying on the lattice plane facing NormalAxis:
	 * the one covering every module face the cell touches in that plane. INDEX_NONE when there is
	 * none - a cell on a platform's rim touches a face outside it.
	 */
	int32 FindInteriorOwner(const FIntVector& Cell, int32 NormalAxis) const;

	/** Whether a platform's tile has blocked off this side of its interior. */
	bool IsFaceBlocked(const FGridPlatform& Platform, EGridFace Face) const;

	/** Whether any platform covers this cell. */
	bool IsSolidCell(const FIntVector& Cell) const;

	/** What one face carries and whether it is built against, resolved together. */
	void ResolveSurface(const FIntVector& Cell, EGridFace Face, ESurfaceCapacity& OutCapacity, bool& bOutOccupied) const;

	/**
	 * Every placement FirstTile could take to start the structure: centred on the cube's centre
	 * layer, one to four of them for each way round it may lie.
	 */
	void GatherFirstPlatformCandidates(TArray<FGridPlatform>& OutCandidates) const;

	/** Every free placement of every usable tile around the structure, into TileCandidates. Walks the whole structure. */
	void GatherPlatformCandidates();

	/** Gathers every tile's candidate lists afresh if a rebuild has left them stale. */
	void EnsurePlatformCandidates();

	/**
	 * Re-judges one placement of the usable tile at TileIndex, adding it to or dropping it from
	 * its list to match. A placement not listed is only judged when bMayHaveComeFree says a
	 * placement may just have given it something to grow off. Returns whether it was judged.
	 */
	bool RefreshPlatformCandidate(int32 TileIndex, const FGridPlatform& Candidate, bool bMayHaveComeFree);

	/**
	 * Draws one platform of a kind from the placement stream - a tile by weight, then one of its
	 * placements - and places it. False when that kind has nowhere to go.
	 */
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

	/** One usable tile, and every free placement of it per kind (indexed by EPlatformKind), each list in ascending key order. */
	struct FTileCandidates
	{
		TObjectPtr<UPlatformTileData> Tile;
		TArray<FGridPlatform> ByKind[PlatformKindCount];
	};

	/*
	 * Everything derived from Platforms. None of it is reflected, so none of it is saved or
	 * copied with the actor - a loaded level or a PIE copy arrives with the list alone and
	 * rebuilds the rest. All of it is sparse: keyed by packed cell coordinates, and holding only
	 * what cannot be worked out from a coordinate on the spot. RebuildPlatformState makes it from
	 * scratch; AddPlacedPlatform keeps it up to date one placement at a time.
	 *
	 * The tiles are held here by pointer only. Tiles and Platforms hold them for real.
	 */

	/** The platforms that fit the lattice as it stands, each once, in list order. */
	TArray<FGridPlatform> BuiltPlatforms;

	/**
	 * Every module face a platform covers, keyed by min node and normal axis, with the index in
	 * BuiltPlatforms of the platform covering it. A platform's own key is its min face's.
	 */
	TMap<uint64, int32> ModuleFaceOwners;

	/**
	 * Every module edge a platform takes in, keyed by min node and axis: RimEdgeUse where one runs
	 * around a platform, InteriorEdgeUse where one runs across a platform's interior.
	 */
	TMap<uint64, uint8> ModuleEdgeUses;

	/**
	 * Every cell of rim - on a module edge or node around a platform - with the axes it carries
	 * broad faces along. Interior cells are not listed: whether one is built follows from the
	 * module faces around it.
	 */
	TMap<uint64, uint8> RimCellNormals;

	/** Rim surfaces promoted to all-object, keyed by cell and face. */
	TSet<uint64> PromotedFaces;

	/** The tiles growth draws from: Tiles, less empty entries, repeats, and any that do not fit the lattice. */
	TArray<TObjectPtr<UPlatformTileData>> UsableTiles;

	/** The shortest side of any usable tile: the narrowest step between two flush platforms that growth could still fill. */
	int32 SmallestTileSide = 0;

	/**
	 * Every free placement, per usable tile and in the same order as UsableTiles, then per kind,
	 * in ascending key order - the order the placement stream draws from. Kept up to date
	 * placement by placement: only placements taking in a module edge that a placement touched or
	 * opened can have changed, so only those are judged again.
	 */
	TArray<FTileCandidates> TileCandidates;

	/** Set by every rebuild, and by the first placement; the next draw then gathers the lists afresh. */
	bool bPlatformCandidatesStale = true;

	/** Platforms in the list that the last rebuild left out, so the warning is given once rather than every beat. */
	int32 ReportedSkippedPlatforms = 0;

	/** What the last warning about the tiles said, so it is given again only when that changes. */
	FString ReportedTileProblems;

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

	/** Module edges of rim, the beams. Starts from EdgePieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer EdgeLayer;

	/** Lattice nodes of rim, the posts. Starts from NodePieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer NodeLayer;

	/** Interiors of platforms whose tile brings its own mesh, a layer per tile. Made at runtime and never saved. */
	UPROPERTY(Transient, DuplicateTransient)
	TArray<FTileMeshLayer> TileMeshLayers;

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
	 * One instance per horizontal platform, spanning its whole interior - unless its tile brings
	 * a mesh of its own, which is built in a layer of that tile's instead.
	 *
	 * A platform's interior is drawn as one box because it was placed as one thing; its rim is
	 * drawn apart, as edges and nodes, because neighbours share it.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> HorizontalInteriorPieces;

	/** One instance per vertical platform, spanning its whole interior. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> VerticalInteriorPieces;

	/** One instance per module edge of rim, spanning its run. Shared edges are built once. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> EdgePieces;

	/** One instance per lattice node of rim. Shared nodes are built once. */
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
