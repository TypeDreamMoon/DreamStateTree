// Copyright DreamMoon. All Rights Reserved.
//
// The semantic stage: a parsed, include-resolved and template-expanded file -> the model, with every check that
// needs to know what the names mean (DST4xxx names and types, DST5xxx bindings and expressions, DST6xxx structure).
//
// It applies the text's conveniences on the way in:
//   - short node names, schema namespaces, positional arguments, tag prefixes;
//   - `a < b`, `x == Walk`, a lone bool path, `not C` -> Compare nodes and bInvert;
//   - `and` / `or` / parentheses -> the conditions' operands and indents;
//   - `1 + Params.X * 0.5` in an argument -> property function nodes bound into the property;
//   - `utility 0.5` -> a Constant consideration;
//   - a subtree from an #included library that a tree links -> copied into the tree;
//   - an `on Tag` without a payload type -> the default an extension gives for the tag.
// and it checks what the engine would accept silently or change on its own (spike-report.md): nodes and features the
// schema does not allow, a Group with tasks, a Linked state with children, bindings to properties that do not exist,
// Input properties left unbound.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstTemplates.h"
#include "Model/DstModel.h"

namespace UE::DreamStateTree::Compiler
{
	/**
	 * Where each part of the model was written, so that the generator and the engine's compile log can report against
	 * the text. Keys: a node key (`Root/Attack/Task[0]`), a state path (`Root/Attack`), `<state path>#<transition
	 * index>` for a transition, `Param:<name>` for a tree parameter, `Global:<name>` for a global node, and the empty key
	 * for the file header.
	 */
	struct FDstPlaces
	{
		FString FilePath;
		TMap<FString, Lang::FDstSpan> Spans;

		DREAMSTATETREECOMPILER_API void Add(const FString& Key, const Lang::FDstSpan& Span);
		/** The span for a key, or the closest enclosing one (a node falls back to its state, a state to its parent, then the header). */
		DREAMSTATETREECOMPILER_API Lang::FDstSpan Find(const FString& Key) const;
	};

	struct FDstLowerOptions
	{
		/** Content root of the asset when the header gives none: "Game". */
		FString DefaultRoot = TEXT("Game");
	};

	struct FDstLowerResult
	{
		Model::FDstTree Tree;
		FDstPlaces Places;
		/** Object paths of the StateTree assets the tree links (`link asset`), to build first. */
		TArray<FString> LinkedAssets;
		/** The tag prefixes the file declared, its libraries' included. */
		TArray<FString> TagPrefixes;
	};

	/**
	 * Checks Unit (its main file must be a StateTree document; includes resolved and templates expanded) and lowers it
	 * to the model. Returns false when any error was reported; Out holds whatever lowered.
	 */
	DREAMSTATETREECOMPILER_API bool LowerDst(const Lang::FDstCompilationUnit& Unit, FDstLowerResult& Out,
		Lang::FDstDiagnosticSink& Sink, const FDstLowerOptions& Options = FDstLowerOptions());

	/**
	 * Gives every node that something binds to, and every global node, a BindName when it has none: the node type's
	 * short name, made unique within the tree with a number. The decompiler runs it before raising the model to text,
	 * and the round-trip tests run it on an extracted model before comparing.
	 */
	DREAMSTATETREECOMPILER_API void EnsureDstBindNames(Model::FDstTree& Tree);
}
