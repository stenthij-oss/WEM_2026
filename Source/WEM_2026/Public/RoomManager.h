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

	/**
	 * On a nature platform, the city structure drawn across its nature face from the nature's cover categories (see
	 * UPlatformStructureData::CoverCategories), in place of its mesh or flat colour. The nature is still what the
	 * face is to growth; the cover is how it looks and the categories it offers to the faces beside it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Platform")
	TObjectPtr<UPlatformStructureData> NatureCover;

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

/** Which ways ground and water may face in the world. Cliffs stand across whatever they face. */
UENUM(BlueprintType)
enum class ENatureFacing : uint8
{
	/** Lying flat, looking up at the sky, so cliffs stand upright off their edges. */
	Up UMETA(DisplayName = "Flat, Looking Up"),

	/** Lying flat, looking up or hanging face down under an overhang. */
	Flat UMETA(DisplayName = "Flat, Up Or Down"),

	/** Any way the city's segments face: flat, standing on a facade, or hanging. */
	AnyWay UMETA(DisplayName = "Any Way, Following Up")
};

/** The shapes an air pocket can take. */
UENUM(BlueprintType)
enum class EAirPocketShape : uint8
{
	Sphere,

	/** A cube: all three sides the same. */
	Box,

	/** A box whose three sides are drawn apart. */
	Rectangular,

	/** A stepped pyramid, its tiers narrowing toward its top. */
	Ziggurat,

	Cylinder
};

/** The shapes a mold leaves open inside its box, for growth to fill. */
UENUM(BlueprintType)
enum class EMoldShape : uint8
{
	/** A stepped pyramid, its tiers narrowing toward its top. */
	Ziggurat,

	Cylinder,

	/** Half a sphere, standing on its flat side. */
	Dome,

	/** A square tower. */
	Tower,

	/** Two or three of the others stacked, each no wider than the one below, a dome only ever on top. */
	Combination
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

	/** Where each piece drawn so far stands - its chunk, and its instance there - so it can be taken out again. */
	TMap<uint64, FIntPoint> PieceSlots;

	/** Per chunk, the key of the piece each instance stands for, in instance order. */
	TArray<TArray<uint64>> ChunkKeys;

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
 * its platforms from, or nature - ground, cliff and water - which only the nature beat lays, a piece a
 * beat on a timer of its own. A nature platform is one-sided, nature on one face looking into the open
 * air and any urban ceiling or wall behind it, and meets across its beams only what its form allows. The
 * city meets ground only with exteriors, and water or a cliff never. Ground and water keep the air above
 * them open. The whole of it is told by the nature code further down.
 *
 * Air pockets
 * -----------
 * Whenever the cube is cleared, AirPocketCount pockets of air - spheres, cubes, rectangular boxes,
 * ziggurats and cylinders - are drawn at random across it, and nothing is ever built in them. Before
 * them come MoldCount molds: boxes held back but for a shape left open inside, which growth can fill.
 *
 * Zones and deletion
 * ------------------
 * The cube is split into 27 zones, three by three by three, each with a first tile of its own.
 * Every platform has health; strikes wear a share of them down, and one with none left is taken
 * away. A zone is reshuffled now and then, and pockets and molds move during play. The whole of it
 * is told by the deletion code further down.
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

