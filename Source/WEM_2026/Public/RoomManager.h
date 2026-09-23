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
 * What a single grid cell has been unlocked to carry.
 *
 * A cell is never "a floor" or "a wall" itself - it is a surface that permits a class of
 * things to stand on it. Placement systems read this; nothing here spawns geometry.
 */
UENUM(BlueprintType)
enum class EGridSurface : uint8
{
	/** Nothing has claimed this cell. Carries nothing. */
	Empty UMETA(DisplayName = "Empty"),

	/** Beam surface: carries walls only. The purple rim in the reference drawings. */
	Wall UMETA(DisplayName = "Wall Surface"),

	/** Carries anything. The green field in the reference drawings. */
	AllObject UMETA(DisplayName = "All-Object Surface")
};

/** Which of a floor tile's four sides something stands on. */
UENUM(BlueprintType)
enum class EGridSide : uint8
{
	PosX UMETA(DisplayName = "+X"),
	NegX UMETA(DisplayName = "-X"),
	PosY UMETA(DisplayName = "+Y"),
	NegY UMETA(DisplayName = "-Y")
};

/**
 * One wall: the whole rim run along one side of one floor tile, corner posts included.
 *
 * Named by the tile it was raised from rather than by the cells it covers, so it travels
 * with that tile. The cells are derived from the pair on every rebuild.
 */
USTRUCT(BlueprintType)
struct FWallSegment
{
	GENERATED_BODY()

	/** Interior min corner of the tile this wall was raised from. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wall")
	FIntPoint TileMin = FIntPoint::ZeroValue;

	/** Which of that tile's sides it stands on. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wall")
	EGridSide Side = EGridSide::PosX;
};

/**
 * One storey's worth of the grid: what has been placed on it, and what that works out to.
 *
 * Every level shares the one cell lattice and differs only in height, so a cell coordinate
 * means the same thing on all of them and the levels stack in register.
 */
USTRUCT(BlueprintType)
struct FGridLevel
{
	GENERATED_BODY()

	/** Interior min corner of every tile laid down on this level. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Level")
	TArray<FIntPoint> Tiles;

	/** Every wall raised on this level. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Level")
	TArray<FWallSegment> Walls;

	/** What each cell carries. Derived, never edited in place. Indexed by CellToIndex. */
	UPROPERTY(Transient)
	TArray<EGridSurface> CellSurfaces;

	/** Whether each cell has been built on. Derived alongside the surfaces. */
	UPROPERTY(Transient)
	TArray<uint8> CellOccupied;

	/** The beams as built: this level's tile rims, and whatever the level below leaves under it. */
	UPROPERTY(Transient)
	TArray<FIntPoint> BeamCells;

