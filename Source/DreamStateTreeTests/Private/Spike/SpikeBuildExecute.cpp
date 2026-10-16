// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeBuildHelpers.h"
#include "Spike/SpikeDump.h"

#include "Fixtures/DstTestExecutionLog.h"
#include "Fixtures/DstTestTags.h"
#include "StateTreeExecutionContext.h"

// Runs compiled trees without a world: an execution context, an owner object, ticks, events. Checks that the states
// follow the expected path, that the nodes see their bindings, and that one seed gives one run.

namespace UE::DreamStateTree::Spike
{
	namespace BuildExecutePrivate
	{
		/** The first line where two traces differ, or empty when they are equal. */
		FString FirstTraceDifference(const FString& A, const FString& B)
		{
			TArray<FString> LinesA;
			TArray<FString> LinesB;
			A.ParseIntoArrayLines(LinesA, /*bCullEmpty*/ false);
			B.ParseIntoArrayLines(LinesB, /*bCullEmpty*/ false);
			const int32 Num = FMath::Max(LinesA.Num(), LinesB.Num());
			for (int32 Index = 0; Index < Num; ++Index)
			{
				const FString LineA = LinesA.IsValidIndex(Index) ? LinesA[Index] : FString(TEXT("<missing>"));
				const FString LineB = LinesB.IsValidIndex(Index) ? LinesB[Index] : FString(TEXT("<missing>"));
				if (LineA != LineB)
				{
					return FString::Printf(TEXT("step %d: [%s] vs [%s]"), Index, *LineA, *LineB);
				}
			}
			return FString();
		}

