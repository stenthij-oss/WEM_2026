// Copyright Epic Games, Inc. All Rights Reserved.

#include "RoomManager.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LineBatchComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Half-height of the vertical probe used to find the ground under the grid's origin corner. */
	constexpr double GridHeightTraceHalfExtent = 100000.0;

	/**
	 * Fraction of a cell left bare on each side when drawing a surface square, so adjacent
	 * cells read as separate tiles rather than one unbroken sheet of colour.
	 */
	constexpr double SurfaceCellInsetFraction = 0.18;

	/**
	 * How far the surface overlay clears the planes by. Measured from the planes rather than
	 * from the grid base, so raising PlaneZOffset cannot bury the overlay underneath them.
	 */
	constexpr double SurfaceOverlayZBias = 1.0;

	/** Edge length of the engine's unit plane, which every surface is scaled up from. */
	constexpr double UnitPlaneSize = 100.0;

	/** Edge length of the engine's unit cube, which every wall piece is scaled up from. */
	constexpr double UnitCubeSize = 100.0;

	/** The four directions a tile can grow in. */
	const FIntPoint FloorTileGrowthDirections[] = {
		FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1)
	};

	/** The four sides of a tile a wall can stand on. */
	const EGridSide WallSides[] = {
		EGridSide::PosX, EGridSide::NegX, EGridSide::PosY, EGridSide::NegY
	};

	/** Which way a side faces, out from the tile's interior. */
	FORCEINLINE FIntPoint SideDirection(const EGridSide Side)
	{
		switch (Side)
		{
		case EGridSide::NegX: return FIntPoint(-1, 0);
		case EGridSide::PosY: return FIntPoint(0, 1);
		case EGridSide::NegY: return FIntPoint(0, -1);
		default: return FIntPoint(1, 0);
		}
	}

	/** Inclusive max corner of a tile's interior, given its min corner. */
	FORCEINLINE FIntPoint InteriorMaxCorner(const FIntPoint& InteriorMin, const int32 TileSize)
	{
		return FIntPoint(InteriorMin.X + TileSize - 1, InteriorMin.Y + TileSize - 1);
	}

	/** True when two inclusive integer ranges share at least one value. */
	FORCEINLINE bool RangesOverlap(const int32 AMin, const int32 AMax, const int32 BMin, const int32 BMax)
	{
		return AMin <= BMax && BMin <= AMax;
	}

	/** True when two inclusive integer boxes share at least one cell. */
	FORCEINLINE bool BoxesOverlap(
		const FIntPoint& AMin, const FIntPoint& AMax,
		const FIntPoint& BMin, const FIntPoint& BMax)
	{
		return RangesOverlap(AMin.X, AMax.X, BMin.X, BMax.X)
			&& RangesOverlap(AMin.Y, AMax.Y, BMin.Y, BMax.Y);
	}

	/** The rim cells a wall stands on, and the two corners its posts fill. */
	struct FWallFootprint
	{
		/** Inclusive ends of the run. They differ along one axis only - a wall is one cell thick. */
		FIntPoint RunMin = FIntPoint::ZeroValue;
		FIntPoint RunMax = FIntPoint::ZeroValue;

		/** The corner past each end of the run, where the posts go. */
		FIntPoint PostMin = FIntPoint::ZeroValue;
		FIntPoint PostMax = FIntPoint::ZeroValue;
	};

	/** Resolves where a wall on one side of a tile lands. Pure geometry, so it needs no grid state. */
	FWallFootprint ResolveWallFootprint(
		const FIntPoint& InteriorMin, const int32 TileSize, const EGridSide Side)
	{
		const FIntPoint InteriorMax = InteriorMaxCorner(InteriorMin, TileSize);
		const FIntPoint Direction = SideDirection(Side);

		// The interior's own edge row facing this side.
		FIntPoint EdgeMin = InteriorMin;
		FIntPoint EdgeMax = InteriorMax;

		if (Direction.X != 0)
		{
			const int32 EdgeX = Direction.X > 0 ? InteriorMax.X : InteriorMin.X;
			EdgeMin.X = EdgeX;
			EdgeMax.X = EdgeX;
		}
		else
		{
			const int32 EdgeY = Direction.Y > 0 ? InteriorMax.Y : InteriorMin.Y;
			EdgeMin.Y = EdgeY;
			EdgeMax.Y = EdgeY;
		}

		// Unsigned, so stepping it off either end of the run always lands on a corner.
		const FIntPoint Along(FMath::Abs(Direction.Y), FMath::Abs(Direction.X));

		FWallFootprint Footprint;

		// One cell out from the interior puts the run on the rim, where the wall surface is.
		Footprint.RunMin = EdgeMin + Direction;
		Footprint.RunMax = EdgeMax + Direction;

		// The run stops one cell short of the rim's corners at each end; the posts fill them.
		Footprint.PostMin = Footprint.RunMin - Along;
		Footprint.PostMax = Footprint.RunMax + Along;

		return Footprint;
	}
}

