// Copyright Epic Games, Inc. All Rights Reserved.

#include "MessageLogInitializationOptions.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "PlacementCatalogBuilder.h"
#include "PlacementGeometryCustomization.h"
#include "PlacementTypes.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "WEM_2026Editor"

/** Editor-only tooling for WEM_2026: the placement catalog builder and the views of its data. */
class FWEM_2026EditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The catalog builder reports into a log page of its own, so a build's warnings can be read
		// through and clicked back to their meshes after the fact.
		FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");

		FMessageLogInitializationOptions Options;
		Options.bShowPages = true;
		Options.bAllowClear = true;
		MessageLogModule.RegisterLogListing(UPlacementCatalogBuilder::MessageLogName, LOCTEXT("PlacementCatalogLog", "Placement Catalog"), Options);

		// A footprint view wherever a mesh's placement geometry is shown.
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.RegisterCustomPropertyTypeLayout(
			FPlacementGeometry::StaticStruct()->GetFName(),
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FPlacementGeometryCustomization::MakeInstance));
		PropertyModule.NotifyCustomizationModuleChanged();
	}

	virtual void ShutdownModule() override
	{
		if (FModuleManager::Get().IsModuleLoaded("MessageLog"))
		{
			FMessageLogModule& MessageLogModule = FModuleManager::GetModuleChecked<FMessageLogModule>("MessageLog");
			MessageLogModule.UnregisterLogListing(UPlacementCatalogBuilder::MessageLogName);
		}

		if (FModuleManager::Get().IsModuleLoaded("PropertyEditor") && UObjectInitialized())
		{
			FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.UnregisterCustomPropertyTypeLayout(FPlacementGeometry::StaticStruct()->GetFName());
		}
	}
};

IMPLEMENT_MODULE(FWEM_2026EditorModule, WEM_2026Editor);

#undef LOCTEXT_NAMESPACE