		/** True if some line of the log starts with the prefix and contains the text. */
		bool LogLineHas(const FString& Log, const FString& Prefix, const FString& Text)
		{
			TArray<FString> Lines;
			Log.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(Prefix) && Line.Contains(Text))
				{
					return true;
				}
			}
			return false;
		}

		/** Position of the item in the list at or after StartIndex, or INDEX_NONE. */
		int32 FindFrom(const TArray<FString>& List, const FString& Item, const int32 StartIndex)
		{
			for (int32 Index = StartIndex; Index < List.Num(); ++Index)
			{
				if (List[Index] == Item)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		/** The leg numbers the Wander tree entered, in order, from the execution log of a run. */
		FString LegSequence(const FString& Log)
		{
			TArray<FString> Lines;
			Log.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
			TArray<FString> Legs;
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(TEXT("LegTask")) && Line.Contains(TEXT(": Enter:")))
				{
					Legs.Add(Line.Mid(FString(TEXT("LegTask")).Len(), 1));
				}
			}
			return FString::Join(Legs, TEXT(","));
		}
	}
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstSpikeBuild_Execute, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Build.Execute",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeBuild_Execute::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;
	using namespace UE::DreamStateTree::Tests;

	FDstBuildAssetKeeper Keeper;
	FDstBuildLogCapture LogCapture;
	FDstBuildReport Report;
	UDstTestOwner* Owner = Keeper.NewOwner();
	Owner->Level = 3;

	//----------------------------------------------------------------------//
	// A. The encounter tree: Idle and Patrol alternate, a Hit event starts a fight that ends back in Idle.
	//----------------------------------------------------------------------//
	UStateTree* Encounter = Keeper.NewTransientTree(TEXT("Encounter"), UDstTestSchema::StaticClass());
	{
		FDstBuildTreeBuilder Builder(*Encounter);
		DstBuildEncounterTree(Builder);
	}
	const FDstBuildCompileOutcome EncounterOutcome = DstBuildCompile(*Encounter);
	Report.Heading(TEXT("A. Encounter tree"));
	Report.Line(TEXT("Compile: ") + EncounterOutcome.ToString());
	if (!EncounterOutcome.bSucceeded || !Encounter->IsReadyToRun())
	{
		AddError(FString::Printf(TEXT("The encounter tree cannot run: %s"), *EncounterOutcome.ToString()));
		SaveReport(TEXT("Build"), TEXT("Execute"), Report.ToString());
		return false;
	}

	FDstBuildRunOptions EncounterOptions;
	EncounterOptions.RandomSeed = 12345;
	EncounterOptions.NumTicks = 18;
	EncounterOptions.DeltaTime = 0.1f;
	EncounterOptions.BeforeTick = [](FStateTreeExecutionContext& Context, const int32 TickIndex)
	{
		FDstTestPayload Payload;
		if (TickIndex == 6)
		{
			Payload.Amount = 7;
			Payload.Severity = 0.8f;
			Context.SendEvent(Tag_Event_Hit_Light.GetTag(), FConstStructView::Make(Payload), TEXT("Spike"));
		}
		else if (TickIndex == 12)
		{
			Payload.Amount = 9;
			Payload.Severity = 0.4f;
			Context.SendEvent(Tag_Event_Hit_Heavy.GetTag(), FConstStructView::Make(Payload), TEXT("Spike"));
		}
	};

	LogCapture.Reset();
	const FDstBuildRunResult FirstRun = DstBuildRunTree(*Encounter, *Owner, EncounterOptions);
	const FDstBuildRunResult SecondRun = DstBuildRunTree(*Encounter, *Owner, EncounterOptions);

	Report.Heading(TEXT("A. First run (seed 12345)"));
	Report.Lines(FirstRun.StepLines, TEXT(""));
	Report.Heading(TEXT("A. Recorded transitions of the first run"));
	Report.Lines(FirstRun.RecordedTransitions);
	Report.Heading(TEXT("A. Execution log of the first run"));
	Report.Line(FirstRun.Log);

	if (!TestTrue(TEXT("The encounter tree starts"), FirstRun.bStarted))
	{
		Report.Heading(TEXT("Engine log"));
		Report.Lines(LogCapture.GetLines());
		SaveReport(TEXT("Build"), TEXT("Execute"), Report.ToString());
		return false;
	}

	// The states the tree went through. Exactly which tick a completion transition fires on is the engine's
	// business; the order of the visited states is the contract.
	const TArray<FString> Sequence = DstBuildCompressSequence(FirstRun.ActiveStates);
	Report.Heading(TEXT("A. Visited states in order"));
	Report.Lines(Sequence);

	AddInfo(FString::Printf(TEXT("[Execute] Encounter visited: %s"), *FString::Join(Sequence, TEXT(" > "))));
	TestTrue(TEXT("The tree starts in Root/Idle"), Sequence.Num() > 0 && Sequence[0] == TEXT("Root/Idle"));

	const int32 PatrolIndex = UE::DreamStateTree::Spike::BuildExecutePrivate::FindFrom(Sequence, TEXT("Root/Patrol"), 0);
	TestTrue(TEXT("Idle completes into Patrol"), PatrolIndex != INDEX_NONE);
	const int32 LightIndex = UE::DreamStateTree::Spike::BuildExecutePrivate::FindFrom(Sequence, TEXT("Root/Fight/Light"), 0);
	TestTrue(TEXT("A Hit.Light event enters Fight/Light"), LightIndex != INDEX_NONE);
	const int32 IdleAfterLight = LightIndex == INDEX_NONE ? INDEX_NONE : UE::DreamStateTree::Spike::BuildExecutePrivate::FindFrom(Sequence, TEXT("Root/Idle"), LightIndex + 1);
	TestTrue(TEXT("Fight/Light completes back into Idle"), IdleAfterLight != INDEX_NONE);
	const int32 HeavyIndex = IdleAfterLight == INDEX_NONE ? INDEX_NONE : UE::DreamStateTree::Spike::BuildExecutePrivate::FindFrom(Sequence, TEXT("Root/Fight/Heavy"), IdleAfterLight + 1);
	TestTrue(TEXT("A Hit.Heavy event enters Fight/Heavy after the first fight"), HeavyIndex != INDEX_NONE);

	// The nodes saw their bindings: the root parameter reached the Input, the event payload reached the task, the
	// context object was bound, the evaluator ticked.
	TestTrue(TEXT("The Light task read Payload.Amount = 7 through its binding"), UE::DreamStateTree::Spike::BuildExecutePrivate::LogLineHas(FirstRun.Log, TEXT("LightTask: Enter:"), TEXT("Amount=7")));
	TestTrue(TEXT("The Heavy task read Payload.Amount = 9 through its binding"), UE::DreamStateTree::Spike::BuildExecutePrivate::LogLineHas(FirstRun.Log, TEXT("HeavyTask: Enter:"), TEXT("Amount=9")));
	TestTrue(TEXT("The Idle task read the root parameter Speed = 4 into its Input"), UE::DreamStateTree::Spike::BuildExecutePrivate::LogLineHas(FirstRun.Log, TEXT("IdleTask: Enter:"), TEXT("Input=4")));
	TestTrue(TEXT("The task saw the context object"), UE::DreamStateTree::Spike::BuildExecutePrivate::LogLineHas(FirstRun.Log, TEXT("IdleTask: Enter:"), TEXT("Owner(Level=3)")));
	TestTrue(TEXT("The evaluator ticked"), FirstRun.Log.Contains(TEXT("Counter: Tick:Counter=5")));
	AddInfo(FString::Printf(TEXT("[Execute] Recorded transitions: %d (%s)"), FirstRun.RecordedTransitions.Num(), *FString::Join(FirstRun.RecordedTransitions, TEXT(" | "))));

	// One seed, one run.
	const FString TraceDifference = UE::DreamStateTree::Spike::BuildExecutePrivate::FirstTraceDifference(FirstRun.Trace, SecondRun.Trace);
	TestTrue(TEXT("Two runs with the same seed are identical, step by step"), TraceDifference.IsEmpty());
	Report.Heading(TEXT("A. Second run with the same seed"));
	Report.Line(TraceDifference.IsEmpty() ? FString(TEXT("Identical to the first run.")) : TraceDifference);
	if (!TraceDifference.IsEmpty())
	{
		Report.Lines(SecondRun.StepLines, TEXT(""));
		AddError(FString::Printf(TEXT("Same seed, different run: %s"), *TraceDifference));
	}
	AddInfo(FString::Printf(TEXT("[Execute] Same seed twice: %s"), TraceDifference.IsEmpty() ? TEXT("identical traces") : TEXT("DIFFERENT traces")));

	//----------------------------------------------------------------------//
	// B. The random selector: the seed decides which child is picked.
	//----------------------------------------------------------------------//
	UStateTree* Wander = Keeper.NewTransientTree(TEXT("Wander"), UDstTestSchema::StaticClass());
	{
		FDstBuildTreeBuilder Builder(*Wander);
		DstBuildWanderTree(Builder);
	}
	const FDstBuildCompileOutcome WanderOutcome = DstBuildCompile(*Wander);
	Report.Heading(TEXT("B. Wander tree"));
	Report.Line(TEXT("Compile: ") + WanderOutcome.ToString());
	if (!WanderOutcome.bSucceeded || !Wander->IsReadyToRun())
	{
		AddError(FString::Printf(TEXT("The wander tree cannot run: %s"), *WanderOutcome.ToString()));
	}
	else
	{
		TSet<FString> DistinctSequences;
		int32 MinimumPicks = TNumericLimits<int32>::Max();
		for (int32 Seed = 1; Seed <= 6; ++Seed)
		{
			FDstBuildRunOptions Options;
			Options.RandomSeed = Seed;
			Options.NumTicks = 20;
			Options.DeltaTime = 0.1f;
			const FDstBuildRunResult RunA = DstBuildRunTree(*Wander, *Owner, Options);
			const FDstBuildRunResult RunB = DstBuildRunTree(*Wander, *Owner, Options);

			const FString Difference = UE::DreamStateTree::Spike::BuildExecutePrivate::FirstTraceDifference(RunA.Trace, RunB.Trace);
			const FString Legs = UE::DreamStateTree::Spike::BuildExecutePrivate::LegSequence(RunA.Log);
			TArray<FString> LegList;
			Legs.ParseIntoArray(LegList, TEXT(","));
			MinimumPicks = FMath::Min(MinimumPicks, LegList.Num());
			DistinctSequences.Add(Legs);

			Report.Line(FString::Printf(TEXT("seed %d: legs entered = [%s]; second run %s"), Seed, *Legs, Difference.IsEmpty() ? TEXT("identical") : *Difference));
			if (!Difference.IsEmpty())
			{
				AddError(FString::Printf(TEXT("Wander with seed %d: two runs differ: %s"), Seed, *Difference));
			}
		}
		TestTrue(TEXT("The random selector picked a child several times in 20 ticks"), MinimumPicks >= 4);
		AddInfo(FString::Printf(TEXT("[Execute] Random selector: %d distinct leg sequences for seeds 1-6; fewest picks in a run: %d"), DistinctSequences.Num(), MinimumPicks));
		if (DistinctSequences.Num() < 2)
		{
			AddWarning(TEXT("[Execute] Six different seeds gave the same random selection; the seed may have no effect on state selection"));
		}
	}

	Report.Heading(TEXT("Engine log"));
	Report.Lines(LogCapture.GetLines());
	const FString ReportPath = SaveReport(TEXT("Build"), TEXT("Execute"), Report.ToString());
	AddInfo(FString::Printf(TEXT("Report: %s"), *ReportPath));
	return true;
}
