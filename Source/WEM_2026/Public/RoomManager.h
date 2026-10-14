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
class UPlatformStructureData;
class UPlatformTileData;
class UStructureCategory;
class UStaticMesh;

/** Defined in PlatformStructureData.h. */
enum class ENatureForm : uint8;

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
 *
 * The kind is the world's view of it. What it is in its own frame - floor or wall - follows from its
 * Up instead, and the two need not agree: a platform lying flat in the world can be a wall.
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

	/** The tile it was laid from, which gives it its size. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	TObjectPtr<UPlatformTileData> Tile;

	/** Laid the other way round: the tile's length runs where its width would, and its width where its length would. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	bool bTurned = false;

	/**
	 * The way up in the platform's own frame, which need not be the world's. Along its normal it is a
	 * floor: its Ground face looks along Up and its Ceiling face the other way. In its plane it is a
	 * wall, whose structures stand with their tops along Up. Taken from the platform it grew off, and
	 * changed only where growth turns it. +Z for the first platform, and for any platform saved before
	 * platforms had one, so those read as they always did.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	EGridFace Up = EGridFace::PosZ;

	/**
	 * The structure filling the platform's positive face - the one looking out along +X, +Y or +Z - and
	 * the one filling its negative face. A two-sided structure fills both and is held in
	 * both. A platform placed before there were structures has neither: it is drawn with no fill, and
	 * both of its faces carry anything.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	TObjectPtr<UPlatformStructureData> PositiveStructure;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	TObjectPtr<UPlatformStructureData> NegativeStructure;

	EPlatformKind GetKind() const
	{
		return Normal == EGridAxis::Z ? EPlatformKind::SurfaceHorizontal : EPlatformKind::SurfaceVertical;
	}

	/** Whether it is a wall in its own frame: its Up lies in its plane. A wall need not stand upright in the world. */
	bool IsWall() const
	{
		return static_cast<int32>(Up) / 2 != static_cast<int32>(Normal);
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
 * A box of cells growth may not build through - held for something else standing there, such as
 * a placed object. Inclusive at both corners.
 */
USTRUCT(BlueprintType)
struct FGridObstacle
{
	GENERATED_BODY()

	/** What AddObstacle returned for it, and what RemoveObstacle takes back. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Obstacle")
	int32 Handle = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Obstacle")
	FIntVector MinCell = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Obstacle")
	FIntVector MaxCell = FIntVector::ZeroValue;
};

/**
 * One kind of built piece - a structure, edge, node - spread over a run of small instanced
 * components, "chunks", filled one at a time.
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
	 * component for this kind of piece, or for a structure one made alongside the layer; the rest
	 * are made as the structure grows. None but the actor's own are saved, and none are
	 * copied with the actor: each layer property on ARoomManager is DuplicateTransient.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Chunks;

	/** Every piece drawn so far, keyed by what it stands for, with the sync that last found it. */
	TMap<uint64, uint32> PieceStamps;

	/** Bumped once per sync. Never zero after the first, which marks a piece as not yet drawn. */
	uint32 SyncStamp = 0;
};

/** The structures filling the platforms' openings: one piece layer per structure asset. */
USTRUCT()
struct FStructureLayer
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UPlatformStructureData> Structure;

	/** Its first chunk is made with the layer, set up like EdgePieces, rather than being one of the actor's own. */
	UPROPERTY(Transient)
	FRoomPieceLayer Pieces;

	/** The beams' material tinted to the structure's flat colour, for a nature structure drawn as a slab for want of a mesh. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FlatMaterial;
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
 * with somewhere to go, then draws one of that tile's free placements - again, now and then, where
 * it goes against a growth rule (see Up) - and fills it.
 *
 * Up
 * --
 * Floor, wall and ceiling are not the world's. Every platform carries its own Up (FGridPlatform::Up),
 * and what it is follows from that: a floor when Up lies along its normal, a wall when Up lies in its
 * plane. The first platform's Up is +Z. Every one after takes the Up of a platform it grows off -
 * one drawn at random among those it shares a module edge with - so a platform folding off a floor is
 * a wall standing on it, one folding off a wall's top or foot is a floor, and one going on in a
 * platform's plane is what that platform is.
 *
 * Inherited and nothing more, every Up would stay +Z. Three turns, each switched on its own, change
 * that: a platform folding off another may take the opposite Up (bFlipOnFold); a wall folding off a
 * floor's Ceiling face, where that face holds items, hangs from it upside down (bHangFromCeilings);
 * and a platform carrying a wall on in its plane may become a floor whose Ground looks the way one of
 * the wall's holding faces does (bQuarterTurn). Whatever they turn, the platforms growing off it
 * inherit. Only a way up the structures can fill is taken: failing that the Up it would have
 * inherited, then the Up of another platform it touches, and failing all of them +Z, which every
 * placement drawn can be filled for.
 *
 * Two growth rules lean where platforms fold off others, by drawing a placement again when it goes
 * against them: walls stand up on a ground that holds items rather than hang below it
 * (bWallsStandOnGrounds), and off a wall's holding face a floor goes to the end of the wall its room
 * still needs (bFloorsCloseRooms). Both read Up, so they hold upside down as well.
 *
 * Structures
 * ----------
 * A platform is built as beams around an opening, and the opening is filled by structures - meshes
 * made for it: one two-sided structure, filling both of its faces, or two one-sided ones, one per
 * face. The two faces pair back to back: a floor is a roof or a ground (on the face looking along its
 * Up) over a ceiling, or a ground over a ground, and a wall is wall on both faces. The fill is drawn in the same
 * beat as the platform, from a stream of its own, since what fills a platform can change where growth may go next. A tile that
 * no structures could fill completely, laid a given way, is not laid that way, and the platform
 * that starts growth is always filled with load-bearing structures.
 *
 * A face is pulled toward the categories of the faces beside it, so like grows beside like. Faces
 * meet around the module edges platforms share: walking round such an edge, each platform met
 * there meets the next on the side that faces it - two floors side by side top to top and
 * underside to underside, a floor and the wall standing on it top to inner face and underside to
 * outer face. A filled face keeps a streak per category of its structure: one more than the
 * longest streak of that category among the faces beside it when it was filled, so a face that
 * lacks the category breaks the chain. The longer the streak, the likelier it carries on
 * (ContinueChance, rising to MaxContinueChance at StreakForMaxChance).
 *
 * The two faces of a one-sided pair are not beside each other, but they are back to back: behind an
 * interior, growth reaches for an exterior (bInteriorBacksExterior), each structure's Side saying which
 * it is. A wall standing on an interior ground is an interior on the face that looks over it
 * (bWallsFaceInteriorGrounds).
 *
 * A platform filled with anything not load-bearing blocks some of its beams: a wall the ones along
 * its top - the side its Up points to - so nothing is built onto it while more panels can still join beside it, and a floor or
 * ceiling every beam of its rim no platform used before it. No later platform takes in a blocked
 * module edge. A beam is built only where some platform using it has not blocked it, and a post
 * only where a built beam ends; a beam or post that is not built is not there at all - no surface,
 * nothing solid, no part in promotion. Only module edges are blocked, never nodes, so a panel
 * joining beside a wall that is not load-bearing can still share the post at its top corner.
 *
 * A fence is a two-sided structure that carries on as a run of copies of itself. When a platform is
 * filled with one, each end of it with room beyond is drawn for (FenceContinueChance), and an end
 * that carries on waits in a queue. While anything waits, a beat places the next panel of the
 * oldest waiting end instead of drawing a platform: the same fence on the same tile laid the same
 * way round, with the same Up, straight on in the same plane - across its Up on a wall, along the
 * fence's width across a floor - or, for a wall, round the beam at its end onto a wall facing the
 * other way (FenceTurnChance), laid whichever way round keeps it as tall. A panel that stands on something or hangs from it is preferred, so a parapet
 * follows its roof's edge round a corner. An end that has reached a panel of its own fence - a run
 * closing round a roof, or meeting another of the same fence - is closed. A platform a normal beat
 * happens to lay on a waiting spot, laid as the run is, carries the run on. A fence that is not
 * load-bearing may carry on across the edge its own run blocked.
 *
 * Nature
 * ------
 * Every structure belongs to a section (UPlatformStructureData::Section): the city, which growth fills
 * its platforms from, or nature - ground, cliff and water - which growth never draws. A nature platform
 * is one-sided: its nature face looks into the open air, and behind it stands any one-sided urban
 * structure of the face's type, a ceiling under ground and water and a wall behind a cliff. Ground and
 * water lie along their Up, their nature face looking along it; a cliff is a wall. A nature platform is
 * always load-bearing, whatever stands behind it.
 *
 * Which faces meet across a shared beam is the walk the category pull takes (see Structures), and a
 * nature face meets only what its form allows: ground meets more ground or water in its own plane, an
 * exterior - one of both sides counts - and a cliff along the cliff's top or foot, the edges across its
 * Up; water meets ground or more water in its plane, and a cliff along its top or foot; a cliff meets
 * ground or water along its own top or foot, and other cliffs with its Up. So a landscape's nature faces
 * all look into the same air, and their backs into the city. A placement is held to that for every face
 * it meets, nature or not: the city grows against ground only with exteriors facing it - the faces that
 * meet ground are drawn from exteriors - and never against water or a cliff.
 *
 * Nature has a beat of its own (NaturePlacementInterval), and lays one piece a beat, drawn from a
 * stream of its own. A field still spilling out takes the beat: ground and water are laid as fields,
 * each spilling out, a piece a beat, to the size it drew when it started (GroundSpillMin and Max,
 * WaterSpillMin and Max) - in its own plane, beside the pieces it has laid, likelier where it would
 * meet more of them - and the oldest still spilling goes first. Otherwise the beat starts something,
 * drawn by GroundWeight, CliffWeight and WaterWeight among what has somewhere to go: a field of ground,
 * against an exterior or at a cliff's top or foot; a cliff, off a field's outline or carrying one on;
 * or a pool of water, beside ground or at a cliff's top or foot. Where they could go is sampled
 * rather than walked in full, so a beat costs the same however far nature has spread.
 *
 * Every piece of ground and water keeps the air above it open: a box of cells out along its Up, as
 * high as it drew between MinAirStoreys and MaxAirStoreys, over its opening and over the seam it shares
 * with any field laid before it in its plane with its Up - but never over its outline, so cliffs still
 * rise and drop there. The air holds growth back as an obstacle does, without changing any surface, and
 * a piece is only laid where MinAirStoreys of it are clear of every platform already. Its height is
 * drawn from the run's seed and the piece itself, so a rebuild keeps the same air.
 *
 * Surfaces
 * --------
 * A surface is one face of one cell. Only a platform's two broad faces count - the two faces of
 * each of its cells along its normal - and the thin outer edge of a rim does not. A cell that
 * platforms facing two ways both run through carries surfaces along both.
 *
 * What a surface carries follows from the cell: interior surfaces carry anything, rim surfaces
 * walls only. A structure that holds no items closes off the face it fills
 * (UPlatformStructureData::bHoldsItems), and there the interior carries nothing and is not a
 * surface at all. A surface is occupied once the cell it
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
 *
 * Obstacles
 * ---------
 * Something else can hold a box of cells against growth - a placed object, standing in the air
 * off a segment. Growth never deletes what stands in its way; it simply does not build there. A
 * platform only ever covers lattice cells, and those come in three kinds of piece: the inside of
 * a module face, the run of a module edge, and a node. So a box is turned once, as it is added,
 * into the keys of the pieces it cuts, and a placement is refused if it takes in any of them -
 * the same few lookups however many obstacles stand.
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
	 * The structures that fill the platforms' openings, each drawn by its weight among those that fit
	 * the face. A plain list, like Tiles.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures")
	TArray<TObjectPtr<UPlatformStructureData>> Structures;

	/**
	 * The chance that a face beside a filled one carries on a category that face began: its streak
	 * is one, it has no filled neighbour of that category. Each neighbour with a category the face
	 * could take offers its strongest chance, and the face is pulled when any offer is taken.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ContinueChance = 0.35f;

	/** The chance a streak carries on once it is StreakForMaxChance long. It rises evenly from ContinueChance to here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxContinueChance = 0.70f;

	/** How long a streak has to be to carry on with MaxContinueChance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures", meta = (ClampMin = "2"))
	int32 StreakForMaxChance = 4;

	/**
	 * Draws the back of an interior face from the exteriors, with InteriorBacksExteriorChance - where
	 * any structure that fits the back is one. Each structure's Side says which it is; one of both sides
	 * counts as an exterior here, and binds nothing behind itself. Behind an exterior, anything that
	 * fits may follow.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures")
	bool bInteriorBacksExterior = true;

	/** The chance the back of an interior face is drawn from the exteriors, when that is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bInteriorBacksExterior"))
	float InteriorBacksExteriorChance = 0.8f;

	/**
	 * Draws a wall's face from the interiors (those of both sides among them), with
	 * WallsFaceInteriorGroundsChance, where it looks over an interior ground the wall stands on: a floor
	 * meeting it in a corner whose Ground face, filled with an interior - not one of both sides - looks
	 * back at the wall. That face is drawn first, so what goes behind it follows from it.
	 * It outranks bInteriorBacksExterior, so a wall between two interior grounds is inside on both faces.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures")
	bool bWallsFaceInteriorGrounds = true;

	/** The chance a wall's face over an interior ground is drawn from the interiors, when that is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bWallsFaceInteriorGrounds"))
	float WallsFaceInteriorGroundsChance = 0.95f;

	/**
	 * The chance each end of a fence panel, with room beyond it, carries its run on by another panel.
	 * Each end goes on for a run of panels of its own, on average p / (1 - p) of them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FenceContinueChance = 0.8f;

	/**
	 * The chance a wall fence that could both go straight on and turn the corner turns, split evenly
	 * between the ways it could turn. Where it can only turn, it turns.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Structures", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FenceTurnChance = 0.25f;

	/**
	 * Lets a platform folding off another take the opposite Up, with FlipChance, so it stands upside
	 * down against the one it grew off - and everything growing off it after.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up", meta = (DisplayName = "Flip On Perpendicular Growth"))
	bool bFlipOnFold = false;

	/** The chance a platform folding off another takes the opposite Up, when that is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bFlipOnFold"))
	float FlipChance = 0.15f;

	/**
	 * A wall folding off a floor's Ceiling face, where that face holds items, hangs from it: its Up points
	 * away from the floor, so in its own frame it stands on the ceiling, upside down to the floor above.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up")
	bool bHangFromCeilings = false;

	/**
	 * Lets a platform carrying a wall on in its plane turn a quarter, with QuarterTurnChance: it becomes a
	 * floor whose Ground looks the way one of the wall's holding faces does - drawn evenly when both hold
	 * items - so floors open out of walls. A wall holding nothing on either face is never turned.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up", meta = (DisplayName = "Quarter-Turn"))
	bool bQuarterTurn = false;

	/** The chance a platform carrying a wall on in its plane turns into a floor, when that is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bQuarterTurn"))
	float QuarterTurnChance = 0.15f;

	/**
	 * Off a floor whose Ground face holds items, walls stand up on it rather than hang below it: a
	 * placement folding off the Ceiling side is drawn again with WallsStandChance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up")
	bool bWallsStandOnGrounds = true;

	/** The chance a wall about to hang below a ground that holds items is drawn again, when that is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bWallsStandOnGrounds"))
	float WallsStandChance = 0.8f;

	/**
	 * Off a wall's holding face, a floor goes to the end of it - its top or its foot, across its Up - that
	 * the room it faces still needs: the foot when a ceiling closes its top on that side, the top when a
	 * ground already runs along its foot, and the foot when it has neither. A placement at the other end
	 * is drawn again with FloorsCloseRoomsChance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up", meta = (DisplayName = "Floor Or Ceiling By Opposite Edge"))
	bool bFloorsCloseRooms = true;

	/** The chance a floor about to go to the end of a wall its room does not need is drawn again, when that is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Up", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bFloorsCloseRooms"))
	float FloorsCloseRoomsChance = 0.8f;

	/**
	 * How large a patch of one category the average face sits in: the patches' sizes, each weighted
	 * by itself. A patch is the faces carrying a category that each joined another carrying it
	 * beside them; a face of two categories counts in a patch of each. Readout only.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Structures")
	double CategoryPatchSize = 0.0;

	/** Whether the nature beat runs on its own once play begins. With no nature among Structures it lays nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature")
	bool bAutoPlaceNature = true;

	/** Seconds between nature beats. One piece of nature a beat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "0.0"))
	float NaturePlacementInterval = 1.0f;

	/**
	 * How often a nature beat with no field spilling starts a field of ground - against an exterior, or at
	 * a cliff's top or foot - relative to the other two, among those with somewhere to go.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "0.0"))
	float GroundWeight = 1.0f;

	/** How often such a beat raises a cliff off a field's outline, or carries a cliff on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "0.0"))
	float CliffWeight = 1.0f;

	/** How often such a beat starts a pool of water beside ground, or at a cliff's top or foot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "0.0"))
	float WaterWeight = 0.5f;

	/** The fewest pieces a field of ground spills out to, the one it started from included. Drawn evenly up to GroundSpillMax. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "1"))
	int32 GroundSpillMin = 4;

	/** The most pieces a field of ground spills out to. It stops sooner where it has no room. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "1"))
	int32 GroundSpillMax = 12;

	/** The fewest pieces a pool of water spills out to, the one it started from included. Drawn evenly up to WaterSpillMax. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "1"))
	int32 WaterSpillMin = 2;

	/** The most pieces a pool of water spills out to. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature", meta = (ClampMin = "1"))
	int32 WaterSpillMax = 6;

	/** Cells in one storey, the measure of the air kept open above ground and water. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "1"))
	int32 StoreyCells = 16;

	/**
	 * The fewest storeys of air kept open above every piece of ground and water, out along its Up. A piece is
	 * only laid where this much is clear of every platform. Zero keeps no air.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0"))
	int32 MinAirStoreys = 3;

	/** The most storeys of air a piece keeps open. Each piece keeps a height of its own, drawn between the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0"))
	int32 MaxAirStoreys = 5;

	/** Draws the air kept open above ground and water as boxes of lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air")
	bool bShowAir = true;

	/** Colour of the air kept open above ground and water. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air")
	FColor AirColor = FColor(255, 190, 40);

	/** Platforms of ground standing. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Nature")
	int32 GroundCount = 0;

	/** Platforms of cliff standing. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Nature")
	int32 CliffCount = 0;

	/** Platforms of water standing. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Nature")
	int32 WaterCount = 0;

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

	/**
	 * Every box held against growth, in the order they were added. Kept out of the Details panel for
	 * the same reason Platforms is. Transient: what holds them - placed objects - is not saved either.
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Grid|Obstacles")
	TArray<FGridObstacle> Obstacles;

	/** Master toggle for the built geometry: every beam, post and structure. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	bool bShowPieces = true;

	/**
	 * Unit box every beam and post is built from: the engine's 100cm cube, centred on its own
	 * origin. Anything with those conventions can be swapped in. The openings between them are
	 * filled by the structures, with meshes of their own.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	TObjectPtr<UStaticMesh> PieceMesh;

	/**
	 * Material the beams and posts are built from. Lit, unlike the surface overlay laid over them:
	 * the overlay is notation and reads best flat, while a platform is a thing standing in the world
	 * and needs light falling across it to read as one. Tinted per layer, so one material covers
	 * both colours. It is expected to expose PieceColorParameterName.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	TObjectPtr<UMaterialInterface> PieceMaterial;

	/** Vector parameter on PieceMaterial that the colours below drive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Pieces")
	FName PieceColorParameterName = FName(TEXT("Color"));

	/**
	 * The beams: one run per module edge of rim, however many platforms share it. The structures
	 * keep their own materials.
	 *
	 * This and the colour below are straight linear albedos and not pitched to suit an exposure:
	 * the pieces are lit, so what lands on screen is whatever the level's exposure makes of them. A
	 * level left on auto-exposure renormalises the whole frame and will wash these out however they
	 * are set here - that is fixed in the level, with a post-process volume on manual exposure, not
	 * by dimming the colour.
	 */
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

	/** The structure filling that surface's face, when the surface lies across a platform's opening. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	TObjectPtr<UPlatformStructureData> DebugProbeStructure;

	/** Its categories, each with its streak on that face. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	FString DebugProbeCategories;

	/** Whether that structure is a fence, and its platform a panel of a run. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	bool bDebugProbeInFenceRun = false;

	/** What its fill makes that face, which side its structure stands for, and its platform's Up, as in "roof, exterior, up -Z". Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Coordinates")
	FString DebugProbeRole;

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

	/** Whether any platform covers this cell, rim or interior. */
	UFUNCTION(BlueprintPure, Category = "Grid|Surfaces")
	bool IsCellSolid(const FIntVector& Cell) const;

	/**
	 * Changes with every placement and every rebuild, so whatever is worked out from the structure
	 * can be kept until it does. Obstacles leave it alone: they change no surface.
	 */
	int32 GetStructureRevision() const
	{
		return StructureRevision;
	}

	/**
	 * Every surface the placements since FromRevision may have changed - laid, built against, or
	 * promoted to carry anything - in the order they changed, so what is worked out from the
	 * structure can follow it a placement at a time instead of walking all of it again. False when
	 * that cannot be told: the structure was rebuilt since, or it was too long ago to be kept.
	 */
	bool GatherSurfaceChangesSince(int32 FromRevision, TArray<FGridSurfaceRef>& OutSurfaces) const;

	/**
	 * Changes whenever the platform list is replaced or cleared - which also clears the obstacles -
	 * so anything standing on the old structure knows to go.
	 */
	int32 GetRebuildGeneration() const
	{
		return RebuildGeneration;
	}

	/**
	 * Reserves an inclusive box of cells against growth: no platform will be placed that covers any
	 * of them. Returns a handle for RemoveObstacle.
	 */
	UFUNCTION(BlueprintCallable, Category = "Grid|Obstacles")
	int32 AddObstacle(const FIntVector& MinCell, const FIntVector& MaxCell);

	/** Gives an obstacle's cells back to growth. */
	UFUNCTION(BlueprintCallable, Category = "Grid|Obstacles")
	void RemoveObstacle(int32 Handle);

	UFUNCTION(BlueprintCallable, Category = "Grid|Obstacles")
	void ClearObstacles();

	/**
	 * The lattice pieces an obstacle box would cut, as world boxes: the insides of module faces,
	 * the runs of module edges, and nodes. For drawing what an obstacle holds back.
	 */
	void GetObstaclePieceBounds(const FIntVector& MinCell, const FIntVector& MaxCell, TArray<FBox>& OutPieces) const;

	/** Number of platforms standing in the cube. */
	UFUNCTION(BlueprintPure, Category = "Grid|Platforms")
	int32 GetPlatformCount() const;

	/**
	 * Whether a platform could be placed: at its min node, facing along its axis, laid from its
	 * tile the way round it says.
	 *
	 * Its tile has to allow that orientation - horizontal or vertical - at all.
	 * It has to sit on the lattice with its whole footprint inside the cube, taking no module
	 * face already taken, with nothing built across its interior and no other interior under its
	 * rim, running through no obstacle, and share at least one module edge with a platform already
	 * placed. It has to lie flush
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
	 * across the whole structure at once. While a fence run waits to grow, the beat places its next
	 * panel instead. Returns false when nothing fits anywhere.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Platforms")
	bool PlaceNextPlatform();

	/** Drops every platform and every fence end waiting to grow, and re-seeds the streams. Back to an empty cube. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Platforms")
	void ClearPlatforms();

	/**
	 * One nature beat: the next piece of the oldest field still spilling out, or else a field of ground, a
	 * cliff or a pool of water, started where one can go. Returns false when nothing was laid.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Nature")
	bool PlaceNextNature();

	//~ Begin AActor Interface
	virtual void OnConstruction(const FTransform& Transform) override;
	//~ End AActor Interface

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

private:
	/** Lets the automation tests read the candidate lists and the drift tally. */
	friend struct FRoomManagerTestAccess;

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
	 * all - nothing built faces that way, or its structure closes that side off.
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
		TArray<FPlacedPiece> Edges;
		TArray<FPlacedPiece> Nodes;

		/** The structures filling the openings, per structure. */
		TMap<UPlatformStructureData*, TArray<FPlacedPiece>> Structures;
	};

	/**
	 * Brings the built geometry in line with the platforms - one piece per structure filling an
	 * opening, per module edge and per node of rim - adding only what is new. Walks the whole
	 * structure. Returns how many pieces it had to add.
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

	/** The layer a structure is built in, made the first time the structure needs one. */
	FStructureLayer& FindOrAddStructureLayer(UPlatformStructureData* Structure);

	/** The mesh a structure's layer is built from and the material put over it: none, so it keeps its own, unless it is nature drawn flat. */
	void ResolveStructureLayerLook(FStructureLayer& Layer, UStaticMesh*& OutMesh, UMaterialInterface*& OutMaterial);

	/** Takes down every structure's layer, first chunk and all. */
	void ResetStructureLayers();

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
	 * and no other platform's interior along the module edges around it. Nor may any of its
	 * pieces - module faces, module edges and nodes, rim or interior - cut an obstacle, nor any of
	 * its module edges be one a platform that is not load-bearing has blocked. The half of
	 * CanPlacePlatform that asks only what is where, which the rebuild holds the list to as well.
	 */
	bool IsSpaceFree(const FGridPlatform& Platform) const;

	/**
	 * CanPlacePlatform less the question of whether its tile may be laid that way round at all.
	 * The candidate lists are kept by this, and a tile's orientation is only asked when a beat
	 * draws from them, so it can be changed during a run and take effect on the next beat.
	 */
	bool IsPlacementOpen(const FGridPlatform& Platform) const;

	/**
	 * Whether a platform lies flush with a neighbour it shares rim with, along the axis of the
	 * line they share: their sides there start or end together, and wherever they do not, the
	 * step between them is at least SmallestTileSide.
	 */
	bool IsFlushWith(const FGridPlatform& Platform, const FGridPlatform& Neighbour, int32 SharedAxis) const;

	/**
	 * Brings the usable tiles and structures, which ways each tile can be filled, and the warnings
	 * about the rest up to date with Tiles, Structures, FirstTile and the module.
	 */
	void ResolveUsableTiles();

	/** The usable structures, from Structures, with what is wrong with the rest added to OutProblems. */
	void ResolveUsableStructures(TArray<FString>& OutProblems);

	/**
	 * Whether the usable structures can fill a platform facing NormalAxis, with this Up and these
	 * spans, completely: one two-sided structure, or a one-sided one on each face. Only load-bearing
	 * ones count when asked.
	 */
	bool HasCompleteFill(int32 NormalAxis, EGridFace Up, int32 SpanFirst, int32 SpanSecond, bool bLoadBearingOnly) const;

	/**
	 * Every platform a placement would share a module edge of rim with, as indices into BuiltPlatforms,
	 * each once and in ascending order.
	 */
	void GatherTouchingPlatforms(const FGridPlatform& Platform, TArray<int32, TInlineAllocator<8>>& OutTouching) const;

	/**
	 * The Up a placement about to be filled takes, from the role stream: that of a platform it touches,
	 * drawn at random, turned where the turns that are on say so (see TurnUp). A way up no structures
	 * can fill gives way to the Up it would have inherited, then to the Up of the other platforms it
	 * touches, then to +Z.
	 */
	EGridFace ChooseUp(const FGridPlatform& Platform);

	/**
	 * The Up a placement growing off Parent takes: Parent's own, flipped, hung or quarter-turned as the
	 * turns that are on say, from the role stream.
	 */
	EGridFace TurnUp(const FGridPlatform& Platform, const FGridPlatform& Parent);

	/** Whether the face of a platform on the side Sign looks out of holds items. A face with no structure does. */
	bool FaceHoldsItems(const FGridPlatform& Platform, int32 Sign) const;

	/**
	 * Whether a floor - a platform lying across Up in its own frame - runs along a wall's edge across its
	 * Up at Coordinate along that Up, on the side Side of the wall's plane.
	 */
	bool HasFloorAlong(const FGridPlatform& Wall, int32 Coordinate, int32 Side) const;

	/**
	 * The chance a drawn placement is drawn again because it goes against a growth rule that is on - the
	 * strongest of them, among the platforms it folds off - or zero when it goes against none.
	 * See bWallsStandOnGrounds and bFloorsCloseRooms.
	 */
	float GetRuleRedrawChance(const FGridPlatform& Candidate) const;

	/**
	 * The ways round, one bit each - as it is, then turned - that a usable tile can be laid as a kind
	 * of platform and still be filled. Placements of it laid any other way are never drawn.
	 */
	uint8 GetFillableWays(int32 TileIndex, EPlatformKind Kind) const;

	/**
	 * The usable structures that may fill one face of a platform. For the first face, Front null:
	 * every two-sided one that fits it, and every one-sided one that fits this face - but those only
	 * when a one-sided structure that pairs with it fits the face behind, an exterior one when
	 * bExteriorBackOnly, so a draw never leaves a face empty. For the face behind Front: the one-sided
	 * structures that fit it and pair with Front.
	 */
	void GatherFillOptions(
		const FGridPlatform& Platform,
		int32 FaceSign,
		const UPlatformStructureData* Front,
		bool bLoadBearingOnly,
		TArray<UPlatformStructureData*>& OutOptions,
		bool bExteriorBackOnly = false) const;

	/**
	 * Draws the structures filling a platform that is about to be placed, from the structure
	 * stream: a face first - a floor's face looking along its Up, a wall's face over an interior ground
	 * where only one is, else either - then a structure for it, and when that is one-sided, one for the
	 * face behind that pairs with it. Interiors and exteriors are drawn where bWallsFaceInteriorGrounds
	 * and bInteriorBacksExterior say so, and a face meeting nature ground is always an exterior. False
	 * when nothing fits, or when the platform would meet water or a cliff.
	 */
	bool DrawFill(FGridPlatform& Platform, bool bLoadBearingOnly);

	/**
	 * Which faces of a wall about to be placed look over an interior ground it stands on, as bits:
	 * 1 for its positive face, 2 for its negative one. Such a ground is a floor's Ground face, filled
	 * with an interior, meeting the wall in a corner around an edge they share and looking at the wall,
	 * so that the wall's face looking back into the corner looks over it. 0 for a floor.
	 */
	uint8 GetFacesOverInteriorGround(const FGridPlatform& Wall) const;

	/**
	 * Draws one of the options for one face of a platform about to be placed, from the structure
	 * stream, pulled toward the categories of the faces beside it (see WEMStructures::DrawWithPull).
	 * Null when there are none.
	 */
	UPlatformStructureData* DrawStructure(TConstArrayView<UPlatformStructureData*> Options, const FGridPlatform& Platform, int32 FaceSign);

	/** DrawStructure from a stream of the caller's: the nature beat fills what it lays from its own. */
	UPlatformStructureData* DrawStructureFrom(FRandomStream& Stream, TConstArrayView<UPlatformStructureData*> Options, const FGridPlatform& Platform, int32 FaceSign);

	/** The structure filling one face of a platform: its positive face for a positive Sign, else its negative one. */
	static UPlatformStructureData* GetFaceStructure(const FGridPlatform& Platform, int32 Sign);

	/** The face of a platform a nature structure fills, and its form. False for a platform of the city alone. */
	static bool GetNatureFace(const FGridPlatform& Platform, int32& OutSign, ENatureForm& OutForm);

	/** Whether a nature structure fills either face of a platform. */
	static bool IsNatureSegment(const FGridPlatform& Platform);

	/**
	 * Whether the nature face of one platform takes the face of another that it meets across a shared beam:
	 * what its form allows, given how the two platforms lie. See Nature in the class comment.
	 */
	static bool AcceptsMeeting(const FGridPlatform& Nature, int32 NatureSign, const FGridPlatform& Other, int32 OtherSign);

	/**
	 * Calls Visit(OwnSign, NeighbourIndex, NeighbourSign) for every pair of faces a platform meets others in:
	 * around every module edge of its rim another platform shares, each face of it meeting the face of
	 * the platform next round the edge, either way. The platform is BuiltPlatforms[PlatformIndex], or the
	 * one about to be placed there.
	 */
	template <typename FunctorType>
	void ForEachMeeting(const FGridPlatform& Platform, int32 PlatformIndex, FunctorType&& Visit) const;

	/**
	 * Whether every face a filled platform about to be placed would meet is one the nature on either side
	 * takes. OutNatureMeets says what its own nature face, if it has one, meets: NatureMeetsCity for a face
	 * of the city, and GetNatureMeetsBit for each form of nature.
	 */
	bool AreNatureMeetingsLegal(const FGridPlatform& Platform, uint8* OutNatureMeets = nullptr) const;

	/** What a nature face meets, as AreNatureMeetingsLegal reports it: a face of the city, or nature of a form. */
	static constexpr uint8 NatureMeetsCity = 1 << 0;

	static uint8 GetNatureMeetsBit(ENatureForm Form);

	/**
	 * Whether a placement not yet filled would meet water or a cliff, which take no face of the city. The
	 * faces of it that would meet ground, and so have to be drawn from exteriors, go into OutFacesOnGround
	 * as FaceBit flags.
	 */
	bool MeetsClosedNature(const FGridPlatform& Platform, uint8* OutFacesOnGround = nullptr) const;

	/**
	 * Fills a platform about to be placed with nature of one form on the face NatureSign looks out from,
	 * and one-sided urban structure behind it - any of the face's type there, an exterior where the back
	 * meets ground - both from Stream. Its Up has to suit the form already: along its normal and looking
	 * out of the nature face for ground and water, in its plane for a cliff. False when that is not so,
	 * when the back would meet water or a cliff, or when nothing fits either face.
	 */
	bool DrawNatureFill(FGridPlatform& Platform, ENatureForm Form, int32 NatureSign, FRandomStream& Stream);

	/** Where a nature structure with no mesh is drawn on one face of a platform: a slab across the opening, from the mid-plane out to the face. */
	FTransform MakeFlatSlabTransform(const FGridPlatform& Platform, int32 FaceSign) const;

	/**
	 * The faces of a placement about to be placed, as FaceBit flags, that would meet nature: those meeting
	 * ground, which only an exterior may fill, and those meeting water or a cliff, which nothing of the city may.
	 */
	void GetNatureNeighbourFaces(const FGridPlatform& Platform, uint8& OutOnGround, uint8& OutOnClosed) const;

	/** How many nature forms there are: ground, cliff and water. */
	static constexpr int32 NatureFormCount = 3;

	/** One placement the nature beat could lay: the platform with its Up, the form, and which face is the nature. */
	struct FNatureCandidate
	{
		FGridPlatform Platform;
		ENatureForm Form{};
		int32 NatureSign = 1;

		/** How many module edges it was found growing off. A field's spill is drawn by it. */
		int32 Weight = 1;
	};

	/** A placement as the nature beat tells them apart: its candidate key, its tile's index in UsableTiles, and its Up and nature face. */
	using FNatureCandidateKey = TTuple<uint64, int32, int32>;

	/** A field of ground or water still spilling out: its form, the pieces it has laid, and how many more it may. */
	struct FNatureSpill
	{
		ENatureForm Form{};
		TArray<FGridPlatform> Pieces;
		int32 Remaining = 0;
	};

	/** The nature beat. It never stops its own timer: the city growing on can always give nature somewhere new. */
	void AdvanceNature();

	/** Lays the first piece of a field of ground, a cliff or a pool of water, among the places sampling finds. False when it finds none. */
	bool PlaceNatureOfForm(ENatureForm Form);

	/**
	 * Every placement of Form that could grow off one module edge of a nature platform's rim, judged with
	 * Starts (see JudgeNatureCandidate), added to Out each once - its weight counting how many such edges it
	 * was found off. Seen holds what has been judged, as an index into Out or INDEX_NONE.
	 */
	void GatherNatureAround(const FGridPlatform& Anchor, const FIntVector& EdgeNode, int32 EdgeAxis, ENatureForm Form, uint8 Starts, TMap<FNatureCandidateKey, int32>& Seen, TArray<FNatureCandidate>& Out);

	/**
	 * Whether a placement, given its Up, could be laid as nature of Form on the face NatureSign looks out
	 * from: its tile may lie that way, nature and something behind it fit, and it is free and legal to
	 * place filled with them - its meetings included - with its nature face meeting one of Starts, as
	 * AreNatureMeetingsLegal reports them: what it may start against, or the field it spills from. On
	 * success it is left filled with stand-ins (see FindNatureFill).
	 */
	bool JudgeNatureCandidate(FGridPlatform& Platform, ENatureForm Form, int32 NatureSign, uint8 Starts);

	/**
	 * Stand-ins for what fills nature of a form on a placement, for judging it: the first nature of the
	 * form that fits, and a back that fits - an exterior where one does, which any nature it meets takes if
	 * it takes any of the city. False when either is missing. Cached by opening, Up and form.
	 */
	bool FindNatureFill(const FGridPlatform& Platform, ENatureForm Form, int32 NatureSign, UPlatformStructureData*& OutFront, UPlatformStructureData*& OutBack);

	/** Fills a candidate from the nature stream and places it. False when no fill can be drawn, or it is no longer legal. */
	bool PlaceNatureCandidate(FNatureCandidate& Candidate);

	/** One box of air kept open, inclusive at both corners. */
	struct FAirBox
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Max = FIntVector::ZeroValue;
	};

	/** Whether a platform is a piece of a field: ground or water, which keep the air above them open. */
	static bool IsFieldPiece(const FGridPlatform& Platform);

	/** How many cells of air a piece of ground or water keeps open, drawn between the storeys from the run's seed and the piece's own key. */
	int32 GetAirHeight(const FGridPlatform& Platform) const;

	/**
	 * The boxes of air a piece of ground or water keeps open, Height cells out along its Up: one over its
	 * opening, and one over each run of seam it shares with a piece of a field laid before it - before
	 * PlatformIndex - in its plane with its Up. Its outline is left open.
	 */
	void GatherAirBoxes(const FGridPlatform& Platform, int32 PlatformIndex, int32 Height, TArray<FAirBox>& OutBoxes) const;

	/** Whether a piece of ground or water about to be placed would have MinAirStoreys of air clear of every platform. */
	bool IsAirClear(const FGridPlatform& Platform) const;

	/** Adds the lattice pieces an inclusive box of cells cuts to the obstacle keys. */
	void AddObstacleKeys(const FIntVector& MinCell, const FIntVector& MaxCell);

	/** Judges again every listed placement whose footprint reaches into a box just held back, dropping those it now cuts. */
	void DropCandidatesCutBy(const FIntVector& MinCell, const FIntVector& MaxCell);

	/** Flushes and redraws the air kept open, in its own batcher. */
	void RebuildAirLines();

	/** The category indices a structure carries, each once, leaving out any no usable structure carries. */
	void GetCategoryIndices(const UPlatformStructureData* Structure, TArray<int32, TInlineAllocator<4>>& OutCategories) const;

	/**
	 * The faces beside one face of a platform, as indices into FaceFills: around every module edge of
	 * its rim another platform shares, the faces it meets walking round the edge. The platform is
	 * BuiltPlatforms[PlatformIndex], or the one about to be placed there.
	 */
	void GatherFaceNeighbours(const FGridPlatform& Platform, int32 PlatformIndex, int32 FaceSign, TArray<int32, TInlineAllocator<16>>& OutNeighbours) const;

	/** Records the fill of a platform just added to BuiltPlatforms: its faces' categories, streaks and patches. */
	void RecordFill(int32 PlatformIndex);

	/** The patch a (face, category) node belongs to, by its root node. */
	int32 FindPatch(int32 Node);

	/** Joins the patches of two nodes. */
	void JoinPatches(int32 First, int32 Second);

	/** Per node, the lowest node in its patch: the same for the same patches, however they were joined. */
	TArray<int32> GetPatchLabels();

	/** Works out the pieces every obstacle cuts afresh from Obstacles. */
	void RebuildObstacleKeys();

	/**
	 * Drops every obstacle and counts a new generation: the platform list has just been replaced or
	 * cleared, and whatever stood on the old structure stood on nothing that is still there.
	 */
	void BeginNewGeneration();

	/**
	 * Rebuilds everything derived from Platforms. Wholesale, so it cannot drift out of step -
	 * which is also what makes it the reference AddPlacedPlatform is checked against.
	 */
	void RebuildPlatformState();

	/**
	 * Takes one newly placed platform into the derived state without rebuilding it: its module
	 * faces, edges and rim, the surfaces it builds against, and the promotions that spread out
	 * from it. Collects every surface whose capacity or occupancy may have changed, for the overlay,
	 * and records them for GatherSurfaceChangesSince.
	 */
	void AddPlacedPlatform(const FGridPlatform& Platform, TArray<FGridSurfaceRef>& OutChangedSurfaces);

	/** Records the surfaces the placement that made the current revision may have changed. */
	void JournalSurfaceChanges(TConstArrayView<FGridSurfaceRef> Surfaces);

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

	/** Whether the structure filling this face of a platform holds nothing, which closes its interior off on that side. */
	bool IsFaceBlocked(const FGridPlatform& Platform, EGridFace Face) const;

	/** Whether any platform covers this cell. */
	bool IsSolidCell(const FIntVector& Cell) const;

	/** Whether every structure filling a platform is load-bearing. One with none is: it was placed before structures were. */
	static bool IsLoadBearing(const FGridPlatform& Platform);

	/** Whether a module edge of rim is built as a beam: more platforms run around it than have blocked it. */
	bool IsEdgeBuilt(uint64 EdgeKey) const;

	/** Whether a lattice node is built as a post: a built beam ends at it. */
	bool IsNodeBuilt(const FIntVector& Node) const;

	/** Whether a cell of rim - on a module edge's run, or a node - is built. */
	bool IsRimCellBuilt(const FIntVector& Cell, int32 LatticePlaneCount) const;

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
	 * Takes a platform, fill and all, into the structure: the list, everything derived, the visuals,
	 * and when it is a fence panel the ends its run may carry on from. JoinedSide is the end of a
	 * panel that carries on a run, which is no end of it; zero for a panel starting one.
	 */
	void PlacePlatform(const FGridPlatform& Platform, int32 JoinedSide);

	/** One end of a fence run waiting to grow: the panel it belongs to, fill and all, and which end of it. */
	struct FFenceEnd
	{
		FGridPlatform Panel;

		/** -1 for the end at the panel's min node along its run, +1 for the far end. */
		int32 Side = 0;
	};

	/** Whether a platform is filled with a fence: a two-sided fence structure on both faces. */
	static bool IsFencePanel(const FGridPlatform& Platform);

	/** The in-plane axis a fence panel's run goes along: across its Up on a wall, along the fence's width across a floor. */
	int32 GetRunAxis(const FGridPlatform& Panel) const;

	/**
	 * Whether Panel carries Previous's run on straight: the same fence, tile and way round, in the
	 * same plane, beside it along the run. Asks nothing but the two platforms, so the rebuild answers
	 * it as the placement did.
	 */
	bool IsRunContinuation(const FGridPlatform& Panel, const FGridPlatform& Previous) const;

	/**
	 * Where a run could carry on from one end: straight on, and for a wall round the beam at the end
	 * either way. Each is filled with the run's fence.
	 */
	void GatherFenceContinuations(const FFenceEnd& End, FGridPlatform& OutStraight, TArray<FGridPlatform, TInlineAllocator<2>>& OutTurns) const;

	/**
	 * Whether a run has met itself at an end: one of the end's continuations already stands, a panel
	 * of the same fence laid the same way. Such an end is closed rather than carried on elsewhere.
	 */
	bool IsFenceEndClosed(const FGridPlatform& Straight, TConstArrayView<FGridPlatform> Turns) const;

	/** The end of a continuation of End that meets End: the side of it lying on End's beam. */
	int32 GetJoinedSide(const FFenceEnd& End, const FGridPlatform& Continuation) const;

	/**
	 * Whether a platform stands on something: a wall when every module edge along its foot, or every
	 * one along its top, is used already - hanging counts. Its foot and top run across its Up. Floors
	 * and ceilings always do.
	 */
	bool StandsOnSomething(const FGridPlatform& Platform) const;

	/**
	 * For each end of a newly placed fence panel with room left beyond it, draws whether its run
	 * carries on, and queues the end if so.
	 */
	void QueueFenceEnds(const FGridPlatform& Panel, int32 JoinedSide);

	/** A fence beat: places the next panel of the oldest waiting end that still has somewhere to go. False when none has. */
	bool PlaceFencePanel();

	/**
	 * Whether a platform a normal beat is about to place lands on a waiting end's continuation, laid
	 * as its run is. If so it carries the run on: it is filled with the fence and the end is taken
	 * off the queue.
	 */
	bool TakeWaitingFenceSpot(FGridPlatform& Platform, int32& OutJoinedSide);

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

	/**
	 * The pieces of the lattice the obstacles cut, keyed as ModuleFaceOwners, ModuleEdgeUses and
	 * the node cells are: module faces by min node and normal, module edges by node and axis,
	 * nodes by cell. A placement taking in any of them is refused.
	 */
	TSet<uint64> ObstacleFaces;
	TSet<uint64> ObstacleEdges;
	TSet<uint64> ObstacleNodes;

	/**
	 * How many platforms run around each module edge as rim, and the edges a platform that is not
	 * load-bearing has blocked, each with the first platform to block it - an index into
	 * BuiltPlatforms. A beam is built where more platforms use it than block it.
	 */
	TMap<uint64, int32> RimEdgeUsers;
	TMap<uint64, int32> BlockedEdges;

	/** The handle the next obstacle gets. Never reused within a run, so a stale handle removes nothing. */
	int32 NextObstacleHandle = 1;

	int32 StructureRevision = 0;
	int32 RebuildGeneration = 0;

	/**
	 * The surfaces each recent placement may have changed, run after run, oldest first, and where
	 * each run starts: run I is revision SurfaceJournalRevision + I + 1's. Only the last few
	 * thousand placements are kept, and a rebuild starts the record over.
	 */
	TArray<FGridSurfaceRef> SurfaceJournal;
	TArray<int32> SurfaceJournalStarts;
	int32 SurfaceJournalRevision = 0;

	/** The tiles growth draws from: Tiles, less empty entries, repeats, and any that do not fit the lattice. */
	TArray<TObjectPtr<UPlatformTileData>> UsableTiles;

	/**
	 * The structures growth fills platforms from: Structures, less empty entries, repeats, any with
	 * no mesh, no type or no weight, fences that are not two-sided, and nature.
	 */
	TArray<TObjectPtr<UPlatformStructureData>> UsableStructures;

	/** The nature among Structures, less empty entries, repeats and any with no weight. A mesh is not asked for. */
	TArray<TObjectPtr<UPlatformStructureData>> UsableNatureStructures;

	/** How many of BuiltPlatforms have nature on a face. Until there are any, nothing has to ask what a placement meets. */
	int32 NaturePlatformCount = 0;

	/** The platforms of each nature form, by ENatureForm, as indices into BuiltPlatforms in the order they were taken in. */
	TArray<int32> NatureIndices[NatureFormCount];

	/** Fields of ground and water still spilling out, oldest first. Beat state, like FenceQueue. */
	TArray<FNatureSpill> SpillQueue;

	/** Stand-ins for nature's fills (see FindNatureFill), by opening, Up and form. Cleared whenever the structures are resolved. */
	struct FNatureFillStandIns
	{
		UPlatformStructureData* Front = nullptr;
		UPlatformStructureData* Back = nullptr;
	};

	TMap<uint64, FNatureFillStandIns> NatureFillCache;

	/** The air every piece of ground and water keeps open, in the order the pieces were taken in. Its pieces are among the obstacle keys. */
	TArray<FAirBox> AirBoxes;

	/** The seed the streams were last started from, which the heights of the air are drawn from too. */
	int32 RunSeed = 0;

	/** Per usable tile and kind, in that order, the ways round it can be filled (see GetFillableWays). */
	TArray<uint8> FillableWays;

	/**
	 * The categories the usable structures carry, each with a dense index of its own, and by index.
	 * Numbered afresh by every rebuild; the categories are held by the structures.
	 */
	TMap<const UStructureCategory*, int32> CategoryIndices;
	TArray<const UStructureCategory*> CategoriesByIndex;

	/** One face's fill, as the category pull sees it. */
	struct FFaceFill
	{
		const UPlatformStructureData* Structure = nullptr;

		/** The category indices its structure carries, each once, and parallel to them its streak and patch node in each. */
		TArray<int32, TInlineAllocator<4>> Categories;
		TArray<int32, TInlineAllocator<4>> Streaks;
		TArray<int32, TInlineAllocator<4>> PatchNodes;
	};

	/** Two per platform in BuiltPlatforms, its positive face and then its negative one. */
	TArray<FFaceFill> FaceFills;

	/**
	 * The category patches as a union-find over (face, category) nodes: each node's parent, each
	 * root's size, and the sum of every patch's size squared, from which CategoryPatchSize follows.
	 */
	TArray<int32> PatchParents;
	TArray<int32> PatchSizes;
	int64 PatchSizeSquares = 0;

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

	/** Module edges of rim, the beams. Starts from EdgePieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer EdgeLayer;

	/** Lattice nodes of rim, the posts. Starts from NodePieces. */
	UPROPERTY(Transient, DuplicateTransient)
	FRoomPieceLayer NodeLayer;

	/** The structures filling the openings, a layer per structure. Made at runtime and never saved. */
	UPROPERTY(Transient, DuplicateTransient)
	TArray<FStructureLayer> StructureLayers;

	FRandomStream PlacementStream;

	/**
	 * What fills each platform is drawn from a stream of its own, so the platforms themselves are
	 * drawn as they would be with no structures at all.
	 */
	FRandomStream StructureStream;

	/** Which way up each platform is, likewise from a stream of its own. */
	FRandomStream RoleStream;

	/** What the nature beat draws, where and with what, from a stream of its own. */
	FRandomStream NatureStream;

	FTimerHandle NatureTimerHandle;

	/**
	 * Fence ends waiting to grow, oldest first. Beat state, like the streams: never saved, kept by a
	 * rebuild, emptied by ClearPlatforms. Each is judged again when it is used.
	 */
	TArray<FFenceEnd> FenceQueue;

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
	 * One instance per module edge of rim, spanning its run. Shared edges are built once. A
	 * platform's rim is drawn apart from what fills it, as edges and nodes, because neighbours
	 * share it. The structures' layers are set up like this one.
	 */
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

	/** Draws the air kept open. A batcher of its own, since it is redrawn as nature grows and the lattice is not. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<ULineBatchComponent> AirLineBatcher;
};
