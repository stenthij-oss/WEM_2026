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
	 * The custom stencil value every piece is drawn into, beams, nodes and structures alike - or, with
	 * the overlay's share of segments below 100, every piece it picks. The AI overlay
	 * (WEM_2026Rendering's AIOverlay, console wem.AIOverlay.Stencil) finds the pieces on screen by it;
	 * the surface marks are notation, not pieces, and stay out.
	 */
	constexpr int32 AIOverlayStencil = 1;

	/**
	 * The custom stencil value of a piece the AI overlay leaves alone, with its share of segments below 100
	 * (wem.AIOverlay.SegmentPercent). Still drawn into custom depth, so it hides the pieces the overlay takes in behind it.
	 */
	constexpr int32 OutOfOverlayStencil = 0;

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

	TAutoConsoleVariable<int32> CVarRoomManagerRemoveByRebuild(
		TEXT("wem.RoomManager.RemoveByRebuild"),
		0,
		TEXT("1 takes platforms out of ARoomManager by rebuilding everything derived from the list (step A) rather than\n")
		TEXT("by undoing just what they brought (step B). Slower; for comparing the two."),
		ECVF_Default);

	/** The ways round a tile can be laid, one bit each, as GetFillableWays reports them. */
	constexpr uint8 AsLaidWay = 1 << 0;
	constexpr uint8 TurnedWay = 1 << 1;

	/**
	 * How many times a beat draws a placement again when the one it drew goes against a growth rule
	 * (see ARoomManager::GetRuleRedrawChance). Bounded, so a structure with nowhere the rules favour
	 * still grows, and a beat costs at most this many draws more.
	 */
	constexpr int32 MaxRuleRedraws = 4;

	/**
	 * How many times a beat draws a placement afresh when no fill can be drawn for the one it drew (see
	 * ARoomManager::DrawFill). Only a placement meeting nature can go unfilled.
	 */
	constexpr int32 MaxFillRedraws = 8;

	/**
	 * How many of the city's open placements a nature beat tries for a new field of ground, each looking
	 * either way. Sampled rather than walked, so the beat costs the same however large the city grows.
	 */
	constexpr int32 NatureSampleSeeds = 48;

	/** How many module edges of nature's rim a nature beat tries for what grows off nature: cliffs, pools and terraces. */
	constexpr int32 NatureSampleEdges = 24;

	/** How many places an air pocket is drawn at before it is left out for taking the first platform's place every time. */
	constexpr int32 MaxAirPocketPlaces = 16;

	/** Segments in each circle of a sphere's or cylinder's outline. */
	constexpr int32 AirPocketCircleSegments = 32;

	/** Size on screen, in pixels, of the point drawn at each lattice piece a pocket holds back. */
	constexpr float AirPocketPieceSize = 6.0f;

	/** Seconds between ticks of the clock the paths fall due by. */
	constexpr float PathClockSeconds = 0.05f;

	/** The most paths one tick of the clock draws afresh; any more due wait for the next tick. */
	constexpr int32 MaxPathRedrawsPerTick = 2;

	/**
	 * The most platforms with no health left one tick of the clock takes away. Each costs about what laying one does, and
	 * they are taken away together, so this bounds a tick's frame: 4 a tick is 80 a second.
	 */
	constexpr int32 MaxRemovalsPerTick = 4;

	/** How many places in its zone a first tile is drawn at before the zone is left without one. */
	constexpr int32 MaxSeedPlaces = 32;

	/** The air over the fields is bucketed in cubes of 2^this cells, so a placement asks only the boxes near it. */
	constexpr int32 AirBoxBucketShift = 5;

	/**
	 * The colour a zone is drawn in when ZoneColors names too few: one of nine hues across its layer, the same nine on
	 * every layer, darker on the bottom one and paler on the top one.
	 */
	FColor GetDefaultZoneColor(const int32 Zone, const int32 ZonesPerLayer)
	{
		const int32 Layer = Zone / ZonesPerLayer;
		const uint8 Hue = static_cast<uint8>((Zone % ZonesPerLayer) * 256 / ZonesPerLayer);

		return FLinearColor::MakeFromHSV8(Hue, Layer >= 2 ? 120 : 255, Layer == 0 ? 150 : 255).ToFColor(/*bSRGB=*/true);
	}

	/** A bucket of the air over the fields, named by its coordinates in buckets. */
	FORCEINLINE uint64 MakeAirBucketKey(const int32 X, const int32 Y, const int32 Z)
	{
		return WEMGrid::MakeCellKey(FIntVector(X, Y, Z));
	}

	/** A one-sided structure on a platform's negative face is keyed apart from the one on its positive face. */
	constexpr uint64 NegativeFacePieceBit = uint64(1) << 63;

	/**
	 * A beam or post cut down at a seam in a sheet of water, and the water over it, keyed apart from the whole piece. No
	 * tag reaches this bit, so in a structure's layer it cannot meet a platform's key either.
	 */
	constexpr uint64 WaterSeamPieceBit = uint64(1) << 62;

	/**
	 * The share of segments, in percent, the AI overlay takes over: wem.AIOverlay.SegmentPercent, made by
	 * WEM_2026Rendering, which loads before this module. All of them when it is not there.
	 */
	float GetOverlaySegmentPercent()
	{
		static IConsoleVariable* Variable = nullptr;
		if (!Variable)
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.AIOverlay.SegmentPercent"));
		}

		return Variable ? FMath::Clamp(Variable->GetFloat(), 0.0f, 100.0f) : 100.0f;
	}

	/**
	 * Whether the AI overlay takes in the segment with this platform key at a share of Percent. Picked by the key alone,
	 * so a segment stays picked however the structure around it changes, and a higher share keeps every segment a lower
	 * one picked.
	 */
	bool IsSegmentPicked(const uint64 PlatformKey, const float Percent)
	{
		if (Percent >= 100.0f)
		{
			return true;
		}
		if (Percent <= 0.0f)
		{
			return false;
		}

		// splitmix64's finish, so segments side by side on the lattice scatter.
		uint64 Hash = PlatformKey;
		Hash = (Hash ^ (Hash >> 30)) * 0xbf58476d1ce4e5b9ull;
		Hash = (Hash ^ (Hash >> 27)) * 0x94d049bb133111ebull;
		Hash ^= Hash >> 31;

		return static_cast<double>(Hash >> 40) < Percent * 0.01 * static_cast<double>(uint64(1) << 24);
	}

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

	/** Faces as GetFacesOverInteriorGround reports them, one bit each. */
	constexpr uint8 PositiveFaceBit = 1 << 0;
	constexpr uint8 NegativeFaceBit = 1 << 1;

	FORCEINLINE uint8 FaceBit(const int32 FaceSign)
	{
		return FaceSign > 0 ? PositiveFaceBit : NegativeFaceBit;
	}

	/** The sign of a direction round a module edge, as an index in turning order: +A and +B, then -A and -B. */
	FORCEINLINE int32 TurnSign(const int32 Turn)
	{
		return Turn % 4 < 2 ? 1 : -1;
	}

	/** Whether a structure is an interior and nothing else, the only kind the rules bind by. */
	FORCEINLINE bool IsInterior(const UPlatformStructureData* Structure)
	{
		return Structure && Structure->Side == EStructureSide::Interior;
	}

	/** Keeps the options that stand for one side, those of both sides among them, where any does; otherwise leaves them all. */
	void KeepSide(TArray<UPlatformStructureData*>& Options, const EStructureSide Side)
	{
		TArray<UPlatformStructureData*> Kept = Options.FilterByPredicate([Side](const UPlatformStructureData* Option)
		{
			return Option->StandsFor(Side);
		});

		if (!Kept.IsEmpty())
		{
			Options = MoveTemp(Kept);
		}
	}

	/** The face looking the other way along the same axis. */
	FORCEINLINE EGridFace OppositeFace(const EGridFace Face)
	{
		return MakeFace(FaceAxis(Face), -FaceSign(Face));
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

	/** Calls Visit(Node) for every lattice node a platform takes in: those round its rim, and those across its inside. */
	template <typename FunctorType>
	void ForEachPlatformNode(
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
				Visit(Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV));
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

	for (int32 Zone = 0; Zone < ZoneCount; ++Zone)
	{
		ZoneColors.Add(GetDefaultZoneColor(Zone, ZonesPerSide * ZonesPerSide));
	}

	GridLineBatcher = CreateDefaultSubobject<ULineBatchComponent>(TEXT("GridLineBatcher"));
	GridLineBatcher->SetupAttachment(SceneRoot);

	AirLineBatcher = CreateDefaultSubobject<ULineBatchComponent>(TEXT("AirLineBatcher"));
	AirLineBatcher->SetupAttachment(SceneRoot);

	PathLineBatcher = CreateDefaultSubobject<ULineBatchComponent>(TEXT("PathLineBatcher"));
	PathLineBatcher->SetupAttachment(SceneRoot);

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
		// off it the structure reads as flat as the notation laid over it. And it is solid: the drone
		// flies among the pieces, and meets every one of them. A structure collides with its own
		// triangles (the importer sees to that), so whatever openings it has stay open.
		Pieces->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

		// Nothing walks the tower, so no navigation is built over it; without this, every chunk that
		// fills would have the navigation system look at it again.
		Pieces->SetCanEverAffectNavigation(false);
		Pieces->SetCastShadow(true);
		Pieces->SetMobility(EComponentMobility::Movable);
		Pieces->SetRenderCustomDepth(true);
		Pieces->SetCustomDepthStencilValue(AIOverlayStencil);
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

	// The pockets stay where they were drawn, cut afresh for the lattice as it now stands.
	RebuildPocketKeys();

	// Paths hold back nothing standing, so they are drawn again whatever changed - on the same routes, which
	// a change to their own settings then shows straight away.
	SpawnPaths();

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
		SpawnPaths();
		UpdateDebugReadouts();
		RebuildVisuals();
	}

	// The AI overlay's share of segments can be changed during play, from its settings page or the console; the pieces
	// already drawn are sorted again for it.
	if (IConsoleVariable* OverlayShare = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.AIOverlay.SegmentPercent")))
	{
		OverlayShareChangedHandle = OverlayShare->OnChangedDelegate().AddWeakLambda(this, [this](IConsoleVariable*)
		{
			SortPiecesForOverlay();
		});
	}

	// Paths, pockets, strikes and reshuffles all fall due by one clock, starting from now.
	const double Now = GetClock();
	NextStrikeAt = Now + DamageStream.FRandRange(MinStrikeInterval, FMath::Max(MaxStrikeInterval, MinStrikeInterval));
	NextReshuffleAt = Now + ReshuffleInterval;
	StartClock();

	// One placement per beat - the first lands a full interval in, so the empty cube is
	// visible before anything grows in it.
	if (bAutoPlacePlatforms && PlatformPlacementInterval > 0.0f)
	{
		FTimerManagerTimerParameters BeatParameters;
		BeatParameters.bLoop = true;
		BeatParameters.FirstDelay = PlatformPlacementInterval;

		// The timer fires at most once a frame. Beats owed beyond that run in the same frame only as far as
		// MaxBeatsPerFrame and the frame budget allow (see AdvancePlacement); the rest are dropped, not made up.
		// Catching up without a bound would stack placements into a frame that is already slow, which only makes
		// the next one slower.
		BeatParameters.bMaxOncePerFrame = true;
		BeatClock = Now;
		PlacementSecondStart = Now;
		PlacementsThisSecond = 0;

		GetWorldTimerManager().SetTimer(
			PlacementTimerHandle, this, &ARoomManager::AdvancePlacement,
			PlatformPlacementInterval, BeatParameters);
	}

	// Nature keeps a beat of its own, set up the same way.
	if (bAutoPlaceNature && NaturePlacementInterval > 0.0f)
	{
		FTimerManagerTimerParameters NatureParameters;
		NatureParameters.bLoop = true;
		NatureParameters.FirstDelay = NaturePlacementInterval;
		NatureParameters.bMaxOncePerFrame = true;

		GetWorldTimerManager().SetTimer(
			NatureTimerHandle, this, &ARoomManager::AdvanceNature,
			NaturePlacementInterval, NatureParameters);
	}
}

void ARoomManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PlacementTimerHandle);
		World->GetTimerManager().ClearTimer(NatureTimerHandle);
		World->GetTimerManager().ClearTimer(ClockTimerHandle);
	}

	if (IConsoleVariable* OverlayShare = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.AIOverlay.SegmentPercent")))
	{
		OverlayShare->OnChangedDelegate().Remove(OverlayShareChangedHandle);
	}
	OverlayShareChangedHandle.Reset();

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

	// Air held back - a path, a pocket, a mold, the air over a field - holds back only what is placed from now
	// on, so the rebuild, which asks IsSpaceFree, never leaves out a platform it has since reached.
	if (!IsSpaceFree(Platform) || !IsClearOfHeldAir(Platform))
	{
		return false;
	}

	int32 SpanU, SpanV;
	GetPlatformSpans(Platform, SpanU, SpanV);

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	bool bTouchesStructure = false;
	bool bTouchesNature = false;
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

			bTouchesNature = bTouchesNature || IsNatureSegment(BuiltPlatforms[*Owner]);

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
	if (!bFlush || !bFoldsLegally || !bTouchesStructure)
	{
		return false;
	}

	// Nature takes only the faces its form allows. A filled placement is held to that as it stands. One not
	// filled yet - the city's, still to be drawn for - is refused only where it would meet water or a cliff,
	// which take no face of the city; its fill sees to it that what meets ground is an exterior. Faces only
	// meet round the edges a placement shares, so one touching no nature, and none itself, meets none.
	if (bTouchesNature || IsNatureSegment(Platform))
	{
		return Platform.PositiveStructure || Platform.NegativeStructure ? AreNatureMeetingsLegal(Platform) : !MeetsClosedNature(Platform);
	}

	return true;
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

	// It grows off one of the platforms it touches, and takes that one's Up, as the turns leave it.
	const FGridPlatform& Parent = BuiltPlatforms[Touching[Touching.Num() > 1 ? RoleStream.RandRange(0, Touching.Num() - 1) : 0]];
	const EGridFace Up = TurnUp(Platform, Parent);

	if (CanFill(Up))
	{
		return Up;
	}

	// A way up the structures cannot fill is no use. Unturned it may be one they can; failing that,
	// another platform it touches may offer one, and the world's always can: the placement was only
	// drawn because it can be filled that way.
	if (Up != Parent.Up && CanFill(Parent.Up))
	{
		return Parent.Up;
	}

	for (const int32 Other : Touching)
	{
		if (CanFill(BuiltPlatforms[Other].Up))
		{
			return BuiltPlatforms[Other].Up;
		}
	}

	return EGridFace::PosZ;
}

EGridFace ARoomManager::TurnUp(const FGridPlatform& Platform, const FGridPlatform& Parent)
{
	EGridFace Up = Parent.Up;
	const int32 ParentNormal = AxisIndex(Parent.Normal);

	auto HoldsItems = [this, &Parent](const int32 Sign)
	{
		return FaceHoldsItems(Parent, Sign);
	};

	if (Platform.Normal != Parent.Normal)
	{
		// Folding off it, the placement reaches from their shared edge to one side of the parent's plane.
		const int32 Side = Platform.MinNode[ParentNormal] == Parent.MinNode[ParentNormal] ? 1 : -1;

		// Off a floor's Ceiling face - the side its Up does not point to - a wall hangs, its Up pointing
		// away from the floor.
		if (bHangFromCeilings && !Parent.IsWall() && Side != FaceSign(Parent.Up) && HoldsItems(Side))
		{
			Up = OppositeFace(Up);
		}

		if (bFlipOnFold && RoleStream.FRand() < FlipChance)
		{
			Up = OppositeFace(Up);
		}
	}
	else if (bQuarterTurn && Parent.IsWall() && RoleStream.FRand() < QuarterTurnChance)
	{
		// Carried on in a wall's plane, it becomes a floor whose Ground looks out of a face of the wall
		// that holds items.
		const bool bPositiveHolds = HoldsItems(1);
		const bool bNegativeHolds = HoldsItems(-1);

		if (bPositiveHolds || bNegativeHolds)
		{
			const int32 Sign = bPositiveHolds && bNegativeHolds ? (RoleStream.FRand() < 0.5f ? 1 : -1) : (bPositiveHolds ? 1 : -1);
			Up = MakeFace(ParentNormal, Sign);
		}
	}

	return Up;
}

bool ARoomManager::FaceHoldsItems(const FGridPlatform& Platform, const int32 Sign) const
{
	return !IsFaceBlocked(Platform, MakeFace(AxisIndex(Platform.Normal), Sign));
}

bool ARoomManager::HasFloorAlong(const FGridPlatform& Wall, const int32 Coordinate, const int32 Side) const
{
	int32 SpanU, SpanV, U, V;
	if (!GetPlatformSpans(Wall, SpanU, SpanV))
	{
		return false;
	}

	const int32 Module = GetModule();
	const int32 WallNormal = AxisIndex(Wall.Normal);
	const int32 UpAxis = FaceAxis(Wall.Up);
	GetInPlaneAxes(WallNormal, U, V);

	const int32 AcrossAxis = UpAxis == U ? V : U;
	bool bFound = false;

	// The module faces lying across Up beside the wall's edge there, on that side of its plane.
	ForEachModuleEdge(Wall, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		if (bFound || !bRim || Edge.Axis != AcrossAxis || Edge.Node[UpAxis] != Coordinate)
		{
			return;
		}

		const FIntVector FaceMinNode = Side > 0 ? Edge.Node : Edge.Node - AxisStep(WallNormal, Module);

		if (const int32* Owner = ModuleFaceOwners.Find(MakeModuleFaceKey(FaceMinNode, UpAxis)))
		{
			bFound = !BuiltPlatforms[*Owner].IsWall();
		}
	});

	return bFound;
}

float ARoomManager::GetRuleRedrawChance(const FGridPlatform& Candidate) const
{
	if (!bWallsStandOnGrounds && !bFloorsCloseRooms)
	{
		return 0.0f;
	}

	TArray<int32, TInlineAllocator<8>> Touching;
	GatherTouchingPlatforms(Candidate, Touching);

	float Chance = 0.0f;

	for (const int32 Index : Touching)
	{
		const FGridPlatform& Parent = BuiltPlatforms[Index];

		// Both rules are about folding off a platform, into one side of its plane or the other.
		if (Candidate.Normal == Parent.Normal)
		{
			continue;
		}

		const int32 ParentNormal = AxisIndex(Parent.Normal);
		const int32 Side = Candidate.MinNode[ParentNormal] == Parent.MinNode[ParentNormal] ? 1 : -1;

		if (!Parent.IsWall())
		{
			// Off a ground that holds items, walls stand up on it rather than hang below it.
			const int32 GroundSide = FaceSign(Parent.Up);

			if (bWallsStandOnGrounds && Side != GroundSide && FaceHoldsItems(Parent, GroundSide))
			{
				Chance = FMath::Max(Chance, WallsStandChance);
			}

			continue;
		}

		// Off a wall's holding face, a floor at its top or its foot - the ends across its Up - goes where the
		// room it faces still needs one: at the foot when its top is closed by a ceiling, at the top when its
		// foot already has a ground, and at the foot when it has neither.
		const int32 UpAxis = FaceAxis(Parent.Up);

		if (!bFloorsCloseRooms || AxisIndex(Candidate.Normal) != UpAxis || !FaceHoldsItems(Parent, Side))
		{
			continue;
		}

		int32 SpanU, SpanV, U, V;
		GetPlatformSpans(Parent, SpanU, SpanV);
		GetInPlaneAxes(ParentNormal, U, V);

		const int32 UpSpan = UpAxis == U ? SpanU : SpanV;
		const bool bUpPositive = FaceSign(Parent.Up) > 0;
		const int32 TopCoordinate = Parent.MinNode[UpAxis] + (bUpPositive ? UpSpan : 0);
		const int32 FootCoordinate = Parent.MinNode[UpAxis] + (bUpPositive ? 0 : UpSpan);

		const bool bCeilingAtTop = HasFloorAlong(Parent, TopCoordinate, Side);
		const bool bGroundAtFoot = HasFloorAlong(Parent, FootCoordinate, Side);

		// A room closed at both ends wants neither.
		if (bCeilingAtTop && bGroundAtFoot)
		{
			continue;
		}

		const bool bTopWanted = bGroundAtFoot;
		const bool bAtTop = Candidate.MinNode[UpAxis] == TopCoordinate;

		if (bAtTop != bTopWanted)
		{
			Chance = FMath::Max(Chance, FloorsCloseRoomsChance);
		}
	}

	return Chance;
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

void ARoomManager::UnlistPlatformCandidate(const int32 TileIndex, const FGridPlatform& Candidate)
{
	TArray<FGridPlatform>& Candidates = TileCandidates[TileIndex].ByKind[KindIndex(Candidate.GetKind())];

	const uint64 Key = MakeCandidateKey(Candidate);
	const int32 Index = Algo::LowerBoundBy(Candidates, Key, [](const FGridPlatform& Listed)
	{
		return MakeCandidateKey(Listed);
	});

	if (Candidates.IsValidIndex(Index) && MakeCandidateKey(Candidates[Index]) == Key)
	{
		Candidates.RemoveAt(Index);
	}
}

bool ARoomManager::MeetsStructure(const FGridPlatform& Placement) const
{
	int32 SpanU, SpanV;

	if (!GetPlatformSpans(Placement, SpanU, SpanV))
	{
		return false;
	}

	const int32 Module = GetModule();

	int32 U, V;
	GetInPlaneAxes(AxisIndex(Placement.Normal), U, V);

	// Round its rim alone, and only until something is met there: its two sides running along U, then its two along V.
	for (const int32 StepV : { 0, SpanV })
	{
		for (int32 StepU = 0; StepU < SpanU; StepU += Module)
		{
			if (ModuleEdgeUses.Contains(MakeEdgeKey(FLatticeEdge{ Placement.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV), U })))
			{
				return true;
			}
		}
	}

	for (const int32 StepU : { 0, SpanU })
	{
		for (int32 StepV = 0; StepV < SpanV; StepV += Module)
		{
			if (ModuleEdgeUses.Contains(MakeEdgeKey(FLatticeEdge{ Placement.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV), V })))
			{
				return true;
			}
		}
	}

	return false;
}

bool ARoomManager::PlacePlatformOfKind(const EPlatformKind Kind, const TFunction<bool(const FGridPlatform&)>* Filter)
{
	FGridPlatform Platform;

	// A platform landing where a fence run waits to carry on, laid as the run is, carries it on.
	// Anything else is filled as drawn, with the platform and before it is taken in, since what fills a
	// face decides what that face carries.
	int32 JoinedSide = 0;

	if (BuiltPlatforms.IsEmpty())
	{
		// The empty cube is the one case with nothing to grow from, and only FirstTile, laid
		// horizontal, starts it - whatever its weight. Nothing kept to a part of the cube starts it.
		if (Kind != EPlatformKind::SurfaceHorizontal || Filter)
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

		// The platform that starts growth holds up all the rest.
		if (!TakeWaitingFenceSpot(Platform, JoinedSide) && !DrawFill(Platform, /*bLoadBearingOnly=*/true))
		{
			return false;
		}
	}
	else
	{
		EnsurePlatformCandidates();

		// A placement is only drawn laid a way round that the structures can fill, and only where the filter - a
		// reshuffle keeping to its zone - lets it be. Where that is every way the tile is listed, every placement counts.
		auto IsFillable = [this, Kind, Filter](const int32 TileIndex, const FGridPlatform& Candidate)
		{
			return (GetFillableWays(TileIndex, Kind) & (Candidate.bTurned ? TurnedWay : AsLaidWay)) != 0
				&& (!Filter || (*Filter)(Candidate));
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

		// A placement no fill can be drawn for - one whose face meets nature ground where no exterior fits
		// it - is drawn again from the top, a few times at most. Anywhere else the first draw is filled.
		for (int32 FillDraw = 0; ; ++FillDraw)
		{
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
			// list it was drawn from. Where every way the tile is listed can be filled - nearly always - and nothing
			// filters them, every placement is fillable, and the draw is the same without counting them.
			const uint8 ListedWays = Tile->bCanTurn && Tile->Width != Tile->Length ? (AsLaidWay | TurnedWay) : AsLaidWay;
			const bool bAllFillable = !Filter && (GetFillableWays(ChosenTile, Kind) & ListedWays) == ListedWays;
			const int32 Fillable = bAllFillable ? Candidates.Num() : Algo::CountIf(Candidates, [ChosenTile, &IsFillable](const FGridPlatform& Candidate)
			{
				return IsFillable(ChosenTile, Candidate);
			});

			// Inside a mold's box a placement weighs MoldGrowthWeight, so the city fills a mold sooner than it would
			// by chance. Weighed only while a mold stands, so without one the draw is as it always was.
			TArray<TPair<FVector, FVector>, TInlineAllocator<4>> MoldBounds;

			if (MoldGrowthWeight > 1.0f)
			{
				for (const FAirPocket& Pocket : AirPockets)
				{
					if (Pocket.bMold)
					{
						TPair<FVector, FVector>& Bounds = MoldBounds.AddDefaulted_GetRef();
						GetPocketBounds(Pocket, Bounds.Key, Bounds.Value);
					}
				}
			}

			const bool bMoldPull = !MoldBounds.IsEmpty();
			TArray<double> PullWeights;

			if (bMoldPull)
			{
				PullWeights.Reserve(Candidates.Num());
				double Running = 0.0;

				for (const FGridPlatform& Candidate : Candidates)
				{
					// As IsInMoldBox asks it, with the molds' bounds worked out once rather than for every placement.
					const FAirBox Box = GetPlatformBox(Candidate);
					const FVector Middle = 0.5 * FVector(Box.Min + Box.Max) + FVector(0.5);
					const bool bInMold = MoldBounds.ContainsByPredicate([&Middle](const TPair<FVector, FVector>& Bounds)
					{
						return Middle.X >= Bounds.Key.X && Middle.X <= Bounds.Value.X
							&& Middle.Y >= Bounds.Key.Y && Middle.Y <= Bounds.Value.Y
							&& Middle.Z >= Bounds.Key.Z && Middle.Z <= Bounds.Value.Z;
					});

					Running += !IsFillable(ChosenTile, Candidate) ? 0.0 : (bInMold ? MoldGrowthWeight : 1.0);
					PullWeights.Add(Running);
				}
			}

			auto DrawPlacement = [&]() -> FGridPlatform
			{
				if (bMoldPull)
				{
					// The first placement whose running weight passes the roll.
					const double Roll = PlacementStream.FRand() * PullWeights.Last();
					const int32 Pick = FMath::Min(Algo::UpperBound(PullWeights, Roll), Candidates.Num() - 1);
					return Candidates[Pick];
				}

				if (bAllFillable)
				{
					return Candidates[PlacementStream.RandRange(0, Candidates.Num() - 1)];
				}

				int32 Pick = PlacementStream.RandRange(0, Fillable - 1);

				for (const FGridPlatform& Candidate : Candidates)
				{
					if (IsFillable(ChosenTile, Candidate) && Pick-- == 0)
					{
						return Candidate;
					}
				}

				return Candidates[0];
			};

			Platform = DrawPlacement();

			// A placement going against a growth rule is drawn again, with the rule's chance, so growth leans
			// the way the rules say without ever being held to it.
			for (int32 Redraw = 0; Redraw < MaxRuleRedraws; ++Redraw)
			{
				const float Chance = GetRuleRedrawChance(Platform);

				if (Chance <= 0.0f || PlacementStream.FRand() >= Chance)
				{
					break;
				}

				Platform = DrawPlacement();
			}

			// Which way up it is follows from what it grows off, and decides what can fill it.
			Platform.Up = ChooseUp(Platform);

			if (TakeWaitingFenceSpot(Platform, JoinedSide) || DrawFill(Platform, /*bLoadBearingOnly=*/false))
			{
				break;
			}

			if (FillDraw + 1 >= MaxFillRedraws)
			{
				return false;
			}
		}
	}

	PlacePlatform(Platform, JoinedSide);

	// A ground it is laid with may spill out from here, and a wall standing on the outline of grounds run on round it. A
	// reshuffle keeps to its zone, so it starts neither.
	if (!Filter)
	{
		QueueGroundSpill(Platform);
		QueueWallRun(Platform);
	}

	return true;
}

void ARoomManager::PlacePlatform(const FGridPlatform& Platform, const int32 JoinedSide)
{
	// The first platform in the empty cube is the one every zone's first tile keeps in step with.
	if (BuiltPlatforms.IsEmpty())
	{
		SeedPhase = Platform.MinNode;
	}

	Platforms.Add(Platform);
	GiveHealth(Platform);

	TArray<FGridSurfaceRef> ChangedSurfaces;
	AddPlacedPlatform(Platform, ChangedSurfaces);

	// The first laid in a zone with no ground tile is its ground tile: the middle zone's very first, a seed, or the first
	// to reach a zone whose ground tiles have all gone.
	if (GroundTileCounts[GetZoneOf(Platform)] == 0 && MakeGroundTile(Platform) && bShowZones)
	{
		RebuildDebugGrid();
	}

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
		QueueRunEnds(Platform, JoinedSide, FenceContinueChance, StructureStream, FenceQueue);
	}
}

bool ARoomManager::IsFencePanel(const FGridPlatform& Platform)
{
	const UPlatformStructureData* Fence = Platform.PositiveStructure;

	return Fence && Fence == Platform.NegativeStructure && Fence->bFence && Fence->bTwoSided;
}

UPlatformStructureData* ARoomManager::GetRunStructure(const FGridPlatform& Platform)
{
	if (IsFencePanel(Platform))
	{
		return Platform.PositiveStructure;
	}

	int32 Sign = 0;
	ENatureForm Form = ENatureForm::Ground;
	return GetNatureFace(Platform, Sign, Form) && Form == ENatureForm::Cliff ? GetFaceStructure(Platform, Sign) : nullptr;
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

bool ARoomManager::IsRunEndClosed(const FFenceEnd& End, const FGridPlatform& Straight, const TConstArrayView<FGridPlatform> Turns) const
{
	// A fence or a cliff meets more of itself; a run of walls, which varies as it goes, meets a wall on the outline it follows.
	const UPlatformStructureData* Run = GetRunStructure(End.Panel);

	auto IsStanding = [this, Run, &End](const FGridPlatform& Spot)
	{
		const int32* Owner = ModuleFaceOwners.Find(MakePlatformKey(Spot));

		if (!Owner)
		{
			return false;
		}

		const FGridPlatform& Standing = BuiltPlatforms[*Owner];

		if (Standing.MinNode != Spot.MinNode || Standing.Normal != Spot.Normal || Standing.Tile != Spot.Tile || Standing.bTurned != Spot.bTurned)
		{
			return false;
		}

		return Run ? GetRunStructure(Standing) == Run
			: StandsOnGroundOutline(Standing, 1, End.RunSide) || StandsOnGroundOutline(Standing, -1, End.RunSide);
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

void ARoomManager::QueueRunEnds(
	const FGridPlatform& Panel,
	const int32 JoinedSide,
	const float Chance,
	FRandomStream& Stream,
	TArray<FFenceEnd>& Queue,
	const EStructureSide RunSide)
{
	for (const int32 Side : { -1, 1 })
	{
		if (Side == JoinedSide)
		{
			continue;
		}

		const FFenceEnd End{ Panel, Side, RunSide };

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

		bHasRoom = bHasRoom && !IsRunEndClosed(End, Straight, Turns);

		if (bHasRoom && Stream.FRand() < Chance)
		{
			Queue.Add(End);
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
		if (IsRunEndClosed(End, Straight, Turns))
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

void ARoomManager::QueueGroundSpill(const FGridPlatform& Platform)
{
	// Only a floor of the city, in its own frame, whose face looking along its Up is walked on.
	if (Platform.IsWall() || IsFencePanel(Platform) || IsNatureSegment(Platform))
	{
		return;
	}

	const int32 UpSign = FaceSign(Platform.Up);
	const UPlatformStructureData* Top = GetFaceStructure(Platform, UpSign);

	// One of both sides binds nothing, so it starts a patch of neither.
	if (!Top || Top->Side == EStructureSide::Both
		|| !EnumHasAnyFlags(WEMStructures::GetTypesOnFace(*Top, AxisIndex(Platform.Normal), UpSign, Platform.Up), EStructureType::Ground))
	{
		return;
	}

	const bool bInterior = Top->Side == EStructureSide::Interior;
	const float Chance = bInterior ? InteriorGroundSpillChance : ExteriorGroundSpillChance;

	// Nothing is drawn at a chance of zero, so with spills off growth draws just as it did before there were any.
	if (Chance <= 0.0f || StructureStream.FRand() >= Chance)
	{
		return;
	}

	const int32 Fewest = FMath::Max(bInterior ? InteriorGroundSpillMin : ExteriorGroundSpillMin, 1);
	const int32 Most = FMath::Max(bInterior ? InteriorGroundSpillMax : ExteriorGroundSpillMax, Fewest);
	const int32 Size = StructureStream.RandRange(Fewest, Most);

	if (Size > 1)
	{
		FGroundSpill& Spill = GroundSpillQueue.AddDefaulted_GetRef();
		Spill.Side = Top->Side;
		Spill.Pieces.Add(Platform);
		Spill.Remaining = Size - 1;
	}
}

bool ARoomManager::PlaceGroundSpillPiece()
{
	const int32 Module = GetModule();

	while (!GroundSpillQueue.IsEmpty())
	{
		FGroundSpill& Spill = GroundSpillQueue[0];
		const EGridAxis Normal = Spill.Pieces[0].Normal;
		const EGridFace Up = Spill.Pieces[0].Up;

		// Every free placement beside the patch in its plane, each once, weighing as many module edges as it shares with
		// the patch's pieces.
		TMap<TPair<uint64, int32>, int32> Seen;
		TArray<FGridPlatform> Candidates;
		TArray<int32> Weights;

		for (const FGridPlatform& Piece : Spill.Pieces)
		{
			int32 SpanU, SpanV;
			GetPlatformSpans(Piece, SpanU, SpanV);

			ForEachModuleEdge(Piece, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
			{
				if (!bRim)
				{
					return;
				}

				ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/true, [&](const int32 TileIndex, const FGridPlatform& Placement, bool)
				{
					if (Placement.Normal != Normal)
					{
						return;
					}

					const TPair<uint64, int32> Key(MakeCandidateKey(Placement), TileIndex);

					if (const int32* Found = Seen.Find(Key))
					{
						if (*Found != INDEX_NONE)
						{
							++Weights[*Found];
						}

						return;
					}

					const bool bFree = IsPlatformOnGrid(Placement) && IsSpaceFree(Placement);
					Seen.Add(Key, bFree ? Candidates.Num() : INDEX_NONE);

					if (bFree)
					{
						Candidates.Add_GetRef(Placement).Up = Up;
						Weights.Add(1);
					}
				});
			});
		}

		// Likelier where it would meet more of the patch, so it spills out as a patch rather than a line. Each is held to
		// what any floor is held to there, and then topped with a ground of the patch's side.
		while (!Candidates.IsEmpty())
		{
			int32 TotalWeight = 0;
			for (const int32 Weight : Weights)
			{
				TotalWeight += Weight;
			}

			int32 Roll = PlacementStream.RandRange(0, TotalWeight - 1);
			int32 Chosen = 0;

			while (Roll >= Weights[Chosen])
			{
				Roll -= Weights[Chosen];
				++Chosen;
			}

			FGridPlatform Platform = Candidates[Chosen];
			const FFaceWish Wish{ FaceSign(Up), EStructureType::Ground, Spill.Side };

			if (CanPlacePlatform(Platform) && DrawFill(Platform, /*bLoadBearingOnly=*/false, &Wish) && CanPlacePlatform(Platform))
			{
				Spill.Pieces.Add(Platform);

				if (--Spill.Remaining <= 0)
				{
					GroundSpillQueue.RemoveAt(0);
				}

				// A ground laid beside walls standing may start them running round it.
				PlacePlatform(Platform, /*JoinedSide=*/0);
				QueueWallRun(Platform);
				return true;
			}

			Candidates.RemoveAt(Chosen);
			Weights.RemoveAt(Chosen);
		}

		GroundSpillQueue.RemoveAt(0);
	}

	return false;
}

void ARoomManager::QueueWallRun(const FGridPlatform& Platform)
{
	// Nothing is looked for or drawn while both chances are zero, so with runs off growth goes just as it did before.
	if (IsFencePanel(Platform) || IsNatureSegment(Platform) || (InteriorWallRunChance <= 0.0f && ExteriorWallRunChance <= 0.0f))
	{
		return;
	}

	if (Platform.IsWall())
	{
		TryStartWallRun(Platform);
		return;
	}

	// A floor laid with a ground of the city gives the walls already standing round its rim a new outline to stand on.
	const int32 NormalAxis = AxisIndex(Platform.Normal);

	bool bHasGround = false;

	for (const int32 Sign : { 1, -1 })
	{
		const UPlatformStructureData* Face = GetFaceStructure(Platform, Sign);
		bHasGround = bHasGround || (Face && EnumHasAnyFlags(WEMStructures::GetTypesOnFace(*Face, NormalAxis, Sign, Platform.Up), EStructureType::Ground));
	}

	int32 SpanU, SpanV;
	if (!bHasGround || !GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return;
	}

	const int32 Module = GetModule();
	TArray<int32, TInlineAllocator<8>> Walls;

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
			const int32* Owner = Face.Normal == NormalAxis ? nullptr : ModuleFaceOwners.Find(MakeModuleFaceKey(Face.MinNode, Face.Normal));

			if (Owner && BuiltPlatforms[*Owner].IsWall())
			{
				Walls.AddUnique(*Owner);
			}
		}
	});

	// Each of them standing on it wholly may start a run, as if it had just been laid - unless a run of it waits already.
	for (const int32 Index : Walls)
	{
		const FGridPlatform Wall = BuiltPlatforms[Index];
		const uint64 Key = MakePlatformKey(Wall);

		if (!IsFencePanel(Wall) && !IsNatureSegment(Wall)
			&& !WallRunQueue.ContainsByPredicate([Key](const FFenceEnd& End) { return MakePlatformKey(End.Panel) == Key; }))
		{
			TryStartWallRun(Wall);
		}
	}
}

void ARoomManager::TryStartWallRun(const FGridPlatform& Wall)
{
	// The face over the grounds sets the run's side. One of both sides binds nothing, so it starts no run.
	for (const int32 Sign : { 1, -1 })
	{
		const UPlatformStructureData* Face = GetFaceStructure(Wall, Sign);

		if (!Face || Face->Side == EStructureSide::Both)
		{
			continue;
		}

		// Nothing is drawn at a chance of zero, so with runs off growth draws just as it did before there were any.
		const float Chance = Face->Side == EStructureSide::Interior ? InteriorWallRunChance : ExteriorWallRunChance;

		if (Chance <= 0.0f || !StandsOnGroundOutline(Wall, Sign, Face->Side))
		{
			continue;
		}

		if (StructureStream.FRand() < Chance)
		{
			QueueRunEnds(Wall, /*JoinedSide=*/0, WallRunContinueChance, StructureStream, WallRunQueue, Face->Side);
		}

		return;
	}
}

bool ARoomManager::PlaceWallRunPanel()
{
	while (!WallRunQueue.IsEmpty())
	{
		const FFenceEnd End = WallRunQueue[0];
		WallRunQueue.RemoveAt(0, EAllowShrinking::No);

		FGridPlatform Straight;
		TArray<FGridPlatform, TInlineAllocator<2>> Turns;
		GatherFenceContinuations(End, Straight, Turns);

		// A run that has met itself here since the end was queued - closing round a room - stops.
		if (IsRunEndClosed(End, Straight, Turns))
		{
			continue;
		}

		// Each spot as a wall, still to be filled, standing on the outline of the run's grounds with either face over them.
		// Straight on, that is the face the run's last wall looked over them with; round a corner, the one looking into it.
		using FSpot = TPair<FGridPlatform, int32>;
		TArray<FSpot, TInlineAllocator<2>> StraightOn;
		TArray<FSpot, TInlineAllocator<4>> RoundCorners;

		auto Judge = [&](const FGridPlatform& Spot, auto& Out)
		{
			FGridPlatform Platform = Spot;
			Platform.PositiveStructure = nullptr;
			Platform.NegativeStructure = nullptr;
			Platform.NatureCover = nullptr;

			if (!CanPlacePlatform(Platform))
			{
				return;
			}

			for (const int32 Sign : { 1, -1 })
			{
				if (StandsOnGroundOutline(Platform, Sign, End.RunSide))
				{
					Out.Emplace(Platform, Sign);
					return;
				}
			}
		};

		Judge(Straight, StraightOn);

		for (const FGridPlatform& Turn : Turns)
		{
			Judge(Turn, RoundCorners);
		}

		// Straight on when nothing else is left, round a corner when only that is, and where both are, round a corner with
		// WallRunTurnChance, split evenly between the ways left. One that cannot be filled after all gives way to the rest.
		while (!StraightOn.IsEmpty() || !RoundCorners.IsEmpty())
		{
			const bool bTurn = StraightOn.IsEmpty() || (!RoundCorners.IsEmpty() && StructureStream.FRand() < WallRunTurnChance);
			FSpot Spot;

			if (bTurn)
			{
				const int32 Chosen = RoundCorners.Num() > 1 ? StructureStream.RandRange(0, RoundCorners.Num() - 1) : 0;
				Spot = RoundCorners[Chosen];
				RoundCorners.RemoveAt(Chosen);
			}
			else
			{
				Spot = StraightOn[0];
				StraightOn.RemoveAt(0);
			}

			// Its face over the grounds is of the run's side, drawn as usual; its back follows the usual rules.
			FGridPlatform& Platform = Spot.Key;
			const FFaceWish Wish{ Spot.Value, EStructureType::Wall, End.RunSide };

			if (DrawFill(Platform, /*bLoadBearingOnly=*/false, &Wish) && CanPlacePlatform(Platform))
			{
				PlacePlatform(Platform, /*JoinedSide=*/0);
				QueueRunEnds(Platform, GetJoinedSide(End, Platform), WallRunContinueChance, StructureStream, WallRunQueue, End.RunSide);
				return true;
			}
		}
	}

	return false;
}

bool ARoomManager::PlaceNextPlatform()
{
	// Once the middle zone's first tile stands, every other zone's is laid before anything else, a zone a beat. A zone
	// with no free place for one goes without.
	while (!BuiltPlatforms.IsEmpty() && !SeedQueue.IsEmpty())
	{
		const int32 Zone = SeedQueue[0];
		SeedQueue.RemoveAt(0);

		if (PlaceSeed(Zone))
		{
			return true;
		}
	}

	// A fence run waiting to grow takes the beat before anything else, a panel a beat, then a patch of ground spilling out,
	// a piece a beat, then a run of walls round the outline of grounds, a wall a beat.
	if (PlaceFencePanel() || PlaceGroundSpillPiece() || PlaceWallRunPanel())
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
	GroundSpillQueue.Reset();
	WallRunQueue.Reset();
	SpillQueue.Reset();
	CliffQueue.Reset();

	// Nothing has health or is waiting to go, no reshuffle is under way, and every zone waits for a first tile again.
	PlatformHealth.Reset();
	DyingQueue.Reset();
	DyingKeys.Reset();
	ReshuffleZoneIndex = INDEX_NONE;
	ReshuffleKeys.Reset();
	ReshuffleEmptied.Reset();
	StrikeCount = 0;
	HitCount = 0;
	VulnerableHitCount = 0;
	DamageRemovals = 0;
	ReshuffleCount = 0;
	ReshuffleRemovals = 0;
	ReshufflePlacements = 0;
	PocketMoves = 0;
	SeedPhase.Reset();
	QueueSeeds();

	// Nor has any zone a ground tile.
	GroundTiles.Reset();
	FMemory::Memzero(GroundTileCounts);
	GroundTilesPassedOn = 0;
	CutLooseRemovals = 0;

	ResetPlacementStream();
	BeginNewGeneration();

	// An empty cube is drawn new pockets of air, before anything grows round them.
	SpawnAirPockets();

	RebuildPlatformState();

	// And its paths, from the first of their draws.
	AirPaths.Reset();
	PathRedraws = 0;
	SpawnPaths();

	UpdateZoneSummary();
	UpdateDeletionSummary();
	UpdateDebugReadouts();
	RebuildVisuals();
}

void ARoomManager::ResetPlacementStream()
{
	// A zero seed means "different every run"; any other value replays the same growth.
	const int32 Seed = PlatformRandomSeed != 0 ? PlatformRandomSeed : FMath::Rand();
	PlacementStream.Initialize(Seed);
	RunSeed = Seed;

	// The fills are drawn from the same seed but a stream apart, so a seed lays the same platforms
	// whatever the structures draw in between.
	StructureStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Structures"))))));
	RoleStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Roles"))))));
	NatureStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Nature"))))));
	DamageStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Damage"))))));
	SeedStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Seeds"))))));
	ReshuffleStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("Reshuffle"))))));
	GroundTileStream.Initialize(static_cast<int32>(HashCombineFast(GetTypeHash(Seed), GetTypeHash(FStringView(TEXT("GroundTiles"))))));
}

