// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PlacementArchetype.h"
#include "PlacementCatalogBuild.h"
#include "SyntheticMeshes.h"
#include "UObject/Package.h"

namespace PlacementCatalogBuildTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	UPlacementArchetype* MakeArchetype(const TCHAR* Name, const TArray<FString>& Folders, const TArray<FString>& Tokens, const int32 Priority = 0)
	{
		UPlacementArchetype* Archetype = NewObject<UPlacementArchetype>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPlacementArchetype::StaticClass(), Name));

		for (const FString& Folder : Folders)
		{
			FDirectoryPath& Path = Archetype->MatchFolders.AddDefaulted_GetRef();
			Path.Path = Folder;
		}

		Archetype->MatchNameTokens = Tokens;
		Archetype->MatchPriority = Priority;
		return Archetype;
	}

	FSoftObjectPath MakeMeshPath(const FString& Folder, const FString& Name)
	{
		return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), *Folder, *Name, *Name));
	}

	FCatalogMeshSource MakeMesh(const FSoftObjectPath& Path, const FMeshAnalysisSource& Source)
	{
		FCatalogMeshSource Mesh;
		Mesh.Path = Path;
		Mesh.Load = [Source](FMeshAnalysisSource& OutSource)
		{
			OutSource = Source;
			return true;
		};

		return Mesh;
	}

	FMeshAnalysisSource MakeBox(const double Size)
	{
		FMeshAnalysisSource Box;
		WEMSyntheticMeshes::AddBox(Box, FVector::ZeroVector, FVector(Size));
		return Box;
	}

	FPlacementCatalogRow* FindRow(TArray<TPair<FName, FPlacementCatalogRow>>& Rows, const TCHAR* Name)
	{
		for (TPair<FName, FPlacementCatalogRow>& Row : Rows)
		{
			if (Row.Key == FName(Name))
			{
				return &Row.Value;
			}
		}

		return nullptr;
	}

	FCatalogBuildOptions MakeOptions()
	{
		FCatalogBuildOptions Options;
		Options.DoesMeshExist = [](const FSoftObjectPath&) { return false; };
		return Options;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementCatalogMatchingTest, "WEM.Placement.Catalog.Matching", PlacementCatalogBuildTestsPrivate::TestFlags)

bool FPlacementCatalogMatchingTest::RunTest(const FString& Parameters)
{
	using namespace PlacementCatalogBuildTestsPrivate;

	UPlacementArchetype* Furniture = MakeArchetype(TEXT("DA_Furniture"), { TEXT("/Game/Furniture") }, {});
	UPlacementArchetype* Desks = MakeArchetype(TEXT("DA_Desk"), { TEXT("/Game/Furniture/Desks/") }, {});
	UPlacementArchetype* ByName = MakeArchetype(TEXT("DA_DeskByName"), {}, { TEXT("desk") }, 10);
	const TArray<UPlacementArchetype*> Archetypes = { Furniture, Desks, ByName };

	const FSoftObjectPath OakDesk = MakeMeshPath(TEXT("/Game/Furniture/Desks/Oak"), TEXT("SM_Desk_Oak_A"));
	const FArchetypeMatch OakMatch = WEMCatalogBuild::MatchArchetype(OakDesk, Archetypes);

	TestTrue(TEXT("The deeper folder wins"), OakMatch.Archetype == Desks);
	TestEqual(TEXT("A mesh in a folder below the matched one takes that folder as its set"), WEMCatalogBuild::MakeSetName(OakDesk, OakMatch), FName(TEXT("Oak")));

	const FSoftObjectPath LooseDesk = MakeMeshPath(TEXT("/Game/Furniture"), TEXT("SM_Desk_Loose"));
	TestTrue(TEXT("A folder match beats a name token, however high the token's priority"), WEMCatalogBuild::MatchArchetype(LooseDesk, Archetypes).Archetype == Furniture);
	TestEqual(TEXT("A mesh directly in its matched folder has no set"),
		WEMCatalogBuild::MakeSetName(LooseDesk, WEMCatalogBuild::MatchArchetype(LooseDesk, Archetypes)), FName(NAME_None));

	const FSoftObjectPath ElsewhereDesk = MakeMeshPath(TEXT("/Game/Props"), TEXT("SM_DESK_Metal"));
	TestTrue(TEXT("With no folder, a whole name token matches, ignoring case"), WEMCatalogBuild::MatchArchetype(ElsewhereDesk, Archetypes).Archetype == ByName);

	const FSoftObjectPath Desktop = MakeMeshPath(TEXT("/Game/Props"), TEXT("SM_Desktop"));
	TestTrue(TEXT("Part of a token is no match"), WEMCatalogBuild::MatchArchetype(Desktop, Archetypes).Archetype == nullptr);

	UPlacementArchetype* Higher = MakeArchetype(TEXT("DA_WritingDesk"), {}, { TEXT("Desk") }, 20);
	TestTrue(TEXT("Between name matches the higher priority wins"),
		WEMCatalogBuild::MatchArchetype(ElsewhereDesk, { Furniture, Desks, ByName, Higher }).Archetype == Higher);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementCatalogBuildRowsTest, "WEM.Placement.Catalog.BuildRows", PlacementCatalogBuildTestsPrivate::TestFlags)

