// Copyright Epic Games, Inc. All Rights Reserved.

#include "Orb.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "WEMPlayers.h"

namespace OrbPrivate
{
	const FName HaloParameter(TEXT("Halo"));
	const FName FadeParameter(TEXT("Fade"));
	const FName OrbFractionParameter(TEXT("OrbFraction"));
	const FName SeedParameter(TEXT("Seed"));

	/** The engine's plane is 100 cm across. */
	constexpr float QuadMeshSize = 100.0f;

	/**
	 * What the orb writes into custom stencil. The AI overlay paints only where the stencil holds the pieces' value
	 * (wem.AIOverlay.Stencil, 1), so an orb in front of a piece, writing a value of its own over it, is never painted.
	 */
	constexpr int32 CustomStencilValue = 2;
}

AOrb::AOrb()
{
	PrimaryActorTick.bCanEverTick = true;

	// After the camera has moved for the frame, so the orb turns to where the drone is now and not where it was.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	Quad = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Quad"));
	Quad->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Quad->SetCollisionResponseToAllChannels(ECR_Ignore);
	Quad->SetGenerateOverlapEvents(false);
	Quad->CastShadow = false;
	Quad->bReceivesDecals = false;
	Quad->SetCanEverAffectNavigation(false);
	Quad->SetRenderCustomDepth(true);
	Quad->SetCustomDepthStencilValue(OrbPrivate::CustomStencilValue);
	SetRootComponent(Quad);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneFinder(TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Game/Orbs/M_Orb.M_Orb"));
	QuadMesh = PlaneFinder.Object;
	Material = MaterialFinder.Object;
}

void AOrb::BeginPlay()
{
	Super::BeginPlay();

	// It hovers where it was put.
	HoverCentre = GetActorLocation();

	// Each its own, so the orbs neither bob nor glisten in step.
	HoverPhase = FMath::FRandRange(0.0f, 2.0f * UE_PI);
	Seed = FMath::FRand();

	Quad->SetStaticMesh(QuadMesh);
	Quads = { Quad };
	MakeMaterial(Quad);

	UpdateLook(0.0f);
}

void AOrb::MakeMaterial(UStaticMeshComponent* Square)
{
	UMaterialInstanceDynamic* Instance = Material ? Square->CreateDynamicMaterialInstance(0, Material) : nullptr;

	if (Instance)
	{
		Instance->SetScalarParameterValue(OrbPrivate::OrbFractionParameter, 1.0f / FMath::Max(HaloRoom, 1.0f));
		Instance->SetScalarParameterValue(OrbPrivate::SeedParameter, Seed);
	}

	MaterialInstances.Add(Instance);
	Proximities.Add(0.0f);
	QuadPlayers.AddDefaulted();
}

void AOrb::FitQuads(const TArray<FWEMViewer>& Viewers)
{
	// Each further viewer gets a square of their own, made like the first and hanging from it, so it moves and scales
	// with the orb but turns by itself.
	while (Quads.Num() < Viewers.Num())
	{
		UStaticMeshComponent* Square = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		Square->SetStaticMesh(QuadMesh);
		Square->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Square->SetCollisionResponseToAllChannels(ECR_Ignore);
		Square->SetGenerateOverlapEvents(false);
		Square->CastShadow = false;
		Square->bReceivesDecals = false;
		Square->SetCanEverAffectNavigation(false);
		Square->SetRenderCustomDepth(true);
		Square->SetCustomDepthStencilValue(OrbPrivate::CustomStencilValue);
		Square->SetUsingAbsoluteRotation(true);
		Square->SetupAttachment(Quad);
		Square->RegisterComponent();
		Quads.Add(Square);
		MakeMaterial(Square);
	}

	// Who sees which square only changes as players come and go.
	bool bPlayersChanged = ShownViewers != Viewers.Num();

	for (int32 Index = 0; Index < Quads.Num(); ++Index)
	{
		bPlayersChanged |= QuadPlayers[Index].Get() != (Viewers.IsValidIndex(Index) ? Viewers[Index].Player.Get() : nullptr);
	}

	if (!bPlayersChanged)
	{
		return;
	}

	ShownViewers = Viewers.Num();

	for (int32 Index = 0; Index < Quads.Num(); ++Index)
	{
		QuadPlayers[Index] = Viewers.IsValidIndex(Index) ? Viewers[Index].Player.Get() : nullptr;

		// The first square is the root and always drawn; one with no viewer left (a player gone) is hidden.
		if (Index > 0)
		{
			Quads[Index]->SetVisibility(Viewers.IsValidIndex(Index));
		}

		WEMPlayers::ShowOnlyTo(Quads[Index], Viewers, Index);
	}
}

void AOrb::SetHoverCentre(const FVector& Centre)
{
	HoverCentre = Centre;
	SetActorLocation(Centre);
}

bool AOrb::CanBeCaught() const
{
	return !IsCaught() && ViewerDistance <= CatchDistance;
}

bool AOrb::CanBeCaughtFrom(const FVector& Location) const
{
	return !IsCaught() && FVector::Distance(Location, GetActorLocation()) <= CatchDistance;
}

void AOrb::Catch()
{
	if (!IsCaught())
	{
		CatchAge = 0.0f;
	}
}

void AOrb::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UpdateLook(DeltaSeconds);
}

