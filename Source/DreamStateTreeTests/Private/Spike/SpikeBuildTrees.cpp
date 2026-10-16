// Copyright DreamMoon. All Rights Reserved.

// The trees the build experiments share. All of them are built by FDstBuildTreeBuilder, so every ID is derived from
// the logical path of the object and building a tree twice gives the same IDs.

#include "Spike/SpikeBuildHelpers.h"

#include "Fixtures/DstTestTags.h"

namespace UE::DreamStateTree::Spike
{
	FDstBuildMinimalIds DstBuildMinimalTree(FDstBuildTreeBuilder& B)
	{
		FDstBuildMinimalIds Ids;
		B.EnsureSpeedParameter();

		{
			TStateTreeEditorNode<FDstTestEvaluator>& Counter = B.AddEvaluator<FDstTestEvaluator>(TEXT("Counter"));
			Ids.EvaluatorCounter = Counter.ID;
		}

		// Root picks the child with the highest utility, which is what makes the children's considerations count.
		UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
		Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility;
		UStateTreeState& StateA = B.AddChild(Root, TEXT("A"));
		StateA.SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
		UStateTreeState& StateB = B.AddChild(Root, TEXT("B"));
		StateB.SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
		Ids.Root = Root.ID;
		Ids.StateA = StateA.ID;
		Ids.StateB = StateB.ID;

		Ids.TaskA = B.AddBoundTask(StateA, TEXT("TaskA"), 2).ID;
		B.AddBoundTask(StateB, TEXT("TaskB"), 2);

		Ids.EnterConditionA = B.AddBoundEnterCondition(StateA, TEXT("CondA"), Ids.EvaluatorCounter, 0.f).ID;

		{
			TStateTreeEditorNode<FDstTestConsideration>& Consideration = B.AddConsideration<FDstTestConsideration>(StateA, TEXT("ScoreA"));
			Consideration.GetInstanceData().Score = 0.7f;
			Ids.ConsiderationA = Consideration.ID;
		}
		{
			TStateTreeEditorNode<FDstTestConsideration>& Consideration = B.AddConsideration<FDstTestConsideration>(StateB, TEXT("ScoreB"));
			Consideration.GetInstanceData().Score = 0.3f;
		}

		Ids.TransitionAToB = B.AddTransition(StateA, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &StateB).ID;
		return Ids;
	}

