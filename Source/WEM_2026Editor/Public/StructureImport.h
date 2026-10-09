// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * One structure's settings file, as the Blender add-on writes it beside the structure's .glb: what
 * the room manager needs to know about the structure, and where it came from.
 */
struct FStructureSettings
{
	int32 Format = 0;

	/** Becomes SM_<Name> and DA_Structure_<Name>. */
	FString Name;

	/** The .blend and the object in it the structure was exported from, so it can be found again. */
	FString Blend;
	FString Object;

	/** Cells beam to beam, measured from the bounds by the add-on. */
	int32 Width = 0;
	int32 Length = 0;

	bool bTwoSided = false;

	/** Category asset names, such as DA_Category_ExteriorUrban. */
	TArray<FString> Categories;

	/** Ground, Ceiling and Wall. */
	TArray<FString> Types;

	bool bLoadBearing = true;
	bool bHoldsItems = true;
	bool bFence = false;
	bool bMayLieSideways = false;
	float Weight = 1.0f;

	/** In metres, in Blender's axes. */
	FVector BoundsMin = FVector::ZeroVector;
	FVector BoundsMax = FVector::ZeroVector;

	FString Exported;
};

/** The structure import with no assets or editor attached: reading settings files and checking meshes against them. */
namespace WEMStructureImport
{
	/** The settings format this importer understands. A file of any other is refused. */
	constexpr int32 SettingsFormat = 1;

	/** How far a measured size may stray from a whole number of cells, in centimetres. */
	constexpr double SizeToleranceCm = 1.0;

	/** Reads a settings file. False, with the reason, when it is not one this importer understands. */
	WEM_2026EDITOR_API bool ParseSettings(const FString& Json, FStructureSettings& OutSettings, FString& OutError);

	/** The type names as an EStructureType mask. Names it does not know are listed in OutUnknown. */
	WEM_2026EDITOR_API int32 MakeTypeMask(TConstArrayView<FString> Types, TArray<FString>& OutUnknown);

	/**
	 * Cells beam to beam for an opening ExtentCm across: one more than the cells it spans, since each
	 * beam takes half a cell from either side of its line. False when it is not a whole number of
	 * cells to within SizeToleranceCm.
	 */
	WEM_2026EDITOR_API bool MeasureCells(double ExtentCm, double CellSize, int32& OutCells);

	/**
	 * The bounds the settings file says the mesh has, in centimetres in the imported mesh's own frame
	 * rather than Blender's.
	 */
	WEM_2026EDITOR_API FBox GetExpectedMeshBounds(const FStructureSettings& Settings);

	/** Whether two boxes agree to within SizeToleranceCm on every side. */
	WEM_2026EDITOR_API bool BoundsMatch(const FBox& Expected, const FBox& Measured);
}
