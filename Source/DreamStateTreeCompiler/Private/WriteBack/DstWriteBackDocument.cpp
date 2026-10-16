// Copyright DreamMoon. All Rights Reserved.
//
// The document: the schema in the header, the sections (Settings, Parameters, Globals, Colors) and the top-level states.
// A section is edited entry by entry where only values changed (an entry that did not change keeps its text, comments
// and alignment); otherwise it is printed again, added, or removed. Tags is not part of the model and is never touched.

#include "WriteBack/DstWriteBackInternal.h"

namespace UE::DreamStateTree::Compiler::WriteBackPrivate
{
	// ------------------------------------------------------------------------------------------
	// Lines in and out
	// ------------------------------------------------------------------------------------------

	void FWriteBack::AddRemoveStatement(TArray<FEdit>& Out, const Lang::FDstNode& Node)
	{
		const FString& Source = Edit->GetText();
		int32 Start = Node.Span.Offset;
		int32 End = Node.Span.End();
		if (const Lang::FDstTrivia* Trivia = Edit->GetFile().FindTrivia(&Node))
		{
			// Its own comments go with it.
			if (!Trivia->Leading.IsEmpty())
			{
				Start = FMath::Min(Start, Trivia->Leading[0].Span.Offset);
			}
			if (Trivia->Trailing.IsSet())
			{
				End = FMath::Max(End, Trivia->Trailing->Span.End());
			}
		}

		// Whole lines when the statement has them to itself.
		const int32 LineStart = Edit->GetLineStart(Start);
		if (Source.Mid(LineStart, Start - LineStart).TrimStartAndEnd().IsEmpty())
		{
			int32 After = End;
			while (After < Source.Len() && (Source[After] == TEXT(' ') || Source[After] == TEXT('\t')))
			{
				++After;
			}
			if (After >= Source.Len() || Source[After] == TEXT('\r') || Source[After] == TEXT('\n'))
			{
				if (After < Source.Len())
				{
					After += (Source[After] == TEXT('\r') && After + 1 < Source.Len() && Source[After + 1] == TEXT('\n')) ? 2 : 1;
				}
				Start = LineStart;
				End = After;
			}
		}
		AddEdit(Out, Start, End - Start, FString());
	}

	void FWriteBack::AddInsertLines(TArray<FEdit>& Out, const int32 AtOffset, const FString& Lines)
	{
		AddEdit(Out, AtOffset, 0, Lines);
	}

	/** The name an entry of a section goes by when its comments are carried over to the entry that replaces it. */
	FString WbEntryKey(const Lang::FDstStmt& Entry)
	{
		if (const Lang::FDstAssignStmt* Assign = Entry.As<Lang::FDstAssignStmt>())
		{
			return TEXT("A:") + Assign->Name;
		}
		if (const Lang::FDstDeclStmt* Decl = Entry.As<Lang::FDstDeclStmt>())
		{
			return TEXT("D:") + Decl->Name;
		}
		if (const Lang::FDstExprStmt* Expression = Entry.As<Lang::FDstExprStmt>())
		{
			return TEXT("E:") + (Expression->Expr ? Lang::PrintDstExpr(*Expression->Expr) : FString());
		}
		return FString();
	}

	FString WbEntryName(const Lang::FDstStmt& Entry)
	{
		if (const Lang::FDstAssignStmt* Assign = Entry.As<Lang::FDstAssignStmt>())
		{
			return Assign->Name;
		}
		if (const Lang::FDstDeclStmt* Decl = Entry.As<Lang::FDstDeclStmt>())
		{
			return Decl->Name;
		}
		return FString();
	}

	const Lang::FDstExpr* WbEntryValue(const Lang::FDstStmt& Entry)
	{
		if (const Lang::FDstAssignStmt* Assign = Entry.As<Lang::FDstAssignStmt>())
		{
			return Assign->Value.Get();
		}
		if (const Lang::FDstDeclStmt* Decl = Entry.As<Lang::FDstDeclStmt>())
		{
			return Decl->Value.Get();
		}
		return nullptr;
	}

