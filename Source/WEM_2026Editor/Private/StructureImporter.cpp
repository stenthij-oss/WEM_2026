// Copyright Epic Games, Inc. All Rights Reserved.

#include "StructureImporter.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeManager.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "ObjectTools.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/UObjectToken.h"
#include "PlatformStructureData.h"
#include "RoomManager.h"
#include "ScopedTransaction.h"
#include "StructureCategory.h"
#include "StructureImport.h"

#define LOCTEXT_NAMESPACE "StructureImporter"

const FName UStructureImporter::MessageLogName(TEXT("StructureImport"));

namespace StructureImporterPrivate
{
	IAssetRegistry& GetAssetRegistry()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		// An import straight after the editor opens would otherwise miss whatever is still being scanned.
		Registry.WaitForCompletion();
		return Registry;
	}

	/** Every structure category in the project, by asset name, in name order. */
	TArray<FAssetData> FindCategories()
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UStructureCategory::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		GetAssetRegistry().GetAssets(Filter, Assets);

		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.AssetName.LexicalLess(B.AssetName);
		});

		return Assets;
	}

	FString MakeObjectPath(const FString& Folder, const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s.%s"), *Folder, *AssetName, *AssetName);
	}

	/** Where an asset of that name already is below Root, if anywhere. */
	FAssetData FindAssetBelow(const FString& Root, const FString& AssetName)
	{
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Root));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		GetAssetRegistry().GetAssets(Filter, Assets);

		const FAssetData* Found = Assets.FindByPredicate([&AssetName](const FAssetData& Asset)
		{
			return Asset.AssetName.ToString() == AssetName;
		});

		return Found ? *Found : FAssetData();
	}

	/**
	 * The engine's own pipeline for glTF assets, as an import would use it, except that everything
	 * lands straight in the destination folder - the defaults put each file's assets in a folder named
	 * after it, and a folder per asset type below that - and materials come in as materials of the
	 * project's own. The engine's glTF pipeline is left out: it makes instances of glTF parent materials
	 * in the engine's content, which may not be drawn instanced, and the room manager draws every
	 * structure instanced.
	 */
	UInterchangePipelineStackOverride* MakePipelineStack()
	{
		UInterchangePipelineStackOverride* Stack = NewObject<UInterchangePipelineStackOverride>();

		const UInterchangeGenericAssetsPipeline* DefaultAssets = LoadObject<UInterchangeGenericAssetsPipeline>(
			nullptr, TEXT("/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline"));

		if (UInterchangeGenericAssetsPipeline* Assets = DefaultAssets ? DuplicateObject(DefaultAssets, GetTransientPackage()) : nullptr)
		{
			Assets->bSceneNameSubFolder = false;
			Assets->bAssetTypeSubFolders = false;

			if (Assets->MaterialPipeline)
			{
				Assets->MaterialPipeline->MaterialImport = EInterchangeMaterialImportOption::ImportAsMaterials;
			}

			Stack->AddPipeline(Assets);
		}

		return Stack;
	}

	/** What a structure asset holds of what the settings file gives it. */
	FStructureValues ReadValues(const UPlatformStructureData& Structure)
	{
		FStructureValues Values;
		Values.Width = Structure.Width;
		Values.Length = Structure.Length;
		Values.bTwoSided = Structure.bTwoSided;
		Values.Types = Structure.Types;
		Values.Side = Structure.Side;
		Values.bLoadBearing = Structure.bLoadBearing;
		Values.bHoldsItems = Structure.bHoldsItems;
		Values.bFence = Structure.bFence;
		Values.bMayLieSideways = Structure.bMayLieSideways;

		for (const UStructureCategory* Category : Structure.Categories)
		{
			Values.Categories.Add(Category ? Category->GetName() : TEXT("(empty)"));
		}

		return Values;
	}
}

