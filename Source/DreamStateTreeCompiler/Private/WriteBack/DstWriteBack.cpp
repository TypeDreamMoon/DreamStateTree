// Copyright DreamMoon. All Rights Reserved.
//
// Write-back: the entry points. The text is checked and compared with the model extracted from the asset; the
// differences are planned against the text (DstWriteBackState.cpp and friends), applied, and the new text must check to
// the asset's model again or nothing is written.

#include "DstWriteBack.h"

#include "StateTree.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "WriteBack/DstWriteBackInternal.h"

namespace UE::DreamStateTree::Compiler::WriteBackPrivate
{
	// ------------------------------------------------------------------------------------------
	// Indexes
	// ------------------------------------------------------------------------------------------

	void WbIndexStates(const TArray<Lang::FDstStmtPtr>& Members, const FString& ParentPath, TMap<FString, Lang::FDstStateStmt*>& OutMap)
	{
		for (const Lang::FDstStmtPtr& Member : Members)
		{
			Lang::FDstStateStmt* State = Member ? Member->As<Lang::FDstStateStmt>() : nullptr;
			if (!State)
			{
				continue;
			}
			const FString Path = ParentPath.IsEmpty() ? State->Name : ParentPath + TEXT("/") + State->Name;
			OutMap.Add(Path, State);
			WbIndexStates(State->Body, Path, OutMap);
		}
	}

	void WbIndexRaisedStates(const TArray<Lang::FDstStmtPtr>& Members, const FString& ParentPath, TMap<FString, const Lang::FDstStateStmt*>& OutMap)
	{
		for (const Lang::FDstStmtPtr& Member : Members)
		{
			const Lang::FDstStateStmt* State = Member ? Member->As<Lang::FDstStateStmt>() : nullptr;
			if (!State)
			{
				continue;
			}
			const FString Path = ParentPath.IsEmpty() ? State->Name : ParentPath + TEXT("/") + State->Name;
			OutMap.Add(Path, State);
			WbIndexRaisedStates(State->Body, Path, OutMap);
		}
	}

	void WbCollectTemplateNames(const Lang::FDstFile* SourceFile, TSet<FString>& OutNames)
	{
		if (!SourceFile || !SourceFile->Document)
		{
			return;
		}
		for (const Lang::FDstStmtPtr& Member : SourceFile->Document->Members)
		{
			if (const Lang::FDstTemplateStmt* Template = Member ? Member->As<Lang::FDstTemplateStmt>() : nullptr)
			{
				OutNames.Add(Template->Name);
			}
		}
	}

	/** The first line where two dumps differ, for a message. */
	FString WbFirstDumpDifference(const FString& Actual, const FString& Expected)
	{
		TArray<FString> ActualLines;
		TArray<FString> ExpectedLines;
		Actual.ParseIntoArrayLines(ActualLines, false);
		Expected.ParseIntoArrayLines(ExpectedLines, false);
		for (int32 Index = 0; Index < FMath::Max(ActualLines.Num(), ExpectedLines.Num()); ++Index)
		{
			const FString Left = ActualLines.IsValidIndex(Index) ? ActualLines[Index].TrimStartAndEnd() : FString(TEXT("<end>"));
			const FString Right = ExpectedLines.IsValidIndex(Index) ? ExpectedLines[Index].TrimStartAndEnd() : FString(TEXT("<end>"));
			if (!Left.Equals(Right, ESearchCase::CaseSensitive))
			{
				return FString::Printf(TEXT("the text gives `%s` where the asset has `%s`"), *Left, *Right);
			}
		}
		return FString(TEXT("no difference was found"));
	}

	// ------------------------------------------------------------------------------------------
	// The run
	// ------------------------------------------------------------------------------------------

	FWriteBack::FWriteBack(const UStateTree& InAsset, const FString& InFilePath, const FString& InBody, const FDstWriteBackOptions& InOptions,
		FDstWriteBackResult& InResult)
		: Asset(InAsset)
		, FilePath(InFilePath)
		, Body(InBody)
		, Options(InOptions)
		, Result(InResult)
	{
	}

	void FWriteBack::AddChange(const FString& Location, const FString& What, const bool bWritten, const bool bReprinted)
	{
		FDstWriteBackChange& Change = Result.Changes.AddDefaulted_GetRef();
		Change.Location = Location;
		Change.What = What;
		Change.bWritten = bWritten;
		Change.bReprinted = bReprinted;
	}

	void FWriteBack::Refuse(const TCHAR* Code, const Lang::FDstSpan& Span, const FString& Message, const FString& Hint, const bool bRefusal)
	{
		Result.bRefused |= bRefusal;
		Result.Diagnostics.Warning(Code, Span, Message, Hint);
	}

	void FWriteBack::AddEdit(TArray<FEdit>& Out, const int32 Offset, const int32 Length, const FString& NewText)
	{
		FEdit& Added = Out.AddDefaulted_GetRef();
		Added.Offset = Offset;
		Added.Length = Length;
		Added.Text = NewText;
		Added.Sequence = NextSequence++;
	}

