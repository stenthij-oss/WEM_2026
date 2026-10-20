// Copyright Epic Games, Inc. All Rights Reserved.

#include "AIBridgeLauncher.h"
#include "AIOverlay.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PortraitFrame.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"
#include "SkyGlitch.h"

class FWEM_2026RenderingModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The project's Shaders folder, as "/WEM_2026/Private/SkyGlitch.usf" and so on.
		AddShaderSourceDirectoryMapping(TEXT("/WEM_2026"), FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders")));

		// View extensions need the engine, which does not exist yet this early.
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]()
		{
			SkyGlitch = FSceneViewExtensions::NewExtension<FSkyGlitchViewExtension>();
			Portrait = FSceneViewExtensions::NewExtension<FPortraitFrameViewExtension>();
			AIOverlayExtension = FSceneViewExtensions::NewExtension<FAIOverlayViewExtension>();

			// A packaged game's bridge, loading while the map does.
			AIBridgeLauncher::Start();
		});
	}

	virtual void ShutdownModule() override
	{
		AIBridgeLauncher::Stop();
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		SkyGlitch.Reset();
		Portrait.Reset();
		AIOverlayExtension.Reset();
	}

private:
	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FSkyGlitchViewExtension, ESPMode::ThreadSafe> SkyGlitch;
	TSharedPtr<FPortraitFrameViewExtension, ESPMode::ThreadSafe> Portrait;
	TSharedPtr<FAIOverlayViewExtension, ESPMode::ThreadSafe> AIOverlayExtension;
};

IMPLEMENT_MODULE(FWEM_2026RenderingModule, WEM_2026Rendering);
