// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PlatformStructureData.h"
#include "PlatformTileData.h"
#include "RoomManager.h"
#include "StructureCategory.h"
#include "UObject/Package.h"

/** What the tests read of the room manager's private state. Befriended by ARoomManager. */
struct FRoomManagerTestAccess
{
	static void EnsureCandidates(ARoomManager& Manager)
	{
		Manager.EnsurePlatformCandidates();
	}

	static bool IsListed(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		for (const ARoomManager::FTileCandidates& Candidates : Manager.TileCandidates)
		{
			if (Candidates.Tile != Platform.Tile)
			{
				continue;
			}

			for (const TArray<FGridPlatform>& ByKind : Candidates.ByKind)
			{
				for (const FGridPlatform& Listed : ByKind)
				{
					if (Listed.MinNode == Platform.MinNode && Listed.Normal == Platform.Normal && Listed.bTurned == Platform.bTurned)
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	static TArray<FGridPlatform> GetListed(const ARoomManager& Manager)
	{
		TArray<FGridPlatform> Listed;
		for (const ARoomManager::FTileCandidates& Candidates : Manager.TileCandidates)
		{
			for (const TArray<FGridPlatform>& ByKind : Candidates.ByKind)
			{
				Listed.Append(ByKind);
			}
		}

		return Listed;
	}

	/** Runs the wholesale check and returns how many checks have drifted so far. */
	static int32 Verify(ARoomManager& Manager)
	{
		Manager.VerifyIncrementalState();
		return Manager.FailedVerifications;
	}

	static int32 GetFailedVerifications(const ARoomManager& Manager)
	{
		return Manager.FailedVerifications;
	}

	static int32 GetVerifiedPlacements(const ARoomManager& Manager)
	{
		return Manager.VerifiedPlacements;
	}

	static bool IsStale(const ARoomManager& Manager)
	{
		return Manager.bPlatformCandidatesStale;
	}

	/** The platforms that stand, in the order they were taken in. */
	static const TArray<FGridPlatform>& GetBuilt(const ARoomManager& Manager)
	{
		return Manager.BuiltPlatforms;
	}

	/** Replaces the platform list and rebuilds everything from it, as loading a level would. */
	static void Rebuild(ARoomManager& Manager, const TArray<FGridPlatform>& Platforms)
	{
		Manager.Platforms = Platforms;
		Manager.RebuildPlatformState();
	}

	/** The faces beside one face of a built platform, as face indices (platform * 2, + 1 for its negative face), sorted. */
	static TArray<int32> GetFaceNeighbours(const ARoomManager& Manager, const int32 PlatformIndex, const int32 FaceSign)
	{
		TArray<int32, TInlineAllocator<16>> Neighbours;
		Manager.GatherFaceNeighbours(Manager.BuiltPlatforms[PlatformIndex], PlatformIndex, FaceSign, Neighbours);

		TArray<int32> Sorted(Neighbours);
		Sorted.Sort();
		return Sorted;
	}

	/** A face's streak in a category, or zero when its structure does not carry it. */
	static int32 GetStreak(const ARoomManager& Manager, const int32 PlatformIndex, const int32 FaceSign, const UStructureCategory* Category)
	{
		const int32* CategoryIndex = Manager.CategoryIndices.Find(Category);
		const ARoomManager::FFaceFill& Fill = Manager.FaceFills[PlatformIndex * 2 + (FaceSign > 0 ? 0 : 1)];
		const int32 Index = CategoryIndex ? Fill.Categories.Find(*CategoryIndex) : INDEX_NONE;

		return Index != INDEX_NONE ? Fill.Streaks[Index] : 0;
	}
};

/** Worlds, tiles, structures and room managers for the room manager's tests, made by hand rather than from assets. */
namespace WEMRoomManagerTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** For checks that take minutes: run by name, never with the rest. */
	constexpr EAutomationTestFlags SlowTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter;

	/** A game world with nothing in it, torn down again when this goes out of scope. */
	struct FScopedTestWorld
	{
		FScopedTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false, TEXT("WEMRoomManagerTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
		}

		~FScopedTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		UWorld* World = nullptr;
	};

	inline void ReleaseRooted(TConstArrayView<UObject*> Objects)
	{
		for (UObject* Object : Objects)
		{
			if (Object)
			{
				Object->RemoveFromRoot();
			}
		}
	}

	/** Rooted, so nothing collects it in the middle of a test; the test lets go of it with ReleaseRooted. */
	inline UPlatformTileData* MakeTile(const int32 Width, const int32 Length, const bool bCanTurn = false)
	{
		UPlatformTileData* Tile = NewObject<UPlatformTileData>(GetTransientPackage());
		Tile->Width = Width;
		Tile->Length = Length;
		Tile->bCanTurn = bCanTurn;
		Tile->AddToRoot();
		return Tile;
	}

	/** A structure on the engine's cube, which the room manager draws but no test looks at. Rooted, like a tile. */
	inline UPlatformStructureData* MakeStructure(
		const TCHAR* Name,
		const int32 Width,
		const int32 Length,
		const EStructureType Types,
		const bool bTwoSided,
		const bool bMayLieSideways = false)
	{
		UPlatformStructureData* Structure = NewObject<UPlatformStructureData>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPlatformStructureData::StaticClass(), Name));
		Structure->Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		Structure->Width = Width;
		Structure->Length = Length;
		Structure->Types = static_cast<int32>(Types);
		Structure->bTwoSided = bTwoSided;
		Structure->bMayLieSideways = bMayLieSideways;
		Structure->AddToRoot();
		return Structure;
	}

	/** One two-sided slab per size, of every type and free to lie sideways: every tile filled, any way it is laid. */
	inline TArray<TObjectPtr<UPlatformStructureData>> MakeSlabs(TConstArrayView<UPlatformTileData*> Tiles, TArray<UObject*>& OutRooted)
	{
		TArray<TObjectPtr<UPlatformStructureData>> Slabs;

		for (const UPlatformTileData* Tile : Tiles)
		{
			UPlatformStructureData* Slab = MakeStructure(TEXT("TestSlab"), Tile->Width, Tile->Length,
				EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall, /*bTwoSided=*/true, /*bMayLieSideways=*/true);
			Slabs.Add(Slab);
			OutRooted.Add(Slab);
		}

		return Slabs;
	}

	/**
	 * A room manager with the designer's tile sizes, a two-sided slab filling each, overlay and
	 * pieces off, on a fixed seed. Structures replaces the slabs when given.
	 */
	inline ARoomManager* SpawnRoomManager(
		UWorld* World,
		TArray<UObject*>& OutRooted,
		const TOptional<TArray<TObjectPtr<UPlatformStructureData>>>& Structures = {})
	{
		// Deferred, so its construction sees the tiles rather than warning that there are none.
		ARoomManager* Manager = World->SpawnActorDeferred<ARoomManager>(ARoomManager::StaticClass(), FTransform::Identity);
		Manager->bShowSurfaces = false;
		Manager->bShowPieces = false;
		Manager->bShowDebugGrid = false;
		Manager->bSnapToLandscapeHeight = false;
		Manager->PlatformRandomSeed = 1234;

		UPlatformTileData* Square = MakeTile(16, 16);
		UPlatformTileData* Small = MakeTile(8, 8);
		UPlatformTileData* Long = MakeTile(8, 16, true);
		OutRooted.Append({ Square, Small, Long });

		Manager->FirstTile = Square;
		Manager->Tiles = { Square, Small, Long };
		Manager->Structures = Structures.IsSet() ? Structures.GetValue() : MakeSlabs({ Square, Small, Long }, OutRooted);
		Manager->FinishSpawning(FTransform::Identity);
		Manager->ClearPlatforms();
		return Manager;
	}
}

#endif
