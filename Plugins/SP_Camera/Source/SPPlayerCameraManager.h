// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/PlayerCameraManager.h"
#include "SPCameraMode.h"

#include "SPPlayerCameraManager.generated.h"

class ASPPlayerController;
class UCineCameraComponent;

/**
 * Enumerated equivalents of default camera modes for ease of use
 */
UENUM(BlueprintType)
enum class EDebugCameraStyle : uint8
{
	None,
	Fixed,
	ThirdPerson,
	FreeCam,
	FreeCam_Default,
	FirstPerson,
};


/**
 * Representations of active cameras that the manager is currently blending between
 */
USTRUCT()
struct FActiveSPCamera
{
	GENERATED_BODY()

public:
	/** Pointer to camera instance */
	UPROPERTY()
	USPCameraMode* Camera = nullptr;

	/** Pointer to current view target */
	UPROPERTY()
	AActor* ViewTarget = nullptr;

	/** Interpolates towards one for incoming camera mode, towards zero for the other ones. */
	float TransitionAlpha = 0.f;

	/** Update rate for transition */
	float TransitionUpdateRate = 0.f;

	/** Current blend weight in blend stack */
	float BlendWeight = 0.f;

	/** SPCameraModeInstance associated with this active camera object */
	int32 InstanceIndex = INDEX_NONE;

	/** Cache of camera previous view info */
	FMinimalViewInfo LastPOV;

	/** If true, view info will be locked during camera transitions involving this camera */
	bool bLockOutgoingPOV = false;
};

/** Instances of camera modes that can be used/reused to support active cameras */
USTRUCT(BlueprintType)
struct FSPCameraModeInstance
{
	GENERATED_BODY()

public:

	/** Camera mode class associated with the instance */
	UPROPERTY()
	TSubclassOf<class USPCameraMode> CameraModeClass;

	/** View target that the instance is focusing */
	UPROPERTY()
	AActor* ViewTarget = nullptr;

	/** Camera mode object associated with the instance */
	UPROPERTY(BlueprintReadOnly, EditInstanceOnly)
	USPCameraMode* CameraMode = nullptr;

	/** Cine cam component associated with the instance */
	UPROPERTY(BlueprintReadOnly, EditInstanceOnly)
	UCineCameraComponent* CineCameraComponent = nullptr;

	/** Triggers an update on the underlying camera mode associated with the instance */
	void UpdateCamera(float DeltaTime, FTViewTarget& OutVT);
};


/**
 * Representations of active cameras that the manager is currently blending between
 */
UCLASS()
class ASPPlayerCameraManager : public APlayerCameraManager
{
	GENERATED_BODY()

public:
	ASPPlayerCameraManager(const FObjectInitializer& ObjectInitializer);

	//~ Begin APlayerCameraManager Interface
	virtual void UpdateViewTarget(struct FTViewTarget& OutVT, float DeltaTime) override;
	virtual void DisplayDebug(class UCanvas* Canvas, const FDebugDisplayInfo& DebugDisplay, float& YL, float& YPos) override;
	//~ End APlayerCameraManager Interface

	/** Returns the camera mode that is currently in use by the Camera Manager */
	UFUNCTION(BlueprintCallable)
	USPCameraMode* GetCurrentCameraMode();

	/** Will create if it doesn't find one.  Returns index into CameraModeInstances. */
	int32 GetBestCameraMode(AActor* ViewTarget);

	/** Returns the view info that the camera on the top of our camera blend stack is transitioning to */
	FMinimalViewInfo GetTransitionGoalPOV() const
	{
		return TransitionGoalPOV;
	}

	/** Applies a premade camera style for debugging purposes */
	UFUNCTION(BlueprintCallable, Category = Camera)
	void SetDebugCameraStyle(EDebugCameraStyle NewDebugCameraStyle);

	/** Sets view pitch limits to the values passed in */
	UFUNCTION(BlueprintCallable)
	void SetViewPitchLimits(float MinPitch, float MaxPitch);

	/** Sets view pitch limits to values that were originally set in class defaults */
	UFUNCTION(BlueprintCallable)
	void ResetViewPitchLimits();

	/** Pelvis Z height, in component space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BasePelvisRelativeZ;

	/** Pelvis bone name for characters acting as camera view targets */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FName PelvisBoneName;

