// Copyright DreamMoon. All Rights Reserved.
//
// What the files of the extractor (DstExtract*.cpp) share: the extractor class, the table that says what each struct ID of
// the editor data stands for, and the keys of model nodes.
//
// Reading order of the files:
//   DstExtract.cpp          ExtractDst; the tree: schema, settings, parameters, colors, states, transitions
//   DstExtractNode.cpp      one node: its type, name and the constants that differ from the default
//   DstExtractBinding.cpp   the table of struct IDs, bindings -> arguments, property functions, delegates, the event repair
//   DstExtractRepair.cpp    the standalone pieces: path conversion and the repair of event bindings broken by duplicating
//
// The extractor is literal: it writes what the editor data holds and none of the text's sugar. Everything it cannot
// write is reported (DST8xxx) instead of being dropped without a word.
//
// Ids written for the write-back (FDstExtractResult::Ids), the model key -> the GUID the asset holds:
//   `Root/Attack`                    the state                         `Root/Attack|Parameters`   its parameters struct
//   `Root/Attack/Param:Speed`        a state parameter's property      `Root/Attack#2`            the third transition
//   `Root/Attack/Task[1]`, ...       every node, property functions too (`<node key>/Fn:<property path>`)
//   `Parameters`                     the tree's parameters struct      `Param:Speed`              a tree parameter's property
//   `Color:Combat`                   a color

#pragma once

#include "CoreMinimal.h"
#include "DstExtract.h"
#include "DstExtractRepair.h"
#include "Lang/DstDiagnostic.h"
#include "Model/DstModel.h"
#include "UObject/Class.h"
#include "UObject/ReflectedTypeAccessors.h"

class FProperty;
class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;
class UStruct;
struct FInstancedPropertyBag;
struct FPropertyBindingPath;
struct FStateTreeEditorNode;
struct FStateTreePropertyPathBinding;
struct FStateTreeTransition;

namespace UE::DreamStateTree::Compiler::Extracting
{
	// ------------------------------------------------------------------------------------------
	// Keys
	// ------------------------------------------------------------------------------------------

	/** `Root/Attack` from the parent's path (null for a top-level state): the rule of FDstTree::UpdatePaths. */
	inline FString MakeChildPath(const FString* ParentPath, const FString& Name)
	{
		return ParentPath ? *ParentPath + TEXT("/") + Name : Name;
	}

	/** `Root/Attack/Task[1]`. */
	inline FString MakeStateNodeKey(const FString& StatePath, const TCHAR* RoleWord, const int32 Index)
	{
		return FString::Printf(TEXT("%s/%s[%d]"), *StatePath, RoleWord, Index);
	}

	/** `Root/Attack#2`: the transition, and the owner of its event. */
	inline FString MakeTransitionKey(const FString& StatePath, const int32 TransitionIndex)
	{
		return FString::Printf(TEXT("%s#%d"), *StatePath, TransitionIndex);
	}

	/** `Root/Attack/Transition[2]/Condition[0]`. */
	inline FString MakeConditionKey(const FString& StatePath, const int32 TransitionIndex, const int32 Index)
	{
		return FString::Printf(TEXT("%s/Transition[%d]/Condition[%d]"), *StatePath, TransitionIndex, Index);
	}

	/** `Global[0]`: evaluators first, then global tasks. */
	inline FString MakeGlobalKey(const int32 Index)
	{
		return FString::Printf(TEXT("Global[%d]"), Index);
	}

