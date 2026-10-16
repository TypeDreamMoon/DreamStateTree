// Copyright DreamMoon. All Rights Reserved.
//
// What the files of the raise (DstRaise*.cpp) share: the raiser class, the scope a node is raised in, and the builders
// of syntax tree nodes. The raise is the exact inverse of the semantic stage (DstLower*.cpp): every convenience the
// lowering applies is recovered here with the same tables (DstSugar.h), so that the text written for a model lowers
// back to that model.
//
// Reading order of the files:
//   DstRaise.cpp            RaiseDst, the builders, the header, the sections, states, clauses and transitions
//   DstRaiseNode.cpp        node calls: names, arguments, settings, values, binding paths, property functions
//   DstRaiseCondition.cpp   condition and consideration lists: operands and indents back to `and` / `or` / `*`, comparisons

#pragma once

#include "CoreMinimal.h"
#include "DstLower.h"
#include "DstLowerPrivate.h"
#include "DstNames.h"
#include "DstRaise.h"
#include "DstSugar.h"
#include "DstValues.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Model/DstModel.h"
#include "UObject/StructOnScope.h"

class FProperty;
class UClass;
class UScriptStruct;
class UStruct;

namespace UE::DreamStateTree::Compiler::Raising
{
	using FExprPtr = Lang::FDstExprPtr;
	using FModelNode = Model::FDstNode;
	using FModelArg = Model::FDstArg;
	using FModelState = Model::FDstState;
	using FModelTree = Model::FDstTree;

	// ------------------------------------------------------------------------------------------
	// Syntax tree builders (DstRaise.cpp)
	// ------------------------------------------------------------------------------------------

	/** A name; between backticks (bQuoted) when it is not a plain identifier or when the caller forces it. */
	FExprPtr MakeNameExpr(const FString& Name, bool bForceQuoted = false);
	/** `A.B.C`. The first segment can be forced into backticks. */
	FExprPtr MakePathExpr(TConstArrayView<FString> Segments, bool bQuoteFirst = false);
	FExprPtr MakeStringExpr(const FString& Text);
	FExprPtr MakeBoolExpr(bool bValue);
	/** A number; a negative one is a minus in front of its digits, as the parser reads it. */
	FExprPtr MakeNumberExpr(double Value);
	Lang::FDstArgumentPtr MakeArgument(const FString& Name, FExprPtr Value, bool bOutput = false);
	FExprPtr MakeParenExpr(FExprPtr Inner);
	/** `not X`, `-X`; a binary operand gets its parentheses. */
	FExprPtr MakeUnaryExpr(Lang::EDstUnaryOp Op, FExprPtr Operand);
	/** `L op R` with the parentheses the text's precedence needs (the operators associate to the left). */
	FExprPtr MakeBinaryExpr(Lang::EDstBinaryOp Op, FExprPtr Lhs, FExprPtr Rhs);
	/** Adds `Key = Value` (a flag when Value is null) to Meta, making it when needed. */
	void AddMetaEntry(TUniquePtr<Lang::FDstMeta>& Meta, const FString& Key, FExprPtr Value);

	// ------------------------------------------------------------------------------------------
	// The raiser
	// ------------------------------------------------------------------------------------------

	/** Where a node sits, which decides how paths to events are written. */
	struct FScope
	{
		/** The states from the top-level one down to the one being raised; empty for the globals. */
		TArray<const FModelState*> Chain;
		/** `<state path>#<index>` of the event transition whose conditions are being raised; empty otherwise. */
		FString TransitionEventOwner;
		/** The arguments of a `link`: evaluated before the state's own event exists. */
		bool bLinkParameter = false;
	};

	/** A node type with what the raise needs to know about it, built once per type. */
	struct FNodeInfo
	{
		const FDstNodeType* Type = nullptr;
		const UScriptStruct* NodeStruct = nullptr;
		/** A UScriptStruct for a C++ node, the generated UClass for a Blueprint node; null for a node without data. */
		const UStruct* InstanceStruct = nullptr;
		bool bBlueprint = false;
		TSharedPtr<FStructOnScope> NodeDefaults;
		TSharedPtr<FStructOnScope> InstanceDefaults;
		const UObject* InstanceCDO = nullptr;
		/** The callee as the text spells it; empty for a type that cannot be named. */
		FString WrittenName;
		/** For messages: the written name or the struct's name. */
		FString DisplayName;
		/** The written name is not a name that resolves back to this type alone; the callee is a Path(...) then. */
		bool bUseAssetCallee = false;
		/** The Blueprint asset's object path (for the Path(...) callee). */
		FString BlueprintAssetPath;

