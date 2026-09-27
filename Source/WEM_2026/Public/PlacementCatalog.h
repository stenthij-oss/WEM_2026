// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "PlacementTypes.h"
#include "PlacementCatalog.generated.h"

class UPlacementArchetype;
class UStaticMesh;

/**
 * One mesh the placer can spawn, as the catalog builder measured it. A catalog is a data table of
 * these, one row per mesh, named after the mesh asset.
 *
 * A table rather than an asset per mesh or one long array: the row editor shows one row at a
 * time, however many there are, and the table exports to CSV or JSON for edits across hundreds
 * of rows at once, such as weights.
 *
 * The builder writes the columns under Derived and never touches the rest; those belong to
 * people, and are how the odd exception is put right without the next build undoing it. All of
 * it is plain cooked data, since the placer reads it at runtime.
 */
USTRUCT(BlueprintType)
struct WEM_2026_API FPlacementCatalogRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Derived")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Written by the builder unless bArchetypeLocked, in which case whoever locked it set it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Derived")
	TObjectPtr<UPlacementArchetype> Archetype;

	/**
	 * The mesh's own folder, when it sits below the folder its archetype matched it by. Companions
	 * use it to keep to one style: the oak desk's chair from the oak set.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Derived")
	FName SetName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Derived")
	FPlacementGeometry Geometry;

	/** Of the mesh's source vertex positions and triangle indices, so a build can tell which meshes changed. Not readable from Blueprint, which has no uint32. */
	UPROPERTY(VisibleAnywhere, Category = "Derived")
	uint32 SourceHash = 0;

	/**
	 * Of everything else the derived columns were worked out from: the mesh's sockets, the
	 * archetype's surface policy and special edge templates, and the builder's analysis settings.
	 * A change to any of them leaves the geometry stale as surely as a change to the mesh does.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Derived")
	uint32 SettingsHash = 0;

	/** The cell size the row was measured at. The placer refuses a catalog measured at another. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Derived")
	float BuiltCellSize = 0.0f;

	/** What the builder had to say about this mesh. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Derived", meta = (MultiLine = "true"))
	FString BuildNotes;

	/**
	 * The builder turned bEnabled off itself, because the mesh matched no archetype or has gone.
	 * Only a row the builder disabled is enabled again by it once the reason has gone away; a row
	 * a person disabled stays as they left it.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Derived")
	bool bDisabledByBuilder = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement")
	bool bEnabled = true;

	/** This mesh's weight among its archetype's meshes. The archetype's share of spawns is its own. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	/** Zero is unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement", meta = (ClampMin = "0"))
	int32 MaxInstances = 0;

	/** Keeps the builder from changing Archetype. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Placement")
	bool bArchetypeLocked = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideSize = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (EditCondition = "bOverrideSize", DisplayName = "Size Cells"))
	FIntVector OverrideSizeCells = FIntVector(1, 1, 1);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideSurfaces = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (EditCondition = "bOverrideSurfaces", DisplayName = "Surfaces", ScriptName = "SurfacesOverride"))
	TArray<FDerivedSurfaceLayer> OverrideSurfaces;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideSpecialEdges = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (EditCondition = "bOverrideSpecialEdges", DisplayName = "Special Edges", ScriptName = "SpecialEdgesOverride"))
	TArray<FSpecialEdge> OverrideSpecialEdges;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideSet = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Overrides", meta = (EditCondition = "bOverrideSet", DisplayName = "Set Name"))
	FName OverrideSetName;

	/**
	 * The geometry the placer works with: what the builder derived, with this row's overrides laid
	 * over it. A size override keeps only the derived per-cell data that still fits the new
	 * footprint; anything that does not has to be overridden too.
	 */
	FPlacementGeometry GetEffectiveGeometry() const;

	FName GetEffectiveSetName() const
	{
		return bOverrideSet ? OverrideSetName : SetName;
	}
};
