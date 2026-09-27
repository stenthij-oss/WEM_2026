// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementSolver.h"

#include "Algo/BinarySearch.h"
#include "Engine/DataTable.h"
#include "GridMath.h"
#include "PlacementArchetype.h"
#include "PlacementWorld.h"

namespace PlacementSolverPrivate
{
	/** A hosted plane's id has this bit set, so every segment plane sorts before every hosted one. */
	constexpr uint64 HostedPlaneBit = uint64(1) << 63;

	/** How much a height may overshoot a clearance and still be taken to fit. */
	constexpr double ClearanceTolerance = 0.01;

	/** Most cells either way the log's map shows of a plane around an object. */
	constexpr int32 MaxMapMargin = 8;

	/**
	 * Most poses one FindPose draws before giving up. Where there is room a pose is found in a few
	 * draws; this only bounds the search that comes up empty, which would otherwise test every pose
	 * of a structure that can offer hundreds of thousands - on every beat, since the structure
	 * changes on every beat and what was found to have nowhere to go is only known until it does.
	 */
	constexpr int32 MaxPoseDraws = 2048;

	/** Planes whose seed lists are this short are never compacted: passing over their closed cells costs nothing. */
	constexpr int32 MinSeedCellsToCompact = 64;

	FIntVector MakePerimeterKey(const FIntPoint& Cell, const int32 DirectionIndex)
	{
		return FIntVector(Cell.X, Cell.Y, DirectionIndex);
	}

	bool ZoneExcludes(const FExclusionZone& Zone, const UPlacementArchetype* Archetype)
	{
		const bool bListed = Archetype && Archetype->IsMatchedBy(Zone.Archetypes, Zone.Classes);
		return Zone.Mode == EZoneMode::BlockListed ? bListed : !bListed;
	}

	/** Whether a target face lies on the edge a contact asks for. */
	bool IsOnTargetEdge(const FContactRequirement& Contact, const FPlacedPerimeterFace& Face)
	{
		switch (Contact.TargetEdge)
		{
		case ETargetEdge::Front: return Face.Side == EObjectSide::Front;
		case ETargetEdge::Back: return Face.Side == EObjectSide::Back;
		case ETargetEdge::Left: return Face.Side == EObjectSide::Left;
		case ETargetEdge::Right: return Face.Side == EObjectSide::Right;
		case ETargetEdge::Special: return Face.SpecialEdgeIds.Contains(Contact.TargetSpecialEdgeId);
		default: return true;
		}
	}

	bool CompareSpecialEdges(const FSpecialEdge& A, const FSpecialEdge& B)
	{
		if (A.Id != B.Id)
		{
			return A.Id < B.Id;
		}

		if (A.Side != B.Side)
		{
			return A.Side < B.Side;
		}

		return A.Start != B.Start ? A.Start < B.Start : A.Count < B.Count;
	}

	/** The letter the log's map draws a footprint cell with: the side it lies on, C where two meet, . inside. */
	TCHAR GetFootprintLetter(const FIntVector& Size, const FIntPoint& LocalCell)
	{
		int32 Sides = 0;
		TCHAR Letter = TEXT('.');

		auto Check = [&](const EObjectSide Side, const TCHAR SideLetter)
		{
			const FIntVector Direction = WEMPlacement::GetSideDirection(Side);
			const bool bOnSide = Direction.X > 0 ? LocalCell.X == Size.X - 1
				: Direction.X < 0 ? LocalCell.X == 0
				: Direction.Y > 0 ? LocalCell.Y == Size.Y - 1
				: LocalCell.Y == 0;

			if (bOnSide)
			{
				++Sides;
				Letter = SideLetter;
			}
		};

		Check(EObjectSide::Back, TEXT('B'));
		Check(EObjectSide::Front, TEXT('F'));
		Check(EObjectSide::Left, TEXT('L'));
		Check(EObjectSide::Right, TEXT('R'));

		return Sides >= 2 ? TEXT('C') : Letter;
	}
}

// FPlacementCatalogData

bool FPlacementCatalogData::AddTables(
	const TConstArrayView<const UDataTable*> Tables,
	const double CellSize,
	FString& OutError,
	TArray<FString>& OutWarnings)
{
	Reset();

	TArray<FString> WrongCellSize;
	int32 Incomplete = 0;

	for (const UDataTable* Table : Tables)
	{
		if (!Table)
		{
			continue;
		}

		if (Table->GetRowStruct() != FPlacementCatalogRow::StaticStruct())
		{
			OutWarnings.Add(FString::Printf(TEXT("%s does not hold PlacementCatalogRow rows and is skipped."), *Table->GetName()));
			continue;
		}

		for (const TPair<FName, uint8*>& Entry : Table->GetRowMap())
		{
			const FPlacementCatalogRow& Row = *reinterpret_cast<const FPlacementCatalogRow*>(Entry.Value);

			if (!Row.bEnabled)
			{
				continue;
			}

			if (!FMath::IsNearlyEqual(Row.BuiltCellSize, static_cast<float>(CellSize)))
			{
				WrongCellSize.Add(FString::Printf(TEXT("%s.%s (%g)"), *Table->GetName(), *Entry.Key.ToString(), Row.BuiltCellSize));
				continue;
			}

			if (!Row.Archetype || Row.Mesh.IsNull())
			{
				++Incomplete;
				continue;
			}

			AddRow(Entry.Key, Row);
		}
	}

	if (Incomplete > 0)
	{
		OutWarnings.Add(FString::Printf(TEXT("%d enabled row(s) have no archetype or no mesh and are skipped."), Incomplete));
	}

	if (!WrongCellSize.IsEmpty())
	{
		// Every cell the builder measured these in is the wrong size for this grid, so nothing
		// derived from them - footprint, surfaces, edges - can be trusted.
		OutError = FString::Printf(TEXT("%d enabled row(s) were built at a cell size other than the room manager's %g: %s. Build the catalog again at that size."),
			WrongCellSize.Num(), CellSize, *FString::Join(WrongCellSize, TEXT(", ")));
		Reset();
		return false;
	}

	return true;
}

void FPlacementCatalogData::AddRow(const FName RowName, const FPlacementCatalogRow& Row)
{
	if (!Row.Archetype)
	{
		return;
	}

	FPlacementEntry& Entry = Entries.AddDefaulted_GetRef();
	const int32 EntryIndex = Entries.Num() - 1;

	Entry.RowName = RowName;
	Entry.Mesh = Row.Mesh;
	Entry.Archetype = Row.Archetype;
	Entry.SetName = Row.GetEffectiveSetName();
	Entry.Geometry = Row.GetEffectiveGeometry();
	Entry.Weight = FMath::Max(Row.Weight, 0.0f);
	Entry.MaxInstances = Row.MaxInstances;

	TArray<FSpecialEdge> SpecialEdges = Entry.Geometry.SpecialEdges;
	SpecialEdges.Sort(&PlacementSolverPrivate::CompareSpecialEdges);

	// The signature it shares with every entry that agrees on archetype, size and special edges.
	int32 SignatureIndex = Signatures.IndexOfByPredicate([&](const FPlacementSignature& Signature)
	{
		return Signature.Archetype == Entry.Archetype
			&& Signature.SizeCells == Entry.Geometry.SizeCells
			&& Signature.SpecialEdges == SpecialEdges;
	});

	if (SignatureIndex == INDEX_NONE)
	{
		FPlacementSignature& Signature = Signatures.AddDefaulted_GetRef();
		SignatureIndex = Signatures.Num() - 1;

		Signature.Archetype = Entry.Archetype;
		Signature.SizeCells = Entry.Geometry.SizeCells;
		Signature.SpecialEdges = SpecialEdges;
		WEMPlacement::GatherEdgeFaces(Signature.SizeCells, {}, Signature.EdgeFaces);

		int32 KindIndex = Kinds.IndexOfByPredicate([&](const FPlacementKind& Kind) { return Kind.Archetype == Entry.Archetype; });

		if (KindIndex == INDEX_NONE)
		{
			FPlacementKind& Kind = Kinds.AddDefaulted_GetRef();
			Kind.Archetype = Entry.Archetype;
			KindIndex = Kinds.Num() - 1;
		}

		Kinds[KindIndex].Signatures.Add(SignatureIndex);
	}

	Entry.Signature = SignatureIndex;
	Signatures[SignatureIndex].Entries.Add(EntryIndex);
}

void FPlacementCatalogData::Reset()
{
	Entries.Reset();
	Signatures.Reset();
	Kinds.Reset();
}

// FPlacementSolver

FPlacementSolver::FPlacementSolver(const IPlacementWorld& InWorld, FPlacementCatalogData& InCatalog, FRandomStream& InStream)
	: World(InWorld)
	, Catalog(InCatalog)
	, Stream(InStream)
{
}

uint64 FPlacementSolver::MakeSegmentPlaneId(const EGridFace Face, const int32 Layer)
{
	return (static_cast<uint64>(Face) << 32) | static_cast<uint32>(Layer);
}

uint64 FPlacementSolver::MakeHostedPlaneId(const int32 Host, const int32 Layer)
{
	return PlacementSolverPrivate::HostedPlaneBit | (static_cast<uint64>(static_cast<uint32>(Host)) << 16) | static_cast<uint16>(Layer);
}

int32 FPlacementSolver::GetRevision() const
{
	// Both only ever count up, so their sum changes whenever either does.
	return World.GetStructureRevision() + ObjectRevision;
}

const FPlacedObject* FPlacementSolver::FindObject(const int32 Id) const
{
	return Objects.IsValidIndex(Id - 1) ? &Objects[Id - 1] : nullptr;
}

const FSupportPlane* FPlacementSolver::FindPlane(const uint64 Id) const
{
	return Planes.Find(Id);
}

void FPlacementSolver::AddPlaneToOrder(const uint64 Id)
{
	PlaneOrder.Insert(Id, Algo::LowerBound(PlaneOrder, Id));
}

FSupportPlane& FPlacementSolver::FindOrAddSegmentPlane(const EGridFace Face, const int32 Layer)
{
	const uint64 Id = MakeSegmentPlaneId(Face, Layer);

	if (FSupportPlane* Existing = Planes.Find(Id))
	{
		return *Existing;
	}

	FSupportPlane& Plane = Planes.Add(Id);
	Plane.Id = Id;
	Plane.Face = Face;
	Plane.Layer = Layer;
	Plane.Axes = WEMPlacement::MakeSegmentPlaneAxes(Face);
	Plane.Name = WEMPlacement::DescribeSegmentPlane(Face, Layer);

	AddPlaneToOrder(Id);
	return Plane;
}

