// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/Actor.h"
#include "Fonts/SlateFontInfo.h"
#include "Math/RandomStream.h"
#include "OrbSnippets.h"
#include "OrbSpot.h"
#include "PlacementSolver.h"
#include "PlacementWorld.h"
#include "ObjectPlacer.generated.h"

class AOrb;
class ARoomManager;
class SOrbHud;
class APlayerController;
class UDataTable;
class UEnhancedInputComponent;
class UFont;
class UInputAction;
class ULocalPlayer;
class ULineBatchComponent;
class UPlacedObjectVisuals;
class UTexture2D;
struct FBatchedLine;

/**
 * Furnishes the structure the room manager grows: on a beat of its own, it draws an object from
 * its catalogs and stands it wherever that object's archetype allows - on any face of any
 * segment, since every face is a floor to whatever stands on it, or on top of another object.
 *
 * Each object it places is held against growth as an obstacle, so segments grow around furniture
 * and never through it. When the room manager starts over from a new platform list, the objects
 * go with the old structure.
 *
 * The objects are runtime state and never saved. What goes where is worked out by
 * FPlacementSolver from the catalogs alone, on a seeded stream, so the same seeds for the room
 * manager and the placer replay a run exactly, however quickly the meshes load. With no catalog
 * set, no furniture is placed.
 *
 * Orbs
 * ----
 * It also stands an orb in each of the room manager's zones, for the player to catch: over a
 * horizontal segment of the zone drawn at random, near its middle, wherever the surface there
 * carries anything and the air above it is open (WEMOrbSpot). Should the segment be taken away -
 * struck down, cut loose or reshuffled - or something be built into the orb's air, the orb stands
 * again at once on another segment of the zone.
 *
 * Near enough, the catch input (A) catches the orb: it fades, a snippet of text is written out on
 * screen, the count in the top right goes up, and the segment it hovered over is given
 * CatchHealthBoost health. The zone then rests for ZoneCooldownSeconds before its next orb. The
 * snippets come from plain text files (WEMOrbSnippets), each shown once before any comes round
 * again. Once OrbsToCatch have been caught, the game stops.
 *
 * With more than one player (UWEMPlayersSettings), each catches with their own pad, the orb nearest
 * their own drone. An orb one player catches is gone for the other. Each has a count of their own
 * in their own frame, and the snippet shows only on the catcher's screen. The game stops once one
 * player has caught OrbsToCatch; it is not a race, and nothing marks who got there.
 */
UCLASS()
class WEM_2026_API AObjectPlacer : public AActor
{
	GENERATED_BODY()

public:
	AObjectPlacer();

