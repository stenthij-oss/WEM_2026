// Copyright Epic Games, Inc. All Rights Reserved.

#include "DroneTrailComponent.h"

#include "HAL/IConsoleManager.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DroneTrailComponent)

DECLARE_CYCLE_STAT(TEXT("Advance (game thread)"), STAT_DroneTrailAdvance, STATGROUP_DroneTrail);

namespace DroneTrailPrivate
{
	/** More places than this since the last point and the newest becomes a point anyway, so each frame's check stays short. */
	constexpr int32 MaxPassed = 512;

	float DistanceToSegment(const FVector3f& Point, const FVector3f& Start, const FVector3f& End)
	{
		const FVector3f Segment = End - Start;
		const float LengthSquared = Segment.SizeSquared();
		const float Along = LengthSquared > 0.0f ? FMath::Clamp(FVector3f::DotProduct(Point - Start, Segment) / LengthSquared, 0.0f, 1.0f) : 0.0f;
		return FVector3f::Dist(Point, Start + Segment * Along);
	}

	void PrefillPlaying(const TArray<FString>& Args)
	{
		const float Minutes = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 60.0f;
		for (TObjectIterator<UDroneTrailComponent> It; It; ++It)
		{
			if (It->HasBegunPlay())
			{
				It->Prefill(Minutes);
			}
		}
	}

	FAutoConsoleCommand PrefillCommand(
		TEXT("wem.DroneTrail.Prefill"),
		TEXT("wem.DroneTrail.Prefill <minutes, default 60>: lays a made-up flight of that long into every playing drone's trail, to see and measure a long game's trail."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&PrefillPlaying));

	/** Turns the last ring's normal square to the new tangent, so the tube bends without twisting. */
	FVector3f CarryNormal(const FVector3f& Previous, const FVector3f& Tangent)
	{
		FVector3f Normal = Previous - Tangent * FVector3f::DotProduct(Previous, Tangent);
		if (Normal.SizeSquared() < 1.0e-4f)
		{
			Normal = FVector3f::CrossProduct(Tangent, FMath::Abs(Tangent.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector);
		}
		return Normal.GetSafeNormal();
	}
}

bool FDroneTrailPath::Advance(const FVector3f& Location, TArray<FVector3f>& OutPoints)
{
	using namespace DroneTrailPrivate;

	if (!bStarted)
	{
		bStarted = true;
		Last = Location;
		Tip = Location;
		OutPoints.Add(Location);
		return true;
	}
	if (FVector3f::DistSquared(Location, Tip) < UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	// The line from the last point to here has to pass near every place the drone has been since.
	bool bStraight = Passed.Num() < MaxPassed && DistanceToSegment(Tip, Last, Location) <= Tolerance;
	for (int32 Index = 0; bStraight && Index < Passed.Num(); ++Index)
	{
		bStraight = DistanceToSegment(Passed[Index], Last, Location) <= Tolerance;
	}

	if (bStraight)
	{
		// Places closer together than half the tolerance add nothing the check needs.
		const FVector3f& Previous = Passed.IsEmpty() ? Last : Passed.Last();
		if (FVector3f::Dist(Tip, Previous) >= 0.5f * Tolerance)
		{
			Passed.Add(Tip);
		}
	}
	else
	{
		// The line could still reach the drone's last place, so that becomes the point the next line starts from.
		OutPoints.Add(Tip);
		Last = Tip;
		Passed.Reset();
	}
	Tip = Location;
	return true;
}

UDroneTrailComponent::UDroneTrailComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// After the drone has moved this frame, so the trail ends exactly where it is drawn from.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultMaterial(TEXT("/Game/DroneTrail/M_DroneTrail.M_DroneTrail"));
	Material = DefaultMaterial.Object;
}

void UDroneTrailComponent::BeginPlay()
{
	Super::BeginPlay();

	ApplyLook();
	Advance(GetComponentLocation());
}

FDroneTrailInstanceData::FDroneTrailInstanceData(const UDroneTrailComponent* SourceComponent)
	: FSceneComponentInstanceData(SourceComponent)
	, Finished(SourceComponent->Finished)
	, Head(SourceComponent->Head)
	, Path(SourceComponent->Path)
	, HeadPoints(SourceComponent->HeadPoints)
	, HeadRings(SourceComponent->HeadRings)
	, FirstChanged(SourceComponent->FirstChanged)
	, bHeadHasStart(SourceComponent->bHeadHasStart)
	, NumPoints(SourceComponent->NumPoints)
{
}

void FDroneTrailInstanceData::ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase)
{
	Super::ApplyToComponent(Component, CacheApplyPhase);
	CastChecked<UDroneTrailComponent>(Component)->Adopt(*this);
}

