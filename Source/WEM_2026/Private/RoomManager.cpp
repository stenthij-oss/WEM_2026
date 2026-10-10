// Copyright Epic Games, Inc. All Rights Reserved.

#include "RoomManager.h"

#include "GridMath.h"
#include "PlatformStructureData.h"
#include "PlatformTileData.h"
#include "StructureCategory.h"
#include "StructurePlacement.h"
#include "Algo/BinarySearch.h"
#include "Algo/Count.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LineBatchComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

using namespace WEMGrid;

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
	 * Most squares one chunk of an overlay colour holds before a fresh chunk is started.
	 *
	 * The overlay is kept out of Lumen, so what a changed chunk costs is rebuilding its bounds and
	 * render data - a pass over every instance it holds. Larger than the piece chunks, since a
	 * platform brings well over a hundred squares and only nine pieces.
	 */
	constexpr int32 SurfaceMarkChunkCapacity = 4096;

	/**
	 * How many placements' worth of changed surfaces are kept for GatherSurfaceChangesSince: once
	 * twice this many are held, the older half is let go. A reader that falls further behind than
	 * that - one beat of the object placer at its slowest interval, many times over - walks the
	 * whole structure instead.
	 */
	constexpr int32 SurfaceJournalPlacements = 1024;

	TAutoConsoleVariable<int32> CVarRoomManagerVerifyEvery(
		TEXT("wem.RoomManager.VerifyEvery"),
		0,
		TEXT("Every Nth placement, rebuilds ARoomManager's derived state and visuals from scratch and logs anything the\n")
		TEXT("placements had built up differently. Costs a full rebuild each time, so leave it at 0 (off) outside of testing."),
		ECVF_Default);

	/** The ways round a tile can be laid, one bit each, as GetFillableWays reports them. */
	constexpr uint8 AsLaidWay = 1 << 0;
	constexpr uint8 TurnedWay = 1 << 1;

	/** A one-sided structure on a platform's negative face is keyed apart from the one on its positive face. */
	constexpr uint64 NegativeFacePieceBit = uint64(1) << 63;

	/** Where one face of a platform is kept in ARoomManager::FaceFills: its positive face, then its negative one. */
	FORCEINLINE int32 GetFaceFillIndex(const int32 PlatformIndex, const int32 FaceSign)
	{
		return PlatformIndex * 2 + (FaceSign > 0 ? 0 : 1);
	}

	/**
	 * The four half-planes around a module edge in the order a quarter turn about the edge's axis
	 * walks them - +A, +B, -A, -B, where A and B are the next two axes after the edge's - as indices
	 * into GetEdgeFaces, which lists them +B, -B, +A, -A.
	 */
	constexpr int32 TurningOrder[4] = { 2, 0, 3, 1 };

	FORCEINLINE int32 AxisIndex(const EGridAxis Axis)
	{
		return static_cast<int32>(Axis);
	}

	FORCEINLINE uint8 AxisBit(const int32 Axis)
	{
		return static_cast<uint8>(1 << Axis);
	}

	FORCEINLINE FGridPlatform MakePlatform(
		const FIntVector& MinNode,
		const int32 NormalAxis,
		UPlatformTileData* Tile,
		const bool bTurned)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = static_cast<EGridAxis>(NormalAxis);
		Platform.Tile = Tile;
		Platform.bTurned = bTurned;
		return Platform;
	}

	/** A module face is named by its min node and the axis it faces along. */
	FORCEINLINE uint64 MakeModuleFaceKey(const FIntVector& MinNode, const int32 NormalAxis)
	{
		return MakeTaggedKey(MinNode, NormalAxis);
	}

	/** A platform is named by its min module face, which no other platform can ever cover. */
	FORCEINLINE uint64 MakePlatformKey(const FGridPlatform& Platform)
	{
		return MakeModuleFaceKey(Platform.MinNode, AxisIndex(Platform.Normal));
	}

	/**
	 * A placement of a tile: its platform key, with which way round it lies in the key's top bit,
	 * which no tag reaches. Each tile keeps lists of its own, so the tile needs no bits here.
	 */
	FORCEINLINE uint64 MakeCandidateKey(const FGridPlatform& Candidate)
	{
		return MakePlatformKey(Candidate) | (Candidate.bTurned ? (uint64(1) << 63) : 0);
	}

	/** One module edge: the node its run starts from, and the axis the run heads along. */
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

	/** One module face: the node at its min corner, and the axis it faces along. */
	struct FModuleFace
	{
		FIntVector MinNode = FIntVector::ZeroValue;
		int32 Normal = 0;
	};

	/**
	 * Calls Visit(MinNode) for every module face a platform covers, given its spans along its
	 * first and second in-plane axes.
	 */
	template <typename FunctorType>
	void ForEachModuleFace(
		const FGridPlatform& Platform,
		const int32 SpanU,
		const int32 SpanV,
		const int32 Module,
		FunctorType&& Visit)
	{
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		for (int32 StepV = 0; StepV < SpanV; StepV += Module)
		{
			for (int32 StepU = 0; StepU < SpanU; StepU += Module)
			{
				Visit(Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV));
			}
		}
	}

	/**
	 * Calls Visit(Edge, bRim) for every module edge a platform takes in: those around it, which
	 * are its rim, and those across it, which its interior bridges.
	 */
	template <typename FunctorType>
	void ForEachModuleEdge(
		const FGridPlatform& Platform,
		const int32 SpanU,
		const int32 SpanV,
		const int32 Module,
		FunctorType&& Visit)
	{
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		// The edges running along U, on every line of nodes across V...
		for (int32 StepV = 0; StepV <= SpanV; StepV += Module)
		{
			for (int32 StepU = 0; StepU < SpanU; StepU += Module)
			{
				Visit(FLatticeEdge{ Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV), U }, StepV == 0 || StepV == SpanV);
			}
		}

		// ...and the edges running along V, on every line of nodes across U.
		for (int32 StepU = 0; StepU <= SpanU; StepU += Module)
		{
			for (int32 StepV = 0; StepV < SpanV; StepV += Module)
			{
				Visit(FLatticeEdge{ Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV), V }, StepU == 0 || StepU == SpanU);
			}
		}
	}

	/** Calls Visit(Node) for every lattice node around a platform: its corners, and every node along its rim between them. */
	template <typename FunctorType>
	void ForEachRimNode(
		const FGridPlatform& Platform,
		const int32 SpanU,
		const int32 SpanV,
		const int32 Module,
		FunctorType&& Visit)
	{
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		for (int32 StepV = 0; StepV <= SpanV; StepV += Module)
		{
			for (int32 StepU = 0; StepU <= SpanU; StepU += Module)
			{
				if (StepU == 0 || StepU == SpanU || StepV == 0 || StepV == SpanV)
				{
					Visit(Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV));
				}
			}
		}
	}

	/**
	 * The four module faces that meet along one module edge, two in each plane that contains it.
	 * For an edge along X, the faces facing Z sit either side of it along Y, and the faces facing
	 * Y sit either side of it along Z. Some may fall outside the cube; the caller checks.
	 */
	void GetEdgeFaces(const FLatticeEdge& Edge, const int32 Module, FModuleFace (&OutFaces)[4])
	{
		const int32 AxisA = (Edge.Axis + 1) % 3;
		const int32 AxisB = (Edge.Axis + 2) % 3;

		OutFaces[0] = { Edge.Node, AxisA };
		OutFaces[1] = { Edge.Node - AxisStep(AxisB, Module), AxisA };
		OutFaces[2] = { Edge.Node, AxisB };
		OutFaces[3] = { Edge.Node - AxisStep(AxisA, Module), AxisB };
	}

	/**
	 * The edge whose run a cell lies on: a cell on exactly two lattice planes, which leaves one
	 * axis it runs along. False for a node, an interior cell, or a cell off the lattice.
	 */
	bool GetRunEdge(const FIntVector& Cell, const int32 Module, FLatticeEdge& OutEdge)
	{
		int32 RunAxis = INDEX_NONE;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Cell[Axis] % Module != 0)
			{
				if (RunAxis != INDEX_NONE)
				{
					return false;
				}

				RunAxis = Axis;
			}
		}

		if (RunAxis == INDEX_NONE)
		{
			return false;
		}

		OutEdge.Node = Cell;
		OutEdge.Node[RunAxis] -= Cell[RunAxis] % Module;
		OutEdge.Axis = RunAxis;
		return true;
	}

	/** The first lattice line at or above a coordinate. Never below zero, where the lattice starts. */
	FORCEINLINE int32 FirstNodeAtOrAbove(const int32 Coordinate, const int32 Module)
	{
		return Coordinate <= 0 ? 0 : ((Coordinate + Module - 1) / Module) * Module;
	}

	/**
	 * Calls VisitFace(MinNode, NormalAxis), VisitEdge(Edge) and VisitNode(Node) for every lattice
	 * piece an inclusive box of cells cuts. A module face is cut when the box reaches into its
	 * inside - the cells between its edges - on its own plane; a module edge when the box holds its
	 * line and reaches into its run between the nodes; a node when the box holds it. An edge lies in
	 * two lattice planes, and may be visited from both.
	 */
	template <typename FaceFunctorType, typename EdgeFunctorType, typename NodeFunctorType>
	void ForEachObstaclePiece(
		const FIntVector& Min,
		const FIntVector& Max,
		const int32 Module,
		FaceFunctorType&& VisitFace,
		EdgeFunctorType&& VisitEdge,
		NodeFunctorType&& VisitNode)
	{
		for (int32 Z = FirstNodeAtOrAbove(Min.Z, Module); Z <= Max.Z; Z += Module)
		{
			for (int32 Y = FirstNodeAtOrAbove(Min.Y, Module); Y <= Max.Y; Y += Module)
			{
				for (int32 X = FirstNodeAtOrAbove(Min.X, Module); X <= Max.X; X += Module)
				{
					VisitNode(FIntVector(X, Y, Z));
				}
			}
		}

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			int32 U, V;
			GetInPlaneAxes(Axis, U, V);

			// A run or an inside starting at a node reaches Module - 1 cells past it, so a node that
			// far short of the box still has cells in it.
			const int32 FirstU = FirstNodeAtOrAbove(Min[U] - Module + 1, Module);
			const int32 FirstV = FirstNodeAtOrAbove(Min[V] - Module + 1, Module);

			for (int32 Plane = FirstNodeAtOrAbove(Min[Axis], Module); Plane <= Max[Axis]; Plane += Module)
			{
				for (int32 NodeV = FirstV; NodeV <= Max[V] - 1; NodeV += Module)
				{
					for (int32 NodeU = FirstU; NodeU <= Max[U] - 1; NodeU += Module)
					{
						VisitFace(AxisStep(Axis, Plane) + AxisStep(U, NodeU) + AxisStep(V, NodeV), Axis);
					}
				}

				// Edges along U lie on the lines across V that the box holds, and the other way round.
				for (int32 LineV = FirstNodeAtOrAbove(Min[V], Module); LineV <= Max[V]; LineV += Module)
				{
					for (int32 NodeU = FirstU; NodeU <= Max[U] - 1; NodeU += Module)
					{
						VisitEdge(FLatticeEdge{ AxisStep(Axis, Plane) + AxisStep(U, NodeU) + AxisStep(V, LineV), U });
					}
				}

				for (int32 LineU = FirstNodeAtOrAbove(Min[U], Module); LineU <= Max[U]; LineU += Module)
				{
					for (int32 NodeV = FirstV; NodeV <= Max[V] - 1; NodeV += Module)
					{
						VisitEdge(FLatticeEdge{ AxisStep(Axis, Plane) + AxisStep(U, LineU) + AxisStep(V, NodeV), V });
					}
				}
			}
		}
	}

	FORCEINLINE FGridSurfaceRef MakeSurfaceRef(const FIntVector& Cell, const EGridFace Face)
	{
		FGridSurfaceRef Surface;
		Surface.Cell = Cell;
		Surface.Face = Face;
		return Surface;
	}

	FORCEINLINE int32 KindIndex(const EPlatformKind Kind)
	{
		return static_cast<int32>(Kind);
	}

	/**
	 * Calls Visit(Cell, bRim) for every cell of a platform - interior, edges and nodes alike -
	 * given its spans along its first and second in-plane axes.
	 */
	template <typename FunctorType>
	void ForEachPlatformCell(const FGridPlatform& Platform, const int32 SpanU, const int32 SpanV, FunctorType&& Visit)
	{
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		for (int32 StepV = 0; StepV <= SpanV; ++StepV)
		{
			for (int32 StepU = 0; StepU <= SpanU; ++StepU)
			{
				const bool bRim = StepU == 0 || StepU == SpanU || StepV == 0 || StepV == SpanV;
				Visit(Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV), bRim);
			}
		}
	}

	/**
	 * Calls Visit(TileIndex, Candidate, bOnRim) for every placement of every tile in Tiles whose
	 * footprint takes in one module edge: in either plane that contains the edge, each way round
	 * the tile may lie, at every module step that still covers the edge. bOnRim says whether the
	 * edge runs around the placement rather than across it; with bRimOnly, only those are
	 * visited. Every tile has to be usable on the lattice. Placements may fall outside the cube;
	 * the caller checks.
	 */
	template <typename FunctorType>
	void ForEachPlacementAround(
		const FLatticeEdge& Edge,
		const TConstArrayView<TObjectPtr<UPlatformTileData>> Tiles,
		const int32 Module,
		const bool bRimOnly,
		FunctorType&& Visit)
	{
		for (const int32 NormalAxis : { (Edge.Axis + 1) % 3, (Edge.Axis + 2) % 3 })
		{
			// Within that plane the edge runs along one axis, and a placement reaches away from it
			// along the other.
			const int32 AcrossAxis = 3 - NormalAxis - Edge.Axis;

			int32 U, V;
			GetInPlaneAxes(NormalAxis, U, V);

			for (int32 TileIndex = 0; TileIndex < Tiles.Num(); ++TileIndex)
			{
				UPlatformTileData* Tile = Tiles[TileIndex];

				for (const bool bTurned : { false, true })
				{
					if (bTurned && (!Tile->bCanTurn || Tile->Width == Tile->Length))
					{
						continue;
					}

					const int32 SpanU = bTurned ? Tile->Length : Tile->Width;
					const int32 SpanV = bTurned ? Tile->Width : Tile->Length;
					const int32 SpanAlong = Edge.Axis == U ? SpanU : SpanV;
					const int32 SpanAcross = AcrossAxis == U ? SpanU : SpanV;

					// Across, the edge is either one of the placement's two sides - a step of
					// SpanAcross goes from the one straight to the other - or any line between.
					const int32 AcrossStep = bRimOnly ? SpanAcross : Module;

					for (int32 Along = 0; Along < SpanAlong; Along += Module)
					{
						for (int32 Across = 0; Across <= SpanAcross; Across += AcrossStep)
						{
							const FIntVector MinNode = Edge.Node - AxisStep(Edge.Axis, Along) - AxisStep(AcrossAxis, Across);

							Visit(TileIndex, MakePlatform(MinNode, NormalAxis, Tile, bTurned), Across == 0 || Across == SpanAcross);
						}
					}
				}
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

	/** Puts a mesh and a material on every chunk. Both setters return at once when nothing changes. */
	void ApplyMeshAndMaterial(
		const TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Chunks,
		UStaticMesh* Mesh,
		UMaterialInterface* Material)
	{
		for (UInstancedStaticMeshComponent* Chunk : Chunks)
		{
			if (IsValid(Chunk))
			{
				Chunk->SetStaticMesh(Mesh);
				Chunk->SetMaterial(0, Material);
			}
		}
	}

	/** Edge length of one overlay square: a cell, less the inset on both sides. */
	FORCEINLINE double GetSurfaceMarkSpan(const double CellSize)
	{
		return CellSize * (1.0 - 2.0 * SurfaceCellInsetFraction);
	}

	/** Scale that turns the engine's unit plane into one overlay square. */
	FORCEINLINE FVector MakeSurfaceMarkScale(const double CellSize)
	{
		const double Span = GetSurfaceMarkSpan(CellSize);

		return FVector(Span / UnitPlaneSize, Span / UnitPlaneSize, 1.0);
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
	// these two is only the first chunk of its kind; SyncPieceLayer adds more as the structure
	// grows, set up the way these are set up here. Each structure's layer starts from a chunk set up
	// like the edges.
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

	for (UInstancedStaticMeshComponent* Pieces : { EdgePieces.Get(), NodePieces.Get() })
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
	// The module comes first, since the cube is rounded to whole modules of it.
	LatticeModule = GetModule();
	GridSize = ResolveGridSize();

	// One height sample for the whole cube, resolved before anything asks for a cell position.
	GridBaseZ = ResolveGridBaseZ();

	// Whatever stood on the structure before this may no longer stand on anything - the cells may
	// all have moved - so it goes, and nothing it held back holds growth back any more.
	BeginNewGeneration();

	// Everything derived is rebuilt from the list, so resizing the cube, changing the module or
	// swapping a tile re-resolves it from scratch.
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
		BeginNewGeneration();

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

	// Two coplanar platforms both name the rim they share, so a rim surface is listed only the
	// first time. Interiors are never shared and are not tracked.
	TSet<uint64> ListedRimSurfaces;

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		const int32 NormalAxis = AxisIndex(Platform.Normal);

		int32 SpanU, SpanV;
		GetPlatformSpans(Platform, SpanU, SpanV);

		ForEachPlatformCell(Platform, SpanU, SpanV, [&](const FIntVector& Cell, const bool bRim)
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

bool ARoomManager::CanPlacePlatform(const FGridPlatform& Platform) const
{
	return Platform.Tile
		&& Platform.Tile->AllowsOrientation(Platform.GetKind() == EPlatformKind::SurfaceVertical)
		&& IsPlacementOpen(Platform);
}

bool ARoomManager::IsPlacementOpen(const FGridPlatform& Platform) const
{
	// On the lattice, laid from a usable tile, with the whole footprint - interior, rim and far
	// nodes - inside the cube. That alone keeps anything from going below the ground layer.
	if (!IsPlatformOnGrid(Platform))
	{
		return false;
	}

	// With nothing placed there is nothing to grow from, so the first platform has a rule of
	// its own: FirstTile, horizontal, at the centre of the centre layer.
	if (BuiltPlatforms.IsEmpty())
	{
		TArray<FGridPlatform> FirstCandidates;
		GatherFirstPlatformCandidates(FirstCandidates);

		return FirstCandidates.ContainsByPredicate([&Platform](const FGridPlatform& Candidate)
		{
			return Candidate.Tile == Platform.Tile && MakeCandidateKey(Candidate) == MakeCandidateKey(Platform);
		});
	}

	if (!IsSpaceFree(Platform))
	{
		return false;
	}

	int32 SpanU, SpanV;
	GetPlatformSpans(Platform, SpanU, SpanV);

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	bool bTouchesStructure = false;
	bool bFlush = true;
	bool bFoldsLegally = true;

	// The placement is judged against every platform it would share a module edge with, not only
	// the one it was found from. It has to lie flush with each of them. Continuing a platform in
	// its own plane asks nothing more, but a single fold that is refused refuses the whole
	// placement - otherwise a wall carrying on from one already standing could be raised over a
	// seam that has opened, closing it again.
	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		// Only its rim can be shared, and only where some platform already runs around the same
		// edge. IsSpaceFree has already seen to it that none runs across it.
		if (!bRim || !bFlush || !bFoldsLegally || !ModuleEdgeUses.Contains(MakeEdgeKey(Edge)))
		{
			return;
		}

		bTouchesStructure = true;

		FModuleFace Faces[4];
		GetEdgeFaces(Edge, Module, Faces);

		bool bFoldChecked = false;

		for (const FModuleFace& Face : Faces)
		{
			const int32* Owner = ModuleFaceOwners.Find(MakeModuleFaceKey(Face.MinNode, Face.Normal));

			if (!Owner)
			{
				continue;
			}

			// Every platform met along the edge, whichever way it faces, has to be met flush.
			if (!IsFlushWith(Platform, BuiltPlatforms[*Owner], Edge.Axis))
			{
				bFlush = false;
				return;
			}

			// A neighbour on the other side of this placement's plane would be folded off from
			// the same run toward the same side, so one check answers for both.
			if (Face.Normal == NormalAxis || bFoldChecked)
			{
				continue;
			}

			bFoldChecked = true;

			// A fold leaves the edge along the neighbour's normal, to one side of it or the
			// other. Every cell of the edge's run has to be offering itself on that side: wall
			// surface, not yet built against. A seam that has opened reads as all-object and a
			// side already folded off reads as occupied, and either refuses. The nodes at the
			// run's ends are not asked, since a node is shared by whatever meets at it.
			const int32 FoldAxis = Face.Normal;
			const EGridFace Toward = MakeFace(FoldAxis, Platform.MinNode[FoldAxis] == Edge.Node[FoldAxis] ? 1 : -1);

			for (int32 RunStep = 1; RunStep < Module && bFoldsLegally; ++RunStep)
			{
				ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
				bool bOccupied = false;
				ResolveSurface(Edge.Node + AxisStep(Edge.Axis, RunStep), Toward, Capacity, bOccupied);

				bFoldsLegally = Capacity == ESurfaceCapacity::Wall && !bOccupied;
			}
		}
	});

	// A placement touching nothing would start a second structure; growth only ever extends the one.
	return bFlush && bFoldsLegally && bTouchesStructure;
}

void ARoomManager::GatherTouchingPlatforms(const FGridPlatform& Platform, TArray<int32, TInlineAllocator<8>>& OutTouching) const
{
	OutTouching.Reset();

	int32 SpanU, SpanV;
	if (!GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return;
	}

	const int32 Module = GetModule();

	// Only its rim can be shared, and only where some platform already runs around the same edge.
	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		if (!bRim || !ModuleEdgeUses.Contains(MakeEdgeKey(Edge)))
		{
			return;
		}

		FModuleFace Faces[4];
		GetEdgeFaces(Edge, Module, Faces);

		for (const FModuleFace& Face : Faces)
		{
			if (const int32* Owner = ModuleFaceOwners.Find(MakeModuleFaceKey(Face.MinNode, Face.Normal)))
			{
				OutTouching.AddUnique(*Owner);
			}
		}
	});

	// In index order, so a draw among them depends only on which platforms stand.
	OutTouching.Sort();
}

EGridFace ARoomManager::ChooseUp(const FGridPlatform& Platform)
{
	TArray<int32, TInlineAllocator<8>> Touching;
	GatherTouchingPlatforms(Platform, Touching);

	int32 SpanU, SpanV;
	if (Touching.IsEmpty() || !GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return EGridFace::PosZ;
	}

	const int32 NormalAxis = AxisIndex(Platform.Normal);

	auto CanFill = [&](const EGridFace Up)
	{
		return HasCompleteFill(NormalAxis, Up, SpanU, SpanV, /*bLoadBearingOnly=*/false);
	};

	// It grows off one of the platforms it touches, and takes that one's Up.
	const int32 Parent = Touching[Touching.Num() > 1 ? RoleStream.RandRange(0, Touching.Num() - 1) : 0];
	const EGridFace Up = BuiltPlatforms[Parent].Up;

	if (CanFill(Up))
	{
		return Up;
	}

	// A way up the structures cannot fill is no use. Another platform it touches may offer one they can,
	// and the world's always can: the placement was only drawn because it can be filled that way.
	for (const int32 Other : Touching)
	{
		if (CanFill(BuiltPlatforms[Other].Up))
		{
			return BuiltPlatforms[Other].Up;
		}
	}

	return EGridFace::PosZ;
}

void ARoomManager::GatherFirstPlatformCandidates(TArray<FGridPlatform>& OutCandidates) const
{
	OutCandidates.Reset();

	// The first platform is always laid flat, so a first tile kept to walls starts nothing.
	if (!IsTileUsable(FirstTile) || !FirstTile->AllowsOrientation(/*bVertical=*/false))
	{
		return;
	}

	const int32 Module = GetModule();
	const int32 ModulesPerSide = (ResolveGridSize() - 1) / Module;
	const int32 Layer = GetFirstPlatformLayer();

	for (const bool bTurned : { false, true })
	{
		if (bTurned && (!FirstTile->bCanTurn || FirstTile->Width == FirstTile->Length))
		{
			continue;
		}

		// Counted in modules from the cube's corner. Centred on the layer, so the structure starts
		// at the heart of the cube and has as far to grow on every side before it reaches the
		// faces. Where the tile and the cube differ by an odd number of modules the centre falls
		// between two positions, and both are offered.
		const int32 SpanX = (bTurned ? FirstTile->Length : FirstTile->Width) / Module;
		const int32 SpanY = (bTurned ? FirstTile->Width : FirstTile->Length) / Module;

		if (SpanX > ModulesPerSide || SpanY > ModulesPerSide)
		{
			continue;
		}

		// It holds up whatever grows off it, so it is laid only a way round that load-bearing
		// structures can fill.
		if (!HasCompleteFill(AxisIndex(EGridAxis::Z), EGridFace::PosZ, SpanX * Module, SpanY * Module, /*bLoadBearingOnly=*/true))
		{
			continue;
		}

		for (int32 SlotY = (ModulesPerSide - SpanY) / 2; SlotY <= (ModulesPerSide - SpanY + 1) / 2; ++SlotY)
		{
			for (int32 SlotX = (ModulesPerSide - SpanX) / 2; SlotX <= (ModulesPerSide - SpanX + 1) / 2; ++SlotX)
			{
				OutCandidates.Add(MakePlatform(
					FIntVector(SlotX * Module, SlotY * Module, Layer), AxisIndex(EGridAxis::Z), FirstTile, bTurned));
			}
		}
	}
}

void ARoomManager::GatherPlatformCandidates()
{
	TileCandidates.Reset();
	TileCandidates.SetNum(UsableTiles.Num());

	for (int32 TileIndex = 0; TileIndex < UsableTiles.Num(); ++TileIndex)
	{
		TileCandidates[TileIndex].Tile = UsableTiles[TileIndex];
	}

	// The empty cube is the one case with nothing to grow from. Its first platform is drawn from
	// a list of its own.
	if (BuiltPlatforms.IsEmpty())
	{
		return;
	}

	const int32 Module = GetModule();

	// Neighbouring platforms share edges, and so share placements; each is judged once.
	TArray<TSet<uint64>> Considered;
	Considered.SetNum(UsableTiles.Num());

	// Every placement around every edge of every platform goes into one draw, so the structure
	// grows wherever there is room rather than finishing one part before starting the next.
	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		int32 SpanU, SpanV;
		GetPlatformSpans(Platform, SpanU, SpanV);

		ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
		{
			if (!bRim)
			{
				return;
			}

			ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/true,
				[&](const int32 TileIndex, const FGridPlatform& Candidate, bool)
			{
				bool bAlreadyConsidered = false;
				Considered[TileIndex].Add(MakeCandidateKey(Candidate), &bAlreadyConsidered);

				if (!bAlreadyConsidered && IsPlacementOpen(Candidate))
				{
					TileCandidates[TileIndex].ByKind[KindIndex(Candidate.GetKind())].Add(Candidate);
				}
			});
		});
	}

	// Drawn from in key order rather than the order they were found in, so what the stream picks
	// depends only on which platforms stand - not on the order the list happens to hold them.
	for (FTileCandidates& Candidates : TileCandidates)
	{
		for (TArray<FGridPlatform>& KindCandidates : Candidates.ByKind)
		{
			KindCandidates.Sort([](const FGridPlatform& A, const FGridPlatform& B)
			{
				return MakeCandidateKey(A) < MakeCandidateKey(B);
			});
		}
	}
}

