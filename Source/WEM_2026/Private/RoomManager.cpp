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
	/** Half-height of the vertical probe used to find the ground under the cube's origin corner. */
	constexpr double GridHeightTraceHalfExtent = 100000.0;

	/**
	 * Fraction of a cell left bare on each side when drawing a surface square, so adjacent
	 * surfaces read as separate tiles rather than one unbroken sheet of colour.
	 */
	constexpr double SurfaceCellInsetFraction = 0.18;

	/**
	 * How far each overlay square stands off the face it marks, out along that face's normal.
	 * Every surface lies exactly on the side of a built box, so without it the two would z-fight.
	 */
	constexpr double SurfaceOverlayBias = 1.0;

	/** Edge length of the engine's unit plane, which every overlay square is scaled down from. */
	constexpr double UnitPlaneSize = 100.0;

	/** Edge length of the engine's unit cube, which every piece is scaled from. */
	constexpr double UnitCubeSize = 100.0;

	/** The overlay's colours, in the order ARoomManager keeps their components and bookkeeping. */
	enum ESurfaceMarkLayer : int32
	{
		AllObjectMarkLayer,
		WallMarkLayer,
		OccupiedMarkLayer
	};

	/**
	 * Most pieces one chunk of a piece layer holds before a fresh chunk is started.
	 *
	 * A placement only ever grows the chunks still being filled, and Lumen recaptures the whole
	 * surface cache of any component it grows - at most 300 cards a frame - so this bounds what
	 * one placement costs it. Smaller is cheaper per placement but means more components to draw.
	 */
	constexpr int32 PieceChunkCapacity = 128;

	/**
	 * Bits per coordinate in a packed key. Inside the cube a coordinate is never negative and
	 * never past the 512 cap, so 20 bits is ample, and the top four bits of the key are left to
	 * say which face or axis it means.
	 */
	constexpr int32 KeyCoordinateBits = 20;
	constexpr uint64 KeyCoordinateMask = (uint64(1) << KeyCoordinateBits) - 1;
	constexpr int32 KeyTagShift = 3 * KeyCoordinateBits;

	/**
	 * One key per cell, packed from the cell's own coordinate. Never from a lattice index: those
	 * count from wherever a platform happens to be and run negative below it.
	 */
	FORCEINLINE uint64 MakeCellKey(const FIntVector& Cell)
	{
		return (static_cast<uint64>(Cell.X) & KeyCoordinateMask)
			| ((static_cast<uint64>(Cell.Y) & KeyCoordinateMask) << KeyCoordinateBits)
			| ((static_cast<uint64>(Cell.Z) & KeyCoordinateMask) << (2 * KeyCoordinateBits));
	}

	FORCEINLINE FIntVector CellFromKey(const uint64 Key)
	{
		return FIntVector(
			static_cast<int32>(Key & KeyCoordinateMask),
			static_cast<int32>((Key >> KeyCoordinateBits) & KeyCoordinateMask),
			static_cast<int32>((Key >> (2 * KeyCoordinateBits)) & KeyCoordinateMask));
	}

	/** A cell plus a small tag - an axis or a face - in the top bits. Offset by one, so no tagged key is ever a bare cell key. */
	FORCEINLINE uint64 MakeTaggedKey(const FIntVector& Cell, const int32 Tag)
	{
		return MakeCellKey(Cell) | (static_cast<uint64>(Tag + 1) << KeyTagShift);
	}

	FORCEINLINE int32 AxisIndex(const EGridAxis Axis)
	{
		return static_cast<int32>(Axis);
	}

	FORCEINLINE uint8 AxisBit(const int32 Axis)
	{
		return static_cast<uint8>(1 << Axis);
	}

	/** A step of Length cells along one axis. */
	FORCEINLINE FIntVector AxisStep(const int32 Axis, const int32 Length = 1)
	{
		FIntVector Step = FIntVector::ZeroValue;
		Step[Axis] = Length;
		return Step;
	}

	/** The two axes a platform facing along NormalAxis lies across. */
	FORCEINLINE void GetInPlaneAxes(const int32 NormalAxis, int32& OutU, int32& OutV)
	{
		OutU = (NormalAxis + 1) % 3;
		OutV = (NormalAxis + 2) % 3;
	}

	/** EGridFace runs +X, -X, +Y, -Y, +Z, -Z, so the axis and the sign fall straight out of it. */
	FORCEINLINE int32 FaceAxis(const EGridFace Face)
	{
		return static_cast<int32>(Face) / 2;
	}

	FORCEINLINE int32 FaceSign(const EGridFace Face)
	{
		return (static_cast<int32>(Face) % 2 == 0) ? 1 : -1;
	}

	FORCEINLINE EGridFace MakeFace(const int32 Axis, const int32 Sign)
	{
		return static_cast<EGridFace>(Axis * 2 + (Sign > 0 ? 0 : 1));
	}

	/** The step from a cell to the one its face looks into. */
	FORCEINLINE FIntVector FaceStep(const EGridFace Face)
	{
		return AxisStep(FaceAxis(Face), FaceSign(Face));
	}

	FORCEINLINE uint64 MakeFaceKey(const FIntVector& Cell, const EGridFace Face)
	{
		return MakeTaggedKey(Cell, static_cast<int32>(Face));
	}

	FORCEINLINE FGridPlatform MakePlatform(const FIntVector& MinNode, const int32 NormalAxis)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = static_cast<EGridAxis>(NormalAxis);
		return Platform;
	}

	/** A platform is named by its slot, so a slot filled twice is still one key. */
	FORCEINLINE uint64 MakePlatformKey(const FGridPlatform& Platform)
	{
		return MakeTaggedKey(Platform.MinNode, AxisIndex(Platform.Normal));
	}

	/** One lattice edge: the node its run starts from, and the axis the run heads along. */
	struct FLatticeEdge
	{
		FIntVector Node = FIntVector::ZeroValue;
		int32 Axis = 0;
	};

	/** An edge is named from its lower node, so every platform that shares it names the same one. */
	FORCEINLINE uint64 MakeEdgeKey(const FLatticeEdge& Edge)
	{
		return MakeTaggedKey(Edge.Node, Edge.Axis);
	}

	/** A platform's four edges: two across each of its in-plane axes. */
	void GetPlatformEdges(const FGridPlatform& Platform, const int32 Pitch, FLatticeEdge (&OutEdges)[4])
	{
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		OutEdges[0] = { Platform.MinNode, U };
		OutEdges[1] = { Platform.MinNode + AxisStep(V, Pitch), U };
		OutEdges[2] = { Platform.MinNode, V };
		OutEdges[3] = { Platform.MinNode + AxisStep(U, Pitch), V };
	}

	/** A platform's four nodes, the posts at its corners. */
	void GetPlatformNodes(const FGridPlatform& Platform, const int32 Pitch, FIntVector (&OutNodes)[4])
	{
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		OutNodes[0] = Platform.MinNode;
		OutNodes[1] = Platform.MinNode + AxisStep(U, Pitch);
		OutNodes[2] = Platform.MinNode + AxisStep(V, Pitch);
		OutNodes[3] = Platform.MinNode + AxisStep(U, Pitch) + AxisStep(V, Pitch);
	}

	/**
	 * The four slots that meet along one edge, two in each plane that contains it. For an edge
	 * along X, the slots facing Z sit either side of it along Y, and the slots facing Y sit
	 * either side of it along Z. Some may fall outside the cube; the caller checks.
	 */
	void GetEdgeSlots(const FLatticeEdge& Edge, const int32 Pitch, FGridPlatform (&OutSlots)[4])
	{
		const int32 AxisA = (Edge.Axis + 1) % 3;
		const int32 AxisB = (Edge.Axis + 2) % 3;

		OutSlots[0] = MakePlatform(Edge.Node, AxisA);
		OutSlots[1] = MakePlatform(Edge.Node - AxisStep(AxisB, Pitch), AxisA);
		OutSlots[2] = MakePlatform(Edge.Node, AxisB);
		OutSlots[3] = MakePlatform(Edge.Node - AxisStep(AxisA, Pitch), AxisB);
	}

	/** Calls Visit(Cell, bRim) for every cell of a platform: interior, edges and nodes alike. */
	template <typename FunctorType>
	void ForEachPlatformCell(const FGridPlatform& Platform, const int32 Pitch, FunctorType&& Visit)
	{
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		for (int32 StepV = 0; StepV <= Pitch; ++StepV)
		{
			for (int32 StepU = 0; StepU <= Pitch; ++StepU)
			{
				const bool bRim = StepU == 0 || StepU == Pitch || StepV == 0 || StepV == Pitch;
				Visit(Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV), bRim);
			}
		}
	}

	/**
	 * The twelve edges of an axis-aligned box, plus its four body diagonals when asked. A corner
	 * is numbered by which of its three coordinates sit at the max, one bit each, so an edge joins
	 * two corners one bit apart and a diagonal joins a corner to its opposite.
	 */
	void AppendBoxLines(
		TArray<FBatchedLine>& Lines,
		const FVector& Min,
		const FVector& Max,
		const FLinearColor& Color,
		const float Thickness,
		const uint8 DepthPriority,
		const bool bWithDiagonals)
	{
		FVector Corners[8];
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Corners[Corner] = FVector(
				(Corner & 1) ? Max.X : Min.X,
				(Corner & 2) ? Max.Y : Min.Y,
				(Corner & 4) ? Max.Z : Min.Z);
		}

		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			for (const int32 Bit : { 1, 2, 4 })
			{
				if ((Corner & Bit) == 0)
				{
					Lines.Emplace(Corners[Corner], Corners[Corner | Bit], Color, /*LifeTime=*/0.0f, Thickness, DepthPriority);
				}
			}
		}

		if (bWithDiagonals)
		{
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				Lines.Emplace(Corners[Corner], Corners[7 - Corner], Color, /*LifeTime=*/0.0f, Thickness, DepthPriority);
			}
		}
	}
}