UStructureImporter::UStructureImporter()
{
	SourceFolder.Path = TEXT("SourceArt/Structures/Export");
	DestinationFolder.Path = TEXT("/Game/Structures");
	CategoryListFile.FilePath = TEXT("SourceArt/Structures/categories.json");
	MaterialFolder.Path = TEXT("/Game/Structures/Materials");
}

void UStructureImporter::Import()
{
	const FString SourceDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / SourceFolder.Path);

	TArray<FString> SettingsFiles;
	IFileManager::Get().FindFiles(SettingsFiles, *(SourceDirectory / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);

	// In name order, so a report reads the same from one import to the next.
	SettingsFiles.Sort();

	TArray<FString> Lines;

	// The list goes out first, so a category made since the last import is offered in Blender at once.
	FString CategoryListPath;
	const int32 CategoryCount = WriteCategoryListFile(CategoryListPath);
	Lines.Add(CategoryCount == INDEX_NONE
		? FString::Printf(TEXT("The category list could not be written to %s."), *CategoryListPath)
		: FString::Printf(TEXT("%d categories listed in %s."), CategoryCount, *CategoryListPath));

	TArray<FImportResult> Results;
	Results.SetNum(SettingsFiles.Num());

	{
		FScopedSlowTask Progress(SettingsFiles.Num(), FText::Format(LOCTEXT("Importing", "Importing {0} structure(s)"), SettingsFiles.Num()));
		Progress.MakeDialog();

		for (int32 Index = 0; Index < SettingsFiles.Num(); ++Index)
		{
			Progress.EnterProgressFrame(1.0f, FText::FromString(SettingsFiles[Index]));

			Results[Index].File = SettingsFiles[Index];
			ImportStructure(SourceDirectory / SettingsFiles[Index], Results[Index]);
		}
	}

	TArray<UPlatformStructureData*> NewStructures;
	int32 Failed = 0;
	int32 Unchanged = 0;

	for (const FImportResult& Result : Results)
	{
		Failed += Result.Errors.IsEmpty() ? 0 : 1;
		Unchanged += Result.Outcome == TEXT("unchanged") ? 1 : 0;

		if (Result.bCreated && Result.Structure)
		{
			NewStructures.Add(Result.Structure);
		}
	}

	// Any structure brought in again has a new mesh, which the room managers draw only once they rebuild.
	const bool bAnyUpdated = Results.Num() - NewStructures.Num() - Unchanged - Failed > 0;
	const int32 RoomManagers = AddToRoomManagers(NewStructures, bAnyUpdated);

	if (bReimportAll)
	{
		Modify();
		bReimportAll = false;
	}

	const FString Summary = FString::Printf(
		TEXT("%d settings file(s) in %s: %d new, %d updated, %d unchanged, %d failed. New structures added to %d room manager(s)."),
		Results.Num(), *SourceDirectory, NewStructures.Num(), Results.Num() - NewStructures.Num() - Unchanged - Failed, Unchanged, Failed, RoomManagers);
	Lines.Insert(Summary, 0);

	FMessageLog Log(MessageLogName);
	Log.NewPage(FText::Format(LOCTEXT("PageTitle", "{0}: Import"), FText::FromString(GetName())));
	Log.Info()->AddToken(FUObjectToken::Create(this))->AddToken(FTextToken::Create(FText::FromString(Summary)));

	for (const FImportResult& Result : Results)
	{
		const FString Name = Result.Name.IsEmpty() ? Result.File : Result.Name;
		Lines.Add(FString::Printf(TEXT("%s: %s"), *Name, *Result.Outcome));

		const TSharedRef<FTokenizedMessage> Message = Result.Errors.IsEmpty() ? Log.Info() : Log.Error();

		if (Result.Structure)
		{
			Message->AddToken(FUObjectToken::Create(Result.Structure));
		}

		Message->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s: %s"), *Name, *Result.Outcome))));

		for (const FString& Error : Result.Errors)
		{
			Lines.Add(TEXT("  error: ") + Error);
			Log.Error()->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s: %s"), *Name, *Error))));
		}

		for (const FString& Note : Result.Notes)
		{
			Lines.Add(TEXT("  ") + Note);
			Log.Warning()->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s: %s"), *Name, *Note))));
		}
	}

	Log.Open(Failed > 0 ? EMessageSeverity::Error : EMessageSeverity::Info, /*bForce=*/true);

	Modify();
	LastReport = FString::Join(Lines, TEXT("\n"));

	UE_LOG(LogTemp, Log, TEXT("StructureImporter: %s See the Structure Import message log."), *Summary);
}

