// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DroneTrailComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace DroneTrailTestsPrivate
{
	float DistanceToPolyline(const FVector3f& Point, TConstArrayView<FVector3f> Line)
	{
		float Best = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index + 1 < Line.Num(); ++Index)
		{
			const FVector3f Segment = Line[Index + 1] - Line[Index];
			const float LengthSquared = Segment.SizeSquared();
			const float Along = LengthSquared > 0.0f ? FMath::Clamp(FVector3f::DotProduct(Point - Line[Index], Segment) / LengthSquared, 0.0f, 1.0f) : 0.0f;
			Best = FMath::Min(Best, FVector3f::Dist(Point, Line[Index] + Segment * Along));
		}
		return Line.Num() == 1 ? FVector3f::Dist(Point, Line[0]) : Best;
	}

	/** Feeds every place to a path and returns its points with the tip on the end: the line the trail is drawn along. */
	TArray<FVector3f> Follow(FDroneTrailPath& Path, TConstArrayView<FVector3f> Places)
	{
		TArray<FVector3f> Line;
		for (const FVector3f& Place : Places)
		{
			Path.Advance(Place, Line);
		}
		Line.Add(Path.GetTip());
		return Line;
	}

	/** A drone flying about inside the cube: turning and climbing by smooth noise, never leaving it. One place per frame at 60 fps. */
	struct FWanderingFlight
	{
		FVector3f Position = FVector3f(5000.0f);
		FVector3f Heading = FVector3f::ForwardVector;
		float Time = 0.0f;

		FVector3f Step()
		{
			constexpr float DeltaTime = 1.0f / 60.0f;
			Time += DeltaTime;

			const float Yaw = FMath::DegreesToRadians(120.0f) * FMath::PerlinNoise1D(0.3f * Time) * DeltaTime;
			const float Pitch = FMath::DegreesToRadians(60.0f) * FMath::PerlinNoise1D(0.37f * Time + 100.0f) * DeltaTime;
			const float Speed = 900.0f + 700.0f * FMath::PerlinNoise1D(0.1f * Time + 50.0f);

			const FVector3f Right = FVector3f::CrossProduct(FVector3f::UpVector, Heading).GetSafeNormal();
			Heading = FQuat4f(FVector3f::UpVector, Yaw).RotateVector(Heading);
			if (!Right.IsZero())
			{
				Heading = FQuat4f(Right, Pitch).RotateVector(Heading);
			}

			// Back towards the middle near the cube's sides.
			const FVector3f ToMiddle = FVector3f(5000.0f) - Position;
			if (ToMiddle.GetAbsMax() > 4000.0f)
			{
				Heading = (Heading + 0.03f * ToMiddle.GetSafeNormal()).GetSafeNormal();
			}

			Position += Heading * Speed * DeltaTime;
			return Position;
		}
	};

	UDroneTrailComponent* SpawnTrail(UWorld* World)
	{
		AActor* Drone = World->SpawnActor<AActor>();
		UDroneTrailComponent* Trail = NewObject<UDroneTrailComponent>(Drone);
		Drone->SetRootComponent(Trail);
		Trail->RegisterComponent();
		return Trail;
	}

	bool SameRing(const FDroneTrailRing& A, const FDroneTrailRing& B)
	{
		return A.Position.Equals(B.Position, 1.0e-3f) && A.Tangent.Equals(B.Tangent, 1.0e-4f) && A.Normal.Equals(B.Normal, 1.0e-4f) && A.Cap == B.Cap;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDroneTrailPathTest, "WEM.DroneTrail.Path", WEMRoomManagerTests::TestFlags)

