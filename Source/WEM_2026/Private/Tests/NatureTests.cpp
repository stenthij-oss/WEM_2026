// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMath.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "RoomManagerTestHelpers.h"

namespace NatureTestsPrivate
{
	using namespace WEMRoomManagerTests;

	FGridPlatform MakeSegment(
		const FIntVector& MinNode,
		const EGridAxis Normal,
		UPlatformTileData* Tile,
		const EGridFace Up,
		UPlatformStructureData* Positive,
		UPlatformStructureData* Negative)
	{
		FGridPlatform Platform;
		Platform.MinNode = MinNode;
		Platform.Normal = Normal;
		Platform.Tile = Tile;
		Platform.Up = Up;
		Platform.PositiveStructure = Positive;
		Platform.NegativeStructure = Negative;
		return Platform;
	}

	/** A one-sided 16 x 16 structure of one type, standing for one side of an enclosure. */
	UPlatformStructureData* MakeSided(const TCHAR* Name, const EStructureType Type, const EStructureSide Side, TArray<UObject*>& OutRooted)
	{
		UPlatformStructureData* Structure = MakeStructure(Name, 16, 16, Type, false);
		Structure->Side = Side;
		OutRooted.Add(Structure);
		return Structure;
	}

	/** The city's one-sided structures, inside and out, and the three forms of nature, all for 16 x 16. */
	struct FNatureKit
	{
		UPlatformStructureData* RoofExterior = nullptr;
		UPlatformStructureData* CeilingInterior = nullptr;
		UPlatformStructureData* CeilingExterior = nullptr;
		UPlatformStructureData* GroundInterior = nullptr;
		UPlatformStructureData* WallExterior = nullptr;
		UPlatformStructureData* WallInterior = nullptr;
		UPlatformStructureData* Ground = nullptr;
		UPlatformStructureData* Cliff = nullptr;
		UPlatformStructureData* Water = nullptr;

		explicit FNatureKit(TArray<UObject*>& OutRooted)
		{
			RoofExterior = MakeSided(TEXT("TestNatureRoofExt"), EStructureType::Roof, EStructureSide::Exterior, OutRooted);
			CeilingInterior = MakeSided(TEXT("TestNatureCeilingInt"), EStructureType::Ceiling, EStructureSide::Interior, OutRooted);
			CeilingExterior = MakeSided(TEXT("TestNatureCeilingExt"), EStructureType::Ceiling, EStructureSide::Exterior, OutRooted);
			GroundInterior = MakeSided(TEXT("TestNatureGroundInt"), EStructureType::Ground, EStructureSide::Interior, OutRooted);
			WallExterior = MakeSided(TEXT("TestNatureWallExt"), EStructureType::Wall, EStructureSide::Exterior, OutRooted);
			WallInterior = MakeSided(TEXT("TestNatureWallInt"), EStructureType::Wall, EStructureSide::Interior, OutRooted);
			Ground = MakeNature(TEXT("TestNatureGround"), ENatureForm::Ground);
			Cliff = MakeNature(TEXT("TestNatureCliff"), ENatureForm::Cliff);
			Water = MakeNature(TEXT("TestNatureWater"), ENatureForm::Water);
			OutRooted.Append({ Ground, Cliff, Water });
		}

		TArray<TObjectPtr<UPlatformStructureData>> GetAll() const
		{
			return { RoofExterior, CeilingInterior, CeilingExterior, GroundInterior, WallExterior, WallInterior, Ground, Cliff, Water };
		}
	};

	/** Whether a filled placement's meetings are all legal, and whether its nature face meets anything, as one word for a message. */
	FString DescribeMeetings(const ARoomManager& Manager, const FGridPlatform& Platform)
	{
		bool bMet = false;
		const bool bLegal = FRoomManagerTestAccess::AreNatureMeetingsLegal(Manager, Platform, bMet);
		return FString::Printf(TEXT("%s%s"), bLegal ? TEXT("legal") : TEXT("illegal"), bMet ? TEXT(", met") : TEXT(""));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureSectionsTest, "WEM.Nature.Sections", WEMRoomManagerTests::TestFlags)

bool FNatureSectionsTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	const UPlatformStructureData* Default = GetDefault<UPlatformStructureData>();
	TestEqual(TEXT("A structure is urban unless set otherwise"), Default->Section, EStructureSection::Urban);