	/**
	 * The most beats one frame may run when the interval is shorter than a frame. Each beat still places one platform,
	 * but more of them arrive per frame: at 60 frames a second, 1 tops growth out at 60 platforms a second however short
	 * the interval, 8 at 480. Beats owed beyond this are dropped rather than made up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxBeatsPerFrame = 1;

	/**
	 * Milliseconds of a frame the beats may take between them before the rest owed that frame are dropped, so a frame
	 * already slow is not made slower. The first beat of a frame always runs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Platforms", meta = (ClampMin = "0.0"))
	float BeatBudgetMilliseconds = 8.0f;

	/** Platforms placed in the last second of play. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Platforms")
	int32 PlacementsPerSecond = 0;

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
	 * Which ways ground and water may face in the world: lying flat and looking up, by default, so a landscape
	 * lies like one and its cliffs stand upright. Takes effect on the next nature beat.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Nature")
	ENatureFacing NatureFacing = ENatureFacing::Up;

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
	 * The fewest storeys of air kept open above every piece of ground and water, out along its Up: what every piece
	 * starts with, before its landscape grows. A piece is only laid where this much is clear of every platform. Zero
	 * keeps no air.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0"))
	int32 MinAirStoreys = 1;

	/** The most storeys of air a piece keeps open to start with. Each piece keeps a height of its own, drawn between the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0"))
	int32 MaxAirStoreys = 1;

	/** Draws the air kept open above ground and water as boxes of lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air")
	bool bShowAir = true;

	/** Colour of the air kept open above ground and water. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air")
	FColor AirColor = FColor(255, 190, 40);

	/**
	 * Lets the air above a landscape - ground and water joined across their seams in one plane - grow like a tree as the
	 * landscape does. Every piece keeps its own storey of air, the trunk, as wide as the landscape. As pieces join, a crown
	 * grows above the trunk - never in the first storey - that rises a storey at a time and reaches out past the
	 * landscape's edges.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air")
	bool bMushroomAir = true;

	/** Storeys the crown rises above the trunk for each piece the landscape has beyond its first, rounded down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0.0", EditCondition = "bMushroomAir"))
	float MushroomStoreysPerPiece = 0.25f;

	/** The most storeys the crown rises, however large the landscape gets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0", EditCondition = "bMushroomAir"))
	int32 MaxMushroomStoreys = 4;

	/** Cells the crown reaches out past the landscape's edges for each piece it has beyond its first, in whole modules. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0.0", EditCondition = "bMushroomAir"))
	float MushroomSpreadPerPiece = 2.0f;

	/** The furthest the crown reaches out past the landscape's edges, in cells. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air", meta = (ClampMin = "0", EditCondition = "bMushroomAir"))
	int32 MaxMushroomSpread = 48;

	/** The landscapes standing and the air over the largest. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Air")
	FString AirSummary;

	/** How many pockets of air are drawn across the cube each time it is cleared. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "0"))
	int32 AirPocketCount = 6;

	/** The smallest a pocket is across, in cells: a sphere's or cylinder's diameter, a box's side, a ziggurat's base. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "1"))
	int32 MinAirPocketSize = 24;

	/** The largest a pocket is across, in cells. Each of its measures is drawn evenly between the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "1"))
	int32 MaxAirPocketSize = 64;

	/** How often a pocket is a sphere, relative to the other shapes. Zero never. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "0.0"))
	float SphereWeight = 1.0f;

	/** How often a pocket is a cube. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "0.0"))
	float BoxWeight = 1.0f;

	/** How often a pocket is a rectangular box, its three sides drawn apart. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "0.0"))
	float RectangularWeight = 1.0f;

	/** How often a pocket is a ziggurat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "0.0"))
	float ZigguratWeight = 1.0f;

	/** How often a pocket is a cylinder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "0.0"))
	float CylinderWeight = 1.0f;

	/** How many tiers a ziggurat steps up in, each narrower than the one below by an even step. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "1"))
	int32 ZigguratTiers = 4;

	/** Lets a ziggurat or a cylinder stand along any axis, either way, rather than upright in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	bool bAirPocketsAlongAnyAxis = true;

	/**
	 * Lets every pocket and mold break down during play and form again somewhere else, one at a time, the same shape.
	 * The air kept above ground and water stays where it is.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	bool bMovePockets = true;

	/** Seconds by which half the pockets and molds drawn at one time have broken down. Each draws how long it lasts from this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "1.0", EditCondition = "bMovePockets"))
	float PocketHalfLife = 120.0f;

	/** The shortest a pocket or mold lasts in one place, in seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets", meta = (ClampMin = "0.0", EditCondition = "bMovePockets"))
	float MinPocketLife = 30.0f;

	/** Draws each pocket's shape in lines of its shape's colour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	bool bShowAirPockets = true;

	/** Draws a point, in its pocket's colour, at every lattice piece a pocket holds back: what growth can actually no longer use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	bool bShowAirPocketPieces = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	FColor SphereColor = FColor(0, 200, 255);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	FColor BoxColor = FColor(255, 0, 200);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	FColor RectangularColor = FColor(255, 110, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	FColor ZigguratColor = FColor(140, 255, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Air Pockets")
	FColor CylinderColor = FColor(120, 80, 255);

	/** The pockets drawn the last time the cube was cleared, by shape, and how many were left out. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Air Pockets")
	FString AirPocketSummary;

	/**
	 * How many molds are drawn across the cube each time it is cleared, before the pockets: boxes of air with a
	 * shape left open inside, standing on the box's floor, that growth can only fill from below.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0"))
	int32 MoldCount = 2;

	/** The smallest a mold's box is along each side, in cells. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "4"))
	int32 MinMoldSize = 64;

	/** The largest a mold's box is along each side, in cells. Each side is drawn evenly between the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "4"))
	int32 MaxMoldSize = 128;

	/** The least of its box the open shape fills, across and in height, as a fraction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float MinMoldFill = 0.5f;

	/** The most of its box the open shape fills. Its width and its height are each drawn evenly between the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float MaxMoldFill = 0.9f;

	/** How often a mold's open shape is a ziggurat, relative to the others. Zero never. Its tiers are ZigguratTiers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.0"))
	float MoldZigguratWeight = 1.0f;

	/** How often it is a cylinder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.0"))
	float MoldCylinderWeight = 1.0f;

	/** How often it is a dome. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.0"))
	float MoldDomeWeight = 1.0f;

	/** How often it is a square tower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.0"))
	float MoldTowerWeight = 1.0f;

	/** How often it is two or three shapes stacked, such as a tower on a ziggurat crowned by a dome. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.0"))
	float MoldCombinationWeight = 1.0f;

	/**
	 * How far from the cube's centre - where growth starts - a mold may be drawn, as a part of the cube: its centre
	 * lies within this much of the cube's middle along each axis. Small keeps molds where growth soon reaches them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MoldSpread = 0.35f;

	/**
	 * Cells at the bottom of every mold's box left open all the way across, for growth to spread in under the shape
	 * from any side; the shape stands on top of it. One module is room for floors to slide in under the shape, and
	 * walls standing on them rise into it; deeper, more of the growth stays spread out in the floor instead. Zero
	 * leaves only the shape open, reached from below its foot.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "0"))
	int32 MoldOpenFloor = 4;

	/** How much likelier a beat draws a placement whose middle lies in a mold's box than one anywhere else. One draws them alike. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds", meta = (ClampMin = "1.0"))
	float MoldGrowthWeight = 4.0f;

	/** Lets a mold stand along any axis, either way, rather than upright in the world, its floor at the bottom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds")
	bool bMoldsAlongAnyAxis = false;

	/** Draws each mold's box and the shape open inside it in lines, the box in MoldColor and the shape in MoldShapeColor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds")
	bool bShowMolds = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds")
	FColor MoldColor = FColor(230, 230, 230);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Molds")
	FColor MoldShapeColor = FColor(255, 40, 40);

	/** The molds drawn the last time the cube was cleared, by shape, and how many were left out. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Molds")
	FString MoldSummary;

	/**
	 * Turns the paths on or off. Off, none are drawn and none move, so their redraws cost nothing; platforms they
	 * held back may be placed again. Takes effect at once, in play too.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths")
	bool bEnablePaths = true;

	/**
	 * How many paths of air run through the cube: thin tubes, each a few straight legs turning at corners, which no
	 * new platform may be placed across. They keep doorways open, so the cube is never closed off. Platforms already
	 * standing in a path stay. Zero draws none.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "0"))
	int32 PathCount = 12;

	/** The fewest straight legs a path runs in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "1"))
	int32 MinPathLegs = 2;

	/** The most straight legs a path runs in. Each path draws its number evenly between the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "1"))
	int32 MaxPathLegs = 5;

	/** The shortest a leg runs, in cells. Legs are whole modules long, so corners meet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "1"))
	int32 MinPathLegLength = 16;

	/** The longest a leg runs, in cells. Each leg draws its length evenly between the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "1"))
	int32 MaxPathLegLength = 64;

	/**
	 * How thick a path is, in cells, across both sides. A path runs down the middle of the modules, so while it is
	 * thinner than a module it only stops platforms laid across it - walls, floors and ceilings it would pass
	 * through - and leaves those along its sides alone: a line of doorways. A module thick or more, it reaches
	 * the lattice planes beside it and holds back those as well: a corridor of open air.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "1"))
	int32 PathWidth = 2;

	/** How often a leg runs up or down rather than level. Up and down, a path keeps holes open in floors and ceilings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PathVerticalChance = 0.2f;

	/**
	 * Seconds each path stays where it is during play before it is drawn afresh somewhere new. The paths fall due one
	 * after another, spread across this, so only a path or two moves in any one frame. Zero keeps them where they are.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "0.0"))
	float PathRerollInterval = 30.0f;

	/** How far each path's wait may stray from the interval, as a part of it, either way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PathRerollJitter = 0.2f;

	/** Draws each leg of every path as a thin box of lines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths")
	bool bShowPaths = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Paths")
	FColor PathColor = FColor(120, 230, 255);

	/** The paths drawn last: how many, their legs, the lattice pieces they hold back, and the platforms standing in them. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Paths")
	FString PathSummary;

	/** Draws the 27 zones as boxes of lines, each in its own colour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Zones")
	bool bShowZones = false;

	/**
	 * The zones' colours, in zone order: three along X, then the next three along Y, then the next nine a layer up. The
	 * fourteenth is the middle zone. By default the same nine hues on every layer, darker below and paler above.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Zones")
	TArray<FColor> ZoneColors;

	/**
	 * Gives every zone a first tile of its own, growth starting from all 27: the middle zone's at the heart of the cube
	 * as before, each other one at a random place in its zone, in step with the middle one so their growth lines up when
	 * it meets. Off, growth starts from the middle one alone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Zones")
	bool bSeedEveryZone = true;

	/**
	 * How many first tiles each zone gets, the middle zone's at the heart of the cube among them. They are laid a round
	 * at a time - one in every zone, then a second in every zone - each clear of everything standing, so none touches
	 * another. A zone with no free place left goes without the rest.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Zones", meta = (ClampMin = "1", ClampMax = "32", EditCondition = "bSeedEveryZone"))
	int32 SeedsPerZone = 1;

	/** Platforms standing in each zone, in zone order, a layer at a time from the bottom, as the last strike or rebuild counted them. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Zones")
	FString ZoneSummary;

	/** Strikes wear the platforms down during play, and a platform with no health left is taken away. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion")
	bool bEnableDamage = true;

	/** The least health a platform is laid with. Each draws its own evenly up to MaxHealth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "1.0"))
	float MinHealth = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "1.0"))
	float MaxHealth = 120.0f;

	/** The shortest wait between two strikes, in seconds. Each wait is drawn evenly up to MaxStrikeInterval. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0.05"))
	float MinStrikeInterval = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0.05"))
	float MaxStrikeInterval = 3.0f;

	/** The chance each platform standing is hit by a strike: about this share of them, each strike. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StrikeFraction = 0.1f;

	/** The least damage a hit does. Each hit draws its own evenly up to MaxDamage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0.0"))
	float MinDamage = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0.0"))
	float MaxDamage = 20.0f;

	/**
	 * What a hit does to a platform crossing held-back air - a pocket, a mold's held-back box, a path, or the air over a
	 * landscape - as a multiple of what it would do anywhere else: 1.6 is 60% more.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "1.0"))
	float VulnerableDamageMultiplier = 1.6f;

	/**
	 * Now and then, in a zone drawn at random, a number of platforms holding no objects are taken away one by one, and
	 * as each goes another is laid somewhere else in the same zone: the zone reshuffled a little.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion")
	bool bEnableReshuffle = true;

	/** Seconds between reshuffles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0.1", EditCondition = "bEnableReshuffle"))
	float ReshuffleInterval = 10.0f;

	/** The fewest platforms a reshuffle moves. Each draws its number evenly up to MaxReshuffleCount. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0", EditCondition = "bEnableReshuffle"))
	int32 MinReshuffleCount = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0", EditCondition = "bEnableReshuffle"))
	int32 MaxReshuffleCount = 12;

	/** Seconds between one platform of a reshuffle being moved and the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Deletion", meta = (ClampMin = "0.05", EditCondition = "bEnableReshuffle"))
	float ReshuffleStepInterval = 0.25f;

	/** Strikes, hits and what they took away, reshuffles, and pockets moved, since the cube was cleared. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Grid|Deletion")
	FString DeletionSummary;

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

	/** Draws every path afresh in new places at once. Play moves them on its own, one at a time, each every PathRerollInterval. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Paths")
	void RerollPaths();

	/**
	 * One strike, and whatever it leaves with no health taken away at once. Play strikes on its own every MinStrikeInterval
	 * to MaxStrikeInterval seconds, and takes the platforms away a few at a time.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Deletion")
	void StrikeSegments();

	/** Reshuffles a zone drawn at random, all at once. Play reshuffles on its own every ReshuffleInterval, a platform at a time. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Deletion")
	void ReshuffleZone();

	/** Breaks every pocket and mold down at once and forms each again somewhere else. Play moves them on its own, one at a time. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Grid|Air Pockets")
	void MoveAirPockets();

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
	void AddPiecesToLayer(FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk, TConstArrayView<FPlacedPiece> NewPieces);

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
	 * CanPlacePlatform that asks only what is where, which the rebuild holds the list to as well - all
	 * but the blocked edges (bIgnoreBlockedEdges), which are a rule of growth rather than room taken.
	 */
	bool IsSpaceFree(const FGridPlatform& Platform, bool bIgnoreBlockedEdges = false) const;

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