uint64 FPlacementSolver::GetSegmentPlaneId(const FIntVector& SurfaceCell, const EGridFace Face)
{
	RefreshSegmentPlanes();
	return FindOrAddSegmentPlane(Face, SurfaceCell[WEMGrid::FaceAxis(Face)]).Id;
}

void FPlacementSolver::RefreshSegmentPlanes()
{
	const int32 StructureRevision = World.GetStructureRevision();

	if (StructureRevision == CachedStructureRevision)
	{
		return;
	}

	const double Start = FPlatformTime::Seconds();

	// Whatever was found to have nowhere to go was found on the planes as they stood; on the planes
	// as they stand now it may have somewhere.
	for (FPlacementSignature& Signature : Catalog.Signatures)
	{
		Signature.InfeasibleAtRevision = INDEX_NONE;
	}

	// What the structure's changes since the last refresh touched, when the world can say - which
	// is a placement's worth of surfaces rather than all of them.
	TArray<FGridSurfaceRef> Changed;

	if (CachedStructureRevision != INDEX_NONE && World.GatherSurfaceChangesSince(CachedStructureRevision, Changed))
	{
		ApplySurfaceChanges(Changed);
	}
	else
	{
		RebuildSegmentPlanes();
	}

	CachedStructureRevision = StructureRevision;
	LastRefreshMilliseconds += (FPlatformTime::Seconds() - Start) * 1000.0;
}

void FPlacementSolver::RebuildSegmentPlanes()
{
	// Planes are kept, however much of them the structure has built over, since what stands on them
	// is recorded there. Only which of their cells are open is taken afresh.
	for (TPair<uint64, FSupportPlane>& Entry : Planes)
	{
		if (!Entry.Value.bHosted)
		{
			Entry.Value.OpenCells.Reset();
			Entry.Value.SeedCells.Reset();
			Entry.Value.ClosedSeedCells = 0;
			Entry.Value.WallSeeds.Reset();
			Entry.Value.BoundarySeeds.Reset();
			Entry.Value.WallSeedKeys.Reset();
			Entry.Value.bWallSeedsBuilt = false;
			Entry.Value.bBoundarySeedsBuilt = false;
		}
	}

	TArray<FGridSurfaceRef> Surfaces;
	World.GatherOpenAllObjectSurfaces(Surfaces);

	// In the order the structure lists them, which depends only on the order its platforms were
	// placed in, so the same growth always draws the same poses.
	for (const FGridSurfaceRef& Surface : Surfaces)
	{
		FSupportPlane& Plane = FindOrAddSegmentPlane(Surface.Face, Surface.Cell[WEMGrid::FaceAxis(Surface.Face)]);
		const FIntPoint Cell = WEMPlacement::WorldToSegmentPlaneCell(Surface.Face, Surface.Cell);

		bool bAlreadyOpen = false;
		Plane.OpenCells.Add(Cell, &bAlreadyOpen);

		if (!bAlreadyOpen)
		{
			Plane.SeedCells.Add(Cell);
		}
	}
}

void FPlacementSolver::ApplySurfaceChanges(const TConstArrayView<FGridSurfaceRef> Surfaces)
{
	using namespace WEMPlacement;

	// A surface can be touched by more than one change; it is only judged once, as it stands now.
	TSet<uint64> Judged;
	Judged.Reserve(Surfaces.Num());

	// The cells the changes touched, each once, in the order first met: every one of them is built,
	// and any may have just been.
	TArray<FIntVector> TouchedCells;
	TSet<uint64> TouchedCellKeys;

	TArray<uint64> ChangedPlanes;

	for (const FGridSurfaceRef& Surface : Surfaces)
	{
		bool bAlreadyJudged = false;
		Judged.Add(WEMGrid::MakeFaceKey(Surface.Cell, Surface.Face), &bAlreadyJudged);

		if (bAlreadyJudged)
		{
			continue;
		}

		bool bAlreadyTouched = false;
		TouchedCellKeys.Add(WEMGrid::MakeCellKey(Surface.Cell), &bAlreadyTouched);

		if (!bAlreadyTouched)
		{
			TouchedCells.Add(Surface.Cell);
		}

		// Open as GatherOpenAllObjectSurfaces counts it: carrying anything, and not built against.
		const bool bOpen = World.GetSurfaceCapacity(Surface.Cell, Surface.Face) == ESurfaceCapacity::AllObject
			&& !World.IsCellSolid(Surface.Cell + WEMGrid::FaceStep(Surface.Face));

		const int32 Layer = Surface.Cell[WEMGrid::FaceAxis(Surface.Face)];
		const FIntPoint Cell = WorldToSegmentPlaneCell(Surface.Face, Surface.Cell);

		if (bOpen)
		{
			FSupportPlane& Plane = FindOrAddSegmentPlane(Surface.Face, Layer);

			bool bAlreadyOpen = false;
			Plane.OpenCells.Add(Cell, &bAlreadyOpen);

			// A surface only ever opens once - what carries anything keeps carrying it, and what is
			// built against stays built against - so a newly open cell is never listed already.
			if (!bAlreadyOpen)
			{
				Plane.SeedCells.Add(Cell);

				if (Plane.bWallSeedsBuilt)
				{
					AddAnchorSeeds(Plane, Cell, /*bWall=*/true);
				}

				if (Plane.bBoundarySeedsBuilt)
				{
					AddAnchorSeeds(Plane, Cell, /*bWall=*/false);
				}
			}
		}
		else if (FSupportPlane* Plane = Planes.Find(MakeSegmentPlaneId(Surface.Face, Layer)))
		{
			if (Plane->OpenCells.Remove(Cell) > 0)
			{
				++Plane->ClosedSeedCells;
				ChangedPlanes.AddUnique(Plane->Id);
			}
		}
	}

	// Structure just built beside an open cell stands as a wall to whatever stands there. Such a
	// cell lies one step across and one layer down from the structure, on any of the six faces.
	for (const FIntVector& Solid : TouchedCells)
	{
		for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
		{
			const EGridFace Face = static_cast<EGridFace>(FaceIndex);
			const FPlaneAxes Axes = MakeSegmentPlaneAxes(Face);

			for (int32 Direction = 0; Direction < 4; ++Direction)
			{
				const FIntVector Surface = Solid - Axes.Normal - PlaneToWorldDirection(Axes, PlaneDirections[Direction]);
				FSupportPlane* Plane = Planes.Find(MakeSegmentPlaneId(Face, Surface[WEMGrid::FaceAxis(Face)]));

				if (!Plane || !Plane->bWallSeedsBuilt)
				{
					continue;
				}

				const FIntPoint Cell = WorldToSegmentPlaneCell(Face, Surface);

				if (Plane->OpenCells.Contains(Cell) && !Plane->WallSeedKeys.Contains(FIntVector(Cell.X, Cell.Y, Direction)))
				{
					Plane->WallSeedKeys.Add(FIntVector(Cell.X, Cell.Y, Direction));
					Plane->WallSeeds.Add({ Cell, Direction });
				}
			}
		}
	}

	for (const uint64 PlaneId : ChangedPlanes)
	{
		FSupportPlane& Plane = Planes[PlaneId];

		if (Plane.SeedCells.Num() >= PlacementSolverPrivate::MinSeedCellsToCompact && 2 * Plane.ClosedSeedCells >= Plane.SeedCells.Num())
		{
			CompactSeeds(Plane);
		}
	}
}

void FPlacementSolver::CompactSeeds(FSupportPlane& Plane)
{
	auto IsClosed = [&Plane](const FIntPoint& Cell)
	{
		return !Plane.OpenCells.Contains(Cell);
	};

	Plane.SeedCells.RemoveAll(IsClosed);
	Plane.WallSeeds.RemoveAll([&IsClosed](const FPoseSeed& Seed) { return IsClosed(Seed.Cell); });
	Plane.BoundarySeeds.RemoveAll([&IsClosed](const FPoseSeed& Seed) { return IsClosed(Seed.Cell); });
	Plane.ClosedSeedCells = 0;
}

bool FPlacementSolver::VerifySegmentPlanes(TArray<FString>& OutProblems) const
{
	using namespace WEMPlacement;

	OutProblems.Reset();

	TArray<FGridSurfaceRef> Surfaces;
	World.GatherOpenAllObjectSurfaces(Surfaces);

	// Every open cell as gathered afresh, plane by plane.
	TMap<uint64, TSet<FIntPoint>> Gathered;
	for (const FGridSurfaceRef& Surface : Surfaces)
	{
		const uint64 PlaneId = MakeSegmentPlaneId(Surface.Face, Surface.Cell[WEMGrid::FaceAxis(Surface.Face)]);
		Gathered.FindOrAdd(PlaneId).Add(WorldToSegmentPlaneCell(Surface.Face, Surface.Cell));
	}

	for (const TPair<uint64, TSet<FIntPoint>>& Entry : Gathered)
	{
		if (!Planes.Contains(Entry.Key))
		{
			OutProblems.Add(FString::Printf(TEXT("%s: missing, with %d open cell(s)"),
				*DescribeSegmentPlane(static_cast<EGridFace>(Entry.Key >> 32), static_cast<int32>(Entry.Key & 0xFFFFFFFF)), Entry.Value.Num()));
		}
	}

	const TSet<FIntPoint> NoCells;

	for (const uint64 PlaneId : PlaneOrder)
	{
		const FSupportPlane& Plane = Planes[PlaneId];

		if (Plane.bHosted)
		{
			continue;
		}

		const TSet<FIntPoint>* Found = Gathered.Find(PlaneId);
		const TSet<FIntPoint>& Open = Found ? *Found : NoCells;

		const TSet<FIntPoint> Listed(Plane.SeedCells);
		TSet<FIntVector> WallListed;
		TSet<FIntVector> BoundaryListed;

		for (const FPoseSeed& Seed : Plane.WallSeeds)
		{
			WallListed.Add(FIntVector(Seed.Cell.X, Seed.Cell.Y, Seed.Direction));
		}

		for (const FPoseSeed& Seed : Plane.BoundarySeeds)
		{
			BoundaryListed.Add(FIntVector(Seed.Cell.X, Seed.Cell.Y, Seed.Direction));
		}

		int32 Missing = 0;
		int32 Unlisted = 0;
		int32 MissingWallSeeds = 0;
		int32 MissingBoundarySeeds = 0;

		for (const FIntPoint& Cell : Open)
		{
			Missing += Plane.OpenCells.Contains(Cell) ? 0 : 1;
			Unlisted += Listed.Contains(Cell) ? 0 : 1;

			// Listed seeds the structure has since changed around are let be; a seed a fresh build
			// would list has to be there.
			for (int32 Direction = 0; Direction < 4; ++Direction)
			{
				const FIntVector Key(Cell.X, Cell.Y, Direction);

				if (Plane.bWallSeedsBuilt && IsWallAcross(Plane, Cell, Direction) && !WallListed.Contains(Key))
				{
					++MissingWallSeeds;
				}

				const FIntPoint Across = Cell + PlaneDirections[Direction];

				if (Plane.bBoundarySeedsBuilt && !Open.Contains(Across) && !IsSupportCell(Plane, Across) && !BoundaryListed.Contains(Key))
				{
					++MissingBoundarySeeds;
				}
			}
		}

		int32 Extra = 0;
		for (const FIntPoint& Cell : Plane.OpenCells)
		{
			Extra += Open.Contains(Cell) ? 0 : 1;
		}

		if (Missing > 0 || Extra > 0 || Unlisted > 0 || MissingWallSeeds > 0 || MissingBoundarySeeds > 0)
		{
			OutProblems.Add(FString::Printf(TEXT("%s: %d open cell(s) missing, %d extra, %d not listed to draw from, %d wall seed(s) and %d boundary seed(s) missing"),
				*Plane.Name, Missing, Extra, Unlisted, MissingWallSeeds, MissingBoundarySeeds));
		}
	}

	return OutProblems.IsEmpty();
}