ARoomManager::ARoomManager()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	GridLineBatcher = CreateDefaultSubobject<ULineBatchComponent>(TEXT("GridLineBatcher"));
	GridLineBatcher->SetupAttachment(SceneRoot);

	// Instanced rather than a component per piece: a saturated 52x52 grid is a couple of
	// thousand planes, which is one draw call each way like this.
	FloorTilePlanes = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("FloorTilePlanes"));
	FloorTilePlanes->SetupAttachment(SceneRoot);

	WallBeamPlanes = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("WallBeamPlanes"));
	WallBeamPlanes->SetupAttachment(SceneRoot);

	WallPieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("WallPieces"));
	WallPieces->SetupAttachment(SceneRoot);

	WallPostPieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("WallPostPieces"));
	WallPostPieces->SetupAttachment(SceneRoot);

	SlabPieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("SlabPieces"));
	SlabPieces->SetupAttachment(SceneRoot);

	BeamPieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeamPieces"));
	BeamPieces->SetupAttachment(SceneRoot);

	for (UInstancedStaticMeshComponent* Planes : { FloorTilePlanes.Get(), WallBeamPlanes.Get() })
	{
		// Flat, zero-thickness surfaces that mark out where things may go. They are not
		// meant to be stood on or lit, and collision would also be picked up by the grid's
		// own height trace.
		Planes->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Planes->SetCastShadow(false);
		Planes->SetMobility(EComponentMobility::Movable);
	}

	for (UInstancedStaticMeshComponent* Pieces :
		{ WallPieces.Get(), WallPostPieces.Get(), SlabPieces.Get(), BeamPieces.Get() })
	{
		// Everything above the ground stands rather than marks, so it is lit and casts:
		// without shadows falling off it a storey reads as flat as the notation under it.
		// Still no collision, since there is nothing yet to walk into it.
		Pieces->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pieces->SetCastShadow(true);
		Pieces->SetMobility(EComponentMobility::Movable);
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> UnitPlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (UnitPlaneMesh.Succeeded())
	{
		PlaneMesh = UnitPlaneMesh.Object;
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> UnitCubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (UnitCubeMesh.Succeeded())
	{
		WallMesh = UnitCubeMesh.Object;
	}

	// Lit, with the same "Color" parameter the debug material uses, so the walls take light
	// while the notation under them stays flat.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ShapeMaterial(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (ShapeMaterial.Succeeded())
	{
		WallMaterial = ShapeMaterial.Object;
	}

	// Unlit debug material with a "Color" vector parameter, left over from the earlier build.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DebugMaterial(TEXT("/Game/Debug/M_WEMDebug.M_WEMDebug"));
	if (DebugMaterial.Succeeded())
	{
		PlaneMaterial = DebugMaterial.Object;
	}
}

void ARoomManager::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Re-clamp in code so the cap holds even if the values are set outside the Details panel.
	GridSizeX = FMath::Clamp(GridSizeX, 1, MaxGridDimension);
	GridSizeY = FMath::Clamp(GridSizeY, 1, MaxGridDimension);
	FloorTileSize = FMath::Max(FloorTileSize, 1);

	// One height sample for the whole grid, resolved before anything asks for a cell position.
	GridBaseZ = ResolveGridBaseZ();

	// Readouts for the cell currently under inspection.
	DebugCellIndex = CellToIndex(DebugCell);
	DebugCellCenter = IsValidCell(DebugCell) ? GetCellCenter(DebugCell) : FVector::ZeroVector;
	WorldToCell(DebugProbeLocation, DebugProbeCell);

	// Surfaces are derived, so resizing the grid or the tile re-resolves them from scratch.
	RebuildSurfaces();

	RebuildVisuals();
}

void ARoomManager::BeginPlay()
{
	Super::BeginPlay();

	// This actor is meant to be the simulation's sole director. Log rather than enforce.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ARoomManager> It(World); It; ++It)
		{
			if (*It != this)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("RoomManager: more than one ARoomManager in the level ('%s' and '%s'). This actor is meant to be the simulation's sole director."),
					*GetName(), *(*It)->GetName());
				break;
			}
		}
	}

	if (bClearFloorTilesOnBeginPlay)
	{
		ClearFloorTiles();
	}
	else
	{
		ResetPlacementStream();
	}

	// One placement per beat - the first lands a full interval in, so the bare grid is
	// visible before anything grows on it.
	if (bAutoPlaceFloorTiles && FloorTilePlacementInterval > 0.0f)
	{
		GetWorldTimerManager().SetTimer(
			PlacementTimerHandle, this, &ARoomManager::AdvancePlacement,
			FloorTilePlacementInterval, /*bLoop=*/true, /*InFirstDelay=*/FloorTilePlacementInterval);
	}
}

void ARoomManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PlacementTimerHandle);
	}

	Super::EndPlay(EndPlayReason);
}

int32 ARoomManager::GetCellCount() const
{
	return GridSizeX * GridSizeY;
}

bool ARoomManager::IsValidCell(const FIntPoint& Cell) const
{
	return Cell.X >= 0 && Cell.X < GridSizeX && Cell.Y >= 0 && Cell.Y < GridSizeY;
}

int32 ARoomManager::CellToIndex(const FIntPoint& Cell) const
{
	return IsValidCell(Cell) ? Cell.Y * GridSizeX + Cell.X : INDEX_NONE;
}

FIntPoint ARoomManager::IndexToCell(const int32 Index) const
{
	if (Index < 0 || Index >= GetCellCount())
	{
		return FIntPoint(INDEX_NONE, INDEX_NONE);
	}

	return FIntPoint(Index % GridSizeX, Index / GridSizeX);
}

FVector ARoomManager::GetCellMinCorner(const FIntPoint& Cell) const
{
	const FVector Origin = GetActorLocation();

	return FVector(
		Origin.X + static_cast<double>(Cell.X) * CellSize,
		Origin.Y + static_cast<double>(Cell.Y) * CellSize,
		GridBaseZ);
}

FVector ARoomManager::GetCellCenter(const FIntPoint& Cell) const
{
	const double Half = 0.5 * CellSize;

	return GetCellMinCorner(Cell) + FVector(Half, Half, 0.0);
}

FBox ARoomManager::GetCellBounds(const FIntPoint& Cell) const
{
	const FVector Min = GetCellMinCorner(Cell);

	return FBox(Min, Min + FVector(CellSize, CellSize, 0.0));
}

bool ARoomManager::WorldToCell(const FVector& WorldLocation, FIntPoint& OutCell) const
{
	OutCell = FIntPoint(INDEX_NONE, INDEX_NONE);

	if (CellSize <= 0.0f)
	{
		return false;
	}

	const FVector Local = WorldLocation - GetActorLocation();
	const FIntPoint Candidate(
		FMath::FloorToInt32(Local.X / CellSize),
		FMath::FloorToInt32(Local.Y / CellSize));

	if (!IsValidCell(Candidate))
	{
		return false;
	}

	OutCell = Candidate;
	return true;
}

int32 ARoomManager::GetLevelCount() const
{
	return Levels.Num();
}

double ARoomManager::GetLevelBaseZ(const int32 Level) const
{
	// The ground is where the grid sits. Every storey above it clears the one below by a
	// wall's height plus the slab that rests on top of that wall.
	const double StoreyHeight = static_cast<double>(WallHeight) + static_cast<double>(SlabThickness);

	return GridBaseZ + static_cast<double>(Level) * StoreyHeight;
}

