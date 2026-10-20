// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WEM_2026 : ModuleRules
{
	public WEM_2026(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "GameplayTags" });

		// The drone trail draws its own mesh (DroneTrailMeshComponent.cpp).
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI" });


		// The Players page in Project Settings (WEMPlayers.h).
		PublicDependencyModuleNames.Add("DeveloperSettings");
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
