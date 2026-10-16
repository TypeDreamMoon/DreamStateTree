// Copyright DreamMoon. All Rights Reserved.
//
// The names the text uses for engine things: node types, schemas, Blueprint nodes. One registry serves both ways:
// the semantic stage resolves what the text wrote, and the decompiler asks what to write for a struct or class.
//
// Rules (Docs/Language.md, "Names"):
// - A node struct's short name drops a leading `F`, then a leading `StateTree`, then a trailing `Task`, `Condition`,
//   `Evaluator`, `Consideration` or `PropertyFunction`: FStateTreeDelayTask -> Delay,
//   FStateTreeAddFloatPropertyFunction -> AddFloat. A struct with `meta=(DreamStateTree="Villager.Walk")` is called
//   that instead; `DreamStateTreeAliases="Villager.Move;Move"` adds old names that still resolve.
// - The struct's name without `F` always resolves too (StateTreeDelayTask).
// - A schema with `meta=(DreamStateTreeNamespace="Villager;Game")` makes `Walk` find `Villager.Walk` first, then
//   `Game.Walk`, then the global `Walk`. The first namespace that has the name wins; a shadowed name is a warning.
// - Schemas: the class name without `U`, `StateTree` and `Schema` (UStateTreeAIComponentSchema -> AIComponent), or
//   the `DreamStateTree` meta.
// - Blueprint nodes: the Blueprint asset's name (STT_Wander), found through the asset registry.

#pragma once

#include "CoreMinimal.h"

class UClass;
class UScriptStruct;
class UStateTreeSchema;

namespace UE::DreamStateTree::Compiler
{
	enum class EDstNodeCategory : uint8
	{
		Task,
		Condition,
		Evaluator,
		Consideration,
		PropertyFunction,
		Unknown,
	};
	DREAMSTATETREECOMPILER_API const TCHAR* LexToString(EDstNodeCategory Category);

	/** A node type the text can name. */
	struct FDstNodeType
	{
		/** The C++ node struct; for a Blueprint node, the engine's wrapper struct (FStateTreeBlueprintTaskWrapper, ...). */
		const UScriptStruct* Struct = nullptr;
		/** A Blueprint node's generated class; null for C++ nodes. Loaded on demand. */
		const UClass* BlueprintClass = nullptr;
		/** The Blueprint asset's object path when BlueprintClass is not loaded yet. */
		FString BlueprintAssetPath;
		EDstNodeCategory Category = EDstNodeCategory::Unknown;
		/** What the decompiler writes: the meta name (`Villager.Walk`) or the short name (`Delay`). */
		FString Name;
		/** Names that also resolve: the struct name without `F`, aliases. */
		TArray<FString> OtherNames;
		/** Properties that may be passed by position, in order (meta DreamStateTreeArgs, or the built-in table). */
		TArray<FName> PositionalArgs;
		/** Validation tags (meta DreamStateTreeTags), for extensions. */
		TArray<FString> Tags;
	};

	/** What a written name resolved to. */
	struct FDstNameMatch
	{
		const FDstNodeType* Type = nullptr;
		/** The namespace that matched (`Villager`); empty for a global name. */
		FString Namespace;
	};

	class DREAMSTATETREECOMPILER_API FDstNameRegistry
	{
	public:
		/** The registry of the running editor, built on first use. */
		static FDstNameRegistry& Get();

		/** Rebuilds from the loaded types and the asset registry (after a hot reload, or when Blueprints were added). */
		void Refresh();

		/**
		 * Every type a written node name can mean under the schema's namespaces (`Walk`, `Villager.Walk`, `Delay`,
		 * `StateTreeDelayTask`, `STT_Wander`): one entry when unique; several when ambiguous (the caller reports them);
		 * none when unknown. Namespaces are searched in order, then the global names; the first that has the name wins,
		 * and OutShadowed (when given) receives the matches it hid. Categories (when given) are the kinds the place takes:
		 * only those count, so a task does not hide or clash with a condition of the same name.
		 */
		TArray<FDstNameMatch> ResolveNode(const FString& Written, TConstArrayView<FString> Namespaces, TArray<FDstNameMatch>* OutShadowed = nullptr,
			TConstArrayView<EDstNodeCategory> Categories = {}) const;

		const FDstNodeType* FindByStruct(const UScriptStruct* Struct) const;
		const FDstNodeType* FindByBlueprintClass(const UClass* Class) const;

		/**
		 * The text name for a node type under a schema's namespaces: the shortest form that resolves back to exactly
		 * this type (`Walk` under the Villager namespace, `Villager.Walk` elsewhere, `StateTreeDelayTask` when `Delay`
		 * would be ambiguous).
		 */
		FString GetWrittenName(const FDstNodeType& Type, TConstArrayView<FString> Namespaces, TConstArrayView<EDstNodeCategory> Categories = {}) const;

		/**
		 * The kinds a name is resolved among where a node of this category is written: a task or an evaluator both
		 * (Globals takes either), any other kind alone.
		 */
		static TArray<EDstNodeCategory> GetNamingCategories(EDstNodeCategory Category);

		/** Schema classes a written schema name can mean. */
		TArray<UClass*> ResolveSchema(const FString& Written) const;
		/** The text name of a schema class. */
		FString GetSchemaName(const UClass* SchemaClass) const;
		/** The schema's default namespaces (meta DreamStateTreeNamespace), in order. */
		TArray<FString> GetSchemaNamespaces(const UClass* SchemaClass) const;

		/** The short name rule on its own. */
		static FString MakeShortName(const FString& StructName);
		/** Every type, for listings and the VS Code extension's completion. */
		const TArray<FDstNodeType>& GetAllTypes() const { return Types; }

	private:
		void Build();

		TArray<FDstNodeType> Types;
		/** Lower-case written name -> indices into Types. */
		TMultiMap<FString, int32> ByName;
		TMap<const UScriptStruct*, int32> ByStruct;
		TMap<FString, int32> ByBlueprintPath;
		bool bBuilt = false;
	};
}