void FPlacementSolver::Reset()
{
	for (TPair<uint64, FSupportPlane>& Entry : Planes)
	{
		Entry.Value.Footprints.Reset();
		Entry.Value.PerimeterFaces.Reset();
		Entry.Value.Zones.Reset();
		Entry.Value.Objects.Reset();
	}

	for (const FPlacedObject& Object : Objects)
	{
		for (const uint64 Hosted : Object.HostedPlanes)
		{
			Planes.Remove(Hosted);
		}
	}

	PlaneOrder.RemoveAll([](const uint64 Id) { return (Id & PlacementSolverPrivate::HostedPlaneBit) != 0; });
	HostedPlaneGroups.Reset();

	Objects.Reset();
	ReservedCells.Reset();

	for (FPlacementEntry& Entry : Catalog.Entries)
	{
		Entry.Instances = 0;
	}

	for (FPlacementSignature& Signature : Catalog.Signatures)
	{
		Signature.InfeasibleAtRevision = INDEX_NONE;
	}

	for (FPlacementKind& Kind : Catalog.Kinds)
	{
		Kind.Instances = 0;
	}

	++ObjectRevision;
}

bool FPlacementSolver::GetColumnBase(const FSupportPlane& Plane, const FIntPoint& Cell, FIntVector& OutBase) const
{
	if (!Plane.bHosted)
	{
		OutBase = WEMPlacement::SegmentPlaneCellToWorld(Plane.Face, Plane.Layer, Cell);
		return true;
	}

	if (const FIntVector* Base = Plane.ColumnBases.Find(Cell))
	{
		OutBase = *Base;
		return true;
	}

	return false;
}

bool FPlacementSolver::IsSupportCell(const FSupportPlane& Plane, const FIntPoint& Cell) const
{
	if (Plane.bHosted)
	{
		return Plane.LayerCells.Contains(Cell);
	}

	// Part of this segment support whether or not it is open: an occupied or built-against cell
	// still carries all-object surface, it only has something on it.
	return World.GetSurfaceCapacity(WEMPlacement::SegmentPlaneCellToWorld(Plane.Face, Plane.Layer, Cell), Plane.Face) == ESurfaceCapacity::AllObject;
}

bool FPlacementSolver::IsCellAvailable(const FSupportPlane& Plane, const FIntPoint& Cell) const
{
	if (Plane.Footprints.Contains(Cell))
	{
		return false;
	}

	if (Plane.bHosted)
	{
		return Plane.LayerCells.Contains(Cell);
	}

	if (!Plane.OpenCells.Contains(Cell))
	{
		return false;
	}

	// Something from another plane - a wall's shelf reaching over a floor - can fill the air here.
	const FIntVector Base = WEMPlacement::SegmentPlaneCellToWorld(Plane.Face, Plane.Layer, Cell) + Plane.Axes.Normal;
	return !ReservedCells.Contains(WEMGrid::MakeCellKey(Base));
}

void FPlacementSolver::EnsureAnchorSeeds(FSupportPlane& Plane, const EContactTarget Target)
{
	using namespace WEMPlacement;

	const bool bWall = Target == EContactTarget::Wall;
	bool& bBuilt = bWall ? Plane.bWallSeedsBuilt : Plane.bBoundarySeedsBuilt;
	TArray<FPoseSeed>& Seeds = bWall ? Plane.WallSeeds : Plane.BoundarySeeds;

	if (bBuilt)
	{
		return;
	}

	bBuilt = true;
	Seeds.Reset();

	if (bWall)
	{
		Plane.WallSeedKeys.Reset();
	}

	// Only a segment has structure beside it.
	if (bWall && Plane.bHosted)
	{
		return;
	}

	for (const FIntPoint& Cell : Plane.SeedCells)
	{
		// A segment cell the structure has closed since it was listed offers nothing to grow from.
		if (Plane.bHosted || Plane.OpenCells.Contains(Cell))
		{
			AddAnchorSeeds(Plane, Cell, bWall);
		}
	}
}

void FPlacementSolver::AddAnchorSeeds(FSupportPlane& Plane, const FIntPoint& Cell, const bool bWall)
{
	for (int32 Direction = 0; Direction < 4; ++Direction)
	{
		if (!bWall)
		{
			if (IsBoundaryAcross(Plane, Cell, Direction))
			{
				Plane.BoundarySeeds.Add({ Cell, Direction });
			}

			continue;
		}

		if (IsWallAcross(Plane, Cell, Direction))
		{
			bool bAlreadyListed = false;
			Plane.WallSeedKeys.Add(FIntVector(Cell.X, Cell.Y, Direction), &bAlreadyListed);

			if (!bAlreadyListed)
			{
				Plane.WallSeeds.Add({ Cell, Direction });
			}
		}
	}
}

bool FPlacementSolver::IsWallAcross(const FSupportPlane& Plane, const FIntPoint& Cell, const int32 Direction) const
{
	using namespace WEMPlacement;

	const FIntVector Base = SegmentPlaneCellToWorld(Plane.Face, Plane.Layer, Cell) + Plane.Axes.Normal;
	return World.IsCellSolid(Base + PlaneToWorldDirection(Plane.Axes, PlaneDirections[Direction]));
}

bool FPlacementSolver::IsBoundaryAcross(const FSupportPlane& Plane, const FIntPoint& Cell, const int32 Direction) const
{
	// An open neighbour is plainly support, which spares asking the structure about most of them.
	const FIntPoint Across = Cell + WEMPlacement::PlaneDirections[Direction];
	return !Plane.OpenCells.Contains(Across) && !IsSupportCell(Plane, Across);
}

bool FPlacementSolver::GatherVolume(
	const FSupportPlane& Plane,
	const WEMPlacement::FPoseFrame& Frame,
	const FIntPoint& Origin,
	const FIntVector& SizeCells,
	TArray<FIntVector>& OutCells) const
{
	OutCells.Reset();

	const double CellSize = World.GetCellSize();
	const double Height = SizeCells.Z * CellSize;

	// Counted in layers of air off the segment face at the bottom of the column: one up to the
	// object's height on a segment, and whichever layers its box reaches into when it stands higher.
	const int32 FirstLayer = FMath::FloorToInt32(Plane.BaseOffsetCm / CellSize + 1.0e-6) + 1;
	const int32 LastLayer = FMath::CeilToInt32((Plane.BaseOffsetCm + Height) / CellSize - 1.0e-6);

	for (int32 X = 0; X < SizeCells.X; ++X)
	{
		for (int32 Y = 0; Y < SizeCells.Y; ++Y)
		{
			FIntVector Base;

			if (!GetColumnBase(Plane, WEMPlacement::LocalToPlaneCell(Frame, Origin, FIntPoint(X, Y)), Base))
			{
				return false;
			}

			for (int32 Layer = FirstLayer; Layer <= LastLayer; ++Layer)
			{
				OutCells.Add(Base + Plane.Axes.Normal * Layer);
			}
		}
	}

	return true;
}

bool FPlacementSolver::DoesPlaneAccept(const FSupportPlane& Plane, const UPlacementArchetype* Archetype) const
{
	if (!Plane.bHosted)
	{
		return true;
	}

	return DoesHostAccept(Plane.HostArchetype, Archetype);
}

bool FPlacementSolver::DoesHostAccept(const UPlacementArchetype* HostArchetype, const UPlacementArchetype* Archetype)
{
	if (!HostArchetype)
	{
		return false;
	}

	const FSurfacePolicy& Policy = HostArchetype->Surfaces;

	switch (Policy.Accept)
	{
	case EAcceptMode::All: return true;
	case EAcceptMode::None: return false;
	default: return Archetype && Archetype->IsMatchedBy(Policy.AcceptedArchetypes, Policy.AcceptedClasses);
	}
}

bool FPlacementSolver::IsPlaneAllowed(const FSupportPlane& Plane, const FPlacementAttempt& Attempt) const
{
	const ESupportKind Kind = Plane.bHosted ? ESupportKind::ObjectSurface : ESupportKind::Segment;

	if ((Attempt.SupportMask & WEMPlacement::SupportBit(Kind)) == 0)
	{
		return false;
	}

	if (Attempt.RestrictToHost != INDEX_NONE)
	{
		// A companion stays with its host: on the host's own surfaces, or on a segment only when a
		// contact ties it to the host there.
		const bool bTiedByContact = Attempt.Rule && Attempt.Rule->Kind == ERuleKind::Edge
			&& Attempt.Rule->Contacts.ContainsByPredicate([](const FContactRequirement& Contact) { return Contact.Target == EContactTarget::Object; });

		if (Plane.bHosted ? Plane.Host != Attempt.RestrictToHost : !bTiedByContact)
		{
			return false;
		}
	}

	return DoesPlaneAccept(Plane, Catalog.Signatures[Attempt.Signature].Archetype);
}

