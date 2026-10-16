// Copyright DreamMoon. All Rights Reserved.
//
// Struct IDs and bindings.
//
// A binding in the editor data names its two ends by struct ID and a path inside the struct. The extractor first learns
// what each ID stands for (RegisterStructs): the tree's parameters, a context, a node's instance data or its template,
// a state's parameters, a state's event, a transition, a transition's event. Then each binding is attached to the node
// at its target end as an argument of that node:
//   - a plain binding            -> Binding, Source = what the source ID and path mean;
//   - one that carries a property function node -> Function, the function extracted as a node of its own (it may be fed
//     by further bindings; they land on it);
//   - one into a property reference -> Ref;
//   - an output binding          -> Output (the node's property is copied into Source);
//   - a binding into a transition's delegate listener -> the transition's Delegate (a dispatcher, or a task completion);
//   - a binding into a linked state's parameters -> the state's ParameterBindings (same four kinds).
// A binding to a context that equals what the compiler binds by itself is left out.
//
// Nothing is dropped without a report: a binding that reaches no node, or that the model has no place for, is reported
// at the end (ReportUnconsumedBindings).

#include "DstExtractCommon.h"

#include "PropertyBindingPath.h"
#include "StateTreeDelegate.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreePropertyBindings.h"
#include "StateTreePropertyRefHelpers.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Extracting
{
	using namespace Model;

	namespace
	{
		/** How deep property functions may feed one another before the extractor gives up (a cycle in damaged data). */
		constexpr int32 MaxFunctionDepth = 16;

		/** Whether the property the path ends in, or one it passes through, is a property reference. */
		bool IsReferenceTarget(const FPropertyBindingPath& TargetPath, const UStruct* TargetStruct)
		{
			if (!TargetStruct)
			{
				return false;
			}
			TArray<FPropertyBindingPathIndirection> Indirections;
			if (!TargetPath.ResolveIndirections(TargetStruct, Indirections))
			{
				return false;
			}
			for (const FPropertyBindingPathIndirection& Indirection : Indirections)
			{
				if (const FProperty* Property = Indirection.GetProperty())
				{
					if (UE::StateTree::PropertyRefHelpers::IsPropertyRef(*Property))
					{
						return true;
					}
				}
			}
			return false;
		}

		/** The output property of a property function by the text's convention: Result, bResult, Output, or the only Output-category property. */
		FName GetConventionalOutputName(const UStruct& InstanceStruct)
		{
			static const FName Names[] = { FName(TEXT("Result")), FName(TEXT("bResult")), FName(TEXT("Output")) };
			for (const FName& Name : Names)
			{
				if (InstanceStruct.FindPropertyByName(Name))
				{
					return Name;
				}
			}
			if (const FProperty* Single = UE::StateTree::GetStructSingleOutputProperty(InstanceStruct))
			{
				return Single->GetFName();
			}
			return NAME_None;
		}
	}

	// ------------------------------------------------------------------------------------------
	// What each struct ID stands for
	// ------------------------------------------------------------------------------------------

	void FExtractor::AddStruct(const FGuid& Id, FStructInfo&& Info)
	{
		if (Id.IsValid() && !Structs.Contains(Id))
		{
			Structs.Add(Id, MoveTemp(Info));
		}
	}

	void FExtractor::RegisterNode(const FStateTreeEditorNode& Node, const FString& Key, const bool bGlobal, const bool bTask)
	{
		if (!Node.ID.IsValid())
		{
			return;
		}

		FStructInfo Instance;
		Instance.Kind = EStructKind::NodeInstance;
		Instance.Owner = Key;
		Instance.bGlobal = bGlobal;
		Instance.bTask = bTask;

		FStructInfo Template = Instance;
		Template.Kind = EStructKind::NodeTemplate;

		AddStruct(Node.ID, MoveTemp(Instance));
		AddStruct(Node.GetNodeID(), MoveTemp(Template));
	}

	void FExtractor::RegisterNodeList(const TArray<const FStateTreeEditorNode*>& Nodes, const FString& StatePath, const TCHAR* RoleWord, const bool bTask)
	{
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			RegisterNode(*Nodes[Index], MakeStateNodeKey(StatePath, RoleWord, Index), /*bGlobal*/ false, bTask);
		}
	}

	void FExtractor::RegisterStructs()
	{
		{
			FStructInfo Info;
			Info.Kind = EStructKind::TreeParameters;
			AddStruct(Data->GetRootParametersGuid(), MoveTemp(Info));
		}

		for (const FStateTreeExternalDataDesc& Desc : Data->Schema->GetContextDataDescs())
		{
			FStructInfo Info;
			Info.Kind = EStructKind::Context;
			Info.Owner = Desc.Name.ToString();
			AddStruct(Desc.ID, MoveTemp(Info));
		}

		// The same order and the same skipping as ExtractGlobals, so the keys agree.
		int32 GlobalIndex = 0;
		for (const FStateTreeEditorNode& Node : Data->Evaluators)
		{
			if (Node.Node.IsValid())
			{
				RegisterNode(Node, MakeGlobalKey(GlobalIndex++), /*bGlobal*/ true, /*bTask*/ false);
			}
		}
		for (const FStateTreeEditorNode& Node : Data->GlobalTasks)
		{
			if (Node.Node.IsValid())
			{
				RegisterNode(Node, MakeGlobalKey(GlobalIndex++), /*bGlobal*/ true, /*bTask*/ true);
			}
		}

		RegisterStates(Data->SubTrees, nullptr, nullptr);
	}

	void FExtractor::RegisterStates(const TArray<TObjectPtr<UStateTreeState>>& States, const UStateTreeState* Parent, const FString* ParentPath)
	{
		TSet<FName> SeenNames;
		for (const TObjectPtr<UStateTreeState>& StatePtr : States)
		{
			const UStateTreeState* State = StatePtr;
			if (!State)
			{
				continue;
			}

			const FString Path = MakeChildPath(ParentPath, State->Name.ToString());
			bool bDuplicate = false;
			SeenNames.Add(State->Name, &bDuplicate);
			if (bDuplicate)
			{
				Sink.Error(TEXT("DST8005"), Lang::FDstSpan(),
					FString::Printf(TEXT("State `%s` has the same name as a sibling state, so both of them have the path `%s`."),
						*State->Name.ToString(), *Path),
					TEXT("Rename one of them in the editor: the model identifies a state by its path."));
			}

			StatePaths.Add(State, Path);
			ParentOf.Add(State, Parent);
			if (State->ID.IsValid() && !StatePathById.Contains(State->ID))
			{
				StatePathById.Add(State->ID, Path);
			}

			{
				FStructInfo Info;
				Info.Kind = EStructKind::StateParameters;
				Info.Owner = Path;
				AddStruct(State->Parameters.ID, MoveTemp(Info));
			}
			// The engine only offers a state's event struct as a source while the state requires an event.
			if (State->bHasRequiredEventToEnter)
			{
				FStructInfo Info;
				Info.Kind = EStructKind::StateEvent;
				Info.Owner = Path;
				AddStruct(State->GetEventID(), MoveTemp(Info));
			}

			RegisterNodeList(CollectNodes(State->EnterConditions), Path, TEXT("EnterCondition"), /*bTask*/ false);
			RegisterNodeList(CollectNodes(State->Considerations), Path, TEXT("Consideration"), /*bTask*/ false);
			RegisterNodeList(CollectTasks(*State), Path, TEXT("Task"), /*bTask*/ true);

			for (int32 TransitionIndex = 0; TransitionIndex < State->Transitions.Num(); ++TransitionIndex)
			{
				const FStateTreeTransition& Transition = State->Transitions[TransitionIndex];
				const FString TransitionKey = MakeTransitionKey(Path, TransitionIndex);

				{
					FStructInfo Info;
					Info.Kind = EStructKind::Transition;
					Info.Owner = TransitionKey;
					AddStruct(Transition.ID, MoveTemp(Info));
				}
				if (Transition.Trigger == EStateTreeTransitionTrigger::OnEvent)
				{
					FStructInfo Info;
					Info.Kind = EStructKind::TransitionEvent;
					Info.Owner = TransitionKey;
					AddStruct(Transition.GetEventID(), MoveTemp(Info));
				}

				const TArray<const FStateTreeEditorNode*> Conditions = CollectNodes(Transition.Conditions);
				for (int32 Index = 0; Index < Conditions.Num(); ++Index)
				{
					RegisterNode(*Conditions[Index], MakeConditionKey(Path, TransitionIndex, Index), /*bGlobal*/ false, /*bTask*/ false);
				}
			}

			RegisterStates(State->Children, State, &Path);
		}
	}

	void FExtractor::GroupBindings()
	{
		for (const FStateTreePropertyPathBinding& Binding : Data->EditorBindings.GetBindings())
		{
			BindingsByTarget.FindOrAdd(Binding.GetTargetPath().GetStructID()).Add(&Binding);
		}
	}

	const TArray<const FStateTreePropertyPathBinding*>* FExtractor::FindBindings(const FGuid& TargetId) const
	{
		return BindingsByTarget.Find(TargetId);
	}

	// ------------------------------------------------------------------------------------------
	// Sources
	// ------------------------------------------------------------------------------------------

	TArray<FDstEventCandidate> FExtractor::CollectEventCandidates(const FNodeSite& Site) const
	{
		TArray<FDstEventCandidate> Candidates;

		// The events the state and its ancestors require, up to the root of the subtree they sit in.
		for (const UStateTreeState* State = Site.State; State; State = ParentOf.FindRef(State))
		{
			if (State->bHasRequiredEventToEnter)
			{
				if (const FString* Path = StatePaths.Find(State))
				{
					FDstEventCandidate& Candidate = Candidates.AddDefaulted_GetRef();
					Candidate.Kind = EDstSourceKind::StateEvent;
					Candidate.Owner = *Path;
					Candidate.PayloadStruct = State->RequiredEventToEnter.PayloadStruct.Get();
				}
			}
			if (State->Type == EStateTreeStateType::Subtree)
			{
				break;
			}
		}

		// A transition condition also reads the event that triggers its transition.
		if (Site.State && Site.State->Transitions.IsValidIndex(Site.TransitionIndex))
		{
			const FStateTreeTransition& Transition = Site.State->Transitions[Site.TransitionIndex];
			if (Transition.Trigger == EStateTreeTransitionTrigger::OnEvent)
			{
				if (const FString* Path = StatePaths.Find(Site.State))
				{
					FDstEventCandidate& Candidate = Candidates.AddDefaulted_GetRef();
					Candidate.Kind = EDstSourceKind::TransitionEvent;
					Candidate.Owner = MakeTransitionKey(*Path, Site.TransitionIndex);
					Candidate.PayloadStruct = Transition.RequiredEvent.PayloadStruct.Get();
				}
			}
		}

		return Candidates;
	}

	bool FExtractor::ResolveSource(const FPropertyBindingPath& Path, const FString& TargetText, const FNodeSite& Site, FDstSourceRef& OutSource)
	{
		OutSource = FDstSourceRef();

		if (const FStructInfo* Info = Structs.Find(Path.GetStructID()))
		{
			switch (Info->Kind)
			{
			case EStructKind::TreeParameters:
				OutSource.Kind = EDstSourceKind::Parameters;
				break;
			case EStructKind::Context:
				OutSource.Kind = EDstSourceKind::Context;
				OutSource.Owner = Info->Owner;
				break;
			case EStructKind::NodeInstance:
				OutSource.Kind = Info->bGlobal ? EDstSourceKind::Global : EDstSourceKind::Node;
				OutSource.Owner = Info->Owner;
				break;
			case EStructKind::StateParameters:
				OutSource.Kind = EDstSourceKind::StateParameters;
				OutSource.Owner = Info->Owner;
				break;
			case EStructKind::StateEvent:
				OutSource.Kind = EDstSourceKind::StateEvent;
				OutSource.Owner = Info->Owner;
				break;
			case EStructKind::TransitionEvent:
				OutSource.Kind = EDstSourceKind::TransitionEvent;
				OutSource.Owner = Info->Owner;
				break;
			case EStructKind::NodeTemplate:
			case EStructKind::Transition:
				Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
					FString::Printf(TEXT("`%s` reads from `%s`, which is %s rather than the instance data of a node; the model cannot name that source."),
						*TargetText, *Info->Owner, Info->Kind == EStructKind::Transition ? TEXT("a transition") : TEXT("the node's own struct")),
					TEXT("Bind the property to a property of the node's instance data, or to a parameter."));
				return false;
			}
			OutSource.Path = ToDstPropertyPath(Path);
			return true;
		}

		// Nobody owns the source struct. An event binding of a duplicated asset is the one case that can be put right.
		const TArray<FDstEventCandidate> Candidates = CollectEventCandidates(Site);
		int32 NumMatches = 0;
		switch (RepairBrokenEventSource(Path, Candidates, OutSource, &NumMatches))
		{
		case EDstEventRepair::Repaired:
			Sink.Warning(TEXT("DST8006"), Lang::FDstSpan(),
				TEXT("Repaired an event binding broken by duplicating the asset."),
				FString::Printf(TEXT("`%s` now reads `%s`. The engine gives a copy of an asset new IDs for the events it reads, and the binding keeps the old one; saving the asset again from the text rebuilds it correctly."),
					*TargetText, *OutSource.ToString()));
			return true;
		case EDstEventRepair::Ambiguous:
			Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
				FString::Printf(TEXT("`%s` reads an event, but the event it read no longer exists and %d events it could read fit its path, so the binding cannot be repaired."),
					*TargetText, NumMatches),
				TEXT("Bind the property to the event again in the editor."));
			return false;
		case EDstEventRepair::NoMatch:
			Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
				FString::Printf(TEXT("`%s` reads an event, but the event it read no longer exists and none of the %d events it could read fits its path."),
					*TargetText, Candidates.Num()),
				TEXT("Bind the property to the event again in the editor."));
			return false;
		case EDstEventRepair::NotAnEventPath:
			break;
		}

		Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
			FString::Printf(TEXT("`%s` reads from a struct that is not part of this tree (ID %s)."),
				*TargetText, *Path.GetStructID().ToString(EGuidFormats::Digits)),
			TEXT("The source was deleted or moved out of reach. Bind the property again in the editor."));
		return false;
	}

	bool FExtractor::IsAutomaticContextBinding(const FStateTreePropertyPathBinding& Binding, const FStructInfo& SourceInfo, const UStruct* TargetStruct) const
	{
		if (SourceInfo.Kind != EStructKind::Context || !Binding.GetSourcePath().IsPathEmpty() || !TargetStruct)
		{
			return false;
		}
		const FPropertyBindingPath& Target = Binding.GetTargetPath();
		if (Target.NumSegments() != 1 || Target.GetSegment(0).GetArrayIndex() != INDEX_NONE)
		{
			return false;
		}

		// What FStateTreeCompiler does for a Context property nobody bound: the context of that type (the closest name
		// when several have it).
		const FProperty* Property = TargetStruct->FindPropertyByName(Target.GetSegment(0).GetName());
		if (!Property || UE::StateTree::GetUsageFromMetaData(Property) != EStateTreePropertyUsage::Context)
		{
			return false;
		}
		const UStruct* ContextType = nullptr;
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			ContextType = StructProperty->Struct;
		}
		else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			ContextType = ObjectProperty->PropertyClass;
		}
		if (!ContextType)
		{
			return false;
		}

		const FStateTreeBindableStructDesc Automatic = Data->FindContextData(ContextType, Property->GetName());
		return Automatic.IsValid() && Automatic.ID == Binding.GetSourcePath().GetStructID();
	}

	// ------------------------------------------------------------------------------------------
	// Arguments
	// ------------------------------------------------------------------------------------------

	bool FExtractor::BuildBindingArg(const FStateTreePropertyPathBinding& Binding, const EDstPropertySide Side, const UStruct* TargetStruct,
		const FString& NodeKey, const FNodeSite& Site, const int32 Depth, FDstArg& OutArg)
	{
		Consumed.Add(&Binding);

		OutArg.Side = Side;
		OutArg.Property = ToDstPropertyPath(Binding.GetTargetPath());
		const FString TargetText = NodeKey + TEXT(".") + OutArg.Property.ToString();

		if (Binding.IsTaskCompletionBinding())
		{
			Sink.Error(TEXT("DST8007"), Lang::FDstSpan(),
				FString::Printf(TEXT("`%s` listens to the completion of a task. The model holds that only for a transition that triggers on a delegate."), *TargetText),
				TEXT("Listen to a delegate dispatcher of the task, or use an `on Delegate` transition."));
			return false;
		}

		// A property function feeding the property.
		if (const FStateTreeEditorNode* Function = Binding.GetPropertyFunctionNode().GetPtr<const FStateTreeEditorNode>())
		{
			if (Depth >= MaxFunctionDepth)
			{
				Sink.Error(TEXT("DST8007"), Lang::FDstSpan(),
					FString::Printf(TEXT("Property functions feeding `%s` are nested more than %d deep."), *TargetText, MaxFunctionDepth),
					TEXT("A property function that feeds itself, directly or through others, cannot be extracted."));
				return false;
			}
			if (!Function->Node.IsValid())
			{
				Sink.Warning(TEXT("DST8008"), Lang::FDstSpan(),
					FString::Printf(TEXT("`%s` is fed by a property function without a node; the binding was left out."), *TargetText));
				return false;
			}

			// The text names the function's output by convention; a binding that reads another property of it is not that.
			if (const UStruct* FunctionInstance = Function->Instance.GetScriptStruct())
			{
				const FName Conventional = GetConventionalOutputName(*FunctionInstance);
				const FPropertyBindingPath& SourcePath = Binding.GetSourcePath();
				if (!Conventional.IsNone() && (SourcePath.NumSegments() != 1 || SourcePath.GetSegment(0).GetName() != Conventional))
				{
					Sink.Warning(TEXT("DST8008"), Lang::FDstSpan(),
						FString::Printf(TEXT("`%s` reads `%s` of its property function, not the output `%s` the model assumes."),
							*TargetText, *SourcePath.ToString(), *Conventional.ToString()));
				}
			}

			OutArg.Kind = EDstValueKind::Function;
			OutArg.Function = MakeShared<FDstNode>(ExtractNode(*Function, NodeKey + TEXT("/Fn:") + OutArg.Property.ToString(),
				EDstNodeRole::PropertyFunction, Site, Depth + 1));
			return true;
		}

		const FStructInfo* SourceInfo = Structs.Find(Binding.GetSourcePath().GetStructID());
		FDstSourceRef Source;
		if (!ResolveSource(Binding.GetSourcePath(), TargetText, Site, Source))
		{
			return false;
		}

		if (Binding.IsOutputBinding())
		{
			OutArg.Kind = EDstValueKind::Output;
		}
		else if (IsReferenceTarget(Binding.GetTargetPath(), TargetStruct))
		{
			OutArg.Kind = EDstValueKind::Ref;
		}
		else
		{
			if (SourceInfo && IsAutomaticContextBinding(Binding, *SourceInfo, TargetStruct))
			{
				// The compiler makes this binding on its own.
				return false;
			}
			OutArg.Kind = EDstValueKind::Binding;
		}
		OutArg.Source = MoveTemp(Source);
		return true;
	}

	void FExtractor::ExtractDelegate(const FStateTreeTransition& Transition, const FString& TransitionKey, const FNodeSite& Site, FDstTransition& Out)
	{
		const TArray<const FStateTreePropertyPathBinding*>* Bound = FindBindings(Transition.ID);
		if (!Bound)
		{
			return;
		}

		static const FName ListenerName(TEXT("DelegateListener"));
		for (const FStateTreePropertyPathBinding* Binding : *Bound)
		{
			Consumed.Add(Binding);

			const FPropertyBindingPath& Target = Binding->GetTargetPath();
			const FString TargetText = TransitionKey + TEXT(".") + Target.ToString();
			if (Target.NumSegments() == 0 || Target.GetSegment(0).GetName() != ListenerName)
			{
				Sink.Error(TEXT("DST8007"), Lang::FDstSpan(),
					FString::Printf(TEXT("A binding ends in `%s`, a property of a transition other than its delegate listener, which the model cannot hold."), *TargetText));
				continue;
			}
			if (Transition.Trigger != EStateTreeTransitionTrigger::OnDelegate)
			{
				Sink.Warning(TEXT("DST8008"), Lang::FDstSpan(),
					FString::Printf(TEXT("`%s` has a delegate binding, but the transition does not trigger on a delegate; the binding was left out."), *TransitionKey));
				continue;
			}

			if (const TOptional<UE::StateTree::ETaskCompletionCondition> Completion = Binding->GetCompletionBinding(); Completion.IsSet())
			{
				// The source is the task's node struct, not its instance data: "the task completes".
				const FStructInfo* TaskInfo = Structs.Find(Binding->GetSourcePath().GetStructID());
				if (!TaskInfo || TaskInfo->Kind != EStructKind::NodeTemplate || !TaskInfo->bTask)
				{
					Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
						FString::Printf(TEXT("`%s` listens to the completion of something that is not a task of this tree."), *TargetText));
					continue;
				}
				Out.Delegate = FDstSourceRef();
				Out.Delegate.Kind = TaskInfo->bGlobal ? EDstSourceKind::Global : EDstSourceKind::Node;
				Out.Delegate.Owner = TaskInfo->Owner;
				switch (Completion.GetValue())
				{
				case UE::StateTree::ETaskCompletionCondition::Succeeds:
					Out.Completion = EDstCompletion::Succeeds;
					break;
				case UE::StateTree::ETaskCompletionCondition::Fails:
					Out.Completion = EDstCompletion::Fails;
					break;
				case UE::StateTree::ETaskCompletionCondition::Completes:
					Out.Completion = EDstCompletion::Completes;
					break;
				}
			}
			else
			{
				FDstSourceRef Dispatcher;
				if (ResolveSource(Binding->GetSourcePath(), TargetText, Site, Dispatcher))
				{
					Out.Delegate = MoveTemp(Dispatcher);
				}
			}
		}
	}

	void FExtractor::ExtractParameterBindings(const UStateTreeState& State, const FString& StatePath, FDstState& Out)
	{
		const TArray<const FStateTreePropertyPathBinding*>* Bound = FindBindings(State.Parameters.ID);
		if (!Bound)
		{
			return;
		}

		FNodeSite Site;
		Site.State = &State;
		const UStruct* BagStruct = State.Parameters.Parameters.GetPropertyBagStruct();
		for (const FStateTreePropertyPathBinding* Binding : *Bound)
		{
			FDstArg Arg;
			if (BuildBindingArg(*Binding, EDstPropertySide::Instance, BagStruct, StatePath + TEXT("/Parameters"), Site, /*Depth*/ 0, Arg))
			{
				Out.ParameterBindings.Add(MoveTemp(Arg));
			}
		}

		Out.ParameterBindings.StableSort([](const FDstArg& A, const FDstArg& B)
		{
			return A.Property.ToString().Compare(B.Property.ToString(), ESearchCase::CaseSensitive) < 0;
		});
	}

	void FExtractor::ReportUnconsumedBindings()
	{
		for (const FStateTreePropertyPathBinding& Binding : Data->EditorBindings.GetBindings())
		{
			if (Consumed.Contains(&Binding))
			{
				continue;
			}

			const FString TargetText = Binding.GetTargetPath().ToString();
			const FStructInfo* Info = Structs.Find(Binding.GetTargetPath().GetStructID());
			if (Info && (Info->Kind == EStructKind::TreeParameters || Info->Kind == EStructKind::StateParameters))
			{
				const FString Whose = Info->Kind == EStructKind::TreeParameters ? FString(TEXT("the tree")) : FString::Printf(TEXT("state `%s`"), *Info->Owner);
				Sink.Error(TEXT("DST8007"), Lang::FDstSpan(),
					FString::Printf(TEXT("A binding feeds the parameter `%s` of %s, which the model has no place for: only the parameters of a linked state can be fed."), *TargetText, *Whose),
					TEXT("Give the parameter a constant value, or bind inside the subtree that reads it."));
			}
			else if (Info)
			{
				Sink.Error(TEXT("DST8007"), Lang::FDstSpan(),
					FString::Printf(TEXT("A binding ends in `%s` of `%s`, which the model cannot hold."), *TargetText, *Info->Owner));
			}
			else
			{
				Sink.Warning(TEXT("DST8008"), Lang::FDstSpan(),
					FString::Printf(TEXT("A binding into `%s` (struct %s) reaches nothing in this tree and was left out."),
						*TargetText, *Binding.GetTargetPath().GetStructID().ToString(EGuidFormats::Digits)));
			}
		}
	}
}