void FDroneTrailInstanceData::AddReferencedObjects(FReferenceCollector& Collector)
{
	Super::AddReferencedObjects(Collector);
	Collector.AddReferencedObjects(Finished);
	Collector.AddReferencedObject(Head);
}

TStructOnScope<FActorComponentInstanceData> UDroneTrailComponent::GetComponentInstanceData() const
{
	return MakeStructOnScope<FActorComponentInstanceData, FDroneTrailInstanceData>(this);
}

void UDroneTrailComponent::Adopt(const FDroneTrailInstanceData& Data)
{
	Path = Data.Path;
	HeadPoints = Data.HeadPoints;
	HeadRings = Data.HeadRings;
	FirstChanged = Data.FirstChanged;
	bHeadHasStart = Data.bHeadHasStart;
	NumPoints = Data.NumPoints;
	Finished = Data.Finished;
	Head = Data.Head;

	// The old component's material instance goes with it.
	UMaterialInstanceDynamic* Instance = GetMaterialInstance();
	ApplyLook();
	for (UDroneTrailMeshComponent* Piece : GetPieces())
	{
		Piece->SetMaterial(0, Instance);
		Piece->SetRadius(0.5f * Width);
	}
}

void UDroneTrailComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	Advance(GetComponentLocation());
}

void UDroneTrailComponent::SetColor(const FLinearColor NewColor)
{
	Color = NewColor;
	ApplyLook();
}

void UDroneTrailComponent::SetGlow(const float NewGlow)
{
	Glow = FMath::Max(0.0f, NewGlow);
	ApplyLook();
}

void UDroneTrailComponent::SetWidth(const float NewWidth)
{
	Width = FMath::Max(0.1f, NewWidth);
	ApplyLook();
	for (UDroneTrailMeshComponent* Piece : GetPieces())
	{
		Piece->SetRadius(0.5f * Width);
	}
}

TArray<UDroneTrailMeshComponent*> UDroneTrailComponent::GetPieces() const
{
	TArray<UDroneTrailMeshComponent*> Pieces(Finished);
	if (Head)
	{
		Pieces.Add(Head);
	}
	return Pieces;
}

void UDroneTrailComponent::Advance(const FVector& Location)
{
	SCOPE_CYCLE_COUNTER(STAT_DroneTrailAdvance);

	Path.Tolerance = Accuracy;
	NewPoints.Reset();
	if (!Path.Advance(FVector3f(Location), NewPoints))
	{
		return;
	}
	for (const FVector3f& Point : NewPoints)
	{
		AddPoint(Point);
	}

	if (Head)
	{
		if (!bDeferRedraw)
		{
			Redraw();
		}
	}
	// Nothing is drawn until the drone has moved off where it started.
	else if (HeadPoints.Num() > 1 || FVector3f::Dist(Path.GetTip(), HeadPoints[0]) >= Accuracy)
	{
		StartPiece();
	}
}

