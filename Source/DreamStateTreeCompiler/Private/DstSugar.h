// Copyright DreamMoon. All Rights Reserved.
//
// The dictionary the semantic stage (DstLower) and the raise (DstRaise) share: every convenience of the text and the
// engine construct it stands for. Lowering applies an entry, raising recognises it; keeping both in one table is what
// makes `text -> model -> text` a fixed point. Change an entry here and both directions follow.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamStateTree::Compiler::Sugar
{
	// ------------------------------------------------------------------------------------------------------------
	// Comparisons in conditions: `a < b`, `a == b`, a lone bool path, `not C`
	// ------------------------------------------------------------------------------------------------------------

	/** Which Compare node a comparison becomes, by the type of its left side. */
	struct FCompareNode
	{
		/** `/Script/StateTreeModule.StateTreeCompareFloatCondition`. */
		const TCHAR* StructPath;
		/** Instance data properties: the left side (Category Input: always a binding) and the right side. */
		const TCHAR* Left;
		const TCHAR* Right;
		/** Node property holding the operator; null when the node only tests equality (Bool, Enum, Name: `!=` is bInvert). */
		const TCHAR* OperatorProperty;
	};

	inline constexpr FCompareNode CompareFloat = { TEXT("/Script/StateTreeModule.StateTreeCompareFloatCondition"), TEXT("Left"), TEXT("Right"), TEXT("Operator") };
	inline constexpr FCompareNode CompareInt = { TEXT("/Script/StateTreeModule.StateTreeCompareIntCondition"), TEXT("Left"), TEXT("Right"), TEXT("Operator") };
	inline constexpr FCompareNode CompareBool = { TEXT("/Script/StateTreeModule.StateTreeCompareBoolCondition"), TEXT("bLeft"), TEXT("bRight"), nullptr };
	inline constexpr FCompareNode CompareEnum = { TEXT("/Script/StateTreeModule.StateTreeCompareEnumCondition"), TEXT("Left"), TEXT("Right"), nullptr };
	inline constexpr FCompareNode CompareName = { TEXT("/Script/StateTreeModule.StateTreeCompareNameCondition"), TEXT("Left"), TEXT("Right"), nullptr };

	/** Node property inverting any condition that has it. */
	inline constexpr const TCHAR* InvertProperty = TEXT("bInvert");

	/**
	 * Comparison spellings and the UE::StateTree::EComparisonOperator value names they mean. Only these six. A float
	 * or int comparison writes the operator; `==` / `!=` on bool, enum and FName use the equality node, `!=` setting
	 * bInvert. Lowering puts the bound side on the left (Left is an Input property); a constant written on the left is
	 * swapped and the operator mirrored (`3 < x` -> `x > 3`). Two constants is an error.
	 */
	struct FComparisonSpelling
	{
		const TCHAR* Spelling;
		const TCHAR* OperatorName;
		/** The operator with its sides swapped. */
		const TCHAR* MirroredSpelling;
	};
	inline constexpr FComparisonSpelling Comparisons[] = {
		{ TEXT("<"), TEXT("Less"), TEXT(">") },
		{ TEXT("<="), TEXT("LessOrEqual"), TEXT(">=") },
		{ TEXT("=="), TEXT("Equal"), TEXT("==") },
		{ TEXT("!="), TEXT("NotEqual"), TEXT("!=") },
		{ TEXT(">="), TEXT("GreaterOrEqual"), TEXT("<=") },
		{ TEXT(">"), TEXT("Greater"), TEXT("<") },
	};

	/**
	 * A lone bool path as a condition (`if Player.Sees`) is a CompareBool with bLeft bound to the path and bRight
	 * true; `not Player.Sees` sets bInvert. Raising writes a CompareBool against a constant true back as the bare path.
	 */

	// ------------------------------------------------------------------------------------------------------------
	// Arithmetic and logic in arguments: property functions
	// ------------------------------------------------------------------------------------------------------------

	/** A binary property function: `Left <op> Right` into Result. */
	struct FBinaryFunction
	{
		const TCHAR* Spelling;
		/** Picked by the value type of the property being fed: float/double -> Float, int32 -> Int, bool -> Boolean. */
		const TCHAR* StructPath;
		const TCHAR* Left;
		const TCHAR* Right;
		const TCHAR* Result;
	};
	inline constexpr FBinaryFunction FloatFunctions[] = {
		{ TEXT("+"), TEXT("/Script/StateTreeModule.StateTreeAddFloatPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
		{ TEXT("-"), TEXT("/Script/StateTreeModule.StateTreeSubtractFloatPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
		{ TEXT("*"), TEXT("/Script/StateTreeModule.StateTreeMultiplyFloatPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
		{ TEXT("/"), TEXT("/Script/StateTreeModule.StateTreeDivideFloatPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
	};
	inline constexpr FBinaryFunction IntFunctions[] = {
		{ TEXT("+"), TEXT("/Script/StateTreeModule.StateTreeAddIntPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
		{ TEXT("-"), TEXT("/Script/StateTreeModule.StateTreeSubtractIntPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
		{ TEXT("*"), TEXT("/Script/StateTreeModule.StateTreeMultiplyIntPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
		{ TEXT("/"), TEXT("/Script/StateTreeModule.StateTreeDivideIntPropertyFunction"), TEXT("Left"), TEXT("Right"), TEXT("Result") },
	};
	inline constexpr FBinaryFunction BooleanFunctions[] = {
		{ TEXT("and"), TEXT("/Script/StateTreeModule.StateTreeBooleanAndPropertyFunction"), TEXT("bLeft"), TEXT("bRight"), TEXT("bResult") },
		{ TEXT("or"), TEXT("/Script/StateTreeModule.StateTreeBooleanOrPropertyFunction"), TEXT("bLeft"), TEXT("bRight"), TEXT("bResult") },
	};

	/** A unary property function: `<op> Input` into Result. */
	struct FUnaryFunction
	{
		const TCHAR* Spelling;
		const TCHAR* StructPath;
		const TCHAR* Input;
		const TCHAR* Result;
	};
	inline constexpr FUnaryFunction UnaryFunctions[] = {
		{ TEXT("-"), TEXT("/Script/StateTreeModule.StateTreeInvertFloatPropertyFunction"), TEXT("Input"), TEXT("Result") },
		{ TEXT("-"), TEXT("/Script/StateTreeModule.StateTreeInvertIntPropertyFunction"), TEXT("Input"), TEXT("Result") },
		{ TEXT("not"), TEXT("/Script/StateTreeModule.StateTreeBooleanNotPropertyFunction"), TEXT("bInput"), TEXT("bResult") },
	};

	/**
	 * Every other property function (AbsoluteFloat, MakeInterval, BreakTransform, GetActorLocation, a game's own) is
	 * written as a call: `Location = GetActorLocation(Player.Pawn)`. Its output property is the one named Result,
	 * bResult or Output, or the only Output-category property; the raise writes it as a call likewise.
	 * A side of an operator that is a constant becomes that side's value on the function node; a path becomes a binding.
	 * A whole expression of constants is folded to one constant instead.
	 */

	// ------------------------------------------------------------------------------------------------------------
	// Utility
	// ------------------------------------------------------------------------------------------------------------

	/** `utility 0.5`: a Constant consideration holding the number. */
	inline constexpr const TCHAR* ConstantConsideration = TEXT("/Script/StateTreeModule.StateTreeConstantConsideration");
	inline constexpr const TCHAR* ConstantConsiderationValue = TEXT("Constant");

	// ------------------------------------------------------------------------------------------------------------
	// Words of clauses and settings
	// ------------------------------------------------------------------------------------------------------------

	struct FWord
	{
		const TCHAR* Written;
		const TCHAR* Engine;
	};

	/** `pick <word>` -> EStateTreeStateSelectionBehavior. The default (InOrder) is not printed. */
	inline constexpr FWord PickWords[] = {
		{ TEXT("InOrder"), TEXT("TrySelectChildrenInOrder") },
		{ TEXT("Random"), TEXT("TrySelectChildrenAtRandom") },
		{ TEXT("UniformRandom"), TEXT("TrySelectChildrenAtUniformRandom") },
		{ TEXT("HighestUtility"), TEXT("TrySelectChildrenWithHighestUtility") },
		{ TEXT("WeightedUtility"), TEXT("TrySelectChildrenAtRandomWeightedByUtility") },
		{ TEXT("RelativeUtility"), TEXT("TrySelectChildrenBasedOnRelativeUtility") },
		{ TEXT("FollowTransitions"), TEXT("TryFollowTransitions") },
		{ TEXT("Enter"), TEXT("TryEnterState") },
		{ TEXT("None"), TEXT("None") },
	};

	/** `-> X on <word>` -> EStateTreeTransitionTrigger. No `on` is OnStateCompleted. A tag or `Tag(Payload)` is OnEvent; `Delegate(...)` is OnDelegate. */
	inline constexpr FWord TriggerWords[] = {
		{ TEXT("Completed"), TEXT("OnStateCompleted") },
		{ TEXT("Succeeded"), TEXT("OnStateSucceeded") },
		{ TEXT("Failed"), TEXT("OnStateFailed") },
		{ TEXT("Tick"), TEXT("OnTick") },
	};

	/** `-> <word>` -> EStateTreeTransitionType. Any other target is GotoState to the state of that name or path. */
	inline constexpr FWord TargetWords[] = {
		{ TEXT("Next"), TEXT("NextState") },
		{ TEXT("NextSelectable"), TEXT("NextSelectableState") },
		{ TEXT("Parent"), TEXT("Parent") },
		{ TEXT("NextParent"), TEXT("NextParent") },
		{ TEXT("NextSelectableParent"), TEXT("NextSelectableParent") },
		{ TEXT("TreeSucceeded"), TEXT("Succeeded") },
		{ TEXT("TreeFailed"), TEXT("Failed") },
	};

	/** `Delegate(Task.<word>)`: a task completion a transition listens to. */
	inline constexpr FWord CompletionWords[] = {
		{ TEXT("Succeeds"), TEXT("Succeeds") },
		{ TEXT("Fails"), TEXT("Fails") },
		{ TEXT("Completes"), TEXT("Completes") },
	};

	/** Transition options `[ ... ]` -> FStateTreeTransition fields. Delay and Variance also set bDelayTransition. */
	inline constexpr FWord TransitionOptions[] = {
		{ TEXT("Priority"), TEXT("Priority") },
		{ TEXT("Delay"), TEXT("DelayDuration") },
		{ TEXT("Variance"), TEXT("DelayRandomVariance") },
		{ TEXT("Reactivate"), TEXT("ReactivateTargetState") },
		{ TEXT("Enabled"), TEXT("bTransitionEnabled") },
		{ TEXT("ConsumeEvent"), TEXT("bConsumeEventOnSelect") },
	};

	/** State settings `State X [ ... ]` -> UStateTreeState fields (any other key is a UStateTreeState property of that name). */
	inline constexpr FWord StateSettings[] = {
		{ TEXT("Tag"), TEXT("Tag") },
		{ TEXT("Color"), TEXT("ColorRef") },
		{ TEXT("Description"), TEXT("Description") },
		{ TEXT("Enabled"), TEXT("bEnabled") },
		{ TEXT("Complete"), TEXT("TasksCompletion") },
		{ TEXT("TickRate"), TEXT("CustomTickRate") },
		{ TEXT("Weight"), TEXT("Weight") },
		{ TEXT("Group"), TEXT("Type") },
		{ TEXT("ConsumeEvent"), TEXT("bConsumeEventOnSelect") },
	};

	/** Node settings `Call(...) [ ... ]` -> node struct fields (any other key is a node struct property of that name). */
	inline constexpr FWord NodeSettings[] = {
		{ TEXT("Name"), TEXT("Name") },
		{ TEXT("Enabled"), TEXT("bTaskEnabled") },
		{ TEXT("ConsideredForCompletion"), TEXT("bConsideredForCompletion") },
		{ TEXT("TransitionPriority"), TEXT("TransitionHandlingPriority") },
		{ TEXT("Evaluation"), TEXT("EvaluationMode") },
	};

	// ------------------------------------------------------------------------------------------------------------
	// Positional arguments of engine nodes (a game's own nodes declare theirs with meta DreamStateTreeArgs)
	// ------------------------------------------------------------------------------------------------------------

	struct FPositional
	{
		const TCHAR* StructPath;
		/** Comma-separated property names, in order. */
		const TCHAR* Properties;
	};
	inline constexpr FPositional EnginePositionalArgs[] = {
		{ TEXT("/Script/StateTreeModule.StateTreeDelayTask"), TEXT("Duration,RandomDeviation") },
		{ TEXT("/Script/StateTreeModule.StateTreeDebugTextTask"), TEXT("Text") },
		{ TEXT("/Script/StateTreeModule.StateTreeRandomCondition"), TEXT("Threshold") },
		{ TEXT("/Script/StateTreeModule.StateTreeObjectIsValidCondition"), TEXT("Object") },
		{ TEXT("/Script/StateTreeModule.StateTreeFloatInputConsideration"), TEXT("Input,Interval") },
		{ TEXT("/Script/StateTreeModule.StateTreeConstantConsideration"), TEXT("Constant") },
		{ TEXT("/Script/GameplayStateTreeModule.StateTreeMoveToTask"), TEXT("Destination") },
		{ TEXT("/Script/GameplayStateTreeModule.StateTreeRunEnvQueryTask"), TEXT("QueryTemplate") },
		{ TEXT("/Script/GameplayStateTreeModule.StateTreeGetActorLocationPropertyFunction"), TEXT("Input") },
	};
}