	/** The entry as it prints without its value: name, type and settings. */
	FString WbEntryWithoutValue(const Lang::FDstStmt& Entry)
	{
		Lang::FDstStmtPtr Copy = Lang::CloneStmt(Entry);
		if (Lang::FDstAssignStmt* Assign = Copy->As<Lang::FDstAssignStmt>())
		{
			Assign->Value.Reset();
		}
		else if (Lang::FDstDeclStmt* Decl = Copy->As<Lang::FDstDeclStmt>())
		{
			Decl->Value.Reset();
		}
		return Lang::PrintDstStmt(*Copy, 0);
	}

	// ------------------------------------------------------------------------------------------
	// Sections
	// ------------------------------------------------------------------------------------------

	Lang::FDstSectionStmt* FWriteBack::FindTextSection(const TCHAR* Name) const
	{
		if (!Edit.IsValid())
		{
			return nullptr;
		}
		for (const Lang::FDstStmtPtr& Member : Edit->GetDocument().Members)
		{
			Lang::FDstSectionStmt* Section = Member ? Member->As<Lang::FDstSectionStmt>() : nullptr;
			if (Section && Section->Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return Section;
			}
		}
		return nullptr;
	}

	const Lang::FDstSectionStmt* FWriteBack::FindRaisedSection(const TCHAR* Name) const
	{
		if (!Raised.IsValid() || !Raised->Document.IsValid())
		{
			return nullptr;
		}
		for (const Lang::FDstStmtPtr& Member : Raised->Document->Members)
		{
			const Lang::FDstSectionStmt* Section = Member ? Member->As<Lang::FDstSectionStmt>() : nullptr;
			if (Section && Section->Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return Section;
			}
		}
		return nullptr;
	}

	bool FWriteBack::TryPlanSectionValues(const Lang::FDstSectionStmt& TextSection, const Lang::FDstSectionStmt& NewSection,
		const TSet<FString>& ChangedNames, TArray<FEdit>& Out, TArray<FString>& OutChanged)
	{
		if (TextSection.Entries.Num() != NewSection.Entries.Num())
		{
			return false;
		}

		TArray<FEdit> Replaced;
		for (int32 Index = 0; Index < TextSection.Entries.Num(); ++Index)
		{
			const Lang::FDstStmt* Old = TextSection.Entries[Index].Get();
			const Lang::FDstStmt* New = NewSection.Entries[Index].Get();
			if (!Old || !New || Old->Kind != New->Kind)
			{
				return false;
			}
			const FString Name = WbEntryName(*Old);
			if (Name.IsEmpty() || !Name.Equals(WbEntryName(*New), ESearchCase::CaseSensitive))
			{
				return false;
			}
			if (!ChangedNames.Contains(Name))
			{
				continue;
			}

			// Only the value may differ, and both sides have one.
			const Lang::FDstExpr* OldValue = WbEntryValue(*Old);
			const Lang::FDstExpr* NewValue = WbEntryValue(*New);
			if (!OldValue || !NewValue || !WbEntryWithoutValue(*Old).Equals(WbEntryWithoutValue(*New), ESearchCase::CaseSensitive))
			{
				return false;
			}
			FEdit Replacement;
			Replacement.Offset = OldValue->Span.Offset;
			Replacement.Length = OldValue->Span.Length;
			Replacement.Text = Edit->PrintExpr(*NewValue, Edit->GetIndentLevel(OldValue->Span.Offset), OldValue->Span.Column - 1);
			Replaced.Add(MoveTemp(Replacement));
			OutChanged.Add(Name);
		}

		for (FEdit& Replacement : Replaced)
		{
			AddEdit(Out, Replacement.Offset, Replacement.Length, Replacement.Text);
		}
		return true;
	}