void UDroneTrailComponent::Prefill(const float Minutes, const bool bRedrawOnce)
{
	// Turning and climbing by smooth noise at 2 to 16 m/s, steered back when it strays too far.
	constexpr float DeltaTime = 1.0f / 60.0f;
	const FVector3f Home(GetComponentLocation());
	FVector3f Position = Home;
	FVector3f Heading = FVector3f(GetForwardVector()).GetSafeNormal2D();
	if (Heading.IsZero())
	{
		Heading = FVector3f::ForwardVector;
	}

	// The growing piece is redrawn once at the end instead of every step; finished pieces are drawn as they finish.
	bDeferRedraw = bRedrawOnce;
	const int32 Frames = FMath::RoundToInt32(Minutes * 60.0f * 60.0f);
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		const float Time = Frame * DeltaTime;
		const float Yaw = FMath::DegreesToRadians(120.0f) * FMath::PerlinNoise1D(0.3f * Time) * DeltaTime;
		const float Pitch = FMath::DegreesToRadians(60.0f) * FMath::PerlinNoise1D(0.37f * Time + 100.0f) * DeltaTime;
		const float Speed = 900.0f + 700.0f * FMath::PerlinNoise1D(0.1f * Time + 50.0f);

		const FVector3f Right = FVector3f::CrossProduct(FVector3f::UpVector, Heading).GetSafeNormal();
		Heading = FQuat4f(FVector3f::UpVector, Yaw).RotateVector(Heading);
		if (!Right.IsZero())
		{
			Heading = FQuat4f(Right, Pitch).RotateVector(Heading);
		}
		const FVector3f ToHome = Home - Position;
		if (ToHome.GetAbsMax() > 4000.0f)
		{
			Heading = (Heading + 0.03f * ToHome.GetSafeNormal()).GetSafeNormal();
		}

		Position += Heading * Speed * DeltaTime;
		Advance(FVector(Position));
	}

	bDeferRedraw = false;
	if (Head)
	{
		Redraw();
	}
}

void UDroneTrailComponent::AddPoint(const FVector3f& Point)
{
	HeadPoints.Add(Point);
	HeadRings.AddDefaulted();
	++NumPoints;

	// The point before this one now has the path going out of it, so its ring turns.
	FirstChanged = FMath::Min(FirstChanged, HeadPoints.Num() - 2);

	// The point, the drone's place and the rounded end each take a ring.
	if (Head && StartRings() + HeadPoints.Num() + 2 > UDroneTrailMeshComponent::MaxRings)
	{
		FinishPiece();
	}
}

void UDroneTrailComponent::StartPiece()
{
	const int32 First = bHeadHasStart ? 0 : 1;
	for (int32 Index = First; Index < HeadPoints.Num(); ++Index)
	{
		HeadRings[Index] = MakePointRing(Index);
	}

	TArray<FDroneTrailRing> Rings;
	Rings.Reserve(StartRings() + HeadRings.Num() + 2);
	if (bHeadHasStart)
	{
		FDroneTrailRing& Start = Rings.Add_GetRef(HeadRings[0]);
		Start.Cap = -1.0f;
	}
	Rings.Append(HeadRings);
	AddTipRings(Rings);

	Head = NewObject<UDroneTrailMeshComponent>(GetOwner(), NAME_None, RF_Transient);
	Head->SetMaterial(0, GetMaterialInstance());
	Head->Begin(MoveTemp(Rings), 0.5f * Width);
	Head->RegisterComponent();

	FirstChanged = HeadPoints.Num() - 1;
}

void UDroneTrailComponent::FinishPiece()
{
	// Every point but the newest has the next one now, so their rings are final. The piece keeps
	// them; the next starts again from the last it keeps.
	const int32 NumKept = HeadPoints.Num() - 1;
	const int32 First = FMath::Clamp(FirstChanged, bHeadHasStart ? 0 : 1, NumKept - 1);

	TArray<FDroneTrailRing> Rings;
	if (First == 0 && bHeadHasStart)
	{
		Rings.AddDefaulted();
	}
	for (int32 Index = First; Index < NumKept; ++Index)
	{
		HeadRings[Index] = MakePointRing(Index);
		Rings.Add(HeadRings[Index]);
	}
	if (First == 0 && bHeadHasStart)
	{
		Rings[0] = HeadRings[0];
		Rings[0].Cap = -1.0f;
	}
	const int32 FirstRing = (First == 0 && bHeadHasStart) ? 0 : StartRings() + First;
	Head->SetRings(FirstRing, Rings);
	Head->Finish(StartRings() + NumKept);
	Finished.Add(Head);
	Head = nullptr;

	HeadPoints = {HeadPoints[NumKept - 1], HeadPoints[NumKept]};
	HeadRings = {HeadRings[NumKept - 1], FDroneTrailRing()};
	bHeadHasStart = false;
	StartPiece();
}