void UStructureImporter::WriteCategoryList()
{
	FString Path;
	const int32 Count = WriteCategoryListFile(Path);

	const FString Summary = Count == INDEX_NONE
		? FString::Printf(TEXT("The category list could not be written to %s."), *Path)
		: FString::Printf(TEXT("%d categories listed in %s."), Count, *Path);

	FMessageLog Log(MessageLogName);
	Log.NewPage(FText::Format(LOCTEXT("CategoriesTitle", "{0}: Categories"), FText::FromString(GetName())));
	(Count == INDEX_NONE ? Log.Error() : Log.Info())->AddToken(FTextToken::Create(FText::FromString(Summary)));

	UE_LOG(LogTemp, Log, TEXT("StructureImporter: %s"), *Summary);
}

int32 UStructureImporter::WriteCategoryListFile(FString& OutPath) const
{
	using namespace StructureImporterPrivate;

	OutPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / CategoryListFile.FilePath);

	TArray<TPair<FString, FString>> Categories;

	for (const FAssetData& Asset : FindCategories())
	{
		const UStructureCategory* Category = Cast<UStructureCategory>(Asset.GetAsset());
		const FString AssetName = Asset.AssetName.ToString();

		Categories.Emplace(AssetName, Category && !Category->DisplayName.IsEmpty() ? Category->DisplayName.ToString() : AssetName);
	}

	const bool bWritten = FFileHelper::SaveStringToFile(
		WEMStructureImport::FormatCategoryList(Categories), *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	return bWritten ? Categories.Num() : INDEX_NONE;
}

