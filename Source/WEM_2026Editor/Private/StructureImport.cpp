// Copyright Epic Games, Inc. All Rights Reserved.

#include "StructureImport.h"

#include "Dom/JsonObject.h"
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
		else
		{
			OutUnknown.Add(Type);
		}
	}

	return Mask;
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
