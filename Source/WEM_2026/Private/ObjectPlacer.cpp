// Copyright Epic Games, Inc. All Rights Reserved.

#include "ObjectPlacer.h"

#include "Components/LineBatchComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PlacedObjectVisuals.h"
#include "PlacementArchetype.h"
#include "RoomManager.h"
#include "TimerManager.h"

namespace ObjectPlacerPrivate
{
	/** How far the debug lines stand off the surface they mark, so they do not z-fight it. */
	constexpr double DebugBias = 0.6;

	/**
	 * Most objects whose debug lines one chunk holds before a fresh chunk is started. An object runs
	 * to a hundred lines or more, and every line in a chunk is sent to the renderer again whenever
	 * the chunk grows.
	 */
	constexpr int32 DebugObjectsPerChunk = 64;

	const FLinearColor FootprintColour(0.75f, 0.75f, 0.75f);
	const FLinearColor BackColour(0.01f, 0.01f, 0.01f);
	const FLinearColor FrontColour(1.0f, 0.85f, 0.0f);
	const FLinearColor SideColour(0.1f, 0.35f, 1.0f);
	const FLinearColor ContactColour(1.0f, 0.15f, 1.0f);
	const FLinearColor SpecialColour(1.0f, 0.45f, 0.0f);
	const FLinearColor ZoneColour(1.0f, 0.05f, 0.05f);
	const FLinearColor SurfaceColour(0.1f, 0.9f, 0.1f);
	const FLinearColor UpColour(1.0f, 1.0f, 1.0f);
	const FLinearColor ObstacleColour(0.9f, 0.4f, 0.05f);

	FLinearColor GetSideColour(const EObjectSide Side)
	{
		switch (Side)
		{
		case EObjectSide::Back: return BackColour;
		case EObjectSide::Front: return FrontColour;
		default: return SideColour;
		}
	}

	void AddLine(TArray<FBatchedLine>& Lines, const FVector& Start, const FVector& End, const FLinearColor& Colour, const float Thickness)
	{
		Lines.Emplace(Start, End, Colour, /*LifeTime=*/0.0f, Thickness, SDPG_World);
	}

	/** A square of half-size Half around Centre, across the two directions given. */
	void AddSquare(TArray<FBatchedLine>& Lines, const FVector& Centre, const FVector& AxisA, const FVector& AxisB, const double Half, const FLinearColor& Colour, const float Thickness)
	{
		const FVector Corners[4] =
		{
			Centre + (-AxisA - AxisB) * Half,
			Centre + (AxisA - AxisB) * Half,
			Centre + (AxisA + AxisB) * Half,
			Centre + (-AxisA + AxisB) * Half
		};

		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			AddLine(Lines, Corners[Corner], Corners[(Corner + 1) % 4], Colour, Thickness);
		}
	}

	void AddBox(TArray<FBatchedLine>& Lines, const FBox& Box, const FLinearColor& Colour, const float Thickness)
	{
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector From((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z);

			for (const int32 Bit : { 1, 2, 4 })
			{
				if ((Corner & Bit) == 0)
				{
					const int32 Other = Corner | Bit;
					const FVector To((Other & 1) ? Box.Max.X : Box.Min.X, (Other & 2) ? Box.Max.Y : Box.Min.Y, (Other & 4) ? Box.Max.Z : Box.Min.Z);
					AddLine(Lines, From, To, Colour, Thickness);
				}
			}
		}
	}

	/**
	 * A number written in line segments, the way a seven-segment display writes it, lying in the
	 * plane of Along and Up with its baseline centred on Centre. There is no text in a line batcher,
	 * and a text component per special edge is far more than a debug view is worth.
	 */
	void AddNumber(TArray<FBatchedLine>& Lines, const int32 Number, const FVector& Centre, const FVector& Along, const FVector& Up, const double Height, const FLinearColor& Colour, const float Thickness)
	{
		// Segments a to g, one bit each, per digit.
		static const uint8 DigitSegments[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

		const FString Digits = FString::FromInt(FMath::Abs(Number));
		const double Width = 0.5 * Height;
		const double Pitch = 1.6 * Width;
		const FVector Start = Centre - Along * (0.5 * (Digits.Len() * Pitch - (Pitch - Width)));

		for (int32 Index = 0; Index < Digits.Len(); ++Index)
		{
			const uint8 Segments = DigitSegments[Digits[Index] - TEXT('0')];
			const FVector Origin = Start + Along * (Index * Pitch);

			auto Point = [&](const double X, const double Y)
			{
				return Origin + Along * (X * Width) + Up * (Y * Height);
			};

			const FVector Ends[7][2] =
			{
				{ Point(0, 1), Point(1, 1) },
				{ Point(1, 1), Point(1, 0.5) },
				{ Point(1, 0.5), Point(1, 0) },
				{ Point(0, 0), Point(1, 0) },
				{ Point(0, 0), Point(0, 0.5) },
				{ Point(0, 0.5), Point(0, 1) },
				{ Point(0, 0.5), Point(1, 0.5) }
			};

			for (int32 Segment = 0; Segment < 7; ++Segment)
			{
				if (Segments & (1 << Segment))
				{
					AddLine(Lines, Ends[Segment][0], Ends[Segment][1], Colour, Thickness);
				}
			}
		}
	}
}

