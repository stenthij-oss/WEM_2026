// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementCatalogPreview.h"

#include "Components/LineBatchComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "PlacementArchetype.h"
#include "PlacementCatalog.h"
#include "PlacementMath.h"

namespace PlacementCatalogPreviewPrivate
{
	/** How far the drawing stands off the ground, so it does not z-fight whatever is under the preview. */
	constexpr double DrawBias = 0.5;

	constexpr double TitleSize = 10.0;
	constexpr double LabelSize = 4.0;

	/** Roughly how wide a character of a label is, as a share of its size. */
	constexpr double CharacterWidth = 0.55;

	const FLinearColor FootprintColour(0.7f, 0.7f, 0.7f);
	const FLinearColor ProblemColour(1.0f, 0.1f, 0.1f);
	const FLinearColor BackColour(0.01f, 0.01f, 0.01f);
	const FLinearColor FrontColour(1.0f, 0.85f, 0.0f);
	const FLinearColor SideColour(0.1f, 0.35f, 1.0f);
	const FLinearColor SurfaceColour(0.1f, 0.9f, 0.1f);
	const FLinearColor SpecialColour(1.0f, 0.45f, 0.0f);

	void AddLine(TArray<FBatchedLine>& Lines, const FVector& From, const FVector& To, const FLinearColor& Colour, const float Thickness)
	{
		Lines.Emplace(From, To, Colour, /*LifeTime=*/0.0f, Thickness, SDPG_World);
	}

	FLinearColor GetSideColour(const EObjectSide Side)
	{
		switch (Side)
		{
		case EObjectSide::Back: return BackColour;
		case EObjectSide::Front: return FrontColour;
		default: return SideColour;
		}
	}
}

APlacementCatalogPreview::APlacementCatalogPreview()
{
	PrimaryActorTick.bCanEverTick = false;
	bIsEditorOnlyActor = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Lines = CreateDefaultSubobject<ULineBatchComponent>(TEXT("Lines"));
	Lines->SetupAttachment(SceneRoot);
}

