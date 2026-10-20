// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace AIOverlayPieceTestsPrivate
{
	using namespace WEMGrid;
	using namespace WEMRoomManagerTests;

	/** As AIOverlayStencil in RoomManager.cpp: the value of a piece the AI overlay takes in. */
	constexpr int32 OverlayStencil = 1;

	/** As NegativeFacePieceBit in RoomManager.cpp: a one-sided fill on a platform's negative face. */
	constexpr uint64 NegativeFaceBit = uint64(1) << 63;

	/** What is drawn on each side of the overlay. */
	struct FSides
	{
		int32 FillsIn = 0;
		int32 FillsOut = 0;
		int32 LatticeIn = 0;
		int32 LatticeOut = 0;
		/** The fills taken in, by their platform's key: the segments picked. */
		TSet<uint64> FillKeysIn;
		/** Every beam and post drawn, by its key, and whether it is taken in. */
		TMap<uint64, bool> Lattice;
		bool bEveryChunkInCustomDepth = true;
	};

	FSides CountSides(const ARoomManager& Manager)
	{
		FSides Sides;
		FRoomManagerTestAccess::ForEachDrawnPiece(Manager, [&Sides](const uint64 Key, const UInstancedStaticMeshComponent& Chunk, const bool bFill)
		{
			const bool bIn = Chunk.CustomDepthStencilValue == OverlayStencil;
			(bFill ? (bIn ? Sides.FillsIn : Sides.FillsOut) : (bIn ? Sides.LatticeIn : Sides.LatticeOut))++;
			if (bFill && bIn)
			{
				Sides.FillKeysIn.Add(Key & ~NegativeFaceBit);
			}
			if (!bFill)
			{
				Sides.Lattice.Add(Key & ~FRoomManagerTestAccess::WaterSeamBit, bIn);
			}

			// A piece left alone still hides the pieces behind it from the overlay.
			Sides.bEveryChunkInCustomDepth &= Chunk.bRenderCustomDepth;
		});
		return Sides;
	}

	/**
	 * The beams and posts that should be taken in: every one round the rim of a picked segment, worked out here from the
	 * platforms rather than asked of the room manager.
	 */
	TSet<uint64> GetFramingKeys(const ARoomManager& Manager, const TSet<uint64>& PickedSegments)
	{
		const int32 Module = FRoomManagerTestAccess::GetModule(Manager);
		TSet<uint64> Framing;

		for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(Manager))
		{
			const int32 Normal = static_cast<int32>(Platform.Normal);
			if (!PickedSegments.Contains(MakeTaggedKey(Platform.MinNode, Normal)))
			{
				continue;
			}

			int32 SpanU, SpanV, U, V;
			FRoomManagerTestAccess::GetSpans(Manager, Platform, SpanU, SpanV);
			GetInPlaneAxes(Normal, U, V);

			for (int32 StepV = 0; StepV <= SpanV; StepV += Module)
			{
				for (int32 StepU = 0; StepU <= SpanU; StepU += Module)
				{
					const FIntVector Node = Platform.MinNode + AxisStep(U, StepU) + AxisStep(V, StepV);
					const bool bOnRimAlongU = StepV == 0 || StepV == SpanV;
					const bool bOnRimAlongV = StepU == 0 || StepU == SpanU;

					if (bOnRimAlongU && StepU < SpanU)
					{
						Framing.Add(MakeTaggedKey(Node, U));
					}
					if (bOnRimAlongV && StepV < SpanV)
					{
						Framing.Add(MakeTaggedKey(Node, V));
					}
					if (bOnRimAlongU || bOnRimAlongV)
					{
						Framing.Add(MakeCellKey(Node));
					}
				}
			}
		}

		return Framing;
	}

	/** How many beams and posts are on the wrong side: in without framing a picked segment, or out while framing one. */
	int32 CountWrongLattice(const ARoomManager& Manager, const FSides& Sides)
	{
		const TSet<uint64> Framing = GetFramingKeys(Manager, Sides.FillKeysIn);
		int32 Wrong = 0;
		for (const TPair<uint64, bool>& Piece : Sides.Lattice)
		{
			Wrong += Framing.Contains(Piece.Key) != Piece.Value ? 1 : 0;
		}
		return Wrong;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIOverlaySegmentShareTest, "WEM.AIOverlay.SegmentShare", WEMRoomManagerTests::TestFlags)