	/** The grid to furnish. Left empty, the level's one room manager is found. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TObjectPtr<ARoomManager> RoomManager;

	/** Catalog tables built by the placement catalog builder, drawn from together. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (RequiredAssetDataTags = "RowStructure=/Script/WEM_2026.PlacementCatalogRow"))
	TArray<TObjectPtr<UDataTable>> Catalogs;

	/** Seconds between placements. One object, with its companions, per beat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0"))
	float PlacementInterval = 3.0f;

	/** Whether the beat runs on its own once play begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	bool bAutoPlace = true;

	/** Seed for the placement stream. Zero draws a fresh seed each run. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	int32 RandomSeed = 0;

	/** How many kinds a beat tries before giving up, when the ones it draws have nowhere to go. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "1"))
	int32 MaxAttemptsPerBeat = 4;

	/** Objects standing. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Placement")
	int32 PlacedObjectCount = 0;

	/** How long the last beat took to work out where things go, in milliseconds, placed or not. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Placement")
	float LastBeatMilliseconds = 0.0f;

	/** The slowest beat since the last clear. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Placement")
	float SlowestBeatMilliseconds = 0.0f;

	/** Prints a block per placement to the output log: what, where, why, what each edge touches, and a map. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement|Debug")
	bool bLogPlacements = true;

	/**
	 * Draws each object's footprint, edges, special runs, contacts, zones, surfaces and up in the
	 * world. Back edges black, front yellow, left and right blue, zones red, surfaces green.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement|Debug")
	bool bDrawDebug = true;

	/** Also draws the lattice pieces each object holds back from growth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement|Debug")
	bool bDrawObstacles = false;

	/** Loads each mesh as it is placed rather than in the background. Placement is the same either way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement|Debug")
	bool bLoadMeshesSynchronously = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement|Debug", meta = (ClampMin = "0.1"))
	float DebugLineThickness = 1.0f;

	/** Stands an orb in every zone for the player to catch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs")
	bool bPlaceOrbs = true;

	/** What stands in each zone: AOrb, or a Blueprint made from it to change its look. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs")
	TSubclassOf<AOrb> OrbClass;

	/** How high the middle of an orb hovers over its segment, in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs", meta = (ClampMin = "0.0"))
	float OrbHoverHeight = 130.0f;

	/** How many cells either way from a segment's middle an orb may stand, when there is no room right at it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs", meta = (ClampMin = "0"))
	int32 OrbMaxShift = 4;

	/** Seconds a zone rests once its orb is caught, before another stands there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs", meta = (ClampMin = "0.0"))
	float ZoneCooldownSeconds = 60.0f;

	/** Health given to the segment an orb is caught over. Segments are laid with MinHealth to MaxHealth, 80 to 120. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs", meta = (ClampMin = "0.0"))
	float CatchHealthBoost = 300.0f;

	/** How many orbs there are to catch. Once all are - by one player, when more than one plays - the game stops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs", meta = (ClampMin = "1"))
	int32 OrbsToCatch = 140;

	/** The input that catches the nearest orb near enough: A on the gamepad, space on the keyboard. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs")
	TSoftObjectPtr<UInputAction> CatchAction;

	/**
	 * The folder the snippets are written in, under the project's Content folder. Every .txt file in it is read when play
	 * starts; a paragraph is a snippet, and a line starting with # is never shown.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text")
	FString SnippetFolder = TEXT("Orbs/Snippets");

	/** The typeface for the snippets and the count. Left empty, the Garamond that comes with Windows is used, if it is there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text")
	TObjectPtr<UFont> TextFont;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text")
	FLinearColor TextColor = FLinearColor(0.93f, 0.78f, 0.16f);

	/** A letter's height, as a share of the frame's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.005", ClampMax = "0.2"))
	float TextHeight = 0.03f;

	/** Room after every letter, as a share of a letter's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "-0.5", ClampMax = "2.0"))
	float LetterSpacing = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.5"))
	float LettersPerSecond = 10.0f;

	/** Seconds each letter takes to fade in as it is written. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.0"))
	float LetterFadeSeconds = 0.3f;

	/** The longest a snippet takes to be written out, in seconds: a longer one is written faster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "1.0"))
	float LongestWriteSeconds = 15.0f;

	/** How long a snippet stays once written out: this, and HoldSecondsPerLetter more for every letter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.0"))
	float HoldSeconds = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.0"))
	float HoldSecondsPerLetter = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.0"))
	float TextFadeOutSeconds = 1.5f;

	/** How far each line leans, its end higher than its start, in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "-45.0", ClampMax = "45.0"))
	float TextTilt = 3.0f;

	/** Where the first line starts, as shares of the frame's width and height from its top left. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text")
	FVector2D TextPosition = FVector2D(0.28, 0.54);

	/** How wide the lines may run, as a share of the frame's width. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float TextWidth = 0.36f;

	/** The same two in the portrait frame (P), which is too narrow for the wide frame's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text")
	FVector2D PortraitTextPosition = FVector2D(0.1, 0.56);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float PortraitTextWidth = 0.8f;

	/**
	 * How high a long snippet may start and how low it may end, as shares of the frame's height from the top. One that
	 * would run past TextBottom starts higher, as far up as TextTop, and only then is written smaller.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TextTop = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TextBottom = 0.94f;

	/** The smallest a long snippet's letters get, as a share of TextHeight. One too long even then runs past TextBottom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float SmallestTextScale = 0.6f;

	/** The count's height, as a share of the frame's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text", meta = (ClampMin = "0.005", ClampMax = "0.2"))
	float CounterHeight = 0.032f;

	/** The orb beside the count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orbs|Text")
	TObjectPtr<UTexture2D> CounterIcon;

	/** Orbs caught this run, by everyone together. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Orbs")
	int32 OrbsCaught = 0;

	/** Orbs caught this run by each player, the first (left screen) first. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Orbs")
	TArray<int32> OrbsCaughtByPlayer;

	/** Orbs standing, zones waiting for room or resting, orbs that had to move, and snippets. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Orbs")
	FString OrbSummary;

	/** Places one object, with its companions. False when nothing fits anywhere just now. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Placement")
	bool PlaceNextObject();

	/** Takes every object down, gives their space back to growth, and re-seeds the stream. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Placement")
	void ClearObjects();

	/**
	 * Catches the orb nearest local player Player (0 the first, on the left) if it is near enough, as their catch input
	 * does. With no player (Simulate), player 0 catches from the view drawn last. False when no orb is near enough.
	 */
	UFUNCTION(BlueprintCallable, Category = "Orbs")
	bool CatchNearestOrb(int32 Player = 0);

	/** The orb standing in a zone, or null while the zone waits for room or rests. */
	AOrb* GetZoneOrb(int32 Zone) const;

	/** Null until the placer has a room manager and a catalog to work with. */
	const FPlacementSolver* GetSolver() const
	{
		return Solver.Get();
	}

	/** Every placement's log block since the last clear, in order. */
	const TArray<FString>& GetPlacementLog() const
	{
		return PlacementLog;
	}

	/** Objects still waiting for their mesh to load. */
	int32 CountObjectsWaitingForMeshes() const;

	//~ Begin AActor Interface
	virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End AActor Interface

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

private:
	/** Lets the orb tests drive the orbs without a game running. */
	friend struct FObjectPlacerTestAccess;

	/**
	 * Finds the room manager and resolves the catalogs, the first time they are needed and after a
	 * clear. False, with the reason logged once, when there is nothing to place or nothing to place
	 * it on. Drops every object first if the room manager has started over since the last call.
	 */
	bool EnsureReady();

