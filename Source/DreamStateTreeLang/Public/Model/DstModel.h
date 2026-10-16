// Copyright DreamMoon. All Rights Reserved.
//
// The model: what a StateTree authored in DreamStateTree holds, independent of how the text spells it and of the
// editor's object graph. Everything in the plugin meets here:
//
//   text --parse--> syntax tree --(semantic check, lowering)--> model --(generate)--> StateTree asset
//   StateTree asset --(extract)--> model --(raise)--> syntax tree --print--> text      (the decompiler)
//
// and the write-back compares the model extracted from an edited asset with the model the text produces.
//
// The model is engine-shaped: node types are struct paths, property names are the engine's, conditions are a flat
// list with operand and indent, comparisons are Compare nodes and arithmetic is property functions. The text's
// conveniences (`a < b`, `1 + Params.X`, short names, schema namespaces, tag prefixes, templates) are applied on the
// way in and recovered on the way out, so the generator and the extractor stay literal.
//
// No IDs live here. Identity is the state path and, inside a state, the node's role and index: the generator derives
// deterministic GUIDs from them, and the extractor reads them back the same way. Constant values are the engine's
// export text of the property (FProperty::ExportText), so two models are equal when their dumps are equal.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamStateTree::Model
{
	/**
	 * The InstanceType of the segment that enters the parameters of a reference to another tree
	 * (`StateTree.Parameters.Value<PropertyBag>.Target`, FStateTreeReference). The engine makes that struct out of the
	 * referenced tree's parameters and names it after a hash of their IDs, so the model names no struct: the generator
	 * takes the one the reference holds.
	 */
	inline constexpr const TCHAR* PropertyBagInstanceType = TEXT("PropertyBag");

	/** One step of a property path: `Payload`, `Items[2]`, a struct member. */
	struct FDstPathSegment
	{
		FString Name;
		/** INDEX_NONE when the segment is not an array element. */
		int32 ArrayIndex = INDEX_NONE;
		/** For an instanced struct or object, the type the segment holds (struct or class path); empty otherwise. */
		FString InstanceType;

		bool operator==(const FDstPathSegment& Other) const = default;
	};

	/** A property path below a binding source or a node: `Payload.Amount`, `Location.X`. */
	struct FDstPropertyPath
	{
		TArray<FDstPathSegment> Segments;

		bool IsEmpty() const { return Segments.IsEmpty(); }
		/** `Payload<FDstTestPayload>.Amount`, `Items[2].X`. */
		DREAMSTATETREELANG_API FString ToString() const;
		/** Parses what ToString writes. */
		DREAMSTATETREELANG_API static FDstPropertyPath FromString(const FString& Text);

		bool operator==(const FDstPropertyPath& Other) const = default;
	};

	/** What a binding reads from. */
	enum class EDstSourceKind : uint8
	{
		None,
		/** The tree's parameters. */
		Parameters,
		/** A context data of the schema; Owner is the context's name. */
		Context,
		/** A global task or evaluator; Owner is its node key. */
		Global,
		/** A task, condition or consideration of a state, or a property function; Owner is its node key. */
		Node,
		/** A state's parameters (a subtree's, or a linked state's); Owner is the state path. */
		StateParameters,
		/** The event a state requires (`on`); Owner is the state path. The path starts at the event struct (`Payload...`). */
		StateEvent,
		/** The event of an event transition; Owner is `<state path>#<transition index>`. */
		TransitionEvent,
	};
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstSourceKind Kind);

	struct FDstSourceRef
	{
		EDstSourceKind Kind = EDstSourceKind::None;
		FString Owner;
		FDstPropertyPath Path;

		bool IsSet() const { return Kind != EDstSourceKind::None; }
		/** `Parameters:Speed`, `Node:Root/Attack/Task[0]:Result`, `StateEvent:Root/Hurt:Payload<...>.Amount`. */
		DREAMSTATETREELANG_API FString ToString() const;
	};

	struct FDstNode;

	/** How a node property gets its value. */
	enum class EDstValueKind : uint8
	{
		/** ConstantText. */
		Constant,
		/** Copied from Source every update. */
		Binding,
		/** Computed by a property function node (Function), whose own properties may bind further. */
		Function,
		/** A property reference (FStateTreePropertyRef) pointing at Source. */
		Ref,
		/** An output binding: the node's property is copied into Source. */
		Output,
	};
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstValueKind Kind);

	/** Which struct of a node a property lives in. */
	enum class EDstPropertySide : uint8
	{
		/** The instance data (or the instance object of a Blueprint node). */
		Instance,
		/** The node struct itself (`Operator`, `bInvert`, `Name`, `bTaskEnabled`). */
		Node,
	};

	/**
	 * One property of a node that differs from its default or is fed from elsewhere.
	 *
	 * A constant is the export text of a whole top-level property. When parts of that property are fed from elsewhere
	 * (`Interval.Min`, `QueryConfig[1].Value`, a parameter of a referenced tree) each part has an argument of its own with
	 * the longer path, and the constant holds those parts at the default a text gives a field it leaves out: a struct's
	 * default (FLinearColor and FColor opaque), an array element's default. Whatever an asset holds under a binding is
	 * overwritten by it, and the text has no place for it.
	 */
	struct FDstArg
	{
		EDstPropertySide Side = EDstPropertySide::Instance;
		/** The property, from the side's root: `Duration`, `Interval.Min`, `QueryConfig[1].Value`. */
		FDstPropertyPath Property;
		EDstValueKind Kind = EDstValueKind::Constant;
		/** Constant only: the engine's export text. */
		FString ConstantText;
		/** Binding, Ref and Output. */
		FDstSourceRef Source;
		/** Function only. */
		TSharedPtr<FDstNode> Function;
	};

	enum class EDstNodeRole : uint8
	{
		Task,
		EnterCondition,
		Consideration,
		TransitionCondition,
		Evaluator,
		GlobalTask,
		PropertyFunction,
	};
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstNodeRole Role);

	/** ExpressionOperand of conditions and considerations. The first of a list is always Copy. */
	enum class EDstOperand : uint8 { Copy, And, Or, Multiply };
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstOperand Operand);

	struct FDstNode
	{
		/**
		 * Unique within the tree, and stable: `Root/Attack/Task[1]`, `Root/Attack/EnterCondition[0]`,
		 * `Root/Attack/Transition[2]/Condition[0]`, `Global[0]`, `<target node key>/Fn:<property path>` for a property
		 * function. The generator derives the node's GUID from it.
		 */
		FString Key;
		EDstNodeRole Role = EDstNodeRole::Task;
		/** The node struct (`/Script/StateTreeModule.StateTreeDelayTask`) or, for a Blueprint node, the wrapper struct. */
		FString StructType;
		/** A Blueprint node's class path; empty for C++ nodes. */
		FString BlueprintClass;
		/**
		 * The node's Name in the engine, which is also the name the text binds to (`Combo` in `do Combo = ComboAttack()`).
		 * Empty when the node has none; a node something binds to always gets one (the decompiler makes one up from
		 * its type when the asset has none).
		 */
		FString BindName;
		/** Conditions and considerations. */
		EDstOperand Operand = EDstOperand::Copy;
		int32 Indent = 0;
		/** Properties that differ from the defaults or are fed from elsewhere, in a stable order (Side, then property path). */
		TArray<FDstArg> Args;
	};

	/** A parameter of the tree or of a state (subtree signature, linked state override). */
	struct FDstParameter
	{
		FString Name;
		/**
		 * The property bag type, canonical: `bool`, `int32`, `int64`, `float`, `double`, `Name`, `String`, `Text`,
		 * `Enum:<path>`, `Struct:<path>`, `Object:<class path>`, `SoftObject:<class path>`, `Class:<class path>`,
		 * `SoftClass:<class path>`, and `Array<...>` / `Set<...>` around any of them.
		 */
		FString Type;
		/** The default (or overriding) value as export text; empty for the type's zero value. */
		FString ValueText;
		/** A linked state's parameter whose value overrides the subtree's. */
		bool bOverridden = false;
		/** Property metadata worth keeping (Category, Tooltip). */
		TMap<FString, FString> Meta;
	};

	struct FDstEvent
	{
		/** The full tag name. */
		FString Tag;
		/** The payload struct path; empty for none. */
		FString PayloadType;
		bool bConsumeOnSelect = true;
	};

	enum class EDstTransitionTrigger : uint8 { OnStateCompleted, OnStateSucceeded, OnStateFailed, OnTick, OnEvent, OnDelegate };
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstTransitionTrigger Trigger);

	enum class EDstTransitionTarget : uint8
	{
		GotoState, Succeeded, Failed, Parent, NextState, NextSelectableState, NextParent, NextSelectableParent, None,
	};
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstTransitionTarget Target);

	/** A task completion a delegate transition listens to. */
	enum class EDstCompletion : uint8 { None, Succeeds, Fails, Completes };

	struct FDstTransition
	{
		EDstTransitionTrigger Trigger = EDstTransitionTrigger::OnStateCompleted;
		EDstTransitionTarget TargetKind = EDstTransitionTarget::GotoState;
		/** GotoState only: the target's state path. */
		FString TargetState;
		/** OnEvent only. */
		FDstEvent Event;
		/** OnDelegate only: the dispatcher listened to (a node's delegate property), or a task completion (Owner = the task's key). */
		FDstSourceRef Delegate;
		EDstCompletion Completion = EDstCompletion::None;
		/** Low, Normal, Medium, High, Critical. */
		FString Priority = TEXT("Normal");
		bool bDelay = false;
		/** Export text of DelayDuration and DelayRandomVariance. */
		FString DelayText;
		FString VarianceText;
		bool bReactivate = false;
		bool bEnabled = true;
		TArray<FDstNode> Conditions;
	};

	enum class EDstStateType : uint8 { State, Group, Linked, LinkedAsset, Subtree };
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstStateType Type);

	struct FDstState
	{
		FString Name;
		/** `Root/Attack/Combo`; a subtree is its own root. Filled by FDstTree::UpdatePaths. */
		FString Path;
		EDstStateType Type = EDstStateType::State;
		/** EStateTreeStateSelectionBehavior value name: TrySelectChildrenInOrder, TryEnterState, ... */
		FString Selection = TEXT("TrySelectChildrenInOrder");
		/** Any (the engine's default: the state completes when any of its tasks does) or All. */
		FString TasksCompletion = TEXT("Any");
		FString Tag;
		FString ColorName;
		FString Description;
		bool bEnabled = true;
		/** Export text of Weight; empty for the default. */
		FString WeightText;
		bool bCustomTickRate = false;
		FString TickRateText;
		bool bCopyParameterBindingsOnTick = false;
		bool bCheckPrerequisitesWhenActivatingChildDirectly = true;
		TOptional<FDstEvent> Event;
		/** Linked: the subtree's state path. LinkedAsset: the asset's object path. */
		FString LinkTarget;
		/** Subtree: its signature. Linked and LinkedAsset: the target's parameters, overridden ones marked. */
		TArray<FDstParameter> Parameters;
		/**
		 * Linked and LinkedAsset: parameters fed from elsewhere (`link Hit(Toughness = 1 + Params.Courage)`), as
		 * bindings or property functions into the state's parameters. Side is Instance and Property starts with the
		 * parameter's name; the binding target is the state's parameters (`<state path>/Parameters` is the key a
		 * property function's own key starts with: `Root/Hit/Parameters/Fn:Toughness`).
		 */
		TArray<FDstArg> ParameterBindings;
		bool bFixedLayout = false;
		TArray<FDstNode> EnterConditions;
		TArray<FDstNode> Considerations;
		TArray<FDstNode> Tasks;
		TArray<FDstTransition> Transitions;
		TArray<FDstState> Children;
	};

	struct FDstColor
	{
		FString Name;
		/** Export text of the FLinearColor. */
		FString ColorText;
	};

	struct FDstTree
	{
		/** The asset's object path, `/Game/AI/ST_Brawler.ST_Brawler`. */
		FString AssetPath;
		/** The schema class path. */
		FString SchemaClass;
		/** Schema properties set by the text (context classes), by property name, as export text. */
		TMap<FString, FString> SchemaProperties;
		/** Other editor data settings by property name (GlobalTasksCompletion), as export text. */
		TMap<FString, FString> Settings;
		TArray<FDstParameter> Parameters;
		/** Evaluators and global tasks, in editor order (evaluators first). */
		TArray<FDstNode> Globals;
		TArray<FDstColor> Colors;
		/** Top-level states: the root first, then subtrees. */
		TArray<FDstState> States;

		/** Fills every state's Path from the hierarchy. */
		DREAMSTATETREELANG_API void UpdatePaths();
		/** The state at a path, or null. */
		DREAMSTATETREELANG_API const FDstState* FindState(const FString& Path) const;
		DREAMSTATETREELANG_API FDstState* FindState(const FString& Path);
		/** The node with a key anywhere in the tree (property functions included), or null. */
		DREAMSTATETREELANG_API const FDstNode* FindNode(const FString& Key) const;
	};

	struct FDstDumpOptions
	{
		/** Print node keys. Off to compare trees whose structure differs only in where a node sits. */
		bool bKeys = true;
	};

	/**
	 * Every field of the model, one item per line, in a fixed order: the same model always dumps to the same text, and
	 * two models are equal exactly when their dumps are. The corpus round trips compare these.
	 */
	DREAMSTATETREELANG_API FString DumpDstModel(const FDstTree& Tree, const FDstDumpOptions& Options = FDstDumpOptions());
}