bool FAIOverlaySegmentShareTest::RunTest(const FString& Parameters)
{
	using namespace AIOverlayPieceTestsPrivate;

	IConsoleVariable* Share = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.AIOverlay.SegmentPercent"));
	if (!TestNotNull(TEXT("wem.AIOverlay.SegmentPercent exists"), Share))
	{
		return false;
	}
	const FString Before = Share->GetString();
	Share->Set(100.0f, ECVF_SetByConsole);

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->bShowPieces = true;
	Manager->ClearPlatforms();

	for (int32 Placement = 0; Placement < 80; ++Placement)
	{
		Manager->PlaceNextPlatform();
	}

	// At 100, the default, the overlay takes in every piece, as before there was a share.
	FSides All = CountSides(*Manager);
	TestTrue(TEXT("Something grew"), All.FillsIn > 40 && All.LatticeIn > 100);
	TestEqual(TEXT("At 100 no fill is left alone"), All.FillsOut, 0);
	TestEqual(TEXT("At 100 no beam or post is left alone"), All.LatticeOut, 0);
	TestEqual(TEXT("At 100 there is nothing to sort"), FRoomManagerTestAccess::SortPiecesForOverlay(*Manager), 0);

	// At 30, about that share of the segments goes in, with the beams and posts round them, and every piece is still
	// drawn, once.
	Share->Set(30.0f, ECVF_SetByConsole);
	const int32 Moved = FRoomManagerTestAccess::SortPiecesForOverlay(*Manager);
	const FSides Third = CountSides(*Manager);
	const int32 Fills = All.FillsIn;
	const int32 Lattice = All.LatticeIn;
	TestEqual(TEXT("Every fill still drawn"), Third.FillsIn + Third.FillsOut, Fills);
	TestEqual(TEXT("Every beam and post still drawn"), Third.LatticeIn + Third.LatticeOut, Lattice);
	TestEqual(TEXT("What moved is what left"), Moved, Third.FillsOut + Third.LatticeOut);
	const float FillShare = (float)Third.FillsIn / Fills;
	TestTrue(FString::Printf(TEXT("About 30 %% of the segments go in (%.0f %%)"), FillShare * 100.0f), FillShare > 0.15f && FillShare < 0.45f);
	TestTrue(TEXT("Some beams and posts stay out"), Third.LatticeOut > 0);
	TestEqual(TEXT("Exactly the beams and posts round the picked segments go in"), CountWrongLattice(*Manager, Third), 0);
	TestTrue(TEXT("Every chunk still draws into custom depth"), Third.bEveryChunkInCustomDepth);
	TestEqual(TEXT("Sorted once is sorted"), FRoomManagerTestAccess::SortPiecesForOverlay(*Manager), 0);
	TestEqual(TEXT("In step with a full sync"), FRoomManagerTestAccess::SyncPieces(*Manager), 0);

	// Growth at 30 puts each new piece on its side straight away, and a picked segment brings in the beams and posts a
	// neighbour drew before it.
	for (int32 Placement = 0; Placement < 40; ++Placement)
	{
		Manager->PlaceNextPlatform();
	}
	const FSides Grown = CountSides(*Manager);
	TestTrue(TEXT("More grew"), Grown.FillsIn + Grown.FillsOut > Fills);
	TestEqual(TEXT("After growth, exactly the beams and posts round the picked segments are in"), CountWrongLattice(*Manager, Grown), 0);
	TestEqual(TEXT("Grown pieces are already on their side"), FRoomManagerTestAccess::SortPiecesForOverlay(*Manager), 0);

	// Taking picked segments away lets the beams and posts go that nothing picked frames any more.
	TArray<FGridPlatform> Picked;
	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		if (Grown.FillKeysIn.Contains(MakeTaggedKey(Platform.MinNode, static_cast<int32>(Platform.Normal))) && Picked.Num() < 8)
		{
			Picked.Add(Platform);
		}
	}
	FRoomManagerTestAccess::Remove(*Manager, Picked);
	const FSides Shrunk = CountSides(*Manager);
	TestTrue(TEXT("Picked segments were taken away"), Shrunk.FillsIn < Grown.FillsIn);
	TestEqual(TEXT("After removal, exactly the beams and posts round the picked segments are in"), CountWrongLattice(*Manager, Shrunk), 0);
	TestEqual(TEXT("After removal, in step with a full sync"), FRoomManagerTestAccess::SyncPieces(*Manager), 0);

	// A higher share keeps every segment a lower one took in.
	Share->Set(60.0f, ECVF_SetByConsole);
	FRoomManagerTestAccess::SortPiecesForOverlay(*Manager);
	const FSides More = CountSides(*Manager);
	TestTrue(TEXT("60 takes in more than 30"), More.FillsIn > Shrunk.FillsIn);
	TestTrue(TEXT("Including every segment 30 took in"), More.FillKeysIn.Includes(Shrunk.FillKeysIn));
	TestEqual(TEXT("At 60, exactly the beams and posts round the picked segments are in"), CountWrongLattice(*Manager, More), 0);

	// At 0 the overlay takes in nothing.
	Share->Set(0.0f, ECVF_SetByConsole);
	FRoomManagerTestAccess::SortPiecesForOverlay(*Manager);
	const FSides None = CountSides(*Manager);
	TestEqual(TEXT("At 0 no fill goes in"), None.FillsIn, 0);
	TestEqual(TEXT("At 0 no beam or post goes in"), None.LatticeIn, 0);
	TestEqual(TEXT("In step with a full sync at 0"), FRoomManagerTestAccess::SyncPieces(*Manager), 0);

	// And back at 100, every piece again.
	Share->Set(100.0f, ECVF_SetByConsole);
	FRoomManagerTestAccess::SortPiecesForOverlay(*Manager);
	const FSides Back = CountSides(*Manager);
	TestEqual(TEXT("Back at 100 no fill is left alone"), Back.FillsOut, 0);
	TestEqual(TEXT("Back at 100 no beam or post is left alone"), Back.LatticeOut, 0);

	Share->Set(*Before, ECVF_SetByConsole);
	ReleaseRooted(Rooted);
	return true;
}

#endif
