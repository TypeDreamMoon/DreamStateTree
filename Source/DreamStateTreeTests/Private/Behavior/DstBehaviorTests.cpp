// Copyright DreamMoon. All Rights Reserved.
//
// Behavior tests. A tree of the corpus that says how to run it is built, started without a world and ticked a fixed number of
// times with a seeded random stream, and what happened is written down: the states that were active after every step, the
// transitions the context recorded, and what the fixture nodes logged. Two tests take each such tree:
//
//   Run       The trace is the same as `<file>.run` (and the same on a second run with the same seed).
//   Rebuilt   The generated asset is decompiled, the text is built into another asset, and that asset runs to the same trace:
//             the text the decompiler writes means what the asset does.
//
// A tree opts in with directives in comments, anywhere in its file:
//
//   // RUN ticks=18 dt=0.1 seed=12345        number of ticks, seconds per tick, seed of the random stream
//   // OWNER Level=3 Health=80 Mode=Combat Alerted=true
//                                            values of the context object (UDstTestOwner); all optional
//   // SEND 6 DreamStateTreeTest.Event.Hit.Light Amount=7 Severity=0.8 Source=Origin Critical=true
//                                            before tick 6, send this event with this payload (FDstTestPayload); `nopayload`
//                                            instead of the values sends it without
//
// Every node that logs is named in its tree, so the log says who wrote the line and the rebuilt tree, whose nodes keep their
// names, logs the same.

#include "Compiler/DstCompilerTestSupport.h"