void ARoomManager::AdvancePlacement()
{
	// The timer fires at most once a frame. With an interval shorter than a frame more beats than that are owed, and up to
	// MaxBeatsPerFrame of them run now, within the frame budget, each placing one platform as a beat always has.
	const double Now = GetClock();
	const double Interval = FMath::Max(static_cast<double>(PlatformPlacementInterval), UE_DOUBLE_KINDA_SMALL_NUMBER);
	const int32 Most = FMath::Clamp(MaxBeatsPerFrame, 1, 64);
	const int32 Owed = FMath::Clamp(FMath::FloorToInt32((Now - BeatClock) / Interval), 1, Most);

	// Beats owed beyond what a frame may run are dropped, not made up.
	BeatClock = Owed >= Most ? Now : BeatClock + Owed * Interval;

	const double Start = FPlatformTime::Seconds();
	const double Budget = FMath::Max(BeatBudgetMilliseconds, 0.0f) / 1000.0;

	for (int32 Beat = 0; Beat < Owed; ++Beat)
	{
		if (Beat > 0 && FPlatformTime::Seconds() - Start >= Budget)
		{
			break;
		}

		// A beat with nowhere to grow is simply lost, and so is the rest of the frame's. The beat keeps going, since
		// platforms taken away give room back.
		if (!PlaceNextPlatform())
		{
			break;
		}

		++PlacementsThisSecond;
	}

	if (Now - PlacementSecondStart >= 1.0)
	{
		PlacementsPerSecond = FMath::RoundToInt32(PlacementsThisSecond / (Now - PlacementSecondStart));
		PlacementsThisSecond = 0;
		PlacementSecondStart = Now;
	}
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

bool ARoomManager::IsSpaceFree(const FGridPlatform& Platform, const bool bIgnoreBlockedEdges) const
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
	if (bFree && !bIgnoreBlockedEdges && !BlockedEdges.IsEmpty())
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

	return bFree && IsClearOfPieces(Platform, SpanU, SpanV, ObstacleFaces, ObstacleEdges, ObstacleNodes);
}

template <typename KeySetType>
bool ARoomManager::IsClearOfPieces(
	const FGridPlatform& Platform,
	const int32 SpanU,
	const int32 SpanV,
	const KeySetType& Faces,
	const KeySetType& Edges,
	const KeySetType& Nodes) const
{
	if (Faces.IsEmpty() && Edges.IsEmpty() && Nodes.IsEmpty())
	{
		return true;
	}

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	bool bFree = true;

	// Every cell a platform covers is the inside of one of its module faces, the run of one of its
	// module edges, or one of its nodes, so asking those pieces asks every cell - without comparing
	// boxes, however many obstacles stand.
	ForEachModuleFace(Platform, SpanU, SpanV, Module, [&](const FIntVector& FaceMinNode)
	{
		bFree = bFree && !Faces.Contains(MakeModuleFaceKey(FaceMinNode, NormalAxis));
	});

	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, bool)
	{
		bFree = bFree && !Edges.Contains(MakeEdgeKey(Edge));
	});

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	for (int32 StepV = 0; bFree && StepV <= SpanV; StepV += Module)
	{
		for (int32 StepU = 0; bFree && StepU <= SpanU; StepU += Module)
		{
			bFree = !Nodes.Contains(MakeCellKey(Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV)));
		}
	}

	return bFree;
}

bool ARoomManager::IsClearOfPaths(const FGridPlatform& Platform) const
{
	int32 SpanU, SpanV;
	return GetPlatformSpans(Platform, SpanU, SpanV)
		&& IsClearOfPieces(Platform, SpanU, SpanV, PathFaces, PathEdges, PathNodes);
}

bool ARoomManager::IsClearOfPockets(const FGridPlatform& Platform) const
{
	int32 SpanU, SpanV;
	return GetPlatformSpans(Platform, SpanU, SpanV)
		&& IsClearOfPieces(Platform, SpanU, SpanV, PocketFaces, PocketEdges, PocketNodes);
}

bool ARoomManager::IsClearOfHeldAir(const FGridPlatform& Platform) const
{
	return IsClearOfPaths(Platform) && IsClearOfPockets(Platform) && IsClearOfAirBoxes(Platform);
}

ARoomManager::FAirBox ARoomManager::GetPlatformBox(const FGridPlatform& Platform) const
{
	int32 SpanU, SpanV, U, V;
	GetPlatformSpans(Platform, SpanU, SpanV);
	GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

	FAirBox Box{ Platform.MinNode, Platform.MinNode };
	Box.Max[U] += SpanU;
	Box.Max[V] += SpanV;
	return Box;
}

bool ARoomManager::IsClearOfAirBoxes(const FGridPlatform& Platform) const
{
	if (AirBoxBuckets.IsEmpty())
	{
		return true;
	}

	// Every cell a platform covers lies in one of its pieces, and a box cuts a piece exactly where it holds one of the
	// piece's cells - so a box holds a platform back exactly where it holds one of its cells.
	const FAirBox Box = GetPlatformBox(Platform);

	for (int32 Z = Box.Min.Z >> AirBoxBucketShift; Z <= Box.Max.Z >> AirBoxBucketShift; ++Z)
	{
		for (int32 Y = Box.Min.Y >> AirBoxBucketShift; Y <= Box.Max.Y >> AirBoxBucketShift; ++Y)
		{
			for (int32 X = Box.Min.X >> AirBoxBucketShift; X <= Box.Max.X >> AirBoxBucketShift; ++X)
			{
				const TArray<FAirBox>* Bucket = AirBoxBuckets.Find(MakeAirBucketKey(X, Y, Z));

				if (Bucket && Bucket->ContainsByPredicate([&Box](const FAirBox& Air) { return Air.Intersects(Box); }))
				{
					return false;
				}
			}
		}
	}

	return true;
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
	UsableNatureStructures.Reset();
	NatureFillCache.Reset();
	CategoryIndices.Reset();
	CategoriesByIndex.Reset();

	// Numbered in the order the structures list them, so the same list numbers them the same way.
	auto IndexCategories = [this](const UPlatformStructureData* Structure)
	{
		for (const UStructureCategory* Category : Structure->Categories)
		{
			if (Category && !CategoryIndices.Contains(Category))
			{
				CategoryIndices.Add(Category, CategoriesByIndex.Add(Category));
			}
		}
	};

	for (UPlatformStructureData* Structure : Structures)
	{
		// An empty entry is only one not filled in yet, and a repeat adds nothing.
		if (!Structure || UsableStructures.Contains(Structure) || UsableNatureStructures.Contains(Structure))
		{
			continue;
		}

		// Nature is laid by the nature beat alone, one-sided whatever it is set to, and drawn flat while it has no mesh.
		if (Structure->IsNature())
		{
			if (Structure->Weight <= 0.0f)
			{
				continue;
			}

			if (Structure->bTwoSided || Structure->bFence)
			{
				OutProblems.Add(FString::Printf(TEXT("nature structure '%s' is set two-sided or a fence, which nature never is - it fills its own face, with an urban structure behind it"),
					*Structure->GetName()));
			}

			UsableNatureStructures.Add(Structure);
			IndexCategories(Structure);
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
		IndexCategories(Structure);
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

template <typename FunctorType>
void ARoomManager::ForEachMeeting(const FGridPlatform& Platform, const int32 PlatformIndex, FunctorType&& Visit) const
{
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

			if (Owners[First] == PlatformIndex && Owners[Second] != PlatformIndex)
			{
				Visit(FirstSign, Owners[Second], SecondSign, Edge);
			}

			if (Owners[Second] == PlatformIndex && Owners[First] != PlatformIndex)
			{
				Visit(SecondSign, Owners[First], FirstSign, Edge);
			}
		}
	});
}

void ARoomManager::GatherFaceNeighbours(
	const FGridPlatform& Platform,
	const int32 PlatformIndex,
	const int32 FaceSign,
	TArray<int32, TInlineAllocator<16>>& OutNeighbours) const
{
	OutNeighbours.Reset();

	ForEachMeeting(Platform, PlatformIndex, [&OutNeighbours, FaceSign](const int32 OwnSign, const int32 Neighbour, const int32 NeighbourSign, const FLatticeEdge&)
	{
		if (OwnSign == FaceSign)
		{
			OutNeighbours.AddUnique(GetFaceFillIndex(Neighbour, NeighbourSign));
		}
	});
}

UPlatformStructureData* ARoomManager::GetFaceStructure(const FGridPlatform& Platform, const int32 Sign)
{
	return Sign > 0 ? Platform.PositiveStructure.Get() : Platform.NegativeStructure.Get();
}

bool ARoomManager::GetNatureFace(const FGridPlatform& Platform, int32& OutSign, ENatureForm& OutForm)
{
	for (const int32 Sign : { 1, -1 })
	{
		if (const UPlatformStructureData* Structure = GetFaceStructure(Platform, Sign); Structure && Structure->IsNature())
		{
			OutSign = Sign;
			OutForm = Structure->NatureForm;
			return true;
		}
	}

	return false;
}

bool ARoomManager::IsNatureSegment(const FGridPlatform& Platform)
{
	int32 Sign;
	ENatureForm Form;
	return GetNatureFace(Platform, Sign, Form);
}

bool ARoomManager::AcceptsMeeting(const FGridPlatform& Nature, const int32 NatureSign, const FGridPlatform& Other, const int32 OtherSign)
{
	const UPlatformStructureData* NatureFace = GetFaceStructure(Nature, NatureSign);
	const UPlatformStructureData* OtherFace = GetFaceStructure(Other, OtherSign);

	if (!NatureFace || !NatureFace->IsNature())
	{
		return true;
	}

	// Two platforms meeting round a beam they share either lie in one plane or fold at it.
	const bool bCoplanar = Nature.Normal == Other.Normal;
	const ENatureForm Form = NatureFace->NatureForm;

	if (OtherFace && OtherFace->IsNature())
	{
		const ENatureForm OtherForm = OtherFace->NatureForm;

		// Ground and water lie in one sheet. A cliff carries a cliff on with the same Up, beside it, over it or
		// round a corner.
		if (Form != ENatureForm::Cliff && OtherForm != ENatureForm::Cliff)
		{
			return bCoplanar;
		}

		if (Form == ENatureForm::Cliff && OtherForm == ENatureForm::Cliff)
		{
			return Nature.Up == Other.Up;
		}

		// A cliff and ground or water fold at the cliff's top or foot: an edge across its Up, which leaves the
		// cliff's Up along the other's normal.
		const FGridPlatform& Cliff = Form == ENatureForm::Cliff ? Nature : Other;
		const FGridPlatform& Field = Form == ENatureForm::Cliff ? Other : Nature;

		return !bCoplanar && FaceAxis(Cliff.Up) == AxisIndex(Field.Normal);
	}

	// Of the city, only ground takes a face, and only an exterior - one of both sides counts. A face with no
	// structure, laid before there were structures, takes anything.
	return Form == ENatureForm::Ground && (!OtherFace || OtherFace->StandsFor(EStructureSide::Exterior));
}

uint8 ARoomManager::GetNatureMeetsBit(const ENatureForm Form)
{
	return static_cast<uint8>(1 << (1 + static_cast<int32>(Form)));
}

uint8 ARoomManager::GetOutlineMeets()
{
	return GetNatureMeetsBit(ENatureForm::Ground) | GetNatureMeetsBit(ENatureForm::Water);
}

bool ARoomManager::AreNatureMeetingsLegal(const FGridPlatform& Platform, uint8* OutNatureMeets) const
{
	bool bLegal = true;
	uint8 NatureMeets = 0;

	ForEachMeeting(Platform, BuiltPlatforms.Num(), [&](const int32 OwnSign, const int32 Neighbour, const int32 NeighbourSign, const FLatticeEdge&)
	{
		const FGridPlatform& Other = BuiltPlatforms[Neighbour];
		const UPlatformStructureData* OwnFace = GetFaceStructure(Platform, OwnSign);
		const UPlatformStructureData* OtherFace = GetFaceStructure(Other, NeighbourSign);

		// Each side's nature has its say, so a meeting stands only where both take it.
		if (OwnFace && OwnFace->IsNature())
		{
			NatureMeets |= OtherFace && OtherFace->IsNature() ? GetNatureMeetsBit(OtherFace->NatureForm) : NatureMeetsCity;
			bLegal = bLegal && AcceptsMeeting(Platform, OwnSign, Other, NeighbourSign);
		}

		if (OtherFace && OtherFace->IsNature())
		{
			bLegal = bLegal && AcceptsMeeting(Other, NeighbourSign, Platform, OwnSign);
		}
	});

	if (OutNatureMeets)
	{
		*OutNatureMeets = NatureMeets;
	}

	return bLegal;
}

void ARoomManager::GetNatureNeighbourFaces(const FGridPlatform& Platform, uint8& OutOnGround, uint8& OutOnClosed) const
{
	OutOnGround = 0;
	OutOnClosed = 0;

	ForEachMeeting(Platform, BuiltPlatforms.Num(), [&](const int32 OwnSign, const int32 Neighbour, const int32 NeighbourSign, const FLatticeEdge&)
	{
		const UPlatformStructureData* OtherFace = GetFaceStructure(BuiltPlatforms[Neighbour], NeighbourSign);

		if (OtherFace && OtherFace->IsNature())
		{
			(OtherFace->NatureForm == ENatureForm::Ground ? OutOnGround : OutOnClosed) |= FaceBit(OwnSign);
		}
	});
}

template <typename FunctorType>
bool ARoomManager::IsOnOutline(const FGridPlatform& Wall, const int32 FaceSign, FunctorType&& IsOutlineFace) const
{
	int32 SpanU, SpanV, U, V;
	if (!Wall.IsWall() || !GetPlatformSpans(Wall, SpanU, SpanV))
	{
		return false;
	}

	GetInPlaneAxes(AxisIndex(Wall.Normal), U, V);

	// Its foot and its top are the rim running across its Up, at either end of it.
	const int32 Module = GetModule();
	const int32 UpAxis = FaceAxis(Wall.Up);
	const int32 AcrossAxis = UpAxis == U ? V : U;
	const int32 EdgesAcross = (AcrossAxis == U ? SpanU : SpanV) / Module;

	TArray<uint64, TInlineAllocator<16>> FootEdges;
	TArray<uint64, TInlineAllocator<16>> TopEdges;
	TArray<uint64, TInlineAllocator<16>> SeamEdges;

	ForEachMeeting(Wall, BuiltPlatforms.Num(), [&](const int32 OwnSign, const int32 Neighbour, const int32 NeighbourSign, const FLatticeEdge& Edge)
	{
		const FGridPlatform& Other = BuiltPlatforms[Neighbour];

		if (Edge.Axis != AcrossAxis || !IsOutlineFace(GetFaceStructure(Other, NeighbourSign), Other, NeighbourSign))
		{
			return;
		}

		// The same behind it as well as in front makes the edge a seam within, not the outline.
		if (OwnSign != FaceSign)
		{
			SeamEdges.AddUnique(MakeEdgeKey(Edge));
			return;
		}

		(Edge.Node[UpAxis] == Wall.MinNode[UpAxis] ? FootEdges : TopEdges).AddUnique(MakeEdgeKey(Edge));
	});

	auto IsWholeOutline = [&SeamEdges, EdgesAcross](const TConstArrayView<uint64> Edges)
	{
		return Edges.Num() == EdgesAcross && !Edges.ContainsByPredicate([&SeamEdges](const uint64 Key) { return SeamEdges.Contains(Key); });
	};

	return IsWholeOutline(FootEdges) || IsWholeOutline(TopEdges);
}

bool ARoomManager::StandsOnOutline(const FGridPlatform& Cliff, const int32 NatureSign) const
{
	return IsOnOutline(Cliff, NatureSign, [](const UPlatformStructureData* Face, const FGridPlatform&, int32)
	{
		return Face && Face->IsNature() && Face->NatureForm != ENatureForm::Cliff;
	});
}

bool ARoomManager::StandsOnGroundOutline(const FGridPlatform& Wall, const int32 FaceSign, const EStructureSide Side) const
{
	return IsOnOutline(Wall, FaceSign, [Side](const UPlatformStructureData* Face, const FGridPlatform& Floor, const int32 FloorSign)
	{
		return Face && !Face->IsNature() && Face->StandsFor(Side)
			&& EnumHasAnyFlags(WEMStructures::GetTypesOnFace(*Face, AxisIndex(Floor.Normal), FloorSign, Floor.Up), EStructureType::Ground);
	});
}

bool ARoomManager::MeetsClosedNature(const FGridPlatform& Platform, uint8* OutFacesOnGround) const
{
	uint8 OnGround = 0;
	uint8 OnClosed = 0;
	GetNatureNeighbourFaces(Platform, OnGround, OnClosed);

	if (OutFacesOnGround)
	{
		*OutFacesOnGround = OnGround;
	}

	return OnClosed != 0;
}

bool ARoomManager::DrawNatureFill(FGridPlatform& Platform, const ENatureForm Form, const int32 NatureSign, FRandomStream& Stream, UPlatformStructureData* Nature)
{
	Platform.PositiveStructure = nullptr;
	Platform.NegativeStructure = nullptr;
	Platform.NatureCover = nullptr;

	int32 SpanU, SpanV;
	if (!GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return false;
	}

	const int32 NormalAxis = AxisIndex(Platform.Normal);
	const bool bWall = Platform.IsWall();

	// A cliff stands as a wall in its own frame; ground and water lie along their Up, looking out along it.
	if (bWall != (Form == ENatureForm::Cliff) || (!bWall && NatureSign != FaceSign(Platform.Up)))
	{
		return false;
	}

	auto Fits = [&](const UPlatformStructureData* Structure)
	{
		bool bTurned = false;
		return WEMStructures::FitsShape(Structure->Width, Structure->Length, Structure->bMayLieSideways, NormalAxis, Platform.Up, SpanU, SpanV, bTurned);
	};

	// The nature face, from the nature of the form - a slab of its colour fits any opening - or the one a run carries on.
	TArray<UPlatformStructureData*> Options;

	for (UPlatformStructureData* Structure : UsableNatureStructures)
	{
		if (Structure->NatureForm == Form && (!Nature || Structure == Nature) && (!Structure->Mesh || Fits(Structure)))
		{
			Options.Add(Structure);
		}
	}

	UPlatformStructureData* Front = DrawStructureFrom(Stream, Options, Platform, NatureSign);

	if (!Front)
	{
		return false;
	}

	// Behind it any one-sided urban structure of the face's type there: a ceiling under ground and water, a wall
	// behind a cliff. Whatever it is, the platform holds up what grows off it. The back is the city's, so it meets
	// nature the way the city does: an exterior where it meets ground, and never water or a cliff.
	uint8 BackOnGround = 0;
	uint8 BackOnClosed = 0;
	GetNatureNeighbourFaces(Platform, BackOnGround, BackOnClosed);

	if ((BackOnClosed & FaceBit(-NatureSign)) != 0)
	{
		return false;
	}

	const bool bExteriorBack = (BackOnGround & FaceBit(-NatureSign)) != 0;
	const EStructureType BackType = bWall ? EStructureType::Wall : EStructureType::Ceiling;
	Options.Reset();

	for (UPlatformStructureData* Structure : UsableStructures)
	{
		if (!Structure->bTwoSided
			&& EnumHasAnyFlags(WEMStructures::GetTypesOnFace(*Structure, NormalAxis, -NatureSign, Platform.Up), BackType)
			&& (!bExteriorBack || Structure->StandsFor(EStructureSide::Exterior))
			&& Fits(Structure))
		{
			Options.Add(Structure);
		}
	}

	UPlatformStructureData* Back = DrawStructureFrom(Stream, Options, Platform, -NatureSign);

	if (!Back)
	{
		return false;
	}

	Platform.PositiveStructure = NatureSign > 0 ? Front : Back;
	Platform.NegativeStructure = NatureSign > 0 ? Back : Front;

	// Its face covered by one of the city's structures from the categories the nature names, of the type the face
	// takes: a ground on ground and water, a wall on a cliff. With none that fits, it keeps its own look.
	if (Front->CoverCategories.IsEmpty())
	{
		return true;
	}

	const EStructureType CoverType = bWall ? EStructureType::Wall : EStructureType::Ground;
	Options.Reset();

	for (UPlatformStructureData* Structure : UsableStructures)
	{
		const bool bInCoverCategory = Structure->Categories.ContainsByPredicate([Front](const TObjectPtr<UStructureCategory>& Category)
		{
			return Category && Front->CoverCategories.Contains(Category);
		});

		if (bInCoverCategory
			&& !Structure->bTwoSided
			&& EnumHasAnyFlags(WEMStructures::GetTypesOnFace(*Structure, NormalAxis, NatureSign, Platform.Up), CoverType)
			&& Fits(Structure))
		{
			Options.Add(Structure);
		}
	}

	if (!Options.IsEmpty())
	{
		Platform.NatureCover = DrawStructureFrom(Stream, Options, Platform, NatureSign);
	}

	return true;
}

FTransform ARoomManager::MakeFlatSlabTransform(const FGridPlatform& Platform, const int32 FaceSign) const
{
	int32 SpanU, SpanV, U, V;
	GetPlatformSpans(Platform, SpanU, SpanV);

	const int32 NormalAxis = AxisIndex(Platform.Normal);
	GetInPlaneAxes(NormalAxis, U, V);

	// The opening between the beams, half a cell thick, its back on the platform's mid-plane.
	FVector Extent;
	Extent[U] = static_cast<double>(SpanU - 1) * CellSize;
	Extent[V] = static_cast<double>(SpanV - 1) * CellSize;
	Extent[NormalAxis] = 0.5 * CellSize;

	FVector Centre = GetCellMinCorner(Platform.MinNode);
	Centre[U] += 0.5 * static_cast<double>(SpanU + 1) * CellSize;
	Centre[V] += 0.5 * static_cast<double>(SpanV + 1) * CellSize;
	Centre[NormalAxis] += (0.5 + 0.25 * FaceSign) * CellSize;

	return FTransform(FRotator::ZeroRotator, Centre, Extent / UnitCubeSize);
}

bool ARoomManager::PlaceNextNature()
{
	if (BuiltPlatforms.IsEmpty() || UsableNatureStructures.IsEmpty())
	{
		return false;
	}

	// A field still spilling out takes the beat, a piece a beat, the oldest first. One with no room left stops
	// where it is.
	while (!SpillQueue.IsEmpty())
	{
		FNatureSpill& Spill = SpillQueue[0];
		const int32 Module = GetModule();

		TMap<FNatureCandidateKey, int32> Seen;
		TArray<FNatureCandidate> Candidates;

		for (const FGridPlatform& Piece : Spill.Pieces)
		{
			int32 SpanU, SpanV;
			GetPlatformSpans(Piece, SpanU, SpanV);

			ForEachModuleEdge(Piece, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
			{
				if (bRim)
				{
					GatherNatureAround(Piece, Edge.Node, Edge.Axis, Spill.Form, GetNatureMeetsBit(Spill.Form), Seen, Candidates);
				}
			});
		}

		// Likelier where it would meet more of the field, so it spills out as a patch rather than a line.
		while (!Candidates.IsEmpty())
		{
			int32 TotalWeight = 0;
			for (const FNatureCandidate& Candidate : Candidates)
			{
				TotalWeight += Candidate.Weight;
			}

			int32 Roll = NatureStream.RandRange(0, TotalWeight - 1);
			int32 Chosen = 0;

			while (Roll >= Candidates[Chosen].Weight)
			{
				Roll -= Candidates[Chosen].Weight;
				++Chosen;
			}

			FNatureCandidate Candidate = Candidates[Chosen];

			if (PlaceNatureCandidate(Candidate))
			{
				Spill.Pieces.Add(Candidate.Platform);

				if (--Spill.Remaining <= 0)
				{
					SpillQueue.RemoveAt(0);
				}

				return true;
			}

			Candidates.RemoveAt(Chosen);
		}

		SpillQueue.RemoveAt(0);
	}

	// Then a cliff still running along an outline, a piece a beat.
	if (PlaceCliffPanel())
	{
		return true;
	}

	// Otherwise something new, its form drawn by weight - and another drawn in its place where the one drawn has
	// nowhere to go, so a beat is only lost when nature has nowhere at all.
	TArray<ENatureForm, TInlineAllocator<NatureFormCount>> Forms = { ENatureForm::Ground, ENatureForm::Cliff, ENatureForm::Water };

	auto GetFormWeight = [this](const ENatureForm Form)
	{
		return FMath::Max(Form == ENatureForm::Ground ? GroundWeight : (Form == ENatureForm::Cliff ? CliffWeight : WaterWeight), 0.0f);
	};

	while (!Forms.IsEmpty())
	{
		float TotalWeight = 0.0f;
		for (const ENatureForm Form : Forms)
		{
			TotalWeight += GetFormWeight(Form);
		}

		if (TotalWeight <= 0.0f)
		{
			return false;
		}

		float Roll = NatureStream.FRand() * TotalWeight;
		ENatureForm Drawn = Forms.Last();

		for (const ENatureForm Form : Forms)
		{
			const float Weight = GetFormWeight(Form);

			if (Weight <= 0.0f)
			{
				continue;
			}

			Drawn = Form;

			if (Roll < Weight)
			{
				break;
			}

			Roll -= Weight;
		}

		if (PlaceNatureOfForm(Drawn))
		{
			return true;
		}

		Forms.Remove(Drawn);
	}

	return false;
}

bool ARoomManager::PlaceNatureOfForm(const ENatureForm Form)
{
	TMap<FNatureCandidateKey, int32> Seen;
	TArray<FNatureCandidate> Candidates;

	// What each starts against: ground an exterior or a cliff's top or foot, a cliff the outline of ground or water,
	// water ground or a cliff's top or foot. Only a field's own spill grows off more of itself.
	const uint8 Starts = Form == ENatureForm::Ground
		? (NatureMeetsCity | GetNatureMeetsBit(ENatureForm::Cliff))
		: (Form == ENatureForm::Cliff ? GetOutlineMeets() : (GetNatureMeetsBit(ENatureForm::Ground) | GetNatureMeetsBit(ENatureForm::Cliff)));

	// Ground starts against an exterior: a sample of the city's own open placements - only those lying flat,
	// unless it may face any way - each looking whichever ways NatureFacing allows.
	if (Form == ENatureForm::Ground)
	{
		EnsurePlatformCandidates();

		const bool bFlatOnly = NatureFacing != ENatureFacing::AnyWay;

		auto IsSampled = [bFlatOnly](const int32 Kind)
		{
			return !bFlatOnly || Kind == KindIndex(EPlatformKind::SurfaceHorizontal);
		};

		int32 Listed = 0;
		for (const FTileCandidates& TileList : TileCandidates)
		{
			for (int32 Kind = 0; Kind < PlatformKindCount; ++Kind)
			{
				Listed += IsSampled(Kind) ? TileList.ByKind[Kind].Num() : 0;
			}
		}

		for (int32 Sample = 0; Listed > 0 && Sample < NatureSampleSeeds; ++Sample)
		{
			int32 Pick = NatureStream.RandRange(0, Listed - 1);
			int32 PickedTile = INDEX_NONE;
			FGridPlatform Placement;

			for (int32 TileIndex = 0; PickedTile == INDEX_NONE && TileIndex < TileCandidates.Num(); ++TileIndex)
			{
				for (int32 Kind = 0; Kind < PlatformKindCount; ++Kind)
				{
					const TArray<FGridPlatform>& KindList = TileCandidates[TileIndex].ByKind[Kind];

					if (!IsSampled(Kind))
					{
						continue;
					}

					if (Pick < KindList.Num())
					{
						Placement = KindList[Pick];
						PickedTile = TileIndex;
						break;
					}

					Pick -= KindList.Num();
				}
			}

			// Looking up, it only ever lies with its face to the sky; otherwise either way, the first drawn.
			const bool bUpOnly = NatureFacing == ENatureFacing::Up;
			const int32 FirstSign = bUpOnly || NatureStream.FRand() < 0.5f ? 1 : -1;

			for (int32 SignIndex = 0; SignIndex < (bUpOnly ? 1 : 2); ++SignIndex)
			{
				const int32 Sign = SignIndex == 0 ? FirstSign : -FirstSign;

				FGridPlatform Platform = Placement;
				Platform.Up = MakeFace(AxisIndex(Platform.Normal), Sign);

				const FNatureCandidateKey Key(MakeCandidateKey(Platform), PickedTile, static_cast<int32>(Platform.Up) * 2 + (Sign > 0 ? 0 : 1));

				if (Seen.Contains(Key))
				{
					continue;
				}

				const bool bCandidate = JudgeNatureCandidate(Platform, Form, Sign, Starts);
				Seen.Add(Key, bCandidate ? Candidates.Num() : INDEX_NONE);

				if (bCandidate)
				{
					Candidates.Add(FNatureCandidate{ Platform, Form, Sign, 1 });
				}
			}
		}
	}

	// What grows off nature, off a sample of nature's rim: ground at a cliff's top or foot; a cliff off the outline of
	// ground or water, never off a cliff alone; water beside ground or at a cliff's top or foot.
	TArray<int32> Anchors;

	if (Form != ENatureForm::Cliff)
	{
		Anchors.Append(NatureIndices[static_cast<int32>(ENatureForm::Cliff)]);
	}

	if (Form != ENatureForm::Ground)
	{
		Anchors.Append(NatureIndices[static_cast<int32>(ENatureForm::Ground)]);
	}

	if (Form == ENatureForm::Cliff)
	{
		Anchors.Append(NatureIndices[static_cast<int32>(ENatureForm::Water)]);
	}

	const int32 Module = GetModule();

	for (int32 Sample = 0; !Anchors.IsEmpty() && Sample < NatureSampleEdges; ++Sample)
	{
		const FGridPlatform& Anchor = BuiltPlatforms[Anchors[NatureStream.RandRange(0, Anchors.Num() - 1)]];

		int32 SpanU, SpanV;
		GetPlatformSpans(Anchor, SpanU, SpanV);

		TArray<FLatticeEdge, TInlineAllocator<32>> RimEdges;
		ForEachModuleEdge(Anchor, SpanU, SpanV, Module, [&RimEdges](const FLatticeEdge& Edge, const bool bRim)
		{
			if (bRim)
			{
				RimEdges.Add(Edge);
			}
		});

		const FLatticeEdge& Edge = RimEdges[NatureStream.RandRange(0, RimEdges.Num() - 1)];
		GatherNatureAround(Anchor, Edge.Node, Edge.Axis, Form, Starts, Seen, Candidates);
	}

	// Drawn evenly among what was found: anywhere it was found, it could start.
	while (!Candidates.IsEmpty())
	{
		const int32 Chosen = NatureStream.RandRange(0, Candidates.Num() - 1);
		FNatureCandidate Candidate = Candidates[Chosen];

		if (PlaceNatureCandidate(Candidate))
		{
			// Ground and water spill out from here, to a size drawn now.
			if (Form != ENatureForm::Cliff)
			{
				const int32 Fewest = FMath::Max(Form == ENatureForm::Ground ? GroundSpillMin : WaterSpillMin, 1);
				const int32 Most = FMath::Max(Form == ENatureForm::Ground ? GroundSpillMax : WaterSpillMax, Fewest);
				const int32 Size = NatureStream.RandRange(Fewest, Most);

				if (Size > 1)
				{
					FNatureSpill& Spill = SpillQueue.AddDefaulted_GetRef();
					Spill.Form = Form;
					Spill.Pieces.Add(Candidate.Platform);
					Spill.Remaining = Size - 1;
				}
			}

			return true;
		}

		Candidates.RemoveAt(Chosen);
	}

	return false;
}

