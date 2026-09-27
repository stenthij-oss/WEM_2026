// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "PlacementCatalog.h"
#include "PlacementMath.h"
#include "PlacementTypes.h"
#include "RoomManager.h"

class IPlacementWorld;
class UDataTable;
class UPlacementArchetype;
class UStaticMesh;

/** One enabled catalog row, ready to place: its overrides already laid over what the builder derived. */
struct FPlacementEntry
{
	FName RowName;
	TSoftObjectPtr<UStaticMesh> Mesh;
	UPlacementArchetype* Archetype = nullptr;
	FName SetName;
	FPlacementGeometry Geometry;
	float Weight = 1.0f;
	int32 MaxInstances = 0;

	/** Index of the signature it belongs to. */
	int32 Signature = INDEX_NONE;

	/** How many of it stand. */
	int32 Instances = 0;

	bool HasRoom() const
	{
		return MaxInstances <= 0 || Instances < MaxInstances;
	}
};

/**
 * Everything where an object may stand depends on: its archetype, its size, and its special edges.
 * Its surfaces only matter to what stands on it. Meshes that agree on those three are one
 * signature, and are placed as one: the pose is found for the signature, and the mesh drawn from it
 * afterwards. So if one 3 x 6 x 4 desk cannot be placed, no other desk of that archetype and shape
 * can either, and it is only worked out once.
 */
struct FPlacementSignature
{
	UPlacementArchetype* Archetype = nullptr;
	FIntVector SizeCells = FIntVector(1, 1, 1);

	/** Sorted, so two meshes that list the same edges in a different order still agree. */
	TArray<FSpecialEdge> SpecialEdges;

	/** Every face on the footprint's perimeter, in the object's own frame. */
	TArray<WEMPlacement::FEdgeFace> EdgeFaces;

	/** Its entries, in catalog order. */
	TArray<int32> Entries;

	/** The placer revision at which it last failed to find anywhere; INDEX_NONE when it has not. */
	int32 InfeasibleAtRevision = INDEX_NONE;
};

/** One archetype's standing in the catalog: its signatures and how many of its objects stand. */
struct FPlacementKind
{
	UPlacementArchetype* Archetype = nullptr;
	TArray<int32> Signatures;
	int32 Instances = 0;
};

/**
 * The catalog tables, resolved into what the placer draws from: an entry per enabled row, grouped
 * into signatures, grouped by archetype, all in the tables' own order so a run replays exactly.
 */
class WEM_2026_API FPlacementCatalogData
{
public:
	/**
	 * Takes in every enabled row of every table. Refuses the lot, with the reason in OutError, when
	 * any enabled row was measured at a cell size other than CellSize: its cells would not be the
	 * grid's. Rows with no archetype or no mesh are skipped, with one warning for all of them.
	 */
	bool AddTables(TConstArrayView<const UDataTable*> Tables, double CellSize, FString& OutError, TArray<FString>& OutWarnings);

	/** Takes in one row as it stands, enabled or not. It needs an archetype. */
	void AddRow(FName RowName, const FPlacementCatalogRow& Row);

	void Reset();

	TArray<FPlacementEntry> Entries;
	TArray<FPlacementSignature> Signatures;
	TArray<FPlacementKind> Kinds;
};

/** Where an object stands: on which support plane, turned which way, with its local (0, 0) cell on which plane cell. */
struct FPlacementPose
{
	uint64 Plane = 0;
	int32 Rotation = 0;
	FIntPoint Origin = FIntPoint::ZeroValue;

	bool operator==(const FPlacementPose& Other) const
	{
		return Plane == Other.Plane && Rotation == Other.Rotation && Origin == Other.Origin;
	}

	friend uint32 GetTypeHash(const FPlacementPose& Pose)
	{
		return HashCombine(HashCombine(GetTypeHash(Pose.Plane), GetTypeHash(Pose.Rotation)), GetTypeHash(Pose.Origin));
	}
};

/** One face of a placed object's perimeter, as the plane it stands on keeps it. */
struct FPlacedPerimeterFace
{
	int32 Object = INDEX_NONE;
	EObjectSide Side = EObjectSide::Front;
	int32 Index = 0;
	bool bCorner = false;
	TArray<int32, TInlineAllocator<2>> SpecialEdgeIds;
};

