// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "PlatformStructureData.h"
#include "StructurePlacement.h"
#include "UObject/Package.h"

namespace StructureTransformTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	constexpr double CellSize = 20.0;

	/** Where Blender's axes - and so the mesh's face, top and width - land in the world under a transform. */
	struct FLaidAxes
	{
		FVector Width;
		FVector Top;
		FVector Face;
		double Determinant = 0.0;
	};

	FLaidAxes LayAxes(const FTransform& Transform)
	{
		FLaidAxes Axes;
		Axes.Width = Transform.TransformVectorNoScale(WEMStructures::GetMeshAxisForBlenderAxis(0));
		Axes.Top = Transform.TransformVectorNoScale(WEMStructures::GetMeshAxisForBlenderAxis(1));
		Axes.Face = Transform.TransformVectorNoScale(WEMStructures::GetMeshAxisForBlenderAxis(2));
		Axes.Determinant = Transform.ToMatrixNoScale().Determinant();
		return Axes;
	}

	/**
	 * One case: a segment facing along NormalAxis at the cube's corner, the face it is filled on, and
	 * where the mesh's face and top should then point.
	 */
	struct FTransformCase
	{
		const TCHAR* Name;
		int32 NormalAxis;
		int32 SpanFirst;
		int32 SpanSecond;
		int32 FaceSign;
		bool bTurned;
		bool bTwoSided;
		FVector ExpectedFace;
		FVector ExpectedTop;
	};

	/** The mesh's back sits on the mid-plane, or half a cell behind it when two-sided, at the opening's centre. */
	FVector ExpectedCentre(const FTransformCase& Case)
	{
		const int32 U = Case.NormalAxis == 0 ? 1 : 0;
		const int32 V = Case.NormalAxis == 2 ? 1 : 2;

		FVector Centre = FVector::ZeroVector;
		Centre[U] = 0.5 * (Case.SpanFirst + 1) * CellSize;
		Centre[V] = 0.5 * (Case.SpanSecond + 1) * CellSize;
		Centre[Case.NormalAxis] = 0.5 * CellSize - (Case.bTwoSided ? 0.5 * CellSize * Case.ExpectedFace[Case.NormalAxis] : 0.0);
		return Centre;
	}

	bool CheckCase(FAutomationTestBase& Test, const FTransformCase& Case)
	{
		const FTransform Transform = WEMStructures::MakeStructureTransform(
			FVector::ZeroVector, CellSize, Case.NormalAxis, Case.SpanFirst, Case.SpanSecond, Case.FaceSign, Case.bTurned, Case.bTwoSided);

		const FLaidAxes Axes = LayAxes(Transform);
		bool bPassed = true;

		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: the face looks out along %s (got %s)"), Case.Name, *Case.ExpectedFace.ToString(), *Axes.Face.ToString()),
			Axes.Face.Equals(Case.ExpectedFace, 1.e-6));
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: the top points along %s (got %s)"), Case.Name, *Case.ExpectedTop.ToString(), *Axes.Top.ToString()),
			Axes.Top.Equals(Case.ExpectedTop, 1.e-6));
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: turned, never mirrored (determinant %f)"), Case.Name, Axes.Determinant),
			FMath::IsNearlyEqual(Axes.Determinant, 1.0, 1.e-6));
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: stands at %s (got %s)"), Case.Name, *ExpectedCentre(Case).ToString(), *Transform.GetLocation().ToString()),
			Transform.GetLocation().Equals(ExpectedCentre(Case), 1.e-6));
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: at its own size"), Case.Name), Transform.GetScale3D().Equals(FVector::OneVector));

		// Written out, so the same rotation can be put on a mesh in a level and looked at.
		const FRotator Rotation = Transform.Rotator();
		Test.AddInfo(FString::Printf(TEXT("%s: pitch %.1f, yaw %.1f, roll %.1f"), Case.Name, Rotation.Pitch, Rotation.Yaw, Rotation.Roll));

		return bPassed;
	}

	UPlatformStructureData* MakeStructure(const int32 Width, const int32 Length, const EStructureType Types, const bool bTwoSided, const bool bMayLieSideways = false)
	{
		UPlatformStructureData* Structure = NewObject<UPlatformStructureData>(GetTransientPackage());
		Structure->Width = Width;
		Structure->Length = Length;
		Structure->Types = static_cast<int32>(Types);
		Structure->bTwoSided = bTwoSided;
		Structure->bMayLieSideways = bMayLieSideways;
		return Structure;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureTransformFacesTest, "WEM.Structures.Transform.Faces", StructureTransformTestsPrivate::TestFlags)