ARoomManager::ARoomManager()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	GridLineBatcher = CreateDefaultSubobject<ULineBatchComponent>(TEXT("GridLineBatcher"));
	GridLineBatcher->SetupAttachment(SceneRoot);

	// Instanced rather than a component per piece: a cube fills with thousands of them. Each of
	// these four is only the first chunk of its kind; SyncPieceLayer adds more as the structure
	// grows, set up the way these are set up here.
	HorizontalInteriorPieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("HorizontalInteriorPieces"));
	HorizontalInteriorPieces->SetupAttachment(SceneRoot);

	VerticalInteriorPieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("VerticalInteriorPieces"));
	VerticalInteriorPieces->SetupAttachment(SceneRoot);

	EdgePieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("EdgePieces"));
	EdgePieces->SetupAttachment(SceneRoot);

	NodePieces = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("NodePieces"));
	NodePieces->SetupAttachment(SceneRoot);

	AllObjectSurfaceMarks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("AllObjectSurfaceMarks"));
	AllObjectSurfaceMarks->SetupAttachment(SceneRoot);

	WallSurfaceMarks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("WallSurfaceMarks"));
	WallSurfaceMarks->SetupAttachment(SceneRoot);

	OccupiedSurfaceMarks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OccupiedSurfaceMarks"));
	OccupiedSurfaceMarks->SetupAttachment(SceneRoot);

	for (UInstancedStaticMeshComponent* Pieces :
		{ HorizontalInteriorPieces.Get(), VerticalInteriorPieces.Get(), EdgePieces.Get(), NodePieces.Get() })
	{
		// Every platform stands rather than marks, so it is lit and casts: without shadows falling
		// off it the structure reads as flat as the notation laid over it. Still no collision,
		// since there is nothing yet to walk into it.
		Pieces->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pieces->SetCastShadow(true);
		Pieces->SetMobility(EComponentMobility::Movable);
	}

	for (UInstancedStaticMeshComponent* Marks :
		{ AllObjectSurfaceMarks.Get(), WallSurfaceMarks.Get(), OccupiedSurfaceMarks.Get() })
	{
		// Notation laid over the structure, one small square per surface - tens of thousands of
		// them once the cube fills. Kept out of everything that would otherwise track each one:
		// collision, shadows, distance fields, Lumen and ray tracing.
		Marks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Marks->SetCastShadow(false);
		Marks->bAffectDistanceFieldLighting = false;
		Marks->bAffectDynamicIndirectLighting = false;
		Marks->SetVisibleInRayTracing(false);
		Marks->SetMobility(EComponentMobility::Movable);
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> UnitPlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (UnitPlaneMesh.Succeeded())
	{
		PlaneMesh = UnitPlaneMesh.Object;
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> UnitCubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (UnitCubeMesh.Succeeded())
	{
		PieceMesh = UnitCubeMesh.Object;
	}

	// Lit, with the same "Color" parameter the debug material uses, so the pieces take light
	// while the notation over them stays flat.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ShapeMaterial(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (ShapeMaterial.Succeeded())
	{
		PieceMaterial = ShapeMaterial.Object;
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

	// Re-clamp in code so the caps hold even if the values are set outside the Details panel.
	// The platform comes first, since how many of them fit across the cube depends on it.
	PlatformSize = FMath::Clamp(PlatformSize, 1, MaxGridDimension - 2);
	GridPlatformsPerSide = GetPlatformsPerSide();

	// One height sample for the whole cube, resolved before anything asks for a cell position.
	GridBaseZ = ResolveGridBaseZ();

	// Everything derived is rebuilt from the list, so resizing the cube or the platform
	// re-resolves it from scratch.
	RebuildPlatformState();
	UpdateDebugReadouts();

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

	if (bClearPlatformsOnBeginPlay)
	{
		ClearPlatforms();
	}
	else
	{
		ResetPlacementStream();

		// The derived state is never saved or copied, so a PIE copy arrives with the platform
		// list and nothing worked out from it. Work it out before the first beat reads it.
		RebuildPlatformState();
		UpdateDebugReadouts();
		RebuildVisuals();
	}

	// One placement per beat - the first lands a full interval in, so the empty cube is
	// visible before anything grows in it.
	if (bAutoPlacePlatforms && PlatformPlacementInterval > 0.0f)
	{
		FTimerManagerTimerParameters BeatParameters;
		BeatParameters.bLoop = true;
		BeatParameters.FirstDelay = PlatformPlacementInterval;

		// A beat that falls behind is dropped, not made up. Catching up would stack several
		// placements into one frame that is already slow, which only makes the next one slower,
		// and it would break the rhythm of one piece arriving at a time.
		BeatParameters.bMaxOncePerFrame = true;

		GetWorldTimerManager().SetTimer(
			PlacementTimerHandle, this, &ARoomManager::AdvancePlacement,
			PlatformPlacementInterval, BeatParameters);
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

bool ARoomManager::IsValidCell(const FIntVector& Cell) const
{
	const int32 CubeCells = ResolveGridSize();

	return Cell.X >= 0 && Cell.X < CubeCells
		&& Cell.Y >= 0 && Cell.Y < CubeCells
		&& Cell.Z >= 0 && Cell.Z < CubeCells;
}

FVector ARoomManager::GetCellMinCorner(const FIntVector& Cell) const
{
	const FVector Origin = GetActorLocation();

	return FVector(
		Origin.X + static_cast<double>(Cell.X) * CellSize,
		Origin.Y + static_cast<double>(Cell.Y) * CellSize,
		GridBaseZ + static_cast<double>(Cell.Z) * CellSize);
}

FVector ARoomManager::GetCellCenter(const FIntVector& Cell) const
{
	return GetCellMinCorner(Cell) + FVector(0.5 * CellSize);
}

FBox ARoomManager::GetCellBounds(const FIntVector& Cell) const
{
	const FVector Min = GetCellMinCorner(Cell);

	return FBox(Min, Min + FVector(CellSize));
}

bool ARoomManager::WorldToCell(const FVector& WorldLocation, FIntVector& OutCell) const
{
	OutCell = FIntVector(INDEX_NONE, INDEX_NONE, INDEX_NONE);

	if (CellSize <= 0.0f)
	{
		return false;
	}

	// Measured from the cube's corner, which stands on the resolved ground rather than at the
	// actor's own height.
	const FVector Origin = GetActorLocation();
	const FIntVector Candidate(
		FMath::FloorToInt32((WorldLocation.X - Origin.X) / CellSize),
		FMath::FloorToInt32((WorldLocation.Y - Origin.Y) / CellSize),
		FMath::FloorToInt32((WorldLocation.Z - GridBaseZ) / CellSize));

	if (!IsValidCell(Candidate))
	{
		return false;
	}

	OutCell = Candidate;
	return true;
}

ESurfaceCapacity ARoomManager::GetSurfaceCapacity(const FIntVector& Cell, const EGridFace Face) const
{
	ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
	bool bOccupied = false;
	ResolveSurface(Cell, Face, Capacity, bOccupied);

	return Capacity;
}

bool ARoomManager::IsSurfaceOccupied(const FIntVector& Cell, const EGridFace Face) const
{
	ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
	bool bOccupied = false;
	ResolveSurface(Cell, Face, Capacity, bOccupied);

	return bOccupied;
}

FTransform ARoomManager::GetSurfaceWorldTransform(const FIntVector& Cell, const EGridFace Face) const
{
	const FIntVector Step = FaceStep(Face);
	const FVector Normal(Step);

	// Up is the face's normal. Forward is pinned to a world axis lying in the face, so a
	// surface's frame never depends on how a rotation happened to be solved: along X for floors,
	// ceilings and walls facing Y, and along Y for walls facing X.
	const FVector Forward = FaceAxis(Face) == 0 ? FVector::YAxisVector : FVector::XAxisVector;

	return FTransform(
		FRotationMatrix::MakeFromZX(Normal, Forward).Rotator(),
		GetCellCenter(Cell) + Normal * (0.5 * CellSize));
}

void ARoomManager::GatherSurfaces(
	const ESurfaceCapacity Filter,
	const bool bIncludeOccupied,
	TArray<FGridSurfaceRef>& OutSurfaces) const
{
	OutSurfaces.Reset();

	// Everything that is a surface carries something, so there is nothing to find.
	if (Filter == ESurfaceCapacity::Empty)
	{
		return;
	}

	const int32 Pitch = GetPitch();

	// Two coplanar platforms both name the rim they share, so a rim surface is listed only the
	// first time. Interiors are never shared and are not tracked.
	TSet<uint64> ListedRimSurfaces;

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		const int32 NormalAxis = AxisIndex(Platform.Normal);

		ForEachPlatformCell(Platform, Pitch, [&](const FIntVector& Cell, const bool bRim)
		{
			for (const int32 Sign : { 1, -1 })
			{
				const EGridFace Face = MakeFace(NormalAxis, Sign);

				if (bRim)
				{
					bool bAlreadyListed = false;
					ListedRimSurfaces.Add(MakeFaceKey(Cell, Face), &bAlreadyListed);

					if (bAlreadyListed)
					{
						continue;
					}
				}

				ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
				bool bOccupied = false;
				ResolveSurface(Cell, Face, Capacity, bOccupied);

				if (Capacity == Filter && (bIncludeOccupied || !bOccupied))
				{
					FGridSurfaceRef& Surface = OutSurfaces.AddDefaulted_GetRef();
					Surface.Cell = Cell;
					Surface.Face = Face;
				}
			}
		});
	}
}

int32 ARoomManager::GetPlatformCount() const
{
	return BuiltPlatforms.Num();
}

bool ARoomManager::CanPlacePlatform(const FIntVector& MinNode, const EGridAxis Normal) const
{
	const FGridPlatform Candidate = MakePlatform(MinNode, AxisIndex(Normal));

	// On the lattice, with the whole footprint - interior, rim and far nodes - inside the cube.
	// That alone keeps anything from going below the ground layer.
	if (!IsPlatformOnGrid(Candidate))
	{
		return false;
	}

	// With nothing placed there is nothing to grow from, so the first platform has a rule of
	// its own: horizontal, and on the centre layer.
	if (BuiltPlatforms.IsEmpty())
	{
		return Normal == EGridAxis::Z && MinNode.Z == GetFirstPlatformLayer();
	}

	if (PlatformKeys.Contains(MakePlatformKey(Candidate)))
	{
		return false;
	}

	// No interior cell may already be built. On one lattice an interior is all or nothing - the
	// only thing that can cover any of it is this slot's own platform - so one cell stands for
	// all of them.
	int32 U, V;
	GetInPlaneAxes(AxisIndex(Normal), U, V);

	if (IsSolidCell(MinNode + AxisStep(U) + AxisStep(V)))
	{
		return false;
	}

	const int32 Pitch = GetPitch();
	const int32 RunLength = Pitch - 1;
	bool bTouchesStructure = false;

	// The candidate is judged against every platform it would share an edge with, not only the
	// one it was found from. Continuing a platform in its own plane asks nothing more, but a
	// single fold that is refused refuses the whole slot - otherwise a wall carrying on from one
	// already standing could be raised over a seam that has opened, closing it again.
	FLatticeEdge Edges[4];
	GetPlatformEdges(Candidate, Pitch, Edges);

	for (const FLatticeEdge& Edge : Edges)
	{
		FGridPlatform Slots[4];
		GetEdgeSlots(Edge, Pitch, Slots);

		for (const FGridPlatform& Neighbour : Slots)
		{
			const bool bIsCandidate = Neighbour.Normal == Candidate.Normal && Neighbour.MinNode == Candidate.MinNode;

			if (bIsCandidate || !IsPlatformOnGrid(Neighbour) || !PlatformKeys.Contains(MakePlatformKey(Neighbour)))
			{
				continue;
			}

			bTouchesStructure = true;

			if (Neighbour.Normal == Candidate.Normal)
			{
				continue;
			}

			// A fold leaves the edge along the neighbour's normal, to one side of it or the
			// other. Every cell of the edge's run has to be offering itself on that side: wall
			// surface, not yet built against. A seam that has opened reads as all-object and a
			// side already folded off reads as occupied, and either refuses. The posts at the
			// run's ends are not asked, since a post is shared by whatever meets at it.
			const int32 FoldAxis = AxisIndex(Neighbour.Normal);
			const EGridFace Toward = MakeFace(FoldAxis, MinNode[FoldAxis] == Edge.Node[FoldAxis] ? 1 : -1);

			for (int32 RunStep = 1; RunStep <= RunLength; ++RunStep)
			{
				ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
				bool bOccupied = false;
				ResolveSurface(Edge.Node + AxisStep(Edge.Axis, RunStep), Toward, Capacity, bOccupied);

				if (Capacity != ESurfaceCapacity::Wall || bOccupied)
				{
					return false;
				}
			}
		}
	}

	// A slot touching nothing would start a second structure; growth only ever extends the one.
	return bTouchesStructure;
}

void ARoomManager::GatherFirstPlatformCandidates(TArray<FGridPlatform>& OutCandidates) const
{
	OutCandidates.Reset();

	const int32 Pitch = GetPitch();
	const int32 PlatformsPerSide = GetPlatformsPerSide();
	const int32 Layer = GetFirstPlatformLayer();

	// Every lattice position across the centre layer. The lattice is pinned to the cube, so the
	// first platform only chooses where on it to start, never where the lattice falls.
	for (int32 SlotY = 0; SlotY < PlatformsPerSide; ++SlotY)
	{
		for (int32 SlotX = 0; SlotX < PlatformsPerSide; ++SlotX)
		{
			OutCandidates.Add(MakePlatform(FIntVector(SlotX * Pitch, SlotY * Pitch, Layer), AxisIndex(EGridAxis::Z)));
		}
	}
}

void ARoomManager::GatherPlatformCandidates(const EPlatformKind Kind, TArray<FGridPlatform>& OutCandidates) const
{
	OutCandidates.Reset();

	// The empty cube is the one case with nothing to grow from, and only a horizontal starts it.
	if (BuiltPlatforms.IsEmpty())
	{
		if (Kind == EPlatformKind::SurfaceHorizontal)
		{
			GatherFirstPlatformCandidates(OutCandidates);
		}

		return;
	}

	const int32 Pitch = GetPitch();

	// Neighbouring platforms share edges, and so share candidates; test each slot once.
	TSet<uint64> Considered;

	// Every slot around every edge of every platform goes into one draw, so the structure grows
	// wherever there is room rather than finishing one part before starting the next.
	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		FLatticeEdge Edges[4];
		GetPlatformEdges(Platform, Pitch, Edges);

		for (const FLatticeEdge& Edge : Edges)
		{
			FGridPlatform Slots[4];
			GetEdgeSlots(Edge, Pitch, Slots);

			for (const FGridPlatform& Slot : Slots)
			{
				if (Slot.GetKind() != Kind || !IsPlatformOnGrid(Slot))
				{
					continue;
				}

				bool bAlreadyConsidered = false;
				Considered.Add(MakePlatformKey(Slot), &bAlreadyConsidered);

				if (!bAlreadyConsidered && CanPlacePlatform(Slot.MinNode, Slot.Normal))
				{
					OutCandidates.Add(Slot);
				}
			}
		}
	}

	// Drawn from in key order rather than the order they were found in, so what the stream picks
	// depends only on which platforms stand - not on the order the list happens to hold them.
	OutCandidates.Sort([](const FGridPlatform& A, const FGridPlatform& B)
	{
		return MakePlatformKey(A) < MakePlatformKey(B);
	});
}

bool ARoomManager::PlacePlatformOfKind(const EPlatformKind Kind)
{
	TArray<FGridPlatform> Candidates;
	GatherPlatformCandidates(Kind, Candidates);

	if (Candidates.IsEmpty())
	{
		return false;
	}

	Platforms.Add(Candidates[PlacementStream.RandRange(0, Candidates.Num() - 1)]);

	RebuildPlatformState();
	UpdateDebugReadouts();
	UpdateVisualsAfterPlacement();

	return true;
}

bool ARoomManager::PlaceNextPlatform()
{
	// The roll only decides which kind is tried first. When the chosen kind has nowhere to go
	// the other one takes the beat, so the opening beat lays the first platform however it
	// falls, and a beat is lost only when neither kind can be placed.
	const bool bVerticalFirst = PlacementStream.FRand() < VerticalPlacementChance;

	const EPlatformKind FirstKind = bVerticalFirst ? EPlatformKind::SurfaceVertical : EPlatformKind::SurfaceHorizontal;
	const EPlatformKind SecondKind = bVerticalFirst ? EPlatformKind::SurfaceHorizontal : EPlatformKind::SurfaceVertical;

	return PlacePlatformOfKind(FirstKind) || PlacePlatformOfKind(SecondKind);
}

void ARoomManager::ClearPlatforms()
{
	Platforms.Reset();

	ResetPlacementStream();

	RebuildPlatformState();
	UpdateDebugReadouts();
	RebuildVisuals();
}

void ARoomManager::ResetPlacementStream()
{
	// A zero seed means "different every run"; any other value replays the same growth.
	PlacementStream.Initialize(PlatformRandomSeed != 0 ? PlatformRandomSeed : FMath::Rand());
}

void ARoomManager::AdvancePlacement()
{
	if (PlaceNextPlatform())
	{
		return;
	}

	// Nowhere left to grow. Stop the beat rather than retrying on every interval.
	GetWorldTimerManager().ClearTimer(PlacementTimerHandle);

	UE_LOG(LogTemp, Log,
		TEXT("RoomManager: growth stopped with %d platform(s), %d horizontal and %d vertical - there is no room left."),
		PlatformCount, HorizontalPlatformCount, VerticalPlatformCount);
}

int32 ARoomManager::GetPitch() const
{
	return FMath::Clamp(PlatformSize, 1, MaxGridDimension - 2) + 1;
}

int32 ARoomManager::GetPlatformsPerSide() const
{
	// The pitch is capped so that at least one platform, and the node closing it, always fits.
	return FMath::Clamp(GridPlatformsPerSide, 1, (MaxGridDimension - 1) / GetPitch());
}

int32 ARoomManager::ResolveGridSize() const
{
	return GetPlatformsPerSide() * GetPitch() + 1;
}

int32 ARoomManager::GetFirstPlatformLayer() const
{
	// Node layers run from the ground to the cube's top. With an even count of platforms per
	// side one lands exactly on the centre; with an odd count the centre falls mid-pitch, and
	// the layer just above it is taken.
	return ((GetPlatformsPerSide() + 1) / 2) * GetPitch();
}

bool ARoomManager::IsPlatformOnGrid(const FGridPlatform& Platform) const
{
	const int32 NormalAxis = AxisIndex(Platform.Normal);

	if (NormalAxis < 0 || NormalAxis > 2)
	{
		return false;
	}

	const int32 Pitch = GetPitch();
	const int32 LastNode = ResolveGridSize() - 1;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const int32 Coordinate = Platform.MinNode[Axis];

		// Across its plane a platform reaches a whole pitch past its min node, so the far rim
		// has to fit inside too, which leaves one pitch less room there than along its normal.
		const int32 Limit = Axis == NormalAxis ? LastNode : LastNode - Pitch;

		if (Coordinate < 0 || Coordinate > Limit || Coordinate % Pitch != 0)
		{
			return false;
		}
	}

	return true;
}

void ARoomManager::RebuildPlatformState()
{
	GridSize = ResolveGridSize();

	const int32 Pitch = GetPitch();

	BuiltPlatforms.Reset();
	PlatformKeys.Reset();
	RimCellNormals.Reset();
	PromotedFaces.Reset();

	// Only the platforms that fit the lattice as it stands now. One placed under a different
	// platform size or a larger cube no longer does, and is left out rather than drawn across the
	// grid - but kept in the list, so changing the size back brings it back.
	int32 SkippedPlatforms = 0;

	for (const FGridPlatform& Platform : Platforms)
	{
		if (!IsPlatformOnGrid(Platform))
		{
			++SkippedPlatforms;
			continue;
		}

		bool bAlreadyBuilt = false;
		PlatformKeys.Add(MakePlatformKey(Platform), &bAlreadyBuilt);

		if (!bAlreadyBuilt)
		{
			BuiltPlatforms.Add(Platform);
		}
	}

	// This runs every beat, so the warning is only given when what it would say has changed.
	if (SkippedPlatforms != ReportedSkippedPlatforms)
	{
		ReportedSkippedPlatforms = SkippedPlatforms;

		if (SkippedPlatforms > 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("RoomManager: %d platform(s) in the list do not fit a lattice of PlatformSize %d across %d platform(s) per side, and are left out. ClearPlatforms starts over."),
				SkippedPlatforms, GetPitch() - 1, GetPlatformsPerSide());
		}
	}

	// Every rim cell, with the axes it faces along. A cell on an edge can be rim to platforms
	// facing two ways, and a node to all three; each platform brings the pair of faces along its
	// own normal, which is how a cell comes to carry surfaces along more than one axis.
	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		const uint8 NormalBit = AxisBit(AxisIndex(Platform.Normal));

		ForEachPlatformCell(Platform, Pitch, [this, NormalBit](const FIntVector& Cell, const bool bRim)
		{
			if (bRim)
			{
				RimCellNormals.FindOrAdd(MakeCellKey(Cell)) |= NormalBit;
			}
		});
	}

	// A rim surface caught between two fields is no longer rim, so it joins them. It is judged
	// within its own plane and on its own side: flanked on opposite sides, along either axis of
	// that plane, by surfaces facing the same way that carry anything and are not built against.
	// The outer rim can never satisfy this - there is always emptiness on one side of it - so a
	// field opens up while staying enclosed. Promotions feed each other, since the node at the
	// centre of a 2x2 only resolves once the seams around it have, so this runs to a fixpoint.
	// The rule is monotone, which makes that fixpoint independent of the sweep order, and of the
	// order the platforms were placed in.
	//
	// A surface already built against is never a candidate: the thing standing on it holds that
	// side shut, which is the whole of what separates one room from the next. It holds at the
	// posts as much as along the run, so two fields cannot find each other around the end of a
	// wall once the platforms around it close up.
	//
	// Interior surfaces already carry anything and never need deciding, so the sweep walks the
	// rim surfaces alone, and drops each as it is decided: every pass costs only what is still
	// open rather than the whole structure.
	struct FPendingSurface
	{
		FIntVector Cell;
		EGridFace Face;
		uint64 Key;
	};

	TArray<FPendingSurface> PendingSurfaces;
	PendingSurfaces.Reserve(RimCellNormals.Num() * 2);

	for (const TPair<uint64, uint8>& RimCell : RimCellNormals)
	{
		const FIntVector Cell = CellFromKey(RimCell.Key);

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if ((RimCell.Value & AxisBit(Axis)) == 0)
			{
				continue;
			}

			for (const int32 Sign : { 1, -1 })
			{
				const EGridFace Face = MakeFace(Axis, Sign);

				if (!IsSolidCell(Cell + FaceStep(Face)))
				{
					PendingSurfaces.Add({ Cell, Face, MakeFaceKey(Cell, Face) });
				}
			}
		}
	}

	auto IsOpenAllObject = [this](const FIntVector& Neighbour, const EGridFace Side) -> bool
	{
		ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
		bool bOccupied = false;
		ResolveSurface(Neighbour, Side, Capacity, bOccupied);

		return Capacity == ESurfaceCapacity::AllObject && !bOccupied;
	};

	bool bPromotedAny = true;
	while (bPromotedAny)
	{
		bPromotedAny = false;

		// Backwards, so removing the surface just decided cannot disturb the ones still to come.
		for (int32 PendingIndex = PendingSurfaces.Num() - 1; PendingIndex >= 0; --PendingIndex)
		{
			const FPendingSurface Pending = PendingSurfaces[PendingIndex];
			const int32 NormalAxis = FaceAxis(Pending.Face);

			bool bFlanked = false;

			for (int32 InPlaneAxis = 0; InPlaneAxis < 3 && !bFlanked; ++InPlaneAxis)
			{
				if (InPlaneAxis == NormalAxis)
				{
					continue;
				}

				const FIntVector Across = AxisStep(InPlaneAxis);
				bFlanked = IsOpenAllObject(Pending.Cell - Across, Pending.Face)
					&& IsOpenAllObject(Pending.Cell + Across, Pending.Face);
			}

			if (bFlanked)
			{
				PromotedFaces.Add(Pending.Key);
				PendingSurfaces.RemoveAtSwap(PendingIndex, EAllowShrinking::No);
				bPromotedAny = true;
			}
		}
	}

	// The Details panel's view of the list, in place of the list itself.
	PlatformCount = BuiltPlatforms.Num();
	HorizontalPlatformCount = 0;
	VerticalPlatformCount = 0;

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		if (Platform.GetKind() == EPlatformKind::SurfaceHorizontal)
		{
			++HorizontalPlatformCount;
		}
		else
		{
			++VerticalPlatformCount;
		}
	}
}

