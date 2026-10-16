// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureHelpers.h"
#include "Spike/SpikeCaptureOps.h"
#include "Spike/SpikeDump.h"

#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "StateTree.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeViewModel.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectGlobals.h"

// A write-back changes the editor data from outside while the asset may be open in the state tree editor. This
// checks, without any UI, how the open editor's model is told: the view model's refresh entry points, what a compile
// triggers by itself, and what happens when the editor data object is replaced instead of changed in place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstSpikeCaptureExternalChangeTest, "DreamStateTree.Spike.Capture.ExternalChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeCaptureExternalChangeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	if (!GEditor)
	{
		AddError(TEXT("GEditor is null: the test needs the editor."));
		return false;
	}

	FDstCaptureFixture Fixture;
	FString Error;
	if (!Fixture.Initialize(TEXT("ExternalChange"), Error))
	{
		AddError(FString::Printf(TEXT("Could not set up the asset: %s"), *Error));
		return false;
	}

	UStateTree* Tree = Fixture.GetTree();
	UStateTreeEditorData* EditorData = Fixture.GetEditorData();
	UStateTreeState* A = Fixture.FindState(Fixture.Ids.A);
	FStateTreeEditorNode* ATask = Fixture.FindNode(Fixture.Ids.A, Fixture.Ids.ATask);
	if (!Tree || !EditorData || !A || !ATask)
	{
		AddError(TEXT("The baseline asset is missing parts the experiment needs."));
		return false;
	}

	FString Report;
	auto Log = [this, &Report](const FString& Text)
	{
		AddInfo(Text);
		Report += Text + TEXT("\n\n");
	};

	// ---- The editing subsystem and its view model ----------------------------------------------------------------
	UStateTreeEditingSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStateTreeEditingSubsystem>();
	Log(FString::Printf(TEXT("UStateTreeEditingSubsystem instantiated in this editor session: %s"), Subsystem ? TEXT("yes") : TEXT("NO (view models are then built by hand in these tests)")));
	if (Subsystem)
	{
		Log(FString::Printf(TEXT("FindViewModel before any editor opened the asset: %s"), Subsystem->FindViewModel(Tree).IsValid() ? TEXT("a view model exists") : TEXT("none (created on demand by FindOrAddViewModel)")));
	}

	const TSharedPtr<FStateTreeViewModel> ViewModel = Fixture.GetOrCreateViewModel();
	if (!ViewModel.IsValid())
	{
		AddError(TEXT("No view model could be created for the asset."));
		return false;
	}
	if (Subsystem)
	{
		const TSharedPtr<FStateTreeViewModel> Found = Subsystem->FindViewModel(Tree);
		const TSharedRef<FStateTreeViewModel> Again = Subsystem->FindOrAddViewModel(Tree);
		Log(FString::Printf(TEXT("After FindOrAddViewModel: FindViewModel finds it=%s, a second FindOrAddViewModel returns the same object=%s"),
			Found.IsValid() ? TEXT("yes") : TEXT("no"), (Again == ViewModel.ToSharedRef()) ? TEXT("yes") : TEXT("no")));
	}

	FDstCaptureRecorder Recorder;
	Recorder.Track(Tree);
	Recorder.Start();
	Recorder.AttachViewModel(ViewModel);
	DstQuietCompile(Fixture, Recorder);

	// ---- The two entry points meant for outside callers -----------------------------------------------------------
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("NotifyAssetChangedExternally"));
		ViewModel->NotifyAssetChangedExternally();
		Log(FString::Printf(TEXT("[NotifyAssetChangedExternally] callable without a UI, no crash. Events:\n%s"), *Recorder.SummarizeSegment(Segment)));
	}
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("NotifyStatesChangedExternally"));
		TSet<UStateTreeState*> ChangedStates;
		ChangedStates.Add(A);
		FPropertyChangedEvent NameChanged(DstFindProperty(UStateTreeState::StaticClass(), TEXT("Name")), EPropertyChangeType::ValueSet);
		ViewModel->NotifyStatesChangedExternally(ChangedStates, NameChanged);
		Log(FString::Printf(TEXT("[NotifyStatesChangedExternally] callable without a UI, no crash. Events:\n%s"), *Recorder.SummarizeSegment(Segment)));
	}

	// ---- An in-place change with no UI call, the way a write-back would make it -----------------------------------
	const FString DumpBeforeEdit = Fixture.Dump();
	const uint32 HashBeforeEdit = Fixture.EditorDataHash();
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("In-place edit by the write-back (no transaction, no property events)"));
		A->Name = FName(TEXT("A_Written"));
		A->AddChildState(FName(TEXT("Fresh")));
		DstSetFloat(ATask->Instance.GetScriptStruct(), ATask->Instance.GetMutableMemory(), TEXT("Duration"), 9.0f);
		UStateTreeEditingSubsystem::MarkAsPubliclyModified(Tree);
		Tree->MarkPackageDirty();

		const UStateTreeState* SeenByViewModel = ViewModel->GetStateByID(Fixture.Ids.A);
		Log(FString::Printf(TEXT("[In-place edit] events raised by the edit itself:\n%s\n  view model reads the new name right away (it holds no copy): %s\n  engine hash changed: %s, tree dirty: %s, needs recompile: %s, package dirty: %s\n  data change:\n%s"),
			*Recorder.SummarizeSegment(Segment),
			(SeenByViewModel && SeenByViewModel->Name == FName(TEXT("A_Written"))) ? TEXT("yes") : TEXT("no"),
			HashBeforeEdit != Fixture.EditorDataHash() ? TEXT("yes") : TEXT("no"),
			Tree->IsEditorDataDirty() ? TEXT("yes") : TEXT("no"),
			UStateTreeEditingSubsystem::NeedsRecompile(Tree) ? TEXT("yes") : TEXT("no"),
			Fixture.GetPackage() && Fixture.GetPackage()->IsDirty() ? TEXT("yes") : TEXT("no"),
			*DiffLines(DumpBeforeEdit, Fixture.Dump(), 8)));
	}
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("NotifyAssetChangedExternally after the in-place edit"));
		ViewModel->NotifyAssetChangedExternally();
		Log(FString::Printf(TEXT("[Refresh after the edit] Events:\n%s"), *Recorder.SummarizeSegment(Segment)));
	}

	// ---- Does a compile refresh an open editor by itself? ---------------------------------------------------------
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("Compile after the in-place edit"));
		FString Messages;
		const bool bCompiled = Fixture.Compile(Messages);
		const int32 Refreshes = Recorder.CountEvents(Segment, TEXT("VM.AssetChanged"));
		Log(FString::Printf(TEXT("[Compile] succeeded=%s. The view model announced an asset refresh by itself %d time(s): the post-compile hook calls NotifyAssetChangedExternally, so compiling after a write-back also refreshes an open editor.\n%s%s"),
			bCompiled ? TEXT("yes") : TEXT("no"), Refreshes, *Recorder.SummarizeSegment(Segment), Messages.IsEmpty() ? TEXT("") : *(TEXT("\n  compiler log:\n") + Messages)));
	}
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("ValidateStateTree after the edit"));
		UStateTreeEditingSubsystem::ValidateStateTree(Tree);
		Log(FString::Printf(TEXT("[ValidateStateTree] Events (this is the normalisation the editor runs when it opens an asset):\n%s"), *Recorder.SummarizeSegment(Segment)));
	}

	// ---- Replacing the editor data object instead of changing it ---------------------------------------------------
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("Replace the editor data object"));
		UStateTreeEditorData* OldEditorData = EditorData;
		UStateTreeEditorData* NewEditorData = DuplicateObject<UStateTreeEditorData>(OldEditorData, Tree);
		Tree->EditorData = NewEditorData;

		const bool bViewModelStale = ViewModel->GetStateTreeEditorData() == OldEditorData;
		const bool bFoundAgain = Subsystem && Subsystem->FindViewModel(Tree).Get() == ViewModel.Get();
		ViewModel->NotifyAssetChangedExternally();
		const bool bViewModelStillStale = ViewModel->GetStateTreeEditorData() == OldEditorData;

		// Put things back so the fixture cleans up like the other tests.
		Tree->EditorData = OldEditorData;

		Log(FString::Printf(TEXT("[Replace EditorData object] the view model still points at the OLD editor data afterwards=%s (also after NotifyAssetChangedExternally=%s); the subsystem still hands out the same view model=%s.\n  Meaning: a write-back must change the existing editor data in place; swapping the object leaves an open editor on stale data.\n  Events:\n%s"),
			bViewModelStale ? TEXT("yes") : TEXT("no"), bViewModelStillStale ? TEXT("yes") : TEXT("no"), bFoundAgain ? TEXT("yes") : TEXT("no"), *Recorder.SummarizeSegment(Segment)));
	}

	// ---- Is the asset open in an editor here? ----------------------------------------------------------------------
	{
		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		const bool bOpen = AssetEditors && AssetEditors->FindEditorForAsset(Tree, /*bFocusIfOpen*/ false) != nullptr;
		Log(FString::Printf(TEXT("An asset editor window is open for the test asset: %s. Opening one needs the full Slate editor; this run has none."), bOpen ? TEXT("yes") : TEXT("no")));
	}

	Log(FString(TEXT("NOT TESTABLE HEADLESS, needs a manual test in an editor with the asset open:\n"))
		+ TEXT("  1. Does the SStateTreeView outline redraw when OnAssetChanged / NotifyAssetChangedExternally fires (names, new child state)?\n")
		+ TEXT("  2. Does the Details panel of the selected state show the new values after an in-place change, or does it keep showing the old ones until reselected?\n")
		+ TEXT("  3. Does the editor's undo stack stay consistent when the write-back changes data without a transaction (Ctrl+Z after a write-back)?\n")
		+ TEXT("  4. Does the Content Browser show the asset as modified (package dirty star) after Tree->MarkPackageDirty()?"));

	Report += TEXT("Full event log:\n") + Recorder.FormatAll();
	Recorder.Stop();

	AddInfo(FString::Printf(TEXT("Full report: %s"), *SaveReport(TEXT("Capture"), TEXT("ExternalChange"), Report)));
	return true;
}