void UStructureImporter::ImportStructure(const FString& SettingsPath, FImportResult& Result)
{
	using namespace StructureImporterPrivate;

	Result.Outcome = TEXT("failed");

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *SettingsPath))
	{
		Result.Errors.Add(TEXT("the settings file could not be read"));
		return;
	}

	FStructureSettings Settings;
	FString Error;

	if (!WEMStructureImport::ParseSettings(Json, Settings, Error))
	{
		Result.Errors.Add(FString::Printf(TEXT("the settings file is refused: %s"), *Error));
		return;
	}

	Result.Name = Settings.Name;
	const FString MeshFile = FPaths::GetPath(SettingsPath) / Settings.Name + TEXT(".glb");

	if (!FPaths::FileExists(MeshFile))
	{
		Result.Errors.Add(FString::Printf(TEXT("there is no %s.glb beside it"), *Settings.Name));
		return;
	}

	// The size is never taken on trust: it is measured from the bounds again, in this importer's cells.
	int32 MeasuredWidth = 0;
	int32 MeasuredLength = 0;

	if (!WEMStructureImport::MeasureCells((Settings.BoundsMax.X - Settings.BoundsMin.X) * 100.0, CellSize, MeasuredWidth)
		|| !WEMStructureImport::MeasureCells((Settings.BoundsMax.Y - Settings.BoundsMin.Y) * 100.0, CellSize, MeasuredLength)
		|| MeasuredWidth != Settings.Width
		|| MeasuredLength != Settings.Length)
	{
		Result.Errors.Add(FString::Printf(TEXT("its bounds are not the opening of a %d x %d structure in %g cm cells"), Settings.Width, Settings.Length, CellSize));
		return;
	}

	const FString Folder = DestinationFolder.Path / FString::Printf(TEXT("%dx%d"), Settings.Width, Settings.Length);
	const FString MeshName = TEXT("SM_") + Settings.Name;
	const FString StructureName = TEXT("DA_Structure_") + Settings.Name;

	// A structure of that name already made for another size would be left behind with its old mesh.
	const FAssetData Existing = FindAssetBelow(DestinationFolder.Path, StructureName);

	if (Existing.IsValid() && Existing.PackagePath.ToString() != Folder)
	{
		Result.Errors.Add(FString::Printf(TEXT("%s already stands in %s, for another size; delete it there, or give the object another name"),
			*StructureName, *Existing.PackagePath.ToString()));
		return;
	}

	const FString Hash = WEMStructureImport::HashFiles({ MeshFile, SettingsPath });
	UPlatformStructureData* Previous = Existing.IsValid() ? Cast<UPlatformStructureData>(Existing.GetAsset()) : nullptr;

	if (Previous && Previous->Mesh && !Hash.IsEmpty() && Previous->ImportHash == Hash && !bReimportAll)
	{
		PrepareMaterials(*Previous->Mesh, Result);
		Result.Structure = Previous;
		Result.Outcome = TEXT("unchanged");
		return;
	}

	// A changed structure comes in afresh rather than as a re-import over its old mesh: a re-import only
	// looks for the materials the mesh had, so new or changed ones would never come in. Nothing but
	// the structure asset holds the mesh, and that is pointed at the new one below.
	if (UStaticMesh* OldMesh = Previous ? LoadObject<UStaticMesh>(nullptr, *MakeObjectPath(Folder, MeshName), nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr)
	{
		ObjectTools::ForceDeleteObjects({ OldMesh }, /*ShowConfirmation=*/false);
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = MeshFile;
	Task->DestinationPath = Folder;
	Task->DestinationName = MeshName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = false;
	Task->Options = MakePipelineStack();

	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });

	UStaticMesh* Mesh = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Mesh = Mesh ? Mesh : Cast<UStaticMesh>(Object);
	}

	if (!Mesh)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, *MakeObjectPath(Folder, MeshName), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	if (!Mesh || !Mesh->GetMeshDescription(0))
	{
		Result.Errors.Add(FString::Printf(TEXT("%s.glb did not import as a static mesh"), *Settings.Name));
		return;
	}

	// Measured from the mesh as it came in, in its own frame, against what the add-on measured in
	// Blender's: the check that the axes came across the way the room manager lays them.
	const FMeshDescription& Description = *Mesh->GetMeshDescription(0);
	const FBox Expected = WEMStructureImport::GetExpectedMeshBounds(Settings);
	const FBox Measured = Description.ComputeBoundingBox();

	if (!WEMStructureImport::BoundsMatch(Expected, Measured))
	{
		Result.Errors.Add(FString::Printf(TEXT("the imported mesh's bounds %s do not match the settings file's %s"), *Measured.ToString(), *Expected.ToString()));
		return;
	}

	MapMaterials(*Mesh, Result);
	PrepareMaterials(*Mesh, Result);

	// Dense relief is drawn with Nanite; a plain slab is cheaper without it.
	const int32 Triangles = Description.Triangles().Num();

	if (Triangles > NaniteTriangleThreshold && !Mesh->GetNaniteSettings().bEnabled)
	{
		Mesh->Modify();
		FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
		Nanite.bEnabled = true;
		Mesh->SetNaniteSettings(Nanite);
		Mesh->PostEditChange();
		Result.Notes.Add(FString::Printf(TEXT("Nanite is on: %d triangles, more than %d"), Triangles, NaniteTriangleThreshold));
	}

	bool bCreated = false;
	UPlatformStructureData* Structure = Previous ? Previous : FindOrCreateStructure(Folder, StructureName, bCreated);

	if (!Structure)
	{
		Result.Errors.Add(TEXT("its structure asset could not be made"));
		return;
	}

	TArray<FString> UnknownTypes;
	const int32 Types = WEMStructureImport::MakeTypeMask(Settings.Types, UnknownTypes);

	for (const FString& Type : UnknownTypes)
	{
		Result.Notes.Add(FString::Printf(TEXT("type '%s' is not Ground, Ceiling, Wall or Roof, and is ignored"), *Type));
	}

	EStructureSide Side = EStructureSide::Exterior;

	if (!WEMStructureImport::MakeSide(Settings.Side, Side))
	{
		Result.Notes.Add(FString::Printf(TEXT("side '%s' is not Exterior, Interior or Both, so it stands for an exterior"), *Settings.Side));
	}

	TMap<FString, UStructureCategory*> KnownCategories;
	for (const FAssetData& Asset : FindCategories())
	{
		KnownCategories.Add(Asset.AssetName.ToString(), Cast<UStructureCategory>(Asset.GetAsset()));
	}

	TArray<TObjectPtr<UStructureCategory>> Categories;

	for (const FString& CategoryName : Settings.Categories)
	{
		UStructureCategory* const* Category = KnownCategories.Find(CategoryName);

		if (!Category || !*Category)
		{
			Result.Notes.Add(FString::Printf(TEXT("there is no category asset named '%s'"), *CategoryName));
			continue;
		}

		Categories.AddUnique(*Category);
	}

	if (Categories.IsEmpty())
	{
		Result.Notes.Add(TEXT("it has no category, so it takes no part in the category pull"));
	}

	if (Settings.bFence && !Settings.bTwoSided)
	{
		Result.Notes.Add(TEXT("it is a fence but not two-sided, so growth leaves it out"));
	}

	const FStructureValues Before = ReadValues(*Structure);

	Structure->Modify();
	Structure->Mesh = Mesh;
	Structure->Width = Settings.Width;
	Structure->Length = Settings.Length;
	Structure->bTwoSided = Settings.bTwoSided;
	Structure->Categories = Categories;
	Structure->Types = Types;
	Structure->Side = Side;
	Structure->bLoadBearing = Settings.bLoadBearing;
	Structure->bHoldsItems = Settings.bHoldsItems;
	Structure->bFence = Settings.bFence;
	Structure->bMayLieSideways = Settings.bMayLieSideways;
	Structure->SourceBlend = Settings.Blend;
	Structure->SourceObject = Settings.Object;
	Structure->ImportHash = Hash;

	// Weights are tuned in Unreal while watching growth, so the file only gives a new structure its first.
	if (bCreated)
	{
		Structure->Weight = Settings.Weight;
	}

	Structure->MarkPackageDirty();

	Result.Structure = Structure;
	Result.bCreated = bCreated;

	if (bCreated)
	{
		Result.Outcome = FString::Printf(TEXT("new, %d x %d, in %s"), Settings.Width, Settings.Length, *Folder);
		return;
	}

	// Every setting the file overwrote, so nothing set in Unreal is lost without a word.
	const TArray<FString> Changes = WEMStructureImport::DescribeChanges(Before, ReadValues(*Structure));
	Result.Outcome = Changes.IsEmpty() ? TEXT("updated, its settings as they were") : TEXT("updated: ") + FString::Join(Changes, TEXT("; "));
}

