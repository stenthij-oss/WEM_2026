// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PlacementTypes.h"

/** The catalog builder's analysis settings, shared by every mesh in a build. */
struct FMeshAnalysisSettings
{
	/** Has to equal the room manager's, or the cells measured here are not the cells objects are placed on. */
	double CellSize = 20.0;

	/** Sample lines per cell along each axis, so a cell is sampled by the square of this. */
	int32 SamplesPerCellAxis = 4;

	/** How far apart two hits may be and still be one surface. */
	double HeightToleranceCm = 2.0;

	/** How far off level an upward face may tilt and still be something to stand on. */
	double MaxSurfaceSlopeDeg = 10.0;

	/** A layer of fewer cells than this is a ledge, not a surface. */
	int32 MinLayerCells = 2;
};

/** A socket on the mesh, in the mesh's own space. Sockets named PL_... override what the analysis finds. */
struct FMeshAnalysisSocket
{
	FName Name;
	FVector Location = FVector::ZeroVector;
};

/**
 * A mesh as the analysis reads it: plain triangles in the mesh's own space, wound the engine's way
 * round, so that (P2 - P0) ^ (P1 - P0) points out of the front. Nothing here is an asset, so the
 * analysis runs as well on a list of triangles written by hand as on a mesh.
 */
struct FMeshAnalysisSource
{
	TArray<FVector3f> Positions;

	/** Three per triangle, into Positions. */
	TArray<int32> Indices;

	TArray<FMeshAnalysisSocket> Sockets;
};

struct FMeshAnalysisResult
{
	/** False when there was nothing to measure: no triangles, or no cell size. */
	bool bValid = false;

	FPlacementGeometry Geometry;

	/** What a person should look at on this mesh, one sentence each. */
	TArray<FString> Warnings;
};

/**
 * Measures a mesh for placement: its size in cells, the surfaces other objects can stand on, the
 * open space under it, and where its archetype's special edges fall.
 *
 * Everything is found by casting vertical sample lines down through the mesh's triangles, a grid of
 * them per footprint cell, and reading off what each line meets. No collision, no render data and
 * no pivot are involved.
 */
namespace WEMMeshAnalysis
{
	WEM_2026EDITOR_API FMeshAnalysisResult Analyse(
		const FMeshAnalysisSource& Source,
		const FSurfacePolicy& Policy,
		TConstArrayView<FSpecialEdgeTemplate> Templates,
		const FMeshAnalysisSettings& Settings);

	/**
	 * Finds each template's run on a footprint. An Inset run is the side less its insets; an
	 * OpenBelow run is the longest stretch of faces on the side whose cells have at least the
	 * template's open space under them, ties going to the stretch nearest the side's centre.
	 */
	WEM_2026EDITOR_API void ResolveSpecialEdges(
		const FIntVector& SizeCells,
		TConstArrayView<float> OpenBelowCm,
		TConstArrayView<FSpecialEdgeTemplate> Templates,
		TArray<FSpecialEdge>& OutEdges,
		TArray<FString>& OutWarnings);

	/** Of the source's vertex positions and triangle indices: what changes when the mesh itself does. */
	WEM_2026EDITOR_API uint32 HashSource(const FMeshAnalysisSource& Source);

	/** Of everything else the analysis reads: the sockets, the policy, the templates and the settings. */
	WEM_2026EDITOR_API uint32 HashSettings(
		const FMeshAnalysisSource& Source,
		const FSurfacePolicy& Policy,
		TConstArrayView<FSpecialEdgeTemplate> Templates,
		const FMeshAnalysisSettings& Settings);
}
