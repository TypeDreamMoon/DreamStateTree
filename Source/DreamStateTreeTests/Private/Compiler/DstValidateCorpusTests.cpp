// Copyright DreamMoon. All Rights Reserved.
//
// Corpus tests for the semantic stage's checks. Every .dtr and .dth under Tests/Corpus/Validate is a case (the files in
// the `inc` folders are libraries the cases include, not cases):
//
//   - the text goes through the driver without generating anything (BuildDstText, bGenerate off): parse, includes,
//     templates, lowering;
//   - `// EXPECT DSTnnnn (line,col)` comments list the diagnostics it must raise, exactly, warnings included; a file
//     without any must come out with no diagnostics at all. A diagnostic about an #included library matches on its
//     code alone, as in the language corpus.
//
// Coverage checks that every DST4xxx, DST5xxx and DST6xxx code the semantic stage can raise has a case. The list below
// is the stage's own: a code it raises that is not in the list is found by reading the source when the source is on
// disk, so a new code cannot slip in without a case.

#include "Compiler/DstCompilerTestSupport.h"

#include "Interfaces/IPluginManager.h"

namespace UE::DreamStateTree::Tests::ValidateCorpus
{
	namespace
	{
		/** Cases are .dtr and .dth files, except in `inc` folders. */
		const TArray<FString>& GetSkippedFolders()
		{
			static const TArray<FString> Folders = { TEXT("inc") };
			return Folders;
		}

		/**
		 * Every code the semantic stage (and the driver, for the cycle of links) raises in the DST4xxx to DST6xxx range.
		 * DST4012, DST5006 and DST6011 are not raised by anything; they are left out on purpose.
		 */
		const TArray<FString>& GetRaisableCodes()
		{
			static TArray<FString> Codes;
			if (Codes.IsEmpty())
			{
				for (int32 Number = 4001; Number <= 4022; ++Number)
				{
					if (Number != 4012)
					{
						Codes.Add(FString::Printf(TEXT("DST%d"), Number));
					}
				}
				for (int32 Number = 5001; Number <= 5018; ++Number)
				{
					if (Number != 5006)
					{
						Codes.Add(FString::Printf(TEXT("DST%d"), Number));
					}
				}
				for (int32 Number = 6001; Number <= 6018; ++Number)
				{
					if (Number != 6011)
					{
						Codes.Add(FString::Printf(TEXT("DST%d"), Number));
					}
				}
			}
			return Codes;
		}