	/*
	 * Nature
	 * ------
	 * Every structure belongs to a section (UPlatformStructureData::Section): the city, which growth fills
	 * its platforms from, or nature - ground, cliff and water - which growth never draws. A nature platform
	 * is one-sided: its nature face looks into the open air, and behind it stands any one-sided urban
	 * structure of the face's type, a ceiling under ground and water and a wall behind a cliff. Ground and
	 * water lie along their Up, their nature face looking along it; a cliff is a wall. Which ways they may
	 * face in the world NatureFacing says - by default only flat, looking up at the sky, so cliffs stand
	 * upright off their edges. A nature platform is always load-bearing, whatever stands behind it. Its
	 * nature face is covered by a city structure from the categories the nature names (CoverCategories) -
	 * a ground on ground and water, a wall on a cliff - kept as the platform's NatureCover, drawn with the
	 * category pull and offering its categories to the faces beside it. To growth the face is still nature.
	 *
	 * Which faces meet across a shared beam is the walk the category pull takes (see Structures in the class
	 * comment), and a nature face meets only what its form allows: ground meets more ground or water in its
	 * own plane, an exterior - one of both sides counts - and a cliff along the cliff's top or foot, the edges across its
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
	 * Air pockets
	 * -----------
	 * Whenever the cube is cleared - at the start of play among other times - AirPocketCount pockets of air
	 * are drawn at random across it, from a stream of their own: spheres, cubes, rectangular boxes,
	 * ziggurats and cylinders, by their weights, between MinAirPocketSize and MaxAirPocketSize across, a
	 * ziggurat or a cylinder standing along any axis either way unless bAirPocketsAlongAnyAxis is off.
	 * Each holds back every lattice piece it reaches into - the inside of a module face, the run of a
	 * module edge, a node - as an obstacle does, so nothing is ever built there. None is drawn where it
	 * would take the first platform's place; one that cannot be moved clear of it is left out. They stay
	 * as they were drawn until the cube is cleared again, however the settings change in between, since
	 * the platforms standing grew round them.
	 *
	 * Molds
	 * -----
	 * Before the pockets, MoldCount molds are drawn the same way, from a stream of their own: rectangular boxes,
	 * each side between MinMoldSize and MaxMoldSize, upright unless bMoldsAlongAnyAxis is on. A mold holds back
	 * everything in its box but a shape left open inside it - a ziggurat, a cylinder, a dome, a square tower, or
	 * two or three of them stacked - which fills between MinMoldFill and MaxMoldFill of the box across and in
	 * height above an open layer, MoldOpenFloor cells deep, across the bottom of the box: growth spreads in under
	 * the shape from any side, and rises from there into the shape, inside which it grows as anywhere: whatever
	 * forms above the layer, forms in the shape. Molds are kept clear of
	 * each other, and pockets clear of molds - pockets may still overlap pockets.
	 *
	 * Paths
	 * -----
	 * PathCount paths of air run through the cube, dispersed all over it, from a stream of their own: each a
	 * tube PathWidth cells thick down the middle of the modules, in MinPathLegs to MaxPathLegs straight legs
	 * of whole modules, turning at every corner, a leg running up or down with PathVerticalChance. A path
	 * keeps doorways open: no new platform - growth, a fence panel or nature - is placed across a lattice
	 * piece it reaches into. Unlike a pocket it holds nothing back that already stands, and nor does the
	 * rebuild ask about it, so a path may be drawn anywhere at any time, through platforms or not. During play
	 * each path is drawn afresh somewhere new every PathRerollInterval, give or take PathRerollJitter, the paths
	 * falling due one after another across the interval rather than all at once, and the placements a path no
	 * longer holds back come free. The run's seed, the path's place in the list and its count of redraws decide
	 * where it runs, so a change to their settings redraws them on the same routes, thicker or longer, for
	 * calibrating by eye.
	 */

