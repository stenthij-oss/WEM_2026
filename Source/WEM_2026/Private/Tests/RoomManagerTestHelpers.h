// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GridMath.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "PlatformStructureData.h"
#include "PlatformTileData.h"
#include "RoomManager.h"
#include "StructureCategory.h"
#include "Templates/Function.h"
#include "UObject/Package.h"

/** What the tests read of the room manager's private state. Befriended by ARoomManager. */
struct FRoomManagerTestAccess
{
	static void EnsureCandidates(ARoomManager& Manager)
	{
		Manager.EnsurePlatformCandidates();
	}

	static bool IsListed(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		for (const ARoomManager::FTileCandidates& Candidates : Manager.TileCandidates)
		{
			if (Candidates.Tile != Platform.Tile)
			{
				continue;
			}

			for (const TArray<FGridPlatform>& ByKind : Candidates.ByKind)
			{
				for (const FGridPlatform& Listed : ByKind)
				{
					if (Listed.MinNode == Platform.MinNode && Listed.Normal == Platform.Normal && Listed.bTurned == Platform.bTurned)
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	static TArray<FGridPlatform> GetListed(const ARoomManager& Manager)
	{
		TArray<FGridPlatform> Listed;
		for (const ARoomManager::FTileCandidates& Candidates : Manager.TileCandidates)
		{
			for (const TArray<FGridPlatform>& ByKind : Candidates.ByKind)
			{
				Listed.Append(ByKind);
			}
		}

		return Listed;
	}

	/** Runs the wholesale check and returns how many checks have drifted so far. */
	static int32 Verify(ARoomManager& Manager)
	{
		Manager.VerifyIncrementalState();
		return Manager.FailedVerifications;
	}

	static int32 GetFailedVerifications(const ARoomManager& Manager)
	{
		return Manager.FailedVerifications;
	}

	static int32 GetVerifiedPlacements(const ARoomManager& Manager)
	{
		return Manager.VerifiedPlacements;
	}

	static bool IsStale(const ARoomManager& Manager)
	{
		return Manager.bPlatformCandidatesStale;
	}

	/** The platforms that stand, in the order they were taken in. */
	static const TArray<FGridPlatform>& GetBuilt(const ARoomManager& Manager)
	{
		return Manager.BuiltPlatforms;
	}

	/** Replaces the platform list and rebuilds everything from it, as loading a level would. */
	static void Rebuild(ARoomManager& Manager, const TArray<FGridPlatform>& Platforms)
	{
		Manager.Platforms = Platforms;
		Manager.RebuildPlatformState();
	}

	/** The faces beside one face of a built platform, as face indices (platform * 2, + 1 for its negative face), sorted. */
	static TArray<int32> GetFaceNeighbours(const ARoomManager& Manager, const int32 PlatformIndex, const int32 FaceSign)
	{
		TArray<int32, TInlineAllocator<16>> Neighbours;
		Manager.GatherFaceNeighbours(Manager.BuiltPlatforms[PlatformIndex], PlatformIndex, FaceSign, Neighbours);

		TArray<int32> Sorted(Neighbours);
		Sorted.Sort();
		return Sorted;
	}

	/** Places a platform as it is, fill and all, the way a beat does once it has drawn one - fence ends included. */
	static void Place(ARoomManager& Manager, const FGridPlatform& Platform)
	{
		Manager.PlacePlatform(Platform, 0);
	}

	/** How many fence ends wait to grow. */
	static int32 GetWaitingFenceEnds(const ARoomManager& Manager)
	{
		return Manager.FenceQueue.Num();
	}

	/** How many cliff ends wait to carry their run on. */
	static int32 GetWaitingCliffEnds(const ARoomManager& Manager)
	{
		return Manager.CliffQueue.Num();
	}

	/** Draws whether a platform just placed starts a patch of ground spilling out, as a growth beat does. */
	static void QueueGroundSpill(ARoomManager& Manager, const FGridPlatform& Platform)
	{
		Manager.QueueGroundSpill(Platform);
	}

	/** One ground spill beat. False when no patch had anywhere to go. */
	static bool PlaceGroundSpillPiece(ARoomManager& Manager)
	{
		return Manager.PlaceGroundSpillPiece();
	}

	/** How many patches of the city's ground are still spilling out, and how many pieces the oldest may still lay. */
	static int32 GetGroundSpills(const ARoomManager& Manager, int32& OutOldestRemaining)
	{
		OutOldestRemaining = Manager.GroundSpillQueue.IsEmpty() ? 0 : Manager.GroundSpillQueue[0].Remaining;
		return Manager.GroundSpillQueue.Num();
	}

	/** Draws whether a wall just placed starts a run of walls round the outline of grounds, as a growth beat does. */
	static void QueueWallRun(ARoomManager& Manager, const FGridPlatform& Wall)
	{
		Manager.QueueWallRun(Wall);
	}

	/** How many ends of runs of walls wait to carry on. */
	static int32 GetWaitingWallRunEnds(const ARoomManager& Manager)
	{
		return Manager.WallRunQueue.Num();
	}

	/** Whether a wall stands wholly on the outline of the city's grounds of one side, its face FaceSign over them. */
	static bool StandsOnGroundOutline(const ARoomManager& Manager, const FGridPlatform& Wall, const int32 FaceSign, const EStructureSide Side)
	{
		return Manager.StandsOnGroundOutline(Wall, FaceSign, Side);
	}

	/** Whether a cliff about to be placed stands wholly on the outline of ground or water. */
	static bool StandsOnOutline(const ARoomManager& Manager, const FGridPlatform& Cliff, const int32 NatureSign)
	{
		return Manager.StandsOnOutline(Cliff, NatureSign);
	}

	/** The Up a placement would take if it were drawn now. Draws from the role stream, as a beat would. */
	static EGridFace ChooseUp(ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.ChooseUp(Platform);
	}

	/** Fills a platform as a beat would, from the structure stream. False when nothing fits. */
	static bool DrawFill(ARoomManager& Manager, FGridPlatform& Platform)
	{
		return Manager.DrawFill(Platform, /*bLoadBearingOnly=*/false);
	}

	/** Which faces of a wall about to be placed look over an interior ground it stands on: 1 positive, 2 negative. */
	static uint8 GetFacesOverInteriorGround(const ARoomManager& Manager, const FGridPlatform& Wall)
	{
		return Manager.GetFacesOverInteriorGround(Wall);
	}

	/** The axis a fence panel's run goes along. */
	static int32 GetRunAxis(const ARoomManager& Manager, const FGridPlatform& Panel)
	{
		return Manager.GetRunAxis(Panel);
	}

	/** Where a run could carry on from one end of a panel - straight on, and round each corner. */
	static void GatherFenceContinuations(const ARoomManager& Manager, const FGridPlatform& Panel, const int32 Side, FGridPlatform& OutStraight, TArray<FGridPlatform>& OutTurns)
	{
		TArray<FGridPlatform, TInlineAllocator<2>> Turns;
		Manager.GatherFenceContinuations(ARoomManager::FFenceEnd{ Panel, Side }, OutStraight, Turns);
		OutTurns = Turns;
	}

	/** The chance a drawn placement is drawn again for going against a growth rule. */
	static float GetRuleRedrawChance(const ARoomManager& Manager, const FGridPlatform& Candidate)
	{
		return Manager.GetRuleRedrawChance(Candidate);
	}

	/** Whether a floor runs along a wall's edge across its Up at Coordinate, on the side Side of its plane. */
	static bool HasFloorAlong(const ARoomManager& Manager, const FGridPlatform& Wall, const int32 Coordinate, const int32 Side)
	{
		return Manager.HasFloorAlong(Wall, Coordinate, Side);
	}

	/** Whether a platform stands on something, as a fence beat asks it. */
	static bool StandsOnSomething(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.StandsOnSomething(Platform);
	}

	/** Fills a platform with nature of one form on the face NatureSign looks out from, and urban behind it, from the structure stream. */
	static bool DrawNatureFill(ARoomManager& Manager, FGridPlatform& Platform, const ENatureForm Form, const int32 NatureSign)
	{
		return Manager.DrawNatureFill(Platform, Form, NatureSign, Manager.StructureStream);
	}

	/** Whether every face a filled platform about to be placed would meet is one the nature on either side takes. */
	static bool AreNatureMeetingsLegal(const ARoomManager& Manager, const FGridPlatform& Platform, bool& bOutNatureMet)
	{
		uint8 NatureMeets = 0;
		const bool bLegal = Manager.AreNatureMeetingsLegal(Platform, &NatureMeets);
		bOutNatureMet = NatureMeets != 0;
		return bLegal;
	}

	/** Whether a placement not yet filled would meet water or a cliff, and which of its faces would meet ground (1 positive, 2 negative). */
	static bool MeetsClosedNature(const ARoomManager& Manager, const FGridPlatform& Platform, uint8& OutFacesOnGround)
	{
		return Manager.MeetsClosedNature(Platform, &OutFacesOnGround);
	}

	static int32 GetUsableNatureCount(const ARoomManager& Manager)
	{
		return Manager.UsableNatureStructures.Num();
	}

	/** The air kept open above ground and water, as inclusive boxes of cells, in the order it was laid. */
	static TArray<TPair<FIntVector, FIntVector>> GetAirBoxes(const ARoomManager& Manager)
	{
		TArray<TPair<FIntVector, FIntVector>> Boxes;
		for (const ARoomManager::FAirBox& Box : Manager.AirBoxes)
		{
			Boxes.Emplace(Box.Min, Box.Max);
		}

		return Boxes;
	}

	/** One pocket of air as the tests read it. */
	struct FPocket
	{
		EAirPocketShape Shape = EAirPocketShape::Sphere;
		FVector Centre = FVector::ZeroVector;
		FVector HalfSize = FVector::ZeroVector;
		int32 Axis = 2;
		int32 AxisSign = 1;
		int32 CutPieces = 0;
		bool bMold = false;
		EMoldShape MoldShape = EMoldShape::Tower;
		int32 Parts = 0;
		double OpenFloor = 0.0;

		/** Its reach, in cells, as far as its warp moves it. */
		FVector Min = FVector::ZeroVector;
		FVector Max = FVector::ZeroVector;

		/** Whether its noise moves its outline - a mold's box or its shape - and whether a mold's shape runs out through its top. */
		bool bWarped = false;
		bool bOpenTop = false;
	};

	static TArray<FPocket> GetAirPockets(const ARoomManager& Manager)
	{
		TArray<FPocket> Pockets;
		for (const ARoomManager::FAirPocket& Pocket : Manager.AirPockets)
		{
			FPocket& Read = Pockets.Add_GetRef(FPocket{ Pocket.Shape, Pocket.Centre, Pocket.HalfSize, Pocket.Axis, Pocket.AxisSign,
				Pocket.FaceKeys.Num() + Pocket.EdgeKeys.Num() + Pocket.NodeKeys.Num(), Pocket.bMold, Pocket.MoldShape, Pocket.Parts.Num(), Pocket.OpenFloor });
			Manager.GetPocketBounds(Pocket, Read.Min, Read.Max);
			Read.bWarped = !Pocket.Warp.IsEmpty() || !Pocket.ShapeWarp.IsEmpty();
			Read.bOpenTop = Pocket.bOpenTop;
		}

		return Pockets;
	}

	/** Whether a cell's centre lies inside a pocket of the given shape and measures. */
	static bool IsCellInPocket(const ARoomManager& Manager, const FPocket& Pocket, const FIntVector& Cell)
	{
		ARoomManager::FAirPocket Shape;
		Shape.Shape = Pocket.Shape;
		Shape.Centre = Pocket.Centre;
		Shape.HalfSize = Pocket.HalfSize;
		Shape.Axis = Pocket.Axis;
		Shape.AxisSign = Pocket.AxisSign;
		return Manager.IsCellInPocket(Shape, Cell);
	}

	/** Whether a cell's centre lies inside the pocket or mold drawn at Index, as it holds back growth. */
	static bool IsCellInAirPocket(const ARoomManager& Manager, const int32 Index, const FIntVector& Cell)
	{
		return Manager.IsCellInPocket(Manager.AirPockets[Index], Cell);
	}

	/** The shapes the mold drawn at Index leaves open, from its floor up: shape, start, end and half-width. */
	static TArray<TTuple<EMoldShape, double, double, double>> GetMoldParts(const ARoomManager& Manager, const int32 Index)
	{
		TArray<TTuple<EMoldShape, double, double, double>> Parts;
		for (const ARoomManager::FAirPocket::FMoldPart& Part : Manager.AirPockets[Index].Parts)
		{
			Parts.Emplace(Part.Shape, Part.Start, Part.End, Part.Half);
		}

		return Parts;
	}

	/** Whether a cell's centre lies inside one of the shapes the mold drawn at Index leaves open. */
	static bool IsCellInMoldShape(const ARoomManager& Manager, const int32 Index, const FIntVector& Cell)
	{
		return Manager.IsCellInMoldShape(Manager.AirPockets[Index], Cell);
	}

	/** Whether a cell's centre lies in the box of the mold drawn at Index as its warp moves it, held back or open. */
	static bool IsCellInMoldBox(const ARoomManager& Manager, const int32 Index, const FIntVector& Cell)
	{
		const ARoomManager::FAirPocket& Mold = Manager.AirPockets[Index];
		FVector Offset = FVector(Cell) + FVector(0.5) - Mold.Centre;
		Offset += ARoomManager::DampWarp(Mold, Manager.SamplePocketShift(Mold, Cell).Box, Offset);

		return FMath::Abs(Offset[(Mold.Axis + 1) % 3]) <= Mold.HalfSize.X
			&& FMath::Abs(Offset[(Mold.Axis + 2) % 3]) <= Mold.HalfSize.Y
			&& FMath::Abs(Offset[Mold.Axis]) <= Mold.HalfSize.Z;
	}

	/** How many lattice pieces the obstacles hold back - the objects'. */
	static int32 GetObstaclePieceCount(const ARoomManager& Manager)
	{
		return Manager.ObstacleFaces.Num() + Manager.ObstacleEdges.Num() + Manager.ObstacleNodes.Num();
	}

	/** How many lattice pieces the pockets and molds hold back, each once. */
	static int32 GetPocketPieceCount(const ARoomManager& Manager)
	{
		return Manager.PocketFaces.Num() + Manager.PocketEdges.Num() + Manager.PocketNodes.Num();
	}

	/** Whether a platform keeps clear of every pocket and every mold's held-back box. */
	static bool IsClearOfPockets(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.IsClearOfPockets(Platform);
	}

	/** Whether a platform keeps clear of all the air held back: paths, pockets, molds and the air over the fields. */
	static bool IsClearOfHeldAir(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.IsClearOfHeldAir(Platform);
	}

	/** Whether a platform crosses air held back, so a hit does it more damage. */
	static bool IsVulnerable(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.IsVulnerable(Platform);
	}

	/** Breaks the pocket or mold at Index down and forms it again elsewhere, all at once. */
	static void MovePocket(ARoomManager& Manager, const int32 Index)
	{
		Manager.MovePocket(Index);
	}

	/** Starts moving the pocket or mold at Index, and steps the move a slab, as the clock does a tick. */
	static void StartPocketMove(ARoomManager& Manager, const int32 Index)
	{
		Manager.MovingPocket = Index;
		Manager.bPocketForming = false;
	}

	static void StepPocketMove(ARoomManager& Manager)
	{
		Manager.StepPocketMove();
	}

	/** Cuts the pieces of the pocket or mold drawn at Index afresh, every slab, as the lattice stands: what drawing one costs. */
	static void CutPocketPieces(ARoomManager& Manager, const int32 Index)
	{
		Manager.CutPocketPieces(Manager.AirPockets[Index]);
	}

	static bool IsPocketMoving(const ARoomManager& Manager)
	{
		return Manager.MovingPocket != INDEX_NONE;
	}

	/** The zone a platform is in, and the cells a zone spans. */
	static int32 GetZoneOf(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.GetZoneOf(Platform);
	}

	static TPair<FIntVector, FIntVector> GetZoneBox(const ARoomManager& Manager, const int32 Zone)
	{
		const ARoomManager::FAirBox Box = Manager.GetZoneBox(Zone);
		return TPair<FIntVector, FIntVector>(Box.Min, Box.Max);
	}

	/** One strike, leaving those with no health waiting, as the clock strikes. */
	static void Strike(ARoomManager& Manager)
	{
		Manager.Strike();
	}

	/** Takes away up to Most of the platforms waiting with no health, as the clock does a few a tick. */
	static void RemoveDying(ARoomManager& Manager, const int32 Most)
	{
		Manager.RemoveDying(Most);
	}

	static int32 GetDyingCount(const ARoomManager& Manager)
	{
		return Manager.DyingQueue.Num();
	}

	/** A platform's health, or a negative number when it has none. */
	static float GetHealth(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		const float* Health = Manager.PlatformHealth.Find(WEMGrid::MakeTaggedKey(Platform.MinNode, static_cast<int32>(Platform.Normal)));
		return Health ? *Health : -1.0f;
	}

	/** Takes platforms away, as a strike or a reshuffle does. */
	static void Remove(ARoomManager& Manager, const TArray<FGridPlatform>& Platforms)
	{
		Manager.RemovePlatforms(Platforms);
	}

	/** Starts a reshuffle, and steps it, as the clock does. */
	static void BeginReshuffle(ARoomManager& Manager)
	{
		Manager.BeginReshuffle();
	}

	static bool StepReshuffle(ARoomManager& Manager)
	{
		return Manager.StepReshuffle();
	}

	/** The zone the reshuffle under way keeps to, or INDEX_NONE, and the platforms it still has to move. */
	static int32 GetReshuffleZone(const ARoomManager& Manager, int32& OutWaiting)
	{
		OutWaiting = Manager.ReshuffleKeys.Num();
		return Manager.ReshuffleZoneIndex;
	}

	/** Whether a placement's middle lies in a mold's box. */
	static bool IsInMoldBox(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.IsInMoldBox(Platform);
	}

	/** One firing of the placement timer, as play does it, with the beats last caught up to SecondsAgo seconds back. */
	static void AdvancePlacement(ARoomManager& Manager, const double SecondsAgo)
	{
		Manager.BeatClock = Manager.GetClock() - SecondsAgo;
		Manager.AdvancePlacement();
	}

	/** The zones still waiting for a first tile. */
	static int32 GetWaitingSeeds(const ARoomManager& Manager)
	{
		return Manager.SeedQueue.Num();
	}

	/** Whether a platform standing is a ground tile, and the ground tiles standing, in list order. */
	static bool IsGroundTile(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.GroundTiles.Contains(WEMGrid::MakeTaggedKey(Platform.MinNode, static_cast<int32>(Platform.Normal)));
	}

	static TArray<FGridPlatform> GetGroundTiles(const ARoomManager& Manager)
	{
		return Manager.BuiltPlatforms.FilterByPredicate([&Manager](const FGridPlatform& Platform) { return IsGroundTile(Manager, Platform); });
	}

	/** How many ground tiles a zone has. */
	static int32 GetGroundTileCount(const ARoomManager& Manager, const int32 Zone)
	{
		return Manager.GroundTileCounts[Zone];
	}

	/** How many platforms standing link to no ground tile, by a search of the whole structure. */
	static int32 CountLoose(const ARoomManager& Manager)
	{
		TArray<int32> Loose;
		Manager.GatherAllLoose(Loose);
		return Loose.Num();
	}

	/** The platforms sharing a beam with the one at Index in the built list, as indices into it. */
	static TArray<int32> GetLinked(const ARoomManager& Manager, const int32 Index)
	{
		TArray<int32, TInlineAllocator<8>> Touching;
		Manager.GatherTouchingPlatforms(Manager.BuiltPlatforms[Index], Touching);
		Touching.Remove(Index);
		return TArray<int32>(Touching);
	}

	/** Platforms taken away for losing their link, and ground tiles passed on, since the cube was cleared. */
	static int32 GetCutLoose(const ARoomManager& Manager)
	{
		return Manager.CutLooseRemovals;
	}

	static int32 GetGroundTilesPassedOn(const ARoomManager& Manager)
	{
		return Manager.GroundTilesPassedOn;
	}

	/** One path of air as drawn: its corners and the box of cells each leg fills. */
	struct FPath
	{
		TArray<FIntVector> Corners;
		TArray<FIntVector> LegMins;
		TArray<FIntVector> LegMaxes;
	};

	static TArray<FPath> GetPaths(const ARoomManager& Manager)
	{
		TArray<FPath> Paths;
		for (const ARoomManager::FAirPath& Path : Manager.AirPaths)
		{
			Paths.Add(FPath{ Path.Corners, Path.LegMins, Path.LegMaxes });
		}

		return Paths;
	}

	/** How many module faces, edges and nodes the paths hold back. */
	static void GetPathPieceCounts(const ARoomManager& Manager, int32& OutFaces, int32& OutEdges, int32& OutNodes)
	{
		OutFaces = Manager.PathFaces.Num();
		OutEdges = Manager.PathEdges.Num();
		OutNodes = Manager.PathNodes.Num();
	}

	/** The axis each module face a path holds back looks along. */
	static TArray<int32> GetPathFaceAxes(const ARoomManager& Manager)
	{
		TArray<int32> Axes;
		for (const TPair<uint64, int32>& Piece : Manager.PathFaces)
		{
			Axes.Add(static_cast<int32>(Piece.Key >> WEMGrid::KeyTagShift) - 1);
		}

		return Axes;
	}

	static bool IsClearOfPaths(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.IsClearOfPaths(Platform);
	}

	/** Draws one path afresh, as the path clock does when it falls due. */
	static void RedrawPath(ARoomManager& Manager, const int32 Index)
	{
		Manager.RedrawPath(Index);
	}

	/** The platforms standing in any path, as the paths found them when drawn, each once. */
	static int32 GetPathStandingCount(const ARoomManager& Manager)
	{
		TSet<int32> Standing;
		for (const ARoomManager::FAirPath& Path : Manager.AirPaths)
		{
			Standing.Append(Path.Standing);
		}

		return Standing.Num();
	}

	/** When each path next falls due. */
	static TArray<double> GetPathRedrawTimes(const ARoomManager& Manager)
	{
		TArray<double> Times;
		for (const ARoomManager::FAirPath& Path : Manager.AirPaths)
		{
			Times.Add(Path.RedrawAt);
		}

		return Times;
	}

	/** Gathers the candidate lists afresh and returns how long it took, in seconds. */
	static double TimeCandidateGather(ARoomManager& Manager)
	{
		const double Start = FPlatformTime::Seconds();
		Manager.EnsurePlatformCandidates();
		return FPlatformTime::Seconds() - Start;
	}

	/** Whether a piece of ground or water about to be placed would have its fewest storeys of air clear. */
	static bool IsAirClear(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.IsAirClear(Platform);
	}

	/** How many fields of nature are still spilling out, and how many pieces the oldest may still lay. */
	static int32 GetSpillingFields(const ARoomManager& Manager, int32& OutOldestRemaining)
	{
		OutOldestRemaining = Manager.SpillQueue.IsEmpty() ? 0 : Manager.SpillQueue[0].Remaining;
		return Manager.SpillQueue.Num();
	}

	/** How many pairs of faces among the platforms that stand meet where the nature on either side does not take the other. */
	static int32 CountIllegalNatureMeetings(const ARoomManager& Manager)
	{
		int32 Illegal = 0;

		for (int32 Index = 0; Index < Manager.BuiltPlatforms.Num(); ++Index)
		{
			const FGridPlatform& Platform = Manager.BuiltPlatforms[Index];

			for (const int32 OwnSign : { 1, -1 })
			{
				TArray<int32, TInlineAllocator<16>> Neighbours;
				Manager.GatherFaceNeighbours(Platform, Index, OwnSign, Neighbours);

				// Face indices run platform * 2, + 1 for its negative face.
				for (const int32 Face : Neighbours)
				{
					Illegal += ARoomManager::AcceptsMeeting(Platform, OwnSign, Manager.BuiltPlatforms[Face / 2], Face % 2 == 0 ? 1 : -1) ? 0 : 1;
				}
			}
		}

		return Illegal;
	}

	/** The instances a structure's layer holds, and the mesh and material on its first chunk. Zero when it has no layer. */
	static int32 GetStructureLayerPieces(const ARoomManager& Manager, const UPlatformStructureData* Structure, UStaticMesh*& OutMesh, UMaterialInterface*& OutMaterial)
	{
		OutMesh = nullptr;
		OutMaterial = nullptr;

		for (const FStructureLayer& Layer : Manager.StructureLayers)
		{
			if (Layer.Structure != Structure || Layer.Pieces.Chunks.IsEmpty())
			{
				continue;
			}

			int32 Pieces = 0;
			for (const UInstancedStaticMeshComponent* Chunk : Layer.Pieces.Chunks)
			{
				Pieces += Chunk ? Chunk->GetInstanceCount() : 0;
			}

			OutMesh = Layer.Pieces.Chunks[0]->GetStaticMesh();
			OutMaterial = Layer.Pieces.Chunks[0]->GetMaterial(0);
			return Pieces;
		}

		return 0;
	}

	/** Moves every piece to the chunks of its side of the AI overlay, as a change to wem.AIOverlay.SegmentPercent in play does. How many moved. */
	static int32 SortPiecesForOverlay(ARoomManager& Manager)
	{
		return Manager.SortPiecesForOverlay();
	}

	/** How many cells a built platform spans along its first and second in-plane axes. */
	static void GetSpans(const ARoomManager& Manager, const FGridPlatform& Platform, int32& OutSpanU, int32& OutSpanV)
	{
		Manager.GetPlatformSpans(Platform, OutSpanU, OutSpanV);
	}

	/** Every piece drawn, with the chunk it is drawn in and whether it fills an opening rather than being a beam or post. */
	static void ForEachDrawnPiece(const ARoomManager& Manager, TFunctionRef<void(uint64 Key, const UInstancedStaticMeshComponent& Chunk, bool bFill)> Visit)
	{
		const auto VisitLayer = [&Visit](const FRoomPieceLayer& Layer, const bool bFill)
		{
			for (int32 ChunkIndex = 0; ChunkIndex < Layer.Chunks.Num(); ++ChunkIndex)
			{
				if (Layer.Chunks[ChunkIndex] && Layer.ChunkKeys.IsValidIndex(ChunkIndex))
				{
					for (const uint64 Key : Layer.ChunkKeys[ChunkIndex])
					{
						Visit(Key, *Layer.Chunks[ChunkIndex], bFill);
					}
				}
			}
		};

		VisitLayer(Manager.EdgeLayer, false);
		VisitLayer(Manager.NodeLayer, false);

		for (const FStructureLayer& Layer : Manager.StructureLayers)
		{
			VisitLayer(Layer.Pieces, true);
		}
	}

	/** As WaterSeamPieceBit in RoomManager.cpp: a beam or post cut down at a seam in water, and the water over it. */
	static constexpr uint64 WaterSeamBit = uint64(1) << 62;

	static int32 GetModule(const ARoomManager& Manager)
	{
		return Manager.GetModule();
	}

	/** Brings every piece in line with the platforms, as a rebuild does. How many it had to add or take away. */
	static int32 SyncPieces(ARoomManager& Manager)
	{
		return Manager.SyncPieces();
	}

	/** How many beams and posts are drawn, and how many of each are cut down at a seam in a sheet of water. */
	static void GetLatticePieceCounts(const ARoomManager& Manager, int32& OutEdges, int32& OutCutEdges, int32& OutNodes, int32& OutCutNodes)
	{
		OutEdges = Manager.EdgeLayer.PieceSlots.Num();
		OutNodes = Manager.NodeLayer.PieceSlots.Num();
		OutCutEdges = 0;
		OutCutNodes = 0;

		for (const TPair<uint64, FIntPoint>& Piece : Manager.EdgeLayer.PieceSlots)
		{
			OutCutEdges += (Piece.Key & WaterSeamBit) != 0 ? 1 : 0;
		}

		for (const TPair<uint64, FIntPoint>& Piece : Manager.NodeLayer.PieceSlots)
		{
			OutCutNodes += (Piece.Key & WaterSeamBit) != 0 ? 1 : 0;
		}
	}

	/** The box one piece of a layer fills, from its instance on the unit cube. False when the layer has no such piece. */
	static bool GetPieceBox(const FRoomPieceLayer& Layer, const uint64 Key, FBox& OutBox)
	{
		const FIntPoint* Slot = Layer.PieceSlots.Find(Key);

		if (!Slot || !Layer.Chunks.IsValidIndex(Slot->X) || !Layer.Chunks[Slot->X])
		{
			return false;
		}

		FTransform Transform;
		Layer.Chunks[Slot->X]->GetInstanceTransform(Slot->Y, Transform, /*bWorldSpace=*/true);

		const FVector Half = Transform.GetScale3D() * 50.0;
		OutBox = FBox(Transform.GetLocation() - Half, Transform.GetLocation() + Half);
		return true;
	}

	/** The beam from Node along Axis as drawn - whole or cut down - and the box it fills. False when none is drawn. */
	static bool GetBeamBox(const ARoomManager& Manager, const FIntVector& Node, const int32 Axis, bool& bOutCut, FBox& OutBox)
	{
		const uint64 Key = WEMGrid::MakeTaggedKey(Node, Axis);
		bOutCut = GetPieceBox(Manager.EdgeLayer, Key | WaterSeamBit, OutBox);
		return bOutCut || GetPieceBox(Manager.EdgeLayer, Key, OutBox);
	}

	/** The post at Node as drawn - whole or cut down - and the box it fills. False when none is drawn. */
	static bool GetPostBox(const ARoomManager& Manager, const FIntVector& Node, bool& bOutCut, FBox& OutBox)
	{
		const uint64 Key = WEMGrid::MakeCellKey(Node);
		bOutCut = GetPieceBox(Manager.NodeLayer, Key | WaterSeamBit, OutBox);
		return bOutCut || GetPieceBox(Manager.NodeLayer, Key, OutBox);
	}

	/** The box of the water over the seam at the beam from Node along Axis, or at the post at Node with Axis -1. */
	static bool GetSeamWaterBox(const ARoomManager& Manager, const UPlatformStructureData* Water, const FIntVector& Node, const int32 Axis, FBox& OutBox)
	{
		const uint64 Key = (Axis < 0 ? WEMGrid::MakeCellKey(Node) : WEMGrid::MakeTaggedKey(Node, Axis)) | WaterSeamBit;

		for (const FStructureLayer& Layer : Manager.StructureLayers)
		{
			if (Layer.Structure == Water)
			{
				return GetPieceBox(Layer.Pieces, Key, OutBox);
			}
		}

		return false;
	}

	/** A face's streak in a category, or zero when its structure does not carry it. */
	static int32 GetStreak(const ARoomManager& Manager, const int32 PlatformIndex, const int32 FaceSign, const UStructureCategory* Category)
	{
		const int32* CategoryIndex = Manager.CategoryIndices.Find(Category);
		const ARoomManager::FFaceFill& Fill = Manager.FaceFills[PlatformIndex * 2 + (FaceSign > 0 ? 0 : 1)];
		const int32 Index = CategoryIndex ? Fill.Categories.Find(*CategoryIndex) : INDEX_NONE;

		return Index != INDEX_NONE ? Fill.Streaks[Index] : 0;
	}
};

/** Worlds, tiles, structures and room managers for the room manager's tests, made by hand rather than from assets. */
namespace WEMRoomManagerTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** For checks that take minutes: run by name, never with the rest. */
	constexpr EAutomationTestFlags SlowTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter;

	/** A game world with nothing in it, torn down again when this goes out of scope. */
	struct FScopedTestWorld
	{
		FScopedTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false, TEXT("WEMRoomManagerTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
		}

		~FScopedTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		UWorld* World = nullptr;
	};

	inline void ReleaseRooted(TConstArrayView<UObject*> Objects)
	{
		for (UObject* Object : Objects)
		{
			if (Object)
			{
				Object->RemoveFromRoot();
			}
		}
	}

	/** Rooted, so nothing collects it in the middle of a test; the test lets go of it with ReleaseRooted. */
	inline UPlatformTileData* MakeTile(const int32 Width, const int32 Length, const bool bCanTurn = false)
	{
		UPlatformTileData* Tile = NewObject<UPlatformTileData>(GetTransientPackage());
		Tile->Width = Width;
		Tile->Length = Length;
		Tile->bCanTurn = bCanTurn;
		Tile->AddToRoot();
		return Tile;
	}

	/** A structure on the engine's cube, which the room manager draws but no test looks at. Rooted, like a tile. */
	inline UPlatformStructureData* MakeStructure(
		const TCHAR* Name,
		const int32 Width,
		const int32 Length,
		const EStructureType Types,
		const bool bTwoSided,
		const bool bMayLieSideways = false)
	{
		UPlatformStructureData* Structure = NewObject<UPlatformStructureData>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPlatformStructureData::StaticClass(), Name));
		Structure->Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		Structure->Width = Width;
		Structure->Length = Length;
		Structure->Types = static_cast<int32>(Types);
		Structure->bTwoSided = bTwoSided;
		Structure->bMayLieSideways = bMayLieSideways;
		Structure->AddToRoot();
		return Structure;
	}

	/** Nature of one form with no mesh, drawn as a slab of its flat colour. Only ground holds items. Rooted, like a tile. */
	inline UPlatformStructureData* MakeNature(const TCHAR* Name, const ENatureForm Form)
	{
		UPlatformStructureData* Nature = NewObject<UPlatformStructureData>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPlatformStructureData::StaticClass(), Name));
		Nature->Section = EStructureSection::Nature;
		Nature->NatureForm = Form;
		Nature->bHoldsItems = Form == ENatureForm::Ground;
		Nature->AddToRoot();
		return Nature;
	}

	/** One two-sided slab per size, of every type and free to lie sideways: every tile filled, any way it is laid. */
	inline TArray<TObjectPtr<UPlatformStructureData>> MakeSlabs(TConstArrayView<UPlatformTileData*> Tiles, TArray<UObject*>& OutRooted)
	{
		TArray<TObjectPtr<UPlatformStructureData>> Slabs;

		for (const UPlatformTileData* Tile : Tiles)
		{
			UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), Tile->Width, Tile->Length,
				EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall, /*bTwoSided=*/true, /*bMayLieSideways=*/true);
			Slabs.Add(Slab);
			OutRooted.Add(Slab);
		}

		return Slabs;
	}

	/**
	 * A room manager with the designer's tile sizes, a two-sided slab filling each, overlay and
	 * pieces off, no air pockets, molds or paths, one first tile in the middle, no strikes, reshuffles,
	 * moving pockets, spilling grounds or runs of walls, nothing cut loose from the ground tiles, on a fixed
	 * seed. Structures replaces the slabs when given.
	 */
	inline ARoomManager* SpawnRoomManager(
		UWorld* World,
		TArray<UObject*>& OutRooted,
		const TOptional<TArray<TObjectPtr<UPlatformStructureData>>>& Structures = {})
	{
		// Deferred, so its construction sees the tiles rather than warning that there are none.
		ARoomManager* Manager = World->SpawnActorDeferred<ARoomManager>(ARoomManager::StaticClass(), FTransform::Identity);
		Manager->bShowSurfaces = false;
		Manager->bShowPieces = false;
		Manager->bShowDebugGrid = false;
		Manager->bSnapToLandscapeHeight = false;
		Manager->PlatformRandomSeed = 1234;
		Manager->AirPocketCount = 0;
		Manager->MoldCount = 0;
		Manager->PathCount = 0;
		Manager->bSeedEveryZone = false;
		Manager->bEnableDamage = false;
		Manager->bEnableReshuffle = false;
		Manager->bMovePockets = false;
		Manager->bLinkToGroundTiles = false;
		Manager->InteriorGroundSpillChance = 0.0f;
		Manager->ExteriorGroundSpillChance = 0.0f;
		Manager->InteriorWallRunChance = 0.0f;
		Manager->ExteriorWallRunChance = 0.0f;

		UPlatformTileData* Square = MakeTile(16, 16);
		UPlatformTileData* Small = MakeTile(8, 8);
		UPlatformTileData* Long = MakeTile(8, 16, true);
		OutRooted.Append({ Square, Small, Long });

		Manager->FirstTile = Square;
		Manager->Tiles = { Square, Small, Long };
		Manager->Structures = Structures.IsSet() ? Structures.GetValue() : MakeSlabs({ Square, Small, Long }, OutRooted);
		Manager->FinishSpawning(FTransform::Identity);
		Manager->ClearPlatforms();
		return Manager;
	}
}

#endif
