// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// Rendering that needs its own shaders. It loads at PostConfigInit (see WEM_2026.uproject), before
// the engine gathers its shader types, which a module loading at Default is too late for.
public class WEM_2026Rendering : ModuleRules
{
	public WEM_2026Rendering(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });

		PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Engine", "RenderCore", "Renderer", "RHI" });

		// The AI overlay's page in Project Settings (AIOverlaySettings.h).
		PrivateDependencyModuleNames.Add("DeveloperSettings");
	}
}
