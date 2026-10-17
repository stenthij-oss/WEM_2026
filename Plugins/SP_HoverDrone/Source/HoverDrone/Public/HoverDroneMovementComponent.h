// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/SpectatorPawnMovement.h"
#include "HoverDroneTypes.h"
#include "SPInterpolators.h"
#include "HoverDroneMovementComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FMaxAllowedSpeedUpdated);

UENUM()
enum class EHoverDroneDebug
{
	Off = 0,
	Position = 1 << 0,
	Velocity = 1 << 2,
	RotationalVelocity = 1 << 3,
	Altitude = 1 << 4,
	ForceFacing = 1 << 5,
	FOV = 1 << 6,
	All = Position | Velocity | RotationalVelocity | Altitude | ForceFacing | FOV
};

ENUM_CLASS_FLAGS(EHoverDroneDebug);

UCLASS()
class HOVERDRONE_API UHoverDroneMovementComponent : public USpectatorPawnMovement
{
	GENERATED_BODY()

public:
	/** ctor */
	UHoverDroneMovementComponent(const FObjectInitializer& ObjectInitializer);

	// UActorComponent interface
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void InitializeComponent() override;

	// CharacterMovementComponent interface
	virtual void OnTeleported() override;

	void AddRotationInput(FRotator RotInput);
	void AddDirectRotationInput(FRotator RotInput);

	void ForceFacing(FVector Location);
	void ForceFacingFollowedPlayer();
	void StopForceFacing();
	bool IsForceFacingFollowedPlayer() const { return bForceFacingFollowedPlayer; };

	void TetherToFollowedPlayer();
	void StopTether();
	bool IsTetheredToFollowedPlayer() const { return bTetherToFollowedPlayer; };

	/** Auto-altitude controls */
	void SetMaintainHoverHeight(bool bShouldMaintainHeight);
	bool GetMaintainHoverHeight() const;
	void ResetDesiredAltitude();

	/** Returns height above the ground. */
	float GetAltitude() const { return CurrentAltitude; };

	/** Turbo controls */
	void SetTurbo(bool bNewTurbo) { bTurbo = bNewTurbo; };
	bool IsTurbo() { return bTurbo; };

	/** Call when switch to this to do internal setup */
	void Init();

	FVector MeasuredVelocity;

	int32 GetDroneSpeedIndex() const { return DroneSpeedParamIndex; };
	void SetDroneSpeedIndex(int32 SpeedIndex);

	/** Snaps any interpolations to the goal position, useful on cuts. */
	void ResetInterpolation() { bResetInterpolation = true; };			

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = HoverDroneMovement)
	int32 MaxAllowedSpeedIndex;

	UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = HoverDroneMovement)
	FMaxAllowedSpeedUpdated MaxAllowedSpeedUpdated;

	UFUNCTION(BlueprintCallable, Category = HoverDroneMovement)
	void SetCurrentFOV(float NewFOV)
	{
		CurrentFOV = NewFOV;
	}

	UFUNCTION(BlueprintCallable, Category = HoverDroneMovement)
	void AddVelocity(FVector VelocityImpulse);

	UFUNCTION(BlueprintCallable, Category = HoverDroneMovement)
	void AddRotationalVelocity(FRotator RotationalVel);

protected:
	FRotator RotationInput;
	FRotator RotVelocity;
	FRotator DirectRotationInput;
	
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	FRotator LastRotationInput;
	FRotator LastDirectRotationInput;
#endif

	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float DirectRotationInputYawScale;

	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float DirectRotationInputPitchScale;

	FRotator DirectRotationInputGoalRotation;
	
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float DirectRotationInputInterpSpeed;

	UPROPERTY(EditAnywhere, Category = "HoverDroneMovement|FOVScaling")
	bool bUseFOVScaling = false;

	/** Camera FOV ranges the drone should expect to deal with*/
	UPROPERTY(EditAnywhere, Category = "HoverDroneMovement|FOVScaling", meta = (EditCondition = bUseFOVScaling))
	FVector2D CameraFovRange = FVector2D(90.0f, 90.0f);

	/** Input Value scaling that is mapped to the CameraFOVRange. Affects how intense inputs can be at certain FOV values.*/
	UPROPERTY(EditAnywhere, Category = "HoverDroneMovement|FOVScaling", meta = (EditCondition = bUseFOVScaling))
	FVector2D InputFovScaleRange = FVector2D(1.0f, 1.0f);
	
	/** Rotational acceleration when turning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = HoverDroneMovement)
	float RotAcceleration;

	/** Rotational deceleration when not turning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = HoverDroneMovement)
	float RotDeceleration;

	/** Maximum rotational speed, pitch */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = HoverDroneMovement)
	float MaxPitchRotSpeed;

	/** Maximum rotational speed, yaw */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = HoverDroneMovement)
	float MaxYawRotSpeed;

	/** Controls how much Deceleration to apply based on velocity. At this velocity, air friction will be 100% of Deceleration. Uncapped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = HoverDroneMovement)
	float FullAirFrictionVelocity;

	/** Maximum rotational speed, yaw */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = HoverDroneMovement)
	FVector MovementAccelFactor;

	virtual void ApplyControlInputToVelocity(float DeltaTime) override;
	void ApplyControlInputToRotation(float DeltaTime);


	// Experimental new flight model
	void ApplyControlInputToVelocity_NewModel(float DeltaTime);
	void ApplyControlInputToRotation_NewModel(float DeltaTime);
	void UpdateAutoHover();

	FAccelerationInterpolatorVector LinearVelInterpolator;
	FAccelerationInterpolatorFloat YawVelInterpolator;
	FAccelerationInterpolatorFloat PitchVelInterpolator;

	FIIRInterpolatorVector LinearVelInterpolator_IIR;
	FIIRInterpolatorFloat YawVelInterpolator_IIR;
	FIIRInterpolatorFloat PitchVelInterpolator_IIR;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	TArray<FDroneSpeedParameters> DroneSpeedParameters;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	TArray<FDroneSpeedParameters> DroneSpeedParameters_NewModel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	float Acceleration_NewModel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	float Deceleration_NewModel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	float MaxSpeed_NewModel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	float MaxYawRotSpeed_NewModel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	float MaxPitchRotSpeed_NewModel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	float RotAcceleration_NewModel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	float RotDeceleration_NewModel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = NewModel)
	bool bUseNewDroneFlightModel = false;


