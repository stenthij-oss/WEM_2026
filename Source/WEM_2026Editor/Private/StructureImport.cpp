// Copyright Epic Games, Inc. All Rights Reserved.

#include "StructureImport.h"

#include "Dom/JsonObject.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "PlatformStructureData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StructurePlacement.h"

namespace StructureImportPrivate
{
	bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& OutVector)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;

		if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
		{
			return false;
		}

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			double Value = 0.0;
			if (!(*Values)[Axis]->TryGetNumber(Value))
			{
				return false;
			}

			OutVector[Axis] = Value;
		}

		return true;
	}

	TArray<FString> ReadStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Strings;
		Object->TryGetStringArrayField(Field, Strings);
		return Strings;
	}

	/** A string as a JSON string literal, quotes and all. */
	FString Quote(const FString& Text)
	{
		FString Escaped;
		Escaped.Reserve(Text.Len() + 2);
		Escaped += TEXT('"');

		for (const TCHAR Character : Text)
		{
			switch (Character)
			{
			case TEXT('"'): Escaped += TEXT("\\\""); break;
			case TEXT('\\'): Escaped += TEXT("\\\\"); break;
			case TEXT('\n'): Escaped += TEXT("\\n"); break;
			case TEXT('\r'): Escaped += TEXT("\\r"); break;
			case TEXT('\t'): Escaped += TEXT("\\t"); break;
			default:
				if (Character < 0x20)
				{
					Escaped += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(Character));
				}
				else
				{
					Escaped += Character;
				}
			}
		}

		Escaped += TEXT('"');
		return Escaped;
	}

	FString DescribeTypes(const int32 Types)
	{
		TArray<FString> Names;
		if (Types & static_cast<int32>(EStructureType::Ground)) { Names.Add(TEXT("Ground")); }
		if (Types & static_cast<int32>(EStructureType::Ceiling)) { Names.Add(TEXT("Ceiling")); }
		if (Types & static_cast<int32>(EStructureType::Wall)) { Names.Add(TEXT("Wall")); }
		if (Types & static_cast<int32>(EStructureType::Roof)) { Names.Add(TEXT("Roof")); }
		return Names.IsEmpty() ? TEXT("none") : FString::Join(Names, TEXT(", "));
	}

	FString DescribeSide(const EStructureSide Side)
	{
		return Side == EStructureSide::Interior ? TEXT("Interior") : Side == EStructureSide::Both ? TEXT("Both") : TEXT("Exterior");
	}

	FString DescribeBool(const bool bValue)
	{
		return bValue ? TEXT("on") : TEXT("off");
	}
}

bool WEMStructureImport::ParseSettings(const FString& Json, FStructureSettings& OutSettings, FString& OutError)
{
	using namespace StructureImportPrivate;

	OutSettings = FStructureSettings();

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);

	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		OutError = TEXT("it is not valid JSON");
		return false;
	}

	// The format comes first: a file written by a newer add-on may mean something else by the same keys.
	if (!Object->TryGetNumberField(TEXT("format"), OutSettings.Format) || OutSettings.Format != SettingsFormat)
	{
		OutError = FString::Printf(TEXT("its format is %d, and this importer reads format %d"), OutSettings.Format, SettingsFormat);
		return false;
	}

	if (!Object->TryGetStringField(TEXT("name"), OutSettings.Name) || OutSettings.Name.IsEmpty())
	{
		OutError = TEXT("it has no name");
		return false;
	}

	if (!FName::IsValidXName(OutSettings.Name, INVALID_OBJECTNAME_CHARACTERS INVALID_LONGPACKAGE_CHARACTERS))
	{
		OutError = FString::Printf(TEXT("'%s' cannot name an asset"), *OutSettings.Name);
		return false;
	}

	if (!Object->TryGetNumberField(TEXT("width"), OutSettings.Width) || !Object->TryGetNumberField(TEXT("length"), OutSettings.Length)
		|| OutSettings.Width < 2 || OutSettings.Length < 2)
	{
		OutError = TEXT("it has no width and length of at least 2 cells");
		return false;
	}

	const TSharedPtr<FJsonObject>* Bounds = nullptr;

	if (!Object->TryGetObjectField(TEXT("bounds"), Bounds)
		|| !ReadVector(*Bounds, TEXT("min"), OutSettings.BoundsMin)
		|| !ReadVector(*Bounds, TEXT("max"), OutSettings.BoundsMax))
	{
		OutError = TEXT("it has no bounds");
		return false;
	}

	Object->TryGetStringField(TEXT("blend"), OutSettings.Blend);
	Object->TryGetStringField(TEXT("object"), OutSettings.Object);
	Object->TryGetBoolField(TEXT("twoSided"), OutSettings.bTwoSided);
	Object->TryGetBoolField(TEXT("loadBearing"), OutSettings.bLoadBearing);
	Object->TryGetBoolField(TEXT("holdsItems"), OutSettings.bHoldsItems);
	Object->TryGetBoolField(TEXT("fence"), OutSettings.bFence);
	Object->TryGetBoolField(TEXT("mayLieSideways"), OutSettings.bMayLieSideways);
	Object->TryGetNumberField(TEXT("weight"), OutSettings.Weight);
	Object->TryGetStringField(TEXT("exported"), OutSettings.Exported);
	OutSettings.Categories = ReadStrings(Object, TEXT("categories"));
	OutSettings.Types = ReadStrings(Object, TEXT("types"));
	Object->TryGetStringField(TEXT("side"), OutSettings.Side);

	return true;
}