/**
 * A plane cell with something across one of its faces - structure, or the edge of the support -
 * and which of the four plane directions that face looks along. Poses that have to touch it are
 * grown from these.
 */
struct FPoseSeed
{
	FIntPoint Cell = FIntPoint::ZeroValue;
	int32 Direction = INDEX_NONE;
};

/** A zone cell's owner: which object, and which of its archetype's zones. */
struct FPlacedZoneRef
{
	int32 Object = INDEX_NONE;
	int32 Zone = INDEX_NONE;
};

/**
 * A plane objects can stand on. Every face direction of every segment, and every surface layer of
 * every placed object, is one of these and goes through the same code.
 *
 * Cells are two whole numbers along the plane's AxisA and AxisB. On a segment plane they are the
 * world cell's own coordinates along those axes; on a hosted plane, the host's local footprint
 * cells, so AxisA and AxisB are the host's front and right.
 */
struct FSupportPlane
{
	uint64 Id = 0;
	bool bHosted = false;

	/** Segment planes: the face the surfaces look out of, and the layer along its axis. */
	EGridFace Face = EGridFace::PosZ;
	int32 Layer = 0;

	/** Hosted planes: the placed object, and which of its surface layers. */
	int32 Host = INDEX_NONE;
	int32 HostLayer = INDEX_NONE;

	WEMPlacement::FPlaneAxes Axes;

	/**
	 * Height of the plane's surface above the segment face at the bottom of it all: zero on a
	 * segment, a desk top's height on a desk, the two added up for something on something on a desk.
	 */
	double BaseOffsetCm = 0.0;

	/** Segment planes: every open all-object cell, as of CachedStructureRevision. Kept up to date with the structure. */
	TSet<FIntPoint> OpenCells;

	/**
	 * The cells poses are drawn from, in a fixed order: a segment plane's open cells, or a hosted
	 * plane's layer cells. Whether one is free is asked as it is drawn, so a segment cell the
	 * structure has since closed can stay listed until the list is compacted.
	 */
	TArray<FIntPoint> SeedCells;

	/** Segment planes: listed seed cells since closed. Once they are half the list, it is compacted. */
	int32 ClosedSeedCells = 0;

	/**
	 * Seed cells with structure across a face, and with the support's edge across one. Worked out
	 * when first asked for, then added to as the structure grows. One the structure has since
	 * changed around fails its pose's test, as any other would.
	 */
	TArray<FPoseSeed> WallSeeds;
	TArray<FPoseSeed> BoundarySeeds;
	bool bWallSeedsBuilt = false;
	bool bBoundarySeedsBuilt = false;

	/** The wall seeds listed, as (cell x, cell y, direction index), so structure found beside one again does not list it twice. */
	TSet<FIntVector> WallSeedKeys;

	/** Hosted planes: the layer's cells, and what the layer offers. */
	TSet<FIntPoint> LayerCells;
	double LayerHeightCm = 0.0;
	double ClearanceCm = 0.0;
	UPlacementArchetype* HostArchetype = nullptr;
	FTransform HostTransform;
	FBox HostBounds = FBox(ForceInit);
	FIntVector HostSize = FIntVector(1, 1, 1);

	/** Hosted planes: every object under this plane, host first, whose cells a stacked object may overlap. */
	TArray<int32> HostChain;

	/** Hosted planes: the world cell on the segment face at the bottom of each plane cell's column. */
	TMap<FIntPoint, FIntVector> ColumnBases;

	/** Which placed object's footprint covers each cell. */
	TMap<FIntPoint, int32> Footprints;

	/** Every placed object's perimeter faces, keyed by (cell x, cell y, direction index). */
	TMap<FIntVector, FPlacedPerimeterFace> PerimeterFaces;

	/** Every placed object's zone cells. */
	TMap<FIntPoint, TArray<FPlacedZoneRef>> Zones;

	/** Placed objects standing on it, in the order they were placed. */
	TArray<int32> Objects;

	/** As the log writes it. */
	FString Name;
};

/**
 * Every hosted plane whose host is of one archetype, in the order they were made, with how many
 * cells and how many boundary seeds the planes before each one hold between them. A hosted plane
 * never changes once made - its cells are its host's - so a group only ever grows, and whatever
 * may stand on any object's surface is drawn from a group at a time rather than plane by plane.
 */