	/** This level's corner posts, deduplicated - two walls meeting at a corner share one. */
	UPROPERTY(Transient)
	TArray<FIntPoint> WallPostCells;
};

/**
 * Directs the simulation's grid: a debug-visible lattice laid out across the landscape.
 *
 * The grid's corner origin is this actor's own world-space location and extends into
 * +X/+Y from there, so placing the actor at world origin gives a grid from world origin.
 * Actor rotation and scale are ignored - the grid stays axis-aligned to world X/Y.
 *
 * The lattice is drawn from OnConstruction, not from Tick, so it is visible in the editor
 * without entering Play.
 *
 * Cell addressing: every cell has a unique FIntPoint coordinate, X in [0, GridSizeX) and
 * Y in [0, GridSizeY), and a unique row-major index in [0, GetCellCount()). The two map
 * onto each other one-to-one via CellToIndex / IndexToCell. Coordinates are derived from
 * the grid's dimensions rather than stored per cell, so resizing the grid cannot leave
 * stale cell data behind. Anything that needs per-cell state (occupancy, for instance)
 * can keep a parallel array of GetCellCount() entries addressed by CellToIndex.
 *
 * Floorspace growth
 * -----------------
 * A floor tile is a FloorTileSize x FloorTileSize block of all-object cells, laid down
 * with its one-cell beam rim already around it - tile and beams arrive in the same beat.
 * The first tile lands anywhere on the grid; every later one lands flush against a tile
 * already down, with exactly one beam line between them. That puts the tile pitch at
 * FloorTileSize + 1, so neighbouring tiles share that beam line rather than doubling it up.
 *
 * A beam line caught between two tiles stops being a rim and joins the interior: any wall
 * cell flanked by all-object cells on opposite sides is promoted to all-object. The
 * promotion cannot reach the outer rim, since a rim cell always has emptiness on one side,
 * so the field grows while staying enclosed. It does cascade - the centre of a 2x2
 * arrangement of tiles opens up completely - so it is resolved to a fixpoint.
 *
 * CellSurfaces is derived state, rebuilt wholesale from FloorTiles and Walls. The promotion
 * rule is order-independent, so the same set of tiles always yields the same surfaces
 * regardless of the sequence they were placed in.
 *
 * Walls
 * -----
 * A wall stands on one whole side of a tile's rim - the run of cells facing the interior,
 * corners excluded - and two 1x1 posts are raised with it to fill those corners, so the
 * barrier is continuous from end to end. A wall may only go where the wall surface is still
 * free, which is what keeps walls on the outside of the floorspace: the moment two tiles
 * conjoin, the beam line between them has been promoted to all-object and no longer offers
 * itself.
 *
 * The cells a wall and its posts stand on are marked occupied, and occupancy is what the
 * promotion rule steps over. So a tile conjoining across a walled line still arrives and
 * still lays its own floorspace, but the line between them never opens: the two fields stay
 * separate, each still ringed by wall surface. Since purple is the potential for a wall and
 * a wall keeps the cell purple, a field is enclosed once every side around it has been
 * walled, and open on the sides that have not - rooms accumulate rather than arrive.
 *
 * Posts are the one thing two walls may share. A tile's +X and +Y walls both want a post in
 * the corner between them, and the second wall finds it already standing; only the run has
 * to be free. Runs and corners never collide, since every tile sits on the same pitch
 * lattice, so a corner cell is a corner for every tile that touches it.
 *
 * Storeys
 * -------
 * What a wall leaves behind when it tops out is a beam over its run and one over each of
 * its posts, resting on the level above. A run is a tile's width plus its two corners, which
 * is exactly one whole rim side of a tile - so a wall hands the level above a finished edge
 * to build against, and a tile may conjoin to it from either side, the same move as
 * conjoining to a tile already down. That is the only rule an upper level does not share
 * with the ground: its tiles have to start from a beam left by a wall, where the ground's
 * first tile started anywhere.
 *
 * Everything after that repeats. An upper level's tiles carry their own rims, its beam lines
 * open up where two tiles meet, walls stand on what stays closed, and those walls leave beams
 * for the level above them. A beam resting on a wall is an ordinary beam and opens like any
 * other, so a wall divides its own storey and not the one over it - each floor is free to be
 * laid out differently from the one it sits on.
 *
 * Levels are not drawn alike. The ground is notation - flat, weightless, a reading of where
 * things may go - while everything above it is built: slabs and beams of SlabThickness, walls
 * of WallHeight standing on them, so one storey is WallHeight + SlabThickness tall.
 */
UCLASS()
class WEM_2026_API ARoomManager : public AActor
{
	GENERATED_BODY()

public:
	ARoomManager();

	/** Hard cap on cells per axis. Mirrored by the ClampMax below, and re-applied in code. */
	static constexpr int32 MaxGridDimension = 512;