bool FStructureTransformFacesTest::RunTest(const FString& Parameters)
{
	using namespace StructureTransformTestsPrivate;

	// A floor's top and a ceiling's underside, and a wall facing each of the four ways, from both of its
	// faces: the mesh always looks out of the face it fills, and its top is up on every wall.
	const FTransformCase Cases[] =
	{
		{ TEXT("Floor top"),            2, 16, 16,  1, false, false, FVector(0, 0, 1),  FVector(0, 1, 0) },
		{ TEXT("Ceiling underside"),    2, 16, 16, -1, false, false, FVector(0, 0, -1), FVector(0, 1, 0) },
		{ TEXT("Wall facing +X"),       0, 16, 16,  1, false, false, FVector(1, 0, 0),  FVector(0, 0, 1) },
		{ TEXT("Wall facing -X"),       0, 16, 16, -1, false, false, FVector(-1, 0, 0), FVector(0, 0, 1) },
		{ TEXT("Wall facing +Y"),       1, 16, 16,  1, false, false, FVector(0, 1, 0),  FVector(0, 0, 1) },
		{ TEXT("Wall facing -Y"),       1, 16, 16, -1, false, false, FVector(0, -1, 0), FVector(0, 0, 1) },
		{ TEXT("Upright wall 8 x 16"),  0, 8, 16,   1, false, false, FVector(1, 0, 0),  FVector(0, 0, 1) },
	};

	for (const FTransformCase& Case : Cases)
	{
		CheckCase(*this, Case);
	}

	// Opposite faces of one wall show the same mesh from either side, so their widths run opposite
	// ways: each reads left to right for whoever stands in front of it.
	const FLaidAxes Front = LayAxes(WEMStructures::MakeStructureTransform(FVector::ZeroVector, CellSize, 0, 16, 16, 1, false, false));
	const FLaidAxes Back = LayAxes(WEMStructures::MakeStructureTransform(FVector::ZeroVector, CellSize, 0, 16, 16, -1, false, false));
	TestTrue(TEXT("The two faces of a wall run their widths opposite ways"), Front.Width.Equals(-Back.Width, 1.e-6));

	// A wall facing X and one facing Y differ by a quarter turn about up, never by a half turn: the
	// same face of the same structure, walked round a corner, keeps its top up and its width level.
	const FLaidAxes FacingX = LayAxes(WEMStructures::MakeStructureTransform(FVector::ZeroVector, CellSize, 0, 16, 16, 1, false, false));
	const FLaidAxes FacingY = LayAxes(WEMStructures::MakeStructureTransform(FVector::ZeroVector, CellSize, 1, 16, 16, 1, false, false));
	TestTrue(TEXT("Walls facing X and Y differ by a quarter turn about up"),
		FVector::CrossProduct(FacingX.Face, FacingY.Face).Equals(FVector::CrossProduct(FacingX.Width, FacingY.Width), 1.e-6)
		&& FMath::Abs(FVector::DotProduct(FacingX.Width, FacingY.Width)) < 1.e-6);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureTransformTurnedTest, "WEM.Structures.Transform.Turned", StructureTransformTestsPrivate::TestFlags)