uint8 ARoomManager::GetCellSurfaceAxes(const FIntVector& Cell, bool& bOutInterior) const
{
	bOutInterior = false;

	if (!IsValidCell(Cell))
	{
		return 0;
	}

	const int32 Pitch = GetPitch();

	// Which lattice planes the cell lies on says what it could be: on none, it is inside one of
	// the lattice's cubes, where nothing is ever built; on one, it is interior to the one slot in
	// that plane; on two it is on an edge, and on three it is a node.
	int32 LatticePlaneCount = 0;
	int32 LatticePlaneAxis = INDEX_NONE;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Cell[Axis] % Pitch == 0)
		{
			++LatticePlaneCount;
			LatticePlaneAxis = Axis;
		}
	}

	if (LatticePlaneCount == 0)
	{
		return 0;
	}

	// An interior cell is built exactly when its slot is filled, so interiors need no record of
	// their own - which is most of every platform.
	if (LatticePlaneCount == 1)
	{
		FIntVector SlotMinNode = Cell;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			SlotMinNode[Axis] -= Cell[Axis] % Pitch;
		}

		if (!PlatformKeys.Contains(MakeTaggedKey(SlotMinNode, LatticePlaneAxis)))
		{
			return 0;
		}

		bOutInterior = true;
		return AxisBit(LatticePlaneAxis);
	}

	// An edge or a node can have been brought by any of several platforms, so those are recorded.
	const uint8* RimAxes = RimCellNormals.Find(MakeCellKey(Cell));

	return RimAxes ? *RimAxes : 0;
}

