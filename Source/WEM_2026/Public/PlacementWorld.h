// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RoomManager.h"

/**
 * What object placement asks of the grid: which cells exist and are built, what the surfaces
 * carry, and where they are in the world. The placer asks the room manager through this, and the
 * tests ask a fake grid through it, so placement is tested without a level or a structure grown
 * in one.
 */
class WEM_2026_API IPlacementWorld
{
public:
	virtual ~IPlacementWorld() = default;

	virtual double GetCellSize() const = 0;

	virtual bool IsValidCell(const FIntVector& Cell) const = 0;

	/** Whether structure fills the cell. */
	virtual bool IsCellSolid(const FIntVector& Cell) const = 0;

	virtual ESurfaceCapacity GetSurfaceCapacity(const FIntVector& Cell, EGridFace Face) const = 0;

	/** Every all-object surface nothing is built against, in an order that depends only on the structure. */
	virtual void GatherOpenAllObjectSurfaces(TArray<FGridSurfaceRef>& OutSurfaces) const = 0;

	/** World position of the centre of one face of a cell. */
	virtual FVector GetFaceCentre(const FIntVector& Cell, EGridFace Face) const = 0;

	/** Changes whenever the structure does, so what was worked out from it can be kept until then. */
	virtual int32 GetStructureRevision() const = 0;

	/**
	 * Every surface that may have changed since FromRevision - laid, built against, or opened to
	 * all objects - in the order it changed. False when the world cannot tell, and only gathering
	 * every surface again will do.
	 */
	virtual bool GatherSurfaceChangesSince(int32 FromRevision, TArray<FGridSurfaceRef>& OutSurfaces) const = 0;
};

/** The room manager, as placement sees it. */
class WEM_2026_API FRoomManagerPlacementWorld : public IPlacementWorld
{
public:
	explicit FRoomManagerPlacementWorld(const ARoomManager* InRoomManager)
		: RoomManager(InRoomManager)
	{
	}

	virtual double GetCellSize() const override;
	virtual bool IsValidCell(const FIntVector& Cell) const override;
	virtual bool IsCellSolid(const FIntVector& Cell) const override;
	virtual ESurfaceCapacity GetSurfaceCapacity(const FIntVector& Cell, EGridFace Face) const override;
	virtual void GatherOpenAllObjectSurfaces(TArray<FGridSurfaceRef>& OutSurfaces) const override;
	virtual FVector GetFaceCentre(const FIntVector& Cell, EGridFace Face) const override;
	virtual int32 GetStructureRevision() const override;
	virtual bool GatherSurfaceChangesSince(int32 FromRevision, TArray<FGridSurfaceRef>& OutSurfaces) const override;

private:
	const ARoomManager* RoomManager = nullptr;
};
