// Copyright Epic Games, Inc. All Rights Reserved.

#include "RoomManager.h"

#include "PlatformTileData.h"
#include "Algo/BinarySearch.h"
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

	TAutoConsoleVariable<int32> CVarRoomManagerVerifyEvery(
		TEXT("wem.RoomManager.VerifyEvery"),
		0,
		TEXT("Every Nth placement, rebuilds ARoomManager's derived state and visuals from scratch and logs anything the\n")
		TEXT("placements had built up differently. Costs a full rebuild each time, so leave it at 0 (off) outside of testing."),
		ECVF_Default);

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

	/**
	 * The two axes a platform facing along NormalAxis lies across, in the order its tile's width
	 * and length run: the first is always level, and up a wall the second is height. Across a
	 * floor they are X and Y; along a wall facing X, Y and Z; along one facing Y, X and Z.
	 */
	FORCEINLINE void GetInPlaneAxes(const int32 NormalAxis, int32& OutFirst, int32& OutSecond)
	{
		OutFirst = NormalAxis == 0 ? 1 : 0;
		OutSecond = NormalAxis == 2 ? 1 : 2;
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
	// The module comes first, since the cube is rounded to whole modules of it.
	LatticeModule = GetModule();
	GridSize = ResolveGridSize();

	// One height sample for the whole cube, resolved before anything asks for a cell position.
	GridBaseZ = ResolveGridBaseZ();

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

void ARoomManager::GatherFirstPlatformCandidates(TArray<FGridPlatform>& OutCandidates) const
{
	OutCandidates.Reset();

	if (!IsTileUsable(FirstTile))
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

				if (!bAlreadyConsidered && CanPlacePlatform(Candidate))
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

	const bool bFree = CanPlacePlatform(Candidate);

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

		// The tile is rolled only among those with somewhere to go, so a tile with no room left
		// never costs the beat while another has some.
		auto GetDrawWeight = [Kind](const FTileCandidates& Candidates) -> float
		{
			return Candidates.ByKind[KindIndex(Kind)].IsEmpty() ? 0.0f : FMath::Max(Candidates.Tile->Weight, 0.0f);
		};

		float TotalWeight = 0.0f;
		for (const FTileCandidates& Candidates : TileCandidates)
		{
			TotalWeight += GetDrawWeight(Candidates);
		}

		if (TotalWeight <= 0.0f)
		{
			return false;
		}

		float Roll = PlacementStream.FRand() * TotalWeight;
		const FTileCandidates* ChosenTile = nullptr;

		for (const FTileCandidates& Candidates : TileCandidates)
		{
			const float Weight = GetDrawWeight(Candidates);

			if (Weight <= 0.0f)
			{
				continue;
			}

			// Taken whenever it is in the running, so rounding at the very top of the roll still
			// lands on the last tile that could have been drawn.
			ChosenTile = &Candidates;

			if (Roll < Weight)
			{
				break;
			}

			Roll -= Weight;
		}

		const TArray<FGridPlatform>& Candidates = ChosenTile->ByKind[KindIndex(Kind)];

		// Copied out, since taking the platform in changes the list it was drawn from.
		Platform = Candidates[PlacementStream.RandRange(0, Candidates.Num() - 1)];
	}

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

	if (!FirstTile)
	{
		Problems.Add(TEXT("there is no FirstTile, so nothing grows"));
	}
	else if (!IsTileUsable(FirstTile))
	{
		Problems.Add(FString::Printf(TEXT("FirstTile '%s' (%d x %d) is not a whole number of %d-cell modules along both sides, so nothing grows"),
			*FirstTile->GetName(), FirstTile->Width, FirstTile->Length, Module));
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

void ARoomManager::RebuildPlatformState()
{
	// The tiles first, since which of them fit the lattice decides which platforms do.
	ResolveUsableTiles();

	BuiltPlatforms.Reset();
	ModuleFaceOwners.Reset();
	ModuleEdgeUses.Reset();
	RimCellNormals.Reset();
	PromotedFaces.Reset();

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

	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [this](const FLatticeEdge& Edge, const bool bRim)
	{
		ModuleEdgeUses.FindOrAdd(MakeEdgeKey(Edge)) |= bRim ? RimEdgeUse : InteriorEdgeUse;
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
	const TSet<uint64> IncrementalPromotedFaces = PromotedFaces;
	const bool bHadCandidates = !bPlatformCandidatesStale;
	const TArray<FTileCandidates> IncrementalCandidates = TileCandidates;

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
	CompareKeySets(TEXT("promoted surfaces"), IncrementalPromotedFaces, PromotedFaces);

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

	if (!Mismatches.IsEmpty())
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

	// Rim can have been brought by any of several platforms, so it is recorded.
	if (LatticePlaneCount >= 2)
	{
		if (const uint8* RimAxes = RimCellNormals.Find(MakeCellKey(Cell)))
		{
			return *RimAxes;
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
	if (!Platform.Tile)
	{
		return false;
	}

	// Top and bottom are the tile's own, and a platform always lies with its top looking out
	// along its normal - up from a floor, out along +X or +Y from a wall.
	switch (Platform.Tile->BlockedFace)
	{
	case ETileBlockedFace::Top: return FaceSign(Face) > 0;
	case ETileBlockedFace::Bottom: return FaceSign(Face) < 0;
	default: return false;
	}
}

bool ARoomManager::IsSolidCell(const FIntVector& Cell) const
{
	int32 InteriorOwner = INDEX_NONE;

	return GetCellSurfaceAxes(Cell, InteriorOwner) != 0;
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

	// A side a tile has blocked off is not a surface at all. Only an interior can be: the rim
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

						// A side a tile has blocked off is no surface to find.
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
	if (!HorizontalInteriorPieces || !VerticalInteriorPieces || !EdgePieces || !NodePieces)
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

	AddedPieces += SyncPieceLayer(HorizontalInteriorLayer, HorizontalInteriorPieces, Pieces.HorizontalInteriors, PieceMesh, ResolveTintedMaterial(
		HorizontalInteriorPieceMaterial, PieceMaterial, PieceColorParameterName, HorizontalInteriorColor));

	AddedPieces += SyncPieceLayer(VerticalInteriorLayer, VerticalInteriorPieces, Pieces.VerticalInteriors, PieceMesh, ResolveTintedMaterial(
		VerticalInteriorPieceMaterial, PieceMaterial, PieceColorParameterName, VerticalInteriorColor));

	AddedPieces += SyncPieceLayer(EdgeLayer, EdgePieces, Pieces.Edges, PieceMesh, ResolveTintedMaterial(
		EdgePieceMaterial, PieceMaterial, PieceColorParameterName, EdgeColor));

	AddedPieces += SyncPieceLayer(NodeLayer, NodePieces, Pieces.Nodes, PieceMesh, ResolveTintedMaterial(
		NodePieceMaterial, PieceMaterial, PieceColorParameterName, NodeColor));

	// A tile that brings its own mesh has a layer of its own. One whose tile has nothing left to
	// build - taken out of the structure, or its mesh cleared - is taken down rather than kept empty.
	for (int32 LayerIndex = TileMeshLayers.Num() - 1; LayerIndex >= 0; --LayerIndex)
	{
		FTileMeshLayer& Layer = TileMeshLayers[LayerIndex];

		if (!Pieces.MeshInteriors.Contains(Layer.Tile.Get()))
		{
			ResetChunks(Layer.Pieces.Chunks, nullptr);
			TileMeshLayers.RemoveAt(LayerIndex);
		}
	}

	// The mesh keeps its own materials, so no tint is put over it.
	for (const TPair<UPlatformTileData*, TArray<FPlacedPiece>>& MeshInteriors : Pieces.MeshInteriors)
	{
		FTileMeshLayer& Layer = FindOrAddTileMeshLayer(MeshInteriors.Key);

		AddedPieces += SyncPieceLayer(
			Layer.Pieces, Layer.Pieces.Chunks[0], MeshInteriors.Value, MeshInteriors.Key->Mesh, /*Material=*/nullptr);
	}

	// Only a sync that built every platform can be added to one placement at a time.
	bPiecesComplete = bDrawPieces;

	return AddedPieces;
}

void ARoomManager::AppendPlatformPieces(const FGridPlatform& Platform)
{
	bool bLayersIntact = IsPieceLayerIntact(HorizontalInteriorLayer, HorizontalInteriorPieces)
		&& IsPieceLayerIntact(VerticalInteriorLayer, VerticalInteriorPieces)
		&& IsPieceLayerIntact(EdgeLayer, EdgePieces)
		&& IsPieceLayerIntact(NodeLayer, NodePieces);

	for (const FTileMeshLayer& Layer : TileMeshLayers)
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
			// complete - or the making of a tile's layer - left SyncStamp above zero.
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

	AppendToFixedLayer(HorizontalInteriorLayer, HorizontalInteriorPieces, Pieces.HorizontalInteriors, ResolveTintedMaterial(
		HorizontalInteriorPieceMaterial, PieceMaterial, PieceColorParameterName, HorizontalInteriorColor));

	AppendToFixedLayer(VerticalInteriorLayer, VerticalInteriorPieces, Pieces.VerticalInteriors, ResolveTintedMaterial(
		VerticalInteriorPieceMaterial, PieceMaterial, PieceColorParameterName, VerticalInteriorColor));

	AppendToFixedLayer(EdgeLayer, EdgePieces, Pieces.Edges, ResolveTintedMaterial(
		EdgePieceMaterial, PieceMaterial, PieceColorParameterName, EdgeColor));

	AppendToFixedLayer(NodeLayer, NodePieces, Pieces.Nodes, ResolveTintedMaterial(
		NodePieceMaterial, PieceMaterial, PieceColorParameterName, NodeColor));

	for (const TPair<UPlatformTileData*, TArray<FPlacedPiece>>& MeshInteriors : Pieces.MeshInteriors)
	{
		FTileMeshLayer& Layer = FindOrAddTileMeshLayer(MeshInteriors.Key);
		ApplyMeshAndMaterial(Layer.Pieces.Chunks, MeshInteriors.Key->Mesh, /*Material=*/nullptr);

		TArray<FTransform> NewPieces;
		AppendToLayer(Layer.Pieces, MeshInteriors.Value, NewPieces);
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

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// One piece for the whole interior - a platform is one thing that was placed, so it is drawn
	// as one thing.
	FVector InteriorExtent(CellSize);
	InteriorExtent[U] = static_cast<double>(SpanU - 1) * CellSize;
	InteriorExtent[V] = static_cast<double>(SpanV - 1) * CellSize;

	const FIntVector InteriorMinCell = Platform.MinNode + AxisStep(U) + AxisStep(V);
	const uint64 PlatformKey = MakePlatformKey(Platform);

	if (Platform.Tile && Platform.Tile->Mesh)
	{
		// The tile's own mesh stands at the interior's centre at its own size, its top looking
		// out along the platform's normal and its width running wherever the tile's width does.
		const FVector Normal(AxisStep(NormalAxis));
		const FVector WidthDirection(AxisStep(Platform.bTurned ? V : U));
		const FVector Centre = GetCellMinCorner(InteriorMinCell) + 0.5 * InteriorExtent;

		OutPieces.MeshInteriors.FindOrAdd(Platform.Tile.Get()).Add(FPlacedPiece{
			PlatformKey, FTransform(FRotationMatrix::MakeFromXZ(WidthDirection, Normal).ToQuat(), Centre) });
	}
	else
	{
		TArray<FPlacedPiece>& InteriorBoxes = Platform.GetKind() == EPlatformKind::SurfaceHorizontal
			? OutPieces.HorizontalInteriors
			: OutPieces.VerticalInteriors;

		InteriorBoxes.Add(FPlacedPiece{ PlatformKey, MakeBox(InteriorMinCell, InteriorExtent) });
	}

	// The rim is drawn per module edge and per node, each named by what it is rather than by the
	// platform that brought it. A neighbour sharing one names the same piece, so it is built
	// once - even where a neighbour shares only part of a side - and no two beams, nor a beam
	// and a post, can ever overlap.
	const double RunExtent = static_cast<double>(Module - 1) * CellSize;

	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		if (!bRim)
		{
			return;
		}

		FVector EdgeExtent(CellSize);
		EdgeExtent[Edge.Axis] = RunExtent;

		OutPieces.Edges.Add(FPlacedPiece{ MakeEdgeKey(Edge), MakeBox(Edge.Node + AxisStep(Edge.Axis), EdgeExtent) });
	});

	ForEachRimNode(Platform, SpanU, SpanV, Module, [&](const FIntVector& Node)
	{
		OutPieces.Nodes.Add(FPlacedPiece{ MakeCellKey(Node), MakeBox(Node, FVector(CellSize)) });
	});
}

void ARoomManager::ResetPieces()
{
	ResetPieceLayer(HorizontalInteriorLayer, HorizontalInteriorPieces);
	ResetPieceLayer(VerticalInteriorLayer, VerticalInteriorPieces);
	ResetPieceLayer(EdgeLayer, EdgePieces);
	ResetPieceLayer(NodeLayer, NodePieces);
	ResetTileMeshLayers();

	bPiecesComplete = false;
}

FTileMeshLayer& ARoomManager::FindOrAddTileMeshLayer(UPlatformTileData* Tile)
{
	FTileMeshLayer* Layer = TileMeshLayers.FindByPredicate([Tile](const FTileMeshLayer& Existing)
	{
		return Existing.Tile == Tile;
	});

	if (!Layer)
	{
		Layer = &TileMeshLayers.AddDefaulted_GetRef();
		Layer->Tile = Tile;
	}

	// Its first chunk is made here rather than being one of the actor's own, so if that has been
	// lost there is nothing of the layer worth keeping: it starts again.
	if (Layer->Pieces.Chunks.IsEmpty() || !IsValid(Layer->Pieces.Chunks[0]))
	{
		ResetChunks(Layer->Pieces.Chunks, nullptr);
		Layer->Pieces.PieceStamps.Reset();

		// A stamp of zero means "not drawn yet", so the layer's own has to start above it.
		Layer->Pieces.SyncStamp = FMath::Max<uint32>(Layer->Pieces.SyncStamp, 1);

		// Set up like the plain interiors it stands in for. The tile's mesh, with its own
		// materials, goes on when the layer is first filled.
		AddInstancedChunk(Layer->Pieces.Chunks, HorizontalInteriorPieces, FName(*FString::Printf(TEXT("%s_Pieces"), *Tile->GetName())));
	}

	return *Layer;
}

void ARoomManager::ResetTileMeshLayers()
{
	for (FTileMeshLayer& Layer : TileMeshLayers)
	{
		ResetChunks(Layer.Pieces.Chunks, nullptr);
	}

	TileMeshLayers.Reset();
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
					// A side a tile has blocked off is no surface, and never has a square.
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
		// A side a tile has blocked off is no surface, and never has a square.
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
