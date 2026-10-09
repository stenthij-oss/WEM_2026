// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "StructureCategory.generated.h"

/**
 * One category a structure can belong to: urban facades, rural fences, office interiors, sand.
 *
 * A data asset rather than an enum, so a category can be added without a build. Growth pulls a
 * face toward the categories of the faces around it, so like goes beside like and districts form;
 * that pull is all a category does. The Blender add-on lists the categories from the file the
 * structure importer writes, so a new one shows up there too.
 */
UCLASS(BlueprintType)
class WEM_2026_API UStructureCategory : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The name shown for the category, in the Blender add-on among other places. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Category")
	FText DisplayName;
};