	void FWriteBack::PlanSection(const TCHAR* SectionName, const TSet<FString>& ChangedNames)
	{
		const FString Location = SectionName;
		Lang::FDstSectionStmt* TextSection = FindTextSection(SectionName);
		const Lang::FDstSectionStmt* NewSection = bRaiseOk ? FindRaisedSection(SectionName) : nullptr;
		const Lang::FDstSpan HeaderSpan = Places.Find(FString());

		if (!bRaiseOk || (!TextSection && !NewSection))
		{
			AddChange(Location, TEXT("the entries"), false, true);
			Refuse(TEXT("DST8105"), TextSection ? TextSection->Span : HeaderSpan,
				FString::Printf(TEXT("%s changed in the editor, but it could not be written from the asset."), *Location));
			return;
		}

		// Values changed where they stand.
		if (TextSection && NewSection)
		{
			TArray<FEdit> Replaced;
			TArray<FString> Changed;
			if (TryPlanSectionValues(*TextSection, *NewSection, ChangedNames, Replaced, Changed))
			{
				Edits.Append(MoveTemp(Replaced));
				AddChange(Location, FString::Join(Changed, TEXT(", ")), true, false);
				return;
			}
		}

		const TCHAR* What = !TextSection ? TEXT("added") : (!NewSection ? TEXT("removed") : TEXT("the entries"));
		if (!Options.bStructure)
		{
			AddChange(Location, What, false, true);
			Refuse(TEXT("DST8104"), TextSection ? TextSection->Span : HeaderSpan,
				FString::Printf(TEXT("%s changed in the editor (%s); structural changes are not written back."), *Location, What),
				TEXT("Make the change in the text, or turn on writing structural changes back."));
			return;
		}

		Lang::FDstFile& File = Edit->GetFile();
		if (TextSection && !NewSection)
		{
			AddRemoveStatement(Edits, *TextSection);
			AddChange(Location, What, true, true);
			return;
		}

		Lang::FDstStmtPtr Fresh = Lang::CloneStmt(*NewSection);
		if (TextSection)
		{
			// Comments follow their entries by name; the ones after the last entry stay inside.
			if (const Lang::FDstTrivia* Found = File.FindTrivia(TextSection))
			{
				Lang::FDstTrivia Inner;
				Inner.Inner = Found->Inner;
				File.Trivia.FindOrAdd(Fresh.Get()) = MoveTemp(Inner);
			}
			Lang::FDstSectionStmt* FreshSection = Fresh->As<Lang::FDstSectionStmt>();
			TSet<const Lang::FDstStmt*> Taken;
			for (const Lang::FDstStmtPtr& NewEntry : FreshSection->Entries)
			{
				if (!NewEntry)
				{
					continue;
				}
				const FString Key = WbEntryKey(*NewEntry);
				for (const Lang::FDstStmtPtr& OldEntry : TextSection->Entries)
				{
					if (OldEntry && !Taken.Contains(OldEntry.Get()) && !Key.IsEmpty() && WbEntryKey(*OldEntry).Equals(Key, ESearchCase::CaseSensitive))
					{
						CopyTrivia(File, OldEntry.Get(), NewEntry.Get());
						Taken.Add(OldEntry.Get());
						break;
					}
				}
			}
			const FString Printed = Edit->PrintStmt(*Fresh, Edit->GetIndentLevel(TextSection->Span.Offset));
			AddReplace(Edits, TextSection->Span, Printed);
			Edit->Bury(MoveTemp(Fresh));
			AddChange(Location, What, true, true);
			return;
		}

		// A section the text does not have yet: after the last one, or first in the document.
		const FString Indent = FString::ChrN(4, TEXT(' '));
		const FString Printed = Indent + Edit->PrintStmt(*Fresh, 1) + Edit->GetLineEnd();
		const FString& Source = Edit->GetText();
		const Lang::FDstSectionStmt* LastSection = nullptr;
		const Lang::FDstStmt* FirstMember = nullptr;
		for (const Lang::FDstStmtPtr& Member : Edit->GetDocument().Members)
		{
			if (!Member)
			{
				continue;
			}
			FirstMember = FirstMember ? FirstMember : Member.Get();
			if (const Lang::FDstSectionStmt* Existing = Member->As<Lang::FDstSectionStmt>())
			{
				LastSection = Existing;
			}
		}
		if (LastSection)
		{
			int32 After = Edit->GetLineEndOffset(LastSection->Span.End());
			if (After < Source.Len())
			{
				After += (Source[After] == TEXT('\r') && After + 1 < Source.Len() && Source[After + 1] == TEXT('\n')) ? 2 : 1;
			}
			AddInsertLines(Edits, After, Printed);
		}
		else if (FirstMember)
		{
			int32 First = FirstMember->Span.Offset;
			if (const Lang::FDstTrivia* Found = File.FindTrivia(FirstMember); Found && !Found->Leading.IsEmpty())
			{
				First = FMath::Min(First, Found->Leading[0].Span.Offset);
			}
			AddInsertLines(Edits, Edit->GetLineStart(First), Printed + Edit->GetLineEnd());
		}
		else
		{
			AddInsertLines(Edits, Edit->GetLineStart(Edit->GetDocument().CloseSpan.Offset), Printed);
		}
		Edit->Bury(MoveTemp(Fresh));
		AddChange(Location, What, true, true);
	}

