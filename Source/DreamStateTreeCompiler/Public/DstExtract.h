// Copyright DreamMoon. All Rights Reserved.
//
// The extractor: StateTree asset -> model. It reads the editor data literally (spike S2 read every field of nine
// corpus trees with nothing left unresolved) and writes what the model holds:
//   - node properties only where they differ from the type's default (the struct's default, a Blueprint node's
//     class default object), plus every binding into them;
//   - bindings resolved to their source kind and owner (parameters, context, global nodes, nodes, state parameters,
//     state and transition events), property functions nested into the property they feed;
//   - an explicit binding to a context that equals the binding the compiler would make by itself is left out;
//   - the editor's view-settings extension (StateCentricViewEditorDataExtension) is ignored.
// What the model cannot hold (an editor data subclass, an extension it does not know) is reported as DST8xxx and the
// extraction fails.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstDiagnostic.h"
#include "Model/DstModel.h"

class UStateTree;

namespace UE::DreamStateTree::Compiler
{
	struct FDstExtractResult
	{
		/** Model key (or state path, `<state path>#<transition index>`, `Param:<name>`) -> the GUID the asset holds for it. */
		TMap<FString, FGuid> Ids;
	};

	/** Reads Asset into OutTree. Returns false when any error was reported. */
	DREAMSTATETREECOMPILER_API bool ExtractDst(const UStateTree& Asset, Model::FDstTree& OutTree, Lang::FDstDiagnosticSink& Sink,
		FDstExtractResult* OutResult = nullptr);
}
