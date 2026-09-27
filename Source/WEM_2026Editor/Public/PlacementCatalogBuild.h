// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MeshPlacementAnalysis.h"
#include "PlacementCatalog.h"
#include "UObject/SoftObjectPath.h"

class UPlacementArchetype;

/** One mesh a build found, and how to read its triangles if it has to be measured. */
struct FCatalogMeshSource
{
	FSoftObjectPath Path;

	/** Fills in the mesh's triangles and sockets. False when they cannot be read. */
	TFunction<bool(FMeshAnalysisSource&)> Load;
};

struct FCatalogBuildOptions
{
	FMeshAnalysisSettings Analysis;

	/** Measures every mesh again, changed or not. */
	bool bFullRebuild = false;

	/** Whether a mesh a row names still exists anywhere. A row whose mesh does not is orphaned. */
	TFunction<bool(const FSoftObjectPath&)> DoesMeshExist;
};

/** What a build or a validation found, for the message log. */
struct FCatalogBuildReport
{
	struct FNote
	{
		FName Row;
		FSoftObjectPath Mesh;
		FString Text;
		bool bError = false;
	};

	TArray<FName> Added;
	TArray<FName> Updated;
	TArray<FName> Unchanged;
	TArray<FName> Unmatched;
	TArray<FName> Orphaned;

	/** Every warning and error, per row. */
	TArray<FNote> Notes;

	void AddNote(const FName Row, const FSoftObjectPath& Mesh, const FString& Text, const bool bError = false)
	{
		Notes.Add({ Row, Mesh, Text, bError });
	}

	int32 CountErrors() const
	{
		return Notes.FilterByPredicate([](const FNote& Note) { return Note.bError; }).Num();
	}
};

/** An archetype a mesh matched, and how. */
struct FArchetypeMatch
{
	UPlacementArchetype* Archetype = nullptr;

	/** The folder it was matched by, when it was matched by folder. */
	FString MatchedFolder;

	/** Set when more than one archetype matched equally well; the match is then the first by name. */
	FString Ambiguity;
};

/**
 * The catalog build with no assets or editor attached: matching meshes to archetypes, and writing
 * the rows. The catalog builder asset feeds it from the asset registry; the tests feed it by hand.
 */
namespace WEMCatalogBuild
{
	/**
	 * The archetype a mesh is. An archetype whose MatchFolders hold the mesh's folder, or one above
	 * it, wins, and the deepest such folder wins among them. Failing that, the mesh's name is split
	 * on underscores and matched against MatchNameTokens, ignoring case, highest MatchPriority first.
	 */
	WEM_2026EDITOR_API FArchetypeMatch MatchArchetype(const FSoftObjectPath& MeshPath, TConstArrayView<UPlacementArchetype*> Archetypes);

	/** The mesh's own folder's name, when it sits below the folder it was matched by. */
	WEM_2026EDITOR_API FName MakeSetName(const FSoftObjectPath& MeshPath, const FArchetypeMatch& Match);

	/**
	 * Brings Rows up to date with Meshes, in place and in order, new rows at the end. Only the
	 * columns the builder owns are written: a person's weights, limits, enabled flags, overrides and
	 * locked archetypes survive every build. A mesh whose source and settings are unchanged since its
	 * row was built is not measured again, unless the options ask for a full rebuild. A row whose
	 * mesh has gone is disabled and reported, never deleted.
	 */
	WEM_2026EDITOR_API void BuildRows(
		TArray<TPair<FName, FPlacementCatalogRow>>& Rows,
		TConstArrayView<FCatalogMeshSource> Meshes,
		TConstArrayView<UPlacementArchetype*> Archetypes,
		const FCatalogBuildOptions& Options,
		FCatalogBuildReport& OutReport);

	/**
	 * Checks every enabled row: it has an archetype and a mesh, it was built at this cell size, its
	 * mesh and settings are what it was built from, and its overrides fit its footprint. Warns of
	 * anything too tall to fit between the closest parallel segments. LoadMesh reads a row's mesh,
	 * so its hashes can be checked; left unset, the hashes go unchecked.
	 */
	WEM_2026EDITOR_API void ValidateRows(
		TConstArrayView<TPair<FName, FPlacementCatalogRow>> Rows,
		const FMeshAnalysisSettings& Settings,
		const TFunction<bool(const FSoftObjectPath&, FMeshAnalysisSource&)>& LoadMesh,
		FCatalogBuildReport& OutReport);
}
