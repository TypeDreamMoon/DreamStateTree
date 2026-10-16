// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureHelpers.h"
#include "Spike/SpikeCaptureOps.h"
#include "Spike/SpikeDump.h"

#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "StateTree.h"
#include "StateTreeViewModel.h"

// For every operation of the other two experiments: make it, undo it, redo it, and record which hooks fire during
// the undo and the redo, and whether the editor data really goes back to what it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstSpikeCaptureUndoRedoTest, "DreamStateTree.Spike.Capture.UndoRedo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeCaptureUndoRedoTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	if (!GEditor || !GEditor->Trans)
	{
		AddError(TEXT("The editor has no transaction buffer, so undo and redo cannot be exercised."));
		return false;
	}

	FDstCaptureFixture Fixture;
	FString Error;
	if (!Fixture.Initialize(TEXT("UndoRedo"), Error))
	{
		AddError(FString::Printf(TEXT("Could not set up the asset: %s"), *Error));
		return false;
	}

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
	DstQuietCompile(Fixture, Recorder);

	TArray<FDstCaptureOp> Ops = MakeViewModelOps();
	Ops.Append(MakeDetailsOps());

	FString Report;
	Report += TEXT("Each operation starts from the baseline: do, undo, redo, undo.\n\n");

	int32 UndoRestoredCount = 0;
	int32 RedoRestoredCount = 0;
	int32 TestedCount = 0;
	TArray<FString> UndoNotFaithful;
	TArray<FString> RedoNotFaithful;
	TArray<FString> NotUndoable;
	TArray<FString> BaselineDrift;
	TArray<FString> UndoWithoutUndoRedoEvent;
	TArray<FString> UndoWithoutViewModelRefresh;

	for (const FDstCaptureOp& Op : Ops)
	{
		const FDstOpRecord Record = DstRunOp(Fixture, Recorder, Op);
		if (!Record.bRan)
		{
			const FString Text = FString::Printf(TEXT("[%s] NOT RUN: %s"), *Record.Name, *Record.Note);
			AddInfo(Text);
			Report += Text + TEXT("\n\n");
			DstQuietCompile(Fixture, Recorder);
			continue;
		}
		if (Record.NewTransactions <= 0)
		{
			NotUndoable.Add(Record.Name);
			const FString Text = FString::Printf(TEXT("[%s] made %d undo records: nothing to undo (the operation is invisible to the transaction buffer)\n%s"),
				*Record.Name, Record.NewTransactions, *Recorder.SummarizeSegment(Record.Segment));
			AddInfo(Text);
			Report += Text + TEXT("\n\n");
			DstQuietCompile(Fixture, Recorder);
			continue;
		}

		++TestedCount;
		const FDstUndoRedoRecord UndoRedo = DstRunUndoRedo(Fixture, Recorder, Record);
		UndoRestoredCount += UndoRedo.bUndoRestoredData ? 1 : 0;
		RedoRestoredCount += UndoRedo.bRedoRestoredData ? 1 : 0;
		if (!UndoRedo.bUndoRestoredData)
		{
			UndoNotFaithful.Add(Record.Name);
		}
		if (!UndoRedo.bRedoRestoredData)
		{
			RedoNotFaithful.Add(Record.Name);
		}
		if (!UndoRedo.bBackAtBaseline)
		{
			BaselineDrift.Add(Record.Name);
		}
		// The undo event the engine sends for objects restored by undo, and the refresh the outliner is told to do.
		const bool bUndoRedoEvent = Recorder.HasEventMentioning(UndoRedo.UndoSegment, TEXT("Transacted"), TEXT("Type=UndoRedo"));
		if (!bUndoRedoEvent)
		{
			UndoWithoutUndoRedoEvent.Add(Record.Name);
		}
		const bool bViewModelRefresh = Recorder.CountEvents(UndoRedo.UndoSegment, TEXT("VM.AssetChanged")) > 0;
		if (!bViewModelRefresh)
		{
			UndoWithoutViewModelRefresh.Add(Record.Name);
		}

		FString Text = FString::Printf(TEXT("[%s] undo-records=%d | undo: returned-true=%s data-restored=%s | redo: returned-true=%s data-restored=%s | back-at-baseline=%s"),
			*Record.Name, Record.NewTransactions,
			UndoRedo.bUndoReturnedTrue ? TEXT("yes") : TEXT("no"), UndoRedo.bUndoRestoredData ? TEXT("yes") : TEXT("no"),
			UndoRedo.bRedoReturnedTrue ? TEXT("yes") : TEXT("no"), UndoRedo.bRedoRestoredData ? TEXT("yes") : TEXT("no"),
			UndoRedo.bBackAtBaseline ? TEXT("yes") : TEXT("no"));
		Text += TEXT("\n  original operation events:\n") + Recorder.SummarizeSegment(Record.Segment);
		Text += TEXT("\n  undo events:\n") + Recorder.SummarizeSegment(UndoRedo.UndoSegment);
		Text += TEXT("\n  redo events:\n") + Recorder.SummarizeSegment(UndoRedo.RedoSegment);
		if (!UndoRedo.UndoDiff.IsEmpty())
		{
			Text += TEXT("\n  data differs from the pre-operation state after undo (only-before / only-after):\n") + UndoRedo.UndoDiff;
		}
		if (!UndoRedo.RedoDiff.IsEmpty())
		{
			Text += TEXT("\n  data differs from the post-operation state after redo (only-expected / only-actual):\n") + UndoRedo.RedoDiff;
		}
		AddInfo(Text);
		Report += Text + TEXT("\n\n");

		DstQuietCompile(Fixture, Recorder);
	}

	FString Digest;
	Digest += FString::Printf(TEXT("Operations undone and redone: %d\n"), TestedCount);
	Digest += FString::Printf(TEXT("Undo restored the editor data exactly: %d of %d\n"), UndoRestoredCount, TestedCount);
	Digest += FString::Printf(TEXT("Redo restored the post-operation data exactly: %d of %d\n"), RedoRestoredCount, TestedCount);
	Digest += FString::Printf(TEXT("Operations whose undo did not restore the data: %s\n"), UndoNotFaithful.IsEmpty() ? TEXT("none") : *FString::Join(UndoNotFaithful, TEXT("; ")));
	Digest += FString::Printf(TEXT("Operations whose redo did not restore the data: %s\n"), RedoNotFaithful.IsEmpty() ? TEXT("none") : *FString::Join(RedoNotFaithful, TEXT("; ")));
	Digest += FString::Printf(TEXT("Operations that left no undo record at all: %s\n"), NotUndoable.IsEmpty() ? TEXT("none") : *FString::Join(NotUndoable, TEXT("; ")));
	Digest += FString::Printf(TEXT("Operations after which the final undo did not return to the baseline: %s\n"), BaselineDrift.IsEmpty() ? TEXT("none") : *FString::Join(BaselineDrift, TEXT("; ")));
	Digest += FString::Printf(TEXT("Undos with no OnObjectTransacted event of type UndoRedo: %s\n"), UndoWithoutUndoRedoEvent.IsEmpty() ? TEXT("none") : *FString::Join(UndoWithoutUndoRedoEvent, TEXT("; ")));
	Digest += FString::Printf(TEXT("Undos where the view model did not announce an asset refresh: %s\n"), UndoWithoutViewModelRefresh.IsEmpty() ? TEXT("none") : *FString::Join(UndoWithoutViewModelRefresh, TEXT("; ")));
	AddInfo(Digest);

	Report += Digest + TEXT("\n");
	Report += TEXT("Full event log:\n") + Recorder.FormatAll();
	Recorder.Stop();

	AddInfo(FString::Printf(TEXT("Full report: %s"), *SaveReport(TEXT("Capture"), TEXT("UndoRedo"), Report)));
	return true;
}
