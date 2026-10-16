// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UStateTree;
class UStateTreeEditorData;

// Text dumps of StateTree assets for the engine-behaviour experiments: what the editor data holds, what the compiler
// made of it, and which lines differ between two dumps. Every dump is deterministic: the same data gives the same text.
namespace UE::DreamStateTree::Spike
{
	struct FDumpOptions
	{
		/** Print IDs (states, nodes, transitions, parameters, events, binding sources). Off to compare two trees whose IDs differ. */
		bool bIncludeIds = true;
		/** Print the values of every authored property of nodes and instance data, not only the ones that differ from the type's default. */
		bool bAllValues = false;
	};

	/**
	 * Every authored field of the editor data, one item per line, in tree order: schema, parameters, global nodes,
	 * colors, then each state (type, selection, tasks completion, event, enter conditions with operand and indent,
	 * considerations, tasks, transitions, linked subtree or asset, parameters and overrides, children), then every
	 * property binding with its source resolved to a readable name.
	 */
	FString DumpEditorData(const UStateTreeEditorData& EditorData, const FDumpOptions& Options = FDumpOptions());

	/** The compiled runtime data of the asset (states, nodes, transitions, bindings) in a readable form, for comparing two compiles. */
	FString DumpCompiled(const UStateTree& StateTree);

	/** The lines only in A and only in B, at most MaxLines of each; empty when the two are equal. */
	FString DiffLines(const FString& A, const FString& B, int32 MaxLines = 200);

	/** Writes the text to <Saved>/DreamStateTreeTests/Spike/<Folder>/<Name>.txt (UTF-8) and returns the full path. */
	FString SaveReport(const FString& Folder, const FString& Name, const FString& Text);
}