EGridSurface ARoomManager::GetCellSurface(const int32 Level, const FIntPoint& Cell) const
{
	if (!Levels.IsValidIndex(Level))
	{
		return EGridSurface::Empty;
	}

	const TArray<EGridSurface>& Surfaces = Levels[Level].CellSurfaces;
	const int32 Index = CellToIndex(Cell);

	return Surfaces.IsValidIndex(Index) ? Surfaces[Index] : EGridSurface::Empty;
}

bool ARoomManager::IsCellOccupied(const int32 Level, const FIntPoint& Cell) const
{
	if (!Levels.IsValidIndex(Level))
	{
		return false;
	}

	const TArray<uint8>& Occupied = Levels[Level].CellOccupied;
	const int32 Index = CellToIndex(Cell);

	return Occupied.IsValidIndex(Index) && Occupied[Index] != 0;
}

int32 ARoomManager::GetTileCount(const int32 Level) const
{
	return Levels.IsValidIndex(Level) ? Levels[Level].Tiles.Num() : 0;
}

int32 ARoomManager::GetWallCount(const int32 Level) const
{
	return Levels.IsValidIndex(Level) ? Levels[Level].Walls.Num() : 0;
}

bool ARoomManager::CanPlaceFloorTileAt(const int32 Level, const FIntPoint& InteriorMin) const
{
	if (!Levels.IsValidIndex(Level))
	{
		return false;
	}

	const int32 TileSize = FMath::Max(FloorTileSize, 1);
	const FIntPoint InteriorMax = InteriorMaxCorner(InteriorMin, TileSize);

	// The beam rim is part of the tile, so the whole footprint has to land on the grid.
	const FIntPoint FootprintMin(InteriorMin.X - 1, InteriorMin.Y - 1);
	const FIntPoint FootprintMax(InteriorMax.X + 1, InteriorMax.Y + 1);

	if (!IsValidCell(FootprintMin) || !IsValidCell(FootprintMax))
	{
		return false;
	}

	// Only this level's own tiles are in the way. A tile lands over whatever the storey
	// below happens to hold, including nothing at all.
	for (const FIntPoint& OtherMin : Levels[Level].Tiles)
	{
		const FIntPoint OtherMax = InteriorMaxCorner(OtherMin, TileSize);
		const FIntPoint OtherFootprintMin(OtherMin.X - 1, OtherMin.Y - 1);
		const FIntPoint OtherFootprintMax(OtherMax.X + 1, OtherMax.Y + 1);

		// Neither interior may sit under the other tile's beams. Two tiles a full pitch
		// apart clear this by exactly one cell - the beam line they will share - so they
		// pass, while anything closer overlaps and is rejected.
		if (BoxesOverlap(InteriorMin, InteriorMax, OtherFootprintMin, OtherFootprintMax)
			|| BoxesOverlap(FootprintMin, FootprintMax, OtherMin, OtherMax))
		{
			return false;
		}
	}

	return true;
}

void ARoomManager::GatherFirstTileCandidates(TArray<FIntPoint>& OutCandidates) const
{
	OutCandidates.Reset();

	const int32 TileSize = FMath::Max(FloorTileSize, 1);

	// The interior starts one cell in from each edge, leaving the border free for the rim.
	for (int32 Y = 1; Y + TileSize <= GridSizeY - 1; ++Y)
	{
		for (int32 X = 1; X + TileSize <= GridSizeX - 1; ++X)
		{
			OutCandidates.Emplace(X, Y);
		}
	}
}

void ARoomManager::GatherFloorTileCandidates(const int32 Level, TArray<FIntPoint>& OutCandidates) const
{
	OutCandidates.Reset();

	if (!Levels.IsValidIndex(Level))
	{
		return;
	}

	// The ground with nothing on it is the one case with nothing to grow from, so that tile
	// is placed freely. Every level above it starts from a beam a wall has left resting there.
	if (Level == 0 && Levels[Level].Tiles.IsEmpty())
	{
		GatherFirstTileCandidates(OutCandidates);
		return;
	}

	// The interior, plus the single beam line the two tiles will share.
	const int32 Pitch = FMath::Max(FloorTileSize, 1) + 1;

	TSet<FIntPoint> Considered;

	// Tiles share candidates with their neighbours, so only test each cell once.
	auto Consider = [this, Level, &Considered, &OutCandidates](const FIntPoint& Candidate)
	{
		bool bAlreadyConsidered = false;
		Considered.Add(Candidate, &bAlreadyConsidered);

		if (!bAlreadyConsidered && CanPlaceFloorTileAt(Level, Candidate))
		{
			OutCandidates.Add(Candidate);
		}
	};

	// Flush against a tile already down on this level.
	for (const FIntPoint& TileMin : Levels[Level].Tiles)
	{
		for (const FIntPoint& Direction : FloorTileGrowthDirections)
		{
			Consider(FIntPoint(TileMin.X + Direction.X * Pitch, TileMin.Y + Direction.Y * Pitch));
		}
	}

	// Or against a beam left by a wall on the level below. That wall stands on a rim line of
	// the same lattice, so the two tile positions it lies between are the one over the tile
	// that raised it and the one across from that - which may have nothing underneath it at
	// all, since a slab is free to reach out over bare ground.
	if (Levels.IsValidIndex(Level - 1))
	{
		for (const FWallSegment& Wall : Levels[Level - 1].Walls)
		{
			const FIntPoint Direction = SideDirection(Wall.Side);

			Consider(Wall.TileMin);
			Consider(FIntPoint(Wall.TileMin.X + Direction.X * Pitch, Wall.TileMin.Y + Direction.Y * Pitch));
		}
	}
}