bool FPlacementCatalogBuildRowsTest::RunTest(const FString& Parameters)
{
	using namespace PlacementCatalogBuildTestsPrivate;

	UPlacementArchetype* Desks = MakeArchetype(TEXT("DA_Desk"), { TEXT("/Game/Furniture/Desks") }, {});
	UPlacementArchetype* Boxes = MakeArchetype(TEXT("DA_Box"), { TEXT("/Game/Furniture/Boxes") }, {});
	const TArray<UPlacementArchetype*> Archetypes = { Desks, Boxes };

	const FSoftObjectPath DeskPath = MakeMeshPath(TEXT("/Game/Furniture/Desks/Oak"), TEXT("SM_Desk_Oak_A"));
	const FSoftObjectPath BoxPath = MakeMeshPath(TEXT("/Game/Furniture/Boxes"), TEXT("SM_Box"));
	const FSoftObjectPath LampPath = MakeMeshPath(TEXT("/Game/Props"), TEXT("SM_Lamp"));

	TArray<TPair<FName, FPlacementCatalogRow>> Rows;

	// The first build adds a row per mesh.
	{
		FCatalogBuildReport Report;
		WEMCatalogBuild::BuildRows(Rows,
			{ MakeMesh(DeskPath, WEMSyntheticMeshes::MakeDesk()), MakeMesh(BoxPath, MakeBox(60.0)), MakeMesh(LampPath, MakeBox(20.0)) },
			Archetypes, MakeOptions(), Report);

		TestEqual(TEXT("Three meshes add three rows"), Report.Added.Num(), 3);
		TestEqual(TEXT("The lamp matches nothing"), Report.Unmatched.Num(), 1);

		const FPlacementCatalogRow* Lamp = FindRow(Rows, TEXT("SM_Lamp"));
		TestTrue(TEXT("An unmatched mesh gets a row, disabled and with no archetype"), Lamp && !Lamp->bEnabled && !Lamp->Archetype);
		TestTrue(TEXT("An unmatched mesh is named in the report"),
			Report.Notes.ContainsByPredicate([](const FCatalogBuildReport::FNote& Note) { return Note.Row == FName(TEXT("SM_Lamp")); }));

		const FPlacementCatalogRow* Desk = FindRow(Rows, TEXT("SM_Desk_Oak_A"));
		TestTrue(TEXT("The desk is measured"), Desk && Desk->Geometry.SizeCells == FIntVector(3, 6, 4));
		TestTrue(TEXT("The desk is in the Oak set"), Desk && Desk->SetName == FName(TEXT("Oak")));
		TestTrue(TEXT("The desk records the cell size it was measured at"), Desk && Desk->BuiltCellSize == 20.0f);
	}

	// People's columns, and a locked archetype, survive a rebuild.
	FPlacementCatalogRow* Desk = FindRow(Rows, TEXT("SM_Desk_Oak_A"));
	Desk->Weight = 3.0f;
	Desk->MaxInstances = 2;
	Desk->bOverrideSet = true;
	Desk->OverrideSetName = TEXT("Walnut");
	Desk->bArchetypeLocked = true;
	Desk->Archetype = Boxes;

	{
		FCatalogBuildReport Report;
		WEMCatalogBuild::BuildRows(Rows,
			{ MakeMesh(DeskPath, WEMSyntheticMeshes::MakeDesk()), MakeMesh(BoxPath, MakeBox(60.0)), MakeMesh(LampPath, MakeBox(20.0)) },
			Archetypes, MakeOptions(), Report);

		Desk = FindRow(Rows, TEXT("SM_Desk_Oak_A"));
		TestEqual(TEXT("A rebuild keeps a row's weight"), Desk->Weight, 3.0f);
		TestEqual(TEXT("A rebuild keeps a row's instance limit"), Desk->MaxInstances, 2);
		TestTrue(TEXT("A rebuild keeps a row's overrides"), Desk->bOverrideSet && Desk->OverrideSetName == FName(TEXT("Walnut")));
		TestTrue(TEXT("A rebuild keeps a locked archetype"), Desk->Archetype == Boxes);
		TestEqual(TEXT("Nothing is added a second time"), Report.Added.Num(), 0);
		TestTrue(TEXT("An unchanged mesh is not measured again"), Report.Unchanged.Contains(FName(TEXT("SM_Box"))));
	}

	// A changed mesh is measured again; an unchanged one is not.
	{
		FCatalogBuildReport Report;
		WEMCatalogBuild::BuildRows(Rows,
			{ MakeMesh(DeskPath, WEMSyntheticMeshes::MakeDesk()), MakeMesh(BoxPath, MakeBox(80.0)), MakeMesh(LampPath, MakeBox(20.0)) },
			Archetypes, MakeOptions(), Report);

		TestTrue(TEXT("A changed mesh's row is updated"), Report.Updated.Contains(FName(TEXT("SM_Box"))));
		TestEqual(TEXT("A changed mesh's derived columns are rewritten"), FindRow(Rows, TEXT("SM_Box"))->Geometry.SizeCells, FIntVector(4, 4, 4));
		TestTrue(TEXT("An unchanged mesh is left alone"), Report.Unchanged.Contains(FName(TEXT("SM_Desk_Oak_A"))));
	}

	// A mesh that has gone disables its row, and never deletes it.
	{
		FCatalogBuildReport Report;
		WEMCatalogBuild::BuildRows(Rows,
			{ MakeMesh(DeskPath, WEMSyntheticMeshes::MakeDesk()), MakeMesh(LampPath, MakeBox(20.0)) },
			Archetypes, MakeOptions(), Report);

		const FPlacementCatalogRow* Box = FindRow(Rows, TEXT("SM_Box"));
		TestTrue(TEXT("A row whose mesh has gone is kept"), Box != nullptr);
		TestTrue(TEXT("A row whose mesh has gone is disabled"), Box && !Box->bEnabled);
		TestTrue(TEXT("A row whose mesh has gone is reported"), Report.Orphaned.Contains(FName(TEXT("SM_Box"))));
	}

	// When it comes back, the row the builder disabled is enabled again.
	{
		FCatalogBuildReport Report;
		WEMCatalogBuild::BuildRows(Rows,
			{ MakeMesh(DeskPath, WEMSyntheticMeshes::MakeDesk()), MakeMesh(BoxPath, MakeBox(80.0)), MakeMesh(LampPath, MakeBox(20.0)) },
			Archetypes, MakeOptions(), Report);

		const FPlacementCatalogRow* Box = FindRow(Rows, TEXT("SM_Box"));
		TestTrue(TEXT("A row the builder disabled is enabled again once its mesh is back"), Box && Box->bEnabled);
	}

	// Two meshes of the same name are told apart by a suffix.
	{
		FCatalogBuildReport Report;
		const FSoftObjectPath OtherBox = MakeMeshPath(TEXT("/Game/Furniture/Boxes/Old"), TEXT("SM_Box"));

		WEMCatalogBuild::BuildRows(Rows,
			{ MakeMesh(DeskPath, WEMSyntheticMeshes::MakeDesk()), MakeMesh(BoxPath, MakeBox(80.0)), MakeMesh(OtherBox, MakeBox(40.0)), MakeMesh(LampPath, MakeBox(20.0)) },
			Archetypes, MakeOptions(), Report);

		TestTrue(TEXT("A second mesh of the same name gets a suffixed row"), FindRow(Rows, TEXT("SM_Box_2")) != nullptr);
		TestTrue(TEXT("The duplicate name is reported"),
			Report.Notes.ContainsByPredicate([](const FCatalogBuildReport::FNote& Note) { return Note.Row == FName(TEXT("SM_Box_2")); }));
	}

	return true;
}

#endif
