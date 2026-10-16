// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeBuildHelpers.h"
#include "Spike/SpikeDump.h"

#include "StateTreeEditorData.h"
#include "UObject/UObjectGlobals.h"

// Builds a tree that uses every authoring feature the text language will have to produce, compiles it, and reports
// what the compiler said and what it changed. Then builds the same tree twice and duplicates it, to see which IDs
// stay put.

namespace UE::DreamStateTree::Spike
{
	namespace BuildAllFeaturesPrivate
	{
		int32 CountLines(const FString& Text)
		{
			if (Text.IsEmpty())
			{
				return 0;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
			return Lines.Num();
		}

		/** The first line where the two texts differ, "A: ... | B: ...", or empty when they are equal. */
		FString FirstDifferenceOrEmpty(const FString& A, const FString& B)
		{
			TArray<FString> LinesA;
			TArray<FString> LinesB;
			A.ParseIntoArrayLines(LinesA, /*bCullEmpty*/ false);
			B.ParseIntoArrayLines(LinesB, /*bCullEmpty*/ false);
			const int32 Num = FMath::Max(LinesA.Num(), LinesB.Num());
			for (int32 Index = 0; Index < Num; ++Index)
			{
				const FString LineA = LinesA.IsValidIndex(Index) ? LinesA[Index] : FString(TEXT("<end>"));
				const FString LineB = LinesB.IsValidIndex(Index) ? LinesB[Index] : FString(TEXT("<end>"));
				if (LineA != LineB)
				{
					return FString::Printf(TEXT("line %d: A: %s | B: %s"), Index + 1, *LineA, *LineB);
				}
			}
			return FString();
		}

		/** The kind of object an ID belongs to, from its label, for the duplicate statistics. */
		FString IdCategoryOf(const FString& Label)
		{
			if (Label == TEXT("RootParametersGuid"))
			{
				return TEXT("root parameters struct");
			}
			if (Label.StartsWith(TEXT("RootParam/")) || Label.Contains(TEXT("/Param/")))
			{
				return TEXT("parameter property");
			}
			if (Label.StartsWith(TEXT("Binding:")))
			{
				return TEXT("property function node");
			}
			if (Label.Contains(TEXT("/Condition:")))
			{
				return TEXT("transition condition");
			}
			if (Label.Contains(TEXT("/Transition:")))
			{
				return TEXT("transition");
			}
			if (Label.Contains(TEXT("/Parameters")))
			{
				return TEXT("state parameters struct");
			}
			if (Label.Contains(TEXT("/Task:")) || Label.Contains(TEXT("/EnterCondition:")) || Label.Contains(TEXT("/Consideration:")) || Label.Contains(TEXT("/SingleTask"))
				|| Label.StartsWith(TEXT("Global/")))
			{
				return TEXT("node");
			}
			return TEXT("state");
		}
	}
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstSpikeBuild_AllFeatures, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Build.AllFeatures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeBuild_AllFeatures::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstBuildAssetKeeper Keeper;
	FDstBuildLogCapture LogCapture;
	FDstBuildReport Report;

	UStateTree* Tree = Keeper.NewTransientTree(TEXT("AllFeatures"), UDstTestSchema::StaticClass());
	if (!TestNotNull(TEXT("The tree is created"), Tree))
	{
		return false;
	}

	FDstBuildTreeBuilder Builder(*Tree);
	const FDstBuildAllFeaturesInfo Info = DstBuildAllFeaturesTree(Builder);
	UStateTreeEditorData& EditorData = Builder.GetEditorData();

	Report.Heading(TEXT("What the tree contains"));
	Report.Lines(Info.Features);
	Report.Line(TEXT("Size: ") + Builder.DescribeSize());