	void DstBuildEncounterTree(FDstBuildTreeBuilder& B)
	{
		using namespace UE::DreamStateTree::Tests;

		B.EnsureSpeedParameter();
		{
			TStateTreeEditorNode<FDstTestEvaluator>& Counter = B.AddEvaluator<FDstTestEvaluator>(TEXT("Counter"));
			Counter.GetInstanceData().Step = 1;
		}

		UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
		Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
		UStateTreeState& Idle = B.AddChild(Root, TEXT("Idle"));
		UStateTreeState& Patrol = B.AddChild(Root, TEXT("Patrol"));
		UStateTreeState& Fight = B.AddChild(Root, TEXT("Fight"));
		UStateTreeState& Light = B.AddChild(Fight, TEXT("Light"));
		UStateTreeState& Heavy = B.AddChild(Fight, TEXT("Heavy"));
		UStateTreeState& Generic = B.AddChild(Fight, TEXT("Generic"));
		for (UStateTreeState* State : { &Idle, &Patrol, &Light, &Heavy, &Generic })
		{
			State->SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
		}
		Fight.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;

		const UScriptStruct* PayloadStruct = FDstTestPayload::StaticStruct();
		auto RequireEvent = [PayloadStruct](UStateTreeState& State, const FGameplayTag Tag)
		{
			State.bHasRequiredEventToEnter = true;
			State.RequiredEventToEnter.Tag = Tag;
			State.RequiredEventToEnter.PayloadStruct = PayloadStruct;
		};
		RequireEvent(Fight, Tag_Event_Hit.GetTag());
		RequireEvent(Light, Tag_Event_Hit_Light.GetTag());
		RequireEvent(Heavy, Tag_Event_Hit_Heavy.GetTag());
		RequireEvent(Generic, Tag_Event_Hit.GetTag());

		B.AddBoundTask(Idle, TEXT("IdleTask"), 2);
		B.AddBoundTask(Patrol, TEXT("PatrolTask"), 3);
		B.AddBoundTask(Fight, TEXT("FightTask"), 1000);
		const FGuid LightTask = B.AddBoundTask(Light, TEXT("LightTask"), 1).ID;
		const FGuid HeavyTask = B.AddBoundTask(Heavy, TEXT("HeavyTask"), 2).ID;
		const FGuid GenericTask = B.AddBoundTask(Generic, TEXT("GenericTask"), 1).ID;
		B.Bind(Light.GetEventID(), TEXT("Payload.Amount"), LightTask, TEXT("PayloadAmount"));
		B.Bind(Heavy.GetEventID(), TEXT("Payload.Amount"), HeavyTask, TEXT("PayloadAmount"));
		B.Bind(Generic.GetEventID(), TEXT("Payload.Amount"), GenericTask, TEXT("PayloadAmount"));

		// A hit anywhere in Idle or Patrol starts a fight; the transition requires exactly the tag the state requires.
		B.AddEventTransition(Idle, Tag_Event_Hit.GetTag(), PayloadStruct, EStateTreeTransitionType::GotoState, &Fight);
		B.AddTransition(Idle, EStateTreeTransitionTrigger::OnStateSucceeded, EStateTreeTransitionType::GotoState, &Patrol);
		B.AddEventTransition(Patrol, Tag_Event_Hit.GetTag(), PayloadStruct, EStateTreeTransitionType::GotoState, &Fight);
		B.AddTransition(Patrol, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Idle);
		for (UStateTreeState* State : { &Light, &Heavy, &Generic })
		{
			B.AddTransition(*State, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Idle);
		}
	}

	void DstBuildWanderTree(FDstBuildTreeBuilder& B)
	{
		B.EnsureSpeedParameter();

		UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
		Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
		UStateTreeState& Wander = B.AddChild(Root, TEXT("Wander"));
		// The node under test: it picks one of its children at random every time it is selected.
		Wander.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom;

		for (int32 LegIndex = 0; LegIndex < 4; ++LegIndex)
		{
			UStateTreeState& Leg = B.AddChild(Wander, *FString::Printf(TEXT("Leg%d"), LegIndex));
			Leg.SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
			B.AddBoundTask(Leg, *FString::Printf(TEXT("LegTask%d"), LegIndex), 1);
			B.AddTransition(Leg, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Wander);
		}
	}

	void DstBuildLinkableChildTree(FDstBuildTreeBuilder& B)
	{
		B.EnsureSpeedParameter();
		B.AddRootParameter(TEXT("Radius"), EPropertyBagPropertyType::Float);
		B.AddRootParameter(TEXT("Count"), EPropertyBagPropertyType::Int32);
		B.GetRootBag().SetValueFloat(TEXT("Radius"), 2.f);
		B.GetRootBag().SetValueInt32(TEXT("Count"), 3);

		UStateTreeState& Root = B.AddSubTree(TEXT("ChildRoot"));
		Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
		const FGuid ChildTask = B.AddBoundTask(Root, TEXT("ChildTask"), 5).ID;
		B.Bind(B.GetRootParametersId(), TEXT("Radius"), ChildTask, TEXT("Scale"));
		B.Bind(B.GetRootParametersId(), TEXT("Count"), ChildTask, TEXT("TicksToComplete"));
	}