	/** Number of cells along world +X. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1", ClampMax = "512"))
	int32 GridSizeX = 52;

	/** Number of cells along world +Y. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1", ClampMax = "512"))
	int32 GridSizeY = 52;

	/** Edge length of one cell, in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1.0"))
	float CellSize = 20.0f;

	/** Manual Z nudge applied on top of the resolved base height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Placement")
	float GridHeightOffset = 0.0f;

	/** If true, a single downward trace at the grid's origin corner sets the base Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Placement")
	bool bSnapToLandscapeHeight = true;

	/** Collision channel used by that height trace. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Placement")
	TEnumAsByte<ECollisionChannel> LandscapeTraceChannel = ECC_WorldStatic;

	/** Cells per side of one floor tile's all-object interior, before its beam rim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Floorspace", meta = (ClampMin = "1"))
	int32 FloorTileSize = 8;

	/** Seconds between automatic placements. One tile with its rim, or one wall with its posts, per beat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Floorspace", meta = (ClampMin = "0.0"))
	float FloorTilePlacementInterval = 3.0f;

	/** Whether the placement beat runs on its own once play begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Floorspace")
	bool bAutoPlaceFloorTiles = true;

	/** Discards tiles and walls authored in the editor at BeginPlay, so a run starts from bare grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Floorspace")
	bool bClearFloorTilesOnBeginPlay = true;

	/** Seed for the placement stream. Zero draws a fresh seed each run. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Floorspace")
	int32 FloorTileRandomSeed = 0;

	/**
	 * The storeys, from the ground up. Index zero always exists, and a level is added above
	 * any level carrying a wall, since that wall leaves a beam resting up there.
	 *
	 * The tiles and walls in here are the whole of what has been placed; everything else is
	 * derived from them, so this list alone is the simulation's state.
	 *
	 * Deliberately kept out of the Details panel. Every placement changes it, and the panel
	 * answers a changed array by rebuilding its whole property tree - with this actor selected
	 * during a run that cost over a second a frame. The counts below stand in for it there.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Grid|Floorspace")
	TArray<FGridLevel> Levels;

	/** Tiles laid down across every level. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Floorspace")
	int32 TotalTileCount = 0;

	/** Walls raised across every level. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Floorspace")
	int32 TotalWallCount = 0;

	/** How many storeys the stack is currently deep. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Floorspace")
	int32 TotalLevelCount = 0;

	/**
	 * Chance that a beat raises a wall rather than laying a floor tile.
	 *
	 * The roll only decides which of the two is tried first: when the chosen kind has
	 * nowhere to go the other one takes the beat, so a beat is lost only when the grid has
	 * room for neither. That also covers the opening beat, where there is no tile to wall.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Walls", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WallPlacementChance = 0.5f;

	/**
	 * Height of a wall, and of the corner posts raised with it, in centimetres.
	 *
	 * 280 is the WBS70 storey height of 2.80m in Unreal's units, where one unit is one
	 * centimetre. Set it to 2.8 if a wall the height of a kerb is what was meant.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Walls", meta = (ClampMin = "0.0"))
	float WallHeight = 280.0f;

	/**
	 * Every wall raised so far, each named by a tile and one of its sides.
	 * As with FloorTiles, this list alone is the walls' state.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid|Walls")
	TArray<FWallSegment> Walls;

	/**
	 * Unit box every wall piece is built from: the engine's 100cm cube, centred on its own
	 * origin. Anything with those conventions can be swapped in.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Walls")
	TObjectPtr<UStaticMesh> WallMesh;

	/**
	 * Material the walls and posts are built from. Lit, unlike the surfaces below them:
	 * the surfaces are notation and read best flat, while a wall is a thing standing in
	 * the world and needs light falling across it to read as one. Tinted per layer, so one
	 * material covers both colours. It is expected to expose WallColorParameterName.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Walls")
	TObjectPtr<UMaterialInterface> WallMaterial;

	/** Vector parameter on WallMaterial that the two colours below drive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Walls")
	FName WallColorParameterName = FName(TEXT("Color"));

	/**
	 * The walls themselves.
	 *
	 * Unlike the plane colours above, this is a straight linear albedo and not pitched to
	 * suit an exposure: the walls are lit, so what lands on screen is whatever the level's
	 * exposure makes of them. A level left on auto-exposure renormalises the whole frame and
	 * will wash these out however they are set here - that is fixed in the level, with a
	 * post-process volume on manual exposure, not by dimming the colour.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Walls")
	FLinearColor WallColor = FLinearColor(0.25f, 0.10f, 0.03f);

	/** The corner posts. Held apart from the walls they fill in between. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Walls")
	FLinearColor WallPostColor = FLinearColor(0.21f, 0.21f, 0.21f);

	/**
	 * Thickness of the slabs and beams an upper storey is built from, in centimetres.
	 * One storey is this plus WallHeight, so the levels stack at a fixed pitch.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Storeys", meta = (ClampMin = "0.0"))
	float SlabThickness = 20.0f;

	/** The tile slabs of an upper storey. Built, unlike the flat notation on the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Storeys")
	FLinearColor SlabColor = FLinearColor(0.35f, 0.34f, 0.32f);

	/** The beams around those slabs, and the ones left resting on a wall. Grey, as the posts are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Storeys")
	FLinearColor BeamColor = FLinearColor(0.21f, 0.21f, 0.21f);

	/** Master toggle for the built geometry: the red floor tiles, grey wall beams and the walls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Planes")
	bool bShowPlanes = true;

	/**
	 * Unit plane every surface is built from: the engine's 100x100cm plane, centred on its
	 * own origin and facing +Z. Anything with those conventions can be swapped in.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Planes")
	TObjectPtr<UStaticMesh> PlaneMesh;

	/**
	 * Base material for both layers. It is not used directly - a dynamic instance per layer
	 * is made from it and tinted, so one material covers both colours.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Planes")
	TObjectPtr<UMaterialInterface> PlaneMaterial;

	/** Vector parameter on PlaneMaterial that the colours below drive. M_WEMDebug calls it "Color". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Planes")
	FName PlaneColorParameterName = FName(TEXT("Color"));

	/**
	 * One plane per floor tile, covering its whole interior.
	 *
	 * These are linear values through an unlit material, so what lands on screen also
	 * depends on the level's exposure - the defaults are pitched low because the default
	 * auto-exposure lifts them considerably. Tune here rather than in the material.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Planes")
	FLinearColor FloorTilePlaneColor = FLinearColor(0.45f, 0.015f, 0.015f);

	/** One plane per beam cell, so beams can never overlap each other or a tile. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Planes")
	FLinearColor WallBeamPlaneColor = FLinearColor(0.16f, 0.16f, 0.16f);

	/** Lifts the planes off the grid base so they do not z-fight the ground under them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Planes")
	float PlaneZOffset = 1.0f;

	/** Master toggle for the debug lattice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	bool bShowDebugGrid = true;

	/**
	 * Overlays what each cell is unlocked to carry, one inset square per claimed cell, in
	 * green and purple. This is the only view that shows which beams have been promoted -
	 * the planes cannot, since a promoted beam is still a beam - so it reads on top of them
	 * rather than instead of them, and both are on by default.
	 *
	 * The squares are instanced and only the cells a placement changed are touched, so the
	 * overlay costs little however far the grid has filled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	bool bShowSurfaces = true;

	/** Colour of the interior lattice lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor GridLineColor = FColor(200, 200, 200);

	/** Colour of the four outer edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor BoundaryLineColor = FColor::Blue;

	/** Cells that carry anything. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor AllObjectSurfaceColor = FColor(0, 230, 0);

	/** Cells that carry walls only, and are still free to. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor WallSurfaceColor = FColor(170, 0, 230);

	/** Cells already built on. Drawn over whatever the cell carries, since occupancy outranks it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug")
	FColor OccupiedSurfaceColor = FColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug", meta = (ClampMin = "0.1"))
	float GridLineThickness = 1.5f;

	/** Drawn thicker than the interior so the grid reads as surrounded by a boundary line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Debug", meta = (ClampMin = "0.1"))
	float BoundaryLineThickness = 5.0f;

	/** Marks DebugCell in the viewport so a single cell can be located by coordinate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	bool bHighlightDebugCell = false;

	/** Cell to mark. Nothing is drawn when it falls outside the grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	FIntPoint DebugCell = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	FColor DebugCellColor = FColor::Green;

	/** Base Z resolved at the last rebuild. Every cell shares it. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	double GridBaseZ = 0.0;

	/** Row-major index of DebugCell, or INDEX_NONE when it is outside the grid. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	int32 DebugCellIndex = INDEX_NONE;

	/** World-space centre of DebugCell, or zero when it is outside the grid. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	FVector DebugCellCenter = FVector::ZeroVector;

	/** World location fed through WorldToCell on each rebuild, for the inverse lookup. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Coordinates")
	FVector DebugProbeLocation = FVector::ZeroVector;

	/** Cell containing DebugProbeLocation, or (INDEX_NONE, INDEX_NONE) when off the grid. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	FIntPoint DebugProbeCell = FIntPoint(INDEX_NONE, INDEX_NONE);

	/** Total number of cells: GridSizeX * GridSizeY. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	int32 GetCellCount() const;

	/** True when the coordinate addresses a cell that exists in this grid. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	bool IsValidCell(const FIntPoint& Cell) const;

	/** Row-major index for a cell, or INDEX_NONE when the cell is outside the grid. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	int32 CellToIndex(const FIntPoint& Cell) const;

	/** Inverse of CellToIndex. Returns (INDEX_NONE, INDEX_NONE) for an out-of-range index. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	FIntPoint IndexToCell(int32 Index) const;

	/** World-space corner of a cell nearest the grid origin, at the grid's base Z. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	FVector GetCellMinCorner(const FIntPoint& Cell) const;

	/** World-space centre of a cell, at the grid's base Z. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	FVector GetCellCenter(const FIntPoint& Cell) const;

	/** World-space footprint of a cell. Flat: min and max share the grid's base Z. */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	FBox GetCellBounds(const FIntPoint& Cell) const;

