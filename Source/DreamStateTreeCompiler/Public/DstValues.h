// Copyright DreamMoon. All Rights Reserved.
//
// Constant values: the text's literals and the engine's export text, converted both ways for one property. The
// model stores constants as export text (DstExportDefault), so the generator imports them as they are and
// the extractor reads them as they are; these two functions are where the text's spelling meets them.
//
// What a literal may be for each property type (Docs/Language.md, "Values"):
//   numbers                      2, 0.5, -1, 1e3
//   bool                         true, false
//   FName, FString, FText        "text" (a plain identifier is accepted for FName)
//   enum                         the value's name, `Walk` (or `EMode::Walk`)
//   FGameplayTag                 a tag path `Game.Event.Hit` or "Game.Event.Hit"; tag prefixes apply
//   FGameplayTagContainer        [Tag, Tag]
//   struct                       a tuple in declaration order `(0, 3000)` or by field `(Min = 0, Max = 3000)`
//   array, set                   [a, b, c]
//   object, soft object          an asset's short name `EQ_Wander` (unique in the asset registry), `Path(Root, "Rel")`,
//                                `None`
//   class, soft class            a class name (`Actor`, `BP_Enemy`) or `Path(...)`, `None`

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Lang/DstAst.h"

class FProperty;
class UClass;

namespace UE::DreamStateTree::Compiler
{
	struct FDstValueContext
	{
		/** Tag prefixes declared by the file (`Tags = { ... }`), tried in order to complete a written tag. */
		TArray<FString> TagPrefixes;
	};

	/** Shortest text that reads back as the same double: `0.2`, `1`, `-0.5`, `1e-06`. */
	DREAMSTATETREECOMPILER_API FString FormatDstNumber(double Value);

	/**
	 * The export text for a literal written for Property. False with OutError set when the literal does not fit the
	 * property (wrong kind, unknown enum value, unknown or ambiguous asset, unknown tag).
	 */
	DREAMSTATETREECOMPILER_API bool DstLiteralToExportText(const Lang::FDstExpr& Literal, const FProperty& Property,
		const FDstValueContext& Context, FString& OutText, FString& OutError);

	/** The literal the decompiler writes for an export text of Property. Null when the text does not import. */
	DREAMSTATETREECOMPILER_API Lang::FDstExprPtr DstExportTextToLiteral(const FString& ExportText, const FProperty& Property,
		const FDstValueContext& Context);

	/**
	 * The export text of Property inside Container: the whole value, no delta (PPF_None). The one exporter every
	 * constant in the model goes through, so text and asset agree to the character. A null container exports the
	 * property's freshly initialised value.
	 */
	DREAMSTATETREECOMPILER_API FString DstExportDefault(const FProperty& Property, const void* DefaultContainer);

	/**
	 * The tag a written name means: as written when the project has it, otherwise completed with the prefixes in turn.
	 * Several different completions are an error; so is none. An empty name or `None` is the empty tag.
	 */
	DREAMSTATETREECOMPILER_API bool ResolveDstTagName(const FString& Written, TConstArrayView<FString> Prefixes, FGameplayTag& OutTag,
		FString& OutError);

	/** The shortest way to write Tag that ResolveDstTagName turns back into Tag. */
	DREAMSTATETREECOMPILER_API FString ShortenDstTagName(const FGameplayTag& Tag, TConstArrayView<FString> Prefixes);

	/** `Path(Root, "Rel")` as an object path. False when Expr is not that call. */
	DREAMSTATETREECOMPILER_API bool TryGetDstAssetPath(const Lang::FDstExpr& Expr, FString& OutObjectPath);

	/** The unique asset called Name whose class is AssetClass (or a child; null for any). Sets OutError otherwise. */
	DREAMSTATETREECOMPILER_API bool FindDstAsset(const FString& Name, const UClass* AssetClass, FString& OutObjectPath, FString& OutError);

	/** The asset as the decompiler writes it: its short name when unique among assets of AssetClass, else `Path(Root, "Rel")`. */
	DREAMSTATETREECOMPILER_API Lang::FDstExprPtr MakeDstAssetExpr(const FString& ObjectPath, const UClass* AssetClass);
}