bool FPlacementSolver::IsContactTarget(const FContactRequirement& Contact, const FPlacementAttempt& Attempt, const int32 Object) const
{
	if (Attempt.RestrictToHost != INDEX_NONE && Object != Attempt.RestrictToHost)
	{
		return false;
	}

	const FPlacedObject* Placed = FindObject(Object);
	return Placed && Placed->Archetype && Placed->Archetype->IsMatchedBy(Contact.TargetArchetypes, Contact.TargetClasses);
}

const FContactRequirement* FPlacementSolver::ChooseAnchor(const FPlacementRule* Rule)
{
	if (!Rule || Rule->Kind != ERuleKind::Edge || Rule->Contacts.IsEmpty())
	{
		return nullptr;
	}

	// An object's faces are few, the structure's many, and the support's edge more still, so poses
	// are grown from whichever the rule asks for that offers the fewest.
	for (const EContactTarget Target : { EContactTarget::Object, EContactTarget::Wall, EContactTarget::SupportBoundary })
	{
		for (const FContactRequirement& Contact : Rule->Contacts)
		{
			if (Contact.Target == Target && WEMPlacement::IsEdgeSide(Contact.Side))
			{
				return &Contact;
			}
		}
	}

	return nullptr;
}

bool FPlacementSolver::EvaluateContacts(
	const FPlacementAttempt& Attempt,
	const FSupportPlane& Plane,
	const WEMPlacement::FPoseFrame& Frame,
	const FIntPoint& Origin,
	FPoseCheck& OutCheck) const
{
	using namespace WEMPlacement;

	const FPlacementRule* Rule = Attempt.Rule;

	if (!Rule || Rule->Kind != ERuleKind::Edge)
	{
		return true;
	}

	const FPlacementSignature& Signature = Catalog.Signatures[Attempt.Signature];

	for (const FContactRequirement& Contact : Rule->Contacts)
	{
		// Every face of my edge, as it lands on the plane.
		struct FPosedFace
		{
			FIntPoint Cell;
			FIntPoint Direction;
		};

		TArray<FPosedFace> Faces;
		for (const FEdgeFace& Face : Signature.EdgeFaces)
		{
			if (Face.Side == Contact.Side)
			{
				Faces.Add({ LocalToPlaneCell(Frame, Origin, Face.Cell), LocalToPlaneDirection(Frame, Face.Direction) });
			}
		}

		const int32 Needed = Contact.Overlap == EOverlapMode::Flush ? Faces.Num() : FMath::Max(1, Contact.MinFaces);
		TArray<FIntVector> Touching;
		bool bHolds = false;

		if (Contact.Target == EContactTarget::Object)
		{
			// Counted per target, since every face that counts has to meet the same one.
			TMap<int32, TArray<FIntVector>> ByTarget;
			bool bMetCorner = false;

			for (const FPosedFace& Face : Faces)
			{
				const FIntPoint Across = Face.Cell + Face.Direction;
				const int32* Target = Plane.Footprints.Find(Across);

				if (!Target || !IsContactTarget(Contact, Attempt, *Target))
				{
					continue;
				}

				const int32 Facing = GetPlaneDirectionIndex(FIntPoint(-Face.Direction.X, -Face.Direction.Y));
				const FPlacedPerimeterFace* TargetFace = Plane.PerimeterFaces.Find(PlacementSolverPrivate::MakePerimeterKey(Across, Facing));

				if (!TargetFace || !PlacementSolverPrivate::IsOnTargetEdge(Contact, *TargetFace))
				{
					continue;
				}

				if (Contact.bExcludeTargetCorners && TargetFace->bCorner)
				{
					bMetCorner = true;
					continue;
				}

				ByTarget.FindOrAdd(*Target).Add(PlacementSolverPrivate::MakePerimeterKey(Face.Cell, GetPlaneDirectionIndex(Face.Direction)));
			}

			// The target the most faces meet stands for the contact; ties go to the lowest id, so
			// the choice never depends on how the map was walked.
			int32 BestTarget = INDEX_NONE;
			for (const TPair<int32, TArray<FIntVector>>& Entry : ByTarget)
			{
				if (BestTarget == INDEX_NONE
					|| Entry.Value.Num() > ByTarget[BestTarget].Num()
					|| (Entry.Value.Num() == ByTarget[BestTarget].Num() && Entry.Key < BestTarget))
				{
					BestTarget = Entry.Key;
				}
			}

			if (BestTarget != INDEX_NONE)
			{
				Touching = ByTarget[BestTarget];
			}

			bHolds = Touching.Num() >= Needed && !(Contact.Overlap == EOverlapMode::Flush && bMetCorner);
		}
		else
		{
			for (const FPosedFace& Face : Faces)
			{
				bool bTouches = false;

				if (Contact.Target == EContactTarget::Wall)
				{
					FIntVector Base;
					bTouches = !Plane.bHosted
						&& GetColumnBase(Plane, Face.Cell, Base)
						&& World.IsCellSolid(Base + Plane.Axes.Normal + PlaneToWorldDirection(Plane.Axes, Face.Direction));
				}
				else
				{
					bTouches = !IsSupportCell(Plane, Face.Cell + Face.Direction);
				}

				if (bTouches)
				{
					Touching.Add(PlacementSolverPrivate::MakePerimeterKey(Face.Cell, GetPlaneDirectionIndex(Face.Direction)));
				}
			}

			bHolds = Touching.Num() >= Needed;
		}

		if (!bHolds)
		{
			OutCheck.Failure = FString::Printf(TEXT("contact on its %s: %d of %d faces, %d needed"),
				GetSideName(Contact.Side), Touching.Num(), Faces.Num(), Needed);
			return false;
		}

		OutCheck.ContactSideMask |= SideBit(Contact.Side);
		OutCheck.ContactFaces.Append(Touching);
	}

	return true;
}

void FPlacementSolver::GatherZoneCells(
	const FPlacementSignature& Signature,
	const WEMPlacement::FPoseFrame& Frame,
	const FIntPoint& Origin,
	TArray<TPair<FIntPoint, int32>>& OutCells) const
{
	using namespace WEMPlacement;

	OutCells.Reset();

	const TArray<FExclusionZone>& Zones = Signature.Archetype->ExclusionZones;
	TArray<FIntPoint> LocalCells;

	for (int32 ZoneIndex = 0; ZoneIndex < Zones.Num(); ++ZoneIndex)
	{
		WEMPlacement::GatherZoneCells(Signature.SizeCells, Zones[ZoneIndex], LocalCells);

		for (const FIntPoint& LocalCell : LocalCells)
		{
			OutCells.Emplace(LocalToPlaneCell(Frame, Origin, LocalCell), ZoneIndex);
		}
	}
}

bool FPlacementSolver::DoZonesHold(
	const FPlacementSignature& Signature,
	const FSupportPlane& Plane,
	const WEMPlacement::FPoseFrame& Frame,
	const FIntPoint& Origin) const
{
	using namespace WEMPlacement;

	const UPlacementArchetype* Archetype = Signature.Archetype;

	// Nothing may stand in a zone that keeps it out...
	if (!Plane.Zones.IsEmpty())
	{
		for (int32 X = 0; X < Signature.SizeCells.X; ++X)
		{
			for (int32 Y = 0; Y < Signature.SizeCells.Y; ++Y)
			{
				const TArray<FPlacedZoneRef>* Refs = Plane.Zones.Find(LocalToPlaneCell(Frame, Origin, FIntPoint(X, Y)));

				if (!Refs)
				{
					continue;
				}

				for (const FPlacedZoneRef& Ref : *Refs)
				{
					const UPlacementArchetype* Owner = Objects[Ref.Object - 1].Archetype;

					if (PlacementSolverPrivate::ZoneExcludes(Owner->ExclusionZones[Ref.Zone], Archetype))
					{
						return false;
					}
				}
			}
		}
	}

	// ...and a zone may not be laid over anything standing that it keeps out. Without this the rule
	// would only hold one way round: a table placed before the couch would stand in the couch's zone.
	if (Archetype->ExclusionZones.IsEmpty() || Plane.Footprints.IsEmpty())
	{
		return true;
	}

	TArray<TPair<FIntPoint, int32>> ZoneCells;
	GatherZoneCells(Signature, Frame, Origin, ZoneCells);

	for (const TPair<FIntPoint, int32>& ZoneCell : ZoneCells)
	{
		if (const int32* Standing = Plane.Footprints.Find(ZoneCell.Key))
		{
			if (PlacementSolverPrivate::ZoneExcludes(Archetype->ExclusionZones[ZoneCell.Value], Objects[*Standing - 1].Archetype))
			{
				return false;
			}
		}
	}

	return true;
}

