// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GridMath.h"
#include "HAL/PlatformTime.h"
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

namespace NatureTestsPrivate
{
	/** The nature on a platform's nature face, and that face, or false for one of the city. */
	bool GetNature(const FGridPlatform& Platform, ENatureForm& OutForm, int32& OutSign)
	{
		for (const int32 Sign : { 1, -1 })
		{
			const UPlatformStructureData* Structure = Sign > 0 ? Platform.PositiveStructure.Get() : Platform.NegativeStructure.Get();

			if (Structure && Structure->IsNature())
			{
				OutForm = Structure->NatureForm;
				OutSign = Sign;
				return true;
			}
		}

		return false;
	}

	/** Sets the nature beat to start only one form, each field spilling out to exactly Size pieces. */
	void KeepToForm(ARoomManager& Manager, const ENatureForm Form, const int32 Size)
	{
		Manager.GroundWeight = Form == ENatureForm::Ground ? 1.0f : 0.0f;
		Manager.CliffWeight = Form == ENatureForm::Cliff ? 1.0f : 0.0f;
		Manager.WaterWeight = Form == ENatureForm::Water ? 1.0f : 0.0f;
		Manager.GroundSpillMin = Size;
		Manager.GroundSpillMax = Size;
		Manager.WaterSpillMin = Size;
		Manager.WaterSpillMax = Size;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureSpillTest, "WEM.Nature.Spill", WEMRoomManagerTests::TestFlags)

bool FNatureSpillTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	UPlatformTileData* Square = Manager->Tiles[0];
	KeepToForm(*Manager, ENatureForm::Ground, 4);

	// Four roofs, X and Y 16 to 48, with room around them.
	TArray<FGridPlatform> Roofs;
	for (const FIntVector& MinNode : { FIntVector(16, 16, 32), FIntVector(32, 16, 32), FIntVector(16, 32, 32), FIntVector(32, 32, 32) })
	{
		Roofs.Add(MakeSegment(MinNode, EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, Kit.CeilingInterior));
	}

	FRoomManagerTestAccess::Rebuild(*Manager, Roofs);

	// Each beat lays one piece: a field started against the roofs, then spilling out three more, a piece a beat.
	TArray<FGridPlatform> Field;

	for (int32 Beat = 0; Beat < 4; ++Beat)
	{
		const int32 Before = Manager->GetPlatformCount();
		TestTrue(FString::Printf(TEXT("Beat %d lays nature"), Beat), Manager->PlaceNextNature());
		TestEqual(FString::Printf(TEXT("Beat %d lays one piece"), Beat), Manager->GetPlatformCount(), Before + 1);

		const FGridPlatform Laid = FRoomManagerTestAccess::GetBuilt(*Manager).Last();
		ENatureForm Form = ENatureForm::Water;
		int32 Sign = 0;
		TestTrue(TEXT("It is ground"), GetNature(Laid, Form, Sign) && Form == ENatureForm::Ground);
		Field.Add(Laid);

		int32 Remaining = 0;
		const int32 Spilling = FRoomManagerTestAccess::GetSpillingFields(*Manager, Remaining);
		TestEqual(FString::Printf(TEXT("After beat %d, the field is still spilling until it has four pieces"), Beat), Spilling, Beat < 3 ? 1 : 0);
	}

	// The field lies in one sheet, with one Up.
	bool bOneSheet = true;
	for (const FGridPlatform& Piece : Field)
	{
		const int32 NormalAxis = static_cast<int32>(Piece.Normal);
		bOneSheet = bOneSheet && Piece.Normal == Field[0].Normal && Piece.Up == Field[0].Up && Piece.MinNode[NormalAxis] == Field[0].MinNode[NormalAxis];
	}

	TestTrue(TEXT("The field lies in one plane with one Up"), bOneSheet);
	TestEqual(TEXT("The counts follow"), Manager->GroundCount, 4);

	// The next beat starts another field, spilling out in turn.
	TestTrue(TEXT("The fifth beat lays nature"), Manager->PlaceNextNature());
	int32 Remaining = 0;
	TestEqual(TEXT("A new field is spilling"), FRoomManagerTestAccess::GetSpillingFields(*Manager, Remaining), 1);
	TestEqual(TEXT("With three pieces to go"), Remaining, 3);

	TestEqual(TEXT("No two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// With no nature among the structures, the beat lays nothing.
	ARoomManager* Bare = SpawnRoomManager(TestWorld.World, Rooted, TArray<TObjectPtr<UPlatformStructureData>>{ Kit.RoofExterior, Kit.CeilingInterior });
	FRoomManagerTestAccess::Rebuild(*Bare, Roofs);
	TestFalse(TEXT("No nature, nothing laid"), Bare->PlaceNextNature());
	TestEqual(TEXT("And nothing placed"), Bare->GetPlatformCount(), 4);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureCliffBeatTest, "WEM.Nature.CliffBeat", WEMRoomManagerTests::TestFlags)

bool FNatureCliffBeatTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	UPlatformTileData* Square = Manager->Tiles[0];

	// A field of four pieces, X and Y 16 to 48 at Z 32.
	TArray<FGridPlatform> Field;
	for (const FIntVector& MinNode : { FIntVector(16, 16, 32), FIntVector(32, 16, 32), FIntVector(16, 32, 32), FIntVector(32, 32, 32) })
	{
		Field.Add(MakeSegment(MinNode, EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior));
	}

	FRoomManagerTestAccess::Rebuild(*Manager, Field);

	// Cliffs alone: each beat raises one off the field's outline or carries one on.
	KeepToForm(*Manager, ENatureForm::Cliff, 1);

	for (int32 Beat = 0; Beat < 8; ++Beat)
	{
		const int32 Before = Manager->GetPlatformCount();
		TestTrue(FString::Printf(TEXT("Beat %d raises a cliff"), Beat), Manager->PlaceNextNature());
		TestEqual(FString::Printf(TEXT("Beat %d lays one piece"), Beat), Manager->GetPlatformCount(), Before + 1);
	}

	TestEqual(TEXT("Eight cliffs stand"), Manager->CliffCount, 8);

	int32 Walls = 0;
	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		ENatureForm Form = ENatureForm::Ground;
		int32 Sign = 0;

		if (GetNature(Platform, Form, Sign) && Form == ENatureForm::Cliff)
		{
			// A cliff is a wall in the field's frame: standing across the field, with the field's Up.
			Walls += Platform.Normal != EGridAxis::Z && Platform.Up == EGridFace::PosZ ? 1 : 0;
		}
	}