	void FWriteBack::Commit(FPlan& Plan, TArray<FEdit>& Out)
	{
		for (const FEdit& Planned : Plan.Edits)
		{
			AddEdit(Out, Planned.Offset, Planned.Length, Planned.Text);
		}
		for (FDstWriteBackChange& Change : Plan.Changes)
		{
			Result.Changes.Add(MoveTemp(Change));
		}
		for (const FPendingRefusal& Pending : Plan.Refusals)
		{
			Refuse(Pending.Code, Pending.Span, Pending.Message, Pending.Hint, Pending.bRefusal);
		}
		Plan = FPlan();
	}

	bool FWriteBack::PrepareText()
	{
		FDstBuildOptions BuildOptions;
		BuildOptions.bGenerate = false;
		Built = BuildDstText(FilePath, Body, BuildOptions);

		const bool bReadable = !Built.Diagnostics.HasErrors() && Built.Lowered.IsSet() && Built.Unit.IsValid() && Built.Unit->Main.IsValid()
			&& Built.Unit->Main->Document.IsValid() && Built.Unit->Main->Document->DocumentKind == Lang::EDstDocumentKind::StateTree;
		if (!bReadable)
		{
			// The text's own errors, as they are: there is nothing to compare an asset with until they are fixed.
			const bool bHadErrors = Built.Diagnostics.HasErrors();
			Result.Diagnostics.Append(MoveTemp(Built.Diagnostics));
			if (!bHadErrors)
			{
				Result.Diagnostics.Error(TEXT("DST8105"), Lang::FDstSpan(), TEXT("The text is not a StateTree document, so there is no tree to compare the asset with."));
			}
			return false;
		}

		T = Built.Lowered->Tree;
		Places = Built.Lowered->Places;
		ValueContext.TagPrefixes = Built.Lowered->TagPrefixes;
		EnsureDstBindNames(T);

		Edit = MakeUnique<FEditableText>(FilePath, Body);
		if (!Edit->IsValid())
		{
			Result.Diagnostics.Error(TEXT("DST8105"), Lang::FDstSpan(), TEXT("The text could not be read back for editing."));
			return false;
		}

		WbCollectTemplateNames(Built.Unit->Main.Get(), TemplateNames);
		for (const TUniquePtr<Lang::FDstFile>& Library : Built.Unit->Libraries)
		{
			WbCollectTemplateNames(Library.Get(), TemplateNames);
		}
		for (const TPair<const Lang::FDstNode*, Lang::FDstExpansionOrigin>& Origin : Built.Unit->Main->Origins)
		{
			FExpansionSite& Site = Expansions.AddDefaulted_GetRef();
			Site.Span = Origin.Value.CallSpan;
			Site.TemplateName = Origin.Value.TemplateName;
		}
		WbIndexStates(Edit->GetDocument().Members, FString(), StateStmts);
		return true;
	}

	bool FWriteBack::PrepareAsset()
	{
		Lang::FDstDiagnosticSink ExtractSink(FilePath);
		const bool bExtracted = ExtractDst(Asset, A, ExtractSink, &Extracted);
		Result.Diagnostics.Append(MoveTemp(ExtractSink));
		if (!bExtracted)
		{
			return false;
		}
		// The asset may live elsewhere (a copy, a test's package); it was made from this text all the same.
		A.AssetPath = T.AssetPath;
		EnsureDstBindNames(A);
		return true;
	}

	void FWriteBack::PrepareRaise()
	{
		FDstRaiseOptions RaiseOptions;
		RaiseOptions.FilePath = FilePath;
		RaiseOptions.TagPrefixes = Built.Lowered->TagPrefixes;
		Lang::FDstDiagnosticSink RaiseSink(FilePath);
		Raised = RaiseDst(A, RaiseOptions, RaiseSink);
		bRaiseOk = Raised.IsValid() && Raised->Document.IsValid() && !RaiseSink.HasErrors();
		Result.Diagnostics.Append(MoveTemp(RaiseSink));
		if (Raised.IsValid() && Raised->Document.IsValid())
		{
			WbIndexRaisedStates(Raised->Document->Members, FString(), RaisedStates);
		}
	}

