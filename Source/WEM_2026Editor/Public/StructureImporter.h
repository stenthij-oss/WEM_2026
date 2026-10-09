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

	/** Has to equal the room manager's cell size, since sizes are measured in its cells. */
	UPROPERTY(EditAnywhere, Category = "Sources", meta = (ClampMin = "1.0"))
	float CellSize = 20.0f;

	UStructureImporter();

	/** Imports every structure in SourceFolder. Reports to the Structure Import log. */
	UFUNCTION(CallInEditor, Category = "Structures")
	void Import();

	virtual bool IsEditorOnly() const override
	{
		return true;
	}

private:
	/** Imports one structure from its settings file. Returns false, having reported why, when it could not. */
	bool ImportStructure(const FString& SettingsPath, TArray<FString>& OutNotes, TArray<FString>& OutErrors);

	/** The structure asset of that name, made in Folder when there is none yet. */
	UPlatformStructureData* FindOrCreateStructure(const FString& Folder, const FString& AssetName, bool& bOutCreated) const;
};