	/**
	 * Cell containing a world location, ignoring Z.
	 * Returns false and sets OutCell to (INDEX_NONE, INDEX_NONE) when the location is
	 * outside the grid.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Coordinates")
	bool WorldToCell(const FVector& WorldLocation, FIntPoint& OutCell) const;

	/** How many storeys the stack is currently deep. Never less than one. */
	UFUNCTION(BlueprintPure, Category = "Grid|Floorspace")
	int32 GetLevelCount() const;

	/** World-space Z of a level's surface: the ground for level zero, a slab's top above that. */
	UFUNCTION(BlueprintPure, Category = "Grid|Floorspace")
	double GetLevelBaseZ(int32 Level) const;

	/** What a cell has been unlocked to carry on one level. Cells outside the grid carry nothing. */
	UFUNCTION(BlueprintPure, Category = "Grid|Floorspace")
	EGridSurface GetCellSurface(int32 Level, const FIntPoint& Cell) const;

	/**
	 * Whether a cell's surface has been built on.
	 *
	 * Occupancy is not the same as what a cell carries: a walled cell is still a wall
	 * surface, it has only stopped being a free one. Nothing new may be placed there, and
	 * the promotion that opens beam lines into floorspace steps over it.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Floorspace")
	bool IsCellOccupied(int32 Level, const FIntPoint& Cell) const;

	/** Number of tiles laid down on one level. */
	UFUNCTION(BlueprintPure, Category = "Grid|Floorspace")
	int32 GetTileCount(int32 Level) const;

