// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementCatalogBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "MeshDescription.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/UObjectToken.h"
#include "PlacementArchetype.h"
#include "PlacementCatalog.h"
#include "PlacementCatalogBuild.h"
#include "PlacementCatalogPreview.h"
#include "RoomManager.h"
#include "StaticMeshAttributes.h"

#define LOCTEXT_NAMESPACE "PlacementCatalogBuilder"

const FName UPlacementCatalogBuilder::MessageLogName(TEXT("PlacementCatalog"));

namespace PlacementCatalogBuilderPrivate
{
	IAssetRegistry& GetAssetRegistry()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		// A build straight after the editor opens would otherwise miss whatever is still being scanned.
		Registry.WaitForCompletion();
		return Registry;
	}

	/** The table's rows, in the table's own order. */
	TArray<TPair<FName, FPlacementCatalogRow>> ReadRows(const UDataTable& Catalog)
	{
		TArray<TPair<FName, FPlacementCatalogRow>> Rows;

		for (const TPair<FName, uint8*>& Entry : Catalog.GetRowMap())
		{
			Rows.Emplace(Entry.Key, *reinterpret_cast<const FPlacementCatalogRow*>(Entry.Value));
		}

		return Rows;
	}

	void AddAssetToken(const TSharedRef<FTokenizedMessage>& Message, const FSoftObjectPath& Path, const FName RowName)
	{
		if (Path.IsNull())
		{
			Message->AddToken(FTextToken::Create(FText::FromName(RowName)));
		}
		else
		{
			Message->AddToken(FAssetNameToken::Create(Path.GetLongPackageName(), FText::FromName(RowName)));
		}
	}
}

FMeshAnalysisSettings UPlacementCatalogBuilder::MakeAnalysisSettings() const
{
	FMeshAnalysisSettings Settings;
	Settings.CellSize = CellSize;
	Settings.SamplesPerCellAxis = SamplesPerCellAxis;
	Settings.HeightToleranceCm = HeightToleranceCm;
	Settings.MaxSurfaceSlopeDeg = MaxSurfaceSlopeDeg;
	Settings.MinLayerCells = MinLayerCells;
	return Settings;
}

bool UPlacementCatalogBuilder::LoadMeshSource(const UStaticMesh* Mesh, FMeshAnalysisSource& OutSource)
{
	OutSource = FMeshAnalysisSource();

	if (!Mesh)
	{
		return false;
	}

	// The source geometry as it was imported or modelled - not the render data, which is built from
	// it with whatever reduction and splitting that brings, and not collision, which is a separate
	// simplified shape altogether.
	const FMeshDescription* Description = Mesh->GetMeshDescription(0);

	if (!Description)
	{
		return false;
	}

	const FStaticMeshConstAttributes Attributes(*Description);
	const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

	// Vertices are numbered in the order the triangles first use them, so the hash of an unchanged
	// mesh never depends on how its description happens to number or pad its vertex array.
	TMap<int32, int32> CompactIndices;

	for (const FTriangleID Triangle : Description->Triangles().GetElementIDs())
	{
		for (const FVertexID Vertex : Description->GetTriangleVertices(Triangle))
		{
			int32& Index = CompactIndices.FindOrAdd(Vertex.GetValue(), INDEX_NONE);

			if (Index == INDEX_NONE)
			{
				Index = OutSource.Positions.Add(Positions[Vertex]);
			}

			OutSource.Indices.Add(Index);
		}
	}

	for (const UStaticMeshSocket* Socket : Mesh->Sockets)
	{
		if (Socket)
		{
			OutSource.Sockets.Add({ Socket->SocketName, Socket->RelativeLocation });
		}
	}

	return !OutSource.Indices.IsEmpty();
}

TArray<UPlacementArchetype*> UPlacementCatalogBuilder::GatherArchetypes() const
{
	TArray<UPlacementArchetype*> Found;

	if (!bDiscoverArchetypes)
	{
		for (UPlacementArchetype* Archetype : Archetypes)
		{
			if (Archetype)
			{
				Found.AddUnique(Archetype);
			}
		}

		return Found;
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UPlacementArchetype::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	PlacementCatalogBuilderPrivate::GetAssetRegistry().GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets)
	{
		if (UPlacementArchetype* Archetype = Cast<UPlacementArchetype>(Asset.GetAsset()))
		{
			Found.AddUnique(Archetype);
		}
	}

	return Found;
}