void ARoomManager::GatherNatureAround(
	const FGridPlatform& Anchor,
	const FIntVector& EdgeNode,
	const int32 EdgeAxis,
	const ENatureForm Form,
	const uint8 Starts,
	TMap<FNatureCandidateKey, int32>& Seen,
	TArray<FNatureCandidate>& Out)
{
	int32 AnchorSign = 0;
	ENatureForm AnchorForm = ENatureForm::Ground;

	if (!GetNatureFace(Anchor, AnchorSign, AnchorForm))
	{
		return;
	}

	const int32 Module = GetModule();
	const FLatticeEdge Edge{ EdgeNode, EdgeAxis };
	const int32 AnchorNormal = AxisIndex(Anchor.Normal);
	const int32 AnchorUpAxis = FaceAxis(Anchor.Up);

	// How it grows off the anchor: a field carries a field on in its plane, a cliff stands on a field's outline, and a
	// field lies at a cliff's top or foot. A cliff carries a cliff on only as its run does (see PlaceCliffPanel).
	if (Form == ENatureForm::Cliff && AnchorForm == ENatureForm::Cliff)
	{
		return;
	}

	const bool bFieldOffField = Form != ENatureForm::Cliff && AnchorForm != ENatureForm::Cliff;
	const bool bInPlane = bFieldOffField;
	const bool bAcross = !bFieldOffField;

	// Nothing to judge where every half-plane it could take round the edge is already taken.
	FModuleFace EdgeFaces[4];
	GetEdgeFaces(Edge, Module, EdgeFaces);

	bool bAnyFree = false;

	for (const FModuleFace& Face : EdgeFaces)
	{
		const bool bWanted = Face.Normal == AnchorNormal ? bInPlane : bAcross;
		bAnyFree = bAnyFree || (bWanted && !ModuleFaceOwners.Contains(MakeModuleFaceKey(Face.MinNode, Face.Normal)));
	}

	if (!bAnyFree)
	{
		return;
	}

	ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/true, [&](const int32 TileIndex, const FGridPlatform& Placement, bool)
	{
		const int32 NormalAxis = AxisIndex(Placement.Normal);
		const bool bCoplanar = NormalAxis == AnchorNormal;

		if (bCoplanar ? !bInPlane : !bAcross)
		{
			return;
		}

		// The Ups it could take, each with the face that would be nature. Carrying a field on, the field's own; at a
		// cliff's top or foot, either way along its own normal. A cliff takes the Up of what it grows off, which lies
		// in its plane.
		TArray<TPair<EGridFace, int32>, TInlineAllocator<2>> Ways;

		if (bFieldOffField)
		{
			Ways.Emplace(Anchor.Up, FaceSign(Anchor.Up));
		}
		else if (Form != ENatureForm::Cliff)
		{
			if (NormalAxis == AnchorUpAxis)
			{
				Ways.Emplace(MakeFace(NormalAxis, 1), 1);
				Ways.Emplace(MakeFace(NormalAxis, -1), -1);
			}
		}
		else if (AnchorUpAxis != NormalAxis)
		{
			Ways.Emplace(Anchor.Up, 1);
			Ways.Emplace(Anchor.Up, -1);
		}

		for (const TPair<EGridFace, int32>& Way : Ways)
		{
			FGridPlatform Platform = Placement;
			Platform.Up = Way.Key;

			const FNatureCandidateKey Key(MakeCandidateKey(Platform), TileIndex, static_cast<int32>(Way.Key) * 2 + (Way.Value > 0 ? 0 : 1));

			// Found again off another edge it would share: likelier to be drawn as a field's next piece.
			if (const int32* Found = Seen.Find(Key))
			{
				if (*Found != INDEX_NONE)
				{
					++Out[*Found].Weight;
				}

				continue;
			}

			const bool bCandidate = JudgeNatureCandidate(Platform, Form, Way.Value, Starts);
			Seen.Add(Key, bCandidate ? Out.Num() : INDEX_NONE);

			if (bCandidate)
			{
				Out.Add(FNatureCandidate{ Platform, Form, Way.Value, 1 });
			}
		}
	});
}

bool ARoomManager::AllowsNatureFacing(const FGridPlatform& Platform) const
{
	// Ground and water look out along their Up, and a cliff stands with the Up of what it grows off, so the Up
	// answers for both: along world Z when they lie flat, and +Z itself when they look up.
	switch (NatureFacing)
	{
	case ENatureFacing::Up:
		return Platform.Up == EGridFace::PosZ;

	case ENatureFacing::Flat:
		return FaceAxis(Platform.Up) == AxisIndex(EGridAxis::Z);

	default:
		return true;
	}
}

bool ARoomManager::JudgeNatureCandidate(FGridPlatform& Platform, const ENatureForm Form, const int32 NatureSign, const uint8 Starts)
{
	UPlatformStructureData* Front = nullptr;
	UPlatformStructureData* Back = nullptr;

	if (!AllowsNatureFacing(Platform)
		|| !Platform.Tile
		|| !Platform.Tile->AllowsOrientation(Platform.GetKind() == EPlatformKind::SurfaceVertical)
		|| !FindNatureFill(Platform, Form, NatureSign, Front, Back))
	{
		return false;
	}

	Platform.PositiveStructure = NatureSign > 0 ? Front : Back;
	Platform.NegativeStructure = NatureSign > 0 ? Back : Front;

	// Free, and legal to place filled so - which asks every face it would meet - with its nature face against what
	// it may start against.
	uint8 NatureMeets = 0;

	if (!IsPlacementOpen(Platform) || !AreNatureMeetingsLegal(Platform, &NatureMeets) || (NatureMeets & Starts) == 0)
	{
		return false;
	}

	// A cliff stands wholly on the outline of ground or water, its foot or its top; ground and water only go where the
	// air above them is clear.
	return Form == ENatureForm::Cliff ? StandsOnOutline(Platform, NatureSign) : IsAirClear(Platform);
}

bool ARoomManager::FindNatureFill(
	const FGridPlatform& Platform,
	const ENatureForm Form,
	const int32 NatureSign,
	UPlatformStructureData*& OutFront,
	UPlatformStructureData*& OutBack)
{
	OutFront = nullptr;
	OutBack = nullptr;

	int32 SpanU, SpanV;
	if (!GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return false;
	}

	const int32 NormalAxis = AxisIndex(Platform.Normal);
	const bool bWall = Platform.IsWall();

	// A cliff stands as a wall in its own frame; ground and water lie along their Up, looking out along it.
	if (bWall != (Form == ENatureForm::Cliff) || (!bWall && NatureSign != FaceSign(Platform.Up)))
	{
		return false;
	}

	const uint64 Key = static_cast<uint64>(SpanU)
		| (static_cast<uint64>(SpanV) << 12)
		| (static_cast<uint64>(NormalAxis) << 24)
		| (static_cast<uint64>(Platform.Up) << 28)
		| (static_cast<uint64>(Form) << 32)
		| (static_cast<uint64>(NatureSign > 0 ? 1 : 0) << 36);

	if (const FNatureFillStandIns* Cached = NatureFillCache.Find(Key))
	{
		OutFront = Cached->Front;
		OutBack = Cached->Back;
		return OutFront && OutBack;
	}

	auto Fits = [&](const UPlatformStructureData* Structure)
	{
		bool bTurned = false;
		return WEMStructures::FitsShape(Structure->Width, Structure->Length, Structure->bMayLieSideways, NormalAxis, Platform.Up, SpanU, SpanV, bTurned);
	};

	FNatureFillStandIns StandIns;

	for (UPlatformStructureData* Structure : UsableNatureStructures)
	{
		if (Structure->NatureForm == Form && (!Structure->Mesh || Fits(Structure)))
		{
			StandIns.Front = Structure;
			break;
		}
	}

	const EStructureType BackType = bWall ? EStructureType::Wall : EStructureType::Ceiling;

	for (UPlatformStructureData* Structure : UsableStructures)
	{
		if (Structure->bTwoSided
			|| !EnumHasAnyFlags(WEMStructures::GetTypesOnFace(*Structure, NormalAxis, -NatureSign, Platform.Up), BackType)
			|| !Fits(Structure))
		{
			continue;
		}

		// An exterior where one fits, since that is what the most nature takes.
		if (!StandIns.Back || Structure->StandsFor(EStructureSide::Exterior))
		{
			StandIns.Back = Structure;
		}

		if (StandIns.Back->StandsFor(EStructureSide::Exterior))
		{
			break;
		}
	}

	NatureFillCache.Add(Key, StandIns);

	OutFront = StandIns.Front;
	OutBack = StandIns.Back;
	return OutFront && OutBack;
}

bool ARoomManager::PlaceNatureCandidate(FNatureCandidate& Candidate, const int32 JoinedSide)
{
	// Its stand-ins only said it could be filled. What fills it is drawn now, and held to the rules again.
	if (!DrawNatureFill(Candidate.Platform, Candidate.Form, Candidate.NatureSign, NatureStream, Candidate.Nature) || !CanPlacePlatform(Candidate.Platform))
	{
		return false;
	}

	PlacePlatform(Candidate.Platform, /*JoinedSide=*/0);

	// A cliff carries on from its ends as a fence does, the room each has judged with it standing.
	if (Candidate.Form == ENatureForm::Cliff)
	{
		QueueRunEnds(Candidate.Platform, JoinedSide, CliffContinueChance, NatureStream, CliffQueue);
	}

	return true;
}

bool ARoomManager::PlaceCliffPanel()
{
	while (!CliffQueue.IsEmpty())
	{
		const FFenceEnd End = CliffQueue[0];
		CliffQueue.RemoveAt(0, EAllowShrinking::No);

		// A run of a cliff no longer among the structures stops where it is.
		UPlatformStructureData* Cliff = GetRunStructure(End.Panel);

		if (!Cliff || !UsableNatureStructures.Contains(Cliff))
		{
			continue;
		}

		FGridPlatform Straight;
		TArray<FGridPlatform, TInlineAllocator<2>> Turns;
		GatherFenceContinuations(End, Straight, Turns);

		// A run that has met itself here since the end was queued - closing round a field - stops.
		if (IsRunEndClosed(End, Straight, Turns))
		{
			continue;
		}

		// Each spot as a cliff on the outline of ground or water, its nature face looking either way. Straight on, only the
		// way the run's looks meets it legally; round a corner, only the way that meets the run's nature face.
		TArray<FNatureCandidate, TInlineAllocator<2>> StraightOn;
		TArray<FNatureCandidate, TInlineAllocator<4>> RoundCorners;

		auto Judge = [&](const FGridPlatform& Spot, auto& Out)
		{
			for (const int32 Sign : { 1, -1 })
			{
				FGridPlatform Platform = Spot;

				if (JudgeNatureCandidate(Platform, ENatureForm::Cliff, Sign, GetOutlineMeets()))
				{
					Out.Add(FNatureCandidate{ Platform, ENatureForm::Cliff, Sign, 1, Cliff });
				}
			}
		};

		Judge(Straight, StraightOn);

		for (const FGridPlatform& Turn : Turns)
		{
			Judge(Turn, RoundCorners);
		}

		// Straight on when nothing else is left, round a corner when only that is, and where both are, round a corner with
		// CliffTurnChance, split evenly between the ways left. One that cannot be filled after all gives way to the rest.
		while (!StraightOn.IsEmpty() || !RoundCorners.IsEmpty())
		{
			const bool bTurn = StraightOn.IsEmpty() || (!RoundCorners.IsEmpty() && NatureStream.FRand() < CliffTurnChance);
			FNatureCandidate Candidate;

			if (bTurn)
			{
				const int32 Chosen = RoundCorners.Num() > 1 ? NatureStream.RandRange(0, RoundCorners.Num() - 1) : 0;
				Candidate = RoundCorners[Chosen];
				RoundCorners.RemoveAt(Chosen);
			}
			else
			{
				Candidate = StraightOn[0];
				StraightOn.RemoveAt(0);
			}

			if (PlaceNatureCandidate(Candidate, GetJoinedSide(End, Candidate.Platform)))
			{
				return true;
			}
		}
	}

	return false;
}

void ARoomManager::AdvanceNature()
{
	PlaceNextNature();
}

bool ARoomManager::IsFieldPiece(const FGridPlatform& Platform)
{
	int32 Sign = 0;
	ENatureForm Form = ENatureForm::Ground;
	return GetNatureFace(Platform, Sign, Form) && Form != ENatureForm::Cliff;
}

int32 ARoomManager::GetAirHeight(const FGridPlatform& Platform) const
{
	const int32 Storey = FMath::Max(StoreyCells, 1);
	const int32 Fewest = FMath::Max(MinAirStoreys, 0) * Storey;
	const int32 Most = FMath::Max(FMath::Max(MaxAirStoreys, 0) * Storey, Fewest);

	// From the piece itself, so the same piece keeps the same air however often it is rebuilt.
	FRandomStream Stream(static_cast<int32>(HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(MakePlatformKey(Platform)))));
	return Stream.RandRange(Fewest, Most);
}

void ARoomManager::GatherAirBoxes(const FGridPlatform& Platform, const int32 PlatformIndex, const int32 Height, TArray<FAirBox>& OutBoxes) const
{
	OutBoxes.Reset();

	int32 SpanU, SpanV;
	if (Height <= 0 || !GetPlatformSpans(Platform, SpanU, SpanV))
	{
		return;
	}

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	const int32 Plane = Platform.MinNode[NormalAxis];

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// Out along its Up, which for a piece of a field is along its normal.
	auto MakeBox = [&](FIntVector Min, FIntVector Max, const int32 BoxHeight) -> FAirBox
	{
		const bool bOutPositive = FaceSign(Platform.Up) > 0;
		Min[NormalAxis] = bOutPositive ? Plane + 1 : Plane - BoxHeight;
		Max[NormalAxis] = bOutPositive ? Plane + BoxHeight : Plane - 1;
		return FAirBox{ Min, Max };
	};

	// Over its opening, between its beams.
	{
		FIntVector Min = Platform.MinNode;
		FIntVector Max = Platform.MinNode;
		Min[U] += 1;
		Min[V] += 1;
		Max[U] += SpanU - 1;
		Max[V] += SpanV - 1;
		OutBoxes.Add(MakeBox(Min, Max, Height));
	}

	// Over each run of seam it shares with a field beside it in its plane, laid before it with its Up - the lower
	// of the two heights - leaving the nodes at either end of the run, which may be on the outline, open.
	for (const int32 AcrossAxis : { U, V })
	{
		const int32 AlongAxis = AcrossAxis == U ? V : U;
		const int32 Span = AcrossAxis == U ? SpanU : SpanV;
		const int32 AlongSpan = AcrossAxis == U ? SpanV : SpanU;

		for (const int32 Side : { -1, 1 })
		{
			const int32 Line = Platform.MinNode[AcrossAxis] + (Side > 0 ? Span : 0);
			int32 RunStart = INDEX_NONE;
			int32 RunHeight = Height;

			auto CloseRun = [&](const int32 RunEnd)
			{
				if (RunStart == INDEX_NONE)
				{
					return;
				}

				FIntVector Min = Platform.MinNode;
				FIntVector Max = Platform.MinNode;
				Min[AcrossAxis] = Line;
				Max[AcrossAxis] = Line;
				Min[AlongAxis] = RunStart + 1;
				Max[AlongAxis] = RunEnd - 1;
				OutBoxes.Add(MakeBox(Min, Max, RunHeight));

				RunStart = INDEX_NONE;
				RunHeight = Height;
			};

			for (int32 Step = 0; Step < AlongSpan; Step += Module)
			{
				// The module face across this edge of its rim, in its plane.
				FIntVector FaceMinNode = Platform.MinNode;
				FaceMinNode[AcrossAxis] = Side > 0 ? Line : Line - Module;
				FaceMinNode[AlongAxis] += Step;

				const int32* FaceOwner = ModuleFaceOwners.Find(MakeModuleFaceKey(FaceMinNode, NormalAxis));
				const FGridPlatform* Neighbour = FaceOwner && *FaceOwner < PlatformIndex ? &BuiltPlatforms[*FaceOwner] : nullptr;
				const bool bShared = Neighbour && Neighbour->Up == Platform.Up && IsFieldPiece(*Neighbour);

				if (bShared)
				{
					if (RunStart == INDEX_NONE)
					{
						RunStart = Platform.MinNode[AlongAxis] + Step;
					}

					RunHeight = FMath::Min(RunHeight, GetAirHeight(*Neighbour));
				}
				else
				{
					CloseRun(Platform.MinNode[AlongAxis] + Step);
				}
			}

			CloseRun(Platform.MinNode[AlongAxis] + AlongSpan);
		}
	}
}

bool ARoomManager::IsAirClear(const FGridPlatform& Platform) const
{
	const int32 Fewest = FMath::Max(MinAirStoreys, 0) * FMath::Max(StoreyCells, 1);

	TArray<FAirBox> Boxes;
	GatherAirBoxes(Platform, BuiltPlatforms.Num(), Fewest, Boxes);

	bool bClear = true;

	// Clear of every platform: no module face, module edge or node any of them takes in. Air may overlap air.
	for (const FAirBox& Box : Boxes)
	{
		ForEachObstaclePiece(Box.Min, Box.Max, GetModule(),
			[&](const FIntVector& FaceMinNode, const int32 NormalAxis) { bClear = bClear && !ModuleFaceOwners.Contains(MakeModuleFaceKey(FaceMinNode, NormalAxis)); },
			[&](const FLatticeEdge& Edge) { bClear = bClear && !ModuleEdgeUses.Contains(MakeEdgeKey(Edge)); },
			[&](const FIntVector& Node) { bClear = bClear && !RimCellNormals.Contains(MakeCellKey(Node)); });

		if (!bClear)
		{
			return false;
		}
	}

	return true;
}

namespace
{
	/** The smallest box holding every one of a set. */
	FIntVector ComponentMin(const FIntVector& A, const FIntVector& B)
	{
		return FIntVector(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y), FMath::Min(A.Z, B.Z));
	}

	FIntVector ComponentMax(const FIntVector& A, const FIntVector& B)
	{
		return FIntVector(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y), FMath::Max(A.Z, B.Z));
	}
}

void ARoomManager::DropCandidatesIn(const TConstArrayView<FAirBox> Regions)
{
	// Lists that are going to be gathered afresh anyway hold nothing worth judging.
	if (Regions.IsEmpty() || !CanRefreshCandidateLists())
	{
		return;
	}

	FAirBox Bounds = Regions[0];

	for (const FAirBox& Region : Regions)
	{
		Bounds.Min = ComponentMin(Bounds.Min, Region.Min);
		Bounds.Max = ComponentMax(Bounds.Max, Region.Max);
	}

	// Only a placement reaching into a region can be held back by it, so only those are judged again - in their lists'
	// own order, which dropping some keeps. Most are passed over by the regions' bounds alone.
	for (FTileCandidates& TileList : TileCandidates)
	{
		for (TArray<FGridPlatform>& Candidates : TileList.ByKind)
		{
			for (int32 Index = Candidates.Num() - 1; Index >= 0; --Index)
			{
				const FAirBox Box = GetPlatformBox(Candidates[Index]);

				if (Bounds.Intersects(Box)
					&& Regions.ContainsByPredicate([&Box](const FAirBox& Region) { return Region.Intersects(Box); })
					&& !IsPlacementOpen(Candidates[Index]))
				{
					Candidates.RemoveAt(Index);
				}
			}
		}
	}
}

void ARoomManager::FreeCandidatesIn(const TConstArrayView<FAirBox> Regions)
{
	// The empty cube has a rule of its own, and lists that are going to be gathered afresh anyway need nothing.
	if (Regions.IsEmpty() || !CanRefreshCandidateLists() || BuiltPlatforms.IsEmpty())
	{
		return;
	}

	const int32 Module = GetModule();

	// A placement is only ever open sharing a module edge of rim with a platform, and then it lies within its own longest
	// side of that platform. So the platforms that near a region are the only ones whose placements need asking.
	int32 Reach = 0;

	for (const UPlatformTileData* Tile : UsableTiles)
	{
		Reach = FMath::Max(Reach, FMath::Max(Tile->Width, Tile->Length));
	}

	FAirBox Bounds = Regions[0];

	for (const FAirBox& Region : Regions)
	{
		Bounds.Min = ComponentMin(Bounds.Min, Region.Min);
		Bounds.Max = ComponentMax(Bounds.Max, Region.Max);
	}

	auto ReachesRegion = [&Bounds, &Regions](const FAirBox& Box)
	{
		return Bounds.Intersects(Box) && Regions.ContainsByPredicate([&Box](const FAirBox& Region) { return Region.Intersects(Box); });
	};

	TArray<TSet<uint64>> Judged;
	Judged.SetNum(TileCandidates.Num());

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		FAirBox Near = GetPlatformBox(Platform);
		Near.Min -= FIntVector(Reach);
		Near.Max += FIntVector(Reach);

		if (!ReachesRegion(Near))
		{
			continue;
		}

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
				if (!ReachesRegion(GetPlatformBox(Candidate)))
				{
					return;
				}

				bool bAlreadyJudged = false;
				Judged[TileIndex].Add(MakeCandidateKey(Candidate), &bAlreadyJudged);

				if (!bAlreadyJudged)
				{
					RefreshPlatformCandidate(TileIndex, Candidate, /*bMayHaveComeFree=*/true);
				}
			});
		});
	}
}

int32 ARoomManager::GetMushroomStoreys(const int32 Pieces) const
{
	if (!bMushroomAir || Pieces <= 1)
	{
		return 0;
	}

	return FMath::Clamp(FMath::FloorToInt32((Pieces - 1) * MushroomStoreysPerPiece), 0, FMath::Max(MaxMushroomStoreys, 0));
}

int32 ARoomManager::GetMushroomSpread(const int32 Pieces) const
{
	if (!bMushroomAir || Pieces <= 1)
	{
		return 0;
	}

	// In whole modules, so the cap's sides lie on lattice lines like everything else.
	const int32 Module = GetModule();
	const int32 Cells = FMath::Clamp(FMath::FloorToInt32((Pieces - 1) * MushroomSpreadPerPiece), 0, FMath::Max(MaxMushroomSpread, 0));
	return (Cells / Module) * Module;
}

void ARoomManager::GatherCrownBoxes(const FAirBox& Block, const int32 NormalAxis, const bool bOutPositive, const uint32 LandscapeHash, TArray<FAirBox>& OutBoxes) const
{
	const float Irregularity = FMath::Clamp(MushroomIrregularity, 0.0f, 1.0f);

	if (Irregularity <= 0.0f)
	{
		OutBoxes.Add(Block);
		return;
	}

	const int32 Module = GetModule();
	const int32 Storey = FMath::Max(StoreyCells, 1);
	const int32 LastCell = ResolveGridSize() - 1;
	const int32 Height = Block.Max[NormalAxis] - Block.Min[NormalAxis] + 1;
	const int32 Storeys = FMath::DivideAndRoundUp(Height, Storey);

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// In whole modules, so every side lies on lattice lines like the block's.
	auto Snap = [Module](const double Cells)
	{
		return FMath::RoundToInt32(Cells / Module) * Module;
	};

	for (int32 Level = 0; Level < Storeys; ++Level)
	{
		// This storey of it, counted out from the trunk.
		FAirBox Slab = Block;
		const int32 Near = Level * Storey;
		const int32 Far = FMath::Min(Near + Storey, Height) - 1;

		if (bOutPositive)
		{
			Slab.Min[NormalAxis] = Block.Min[NormalAxis] + Near;
			Slab.Max[NormalAxis] = Block.Min[NormalAxis] + Far;
		}
		else
		{
			Slab.Max[NormalAxis] = Block.Max[NormalAxis] - Near;
			Slab.Min[NormalAxis] = Block.Max[NormalAxis] - Far;
		}

		// From the landscape and the storey alone, so a storey keeps its shape while the crown grows round it.
		FRandomStream Stream(static_cast<int32>(HashCombineFast(
			HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(TEXT("Crowns")))),
			HashCombineFast(LandscapeHash, GetTypeHash(Level)))));

		// Each side in or out by up to a quarter of the block's width, at full irregularity. Two quarters in at most, so
		// the sides never cross.
		for (const int32 Axis : { U, V })
		{
			const double Wander = 0.25 * Irregularity * (Block.Max[Axis] - Block.Min[Axis]);
			Slab.Min[Axis] = FMath::Clamp(Slab.Min[Axis] - Snap(Wander * Stream.FRandRange(-1.0f, 1.0f)), 0, LastCell);
			Slab.Max[Axis] = FMath::Clamp(Slab.Max[Axis] + Snap(Wander * Stream.FRandRange(-1.0f, 1.0f)), Slab.Min[Axis], LastCell);
		}

		if (Storeys < 2 || Level < Storeys - 1)
		{
			OutBoxes.Add(Slab);
			continue;
		}

		// The top storey as two lumps, each over a part of it: the less irregular, the more of it.
		for (int32 Lump = 0; Lump < 2; ++Lump)
		{
			FAirBox Part = Slab;

			for (const int32 Axis : { U, V })
			{
				const int32 Width = Slab.Max[Axis] - Slab.Min[Axis];
				const int32 PartWidth = FMath::Clamp(Snap(Width * Stream.FRandRange(1.0f - 0.6f * Irregularity, 1.0f - 0.2f * Irregularity)), FMath::Min(Module, Width), Width);
				const int32 Room = Width - PartWidth;

				Part.Min[Axis] = Slab.Min[Axis] + FMath::FloorToInt32(Room * Stream.FRand() / Module) * Module;
				Part.Max[Axis] = Part.Min[Axis] + PartWidth;
			}

			OutBoxes.Add(Part);
		}
	}
}

void ARoomManager::GatherFieldAir(TArray<FAirBox>& OutBoxes, int32* OutLandscapes, int32* OutLargest) const
{
	OutBoxes.Reset();

	if (OutLandscapes)
	{
		*OutLandscapes = 0;
	}

	if (OutLargest)
	{
		*OutLargest = 0;
	}

	// The pieces of every field, in list order - the order their seams are laid in.
	TArray<int32> Pieces;
	Pieces.Append(NatureIndices[static_cast<int32>(ENatureForm::Ground)]);
	Pieces.Append(NatureIndices[static_cast<int32>(ENatureForm::Water)]);

	if (Pieces.IsEmpty())
	{
		return;
	}

	Pieces.Sort();

	TMap<int32, int32> Slots;
	Slots.Reserve(Pieces.Num());

	TArray<int32> Parents;
	Parents.SetNumUninitialized(Pieces.Num());

	for (int32 Slot = 0; Slot < Pieces.Num(); ++Slot)
	{
		Slots.Add(Pieces[Slot], Slot);
		Parents[Slot] = Slot;
	}

	auto FindRoot = [&Parents](int32 Slot)
	{
		while (Parents[Slot] != Slot)
		{
			Parents[Slot] = Parents[Parents[Slot]];
			Slot = Parents[Slot];
		}

		return Slot;
	};

	// Landscapes: the pieces joined across a seam in their plane, with their Up - ground and water alike.
	const int32 Module = GetModule();

	for (int32 Slot = 0; Slot < Pieces.Num(); ++Slot)
	{
		const FGridPlatform& Piece = BuiltPlatforms[Pieces[Slot]];
		const int32 NormalAxis = AxisIndex(Piece.Normal);

		int32 SpanU, SpanV, U, V;
		GetPlatformSpans(Piece, SpanU, SpanV);
		GetInPlaneAxes(NormalAxis, U, V);

		for (const int32 AcrossAxis : { U, V })
		{
			const int32 AlongAxis = AcrossAxis == U ? V : U;
			const int32 Span = AcrossAxis == U ? SpanU : SpanV;
			const int32 AlongSpan = AcrossAxis == U ? SpanV : SpanU;

			for (const int32 Side : { -1, 1 })
			{
				for (int32 Step = 0; Step < AlongSpan; Step += Module)
				{
					// The module face just past this side of it, in its plane.
					FIntVector FaceMinNode = Piece.MinNode;
					FaceMinNode[AcrossAxis] += Side > 0 ? Span : -Module;
					FaceMinNode[AlongAxis] += Step;

					const int32* FaceOwner = ModuleFaceOwners.Find(MakeModuleFaceKey(FaceMinNode, NormalAxis));
					const int32* Other = FaceOwner ? Slots.Find(*FaceOwner) : nullptr;

					if (Other && BuiltPlatforms[*FaceOwner].Up == Piece.Up)
					{
						const int32 Root = FindRoot(Slot);
						const int32 OtherRoot = FindRoot(*Other);

						// The lower slot is kept as root, so a landscape is named by its first piece whatever order they joined in.
						Parents[FMath::Max(Root, OtherRoot)] = FMath::Min(Root, OtherRoot);
					}
				}
			}
		}
	}

	TArray<int32> Sizes;
	Sizes.Init(0, Pieces.Num());

	for (int32 Slot = 0; Slot < Pieces.Num(); ++Slot)
	{
		++Sizes[FindRoot(Slot)];
	}

	// Each piece's own air - its trunk - as high as it drew, its seams the lower of their two.
	const int32 Storey = FMath::Max(StoreyCells, 1);
	TArray<FAirBox> Boxes;

	// Per landscape, by its root: how far it reaches in its plane, and the highest its trunks go.
	TMap<int32, TPair<FAirBox, int32>> Crowns;
	TArray<int32> Roots;

	for (int32 Slot = 0; Slot < Pieces.Num(); ++Slot)
	{
		const FGridPlatform& Piece = BuiltPlatforms[Pieces[Slot]];
		const int32 Root = FindRoot(Slot);
		const int32 Height = GetAirHeight(Piece);

		GatherAirBoxes(Piece, Pieces[Slot], Height, Boxes);
		OutBoxes.Append(Boxes);

		const FAirBox PieceBox = GetPlatformBox(Piece);

		if (TPair<FAirBox, int32>* Crown = Crowns.Find(Root))
		{
			Crown->Key.Min = ComponentMin(Crown->Key.Min, PieceBox.Min);
			Crown->Key.Max = ComponentMax(Crown->Key.Max, PieceBox.Max);
			Crown->Value = FMath::Max(Crown->Value, Height);
		}
		else
		{
			Crowns.Add(Root, TPair<FAirBox, int32>(PieceBox, Height));
			Roots.Add(Root);
		}
	}

	// Then each landscape's crown, once it has grown a storey: above the trunk - never in the first storey, which stays as
	// wide as the landscape - up to the trunks' top raised by the storeys it has grown, and reaching past the landscape's
	// edges in its plane.
	const int32 LastCell = ResolveGridSize() - 1;
	const int32 TrunkTop = FMath::Max(MinAirStoreys, 1) * Storey;

	for (const int32 Root : Roots)
	{
		const int32 Grown = GetMushroomStoreys(Sizes[Root]) * Storey;

		if (Grown <= 0)
		{
			continue;
		}

		const FGridPlatform& First = BuiltPlatforms[Pieces[Root]];
		const int32 NormalAxis = AxisIndex(First.Normal);
		const bool bOutPositive = FaceSign(First.Up) > 0;
		const TPair<FAirBox, int32>& Crown = Crowns.FindChecked(Root);
		const int32 Top = FMath::Max(Crown.Value, TrunkTop) + Grown;
		const int32 Spread = GetMushroomSpread(Sizes[Root]);
		const int32 Plane = First.MinNode[NormalAxis];

		int32 U, V;
		GetInPlaneAxes(NormalAxis, U, V);

		FAirBox Box = Crown.Key;

		for (const int32 Axis : { U, V })
		{
			Box.Min[Axis] = FMath::Max(Box.Min[Axis] - Spread, 0);
			Box.Max[Axis] = FMath::Min(Box.Max[Axis] + Spread, LastCell);
		}

		Box.Min[NormalAxis] = bOutPositive ? Plane + TrunkTop + 1 : Plane - Top;
		Box.Max[NormalAxis] = bOutPositive ? Plane + Top : Plane - TrunkTop - 1;
		GatherCrownBoxes(Box, NormalAxis, bOutPositive, GetTypeHash(MakePlatformKey(First)), OutBoxes);
	}

	if (OutLandscapes)
	{
		*OutLandscapes = Roots.Num();
	}

	if (OutLargest)
	{
		for (const int32 Size : Sizes)
		{
			*OutLargest = FMath::Max(*OutLargest, Size);
		}
	}
}

void ARoomManager::AddAirBoxToBuckets(const FAirBox& Box)
{
	// Only what lies in the cube can hold a platform back.
	const int32 LastCell = ResolveGridSize() - 1;
	const FIntVector Min = ComponentMax(Box.Min, FIntVector::ZeroValue);
	const FIntVector Max = ComponentMin(Box.Max, FIntVector(LastCell));

	for (int32 Z = Min.Z >> AirBoxBucketShift; Z <= Max.Z >> AirBoxBucketShift; ++Z)
	{
		for (int32 Y = Min.Y >> AirBoxBucketShift; Y <= Max.Y >> AirBoxBucketShift; ++Y)
		{
			for (int32 X = Min.X >> AirBoxBucketShift; X <= Max.X >> AirBoxBucketShift; ++X)
			{
				AirBoxBuckets.FindOrAdd(MakeAirBucketKey(X, Y, Z)).Add(Box);
			}
		}
	}
}

void ARoomManager::RemoveAirBoxFromBuckets(const FAirBox& Box)
{
	const int32 LastCell = ResolveGridSize() - 1;
	const FIntVector Min = ComponentMax(Box.Min, FIntVector::ZeroValue);
	const FIntVector Max = ComponentMin(Box.Max, FIntVector(LastCell));

	for (int32 Z = Min.Z >> AirBoxBucketShift; Z <= Max.Z >> AirBoxBucketShift; ++Z)
	{
		for (int32 Y = Min.Y >> AirBoxBucketShift; Y <= Max.Y >> AirBoxBucketShift; ++Y)
		{
			for (int32 X = Min.X >> AirBoxBucketShift; X <= Max.X >> AirBoxBucketShift; ++X)
			{
				const uint64 Key = MakeAirBucketKey(X, Y, Z);

				if (TArray<FAirBox>* Bucket = AirBoxBuckets.Find(Key))
				{
					Bucket->RemoveSingleSwap(Box, EAllowShrinking::No);

					if (Bucket->IsEmpty())
					{
						AirBoxBuckets.Remove(Key);
					}
				}
			}
		}
	}
}

