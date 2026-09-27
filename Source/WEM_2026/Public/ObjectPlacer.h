// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "PlacementSolver.h"
#include "PlacementWorld.h"
#include "ObjectPlacer.generated.h"

class ARoomManager;
class UDataTable;
class ULineBatchComponent;
class UPlacedObjectVisuals;
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
 * manager and the placer replay a run exactly, however quickly the meshes load.
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

	/** Places one object, with its companions. False when nothing fits anywhere just now. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Placement")
	bool PlaceNextObject();

	/** Takes every object down, gives their space back to growth, and re-seeds the stream. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Placement")
	void ClearObjects();

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
	/**
	 * Finds the room manager and resolves the catalogs, the first time they are needed and after a
	 * clear. False, with the reason logged once, when there is nothing to place or nothing to place
	 * it on. Drops every object first if the room manager has started over since the last call.
	 */
	bool EnsureReady();

	/** The beat. Unlike the room manager's, it never stops itself: the structure keeps growing room. */
	void AdvancePlacement();

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
};