void ARoomManager::EnsurePlatformCandidates()
{
	if (!bPlatformCandidatesStale)
	{
		return;
	}

	GatherPlatformCandidates();

	bPlatformCandidatesStale = false;
}

bool ARoomManager::RefreshPlatformCandidate(
	const int32 TileIndex,
	const FGridPlatform& Candidate,
	const bool bMayHaveComeFree)
{
	TArray<FGridPlatform>& Candidates = TileCandidates[TileIndex].ByKind[KindIndex(Candidate.GetKind())];

	const uint64 Key = MakeCandidateKey(Candidate);
	const int32 Index = Algo::LowerBoundBy(Candidates, Key, [](const FGridPlatform& Listed)
	{
		return MakeCandidateKey(Listed);
	});

	const bool bListed = Candidates.IsValidIndex(Index) && MakeCandidateKey(Candidates[Index]) == Key;

	// Growth only ever takes room away, except where it brings a placement something to grow off.
	// So a placement that is not listed stays unlisted unless it may just have gained that.
	if (!bListed && !bMayHaveComeFree)
	{
		return false;
	}

	const bool bFree = IsPlacementOpen(Candidate);

	// Inserted where it sorts, so the list stays in the key order a full gather would give it.
	if (bFree && !bListed)
	{
		Candidates.Insert(Candidate, Index);
	}
	else if (!bFree && bListed)
	{
		Candidates.RemoveAt(Index);
	}

	return true;
}

bool ARoomManager::PlacePlatformOfKind(const EPlatformKind Kind)
{
	FGridPlatform Platform;

	if (BuiltPlatforms.IsEmpty())
	{
		// The empty cube is the one case with nothing to grow from, and only FirstTile, laid
		// horizontal, starts it - whatever its weight.
		if (Kind != EPlatformKind::SurfaceHorizontal)
		{
			return false;
		}

		TArray<FGridPlatform> FirstCandidates;
		GatherFirstPlatformCandidates(FirstCandidates);

		if (FirstCandidates.IsEmpty())
		{
			return false;
		}

		Platform = FirstCandidates[PlacementStream.RandRange(0, FirstCandidates.Num() - 1)];
	}
	else
	{
		EnsurePlatformCandidates();

		// A placement is only drawn laid a way round that the structures can fill. Where that is
		// every way the tile is listed, every placement counts.
		auto IsFillable = [this, Kind](const int32 TileIndex, const FGridPlatform& Candidate)
		{
			return (GetFillableWays(TileIndex, Kind) & (Candidate.bTurned ? TurnedWay : AsLaidWay)) != 0;
		};

		// The tile is rolled only among those with somewhere to go, so a tile with no room left
		// never costs the beat while another has some. A tile kept to the other orientation has
		// nowhere to go either, and nor has one with room only laid a way no structures can fill;
		// both are asked here rather than when the lists are kept, so a change to either takes
		// effect on the very next beat.
		auto GetDrawWeight = [this, Kind, &IsFillable](const int32 TileIndex) -> float
		{
			const FTileCandidates& Candidates = TileCandidates[TileIndex];
			const bool bAllowed = Candidates.Tile->AllowsOrientation(Kind == EPlatformKind::SurfaceVertical);
			const bool bHasRoom = Candidates.ByKind[KindIndex(Kind)].ContainsByPredicate([TileIndex, &IsFillable](const FGridPlatform& Candidate)
			{
				return IsFillable(TileIndex, Candidate);
			});

			return !bAllowed || !bHasRoom ? 0.0f : FMath::Max(Candidates.Tile->Weight, 0.0f);
		};

		float TotalWeight = 0.0f;
		for (int32 TileIndex = 0; TileIndex < TileCandidates.Num(); ++TileIndex)
		{
			TotalWeight += GetDrawWeight(TileIndex);
		}

		if (TotalWeight <= 0.0f)
		{
			return false;
		}

		float Roll = PlacementStream.FRand() * TotalWeight;
		int32 ChosenTile = INDEX_NONE;

		for (int32 TileIndex = 0; TileIndex < TileCandidates.Num(); ++TileIndex)
		{
			const float Weight = GetDrawWeight(TileIndex);

			if (Weight <= 0.0f)
			{
				continue;
			}

			// Taken whenever it is in the running, so rounding at the very top of the roll still
			// lands on the last tile that could have been drawn.
			ChosenTile = TileIndex;

			if (Roll < Weight)
			{
				break;
			}

			Roll -= Weight;
		}

		const TArray<FGridPlatform>& Candidates = TileCandidates[ChosenTile].ByKind[KindIndex(Kind)];
		const UPlatformTileData* Tile = TileCandidates[ChosenTile].Tile;

		// Drawn by place among the fillable ones. Copied out, since taking the platform in changes the
		// list it was drawn from. Where every way the tile is listed can be filled - nearly always - every
		// placement is fillable, and the draw is the same without counting them.
		const uint8 ListedWays = Tile->bCanTurn && Tile->Width != Tile->Length ? (AsLaidWay | TurnedWay) : AsLaidWay;

		if ((GetFillableWays(ChosenTile, Kind) & ListedWays) == ListedWays)
		{
			Platform = Candidates[PlacementStream.RandRange(0, Candidates.Num() - 1)];
		}
		else
		{
			const int32 Fillable = Algo::CountIf(Candidates, [ChosenTile, &IsFillable](const FGridPlatform& Candidate)
			{
				return IsFillable(ChosenTile, Candidate);
			});

			int32 Pick = PlacementStream.RandRange(0, Fillable - 1);

			for (const FGridPlatform& Candidate : Candidates)
			{
				if (IsFillable(ChosenTile, Candidate) && Pick-- == 0)
				{
					Platform = Candidate;
					break;
				}
			}
		}

		// Which way up it is follows from what it grows off, and decides what can fill it.
		Platform.Up = ChooseUp(Platform);
	}

	// A platform landing where a fence run waits to carry on, laid as the run is, carries it on.
	// Anything else is filled as drawn, with the platform and before it is taken in, since what fills a
	// face decides what that face carries. The platform that starts growth holds up all the rest.
	int32 JoinedSide = 0;

	if (!TakeWaitingFenceSpot(Platform, JoinedSide) && !DrawFill(Platform, /*bLoadBearingOnly=*/BuiltPlatforms.IsEmpty()))
	{
		return false;
	}

	PlacePlatform(Platform, JoinedSide);
	return true;
}

void ARoomManager::PlacePlatform(const FGridPlatform& Platform, const int32 JoinedSide)
{
	Platforms.Add(Platform);

	TArray<FGridSurfaceRef> ChangedSurfaces;
	AddPlacedPlatform(Platform, ChangedSurfaces);
	UpdateDebugReadouts();
	UpdateVisualsAfterPlacement(Platform, ChangedSurfaces);

	const int32 VerifyEvery = CVarRoomManagerVerifyEvery.GetValueOnGameThread();

	if (VerifyEvery > 0 && ++PlacementsSinceVerify >= VerifyEvery)
	{
		PlacementsSinceVerify = 0;
		VerifyIncrementalState();
	}

	// Taken in first, so the room each end has left is judged with this panel standing.
	if (IsFencePanel(Platform))
	{
		QueueFenceEnds(Platform, JoinedSide);
	}
}

bool ARoomManager::IsFencePanel(const FGridPlatform& Platform)
{
	const UPlatformStructureData* Fence = Platform.PositiveStructure;

	return Fence && Fence == Platform.NegativeStructure && Fence->bFence && Fence->bTwoSided;
}

int32 ARoomManager::GetRunAxis(const FGridPlatform& Panel) const
{
	const int32 NormalAxis = AxisIndex(Panel.Normal);

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// On a wall the run keeps across its Up - along the ground, in the wall's own frame - whichever way
	// the fence is laid.
	if (Panel.IsWall())
	{
		return FaceAxis(Panel.Up) == V ? U : V;
	}

	// Across a floor it goes the way the fence's width does: along the first axis as authored, the
	// second when the fence is turned to fit.
	int32 SpanU, SpanV;
	GetPlatformSpans(Panel, SpanU, SpanV);

	bool bTurned = false;
	if (const UPlatformStructureData* Fence = Panel.PositiveStructure)
	{
		WEMStructures::FitsShape(Fence->Width, Fence->Length, Fence->bMayLieSideways, NormalAxis, Panel.Up, SpanU, SpanV, bTurned);
	}

	return bTurned ? V : U;
}