void ARoomManager::UpdateFieldAir(const bool bRefreshLists)
{
	TArray<FAirBox> NewBoxes;
	int32 Landscapes = 0;
	int32 Largest = 0;
	GatherFieldAir(NewBoxes, &Landscapes, &Largest);

	// What changed: the old boxes counted off against the new, the new left over added and the old left over gone.
	TMap<FAirBox, int32> Kept;

	for (const FAirBox& Box : AirBoxes)
	{
		++Kept.FindOrAdd(Box);
	}

	TArray<FAirBox> Added;

	for (const FAirBox& Box : NewBoxes)
	{
		int32* Count = Kept.Find(Box);

		if (Count && *Count > 0)
		{
			--*Count;
		}
		else
		{
			Added.Add(Box);
		}
	}

	TArray<FAirBox> Gone;

	for (const TPair<FAirBox, int32>& Box : Kept)
	{
		for (int32 Copy = 0; Copy < Box.Value; ++Copy)
		{
			Gone.Add(Box.Key);
		}
	}

	for (const FAirBox& Box : Gone)
	{
		RemoveAirBoxFromBuckets(Box);
	}

	for (const FAirBox& Box : Added)
	{
		AddAirBoxToBuckets(Box);
	}

	AirBoxes = MoveTemp(NewBoxes);

	AirSummary = Landscapes == 0 ? TEXT("No landscapes") : FString::Printf(TEXT("%d landscapes, %d boxes of air; the largest, %d pieces, grows a crown %d storeys above its trunk, reaching %d cells past its edges"),
		Landscapes, AirBoxes.Num(), Largest, GetMushroomStoreys(Largest), GetMushroomSpread(Largest));

	if (!bRefreshLists || (Added.IsEmpty() && Gone.IsEmpty()))
	{
		return;
	}

	DropCandidatesIn(Added);

	// Where a box has gone, what it held back may have come free - all but what a box in its place still holds, as when
	// a crown only rises or widens.
	TArray<FAirBox> Freed;

	for (const FAirBox& Box : Gone)
	{
		bool bHandled = false;

		for (const FAirBox& Other : Added)
		{
			if (Other.Min.X <= Box.Min.X && Other.Min.Y <= Box.Min.Y && Other.Min.Z <= Box.Min.Z
				&& Other.Max.X >= Box.Max.X && Other.Max.Y >= Box.Max.Y && Other.Max.Z >= Box.Max.Z)
			{
				bHandled = true;
				break;
			}

			// The same but along one axis: what lies past the new one along it.
			int32 DifferingAxis = INDEX_NONE;
			int32 Differing = 0;

			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (Box.Min[Axis] != Other.Min[Axis] || Box.Max[Axis] != Other.Max[Axis])
				{
					DifferingAxis = Axis;
					++Differing;
				}
			}

			if (Differing == 1 && Box.Intersects(Other))
			{
				if (Box.Min[DifferingAxis] < Other.Min[DifferingAxis])
				{
					FAirBox Part = Box;
					Part.Max[DifferingAxis] = Other.Min[DifferingAxis] - 1;
					Freed.Add(Part);
				}

				if (Box.Max[DifferingAxis] > Other.Max[DifferingAxis])
				{
					FAirBox Part = Box;
					Part.Min[DifferingAxis] = Other.Max[DifferingAxis] + 1;
					Freed.Add(Part);
				}

				bHandled = true;
				break;
			}
		}

		if (!bHandled)
		{
			Freed.Add(Box);
		}
	}

	FreeCandidatesIn(Freed);
}

void ARoomManager::RebuildAirLines()
{
	if (!AirLineBatcher)
	{
		return;
	}

	AirLineBatcher->Flush();

	TArray<FBatchedLine> Lines;

	// Each box of air as far as it lies inside the cube, its outer faces on the cells' outer sides.
	if (bShowAir)
	{
		const int32 LastCell = ResolveGridSize() - 1;
		const FLinearColor Color(AirColor);

		Lines.Reserve(AirBoxes.Num() * 12);

		for (const FAirBox& Box : AirBoxes)
		{
			const FIntVector Min(FMath::Max(Box.Min.X, 0), FMath::Max(Box.Min.Y, 0), FMath::Max(Box.Min.Z, 0));
			const FIntVector Max(FMath::Min(Box.Max.X, LastCell), FMath::Min(Box.Max.Y, LastCell), FMath::Min(Box.Max.Z, LastCell));

			if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
			{
				continue;
			}

			AppendBoxLines(Lines, GetCellMinCorner(Min), GetCellMinCorner(Max) + FVector(CellSize), Color, GridLineThickness, SDPG_World, /*bWithDiagonals=*/false);
		}
	}

	for (const FAirPocket& Pocket : AirPockets)
	{
		if (Pocket.bMold && bShowMolds)
		{
			AppendMoldLines(Pocket, Lines);
		}
		else if (!Pocket.bMold && bShowAirPockets)
		{
			AppendAirPocketLines(Pocket, Lines);
		}
	}

	if (!Lines.IsEmpty())
	{
		AirLineBatcher->DrawLines(Lines);
	}

	// What each pocket holds back, a point at the middle of every piece it cuts: a module face's inside, a module
	// edge's run, a node.
	if (bShowAirPocketPieces)
	{
		const double Middle = 0.5 * static_cast<double>(GetModule());

		for (const FAirPocket& Pocket : AirPockets)
		{
			const FLinearColor Color = Pocket.bMold ? FLinearColor(MoldColor) : GetAirPocketColor(Pocket.Shape);

			auto DrawPiece = [&](const uint64 Key, const int32 AcrossFirst, const int32 AcrossSecond)
			{
				FVector Point = FVector(CellFromKey(Key)) + FVector(0.5);

				for (const int32 Axis : { AcrossFirst, AcrossSecond })
				{
					if (Axis != INDEX_NONE)
					{
						Point[Axis] += Middle - 0.5;
					}
				}

				AirLineBatcher->DrawPoint(CellPointToWorld(Point), Color, AirPocketPieceSize, SDPG_World);
			};

			// A face or an edge key carries its axis in its tag, offset by one.
			auto GetTag = [](const uint64 Key)
			{
				return static_cast<int32>(Key >> KeyTagShift) - 1;
			};

			for (const uint64 Key : Pocket.FaceKeys)
			{
				int32 U, V;
				GetInPlaneAxes(GetTag(Key), U, V);
				DrawPiece(Key, U, V);
			}

			for (const uint64 Key : Pocket.EdgeKeys)
			{
				DrawPiece(Key, GetTag(Key), INDEX_NONE);
			}

			for (const uint64 Key : Pocket.NodeKeys)
			{
				DrawPiece(Key, INDEX_NONE, INDEX_NONE);
			}
		}
	}
}

bool ARoomManager::PlacePocket(FAirPocket& Pocket, FRandomStream& Stream, const double Spread, const int32 Ignore, const bool bKeepFirstPlace) const
{
	const int32 CubeCells = ResolveGridSize();

	// The first platform's place, which neither a mold nor a pocket drawn at a clear may take. Its fill decides where it
	// can go, so the structures have to be resolved already.
	TArray<FGridPlatform> FirstCandidates;

	if (bKeepFirstPlace)
	{
		GatherFirstPlatformCandidates(FirstCandidates);
	}

	auto TakesFirstPlace = [this, &FirstCandidates, &Pocket]()
	{
		return FirstCandidates.ContainsByPredicate([this, &Pocket](const FGridPlatform& First)
		{
			int32 SpanU, SpanV;
			GetPlatformSpans(First, SpanU, SpanV);

			bool bTaken = false;
			ForEachPlatformCell(First, SpanU, SpanV, [&](const FIntVector& Cell, bool)
			{
				bTaken = bTaken || IsCellInPocket(Pocket, Cell);
			});

			return bTaken;
		});
	};

	// Whether its reach meets a mold's box: molds stay clear of each other, and pockets of molds.
	auto MeetsMold = [this, &Pocket, Ignore]()
	{
		FVector Min, Max;
		GetPocketBounds(Pocket, Min, Max);

		for (int32 Index = 0; Index < AirPockets.Num(); ++Index)
		{
			const FAirPocket& Other = AirPockets[Index];

			if (Index == Ignore || !Other.bMold)
			{
				continue;
			}

			FVector OtherMin, OtherMax;
			GetPocketBounds(Other, OtherMin, OtherMax);

			if (Min.X <= OtherMax.X && OtherMin.X <= Max.X
				&& Min.Y <= OtherMax.Y && OtherMin.Y <= Max.Y
				&& Min.Z <= OtherMax.Z && OtherMin.Z <= Max.Z)
			{
				return true;
			}
		}

		return false;
	};

	// Within Spread of the cube's middle - all of it at 1 - drawn again while it would take the first platform's place or
	// meet a mold.
	for (int32 Attempt = 0; Attempt < MaxAirPocketPlaces; ++Attempt)
	{
		const FVector Offset(Stream.FRandRange(-1.0f, 1.0f), Stream.FRandRange(-1.0f, 1.0f), Stream.FRandRange(-1.0f, 1.0f));
		Pocket.Centre = (FVector(1.0) + Offset * Spread) * (0.5 * static_cast<double>(CubeCells));

		if (!TakesFirstPlace() && !MeetsMold())
		{
			return true;
		}
	}

	return false;
}

double ARoomManager::DrawPocketLife(FRandomStream& Stream) const
{
	// Lives drawn so that half of them are over by the half-life, and the rest go on breaking down at the same rate.
	const double HalfLife = FMath::Max(PocketHalfLife, 1.0f);
	const double Life = -HalfLife / FMath::Loge(2.0) * FMath::Loge(FMath::Max(1.0 - static_cast<double>(Stream.FRand()), UE_DOUBLE_SMALL_NUMBER));

	return FMath::Max(Life, static_cast<double>(FMath::Max(MinPocketLife, 0.0f)));
}

void ARoomManager::CountPocketSlabKeys(const FAirPocket& Pocket, const int32 Slab, const int32 Delta)
{
	if (!Pocket.SlabEnds.IsValidIndex(Slab))
	{
		return;
	}

	const FIntVector Start = Slab > 0 ? Pocket.SlabEnds[Slab - 1] : FIntVector::ZeroValue;
	const FIntVector End = Pocket.SlabEnds[Slab];

	// A piece stays held back while another pocket still reaches it.
	auto Tally = [Delta](TMap<uint64, int32>& Uses, const TArray<uint64>& Keys, const int32 From, const int32 To)
	{
		for (int32 Index = From; Index < To; ++Index)
		{
			int32& Reaching = Uses.FindOrAdd(Keys[Index]);
			Reaching += Delta;

			if (Reaching <= 0)
			{
				Uses.Remove(Keys[Index]);
			}
		}
	};

	Tally(PocketFaces, Pocket.FaceKeys, Start.X, End.X);
	Tally(PocketEdges, Pocket.EdgeKeys, Start.Y, End.Y);
	Tally(PocketNodes, Pocket.NodeKeys, Start.Z, End.Z);
}

void ARoomManager::AddPocketKeys(const FAirPocket& Pocket)
{
	for (int32 Slab = 0; Slab < Pocket.SlabEnds.Num(); ++Slab)
	{
		CountPocketSlabKeys(Pocket, Slab, 1);
	}
}

void ARoomManager::RemovePocketKeys(const FAirPocket& Pocket)
{
	for (int32 Slab = 0; Slab < Pocket.SlabEnds.Num(); ++Slab)
	{
		CountPocketSlabKeys(Pocket, Slab, -1);
	}
}

void ARoomManager::RebuildPocketKeys()
{
	PocketFaces.Reset();
	PocketEdges.Reset();
	PocketNodes.Reset();

	// A move under way is finished where it was heading: every pocket is cut whole for the lattice as it now stands.
	MovingPocket = INDEX_NONE;
	bPocketForming = false;

	for (FAirPocket& Pocket : AirPockets)
	{
		CutPocketPieces(Pocket);
		AddPocketKeys(Pocket);
	}
}

int32 ARoomManager::GetPocketSlabCount(const FAirPocket& Pocket) const
{
	const FAirBox Bounds = GetPocketCellBounds(Pocket);
	const int32 Module = GetModule();

	return Bounds.Max.Z / Module - Bounds.Min.Z / Module + 1;
}

ARoomManager::FAirBox ARoomManager::GetPocketSlabBox(const FAirPocket& Pocket, const int32 Slab) const
{
	// A slab is one band of the lattice along Z, from a node layer to the cell below the next: every piece's cells lie
	// in exactly one, so a pocket cut a slab at a time cuts every piece once.
	const FAirBox Bounds = GetPocketCellBounds(Pocket);
	const int32 Module = GetModule();
	const int32 Band = Bounds.Min.Z / Module + Slab;

	FAirBox Box = Bounds;
	Box.Min.Z = FMath::Max(Bounds.Min.Z, Band * Module);
	Box.Max.Z = FMath::Min(Bounds.Max.Z, (Band + 1) * Module - 1);
	return Box;
}

ARoomManager::FAirBox ARoomManager::GetPocketCellBounds(const FAirPocket& Pocket) const
{
	const int32 LastCell = ResolveGridSize() - 1;

	FVector PocketMin, PocketMax;
	GetPocketBounds(Pocket, PocketMin, PocketMax);

	FAirBox Box;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Box.Min[Axis] = FMath::Clamp(FMath::FloorToInt32(PocketMin[Axis]), 0, LastCell);
		Box.Max[Axis] = FMath::Clamp(FMath::CeilToInt32(PocketMax[Axis]), 0, LastCell);
	}

	return Box;
}

bool ARoomManager::IsInMoldBox(const FGridPlatform& Platform) const
{
	const FAirBox Box = GetPlatformBox(Platform);
	const FVector Middle = 0.5 * FVector(Box.Min + Box.Max) + FVector(0.5);

	return AirPockets.ContainsByPredicate([this, &Middle](const FAirPocket& Pocket)
	{
		if (!Pocket.bMold)
		{
			return false;
		}

		FVector Min, Max;
		GetPocketBounds(Pocket, Min, Max);

		return Middle.X >= Min.X && Middle.X <= Max.X
			&& Middle.Y >= Min.Y && Middle.Y <= Max.Y
			&& Middle.Z >= Min.Z && Middle.Z <= Max.Z;
	});
}

void ARoomManager::StepPocketMove()
{
	if (!AirPockets.IsValidIndex(MovingPocket))
	{
		MovingPocket = INDEX_NONE;
		return;
	}

	const int32 Index = MovingPocket;
	FAirPocket& Pocket = AirPockets[Index];

	if (!bPocketForming)
	{
		// Breaking down, from the top: one slab let go, and what it held back may come free.
		if (!Pocket.SlabEnds.IsEmpty())
		{
			const int32 Slab = Pocket.SlabEnds.Num() - 1;
			const FIntVector Start = Slab > 0 ? Pocket.SlabEnds[Slab - 1] : FIntVector::ZeroValue;

			// Only where the slab's pieces lie, which is often far less than the slab across its bounds.
			FAirBox Region;
			const bool bHeldAny = GetPocketPiecesBox(Pocket, Start, Pocket.SlabEnds[Slab], Region);

			CountPocketSlabKeys(Pocket, Slab, -1);
			Pocket.FaceKeys.SetNum(Start.X);
			Pocket.EdgeKeys.SetNum(Start.Y);
			Pocket.NodeKeys.SetNum(Start.Z);
			Pocket.SlabEnds.Pop();

			if (bHeldAny)
			{
				FreeCandidatesIn(MakeArrayView(&Region, 1));
			}

			return;
		}

		// Gone: drawn again somewhere else, the same shape and measures, from a stream of its own per pocket and draw so
		// one moving leaves every other where it is - a pocket anywhere, a mold near the middle, clear of the molds. Where no
		// place drawn is clear it forms again where it was.
		++Pocket.Draw;
		++PocketMoves;

		FRandomStream Stream(static_cast<int32>(HashCombineFast(
			HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(TEXT("PocketMoves")))),
			HashCombineFast(GetTypeHash(Index), GetTypeHash(Pocket.Draw)))));

		const FVector WasCentre = Pocket.Centre;

		if (!PlacePocket(Pocket, Stream, Pocket.bMold ? FMath::Clamp(MoldSpread, 0.0f, 1.0f) : 1.0, Index, /*bKeepFirstPlace=*/false))
		{
			Pocket.Centre = WasCentre;
		}

		Pocket.MoveAt = GetClock() + DrawPocketLife(Stream);
		bPocketForming = true;

		UpdateDeletionSummary();
		RebuildAirLines();
		return;
	}

	// Forming, from the bottom: one slab cut and held, and what is listed there drops out.
	if (Pocket.SlabEnds.Num() < GetPocketSlabCount(Pocket))
	{
		const FIntVector Start = Pocket.SlabEnds.IsEmpty() ? FIntVector::ZeroValue : Pocket.SlabEnds.Last();

		CutPocketSlab(Pocket);
		CountPocketSlabKeys(Pocket, Pocket.SlabEnds.Num() - 1, 1);

		FAirBox Region;

		if (GetPocketPiecesBox(Pocket, Start, Pocket.SlabEnds.Last(), Region))
		{
			DropCandidatesIn(MakeArrayView(&Region, 1));
		}
	}

	if (Pocket.SlabEnds.Num() >= GetPocketSlabCount(Pocket))
	{
		MovingPocket = INDEX_NONE;
		bPocketForming = false;
	}
}

bool ARoomManager::GetPocketPiecesBox(const FAirPocket& Pocket, const FIntVector& From, const FIntVector& To, FAirBox& OutBox) const
{
	const int32 Module = GetModule();
	bool bAny = false;

	// Each piece within a module on from its min corner, whatever it is: a face, an edge or a node.
	auto Take = [&](const TArray<uint64>& Keys, const int32 First, const int32 End)
	{
		for (int32 Index = First; Index < End; ++Index)
		{
			const FIntVector Corner = CellFromKey(Keys[Index]);
			OutBox.Min = bAny ? ComponentMin(OutBox.Min, Corner) : Corner;
			OutBox.Max = bAny ? ComponentMax(OutBox.Max, Corner + FIntVector(Module)) : Corner + FIntVector(Module);
			bAny = true;
		}
	};

	Take(Pocket.FaceKeys, From.X, To.X);
	Take(Pocket.EdgeKeys, From.Y, To.Y);
	Take(Pocket.NodeKeys, From.Z, To.Z);
	return bAny;
}

void ARoomManager::MovePocket(const int32 Index)
{
	if (!AirPockets.IsValidIndex(Index))
	{
		return;
	}

	// Any move under way first, then this one, every slab of it at once.
	while (MovingPocket != INDEX_NONE)
	{
		StepPocketMove();
	}

	MovingPocket = Index;
	bPocketForming = false;

	while (MovingPocket != INDEX_NONE)
	{
		StepPocketMove();
	}
}

void ARoomManager::MoveAirPockets()
{
	for (int32 Index = 0; Index < AirPockets.Num(); ++Index)
	{
		MovePocket(Index);
	}
}

void ARoomManager::AdvancePockets(const double Now)
{
	// A move under way goes on a slab a tick, switched off or not, so nothing is left half formed.
	if (MovingPocket != INDEX_NONE)
	{
		StepPocketMove();
		return;
	}

	if (!bMovePockets)
	{
		return;
	}

	// One at a time, the longest due first.
	int32 Due = INDEX_NONE;

	for (int32 Index = 0; Index < AirPockets.Num(); ++Index)
	{
		if (AirPockets[Index].MoveAt <= Now && (Due == INDEX_NONE || AirPockets[Index].MoveAt < AirPockets[Due].MoveAt))
		{
			Due = Index;
		}
	}

	if (Due != INDEX_NONE)
	{
		MovingPocket = Due;
		bPocketForming = false;
		StepPocketMove();
	}
}

void ARoomManager::SpawnAirPockets()
{
	// The pockets drawn before go, and so does what they held back, a move under way with them.
	AirPockets.Reset();
	PocketFaces.Reset();
	PocketEdges.Reset();
	PocketNodes.Reset();
	MovingPocket = INDEX_NONE;
	bPocketForming = false;

	// The first platform's place, which neither a mold nor a pocket may take, follows from the structures.
	if (AirPocketCount > 0 || MoldCount > 0)
	{
		ResolveUsableTiles();
	}

	auto Place = [this](FAirPocket& Pocket, FRandomStream& Stream, const double Spread)
	{
		return PlacePocket(Pocket, Stream, Spread, INDEX_NONE, /*bKeepFirstPlace=*/true);
	};

	auto DrawByWeight = [](FRandomStream& Stream, const TConstArrayView<float> Weights)
	{
		float TotalWeight = 0.0f;
		for (const float Weight : Weights)
		{
			TotalWeight += FMath::Max(Weight, 0.0f);
		}

		if (TotalWeight <= 0.0f)
		{
			return int32(INDEX_NONE);
		}

		float Roll = Stream.FRand() * TotalWeight;
		int32 Drawn = INDEX_NONE;

		for (int32 Index = 0; Index < Weights.Num(); ++Index)
		{
			const float Weight = FMath::Max(Weights[Index], 0.0f);

			if (Weight <= 0.0f)
			{
				continue;
			}

			// Taken whenever it is in the running, so rounding at the very top of the roll still lands on one.
			Drawn = Index;

			if (Roll < Weight)
			{
				break;
			}

			Roll -= Weight;
		}

		return Drawn;
	};

	// The molds first, from a stream of their own, so the pockets can be drawn clear of them.
	const float MoldWeights[] = { MoldZigguratWeight, MoldCylinderWeight, MoldDomeWeight, MoldTowerWeight, MoldCombinationWeight };
	int32 MoldCounts[UE_ARRAY_COUNT(MoldWeights)] = {};
	int32 MoldsLeftOut = 0;
	int32 OpenTops = 0;

	{
		FRandomStream Stream(static_cast<int32>(HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(TEXT("Molds"))))));

		// Which open at the top, from a stream of their own, so the molds' shapes and places are drawn as they were.
		FRandomStream TopStream(static_cast<int32>(HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(TEXT("MoldTops"))))));
		const int32 Smallest = FMath::Max(MinMoldSize, 4);
		const int32 Largest = FMath::Max(MaxMoldSize, Smallest);

		for (int32 Index = 0; Index < MoldCount; ++Index)
		{
			const int32 ShapeIndex = DrawByWeight(Stream, MoldWeights);

			if (ShapeIndex == INDEX_NONE)
			{
				break;
			}

			FAirPocket Mold;
			Mold.bMold = true;
			Mold.Shape = EAirPocketShape::Rectangular;
			Mold.MoldShape = static_cast<EMoldShape>(ShapeIndex);

			const double First = 0.5 * static_cast<double>(Stream.RandRange(Smallest, Largest));
			const double Second = 0.5 * static_cast<double>(Stream.RandRange(Smallest, Largest));
			Mold.HalfSize = FVector(First, Second, 0.5 * static_cast<double>(Stream.RandRange(Smallest, Largest)));

			if (bMoldsAlongAnyAxis)
			{
				Mold.Axis = Stream.RandRange(0, 2);
				Mold.AxisSign = Stream.FRand() < 0.5f ? 1 : -1;
			}

			// Its open floor, and above it room for a shape at least two modules tall.
			Mold.OpenFloor = FMath::Max(MoldOpenFloor, 0);
			Mold.HalfSize.Z = FMath::Max(Mold.HalfSize.Z, 0.5 * (Mold.OpenFloor + 2.0 * GetModule()));

			// A dome closes over, so it never opens at the top.
			Mold.bOpenTop = TopStream.FRand() < MoldOpenTopChance && Mold.MoldShape != EMoldShape::Dome;

			DrawMoldParts(Mold, Stream);
			DrawPocketWarps(Mold, Index);

			if (!Place(Mold, Stream, FMath::Clamp(MoldSpread, 0.0f, 1.0f)))
			{
				++MoldsLeftOut;
				continue;
			}

			CutPocketPieces(Mold);
			++MoldCounts[ShapeIndex];
			OpenTops += Mold.bOpenTop ? 1 : 0;
			AirPockets.Add(MoveTemp(Mold));
		}
	}

	// Then the pockets, which may overlap each other but never a mold.
	const float Weights[] = { SphereWeight, BoxWeight, RectangularWeight, ZigguratWeight, CylinderWeight };
	int32 Counts[UE_ARRAY_COUNT(Weights)] = {};
	int32 LeftOut = 0;

	{
		// From the run's seed, a stream apart from the others, so a seed draws the same pockets every time.
		FRandomStream Stream(static_cast<int32>(HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(TEXT("AirPockets"))))));
		const int32 Smallest = FMath::Max(MinAirPocketSize, 1);
		const int32 Largest = FMath::Max(MaxAirPocketSize, Smallest);

		auto DrawHalf = [&Stream, Smallest, Largest]()
		{
			return 0.5 * static_cast<double>(Stream.RandRange(Smallest, Largest));
		};

		for (int32 Index = 0; Index < AirPocketCount; ++Index)
		{
			const int32 ShapeIndex = DrawByWeight(Stream, Weights);

			if (ShapeIndex == INDEX_NONE)
			{
				break;
			}

			FAirPocket Pocket;
			Pocket.Shape = static_cast<EAirPocketShape>(ShapeIndex);

			// Half its measure across, then along its axis: one measure for a sphere and a cube, three apart for a
			// rectangular box, a base and a height for a ziggurat, a diameter and a length for a cylinder.
			switch (Pocket.Shape)
			{
			case EAirPocketShape::Rectangular:
			{
				const double First = DrawHalf();
				const double Second = DrawHalf();
				Pocket.HalfSize = FVector(First, Second, DrawHalf());
				break;
			}
			case EAirPocketShape::Ziggurat:
			case EAirPocketShape::Cylinder:
			{
				const double Across = DrawHalf();
				Pocket.HalfSize = FVector(Across, Across, DrawHalf());
				break;
			}
			default:
				Pocket.HalfSize = FVector(DrawHalf());
				break;
			}

			if (bAirPocketsAlongAnyAxis)
			{
				Pocket.Axis = Stream.RandRange(0, 2);
				Pocket.AxisSign = Stream.FRand() < 0.5f ? 1 : -1;
			}

			DrawPocketWarps(Pocket, Index);

			if (!Place(Pocket, Stream, 1.0))
			{
				++LeftOut;
				continue;
			}

			CutPocketPieces(Pocket);
			++Counts[ShapeIndex];
			AirPockets.Add(MoveTemp(Pocket));
		}
	}

	// Every one holds back what it reaches from now on, and lasts a life of its own before it moves - drawn from the
	// stream it will move by, so how long it lasts takes nothing from the streams the cube is drawn from.
	const double Now = GetClock();

	for (int32 Index = 0; Index < AirPockets.Num(); ++Index)
	{
		FAirPocket& Pocket = AirPockets[Index];
		AddPocketKeys(Pocket);

		FRandomStream Stream(static_cast<int32>(HashCombineFast(
			HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(TEXT("PocketMoves")))),
			HashCombineFast(GetTypeHash(Index), GetTypeHash(Pocket.Draw)))));

		Pocket.MoveAt = Now + DrawPocketLife(Stream);
	}

	AirPocketSummary =FString::Printf(TEXT("%d pockets: %d sphere, %d box, %d rectangular, %d ziggurat, %d cylinder; %d left out"),
		Counts[0] + Counts[1] + Counts[2] + Counts[3] + Counts[4], Counts[0], Counts[1], Counts[2], Counts[3], Counts[4], LeftOut);

	MoldSummary = FString::Printf(TEXT("%d molds: %d ziggurat, %d cylinder, %d dome, %d tower, %d combination; %d open at the top; %d left out"),
		MoldCounts[0] + MoldCounts[1] + MoldCounts[2] + MoldCounts[3] + MoldCounts[4],
		MoldCounts[0], MoldCounts[1], MoldCounts[2], MoldCounts[3], MoldCounts[4], OpenTops, MoldsLeftOut);
}

void ARoomManager::DrawMoldParts(FAirPocket& Mold, FRandomStream& Stream) const
{
	Mold.Parts.Reset();

	const float LeastFill = FMath::Clamp(MinMoldFill, 0.05f, 1.0f);
	const float MostFill = FMath::Clamp(FMath::Max(MaxMoldFill, LeastFill), 0.05f, 1.0f);

	// How wide and how tall the open shape is, as parts of its box: never narrower than a module, so growth fits in. Open
	// at the top, it runs the whole height of the box above its floor - its height drawn all the same, so the draws after
	// it stay as they were.
	const double Across = FMath::Min(Mold.HalfSize.X, Mold.HalfSize.Y);
	const double Half = FMath::Max(Across * Stream.FRandRange(LeastFill, MostFill), 0.5 * GetModule());
	const double Room = 2.0 * Mold.HalfSize.Z - Mold.OpenFloor;
	const double DrawnTall = FMath::Max(Room * Stream.FRandRange(LeastFill, MostFill), static_cast<double>(GetModule()));
	const double Tall = Mold.bOpenTop ? Room : DrawnTall;
	const double Floor = Mold.OpenFloor;

	using FMoldPart = FAirPocket::FMoldPart;

	if (Mold.MoldShape != EMoldShape::Combination)
	{
		// A dome is as tall as it is wide, and no taller than its share of the box.
		const double Radius = FMath::Min(Half, Tall);
		Mold.Parts.Add(Mold.MoldShape == EMoldShape::Dome
			? FMoldPart{ EMoldShape::Dome, Floor, Floor + Radius, Radius }
			: FMoldPart{ Mold.MoldShape, Floor, Floor + Tall, Half });
		return;
	}

	// Two or three shapes stacked from the floor up, each no wider than the one below it, a dome only ever on top - and
	// none there either when it opens at the top.
	const EMoldShape Stackable[] = { EMoldShape::Ziggurat, EMoldShape::Cylinder, EMoldShape::Tower };
	const int32 PartCount = Stream.RandRange(2, 3);

	double Start = Floor;
	double PartHalf = Half;

	for (int32 Part = 0; Part < PartCount; ++Part)
	{
		const bool bTop = Part == PartCount - 1;
		const EMoldShape Shape = bTop && Stream.FRand() < 0.5f && !Mold.bOpenTop
			? EMoldShape::Dome
			: Stackable[Stream.RandRange(0, static_cast<int32>(UE_ARRAY_COUNT(Stackable)) - 1)];

		const double Remaining = Floor + Tall - Start;
		double PartTall = bTop ? Remaining : Remaining * Stream.FRandRange(0.3f, 0.6f);

		if (Shape == EMoldShape::Dome)
		{
			PartHalf = FMath::Min(PartHalf, PartTall);
			PartTall = PartHalf;
		}

		Mold.Parts.Add(FMoldPart{ Shape, Start, Start + PartTall, PartHalf });

		Start += PartTall;
		PartHalf = FMath::Max(PartHalf * Stream.FRandRange(0.5f, 0.9f), 0.5 * GetModule());
	}
}

FVector ARoomManager::GetPocketReach(const FAirPocket& Pocket)
{
	// Its half measures across and along its axis, along each world axis, or a sphere's radius.
	if (Pocket.Shape == EAirPocketShape::Sphere && !Pocket.bMold)
	{
		return FVector(Pocket.HalfSize.X);
	}

	FVector Reach;
	Reach[Pocket.Axis] = Pocket.HalfSize.Z;
	Reach[(Pocket.Axis + 1) % 3] = Pocket.HalfSize.X;
	Reach[(Pocket.Axis + 2) % 3] = Pocket.HalfSize.Y;
	return Reach;
}

void ARoomManager::GetPocketBounds(const FAirPocket& Pocket, FVector& OutMin, FVector& OutMax) const
{
	// Its reach, and as much again as its warp can move a cell into it. A mold's shape warp moves only what is in its box.
	const FVector Reach = GetPocketReach(Pocket) + FVector(Pocket.Warp.Reach);

	OutMin = Pocket.Centre - Reach;
	OutMax = Pocket.Centre + Reach;
}

FVector ARoomManager::FAirPocket::FWarp::Sample(const FVector& Offset) const
{
	if (Offsets.IsEmpty())
	{
		return FVector::ZeroVector;
	}

	// The node below the place along each axis, kept inside the grid, and how far on toward the next it lies.
	int32 Below[3];
	double Along[3];

	for (int32 Dimension = 0; Dimension < 3; ++Dimension)
	{
		const double Index = FMath::Clamp((Offset[Dimension] - Origin[Dimension]) / Step, 0.0, static_cast<double>(Count[Dimension] - 1));
		Below[Dimension] = FMath::Min(FMath::FloorToInt32(Index), Count[Dimension] - 2);
		Along[Dimension] = Index - Below[Dimension];
	}

	auto At = [this](const int32 X, const int32 Y, const int32 Z)
	{
		return FVector(Offsets[(Z * Count.Y + Y) * Count.X + X]);
	};

	auto Layer = [&](const int32 Z)
	{
		return FMath::Lerp(
			FMath::Lerp(At(Below[0], Below[1], Z), At(Below[0] + 1, Below[1], Z), Along[0]),
			FMath::Lerp(At(Below[0], Below[1] + 1, Z), At(Below[0] + 1, Below[1] + 1, Z), Along[0]),
			Along[1]);
	};

	return FMath::Lerp(Layer(Below[2]), Layer(Below[2] + 1), Along[2]);
}

void ARoomManager::BuildPocketWarp(FAirPocket::FWarp& Warp, const FVector& Reach, const double Amount, const double Wavelength, FRandomStream& Stream) const
{
	Warp = FAirPocket::FWarp();

	if (Amount <= 0.0 || Wavelength <= 0.0)
	{
		return;
	}

	// Nodes a quarter of the finer lumps apart - those are half as broad as the broad ones - but no closer than a module,
	// since growth sees nothing finer.
	Warp.Step = FMath::Max(Wavelength / 8.0, static_cast<double>(GetModule()));
	Warp.Reach = Amount;

	// Over every place a cell could be moved into the shape from, and a node beyond.
	const FVector Extent = Reach + FVector(Amount + Warp.Step);
	Warp.Origin = -Extent;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Warp.Count[Axis] = FMath::Max(FMath::CeilToInt32(2.0 * Extent[Axis] / Warp.Step) + 1, 2);
	}

	// A place of its own in the noise for each axis - Perlin noise repeats every 256.
	FVector Seeds[3];

	for (FVector& Seed : Seeds)
	{
		Seed = FVector(Stream.FRandRange(0.0f, 256.0f), Stream.FRandRange(0.0f, 256.0f), Stream.FRandRange(0.0f, 256.0f));
	}

	Warp.Offsets.SetNumUninitialized(Warp.Count.X * Warp.Count.Y * Warp.Count.Z);
	int32 Node = 0;

	for (int32 Z = 0; Z < Warp.Count.Z; ++Z)
	{
		for (int32 Y = 0; Y < Warp.Count.Y; ++Y)
		{
			for (int32 X = 0; X < Warp.Count.X; ++X)
			{
				const FVector Place = (Warp.Origin + FVector(FIntVector(X, Y, Z)) * Warp.Step) / Wavelength;
				FVector3f Offset;

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					// Broad lumps with finer ones half as strong on them, doubled and clipped, since Perlin noise mostly
					// keeps near nought: a typical cell moves about a third of Amount, and a few all of it.
					const FVector Broad = Place + Seeds[Axis];
					const double Noise = FMath::PerlinNoise3D(Broad) + 0.5 * FMath::PerlinNoise3D(2.13 * Broad + FVector(17.1, 5.3, 11.7));
					Offset[Axis] = static_cast<float>(Amount * FMath::Clamp(Noise * (2.0 / 1.5), -1.0, 1.0));
				}

				Warp.Offsets[Node++] = Offset;
			}
		}
	}
}

void ARoomManager::DrawPocketWarps(FAirPocket& Pocket, const int32 Index) const
{
	// From a stream of its own per draw, so the noise takes nothing from the streams shapes and places are drawn from.
	FRandomStream Stream(static_cast<int32>(HashCombineFast(
		HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(Pocket.bMold ? TEXT("MoldNoise") : TEXT("PocketNoise")))),
		GetTypeHash(Index))));

	const FVector Reach = GetPocketReach(Pocket);
	Pocket.ShapeWarp = FAirPocket::FWarp();

	if (!Pocket.bMold)
	{
		const double Half = Reach.GetMax();
		BuildPocketWarp(Pocket.Warp, Reach, FMath::Max(AirPocketNoise, 0.0f) * Half, FMath::Max(AirPocketNoiseScale, 0.1f) * 2.0 * Half, Stream);
		return;
	}

	// The box by its half width; the shape by its foot's, in lumps as broad as the box's, so a ziggurat's tiers move together.
	const double Across = FMath::Max(Pocket.HalfSize.X, Pocket.HalfSize.Y);
	const double Wavelength = FMath::Max(MoldNoiseScale, 0.1f) * 2.0 * Across;
	const double Foot = Pocket.Parts.IsEmpty() ? 0.0 : Pocket.Parts[0].Half;

	BuildPocketWarp(Pocket.Warp, Reach, FMath::Max(MoldNoise, 0.0f) * Across, Wavelength, Stream);
	BuildPocketWarp(Pocket.ShapeWarp, Reach, FMath::Max(MoldShapeNoise, 0.0f) * Foot, Wavelength, Stream);
}

FVector ARoomManager::GetWarpShift(const FAirPocket& Pocket, const FAirPocket::FWarp& Warp, const FVector& Offset)
{
	return Warp.IsEmpty() ? FVector::ZeroVector : DampWarp(Pocket, Warp.Sample(Offset), Offset);
}

FVector ARoomManager::DampWarp(const FAirPocket& Pocket, const FVector& Shift, const FVector& Offset)
{
	if (!Pocket.bMold)
	{
		return Shift;
	}

	// Along its axis none at the top of the open floor and below, and all of it at the top of the box.
	FVector Damped = Shift;
	const double Height = 2.0 * Pocket.HalfSize.Z - Pocket.OpenFloor;
	const double Rise = Offset[Pocket.Axis] * Pocket.AxisSign + Pocket.HalfSize.Z - Pocket.OpenFloor;
	Damped[Pocket.Axis] *= Height > 0.0 ? FMath::Clamp(Rise / Height, 0.0, 1.0) : 0.0;
	return Damped;
}

ARoomManager::FPocketShift ARoomManager::SamplePocketShift(const FAirPocket& Pocket, const FIntVector& Cell) const
{
	if (Pocket.Warp.IsEmpty() && Pocket.ShapeWarp.IsEmpty())
	{
		return FPocketShift();
	}

	// The middle of the module the cell lies in.
	const int32 Module = GetModule();

	auto NodeBelow = [Module](const int32 Coordinate)
	{
		return (Coordinate >= 0 ? Coordinate : Coordinate - Module + 1) / Module * Module;
	};

	const FVector Middle = FVector(FIntVector(NodeBelow(Cell.X), NodeBelow(Cell.Y), NodeBelow(Cell.Z))) + FVector(0.5 * Module) - Pocket.Centre;
	return FPocketShift{ Pocket.Warp.Sample(Middle), Pocket.ShapeWarp.Sample(Middle) };
}

