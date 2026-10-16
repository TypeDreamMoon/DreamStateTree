// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureHelpers.h"
#include "Spike/SpikeCaptureOps.h"
#include "Spike/SpikeDump.h"

#include "Misc/AutomationTest.h"
#include "StateTree.h"
#include "StateTreeViewModel.h"

// Replays the edits the Details panel makes (scoped transaction, Modify, PreEditChange with a property chain, change,
// PostEditChangeChainProperty) for the values a text round trip has to follow, and records the hooks that fire.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstSpikeCaptureDetailsEditsTest, "DreamStateTree.Spike.Capture.DetailsEdits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeCaptureDetailsEditsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstCaptureFixture Fixture;
	FString Error;
	if (!Fixture.Initialize(TEXT("DetailsEdits"), Error))
	{
		AddError(FString::Printf(TEXT("Could not set up the asset: %s"), *Error));
		return false;
	}

	// An open editor always has a view model; some state callbacks consult it, so it exists here too.
	const TSharedPtr<FStateTreeViewModel> ViewModel = Fixture.GetOrCreateViewModel();
	if (!ViewModel.IsValid())
	{
		AddError(TEXT("No view model could be created for the asset."));
		return false;
	}

	FDstCaptureRecorder Recorder;
	Recorder.Track(Fixture.GetTree());
	Recorder.Start();
	Recorder.AttachViewModel(ViewModel);

	FString Report;
	Report += TEXT("Baseline editor data (IDs hidden):\n") + Fixture.Dump(/*bIncludeIds*/ false) + TEXT("\n\n");
	DstQuietCompile(Fixture, Recorder);

	TArray<FDstOpRecord> Records;
	for (const FDstCaptureOp& Op : MakeDetailsOps())
	{
		const FDstOpRecord Record = DstRunOp(Fixture, Recorder, Op);
		Records.Add(Record);

		const FString Text = DstFormatOpRecord(Record, Recorder);
		AddInfo(Text);
		Report += Text + TEXT("\n\n");

		DstQuietCompile(Fixture, Recorder);
	}

	// Which edits announce themselves with a StateTree-level hook, which only through generic engine hooks.
	TArray<FString> OnlyGeneric;
	TArray<FString> NoPropertyNames;
	for (const FDstOpRecord& Record : Records)
	{
		if (!Record.bRan)
		{
			continue;
		}
		bool bAnyStateTreeHook = false;
		bool bAnyTransacted = false;
		bool bTransactedWithNames = false;
		for (const FDstCaptureEvent& Event : Recorder.GetSegmentEvents(Record.Segment))
		{
			if (Event.Kind.StartsWith(TEXT("ST.")))
			{
				bAnyStateTreeHook = true;
			}
			if (Event.Kind == TEXT("Transacted"))
			{
				bAnyTransacted = true;
				bTransactedWithNames |= !Event.Detail.Contains(TEXT("Props=[]"));
			}
		}
		if (!bAnyStateTreeHook)
		{
			OnlyGeneric.Add(Record.Name);
		}
		if (bAnyTransacted && !bTransactedWithNames)
		{
			NoPropertyNames.Add(Record.Name);
		}
	}

	const FString Matrix = DstFormatCoverageMatrix(Records, Recorder);
	AddInfo(TEXT("Hook coverage per edit (counts of events; Hash = engine editor-data hash changed, Dirty = tree marked dirty):\n") + Matrix);

	FString Digest;
	Digest += FString::Printf(TEXT("Edits that raised no StateTree-level delegate (only generic engine hooks): %s\n"),
		OnlyGeneric.IsEmpty() ? TEXT("none") : *FString::Join(OnlyGeneric, TEXT("; ")));
	Digest += FString::Printf(TEXT("Edits whose Transacted event carried no changed-property names: %s\n"),
		NoPropertyNames.IsEmpty() ? TEXT("none") : *FString::Join(NoPropertyNames, TEXT("; ")));
	AddInfo(Digest);

	// Compile once more and see what the post-compile hooks give and whether the compiler touches the editor data.
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("Compile after the edits"));
		const FString DumpBefore = Fixture.Dump();
		const uint32 HashBefore = Fixture.EditorDataHash();
		FString Messages;
		const bool bCompiled = Fixture.Compile(Messages);
		const bool bDumpChanged = DumpBefore != Fixture.Dump();
		const FString CompileText = FString::Printf(TEXT("[Compile] succeeded=%s editor-data-changed-by-compile=%s hash-before=%08x hash-after=%08x\n  events:\n%s%s"),
			bCompiled ? TEXT("yes") : TEXT("no"), bDumpChanged ? TEXT("yes") : TEXT("no"), HashBefore, Fixture.EditorDataHash(),
			*Recorder.SummarizeSegment(Segment), Messages.IsEmpty() ? TEXT("") : *(TEXT("\n  compiler log:\n") + Messages));
		AddInfo(CompileText);
		Report += CompileText + TEXT("\n\n");
	}

	Report += TEXT("Coverage matrix:\n") + Matrix + TEXT("\n") + Digest + TEXT("\n");
	Report += TEXT("Full event log:\n") + Recorder.FormatAll();
	Recorder.Stop();

	AddInfo(FString::Printf(TEXT("Full report: %s"), *SaveReport(TEXT("Capture"), TEXT("DetailsEdits"), Report)));
	return true;
}
