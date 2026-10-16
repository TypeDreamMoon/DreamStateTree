// Copyright DreamMoon. All Rights Reserved.

// The experiment that checks whether everything needed to turn an existing StateTree asset into text can be read.
// It loads every StateTree under /Game, compiles it, dumps the editor data, and counts which features the trees use.

#include "Spike/SpikeDump.h"
#include "Spike/SpikeBuildHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "GameplayTagContainer.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyBindingBindableStructDescriptor.h"
#include "PropertyBindingDataView.h"
#include "PropertyBindingPath.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeDelegate.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorDataExtension.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeEditorSchema.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreePropertyRefHelpers.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Spike
{
namespace
{
	// ---- Small helpers ---------------------------------------------------------------------------------------

	template <typename EnumType>
	FString EnumToString(const EnumType Value)
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

	FString CompletionConditionText(const UE::StateTree::ETaskCompletionCondition Condition)
	{
		switch (Condition)
		{
		case UE::StateTree::ETaskCompletionCondition::Succeeds:
			return TEXT("Succeeds");
		case UE::StateTree::ETaskCompletionCondition::Fails:
			return TEXT("Fails");
		case UE::StateTree::ETaskCompletionCondition::Completes:
			return TEXT("Completes");
		}
		return TEXT("?");
	}

	FString BoolText(const bool bValue)
	{
		return bValue ? TEXT("true") : TEXT("false");
	}

	FString PathText(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString(TEXT("None"));
	}

	/** The severities are ordered; the lowest one (CriticalError, deprecated) counts as an error. */
	bool IsErrorSeverity(const EMessageSeverity::Type Severity)
	{
		return Severity <= EMessageSeverity::Error;
	}

	const TCHAR* SeverityText(const EMessageSeverity::Type Severity)
	{
		if (IsErrorSeverity(Severity))
		{
			return TEXT("Error");
		}
		switch (Severity)
		{
		case EMessageSeverity::PerformanceWarning:
			return TEXT("PerformanceWarning");
		case EMessageSeverity::Warning:
			return TEXT("Warning");
		case EMessageSeverity::Info:
			return TEXT("Info");
		default:
			break;
		}
		return TEXT("?");
	}

	bool CaseSensitiveLess(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	}

	/** "/Game/AI/ST_Brawler" becomes "Game_AI_ST_Brawler": unique per asset, safe as a file name. */
	FString ReportNameFor(const FString& PackageName)
	{
		FString Name = PackageName;
		Name.ReplaceInline(TEXT("/"), TEXT("_"));
		Name.RemoveFromStart(TEXT("_"));
		return Name;
	}

	const FStateTreeEditorNode* GetFunctionNode(const FStateTreePropertyPathBinding& Binding)
	{
		return Binding.GetPropertyFunctionNode().GetPtr<const FStateTreeEditorNode>();
	}

	FString BagTypeText(const FPropertyBagPropertyDesc& Desc)
	{
		FString Type = EnumToString(Desc.ValueType);
		if (Desc.ValueTypeObject)
		{
			Type += FString::Printf(TEXT("(%s)"), *Desc.ValueTypeObject->GetPathName());
		}
		for (const EPropertyBagContainerType Container : Desc.ContainerTypes)
		{
			Type = FString::Printf(TEXT("%s<%s>"), *EnumToString(Container), *Type);
		}
		return Type;
	}

	bool ReadBoolProperty(const UStruct* Struct, const void* Data, const TCHAR* PropertyName, bool& OutValue)
	{
		if (!Struct || !Data)
		{
			return false;
		}
		if (const FBoolProperty* Property = CastField<FBoolProperty>(Struct->FindPropertyByName(PropertyName)))
		{
			OutValue = Property->GetPropertyValue_InContainer(Data);
			return true;
		}
		return false;
	}

	// ---- Counting --------------------------------------------------------------------------------------------

	/** category -> key -> count, plus the things that could not be resolved. */
	struct FInventory
	{
		TMap<FString, TMap<FString, int32>> Counts;
		TArray<FString> Unresolved;

		void Add(const FString& Category, const FString& Key, const int32 Amount = 1)
		{
			Counts.FindOrAdd(Category).FindOrAdd(Key) += Amount;
		}

		void AddFlag(const FString& Category, const bool bValue)
		{
			Add(Category, BoolText(bValue));
		}

		void Merge(const FInventory& Other)
		{
			for (const TPair<FString, TMap<FString, int32>>& Category : Other.Counts)
			{
				for (const TPair<FString, int32>& Key : Category.Value)
				{
					Add(Category.Key, Key.Key, Key.Value);
				}
			}
		}

		/** "key=count, key=count" for one category, sorted by key; empty when the category is unused. */
		FString Summary(const FString& Category) const
		{
			const TMap<FString, int32>* Keys = Counts.Find(Category);
			if (!Keys)
			{
				return FString();
			}

			TArray<FString> KeyNames;
			Keys->GetKeys(KeyNames);
			KeyNames.Sort(CaseSensitiveLess);

			FString Text;
			for (const FString& KeyName : KeyNames)
			{
				Text += FString::Printf(TEXT("%s%s=%d"), Text.IsEmpty() ? TEXT("") : TEXT(", "), *KeyName, Keys->FindChecked(KeyName));
			}
			return Text;
		}

		void Append(FString& Out, const FString& Indent) const
		{
			TArray<FString> Categories;
			Counts.GetKeys(Categories);
			Categories.Sort(CaseSensitiveLess);
			for (const FString& Category : Categories)
			{
				Out += FString::Printf(TEXT("%s[%s] %s\n"), *Indent, *Category, *Summary(Category));
			}
		}
	};

	/** What the scan found in the scalar-ish parts of a struct value: state links, property refs, delegates. */
	struct FStructScan
	{
		TArray<FStateTreeStateLink> Links;
		int32 NumPropertyRefs = 0;
		int32 NumDelegateListeners = 0;
		int32 NumDelegateDispatchers = 0;
	};

	void ScanStruct(const UStruct* Struct, const void* Data, FStructScan& Scan, int32 Depth);

	void ScanValue(const FProperty* Property, const void* Value, FStructScan& Scan, const int32 Depth)
	{
		// Guards against self-referential data; real node data is far shallower.
		if (Depth > 8)
		{
			return;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const UScriptStruct* Struct = StructProperty->Struct;
			if (Struct == FStateTreeStateLink::StaticStruct())
			{
				Scan.Links.Add(*static_cast<const FStateTreeStateLink*>(Value));
				return;
			}
			if (UE::StateTree::PropertyRefHelpers::IsPropertyRef(*Property))
			{
				++Scan.NumPropertyRefs;
				return;
			}
			if (Struct == FStateTreeDelegateListener::StaticStruct())
			{
				++Scan.NumDelegateListeners;
				return;
			}
			if (Struct == FStateTreeDelegateDispatcher::StaticStruct())
			{
				++Scan.NumDelegateDispatchers;
				return;
			}
			if (Struct == TBaseStructure<FInstancedStruct>::Get())
			{
				const FInstancedStruct& Inner = *static_cast<const FInstancedStruct*>(Value);
				if (Inner.IsValid())
				{
					ScanStruct(Inner.GetScriptStruct(), Inner.GetMemory(), Scan, Depth + 1);
				}
				return;
			}
			ScanStruct(Struct, Value, Scan, Depth + 1);
		}
		else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, Value);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				ScanValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Scan, Depth + 1);
			}
		}
		// Object references, maps and sets are not followed.
	}

	void ScanStruct(const UStruct* Struct, const void* Data, FStructScan& Scan, const int32 Depth)
	{
		if (!Struct || !Data)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
			{
				ScanValue(Property, Property->ContainerPtrToValuePtr<void>(Data, ArrayIndex), Scan, Depth);
			}
		}
	}

	// ---- The scanner -----------------------------------------------------------------------------------------

	class FEditorDataScanner
	{
	public:
		FEditorDataScanner(const UStateTreeEditorData& InData, FInventory& InInventory)
			: Data(InData)
			, Inv(InInventory)
		{
		}

		void Scan()
		{
			Inv.Add(TEXT("EditorData.Class"), Data.GetClass()->GetPathName());
			Inv.AddFlag(TEXT("EditorData.IsBaseClass"), Data.GetClass() == UStateTreeEditorData::StaticClass());

			if (Data.Schema)
			{
				Inv.Add(TEXT("Schema.Class"), Data.Schema->GetClass()->GetPathName());
				Inv.Add(TEXT("Schema.ContextDataCount"), FString::FromInt(Data.Schema->GetContextDataDescs().Num()));
			}
			else
			{
				Inv.Add(TEXT("Schema.Class"), TEXT("None"));
				Unresolved(TEXT("the editor data has no schema"));
			}
			Inv.Add(TEXT("EditorSchema.Class"), PathText(Data.EditorSchema ? Data.EditorSchema->GetClass() : nullptr));

			Inv.Add(TEXT("Extensions.Count"), FString::FromInt(Data.Extensions.Num()));
			for (const UStateTreeEditorDataExtension* Extension : Data.Extensions)
			{
				Inv.Add(TEXT("Extensions.Class"), PathText(Extension ? Extension->GetClass() : nullptr));
			}

			ScanBag(Data.GetRootParametersPropertyBag(), TEXT("RootParameters"));
			Inv.Add(TEXT("Colors.Count"), FString::FromInt(Data.Colors.Num()));

			Inv.Add(TEXT("GlobalTasksCompletion"), EnumToString(Data.GlobalTasksCompletion));
			for (int32 Index = 0; Index < Data.Evaluators.Num(); ++Index)
			{
				ScanNode(Data.Evaluators[Index], TEXT("Evaluator"), false, FString::Printf(TEXT("Global/Evaluator[%d]"), Index));
			}
			for (int32 Index = 0; Index < Data.GlobalTasks.Num(); ++Index)
			{
				ScanNode(Data.GlobalTasks[Index], TEXT("GlobalTask"), false, FString::Printf(TEXT("Global/Task[%d]"), Index));
			}

			ScanStates(Data.SubTrees, 0, FString());
			ScanBindings();
		}

		int32 NumStates = 0;
		int32 NumNodes = 0;
		int32 NumBindings = 0;

	private:
		void Unresolved(const FString& Text)
		{
			Inv.Unresolved.Add(Text);
		}

		void ScanBag(const FInstancedPropertyBag& Bag, const FString& Category)
		{
			const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
			const TConstArrayView<FPropertyBagPropertyDesc> Descs = BagStruct ? BagStruct->GetPropertyDescs() : TConstArrayView<FPropertyBagPropertyDesc>();
			Inv.Add(Category + TEXT(".Count"), FString::FromInt(Descs.Num()));
			for (const FPropertyBagPropertyDesc& Desc : Descs)
			{
				Inv.Add(Category + TEXT(".Type"), BagTypeText(Desc));
			}
		}

		/** Counts what the scan of a node's data found. */
		void RecordScan(const FStructScan& Found, const FString& Where)
		{
			if (Found.NumPropertyRefs > 0)
			{
				Inv.Add(TEXT("NodeProperties.PropertyRefFields"), TEXT("all"), Found.NumPropertyRefs);
			}
			if (Found.NumDelegateListeners > 0)
			{
				Inv.Add(TEXT("NodeProperties.DelegateListenerFields"), TEXT("all"), Found.NumDelegateListeners);
			}
			if (Found.NumDelegateDispatchers > 0)
			{
				Inv.Add(TEXT("NodeProperties.DelegateDispatcherFields"), TEXT("all"), Found.NumDelegateDispatchers);
			}

			for (const FStateTreeStateLink& Link : Found.Links)
			{
				Inv.Add(TEXT("NodeProperties.StateLinkType"), EnumToString(Link.LinkType));
				if (Link.LinkType == EStateTreeTransitionType::GotoState)
				{
					const bool bFound = Data.GetStateByID(Link.ID) != nullptr;
					Inv.AddFlag(TEXT("NodeProperties.StateLinkResolved"), bFound);
					if (!bFound)
					{
						Unresolved(FString::Printf(TEXT("%s: a state link in a node property points to a state that is not in the tree (name at link time: %s)"), *Where, *Link.Name.ToString()));
					}
				}
			}
		}

		static const UScriptStruct* ExpectedBaseFor(const FString& Role)
		{
			if (Role == TEXT("EnterCondition") || Role == TEXT("TransitionCondition"))
			{
				return FStateTreeConditionBase::StaticStruct();
			}
			if (Role == TEXT("Task") || Role == TEXT("SingleTask") || Role == TEXT("GlobalTask"))
			{
				return FStateTreeTaskBase::StaticStruct();
			}
			if (Role == TEXT("Evaluator"))
			{
				return FStateTreeEvaluatorBase::StaticStruct();
			}
			if (Role == TEXT("Consideration"))
			{
				return FStateTreeConsiderationBase::StaticStruct();
			}
			if (Role == TEXT("PropertyFunction"))
			{
				return FStateTreePropertyFunctionBase::StaticStruct();
			}
			return nullptr;
		}

		void ScanNode(const FStateTreeEditorNode& Node, const FString& Role, const bool bExpression, const FString& Where)
		{
			++NumNodes;
			Inv.Add(TEXT("Nodes.Role"), Role);

			const UScriptStruct* NodeStruct = Node.Node.IsValid() ? Node.Node.GetScriptStruct() : nullptr;
			if (!NodeStruct)
			{
				Inv.Add(TEXT("Nodes.Type.") + Role, TEXT("<no node struct>"));
				Unresolved(FString::Printf(TEXT("%s: the %s has no node struct"), *Where, *Role));
				return;
			}

			// A Blueprint node is a wrapper struct around an instance object; the interesting type is the class.
			const FString TypeKey = Node.InstanceObject ? FString::Printf(TEXT("BP:%s"), *Node.InstanceObject->GetClass()->GetPathName()) : NodeStruct->GetName();
			Inv.Add(TEXT("Nodes.Type.") + Role, TypeKey);

			if (const UScriptStruct* ExpectedBase = ExpectedBaseFor(Role))
			{
				if (!NodeStruct->IsChildOf(ExpectedBase))
				{
					Unresolved(FString::Printf(TEXT("%s: the %s uses %s, which is not a %s"), *Where, *Role, *NodeStruct->GetName(), *ExpectedBase->GetName()));
				}
			}

			Inv.Add(TEXT("Nodes.InstanceKind"), Node.InstanceObject ? TEXT("Object") : (Node.Instance.IsValid() ? TEXT("Struct") : TEXT("None")));
			Inv.Add(TEXT("Nodes.RuntimeDataKind"), Node.ExecutionRuntimeDataObject ? TEXT("Object") : (Node.ExecutionRuntimeData.IsValid() ? TEXT("Struct") : TEXT("None")));

			if (const FNameProperty* NameProperty = CastField<FNameProperty>(NodeStruct->FindPropertyByName(TEXT("Name"))))
			{
				Inv.AddFlag(TEXT("Nodes.HasNameOverride"), !NameProperty->GetPropertyValue_InContainer(Node.Node.GetMemory()).IsNone());
			}

			if (bExpression)
			{
				Inv.Add(TEXT("Expression.Operand.") + Role, EnumToString(Node.ExpressionOperand));
				Inv.Add(TEXT("Expression.Indent.") + Role, FString::FromInt(Node.ExpressionIndent));
			}

			if (const FStateTreeConditionBase* Condition = Node.Node.GetPtr<FStateTreeConditionBase>())
			{
				Inv.Add(TEXT("Conditions.EvaluationMode"), EnumToString(Condition->EvaluationMode));
			}
			if (const FStateTreeTaskBase* Task = Node.Node.GetPtr<FStateTreeTaskBase>())
			{
				Inv.AddFlag(TEXT("Tasks.Enabled"), Task->bTaskEnabled);
				Inv.AddFlag(TEXT("Tasks.ConsideredForCompletion"), Task->bConsideredForCompletion);
				Inv.Add(TEXT("Tasks.TransitionHandlingPriority"), EnumToString(Task->TransitionHandlingPriority));
			}

			// bInvert lives on the node or on its instance data, depending on the condition.
			bool bInvert = false;
			if (ReadBoolProperty(NodeStruct, Node.Node.GetMemory(), TEXT("bInvert"), bInvert)
				|| (Node.Instance.IsValid() && ReadBoolProperty(Node.Instance.GetScriptStruct(), Node.Instance.GetMemory(), TEXT("bInvert"), bInvert)))
			{
				Inv.AddFlag(TEXT("Nodes.bInvert"), bInvert);
			}

			// State links, property refs and delegates inside the node's own data.
			FStructScan Found;
			ScanStruct(NodeStruct, Node.Node.GetMemory(), Found, 0);
			if (Node.InstanceObject)
			{
				ScanStruct(Node.InstanceObject->GetClass(), Node.InstanceObject.Get(), Found, 0);
			}
			else if (Node.Instance.IsValid())
			{
				ScanStruct(Node.Instance.GetScriptStruct(), Node.Instance.GetMemory(), Found, 0);
			}
			if (Node.ExecutionRuntimeData.IsValid())
			{
				ScanStruct(Node.ExecutionRuntimeData.GetScriptStruct(), Node.ExecutionRuntimeData.GetMemory(), Found, 0);
			}
			RecordScan(Found, Where);
		}

		void ScanEvent(const FStateTreeEventDesc& Event, const TCHAR* Owner)
		{
			Inv.AddFlag(FString::Printf(TEXT("Events.%s.HasTag"), Owner), Event.Tag.IsValid());
			Inv.Add(FString::Printf(TEXT("Events.%s.PayloadStruct"), Owner), PathText(Event.PayloadStruct));
			Inv.AddFlag(FString::Printf(TEXT("Events.%s.ConsumeOnSelect"), Owner), Event.bConsumeEventOnSelect);
		}

		void ScanStates(const TArray<TObjectPtr<UStateTreeState>>& States, const int32 Depth, const FString& ParentPath)
		{
			for (const UStateTreeState* State : States)
			{
				if (!State)
				{
					Unresolved(FString::Printf(TEXT("%s: a null child state"), *ParentPath));
					continue;
				}
				const FString Path = ParentPath.IsEmpty() ? State->Name.ToString() : ParentPath + TEXT("/") + State->Name.ToString();
				ScanState(*State, Depth, Path);
				ScanStates(State->Children, Depth + 1, Path);
			}
		}

		void ScanState(const UStateTreeState& State, const int32 Depth, const FString& Path)
		{
			++NumStates;
			Inv.Add(TEXT("States.Depth"), FString::FromInt(Depth));
			Inv.Add(TEXT("States.Type"), EnumToString(State.Type));
			Inv.Add(TEXT("States.SelectionBehavior"), EnumToString(State.SelectionBehavior));
			Inv.Add(TEXT("States.TasksCompletion"), EnumToString(State.TasksCompletion));
			Inv.AddFlag(TEXT("States.Enabled"), State.bEnabled);
			Inv.AddFlag(TEXT("States.HasTag"), State.Tag.IsValid());
			Inv.AddFlag(TEXT("States.HasDescription"), !State.Description.IsEmpty());
			Inv.AddFlag(TEXT("States.HasCustomTickRate"), State.bHasCustomTickRate);
			Inv.AddFlag(TEXT("States.CopyParameterBindingsOnTick"), State.bCopyParameterBindingsOnTick);
			Inv.AddFlag(TEXT("States.CheckPrerequisitesWhenActivatingChildDirectly"), State.bCheckPrerequisitesWhenActivatingChildDirectly);
			Inv.AddFlag(TEXT("States.HasRequiredEventToEnter"), State.bHasRequiredEventToEnter);
			Inv.AddFlag(TEXT("States.NonDefaultWeight"), !FMath::IsNearlyEqual(State.Weight, 1.0f));
			Inv.AddFlag(TEXT("States.HasSingleTask"), State.SingleTask.Node.IsValid());
			Inv.Add(TEXT("States.EnterConditionCount"), FString::FromInt(State.EnterConditions.Num()));
			Inv.Add(TEXT("States.ConsiderationCount"), FString::FromInt(State.Considerations.Num()));
			Inv.Add(TEXT("States.TaskCount"), FString::FromInt(State.Tasks.Num()));
			Inv.Add(TEXT("States.TransitionCount"), FString::FromInt(State.Transitions.Num()));

			if (State.bHasRequiredEventToEnter)
			{
				ScanEvent(State.RequiredEventToEnter, TEXT("StateEnter"));
			}

			// Color
			if (State.ColorRef.ID.IsValid())
			{
				const bool bFound = Data.FindColor(State.ColorRef) != nullptr;
				Inv.AddFlag(TEXT("States.ColorResolved"), bFound);
				if (!bFound)
				{
					Unresolved(FString::Printf(TEXT("%s: the state color refers to a color that is not in the color table"), *Path));
				}
			}
			else
			{
				Inv.Add(TEXT("States.ColorResolved"), TEXT("no color"));
			}

			// Links
			if (State.LinkedSubtree.LinkType != EStateTreeTransitionType::None || State.LinkedSubtree.ID.IsValid())
			{
				Inv.Add(TEXT("States.LinkedSubtreeLinkType"), EnumToString(State.LinkedSubtree.LinkType));
				const UStateTreeState* Target = Data.GetStateByID(State.LinkedSubtree.ID);
				Inv.AddFlag(TEXT("States.LinkedSubtreeResolved"), Target != nullptr);
				if (!Target)
				{
					Unresolved(FString::Printf(TEXT("%s: the linked subtree is not in the tree (name at link time: %s)"), *Path, *State.LinkedSubtree.Name.ToString()));
				}
				else if (Target->Type != EStateTreeStateType::Subtree)
				{
					Unresolved(FString::Printf(TEXT("%s: the linked subtree %s is not of type Subtree"), *Path, *Target->Name.ToString()));
				}
			}
			Inv.AddFlag(TEXT("States.HasLinkedAsset"), State.LinkedAsset != nullptr);
			if (State.Type == EStateTreeStateType::Linked && !State.LinkedSubtree.ID.IsValid())
			{
				Unresolved(FString::Printf(TEXT("%s: a Linked state without a linked subtree"), *Path));
			}
			if (State.Type == EStateTreeStateType::LinkedAsset && !State.LinkedAsset)
			{
				Unresolved(FString::Printf(TEXT("%s: a LinkedAsset state without a linked asset"), *Path));
			}

			// Parameters
			ScanBag(State.Parameters.Parameters, TEXT("StateParameters"));
			Inv.Add(TEXT("StateParameters.OverrideCount"), FString::FromInt(State.Parameters.PropertyOverrides.Num()));
			Inv.AddFlag(TEXT("StateParameters.FixedLayout"), State.Parameters.bFixedLayout);

			// Nodes
			for (int32 Index = 0; Index < State.EnterConditions.Num(); ++Index)
			{
				ScanNode(State.EnterConditions[Index], TEXT("EnterCondition"), true, FString::Printf(TEXT("%s/EnterCondition[%d]"), *Path, Index));
			}
			for (int32 Index = 0; Index < State.Considerations.Num(); ++Index)
			{
				ScanNode(State.Considerations[Index], TEXT("Consideration"), true, FString::Printf(TEXT("%s/Consideration[%d]"), *Path, Index));
			}
			for (int32 Index = 0; Index < State.Tasks.Num(); ++Index)
			{
				ScanNode(State.Tasks[Index], TEXT("Task"), false, FString::Printf(TEXT("%s/Task[%d]"), *Path, Index));
			}
			if (State.SingleTask.Node.IsValid())
			{
				ScanNode(State.SingleTask, TEXT("SingleTask"), false, FString::Printf(TEXT("%s/SingleTask"), *Path));
			}

			for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
			{
				ScanTransition(State.Transitions[Index], FString::Printf(TEXT("%s/Transition[%d]"), *Path, Index));
			}
		}

		void ScanTransition(const FStateTreeTransition& Transition, const FString& Where)
		{
			Inv.Add(TEXT("Transitions.Trigger"), EnumToString(Transition.Trigger));
			Inv.Add(TEXT("Transitions.Priority"), EnumToString(Transition.Priority));
			Inv.Add(TEXT("Transitions.TargetType"), EnumToString(Transition.State.LinkType));
			Inv.Add(TEXT("Transitions.Fallback"), EnumToString(Transition.State.Fallback));
			Inv.AddFlag(TEXT("Transitions.Enabled"), Transition.bTransitionEnabled);
			Inv.AddFlag(TEXT("Transitions.Delay"), Transition.bDelayTransition);
			Inv.AddFlag(TEXT("Transitions.DelayVariance"), Transition.bDelayTransition && Transition.DelayRandomVariance > 0.0f);
			Inv.Add(TEXT("Transitions.ReactivateTargetState"), EnumToString(Transition.ReactivateTargetState));
			Inv.Add(TEXT("Transitions.ConditionCount"), FString::FromInt(Transition.Conditions.Num()));
			Inv.AddFlag(TEXT("Transitions.HasRequiredEvent"), Transition.RequiredEvent.IsValid());
			if (Transition.RequiredEvent.IsValid())
			{
				ScanEvent(Transition.RequiredEvent, TEXT("Transition"));
			}
			if (Transition.State.LinkType == EStateTreeTransitionType::GotoState)
			{
				const bool bFound = Data.GetStateByID(Transition.State.ID) != nullptr;
				Inv.AddFlag(TEXT("Transitions.GotoTargetResolved"), bFound);
				if (!bFound)
				{
					Unresolved(FString::Printf(TEXT("%s: the transition target is not in the tree (name at link time: %s)"), *Where, *Transition.State.Name.ToString()));
				}
			}
			for (int32 Index = 0; Index < Transition.Conditions.Num(); ++Index)
			{
				ScanNode(Transition.Conditions[Index], TEXT("TransitionCondition"), true, FString::Printf(TEXT("%s/Condition[%d]"), *Where, Index));
			}
		}

		// -- Bindings --

		struct FSourceInfo
		{
			EStateTreeBindableStructSource Source = EStateTreeBindableStructSource::Context;
			bool bNodeTemplate = false;
		};

		FString KindText(const TMap<FGuid, FSourceInfo>& Sources, const FGuid& Id) const
		{
			if (const FSourceInfo* Info = Sources.Find(Id))
			{
				return EnumToString(Info->Source) + (Info->bNodeTemplate ? TEXT("(node)") : TEXT(""));
			}
			return TEXT("<unresolved>");
		}

		/** Resolves the path against the live data of its struct. Returns false (and records why) if that fails. */
		bool ResolveEndpoint(const FPropertyBindingPath& Path, const TCHAR* Which, const FString& Describe, const FProperty*& OutLeaf)
		{
			OutLeaf = nullptr;

			FPropertyBindingDataView View;
			if (!Data.GetBindingDataViewByID(Path.GetStructID(), View))
			{
				Unresolved(FString::Printf(TEXT("binding %s: the %s struct id is not in the tree"), *Describe, Which));
				return false;
			}

			TArray<FPropertyBindingPathIndirection> Indirections;
			FString Error;
			bool bResolved = Path.ResolveIndirectionsWithValue(View, Indirections, &Error, /*bHandleRedirects*/ false);
			if (!bResolved)
			{
				bResolved = Path.ResolveIndirectionsWithValue(View, Indirections, &Error, /*bHandleRedirects*/ true);
				if (bResolved)
				{
					Inv.Add(TEXT("Bindings.PathNeedsRedirect"), Which);
				}
			}
			if (!bResolved)
			{
				Unresolved(FString::Printf(TEXT("binding %s: the %s path does not resolve: %s"), *Describe, Which, *Error));
				return false;
			}

			if (!Indirections.IsEmpty())
			{
				OutLeaf = Indirections.Last().GetProperty();
			}
			return true;
		}

		static FString LeafKind(const FProperty* Leaf)
		{
			if (!Leaf)
			{
				return TEXT("(struct itself)");
			}
			if (UE::StateTree::PropertyRefHelpers::IsPropertyRef(*Leaf))
			{
				return TEXT("PropertyRef");
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Leaf))
			{
				if (StructProperty->Struct == FStateTreeDelegateListener::StaticStruct() || StructProperty->Struct == FStateTreeTransitionDelegateListener::StaticStruct())
				{
					return TEXT("DelegateListener");
				}
				if (StructProperty->Struct == FStateTreeDelegateDispatcher::StaticStruct())
				{
					return TEXT("DelegateDispatcher");
				}
			}
			return TEXT("Plain");
		}

		void ScanBindings()
		{
			// The engine's own walk gives every bindable struct a source kind; it is the reference for "can this ID be resolved".
			TMap<FGuid, FSourceInfo> Sources;
			Data.VisitAllNodes([&Sources](const UStateTreeState*, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView)
			{
				FSourceInfo Info;
				Info.Source = Desc.DataSource;
				Info.bNodeTemplate = Desc.Struct && Desc.Struct->IsChildOf(FStateTreeNodeBase::StaticStruct());
				Sources.Add(Desc.ID, Info);
				return EStateTreeVisitor::Continue;
			});

			// For nested property functions: function instance id -> the id of the struct the function feeds.
			TMap<FGuid, FGuid> FunctionTargets;
			for (const FStateTreePropertyPathBinding& Binding : Data.EditorBindings.GetBindings())
			{
				if (const FStateTreeEditorNode* Function = GetFunctionNode(Binding))
				{
					FunctionTargets.Add(Function->ID, Binding.GetTargetPath().GetStructID());
				}
			}

			int32 BindingIndex = 0;
			for (const FStateTreePropertyPathBinding& Binding : Data.EditorBindings.GetBindings())
			{
				++NumBindings;

				const FPropertyBindingPath& SourcePath = Binding.GetSourcePath();
				const FPropertyBindingPath& TargetPath = Binding.GetTargetPath();
				const FString SourceKind = KindText(Sources, SourcePath.GetStructID());
				const FString TargetKind = KindText(Sources, TargetPath.GetStructID());
				const FString Describe = FString::Printf(TEXT("#%d (%s -> %s, target path %s)"), BindingIndex++, *SourceKind, *TargetKind, *TargetPath.ToString());

				Inv.Add(TEXT("Bindings.SourceKind"), SourceKind);
				Inv.Add(TEXT("Bindings.TargetKind"), TargetKind);
				Inv.Add(TEXT("Bindings.SourceToTarget"), SourceKind + TEXT(" -> ") + TargetKind);
				Inv.Add(TEXT("Bindings.SourcePathSegments"), FString::FromInt(SourcePath.NumSegments()));
				Inv.AddFlag(TEXT("Bindings.PathHasInstancedIndirection"), SourcePath.HasAnyInstancedIndirection() || TargetPath.HasAnyInstancedIndirection());

				bool bArrayIndex = false;
				for (const FPropertyBindingPathSegment& Segment : SourcePath.GetSegments())
				{
					bArrayIndex = bArrayIndex || Segment.GetArrayIndex() >= 0;
				}
				for (const FPropertyBindingPathSegment& Segment : TargetPath.GetSegments())
				{
					bArrayIndex = bArrayIndex || Segment.GetArrayIndex() >= 0;
				}
				Inv.AddFlag(TEXT("Bindings.PathHasArrayIndex"), bArrayIndex);

				if (Binding.IsOutputBinding())
				{
					Inv.Add(TEXT("Bindings.Output"), SourceKind + TEXT(" <- ") + TargetKind);
				}

				const TOptional<UE::StateTree::ETaskCompletionCondition> Completion = Binding.GetCompletionBinding();
				if (Completion.IsSet())
				{
					Inv.Add(TEXT("Bindings.TaskCompletion"), CompletionConditionText(Completion.GetValue()));
				}

				if (const FStateTreeEditorNode* Function = GetFunctionNode(Binding))
				{
					Inv.Add(TEXT("Bindings.PropertyFunctionType"), Function->Node.IsValid() ? Function->Node.GetScriptStruct()->GetName() : FString(TEXT("<no node struct>")));

					// 1 = the function feeds a node or state directly; 2 = it feeds another function, and so on.
					int32 Nesting = 1;
					FGuid Current = TargetPath.GetStructID();
					while (const FGuid* Next = FunctionTargets.Find(Current))
					{
						++Nesting;
						Current = *Next;
						if (Nesting > 16)
						{
							break;
						}
					}
					Inv.Add(TEXT("Bindings.PropertyFunctionNesting"), FString::FromInt(Nesting));

					ScanNode(*Function, TEXT("PropertyFunction"), false, FString::Printf(TEXT("function bound at %s"), *Describe));
				}

				const FProperty* SourceLeaf = nullptr;
				const FProperty* TargetLeaf = nullptr;
				if (ResolveEndpoint(SourcePath, TEXT("source"), Describe, SourceLeaf))
				{
					Inv.Add(TEXT("Bindings.SourceLeaf"), LeafKind(SourceLeaf));
				}
				if (ResolveEndpoint(TargetPath, TEXT("target"), Describe, TargetLeaf))
				{
					Inv.Add(TEXT("Bindings.TargetLeaf"), LeafKind(TargetLeaf));
				}
			}
		}

		const UStateTreeEditorData& Data;
		FInventory& Inv;
	};

	// ---- Per tree record and report --------------------------------------------------------------------------

	struct FTreeRecord
	{
		FString ObjectPath;
		FString ReportName;
		bool bLoaded = false;
		bool bCompiled = false;
		int32 NumMessages = 0;
		int32 NumErrors = 0;
		int32 NumWarnings = 0;
		TArray<FString> LogMessages;
		bool bCompileChangedEditorData = false;
		bool bDumpDeterministic = true;
		int32 NumStates = 0;
		int32 NumNodes = 0;
		int32 NumBindings = 0;
		FString EditorDataClass;
		FInventory Inventory;
	};

	void CompileTree(UStateTree& StateTree, FTreeRecord& Record)
	{
		FStateTreeCompilerLog Log;
		Record.bCompiled = UStateTreeEditingSubsystem::CompileStateTree(&StateTree, Log);

		for (const TSharedRef<FTokenizedMessage>& Message : Log.ToTokenizedMessages())
		{
			++Record.NumMessages;
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (IsErrorSeverity(Severity))
			{
				++Record.NumErrors;
			}
			else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
			{
				++Record.NumWarnings;
			}
			if (Record.LogMessages.Num() < 20)
			{
				Record.LogMessages.Add(FString::Printf(TEXT("[%s] %s"), SeverityText(Severity), *Message->ToText().ToString()));
			}
		}
	}

	FString BuildInventoryReport(const TArray<FTreeRecord>& Records, const FInventory& Totals)
	{
		int32 NumLoaded = 0;
		int32 NumCompiled = 0;
		for (const FTreeRecord& Record : Records)
		{
			NumLoaded += Record.bLoaded ? 1 : 0;
			NumCompiled += Record.bCompiled ? 1 : 0;
		}

		FString Out;
		Out += TEXT("StateTree corpus inventory\n");
		Out += FString::Printf(TEXT("trees=%d loaded=%d compiled=%d\n\n"), Records.Num(), NumLoaded, NumCompiled);

		Out += TEXT("== Totals ==\n");
		Totals.Append(Out, FString());

		int32 NumUnresolved = 0;
		for (const FTreeRecord& Record : Records)
		{
			NumUnresolved += Record.Inventory.Unresolved.Num();
		}
		Out += FString::Printf(TEXT("\n== Unresolved (%d) ==\n"), NumUnresolved);
		for (const FTreeRecord& Record : Records)
		{
			for (const FString& Text : Record.Inventory.Unresolved)
			{
				Out += FString::Printf(TEXT("%s: %s\n"), *Record.ObjectPath, *Text);
			}
		}

		Out += TEXT("\n== Compile ==\n");
		for (const FTreeRecord& Record : Records)
		{
			Out += FString::Printf(TEXT("%s: loaded=%s compiled=%s messages=%d errors=%d warnings=%d compileChangedEditorData=%s dumpDeterministic=%s\n"),
				*Record.ObjectPath, *BoolText(Record.bLoaded), *BoolText(Record.bCompiled), Record.NumMessages, Record.NumErrors, Record.NumWarnings,
				*BoolText(Record.bCompileChangedEditorData), *BoolText(Record.bDumpDeterministic));
			for (const FString& Message : Record.LogMessages)
			{
				Out += FString::Printf(TEXT("    %s\n"), *Message);
			}
		}

		Out += TEXT("\n== Per tree ==\n");
		for (const FTreeRecord& Record : Records)
		{
			Out += FString::Printf(TEXT("-- %s (states=%d nodes=%d bindings=%d editorData=%s)\n"), *Record.ObjectPath, Record.NumStates, Record.NumNodes, Record.NumBindings, *Record.EditorDataClass);
			Record.Inventory.Append(Out, TEXT("  "));
		}
		return Out;
	}

	// ---- The corpus run --------------------------------------------------------------------------------------

	void RunCorpus(FAutomationTestBase& Test)
	{
		FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = RegistryModule.Get();
		if (!Registry.IsSearchAllAssets())
		{
			Registry.SearchAllAssets(/*bSynchronousSearch*/ true);
		}
		else
		{
			Registry.WaitForCompletion();
		}

		FARFilter Filter;
		Filter.ClassPaths.Add(UStateTree::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return CaseSensitiveLess(A.GetSoftObjectPath().ToString(), B.GetSoftObjectPath().ToString());
		});

		if (Assets.IsEmpty())
		{
			Test.AddError(TEXT("No StateTree asset was found under /Game."));
			return;
		}

		FDumpOptions DefaultOptions;
		FDumpOptions AllValuesOptions;
		AllValuesOptions.bAllValues = true;

		TArray<FTreeRecord> Records;
		FInventory Totals;

		for (const FAssetData& Asset : Assets)
		{
			FTreeRecord& Record = Records.AddDefaulted_GetRef();
			Record.ObjectPath = Asset.GetSoftObjectPath().ToString();
			Record.ReportName = ReportNameFor(Asset.PackageName.ToString());

			UStateTree* StateTree = Cast<UStateTree>(Asset.GetAsset());
			if (!StateTree)
			{
				Test.AddError(FString::Printf(TEXT("Could not load %s."), *Record.ObjectPath));
				continue;
			}
			Record.bLoaded = true;

			UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
			if (!EditorData)
			{
				Test.AddError(FString::Printf(TEXT("%s has no editor data."), *Record.ObjectPath));
				continue;
			}
			Record.EditorDataClass = EditorData->GetClass()->GetName();

			// What compiling does to the editor data is a finding in itself (it validates and fixes up IDs).
			const FString BeforeCompile = DumpEditorData(*EditorData, AllValuesOptions);

			CompileTree(*StateTree, Record);
			if (!Record.bCompiled)
			{
				Test.AddError(FString::Printf(TEXT("%s did not compile (%d errors, %d warnings)."), *Record.ObjectPath, Record.NumErrors, Record.NumWarnings));
			}

			const FString AfterCompile = DumpEditorData(*EditorData, AllValuesOptions);
			const FString CompileDiff = DiffLines(BeforeCompile, AfterCompile);
			if (!CompileDiff.IsEmpty())
			{
				Record.bCompileChangedEditorData = true;
				SaveReport(TEXT("Read"), Record.ReportName + TEXT(".compile-changes"), CompileDiff);
			}

			const FString Dump = DumpEditorData(*EditorData, DefaultOptions);
			Record.bDumpDeterministic = DiffLines(Dump, DumpEditorData(*EditorData, DefaultOptions)).IsEmpty();
			SaveReport(TEXT("Read"), Record.ReportName, Dump);
			SaveReport(TEXT("Read"), Record.ReportName + TEXT(".all"), AfterCompile);
			SaveReport(TEXT("Read"), Record.ReportName + TEXT(".compiled"), DumpCompiled(*StateTree));

			FEditorDataScanner Scanner(*EditorData, Record.Inventory);
			Scanner.Scan();
			Record.NumStates = Scanner.NumStates;
			Record.NumNodes = Scanner.NumNodes;
			Record.NumBindings = Scanner.NumBindings;

			Record.Inventory.Add(TEXT("Trees.Compile"), Record.bCompiled ? TEXT("ok") : TEXT("failed"));
			Record.Inventory.Add(TEXT("Trees.CompileMessages"), Record.NumMessages == 0 ? TEXT("none") : (Record.NumErrors > 0 ? TEXT("errors") : TEXT("warnings or info")));
			Record.Inventory.AddFlag(TEXT("Trees.CompileChangedEditorData"), Record.bCompileChangedEditorData);
			Record.Inventory.AddFlag(TEXT("Trees.DumpDeterministic"), Record.bDumpDeterministic);
			Totals.Merge(Record.Inventory);
		}

		const FString InventoryPath = SaveReport(TEXT("Read"), TEXT("Inventory"), BuildInventoryReport(Records, Totals));

		// One line per tree, then the totals that matter most. The full text is in the inventory report.
		int32 TotalStates = 0;
		int32 TotalNodes = 0;
		int32 TotalBindings = 0;
		int32 TotalUnresolved = 0;
		for (const FTreeRecord& Record : Records)
		{
			TotalStates += Record.NumStates;
			TotalNodes += Record.NumNodes;
			TotalBindings += Record.NumBindings;
			TotalUnresolved += Record.Inventory.Unresolved.Num();
			Test.AddInfo(FString::Printf(TEXT("%s: states=%d nodes=%d bindings=%d compile=%s (%d errors, %d warnings) unresolved=%d editorData=%s"),
				*Record.ObjectPath, Record.NumStates, Record.NumNodes, Record.NumBindings, Record.bCompiled ? TEXT("ok") : TEXT("FAILED"),
				Record.NumErrors, Record.NumWarnings, Record.Inventory.Unresolved.Num(), *Record.EditorDataClass));
		}
		Test.AddInfo(FString::Printf(TEXT("Total: %d trees, %d states, %d nodes, %d bindings, %d unresolved"), Records.Num(), TotalStates, TotalNodes, TotalBindings, TotalUnresolved));

		static const TCHAR* SummaryCategories[] =
		{
			TEXT("EditorData.Class"), TEXT("EditorData.IsBaseClass"), TEXT("Schema.Class"), TEXT("Extensions.Class"),
			TEXT("States.Type"), TEXT("States.SelectionBehavior"), TEXT("States.TasksCompletion"),
			TEXT("Transitions.Trigger"), TEXT("Transitions.TargetType"), TEXT("Transitions.Priority"),
			TEXT("Nodes.Role"), TEXT("Bindings.SourceKind"), TEXT("Bindings.PropertyFunctionNesting"), TEXT("Bindings.Output"),
			TEXT("Bindings.TaskCompletion"), TEXT("Bindings.TargetLeaf"), TEXT("Bindings.PathNeedsRedirect"),
			TEXT("NodeProperties.StateLinkType"), TEXT("Events.Transition.PayloadStruct"), TEXT("Trees.CompileChangedEditorData"),
		};
		for (const TCHAR* Category : SummaryCategories)
		{
			const FString Summary = Totals.Summary(Category);
			if (!Summary.IsEmpty())
			{
				Test.AddInfo(FString::Printf(TEXT("[%s] %s"), Category, *Summary));
			}
		}

		for (int32 Index = 0; Index < Records.Num(); ++Index)
		{
			for (const FString& Text : Records[Index].Inventory.Unresolved)
			{
				Test.AddInfo(FString::Printf(TEXT("unresolved: %s: %s"), *Records[Index].ObjectPath, *Text));
			}
		}
		Test.AddInfo(FString::Printf(TEXT("Reports are in %s"), *FPaths::GetPath(InventoryPath)));
	}
} // namespace
} // namespace UE::DreamStateTree::Spike

// Loading a corpus tree also loads what it references; in the test host the templates' Blueprints lack their
// shared content (meshes, skeletons) and log errors that say nothing about the trees, so log errors do not fail it.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDreamStateTreeSpikeReadCorpusTest, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Read.Corpus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamStateTreeSpikeReadCorpusTest::RunTest(const FString& Parameters)
{
	UE::DreamStateTree::Spike::RunCorpus(*this);
	return !HasAnyErrors();
}