	/** Number of walls raised on one level. */
	UFUNCTION(BlueprintPure, Category = "Grid|Walls")
	int32 GetWallCount(int32 Level) const;

	/**
	 * Whether a wall could be raised along one side of a tile.
	 *
	 * Every cell of that side's rim run has to be a wall surface and still free. A side a
	 * neighbouring tile has conjoined across reads as all-object and fails; a side already
	 * walled - from either of the two tiles that meet along it - reads as occupied and fails
	 * too, so one line can never carry two walls. The corner posts are not tested, since the
	 * only thing they can meet there is another post, which they share.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Walls")
	bool CanPlaceWallAt(int32 Level, const FIntPoint& TileMin, EGridSide Side) const;

	/**
	 * Whether a tile whose interior starts at this cell could be laid down on one level.
	 *
	 * Its full footprint - interior plus beam rim - must fit inside the grid, and it must
	 * neither cover an existing tile's beams nor let an existing tile's beams cover it.
	 * Two tiles sharing a single beam line pass this; anything closer does not.
	 */
	UFUNCTION(BlueprintPure, Category = "Grid|Floorspace")
	bool CanPlaceFloorTileAt(int32 Level, const FIntPoint& InteriorMin) const;

	/**
	 * Lays down one tile with its beam rim, then rebuilds the surfaces.
	 *
	 * The ground's first tile goes anywhere it fits. After that a tile goes flush against a
	 * tile already down on its own level, one beam line apart, or - above the ground - to
	 * either side of a beam a wall has left resting there. The choice is drawn from the
	 * placement stream across every level at once, so the stack grows where there is room
	 * rather than storey by storey. Returns false when nowhere is left.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Floorspace")
	bool PlaceNextFloorTile();

	/**
	 * Raises one wall with its two corner posts, then rebuilds the surfaces.
	 *
	 * The side is drawn from the placement stream out of every tile side on every level that
	 * is still free to take one. Returns false when no side is.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Walls")
	bool PlaceNextWall();

	/** Drops every storey and re-seeds the placement stream. Back to bare grid. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Floorspace")
	void ClearFloorTiles();

	//~ Begin AActor Interface
	virtual void OnConstruction(const FTransform& Transform) override;
	//~ End AActor Interface

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

private:
	/** How many colours the surface overlay has: all-object, wall, and occupied. One component each. */
	static constexpr int32 SurfaceMarkLayerCount = 3;