	// Every ID must be valid and unique, except the parameter properties a linked state copies from its subtree.
	{
		const TArray<FDstBuildIdEntry> Ids = DstBuildCollectIds(EditorData);
		TMap<FGuid, FString> Seen;
		int32 NumInvalid = 0;
		int32 NumDuplicate = 0;
		for (const FDstBuildIdEntry& Entry : Ids)
		{
			if (!Entry.Id.IsValid())
			{
				++NumInvalid;
				AddError(FString::Printf(TEXT("ID of '%s' is invalid"), *Entry.Label));
				continue;
			}
			if (const FString* Other = Seen.Find(Entry.Id))
			{
				const bool bExpected = Entry.Label.Contains(TEXT("/Param/")) && Other->Contains(TEXT("/Param/"));
				if (!bExpected)
				{
					++NumDuplicate;
					AddError(FString::Printf(TEXT("'%s' and '%s' got the same ID"), **Other, *Entry.Label));
				}
			}
			else
			{
				Seen.Add(Entry.Id, Entry.Label);
			}
		}
		Report.Line(FString::Printf(TEXT("IDs collected: %d, invalid: %d, unexpected duplicates: %d"), Ids.Num(), NumInvalid, NumDuplicate));
	}

	const FString EditorBefore = UE::DreamStateTree::Spike::DumpEditorData(EditorData);
	const FDstBuildCompileOutcome Outcome = DstBuildCompile(*Tree);
	const FString EditorAfter = UE::DreamStateTree::Spike::DumpEditorData(EditorData);
	const FString Compiled = UE::DreamStateTree::Spike::DumpCompiled(*Tree);
	const FString EditorDiff = UE::DreamStateTree::Spike::DiffLines(EditorBefore, EditorAfter);

	// A second compile of an unchanged tree: does it produce the same runtime data?
	const FDstBuildCompileOutcome SecondOutcome = DstBuildCompile(*Tree);
	const FString CompiledAgain = UE::DreamStateTree::Spike::DumpCompiled(*Tree);
	const FString CompiledDiff = UE::DreamStateTree::Spike::DiffLines(Compiled, CompiledAgain);

	Report.Heading(TEXT("Compile"));
	Report.Line(Outcome.ToString());
	Report.Heading(TEXT("Second compile of the unchanged tree"));
	Report.Line(SecondOutcome.Summary());
	Report.Block(TEXT("Compiled data that differs between the two compiles"), CompiledDiff);
	Report.Heading(TEXT("Engine log during build and compile"));
	Report.Lines(LogCapture.GetLines());
	Report.Heading(TEXT("Editor data changed by the compile (before -> after)"));
	Report.Line(EditorDiff.IsEmpty() ? FString(TEXT("The compile did not change the editor data.")) : EditorDiff);

	if (!Outcome.bSucceeded)
	{
		AddError(FString::Printf(TEXT("The all-features tree did not compile: %s"), *Outcome.FirstError()));
	}
	for (const FString& Line : Outcome.Lines)
	{
		if (Line.StartsWith(TEXT("[Error]")) || Line.StartsWith(TEXT("[CriticalError]")))
		{
			AddError(Line);
		}
		else if (Line.StartsWith(TEXT("[Warning]")) || Line.StartsWith(TEXT("[PerformanceWarning]")))
		{
			AddInfo(FString::Printf(TEXT("Compiler warning: %s"), *Line));
		}
	}
	if (Outcome.bSucceeded && !Tree->IsReadyToRun())
	{
		AddError(TEXT("The tree compiled but is not ready to run"));
	}

	AddInfo(FString::Printf(TEXT("All features: %s; compile: %s"), *Builder.DescribeSize(), *Outcome.Summary()));
	AddInfo(FString::Printf(TEXT("Editor data lines changed by the compile: %d"), UE::DreamStateTree::Spike::BuildAllFeaturesPrivate::CountLines(EditorDiff)));
	AddInfo(FString::Printf(TEXT("Compiled data lines that differ on a second compile: %d"), UE::DreamStateTree::Spike::BuildAllFeaturesPrivate::CountLines(CompiledDiff)));
	AddInfo(FString::Printf(TEXT("Features covered: %d"), Info.Features.Num()));