struct FHostedPlaneGroup
{
	UPlacementArchetype* HostArchetype = nullptr;
	TArray<uint64> Planes;
	TArray<int32> CellsBefore;
	TArray<int32> BoundarySeedsBefore;
	int32 Cells = 0;
	int32 BoundarySeeds = 0;
};

/** One rule being tried for a signature, or the plain surface placement a fallback makes. */
struct FPlacementAttempt
{
	int32 Signature = INDEX_NONE;

	/** Null for a placement with no contacts: a Surface rule, a fallback, or an archetype with no rules. */
	const FPlacementRule* Rule = nullptr;

	/** ESupportKind bits this attempt may stand on. */
	int32 SupportMask = 0;

	/** Keeps every Object contact, and every hosted support, to this object. INDEX_NONE for none. */
	int32 RestrictToHost = INDEX_NONE;
};

/** What testing a pose found. */
struct FPoseCheck
{
	/** Sides whose contacts held, as WEMPlacement::SideBit, so the mesh can slide against them. */
	int32 ContactSideMask = 0;

	/** Faces that made a contact hold, as (plane cell x, plane cell y, direction index). */
	TArray<FIntVector> ContactFaces;

	/** Why the pose failed, when it did. */
	FString Failure;
};

/** An object the solver has placed. */
struct FPlacedObject
{
	int32 Id = INDEX_NONE;
	int32 Entry = INDEX_NONE;
	int32 Signature = INDEX_NONE;
	UPlacementArchetype* Archetype = nullptr;

	FPlacementPose Pose;
	WEMPlacement::FPoseFrame Frame;
	FTransform Transform;

	/** Index of the rule that placed it, or INDEX_NONE for a fallback. */
	int32 Rule = INDEX_NONE;

	FPoseCheck Contacts;

	/** Plane cells under it, in local order: x, then y. */
	TArray<FIntPoint> Footprint;

	/** Its perimeter faces' keys on its plane. */
	TArray<FIntVector> PerimeterKeys;

	/** Its zone cells on its plane, with the zone each came from. */
	TArray<TPair<FIntPoint, int32>> ZoneCells;

	/** The world cells it reserves, as an inclusive box. Exact on a segment; for a stacked object, a cover of its box. */
	FIntVector VolumeMin = FIntVector::ZeroValue;
	FIntVector VolumeMax = FIntVector::ZeroValue;

	/** The segment's surface cells under its footprint. Empty for a stacked object, which has Host instead. */
	TArray<FIntVector> SupportCells;
	int32 Host = INDEX_NONE;

	/** Planes on top of it, one per surface layer of its mesh. */
	TArray<uint64> HostedPlanes;

	/** The object it was placed as a companion of, or INDEX_NONE. */
	int32 CompanionOf = INDEX_NONE;

	/** The block the log prints for it, made as it was placed. */
	FString Description;
};

/**
 * Places objects from a catalog onto a grid, one at a time, on a seeded stream. It knows nothing of
 * actors, meshes or levels: it answers where an object goes and keeps track of what stands where,
 * and the object placer actor turns that into meshes and obstacles. Nothing it decides depends on
 * whether any mesh has loaded, so a run replays exactly from the same seeds.
 */
class WEM_2026_API FPlacementSolver
{
public:
	FPlacementSolver(const IPlacementWorld& InWorld, FPlacementCatalogData& InCatalog, FRandomStream& InStream);

	/**
	 * One beat: draws an archetype by its share of spawns, then a signature, tries the archetype's
	 * rules for it, and places the object - trying again with another draw, up to MaxAttempts
	 * times, when a signature has nowhere to go. OutPlaced gets the id of what was placed.
	 */
	bool PlaceNext(int32 MaxAttempts, TArray<int32>& OutPlaced);

	/** Tests a pose for an attempt without placing anything. */
	bool TestPose(const FPlacementAttempt& Attempt, const FPlacementPose& Pose, FPoseCheck& OutCheck) const;

	/** Tests a pose, and places the entry there when it passes. Returns the new object's id, or INDEX_NONE. */
	int32 TryPlaceAt(const FPlacementAttempt& Attempt, const FPlacementPose& Pose, int32 Entry, int32 RuleIndex = INDEX_NONE, FPoseCheck* OutCheck = nullptr);