bool FDroneTrailPathTest::RunTest(const FString& Parameters)
{
	using namespace DroneTrailTestsPrivate;

	// Straight flight needs no point between where it started and where the drone is, but for one
	// every 512 frames that keeps each frame's check short.
	{
		FDroneTrailPath Path;
		TArray<FVector3f> Places;
		for (int32 Step = 0; Step <= 1000; ++Step)
		{
			Places.Add(FVector3f(Step, 0.0f, 0.0f));
		}
		const TArray<FVector3f> Line = Follow(Path, Places);
		TestEqual(TEXT("A straight line is the start, one point and the drone"), Line.Num(), 3);
		for (const FVector3f& Point : Line)
		{
			TestTrue(TEXT("Every point of a straight line is on it"), Point.Y == 0.0f && Point.Z == 0.0f);
		}
	}

	// A circle needs points, but only as many as keep the line within the tolerance of where the drone flew.
	{
		FDroneTrailPath Path;
		TArray<FVector3f> Places;
		for (int32 Step = 0; Step <= 3600; ++Step)
		{
			const float Angle = FMath::DegreesToRadians(0.1f * Step);
			Places.Add(FVector3f(500.0f * FMath::Cos(Angle), 500.0f * FMath::Sin(Angle), 0.0f));
		}
		const TArray<FVector3f> Line = Follow(Path, Places);
		float Worst = 0.0f;
		for (const FVector3f& Place : Places)
		{
			Worst = FMath::Max(Worst, DistanceToPolyline(Place, Line));
		}
		AddInfo(FString::Printf(TEXT("Circle of 5 m: %d points, worst %.2f cm off"), Line.Num(), Worst));
		TestTrue(TEXT("The circle stays within 1.5 tolerances"), Worst <= 1.5f * Path.Tolerance);
		TestTrue(TEXT("The circle takes a modest number of points"), Line.Num() > 20 && Line.Num() < 120);
	}

	// Turning straight back keeps the farthest place the drone reached.
	{
		FDroneTrailPath Path;
		TArray<FVector3f> Places;
		for (int32 Step = 0; Step <= 100; ++Step)
		{
			Places.Add(FVector3f(Step, 0.0f, 0.0f));
		}
		for (int32 Step = 99; Step >= -100; --Step)
		{
			Places.Add(FVector3f(Step, 0.0f, 0.0f));
		}
		const TArray<FVector3f> Line = Follow(Path, Places);
		float Farthest = -1.0f;
		for (const FVector3f& Point : Line)
		{
			Farthest = FMath::Max(Farthest, Point.X);
		}
		TestTrue(TEXT("The turning point is kept"), Farthest >= 99.0f);
	}

	// Standing still adds nothing.
	{
		FDroneTrailPath Path;
		TArray<FVector3f> Points;
		Path.Advance(FVector3f(1.0f, 2.0f, 3.0f), Points);
		TestFalse(TEXT("No movement, no change"), Path.Advance(FVector3f(1.0f, 2.0f, 3.0f), Points));
		TestEqual(TEXT("Only the start"), Points.Num(), 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDroneTrailPiecesTest, "WEM.DroneTrail.Pieces", WEMRoomManagerTests::TestFlags)

bool FDroneTrailPiecesTest::RunTest(const FString& Parameters)
{
	using namespace DroneTrailTestsPrivate;

	WEMRoomManagerTests::FScopedTestWorld TestWorld;
	UDroneTrailComponent* Trail = SpawnTrail(TestWorld.World);

	// Nothing is drawn before the drone moves.
	Trail->Advance(FVector(5000.0));
	Trail->Advance(FVector(5000.0));
	TestEqual(TEXT("No piece before moving"), Trail->GetPieces().Num(), 0);

	// A long wander, enough for several pieces.
	FWanderingFlight Flight;
	TArray<FVector3f> Places = {FVector3f(5000.0f)};
	while (Trail->GetNumPoints() < 3 * UDroneTrailMeshComponent::MaxRings)
	{
		Places.Add(Flight.Step());
		Trail->Advance(FVector(Places.Last()));
	}

	const TArray<UDroneTrailMeshComponent*> Pieces = Trail->GetPieces();
	AddInfo(FString::Printf(TEXT("%d frames, %d points, %d pieces"), Places.Num(), Trail->GetNumPoints(), Pieces.Num()));
	TestTrue(TEXT("Several pieces"), Pieces.Num() >= 3);

	TArray<FVector3f> Line;
	int32 BadFrames = 0;
	int32 Flips = 0;
	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		const UDroneTrailMeshComponent* Piece = Pieces[PieceIndex];
		const TConstArrayView<FDroneTrailRing> Rings = Piece->GetRings();
		const bool bLast = PieceIndex == Pieces.Num() - 1;

		TestEqual(TEXT("Only the last piece grows"), Piece->IsGrowing(), bLast);
		TestTrue(TEXT("A piece fits its rings"), Rings.Num() >= 2 && Rings.Num() <= UDroneTrailMeshComponent::MaxRings);
		TestTrue(TEXT("A piece sits at its first ring"), FVector3f(Piece->GetComponentLocation()).Equals(Rings[0].Position, 1.0e-2f));

		if (PieceIndex == 0)
		{
			TestEqual(TEXT("The trail starts rounded"), Rings[0].Cap, -1.0f);
		}
		else
		{
			// The tube carries on from the piece before without a seam.
			const TConstArrayView<FDroneTrailRing> Before = Pieces[PieceIndex - 1]->GetRings();
			TestTrue(TEXT("A piece starts on the ring the one before ends on"), SameRing(Rings[0], Before.Last()));
		}
		if (bLast)
		{
			TestEqual(TEXT("The trail ends rounded"), Rings.Last().Cap, 1.0f);
			TestTrue(TEXT("The trail reaches the drone"), Rings[Rings.Num() - 2].Position.Equals(Places.Last(), 1.0e-2f));
		}
		else
		{
			TestEqual(TEXT("A finished piece ends on the tube"), Rings.Last().Cap, 0.0f);
		}

		for (int32 Index = 0; Index < Rings.Num(); ++Index)
		{
			const FDroneTrailRing& Ring = Rings[Index];
			if (!FMath::IsNearlyEqual(Ring.Tangent.Size(), 1.0f, 1.0e-3f) || !FMath::IsNearlyEqual(Ring.Normal.Size(), 1.0f, 1.0e-3f)
				|| FMath::Abs(FVector3f::DotProduct(Ring.Normal, Ring.Tangent)) > 1.0e-3f)
			{
				++BadFrames;
			}
			if (Index > 0 && Ring.Cap == 0.0f && Rings[Index - 1].Cap == 0.0f && FVector3f::DotProduct(Ring.Normal, Rings[Index - 1].Normal) < 0.0f)
			{
				++Flips;
			}
			if (Ring.Cap == 0.0f && (PieceIndex == 0 || Index > 0))
			{
				Line.Add(Ring.Position);
			}
		}
	}
	TestEqual(TEXT("Every ring is square to the path"), BadFrames, 0);
	TestEqual(TEXT("The tube never twists over"), Flips, 0);
	TestEqual(TEXT("Every point is drawn once, and the drone's place"), Line.Num(), Trail->GetNumPoints() + 1);

	// The drawn line follows the flight. Every 7th place keeps this quick.
	float Worst = 0.0f;
	for (int32 Index = 0; Index < Places.Num(); Index += 7)
	{
		Worst = FMath::Max(Worst, DistanceToPolyline(Places[Index], Line));
	}
	AddInfo(FString::Printf(TEXT("Worst %.2f cm from the flight"), Worst));
	TestTrue(TEXT("The trail follows the flight"), Worst <= 1.5f * Trail->Accuracy);

	// The AI overlay paints only the pieces' custom stencil value; the trail writes one of its own over them, as the orbs do.
	const IConsoleVariable* PieceStencil = IConsoleManager::Get().FindConsoleVariable(TEXT("wem.AIOverlay.Stencil"));
	for (const UDroneTrailMeshComponent* Piece : Pieces)
	{
		TestTrue(TEXT("The trail writes custom stencil"), Piece->bRenderCustomDepth);
		TestTrue(TEXT("Not the pieces' value, so the AI overlay leaves it alone"), Piece->CustomDepthStencilValue != (PieceStencil ? PieceStencil->GetInt() : 1));
	}

	// Thickening reaches every piece.
	Trail->SetWidth(10.0f);
	for (const UDroneTrailMeshComponent* Piece : Trail->GetPieces())
	{
		TestEqual(TEXT("Every piece takes the new width"), Piece->GetRadius(), 5.0f);
	}

	// Editing a setting while playing rebuilds the drone's components: the new Trail takes the trail
	// over, in its own width, and flies on from where it was.
	UDroneTrailComponent* Rebuilt = NewObject<UDroneTrailComponent>(Trail->GetOwner());
	Rebuilt->RegisterComponent();
	TStructOnScope<FActorComponentInstanceData> Data = Trail->GetComponentInstanceData();
	Data.Get()->ApplyToComponent(Rebuilt, ECacheApplyPhase::PostUserConstructionScript);
	TestTrue(TEXT("The rebuilt Trail has the pieces"), Rebuilt->GetPieces() == Trail->GetPieces());
	TestEqual(TEXT("The rebuilt Trail has the points"), Rebuilt->GetNumPoints(), Trail->GetNumPoints());
	TestEqual(TEXT("The pieces take the rebuilt Trail's width"), Rebuilt->GetPieces()[0]->GetRadius(), 0.5f * Rebuilt->Width);

	const int32 PointsBefore = Rebuilt->GetNumPoints();
	UDroneTrailMeshComponent* Growing = Rebuilt->GetPieces().Last();
	const int32 RingsBefore = Growing->GetRings().Num();
	for (int32 Step = 0; Step < 600; ++Step)
	{
		Rebuilt->Advance(FVector(Flight.Step()));
	}
	TestTrue(TEXT("The rebuilt Trail flies on"), Rebuilt->GetNumPoints() > PointsBefore);
	TestTrue(TEXT("It carries on in the same piece"), Rebuilt->GetPieces().Contains(Growing) && (Growing != Rebuilt->GetPieces().Last() || Growing->GetRings().Num() > RingsBefore));

	TestWorld.World->DestroyActor(Trail->GetOwner());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDroneTrailPrefillTest, "WEM.DroneTrail.Prefill", WEMRoomManagerTests::TestFlags)

bool FDroneTrailPrefillTest::RunTest(const FString& Parameters)
{
	using namespace DroneTrailTestsPrivate;

	// Prefilling redraws the growing piece only at the end, and comes out the same as redrawing it every step.
	WEMRoomManagerTests::FScopedTestWorld TestWorld;
	UDroneTrailComponent* Once = SpawnTrail(TestWorld.World);
	UDroneTrailComponent* EveryStep = SpawnTrail(TestWorld.World);
	Once->Prefill(6.0f, true);
	EveryStep->Prefill(6.0f, false);

	const TArray<UDroneTrailMeshComponent*> A = Once->GetPieces();
	const TArray<UDroneTrailMeshComponent*> B = EveryStep->GetPieces();
	AddInfo(FString::Printf(TEXT("6 minutes: %d points, %d pieces"), Once->GetNumPoints(), A.Num()));
	TestTrue(TEXT("More than one piece"), A.Num() > 1);
	TestEqual(TEXT("As many pieces"), A.Num(), B.Num());
	int32 Different = 0;
	for (int32 Piece = 0; Piece < FMath::Min(A.Num(), B.Num()); ++Piece)
	{
		const TConstArrayView<FDroneTrailRing> RingsA = A[Piece]->GetRings();
		const TConstArrayView<FDroneTrailRing> RingsB = B[Piece]->GetRings();
		TestEqual(TEXT("As many rings"), RingsA.Num(), RingsB.Num());
		for (int32 Ring = 0; Ring < FMath::Min(RingsA.Num(), RingsB.Num()); ++Ring)
		{
			Different += SameRing(RingsA[Ring], RingsB[Ring]) ? 0 : 1;
		}
	}
	TestEqual(TEXT("Every ring the same"), Different, 0);

	TestWorld.World->DestroyActor(Once->GetOwner());
	TestWorld.World->DestroyActor(EveryStep->GetOwner());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDroneTrailLongFlightTest, "WEM.DroneTrail.Perf.LongFlight", WEMRoomManagerTests::SlowTestFlags)

bool FDroneTrailLongFlightTest::RunTest(const FString& Parameters)
{
	using namespace DroneTrailTestsPrivate;

	// Two hours of wandering flight at 60 fps: how fast the trail gains points and pieces, what a
	// frame of it costs, and how much memory it holds at the end.
	WEMRoomManagerTests::FScopedTestWorld TestWorld;
	UDroneTrailComponent* Trail = SpawnTrail(TestWorld.World);

	constexpr int32 Minutes = 120;
	constexpr int32 Frames = Minutes * 60 * 60;
	FWanderingFlight Flight;
	double Total = 0.0;
	double Worst = 0.0;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		const FVector Place(Flight.Step());
		const double Start = FPlatformTime::Seconds();
		Trail->Advance(Place);
		const double Took = FPlatformTime::Seconds() - Start;
		Total += Took;
		Worst = FMath::Max(Worst, Took);

		if ((Frame + 1) % (30 * 60 * 60) == 0)
		{
			AddInfo(FString::Printf(TEXT("%3d min: %7d points, %4d pieces"), (Frame + 1) / 3600, Trail->GetNumPoints(), Trail->GetPieces().Num()));
		}
	}

	const int32 Points = Trail->GetNumPoints();
	const int32 Pieces = Trail->GetPieces().Num();
	const int64 GpuBytes = int64(Pieces) * UDroneTrailMeshComponent::MaxRings * UDroneTrailMeshComponent::Sides * (12 + 8 + 4);
	const int64 CpuBytes = int64(Points) * sizeof(FDroneTrailRing);
	AddInfo(FString::Printf(TEXT("%d min: %d points (%.1f a second), %d pieces, %.1f triangles a piece"), Minutes, Points, Points / (Minutes * 60.0), Pieces,
		2.0 * UDroneTrailMeshComponent::Sides * (UDroneTrailMeshComponent::MaxRings - 1)));
	AddInfo(FString::Printf(TEXT("Advance: %.2f us a frame on average, %.2f ms at worst"), 1.0e6 * Total / Frames, 1.0e3 * Worst));
	AddInfo(FString::Printf(TEXT("Memory at most: %.1f MB of vertices on the GPU, %.1f MB of rings on the CPU"), GpuBytes / 1048576.0, CpuBytes / 1048576.0));
	TestTrue(TEXT("A frame of trail stays well under a millisecond on average"), Total / Frames < 1.0e-4);

	TestWorld.World->DestroyActor(Trail->GetOwner());
	return true;
}

#endif
