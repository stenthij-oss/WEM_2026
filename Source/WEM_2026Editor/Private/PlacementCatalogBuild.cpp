// Copyright Epic Games, Inc. All Rights Reserved.

#include "PlacementCatalogBuild.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PlacementArchetype.h"
#include "PlacementMath.h"

namespace PlacementCatalogBuildPrivate
{
	/** Tallest an object can be, in cells, and still fit between the closest parallel segments. */
	constexpr int32 SingleGapCells = 7;

	FString GetMeshFolder(const FSoftObjectPath& MeshPath)
	{
		return FPackageName::GetLongPackagePath(MeshPath.GetLongPackageName());
	}

	/** A content folder as a folder picker writes it, without the slash some pickers leave on the end. */
	FString NormaliseFolder(FString Folder)
	{
		Folder.TrimStartAndEndInline();

		while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/")))
		{
			Folder.LeftChopInline(1);
		}

		return Folder;
	}

	bool IsFolderAtOrBelow(const FString& Folder, const FString& Ancestor)
	{
		return !Ancestor.IsEmpty()
			&& (Folder.Equals(Ancestor, ESearchCase::IgnoreCase) || Folder.StartsWith(Ancestor + TEXT("/"), ESearchCase::IgnoreCase));
	}

	FString DescribeAmbiguity(TConstArrayView<UPlacementArchetype*> Tied, const TCHAR* How)
	{
		TArray<FString> Names;
		for (const UPlacementArchetype* Archetype : Tied)
		{
			Names.Add(Archetype->GetName());
		}

		return FString::Printf(TEXT("Matched %s by %s equally well; %s was taken."), *FString::Join(Names, TEXT(", ")), How, *Names[0]);
	}

	/** Surface policy and templates of the archetype a row names, or the defaults for a row that names none. */
	void GetAnalysisInputs(const UPlacementArchetype* Archetype, FSurfacePolicy& OutPolicy, TArray<FSpecialEdgeTemplate>& OutTemplates)
	{
		OutPolicy = Archetype ? Archetype->Surfaces : FSurfacePolicy();
		OutTemplates = Archetype ? Archetype->SpecialEdges : TArray<FSpecialEdgeTemplate>();
	}
}

namespace WEMCatalogBuild
{
	using namespace PlacementCatalogBuildPrivate;

	FArchetypeMatch MatchArchetype(const FSoftObjectPath& MeshPath, const TConstArrayView<UPlacementArchetype*> Archetypes)
	{
		FArchetypeMatch Match;

		// Sorted by name first, so that whichever wins a tie wins it every build.
		TArray<UPlacementArchetype*> Sorted;
		for (UPlacementArchetype* Archetype : Archetypes)
		{
			if (Archetype)
			{
				Sorted.Add(Archetype);
			}
		}

		Sorted.Sort([](const UPlacementArchetype& A, const UPlacementArchetype& B) { return A.GetName() < B.GetName(); });

		const FString Folder = GetMeshFolder(MeshPath);

		// By folder: the deepest folder that holds the mesh wins, then the highest priority.
		{
			TArray<UPlacementArchetype*> Best;
			FString BestFolder;

			for (UPlacementArchetype* Archetype : Sorted)
			{
				for (const FDirectoryPath& MatchFolder : Archetype->MatchFolders)
				{
					const FString Candidate = NormaliseFolder(MatchFolder.Path);

					if (!IsFolderAtOrBelow(Folder, Candidate))
					{
						continue;
					}

					const bool bDeeper = Candidate.Len() > BestFolder.Len();
					const bool bSameDepth = !Best.IsEmpty() && Candidate.Len() == BestFolder.Len();

					if (Best.IsEmpty() || bDeeper
						|| (bSameDepth && Archetype->MatchPriority > Best[0]->MatchPriority))
					{
						Best.Reset();
						Best.Add(Archetype);
						BestFolder = Candidate;
					}
					else if (bSameDepth && Archetype->MatchPriority == Best[0]->MatchPriority && !Best.Contains(Archetype))
					{
						Best.Add(Archetype);
					}
				}
			}

			if (!Best.IsEmpty())
			{
				Match.Archetype = Best[0];
				Match.MatchedFolder = BestFolder;

				if (Best.Num() > 1)
				{
					Match.Ambiguity = DescribeAmbiguity(Best, TEXT("folder"));
				}

				return Match;
			}
		}

		// By name: whole tokens of the asset name, ignoring case.
		TArray<FString> Tokens;
		MeshPath.GetAssetName().ParseIntoArray(Tokens, TEXT("_"), /*bCullEmpty=*/true);

		TArray<UPlacementArchetype*> Best;

		for (UPlacementArchetype* Archetype : Sorted)
		{
			const bool bMatches = Archetype->MatchNameTokens.ContainsByPredicate([&Tokens](const FString& Wanted)
			{
				return Tokens.ContainsByPredicate([&Wanted](const FString& Token) { return Token.Equals(Wanted, ESearchCase::IgnoreCase); });
			});

			if (!bMatches)
			{
				continue;
			}

			if (Best.IsEmpty() || Archetype->MatchPriority > Best[0]->MatchPriority)
			{
				Best.Reset();
				Best.Add(Archetype);
			}
			else if (Archetype->MatchPriority == Best[0]->MatchPriority)
			{
				Best.Add(Archetype);
			}
		}

		if (!Best.IsEmpty())
		{
			Match.Archetype = Best[0];

			if (Best.Num() > 1)
			{
				Match.Ambiguity = DescribeAmbiguity(Best, TEXT("name"));
			}
		}

		return Match;
	}

