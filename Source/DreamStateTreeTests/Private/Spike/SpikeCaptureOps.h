// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Spike/SpikeCaptureHelpers.h"

// Editor operations the capture experiments replay against the baseline asset, and the bookkeeping that turns one
// replayed operation into a readable record.
namespace UE::DreamStateTree::Spike
{
	/** One editor action, written the way the editor UI performs it. */
	struct FDstCaptureOp
	{
		FString Name;
		/**
		 * Performs the action on the fixture. Returns false and fills OutNote when it could not run because the
		 * baseline no longer has what it needs (an earlier operation of a cumulative run changed it).
		 */
		TFunction<bool(FDstCaptureFixture& Fixture, FString& OutNote)> Run;
	};

	/** Actions the state-tree outliner performs through FStateTreeViewModel. */
	TArray<FDstCaptureOp> MakeViewModelOps();

	/** Edits the Details panel performs: PreEditChange / PostEditChangeChainProperty around a property change. */
	TArray<FDstCaptureOp> MakeDetailsOps();

	/** What one replayed operation did, measured around the call. */
	struct FDstOpRecord
	{
		FString Name;
		bool bRan = false;
		/** Why the operation did not run, or what it found out. */
		FString Note;
		/** Recorder segment holding the events of the operation. */
		int32 Segment = INDEX_NONE;
		/** Records the operation added to the editor's undo buffer. */
		int32 NewTransactions = 0;
		bool bDataChanged = false;
		/** The engine's own change detector (hash used to decide whether a recompile is needed) saw a change. */
		bool bHashChanged = false;
		bool bTreeDirtyAfter = false;
		bool bNeedsRecompileAfter = false;
		bool bPackageDirtyAfter = false;
		FString PreDump;
		FString PostDump;
		/** Lines only in the dump before / only in the dump after. */
		FString DataDiff;
	};

	/** Runs the operation inside its own recorder segment and measures what changed. */
	FDstOpRecord DstRunOp(FDstCaptureFixture& Fixture, FDstCaptureRecorder& Recorder, const FDstCaptureOp& Op);

	/** Compiles without recording, so the dirty state the engine tracks starts clean for the next operation. */
	void DstQuietCompile(FDstCaptureFixture& Fixture, FDstCaptureRecorder& Recorder);

	/** The record and the events of its segment as report text. */
	FString DstFormatOpRecord(const FDstOpRecord& Record, const FDstCaptureRecorder& Recorder);

	/** One row per operation, one column per hook, with the number of events each hook delivered. */
	FString DstFormatCoverageMatrix(const TArray<FDstOpRecord>& Records, const FDstCaptureRecorder& Recorder);

	/** Result of undoing and redoing one recorded operation. */
	struct FDstUndoRedoRecord
	{
		bool bUndoReturnedTrue = false;
		bool bRedoReturnedTrue = false;
		int32 UndoSegment = INDEX_NONE;
		int32 RedoSegment = INDEX_NONE;
		/** The editor data dump after the undo equals the dump before the operation. */
		bool bUndoRestoredData = false;
		/** The editor data dump after the redo equals the dump right after the operation. */
		bool bRedoRestoredData = false;
		/** After the final undo the data equals the baseline again, so the next operation starts from the same tree. */
		bool bBackAtBaseline = false;
		FString UndoDiff;
		FString RedoDiff;
	};

	/**
	 * Undoes the operation (once per undo record it made), redoes it, then undoes it again so the next operation
	 * starts from the baseline. Only the first undo and the redo are recorded.
	 */
	FDstUndoRedoRecord DstRunUndoRedo(FDstCaptureFixture& Fixture, FDstCaptureRecorder& Recorder, const FDstOpRecord& Record);
}
