// Copyright DreamMoon. All Rights Reserved.
//
// Corpus tests for the language front end. Every .dtr and .dth under Tests/Corpus is a test case:
//
//   Parse/    each file is parsed; `// EXPECT DSTnnnn (line,col)` comments list the diagnostics it must raise, exactly,
//             and a file without any must parse with no diagnostics at all (warnings included). A clean file is
//             compared with its `<file>.tree` golden (DumpDstTree).
//   Format/   each file must parse clean and is printed; the print is compared with `<file>.formatted`. A file whose name
//             starts with `canon_` is already in canonical form: it is its own golden, so the print must equal the input
//             and no .formatted file is needed.
//
// Every clean file in Parse/ and Format/ is also printed and checked: the print parses clean and has the same tree, printing
// it again changes nothing, no comment is lost, and the layout rules that do not depend on a golden hold (one final
// newline, no trailing blanks, CRLF output differs only in its line ends, reprinting with the author's layout keeps the tree).
//
// A golden that is missing or different is written next to the case as `<golden>.actual` and the test fails with that path
// in the message; review the .actual and rename it to promote it.
//
// Coverage checks that the Parse corpus raises every lexer and parser code at least once and has no golden without a case.

#include "DstLangTestSupport.h"

#include "HAL/FileManager.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstParser.h"
#include "Lang/DstPrinter.h"
#include "Lang/DstSource.h"
#include "Lang/DstToken.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamStateTree::Tests::CorpusTests
{
	using namespace UE::DreamStateTree::Lang;
	using namespace UE::DreamStateTree::Tests::LangSupport;

	namespace
	{
		/** `.dtr` or `.dth`, exactly (the Windows file search also matches longer extensions that start the same). */
		bool IsCaseFile(const FString& File)
		{
			const FString Extension = FPaths::GetExtension(File);
			return Extension.Equals(TEXT("dtr"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("dth"), ESearchCase::IgnoreCase);
		}

		/** Every case file under Corpus/SubDir, in a stable order, as full paths. */
		void CollectCaseFiles(const FString& SubDir, TArray<FString>& OutFullPaths)
		{
			const FString Dir = FPaths::Combine(GetCorpusDir(), SubDir);
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Dir, TEXT("*.*"), true, false);
			for (const FString& File : Found)
			{
				if (IsCaseFile(File))
				{
					FString Full = File;
					FPaths::NormalizeFilename(Full);
					OutFullPaths.Add(Full);
				}
			}
			OutFullPaths.Sort();
		}

		void FindCases(const FString& SubDir, TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
		{
			FString Dir = FPaths::Combine(GetCorpusDir(), SubDir);
			FPaths::NormalizeFilename(Dir);
			TArray<FString> Files;
			CollectCaseFiles(SubDir, Files);
			if (Files.IsEmpty())
			{
				// A missing corpus must fail loudly, not pass with nothing to run.
				OutBeautifiedNames.Add(TEXT("corpus_not_found"));
				OutTestCommands.Add(FString());
				return;
			}
			for (const FString& File : Files)
			{
				FString Relative = File.StartsWith(Dir) ? File.Mid(Dir.Len()) : FPaths::GetCleanFilename(File);
				Relative.RemoveFromStart(TEXT("/"));
				OutBeautifiedNames.Add(Relative);
				OutTestCommands.Add(File);
			}
		}

		/** One comment's text with runs of white space made single spaces, so a reprint that re-indents a block comment still matches. */
		FString NormalizeCommentText(const FString& Text)
		{
			TArray<FString> Words;
			Text.ParseIntoArrayWS(Words);
			return FString::Join(Words, TEXT(" "));
		}

		TArray<FString> CollectComments(const FString& Text)
		{
			TArray<FString> Comments;
			const FDstSourceText Source(TEXT("comments"), Text);
			FDstDiagnosticSink Sink;
			FDstLexOptions Options;
			Options.bEmitComments = true;
			for (const FDstToken& Token : LexDst(Source, Sink, Options))
			{
				if (Token.Is(EDstTokenKind::Comment))
				{
					Comments.Add(NormalizeCommentText(Token.Text));
				}
			}
			// FString's own ordering and equality ignore case; a comment that changed case is a changed comment.
			Comments.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
			return Comments;
		}

		/** Parses Printed (a print of File) and returns its tree dump; empty and an error when it does not parse clean. */
		bool ReparseClean(FAutomationTestBase& Test, const TCHAR* What, const FString& Path, const FString& Printed, TUniquePtr<FDstFile>& OutFile)
		{
			const FDstSourceText Source(Path, Printed);
			FDstDiagnosticSink Sink(Path);
			OutFile = ParseDst(Source, Sink);
			if (Sink.Num() > 0)
			{
				Test.AddError(FString::Printf(TEXT("The %s does not parse clean: %s"), What, *DescribeDiagnostics(Sink)));
				return false;
			}
			return true;
		}

		/**
		 * Prints the file and checks everything about the print that needs no golden. OutPrinted gets the canonical text.
		 * SourceText is the text the file was parsed from. bCanonical: the source is already in canonical form.
		 */
		void CheckPrintRoundTrip(FAutomationTestBase& Test, const FDstFile& File, const FString& SourceText, const bool bCanonical, FString& OutPrinted)
		{
			OutPrinted = PrintDst(File);
			const FString OriginalTree = DumpDstTree(File);

			// The print is a text of the same tree.
			TUniquePtr<FDstFile> Reparsed;
			if (!ReparseClean(Test, TEXT("printed text"), File.Path, OutPrinted, Reparsed))
			{
				return;
			}
			const FString TreeDifference = DescribeDifference(OriginalTree, DumpDstTree(*Reparsed));
			if (!TreeDifference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("Printing changed the tree (%s)."), *TreeDifference));
			}

			// Formatting is idempotent.
			const FString PrintedAgain = PrintDst(*Reparsed);
			const FString FormatDifference = DescribeDifference(OutPrinted, PrintedAgain);
			if (!FormatDifference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("Formatting is not idempotent (%s)."), *FormatDifference));
			}

			// Comments are never lost, added or changed.
			const TArray<FString> CommentsBefore = CollectComments(SourceText);
			const TArray<FString> CommentsAfter = CollectComments(OutPrinted);
			bool bSameComments = CommentsBefore.Num() == CommentsAfter.Num();
			for (int32 Index = 0; bSameComments && Index < CommentsBefore.Num(); ++Index)
			{
				bSameComments = CommentsBefore[Index].Equals(CommentsAfter[Index], ESearchCase::CaseSensitive);
			}
			if (!bSameComments)
			{
				FString Detail;
				const auto ContainsExactly = [](const TArray<FString>& Comments, const FString& Wanted)
				{
					return Comments.ContainsByPredicate([&Wanted](const FString& Comment) { return Comment.Equals(Wanted, ESearchCase::CaseSensitive); });
				};
				for (const FString& Comment : CommentsBefore)
				{
					if (!ContainsExactly(CommentsAfter, Comment))
					{
						Detail = FString::Printf(TEXT("lost `%s`"), *Comment);
						break;
					}
				}
				if (Detail.IsEmpty())
				{
					for (const FString& Comment : CommentsAfter)
					{
						if (!ContainsExactly(CommentsBefore, Comment))
						{
							Detail = FString::Printf(TEXT("added `%s`"), *Comment);
							break;
						}
					}
				}
				Test.AddError(FString::Printf(TEXT("Printing changed the comments (%d before, %d after; %s)."),
					CommentsBefore.Num(), CommentsAfter.Num(), Detail.IsEmpty() ? TEXT("a comment appears a different number of times") : *Detail));
			}

			// The layout rules that hold for any file.
			if (!OutPrinted.EndsWith(TEXT("\n")) || OutPrinted.EndsWith(TEXT("\n\n")))
			{
				Test.AddError(TEXT("The print must end with exactly one newline."));
			}
			if (OutPrinted.Contains(TEXT("\r")))
			{
				Test.AddError(TEXT("The default print must use \\n alone."));
			}
			TArray<FString> Lines;
			OutPrinted.ParseIntoArray(Lines, TEXT("\n"), false);
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				if (Lines[Index].EndsWith(TEXT(" ")) || Lines[Index].EndsWith(TEXT("\t")))
				{
					Test.AddError(FString::Printf(TEXT("Line %d of the print ends with white space: `%s`."), Index + 1, *Lines[Index]));
					break;
				}
			}

			// A different line end changes nothing else.
			{
				FDstPrintOptions Crlf;
				Crlf.LineEnd = TEXT("\r\n");
				const FString PrintedCrlf = PrintDst(File, Crlf);
				const FString Difference = DescribeDifference(OutPrinted, PrintedCrlf);
				if (!Difference.IsEmpty())
				{
					Test.AddError(FString::Printf(TEXT("A CRLF print differs from the LF print beyond its line ends (%s)."), *Difference));
				}
				if (PrintedCrlf.Replace(TEXT("\r\n"), TEXT("")).Contains(TEXT("\n")))
				{
					Test.AddError(TEXT("A CRLF print has a line that ends with a bare \\n."));
				}
			}

			// Reprinting with the author's layout keeps the tree (the write-back prints this way), and a file that is
			// already canonical comes back unchanged.
			{
				FDstPrintOptions Keep;
				Keep.KeepBrokenIn = &SourceText;
				const FString PrintedKeeping = PrintDst(File, Keep);
				TUniquePtr<FDstFile> KeptFile;
				if (ReparseClean(Test, TEXT("print that keeps the author's layout"), File.Path, PrintedKeeping, KeptFile))
				{
					const FString KeptDifference = DescribeDifference(OriginalTree, DumpDstTree(*KeptFile));
					if (!KeptDifference.IsEmpty())
					{
						Test.AddError(FString::Printf(TEXT("Printing with the author's layout changed the tree (%s)."), *KeptDifference));
					}
				}
				if (bCanonical)
				{
					const FString CanonicalDifference = DescribeDifference(SourceText, PrintedKeeping);
					if (!CanonicalDifference.IsEmpty())
					{
						Test.AddError(FString::Printf(TEXT("Printing a canonical file with its own layout changed it (%s)."), *CanonicalDifference));
					}
				}
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Parse
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDstLangCorpusParseTest, "DreamStateTree.Lang.Corpus.Parse",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstLangCorpusParseTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindCases(TEXT("Parse"), OutBeautifiedNames, OutTestCommands);
	}

	bool FDstLangCorpusParseTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No parse cases found under %s/Parse; is the DreamStateTree plugin's Tests/Corpus folder there?"), *GetCorpusDir()));
			return false;
		}

		FString Text;
		if (!LoadText(Path, Text))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}

		const FDstSourceText Source(Path, Text);
		FDstDiagnosticSink Sink(Path);
		const TUniquePtr<FDstFile> File = ParseDst(Source, Sink);
		if (!File)
		{
			AddError(TEXT("ParseDst returned null; it must always return a file."));
			return false;
		}

		const TArray<FExpectation> Expected = ReadExpectations(Text);
		CheckDiagnostics(*this, Path, Expected, Sink.GetDiagnostics());

		if (Expected.IsEmpty() && !Sink.HasErrors())
		{
			CheckGolden(*this, Path + TEXT(".tree"), DumpDstTree(*File));
			FString Printed;
			CheckPrintRoundTrip(*this, *File, Source.GetText(), /*bCanonical*/ false, Printed);
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Format
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDstLangCorpusFormatTest, "DreamStateTree.Lang.Corpus.Format",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstLangCorpusFormatTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindCases(TEXT("Format"), OutBeautifiedNames, OutTestCommands);
	}

	bool FDstLangCorpusFormatTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No format cases found under %s/Format; is the DreamStateTree plugin's Tests/Corpus folder there?"), *GetCorpusDir()));
			return false;
		}

		FString Text;
		if (!LoadText(Path, Text))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}

		const FDstSourceText Source(Path, Text);
		FDstDiagnosticSink Sink(Path);
		const TUniquePtr<FDstFile> File = ParseDst(Source, Sink);
		if (!File || Sink.Num() > 0)
		{
			AddError(FString::Printf(TEXT("A format case must parse clean: %s"), *DescribeDiagnostics(Sink)));
			return false;
		}

		const bool bCanonical = FPaths::GetCleanFilename(Path).StartsWith(TEXT("canon_"));
		FString Printed;
		CheckPrintRoundTrip(*this, *File, Source.GetText(), bCanonical, Printed);

		// A canonical file is its own golden unless one was written for it.
		const FString CanonicalInput = NormalizeNewlines(Source.GetText());
		CheckGolden(*this, Path + TEXT(".formatted"), Printed, bCanonical ? &CanonicalInput : nullptr);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Coverage
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangCorpusCoverageTest, "DreamStateTree.Lang.Corpus.Coverage",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangCorpusCoverageTest::RunTest(const FString& Parameters)
	{
		TArray<FString> ParseFiles;
		CollectCaseFiles(TEXT("Parse"), ParseFiles);
		TArray<FString> FormatFiles;
		CollectCaseFiles(TEXT("Format"), FormatFiles);
		if (ParseFiles.IsEmpty() || FormatFiles.IsEmpty())
		{
			AddError(FString::Printf(TEXT("The corpus under %s is missing or empty (%d parse files, %d format files)."),
				*GetCorpusDir(), ParseFiles.Num(), FormatFiles.Num()));
			return false;
		}

		// Every code the lexer and the parser can raise has a case. Codes of the later stages are tested elsewhere.
		TSet<FString> Raised;
		for (const FString& File : ParseFiles)
		{
			FString Text;
			if (LoadText(File, Text))
			{
				for (const FExpectation& Expectation : ReadExpectations(Text))
				{
					Raised.Add(Expectation.Code);
				}
			}
		}
		TArray<FString> Missing;
		for (int32 Number = 1001; Number <= 1007; ++Number)
		{
			Missing.Add(FString::Printf(TEXT("DST%d"), Number));
		}
		for (int32 Number = 2001; Number <= 2024; ++Number)
		{
			Missing.Add(FString::Printf(TEXT("DST%d"), Number));
		}
		Missing.Add(TEXT("DST3106"));
		Missing.RemoveAll([&Raised](const FString& Code) { return Raised.Contains(Code); });
		if (!Missing.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No Parse case expects %s."), *FString::Join(Missing, TEXT(", "))));
		}

		// A golden whose case is gone would never be looked at again.
		for (const TCHAR* Directory : { TEXT("Parse"), TEXT("Format") })
		{
			TArray<FString> Goldens;
			IFileManager::Get().FindFilesRecursive(Goldens, *FPaths::Combine(GetCorpusDir(), Directory), TEXT("*.*"), true, false);
			for (const FString& Golden : Goldens)
			{
				const FString Extension = FPaths::GetExtension(Golden);
				const bool bTree = Extension == TEXT("tree");
				const bool bFormatted = Extension == TEXT("formatted");
				if (!bTree && !bFormatted)
				{
					continue;
				}
				FString CasePath = Golden.LeftChop(Extension.Len() + 1);
				if (!IFileManager::Get().FileExists(*CasePath))
				{
					AddError(FString::Printf(TEXT("The golden %s has no case file %s."), *Golden, *CasePath));
				}
			}
		}

		// Names are what the test list shows: two cases of one name would hide each other.
		TSet<FString> Names;
		for (const FString& File : ParseFiles)
		{
			const FString Name = FPaths::GetCleanFilename(File);
			if (Names.Contains(Name))
			{
				AddError(FString::Printf(TEXT("Two Parse cases are called %s."), *Name));
			}
			Names.Add(Name);
		}
		return true;
	}
}
