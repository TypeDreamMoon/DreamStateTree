// Copyright DreamMoon. All Rights Reserved.
//
// Round trips of the StateTree assets the project already has (the host's two template trees; the corpus projects' own, when the
// tests run there with -Project). Each asset is a case:
//
//   1. It is extracted to the model; the model gets the names EnsureDstBindNames gives.
//   2. It is decompiled to text, which is compared with `Tests/Corpus/Decompiled/<project>/<package path>.dtr`
//      (the golden mechanism of the other corpora: a `.actual` is written on a miss).
//   3. The text lowers (no generation) to exactly the extracted model.
//   4. The text is generated into /Game/DreamStateTreeTests/RoundTrip/<package path>: it compiles, the compile changes nothing in
//      the editor data (no DST7005), and the asset extracts to the same model again.
//   5. The compiled data of the rebuilt asset is the same as the original's, apart from IDs, which differ by design.
//
// The DST8xxx notes the decompiler makes about what the text cannot say are printed as information; an error fails the case.
// Loading a corpus project's trees loads what they reference, and in the host the templates' Blueprints lack their shared content
// and log errors that say nothing about the trees, so log errors do not fail these tests.

#include "Compiler/DstCompilerTestSupport.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Spike/SpikeBuildHelpers.h"
#include "Spike/SpikeDump.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"

namespace UE::DreamStateTree::Tests::RoundTripTests
{
	FString DropBoundValues(const FString& Dump);

	namespace
	{
		/** The StateTree assets of /Game, minus the ones these tests generate, sorted by path. */
		TArray<FAssetData> FindProjectTrees()
		{
			FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& Registry = RegistryModule.Get();
			if (!Registry.IsSearchAllAssets())
			{
				Registry.SearchAllAssets(/*bSynchronousSearch*/ true);
			}
			else
			{
				Registry.WaitForCompletion();
			}

			FARFilter Filter;
			Filter.ClassPaths.Add(UStateTree::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			Filter.PackagePaths.Add(TEXT("/Game"));
			Filter.bRecursivePaths = true;

			TArray<FAssetData> Assets;
			Registry.GetAssets(Filter, Assets);
			Assets.RemoveAll([](const FAssetData& Asset) { return Asset.PackageName.ToString().StartsWith(TEXT("/Game/DreamStateTreeTests/")); });
			Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString(); });
			return Assets;
		}

		/** `/Game/AI/ST_Brawler` -> `AI/ST_Brawler`. */
		FString GetRelativePackagePath(const UObject& Asset)
		{
			FString Package = Asset.GetOutermost()->GetName();
			Package.RemoveFromStart(TEXT("/Game/"));
			return Package;
		}

		/** One line per error of a sink. */
		void ReportSinkErrors(FAutomationTestBase& Test, const TCHAR* What, const Lang::FDstDiagnosticSink& Sink)
		{
			for (const Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				if (Diagnostic.Severity == Lang::EDstSeverity::Error)
				{
					Test.AddError(FString::Printf(TEXT("%s: %s"), What, *Lang::FDstDiagnosticSink::ToWireString(Diagnostic)));
				}
			}
		}

		/** What the decompiler and the extractor could not say, as information. */
		void ReportSinkNotes(FAutomationTestBase& Test, const TCHAR* What, const Lang::FDstDiagnosticSink& Sink)
		{
			for (const Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				if (Diagnostic.Severity != Lang::EDstSeverity::Error)
				{
					Test.AddInfo(FString::Printf(TEXT("%s: %s"), What, *Lang::FDstDiagnosticSink::ToWireString(Diagnostic)));
				}
			}
		}