	/** The name of an enum value, as the text and the model write it. */
	template <typename EnumType>
	FString EnumToName(const EnumType Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			const FString Name = Enum->GetNameStringByValue(static_cast<int64>(Value));
			if (!Name.IsEmpty())
			{
				return Name;
			}
		}
		return FString::Printf(TEXT("%lld"), static_cast<int64>(Value));
	}

	/** The nodes of a list the engine would use: entries without a node struct are skipped (the compiler skips them too). */
	TArray<const FStateTreeEditorNode*> CollectNodes(const TArray<FStateTreeEditorNode>& List);

	/** The tasks the compiler compiles for a state: Tasks, or SingleTask when Tasks is empty. */
	TArray<const FStateTreeEditorNode*> CollectTasks(const UStateTreeState& State);

	// ------------------------------------------------------------------------------------------
	// What a struct ID stands for
	// ------------------------------------------------------------------------------------------

	enum class EStructKind : uint8
	{
		/** The tree's parameters. */
		TreeParameters,
		/** A context data of the schema; Owner is its name. */
		Context,
		/** A node's instance data (an evaluator, task, condition, consideration); Owner is the node key. */
		NodeInstance,
		/** A node's own struct, the template: the only thing a binding may read from it is a task's completion. */
		NodeTemplate,
		/** A state's parameters; Owner is the state path. */
		StateParameters,
		/** The event a state requires; Owner is the state path. */
		StateEvent,
		/** A transition (the target of its delegate listener); Owner is `<state path>#<index>`. */
		Transition,
		/** The event of an event transition; Owner is `<state path>#<index>`. */
		TransitionEvent,
	};

	struct FStructInfo
	{
		EStructKind Kind = EStructKind::NodeInstance;
		FString Owner;
		/** Nodes: an evaluator or global task, which the model calls Global. */
		bool bGlobal = false;
		/** Nodes: a task, the only kind whose completion can be listened to. */
		bool bTask = false;
	};

	/** Where a node sits, which decides the events a binding on it can read. */
	struct FNodeSite
	{
		/** Null for a global node. */
		const UStateTreeState* State = nullptr;
		/** The transition a transition condition belongs to; INDEX_NONE otherwise. */
		int32 TransitionIndex = INDEX_NONE;
	};

	// ------------------------------------------------------------------------------------------
	// The extractor
	// ------------------------------------------------------------------------------------------

	class FExtractor
	{
	public:
		FExtractor(const UStateTree& InAsset, Lang::FDstDiagnosticSink& InSink);

		/** The whole extraction. False when it reported an error. */
		bool Run(Model::FDstTree& OutTree, TMap<FString, FGuid>& OutIds);

	private:
		// ---- DstExtract.cpp: the tree ----
		bool CheckEditorData();
		void ExtractSchema();
		void ExtractExtensions();
		void ExtractSettings();
		void ExtractColors();
		void ExtractTreeParameters();
		void ExtractGlobals();
		void ExtractStates();
		void ExtractState(const UStateTreeState& State, Model::FDstState& Out);
		void ExtractTransition(const UStateTreeState& State, const FString& StatePath, int32 TransitionIndex,
			const FStateTreeTransition& Transition, Model::FDstTransition& Out);
		void ExtractParameters(const FInstancedPropertyBag& Bag, const UStateTreeState* OverridesOwner, const FString& Label,
			const FString& IdPrefix, TArray<Model::FDstParameter>& Out);
		/** Fills a node list. MakeKey gives the key of the node at an index; bExpression sets operands and indents. */
		void ExtractNodeList(const TArray<const FStateTreeEditorNode*>& Nodes, Model::EDstNodeRole Role, const FNodeSite& Site,
			bool bExpression, TFunctionRef<FString(int32)> MakeKey, TArray<Model::FDstNode>& Out);
		/** A state's path by its ID, for a link; empty (with DST8009) when the tree has no such state. */
		FString FindStatePath(const FGuid& StateId, const FString& What);

		// ---- DstExtractNode.cpp: nodes ----
		Model::FDstNode ExtractNode(const FStateTreeEditorNode& Editor, const FString& Key, Model::EDstNodeRole Role,
			const FNodeSite& Site, int32 Depth = 0);
		void ExtractConstants(const UStruct* Struct, const void* Actual, const void* Default, Model::EDstPropertySide Side,
			const TSet<FName>& Bound, const FString& NodeKey, const UScriptStruct* NodeStruct, TArray<Model::FDstArg>& OutArgs);

		// ---- DstExtractBinding.cpp: struct IDs and bindings ----
		void RegisterStructs();
		void RegisterStates(const TArray<TObjectPtr<UStateTreeState>>& States, const UStateTreeState* Parent, const FString* ParentPath);
		void RegisterNode(const FStateTreeEditorNode& Node, const FString& Key, bool bGlobal, bool bTask);
		void RegisterNodeList(const TArray<const FStateTreeEditorNode*>& Nodes, const FString& StatePath, const TCHAR* RoleWord, bool bTask);
		void AddStruct(const FGuid& Id, FStructInfo&& Info);
		void GroupBindings();
		/** Bindings that target the struct with this ID, in editor order; null when there are none. */
		const TArray<const FStateTreePropertyPathBinding*>* FindBindings(const FGuid& TargetId) const;
		/** Makes the argument of one binding that targets a node. False when it yields none (reported, or left out). */
		bool BuildBindingArg(const FStateTreePropertyPathBinding& Binding, Model::EDstPropertySide Side, const UStruct* TargetStruct,
			const FString& NodeKey, const FNodeSite& Site, int32 Depth, Model::FDstArg& OutArg);
		bool ResolveSource(const FPropertyBindingPath& Path, const FString& TargetText, const FNodeSite& Site, Model::FDstSourceRef& OutSource);
		TArray<FDstEventCandidate> CollectEventCandidates(const FNodeSite& Site) const;
		bool IsAutomaticContextBinding(const FStateTreePropertyPathBinding& Binding, const FStructInfo& SourceInfo, const UStruct* TargetStruct) const;
		void ExtractDelegate(const FStateTreeTransition& Transition, const FString& TransitionKey, const FNodeSite& Site,
			Model::FDstTransition& Out);
		/**
		 * The bindings and property functions that feed the parameters of a linked state, as the state's ParameterBindings.
		 * A function's key starts with `<state path>/Parameters`.
		 */
		void ExtractParameterBindings(const UStateTreeState& State, const FString& StatePath, Model::FDstState& Out);
		void ReportUnconsumedBindings();

		const UStateTree& Asset;
		Lang::FDstDiagnosticSink& Sink;
		const UStateTreeEditorData* Data = nullptr;
		Model::FDstTree* Tree = nullptr;
		TMap<FString, FGuid> Ids;

		/** What each struct ID stands for. */
		TMap<FGuid, FStructInfo> Structs;
		/** State ID -> state path, for links. */
		TMap<FGuid, FString> StatePathById;
		TMap<const UStateTreeState*, FString> StatePaths;
		TMap<const UStateTreeState*, const UStateTreeState*> ParentOf;
		/** Target struct ID -> the bindings that end in it, in editor order. */
		TMap<FGuid, TArray<const FStateTreePropertyPathBinding*>> BindingsByTarget;
		/** Bindings that became part of the model; the rest are reported at the end. */
		TSet<const FStateTreePropertyPathBinding*> Consumed;
	};
}