	// ------------------------------------------------------------------------------------------
	// The comparison
	// ------------------------------------------------------------------------------------------

	void FWriteBack::CompareHeader()
	{
		const auto DumpHeader = [](const Model::FDstTree& Tree)
		{
			Model::FDstTree Header;
			Header.SchemaClass = Tree.SchemaClass;
			Header.SchemaProperties = Tree.SchemaProperties;
			return Model::DumpDstModel(Header);
		};
		if (DumpHeader(T).Equals(DumpHeader(A), ESearchCase::CaseSensitive))
		{
			return;
		}

		const Lang::FDstArgument* TextArg = Edit->GetDocument().FindHeaderArg(TEXT("Schema"));
		const Lang::FDstArgument* NewArg = (bRaiseOk && Raised->Document.IsValid()) ? Raised->Document->FindHeaderArg(TEXT("Schema")) : nullptr;
		if (!TextArg || !TextArg->Value || !NewArg || !NewArg->Value)
		{
			AddChange(TEXT("Header"), TEXT("the schema"), false, false);
			Refuse(TEXT("DST8105"), Places.Find(FString()), TEXT("The schema changed in the editor, but the header could not be written from the asset."));
			return;
		}
		AddReplace(Edits, TextArg->Value->Span,
			Edit->PrintExpr(*NewArg->Value, Edit->GetIndentLevel(TextArg->Value->Span.Offset), TextArg->Value->Span.Column - 1));
		AddChange(TEXT("Header"), TEXT("the schema"), true, false);
	}

	void FWriteBack::CompareSettings()
	{
		const auto DumpSettings = [](const Model::FDstTree& Tree)
		{
			Model::FDstTree Piece;
			Piece.Settings = Tree.Settings;
			return Model::DumpDstModel(Piece);
		};
		if (DumpSettings(T).Equals(DumpSettings(A), ESearchCase::CaseSensitive))
		{
			return;
		}

		TSet<FString> Changed;
		for (const TPair<FString, FString>& Pair : A.Settings)
		{
			const FString* Old = T.Settings.Find(Pair.Key);
			if (!Old || !Old->Equals(Pair.Value, ESearchCase::CaseSensitive))
			{
				Changed.Add(Pair.Key);
			}
		}
		for (const TPair<FString, FString>& Pair : T.Settings)
		{
			if (!A.Settings.Contains(Pair.Key))
			{
				Changed.Add(Pair.Key);
			}
		}
		PlanSection(TEXT("Settings"), Changed);
	}

