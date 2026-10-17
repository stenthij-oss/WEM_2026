// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SPInterpolators.h"

#include "SPInterpolatorsBPLibrary.generated.h"


/**
 * Collection of blueprint utility functions for SPInterpolators.
 */
UCLASS()
class USPInterpolatorsBPLibrary: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	
	//////////////////////////////////////////////////////////////////////////
	// Interpolators
	// Ability to eval the interpolator UStructs

	// Acceleration - Float
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static float EvalAccelInterpolatorFloat(UPARAM(ref) FAccelerationInterpolatorFloat& Interpolator, float NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetAccelInterpolatorFloat(UPARAM(ref) FAccelerationInterpolatorFloat& Interpolator);


	// Acceleration - Vector
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FVector EvalAccelInterpolatorVector(UPARAM(ref) FAccelerationInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetAccelInterpolatorVector(UPARAM(ref) FAccelerationInterpolatorVector& Interpolator);


	// Acceleration - Rotator
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FRotator EvalAccelInterpolatorRotator(UPARAM(ref) FAccelerationInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetAccelInterpolatorRotator(UPARAM(ref) FAccelerationInterpolatorRotator& Interpolator);


	// IIR - Float
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static float EvalIIRInterpolatorFloat(UPARAM(ref) FIIRInterpolatorFloat& Interpolator, float NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetIIRInterpolatorFloat(UPARAM(ref) FIIRInterpolatorFloat& Interpolator);


	// IIR - Vector
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FVector EvalIIRInterpolatorVector(UPARAM(ref) FIIRInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetIIRInterpolatorVector(UPARAM(ref) FIIRInterpolatorVector& Interpolator);


	// IIR - Rotator
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FRotator EvalIIRInterpolatorRotator(UPARAM(ref) FIIRInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetIIRInterpolatorRotator(UPARAM(ref) FIIRInterpolatorRotator& Interpolator);


	// Double IIR - Vector
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FVector EvalDoubleIIRInterpolatorVector(UPARAM(ref) FDoubleIIRInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetDoubleIIRInterpolatorVector(UPARAM(ref) FDoubleIIRInterpolatorVector& Interpolator);

	
	// Double IIR - Rotator
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FRotator EvalDoubleIIRInterpolatorRotator(UPARAM(ref) FDoubleIIRInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetDoubleIIRInterpolatorRotator(UPARAM(ref) FDoubleIIRInterpolatorRotator& Interpolator);


	// Double IIR - Float
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static float EvalDoubleIIRInterpolatorFloat(UPARAM(ref) FDoubleIIRInterpolatorFloat& Interpolator, float NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetDoubleIIRInterpolatorFloat(UPARAM(ref) FDoubleIIRInterpolatorFloat& Interpolator);


	// Critically-damped spring - Vector
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FVector EvalCritDampedSpringInterpolatorVector(FCritDampSpringInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetCritDampedSpringInterpolatorVector(FCritDampSpringInterpolatorVector& Interpolator);


	// Critically-damped spring - Rotator
	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static FRotator EvalCritDampedSpringInterpolatorRotator(FCritDampSpringInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime);

	UFUNCTION(BlueprintCallable, Category = "SPInterpolators")
	static void ResetCritDampedSpringInterpolatorRotator(FCritDampSpringInterpolatorRotator& Interpolator);
};