	FName MakeSetName(const FSoftObjectPath& MeshPath, const FArchetypeMatch& Match)
	{
		const FString Folder = GetMeshFolder(MeshPath);

		if (Match.MatchedFolder.IsEmpty() || Folder.Equals(Match.MatchedFolder, ESearchCase::IgnoreCase))
		{
			return NAME_None;
		}

		return FName(FPaths::GetCleanFilename(Folder));
	}

	void BuildRows(
		TArray<TPair<FName, FPlacementCatalogRow>>& Rows,
		const TConstArrayView<FCatalogMeshSource> Meshes,
		const TConstArrayView<UPlacementArchetype*> Archetypes,
		const FCatalogBuildOptions& Options,
		FCatalogBuildReport& OutReport)
	{
		TMap<FSoftObjectPath, int32> RowsByMesh;
		TSet<FName> UsedNames;

		for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
		{
			UsedNames.Add(Rows[RowIndex].Key);

			if (!Rows[RowIndex].Value.Mesh.IsNull())
			{
				RowsByMesh.Add(Rows[RowIndex].Value.Mesh.ToSoftObjectPath(), RowIndex);
			}
		}

		// In path order, so which of two meshes of the same name keeps the plain name never depends
		// on the order the asset registry happened to return them in.
		TArray<const FCatalogMeshSource*> Sorted;
		for (const FCatalogMeshSource& Mesh : Meshes)
		{
			Sorted.Add(&Mesh);
		}

		Sorted.Sort([](const FCatalogMeshSource& A, const FCatalogMeshSource& B) { return A.Path.ToString() < B.Path.ToString(); });

		TSet<int32> VisitedRows;

		for (const FCatalogMeshSource* Mesh : Sorted)
		{
			int32 RowIndex = INDEX_NONE;
			bool bNewRow = false;

			if (const int32* Existing = RowsByMesh.Find(Mesh->Path))
			{
				RowIndex = *Existing;
			}
			else
			{
				// Named after the mesh; a second mesh of the same name elsewhere takes a suffix.
				const FString BaseName = Mesh->Path.GetAssetName();
				FName RowName(*BaseName);

				for (int32 Suffix = 2; UsedNames.Contains(RowName); ++Suffix)
				{
					RowName = FName(*FString::Printf(TEXT("%s_%d"), *BaseName, Suffix));
				}

				if (RowName != FName(*BaseName))
				{
					OutReport.AddNote(RowName, Mesh->Path, FString::Printf(TEXT("Another mesh is already named %s, so this row is %s."), *BaseName, *RowName.ToString()));
				}

				UsedNames.Add(RowName);

				FPlacementCatalogRow NewRow;
				NewRow.Mesh = TSoftObjectPtr<UStaticMesh>(Mesh->Path);
				RowIndex = Rows.Emplace(RowName, NewRow);
				RowsByMesh.Add(Mesh->Path, RowIndex);
				bNewRow = true;
			}

			VisitedRows.Add(RowIndex);

			const FName RowName = Rows[RowIndex].Key;
			FPlacementCatalogRow& Row = Rows[RowIndex].Value;
			const FPlacementCatalogRow Before = Row;

			// The archetype, unless a person has locked it. A locked row still takes its set from
			// wherever its own archetype's folders put it.
			FArchetypeMatch Match;

			if (Row.bArchetypeLocked)
			{
				TArray<UPlacementArchetype*> Locked;
				if (Row.Archetype)
				{
					Locked.Add(Row.Archetype);
				}

				Match = MatchArchetype(Mesh->Path, Locked);
				Match.Archetype = Row.Archetype;
			}
			else
			{
				Match = MatchArchetype(Mesh->Path, Archetypes);
				Row.Archetype = Match.Archetype;
			}

			if (!Match.Ambiguity.IsEmpty())
			{
				OutReport.AddNote(RowName, Mesh->Path, Match.Ambiguity);
			}

			Row.SetName = MakeSetName(Mesh->Path, Match);

			// A row with no archetype can never be placed, so it is shown disabled until it matches.
			// Only a row the builder disabled is enabled again, never one a person turned off.
			if (!Row.Archetype)
			{
				OutReport.Unmatched.Add(RowName);
				OutReport.AddNote(RowName, Mesh->Path, TEXT("No archetype matched this mesh, so its row is disabled."));

				if (Row.bEnabled)
				{
					Row.bEnabled = false;
					Row.bDisabledByBuilder = true;
				}
			}
			else if (Row.bDisabledByBuilder)
			{
				Row.bEnabled = true;
				Row.bDisabledByBuilder = false;
			}

			FMeshAnalysisSource Source;

			if (!Mesh->Load || !Mesh->Load(Source))
			{
				OutReport.AddNote(RowName, Mesh->Path, TEXT("Its triangles could not be read, so it was not measured."), /*bError=*/true);
				continue;
			}

			FSurfacePolicy Policy;
			TArray<FSpecialEdgeTemplate> Templates;
			GetAnalysisInputs(Row.Archetype, Policy, Templates);

			const uint32 SourceHash = WEMMeshAnalysis::HashSource(Source);
			const uint32 SettingsHash = WEMMeshAnalysis::HashSettings(Source, Policy, Templates, Options.Analysis);

			const bool bCurrent = !bNewRow
				&& !Options.bFullRebuild
				&& Row.SourceHash == SourceHash
				&& Row.SettingsHash == SettingsHash
				&& FMath::IsNearlyEqual(Row.BuiltCellSize, static_cast<float>(Options.Analysis.CellSize));

			if (bCurrent)
			{
				// What it said when it was last measured still holds, so it is said again.
				TArray<FString> Lines;
				Row.BuildNotes.ParseIntoArrayLines(Lines);

				for (const FString& Line : Lines)
				{
					OutReport.AddNote(RowName, Mesh->Path, Line);
				}
			}
			else
			{
				const FMeshAnalysisResult Result = WEMMeshAnalysis::Analyse(Source, Policy, Templates, Options.Analysis);

				Row.Geometry = Result.Geometry;
				Row.SourceHash = SourceHash;
				Row.SettingsHash = SettingsHash;
				Row.BuiltCellSize = static_cast<float>(Options.Analysis.CellSize);
				Row.BuildNotes = FString::Join(Result.Warnings, TEXT("\n"));

				for (const FString& Warning : Result.Warnings)
				{
					OutReport.AddNote(RowName, Mesh->Path, Warning, /*bError=*/!Result.bValid);
				}
			}

			if (bNewRow)
			{
				OutReport.Added.Add(RowName);
			}
			else
			{
				// Anything the builder writes counts, whether or not the mesh was measured again.
				const bool bChanged = !bCurrent
					|| Row.Archetype != Before.Archetype
					|| Row.SetName != Before.SetName
					|| Row.bEnabled != Before.bEnabled;

				(bChanged ? OutReport.Updated : OutReport.Unchanged).Add(RowName);
			}
		}

		// Rows no build found a mesh for. One whose mesh has gone is disabled and reported, never
		// deleted, so whatever a person set on it is still there if the mesh comes back.
		for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
		{
			if (VisitedRows.Contains(RowIndex))
			{
				continue;
			}

			const FName RowName = Rows[RowIndex].Key;
			FPlacementCatalogRow& Row = Rows[RowIndex].Value;
			const FSoftObjectPath MeshPath = Row.Mesh.ToSoftObjectPath();

			if (!MeshPath.IsNull() && Options.DoesMeshExist && Options.DoesMeshExist(MeshPath))
			{
				OutReport.AddNote(RowName, MeshPath, TEXT("Its mesh is outside the builder's source folders, so it was left as it is."));
				continue;
			}

			OutReport.Orphaned.Add(RowName);
			OutReport.AddNote(RowName, MeshPath, TEXT("Its mesh no longer exists, so its row is disabled."));

			if (Row.bEnabled)
			{
				Row.bEnabled = false;
				Row.bDisabledByBuilder = true;
			}
		}
	}