bool FPlacementSolver::TestPose(const FPlacementAttempt& Attempt, const FPlacementPose& Pose, FPoseCheck& OutCheck) const
{
	using namespace WEMPlacement;

	OutCheck = FPoseCheck();

	const FSupportPlane* Plane = Planes.Find(Pose.Plane);

	if (!Plane || !Catalog.Signatures.IsValidIndex(Attempt.Signature))
	{
		OutCheck.Failure = TEXT("no such plane");
		return false;
	}

	if (!IsPlaneAllowed(*Plane, Attempt))
	{
		OutCheck.Failure = TEXT("support not allowed");
		return false;
	}

	const FPlacementSignature& Signature = Catalog.Signatures[Attempt.Signature];
	const FPoseFrame Frame = MakePoseFrame(Plane->Axes, Pose.Rotation);

	for (int32 X = 0; X < Signature.SizeCells.X; ++X)
	{
		for (int32 Y = 0; Y < Signature.SizeCells.Y; ++Y)
		{
			if (!IsCellAvailable(*Plane, LocalToPlaneCell(Frame, Pose.Origin, FIntPoint(X, Y))))
			{
				OutCheck.Failure = TEXT("footprint not free");
				return false;
			}
		}
	}

	// A stacked object stands within the height its layer leaves it.
	if (Plane->bHosted && Plane->ClearanceCm > 0.0
		&& Signature.SizeCells.Z * World.GetCellSize() > Plane->ClearanceCm + PlacementSolverPrivate::ClearanceTolerance)
	{
		OutCheck.Failure = TEXT("taller than its layer's clearance");
		return false;
	}

	TArray<FIntVector> Volume;
	if (!GatherVolume(*Plane, Frame, Pose.Origin, Signature.SizeCells, Volume))
	{
		OutCheck.Failure = TEXT("footprint off its host");
		return false;
	}

	for (const FIntVector& Cell : Volume)
	{
		if (!World.IsValidCell(Cell) || World.IsCellSolid(Cell))
		{
			OutCheck.Failure = TEXT("volume meets structure or the cube's edge");
			return false;
		}

		// Whatever it is stacked on stands in these cells too, and is not in its way.
		const int32* Owner = ReservedCells.Find(WEMGrid::MakeCellKey(Cell));

		if (Owner && !Plane->HostChain.Contains(*Owner))
		{
			OutCheck.Failure = TEXT("volume meets another object");
			return false;
		}
	}

	if (!EvaluateContacts(Attempt, *Plane, Frame, Pose.Origin, OutCheck))
	{
		return false;
	}

	if (!DoZonesHold(Signature, *Plane, Frame, Pose.Origin))
	{
		OutCheck.Failure = TEXT("exclusion zone");
		return false;
	}

	return true;
}

bool FPlacementSolver::FindPose(const FPlacementAttempt& Attempt, FPlacementPose& OutPose, FPoseCheck& OutCheck)
{
	using namespace WEMPlacement;

	const FPlacementSignature& Signature = Catalog.Signatures[Attempt.Signature];
	const FContactRequirement* Anchor = ChooseAnchor(Attempt.Rule);

	TArray<FEdgeFace> AnchorFaces;
	if (Anchor)
	{
		for (const FEdgeFace& Face : Signature.EdgeFaces)
		{
			if (Face.Side == Anchor->Side)
			{
				AnchorFaces.Add(Face);
			}
		}
	}

	// Each seed stands for this many poses: a free cell for all four ways round, an anchor seed for
	// each face of the anchor edge that could be the one lying on it.
	const int32 PosesPerSeed = Anchor ? AnchorFaces.Num() : 4;

	if (PosesPerSeed == 0)
	{
		return false;
	}

	// The seeds, plane by plane, each block numbered on from the one before - or a whole group of
	// hosted planes at once, numbered through by the group's running totals.
	struct FSeedBlock
	{
		uint64 Plane = 0;
		const TArray<FIntPoint>* Cells = nullptr;
		const TArray<FPoseSeed>* Seeds = nullptr;
		const FHostedPlaneGroup* Group = nullptr;
		const TArray<int32>* GroupSeedsBefore = nullptr;
		int32 First = 0;
	};

	TArray<FSeedBlock> Blocks;

	// Only the planes the attempt could stand on are walked, since every object placed brings a plane
	// of its own for each of its surfaces, and there are soon thousands. A companion stands on its
	// host's surfaces, or beside the host on the plane the host stands on - a contact with the host
	// holds nowhere else. Otherwise the segment planes are walked, which sort before every hosted
	// one, and the hosted planes are taken a group at a time: their cells and boundary seeds are
	// fixed when they are made. Only seeds grown from the objects standing on a hosted plane change
	// as it fills, so those are still gathered plane by plane. Every plane passed over is one
	// IsPlaneAllowed would turn away, or one where no pose could hold its contact with the host.
	TArray<uint64, TInlineAllocator<4>> HostPlanes;
	TConstArrayView<uint64> CandidatePlanes = PlaneOrder;
	bool bHostedByGroup = false;

	if (Attempt.RestrictToHost != INDEX_NONE)
	{
		if (const FPlacedObject* Host = FindObject(Attempt.RestrictToHost))
		{
			// Already in the planes' order: whatever the host stands on was there before the host.
			HostPlanes.Add(Host->Pose.Plane);
			HostPlanes.Append(Host->HostedPlanes);
		}

		CandidatePlanes = HostPlanes;
	}
	else
	{
		const int32 FirstHosted = Algo::LowerBound(PlaneOrder, PlacementSolverPrivate::HostedPlaneBit);
		const bool bSegments = (Attempt.SupportMask & SupportBit(ESupportKind::Segment)) != 0;
		const bool bHosted = (Attempt.SupportMask & SupportBit(ESupportKind::ObjectSurface)) != 0;

		bHostedByGroup = bHosted && (!Anchor || Anchor->Target != EContactTarget::Object);

		if (!bHosted || bHostedByGroup)
		{
			CandidatePlanes = bSegments ? CandidatePlanes.Left(FirstHosted) : TConstArrayView<uint64>();
		}
		else if (!bSegments)
		{
			CandidatePlanes = CandidatePlanes.RightChop(FirstHosted);
		}
	}

	// Object seeds depend on what stands, so they are made per draw. Reserved, so none moves while
	// a block points at it.
	TArray<TArray<FPoseSeed>> ObjectSeeds;
	ObjectSeeds.Reserve(CandidatePlanes.Num());

	int32 Total = 0;

	for (const uint64 PlaneId : CandidatePlanes)
	{
		FSupportPlane& Plane = Planes[PlaneId];

		if (!IsPlaneAllowed(Plane, Attempt))
		{
			continue;
		}

		FSeedBlock Block;
		Block.Plane = PlaneId;
		Block.First = Total;

		if (!Anchor)
		{
			Block.Cells = &Plane.SeedCells;
		}
		else if (Anchor->Target == EContactTarget::Object)
		{
			TArray<FPoseSeed>& Seeds = ObjectSeeds.AddDefaulted_GetRef();

			for (const int32 Target : Plane.Objects)
			{
				if (!IsContactTarget(*Anchor, Attempt, Target))
				{
					continue;
				}

				for (const FIntVector& Key : Objects[Target - 1].PerimeterKeys)
				{
					const FPlacedPerimeterFace& Face = Plane.PerimeterFaces[Key];

					if (PlacementSolverPrivate::IsOnTargetEdge(*Anchor, Face) && !(Anchor->bExcludeTargetCorners && Face.bCorner))
					{
						// The cell across the target's face, turned to look back at it.
						const FIntPoint Outward = PlaneDirections[Key.Z];
						Seeds.Add({ FIntPoint(Key.X, Key.Y) + Outward, GetPlaneDirectionIndex(FIntPoint(-Outward.X, -Outward.Y)) });
					}
				}
			}

			Block.Seeds = &Seeds;
		}
		else
		{
			EnsureAnchorSeeds(Plane, Anchor->Target);
			Block.Seeds = Anchor->Target == EContactTarget::Wall ? &Plane.WallSeeds : &Plane.BoundarySeeds;
		}

		const int32 Count = (Block.Cells ? Block.Cells->Num() : Block.Seeds->Num()) * PosesPerSeed;

		if (Count > 0)
		{
			Blocks.Add(Block);
			Total += Count;
		}
	}

	// A hosted plane has no structure beside it, so no wall seeds: only its cells, or the edge of it.
	if (bHostedByGroup && (!Anchor || Anchor->Target == EContactTarget::SupportBoundary))
	{
		for (const FHostedPlaneGroup& Group : HostedPlaneGroups)
		{
			const int32 GroupSeeds = Anchor ? Group.BoundarySeeds : Group.Cells;

			if (GroupSeeds == 0 || !DoesHostAccept(Group.HostArchetype, Signature.Archetype))
			{
				continue;
			}

			FSeedBlock Block;
			Block.Group = &Group;
			Block.GroupSeedsBefore = Anchor ? &Group.BoundarySeedsBefore : &Group.CellsBefore;
			Block.First = Total;

			Blocks.Add(Block);
			Total += GroupSeeds * PosesPerSeed;
		}
	}

	if (Total == 0)
	{
		return false;
	}

	// Drawn one at a time from a random order, stopping at the first that passes. The first valid
	// element of a uniformly random order is uniformly random among the valid ones, so this is as fair
	// as testing every pose and drawing from those that pass, and seldom tests more than a few.
	//
	// The order is shuffled as it is drawn - swap by swap, as a shuffle of the whole list would, but
	// remembering only the places a swap has moved - so a draw costs the same however many poses
	// there are to draw from.
	TMap<int32, int32> Moved;

	auto GetOrderAt = [&Moved](const int32 Place)
	{
		const int32* Found = Moved.Find(Place);
		return Found ? *Found : Place;
	};

	TSet<FPlacementPose> Tested;
	const int32 Draws = FMath::Min(Total, PlacementSolverPrivate::MaxPoseDraws);

	for (int32 Drawn = 0; Drawn < Draws; ++Drawn)
	{
		const int32 Swap = Stream.RandRange(Drawn, Total - 1);
		const int32 Index = GetOrderAt(Swap);
		Moved.Add(Swap, GetOrderAt(Drawn));

		const FSeedBlock& Block = Blocks[Algo::UpperBoundBy(Blocks, Index, &FSeedBlock::First) - 1];

		int32 Seed = (Index - Block.First) / PosesPerSeed;
		const int32 Variant = (Index - Block.First) % PosesPerSeed;

		uint64 PlaneId = Block.Plane;
		const TArray<FIntPoint>* Cells = Block.Cells;
		const TArray<FPoseSeed>* Seeds = Block.Seeds;

		if (Block.Group)
		{
			// Which of the group's planes the seed falls in - the last to start at or before it - and
			// which of that plane's own seeds it is.
			const TArray<int32>& SeedsBefore = *Block.GroupSeedsBefore;
			const int32 PlaneIndex = Algo::UpperBound(SeedsBefore, Seed) - 1;

			PlaneId = Block.Group->Planes[PlaneIndex];
			Seed -= SeedsBefore[PlaneIndex];

			const FSupportPlane& Hosted = Planes[PlaneId];
			Cells = Anchor ? nullptr : &Hosted.SeedCells;
			Seeds = Anchor ? &Hosted.BoundarySeeds : nullptr;
		}

		const FSupportPlane& Plane = Planes[PlaneId];

		FPlacementPose Pose;
		Pose.Plane = PlaneId;
		FIntPoint Cell;

		if (Cells)
		{
			Cell = (*Cells)[Seed];
			Pose.Rotation = Variant;
			Pose.Origin = Cell;
		}
		else
		{
			const FPoseSeed& Anchored = (*Seeds)[Seed];
			Cell = Anchored.Cell;
			Pose.Rotation = FindRotationFacing(Plane.Axes, Anchor->Side, PlaneDirections[Anchored.Direction]);

			if (Pose.Rotation == INDEX_NONE)
			{
				continue;
			}

			Pose.Origin = Cell - LocalToPlaneDirection(MakePoseFrame(Plane.Axes, Pose.Rotation), AnchorFaces[Variant].Cell);
		}

		if (!IsCellAvailable(Plane, Cell))
		{
			continue;
		}

		// Two seeds can grow the same pose; it is only worth testing once.
		bool bAlreadyTested = false;
		Tested.Add(Pose, &bAlreadyTested);

		if (bAlreadyTested)
		{
			continue;
		}

		if (TestPose(Attempt, Pose, OutCheck))
		{
			OutPose = Pose;
			return true;
		}
	}

	if (Draws < Total)
	{
		bSearchCutShort = true;
	}

	return false;
}