int32 WEMStructureImport::MakeTypeMask(const TConstArrayView<FString> Types, TArray<FString>& OutUnknown)
{
	OutUnknown.Reset();
	int32 Mask = 0;

	for (const FString& Type : Types)
	{
		if (Type == TEXT("Ground"))
		{
			Mask |= static_cast<int32>(EStructureType::Ground);
		}
		else if (Type == TEXT("Ceiling"))
		{
			Mask |= static_cast<int32>(EStructureType::Ceiling);
		}
		else if (Type == TEXT("Wall"))
		{
			Mask |= static_cast<int32>(EStructureType::Wall);
		}
		else if (Type == TEXT("Roof"))
		{
			Mask |= static_cast<int32>(EStructureType::Roof);
		}
		else
		{
			OutUnknown.Add(Type);
		}
	}

	return Mask;
}

bool WEMStructureImport::MakeSide(const FString& Side, EStructureSide& OutSide)
{
	OutSide = Side == TEXT("Interior") ? EStructureSide::Interior : Side == TEXT("Both") ? EStructureSide::Both : EStructureSide::Exterior;
	return Side.IsEmpty() || Side == TEXT("Interior") || Side == TEXT("Both") || Side == TEXT("Exterior");
}

bool WEMStructureImport::MeasureCells(const double ExtentCm, const double CellSize, int32& OutCells)
{
	OutCells = 0;

	if (CellSize <= 0.0)
	{
		return false;
	}

	const int32 OpeningCells = FMath::RoundToInt32(ExtentCm / CellSize);

	if (OpeningCells < 1 || FMath::Abs(ExtentCm - OpeningCells * CellSize) > SizeToleranceCm)
	{
		return false;
	}

	OutCells = OpeningCells + 1;
	return true;
}

FBox WEMStructureImport::GetExpectedMeshBounds(const FStructureSettings& Settings)
{
	// Each corner of the box in Blender's axes, carried into the mesh's own frame the way the
	// importer carries them, and bounded again there.
	FBox Bounds(ForceInit);

	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector BlenderCorner(
			(Corner & 1) ? Settings.BoundsMax.X : Settings.BoundsMin.X,
			(Corner & 2) ? Settings.BoundsMax.Y : Settings.BoundsMin.Y,
			(Corner & 4) ? Settings.BoundsMax.Z : Settings.BoundsMin.Z);

		FVector MeshCorner = FVector::ZeroVector;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			MeshCorner += BlenderCorner[Axis] * 100.0 * WEMStructures::GetMeshAxisForBlenderAxis(Axis);
		}

		Bounds += MeshCorner;
	}

	return Bounds;
}

bool WEMStructureImport::BoundsMatch(const FBox& Expected, const FBox& Measured)
{
	return Expected.IsValid && Measured.IsValid
		&& Expected.Min.Equals(Measured.Min, SizeToleranceCm)
		&& Expected.Max.Equals(Measured.Max, SizeToleranceCm);
}