	// The designer's slabs fill every tile; nature of all three forms sits in the same list.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	TArray<TObjectPtr<UPlatformStructureData>> Structures = Manager->Structures;
	UPlatformStructureData* Ground = MakeNature(TEXT("TestSectionGround"), ENatureForm::Ground);
	UPlatformStructureData* Cliff = MakeNature(TEXT("TestSectionCliff"), ENatureForm::Cliff);
	UPlatformStructureData* Water = MakeNature(TEXT("TestSectionWater"), ENatureForm::Water);
	Rooted.Append({ Ground, Cliff, Water });
	Structures.Append({ Ground, Cliff, Water });
	Manager->Structures = Structures;
	Manager->ClearPlatforms();

	TestEqual(TEXT("Nature with no mesh is usable as nature"), FRoomManagerTestAccess::GetUsableNatureCount(*Manager), 3);

	for (int32 Beat = 0; Beat < 300; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	int32 NatureDrawn = 0;
	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		NatureDrawn += (Platform.PositiveStructure && Platform.PositiveStructure->IsNature()) || (Platform.NegativeStructure && Platform.NegativeStructure->IsNature()) ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("%d platforms grew."), Manager->GetPlatformCount()));
	TestTrue(TEXT("Growth goes on"), Manager->GetPlatformCount() > 100);
	TestEqual(TEXT("Growth never fills a platform with nature"), NatureDrawn, 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureFillTest, "WEM.Nature.Fill", WEMRoomManagerTests::TestFlags)

bool FNatureFillTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	// A two-sided slab too, which never goes behind nature.
	UPlatformStructureData* Slab = MakeStructure(TEXT("TestNatureSlab"), 16, 16, EStructureType::Ground | EStructureType::Ceiling | EStructureType::Wall, true, true);
	Rooted.Add(Slab);

	TArray<TObjectPtr<UPlatformStructureData>> Structures = Kit.GetAll();
	Structures.Add(Slab);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
	UPlatformTileData* Square = Manager->Tiles[0];

	// Ground on a floor looks out along its Up, over a ceiling of either side - never a roof, never the slab.
	int32 InteriorBacks = 0;
	int32 ExteriorBacks = 0;

	for (int32 Draw = 0; Draw < 64; ++Draw)
	{
		FGridPlatform Floor = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, nullptr, nullptr);

		if (!FRoomManagerTestAccess::DrawNatureFill(*Manager, Floor, ENatureForm::Ground, 1))
		{
			AddError(TEXT("Ground is not laid on a floor looking up"));
			break;
		}

		TestTrue(TEXT("Its face looking along Up is the ground"), Floor.PositiveStructure == Kit.Ground);
		InteriorBacks += Floor.NegativeStructure == Kit.CeilingInterior ? 1 : 0;
		ExteriorBacks += Floor.NegativeStructure == Kit.CeilingExterior ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("Behind ground, of 64: %d interior ceilings, %d exterior ones."), InteriorBacks, ExteriorBacks));
	TestEqual(TEXT("Behind ground is always a ceiling"), InteriorBacks + ExteriorBacks, 64);
	TestTrue(TEXT("Of either side"), InteriorBacks > 0 && ExteriorBacks > 0);

	// Upside down it is the same, on the other face.
	FGridPlatform Hanging = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::NegZ, nullptr, nullptr);
	TestTrue(TEXT("Ground hangs upside down"), FRoomManagerTestAccess::DrawNatureFill(*Manager, Hanging, ENatureForm::Ground, -1));
	TestTrue(TEXT("On its negative face, a ceiling above it"),
		Hanging.NegativeStructure == Kit.Ground && (Hanging.PositiveStructure == Kit.CeilingInterior || Hanging.PositiveStructure == Kit.CeilingExterior));

	FGridPlatform Mismatched = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, nullptr, nullptr);
	TestFalse(TEXT("Ground never looks against its Up"), FRoomManagerTestAccess::DrawNatureFill(*Manager, Mismatched, ENatureForm::Ground, -1));
	TestFalse(TEXT("A cliff is never laid on a floor"), FRoomManagerTestAccess::DrawNatureFill(*Manager, Mismatched, ENatureForm::Cliff, 1));

	// A cliff is a wall, in front of either side's wall.
	int32 Fronts = 0;
	int32 WallBacks = 0;

	for (int32 Draw = 0; Draw < 32; ++Draw)
	{
		FGridPlatform Wall = MakeSegment(FIntVector(32, 16, 16), EGridAxis::X, Square, EGridFace::PosZ, nullptr, nullptr);

		if (FRoomManagerTestAccess::DrawNatureFill(*Manager, Wall, ENatureForm::Cliff, -1))
		{
			Fronts += Wall.NegativeStructure == Kit.Cliff ? 1 : 0;
			WallBacks += Wall.PositiveStructure == Kit.WallExterior || Wall.PositiveStructure == Kit.WallInterior ? 1 : 0;
		}
	}

	TestEqual(TEXT("A cliff stands on the face asked for"), Fronts, 32);
	TestEqual(TEXT("With a wall behind it"), WallBacks, 32);

	FGridPlatform StandingWall = MakeSegment(FIntVector(32, 16, 16), EGridAxis::X, Square, EGridFace::PosZ, nullptr, nullptr);
	TestFalse(TEXT("Ground is never laid on a wall"), FRoomManagerTestAccess::DrawNatureFill(*Manager, StandingWall, ENatureForm::Ground, 1));

	// Water lies like ground.
	FGridPlatform Pool = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, nullptr, nullptr);
	TestTrue(TEXT("Water lies on a floor"), FRoomManagerTestAccess::DrawNatureFill(*Manager, Pool, ENatureForm::Water, 1));
	TestTrue(TEXT("Over a ceiling"), Pool.PositiveStructure == Kit.Water
		&& (Pool.NegativeStructure == Kit.CeilingInterior || Pool.NegativeStructure == Kit.CeilingExterior));

	// With nothing one-sided to stand behind it, nature is not laid.
	ARoomManager* SlabsOnly = SpawnRoomManager(TestWorld.World, Rooted, TArray<TObjectPtr<UPlatformStructureData>>{ Slab, Kit.Ground });
	FGridPlatform Bare = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, SlabsOnly->Tiles[0], EGridFace::PosZ, nullptr, nullptr);
	TestFalse(TEXT("Nothing goes behind ground but a one-sided ceiling"), FRoomManagerTestAccess::DrawNatureFill(*SlabsOnly, Bare, ENatureForm::Ground, 1));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureMeetingsTest, "WEM.Nature.Meetings", WEMRoomManagerTests::TestFlags)

