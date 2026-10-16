// Copyright DreamMoon. All Rights Reserved.
//
// #include resolution and template expansion: the two steps between parsing a file and checking what its names
// mean.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"

namespace UE::DreamStateTree::Lang
{
	/** Finds and reads #included files. */
	class IDstIncludeLoader
	{
	public:
		virtual ~IDstIncludeLoader() = default;

		/**
		 * Resolves IncludePath as written in the file FromFile. On success fills the resolved path (the same file must
		 * always resolve to the same string, it is the cycle and dedupe key) and its text.
		 */
		virtual bool Load(const FString& FromFile, const FString& IncludePath, FString& OutResolvedPath, FString& OutText) = 0;
	};

	/** Resolves relative to the including file's folder first, then against each root in order. */
	class DREAMSTATETREELANG_API FDstFileSystemIncludeLoader final : public IDstIncludeLoader
	{
	public:
		explicit FDstFileSystemIncludeLoader(TArray<FString> InRoots = {}) : Roots(MoveTemp(InRoots)) {}
		virtual bool Load(const FString& FromFile, const FString& IncludePath, FString& OutResolvedPath, FString& OutText) override;

	private:
		TArray<FString> Roots;
	};

	/** One file and every library it reaches through #include. */
	struct FDstCompilationUnit
	{
		FDstSourceText MainSource;
		TUniquePtr<FDstFile> Main;
		/** Each library once, in the order first reached. */
		TArray<FDstSourceText> LibrarySources;
		TArray<TUniquePtr<FDstFile>> Libraries;
	};

	/**
	 * Parses every file Unit.Main #includes, recursively, each once. An included file must hold a StateTreeLibrary
	 * document (DST3203); a missing file is DST3201; a cycle is DST3202. Diagnostics about a library carry its path.
	 * Returns false when any error was reported.
	 */
	DREAMSTATETREELANG_API bool ResolveDstIncludes(FDstCompilationUnit& Unit, IDstIncludeLoader& Loader, FDstDiagnosticSink& Sink);

	/**
	 * Replaces every template use in Unit.Main's document with the template's body.
	 *
	 * - Templates are the `Template Name(...) = Body;` members, in Main and in every library. Two with the same name is
	 *   DST3105.
	 * - A use is a CallExpr whose callee is a bare NameExpr naming a template, or a bare NameExpr naming a template
	 *   declared without parentheses. Arguments bind to parameters positionally, then by name; a parameter without an
	 *   argument takes its default. DST3101 / 3102 / 3103 for a missing, unknown or extra argument.
	 * - The body is cloned and every NameExpr naming a parameter is replaced by a clone of the bound value; this also
	 *   covers a parameter used as a path prefix (`VB.ForwardAmount`). Argument names and metadata keys are never
	 *   substituted.
	 * - The result is expanded again, so a template may use another; more than 16 levels is DST3104.
	 * - Spans: nodes that came from the template body take the call's span; nodes that came from an argument keep
	 *   their own spans. Unit.Main->Origins gets one entry per expansion, keyed by the root of the expanded expression.
	 * - Template definitions themselves are left in the tree, unexpanded.
	 * - Library subtrees are not templates: the semantic stage copies one into a tree that links it.
	 *
	 * Returns false when any error was reported.
	 */
	DREAMSTATETREELANG_API bool ExpandDstTemplates(FDstCompilationUnit& Unit, FDstDiagnosticSink& Sink);
}