AObjectPlacer::AObjectPlacer()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	DebugLines = CreateDefaultSubobject<ULineBatchComponent>(TEXT("DebugLines"));
	DebugLines->SetupAttachment(SceneRoot);

	// Its tick only ages lines drawn to expire, and these never do; it would walk every line each frame.
	DebugLines->PrimaryComponentTick.bCanEverTick = false;
}

void AObjectPlacer::BeginPlay()
{
	Super::BeginPlay();

	// Objects are never saved, so a run always starts from none, on a freshly seeded stream.
	ClearObjects();

	if (!EnsureReady())
	{
		return;
	}

	if (bAutoPlace && PlacementInterval > 0.0f)
	{
		FTimerManagerTimerParameters BeatParameters;
		BeatParameters.bLoop = true;
		BeatParameters.FirstDelay = PlacementInterval;

		// A beat that falls behind is dropped, not made up, as the room manager's is.
		BeatParameters.bMaxOncePerFrame = true;

		GetWorldTimerManager().SetTimer(PlacementTimerHandle, this, &AObjectPlacer::AdvancePlacement, PlacementInterval, BeatParameters);
	}
}

void AObjectPlacer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PlacementTimerHandle);
	}

	// Nothing still loading is wanted any more.
	if (Visuals)
	{
		Visuals->Reset();
	}

	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void AObjectPlacer::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName Property = PropertyChangedEvent.GetMemberPropertyName();

	if (Property == GET_MEMBER_NAME_CHECKED(AObjectPlacer, bDrawDebug)
		|| Property == GET_MEMBER_NAME_CHECKED(AObjectPlacer, bDrawObstacles)
		|| Property == GET_MEMBER_NAME_CHECKED(AObjectPlacer, DebugLineThickness))
	{
		RedrawDebug();
	}
}
#endif

void AObjectPlacer::ResetPlacementStream()
{
	// A zero seed means "different every run"; any other value replays the same placements.
	PlacementStream.Initialize(RandomSeed != 0 ? RandomSeed : FMath::Rand());
}

bool AObjectPlacer::EnsureReady()
{
	// The room manager started over: whatever stood on the old structure goes with it. Its
	// obstacles have already gone with its old platform list.
	if (Solver && RoomManager && RoomManager->GetRebuildGeneration() != SeenRebuildGeneration)
	{
		ClearObjects();
	}

	if (Solver && RoomManager)
	{
		return true;
	}

	auto Report = [this](const FString& Problem)
	{
		if (Problem != ReportedProblem)
		{
			ReportedProblem = Problem;
			UE_LOG(LogTemp, Error, TEXT("ObjectPlacer: %s"), *Problem);
		}

		return false;
	};

	if (!RoomManager)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ARoomManager> It(World); It; ++It)
			{
				RoomManager = *It;
				break;
			}
		}
	}

	if (!RoomManager)
	{
		return Report(TEXT("there is no room manager in the level to place objects on."));
	}

	TArray<const UDataTable*> Tables;
	for (const UDataTable* Table : Catalogs)
	{
		if (Table)
		{
			Tables.Add(Table);
		}
	}

	FString Error;
	TArray<FString> Warnings;

	if (!CatalogData.AddTables(Tables, RoomManager->CellSize, Error, Warnings))
	{
		return Report(Error);
	}

	for (const FString& Warning : Warnings)
	{
		UE_LOG(LogTemp, Warning, TEXT("ObjectPlacer: %s"), *Warning);
	}

	if (CatalogData.Entries.IsEmpty())
	{
		return Report(TEXT("the catalogs hold no enabled row to place."));
	}

	ReportedProblem.Reset();

	PlacementWorld = MakeUnique<FRoomManagerPlacementWorld>(RoomManager);
	Solver = MakeUnique<FPlacementSolver>(*PlacementWorld, CatalogData, PlacementStream);
	SeenRebuildGeneration = RoomManager->GetRebuildGeneration();

	if (!Visuals)
	{
		Visuals = NewObject<UPlacedObjectVisuals>(this, NAME_None, RF_Transient);
	}

	return true;
}