		/** Compiles an asset the way the editor does; false (with a note) when it does not compile. */
		bool CompileOriginal(FAutomationTestBase& Test, UStateTree& Asset)
		{
			FStateTreeCompilerLog Log;
			const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(&Asset, Log);
			if (!bCompiled)
			{
				Test.AddWarning(FString::Printf(TEXT("%s does not compile as it is; its compiled data is not compared."), *Asset.GetPathName()));
			}
			return bCompiled;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Assets
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FDstRoundTripCorpusAssetsTest, FDstSpikeBuildTestBase, "DreamStateTree.RoundTrip.Corpus.Assets",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstRoundTripCorpusAssetsTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		for (const FAssetData& Asset : FindProjectTrees())
		{
			FString Name = Asset.PackageName.ToString();
			Name.RemoveFromStart(TEXT("/Game/"));
			OutBeautifiedNames.Add(Name);
			OutTestCommands.Add(Asset.GetSoftObjectPath().ToString());
		}
		if (OutBeautifiedNames.IsEmpty())
		{
			// A project without a StateTree asset must fail loudly, not pass with nothing to run.
			OutBeautifiedNames.Add(TEXT("no_state_tree_assets"));
			OutTestCommands.Add(FString());
		}
	}

	bool FDstRoundTripCorpusAssetsTest::RunTest(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			AddError(TEXT("No StateTree asset was found under /Game."));
			return false;
		}
		UStateTree* Asset = LoadObject<UStateTree>(nullptr, *ObjectPath);
		if (!Asset)
		{
			AddError(FString::Printf(TEXT("Cannot load %s."), *ObjectPath));
			return false;
		}
		CompilerSupport::EnsureTestTags();

		const FString Relative = GetRelativePackagePath(*Asset);
		const FString ReportName = Relative.Replace(TEXT("/"), TEXT("_"));
		CompilerSupport::FGeneratedAssets Generated;

		// 1. The asset as a model.
		Model::FDstTree Original;
		{
			Lang::FDstDiagnosticSink ExtractSink;
			const bool bExtracted = Compiler::ExtractDst(*Asset, Original, ExtractSink);
			ReportSinkNotes(*this, TEXT("Extract"), ExtractSink);
			if (!bExtracted)
			{
				ReportSinkErrors(*this, TEXT("Extract"), ExtractSink);
				AddError(FString::Printf(TEXT("%s cannot be extracted."), *ObjectPath));
				return false;
			}
		}
		const FString OriginalDump = CompilerSupport::DumpEnsured(Original);
		// The model the asset gives, kept so a failure further down can be read against it.
		CompilerSupport::WriteReport(TEXT("RoundTrip"), ReportName + TEXT(".model"), OriginalDump);

		// 2. The asset as text, against its golden.
		FString Text;
		{
			Lang::FDstDiagnosticSink DecompileSink;
			const bool bDecompiled = Compiler::DecompileDstAsset(*Asset, Compiler::FDstDecompileOptions(), Text, DecompileSink);
			ReportSinkNotes(*this, TEXT("Decompile"), DecompileSink);
			if (!bDecompiled)
			{
				ReportSinkErrors(*this, TEXT("Decompile"), DecompileSink);
				AddError(FString::Printf(TEXT("%s cannot be written as text."), *ObjectPath));
				return false;
			}
		}
		const FString Golden = FPaths::Combine(LangSupport::GetCorpusDir(), TEXT("Decompiled"), FApp::GetProjectName(), Relative + TEXT(".dtr"));
		CompilerSupport::CheckGoldenInFolder(*this, Golden, Text);
		const FString TextPath = CompilerSupport::WriteReport(TEXT("RoundTrip"), ReportName + TEXT(".dtr"), Text);
		const FString VirtualPath = FPaths::Combine(CompilerSupport::GetReportDir(TEXT("RoundTrip")), ReportName + TEXT(".dtr"));

