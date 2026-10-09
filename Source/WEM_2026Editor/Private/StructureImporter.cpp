// Copyright Epic Games, Inc. All Rights Reserved.

#include "StructureImporter.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Engine/StaticMesh.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeManager.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/UObjectToken.h"
#include "PlatformStructureData.h"
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

	/** Every structure category in the project, by asset name. */
	TMap<FString, FAssetData> FindCategories()
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UStructureCategory::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		GetAssetRegistry().GetAssets(Filter, Assets);

		TMap<FString, FAssetData> Categories;
		for (const FAssetData& Asset : Assets)
		{
			Categories.Add(Asset.AssetName.ToString(), Asset);
		}

		return Categories;
	}

	FString MakeObjectPath(const FString& Folder, const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s.%s"), *Folder, *AssetName, *AssetName);
	}

	/**
	 * The engine's own glTF pipelines, as an import would use them, except that everything lands
	 * straight in the destination folder: the defaults put each file's assets in a folder named after
	 * it, and a folder per asset type below that.
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
			Stack->AddPipeline(Assets);
		}

		const UInterchangePipelineBase* DefaultGLTF = LoadObject<UInterchangePipelineBase>(
			nullptr, TEXT("/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline"));

		if (DefaultGLTF)
		{
			Stack->AddPipeline(DuplicateObject(DefaultGLTF, GetTransientPackage()));
		}

		return Stack;
	}
}

UStructureImporter::UStructureImporter()
{
	SourceFolder.Path = TEXT("SourceArt/Structures/Export");
	DestinationFolder.Path = TEXT("/Game/Structures");
}

void UStructureImporter::Import()
{
	const FString SourceDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / SourceFolder.Path);

	TArray<FString> SettingsFiles;
	IFileManager::Get().FindFiles(SettingsFiles, *(SourceDirectory / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);

	// In name order, so a report reads the same from one import to the next.
	SettingsFiles.Sort();

	FMessageLog Log(MessageLogName);
	Log.NewPage(FText::Format(LOCTEXT("PageTitle", "{0}: Import"), FText::FromString(GetName())));

	int32 Imported = 0;
	int32 Failed = 0;

	{
		FScopedSlowTask Progress(SettingsFiles.Num(), FText::Format(LOCTEXT("Importing", "Importing {0} structure(s)"), SettingsFiles.Num()));
		Progress.MakeDialog();

		for (const FString& SettingsFile : SettingsFiles)
		{
			Progress.EnterProgressFrame(1.0f, FText::FromString(SettingsFile));

			TArray<FString> Notes;
			TArray<FString> Errors;

			if (ImportStructure(SourceDirectory / SettingsFile, Notes, Errors))
			{
				++Imported;
			}
			else
			{
				++Failed;
			}

			for (const FString& Note : Notes)
			{
				Log.Warning()->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s: %s"), *SettingsFile, *Note))));
			}

			for (const FString& Error : Errors)
			{
				Log.Error()->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s: %s"), *SettingsFile, *Error))));
			}
		}
	}

	const FString Summary = FString::Printf(TEXT("%d structure(s) imported, %d failed, from %s."), Imported, Failed, *SourceDirectory);
	Log.Info()->AddToken(FUObjectToken::Create(this))->AddToken(FTextToken::Create(FText::FromString(Summary)));
	Log.Open(Failed > 0 ? EMessageSeverity::Error : EMessageSeverity::Info, /*bForce=*/true);

	UE_LOG(LogTemp, Log, TEXT("StructureImporter: %s See the Structure Import message log."), *Summary);
}