void AObjectPlacer::AdvancePlacement()
{
	PlaceNextObject();
}

bool AObjectPlacer::PlaceNextObject()
{
	if (!EnsureReady())
	{
		return false;
	}

	TArray<int32> Placed;

	const double Start = FPlatformTime::Seconds();
	const bool bPlaced = Solver->PlaceNext(MaxAttemptsPerBeat, Placed);

	LastBeatMilliseconds = static_cast<float>((FPlatformTime::Seconds() - Start) * 1000.0);
	SlowestBeatMilliseconds = FMath::Max(SlowestBeatMilliseconds, LastBeatMilliseconds);

	for (const int32 Id : Placed)
	{
		ShowPlaced(Id);
	}

	if (bLogPlacements && bPlaced)
	{
		UE_LOG(LogTemp, Log, TEXT("ObjectPlacer: worked out in %.1f ms, %.1f of it refreshing the surfaces (slowest so far %.1f ms), %d object(s) standing."),
			LastBeatMilliseconds, Solver->GetLastRefreshMilliseconds(), SlowestBeatMilliseconds, PlacedObjectCount);
	}

	return bPlaced;
}

void AObjectPlacer::ClearObjects()
{
	if (RoomManager)
	{
		for (const TPair<int32, int32>& Obstacle : ObstacleHandles)
		{
			RoomManager->RemoveObstacle(Obstacle.Value);
		}
	}

	if (Visuals)
	{
		Visuals->Reset();
	}

	// The catalogs are read again on the next placement, so a catalog built since is picked up.
	Solver.Reset();
	PlacementWorld.Reset();
	CatalogData.Reset();

	ObstacleHandles.Reset();
	VisualHandles.Reset();
	PlacementLog.Reset();
	PlacedObjectCount = 0;
	LastBeatMilliseconds = 0.0f;
	SlowestBeatMilliseconds = 0.0f;
	SeenRebuildGeneration = INDEX_NONE;

	ResetPlacementStream();
	RedrawDebug();
}

int32 AObjectPlacer::CountObjectsWaitingForMeshes() const
{
	return Visuals ? Visuals->CountWaiting() : 0;
}

void AObjectPlacer::ShowPlaced(const int32 Id)
{
	const FPlacedObject* Object = Solver->FindObject(Id);

	if (!Object)
	{
		return;
	}

	// Held against growth from the moment it is placed, whether or not its mesh has loaded yet.
	ObstacleHandles.Add(Id, RoomManager->AddObstacle(Object->VolumeMin, Object->VolumeMax));

	const FPlacementEntry& Entry = CatalogData.Entries[Object->Entry];
	VisualHandles.Add(Id, Visuals->Add(this, SceneRoot, Entry.Mesh, Object->Transform, bLoadMeshesSynchronously));

	PlacementLog.Add(Object->Description);
	PlacedObjectCount = Solver->GetObjects().Num();

	if (bLogPlacements)
	{
		UE_LOG(LogTemp, Log, TEXT("%s"), *Object->Description);
	}

	if (bDrawDebug && DebugLines)
	{
		TArray<FBatchedLine> Lines;
		AppendDebugLines(*Object, Lines);
		GetOpenDebugLineChunk()->DrawLines(Lines);
	}
}