	TestEqual(TEXT("Every cliff stands across the field, with its Up"), Walls, 8);
	TestEqual(TEXT("No two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);

	// Water beside the field, or at a cliff's top or foot.
	KeepToForm(*Manager, ENatureForm::Water, 1);
	TestTrue(TEXT("A pool is laid"), Manager->PlaceNextNature());

	ENatureForm Form = ENatureForm::Ground;
	int32 Sign = 0;
	const FGridPlatform Pool = FRoomManagerTestAccess::GetBuilt(*Manager).Last();
	TestTrue(TEXT("It is water"), GetNature(Pool, Form, Sign) && Form == ENatureForm::Water);
	TestTrue(TEXT("Looking out along its Up"), Pool.Up == WEMGrid::MakeFace(static_cast<int32>(Pool.Normal), Sign));
	TestEqual(TEXT("Still no two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureTerraceTest, "WEM.Nature.Terrace", WEMRoomManagerTests::TestFlags)

bool FNatureTerraceTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	// No exterior in the city at all, so ground can only start at a cliff's top or foot.
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted,
		TArray<TObjectPtr<UPlatformStructureData>>{ Kit.CeilingInterior, Kit.GroundInterior, Kit.WallInterior, Kit.Ground, Kit.Cliff, Kit.Water });
	UPlatformTileData* Square = Manager->Tiles[0];
	KeepToForm(*Manager, ENatureForm::Ground, 1);

	// A field at Z 32, X 16 to 32, and a cliff rising off its edge, facing over it, X 32, Z 32 to 48.
	const FGridPlatform Field = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior);
	const FGridPlatform Rise = MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallInterior, Kit.Cliff);
	FRoomManagerTestAccess::Rebuild(*Manager, { Field, Rise });

	TestTrue(TEXT("Ground starts at the cliff"), Manager->PlaceNextNature());
	TestEqual(TEXT("Two of ground stand"), Manager->GroundCount, 2);

	const FGridPlatform Terrace = FRoomManagerTestAccess::GetBuilt(*Manager).Last();
	TestEqual(TEXT("On the cliff's top, beyond it"), Terrace.MinNode, FIntVector(32, 16, 48));
	TestEqual(TEXT("Lying flat"), Terrace.Normal, EGridAxis::Z);
	TestEqual(TEXT("Looking up"), Terrace.Up, EGridFace::PosZ);

	TestFalse(TEXT("Nowhere else for ground to start"), Manager->PlaceNextNature());

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureGrowthTest, "WEM.Nature.Growth", WEMRoomManagerTests::TestFlags)

bool FNatureGrowthTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	// Two room managers on the same seed in a larger cube: the city growing, nature beating once every third beat.
	auto Grow = [&](TArray<FGridPlatform>& OutPlatforms) -> ARoomManager*
	{
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
		Manager->GridSize = 129;
		Manager->ClearPlatforms();

		for (int32 Beat = 0; Beat < 150; ++Beat)
		{
			Manager->PlaceNextPlatform();
		}

		for (int32 Beat = 0; Beat < 600; ++Beat)
		{
			if (Beat % 3 == 2)
			{
				const int32 Before = Manager->GetPlatformCount();
				const bool bLaid = Manager->PlaceNextNature();

				if (Manager->GetPlatformCount() != Before + (bLaid ? 1 : 0))
				{
					AddError(FString::Printf(TEXT("Nature beat %d laid %d pieces"), Beat, Manager->GetPlatformCount() - Before));
				}
			}
			else
			{
				Manager->PlaceNextPlatform();
			}
		}

		OutPlatforms = FRoomManagerTestAccess::GetBuilt(*Manager);
		return Manager;
	};

	TArray<FGridPlatform> First;
	TArray<FGridPlatform> Second;
	ARoomManager* Manager = Grow(First);
	Grow(Second);

	AddInfo(FString::Printf(TEXT("%d platforms: %d ground, %d cliff, %d water."),
		Manager->GetPlatformCount(), Manager->GroundCount, Manager->CliffCount, Manager->WaterCount));