bool FNatureMeetingsTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	UPlatformTileData* Square = Manager->Tiles[0];

	auto Expect = [&](const TCHAR* What, const FGridPlatform& Platform, const TCHAR* Expected)
	{
		TestEqual(What, DescribeMeetings(*Manager, Platform), FString(Expected));
	};

	// Ground beside a roof, over a ceiling inside: its ground meets the roof, which is an exterior.
	const FGridPlatform Roof = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, Kit.CeilingInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Roof });

	Expect(TEXT("Ground beside a roof, looking up"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior), TEXT("legal, met"));
	Expect(TEXT("Ground beside it hanging down meets the ceiling inside"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::NegZ, Kit.CeilingInterior, Kit.Ground), TEXT("illegal, met"));
	Expect(TEXT("Water beside a roof"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Water, Kit.CeilingInterior), TEXT("illegal, met"));

	const FGridPlatform OpenUnderside = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, Kit.CeilingExterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { OpenUnderside });
	Expect(TEXT("Ground hanging down beside an exterior ceiling"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::NegZ, Kit.CeilingInterior, Kit.Ground), TEXT("legal, met"));

	// A wall facing out along +X, inside along -X, Z 16 to 32. Off its top, ground over the inside meets its
	// outside round the corner - a green roof - and ground reaching out over the outside meets the inside.
	const FGridPlatform Wall = MakeSegment(FIntVector(32, 16, 16), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallExterior, Kit.WallInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Wall });

	Expect(TEXT("Ground over the inside of a wall"),
		MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior), TEXT("legal, met"));
	Expect(TEXT("Ground reaching out over the outside, looking up"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior), TEXT("illegal, met"));
	Expect(TEXT("Ground reaching out over the outside, hanging down"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::NegZ, Kit.CeilingInterior, Kit.Ground), TEXT("legal, met"));
	Expect(TEXT("Ground standing on the wall's face, as a wall would"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosX, Kit.Ground, Kit.CeilingInterior), TEXT("legal, met"));

	// A field of ground at Z 32, X 16 to 32. Off its edge at X 32 a cliff drops, facing out, or rises, facing
	// over it; the other way round its face meets the city behind the field.
	const FGridPlatform Field = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Field });

	Expect(TEXT("A cliff dropping off the field, facing out"),
		MakeSegment(FIntVector(32, 16, 16), EGridAxis::X, Square, EGridFace::PosZ, Kit.Cliff, Kit.WallInterior), TEXT("legal, met"));
	Expect(TEXT("A cliff dropping off the field, facing in"),
		MakeSegment(FIntVector(32, 16, 16), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallInterior, Kit.Cliff), TEXT("illegal, met"));
	Expect(TEXT("A cliff rising from the field, facing over it"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallExterior, Kit.Cliff), TEXT("legal, met"));
	Expect(TEXT("A cliff rising from the field, facing away"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosZ, Kit.Cliff, Kit.WallExterior), TEXT("illegal, met"));
	Expect(TEXT("A cliff on its side, its Up along the field's edge"),
		MakeSegment(FIntVector(32, 16, 16), EGridAxis::X, Square, EGridFace::PosY, Kit.Cliff, Kit.WallInterior), TEXT("illegal, met"));
	Expect(TEXT("Ground carrying the field on"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior), TEXT("legal, met"));
	Expect(TEXT("Water carrying the field on"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Water, Kit.CeilingInterior), TEXT("legal, met"));
	Expect(TEXT("Water upside down beside it"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::NegZ, Kit.CeilingInterior, Kit.Water), TEXT("illegal, met"));
	Expect(TEXT("Ground folding up off the field, its ground facing in"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::NegX, Kit.CeilingInterior, Kit.Ground), TEXT("illegal, met"));
	Expect(TEXT("Ground folding up off the field, its ground facing out"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosX, Kit.Ground, Kit.CeilingInterior), TEXT("illegal, met"));
	Expect(TEXT("An exterior roof carrying the field on, as the city may"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, Kit.CeilingInterior), TEXT("legal"));
	Expect(TEXT("An interior ground carrying it on"),
		MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.GroundInterior, Kit.CeilingInterior), TEXT("illegal"));

	// The field with its cliff dropping off the edge, facing out: water pools at the cliff's foot, out in front
	// of it, but not behind it, under the field.
	const FGridPlatform Drop = MakeSegment(FIntVector(32, 16, 16), EGridAxis::X, Square, EGridFace::PosZ, Kit.Cliff, Kit.WallInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Field, Drop });
	TestEqual(TEXT("Field and cliff stand"), Manager->GetPlatformCount(), 2);

	Expect(TEXT("Water at the cliff's foot, in front of it"),
		MakeSegment(FIntVector(32, 16, 16), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Water, Kit.CeilingInterior), TEXT("legal, met"));
	Expect(TEXT("Ground at the cliff's foot, in front of it"),
		MakeSegment(FIntVector(32, 16, 16), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior), TEXT("legal, met"));
	Expect(TEXT("Water behind the cliff, under the field"),
		MakeSegment(FIntVector(16, 16, 16), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Water, Kit.CeilingInterior), TEXT("illegal, met"));
	Expect(TEXT("A cliff carrying the drop on beside it"),
		MakeSegment(FIntVector(32, 32, 16), EGridAxis::X, Square, EGridFace::PosZ, Kit.Cliff, Kit.WallInterior), TEXT("legal, met"));
	Expect(TEXT("A cliff beside it the other way up"),
		MakeSegment(FIntVector(32, 32, 16), EGridAxis::X, Square, EGridFace::NegZ, Kit.Cliff, Kit.WallInterior), TEXT("illegal, met"));
	Expect(TEXT("A cliff turning the corner round its side, facing the same air"),
		MakeSegment(FIntVector(32, 32, 16), EGridAxis::Y, Square, EGridFace::PosZ, Kit.WallInterior, Kit.Cliff), TEXT("legal, met"));
	Expect(TEXT("A cliff turning the corner, facing away"),
		MakeSegment(FIntVector(32, 32, 16), EGridAxis::Y, Square, EGridFace::PosZ, Kit.Cliff, Kit.WallInterior), TEXT("illegal, met"));
	Expect(TEXT("An exterior wall carrying the drop on"),
		MakeSegment(FIntVector(32, 32, 16), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallExterior, Kit.WallInterior), TEXT("illegal"));
	Expect(TEXT("Ground folding off the cliff's side"),
		MakeSegment(FIntVector(32, 32, 16), EGridAxis::Y, Square, EGridFace::PosY, Kit.Ground, Kit.CeilingInterior), TEXT("illegal, met"));

	// A cliff rising from the field, facing over it, X 32, Z 32 to 48: off its top, a terrace on the far side.
	const FGridPlatform Rise = MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallExterior, Kit.Cliff);
	FRoomManagerTestAccess::Rebuild(*Manager, { Field, Rise });
	TestEqual(TEXT("Field and rising cliff stand"), Manager->GetPlatformCount(), 2);

	Expect(TEXT("Ground off the cliff's top, beyond it"),
		MakeSegment(FIntVector(32, 16, 48), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior), TEXT("legal, met"));
	Expect(TEXT("Ground off the cliff's top, over the field"),
		MakeSegment(FIntVector(16, 16, 48), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior), TEXT("illegal, met"));
	Expect(TEXT("Water off the cliff's top, beyond it"),
		MakeSegment(FIntVector(32, 16, 48), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Water, Kit.CeilingInterior), TEXT("legal, met"));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureCityTest, "WEM.Nature.City", WEMRoomManagerTests::TestFlags)

