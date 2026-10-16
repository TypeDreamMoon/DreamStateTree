// Copyright DreamMoon. All Rights Reserved.
//
// The generator: model -> StateTree asset. It creates the asset or rebuilds it in place (the asset object and its
// editor data object stay, so references and an open editor survive), compiles it through the engine's compiler
// manager, and reports the compile's messages against the text.
//
// The editor steps code must do itself (spike-report.md, "决定一"): OnBindingChanged on every bound node,
// SetLinkedState / SetLinkedStateAsset for links, AddTransition for transitions, UpdateBindings after the parameters,
// Blueprint instance objects outered to their state, every ID set to a deterministic GUID right after creation, and
// all bindings added after every node and parameter exists. A compile that changes the editor data is reported as an
// error: it means the generator wrote something the engine had to correct.

#pragma once

#include "CoreMinimal.h"
#include "DstLower.h"
#include "Lang/DstDiagnostic.h"
#include "Model/DstModel.h"

class UStateTree;

namespace UE::DreamStateTree::Compiler
{
	/** The package metadata key holding the text an asset was generated from. */
	DREAMSTATETREECOMPILER_API extern const FName DstSourceMetadataKey;
	/** The package metadata key holding the path of that source file, relative to its source root. */
	DREAMSTATETREECOMPILER_API extern const FName DstSourcePathMetadataKey;

	/** The GUID of a model key in an asset: MD5 of `<asset path>|<key>[|<salt>]`. Stable across rebuilds. */
	DREAMSTATETREECOMPILER_API FGuid MakeDstGuid(const FString& AssetPath, const FString& Key, const TCHAR* Salt = nullptr);

	struct FDstGenerateOptions
	{
		/** Save the package after a successful build. */
		bool bSave = true;
		/** Kept in the package metadata (DstSourceMetadataKey): the base for detecting and merging edits. */
		FString SourceText;
		FString SourcePath;
	};

	struct FDstGenerateResult
	{
		UStateTree* Asset = nullptr;
		bool bCreated = false;
		bool bCompiled = false;
		/** The editor data dumps before and after the compile differed (an error is reported too). */
		bool bCompileChangedEditorData = false;
		/** Model key -> the GUID it got, for the write-back. */
		TMap<FString, FGuid> Ids;
	};

	/**
	 * Builds Tree into its asset (Tree.AssetPath). Places maps the model to the text for diagnostics. Returns false
	 * when any error was reported.
	 */
	DREAMSTATETREECOMPILER_API bool GenerateDst(const Model::FDstTree& Tree, const FDstPlaces& Places, const FDstGenerateOptions& Options,
		FDstGenerateResult& OutResult, Lang::FDstDiagnosticSink& Sink);
}
