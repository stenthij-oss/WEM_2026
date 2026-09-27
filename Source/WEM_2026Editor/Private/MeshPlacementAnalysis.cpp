// Copyright Epic Games, Inc. All Rights Reserved.

#include "MeshPlacementAnalysis.h"

#include "Misc/Crc.h"
#include "PlacementMath.h"
#include "Serialization/BufferArchive.h"

namespace MeshPlacementAnalysisPrivate
{
	/** Clearance with nothing above it. Written out as zero, which the layer data reads as unlimited. */
	constexpr double UnlimitedClearance = TNumericLimits<double>::Max();

	/**
	 * Two hits on one line closer than this are one hit. A line that runs exactly along the edge
	 * two triangles share meets both of them there, and would otherwise see the surface twice.
	 */
	constexpr double SameHitTolerance = 1.0e-3;

	/** Tallest or widest an object can be, in cells, and still fit between neighbouring segments 8 cells apart. */
	constexpr int32 SingleGapCells = 7;

	struct FTriangle
	{
		FVector P0;
		FVector P1;
		FVector P2;
		bool bFacesUp = false;
		bool bFlat = false;
	};

	/** Where a sample line meets a triangle, measured up from the object's bottom. */
	struct FHit
	{
		double Height = 0.0;
		bool bFacesUp = false;
		bool bFlat = false;

		/** Up to the next hit on the same line; UnlimitedClearance when there is none. */
		double Clearance = UnlimitedClearance;
	};

	/** One cell's claim to a surface: a height enough of its lines agree on. */
	struct FCellSurface
	{
		int32 Cell = 0;
		double Height = 0.0;
		double Clearance = UnlimitedClearance;
	};

	/** The height a vertical line through (X, Y) meets a triangle at, if it meets it at all. Edges count. */
	bool IntersectVertical(const FTriangle& Triangle, const double X, const double Y, double& OutZ)
	{
		const FVector& A = Triangle.P0;
		const FVector& B = Triangle.P1;
		const FVector& C = Triangle.P2;

		const double Denominator = (B.Y - C.Y) * (A.X - C.X) + (C.X - B.X) * (A.Y - C.Y);

		if (FMath::Abs(Denominator) < 1.0e-12)
		{
			return false;
		}

		const double WeightA = ((B.Y - C.Y) * (X - C.X) + (C.X - B.X) * (Y - C.Y)) / Denominator;
		const double WeightB = ((C.Y - A.Y) * (X - C.X) + (A.X - C.X) * (Y - C.Y)) / Denominator;
		const double WeightC = 1.0 - WeightA - WeightB;

		constexpr double EdgeTolerance = 1.0e-9;

		if (WeightA < -EdgeTolerance || WeightB < -EdgeTolerance || WeightC < -EdgeTolerance)
		{
			return false;
		}

		OutZ = WeightA * A.Z + WeightB * B.Z + WeightC * C.Z;
		return true;
	}

	double Median(TArray<double> Values)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}

		Values.Sort();
		const int32 Middle = Values.Num() / 2;

		return Values.Num() % 2 == 1 ? Values[Middle] : 0.5 * (Values[Middle - 1] + Values[Middle]);
	}

	/**
	 * Groups sorted heights into runs no taller than twice the tolerance, calling Visit(First, Count)
	 * for each. A flat surface lands in one run however its hits scatter within the tolerance.
	 */
	template <typename ItemType, typename HeightFunctor, typename VisitFunctor>
	void ForEachHeightCluster(TConstArrayView<ItemType> SortedItems, const double Tolerance, HeightFunctor&& GetHeight, VisitFunctor&& Visit)
	{
		for (int32 First = 0; First < SortedItems.Num();)
		{
			const double Start = GetHeight(SortedItems[First]);
			int32 End = First + 1;

			while (End < SortedItems.Num() && GetHeight(SortedItems[End]) - Start <= 2.0 * Tolerance)
			{
				++End;
			}

			Visit(First, End - First);
			First = End;
		}
	}

	bool IsSurfaceSocket(const FName Name)
	{
		const FString Text = Name.ToString();
		return Text.Equals(TEXT("PL_Surface"), ESearchCase::IgnoreCase) || Text.StartsWith(TEXT("PL_Surface_"), ESearchCase::IgnoreCase);
	}

	bool IsNoSurfacesSocket(const FName Name)
	{
		return Name.ToString().Equals(TEXT("PL_NoSurfaces"), ESearchCase::IgnoreCase);
	}
}

