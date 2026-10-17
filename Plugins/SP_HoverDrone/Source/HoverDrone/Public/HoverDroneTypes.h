// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include "HoverDroneTypes.generated.h"

USTRUCT(BlueprintType)
struct FDroneSpeedParameters
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, Category = "HoverDrone")
	float LinearAccelScale;

	UPROPERTY(EditAnywhere, Category = "HoverDrone")
	float LinearDecelScale;
	 
	UPROPERTY(EditAnywhere, Category = "HoverDrone")
	float RotAccelScale;

	UPROPERTY(EditAnywhere, Category = "HoverDrone")
	float RotDecelScale;

	UPROPERTY(EditAnywhere, Category = "HoverDrone")
	float MaxLinearSpeedScale;

	UPROPERTY(EditAnywhere, Category = "HoverDrone")
	float MaxRotSpeedScale;

	UPROPERTY(EditAnywhere, Category = "HoverDrone")
	float HoverThrustScale;

	FDroneSpeedParameters(float InLinearAccelScale, float InLinearDecelScale, float InRotAccelScale, float InRotDecelScale, float InMaxLinearSpeedScale, float InMaxRotSpeedScale, float InHoverThrustScale)
		: LinearAccelScale(InLinearAccelScale)
		, LinearDecelScale(InLinearDecelScale)
		, RotAccelScale(InRotAccelScale)
		, RotDecelScale(InRotDecelScale)
		, MaxLinearSpeedScale(InMaxLinearSpeedScale)
		, MaxRotSpeedScale(InMaxRotSpeedScale)
		, HoverThrustScale(InHoverThrustScale)
	{}

	FDroneSpeedParameters(float Scales)
		: LinearAccelScale(Scales)
		, LinearDecelScale(Scales)
		, RotAccelScale(Scales)
		, RotDecelScale(Scales)
		, MaxLinearSpeedScale(Scales)
		, MaxRotSpeedScale(Scales)
		, HoverThrustScale(Scales)
	{}

	FDroneSpeedParameters():
		FDroneSpeedParameters(1.f)
	{}
};