bool FNatureCityTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	UPlatformTileData* Square = Manager->Tiles[0];

	const FGridPlatform Field = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Field });

	// A floor of the city beside the field meets its ground with its top, which has to be an exterior.
	const FGridPlatform Beside = MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, nullptr, nullptr);
	uint8 FacesOnGround = 0;
	TestFalse(TEXT("Ground takes the city"), FRoomManagerTestAccess::MeetsClosedNature(*Manager, Beside, FacesOnGround));
	TestEqual(TEXT("With the face looking up"), FacesOnGround, uint8(1));
	TestTrue(TEXT("So the placement is open"), Manager->CanPlacePlatform(Beside));

	for (const EGridFace Up : { EGridFace::PosZ, EGridFace::NegZ })
	{
		int32 Exteriors = 0;

		for (int32 Draw = 0; Draw < 32; ++Draw)
		{
			FGridPlatform Floor = Beside;
			Floor.Up = Up;

			if (FRoomManagerTestAccess::DrawFill(*Manager, Floor) && Floor.PositiveStructure && Floor.PositiveStructure->StandsFor(EStructureSide::Exterior))
			{
				++Exteriors;
			}
		}

		TestEqual(FString::Printf(TEXT("Up %d: the face meeting ground is always drawn from the exteriors"), static_cast<int32>(Up)), Exteriors, 32);
	}

	// Water and cliffs take no face of the city.
	const FGridPlatform Rise = MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallExterior, Kit.Cliff);
	const FGridPlatform Pool = MakeSegment(FIntVector(16, 32, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Water, Kit.CeilingInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Field, Rise, Pool });
	TestEqual(TEXT("Field, cliff and pool stand"), Manager->GetPlatformCount(), 3);

	const FGridPlatform BesideCliff = MakeSegment(FIntVector(32, 32, 32), EGridAxis::X, Square, EGridFace::PosZ, nullptr, nullptr);
	TestTrue(TEXT("A wall of the city carrying the cliff on meets it"), FRoomManagerTestAccess::MeetsClosedNature(*Manager, BesideCliff, FacesOnGround));
	TestFalse(TEXT("And is not open"), Manager->CanPlacePlatform(BesideCliff));

	FGridPlatform Filled = BesideCliff;
	TestFalse(TEXT("Nor filled"), FRoomManagerTestAccess::DrawFill(*Manager, Filled));

	const FGridPlatform BesidePool = MakeSegment(FIntVector(16, 48, 32), EGridAxis::Z, Square, EGridFace::PosZ, nullptr, nullptr);
	TestTrue(TEXT("A floor of the city beside the pool meets it"), FRoomManagerTestAccess::MeetsClosedNature(*Manager, BesidePool, FacesOnGround));
	TestFalse(TEXT("And is not open"), Manager->CanPlacePlatform(BesidePool));

	// With no exterior to draw, the face meeting ground cannot be filled.
	ARoomManager* Inside = SpawnRoomManager(TestWorld.World, Rooted, TArray<TObjectPtr<UPlatformStructureData>>{ Kit.CeilingInterior, Kit.GroundInterior, Kit.WallInterior, Kit.Ground });
	FRoomManagerTestAccess::Rebuild(*Inside, { MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Inside->Tiles[0], EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior) });
	FGridPlatform Unfillable = MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Inside->Tiles[0], EGridFace::PosZ, nullptr, nullptr);
	TestFalse(TEXT("Nothing but interiors to draw: not filled"), FRoomManagerTestAccess::DrawFill(*Inside, Unfillable));

	// Growth around a roof with a field beside it keeps to the rules every beat, and agrees with a full rebuild.
	const FGridPlatform Roof = MakeSegment(FIntVector(24, 24, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, Kit.CeilingInterior);
	const FGridPlatform Meadow = MakeSegment(FIntVector(40, 24, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Roof, Meadow });
	TestEqual(TEXT("Roof and meadow stand"), Manager->GetPlatformCount(), 2);

	for (int32 Beat = 0; Beat < 200; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	int32 MeetingMeadow = 0;
	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		uint8 OnGround = 0;
		FRoomManagerTestAccess::MeetsClosedNature(*Manager, Platform, OnGround);
		MeetingMeadow += OnGround != 0 ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("%d platforms grew, %d of them meeting the meadow."), Manager->GetPlatformCount(), MeetingMeadow));
	TestEqual(TEXT("No two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureLoadBearingTest, "WEM.Nature.LoadBearing", WEMRoomManagerTests::TestFlags)

bool FNatureLoadBearingTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	UPlatformStructureData* LightCeiling = MakeSided(TEXT("TestNatureLightCeiling"), EStructureType::Ceiling, EStructureSide::Interior, Rooted);
	LightCeiling->bLoadBearing = false;

	TArray<TObjectPtr<UPlatformStructureData>> Structures = Kit.GetAll();
	Structures.Add(LightCeiling);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
	UPlatformTileData* Square = Manager->Tiles[0];

	// A roof over a ceiling that is not load-bearing blocks every beam no platform used before it.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, LightCeiling) });
	TestFalse(TEXT("Over a light ceiling, a roof's beams are not built"), Manager->IsCellSolid(FIntVector(18, 16, 32)));

	// Ground over the same ceiling holds up whatever grows off it.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, LightCeiling) });
	TestTrue(TEXT("Over a light ceiling, ground's beams are built"), Manager->IsCellSolid(FIntVector(18, 16, 32)));
	TestTrue(TEXT("And more ground can carry it on"),
		Manager->CanPlacePlatform(MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior)));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureFlatPiecesTest, "WEM.Nature.FlatPieces", WEMRoomManagerTests::TestFlags)

