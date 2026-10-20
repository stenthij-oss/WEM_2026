// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DroneTrailMeshComponent.h"
#include "DroneTrailComponent.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;

/**
 * Picks the few points a drone's trail needs out of where the drone is every frame. A point is
 * added only once a straight line from the last point would pass farther than Tolerance from
 * somewhere the drone really was, so straight flight adds none and tight turns add many.
 */
class WEM_2026_API FDroneTrailPath
{
public:
	/** cm. */
	float Tolerance = 1.0f;

	/**
	 * Feeds where the drone is now, and adds to OutPoints the points this makes certain, oldest first.
	 * The first call adds where the drone starts. Returns false when the drone has not moved.
	 */
	bool Advance(const FVector3f& Location, TArray<FVector3f>& OutPoints);

	/** Where the drone was at the last Advance. The trail runs on from the last point to here. */
	const FVector3f& GetTip() const { return Tip; }

private:
	/** Where the drone has been since the last point, which the line from that point has to stay near. */
	TArray<FVector3f> Passed;

	FVector3f Last = FVector3f::ZeroVector;
	FVector3f Tip = FVector3f::ZeroVector;
	bool bStarted = false;
};

class UDroneTrailComponent;

/**
 * Carries the trail flown so far over to the new Trail component when the drone's Blueprint
 * rebuilds its components, which editing one of its settings while playing does.
 */
USTRUCT()
struct FDroneTrailInstanceData : public FSceneComponentInstanceData
{
	GENERATED_BODY()

	FDroneTrailInstanceData() = default;
	FDroneTrailInstanceData(const UDroneTrailComponent* SourceComponent);
	virtual ~FDroneTrailInstanceData() override = default;

	virtual bool ContainsData() const override { return true; }
	virtual void ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase) override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

	UPROPERTY()
	TArray<TObjectPtr<UDroneTrailMeshComponent>> Finished;

	UPROPERTY()
	TObjectPtr<UDroneTrailMeshComponent> Head;

	FDroneTrailPath Path;
	TArray<FVector3f> HeadPoints;
	TArray<FDroneTrailRing> HeadRings;
	int32 FirstChanged = 0;
	bool bHeadHasStart = true;
	int32 NumPoints = 0;
};

/**
 * Leaves a glowing trail behind the drone wherever it flies, for the whole game. Put it on the
 * drone where the trail should come out.
 *
 * The trail is a thin tube through the points FDroneTrailPath picks, cut into pieces of up to
 * UDroneTrailMeshComponent::MaxRings rings. Only the newest piece changes as the drone flies;
 * the rest are finished and cost one draw each, so the trail can keep growing all game.
 */
UCLASS(ClassGroup = (WEM), meta = (BlueprintSpawnableComponent), hidecategories = (Activation, AssetUserData, Collision, Cooking, Navigation, Physics, Tags))
class WEM_2026_API UDroneTrailComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UDroneTrailComponent();

	/** The trail's colour. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drone Trail", meta = (HideAlphaChannel))
	FLinearColor Color = FLinearColor(0.05f, 0.45f, 1.0f);

	/** How brightly it shines. 1 is about as bright as a white wall in the current light, whatever the exposure; more glows and blooms. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drone Trail", meta = (ClampMin = "0.0", UIMax = "50.0"))
	float Glow = 10.0f;

	/** How thick the trail is. Far away it never gets thinner than a pixel or two, so it does not break up. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drone Trail", meta = (ClampMin = "0.1", UIMin = "0.5", UIMax = "50.0", Units = "cm"))
	float Width = 6.0f;

	/** How far the drawn trail may stray from where the drone really flew. Smaller follows tight turns more closely, with more points. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drone Trail|Advanced", meta = (ClampMin = "0.1", UIMax = "10.0", Units = "cm"))
	float Accuracy = 1.0f;

	/** Takes the parameters Color, Glow and Radius (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drone Trail|Advanced")
	TObjectPtr<UMaterialInterface> Material;

	/** Recolours the whole trail, the part already flown included. */
	UFUNCTION(BlueprintCallable, Category = "Drone Trail")
	void SetColor(FLinearColor NewColor);

	UFUNCTION(BlueprintCallable, Category = "Drone Trail")
	void SetGlow(float NewGlow);

	/** Rethickens the whole trail, the part already flown included. */
	UFUNCTION(BlueprintCallable, Category = "Drone Trail")
	void SetWidth(float NewWidth);

	/** Moves the trail's end to Location. Every tick does this with this component's own location. */
	void Advance(const FVector& Location);

	/**
	 * Lays a made-up flight of this many minutes at 60 fps into the trail, wandering within 40 m of
	 * here, to see and measure what a long game's trail costs. The console's wem.DroneTrail.Prefill
	 * does this for every drone that is playing. bRedrawOnce redraws the growing piece only at the
	 * end, which is what makes hours of it quick; the tests check it draws the same.
	 */
	void Prefill(float Minutes, bool bRedrawOnce = true);

	/** The trail's pieces, oldest first. The last is the one still growing. */
	TArray<UDroneTrailMeshComponent*> GetPieces() const;

	/** Points the trail has been given since play began. */
	int32 GetNumPoints() const { return NumPoints; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	friend struct FDroneTrailInstanceData;

	/** Takes over the trail a component before this one was drawing, in this one's colour and size. */
	void Adopt(const FDroneTrailInstanceData& Data);

	void AddPoint(const FVector3f& Point);
	void StartPiece();
	void FinishPiece();
	void Redraw();

	/** The ring of HeadPoints[Index], turned halfway between the path coming in and going out. */
	FDroneTrailRing MakePointRing(int32 Index) const;

	/** The rings after the last point: one where the drone is, and the rounded end just past it. */
	void AddTipRings(TArray<FDroneTrailRing>& Rings) const;

	/** Rings in the growing piece before HeadPoints' first: 1 for the rounded start of the whole trail, else 0. */
	int32 StartRings() const { return bHeadHasStart ? 1 : 0; }

	void ApplyLook();
	UMaterialInstanceDynamic* GetMaterialInstance();

	FDroneTrailPath Path;

	/** The growing piece's points. The first is also the last of the piece before, so the tube carries on without a seam. */
	TArray<FVector3f> HeadPoints;

	/** Their rings. The last point's depends on where the drone is now, so it changes until the next point comes. */
	TArray<FDroneTrailRing> HeadRings;

	/** The first of HeadPoints whose ring has to be worked out again. */
	int32 FirstChanged = 0;

	bool bHeadHasStart = true;
	int32 NumPoints = 0;
	TArray<FVector3f> NewPoints;

	/** Set while Prefill runs: the growing piece is redrawn once at the end. */
	bool bDeferRedraw = false;

	/** The pieces belong to the drone, so they go when it does. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UDroneTrailMeshComponent>> Finished;

	UPROPERTY(Transient)
	TObjectPtr<UDroneTrailMeshComponent> Head;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MaterialInstance;
};
