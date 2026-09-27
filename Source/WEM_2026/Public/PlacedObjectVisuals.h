// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "PlacedObjectVisuals.generated.h"

class AActor;
class UStaticMesh;
class UStaticMeshComponent;
class USceneComponent;
struct FStreamableHandle;

/**
 * What placed objects look like: one mesh standing at one transform each, behind Add and Remove.
 *
 * Placement never waits for a mesh. The space is reserved the moment a pose is found, and the mesh
 * turns up once it has loaded in the background. One streamable handle is kept per mesh while any
 * object uses it, so a mesh placed a hundred times loads once and stays loaded.
 *
 * Each object is a component of its own, attached to the placer, with no collision and never
 * saved. That is plenty for hundreds of objects; thousands would want per-mesh instanced
 * components filled in chunks, the way the room manager's pieces are, which can be done here
 * without placement noticing.
 */
UCLASS(Transient)
class WEM_2026_API UPlacedObjectVisuals : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Stands a mesh at a world transform, attached to Parent and owned by Owner. Returns a handle for
	 * Remove. The mesh shows as soon as it has loaded; with bLoadSynchronously it is loaded on the spot.
	 */
	int32 Add(AActor* Owner, USceneComponent* Parent, const TSoftObjectPtr<UStaticMesh>& Mesh, const FTransform& Transform, bool bLoadSynchronously);

	void Remove(int32 Handle);

	/** Removes everything, and cancels whatever is still loading. */
	void Reset();

	/** Objects still waiting for their mesh. */
	int32 CountWaiting() const;

private:
	/** Puts a mesh that has just loaded on every object waiting for it. */
	void OnMeshLoaded(FSoftObjectPath Path);

	struct FMeshUse
	{
		TSharedPtr<FStreamableHandle> Handle;
		TArray<int32> Users;
	};

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UStaticMeshComponent>> Components;

	TMap<int32, FSoftObjectPath> ComponentMeshes;
	TMap<FSoftObjectPath, FMeshUse> MeshUses;
	int32 NextHandle = 1;
};
