// Copyright DreamMoon. All Rights Reserved.
//
// A three-way merge of source texts by lines, for when the text changed on disk while the asset had changes of its
// own: Base is what both started from, Ours the editor's changes written into Base, Theirs the text as it is now.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamStateTree::Compiler
{
	struct FDstMergeConflict
	{
		/** 1-based, in Base. */
		int32 Line = 0;
		/** What each side made of the lines there, joined with newlines. */
		FString Ours;
		FString Theirs;
	};

	/**
	 * Both sides' changes to Base in one text. Changes that touch the same or neighbouring lines conflict unless they
	 * are the same; then OutConflicts lists them, OutMerged is left empty and the result is false.
	 */
	DREAMSTATETREECOMPILER_API bool MergeDstTexts(const FString& Base, const FString& Ours, const FString& Theirs, FString& OutMerged,
		TArray<FDstMergeConflict>& OutConflicts);
}
