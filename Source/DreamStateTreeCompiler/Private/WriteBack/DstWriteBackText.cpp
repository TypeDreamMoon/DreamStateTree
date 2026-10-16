// Copyright DreamMoon. All Rights Reserved.
//
// The text being edited: a parse of the source with its comments, an index of its nodes by where they start, the edits
// applied from the back to the front, and the small text operations the reprints need.

#include "WriteBack/DstWriteBackInternal.h"

namespace UE::DreamStateTree::Compiler::WriteBackPrivate
{
	// ------------------------------------------------------------------------------------------
	// Edits
	// ------------------------------------------------------------------------------------------

	FString ApplyEdits(const FString& Text, TArray<FEdit> Edits, bool& bOutOk)
	{
		bOutOk = true;

		// Inserts at an offset come before a replacement that starts there (applied from the back, the replacement goes
		// first and the insert lands in front of it); longer replacements come before the ones inside them.
		Edits.StableSort([](const FEdit& A, const FEdit& B)
		{
			if (A.Offset != B.Offset)
			{
				return A.Offset < B.Offset;
			}
			const bool bAInsert = A.Length == 0;
			const bool bBInsert = B.Length == 0;
			if (bAInsert != bBInsert)
			{
				return bAInsert;
			}
			if (A.Length != B.Length)
			{
				return A.Length > B.Length;
			}
			return A.Sequence < B.Sequence;
		});

		TArray<FEdit> Kept;
		for (const FEdit& Edit : Edits)
		{
			bool bSkip = false;
			for (const FEdit& Outer : Kept)
			{
				if (Outer.Length <= 0)
				{
					continue;
				}
				const int32 OuterEnd = Outer.Offset + Outer.Length;
				if (Edit.Length == 0)
				{
					// Inside the replaced text, not at either end of it.
					bSkip |= Edit.Offset > Outer.Offset && Edit.Offset < OuterEnd;
				}
				else if (Edit.Offset >= OuterEnd)
				{
					// After it.
				}
				else if (Edit.Offset + Edit.Length <= OuterEnd)
				{
					// Inside it: the outer edit is a reprint that holds this one already (or the same one asked twice).
					bSkip = true;
				}
				else
				{
					bOutOk = false;
				}
			}
			if (!bSkip)
			{
				Kept.Add(Edit);
			}
		}

		FString Out = Text;
		for (int32 Index = Kept.Num() - 1; Index >= 0; --Index)
		{
			const FEdit& Edit = Kept[Index];
			if (Edit.Offset < 0 || Edit.Offset + Edit.Length > Out.Len())
			{
				bOutOk = false;
				continue;
			}
			Out = Out.Left(Edit.Offset) + Edit.Text + Out.Mid(Edit.Offset + Edit.Length);
		}
		return Out;
	}

	FString SliceWithEdits(const FString& Original, const Lang::FDstSpan& Span, const TArray<FEdit>& Edits)
	{
		const FString Slice = Original.Mid(Span.Offset, Span.Length);
		TArray<FEdit> Inside;
		for (const FEdit& Edit : Edits)
		{
			if (Edit.Offset >= Span.Offset && Edit.Offset + Edit.Length <= Span.End())
			{
				FEdit Shifted = Edit;
				Shifted.Offset -= Span.Offset;
				Inside.Add(MoveTemp(Shifted));
			}
		}
		if (Inside.IsEmpty())
		{
			return Slice;
		}
		bool bOk = true;
		return ApplyEdits(Slice, MoveTemp(Inside), bOk);
	}

	FString ReindentBlock(const FString& Block, const int32 OldIndentChars, const FString& NewIndent)
	{
		TArray<FString> Lines;
		Block.ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ false);
		if (Lines.Num() <= 1)
		{
			return Block;
		}