int32 FPlacementSolver::RollWeighted(const TConstArrayView<float> Weights)
{
	float Total = 0.0f;
	for (const float Weight : Weights)
	{
		Total += FMath::Max(Weight, 0.0f);
	}

	if (Total <= 0.0f)
	{
		return INDEX_NONE;
	}

	float Roll = Stream.FRand() * Total;
	int32 Chosen = INDEX_NONE;

	for (int32 Index = 0; Index < Weights.Num(); ++Index)
	{
		if (Weights[Index] <= 0.0f)
		{
			continue;
		}

		// Taken whenever it is in the running, so rounding at the very top of the roll still lands
		// on the last index that could have been drawn.
		Chosen = Index;

		if (Roll < Weights[Index])
		{
			break;
		}

		Roll -= Weights[Index];
	}

	return Chosen;
}

float FPlacementSolver::GetSignatureDrawWeight(const int32 SignatureIndex) const
{
	float Weight = 0.0f;

	for (const int32 EntryIndex : Catalog.Signatures[SignatureIndex].Entries)
	{
		const FPlacementEntry& Entry = Catalog.Entries[EntryIndex];

		if (Entry.HasRoom())
		{
			Weight += Entry.Weight;
		}
	}

	return Weight;
}

void FPlacementSolver::GetArchetypeDrawWeights(TArray<float>& OutWeights) const
{
	OutWeights.Reset();

	const int32 Revision = GetRevision();

	for (const FPlacementKind& Kind : Catalog.Kinds)
	{
		const UPlacementArchetype* Archetype = Kind.Archetype;
		const bool bHasRoom = Archetype->MaxInstances <= 0 || Kind.Instances < Archetype->MaxInstances;

		const bool bHasSignature = Kind.Signatures.ContainsByPredicate([&](const int32 Signature)
		{
			return Catalog.Signatures[Signature].InfeasibleAtRevision != Revision && GetSignatureDrawWeight(Signature) > 0.0f;
		});

		OutWeights.Add(bHasRoom && bHasSignature ? FMath::Max(Archetype->SpawnWeight, 0.0f) : 0.0f);
	}
}

bool FPlacementSolver::TryRules(const int32 SignatureIndex, FPlacementPose& OutPose, FPoseCheck& OutCheck, int32& OutRuleIndex, const int32 RestrictToHost)
{
	const UPlacementArchetype* Archetype = Catalog.Signatures[SignatureIndex].Archetype;
	const TArray<FPlacementRule>& Rules = Archetype->Rules;

	auto TryRule = [&](const FPlacementRule* Rule)
	{
		FPlacementAttempt Attempt;
		Attempt.Signature = SignatureIndex;
		Attempt.Rule = Rule;
		Attempt.SupportMask = Archetype->AllowedSupports & (Rule ? Rule->SupportMask : ~0);
		Attempt.RestrictToHost = RestrictToHost;

		return Attempt.SupportMask != 0 && FindPose(Attempt, OutPose, OutCheck);
	};

	// An archetype with no rules goes anywhere it fits.
	if (Rules.IsEmpty())
	{
		OutRuleIndex = INDEX_NONE;
		return TryRule(nullptr);
	}

	if (Archetype->RuleSelection == ERuleSelection::Priority)
	{
		for (int32 RuleIndex = 0; RuleIndex < Rules.Num(); ++RuleIndex)
		{
			if (TryRule(&Rules[RuleIndex]))
			{
				OutRuleIndex = RuleIndex;
				return true;
			}
		}

		return false;
	}

	// By weight, among the rules not yet tried. A rule that finds nothing drops out and the roll is
	// made again, which comes to weighting only among the rules that can place - as the room
	// manager's tile roll does.
	TArray<float> Weights;
	for (const FPlacementRule& Rule : Rules)
	{
		Weights.Add(FMath::Max(Rule.Weight, 0.0f));
	}

	for (int32 RuleIndex = RollWeighted(Weights); RuleIndex != INDEX_NONE; RuleIndex = RollWeighted(Weights))
	{
		if (TryRule(&Rules[RuleIndex]))
		{
			OutRuleIndex = RuleIndex;
			return true;
		}

		Weights[RuleIndex] = 0.0f;
	}

	return false;
}

bool FPlacementSolver::PlaceNext(const int32 MaxAttempts, TArray<int32>& OutPlaced)
{
	using namespace PlacementSolverPrivate;

	OutPlaced.Reset();
	LastRefreshMilliseconds = 0.0;

	// Following the structure costs what its latest placements touched, so the planes are brought
	// up to date on every beat and always offer exactly the room it has.
	RefreshSegmentPlanes();

	return PlaceFromDraws(MaxAttempts, OutPlaced);
}

bool FPlacementSolver::PlaceFromDraws(const int32 MaxAttempts, TArray<int32>& OutPlaced)
{
	for (int32 Attempt = 0; Attempt < FMath::Max(1, MaxAttempts); ++Attempt)
	{
		TArray<float> KindWeights;
		GetArchetypeDrawWeights(KindWeights);

		const int32 KindIndex = RollWeighted(KindWeights);

		// Every archetype is full, or has nowhere to go until something changes.
		if (KindIndex == INDEX_NONE)
		{
			return false;
		}

		const FPlacementKind& Kind = Catalog.Kinds[KindIndex];
		const int32 Revision = GetRevision();

		TArray<float> SignatureWeights;
		for (const int32 Signature : Kind.Signatures)
		{
			const bool bOpen = Catalog.Signatures[Signature].InfeasibleAtRevision != Revision;
			SignatureWeights.Add(bOpen ? GetSignatureDrawWeight(Signature) : 0.0f);
		}

		const int32 SignatureIndex = Kind.Signatures[RollWeighted(SignatureWeights)];
		const UPlacementArchetype* Archetype = Kind.Archetype;

		FPlacementPose Pose;
		FPoseCheck Check;
		int32 RuleIndex = INDEX_NONE;

		bSearchCutShort = false;
		bool bFound = TryRules(SignatureIndex, Pose, Check, RuleIndex);

		if (!bFound && Archetype->FallbackChance > 0.0f && Stream.FRand() < Archetype->FallbackChance)
		{
			FPlacementAttempt Fallback;
			Fallback.Signature = SignatureIndex;
			Fallback.SupportMask = Archetype->AllowedSupports;

			bFound = FindPose(Fallback, Pose, Check);
			RuleIndex = INDEX_NONE;
		}

		if (!bFound)
		{
			// Nothing about the structure or the objects has changed since this failed, so nothing
			// of this signature can be placed until something does - when every pose was tried.
			// A search that gave up early only found nothing among the poses it drew.
			if (!bSearchCutShort)
			{
				Catalog.Signatures[SignatureIndex].InfeasibleAtRevision = Revision;
			}

			continue;
		}

		// The mesh last: every mesh of the signature fits the pose just found.
		TArray<float> EntryWeights;
		const TArray<int32>& Entries = Catalog.Signatures[SignatureIndex].Entries;

		for (const int32 EntryIndex : Entries)
		{
			const FPlacementEntry& Entry = Catalog.Entries[EntryIndex];
			EntryWeights.Add(Entry.HasRoom() ? Entry.Weight : 0.0f);
		}

		FPlacementAttempt Placed;
		Placed.Signature = SignatureIndex;
		Placed.Rule = RuleIndex != INDEX_NONE ? &Archetype->Rules[RuleIndex] : nullptr;
		Placed.SupportMask = Archetype->AllowedSupports;

		const int32 Id = Commit(Placed, Pose, Check, Entries[RollWeighted(EntryWeights)], RuleIndex);
		OutPlaced.Add(Id);

		PlaceCompanions(Id, OutPlaced);
		return true;
	}

	return false;
}

void FPlacementSolver::PlaceCompanions(const int32 Host, TArray<int32>& OutPlaced)
{
	// Copied, since placing a companion adds to the list the host is in.
	const TArray<FCompanion> Companions = Objects[Host - 1].Archetype->Companions;

	for (const FCompanion& Companion : Companions)
	{
		if (!Companion.Archetype || Stream.FRand() >= Companion.Chance)
		{
			continue;
		}

		const int32 Count = Stream.RandRange(FMath::Min(Companion.MinCount, Companion.MaxCount), FMath::Max(Companion.MinCount, Companion.MaxCount));

		for (int32 Placed = 0; Placed < Count; ++Placed)
		{
			const int32 Id = PlaceCompanion(Host, Companion);

			// Placing one more only takes room away, so once one has nowhere to go, none after it will.
			if (Id == INDEX_NONE)
			{
				break;
			}

			OutPlaced.Add(Id);
		}
	}
}

