// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/EngineTypes.h"
#include "StructureImporter.generated.h"

class UPlatformStructureData;
class UStaticMesh;
struct FStructureSettings;

/**
 * Brings the structures exported from Blender into the project: for every .glb and the settings
 * file beside it in SourceFolder, a mesh at <DestinationFolder>/<W>x<L>/SM_<Name> and the structure
 * asset DA_Structure_<Name> beside it, its settings taken from the file.
 *
 * The settings file is the source of truth: a re-import overwrites every setting it holds, and the
 * report lists each value that changed, so nothing set in Unreal is lost without a word. The weight
 * is the exception - it is tuned in Unreal while watching growth, so the file only gives a new
 * structure its first. A structure whose two files have not changed since its last import is left
 * as it is.
 *
 * Editor only. It is never cooked, and nothing at runtime reads it - the room manager reads the
 * structure assets.
 */
UCLASS(BlueprintType)
class WEM_2026EDITOR_API UStructureImporter : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The message log page every import reports into. */
	static const FName MessageLogName;

	/** Where the Blender add-on exports to: a .glb and a settings .json per structure. Relative to the project. */
	UPROPERTY(EditAnywhere, Category = "Sources", meta = (RelativeToGameDir))
	FDirectoryPath SourceFolder;

	/** The structures go in a folder per size below this one. */
	UPROPERTY(EditAnywhere, Category = "Sources", meta = (ContentDir))
	FDirectoryPath DestinationFolder;

	/**
	 * The list of categories the Blender add-on offers, written from every structure category in the
	 * project before each import and by Write Category List. Relative to the project.
	 */
	UPROPERTY(EditAnywhere, Category = "Sources", meta = (RelativeToGameDir, FilePathFilter = "json"))
	FFilePath CategoryListFile;

	/** Has to equal the room manager's cell size, since sizes are measured in its cells. */
	UPROPERTY(EditAnywhere, Category = "Sources", meta = (ClampMin = "1.0"))
	float CellSize = 20.0f;

	/**
	 * Unreal materials to use in place of the imported ones: a mesh's material slot named after a
	 * material in this folder takes that material. Any other slot keeps the material the .glb brought.
	 */
	UPROPERTY(EditAnywhere, Category = "Meshes", meta = (ContentDir))
	FDirectoryPath MaterialFolder;

	/** A mesh with more triangles than this is built with Nanite. */
	UPROPERTY(EditAnywhere, Category = "Meshes", meta = (ClampMin = "0"))
	int32 NaniteTriangleThreshold = 10000;

	/** What the last import did, structure by structure. */
	UPROPERTY(VisibleAnywhere, Category = "Report", meta = (MultiLine = "true"))
	FString LastReport;

	UStructureImporter();

	/**
	 * Writes the category list, then imports every new or changed structure in SourceFolder, and adds
	 * the new ones to every room manager in the open level. Reports to the Structure Import log.
	 */
	UFUNCTION(CallInEditor, Category = "Structures")
	void Import();

	/** Writes the list of categories the Blender add-on offers, from every structure category in the project. */
	UFUNCTION(CallInEditor, Category = "Structures")
	void WriteCategoryList();

	virtual bool IsEditorOnly() const override
	{
		return true;
	}

private:
	/** What importing one structure came to. */
	struct FImportResult
	{
		FString File;
		FString Name;
		FString Outcome;
		TArray<FString> Notes;
		TArray<FString> Errors;
		UPlatformStructureData* Structure = nullptr;
		bool bCreated = false;
	};

	/** Imports one structure from its settings file, unless its files have not changed. */
	void ImportStructure(const FString& SettingsPath, FImportResult& Result);

	/** The structure asset of that name, made in Folder when there is none yet. */
	UPlatformStructureData* FindOrCreateStructure(const FString& Folder, const FString& AssetName, bool& bOutCreated) const;

	/** Gives material slots named after a material in MaterialFolder that material. */
	void MapMaterials(UStaticMesh& Mesh, FImportResult& Result) const;

	/** Adds structures to every room manager in the open level that does not list them yet, as one undoable step. Returns how many room managers took them. */
	int32 AddToRoomManagers(TConstArrayView<UPlatformStructureData*> NewStructures) const;

	/** Writes CategoryListFile. Returns how many categories it lists, or INDEX_NONE when it could not be written. */
	int32 WriteCategoryListFile(FString& OutPath) const;
};
