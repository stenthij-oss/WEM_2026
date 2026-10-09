// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "PlatformStructureData.h"
#include "StaticMeshAttributes.h"
#include "StructureImport.h"
#include "StructurePlacement.h"

namespace StructureImportTestsPrivate
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* FacadeSettings = TEXT(R"json({
  "format": 1,
  "name": "Facade_Panel_A",
  "blend": "facades.blend",
  "object": "Facade_Panel_A",
  "width": 8,
  "length": 16,
  "twoSided": false,
  "categories": ["DA_Category_ExteriorUrban", "DA_Category_ExteriorIndustrial"],
  "types": ["Wall"],
  "loadBearing": false,
  "holdsItems": true,
  "fence": false,
  "mayLieSideways": false,
  "weight": 1.0,
  "bounds": { "min": [-0.70, -1.50, 0.0], "max": [0.70, 1.50, 0.10] },
  "exported": "2026-10-08T14:02:11"
})json");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureImportSettingsTest, "WEM.Structures.Import.Settings", StructureImportTestsPrivate::TestFlags)

bool FStructureImportSettingsTest::RunTest(const FString& Parameters)
{
	using namespace StructureImportTestsPrivate;

	FStructureSettings Settings;
	FString Error;

	if (!TestTrue(TEXT("The settings file in the brief's own format reads"), WEMStructureImport::ParseSettings(FacadeSettings, Settings, Error)))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("Its name"), Settings.Name, FString(TEXT("Facade_Panel_A")));
	TestEqual(TEXT("Where it came from"), Settings.Blend, FString(TEXT("facades.blend")));
	TestEqual(TEXT("Its width"), Settings.Width, 8);
	TestEqual(TEXT("Its length"), Settings.Length, 16);
	TestFalse(TEXT("One-sided"), Settings.bTwoSided);
	TestFalse(TEXT("Not load-bearing"), Settings.bLoadBearing);
	TestTrue(TEXT("Holds items"), Settings.bHoldsItems);
	TestEqual(TEXT("Two categories"), Settings.Categories.Num(), 2);
	TestTrue(TEXT("Bounds in metres"), Settings.BoundsMin.Equals(FVector(-0.7, -1.5, 0.0)) && Settings.BoundsMax.Equals(FVector(0.7, 1.5, 0.1)));

	TArray<FString> Unknown;
	TestEqual(TEXT("Wall alone"), WEMStructureImport::MakeTypeMask(Settings.Types, Unknown), static_cast<int32>(EStructureType::Wall));
	TestEqual(TEXT("All three types"), WEMStructureImport::MakeTypeMask({ TEXT("Ground"), TEXT("Ceiling"), TEXT("Wall"), TEXT("Roof") }, Unknown),
		static_cast<int32>(EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall));
	TestEqual(TEXT("A type it does not know is reported"), Unknown.Num(), 1);

	// A file of another format, or one missing what the importer needs, is refused rather than guessed at.
	FString Newer = FacadeSettings;
	Newer.ReplaceInline(TEXT("\"format\": 1"), TEXT("\"format\": 2"));
	TestFalse(TEXT("A newer format is refused"), WEMStructureImport::ParseSettings(Newer, Settings, Error));

	FString Unbounded = FacadeSettings;
	Unbounded.ReplaceInline(TEXT("\"bounds\""), TEXT("\"limits\""));
	TestFalse(TEXT("A file without bounds is refused"), WEMStructureImport::ParseSettings(Unbounded, Settings, Error));

	TestFalse(TEXT("Something that is not JSON is refused"), WEMStructureImport::ParseSettings(TEXT("Facade_Panel_A"), Settings, Error));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureImportMeasureTest, "WEM.Structures.Import.Measure", StructureImportTestsPrivate::TestFlags)

