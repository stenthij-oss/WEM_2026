// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Orb.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class APlayerController;
struct FWEMViewer;

/**
 * An orb hovering over a segment, to be caught. It is a picture on a square that always turns to
 * face whoever is looking - the drone - and bobs up and down. As the drone comes near, a halo
 * opens round it and glistens wider and harder the nearer it comes. Caught, it fades away.
 *
 * With more than one player, each sees a square of their own, turned to them and lit by how near
 * they are; the others' squares are hidden from them.
 *
 * The object placer stands one in each zone and catches it when the player asks; the orb only
 * looks after how it looks. Its material is expected to take the scalar parameters Halo (0 to 1,
 * how near the drone is), Fade (1 shown, 0 gone), OrbFraction (the orb's share of the square's
 * width, the rest left for the halo) and Seed (a number of its own, so no two glisten alike).
 *
 * The AI overlay never paints it: it writes a custom stencil value of its own over the pieces'.
 */
UCLASS()
class WEM_2026_API AOrb : public AActor
{
	GENERATED_BODY()

public:
	AOrb();

	/** How wide the orb itself is, in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb", meta = (ClampMin = "1.0"))
	float Diameter = 90.0f;

	/** How much wider than the orb the square it is drawn on is, leaving room for the halo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb", meta = (ClampMin = "1.0"))
	float HaloRoom = 3.0f;

	/** How far it bobs above and below where it hovers, in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Hover", meta = (ClampMin = "0.0"))
	float HoverAmplitude = 12.0f;

	/** Seconds for one bob, up and down again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Hover", meta = (ClampMin = "0.1"))
	float HoverPeriod = 3.2f;

	/** How near the drone has to come for the halo to start to show, in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Halo", meta = (ClampMin = "0.0"))
	float HaloDistance = 1200.0f;

	/** How near it has to come for the halo to be at its widest and brightest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Halo", meta = (ClampMin = "0.0"))
	float FullHaloDistance = 150.0f;

	/** How near the drone has to be to catch it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Catch", meta = (ClampMin = "0.0"))
	float CatchDistance = 500.0f;

	/** Seconds it takes to fade away once caught. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Catch", meta = (ClampMin = "0.05"))
	float FadeSeconds = 1.4f;

	/** How far it drifts up while it fades, in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Catch")
	float FadeRise = 40.0f;

	/** The square it is drawn on: a unit plane facing +Z, like the engine's 100 cm plane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Look")
	TObjectPtr<UStaticMesh> QuadMesh;

	/** The orb's look; see the class comment for the parameters it takes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb|Look")
	TObjectPtr<UMaterialInterface> Material;

	/** How near the nearest drone is, from 0 - outside HaloDistance - to 1 - inside FullHaloDistance. Readout only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Orb|Halo")
	float Proximity = 0.0f;

	/** Puts the orb where it hovers: the middle of its bob. */
	void SetHoverCentre(const FVector& Centre);

	FVector GetHoverCentre() const
	{
		return HoverCentre;
	}

	/** How far the nearest one looking is, in centimetres. Very far when there is no one. */
	float GetViewerDistance() const
	{
		return ViewerDistance;
	}

	/** Whether any drone is near enough to catch it, and it has not been caught already. */
	bool CanBeCaught() const;

	/** Whether a drone looking from Location is near enough to catch it, and it has not been caught already. */
	bool CanBeCaughtFrom(const FVector& Location) const;

	/** Starts it fading away; it takes itself out of the world once gone. */
	void Catch();

	bool IsCaught() const
	{
		return CatchAge >= 0.0f;
	}

	//~ Begin AActor Interface
	virtual void Tick(float DeltaSeconds) override;
	//~ End AActor Interface

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	//~ End AActor Interface

private:
	/** Turns, bobs, fades and sets the halo for this frame. */
	void UpdateLook(float DeltaSeconds);

	/** A square for every viewer, at least one, each shown to its viewer's player only; squares left over are hidden. */
	void FitQuads(const TArray<FWEMViewer>& Viewers);

	/** Makes a square's material, with the orb's own parameters. */
	void MakeMaterial(UStaticMeshComponent* Square);

	/** The first viewer's square, and the root: the orb is where it is. */
	UPROPERTY(VisibleAnywhere, Category = "Orb", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> Quad;

	/** Quad, then one more for each further viewer, each turned to its own. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Quads;

	/** Each square's material instance, in the same order. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> MaterialInstances;

	/** Each square's halo, from its viewer's distance; kept as it was once caught. */
	TArray<float> Proximities;

	/** The player each square was last shown to, so the hiding is only changed when the players do. */
	TArray<TWeakObjectPtr<APlayerController>> QuadPlayers;
	int32 ShownViewers = INDEX_NONE;

	FVector HoverCentre = FVector::ZeroVector;
	float HoverPhase = 0.0f;
	float Seed = 0.0f;
	float Age = 0.0f;
	float CatchAge = -1.0f;
	float ViewerDistance = UE_BIG_NUMBER;
};