FVector ARoomManager::GetUnwarpedPoint(const FAirPocket& Pocket, const FVector& ShapeOffset, const bool bInShape) const
{
	// The place a warp moves onto Target, stepped toward a few times: each step goes back by what the warp does where it
	// stands, which settles while the lumps are broader than they are tall.
	auto Undo = [&Pocket](const FAirPocket::FWarp& Warp, const FVector& Target)
	{
		FVector Point = Target;

		for (int32 Round = 0; Round < 8 && !Warp.IsEmpty(); ++Round)
		{
			Point = Target - GetWarpShift(Pocket, Warp, Point);
		}

		return Point;
	};

	const FVector InBox = bInShape ? Undo(Pocket.ShapeWarp, ShapeOffset) : ShapeOffset;
	return Pocket.Centre + Undo(Pocket.Warp, InBox);
}

bool ARoomManager::IsCellInMoldShape(const FAirPocket& Mold, const FIntVector& Cell) const
{
	const FPocketShift Shift = SamplePocketShift(Mold, Cell);
	const FVector Offset = FVector(Cell) + FVector(0.5) - Mold.Centre;
	return IsOffsetInMoldShape(Mold, Offset + DampWarp(Mold, Shift.Box, Offset), Shift.Shape);
}

bool ARoomManager::IsOffsetInMoldShape(const FAirPocket& Mold, const FVector& BoxOffset, const FVector& ShapeShift) const
{
	// The open layer across the bottom of the box, level, as the box's warp leaves it.
	const double FloorRise = BoxOffset[Mold.Axis] * Mold.AxisSign + Mold.HalfSize.Z;

	if (FloorRise >= 0.0 && FloorRise <= Mold.OpenFloor)
	{
		return true;
	}

	// However far the shapes wander, a module of the box stays held round them, and over them unless they open at the
	// top: growth reaches them only from the floor, or from above.
	const double Wall = static_cast<double>(GetModule());

	if (FMath::Abs(BoxOffset[(Mold.Axis + 1) % 3]) > Mold.HalfSize.X - Wall
		|| FMath::Abs(BoxOffset[(Mold.Axis + 2) % 3]) > Mold.HalfSize.Y - Wall
		|| (!Mold.bOpenTop && FloorRise > 2.0 * Mold.HalfSize.Z - Wall))
	{
		return false;
	}

	// The shapes, once their own warp has moved the place too: its height above the mold's floor, and how far it lies
	// off the mold's axis either way across it.
	const FVector Offset = BoxOffset + DampWarp(Mold, ShapeShift, BoxOffset);
	const double Rise = Offset[Mold.Axis] * Mold.AxisSign + Mold.HalfSize.Z;
	const double First = Offset[(Mold.Axis + 1) % 3];
	const double Second = Offset[(Mold.Axis + 2) % 3];

	for (int32 Index = 0; Index < Mold.Parts.Num(); ++Index)
	{
		const FAirPocket::FMoldPart& Part = Mold.Parts[Index];

		// Open at the top, the top part reaches on out of the box, however far its top has wandered.
		const bool bReachesOut = Mold.bOpenTop && Index == Mold.Parts.Num() - 1;

		if (Rise < Part.Start || (Rise > Part.End && !bReachesOut))
		{
			continue;
		}

		switch (Part.Shape)
		{
		case EMoldShape::Cylinder:
			if (First * First + Second * Second <= Part.Half * Part.Half)
			{
				return true;
			}
			break;

		case EMoldShape::Dome:
		{
			const double Above = Rise - Part.Start;
			if (Above * Above + First * First + Second * Second <= Part.Half * Part.Half)
			{
				return true;
			}
			break;
		}

		case EMoldShape::Ziggurat:
		{
			// Its tiers count up from the part's foot, each narrower by an even step.
			const int32 Tiers = FMath::Max(ZigguratTiers, 1);
			const double Height = FMath::Max(Part.End - Part.Start, UE_DOUBLE_SMALL_NUMBER);
			const int32 Tier = FMath::Clamp(FMath::FloorToInt32((Rise - Part.Start) / Height * Tiers), 0, Tiers - 1);
			const double TierHalf = Part.Half * (1.0 - static_cast<double>(Tier) / Tiers);

			if (FMath::Abs(First) <= TierHalf && FMath::Abs(Second) <= TierHalf)
			{
				return true;
			}
			break;
		}

		default:
			if (FMath::Abs(First) <= Part.Half && FMath::Abs(Second) <= Part.Half)
			{
				return true;
			}
			break;
		}
	}

	return false;
}

void ARoomManager::AppendWarpedLine(const FAirPocket& Pocket, const FVector& From, const FVector& To, const bool bInShape, const FLinearColor& Color, TArray<FBatchedLine>& Lines) const
{
	// Clean, one line; wandering, steps half a node of the warp long, so the lumps show.
	double Step = UE_DOUBLE_BIG_NUMBER;

	if (!Pocket.Warp.IsEmpty())
	{
		Step = FMath::Min(Step, Pocket.Warp.Step);
	}

	if (bInShape && !Pocket.ShapeWarp.IsEmpty())
	{
		Step = FMath::Min(Step, Pocket.ShapeWarp.Step);
	}

	const int32 Steps = Step < UE_DOUBLE_BIG_NUMBER ? FMath::Max(FMath::CeilToInt32(FVector::Dist(From, To) / (0.5 * Step)), 1) : 1;
	FVector Previous = CellPointToWorld(GetUnwarpedPoint(Pocket, From, bInShape));

	for (int32 Index = 1; Index <= Steps; ++Index)
	{
		const FVector Next = CellPointToWorld(GetUnwarpedPoint(Pocket, FMath::Lerp(From, To, static_cast<double>(Index) / Steps), bInShape));
		Lines.Emplace(Previous, Next, Color, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_World);
		Previous = Next;
	}
}

void ARoomManager::AppendWarpedBox(const FAirPocket& Pocket, const FVector& Min, const FVector& Max, const bool bInShape, const FLinearColor& Color, TArray<FBatchedLine>& Lines) const
{
	// Its twelve edges, four along each axis.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const int32 First = (Axis + 1) % 3;
		const int32 Second = (Axis + 2) % 3;

		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			FVector From = Min;
			From[First] = (Corner & 1) ? Max[First] : Min[First];
			From[Second] = (Corner & 2) ? Max[Second] : Min[Second];

			FVector To = From;
			To[Axis] = Max[Axis];

			AppendWarpedLine(Pocket, From, To, bInShape, Color, Lines);
		}
	}
}

void ARoomManager::AppendMoldLines(const FAirPocket& Mold, TArray<FBatchedLine>& Lines) const
{
	const FLinearColor FrameColor(MoldColor);
	const FLinearColor ShapeColor(MoldShapeColor);

	// A point in the mold's own frame - off its axis across it, and its height above its floor - from its centre. Its box
	// and floor are drawn as the box's warp moves them, its shapes as their own warp moves them too.
	auto Local = [&Mold](const double First, const double Second, const double Rise)
	{
		FVector Point = FVector::ZeroVector;
		Point[(Mold.Axis + 1) % 3] = First;
		Point[(Mold.Axis + 2) % 3] = Second;
		Point[Mold.Axis] = (Rise - Mold.HalfSize.Z) * Mold.AxisSign;
		return Point;
	};

	auto AddBox = [&](const FVector& A, const FVector& B, const bool bInShape, const FLinearColor& Color)
	{
		AppendWarpedBox(Mold, A.ComponentMin(B), A.ComponentMax(B), bInShape, Color, Lines);
	};

	auto AddSquare = [&](const double Half, const double Start, const double End)
	{
		AddBox(Local(-Half, -Half, Start), Local(Half, Half, End), /*bInShape=*/true, ShapeColor);
	};

	auto AddLine = [&](const FVector& From, const FVector& To)
	{
		AppendWarpedLine(Mold, From, To, /*bInShape=*/true, ShapeColor, Lines);
	};

	auto AddCircle = [&](const double Radius, const double Rise)
	{
		for (int32 Segment = 0; Segment < AirPocketCircleSegments; ++Segment)
		{
			const double From = 2.0 * UE_DOUBLE_PI * Segment / AirPocketCircleSegments;
			const double To = 2.0 * UE_DOUBLE_PI * (Segment + 1) / AirPocketCircleSegments;

			AddLine(Local(Radius * FMath::Cos(From), Radius * FMath::Sin(From), Rise), Local(Radius * FMath::Cos(To), Radius * FMath::Sin(To), Rise));
		}
	};

	// Its box.
	AddBox(Local(-Mold.HalfSize.X, -Mold.HalfSize.Y, 0.0), Local(Mold.HalfSize.X, Mold.HalfSize.Y, 2.0 * Mold.HalfSize.Z), /*bInShape=*/false, FrameColor);

	// The top of its open floor, across the whole box.
	if (Mold.OpenFloor > 0.0)
	{
		AddBox(Local(-Mold.HalfSize.X, -Mold.HalfSize.Y, 0.0), Local(Mold.HalfSize.X, Mold.HalfSize.Y, Mold.OpenFloor), /*bInShape=*/false, FrameColor);
	}

	for (const FAirPocket::FMoldPart& Part : Mold.Parts)
	{
		switch (Part.Shape)
		{
		case EMoldShape::Cylinder:
			AddCircle(Part.Half, Part.Start);
			AddCircle(Part.Half, Part.End);

			for (int32 Quarter = 0; Quarter < 4; ++Quarter)
			{
				const double Angle = 0.5 * UE_DOUBLE_PI * Quarter;
				const double First = Part.Half * FMath::Cos(Angle);
				const double Second = Part.Half * FMath::Sin(Angle);
				AddLine(Local(First, Second, Part.Start), Local(First, Second, Part.End));
			}
			break;

		case EMoldShape::Dome:
			// Rings up its height, and two arcs over its top.
			for (int32 Ring = 0; Ring < 4; ++Ring)
			{
				const double Angle = 0.5 * UE_DOUBLE_PI * Ring / 4;
				AddCircle(Part.Half * FMath::Cos(Angle), Part.Start + Part.Half * FMath::Sin(Angle));
			}

			for (const bool bAlongFirst : { true, false })
			{
				for (int32 Segment = 0; Segment < AirPocketCircleSegments / 2; ++Segment)
				{
					auto ArcPoint = [&](const int32 Step)
					{
						const double Angle = UE_DOUBLE_PI * Step / (AirPocketCircleSegments / 2);
						const double Across = Part.Half * FMath::Cos(Angle);
						const double Rise = Part.Start + Part.Half * FMath::Sin(Angle);
						return bAlongFirst ? Local(Across, 0.0, Rise) : Local(0.0, Across, Rise);
					};

					AddLine(ArcPoint(Segment), ArcPoint(Segment + 1));
				}
			}
			break;

		case EMoldShape::Ziggurat:
		{
			const int32 Tiers = FMath::Max(ZigguratTiers, 1);
			const double TierHeight = (Part.End - Part.Start) / Tiers;

			for (int32 Tier = 0; Tier < Tiers; ++Tier)
			{
				AddSquare(Part.Half * (1.0 - static_cast<double>(Tier) / Tiers), Part.Start + Tier * TierHeight, Part.Start + (Tier + 1) * TierHeight);
			}
			break;
		}

		default:
			AddSquare(Part.Half, Part.Start, Part.End);
			break;
		}
	}
}

bool ARoomManager::IsCellInPocket(const FAirPocket& Pocket, const FIntVector& Cell) const
{
	return IsCellInPocket(Pocket, Cell, SamplePocketShift(Pocket, Cell));
}

bool ARoomManager::IsCellInPocket(const FAirPocket& Pocket, const FIntVector& Cell, const FPocketShift& Shift) const
{
	// The cell's centre as the pocket's warp moves it, tested against the clean shape.
	FVector Offset = FVector(Cell) + FVector(0.5) - Pocket.Centre;
	Offset += DampWarp(Pocket, Shift.Box, Offset);

	const double Along = Offset[Pocket.Axis];
	const double First = Offset[(Pocket.Axis + 1) % 3];
	const double Second = Offset[(Pocket.Axis + 2) % 3];
	const FVector& Half = Pocket.HalfSize;

	// A mold holds back its whole box but the shapes it leaves open.
	if (Pocket.bMold)
	{
		return FMath::Abs(First) <= Half.X && FMath::Abs(Second) <= Half.Y && FMath::Abs(Along) <= Half.Z && !IsOffsetInMoldShape(Pocket, Offset, Shift.Shape);
	}

	switch (Pocket.Shape)
	{
	case EAirPocketShape::Sphere:
		return Offset.SizeSquared() <= Half.X * Half.X;

	case EAirPocketShape::Cylinder:
		return FMath::Abs(Along) <= Half.Z && First * First + Second * Second <= Half.X * Half.X;

	case EAirPocketShape::Ziggurat:
	{
		if (FMath::Abs(Along) > Half.Z || Half.Z <= 0.0)
		{
			return false;
		}

		// Its tiers count up from its base, on the side its top looks away from, each narrower by an even step.
		const int32 Tiers = FMath::Max(ZigguratTiers, 1);
		const double Rise = (Along * Pocket.AxisSign + Half.Z) / (2.0 * Half.Z);
		const int32 Tier = FMath::Clamp(FMath::FloorToInt32(Rise * Tiers), 0, Tiers - 1);
		const double TierHalf = Half.X * (1.0 - static_cast<double>(Tier) / Tiers);

		return FMath::Abs(First) <= TierHalf && FMath::Abs(Second) <= TierHalf;
	}

	default:
		return FMath::Abs(First) <= Half.X && FMath::Abs(Second) <= Half.Y && FMath::Abs(Along) <= Half.Z;
	}
}

void ARoomManager::CutPocketPieces(FAirPocket& Pocket) const
{
	Pocket.FaceKeys.Reset();
	Pocket.EdgeKeys.Reset();
	Pocket.NodeKeys.Reset();
	Pocket.SlabEnds.Reset();

	for (int32 Slab = GetPocketSlabCount(Pocket); Slab > 0; --Slab)
	{
		CutPocketSlab(Pocket);
	}
}

void ARoomManager::CutPocketSlab(FAirPocket& Pocket) const
{
	const int32 Module = GetModule();
	const FAirBox Slab = GetPocketSlabBox(Pocket, Pocket.SlabEnds.Num());

	// A piece is cut when any one of its cells lies inside: the inside of a module face, the run of a module edge, or the
	// node itself. An edge can be visited from both planes it lies in; it is kept once. A piece's cells all lie in the
	// module from its min node, so its warps are sampled once.
	TSet<uint64> Edges;

	// Whether cells from MinCell to MaxCell, moved by a piece's shift, could reach the clean shape's reach at all - most of
	// a noisy pocket's bounds are room for the noise, and their pieces are passed over without a cell tested. A mold
	// moves cells along its axis by anywhere between none of the shift and all of it.
	const FVector Reach = GetPocketReach(Pocket);

	auto MayReach = [&](const FIntVector& MinCell, const FIntVector& MaxCell, const FPocketShift& Shift)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Least = Pocket.bMold && Axis == Pocket.Axis ? FMath::Min(Shift.Box[Axis], 0.0) : Shift.Box[Axis];
			const double Most = Pocket.bMold && Axis == Pocket.Axis ? FMath::Max(Shift.Box[Axis], 0.0) : Shift.Box[Axis];

			if (MaxCell[Axis] + 0.5 - Pocket.Centre[Axis] + Most < -Reach[Axis] || MinCell[Axis] + 0.5 - Pocket.Centre[Axis] + Least > Reach[Axis])
			{
				return false;
			}
		}

		return true;
	};

	ForEachObstaclePiece(Slab.Min, Slab.Max, Module,
		[&](const FIntVector& FaceMinNode, const int32 NormalAxis)
		{
			int32 U, V;
			GetInPlaneAxes(NormalAxis, U, V);
			const FPocketShift Shift = SamplePocketShift(Pocket, FaceMinNode);

			if (!MayReach(FaceMinNode + AxisStep(U, 1) + AxisStep(V, 1), FaceMinNode + AxisStep(U, Module - 1) + AxisStep(V, Module - 1), Shift))
			{
				return;
			}

			for (int32 StepV = 1; StepV < Module; ++StepV)
			{
				for (int32 StepU = 1; StepU < Module; ++StepU)
				{
					if (IsCellInPocket(Pocket, FaceMinNode + AxisStep(U, StepU) + AxisStep(V, StepV), Shift))
					{
						Pocket.FaceKeys.Add(MakeModuleFaceKey(FaceMinNode, NormalAxis));
						return;
					}
				}
			}
		},
		[&](const FLatticeEdge& Edge)
		{
			const FPocketShift Shift = SamplePocketShift(Pocket, Edge.Node);

			if (!MayReach(Edge.Node + AxisStep(Edge.Axis, 1), Edge.Node + AxisStep(Edge.Axis, Module - 1), Shift))
			{
				return;
			}

			for (int32 Step = 1; Step < Module; ++Step)
			{
				if (IsCellInPocket(Pocket, Edge.Node + AxisStep(Edge.Axis, Step), Shift))
				{
					Edges.Add(MakeEdgeKey(Edge));
					return;
				}
			}
		},
		[&](const FIntVector& Node)
		{
			if (IsCellInPocket(Pocket, Node))
			{
				Pocket.NodeKeys.Add(MakeCellKey(Node));
			}
		});

	Pocket.EdgeKeys.Append(Edges.Array());
	Pocket.SlabEnds.Add(FIntVector(Pocket.FaceKeys.Num(), Pocket.EdgeKeys.Num(), Pocket.NodeKeys.Num()));
}

void ARoomManager::RerollPaths()
{
	// Every path at once, from the button: each drawn afresh at its next draw, then the lists refreshed in one go.
	for (FAirPath& Path : AirPaths)
	{
		++Path.Draw;
	}

	PathRedraws += AirPaths.Num();
	SpawnPaths();
	RebuildPathLines();
}

double ARoomManager::GetClock() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

void ARoomManager::StartClock()
{
	UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	// A tick that falls behind is dropped, as the beats are: what is due is still due at the next one.
	FTimerManagerTimerParameters Parameters;
	Parameters.bLoop = true;
	Parameters.bMaxOncePerFrame = true;

	World->GetTimerManager().SetTimer(ClockTimerHandle, this, &ARoomManager::AdvanceClock, PathClockSeconds, Parameters);
}

void ARoomManager::AdvanceClock()
{
	const double Now = GetClock();

	AdvancePaths(Now);
	AdvancePockets(Now);
	AdvanceDeletion(Now);

	// A few of the platforms with no health left at a time, so however many a strike leaves, no frame takes them all.
	RemoveDying(MaxRemovalsPerTick);
}

void ARoomManager::AdvancePaths(const double Now)
{
	// Switched off, or with no interval, the paths stay as they are and nothing is drawn or judged.
	if (!bEnablePaths || PathRerollInterval <= 0.0f)
	{
		return;
	}

	// The paths fall due one after another across the interval, so a tick draws a path or two afresh rather than
	// all of them: what a redraw costs is the placements round one path, not round every path. The longest due
	// go first; more than a tick's share wait for the next.
	for (int32 Redrawn = 0; Redrawn < MaxPathRedrawsPerTick; ++Redrawn)
	{
		int32 Due = INDEX_NONE;

		for (int32 Index = 0; Index < AirPaths.Num(); ++Index)
		{
			if (AirPaths[Index].RedrawAt <= Now && (Due == INDEX_NONE || AirPaths[Index].RedrawAt < AirPaths[Due].RedrawAt))
			{
				Due = Index;
			}
		}

		if (Due == INDEX_NONE)
		{
			break;
		}

		RedrawPath(Due);
	}
}

bool ARoomManager::CanRefreshCandidateLists() const
{
	return !bPlatformCandidatesStale && TileCandidates.Num() == UsableTiles.Num();
}

void ARoomManager::RefreshCandidatesOnPathPieces(
	const TConstArrayView<uint64> Faces,
	const TConstArrayView<uint64> Edges,
	const TConstArrayView<uint64> Nodes,
	const bool bMayHaveComeFree,
	TArray<TSet<uint64>>& Judged)
{
	const int32 Module = GetModule();

	// Only a placement meeting a platform along its rim is ever open, and the lists hold only open ones. So where a
	// path has gone, one meeting nothing is not listed and stays unlisted: it is passed over without being judged in
	// full, which is most of them, since paths run mostly through open air. The empty cube has its own rule.
	const bool bPassOverLoose = bMayHaveComeFree && !BuiltPlatforms.IsEmpty();

	auto Refresh = [&](const int32 TileIndex, const FGridPlatform& Candidate)
	{
		bool bAlreadyJudged = false;
		Judged[TileIndex].Add(MakeCandidateKey(Candidate), &bAlreadyJudged);

		if (!bAlreadyJudged && (!bPassOverLoose || MeetsStructure(Candidate)))
		{
			RefreshPlatformCandidate(TileIndex, Candidate, bMayHaveComeFree);
		}
	};

	auto RefreshAround = [&](const FLatticeEdge& Edge)
	{
		ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/false,
			[&](const int32 TileIndex, const FGridPlatform& Candidate, bool) { Refresh(TileIndex, Candidate); });
	};

	// A face is taken in only by placements in its own plane that cover it, which all take in its edge along U and
	// reach across it along V.
	for (const uint64 Key : Faces)
	{
		const FIntVector FaceMinNode = CellFromKey(Key);
		const int32 NormalAxis = static_cast<int32>(Key >> KeyTagShift) - 1;

		int32 U, V;
		GetInPlaneAxes(NormalAxis, U, V);

		ForEachPlacementAround(FLatticeEdge{ FaceMinNode, U }, UsableTiles, Module, /*bRimOnly=*/false,
			[&](const int32 TileIndex, const FGridPlatform& Candidate, bool)
		{
			int32 SpanU, SpanV;

			if (AxisIndex(Candidate.Normal) == NormalAxis
				&& GetPlatformSpans(Candidate, SpanU, SpanV)
				&& Candidate.MinNode[V] + SpanV >= FaceMinNode[V] + Module)
			{
				Refresh(TileIndex, Candidate);
			}
		});
	}

	for (const uint64 Key : Edges)
	{
		RefreshAround(FLatticeEdge{ CellFromKey(Key), static_cast<int32>(Key >> KeyTagShift) - 1 });
	}

	// A placement taking in a node takes in an edge running from it.
	for (const uint64 Key : Nodes)
	{
		const FIntVector Node = CellFromKey(Key);

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			RefreshAround(FLatticeEdge{ Node, Axis });
			RefreshAround(FLatticeEdge{ Node - AxisStep(Axis, Module), Axis });
		}
	}
}

void ARoomManager::DrawPath(const int32 Index, FAirPath& Path) const
{
	Path.Corners.Reset();
	Path.LegMins.Reset();
	Path.LegMaxes.Reset();
	Path.FaceKeys.Reset();
	Path.EdgeKeys.Reset();
	Path.NodeKeys.Reset();

	const int32 Module = GetModule();
	const int32 LastCell = ResolveGridSize() - 1;

	// The cube is whole modules and the node closing them, so this many module middles lie along each axis.
	const int32 Modules = LastCell / Module;

	if (Modules <= 0)
	{
		return;
	}

	// A stream of its own per path and draw, so one path moving leaves every other where it is.
	FRandomStream Stream(static_cast<int32>(HashCombineFast(
		HashCombineFast(GetTypeHash(RunSeed), GetTypeHash(FStringView(TEXT("Paths")))),
		HashCombineFast(GetTypeHash(Index), GetTypeHash(Path.Draw)))));

	const int32 FewestLegs = FMath::Max(MinPathLegs, 1);
	const int32 MostLegs = FMath::Max(MaxPathLegs, FewestLegs);
	const int32 Shortest = FMath::Max(MinPathLegLength, 1);
	const int32 Longest = FMath::Max(MaxPathLegLength, Shortest);
	const int32 Width = FMath::Max(PathWidth, 1);

	// A corner sits at a module's middle - between its two central cells - and a path's Width cells across
	// lie about it: two of them clear both lattice planes beside it, as anything under a module does.
	const int32 Below = Width / 2;

	auto ToCells = [Module](const FIntVector& ModuleIndex)
	{
		return ModuleIndex * Module + FIntVector(Module / 2);
	};

	// Anywhere in the cube, so paths are dispersed all over it rather than gathered round growth.
	FIntVector At(Stream.RandRange(0, Modules - 1), Stream.RandRange(0, Modules - 1), Stream.RandRange(0, Modules - 1));
	Path.Corners.Add(ToCells(At));

	const int32 Legs = Stream.RandRange(FewestLegs, MostLegs);
	int32 PreviousAxis = INDEX_NONE;

	// Legs overlap where they meet, and an edge lies in two lattice planes, so a piece is kept once.
	TSet<uint64> Faces, Edges, Nodes;

	for (int32 Leg = 0; Leg < Legs; ++Leg)
	{
		// Every corner turns: up or down with the vertical chance, unless the last leg was, else level,
		// across the last level leg.
		int32 Axis = 2;

		if (PreviousAxis == 2 || Stream.FRand() >= PathVerticalChance)
		{
			Axis = PreviousAxis == 0 ? 1 : (PreviousAxis == 1 ? 0 : Stream.RandRange(0, 1));
		}

		int32 Steps = FMath::Max(FMath::RoundToInt32(static_cast<float>(Stream.RandRange(Shortest, Longest)) / Module), 1);
		int32 Sign = Stream.RandBool() ? 1 : -1;

		// Kept inside the cube: turned round when the other way has more room, cut short at the side.
		const int32 Ahead = Sign > 0 ? Modules - 1 - At[Axis] : At[Axis];
		const int32 Behind = Sign > 0 ? At[Axis] : Modules - 1 - At[Axis];

		if (Ahead < Steps && Behind > Ahead)
		{
			Sign = -Sign;
			Steps = FMath::Min(Steps, Behind);
		}
		else
		{
			Steps = FMath::Min(Steps, Ahead);
		}

		if (Steps == 0)
		{
			continue;
		}

		const FIntVector From = ToCells(At);
		At[Axis] += Sign * Steps;
		const FIntVector To = ToCells(At);

		Path.Corners.Add(To);
		PreviousAxis = Axis;

		// The leg's ends reach as far past its corners as its sides do, so legs meet whole at a corner.
		FIntVector Min, Max;

		for (int32 Each = 0; Each < 3; ++Each)
		{
			Min[Each] = FMath::Clamp(FMath::Min(From[Each], To[Each]) - Below, 0, LastCell);
			Max[Each] = FMath::Clamp(FMath::Max(From[Each], To[Each]) - Below + Width - 1, 0, LastCell);
		}

		Path.LegMins.Add(Min);
		Path.LegMaxes.Add(Max);

		ForEachObstaclePiece(Min, Max, Module,
			[&Faces](const FIntVector& FaceMinNode, const int32 NormalAxis) { Faces.Add(MakeModuleFaceKey(FaceMinNode, NormalAxis)); },
			[&Edges](const FLatticeEdge& Edge) { Edges.Add(MakeEdgeKey(Edge)); },
			[&Nodes](const FIntVector& Node) { Nodes.Add(MakeCellKey(Node)); });
	}

	Path.FaceKeys = Faces.Array();
	Path.EdgeKeys = Edges.Array();
	Path.NodeKeys = Nodes.Array();
}

void ARoomManager::AddPathKeys(const FAirPath& Path)
{
	auto Add = [](TMap<uint64, int32>& Uses, const TConstArrayView<uint64> Keys)
	{
		for (const uint64 Key : Keys)
		{
			++Uses.FindOrAdd(Key);
		}
	};

	Add(PathFaces, Path.FaceKeys);
	Add(PathEdges, Path.EdgeKeys);
	Add(PathNodes, Path.NodeKeys);
}

void ARoomManager::RemovePathKeys(const FAirPath& Path)
{
	// A piece stays held back while another path still reaches it.
	auto Remove = [](TMap<uint64, int32>& Uses, const TConstArrayView<uint64> Keys)
	{
		for (const uint64 Key : Keys)
		{
			if (int32* Count = Uses.Find(Key); Count && --*Count <= 0)
			{
				Uses.Remove(Key);
			}
		}
	};

	Remove(PathFaces, Path.FaceKeys);
	Remove(PathEdges, Path.EdgeKeys);
	Remove(PathNodes, Path.NodeKeys);
}

void ARoomManager::FindPathStanding(FAirPath& Path) const
{
	const int32 Module = GetModule();
	TSet<int32> Owners;

	// Every platform is a module or more across, so one that takes in an edge or a node covers a module face beside
	// it in its own plane - which is all ModuleFaceOwners has to be asked, however many platforms stand.
	auto AddOwner = [&](const FIntVector& FaceMinNode, const int32 NormalAxis)
	{
		if (const int32* Owner = ModuleFaceOwners.Find(MakeModuleFaceKey(FaceMinNode, NormalAxis)))
		{
			Owners.Add(*Owner);
		}
	};

	for (const uint64 Key : Path.FaceKeys)
	{
		AddOwner(CellFromKey(Key), static_cast<int32>(Key >> KeyTagShift) - 1);
	}

	for (const uint64 Key : Path.EdgeKeys)
	{
		const FIntVector Node = CellFromKey(Key);
		const int32 Axis = static_cast<int32>(Key >> KeyTagShift) - 1;

		for (const int32 NormalAxis : { (Axis + 1) % 3, (Axis + 2) % 3 })
		{
			const int32 Across = 3 - NormalAxis - Axis;
			AddOwner(Node, NormalAxis);
			AddOwner(Node - AxisStep(Across, Module), NormalAxis);
		}
	}

	for (const uint64 Key : Path.NodeKeys)
	{
		const FIntVector Node = CellFromKey(Key);

		for (int32 NormalAxis = 0; NormalAxis < 3; ++NormalAxis)
		{
			int32 U, V;
			GetInPlaneAxes(NormalAxis, U, V);

			for (const int32 StepU : { 0, Module })
			{
				for (const int32 StepV : { 0, Module })
				{
					AddOwner(Node - AxisStep(U, StepU) - AxisStep(V, StepV), NormalAxis);
				}
			}
		}
	}

	Path.Standing = Owners.Array();
}

void ARoomManager::SpawnPaths()
{
	// Lists that are going to be gathered afresh anyway are left to that; any others are refreshed where the
	// paths were and where they now run, below.
	const bool bRefreshLists = CanRefreshCandidateLists();

	TArray<uint64> OldFaces, OldEdges, OldNodes;
	PathFaces.GenerateKeyArray(OldFaces);
	PathEdges.GenerateKeyArray(OldEdges);
	PathNodes.GenerateKeyArray(OldNodes);

	PathFaces.Reset();
	PathEdges.Reset();
	PathNodes.Reset();

	// Those already drawn keep their draw, so they come out on the same routes; any more start at their first.
	const int32 Count = bEnablePaths ? FMath::Max(PathCount, 0) : 0;
	AirPaths.SetNum(Count);

	const double Now = GetClock();
	const double Interval = FMath::Max(PathRerollInterval, 0.0f);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		FAirPath& Path = AirPaths[Index];
		DrawPath(Index, Path);
		AddPathKeys(Path);

		// Due one after another across the interval from now, so they never all move in one frame.
		Path.RedrawAt = Now + Interval * (static_cast<double>(Index) + FMath::FRand()) / Count;
	}

	// Where the old paths ran, placements may have come free; where the new ones run, listed ones drop out. Gathering
	// every list afresh instead stalls the beat for a second once a few thousand platforms stand.
	if (bRefreshLists)
	{
		TArray<uint64> NewFaces, NewEdges, NewNodes;
		PathFaces.GenerateKeyArray(NewFaces);
		PathEdges.GenerateKeyArray(NewEdges);
		PathNodes.GenerateKeyArray(NewNodes);

		TArray<TSet<uint64>> Judged;
		Judged.SetNum(TileCandidates.Num());

		RefreshCandidatesOnPathPieces(OldFaces, OldEdges, OldNodes, /*bMayHaveComeFree=*/true, Judged);
		RefreshCandidatesOnPathPieces(NewFaces, NewEdges, NewNodes, /*bMayHaveComeFree=*/false, Judged);
	}
	else
	{
		bPlatformCandidatesStale = true;
	}

	for (FAirPath& Path : AirPaths)
	{
		FindPathStanding(Path);
	}

	UpdatePathSummary();
}

void ARoomManager::RedrawPath(const int32 Index)
{
	if (!AirPaths.IsValidIndex(Index))
	{
		return;
	}

	const bool bRefreshLists = CanRefreshCandidateLists();
	FAirPath& Path = AirPaths[Index];

	RemovePathKeys(Path);

	const TArray<uint64> OldFaces = MoveTemp(Path.FaceKeys);
	const TArray<uint64> OldEdges = MoveTemp(Path.EdgeKeys);
	const TArray<uint64> OldNodes = MoveTemp(Path.NodeKeys);

	++Path.Draw;
	++PathRedraws;
	DrawPath(Index, Path);
	AddPathKeys(Path);

	// Only what this one path left and now reaches is judged again. A piece another path still holds is judged
	// too, and simply stays held back.
	if (bRefreshLists)
	{
		TArray<TSet<uint64>> Judged;
		Judged.SetNum(TileCandidates.Num());

		RefreshCandidatesOnPathPieces(OldFaces, OldEdges, OldNodes, /*bMayHaveComeFree=*/true, Judged);
		RefreshCandidatesOnPathPieces(Path.FaceKeys, Path.EdgeKeys, Path.NodeKeys, /*bMayHaveComeFree=*/false, Judged);
	}
	else
	{
		bPlatformCandidatesStale = true;
	}

	FindPathStanding(Path);

	// Only when it next moves is left to chance, not where it goes, so the wait comes from no stream of the run's.
	const float Jitter = FMath::Clamp(PathRerollJitter, 0.0f, 1.0f);
	Path.RedrawAt = GetClock() + FMath::Max(PathRerollInterval, 0.0f) * (1.0f + FMath::FRandRange(-Jitter, Jitter));

	UpdatePathSummary();
	RebuildPathLines();
}

void ARoomManager::UpdatePathSummary()
{
	if (!bEnablePaths)
	{
		PathSummary = TEXT("Paths off");
		return;
	}

	int32 Legs = 0;
	TSet<int32> Standing;

	for (const FAirPath& Path : AirPaths)
	{
		Legs += Path.LegMins.Num();
		Standing.Append(Path.Standing);
	}

	PathSummary = FString::Printf(TEXT("%d paths, %d legs, %d redraws; %d pieces held back: %d faces, %d edges, %d nodes; %d platforms stand in them"),
		AirPaths.Num(), Legs, PathRedraws, PathFaces.Num() + PathEdges.Num() + PathNodes.Num(),
		PathFaces.Num(), PathEdges.Num(), PathNodes.Num(), Standing.Num());
}

void ARoomManager::RebuildPathLines()
{
	if (!PathLineBatcher)
	{
		return;
	}

	PathLineBatcher->Flush();

	if (!bShowPaths || AirPaths.IsEmpty())
	{
		return;
	}

	// Each leg of every path as the box of cells it fills.
	const FLinearColor Color(PathColor);
	TArray<FBatchedLine> Lines;

	for (const FAirPath& Path : AirPaths)
	{
		for (int32 Leg = 0; Leg < Path.LegMins.Num(); ++Leg)
		{
			AppendBoxLines(Lines, GetCellMinCorner(Path.LegMins[Leg]), GetCellMinCorner(Path.LegMaxes[Leg]) + FVector(CellSize),
				Color, GridLineThickness, SDPG_World, /*bWithDiagonals=*/false);
		}
	}

	PathLineBatcher->DrawLines(Lines);
}

FVector ARoomManager::CellPointToWorld(const FVector& Point) const
{
	const FVector Origin = GetActorLocation();
	return FVector(Origin.X + Point.X * CellSize, Origin.Y + Point.Y * CellSize, GridBaseZ + Point.Z * CellSize);
}

FLinearColor ARoomManager::GetAirPocketColor(const EAirPocketShape Shape) const
{
	switch (Shape)
	{
	case EAirPocketShape::Sphere: return FLinearColor(SphereColor);
	case EAirPocketShape::Box: return FLinearColor(BoxColor);
	case EAirPocketShape::Rectangular: return FLinearColor(RectangularColor);
	case EAirPocketShape::Ziggurat: return FLinearColor(ZigguratColor);
	default: return FLinearColor(CylinderColor);
	}
}

