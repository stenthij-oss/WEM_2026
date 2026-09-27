// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WEM_2026Editor : ModuleRules
{
	public WEM_2026Editor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "GameplayTags", "WEM_2026" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"InputCore",
			"MeshDescription",
			"MessageLog",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"StaticMeshDescription",
			"UnrealEd"
		});
	}
}