void AObjectPlacer::RedrawDebug()
{
	if (!DebugLines)
	{
		return;
	}

	DebugLines->Flush();

	for (ULineBatchComponent* Chunk : ExtraDebugLineChunks)
	{
		if (IsValid(Chunk))
		{
			Chunk->DestroyComponent();
		}
	}

	ExtraDebugLineChunks.Reset();
	ObjectsInOpenDebugLineChunk = 0;

	if (!bDrawDebug || !Solver)
	{
		return;
	}

	TArray<FBatchedLine> Lines;
	for (const FPlacedObject& Object : Solver->GetObjects())
	{
		Lines.Reset();
		AppendDebugLines(Object, Lines);
		GetOpenDebugLineChunk()->DrawLines(Lines);
	}
}

ULineBatchComponent* AObjectPlacer::GetOpenDebugLineChunk()
{
	if (ObjectsInOpenDebugLineChunk >= ObjectPlacerPrivate::DebugObjectsPerChunk)
	{
		// Made at runtime and never saved, and never copied with the placer.
		ULineBatchComponent* Chunk = NewObject<ULineBatchComponent>(
			this,
			MakeUniqueObjectName(this, ULineBatchComponent::StaticClass(), DebugLines->GetFName()),
			RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);

		Chunk->PrimaryComponentTick.bCanEverTick = false;
		Chunk->SetupAttachment(SceneRoot);
		Chunk->RegisterComponent();

		ExtraDebugLineChunks.Add(Chunk);
		ObjectsInOpenDebugLineChunk = 0;
	}

	++ObjectsInOpenDebugLineChunk;
	return ExtraDebugLineChunks.IsEmpty() ? DebugLines.Get() : ExtraDebugLineChunks.Last().Get();
}

