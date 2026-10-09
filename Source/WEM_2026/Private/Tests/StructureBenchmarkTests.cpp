// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace StructureBenchmarkTestsPrivate
{
	using namespace WEMRoomManagerTests;

	/**
	 * Grows a room manager set up as NewMap's - a 509-cell cube, 16 x 16 tiles first, 8 x 8 laid flat
	 * only, 8 x 16 that does not turn, pieces built, the overlay off - to Target platforms, and returns
	 * the mean time of the last Measured beats in milliseconds.
	 */
	double MeasureBeats(UWorld* World, TArray<UObject*>& Rooted, const TOptional<TArray<TObjectPtr<UPlatformStructureData>>>& Structures, const int32 Target, const int32 Measured, int32& OutPlatforms)
	{
		ARoomManager* Manager = SpawnRoomManager(World, Rooted, Structures);
		Manager->Tiles[1]->Orientation = ETileOrientation::HorizontalOnly;
		Manager->Tiles[2]->bCanTurn = false;
		Manager->GridSize = 509;
		Manager->PlatformRandomSeed = 4242;
		Manager->bShowPieces = true;
		Manager->ClearPlatforms();

		double MeasuredSeconds = 0.0;
		int32 MeasuredBeats = 0;

		while (Manager->GetPlatformCount() < Target)
		{
			const bool bMeasure = Manager->GetPlatformCount() >= Target - Measured;
			const double Start = FPlatformTime::Seconds();

			if (!Manager->PlaceNextPlatform())
			{
				break;
			}

			if (bMeasure)
			{
				MeasuredSeconds += FPlatformTime::Seconds() - Start;
				++MeasuredBeats;
			}
		}

		OutPlatforms = Manager->GetPlatformCount();
		Manager->Destroy();
		return MeasuredBeats > 0 ? 1000.0 * MeasuredSeconds / MeasuredBeats : 0.0;
	}

	/** The test kit's structures, as the importer made them. */
	TArray<TObjectPtr<UPlatformStructureData>> LoadTestKit()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.WaitForCompletion();

		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game/Structures/TestKit"));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UPlatformStructureData::StaticClass()->GetClassPathName());

		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });

		TArray<TObjectPtr<UPlatformStructureData>> Structures;
		for (const FAssetData& Asset : Assets)
		{
			if (UPlatformStructureData* Structure = Cast<UPlatformStructureData>(Asset.GetAsset()))
			{
				Structures.Add(Structure);
			}
		}

		return Structures;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureBenchmarkBeatTest, "WEM.Structures.Perf.Beat", WEMRoomManagerTests::SlowTestFlags)

bool FStructureBenchmarkBeatTest::RunTest(const FString& Parameters)
{
	using namespace StructureBenchmarkTestsPrivate;

	// The time a beat takes once about 3000 platforms stand, filled from a plain slab per size, and
	// from the whole test kit - categories, one-sided pairs, fences and structures that are not
	// load-bearing among them. Each the best of three runs.
	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	const TArray<TObjectPtr<UPlatformStructureData>> TestKit = LoadTestKit();
	TestTrue(TEXT("The test kit is there"), TestKit.Num() > 60);

	for (const bool bTestKit : { false, true })
	{
		double Best = TNumericLimits<double>::Max();
		int32 Platforms = 0;

		// Unset, the room manager is given a plain slab for each of its own tiles.
		TOptional<TArray<TObjectPtr<UPlatformStructureData>>> Fills;
		if (bTestKit)
		{
			Fills = TestKit;
		}

		for (int32 Run = 0; Run < 3; ++Run)
		{
			Best = FMath::Min(Best, MeasureBeats(TestWorld.World, Rooted, Fills, 3000, 1000, Platforms));
		}

		AddInfo(FString::Printf(TEXT("%s: %.3f ms a beat over the last 1000 beats to %d platforms."), bTestKit ? TEXT("Test kit") : TEXT("Plain slabs"), Best, Platforms));
	}

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureBenchmarkPatchTest, "WEM.Structures.Perf.CategoryPatches", WEMRoomManagerTests::SlowTestFlags)

bool FStructureBenchmarkPatchTest::RunTest(const FString& Parameters)
{
	using namespace StructureBenchmarkTestsPrivate;

	// CategoryPatchSize with the test kit at 3000 platforms, set up as NewMap is, for the default pull
	// and a weaker and a stronger one, over several seeds.
	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	const TArray<TObjectPtr<UPlatformStructureData>> TestKit = LoadTestKit();

	struct FSetting
	{
		const TCHAR* Name;
		float Continue;
		float MaxContinue;
	};

	const FSetting Settings[] = { { TEXT("No pull (0 / 0)"), 0.0f, 0.0f }, { TEXT("Weak (0.15 / 0.35)"), 0.15f, 0.35f }, { TEXT("Defaults (0.35 / 0.70)"), 0.35f, 0.70f }, { TEXT("Strong (0.50 / 0.90)"), 0.50f, 0.90f } };

	for (const FSetting& Setting : Settings)
	{
		TArray<double> Sizes;

		for (int32 Seed = 1; Seed <= 4; ++Seed)
		{
			ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, TestKit);
			Manager->Tiles[1]->Orientation = ETileOrientation::HorizontalOnly;
			Manager->Tiles[2]->bCanTurn = false;
			Manager->GridSize = 509;
			Manager->PlatformRandomSeed = Seed;
			Manager->ContinueChance = Setting.Continue;
			Manager->MaxContinueChance = Setting.MaxContinue;
			Manager->ClearPlatforms();

			while (Manager->GetPlatformCount() < 3000 && Manager->PlaceNextPlatform())
			{
			}

			Sizes.Add(Manager->CategoryPatchSize);
			Manager->Destroy();
		}

		Sizes.Sort();
		AddInfo(FString::Printf(TEXT("%s: CategoryPatchSize %s (median %.1f) at 3000 platforms."), Setting.Name,
			*FString::JoinBy(Sizes, TEXT(", "), [](const double Size) { return FString::Printf(TEXT("%.1f"), Size); }), 0.5 * (Sizes[1] + Sizes[2])));
	}

	ReleaseRooted(Rooted);
	return true;
}

#endif
