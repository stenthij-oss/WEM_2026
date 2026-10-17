// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SPCameraMode.h"
#include "SPInterpolators.h"

#include "SPCam_AttachedCamera.generated.h"

/**
 * For viewing through a selected cameracomponent of the ViewTarget
 */
UCLASS(Blueprintable)
class USPCam_AttachedCamera : public USPCameraMode
{
	GENERATED_BODY()
	
public:
	USPCam_AttachedCamera();

	//~ Begin USPCameraMode Interface
	virtual void UpdateCamera(class AActor* ViewTarget, UCineCameraComponent* CineCamComp, float DeltaTime, struct FTViewTarget& OutVT) override;
	virtual void OnBecomeActive(AActor* ViewTarget, USPCameraMode* PreviouslyActiveMode, bool bAlreadyInStack) override;
	virtual void SkipNextInterpolation() override;
	//~ End USPCameraModeInterface

	UFUNCTION(BlueprintNativeEvent, Category = Camera)
	UCameraComponent* ChooseViewCameraComponent(AActor* ViewTarget) const;
	UCameraComponent* ChooseViewCameraComponent_Implementation(AActor* ViewTarget) const;

protected:
	/** Interpolator for smooth changes to the camera pivot's location in world space. Note: For very fast moving objects you may want to set this to 0,0 for instant pivot updates */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CameraSettings")
	FDoubleIIRInterpolatorVector LocInterpolator = FDoubleIIRInterpolatorVector(4.f, 12.f);

	/** Interpolator for smooth changes to the camera pivot's rotation in world space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CameraSettings")
	FDoubleIIRInterpolatorRotator RotInterpolator = FDoubleIIRInterpolatorRotator(4.f, 7.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CameraSettings")
	bool bAllowPlayerRotationControl = false;
	
	/** Interpolator for smooth changes to the player's control rot -- applied in attached camera's local space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CameraSettings")
	FDoubleIIRInterpolatorRotator PlayerControlRotInterpolator = FDoubleIIRInterpolatorRotator(8.f, 12.f);

	/** Applied after LocInterpolator, but only to the Z component of the location. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CameraSettings")
	FIIRInterpolatorFloat ExtraLocZInterpolator = FIIRInterpolatorFloat(0.f);

	
	/** Min and Max pitch thresholds for the camera pivot, in degrees  */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CameraSettings")
	FVector2D PivotPitchLimits;

	/** Min and Max yaw thresholds for the camera pivot in degrees */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CameraSettings")
	FVector2D PivotYawLimits;

	UPROPERTY(transient)
	UCameraComponent* LastViewCameraComponent;
};