bool ARoomManager::IsRunContinuation(const FGridPlatform& Panel, const FGridPlatform& Previous) const
{
	if (!IsFencePanel(Panel) || !IsFencePanel(Previous)
		|| Panel.PositiveStructure != Previous.PositiveStructure
		|| Panel.Tile != Previous.Tile
		|| Panel.bTurned != Previous.bTurned
		|| Panel.Normal != Previous.Normal
		|| Panel.Up != Previous.Up)
	{
		return false;
	}

	const int32 NormalAxis = AxisIndex(Panel.Normal);
	int32 SpanU, SpanV;

	if (Panel.MinNode[NormalAxis] != Previous.MinNode[NormalAxis] || !GetPlatformSpans(Panel, SpanU, SpanV))
	{
		return false;
	}

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	const int32 RunAxis = GetRunAxis(Panel);
	const int32 AcrossAxis = RunAxis == U ? V : U;
	const int32 RunSpan = RunAxis == U ? SpanU : SpanV;

	return Panel.MinNode[AcrossAxis] == Previous.MinNode[AcrossAxis]
		&& FMath::Abs(Panel.MinNode[RunAxis] - Previous.MinNode[RunAxis]) == RunSpan;
}

void ARoomManager::GatherFenceContinuations(
	const FFenceEnd& End,
	FGridPlatform& OutStraight,
	TArray<FGridPlatform, TInlineAllocator<2>>& OutTurns) const
{
	const FGridPlatform& Panel = End.Panel;
	const int32 NormalAxis = AxisIndex(Panel.Normal);

	int32 SpanU, SpanV, U, V;
	GetPlatformSpans(Panel, SpanU, SpanV);
	GetInPlaneAxes(NormalAxis, U, V);

	const int32 RunAxis = GetRunAxis(Panel);
	const int32 RunSpan = RunAxis == U ? SpanU : SpanV;

	// Straight on: the same panel, a span further along the run.
	OutStraight = Panel;
	OutStraight.MinNode[RunAxis] += End.Side * RunSpan;

	OutTurns.Reset();

	if (!Panel.IsWall())
	{
		return;
	}

	// Round the corner: a wall facing along the run, standing in the plane of the beam at this end and
	// reaching away from this wall's plane, either way, as far as this one runs. It keeps this wall's Up
	// and is as tall along it, so the fence fills it as it fills this one. It is laid from the same tile,
	// the same way round where that gives it that shape - as it always does for a wall standing up in
	// the world - or else turned, where the tile may be. A tile that can be neither has no corner to turn.
	const int32 UpAxis = FaceAxis(Panel.Up);
	const int32 UpSpan = UpAxis == U ? SpanU : SpanV;

	int32 TurnU, TurnV;
	GetInPlaneAxes(RunAxis, TurnU, TurnV);

	const int32 TurnSpanU = TurnU == UpAxis ? UpSpan : RunSpan;
	const int32 TurnSpanV = TurnU == UpAxis ? RunSpan : UpSpan;

	const UPlatformTileData* Tile = Panel.Tile;
	bool bTurnTurned = Panel.bTurned;

	auto GivesShape = [Tile, TurnSpanU, TurnSpanV](const bool bTurned)
	{
		return (bTurned ? Tile->Length : Tile->Width) == TurnSpanU && (bTurned ? Tile->Width : Tile->Length) == TurnSpanV;
	};

	if (!GivesShape(bTurnTurned))
	{
		bTurnTurned = !bTurnTurned;

		if (!Tile->bCanTurn || !GivesShape(bTurnTurned))
		{
			return;
		}
	}

	const int32 EndCoordinate = Panel.MinNode[RunAxis] + (End.Side > 0 ? RunSpan : 0);

	for (const int32 Away : { 1, -1 })
	{
		FGridPlatform& Turn = OutTurns.Add_GetRef(Panel);
		Turn.Normal = static_cast<EGridAxis>(RunAxis);
		Turn.bTurned = bTurnTurned;
		Turn.MinNode[RunAxis] = EndCoordinate;
		Turn.MinNode[NormalAxis] = Away > 0 ? Panel.MinNode[NormalAxis] : Panel.MinNode[NormalAxis] - RunSpan;
	}
}

bool ARoomManager::IsFenceEndClosed(const FGridPlatform& Straight, const TConstArrayView<FGridPlatform> Turns) const
{
	auto IsStanding = [this](const FGridPlatform& Spot)
	{
		const int32* Owner = ModuleFaceOwners.Find(MakePlatformKey(Spot));

		if (!Owner)
		{
			return false;
		}

		const FGridPlatform& Standing = BuiltPlatforms[*Owner];

		return Standing.MinNode == Spot.MinNode && Standing.Normal == Spot.Normal && Standing.Tile == Spot.Tile
			&& Standing.bTurned == Spot.bTurned && IsFencePanel(Standing) && Standing.PositiveStructure == Spot.PositiveStructure;
	};

	return IsStanding(Straight) || Turns.ContainsByPredicate(IsStanding);
}

int32 ARoomManager::GetJoinedSide(const FFenceEnd& End, const FGridPlatform& Continuation) const
{
	// Straight on, the end facing back meets it.
	if (Continuation.Normal == End.Panel.Normal)
	{
		return -End.Side;
	}

	// Round a corner, the continuation's run goes along the first panel's normal, and the end of it
	// in the first panel's plane is the one at the beam.
	const int32 FirstNormal = AxisIndex(End.Panel.Normal);
	return Continuation.MinNode[FirstNormal] == End.Panel.MinNode[FirstNormal] ? -1 : 1;
}

bool ARoomManager::StandsOnSomething(const FGridPlatform& Platform) const
{
	if (!Platform.IsWall())
	{
		return true;
	}

	int32 SpanU, SpanV, U, V;
	GetPlatformSpans(Platform, SpanU, SpanV);
	GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

	// Its foot and its top are the rim running across its Up, at either end of it. Which is which does
	// not matter, since hanging counts.
	const int32 UpAxis = FaceAxis(Platform.Up);
	const int32 AcrossAxis = UpAxis == U ? V : U;

	bool bMinEndUsed = true;
	bool bMaxEndUsed = true;

	ForEachModuleEdge(Platform, SpanU, SpanV, GetModule(), [&](const FLatticeEdge& Edge, const bool bRim)
	{
		if (!bRim || Edge.Axis != AcrossAxis)
		{
			return;
		}

		const bool bUsed = ModuleEdgeUses.Contains(MakeEdgeKey(Edge));
		bool& bEndUsed = Edge.Node[UpAxis] == Platform.MinNode[UpAxis] ? bMinEndUsed : bMaxEndUsed;
		bEndUsed = bEndUsed && bUsed;
	});

	return bMinEndUsed || bMaxEndUsed;
}

void ARoomManager::QueueFenceEnds(const FGridPlatform& Panel, const int32 JoinedSide)
{
	for (const int32 Side : { -1, 1 })
	{
		if (Side == JoinedSide)
		{
			continue;
		}

		const FFenceEnd End{ Panel, Side };

		FGridPlatform Straight;
		TArray<FGridPlatform, TInlineAllocator<2>> Turns;
		GatherFenceContinuations(End, Straight, Turns);

		// Only an end with a free spot beyond it is drawn for; one already closed off is no end of a run,
		// and nor is one that has met its own fence.
		bool bHasRoom = IsPlatformOnGrid(Straight) && IsSpaceFree(Straight);

		for (const FGridPlatform& Turn : Turns)
		{
			bHasRoom = bHasRoom || (IsPlatformOnGrid(Turn) && IsSpaceFree(Turn));
		}

		bHasRoom = bHasRoom && !IsFenceEndClosed(Straight, Turns);

		if (bHasRoom && StructureStream.FRand() < FenceContinueChance)
		{
			FenceQueue.Add(End);
		}
	}
}

bool ARoomManager::PlaceFencePanel()
{
	while (!FenceQueue.IsEmpty())
	{
		const FFenceEnd End = FenceQueue[0];
		FenceQueue.RemoveAt(0, EAllowShrinking::No);

		// A run of a fence no longer among the structures stops where it is.
		if (!UsableStructures.Contains(End.Panel.PositiveStructure))
		{
			continue;
		}

		FGridPlatform Straight;
		TArray<FGridPlatform, TInlineAllocator<2>> Turns;
		GatherFenceContinuations(End, Straight, Turns);

		// A run that has met its own fence here since the end was queued - closing round a roof, or
		// meeting another run of it - stops rather than heading off somewhere else.
		if (IsFenceEndClosed(Straight, Turns))
		{
			continue;
		}

		// Only the spots still free, and legal to place on now.
		bool bStraight = CanPlacePlatform(Straight);
		Turns.RemoveAll([this](const FGridPlatform& Turn) { return !CanPlacePlatform(Turn); });

		// Where any of them stands on something - a floor's edge under it, a ceiling's above it - only
		// those are kept, so a parapet follows its roof's edge round a corner rather than running off
		// into the air.
		const bool bStraightStands = bStraight && StandsOnSomething(Straight);
		const bool bTurnStands = Turns.ContainsByPredicate([this](const FGridPlatform& Turn) { return StandsOnSomething(Turn); });

		if (bStraightStands || bTurnStands)
		{
			bStraight = bStraightStands;
			Turns.RemoveAll([this](const FGridPlatform& Turn) { return !StandsOnSomething(Turn); });
		}

		if (!bStraight && Turns.IsEmpty())
		{
			continue;
		}

		// Straight on when nothing else is left, round a corner when only that is, and where both are,
		// round a corner with FenceTurnChance, split evenly between the ways left.
		FGridPlatform Panel = Straight;

		if (!bStraight)
		{
			Panel = Turns[Turns.Num() > 1 ? StructureStream.RandRange(0, Turns.Num() - 1) : 0];
		}
		else if (!Turns.IsEmpty())
		{
			const float Roll = StructureStream.FRand();

			if (Roll < FenceTurnChance)
			{
				Panel = Turns[FMath::Min(FMath::FloorToInt32(Roll / FenceTurnChance * Turns.Num()), Turns.Num() - 1)];
			}
		}

		PlacePlatform(Panel, GetJoinedSide(End, Panel));
		return true;
	}

	return false;
}

bool ARoomManager::TakeWaitingFenceSpot(FGridPlatform& Platform, int32& OutJoinedSide)
{
	OutJoinedSide = 0;

	for (int32 Index = 0; Index < FenceQueue.Num(); ++Index)
	{
		const FFenceEnd& End = FenceQueue[Index];

		if (End.Panel.Tile != Platform.Tile || !UsableStructures.Contains(End.Panel.PositiveStructure))
		{
			continue;
		}

		FGridPlatform Straight;
		TArray<FGridPlatform, TInlineAllocator<2>> Turns;
		GatherFenceContinuations(End, Straight, Turns);

		auto IsSpot = [&Platform](const FGridPlatform& Spot)
		{
			return Spot.MinNode == Platform.MinNode && Spot.Normal == Platform.Normal && Spot.bTurned == Platform.bTurned;
		};

		const FGridPlatform* Spot = IsSpot(Straight) ? &Straight : Turns.FindByPredicate(IsSpot);

		// Laid there, it would have to be a legal next panel, fence and all.
		if (!Spot || !CanPlacePlatform(*Spot))
		{
			continue;
		}

		Platform.Up = Spot->Up;
		Platform.PositiveStructure = Spot->PositiveStructure;
		Platform.NegativeStructure = Spot->NegativeStructure;
		OutJoinedSide = GetJoinedSide(End, *Spot);

		FenceQueue.RemoveAt(Index);
		return true;
	}

	return false;
}

bool ARoomManager::PlaceNextPlatform()
{
	// A fence run waiting to grow takes the beat before anything else, a panel a beat.
	if (PlaceFencePanel())
	{
		return true;
	}

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
	FenceQueue.Reset();

	ResetPlacementStream();
	BeginNewGeneration();

	RebuildPlatformState();
	UpdateDebugReadouts();
	RebuildVisuals();
}

void ARoomManager::ResetPlacementStream()
{
	// A zero seed means "different every run"; any other value replays the same growth.
	const int32 Seed = PlatformRandomSeed != 0 ? PlatformRandomSeed : FMath::Rand();
	PlacementStream.Initialize(Seed);

	// The fills are drawn from the same seed but a stream apart, so a seed lays the same platforms
	// whatever the structures draw in between.
	StructureStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Structures"))))));
	RoleStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Roles"))))));
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

int32 ARoomManager::GetModule() const
{
	// Capped so that at least one module, and the node closing it, always fit the cube.
	return FMath::Clamp(LatticeModule, 2, MaxGridDimension - 1);
}

int32 ARoomManager::ResolveGridSize() const
{
	const int32 Module = GetModule();
	const int32 Cells = FMath::Clamp(GridSize, Module + 1, MaxGridDimension);

	// Whole modules, and the node that closes the far side, so a node lands on every face.
	return ((Cells - 1) / Module) * Module + 1;
}

int32 ARoomManager::GetFirstPlatformLayer() const
{
	// Node layers run from the ground to the cube's top. With an even count of modules per side
	// one lands exactly on the centre; with an odd count the centre falls mid-module, and the
	// layer just above it is taken.
	const int32 Module = GetModule();
	const int32 ModulesPerSide = (ResolveGridSize() - 1) / Module;

	return ((ModulesPerSide + 1) / 2) * Module;
}

bool ARoomManager::IsTileUsable(const UPlatformTileData* Tile) const
{
	const int32 Module = GetModule();

	return Tile
		&& Tile->Width >= Module && Tile->Width % Module == 0
		&& Tile->Length >= Module && Tile->Length % Module == 0;
}

bool ARoomManager::GetPlatformSpans(const FGridPlatform& Platform, int32& OutSpanFirst, int32& OutSpanSecond) const
{
	OutSpanFirst = 0;
	OutSpanSecond = 0;

	if (!IsTileUsable(Platform.Tile))
	{
		return false;
	}

	OutSpanFirst = Platform.bTurned ? Platform.Tile->Length : Platform.Tile->Width;
	OutSpanSecond = Platform.bTurned ? Platform.Tile->Width : Platform.Tile->Length;
	return true;
}

bool ARoomManager::IsPlatformOnGrid(const FGridPlatform& Platform) const
{
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	int32 SpanU, SpanV;

	if (NormalAxis < 0 || NormalAxis > 2 || !GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return false;
	}

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	const int32 Module = GetModule();
	const int32 LastNode = ResolveGridSize() - 1;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const int32 Coordinate = Platform.MinNode[Axis];

		// Across its plane a platform reaches its span past its min node, so its far rim has to
		// fit inside too. Along its normal it is one cell thick and reaches no further.
		const int32 Reach = Axis == U ? SpanU : (Axis == V ? SpanV : 0);

		if (Coordinate < 0 || Coordinate + Reach > LastNode || Coordinate % Module != 0)
		{
			return false;
		}
	}

	return true;
}

bool ARoomManager::IsSpaceFree(const FGridPlatform& Platform) const
{
	int32 SpanU, SpanV;

	if (!GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return false;
	}

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	bool bFree = true;

	// Another platform in the same plane would cover one of its module faces.
	ForEachModuleFace(Platform, SpanU, SpanV, Module, [&](const FIntVector& FaceMinNode)
	{
		bFree = bFree && !ModuleFaceOwners.Contains(MakeModuleFaceKey(FaceMinNode, NormalAxis));
	});

	if (!bFree)
	{
		return false;
	}

	// A platform standing across the plane can only meet it along module edges. Across the
	// interior nothing may meet it at all. Around the rim a neighbour's rim may, since rim is
	// shared - that is how anything grows - but a neighbour's interior may not.
	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		if (!bFree)
		{
			return;
		}

		if (const uint8* Uses = ModuleEdgeUses.Find(MakeEdgeKey(Edge)))
		{
			bFree = bRim && (*Uses & InteriorEdgeUse) == 0;
		}
	});

	// An edge a platform that is not load-bearing has blocked holds nothing further, rim or interior -
	// except the next panel of a fence run the blocking panel belongs to, which carries on across it.
	if (bFree && !BlockedEdges.IsEmpty())
	{
		ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, bool)
		{
			if (!bFree)
			{
				return;
			}

			if (const int32* Blocker = BlockedEdges.Find(MakeEdgeKey(Edge)))
			{
				bFree = IsRunContinuation(Platform, BuiltPlatforms[*Blocker]);
			}
		});
	}

	if (!bFree || (ObstacleFaces.IsEmpty() && ObstacleEdges.IsEmpty() && ObstacleNodes.IsEmpty()))
	{
		return bFree;
	}

	// Every cell a platform covers is the inside of one of its module faces, the run of one of its
	// module edges, or one of its nodes, so asking those pieces asks every cell - without comparing
	// boxes, however many obstacles stand.
	ForEachModuleFace(Platform, SpanU, SpanV, Module, [&](const FIntVector& FaceMinNode)
	{
		bFree = bFree && !ObstacleFaces.Contains(MakeModuleFaceKey(FaceMinNode, NormalAxis));
	});

	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, bool)
	{
		bFree = bFree && !ObstacleEdges.Contains(MakeEdgeKey(Edge));
	});

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	for (int32 StepV = 0; bFree && StepV <= SpanV; StepV += Module)
	{
		for (int32 StepU = 0; bFree && StepU <= SpanU; StepU += Module)
		{
			bFree = !ObstacleNodes.Contains(MakeCellKey(Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV)));
		}
	}

	return bFree;
}

