// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PlacementArchetype.h"
#include "PlacementCatalog.h"
#include "PlacementCatalogPreview.h"
#include "SPlacementFootprint.h"
#include "UObject/Package.h"

namespace PlacementEditorViewTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FPlacementCatalogRow MakeRow(UPlacementArchetype* Archetype, const FIntVector& Size)
	{
		FPlacementCatalogRow Row;
		Row.Archetype = Archetype;
		Row.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
		Row.BuiltCellSize = 20.0f;
		Row.Geometry.SizeCells = Size;
		Row.Geometry.LocalBoundsCm = FBox(FVector(-50.0, -50.0, -50.0), FVector(50.0, 50.0, 50.0));

		FDerivedSurfaceLayer& Top = Row.Geometry.Surfaces.AddDefaulted_GetRef();
		Top.HeightCm = 100.0f;
		Top.CellMask.Init(1, Size.X * Size.Y);

		FSpecialEdge& Special = Row.Geometry.SpecialEdges.AddDefaulted_GetRef();
		Special.Id = 1;
		Special.Side = EObjectSide::Front;
		Special.Start = 1;
		Special.Count = FMath::Max(1, Size.Y - 2);
		return Row;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementFootprintWidgetTest, "WEM.Placement.Editor.FootprintView", PlacementEditorViewTestsPrivate::TestFlags)

bool FPlacementFootprintWidgetTest::RunTest(const FString& Parameters)
{
	UPlacementArchetype* Couch = NewObject<UPlacementArchetype>(GetTransientPackage());
	FExclusionZone& Zone = Couch->ExclusionZones.AddDefaulted_GetRef();
	Zone.SideMask = 1 << static_cast<int32>(EObjectSide::Front);
	Zone.DepthCells = 3;

	const FPlacementCatalogRow Row = PlacementEditorViewTestsPrivate::MakeRow(Couch, FIntVector(5, 5, 5));

	TSharedRef<SPlacementFootprint> Widget = SNew(SPlacementFootprint)
		.OnGetView([&Row, Couch](FPlacementFootprintView& OutView)
		{
			OutView.Geometry = Row.GetEffectiveGeometry();
			OutView.Zones = Couch->ExclusionZones;
			return true;
		});

	const FVector2D Size = Widget->ComputeDesiredSize(1.0f);
	TestTrue(TEXT("The footprint view asks for room to draw in"), Size.X > 100.0 && Size.Y > 100.0);

	TSharedRef<SPlacementFootprint> Empty = SNew(SPlacementFootprint);
	TestTrue(TEXT("With nothing to show it still has a size"), Empty->ComputeDesiredSize(1.0f).X > 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlacementPreviewTest, "WEM.Placement.Editor.Preview", PlacementEditorViewTestsPrivate::TestFlags)

bool FPlacementPreviewTest::RunTest(const FString& Parameters)
{
	using namespace PlacementEditorViewTestsPrivate;

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("WEMPreviewTestWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);

	UPlacementArchetype* Desk = NewObject<UPlacementArchetype>(GetTransientPackage(), TEXT("DA_PreviewDesk"));
	Desk->AddToRoot();

	UDataTable* Table = NewObject<UDataTable>(GetTransientPackage());
	Table->RowStruct = FPlacementCatalogRow::StaticStruct();
	Table->AddRow(TEXT("SM_Desk_A"), MakeRow(Desk, FIntVector(3, 6, 4)));
	Table->AddRow(TEXT("SM_Desk_B"), MakeRow(Desk, FIntVector(3, 5, 4)));

	FPlacementCatalogRow Unmatched = MakeRow(nullptr, FIntVector(2, 2, 2));
	Unmatched.bEnabled = false;
	Table->AddRow(TEXT("SM_Lamp"), Unmatched);

	APlacementCatalogPreview* Preview = World->SpawnActor<APlacementCatalogPreview>();
	Preview->Catalog = Table;
	const FVector2D Extent = Preview->Rebuild();

	TestTrue(TEXT("The preview lays the catalog out over some ground"), Extent.X > 0.0 && Extent.Y > 0.0);

	TArray<UStaticMeshComponent*> Meshes;
	Preview->GetComponents(Meshes);
	TestEqual(TEXT("Every row's mesh is shown, disabled and unmatched ones included"), Meshes.Num(), 3);

	TArray<UTextRenderComponent*> Labels;
	Preview->GetComponents(Labels);
	TestTrue(TEXT("Each group and each mesh is labelled"), Labels.Num() >= 5);

	// Running it again replaces what was there rather than adding to it.
	Preview->Rebuild();
	Preview->GetComponents(Meshes);
	TestEqual(TEXT("A second rebuild shows each mesh once"), Meshes.Num(), 3);

	Desk->RemoveFromRoot();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	return true;
}

#endif