#include "Fixtures/DstTestSchema.h"
#include "Fixtures/DstTestTypes.h"
#include "GameplayTagsManager.h"
#include "Spike/SpikeBuildHelpers.h"
#include "StateTreeExecutionContext.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::DreamStateTree::Tests::BehaviorTests
{
	namespace
	{
		struct FSendSpec
		{
			int32 Tick = 0;
			FString Tag;
			bool bPayload = true;
			FDstTestPayload Payload;
		};

		struct FRunSpec
		{
			bool bHasRun = false;
			int32 Ticks = 10;
			float DeltaTime = 0.1f;
			int32 Seed = 1;
			int32 Level = 1;
			float Health = 100.f;
			EDstTestMode Mode = EDstTestMode::Patrol;
			bool bAlerted = false;
			TArray<FSendSpec> Sends;
			/** What was wrong with the directives, if anything. */
			TArray<FString> Problems;
		};

		/** `Key=Value` words of a directive line. */
		TMap<FString, FString> ReadPairs(const TArray<FString>& Words, const int32 FirstIndex)
		{
			TMap<FString, FString> Pairs;
			for (int32 Index = FirstIndex; Index < Words.Num(); ++Index)
			{
				FString Key;
				FString Value;
				if (Words[Index].Split(TEXT("="), &Key, &Value))
				{
					Pairs.Add(Key, Value);
				}
			}
			return Pairs;
		}

		FRunSpec ReadRunSpec(const FString& Text)
		{
			FRunSpec Spec;
			const TArray<FString> RunLines = CompilerSupport::ReadDirectives(Text, TEXT("RUN"));
			Spec.bHasRun = !RunLines.IsEmpty();
			if (!Spec.bHasRun)
			{
				return Spec;
			}

			{
				TArray<FString> Words;
				RunLines[0].ParseIntoArrayWS(Words);
				const TMap<FString, FString> Pairs = ReadPairs(Words, 0);
				if (const FString* Value = Pairs.Find(TEXT("ticks"))) { Spec.Ticks = FCString::Atoi(**Value); }
				if (const FString* Value = Pairs.Find(TEXT("dt"))) { Spec.DeltaTime = FCString::Atof(**Value); }
				if (const FString* Value = Pairs.Find(TEXT("seed"))) { Spec.Seed = FCString::Atoi(**Value); }
			}

			for (const FString& Line : CompilerSupport::ReadDirectives(Text, TEXT("OWNER")))
			{
				TArray<FString> Words;
				Line.ParseIntoArrayWS(Words);
				const TMap<FString, FString> Pairs = ReadPairs(Words, 0);
				if (const FString* Value = Pairs.Find(TEXT("Level"))) { Spec.Level = FCString::Atoi(**Value); }
				if (const FString* Value = Pairs.Find(TEXT("Health"))) { Spec.Health = FCString::Atof(**Value); }
				if (const FString* Value = Pairs.Find(TEXT("Alerted"))) { Spec.bAlerted = Value->Equals(TEXT("true"), ESearchCase::IgnoreCase); }
				if (const FString* Value = Pairs.Find(TEXT("Mode")))
				{
					const int64 EnumValue = StaticEnum<EDstTestMode>()->GetValueByNameString(*Value);
					if (EnumValue == INDEX_NONE)
					{
						Spec.Problems.Add(FString::Printf(TEXT("OWNER: `%s` is not a value of EDstTestMode."), **Value));
					}
					else
					{
						Spec.Mode = static_cast<EDstTestMode>(EnumValue);
					}
				}
			}

			for (const FString& Line : CompilerSupport::ReadDirectives(Text, TEXT("SEND")))
			{
				TArray<FString> Words;
				Line.ParseIntoArrayWS(Words);
				if (Words.Num() < 2 || !Words[0].IsNumeric())
				{
					Spec.Problems.Add(FString::Printf(TEXT("SEND: expected `<tick> <tag> [values]`, got `%s`."), *Line));
					continue;
				}
				FSendSpec Send;
				Send.Tick = FCString::Atoi(*Words[0]);
				Send.Tag = Words[1];
				Send.bPayload = !Words.Contains(TEXT("nopayload"));
				const TMap<FString, FString> Pairs = ReadPairs(Words, 2);
				if (const FString* Value = Pairs.Find(TEXT("Amount"))) { Send.Payload.Amount = FCString::Atoi(**Value); }
				if (const FString* Value = Pairs.Find(TEXT("Severity"))) { Send.Payload.Severity = FCString::Atof(**Value); }
				if (const FString* Value = Pairs.Find(TEXT("Source"))) { Send.Payload.Source = FName(**Value); }
				if (const FString* Value = Pairs.Find(TEXT("Critical"))) { Send.Payload.bCritical = Value->Equals(TEXT("true"), ESearchCase::IgnoreCase); }
				Spec.Sends.Add(MoveTemp(Send));
			}
			return Spec;
		}

		/** The trace of one run: the spec it ran with, then the steps, the recorded transitions and the log. */
		FString FormatRun(const FRunSpec& Spec, const Spike::FDstBuildRunResult& Run)
		{
			FString Text;
			Text += FString::Printf(TEXT("run ticks=%d dt=%g seed=%d\n"), Spec.Ticks, Spec.DeltaTime, Spec.Seed);
			Text += FString::Printf(TEXT("owner Level=%d Health=%g Mode=%s Alerted=%s\n"), Spec.Level, Spec.Health,
				*StaticEnum<EDstTestMode>()->GetNameStringByValue(static_cast<int64>(Spec.Mode)), Spec.bAlerted ? TEXT("true") : TEXT("false"));
			for (const FSendSpec& Send : Spec.Sends)
			{
				Text += FString::Printf(TEXT("send tick=%d %s%s\n"), Send.Tick, *Send.Tag,
					Send.bPayload ? *FString::Printf(TEXT(" Amount=%d Severity=%g Source=%s Critical=%s"), Send.Payload.Amount, Send.Payload.Severity,
						*Send.Payload.Source.ToString(), Send.Payload.bCritical ? TEXT("true") : TEXT("false")) : TEXT(" nopayload"));
			}
			Text += FString::Printf(TEXT("started=%s\n"), Run.bStarted ? TEXT("yes") : TEXT("no"));
			Text += TEXT("--- steps\n");
			for (const FString& Line : Run.StepLines)
			{
				Text += Line;
				Text += TEXT("\n");
			}
			Text += TEXT("--- recorded transitions\n");
			for (const FString& Line : Run.RecordedTransitions)
			{
				Text += Line;
				Text += TEXT("\n");
			}
			Text += TEXT("--- log\n");
			Text += Run.Log;
			return Text;
		}

		/** Runs a compiled tree as the spec says. */
		Spike::FDstBuildRunResult RunTree(UStateTree& Tree, const FRunSpec& Spec)
		{
			TStrongObjectPtr<UDstTestOwner> Owner(NewObject<UDstTestOwner>(GetTransientPackage()));
			Owner->Level = Spec.Level;
			Owner->Health = Spec.Health;
			Owner->Mode = Spec.Mode;
			Owner->bAlerted = Spec.bAlerted;

			Spike::FDstBuildRunOptions Options;
			Options.RandomSeed = Spec.Seed;
			Options.NumTicks = Spec.Ticks;
			Options.DeltaTime = Spec.DeltaTime;
			Options.BeforeTick = [&Spec](FStateTreeExecutionContext& Context, const int32 TickIndex)
			{
				for (const FSendSpec& Send : Spec.Sends)
				{
					if (Send.Tick != TickIndex)
					{
						continue;
					}
					const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*Send.Tag), /*ErrorIfNotFound*/ false);
					if (Send.bPayload)
					{
						Context.SendEvent(Tag, FConstStructView::Make(Send.Payload), FName(TEXT("Corpus")));
					}
					else
					{
						Context.SendEvent(Tag, FConstStructView(), FName(TEXT("Corpus")));
					}
				}
			};
			return Spike::DstBuildRunTree(Tree, *Owner, Options);
		}

		/** The trees of the corpus that have a `// RUN` line. */
		void ListRunCases(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
		{
			TArray<FString> Files;
			CompilerSupport::CollectCaseFiles(CompilerSupport::GetCorpusSubDir(TEXT("Trees")), /*bTrees*/ true, /*bLibraries*/ false, TConstArrayView<FString>(), Files);
			const FString Root = CompilerSupport::GetCorpusSubDir(TEXT("Trees"));
			for (const FString& File : Files)
			{
				FString Text;
				if (LangSupport::LoadText(File, Text) && !CompilerSupport::ReadDirectives(Text, TEXT("RUN")).IsEmpty())
				{
					FString Relative = File.StartsWith(Root) ? File.Mid(Root.Len()) : FPaths::GetCleanFilename(File);
					Relative.RemoveFromStart(TEXT("/"));
					OutBeautifiedNames.Add(Relative);
					OutTestCommands.Add(File);
				}
			}
			if (OutBeautifiedNames.IsEmpty())
			{
				OutBeautifiedNames.Add(TEXT("corpus_not_found"));
				OutTestCommands.Add(FString());
			}
		}

		/** Generates the case into its asset and checks it can run. */
		bool BuildRunnable(FAutomationTestBase& Test, const FString& Path, const FString& AssetPath, const FString& Text, CompilerSupport::FGeneratedAssets& Assets,
			UStateTree*& OutAsset)
		{
			const FString CaseDir = FPaths::GetPath(Path);
			TSet<FString> Done;
			if (!CompilerSupport::BuildRequiredTrees(Test, CaseDir, Text, Assets, Done))
			{
				return false;
			}
			Assets.Track(AssetPath);
			Compiler::FDstFileResult Result = Compiler::BuildDstText(Path, Text, CompilerSupport::MakeBuildOptions(CaseDir, /*bGenerate*/ true, AssetPath));
			if (Result.Diagnostics.HasErrors() || !Result.Generated.IsSet() || !Result.Generated->Asset)
			{
				Test.AddError(FString::Printf(TEXT("The tree does not build: %s"), *CompilerSupport::DescribeErrors(Result.Diagnostics)));
				return false;
			}
			OutAsset = Result.Generated->Asset;
			if (!OutAsset->IsReadyToRun())
			{
				Test.AddError(TEXT("The generated tree is not ready to run."));
				return false;
			}
			return true;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Run
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FDstBehaviorRunTest, FDstSpikeBuildTestBase, "DreamStateTree.Behavior.Run",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstBehaviorRunTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		ListRunCases(OutBeautifiedNames, OutTestCommands);
	}

	bool FDstBehaviorRunTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No tree with a `// RUN` line was found under %s/Trees."), *LangSupport::GetCorpusDir()));
			return false;
		}

		FString Text;
		if (!LangSupport::LoadText(Path, Text))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}
		const FRunSpec Spec = ReadRunSpec(Text);
		for (const FString& Problem : Spec.Problems)
		{
			AddError(Problem);
		}
		if (!Spec.Problems.IsEmpty())
		{
			return false;
		}

		CompilerSupport::EnsureTestTags();
		CompilerSupport::FGeneratedAssets Assets;
		UStateTree* Asset = nullptr;
		const FString Name = FPaths::GetBaseFilename(Path);
		if (!BuildRunnable(*this, Path, CompilerSupport::GetGeneratedPackagePath(Name), Text, Assets, Asset))
		{
			return false;
		}

		const Spike::FDstBuildRunResult First = RunTree(*Asset, Spec);
		if (!First.bStarted)
		{
			AddError(FString::Printf(TEXT("The tree did not start: %s"), First.StepLines.IsEmpty() ? TEXT("(no step)") : *First.StepLines[0]));
		}
		const Spike::FDstBuildRunResult Second = RunTree(*Asset, Spec);
		const FString FirstTrace = FormatRun(Spec, First);
		CompilerSupport::CheckSameText(*this, TEXT("Two runs with the same seed differ (first on the left)"), TEXT("Behavior"), Name + TEXT(".twice"),
			FirstTrace, FormatRun(Spec, Second));

		CompilerSupport::CheckGoldenInFolder(*this, Path + TEXT(".run"), FirstTrace);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Rebuilt
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_CUSTOM_COMPLEX_AUTOMATION_TEST(FDstBehaviorRebuiltTest, FDstSpikeBuildTestBase, "DreamStateTree.Behavior.Rebuilt",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDstBehaviorRebuiltTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		ListRunCases(OutBeautifiedNames, OutTestCommands);
	}

	bool FDstBehaviorRebuiltTest::RunTest(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			AddError(FString::Printf(TEXT("No tree with a `// RUN` line was found under %s/Trees."), *LangSupport::GetCorpusDir()));
			return false;
		}

		FString Text;
		if (!LangSupport::LoadText(Path, Text))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}
		const FRunSpec Spec = ReadRunSpec(Text);
		if (!Spec.Problems.IsEmpty())
		{
			for (const FString& Problem : Spec.Problems)
			{
				AddError(Problem);
			}
			return false;
		}

		CompilerSupport::EnsureTestTags();
		CompilerSupport::FGeneratedAssets Assets;
		UStateTree* Original = nullptr;
		const FString Name = FPaths::GetBaseFilename(Path);
		if (!BuildRunnable(*this, Path, CompilerSupport::GetGeneratedPackagePath(Name), Text, Assets, Original))
		{
			return false;
		}

		// The asset as text, and the text as another asset.
		FString Decompiled;
		Lang::FDstDiagnosticSink DecompileSink;
		if (!Compiler::DecompileDstAsset(*Original, Compiler::FDstDecompileOptions(), Decompiled, DecompileSink))
		{
			AddError(FString::Printf(TEXT("The generated tree cannot be written as text: %s"), *CompilerSupport::DescribeErrors(DecompileSink)));
			return false;
		}
		const FString TextPath = CompilerSupport::WriteReport(TEXT("Behavior"), Name + TEXT(".rebuilt.dtr"), Decompiled);

		const FString RebuiltAssetPath = CompilerSupport::GetGeneratedPackagePath(Name + TEXT("_Rebuilt"));
		UStateTree* Rebuilt = nullptr;
		const FString RebuiltTextPath = FPaths::Combine(CompilerSupport::GetReportDir(TEXT("Behavior")), Name + TEXT(".rebuilt.dtr"));
		{
			// Built by hand, not by BuildRunnable: the required trees are there already, and the text is not the corpus file.
			Assets.Track(RebuiltAssetPath);
			Compiler::FDstFileResult Result = Compiler::BuildDstText(RebuiltTextPath, Decompiled,
				CompilerSupport::MakeBuildOptions(FPaths::GetPath(Path), /*bGenerate*/ true, RebuiltAssetPath));
			if (Result.Diagnostics.HasErrors() || !Result.Generated.IsSet() || !Result.Generated->Asset)
			{
				AddError(FString::Printf(TEXT("The decompiled text does not build (%s): %s"), *TextPath, *CompilerSupport::DescribeErrors(Result.Diagnostics)));
				return false;
			}
			Rebuilt = Result.Generated->Asset;
			if (!Rebuilt->IsReadyToRun())
			{
				AddError(FString::Printf(TEXT("The rebuilt tree is not ready to run (%s)."), *TextPath));
				return false;
			}
		}

		const FString OriginalTrace = FormatRun(Spec, RunTree(*Original, Spec));
		const FString RebuiltTrace = FormatRun(Spec, RunTree(*Rebuilt, Spec));
		CompilerSupport::CheckSameText(*this, FString::Printf(TEXT("The tree rebuilt from its decompiled text runs differently (original on the left, text in %s)"), *TextPath),
			TEXT("Behavior"), Name, OriginalTrace, RebuiltTrace);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Coverage
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstBehaviorCoverageTest, "DreamStateTree.Behavior.Coverage",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstBehaviorCoverageTest::RunTest(const FString& Parameters)
	{
		TArray<FString> Names;
		TArray<FString> Commands;
		ListRunCases(Names, Commands);
		if (Commands.Num() == 1 && Commands[0].IsEmpty())
		{
			AddError(TEXT("No tree of the corpus says how to run it."));
			return false;
		}
		TestTrue(TEXT("At least eight trees of the corpus run"), Commands.Num() >= 8);

		// Every directive is understood.
		for (const FString& Path : Commands)
		{
			FString Text;
			if (LangSupport::LoadText(Path, Text))
			{
				for (const FString& Problem : ReadRunSpec(Text).Problems)
				{
					AddError(FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(Path), *Problem));
				}
			}
		}
		return true;
	}
}