	//~ Begin Alternate Camera Mode Support
	/** Sets camera mode, potential view target, and transition time when alternate camera mode is enabled */
	UFUNCTION(BlueprintCallable)
	void ConfigureAlternateCamera(TSubclassOf<USPCameraMode> NewAltCameraMode, AActor* NewAltViewTarget, float NewAltCameraTransitionTime);

	/** Whether to use the current alternate camera settings or not */
	UFUNCTION(BlueprintCallable)
	void SetUsingAlternateCamera(bool bNewUsingAltCamera);

	/** Clears any alternate camera settings stored*/
	UFUNCTION(BlueprintCallable)
	void ClearAlternateCamera();

	UFUNCTION(BlueprintCallable)
	bool IsUsingAlternateCamera() const
	{
		return bUsingAltCameraMode;
	}

	bool HasAlternateCameraAvailable() const
	{
		return AltCameraMode != nullptr;
	}

	UFUNCTION(BlueprintCallable)
	void SkipBlends();

	bool bSkipBlendsOnNextUpdate = false;

	UFUNCTION(BlueprintCallable)
	void SkipNextInterpolation() { bSkipNextInterpolation = true; };

	bool bSkipNextInterpolation = false;

	/** If true, the alternate camera mode is active */
	bool bUsingAltCameraMode;

	/** Camera mode to use when alt camera is enabled */
	TSubclassOf<USPCameraMode> AltCameraMode;

	/** View target to focus when using alt camera mode */
	UPROPERTY(transient)
	AActor* AltViewTarget;

	/** Time to transition to alt camera mode */
	float AltCameraTransitionTime;

	/** View target to return to when leaving the alt camera mode */
	UPROPERTY(transient)
	AActor* SavedMainViewTarget;

	UFUNCTION(BlueprintCallable)
	void StartAmbientCameraShake();
	
	UFUNCTION(BlueprintCallable)
	void StopAmbientCameraShake(bool bImmediate);

protected:
	/** Begins a transition from the main camera to currently configured alt camera settings */
	void TransitionToAltCamera();

	/** Begins a transition from the alt camera to currently configured main camera settings*/
	void TransitionFromAltCamera();

	//~ End Alternate Camera Mode Support

	/** Returns camera mode that is deemed appropriate for the current view target */
	UFUNCTION(BlueprintNativeEvent, Category = Camera)
	TSubclassOf<USPCameraMode> GetCameraClassForCharacter(const AActor* InViewTarget) const;
	TSubclassOf<USPCameraMode> GetCameraClassForCharacter_Implementation(const AActor* InViewTarget) const;

	/** Update individual camera modes that correspond with the index passed in */
	void UpdateCameraInStack(int32 StackIdx, float DeltaTime, FTViewTarget& OutVT);

	/** Returns transition time determined by the camera modes we are transitioning between */
	float GetModeTransitionTime(USPCameraMode* ToMode) const;

	/** Stack of active camera the manager will attempt to blend between */
	UPROPERTY(Transient)
	TArray<struct FActiveSPCamera> CameraBlendStack;

	/** List of available camera mode instances */
	UPROPERTY(Transient, EditInstanceOnly, BlueprintReadOnly)
	TArray<FSPCameraModeInstance> CameraModeInstances;

	/** Attempts to cast and return the camera manager's owning SP Player Controller */
	APlayerController* GetOwningPC() const;

private:
	/** Determines the best camera mode for a potential view target */
	TSubclassOf<USPCameraMode> DetermineBestCameraClass(AActor const* ViewTarget) const;

	/** Attempts to find existing camera mode instance to set the new view target, and creates a new camera mode instance if no existing candidates are found */
	int32 FindOrCreateCameraModeInstance(TSubclassOf<USPCameraMode> CameraModeClass, AActor* InViewTarget);

	/** Removes any existing camera mode instances that don't currently have a view target set */
	void CleanUpOutdatedCameraModeInstances();

	/** The destination POV of an active transition */
	FMinimalViewInfo TransitionGoalPOV;

	/** Cache of starting min pitch limit value */
	float DefaultMinPitchLimit = 0.0f;

	/** Cache of starting max pitch limit value */
	float DefaultMaxPitchLimit = 0.0f;
};