private:

	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MinAirFriction;

	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	int32 DroneSpeedParamIndex;

	/** Max timestep to simulate in one step. Frames longer than this will do multiple simulations. */
	float MaxSimulationTimestep;

	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MinSpeedHeight;
	
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MaxSpeedHeight;

	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MaxSpeedHeightMultiplier;

	/** Valid Pitch range */
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MinPitch;
	
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MaxPitch;

	/** Current distance to the ground. */
	float CurrentAltitude;

	/** Height limit for the drone. */
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float DroneMaxAltitude;

	/** True if we should automatically apply impulses in an attempt to maintain a fixed hover height */
	uint8 bMaintainHoverHeight : 1;
	float DesiredHoverHeight;

	/** Set hover height must exceed this value. */
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MinHoverHeight;

	/** Within this absolute distance of DesiredHoverHeight, we are considered to be at the desired height and making no corrections. */
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MaintainHoverHeightTolerance;
	
	/** How far ahead, in seconds, to check for and respond to upcoming ground height changes. */
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float MaintainHoverHeightPredictionTime;

	FVector ForcedFacingLocation;
	float ForceFacingInterpInPct = 0.f;
	
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float ForceFacingInterpInTime;

	FVector LastFollowedPlayerFacingLoc;

public:
	/** true to simulate rotation with rot acceleration, false to ignore rotation. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = HoverDroneMovement)
	uint32 bSimulateRotation : 1;

	/** true to ignore drone control and speed limit volumes */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = HoverDroneMovement)
	uint32 bIgnoreDroneLimiters : 1;

private:
	/** True if currently forced to face ForcedFacingLocation, false otherwise. */
	uint32 bForceFacingLocation : 1;

	/** True if currently in turbo mode, false otherwise. */
	uint32 bTurbo : 1;

	/** True if currently forced to face the controller's followed player */
	uint32 bForceFacingFollowedPlayer : 1;

	/** True if currently forced to translate along with the controller's followed player */
	uint32 bTetherToFollowedPlayer : 1;

	uint32 bResetInterpolation : 1;

	FVector FollowedActorLastPosition;
	FVector FollowedActorSmoothedPosition;
	
	UPROPERTY(EditAnywhere, Category = HoverDroneMovement)
	float FollowedActorPositionInterpSpeed;
	
	/** Actor-space offset for the actual point to face when bForceFacingFollowedPlayer */
	FVector ForceFacingPlayerLocalOffset;

	float MeasureAltitude(FVector Location) const;

	float GetInputFOVScale() const;

	/**FOV the movement component is currently scaling input for */
	float CurrentFOV = 90.0f;

	void UpdatedMaxAllowedSpeed(int32 NewMaxAllowedSpeed);

protected:
	FVector PendingVelocityToAdd = FVector::ZeroVector;
	FRotator PendingRotVelocityToAdd = FRotator::ZeroRotator;
	FRotator LastForceFacingRotVelocity = FRotator::ZeroRotator;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	
	virtual void DrawDebug(class UCanvas* Canvas, float& YL, float& YPos);

	void ShowDebugInfo(class AHUD* HUD, class UCanvas* Canvas, const class FDebugDisplayInfo& DisplayInfo, float& YL, float& YPos);

	// Note: This isn't exposed anywhere currently, but can be flipped on with the debugger.
	//			Typically, this isn't necessary to mess with as the drone is fairly stable
	//			at this point.
	EHoverDroneDebug DebugFlags = EHoverDroneDebug::Off;

#endif
};





