// Copyright DreamMoon. All Rights Reserved.
//
// Corpus tests for lowering, generating and decompiling. Every .dtr under Tests/Corpus/Trees is a small tree written in the
// test schema's terms (the fixture nodes and the engine's general ones); together they use every way syntax-v1 writes
// something. Three tests take each tree through one more stage:
//
//   Lower      The text lowers with exactly the diagnostics its `// EXPECT` comments list (none by default), and the model
//              (DumpDstModel) is the same as `<file>.model`.
//   Generate   The text builds into /Game/DreamStateTreeTests/Generated/<name> (unsaved): it compiles, the compile changes
//              nothing in the editor data (no DST7005), and the model extracted from the asset equals the lowered one, both
//              after EnsureDstBindNames. The GUIDs the generator reports are the ones the asset holds.
//   Decompile  The generated asset is decompiled and the text is lowered again: the model is the same. Done with full tag names
//              and with the tag prefixes of the original. The decompiled text is compared with the original's canonical form
//              for information only.
//
// A golden that is missing or different is written next to the case as `<golden>.actual` and the test fails with that path in
// the message; review the .actual and rename it to promote it.
//
// A tree that links another tree's asset says `// REQUIRE <file>`; the required tree is built first. The directives of the behavior
// tests (`// RUN`) are described in DstBehaviorTests.cpp.

#include "Compiler/DstCompilerTestSupport.h"

#include "Spike/SpikeBuildHelpers.h"

namespace UE::DreamStateTree::Tests::TreesCorpus
{
	namespace
	{
		const TArray<FString>& GetNoSkippedFolders()
		{
			static const TArray<FString> None;
			return None;
		}

		/** A tree built from a corpus case, kept as far as the later stages need it. */
		struct FBuiltCase
		{
			FString Path;
			FString CaseDir;
			FString Name;
			FString Text;
			FString GeneratedPath;
			Compiler::FDstFileResult Result;
			UStateTree* Asset = nullptr;
		};

		/** One test failure per error of a sink. */
		void ReportErrors(FAutomationTestBase& Test, const TCHAR* What, const Lang::FDstDiagnosticSink& Sink)
		{
			for (const Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				if (Diagnostic.Severity == Lang::EDstSeverity::Error)
				{
					Test.AddError(FString::Printf(TEXT("%s: %s"), What, *Lang::FDstDiagnosticSink::ToWireString(Diagnostic)));
				}
			}
		}

		/** The warnings and notes of a sink, as information. */
		void ReportOthers(FAutomationTestBase& Test, const TCHAR* What, const Lang::FDstDiagnosticSink& Sink)
		{
			for (const Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				if (Diagnostic.Severity != Lang::EDstSeverity::Error)
				{
					Test.AddInfo(FString::Printf(TEXT("%s: %s"), What, *Lang::FDstDiagnosticSink::ToWireString(Diagnostic)));
				}
			}
		}

		/**
		 * Builds the case into its generated asset: the required trees first, then the case itself. False, with the test failed, when
		 * anything does not build, compile or read back as a tree.
		 */
		bool GenerateCase(FAutomationTestBase& Test, const FString& Path, CompilerSupport::FGeneratedAssets& Assets, FBuiltCase& Out)
		{
			Out.Path = Path;
			Out.CaseDir = FPaths::GetPath(Path);
			Out.Name = FPaths::GetBaseFilename(Path);
			if (!LangSupport::LoadText(Path, Out.Text))
			{
				Test.AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
				return false;
			}

			CompilerSupport::EnsureTestTags();
			TSet<FString> Done;
			if (!CompilerSupport::BuildRequiredTrees(Test, Out.CaseDir, Out.Text, Assets, Done))
			{
				return false;
			}

			Out.GeneratedPath = CompilerSupport::GetGeneratedPackagePath(Out.Name);
			Assets.Track(Out.GeneratedPath);
			Out.Result = Compiler::BuildDstText(Path, Out.Text, CompilerSupport::MakeBuildOptions(Out.CaseDir, /*bGenerate*/ true, Out.GeneratedPath));

			ReportOthers(Test, TEXT("Build"), Out.Result.Diagnostics);
			if (Out.Result.Diagnostics.HasErrors())
			{
				ReportErrors(Test, TEXT("Build"), Out.Result.Diagnostics);
				return false;
			}
			if (const Lang::FDstDiagnostic* Changed = LangSupport::FindDiagnostic(Out.Result.Diagnostics, TEXT("DST7005")))
			{
				Test.AddError(FString::Printf(TEXT("The compile changed the editor data the generator wrote: %s"), *Lang::FDstDiagnosticSink::ToWireString(*Changed)));
				return false;
			}
			if (!Out.Result.Lowered.IsSet() || !Out.Result.Generated.IsSet() || !Out.Result.Generated->Asset)
			{
				Test.AddError(TEXT("The build reported no error but produced no asset."));
				return false;
			}
			Out.Asset = Out.Result.Generated->Asset;
			if (!Out.Result.Generated->bCompiled)
			{
				Test.AddError(TEXT("The generated asset did not compile."));
				return false;
			}
			if (Out.Result.Generated->bCompileChangedEditorData)
			{
				Test.AddError(TEXT("The engine's compile changed the generated editor data."));
				return false;
			}
			return true;
		}