		// 3. The text as a model: the one the asset gave.
		{
			Compiler::FDstFileResult Checked = Compiler::BuildDstText(VirtualPath, Text, CompilerSupport::MakeBuildOptions(FString(), /*bGenerate*/ false));
			if (Checked.Diagnostics.HasErrors() || !Checked.Lowered.IsSet())
			{
				ReportSinkErrors(*this, TEXT("Lower"), Checked.Diagnostics);
				AddError(FString::Printf(TEXT("The decompiled text of %s does not lower (%s)."), *ObjectPath, *TextPath));
				return false;
			}
			ReportSinkNotes(*this, TEXT("Lower"), Checked.Diagnostics);
			const FString TextDump = CompilerSupport::DumpEnsured(Checked.Lowered->Tree);
			CompilerSupport::CheckSameText(*this, FString::Printf(TEXT("The decompiled text lowers to another model than the asset extracts to (asset on the left, text in %s)"), *TextPath),
				TEXT("RoundTrip"), ReportName + TEXT(".lowered"), OriginalDump, TextDump);
		}

		// 4. The text as an asset, which reads back as the same model.
		const FString RebuiltPath = FString(TEXT("/Game/DreamStateTreeTests/RoundTrip/")) + Relative;
		Generated.Track(RebuiltPath);
		Compiler::FDstFileResult Built = Compiler::BuildDstText(VirtualPath, Text, CompilerSupport::MakeBuildOptions(FString(), /*bGenerate*/ true, RebuiltPath));
		ReportSinkNotes(*this, TEXT("Build"), Built.Diagnostics);
		if (Built.Diagnostics.HasErrors() || !Built.Generated.IsSet() || !Built.Generated->Asset)
		{
			ReportSinkErrors(*this, TEXT("Build"), Built.Diagnostics);
			AddError(FString::Printf(TEXT("The decompiled text of %s does not build (%s)."), *ObjectPath, *TextPath));
			return false;
		}
		if (const Lang::FDstDiagnostic* Changed = LangSupport::FindDiagnostic(Built.Diagnostics, TEXT("DST7005")))
		{
			AddError(FString::Printf(TEXT("The compile changed the editor data the generator wrote: %s"), *Lang::FDstDiagnosticSink::ToWireString(*Changed)));
		}
		UStateTree* Rebuilt = Built.Generated->Asset;
		TestTrue(TEXT("The rebuilt asset compiled"), Built.Generated->bCompiled);

		{
			Model::FDstTree Again;
			Lang::FDstDiagnosticSink AgainSink;
			if (!Compiler::ExtractDst(*Rebuilt, Again, AgainSink))
			{
				ReportSinkErrors(*this, TEXT("Extract (rebuilt)"), AgainSink);
				AddError(TEXT("The rebuilt asset cannot be extracted."));
			}
			else
			{
				// The asset lives somewhere else now; that is the only thing that may differ.
				Again.AssetPath = Original.AssetPath;
				CompilerSupport::CheckSameText(*this, TEXT("The rebuilt asset extracts to another model than the original (original on the left)"),
					TEXT("RoundTrip"), ReportName + TEXT(".rebuilt"), OriginalDump, CompilerSupport::DumpEnsured(Again));
			}
		}