void ARoomManager::AppendAirPocketLines(const FAirPocket& Pocket, TArray<FBatchedLine>& Lines) const
{
	const FLinearColor Color = GetAirPocketColor(Pocket.Shape);

	const int32 AlongAxis = Pocket.Axis;
	const int32 FirstAxis = (Pocket.Axis + 1) % 3;
	const int32 SecondAxis = (Pocket.Axis + 2) % 3;

	// Every point from the pocket's centre, in its clean shape, drawn where its warp has moved it.

	// A box in the pocket's own axes - half across its first and second axes, from Start to End along its axis.
	auto AddBox = [&](const double HalfFirst, const double HalfSecond, const double Start, const double End)
	{
		FVector Min = FVector::ZeroVector;
		FVector Max = FVector::ZeroVector;
		Min[FirstAxis] = -HalfFirst;
		Max[FirstAxis] = HalfFirst;
		Min[SecondAxis] = -HalfSecond;
		Max[SecondAxis] = HalfSecond;
		Min[AlongAxis] = FMath::Min(Start, End);
		Max[AlongAxis] = FMath::Max(Start, End);

		AppendWarpedBox(Pocket, Min, Max, /*bInShape=*/false, Color, Lines);
	};

	// A circle round the pocket's centre, Offset along one axis, in the plane of the other two.
	auto AddCircle = [&](const int32 NormalAxis, const double Offset, const double Radius)
	{
		const int32 U = (NormalAxis + 1) % 3;
		const int32 V = (NormalAxis + 2) % 3;

		auto PointAt = [&](const int32 Segment)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * static_cast<double>(Segment) / AirPocketCircleSegments;
			FVector Point = FVector::ZeroVector;
			Point[NormalAxis] = Offset;
			Point[U] = Radius * FMath::Cos(Angle);
			Point[V] = Radius * FMath::Sin(Angle);
			return Point;
		};

		for (int32 Segment = 0; Segment < AirPocketCircleSegments; ++Segment)
		{
			AppendWarpedLine(Pocket, PointAt(Segment), PointAt(Segment + 1), /*bInShape=*/false, Color, Lines);
		}
	};

	const FVector& Half = Pocket.HalfSize;

	switch (Pocket.Shape)
	{
	case EAirPocketShape::Sphere:
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			AddCircle(Axis, 0.0, Half.X);
		}
		break;

	case EAirPocketShape::Cylinder:
	{
		AddCircle(AlongAxis, -Half.Z, Half.X);
		AddCircle(AlongAxis, Half.Z, Half.X);

		// And four lines along it, joining the two.
		for (int32 Quarter = 0; Quarter < 4; ++Quarter)
		{
			const double Angle = 0.5 * UE_DOUBLE_PI * Quarter;
			FVector Start = FVector::ZeroVector;
			Start[FirstAxis] = Half.X * FMath::Cos(Angle);
			Start[SecondAxis] = Half.X * FMath::Sin(Angle);

			FVector End = Start;
			Start[AlongAxis] = -Half.Z;
			End[AlongAxis] = Half.Z;

			AppendWarpedLine(Pocket, Start, End, /*bInShape=*/false, Color, Lines);
		}
		break;
	}

	case EAirPocketShape::Ziggurat:
	{
		// A box per tier, from its base on the side its top looks away from.
		const int32 Tiers = FMath::Max(ZigguratTiers, 1);
		const double TierHeight = 2.0 * Half.Z / Tiers;

		for (int32 Tier = 0; Tier < Tiers; ++Tier)
		{
			const double TierHalf = Half.X * (1.0 - static_cast<double>(Tier) / Tiers);
			const double Start = Pocket.AxisSign * (-Half.Z + Tier * TierHeight);
			AddBox(TierHalf, TierHalf, Start, Start + Pocket.AxisSign * TierHeight);
		}
		break;
	}

	default:
		AddBox(Half.X, Half.Y, -Half.Z, Half.Z);
		break;
	}
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

		// Nature covered by a city structure offers the cover's categories too, so a field tends to stay one of them.
		if (Platform.NatureCover && Fill.Structure && Fill.Structure->IsNature())
		{
			TArray<int32, TInlineAllocator<4>> CoverCategories;
			GetCategoryIndices(Platform.NatureCover, CoverCategories);

			for (const int32 Category : CoverCategories)
			{
				Fill.Categories.AddUnique(Category);
			}
		}

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
	// What a one-sided structure on the front asks behind it, and what those on the back can be there.
	EStructureType FrontAsks = EStructureType::None;
	EStructureType BackOffers = EStructureType::None;

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

		if (WEMStructures::FitsFace(*Structure, NormalAxis, 1, Up, SpanFirst, SpanSecond, bTurned))
		{
			FrontAsks |= WEMStructures::GetBackTypes(WEMStructures::GetTypesOnFace(*Structure, NormalAxis, 1, Up));
		}

		if (WEMStructures::FitsFace(*Structure, NormalAxis, -1, Up, SpanFirst, SpanSecond, bTurned))
		{
			BackOffers |= WEMStructures::GetTypesOnFace(*Structure, NormalAxis, -1, Up);
		}
	}

	// Pairing back to back goes both ways, so some front pairs with some back whenever this holds.
	return EnumHasAnyFlags(FrontAsks, BackOffers);
}

uint8 ARoomManager::GetFillableWays(const int32 TileIndex, const EPlatformKind Kind) const
{
	const int32 Index = TileIndex * PlatformKindCount + KindIndex(Kind);

	return FillableWays.IsValidIndex(Index) ? FillableWays[Index] : 0;
}

void ARoomManager::GatherFillOptions(
	const FGridPlatform& Platform,
	const int32 FaceSign,
	const UPlatformStructureData* Front,
	const bool bLoadBearingOnly,
	TArray<UPlatformStructureData*>& OutOptions,
	const bool bExteriorBackOnly) const
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

	// The face behind the front takes one-sided structures that pair with it.
	if (Front)
	{
		for (UPlatformStructureData* Structure : UsableStructures)
		{
			if (!Structure->bTwoSided && Fits(Structure, FaceSign)
				&& WEMStructures::PairsBackToBack(*Front, *Structure, NormalAxis, -FaceSign, Platform.Up))
			{
				OutOptions.Add(Structure);
			}
		}

		return;
	}

	// A one-sided structure leaves the face behind it to another, so it is only offered when one that
	// pairs with it could fill that face - an exterior, where that face has to be one.
	EStructureType BackOffers = EStructureType::None;

	for (const UPlatformStructureData* Structure : UsableStructures)
	{
		if (!Structure->bTwoSided && Fits(Structure, -FaceSign) && (!bExteriorBackOnly || Structure->StandsFor(EStructureSide::Exterior)))
		{
			BackOffers |= WEMStructures::GetTypesOnFace(*Structure, NormalAxis, -FaceSign, Platform.Up);
		}
	}

	for (UPlatformStructureData* Structure : UsableStructures)
	{
		const bool bOffered = Structure->bTwoSided
			|| EnumHasAnyFlags(WEMStructures::GetBackTypes(WEMStructures::GetTypesOnFace(*Structure, NormalAxis, FaceSign, Platform.Up)), BackOffers);

		if (bOffered && Fits(Structure, FaceSign))
		{
			OutOptions.Add(Structure);
		}
	}
}

bool ARoomManager::DrawFill(FGridPlatform& Platform, const bool bLoadBearingOnly, const FFaceWish* Wish)
{
	Platform.PositiveStructure = nullptr;
	Platform.NegativeStructure = nullptr;
	Platform.NatureCover = nullptr;

	// The city meets nature only with exteriors, and only ground: a face meeting ground is drawn from the
	// exteriors - those of both sides among them - and a placement meeting water or a cliff is not filled.
	uint8 FacesOnGround = 0;

	if (NaturePlatformCount > 0 && MeetsClosedNature(Platform, &FacesOnGround))
	{
		return false;
	}

	auto KeepExteriorsOnGround = [FacesOnGround](TArray<UPlatformStructureData*>& Options, const int32 Sign)
	{
		Options.RemoveAll([FacesOnGround, Sign](const UPlatformStructureData* Option)
		{
			const bool bOnGround = (FacesOnGround & FaceBit(Sign)) != 0 || (Option->bTwoSided && (FacesOnGround & FaceBit(-Sign)) != 0);
			return bOnGround && !Option->StandsFor(EStructureSide::Exterior);
		});
	};

	// A wall's face over an interior ground it stands on is drawn from the interiors, with the rule's chance.
	const uint8 OverInteriorGround = bWallsFaceInteriorGrounds ? GetFacesOverInteriorGround(Platform) : 0;

	auto KeepInteriorOverGround = [&](TArray<UPlatformStructureData*>& Options, const int32 Sign)
	{
		if ((OverInteriorGround & FaceBit(Sign)) == 0 || StructureStream.FRand() >= WallsFaceInteriorGroundsChance)
		{
			return false;
		}

		KeepSide(Options, EStructureSide::Interior);
		return true;
	};

	// A face a patch or a run asks for is filled first. Otherwise a floor is filled from the face looking along its Up,
	// its roof or the ground it is walked on. A wall over an interior ground on one side only is filled from that side,
	// so what goes behind it follows from it; any other wall from either face, evenly.
	int32 FirstSign = 1;

	if (Wish)
	{
		FirstSign = Wish->Sign;
	}
	else if (!Platform.IsWall())
	{
		FirstSign = FaceSign(Platform.Up);
	}
	else if (OverInteriorGround == PositiveFaceBit || OverInteriorGround == NegativeFaceBit)
	{
		FirstSign = OverInteriorGround == PositiveFaceBit ? 1 : -1;
	}
	else
	{
		FirstSign = StructureStream.FRand() < 0.5f ? 1 : -1;
	}

	TArray<UPlatformStructureData*> Options;
	GatherFillOptions(Platform, FirstSign, /*Front=*/nullptr, bLoadBearingOnly, Options, /*bExteriorBackOnly=*/(FacesOnGround & FaceBit(-FirstSign)) != 0);
	KeepExteriorsOnGround(Options, FirstSign);
	KeepInteriorOverGround(Options, FirstSign);

	// A patch of ground tops each floor it lays with a ground of its side, and a run of walls faces the ground it runs
	// round with a wall of its side, and nothing else.
	if (Wish)
	{
		const int32 NormalAxis = AxisIndex(Platform.Normal);

		Options.RemoveAll([&](const UPlatformStructureData* Option)
		{
			return Option->bFence || !Option->StandsFor(Wish->Side)
				|| !EnumHasAnyFlags(WEMStructures::GetTypesOnFace(*Option, NormalAxis, FirstSign, Platform.Up), Wish->Type);
		});
	}

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

	GatherFillOptions(Platform, -FirstSign, First, bLoadBearingOnly, Options);
	KeepExteriorsOnGround(Options, -FirstSign);

	// Over an interior ground the back is an inside too. Otherwise, behind an inside, with
	// InteriorBacksExteriorChance, an outside - where any of the options is one. Behind an outside,
	// anything.
	if (!KeepInteriorOverGround(Options, -FirstSign)
		&& bInteriorBacksExterior && IsInterior(First) && StructureStream.FRand() < InteriorBacksExteriorChance)
	{
		KeepSide(Options, EStructureSide::Exterior);
	}

	UPlatformStructureData* Second = DrawStructure(Options, Platform, -FirstSign);

	if (!Second)
	{
		return false;
	}

	Platform.PositiveStructure = FirstSign > 0 ? First : Second;
	Platform.NegativeStructure = FirstSign > 0 ? Second : First;
	return true;
}

uint8 ARoomManager::GetFacesOverInteriorGround(const FGridPlatform& Wall) const
{
	int32 SpanU, SpanV;
	if (!Wall.IsWall() || !GetPlatformSpans(Wall, SpanU, SpanV))
	{
		return 0;
	}

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Wall.Normal);

	int32 U, V;
	GetInPlaneAxes(NormalAxis, U, V);

	// Whether a module face is one of the wall's own, which is not recorded yet.
	auto IsOwnFace = [&](const FModuleFace& Face)
	{
		return Face.Normal == NormalAxis
			&& Face.MinNode[NormalAxis] == Wall.MinNode[NormalAxis]
			&& Face.MinNode[U] >= Wall.MinNode[U] && Face.MinNode[U] < Wall.MinNode[U] + SpanU
			&& Face.MinNode[V] >= Wall.MinNode[V] && Face.MinNode[V] < Wall.MinNode[V] + SpanV;
	};

	uint8 Faces = 0;

	ForEachModuleEdge(Wall, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		// Only its rim can be shared, and only where a platform already runs around the same edge.
		if (!bRim || !ModuleEdgeUses.Contains(MakeEdgeKey(Edge)))
		{
			return;
		}

		FModuleFace EdgeFaces[4];
		GetEdgeFaces(Edge, Module, EdgeFaces);

		int32 Own = INDEX_NONE;

		for (int32 Turn = 0; Turn < 4 && Own == INDEX_NONE; ++Turn)
		{
			Own = IsOwnFace(EdgeFaces[TurningOrder[Turn]]) ? Turn : INDEX_NONE;
		}

		if (Own == INDEX_NONE)
		{
			return;
		}

		// A floor a quarter turn round the edge either way meets the wall in a corner. Into it the wall
		// looks toward the floor's direction, and the floor toward the wall's.
		for (const int32 Step : { 1, 3 })
		{
			const int32 Turn = (Own + Step) % 4;
			const FModuleFace& Face = EdgeFaces[TurningOrder[Turn]];
			const int32* Owner = ModuleFaceOwners.Find(MakeModuleFaceKey(Face.MinNode, Face.Normal));

			if (!Owner || BuiltPlatforms[*Owner].IsWall())
			{
				continue;
			}

			const FGridPlatform& Floor = BuiltPlatforms[*Owner];
			const int32 FloorSign = TurnSign(Own);

			// Its face looking at the wall has to be the Ground it is walked on, and an interior.
			if (FloorSign == FaceSign(Floor.Up) && IsInterior(FloorSign > 0 ? Floor.PositiveStructure : Floor.NegativeStructure))
			{
				Faces |= FaceBit(TurnSign(Turn));
			}
		}
	});

	return Faces;
}

UPlatformStructureData* ARoomManager::DrawStructure(
	const TConstArrayView<UPlatformStructureData*> Options,
	const FGridPlatform& Platform,
	const int32 FaceSign)
{
	return DrawStructureFrom(StructureStream, Options, Platform, FaceSign);
}

UPlatformStructureData* ARoomManager::DrawStructureFrom(
	FRandomStream& Stream,
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

	const int32 Chosen = WEMStructures::DrawWithPull(Stream, DrawOptions, Offers);

	return Chosen == INDEX_NONE ? nullptr : Options[Chosen];
}

void ARoomManager::RebuildPlatformState()
{
	// The tiles first, since which of them fit the lattice decides which platforms do.
	ResolveUsableTiles();

	// And the obstacles, which every placement below is held against as well. The air nature keeps open
	// follows from the platforms, so it is worked out again once they are all in.
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
	bHasRemovals = false;
	PromotedFaces.Reset();
	RimEdgeUsers.Reset();
	BlockedEdges.Reset();
	FaceFills.Reset();
	PatchParents.Reset();
	PatchSizes.Reset();
	PatchSizeSquares = 0;
	CategoryPatchSize = 0.0;
	NaturePlatformCount = 0;
	GroundCount = 0;
	CliffCount = 0;
	WaterCount = 0;

	for (TArray<int32>& Indices : NatureIndices)
	{
		Indices.Reset();
	}

	// The candidates follow from everything below, so they are gathered afresh on the next draw.
	bPlatformCandidatesStale = true;

	// Only the platforms that fit the lattice as it stands now, each on room of its own. One laid
	// from a tile that has since changed size, under a different module or in a larger cube may no
	// longer fit, or may now run into one listed before it. It is left out rather than drawn
	// across the grid - but kept in the list, so changing things back brings it back.
	int32 SkippedPlatforms = 0;

	for (const FGridPlatform& Platform : Platforms)
	{
		// A platform standing on an edge that a platform listed before it blocks is kept: it stood there before the
		// block came about - once what the blocker grew from was taken away - and RecordPlatform lifts it.
		if (!IsPlatformOnGrid(Platform) || !IsSpaceFree(Platform, /*bIgnoreBlockedEdges=*/true))
		{
			++SkippedPlatforms;
			continue;
		}

		RecordPlatform(Platform);
	}

	// The air the fields keep open, from the fields that stand, and health for any platform that has none yet. Neither
	// holds back what stands, so the order they come in leaves the platforms as they are. The air starts from nothing,
	// since the cube its buckets were cut for may have changed.
	AirBoxes.Reset();
	AirBoxBuckets.Reset();
	UpdateFieldAir(/*bRefreshLists=*/false);

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		if (!PlatformHealth.Contains(MakePlatformKey(Platform)))
		{
			GiveHealth(Platform);
		}
	}

	// The ground tiles are kept by key, like health, and a zone left without one takes its first.
	ReconcileGroundTiles();
	UpdateZoneSummary();

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

	// Promoted surfaces this platform builds against. Growth held to the rules folds off no promoted run, but it can
	// meet one at a node, and a platform standing where one was taken away can build against what has opened since.
	TArray<FGridSurfaceRef> Demoted;

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

			// A rim cell can carry other platforms' faces too, along the other axes, and its beam or post may only now
			// be built - one every platform round it had blocked, now ending a beam of this one - which is when those
			// faces first become surfaces at all.
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (Axis == NormalAxis)
				{
					continue;
				}

				for (const int32 Sign : { 1, -1 })
				{
					OutChangedSurfaces.Add(MakeSurfaceRef(Cell, MakeFace(Axis, Sign)));
					PromotionQueue.Add(MakeSurfaceRef(Cell, MakeFace(Axis, Sign)));
				}
			}
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

					if (PromotedFaces.Remove(MakeFaceKey(Neighbour, MakeFace(Axis, -Sign))) > 0)
					{
						Demoted.Add(MakeSurfaceRef(Neighbour, MakeFace(Axis, -Sign)));
					}
				}
			}
		}
	});

	// A surface built against is promoted no more, and nor is whatever promotion hung on it: every promoted surface
	// joined to it, in its own plane and on its own side, is let go, and worked out again below with the rest wherever
	// the rule still holds.
	for (int32 Index = 0; Index < Demoted.Num(); ++Index)
	{
		const FGridSurfaceRef Surface = Demoted[Index];

		for (int32 InPlaneAxis = 0; InPlaneAxis < 3; ++InPlaneAxis)
		{
			if (InPlaneAxis == FaceAxis(Surface.Face))
			{
				continue;
			}

			for (const int32 Sign : { 1, -1 })
			{
				const FIntVector Beside = Surface.Cell + AxisStep(InPlaneAxis, Sign);

				if (PromotedFaces.Remove(MakeFaceKey(Beside, Surface.Face)) > 0)
				{
					Demoted.Add(MakeSurfaceRef(Beside, Surface.Face));
				}
			}
		}
	}

	OutChangedSurfaces.Append(Demoted);
	PromotionQueue.Append(Demoted);

	// The same rule the rebuild sweeps to a fixpoint, worked outward from this platform instead.
	// Otherwise a surface once promoted stays promoted as the structure grows - that is what lets the
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

	// Ground and water keep the air above them open from the moment they are laid, and a piece can grow its landscape's
	// mushroom, so the air is worked out again - the lists with it, unless they are about to be gathered afresh.
	if (IsFieldPiece(Platform))
	{
		UpdateFieldAir(/*bRefreshLists=*/!bPlatformCandidatesStale);
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

	// A run let go and not promoted again offers a fold once more, so a placement folding off it may have come free.
	TSet<uint64> OpenedRunKeys;

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

	for (const FGridSurfaceRef& Surface : Demoted)
	{
		FLatticeEdge Edge;
		if (!PromotedFaces.Contains(MakeFaceKey(Surface.Cell, Surface.Face)) && GetRunEdge(Surface.Cell, Module, Edge))
		{
			AddChangedEdge(Edge);
			OpenedRunKeys.Add(MakeEdgeKey(Edge));
		}
	}

	// Everything else a placement asks for only ever closes as the structure grows, so a placement
	// not yet listed can only have come free by coming to share a module edge of rim with this
	// platform, or by folding off a run let go - and only those are judged, along with every listed
	// one. This platform's own placement is among the listed, and drops out here too.
	TArray<TSet<uint64>> Judged;
	Judged.SetNum(TileCandidates.Num());

	for (const FLatticeEdge& Edge : ChangedEdges)
	{
		const uint64 EdgeKey = MakeEdgeKey(Edge);
		const bool bMayOpen = NewRimEdgeKeys.Contains(EdgeKey) || OpenedRunKeys.Contains(EdgeKey);

		ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/false,
			[&](const int32 TileIndex, const FGridPlatform& Candidate, const bool bOnRim)
		{
			const uint64 Key = MakeCandidateKey(Candidate);

			if (!Judged[TileIndex].Contains(Key) && RefreshPlatformCandidate(TileIndex, Candidate, bMayOpen && bOnRim))
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

		// A floor or ceiling blocks a beam only while it is the one platform using it. While growth only adds, the only
		// platform ever to take a blocked beam in after it is the next panel of its own fence run, which leaves it
		// blocked; anything else standing there - as a platform can, once what the blocker grew from has been taken
		// away - lifts the block. A wall's block on its top holds whatever uses the beam.
		if (bRim)
		{
			if (const int32* Blocker = BlockedEdges.Find(EdgeKey))
			{
				const FGridPlatform& Blocking = BuiltPlatforms[*Blocker];

				if (*Blocker != PlatformIndex && !Blocking.IsWall() && !IsRunContinuation(Platform, Blocking))
				{
					BlockedEdges.Remove(EdgeKey);
				}
			}
		}

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

	int32 NatureSign = 0;
	ENatureForm Form = ENatureForm::Ground;

	if (GetNatureFace(Platform, NatureSign, Form))
	{
		++NaturePlatformCount;
		NatureIndices[static_cast<int32>(Form)].Add(PlatformIndex);

		GroundCount = NatureIndices[static_cast<int32>(ENatureForm::Ground)].Num();
		CliffCount = NatureIndices[static_cast<int32>(ENatureForm::Cliff)].Num();
		WaterCount = NatureIndices[static_cast<int32>(ENatureForm::Water)].Num();
	}
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
	const TArray<FAirBox> IncrementalAirBoxes = AirBoxes;
	const bool bHadRemovals = bHasRemovals;
	const TMap<uint64, int32> IncrementalGroundTiles = GroundTiles;

	// Every platform links to a ground tile, when it has to.
	TArray<int32> Loose;

	if (bLinkToGroundTiles)
	{
		GatherAllLoose(Loose);
	}

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

		// A removal leaves the streaks and patches as they were, so after one only what fills each face is asked.
		DifferentFills += Incremental.Structure == Rebuilt.Structure
			&& Incremental.Categories == Rebuilt.Categories
			&& (bHadRemovals || (Incremental.Streaks == Rebuilt.Streaks && Incremental.PatchNodes == Rebuilt.PatchNodes)) ? 0 : 1;
	}

	if (DifferentFills > 0)
	{
		Mismatches.Add(FString::Printf(TEXT("face fills: %d differ"), DifferentFills));
	}

	if (!bHadRemovals && IncrementalPatchLabels != GetPatchLabels())
	{
		Mismatches.Add(TEXT("category patches differ"));
	}

	// The air over the fields, box for box, whatever order the changes came in.
	{
		TMap<FAirBox, int32> Counts;

		for (const FAirBox& Box : IncrementalAirBoxes)
		{
			++Counts.FindOrAdd(Box);
		}

		int32 DifferentBoxes = 0;

		for (const FAirBox& Box : AirBoxes)
		{
			int32* Count = Counts.Find(Box);
			DifferentBoxes += Count && (*Count)-- > 0 ? 0 : 1;
		}

		for (const TPair<FAirBox, int32>& Left : Counts)
		{
			DifferentBoxes += FMath::Max(Left.Value, 0);
		}

		if (DifferentBoxes > 0)
		{
			Mismatches.Add(FString::Printf(TEXT("air over the fields: %d boxes differ"), DifferentBoxes));
		}
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

	// The rebuild keeps every ground tile and adds none: no zone with platforms is ever left without one.
	if (!GroundTiles.OrderIndependentCompareEqual(IncrementalGroundTiles))
	{
		Mismatches.Add(FString::Printf(TEXT("ground tiles: %d kept, %d rebuilt"), IncrementalGroundTiles.Num(), GroundTiles.Num()));
	}

	if (!Loose.IsEmpty())
	{
		Mismatches.Add(FString::Printf(TEXT("%d platform(s) with no link to a ground tile"), Loose.Num()));
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
	// Nature holds up whatever grows off it, whatever stands behind it.
	if (IsNatureSegment(Platform))
	{
		return true;
	}

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

	// The air held back - pockets, molds, paths and the air over the fields - is kept apart, since what stands in it stays.
	for (const FGridObstacle& Obstacle : Obstacles)
	{
		AddObstacleKeys(Obstacle.MinCell, Obstacle.MaxCell);
	}
}

void ARoomManager::AddObstacleKeys(const FIntVector& MinCell, const FIntVector& MaxCell)
{
	ForEachObstaclePiece(MinCell, MaxCell, GetModule(),
		[this](const FIntVector& FaceMinNode, const int32 NormalAxis) { ObstacleFaces.Add(MakeModuleFaceKey(FaceMinNode, NormalAxis)); },
		[this](const FLatticeEdge& Edge) { ObstacleEdges.Add(MakeEdgeKey(Edge)); },
		[this](const FIntVector& Node) { ObstacleNodes.Add(MakeCellKey(Node)); });
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

	// What its fill makes the face: those of its structure's types there that pair with the structure behind it.
	const int32 ProbeSign = FaceSign(DebugProbeSurface.Face);
	const int32 OwnerNormal = AxisIndex(OwnerPlatform.Normal);
	const UPlatformStructureData* Behind = ProbeSign > 0 ? OwnerPlatform.NegativeStructure : OwnerPlatform.PositiveStructure;
	EStructureType FaceRole = EStructureType::None;

	if (DebugProbeStructure && Behind)
	{
		// Nature behind it pairs with whatever was laid there, so only the face's own types are asked.
		FaceRole = WEMStructures::GetTypesOnFace(*DebugProbeStructure, OwnerNormal, ProbeSign, OwnerPlatform.Up)
			& (Behind->IsNature() ? WEMStructures::GetFaceTypes(OwnerNormal, ProbeSign, OwnerPlatform.Up)
				: WEMStructures::GetBackTypes(WEMStructures::GetTypesOnFace(*Behind, OwnerNormal, -ProbeSign, OwnerPlatform.Up)));
	}

	TArray<FString> RoleNames;
	for (const TPair<EStructureType, const TCHAR*>& Name : { MakeTuple(EStructureType::Ground, TEXT("ground")), MakeTuple(EStructureType::Ceiling, TEXT("ceiling")),
		MakeTuple(EStructureType::Wall, TEXT("wall")), MakeTuple(EStructureType::Roof, TEXT("roof")) })
	{
		if (EnumHasAnyFlags(FaceRole, Name.Key))
		{
			RoleNames.Add(Name.Value);
		}
	}

	const EStructureSide ProbeSide = DebugProbeStructure ? DebugProbeStructure->Side : EStructureSide::Exterior;
	const FString UpName = StaticEnum<EGridFace>()->GetDisplayNameTextByValue(static_cast<int64>(OwnerPlatform.Up)).ToString();

	// Nature is what its form is, on whichever face it was laid, and says what covers it.
	if (DebugProbeStructure && DebugProbeStructure->IsNature())
	{
		DebugProbeRole = FString::Printf(TEXT("%s, nature, up %s%s"),
			*StaticEnum<ENatureForm>()->GetDisplayNameTextByValue(static_cast<int64>(DebugProbeStructure->NatureForm)).ToString().ToLower(), *UpName,
			OwnerPlatform.NatureCover ? *FString::Printf(TEXT(", covered with %s"), *OwnerPlatform.NatureCover->GetName()) : TEXT(""));
	}
	else
	{
		DebugProbeRole = FString::Printf(TEXT("%s, %s, up %s"),
			RoleNames.IsEmpty() ? TEXT("none") : *FString::Join(RoleNames, TEXT(" or ")),
			ProbeSide == EStructureSide::Interior ? TEXT("interior") : ProbeSide == EStructureSide::Both ? TEXT("interior or exterior") : TEXT("exterior"),
			*UpName);
	}

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
	RebuildAirLines();
	RebuildPathLines();
}

void ARoomManager::UpdateVisualsAfterPlacement(
	const FGridPlatform& Platform,
	const TConstArrayView<FGridSurfaceRef> ChangedSurfaces)
{
	AppendPlatformPieces(Platform);

	// Water laid beside water makes a seam of the beam between them, and anything standing on that beam ends it. Either
	// can happen on any beam or post the platform takes in, inside it as well as round it.
	if (!NatureIndices[static_cast<int32>(ENatureForm::Water)].IsEmpty())
	{
		int32 SpanU, SpanV;
		GetPlatformSpans(Platform, SpanU, SpanV);

		const int32 Module = GetModule();
		TSet<uint64> EdgeKeys;
		TSet<uint64> NodeKeys;

		ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&EdgeKeys](const FLatticeEdge& Edge, bool) { EdgeKeys.Add(MakeEdgeKey(Edge)); });
		ForEachPlatformNode(Platform, SpanU, SpanV, Module, [&NodeKeys](const FIntVector& Node) { NodeKeys.Add(MakeCellKey(Node)); });

		RefreshLatticePieces(EdgeKeys, NodeKeys);
	}

	UpdateSurfaceMarks(ChangedSurfaces);

	if (IsFieldPiece(Platform))
	{
		RebuildAirLines();
	}
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
			ResetPieceLayer(Layer.Pieces, nullptr);
			StructureLayers.RemoveAt(LayerIndex);
		}
	}

	// A structure keeps its own materials, so no tint is put over it - unless it is nature drawn flat.
	for (const TPair<UPlatformStructureData*, TArray<FPlacedPiece>>& StructurePieces : Pieces.Structures)
	{
		FStructureLayer& Layer = FindOrAddStructureLayer(StructurePieces.Key);

		UStaticMesh* Mesh = nullptr;
		UMaterialInterface* Material = nullptr;
		ResolveStructureLayerLook(Layer, Mesh, Material);

		AddedPieces += SyncPieceLayer(Layer.Pieces, Layer.Pieces.Chunks[0], StructurePieces.Value, Mesh, Material);
	}

	// A beam or post kept from before may frame a segment the AI overlay takes in that it did not frame then, or the
	// other way round: it goes to the chunks of the side it is on now.
	AddedPieces += SortPiecesForOverlay();

	// Only a sync that built every platform can be added to one placement at a time.
	bPiecesComplete = bDrawPieces;

	return AddedPieces;
}

void ARoomManager::AppendPlatformPieces(const FGridPlatform& Platform)
{
	// Anything but the plain case - pieces switched off, never built in full, or a layer out of
	// step with its record - is left to the full sync.
	if (!ArePiecesInStep())
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
		TArray<FPlacedPiece>& OutNewPieces)
	{
		for (const FPlacedPiece& Piece : LayerPieces)
		{
			uint32& Stamp = Layer.PieceStamps.FindOrAdd(Piece.Key);

			// Any stamp but zero marks a piece as drawn, and the full sync that made this layer
			// complete - or the making of a structure's layer - left SyncStamp above zero.
			if (Stamp == 0)
			{
				Stamp = Layer.SyncStamp;
				OutNewPieces.Add(Piece);
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

		TArray<FPlacedPiece> NewPieces;
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

		UStaticMesh* Mesh = nullptr;
		UMaterialInterface* Material = nullptr;
		ResolveStructureLayerLook(Layer, Mesh, Material);
		ApplyMeshAndMaterial(Layer.Pieces.Chunks, Mesh, Material);

		TArray<FPlacedPiece> NewPieces;
		AppendToLayer(Layer.Pieces, StructurePieces.Value, NewPieces);
		AddPiecesToLayer(Layer.Pieces, Layer.Pieces.Chunks[0], NewPieces);
	}

	// A segment the AI overlay takes in brings the beams and posts round it in with it, the ones a neighbour already drew
	// too. Taking a segment in never lets another piece go, so nothing else changes side.
	const float OverlayPercent = GetOverlaySegmentPercent();

	if (OverlayPercent > 0.0f && OverlayPercent < 100.0f && IsSegmentPicked(MakePlatformKey(Platform), OverlayPercent))
	{
		const auto KeysOf = [](const TConstArrayView<FPlacedPiece> LayerPieces)
		{
			TArray<uint64> Keys;
			Keys.Reserve(LayerPieces.Num());
			for (const FPlacedPiece& Piece : LayerPieces)
			{
				Keys.Add(Piece.Key);
			}
			return Keys;
		};

		SortLayerForOverlay(EdgeLayer, EdgePieces, KeysOf(Pieces.Edges));
		SortLayerForOverlay(NodeLayer, NodePieces, KeysOf(Pieces.Nodes));

		// The water over a seam among them goes with its beam or post.
		for (const TPair<UPlatformStructureData*, TArray<FPlacedPiece>>& StructurePieces : Pieces.Structures)
		{
			FStructureLayer& Layer = FindOrAddStructureLayer(StructurePieces.Key);
			SortLayerForOverlay(Layer.Pieces, Layer.Pieces.Chunks[0], KeysOf(StructurePieces.Value));
		}
	}
}

void ARoomManager::GatherPlatformPieces(const FGridPlatform& Platform, FGatheredPieces& OutPieces) const
{
	int32 SpanU, SpanV;
	GetPlatformSpans(Platform, SpanU, SpanV);

	const int32 Module = GetModule();
	const int32 NormalAxis = AxisIndex(Platform.Normal);

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

		auto Fits = [&](const UPlatformStructureData* Candidate)
		{
			return Candidate && Candidate->Mesh
				&& WEMStructures::FitsShape(Candidate->Width, Candidate->Length, Candidate->bMayLieSideways, NormalAxis, Platform.Up, SpanU, SpanV, bTurned);
		};

		// Nature is drawn as the city structure covering it while that still fits, and one-sided like the nature.
		const bool bNature = Structure && Structure->IsNature();

		if (bNature && Fits(Platform.NatureCover))
		{
			Structure = Platform.NatureCover;
		}
		// Nature with no mesh of its own is a slab of its flat colour, which fits any opening.
		else if (bNature && !Structure->Mesh)
		{
			OutPieces.Structures.FindOrAdd(Structure).Add(FPlacedPiece{ Key, MakeFlatSlabTransform(Platform, Sign) });
			return;
		}
		else if (!Fits(Structure))
		{
			return;
		}

		OutPieces.Structures.FindOrAdd(Structure).Add(FPlacedPiece{
			Key, WEMStructures::MakeStructureTransform(MinCorner, CellSize, NormalAxis, Platform.Up, SpanU, SpanV, Sign, bTurned, Structure->bTwoSided && !bNature) });
	};

	// Nature is one-sided, whatever it is set to.
	if (Platform.PositiveStructure && Platform.PositiveStructure->bTwoSided && !Platform.PositiveStructure->IsNature())
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
	ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, const bool bRim)
	{
		if (bRim)
		{
			GatherEdgePieces(Edge.Node, Edge.Axis, OutPieces);
		}
	});

	ForEachRimNode(Platform, SpanU, SpanV, Module, [&](const FIntVector& Node)
	{
		GatherNodePieces(Node, OutPieces);
	});
}

void ARoomManager::GatherEdgePieces(const FIntVector& Node, const int32 Axis, FGatheredPieces& OutPieces) const
{
	const uint64 EdgeKey = MakeEdgeKey(FLatticeEdge{ Node, Axis });

	// A beam every platform around it has blocked is not built.
	if (!IsEdgeBuilt(EdgeKey))
	{
		return;
	}

	// The run between the posts at its two ends.
	const FIntVector MinCell = Node + AxisStep(Axis);
	FVector Extent(CellSize);
	Extent[Axis] = static_cast<double>(GetModule() - 1) * CellSize;

	FWaterSeam Seam;

	if (FindWaterSeam(Node, Axis, Seam))
	{
		OutPieces.Edges.Add(FPlacedPiece{ EdgeKey | WaterSeamPieceBit, MakeLatticePieceTransform(MinCell, Extent, &Seam, /*bWater=*/false) });
		OutPieces.Structures.FindOrAdd(Seam.Water).Add(FPlacedPiece{ EdgeKey | WaterSeamPieceBit, MakeLatticePieceTransform(MinCell, Extent, &Seam, /*bWater=*/true) });
		return;
	}

	OutPieces.Edges.Add(FPlacedPiece{ EdgeKey, MakeLatticePieceTransform(MinCell, Extent, nullptr, /*bWater=*/false) });
}

void ARoomManager::GatherNodePieces(const FIntVector& Node, FGatheredPieces& OutPieces) const
{
	// Nor a post no built beam ends at.
	if (!IsNodeBuilt(Node))
	{
		return;
	}

	const uint64 NodeKey = MakeCellKey(Node);
	const FVector Extent(CellSize);

	FWaterSeam Seam;

	if (FindWaterSeamAtNode(Node, Seam))
	{
		OutPieces.Nodes.Add(FPlacedPiece{ NodeKey | WaterSeamPieceBit, MakeLatticePieceTransform(Node, Extent, &Seam, /*bWater=*/false) });
		OutPieces.Structures.FindOrAdd(Seam.Water).Add(FPlacedPiece{ NodeKey | WaterSeamPieceBit, MakeLatticePieceTransform(Node, Extent, &Seam, /*bWater=*/true) });
		return;
	}

	OutPieces.Nodes.Add(FPlacedPiece{ NodeKey, MakeLatticePieceTransform(Node, Extent, nullptr, /*bWater=*/false) });
}

FTransform ARoomManager::MakeLatticePieceTransform(const FIntVector& MinCell, FVector Extent, const FWaterSeam* Seam, const bool bWater) const
{
	FVector Min = GetCellMinCorner(MinCell);

	// Across a seam the water takes the half of the layer on the side it looks out from, where its slabs lie, and the
	// beam or post keeps the half behind.
	if (Seam)
	{
		Extent[Seam->NormalAxis] = 0.5 * CellSize;

		if ((Seam->Sign > 0) == bWater)
		{
			Min[Seam->NormalAxis] += 0.5 * CellSize;
		}
	}

	// Everything on the lattice is axis-aligned, so every box is the unit cube scaled to the cells it fills.
	return FTransform(FRotator::ZeroRotator, Min + 0.5 * Extent, Extent / UnitCubeSize);
}

