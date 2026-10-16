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
//   reference to a StateTree     the tree's asset, with the parameters it overrides: `ST_Shoot`, `ST_Shoot(Range = 800)`,
//                                `Path(Game, "AI/ST_Shoot")(Range = 800)`, `None`

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Lang/DstAst.h"
#include "Model/DstModel.h"

class FProperty;
class UClass;
class UStruct;

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

	// ------------------------------------------------------------------------------------------------------------------
	// Values with parts fed from elsewhere
	// ------------------------------------------------------------------------------------------------------------------

	/**
	 * The fields a tuple reads by position, in declaration order (a base struct's first): the ones a text can set, which are
	 * not transient or deprecated.
	 */
	DREAMSTATETREECOMPILER_API TArray<const FProperty*> GetDstTupleFields(const UStruct& Struct);

	/**
	 * A literal for the value a field or element has when the text leaves it out: what a constant holds at a place that is
	 * fed from elsewhere. A struct's literal is the empty tuple. Null when the property has no literal.
	 */
	DREAMSTATETREECOMPILER_API Lang::FDstExprPtr MakeDstDefaultLiteral(const FProperty& Property, const FDstValueContext& Context);

	/** Whether Property is an FStateTreeReference: a tree asset and the values it gives that tree's parameters. */
	DREAMSTATETREECOMPILER_API bool IsDstStateTreeReferenceProperty(const FProperty& Property);

	/**
	 * The parameter of the tree at TreeObjectPath, as a property of the parameter bag that tree gives its references. Null
	 * when the tree cannot be loaded or has no parameter of that name.
	 */
	DREAMSTATETREECOMPILER_API const FProperty* FindDstTreeParameter(const FString& TreeObjectPath, const FString& ParameterName);

	/**
	 * Puts the part of the value that a binding path leads to back to the default a text gives it (see FDstArg). Path[0] names
	 * Property itself (its ArrayIndex picks an element when Property is an array), the rest are members; through a reference
	 * to a tree the path is `Parameters`, `Value<PropertyBag>` and the parameter. False with OutError when the path leads
	 * nowhere: no such member, an element the array does not have.
	 */
	DREAMSTATETREECOMPILER_API bool ResetDstBoundValue(const FProperty& Property, void* Value, TConstArrayView<Model::FDstPathSegment> Path,
		FString& OutError);

	/**
	 * The export text of Property in Container the way the model holds it when parts of it are fed by bindings: the parts
	 * BoundPaths lead to put back to their defaults (ResetDstBoundValue); for a reference to a tree, also its parameters
	 * brought in step with the tree, the overridden ones in the order of the tree's parameters, the fed ones overridden.
	 * Container is not changed. OutSameAsDefault tells whether that value equals Property in DefaultContainer (null: never).
	 * False with OutError when a path leads nowhere.
	 */
	DREAMSTATETREECOMPILER_API bool ExportDstValueWithBoundParts(const FProperty& Property, const void* Container, const void* DefaultContainer,
		TConstArrayView<Model::FDstPropertyPath> BoundPaths, FString& OutText, bool& bOutSameAsDefault, FString& OutError);
}
