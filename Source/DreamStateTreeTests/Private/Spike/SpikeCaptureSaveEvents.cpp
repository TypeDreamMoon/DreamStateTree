// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureHelpers.h"
#include "Spike/SpikeCaptureOps.h"
#include "Spike/SpikeDump.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "StateTree.h"
#include "StateTreeViewModel.h"
#include "UObject/SavePackage.h"

// What an observer sees when the asset is saved (OnObjectPreSave and friends). Saving needs a file, so this one is
// opt-in: pass -DstSpikeSave on the command line. The file is written under <Saved>/DreamStateTreeTests/Spike/Capture/Tmp
// and removed again; nothing is written to Content.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstSpikeCaptureSaveEventsTest, "DreamStateTree.Spike.Capture.SaveEvents",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeCaptureSaveEventsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	if (!FParse::Param(FCommandLine::Get(), TEXT("DstSpikeSave")))
	{
		AddInfo(TEXT("Skipped: this experiment saves the test asset to a temporary file under Saved/. Add -DstSpikeSave to the command line to run it."));
		return true;
	}

	FDstCaptureFixture Fixture;
	FString Error;
	if (!Fixture.Initialize(TEXT("SaveEvents"), Error))
	{
		AddError(FString::Printf(TEXT("Could not set up the asset: %s"), *Error));
		return false;
	}

	const TSharedPtr<FStateTreeViewModel> ViewModel = Fixture.GetOrCreateViewModel();
	FDstCaptureRecorder Recorder;
	Recorder.Track(Fixture.GetTree());
	Recorder.Start();
	Recorder.AttachViewModel(ViewModel);

	FString Report;

	// Dirty the asset first, the way an edit session would.
	const TArray<FDstCaptureOp> Ops = MakeDetailsOps();
	if (!Ops.IsEmpty())
	{
		const FDstOpRecord Record = DstRunOp(Fixture, Recorder, Ops[0]);
		Report += DstFormatOpRecord(Record, Recorder) + TEXT("\n\n");
	}

	const FString TempDirectory = FPaths::ProjectSavedDir() / TEXT("DreamStateTreeTests/Spike/Capture/Tmp");
	IFileManager::Get().MakeDirectory(*TempDirectory, /*bTree*/ true);
	const FString FilePath = TempDirectory / (Fixture.GetTree()->GetName() + FPackageName::GetAssetPackageExtension());

	const int32 Segment = Recorder.BeginSegment(TEXT("UPackage::SavePackage to a temporary file"));
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.bSlowTask = false;
	const bool bSaved = UPackage::SavePackage(Fixture.GetPackage(), Fixture.GetTree(), *FilePath, SaveArgs);

	const FString SaveText = FString::Printf(TEXT("[SavePackage] succeeded=%s, package dirty afterwards=%s, tree needs recompile afterwards=%s\n  events:\n%s"),
		bSaved ? TEXT("yes") : TEXT("no"), (Fixture.GetPackage() && Fixture.GetPackage()->IsDirty()) ? TEXT("yes") : TEXT("no"),
		Fixture.GetTree()->IsEditorDataDirty() ? TEXT("yes") : TEXT("no"), *Recorder.SummarizeSegment(Segment));
	AddInfo(SaveText);
	Report += SaveText + TEXT("\n\n");

	IFileManager::Get().Delete(*FilePath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);

	Report += TEXT("Full event log:\n") + Recorder.FormatAll();
	Recorder.Stop();

	AddInfo(FString::Printf(TEXT("Full report: %s"), *SaveReport(TEXT("Capture"), TEXT("SaveEvents"), Report)));
	return true;
}