UPlatformStructureData* ARoomManager::GetFlatWater(const FIntVector& FaceMinNode, const int32 NormalAxis, int32& OutSign) const
{
	const int32* FaceOwner = ModuleFaceOwners.Find(MakeModuleFaceKey(FaceMinNode, NormalAxis));

	if (!FaceOwner || !BuiltPlatforms.IsValidIndex(*FaceOwner))
	{
		return nullptr;
	}

	const FGridPlatform& Platform = BuiltPlatforms[*FaceOwner];
	ENatureForm Form;

	if (!GetNatureFace(Platform, OutSign, Form) || Form != ENatureForm::Water)
	{
		return nullptr;
	}

	// Water with a mesh of its own, or a cover, is drawn as that, and has no slab to carry on across a beam.
	UPlatformStructureData* Water = GetFaceStructure(Platform, OutSign);
	return Water->Mesh || Platform.NatureCover ? nullptr : Water;
}

bool ARoomManager::FindWaterSeam(const FIntVector& Node, const int32 Axis, FWaterSeam& OutSeam) const
{
	if (NatureIndices[static_cast<int32>(ENatureForm::Water)].IsEmpty())
	{
		return false;
	}

	const int32 Module = GetModule();

	// The edge lies in two lattice planes, each facing along one of the other two axes.
	for (int32 NormalAxis = 0; NormalAxis < 3; ++NormalAxis)
	{
		if (NormalAxis == Axis)
		{
			continue;
		}

		// The axis across the edge in that plane, with a module face either side of it.
		const int32 Across = 3 - Axis - NormalAxis;

		int32 Sign = 0;
		int32 OtherSign = 0;
		UPlatformStructureData* Water = GetFlatWater(Node, NormalAxis, Sign);

		if (!Water || GetFlatWater(Node - AxisStep(Across, Module), NormalAxis, OtherSign) != Water || OtherSign != Sign)
		{
			continue;
		}

		// A face standing on the edge on the water's side parts the two, which then meet it rather than each other.
		const FIntVector StandingMinNode = Sign > 0 ? Node : Node - AxisStep(NormalAxis, Module);

		if (ModuleFaceOwners.Contains(MakeModuleFaceKey(StandingMinNode, Across)))
		{
			continue;
		}

		OutSeam.Water = Water;
		OutSeam.NormalAxis = NormalAxis;
		OutSeam.Sign = Sign;
		return true;
	}

	return false;
}

bool ARoomManager::FindWaterSeamAtNode(const FIntVector& Node, FWaterSeam& OutSeam) const
{
	if (NatureIndices[static_cast<int32>(ENatureForm::Water)].IsEmpty())
	{
		return false;
	}

	const int32 Module = GetModule();

	// The beams built at the node: the one starting at it along each axis, and the one ending at it.
	TArray<FLatticeEdge, TInlineAllocator<6>> Built;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (const FIntVector& Start : { Node, Node - AxisStep(Axis, Module) })
		{
			if (IsEdgeBuilt(MakeEdgeKey(FLatticeEdge{ Start, Axis })))
			{
				Built.Add(FLatticeEdge{ Start, Axis });
			}
		}
	}

	// The sheet is the one the first seam among them is in...
	const FLatticeEdge* First = Built.FindByPredicate([&](const FLatticeEdge& Edge) { return FindWaterSeam(Edge.Node, Edge.Axis, OutSeam); });

	if (!First)
	{
		return false;
	}

	// ...and every other beam there is a seam of the same sheet, or hangs behind it, as the wall under a pool does.
	for (const FLatticeEdge& Edge : Built)
	{
		if (Edge.Axis == OutSeam.NormalAxis)
		{
			// The one starting at the node heads along the axis, the one ending at it against.
			if ((Edge.Node == Node) == (OutSeam.Sign > 0))
			{
				return false;
			}

			continue;
		}

		FWaterSeam Seam;

		if (&Edge != First && !(FindWaterSeam(Edge.Node, Edge.Axis, Seam) && Seam == OutSeam))
		{
			return false;
		}
	}

	return true;
}

bool ARoomManager::ArePiecesInStep() const
{
	if (!bPiecesComplete || !bShowPieces || !PieceMesh
		|| !IsPieceLayerIntact(EdgeLayer, EdgePieces) || !IsPieceLayerIntact(NodeLayer, NodePieces))
	{
		return false;
	}

	for (const FStructureLayer& Layer : StructureLayers)
	{
		if (Layer.Pieces.Chunks.IsEmpty() || !IsPieceLayerIntact(Layer.Pieces, Layer.Pieces.Chunks[0]))
		{
			return false;
		}
	}

	return true;
}

void ARoomManager::RefreshLatticePieces(const TSet<uint64>& EdgeKeys, const TSet<uint64>& NodeKeys)
{
	if (!ArePiecesInStep())
	{
		return;
	}

	FGatheredPieces Wanted;

	// Every key either kind of piece could be drawn under, cut down or whole, to look for in the layers.
	TArray<uint64> LatticeKeys;
	TArray<uint64> NodeLatticeKeys;
	TArray<uint64> SeamKeys;

	for (const uint64 EdgeKey : EdgeKeys)
	{
		GatherEdgePieces(CellFromKey(EdgeKey), static_cast<int32>(EdgeKey >> KeyTagShift) - 1, Wanted);
		LatticeKeys.Append({ EdgeKey, EdgeKey | WaterSeamPieceBit });
		SeamKeys.Add(EdgeKey | WaterSeamPieceBit);
	}

	for (const uint64 NodeKey : NodeKeys)
	{
		GatherNodePieces(CellFromKey(NodeKey), Wanted);
		NodeLatticeKeys.Append({ NodeKey, NodeKey | WaterSeamPieceBit });
		SeamKeys.Add(NodeKey | WaterSeamPieceBit);
	}

	// A piece drawn under a key no longer wanted is taken away - the whole beam where it is now cut down, or the other
	// way round - and one wanted but not drawn is added.
	auto BringInLine = [this](FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk, const TConstArrayView<uint64> Keys, const TConstArrayView<FPlacedPiece> WantedPieces)
	{
		TSet<uint64> WantedKeys;
		TArray<FPlacedPiece> NewPieces;

		for (const FPlacedPiece& Piece : WantedPieces)
		{
			bool bAlreadyWanted = false;
			WantedKeys.Add(Piece.Key, &bAlreadyWanted);

			if (!bAlreadyWanted && !Layer.PieceSlots.Contains(Piece.Key))
			{
				Layer.PieceStamps.Add(Piece.Key, Layer.SyncStamp);
				NewPieces.Add(Piece);
			}
		}

		for (const uint64 Key : Keys)
		{
			if (!WantedKeys.Contains(Key))
			{
				RemovePieceFromLayer(Layer, Key);
			}
		}

		AddPiecesToLayer(Layer, FirstChunk, NewPieces);

		// One kept may have framed a segment the AI overlay takes in that has just gone, or met one just come.
		SortLayerForOverlay(Layer, FirstChunk, Keys);
	};

	BringInLine(EdgeLayer, EdgePieces, LatticeKeys, Wanted.Edges);
	BringInLine(NodeLayer, NodePieces, NodeLatticeKeys, Wanted.Nodes);

	// The water over the seams, in the layer of the water it belongs to and taken out of any other.
	for (const TPair<UPlatformStructureData*, TArray<FPlacedPiece>>& WaterPieces : Wanted.Structures)
	{
		FStructureLayer& Layer = FindOrAddStructureLayer(WaterPieces.Key);

		UStaticMesh* Mesh = nullptr;
		UMaterialInterface* Material = nullptr;
		ResolveStructureLayerLook(Layer, Mesh, Material);
		ApplyMeshAndMaterial(Layer.Pieces.Chunks, Mesh, Material);
	}

	for (FStructureLayer& Layer : StructureLayers)
	{
		if (Layer.Structure && Layer.Structure->IsNature() && Layer.Structure->NatureForm == ENatureForm::Water)
		{
			const TArray<FPlacedPiece>* WaterPieces = Wanted.Structures.Find(Layer.Structure.Get());
			BringInLine(Layer.Pieces, Layer.Pieces.Chunks[0], SeamKeys, WaterPieces ? TConstArrayView<FPlacedPiece>(*WaterPieces) : TConstArrayView<FPlacedPiece>());
		}
	}
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
		ResetPieceLayer(Layer->Pieces, nullptr);

		// A stamp of zero means "not drawn yet", so the layer's own has to start above it.
		Layer->Pieces.SyncStamp = FMath::Max<uint32>(Layer->Pieces.SyncStamp, 1);

		// Set up like the beams it stands among: lit, casting shadows, solid. The structure's mesh,
		// with its own materials, goes on when the layer is first filled.
		AddInstancedChunk(Layer->Pieces.Chunks, EdgePieces, FName(*FString::Printf(TEXT("%s_Pieces"), *Structure->GetName())));
	}

	return *Layer;
}

void ARoomManager::ResolveStructureLayerLook(FStructureLayer& Layer, UStaticMesh*& OutMesh, UMaterialInterface*& OutMaterial)
{
	const UPlatformStructureData* Structure = Layer.Structure;

	// Its own mesh with its own materials; or, for nature without one, the beams' box in its flat material, or the
	// beams' own, tinted to its flat colour.
	if (!Structure || Structure->Mesh)
	{
		OutMesh = Structure ? Structure->Mesh.Get() : nullptr;
		OutMaterial = nullptr;
		return;
	}

	OutMesh = PieceMesh;
	OutMaterial = ResolveTintedMaterial(
		Layer.FlatMaterial, Structure->FlatMaterial ? Structure->FlatMaterial.Get() : PieceMaterial.Get(), PieceColorParameterName, Structure->FlatColor);
}

void ARoomManager::ResetStructureLayers()
{
	for (FStructureLayer& Layer : StructureLayers)
	{
		ResetPieceLayer(Layer.Pieces, nullptr);
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

	TArray<FPlacedPiece> NewPieces;

	// Marks every piece still wanted and collects the ones not drawn yet.
	++Layer.SyncStamp;

	for (const FPlacedPiece& Piece : Pieces)
	{
		uint32& Stamp = Layer.PieceStamps.FindOrAdd(Piece.Key);

		// Named once however often it is named.
		if (Stamp == Layer.SyncStamp)
		{
			continue;
		}

		if (Stamp == 0)
		{
			NewPieces.Add(Piece);
		}

		Stamp = Layer.SyncStamp;
	}

	// A piece drawn but no longer wanted - its platform taken away - is taken out on its own.
	TArray<uint64> StalePieces;

	for (const TPair<uint64, uint32>& Drawn : Layer.PieceStamps)
	{
		if (Drawn.Value != Layer.SyncStamp)
		{
			StalePieces.Add(Drawn.Key);
		}
	}

	for (const uint64 Key : StalePieces)
	{
		if (!RemovePieceFromLayer(Layer, Key))
		{
			Layer.PieceStamps.Remove(Key);
		}
	}

	AddPiecesToLayer(Layer, FirstChunk, NewPieces);

	return NewPieces.Num() + StalePieces.Num();
}

bool ARoomManager::IsPieceLayerIntact(const FRoomPieceLayer& Layer, const UInstancedStaticMeshComponent* FirstChunk) const
{
	if (!FirstChunk || Layer.Chunks.IsEmpty() || Layer.Chunks[0].Get() != FirstChunk)
	{
		return false;
	}

	int32 DrawnPieces = 0;

	for (int32 ChunkIndex = 0; ChunkIndex < Layer.Chunks.Num(); ++ChunkIndex)
	{
		const UInstancedStaticMeshComponent* Chunk = Layer.Chunks[ChunkIndex];

		// Every chunk holds exactly the pieces its record names, in the same number.
		if (!IsValid(Chunk) || Chunk->GetInstanceCount() != (Layer.ChunkKeys.IsValidIndex(ChunkIndex) ? Layer.ChunkKeys[ChunkIndex].Num() : 0))
		{
			return false;
		}

		DrawnPieces += Chunk->GetInstanceCount();
	}

	return DrawnPieces == Layer.PieceStamps.Num() && DrawnPieces == Layer.PieceSlots.Num();
}

void ARoomManager::AddPiecesToLayer(
	FRoomPieceLayer& Layer,
	UInstancedStaticMeshComponent* FirstChunk,
	const TConstArrayView<FPlacedPiece> NewPieces)
{
	// A chunk made with the layer has no record of its own yet.
	Layer.ChunkKeys.SetNum(Layer.Chunks.Num());

	// A chunk draws everything it holds with one stencil value, so the pieces the AI overlay takes in and the ones it
	// leaves alone go to chunks of their own. With every piece taken in - the default - only the first kind is made.
	const float OverlayPercent = GetOverlaySegmentPercent();
	TArray<FPlacedPiece> PiecesBySide[2];

	for (const FPlacedPiece& Piece : NewPieces)
	{
		PiecesBySide[IsPieceInOverlay(Layer, Piece.Key, OverlayPercent) ? 1 : 0].Add(Piece);
	}

	for (int32 Side = 0; Side < 2; ++Side)
	{
		const TArray<FPlacedPiece>& SidePieces = PiecesBySide[Side];
		const int32 Stencil = Side == 1 ? AIOverlayStencil : OutOfOverlayStencil;
		const auto IsOfSide = [Side](const UInstancedStaticMeshComponent* Chunk)
		{
			return IsValid(Chunk) && (Chunk->CustomDepthStencilValue == AIOverlayStencil) == (Side == 1);
		};

		// New pieces go into the side's chunk still being filled, and a fresh chunk is started once that
		// one is full. Those are the only components whose instance count changes, so they are the
		// only ones whose surface cache Lumen has to capture again.
		int32 ChunkIndex = Layer.Chunks.FindLastByPredicate(IsOfSide);

		for (int32 NextPiece = 0; NextPiece < SidePieces.Num();)
		{
			int32 Room = ChunkIndex != INDEX_NONE ? PieceChunkCapacity - Layer.Chunks[ChunkIndex]->GetInstanceCount() : 0;

			if (Room <= 0)
			{
				UInstancedStaticMeshComponent* NewChunk = AddInstancedChunk(Layer.Chunks, FirstChunk ? FirstChunk : Layer.Chunks[0].Get());
				NewChunk->SetCustomDepthStencilValue(Stencil);
				Layer.ChunkKeys.AddDefaulted();
				ChunkIndex = Layer.Chunks.Num() - 1;
				Room = PieceChunkCapacity;
			}

			UInstancedStaticMeshComponent* OpenChunk = Layer.Chunks[ChunkIndex];
			const int32 Count = FMath::Min(Room, SidePieces.Num() - NextPiece);
			TArray<uint64>& Keys = Layer.ChunkKeys[ChunkIndex];
			const int32 FirstInstance = Keys.Num();

			TArray<FTransform> Transforms;
			Transforms.Reserve(Count);

			for (int32 Offset = 0; Offset < Count; ++Offset)
			{
				const FPlacedPiece& Piece = SidePieces[NextPiece + Offset];
				Transforms.Add(Piece.Transform);
				Keys.Add(Piece.Key);
				Layer.PieceSlots.Add(Piece.Key, FIntPoint(ChunkIndex, FirstInstance + Offset));
			}

			// One batch per chunk: one instance at a time would redo the component's bookkeeping
			// for every instance.
			OpenChunk->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);

			NextPiece += Count;
		}
	}
}

bool ARoomManager::IsPieceInOverlay(const FRoomPieceLayer& Layer, const uint64 Key, const float Percent) const
{
	if (Percent >= 100.0f)
	{
		return true;
	}
	if (Percent <= 0.0f)
	{
		return false;
	}

	// A structure's layer holds the fills, keyed by their platform, and the water over the seams, keyed by its beam or post.
	const bool bLattice = &Layer == &EdgeLayer || &Layer == &NodeLayer || (Key & WaterSeamPieceBit) != 0;
	const uint64 BareKey = Key & ~(NegativeFacePieceBit | WaterSeamPieceBit);

	if (!bLattice)
	{
		return IsSegmentPicked(BareKey, Percent);
	}

	// A post is keyed by its cell alone, a beam with its axis as the tag.
	const FIntVector Node = CellFromKey(BareKey);
	const int32 Tag = static_cast<int32>(BareKey >> KeyTagShift);

	return Tag == 0 ? DoesPostFramePickedSegment(Node, Percent) : DoesBeamFramePickedSegment(Node, Tag - 1, Percent);
}

bool ARoomManager::DoesBeamFramePickedSegment(const FIntVector& Node, const int32 Axis, const float Percent) const
{
	FModuleFace Faces[4];
	GetEdgeFaces(FLatticeEdge{ Node, Axis }, GetModule(), Faces);

	// In each plane through the beam, the faces either side of it. Where one platform holds both the beam runs across
	// its inside rather than round it.
	for (int32 Plane = 0; Plane < 2; ++Plane)
	{
		const int32* Owners[2] = {
			ModuleFaceOwners.Find(MakeModuleFaceKey(Faces[Plane * 2].MinNode, Faces[Plane * 2].Normal)),
			ModuleFaceOwners.Find(MakeModuleFaceKey(Faces[Plane * 2 + 1].MinNode, Faces[Plane * 2 + 1].Normal)) };

		if (Owners[0] && Owners[1] && *Owners[0] == *Owners[1])
		{
			continue;
		}

		for (const int32* FaceOwner : Owners)
		{
			if (FaceOwner && BuiltPlatforms.IsValidIndex(*FaceOwner) && IsSegmentPicked(MakePlatformKey(BuiltPlatforms[*FaceOwner]), Percent))
			{
				return true;
			}
		}
	}

	return false;
}

bool ARoomManager::DoesPostFramePickedSegment(const FIntVector& Node, const float Percent) const
{
	const int32 Module = GetModule();

	// A post on a segment's rim is where two beams of that rim meet, or one ends that carries on beyond it.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (DoesBeamFramePickedSegment(Node, Axis, Percent) || DoesBeamFramePickedSegment(Node - AxisStep(Axis, Module), Axis, Percent))
		{
			return true;
		}
	}

	return false;
}

int32 ARoomManager::SortLayerForOverlay(FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk, const TConstArrayView<uint64> Keys)
{
	// A layer out of step with its record is drawn again whole by the next sync, sorted as it is drawn.
	if (!IsPieceLayerIntact(Layer, FirstChunk))
	{
		return 0;
	}

	const float OverlayPercent = GetOverlaySegmentPercent();
	TArray<FPlacedPiece> Moving;
	TArray<uint32> Stamps;

	for (const uint64 Key : Keys)
	{
		const FIntPoint* Slot = Layer.PieceSlots.Find(Key);
		if (!Slot || !Layer.Chunks.IsValidIndex(Slot->X))
		{
			continue;
		}

		const UInstancedStaticMeshComponent* Chunk = Layer.Chunks[Slot->X];
		if (IsPieceInOverlay(Layer, Key, OverlayPercent) != (Chunk->CustomDepthStencilValue == AIOverlayStencil))
		{
			FPlacedPiece& Piece = Moving.AddDefaulted_GetRef();
			Piece.Key = Key;
			Chunk->GetInstanceTransform(Slot->Y, Piece.Transform, /*bWorldSpace=*/true);
			Stamps.Add(Layer.PieceStamps.FindRef(Key));
		}
	}

	// Taken out and drawn again on the other side, still named by the sync that last found it.
	for (const FPlacedPiece& Piece : Moving)
	{
		RemovePieceFromLayer(Layer, Piece.Key);
	}

	AddPiecesToLayer(Layer, FirstChunk, Moving);

	for (int32 Index = 0; Index < Moving.Num(); ++Index)
	{
		Layer.PieceStamps.Add(Moving[Index].Key, Stamps[Index]);
	}

	return Moving.Num();
}

int32 ARoomManager::SortPiecesForOverlay()
{
	const bool bEveryPieceIn = GetOverlaySegmentPercent() >= 100.0f;
	int32 Moved = 0;

	auto SortLayer = [this, bEveryPieceIn, &Moved](FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk)
	{
		// At the default, with every piece taken in, a layer with nothing drawn out of the overlay has nothing to move.
		const bool bAnyOut = Layer.Chunks.ContainsByPredicate([](const UInstancedStaticMeshComponent* Chunk)
		{
			return IsValid(Chunk) && Chunk->CustomDepthStencilValue != AIOverlayStencil && Chunk->GetInstanceCount() > 0;
		});

		if (bEveryPieceIn && !bAnyOut)
		{
			return;
		}

		TArray<uint64> Keys;
		Layer.PieceSlots.GenerateKeyArray(Keys);
		Moved += SortLayerForOverlay(Layer, FirstChunk, Keys);
	};

	SortLayer(EdgeLayer, EdgePieces);
	SortLayer(NodeLayer, NodePieces);

	for (FStructureLayer& Layer : StructureLayers)
	{
		if (!Layer.Pieces.Chunks.IsEmpty())
		{
			SortLayer(Layer.Pieces, Layer.Pieces.Chunks[0]);
		}
	}

	return Moved;
}

void ARoomManager::ResetPieceLayer(FRoomPieceLayer& Layer, UInstancedStaticMeshComponent* FirstChunk)
{
	ResetChunks(Layer.Chunks, FirstChunk);
	Layer.PieceStamps.Reset();
	Layer.PieceSlots.Reset();
	Layer.ChunkKeys.Reset();
	Layer.ChunkKeys.SetNum(Layer.Chunks.Num());
}

bool ARoomManager::RemovePieceFromLayer(FRoomPieceLayer& Layer, const uint64 Key)
{
	FIntPoint Slot;

	if (!Layer.PieceSlots.RemoveAndCopyValue(Key, Slot))
	{
		return false;
	}

	Layer.PieceStamps.Remove(Key);

	const int32 Chunk = Slot.X;
	const int32 Instance = Slot.Y;

	if (!Layer.Chunks.IsValidIndex(Chunk) || !IsValid(Layer.Chunks[Chunk])
		|| !Layer.ChunkKeys.IsValidIndex(Chunk) || !Layer.ChunkKeys[Chunk].IsValidIndex(Instance))
	{
		return true;
	}

	UInstancedStaticMeshComponent* Component = Layer.Chunks[Chunk];
	TArray<uint64>& Keys = Layer.ChunkKeys[Chunk];
	const int32 Last = Keys.Num() - 1;

	// As the overlay's squares are taken out: the chunk's last piece is moved into the hole first, so no other piece
	// changes index and the record needs one fix-up at most. Its collision jumps there with it, rather than sweeping
	// across the tower over the next physics step.
	if (Instance != Last)
	{
		FTransform LastTransform;
		Component->GetInstanceTransform(Last, LastTransform, /*bWorldSpace=*/true);
		Component->UpdateInstanceTransform(Instance, LastTransform, /*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);

		Keys[Instance] = Keys[Last];
		Layer.PieceSlots.FindChecked(Keys[Instance]).Y = Instance;
	}

	Component->RemoveInstance(Last);
	Keys.Pop(EAllowShrinking::No);
	return true;
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
	Chunk->SetCanEverAffectNavigation(FirstChunk->CanEverAffectNavigation());
	Chunk->SetCastShadow(FirstChunk->CastShadow);
	Chunk->bAffectDistanceFieldLighting = FirstChunk->bAffectDistanceFieldLighting;
	Chunk->bAffectDynamicIndirectLighting = FirstChunk->bAffectDynamicIndirectLighting;
	Chunk->SetVisibleInRayTracing(FirstChunk->bVisibleInRayTracing);
	Chunk->SetRenderCustomDepth(FirstChunk->bRenderCustomDepth);
	Chunk->SetCustomDepthStencilValue(FirstChunk->CustomDepthStencilValue);
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
	TArray<FBatchedLine> Lines;

	if (bShowDebugGrid)
	{
		AppendLatticeLines(Lines);
	}

	// The zones are drawn on their own switch, off by default.
	if (bShowZones)
	{
		AppendZoneLines(Lines);
	}

	if (!Lines.IsEmpty())
	{
		GridLineBatcher->DrawLines(Lines);
	}
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
		// A side its structure closes off is no surface, and never has a square; nor has one whose platform has been
		// taken away, and its square goes.
		const int32 Layer = ResolveSurfaceMarkLayer(Surface.Cell, Surface.Face);
		const uint64 Key = MakeFaceKey(Surface.Cell, Surface.Face);

		if (Layer == INDEX_NONE)
		{
			FSurfaceMarkSlot Gone;

			if (SurfaceMarkSlots.RemoveAndCopyValue(Key, Gone) && Gone.Instance != INDEX_NONE)
			{
				RemoveSurfaceMark(Gone.Layer, Gone.Chunk, Gone.Instance);
			}

			continue;
		}

		SyncSurfaceMark(Surface.Cell, Surface.Face, Key, Layer, SurfaceMarkSlots.FindOrAdd(Key), Pending);
	}

	AddPendingSurfaceMarks(Pending);
}

int32 ARoomManager::GetZoneLine(const int32 Line) const
{
	// On lattice lines, a third of the modules apart, so a border never runs through a module face.
	const int32 Module = GetModule();
	const int32 Modules = (ResolveGridSize() - 1) / Module;

	return FMath::RoundToInt32(static_cast<double>(Modules) * Line / ZonesPerSide) * Module;
}

int32 ARoomManager::GetZoneOf(const FGridPlatform& Platform) const
{
	// Its middle, doubled so a middle between two cells stays whole, against the borders doubled likewise.
	const FAirBox Box = GetPlatformBox(Platform);

	int32 Zone = 0;
	int32 Stride = 1;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const int32 TwiceMiddle = Box.Min[Axis] + Box.Max[Axis];
		int32 Index = 0;

		while (Index < ZonesPerSide - 1 && TwiceMiddle >= 2 * GetZoneLine(Index + 1))
		{
			++Index;
		}

		Zone += Index * Stride;
		Stride *= ZonesPerSide;
	}

	return Zone;
}

ARoomManager::FAirBox ARoomManager::GetZoneBox(const int32 Zone) const
{
	// Along X, then Y, then up a layer.
	const FIntVector Index(Zone % ZonesPerSide, (Zone / ZonesPerSide) % ZonesPerSide, Zone / (ZonesPerSide * ZonesPerSide));

	FAirBox Box;
	Box.Min = FIntVector(GetZoneLine(Index.X), GetZoneLine(Index.Y), GetZoneLine(Index.Z));
	Box.Max = FIntVector(GetZoneLine(Index.X + 1), GetZoneLine(Index.Y + 1), GetZoneLine(Index.Z + 1));
	return Box;
}

void ARoomManager::AppendZoneLines(TArray<FBatchedLine>& Lines) const
{
	// A quarter of a cell inside its borders, so the lines of two zones side by side never lie on each other.
	const FVector Inset(0.25 * CellSize);

	for (int32 Zone = 0; Zone < ZoneCount; ++Zone)
	{
		const FAirBox Box = GetZoneBox(Zone);
		const FVector Min = GetCellMinCorner(Box.Min) + Inset;
		const FVector Max = GetCellMinCorner(Box.Max) + FVector(CellSize) - Inset;
		const FLinearColor Color(ZoneColors.IsValidIndex(Zone) ? ZoneColors[Zone] : GetDefaultZoneColor(Zone, ZonesPerSide * ZonesPerSide));

		AppendBoxLines(Lines, Min, Max, Color, BoundaryLineThickness, SDPG_World, /*bWithDiagonals=*/false);

		// A cross over its floor and its roof, so each zone's colour reads from above and below as well as from the side.
		for (const double Z : { Min.Z, Max.Z })
		{
			Lines.Emplace(FVector(Min.X, Min.Y, Z), FVector(Max.X, Max.Y, Z), Color, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_World);
			Lines.Emplace(FVector(Max.X, Min.Y, Z), FVector(Min.X, Max.Y, Z), Color, /*LifeTime=*/0.0f, BoundaryLineThickness, SDPG_World);
		}
	}

	// Each ground tile as a crossed box in its zone's colour, a cell out all round, so it reads over its own beams.
	for (const TPair<uint64, int32>& Ground : GroundTiles)
	{
		const int32* Index = ModuleFaceOwners.Find(Ground.Key);

		if (!Index)
		{
			continue;
		}

		const FAirBox Box = GetPlatformBox(BuiltPlatforms[*Index]);
		const FLinearColor Color(ZoneColors.IsValidIndex(Ground.Value) ? ZoneColors[Ground.Value] : GetDefaultZoneColor(Ground.Value, ZonesPerSide * ZonesPerSide));

		AppendBoxLines(Lines, GetCellMinCorner(Box.Min) - FVector(CellSize), GetCellMinCorner(Box.Max) + FVector(2.0 * CellSize),
			Color, 2.0f * BoundaryLineThickness, SDPG_World, /*bWithDiagonals=*/true);
	}
}

void ARoomManager::UpdateZoneSummary()
{
	int32 Counts[ZoneCount] = {};

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		++Counts[GetZoneOf(Platform)];
	}

	// A layer at a time, from the bottom up.
	TArray<FString> LayerParts;

	for (int32 Layer = 0; Layer < ZonesPerSide; ++Layer)
	{
		TArray<FString> Parts;

		for (int32 Zone = Layer * ZonesPerSide * ZonesPerSide; Zone < (Layer + 1) * ZonesPerSide * ZonesPerSide; ++Zone)
		{
			Parts.Add(FString::Printf(TEXT("%d"), Counts[Zone]));
		}

		LayerParts.Add(FString::Join(Parts, TEXT(", ")));
	}

	ZoneSummary = FString::Join(LayerParts, TEXT(" | "));
}

void ARoomManager::QueueSeeds()
{
	SeedQueue.Reset();

	if (!bSeedEveryZone)
	{
		return;
	}

	// The middle zone's first tile is the first platform, laid as it always was; every other zone waits for its own. A
	// round at a time, so every zone has its first before any has its second.
	const int32 Rounds = FMath::Clamp(SeedsPerZone, 1, 32);

	for (int32 Round = 0; Round < Rounds; ++Round)
	{
		for (int32 Zone = 0; Zone < ZoneCount; ++Zone)
		{
			if (Round > 0 || Zone != MiddleZone)
			{
				SeedQueue.Add(Zone);
			}
		}
	}
}

bool ARoomManager::PlaceSeed(const int32 Zone)
{
	// A first tile is always laid flat, so one kept to walls starts nothing.
	if (!IsTileUsable(FirstTile) || !FirstTile->AllowsOrientation(/*bVertical=*/false))
	{
		return false;
	}

	// In step with the middle one: whole tiles away from it across the cube, and whole shorter sides up or down, so the
	// growth of the two lines up where it meets.
	FIntVector Phase = FIntVector::ZeroValue;

	if (SeedPhase.IsSet())
	{
		Phase = SeedPhase.GetValue();
	}
	else
	{
		TArray<FGridPlatform> FirstCandidates;
		GatherFirstPlatformCandidates(FirstCandidates);
		Phase = FirstCandidates.IsEmpty() ? FIntVector::ZeroValue : FirstCandidates[0].MinNode;
	}

	const int32 StepX = FirstTile->Width;
	const int32 StepY = FirstTile->Length;
	const int32 StepZ = FMath::Min(FirstTile->Width, FirstTile->Length);
	const int32 LastNode = ResolveGridSize() - 1;
	const FAirBox ZoneBox = GetZoneBox(Zone);

	// Along one axis, every place on the steps from PhaseAt between Lowest and Highest.
	auto GetPlaces = [](const int32 PhaseAt, const int32 Step, const int32 Lowest, const int32 Highest)
	{
		TArray<int32> Places;

		for (int32 At = PhaseAt + FMath::CeilToInt32(static_cast<double>(Lowest - PhaseAt) / Step) * Step; At <= Highest; At += Step)
		{
			Places.Add(At);
		}

		return Places;
	};

	// The whole tile inside its zone.
	const TArray<int32> PlacesX = GetPlaces(Phase.X, StepX, ZoneBox.Min.X, ZoneBox.Max.X - StepX);
	const TArray<int32> PlacesY = GetPlaces(Phase.Y, StepY, ZoneBox.Min.Y, ZoneBox.Max.Y - StepY);
	const TArray<int32> PlacesZ = GetPlaces(Phase.Z, StepZ, ZoneBox.Min.Z, FMath::Min(ZoneBox.Max.Z, LastNode));

	if (PlacesX.IsEmpty() || PlacesY.IsEmpty() || PlacesZ.IsEmpty())
	{
		return false;
	}

	const int32 Module = GetModule();

	for (int32 Attempt = 0; Attempt < MaxSeedPlaces; ++Attempt)
	{
		FGridPlatform Seed = MakePlatform(
			FIntVector(PlacesX[SeedStream.RandRange(0, PlacesX.Num() - 1)], PlacesY[SeedStream.RandRange(0, PlacesY.Num() - 1)], PlacesZ[SeedStream.RandRange(0, PlacesZ.Num() - 1)]),
			AxisIndex(EGridAxis::Z), FirstTile, /*bTurned=*/false);

		if (!IsPlatformOnGrid(Seed) || !IsSpaceFree(Seed) || !IsClearOfHeldAir(Seed))
		{
			continue;
		}

		// Clear of everything standing - sharing no beam, nor even a post at a corner - so it starts growth of its own.
		int32 SpanU, SpanV;
		GetPlatformSpans(Seed, SpanU, SpanV);

		bool bTouches = false;

		ForEachModuleEdge(Seed, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, bool)
		{
			bTouches = bTouches || ModuleEdgeUses.Contains(MakeEdgeKey(Edge));
		});

		ForEachRimNode(Seed, SpanU, SpanV, Module, [&](const FIntVector& Node)
		{
			bTouches = bTouches || RimCellNormals.Contains(MakeCellKey(Node));
		});

		// It holds up whatever grows off it, as the middle one does.
		if (bTouches || !DrawFill(Seed, /*bLoadBearingOnly=*/true))
		{
			continue;
		}

		PlacePlatform(Seed, /*JoinedSide=*/0);

		// Every seed is a ground tile, a zone's second and third as much as its first.
		if (MakeGroundTile(Seed) && bShowZones)
		{
			RebuildDebugGrid();
		}

		QueueGroundSpill(Seed);
		return true;
	}

	return false;
}

void ARoomManager::GiveHealth(const FGridPlatform& Platform)
{
	PlatformHealth.Add(MakePlatformKey(Platform), DamageStream.FRandRange(MinHealth, FMath::Max(MaxHealth, MinHealth)));
}

bool ARoomManager::IsVulnerable(const FGridPlatform& Platform) const
{
	return !IsClearOfHeldAir(Platform);
}

void ARoomManager::Strike()
{
	++StrikeCount;

	const float Chance = FMath::Clamp(StrikeFraction, 0.0f, 1.0f);
	const float Least = FMath::Max(MinDamage, 0.0f);
	const float Most = FMath::Max(MaxDamage, Least);

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		if (DamageStream.FRand() >= Chance)
		{
			continue;
		}

		const uint64 Key = MakePlatformKey(Platform);
		float Damage = DamageStream.FRandRange(Least, Most);
		++HitCount;

		// Asked only of the platforms hit, which is all it matters for.
		if (IsVulnerable(Platform))
		{
			Damage *= FMath::Max(VulnerableDamageMultiplier, 1.0f);
			++VulnerableHitCount;
		}

		float* Health = PlatformHealth.Find(Key);

		if (!Health)
		{
			GiveHealth(Platform);
			Health = PlatformHealth.Find(Key);
		}

		*Health -= Damage;

		if (*Health <= 0.0f && !DyingKeys.Contains(Key))
		{
			DyingKeys.Add(Key);
			DyingQueue.Add(Key);
		}
	}

	UpdateZoneSummary();
	UpdateDeletionSummary();
}

void ARoomManager::StrikeSegments()
{
	Strike();
	RemoveDying(MAX_int32);
}

void ARoomManager::RemoveDying(const int32 Most)
{
	if (DyingQueue.IsEmpty() || Most <= 0)
	{
		return;
	}

	// Each as it stands - fill and all - so its pieces can be found. One taken away since it was queued is passed over.
	TArray<FGridPlatform> Removed;
	int32 Taken = 0;

	while (Taken < DyingQueue.Num() && Removed.Num() < Most)
	{
		const uint64 Key = DyingQueue[Taken++];
		DyingKeys.Remove(Key);

		const int32* Index = ModuleFaceOwners.Find(Key);

		if (Index && MakePlatformKey(BuiltPlatforms[*Index]) == Key)
		{
			Removed.Add(BuiltPlatforms[*Index]);
		}
	}

	DyingQueue.RemoveAt(0, Taken, EAllowShrinking::No);
	DamageRemovals += Removed.Num();

	RemovePlatforms(Removed);
	UpdateDeletionSummary();
}

bool ARoomManager::HoldsObject(const FGridPlatform& Platform) const
{
	if (Obstacles.IsEmpty())
	{
		return false;
	}

	// An object stands on a face in the cells just off it, so a box reaching one cell out either way finds it.
	FAirBox Box = GetPlatformBox(Platform);
	const int32 NormalAxis = AxisIndex(Platform.Normal);
	--Box.Min[NormalAxis];
	++Box.Max[NormalAxis];

	return Obstacles.ContainsByPredicate([&Box](const FGridObstacle& Obstacle)
	{
		return Box.Intersects(FAirBox{ Obstacle.MinCell, Obstacle.MaxCell });
	});
}

