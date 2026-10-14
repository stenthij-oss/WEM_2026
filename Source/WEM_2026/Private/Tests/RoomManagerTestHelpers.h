// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/InstancedStaticMeshComponent.h"
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

	/** Places a platform as it is, fill and all, the way a beat does once it has drawn one - fence ends included. */
	static void Place(ARoomManager& Manager, const FGridPlatform& Platform)
	{
		Manager.PlacePlatform(Platform, 0);
	}

	/** How many fence ends wait to grow. */
	static int32 GetWaitingFenceEnds(const ARoomManager& Manager)
	{
		return Manager.FenceQueue.Num();
	}

	/** The Up a placement would take if it were drawn now. Draws from the role stream, as a beat would. */
	static EGridFace ChooseUp(ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.ChooseUp(Platform);
	}

	/** Fills a platform as a beat would, from the structure stream. False when nothing fits. */
	static bool DrawFill(ARoomManager& Manager, FGridPlatform& Platform)
	{
		return Manager.DrawFill(Platform, /*bLoadBearingOnly=*/false);
	}

	/** Which faces of a wall about to be placed look over an interior ground it stands on: 1 positive, 2 negative. */
	static uint8 GetFacesOverInteriorGround(const ARoomManager& Manager, const FGridPlatform& Wall)
	{
		return Manager.GetFacesOverInteriorGround(Wall);
	}

	/** The axis a fence panel's run goes along. */
	static int32 GetRunAxis(const ARoomManager& Manager, const FGridPlatform& Panel)
	{
		return Manager.GetRunAxis(Panel);
	}

	/** Where a run could carry on from one end of a panel - straight on, and round each corner. */
	static void GatherFenceContinuations(const ARoomManager& Manager, const FGridPlatform& Panel, const int32 Side, FGridPlatform& OutStraight, TArray<FGridPlatform>& OutTurns)
	{
		TArray<FGridPlatform, TInlineAllocator<2>> Turns;
		Manager.GatherFenceContinuations(ARoomManager::FFenceEnd{ Panel, Side }, OutStraight, Turns);
		OutTurns = Turns;
	}

	/** The chance a drawn placement is drawn again for going against a growth rule. */
	static float GetRuleRedrawChance(const ARoomManager& Manager, const FGridPlatform& Candidate)
	{
		return Manager.GetRuleRedrawChance(Candidate);
	}

	/** Whether a floor runs along a wall's edge across its Up at Coordinate, on the side Side of its plane. */
	static bool HasFloorAlong(const ARoomManager& Manager, const FGridPlatform& Wall, const int32 Coordinate, const int32 Side)
	{
		return Manager.HasFloorAlong(Wall, Coordinate, Side);
	}

	/** Whether a platform stands on something, as a fence beat asks it. */
	static bool StandsOnSomething(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		return Manager.StandsOnSomething(Platform);
	}

	/** Fills a platform with nature of one form on the face NatureSign looks out from, and urban behind it, from the structure stream. */
	static bool DrawNatureFill(ARoomManager& Manager, FGridPlatform& Platform, const ENatureForm Form, const int32 NatureSign)
	{
		return Manager.DrawNatureFill(Platform, Form, NatureSign, Manager.StructureStream);
	}

	/** Whether every face a filled platform about to be placed would meet is one the nature on either side takes. */
	static bool AreNatureMeetingsLegal(const ARoomManager& Manager, const FGridPlatform& Platform, bool& bOutNatureMet)
	{
		uint8 NatureMeets = 0;
		const bool bLegal = Manager.AreNatureMeetingsLegal(Platform, &NatureMeets);
		bOutNatureMet = NatureMeets != 0;
		return bLegal;
	}

	/** Whether a placement not yet filled would meet water or a cliff, and which of its faces would meet ground (1 positive, 2 negative). */
	static bool MeetsClosedNature(const ARoomManager& Manager, const FGridPlatform& Platform, uint8& OutFacesOnGround)
	{
		return Manager.MeetsClosedNature(Platform, &OutFacesOnGround);
	}

	static int32 GetUsableNatureCount(const ARoomManager& Manager)
	{
		return Manager.UsableNatureStructures.Num();
	}

	/** How many fields of nature are still spilling out, and how many pieces the oldest may still lay. */
	static int32 GetSpillingFields(const ARoomManager& Manager, int32& OutOldestRemaining)
	{
		OutOldestRemaining = Manager.SpillQueue.IsEmpty() ? 0 : Manager.SpillQueue[0].Remaining;
		return Manager.SpillQueue.Num();
	}

	/** How many pairs of faces among the platforms that stand meet where the nature on either side does not take the other. */
	static int32 CountIllegalNatureMeetings(const ARoomManager& Manager)
	{
		int32 Illegal = 0;

		for (int32 Index = 0; Index < Manager.BuiltPlatforms.Num(); ++Index)
		{
			const FGridPlatform& Platform = Manager.BuiltPlatforms[Index];

			for (const int32 OwnSign : { 1, -1 })
			{
				TArray<int32, TInlineAllocator<16>> Neighbours;
				Manager.GatherFaceNeighbours(Platform, Index, OwnSign, Neighbours);

				// Face indices run platform * 2, + 1 for its negative face.
				for (const int32 Face : Neighbours)
				{
					Illegal += ARoomManager::AcceptsMeeting(Platform, OwnSign, Manager.BuiltPlatforms[Face / 2], Face % 2 == 0 ? 1 : -1) ? 0 : 1;
				}
			}
		}

		return Illegal;
	}

	/** The instances a structure's layer holds, and the mesh and material on its first chunk. Zero when it has no layer. */
	static int32 GetStructureLayerPieces(const ARoomManager& Manager, const UPlatformStructureData* Structure, UStaticMesh*& OutMesh, UMaterialInterface*& OutMaterial)
	{
		OutMesh = nullptr;
		OutMaterial = nullptr;

		for (const FStructureLayer& Layer : Manager.StructureLayers)
		{
			if (Layer.Structure != Structure || Layer.Pieces.Chunks.IsEmpty())
			{
				continue;
			}

			int32 Pieces = 0;
			for (const UInstancedStaticMeshComponent* Chunk : Layer.Pieces.Chunks)
			{
				Pieces += Chunk ? Chunk->GetInstanceCount() : 0;
			}

			OutMesh = Layer.Pieces.Chunks[0]->GetStaticMesh();
			OutMaterial = Layer.Pieces.Chunks[0]->GetMaterial(0);
			return Pieces;
		}

		return 0;
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

	/** Nature of one form with no mesh, drawn as a slab of its flat colour. Only ground holds items. Rooted, like a tile. */
	inline UPlatformStructureData* MakeNature(const TCHAR* Name, const ENatureForm Form)
	{
		UPlatformStructureData* Nature = NewObject<UPlatformStructureData>(
			GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPlatformStructureData::StaticClass(), Name));
		Nature->Section = EStructureSection::Nature;
		Nature->NatureForm = Form;
		Nature->bHoldsItems = Form == ENatureForm::Ground;
		Nature->AddToRoot();
		return Nature;
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