bool ARoomManager::PlaceNextFloorTile()
{
	// Every level's candidates go into one draw, so the stack grows wherever there is room
	// rather than finishing a storey before starting the next.
	TArray<TPair<int32, FIntPoint>> Placements;
	TArray<FIntPoint> Candidates;

	for (int32 Level = 0; Level < Levels.Num(); ++Level)
	{
		GatherFloorTileCandidates(Level, Candidates);

		for (const FIntPoint& Candidate : Candidates)
		{
			Placements.Emplace(Level, Candidate);
		}
	}

	if (Placements.IsEmpty())
	{
		return false;
	}

	const TPair<int32, FIntPoint> Chosen = Placements[PlacementStream.RandRange(0, Placements.Num() - 1)];
	Levels[Chosen.Key].Tiles.Add(Chosen.Value);

	RebuildSurfaces();
	RebuildVisuals();

	return true;
}

bool ARoomManager::CanPlaceWallAt(const int32 Level, const FIntPoint& TileMin, const EGridSide Side) const
{
	const FWallFootprint Footprint = ResolveWallFootprint(TileMin, FMath::Max(FloorTileSize, 1), Side);

	// The run differs along one axis only, so this walks its cells and nothing else.
	for (int32 Y = Footprint.RunMin.Y; Y <= Footprint.RunMax.Y; ++Y)
	{
		for (int32 X = Footprint.RunMin.X; X <= Footprint.RunMax.X; ++X)
		{
			const FIntPoint Cell(X, Y);

			// A wall surface that is still free is the only thing a wall may stand on. That
			// one test covers both ways a side can be spoken for: conjoined with a
			// neighbour, which reads as all-object, or already walled, which reads as
			// occupied - and the latter is what stops the tile on the far side of a walled
			// line from raising a second wall along it.
			if (GetCellSurface(Level, Cell) != EGridSurface::Wall || IsCellOccupied(Level, Cell))
			{
				return false;
			}
		}
	}

	// The posts are not tested: by the tile lattice a corner cell is a corner for every tile
	// that touches it and never part of another wall's run, so the only thing a post can meet
	// there is another post, and the two share it.
	return true;
}

void ARoomManager::GatherWallCandidates(const int32 Level, TArray<FWallSegment>& OutCandidates) const
{
	OutCandidates.Reset();

	if (!Levels.IsValidIndex(Level))
	{
		return;
	}

	for (const FIntPoint& TileMin : Levels[Level].Tiles)
	{
		for (const EGridSide Side : WallSides)
		{
			if (CanPlaceWallAt(Level, TileMin, Side))
			{
				FWallSegment& Candidate = OutCandidates.AddDefaulted_GetRef();
				Candidate.TileMin = TileMin;
				Candidate.Side = Side;
			}
		}
	}
}

bool ARoomManager::PlaceNextWall()
{
	TArray<TPair<int32, FWallSegment>> Placements;
	TArray<FWallSegment> Candidates;

	for (int32 Level = 0; Level < Levels.Num(); ++Level)
	{
		GatherWallCandidates(Level, Candidates);

		for (const FWallSegment& Candidate : Candidates)
		{
			Placements.Emplace(Level, Candidate);
		}
	}

	if (Placements.IsEmpty())
	{
		return false;
	}

	const TPair<int32, FWallSegment> Chosen = Placements[PlacementStream.RandRange(0, Placements.Num() - 1)];
	Levels[Chosen.Key].Walls.Add(Chosen.Value);

	RebuildSurfaces();
	RebuildVisuals();

	return true;
}

void ARoomManager::ClearFloorTiles()
{
	// One empty level: the ground, with nothing standing on it.
	Levels.Reset();
	Levels.AddDefaulted();

	ResetPlacementStream();

	RebuildSurfaces();
	RebuildVisuals();
}

void ARoomManager::ResetPlacementStream()
{
	// A zero seed means "different every run"; any other value replays the same growth.
	PlacementStream.Initialize(FloorTileRandomSeed != 0 ? FloorTileRandomSeed : FMath::Rand());
}

void ARoomManager::AdvancePlacement()
{
	// The roll only decides which kind is tried first. When the chosen kind has nowhere to
	// go the other one takes the beat, so the opening beat lays a tile however it falls, and
	// a beat is lost only when neither can be placed.
	const bool bWallFirst = PlacementStream.FRand() < WallPlacementChance;

	const bool bPlaced = bWallFirst
		? (PlaceNextWall() || PlaceNextFloorTile())
		: (PlaceNextFloorTile() || PlaceNextWall());

	if (bPlaced)
	{
		return;
	}

	// Nowhere left to grow. Stop the beat rather than retrying on every interval.
	GetWorldTimerManager().ClearTimer(PlacementTimerHandle);

	int32 TileTotal = 0;
	int32 WallTotal = 0;

	for (const FGridLevel& Level : Levels)
	{
		TileTotal += Level.Tiles.Num();
		WallTotal += Level.Walls.Num();
	}

	UE_LOG(LogTemp, Log,
		TEXT("RoomManager: growth stopped with %d tile(s) and %d wall(s) over %d level(s) - there is no room left."),
		TileTotal, WallTotal, Levels.Num());
}

void ARoomManager::EnsureLevelCapacity()
{
	if (Levels.IsEmpty())
	{
		Levels.AddDefaulted();
	}

	// A wall leaves a beam resting on the level above it, so that level has to exist for the
	// beam to land on. One pass upward is enough: the level added is empty, and an empty
	// level has no walls of its own to carry anything higher.
	while (!Levels.Last().Walls.IsEmpty())
	{
		Levels.AddDefaulted();
	}
}

