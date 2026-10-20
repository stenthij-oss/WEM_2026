// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "DroneTrailMeshComponent.generated.h"

/** `stat DroneTrail` while playing shows what the trail costs a frame. */
DECLARE_STATS_GROUP(TEXT("Drone Trail"), STATGROUP_DroneTrail, STATCAT_Advanced);

/** One ring of a drone trail's tube: where it sits, which way the path runs through it, and how it ends. */
struct FDroneTrailRing
{
	/** World space. */
	FVector3f Position = FVector3f::ZeroVector;

	/** Along the path. */
	FVector3f Tangent = FVector3f::ForwardVector;

	/** Across the path: the ring's first vertex points this way. Carried from ring to ring so the tube does not twist. */
	FVector3f Normal = FVector3f::UpVector;

	/** 0 for the tube. -1 or +1 closes the tube to a point one radius behind or ahead, which rounds off its two ends. */
	float Cap = 0.0f;
};

/**
 * One piece of a drone's light trail: a thin glowing tube through its rings. UDroneTrailComponent
 * makes and feeds these. The newest piece grows as the drone flies; once it is full it is never
 * touched again, so a finished piece costs nothing but drawing it.
 */
UCLASS(ClassGroup = Rendering, NotBlueprintable, Transient)
class WEM_2026_API UDroneTrailMeshComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	/** Sides of the tube. */
	static constexpr int32 Sides = 8;

	/** Rings in a full piece. Keeps a piece's vertices within 16-bit indices. */
	static constexpr int32 MaxRings = 1024;

	UDroneTrailMeshComponent();

	/** Starts this piece, growing, with these rings, and moves it to the first. Call before registering it. */
	void Begin(TArray<FDroneTrailRing>&& InRings, float InRadius);

	/** Replaces the rings from FirstRing on with these, adding any that run past the end. Only while growing. */
	void SetRings(int32 FirstRing, TConstArrayView<FDroneTrailRing> NewRings);

	/** Stops growing and keeps only the first NumRings rings. */
	void Finish(int32 NumRings);

	/** The tube's radius in cm. Rebuilds the piece when it changes. */
	void SetRadius(float InRadius);

	float GetRadius() const { return Radius; }
	bool IsGrowing() const { return bGrowing; }
	TConstArrayView<FDroneTrailRing> GetRings() const { return Rings; }

	/** Where the piece sits: its first ring. Its vertices are kept relative to this. */
	const FVector3f& GetOrigin() const { return Origin; }

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 1; }

private:
	TArray<FDroneTrailRing> Rings;

	FVector3f Origin = FVector3f::ZeroVector;

	/** Around the rings' positions, relative to Origin. */
	FBox3f LocalBox = FBox3f(ForceInit);

	float Radius = 3.0f;
	bool bGrowing = false;
};