		const void* GetInstanceDefaults() const;
		const void* GetNodeDefaults() const;
	};
	using FNodeInfoPtr = TSharedPtr<FNodeInfo>;

	/** How a call is written. */
	struct FCallStyle
	{
		/** Leave bInvert out of the settings: the caller writes it as `not`. */
		bool bSkipInvert = false;
		/** The node's Name goes into its settings (conditions, considerations, property functions); a task or a global carries it outside. */
		bool bNameInSettings = true;
	};

	/** What a source of a binding ends in, for choosing between nodes the way the lowering does. */
	struct FSourceLeaf
	{
		/** The path was followed to a property; false when it could not be (or names a whole context object). */
		bool bKnown = false;
		Lowering::EValueDomain Domain = Lowering::EValueDomain::Other;
		/** The enum of an enum property; null for any other (a FStateTreeAnyEnum has none). */
		const UEnum* Enum = nullptr;
	};

	class FDstRaiser
	{
	public:
		FDstRaiser(const FModelTree& InTree, const FDstRaiseOptions& InOptions, Lang::FDstDiagnosticSink& InSink);

		TUniquePtr<Lang::FDstFile> Run();

		// ---- DstRaise.cpp: the file
		void ResolveSchema();
		void BuildHeader(Lang::FDstDocument& Document);
		FExprPtr MakeSchemaExpr();
		void RaiseTagsSection(Lang::FDstDocument& Document);
		void RaiseSettingsSection(Lang::FDstDocument& Document);
		void RaiseParametersSection(Lang::FDstDocument& Document);
		void RaiseGlobalsSection(Lang::FDstDocument& Document);
		void RaiseColorsSection(Lang::FDstDocument& Document);

		/** The text spelling of a canonical parameter type (`float`, `Vector`, `Array<int>`); verified against the lowering's reader. */
		FString MakeParameterTypeText(const Model::FDstParameter& Parameter);
		/** `Name = default` declarations for a parameter list: the type text, the default and the metadata. */
		FExprPtr MakeParameterDefault(const Model::FDstParameter& Parameter, const FProperty* Property);

		// ---- DstRaise.cpp: states
		TUniquePtr<Lang::FDstStateStmt> RaiseState(const FModelState& State, TArray<const FModelState*>& Chain);
		TUniquePtr<Lang::FDstMeta> MakeStateSettings(const FModelState& State);
		TUniquePtr<Lang::FDstClauseStmt> MakeEventClause(const Model::FDstEvent& Event);
		TUniquePtr<Lang::FDstClauseStmt> MakeLinkClause(const FModelState& State, const FScope& Scope);
		TUniquePtr<Lang::FDstClauseStmt> RaiseTransition(const FModelState& State, int32 Index, const FScope& Scope);
		FExprPtr MakeEventTriggerExpr(const Model::FDstEvent& Event);
		FExprPtr MakeDelegateTriggerExpr(const Model::FDstTransition& Transition, const FScope& Scope);
		FExprPtr MakeStateTargetExpr(const FString& FromPath, const FString& ToPath);
		FExprPtr MakeTagExpr(const FString& TagName);
		FExprPtr MakeStructNameExpr(const FString& StructPath);
		void CollectStatePaths(const TArray<FModelState>& States);

