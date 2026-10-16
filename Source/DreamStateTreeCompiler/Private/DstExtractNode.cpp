// Copyright DreamMoon. All Rights Reserved.
//
// One node of the editor data -> one FDstNode: the struct it is, the name it is bound by, and its arguments.
//
// Arguments are the properties that differ from the type's default, plus every binding into the node:
//   - the default of the node struct is a default-constructed instance of it, the default of an instance data struct
//     likewise, the default of a Blueprint node's instance object is the class default object;
//   - a constant is recorded on the top-level property it lives in, as the export text of the whole property
//     (DstExportDefault, the same function Lower uses), and only when the property differs from the default both as a
//     value and as text;
//   - a property that a binding feeds as a whole has no constant: the binding says everything;
//   - a property that bindings feed in parts (a member, an element, a parameter of a referenced tree) has a constant with those
//     parts at the default a text gives them, because whatever is stored under a binding is overwritten and the text cannot
//     say it (ExportDstValueWithBoundParts); a reference to a tree is also brought in step with its tree;
//   - what the engine keeps in the node for its own use is not an argument: the Name (it is the node's BindName), the
//     compile results, the operand and indent the compiler derives from the editor node's own, the class a Blueprint
//     wrapper holds (it is the node's BlueprintClass).
//
// Every constant is exported whole by DstExportDefault: `false`, `0` and a struct's zero fields included.

#include "DstExtractCommon.h"