namespace WEMMeshAnalysis
{
	using namespace MeshPlacementAnalysisPrivate;

	FMeshAnalysisResult Analyse(
		const FMeshAnalysisSource& Source,
		const FSurfacePolicy& Policy,
		const TConstArrayView<FSpecialEdgeTemplate> Templates,
		const FMeshAnalysisSettings& Settings)
	{
		FMeshAnalysisResult Result;
		FPlacementGeometry& Geometry = Result.Geometry;

		const double CellSize = Settings.CellSize;
		const int32 TriangleCount = Source.Indices.Num() / 3;

		// Bounds from the vertices the triangles use, so a stray vertex nothing is drawn with cannot
		// stretch them.
		FBox Bounds(ForceInit);
		for (const int32 Index : Source.Indices)
		{
			if (Source.Positions.IsValidIndex(Index))
			{
				Bounds += FVector(Source.Positions[Index]);
			}
		}

		if (TriangleCount == 0 || !Bounds.IsValid || CellSize <= 0.0)
		{
			Result.Warnings.Add(TEXT("There are no triangles to measure."));
			return Result;
		}

		Result.bValid = true;

		const FVector SizeCm = Bounds.GetSize();
		const FIntVector Size = WEMPlacement::ComputeSizeCells(SizeCm, CellSize);
		const int32 CellCount = WEMPlacement::GetFootprintCellCount(Size);

		Geometry.SizeCells = Size;
		Geometry.LocalBoundsCm = Bounds;

		// The same grid placement lays the footprint on: centred on the bounds in X and Y, starting at
		// their bottom in Z.
		const double OriginX = Bounds.GetCenter().X - 0.5 * Size.X * CellSize;
		const double OriginY = Bounds.GetCenter().Y - 0.5 * Size.Y * CellSize;
		const double Bottom = Bounds.Min.Z;

		const double MinFlatNormalZ = FMath::Cos(FMath::DegreesToRadians(Settings.MaxSurfaceSlopeDeg));
		const double Tolerance = Settings.HeightToleranceCm;

		// Each triangle goes in the bin of every cell column its XY bounds touch, so a line only ever
		// tests the triangles near it.
		TArray<FTriangle> Triangles;
		Triangles.Reserve(TriangleCount);

		TArray<TArray<int32>> Bins;
		Bins.SetNum(CellCount);

		for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
		{
			const int32 I0 = Source.Indices[TriangleIndex * 3];
			const int32 I1 = Source.Indices[TriangleIndex * 3 + 1];
			const int32 I2 = Source.Indices[TriangleIndex * 3 + 2];

			if (!Source.Positions.IsValidIndex(I0) || !Source.Positions.IsValidIndex(I1) || !Source.Positions.IsValidIndex(I2))
			{
				continue;
			}

			FTriangle Triangle;
			Triangle.P0 = FVector(Source.Positions[I0]);
			Triangle.P1 = FVector(Source.Positions[I1]);
			Triangle.P2 = FVector(Source.Positions[I2]);

			// The engine's winding: the front is the side (P2 - P0) ^ (P1 - P0) points to.
			const FVector Normal = (Triangle.P2 - Triangle.P0) ^ (Triangle.P1 - Triangle.P0);
			const double Length = Normal.Size();

			if (Length < 1.0e-12)
			{
				continue;
			}

			const double NormalZ = Normal.Z / Length;

			// A vertical line runs alongside a vertical triangle rather than through it.
			if (FMath::Abs(NormalZ) < 1.0e-6)
			{
				continue;
			}

			Triangle.bFacesUp = NormalZ > 0.0;
			Triangle.bFlat = NormalZ >= MinFlatNormalZ;

			const int32 NewIndex = Triangles.Add(Triangle);

			const double MinX = FMath::Min3(Triangle.P0.X, Triangle.P1.X, Triangle.P2.X);
			const double MaxX = FMath::Max3(Triangle.P0.X, Triangle.P1.X, Triangle.P2.X);
			const double MinY = FMath::Min3(Triangle.P0.Y, Triangle.P1.Y, Triangle.P2.Y);
			const double MaxY = FMath::Max3(Triangle.P0.Y, Triangle.P1.Y, Triangle.P2.Y);

			const int32 FirstX = FMath::Clamp(FMath::FloorToInt32((MinX - OriginX) / CellSize), 0, Size.X - 1);
			const int32 LastX = FMath::Clamp(FMath::FloorToInt32((MaxX - OriginX) / CellSize), 0, Size.X - 1);
			const int32 FirstY = FMath::Clamp(FMath::FloorToInt32((MinY - OriginY) / CellSize), 0, Size.Y - 1);
			const int32 LastY = FMath::Clamp(FMath::FloorToInt32((MaxY - OriginY) / CellSize), 0, Size.Y - 1);

			for (int32 X = FirstX; X <= LastX; ++X)
			{
				for (int32 Y = FirstY; Y <= LastY; ++Y)
				{
					Bins[WEMPlacement::GetFootprintIndex(Size, X, Y)].Add(NewIndex);
				}
			}
		}

		// Cast every line and keep what it meets, lowest first, with each hit's clearance up to the next.
		const int32 Samples = FMath::Max(1, Settings.SamplesPerCellAxis);
		const int32 LinesPerCell = Samples * Samples;

		TArray<TArray<TArray<FHit>>> CellLines;
		CellLines.SetNum(CellCount);

		int32 LinesWithHits = 0;
		int32 LinesFirstMeetingUnderside = 0;

		for (int32 X = 0; X < Size.X; ++X)
		{
			for (int32 Y = 0; Y < Size.Y; ++Y)
			{
				const int32 Cell = WEMPlacement::GetFootprintIndex(Size, X, Y);
				TArray<TArray<FHit>>& Lines = CellLines[Cell];
				Lines.SetNum(LinesPerCell);

				for (int32 SampleX = 0; SampleX < Samples; ++SampleX)
				{
					for (int32 SampleY = 0; SampleY < Samples; ++SampleY)
					{
						const double LineX = OriginX + (X + (SampleX + 0.5) / Samples) * CellSize;
						const double LineY = OriginY + (Y + (SampleY + 0.5) / Samples) * CellSize;

						TArray<FHit> Hits;

						for (const int32 TriangleIndex : Bins[Cell])
						{
							const FTriangle& Triangle = Triangles[TriangleIndex];
							double HitZ = 0.0;

							if (IntersectVertical(Triangle, LineX, LineY, HitZ))
							{
								FHit& Hit = Hits.AddDefaulted_GetRef();
								Hit.Height = HitZ - Bottom;
								Hit.bFacesUp = Triangle.bFacesUp;
								Hit.bFlat = Triangle.bFacesUp && Triangle.bFlat;
							}
						}

						Hits.Sort([](const FHit& A, const FHit& B) { return A.Height < B.Height; });

						// Shared edges show up as the same hit twice; one of them goes.
						TArray<FHit>& Line = Lines[SampleX * Samples + SampleY];

						for (const FHit& Hit : Hits)
						{
							const bool bRepeat = !Line.IsEmpty()
								&& FMath::Abs(Hit.Height - Line.Last().Height) < SameHitTolerance
								&& Hit.bFacesUp == Line.Last().bFacesUp;

							if (!bRepeat)
							{
								Line.Add(Hit);
							}
						}

						for (int32 HitIndex = 0; HitIndex < Line.Num(); ++HitIndex)
						{
							for (int32 Above = HitIndex + 1; Above < Line.Num(); ++Above)
							{
								if (Line[Above].Height > Line[HitIndex].Height + SameHitTolerance)
								{
									Line[HitIndex].Clearance = Line[Above].Height - Line[HitIndex].Height;
									break;
								}
							}
						}

						// Looking down from above, a closed mesh wound the right way round first shows
						// its top. A line that first meets an underside is looking at a flipped face.
						if (!Line.IsEmpty())
						{
							++LinesWithHits;
							LinesFirstMeetingUnderside += Line.Last().bFacesUp ? 0 : 1;
						}
					}
				}
			}
		}

		// Open below, and whether the cell holds anything at all.
		Geometry.OpenBelowCm.Init(0.0f, CellCount);
		Geometry.FootprintMask.Init(0, CellCount);

		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			double Lowest = SizeCm.Z;
			bool bAnyHit = false;

			for (const TArray<FHit>& Line : CellLines[Cell])
			{
				if (!Line.IsEmpty())
				{
					bAnyHit = true;
					Lowest = FMath::Min(Lowest, Line[0].Height);
				}
			}

			// A leg anywhere in the cell makes it zero, since the lowest hit of any one line counts.
			Geometry.OpenBelowCm[Cell] = static_cast<float>(FMath::Max(0.0, Lowest));
			Geometry.FootprintMask[Cell] = bAnyHit ? 1 : 0;
		}