int32 FPlacementSolver::PlaceCompanion(const int32 Host, const FCompanion& Companion)
{
	const FPlacementKind* Kind = Catalog.Kinds.FindByPredicate([&Companion](const FPlacementKind& Candidate) { return Candidate.Archetype == Companion.Archetype; });

	if (!Kind || (Kind->Archetype->MaxInstances > 0 && Kind->Instances >= Kind->Archetype->MaxInstances))
	{
		return INDEX_NONE;
	}

	const FName HostSet = Catalog.Entries[Objects[Host - 1].Entry].SetName;

	// First among the rows of the host's own set, so the oak desk gets the oak chair; then among all.
	for (const bool bSameSet : { true, false })
	{
		if (bSameSet && (!Companion.bPreferSameSet || HostSet.IsNone()))
		{
			continue;
		}

		auto IsWanted = [&](const FPlacementEntry& Entry)
		{
			return Entry.HasRoom() && Entry.Weight > 0.0f && (!bSameSet || Entry.SetName == HostSet);
		};

		TArray<float> SignatureWeights;
		for (const int32 Signature : Kind->Signatures)
		{
			float Weight = 0.0f;
			for (const int32 EntryIndex : Catalog.Signatures[Signature].Entries)
			{
				Weight += IsWanted(Catalog.Entries[EntryIndex]) ? Catalog.Entries[EntryIndex].Weight : 0.0f;
			}

			SignatureWeights.Add(Weight);
		}

		for (int32 Pick = RollWeighted(SignatureWeights); Pick != INDEX_NONE; Pick = RollWeighted(SignatureWeights))
		{
			const int32 SignatureIndex = Kind->Signatures[Pick];

			FPlacementPose Pose;
			FPoseCheck Check;
			int32 RuleIndex = INDEX_NONE;

			if (!TryRules(SignatureIndex, Pose, Check, RuleIndex, Host))
			{
				SignatureWeights[Pick] = 0.0f;
				continue;
			}

			const TArray<int32>& Entries = Catalog.Signatures[SignatureIndex].Entries;
			TArray<float> EntryWeights;

			for (const int32 EntryIndex : Entries)
			{
				EntryWeights.Add(IsWanted(Catalog.Entries[EntryIndex]) ? Catalog.Entries[EntryIndex].Weight : 0.0f);
			}

			const UPlacementArchetype* Archetype = Kind->Archetype;

			FPlacementAttempt Placed;
			Placed.Signature = SignatureIndex;
			Placed.Rule = RuleIndex != INDEX_NONE ? &Archetype->Rules[RuleIndex] : nullptr;
			Placed.SupportMask = Archetype->AllowedSupports;
			Placed.RestrictToHost = Host;

			return Commit(Placed, Pose, Check, Entries[RollWeighted(EntryWeights)], RuleIndex, Host);
		}
	}

	return INDEX_NONE;
}

int32 FPlacementSolver::TryPlaceAt(
	const FPlacementAttempt& Attempt,
	const FPlacementPose& Pose,
	const int32 Entry,
	const int32 RuleIndex,
	FPoseCheck* OutCheck)
{
	RefreshSegmentPlanes();

	FPoseCheck Check;
	const bool bPasses = TestPose(Attempt, Pose, Check);

	if (OutCheck)
	{
		*OutCheck = Check;
	}

	return bPasses ? Commit(Attempt, Pose, Check, Entry, RuleIndex) : INDEX_NONE;
}

int32 FPlacementSolver::Commit(
	const FPlacementAttempt& Attempt,
	const FPlacementPose& Pose,
	const FPoseCheck& Check,
	const int32 EntryIndex,
	const int32 RuleIndex,
	const int32 CompanionOf)
{
	using namespace WEMPlacement;

	FPlacementEntry& Entry = Catalog.Entries[EntryIndex];
	const FPlacementSignature& Signature = Catalog.Signatures[Attempt.Signature];
	FSupportPlane& Plane = Planes[Pose.Plane];
	const double CellSize = World.GetCellSize();

	FPlacedObject& Object = Objects.AddDefaulted_GetRef();
	Object.Id = Objects.Num();
	Object.Entry = EntryIndex;
	Object.Signature = Attempt.Signature;
	Object.Archetype = Signature.Archetype;
	Object.Pose = Pose;
	Object.Frame = MakePoseFrame(Plane.Axes, Pose.Rotation);
	Object.Rule = RuleIndex;
	Object.Contacts = Check;
	Object.Host = Plane.bHosted ? Plane.Host : INDEX_NONE;
	Object.CompanionOf = CompanionOf;

	const int32 Id = Object.Id;

	for (int32 X = 0; X < Signature.SizeCells.X; ++X)
	{
		for (int32 Y = 0; Y < Signature.SizeCells.Y; ++Y)
		{
			const FIntPoint Cell = LocalToPlaneCell(Object.Frame, Pose.Origin, FIntPoint(X, Y));
			Object.Footprint.Add(Cell);
			Plane.Footprints.Add(Cell, Id);

			if (!Plane.bHosted)
			{
				Object.SupportCells.Add(SegmentPlaneCellToWorld(Plane.Face, Plane.Layer, Cell));
			}
		}
	}

	// Its perimeter, where contacts from objects placed later will look for it, with its special
	// edges marked on the faces they run along.
	for (const FEdgeFace& Face : Signature.EdgeFaces)
	{
		const FIntPoint Cell = LocalToPlaneCell(Object.Frame, Pose.Origin, Face.Cell);
		const FIntVector Key = PlacementSolverPrivate::MakePerimeterKey(Cell, GetPlaneDirectionIndex(LocalToPlaneDirection(Object.Frame, Face.Direction)));

		FPlacedPerimeterFace& Perimeter = Plane.PerimeterFaces.Add(Key);
		Perimeter.Object = Id;
		Perimeter.Side = Face.Side;
		Perimeter.Index = Face.Index;
		Perimeter.bCorner = Face.bCorner;

		for (const FSpecialEdge& Special : Signature.SpecialEdges)
		{
			if (Special.Side == Face.Side && Face.Index >= Special.Start && Face.Index < Special.Start + Special.Count)
			{
				Perimeter.SpecialEdgeIds.Add(Special.Id);
			}
		}

		Object.PerimeterKeys.Add(Key);
	}

	GatherZoneCells(Signature, Object.Frame, Pose.Origin, Object.ZoneCells);

	for (const TPair<FIntPoint, int32>& ZoneCell : Object.ZoneCells)
	{
		Plane.Zones.FindOrAdd(ZoneCell.Key).Add({ Id, ZoneCell.Value });
	}

	TArray<FIntVector> Volume;
	GatherVolume(Plane, Object.Frame, Pose.Origin, Signature.SizeCells, Volume);

	Object.VolumeMin = Volume.IsEmpty() ? FIntVector::ZeroValue : Volume[0];
	Object.VolumeMax = Object.VolumeMin;

	for (const FIntVector& Cell : Volume)
	{
		// A stacked object shares cells with what it stands on, which keeps them.
		ReservedCells.FindOrAdd(WEMGrid::MakeCellKey(Cell), Id);

		Object.VolumeMin = FIntVector(FMath::Min(Object.VolumeMin.X, Cell.X), FMath::Min(Object.VolumeMin.Y, Cell.Y), FMath::Min(Object.VolumeMin.Z, Cell.Z));
		Object.VolumeMax = FIntVector(FMath::Max(Object.VolumeMax.X, Cell.X), FMath::Max(Object.VolumeMax.Y, Cell.Y), FMath::Max(Object.VolumeMax.Z, Cell.Z));
	}

	const FVector FootprintCentre = 0.5 * (GetPlaneCellCentre(Plane, Object.Footprint[0]) + GetPlaneCellCentre(Plane, Object.Footprint.Last()));

	Object.Transform = MakeObjectWorldTransform(
		Object.Frame, FootprintCentre, Entry.Geometry.LocalBoundsCm, Signature.SizeCells, CellSize, Check.ContactSideMask);

	// A plane on top of it for each surface layer of the mesh that was drawn, taking its frame from
	// where the mesh actually stands, so what is stacked on it stands on it.
	const TArray<int32> HostChain = [&Plane, Id]()
	{
		TArray<int32> Chain = Plane.HostChain;
		Chain.Insert(Id, 0);
		return Chain;
	}();

	// Made apart and added together afterwards: adding to Planes can move the plane this stands on.
	TArray<FSupportPlane> HostedPlanes;

	for (int32 LayerIndex = 0; LayerIndex < Entry.Geometry.Surfaces.Num(); ++LayerIndex)
	{
		const FDerivedSurfaceLayer& Layer = Entry.Geometry.Surfaces[LayerIndex];
		const uint64 HostedId = MakeHostedPlaneId(Id, LayerIndex);

		FSupportPlane& Hosted = HostedPlanes.AddDefaulted_GetRef();
		Hosted.Id = HostedId;
		Hosted.bHosted = true;
		Hosted.Host = Id;
		Hosted.HostLayer = LayerIndex;
		Hosted.Axes.Normal = Object.Frame.Up;
		Hosted.Axes.AxisA = Object.Frame.Forward;
		Hosted.Axes.AxisB = Object.Frame.Right;
		Hosted.BaseOffsetCm = Plane.BaseOffsetCm + Layer.HeightCm;
		Hosted.LayerHeightCm = Layer.HeightCm;
		Hosted.ClearanceCm = Layer.ClearanceCm;
		Hosted.HostArchetype = Signature.Archetype;
		Hosted.HostTransform = Object.Transform;
		Hosted.HostBounds = Entry.Geometry.LocalBoundsCm;
		Hosted.HostSize = Signature.SizeCells;
		Hosted.HostChain = HostChain;
		Hosted.Name = FString::Printf(TEXT("#%d surface %d (%.0f cm)"), Id, LayerIndex, Layer.HeightCm);

		for (int32 X = 0; X < Signature.SizeCells.X; ++X)
		{
			for (int32 Y = 0; Y < Signature.SizeCells.Y; ++Y)
			{
				FIntVector Base;
				if (GetColumnBase(Plane, LocalToPlaneCell(Object.Frame, Pose.Origin, FIntPoint(X, Y)), Base))
				{
					Hosted.ColumnBases.Add(FIntPoint(X, Y), Base);
				}

				const int32 MaskIndex = GetFootprintIndex(Signature.SizeCells, X, Y);

				if (Layer.CellMask.IsValidIndex(MaskIndex) && Layer.CellMask[MaskIndex] != 0)
				{
					Hosted.LayerCells.Add(FIntPoint(X, Y));
					Hosted.SeedCells.Add(FIntPoint(X, Y));
				}
			}
		}

		Object.HostedPlanes.Add(HostedId);
	}

	Plane.Objects.Add(Id);

	for (FSupportPlane& Hosted : HostedPlanes)
	{
		const uint64 HostedId = Hosted.Id;
		FSupportPlane& Added = Planes.Add(HostedId, MoveTemp(Hosted));
		AddPlaneToOrder(HostedId);

		// Its seeds are all known now and never change, so it is counted into its host archetype's
		// group straight away, boundary seeds and all.
		EnsureAnchorSeeds(Added, EContactTarget::SupportBoundary);

		FHostedPlaneGroup* Group = HostedPlaneGroups.FindByPredicate([&Added](const FHostedPlaneGroup& Candidate)
		{
			return Candidate.HostArchetype == Added.HostArchetype;
		});

		if (!Group)
		{
			Group = &HostedPlaneGroups.AddDefaulted_GetRef();
			Group->HostArchetype = Added.HostArchetype;
		}

		Group->Planes.Add(HostedId);
		Group->CellsBefore.Add(Group->Cells);
		Group->BoundarySeedsBefore.Add(Group->BoundarySeeds);
		Group->Cells += Added.SeedCells.Num();
		Group->BoundarySeeds += Added.BoundarySeeds.Num();
	}

	++Entry.Instances;

	for (FPlacementKind& Kind : Catalog.Kinds)
	{
		if (Kind.Archetype == Signature.Archetype)
		{
			++Kind.Instances;
		}
	}

	++ObjectRevision;

	FPlacedObject& Placed = Objects[Id - 1];
	Placed.Description = DescribeObject(Placed);
	return Id;
}