bool ARoomManager::IsSolidCell(const FIntVector& Cell) const
{
	bool bInterior = false;

	return GetCellSurfaceAxes(Cell, bInterior) != 0;
}

void ARoomManager::ResolveSurface(
	const FIntVector& Cell,
	const EGridFace Face,
	ESurfaceCapacity& OutCapacity,
	bool& bOutOccupied) const
{
	OutCapacity = ESurfaceCapacity::Empty;
	bOutOccupied = false;

	bool bInterior = false;
	const uint8 SurfaceAxes = GetCellSurfaceAxes(Cell, bInterior);

	// Nothing built here faces this way. The thin outer edge of a rim lands here too: it faces
	// across its platform, not along the platform's normal.
	if ((SurfaceAxes & AxisBit(FaceAxis(Face))) == 0)
	{
		return;
	}

	// Occupancy outranks what a surface carries, but does not replace it: an occupied rim
	// surface is still wall surface, it has only stopped being a free one.
	bOutOccupied = IsSolidCell(Cell + FaceStep(Face));

	OutCapacity = (bInterior || PromotedFaces.Contains(MakeFaceKey(Cell, Face)))
		? ESurfaceCapacity::AllObject
		: ESurfaceCapacity::Wall;
}

void ARoomManager::UpdateDebugReadouts()
{
	DebugCellCenter = IsValidCell(DebugCell) ? GetCellCenter(DebugCell) : FVector::ZeroVector;
	WorldToCell(DebugProbeLocation, DebugProbeCell);

	DebugProbeSurface.Cell = FIntVector(INDEX_NONE, INDEX_NONE, INDEX_NONE);
	DebugProbeSurface.Face = EGridFace::PosZ;
	DebugProbeSurfaceCapacity = ESurfaceCapacity::Empty;
	bDebugProbeSurfaceOccupied = false;

	if (CellSize <= 0.0f)
	{
		return;
	}

	// The nearest surface is looked for in the probe's own cell and the 26 around it. That
	// reaches every face within a cell of the probe - which is what a probe held up against the
	// structure is asking about - without searching the whole of it. The probe may itself sit
	// just outside the cube and still be next to a surface on its boundary, so its cell is taken
	// unclamped.
	const FVector Origin = GetActorLocation();
	const FIntVector ProbeCell(
		FMath::FloorToInt32((DebugProbeLocation.X - Origin.X) / CellSize),
		FMath::FloorToInt32((DebugProbeLocation.Y - Origin.Y) / CellSize),
		FMath::FloorToInt32((DebugProbeLocation.Z - GridBaseZ) / CellSize));

	bool bFoundSurface = false;
	double NearestDistanceSquared = 0.0;
	uint64 NearestKey = 0;

	for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
	{
		for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
		{
			for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
			{
				const FIntVector Cell = ProbeCell + FIntVector(OffsetX, OffsetY, OffsetZ);

				bool bInterior = false;
				const uint8 SurfaceAxes = GetCellSurfaceAxes(Cell, bInterior);

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if ((SurfaceAxes & AxisBit(Axis)) == 0)
					{
						continue;
					}

					for (const int32 Sign : { 1, -1 })
					{
						const EGridFace Face = MakeFace(Axis, Sign);
						const uint64 Key = MakeFaceKey(Cell, Face);
						const double DistanceSquared =
							FVector::DistSquared(DebugProbeLocation, GetSurfaceWorldTransform(Cell, Face).GetLocation());

						// Ties go to the lower key, so the answer never depends on the search order.
						if (!bFoundSurface
							|| DistanceSquared < NearestDistanceSquared
							|| (DistanceSquared == NearestDistanceSquared && Key < NearestKey))
						{
							bFoundSurface = true;
							NearestDistanceSquared = DistanceSquared;
							NearestKey = Key;
							DebugProbeSurface.Cell = Cell;
							DebugProbeSurface.Face = Face;
						}
					}
				}
			}
		}
	}

	if (bFoundSurface)
	{
		ResolveSurface(DebugProbeSurface.Cell, DebugProbeSurface.Face, DebugProbeSurfaceCapacity, bDebugProbeSurfaceOccupied);
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
	// Whatever called this may have moved every cell - a new cell size, a new height, a
	// resized cube - so not one piece or square is kept.
	ResetPieces();
	SyncPieces();

	ResetSurfaceMarks();
	SyncSurfaceMarks();

	RebuildDebugGrid();
}