FVector2D APlacementCatalogPreview::Rebuild()
{
	using namespace PlacementCatalogPreviewPrivate;
	using namespace WEMPlacement;

	for (UActorComponent* Component : PreviewComponents)
	{
		if (IsValid(Component))
		{
			Component->DestroyComponent();
		}
	}

	PreviewComponents.Reset();
	Lines->Flush();

	if (!Catalog || Catalog->GetRowStruct() != FPlacementCatalogRow::StaticStruct())
	{
		return FVector2D::ZeroVector;
	}

	// Grouped by archetype, by name; rows with none come last.
	TMap<FString, TArray<TPair<FName, const FPlacementCatalogRow*>>> Groups;

	for (const TPair<FName, uint8*>& Entry : Catalog->GetRowMap())
	{
		const FPlacementCatalogRow* Row = reinterpret_cast<const FPlacementCatalogRow*>(Entry.Value);
		Groups.FindOrAdd(Row->Archetype ? Row->Archetype->GetName() : FString()).Emplace(Entry.Key, Row);
	}

	TArray<FString> GroupNames;
	Groups.GetKeys(GroupNames);
	GroupNames.Sort([](const FString& A, const FString& B) { return A.IsEmpty() != B.IsEmpty() ? B.IsEmpty() : A < B; });

	const FVector Origin = GetActorLocation();
	const double Gap = FMath::Max(2.0 * CellSize, 30.0);

	auto AddLabel = [this](const FVector& Location, const FString& Text, const double Size, const FLinearColor& Colour)
	{
		UTextRenderComponent* Label = NewObject<UTextRenderComponent>(this, NAME_None, RF_Transient);
		Label->SetupAttachment(SceneRoot);
		Label->RegisterComponent();

		// Lying flat and facing up, reading along +Y with its top toward +X: the way round it reads
		// in the editor's top view.
		Label->SetWorldLocationAndRotation(Location, FRotationMatrix::MakeFromXZ(FVector::UpVector, FVector::ForwardVector).Rotator());
		Label->SetText(FText::FromString(Text));
		Label->SetWorldSize(Size);
		Label->SetTextRenderColor(Colour.ToFColor(true));
		Label->SetHorizontalAlignment(EHTA_Left);
		Label->SetVerticalAlignment(EVRTA_TextTop);
		PreviewComponents.Add(Label);
	};

	TArray<FBatchedLine> Batch;
	double GroupTop = 0.0;
	double FarthestY = 0.0;

	for (const FString& GroupName : GroupNames)
	{
		TArray<TPair<FName, const FPlacementCatalogRow*>>& Rows = Groups[GroupName];
		Rows.Sort([](const TPair<FName, const FPlacementCatalogRow*>& A, const TPair<FName, const FPlacementCatalogRow*>& B)
		{
			return A.Key.LexicalLess(B.Key);
		});

		AddLabel(Origin + FVector(GroupTop, 0.0, DrawBias), GroupName.IsEmpty() ? TEXT("(no archetype)") : GroupName, TitleSize, GroupName.IsEmpty() ? ProblemColour : FLinearColor::White);

		// Every footprint in the group lines up its front on one line, front toward +X.
		const double FrontLine = GroupTop - 1.6 * TitleSize;
		double Along = 0.0;
		double Deepest = 0.0;

		for (const TPair<FName, const FPlacementCatalogRow*>& Entry : Rows)
		{
			const FPlacementCatalogRow& Row = *Entry.Value;
			const FPlacementGeometry Geometry = Row.GetEffectiveGeometry();
			const FIntVector& Size = Geometry.SizeCells;
			const bool bProblem = !Row.bEnabled || !Row.Archetype;

			const double Depth = Size.X * CellSize;
			const double Width = Size.Y * CellSize;
			const FVector Corner = Origin + FVector(FrontLine - Depth, Along, 0.0);
			const FVector Centre = Corner + FVector(0.5 * Depth, 0.5 * Width, 0.0);

			// Stood as the placer would stand it on a floor, facing +X.
			const FPoseFrame Frame{};
			const FBox Bounds = Geometry.LocalBoundsCm.IsValid ? Geometry.LocalBoundsCm : FBox(FVector::ZeroVector, FVector(Size) * CellSize);
			const FTransform Transform = MakeObjectWorldTransform(Frame, Centre, Bounds, Size, CellSize, 0);

			if (UStaticMesh* Mesh = Row.Mesh.LoadSynchronous())
			{
				UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
				MeshComponent->SetStaticMesh(Mesh);
				MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				MeshComponent->SetupAttachment(SceneRoot);
				MeshComponent->RegisterComponent();
				MeshComponent->SetWorldTransform(Transform);
				PreviewComponents.Add(MeshComponent);
			}

			// The footprint, red for a row that will not be placed.
			const FVector Bias(0.0, 0.0, DrawBias);
			const FLinearColor OutlineColour = bProblem ? ProblemColour : FootprintColour;
			const FVector Corners[4] = { Corner, Corner + FVector(Depth, 0.0, 0.0), Corner + FVector(Depth, Width, 0.0), Corner + FVector(0.0, Width, 0.0) };

			for (int32 Index = 0; Index < 4; ++Index)
			{
				AddLine(Batch, Corners[Index] + Bias, Corners[(Index + 1) % 4] + Bias, OutlineColour, bProblem ? 2.0f : 1.0f);
			}

			// Its edge faces, and an arrow out of its front.
			TArray<FEdgeFace> Faces;
			GatherEdgeFaces(Size, {}, Faces);

			auto CellCentre = [&](const FIntPoint& Cell)
			{
				return Corner + FVector((Cell.X + 0.5) * CellSize, (Cell.Y + 0.5) * CellSize, 0.0) + Bias * 2.0;
			};

			for (const FEdgeFace& Face : Faces)
			{
				const FVector Outward(Face.Direction.X, Face.Direction.Y, 0.0);
				const FVector AlongFace(-Face.Direction.Y, Face.Direction.X, 0.0);
				const FVector Middle = CellCentre(Face.Cell) + Outward * (0.5 * CellSize);
				const FLinearColor Colour = Face.bCorner ? FMath::Lerp(GetSideColour(Face.Side), FootprintColour, 0.6f) : GetSideColour(Face.Side);

				AddLine(Batch, Middle - AlongFace * (0.42 * CellSize), Middle + AlongFace * (0.42 * CellSize), Colour, 3.0f);
			}

			const FVector ArrowTip = Centre + FVector(0.5 * Depth + CellSize, 0.0, 0.0) + Bias;
			AddLine(Batch, Centre + Bias, ArrowTip, FrontColour, 1.5f);
			AddLine(Batch, ArrowTip, ArrowTip + FVector(-0.3 * CellSize, 0.2 * CellSize, 0.0), FrontColour, 1.5f);
			AddLine(Batch, ArrowTip, ArrowTip + FVector(-0.3 * CellSize, -0.2 * CellSize, 0.0), FrontColour, 1.5f);

			// Its surfaces, a square per cell at each layer's height on the mesh as it stands.
			for (const FDerivedSurfaceLayer& Layer : Geometry.Surfaces)
			{
				for (int32 X = 0; X < Size.X; ++X)
				{
					for (int32 Y = 0; Y < Size.Y; ++Y)
					{
						const int32 Index = GetFootprintIndex(Size, X, Y);

						if (!Layer.CellMask.IsValidIndex(Index) || Layer.CellMask[Index] == 0)
						{
							continue;
						}

						const FVector Point = Transform.TransformPosition(GetFootprintCellLocalCentre(Bounds, Size, CellSize, FIntPoint(X, Y), Layer.HeightCm)) + Bias;
						const double Half = 0.4 * CellSize;
						const FVector Square[4] = { Point + FVector(-Half, -Half, 0.0), Point + FVector(Half, -Half, 0.0), Point + FVector(Half, Half, 0.0), Point + FVector(-Half, Half, 0.0) };

						for (int32 Side = 0; Side < 4; ++Side)
						{
							AddLine(Batch, Square[Side], Square[(Side + 1) % 4], SurfaceColour, 1.0f);
						}
					}
				}
			}

			// Its special edges, set out a little from their side, with their id.
			for (const FSpecialEdge& Special : Geometry.SpecialEdges)
			{
				const FEdgeFace* First = Faces.FindByPredicate([&](const FEdgeFace& Face) { return Face.Side == Special.Side && Face.Index == Special.Start; });
				const FEdgeFace* Last = Faces.FindByPredicate([&](const FEdgeFace& Face) { return Face.Side == Special.Side && Face.Index == Special.Start + Special.Count - 1; });

				if (!First || !Last)
				{
					continue;
				}

				const FVector Outward(First->Direction.X, First->Direction.Y, 0.0);
				const FVector From = CellCentre(First->Cell) + Outward * (0.8 * CellSize);
				const FVector To = CellCentre(Last->Cell) + Outward * (0.8 * CellSize);

				AddLine(Batch, From, To, SpecialColour, 2.5f);
				AddLabel(0.5 * (From + To) + Outward * (0.3 * CellSize), FString::FromInt(Special.Id), LabelSize * 1.5, SpecialColour);
			}

			// What it is, under it as the top view shows it.
			FString Label = FString::Printf(TEXT("%s\n%s | set %s | %dx%dx%d"),
				*Entry.Key.ToString(),
				Row.Archetype ? *Row.Archetype->GetName() : TEXT("no archetype"),
				Row.GetEffectiveSetName().IsNone() ? TEXT("-") : *Row.GetEffectiveSetName().ToString(),
				Size.X, Size.Y, Size.Z);

			if (!Row.bEnabled)
			{
				Label += TEXT(" | disabled");
			}

			AddLabel(Corner + FVector(-0.3 * CellSize, 0.0, DrawBias), Label, LabelSize, bProblem ? ProblemColour : FLinearColor::White);

			const int32 LongestLine = FMath::Max(Entry.Key.ToString().Len(), Label.Len() - Entry.Key.ToString().Len() - 1);
			Along += FMath::Max(Width, LongestLine * LabelSize * CharacterWidth) + Gap;
			Deepest = FMath::Max(Deepest, Depth);
		}

		FarthestY = FMath::Max(FarthestY, Along);
		GroupTop = FrontLine - Deepest - 0.3 * CellSize - 3.0 * LabelSize - Gap;
	}

	Lines->DrawLines(Batch);

	return FVector2D(-GroupTop, FarthestY);
}
