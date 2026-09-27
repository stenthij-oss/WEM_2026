// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

class IPropertyHandle;
struct FPlacementFootprintView;

/**
 * Puts a footprint view on FPlacementGeometry wherever it is shown - in a catalog row's editor
 * most of all - above its fields. In a catalog row, the view shows the geometry the placer will
 * use, the row's overrides laid over what was derived, with its archetype's exclusion zones.
 */
class FPlacementGeometryCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	/** What the handle holds, as the footprint view draws it. False when it holds no single geometry. */
	static bool GetView(const TSharedPtr<IPropertyHandle>& Handle, FPlacementFootprintView& OutView);
};