		// 5. The compiled data, without IDs.
		if (CompileOriginal(*this, *Asset))
		{
			const FString OriginalCompiled = DropBoundValues(CompilerSupport::ScrubGuids(Spike::DumpCompiled(*Asset)));
			const FString RebuiltCompiled = DropBoundValues(CompilerSupport::ScrubGuids(Spike::DumpCompiled(*Rebuilt)));
			const FString Difference = Spike::DiffLines(OriginalCompiled, RebuiltCompiled, 60);
			if (!Difference.IsEmpty())
			{
				CompilerSupport::WriteReport(TEXT("RoundTrip"), ReportName + TEXT(".compiled.original.txt"), OriginalCompiled);
				CompilerSupport::WriteReport(TEXT("RoundTrip"), ReportName + TEXT(".compiled.rebuilt.txt"), RebuiltCompiled);
				AddError(FString::Printf(TEXT("The compiled data of the rebuilt asset differs from the original's (IDs aside); both dumps are in %s.\n%s"),
					*CompilerSupport::GetReportDir(TEXT("RoundTrip")), *Difference));
			}
		}
		return true;
	}

	/**
	 * The dump without the instance values of properties a binding feeds. An asset can hold a value under a binding (what
	 * was typed before the property was bound); the copy overwrites it before the node runs, and the text has no place
	 * for it, so a rebuilt asset holds the default there instead.
	 */
	FString DropBoundValues(const FString& Dump)
	{
		TArray<FString> Lines;
		Dump.ParseIntoArray(Lines, TEXT("\n"), false);
		TSet<FString> Bound;
		// `target={property=Value root=QueryConfig}`: the property a copy ends in, and the one at the top of the target struct
		// it sits in. Both are fed; a value stored in either is what the binding overwrites.
		static const FString TargetMarker = TEXT("target={property=");
		static const FString RootMarker = TEXT(" root=");
		auto ReadName = [](const FString& Line, const int32 Start)
		{
			int32 End = Start;
			while (End < Line.Len() && (FChar::IsAlnum(Line[End]) || Line[End] == TEXT('_')))
			{
				++End;
			}
			return Line.Mid(Start, End - Start);
		};
		for (const FString& Line : Lines)
		{
			const int32 At = Line.Find(TargetMarker, ESearchCase::CaseSensitive);
			if (At != INDEX_NONE)
			{
				Bound.Add(ReadName(Line, At + TargetMarker.Len()));
				const int32 RootAt = Line.Find(RootMarker, ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
				if (RootAt != INDEX_NONE)
				{
					Bound.Add(ReadName(Line, RootAt + RootMarker.Len()));
				}
			}
		}
		TArray<FString> Kept;
		for (FString Line : Lines)
		{
			const FString Trimmed = Line.TrimStart();
			int32 Equals = INDEX_NONE;
			const bool bValueLine = Line.StartsWith(TEXT("      ")) && Trimmed.FindChar(TEXT('='), Equals)
				&& !Trimmed.Contains(TEXT(" ")) && Bound.Contains(Trimmed.Left(Equals));
			// An empty parameter bag and no bag at all are the same thing.
			const bool bEmptyBag = Trimmed.EndsWith(TEXT("PropertyBag_<hash>())"));
			if (bValueLine || bEmptyBag)
			{
				continue;
			}
			// A node's name: the text gives one to every node something binds to, where the asset may have none (the
			// editor then shows the type). The models compare the names; here they would only repeat that.
			const int32 NameAt = Line.Find(TEXT(" name="), ESearchCase::CaseSensitive);
			const int32 NameEnd = NameAt != INDEX_NONE ? Line.Find(TEXT(" instanceTemplate="), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameAt) : INDEX_NONE;
			if (NameEnd != INDEX_NONE)
			{
				Line = Line.Left(NameAt) + TEXT(" name=*") + Line.Mid(NameEnd);
			}
			Kept.Add(Line);
		}
		return FString::Join(Kept, TEXT("\n"));
	}

	// ------------------------------------------------------------------------------------------
	// Inventory
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstRoundTripCorpusInventoryTest, FDstSpikeBuildTestBase, "DreamStateTree.RoundTrip.Corpus.Inventory",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstRoundTripCorpusInventoryTest::RunTest(const FString& Parameters)
	{
		const TArray<FAssetData> Assets = FindProjectTrees();
		AddInfo(FString::Printf(TEXT("%s has %d StateTree asset(s) under /Game."), FApp::GetProjectName(), Assets.Num()));
		for (const FAssetData& Asset : Assets)
		{
			AddInfo(Asset.GetSoftObjectPath().ToString());
		}
		TestTrue(TEXT("The project has at least one StateTree asset to round-trip"), Assets.Num() > 0);
		return true;
	}
}