	/**
	 * Draws a pose for an attempt: every pose its anchors offer, tested in a random order until one
	 * passes. The poses are never listed: each is made from a seed as it is drawn, so a structure with
	 * a hundred thousand open cells costs no more than the few draws it takes to find a place. It
	 * gives up after a few thousand draws, since a place that rare is left for a later beat to find.
	 */
	bool FindPose(const FPlacementAttempt& Attempt, FPlacementPose& OutPose, FPoseCheck& OutCheck);

	/**
	 * Each archetype's weight in the next draw, in catalog order: its SpawnWeight, while it has room
	 * for another object and a signature that is not known to have nowhere to go. However many
	 * meshes an archetype has, its share is its own.
	 */
	void GetArchetypeDrawWeights(TArray<float>& OutWeights) const;

	/** A signature's weight among its archetype's: the summed weights of its entries that have room. */
	float GetSignatureDrawWeight(int32 Signature) const;

	/** Changes whenever the structure or the placed objects do. What has been found infeasible stays so until it does. */
	int32 GetRevision() const;

	/**
	 * Brings the segment planes up to date with the structure, if it has changed: from the surfaces
	 * the world says its changes touched, or by gathering every surface again when it cannot say.
	 * Every beat does, so the planes always offer what the structure does.
	 */
	void RefreshSegmentPlanes();

	/** How long the last beat spent refreshing the segment planes, in milliseconds; zero when it did not. */
	double GetLastRefreshMilliseconds() const
	{
		return LastRefreshMilliseconds;
	}

	/**
	 * Checks the segment planes, as refreshed, against every surface gathered afresh: the same open
	 * cells, each of them listed to draw from, and every wall and boundary seed listed that a build
	 * from scratch would list. What differs goes in OutProblems. For testing; it walks the whole
	 * structure.
	 */
	bool VerifySegmentPlanes(TArray<FString>& OutProblems) const;

	/** Drops every placed object and everything worked out about them. */
	void Reset();

	const TArray<FPlacedObject>& GetObjects() const
	{
		return Objects;
	}

	const FPlacedObject* FindObject(int32 Id) const;
	const FSupportPlane* FindPlane(uint64 Id) const;

	/** World position of the centre of a plane cell's face, on the plane's surface. */
	FVector GetPlaneCellCentre(const FSupportPlane& Plane, const FIntPoint& Cell) const;

	static uint64 MakeSegmentPlaneId(EGridFace Face, int32 Layer);
	static uint64 MakeHostedPlaneId(int32 Host, int32 Layer);

	/** A segment plane, made if it does not exist yet. For tests that place by hand. */
	uint64 GetSegmentPlaneId(const FIntVector& SurfaceCell, EGridFace Face);

private:
	/** The first contact of a rule to generate poses from: the most selective, Object before Wall before SupportBoundary. */
	static const FContactRequirement* ChooseAnchor(const FPlacementRule* Rule);

	bool IsPlaneAllowed(const FSupportPlane& Plane, const FPlacementAttempt& Attempt) const;
	bool DoesPlaneAccept(const FSupportPlane& Plane, const UPlacementArchetype* Archetype) const;

	/** Whether the surfaces of an object of HostArchetype take an object of Archetype. */
	static bool DoesHostAccept(const UPlacementArchetype* HostArchetype, const UPlacementArchetype* Archetype);

	/** Whether a plane cell can take a footprint cell: open, and nothing standing on it. */
	bool IsCellAvailable(const FSupportPlane& Plane, const FIntPoint& Cell) const;

	/** Whether a plane cell belongs to the support at all, whether or not anything stands there. */
	bool IsSupportCell(const FSupportPlane& Plane, const FIntPoint& Cell) const;

	/** The world cell on the segment face at the bottom of a plane cell's column. */
	bool GetColumnBase(const FSupportPlane& Plane, const FIntPoint& Cell, FIntVector& OutBase) const;

	/** The world cells a pose would fill: its footprint's columns, from its bottom up to its height. */
	bool GatherVolume(const FSupportPlane& Plane, const WEMPlacement::FPoseFrame& Frame, const FIntPoint& Origin, const FIntVector& SizeCells, TArray<FIntVector>& OutCells) const;

	/** Works out a plane's wall or boundary seeds, if they are not already. */
	void EnsureAnchorSeeds(FSupportPlane& Plane, EContactTarget Target);

	/** Lists a segment plane cell's wall or boundary seeds: each face with structure, or the support's edge, across it. */
	void AddAnchorSeeds(FSupportPlane& Plane, const FIntPoint& Cell, bool bWall);

