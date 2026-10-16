// Copyright DreamMoon. All Rights Reserved.

#include "DstTextMerge.h"

namespace UE::DreamStateTree::Compiler::MergePrivate
{
	/** Lines replaced in Base: [BaseStart, BaseEnd) became Lines. An insertion has BaseStart == BaseEnd. */
	struct FMergeHunk
	{
		int32 BaseStart = 0;
		int32 BaseEnd = 0;
		TArray<FString> Lines;
	};

	/** Split on `\n`; a `\r` stays on its line, and joining with `\n` gives the text back. */
	TArray<FString> SplitMergeLines(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), false);
		return Lines;
	}

	/** What turns A into B, by a longest common subsequence of lines between their common start and end. */
	TArray<FMergeHunk> DiffMergeLines(const TArray<FString>& A, const TArray<FString>& B)
	{
		int32 Prefix = 0;
		while (Prefix < A.Num() && Prefix < B.Num() && A[Prefix].Equals(B[Prefix], ESearchCase::CaseSensitive))
		{
			++Prefix;
		}
		int32 Suffix = 0;
		while (Suffix < A.Num() - Prefix && Suffix < B.Num() - Prefix
			&& A[A.Num() - 1 - Suffix].Equals(B[B.Num() - 1 - Suffix], ESearchCase::CaseSensitive))
		{
			++Suffix;
		}
		const int32 N = A.Num() - Prefix - Suffix;
		const int32 M = B.Num() - Prefix - Suffix;

		// Lengths of the longest common subsequences of the tails.
		TArray<int32> Table;
		Table.SetNumZeroed((N + 1) * (M + 1));
		const auto At = [&Table, M](int32 I, int32 J) -> int32& { return Table[I * (M + 1) + J]; };
		for (int32 I = N - 1; I >= 0; --I)
		{
			for (int32 J = M - 1; J >= 0; --J)
			{
				At(I, J) = A[Prefix + I].Equals(B[Prefix + J], ESearchCase::CaseSensitive) ? At(I + 1, J + 1) + 1 : FMath::Max(At(I + 1, J), At(I, J + 1));
			}
		}

		TArray<FMergeHunk> Hunks;
		bool bOpen = false;
		int32 I = 0;
		int32 J = 0;
		while (I < N || J < M)
		{
			if (I < N && J < M && A[Prefix + I].Equals(B[Prefix + J], ESearchCase::CaseSensitive))
			{
				bOpen = false;
				++I;
				++J;
				continue;
			}
			if (!bOpen)
			{
				FMergeHunk& Hunk = Hunks.AddDefaulted_GetRef();
				Hunk.BaseStart = Prefix + I;
				Hunk.BaseEnd = Prefix + I;
				bOpen = true;
			}
			if (J < M && (I >= N || At(I, J + 1) >= At(I + 1, J)))
			{
				Hunks.Last().Lines.Add(B[Prefix + J]);
				++J;
			}
			else
			{
				Hunks.Last().BaseEnd = Prefix + I + 1;
				++I;
			}
		}
		return Hunks;
	}

	bool SameMergeHunk(const FMergeHunk& X, const FMergeHunk& Y)
	{
		if (X.BaseStart != Y.BaseStart || X.BaseEnd != Y.BaseEnd || X.Lines.Num() != Y.Lines.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < X.Lines.Num(); ++Index)
		{
			if (!X.Lines[Index].Equals(Y.Lines[Index], ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	/** Overlapping or next to each other: two edits there cannot both be taken safely. */
	bool TouchMergeHunks(const FMergeHunk& X, const FMergeHunk& Y)
	{
		return X.BaseStart <= Y.BaseEnd && Y.BaseStart <= X.BaseEnd;
	}

	FString JoinMergeLines(const TArray<FString>& Lines)
	{
		return FString::Join(Lines, TEXT("\n")).Replace(TEXT("\r"), TEXT(""));
	}
}

namespace UE::DreamStateTree::Compiler
{
	bool MergeDstTexts(const FString& Base, const FString& Ours, const FString& Theirs, FString& OutMerged, TArray<FDstMergeConflict>& OutConflicts)
	{
		using namespace MergePrivate;
		OutMerged.Reset();
		OutConflicts.Reset();

		const TArray<FString> BaseLines = SplitMergeLines(Base);
		const TArray<FMergeHunk> OurHunks = DiffMergeLines(BaseLines, SplitMergeLines(Ours));
		const TArray<FMergeHunk> TheirHunks = DiffMergeLines(BaseLines, SplitMergeLines(Theirs));

		TArray<FString> Out;
		int32 Cursor = 0;
		int32 NextOurs = 0;
		int32 NextTheirs = 0;
		const auto Take = [&Out, &Cursor, &BaseLines](const FMergeHunk& Hunk)
		{
			for (; Cursor < Hunk.BaseStart; ++Cursor)
			{
				Out.Add(BaseLines[Cursor]);
			}
			Out.Append(Hunk.Lines);
			Cursor = Hunk.BaseEnd;
		};
		while (NextOurs < OurHunks.Num() || NextTheirs < TheirHunks.Num())
		{
			const FMergeHunk* Our = OurHunks.IsValidIndex(NextOurs) ? &OurHunks[NextOurs] : nullptr;
			const FMergeHunk* Their = TheirHunks.IsValidIndex(NextTheirs) ? &TheirHunks[NextTheirs] : nullptr;
			if (Our && Their && TouchMergeHunks(*Our, *Their))
			{
				if (SameMergeHunk(*Our, *Their))
				{
					Take(*Our);
				}
				else
				{
					FDstMergeConflict& Conflict = OutConflicts.AddDefaulted_GetRef();
					Conflict.Line = FMath::Min(Our->BaseStart, Their->BaseStart) + 1;
					Conflict.Ours = JoinMergeLines(Our->Lines);
					Conflict.Theirs = JoinMergeLines(Their->Lines);
				}
				++NextOurs;
				++NextTheirs;
				continue;
			}
			if (Our && (!Their || Our->BaseStart < Their->BaseStart))
			{
				Take(*Our);
				++NextOurs;
			}
			else
			{
				Take(*Their);
				++NextTheirs;
			}
		}
		if (!OutConflicts.IsEmpty())
		{
			return false;
		}
		for (; Cursor < BaseLines.Num(); ++Cursor)
		{
			Out.Add(BaseLines[Cursor]);
		}
		OutMerged = FString::Join(Out, TEXT("\n"));
		return true;
	}
}