	TestTrue(TEXT("Ground grows"), Manager->GroundCount > 0);
	TestTrue(TEXT("Cliffs grow"), Manager->CliffCount > 0);
	TestTrue(TEXT("Water grows"), Manager->WaterCount > 0);
	TestEqual(TEXT("No two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	bool bSame = First.Num() == Second.Num();
	for (int32 Index = 0; bSame && Index < First.Num(); ++Index)
	{
		bSame = First[Index].MinNode == Second[Index].MinNode && First[Index].Normal == Second[Index].Normal
			&& First[Index].Up == Second[Index].Up && First[Index].PositiveStructure == Second[Index].PositiveStructure
			&& First[Index].NegativeStructure == Second[Index].NegativeStructure;
	}

	TestTrue(TEXT("The same seed grows the same world, nature and all"), bSame);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureAirTest, "WEM.Nature.Air", WEMRoomManagerTests::TestFlags)

bool FNatureAirTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
	Manager->GridSize = 129;
	Manager->ClearPlatforms();
	UPlatformTileData* Square = Manager->Tiles[0];

	// A piece of ground at Z 32, X and Y 16 to 32, with a wall of the city rising off its east edge, facing it.
	const FGridPlatform Field = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior);
	const FGridPlatform Wall = MakeSegment(FIntVector(32, 16, 32), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallInterior, Kit.WallExterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Field, Wall });
	TestEqual(TEXT("Field and wall stand"), Manager->GetPlatformCount(), 2);

	// Its air: over its opening, out along its Up, between three and five storeys high.
	TArray<TPair<FIntVector, FIntVector>> Boxes = FRoomManagerTestAccess::GetAirBoxes(*Manager);
	TestEqual(TEXT("One box of air, over its opening"), Boxes.Num(), 1);

	if (Boxes.Num() == 1)
	{
		const int32 Height = Boxes[0].Value.Z - Boxes[0].Key.Z + 1;
		AddInfo(FString::Printf(TEXT("The field keeps %d cells of air."), Height));
		TestEqual(TEXT("Between its beams"), Boxes[0].Key, FIntVector(17, 17, 33));
		TestTrue(TEXT("Between three and five storeys"), Height >= 48 && Height <= 80 && Boxes[0].Value.X == 31 && Boxes[0].Value.Y == 31);
	}

	// Nothing is built in it: no floor off the wall's top over the field. Beyond the field the air is free.
	TestFalse(TEXT("No floor over the field"), Manager->CanPlacePlatform(MakeSegment(FIntVector(16, 16, 48), EGridAxis::Z, Square, EGridFace::PosZ, nullptr, nullptr)));
	TestTrue(TEXT("A floor beyond it"), Manager->CanPlacePlatform(MakeSegment(FIntVector(32, 16, 48), EGridAxis::Z, Square, EGridFace::PosZ, nullptr, nullptr)));

	// Its outline is left open: a wall may still rise off its south edge.
	const FGridPlatform SouthWall = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Y, Square, EGridFace::PosZ, nullptr, nullptr);
	TestTrue(TEXT("A wall rises off the outline"), Manager->CanPlacePlatform(SouthWall));

	// A pool beside it, south, shares a seam with it, and the air closes over the seam: no wall rises there now. Water
	// holds nothing, so the seam stays a wall surface and only the air holds the wall back.
	FRoomManagerTestAccess::Rebuild(*Manager, { Field, MakeSegment(FIntVector(16, 0, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Water, Kit.CeilingInterior) });
	Boxes = FRoomManagerTestAccess::GetAirBoxes(*Manager);
	TestEqual(TEXT("Two openings and the seam between them"), Boxes.Num(), 3);

	if (Boxes.Num() == 3)
	{
		TestTrue(TEXT("The seam runs along its edge, short of the nodes at its ends"),
			Boxes[2].Key == FIntVector(17, 16, 33) && Boxes[2].Value.X == 31 && Boxes[2].Value.Y == 16);
	}

	TestFalse(TEXT("No wall rises off the seam"), Manager->CanPlacePlatform(SouthWall));

	// A piece is only laid where its fewest storeys of air are clear: not under a floor one storey up.
	const FGridPlatform Roof = MakeSegment(FIntVector(16, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, Kit.CeilingInterior);
	const FGridPlatform Rise = MakeSegment(FIntVector(48, 16, 32), EGridAxis::X, Square, EGridFace::PosZ, Kit.WallExterior, Kit.WallInterior);
	const FGridPlatform Over = MakeSegment(FIntVector(32, 16, 48), EGridAxis::Z, Square, EGridFace::PosZ, Kit.RoofExterior, Kit.CeilingInterior);
	FRoomManagerTestAccess::Rebuild(*Manager, { Roof, Rise, Over });
	TestEqual(TEXT("Roof, wall and floor stand"), Manager->GetPlatformCount(), 3);

	const FGridPlatform Under = MakeSegment(FIntVector(32, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior);
	TestFalse(TEXT("Ground under a floor one storey up has no air"), FRoomManagerTestAccess::IsAirClear(*Manager, Under));

	Manager->MinAirStoreys = 0;
	TestTrue(TEXT("Asking no air, it has enough"), FRoomManagerTestAccess::IsAirClear(*Manager, Under));
	Manager->MinAirStoreys = 3;

	const FGridPlatform Open = MakeSegment(FIntVector(0, 16, 32), EGridAxis::Z, Square, EGridFace::PosZ, Kit.Ground, Kit.CeilingInterior);
	TestTrue(TEXT("Beside the roof, with nothing over it, the air is clear"), FRoomManagerTestAccess::IsAirClear(*Manager, Open));

	// Upside down, the air is kept below it.
	FRoomManagerTestAccess::Rebuild(*Manager, { MakeSegment(FIntVector(16, 16, 96), EGridAxis::Z, Square, EGridFace::NegZ, Kit.CeilingInterior, Kit.Ground) });
	Boxes = FRoomManagerTestAccess::GetAirBoxes(*Manager);
	TestTrue(TEXT("Hanging ground keeps the air below it"), Boxes.Num() == 1 && Boxes[0].Value.Z == 95 && Boxes[0].Key.Z <= 95 - 47);

	// The city growing round fields with their air, and nature growing too, agrees with a full rebuild.
	Manager->ClearPlatforms();

	for (int32 Beat = 0; Beat < 120; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	for (int32 Beat = 0; Beat < 300; ++Beat)
	{
		if (Beat % 2 == 0)
		{
			Manager->PlaceNextNature();
		}
		else
		{
			Manager->PlaceNextPlatform();
		}
	}

	AddInfo(FString::Printf(TEXT("%d platforms: %d ground, %d cliff, %d water, %d boxes of air."),
		Manager->GetPlatformCount(), Manager->GroundCount, Manager->CliffCount, Manager->WaterCount, FRoomManagerTestAccess::GetAirBoxes(*Manager).Num()));

	TestTrue(TEXT("Fields keep air"), Manager->GroundCount + Manager->WaterCount > 0 && FRoomManagerTestAccess::GetAirBoxes(*Manager).Num() >= Manager->GroundCount + Manager->WaterCount);
	TestEqual(TEXT("No two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNaturePocketShapesTest, "WEM.Nature.PocketShapes", WEMRoomManagerTests::TestFlags)

bool FNaturePocketShapesTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;
	using FPocket = FRoomManagerTestAccess::FPocket;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->ZigguratTiers = 4;

	auto In = [&](const FPocket& Pocket, const int32 X, const int32 Y, const int32 Z)
	{
		return FRoomManagerTestAccess::IsCellInPocket(*Manager, Pocket, FIntVector(X, Y, Z));
	};

	// Every pocket is centred on the middle of cell (32, 32, 32).
	const FVector Centre(32.5, 32.5, 32.5);

	const FPocket Sphere{ EAirPocketShape::Sphere, Centre, FVector(10.0), 2, 1, 0 };
	TestTrue(TEXT("A sphere holds its centre"), In(Sphere, 32, 32, 32));
	TestTrue(TEXT("And a cell its radius out"), In(Sphere, 32, 32, 42));
	TestFalse(TEXT("But not one further"), In(Sphere, 32, 32, 43));
	TestTrue(TEXT("Nor is it a box: a cell 7 out along two axes is in"), In(Sphere, 39, 39, 32));
	TestFalse(TEXT("And one 8 out along two axes is not"), In(Sphere, 40, 40, 32));

	const FPocket Cube{ EAirPocketShape::Box, Centre, FVector(6.0), 2, 1, 0 };
	TestTrue(TEXT("A cube reaches its half side"), In(Cube, 38, 38, 38));
	TestFalse(TEXT("And no further"), In(Cube, 39, 32, 32));

	// A rectangular box standing along Z is 2 across X, 4 across Y and 6 along Z, each a half; along X, the same
	// measures run Y, Z, X.
	const FPocket Upright{ EAirPocketShape::Rectangular, Centre, FVector(2.0, 4.0, 6.0), 2, 1, 0 };
	TestTrue(TEXT("Upright: 2 across X"), In(Upright, 34, 32, 32) && !In(Upright, 35, 32, 32));
	TestTrue(TEXT("Upright: 4 across Y"), In(Upright, 32, 36, 32) && !In(Upright, 32, 37, 32));
	TestTrue(TEXT("Upright: 6 along Z"), In(Upright, 32, 32, 38) && !In(Upright, 32, 32, 39));

	const FPocket Lying{ EAirPocketShape::Rectangular, Centre, FVector(2.0, 4.0, 6.0), 0, 1, 0 };
	TestTrue(TEXT("Lying along X: 6 along X"), In(Lying, 38, 32, 32) && !In(Lying, 39, 32, 32));
	TestTrue(TEXT("Lying along X: 2 across Y"), In(Lying, 32, 34, 32) && !In(Lying, 32, 35, 32));
	TestTrue(TEXT("Lying along X: 4 across Z"), In(Lying, 32, 32, 36) && !In(Lying, 32, 32, 37));

	const FPocket Cylinder{ EAirPocketShape::Cylinder, Centre, FVector(5.0, 5.0, 3.0), 0, 1, 0 };
	TestTrue(TEXT("A cylinder along X reaches its half length"), In(Cylinder, 35, 32, 32) && !In(Cylinder, 36, 32, 32));
	TestTrue(TEXT("And its radius round it"), In(Cylinder, 32, 37, 32) && !In(Cylinder, 32, 38, 32) && In(Cylinder, 35, 36, 35));

	// A ziggurat 16 across its base and 16 tall, in four tiers of 4: its base 8 out from its axis, its top tier 2.
	const FPocket Ziggurat{ EAirPocketShape::Ziggurat, Centre, FVector(8.0, 8.0, 8.0), 2, 1, 0 };
	TestTrue(TEXT("Its base reaches 8 out"), In(Ziggurat, 40, 32, 25) && !In(Ziggurat, 41, 32, 25));
	TestTrue(TEXT("Its top tier 2"), In(Ziggurat, 34, 32, 39) && !In(Ziggurat, 35, 32, 39));

	const FPocket Inverted{ EAirPocketShape::Ziggurat, Centre, FVector(8.0, 8.0, 8.0), 2, -1, 0 };
	TestTrue(TEXT("Turned the other way, its base is on top"), In(Inverted, 40, 32, 39) && !In(Inverted, 35, 32, 25));

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureAirPocketsTest, "WEM.Nature.AirPockets", WEMRoomManagerTests::TestFlags)

bool FNatureAirPocketsTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;
	using FPocket = FRoomManagerTestAccess::FPocket;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	auto SpawnWithPockets = [&](const int32 Seed) -> ARoomManager*
	{
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
		Manager->GridSize = 129;
		Manager->PlatformRandomSeed = Seed;
		Manager->AirPocketCount = 8;
		Manager->MinAirPocketSize = 16;
		Manager->MaxAirPocketSize = 40;
		Manager->ClearPlatforms();
		return Manager;
	};

	ARoomManager* Manager = SpawnWithPockets(1234);
	const TArray<FPocket> Pockets = FRoomManagerTestAccess::GetAirPockets(*Manager);
	AddInfo(Manager->AirPocketSummary);

	TestEqual(TEXT("Eight pockets are drawn"), Pockets.Num(), 8);
	TestTrue(TEXT("Each holds lattice pieces back"), !Pockets.ContainsByPredicate([](const FPocket& Pocket) { return Pocket.CutPieces == 0; }));
	TestTrue(TEXT("As obstacles"), FRoomManagerTestAccess::GetObstaclePieceCount(*Manager) > 0);

	TArray<EAirPocketShape> Shapes;
	for (const FPocket& Pocket : Pockets)
	{
		Shapes.AddUnique(Pocket.Shape);
		TestTrue(TEXT("Every measure lies between the smallest and largest"),
			Pocket.HalfSize.GetMin() >= 8.0 && Pocket.HalfSize.GetMax() <= 20.0);
	}

	TestTrue(TEXT("More than one shape turns up"), Shapes.Num() > 1);

	// Growth starts, and never builds into a pocket.
	TestTrue(TEXT("The first platform still has its place"), Manager->PlaceNextPlatform());

	for (int32 Beat = 0; Beat < 400; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	int32 CellsInPockets = 0;
	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		const int32 SpanU = Platform.bTurned ? Platform.Tile->Length : Platform.Tile->Width;
		const int32 SpanV = Platform.bTurned ? Platform.Tile->Width : Platform.Tile->Length;

		int32 U, V;
		WEMGrid::GetInPlaneAxes(static_cast<int32>(Platform.Normal), U, V);

		for (int32 StepV = 0; StepV <= SpanV; ++StepV)
		{
			for (int32 StepU = 0; StepU <= SpanU; ++StepU)
			{
				const FIntVector Cell = Platform.MinNode + WEMGrid::AxisStep(U, StepU) + WEMGrid::AxisStep(V, StepV);

				for (const FPocket& Pocket : Pockets)
				{
					CellsInPockets += FRoomManagerTestAccess::IsCellInPocket(*Manager, Pocket, Cell) ? 1 : 0;
				}
			}
		}
	}

	AddInfo(FString::Printf(TEXT("%d platforms grew round the pockets."), Manager->GetPlatformCount()));
	TestTrue(TEXT("Growth goes on"), Manager->GetPlatformCount() > 100);
	TestEqual(TEXT("No platform reaches into a pocket"), CellsInPockets, 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// A rebuild keeps them as they are, since the platforms grew round them.
	const int32 ObstaclePieces = FRoomManagerTestAccess::GetObstaclePieceCount(*Manager);
	FRoomManagerTestAccess::Rebuild(*Manager, TArray<FGridPlatform>(FRoomManagerTestAccess::GetBuilt(*Manager)));
	TestEqual(TEXT("A rebuild keeps the pockets"), FRoomManagerTestAccess::GetAirPockets(*Manager).Num(), 8);
	TestEqual(TEXT("And what they hold back"), FRoomManagerTestAccess::GetObstaclePieceCount(*Manager), ObstaclePieces);

	// The same seed draws the same pockets, another seed others.
	const TArray<FPocket> Again = FRoomManagerTestAccess::GetAirPockets(*SpawnWithPockets(1234));
	const TArray<FPocket> Other = FRoomManagerTestAccess::GetAirPockets(*SpawnWithPockets(4321));

	bool bSame = Again.Num() == Pockets.Num();
	for (int32 Index = 0; bSame && Index < Pockets.Num(); ++Index)
	{
		bSame = Again[Index].Shape == Pockets[Index].Shape && Again[Index].Centre.Equals(Pockets[Index].Centre) && Again[Index].CutPieces == Pockets[Index].CutPieces;
	}

	TestTrue(TEXT("The same seed draws the same pockets"), bSame);
	TestTrue(TEXT("Another seed draws others"), Other.Num() > 0 && !Other[0].Centre.Equals(Pockets[0].Centre));

	// Settings: one shape only, upright; none at all.
	ARoomManager* Ziggurats = SpawnRoomManager(TestWorld.World, Rooted);
	Ziggurats->GridSize = 129;
	Ziggurats->AirPocketCount = 5;
	Ziggurats->SphereWeight = 0.0f;
	Ziggurats->BoxWeight = 0.0f;
	Ziggurats->RectangularWeight = 0.0f;
	Ziggurats->CylinderWeight = 0.0f;
	Ziggurats->bAirPocketsAlongAnyAxis = false;
	Ziggurats->ClearPlatforms();

	const TArray<FPocket> Upright = FRoomManagerTestAccess::GetAirPockets(*Ziggurats);
	TestEqual(TEXT("Five ziggurats"), Upright.Num(), 5);
	TestTrue(TEXT("All ziggurats, all upright"), !Upright.ContainsByPredicate([](const FPocket& Pocket)
	{
		return Pocket.Shape != EAirPocketShape::Ziggurat || Pocket.Axis != 2 || Pocket.AxisSign != 1;
	}));

	Ziggurats->AirPocketCount = 0;
	Ziggurats->ClearPlatforms();
	TestEqual(TEXT("None asked for, none drawn"), FRoomManagerTestAccess::GetAirPockets(*Ziggurats).Num(), 0);
	TestEqual(TEXT("And nothing held back"), FRoomManagerTestAccess::GetObstaclePieceCount(*Ziggurats), 0);

	// Pockets as large as the cube cannot all stay clear of the first platform's place: those that cannot are left
	// out, and growth still starts.
	ARoomManager* Crowded = SpawnRoomManager(TestWorld.World, Rooted);
	Crowded->GridSize = 129;
	Crowded->AirPocketCount = 6;
	Crowded->MinAirPocketSize = 110;
	Crowded->MaxAirPocketSize = 120;
	Crowded->SphereWeight = 0.0f;
	Crowded->ZigguratWeight = 0.0f;
	Crowded->CylinderWeight = 0.0f;
	Crowded->ClearPlatforms();
	AddInfo(Crowded->AirPocketSummary);

	TestTrue(TEXT("Some are left out"), FRoomManagerTestAccess::GetAirPockets(*Crowded).Num() < 6);
	TestTrue(TEXT("Growth still starts"), Crowded->PlaceNextPlatform());

	ReleaseRooted(Rooted);
	return true;
}

namespace NatureTestsPrivate
{
	/** The test kit's structures, as the importer made them. */
	TArray<TObjectPtr<UPlatformStructureData>> LoadNatureTestKit()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.WaitForCompletion();

		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game/Structures/TestKit"));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UPlatformStructureData::StaticClass()->GetClassPathName());

		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });

		TArray<TObjectPtr<UPlatformStructureData>> Structures;
		for (const FAssetData& Asset : Assets)
		{
			if (UPlatformStructureData* Structure = Cast<UPlatformStructureData>(Asset.GetAsset()))
			{
				Structures.Add(Structure);
			}
		}

		return Structures;
	}

	/** Mean and longest times, in milliseconds. */
	struct FBeatTimes
	{
		double Total = 0.0;
		double Longest = 0.0;
		int32 Beats = 0;

		void Add(const double Seconds)
		{
			Total += 1000.0 * Seconds;
			Longest = FMath::Max(Longest, 1000.0 * Seconds);
			++Beats;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("%d beats, mean %.3f ms, longest %.3f ms"), Beats, Beats > 0 ? Total / Beats : 0.0, Longest);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNaturePerfTest, "WEM.Nature.Perf", WEMRoomManagerTests::SlowTestFlags)

bool FNaturePerfTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	// A room manager set up as NewMap's - a 509-cell cube, the test kit, pieces built, the overlay off - grown to
	// 3000 platforms, a nature beat after every tenth beat of the city, timing the beats after the first 1000.
	// Then the same without nature or pockets, for the city's beat as it was.
	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	TArray<TObjectPtr<UPlatformStructureData>> Structures = LoadNatureTestKit();
	TestTrue(TEXT("The test kit is there"), Structures.Num() > 60);

	UPlatformStructureData* Ground = MakeNature(TEXT("PerfGround"), ENatureForm::Ground);
	UPlatformStructureData* Cliff = MakeNature(TEXT("PerfCliff"), ENatureForm::Cliff);
	UPlatformStructureData* Water = MakeNature(TEXT("PerfWater"), ENatureForm::Water);
	Rooted.Append({ Ground, Cliff, Water });

	for (const bool bNature : { true, false })
	{
		TArray<TObjectPtr<UPlatformStructureData>> Fills = Structures;

		if (bNature)
		{
			Fills.Append({ Ground, Cliff, Water });
		}

		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Fills);
		Manager->Tiles[1]->Orientation = ETileOrientation::HorizontalOnly;
		Manager->Tiles[2]->bCanTurn = false;
		Manager->GridSize = 509;
		Manager->PlatformRandomSeed = 4242;
		Manager->bShowPieces = true;
		Manager->AirPocketCount = bNature ? 6 : 0;

		const double ClearStart = FPlatformTime::Seconds();
		Manager->ClearPlatforms();
		const double ClearSeconds = FPlatformTime::Seconds() - ClearStart;

		FBeatTimes CityBeats;
		FBeatTimes NatureBeats;
		int32 Beat = 0;

		while (Manager->GetPlatformCount() < 3000)
		{
			const bool bMeasure = Manager->GetPlatformCount() >= 1000;
			const double Start = FPlatformTime::Seconds();

			if (!Manager->PlaceNextPlatform())
			{
				break;
			}

			if (bMeasure)
			{
				CityBeats.Add(FPlatformTime::Seconds() - Start);
			}

			if (bNature && ++Beat % 10 == 0)
			{
				const double NatureStart = FPlatformTime::Seconds();
				Manager->PlaceNextNature();

				if (bMeasure)
				{
					NatureBeats.Add(FPlatformTime::Seconds() - NatureStart);
				}
			}
		}

		AddInfo(FString::Printf(TEXT("%s: %d platforms (%d ground, %d cliff, %d water), clearing %.1f ms (%s)."),
			bNature ? TEXT("With nature and pockets") : TEXT("City alone"),
			Manager->GetPlatformCount(), Manager->GroundCount, Manager->CliffCount, Manager->WaterCount, 1000.0 * ClearSeconds, *Manager->AirPocketSummary));
		AddInfo(FString::Printf(TEXT("  City beats: %s."), *CityBeats.Describe()));

		if (bNature)
		{
			AddInfo(FString::Printf(TEXT("  Nature beats: %s."), *NatureBeats.Describe()));
			TestEqual(TEXT("No two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);
		}

		Manager->Destroy();
	}

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureFacingTest, "WEM.Nature.Facing", WEMRoomManagerTests::TestFlags)

bool FNatureFacingTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	FNatureKit Kit(Rooted);

	// How the nature that grew faces: ground and water by where their face looks, cliffs by their Up.
	struct FTally
	{
		int32 Up = 0;
		int32 Down = 0;
		int32 Standing = 0;
		int32 Water = 0;
		int32 WaterNotUp = 0;
		int32 CliffsUpright = 0;
		int32 CliffsHanging = 0;
		int32 CliffsSideways = 0;
	};

	auto Grow = [&](const ENatureFacing Facing) -> FTally
	{
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Kit.GetAll());
		Manager->GridSize = 129;
		Manager->NatureFacing = Facing;
		Manager->WaterWeight = 1.5f;
		Manager->ClearPlatforms();

		for (int32 Beat = 0; Beat < 150; ++Beat)
		{
			Manager->PlaceNextPlatform();
		}

		for (int32 Beat = 0; Beat < 600; ++Beat)
		{
			if (Beat % 3 == 2)
			{
				Manager->PlaceNextNature();
			}
			else
			{
				Manager->PlaceNextPlatform();
			}
		}

		FTally Tally;

		for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
		{
			ENatureForm Form = ENatureForm::Ground;
			int32 Sign = 0;

			if (!GetNature(Platform, Form, Sign))
			{
				continue;
			}

			if (Form == ENatureForm::Cliff)
			{
				(Platform.Up == EGridFace::PosZ ? Tally.CliffsUpright : (Platform.Up == EGridFace::NegZ ? Tally.CliffsHanging : Tally.CliffsSideways))++;
				continue;
			}

			// A field's face looks along its Up.
			const bool bUp = Platform.Up == EGridFace::PosZ;
			(bUp ? Tally.Up : (Platform.Up == EGridFace::NegZ ? Tally.Down : Tally.Standing))++;

			if (Form == ENatureForm::Water)
			{
				++Tally.Water;
				Tally.WaterNotUp += bUp ? 0 : 1;
			}
		}

		AddInfo(FString::Printf(TEXT("%s: %d ground and water looking up, %d down, %d standing (%d water, %d of it not looking up); cliffs %d upright, %d hanging, %d on their side."),
			*StaticEnum<ENatureFacing>()->GetNameStringByValue(static_cast<int64>(Facing)),
			Tally.Up, Tally.Down, Tally.Standing, Tally.Water, Tally.WaterNotUp, Tally.CliffsUpright, Tally.CliffsHanging, Tally.CliffsSideways));

		TestEqual(TEXT("No two faces meet against nature's rules"), FRoomManagerTestAccess::CountIllegalNatureMeetings(*Manager), 0);
		return Tally;
	};

	// Looking up, the default: a landscape lies like one, and its cliffs stand upright.
	const FTally Up = Grow(ENatureFacing::Up);
	TestTrue(TEXT("Looking up: ground grows"), Up.Up > 0);
	TestTrue(TEXT("Looking up: water grows"), Up.Water > 0);
	TestEqual(TEXT("Looking up: nothing hangs face down"), Up.Down, 0);
	TestEqual(TEXT("Looking up: nothing stands on a facade"), Up.Standing, 0);
	TestEqual(TEXT("Looking up: water too"), Up.WaterNotUp, 0);
	TestTrue(TEXT("Looking up: every cliff stands upright"), Up.CliffsHanging == 0 && Up.CliffsSideways == 0);

	// Flat either way: some may hang, none stands.
	const FTally Flat = Grow(ENatureFacing::Flat);
	TestEqual(TEXT("Flat: nothing stands on a facade"), Flat.Standing, 0);
	TestEqual(TEXT("Flat: no cliff lies on its side"), Flat.CliffsSideways, 0);

	// Any way: nature follows Up off the facades and overhangs as the city does.
	const FTally AnyWay = Grow(ENatureFacing::AnyWay);
	TestTrue(TEXT("Any way: some of it stands or hangs"), AnyWay.Standing + AnyWay.Down > 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureMoldsTest, "WEM.Nature.Molds", WEMRoomManagerTests::TestFlags)

bool FNatureMoldsTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;
	using FPocket = FRoomManagerTestAccess::FPocket;

	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;

	ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted);
	Manager->GridSize = 129;
	Manager->MoldCount = 3;
	Manager->MoldSpread = 1.0f;
	Manager->MinMoldSize = 32;
	Manager->MaxMoldSize = 48;
	Manager->AirPocketCount = 10;
	Manager->MinAirPocketSize = 16;
	Manager->MaxAirPocketSize = 40;
	Manager->ClearPlatforms();

	AddInfo(Manager->MoldSummary);
	AddInfo(Manager->AirPocketSummary);

	const TArray<FPocket> Pockets = FRoomManagerTestAccess::GetAirPockets(*Manager);
	TArray<int32> Molds;

	for (int32 Index = 0; Index < Pockets.Num(); ++Index)
	{
		if (Pockets[Index].bMold)
		{
			Molds.Add(Index);
		}
	}

	TestEqual(TEXT("Three molds are drawn"), Molds.Num(), 3);
	TestTrue(TEXT("And pockets after them"), Pockets.Num() > Molds.Num());

	auto Meet = [](const FPocket& A, const FPocket& B)
	{
		return A.Min.X <= B.Max.X && B.Min.X <= A.Max.X && A.Min.Y <= B.Max.Y && B.Min.Y <= A.Max.Y && A.Min.Z <= B.Max.Z && B.Min.Z <= A.Max.Z;
	};

	int32 Meetings = 0;
	for (const int32 Mold : Molds)
	{
		for (int32 Other = 0; Other < Pockets.Num(); ++Other)
		{
			Meetings += Other != Mold && Meet(Pockets[Mold], Pockets[Other]) ? 1 : 0;
		}
	}

	TestEqual(TEXT("No pocket or other mold reaches into a mold's box"), Meetings, 0);

	// Each mold holds back its box but the shape it leaves open, which stands on its floor, open from below.
	for (const int32 Index : Molds)
	{
		const FPocket& Mold = Pockets[Index];
		const int32 First = (Mold.Axis + 1) % 3;
		const int32 Second = (Mold.Axis + 2) % 3;

		auto CellAt = [&](const double AlongFirst, const double AlongSecond, const double Rise)
		{
			FVector Point = Mold.Centre;
			Point[First] += AlongFirst;
			Point[Second] += AlongSecond;
			Point[Mold.Axis] += (Rise - Mold.HalfSize.Z) * Mold.AxisSign;
			return FIntVector(FMath::FloorToInt32(Point.X), FMath::FloorToInt32(Point.Y), FMath::FloorToInt32(Point.Z));
		};

		const FIntVector Floor = CellAt(0.0, 0.0, 0.6);
		const FIntVector Corner = CellAt(Mold.HalfSize.X - 0.6, Mold.HalfSize.Y - 0.6, 2.0 * Mold.HalfSize.Z - 0.6);
		const FIntVector Below = CellAt(0.0, 0.0, -1.0);

		TestTrue(TEXT("A mold has a shape open inside it"), Mold.Parts > 0);
		TestTrue(TEXT("Its shape is open on the floor"), FRoomManagerTestAccess::IsCellInMoldShape(*Manager, Index, Floor) && !FRoomManagerTestAccess::IsCellInAirPocket(*Manager, Index, Floor));
		TestTrue(TEXT("Its box's far corner is held back"), FRoomManagerTestAccess::IsCellInAirPocket(*Manager, Index, Corner));
		TestFalse(TEXT("Below its floor is not part of it"), FRoomManagerTestAccess::IsCellInAirPocket(*Manager, Index, Below));

		// Its open floor runs across the whole box; just above it, outside the shape, is held back.
		const FIntVector FloorCorner = CellAt(Mold.HalfSize.X - 0.6, Mold.HalfSize.Y - 0.6, 0.6);
		const FIntVector AboveFloorCorner = CellAt(Mold.HalfSize.X - 0.6, Mold.HalfSize.Y - 0.6, Mold.OpenFloor + 1.6);
		TestTrue(TEXT("It has its open floor"), FMath::IsNearlyEqual(Mold.OpenFloor, static_cast<double>(Manager->MoldOpenFloor)));
		TestFalse(TEXT("Its open floor reaches the box's sides"), FRoomManagerTestAccess::IsCellInAirPocket(*Manager, Index, FloorCorner));
		TestTrue(TEXT("Above the open floor, its corner is held back"), FRoomManagerTestAccess::IsCellInAirPocket(*Manager, Index, AboveFloorCorner));
	}

	// Growth never builds into what a mold or a pocket holds back, and agrees with a full rebuild.
	for (int32 Beat = 0; Beat < 400; ++Beat)
	{
		Manager->PlaceNextPlatform();
	}

	int32 HeldBack = 0;
	int32 InMolds = 0;

	for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
	{
		const int32 SpanU = Platform.bTurned ? Platform.Tile->Length : Platform.Tile->Width;
		const int32 SpanV = Platform.bTurned ? Platform.Tile->Width : Platform.Tile->Length;

		int32 U, V;
		WEMGrid::GetInPlaneAxes(static_cast<int32>(Platform.Normal), U, V);

		bool bInMold = false;

		for (int32 StepV = 0; StepV <= SpanV; ++StepV)
		{
			for (int32 StepU = 0; StepU <= SpanU; ++StepU)
			{
				const FIntVector Cell = Platform.MinNode + WEMGrid::AxisStep(U, StepU) + WEMGrid::AxisStep(V, StepV);

				for (int32 Index = 0; Index < Pockets.Num(); ++Index)
				{
					HeldBack += FRoomManagerTestAccess::IsCellInAirPocket(*Manager, Index, Cell) ? 1 : 0;
					bInMold = bInMold || (Pockets[Index].bMold && FRoomManagerTestAccess::IsCellInMoldShape(*Manager, Index, Cell));
				}
			}
		}

		InMolds += bInMold ? 1 : 0;
	}

	AddInfo(FString::Printf(TEXT("%d platforms grew, %d of them reaching into a mold's open shape."), Manager->GetPlatformCount(), InMolds));
	TestEqual(TEXT("No platform reaches into what is held back"), HeldBack, 0);
	TestEqual(TEXT("The placements agree with a full rebuild"), FRoomManagerTestAccess::Verify(*Manager), 0);

	// Combinations stack two or three shapes from the floor up, each no wider than the one below, a dome only on top.
	ARoomManager* Stacked = SpawnRoomManager(TestWorld.World, Rooted);
	Stacked->GridSize = 129;
	Stacked->MoldCount = 4;
	Stacked->MoldSpread = 1.0f;
	Stacked->MoldZigguratWeight = 0.0f;
	Stacked->MoldCylinderWeight = 0.0f;
	Stacked->MoldDomeWeight = 0.0f;
	Stacked->MoldTowerWeight = 0.0f;
	Stacked->ClearPlatforms();
	AddInfo(Stacked->MoldSummary);

	const TArray<FPocket> StackedMolds = FRoomManagerTestAccess::GetAirPockets(*Stacked);
	TestTrue(TEXT("Some combinations are drawn"), StackedMolds.Num() > 0);

	for (int32 Index = 0; Index < StackedMolds.Num(); ++Index)
	{
		const TArray<TTuple<EMoldShape, double, double, double>> Parts = FRoomManagerTestAccess::GetMoldParts(*Stacked, Index);
		TestTrue(TEXT("A combination of two or three"), StackedMolds[Index].MoldShape == EMoldShape::Combination && Parts.Num() >= 2 && Parts.Num() <= 3);

		for (int32 Part = 0; Part < Parts.Num(); ++Part)
		{
			TestTrue(TEXT("A dome only on top"), Parts[Part].Get<0>() != EMoldShape::Dome || Part == Parts.Num() - 1);

			if (Part > 0)
			{
				TestTrue(TEXT("Each starts where the one below ends"), FMath::IsNearlyEqual(Parts[Part].Get<1>(), Parts[Part - 1].Get<2>()));
				TestTrue(TEXT("No wider than the one below"), Parts[Part].Get<3>() <= Parts[Part - 1].Get<3>() + UE_DOUBLE_KINDA_SMALL_NUMBER);
			}
		}
	}

	// None asked for, none drawn.
	Stacked->MoldCount = 0;
	Stacked->ClearPlatforms();
	TestEqual(TEXT("No molds asked for, none drawn"), FRoomManagerTestAccess::GetAirPockets(*Stacked).Num(), 0);

	ReleaseRooted(Rooted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNatureMoldFillTest, "WEM.Nature.MoldFill", WEMRoomManagerTests::SlowTestFlags)

bool FNatureMoldFillTest::RunTest(const FString& Parameters)
{
	using namespace NatureTestsPrivate;
	using FPocket = FRoomManagerTestAccess::FPocket;

	// A room manager set up as NewMap's - a 509-cell cube, the test kit - with four molds and six pockets, grown to
	// 3000 platforms: how many platforms grow into each mold's open shape, and its open floor, at a few depths of floor.
	FScopedTestWorld TestWorld;
	TArray<UObject*> Rooted;
	const TArray<TObjectPtr<UPlatformStructureData>> Structures = LoadNatureTestKit();

	for (const int32 OpenFloor : { 4, 8, 20 })
	{
		const float Spread = 0.35f;
		ARoomManager* Manager = SpawnRoomManager(TestWorld.World, Rooted, Structures);
		Manager->Tiles[1]->Orientation = ETileOrientation::HorizontalOnly;
		Manager->Tiles[2]->bCanTurn = false;
		Manager->GridSize = 509;
		Manager->PlatformRandomSeed = 4242;
		Manager->MoldCount = 4;
		Manager->MoldSpread = Spread;
		Manager->MoldOpenFloor = OpenFloor;
		Manager->AirPocketCount = 6;
		Manager->ClearPlatforms();

		while (Manager->GetPlatformCount() < 3000 && Manager->PlaceNextPlatform())
		{
		}

		const TArray<FPocket> Pockets = FRoomManagerTestAccess::GetAirPockets(*Manager);
		TArray<FString> PerMold;

		for (int32 Index = 0; Index < Pockets.Num(); ++Index)
		{
			if (!Pockets[Index].bMold)
			{
				continue;
			}

			int32 Inside = 0;
			int32 InFloor = 0;
			for (const FGridPlatform& Platform : FRoomManagerTestAccess::GetBuilt(*Manager))
			{
				const int32 SpanU = Platform.bTurned ? Platform.Tile->Length : Platform.Tile->Width;
				const int32 SpanV = Platform.bTurned ? Platform.Tile->Width : Platform.Tile->Length;

				int32 U, V;
				WEMGrid::GetInPlaneAxes(static_cast<int32>(Platform.Normal), U, V);

				// Its middle cell inside the open shape.
				const FIntVector Middle = Platform.MinNode + WEMGrid::AxisStep(U, SpanU / 2) + WEMGrid::AxisStep(V, SpanV / 2);
				if (FRoomManagerTestAccess::IsCellInMoldShape(*Manager, Index, Middle))
				{
					// Up in the shape, or still in the open floor under it.
					const FPocket& Mold = Pockets[Index];
					const double Rise = (FVector(Middle) + FVector(0.5) - Mold.Centre)[Mold.Axis] * Mold.AxisSign + Mold.HalfSize.Z;
					(Rise > Mold.OpenFloor ? Inside : InFloor)++;
				}
			}

			PerMold.Add(FString::Printf(TEXT("%s %d in the shape, %d in the open floor"),
				*StaticEnum<EMoldShape>()->GetNameStringByValue(static_cast<int64>(Pockets[Index].MoldShape)), Inside, InFloor));
		}

		AddInfo(FString::Printf(TEXT("Spread %.2f, open floor %d: %d platforms; %s; platforms inside each mold: %s."),
			Spread, OpenFloor, Manager->GetPlatformCount(), *Manager->MoldSummary, *FString::Join(PerMold, TEXT(", "))));

		Manager->Destroy();
	}

	ReleaseRooted(Rooted);
	return true;
}

#endif