#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "DstValues.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeDelegate.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Extracting
{
	using namespace Model;

	TArray<const FStateTreeEditorNode*> CollectNodes(const TArray<FStateTreeEditorNode>& List)
	{
		TArray<const FStateTreeEditorNode*> Result;
		for (const FStateTreeEditorNode& Node : List)
		{
			if (Node.Node.IsValid())
			{
				Result.Add(&Node);
			}
		}
		return Result;
	}

	TArray<const FStateTreeEditorNode*> CollectTasks(const UStateTreeState& State)
	{
		// The compiler decides on the raw count of Tasks, not on how many of its entries hold a node.
		if (State.Tasks.Num() > 0)
		{
			return CollectNodes(State.Tasks);
		}

		TArray<const FStateTreeEditorNode*> Result;
		if (State.SingleTask.Node.IsValid())
		{
			Result.Add(&State.SingleTask);
		}
		return Result;
	}

	namespace
	{
		/** The Blueprint class a node runs, from its instance object or, failing that, from the wrapper. */
		const UClass* GetBlueprintClass(const FStateTreeEditorNode& Editor)
		{
			if (Editor.InstanceObject)
			{
				return Editor.InstanceObject->GetClass();
			}
			if (const FStateTreeBlueprintTaskWrapper* Task = Editor.GetNode().GetPtr<FStateTreeBlueprintTaskWrapper>())
			{
				return Task->TaskClass.Get();
			}
			if (const FStateTreeBlueprintConditionWrapper* Condition = Editor.GetNode().GetPtr<FStateTreeBlueprintConditionWrapper>())
			{
				return Condition->ConditionClass.Get();
			}
			if (const FStateTreeBlueprintEvaluatorWrapper* Evaluator = Editor.GetNode().GetPtr<FStateTreeBlueprintEvaluatorWrapper>())
			{
				return Evaluator->EvaluatorClass.Get();
			}
			if (const FStateTreeBlueprintConsiderationWrapper* Consideration = Editor.GetNode().GetPtr<FStateTreeBlueprintConsiderationWrapper>())
			{
				return Consideration->ConsiderationClass.Get();
			}
			return nullptr;
		}

		bool IsBlueprintWrapper(const UStruct* Struct)
		{
			return Struct == FStateTreeBlueprintTaskWrapper::StaticStruct()
				|| Struct == FStateTreeBlueprintConditionWrapper::StaticStruct()
				|| Struct == FStateTreeBlueprintEvaluatorWrapper::StaticStruct()
				|| Struct == FStateTreeBlueprintConsiderationWrapper::StaticStruct();
		}

		/**
		 * Properties of a node struct that are not arguments. The node's settings (bTaskEnabled, TransitionHandlingPriority,
		 * bConsideredForCompletion, EvaluationMode, a condition's bInvert) are arguments of the node side.
		 */
		/** A struct or class the StateTree module declares as a base for nodes. */
		bool IsStateTreeBaseOwner(const UStruct* Owner)
		{
			if (!Owner)
			{
				return false;
			}
			static const TCHAR* const Names[] =
			{
				TEXT("StateTreeNodeBase"), TEXT("StateTreeTaskBase"), TEXT("StateTreeConditionBase"), TEXT("StateTreeConsiderationBase"),
				TEXT("StateTreeEvaluatorBase"), TEXT("StateTreePropertyFunctionBase"), TEXT("StateTreeNodeBlueprintBase"),
				TEXT("StateTreeTaskBlueprintBase"), TEXT("StateTreeConditionBlueprintBase"), TEXT("StateTreeEvaluatorBlueprintBase"),
				TEXT("StateTreeConsiderationBlueprintBase"),
			};
			const FString Name = Owner->GetName();
			for (const TCHAR* Known : Names)
			{
				if (Name.Equals(Known, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		bool IsInternalNodeProperty(const FProperty& Property, const UScriptStruct* NodeStruct)
		{
			const UStruct* Owner = Property.GetOwnerStruct();
			const FName Name = Property.GetFName();

			if (Owner == FStateTreeNodeBase::StaticStruct())
			{
				// Name is the BindName; the rest are the compiler's handles and indices.
				return true;
			}
			if (Owner == FStateTreeTaskBase::StaticStruct())
			{
				static const FName HasDispatcher(TEXT("bHasTaskCompletionDelegateDispatcher"));
				static const FName CanEditCompletion(TEXT("bCanEditConsideredForCompletion"));
				static const FName ConsideredForCompletion(TEXT("bConsideredForCompletion"));
				if (Name == HasDispatcher || Name == CanEditCompletion)
				{
					return true;
				}
				// A Blueprint task keeps this setting on its instance object; the wrapper's own copy is not used.
				return Name == ConsideredForCompletion && IsBlueprintWrapper(NodeStruct);
			}
			if (Owner == FStateTreeConditionBase::StaticStruct() || Owner == FStateTreeConsiderationBase::StaticStruct())
			{
				// Derived by the compiler from the editor node's ExpressionOperand and ExpressionIndent.
				static const FName Operand(TEXT("Operand"));
				static const FName DeltaIndent(TEXT("DeltaIndent"));
				return Name == Operand || Name == DeltaIndent;
			}
			// The class a Blueprint wrapper holds is the node's BlueprintClass; its flags are compile results.
			return IsBlueprintWrapper(Owner);
		}

		/** Whether a binding feeds only a part of its top-level property: a member, or one element of an array. */
		bool IsDeepTarget(const FPropertyBindingPath& Target)
		{
			return Target.NumSegments() > 1 || (Target.NumSegments() == 1 && Target.GetSegment(0).GetArrayIndex() != INDEX_NONE);
		}

		/** The paths of the bindings that feed a part of a top-level property, by that property. */
		void CollectDeepBound(const TArray<const FStateTreePropertyPathBinding*>* Bindings, TMap<FName, TArray<FDstPropertyPath>>& OutBound)
		{
			if (!Bindings)
			{
				return;
			}
			for (const FStateTreePropertyPathBinding* Binding : *Bindings)
			{
				const FPropertyBindingPath& Target = Binding->GetTargetPath();
				if (IsDeepTarget(Target))
				{
					OutBound.FindOrAdd(Target.GetSegment(0).GetName()).Add(ToDstPropertyPath(Target));
				}
			}
		}

		/** The top-level properties a node's bindings feed as a whole (a path of one segment, no array index). */
		void CollectWholeBound(const TArray<const FStateTreePropertyPathBinding*>* Bindings, TSet<FName>& OutBound)
		{
			if (!Bindings)
			{
				return;
			}
			for (const FStateTreePropertyPathBinding* Binding : *Bindings)
			{
				const FPropertyBindingPath& Target = Binding->GetTargetPath();
				if (Target.NumSegments() == 1 && Target.GetSegment(0).GetArrayIndex() == INDEX_NONE)
				{
					OutBound.Add(Target.GetSegment(0).GetName());
				}
			}
		}
	}

	FDstNode FExtractor::ExtractNode(const FStateTreeEditorNode& Editor, const FString& Key, const EDstNodeRole Role,
		const FNodeSite& Site, const int32 Depth)
	{
		FDstNode Out;
		Out.Key = Key;
		Out.Role = Role;

		const UScriptStruct* NodeStruct = Editor.Node.GetScriptStruct();
		if (!NodeStruct)
		{
			// Callers leave empty nodes out; this is a guard.
			return Out;
		}

		Out.StructType = NodeStruct->GetPathName();
		if (const UClass* BlueprintClass = GetBlueprintClass(Editor))
		{
			Out.BlueprintClass = BlueprintClass->GetPathName();
		}
		// The Name the node struct holds, not FStateTreeEditorNode::GetName(), which falls back to the display name.
		if (const FStateTreeNodeBase* Base = Editor.GetNode().GetPtr<FStateTreeNodeBase>())
		{
			if (!Base->Name.IsNone())
			{
				Out.BindName = Base->Name.ToString();
			}
		}
		if (Editor.ID.IsValid())
		{
			Ids.Add(Key, Editor.ID);
		}

		// The instance data: a struct for a C++ node, an object for a Blueprint node.
		const UStruct* InstanceStruct = nullptr;
		const void* InstanceData = nullptr;
		const void* InstanceDefault = nullptr;
		TSharedPtr<FStructOnScope> InstanceDefaultScope;
		if (Editor.InstanceObject)
		{
			InstanceStruct = Editor.InstanceObject->GetClass();
			InstanceData = Editor.InstanceObject.Get();
			InstanceDefault = Editor.InstanceObject->GetClass()->GetDefaultObject();
		}
		else if (Editor.Instance.IsValid())
		{
			InstanceStruct = Editor.Instance.GetScriptStruct();
			InstanceData = Editor.Instance.GetMemory();
			InstanceDefaultScope = MakeShared<FStructOnScope>(Editor.Instance.GetScriptStruct());
			InstanceDefault = InstanceDefaultScope->GetStructMemory();
		}

		const TArray<const FStateTreePropertyPathBinding*>* InstanceBindings = FindBindings(Editor.ID);
		const TArray<const FStateTreePropertyPathBinding*>* NodeBindings = FindBindings(Editor.GetNodeID());
		TSet<FName> InstanceBound;
		TSet<FName> NodeBound;
		CollectWholeBound(InstanceBindings, InstanceBound);
		CollectWholeBound(NodeBindings, NodeBound);
		TMap<FName, TArray<FDstPropertyPath>> InstancePartBound;
		TMap<FName, TArray<FDstPropertyPath>> NodePartBound;
		CollectDeepBound(InstanceBindings, InstancePartBound);
		CollectDeepBound(NodeBindings, NodePartBound);

		// Constants.
		{
			const FStructOnScope DefaultNode(NodeStruct);
			ExtractConstants(NodeStruct, Editor.Node.GetMemory(), DefaultNode.GetStructMemory(), EDstPropertySide::Node,
				NodeBound, NodePartBound, Key, NodeStruct, Out.Args);
		}
		if (InstanceStruct && InstanceData && InstanceDefault)
		{
			ExtractConstants(InstanceStruct, InstanceData, InstanceDefault, EDstPropertySide::Instance,
				InstanceBound, InstancePartBound, Key, nullptr, Out.Args);
		}

		// Bindings, in editor order; the sort below puts them next to the constants of the same property.
		if (InstanceBindings)
		{
			for (const FStateTreePropertyPathBinding* Binding : *InstanceBindings)
			{
				FDstArg Arg;
				if (BuildBindingArg(*Binding, EDstPropertySide::Instance, InstanceStruct, Key, Site, Depth, Arg))
				{
					Out.Args.Add(MoveTemp(Arg));
				}
			}
		}
		if (NodeBindings)
		{
			for (const FStateTreePropertyPathBinding* Binding : *NodeBindings)
			{
				FDstArg Arg;
				if (BuildBindingArg(*Binding, EDstPropertySide::Node, NodeStruct, Key, Site, Depth, Arg))
				{
					Out.Args.Add(MoveTemp(Arg));
				}
			}
		}

		// Side first, then the property path, spelled the way the model dump sorts them.
		Out.Args.StableSort([](const FDstArg& A, const FDstArg& B)
		{
			if (A.Side != B.Side)
			{
				return static_cast<uint8>(A.Side) < static_cast<uint8>(B.Side);
			}
			return A.Property.ToString().Compare(B.Property.ToString(), ESearchCase::CaseSensitive) < 0;
		});

		return Out;
	}

	void FExtractor::ExtractConstants(const UStruct* Struct, const void* Actual, const void* Default, const EDstPropertySide Side,
		const TSet<FName>& Bound, const TMap<FName, TArray<FDstPropertyPath>>& PartBound, const FString& NodeKey, const UScriptStruct* NodeStruct,
		TArray<FDstArg>& OutArgs)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			// Only what an author can set: a property the details panel does not show (an upgrade's version number) is the
			// engine's own bookkeeping, and the text cannot write it either.
			// The StateTree base structs' own fields are the node settings (Enabled, Evaluation, ...): they are edited through
			// the editor node, not the details panel, and have no edit flag; IsInternalNodeProperty sorts them.
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
				|| (!Property->HasAnyPropertyFlags(CPF_Edit) && !IsStateTreeBaseOwner(Property->GetOwnerStruct())))
			{
				continue;
			}
			if (Side == EDstPropertySide::Node && IsInternalNodeProperty(*Property, NodeStruct))
			{
				continue;
			}
			if (Side == EDstPropertySide::Instance)
			{
				// What a node produces at run time, and what the compiler binds by itself, is never written by hand: the
				// value an editor happens to hold there is stale, not authored.
				const EStateTreePropertyUsage Usage = UE::StateTree::GetUsageFromMetaData(Property);
				if (Usage == EStateTreePropertyUsage::Output || Usage == EStateTreePropertyUsage::Context)
				{
					continue;
				}
			}
			if (Bound.Contains(Property->GetFName()))
			{
				continue;
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				// Delegate dispatchers and listeners hold IDs the compiler hands out; only the bindings between them are authored.
				if (StructProperty->Struct == FStateTreeDelegateDispatcher::StaticStruct() || StructProperty->Struct == FStateTreeDelegateListener::StaticStruct())
				{
					continue;
				}
			}

			if (Property->ArrayDim > 1)
			{
				// DstExportDefault exports the first element only; the model has no way to hold the others.
				bool bDiffers = false;
				for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim && !bDiffers; ++ArrayIndex)
				{
					bDiffers = !Property->Identical_InContainer(Actual, Default, ArrayIndex, PPF_None);
				}
				if (bDiffers)
				{
					Sink.Error(TEXT("DST8004"), Lang::FDstSpan(),
						FString::Printf(TEXT("`%s` of `%s` is a fixed-size array whose elements differ from the default, which the model cannot hold."),
							*Property->GetName(), *NodeKey),
						TEXT("Change the property to a TArray, or put the values back to the default."));
				}
				continue;
			}

			// Where a binding feeds a part of the property, the constant holds that part at the default a text gives it: whatever the
			// asset has stored under a binding is overwritten by it, and the text could not say it. A reference to a tree is also
			// brought in step with its tree.
			const TArray<FDstPropertyPath>* PartPaths = PartBound.Find(Property->GetFName());
			FString ActualText;
			if (PartPaths || IsDstStateTreeReferenceProperty(*Property))
			{
				bool bSameAsDefault = false;
				FString Problem;
				if (!ExportDstValueWithBoundParts(*Property, Actual, Default, PartPaths ? TConstArrayView<FDstPropertyPath>(*PartPaths) : TConstArrayView<FDstPropertyPath>(),
					ActualText, bSameAsDefault, Problem))
				{
					Sink.Error(TEXT("DST8007"), Lang::FDstSpan(),
						FString::Printf(TEXT("A binding ends inside `%s.%s`, but its path leads nowhere in the value: %s"), *NodeKey, *Property->GetName(), *Problem),
						TEXT("Bind the property again in the editor."));
					continue;
				}
				if (bSameAsDefault)
				{
					continue;
				}
			}
			else
			{
				if (Property->Identical_InContainer(Actual, Default, 0, PPF_None))
				{
					continue;
				}
				ActualText = DstExportDefault(*Property, Actual);
			}
			// Differs as a value but not as text (something the export does not show): the model could not tell them apart.
			if (ActualText.Equals(DstExportDefault(*Property, Default), ESearchCase::CaseSensitive))
			{
				continue;
			}

			FDstArg& Arg = OutArgs.AddDefaulted_GetRef();
			Arg.Side = Side;
			FDstPathSegment& Segment = Arg.Property.Segments.AddDefaulted_GetRef();
			Segment.Name = Property->GetName();
			Arg.Kind = EDstValueKind::Constant;
			Arg.ConstantText = ActualText;
		}
	}
}