FVector FPlacementSolver::GetPlaneCellCentre(const FSupportPlane& Plane, const FIntPoint& Cell) const
{
	if (!Plane.bHosted)
	{
		return World.GetFaceCentre(WEMPlacement::SegmentPlaneCellToWorld(Plane.Face, Plane.Layer, Cell), Plane.Face);
	}

	return Plane.HostTransform.TransformPosition(
		WEMPlacement::GetFootprintCellLocalCentre(Plane.HostBounds, Plane.HostSize, World.GetCellSize(), Cell, Plane.LayerHeightCm));
}

FString FPlacementSolver::DescribeAcross(const FSupportPlane& Plane, const FIntPoint& Cell, const FIntPoint& Direction) const
{
	const FIntPoint Across = Cell + Direction;

	if (const int32* Standing = Plane.Footprints.Find(Across))
	{
		const FPlacedObject& Other = Objects[*Standing - 1];
		const int32 Facing = WEMPlacement::GetPlaneDirectionIndex(FIntPoint(-Direction.X, -Direction.Y));
		const FPlacedPerimeterFace* Face = Plane.PerimeterFaces.Find(PlacementSolverPrivate::MakePerimeterKey(Across, Facing));

		return FString::Printf(TEXT("Object %s#%d %s"),
			*Catalog.Entries[Other.Entry].RowName.ToString(), Other.Id, Face ? WEMPlacement::GetSideName(Face->Side) : TEXT("?"));
	}

	FIntVector Base;
	if (!Plane.bHosted && GetColumnBase(Plane, Cell, Base)
		&& World.IsCellSolid(Base + Plane.Axes.Normal + WEMPlacement::PlaneToWorldDirection(Plane.Axes, Direction)))
	{
		return TEXT("Wall");
	}

	return IsSupportCell(Plane, Across) ? TEXT("free") : TEXT("boundary");
}

FString FPlacementSolver::DescribeObject(const FPlacedObject& Object) const
{
	using namespace WEMPlacement;

	const FPlacementEntry& Entry = Catalog.Entries[Object.Entry];
	const FPlacementSignature& Signature = Catalog.Signatures[Object.Signature];
	const FSupportPlane& Plane = Planes[Object.Pose.Plane];
	const UPlacementArchetype* Archetype = Object.Archetype;
	const FIntVector& Size = Signature.SizeCells;

	FString RuleText;
	if (Object.Rule == INDEX_NONE)
	{
		RuleText = Archetype->Rules.IsEmpty() ? TEXT("anywhere [no rules]") : TEXT("fallback [surface]");
	}
	else
	{
		const FPlacementRule& Rule = Archetype->Rules[Object.Rule];
		const FString Label = Rule.Label.IsNone() ? FString::Printf(TEXT("rule %d"), Object.Rule + 1) : Rule.Label.ToString();

		RuleText = FString::Printf(TEXT("rule \"%s\" [%s %d of %d]"), *Label,
			Archetype->RuleSelection == ERuleSelection::Priority ? TEXT("priority") : TEXT("weighted"), Object.Rule + 1, Archetype->Rules.Num());
	}

	if (Object.CompanionOf != INDEX_NONE)
	{
		RuleText += FString::Printf(TEXT(" as a companion of #%d"), Object.CompanionOf);
	}

	FString Text = FString::Printf(TEXT("ObjectPlacer: #%d %s [%s, %s, set %s] %s on %s, origin (%d,%d), front \u2192 %s, up %s, size %dx%dx%d"),
		Object.Id,
		*Entry.RowName.ToString(),
		*Archetype->GetName(),
		Archetype->Classification.IsValid() ? *Archetype->Classification.ToString() : TEXT("-"),
		Entry.SetName.IsNone() ? TEXT("-") : *Entry.SetName.ToString(),
		*RuleText,
		*Plane.Name,
		Object.Pose.Origin.X, Object.Pose.Origin.Y,
		*DescribeDirection(Object.Frame.Forward),
		*DescribeDirection(Object.Frame.Up),
		Size.X, Size.Y, Size.Z);

	for (const EObjectSide Side : { EObjectSide::Back, EObjectSide::Front, EObjectSide::Left, EObjectSide::Right })
	{
		FIntPoint Min(MAX_int32, MAX_int32);
		FIntPoint Max(MIN_int32, MIN_int32);

		// What lies across each face, counted, in the order first met along the edge.
		TArray<TPair<FString, int32>> Across;
		int32 FaceCount = 0;

		TArray<FEdgeFace> Faces = Signature.EdgeFaces.FilterByPredicate([Side](const FEdgeFace& Face) { return Face.Side == Side; });
		Faces.Sort([](const FEdgeFace& A, const FEdgeFace& B) { return A.Index < B.Index; });

		for (const FEdgeFace& Face : Faces)
		{
			const FIntPoint Cell = LocalToPlaneCell(Object.Frame, Object.Pose.Origin, Face.Cell);
			Min = FIntPoint(FMath::Min(Min.X, Cell.X), FMath::Min(Min.Y, Cell.Y));
			Max = FIntPoint(FMath::Max(Max.X, Cell.X), FMath::Max(Max.Y, Cell.Y));
			++FaceCount;

			const FString What = DescribeAcross(Plane, Cell, LocalToPlaneDirection(Object.Frame, Face.Direction));
			TPair<FString, int32>* Existing = Across.FindByPredicate([&What](const TPair<FString, int32>& Pair) { return Pair.Key == What; });

			if (Existing)
			{
				++Existing->Value;
			}
			else
			{
				Across.Emplace(What, 1);
			}
		}

		TArray<FString> AcrossText;
		for (const TPair<FString, int32>& Pair : Across)
		{
			AcrossText.Add(FString::Printf(TEXT("%s x%d"), *Pair.Key, Pair.Value));
		}

		FString Specials;
		for (const FSpecialEdge& Special : Signature.SpecialEdges)
		{
			if (Special.Side == Side)
			{
				Specials += FString::Printf(TEXT("   [special %d: faces %d-%d]"), Special.Id, Special.Start, Special.Start + Special.Count - 1);
			}
		}

		Text += FString::Printf(TEXT("\n  %-6s %d %-5s  cells (%d,%d)..(%d,%d)  %s%s"),
			GetSideName(Side), FaceCount, FaceCount == 1 ? TEXT("face") : TEXT("faces"),
			Min.X, Min.Y, Max.X, Max.Y, *FString::Join(AcrossText, TEXT(", ")), *Specials);
	}

	// A map of the plane around it, in the monospace log.
	int32 Margin = 2;
	for (const FExclusionZone& Zone : Archetype->ExclusionZones)
	{
		Margin = FMath::Max(Margin, Zone.DepthCells + 1);
	}

	Margin = FMath::Min(Margin, PlacementSolverPrivate::MaxMapMargin);

	FIntPoint Low(MAX_int32, MAX_int32);
	FIntPoint High(MIN_int32, MIN_int32);
	for (const FIntPoint& Cell : Object.Footprint)
	{
		Low = FIntPoint(FMath::Min(Low.X, Cell.X), FMath::Min(Low.Y, Cell.Y));
		High = FIntPoint(FMath::Max(High.X, Cell.X), FMath::Max(High.Y, Cell.Y));
	}

	Low -= FIntPoint(Margin, Margin);
	High += FIntPoint(Margin, Margin);

	Text += FString::Printf(TEXT("\n  %s, a = %s across from %d to %d, b = %s up: B/F/L/R edges, C corners, . inside, # structure, o objects, z zones, _ open support"),
		*Plane.Name, *DescribeDirection(Plane.Axes.AxisA), Low.X, High.X, *DescribeDirection(Plane.Axes.AxisB));

	for (int32 B = High.Y; B >= Low.Y; --B)
	{
		FString Row = FString::Printf(TEXT("\n  %5d "), B);

		for (int32 A = Low.X; A <= High.X; ++A)
		{
			const FIntPoint Cell(A, B);
			TCHAR Mark = TEXT(' ');

			const int32 FootprintIndex = Object.Footprint.IndexOfByKey(Cell);

			if (FootprintIndex != INDEX_NONE)
			{
				Mark = PlacementSolverPrivate::GetFootprintLetter(Size, FIntPoint(FootprintIndex / Size.Y, FootprintIndex % Size.Y));
			}
			else if (Plane.Footprints.Contains(Cell))
			{
				Mark = TEXT('o');
			}
			else
			{
				FIntVector Base;
				const bool bStructure = !Plane.bHosted && GetColumnBase(Plane, Cell, Base) && World.IsCellSolid(Base + Plane.Axes.Normal);

				if (bStructure)
				{
					Mark = TEXT('#');
				}
				else if (Plane.Zones.Contains(Cell))
				{
					Mark = TEXT('z');
				}
				else if (IsSupportCell(Plane, Cell))
				{
					Mark = TEXT('_');
				}
			}

			Row.AppendChar(Mark);
		}

		Text += Row;
	}

	return Text;
}