	/** Whether structure stands across one face of a segment plane cell, in the layer of air on top of it. */
	bool IsWallAcross(const FSupportPlane& Plane, const FIntPoint& Cell, int32 Direction) const;

	/** Whether the edge of the support lies across one face of a plane cell. */
	bool IsBoundaryAcross(const FSupportPlane& Plane, const FIntPoint& Cell, int32 Direction) const;

	/** Gathers every open surface of the structure afresh into the segment planes. */
	void RebuildSegmentPlanes();

	/** Takes the surfaces the structure's latest changes touched into the segment planes. */
	void ApplySurfaceChanges(TConstArrayView<FGridSurfaceRef> Surfaces);

	/** Drops the cells a segment plane lists that the structure has closed, keeping the rest in order. */
	void CompactSeeds(FSupportPlane& Plane);

	bool EvaluateContacts(const FPlacementAttempt& Attempt, const FSupportPlane& Plane, const WEMPlacement::FPoseFrame& Frame, const FIntPoint& Origin, FPoseCheck& OutCheck) const;

	bool DoZonesHold(const FPlacementSignature& Signature, const FSupportPlane& Plane, const WEMPlacement::FPoseFrame& Frame, const FIntPoint& Origin) const;

	/** Every zone cell a pose would reach out to, with the index of the zone it comes from. */
	void GatherZoneCells(const FPlacementSignature& Signature, const WEMPlacement::FPoseFrame& Frame, const FIntPoint& Origin, TArray<TPair<FIntPoint, int32>>& OutCells) const;

	/** Whether an object counts as a contact's target. */
	bool IsContactTarget(const FContactRequirement& Contact, const FPlacementAttempt& Attempt, int32 Object) const;

	/** The draws of one beat: an archetype, a signature and a pose, up to MaxAttempts times. */
	bool PlaceFromDraws(int32 MaxAttempts, TArray<int32>& OutPlaced);

	/** Tries an archetype's rules for a signature, as its RuleSelection says, kept to one host when RestrictToHost is set. */
	bool TryRules(int32 Signature, FPlacementPose& OutPose, FPoseCheck& OutCheck, int32& OutRuleIndex, int32 RestrictToHost = INDEX_NONE);

	int32 Commit(const FPlacementAttempt& Attempt, const FPlacementPose& Pose, const FPoseCheck& Check, int32 Entry, int32 RuleIndex, int32 CompanionOf = INDEX_NONE);

	/** Places a host's companions, each by its chance and count, adding their ids to OutPlaced. */
	void PlaceCompanions(int32 Host, TArray<int32>& OutPlaced);

	/** One companion of Host, from the same set as the host's mesh where it can be. INDEX_NONE when it has nowhere to go. */
	int32 PlaceCompanion(int32 Host, const FCompanion& Companion);

	/** Draws an index by weight, or INDEX_NONE when every weight is zero. */
	int32 RollWeighted(TConstArrayView<float> Weights);

	/** What lies across one face of a posed object, as the log writes it. */
	FString DescribeAcross(const FSupportPlane& Plane, const FIntPoint& Cell, const FIntPoint& Direction) const;

	FString DescribeObject(const FPlacedObject& Object) const;

	FSupportPlane& FindOrAddSegmentPlane(EGridFace Face, int32 Layer);
	void AddPlaneToOrder(uint64 Id);

	const IPlacementWorld& World;
	FPlacementCatalogData& Catalog;
	FRandomStream& Stream;

	TMap<uint64, FSupportPlane> Planes;

	/** Every plane id, sorted, so planes are always walked in the same order. */
	TArray<uint64> PlaneOrder;

	/** The hosted planes, grouped by their host's archetype, the groups in the order their first plane was made. */
	TArray<FHostedPlaneGroup> HostedPlaneGroups;

	/** Placed objects; an object's id is one more than its index. */
	TArray<FPlacedObject> Objects;

	/** Every world cell an object fills, with the first object to fill it. */
	TMap<uint64, int32> ReservedCells;

	int32 ObjectRevision = 0;
	int32 CachedStructureRevision = INDEX_NONE;

	/** Set when a FindPose gave up before drawing every pose, so what it missed has not been shown to have nowhere to go. */
	bool bSearchCutShort = false;

	double LastRefreshMilliseconds = 0.0;
};
