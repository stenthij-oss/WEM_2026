// Copyright Epic Games, Inc. All Rights Reserved.

#include "SPInterpolatorsBPLibrary.h"
#include "SPInterpolators.h"

float USPInterpolatorsBPLibrary::EvalAccelInterpolatorFloat(FAccelerationInterpolatorFloat& Interpolator, float NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetAccelInterpolatorFloat(FAccelerationInterpolatorFloat& Interpolator)
{
	return Interpolator.Reset();
}


FVector USPInterpolatorsBPLibrary::EvalAccelInterpolatorVector(FAccelerationInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetAccelInterpolatorVector(FAccelerationInterpolatorVector& Interpolator)
{
	return Interpolator.Reset();
}


FRotator USPInterpolatorsBPLibrary::EvalAccelInterpolatorRotator(FAccelerationInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetAccelInterpolatorRotator(FAccelerationInterpolatorRotator& Interpolator)
{
	return Interpolator.Reset();
}


float USPInterpolatorsBPLibrary::EvalIIRInterpolatorFloat(FIIRInterpolatorFloat& Interpolator, float NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetIIRInterpolatorFloat(FIIRInterpolatorFloat& Interpolator)
{
	return Interpolator.Reset();
}


FVector USPInterpolatorsBPLibrary::EvalIIRInterpolatorVector(FIIRInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetIIRInterpolatorVector(FIIRInterpolatorVector& Interpolator)
{
	return Interpolator.Reset();
}


FRotator USPInterpolatorsBPLibrary::EvalIIRInterpolatorRotator(FIIRInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetIIRInterpolatorRotator(FIIRInterpolatorRotator& Interpolator)
{
	return Interpolator.Reset();
}


FVector USPInterpolatorsBPLibrary::EvalDoubleIIRInterpolatorVector(FDoubleIIRInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetDoubleIIRInterpolatorVector(FDoubleIIRInterpolatorVector& Interpolator)
{
	return Interpolator.Reset();
}


FRotator USPInterpolatorsBPLibrary::EvalDoubleIIRInterpolatorRotator(FDoubleIIRInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetDoubleIIRInterpolatorRotator(FDoubleIIRInterpolatorRotator& Interpolator)
{
	return Interpolator.Reset();
}


float USPInterpolatorsBPLibrary::EvalDoubleIIRInterpolatorFloat(FDoubleIIRInterpolatorFloat& Interpolator, float NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetDoubleIIRInterpolatorFloat(FDoubleIIRInterpolatorFloat& Interpolator)
{
	Interpolator.Reset();
}


FVector USPInterpolatorsBPLibrary::EvalCritDampedSpringInterpolatorVector(FCritDampSpringInterpolatorVector& Interpolator, FVector NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetCritDampedSpringInterpolatorVector(FCritDampSpringInterpolatorVector& Interpolator)
{
	return Interpolator.Reset();
}


FRotator USPInterpolatorsBPLibrary::EvalCritDampedSpringInterpolatorRotator(FCritDampSpringInterpolatorRotator& Interpolator, FRotator NewGoal, float DeltaTime)
{
	return Interpolator.Eval(NewGoal, DeltaTime);
}

void USPInterpolatorsBPLibrary::ResetCritDampedSpringInterpolatorRotator(FCritDampSpringInterpolatorRotator& Interpolator)
{
	return Interpolator.Reset();
}