void UStructureImporter::MapMaterials(UStaticMesh& Mesh, FImportResult& Result) const
{
	using namespace StructureImporterPrivate;

	IAssetRegistry& Registry = GetAssetRegistry();
	const TArray<FStaticMaterial>& Slots = Mesh.GetStaticMaterials();

	for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
	{
		const FString SlotName = Slots[Slot].MaterialSlotName.ToString();
		const FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(MakeObjectPath(MaterialFolder.Path, SlotName)));
		UMaterialInterface* Material = Asset.IsValid() ? Cast<UMaterialInterface>(Asset.GetAsset()) : nullptr;

		if (Material && Mesh.GetMaterial(Slot) != Material)
		{
			Mesh.Modify();
			Mesh.SetMaterial(Slot, Material);
			Result.Notes.Add(FString::Printf(TEXT("slot %s takes %s"), *SlotName, *Material->GetPathName()));
		}
	}
}

void UStructureImporter::PrepareMaterials(UStaticMesh& Mesh, FImportResult& Result) const
{
	const FString MeshFolder = FPackageName::GetLongPackagePath(Mesh.GetPackage()->GetName());
	const TArray<FStaticMaterial>& Slots = Mesh.GetStaticMaterials();

	for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
	{
		const FString SlotName = Slots[Slot].MaterialSlotName.ToString();
		UMaterial* Base = Mesh.GetMaterial(Slot) ? Mesh.GetMaterial(Slot)->GetMaterial() : nullptr;

		// glTF's blended alpha comes in translucent, which instanced drawing cannot sort. A material
		// that came in with the mesh is changed; one chosen in Unreal is only reported.
		if (Base && Base->BlendMode == BLEND_Translucent)
		{
			if (FPackageName::GetLongPackagePath(Base->GetPackage()->GetName()) == MeshFolder)
			{
				WEMStructureImport::MaskTranslucency(*Base);
				Base->PostEditChange();
				Base->MarkPackageDirty();
				Result.Notes.Add(FString::Printf(TEXT("slot %s's material %s is drawn masked and dithered rather than translucent"), *SlotName, *Base->GetName()));
			}
			else
			{
				Result.Notes.Add(FString::Printf(TEXT("slot %s's material %s is translucent, which the room manager cannot sort when it draws instanced; make it masked"),
					*SlotName, *Base->GetPathName()));
			}
		}

		// The room manager draws every structure instanced, which a material has to allow, or the
		// default material is drawn in its place. A material of the project's own is given that; one
		// from elsewhere cannot be changed here, so it is reported.
		if (Base && !Base->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes))
		{
			if (Base->GetPackage()->GetName().StartsWith(TEXT("/Game/")))
			{
				Base->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
				Base->MarkPackageDirty();
			}
			else
			{
				Result.Notes.Add(FString::Printf(TEXT("slot %s's material %s may not be drawn instanced, so the room manager draws the default material there; give the slot a material of the project's"),
					*SlotName, *Base->GetPathName()));
			}
		}
	}
}