	/** Where one cell's overlay square stands: which colour's component, and which instance of it. */
	struct FSurfaceMarkSlot
	{
		int32 Layer = INDEX_NONE;

		/** INDEX_NONE while the square is still waiting to be added. */
		int32 Instance = INDEX_NONE;

		/** The sync that last found this cell claimed. Anything older is no longer claimed at all. */
		uint32 SeenStamp = 0;
	};

	/** Every visual from scratch: the planes, the overlay and the debug lattice. */
	void RebuildVisuals();

	/**
	 * What has to follow one placement. A placement only ever adds to the grid, so the lattice
	 * stays as it is and the overlay takes just the cells that changed.
	 */
	void UpdateVisualsAfterPlacement();

	/** Flushes and redraws the debug lattice. Owned batcher, so no other system's lines are touched. */
	void RebuildDebugGrid();

	/** Drops every overlay square and the record of them, so the next sync draws the lot. */
	void ResetSurfaceMarks();

	/** Brings the overlay in line with the surfaces, adding, recolouring or dropping only what differs. */
	void SyncSurfaceMarks();

	/** Takes one square out of a colour's component without moving any square but the last. */
	void RemoveSurfaceMark(int32 Layer, int32 Instance);

	UInstancedStaticMeshComponent* GetSurfaceMarkLayer(int32 Layer) const;

	/** Rebuilds the built geometry: one plane per floor tile, one per beam cell, one box per wall and post. */
	void RebuildPlanes();

	/**
	 * Dynamic instance of a base material tinted to Color, made once and re-tinted after that.
	 * Returns null when no base material is set, which leaves the mesh's own material in place.
	 */
	UMaterialInstanceDynamic* ResolveTintedMaterial(
		TObjectPtr<UMaterialInstanceDynamic>& CachedMaterial,
		UMaterialInterface* BaseMaterial,
		FName ParameterName,
		const FLinearColor& Color);

