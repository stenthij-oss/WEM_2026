// Copyright Epic Games, Inc. All Rights Reserved.

#include "HAL/IConsoleManager.h"
#include "ISettingsModule.h"
#include "MessageLogInitializationOptions.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "PlacementCatalogBuilder.h"
#include "PlacementGeometryCustomization.h"
#include "PlacementTypes.h"
#include "PropertyEditorModule.h"
#include "StructureImporter.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "WEM_2026Editor"

/**
 * Editor-only tooling for WEM_2026: the placement catalog builder and the views of its data, the structure importer,
 * and an AI Overlay button on the level editor's toolbar.
 */
class FWEM_2026EditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FWEM_2026EditorModule::RegisterMenus));

		// The catalog builder reports into a log page of its own, so a build's warnings can be read
		// through and clicked back to their meshes after the fact.
		FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");

		FMessageLogInitializationOptions Options;
		Options.bShowPages = true;
		Options.bAllowClear = true;
		MessageLogModule.RegisterLogListing(UPlacementCatalogBuilder::MessageLogName, LOCTEXT("PlacementCatalogLog", "Placement Catalog"), Options);

		// And so does the structure importer.
		MessageLogModule.RegisterLogListing(UStructureImporter::MessageLogName, LOCTEXT("StructureImportLog", "Structure Import"), Options);

		// A footprint view wherever a mesh's placement geometry is shown.
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.RegisterCustomPropertyTypeLayout(
			FPlacementGeometry::StaticStruct()->GetFName(),
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FPlacementGeometryCustomization::MakeInstance));
		PropertyModule.NotifyCustomizationModuleChanged();
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);

		if (FModuleManager::Get().IsModuleLoaded("MessageLog"))
		{
			FMessageLogModule& MessageLogModule = FModuleManager::GetModuleChecked<FMessageLogModule>("MessageLog");
			MessageLogModule.UnregisterLogListing(UPlacementCatalogBuilder::MessageLogName);
			MessageLogModule.UnregisterLogListing(UStructureImporter::MessageLogName);
		}

		if (FModuleManager::Get().IsModuleLoaded("PropertyEditor") && UObjectInitialized())
		{
			FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.UnregisterCustomPropertyTypeLayout(FPlacementGeometry::StaticStruct()->GetFName());
		}
	}

private:
	/** The AI overlay's toolbar button: its settings page (UAIOverlaySettings in WEM_2026Rendering), and a cycle on demand. */
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
		FToolMenuSection& Section = Toolbar->FindOrAddSection("WEM_2026");

		FToolMenuEntry Entry = FToolMenuEntry::InitComboButton(
			"AIOverlay",
			FUIAction(),
			FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
			{
				FToolMenuSection& MenuSection = Menu->AddSection("AIOverlay");
				MenuSection.AddMenuEntry(
					"AIOverlaySettings",
					LOCTEXT("AIOverlaySettings", "Settings..."),
					LOCTEXT("AIOverlaySettingsTooltip", "When the AI comes in, for how long, the prompt and how strongly it repaints the pieces. Changes apply at once, also during play."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
					FUIAction(FExecuteAction::CreateLambda([]()
					{
						FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Game", "AIOverlaySettings");
					})));
				MenuSection.AddMenuEntry(
					"AIOverlayStart",
					LOCTEXT("AIOverlayStart", "Start a Cycle Now"),
					LOCTEXT("AIOverlayStartTooltip", "Ends the current gap, so the AI comes in now (during play, with the bridge running)."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Play"),
					FUIAction(FExecuteAction::CreateLambda([]()
					{
						IConsoleManager::Get().ProcessUserConsoleInput(TEXT("wem.AIOverlay.Start"), *GLog, nullptr);
					})));
			}),
			LOCTEXT("AIOverlay", "AI Overlay"),
			LOCTEXT("AIOverlayTooltip", "The AI overlay's settings, and a cycle on demand"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"));
		Entry.StyleNameOverride = "CalloutToolbar";
		Section.AddEntry(Entry);
	}
};

IMPLEMENT_MODULE(FWEM_2026EditorModule, WEM_2026Editor);

#undef LOCTEXT_NAMESPACE
