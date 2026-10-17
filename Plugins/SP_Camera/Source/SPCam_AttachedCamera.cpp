// Copyright Epic Games, Inc. All Rights Reserved.

#include "SPCam_AttachedCamera.h"
#include "SPPlayerCameraManager.h"

USPCam_AttachedCamera::USPCam_AttachedCamera()
{
	PivotPitchLimits = FVector2D(-90.f, 90.f);
	PivotYawLimits = FVector2D(-180.f, 180.f);
}

UCameraComponent* USPCam_AttachedCamera::ChooseViewCameraComponent_Implementation(AActor* ViewTarget) const
{
	if (ViewTarget)
	{
		// by default, just find the first one and use that
		UCameraComponent* const CamComp = ViewTarget->FindComponentByClass<UCameraComponent>();
		return CamComp;
	}

	return nullptr;
}

void USPCam_AttachedCamera::UpdateCamera(class AActor* ViewTarget, UCineCameraComponent* CineCamComp, float DeltaTime, FTViewTarget& OutVT)
{
	// let super do any updates it wants (e.g. camera shakes), but we will fully determine the POV below
	Super::UpdateCamera(ViewTarget, CineCamComp, DeltaTime, OutVT);

	UWorld const* const World = ViewTarget ? ViewTarget->GetWorld() : nullptr;
	if (World)
	{
		// if the pawn is pending destroy, the position of the pawn gets reset and camera will be teleported to weird positions.
		// so just use the old FOV without any update
		if (!IsValid(ViewTarget) || !PlayerCamera || !PlayerCamera->PCOwner)
		{
			return;
		}

		UCameraComponent* const CamComp = ChooseViewCameraComponent(ViewTarget);
		if (CamComp != LastViewCameraComponent)
		{
			SkipNextInterpolation();
		}

		if (CamComp)
		{
			CamComp->GetCameraView(DeltaTime, OutVT.POV);
		}

		// Optional smoothing
		{
			// smooth rotation	
			if (bSkipNextInterpolation)
			{
				LocInterpolator.Reset();
				RotInterpolator.Reset();
				ExtraLocZInterpolator.Reset();
			}
			else
			{
				OutVT.POV.Location = LocInterpolator.Eval(OutVT.POV.Location, DeltaTime);;
				OutVT.POV.Location.Z = ExtraLocZInterpolator.Eval(OutVT.POV.Location.Z, DeltaTime);
				OutVT.POV.Rotation = RotInterpolator.Eval(OutVT.POV.Rotation, DeltaTime);
			}
		}

		if (bAllowPlayerRotationControl)
		{
			FTransform BaseToWorld(OutVT.POV.Rotation, OutVT.POV.Location);
			FTransform PlayerToBase;
			AController* const PC = Cast<APlayerController>(PlayerCamera->PCOwner);
			if (PC != nullptr)
			{
				if (bSkipNextInterpolation)
				{
					PlayerControlRotInterpolator.Reset();
					PC->SetControlRotation(FRotator::ZeroRotator);
				}
				else
				{
					PlayerToBase.SetRotation(PC->GetControlRotation().Quaternion());
				}
			}

			// apply view limits
			FRotator PlayerToBaseRot = PlayerToBase.Rotator();
			PlayerCamera->LimitViewPitch(PlayerToBaseRot, PivotPitchLimits.X, PivotPitchLimits.Y);
			PlayerCamera->LimitViewYaw(PlayerToBaseRot, PivotYawLimits.X, PivotYawLimits.Y);

			// set control rotation back to pivot rotation, so controls keep making sense
			PlayerCamera->PCOwner->SetControlRotation(PlayerToBaseRot);

			FTransform PlayerToWorld = PlayerToBase * BaseToWorld;
			OutVT.POV.Location = PlayerToWorld.GetTranslation();
			OutVT.POV.Rotation = PlayerControlRotInterpolator.Eval(PlayerToWorld.Rotator(), DeltaTime);
		}

		PlayerCamera->ApplyCameraModifiers(DeltaTime, OutVT.POV);

		LastViewCameraComponent = CamComp;
	}

	bSkipNextInterpolation = false;
}

void USPCam_AttachedCamera::OnBecomeActive(AActor* ViewTarget, USPCameraMode* PreviouslyActiveMode, bool bAlreadyInStack)
{
	Super::OnBecomeActive(ViewTarget, PreviouslyActiveMode, bAlreadyInStack);

}

void USPCam_AttachedCamera::SkipNextInterpolation()
{
	Super::SkipNextInterpolation();

	LocInterpolator.Reset();
	RotInterpolator.Reset();
	PlayerControlRotInterpolator.Reset();
}