		FString Out = Lines[0];
		for (int32 Index = 1; Index < Lines.Num(); ++Index)
		{
			Out += TEXT("\n");
			const FString& Line = Lines[Index];
			if (Line.TrimStartAndEnd().IsEmpty())
			{
				Out += Line;
				continue;
			}
			int32 Strip = 0;
			while (Strip < OldIndentChars && Strip < Line.Len() && (Line[Strip] == TEXT(' ') || Line[Strip] == TEXT('\t')))
			{
				++Strip;
			}
			Out += NewIndent;
			Out += Line.Mid(Strip);
		}
		return Out;
	}

	void CopyTrivia(Lang::FDstFile& File, const Lang::FDstNode* From, const Lang::FDstNode* To)
	{
		if (!From || !To)
		{
			return;
		}
		if (const Lang::FDstTrivia* Found = File.FindTrivia(From))
		{
			// A copy first: adding to the table may move what Found points to.
			Lang::FDstTrivia Copy = *Found;
			File.Trivia.FindOrAdd(To) = MoveTemp(Copy);
		}
	}

	// ------------------------------------------------------------------------------------------
	// The editable text
	// ------------------------------------------------------------------------------------------

	FEditableText::FEditableText(const FString& InPath, const FString& InText)
		: Source(InPath, InText)
	{
		LineEnd = Source.GetText().Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");

		Lang::FDstDiagnosticSink Sink(InPath);
		File = Lang::ParseDst(Source, Sink);
		if (!File)
		{
			return;
		}

		// Every node by where it starts, and what it sits in.
		const TFunction<void(Lang::FDstNode&, Lang::FDstNode*)> Walk = [this, &Walk](Lang::FDstNode& Node, Lang::FDstNode* Parent)
		{
			ByOffset.Add(Node.Span.Offset, &Node);
			Parents.Add(&Node, Parent);
			Lang::ForEachChild(Node, [&Walk, &Node](const Lang::FDstNode& Child)
			{
				Walk(const_cast<Lang::FDstNode&>(Child), &Node);
			});
		};
		for (const TUniquePtr<Lang::FDstInclude>& Include : File->Includes)
		{
			if (Include)
			{
				Walk(*Include, nullptr);
			}
		}
		if (File->Document)
		{
			Walk(*File->Document, nullptr);
		}
	}

	Lang::FDstNode* FEditableText::FindNode(const Lang::FDstSpan& Span, std::initializer_list<Lang::EDstNodeKind> Kinds) const
	{
		TArray<Lang::FDstNode*> Candidates;
		ByOffset.MultiFind(Span.Offset, Candidates);
		for (Lang::FDstNode* Candidate : Candidates)
		{
			if (Candidate->Span.Length != Span.Length)
			{
				continue;
			}
			for (const Lang::EDstNodeKind Kind : Kinds)
			{
				if (Candidate->Kind == Kind)
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	Lang::FDstNode* FEditableText::GetParent(const Lang::FDstNode& Node) const
	{
		Lang::FDstNode* const* Parent = Parents.Find(&Node);
		return Parent ? *Parent : nullptr;
	}

	int32 FEditableText::GetLineStart(const int32 Offset) const
	{
		const FString& Text = Source.GetText();
		int32 Start = FMath::Clamp(Offset, 0, Text.Len());
		while (Start > 0 && Text[Start - 1] != TEXT('\n') && Text[Start - 1] != TEXT('\r'))
		{
			--Start;
		}
		return Start;
	}

	int32 FEditableText::GetLineEndOffset(const int32 Offset) const
	{
		const FString& Text = Source.GetText();
		int32 End = FMath::Clamp(Offset, 0, Text.Len());
		while (End < Text.Len() && Text[End] != TEXT('\n') && Text[End] != TEXT('\r'))
		{
			++End;
		}
		return End;
	}

	int32 FEditableText::GetIndentChars(const int32 Offset) const
	{
		const FString& Text = Source.GetText();
		int32 Count = 0;
		for (int32 Index = GetLineStart(Offset); Index < Text.Len() && (Text[Index] == TEXT(' ') || Text[Index] == TEXT('\t')); ++Index)
		{
			++Count;
		}
		return Count;
	}

	int32 FEditableText::GetIndentLevel(const int32 Offset) const
	{
		const FString& Text = Source.GetText();
		int32 Columns = 0;
		for (int32 Index = GetLineStart(Offset); Index < Text.Len(); ++Index)
		{
			if (Text[Index] == TEXT(' '))
			{
				++Columns;
			}
			else if (Text[Index] == TEXT('\t'))
			{
				Columns += 4;
			}
			else
			{
				break;
			}
		}
		return Columns / 4;
	}

	Lang::FDstPrintOptions FEditableText::MakePrintOptions() const
	{
		Lang::FDstPrintOptions PrintOptions;
		// The file's own line ends, and its author's layout: what was broken over lines stays broken.
		PrintOptions.LineEnd = LineEnd;
		PrintOptions.KeepBrokenIn = &Source.GetText();
		return PrintOptions;
	}

	FString FEditableText::PrintStmt(const Lang::FDstStmt& Stmt, const int32 Level) const
	{
		return Lang::PrintDstStmt(Stmt, Level, File.Get(), MakePrintOptions());
	}

	FString FEditableText::PrintExpr(const Lang::FDstExpr& Expr, const int32 Level, const int32 Column) const
	{
		return Lang::PrintDstExpr(Expr, Level, FMath::Max(Column, 0), File.Get(), MakePrintOptions());
	}
}
