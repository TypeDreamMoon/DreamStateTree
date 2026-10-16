// Copyright DreamMoon. All Rights Reserved.
//
// The raise: model -> syntax tree, the way back from the extractor to text (the decompiler prints the result). It
// recovers the text's conveniences, each as the exact inverse of what the semantic stage applies, so that lowering the
// result gives the same model again:
//   - node types -> the shortest name that resolves back under the schema's namespaces; positional arguments where
//     the type declares them;
//   - Compare nodes -> `a < b` (with `not` for bInvert); a CompareBool against true -> the bare path;
//   - operands and indents -> `and` / `or` with the parentheses they need;
//   - property functions -> `+ - * /`, `-x`, `and`, `or`, `not`, where the function is one of those;
//   - a Constant consideration alone -> `utility 0.5`;
//   - constants -> literals (DstExportTextToLiteral); asset references -> short names when unique;
//   - nodes something binds to get a name (EnsureDstBindNames runs first).

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Model/DstModel.h"

namespace UE::DreamStateTree::Compiler
{
	struct FDstRaiseOptions
	{
		/** The path the text will live at (`DTree/AI/ST_Brawler.dtr`); the file's Path. */
		FString FilePath;
		/** Tag prefixes to shorten tags with, and to write as the `Tags` section. Empty: full tags, no section. */
		TArray<FString> TagPrefixes;
		/** Content root the header's Name is relative to; empty to work it out from the asset path. */
		FString Root;
	};

	/**
	 * The syntax tree of a StateTree document for Tree. Reports what it cannot write (DST8xxx) and returns null only
	 * when nothing could be written.
	 */
	DREAMSTATETREECOMPILER_API TUniquePtr<Lang::FDstFile> RaiseDst(const Model::FDstTree& Tree, const FDstRaiseOptions& Options,
		Lang::FDstDiagnosticSink& Sink);
}
