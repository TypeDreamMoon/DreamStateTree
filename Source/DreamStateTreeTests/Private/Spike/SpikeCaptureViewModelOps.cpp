// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureHelpers.h"
#include "Spike/SpikeCaptureOps.h"
#include "Spike/SpikeDump.h"

#include "Misc/AutomationTest.h"
#include "StateTree.h"
#include "StateTreeViewModel.h"

// Replays what the state-tree outliner does through FStateTreeViewModel (add, rename, delete, copy and paste, move,
// node and transition changes) against a small real asset, and records which engine hooks an outside observer gets
// for each one, next to what changed in the editor data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstSpikeCaptureViewModelOpsTest, "DreamStateTree.Spike.Capture.ViewModelOps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeCaptureViewModelOpsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstCaptureFixture Fixture;
	FString Error;
	if (!Fixture.Initialize(TEXT("ViewModelOps"), Error))
	{
		AddError(FString::Printf(TEXT("Could not set up the asset: %s"), *Error));
		return false;
	}

	// Found or created through the editing subsystem, as the editor does when it opens the asset.
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

	// The operations run one after the other on the same tree (destructive ones last), the way a session would.
	TArray<FDstOpRecord> Records;
	for (const FDstCaptureOp& Op : MakeViewModelOps())
	{
		const FDstOpRecord Record = DstRunOp(Fixture, Recorder, Op);
		Records.Add(Record);

		const FString Text = DstFormatOpRecord(Record, Recorder);
		AddInfo(Text);
		Report += Text + TEXT("\n\n");

		DstQuietCompile(Fixture, Recorder);
	}

	// Digest: how far the hooks go on their own.
	int32 RanCount = 0;
	TArray<FString> NoEngineHook;
	TArray<FString> NoTransacted;
	TArray<FString> NoHash;
	for (const FDstOpRecord& Record : Records)
	{
		if (!Record.bRan)
		{
			continue;
		}
		++RanCount;
		const bool bAnyPropertyEvent = Recorder.CountEvents(Record.Segment, TEXT("PostEditChain")) > 0
			|| Recorder.CountEvents(Record.Segment, TEXT("PostEdit")) > 0;
		const bool bTransacted = Recorder.CountEvents(Record.Segment, TEXT("Transacted")) > 0;
		if (!bAnyPropertyEvent && !bTransacted)
		{
			NoEngineHook.Add(Record.Name);
		}
		if (!bTransacted)
		{
			NoTransacted.Add(Record.Name);
		}
		if (!Record.bHashChanged)
		{
			NoHash.Add(Record.Name);
		}
	}

	const FString Matrix = DstFormatCoverageMatrix(Records, Recorder);
	AddInfo(TEXT("Hook coverage per operation (counts of events; Hash = engine editor-data hash changed, Dirty = tree marked dirty):\n") + Matrix);

	FString Digest;
	Digest += FString::Printf(TEXT("Operations run: %d of %d\n"), RanCount, Records.Num());
	Digest += FString::Printf(TEXT("Operations with no property-change event and no transaction event (only view model delegates or nothing): %s\n"),
		NoEngineHook.IsEmpty() ? TEXT("none") : *FString::Join(NoEngineHook, TEXT("; ")));
	Digest += FString::Printf(TEXT("Operations with no OnObjectTransacted event: %s\n"),
		NoTransacted.IsEmpty() ? TEXT("none") : *FString::Join(NoTransacted, TEXT("; ")));
	Digest += FString::Printf(TEXT("Operations the engine's own editor-data hash did not notice: %s\n"),
		NoHash.IsEmpty() ? TEXT("none") : *FString::Join(NoHash, TEXT("; ")));
	AddInfo(Digest);

	// A compile after the whole session shows what the post-compile hooks deliver.
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("Compile after the session"));
		FString Messages;
		const bool bCompiled = Fixture.Compile(Messages);
		const FString CompileText = FString::Printf(TEXT("[Compile] succeeded=%s\n  events:\n%s%s"),
			bCompiled ? TEXT("yes") : TEXT("no"), *Recorder.SummarizeSegment(Segment),
			Messages.IsEmpty() ? TEXT("") : *(TEXT("\n  compiler log:\n") + Messages));
		AddInfo(CompileText);
		Report += CompileText + TEXT("\n\n");
	}

	Report += TEXT("Coverage matrix:\n") + Matrix + TEXT("\n") + Digest + TEXT("\n");
	Report += TEXT("Full event log:\n") + Recorder.FormatAll();
	Recorder.Stop();

	AddInfo(FString::Printf(TEXT("Full report: %s"), *SaveReport(TEXT("Capture"), TEXT("ViewModelOps"), Report)));
	return true;
}
