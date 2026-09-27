// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PlacementCatalogBuilder.generated.h"

class UDataTable;
class UPlacementArchetype;
class UStaticMesh;
struct FCatalogBuildReport;
struct FMeshAnalysisSettings;
struct FMeshAnalysisSource;

/**
 * Builds a placement catalog: finds every static mesh under its source folders, matches each to an
 * archetype, measures it, and writes a row for it into the catalog table. Run it again after
 * adding meshes or archetypes; it only measures what has changed, and it never touches the columns
 * people own.
 *
 * Editor only. It is never cooked, and nothing at runtime reads it - the placer reads the table.
 */
UCLASS(BlueprintType)
class WEM_2026EDITOR_API UPlacementCatalogBuilder : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The message log page every build, validation and preview reports into. */
	static const FName MessageLogName;

	/** Every static mesh in these folders, or below them, gets a row. */
	UPROPERTY(EditAnywhere, Category = "Sources", meta = (ContentDir))
	TArray<FDirectoryPath> SourceFolders;

	/** The table written to. Its row structure has to be PlacementCatalogRow. */
	UPROPERTY(EditAnywhere, Category = "Sources")
	TObjectPtr<UDataTable> Catalog;

	/** Matches against every placement archetype in the project rather than only the ones listed below. */
	UPROPERTY(EditAnywhere, Category = "Archetypes")
	bool bDiscoverArchetypes = true;

	UPROPERTY(EditAnywhere, Category = "Archetypes", meta = (EditCondition = "!bDiscoverArchetypes"))
	TArray<TObjectPtr<UPlacementArchetype>> Archetypes;

	/** Has to equal the room manager's cell size, or the placer refuses the catalog. */
	UPROPERTY(EditAnywhere, Category = "Analysis", meta = (ClampMin = "1.0"))
	float CellSize = 20.0f;

	/** Sample lines per cell along each axis; a cell is sampled by the square of this. */
	UPROPERTY(EditAnywhere, Category = "Analysis", meta = (ClampMin = "1", ClampMax = "16"))
	int32 SamplesPerCellAxis = 4;

	UPROPERTY(EditAnywhere, Category = "Analysis", meta = (ClampMin = "0.0"))
	float HeightToleranceCm = 2.0f;

	UPROPERTY(EditAnywhere, Category = "Analysis", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float MaxSurfaceSlopeDeg = 10.0f;

	UPROPERTY(EditAnywhere, Category = "Analysis", meta = (ClampMin = "1"))
	int32 MinLayerCells = 2;

	/** Measures every mesh again on the next build, changed or not, and then turns itself off. */
	UPROPERTY(EditAnywhere, Category = "Analysis")
	bool bRebuildAllOnNextBuild = false;

	/** Finds the meshes, matches and measures them, and writes the catalog. Reports to the Placement Catalog log. */
	UFUNCTION(CallInEditor, Category = "Catalog")
	void BuildCatalog();

	/** Checks every enabled row against its mesh, its archetype and these settings, without changing anything. */
	UFUNCTION(CallInEditor, Category = "Catalog")
	void ValidateCatalog();

	/**
	 * Lays every mesh of the catalog out in the current level, grouped by archetype, with its
	 * footprint, front, edges, surfaces and special edges drawn on it - disabled and unmatched rows
	 * in red. Replaces a preview spawned before. Never saved with the level.
	 */
	UFUNCTION(CallInEditor, Category = "Catalog")
	void SpawnPreviewInLevel();

	virtual bool IsEditorOnly() const override
	{
		return true;
	}

	FMeshAnalysisSettings MakeAnalysisSettings() const;

	/** Reads one mesh's source triangles - LOD0 of its mesh description, every section - and its sockets. */
	static bool LoadMeshSource(const UStaticMesh* Mesh, FMeshAnalysisSource& OutSource);

private:
	/** The archetypes to match against: every one in the project, or the listed ones. */
	TArray<UPlacementArchetype*> GatherArchetypes() const;

	/** Whether Catalog is set and holds catalog rows, reporting to the log when it does not. */
	bool CheckCatalog() const;

	void WriteReport(const FText& Title, const FCatalogBuildReport& Report, bool bBuild) const;
};