		// Each cell's surfaces: heights enough of its lines agree on, high enough, with room above them.
		const double RequiredLines = Policy.CoverageFraction * LinesPerCell - 1.0e-6;
		TArray<FCellSurface> CellSurfaces;

		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			struct FCandidate
			{
				double Height;
				double Clearance;
				int32 Line;
			};

			TArray<FCandidate> Candidates;

			for (int32 LineIndex = 0; LineIndex < LinesPerCell; ++LineIndex)
			{
				for (const FHit& Hit : CellLines[Cell][LineIndex])
				{
					if (Hit.bFlat && Hit.Clearance >= Policy.MinClearanceCm)
					{
						Candidates.Add({ Hit.Height, Hit.Clearance, LineIndex });
					}
				}
			}

			Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Height < B.Height; });

			ForEachHeightCluster(TConstArrayView<FCandidate>(Candidates), Tolerance,
				[](const FCandidate& Candidate) { return Candidate.Height; },
				[&](const int32 First, const int32 Count)
			{
				TArray<double> Heights;
				for (int32 Index = First; Index < First + Count; ++Index)
				{
					Heights.Add(Candidates[Index].Height);
				}

				const double Height = Median(Heights);

				TSet<int32> AgreeingLines;
				double Clearance = UnlimitedClearance;

				for (int32 Index = First; Index < First + Count; ++Index)
				{
					if (FMath::Abs(Candidates[Index].Height - Height) <= Tolerance)
					{
						AgreeingLines.Add(Candidates[Index].Line);
						Clearance = FMath::Min(Clearance, Candidates[Index].Clearance);
					}
				}

				if (Height >= Policy.MinHeightCm && AgreeingLines.Num() >= RequiredLines)
				{
					CellSurfaces.Add({ Cell, Height, Clearance });
				}
			});
		}

		// Layers: the cells' heights, clustered across the whole footprint.
		TArray<FDerivedSurfaceLayer> Layers;

		CellSurfaces.Sort([](const FCellSurface& A, const FCellSurface& B) { return A.Height < B.Height; });

		ForEachHeightCluster(TConstArrayView<FCellSurface>(CellSurfaces), Tolerance,
			[](const FCellSurface& Surface) { return Surface.Height; },
			[&](const int32 First, const int32 Count)
		{
			TArray<double> Heights;
			for (int32 Index = First; Index < First + Count; ++Index)
			{
				Heights.Add(CellSurfaces[Index].Height);
			}

			const double Height = Median(Heights);

			FDerivedSurfaceLayer Layer;
			Layer.HeightCm = static_cast<float>(Height);
			Layer.CellMask.Init(0, CellCount);

			double Clearance = UnlimitedClearance;
			int32 LayerCells = 0;

			for (int32 Index = First; Index < First + Count; ++Index)
			{
				const FCellSurface& Surface = CellSurfaces[Index];

				if (FMath::Abs(Surface.Height - Height) <= Tolerance && Layer.CellMask[Surface.Cell] == 0)
				{
					Layer.CellMask[Surface.Cell] = 1;
					Clearance = FMath::Min(Clearance, Surface.Clearance);
					++LayerCells;
				}
			}

			if (LayerCells >= Settings.MinLayerCells)
			{
				Layer.ClearanceCm = Clearance == UnlimitedClearance ? 0.0f : static_cast<float>(Clearance);
				Layers.Add(MoveTemp(Layer));
			}
		});

		const bool bNoSurfacesSocket = Source.Sockets.ContainsByPredicate([](const FMeshAnalysisSocket& Socket)
		{
			return IsNoSurfacesSocket(Socket.Name);
		});

		if (!Policy.bDetectSurfaces || bNoSurfacesSocket)
		{
			Layers.Reset();
		}
		else if (Policy.bTopLayerOnly && Layers.Num() > 1)
		{
			Layers.RemoveAt(0, Layers.Num() - 1);
		}

		// A PL_Surface socket pins a layer at its height whatever the coverage: every cell with a flat
		// upward hit there joins it. It replaces whatever the analysis found at the same height.
		for (const FMeshAnalysisSocket& Socket : Source.Sockets)
		{
			if (!IsSurfaceSocket(Socket.Name))
			{
				continue;
			}

			const double Height = Socket.Location.Z - Bottom;

			FDerivedSurfaceLayer Pinned;
			Pinned.HeightCm = static_cast<float>(Height);
			Pinned.CellMask.Init(0, CellCount);

			double Clearance = UnlimitedClearance;
			int32 PinnedCells = 0;

			for (int32 Cell = 0; Cell < CellCount; ++Cell)
			{
				for (const TArray<FHit>& Line : CellLines[Cell])
				{
					for (const FHit& Hit : Line)
					{
						if (Hit.bFlat && FMath::Abs(Hit.Height - Height) <= Tolerance)
						{
							PinnedCells += Pinned.CellMask[Cell] == 0 ? 1 : 0;
							Pinned.CellMask[Cell] = 1;
							Clearance = FMath::Min(Clearance, Hit.Clearance);
						}
					}
				}
			}

			if (PinnedCells == 0)
			{
				Result.Warnings.Add(FString::Printf(TEXT("Socket %s pins a surface at %.1f cm, but there is no flat upward face there."),
					*Socket.Name.ToString(), Height));
				continue;
			}

			Pinned.ClearanceCm = Clearance == UnlimitedClearance ? 0.0f : static_cast<float>(Clearance);

			Layers.RemoveAll([&](const FDerivedSurfaceLayer& Layer)
			{
				return FMath::Abs(Layer.HeightCm - Height) <= Tolerance;
			});

			Layers.Add(MoveTemp(Pinned));
		}

		Layers.Sort([](const FDerivedSurfaceLayer& A, const FDerivedSurfaceLayer& B) { return A.HeightCm < B.HeightCm; });
		Geometry.Surfaces = MoveTemp(Layers);

		ResolveSpecialEdges(Size, Geometry.OpenBelowCm, Templates, Geometry.SpecialEdges, Result.Warnings);

		if (LinesWithHits > 0 && LinesFirstMeetingUnderside * 2 > LinesWithHits)
		{
			Result.Warnings.Add(FString::Printf(TEXT("%d of %d sample lines first meet a downward face; its normals are probably flipped."),
				LinesFirstMeetingUnderside, LinesWithHits));
		}

		if (Policy.bDetectSurfaces && Policy.Accept != EAcceptMode::None && !bNoSurfacesSocket && Geometry.Surfaces.IsEmpty())
		{
			Result.Warnings.Add(TEXT("No surface was found, though its archetype takes things on its surfaces."));
		}

		if (Size.Z > SingleGapCells)
		{
			Result.Warnings.Add(FString::Printf(TEXT("It is %d cells tall, over %d, so it only fits where parallel segments are at least 16 cells apart."),
				Size.Z, SingleGapCells));
		}

		if (Size.X > SingleGapCells || Size.Y > SingleGapCells)
		{
			Result.Warnings.Add(FString::Printf(TEXT("Its footprint is %d x %d cells, wider than %d, so it only fits on segments that have joined."),
				Size.X, Size.Y, SingleGapCells));
		}

		return Result;
	}

	void ResolveSpecialEdges(
		const FIntVector& SizeCells,
		const TConstArrayView<float> OpenBelowCm,
		const TConstArrayView<FSpecialEdgeTemplate> Templates,
		TArray<FSpecialEdge>& OutEdges,
		TArray<FString>& OutWarnings)
	{
		OutEdges.Reset();

		TArray<WEMPlacement::FEdgeFace> Faces;
		WEMPlacement::GatherEdgeFaces(SizeCells, {}, Faces);

		for (const FSpecialEdgeTemplate& Template : Templates)
		{
			const FString SideName = WEMPlacement::GetSideName(Template.Side);

			if (!WEMPlacement::IsEdgeSide(Template.Side))
			{
				OutWarnings.Add(FString::Printf(TEXT("Special edge %d is on the %s, which has no edge."), Template.Id, *SideName));
				continue;
			}

			const int32 Length = WEMPlacement::GetEdgeLength(SizeCells, Template.Side);

			FSpecialEdge Edge;
			Edge.Id = Template.Id;
			Edge.Side = Template.Side;

			if (Template.Mode == ESpecialEdgeMode::Inset)
			{
				Edge.Start = Template.InsetStart;
				Edge.Count = Length - Template.InsetStart - Template.InsetEnd;

				// An inset that leaves nothing of the edge has no run to name.
				if (Edge.Count >= 1)
				{
					OutEdges.Add(Edge);
				}

				continue;
			}

			// Which faces along the side stand over enough open space, by their index along the edge.
			TArray<bool> Open;
			Open.Init(false, Length);

			for (const WEMPlacement::FEdgeFace& Face : Faces)
			{
				if (Face.Side != Template.Side || !Open.IsValidIndex(Face.Index))
				{
					continue;
				}

				const int32 Cell = WEMPlacement::GetFootprintIndex(SizeCells, Face.Cell.X, Face.Cell.Y);
				Open[Face.Index] = OpenBelowCm.IsValidIndex(Cell) && OpenBelowCm[Cell] >= Template.MinOpenBelowCm;
			}

			int32 BestStart = INDEX_NONE;
			int32 BestCount = 0;

			for (int32 Start = 0; Start < Length;)
			{
				if (!Open[Start])
				{
					++Start;
					continue;
				}

				int32 Count = 1;
				while (Start + Count < Length && Open[Start + Count])
				{
					++Count;
				}

				// Twice each centre, so the comparison stays in whole numbers.
				auto CentreOffset = [Length](const int32 RunStart, const int32 RunCount)
				{
					return FMath::Abs(2 * RunStart + RunCount - Length);
				};

				if (Count > BestCount || (Count == BestCount && CentreOffset(Start, Count) < CentreOffset(BestStart, BestCount)))
				{
					BestStart = Start;
					BestCount = Count;
				}

				Start += Count;
			}

			if (BestStart == INDEX_NONE)
			{
				OutWarnings.Add(FString::Printf(TEXT("Special edge %d found no run on its %s with %.0f cm open below."),
					Template.Id, *SideName, Template.MinOpenBelowCm));
				continue;
			}

			Edge.Start = BestStart;
			Edge.Count = BestCount;
			OutEdges.Add(Edge);
		}
	}

	uint32 HashSource(const FMeshAnalysisSource& Source)
	{
		uint32 Hash = FCrc::MemCrc32(Source.Positions.GetData(), Source.Positions.Num() * Source.Positions.GetTypeSize());
		return FCrc::MemCrc32(Source.Indices.GetData(), Source.Indices.Num() * Source.Indices.GetTypeSize(), Hash);
	}

	uint32 HashSettings(
		const FMeshAnalysisSource& Source,
		const FSurfacePolicy& Policy,
		const TConstArrayView<FSpecialEdgeTemplate> Templates,
		const FMeshAnalysisSettings& Settings)
	{
		FBufferArchive Archive;

		double CellSize = Settings.CellSize;
		int32 Samples = Settings.SamplesPerCellAxis;
		double Tolerance = Settings.HeightToleranceCm;
		double Slope = Settings.MaxSurfaceSlopeDeg;
		int32 MinLayerCells = Settings.MinLayerCells;
		Archive << CellSize << Samples << Tolerance << Slope << MinLayerCells;

		bool bDetect = Policy.bDetectSurfaces;
		float MinHeight = Policy.MinHeightCm;
		float MinClearance = Policy.MinClearanceCm;
		float Coverage = Policy.CoverageFraction;
		bool bTopOnly = Policy.bTopLayerOnly;
		uint8 Accept = static_cast<uint8>(Policy.Accept);
		Archive << bDetect << MinHeight << MinClearance << Coverage << bTopOnly << Accept;

		for (const FSpecialEdgeTemplate& Template : Templates)
		{
			int32 Id = Template.Id;
			uint8 Side = static_cast<uint8>(Template.Side);
			uint8 Mode = static_cast<uint8>(Template.Mode);
			int32 InsetStart = Template.InsetStart;
			int32 InsetEnd = Template.InsetEnd;
			float MinOpenBelow = Template.MinOpenBelowCm;
			Archive << Id << Side << Mode << InsetStart << InsetEnd << MinOpenBelow;
		}

		for (const FMeshAnalysisSocket& Socket : Source.Sockets)
		{
			FString Name = Socket.Name.ToString();
			FVector Location = Socket.Location;
			Archive << Name << Location;
		}

		return FCrc::MemCrc32(Archive.GetData(), Archive.Num());
	}
}