		/** The codes written as `TEXT("DST4xxx")`-style literals in a source file's text, DST4xxx to DST6xxx only. */
		void CollectCodesInSource(const FString& Source, TSet<FString>& OutCodes)
		{
			const FString Marker(TEXT("\"DST"));
			int32 From = 0;
			while (true)
			{
				const int32 At = Source.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
				if (At == INDEX_NONE)
				{
					break;
				}
				From = At + Marker.Len();
				if (From + 5 > Source.Len() || Source[From + 4] != TEXT('"'))
				{
					continue;
				}
				const FString Digits = Source.Mid(From, 4);
				if (!Digits.IsNumeric() || Digits.Len() != 4)
				{
					continue;
				}
				const int32 Number = FCString::Atoi(*Digits);
				if (Number >= 4000 && Number < 7000)
				{
					OutCodes.Add(FString::Printf(TEXT("DST%d"), Number));
				}
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Cases
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDstValidateCorpusTest, "DreamStateTree.Compiler.Validate.Corpus",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstValidateCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		CompilerSupport::ListCases(CompilerSupport::GetCorpusSubDir(TEXT("Validate")), /*bTrees*/ true, /*bLibraries*/ true, GetSkippedFolders(),
			OutBeautifiedNames, OutTestCommands);
	}

	bool FDstValidateCorpusTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No validate cases found under %s/Validate; is the DreamStateTree plugin's Tests/Corpus folder there?"),
				*LangSupport::GetCorpusDir()));
			return false;
		}

		FString Text;
		if (!LangSupport::LoadText(Path, Text))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}

		// Texts name the fixture tags (`on DreamStateTreeTest.Event.Hit`); they have to exist before the text is read.
		CompilerSupport::EnsureTestTags();

		const FString Root = FPaths::GetPath(Path);
		const Compiler::FDstFileResult Result = Compiler::BuildDstText(Path, Text, CompilerSupport::MakeBuildOptions(Root, /*bGenerate*/ false));

		const TArray<LangSupport::FExpectation> Expected = LangSupport::ReadExpectations(Text);
		LangSupport::CheckDiagnostics(*this, Result.FilePath, Expected, Result.Diagnostics.GetDiagnostics());

		// Checking must not touch an asset, and a case that expects an error must not get as far as generating.
		TestFalse(TEXT("A validate case does not generate"), Result.Generated.IsSet());
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Coverage
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstValidateCorpusCoverageTest, "DreamStateTree.Compiler.Validate.Coverage",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstValidateCorpusCoverageTest::RunTest(const FString& Parameters)
	{
		TArray<FString> Files;
		CompilerSupport::CollectCaseFiles(CompilerSupport::GetCorpusSubDir(TEXT("Validate")), true, true, GetSkippedFolders(), Files);
		if (Files.IsEmpty())
		{
			AddError(FString::Printf(TEXT("The validate corpus under %s is missing or empty."), *LangSupport::GetCorpusDir()));
			return false;
		}

		TSet<FString> Expected;
		TSet<FString> Names;
		for (const FString& File : Files)
		{
			FString Text;
			if (LangSupport::LoadText(File, Text))
			{
				for (const LangSupport::FExpectation& Expectation : LangSupport::ReadExpectations(Text))
				{
					Expected.Add(Expectation.Code);
				}
			}

			const FString Name = FPaths::GetCleanFilename(File);
			if (Names.Contains(Name))
			{
				AddError(FString::Printf(TEXT("Two validate cases are called %s."), *Name));
			}
			Names.Add(Name);
		}

		TArray<FString> Missing;
		for (const FString& Code : GetRaisableCodes())
		{
			if (!Expected.Contains(Code))
			{
				Missing.Add(Code);
			}
		}
		if (!Missing.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No validate case expects %s."), *FString::Join(Missing, TEXT(", "))));
		}

		// A code a case expects that nothing raises would be a typo in the case.
		for (const FString& Code : Expected)
		{
			const bool bInRange = Code.StartsWith(TEXT("DST4")) || Code.StartsWith(TEXT("DST5")) || Code.StartsWith(TEXT("DST6"));
			if (bInRange && !GetRaisableCodes().Contains(Code))
			{
				AddError(FString::Printf(TEXT("A validate case expects %s, which the semantic stage does not raise."), *Code));
			}
		}

		// The source, when it is on disk: every DST4xxx to DST6xxx code the stage writes must be in the list above, so a
		// new code is noticed here and gets a case.
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamStateTree"));
		const FString SourceDir = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source"), TEXT("DreamStateTreeCompiler"), TEXT("Private")) : FString();
		TArray<FString> SourceFiles;
		if (!SourceDir.IsEmpty())
		{
			IFileManager::Get().FindFiles(SourceFiles, *FPaths::Combine(SourceDir, TEXT("DstLower*.cpp")), /*Files*/ true, /*Directories*/ false);
			SourceFiles.Add(TEXT("DstDriver.cpp"));
		}
		if (SourceFiles.Num() <= 1)
		{
			AddInfo(TEXT("The compiler's source is not on disk here; the codes in the source were not compared with the corpus."));
		}
		else
		{
			TSet<FString> InSource;
			for (const FString& SourceFile : SourceFiles)
			{
				FString Source;
				if (FFileHelper::LoadFileToString(Source, *FPaths::Combine(SourceDir, SourceFile)))
				{
					CollectCodesInSource(Source, InSource);
				}
			}
			TArray<FString> NotListed;
			for (const FString& Code : InSource)
			{
				if (!GetRaisableCodes().Contains(Code))
				{
					NotListed.Add(Code);
				}
			}
			NotListed.Sort();
			if (!NotListed.IsEmpty())
			{
				AddError(FString::Printf(TEXT("The semantic stage raises %s, which the coverage test does not list: add a validate case and the code to GetRaisableCodes."),
					*FString::Join(NotListed, TEXT(", "))));
			}
			TArray<FString> NotRaised;
			for (const FString& Code : GetRaisableCodes())
			{
				if (!InSource.Contains(Code))
				{
					NotRaised.Add(Code);
				}
			}
			if (!NotRaised.IsEmpty())
			{
				AddWarning(FString::Printf(TEXT("The coverage test lists %s, but the source no longer writes it."), *FString::Join(NotRaised, TEXT(", "))));
			}
		}
		return true;
	}
}