bool ARoomManager::IsFlushWith(const FGridPlatform& Platform, const FGridPlatform& Neighbour, const int32 SharedAxis) const
{
	// Where a platform's side runs along the shared line: from its min node, as far as it reaches
	// along that axis.
	auto GetSide = [this, SharedAxis](const FGridPlatform& Side, int32& OutStart, int32& OutEnd)
	{
		int32 SpanU, SpanV, U, V;
		GetPlatformSpans(Side, SpanU, SpanV);
		GetInPlaneAxes(AxisIndex(Side.Normal), U, V);

		OutStart = Side.MinNode[SharedAxis];
		OutEnd = OutStart + (SharedAxis == U ? SpanU : (SharedAxis == V ? SpanV : 0));
	};

	int32 Start, End, NeighbourStart, NeighbourEnd;
	GetSide(Platform, Start, End);
	GetSide(Neighbour, NeighbourStart, NeighbourEnd);

	const int32 StartStep = FMath::Abs(Start - NeighbourStart);
	const int32 EndStep = FMath::Abs(End - NeighbourEnd);

	// A step shorter than any tile's side leaves a notch beside it that nothing can fill flush.
	auto IsFillable = [this](const int32 Step)
	{
		return Step == 0 || Step >= SmallestTileSide;
	};

	return (StartStep == 0 || EndStep == 0) && IsFillable(StartStep) && IsFillable(EndStep);
}

void ARoomManager::ResolveUsableTiles()
{
	UsableTiles.Reset();
	SmallestTileSide = 0;

	const int32 Module = GetModule();
	TArray<FString> Problems;

	// The structures first, since which ways round a tile can be filled decides which ways it is laid.
	ResolveUsableStructures(Problems);

	for (UPlatformTileData* Tile : Tiles)
	{
		// An empty entry is only one not filled in yet, and a repeat adds nothing.
		if (!Tile || UsableTiles.Contains(Tile))
		{
			continue;
		}

		if (IsTileUsable(Tile))
		{
			UsableTiles.Add(Tile);

			const int32 ShorterSide = FMath::Min(Tile->Width, Tile->Length);
			SmallestTileSide = SmallestTileSide == 0 ? ShorterSide : FMath::Min(SmallestTileSide, ShorterSide);
		}
		else
		{
			Problems.Add(FString::Printf(TEXT("tile '%s' (%d x %d) is not a whole number of %d-cell modules along both sides, and is left out"),
				*Tile->GetName(), Tile->Width, Tile->Length, Module));
		}
	}

	// Which ways round each tile can be filled, per kind, the way up the world is - the Up a placement
	// can always fall back on (see ChooseUp). Every wall's first axis is level and its second up,
	// whichever way it faces, so a wall facing X answers for both.
	FillableWays.Reset();

	for (const UPlatformTileData* Tile : UsableTiles)
	{
		for (const EPlatformKind Kind : { EPlatformKind::SurfaceHorizontal, EPlatformKind::SurfaceVertical })
		{
			const bool bVertical = Kind == EPlatformKind::SurfaceVertical;
			const int32 NormalAxis = bVertical ? AxisIndex(EGridAxis::X) : AxisIndex(EGridAxis::Z);
			uint8 Ways = 0;

			for (const bool bTurned : { false, true })
			{
				const int32 SpanFirst = bTurned ? Tile->Length : Tile->Width;
				const int32 SpanSecond = bTurned ? Tile->Width : Tile->Length;

				if (HasCompleteFill(NormalAxis, EGridFace::PosZ, SpanFirst, SpanSecond, /*bLoadBearingOnly=*/false))
				{
					Ways |= bTurned ? TurnedWay : AsLaidWay;
				}
			}

			FillableWays.Add(Ways);

			const bool bCanTurn = Tile->bCanTurn && Tile->Width != Tile->Length;

			if (Tile->AllowsOrientation(bVertical) && (Ways & (bCanTurn ? (AsLaidWay | TurnedWay) : AsLaidWay)) == 0)
			{
				Problems.Add(FString::Printf(TEXT("no structures can fill tile '%s' (%d x %d) %s, so it is never laid that way"),
					*Tile->GetName(), Tile->Width, Tile->Length, bVertical ? TEXT("stood up as a wall") : TEXT("laid flat")));
			}
		}
	}

	if (!FirstTile)
	{
		Problems.Add(TEXT("there is no FirstTile, so nothing grows"));
	}
	else if (!IsTileUsable(FirstTile))
	{
		Problems.Add(FString::Printf(TEXT("FirstTile '%s' (%d x %d) is not a whole number of %d-cell modules along both sides, so nothing grows"),
			*FirstTile->GetName(), FirstTile->Width, FirstTile->Length, Module));
	}
	else if (!FirstTile->AllowsOrientation(/*bVertical=*/false))
	{
		Problems.Add(FString::Printf(TEXT("FirstTile '%s' is kept to vertical, but the first platform is always laid horizontal, so nothing grows"),
			*FirstTile->GetName()));
	}
	else if (!HasCompleteFill(AxisIndex(EGridAxis::Z), EGridFace::PosZ, FirstTile->Width, FirstTile->Length, /*bLoadBearingOnly=*/true)
		&& !(FirstTile->bCanTurn && HasCompleteFill(AxisIndex(EGridAxis::Z), EGridFace::PosZ, FirstTile->Length, FirstTile->Width, /*bLoadBearingOnly=*/true)))
	{
		Problems.Add(FString::Printf(TEXT("no load-bearing structures can fill FirstTile '%s' (%d x %d) laid flat, and the first platform holds up all the rest, so nothing grows"),
			*FirstTile->GetName(), FirstTile->Width, FirstTile->Length));
	}

	// This runs on every edit and every check, so the warning is only given when what it would
	// say has changed.
	const FString Report = FString::Join(Problems, TEXT("; "));

	if (Report != ReportedTileProblems)
	{
		ReportedTileProblems = Report;

		if (!Report.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("RoomManager: %s."), *Report);
		}
	}
}

void ARoomManager::ResolveUsableStructures(TArray<FString>& OutProblems)
{
	UsableStructures.Reset();
	CategoryIndices.Reset();
	CategoriesByIndex.Reset();

	for (UPlatformStructureData* Structure : Structures)
	{
		// An empty entry is only one not filled in yet, and a repeat adds nothing.
		if (!Structure || UsableStructures.Contains(Structure))
		{
			continue;
		}

		if (!Structure->Mesh)
		{
			OutProblems.Add(FString::Printf(TEXT("structure '%s' has no mesh, and is left out"), *Structure->GetName()));
			continue;
		}

		// A fence runs as one piece through the segment, both faces at once.
		if (Structure->bFence && !Structure->bTwoSided)
		{
			OutProblems.Add(FString::Printf(TEXT("structure '%s' is a fence but not two-sided, as every fence is, and is left out"), *Structure->GetName()));
			continue;
		}

		// One of no type fits no face, and one of no weight is never drawn: both are left out as asked.
		if (Structure->Types == 0 || Structure->Weight <= 0.0f)
		{
			continue;
		}

		UsableStructures.Add(Structure);

		// Numbered in the order the structures list them, so the same list numbers them the same way.
		for (const UStructureCategory* Category : Structure->Categories)
		{
			if (Category && !CategoryIndices.Contains(Category))
			{
				CategoryIndices.Add(Category, CategoriesByIndex.Add(Category));
			}
		}
	}
}

void ARoomManager::GetCategoryIndices(const UPlatformStructureData* Structure, TArray<int32, TInlineAllocator<4>>& OutCategories) const
{
	OutCategories.Reset();

	if (!Structure)
	{
		return;
	}

	for (const UStructureCategory* Category : Structure->Categories)
	{
		if (const int32* Index = CategoryIndices.Find(Category))
		{
			OutCategories.AddUnique(*Index);
		}
	}
}

void ARoomManager::GatherFaceNeighbours(
	const FGridPlatform& Platform,
	const int32 PlatformIndex,
	const int32 FaceSign,
	TArray<int32, TInlineAllocator<16>>& OutNeighbours) const
{
	OutNeighbours.Reset();

	int32 SpanU, SpanV;
	if (!GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return;
	}

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// Whether a module face is one of the platform's own, which it may not have been recorded as yet.
	auto IsOwnFace = [&](const FModuleFace& Face)
	{
		return Face.Normal == NormalAxis
			&& Face.MinNode[NormalAxis] == Platform.MinNode[NormalAxis]
			&& Face.MinNode[U] >= Platform.MinNode[U] && Face.MinNode[U] < Platform.MinNode[U] + SpanU
			&& Face.MinNode[V] >= Platform.MinNode[V] && Face.MinNode[V] < Platform.MinNode[V] + SpanV;
	};

	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		// Only rim can be shared, and only where a platform already runs around the same edge.
		if (!bRim || !ModuleEdgeUses.Contains(MakeEdgeKey(Edge)))
		{
			return;
		}

		FModuleFace Faces[4];
		GetEdgeFaces(Edge, Module, Faces);

		// Who stands in each half-plane round the edge, walked in turning order.
		int32 Owners[4];
		int32 Occupied[4];
		int32 OccupiedCount = 0;

		for (int32 Turn = 0; Turn < 4; ++Turn)
		{
			const FModuleFace& Face = Faces[TurningOrder[Turn]];
			const int32* FaceOwner = IsOwnFace(Face) ? &PlatformIndex : ModuleFaceOwners.Find(MakeModuleFaceKey(Face.MinNode, Face.Normal));

			Owners[Turn] = FaceOwner ? *FaceOwner : INDEX_NONE;

			if (FaceOwner)
			{
				Occupied[OccupiedCount++] = Turn;
			}
		}

		// Each half-plane meets the next one round the edge: the first with its face looking the way
		// its own direction turns forward a quarter, the second with its face looking the way its own
		// turns back a quarter. Those directions lie along each one's normal. Two half-planes meet
		// twice, once each way round.
		for (int32 Pair = 0; OccupiedCount >= 2 && Pair < OccupiedCount; ++Pair)
		{
			const int32 First = Occupied[Pair];
			const int32 Second = Occupied[(Pair + 1) % OccupiedCount];

			// Turning order runs +A, +B, -A, -B, so the first two directions are the positive ones.
			const int32 FirstSign = (First + 1) % 4 < 2 ? 1 : -1;
			const int32 SecondSign = (Second + 3) % 4 < 2 ? 1 : -1;

			if (Owners[First] == PlatformIndex && FirstSign == FaceSign && Owners[Second] != PlatformIndex)
			{
				OutNeighbours.AddUnique(GetFaceFillIndex(Owners[Second], SecondSign));
			}

			if (Owners[Second] == PlatformIndex && SecondSign == FaceSign && Owners[First] != PlatformIndex)
			{
				OutNeighbours.AddUnique(GetFaceFillIndex(Owners[First], FirstSign));
			}
		}
	});
}

void ARoomManager::RecordFill(const int32 PlatformIndex)
{
	const FGridPlatform& Platform = BuiltPlatforms[PlatformIndex];
	FaceFills.SetNum(GetFaceFillIndex(PlatformIndex + 1, 1));

	for (const int32 FaceSign : { 1, -1 })
	{
		FFaceFill& Fill = FaceFills[GetFaceFillIndex(PlatformIndex, FaceSign)];
		Fill = FFaceFill();
		Fill.Structure = FaceSign > 0 ? Platform.PositiveStructure.Get() : Platform.NegativeStructure.Get();

		GetCategoryIndices(Fill.Structure, Fill.Categories);

		if (Fill.Categories.IsEmpty())
		{
			continue;
		}

		// Its streaks, and its patches, follow from the faces beside it that were filled before it.
		TArray<int32, TInlineAllocator<16>> Neighbours;
		GatherFaceNeighbours(Platform, PlatformIndex, FaceSign, Neighbours);

		for (const int32 Category : Fill.Categories)
		{
			const int32 Node = PatchParents.Add(PatchParents.Num());
			PatchSizes.Add(1);
			PatchSizeSquares += 1;

			int32 LongestBeside = 0;

			for (const int32 Neighbour : Neighbours)
			{
				const FFaceFill& NeighbourFill = FaceFills[Neighbour];
				const int32 Index = NeighbourFill.Categories.Find(Category);

				if (Index != INDEX_NONE)
				{
					LongestBeside = FMath::Max(LongestBeside, NeighbourFill.Streaks[Index]);
					JoinPatches(Node, NeighbourFill.PatchNodes[Index]);
				}
			}

			Fill.Streaks.Add(1 + LongestBeside);
			Fill.PatchNodes.Add(Node);
		}
	}

	CategoryPatchSize = PatchParents.IsEmpty() ? 0.0 : static_cast<double>(PatchSizeSquares) / PatchParents.Num();
}

int32 ARoomManager::FindPatch(int32 Node)
{
	// Halving the path on the way up keeps every later find short.
	while (PatchParents[Node] != Node)
	{
		PatchParents[Node] = PatchParents[PatchParents[Node]];
		Node = PatchParents[Node];
	}

	return Node;
}

void ARoomManager::JoinPatches(const int32 First, const int32 Second)
{
	int32 Root = FindPatch(First);
	int32 Other = FindPatch(Second);

	if (Root == Other)
	{
		return;
	}

	// The smaller goes under the larger, so no path grows long.
	if (PatchSizes[Root] < PatchSizes[Other])
	{
		Swap(Root, Other);
	}

	// Two patches of a and b become one of a + b, which adds 2ab to the sum of squares.
	PatchSizeSquares += 2 * static_cast<int64>(PatchSizes[Root]) * PatchSizes[Other];
	PatchParents[Other] = Root;
	PatchSizes[Root] += PatchSizes[Other];
}

TArray<int32> ARoomManager::GetPatchLabels()
{
	TArray<int32> Lowest;
	Lowest.Init(INDEX_NONE, PatchParents.Num());

	for (int32 Node = 0; Node < PatchParents.Num(); ++Node)
	{
		int32& Label = Lowest[FindPatch(Node)];
		Label = Label == INDEX_NONE ? Node : FMath::Min(Label, Node);
	}

	TArray<int32> Labels;
	Labels.SetNumUninitialized(PatchParents.Num());

	for (int32 Node = 0; Node < PatchParents.Num(); ++Node)
	{
		Labels[Node] = Lowest[FindPatch(Node)];
	}

	return Labels;
}

bool ARoomManager::HasCompleteFill(const int32 NormalAxis, const EGridFace Up, const int32 SpanFirst, const int32 SpanSecond, const bool bLoadBearingOnly) const
{
	bool bFrontFits = false;
	bool bBackFits = false;

	for (const UPlatformStructureData* Structure : UsableStructures)
	{
		if (bLoadBearingOnly && !Structure->bLoadBearing)
		{
			continue;
		}

		bool bTurned = false;

		// A two-sided structure fits only where it fits both faces, so one is enough.
		if (Structure->bTwoSided)
		{
			if (WEMStructures::FitsFace(*Structure, NormalAxis, 1, Up, SpanFirst, SpanSecond, bTurned))
			{
				return true;
			}

			continue;
		}

		bFrontFits = bFrontFits || WEMStructures::FitsFace(*Structure, NormalAxis, 1, Up, SpanFirst, SpanSecond, bTurned);
		bBackFits = bBackFits || WEMStructures::FitsFace(*Structure, NormalAxis, -1, Up, SpanFirst, SpanSecond, bTurned);
	}

	return bFrontFits && bBackFits;
}

uint8 ARoomManager::GetFillableWays(const int32 TileIndex, const EPlatformKind Kind) const
{
	const int32 Index = TileIndex * PlatformKindCount + KindIndex(Kind);

	return FillableWays.IsValidIndex(Index) ? FillableWays[Index] : 0;
}

void ARoomManager::GatherFillOptions(
	const FGridPlatform& Platform,
	const int32 FaceSign,
	const bool bSecondFace,
	const bool bLoadBearingOnly,
	TArray<UPlatformStructureData*>& OutOptions) const
{
	OutOptions.Reset();

	int32 SpanU, SpanV;
	if (!GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return;
	}

	const int32 NormalAxis = AxisIndex(Platform.Normal);

	auto Fits = [&](const UPlatformStructureData* Structure, const int32 Sign)
	{
		bool bTurned = false;
		return (!bLoadBearingOnly || Structure->bLoadBearing)
			&& WEMStructures::FitsFace(*Structure, NormalAxis, Sign, Platform.Up, SpanU, SpanV, bTurned);
	};

	// A one-sided structure leaves the face behind it to another, so it is only offered when there
	// is one that could fill it. The second face has nothing behind it to leave.
	bool bBackCanBeFilled = bSecondFace;

	for (int32 Index = 0; !bBackCanBeFilled && Index < UsableStructures.Num(); ++Index)
	{
		bBackCanBeFilled = !UsableStructures[Index]->bTwoSided && Fits(UsableStructures[Index], -FaceSign);
	}

	for (UPlatformStructureData* Structure : UsableStructures)
	{
		const bool bOffered = Structure->bTwoSided ? !bSecondFace : bBackCanBeFilled;

		if (bOffered && Fits(Structure, FaceSign))
		{
			OutOptions.Add(Structure);
		}
	}
}

bool ARoomManager::DrawFill(FGridPlatform& Platform, const bool bLoadBearingOnly)
{
	Platform.PositiveStructure = nullptr;
	Platform.NegativeStructure = nullptr;

	// A floor or ceiling is filled from its Ground face, the one it is walked on. A wall has no such
	// side, so either face goes first, evenly.
	const int32 FirstSign = !Platform.IsWall() ? FaceSign(Platform.Up) : (StructureStream.FRand() < 0.5f ? 1 : -1);

	TArray<UPlatformStructureData*> Options;
	GatherFillOptions(Platform, FirstSign, /*bSecondFace=*/false, bLoadBearingOnly, Options);

	UPlatformStructureData* First = DrawStructure(Options, Platform, FirstSign);

	if (!First)
	{
		return false;
	}

	if (First->bTwoSided)
	{
		Platform.PositiveStructure = First;
		Platform.NegativeStructure = First;
		return true;
	}

	GatherFillOptions(Platform, -FirstSign, /*bSecondFace=*/true, bLoadBearingOnly, Options);

	UPlatformStructureData* Second = DrawStructure(Options, Platform, -FirstSign);

	if (!Second)
	{
		return false;
	}

	Platform.PositiveStructure = FirstSign > 0 ? First : Second;
	Platform.NegativeStructure = FirstSign > 0 ? Second : First;
	return true;
}

