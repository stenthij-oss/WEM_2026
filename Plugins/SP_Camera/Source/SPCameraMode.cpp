// Copyright Epic Games, Inc. All Rights Reserved.

#include "SPCameraMode.h"

#include "Camera/CameraShakeBase.h"
#include "DrawDebugHelpers.h"

#include "SPPlayerCameraManager.h"
#include "GameFramework/PlayerController.h"

USPCameraMode::USPCameraMode()
	: TransitionInTime(0.5f)
{
	TransitionParams.BlendFunction = VTBlend_Cubic;
}

APlayerController* USPCameraMode::GetOwningPC() const
{
	return PlayerCamera ? Cast<APlayerController>(PlayerCamera->PCOwner) : nullptr;
}

void USPCameraMode::SkipNextInterpolation()
{
	bSkipNextInterpolation = true;
	ShakeScaleInterpolator.Reset();
}

void USPCameraMode::OnBecomeActive(AActor* ViewTarget, USPCameraMode* PreviouslyActiveMode, bool bAlreadyInStack)
{
	if (PlayerCamera != nullptr)
	{
		if (CameraShakeClass != nullptr)
		{
			StartAmbientCameraShake();
		}

		if (bOverrideViewPitchMinAndMax)
		{
			PlayerCamera->SetViewPitchLimits(ViewPitchMinOverride, ViewPitchMaxOverride);
		}
		else
		{
			PlayerCamera->ResetViewPitchLimits();
		}
	}

	bIsActive = true;
}

void USPCameraMode::OnBecomeInactive(AActor* ViewTarget, USPCameraMode* NewActiveMode)
{
	StopAmbientCameraShake(false);

	bIsActive = false;
}

float USPCameraMode::GetTransitionTime() const
{
	return TransitionInTime;
}

void USPCameraMode::ApplyCineCamSettings(FTViewTarget& OutVT, UCineCameraComponent* CineCamComp, float DeltaTime)
{
	if (CineCamComp)
	{
		// put cine cam component at final camera transform, then evaluate it
		CineCamComp->SetWorldTransform(LastCameraToWorld);
		if (bUseCineCamSettings)
		{
			CineCamComp->SetCurrentFocalLength(CineCam_CurrentFocalLength);
			const float FocusDistance = GetDesiredFocusDistance(OutVT.Target, LastCameraToWorld) + CineCam_FocusDistanceAdjustment;
			CineCamComp->FocusSettings.ManualFocusDistance = FocusDistance;
			CineCamComp->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			CineCamComp->CurrentAperture = CineCam_CurrentAperture;
		}
		else
		{
			CineCamComp->SetFieldOfView(OutVT.POV.FOV);
			CineCamComp->FocusSettings.FocusMethod = ECameraFocusMethod::DoNotOverride;
			CineCamComp->CurrentAperture = 22.f;
		}

		CineCamComp->GetCameraView(DeltaTime, OutVT.POV);

		CineCam_DisplayOnly_FOV = OutVT.POV.FOV;
	}
}

float USPCameraMode::GetDesiredFocusDistance(AActor* ViewTarget, const FTransform& ViewToWorld) const
{
	if (bUseCustomFocusDistance)
	{
		const float Dist = GetCustomFocusDistance(ViewTarget, ViewToWorld);
		if (Dist > 0.f)
		{
			return Dist;
		}
	}

	FVector FocusPoint(0.f);
	if (ViewTarget)
	{
		FocusPoint = ViewTarget->GetActorLocation();
	}

	return (FocusPoint - ViewToWorld.GetLocation()).Size();
}

void USPCameraMode::UpdateCamera(class AActor* ViewTarget, UCineCameraComponent* CineCamComp, float DeltaTime, FTViewTarget& OutVT)
{
	if (ViewTarget && bUseViewTargetCameraComponent)
	{
		if (UCameraComponent* const Cam = ViewTarget->FindComponentByClass<UCameraComponent>())
		{
			Cam->GetCameraView(DeltaTime, OutVT.POV);
		}
	}

	if (CameraShakeInstance)
	{
		// cover all the else clauses below
		CameraShakeInstance->ShakeScale = 1.f;

		if (bScaleShakeWithViewTargetVelocity)
		{
			if (ViewTarget)
			{
				const float Speed = ViewTarget->GetVelocity().Size();
				const float GoalScale = FMath::GetMappedRangeValueClamped(ShakeScaling_SpeedRange, ShakeScaling_ScaleRange, Speed);
				CameraShakeInstance->ShakeScale = ShakeScaleInterpolator.Eval(GoalScale, DeltaTime);

				if (bDrawDebugShake)
				{
#if ENABLE_DRAW_DEBUG
					::FlushDebugStrings(ViewTarget->GetWorld());
					::DrawDebugString(ViewTarget->GetWorld(), ViewTarget->GetActorLocation() + FVector(0, 0, 60.f), FString::Printf(TEXT("%f"), CameraShakeInstance->ShakeScale), nullptr, FColor::Yellow);
#endif
				}
			}
		}
	}
}


void USPCameraMode::StartAmbientCameraShake()
{
	if (CameraShakeInstance == nullptr)
	{
		if (PlayerCamera && (CameraShakeClass != nullptr))
		{
			CameraShakeInstance = PlayerCamera->StartCameraShake(CameraShakeClass, 1.f);
		}
	}
}
void USPCameraMode::StopAmbientCameraShake(bool bImmediate)
{
	if (CameraShakeInstance)
	{
		PlayerCamera->StopCameraShake(CameraShakeInstance, bImmediate);
		CameraShakeInstance = nullptr;
	}
}

bool USPCameraMode::ShouldLockOutgoingPOV() const
{
	return TransitionParams.bLockOutgoing;
}

void USPCameraMode::ResetToDefaultSettings_Implementation()
{
	USPCameraMode const * const ThisCDO = CastChecked<USPCameraMode>(GetClass()->GetDefaultObject());
	
	FOV = ThisCDO->FOV;
	bUseCineCamSettings = ThisCDO->bUseCineCamSettings;
	bUseCineCam = ThisCDO->bUseCineCam;
	CineCam_CurrentFocalLength = ThisCDO->CineCam_CurrentFocalLength;
	CineCam_CurrentAperture = ThisCDO->CineCam_CurrentAperture;
	CineCam_FocusDistanceAdjustment = ThisCDO->CineCam_FocusDistanceAdjustment;
	bUseCustomFocusDistance = ThisCDO->bUseCustomFocusDistance;
	TransitionInTime = ThisCDO->TransitionInTime;
	TransitionParams = ThisCDO->TransitionParams;
	ShakeScaling_SpeedRange  = ThisCDO->ShakeScaling_SpeedRange;
	ShakeScaling_ScaleRange = ThisCDO->ShakeScaling_ScaleRange;
	ShakeScaleInterpolator = ThisCDO->ShakeScaleInterpolator;
}