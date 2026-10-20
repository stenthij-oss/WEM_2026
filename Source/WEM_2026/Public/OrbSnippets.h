// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

/**
 * The snippets of text a caught orb shows, written as plain text files rather than assets, so a
 * snippet is added by typing it into any text editor and is in the game the next time play starts.
 *
 * Every .txt file in the snippet folder is read, in name order. Within a file a snippet is a
 * paragraph: lines running on until a blank line. A line break inside a paragraph is kept, and a
 * long line is wrapped on screen. A line starting with # is a note to the writer and never shown.
 */
namespace WEMOrbSnippets
{
	/** The snippets written in one file's text, in order. */
	WEM_2026_API void Parse(const FString& Text, TArray<FString>& OutSnippets);

	/** Every snippet in every .txt file directly in Folder, files in name order. Empty when there are none. */
	WEM_2026_API void LoadFolder(const FString& Folder, TArray<FString>& OutSnippets);

	/**
	 * Where a block of text Height tall starts, top down, so that it ends above Bottom: at Start when it fits there, else
	 * moved up just as far as it has to, as long as its top - Rise above where it starts, where its leaning lines reach
	 * highest - stays below Top. False when it does not fit even then; OutStart is then as high as it may go.
	 */
	WEM_2026_API bool PlaceBlock(float Start, float Height, float Rise, float Top, float Bottom, float& OutStart);

	/** How fast a snippet of Letters is written: LettersPerSecond, or as much faster as it takes to be written within LongestSeconds. */
	WEM_2026_API float GetWritingSpeed(int32 Letters, float LettersPerSecond, float LongestSeconds);
}

/**
 * Draws snippets at random without one coming up twice: each is drawn once before any is drawn
 * again. When every one has been drawn the bag is shuffled anew, and the first of the new round is
 * never the one just shown.
 */
class WEM_2026_API FOrbSnippetBag
{
public:
	void Reset(TArray<FString> InSnippets, int32 Seed);

	/** The next snippet. Empty when the bag holds none. */
	FString Draw();

	int32 Num() const
	{
		return Snippets.Num();
	}

	/** How many have been drawn in this round, which starts again at zero once every snippet has been shown. */
	int32 GetDrawnThisRound() const
	{
		return Next;
	}

	/** How many times every snippet has been through and the bag has been shuffled again. */
	int32 GetRerolls() const
	{
		return Rerolls;
	}

private:
	void Shuffle();

	TArray<FString> Snippets;

	/** This round's order, by index into Snippets, and how far it has been drawn. */
	TArray<int32> Order;
	int32 Next = 0;
	int32 Last = INDEX_NONE;
	int32 Rerolls = 0;

	FRandomStream Stream;
};