	FDstWriteBackResult RunWriteBack(const UStateTree& Asset, const FString& FilePath, const FString& Text, const FDstWriteBackOptions& Options)
	{
		FDstWriteBackResult Result;
		Result.Text = Text;
		Result.Diagnostics = Lang::FDstDiagnosticSink(FilePath);

		// The offsets of every span are into the text without its byte-order mark.
		const bool bBom = !Text.IsEmpty() && Text[0] == TEXT('\xFEFF');
		const FString Prefix = bBom ? FString(TEXT("\xFEFF")) : FString();
		const FString Body = bBom ? Text.RightChop(1) : Text;

		FWriteBack WriteBack(Asset, FilePath, Body, Options, Result);
		if (!WriteBack.PrepareText() || !WriteBack.PrepareAsset())
		{
			return Result;
		}
		Result.bCompared = true;

		const FString Before = Model::DumpDstModel(WriteBack.T);
		const FString Wanted = Model::DumpDstModel(WriteBack.A);
		if (Before.Equals(Wanted, ESearchCase::CaseSensitive))
		{
			Result.bFixedPoint = true;
			return Result;
		}

		WriteBack.AlignStates();
		WriteBack.PrepareRaise();
		WriteBack.Compare();

		const auto Revert = [&Result, &Text]()
		{
			Result.Text = Text;
			for (FDstWriteBackChange& Change : Result.Changes)
			{
				Change.bWritten = false;
			}
		};

		if (WriteBack.Edits.IsEmpty())
		{
			// Nothing to write: what differs was refused, or is something no edit was found for.
			if (!Result.bRefused)
			{
				Result.Diagnostics.Error(TEXT("DST8101"), Lang::FDstSpan(),
					FString::Printf(TEXT("The asset differs from the text, and no edit was found for it: %s."), *WbFirstDumpDifference(Before, Wanted)),
					TEXT("Rebuild the asset from the text to drop the difference, or make the change in the text."));
			}
			return Result;
		}

		bool bApplied = true;
		const FString NewBody = ApplyEdits(Body, WriteBack.Edits, bApplied);
		if (!bApplied)
		{
			Result.Diagnostics.Error(TEXT("DST8108"), Lang::FDstSpan(), TEXT("Two of the edits overlap, so nothing was written."),
				TEXT("This is a bug in write-back; the asset keeps its changes."));
			Revert();
			return Result;
		}

		// The new text must check to what the asset holds; anything left over was not written.
		FDstBuildOptions CheckOptions;
		CheckOptions.bGenerate = false;
		FDstFileResult Checked = BuildDstText(FilePath, NewBody, CheckOptions);
		if (Checked.Diagnostics.HasErrors() || !Checked.Lowered.IsSet())
		{
			const Lang::FDstDiagnostic* First = Checked.Diagnostics.FirstError();
			Result.Diagnostics.Error(TEXT("DST8101"), First ? First->Span : Lang::FDstSpan(),
				FString::Printf(TEXT("Writing the changes broke the text (%s); nothing was written."),
					First ? *Lang::FDstDiagnosticSink::ToWireString(*First) : TEXT("it no longer checks")),
				TEXT("The asset keeps its changes."));
			Revert();
			return Result;
		}

		Model::FDstTree Rechecked = Checked.Lowered->Tree;
		EnsureDstBindNames(Rechecked);
		const FString After = Model::DumpDstModel(Rechecked);
		if (After.Equals(Wanted, ESearchCase::CaseSensitive))
		{
			Result.bFixedPoint = true;
			Result.Text = Prefix + NewBody;
			return Result;
		}
		if (Result.bRefused)
		{
			// Something was refused, so the text cannot reach the asset; what could be written is written.
			Result.Text = Prefix + NewBody;
			return Result;
		}

		Result.Diagnostics.Error(TEXT("DST8101"), Lang::FDstSpan(),
			FString::Printf(TEXT("The text written for the asset's changes still differs from the asset: %s; nothing was written."), *WbFirstDumpDifference(After, Wanted)),
			TEXT("The asset keeps its changes."));
		Revert();
		return Result;
	}
}

namespace UE::DreamStateTree::Compiler
{
	FDstWriteBackResult WriteBackDstAsset(const UStateTree& Asset, const FString& FilePath, const FString& Text, const FDstWriteBackOptions& Options)
	{
		return WriteBackPrivate::RunWriteBack(Asset, FilePath, Text, Options);
	}

	bool GetDstGeneratedText(const UStateTree& Asset, FString& OutText, FString& OutSourcePath)
	{
		UPackage* Package = Asset.GetPackage();
		if (!Package)
		{
			return false;
		}
		FMetaData& MetaData = Package->GetMetaData();
		const FString* Text = MetaData.FindValue(&Asset, DstSourceMetadataKey);
		if (!Text)
		{
			return false;
		}
		OutText = *Text;
		const FString* Path = MetaData.FindValue(&Asset, DstSourcePathMetadataKey);
		OutSourcePath = Path ? *Path : FString();
		return true;
	}

	void SetDstGeneratedText(UStateTree& Asset, const FString& Text, const FString& SourcePath)
	{
		UPackage* Package = Asset.GetPackage();
		if (!Package)
		{
			return;
		}
		FMetaData& MetaData = Package->GetMetaData();
		if (Text.IsEmpty())
		{
			MetaData.RemoveValue(&Asset, DstSourceMetadataKey);
		}
		else
		{
			MetaData.SetValue(&Asset, DstSourceMetadataKey, *Text);
		}
		if (SourcePath.IsEmpty())
		{
			MetaData.RemoveValue(&Asset, DstSourcePathMetadataKey);
		}
		else
		{
			MetaData.SetValue(&Asset, DstSourcePathMetadataKey, *SourcePath);
		}
		Package->MarkPackageDirty();
	}
}