	/** Resolves the world-space Z the whole grid sits at. Cached into GridBaseZ each rebuild. */
	double ResolveGridBaseZ() const;

	/** Rebuilds every level's derived state from its tiles and walls. Wholesale, so it cannot drift out of step. */
	void RebuildSurfaces();

	/** Adds a level above any that carries a wall, so the beams that wall leaves have somewhere to rest. */
	void EnsureLevelCapacity();

	/** Every interior min corner a first, unconstrained tile could occupy. */
	void GatherFirstTileCandidates(TArray<FIntPoint>& OutCandidates) const;

	/** Every interior min corner a next tile could occupy on one level. */
	void GatherFloorTileCandidates(int32 Level, TArray<FIntPoint>& OutCandidates) const;

	/** Every tile side a wall could still be raised on, on one level. */
	void GatherWallCandidates(int32 Level, TArray<FWallSegment>& OutCandidates) const;

	/** The placement beat: one tile or one wall. Stops its own timer once the grid has no room left. */
	void AdvancePlacement();

	void ResetPlacementStream();

	void AppendLatticeLines(TArray<FBatchedLine>& Lines) const;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FloorTilePlaneMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> WallBeamPlaneMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> WallPieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> WallPostPieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SlabPieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BeamPieceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> AllObjectMarkMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> WallMarkMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> OccupiedMarkMaterial;

	/**
	 * Every cell the overlay has drawn, keyed by level and cell index. Mirrors the mark
	 * components and is neither saved nor duplicated with them, so a sync that finds the two
	 * out of step starts again from scratch rather than trusting it.
	 */
	TMap<uint64, FSurfaceMarkSlot> SurfaceMarkSlots;

	/** Per colour, the key of the cell each instance stands for, in instance order. */
	TArray<uint64> SurfaceMarkInstanceKeys[SurfaceMarkLayerCount];

	/** Bumped once per sync, to tell the cells it found claimed from ones it did not. */
	uint32 SurfaceMarkSyncStamp = 0;

	FRandomStream PlacementStream;

	FTimerHandle PlacementTimerHandle;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;

	/*
	 * The instanced components below are not VisibleAnywhere. The Details panel shows a visible
	 * component's properties inline, its per-instance array included, and every placement changes
	 * those arrays - so with this actor selected during a run the panel rebuilt thousands of rows
	 * a beat. BlueprintReadOnly keeps them reachable from Blueprint and MCP.
	 */

	/** One instance per tile on the ground, scaled to the tile's whole interior. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> FloorTilePlanes;

	/** One instance per beam cell on the ground. Adjacent instances are coplanar, so a run reads as one beam. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> WallBeamPlanes;

	/** One instance per tile above the ground, where a tile is a slab rather than a reading. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> SlabPieces;

	/** One instance per beam cell above the ground, including the beams left resting on a wall. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> BeamPieces;

	/**
	 * One instance per wall, spanning its whole run.
	 *
	 * A wall is drawn as one box because it was placed as one thing, the way a floor tile is
	 * drawn as one plane.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> WallPieces;

	/** One instance per corner post. Held apart from the walls so it can be coloured apart from them. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> WallPostPieces;

	/** The overlay's green squares: one per all-object cell, on every level. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> AllObjectSurfaceMarks;

	/** The overlay's purple squares: one per wall-surface cell still free to take a wall. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> WallSurfaceMarks;

	/** The overlay's white squares: one per cell already built on. */
	UPROPERTY(BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInstancedStaticMeshComponent> OccupiedSurfaceMarks;

	/**
	 * Draws the grid. An actor-owned batcher rather than DrawDebugLine/FlushPersistentDebugLines:
	 * the global persistent-line buffer is world-wide, so flushing it to redraw this grid would
	 * also wipe any other system's persistent debug lines, and vice versa.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<ULineBatchComponent> GridLineBatcher;
};