		// ---- DstRaiseNode.cpp: nodes
		FNodeInfoPtr GetNodeInfo(const FModelNode& Node);
		FExprPtr MakeCalleeExpr(const FModelNode& Node, const FNodeInfo& Info);
		/** One call for a node: its type's name, the arguments (positional where the type declares them) and the settings. */
		FExprPtr RaiseNodeCall(const FModelNode& Node, const FScope& Scope, const FCallStyle& Style);
		/** `Name = Call()` etc. for a task. */
		TUniquePtr<Lang::FDstTaskItem> RaiseTask(const FModelNode& Node, const FScope& Scope);
		/**
		 * The text of one top-level property of a node, from every argument that feeds it. Null (after reporting) when it cannot be
		 * written. DefaultContainer holds the property's default (null: the type's zero value).
		 */
		FExprPtr RaiseArgGroup(const FProperty* Property, const FString& PropertyName, const TArray<const FModelArg*>& Group,
			const FScope& Scope, bool& bOutOutput, const FString& Display, const void* DefaultContainer = nullptr);
		FExprPtr RaiseWholeArg(const FModelArg& Arg, const FProperty* Property, const FScope& Scope, bool& bOutOutput, const FString& Display);
		/** A struct, an array or a reference to a tree with parts fed from elsewhere: the constant, with those parts written in. */
		FExprPtr RaiseMixedArg(const FProperty* Property, const FModelArg* Whole, const TArray<const FModelArg*>& Deep, const FScope& Scope,
			const FString& Display, const void* DefaultContainer);
		/** Writes the value of Arg into the expression Slot holds for Property, at the place Segments[Index..] lead to. */
		bool InsertDeepArg(FExprPtr& Slot, const FProperty* Property, const TArray<Model::FDstPathSegment>& Segments, int32 Index,
			const FModelArg& Arg, const FScope& Scope, const FString& Display);
		/** A property function feeding Target: an operation when it is one of the sugar's, otherwise a call. */
		FExprPtr RaiseFunction(const FModelNode& Function, const FProperty* Target, const FScope& Scope);
		FExprPtr TryRaiseOperation(const FModelNode& Function, const FProperty* Target, const FScope& Scope);
		/** The literal of a constant; on failure DST8004 and the export text as a string. */
		FExprPtr MakeConstantExpr(const FString& ExportText, const FProperty* Property, const FString& What);
		/** A path to where a binding reads from (`Params.X`, `Info.Target`, `Event.Offer`). Null (after reporting) when it has no spelling. */
		FExprPtr RaiseSource(const Model::FDstSourceRef& Source, const FScope& Scope, const FString& What);
		/** The property a source ends in, when it can be told: the compare nodes' choice depends on its type. */
		FSourceLeaf ResolveSourceLeaf(const Model::FDstSourceRef& Source, const FScope& Scope);
		/** The name a node is bound by (`Info`, `Combo`); empty when it has none. */
		FString FindBindName(const FString& NodeKey) const;
		const FModelNode* FindNodeByKey(const FString& NodeKey) const;
		const FModelState* FindStateByPath(const FString& Path) const;
		const FModelState* FindNearestEventState(const FScope& Scope) const;
		void ReportOnce(const FString& Key, const TCHAR* Code, const FString& Message, const FString& Hint = FString(), bool bWarning = false);

		// ---- DstRaiseCondition.cpp: conditions
		/** The expression a condition or consideration list stands for; null for an empty list. */
		FExprPtr RaiseConditionList(const TArray<FModelNode>& Nodes, const FScope& Scope, bool bConsideration);
		FExprPtr RaiseConditionLeaf(const FModelNode& Node, const FScope& Scope, bool bConsideration);
		FExprPtr TryRaiseCompare(const FModelNode& Node, bool bInvert, const FScope& Scope);
		FExprPtr TryRaiseConstantConsideration(const FModelNode& Node);

		// ---- shared state
		const FModelTree& Tree;
		FDstRaiseOptions Options;
		Lang::FDstDiagnosticSink& Sink;
		FDstNameRegistry& Registry;

		const UClass* SchemaClass = nullptr;
		TArray<FString> Namespaces;
		/** The schema keeps one task per state and the engine names it after the state: the text does not write that name. */
		bool bSingleTaskSchema = false;
		FDstValueContext ValueContext;

		/** The tree's parameters as a property bag, for the literals of their defaults and the types of `Params.X`. */
		Lowering::FParamSet TreeParams;
		bool bTreeParamsBuilt = false;

		/** Every state's path split into names, for the shortest unique ending of a transition target. */
		TArray<TArray<FString>> AllStatePaths;

		TMap<FString, FNodeInfoPtr> NodeInfoCache;
		TSet<FString> Reported;
	};

	/** The struct at a path (loaded when it has to be); null when there is none. */
	const UScriptStruct* FindStructByPath(const FString& Path);
	const UClass* FindClassByPath(const FString& Path);

	/** Names the lowering reads as roots of a path, which a task or global node must not be called. */
	bool IsReservedRootName(const FString& Name);
}