		/** A corpus case listed by the Lower, Generate and Decompile tests alike. */
		void ListTreeCases(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
		{
			CompilerSupport::ListCases(CompilerSupport::GetCorpusSubDir(TEXT("Trees")), /*bTrees*/ true, /*bLibraries*/ false, GetNoSkippedFolders(),
				OutBeautifiedNames, OutTestCommands);
		}
	}

	// ------------------------------------------------------------------------------------------
	// Lower
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDstLowerCorpusTest, "DreamStateTree.Compiler.Lower.Corpus",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstLowerCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		ListTreeCases(OutBeautifiedNames, OutTestCommands);
	}

	bool FDstLowerCorpusTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No tree cases found under %s/Trees; is the DreamStateTree plugin's Tests/Corpus folder there?"), *LangSupport::GetCorpusDir()));
			return false;
		}

		FString Text;
		if (!LangSupport::LoadText(Path, Text))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}

		CompilerSupport::EnsureTestTags();
		CompilerSupport::FGeneratedAssets Assets;
		TSet<FString> Done;
		const FString CaseDir = FPaths::GetPath(Path);
		if (!CompilerSupport::BuildRequiredTrees(*this, CaseDir, Text, Assets, Done))
		{
			return false;
		}

		const Compiler::FDstFileResult Result = Compiler::BuildDstText(Path, Text, CompilerSupport::MakeBuildOptions(CaseDir, /*bGenerate*/ false));
		LangSupport::CheckDiagnostics(*this, Result.FilePath, LangSupport::ReadExpectations(Text), Result.Diagnostics.GetDiagnostics());
		if (Result.Diagnostics.HasErrors() || !Result.Lowered.IsSet())
		{
			AddError(TEXT("The tree does not lower."));
			return false;
		}

		CompilerSupport::CheckGoldenInFolder(*this, Path + TEXT(".model"), Model::DumpDstModel(Result.Lowered->Tree));
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Generate
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FDstGenerateCorpusTest, FDstSpikeBuildTestBase, "DreamStateTree.Compiler.Generate.Corpus",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstGenerateCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		ListTreeCases(OutBeautifiedNames, OutTestCommands);
	}

	bool FDstGenerateCorpusTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No tree cases found under %s/Trees."), *LangSupport::GetCorpusDir()));
			return false;
		}

		CompilerSupport::FGeneratedAssets Assets;
		FBuiltCase Built;
		if (!GenerateCase(*this, Path, Assets, Built))
		{
			return false;
		}

		// The asset reads back as the model the text lowered to.
		Model::FDstTree Extracted;
		Compiler::FDstExtractResult ExtractIds;
		Lang::FDstDiagnosticSink ExtractSink;
		const bool bExtracted = Compiler::ExtractDst(*Built.Asset, Extracted, ExtractSink, &ExtractIds);
		ReportOthers(*this, TEXT("Extract"), ExtractSink);
		if (!bExtracted)
		{
			ReportErrors(*this, TEXT("Extract"), ExtractSink);
			AddError(TEXT("The generated asset cannot be read back."));
			return false;
		}

		const FString LoweredDump = CompilerSupport::DumpEnsured(Built.Result.Lowered->Tree);
		const FString ExtractedDump = CompilerSupport::DumpEnsured(Extracted);
		CompilerSupport::CheckSameText(*this, TEXT("The model extracted from the generated asset differs from the lowered one (lowered on the left)"),
			TEXT("Generate"), Built.Name, LoweredDump, ExtractedDump);

		// The GUIDs the generator derived are the ones the asset holds, which the write-back relies on to match things up.
		int32 NumMismatched = 0;
		int32 NumOnlyGenerated = 0;
		FString FirstMismatch;
		for (const TPair<FString, FGuid>& Pair : Built.Result.Generated->Ids)
		{
			if (const FGuid* Found = ExtractIds.Ids.Find(Pair.Key))
			{
				if (*Found != Pair.Value)
				{
					++NumMismatched;
					if (FirstMismatch.IsEmpty())
					{
						FirstMismatch = Pair.Key;
					}
				}
			}
			else
			{
				++NumOnlyGenerated;
			}
		}
		if (NumMismatched > 0)
		{
			AddError(FString::Printf(TEXT("%d of the IDs the generator reports differ from the ones the asset holds (first: %s)."), NumMismatched, *FirstMismatch));
		}
		if (NumOnlyGenerated > 0)
		{
			AddInfo(FString::Printf(TEXT("%d of the generator's IDs have no key of the same name in what the extractor reports."), NumOnlyGenerated));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Decompile
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FDstDecompileCorpusTest, FDstSpikeBuildTestBase, "DreamStateTree.Compiler.Decompile.Corpus",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstDecompileCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		ListTreeCases(OutBeautifiedNames, OutTestCommands);
	}

	bool FDstDecompileCorpusTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No tree cases found under %s/Trees."), *LangSupport::GetCorpusDir()));
			return false;
		}

		CompilerSupport::FGeneratedAssets Assets;
		FBuiltCase Built;
		if (!GenerateCase(*this, Path, Assets, Built))
		{
			return false;
		}
		const FString OriginalDump = CompilerSupport::DumpEnsured(Built.Result.Lowered->Tree);

		bool bAllLowered = true;
		for (int32 Variant = 0; Variant < 2; ++Variant)
		{
			const bool bPrefixed = Variant == 1;
			const FString Label = FString::Printf(TEXT("Decompile (%s)"), bPrefixed ? TEXT("tag prefixes") : TEXT("full tag names"));
			const FString FileName = Built.Name + (bPrefixed ? TEXT("_prefixed") : TEXT(""));

			Compiler::FDstDecompileOptions Options;
			if (bPrefixed)
			{
				Options.TagPrefixes = Built.Result.Lowered->TagPrefixes;
			}
			FString Decompiled;
			Lang::FDstDiagnosticSink DecompileSink;
			const bool bDecompiled = Compiler::DecompileDstAsset(*Built.Asset, Options, Decompiled, DecompileSink);
			ReportOthers(*this, *Label, DecompileSink);
			if (!bDecompiled)
			{
				ReportErrors(*this, *Label, DecompileSink);
				AddError(FString::Printf(TEXT("%s: the generated asset cannot be written as text."), *Label));
				bAllLowered = false;
				continue;
			}
			const FString TextPath = CompilerSupport::WriteReport(TEXT("Decompile"), FileName + TEXT(".dtr"), Decompiled);

			// The text lowers again, to the same model.
			const FString VirtualPath = FPaths::Combine(CompilerSupport::GetReportDir(TEXT("Decompile")), FileName + TEXT(".dtr"));
			Compiler::FDstFileResult Again = Compiler::BuildDstText(VirtualPath, Decompiled,
				CompilerSupport::MakeBuildOptions(Built.CaseDir, /*bGenerate*/ false, Built.GeneratedPath));
			if (Again.Diagnostics.HasErrors() || !Again.Lowered.IsSet())
			{
				ReportErrors(*this, *Label, Again.Diagnostics);
				AddError(FString::Printf(TEXT("%s: the decompiled text does not lower (%s)."), *Label, *TextPath));
				bAllLowered = false;
				continue;
			}
			ReportOthers(*this, *Label, Again.Diagnostics);
			const FString AgainDump = CompilerSupport::DumpEnsured(Again.Lowered->Tree);
			CompilerSupport::CheckSameText(*this, FString::Printf(TEXT("%s: the decompiled text lowers to another model (original on the left, text in %s)"), *Label, *TextPath),
				TEXT("Decompile"), FileName, OriginalDump, AgainDump);

			// For information: how far the decompiled text is from the author's, once both are in canonical form.
			FString Canonical;
			Lang::FDstDiagnosticSink FormatSink;
			if (Compiler::FormatDstText(Path, Built.Text, Canonical, FormatSink))
			{
				const FString Difference = LangSupport::DescribeDifference(Canonical, Decompiled);
				AddInfo(FString::Printf(TEXT("%s: the text %s the original in canonical form%s%s."), *Label, Difference.IsEmpty() ? TEXT("is") : TEXT("is not"),
					Difference.IsEmpty() ? TEXT("") : TEXT(" ("), Difference.IsEmpty() ? TEXT("") : *(Difference + TEXT(")"))));
			}
		}
		return bAllLowered;
	}

	// ------------------------------------------------------------------------------------------
	// Coverage
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLowerCoverageTest, "DreamStateTree.Compiler.Lower.Coverage",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLowerCoverageTest::RunTest(const FString& Parameters)
	{
		const FString TreesDir = CompilerSupport::GetCorpusSubDir(TEXT("Trees"));
		TArray<FString> Trees;
		CompilerSupport::CollectCaseFiles(TreesDir, /*bTrees*/ true, /*bLibraries*/ false, GetNoSkippedFolders(), Trees);
		TArray<FString> Libraries;
		CompilerSupport::CollectCaseFiles(TreesDir, /*bTrees*/ false, /*bLibraries*/ true, GetNoSkippedFolders(), Libraries);
		if (Trees.IsEmpty())
		{
			AddError(FString::Printf(TEXT("The tree corpus under %s is missing or empty."), *LangSupport::GetCorpusDir()));
			return false;
		}
		TestTrue(TEXT("The tree corpus has at least 15 trees"), Trees.Num() >= 15);

		// Everything the texts say, to look the syntax up in.
		FString AllText;
		TSet<FString> Names;
		for (const FString& File : Trees)
		{
			FString Text;
			if (!LangSupport::LoadText(File, Text))
			{
				AddError(FString::Printf(TEXT("Cannot read %s"), *File));
				continue;
			}
			AllText += Text;
			AllText += TEXT("\n");

			// The convention the tests rely on: a tree is called what its file is, below the generated folder, so that a link finds
			// it and the override of the asset path changes nothing.
			const FString Base = FPaths::GetBaseFilename(File);
			if (!Text.Contains(FString::Printf(TEXT("Name=\"DreamStateTreeTests/Generated/%s\""), *Base), ESearchCase::CaseSensitive))
			{
				AddError(FString::Printf(TEXT("%s must be named Name=\"DreamStateTreeTests/Generated/%s\" in its header."), *FPaths::GetCleanFilename(File), *Base));
			}
			if (Names.Contains(Base))
			{
				AddError(FString::Printf(TEXT("Two trees are called %s."), *Base));
			}
			Names.Add(Base);
		}
		for (const FString& File : Libraries)
		{
			FString Text;
			if (LangSupport::LoadText(File, Text))
			{
				AllText += Text;
				AllText += TEXT("\n");
			}
		}

		// Every way the syntax writes something is used by some tree.
		struct FFeature
		{
			const TCHAR* Name;
			const TCHAR* Needle;
		};
		static const FFeature Features[] = {
			{ TEXT("Settings section"), TEXT("Settings = {") },
			{ TEXT("Parameters section"), TEXT("Parameters = {") },
			{ TEXT("Globals section"), TEXT("Globals = {") },
			{ TEXT("Tags section"), TEXT("Tags = {") },
			{ TEXT("Colors section"), TEXT("Colors = {") },
			{ TEXT("parameter meta"), TEXT("[ Category = ") },
			{ TEXT("int64 parameter"), TEXT("int64 ") },
			{ TEXT("double parameter"), TEXT("double ") },
			{ TEXT("Name parameter"), TEXT("Name Label") },
			{ TEXT("String parameter"), TEXT("String Title") },
			{ TEXT("struct parameter"), TEXT("FVector ") },
			{ TEXT("enum parameter"), TEXT("EDstTestMode ") },
			{ TEXT("array parameter"), TEXT("Array<int>") },
			{ TEXT("object parameter"), TEXT("DstTestOwner Boss") },
			{ TEXT("class parameter"), TEXT("Class<Actor>") },
			{ TEXT("group state"), TEXT("[ Group ]") },
			{ TEXT("state color"), TEXT("Color = ") },
			{ TEXT("state tag"), TEXT("Tag = ") },
			{ TEXT("state description"), TEXT("Description = ") },
			{ TEXT("disabled state"), TEXT("Enabled = false") },
			{ TEXT("complete all"), TEXT("Complete = All") },
			{ TEXT("tick rate"), TEXT("TickRate = ") },
			{ TEXT("weight"), TEXT("Weight = ") },
			{ TEXT("state flag"), TEXT("bCopyParameterBindingsOnTick") },
			{ TEXT("pick InOrder"), TEXT("pick InOrder") },
			{ TEXT("pick Random"), TEXT("pick Random") },
			{ TEXT("pick UniformRandom"), TEXT("pick UniformRandom") },
			{ TEXT("pick HighestUtility"), TEXT("pick HighestUtility") },
			{ TEXT("pick WeightedUtility"), TEXT("pick WeightedUtility") },
			{ TEXT("pick RelativeUtility"), TEXT("pick RelativeUtility") },
			{ TEXT("pick FollowTransitions"), TEXT("pick FollowTransitions") },
			{ TEXT("pick Enter"), TEXT("pick Enter") },
			{ TEXT("pick None"), TEXT("pick None") },
			{ TEXT("utility constant"), TEXT("utility 0.5") },
			{ TEXT("utility node"), TEXT("DstTestConsideration(") },
			{ TEXT("utility positional node"), TEXT("FloatInput(Owner.Health") },
			{ TEXT("utility product"), TEXT(") * 0.9") },
			{ TEXT("utility or"), TEXT(" or DstTestConsideration") },
			{ TEXT("grouped condition"), TEXT(" and (") },
			{ TEXT("nested group"), TEXT(" or (") },
			{ TEXT("not on a comparison"), TEXT("not (Owner") },
			{ TEXT("not on a path"), TEXT("not Params.") },
			{ TEXT("equality"), TEXT(" == ") },
			{ TEXT("inequality"), TEXT(" != ") },
			{ TEXT("less than"), TEXT(" < ") },
			{ TEXT("greater or equal"), TEXT(" >= ") },
			{ TEXT("constant on the left"), TEXT("3 < Owner.Level") },
			{ TEXT("random condition"), TEXT("Random(0.3)") },
			{ TEXT("object condition"), TEXT("ObjectIsValid(") },
			{ TEXT("condition node"), TEXT("DstTestCondition(") },
			{ TEXT("evaluation mode"), TEXT("Evaluation = ForcedTrue") },
			{ TEXT("node name setting"), TEXT("[ Name = ") },
			{ TEXT("completion setting"), TEXT("ConsideredForCompletion = false") },
			{ TEXT("priority setting"), TEXT("TransitionPriority = High") },
			{ TEXT("disabled task"), TEXT("[ Enabled = false ]") },
			{ TEXT("arithmetic"), TEXT("Params.Speed * 0.5") },
			{ TEXT("division and subtraction"), TEXT("/ 2 - 1") },
			{ TEXT("negation"), TEXT("-Params.Speed") },
			{ TEXT("logic"), TEXT("and not Params.Calm") },
			{ TEXT("folded constants"), TEXT("2 * 3") },
			{ TEXT("property function call"), TEXT("AbsoluteFloat(") },
			{ TEXT("tuple with bindings"), TEXT("(Params.Home.X, 0, ") },
			{ TEXT("output binding"), TEXT("-> Params.") },
			{ TEXT("property reference"), TEXT("ref Stats.Counter") },
			{ TEXT("whole context"), TEXT("Target = Owner") },
			{ TEXT("global node output"), TEXT("Stats.Counter") },
			{ TEXT("task output"), TEXT("Worker.TickCount") },
			{ TEXT("event payload"), TEXT("Event.Amount") },
			{ TEXT("event of a named state"), TEXT("Fight.Event.Amount") },
			{ TEXT("event tag"), TEXT("Event.Tag") },
			{ TEXT("event in a transition condition"), TEXT("Event.Severity > 0.5") },
			{ TEXT("state event with payload"), TEXT("on Hit(DstTestPayload)") },
			{ TEXT("state event without payload"), TEXT("on Ask") },
			{ TEXT("state event of a child tag"), TEXT("on Hit.Light(") },
			{ TEXT("event setting"), TEXT("[ ConsumeEvent = false ]") },
			{ TEXT("trigger Succeeded"), TEXT("on Succeeded") },
			{ TEXT("trigger Failed"), TEXT("on Failed") },
			{ TEXT("trigger Completed"), TEXT("on Completed") },
			{ TEXT("trigger Tick"), TEXT("on Tick") },
			{ TEXT("trigger delegate"), TEXT("on Delegate(") },
			{ TEXT("task completion Succeeds"), TEXT(".Succeeds)") },
			{ TEXT("task completion Fails"), TEXT(".Fails)") },
			{ TEXT("task completion Completes"), TEXT(".Completes)") },
			{ TEXT("dispatcher"), TEXT(".OnFinished)") },
			{ TEXT("target Next"), TEXT("-> Next") },
			{ TEXT("target NextSelectable"), TEXT("-> NextSelectable ") },
			{ TEXT("target Parent"), TEXT("-> Parent ") },
			{ TEXT("target NextParent"), TEXT("-> NextParent ") },
			{ TEXT("target NextSelectableParent"), TEXT("-> NextSelectableParent ") },
			{ TEXT("target TreeSucceeded"), TEXT("-> TreeSucceeded") },
			{ TEXT("target TreeFailed"), TEXT("-> TreeFailed") },
			{ TEXT("target path"), TEXT("-> Combat.Strike") },
			{ TEXT("option Priority Low"), TEXT("Priority = Low") },
			{ TEXT("option Priority Medium"), TEXT("Priority = Medium") },
			{ TEXT("option Priority High"), TEXT("Priority = High") },
			{ TEXT("option Priority Critical"), TEXT("Priority = Critical") },
			{ TEXT("option Delay"), TEXT("Delay = 1") },
			{ TEXT("option Variance"), TEXT("Variance = ") },
			{ TEXT("option Reactivate"), TEXT("Reactivate") },
			{ TEXT("subtree"), TEXT("Subtree ") },
			{ TEXT("link"), TEXT("link Flinch") },
			{ TEXT("link with arguments"), TEXT("link Flinch(Intensity = 1 + Params.Courage") },
			{ TEXT("link with a positional argument"), TEXT("link Wobble(0.8)") },
			{ TEXT("link asset"), TEXT("link asset LinkChild(") },
			{ TEXT("include"), TEXT("#include ") },
			{ TEXT("template"), TEXT("Template ") },
			{ TEXT("template with a default"), TEXT("Template Work(Ticks = 2)") },
			{ TEXT("Blueprint node"), TEXT("DstTestBlueprintTask(") },
			{ TEXT("namespaced node"), TEXT("= Move(2, true)") },
			{ TEXT("qualified node"), TEXT("TestAgent.Move(") },
			{ TEXT("alias in a namespace"), TEXT("= Run(") },
			{ TEXT("global alias"), TEXT("= Sprint(") },
			{ TEXT("struct name"), TEXT("DstTestMoveTask(") },
			{ TEXT("bool without its b"), TEXT("Sprint = true") },
			{ TEXT("backtick name"), TEXT("`Move To`") },
			{ TEXT("backtick task name"), TEXT("`Take Off`") },
			{ TEXT("non-ASCII name"), TEXT("巡逻") },
			{ TEXT("engine schema with a context"), TEXT("Schema=Component(") },
			{ TEXT("restricted schema"), TEXT("Schema=RestrictedAgent") },
			{ TEXT("behavior run"), TEXT("// RUN ") },
		};
		TArray<FString> Unused;
		for (const FFeature& Feature : Features)
		{
			if (!AllText.Contains(Feature.Needle, ESearchCase::CaseSensitive))
			{
				Unused.Add(FString::Printf(TEXT("%s (`%s`)"), Feature.Name, Feature.Needle));
			}
		}
		if (!Unused.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No tree uses: %s."), *FString::Join(Unused, TEXT("; "))));
		}

		// A golden whose case is gone would never be looked at again.
		for (const TCHAR* Extension : { TEXT("model"), TEXT("run") })
		{
			TArray<FString> Goldens;
			IFileManager::Get().FindFilesRecursive(Goldens, *TreesDir, *FString::Printf(TEXT("*.%s"), Extension), /*Files*/ true, /*Directories*/ false);
			for (const FString& Golden : Goldens)
			{
				const FString CasePath = Golden.LeftChop(FCString::Strlen(Extension) + 1);
				if (!IFileManager::Get().FileExists(*CasePath))
				{
					AddError(FString::Printf(TEXT("The golden %s has no case file %s."), *Golden, *CasePath));
				}
			}
		}
		return true;
	}
}