void UDroneTrailComponent::Redraw()
{
	const int32 First = FMath::Clamp(FirstChanged, bHeadHasStart ? 0 : 1, HeadPoints.Num() - 1);
	const bool bWithStart = First == 0 && bHeadHasStart;

	TArray<FDroneTrailRing, TInlineAllocator<8>> Rings;
	if (bWithStart)
	{
		Rings.AddDefaulted();
	}
	for (int32 Index = First; Index < HeadPoints.Num(); ++Index)
	{
		HeadRings[Index] = MakePointRing(Index);
		Rings.Add(HeadRings[Index]);
	}
	if (bWithStart)
	{
		Rings[0] = HeadRings[0];
		Rings[0].Cap = -1.0f;
	}

	TArray<FDroneTrailRing> TipRings;
	AddTipRings(TipRings);
	Rings.Append(TipRings);

	Head->SetRings(bWithStart ? 0 : StartRings() + First, Rings);
	FirstChanged = HeadPoints.Num() - 1;
}

FDroneTrailRing UDroneTrailComponent::MakePointRing(const int32 Index) const
{
	using namespace DroneTrailPrivate;

	const FVector3f& Point = HeadPoints[Index];
	const FVector3f& Next = Index + 1 < HeadPoints.Num() ? HeadPoints[Index + 1] : Path.GetTip();
	const FVector3f In = Index > 0 ? (Point - HeadPoints[Index - 1]).GetSafeNormal() : FVector3f::ZeroVector;
	const FVector3f Out = (Next - Point).GetSafeNormal();

	// Halfway between in and out; straight back on itself, just the way in.
	FVector3f Tangent = (In + Out).GetSafeNormal();
	if (Tangent.IsZero())
	{
		Tangent = !In.IsZero() ? In : (Index > 0 ? HeadRings[Index - 1].Tangent : FVector3f::ForwardVector);
	}

	FDroneTrailRing Ring;
	Ring.Position = Point;
	Ring.Tangent = Tangent;
	Ring.Normal = CarryNormal(Index > 0 ? HeadRings[Index - 1].Normal : FVector3f::UpVector, Tangent);
	return Ring;
}

void UDroneTrailComponent::AddTipRings(TArray<FDroneTrailRing>& Rings) const
{
	const FDroneTrailRing& LastRing = HeadRings.Last();
	const FVector3f Toward = (Path.GetTip() - HeadPoints.Last()).GetSafeNormal();

	FDroneTrailRing& Tip = Rings.AddDefaulted_GetRef();
	Tip.Position = Path.GetTip();
	Tip.Tangent = Toward.IsZero() ? LastRing.Tangent : Toward;
	Tip.Normal = DroneTrailPrivate::CarryNormal(LastRing.Normal, Tip.Tangent);

	FDroneTrailRing End = Tip;
	End.Cap = 1.0f;
	Rings.Add(End);
}

UMaterialInstanceDynamic* UDroneTrailComponent::GetMaterialInstance()
{
	if (!MaterialInstance)
	{
		MaterialInstance = UMaterialInstanceDynamic::Create(Material ? Material.Get() : UMaterial::GetDefaultMaterial(MD_Surface), this);
		ApplyLook();
	}
	return MaterialInstance;
}

void UDroneTrailComponent::ApplyLook()
{
	if (UMaterialInstanceDynamic* Instance = GetMaterialInstance())
	{
		Instance->SetVectorParameterValue(TEXT("Color"), Color);
		Instance->SetScalarParameterValue(TEXT("Glow"), Glow);
		Instance->SetScalarParameterValue(TEXT("Radius"), 0.5f * Width);
	}
}

#if WITH_EDITOR
void UDroneTrailComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Edits while playing reach the trail already flown. On a drone Blueprint the edit has rebuilt
	// this component already, and the new one has taken the trail over (Adopt).
	if (!HasBegunPlay())
	{
		return;
	}
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UDroneTrailComponent, Material))
	{
		MaterialInstance = nullptr;
		for (UDroneTrailMeshComponent* Piece : GetPieces())
		{
			Piece->SetMaterial(0, GetMaterialInstance());
		}
	}
	SetWidth(Width);
}
#endif