void ARoomManager::BeginReshuffle()
{
	ReshuffleKeys.Reset();
	ReshuffleEmptied.Reset();
	ReshuffleZoneIndex = ReshuffleStream.RandRange(0, ZoneCount - 1);

	const int32 Fewest = FMath::Max(MinReshuffleCount, 0);
	const int32 Count = ReshuffleStream.RandRange(Fewest, FMath::Max(MaxReshuffleCount, Fewest));

	// The platforms in the zone holding no objects, in list order, and as many of them as drawn picked at random.
	TArray<uint64> Pool;

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		if (GetZoneOf(Platform) == ReshuffleZoneIndex && !HoldsObject(Platform))
		{
			Pool.Add(MakePlatformKey(Platform));
		}
	}

	for (int32 Pick = 0; Pick < Count && Pick < Pool.Num(); ++Pick)
	{
		Pool.Swap(Pick, ReshuffleStream.RandRange(Pick, Pool.Num() - 1));
		ReshuffleKeys.Add(Pool[Pick]);
	}

	++ReshuffleCount;
	UpdateDeletionSummary();
}

bool ARoomManager::StepReshuffle()
{
	while (ReshuffleZoneIndex != INDEX_NONE && !ReshuffleKeys.IsEmpty())
	{
		const uint64 Key = ReshuffleKeys[0];
		ReshuffleKeys.RemoveAt(0);

		// One taken away since it was drawn - struck down, say - is passed over.
		const int32* Index = ModuleFaceOwners.Find(Key);

		if (!Index || MakePlatformKey(BuiltPlatforms[*Index]) != Key)
		{
			continue;
		}

		const FGridPlatform Platform = BuiltPlatforms[*Index];

		// The spot it empties is kept free for the rest of the reshuffle: the opening between its beams, so a platform
		// may still share the beams round it.
		int32 U, V;
		GetInPlaneAxes(AxisIndex(Platform.Normal), U, V);

		FAirBox Emptied = GetPlatformBox(Platform);
		++Emptied.Min[U];
		++Emptied.Min[V];
		--Emptied.Max[U];
		--Emptied.Max[V];
		ReshuffleEmptied.Add(Emptied);

		RemovePlatforms(MakeArrayView(&Platform, 1));
		++ReshuffleRemovals;

		// And one laid elsewhere in the same zone, as a beat would lay it, away from every spot the reshuffle emptied.
		const int32 Zone = ReshuffleZoneIndex;
		const TFunction<bool(const FGridPlatform&)> InZone = [this, Zone](const FGridPlatform& Candidate)
		{
			if (GetZoneOf(Candidate) != Zone)
			{
				return false;
			}

			const FAirBox Box = GetPlatformBox(Candidate);
			return !ReshuffleEmptied.ContainsByPredicate([&Box](const FAirBox& Spot) { return Spot.Intersects(Box); });
		};

		const bool bVerticalFirst = PlacementStream.FRand() < VerticalPlacementChance;
		const EPlatformKind FirstKind = bVerticalFirst ? EPlatformKind::SurfaceVertical : EPlatformKind::SurfaceHorizontal;
		const EPlatformKind SecondKind = bVerticalFirst ? EPlatformKind::SurfaceHorizontal : EPlatformKind::SurfaceVertical;

		if (PlacePlatformOfKind(FirstKind, &InZone) || PlacePlatformOfKind(SecondKind, &InZone))
		{
			++ReshufflePlacements;
		}

		UpdateDeletionSummary();
		return true;
	}

	ReshuffleZoneIndex = INDEX_NONE;
	ReshuffleEmptied.Reset();
	return false;
}

void ARoomManager::ReshuffleZone()
{
	BeginReshuffle();

	while (StepReshuffle())
	{
	}
}

void ARoomManager::AdvanceDeletion(const double Now)
{
	if (bEnableDamage && Now >= NextStrikeAt)
	{
		Strike();

		const float Shortest = FMath::Max(MinStrikeInterval, 0.05f);
		NextStrikeAt = Now + DamageStream.FRandRange(Shortest, FMath::Max(MaxStrikeInterval, Shortest));
	}

	if (!bEnableReshuffle)
	{
		return;
	}

	// One reshuffle at a time: the next falls due once the last has started, and starts once that one is over.
	if (ReshuffleZoneIndex == INDEX_NONE && Now >= NextReshuffleAt)
	{
		BeginReshuffle();
		NextReshuffleAt = Now + FMath::Max(ReshuffleInterval, 0.1f);
		NextReshuffleStepAt = Now;
	}

	if (ReshuffleZoneIndex != INDEX_NONE && Now >= NextReshuffleStepAt)
	{
		StepReshuffle();
		NextReshuffleStepAt = Now + FMath::Max(ReshuffleStepInterval, 0.05f);
	}
}

void ARoomManager::UpdateDeletionSummary()
{
	DeletionSummary = FString::Printf(TEXT("%d strikes, %d hits (%d vulnerable), %d taken away, %d waiting; %d reshuffles, %d taken away and %d laid; %d pocket moves; %d ground tiles, %d passed on, %d cut loose"),
		StrikeCount, HitCount, VulnerableHitCount, DamageRemovals, DyingQueue.Num(), ReshuffleCount, ReshuffleRemovals, ReshufflePlacements, PocketMoves,
		GroundTiles.Num(), GroundTilesPassedOn, CutLooseRemovals);
}

bool ARoomManager::MakeGroundTile(const FGridPlatform& Platform)
{
	const uint64 Key = MakePlatformKey(Platform);

	if (GroundTiles.Contains(Key))
	{
		return false;
	}

	const int32 Zone = GetZoneOf(Platform);
	GroundTiles.Add(Key, Zone);
	++GroundTileCounts[Zone];
	return true;
}

bool ARoomManager::PassOnGroundTiles(const TSet<uint64>& RemovedKeys)
{
	// The zone of each ground tile going, in zone order, so the draws come out the same whatever order they were named in.
	TArray<int32> Zones;

	for (const uint64 Key : RemovedKeys)
	{
		if (const int32* Zone = GroundTiles.Find(Key))
		{
			Zones.Add(*Zone);
			--GroundTileCounts[*Zone];
			GroundTiles.Remove(Key);
		}
	}

	if (Zones.IsEmpty())
	{
		return false;
	}

	Zones.Sort();

	for (const int32 Zone : Zones)
	{
		// Among the platforms staying in its zone, in list order, any but the ground tiles it has already. A zone with none
		// left goes without, until the next platform laid in it.
		TArray<int32> Pool;

		for (int32 Index = 0; Index < BuiltPlatforms.Num(); ++Index)
		{
			const uint64 Key = MakePlatformKey(BuiltPlatforms[Index]);

			if (!RemovedKeys.Contains(Key) && !GroundTiles.Contains(Key) && GetZoneOf(BuiltPlatforms[Index]) == Zone)
			{
				Pool.Add(Index);
			}
		}

		if (!Pool.IsEmpty())
		{
			MakeGroundTile(BuiltPlatforms[Pool[GroundTileStream.RandRange(0, Pool.Num() - 1)]]);
			++GroundTilesPassedOn;
		}
	}

	return true;
}

void ARoomManager::GatherCutLoose(const TConstArrayView<FGridPlatform> Removed, TArray<FGridPlatform>& OutLoose) const
{
	OutLoose.Reset();

	// With no ground tile at all there is nothing to link to, and nothing is cut from it: a cube emptied of them is
	// empty, since a zone with platforms always has one.
	if (GroundTiles.IsEmpty() || BuiltPlatforms.IsEmpty())
	{
		return;
	}

	// Where each ground tile's middle lies, doubled so it stays whole, for the search to head for.
	auto GetTwiceMiddle = [this](const FGridPlatform& Platform)
	{
		const FAirBox Box = GetPlatformBox(Platform);
		return Box.Min + Box.Max;
	};

	TArray<FIntVector> GroundMiddles;
	GroundMiddles.Reserve(GroundTiles.Num());

	for (const TPair<uint64, int32>& Ground : GroundTiles)
	{
		if (const int32* Index = ModuleFaceOwners.Find(Ground.Key))
		{
			GroundMiddles.Add(GetTwiceMiddle(BuiltPlatforms[*Index]));
		}
	}

	auto GetDistance = [&](const int32 Index)
	{
		const FIntVector Middle = GetTwiceMiddle(BuiltPlatforms[Index]);
		int32 Nearest = MAX_int32;

		for (const FIntVector& Ground : GroundMiddles)
		{
			const FIntVector Step = Ground - Middle;
			Nearest = FMath::Min(Nearest, FMath::Abs(Step.X) + FMath::Abs(Step.Y) + FMath::Abs(Step.Z));
		}

		return Nearest;
	};

	auto IsGroundTile = [this](const int32 Index)
	{
		return GroundTiles.Contains(MakePlatformKey(BuiltPlatforms[Index]));
	};

	// Only what touched a platform taken away can have lost its link: everything else still links the way it did.
	TArray<int32> Starts;
	TArray<int32, TInlineAllocator<8>> Touching;

	for (const FGridPlatform& Platform : Removed)
	{
		GatherTouchingPlatforms(Platform, Touching);

		for (const int32 Index : Touching)
		{
			Starts.AddUnique(Index);
		}
	}

	Starts.Sort();

	// Each searches out through the beams it shares, nearest a ground tile first, until it meets one, or one already found
	// to link: then everything it came across links too, being joined to it. A search that runs out has been through the
	// whole piece it is in, and found no ground tile in it.
	TSet<int32> Linked;
	TSet<int32> Loose;

	using FStep = TPair<int32, int32>;
	auto Nearer = [](const FStep& First, const FStep& Second)
	{
		return First.Key != Second.Key ? First.Key < Second.Key : First.Value < Second.Value;
	};

	for (const int32 Start : Starts)
	{
		if (Linked.Contains(Start) || Loose.Contains(Start))
		{
			continue;
		}

		TSet<int32> Seen;
		TArray<int32> Reached;
		TArray<FStep> Frontier;
		bool bLinks = IsGroundTile(Start);

		Seen.Add(Start);
		Reached.Add(Start);
		Frontier.HeapPush(FStep(GetDistance(Start), Start), Nearer);

		while (!bLinks && !Frontier.IsEmpty())
		{
			FStep Next;
			Frontier.HeapPop(Next, Nearer, EAllowShrinking::No);

			GatherTouchingPlatforms(BuiltPlatforms[Next.Value], Touching);

			for (const int32 Index : Touching)
			{
				bool bAlreadySeen = false;
				Seen.Add(Index, &bAlreadySeen);

				if (bAlreadySeen)
				{
					continue;
				}

				Reached.Add(Index);

				if (Linked.Contains(Index) || IsGroundTile(Index))
				{
					bLinks = true;
					break;
				}

				Frontier.HeapPush(FStep(GetDistance(Index), Index), Nearer);
			}
		}

		(bLinks ? Linked : Loose).Append(Reached);
	}

	// In list order, as they stand.
	TArray<int32> LooseIndices = Loose.Array();
	LooseIndices.Sort();
	OutLoose.Reserve(LooseIndices.Num());

	for (const int32 Index : LooseIndices)
	{
		OutLoose.Add(BuiltPlatforms[Index]);
	}
}

void ARoomManager::GatherAllLoose(TArray<int32>& OutLoose) const
{
	OutLoose.Reset();

	// Out from every ground tile at once, through the beams shared; whatever is never reached has no link.
	TArray<bool> bReached;
	bReached.Init(false, BuiltPlatforms.Num());

	TArray<int32> Frontier;

	for (const TPair<uint64, int32>& Ground : GroundTiles)
	{
		if (const int32* Index = ModuleFaceOwners.Find(Ground.Key); Index && !bReached[*Index])
		{
			bReached[*Index] = true;
			Frontier.Add(*Index);
		}
	}

	TArray<int32, TInlineAllocator<8>> Touching;

	for (int32 Next = 0; Next < Frontier.Num(); ++Next)
	{
		GatherTouchingPlatforms(BuiltPlatforms[Frontier[Next]], Touching);

		for (const int32 Index : Touching)
		{
			if (!bReached[Index])
			{
				bReached[Index] = true;
				Frontier.Add(Index);
			}
		}
	}

	for (int32 Index = 0; Index < BuiltPlatforms.Num(); ++Index)
	{
		if (!bReached[Index])
		{
			OutLoose.Add(Index);
		}
	}
}

void ARoomManager::ReconcileGroundTiles()
{
	FMemory::Memzero(GroundTileCounts);

	// The zones are worked out again, since the cube they lie in may have changed.
	for (auto Ground = GroundTiles.CreateIterator(); Ground; ++Ground)
	{
		const int32* Index = ModuleFaceOwners.Find(Ground.Key());

		if (!Index || MakePlatformKey(BuiltPlatforms[*Index]) != Ground.Key())
		{
			Ground.RemoveCurrent();
			continue;
		}

		Ground.Value() = GetZoneOf(BuiltPlatforms[*Index]);
		++GroundTileCounts[Ground.Value()];
	}

	for (const FGridPlatform& Platform : BuiltPlatforms)
	{
		if (GroundTileCounts[GetZoneOf(Platform)] == 0)
		{
			MakeGroundTile(Platform);
		}
	}
}

void ARoomManager::RemovePlatforms(const TConstArrayView<FGridPlatform> Removed)
{
	// Only those standing, each once, as they stand - fill and all - so their pieces can be found.
	TSet<uint64> RemovedKeys;
	TArray<int32> Indices;
	TArray<FGridPlatform> Standing;

	for (const FGridPlatform& Platform : Removed)
	{
		const uint64 Key = MakePlatformKey(Platform);
		const int32* Index = ModuleFaceOwners.Find(Key);

		if (!Index || MakePlatformKey(BuiltPlatforms[*Index]) != Key || RemovedKeys.Contains(Key))
		{
			continue;
		}

		RemovedKeys.Add(Key);
		Indices.Add(*Index);
		Standing.Add(BuiltPlatforms[*Index]);
	}

	if (Indices.IsEmpty())
	{
		return;
	}

	Indices.Sort();

	auto IsRemoved = [&RemovedKeys](const FGridPlatform& Platform)
	{
		return RemovedKeys.Contains(MakePlatformKey(Platform));
	};

	// A ground tile going is passed on to another platform of its zone while they all still stand, so the draw sees the zone
	// as it is.
	const bool bGroundTilesChanged = PassOnGroundTiles(RemovedKeys);

	// The list first, which is what is saved, then whatever waited on them: a fence's, a cliff's or a wall run's end on a panel, a piece of
	// a field or of a patch of ground, a platform's health and its place in the queue to go.
	Platforms.RemoveAll(IsRemoved);
	FenceQueue.RemoveAll([&IsRemoved](const FFenceEnd& End) { return IsRemoved(End.Panel); });
	CliffQueue.RemoveAll([&IsRemoved](const FFenceEnd& End) { return IsRemoved(End.Panel); });
	WallRunQueue.RemoveAll([&IsRemoved](const FFenceEnd& End) { return IsRemoved(End.Panel); });

	for (FNatureSpill& Spill : SpillQueue)
	{
		Spill.Pieces.RemoveAll(IsRemoved);
	}

	SpillQueue.RemoveAll([](const FNatureSpill& Spill) { return Spill.Pieces.IsEmpty(); });

	for (FGroundSpill& Spill : GroundSpillQueue)
	{
		Spill.Pieces.RemoveAll(IsRemoved);
	}

	GroundSpillQueue.RemoveAll([](const FGroundSpill& Spill) { return Spill.Pieces.IsEmpty(); });

	for (const uint64 Key : RemovedKeys)
	{
		PlatformHealth.Remove(Key);
		DyingKeys.Remove(Key);
	}

	DyingQueue.RemoveAll([&RemovedKeys](const uint64 Key) { return RemovedKeys.Contains(Key); });

	const bool bFieldsChanged = Standing.ContainsByPredicate([](const FGridPlatform& Platform) { return IsFieldPiece(Platform); });

	if (CVarRoomManagerRemoveByRebuild.GetValueOnGameThread() != 0)
	{
		// Step A: everything derived worked out again from the list, and the visuals brought in line with it.
		RebuildPlatformState();
		SyncPieces();
		SyncSurfaceMarks();
	}
	else
	{
		// Step B: just what the platforms brought, undone.
		TArray<FGridSurfaceRef> ChangedSurfaces;
		TArray<FGridSurfaceRef> FlippedSurfaces;
		TSet<uint64> EdgeKeys;
		TSet<uint64> NodeKeys;

		RemovePlacedPlatforms(Indices, ChangedSurfaces, EdgeKeys, NodeKeys, FlippedSurfaces);
		RefreshCandidatesAfterRemoval(Standing, FlippedSurfaces);

		// A field going can shrink its landscape's mushroom, or split it in two.
		if (bFieldsChanged)
		{
			UpdateFieldAir(/*bRefreshLists=*/true);
		}

		UpdateVisualsAfterRemoval(Standing, EdgeKeys, NodeKeys, ChangedSurfaces);
	}

	if (bFieldsChanged)
	{
		RebuildAirLines();
	}

	if (bGroundTilesChanged && bShowZones)
	{
		RebuildDebugGrid();
	}

	// Whatever hung on them alone goes with them, at once: every platform left with no link to a ground tile. It was all
	// one piece with nothing else touching it, so taking it away cuts nothing more loose.
	if (bLinkToGroundTiles)
	{
		TArray<FGridPlatform> Loose;
		GatherCutLoose(Standing, Loose);

		if (!Loose.IsEmpty())
		{
			CutLooseRemovals += Loose.Num();
			RemovePlatforms(Loose);
		}
	}

	// With every platform gone, growth starts over: the middle zone's first tile, then every other zone's.
	if (BuiltPlatforms.IsEmpty())
	{
		QueueSeeds();
	}

	UpdateDebugReadouts();
	UpdateDeletionSummary();

	const int32 VerifyEvery = CVarRoomManagerVerifyEvery.GetValueOnGameThread();

	if (VerifyEvery > 0 && ++PlacementsSinceVerify >= VerifyEvery)
	{
		PlacementsSinceVerify = 0;
		VerifyIncrementalState();
	}
}

void ARoomManager::RemovePlacedPlatforms(
	const TConstArrayView<int32> Indices,
	TArray<FGridSurfaceRef>& OutChangedSurfaces,
	TSet<uint64>& OutEdgeKeys,
	TSet<uint64>& OutNodeKeys,
	TArray<FGridSurfaceRef>& OutFlippedSurfaces)
{
	OutChangedSurfaces.Reset();
	OutEdgeKeys.Reset();
	OutNodeKeys.Reset();
	OutFlippedSurfaces.Reset();

	if (Indices.IsEmpty())
	{
		return;
	}

	const int32 Module = GetModule();

	TArray<bool> bRemoved;
	bRemoved.Init(false, BuiltPlatforms.Num());

	TArray<FLatticeEdge> Edges;
	TArray<FIntVector> RimCells;
	TSet<uint64> RimCellKeys;

	// What each platform alone held goes: its module faces. Everything it shared is noted, to be worked out afresh from
	// what stands once they are all gone.
	for (const int32 Index : Indices)
	{
		bRemoved[Index] = true;

		const FGridPlatform& Platform = BuiltPlatforms[Index];
		const int32 NormalAxis = AxisIndex(Platform.Normal);
		const EGridFace FrontFace = MakeFace(NormalAxis, 1);
		const EGridFace BackFace = MakeFace(NormalAxis, -1);

		int32 SpanU, SpanV;
		GetPlatformSpans(Platform, SpanU, SpanV);

		ForEachModuleFace(Platform, SpanU, SpanV, Module, [&](const FIntVector& FaceMinNode)
		{
			ModuleFaceOwners.Remove(MakeModuleFaceKey(FaceMinNode, NormalAxis));
		});

		ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, bool)
		{
			bool bAlreadyNoted = false;
			OutEdgeKeys.Add(MakeEdgeKey(Edge), &bAlreadyNoted);

			if (!bAlreadyNoted)
			{
				Edges.Add(Edge);
			}
		});

		// Inside it as well as round it: a post there, of a wall hanging under water, can have been cut down at a seam.
		ForEachPlatformNode(Platform, SpanU, SpanV, Module, [&](const FIntVector& Node)
		{
			OutNodeKeys.Add(MakeCellKey(Node));
		});

		ForEachPlatformCell(Platform, SpanU, SpanV, [&](const FIntVector& Cell, const bool bRim)
		{
			// Its own two broad faces go with it, or are what another platform's rim makes them now.
			OutChangedSurfaces.Add(MakeSurfaceRef(Cell, FrontFace));
			OutChangedSurfaces.Add(MakeSurfaceRef(Cell, BackFace));

			if (bRim)
			{
				bool bAlreadyNoted = false;
				RimCellKeys.Add(MakeCellKey(Cell), &bAlreadyNoted);

				if (!bAlreadyNoted)
				{
					RimCells.Add(Cell);
				}
			}
		});
	}

	// The platforms that stay keep their order, and every index that names one is renumbered to match.
	TArray<int32> Renumbered;
	Renumbered.SetNumUninitialized(BuiltPlatforms.Num());

	TArray<FGridPlatform> KeptPlatforms;
	TArray<FFaceFill> KeptFills;
	KeptPlatforms.Reserve(BuiltPlatforms.Num() - Indices.Num());
	KeptFills.Reserve(FaceFills.Num());

	for (int32 Index = 0; Index < BuiltPlatforms.Num(); ++Index)
	{
		if (bRemoved[Index])
		{
			Renumbered[Index] = INDEX_NONE;
			continue;
		}

		Renumbered[Index] = KeptPlatforms.Add(BuiltPlatforms[Index]);

		if (FaceFills.IsValidIndex(GetFaceFillIndex(Index, -1)))
		{
			KeptFills.Add(MoveTemp(FaceFills[GetFaceFillIndex(Index, 1)]));
			KeptFills.Add(MoveTemp(FaceFills[GetFaceFillIndex(Index, -1)]));
		}
	}

	BuiltPlatforms = MoveTemp(KeptPlatforms);
	FaceFills = MoveTemp(KeptFills);

	for (TPair<uint64, int32>& FaceOwner : ModuleFaceOwners)
	{
		FaceOwner.Value = Renumbered[FaceOwner.Value];
	}

	for (auto Blocked = BlockedEdges.CreateIterator(); Blocked; ++Blocked)
	{
		const int32 Blocker = Renumbered[Blocked.Value()];

		if (Blocker == INDEX_NONE)
		{
			Blocked.RemoveCurrent();
		}
		else
		{
			Blocked.Value() = Blocker;
		}
	}

	// Nature by form, and the platforms standing in each path, likewise.
	auto RenumberAll = [&Renumbered](TArray<int32>& Named)
	{
		TArray<int32> Kept;
		Kept.Reserve(Named.Num());

		for (const int32 Index : Named)
		{
			if (Renumbered[Index] != INDEX_NONE)
			{
				Kept.Add(Renumbered[Index]);
			}
		}

		Named = MoveTemp(Kept);
	};

	NaturePlatformCount = 0;

	for (TArray<int32>& FormIndices : NatureIndices)
	{
		RenumberAll(FormIndices);
		NaturePlatformCount += FormIndices.Num();
	}

	GroundCount = NatureIndices[static_cast<int32>(ENatureForm::Ground)].Num();
	CliffCount = NatureIndices[static_cast<int32>(ENatureForm::Cliff)].Num();
	WaterCount = NatureIndices[static_cast<int32>(ENatureForm::Water)].Num();

	for (FAirPath& Path : AirPaths)
	{
		RenumberAll(Path.Standing);
	}

	// Every module edge and rim cell they took in, from whatever still stands around it.
	for (const FLatticeEdge& Edge : Edges)
	{
		RecomputeModuleEdge(Edge.Node, Edge.Axis);
	}

	for (const FIntVector& Cell : RimCells)
	{
		RecomputeRimCell(Cell);
	}

	++StructureRevision;
	bHasRemovals = true;

	PlatformCount = BuiltPlatforms.Num();
	HorizontalPlatformCount = Algo::CountIf(BuiltPlatforms, [](const FGridPlatform& Platform) { return Platform.GetKind() == EPlatformKind::SurfaceHorizontal; });
	VerticalPlatformCount = PlatformCount - HorizontalPlatformCount;

	// Promotion, undone and worked out again. A promotion rests on the surfaces flanking it, in its own plane and on its
	// own side, and through promotions beside it on theirs - so whatever hung on the platforms taken away is among the
	// promoted surfaces joined, that way, to their rim. All of those are let go, then promoted again wherever the rule
	// still holds, worked outward as a placement works it; and the surfaces round the rim, which may no longer be built
	// against, are asked too. The rule is monotone, so this lands where the rebuild's fixpoint does.
	TArray<FGridSurfaceRef> Seeds;
	Seeds.Reserve(RimCells.Num() * 12);

	for (const FIntVector& Cell : RimCells)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (const int32 Sign : { 1, -1 })
			{
				const EGridFace Face = MakeFace(Axis, Sign);
				Seeds.Add(MakeSurfaceRef(Cell, Face));
				Seeds.Add(MakeSurfaceRef(Cell + FaceStep(Face), OppositeFace(Face)));
			}
		}
	}

	TArray<FGridSurfaceRef> Demoted;
	TSet<uint64> DemotedKeys;

	for (const FGridSurfaceRef& Seed : Seeds)
	{
		const uint64 Key = MakeFaceKey(Seed.Cell, Seed.Face);

		if (PromotedFaces.Remove(Key) > 0)
		{
			Demoted.Add(Seed);
			DemotedKeys.Add(Key);
		}
	}

	for (int32 Index = 0; Index < Demoted.Num(); ++Index)
	{
		const FGridSurfaceRef Surface = Demoted[Index];

		for (int32 InPlaneAxis = 0; InPlaneAxis < 3; ++InPlaneAxis)
		{
			if (InPlaneAxis == FaceAxis(Surface.Face))
			{
				continue;
			}

			for (const int32 Sign : { 1, -1 })
			{
				const FIntVector Beside = Surface.Cell + AxisStep(InPlaneAxis, Sign);
				const uint64 Key = MakeFaceKey(Beside, Surface.Face);

				if (PromotedFaces.Remove(Key) > 0)
				{
					Demoted.Add(MakeSurfaceRef(Beside, Surface.Face));
					DemotedKeys.Add(Key);
				}
			}
		}
	}

	OutChangedSurfaces.Append(Seeds);
	OutChangedSurfaces.Append(Demoted);

	TArray<FGridSurfaceRef> PromotionQueue = Seeds;
	PromotionQueue.Append(Demoted);

	for (int32 QueueIndex = 0; QueueIndex < PromotionQueue.Num(); ++QueueIndex)
	{
		const FGridSurfaceRef Surface = PromotionQueue[QueueIndex];
		const uint64 FaceKey = MakeFaceKey(Surface.Cell, Surface.Face);

		if (PromotedFaces.Contains(FaceKey))
		{
			continue;
		}

		// As AddPlacedPlatform decides it: only an open rim surface is ever promoted.
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
		OutChangedSurfaces.Add(Surface);

		// Promoted where it was not before: it offers a fold no more.
		if (!DemotedKeys.Contains(FaceKey))
		{
			OutFlippedSurfaces.Add(Surface);
		}

		for (int32 InPlaneAxis = 0; InPlaneAxis < 3; ++InPlaneAxis)
		{
			if (InPlaneAxis != FaceAxis(Surface.Face))
			{
				PromotionQueue.Add(MakeSurfaceRef(Surface.Cell - AxisStep(InPlaneAxis), Surface.Face));
				PromotionQueue.Add(MakeSurfaceRef(Surface.Cell + AxisStep(InPlaneAxis), Surface.Face));
			}
		}
	}

	// Let go and not promoted again: it offers a fold once more.
	for (const FGridSurfaceRef& Surface : Demoted)
	{
		if (!PromotedFaces.Contains(MakeFaceKey(Surface.Cell, Surface.Face)))
		{
			OutFlippedSurfaces.Add(Surface);
		}
	}

	JournalSurfaceChanges(OutChangedSurfaces);
}

void ARoomManager::RecomputeModuleEdge(const FIntVector& Node, const int32 Axis)
{
	const int32 Module = GetModule();
	const FLatticeEdge Edge{ Node, Axis };
	const uint64 EdgeKey = MakeEdgeKey(Edge);

	// Every platform that takes the edge in covers one of the four module faces around it.
	FModuleFace Faces[4];
	GetEdgeFaces(Edge, Module, Faces);

	TArray<int32, TInlineAllocator<4>> Users;

	for (const FModuleFace& Face : Faces)
	{
		if (const int32* FaceOwner = ModuleFaceOwners.Find(MakeModuleFaceKey(Face.MinNode, Face.Normal)))
		{
			Users.AddUnique(*FaceOwner);
		}
	}

	// In list order, as the rebuild takes them in: the first that blocks it keeps it, and a floor blocks it only where
	// no platform before it used it.
	Users.Sort();

	uint8 Uses = 0;
	int32 RimUsers = 0;
	int32 Blocker = INDEX_NONE;
	bool bUsedBefore = false;

	for (const int32 User : Users)
	{
		const FGridPlatform& Platform = BuiltPlatforms[User];
		const int32 NormalAxis = AxisIndex(Platform.Normal);

		int32 SpanU, SpanV, U, V;
		GetPlatformSpans(Platform, SpanU, SpanV);
		GetInPlaneAxes(NormalAxis, U, V);

		// Round its rim, the edge lies on one of its two sides across the edge's run; anywhere else it is interior.
		const int32 AcrossAxis = Axis == U ? V : U;
		const int32 AcrossSpan = AcrossAxis == U ? SpanU : SpanV;
		const bool bRim = Node[AcrossAxis] == Platform.MinNode[AcrossAxis] || Node[AcrossAxis] == Platform.MinNode[AcrossAxis] + AcrossSpan;

		// As RecordPlatform takes it in: a later platform standing on a floor's block lifts it, unless it carries that
		// floor's fence run on.
		if (bRim && Blocker != INDEX_NONE)
		{
			const FGridPlatform& Blocking = BuiltPlatforms[Blocker];

			if (!Blocking.IsWall() && !IsRunContinuation(Platform, Blocking))
			{
				Blocker = INDEX_NONE;
			}
		}

		if (bRim && Blocker == INDEX_NONE && !IsLoadBearing(Platform))
		{
			if (Platform.IsWall())
			{
				const int32 UpAxis = FaceAxis(Platform.Up);
				const int32 TopCoordinate = Platform.MinNode[UpAxis] + (FaceSign(Platform.Up) > 0 ? (UpAxis == U ? SpanU : SpanV) : 0);
				Blocker = Axis != UpAxis && Node[UpAxis] == TopCoordinate ? User : INDEX_NONE;
			}
			else if (!bUsedBefore)
			{
				Blocker = User;
			}
		}

		Uses |= bRim ? RimEdgeUse : InteriorEdgeUse;
		RimUsers += bRim ? 1 : 0;
		bUsedBefore = true;
	}

	if (Uses != 0)
	{
		ModuleEdgeUses.Add(EdgeKey, Uses);
	}
	else
	{
		ModuleEdgeUses.Remove(EdgeKey);
	}

	if (RimUsers > 0)
	{
		RimEdgeUsers.Add(EdgeKey, RimUsers);
	}
	else
	{
		RimEdgeUsers.Remove(EdgeKey);
	}

	if (Blocker != INDEX_NONE)
	{
		BlockedEdges.Add(EdgeKey, Blocker);
	}
	else
	{
		BlockedEdges.Remove(EdgeKey);
	}
}

void ARoomManager::RecomputeRimCell(const FIntVector& Cell)
{
	const int32 Module = GetModule();
	uint8 Normals = 0;

	// In each lattice plane it lies on, the module faces it touches - one, two or four - and of the platforms covering
	// them, any with the cell on its rim.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Cell[Axis] % Module != 0)
		{
			continue;
		}

		int32 U, V;
		GetInPlaneAxes(Axis, U, V);

		const int32 FirstModuleU = Cell[U] % Module == 0 ? Cell[U] / Module - 1 : Cell[U] / Module;
		const int32 FirstModuleV = Cell[V] % Module == 0 ? Cell[V] / Module - 1 : Cell[V] / Module;

		for (int32 ModuleV = FMath::Max(FirstModuleV, 0); ModuleV <= Cell[V] / Module && (Normals & AxisBit(Axis)) == 0; ++ModuleV)
		{
			for (int32 ModuleU = FMath::Max(FirstModuleU, 0); ModuleU <= Cell[U] / Module; ++ModuleU)
			{
				FIntVector FaceMinNode = Cell;
				FaceMinNode[U] = ModuleU * Module;
				FaceMinNode[V] = ModuleV * Module;

				const int32* FaceOwner = ModuleFaceOwners.Find(MakeModuleFaceKey(FaceMinNode, Axis));

				if (!FaceOwner)
				{
					continue;
				}

				const FGridPlatform& Platform = BuiltPlatforms[*FaceOwner];

				int32 SpanU, SpanV;
				GetPlatformSpans(Platform, SpanU, SpanV);

				if (Cell[U] == Platform.MinNode[U] || Cell[U] == Platform.MinNode[U] + SpanU
					|| Cell[V] == Platform.MinNode[V] || Cell[V] == Platform.MinNode[V] + SpanV)
				{
					Normals |= AxisBit(Axis);
					break;
				}
			}
		}
	}

	if (Normals != 0)
	{
		RimCellNormals.Add(MakeCellKey(Cell), Normals);
	}
	else
	{
		RimCellNormals.Remove(MakeCellKey(Cell));
	}
}

void ARoomManager::RefreshCandidatesAfterRemoval(const TConstArrayView<FGridPlatform> Removed, const TConstArrayView<FGridSurfaceRef> FlippedSurfaces)
{
	// The empty cube has a list of its own, and lists going to be gathered afresh anyway need nothing.
	if (!CanRefreshCandidateLists() || BuiltPlatforms.IsEmpty())
	{
		bPlatformCandidatesStale = true;
		return;
	}

	const int32 Module = GetModule();

	// A few hundred placements are judged round each platform taken away, so the sets are sized for them up front.
	TArray<TSet<uint64>> Judged;
	Judged.SetNum(TileCandidates.Num());

	for (TSet<uint64>& TileJudged : Judged)
	{
		TileJudged.Reserve(Removed.Num() * 256);
	}

	auto Judge = [&](const int32 TileIndex, const FGridPlatform& Candidate, bool)
	{
		bool bAlreadyJudged = false;
		Judged[TileIndex].Add(MakeCandidateKey(Candidate), &bAlreadyJudged);

		if (bAlreadyJudged)
		{
			return;
		}

		// Room has come free, so a placement not listed is judged as well as every listed one. But only one meeting what
		// stands along its rim is ever open, and round platforms taken away many meet nothing any more - all of them round
		// a piece cut loose. Those are let go from the lists, or left out, without being judged in full.
		if (MeetsStructure(Candidate))
		{
			RefreshPlatformCandidate(TileIndex, Candidate, /*bMayHaveComeFree=*/true);
		}
		else
		{
			UnlistPlatformCandidate(TileIndex, Candidate);
		}
	};

	// Every placement taking in a module edge of theirs: one that stood where they stood, met them - flush or not -
	// folded off them, grew off them alone, or was held off by a beam one of them blocked.
	TSet<uint64> Visited;

	for (const FGridPlatform& Platform : Removed)
	{
		int32 SpanU, SpanV;
		GetPlatformSpans(Platform, SpanU, SpanV);

		ForEachModuleEdge(Platform, SpanU, SpanV, Module, [&](const FLatticeEdge& Edge, bool)
		{
			bool bAlreadyVisited = false;
			Visited.Add(MakeEdgeKey(Edge), &bAlreadyVisited);

			if (!bAlreadyVisited)
			{
				ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/false, Judge);
			}
		});
	}

	// And every placement folding off a run whose promotion has changed: it offers a fold now, or no longer does.
	for (const FGridSurfaceRef& Surface : FlippedSurfaces)
	{
		FLatticeEdge Edge;
		bool bAlreadyVisited = false;

		if (GetRunEdge(Surface.Cell, Module, Edge))
		{
			Visited.Add(MakeEdgeKey(Edge), &bAlreadyVisited);

			if (!bAlreadyVisited)
			{
				ForEachPlacementAround(Edge, UsableTiles, Module, /*bRimOnly=*/true, Judge);
			}
		}
	}
}

void ARoomManager::UpdateVisualsAfterRemoval(
	const TConstArrayView<FGridPlatform> Removed,
	const TSet<uint64>& EdgeKeys,
	const TSet<uint64>& NodeKeys,
	const TConstArrayView<FGridSurfaceRef> ChangedSurfaces)
{
	// Anything but the plain case is left to the full sync, which takes away whatever is no longer wanted.
	if (!ArePiecesInStep())
	{
		SyncPieces();
	}
	else
	{
		// Each platform's own pieces: what filled its opening.
		for (const FGridPlatform& Platform : Removed)
		{
			const uint64 PlatformKey = MakePlatformKey(Platform);

			for (FStructureLayer& Layer : StructureLayers)
			{
				if (Layer.Structure == Platform.PositiveStructure || Layer.Structure == Platform.NegativeStructure || Layer.Structure == Platform.NatureCover)
				{
					RemovePieceFromLayer(Layer.Pieces, PlatformKey);
					RemovePieceFromLayer(Layer.Pieces, PlatformKey | NegativeFacePieceBit);
				}
			}
		}

		// The beams and posts they took in, each as it now is: gone where nothing uses it any more, back where the
		// platform that blocked it was taken away, and whole again where water going ended a seam.
		RefreshLatticePieces(EdgeKeys, NodeKeys);
	}

	UpdateSurfaceMarks(ChangedSurfaces);
}
