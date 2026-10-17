// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "HoverDronePawnBase.h"
#include "SPInterpolators.h"
#include "HoverDronePawn.generated.h"

class UInputMappingContext;

HOVERDRONE_API extern float DroneSpeedScalar;

UCLASS()
class HOVERDRONE_API AHoverDronePawn : public AHoverDronePawnBase
{
	GENERATED_BODY()

public:
	/** ctor */
	AHoverDronePawn(const FObjectInitializer& ObjectInitializer);

	// APawn interface
	virtual FRotator GetViewRotation() const override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;
	virtual void PawnClientRestart() override;
	virtual void PossessedBy(AController* NewController) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hover Drone Pawn|Tilt")
	bool bIsTiltingEnabled = true;

	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	FRotator GetTiltedDroneRotation(float DeltaTime);

	/** Returns drone's current height above the ground. */
	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	float GetAltitude() const;

	/** Returns true if this drone has auto-altitude on. */
	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	bool IsMaintainingConstantAltitude() const;

	/** Movement input handlers */
	virtual void MoveForward(float Val) override;
	virtual void MoveRight(float Val) override;
	void MoveUp(float Val);

	/** Turn by accelerating (i.e. drone's thrusters) */
	void TurnAccel(float Val);
	/** Look up/down by accelerating (i.e. drone's thrusters) */
	void LookUpAccel(float Val);

	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	int32 GetDroneSpeedIndex() const;

	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	void SetDroneSpeedIndex(int32 SpeedIndex);

	/** Returns drone speed to the default setting */
	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	void SetToDefaultDroneSpeedIndex();

	void ResetInterpolation();

protected:
	/** When true, speed can be changed by calls to IncreaseHoverDroneSpeed and DecreaseHoverDroneSpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hover Drone Pawn|Input")
	bool bAllowSpeedChange;

private:

	// override to ignore base class bindings
	virtual void MoveUp_World(float Val) override {};

	/** Input handler for lookat functionality */
	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	void BeginLookat();
	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	void EndLookat();

	/** Input handler for turbo auto-altitude */
	void ToggleFixedHeight();

	/** Returns drone's current height above the ground. */
	UFUNCTION(BlueprintCallable, Category = HoverDrone)
	void SetAllowSpeedChange(bool bOnOff) { bAllowSpeedChange = bOnOff; };

	/** For interpolating the tilt. */
	FRotator LastTiltedDroneRot;

	/** Input handler for speed adjusting */
	void IncreaseHoverDroneSpeed();
	void DecreaseHoverDroneSpeed();

protected:

	UPROPERTY(EditDefaultsOnly, Category = "Hover Drone Pawn|Input")
	TObjectPtr<class UInputMappingContext> InputMappingContext;

	UPROPERTY(EditDefaultsOnly, Category = "Hover Drone Pawn|Input")
	TObjectPtr<class UInputAction> MoveAction;

	UPROPERTY(EditDefaultsOnly, Category = "Hover Drone Pawn|Input")
	TObjectPtr<class UInputAction> LookAction;

	UPROPERTY(EditDefaultsOnly, Category = "Hover Drone Pawn|Input")
	TObjectPtr<class UInputAction> ChangeAltitudeAction;

	UPROPERTY(EditDefaultsOnly, Category = "Hover Drone Pawn|Input")
	TObjectPtr<class UInputAction> ChangeSpeedAction;

	UPROPERTY(EditDefaultsOnly, Category = "Hover Drone Pawn|Input")
	int32 InputMappingPriority = 1;

	/** If true, the Movement input moves the drone in the XY plane only (drone-style), regardless of view pitch. Otherwise, drone Movement input is relative to the view transform (airplane-style movement) */
	UPROPERTY(EditDefaultsOnly, Category = "Hover Drone Pawn|Input")
	bool bConstrainMovementToXYPlane = true;

	void MoveActionBinding(const struct FInputActionValue& ActionValue);
	void LookActionBinding(const struct FInputActionValue& ActionValue);
	void ChangeAltitudeActionBinding(const struct FInputActionValue& ActionValue);
	void ChangeSpeedActionBinding(const struct FInputActionValue& ActionValue);


	/** How quickly/aggressively to interp into the tilted position. */
	UPROPERTY(EditAnywhere)
	FIIRInterpolatorRotator DroneTiltInterpolator = FIIRInterpolatorRotator(8.f);

	/** The drone's up vector during neutral hovering. The magnitude determines resistance to tilt when moving. */
	UPROPERTY(EditAnywhere)
	FVector TiltUpVector;

	UPROPERTY(EditAnywhere)
	bool bEnableTiltLimits = false;

	UPROPERTY(EditAnywhere)
	FRotator TiltLimits;

};