void ARoomManager::RebuildSurfaces()
{
	EnsureLevelCapacity();

	const int32 CellCount = GetCellCount();
	const int32 TileSize = FMath::Max(FloorTileSize, 1);

	for (int32 LevelIndex = 0; LevelIndex < Levels.Num(); ++LevelIndex)
	{
		FGridLevel& Level = Levels[LevelIndex];

		// EGridSurface::Empty is zero, so a zeroed array is a level that carries nothing.
		Level.CellSurfaces.Reset();
		Level.CellSurfaces.SetNumZeroed(CellCount);

		Level.CellOccupied.Reset();
		Level.CellOccupied.SetNumZeroed(CellCount);

		Level.BeamCells.Reset();
		Level.WallPostCells.Reset();

		// This level's own walls first, because occupancy is the one thing nothing placed
		// later can take back. Which cells they stand on is pure geometry, so this reads
		// none of the state below it.
		for (const FWallSegment& Wall : Level.Walls)
		{
			const FWallFootprint Footprint = ResolveWallFootprint(Wall.TileMin, TileSize, Wall.Side);

			for (int32 Y = Footprint.RunMin.Y; Y <= Footprint.RunMax.Y; ++Y)
			{
				for (int32 X = Footprint.RunMin.X; X <= Footprint.RunMax.X; ++X)
				{
					const int32 Index = CellToIndex(FIntPoint(X, Y));
					if (Level.CellOccupied.IsValidIndex(Index))
					{
						Level.CellOccupied[Index] = 1;
					}
				}
			}

			// The two corners the run stops short of. A post already standing in one is
			// shared with the wall that raised it rather than doubled, which is what lets
			// the walls around a tile meet: the run is the only part that has to find its
			// cells free.
			for (const FIntPoint& Post : { Footprint.PostMin, Footprint.PostMax })
			{
				const int32 Index = CellToIndex(Post);
				if (Level.CellOccupied.IsValidIndex(Index) && Level.CellOccupied[Index] == 0)
				{
					Level.CellOccupied[Index] = 1;
					Level.WallPostCells.Add(Post);
				}
			}
		}

		// Interiors next, so a tile's own cells always win over a rim written afterwards.
		for (const FIntPoint& TileMin : Level.Tiles)
		{
			const FIntPoint TileMax = InteriorMaxCorner(TileMin, TileSize);

			for (int32 Y = TileMin.Y; Y <= TileMax.Y; ++Y)
			{
				for (int32 X = TileMin.X; X <= TileMax.X; ++X)
				{
					const int32 Index = CellToIndex(FIntPoint(X, Y));
					if (Level.CellSurfaces.IsValidIndex(Index))
					{
						Level.CellSurfaces[Index] = EGridSurface::AllObject;
					}
				}
			}
		}

		// A beam is written only onto a cell no interior has claimed. That is what makes
		// neighbouring tiles share one beam line instead of stacking two - and because a
		// cell is only ever claimed once, recording it as it is claimed gives a beam list
		// with no duplicates, so no two beams can overlap.
		auto ClaimBeam = [this, &Level](const FIntPoint& Cell)
		{
			const int32 Index = CellToIndex(Cell);

			if (Level.CellSurfaces.IsValidIndex(Index) && Level.CellSurfaces[Index] == EGridSurface::Empty)
			{
				Level.CellSurfaces[Index] = EGridSurface::Wall;
				Level.BeamCells.Add(Cell);
			}
		};

		// Each tile's own rim.
		for (const FIntPoint& TileMin : Level.Tiles)
		{
			const FIntPoint TileMax = InteriorMaxCorner(TileMin, TileSize);

			for (int32 Y = TileMin.Y - 1; Y <= TileMax.Y + 1; ++Y)
			{
				for (int32 X = TileMin.X - 1; X <= TileMax.X + 1; ++X)
				{
					ClaimBeam(FIntPoint(X, Y));
				}
			}
		}

		// Then what the level below leaves under this one: a beam over each wall's run, and
		// one over each of its posts. A run plus its two corners is a whole rim side of a
		// tile, so a wall hands this level the same edge a tile would have brought with it -
		// and a tile arriving here later finds it already down and shares it, exactly as two
		// tiles share a beam line.
		if (Levels.IsValidIndex(LevelIndex - 1))
		{
			for (const FWallSegment& Wall : Levels[LevelIndex - 1].Walls)
			{
				const FWallFootprint Footprint = ResolveWallFootprint(Wall.TileMin, TileSize, Wall.Side);

				for (int32 Y = Footprint.RunMin.Y; Y <= Footprint.RunMax.Y; ++Y)
				{
					for (int32 X = Footprint.RunMin.X; X <= Footprint.RunMax.X; ++X)
					{
						ClaimBeam(FIntPoint(X, Y));
					}
				}

				ClaimBeam(Footprint.PostMin);
				ClaimBeam(Footprint.PostMax);
			}
		}

		// A beam caught between two tiles is no longer a rim, so it joins the interior. The
		// outer rim can never satisfy this - there is always emptiness on one side of it - so
		// the field opens up while staying enclosed. Promotions feed each other, since the
		// centre of a 2x2 arrangement of tiles only resolves once its arms have, so this runs
		// to a fixpoint. The rule is monotone, which makes that fixpoint independent of the
		// sweep order, and of the order the tiles were placed in.
		//
		// A cell that has been built on is passed over: the wall already occupies the surface,
		// so a tile conjoining across it arrives without the line between them ever opening.
		// That is the whole of what separates one room from the next, and it holds for the
		// corner posts as much as for the run - without them the two fields would find each
		// other around the ends of the wall once a 2x2 closed up.
		//
		// Only this level's own walls occupy anything. A beam resting on a wall from the
		// level below is an ordinary beam and opens like any other, so a wall divides its own
		// storey and not the one over it, and each floor is free to be laid out differently
		// from the one it sits on.
		// Only a beam is ever a candidate, and every beam is already listed, so the sweep walks
		// that list rather than the whole grid: at the 512 cap a few thousand cells a pass
		// instead of 262144, most of which are empty and can never promote. The rule is the
		// same one, so it settles on the same fixpoint.
		auto IsAllObjectAt = [&Level, this](const FIntPoint& Cell) -> bool
		{
			const int32 Index = CellToIndex(Cell);
			return Level.CellSurfaces.IsValidIndex(Index) && Level.CellSurfaces[Index] == EGridSurface::AllObject;
		};

		// The beams still standing as rim. A promoted one drops out, so each pass costs only
		// what is left undecided rather than everything decided so far.
		TArray<FIntPoint> PendingBeams;
		PendingBeams.Reserve(Level.BeamCells.Num());

		for (const FIntPoint& BeamCell : Level.BeamCells)
		{
			const int32 Index = CellToIndex(BeamCell);

			if (Level.CellOccupied.IsValidIndex(Index) && Level.CellOccupied[Index] == 0)
			{
				PendingBeams.Add(BeamCell);
			}
		}

		bool bPromotedAny = true;
		while (bPromotedAny)
		{
			bPromotedAny = false;

			// Backwards, so removing the cell just decided cannot disturb the ones still to come.
			for (int32 PendingIndex = PendingBeams.Num() - 1; PendingIndex >= 0; --PendingIndex)
			{
				const FIntPoint Cell = PendingBeams[PendingIndex];

				const bool bFlankedAlongX =
					IsAllObjectAt(Cell + FIntPoint(-1, 0)) && IsAllObjectAt(Cell + FIntPoint(1, 0));

				const bool bFlankedAlongY =
					IsAllObjectAt(Cell + FIntPoint(0, -1)) && IsAllObjectAt(Cell + FIntPoint(0, 1));

				if (bFlankedAlongX || bFlankedAlongY)
				{
					Level.CellSurfaces[CellToIndex(Cell)] = EGridSurface::AllObject;
					PendingBeams.RemoveAtSwap(PendingIndex, EAllowShrinking::No);
					bPromotedAny = true;
				}
			}
		}
	}
}

