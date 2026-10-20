// Copyright Epic Games, Inc. All Rights Reserved.

#include "OrbSnippets.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

void WEMOrbSnippets::Parse(const FString& Text, TArray<FString>& OutSnippets)
{
	TArray<FString> Lines;
	Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n")).ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);

	TArray<FString> Paragraph;

	auto EndParagraph = [&Paragraph, &OutSnippets]()
	{
		if (!Paragraph.IsEmpty())
		{
			OutSnippets.Add(FString::Join(Paragraph, TEXT("\n")));
			Paragraph.Reset();
		}
	};

	for (const FString& Line : Lines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();

		if (Trimmed.IsEmpty())
		{
			EndParagraph();
		}
		else if (!Trimmed.StartsWith(TEXT("#")))
		{
			Paragraph.Add(Trimmed);
		}
	}

	EndParagraph();
}

void WEMOrbSnippets::LoadFolder(const FString& Folder, TArray<FString>& OutSnippets)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.txt")), /*Files=*/true, /*Directories=*/false);
	Files.Sort();

	for (const FString& File : Files)
	{
		FString Text;

		if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, File)))
		{
			Parse(Text, OutSnippets);
		}
	}
}

bool WEMOrbSnippets::PlaceBlock(const float Start, const float Height, const float Rise, const float Top, const float Bottom, float& OutStart)
{
	if (Start + Height <= Bottom)
	{
		OutStart = Start;
		return true;
	}

	const float Raised = Bottom - Height;

	if (Raised - Rise >= Top)
	{
		OutStart = Raised;
		return true;
	}

	OutStart = FMath::Min(Start, Top + Rise);
	return false;
}

float WEMOrbSnippets::GetWritingSpeed(const int32 Letters, const float LettersPerSecond, const float LongestSeconds)
{
	return FMath::Max(FMath::Max(LettersPerSecond, 0.1f), Letters / FMath::Max(LongestSeconds, 0.1f));
}

void FOrbSnippetBag::Reset(TArray<FString> InSnippets, const int32 Seed)
{
	Snippets = MoveTemp(InSnippets);
	Stream.Initialize(Seed);
	Last = INDEX_NONE;
	Rerolls = 0;
	Shuffle();
}

FString FOrbSnippetBag::Draw()
{
	if (Snippets.IsEmpty())
	{
		return FString();
	}

	if (Next >= Order.Num())
	{
		Shuffle();
		++Rerolls;
	}

	Last = Order[Next++];
	return Snippets[Last];
}

void FOrbSnippetBag::Shuffle()
{
	Order.SetNum(Snippets.Num());

	for (int32 Index = 0; Index < Order.Num(); ++Index)
	{
		Order[Index] = Index;
	}

	for (int32 Index = Order.Num() - 1; Index > 0; --Index)
	{
		Order.Swap(Index, Stream.RandRange(0, Index));
	}

	// The round's first is never the one just shown, which would come up twice running across the reroll.
	if (Order.Num() > 1 && Order[0] == Last)
	{
		Order.Swap(0, Stream.RandRange(1, Order.Num() - 1));
	}

	Next = 0;
}
