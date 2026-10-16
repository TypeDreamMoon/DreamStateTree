// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeBuildHelpers.h"
#include "Spike/SpikeDump.h"

#include "Conditions/StateTreeCommonConditions.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorPropertyBindings.h"

// Things the StateTree editor does for the user while a tree is being edited, and code that builds a tree does not.
// Each experiment builds the tree once the way the editor leaves it and once the way plain code would, compiles
// both, and reports what differs.

namespace UE::DreamStateTree::Spike
{
	namespace BuildEditorHooksPrivate
	{
		FString DescribeEnum(const FStateTreeAnyEnum& Value)
		{
			return FString::Printf(TEXT("%s (value %u)"), Value.Enum ? *Value.Enum->GetName() : TEXT("null enum"), Value.Value);
		}

		FString BagPropertyNames(const FInstancedPropertyBag& Bag)
		{
			TArray<FString> Names;
			if (const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
				{
					Names.Add(Desc.Name.ToString());
				}
			}
			return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
		}

		/** Root > S, with an Enum Compare enter condition on S whose Left reads the owner's Mode. Returns the condition's node ID. */
		FGuid BuildEnumTree(FDstBuildTreeBuilder& B)
		{
			UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
			Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
			UStateTreeState& State = B.AddChild(Root, TEXT("S"));
			State.SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;

			TStateTreeEditorNode<FStateTreeCompareEnumCondition>& Condition = B.AddEnterCondition<FStateTreeCompareEnumCondition>(State, TEXT("EnumCondition"));
			const FGuid ConditionId = Condition.ID;
			// Left is read from the context object's enum property; Right keeps the constant written into the node.
			B.Bind(UDstTestSchema::GetOwnerContextId(), TEXT("Mode"), ConditionId, TEXT("Left"));
			return ConditionId;
		}

		FStateTreeEditorNode* FindEnterCondition(UStateTreeEditorData& EditorData, const FName StateName, const FGuid& NodeId)
		{
			FStateTreeEditorNode* Result = nullptr;
			EditorData.VisitHierarchy([&](UStateTreeState& State, UStateTreeState* /*Parent*/)
			{
				if (State.Name == StateName)
				{
					for (FStateTreeEditorNode& Node : State.EnterConditions)
					{
						if (Node.ID == NodeId)
						{
							Result = &Node;
						}
					}
				}
				return Result ? EStateTreeVisitor::Break : EStateTreeVisitor::Continue;
			});
			return Result;
		}
	}
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstSpikeBuild_EditorHooks, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Build.EditorHooks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeBuild_EditorHooks::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstBuildAssetKeeper Keeper;
	FDstBuildLogCapture LogCapture;
	FDstBuildReport Report;

