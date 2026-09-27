// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PlacementCatalogPreview.generated.h"

class UDataTable;
class ULineBatchComponent;
class UActorComponent;

/**
 * Every mesh of a placement catalog laid out on the ground in a grid, grouped by archetype and
 * spaced by footprint, each with its footprint, front, edges, surfaces and special edges drawn on
 * it and a label giving its row, archetype, set and size. Disabled and unmatched rows are shown too,
 * in red. It is how a person checks hundreds of meshes for a wrong facing, a wrong archetype or a
 * missed surface at a glance.
 *
 * Made by the catalog builder's Spawn Preview In Level button, which replaces any there already.
 * Editor only, and never saved with the level.
 */
UCLASS(Transient, NotPlaceable)
class WEM_2026EDITOR_API APlacementCatalogPreview : public AActor
{
	GENERATED_BODY()

public:
	APlacementCatalogPreview();

	UPROPERTY(VisibleAnywhere, Category = "Preview")
	TObjectPtr<UDataTable> Catalog;

	UPROPERTY(VisibleAnywhere, Category = "Preview")
	float CellSize = 20.0f;

	/** Lays the catalog out afresh from the actor's location. Returns how far the layout reaches along X and Y. */
	FVector2D Rebuild();

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY()
	TObjectPtr<ULineBatchComponent> Lines;

	/** The meshes and labels, made per rebuild. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UActorComponent>> PreviewComponents;
};
