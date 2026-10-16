// Copyright DreamMoon. All Rights Reserved.
//
// The parser. Recursive descent over the tokens of LexDst, following Docs/Grammar.md. It keeps going after an
// error: an entry or clause that fails to parse is skipped up to the next point it can resume at (a `;` in a
// section, the next clause keyword or `State` in a state body, the `}` closing the block), and the rest of the file
// is still parsed, so one run reports as many problems as it can.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"

namespace UE::DreamStateTree::Lang
{
	struct FDstParseOptions
	{
		/** Record comments and blank lines in FDstFile::Trivia (the printer and the write-back need them). */
		bool bKeepTrivia = true;
	};

	/**
	 * Parses one file. Never returns null: on errors the file holds whatever parsed. The file's Path is the source's
	 * path, and the sink should be constructed with the same path.
	 */
	DREAMSTATETREELANG_API TUniquePtr<FDstFile> ParseDst(const FDstSourceText& Source, FDstDiagnosticSink& Sink, const FDstParseOptions& Options = FDstParseOptions());

	/**
	 * Parses a single expression (the whole text must be one expression, surrounding whitespace and comments aside).
	 * Null with an error reported when it is not. Used to check a value the editor is about to write back.
	 */
	DREAMSTATETREELANG_API FDstExprPtr ParseDstExpression(const FDstSourceText& Source, FDstDiagnosticSink& Sink);

	/** The words that start a clause inside a state body: on, if, pick, utility, link, do (the `->` token is the seventh). */
	DREAMSTATETREELANG_API bool IsDstClauseWord(const FString& Word);
}
