// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PlacementArchetype.h"
#include "PlacementCatalog.h"
#include "PlacementMath.h"
#include "UObject/Package.h"

/** Archetypes, rows and contacts for the placement tests, made by hand rather than from assets. */
namespace WEMPlacementTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** Rooted, so nothing collects it in the middle of a test; the test lets go of it with Release. */
	inline UPlacementArchetype* MakeArchetype(const TCHAR* Name)
	{
		UPlacementArchetype* Archetype = NewObject<UPlacementArchetype>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPlacementArchetype::StaticClass(), Name));
		Archetype->AddToRoot();
		return Archetype;
	}

	inline void Release(TConstArrayView<UObject*> Objects)
	{
		for (UObject* Object : Objects)
		{
			if (Object)
			{
				Object->RemoveFromRoot();
			}
		}
	}

	/** A row for a box-shaped mesh a little smaller than its footprint, measured at CellSize. */
	inline FPlacementCatalogRow MakeRow(UPlacementArchetype* Archetype, const FIntVector& SizeCells, const double CellSize = 20.0)
	{
		FPlacementCatalogRow Row;
		Row.Archetype = Archetype;
		Row.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
		Row.BuiltCellSize = static_cast<float>(CellSize);
		Row.Geometry.SizeCells = SizeCells;
		Row.Geometry.LocalBoundsCm = FBox(FVector(-50.0, -50.0, 0.0), FVector(-50.0, -50.0, 0.0) + FVector(SizeCells) * CellSize - FVector(2.0, 2.0, 1.0));

		const int32 Cells = WEMPlacement::GetFootprintCellCount(SizeCells);
		Row.Geometry.OpenBelowCm.Init(0.0f, Cells);
		Row.Geometry.FootprintMask.Init(1, Cells);
		return Row;
	}

	inline FContactRequirement MakeContact(const EObjectSide Side, const EContactTarget Target, const EOverlapMode Overlap = EOverlapMode::Flush, const int32 MinFaces = 1)
	{
		FContactRequirement Contact;
		Contact.Side = Side;
		Contact.Target = Target;
		Contact.Overlap = Overlap;
		Contact.MinFaces = MinFaces;
		return Contact;
	}

	inline FPlacementRule MakeEdgeRule(const TCHAR* Label, TArray<FContactRequirement> Contacts)
	{
		FPlacementRule Rule;
		Rule.Label = Label;
		Rule.Kind = ERuleKind::Edge;
		Rule.Contacts = MoveTemp(Contacts);
		return Rule;
	}

	inline FSpecialEdge MakeSpecialEdge(const int32 Id, const EObjectSide Side, const int32 Start, const int32 Count)
	{
		FSpecialEdge Edge;
		Edge.Id = Id;
		Edge.Side = Side;
		Edge.Start = Start;
		Edge.Count = Count;
		return Edge;
	}
}

#endif