UPlatformStructureData* ARoomManager::DrawStructure(
	const TConstArrayView<UPlatformStructureData*> Options,
	const FGridPlatform& Platform,
	const int32 FaceSign)
{
	TArray<WEMStructures::FDrawOption, TInlineAllocator<32>> DrawOptions;

	for (const UPlatformStructureData* Option : Options)
	{
		WEMStructures::FDrawOption& DrawOption = DrawOptions.AddDefaulted_GetRef();
		DrawOption.Weight = Option->Weight;
		GetCategoryIndices(Option, DrawOption.Categories);
	}

	// Every filled face beside this one offers each of its categories, as likely to carry on as its
	// streak there is long. The platform is not placed yet, so it is named by the index it will take.
	TArray<int32, TInlineAllocator<16>> Neighbours;
	GatherFaceNeighbours(Platform, BuiltPlatforms.Num(), FaceSign, Neighbours);

	TArray<WEMStructures::FNeighbourOffer, TInlineAllocator<16>> Offers;

	for (const int32 Neighbour : Neighbours)
	{
		const FFaceFill& Fill = FaceFills[Neighbour];

		if (Fill.Categories.IsEmpty())
		{
			continue;
		}

		WEMStructures::FNeighbourOffer& Offer = Offers.AddDefaulted_GetRef();

		for (int32 Index = 0; Index < Fill.Categories.Num(); ++Index)
		{
			Offer.Categories.Emplace(Fill.Categories[Index],
				WEMStructures::GetContinueChance(Fill.Streaks[Index], ContinueChance, MaxContinueChance, StreakForMaxChance));
		}
	}

	const int32 Chosen = WEMStructures::DrawWithPull(StructureStream, DrawOptions, Offers);

	return Chosen == INDEX_NONE ? nullptr : Options[Chosen];
}

void ARoomManager::RebuildPlatformState()
{
	// The tiles first, since which of them fit the lattice decides which platforms do.
	ResolveUsableTiles();

	// And the obstacles, which every placement below is held against as well.
	RebuildObstacleKeys();

	++StructureRevision;

	// Nothing before this can be followed on from: whoever reads the record walks it all again.
	SurfaceJournal.Reset();
	SurfaceJournalStarts.Reset();
	SurfaceJournalRevision = StructureRevision;

	BuiltPlatforms.Reset();
	ModuleFaceOwners.Reset();
	ModuleEdgeUses.Reset();
	RimCellNormals.Reset();
	PromotedFaces.Reset();
	RimEdgeUsers.Reset();
	BlockedEdges.Reset();
	FaceFills.Reset();
	PatchParents.Reset();
	PatchSizes.Reset();
	PatchSizeSquares = 0;
	CategoryPatchSize = 0.0;

	// The candidates follow from everything below, so they are gathered afresh on the next draw.
	bPlatformCandidatesStale = true;

	// Only the platforms that fit the lattice as it stands now, each on room of its own. One laid
	// from a tile that has since changed size, under a different module or in a larger cube may no
	// longer fit, or may now run into one listed before it. It is left out rather than drawn
	// across the grid - but kept in the list, so changing things back brings it back.
	int32 SkippedPlatforms = 0;

	for (const FGridPlatform& Platform : Platforms)
	{
		if (!IsPlatformOnGrid(Platform) || !IsSpaceFree(Platform))
		{
			++SkippedPlatforms;
			continue;
		}

		RecordPlatform(Platform);
	}

	// This runs on every edit and every check, so the warning is only given when what it would
	// say has changed.
	if (SkippedPlatforms != ReportedSkippedPlatforms)
	{
		ReportedSkippedPlatforms = SkippedPlatforms;

		if (SkippedPlatforms > 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("RoomManager: %d platform(s) in the list do not fit the lattice as it stands - their tile is missing or not a whole number of %d-cell modules, they fall outside the %d-cell cube, or they run into a platform listed before them - and are left out. ClearPlatforms starts over."),
				SkippedPlatforms, GetModule(), ResolveGridSize());
		}
	}

	// A rim surface caught between two fields is no longer rim, so it joins them (see
	// IsFlankedByOpenFields). The outer rim can never satisfy this - there is always emptiness on
	// one side of it - so a field opens up while staying enclosed. Promotions feed each other,
	// since the node at the centre of a 2x2 only resolves once the seams around it have, so this
	// runs to a fixpoint. The rule is monotone, which makes that fixpoint independent of the sweep
	// order, and of the order the platforms were placed in.
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

	bool bPromotedAny = true;
	while (bPromotedAny)
	{
		bPromotedAny = false;

		// Backwards, so removing the surface just decided cannot disturb the ones still to come.
		for (int32 PendingIndex = PendingSurfaces.Num() - 1; PendingIndex >= 0; --PendingIndex)
		{
			const FPendingSurface Pending = PendingSurfaces[PendingIndex];

			if (IsFlankedByOpenFields(Pending.Cell, Pending.Face))
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

void ARoomManager::AddPlacedPlatform(const FGridPlatform& Platform, TArray<FGridSurfaceRef>& OutChangedSurfaces)
{
	OutChangedSurfaces.Reset();

	// Only ever handed a free placement, which is on the lattice and stands on nothing but rim it
	// can share. Anything else is left to the wholesale rebuild, and the visuals are told to start
	// over with it.
	if (!ensure(IsPlatformOnGrid(Platform) && IsSpaceFree(Platform)))
	{
		RebuildPlatformState();
		bPiecesComplete = false;
		bSurfaceMarksComplete = false;
		return;
	}

	// Until the first platform stands the candidates are the first platform's own list, which
	// nothing below would turn into the lists that follow it. Gathered afresh instead - from one
	// platform, that is nothing.
	if (BuiltPlatforms.IsEmpty())
	{
		bPlatformCandidatesStale = true;
	}

	RecordPlatform(Platform);

	++StructureRevision;
	++PlatformCount;

	if (Platform.GetKind() == EPlatformKind::SurfaceHorizontal)
	{
		++HorizontalPlatformCount;
	}
	else
	{
		++VerticalPlatformCount;
	}

	int32 SpanU, SpanV;
	GetPlatformSpans(Platform, SpanU, SpanV);

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	const EGridFace FrontFace = MakeFace(NormalAxis, 1);
	const EGridFace BackFace = MakeFace(NormalAxis, -1);

	// Rim surfaces whose promotion has to be decided, in the order they come up. Grows as it is
	// worked through, since every promotion can open the surfaces beside it.
	TArray<FGridSurfaceRef> PromotionQueue;

	ForEachPlatformCell(Platform, SpanU, SpanV, [&](const FIntVector& Cell, const bool bRim)
	{
		// The platform's own two broad faces: new surfaces, or rim ones a neighbour already brought.
		OutChangedSurfaces.Add(MakeSurfaceRef(Cell, FrontFace));
		OutChangedSurfaces.Add(MakeSurfaceRef(Cell, BackFace));

		// Every rim surface the platform brings is open to promotion. Its interior carries
		// anything already, and a surface it opens beside can only be one of these rim ones, so
		// they are all the promotion needs to start from.
		if (bRim)
		{
			PromotionQueue.Add(MakeSurfaceRef(Cell, FrontFace));
			PromotionQueue.Add(MakeSurfaceRef(Cell, BackFace));
		}

		// Every surface that looks into this cell is now built against.
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (const int32 Sign : { 1, -1 })
			{
				const FIntVector Neighbour = Cell + AxisStep(Axis, Sign);

				int32 NeighbourOwner = INDEX_NONE;
				if ((GetCellSurfaceAxes(Neighbour, NeighbourOwner) & AxisBit(Axis)) != 0)
				{
					OutChangedSurfaces.Add(MakeSurfaceRef(Neighbour, MakeFace(Axis, -Sign)));
				}
			}
		}
	});

	// The same rule the rebuild sweeps to a fixpoint, worked outward from this platform instead.
	// A surface once promoted stays promoted as the structure grows - that is what lets the
	// promotions already standing be kept - so only a surface next to one that has just opened
	// can newly qualify, and each promotion queues its neighbours in its own plane and side.
	TArray<FGridSurfaceRef> PromotedSurfaces;

	for (int32 QueueIndex = 0; QueueIndex < PromotionQueue.Num(); ++QueueIndex)
	{
		// Copied out, since queueing the neighbours can move the queue.
		const FGridSurfaceRef Surface = PromotionQueue[QueueIndex];
		const uint64 FaceKey = MakeFaceKey(Surface.Cell, Surface.Face);

		if (PromotedFaces.Contains(FaceKey))
		{
			continue;
		}

		// Only an open rim surface is ever decided: an interior carries anything already, and a
		// surface built against holds its side shut.
		int32 InteriorOwner = INDEX_NONE;
		const uint8 SurfaceAxes = GetCellSurfaceAxes(Surface.Cell, InteriorOwner);

		if (InteriorOwner != INDEX_NONE
			|| (SurfaceAxes & AxisBit(FaceAxis(Surface.Face))) == 0
			|| IsSolidCell(Surface.Cell + FaceStep(Surface.Face))
			|| !IsFlankedByOpenFields(Surface.Cell, Surface.Face))
		{
			continue;
		}

		PromotedFaces.Add(FaceKey);
		PromotedSurfaces.Add(Surface);
		OutChangedSurfaces.Add(Surface);

		for (int32 InPlaneAxis = 0; InPlaneAxis < 3; ++InPlaneAxis)
		{
			if (InPlaneAxis != FaceAxis(Surface.Face))
			{
				PromotionQueue.Add(MakeSurfaceRef(Surface.Cell - AxisStep(InPlaneAxis), Surface.Face));
				PromotionQueue.Add(MakeSurfaceRef(Surface.Cell + AxisStep(InPlaneAxis), Surface.Face));
			}
		}
	}

	JournalSurfaceChanges(OutChangedSurfaces);

	if (bPlatformCandidatesStale)
	{
		return;
	}

	// A placement is judged by the module edges it takes in: what stands along them, and what the
	// runs around its rim still offer. So the only placements whose standing can have changed are
	// those taking in an edge this platform takes in - which they would now share, stand across,
	// or fold away from - and those taking in an edge whose run has just been promoted, which no
	// longer offers a fold. Nodes are never asked, so a promoted node changes nothing here.
	TArray<FLatticeEdge> ChangedEdges;
	TSet<uint64> ChangedEdgeKeys;
	TSet<uint64> NewRimEdgeKeys;

	auto AddChangedEdge = [&ChangedEdges, &ChangedEdgeKeys](const FLatticeEdge& Edge)
	{
		bool bAlreadyAdded = false;
		ChangedEdgeKeys.Add(MakeEdgeKey(Edge), &bAlreadyAdded);

		if (!bAlreadyAdded)
		{
			ChangedEdges.Add(Edge);
		}
	};

	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		AddChangedEdge(Edge);

		if (bRim)
		{
			NewRimEdgeKeys.Add(MakeEdgeKey(Edge));
		}
	});

	for (const FGridSurfaceRef& Surface : PromotedSurfaces)
	{
		FLatticeEdge Edge;
		if (GetRunEdge(Surface.Cell, Module, Edge))
		{
			AddChangedEdge(Edge);
		}
	}

	// Everything else a placement asks for only ever closes as the structure grows, so a placement
	// not yet listed can only have come free by coming to share a module edge of rim with this
	// platform - and only those are judged, along with every listed one. This platform's own
	// placement is among the listed, and drops out here too.
	TArray<TSet<uint64>> Judged;
	Judged.SetNum(TileCandidates.Num());

	for (const FLatticeEdge& Edge : ChangedEdges)
	{
		const bool bNewRim = NewRimEdgeKeys.Contains(MakeEdgeKey(Edge));

		ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/false,
			[&](const int32 TileIndex, const FGridPlatform& Candidate, const bool bOnRim)
		{
			const uint64 Key = MakeCandidateKey(Candidate);

			if (!Judged[TileIndex].Contains(Key) && RefreshPlatformCandidate(TileIndex, Candidate, bNewRim && bOnRim))
			{
				Judged[TileIndex].Add(Key);
			}
		});
	}
}

void ARoomManager::RecordPlatform(const FGridPlatform& Platform)
{
	int32 SpanU, SpanV;
	GetPlatformSpans(Platform, SpanU, SpanV);

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	const int32 PlatformIndex = BuiltPlatforms.Add(Platform);

	ForEachModuleFace(Platform, SpanU, SpanV, Module, [&](const FIntVector& FaceMinNode)
	{
		ModuleFaceOwners.Add(MakeModuleFaceKey(FaceMinNode, NormalAxis), PlatformIndex);
	});

	// A platform that is not load-bearing blocks beams of its rim as it is taken in: a wall the ones
	// along its top, running across its Up at the end Up points to; a floor or ceiling every one no
	// platform used before it, which leaves the ones it grew from open.
	const bool bBlocks = !IsLoadBearing(Platform);
	const bool bWall = Platform.IsWall();

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	const int32 UpAxis = FaceAxis(Platform.Up);
	const int32 TopCoordinate = Platform.MinNode[UpAxis] + (FaceSign(Platform.Up) > 0 ? (UpAxis == U ? SpanU : SpanV) : 0);

	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		const uint64 EdgeKey = MakeEdgeKey(Edge);

		if (bRim && bBlocks)
		{
			const bool bTop = Edge.Axis != UpAxis && Edge.Node[UpAxis] == TopCoordinate;
			const bool bFree = !ModuleEdgeUses.Contains(EdgeKey);

			if (bWall ? bTop : bFree)
			{
				BlockedEdges.FindOrAdd(EdgeKey, PlatformIndex);
			}
		}

		ModuleEdgeUses.FindOrAdd(EdgeKey) |= bRim ? RimEdgeUse : InteriorEdgeUse;

		if (bRim)
		{
			++RimEdgeUsers.FindOrAdd(EdgeKey);
		}
	});

	// A cell of rim can be rim to platforms facing two ways, and a node to all three; each
	// platform brings the pair of faces along its own normal, which is how a cell comes to carry
	// surfaces along more than one axis.
	const uint8 NormalBit = AxisBit(NormalAxis);

	ForEachPlatformCell(Platform, SpanU, SpanV, [this, NormalBit](const FIntVector& Cell, const bool bRim)
	{
		if (bRim)
		{
			RimCellNormals.FindOrAdd(MakeCellKey(Cell)) |= NormalBit;
		}
	});

	// What fills it follows from the faces placed before it, so it is recorded in the same order.
	RecordFill(PlatformIndex);
}

void ARoomManager::JournalSurfaceChanges(const TConstArrayView<FGridSurfaceRef> Surfaces)
{
	// A revision the record does not lead straight on to leaves a gap no reader could see across,
	// so it starts over from here.
	if (StructureRevision != SurfaceJournalRevision + SurfaceJournalStarts.Num() + 1)
	{
		SurfaceJournal.Reset();
		SurfaceJournalStarts.Reset();
		SurfaceJournalRevision = StructureRevision;
		return;
	}

	// Let go of the older half once it is full, so the record stays the size of the last few
	// thousand placements however long growth runs.
	if (SurfaceJournalStarts.Num() >= 2 * SurfaceJournalPlacements)
	{
		const int32 Dropped = SurfaceJournalStarts[SurfaceJournalPlacements];

		SurfaceJournal.RemoveAt(0, Dropped, EAllowShrinking::No);
		SurfaceJournalStarts.RemoveAt(0, SurfaceJournalPlacements, EAllowShrinking::No);
		SurfaceJournalRevision += SurfaceJournalPlacements;

		for (int32& Start : SurfaceJournalStarts)
		{
			Start -= Dropped;
		}
	}

	SurfaceJournalStarts.Add(SurfaceJournal.Num());
	SurfaceJournal.Append(Surfaces.GetData(), Surfaces.Num());
}

bool ARoomManager::GatherSurfaceChangesSince(const int32 FromRevision, TArray<FGridSurfaceRef>& OutSurfaces) const
{
	OutSurfaces.Reset();

	// Run I is revision SurfaceJournalRevision + I + 1's, so the record reaches back to
	// SurfaceJournalRevision and, when it is whole, forward to the revision standing now.
	if (FromRevision < SurfaceJournalRevision
		|| FromRevision > StructureRevision
		|| StructureRevision != SurfaceJournalRevision + SurfaceJournalStarts.Num())
	{
		return false;
	}

	if (FromRevision < StructureRevision)
	{
		const int32 First = SurfaceJournalStarts[FromRevision - SurfaceJournalRevision];
		OutSurfaces.Append(SurfaceJournal.GetData() + First, SurfaceJournal.Num() - First);
	}

	return true;
}

bool ARoomManager::IsFlankedByOpenFields(const FIntVector& Cell, const EGridFace Face) const
{
	// Open all-object: carries anything and is not built against. Judged on the same side as the
	// surface itself, so a wall divides only the side it stands on.
	auto IsOpenAllObject = [this, Face](const FIntVector& Neighbour) -> bool
	{
		ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
		bool bOccupied = false;
		ResolveSurface(Neighbour, Face, Capacity, bOccupied);

		return Capacity == ESurfaceCapacity::AllObject && !bOccupied;
	};

	const int32 NormalAxis = FaceAxis(Face);

	for (int32 InPlaneAxis = 0; InPlaneAxis < 3; ++InPlaneAxis)
	{
		if (InPlaneAxis == NormalAxis)
		{
			continue;
		}

		const FIntVector Across = AxisStep(InPlaneAxis);

		if (IsOpenAllObject(Cell - Across) && IsOpenAllObject(Cell + Across))
		{
			return true;
		}
	}

	return false;
}