void AOrb::UpdateLook(const float DeltaSeconds)
{
	Age += DeltaSeconds;

	FVector Centre = HoverCentre + FVector(0.0, 0.0, HoverAmplitude * FMath::Sin(2.0f * UE_PI * Age / FMath::Max(HoverPeriod, 0.1f) + HoverPhase));
	float Fade = 1.0f;
	float Swell = 1.0f;

	if (IsCaught())
	{
		CatchAge += DeltaSeconds;
		const float Done = FMath::Clamp(CatchAge / FMath::Max(FadeSeconds, 0.05f), 0.0f, 1.0f);

		if (Done >= 1.0f)
		{
			Destroy();
			return;
		}

		// Eased out: it lifts away quickly at first, then slows as it goes.
		const float Eased = 1.0f - FMath::Square(1.0f - Done);
		Centre.Z += FadeRise * Eased;
		Fade = 1.0f - Done;
		Swell = 1.0f + 0.25f * Eased;
	}

	SetActorLocation(Centre);
	SetActorScale3D(FVector(Diameter * FMath::Max(HaloRoom, 1.0f) * Swell / OrbPrivate::QuadMeshSize));

	// The players' cameras, or Simulate's view: each gets a square of their own.
	TArray<FWEMViewer> Viewers;
	WEMPlayers::GatherViewers(GetWorld(), Viewers);
	FitQuads(Viewers);

	ViewerDistance = UE_BIG_NUMBER;

	for (int32 Index = 0; Index < Quads.Num(); ++Index)
	{
		float Distance = UE_BIG_NUMBER;

		if (Viewers.IsValidIndex(Index))
		{
			const FWEMViewer& Viewer = Viewers[Index];
			Distance = static_cast<float>(FVector::Distance(Viewer.Location, Centre));

			// The square's face towards the viewer, and the picture's top towards the viewer's up, so it reads the same however the drone is turned.
			const FVector ToViewer = (Viewer.Location - Centre).GetSafeNormal();

			if (!ToViewer.IsNearlyZero())
			{
				Quads[Index]->SetWorldRotation(FRotationMatrix::MakeFromZX(ToViewer, Viewer.Up).Rotator());
			}
		}

		ViewerDistance = FMath::Min(ViewerDistance, Distance);

		// Kept as it was once caught, so the halo fades out with the orb rather than snapping shut.
		if (!IsCaught())
		{
			Proximities[Index] = HaloDistance > FullHaloDistance
				? 1.0f - FMath::SmoothStep(FullHaloDistance, HaloDistance, Distance)
				: (Distance <= FullHaloDistance ? 1.0f : 0.0f);
		}

		if (UMaterialInstanceDynamic* Instance = MaterialInstances[Index])
		{
			Instance->SetScalarParameterValue(OrbPrivate::HaloParameter, Proximities[Index]);
			Instance->SetScalarParameterValue(OrbPrivate::FadeParameter, Fade);
		}
	}

	Proximity = Proximities.IsEmpty() ? 0.0f : FMath::Max(Proximities);
}
