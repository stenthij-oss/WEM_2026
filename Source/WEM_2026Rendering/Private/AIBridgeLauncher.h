// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Starts the AI overlay's bridge (Tools/AIOverlay/run_bridge.bat) together with a packaged game, so starting the game is
 * all it takes, and stops it again when the game quits. The settings are under Bridge on the AI Overlay page.
 *
 * The bridge runs without a window, and what it prints goes to AIBridge.log next to the game's own log. When a bridge
 * already runs (started by hand, or by another game), that one is used and left running at the end. The editor, and
 * games run on the editor's binaries, leave the bridge to be started by hand unless given -AIBridge; -NoAIBridge keeps
 * a packaged game from starting it.
 */
namespace AIBridgeLauncher
{
	/** Starts the bridge, unless one runs already, or the settings or the command line say not to. */
	void Start();

	/** Stops the bridge Start started, if it did. Windows also stops it when the game ends any other way. */
	void Stop();
}
