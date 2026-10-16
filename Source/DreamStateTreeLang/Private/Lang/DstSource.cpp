// Copyright DreamMoon. All Rights Reserved.
//
// FDstSourceText: the text plus its line table. The table is built once, in the constructor, and
// every position question is answered from it by binary search; the lexer asks for a line and column
// per token, so counting newlines per query would make lexing quadratic.

#include "Lang/DstSource.h"

#include "Misc/Paths.h"

namespace UE::DreamStateTree::Lang
{
	EDstFileKind GetDstFileKindFromPath(const FString& Path)
	{
		// The text after the last dot only: "Foo.dtr.bak" is a backup, not a source.
		const FString Extension = FPaths::GetExtension(Path);

		if (Extension.Equals(TEXT("dtr"), ESearchCase::IgnoreCase))
		{
			return EDstFileKind::StateTree;
		}
		if (Extension.Equals(TEXT("dth"), ESearchCase::IgnoreCase))
		{
			return EDstFileKind::Library;
		}
		return EDstFileKind::Unknown;
	}

	const TCHAR* LexToString(const EDstFileKind Kind)
	{
		switch (Kind)
		{
		case EDstFileKind::StateTree:
			return TEXT("StateTree");
		case EDstFileKind::Library:
			return TEXT("Library");
		default:
			return TEXT("Unknown");
		}
	}

	FDstSourceText::FDstSourceText(FString InPath, FString InText)
		: Path(MoveTemp(InPath))
		, Text(MoveTemp(InText))
	{
		// A byte-order mark says how the file was encoded, it is not text: left in, it would become
		// part of the first token and shift every column on line 1.
		if (Text.Len() > 0 && Text[0] == static_cast<TCHAR>(0xFEFF))
		{
			Text.RightChopInline(1);
		}

		// Line 1 exists even in an empty text, so every query below can rely on a non-empty table.
		LineStarts.Add(0);

		const int32 Length = Text.Len();
		for (int32 Offset = 0; Offset < Length; ++Offset)
		{
			const TCHAR Character = Text[Offset];
			if (Character == TEXT('\n'))
			{
				LineStarts.Add(Offset + 1);
			}
			else if (Character == TEXT('\r'))
			{
				// `\r\n` is one break; taking the `\n` here keeps a CRLF file from counting double.
				if (Offset + 1 < Length && Text[Offset + 1] == TEXT('\n'))
				{
					++Offset;
				}
				LineStarts.Add(Offset + 1);
			}
		}
	}

	void FDstSourceText::GetLineAndColumn(const int32 Offset, int32& OutLine, int32& OutColumn) const
	{
		// The end-of-file token points at Len() itself, and a recovering caller may point past it.
		const int32 Clamped = FMath::Clamp(Offset, 0, Text.Len());

		if (LineStarts.Num() == 0)
		{
			// Only a default-constructed text has no table; treat it as one line.
			OutLine = 1;
			OutColumn = Clamped + 1;
			return;
		}

		// The greatest line whose start is at or before the offset.
		int32 Low = 0;
		int32 High = LineStarts.Num() - 1;
		while (Low < High)
		{
			const int32 Mid = Low + ((High - Low) + 1) / 2;
			if (LineStarts[Mid] <= Clamped)
			{
				Low = Mid;
			}
			else
			{
				High = Mid - 1;
			}
		}

		OutLine = Low + 1;
		// One column per TCHAR, tabs included, so an editor that renders tabs wide still lands on the right character.
		OutColumn = (Clamped - LineStarts[Low]) + 1;
	}

	FString FDstSourceText::GetLineText(const int32 Line) const
	{
		if (Line < 1 || Line > LineStarts.Num())
		{
			return FString();
		}

		const int32 Start = LineStarts[Line - 1];
		int32 End = (Line < LineStarts.Num()) ? LineStarts[Line] : Text.Len();

		// Drop the terminator the line was cut at: `\n`, `\r\n` or a lone `\r`.
		while (End > Start && (Text[End - 1] == TEXT('\n') || Text[End - 1] == TEXT('\r')))
		{
			--End;
		}

		return Text.Mid(Start, End - Start);
	}

	FDstSpan FDstSourceText::MakeSpan(const int32 Offset, const int32 Length) const
	{
		FDstSpan Span;
		Span.Offset = FMath::Clamp(Offset, 0, Text.Len());
		Span.Length = FMath::Clamp(Length, 0, Text.Len() - Span.Offset);
		GetLineAndColumn(Span.Offset, Span.Line, Span.Column);
		return Span;
	}

	FString FDstSourceText::Slice(const FDstSpan& Span) const
	{
		const int32 Start = FMath::Clamp(Span.Offset, 0, Text.Len());
		const int32 Count = FMath::Clamp(Span.Length, 0, Text.Len() - Start);
		return Text.Mid(Start, Count);
	}
}
