// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HAL/IConsoleManager.h"

#include "AIOverlaySettings.generated.h"

/** What the overlay draws (wem.AIOverlay.Show). */
UENUM()
enum class EAIOverlayShow : uint8
{
	AIPicture UMETA(DisplayName = "The AI's Picture"),
	SentPicture UMETA(DisplayName = "What the AI Is Sent: the Picture"),
	SentDepth UMETA(DisplayName = "What the AI Is Sent: the Depth Map"),
};

/** Whose screen the AI comes to with more than one player (wem.AIOverlay.Player). */
UENUM()
enum class EAIOverlayPlayer : uint8
{
	TakeTurns UMETA(DisplayName = "Take Turns"),
	Player1 UMETA(DisplayName = "Player 1 (Left)"),
	Player2 UMETA(DisplayName = "Player 2 (Right)"),
};

/**
 * The AI overlay's settings, as a page in Project Settings (Game > AI Overlay), so they can be tuned without typing
 * console commands. Each one drives one of the wem.AIOverlay console variables (AIOverlay.cpp): the page's values are
 * applied at startup, in the editor and in a game alike, and again the moment one is changed, also during play. They
 * are saved in Config/DefaultGame.ini. A console variable typed by hand still works, until the page sets it again.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "AI Overlay"))
class UAIOverlaySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Lets the AI take over the platform pieces every so often. Without the bridge running, nothing changes anyway. (wem.AIOverlay) */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (DisplayName = "On"))
	bool bEnabled = true;

	/** The shortest time the picture stays normal between two cycles. Each gap is picked at random between this and the longest. (wem.AIOverlay.GapMin) */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (DisplayName = "Shortest Gap", ClampMin = "0", UIMax = "120", Units = "s"))
	float GapMin = 20.0f;

	/** The longest time the picture stays normal between two cycles. (wem.AIOverlay.GapMax) */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (DisplayName = "Longest Gap", ClampMin = "0", UIMax = "120", Units = "s"))
	float GapMax = 30.0f;

	/** How long the AI takes to come in fully. (wem.AIOverlay.RampUp) */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (DisplayName = "Ramp Up", ClampMin = "0", UIMax = "30", Units = "s"))
	float RampUp = 8.0f;

	/** How long it stays fully in. (wem.AIOverlay.Hold) */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (ClampMin = "0", UIMax = "60", Units = "s"))
	float Hold = 5.0f;

	/** How long it takes to go again. (wem.AIOverlay.RampDown) */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (DisplayName = "Ramp Down", ClampMin = "0", UIMax = "30", Units = "s"))
	float RampDown = 8.0f;

	/** Keeps the AI in: it ramps in once, with no gap first, and stays at full. Turned off, it holds, ramps out, and the gaps start again. The bridge then works all the time, so the game loses its share of frames all the time too. (wem.AIOverlay.Constant) */
	UPROPERTY(config, EditAnywhere, Category = "Timing")
	bool bConstant = false;

	/** With two players, whose screen the AI comes to: the players take turns, a cycle each, starting with player 1, or it always comes to one of them. One screen at a time either way, so the bridge works no harder than for one player. Kept in (Constant), turns still pass from one to the other, with no gap between. With one player it is always theirs. (wem.AIOverlay.Player) */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (DisplayName = "Whose Screen"))
	EAIOverlayPlayer Player = EAIOverlayPlayer::TakeTurns;

	/** What the AI turns the pieces into. Empty keeps the prompt the bridge was started with (its --prompt). (wem.AIOverlay.Prompt) */
	UPROPERTY(config, EditAnywhere, Category = "Look", meta = (MultiLine = "true"))
	FString Prompt = TEXT("weathered prefabricated concrete panel building, East German Plattenbau, photograph, overcast light");

	/** How far the AI may move from the pieces at full: 0 hands them back as they are, 1 lets it paint whatever the prompt says. It ramps with the cycle. (wem.AIOverlay.Strength) */
	UPROPERTY(config, EditAnywhere, Category = "Look", meta = (ClampMin = "0", ClampMax = "1"))
	float Strength = 0.6f;

	/** How much of the AI's picture covers the pieces at full. 1 covers them completely. (wem.AIOverlay.Opacity) */
	UPROPERTY(config, EditAnywhere, Category = "Look", meta = (ClampMin = "0", ClampMax = "1"))
	float Opacity = 1.0f;

	/** The AI's picture comes back a few frames late. On cuts it to where the pieces were when it was sent, so it does not smear past them while the camera moves; off lets it trail. (wem.AIOverlay.ClipToMask) */
	UPROPERTY(config, EditAnywhere, Category = "Look", meta = (DisplayName = "Clip to the Pieces"))
	bool bClipToMask = true;

	/** What share of the segments the AI takes over; the others stay as they are. Each segment is picked by itself, the fill on both its faces together, and comes with the beams and posts round its rim, which also frame its neighbours. A higher share keeps every segment a lower one took in. Changed during play, every piece that changes sides is drawn again, which takes a moment. (wem.AIOverlay.SegmentPercent) */
	UPROPERTY(config, EditAnywhere, Category = "Look", meta = (DisplayName = "Segments Taken Over", ClampMin = "0", ClampMax = "100", Units = "Percent"))
	float SegmentPercent = 100.0f;

	/** Sends the pieces' depth with each picture when the bridge's AI takes a depth map (its depth ControlNet), so the AI keeps their shape and what is in front of what, even at a strength that repaints their colours completely. (wem.AIOverlay.Depth) */
	UPROPERTY(config, EditAnywhere, Category = "Depth", meta = (DisplayName = "On"))
	bool bDepth = true;

	/** How strongly the depth map steers the AI. 0 leaves it to the picture alone; above 1 holds the shape harder still. (wem.AIOverlay.DepthWeight) */
	UPROPERTY(config, EditAnywhere, Category = "Depth", meta = (DisplayName = "Weight", ClampMin = "0", UIMax = "2"))
	float DepthWeight = 0.7f;

	/** Fits the depth map to the view: the nearest pieces on screen are white and the furthest black, as in the depth maps the AI learned from, and the range follows the camera over a fraction of a second. Off, it runs from Near to Far. (wem.AIOverlay.DepthFit) */
	UPROPERTY(config, EditAnywhere, Category = "Depth", meta = (DisplayName = "Fit to the View"))
	bool bDepthFit = true;

	/** Pieces this near the camera, or nearer, are white in the depth map. Fitted to the view, the nearest the range goes. (wem.AIOverlay.DepthNear) */
	UPROPERTY(config, EditAnywhere, Category = "Depth", meta = (DisplayName = "Near", ClampMin = "0.01", UIMax = "10", Units = "Meters"))
	float DepthNear = 0.5f;

	/** Pieces this far away, or further, are black in the depth map, like the background. In between, even steps of grey are even ratios of distance. Fitted to the view, the furthest the range goes. (wem.AIOverlay.DepthFar) */
	UPROPERTY(config, EditAnywhere, Category = "Depth", meta = (DisplayName = "Far", ClampMin = "1", UIMax = "500", Units = "Meters"))
	float DepthFar = 200.0f;

	/** What the overlay draws: the AI's picture over the pieces, or, to check what the AI works from, the picture or the depth map it is sent, over the whole view. Those two draw all the time, also between cycles and without the bridge. Not saved: back to the AI's picture after a restart. (wem.AIOverlay.Show) */
	UPROPERTY(Transient, EditAnywhere, Category = "Look")
	EAIOverlayShow Show = EAIOverlayShow::AIPicture;

	/** At most this many pictures a second for the AI to make. 0 lets it make one from every frame it can, so it follows the game's frame rate as closely as the GPU they share allows. 15 is how it ran until 2026-10-20: the game keeps more of the GPU, and the AI changes in steps. (wem.AIOverlay.MaxRate) */
	UPROPERTY(config, EditAnywhere, Category = "Smoothness", meta = (DisplayName = "Most Pictures a Second", ClampMin = "0", UIMax = "60"))
	float MaxRate = 0.0f;

	/** Lays each of the AI's pictures where the pieces it shows are now, every frame: a picture comes back a few frames late, and is moved, by the pieces' depth, from where the camera was when it was sent to where it is. Off, it lies where it was on screen, so it trails (or, clipped to the pieces, is cut) while the camera moves. (wem.AIOverlay.Reproject) */
	UPROPERTY(config, EditAnywhere, Category = "Smoothness", meta = (DisplayName = "Keep on the Pieces"))
	bool bReproject = true;

	/** Fades each new picture of the AI's in over the one before, over about the time between two of them (at most a fifth of a second), so the pieces change smoothly rather than in steps. Off, it switches at once. (wem.AIOverlay.Blend) */
	UPROPERTY(config, EditAnywhere, Category = "Smoothness", meta = (DisplayName = "Blend Between Pictures"))
	bool bBlend = true;

	/** Experimental. The side, in pixels, of the pictures the AI works at, 384 to 1024; 0 keeps the bridge's own (512 unless it was started with another). Smaller is faster and coarser: 384 makes about a fifth more pictures a second than 512. Changed, the bridge loads its model again at the new size, which pauses the AI for a few seconds. (wem.AIOverlay.Size) */
	UPROPERTY(config, EditAnywhere, Category = "Experimental", meta = (DisplayName = "Picture Size", ClampMin = "0", ClampMax = "1024", Delta = "64"))
	int32 Size = 0;

	/** Experimental. Holds the game's frame rate near the AI's pictures a second while it trades pictures with the bridge, where about 85% of the frames get a picture of their own: the game leaves the AI the GPU time it would have spent on frames the AI could not keep up with, and the two settle where the GPU is full. Expect the game to run slower and steadier. The frame rate is let go when the cycle ends. (wem.AIOverlay.MatchFrameRate) */
	UPROPERTY(config, EditAnywhere, Category = "Experimental", meta = (DisplayName = "Hold the Game to the AI's Rate"))
	bool bMatchFrameRate = false;

	/** A packaged game starts the bridge (Tools/AIOverlay/run_bridge.bat) itself, without a window, and stops it when it quits, so the game is all there is to start. The AI comes in once the bridge has loaded, about a minute after the game starts. When a bridge already runs, the game uses that one and leaves it running. The editor always leaves the bridge to be started by hand. What the bridge prints goes to AIBridge.log, next to the game's own log (AIBridgeLauncher.h, Structures/BUILD.md). */
	UPROPERTY(config, EditAnywhere, Category = "Bridge", meta = (DisplayName = "Start With a Packaged Game"))
	bool bStartBridge = true;

	/** The options a packaged game starts the bridge with, as for run_bridge.bat: for example --max-rate 10, --no-depth, or --prompt "...". Empty uses the bridge's own defaults. */
	UPROPERTY(config, EditAnywhere, Category = "Bridge", meta = (DisplayName = "Options"))
	FString BridgeOptions;

	/** The project folder with the bridge in it (the one holding Tools/AIOverlay and StreamDiffusion), for a game packaged elsewhere. Empty: the game looks in the folders above its own, which finds this project for a game packaged into it. */
	UPROPERTY(config, EditAnywhere, Category = "Bridge", meta = (DisplayName = "Project Folder"))
	FDirectoryPath BridgeFolder;

	/** Sets the console variables to these values, or with a property's name only that one's. */
	void Apply(EConsoleVariableFlags SetBy, FName OnlyProperty = NAME_None) const;

	virtual void PostInitProperties() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