bool UStructureImporter::ImportStructure(const FString& SettingsPath, TArray<FString>& OutNotes, TArray<FString>& OutErrors)
{
	using namespace StructureImporterPrivate;

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *SettingsPath))
	{
		OutErrors.Add(TEXT("the settings file could not be read"));
		return false;
	}

	FStructureSettings Settings;
	FString Error;

	if (!WEMStructureImport::ParseSettings(Json, Settings, Error))
	{
		OutErrors.Add(FString::Printf(TEXT("the settings file is refused: %s"), *Error));
		return false;
	}

	const FString MeshFile = FPaths::GetPath(SettingsPath) / Settings.Name + TEXT(".glb");

	if (!FPaths::FileExists(MeshFile))
	{
		OutErrors.Add(FString::Printf(TEXT("there is no %s.glb beside it"), *Settings.Name));
		return false;
	}

	// The size is never taken on trust: it is measured from the bounds again, in this importer's cells.
	int32 MeasuredWidth = 0;
	int32 MeasuredLength = 0;

	if (!WEMStructureImport::MeasureCells((Settings.BoundsMax.X - Settings.BoundsMin.X) * 100.0, CellSize, MeasuredWidth)
		|| !WEMStructureImport::MeasureCells((Settings.BoundsMax.Y - Settings.BoundsMin.Y) * 100.0, CellSize, MeasuredLength)
		|| MeasuredWidth != Settings.Width
		|| MeasuredLength != Settings.Length)
	{
		OutErrors.Add(FString::Printf(TEXT("its bounds are not the opening of a %d x %d structure in %g cm cells"), Settings.Width, Settings.Length, CellSize));
		return false;
	}

	const FString Folder = DestinationFolder.Path / FString::Printf(TEXT("%dx%d"), Settings.Width, Settings.Length);
	const FString MeshName = TEXT("SM_") + Settings.Name;

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
		OutErrors.Add(FString::Printf(TEXT("%s.glb did not import as a static mesh"), *Settings.Name));
		return false;
	}

	// Measured from the mesh as it came in, in its own frame, against what the add-on measured in
	// Blender's: the check that the axes came across the way the room manager lays them.
	const FBox Expected = WEMStructureImport::GetExpectedMeshBounds(Settings);
	const FBox Measured = Mesh->GetMeshDescription(0)->ComputeBoundingBox();

	if (!WEMStructureImport::BoundsMatch(Expected, Measured))
	{
		OutErrors.Add(FString::Printf(TEXT("the imported mesh's bounds %s do not match the settings file's %s"), *Measured.ToString(), *Expected.ToString()));
		return false;
	}

	bool bCreated = false;
	UPlatformStructureData* Structure = FindOrCreateStructure(Folder, TEXT("DA_Structure_") + Settings.Name, bCreated);

	if (!Structure)
	{
		OutErrors.Add(TEXT("its structure asset could not be made"));
		return false;
	}

	TArray<FString> UnknownTypes;
	const int32 Types = WEMStructureImport::MakeTypeMask(Settings.Types, UnknownTypes);

	for (const FString& Type : UnknownTypes)
	{
		OutNotes.Add(FString::Printf(TEXT("type '%s' is not Ground, Ceiling or Wall, and is ignored"), *Type));
	}

	const TMap<FString, FAssetData> KnownCategories = FindCategories();
	TArray<TObjectPtr<UStructureCategory>> Categories;

	for (const FString& CategoryName : Settings.Categories)
	{
		const FAssetData* Asset = KnownCategories.Find(CategoryName);
		UStructureCategory* Category = Asset ? Cast<UStructureCategory>(Asset->GetAsset()) : nullptr;

		if (!Category)
		{
			OutNotes.Add(FString::Printf(TEXT("there is no category asset named '%s'"), *CategoryName));
			continue;
		}

		Categories.AddUnique(Category);
	}

	if (Categories.IsEmpty())
	{
		OutNotes.Add(TEXT("it has no category, so it takes no part in the category pull"));
	}

	if (Settings.bFence && !Settings.bTwoSided)
	{
		OutNotes.Add(TEXT("it is a fence but not two-sided, so growth leaves it out"));
	}

	Structure->Modify();
	Structure->Mesh = Mesh;
	Structure->Width = Settings.Width;
	Structure->Length = Settings.Length;
	Structure->bTwoSided = Settings.bTwoSided;
	Structure->Categories = Categories;
	Structure->Types = Types;
	Structure->bLoadBearing = Settings.bLoadBearing;
	Structure->bHoldsItems = Settings.bHoldsItems;
	Structure->bFence = Settings.bFence;
	Structure->bMayLieSideways = Settings.bMayLieSideways;

	// Weights are tuned in Unreal while watching growth, so the file only gives a new structure its first.
	if (bCreated)
	{
		Structure->Weight = Settings.Weight;
	}

	Structure->MarkPackageDirty();
	return true;
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

#undef LOCTEXT_NAMESPACE