	void FWriteBack::CompareParameters()
	{
		const auto DumpAll = [](const Model::FDstTree& Tree)
		{
			Model::FDstTree Piece;
			Piece.Parameters = Tree.Parameters;
			return Model::DumpDstModel(Piece);
		};
		if (DumpAll(T).Equals(DumpAll(A), ESearchCase::CaseSensitive))
		{
			return;
		}

		// The same parameters in the same order, some of them with other values: a value each can be replaced.
		TSet<FString> Changed;
		bool bSameNames = T.Parameters.Num() == A.Parameters.Num();
		for (int32 Index = 0; bSameNames && Index < T.Parameters.Num(); ++Index)
		{
			bSameNames = T.Parameters[Index].Name.Equals(A.Parameters[Index].Name, ESearchCase::CaseSensitive);
		}
		for (int32 Index = 0; bSameNames && Index < T.Parameters.Num(); ++Index)
		{
			Model::FDstTree TextPiece;
			TextPiece.Parameters.Add(T.Parameters[Index]);
			Model::FDstTree AssetPiece;
			AssetPiece.Parameters.Add(A.Parameters[Index]);
			if (!Model::DumpDstModel(TextPiece).Equals(Model::DumpDstModel(AssetPiece), ESearchCase::CaseSensitive))
			{
				Changed.Add(T.Parameters[Index].Name);
			}
		}
		PlanSection(TEXT("Parameters"), Changed);
	}

	void FWriteBack::CompareColors()
	{
		const auto DumpAll = [](const Model::FDstTree& Tree)
		{
			Model::FDstTree Piece;
			Piece.Colors = Tree.Colors;
			return Model::DumpDstModel(Piece);
		};
		if (DumpAll(T).Equals(DumpAll(A), ESearchCase::CaseSensitive))
		{
			return;
		}

		TSet<FString> Changed;
		bool bSameNames = T.Colors.Num() == A.Colors.Num();
		for (int32 Index = 0; bSameNames && Index < T.Colors.Num(); ++Index)
		{
			bSameNames = T.Colors[Index].Name.Equals(A.Colors[Index].Name, ESearchCase::CaseSensitive);
		}
		for (int32 Index = 0; bSameNames && Index < T.Colors.Num(); ++Index)
		{
			if (!T.Colors[Index].ColorText.Equals(A.Colors[Index].ColorText, ESearchCase::CaseSensitive))
			{
				Changed.Add(T.Colors[Index].Name);
			}
		}
		PlanSection(TEXT("Colors"), Changed);
	}

	void FWriteBack::CompareGlobals()
	{
		const auto DumpAll = [](const TArray<Model::FDstNode>& Nodes)
		{
			Model::FDstTree Piece;
			Piece.Globals = Nodes;
			return Model::DumpDstModel(Piece);
		};
		if (DumpAll(T.Globals).Equals(DumpAll(A.Globals), ESearchCase::CaseSensitive))
		{
			return;
		}

		// The same nodes with other arguments: each is written where it stands.
		if (FindTextSection(TEXT("Globals")) && T.Globals.Num() == A.Globals.Num())
		{
			TArray<Model::FDstNode> TextShape = T.Globals;
			TArray<Model::FDstNode> AssetShape = A.Globals;
			for (Model::FDstNode& Node : TextShape)
			{
				Node.Args.Reset();
			}
			for (Model::FDstNode& Node : AssetShape)
			{
				Node.Args.Reset();
			}
			if (DumpAll(TextShape).Equals(DumpAll(AssetShape), ESearchCase::CaseSensitive))
			{
				FPlan Plan;
				for (int32 Index = 0; Index < T.Globals.Num() && !Plan.bNeedsReprint; ++Index)
				{
					PlanNode(T.Globals[Index], A.Globals[Index], TEXT("Globals: ") + T.Globals[Index].BindName, FindRaisedGlobalExpr(Index), Plan);
				}
				if (!Plan.bNeedsReprint)
				{
					Commit(Plan, Edits);
					return;
				}
			}
		}

		TSet<FString> Changed;
		if (T.Globals.Num() == A.Globals.Num())
		{
			for (int32 Index = 0; Index < T.Globals.Num(); ++Index)
			{
				if (!DumpNodeOf(T.Globals[Index]).Equals(DumpNodeOf(A.Globals[Index]), ESearchCase::CaseSensitive))
				{
					Changed.Add(T.Globals[Index].BindName);
				}
			}
		}
		PlanSection(TEXT("Globals"), Changed);
	}