	//----------------------------------------------------------------------//
	// 1. FStateTreeCompareEnumCondition: the enum type of Left and Right is set by OnBindingChanged, which the
	//    Details panel calls when the user binds a property.
	//----------------------------------------------------------------------//
	Report.Heading(TEXT("1. Enum Compare condition"));
	{
		struct FEnumVariant
		{
			const TCHAR* Name;
			bool bCallOnBindingChanged;
			bool bSetEnumsByHand;
		};
		const FEnumVariant Variants[] = {
			{ TEXT("Bound by code, enum types never set"), false, false },
			{ TEXT("Bound by code, then OnBindingChanged called the way the Details panel does"), true, false },
			{ TEXT("Bound by code, enum types set by hand (AnyEnum::Initialize)"), false, true },
		};

		for (const FEnumVariant& Variant : Variants)
		{
			UStateTree* Tree = Keeper.NewTransientTree(TEXT("EnumCondition"), UDstTestSchema::StaticClass());
			FDstBuildTreeBuilder Builder(*Tree);
			const FGuid ConditionId = UE::DreamStateTree::Spike::BuildEditorHooksPrivate::BuildEnumTree(Builder);
			UStateTreeEditorData& EditorData = Builder.GetEditorData();
			FStateTreeEditorNode* Node = UE::DreamStateTree::Spike::BuildEditorHooksPrivate::FindEnterCondition(EditorData, TEXT("S"), ConditionId);
			if (!TestNotNull(TEXT("The enum condition is found"), Node))
			{
				continue;
			}
			FStateTreeCompareEnumCondition* Condition = Node->GetNode().GetPtr<FStateTreeCompareEnumCondition>();
			FStateTreeCompareEnumConditionInstanceData* Instance = Node->GetInstance().GetMutablePtr<FStateTreeCompareEnumConditionInstanceData>();
			if (!TestNotNull(TEXT("The enum condition node and instance data are found"), Condition) || !TestNotNull(TEXT("The enum condition instance data is found"), Instance))
			{
				continue;
			}

			const FString Before = UE::DreamStateTree::Spike::DumpEditorData(EditorData);
			if (Variant.bCallOnBindingChanged)
			{
				const FPropertyBindingPath SourcePath = FDstBuildTreeBuilder::MakePath(UDstTestSchema::GetOwnerContextId(), TEXT("Mode"));
				const FPropertyBindingPath TargetPath = FDstBuildTreeBuilder::MakePath(ConditionId, TEXT("Left"));
				const FStateTreeBindingLookup BindingLookup(&EditorData);
				Condition->OnBindingChanged(ConditionId, Node->GetInstance(), SourcePath, TargetPath, BindingLookup);
			}
			if (Variant.bSetEnumsByHand)
			{
				Instance->Left.Initialize(StaticEnum<EDstTestMode>());
				Instance->Right.Initialize(StaticEnum<EDstTestMode>());
			}
			const FString EnumsAfterHook = FString::Printf(TEXT("Left = %s, Right = %s"), *UE::DreamStateTree::Spike::BuildEditorHooksPrivate::DescribeEnum(Instance->Left), *UE::DreamStateTree::Spike::BuildEditorHooksPrivate::DescribeEnum(Instance->Right));

			// A constant for Right, written the way the user would pick it in the Details panel.
			if (Instance->Right.Enum)
			{
				Instance->Right.Value = static_cast<uint32>(EDstTestMode::Combat);
			}

			LogCapture.Reset();
			const FDstBuildCompileOutcome Outcome = DstBuildCompile(*Tree);
			const FString After = UE::DreamStateTree::Spike::DumpEditorData(EditorData);

			Report.Line(FString::Printf(TEXT("- %s"), Variant.Name));
			Report.Line(TEXT("  enums after the hook: ") + EnumsAfterHook);
			Report.Line(TEXT("  compile: ") + Outcome.ToString());
			Report.Block(TEXT("  engine log"), FString::Join(LogCapture.GetLines(), TEXT("\n")));
			Report.Block(TEXT("  editor data changed by the hook, the constant and the compile"), UE::DreamStateTree::Spike::DiffLines(Before, After));

			AddInfo(FString::Printf(TEXT("[EditorHooks] EnumCompare '%s': %s; %s%s"), Variant.Name, *EnumsAfterHook, *Outcome.Summary(),
				Outcome.FirstError().IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" | %s"), *Outcome.FirstError())));
		}
	}

	//----------------------------------------------------------------------//
	// 2. Linked state: the editor copies the parameter layout of the linked subtree into the state.
	//----------------------------------------------------------------------//
	Report.Heading(TEXT("2. Linked state and the parameters of the subtree it links to"));
	{
		struct FLinkVariant
		{
			const TCHAR* Name;
			bool bSetLinkedStateApi;
			bool bUpdateParameters;
		};
		const FLinkVariant Variants[] = {
			{ TEXT("Type and LinkedSubtree assigned by hand, UpdateParametersFromLinkedSubtree not called"), false, false },
			{ TEXT("Type and LinkedSubtree assigned by hand, then UpdateParametersFromLinkedSubtree()"), false, true },
			{ TEXT("SetLinkedState() (what the editor calls)"), true, false },
		};

		for (const FLinkVariant& Variant : Variants)
		{
			UStateTree* Tree = Keeper.NewTransientTree(TEXT("LinkedState"), UDstTestSchema::StaticClass());
			FDstBuildTreeBuilder Builder(*Tree);
			Builder.EnsureSpeedParameter();

			UStateTreeState& Root = Builder.AddSubTree(TEXT("Root"));
			Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
			UStateTreeState& Sub = Builder.AddSubTree(TEXT("Sub"), EStateTreeStateType::Subtree);
			Builder.AddStateParameter(Sub, TEXT("Radius"), EPropertyBagPropertyType::Float);
			Builder.AddStateParameter(Sub, TEXT("Count"), EPropertyBagPropertyType::Int32);
			const FGuid SubTask = Builder.AddBoundTask(Sub, TEXT("SubTask"), 2).ID;
			Builder.Bind(Sub.Parameters.ID, TEXT("Radius"), SubTask, TEXT("Scale"));
			Builder.Bind(Sub.Parameters.ID, TEXT("Count"), SubTask, TEXT("TicksToComplete"));

			UStateTreeState& Link = Builder.AddChild(Root, TEXT("Link"), EStateTreeStateType::Linked);
			if (Variant.bSetLinkedStateApi)
			{
				Link.SetLinkedState(Sub.GetLinkToState());
			}
			else
			{
				Link.LinkedSubtree = Sub.GetLinkToState();
				if (Variant.bUpdateParameters)
				{
					Link.UpdateParametersFromLinkedSubtree();
				}
			}

			const FString ParametersBeforeCompile = UE::DreamStateTree::Spike::BuildEditorHooksPrivate::BagPropertyNames(Link.Parameters.Parameters);
			const FString SelectionBefore = StaticEnum<EStateTreeStateSelectionBehavior>()->GetNameStringByValue(static_cast<int64>(Link.SelectionBehavior));
			LogCapture.Reset();
			const FDstBuildCompileOutcome Outcome = DstBuildCompile(*Tree);

			Report.Line(FString::Printf(TEXT("- %s"), Variant.Name));
			Report.Line(FString::Printf(TEXT("  linked state parameters before compile: %s; selection behavior: %s; bFixedLayout: %s"),
				*ParametersBeforeCompile, *SelectionBefore, Link.Parameters.bFixedLayout ? TEXT("true") : TEXT("false")));
			Report.Line(TEXT("  compile: ") + Outcome.ToString());
			Report.Block(TEXT("  engine log"), FString::Join(LogCapture.GetLines(), TEXT("\n")));

			AddInfo(FString::Printf(TEXT("[EditorHooks] LinkedState '%s': parameters before compile = [%s]; %s%s"), Variant.Name, *ParametersBeforeCompile, *Outcome.Summary(),
				Outcome.FirstError().IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" | %s"), *Outcome.FirstError())));
		}
	}

	//----------------------------------------------------------------------//
	// 3. States the editor reshapes when their type changes.
	//----------------------------------------------------------------------//
	Report.Heading(TEXT("3. State type changes the editor handles in PostEditChangeChainProperty"));
	{
		// Group with a task: the editor clears the tasks of a Group.
		{
			UStateTree* Tree = Keeper.NewTransientTree(TEXT("GroupWithTask"), UDstTestSchema::StaticClass());
			FDstBuildTreeBuilder Builder(*Tree);
			UStateTreeState& Root = Builder.AddSubTree(TEXT("Root"));
			Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
			UStateTreeState& Group = Builder.AddChild(Root, TEXT("Group"), EStateTreeStateType::Group);
			Builder.AddBoundTask(Group, TEXT("TaskInGroup"), 2);
			Builder.AddChild(Group, TEXT("Child")).SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
			LogCapture.Reset();
			const FDstBuildCompileOutcome Outcome = DstBuildCompile(*Tree);
			Report.Line(TEXT("- Group state that keeps a task: ") + Outcome.ToString());
			AddInfo(FString::Printf(TEXT("[EditorHooks] Group state with a task: %s%s"), *Outcome.Summary(),
				Outcome.Lines.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" | %s"), *Outcome.Lines[0])));
		}

		// Linked state with children and a leftover selection behavior: the editor sets TryEnterState and keeps the state childless.
		{
			UStateTree* Tree = Keeper.NewTransientTree(TEXT("LinkedWithChildren"), UDstTestSchema::StaticClass());
			FDstBuildTreeBuilder Builder(*Tree);
			Builder.EnsureSpeedParameter();
			UStateTreeState& Root = Builder.AddSubTree(TEXT("Root"));
			Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
			UStateTreeState& Sub = Builder.AddSubTree(TEXT("Sub"), EStateTreeStateType::Subtree);
			Builder.AddBoundTask(Sub, TEXT("SubTask"), 2);
			UStateTreeState& Link = Builder.AddChild(Root, TEXT("Link"), EStateTreeStateType::Linked);
			Link.LinkedSubtree = Sub.GetLinkToState();
			Link.UpdateParametersFromLinkedSubtree();
			// SelectionBehavior stays at the default (try children in order) and the state gets a child.
			Builder.AddChild(Link, TEXT("Orphan")).SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
			LogCapture.Reset();
			const FDstBuildCompileOutcome Outcome = DstBuildCompile(*Tree);
			Report.Line(TEXT("- Linked state with a leftover selection behavior and a child: ") + Outcome.ToString());
			AddInfo(FString::Printf(TEXT("[EditorHooks] Linked state with a child and a leftover selection behavior: %s%s"), *Outcome.Summary(),
				Outcome.Lines.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" | %s"), *Outcome.Lines[0])));
		}
	}

	//----------------------------------------------------------------------//
	// 4. The rest of the list, read from the engine source: steps the editor does that code must do itself.
	//----------------------------------------------------------------------//
	Report.Heading(TEXT("4. Editor steps that building from code skips (read from the engine source)"));
	const TCHAR* const EditorSteps[] = {
		TEXT("FStateTreeEditorNodeDetails::OnBindingChanged calls FStateTreeNodeBase::OnBindingChanged on the node that received the binding; nodes use it to adapt their instance data (Enum Compare sets AnyEnum types). The delegate that triggers it (UE::StateTree::PropertyBinding::OnStateTreePropertyBindingChanged) is only listened to by the Details customization, so UStateTreeEditorData::OnPropertyBindingChanged does not reach the node when no editor is open; call the node's OnBindingChanged directly with a FStateTreeBindingLookup."),
		TEXT("UStateTreeState::PostEditChangeChainProperty reacts to Type changes: Group clears tasks; Linked and LinkedAsset reset tasks, force TryEnterState, fix the parameter layout and copy the layout of the target (UpdateParametersFromLinkedSubtree); other types clear the link fields and unfix the layout. It also gives a fresh ID to a transition added through the array and fills its default trigger and target. Code that sets Type or adds transitions directly must do the same, or use SetLinkedState / SetLinkedStateAsset / AddTransition."),
		TEXT("UStateTreeState::OnTreeCompiled refreshes the parameters of a LinkedAsset state when the linked asset finishes a compile (delegate UE::StateTree::Delegates::OnPostCompile, broadcast by the compiler manager, not by FStateTreeCompiler)."),
		TEXT("UStateTreeEditingSubsystem::ValidateStateTree runs when an editor opens the asset: reparents states, fixes duplicate IDs, removes invalid bindings, updates binding paths, refreshes the names of state links and the parameters of linked states, applies the schema restrictions (UStateTreeEditorSchema::Validate: drops evaluators, enter conditions, considerations and extra tasks the schema does not allow) and sets RF_Transactional on the states."),
		TEXT("UStateTreeEditorData::PostEditChangeChainProperty broadcasts UE::StateTree::Delegates::OnParametersChanged when the root parameters or the schema change, which makes the editor data update its binding paths; code that edits the root parameter bag directly has to call UpdateBindings itself if it adds properties after bindings exist."),
		TEXT("UStateTreeEditingSubsystem::MarkAsModified / UStateTree::MarkAsModified is called by the editor operations and by the builder helpers of the editor data (AddTask, AddTransition and so on); a tree changed by direct member access is not marked, and its compiled data goes stale until it is compiled explicitly."),
		TEXT("FStateTreeEditorNode instance objects (Blueprint nodes) must be outered to the state; the editor fixes outers in FixObjectNodes on load. UStateTreeState::PostLoad calls ConditionalUpdateNodeInstanceData and the nodes' PostLoad."),
		TEXT("UE::StateTree::PropertyHelpers::DispatchPostEditToNodes forwards property edits to the nodes' PostEditNodeChangeChainProperty and PostEditInstanceDataChangeChainProperty."),
	};
	for (const TCHAR* Step : EditorSteps)
	{
		Report.Line(FString::Printf(TEXT("- %s"), Step));
		AddInfo(FString::Printf(TEXT("[EditorHooks] Editor-only step: %s"), Step));
	}

	const FString ReportPath = SaveReport(TEXT("Build"), TEXT("EditorHooks"), Report.ToString());
	AddInfo(FString::Printf(TEXT("Report: %s"), *ReportPath));
	return true;
}
