// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/GameViewportClient.h"
#include "Subsystems/WorldSubsystem.h"
#include "WEMPlayers.generated.h"

class APlayerController;
class UPrimitiveComponent;

/**
 * How many play at once, as a page in Project Settings (Game > Players), and where the drones start. Saved in
 * Config/DefaultGame.ini; a change takes effect the next time play starts.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Players"))
class WEM_2026_API UWEMPlayersSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/**
	 * How many play at once. With 2 the window shows two portrait screens side by side, evenly spaced: player 1 on the
	 * left with the first pad (and the keyboard), player 2 on the right with the second pad. With 1 the game plays as it
	 * did before, from the level's player start.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Players", meta = (ClampMin = "1", ClampMax = "2"))
	int32 Players = 2;

	/** With more than one player, how far in front of the cube each drone starts: player 1 before one face, player 2 before the opposite one, both facing the cube's middle. */
	UPROPERTY(config, EditAnywhere, Category = "Start", meta = (DisplayName = "Distance from the Cube", ClampMin = "-50", UIMax = "100", Units = "Meters"))
	float StartDistance = 10.0f;

	/** How high up the cube they start: 0 its bottom, 1 its top. */
	UPROPERTY(config, EditAnywhere, Category = "Start", meta = (DisplayName = "Height", ClampMin = "0", ClampMax = "1"))
	float StartHeight = 0.5f;
};

/** Someone looking at the world: a local player's camera, or, with no player at all (Simulate, tests), the view drawn last. */
struct FWEMViewer
{
	FVector Location = FVector::ZeroVector;
	FVector Up = FVector::UpVector;

	/** Null for the view drawn last. */
	TWeakObjectPtr<APlayerController> Player;
};

namespace WEMPlayers
{
	/**
	 * Each player's frame on a viewport ViewportSize big, left to right, as shares of the viewport: Min is where it
	 * starts, Max where it ends. The frames are Ratio wide for every 1 tall (9:16 is 0.5625), as tall as the viewport,
	 * with even gaps before, between and after them. In a viewport too narrow for that they shrink, centred, so the gaps
	 * stay at least MinGapShare of its width.
	 */
	TArray<FBox2D> GetScreenFrames(const FVector2D& ViewportSize, int32 Players, float Ratio);

	/** The narrowest gap between and beside the frames, as a share of the viewport's width. */
	constexpr double MinGapShare = 0.02;

	/**
	 * Where player Index of Players starts: round the cube's middle, evenly, Distance in front of its side - for two,
	 * player 0 before the face towards -X and player 1 before the one towards +X - at Height (0 the cube's bottom, 1
	 * its top), looking level towards the middle.
	 */
	FTransform GetStart(const FBox& Cube, int32 Index, int32 Players, double Distance, double Height);

	/**
	 * Everyone looking at the world: the local players' cameras, the first (left) player first. With no local player at
	 * all, the view drawn last (Simulate's editor view), if any.
	 */
	void GatherViewers(const UWorld* World, TArray<FWEMViewer>& OutViewers);

	/** Local player Player's camera; with no local player at all, the view drawn last for Player 0. False when there is neither. */
	bool GetViewer(const UWorld* World, int32 Player, FWEMViewer& OutViewer);

	/** Which local player a controller is: 0 for the first (left screen), 1 for the second. INDEX_NONE for none. */
	int32 GetPlayerIndex(const APlayerController* Player);

	/**
	 * Draws Component in Viewers[Index]'s view only, hiding it from every other local player, so each player can have a
	 * copy of their own turned to them. A view with no player (Simulate) sees every copy.
	 */
	void ShowOnlyTo(UPrimitiveComponent* Component, const TArray<FWEMViewer>& Viewers, int32 Index);
}

/**
 * Starts the game with UWEMPlayersSettings::Players playing: the players beyond the first are added as play begins, and
 * with more than one, every drone is put in front of the cube, facing it, before anything has ticked (so a drone's trail
 * starts where the drone does). Game and play-in-editor worlds only.
 */
UCLASS()
class WEM_2026_API UWEMPlayersSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	/** The cube the players start round: the level's room manager's, else none. */
	static bool FindCube(const UWorld& World, FBox& OutCube);
};

/**
 * The game's viewport. With more than one player it lays the players out side by side as portrait frames
 * (WEMPlayers::GetScreenFrames, wem.Portrait.Ratio wide), evenly spaced, with black between, and keeps that layout as
 * the window changes size. One player gets the whole viewport, as always.
 *
 * Set as the engine's GameViewportClientClassName in Config/DefaultEngine.ini.
 */
UCLASS()
class WEM_2026_API UWEMGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual void LayoutPlayers() override;
	virtual void Draw(FViewport* InViewport, FCanvas* SceneCanvas) override;

private:
	/** The viewport size and frame ratio the players were last laid out for. */
	FIntPoint LaidOutSize = FIntPoint::ZeroValue;
	float LaidOutRatio = 0.0f;
};