	void FWriteBack::CompareStates()
	{
		Lang::FDstDocument& Document = Edit->GetDocument();

		// The states both sides have, in the asset's order; the asset's others are new.
		TArray<const Model::FDstState*> Added;
		int32 LastIndex = INDEX_NONE;
		bool bReordered = false;
		for (const Model::FDstState& AState : A.States)
		{
			const FString* TPath = AToT.Find(AState.Path);
			const Model::FDstState* TState = TPath ? T.FindState(*TPath) : nullptr;
			if (!TState)
			{
				Added.Add(&AState);
				continue;
			}
			int32 TIndex = INDEX_NONE;
			for (int32 Index = 0; Index < T.States.Num(); ++Index)
			{
				if (&T.States[Index] == TState)
				{
					TIndex = Index;
				}
			}
			bReordered |= TIndex != INDEX_NONE && TIndex < LastIndex;
			LastIndex = FMath::Max(LastIndex, TIndex);
			PlanState(AState, *TState, Edits);
		}

		// The text's others are gone from the asset. A subtree copied from a library goes with the link that brought it.
		for (const Model::FDstState& TState : T.States)
		{
			if (TToA.Contains(TState.Path))
			{
				continue;
			}
			Lang::FDstStateStmt* Stmt = FindStateStmt(TState.Path);
			if (!Stmt)
			{
				continue;
			}
			const FString Location = TEXT("State ") + TState.Path;
			if (!Options.bStructure)
			{
				AddChange(Location, TEXT("removed"), false, true);
				Refuse(TEXT("DST8104"), Stmt->Span, FString::Printf(TEXT("%s was removed in the editor; structural changes are not written back."), *Location),
					TEXT("Make the change in the text, or turn on writing structural changes back."));
				continue;
			}
			AddRemoveStatement(Edits, *Stmt);
			AddChange(Location, TEXT("removed"), true, true);
		}

		for (const Model::FDstState* AState : Added)
		{
			const FString Location = TEXT("State ") + AState->Path;
			if (!Options.bStructure)
			{
				AddChange(Location, TEXT("added"), false, true);
				Refuse(TEXT("DST8104"), Document.CloseSpan, FString::Printf(TEXT("%s was added in the editor; structural changes are not written back."), *Location),
					TEXT("Make the change in the text, or turn on writing structural changes back."));
				continue;
			}
			const Lang::FDstStateStmt* Card = bRaiseOk ? FindRaisedState(AState->Path) : nullptr;
			if (!Card)
			{
				AddChange(Location, TEXT("added"), false, true);
				Refuse(TEXT("DST8105"), Document.CloseSpan, FString::Printf(TEXT("%s was added in the editor, but it could not be written from the asset."), *Location));
				continue;
			}
			// A blank line, then the card, just above the document's closing brace.
			Lang::FDstStmtPtr Fresh = Lang::CloneStmt(*Card);
			const FString Lines = Edit->GetLineEnd() + FString::ChrN(4, TEXT(' ')) + Edit->PrintStmt(*Fresh, 1) + Edit->GetLineEnd();
			AddInsertLines(Edits, Edit->GetLineStart(Document.CloseSpan.Offset), Lines);
			Edit->Bury(MoveTemp(Fresh));
			AddChange(Location, TEXT("added"), true, true);
		}

		if (bReordered)
		{
			AddChange(TEXT("States"), TEXT("the order of the top-level states"), false, false);
			Refuse(TEXT("DST8109"), Document.OpenSpan, TEXT("The top-level states are in another order in the asset; the text keeps its order."),
				TEXT("The first state is the root: reorder them in the text."));
		}
	}

	void FWriteBack::Compare()
	{
		CompareHeader();
		CompareSettings();
		CompareParameters();
		CompareGlobals();
		CompareColors();
		CompareStates();
	}
}