bool UPlacementCatalogBuilder::CheckCatalog() const
{
	FText Problem;

	if (!Catalog)
	{
		Problem = LOCTEXT("NoCatalog", "Set a Catalog data table to write to.");
	}
	else if (Catalog->GetRowStruct() != FPlacementCatalogRow::StaticStruct())
	{
		Problem = FText::Format(LOCTEXT("WrongRowStruct", "{0} does not hold PlacementCatalogRow rows."), FText::FromString(Catalog->GetName()));
	}

	if (Problem.IsEmpty())
	{
		return true;
	}

	FMessageLog Log(MessageLogName);
	Log.NewPage(FText::FromString(GetName()));
	Log.Error()->AddToken(FUObjectToken::Create(this))->AddToken(FTextToken::Create(Problem));
	Log.Open(EMessageSeverity::Error, /*bForce=*/true);
	return false;
}

void UPlacementCatalogBuilder::BuildCatalog()
{
	using namespace PlacementCatalogBuilderPrivate;

	if (!CheckCatalog())
	{
		return;
	}

	IAssetRegistry& Registry = GetAssetRegistry();

	FARFilter Filter;
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;

	for (const FDirectoryPath& Folder : SourceFolders)
	{
		if (!Folder.Path.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*Folder.Path));
		}
	}

	TArray<FAssetData> MeshAssets;
	if (!Filter.PackagePaths.IsEmpty())
	{
		Registry.GetAssets(Filter, MeshAssets);
	}

	// Meshes are only loaded when a row is built or checked, one at a time as the build reaches them.
	TArray<FCatalogMeshSource> Meshes;
	for (const FAssetData& Asset : MeshAssets)
	{
		FCatalogMeshSource& Mesh = Meshes.AddDefaulted_GetRef();
		Mesh.Path = Asset.GetSoftObjectPath();
		Mesh.Load = [Asset](FMeshAnalysisSource& OutSource)
		{
			return LoadMeshSource(Cast<UStaticMesh>(Asset.GetAsset()), OutSource);
		};
	}

	FCatalogBuildOptions Options;
	Options.Analysis = MakeAnalysisSettings();
	Options.bFullRebuild = bRebuildAllOnNextBuild;
	Options.DoesMeshExist = [&Registry](const FSoftObjectPath& Path)
	{
		return Registry.GetAssetByObjectPath(Path).IsValid();
	};

	TArray<TPair<FName, FPlacementCatalogRow>> Rows = ReadRows(*Catalog);
	FCatalogBuildReport Report;

	{
		FScopedSlowTask Progress(1.0f, FText::Format(LOCTEXT("Building", "Building the placement catalog from {0} meshes"), Meshes.Num()));
		Progress.MakeDialog();

		WEMCatalogBuild::BuildRows(Rows, Meshes, GatherArchetypes(), Options, Report);
	}

	// Written back row by row rather than emptied and refilled, so the table keeps its order and a
	// row editor left open on it keeps its place.
	Catalog->Modify();

	for (const TPair<FName, FPlacementCatalogRow>& Entry : Rows)
	{
		if (FPlacementCatalogRow* Existing = Catalog->FindRow<FPlacementCatalogRow>(Entry.Key, TEXT("PlacementCatalogBuilder"), /*bWarnIfRowMissing=*/false))
		{
			*Existing = Entry.Value;
		}
		else
		{
			Catalog->AddRow(Entry.Key, Entry.Value);
		}
	}

	Catalog->HandleDataTableChanged();
	Catalog->MarkPackageDirty();

	if (bRebuildAllOnNextBuild)
	{
		Modify();
		bRebuildAllOnNextBuild = false;
	}

	WriteReport(LOCTEXT("BuildTitle", "Build"), Report, /*bBuild=*/true);
}

void UPlacementCatalogBuilder::ValidateCatalog()
{
	using namespace PlacementCatalogBuilderPrivate;

	if (!CheckCatalog())
	{
		return;
	}

	const TArray<TPair<FName, FPlacementCatalogRow>> Rows = ReadRows(*Catalog);
	FCatalogBuildReport Report;

	FScopedSlowTask Progress(1.0f, LOCTEXT("Validating", "Validating the placement catalog"));
	Progress.MakeDialog();

	WEMCatalogBuild::ValidateRows(Rows, MakeAnalysisSettings(),
		[](const FSoftObjectPath& Path, FMeshAnalysisSource& OutSource)
		{
			return LoadMeshSource(Cast<UStaticMesh>(Path.TryLoad()), OutSource);
		},
		Report);

	WriteReport(LOCTEXT("ValidateTitle", "Validate"), Report, /*bBuild=*/false);
}