void ARoomManager::VerifyIncrementalState()
{
	// What the placements built up, one at a time.
	const TMap<uint64, int32> IncrementalFaceOwners = ModuleFaceOwners;
	const TMap<uint64, uint8> IncrementalEdgeUses = ModuleEdgeUses;
	const TMap<uint64, uint8> IncrementalRimCellNormals = RimCellNormals;
	const TMap<uint64, int32> IncrementalRimEdgeUsers = RimEdgeUsers;
	const TMap<uint64, int32> IncrementalBlockedEdges = BlockedEdges;
	const TSet<uint64> IncrementalPromotedFaces = PromotedFaces;
	const TSet<uint64> IncrementalObstacleFaces = ObstacleFaces;
	const TSet<uint64> IncrementalObstacleEdges = ObstacleEdges;
	const TSet<uint64> IncrementalObstacleNodes = ObstacleNodes;
	const bool bHadCandidates = !bPlatformCandidatesStale;
	const TArray<FTileCandidates> IncrementalCandidates = TileCandidates;
	const TArray<FFaceFill> IncrementalFaceFills = FaceFills;
	const TArray<int32> IncrementalPatchLabels = GetPatchLabels();
	const int32 IncrementalStructureRevision = StructureRevision;

	// The record of changed surfaces belongs with the revision, and goes back with it below.
	TArray<FGridSurfaceRef> IncrementalJournal = MoveTemp(SurfaceJournal);
	TArray<int32> IncrementalJournalStarts = MoveTemp(SurfaceJournalStarts);
	const int32 IncrementalJournalRevision = SurfaceJournalRevision;

	// And what the wholesale rebuild makes of the same list. That is kept either way: it is the
	// reference, so a slip is reported once and not carried forward.
	RebuildPlatformState();
	EnsurePlatformCandidates();
	const int32 RedrawnPieces = SyncPieces();
	const int32 RedrawnMarks = SyncSurfaceMarks();

	TArray<FString> Mismatches;

	auto CompareKeySets = [&Mismatches](const TCHAR* Name, const TSet<uint64>& Incremental, const TSet<uint64>& Rebuilt)
	{
		int32 Missing = 0;
		for (const uint64 Key : Rebuilt)
		{
			Missing += Incremental.Contains(Key) ? 0 : 1;
		}

		int32 Extra = 0;
		for (const uint64 Key : Incremental)
		{
			Extra += Rebuilt.Contains(Key) ? 0 : 1;
		}

		if (Missing > 0 || Extra > 0)
		{
			Mismatches.Add(FString::Printf(TEXT("%s: %d missing, %d extra"), Name, Missing, Extra));
		}
	};

	auto CompareMaps = [&Mismatches](const TCHAR* Name, const auto& Incremental, const auto& Rebuilt)
	{
		int32 Differences = FMath::Abs(Incremental.Num() - Rebuilt.Num());

		for (const auto& Entry : Rebuilt)
		{
			const auto* IncrementalValue = Incremental.Find(Entry.Key);
			Differences += (IncrementalValue && *IncrementalValue == Entry.Value) ? 0 : 1;
		}

		if (Differences > 0)
		{
			Mismatches.Add(FString::Printf(TEXT("%s: %d differ"), Name, Differences));
		}
	};

	CompareMaps(TEXT("module faces"), IncrementalFaceOwners, ModuleFaceOwners);
	CompareMaps(TEXT("module edges"), IncrementalEdgeUses, ModuleEdgeUses);
	CompareMaps(TEXT("rim cells"), IncrementalRimCellNormals, RimCellNormals);
	CompareMaps(TEXT("rim edge users"), IncrementalRimEdgeUsers, RimEdgeUsers);
	CompareMaps(TEXT("blocked edges"), IncrementalBlockedEdges, BlockedEdges);
	CompareKeySets(TEXT("promoted surfaces"), IncrementalPromotedFaces, PromotedFaces);
	CompareKeySets(TEXT("obstacle faces"), IncrementalObstacleFaces, ObstacleFaces);
	CompareKeySets(TEXT("obstacle edges"), IncrementalObstacleEdges, ObstacleEdges);
	CompareKeySets(TEXT("obstacle nodes"), IncrementalObstacleNodes, ObstacleNodes);

	// The streaks and patches depend on the order platforms were placed in, which the list keeps.
	int32 DifferentFills = FMath::Abs(IncrementalFaceFills.Num() - FaceFills.Num());

	for (int32 Index = 0; Index < FMath::Min(IncrementalFaceFills.Num(), FaceFills.Num()); ++Index)
	{
		const FFaceFill& Incremental = IncrementalFaceFills[Index];
		const FFaceFill& Rebuilt = FaceFills[Index];

		DifferentFills += Incremental.Structure == Rebuilt.Structure
			&& Incremental.Categories == Rebuilt.Categories
			&& Incremental.Streaks == Rebuilt.Streaks
			&& Incremental.PatchNodes == Rebuilt.PatchNodes ? 0 : 1;
	}

	if (DifferentFills > 0)
	{
		Mismatches.Add(FString::Printf(TEXT("face fills: %d differ"), DifferentFills));
	}

	if (IncrementalPatchLabels != GetPatchLabels())
	{
		Mismatches.Add(TEXT("category patches differ"));
	}

	// Compared in order, since the order is what the placement stream draws from.
	if (bHadCandidates)
	{
		bool bSame = IncrementalCandidates.Num() == TileCandidates.Num();
		int32 KeptCandidates = 0;
		int32 RebuiltCandidates = 0;

		for (int32 TileIndex = 0; TileIndex < TileCandidates.Num(); ++TileIndex)
		{
			for (int32 Kind = 0; Kind < PlatformKindCount; ++Kind)
			{
				const TArray<FGridPlatform>& Rebuilt = TileCandidates[TileIndex].ByKind[Kind];
				RebuiltCandidates += Rebuilt.Num();

				if (!IncrementalCandidates.IsValidIndex(TileIndex))
				{
					continue;
				}

				const TArray<FGridPlatform>& Incremental = IncrementalCandidates[TileIndex].ByKind[Kind];
				KeptCandidates += Incremental.Num();

				bSame = bSame && Incremental.Num() == Rebuilt.Num();
				for (int32 Index = 0; bSame && Index < Rebuilt.Num(); ++Index)
				{
					bSame = MakeCandidateKey(Incremental[Index]) == MakeCandidateKey(Rebuilt[Index]);
				}
			}
		}

		if (!bSame)
		{
			Mismatches.Add(FString::Printf(TEXT("candidates: %d kept, %d rebuilt"), KeptCandidates, RebuiltCandidates));
		}
	}

	if (RedrawnPieces > 0)
	{
		Mismatches.Add(FString::Printf(TEXT("pieces: %d missing"), RedrawnPieces));
	}

	if (RedrawnMarks > 0)
	{
		Mismatches.Add(FString::Printf(TEXT("overlay: %d squares missing or the wrong colour"), RedrawnMarks));
	}

	++VerifiedPlacements;

	// A check that found nothing changed nothing, so it leaves the revision where it was. Bumping it
	// would have whatever keeps work against the revision - the object placer's record of what
	// cannot be placed - redo that work, and draw differently for it, only because the check ran.
	if (Mismatches.IsEmpty())
	{
		StructureRevision = IncrementalStructureRevision;
		SurfaceJournal = MoveTemp(IncrementalJournal);
		SurfaceJournalStarts = MoveTemp(IncrementalJournalStarts);
		SurfaceJournalRevision = IncrementalJournalRevision;
	}
	else
	{
		++FailedVerifications;

		UE_LOG(LogTemp, Warning,
			TEXT("RoomManager: at %d platform(s) the placements had drifted from a full rebuild (%s). Carrying on from the rebuild."),
			PlatformCount, *FString::Join(Mismatches, TEXT("; ")));
	}

	if (VerifiedPlacements % 100 == 0)
	{
		UE_LOG(LogTemp, Log,
			TEXT("RoomManager: %d placement(s) checked against a full rebuild, %d drifted. %d platform(s) standing."),
			VerifiedPlacements, FailedVerifications, PlatformCount);
	}
}

uint8 ARoomManager::GetCellSurfaceAxes(const FIntVector& Cell, int32& OutInteriorOwner) const
{
	OutInteriorOwner = INDEX_NONE;

	if (!IsValidCell(Cell))
	{
		return 0;
	}

	const int32 Module = GetModule();

	// Which lattice planes the cell lies on says what it could be: on none, it is inside one of
	// the lattice's cubes, where nothing is ever built; on one, it can only be interior; on two
	// or three - a module edge or a node - it can be rim, or interior to a platform bridging it.
	uint8 LatticePlanes = 0;
	int32 LatticePlaneCount = 0;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Cell[Axis] % Module == 0)
		{
			LatticePlanes |= AxisBit(Axis);
			++LatticePlaneCount;
		}
	}

	if (LatticePlaneCount == 0)
	{
		return 0;
	}

	// Rim can have been brought by any of several platforms, so it is recorded. A beam or post every
	// platform around it has blocked is not built, and is nothing at all.
	if (LatticePlaneCount >= 2)
	{
		if (const uint8* RimAxes = RimCellNormals.Find(MakeCellKey(Cell)))
		{
			return BlockedEdges.IsEmpty() || IsRimCellBuilt(Cell, LatticePlaneCount) ? *RimAxes : 0;
		}
	}

	// Interior follows from the module faces around it, so it needs no record of its own - which
	// is most of every platform. Nothing but the one platform ever takes in an interior cell.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if ((LatticePlanes & AxisBit(Axis)) == 0)
		{
			continue;
		}

		const int32 PlaneOwner = FindInteriorOwner(Cell, Axis);

		if (PlaneOwner != INDEX_NONE)
		{
			OutInteriorOwner = PlaneOwner;
			return AxisBit(Axis);
		}
	}

	return 0;
}

int32 ARoomManager::FindInteriorOwner(const FIntVector& Cell, const int32 NormalAxis) const
{
	const int32 Module = GetModule();

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// Along each in-plane axis the cell lies within one module, or on the line between two, so it
	// touches one module face of the plane, two, or four. It is interior only if one platform
	// covers all of them.
	const int32 FirstModuleU = Cell[U] % Module == 0 ? Cell[U] / Module - 1 : Cell[U] / Module;
	const int32 FirstModuleV = Cell[V] % Module == 0 ? Cell[V] / Module - 1 : Cell[V] / Module;

	if (FirstModuleU < 0 || FirstModuleV < 0)
	{
		return INDEX_NONE;
	}

	int32 CoveringPlatform = INDEX_NONE;

	for (int32 ModuleV = FirstModuleV; ModuleV <= Cell[V] / Module; ++ModuleV)
	{
		for (int32 ModuleU = FirstModuleU; ModuleU <= Cell[U] / Module; ++ModuleU)
		{
			FIntVector FaceMinNode = Cell;
			FaceMinNode[U] = ModuleU * Module;
			FaceMinNode[V] = ModuleV * Module;

			const int32* FaceOwner = ModuleFaceOwners.Find(MakeModuleFaceKey(FaceMinNode, NormalAxis));

			if (!FaceOwner || (CoveringPlatform != INDEX_NONE && *FaceOwner != CoveringPlatform))
			{
				return INDEX_NONE;
			}

			CoveringPlatform = *FaceOwner;
		}
	}

	return CoveringPlatform;
}

bool ARoomManager::IsFaceBlocked(const FGridPlatform& Platform, const EGridFace Face) const
{
	// The structure filling that face answers for it. A two-sided one is held on both faces, so it
	// answers for both; a platform with no structure holds anything.
	const UPlatformStructureData* Structure = FaceSign(Face) > 0 ? Platform.PositiveStructure : Platform.NegativeStructure;

	return Structure && !Structure->bHoldsItems;
}

bool ARoomManager::IsLoadBearing(const FGridPlatform& Platform)
{
	return (!Platform.PositiveStructure || Platform.PositiveStructure->bLoadBearing)
		&& (!Platform.NegativeStructure || Platform.NegativeStructure->bLoadBearing);
}

bool ARoomManager::IsEdgeBuilt(const uint64 EdgeKey) const
{
	// The platform that blocked an edge uses it too, so it is built only where another uses it as well.
	return RimEdgeUsers.FindRef(EdgeKey) > (BlockedEdges.Contains(EdgeKey) ? 1 : 0);
}

bool ARoomManager::IsNodeBuilt(const FIntVector& Node) const
{
	const int32 Module = GetModule();

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// The beams that end here: the one starting at it along each axis, and the one ending at it.
		if (IsEdgeBuilt(MakeEdgeKey(FLatticeEdge{ Node, Axis })) || IsEdgeBuilt(MakeEdgeKey(FLatticeEdge{ Node - AxisStep(Axis, Module), Axis })))
		{
			return true;
		}
	}

	return false;
}

bool ARoomManager::IsRimCellBuilt(const FIntVector& Cell, const int32 LatticePlaneCount) const
{
	if (LatticePlaneCount >= 3)
	{
		return IsNodeBuilt(Cell);
	}

	FLatticeEdge Edge;
	return GetRunEdge(Cell, GetModule(), Edge) && IsEdgeBuilt(MakeEdgeKey(Edge));
}

bool ARoomManager::IsSolidCell(const FIntVector& Cell) const
{
	int32 InteriorOwner = INDEX_NONE;

	return GetCellSurfaceAxes(Cell, InteriorOwner) != 0;
}

bool ARoomManager::IsCellSolid(const FIntVector& Cell) const
{
	return IsSolidCell(Cell);
}

int32 ARoomManager::AddObstacle(const FIntVector& MinCell, const FIntVector& MaxCell)
{
	FGridObstacle Obstacle;
	Obstacle.Handle = NextObstacleHandle++;
	Obstacle.MinCell = FIntVector(FMath::Min(MinCell.X, MaxCell.X), FMath::Min(MinCell.Y, MaxCell.Y), FMath::Min(MinCell.Z, MaxCell.Z));
	Obstacle.MaxCell = FIntVector(FMath::Max(MinCell.X, MaxCell.X), FMath::Max(MinCell.Y, MaxCell.Y), FMath::Max(MinCell.Z, MaxCell.Z));
	Obstacles.Add(Obstacle);

	const int32 Module = GetModule();

	// An obstacle only ever takes room away, so no placement comes free and only listed ones can
	// drop out. Those are exactly the ones taking in a piece it cuts: a cut edge itself, one of the
	// four edges around a cut face - a placement covering the face takes in all four - or one of the
	// edges meeting at a cut node, since a placement covering the node takes in some of them.
	TArray<FLatticeEdge> AffectedEdges;
	TSet<uint64> AffectedEdgeKeys;

	auto AddAffectedEdge = [&AffectedEdges, &AffectedEdgeKeys](const FLatticeEdge& Edge)
	{
		bool bAlreadyAdded = false;
		AffectedEdgeKeys.Add(MakeEdgeKey(Edge), &bAlreadyAdded);

		if (!bAlreadyAdded)
		{
			AffectedEdges.Add(Edge);
		}
	};

	ForEachObstaclePiece(Obstacle.MinCell, Obstacle.MaxCell, Module,
		[&](const FIntVector& FaceMinNode, const int32 NormalAxis)
		{
			ObstacleFaces.Add(MakeModuleFaceKey(FaceMinNode, NormalAxis));

			int32 U, V;
			GetInPlaneAxes(NormalAxis, U, V);

			AddAffectedEdge(FLatticeEdge{ FaceMinNode, U });
			AddAffectedEdge(FLatticeEdge{ FaceMinNode + AxisStep(V, Module), U });
			AddAffectedEdge(FLatticeEdge{ FaceMinNode, V });
			AddAffectedEdge(FLatticeEdge{ FaceMinNode + AxisStep(U, Module), V });
		},
		[&](const FLatticeEdge& Edge)
		{
			ObstacleEdges.Add(MakeEdgeKey(Edge));
			AddAffectedEdge(Edge);
		},
		[&](const FIntVector& Node)
		{
			ObstacleNodes.Add(MakeCellKey(Node));

			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				AddAffectedEdge(FLatticeEdge{ Node, Axis });
				AddAffectedEdge(FLatticeEdge{ Node - AxisStep(Axis, Module), Axis });
			}
		});

	// Lists that are going to be gathered afresh anyway hold nothing worth refreshing.
	if (bPlatformCandidatesStale || TileCandidates.Num() != UsableTiles.Num())
	{
		return Obstacle.Handle;
	}

	TArray<TSet<uint64>> Judged;
	Judged.SetNum(TileCandidates.Num());

	for (const FLatticeEdge& Edge : AffectedEdges)
	{
		ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/false,
			[&](const int32 TileIndex, const FGridPlatform& Candidate, bool)
		{
			bool bAlreadyJudged = false;
			Judged[TileIndex].Add(MakeCandidateKey(Candidate), &bAlreadyJudged);

			if (!bAlreadyJudged)
			{
				RefreshPlatformCandidate(TileIndex, Candidate, /*bMayHaveComeFree=*/false);
			}
		});
	}

	return Obstacle.Handle;
}

void ARoomManager::RemoveObstacle(const int32 Handle)
{
	if (Obstacles.RemoveAll([Handle](const FGridObstacle& Obstacle) { return Obstacle.Handle == Handle; }) == 0)
	{
		return;
	}

	// Taking an obstacle away gives room back, which the incremental refresh is not built for - it
	// only follows room being taken. The lists are gathered afresh on the next draw instead.
	RebuildObstacleKeys();
	bPlatformCandidatesStale = true;
}

void ARoomManager::ClearObstacles()
{
	if (Obstacles.IsEmpty())
	{
		return;
	}

	Obstacles.Reset();
	RebuildObstacleKeys();
	bPlatformCandidatesStale = true;
}

