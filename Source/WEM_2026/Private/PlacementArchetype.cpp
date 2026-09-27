// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementArchetype.h"

#include "PlacementMath.h"
#include "Misc/DataValidation.h"

#define LOCTEXT_NAMESPACE "PlacementArchetype"

bool UPlacementArchetype::IsMatchedBy(
	const TConstArrayView<TObjectPtr<UPlacementArchetype>> Archetypes,
	const FGameplayTagQuery& Classes) const
{
	if (Archetypes.Contains(this))
	{
		return true;
	}

	// An empty query matches nothing, so a list left empty on both counts matches nothing either.
	return !Classes.IsEmpty() && Classification.IsValid() && Classes.Matches(FGameplayTagContainer(Classification));
}

void UPlacementArchetype::GatherProblems(TArray<FText>& OutProblems) const
{
	const FText Name = FText::FromString(GetName());

	auto CheckReferences = [&OutProblems, &Name](const TConstArrayView<TObjectPtr<UPlacementArchetype>> References, const FText& Where)
	{
		for (const UPlacementArchetype* Reference : References)
		{
			if (!Reference)
			{
				OutProblems.Add(FText::Format(LOCTEXT("EmptyReference", "{0}: {1} lists an empty archetype."), Name, Where));
			}
		}
	};

	for (int32 RuleIndex = 0; RuleIndex < Rules.Num(); ++RuleIndex)
	{
		const FPlacementRule& Rule = Rules[RuleIndex];
		const FText RuleName = FText::Format(LOCTEXT("RuleName", "rule {0} \"{1}\""), RuleIndex, FText::FromName(Rule.Label));

		if (Rule.Kind == ERuleKind::Edge && Rule.Contacts.IsEmpty())
		{
			OutProblems.Add(FText::Format(LOCTEXT("EdgeRuleWithoutContacts", "{0}: {1} is an Edge rule with no contacts."), Name, RuleName));
		}

		for (const FContactRequirement& Contact : Rule.Contacts)
		{
			if (!WEMPlacement::IsEdgeSide(Contact.Side))
			{
				OutProblems.Add(FText::Format(LOCTEXT("ContactSide", "{0}: {1} asks for a contact on the top or bottom, which is no edge."), Name, RuleName));
			}

			if (Contact.Target == EContactTarget::Object)
			{
				CheckReferences(Contact.TargetArchetypes, RuleName);
			}
		}
	}

	TSet<int32> SpecialEdgeIds;

	for (const FSpecialEdgeTemplate& Template : SpecialEdges)
	{
		if (!WEMPlacement::IsEdgeSide(Template.Side))
		{
			OutProblems.Add(FText::Format(LOCTEXT("SpecialEdgeSide", "{0}: special edge {1} is on the top or bottom, which is no edge."), Name, Template.Id));
		}

		bool bRepeated = false;
		SpecialEdgeIds.Add(Template.Id, &bRepeated);

		if (bRepeated)
		{
			OutProblems.Add(FText::Format(LOCTEXT("SpecialEdgeId", "{0}: special edge id {1} is used twice."), Name, Template.Id));
		}
	}

	const int32 VerticalSides = WEMPlacement::SideBit(EObjectSide::Top) | WEMPlacement::SideBit(EObjectSide::Bottom);

	for (int32 ZoneIndex = 0; ZoneIndex < ExclusionZones.Num(); ++ZoneIndex)
	{
		const FExclusionZone& Zone = ExclusionZones[ZoneIndex];

		if ((Zone.SideMask & VerticalSides) != 0)
		{
			OutProblems.Add(FText::Format(LOCTEXT("ZoneVerticalSide", "{0}: exclusion zone {1} reaches out from the top or bottom, which it cannot."), Name, ZoneIndex));
		}

		if ((Zone.SideMask & ~VerticalSides) == 0)
		{
			OutProblems.Add(FText::Format(LOCTEXT("ZoneNoSide", "{0}: exclusion zone {1} names no side."), Name, ZoneIndex));
		}

		CheckReferences(Zone.Archetypes, FText::Format(LOCTEXT("ZoneName", "exclusion zone {0}"), ZoneIndex));
	}

	if (Surfaces.Accept == EAcceptMode::OnlyListed)
	{
		CheckReferences(Surfaces.AcceptedArchetypes, LOCTEXT("AcceptedArchetypes", "its surface policy"));
	}

	for (int32 CompanionIndex = 0; CompanionIndex < Companions.Num(); ++CompanionIndex)
	{
		const FCompanion& Companion = Companions[CompanionIndex];

		if (!Companion.Archetype)
		{
			OutProblems.Add(FText::Format(LOCTEXT("CompanionEmpty", "{0}: companion {1} has no archetype."), Name, CompanionIndex));
		}

		if (Companion.MaxCount < Companion.MinCount)
		{
			OutProblems.Add(FText::Format(LOCTEXT("CompanionCount", "{0}: companion {1} has a MaxCount below its MinCount."), Name, CompanionIndex));
		}
	}

	if (AllowedSupports == 0)
	{
		OutProblems.Add(FText::Format(LOCTEXT("NoSupports", "{0}: allows no support at all, so it can never be placed."), Name));
	}
}

#if WITH_EDITOR

EDataValidationResult UPlacementArchetype::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	TArray<FText> Problems;
	GatherProblems(Problems);

	for (const FText& Problem : Problems)
	{
		Context.AddError(Problem);
	}

	return Problems.IsEmpty() ? CombineDataValidationResults(Result, EDataValidationResult::Valid) : EDataValidationResult::Invalid;
}

void UPlacementArchetype::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Said as the edit is made rather than only when the asset is validated, so a side picked for a
	// zone that cannot have one is caught while its row is still in front of whoever picked it.
	TArray<FText> Problems;
	GatherProblems(Problems);

	for (const FText& Problem : Problems)
	{
		UE_LOG(LogTemp, Warning, TEXT("PlacementArchetype: %s"), *Problem.ToString());
	}
}

#endif

#undef LOCTEXT_NAMESPACE