bool FStructureTransformTurnedTest::RunTest(const FString& Parameters)
{
	using namespace StructureTransformTestsPrivate;

	// Turned, a structure's length runs along the segment's first axis: across a floor that is X, and
	// up a wall facing X it is Y - the wall's level axis, which a structure lying on its side runs along.
	const FTransformCase Cases[] =
	{
		{ TEXT("Turned floor top"),          2, 8, 16,  1, true, false, FVector(0, 0, 1),  FVector(1, 0, 0) },
		{ TEXT("Turned ceiling underside"),  2, 8, 16, -1, true, false, FVector(0, 0, -1), FVector(1, 0, 0) },
		{ TEXT("Lying wall facing +X"),      0, 16, 8,  1, true, false, FVector(1, 0, 0),  FVector(0, 1, 0) },
		{ TEXT("Lying wall facing -Y"),      1, 16, 8, -1, true, false, FVector(0, -1, 0), FVector(1, 0, 0) },
	};

	for (const FTransformCase& Case : Cases)
	{
		CheckCase(*this, Case);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureTransformTwoSidedTest, "WEM.Structures.Transform.TwoSided", StructureTransformTestsPrivate::TestFlags)

bool FStructureTransformTwoSidedTest::RunTest(const FString& Parameters)
{
	using namespace StructureTransformTestsPrivate;

	// Modelled lying on the ground, a two-sided structure goes back half a cell so it fills the segment
	// from face to face. CheckCase expects the centre there.
	const FTransformCase Cases[] =
	{
		{ TEXT("Two-sided floor"),          2, 16, 16,  1, false, true, FVector(0, 0, 1),  FVector(0, 1, 0) },
		{ TEXT("Two-sided wall facing +X"), 0, 8, 8,    1, false, true, FVector(1, 0, 0),  FVector(0, 0, 1) },
		{ TEXT("Two-sided wall facing -Y"), 1, 16, 32, -1, false, true, FVector(0, -1, 0), FVector(0, 0, 1) },
	};

	for (const FTransformCase& Case : Cases)
	{
		CheckCase(*this, Case);
	}

	// Its back half-cell behind the mid-plane and its face half a cell in front of it: both faces of
	// the segment, exactly. The mid-plane of a segment at the cube's corner is at half a cell.
	const FTransform Floor = WEMStructures::MakeStructureTransform(FVector::ZeroVector, CellSize, 2, 16, 16, 1, false, true);
	TestEqual(TEXT("A two-sided floor's back lies on the segment's underside"), Floor.GetLocation().Z, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureTransformNeverMirroredTest, "WEM.Structures.Transform.NeverMirrored", StructureTransformTestsPrivate::TestFlags)

bool FStructureTransformNeverMirroredTest::RunTest(const FString& Parameters)
{
	using namespace StructureTransformTestsPrivate;

	// Every face of every orientation, every way round: a proper rotation each time.
	int32 Cases = 0;

	for (int32 NormalAxis = 0; NormalAxis < 3; ++NormalAxis)
	{
		for (const int32 FaceSign : { 1, -1 })
		{
			for (const bool bTurned : { false, true })
			{
				for (const bool bTwoSided : { false, true })
				{
					const FLaidAxes Axes = LayAxes(WEMStructures::MakeStructureTransform(FVector(100.0, 200.0, 300.0), CellSize, NormalAxis, 8, 16, FaceSign, bTurned, bTwoSided));

					TestTrue(FString::Printf(TEXT("Normal %d, face %d, turned %d, two-sided %d is a proper rotation"), NormalAxis, FaceSign, bTurned, bTwoSided),
						FMath::IsNearlyEqual(Axes.Determinant, 1.0, 1.e-6));
					TestTrue(FString::Printf(TEXT("Normal %d, face %d: the width, top and face are a frame"), NormalAxis, FaceSign),
						FMath::Abs(FVector::DotProduct(Axes.Width, Axes.Top)) < 1.e-6 && FMath::Abs(FVector::DotProduct(Axes.Top, Axes.Face)) < 1.e-6);
					++Cases;
				}
			}
		}
	}

	TestEqual(TEXT("Every case was checked"), Cases, 24);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureFitTest, "WEM.Structures.Fit", StructureTransformTestsPrivate::TestFlags)

bool FStructureFitTest::RunTest(const FString& Parameters)
{
	using namespace StructureTransformTestsPrivate;

	bool bTurned = false;

	// Face types follow from the segment alone.
	TestEqual(TEXT("A floor's top is ground"), WEMStructures::GetFaceType(2, 1), EStructureType::Ground);
	TestEqual(TEXT("Its underside is ceiling"), WEMStructures::GetFaceType(2, -1), EStructureType::Ceiling);
	TestEqual(TEXT("Either face of a wall facing X is wall"), WEMStructures::GetFaceType(0, -1), EStructureType::Wall);
	TestEqual(TEXT("Either face of a wall facing Y is wall"), WEMStructures::GetFaceType(1, 1), EStructureType::Wall);

	// Shape: floors turn freely, walls only when the structure may lie sideways.
	TestTrue(TEXT("An 8 x 16 fits an 8 x 16 floor as it is"), WEMStructures::FitsShape(8, 16, false, 2, 8, 16, bTurned) && !bTurned);
	TestTrue(TEXT("An 8 x 16 fits a 16 x 8 floor turned"), WEMStructures::FitsShape(8, 16, false, 2, 16, 8, bTurned) && bTurned);
	TestTrue(TEXT("An upright 8 x 16 fits an upright wall"), WEMStructures::FitsShape(8, 16, false, 0, 8, 16, bTurned) && !bTurned);
	TestFalse(TEXT("An upright 8 x 16 does not fit a lying wall"), WEMStructures::FitsShape(8, 16, false, 1, 16, 8, bTurned));
	TestTrue(TEXT("One that may lie sideways does, turned"), WEMStructures::FitsShape(8, 16, true, 1, 16, 8, bTurned) && bTurned);
	TestTrue(TEXT("A square fits a square wall without turning"), WEMStructures::FitsShape(16, 16, true, 0, 16, 16, bTurned) && !bTurned);
	TestFalse(TEXT("A 16 x 16 does not fit an 8 x 16"), WEMStructures::FitsShape(16, 16, true, 2, 8, 16, bTurned));

	// Types: one-sided needs the face's own, two-sided both faces'.
	UPlatformStructureData* GroundOnly = MakeStructure(16, 16, EStructureType::Ground, false);
	UPlatformStructureData* FloorSlab = MakeStructure(16, 16, EStructureType::Ground | EStructureType::Ceiling, true);
	UPlatformStructureData* GroundSlab = MakeStructure(16, 16, EStructureType::Ground, true);
	UPlatformStructureData* WallSlab = MakeStructure(16, 16, EStructureType::Wall, true);

	TestTrue(TEXT("A one-sided ground structure fits a floor's top"), WEMStructures::FitsFace(*GroundOnly, 2, 1, 16, 16, bTurned));
	TestFalse(TEXT("But not its underside"), WEMStructures::FitsFace(*GroundOnly, 2, -1, 16, 16, bTurned));
	TestFalse(TEXT("Nor a wall"), WEMStructures::FitsFace(*GroundOnly, 0, 1, 16, 16, bTurned));
	TestTrue(TEXT("A two-sided ground-and-ceiling slab fits a floor from either face"),
		WEMStructures::FitsFace(*FloorSlab, 2, 1, 16, 16, bTurned) && WEMStructures::FitsFace(*FloorSlab, 2, -1, 16, 16, bTurned));
	TestFalse(TEXT("A two-sided slab of ground alone fits no floor, since it would be a ceiling too"), WEMStructures::FitsFace(*GroundSlab, 2, 1, 16, 16, bTurned));
	TestTrue(TEXT("A two-sided wall slab fits a wall"), WEMStructures::FitsFace(*WallSlab, 1, -1, 16, 16, bTurned));
	TestFalse(TEXT("But no floor"), WEMStructures::FitsFace(*WallSlab, 2, 1, 16, 16, bTurned));

	return true;
}

#endif