	SaveReport(TEXT("Build"), TEXT("AllFeatures.EditorBefore"), EditorBefore);
	SaveReport(TEXT("Build"), TEXT("AllFeatures.EditorAfter"), EditorAfter);
	SaveReport(TEXT("Build"), TEXT("AllFeatures.Compiled"), Compiled);
	const FString ReportPath = SaveReport(TEXT("Build"), TEXT("AllFeatures"), Report.ToString());
	AddInfo(FString::Printf(TEXT("Report: %s"), *ReportPath));
	return true;
}

//----------------------------------------------------------------------//

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstSpikeBuild_Determinism, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Build.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeBuild_Determinism::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstBuildAssetKeeper Keeper;
	FDstBuildLogCapture LogCapture;
	FDstBuildReport Report;

	UStateTree* TreeA = Keeper.NewTransientTree(TEXT("DeterminismA"), UDstTestSchema::StaticClass());
	UStateTree* TreeB = Keeper.NewTransientTree(TEXT("DeterminismB"), UDstTestSchema::StaticClass());
	if (!TestNotNull(TEXT("Both trees are created"), TreeA) || !TestNotNull(TEXT("Both trees are created"), TreeB))
	{
		return false;
	}

	FDstBuildTreeBuilder BuilderA(*TreeA);
	FDstBuildTreeBuilder BuilderB(*TreeB);
	DstBuildAllFeaturesTree(BuilderA);
	DstBuildAllFeaturesTree(BuilderB);
	UStateTreeEditorData& EditorA = BuilderA.GetEditorData();
	UStateTreeEditorData& EditorB = BuilderB.GetEditorData();

	// 1. Same build code, same editor data, IDs included.
	UE::DreamStateTree::Spike::FDumpOptions WithIds;
	const FString DumpA = UE::DreamStateTree::Spike::DumpEditorData(EditorA, WithIds);
	const FString DumpB = UE::DreamStateTree::Spike::DumpEditorData(EditorB, WithIds);
	const FString EditorDifference = UE::DreamStateTree::Spike::BuildAllFeaturesPrivate::FirstDifferenceOrEmpty(DumpA, DumpB);
	Report.Heading(TEXT("Editor data of two trees built by the same code"));
	Report.Line(EditorDifference.IsEmpty() ? FString(TEXT("Identical, IDs included.")) : EditorDifference);
	if (!EditorDifference.IsEmpty())
	{
		AddError(FString::Printf(TEXT("Two trees built by the same code differ in their editor data: %s"), *EditorDifference));
		Report.Block(TEXT("Differing lines"), UE::DreamStateTree::Spike::DiffLines(DumpA, DumpB));
	}

	// The ID lists must match entry by entry.
	{
		const TArray<FDstBuildIdEntry> IdsA = DstBuildCollectIds(EditorA);
		const TArray<FDstBuildIdEntry> IdsB = DstBuildCollectIds(EditorB);
		int32 NumDifferent = 0;
		if (IdsA.Num() != IdsB.Num())
		{
			AddError(FString::Printf(TEXT("The two trees hold %d and %d IDs"), IdsA.Num(), IdsB.Num()));
		}
		for (int32 Index = 0; Index < FMath::Min(IdsA.Num(), IdsB.Num()); ++Index)
		{
			if (IdsA[Index].Label != IdsB[Index].Label || IdsA[Index].Id != IdsB[Index].Id)
			{
				++NumDifferent;
				Report.Line(FString::Printf(TEXT("ID differs: %s vs %s"), *IdsA[Index].Label, *IdsB[Index].Label));
			}
		}
		if (NumDifferent > 0)
		{
			AddError(FString::Printf(TEXT("%d of %d IDs differ between two builds of the same tree"), NumDifferent, IdsA.Num()));
		}
		Report.Line(FString::Printf(TEXT("IDs compared: %d, different: %d"), IdsA.Num(), NumDifferent));
	}