void ARoomManager::RebuildObstacleKeys()
{
	ObstacleFaces.Reset();
	ObstacleEdges.Reset();
	ObstacleNodes.Reset();

	const int32 Module = GetModule();

	for (const FGridObstacle& Obstacle : Obstacles)
	{
		ForEachObstaclePiece(Obstacle.MinCell, Obstacle.MaxCell, Module,
			[this](const FIntVector& FaceMinNode, const int32 NormalAxis) { ObstacleFaces.Add(MakeModuleFaceKey(FaceMinNode, NormalAxis)); },
			[this](const FLatticeEdge& Edge) { ObstacleEdges.Add(MakeEdgeKey(Edge)); },
			[this](const FIntVector& Node) { ObstacleNodes.Add(MakeCellKey(Node)); });
	}
}

void ARoomManager::BeginNewGeneration()
{
	Obstacles.Reset();
	ObstacleFaces.Reset();
	ObstacleEdges.Reset();
	ObstacleNodes.Reset();

	++RebuildGeneration;
}

void ARoomManager::GetObstaclePieceBounds(const FIntVector& MinCell, const FIntVector& MaxCell, TArray<FBox>& OutPieces) const
{
	OutPieces.Reset();

	const int32 Module = GetModule();
	const FIntVector Min(FMath::Min(MinCell.X, MaxCell.X), FMath::Min(MinCell.Y, MaxCell.Y), FMath::Min(MinCell.Z, MaxCell.Z));
	const FIntVector Max(FMath::Max(MinCell.X, MaxCell.X), FMath::Max(MinCell.Y, MaxCell.Y), FMath::Max(MinCell.Z, MaxCell.Z));

	// An edge can be visited from both planes it lies in; it is drawn once.
	TSet<uint64> DrawnEdges;

	auto AddCells = [this, &OutPieces](const FIntVector& FirstCell, const FIntVector& CellExtent)
	{
		const FVector Corner = GetCellMinCorner(FirstCell);
		OutPieces.Add(FBox(Corner, Corner + FVector(CellExtent) * CellSize));
	};

	ForEachObstaclePiece(Min, Max, Module,
		[&](const FIntVector& FaceMinNode, const int32 NormalAxis)
		{
			int32 U, V;
			GetInPlaneAxes(NormalAxis, U, V);
			AddCells(FaceMinNode + AxisStep(U) + AxisStep(V), FIntVector(1) + AxisStep(U, Module - 2) + AxisStep(V, Module - 2));
		},
		[&](const FLatticeEdge& Edge)
		{
			bool bAlreadyDrawn = false;
			DrawnEdges.Add(MakeEdgeKey(Edge), &bAlreadyDrawn);

			if (!bAlreadyDrawn)
			{
				AddCells(Edge.Node + AxisStep(Edge.Axis), FIntVector(1) + AxisStep(Edge.Axis, Module - 2));
			}
		},
		[&](const FIntVector& Node)
		{
			AddCells(Node, FIntVector(1));
		});
}

void ARoomManager::ResolveSurface(
	const FIntVector& Cell,
	const EGridFace Face,
	ESurfaceCapacity& OutCapacity,
	bool& bOutOccupied) const
{
	OutCapacity = ESurfaceCapacity::Empty;
	bOutOccupied = false;

	int32 InteriorOwner = INDEX_NONE;
	const uint8 SurfaceAxes = GetCellSurfaceAxes(Cell, InteriorOwner);

	// Nothing built here faces this way. The thin outer edge of a rim lands here too: it faces
	// across its platform, not along the platform's normal.
	if ((SurfaceAxes & AxisBit(FaceAxis(Face))) == 0)
	{
		return;
	}

	// A side its structure closes off is not a surface at all. Only an interior can be: the rim
	// around it is shared with its neighbours, and stays a surface for them.
	if (InteriorOwner != INDEX_NONE && IsFaceBlocked(BuiltPlatforms[InteriorOwner], Face))
	{
		return;
	}

	// Occupancy outranks what a surface carries, but does not replace it: an occupied rim
	// surface is still wall surface, it has only stopped being a free one.
	bOutOccupied = IsSolidCell(Cell + FaceStep(Face));

	OutCapacity = (InteriorOwner != INDEX_NONE || PromotedFaces.Contains(MakeFaceKey(Cell, Face)))
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
	DebugProbeStructure = nullptr;
	DebugProbeCategories.Reset();
	bDebugProbeInFenceRun = false;
	DebugProbeRole.Reset();

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

				int32 InteriorOwner = INDEX_NONE;
				const uint8 SurfaceAxes = GetCellSurfaceAxes(Cell, InteriorOwner);

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if ((SurfaceAxes & AxisBit(Axis)) == 0)
					{
						continue;
					}

					for (const int32 Sign : { 1, -1 })
					{
						const EGridFace Face = MakeFace(Axis, Sign);

						// A side its structure closes off is no surface to find.
						if (InteriorOwner != INDEX_NONE && IsFaceBlocked(BuiltPlatforms[InteriorOwner], Face))
						{
							continue;
						}

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

	if (!bFoundSurface)
	{
		return;
	}

	ResolveSurface(DebugProbeSurface.Cell, DebugProbeSurface.Face, DebugProbeSurfaceCapacity, bDebugProbeSurfaceOccupied);

	// Across an opening the surface is one face of its platform's fill. On the rim it is a beam's,
	// which no structure fills.
	int32 InteriorOwner = INDEX_NONE;
	GetCellSurfaceAxes(DebugProbeSurface.Cell, InteriorOwner);

	const int32 FaceIndex = InteriorOwner != INDEX_NONE ? GetFaceFillIndex(InteriorOwner, FaceSign(DebugProbeSurface.Face)) : INDEX_NONE;

	if (!FaceFills.IsValidIndex(FaceIndex))
	{
		return;
	}

	const FFaceFill& Fill = FaceFills[FaceIndex];
	const FGridPlatform& OwnerPlatform = BuiltPlatforms[InteriorOwner];
	DebugProbeStructure = FaceSign(DebugProbeSurface.Face) > 0 ? OwnerPlatform.PositiveStructure : OwnerPlatform.NegativeStructure;
	bDebugProbeInFenceRun = IsFencePanel(OwnerPlatform);

	// As the structure types name the face: a floor's Ground face looks along its Up.
	const TCHAR* RoleName = OwnerPlatform.IsWall() ? TEXT("wall")
		: (FaceSign(DebugProbeSurface.Face) == FaceSign(OwnerPlatform.Up) ? TEXT("ground") : TEXT("ceiling"));
	DebugProbeRole = FString::Printf(TEXT("%s, up %s"), RoleName, *StaticEnum<EGridFace>()->GetDisplayNameTextByValue(static_cast<int64>(OwnerPlatform.Up)).ToString());

	TArray<FString> Streaks;
	for (int32 Index = 0; Index < Fill.Categories.Num(); ++Index)
	{
		const UStructureCategory* Category = CategoriesByIndex[Fill.Categories[Index]];
		const FString Name = Category->DisplayName.IsEmpty() ? Category->GetName() : Category->DisplayName.ToString();

		Streaks.Add(FString::Printf(TEXT("%s: streak %d"), *Name, Fill.Streaks[Index]));
	}

	DebugProbeCategories = Streaks.IsEmpty() ? TEXT("none") : FString::Join(Streaks, TEXT(", "));
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

void ARoomManager::UpdateVisualsAfterPlacement(
	const FGridPlatform& Platform,
	const TConstArrayView<FGridSurfaceRef> ChangedSurfaces)
{
	AppendPlatformPieces(Platform);
	UpdateSurfaceMarks(ChangedSurfaces);
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

int32 ARoomManager::SyncPieces()
{
	if (!EdgePieces || !NodePieces)
	{
		return 0;
	}

	// Every piece that should be standing, each named by what it stands for. A kind that is
	// switched off is simply left empty, which its layer reads as having been taken away.
	FGatheredPieces Pieces;

	const bool bDrawPieces = bShowPieces && PieceMesh;

	if (bDrawPieces)
	{
		for (const FGridPlatform& Platform : BuiltPlatforms)
		{
			GatherPlatformPieces(Platform, Pieces);
		}
	}

	int32 AddedPieces = 0;

	AddedPieces += SyncPieceLayer(EdgeLayer, EdgePieces, Pieces.Edges, PieceMesh, ResolveTintedMaterial(
		EdgePieceMaterial, PieceMaterial, PieceColorParameterName, EdgeColor));

	AddedPieces += SyncPieceLayer(NodeLayer, NodePieces, Pieces.Nodes, PieceMesh, ResolveTintedMaterial(
		NodePieceMaterial, PieceMaterial, PieceColorParameterName, NodeColor));

	// Every structure has a layer of its own. One with nothing left to build - no longer filling any
	// platform, or its mesh cleared - is taken down rather than kept empty.
	for (int32 LayerIndex = StructureLayers.Num() - 1; LayerIndex >= 0; --LayerIndex)
	{
		FStructureLayer& Layer = StructureLayers[LayerIndex];

		if (!Pieces.Structures.Contains(Layer.Structure.Get()))
		{
			ResetChunks(Layer.Pieces.Chunks, nullptr);
			StructureLayers.RemoveAt(LayerIndex);
		}
	}

	// A structure keeps its own materials, so no tint is put over it.
	for (const TPair<UPlatformStructureData*, TArray<FPlacedPiece>>& StructurePieces : Pieces.Structures)
	{
		FStructureLayer& Layer = FindOrAddStructureLayer(StructurePieces.Key);

		AddedPieces += SyncPieceLayer(
			Layer.Pieces, Layer.Pieces.Chunks[0], StructurePieces.Value, StructurePieces.Key->Mesh, /*Material=*/nullptr);
	}

	// Only a sync that built every platform can be added to one placement at a time.
	bPiecesComplete = bDrawPieces;

	return AddedPieces;
}

void ARoomManager::AppendPlatformPieces(const FGridPlatform& Platform)
{
	bool bLayersIntact = IsPieceLayerIntact(EdgeLayer, EdgePieces) && IsPieceLayerIntact(NodeLayer, NodePieces);

	for (const FStructureLayer& Layer : StructureLayers)
	{
		bLayersIntact = bLayersIntact
			&& !Layer.Pieces.Chunks.IsEmpty()
			&& IsPieceLayerIntact(Layer.Pieces, Layer.Pieces.Chunks[0]);
	}

	// Anything but the plain case - pieces switched off, never built in full, or a layer out of
	// step with its record - is left to the full sync.
	if (!bPiecesComplete || !bShowPieces || !PieceMesh || !bLayersIntact)
	{
		SyncPieces();
		return;
	}

	FGatheredPieces Pieces;
	GatherPlatformPieces(Platform, Pieces);

	// An edge or a node a neighbour already built is skipped, so a shared piece is still built once.
	auto AppendToLayer = [](
		FRoomPieceLayer& Layer,
		const TConstArrayView<FPlacedPiece> LayerPieces,
		TArray<FTransform>& OutNewPieces)
	{
		for (const FPlacedPiece& Piece : LayerPieces)
		{
			uint32& Stamp = Layer.PieceStamps.FindOrAdd(Piece.Key);

			// Any stamp but zero marks a piece as drawn, and the full sync that made this layer
			// complete - or the making of a structure's layer - left SyncStamp above zero.
			if (Stamp == 0)
			{
				Stamp = Layer.SyncStamp;
				OutNewPieces.Add(Piece.Transform);
			}
		}
	};

	auto AppendToFixedLayer = [this, &AppendToLayer](
		FRoomPieceLayer& Layer,
		UInstancedStaticMeshComponent* FirstChunk,
		const TConstArrayView<FPlacedPiece> LayerPieces,
		UMaterialInterface* Material)
	{
		ApplyMeshAndMaterial(Layer.Chunks, PieceMesh, Material);

		TArray<FTransform> NewPieces;
		AppendToLayer(Layer, LayerPieces, NewPieces);
		AddPiecesToLayer(Layer, FirstChunk, NewPieces);
	};

	AppendToFixedLayer(EdgeLayer, EdgePieces, Pieces.Edges, ResolveTintedMaterial(
		EdgePieceMaterial, PieceMaterial, PieceColorParameterName, EdgeColor));

	AppendToFixedLayer(NodeLayer, NodePieces, Pieces.Nodes, ResolveTintedMaterial(
		NodePieceMaterial, PieceMaterial, PieceColorParameterName, NodeColor));

	for (const TPair<UPlatformStructureData*, TArray<FPlacedPiece>>& StructurePieces : Pieces.Structures)
	{
		FStructureLayer& Layer = FindOrAddStructureLayer(StructurePieces.Key);
		ApplyMeshAndMaterial(Layer.Pieces.Chunks, StructurePieces.Key->Mesh, /*Material=*/nullptr);

		TArray<FTransform> NewPieces;
		AppendToLayer(Layer.Pieces, StructurePieces.Value, NewPieces);
		AddPiecesToLayer(Layer.Pieces, Layer.Pieces.Chunks[0], NewPieces);
	}
}

void ARoomManager::GatherPlatformPieces(const FGridPlatform& Platform, FGatheredPieces& OutPieces) const
{
	int32 SpanU, SpanV;
	GetPlatformSpans(Platform, SpanU, SpanV);

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);

	// Everything on the lattice is axis-aligned, so every box is the unit cube scaled to the
	// cells it fills, and none needs turning.
	auto MakeBox = [this](const FIntVector& MinCell, const FVector& Extent) -> FTransform
	{
		return FTransform(FRotator::ZeroRotator, GetCellMinCorner(MinCell) + 0.5 * Extent, Extent / UnitCubeSize);
	};

	// The opening is filled by its structures, each laid on the face it fills. A two-sided one is a
	// single piece through both faces; a one-sided pair is two, keyed apart. A structure that no
	// longer fits the opening - its size changed since it was drawn - is not drawn across it.
	const uint64 PlatformKey = MakePlatformKey(Platform);
	const FVector MinCorner = GetCellMinCorner(Platform.MinNode);

	// A two-sided one was modelled lying on the ground, so on a floor it faces the Ground face; on a
	// wall, which has no such side, the positive one.
	const int32 TwoSidedSign = Platform.IsWall() ? 1 : FaceSign(Platform.Up);

	auto AddStructure = [&](UPlatformStructureData* Structure, const int32 Sign, const uint64 Key)
	{
		bool bTurned = false;

		if (!Structure || !Structure->Mesh
			|| !WEMStructures::FitsShape(Structure->Width, Structure->Length, Structure->bMayLieSideways, NormalAxis, Platform.Up, SpanU, SpanV, bTurned))
		{
			return;
		}

		OutPieces.Structures.FindOrAdd(Structure).Add(FPlacedPiece{
			Key, WEMStructures::MakeStructureTransform(MinCorner, CellSize, NormalAxis, Platform.Up, SpanU, SpanV, Sign, bTurned, Structure->bTwoSided) });
	};

	if (Platform.PositiveStructure && Platform.PositiveStructure->bTwoSided)
	{
		AddStructure(Platform.PositiveStructure, TwoSidedSign, PlatformKey);
	}
	else
	{
		AddStructure(Platform.PositiveStructure, 1, PlatformKey);
		AddStructure(Platform.NegativeStructure, -1, PlatformKey | NegativeFacePieceBit);
	}

	// The rim is drawn per module edge and per node, each named by what it is rather than by the
	// platform that brought it. A neighbour sharing one names the same piece, so it is built
	// once - even where a neighbour shares only part of a side - and no two beams, nor a beam
	// and a post, can ever overlap.
	const double RunExtent = static_cast<double>(Module - 1) * CellSize;

	// A beam every platform around it has blocked is not built, nor a post no built beam ends at.
	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		if (!bRim || !IsEdgeBuilt(MakeEdgeKey(Edge)))
		{
			return;
		}

		FVector EdgeExtent(CellSize);
		EdgeExtent[Edge.Axis] = RunExtent;

		OutPieces.Edges.Add(FPlacedPiece{ MakeEdgeKey(Edge), MakeBox(Edge.Node + AxisStep(Edge.Axis), EdgeExtent) });
	});

	ForEachRimNode(Platform, SpanU, SpanV, Module, [&](const FIntVector& Node)
	{
		if (IsNodeBuilt(Node))
		{
			OutPieces.Nodes.Add(FPlacedPiece{ MakeCellKey(Node), MakeBox(Node, FVector(CellSize)) });
		}
	});
}

void ARoomManager::ResetPieces()
{
	ResetPieceLayer(EdgeLayer, EdgePieces);
	ResetPieceLayer(NodeLayer, NodePieces);
	ResetStructureLayers();

	bPiecesComplete = false;
}

FStructureLayer& ARoomManager::FindOrAddStructureLayer(UPlatformStructureData* Structure)
{
	FStructureLayer* Layer = StructureLayers.FindByPredicate([Structure](const FStructureLayer& Existing)
	{
		return Existing.Structure == Structure;
	});

	if (!Layer)
	{
		Layer = &StructureLayers.AddDefaulted_GetRef();
		Layer->Structure = Structure;
	}

	// Its first chunk is made here rather than being one of the actor's own, so if that has been
	// lost there is nothing of the layer worth keeping: it starts again.
	if (Layer->Pieces.Chunks.IsEmpty() || !IsValid(Layer->Pieces.Chunks[0]))
	{
		ResetChunks(Layer->Pieces.Chunks, nullptr);
		Layer->Pieces.PieceStamps.Reset();

		// A stamp of zero means "not drawn yet", so the layer's own has to start above it.
		Layer->Pieces.SyncStamp = FMath::Max<uint32>(Layer->Pieces.SyncStamp, 1);

		// Set up like the beams it stands among: lit, casting shadows, with no collision. The
		// structure's mesh, with its own materials, goes on when the layer is first filled.
		AddInstancedChunk(Layer->Pieces.Chunks, EdgePieces, FName(*FString::Printf(TEXT("%s_Pieces"), *Structure->GetName())));
	}

	return *Layer;
}

void ARoomManager::ResetStructureLayers()
{
	for (FStructureLayer& Layer : StructureLayers)
	{
		ResetChunks(Layer.Pieces.Chunks, nullptr);
	}

	StructureLayers.Reset();
}