double ARoomManager::ResolveGridBaseZ() const
{
	const FVector Origin = GetActorLocation();

	if (bSnapToLandscapeHeight)
	{
		if (const UWorld* World = GetWorld())
		{
			// GDR towers sit on flat graded ground, not sculpted terrain, so one height
			// sample at the origin corner is correct - no need to trace per cell.
			const FVector TraceStart = Origin + FVector(0.0, 0.0, GridHeightTraceHalfExtent);
			const FVector TraceEnd = Origin - FVector(0.0, 0.0, GridHeightTraceHalfExtent);

			FCollisionQueryParams Params(FName(TEXT("RoomManagerGridHeight")), /*bTraceComplex=*/false, this);

			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, LandscapeTraceChannel, Params))
			{
				return Hit.ImpactPoint.Z + GridHeightOffset;
			}
		}
	}

	return Origin.Z + GridHeightOffset;
}

void ARoomManager::RebuildVisuals()
{
	RebuildPlanes();
	RebuildDebugGrid();
}

UMaterialInstanceDynamic* ARoomManager::ResolveTintedMaterial(
	TObjectPtr<UMaterialInstanceDynamic>& CachedMaterial,
	UMaterialInterface* BaseMaterial,
	const FName ParameterName,
	const FLinearColor& Color)
{
	if (!BaseMaterial)
	{
		return nullptr;
	}

	// Made once and re-tinted after that. Creating one per rebuild would leave a trail of
	// dead instances behind every placement and every Details-panel edit.
	if (!CachedMaterial || CachedMaterial->Parent != BaseMaterial)
	{
		CachedMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	}

	if (CachedMaterial)
	{
		CachedMaterial->SetVectorParameterValue(ParameterName, Color);
	}

	return CachedMaterial;
}