	// 2. Same compiled data.
	const FDstBuildCompileOutcome OutcomeA = DstBuildCompile(*TreeA);
	const FDstBuildCompileOutcome OutcomeB = DstBuildCompile(*TreeB);
	Report.Heading(TEXT("Compile"));
	Report.Line(TEXT("A: ") + OutcomeA.Summary());
	Report.Line(TEXT("B: ") + OutcomeB.Summary());
	if (!OutcomeA.bSucceeded || !OutcomeB.bSucceeded)
	{
		AddError(TEXT("The trees did not compile, so the compiled data cannot be compared"));
	}
	else
	{
		const FString CompiledA = UE::DreamStateTree::Spike::DumpCompiled(*TreeA);
		const FString CompiledB = UE::DreamStateTree::Spike::DumpCompiled(*TreeB);
		const FString CompiledDifference = UE::DreamStateTree::Spike::BuildAllFeaturesPrivate::FirstDifferenceOrEmpty(CompiledA, CompiledB);
		Report.Line(CompiledDifference.IsEmpty() ? FString(TEXT("Compiled data identical.")) : CompiledDifference);
		if (!CompiledDifference.IsEmpty())
		{
			AddError(FString::Printf(TEXT("Two trees built by the same code compile to different data: %s"), *CompiledDifference));
			Report.Block(TEXT("Differing compiled lines"), UE::DreamStateTree::Spike::DiffLines(CompiledA, CompiledB));
		}
		AddInfo(FString::Printf(TEXT("Two builds of the same tree: editor data %s, compiled data %s"),
			EditorDifference.IsEmpty() ? TEXT("identical") : TEXT("DIFFERENT"), CompiledDifference.IsEmpty() ? TEXT("identical") : TEXT("DIFFERENT")));
	}

	// 3. A duplicate gets fresh IDs from the engine (UStateTreeEditorData::PostDuplicate). Count which.
	// The engine's DuplicateIDs registers a state's event ID twice (for the state and for its "Enter Event" struct) and
	// ensures on every state that requires an event; an ensure fails the run. By default a tree without required events
	// is duplicated. Pass -DstSpikeEngineEnsures to duplicate the full tree and see what becomes of the bindings on
	// event payloads (the event IDs are derived from the state and transition IDs, so a fresh random ID cannot match).
	const bool bFullTree = FParse::Param(FCommandLine::Get(), TEXT("DstSpikeEngineEnsures"));
	UStateTree* Source = TreeA;
	UStateTreeEditorData* SourceEditor = &EditorA;
	if (!bFullTree)
	{
		UStateTree* TreeC = Keeper.NewTransientTree(TEXT("DeterminismWander"), UDstTestSchema::StaticClass());
		if (!TestNotNull(TEXT("The tree without events is created"), TreeC))
		{
			return false;
		}
		FDstBuildTreeBuilder BuilderC(*TreeC);
		DstBuildWanderTree(BuilderC);
		const FDstBuildCompileOutcome OutcomeC = DstBuildCompile(*TreeC);
		Report.Line(TEXT("Tree without required events (duplicated below): ") + OutcomeC.Summary());
		Source = TreeC;
		SourceEditor = &BuilderC.GetEditorData();
	}
	AddInfo(bFullTree
		? FString(TEXT("Duplicating the full tree, required events included (-DstSpikeEngineEnsures)"))
		: FString(TEXT("Duplicating a tree without required events; -DstSpikeEngineEnsures duplicates the full tree")));
	const TArray<FDstBuildIdEntry> OriginalIds = DstBuildCollectIds(*SourceEditor);
	const FName DuplicateName = MakeUniqueObjectName(GetTransientPackage(), UStateTree::StaticClass(), TEXT("DeterminismDuplicate"));
	UStateTree* Duplicate = DuplicateObject<UStateTree>(Source, GetTransientPackage(), DuplicateName);
	if (!TestNotNull(TEXT("The duplicate is created"), Duplicate))
	{
		return false;
	}
	Keeper.Keep(Duplicate);
	UStateTreeEditorData* DuplicateEditor = Cast<UStateTreeEditorData>(Duplicate->EditorData);
	if (!TestNotNull(TEXT("The duplicate has editor data"), DuplicateEditor))
	{
		return false;
	}