	/** The structure filling one face of a platform: its positive face for a positive Sign, else its negative one. */
	static UPlatformStructureData* GetFaceStructure(const FGridPlatform& Platform, int32 Sign);

	/** The face of a platform a nature structure fills, and its form. False for a platform of the city alone. */
	static bool GetNatureFace(const FGridPlatform& Platform, int32& OutSign, ENatureForm& OutForm);

	/** Whether a nature structure fills either face of a platform. */
	static bool IsNatureSegment(const FGridPlatform& Platform);

	/**
	 * Whether the nature face of one platform takes the face of another that it meets across a shared beam:
	 * what its form allows, given how the two platforms lie. See Nature above.
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
	 * meets ground - both from Stream, and then the city structure covering the nature face, where the
	 * nature names cover categories and one fits. Its Up has to suit the form already: along its normal and looking
	 * out of the nature face for ground and water, in its plane for a cliff. False when that is not so,
	 * when the back would meet water or a cliff, or when nothing fits either face. A missing cover is no failure.
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
	 * Whether nature laid on a placement with its Up faces a way NatureFacing allows. Ground and water look out
	 * along their Up and a cliff stands with the Up of what it grows off, so the Up answers for all three: a
	 * cliff stands upright where they look up, and along the same axis where they lie flat either way.
	 */
	bool AllowsNatureFacing(const FGridPlatform& Platform) const;

	/**
	 * Whether a placement, given its Up, could be laid as nature of Form on the face NatureSign looks out
	 * from: it faces a way NatureFacing allows, its tile may lie that way, nature and something behind it fit, and it is free and legal to
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

	/** One box of air kept open, inclusive at both corners. Also any box of cells, such as a region whose placements are judged again. */
	struct FAirBox
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Max = FIntVector::ZeroValue;

		bool operator==(const FAirBox& Other) const
		{
			return Min == Other.Min && Max == Other.Max;
		}

		bool Intersects(const FAirBox& Other) const
		{
			return Min.X <= Other.Max.X && Other.Min.X <= Max.X
				&& Min.Y <= Other.Max.Y && Other.Min.Y <= Max.Y
				&& Min.Z <= Other.Max.Z && Other.Min.Z <= Max.Z;
		}