	FDstBuildAllFeaturesInfo DstBuildAllFeaturesTree(FDstBuildTreeBuilder& B)
	{
		using namespace UE::DreamStateTree::Tests;

		FDstBuildAllFeaturesInfo Info;
		UStateTreeEditorData& EditorData = B.GetEditorData();
		const FGuid Params = B.GetRootParametersId();
		const UScriptStruct* PayloadStruct = FDstTestPayload::StaticStruct();

		//----------------------------------------------------------------------//
		// Root parameters: float, int, bool, struct, object, enum, array, and the int an output binding writes to.
		//----------------------------------------------------------------------//
		B.AddRootParameter(TEXT("Speed"), EPropertyBagPropertyType::Float);
		B.AddRootParameter(TEXT("Level"), EPropertyBagPropertyType::Int32);
		B.AddRootParameter(TEXT("bAggressive"), EPropertyBagPropertyType::Bool);
		B.AddRootParameter(TEXT("Origin"), EPropertyBagPropertyType::Struct, TBaseStructure<FVector>::Get());
		B.AddRootParameter(TEXT("Target"), EPropertyBagPropertyType::Object, UDstTestOwner::StaticClass());
		B.AddRootParameter(TEXT("Mode"), EPropertyBagPropertyType::Enum, StaticEnum<EDstTestMode>());
		B.AddRootParameter(TEXT("Waypoints"), EPropertyBagPropertyType::Int32, nullptr, EPropertyBagContainerType::Array);
		B.AddRootParameter(TEXT("LastTickCount"), EPropertyBagPropertyType::Int32);
		{
			FInstancedPropertyBag& Bag = B.GetRootBag();
			Bag.SetValueFloat(TEXT("Speed"), 4.f);
			Bag.SetValueInt32(TEXT("Level"), 3);
			Bag.SetValueBool(TEXT("bAggressive"), true);
			Bag.SetValueStruct(TEXT("Origin"), FVector(1.0, 2.0, 3.0));
			Bag.SetValueEnum(TEXT("Mode"), EDstTestMode::Patrol);
		}
		Info.Features.Add(TEXT("Root parameters: float, int, bool, FVector, object, enum, int array"));

		//----------------------------------------------------------------------//
		// Global evaluator and global task
		//----------------------------------------------------------------------//
		FGuid CounterId;
		{
			TStateTreeEditorNode<FDstTestEvaluator>& Counter = B.AddEvaluator<FDstTestEvaluator>(TEXT("Counter"));
			Counter.GetInstanceData().Step = 1;
			CounterId = Counter.ID;
		}
		B.Bind(Params, TEXT("Level"), CounterId, TEXT("Step"));

		FGuid MonitorId;
		MonitorId = B.AddBoundGlobalTask(TEXT("GlobalMonitor")).ID;
		B.BindOutput(Params, TEXT("LastTickCount"), MonitorId, TEXT("TickCount"));
		EditorData.GlobalTasksCompletion = EStateTreeTaskCompletionType::Any;
		Info.Features.Add(TEXT("Global evaluator (Step bound to a root int), global task (Input bound to a root float, Output copied into a root int)"));

		//----------------------------------------------------------------------//
		// States. Created first, with their final IDs, so that every link made later copies the final ID.
		//----------------------------------------------------------------------//
		UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
		UStateTreeState& PatrolSubtree = B.AddSubTree(TEXT("Patrol"), EStateTreeStateType::Subtree);
		B.AddStateParameter(PatrolSubtree, TEXT("Radius"), EPropertyBagPropertyType::Float);
		B.AddStateParameter(PatrolSubtree, TEXT("Loops"), EPropertyBagPropertyType::Int32);
		PatrolSubtree.Parameters.Parameters.SetValueFloat(TEXT("Radius"), 2.f);
		PatrolSubtree.Parameters.Parameters.SetValueInt32(TEXT("Loops"), 1);

		UStateTreeState& Gate = B.AddChild(Root, TEXT("Gate"));
		UStateTreeState& Combat = B.AddChild(Root, TEXT("Combat"));
		UStateTreeState& Explore = B.AddChild(Root, TEXT("Explore"), EStateTreeStateType::Group);
		UStateTreeState& Sleeper = B.AddChild(Root, TEXT("Sleeper"));
		UStateTreeState& Patrolling = B.AddChild(Root, TEXT("Patrolling"), EStateTreeStateType::Linked);
		UStateTreeState& Done = B.AddChild(Root, TEXT("Done"));
		UStateTreeState& Veil = B.AddChild(Root, TEXT("Veil"));
		UStateTreeState& Strike = B.AddChild(Combat, TEXT("Strike"));
		UStateTreeState& Guard = B.AddChild(Combat, TEXT("Guard"));
		UStateTreeState& Wander = B.AddChild(Explore, TEXT("Wander"));
		UStateTreeState& Rest = B.AddChild(Explore, TEXT("Rest"));
		UStateTreeState& LegA = B.AddChild(Wander, TEXT("LegA"));
		UStateTreeState& LegB = B.AddChild(Wander, TEXT("LegB"));
		UStateTreeState& PatrolLeg = B.AddChild(PatrolSubtree, TEXT("Leg"));

		// Selection behaviors: all seven.
		Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
		Gate.SelectionBehavior = EStateTreeStateSelectionBehavior::TryFollowTransitions;
		Combat.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility;
		Explore.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom;
		Wander.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandomWeightedByUtility;
		Veil.SelectionBehavior = EStateTreeStateSelectionBehavior::None;
		for (UStateTreeState* State : { &Sleeper, &Done, &Strike, &Guard, &Rest, &LegA, &LegB, &PatrolLeg })
		{
			State->SelectionBehavior = EStateTreeStateSelectionBehavior::TryEnterState;
		}
		Info.Features.Add(TEXT("Selection behaviors: InOrder, FollowTransitions, HighestUtility, AtRandom, AtRandomWeightedByUtility, None, TryEnterState"));
		Info.Features.Add(TEXT("State types: State, Group (Explore), Subtree (Patrol), Linked (Patrolling -> Patrol)"));

		Root.TasksCompletion = EStateTreeTaskCompletionType::Any;
		Combat.TasksCompletion = EStateTreeTaskCompletionType::Any;
		Rest.TasksCompletion = EStateTreeTaskCompletionType::All;
		Combat.Weight = 2.f;
		Combat.Tag = Tag_State_Combat.GetTag();
		Explore.Tag = Tag_State_Explore.GetTag();
		Combat.bHasCustomTickRate = true;
		Combat.CustomTickRate = 0.25f;
		Sleeper.bEnabled = false;
		Info.Features.Add(TEXT("Tasks completion Any and All, custom tick rate, weight, state tags, disabled state"));

		// Required events: Combat takes any Hit with a payload, Strike only Hit.Light.
		auto RequireEvent = [PayloadStruct](UStateTreeState& State, const FGameplayTag Tag)
		{
			State.bHasRequiredEventToEnter = true;
			State.RequiredEventToEnter.Tag = Tag;
			State.RequiredEventToEnter.PayloadStruct = PayloadStruct;
			State.RequiredEventToEnter.bConsumeEventOnSelect = true;
		};
		RequireEvent(Combat, Tag_Event_Hit.GetTag());
		RequireEvent(Strike, Tag_Event_Hit_Light.GetTag());

		B.AddStateParameter(Combat, TEXT("LocalTicks"), EPropertyBagPropertyType::Int32);

		//----------------------------------------------------------------------//
		// Combat: the busiest state. Bindings of every kind end up on its first task.
		//----------------------------------------------------------------------//
		FGuid CombatTaskId;
		{
			TStateTreeEditorNode<FDstTestTask>& Task = B.AddBoundTask(Combat, TEXT("CombatTask"), 4);
			CombatTaskId = Task.ID;
			Task.GetNode().NodeLink = Guard.GetLinkToState();
			Task.GetInstanceData().InstanceLink = Strike.GetLinkToState();
		}
		B.Bind(Params, TEXT("Mode"), CombatTaskId, TEXT("Mode"));
		B.Bind(Params, TEXT("Origin"), CombatTaskId, TEXT("Location"));
		B.Bind(Params, TEXT("Target"), CombatTaskId, TEXT("Target"));
		B.Bind(Params, TEXT("Waypoints"), CombatTaskId, TEXT("Route"));
		B.Bind(Params, TEXT("bAggressive"), CombatTaskId, TEXT("bFlag"));
		B.Bind(Combat.GetEventID(), TEXT("Payload.Amount"), CombatTaskId, TEXT("PayloadAmount"));
		B.Bind(Combat.GetEventID(), TEXT("Payload.Severity"), CombatTaskId, TEXT("PayloadSeverity"));
		B.Bind(CounterId, TEXT("Counter"), CombatTaskId, TEXT("CounterRef"));
		B.BindOutput(Combat.Parameters.ID, TEXT("LocalTicks"), CombatTaskId, TEXT("TickCount"));
		Info.Features.Add(TEXT("Bindings: root parameter -> Input / Parameter (float, bool, FVector, object, enum, array), event payload -> task, evaluator output -> property ref, task output -> state parameter, root parameter <- global task output"));
		Info.Features.Add(TEXT("Property ref bound to an evaluator output, state links in the node and in the instance data"));

		// Nested property functions: Scale = AddFloat(MultiplyFloat(Speed, 0.5), 1).
		{
			const FGuid AddId = B.AddFunctionBinding(TEXT("StateTreeAddFloatPropertyFunction"), FDstBuildTreeBuilder::MakePath(CombatTaskId, TEXT("Scale")),
				TEXT("Root/Combat/Task:0/Scale/Func:0"));
			if (AddId.IsValid())
			{
				B.SetFunctionNumber(AddId, TEXT("Right"), 1.0);
				const FGuid MultiplyId = B.AddFunctionBinding(TEXT("StateTreeMultiplyFloatPropertyFunction"), FDstBuildTreeBuilder::MakePath(AddId, TEXT("Left")),
					TEXT("Root/Combat/Task:0/Scale/Func:1"));
				if (MultiplyId.IsValid())
				{
					B.SetFunctionNumber(MultiplyId, TEXT("Right"), 0.5);
					B.Bind(Params, TEXT("Speed"), MultiplyId, TEXT("Left"));
				}
			}
			Info.Features.Add(TEXT("Nested property functions AddFloat(MultiplyFloat(Speed, 0.5), 1) bound to a task parameter"));
		}

		// Blueprint-style task: the instance data is a UObject.
		{
			FStateTreeEditorNode& Node = B.AddBlueprintTask(Combat, TEXT("BlueprintTask"), UDstTestBlueprintTask::StaticClass());
			if (UDstTestBlueprintTask* Instance = Cast<UDstTestBlueprintTask>(Node.InstanceObject))
			{
				Instance->Strength = 7;
			}
			Info.Features.Add(TEXT("Blueprint task node (FStateTreeBlueprintTaskWrapper with an instance object)"));
		}

		// A task that sends an event to the tree itself.
		{
			TStateTreeEditorNode<FDstTestSendEventTask>& Sender = B.AddTask<FDstTestSendEventTask>(Combat, TEXT("SendHit"));
			Sender.GetInstanceData().Tag = Tag_Event_Hit_Light.GetTag();
			Sender.GetInstanceData().Payload.Amount = 5;
			Sender.GetInstanceData().Payload.Severity = 0.9f;
		}

		//----------------------------------------------------------------------//
		// Strike: enter conditions with every operand and indent, considerations that multiply.
		//----------------------------------------------------------------------//
		{
			TStateTreeEditorNode<FDstTestTask>& Task = B.AddBoundTask(Strike, TEXT("StrikeTask"), 2);
			const FGuid StrikeTaskId = Task.ID;
			// A child binds the payload of the parent state's required event.
			B.Bind(Combat.GetEventID(), TEXT("Payload.Amount"), StrikeTaskId, TEXT("PayloadAmount"));
		}

		auto AddEnterCondition = [&B](UStateTreeState& State, const TCHAR* Name, const EStateTreeExpressionOperand Operand, const int32 Indent,
			const bool bInvert, const EStateTreeConditionEvaluationMode EvaluationMode, const FGuid& SourceId, const TCHAR* SourcePath, const float Right)
		{
			TStateTreeEditorNode<FDstTestCondition>& Condition = B.AddEnterCondition<FDstTestCondition>(State, Name);
			Condition.ExpressionOperand = Operand;
			Condition.ExpressionIndent = static_cast<uint8>(Indent);
			Condition.GetNode().bInvert = bInvert;
			Condition.GetNode().EvaluationMode = EvaluationMode;
			Condition.GetInstanceData().Right = Right;
			const FGuid ConditionId = Condition.ID;
			B.Bind(SourceId, SourcePath, ConditionId, TEXT("Left"));
		};
		const EStateTreeConditionEvaluationMode Evaluated = EStateTreeConditionEvaluationMode::Evaluated;
		AddEnterCondition(Strike, TEXT("C0"), EStateTreeExpressionOperand::And, 0, false, Evaluated, CounterId, TEXT("Counter"), 0.f);
		AddEnterCondition(Strike, TEXT("C1"), EStateTreeExpressionOperand::And, 0, false, Evaluated, Strike.GetEventID(), TEXT("Payload.Severity"), 0.1f);
		AddEnterCondition(Strike, TEXT("C2"), EStateTreeExpressionOperand::Or, 1, true, Evaluated, CounterId, TEXT("Counter"), 5.f);
		AddEnterCondition(Strike, TEXT("C3"), EStateTreeExpressionOperand::And, 2, false, EStateTreeConditionEvaluationMode::ForcedTrue, CounterId, TEXT("Counter"), 1.f);
		AddEnterCondition(Strike, TEXT("C4"), EStateTreeExpressionOperand::Or, 3, false, Evaluated, CounterId, TEXT("Counter"), 9.f);
		AddEnterCondition(Guard, TEXT("GuardCondition"), EStateTreeExpressionOperand::And, 0, true, Evaluated, CounterId, TEXT("Counter"), 100.f);
		Info.Features.Add(TEXT("Enter conditions: And, Or, indent 0-3, inverted, EvaluationMode ForcedTrue, bound to an evaluator output and to an event payload"));

		{
			TStateTreeEditorNode<FDstTestConsideration>& First = B.AddConsideration<FDstTestConsideration>(Strike, TEXT("StrikeScore"));
			First.GetInstanceData().Score = 0.8f;
		}
		{
			TStateTreeEditorNode<FDstTestConsideration>& Second = B.AddConsideration<FDstTestConsideration>(Strike, TEXT("StrikeBias"));
			Second.ExpressionOperand = EStateTreeExpressionOperand::Multiply;
			Second.GetInstanceData().Score = 0.5f;
		}
		{
			TStateTreeEditorNode<FDstTestConsideration>& Only = B.AddConsideration<FDstTestConsideration>(Guard, TEXT("GuardScore"));
			Only.GetInstanceData().Score = 0.6f;
		}
		Info.Features.Add(TEXT("Utility considerations combined with Multiply"));

		B.AddBoundTask(Guard, TEXT("GuardTask"), 3);

		//----------------------------------------------------------------------//
		// Explore / Wander / Rest
		//----------------------------------------------------------------------//
		{
			TStateTreeEditorNode<FDstTestConsideration>& A = B.AddConsideration<FDstTestConsideration>(LegA, TEXT("LegAScore"));
			A.GetInstanceData().Score = 0.7f;
		}
		{
			TStateTreeEditorNode<FDstTestConsideration>& BScore = B.AddConsideration<FDstTestConsideration>(LegB, TEXT("LegBScore"));
			BScore.GetInstanceData().Score = 0.3f;
		}
		B.AddBoundTask(LegA, TEXT("LegATask"), 1);
		B.AddBoundTask(LegB, TEXT("LegBTask"), 1);

		const FGuid RestTaskId = B.AddBoundTask(Rest, TEXT("RestTask"), 2).ID;
		FGuid RestTask2NodeId;
		{
			TStateTreeEditorNode<FDstTestTask>& Task = B.AddBoundTask(Rest, TEXT("RestTask2"), 3, EStateTreeRunStatus::Failed);
			RestTask2NodeId = Task.GetNodeID();
		}

		{
			TStateTreeEditorNode<FDstTestTask>& Task = B.AddBoundTask(Sleeper, TEXT("SleepTask"), 2);
			Task.GetNode().bTaskEnabled = false;
		}
		Info.Features.Add(TEXT("Disabled task inside a disabled state"));

		//----------------------------------------------------------------------//
		// Patrol subtree and the linked state that runs it with an overridden parameter
		//----------------------------------------------------------------------//
		{
			const FGuid PatrolTaskId = B.AddBoundTask(PatrolSubtree, TEXT("PatrolTask"), 2).ID;
			B.Bind(PatrolSubtree.Parameters.ID, TEXT("Radius"), PatrolTaskId, TEXT("Scale"));
			B.Bind(PatrolSubtree.Parameters.ID, TEXT("Loops"), PatrolTaskId, TEXT("TicksToComplete"));
			B.AddBoundTask(PatrolLeg, TEXT("PatrolLegTask"), 1);
		}
		Patrolling.SetLinkedState(PatrolSubtree.GetLinkToState());
		{
			const FGuid RadiusId = FDstBuildTreeBuilder::FindBagPropertyId(Patrolling.Parameters.Parameters, TEXT("Radius"));
			Patrolling.SetParametersPropertyOverridden(RadiusId, true);
			Patrolling.Parameters.Parameters.SetValueFloat(TEXT("Radius"), 5.f);
		}
		Patrolling.bCopyParameterBindingsOnTick = true;
		B.AddBoundTask(Done, TEXT("DoneTask"), 1);
		B.AddBoundTask(Veil, TEXT("VeilTask"), 1);
		Info.Features.Add(TEXT("Linked state to a local subtree with an overridden parameter"));

		//----------------------------------------------------------------------//
		// Transitions. Every trigger, every target type, priorities, delay, conditions, disabled, reactivation, delegates.
		//----------------------------------------------------------------------//
		B.AddTransition(Root, EStateTreeTransitionTrigger::OnStateFailed, EStateTreeTransitionType::Failed);
		B.AddTransition(Root, EStateTreeTransitionTrigger::OnStateSucceeded, EStateTreeTransitionType::Succeeded);

		// Gate: follows transitions while a state is being selected. The first one has a condition and a high priority.
		{
			FStateTreeTransition& ToExplore = B.AddTransition(Gate, EStateTreeTransitionTrigger::OnTick, EStateTreeTransitionType::GotoState, &Explore);
			ToExplore.Priority = EStateTreeTransitionPriority::High;
			TStateTreeEditorNode<FDstTestCondition>& Condition = B.AddTransitionCondition<FDstTestCondition>(Gate, ToExplore, TEXT("GateCondition"));
			Condition.GetInstanceData().Right = 2.f;
			const FGuid ConditionId = Condition.ID;
			B.Bind(CounterId, TEXT("Counter"), ConditionId, TEXT("Left"));
		}
		{
			FStateTreeTransition& ToNext = B.AddTransition(Gate, EStateTreeTransitionTrigger::OnTick, EStateTreeTransitionType::NextState);
			ToNext.Priority = EStateTreeTransitionPriority::Low;
		}

		// Combat: event with payload, delay with random variance, failure, a disabled transition, an event without payload.
		{
			FStateTreeTransition& OnHit = B.AddEventTransition(Combat, Tag_Event_Hit.GetTag(), PayloadStruct, EStateTreeTransitionType::GotoState, &Combat);
			OnHit.Priority = EStateTreeTransitionPriority::Critical;
			OnHit.ReactivateTargetState = EStateTreeTransitionChangeTypeRules::ForceChanged;
			const FGuid EventId = OnHit.GetEventID();
			TStateTreeEditorNode<FDstTestCondition>& Condition = B.AddTransitionCondition<FDstTestCondition>(Combat, OnHit, TEXT("SeverityCondition"));
			Condition.GetInstanceData().Right = 0.5f;
			const FGuid ConditionId = Condition.ID;
			// The transition condition reads the payload of the event that triggers the transition.
			B.Bind(EventId, TEXT("Payload.Severity"), ConditionId, TEXT("Left"));
		}
		{
			FStateTreeTransition& Delayed = B.AddTransition(Combat, EStateTreeTransitionTrigger::OnTick, EStateTreeTransitionType::Parent);
			Delayed.Priority = EStateTreeTransitionPriority::Medium;
			Delayed.bDelayTransition = true;
			Delayed.DelayDuration = 1.f;
			Delayed.DelayRandomVariance = 0.25f;
			Delayed.ReactivateTargetState = EStateTreeTransitionChangeTypeRules::ForceSustained;
		}
		B.AddTransition(Combat, EStateTreeTransitionTrigger::OnStateFailed, EStateTreeTransitionType::Failed);
		{
			FStateTreeTransition& Disabled = B.AddTransition(Combat, EStateTreeTransitionTrigger::OnTick, EStateTreeTransitionType::GotoState, &Explore);
			Disabled.bTransitionEnabled = false;
		}
		B.AddEventTransition(Combat, Tag_Event_Ask.GetTag(), nullptr, EStateTreeTransitionType::NextSelectableState);
		Info.Features.Add(TEXT("Transitions: completed, succeeded, failed, tick, event with payload, delegate; priority Low/Medium/High/Critical; delay with variance; disabled transition; ReactivateTargetState ForceChanged and ForceSustained"));

		B.AddTransition(Strike, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::NextSelectableState);
		B.AddTransition(Guard, EStateTreeTransitionTrigger::OnStateSucceeded, EStateTreeTransitionType::Parent);
		B.AddTransition(Guard, EStateTreeTransitionTrigger::OnStateFailed, EStateTreeTransitionType::NextParent);
		B.AddTransition(Explore, EStateTreeTransitionTrigger::OnTick, EStateTreeTransitionType::NextSelectableState);
		B.AddTransition(LegA, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::NextSelectableState);
		B.AddTransition(LegB, EStateTreeTransitionTrigger::OnStateSucceeded, EStateTreeTransitionType::Parent);
		B.AddTransition(LegB, EStateTreeTransitionTrigger::OnStateFailed, EStateTreeTransitionType::NextSelectableParent);
		B.AddTransition(Patrolling, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::NextSelectableState);
		B.AddTransition(Done, EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::Succeeded);

		// Rest: two delegate transitions. One listens to the dispatcher in the first task's instance data, the other
		// to the completion of the second task (a task completion binding, which makes the compiler generate a dispatcher).
		{
			FStateTreeTransition& OnDispatcher = B.AddTransition(Rest, EStateTreeTransitionTrigger::OnDelegate, EStateTreeTransitionType::GotoState, &Explore);
			const FGuid TransitionId = OnDispatcher.ID;
			B.Bind(RestTaskId, TEXT("OnFinished"), TransitionId, TEXT("DelegateListener"));
		}
		{
			FStateTreeTransition& OnCompletion = B.AddTransition(Rest, EStateTreeTransitionTrigger::OnDelegate, EStateTreeTransitionType::Failed);
			EditorData.EditorBindings.AddTaskCompletionBinding(RestTask2NodeId, FPropertyBindingPath(OnCompletion.ID, TEXT("DelegateListener")),
				UE::StateTree::ETaskCompletionCondition::Completes);
		}
		Info.Features.Add(TEXT("Delegates: dispatcher in a task -> OnDelegate transition, task completion binding -> OnDelegate transition"));

		return Info;
	}
}