	const TArray<FDstBuildIdEntry> DuplicateIds = DstBuildCollectIds(*DuplicateEditor);
	Report.Heading(TEXT("Duplicate (DuplicateObject)"));
	if (DuplicateIds.Num() != OriginalIds.Num())
	{
		Report.Line(FString::Printf(TEXT("The duplicate holds %d IDs, the original %d"), DuplicateIds.Num(), OriginalIds.Num()));
	}
	TMap<FString, TPair<int32, int32>> PerCategory; // category -> (changed, total)
	int32 NumChanged = 0;
	const int32 NumCompared = FMath::Min(OriginalIds.Num(), DuplicateIds.Num());
	for (int32 Index = 0; Index < NumCompared; ++Index)
	{
		const FString Category = UE::DreamStateTree::Spike::BuildAllFeaturesPrivate::IdCategoryOf(OriginalIds[Index].Label);
		TPair<int32, int32>& Counts = PerCategory.FindOrAdd(Category);
		++Counts.Value;
		if (OriginalIds[Index].Label != DuplicateIds[Index].Label || OriginalIds[Index].Id != DuplicateIds[Index].Id)
		{
			++Counts.Key;
			++NumChanged;
		}
	}
	TArray<FString> CategoryLines;
	for (const TPair<FString, TPair<int32, int32>>& Pair : PerCategory)
	{
		CategoryLines.Add(FString::Printf(TEXT("%s: %d of %d changed"), *Pair.Key, Pair.Value.Key, Pair.Value.Value));
	}
	CategoryLines.Sort();
	Report.Lines(CategoryLines);
	Report.Line(FString::Printf(TEXT("Total: %d of %d IDs differ from the original"), NumChanged, NumCompared));
	AddInfo(FString::Printf(TEXT("Duplicate of a tree: %d of %d IDs differ from the original (%s)"), NumChanged, NumCompared, *FString::Join(CategoryLines, TEXT("; "))));

	// Does the duplicate still compile, i.e. were the bindings remapped together with the IDs?
	const FDstBuildCompileOutcome DuplicateOutcome = DstBuildCompile(*Duplicate);
	Report.Line(TEXT("Compile of the duplicate: ") + DuplicateOutcome.Summary());
	AddInfo(FString::Printf(TEXT("The duplicate compiles: %s"), *DuplicateOutcome.Summary()));
	if (!DuplicateOutcome.bSucceeded)
	{
		Report.Lines(DuplicateOutcome.Lines);
	}

	// Parameters whose IDs were not regenerated would let two trees share a binding source by accident.
	{
		const FString DuplicateDumpNoIds = UE::DreamStateTree::Spike::DumpEditorData(*DuplicateEditor, UE::DreamStateTree::Spike::FDumpOptions{false, false});
		const FString OriginalDumpNoIds = UE::DreamStateTree::Spike::DumpEditorData(*SourceEditor, UE::DreamStateTree::Spike::FDumpOptions{false, false});
		const FString StructuralDifference = UE::DreamStateTree::Spike::BuildAllFeaturesPrivate::FirstDifferenceOrEmpty(OriginalDumpNoIds, DuplicateDumpNoIds);
		Report.Line(StructuralDifference.IsEmpty() ? FString(TEXT("Without IDs the duplicate's editor data equals the original's.")) : StructuralDifference);
		AddInfo(FString::Printf(TEXT("Duplicate without IDs equals the original: %s"), StructuralDifference.IsEmpty() ? TEXT("yes") : TEXT("NO")));
	}

	Report.Heading(TEXT("Engine log"));
	Report.Lines(LogCapture.GetLines());
	const FString ReportPath = SaveReport(TEXT("Build"), TEXT("Determinism"), Report.ToString());
	AddInfo(FString::Printf(TEXT("Report: %s"), *ReportPath));
	return true;
}