UPlatformStructureData* UStructureImporter::FindOrCreateStructure(const FString& Folder, const FString& AssetName, bool& bOutCreated) const
{
	using namespace StructureImporterPrivate;

	bOutCreated = false;

	if (UPlatformStructureData* Existing = LoadObject<UPlatformStructureData>(nullptr, *MakeObjectPath(Folder, AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Existing;
	}

	UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
	Factory->DataAssetClass = UPlatformStructureData::StaticClass();

	UPlatformStructureData* Created = Cast<UPlatformStructureData>(
		FAssetToolsModule::GetModule().Get().CreateAsset(AssetName, Folder, UPlatformStructureData::StaticClass(), Factory));

	bOutCreated = Created != nullptr;
	return Created;
}

int32 UStructureImporter::AddToRoomManagers(const TConstArrayView<UPlatformStructureData*> NewStructures, const bool bRebuildAll) const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

	if (!World || (NewStructures.IsEmpty() && !bRebuildAll))
	{
		return 0;
	}

	// One step to undo, however many room managers take them.
	FScopedTransaction Transaction(LOCTEXT("AddStructures", "Add Imported Structures"));
	int32 Changed = 0;

	for (TActorIterator<ARoomManager> It(World); It; ++It)
	{
		ARoomManager* Manager = *It;
		bool bModified = false;

		for (UPlatformStructureData* Structure : NewStructures)
		{
			if (!Manager->Structures.Contains(Structure))
			{
				if (!bModified)
				{
					Manager->Modify();
					bModified = true;
				}

				Manager->Structures.Add(Structure);
			}
		}

		if (bModified || bRebuildAll)
		{
			// Taken in as an edit in the Details panel would be: the room manager rebuilds from its list.
			Manager->PostEditChange();
		}

		Changed += bModified ? 1 : 0;
	}

	return Changed;
}

#undef LOCTEXT_NAMESPACE
