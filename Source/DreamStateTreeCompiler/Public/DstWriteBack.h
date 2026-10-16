// Copyright DreamMoon. All Rights Reserved.
//
// Write-back: what someone changed in the StateTree editor on an asset generated from text, written into that text.
// The asset is extracted again and compared with the model the text lowers to. Parts are matched by the GUID the
// generator derived from their place in the text, then by state path and node name or index (an asset saved under
// another name has new GUIDs). Then, smallest edit first:
//   - a changed constant replaces just that value's text;
//   - a node whose arguments come or go is reprinted in place;
//   - anything else inside a state (clauses, nodes or transitions added, removed or reordered) reprints that state's
//     card, keeping the text of its child cards that did not change;
//   - a changed header or section (Parameters, Globals, Colors, Settings) reprints that section.
// The result must lower to the asset's model again (the fixed point), or nothing is written. A change inside a
// template's expansion or inside a subtree copied from a library is refused: it belongs in the template or the
// library. Docs/WriteBack.md lists what is written and what is refused; DST8101-DST8199 are its diagnostics.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstDiagnostic.h"

class UStateTree;

namespace UE::DreamStateTree::Compiler
{
	/** One difference between a source and the asset generated from it. */
	struct FDstWriteBackChange
	{
		/** Where, in the text's terms: `State Root/Attack`, `Parameters`, `State Root/Attack: Task Combo`. */
		FString Location;
		/** What, briefly: `Duration`, `the transitions`, `the child states`. */
		FString What;
		/** Written into the text; false when it was refused (the diagnostics say why). */
		bool bWritten = false;
		/** Written by reprinting a card or a section rather than editing a value in place. */
		bool bReprinted = false;
	};

	struct FDstWriteBackOptions
	{
		/** Write structural changes by reprinting the card or section they are in; false refuses them. */
		bool bStructure = true;
	};

	struct FDstWriteBackResult
	{
		/** The source with the asset's changes written in; the input text when nothing was written. */
		FString Text;
		TArray<FDstWriteBackChange> Changes;
		/** Refusals and problems (DST81xx), and the text's own errors when it does not check. */
		Lang::FDstDiagnosticSink Diagnostics;
		/** The text could be read, checked and compared with the asset. */
		bool bCompared = false;
		/** The new text lowers to the asset's model: text and asset agree again. */
		bool bFixedPoint = false;
		/** Something in the asset was refused: the text cannot hold all of it. */
		bool bRefused = false;

		/** Everything the asset holds is in the text: compared, a fixed point, nothing refused, every change written. */
		bool IsComplete() const
		{
			return bCompared && bFixedPoint && !bRefused
				&& !Changes.ContainsByPredicate([](const FDstWriteBackChange& Change) { return !Change.bWritten; });
		}

		bool HasEdits() const
		{
			return Changes.ContainsByPredicate([](const FDstWriteBackChange& Change) { return Change.bWritten; });
		}
	};

	/**
	 * What Asset holds that Text (the source at FilePath it was generated from) does not, written into the text.
	 * FilePath resolves #include and names the file in diagnostics; nothing is read from or written to disk here
	 * except the libraries the text includes.
	 */
	DREAMSTATETREECOMPILER_API FDstWriteBackResult WriteBackDstAsset(const UStateTree& Asset, const FString& FilePath, const FString& Text,
		const FDstWriteBackOptions& Options = FDstWriteBackOptions());

	/**
	 * The text the asset was last generated from and that text's source path, from the package metadata
	 * (DstSourceMetadataKey, DstSourcePathMetadataKey). False when the asset was not generated from text.
	 */
	DREAMSTATETREECOMPILER_API bool GetDstGeneratedText(const UStateTree& Asset, FString& OutText, FString& OutSourcePath);

	/** Records Text as what the asset is in step with (after a write-back), without rebuilding it. Marks the package dirty. */
	DREAMSTATETREECOMPILER_API void SetDstGeneratedText(UStateTree& Asset, const FString& Text, const FString& SourcePath);
}