bool FStructureImportMeasureTest::RunTest(const FString& Parameters)
{
	int32 Cells = 0;

	// The opening between the beams is a cell short of the size beam to beam.
	TestTrue(TEXT("140 cm is 8 cells beam to beam"), WEMStructureImport::MeasureCells(140.0, 20.0, Cells) && Cells == 8);
	TestTrue(TEXT("300 cm is 16"), WEMStructureImport::MeasureCells(300.0, 20.0, Cells) && Cells == 16);
	TestTrue(TEXT("620 cm is 32"), WEMStructureImport::MeasureCells(620.0, 20.0, Cells) && Cells == 32);
	TestTrue(TEXT("Within a centimetre still counts"), WEMStructureImport::MeasureCells(300.9, 20.0, Cells) && Cells == 16);
	TestFalse(TEXT("Further off does not"), WEMStructureImport::MeasureCells(302.0, 20.0, Cells));
	TestFalse(TEXT("Nor does the full size beam to beam"), WEMStructureImport::MeasureCells(310.0, 20.0, Cells));

	// The bounds the importer checks the mesh against are the file's, carried into the mesh's frame.
	FStructureSettings Settings;
	Settings.BoundsMin = FVector(-0.7, -1.5, 0.0);
	Settings.BoundsMax = FVector(0.7, 1.5, 0.1);

	const FBox Expected = WEMStructureImport::GetExpectedMeshBounds(Settings);
	TestTrue(FString::Printf(TEXT("A 1.40 x 3.00 m one-sided structure is 140 x 300 x 10 cm in the mesh's frame (got %s)"), *Expected.ToString()),
		Expected.GetSize().Equals(FVector(140.0, 300.0, 10.0), 1.e-6) && FMath::IsNearlyZero(Expected.Min.Z));

	TestTrue(TEXT("Bounds within a centimetre match"), WEMStructureImport::BoundsMatch(Expected, Expected.ShiftBy(FVector(0.5, -0.5, 0.0))));
	TestFalse(TEXT("Bounds further off do not"), WEMStructureImport::BoundsMatch(Expected, Expected.ExpandBy(2.0)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureImportCalibrationTest, "WEM.Structures.Import.CalibrationAxes", StructureImportTestsPrivate::TestFlags)

bool FStructureImportCalibrationTest::RunTest(const FString& Parameters)
{
	// The calibration structure is an F in relief on a 3 x 3 m slab, made by
	// SourceArt/Structures/Calibration/make_calibration.py. In Blender's axes, the top of its relief
	// is three rectangles at Z = 0.25 m: the stem, X -0.6..-0.3 and Y -1.0..1.0; the top bar,
	// X -0.3..0.7 and Y 0.7..1.0; and the middle bar, X -0.3..0.4 and Y 0.0..0.3. Where that top lands
	// in the imported mesh says where the importer put each of Blender's axes - an F reads differently
	// under every turn and mirror - so it is checked against the axes the room manager lays meshes by.
	const UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Structures/16x16/SM_Calibration_F.SM_Calibration_F"));

	if (!TestNotNull(TEXT("The calibration structure has been imported"), Mesh) || !TestNotNull(TEXT("It has source geometry"), Mesh->GetMeshDescription(0)))
	{
		return false;
	}

	const FMeshDescription& Description = *Mesh->GetMeshDescription(0);
	const TVertexAttributesConstRef<FVector3f> Positions = FStaticMeshConstAttributes(Description).GetVertexPositions();

	// The centroid of the relief's top, weighted by area, so however the importer splits or welds its
	// vertices the answer is the same.
	double Area = 0.0;
	FVector Centroid = FVector::ZeroVector;
	double TopHeight = 0.0;

	for (const FTriangleID Triangle : Description.Triangles().GetElementIDs())
	{
		const TArrayView<const FVertexID> Vertices = Description.GetTriangleVertices(Triangle);
		const FVector A(Positions[Vertices[0]]);
		const FVector B(Positions[Vertices[1]]);
		const FVector C(Positions[Vertices[2]]);

		TopHeight = FMath::Max3(TopHeight, A.Z, FMath::Max(B.Z, C.Z));

		if (A.Z < 24.0 || B.Z < 24.0 || C.Z < 24.0)
		{
			continue;
		}

		const double TriangleArea = 0.5 * FVector::CrossProduct(B - A, C - A).Size();
		Area += TriangleArea;
		Centroid += TriangleArea * (A + B + C) / 3.0;
	}

	if (!TestTrue(TEXT("The relief's top was found"), Area > 0.0))
	{
		return false;
	}

	Centroid /= Area;

	// The same centroid worked out in Blender's axes, in centimetres, and carried into the mesh's frame
	// by the axes the room manager believes the importer uses.
	const double Rectangles[3][4] = { { -60.0, -30.0, -100.0, 100.0 }, { -30.0, 70.0, 70.0, 100.0 }, { -30.0, 40.0, 0.0, 30.0 } };
	double BlenderArea = 0.0;
	FVector BlenderCentroid = FVector::ZeroVector;

	for (const auto& Rectangle : Rectangles)
	{
		const double RectangleArea = (Rectangle[1] - Rectangle[0]) * (Rectangle[3] - Rectangle[2]);
		BlenderArea += RectangleArea;
		BlenderCentroid += RectangleArea * FVector(0.5 * (Rectangle[0] + Rectangle[1]), 0.5 * (Rectangle[2] + Rectangle[3]), 25.0);
	}

	BlenderCentroid /= BlenderArea;

	FVector Expected = FVector::ZeroVector;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Expected += BlenderCentroid[Axis] * WEMStructures::GetMeshAxisForBlenderAxis(Axis);
	}

	TestTrue(FString::Printf(TEXT("The F's top lies where the room manager expects it: expected %s, measured %s"), *Expected.ToString(), *Centroid.ToString()),
		Centroid.Equals(Expected, 1.0));
	TestTrue(FString::Printf(TEXT("Blender's up is the mesh's up: the relief tops out at 25 cm (got %.2f)"), TopHeight), FMath::IsNearlyEqual(TopHeight, 25.0, 0.5));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureImportCategoryListTest, "WEM.Structures.Import.CategoryList", StructureImportTestsPrivate::TestFlags)

bool FStructureImportCategoryListTest::RunTest(const FString& Parameters)
{
	// The format the brief gives, two spaces, a category to a line - and a name that needs escaping.
	const FString Json = WEMStructureImport::FormatCategoryList({
		{ TEXT("DA_Category_ExteriorUrban"), TEXT("Exterior: Urban") },
		{ TEXT("DA_Category_Quoted"), TEXT("Say \"hi\"") } });

	const FString Expected =
		TEXT("{\n")
		TEXT("  \"format\": 1,\n")
		TEXT("  \"categories\": [\n")
		TEXT("    { \"asset\": \"DA_Category_ExteriorUrban\", \"name\": \"Exterior: Urban\" },\n")
		TEXT("    { \"asset\": \"DA_Category_Quoted\", \"name\": \"Say \\\"hi\\\"\" }\n")
		TEXT("  ]\n")
		TEXT("}\n");

	TestEqual(TEXT("categories.json reads as the brief shows it"), Json, Expected);

	// And it is JSON that reads back.
	TSharedPtr<FJsonObject> Object;
	TestTrue(TEXT("It parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) && Object.IsValid());
	TestEqual(TEXT("With both categories"), Object.IsValid() ? Object->GetArrayField(TEXT("categories")).Num() : 0, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureImportChangesTest, "WEM.Structures.Import.Changes", StructureImportTestsPrivate::TestFlags)

bool FStructureImportChangesTest::RunTest(const FString& Parameters)
{
	FStructureValues Before;
	Before.Width = 8;
	Before.Length = 16;
	Before.Types = static_cast<int32>(EStructureType::Wall);
	Before.Categories = { TEXT("DA_Category_ExteriorUrban") };

	TestEqual(TEXT("Nothing changed, nothing reported"), WEMStructureImport::DescribeChanges(Before, Before).Num(), 0);

	// A user who switched load-bearing off in Unreal, and a file that adds a category and a type: the
	// report says what the file put back and what it changed.
	FStructureValues Edited = Before;
	Edited.bLoadBearing = false;

	FStructureValues FromFile = Before;
	FromFile.Types |= static_cast<int32>(EStructureType::Ground);
	FromFile.Categories.Add(TEXT("DA_Category_NaturalSand"));

	const TArray<FString> Changes = WEMStructureImport::DescribeChanges(Edited, FromFile);
	TestEqual(TEXT("Three values changed"), Changes.Num(), 3);
	TestTrue(TEXT("The types"), Changes.Contains(TEXT("Types: Wall -> Ground, Wall")));
	TestTrue(TEXT("The categories"), Changes.Contains(TEXT("Categories: DA_Category_ExteriorUrban -> DA_Category_ExteriorUrban, DA_Category_NaturalSand")));
	TestTrue(TEXT("And the setting made in Unreal that the file overwrote"), Changes.Contains(TEXT("Load-bearing: off -> on")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructureImportHashTest, "WEM.Structures.Import.Hash", StructureImportTestsPrivate::TestFlags)

bool FStructureImportHashTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("StructureImportHash");
	const FString Mesh = Folder / TEXT("Test.glb");
	const FString Settings = Folder / TEXT("Test.json");

	FFileHelper::SaveStringToFile(TEXT("mesh"), *Mesh);
	FFileHelper::SaveStringToFile(TEXT("{ \"format\": 1 }"), *Settings);
	const FString First = WEMStructureImport::HashFiles({ Mesh, Settings });

	TestFalse(TEXT("Two files hash"), First.IsEmpty());
	TestEqual(TEXT("The same files hash the same"), WEMStructureImport::HashFiles({ Mesh, Settings }), First);

	FFileHelper::SaveStringToFile(TEXT("{ \"format\": 1, \"weight\": 2 }"), *Settings);
	TestNotEqual(TEXT("A changed settings file changes the hash"), WEMStructureImport::HashFiles({ Mesh, Settings }), First);

	TestTrue(TEXT("A missing file gives no hash"), WEMStructureImport::HashFiles({ Folder / TEXT("Missing.glb") }).IsEmpty());

	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true);
	return true;
}

#endif
