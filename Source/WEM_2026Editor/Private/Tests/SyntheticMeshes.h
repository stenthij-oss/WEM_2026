// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MeshPlacementAnalysis.h"

/**
 * Meshes written as plain triangle lists for the analysis tests: boxes, and prisms from a convex
 * outline. Every triangle is wound the engine's way round, facing out, unless a test flips it.
 */
namespace WEMSyntheticMeshes
{
	/** Adds a triangle, wound so that it faces Outward. */
	inline void AddTriangle(FMeshAnalysisSource& Mesh, const FVector& A, const FVector& B, const FVector& C, const FVector& Outward)
	{
		const FVector Normal = (C - A) ^ (B - A);
		const bool bFlip = FVector::DotProduct(Normal, Outward) < 0.0;

		const int32 First = Mesh.Positions.Num();
		Mesh.Positions.Add(FVector3f(A));
		Mesh.Positions.Add(FVector3f(bFlip ? C : B));
		Mesh.Positions.Add(FVector3f(bFlip ? B : C));
		Mesh.Indices.Append({ First, First + 1, First + 2 });
	}

	inline void AddQuad(FMeshAnalysisSource& Mesh, const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Outward)
	{
		AddTriangle(Mesh, A, B, C, Outward);
		AddTriangle(Mesh, A, C, D, Outward);
	}

	/**
	 * A closed prism: a convex outline in XY, given in order round it, from Bottom to Top. Its caps
	 * are fanned from the first corner, as a modelling tool would triangulate them.
	 */
	inline void AddPrism(FMeshAnalysisSource& Mesh, const TArray<FVector2D>& Outline, const double Bottom, const double Top)
	{
		const int32 Count = Outline.Num();

		FVector2D Centre = FVector2D::ZeroVector;
		for (const FVector2D& Corner : Outline)
		{
			Centre += Corner / Count;
		}

		for (int32 Index = 1; Index + 1 < Count; ++Index)
		{
			AddTriangle(Mesh, FVector(Outline[0], Top), FVector(Outline[Index], Top), FVector(Outline[Index + 1], Top), FVector::UpVector);
			AddTriangle(Mesh, FVector(Outline[0], Bottom), FVector(Outline[Index], Bottom), FVector(Outline[Index + 1], Bottom), FVector::DownVector);
		}

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector2D& From = Outline[Index];
			const FVector2D& To = Outline[(Index + 1) % Count];
			const FVector2D Middle = 0.5 * (From + To);
			const FVector Outward(Middle - Centre, 0.0);

			AddQuad(Mesh, FVector(From, Bottom), FVector(To, Bottom), FVector(To, Top), FVector(From, Top), Outward);
		}
	}

	inline void AddBox(FMeshAnalysisSource& Mesh, const FVector& Min, const FVector& Max)
	{
		AddPrism(Mesh, { FVector2D(Min.X, Min.Y), FVector2D(Max.X, Min.Y), FVector2D(Max.X, Max.Y), FVector2D(Min.X, Max.Y) }, Min.Z, Max.Z);
	}

	/** Turns every triangle inside out. */
	inline void FlipWinding(FMeshAnalysisSource& Mesh)
	{
		for (int32 Index = 0; Index + 2 < Mesh.Indices.Num(); Index += 3)
		{
			Swap(Mesh.Indices[Index + 1], Mesh.Indices[Index + 2]);
		}
	}

	/** A 60 x 120 top from 71 to 75 cm on four 5 cm legs in its corners: 3 x 6 x 4 cells. */
	inline FMeshAnalysisSource MakeDesk()
	{
		FMeshAnalysisSource Mesh;
		AddBox(Mesh, FVector(0.0, 0.0, 71.0), FVector(60.0, 120.0, 75.0));

		for (const FVector2D& Corner : { FVector2D(0.0, 0.0), FVector2D(55.0, 0.0), FVector2D(0.0, 115.0), FVector2D(55.0, 115.0) })
		{
			AddBox(Mesh, FVector(Corner, 0.0), FVector(Corner + FVector2D(5.0), 71.0));
		}

		return Mesh;
	}

	/** Two side panels, a plinth topped at 4 cm, and planks topped at 60 and 120 cm: 2 x 4 x 6 cells. */
	inline FMeshAnalysisSource MakeShelf()
	{
		FMeshAnalysisSource Mesh;
		AddBox(Mesh, FVector(0.0, 0.0, 0.0), FVector(40.0, 2.0, 120.0));
		AddBox(Mesh, FVector(0.0, 78.0, 0.0), FVector(40.0, 80.0, 120.0));
		AddBox(Mesh, FVector(0.0, 2.0, 0.0), FVector(40.0, 78.0, 4.0));
		AddBox(Mesh, FVector(0.0, 2.0, 57.0), FVector(40.0, 78.0, 60.0));
		AddBox(Mesh, FVector(0.0, 2.0, 117.0), FVector(40.0, 78.0, 120.0));
		return Mesh;
	}

	/** A 60 x 60 top from 70 to 75 cm, its four corners cut off 20 cm along each side, on a central pedestal: 3 x 3 x 4 cells. */
	inline FMeshAnalysisSource MakeCutCornerTop()
	{
		FMeshAnalysisSource Mesh;
		AddBox(Mesh, FVector(25.0, 25.0, 0.0), FVector(35.0, 35.0, 70.0));
		AddPrism(Mesh,
			{
				FVector2D(20.0, 0.0), FVector2D(40.0, 0.0), FVector2D(60.0, 20.0), FVector2D(60.0, 40.0),
				FVector2D(40.0, 60.0), FVector2D(20.0, 60.0), FVector2D(0.0, 40.0), FVector2D(0.0, 20.0)
			},
			70.0, 75.0);
		return Mesh;
	}
}

#endif