void ARoomManager::UpdateVisualsAfterPlacement()
{
	SyncPieces();
	SyncSurfaceMarks();
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

void ARoomManager::SyncPieces()
{
	if (!HorizontalInteriorPieces || !VerticalInteriorPieces || !EdgePieces || !NodePieces)
	{
		return;
	}

	// Every piece that should be standing, each named by what it stands for. A kind that is
	// switched off is simply left empty, which its layer reads as having been taken away.
	TArray<FPlacedPiece> HorizontalInteriorBoxes;
	TArray<FPlacedPiece> VerticalInteriorBoxes;
	TArray<FPlacedPiece> EdgeBoxes;
	TArray<FPlacedPiece> NodeBoxes;

	if (bShowPieces && PieceMesh && !BuiltPlatforms.IsEmpty())
	{
		const int32 Pitch = GetPitch();
		const double RunExtent = static_cast<double>(Pitch - 1) * CellSize;

		// Everything on the lattice is axis-aligned, so every piece is the unit cube scaled to
		// the cells it fills, and none needs turning.
		auto MakeBox = [this](const FIntVector& MinCell, const FVector& Extent) -> FTransform
		{
			return FTransform(FRotator::ZeroRotator, GetCellMinCorner(MinCell) + 0.5 * Extent, Extent / UnitCubeSize);
		};

		for (const FGridPlatform& Platform : BuiltPlatforms)
		{
			int32 U, V;
			GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

			// One box for the whole interior - a platform is one thing that was placed, so it is
			// drawn as one thing.
			FVector InteriorExtent(CellSize);
			InteriorExtent[U] = RunExtent;
			InteriorExtent[V] = RunExtent;

			TArray<FPlacedPiece>& InteriorBoxes =
				Platform.GetKind() == EPlatformKind::SurfaceHorizontal ? HorizontalInteriorBoxes : VerticalInteriorBoxes;

			InteriorBoxes.Add(FPlacedPiece{
				MakePlatformKey(Platform), MakeBox(Platform.MinNode + AxisStep(U) + AxisStep(V), InteriorExtent) });

			// The rim is drawn per edge and per node, each named by what it is rather than by the
			// platform that brought it. A neighbour sharing one names the same piece, so it is
			// built once, and no two beams - nor a beam and a post - can ever overlap.
			FLatticeEdge Edges[4];
			GetPlatformEdges(Platform, Pitch, Edges);

			for (const FLatticeEdge& Edge : Edges)
			{
				FVector EdgeExtent(CellSize);
				EdgeExtent[Edge.Axis] = RunExtent;

				EdgeBoxes.Add(FPlacedPiece{ MakeEdgeKey(Edge), MakeBox(Edge.Node + AxisStep(Edge.Axis), EdgeExtent) });
			}

			FIntVector Nodes[4];
			GetPlatformNodes(Platform, Pitch, Nodes);

			for (const FIntVector& Node : Nodes)
			{
				NodeBoxes.Add(FPlacedPiece{ MakeCellKey(Node), MakeBox(Node, FVector(CellSize)) });
			}
		}
	}

	SyncPieceLayer(HorizontalInteriorLayer, HorizontalInteriorPieces, HorizontalInteriorBoxes, PieceMesh, ResolveTintedMaterial(
		HorizontalInteriorPieceMaterial, PieceMaterial, PieceColorParameterName, HorizontalInteriorColor));

	SyncPieceLayer(VerticalInteriorLayer, VerticalInteriorPieces, VerticalInteriorBoxes, PieceMesh, ResolveTintedMaterial(
		VerticalInteriorPieceMaterial, PieceMaterial, PieceColorParameterName, VerticalInteriorColor));

	SyncPieceLayer(EdgeLayer, EdgePieces, EdgeBoxes, PieceMesh, ResolveTintedMaterial(
		EdgePieceMaterial, PieceMaterial, PieceColorParameterName, EdgeColor));

	SyncPieceLayer(NodeLayer, NodePieces, NodeBoxes, PieceMesh, ResolveTintedMaterial(
		NodePieceMaterial, PieceMaterial, PieceColorParameterName, NodeColor));
}

void ARoomManager::ResetPieces()
{
	ResetPieceLayer(HorizontalInteriorLayer, HorizontalInteriorPieces);
	ResetPieceLayer(VerticalInteriorLayer, VerticalInteriorPieces);
	ResetPieceLayer(EdgeLayer, EdgePieces);
	ResetPieceLayer(NodeLayer, NodePieces);
}

void ARoomManager::SyncPieceLayer(
	FRoomPieceLayer& Layer,
	UInstancedStaticMeshComponent* FirstChunk,
	const TConstArrayView<FPlacedPiece> Pieces,
	UStaticMesh* Mesh,
	UMaterialInterface* Material)
{
	if (!FirstChunk)
	{
		return;
	}

	// The record is neither saved nor duplicated, while the first chunk is both - so a loaded
	// level or a PIE copy arrives with instances the record knows nothing about. A chunk can
	// also be lost from under it. Either way the two no longer agree: start the layer over.
	bool bChunksIntact = !Layer.Chunks.IsEmpty() && Layer.Chunks[0] == FirstChunk;
	int32 DrawnPieces = 0;

	for (const UInstancedStaticMeshComponent* Chunk : Layer.Chunks)
	{
		if (IsValid(Chunk))
		{
			DrawnPieces += Chunk->GetInstanceCount();
		}
		else
		{
			bChunksIntact = false;
		}
	}

	if (!bChunksIntact || DrawnPieces != Layer.PieceStamps.Num())
	{
		ResetPieceLayer(Layer, FirstChunk);
	}

	for (UInstancedStaticMeshComponent* Chunk : Layer.Chunks)
	{
		Chunk->SetStaticMesh(Mesh);
		Chunk->SetMaterial(0, Material);
	}

	TArray<FTransform> NewPieces;

	// Marks every piece still wanted and collects the ones not drawn yet. Returns false if a
	// piece that was drawn is no longer wanted.
	auto WalkPieces = [&Layer, &Pieces, &NewPieces]() -> bool
	{
		++Layer.SyncStamp;
		int32 WantedPieces = 0;

		for (const FPlacedPiece& Piece : Pieces)
		{
			uint32& Stamp = Layer.PieceStamps.FindOrAdd(Piece.Key);

			// Counted once however often it is named, so the tally below stays honest.
			if (Stamp == Layer.SyncStamp)
			{
				continue;
			}

			if (Stamp == 0)
			{
				NewPieces.Add(Piece.Transform);
			}

			Stamp = Layer.SyncStamp;
			++WantedPieces;
		}

		return WantedPieces == Layer.PieceStamps.Num();
	};

	// Growth only ever adds pieces, so this fails only when something took the structure back -
	// and then the whole layer is drawn again rather than picked apart.
	if (!WalkPieces())
	{
		ResetPieceLayer(Layer, FirstChunk);
		NewPieces.Reset();
		WalkPieces();
	}

	// New pieces go into the chunk still being filled, and a fresh chunk is started once that
	// one is full. Those are the only components whose instance count changes, so they are the
	// only ones whose surface cache Lumen has to capture again.
	for (int32 NextPiece = 0; NextPiece < NewPieces.Num();)
	{
		UInstancedStaticMeshComponent* OpenChunk = Layer.Chunks.Last();
		int32 Room = PieceChunkCapacity - OpenChunk->GetInstanceCount();

		if (Room <= 0)
		{
			OpenChunk = AddPieceChunk(Layer, FirstChunk, Mesh, Material);
			Room = PieceChunkCapacity;
		}

		const int32 Count = FMath::Min(Room, NewPieces.Num() - NextPiece);

		// One batch per chunk: one instance at a time would redo the component's bookkeeping
		// for every instance.
		OpenChunk->AddInstances(
			TArray<FTransform>(NewPieces.GetData() + NextPiece, Count),
			/*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);

		NextPiece += Count;
	}
}

void ARoomManager::ResetPieceLayer(FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk)
{
	for (UInstancedStaticMeshComponent* Chunk : Layer.Chunks)
	{
		if (Chunk != FirstChunk && IsValid(Chunk))
		{
			Chunk->DestroyComponent();
		}
	}

	Layer.Chunks.Reset();
	Layer.PieceStamps.Reset();

	if (FirstChunk)
	{
		if (FirstChunk->GetInstanceCount() > 0)
		{
			FirstChunk->ClearInstances();
		}

		Layer.Chunks.Add(FirstChunk);
	}
}

UInstancedStaticMeshComponent* ARoomManager::AddPieceChunk(
	FRoomPieceLayer& Layer,
	UInstancedStaticMeshComponent* FirstChunk,
	UStaticMesh* Mesh,
	UMaterialInterface* Material)
{
	// Named after the first chunk, so the component list reads EdgePieces, EdgePieces_1, ...
	const FName ChunkName =
		MakeUniqueObjectName(this, UInstancedStaticMeshComponent::StaticClass(), FirstChunk->GetFName());

	// Rebuilt from the platform list whenever it is needed, so never saved, and never copied
	// along with the actor - a PIE or pasted copy starts from its own first chunks and grows its own.
	UInstancedStaticMeshComponent* Chunk = NewObject<UInstancedStaticMeshComponent>(
		this, ChunkName, RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);

	// Whatever makes this kind of piece what it is comes from the first chunk, so the
	// constructor stays the one place each kind is set up.
	Chunk->SetMobility(FirstChunk->Mobility);
	Chunk->SetCollisionEnabled(FirstChunk->GetCollisionEnabled());
	Chunk->SetCastShadow(FirstChunk->CastShadow);
	Chunk->SetStaticMesh(Mesh);
	Chunk->SetMaterial(0, Material);
	Chunk->SetupAttachment(SceneRoot);
	Chunk->RegisterComponent();

	Layer.Chunks.Add(Chunk);
	return Chunk;
}

void ARoomManager::RebuildDebugGrid()
{
	if (!GridLineBatcher)
	{
		return;
	}

	GridLineBatcher->Flush();

	// Only the lattice and the cube are drawn as lines. The surfaces are instanced squares
	// instead: a line batcher hands every line to the renderer again each frame and is rebuilt
	// whole whenever one line changes, which a growing structure's worth of surfaces made the
	// costliest thing in a run.
	if (!bShowDebugGrid)
	{
		return;
	}

	TArray<FBatchedLine> Lines;
	AppendLatticeLines(Lines);

	GridLineBatcher->DrawLines(Lines);
}

void ARoomManager::AppendLatticeLines(TArray<FBatchedLine>& Lines) const
{
	// Actor rotation and scale are ignored: the cube is always axis-aligned to the world.
	const FVector Origin = GetActorLocation();
	const double BaseZ = GridBaseZ;
	const int32 CubeCells = ResolveGridSize();
	const double Extent = CubeCells * CellSize;

	const FLinearColor InteriorColor(GridLineColor);
	const FLinearColor EdgeLineColor(BoundaryLineColor);

	Lines.Reserve(Lines.Num() + 2 * (CubeCells + 1) + 12 + 16);

	// The ground under the cube, as a lattice - one line per gridline - rather than a box per
	// cell: 1026 lines at the 512 cap instead of 262144 boxes. The cube above it is not
	// latticed; a volume of lines would bury everything growing inside it.
	for (int32 X = 0; X <= CubeCells; ++X)
	{
		const double WorldX = Origin.X + X * CellSize;
		Lines.Emplace(
			FVector(WorldX, Origin.Y, BaseZ),
			FVector(WorldX, Origin.Y + Extent, BaseZ),
			InteriorColor, /*LifeTime=*/0.0f, GridLineThickness, SDPG_World);
	}

	for (int32 Y = 0; Y <= CubeCells; ++Y)
	{
		const double WorldY = Origin.Y + Y * CellSize;
		Lines.Emplace(
			FVector(Origin.X, WorldY, BaseZ),
			FVector(Origin.X + Extent, WorldY, BaseZ),
			InteriorColor, /*LifeTime=*/0.0f, GridLineThickness, SDPG_World);
	}

	// The cube's twelve edges on top, thicker and in the boundary colour. Its bottom four trace
	// the ground lattice's outline, so the ground reads as the cube's floor.
	const FVector CubeMin(Origin.X, Origin.Y, BaseZ);
	AppendBoxLines(Lines, CubeMin, CubeMin + FVector(Extent), EdgeLineColor, BoundaryLineThickness, SDPG_World, /*bWithDiagonals=*/false);

	// One cell called out by coordinate. Drawn in the foreground group and crossed through,
	// because inside a cube of them a single 20cm cell is otherwise very hard to pick out.
	if (bHighlightDebugCell && IsValidCell(DebugCell))
	{
		const FBox CellBounds = GetCellBounds(DebugCell);
		AppendBoxLines(Lines, CellBounds.Min, CellBounds.Max, FLinearColor(DebugCellColor), BoundaryLineThickness, SDPG_Foreground, /*bWithDiagonals=*/true);
	}
}

UInstancedStaticMeshComponent* ARoomManager::GetSurfaceMarkLayer(const int32 Layer) const
{
	switch (Layer)
	{
	case AllObjectMarkLayer: return AllObjectSurfaceMarks;
	case WallMarkLayer: return WallSurfaceMarks;
	case OccupiedMarkLayer: return OccupiedSurfaceMarks;
	default: return nullptr;
	}
}

void ARoomManager::ResetSurfaceMarks()
{
	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		UInstancedStaticMeshComponent* Marks = GetSurfaceMarkLayer(Layer);

		if (Marks && Marks->GetInstanceCount() > 0)
		{
			Marks->ClearInstances();
		}

		SurfaceMarkInstanceKeys[Layer].Reset();
	}

	SurfaceMarkSlots.Reset();
}