		friend uint32 GetTypeHash(const FAirBox& Box)
		{
			return HashCombineFast(GetTypeHash(Box.Min), GetTypeHash(Box.Max));
		}
	};

	/** Whether a platform is a piece of a field: ground or water, which keep the air above them open. */
	static bool IsFieldPiece(const FGridPlatform& Platform);

	/** How many cells of air a piece of ground or water keeps open, drawn between the storeys from the run's seed and the piece's own key. */
	int32 GetAirHeight(const FGridPlatform& Platform) const;

	/**
	 * The boxes of air a piece of ground or water keeps open, Height cells out along its Up: one over its
	 * opening, and one over each run of seam it shares with a piece of a field laid before it - before
	 * PlatformIndex - in its plane with its Up, as high as the lower of the two. Its outline is left open.
	 */
	void GatherAirBoxes(const FGridPlatform& Platform, int32 PlatformIndex, int32 Height, TArray<FAirBox>& OutBoxes) const;

	/** Whether a piece of ground or water about to be placed would have MinAirStoreys of air clear of every platform. */
	bool IsAirClear(const FGridPlatform& Platform) const;

	/** Adds the lattice pieces an inclusive box of cells cuts to the obstacle keys. */
	void AddObstacleKeys(const FIntVector& MinCell, const FIntVector& MaxCell);

	/**
	 * Every box of air the fields standing keep open, worked out afresh: per piece in list order its boxes - its trunk (see
	 * GatherAirBoxes) - then the crown over each landscape that has grown one. A landscape is the pieces joined across
	 * their seams in one plane with one Up.
	 */
	void GatherFieldAir(TArray<FAirBox>& OutBoxes, int32* OutLandscapes = nullptr, int32* OutLargest = nullptr) const;

	/** How many storeys a landscape of Pieces pieces grows its crown, and how many cells the crown reaches past its edges. */
	int32 GetMushroomStoreys(int32 Pieces) const;
	int32 GetMushroomSpread(int32 Pieces) const;

	/**
	 * Brings the air the fields keep open in line with the fields standing: works it out afresh, swaps in only the boxes
	 * that changed, and - with bRefreshLists - judges again the placements those reach into or have let go of.
	 */
	void UpdateFieldAir(bool bRefreshLists);

	/** Adds a box of air to the buckets it reaches, or takes it out of them. */
	void AddAirBoxToBuckets(const FAirBox& Box);
	void RemoveAirBoxFromBuckets(const FAirBox& Box);

	/** The box of cells a platform covers, rim to rim, one cell thick. */
	FAirBox GetPlatformBox(const FGridPlatform& Platform) const;

	/** Whether a platform keeps clear of every box of air the fields keep open. A platform cuts a box exactly where one of its cells lies in it. */
	bool IsClearOfAirBoxes(const FGridPlatform& Platform) const;

	/** Judges again every listed placement reaching into one of Regions, dropping those no longer open. */
	void DropCandidatesIn(TConstArrayView<FAirBox> Regions);

	/**
	 * Judges every placement reaching into one of Regions that shares a module edge with a platform standing, listed or
	 * not, so those the regions have let go of come free. Found from the platforms near the regions, not by walking them.
	 */
	void FreeCandidatesIn(TConstArrayView<FAirBox> Regions);

	/** Flushes and redraws the air kept open and the air pockets, in their own batcher. */
	void RebuildAirLines();

	/** One pocket of air drawn across the cube, in cells, and the lattice pieces it holds back. */
	struct FAirPocket
	{
		EAirPocketShape Shape = EAirPocketShape::Sphere;

		/** Its centre. A cell's own centre is its coordinate plus a half along each axis. */
		FVector Centre = FVector::ZeroVector;

		/** Half its measure across its axis, along the first and second axes after it, and half its measure along it. */
		FVector HalfSize = FVector::ZeroVector;

		/** The axis a ziggurat or cylinder stands along, and which way a ziggurat's top looks along it. */
		int32 Axis = 2;
		int32 AxisSign = 1;

		/** A mold: a rectangular box held back but for the shapes open inside it, standing on its floor - the face its top looks away from. */
		bool bMold = false;

		/** One shape open inside a mold, from Start to End above its floor, Half its half-width; stacked from the floor up. */
		struct FMoldPart
		{
			EMoldShape Shape = EMoldShape::Tower;
			double Start = 0.0;
			double End = 0.0;
			double Half = 0.0;
		};

		TArray<FMoldPart> Parts;

		/** What a mold was drawn as: one of the shapes, or Combination. */
		EMoldShape MoldShape = EMoldShape::Tower;

		/** How many cells at the bottom of a mold's box are open all the way across; its shapes start above them. */
		double OpenFloor = 0.0;

		/** How many times it has formed again since the cube was cleared. With the run's seed and its index, where it goes. */
		int32 Draw = 0;

		/** When, in the world's time, play has it break down and form again somewhere else. */
		double MoveAt = 0.0;

		/**
		 * The pieces it cuts, keyed as the obstacle keys are, slab by slab from its bottom up - a slab being one layer of
		 * the lattice, a module deep along Z - and counted into the pocket keys. Where each slab's pieces end in the
		 * three lists: as many entries as slabs are cut and held, so a pocket can break down and form a slab at a time.
		 */
		TArray<uint64> FaceKeys;
		TArray<uint64> EdgeKeys;
		TArray<uint64> NodeKeys;
		TArray<FIntVector> SlabEnds;
	};

	/**
	 * Draws MoldCount molds and then AirPocketCount pockets afresh from the run's seed, each clear of the first
	 * platform's place, the molds clear of each other and the pockets clear of the molds, counts their pieces into the
	 * pocket keys, sets when each breaks down, and summarises them.
	 */
	void SpawnAirPockets();

	/**
	 * Draws a place for a pocket or mold from Stream, its centre within Spread of the cube's middle - all of it at 1 -
	 * again while it would meet a mold other than the one at Ignore or, with bKeepFirstPlace, take the first platform's
	 * place. False when every place drawn does.
	 */
	bool PlacePocket(FAirPocket& Pocket, FRandomStream& Stream, double Spread, int32 Ignore, bool bKeepFirstPlace) const;

	/** How long a pocket or mold lasts in one place, drawn from Stream: half of them have broken down by PocketHalfLife. */
	double DrawPocketLife(FRandomStream& Stream) const;

	/** Breaks the pocket or mold at Index down and forms it again somewhere else, the same shape, all at once: what play does a slab a tick. */
	void MovePocket(int32 Index);

	/**
	 * One tick of the move under way: while breaking down, its top slab let go and the placements there judged again; once
	 * gone, a new place drawn; while forming, its next slab from the bottom cut and held, and the placements there judged
	 * again. A slab a tick, so a mold as large as a quarter of the cube costs no frame much.
	 */
	void StepPocketMove();

	/** How many slabs a pocket spans, and the cells one of them covers, as far as the cube goes. */
	int32 GetPocketSlabCount(const FAirPocket& Pocket) const;
	FAirBox GetPocketSlabBox(const FAirPocket& Pocket, int32 Slab) const;

	/** Cuts the pieces of a pocket's next slab, the first not yet cut, onto its lists. */
	void CutPocketSlab(FAirPocket& Pocket) const;

	/** Counts a pocket's pieces into the pocket keys, or out of them: one slab's, or every slab's. */
	void CountPocketSlabKeys(const FAirPocket& Pocket, int32 Slab, int32 Delta);
	void AddPocketKeys(const FAirPocket& Pocket);
	void RemovePocketKeys(const FAirPocket& Pocket);

	/** The pocket or mold moving now, by index, or INDEX_NONE; and whether it is forming again, rather than breaking down. */
	int32 MovingPocket = INDEX_NONE;
	bool bPocketForming = false;

	/** Cuts every pocket's pieces afresh, for the lattice as it stands, and counts them into the pocket keys anew. */
	void RebuildPocketKeys();

	/** The box of cells a pocket or mold reaches into, as far as the cube goes. */
	FAirBox GetPocketCellBounds(const FAirPocket& Pocket) const;

	/** Whether a platform keeps clear of every pocket and every mold's held-back box. */
	bool IsClearOfPockets(const FGridPlatform& Platform) const;

	/** Whether a placement's middle lies in a mold's box, where a beat draws it MoldGrowthWeight times as likely. */
	bool IsInMoldBox(const FGridPlatform& Platform) const;

	/** Draws the shapes one mold leaves open inside its box, from Stream. */
	void DrawMoldParts(FAirPocket& Mold, FRandomStream& Stream) const;

	/** The box of cells a pocket or mold can reach, in cells, its corners included. */
	void GetPocketBounds(const FAirPocket& Pocket, FVector& OutMin, FVector& OutMax) const;

	/** Whether a cell's centre lies inside one of a mold's open shapes, or its open floor. */
	bool IsCellInMoldShape(const FAirPocket& Mold, const FIntVector& Cell) const;

	/** One mold's box and the shapes open inside it, as lines. */
	void AppendMoldLines(const FAirPocket& Mold, TArray<FBatchedLine>& Lines) const;

	/** Whether a cell's centre lies inside a pocket. */
	bool IsCellInPocket(const FAirPocket& Pocket, const FIntVector& Cell) const;

	/** Works out the lattice pieces a pocket reaches into: every one with a cell inside it. */
	void CutPocketPieces(FAirPocket& Pocket) const;

	/** A point in cells - a cell's corner is its coordinate - in the world. */
	FVector CellPointToWorld(const FVector& Point) const;

	/** The colour a pocket of a shape is drawn in. */
	FLinearColor GetAirPocketColor(EAirPocketShape Shape) const;

	/** One pocket's outline, as lines. */
	void AppendAirPocketLines(const FAirPocket& Pocket, TArray<FBatchedLine>& Lines) const;

	/** One path of air: its corners, the box of cells each leg between two of them fills, and what it holds back. */
	struct FAirPath
	{
		/** Its corners, in cells: each the middle of a module, on a module's corner coordinates. */
		TArray<FIntVector> Corners;

		/** Per leg, the inclusive box of cells it fills, as far as the cube goes. */
		TArray<FIntVector> LegMins;
		TArray<FIntVector> LegMaxes;

		/** How many times it has been drawn afresh since the cube was cleared. With the run's seed and its index, where it runs. */
		int32 Draw = 0;

		/** When, in the world's time, play next draws it afresh. */
		double RedrawAt = 0.0;

		/** The lattice pieces it reaches into, each once, keyed as the obstacle keys are. Counted into the path keys. */
		TArray<uint64> FaceKeys;
		TArray<uint64> EdgeKeys;
		TArray<uint64> NodeKeys;

		/** The platforms standing in it when it was drawn, by index in BuiltPlatforms. Growth never adds to them. */
		TArray<int32> Standing;
	};

	/**
	 * Draws every path afresh at its own draw - PathCount of them, keeping the draws of those already there - counts
	 * its pieces into the path keys, judges listed placements again and sets each falling due across the interval
	 * from now. Platforms standing are not touched.
	 */
	void SpawnPaths();

	/** Lays out one path from the run's seed, its index and its draw: its corners, legs and pieces. */
	void DrawPath(int32 Index, FAirPath& Path) const;

	/** Counts a path's pieces into the path keys, or out of them. */
	void AddPathKeys(const FAirPath& Path);
	void RemovePathKeys(const FAirPath& Path);

	/** Finds the platforms standing in a path: every one covering a module face it cuts, or next to an edge or node it cuts. */
	void FindPathStanding(FAirPath& Path) const;

	/** Draws one path afresh somewhere new, judges again only the placements on the pieces it left and now reaches, and sets it due again. */
	void RedrawPath(int32 Index);

	/** Writes PathSummary from the paths as they stand. */
	void UpdatePathSummary();

	/** Lists can be refreshed piece by piece only while they are gathered and match the tiles. */
	bool CanRefreshCandidateLists() const;

	/** The world's time, which the paths, pockets, strikes and reshuffles fall due by. */
	double GetClock() const;

	/** Starts the clock everything that moves during play falls due by: a short tick that does whatever is due. */
	void StartClock();

	/**
	 * The clock's tick: the path or two longest due redrawn, the pocket or mold longest due moved, a strike or a
	 * reshuffle begun or carried on when due, and a few of the platforms with no health left taken away.
	 */
	void AdvanceClock();

	/**
	 * Judges again every placement taking in one of a set of path pieces, each placement once across calls by Judged
	 * (per usable tile). With bMayHaveComeFree, unlisted ones too: the pieces of paths just taken away.
	 */
	void RefreshCandidatesOnPathPieces(
		TConstArrayView<uint64> Faces,
		TConstArrayView<uint64> Edges,
		TConstArrayView<uint64> Nodes,
		bool bMayHaveComeFree,
		TArray<TSet<uint64>>& Judged);

	/** Redraws the path or two longest due at Now. */
	void AdvancePaths(double Now);

	/** Carries the move under way on a slab, or starts moving the pocket or mold longest due at Now, if any is. */
	void AdvancePockets(double Now);

	/** Flushes and redraws the paths' lines, in their own batcher, so moving one does not redraw all the air. */
	void RebuildPathLines();

	/** Whether a platform keeps clear of every lattice piece in a set of keys (or a map keyed by them), as the obstacle keys are asked. */
	template <typename KeySetType>
	bool IsClearOfPieces(
		const FGridPlatform& Platform,
		int32 SpanU,
		int32 SpanV,
		const KeySetType& Faces,
		const KeySetType& Edges,
		const KeySetType& Nodes) const;

	/** Whether a platform keeps clear of every path. Only new placements are held to this. */
	bool IsClearOfPaths(const FGridPlatform& Platform) const;

	/**
	 * Whether a platform keeps clear of all the air held back - paths, pockets, molds and the air over the fields. Only
	 * new placements are held to this; a platform standing in it is vulnerable instead.
	 */
	bool IsClearOfHeldAir(const FGridPlatform& Platform) const;

	/*
	 * Zones
	 * -----
	 * The cube is split into 27 zones, three by three by three - three layers of nine - their borders on lattice
	 * lines a third of the way across along each axis. A platform is in the zone its middle lies in. With
	 * bSeedEveryZone each zone has SeedsPerZone first tiles of its own: the middle zone's first is laid as the first
	 * platform always was, at the heart of the cube, and the beats after it lay the rest, a tile a beat and a round of
	 * zones at a time, before anything else. Each lies flat at a place drawn at random in its zone - on the same steps of the first tile's
	 * width, length and shorter side away from the middle one, so where their growth meets it lines up - clear of
	 * everything standing and of all the air held back. Should every platform be taken away, the cube is seeded
	 * again the same way.
	 *
	 * Deletion
	 * --------
	 * Every platform is laid with health, drawn between MinHealth and MaxHealth. During play a strike falls every
	 * MinStrikeInterval to MaxStrikeInterval seconds: each platform is hit with StrikeFraction, for between MinDamage
	 * and MaxDamage, VulnerableDamageMultiplier times that where the platform crosses air held back. One with no
	 * health left is taken away - no more than that - a few at a time on the clock, so a strike costs no frame
	 * much.
	 *
	 * Taking platforms away undoes just what they brought: their module faces go, every module edge and rim cell
	 * they took in is worked out afresh from what stands around it, every promotion that hung on them is let go
	 * and worked out again, and their pieces and overlay squares are swapped out of the chunks. The placements
	 * round them are judged again, listed or not, since room has come free. The wholesale rebuild stays the
	 * reference (wem.RoomManager.VerifyEvery checks removals too) and the fallback (wem.RoomManager.RemoveByRebuild).
	 * Two rules follow from it: a placement demotes promoted surfaces it builds against, and a floor's block on a
	 * beam holds only while it is the one platform there.
	 *
	 * Every ReshuffleInterval a zone is drawn at random and between MinReshuffleCount and MaxReshuffleCount of its
	 * platforms holding no objects - none standing on either face - are drawn to move. Every ReshuffleStepInterval one
	 * of them is taken away and a platform laid elsewhere in the same zone, as a beat lays one, away from every spot
	 * the reshuffle has emptied.
	 *
	 * Pockets and molds break down and form again somewhere else during play, one at a time on the clock, each after
	 * a life drawn so half of them have gone by PocketHalfLife. Like paths they hold back only what is placed from then
	 * on; what stands where one forms stays, vulnerable, and the rebuild keeps it. Inside a mold's box a beat draws a
	 * placement MoldGrowthWeight times as likely.
	 *
	 * The air over ground and water no longer holds back what stands either, so it can grow. Ground and water joined
	 * across their seams in one plane make a landscape. Every piece keeps MinAirStoreys to MaxAirStoreys of air - one
	 * storey by default - its trunk; with bMushroomAir a crown grows over the landscape like a tree's, from above its
	 * first storey up, MushroomStoreysPerPiece taller and MushroomSpreadPerPiece cells further past the landscape's edges
	 * for every piece beyond the first. It is kept as boxes, bucketed by
	 * where they are, rather than as lattice pieces, since it changes whenever a landscape does.
	 */

	static constexpr int32 ZonesPerSide = 3;
	static constexpr int32 ZoneCount = ZonesPerSide * ZonesPerSide * ZonesPerSide;
	static constexpr int32 MiddleZone = ZoneCount / 2;

	/** Where one of the zones' borders lies along an axis, in cells, from 0 - the cube's near side - to ZonesPerSide, its far side. */
	int32 GetZoneLine(int32 Line) const;

	/** The zone a platform's middle lies in. */
	int32 GetZoneOf(const FGridPlatform& Platform) const;

	/** The cells a zone spans, its borders included. */
	FAirBox GetZoneBox(int32 Zone) const;

	/** The zones' outlines, each in its colour, a little inside its borders so neighbours' lines stay apart. */
	void AppendZoneLines(TArray<FBatchedLine>& Lines) const;

	/** Counts the platforms in each zone into ZoneSummary. */
	void UpdateZoneSummary();

	/** Queues every zone but the middle one for a first tile of its own, when bSeedEveryZone is on. */
	void QueueSeeds();

	/** Lays a first tile of the zone's own: FirstTile, flat, on the middle one's steps, at the first free place drawn. False when none is. */
	bool PlaceSeed(int32 Zone);

	/** Lays health for a platform just taken in, drawn from the damage stream. */
	void GiveHealth(const FGridPlatform& Platform);

	/** Whether a platform crosses air held back, so a hit does it more damage. */
	bool IsVulnerable(const FGridPlatform& Platform) const;

	/** One strike: each platform hit with StrikeFraction, and those left with no health queued to be taken away. */
	void Strike();

	/** Takes away up to Most of the platforms waiting with no health left, oldest first, in one go. */
	void RemoveDying(int32 Most);

	/** Whether any object stands on either face of a platform: one of the obstacles lies against it. */
	bool HoldsObject(const FGridPlatform& Platform) const;

	/** Draws a zone and the platforms in it to move, holding no objects, and starts a reshuffle of them. */
	void BeginReshuffle();

	/** One step of the reshuffle under way: the next platform taken away and another laid elsewhere in its zone. False when it is over. */
	bool StepReshuffle();

	/** Starts a strike or a reshuffle when due, and carries a reshuffle on. */
	void AdvanceDeletion(double Now);

	/** Writes DeletionSummary. */
	void UpdateDeletionSummary();

	/**
	 * Takes platforms out of the structure: out of the list, and out of everything derived from it - by
	 * RemovePlacedPlatforms, or by a wholesale rebuild while wem.RoomManager.RemoveByRebuild is set - along with their
	 * pieces, the overlay's squares, their health, any fence end or field waiting on them, and the air over their fields.
	 */
	void RemovePlatforms(TConstArrayView<FGridPlatform> Removed);

	/**
	 * The inverse of AddPlacedPlatform: takes standing platforms - indices into BuiltPlatforms, ascending - out of the
	 * derived state without rebuilding it, and renumbers whatever names the platforms that stay. Collects the surfaces
	 * that may have changed, the module edges and nodes whose beams and posts may have, and the surfaces whose
	 * promotion now differs.
	 */
	void RemovePlacedPlatforms(
		TConstArrayView<int32> Indices,
		TArray<FGridSurfaceRef>& OutChangedSurfaces,
		TSet<uint64>& OutEdgeKeys,
		TSet<uint64>& OutNodeKeys,
		TArray<FGridSurfaceRef>& OutFlippedSurfaces);

	/** Works out one module edge afresh from the platforms around it: how it is used, how many run round it, and who blocks it. */
	void RecomputeModuleEdge(const FIntVector& Node, int32 Axis);

	/** Works out which axes one rim cell carries broad faces along, afresh from the platforms around it. */
	void RecomputeRimCell(const FIntVector& Cell);

	/**
	 * Judges again the placements round platforms just taken away - every one taking in a module edge of theirs, listed or
	 * not - and those folding off a run whose promotion has changed.
	 */
	void RefreshCandidatesAfterRemoval(TConstArrayView<FGridPlatform> Removed, TConstArrayView<FGridSurfaceRef> FlippedSurfaces);

	/** What has to follow taking platforms out: their pieces, the beams and posts around them, and the overlay's squares. */
	void UpdateVisualsAfterRemoval(TConstArrayView<FGridPlatform> Removed, const TSet<uint64>& EdgeKeys, const TSet<uint64>& NodeKeys, TConstArrayView<FGridSurfaceRef> ChangedSurfaces);

	/** Takes one piece out of its layer, moving no piece but its chunk's last. False when the layer has no such piece. */
	bool RemovePieceFromLayer(FRoomPieceLayer& Layer, uint64 Key);

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
	bool PlacePlatformOfKind(EPlatformKind Kind, const TFunction<bool(const FGridPlatform&)>* Filter = nullptr);

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

	/** When the beats were last caught up to, in the world's time: the beats owed since are what a frame may run. */
	double BeatClock = 0.0;

	/** Placements counted towards PlacementsPerSecond, and when that count started. */
	int32 PlacementsThisSecond = 0;
	double PlacementSecondStart = 0.0;

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

	/** The pockets of air drawn the last time the cube was cleared. Their pieces are among the obstacle keys. */
	TArray<FAirPocket> AirPockets;

	/**
	 * The paths of air, and the lattice pieces they reach into, each with how many paths reach it - kept apart from
	 * the obstacle keys, which the rebuild asks too.
	 */
	TArray<FAirPath> AirPaths;
	TMap<uint64, int32> PathFaces;
	TMap<uint64, int32> PathEdges;
	TMap<uint64, int32> PathNodes;

	/**
	 * The lattice pieces the pockets and molds reach into, each with how many reach it - kept apart from the obstacle keys,
	 * which the rebuild asks, since a pocket moving through platforms leaves them standing.
	 */
	TMap<uint64, int32> PocketFaces;
	TMap<uint64, int32> PocketEdges;
	TMap<uint64, int32> PocketNodes;

	/** How many times a pocket or mold has formed again since the cube was cleared, all of them together. */
	int32 PocketMoves = 0;

	/** The boxes of air the fields keep open, by bucket of cells (see AddAirBoxToBuckets), a box in every bucket it reaches. */
	TMap<uint64, TArray<FAirBox>> AirBoxBuckets;

	/** Every platform's health, by platform key. Beat state, like the streams: never saved, drawn again for any platform missing it. */
	TMap<uint64, float> PlatformHealth;

	/** Platforms with no health left, by platform key, oldest first, waiting to be taken away; and the same as a set. */
	TArray<uint64> DyingQueue;
	TSet<uint64> DyingKeys;

	/** When play next strikes, and next starts and steps a reshuffle. */
	double NextStrikeAt = 0.0;
	double NextReshuffleAt = 0.0;
	double NextReshuffleStepAt = 0.0;

	/** The reshuffle under way: its zone, or INDEX_NONE; the platforms still to move, by key; and the spots it has emptied, kept free. */
	int32 ReshuffleZoneIndex = INDEX_NONE;
	TArray<uint64> ReshuffleKeys;
	TArray<FAirBox> ReshuffleEmptied;

	/** What deletion has done since the cube was cleared, for DeletionSummary. */
	int32 StrikeCount = 0;
	int32 HitCount = 0;
	int32 VulnerableHitCount = 0;
	int32 DamageRemovals = 0;
	int32 ReshuffleCount = 0;
	int32 ReshuffleRemovals = 0;
	int32 ReshufflePlacements = 0;

	/** The zones waiting for a first tile of their own, in order. */
	TArray<int32> SeedQueue;

	/** The min node of the first platform laid in the empty cube, which every other zone's first tile keeps in step with. */
	TOptional<FIntVector> SeedPhase;

	/**
	 * Set when platforms have been taken out since the last rebuild. The faces' streaks and the category patches are
	 * history, which a removal leaves as it was, so the check compares them only while growth has only added.
	 */
	bool bHasRemovals = false;

	/** How many times a path has been drawn afresh since the cube was cleared, all paths together. Readout only. */
	int32 PathRedraws = 0;

	FTimerHandle ClockTimerHandle;

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

	/** Health, strikes and damage, and the first tiles' places, from streams of their own. */
	FRandomStream DamageStream;
	FRandomStream SeedStream;

	/** Which zone is reshuffled, and which of its platforms move. */
	FRandomStream ReshuffleStream;

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

	/** Draws the paths of air. A batcher of its own, since a path moves every fraction of a second during play. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<ULineBatchComponent> PathLineBatcher;
};