void UPlacementCatalogBuilder::SpawnPreviewInLevel()
{
	if (!CheckCatalog() || !GEditor)
	{
		return;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();

	if (!World)
	{
		return;
	}

	for (TActorIterator<APlacementCatalogPreview> It(World); It; ++It)
	{
		It->Destroy();
	}

	// Beside the room manager's cube rather than inside it: the layout runs from its location
	// toward -X and +Y, so it starts a little way short of the cube's near side.
	FVector Location = FVector::ZeroVector;

	for (TActorIterator<ARoomManager> It(World); It; ++It)
	{
		Location = FVector(It->GetActorLocation().X - 10.0 * It->CellSize, It->GetActorLocation().Y, It->GridBaseZ);
		break;
	}

	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;

	APlacementCatalogPreview* Preview = World->SpawnActor<APlacementCatalogPreview>(Location, FRotator::ZeroRotator, Parameters);

	if (!Preview)
	{
		return;
	}

	Preview->Catalog = Catalog;
	Preview->CellSize = CellSize;
	const FVector2D Extent = Preview->Rebuild();

	GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
	GEditor->SelectActor(Preview, /*bInSelected=*/true, /*bNotify=*/true);
	GEditor->MoveViewportCamerasToActor(*Preview, /*bActiveViewportOnly=*/false);

	FMessageLog Log(MessageLogName);
	Log.NewPage(FText::Format(LOCTEXT("PreviewTitle", "{0}: Preview"), FText::FromString(GetName())));
	Log.Info()
		->AddToken(FUObjectToken::Create(Catalog))
		->AddToken(FTextToken::Create(FText::Format(
			LOCTEXT("PreviewSummary", "{0} row(s) laid out over {1} x {2} m, from {3}."),
			Catalog->GetRowMap().Num(),
			FText::AsNumber(FMath::RoundToInt(Extent.X / 100.0)),
			FText::AsNumber(FMath::RoundToInt(Extent.Y / 100.0)),
			FText::FromString(Location.ToCompactString()))));
}

void UPlacementCatalogBuilder::WriteReport(const FText& Title, const FCatalogBuildReport& Report, const bool bBuild) const
{
	using namespace PlacementCatalogBuilderPrivate;

	FMessageLog Log(MessageLogName);
	Log.NewPage(FText::Format(LOCTEXT("PageTitle", "{0}: {1}"), FText::FromString(GetName()), Title));

	if (bBuild)
	{
		Log.Info()
			->AddToken(FUObjectToken::Create(Catalog))
			->AddToken(FTextToken::Create(FText::Format(
				LOCTEXT("BuildSummary", "{0} added, {1} updated, {2} unchanged, {3} unmatched, {4} orphaned."),
				Report.Added.Num(), Report.Updated.Num(), Report.Unchanged.Num(), Report.Unmatched.Num(), Report.Orphaned.Num())));
	}

	const int32 Errors = Report.CountErrors();

	Log.Info()
		->AddToken(FUObjectToken::Create(Catalog))
		->AddToken(FTextToken::Create(FText::Format(
			LOCTEXT("NoteSummary", "{0} error(s), {1} warning(s)."), Errors, Report.Notes.Num() - Errors)));

	for (const FCatalogBuildReport::FNote& Note : Report.Notes)
	{
		const TSharedRef<FTokenizedMessage> Message = Note.bError ? Log.Error() : Log.Warning();
		AddAssetToken(Message, Note.Mesh, Note.Row);
		Message->AddToken(FTextToken::Create(FText::FromString(Note.Text)));
	}

	Log.Open(Errors > 0 ? EMessageSeverity::Error : EMessageSeverity::Info, /*bForce=*/true);

	if (Catalog)
	{
		UE_LOG(LogTemp, Log, TEXT("PlacementCatalogBuilder: %s of %s - %d error(s), %d warning(s). See the Placement Catalog message log."),
			*Title.ToString(), *Catalog->GetName(), Errors, Report.Notes.Num() - Errors);
	}
}

#undef LOCTEXT_NAMESPACE