void ARoomManager::RebuildPlanes()
{
	if (!FloorTilePlanes || !WallBeamPlanes || !WallPieces || !WallPostPieces || !SlabPieces || !BeamPieces)
	{
		return;
	}

	FloorTilePlanes->ClearInstances();
	WallBeamPlanes->ClearInstances();
	SlabPieces->ClearInstances();
	BeamPieces->ClearInstances();
	WallPieces->ClearInstances();
	WallPostPieces->ClearInstances();

	if (!bShowPlanes || Levels.IsEmpty())
	{
		return;
	}

	const int32 TileSize = FMath::Max(FloorTileSize, 1);
	const double TileExtent = TileSize * CellSize;

	// Every batch below is gathered here and handed over in one call. AddInstance one at a
	// time re-invalidates the component's bounds, re-tests navigation relevancy and broadcasts
	// an index-update delegate on each instance; AddInstances does all of that once for the
	// batch, which is the difference between a beat and a stall once a 512 grid fills up.
	TArray<FTransform> Batch;

	// The ground is notation: flat, weightless, a reading of where things may go.
	if (PlaneMesh)
	{
		const double PlaneZ = GridBaseZ + PlaneZOffset;

		// One plane per tile, covering the interior in a single piece - a tile is one thing
		// that was placed, so it is drawn as one thing.
		FloorTilePlanes->SetStaticMesh(PlaneMesh);
		FloorTilePlanes->SetMaterial(0, ResolveTintedMaterial(
			FloorTilePlaneMaterial, PlaneMaterial, PlaneColorParameterName, FloorTilePlaneColor));

		const FVector TileScale(TileExtent / UnitPlaneSize, TileExtent / UnitPlaneSize, 1.0);

		Batch.Reset(Levels[0].Tiles.Num());

		for (const FIntPoint& TileMin : Levels[0].Tiles)
		{
			const FVector Corner = GetCellMinCorner(TileMin);
			const FVector Center(Corner.X + 0.5 * TileExtent, Corner.Y + 0.5 * TileExtent, PlaneZ);

			Batch.Emplace(FRotator::ZeroRotator, Center, TileScale);
		}

		FloorTilePlanes->AddInstances(Batch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);

		// One plane per beam cell. Coplanar and edge-to-edge, so a run of them reads as a
		// single continuous beam, while the per-cell split is what guarantees that no two
		// beams - and no beam and corner - can ever overlap.
		WallBeamPlanes->SetStaticMesh(PlaneMesh);
		WallBeamPlanes->SetMaterial(0, ResolveTintedMaterial(
			WallBeamPlaneMaterial, PlaneMaterial, PlaneColorParameterName, WallBeamPlaneColor));

		const FVector BeamPlaneScale(CellSize / UnitPlaneSize, CellSize / UnitPlaneSize, 1.0);

		Batch.Reset(Levels[0].BeamCells.Num());

		for (const FIntPoint& BeamCell : Levels[0].BeamCells)
		{
			FVector Center = GetCellCenter(BeamCell);
			Center.Z = PlaneZ;

			Batch.Emplace(FRotator::ZeroRotator, Center, BeamPlaneScale);
		}

		WallBeamPlanes->AddInstances(Batch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
	}

	if (!WallMesh)
	{
		return;
	}

	// Everything above the ground is built rather than read: slabs and beams of one
	// thickness, hanging under the surface they make, so a slab's top is the floor stood on.
	if (SlabThickness > 0.0f)
	{
		SlabPieces->SetStaticMesh(WallMesh);
		SlabPieces->SetMaterial(0, ResolveTintedMaterial(
			SlabPieceMaterial, WallMaterial, WallColorParameterName, SlabColor));

		BeamPieces->SetStaticMesh(WallMesh);
		BeamPieces->SetMaterial(0, ResolveTintedMaterial(
			BeamPieceMaterial, WallMaterial, WallColorParameterName, BeamColor));

		const double SlabScaleZ = SlabThickness / UnitCubeSize;
		const FVector SlabScale(TileExtent / UnitCubeSize, TileExtent / UnitCubeSize, SlabScaleZ);
		const FVector BeamScale(CellSize / UnitCubeSize, CellSize / UnitCubeSize, SlabScaleZ);

		TArray<FTransform> BeamBatch;
		Batch.Reset();

		for (int32 LevelIndex = 1; LevelIndex < Levels.Num(); ++LevelIndex)
		{
			const double CenterZ = GetLevelBaseZ(LevelIndex) - 0.5 * SlabThickness;

			for (const FIntPoint& TileMin : Levels[LevelIndex].Tiles)
			{
				const FVector Corner = GetCellMinCorner(TileMin);
				const FVector Center(Corner.X + 0.5 * TileExtent, Corner.Y + 0.5 * TileExtent, CenterZ);

				Batch.Emplace(FRotator::ZeroRotator, Center, SlabScale);
			}

			// One box per beam cell, which covers both a tile's own rim and the beams a wall
			// below has left resting here - by the time they are drawn they are the same thing.
			for (const FIntPoint& BeamCell : Levels[LevelIndex].BeamCells)
			{
				FVector Center = GetCellCenter(BeamCell);
				Center.Z = CenterZ;

				BeamBatch.Emplace(FRotator::ZeroRotator, Center, BeamScale);
			}
		}

		SlabPieces->AddInstances(Batch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		BeamPieces->AddInstances(BeamBatch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
	}

	if (WallHeight <= 0.0f)
	{
		return;
	}

	WallPieces->SetStaticMesh(WallMesh);
	WallPieces->SetMaterial(0, ResolveTintedMaterial(
		WallPieceMaterial, WallMaterial, WallColorParameterName, WallColor));

	WallPostPieces->SetStaticMesh(WallMesh);
	WallPostPieces->SetMaterial(0, ResolveTintedMaterial(
		WallPostPieceMaterial, WallMaterial, WallColorParameterName, WallPostColor));

	const double WallScaleZ = WallHeight / UnitCubeSize;
	const FVector PostScale(CellSize / UnitCubeSize, CellSize / UnitCubeSize, WallScaleZ);

	TArray<FTransform> PostBatch;
	Batch.Reset();

	for (int32 LevelIndex = 0; LevelIndex < Levels.Num(); ++LevelIndex)
	{
		// A wall stands on the surface of its own level, whether that is the ground or a slab.
		const double WallCenterZ = GetLevelBaseZ(LevelIndex) + 0.5 * WallHeight;

		// One box per wall, spanning its whole run - a wall is one thing that was placed, so
		// it is drawn as one thing, the way a tile is.
		for (const FWallSegment& Wall : Levels[LevelIndex].Walls)
		{
			const FWallFootprint Footprint = ResolveWallFootprint(Wall.TileMin, TileSize, Wall.Side);

			if (!IsValidCell(Footprint.RunMin) || !IsValidCell(Footprint.RunMax))
			{
				continue;
			}

			const FVector RunMin = GetCellMinCorner(Footprint.RunMin);
			const FVector RunMax = GetCellMinCorner(Footprint.RunMax) + FVector(CellSize, CellSize, 0.0);

			const FVector Center(0.5 * (RunMin.X + RunMax.X), 0.5 * (RunMin.Y + RunMax.Y), WallCenterZ);
			const FVector Scale(
				(RunMax.X - RunMin.X) / UnitCubeSize,
				(RunMax.Y - RunMin.Y) / UnitCubeSize,
				WallScaleZ);

			Batch.Emplace(FRotator::ZeroRotator, Center, Scale);
		}

		// The posts are separate because they are shared: one stands in a corner however many
		// walls meet there.
		for (const FIntPoint& PostCell : Levels[LevelIndex].WallPostCells)
		{
			FVector Center = GetCellCenter(PostCell);
			Center.Z = WallCenterZ;

			PostBatch.Emplace(FRotator::ZeroRotator, Center, PostScale);
		}
	}

	WallPieces->AddInstances(Batch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
	WallPostPieces->AddInstances(PostBatch, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
}

void ARoomManager::RebuildDebugGrid()
{
	if (!GridLineBatcher)
	{
		return;
	}

	GridLineBatcher->Flush();

	// The lattice and the surfaces are drawn into one batch but toggled separately, so the
	// floorspace can be read on its own without the gridlines behind it.
	TArray<FBatchedLine> Lines;

	if (bShowDebugGrid)
	{
		AppendLatticeLines(Lines);
	}

	if (bShowSurfaces)
	{
		AppendSurfaceLines(Lines);
	}

	if (!Lines.IsEmpty())
	{
		GridLineBatcher->DrawLines(Lines);
	}
}

void ARoomManager::AppendLatticeLines(TArray<FBatchedLine>& Lines) const
{
	// Actor rotation and scale are ignored: the grid is always axis-aligned to world X/Y.
	const FVector Origin = GetActorLocation();
	const double BaseZ = GridBaseZ;
	const double ExtentX = GridSizeX * CellSize;
	const double ExtentY = GridSizeY * CellSize;

	const FLinearColor InteriorColor(GridLineColor);
	const FLinearColor EdgeColor(BoundaryLineColor);

	Lines.Reserve(Lines.Num() + (GridSizeX + 1) + (GridSizeY + 1) + 4 + 6);

	// A lattice - one line per gridline - rather than a box per cell: 1026 lines at the
	// 512 cap instead of 262144 boxes.
	for (int32 X = 0; X <= GridSizeX; ++X)
	{
		const double WorldX = Origin.X + X * CellSize;
		Lines.Emplace(
			FVector(WorldX, Origin.Y, BaseZ),
			FVector(WorldX, Origin.Y + ExtentY, BaseZ),
			InteriorColor, /*LifeTime=*/0.0f, GridLineThickness, SDPG_World);
	}

	for (int32 Y = 0; Y <= GridSizeY; ++Y)
	{
		const double WorldY = Origin.Y + Y * CellSize;
		Lines.Emplace(
			FVector(Origin.X, WorldY, BaseZ),
			FVector(Origin.X + ExtentX, WorldY, BaseZ),
			InteriorColor, /*LifeTime=*/0.0f, GridLineThickness, SDPG_World);
	}

	// The four outer edges again on top, thicker and in the boundary colour.
	const FVector CornerXY(Origin.X, Origin.Y, BaseZ);
	const FVector CornerXMax(Origin.X + ExtentX, Origin.Y, BaseZ);
	const FVector CornerMax(Origin.X + ExtentX, Origin.Y + ExtentY, BaseZ);
	const FVector CornerYMax(Origin.X, Origin.Y + ExtentY, BaseZ);

	Lines.Emplace(CornerXY, CornerXMax, EdgeColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_World);
	Lines.Emplace(CornerXMax, CornerMax, EdgeColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_World);
	Lines.Emplace(CornerMax, CornerYMax, EdgeColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_World);
	Lines.Emplace(CornerYMax, CornerXY, EdgeColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_World);

	// One cell called out by coordinate. Drawn in the foreground group and crossed through,
	// because at grid scale a single 20cm cell is otherwise very hard to pick out.
	if (bHighlightDebugCell && IsValidCell(DebugCell))
	{
		const FVector CellMin = GetCellMinCorner(DebugCell);
		const FVector A = CellMin;
		const FVector B = CellMin + FVector(CellSize, 0.0, 0.0);
		const FVector C = CellMin + FVector(CellSize, CellSize, 0.0);
		const FVector D = CellMin + FVector(0.0, CellSize, 0.0);

		const FLinearColor CellColor(DebugCellColor);

		Lines.Emplace(A, B, CellColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_Foreground);
		Lines.Emplace(B, C, CellColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_Foreground);
		Lines.Emplace(C, D, CellColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_Foreground);
		Lines.Emplace(D, A, CellColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_Foreground);
		Lines.Emplace(A, C, CellColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_Foreground);
		Lines.Emplace(B, D, CellColor, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_Foreground);
	}
}

void ARoomManager::AppendSurfaceLines(TArray<FBatchedLine>& Lines) const
{
	// Each claimed cell is drawn as its own inset square, so a run of them reads as a row
	// of tiles rather than one unbroken sheet of colour.
	const double Inset = CellSize * SurfaceCellInsetFraction;
	const double Span = CellSize - 2.0 * Inset;

	if (Span <= 0.0)
	{
		return;
	}

	const FLinearColor AllObjectColor(AllObjectSurfaceColor);
	const FLinearColor WallSurface(WallSurfaceColor);
	const FLinearColor OccupiedColor(OccupiedSurfaceColor);

	const int32 TileSize = FMath::Max(FloorTileSize, 1);

	// Every storey is read the same way, each at its own height, so the stack can be
	// inspected from the side as well as from above.
	for (int32 LevelIndex = 0; LevelIndex < Levels.Num(); ++LevelIndex)
	{
		const FGridLevel& Level = Levels[LevelIndex];

		// GetCellMinCorner puts a cell on the ground, so the lift is measured from there.
		const double LevelZ = GetLevelBaseZ(LevelIndex) - GridBaseZ + PlaneZOffset + SurfaceOverlayZBias;

		auto AppendCellSquare = [&](const FIntPoint& Cell)
		{
			const int32 Index = CellToIndex(Cell);

			if (!Level.CellSurfaces.IsValidIndex(Index))
			{
				return;
			}

			const EGridSurface Surface = Level.CellSurfaces[Index];

			if (Surface == EGridSurface::Empty)
			{
				return;
			}

			const FVector SquareMin = GetCellMinCorner(Cell) + FVector(Inset, Inset, LevelZ);

			const FVector A = SquareMin;
			const FVector B = SquareMin + FVector(Span, 0.0, 0.0);
			const FVector C = SquareMin + FVector(Span, Span, 0.0);
			const FVector D = SquareMin + FVector(0.0, Span, 0.0);

			// Occupancy is drawn instead of the surface, since what a cell is free to take is
			// the more useful reading and a built-on cell has stopped offering it.
			const FLinearColor& Color =
				(Level.CellOccupied.IsValidIndex(Index) && Level.CellOccupied[Index] != 0)
					? OccupiedColor
					: ((Surface == EGridSurface::AllObject) ? AllObjectColor : WallSurface);

			Lines.Emplace(A, B, Color, /*LifeTime=*/0.0f, SurfaceLineThickness, SDPG_World);
			Lines.Emplace(B, C, Color, /*LifeTime=*/0.0f, SurfaceLineThickness, SDPG_World);
			Lines.Emplace(C, D, Color, /*LifeTime=*/0.0f, SurfaceLineThickness, SDPG_World);
			Lines.Emplace(D, A, Color, /*LifeTime=*/0.0f, SurfaceLineThickness, SDPG_World);
		};

		// A claimed cell is either inside a tile or a beam, and the two lists hold every one of
		// them with no overlap - a beam is only ever written where no interior has been. So the
		// same squares come out of walking those lists as out of reading the whole grid, without
		// asking the 262144 cells of a 512 grid, nearly all empty, one at a time.
		for (const FIntPoint& TileMin : Level.Tiles)
		{
			const FIntPoint TileMax = InteriorMaxCorner(TileMin, TileSize);

			for (int32 Y = TileMin.Y; Y <= TileMax.Y; ++Y)
			{
				for (int32 X = TileMin.X; X <= TileMax.X; ++X)
				{
					AppendCellSquare(FIntPoint(X, Y));
				}
			}
		}

		for (const FIntPoint& BeamCell : Level.BeamCells)
		{
			AppendCellSquare(BeamCell);
		}
	}
}