bool FNatureFlatPiecesTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);
	Kit.Ground->FlatColor = FLinearColor(0.1f, 0.4f, 0.05f);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	Manager->bShowPieces = true;
	UPlatformTileData* Square = Manager->Tiles[0];

	FRoomManagerTestAccess::Place(*Manager, MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior));
	TestEqual(TEXT("The field stands"), Manager->GetPlatformCount(), 1);

	// Ground with no mesh is the beams' box, tinted to its colour, across the opening.
	UStaticMesh* Mesh = nullptr;
	UMaterialInterface* Material = nullptr;
	TestEqual(TEXT("Ground is drawn once"), FRoomManagerTestAccess::GetStructureLayerPieces(*Manager, Kit.Ground, Mesh, Material), 1);
	TestTrue(TEXT("From the beams' box"), Mesh == Manager->PieceMesh);

	const UMaterialInstanceDynamic* Tinted = Cast<UMaterialInstanceDynamic>(Material);
	TestTrue(TEXT("Tinted from the beams' material"), Tinted && Tinted->Parent == Manager->PieceMaterial);

	if (Tinted)
	{
		FLinearColor Color;
		Tinted->GetVectorParameterValue(FHashedMaterialParameterInfo(Manager->PieceColorParameterName), Color);
		TestTrue(TEXT("To its flat colour"), Color.Equals(Kit.Ground->FlatColor));
	}

	// The ceiling behind it keeps its own mesh and material.
	TestEqual(TEXT("The ceiling is drawn once"), FRoomManagerTestAccess::GetStructureLayerPieces(*Manager, Kit.CeilingInterior, Mesh, Material), 1);
	TestTrue(TEXT("From its own mesh"), Mesh == Kit.CeilingInterior->Mesh);
	TestFalse(TEXT("Untinted"), Material && Material->IsA<UMaterialInstanceDynamic>());

	ReleaseRooted(Rooted);
	return true;
}

#endif