void AObjectPlacer::AppendDebugLines(const FPlacedObject& Object, TArray<FBatchedLine>& OutLines) const
{
	using namespace ObjectPlacerPrivate;
	using namespace WEMPlacement;

	const FSupportPlane* Plane = Solver ? Solver->FindPlane(Object.Pose.Plane) : nullptr;

	if (!Plane || !RoomManager)
	{
		return;
	}

	const FPlacementSignature& Signature = CatalogData.Signatures[Object.Signature];
	const FIntVector& Size = Signature.SizeCells;
	const double CellSize = RoomManager->CellSize;
	const float Thickness = DebugLineThickness;

	const FVector Forward(Object.Frame.Forward);
	const FVector Right(Object.Frame.Right);
	const FVector Up(Object.Frame.Up);

	auto CellCentre = [&](const FIntPoint& LocalCell)
	{
		return Solver->GetPlaneCellCentre(*Plane, Object.Footprint[GetFootprintIndex(Size, LocalCell.X, LocalCell.Y)]) + Up * DebugBias;
	};

	// The footprint's outline on its support.
	const FVector Corner = CellCentre(FIntPoint(0, 0)) - (Forward + Right) * (0.5 * CellSize);
	const FVector Depth = Forward * (Size.X * CellSize);
	const FVector Width = Right * (Size.Y * CellSize);

	AddLine(OutLines, Corner, Corner + Depth, FootprintColour, Thickness);
	AddLine(OutLines, Corner + Depth, Corner + Depth + Width, FootprintColour, Thickness);
	AddLine(OutLines, Corner + Depth + Width, Corner + Width, FootprintColour, Thickness);
	AddLine(OutLines, Corner + Width, Corner, FootprintColour, Thickness);

	// Its edge faces, one bar per face, the ends of each run drawn faded.
	auto GetFaceMiddle = [&](const FEdgeFace& Face, FVector& OutOutward, FVector& OutAlong)
	{
		OutOutward = Forward * Face.Direction.X + Right * Face.Direction.Y;
		OutAlong = FVector::CrossProduct(Up, OutOutward);
		return CellCentre(Face.Cell) + OutOutward * (0.5 * CellSize);
	};

	for (const FEdgeFace& Face : Signature.EdgeFaces)
	{
		FVector Outward, Along;
		const FVector Middle = GetFaceMiddle(Face, Outward, Along);
		const FLinearColor Colour = Face.bCorner ? FMath::Lerp(GetSideColour(Face.Side), FootprintColour, 0.6f) : GetSideColour(Face.Side);

		AddLine(OutLines, Middle - Along * (0.42 * CellSize), Middle + Along * (0.42 * CellSize), Colour, Thickness * 3.0f);
	}

	// Faces a contact held on, as ticks standing up off them.
	for (const FIntVector& Key : Object.Contacts.ContactFaces)
	{
		const FVector Outward(PlaneToWorldDirection(Plane->Axes, PlaneDirections[Key.Z]));
		const FVector Base = Solver->GetPlaneCellCentre(*Plane, FIntPoint(Key.X, Key.Y)) + Up * DebugBias + Outward * (0.5 * CellSize);

		AddLine(OutLines, Base, Base + Up * (0.5 * CellSize), ContactColour, Thickness * 3.0f);
	}

	// Special runs, set a little way out from their edge, with their id.
	for (const FSpecialEdge& Special : Signature.SpecialEdges)
	{
		const FEdgeFace* First = nullptr;
		const FEdgeFace* Last = nullptr;

		for (const FEdgeFace& Face : Signature.EdgeFaces)
		{
			if (Face.Side == Special.Side && Face.Index == Special.Start)
			{
				First = &Face;
			}

			if (Face.Side == Special.Side && Face.Index == Special.Start + Special.Count - 1)
			{
				Last = &Face;
			}
		}

		if (!First || !Last)
		{
			continue;
		}

		FVector Outward, Along;
		const FVector Start = GetFaceMiddle(*First, Outward, Along) + Outward * (0.25 * CellSize);
		const FVector End = GetFaceMiddle(*Last, Outward, Along) + Outward * (0.25 * CellSize);
		const FVector RunDirection = (End - Start).GetSafeNormal(UE_SMALL_NUMBER, Along);

		AddLine(OutLines, Start - RunDirection * (0.4 * CellSize), End + RunDirection * (0.4 * CellSize), SpecialColour, Thickness * 2.0f);
		AddNumber(OutLines, Special.Id, 0.5 * (Start + End) + Outward * (0.25 * CellSize), RunDirection, Outward, 0.5 * CellSize, SpecialColour, Thickness);
	}

	// Its zones, a red square per cell.
	const FVector PlaneA(Plane->Axes.AxisA);
	const FVector PlaneB(Plane->Axes.AxisB);

	for (const TPair<FIntPoint, int32>& ZoneCell : Object.ZoneCells)
	{
		AddSquare(OutLines, Solver->GetPlaneCellCentre(*Plane, ZoneCell.Key) + Up * DebugBias, PlaneA, PlaneB, 0.35 * CellSize, ZoneColour, Thickness);
	}

	// The surfaces on top of it, a green square per cell at each layer's height.
	for (const uint64 HostedId : Object.HostedPlanes)
	{
		if (const FSupportPlane* Hosted = Solver->FindPlane(HostedId))
		{
			for (const FIntPoint& Cell : Hosted->LayerCells)
			{
				AddSquare(OutLines, Solver->GetPlaneCellCentre(*Hosted, Cell) + Up * DebugBias, Forward, Right, 0.4 * CellSize, SurfaceColour, Thickness);
			}
		}
	}

	// Which way is up for it: its support's gravity, pointing away from the face.
	const FVector Centre = Corner + 0.5 * (Depth + Width);
	const FVector Tip = Centre + Up * (Size.Z * CellSize);

	AddLine(OutLines, Centre, Tip, UpColour, Thickness);
	AddLine(OutLines, Tip, Tip - Up * (0.3 * CellSize) + Forward * (0.15 * CellSize), UpColour, Thickness);
	AddLine(OutLines, Tip, Tip - Up * (0.3 * CellSize) - Forward * (0.15 * CellSize), UpColour, Thickness);

	if (bDrawObstacles)
	{
		TArray<FBox> Pieces;
		RoomManager->GetObstaclePieceBounds(Object.VolumeMin, Object.VolumeMax, Pieces);

		for (const FBox& Piece : Pieces)
		{
			AddBox(OutLines, Piece, ObstacleColour, Thickness);
		}
	}
}