	/** The beat. Unlike the room manager's, it never stops itself: the structure keeps growing room. */
	void AdvancePlacement();

	/** Finds the level's one room manager when none is set. False when there is none. */
	bool FindRoomManager();

	/** No orbs, none caught, a fresh stream, and the snippets read from their folder again. */
	void ResetOrbs();

	/**
	 * The orbs' frame: an orb whose segment has gone, or whose air has been built into, stands again elsewhere in its
	 * zone at once, and a zone whose rest is over gets one.
	 */
	void AdvanceOrbs();

	/** Stands an orb in each zone waiting for one, over a horizontal segment of the zone drawn at random. */
	void StandOrbs();

	/** Takes every orb away, at once and without a rest for its zone. */
	void DropOrbs();

	/** Binds each local player's catch input, as they join. */
	void BindCatch();
	void UnbindCatch();

	/** Player's catch input pressed. */
	void HandleCatch(int32 Player);

	/** Puts the count and the snippets over each player's frame, once there is a player and a view to put them on. */
	void ShowOrbHud();
	void HideOrbHud();

	/** The HUD's look, from the properties above, so a change to them shows at once. */
	void UpdateOrbHudStyle();

	void UpdateOrbSummary();

	void ResetPlacementStream();

	/** Turns a placed object into a mesh, an obstacle, a log block and debug lines. */
	void ShowPlaced(int32 Id);

	void RedrawDebug();
	void AppendDebugLines(const FPlacedObject& Object, TArray<FBatchedLine>& OutLines) const;

	/** The chunk the next object's debug lines go in, starting a fresh one when the last is full. */
	ULineBatchComponent* GetOpenDebugLineChunk();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Placement", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> SceneRoot;

	/**
	 * An actor-owned batcher rather than global persistent debug lines, for the room manager's
	 * reason: flushing the world's persistent lines to redraw these would wipe every other system's.
	 *
	 * The first of the chunks the debug lines are drawn in. Adding lines to a batcher sends all of
	 * its lines to the renderer again, so each chunk holds only so many objects' lines, and an object
	 * placed costs one chunk's worth rather than every object's.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Placement", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<ULineBatchComponent> DebugLines;

	/** The chunks after the first, made as they are needed. Never saved or copied with the placer. */
	UPROPERTY(Transient, DuplicateTransient)
	TArray<TObjectPtr<ULineBatchComponent>> ExtraDebugLineChunks;

	/** Objects drawn in the last chunk: the last extra one, or DebugLines while there is none. */
	int32 ObjectsInOpenDebugLineChunk = 0;

	UPROPERTY(Transient)
	TObjectPtr<UPlacedObjectVisuals> Visuals;

	FPlacementCatalogData CatalogData;
	TUniquePtr<FRoomManagerPlacementWorld> PlacementWorld;
	TUniquePtr<FPlacementSolver> Solver;
	FRandomStream PlacementStream;

	/** Per placed object, the room manager's handle for its obstacle and the visuals' handle for its mesh. */
	TMap<int32, int32> ObstacleHandles;
	TMap<int32, int32> VisualHandles;

	TArray<FString> PlacementLog;

	/** The room manager's generation the objects stand on. */
	int32 SeenRebuildGeneration = INDEX_NONE;

	/** What EnsureReady last complained of, so a beat does not say it again every interval. */
	FString ReportedProblem;

	FTimerHandle PlacementTimerHandle;

	/** A zone's orb: the orb, the segment and the spot it stands over, and until when the zone rests once its orb is caught. */
	struct FZoneOrb
	{
		TWeakObjectPtr<AOrb> Orb;
		FGridPlatform Platform;
		FOrbSpot Spot;
		double RestUntil = 0.0;
	};

	TArray<FZoneOrb> ZoneOrbs;
	FOrbSnippetBag Snippets;
	FRandomStream OrbStream;

	/** Each local player's count and snippets, and the player it is over. */
	TArray<TSharedPtr<SOrbHud>> OrbHuds;
	TArray<TWeakObjectPtr<ULocalPlayer>> OrbHudPlayers;

	/** The input each local player catches with, pushed onto their controller. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UEnhancedInputComponent>> CatchInputs;

	TArray<TWeakObjectPtr<APlayerController>> CatchInputPlayers;

	/** The room manager's generation the orbs stand on, as for the objects. */
	int32 OrbRebuildGeneration = INDEX_NONE;

	/** The zone whose orb's air is looked at next, a few zones a frame. */
	int32 NextOrbAirCheck = 0;

	/** When the zones waiting for room next look for it. At once when an orb has just had to move. */
	double NextOrbSearchAt = 0.0;

	int32 OrbsMoved = 0;

	/** The catch action failed to load; tried once a run, as it will not load on the next frame either. */
	bool bCatchActionMissing = false;
	bool bOrbsFinished = false;

	/** The typeface the HUD was last given, so it is only made again when TextFont changes. */
	TWeakObjectPtr<UFont> HudFontSource;
	bool bHudFontMade = false;
	FSlateFontInfo HudFont;
};
