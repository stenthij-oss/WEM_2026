// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementWorld.h"

double FRoomManagerPlacementWorld::GetCellSize() const
{
	return RoomManager->CellSize;
}

bool FRoomManagerPlacementWorld::IsValidCell(const FIntVector& Cell) const
{
	return RoomManager->IsValidCell(Cell);
}

bool FRoomManagerPlacementWorld::IsCellSolid(const FIntVector& Cell) const
{
	return RoomManager->IsCellSolid(Cell);
}

ESurfaceCapacity FRoomManagerPlacementWorld::GetSurfaceCapacity(const FIntVector& Cell, const EGridFace Face) const
{
	return RoomManager->GetSurfaceCapacity(Cell, Face);
}

void FRoomManagerPlacementWorld::GatherOpenAllObjectSurfaces(TArray<FGridSurfaceRef>& OutSurfaces) const
{
	RoomManager->GatherSurfaces(ESurfaceCapacity::AllObject, /*bIncludeOccupied=*/false, OutSurfaces);
}

FVector FRoomManagerPlacementWorld::GetFaceCentre(const FIntVector& Cell, const EGridFace Face) const
{
	return RoomManager->GetSurfaceWorldTransform(Cell, Face).GetLocation();
}

int32 FRoomManagerPlacementWorld::GetStructureRevision() const
{
	return RoomManager->GetStructureRevision();
}

bool FRoomManagerPlacementWorld::GatherSurfaceChangesSince(const int32 FromRevision, TArray<FGridSurfaceRef>& OutSurfaces) const
{
	return RoomManager->GatherSurfaceChangesSince(FromRevision, OutSurfaces);
}