int32 ARoomManager::SyncPieceLayer(
	FRoomPieceLayer& Layer,
	UInstancedStaticMeshComponent* FirstChunk,
	const TConstArrayView<FPlacedPiece> Pieces,
	UStaticMesh* Mesh,
	UMaterialInterface* Material)
{
	if (!FirstChunk)
	{
		return 0;
	}

	// The record is neither saved nor duplicated, while the first chunk is both - so a loaded
	// level or a PIE copy arrives with instances the record knows nothing about. A chunk can
	// also be lost from under it. Either way the two no longer agree: start the layer over.
	if (!IsPieceLayerIntact(Layer, FirstChunk))
	{
		ResetPieceLayer(Layer, FirstChunk);
	}

	ApplyMeshAndMaterial(Layer.Chunks, Mesh, Material);

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

	AddPiecesToLayer(Layer, FirstChunk, NewPieces);

	return NewPieces.Num();
}

bool ARoomManager::IsPieceLayerIntact(const FRoomPieceLayer& Layer, const UInstancedStaticMeshComponent* FirstChunk) const
{
	if (!FirstChunk || Layer.Chunks.IsEmpty() || Layer.Chunks[0].Get() != FirstChunk)
	{
		return false;
	}

	int32 DrawnPieces = 0;

	for (const UInstancedStaticMeshComponent* Chunk : Layer.Chunks)
	{
		if (!IsValid(Chunk))
		{
			return false;
		}

		DrawnPieces += Chunk->GetInstanceCount();
	}

	return DrawnPieces == Layer.PieceStamps.Num();
}

void ARoomManager::AddPiecesToLayer(
	FRoomPieceLayer& Layer,
	UInstancedStaticMeshComponent* FirstChunk,
	const TConstArrayView<FTransform> NewPieces)
{
	// New pieces go into the chunk still being filled, and a fresh chunk is started once that
	// one is full. Those are the only components whose instance count changes, so they are the
	// only ones whose surface cache Lumen has to capture again.
	for (int32 NextPiece = 0; NextPiece < NewPieces.Num();)
	{
		UInstancedStaticMeshComponent* OpenChunk = Layer.Chunks.Last();
		int32 Room = PieceChunkCapacity - OpenChunk->GetInstanceCount();

		if (Room <= 0)
		{
			OpenChunk = AddInstancedChunk(Layer.Chunks, FirstChunk);
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
	ResetChunks(Layer.Chunks, FirstChunk);
	Layer.PieceStamps.Reset();
}

void ARoomManager::ResetChunks(
	TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Chunks,
	UInstancedStaticMeshComponent* FirstChunk)
{
	for (UInstancedStaticMeshComponent* Chunk : Chunks)
	{
		if (Chunk != FirstChunk && IsValid(Chunk))
		{
			Chunk->DestroyComponent();
		}
	}

	Chunks.Reset();

	if (FirstChunk)
	{
		if (FirstChunk->GetInstanceCount() > 0)
		{
			FirstChunk->ClearInstances();
		}

		Chunks.Add(FirstChunk);
	}
}

UInstancedStaticMeshComponent* ARoomManager::AddInstancedChunk(
	TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Chunks,
	UInstancedStaticMeshComponent* FirstChunk,
	const FName BaseName)
{
	// Named after the first chunk, so the component list reads EdgePieces, EdgePieces_1, ...
	const FName ChunkName = MakeUniqueObjectName(
		this, UInstancedStaticMeshComponent::StaticClass(), BaseName.IsNone() ? FirstChunk->GetFName() : BaseName);

	// Rebuilt from the platform list whenever it is needed, so never saved, and never copied
	// along with the actor - a PIE or pasted copy starts from its own first chunks and grows its own.
	UInstancedStaticMeshComponent* Chunk = NewObject<UInstancedStaticMeshComponent>(
		this, ChunkName, RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);

	// Whatever makes this kind of piece or square what it is comes from the first chunk, so the
	// constructor stays the one place each kind is set up.
	Chunk->SetMobility(FirstChunk->Mobility);
	Chunk->SetCollisionEnabled(FirstChunk->GetCollisionEnabled());
	Chunk->SetCastShadow(FirstChunk->CastShadow);
	Chunk->bAffectDistanceFieldLighting = FirstChunk->bAffectDistanceFieldLighting;
	Chunk->bAffectDynamicIndirectLighting = FirstChunk->bAffectDynamicIndirectLighting;
	Chunk->SetVisibleInRayTracing(FirstChunk->bVisibleInRayTracing);
	Chunk->SetStaticMesh(FirstChunk->GetStaticMesh());
	Chunk->SetMaterial(0, FirstChunk->OverrideMaterials.IsValidIndex(0) ? FirstChunk->OverrideMaterials[0].Get() : nullptr);
	Chunk->SetupAttachment(SceneRoot);
	Chunk->RegisterComponent();

	Chunks.Add(Chunk);
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

UInstancedStaticMeshComponent* ARoomManager::GetSurfaceMarkComponent(const int32 Layer) const
{
	switch (Layer)
	{
	case AllObjectMarkLayer: return AllObjectSurfaceMarks;
	case WallMarkLayer: return WallSurfaceMarks;
	case OccupiedMarkLayer: return OccupiedSurfaceMarks;
	default: return nullptr;
	}
}

FSurfaceMarkLayer& ARoomManager::GetSurfaceMarkChunks(const int32 Layer)
{
	switch (Layer)
	{
	case AllObjectMarkLayer: return AllObjectMarkChunks;
	case WallMarkLayer: return WallMarkChunks;
	default: return OccupiedMarkChunks;
	}
}

const FSurfaceMarkLayer& ARoomManager::GetSurfaceMarkChunks(const int32 Layer) const
{
	return const_cast<ARoomManager*>(this)->GetSurfaceMarkChunks(Layer);
}

void ARoomManager::ResetSurfaceMarks()
{
	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		FSurfaceMarkLayer& Marks = GetSurfaceMarkChunks(Layer);

		ResetChunks(Marks.Chunks, GetSurfaceMarkComponent(Layer));

		Marks.ChunkKeys.Reset();
		Marks.ChunkKeys.SetNum(Marks.Chunks.Num());
	}

	SurfaceMarkSlots.Reset();
	bSurfaceMarksComplete = false;
}

bool ARoomManager::CanDrawSurfaceMarks() const
{
	return bShowSurfaces && PlaneMesh && !BuiltPlatforms.IsEmpty() && GetSurfaceMarkSpan(CellSize) > 0.0;
}

bool ARoomManager::AreSurfaceMarksIntact() const
{
	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		const UInstancedStaticMeshComponent* FirstChunk = GetSurfaceMarkComponent(Layer);
		const FSurfaceMarkLayer& Marks = GetSurfaceMarkChunks(Layer);

		if (!FirstChunk
			|| Marks.Chunks.IsEmpty()
			|| Marks.Chunks[0].Get() != FirstChunk
			|| Marks.ChunkKeys.Num() != Marks.Chunks.Num())
		{
			return false;
		}

		for (int32 ChunkIndex = 0; ChunkIndex < Marks.Chunks.Num(); ++ChunkIndex)
		{
			const UInstancedStaticMeshComponent* Chunk = Marks.Chunks[ChunkIndex];

			if (!IsValid(Chunk) || Chunk->GetInstanceCount() != Marks.ChunkKeys[ChunkIndex].Num())
			{
				return false;
			}
		}
	}

	return true;
}

void ARoomManager::RefreshSurfaceMarkMaterials()
{
	const FColor LayerColors[SurfaceMarkLayerCount] =
		{ AllObjectSurfaceColor, WallSurfaceColor, OccupiedSurfaceColor };

	TObjectPtr<UMaterialInstanceDynamic>* const LayerMaterials[SurfaceMarkLayerCount] =
		{ &AllObjectMarkMaterial, &WallMarkMaterial, &OccupiedMarkMaterial };

	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		UMaterialInterface* Material = ResolveTintedMaterial(
			*LayerMaterials[Layer], PlaneMaterial, PlaneColorParameterName, FLinearColor(LayerColors[Layer]));

		ApplyMeshAndMaterial(GetSurfaceMarkChunks(Layer).Chunks, PlaneMesh, Material);
	}
}

void ARoomManager::RemoveSurfaceMark(const int32 Layer, const int32 Chunk, const int32 Instance)
{
	FSurfaceMarkLayer& Marks = GetSurfaceMarkChunks(Layer);

	if (!Marks.Chunks.IsValidIndex(Chunk)
		|| !Marks.ChunkKeys.IsValidIndex(Chunk)
		|| !Marks.ChunkKeys[Chunk].IsValidIndex(Instance))
	{
		return;
	}

	UInstancedStaticMeshComponent* Component = Marks.Chunks[Chunk];
	TArray<uint64>& Keys = Marks.ChunkKeys[Chunk];
	const int32 Last = Keys.Num() - 1;

	// Only the chunk's last instance is ever removed, so no other square changes index and the
	// record needs one fix-up at most: the last square is moved into the hole first. The
	// component has a swap-removal of its own, but it is free to ignore being asked for it,
	// which would leave this record guessing.
	if (Instance != Last)
	{
		FTransform LastTransform;
		Component->GetInstanceTransform(Last, LastTransform, /*bWorldSpace=*/true);
		Component->UpdateInstanceTransform(Instance, LastTransform, /*bWorldSpace=*/true);

		Keys[Instance] = Keys[Last];
		SurfaceMarkSlots.FindChecked(Keys[Instance]).Instance = Instance;
	}

	Component->RemoveInstance(Last);
	Keys.Pop(EAllowShrinking::No);
}

int32 ARoomManager::ResolveSurfaceMarkLayer(const FIntVector& Cell, const EGridFace Face) const
{
	ESurfaceCapacity Capacity = ESurfaceCapacity::Empty;
	bool bOccupied = false;
	ResolveSurface(Cell, Face, Capacity, bOccupied);

	if (Capacity == ESurfaceCapacity::Empty)
	{
		return INDEX_NONE;
	}

	// Occupancy is drawn instead of the capacity, since what a surface is free to take is the
	// more useful reading and a built-against surface has stopped offering it.
	return bOccupied
		? OccupiedMarkLayer
		: (Capacity == ESurfaceCapacity::AllObject ? AllObjectMarkLayer : WallMarkLayer);
}

bool ARoomManager::SyncSurfaceMark(
	const FIntVector& Cell,
	const EGridFace Face,
	const uint64 Key,
	const int32 Layer,
	FSurfaceMarkSlot& Slot,
	FPendingSurfaceMarks& Pending)
{
	if (Slot.Layer == Layer)
	{
		return false;
	}

	if (Slot.Instance != INDEX_NONE)
	{
		RemoveSurfaceMark(Slot.Layer, Slot.Chunk, Slot.Instance);
	}

	Slot.Layer = Layer;
	Slot.Chunk = INDEX_NONE;
	Slot.Instance = INDEX_NONE;

	// Lying on the face, turned to look out along its normal, and stood off it a little so it
	// never z-fights the box it marks.
	const FTransform FaceTransform = GetSurfaceWorldTransform(Cell, Face);
	const FVector Outward(FaceStep(Face));

	Pending.Transforms[Layer].Emplace(
		FaceTransform.GetRotation(), FaceTransform.GetLocation() + Outward * SurfaceOverlayBias, Pending.Scale);
	Pending.Keys[Layer].Add(Key);

	return true;
}

void ARoomManager::AddPendingSurfaceMarks(const FPendingSurfaceMarks& Pending)
{
	for (int32 Layer = 0; Layer < SurfaceMarkLayerCount; ++Layer)
	{
		const TArray<FTransform>& Transforms = Pending.Transforms[Layer];
		const TArray<uint64>& Keys = Pending.Keys[Layer];

		FSurfaceMarkLayer& Marks = GetSurfaceMarkChunks(Layer);
		UInstancedStaticMeshComponent* FirstChunk = GetSurfaceMarkComponent(Layer);

		// New squares go into the last chunk, and a fresh one is started once that is full.
		for (int32 NextMark = 0; NextMark < Transforms.Num();)
		{
			int32 ChunkIndex = Marks.Chunks.Num() - 1;
			int32 Room = SurfaceMarkChunkCapacity - Marks.ChunkKeys[ChunkIndex].Num();

			if (Room <= 0)
			{
				AddInstancedChunk(Marks.Chunks, FirstChunk);
				Marks.ChunkKeys.AddDefaulted();

				++ChunkIndex;
				Room = SurfaceMarkChunkCapacity;
			}

			const int32 Count = FMath::Min(Room, Transforms.Num() - NextMark);
			TArray<uint64>& ChunkKeys = Marks.ChunkKeys[ChunkIndex];
			const int32 FirstInstance = ChunkKeys.Num();

			// One batch per chunk, for the same reason the pieces go in that way.
			Marks.Chunks[ChunkIndex]->AddInstances(
				TArray<FTransform>(Transforms.GetData() + NextMark, Count),
				/*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);

			for (int32 Offset = 0; Offset < Count; ++Offset)
			{
				const uint64 Key = Keys[NextMark + Offset];

				FSurfaceMarkSlot& Slot = SurfaceMarkSlots.FindChecked(Key);
				Slot.Chunk = ChunkIndex;
				Slot.Instance = FirstInstance + Offset;

				ChunkKeys.Add(Key);
			}

			NextMark += Count;
		}
	}
}

int32 ARoomManager::SyncSurfaceMarks()
{
	if (!AllObjectSurfaceMarks || !WallSurfaceMarks || !OccupiedSurfaceMarks)
	{
		return 0;
	}

	if (!CanDrawSurfaceMarks())
	{
		ResetSurfaceMarks();
		return 0;
	}

	// The record is neither saved nor duplicated with the chunks, so a loaded level or a PIE
	// copy arrives with squares it knows nothing about. Start clean rather than add to them.
	if (!AreSurfaceMarksIntact())
	{
		ResetSurfaceMarks();
	}

	RefreshSurfaceMarkMaterials();

	const FVector MarkScale = MakeSurfaceMarkScale(CellSize);

	FPendingSurfaceMarks Pending;
	Pending.Scale = MarkScale;

	int32 ChangedMarks = 0;

	// One pass over every surface. A square already the right colour is left alone; one whose
	// surface has changed - a seam promoted into floorspace, a wall folded off it - moves to the
	// right colour; a surface that has appeared since the last pass gets a new square. Returns
	// how many surfaces the record holds that the pass no longer found.
	auto WalkSurfaces = [&]() -> int32
	{
		++SurfaceMarkSyncStamp;
		int32 FoundSurfaces = 0;

		// Every surface is one of the two broad faces of a platform's cell, so walking the
		// platforms finds every one of them - without asking the cube's millions of cells, nearly
		// all empty, one at a time.
		for (const FGridPlatform& Platform : BuiltPlatforms)
		{
			const int32 NormalAxis = AxisIndex(Platform.Normal);
			const EGridFace PlatformFaces[2] = { MakeFace(NormalAxis, 1), MakeFace(NormalAxis, -1) };

			int32 SpanU, SpanV;
			GetPlatformSpans(Platform, SpanU, SpanV);

			ForEachPlatformCell(Platform, SpanU, SpanV, [&](const FIntVector& Cell, bool)
			{
				for (const EGridFace Face : PlatformFaces)
				{
					// A side its structure closes off is no surface, and never has a square.
					const int32 Layer = ResolveSurfaceMarkLayer(Cell, Face);

					if (Layer == INDEX_NONE)
					{
						continue;
					}

					const uint64 Key = MakeFaceKey(Cell, Face);
					FSurfaceMarkSlot& Slot = SurfaceMarkSlots.FindOrAdd(Key);

					// Counted once however often the platforms name it, so the tally stays honest.
					if (Slot.SeenStamp == SurfaceMarkSyncStamp)
					{
						continue;
					}

					Slot.SeenStamp = SurfaceMarkSyncStamp;
					++FoundSurfaces;

					if (SyncSurfaceMark(Cell, Face, Key, Layer, Slot, Pending))
					{
						++ChangedMarks;
					}
				}
			});
		}

		return SurfaceMarkSlots.Num() - FoundSurfaces;
	};

	// Placement only ever adds, so a surface once found stays a surface and a square never has
	// to be taken away. If that stops holding the tally will not match: redraw from scratch.
	const int32 StaleSurfaces = WalkSurfaces();

	if (StaleSurfaces > 0)
	{
		ChangedMarks += StaleSurfaces;

		ResetSurfaceMarks();

		Pending = FPendingSurfaceMarks();
		Pending.Scale = MarkScale;

		WalkSurfaces();
	}

	AddPendingSurfaceMarks(Pending);

	// Every surface now has its square, so placements may add to the overlay from here.
	bSurfaceMarksComplete = true;

	return ChangedMarks;
}

void ARoomManager::UpdateSurfaceMarks(const TConstArrayView<FGridSurfaceRef> Surfaces)
{
	// Anything but the plain case - the overlay switched off, never drawn in full, or out of step
	// with its record - is left to the full sync.
	if (!bSurfaceMarksComplete || !CanDrawSurfaceMarks() || !AreSurfaceMarksIntact())
	{
		SyncSurfaceMarks();
		return;
	}

	RefreshSurfaceMarkMaterials();

	FPendingSurfaceMarks Pending;
	Pending.Scale = MakeSurfaceMarkScale(CellSize);

	// A surface can be named more than once; after the first it is already the right colour.
	for (const FGridSurfaceRef& Surface : Surfaces)
	{
		// A side its structure closes off is no surface, and never has a square.
		const int32 Layer = ResolveSurfaceMarkLayer(Surface.Cell, Surface.Face);

		if (Layer == INDEX_NONE)
		{
			continue;
		}

		const uint64 Key = MakeFaceKey(Surface.Cell, Surface.Face);
		SyncSurfaceMark(Surface.Cell, Surface.Face, Key, Layer, SurfaceMarkSlots.FindOrAdd(Key), Pending);
	}

	AddPendingSurfaceMarks(Pending);
}