FString WEMStructureImport::FormatCategoryList(const TConstArrayView<TPair<FString, FString>> Categories)
{
	using namespace StructureImportPrivate;

	FString Json = FString::Printf(TEXT("{\n  \"format\": %d,\n  \"categories\": ["), SettingsFormat);

	for (int32 Index = 0; Index < Categories.Num(); ++Index)
	{
		Json += FString::Printf(TEXT("%s\n    { \"asset\": %s, \"name\": %s }"),
			Index > 0 ? TEXT(",") : TEXT(""), *Quote(Categories[Index].Key), *Quote(Categories[Index].Value));
	}

	Json += Categories.IsEmpty() ? TEXT("]\n}\n") : TEXT("\n  ]\n}\n");
	return Json;
}

TArray<FString> WEMStructureImport::DescribeChanges(const FStructureValues& Old, const FStructureValues& New)
{
	using namespace StructureImportPrivate;

	TArray<FString> Changes;

	auto Add = [&Changes](const TCHAR* Name, const FString& Before, const FString& After)
	{
		if (Before != After)
		{
			Changes.Add(FString::Printf(TEXT("%s: %s -> %s"), Name, *Before, *After));
		}
	};

	Add(TEXT("Width"), FString::FromInt(Old.Width), FString::FromInt(New.Width));
	Add(TEXT("Length"), FString::FromInt(Old.Length), FString::FromInt(New.Length));
	Add(TEXT("Two-sided"), DescribeBool(Old.bTwoSided), DescribeBool(New.bTwoSided));
	Add(TEXT("Types"), DescribeTypes(Old.Types), DescribeTypes(New.Types));
	Add(TEXT("Side"), DescribeSide(Old.Side), DescribeSide(New.Side));
	Add(TEXT("Categories"),
		Old.Categories.IsEmpty() ? TEXT("none") : FString::Join(Old.Categories, TEXT(", ")),
		New.Categories.IsEmpty() ? TEXT("none") : FString::Join(New.Categories, TEXT(", ")));
	Add(TEXT("Load-bearing"), DescribeBool(Old.bLoadBearing), DescribeBool(New.bLoadBearing));
	Add(TEXT("Holds items"), DescribeBool(Old.bHoldsItems), DescribeBool(New.bHoldsItems));
	Add(TEXT("Fence"), DescribeBool(Old.bFence), DescribeBool(New.bFence));
	Add(TEXT("May lie sideways"), DescribeBool(Old.bMayLieSideways), DescribeBool(New.bMayLieSideways));

	return Changes;
}

FString WEMStructureImport::HashFiles(const TConstArrayView<FString> Paths)
{
	FMD5 Hash;

	for (const FString& Path : Paths)
	{
		TArray<uint8> Bytes;

		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			return FString();
		}

		// Each file's length goes in ahead of it, so moving bytes from one file to the other changes the hash.
		const int64 Length = Bytes.Num();
		Hash.Update(reinterpret_cast<const uint8*>(&Length), sizeof(Length));
		Hash.Update(Bytes.GetData(), Bytes.Num());
	}

	uint8 Digest[16];
	Hash.Final(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool WEMStructureImport::MaskTranslucency(UMaterial& Material)
{
	UMaterialEditorOnlyData* Inputs = Material.GetEditorOnlyData();

	if (Material.BlendMode != BLEND_Translucent || !Inputs)
	{
		return false;
	}

	Material.Modify();

	// A mask already wired is kept; masked, the opacity input is never read, so it is let go.
	if (!Inputs->OpacityMask.IsConnected() && Inputs->Opacity.Expression)
	{
		Inputs->OpacityMask.Connect(Inputs->Opacity.OutputIndex, Inputs->Opacity.Expression);
	}

	Inputs->Opacity.Expression = nullptr;

	Material.BlendMode = BLEND_Masked;
	Material.DitherOpacityMask = true;

	// Dithered, a pixel is drawn with the chance Mask - Clip + 0.5, so at 0.5 that chance is the alpha itself.
	Material.OpacityMaskClipValue = 0.5f;

	return true;
}
