// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "PlacementTypes.h"
#include "PlacementArchetype.generated.h"

/**
 * One kind of object the placer spawns - a desk, an office chair, a couch - and how it behaves:
 * what it is, which meshes are it, how often it comes up, where it may stand and what it must
 * touch there.
 *
 * Archetypes are few and written by hand. The meshes are many, and nothing about their geometry
 * is typed in here: the catalog builder measures each mesh and writes a row for it into a
 * catalog table, naming the archetype it matched.
 */
UCLASS(BlueprintType)
class WEM_2026_API UPlacementArchetype : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * What kind of thing this is, as contacts, zones and surfaces ask for it. Tags rather than an
	 * enum, so a new class needs no code and a group of them is a branch of the hierarchy.
	 *
	 * A query for a tag matches everything below it too, so a class that has to be told apart
	 * from its siblings is best a sibling itself: Furniture.Table.Dining beside
	 * Furniture.Table.Coffee, not Furniture.Table above it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identity")
	FGameplayTag Classification;

	/** Meshes in any of these folders, or below them, are this archetype. The deepest matching folder wins. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Matching", meta = (ContentDir))
	TArray<FDirectoryPath> MatchFolders;

	/**
	 * Meshes no folder claims are matched by name: any of these as a whole token of the asset's
	 * name, split on underscores and compared ignoring case. "Desk" matches SM_Desk_Oak_A.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Matching")
	TArray<FString> MatchNameTokens;

	/** Breaks ties between archetypes that both match a name. Highest wins. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Matching")
	int32 MatchPriority = 0;

	/**
	 * This kind's share of spawns, however many meshes it has. A mesh's own weight only divides
	 * the share among the archetype's meshes, so adding forty chairs does not make chairs forty
	 * times as common.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0.0"))
	float SpawnWeight = 1.0f;

	/** Zero is unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawning", meta = (ClampMin = "0"))
	int32 MaxInstances = 0;

	/** Segment alone means never stacked; Object Surface alone, only ever stacked. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Support", meta = (Bitmask, BitmaskEnum = "/Script/WEM_2026.ESupportKind"))
	int32 AllowedSupports = 1 << static_cast<int32>(ESupportKind::Segment);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules")
	ERuleSelection RuleSelection = ERuleSelection::Priority;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules")
	TArray<FPlacementRule> Rules;

	/** When every rule fails, the chance to try a plain Surface placement on any allowed support instead. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rules", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FallbackChance = 0.0f;

	/** Resolved against each mesh by the catalog builder. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Templates")
	TArray<FSpecialEdgeTemplate> SpecialEdges;

	/** Held in both directions: nothing placed later may stand in them, and nothing already standing may be caught by them. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Templates")
	TArray<FExclusionZone> ExclusionZones;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Templates")
	FSurfacePolicy Surfaces;

	/** Placed with each object of this archetype, right after it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Templates")
	TArray<FCompanion> Companions;

	/** Whether this archetype is one of Archetypes, or its classification matches Classes. */
	bool IsMatchedBy(TConstArrayView<TObjectPtr<UPlacementArchetype>> Archetypes, const FGameplayTagQuery& Classes) const;

	/** What is wrong with the archetype as written: sides that make no sense where they are used, and empty references. */
	void GatherProblems(TArray<FText>& OutProblems) const;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