void ARoomManager::RemoveSurfaceMark(const int32 Layer, const int32 Instance)
{
	UInstancedStaticMeshComponent* Marks = GetSurfaceMarkLayer(Layer);

	if (!Marks || !SurfaceMarkInstanceKeys[Layer].IsValidIndex(Instance))
	{
		return;
	}

	TArray<uint64>& Keys = SurfaceMarkInstanceKeys[Layer];
	const int32 Last = Keys.Num() - 1;

	// Only the last instance is ever removed, so no other square changes index and the record
	// needs one fix-up at most: the last square is moved into the hole first. The component
	// has a swap-removal of its own, but it is free to ignore being asked for it, which would
	// leave this record guessing.
	if (Instance != Last)
	{
		FTransform LastTransform;
		Marks->GetInstanceTransform(Last, LastTransform, /*bWorldSpace=*/true);
		Marks->UpdateInstanceTransform(Instance, LastTransform, /*bWorldSpace=*/true);

		Keys[Instance] = Keys[Last];
		SurfaceMarkSlots.FindChecked(Keys[Instance]).Instance = Instance;
	}

	Marks->RemoveInstance(Last);
	Keys.Pop(EAllowShrinking::No);
}

void ARoomManager::SyncSurfaceMarks()
{
	UInstancedStaticMeshComponent* const MarkLayers[SurfaceMarkLayerCount] =
		{ AllObjectSurfaceMarks, WallSurfaceMarks, OccupiedSurfaceMarks };

	for (const UInstancedStaticMeshComponent* Marks : MarkLayers)
	{
		if (!Marks)
		{
			return;
		}
	}

	// Each surface is drawn as its own inset square, so a run of them reads as a row of tiles
	// rather than one unbroken sheet of colour.
	const double Inset = CellSize * SurfaceCellInsetFraction;
	const double Span = CellSize - 2.0 * Inset;

	if (!bShowSurfaces || !PlaneMesh || BuiltPlatforms.IsEmpty() || Span <= 0.0)
	{
		ResetSurfaceMarks();
		return;
	}

	// The record is neither saved nor duplicated with the components, so a loaded level or a
	// PIE copy arrives with squares it knows nothing about. Start clean rather than add to them.
	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		if (MarkLayers[Layer]->GetInstanceCount() != SurfaceMarkInstanceKeys[Layer].Num())
		{
			ResetSurfaceMarks();
			break;
		}
	}

	const FColor LayerColors[SurfaceMarkLayerCount] =
		{ AllObjectSurfaceColor, WallSurfaceColor, OccupiedSurfaceColor };

	TObjectPtr<UMaterialInstanceDynamic>* const LayerMaterials[SurfaceMarkLayerCount] =
		{ &AllObjectMarkMaterial, &WallMarkMaterial, &OccupiedMarkMaterial };

	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		MarkLayers[Layer]->SetStaticMesh(PlaneMesh);
		MarkLayers[Layer]->SetMaterial(0, ResolveTintedMaterial(
			*LayerMaterials[Layer], PlaneMaterial, PlaneColorParameterName, FLinearColor(LayerColors[Layer])));
	}

	const FVector MarkScale(Span / UnitPlaneSize, Span / UnitPlaneSize, 1.0);
	const int32 Pitch = GetPitch();

	TArray<FTransform> NewMarks[SurfaceMarkLayerCount];
	TArray<uint64> NewMarkKeys[SurfaceMarkLayerCount];

	// One pass over every surface. A square already the right colour is left alone; one whose
	// surface has changed - a seam promoted into floorspace, a wall folded off it - moves to the
	// right colour; a surface that has appeared since the last pass gets a new square. Returns
	// false if the record holds a surface the pass no longer found.
	auto WalkSurfaces = [&]() -> bool
	{
		++SurfaceMarkSyncStamp;
		int32 FoundSurfaces = 0;

		auto SyncSurface = [&](const FIntVector& Cell, const EGridFace Face)
		{
			const uint64 Key = MakeFaceKey(Cell, Face);
			FSurfaceMarkSlot& Slot = SurfaceMarkSlots.FindOrAdd(Key);

			// Counted once however often the platforms name it, so the tally below stays honest.
			if (Slot.SeenStamp == SurfaceMarkSyncStamp)
			{
				return;
			}

			Slot.SeenStamp = SurfaceMarkSyncStamp;
			++FoundSurfaces;

			ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
			bool bOccupied = false;
			ResolveSurface(Cell, Face, Capacity, bOccupied);

			// Occupancy is drawn instead of the capacity, since what a surface is free to take is
			// the more useful reading and a built-against surface has stopped offering it.
			const int32 Layer = bOccupied
				? OccupiedMarkLayer
				: (Capacity == ESurfaceCapacity::AllObject ? AllObjectMarkLayer : WallMarkLayer);

			if (Slot.Layer == Layer)
			{
				return;
			}

			if (Slot.Layer != INDEX_NONE)
			{
				RemoveSurfaceMark(Slot.Layer, Slot.Instance);
			}

			Slot.Layer = Layer;
			Slot.Instance = INDEX_NONE;

			// Lying on the face, turned to look out along its normal, and stood off it a little
			// so it never z-fights the box it marks.
			const FTransform FaceTransform = GetSurfaceWorldTransform(Cell, Face);
			const FVector Outward(FaceStep(Face));

			NewMarks[Layer].Emplace(
				FaceTransform.GetRotation(), FaceTransform.GetLocation() + Outward * SurfaceOverlayBias, MarkScale);
			NewMarkKeys[Layer].Add(Key);
		};

		// Every surface is one of the two broad faces of a platform's cell, so walking the
		// platforms finds every one of them - without asking the cube's millions of cells, nearly
		// all empty, one at a time.
		for (const FGridPlatform& Platform : BuiltPlatforms)
		{
			const int32 NormalAxis = AxisIndex(Platform.Normal);
			const EGridFace FrontFace = MakeFace(NormalAxis, 1);
			const EGridFace BackFace = MakeFace(NormalAxis, -1);

			ForEachPlatformCell(Platform, Pitch, [&SyncSurface, FrontFace, BackFace](const FIntVector& Cell, bool)
			{
				SyncSurface(Cell, FrontFace);
				SyncSurface(Cell, BackFace);
			});
		}

		return FoundSurfaces == SurfaceMarkSlots.Num();
	};

	// Placement only ever adds, so a surface once found stays a surface and a square never has
	// to be taken away. If that stops holding the tally will not match: redraw from scratch.
	if (!WalkSurfaces())
	{
		ResetSurfaceMarks();

		for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
		{
			NewMarks[Layer].Reset();
			NewMarkKeys[Layer].Reset();
		}

		WalkSurfaces();
	}

	// Each colour's new squares go in as one batch, for the same reason the pieces do.
	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		if (NewMarks[Layer].IsEmpty())
		{
			continue;
		}

		TArray<uint64>& Keys = SurfaceMarkInstanceKeys[Layer];
		const int32 FirstInstance = Keys.Num();

		MarkLayers[Layer]->AddInstances(NewMarks[Layer], /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);

		for (int32 Offset = 0; Offset < NewMarkKeys[Layer].Num(); ++Offset)
		{
			SurfaceMarkSlots.FindChecked(NewMarkKeys[Layer][Offset]).Instance = FirstInstance + Offset;
		}

		Keys.Append(NewMarkKeys[Layer]);
	}
}
