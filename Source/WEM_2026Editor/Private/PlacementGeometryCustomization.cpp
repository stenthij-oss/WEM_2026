// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementGeometryCustomization.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "PlacementArchetype.h"
#include "PlacementCatalog.h"
#include "PropertyHandle.h"
#include "SPlacementFootprint.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PlacementGeometryCustomization"

TSharedRef<IPropertyTypeCustomization> FPlacementGeometryCustomization::MakeInstance()
{
	return MakeShared<FPlacementGeometryCustomization>();
}

bool FPlacementGeometryCustomization::GetView(const TSharedPtr<IPropertyHandle>& Handle, FPlacementFootprintView& OutView)
{
	if (!Handle.IsValid() || !Handle->IsValidHandle())
	{
		return false;
	}

	TArray<void*> RawData;
	Handle->AccessRawData(RawData);

	// Several rows selected at once have no one footprint to show.
	if (RawData.Num() != 1 || !RawData[0])
	{
		return false;
	}

	const FPlacementGeometry* Geometry = static_cast<const FPlacementGeometry*>(RawData[0]);
	const FProperty* Property = Handle->GetProperty();

	// A catalog row's own geometry is shown as the placer will use it, which takes the row itself:
	// the geometry sits inside the row at the property's offset.
	const bool bInCatalogRow = Property
		&& Property->GetOwnerStruct() == FPlacementCatalogRow::StaticStruct()
		&& Property->GetFName() == GET_MEMBER_NAME_CHECKED(FPlacementCatalogRow, Geometry);

	if (!bInCatalogRow)
	{
		OutView.Geometry = *Geometry;
		return true;
	}

	const FPlacementCatalogRow* Row = reinterpret_cast<const FPlacementCatalogRow*>(
		reinterpret_cast<const uint8*>(Geometry) - Property->GetOffset_ForInternal());

	OutView.Geometry = Row->GetEffectiveGeometry();
	OutView.bOverridesApplied = Row->bOverrideSize || Row->bOverrideSurfaces || Row->bOverrideSpecialEdges;
	OutView.CellSize = Row->BuiltCellSize > 0.0f ? Row->BuiltCellSize : 20.0;

	if (Row->Archetype)
	{
		OutView.Zones = Row->Archetype->ExclusionZones;
	}

	return true;
}

void FPlacementGeometryCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	const TWeakPtr<IPropertyHandle> WeakHandle = PropertyHandle;

	HeaderRow
		.NameContent()
		[
			PropertyHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(250.0f)
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text_Lambda([WeakHandle]()
			{
				FPlacementFootprintView View;

				if (!GetView(WeakHandle.Pin(), View))
				{
					return LOCTEXT("MultipleValues", "Multiple Values");
				}

				const FIntVector& Size = View.Geometry.SizeCells;
				return FText::Format(LOCTEXT("Summary", "{0} x {1} x {2} cells, {3} surface layer(s), {4} special edge(s)"),
					Size.X, Size.Y, Size.Z, View.Geometry.Surfaces.Num(), View.Geometry.SpecialEdges.Num());
			})
		];
}

void FPlacementGeometryCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	const TWeakPtr<IPropertyHandle> WeakHandle = PropertyHandle;

	ChildBuilder.AddCustomRow(LOCTEXT("Footprint", "Footprint"))
		.WholeRowContent()
		[
			SNew(SPlacementFootprint)
			.OnGetView([WeakHandle](FPlacementFootprintView& OutView)
			{
				return GetView(WeakHandle.Pin(), OutView);
			})
		];

	// The fields themselves stay below, as they would be without the view.
	uint32 ChildCount = 0;
	PropertyHandle->GetNumChildren(ChildCount);

	for (uint32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
	{
		ChildBuilder.AddProperty(PropertyHandle->GetChildHandle(ChildIndex).ToSharedRef());
	}
}

#undef LOCTEXT_NAMESPACE