	void ValidateRows(
		const TConstArrayView<TPair<FName, FPlacementCatalogRow>> Rows,
		const FMeshAnalysisSettings& Settings,
		const TFunction<bool(const FSoftObjectPath&, FMeshAnalysisSource&)>& LoadMesh,
		FCatalogBuildReport& OutReport)
	{
		for (const TPair<FName, FPlacementCatalogRow>& Entry : Rows)
		{
			const FName RowName = Entry.Key;
			const FPlacementCatalogRow& Row = Entry.Value;
			const FSoftObjectPath MeshPath = Row.Mesh.ToSoftObjectPath();

			if (!Row.bEnabled)
			{
				continue;
			}

			if (!Row.Archetype)
			{
				OutReport.AddNote(RowName, MeshPath, TEXT("It is enabled but has no archetype."), /*bError=*/true);
			}

			if (MeshPath.IsNull())
			{
				OutReport.AddNote(RowName, MeshPath, TEXT("It is enabled but has no mesh."), /*bError=*/true);
				continue;
			}

			if (!FMath::IsNearlyEqual(Row.BuiltCellSize, static_cast<float>(Settings.CellSize)))
			{
				OutReport.AddNote(RowName, MeshPath, FString::Printf(TEXT("It was built at a cell size of %g, not %g. Build the catalog again."),
					Row.BuiltCellSize, Settings.CellSize), /*bError=*/true);
			}

			FMeshAnalysisSource Source;

			if (LoadMesh && LoadMesh(MeshPath, Source))
			{
				FSurfacePolicy Policy;
				TArray<FSpecialEdgeTemplate> Templates;
				GetAnalysisInputs(Row.Archetype, Policy, Templates);

				if (WEMMeshAnalysis::HashSource(Source) != Row.SourceHash)
				{
					OutReport.AddNote(RowName, MeshPath, TEXT("Its mesh has changed since it was built. Build the catalog again."), /*bError=*/true);
				}
				else if (WEMMeshAnalysis::HashSettings(Source, Policy, Templates, Settings) != Row.SettingsHash)
				{
					OutReport.AddNote(RowName, MeshPath, TEXT("Its sockets, its archetype's surfaces or special edges, or the analysis settings have changed since it was built. Build the catalog again."), /*bError=*/true);
				}
			}
			else if (LoadMesh)
			{
				OutReport.AddNote(RowName, MeshPath, TEXT("Its mesh could not be read."), /*bError=*/true);
			}

			const FPlacementGeometry Effective = Row.GetEffectiveGeometry();
			const int32 CellCount = WEMPlacement::GetFootprintCellCount(Effective.SizeCells);

			if (Row.bOverrideSurfaces)
			{
				for (int32 LayerIndex = 0; LayerIndex < Row.OverrideSurfaces.Num(); ++LayerIndex)
				{
					if (Row.OverrideSurfaces[LayerIndex].CellMask.Num() != CellCount)
					{
						OutReport.AddNote(RowName, MeshPath, FString::Printf(TEXT("Surface override %d has %d cells in its mask, but the footprint has %d."),
							LayerIndex, Row.OverrideSurfaces[LayerIndex].CellMask.Num(), CellCount), /*bError=*/true);
					}
				}
			}
			else if (Row.bOverrideSize && Effective.Surfaces.Num() != Row.Geometry.Surfaces.Num())
			{
				OutReport.AddNote(RowName, MeshPath, TEXT("Its size is overridden, so the surfaces measured on its old footprint no longer fit and are dropped. Override the surfaces too."));
			}

			if (Row.bOverrideSpecialEdges)
			{
				for (const FSpecialEdge& Edge : Row.OverrideSpecialEdges)
				{
					const int32 Length = WEMPlacement::GetEdgeLength(Effective.SizeCells, Edge.Side);

					if (!WEMPlacement::IsEdgeSide(Edge.Side) || Edge.Count < 1 || Edge.Start < 0 || Edge.Start + Edge.Count > Length)
					{
						OutReport.AddNote(RowName, MeshPath, FString::Printf(TEXT("Special edge override %d runs faces %d to %d of its %s, which has %d."),
							Edge.Id, Edge.Start, Edge.Start + Edge.Count - 1, WEMPlacement::GetSideName(Edge.Side), Length), /*bError=*/true);
					}
				}
			}
			else if (Row.bOverrideSize && Effective.SpecialEdges.Num() != Row.Geometry.SpecialEdges.Num())
			{
				OutReport.AddNote(RowName, MeshPath, TEXT("Its size is overridden, so some special edges no longer fit and are dropped. Override the special edges too."));
			}

			if (Effective.SizeCells.Z > SingleGapCells)
			{
				OutReport.AddNote(RowName, MeshPath, FString::Printf(TEXT("It is %d cells tall, so it only fits where parallel segments are at least 16 cells apart."),
					Effective.SizeCells.Z));
			}
		}
	}
}
