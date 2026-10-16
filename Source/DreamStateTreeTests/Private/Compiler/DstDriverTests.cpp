// Copyright DreamMoon. All Rights Reserved.
//
// Tests of the driver: the file-level pipeline the commandlet and the editor run. Sources are written to a folder of their own
// under <project>/Saved/DreamStateTreeTests/Driver/<test>/ so that a test controls every file, and nothing is saved to the
// project's content.
//
//   SourceIndex   scanning, finding a source by file and by asset, the trees a library change rebuilds, rescanning one file
//   Sources       FindDstSources and IsDstSourceFile
//   BuildOrder    BuildDstFiles builds a tree after the tree whose asset it links, whatever order the files are given in
//   Library       a library given to BuildDstFiles stands for the trees that include it; a library nobody includes is only checked
//   Cycle         trees that link each other are DST6009 and none is built
//   Format        FormatDstFile / FormatDstText: canonical text is left alone, formatting is idempotent, text that does not parse fails
//   DefaultPath   where a decompiled asset's text goes
//   Diagnostics   the line form and the JSON form diagnostics are printed in

#include "Compiler/DstCompilerTestSupport.h"

#include "Spike/SpikeBuildHelpers.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::DreamStateTree::Tests::DriverTests
{
	namespace
	{
		/** A fresh folder for one test. */
		FString MakeTestDir(const TCHAR* Name)
		{
			const FString Dir = CompilerSupport::NormalizeDirectory(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamStateTreeTests"), TEXT("Driver"), Name));
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
			IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
			return Dir;
		}

		/** Writes a source below Dir and returns its normalised path. */
		FString WriteSource(const FString& Dir, const TCHAR* Relative, const FString& Text)
		{
			const FString Path = CompilerSupport::NormalizeDirectory(FPaths::Combine(Dir, Relative));
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
			FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return Path;
		}

		bool SameFile(const FString& A, const FString& B)
		{
			return CompilerSupport::NormalizeDirectory(A).Equals(CompilerSupport::NormalizeDirectory(B), ESearchCase::IgnoreCase);
		}

		/** A tree header and a body of one state with one task. */
		FString MakeTreeText(const TCHAR* Includes, const TCHAR* Name)
		{
			return FString::Printf(TEXT("%s\nStateTree(Name=\"DreamStateTreeTests/Driver/%s\", Schema=TestAgent)\n{\n    State Root {\n        do Delay(1)\n    }\n}\n"), Includes, Name);
		}

		/**
		 * The sources the index and library tests share:
		 *   AI/Alpha.dtr  includes Common/Shared.dth
		 *   AI/Beta.dtr   includes Common/Wrapper.dth, which includes Shared.dth
		 *   Other/Gamma.dtr  includes nothing
		 * and a notes.txt that is not a source.
		 */
		struct FStandardSources
		{
			FString Alpha;
			FString Beta;
			FString Gamma;
			FString Shared;
			FString Wrapper;
		};

		FStandardSources WriteStandardSources(const FString& Dir)
		{
			FStandardSources Sources;
			Sources.Shared = WriteSource(Dir, TEXT("Common/Shared.dth"),
				TEXT("StateTreeLibrary(Name=\"DreamStateTreeTests/Driver/Shared\")\n{\n    Tags = { DreamStateTreeTest.Event; }\n}\n"));
			Sources.Wrapper = WriteSource(Dir, TEXT("Common/Wrapper.dth"),
				TEXT("#include \"Shared.dth\"\n\nStateTreeLibrary(Name=\"DreamStateTreeTests/Driver/Wrapper\")\n{\n    Template Healthy = Owner.Health > 3;\n}\n"));
			Sources.Alpha = WriteSource(Dir, TEXT("AI/Alpha.dtr"), MakeTreeText(TEXT("#include \"Common/Shared.dth\""), TEXT("Alpha")));
			Sources.Beta = WriteSource(Dir, TEXT("AI/Beta.dtr"), MakeTreeText(TEXT("#include \"Common/Wrapper.dth\""), TEXT("Beta")));
			Sources.Gamma = WriteSource(Dir, TEXT("Other/Gamma.dtr"), MakeTreeText(TEXT(""), TEXT("Gamma")));
			WriteSource(Dir, TEXT("notes.txt"), TEXT("Not a source.\n"));
			return Sources;
		}

	}

	// ------------------------------------------------------------------------------------------
	// SourceIndex
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstDriverSourceIndexTest, "DreamStateTree.Compiler.Driver.SourceIndex",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverSourceIndexTest::RunTest(const FString& Parameters)
	{
		const FString Dir = MakeTestDir(TEXT("SourceIndex"));
		const FStandardSources Sources = WriteStandardSources(Dir);
		const TArray<FString> Roots = { Dir };

		Compiler::FDstSourceIndex Index;
		Index.Scan(Roots);
		TestEqual(TEXT("The index holds the five sources and not notes.txt"), Index.GetEntries().Num(), 5);
		TestEqual(TEXT("The index remembers its roots"), Index.GetRoots().Num(), 1);

		// By file.
		const Compiler::FDstSourceEntry* Alpha = Index.FindByFile(Sources.Alpha);
		if (!TestNotNull(TEXT("Alpha is indexed"), Alpha))
		{
			return false;
		}
		TestTrue(TEXT("Alpha is a tree"), Alpha->Kind == Lang::EDstFileKind::StateTree);
		TestEqual(TEXT("Alpha makes /Game/DreamStateTreeTests/Driver/Alpha"), Alpha->AssetPath, FString(TEXT("/Game/DreamStateTreeTests/Driver/Alpha.Alpha")));
		if (TestEqual(TEXT("Alpha includes one file"), Alpha->Includes.Num(), 1))
		{
			TestTrue(TEXT("Alpha includes Shared.dth, resolved against the root"), SameFile(Alpha->Includes[0], Sources.Shared));
		}
		const Compiler::FDstSourceEntry* Shared = Index.FindByFile(Sources.Shared);
		if (TestNotNull(TEXT("Shared.dth is indexed"), Shared))
		{
			TestTrue(TEXT("Shared.dth is a library"), Shared->Kind == Lang::EDstFileKind::Library);
			TestTrue(TEXT("A library makes no asset"), Shared->AssetPath.IsEmpty());
		}
		const Compiler::FDstSourceEntry* Wrapper = Index.FindByFile(Sources.Wrapper);
		if (TestNotNull(TEXT("Wrapper.dth is indexed"), Wrapper) && TestEqual(TEXT("Wrapper includes one file"), Wrapper->Includes.Num(), 1))
		{
			TestTrue(TEXT("Wrapper includes Shared.dth, found next to it"), SameFile(Wrapper->Includes[0], Sources.Shared));
		}
		TestNull(TEXT("A file that is not a source is not indexed"), Index.FindByFile(Dir / TEXT("notes.txt")));

		// By asset: the package path and the object path both find it.
		const Compiler::FDstSourceEntry* ByPackage = Index.FindByAsset(TEXT("/Game/DreamStateTreeTests/Driver/Beta"));
		if (TestNotNull(TEXT("Beta is found by its package path"), ByPackage))
		{
			TestTrue(TEXT("...and it is Beta.dtr"), SameFile(ByPackage->FilePath, Sources.Beta));
		}
		TestNotNull(TEXT("Beta is found by its object path"), Index.FindByAsset(TEXT("/Game/DreamStateTreeTests/Driver/Beta.Beta")));
		TestNotNull(TEXT("Asset names are not case-sensitive"), Index.FindByAsset(TEXT("/game/dreamstatetreetests/driver/BETA")));
		TestNull(TEXT("An asset no source makes is not found"), Index.FindByAsset(TEXT("/Game/DreamStateTreeTests/Driver/Nobody")));

		// What a library change rebuilds: the trees that include it, directly or through another library.
		{
			const TArray<FString> OfShared = Index.FindDependents(Sources.Shared);
			if (TestEqual(TEXT("Shared.dth is used by two trees"), OfShared.Num(), 2))
			{
				TestTrue(TEXT("...Alpha"), SameFile(OfShared[0], Sources.Alpha));
				TestTrue(TEXT("...and Beta, which reaches it through Wrapper.dth"), SameFile(OfShared[1], Sources.Beta));
			}
			const TArray<FString> OfWrapper = Index.FindDependents(Sources.Wrapper);
			if (TestEqual(TEXT("Wrapper.dth is used by one tree"), OfWrapper.Num(), 1))
			{
				TestTrue(TEXT("...Beta"), SameFile(OfWrapper[0], Sources.Beta));
			}
			TestEqual(TEXT("A tree has no dependents"), Index.FindDependents(Sources.Gamma).Num(), 0);
		}

		// One file changes: Beta stops including the wrapper.
		WriteSource(Dir, TEXT("AI/Beta.dtr"), MakeTreeText(TEXT(""), TEXT("Beta")));
		Index.Rescan(Sources.Beta);
		TestEqual(TEXT("Rescanning a changed file keeps the number of entries"), Index.GetEntries().Num(), 5);
		TestEqual(TEXT("Wrapper.dth has no dependents once Beta lets go of it"), Index.FindDependents(Sources.Wrapper).Num(), 0);
		{
			const TArray<FString> OfShared = Index.FindDependents(Sources.Shared);
			if (TestEqual(TEXT("Shared.dth is used by Alpha alone"), OfShared.Num(), 1))
			{
				TestTrue(TEXT("...Alpha"), SameFile(OfShared[0], Sources.Alpha));
			}
		}

		// A file goes away.
		IFileManager::Get().Delete(*Sources.Gamma);
		Index.Rescan(Sources.Gamma);
		TestNull(TEXT("A deleted source leaves the index"), Index.FindByFile(Sources.Gamma));
		TestNull(TEXT("...and so does its asset"), Index.FindByAsset(TEXT("/Game/DreamStateTreeTests/Driver/Gamma")));
		TestEqual(TEXT("Four sources are left"), Index.GetEntries().Num(), 4);

		// A file appears.
		const FString Delta = WriteSource(Dir, TEXT("Other/Delta.dtr"), MakeTreeText(TEXT(""), TEXT("Delta")));
		Index.Rescan(Delta);
		const Compiler::FDstSourceEntry* DeltaEntry = Index.FindByAsset(TEXT("/Game/DreamStateTreeTests/Driver/Delta"));
		if (TestNotNull(TEXT("A new source enters the index"), DeltaEntry))
		{
			TestTrue(TEXT("...as Delta.dtr"), SameFile(DeltaEntry->FilePath, Delta));
		}
		TestEqual(TEXT("Five sources again"), Index.GetEntries().Num(), 5);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Sources
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstDriverSourcesTest, "DreamStateTree.Compiler.Driver.Sources",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverSourcesTest::RunTest(const FString& Parameters)
	{
		TestTrue(TEXT("A .dtr is a source"), Compiler::IsDstSourceFile(TEXT("DTree/AI/ST_Brawler.dtr")));
		TestTrue(TEXT("A .dth is a source"), Compiler::IsDstSourceFile(TEXT("DTree/Common/Villager.dth")));
		TestTrue(TEXT("The extension is not case-sensitive"), Compiler::IsDstSourceFile(TEXT("DTree/AI/ST_Brawler.DTR")));
		TestFalse(TEXT("A .txt is not a source"), Compiler::IsDstSourceFile(TEXT("DTree/notes.txt")));
		TestFalse(TEXT("A longer extension that starts the same is not a source"), Compiler::IsDstSourceFile(TEXT("DTree/AI/ST_Brawler.dtree")));

		const FString Dir = MakeTestDir(TEXT("Sources"));
		const FStandardSources Sources = WriteStandardSources(Dir);
		WriteSource(Dir, TEXT("Other/readme.dtr.bak"), TEXT("A backup.\n"));

		const TArray<FString> Roots = { Dir };
		const TArray<FString> Found = Compiler::FindDstSources(Roots);
		if (TestEqual(TEXT("FindDstSources finds the five sources"), Found.Num(), 5))
		{
			// Sorted, absolute.
			for (int32 Index = 1; Index < Found.Num(); ++Index)
			{
				TestTrue(TEXT("The sources come sorted"), Found[Index - 1] < Found[Index]);
			}
			TestTrue(TEXT("The first is Alpha"), SameFile(Found[0], Sources.Alpha));
		}
		for (const FString& File : Found)
		{
			TestFalse(TEXT("A found path is absolute"), FPaths::IsRelative(File));
		}

		// The same folder twice names every source once.
		const TArray<FString> Twice = { Dir, Dir };
		TestEqual(TEXT("A root given twice finds each source once"), Compiler::FindDstSources(Twice).Num(), 5);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// BuildOrder
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstDriverBuildOrderTest, FDstSpikeBuildTestBase, "DreamStateTree.Compiler.Driver.BuildOrder",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverBuildOrderTest::RunTest(const FString& Parameters)
	{
		CompilerSupport::EnsureTestTags();
		const FString Dir = MakeTestDir(TEXT("BuildOrder"));
		const FString Child = WriteSource(Dir, TEXT("ZChild.dtr"),
			TEXT("StateTree(Name=\"DreamStateTreeTests/Driver/DriverChild\", Schema=TestAgent)\n{\n    Parameters = { float Radius = 2; }\n\n    State ChildRoot {\n        do Delay(Params.Radius)\n    }\n}\n"));
		const FString Parent = WriteSource(Dir, TEXT("AParent.dtr"),
			TEXT("StateTree(Name=\"DreamStateTreeTests/Driver/DriverParent\", Schema=TestAgent)\n{\n    State Root {\n        link asset DriverChild(Radius = 5)\n    }\n}\n"));

		for (int32 Order = 0; Order < 2; ++Order)
		{
			// Whatever order the files are given in, the linked tree is built first. The parent's file name sorts before the
			// child's, so building in name order would build it too early.
			CompilerSupport::FGeneratedAssets Assets;
			Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/DriverChild"));
			Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/DriverParent"));

			const TArray<FString> Files = Order == 0 ? TArray<FString>{ Parent, Child } : TArray<FString>{ Child, Parent };
			Compiler::FDstBuildOptions Options = CompilerSupport::MakeBuildOptions(Dir, /*bGenerate*/ true);
			const TArray<Compiler::FDstFileResult> Results = Compiler::BuildDstFiles(Files, Options);

			const FString Label = Order == 0 ? TEXT("parent given first") : TEXT("child given first");
			if (!TestEqual(*FString::Printf(TEXT("%s: one result per file"), *Label), Results.Num(), 2))
			{
				return false;
			}
			TestTrue(*FString::Printf(TEXT("%s: the child is built first"), *Label), SameFile(Results[0].FilePath, Child));
			TestTrue(*FString::Printf(TEXT("%s: the parent is built second"), *Label), SameFile(Results[1].FilePath, Parent));
			for (const Compiler::FDstFileResult& Result : Results)
			{
				if (!TestFalse(*FString::Printf(TEXT("%s: %s builds without an error (%s)"), *Label, *FPaths::GetCleanFilename(Result.FilePath),
					*CompilerSupport::DescribeErrors(Result.Diagnostics)), Result.Diagnostics.HasErrors()))
				{
					continue;
				}
				TestTrue(*FString::Printf(TEXT("%s: %s produced an asset"), *Label, *FPaths::GetCleanFilename(Result.FilePath)),
					Result.Generated.IsSet() && Result.Generated->Asset != nullptr && Result.Generated->bCompiled);
			}

			// The parent really links the child's asset.
			if (const UStateTree* ParentAsset = FindObject<UStateTree>(nullptr, TEXT("/Game/DreamStateTreeTests/Driver/DriverParent.DriverParent")))
			{
				Model::FDstTree Extracted;
				Lang::FDstDiagnosticSink Sink;
				if (Compiler::ExtractDst(*ParentAsset, Extracted, Sink))
				{
					const Model::FDstState* Link = Extracted.FindState(TEXT("Root"));
					TestTrue(*FString::Printf(TEXT("%s: Root links an asset"), *Label), Link && Link->Type == Model::EDstStateType::LinkedAsset);
					TestTrue(*FString::Printf(TEXT("%s: ...the child's"), *Label), Link && Link->LinkTarget.EndsWith(TEXT("DriverChild.DriverChild")));
				}
			}
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Library
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstDriverLibraryTest, FDstSpikeBuildTestBase, "DreamStateTree.Compiler.Driver.Library",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverLibraryTest::RunTest(const FString& Parameters)
	{
		CompilerSupport::EnsureTestTags();
		const FString Dir = MakeTestDir(TEXT("Library"));
		const FStandardSources Sources = WriteStandardSources(Dir);
		const FString Lonely = WriteSource(Dir, TEXT("Common/Lonely.dth"),
			TEXT("StateTreeLibrary(Name=\"DreamStateTreeTests/Driver/Lonely\")\n{\n    Template Fast = Owner.Health > 3;\n}\n"));

		CompilerSupport::FGeneratedAssets Assets;
		Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/Alpha"));
		Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/Beta"));
		Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/Gamma"));

		// A library stands for the trees that include it.
		{
			const TArray<FString> Files = { Sources.Shared };
			const TArray<Compiler::FDstFileResult> Results = Compiler::BuildDstFiles(Files, CompilerSupport::MakeBuildOptions(Dir, /*bGenerate*/ true));
			if (TestEqual(TEXT("Shared.dth builds the two trees that include it"), Results.Num(), 2))
			{
				TestTrue(TEXT("...Alpha"), SameFile(Results[0].FilePath, Sources.Alpha));
				TestTrue(TEXT("...and Beta"), SameFile(Results[1].FilePath, Sources.Beta));
				for (const Compiler::FDstFileResult& Result : Results)
				{
					TestFalse(*FString::Printf(TEXT("%s builds (%s)"), *FPaths::GetCleanFilename(Result.FilePath), *CompilerSupport::DescribeErrors(Result.Diagnostics)),
						Result.Diagnostics.HasErrors());
					TestTrue(*FString::Printf(TEXT("%s produced an asset"), *FPaths::GetCleanFilename(Result.FilePath)), Result.Generated.IsSet() && Result.Generated->Asset != nullptr);
				}
			}
			TestNull(TEXT("Gamma includes nothing, so it was not built"), FindObject<UStateTree>(nullptr, TEXT("/Game/DreamStateTreeTests/Driver/Gamma.Gamma")));
		}

		// A library nobody includes is checked on its own.
		{
			const TArray<FString> Files = { Lonely };
			const TArray<Compiler::FDstFileResult> Results = Compiler::BuildDstFiles(Files, CompilerSupport::MakeBuildOptions(Dir, /*bGenerate*/ true));
			if (TestEqual(TEXT("A library nobody includes gives one result"), Results.Num(), 1))
			{
				TestFalse(*FString::Printf(TEXT("...without an error (%s)"), *CompilerSupport::DescribeErrors(Results[0].Diagnostics)), Results[0].Diagnostics.HasErrors());
				TestTrue(TEXT("...checked: it was read and lowered as far as a library can be"), Results[0].Lowered.IsSet());
				TestFalse(TEXT("...and nothing was generated"), Results[0].Generated.IsSet());
			}
		}

		// A single library checked through BuildDstFile.
		{
			const Compiler::FDstFileResult Result = Compiler::BuildDstFile(Lonely, CompilerSupport::MakeBuildOptions(Dir, /*bGenerate*/ true));
			TestFalse(TEXT("BuildDstFile on a library does not generate"), Result.Generated.IsSet());
			TestTrue(TEXT("BuildDstFile on a library succeeds"), Result.Succeeded());
		}

		// A file that does not exist is a diagnostic, not a crash.
		{
			const Compiler::FDstFileResult Missing = Compiler::BuildDstFile(Dir / TEXT("Nowhere.dtr"), CompilerSupport::MakeBuildOptions(Dir, /*bGenerate*/ false));
			TestTrue(TEXT("A missing file is reported"), Missing.Diagnostics.HasErrors());
			TestNotNull(TEXT("...as DST3204"), LangSupport::FindDiagnostic(Missing.Diagnostics, TEXT("DST3204")));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Cycle
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstDriverCycleTest, FDstSpikeBuildTestBase, "DreamStateTree.Compiler.Driver.Cycle",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverCycleTest::RunTest(const FString& Parameters)
	{
		CompilerSupport::EnsureTestTags();
		const FString Dir = MakeTestDir(TEXT("Cycle"));
		const FString CycleA = WriteSource(Dir, TEXT("CycleA.dtr"),
			TEXT("StateTree(Name=\"DreamStateTreeTests/Driver/CycleA\", Schema=TestAgent)\n{\n    State Root {\n        link asset CycleB\n    }\n}\n"));
		const FString CycleB = WriteSource(Dir, TEXT("CycleB.dtr"),
			TEXT("StateTree(Name=\"DreamStateTreeTests/Driver/CycleB\", Schema=TestAgent)\n{\n    State Root {\n        link asset CycleA\n    }\n}\n"));
		const FString Fine = WriteSource(Dir, TEXT("Fine.dtr"), MakeTreeText(TEXT(""), TEXT("CycleFine")));

		CompilerSupport::FGeneratedAssets Assets;
		Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/CycleA"));
		Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/CycleB"));
		Assets.Track(TEXT("/Game/DreamStateTreeTests/Driver/CycleFine"));

		const TArray<FString> Files = { CycleA, CycleB, Fine };
		const TArray<Compiler::FDstFileResult> Results = Compiler::BuildDstFiles(Files, CompilerSupport::MakeBuildOptions(Dir, /*bGenerate*/ true));
		if (!TestEqual(TEXT("One result per file"), Results.Num(), 3))
		{
			return false;
		}
		int32 NumInCycle = 0;
		for (const Compiler::FDstFileResult& Result : Results)
		{
			const FString Name = FPaths::GetCleanFilename(Result.FilePath);
			if (Name.StartsWith(TEXT("Cycle")))
			{
				++NumInCycle;
				TestNotNull(*FString::Printf(TEXT("%s is told it is in a circle of links"), *Name), LangSupport::FindDiagnostic(Result.Diagnostics, TEXT("DST6009")));
				TestTrue(*FString::Printf(TEXT("%s fails"), *Name), Result.Diagnostics.HasErrors());
				TestFalse(*FString::Printf(TEXT("%s is not built"), *Name), Result.Generated.IsSet());
			}
			else
			{
				TestFalse(*FString::Printf(TEXT("%s, outside the circle, builds (%s)"), *Name, *CompilerSupport::DescribeErrors(Result.Diagnostics)), Result.Diagnostics.HasErrors());
				TestTrue(*FString::Printf(TEXT("%s produced an asset"), *Name), Result.Generated.IsSet() && Result.Generated->Asset != nullptr);
			}
		}
		TestEqual(TEXT("Two files are in the circle"), NumInCycle, 2);
		TestNull(TEXT("No asset was made for CycleA"), FindObject<UStateTree>(nullptr, TEXT("/Game/DreamStateTreeTests/Driver/CycleA.CycleA")));
		TestNull(TEXT("No asset was made for CycleB"), FindObject<UStateTree>(nullptr, TEXT("/Game/DreamStateTreeTests/Driver/CycleB.CycleB")));
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Format
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstDriverFormatTest, "DreamStateTree.Compiler.Driver.Format",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverFormatTest::RunTest(const FString& Parameters)
	{
		const FString FormatDir = CompilerSupport::GetCorpusSubDir(TEXT("Format"));
		const FString ParseDir = CompilerSupport::GetCorpusSubDir(TEXT("Parse"));
		if (FormatDir.IsEmpty())
		{
			AddError(TEXT("The language corpus is not there."));
			return false;
		}

		// Canonical text comes back as it is, from a file and from memory.
		for (const TCHAR* Name : { TEXT("canon_brawler.dtr"), TEXT("canon_sections.dtr"), TEXT("canon_library_villager.dth") })
		{
			const FString Path = FPaths::Combine(FormatDir, Name);
			FString Original;
			if (!LangSupport::LoadText(Path, Original))
			{
				AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
				continue;
			}
			FString FromFile;
			Lang::FDstDiagnosticSink FileSink;
			if (TestTrue(*FString::Printf(TEXT("%s formats"), Name), Compiler::FormatDstFile(Path, FromFile, FileSink)))
			{
				const FString Difference = LangSupport::DescribeDifference(Original, FromFile);
				TestTrue(*FString::Printf(TEXT("%s is already canonical (%s)"), Name, *Difference), Difference.IsEmpty());
			}
			FString FromText;
			Lang::FDstDiagnosticSink TextSink;
			if (TestTrue(*FString::Printf(TEXT("%s formats from memory"), Name), Compiler::FormatDstText(Path, Original, FromText, TextSink)))
			{
				TestTrue(*FString::Printf(TEXT("The file and the text give the same print for %s"), Name), LangSupport::DescribeDifference(FromFile, FromText).IsEmpty());
			}
		}

		// Untidy text comes out tidy, and tidy stays tidy.
		for (const TCHAR* Name : { TEXT("messy_spacing.dtr"), TEXT("messy_clause_order.dtr"), TEXT("messy_comments.dtr"), TEXT("messy_library.dth") })
		{
			const FString Path = FPaths::Combine(FormatDir, Name);
			FString Original;
			if (!LangSupport::LoadText(Path, Original))
			{
				AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
				continue;
			}
			FString Once;
			Lang::FDstDiagnosticSink Sink;
			if (!TestTrue(*FString::Printf(TEXT("%s formats"), Name), Compiler::FormatDstText(Path, Original, Once, Sink)))
			{
				continue;
			}
			FString Twice;
			Lang::FDstDiagnosticSink SinkAgain;
			if (TestTrue(*FString::Printf(TEXT("The formatted %s formats"), Name), Compiler::FormatDstText(Path, Once, Twice, SinkAgain)))
			{
				const FString Difference = LangSupport::DescribeDifference(Once, Twice);
				TestTrue(*FString::Printf(TEXT("Formatting %s twice changes nothing (%s)"), Name, *Difference), Difference.IsEmpty());
			}
		}

		// Text that does not parse is not printed: the canonical text of a broken file would silently drop what its author wrote.
		{
			const FString Path = FPaths::Combine(ParseDir, TEXT("err"), TEXT("err_dst2012_stray_in_body.dtr"));
			FString Original;
			if (LangSupport::LoadText(Path, Original))
			{
				FString Printed = TEXT("not touched");
				Lang::FDstDiagnosticSink Sink;
				TestFalse(TEXT("A text that does not parse does not format"), Compiler::FormatDstText(Path, Original, Printed, Sink));
				TestTrue(TEXT("...and prints nothing"), Printed.IsEmpty());
				TestNotNull(TEXT("...and says why (DST2012)"), LangSupport::FindDiagnostic(Sink, TEXT("DST2012")));
			}
			else
			{
				AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			}
		}

		// A file that is not there.
		{
			FString Printed;
			Lang::FDstDiagnosticSink Sink;
			TestFalse(TEXT("A file that does not exist does not format"), Compiler::FormatDstFile(FPaths::Combine(FormatDir, TEXT("nowhere.dtr")), Printed, Sink));
			TestNotNull(TEXT("...and says so (DST3204)"), LangSupport::FindDiagnostic(Sink, TEXT("DST3204")));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// DefaultPath
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstDriverDefaultPathTest, "DreamStateTree.Compiler.Driver.DefaultPath",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverDefaultPathTest::RunTest(const FString& Parameters)
	{
		const FString ProjectDtree = CompilerSupport::NormalizeDirectory(FPaths::Combine(FPaths::ProjectDir(), TEXT("DTree")));

		// A new package name every run: an asset made in an earlier run may not have been collected yet.
		static int32 RunNumber = 0;
		++RunNumber;

		struct FCase
		{
			FString Package;
			FString Expected;
		};
		const TArray<FCase> Cases = {
			{ FString::Printf(TEXT("/Game/DreamStateTreeTests/DriverPaths%d/ST_Brawler"), RunNumber), FString::Printf(TEXT("DreamStateTreeTests/DriverPaths%d/ST_Brawler.dtr"), RunNumber) },
			{ FString::Printf(TEXT("/Game/ST_TopLevel%d"), RunNumber), FString::Printf(TEXT("ST_TopLevel%d.dtr"), RunNumber) },
			{ FString::Printf(TEXT("/DreamStateTree/DriverPaths%d/ST_FromPlugin"), RunNumber), FString::Printf(TEXT("DreamStateTree/DriverPaths%d/ST_FromPlugin.dtr"), RunNumber) },
		};
		for (const FCase& Case : Cases)
		{
			UPackage* Package = CreatePackage(*Case.Package);
			TStrongObjectPtr<UStateTree> Asset(NewObject<UStateTree>(Package, FName(*FPackageName::GetShortName(Case.Package))));
			if (!TestNotNull(*FString::Printf(TEXT("An asset in %s"), *Case.Package), Asset.Get()))
			{
				continue;
			}
			const FString Path = Compiler::GetDefaultDstDecompilePath(*Asset);
			const FString Expected = ProjectDtree / Case.Expected;
			TestTrue(*FString::Printf(TEXT("%s decompiles to %s (got %s)"), *Case.Package, *Expected, *Path), SameFile(Path, Expected));
			TestTrue(*FString::Printf(TEXT("The default path of %s is a source path"), *Case.Package), Compiler::IsDstSourceFile(Path));
			TestFalse(TEXT("...and absolute"), FPaths::IsRelative(Path));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Diagnostics
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstDriverDiagnosticsTest, "DreamStateTree.Compiler.Driver.Diagnostics",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstDriverDiagnosticsTest::RunTest(const FString& Parameters)
	{
		const auto MakeSpan = [](const int32 Offset, const int32 Length, const int32 Line, const int32 Column)
		{
			Lang::FDstSpan Span;
			Span.Offset = Offset;
			Span.Length = Length;
			Span.Line = Line;
			Span.Column = Column;
			return Span;
		};

		Lang::FDstDiagnosticSink Main(TEXT("C:/Project/DTree/AI/ST_Brawler.dtr"));
		Main.Error(TEXT("DST4001"), MakeSpan(40, 7, 3, 5), TEXT("There is no task called Delayy."), TEXT("Did you mean Delay?"));
		Main.Warning(TEXT("DST4018"), MakeSpan(90, 4, 8, 2), TEXT("Echo hides another node."));
		Main.ErrorIn(TEXT("DST4001"), TEXT("C:/Project/DTree/Common/Lib.dth"), MakeSpan(0, 3, 1, 1), TEXT("Nope is not a task."));

		// The line form: file(line,col): severity code: message.
		const FString Lines = Compiler::FormatDstDiagnostics(Main);
		const FString ExpectedLines = FString::Join(TArray<FString>{
			TEXT("C:/Project/DTree/AI/ST_Brawler.dtr(3,5): error DST4001: There is no task called Delayy."),
			TEXT("C:/Project/DTree/AI/ST_Brawler.dtr(8,2): warning DST4018: Echo hides another node."),
			TEXT("C:/Project/DTree/Common/Lib.dth(1,1): error DST4001: Nope is not a task."),
		}, TEXT("\n"));
		TestEqual(TEXT("The line form"), Lines, ExpectedLines);
		TestEqual(TEXT("A sink without diagnostics prints nothing"), Compiler::FormatDstDiagnostics(Lang::FDstDiagnosticSink(TEXT("C:/x.dtr"))), FString());

		// The JSON form: one object per diagnostic, 1-based line and column, the file of the diagnostic or else of its sink.
		Lang::FDstDiagnosticSink Other(TEXT("C:/Project/DTree/AI/ST_Other.dtr"));
		Other.Error(TEXT("DST6002"), MakeSpan(12, 5, 2, 9), TEXT("No such state."));
		const TArray<const Lang::FDstDiagnosticSink*> Sinks = { &Main, &Other };
		const FString Json = Compiler::FormatDstDiagnosticsJson(Sinks);
		const FString ExpectedJson = FString::Join(TArray<FString>{
			TEXT("[{\"file\":\"C:/Project/DTree/AI/ST_Brawler.dtr\",\"line\":3,\"column\":5,\"length\":7,\"severity\":\"error\",\"code\":\"DST4001\",\"message\":\"There is no task called Delayy.\",\"hint\":\"Did you mean Delay?\"},"),
			TEXT("{\"file\":\"C:/Project/DTree/AI/ST_Brawler.dtr\",\"line\":8,\"column\":2,\"length\":4,\"severity\":\"warning\",\"code\":\"DST4018\",\"message\":\"Echo hides another node.\",\"hint\":\"\"},"),
			TEXT("{\"file\":\"C:/Project/DTree/Common/Lib.dth\",\"line\":1,\"column\":1,\"length\":3,\"severity\":\"error\",\"code\":\"DST4001\",\"message\":\"Nope is not a task.\",\"hint\":\"\"},"),
			TEXT("{\"file\":\"C:/Project/DTree/AI/ST_Other.dtr\",\"line\":2,\"column\":9,\"length\":5,\"severity\":\"error\",\"code\":\"DST6002\",\"message\":\"No such state.\",\"hint\":\"\"}]"),
		}, TEXT(""));
		TestEqual(TEXT("The JSON form"), Json, ExpectedJson);

		TestEqual(TEXT("No sinks give an empty array"), Compiler::FormatDstDiagnosticsJson(TConstArrayView<const Lang::FDstDiagnosticSink*>()), FString(TEXT("[]")));
		return true;
	}
}
