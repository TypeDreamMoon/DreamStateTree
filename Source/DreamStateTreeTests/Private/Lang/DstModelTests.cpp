// Copyright DreamMoon. All Rights Reserved.
//
// The model (Model/DstModel.h) without any engine object behind it: a tree built by hand, with property functions inside
// property functions and every kind of argument, must dump the same text every time, differ in its dump whenever any
// field differs, and answer path and node lookups. Property paths and source references must survive their own text form.

#include "Misc/AutomationTest.h"
#include "Model/DstModel.h"

namespace UE::DreamStateTree::Tests::ModelTests
{
	using namespace UE::DreamStateTree::Model;

	namespace
	{
		// ------------------------------------------------------------------------------------------
		// Builders
		// ------------------------------------------------------------------------------------------

		FDstPathSegment Seg(const TCHAR* Name, const int32 Index = INDEX_NONE, const TCHAR* InstanceType = TEXT(""))
		{
			FDstPathSegment Segment;
			Segment.Name = Name;
			Segment.ArrayIndex = Index;
			Segment.InstanceType = InstanceType;
			return Segment;
		}

		FDstPropertyPath PathOf(std::initializer_list<FDstPathSegment> Segments)
		{
			FDstPropertyPath Path;
			Path.Segments = TArray<FDstPathSegment>(Segments);
			return Path;
		}

		FDstPropertyPath NamesPath(std::initializer_list<const TCHAR*> Names)
		{
			FDstPropertyPath Path;
			for (const TCHAR* Name : Names)
			{
				Path.Segments.Add(Seg(Name));
			}
			return Path;
		}

		FDstSourceRef MakeRef(const EDstSourceKind Kind, const TCHAR* Owner, std::initializer_list<const TCHAR*> Names)
		{
			FDstSourceRef Ref;
			Ref.Kind = Kind;
			Ref.Owner = Owner;
			Ref.Path = NamesPath(Names);
			return Ref;
		}

		FDstArg ConstantArg(const EDstPropertySide Side, const TCHAR* Property, const TCHAR* Text)
		{
			FDstArg Arg;
			Arg.Side = Side;
			Arg.Property = NamesPath({ Property });
			Arg.Kind = EDstValueKind::Constant;
			Arg.ConstantText = Text;
			return Arg;
		}

		FDstArg SourceArg(const EDstValueKind Kind, const TCHAR* Property, const FDstSourceRef& Source)
		{
			FDstArg Arg;
			Arg.Property = NamesPath({ Property });
			Arg.Kind = Kind;
			Arg.Source = Source;
			return Arg;
		}

		FDstArg FunctionArg(const TCHAR* Property, const TSharedPtr<FDstNode>& Function)
		{
			FDstArg Arg;
			Arg.Property = NamesPath({ Property });
			Arg.Kind = EDstValueKind::Function;
			Arg.Function = Function;
			return Arg;
		}

		FDstNode MakeNode(const TCHAR* Key, const EDstNodeRole Role, const TCHAR* StructType)
		{
			FDstNode Node;
			Node.Key = Key;
			Node.Role = Role;
			Node.StructType = StructType;
			return Node;
		}

		TSharedPtr<FDstNode> MakeFunction(const TCHAR* Key, const TCHAR* StructType)
		{
			return MakeShared<FDstNode>(MakeNode(Key, EDstNodeRole::PropertyFunction, StructType));
		}

		FDstParameter MakeParameter(const TCHAR* Name, const TCHAR* Type, const TCHAR* Value)
		{
			FDstParameter Parameter;
			Parameter.Name = Name;
			Parameter.Type = Type;
			Parameter.ValueText = Value;
			return Parameter;
		}

		/**
		 * A tree that uses every part of the model:
		 *
		 *   States[0] Root                 transitions: completed, event (with a condition), delegate
		 *     Children[0] Attack           event, enter conditions, considerations, two tasks, a child
		 *       Children[0] Combo
		 *     Children[1] Hurt             Linked, an overridden parameter
		 *     Children[2] Rest             LinkedAsset
		 *   States[1] HitReaction          Subtree, a parameter with metadata
		 *     Children[0] Stagger          a task
		 *
		 * Task 0 of Attack has a constant on each side, a binding, a function whose argument is a function, a ref and an
		 * output. The maps are filled in forward or reverse order: the dump must not depend on it.
		 */
		FDstTree MakeSampleTree(const bool bReverseMaps = false)
		{
			FDstTree Tree;
			Tree.AssetPath = TEXT("/Game/AI/ST_Sample.ST_Sample");
			Tree.SchemaClass = TEXT("/Script/GameplayStateTreeModule.StateTreeAIComponentSchema");

			struct FEntry
			{
				const TCHAR* Key;
				const TCHAR* Value;
			};
			const FEntry SchemaProperties[] = {
				{ TEXT("ContextActorClass"), TEXT("/Script/Game.CombatEnemy") },
				{ TEXT("AIControllerClass"), TEXT("/Script/Game.AIC_Brawler") },
				{ TEXT("ContextExtra"), TEXT("/Script/Game.Extra") } };
			const FEntry Settings[] = {
				{ TEXT("GlobalTasksCompletion"), TEXT("Any") },
				{ TEXT("Zeta"), TEXT("1") },
				{ TEXT("Alpha"), TEXT("2") } };
			const FEntry ParameterMeta[] = {
				{ TEXT("Category"), TEXT("性格") },
				{ TEXT("Tooltip"), TEXT("How brave") },
				{ TEXT("Delta"), TEXT("x") } };

			// Adds the entries of a table to a map, last to first when asked to.
			const auto Fill = [bReverseMaps](TMap<FString, FString>& Map, const FEntry* Entries, const int32 Count)
			{
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const FEntry& Entry = Entries[bReverseMaps ? Count - 1 - Index : Index];
					Map.Add(Entry.Key, Entry.Value);
				}
			};
			Fill(Tree.SchemaProperties, SchemaProperties, UE_ARRAY_COUNT(SchemaProperties));
			Fill(Tree.Settings, Settings, UE_ARRAY_COUNT(Settings));

			FDstParameter Courage = MakeParameter(TEXT("Courage"), TEXT("float"), TEXT("0.5"));
			Fill(Courage.Meta, ParameterMeta, UE_ARRAY_COUNT(ParameterMeta));
			Tree.Parameters.Add(Courage);
			Tree.Parameters.Add(MakeParameter(TEXT("Home"), TEXT("Struct:/Script/CoreUObject.Vector"), TEXT("")));
			Tree.Parameters.Add(MakeParameter(TEXT("Targets"), TEXT("Array<Object:/Script/Engine.Actor>"), TEXT("")));

			FDstNode Info = MakeNode(TEXT("Global[0]"), EDstNodeRole::GlobalTask, TEXT("/Script/StateTreeModule.StateTreeDelayTask"));
			Info.BindName = TEXT("Info");
			Info.Args.Add(ConstantArg(EDstPropertySide::Instance, TEXT("Duration"), TEXT("2.0")));
			Tree.Globals.Add(Info);

			Tree.Colors.Add({ TEXT("Combat"), TEXT("(R=0.8,G=0.25,B=0.2,A=1.0)") });

			// Root
			FDstState Root;
			Root.Name = TEXT("Root");
			Root.Selection = TEXT("TrySelectChildrenAtRandom");

			FDstTransition Completed;
			Completed.TargetState = TEXT("Root");
			Root.Transitions.Add(Completed);

			FDstTransition OnHit;
			OnHit.Trigger = EDstTransitionTrigger::OnEvent;
			OnHit.Event.Tag = TEXT("Game.Event.Hit");
			OnHit.Event.PayloadType = TEXT("/Script/Game.HitPayload");
			OnHit.Event.bConsumeOnSelect = false;
			OnHit.TargetState = TEXT("Root/Hurt");
			OnHit.Priority = TEXT("High");
			OnHit.bDelay = true;
			OnHit.DelayText = TEXT("0.5");
			OnHit.VarianceText = TEXT("0.1");
			OnHit.bReactivate = true;
			OnHit.bEnabled = false;
			FDstNode HitCondition = MakeNode(TEXT("Root/Transition[1]/Condition[0]"), EDstNodeRole::TransitionCondition, TEXT("/Script/StateTreeModule.StateTreeCompareFloatCondition"));
			HitCondition.Args.Add(SourceArg(EDstValueKind::Binding, TEXT("Left"), MakeRef(EDstSourceKind::Parameters, TEXT(""), { TEXT("Courage") })));
			HitCondition.Args.Add(ConstantArg(EDstPropertySide::Instance, TEXT("Right"), TEXT("0.5")));
			OnHit.Conditions.Add(HitCondition);
			Root.Transitions.Add(OnHit);

			FDstTransition OnCombo;
			OnCombo.Trigger = EDstTransitionTrigger::OnDelegate;
			OnCombo.TargetKind = EDstTransitionTarget::NextState;
			OnCombo.Delegate = MakeRef(EDstSourceKind::Node, TEXT("Root/Attack/Task[0]"), {});
			OnCombo.Completion = EDstCompletion::Succeeds;
			Root.Transitions.Add(OnCombo);

			// Attack
			FDstState Attack;
			Attack.Name = TEXT("Attack");
			Attack.Selection = TEXT("TrySelectChildrenWithHighestUtility");
			Attack.TasksCompletion = TEXT("Any");
			Attack.Tag = TEXT("Combat.Attack");
			Attack.ColorName = TEXT("Combat");
			Attack.Description = TEXT("击打");
			Attack.bEnabled = false;
			Attack.WeightText = TEXT("2");
			Attack.bCustomTickRate = true;
			Attack.TickRateText = TEXT("0.2");
			Attack.bCopyParameterBindingsOnTick = true;
			Attack.bCheckPrerequisitesWhenActivatingChildDirectly = false;
			Attack.bFixedLayout = true;
			FDstEvent Trade;
			Trade.Tag = TEXT("Game.Event.Villager.Trade");
			Trade.PayloadType = TEXT("/Script/Game.TradeOffer");
			Trade.bConsumeOnSelect = true;
			Attack.Event = Trade;

			FDstNode Sees = MakeNode(TEXT("Root/Attack/EnterCondition[0]"), EDstNodeRole::EnterCondition, TEXT("/Script/Game.SeesPlayerCondition"));
			Sees.Args.Add(ConstantArg(EDstPropertySide::Node, TEXT("bInvert"), TEXT("True")));
			Attack.EnterConditions.Add(Sees);
			FDstNode Near = MakeNode(TEXT("Root/Attack/EnterCondition[1]"), EDstNodeRole::EnterCondition, TEXT("/Script/Game.NearCondition"));
			Near.Operand = EDstOperand::And;
			Near.Indent = 1;
			Attack.EnterConditions.Add(Near);

			Attack.Considerations.Add(MakeNode(TEXT("Root/Attack/Consideration[0]"), EDstNodeRole::Consideration, TEXT("/Script/StateTreeModule.StateTreeConstantConsideration")));
			FDstNode Scaled = MakeNode(TEXT("Root/Attack/Consideration[1]"), EDstNodeRole::Consideration, TEXT("/Script/StateTreeModule.StateTreeFloatInputConsideration"));
			Scaled.Operand = EDstOperand::Multiply;
			Attack.Considerations.Add(Scaled);

			// Task 0: a constant on each side, a binding with a longer path, a function inside a function, a ref and an output.
			FDstNode Move = MakeNode(TEXT("Root/Attack/Task[0]"), EDstNodeRole::Task, TEXT("/Script/StateTreeModule.StateTreeDelayTask"));
			Move.BindName = TEXT("Combo");
			Move.Args.Add(ConstantArg(EDstPropertySide::Instance, TEXT("Duration"), TEXT("2.0")));
			Move.Args.Add(ConstantArg(EDstPropertySide::Node, TEXT("bTaskEnabled"), TEXT("False")));
			Move.Args.Add(SourceArg(EDstValueKind::Binding, TEXT("TargetActor"), MakeRef(EDstSourceKind::Global, TEXT("Global[0]"), { TEXT("TargetPlayerCharacter") })));

			const TSharedPtr<FDstNode> Multiply = MakeFunction(TEXT("Root/Attack/Task[0]/Fn:AcceptableRadius"), TEXT("/Script/StateTreeModule.StateTreeMultiplyFloatPropertyFunction"));
			Multiply->Args.Add(SourceArg(EDstValueKind::Binding, TEXT("Left"), MakeRef(EDstSourceKind::Parameters, TEXT(""), { TEXT("AttackRange") })));
			const TSharedPtr<FDstNode> Add = MakeFunction(TEXT("Root/Attack/Task[0]/Fn:AcceptableRadius/Fn:Right"), TEXT("/Script/StateTreeModule.StateTreeAddFloatPropertyFunction"));
			Add->Args.Add(ConstantArg(EDstPropertySide::Instance, TEXT("Left"), TEXT("1.0")));
			Add->Args.Add(SourceArg(EDstValueKind::Binding, TEXT("Right"), MakeRef(EDstSourceKind::Parameters, TEXT(""), { TEXT("Courage") })));
			Multiply->Args.Add(FunctionArg(TEXT("Right"), Add));
			Move.Args.Add(FunctionArg(TEXT("AcceptableRadius"), Multiply));

			Move.Args.Add(SourceArg(EDstValueKind::Ref, TEXT("Result"), MakeRef(EDstSourceKind::Parameters, TEXT(""), { TEXT("Home") })));
			Move.Args.Add(SourceArg(EDstValueKind::Output, TEXT("Distance"), MakeRef(EDstSourceKind::Parameters, TEXT(""), { TEXT("Dist") })));

			FDstArg Nested;
			Nested.Property = NamesPath({ TEXT("Interval"), TEXT("Min") });
			Nested.Kind = EDstValueKind::Binding;
			Nested.Source.Kind = EDstSourceKind::StateEvent;
			Nested.Source.Owner = TEXT("Root/Attack");
			Nested.Source.Path = PathOf({ Seg(TEXT("Payload"), INDEX_NONE, TEXT("/Script/Game.TradeOffer")), Seg(TEXT("Offers"), 2), Seg(TEXT("Amount")) });
			Move.Args.Add(Nested);
			Attack.Tasks.Add(Move);
			Attack.Tasks.Add(MakeNode(TEXT("Root/Attack/Task[1]"), EDstNodeRole::Task, TEXT("/Script/StateTreeModule.StateTreeDebugTextTask")));

			FDstState Combo;
			Combo.Name = TEXT("Combo");
			Attack.Children.Add(Combo);
			Root.Children.Add(Attack);

			FDstState Hurt;
			Hurt.Name = TEXT("Hurt");
			Hurt.Type = EDstStateType::Linked;
			Hurt.LinkTarget = TEXT("HitReaction");
			FDstParameter Toughness = MakeParameter(TEXT("Toughness"), TEXT("float"), TEXT("1.5"));
			Toughness.bOverridden = true;
			Hurt.Parameters.Add(Toughness);
			Root.Children.Add(Hurt);

			FDstState Rest;
			Rest.Name = TEXT("Rest");
			Rest.Type = EDstStateType::LinkedAsset;
			Rest.LinkTarget = TEXT("/Game/AI/ST_Other.ST_Other");
			Root.Children.Add(Rest);

			FDstState HitReaction;
			HitReaction.Name = TEXT("HitReaction");
			HitReaction.Type = EDstStateType::Subtree;
			FDstParameter SubtreeToughness = MakeParameter(TEXT("Toughness"), TEXT("float"), TEXT("1"));
			SubtreeToughness.Meta.Add(TEXT("Category"), TEXT("Reaction"));
			HitReaction.Parameters.Add(SubtreeToughness);
			FDstState Stagger;
			Stagger.Name = TEXT("Stagger");
			Stagger.Tasks.Add(MakeNode(TEXT("HitReaction/Stagger/Task[0]"), EDstNodeRole::Task, TEXT("/Script/StateTreeModule.StateTreeDelayTask")));
			HitReaction.Children.Add(Stagger);

			Tree.States.Add(Root);
			Tree.States.Add(HitReaction);
			return Tree;
		}

		FString DumpOf(FDstTree Tree)
		{
			Tree.UpdatePaths();
			return DumpDstModel(Tree);
		}

		FDstState& RootOf(FDstTree& Tree) { return Tree.States[0]; }
		FDstState& AttackOf(FDstTree& Tree) { return Tree.States[0].Children[0]; }
		FDstState& HurtOf(FDstTree& Tree) { return Tree.States[0].Children[1]; }
		FDstState& SubtreeOf(FDstTree& Tree) { return Tree.States[1]; }
		FDstNode& MoveOf(FDstTree& Tree) { return Tree.States[0].Children[0].Tasks[0]; }

		struct FMutation
		{
			const TCHAR* Name;
			TFunction<void(FDstTree&)> Apply;
		};

		/** Calls Name(int32) for every value 0..Last and checks each is non-empty and distinct. */
		void CheckEnumNames(FAutomationTestBase& Test, const TCHAR* Enum, const int32 Last, const TFunctionRef<FString(int32)> NameOf)
		{
			TSet<FString> Seen;
			for (int32 Value = 0; Value <= Last; ++Value)
			{
				const FString Name = NameOf(Value);
				Test.TestFalse(FString::Printf(TEXT("%s value %d has a name"), Enum, Value), Name.IsEmpty());
				Test.TestFalse(FString::Printf(TEXT("%s name `%s` is unique"), Enum, *Name), Seen.Contains(Name));
				Seen.Add(Name);
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Property paths and source references
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelPropertyPathTest, "DreamStateTree.Lang.Model.PropertyPath",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelPropertyPathTest::RunTest(const FString& Parameters)
	{
		struct FCase
		{
			FDstPropertyPath Path;
			/** The documented spelling, or null when only the round trip is checked. */
			const TCHAR* Text;
		};
		const FCase Cases[] =
		{
			{ PathOf({}), TEXT("") },
			{ PathOf({ Seg(TEXT("Speed")) }), TEXT("Speed") },
			{ PathOf({ Seg(TEXT("Payload")), Seg(TEXT("Amount")) }), TEXT("Payload.Amount") },
			{ PathOf({ Seg(TEXT("Interval")), Seg(TEXT("Min")) }), TEXT("Interval.Min") },
			{ PathOf({ Seg(TEXT("Items"), 2) }), TEXT("Items[2]") },
			{ PathOf({ Seg(TEXT("Items"), 2), Seg(TEXT("X")) }), TEXT("Items[2].X") },
			{ PathOf({ Seg(TEXT("Items"), 0) }), TEXT("Items[0]") },
			{ PathOf({ Seg(TEXT("Payload"), INDEX_NONE, TEXT("FDstTestPayload")), Seg(TEXT("Amount")) }), TEXT("Payload<FDstTestPayload>.Amount") },
			{ PathOf({ Seg(TEXT("A"), 0), Seg(TEXT("B"), 1), Seg(TEXT("C")) }), TEXT("A[0].B[1].C") },
			// Only the round trip: a type path has dots and slashes of its own, and an element can hold an instanced type.
			{ PathOf({ Seg(TEXT("Payload"), INDEX_NONE, TEXT("/Script/DreamStateTreeTests.DstTestPayload")), Seg(TEXT("Amount")) }), nullptr },
			{ PathOf({ Seg(TEXT("Items"), 3, TEXT("FDstTestPayload")), Seg(TEXT("X")) }), nullptr },
			{ PathOf({ Seg(TEXT("bTaskEnabled")) }), TEXT("bTaskEnabled") },
			{ PathOf({ Seg(TEXT("_Under_score9")), Seg(TEXT("Next_1"), 10) }), nullptr },
		};

		for (const FCase& Case : Cases)
		{
			const FString Text = Case.Path.ToString();
			if (Case.Text)
			{
				TestEqualSensitive(FString::Printf(TEXT("the text of a path (expected `%s`)"), Case.Text), Text, FString(Case.Text));
			}
			const FDstPropertyPath Back = FDstPropertyPath::FromString(Text);
			TestTrue(FString::Printf(TEXT("`%s` reads back as the path it came from"), *Text), Back == Case.Path);
			TestEqualSensitive(FString::Printf(TEXT("`%s` prints the same after reading"), *Text), Back.ToString(), Text);
		}

		TestTrue(TEXT("a path with no segments is empty"), PathOf({}).IsEmpty());
		TestFalse(TEXT("a path with a segment is not"), PathOf({ Seg(TEXT("A")) }).IsEmpty());
		TestTrue(TEXT("the empty text is the empty path"), FDstPropertyPath::FromString(FString()).IsEmpty());

		// Element 0 is an element; no element is none.
		const FDstPropertyPath First = PathOf({ Seg(TEXT("Items"), 0) });
		const FDstPropertyPath Whole = PathOf({ Seg(TEXT("Items")) });
		TestFalse(TEXT("Items[0] is not Items"), First == Whole);
		TestNotEqual(TEXT("and they are spelled differently"), First.ToString(), Whole.ToString());
		TestFalse(TEXT("an instance type is part of the identity"),
			PathOf({ Seg(TEXT("P"), INDEX_NONE, TEXT("A")) }) == PathOf({ Seg(TEXT("P"), INDEX_NONE, TEXT("B")) }));
		TestTrue(TEXT("equal paths are equal"), PathOf({ Seg(TEXT("A")), Seg(TEXT("B"), 1) }) == PathOf({ Seg(TEXT("A")), Seg(TEXT("B"), 1) }));

		// Something ToString does not write still reads: a plain dotted chain.
		const FDstPropertyPath Dotted = FDstPropertyPath::FromString(TEXT("A.B.C"));
		TestEqual(TEXT("A.B.C has three segments"), Dotted.Segments.Num(), 3);
		if (Dotted.Segments.Num() == 3)
		{
			TestEqualSensitive(TEXT("the second segment"), Dotted.Segments[1].Name, FString(TEXT("B")));
			TestEqual(TEXT("which is no element"), Dotted.Segments[1].ArrayIndex, static_cast<int32>(INDEX_NONE));
			TestEqualSensitive(TEXT("and has no instance type"), Dotted.Segments[1].InstanceType, FString());
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelSourceRefTest, "DreamStateTree.Lang.Model.SourceRef",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelSourceRefTest::RunTest(const FString& Parameters)
	{
		FDstSourceRef None;
		TestFalse(TEXT("a default reference is not set"), None.IsSet());
		TestTrue(TEXT("a default reference has no kind"), None.Kind == EDstSourceKind::None);

		// The examples in the header of DstModel.h.
		const FDstSourceRef Speed = MakeRef(EDstSourceKind::Parameters, TEXT(""), { TEXT("Speed") });
		TestTrue(TEXT("a parameters reference is set"), Speed.IsSet());
		TestEqualSensitive(TEXT("Parameters:Speed"), Speed.ToString(), FString(TEXT("Parameters:Speed")));

		const FDstSourceRef Result = MakeRef(EDstSourceKind::Node, TEXT("Root/Attack/Task[0]"), { TEXT("Result") });
		TestEqualSensitive(TEXT("Node:Root/Attack/Task[0]:Result"), Result.ToString(), FString(TEXT("Node:Root/Attack/Task[0]:Result")));

		FDstSourceRef Payload;
		Payload.Kind = EDstSourceKind::StateEvent;
		Payload.Owner = TEXT("Root/Hurt");
		Payload.Path = PathOf({ Seg(TEXT("Payload"), INDEX_NONE, TEXT("FDstTestPayload")), Seg(TEXT("Amount")) });
		TestEqualSensitive(TEXT("StateEvent:Root/Hurt:Payload<...>.Amount"), Payload.ToString(), FString(TEXT("StateEvent:Root/Hurt:Payload<FDstTestPayload>.Amount")));

		// Different references are spelled differently.
		TSet<FString> Seen;
		const EDstSourceKind Kinds[] = { EDstSourceKind::Parameters, EDstSourceKind::Context, EDstSourceKind::Global, EDstSourceKind::Node,
			EDstSourceKind::StateParameters, EDstSourceKind::StateEvent, EDstSourceKind::TransitionEvent };
		for (const EDstSourceKind Kind : Kinds)
		{
			const FDstSourceRef Ref = MakeRef(Kind, TEXT("Root/A"), { TEXT("X") });
			TestTrue(FString::Printf(TEXT("%s is set"), LexToString(Kind)), Ref.IsSet());
			TestFalse(FString::Printf(TEXT("%s is spelled differently from the others"), LexToString(Kind)), Seen.Contains(Ref.ToString()));
			Seen.Add(Ref.ToString());
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelEnumNamesTest, "DreamStateTree.Lang.Model.EnumNames",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelEnumNamesTest::RunTest(const FString& Parameters)
	{
		CheckEnumNames(*this, TEXT("EDstSourceKind"), static_cast<int32>(EDstSourceKind::TransitionEvent),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstSourceKind>(Value))); });
		CheckEnumNames(*this, TEXT("EDstValueKind"), static_cast<int32>(EDstValueKind::Output),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstValueKind>(Value))); });
		CheckEnumNames(*this, TEXT("EDstNodeRole"), static_cast<int32>(EDstNodeRole::PropertyFunction),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstNodeRole>(Value))); });
		CheckEnumNames(*this, TEXT("EDstOperand"), static_cast<int32>(EDstOperand::Multiply),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstOperand>(Value))); });
		CheckEnumNames(*this, TEXT("EDstTransitionTrigger"), static_cast<int32>(EDstTransitionTrigger::OnDelegate),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstTransitionTrigger>(Value))); });
		CheckEnumNames(*this, TEXT("EDstTransitionTarget"), static_cast<int32>(EDstTransitionTarget::None),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstTransitionTarget>(Value))); });
		CheckEnumNames(*this, TEXT("EDstStateType"), static_cast<int32>(EDstStateType::Subtree),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstStateType>(Value))); });
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelDefaultsTest, "DreamStateTree.Lang.Model.Defaults",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelDefaultsTest::RunTest(const FString& Parameters)
	{
		// What a field holds before anyone sets it is part of the contract: the extractor and the lowering leave these alone
		// and the dump prints them.
		const FDstPathSegment Segment = FDstPathSegment();
		TestEqual(TEXT("a segment is no element"), Segment.ArrayIndex, static_cast<int32>(INDEX_NONE));
		TestTrue(TEXT("and has no instance type"), Segment.InstanceType.IsEmpty());

		const FDstState State = FDstState();
		TestTrue(TEXT("a state is a State"), State.Type == EDstStateType::State);
		TestEqualSensitive(TEXT("selects in order"), State.Selection, FString(TEXT("TrySelectChildrenInOrder")));
		TestEqualSensitive(TEXT("completes when any task does"), State.TasksCompletion, FString(TEXT("Any")));
		TestTrue(TEXT("is enabled"), State.bEnabled);
		TestTrue(TEXT("checks prerequisites"), State.bCheckPrerequisitesWhenActivatingChildDirectly);
		TestFalse(TEXT("copies no bindings on tick"), State.bCopyParameterBindingsOnTick);
		TestFalse(TEXT("has no custom tick rate"), State.bCustomTickRate);
		TestFalse(TEXT("has no event"), State.Event.IsSet());

		const FDstTransition Transition = FDstTransition();
		TestTrue(TEXT("a transition fires on completion"), Transition.Trigger == EDstTransitionTrigger::OnStateCompleted);
		TestTrue(TEXT("and goes to a state"), Transition.TargetKind == EDstTransitionTarget::GotoState);
		TestEqualSensitive(TEXT("with normal priority"), Transition.Priority, FString(TEXT("Normal")));
		TestTrue(TEXT("is enabled"), Transition.bEnabled);
		TestFalse(TEXT("has no delay"), Transition.bDelay);
		TestFalse(TEXT("does not reactivate"), Transition.bReactivate);
		TestTrue(TEXT("waits for no completion"), Transition.Completion == EDstCompletion::None);

		const FDstEvent Event = FDstEvent();
		TestTrue(TEXT("an event consumes itself on select"), Event.bConsumeOnSelect);

		const FDstNode Node = FDstNode();
		TestTrue(TEXT("a node is a task"), Node.Role == EDstNodeRole::Task);
		TestTrue(TEXT("starts an expression"), Node.Operand == EDstOperand::Copy);
		TestEqual(TEXT("at indent 0"), Node.Indent, 0);

		const FDstArg Arg = FDstArg();
		TestTrue(TEXT("an argument is an instance constant"), Arg.Side == EDstPropertySide::Instance && Arg.Kind == EDstValueKind::Constant);
		TestFalse(TEXT("without a function"), Arg.Function.IsValid());
		TestFalse(TEXT("or a source"), Arg.Source.IsSet());

		const FDstParameter Parameter = FDstParameter();
		TestFalse(TEXT("a parameter overrides nothing"), Parameter.bOverridden);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Lookups
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelUpdatePathsTest, "DreamStateTree.Lang.Model.UpdatePaths",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelUpdatePathsTest::RunTest(const FString& Parameters)
	{
		FDstTree Tree = MakeSampleTree();
		Tree.UpdatePaths();

		TestEqualSensitive(TEXT("Root"), Tree.States[0].Path, FString(TEXT("Root")));
		TestEqualSensitive(TEXT("Root/Attack"), Tree.States[0].Children[0].Path, FString(TEXT("Root/Attack")));
		TestEqualSensitive(TEXT("Root/Attack/Combo"), Tree.States[0].Children[0].Children[0].Path, FString(TEXT("Root/Attack/Combo")));
		TestEqualSensitive(TEXT("Root/Hurt"), Tree.States[0].Children[1].Path, FString(TEXT("Root/Hurt")));
		TestEqualSensitive(TEXT("a subtree is its own root"), Tree.States[1].Path, FString(TEXT("HitReaction")));
		TestEqualSensitive(TEXT("HitReaction/Stagger"), Tree.States[1].Children[0].Path, FString(TEXT("HitReaction/Stagger")));

		// Lookups find the state itself, not a copy.
		const FDstTree& ConstTree = Tree;
		TestTrue(TEXT("FindState Root"), ConstTree.FindState(TEXT("Root")) == &Tree.States[0]);
		TestTrue(TEXT("FindState Root/Attack/Combo"), ConstTree.FindState(TEXT("Root/Attack/Combo")) == &Tree.States[0].Children[0].Children[0]);
		TestTrue(TEXT("FindState HitReaction"), ConstTree.FindState(TEXT("HitReaction")) == &Tree.States[1]);
		TestTrue(TEXT("FindState HitReaction/Stagger"), ConstTree.FindState(TEXT("HitReaction/Stagger")) == &Tree.States[1].Children[0]);
		TestTrue(TEXT("the non-const FindState finds the same state"), Tree.FindState(TEXT("Root/Hurt")) == &Tree.States[0].Children[1]);
		FDstState* Modifiable = Tree.FindState(TEXT("Root/Rest"));
		TestNotNull(TEXT("FindState Root/Rest"), Modifiable);
		if (Modifiable)
		{
			Modifiable->Description = TEXT("changed through the pointer");
			TestEqualSensitive(TEXT("and it can be changed in place"), Tree.States[0].Children[2].Description, FString(TEXT("changed through the pointer")));
		}

		TestNull(TEXT("an unknown path"), ConstTree.FindState(TEXT("Root/Nowhere")));
		TestNull(TEXT("a child without its parent"), ConstTree.FindState(TEXT("Attack")));
		TestNull(TEXT("a subtree's child under the wrong root"), ConstTree.FindState(TEXT("Root/Stagger")));
		TestNull(TEXT("the empty path"), ConstTree.FindState(FString()));
		TestNull(TEXT("a path with a trailing slash"), ConstTree.FindState(TEXT("Root/")));

		// A rename moves the path after the next UpdatePaths, and a name with a space is part of the path as it is.
		Tree.States[0].Children[0].Name = TEXT("Move To");
		Tree.UpdatePaths();
		TestEqualSensitive(TEXT("the renamed state's path"), Tree.States[0].Children[0].Path, FString(TEXT("Root/Move To")));
		TestEqualSensitive(TEXT("and its child's"), Tree.States[0].Children[0].Children[0].Path, FString(TEXT("Root/Move To/Combo")));
		TestNotNull(TEXT("found by the new path"), ConstTree.FindState(TEXT("Root/Move To/Combo")));
		TestNull(TEXT("and not by the old"), ConstTree.FindState(TEXT("Root/Attack/Combo")));

		// Calling it again changes nothing.
		const FString Before = DumpDstModel(Tree);
		Tree.UpdatePaths();
		TestEqualSensitive(TEXT("UpdatePaths is idempotent"), DumpDstModel(Tree), Before);

		const FDstTree Empty = FDstTree();
		TestNull(TEXT("an empty tree has no states"), Empty.FindState(TEXT("Root")));
		TestNull(TEXT("and no nodes"), Empty.FindNode(TEXT("Root/Task[0]")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelFindNodeTest, "DreamStateTree.Lang.Model.FindNode",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelFindNodeTest::RunTest(const FString& Parameters)
	{
		FDstTree Tree = MakeSampleTree();
		Tree.UpdatePaths();

		const FDstState& Attack = Tree.States[0].Children[0];
		const FDstNode& Move = Attack.Tasks[0];

		TestTrue(TEXT("a global"), Tree.FindNode(TEXT("Global[0]")) == &Tree.Globals[0]);
		TestTrue(TEXT("a task"), Tree.FindNode(TEXT("Root/Attack/Task[0]")) == &Move);
		TestTrue(TEXT("the second task"), Tree.FindNode(TEXT("Root/Attack/Task[1]")) == &Attack.Tasks[1]);
		TestTrue(TEXT("an enter condition"), Tree.FindNode(TEXT("Root/Attack/EnterCondition[1]")) == &Attack.EnterConditions[1]);
		TestTrue(TEXT("a consideration"), Tree.FindNode(TEXT("Root/Attack/Consideration[1]")) == &Attack.Considerations[1]);
		TestTrue(TEXT("a transition condition"), Tree.FindNode(TEXT("Root/Transition[1]/Condition[0]")) == &Tree.States[0].Transitions[1].Conditions[0]);
		TestTrue(TEXT("a task in a subtree"), Tree.FindNode(TEXT("HitReaction/Stagger/Task[0]")) == &Tree.States[1].Children[0].Tasks[0]);

		// Property functions live inside arguments, one inside another.
		const TSharedPtr<FDstNode>& Multiply = Move.Args[3].Function;
		TestTrue(TEXT("the fixture has the function"), Multiply.IsValid() && Move.Args[3].Kind == EDstValueKind::Function);
		if (Multiply.IsValid())
		{
			TestTrue(TEXT("a property function"), Tree.FindNode(TEXT("Root/Attack/Task[0]/Fn:AcceptableRadius")) == Multiply.Get());
			const TSharedPtr<FDstNode>& Add = Multiply->Args[1].Function;
			TestTrue(TEXT("the fixture has the inner function"), Add.IsValid());
			if (Add.IsValid())
			{
				TestTrue(TEXT("a property function inside a property function"), Tree.FindNode(TEXT("Root/Attack/Task[0]/Fn:AcceptableRadius/Fn:Right")) == Add.Get());
			}
		}

		TestNull(TEXT("an unknown key"), Tree.FindNode(TEXT("Root/Attack/Task[7]")));
		TestNull(TEXT("a state path is not a node key"), Tree.FindNode(TEXT("Root/Attack")));
		TestNull(TEXT("the empty key"), Tree.FindNode(FString()));
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// The dump
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelDumpStableTest, "DreamStateTree.Lang.Model.DumpStable",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelDumpStableTest::RunTest(const FString& Parameters)
	{
		const FString Dump = DumpOf(MakeSampleTree());
		TestFalse(TEXT("a dump has text"), Dump.IsEmpty());

		// The same model dumps the same text however often, and from a copy.
		TestEqualSensitive(TEXT("twice from one tree"), DumpOf(MakeSampleTree()), Dump);
		FDstTree Tree = MakeSampleTree();
		Tree.UpdatePaths();
		const FString First = DumpDstModel(Tree);
		TestEqualSensitive(TEXT("again from the same tree"), DumpDstModel(Tree), First);
		const FDstTree Copy = Tree;
		TestEqualSensitive(TEXT("from a copy"), DumpDstModel(Copy), First);
		TestEqualSensitive(TEXT("the dump of the same model built with its maps filled the other way round"), DumpOf(MakeSampleTree(/*bReverseMaps*/ true)), Dump);

		TArray<FString> Lines;
		Dump.ParseIntoArrayLines(Lines);
		TestTrue(TEXT("a dump of a real tree has a line for each item"), Lines.Num() > 30);

		// The text of values that matter is in the dump.
		const TCHAR* Wanted[] = { TEXT("Root"), TEXT("Attack"), TEXT("Combo"), TEXT("HitReaction"), TEXT("Courage"), TEXT("Game.Event.Hit"),
			TEXT("StateTreeMultiplyFloatPropertyFunction"), TEXT("TargetPlayerCharacter"), TEXT("Combat.Attack") };
		for (const TCHAR* Text : Wanted)
		{
			TestTrue(FString::Printf(TEXT("the dump mentions `%s`"), Text), Dump.Contains(Text, ESearchCase::CaseSensitive));
		}

		// Keys can be left out, to compare trees that differ only in where a node sits.
		FDstDumpOptions WithoutKeys;
		WithoutKeys.bKeys = false;
		const FString KeylessDump = DumpDstModel(Tree, WithoutKeys);
		TestTrue(TEXT("the default dump has node keys"), Dump.Contains(TEXT("Root/Attack/Task[1]"), ESearchCase::CaseSensitive));
		TestFalse(TEXT("the dump without keys has none"), KeylessDump.Contains(TEXT("Root/Attack/Task[1]"), ESearchCase::CaseSensitive));
		TestFalse(TEXT("and is not empty"), KeylessDump.IsEmpty());
		TestNotEqual(TEXT("so the two differ"), KeylessDump, Dump);

		// A key alone differs: the default dump shows it, the dump without keys does not.
		FDstTree Renamed = MakeSampleTree();
		Renamed.States[0].Children[0].Tasks[1].Key = TEXT("Root/Attack/Task[renamed]");
		Renamed.UpdatePaths();
		TestNotEqual(TEXT("a different key changes the dump"), DumpDstModel(Renamed), First);
		TestEqualSensitive(TEXT("but not the dump without keys"), DumpDstModel(Renamed, WithoutKeys), KeylessDump);

		// An empty tree can be dumped.
		const FDstTree Empty = FDstTree();
		TestEqualSensitive(TEXT("an empty tree dumps the same twice"), DumpDstModel(Empty), DumpDstModel(Empty));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangModelDumpCoversEveryFieldTest, "DreamStateTree.Lang.Model.DumpCoversEveryField",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangModelDumpCoversEveryFieldTest::RunTest(const FString& Parameters)
	{
		// "Two models are equal exactly when their dumps are": change any one field of the sample and the dump changes.
		TArray<FMutation> Mutations;

		// The tree.
		Mutations.Add({ TEXT("Tree.AssetPath"), [](FDstTree& T) { T.AssetPath += TEXT("2"); } });
		Mutations.Add({ TEXT("Tree.SchemaClass"), [](FDstTree& T) { T.SchemaClass += TEXT("2"); } });
		Mutations.Add({ TEXT("Tree.SchemaProperties value"), [](FDstTree& T) { T.SchemaProperties.FindOrAdd(TEXT("ContextActorClass")) += TEXT("2"); } });
		Mutations.Add({ TEXT("Tree.SchemaProperties key"), [](FDstTree& T) { T.SchemaProperties.Add(TEXT("ContextNew"), TEXT("x")); } });
		Mutations.Add({ TEXT("Tree.Settings value"), [](FDstTree& T) { T.Settings.FindOrAdd(TEXT("GlobalTasksCompletion")) = TEXT("All"); } });
		Mutations.Add({ TEXT("Tree.Settings key"), [](FDstTree& T) { T.Settings.Add(TEXT("New"), TEXT("x")); } });
		Mutations.Add({ TEXT("Tree.Parameters[0].Name"), [](FDstTree& T) { T.Parameters[0].Name += TEXT("2"); } });
		Mutations.Add({ TEXT("Tree.Parameters[0].Type"), [](FDstTree& T) { T.Parameters[0].Type = TEXT("double"); } });
		Mutations.Add({ TEXT("Tree.Parameters[0].ValueText"), [](FDstTree& T) { T.Parameters[0].ValueText = TEXT("0.6"); } });
		Mutations.Add({ TEXT("Tree.Parameters[0].bOverridden"), [](FDstTree& T) { T.Parameters[0].bOverridden = !T.Parameters[0].bOverridden; } });
		Mutations.Add({ TEXT("Tree.Parameters[0].Meta value"), [](FDstTree& T) { T.Parameters[0].Meta.FindOrAdd(TEXT("Tooltip")) += TEXT("2"); } });
		Mutations.Add({ TEXT("Tree.Parameters[0].Meta key"), [](FDstTree& T) { T.Parameters[0].Meta.Add(TEXT("New"), TEXT("x")); } });
		Mutations.Add({ TEXT("Tree.Parameters added"), [](FDstTree& T) { T.Parameters.Add(MakeParameter(TEXT("Extra"), TEXT("bool"), TEXT("true"))); } });
		Mutations.Add({ TEXT("Tree.Parameters order"), [](FDstTree& T) { T.Parameters.Swap(0, 1); } });
		Mutations.Add({ TEXT("Tree.Globals added"), [](FDstTree& T) { T.Globals.Add(MakeNode(TEXT("Global[1]"), EDstNodeRole::Evaluator, TEXT("/Script/Game.Eval"))); } });
		Mutations.Add({ TEXT("Tree.Colors[0].Name"), [](FDstTree& T) { T.Colors[0].Name += TEXT("2"); } });
		Mutations.Add({ TEXT("Tree.Colors[0].ColorText"), [](FDstTree& T) { T.Colors[0].ColorText = TEXT("(R=0,G=0,B=0,A=1.0)"); } });
		Mutations.Add({ TEXT("Tree.States order"), [](FDstTree& T) { T.States.Swap(0, 1); } });

		// A node (the first global, so the same fields serve every role).
		Mutations.Add({ TEXT("Node.Key"), [](FDstTree& T) { T.Globals[0].Key = TEXT("Global[9]"); } });
		Mutations.Add({ TEXT("Node.Role"), [](FDstTree& T) { T.Globals[0].Role = EDstNodeRole::Evaluator; } });
		Mutations.Add({ TEXT("Node.StructType"), [](FDstTree& T) { T.Globals[0].StructType += TEXT("2"); } });
		Mutations.Add({ TEXT("Node.BlueprintClass"), [](FDstTree& T) { T.Globals[0].BlueprintClass = TEXT("/Game/BP_Task.BP_Task_C"); } });
		Mutations.Add({ TEXT("Node.BindName"), [](FDstTree& T) { T.Globals[0].BindName += TEXT("2"); } });
		Mutations.Add({ TEXT("Node.Operand"), [](FDstTree& T) { T.Globals[0].Operand = EDstOperand::Or; } });
		Mutations.Add({ TEXT("Node.Indent"), [](FDstTree& T) { T.Globals[0].Indent = 2; } });
		Mutations.Add({ TEXT("Node.Args added"), [](FDstTree& T) { T.Globals[0].Args.Add(ConstantArg(EDstPropertySide::Instance, TEXT("Extra"), TEXT("1"))); } });

		// An argument (argument 0 of task 0 of Attack: a constant).
		Mutations.Add({ TEXT("Arg.Side"), [](FDstTree& T) { MoveOf(T).Args[0].Side = EDstPropertySide::Node; } });
		Mutations.Add({ TEXT("Arg.Property name"), [](FDstTree& T) { MoveOf(T).Args[0].Property.Segments[0].Name += TEXT("2"); } });
		Mutations.Add({ TEXT("Arg.Property segment added"), [](FDstTree& T) { MoveOf(T).Args[0].Property.Segments.Add(Seg(TEXT("Inner"))); } });
		Mutations.Add({ TEXT("Arg.Property array index"), [](FDstTree& T) { MoveOf(T).Args[0].Property.Segments[0].ArrayIndex = 1; } });
		Mutations.Add({ TEXT("Arg.Property instance type"), [](FDstTree& T) { MoveOf(T).Args[0].Property.Segments[0].InstanceType = TEXT("FSomething"); } });
		Mutations.Add({ TEXT("Arg.Kind"), [](FDstTree& T) { MoveOf(T).Args[0].Kind = EDstValueKind::Ref; } });
		Mutations.Add({ TEXT("Arg.ConstantText"), [](FDstTree& T) { MoveOf(T).Args[0].ConstantText = TEXT("3.0"); } });
		// Argument 2 is a binding to a global.
		Mutations.Add({ TEXT("Arg.Source.Kind"), [](FDstTree& T) { MoveOf(T).Args[2].Source.Kind = EDstSourceKind::Node; } });
		Mutations.Add({ TEXT("Arg.Source.Owner"), [](FDstTree& T) { MoveOf(T).Args[2].Source.Owner = TEXT("Global[1]"); } });
		Mutations.Add({ TEXT("Arg.Source.Path"), [](FDstTree& T) { MoveOf(T).Args[2].Source.Path.Segments[0].Name = TEXT("Other"); } });
		// Argument 3 is a function holding a function.
		Mutations.Add({ TEXT("Function.Key"), [](FDstTree& T) { MoveOf(T).Args[3].Function->Key += TEXT("2"); } });
		Mutations.Add({ TEXT("Function.StructType"), [](FDstTree& T) { MoveOf(T).Args[3].Function->StructType += TEXT("2"); } });
		Mutations.Add({ TEXT("Function.Args[0] source"), [](FDstTree& T) { MoveOf(T).Args[3].Function->Args[0].Source.Path.Segments[0].Name = TEXT("Other"); } });
		Mutations.Add({ TEXT("Function.Function constant"), [](FDstTree& T) { MoveOf(T).Args[3].Function->Args[1].Function->Args[0].ConstantText = TEXT("9.0"); } });
		Mutations.Add({ TEXT("Function.Function source"), [](FDstTree& T) { MoveOf(T).Args[3].Function->Args[1].Function->Args[1].Source.Path.Segments[0].Name = TEXT("Other"); } });
		Mutations.Add({ TEXT("Function.Function key"), [](FDstTree& T) { MoveOf(T).Args[3].Function->Args[1].Function->Key += TEXT("2"); } });
		// Argument 5 is an output, argument 6 a longer path with an index and an instance type.
		Mutations.Add({ TEXT("Output.Source"), [](FDstTree& T) { MoveOf(T).Args[5].Source.Path.Segments[0].Name = TEXT("Other"); } });
		Mutations.Add({ TEXT("Path.Index"), [](FDstTree& T) { MoveOf(T).Args[6].Source.Path.Segments[1].ArrayIndex = 3; } });
		Mutations.Add({ TEXT("Path.InstanceType"), [](FDstTree& T) { MoveOf(T).Args[6].Source.Path.Segments[0].InstanceType = TEXT("/Script/Game.Other"); } });

		// A state (Attack).
		Mutations.Add({ TEXT("State.Name"), [](FDstTree& T) { AttackOf(T).Name = TEXT("Strike"); } });
		Mutations.Add({ TEXT("State.Type"), [](FDstTree& T) { AttackOf(T).Type = EDstStateType::Group; } });
		Mutations.Add({ TEXT("State.Selection"), [](FDstTree& T) { AttackOf(T).Selection = TEXT("TryEnterState"); } });
		Mutations.Add({ TEXT("State.TasksCompletion"), [](FDstTree& T) { AttackOf(T).TasksCompletion = TEXT("All"); } });
		Mutations.Add({ TEXT("State.Tag"), [](FDstTree& T) { AttackOf(T).Tag += TEXT("2"); } });
		Mutations.Add({ TEXT("State.ColorName"), [](FDstTree& T) { AttackOf(T).ColorName += TEXT("2"); } });
		Mutations.Add({ TEXT("State.Description"), [](FDstTree& T) { AttackOf(T).Description += TEXT("2"); } });
		Mutations.Add({ TEXT("State.bEnabled"), [](FDstTree& T) { AttackOf(T).bEnabled = !AttackOf(T).bEnabled; } });
		Mutations.Add({ TEXT("State.WeightText"), [](FDstTree& T) { AttackOf(T).WeightText = TEXT("3"); } });
		Mutations.Add({ TEXT("State.bCustomTickRate"), [](FDstTree& T) { AttackOf(T).bCustomTickRate = !AttackOf(T).bCustomTickRate; } });
		Mutations.Add({ TEXT("State.TickRateText"), [](FDstTree& T) { AttackOf(T).TickRateText = TEXT("0.5"); } });
		Mutations.Add({ TEXT("State.bCopyParameterBindingsOnTick"), [](FDstTree& T) { AttackOf(T).bCopyParameterBindingsOnTick = !AttackOf(T).bCopyParameterBindingsOnTick; } });
		Mutations.Add({ TEXT("State.bCheckPrerequisitesWhenActivatingChildDirectly"), [](FDstTree& T) { AttackOf(T).bCheckPrerequisitesWhenActivatingChildDirectly = !AttackOf(T).bCheckPrerequisitesWhenActivatingChildDirectly; } });
		Mutations.Add({ TEXT("State.bFixedLayout"), [](FDstTree& T) { AttackOf(T).bFixedLayout = !AttackOf(T).bFixedLayout; } });
		Mutations.Add({ TEXT("State.Event removed"), [](FDstTree& T) { AttackOf(T).Event.Reset(); } });
		Mutations.Add({ TEXT("State.Event.Tag"), [](FDstTree& T) { AttackOf(T).Event->Tag += TEXT("2"); } });
		Mutations.Add({ TEXT("State.Event.PayloadType"), [](FDstTree& T) { AttackOf(T).Event->PayloadType = TEXT(""); } });
		Mutations.Add({ TEXT("State.Event.bConsumeOnSelect"), [](FDstTree& T) { AttackOf(T).Event->bConsumeOnSelect = !AttackOf(T).Event->bConsumeOnSelect; } });
		Mutations.Add({ TEXT("State.LinkTarget"), [](FDstTree& T) { HurtOf(T).LinkTarget += TEXT("2"); } });
		Mutations.Add({ TEXT("State.Parameters value"), [](FDstTree& T) { HurtOf(T).Parameters[0].ValueText = TEXT("2.5"); } });
		Mutations.Add({ TEXT("State.Parameters bOverridden"), [](FDstTree& T) { HurtOf(T).Parameters[0].bOverridden = false; } });
		Mutations.Add({ TEXT("State.Parameters meta"), [](FDstTree& T) { SubtreeOf(T).Parameters[0].Meta.FindOrAdd(TEXT("Category")) += TEXT("2"); } });
		Mutations.Add({ TEXT("State.EnterConditions[1].Operand"), [](FDstTree& T) { AttackOf(T).EnterConditions[1].Operand = EDstOperand::Or; } });
		Mutations.Add({ TEXT("State.EnterConditions[1].Indent"), [](FDstTree& T) { AttackOf(T).EnterConditions[1].Indent = 2; } });
		Mutations.Add({ TEXT("State.EnterConditions removed"), [](FDstTree& T) { AttackOf(T).EnterConditions.Pop(); } });
		Mutations.Add({ TEXT("State.Considerations[1].Operand"), [](FDstTree& T) { AttackOf(T).Considerations[1].Operand = EDstOperand::And; } });
		Mutations.Add({ TEXT("State.Tasks order"), [](FDstTree& T) { AttackOf(T).Tasks.Swap(0, 1); } });
		Mutations.Add({ TEXT("State.Tasks removed"), [](FDstTree& T) { AttackOf(T).Tasks.Pop(); } });
		Mutations.Add({ TEXT("State.Children order"), [](FDstTree& T) { RootOf(T).Children.Swap(0, 1); } });
		Mutations.Add({ TEXT("State.Children added"), [](FDstTree& T) { FDstState Extra; Extra.Name = TEXT("Extra"); RootOf(T).Children.Add(Extra); } });

		// A transition (Root's: 0 completed, 1 event, 2 delegate).
		Mutations.Add({ TEXT("Transition.Trigger"), [](FDstTree& T) { RootOf(T).Transitions[0].Trigger = EDstTransitionTrigger::OnStateFailed; } });
		Mutations.Add({ TEXT("Transition.TargetKind"), [](FDstTree& T) { RootOf(T).Transitions[2].TargetKind = EDstTransitionTarget::NextParent; } });
		Mutations.Add({ TEXT("Transition.TargetState"), [](FDstTree& T) { RootOf(T).Transitions[1].TargetState = TEXT("Root/Rest"); } });
		Mutations.Add({ TEXT("Transition.Event.Tag"), [](FDstTree& T) { RootOf(T).Transitions[1].Event.Tag += TEXT("2"); } });
		Mutations.Add({ TEXT("Transition.Event.PayloadType"), [](FDstTree& T) { RootOf(T).Transitions[1].Event.PayloadType = TEXT(""); } });
		Mutations.Add({ TEXT("Transition.Event.bConsumeOnSelect"), [](FDstTree& T) { RootOf(T).Transitions[1].Event.bConsumeOnSelect = true; } });
		Mutations.Add({ TEXT("Transition.Delegate.Kind"), [](FDstTree& T) { RootOf(T).Transitions[2].Delegate.Kind = EDstSourceKind::Global; } });
		Mutations.Add({ TEXT("Transition.Delegate.Owner"), [](FDstTree& T) { RootOf(T).Transitions[2].Delegate.Owner += TEXT("2"); } });
		Mutations.Add({ TEXT("Transition.Delegate.Path"), [](FDstTree& T) { RootOf(T).Transitions[2].Delegate.Path = NamesPath({ TEXT("OnLost") }); } });
		Mutations.Add({ TEXT("Transition.Completion"), [](FDstTree& T) { RootOf(T).Transitions[2].Completion = EDstCompletion::Fails; } });
		Mutations.Add({ TEXT("Transition.Priority"), [](FDstTree& T) { RootOf(T).Transitions[1].Priority = TEXT("Low"); } });
		Mutations.Add({ TEXT("Transition.bDelay"), [](FDstTree& T) { RootOf(T).Transitions[1].bDelay = false; } });
		Mutations.Add({ TEXT("Transition.DelayText"), [](FDstTree& T) { RootOf(T).Transitions[1].DelayText = TEXT("0.6"); } });
		Mutations.Add({ TEXT("Transition.VarianceText"), [](FDstTree& T) { RootOf(T).Transitions[1].VarianceText = TEXT("0.2"); } });
		Mutations.Add({ TEXT("Transition.bReactivate"), [](FDstTree& T) { RootOf(T).Transitions[1].bReactivate = false; } });
		Mutations.Add({ TEXT("Transition.bEnabled"), [](FDstTree& T) { RootOf(T).Transitions[1].bEnabled = true; } });
		Mutations.Add({ TEXT("Transition.Conditions constant"), [](FDstTree& T) { RootOf(T).Transitions[1].Conditions[0].Args[1].ConstantText = TEXT("0.9"); } });
		Mutations.Add({ TEXT("Transition.Conditions removed"), [](FDstTree& T) { RootOf(T).Transitions[1].Conditions.Reset(); } });
		Mutations.Add({ TEXT("Transition order"), [](FDstTree& T) { RootOf(T).Transitions.Swap(0, 2); } });
		Mutations.Add({ TEXT("Transition removed"), [](FDstTree& T) { RootOf(T).Transitions.Pop(); } });

		const FString Baseline = DumpOf(MakeSampleTree());
		for (const FMutation& Mutation : Mutations)
		{
			FDstTree Tree = MakeSampleTree();
			Mutation.Apply(Tree);
			const FString Mutated = DumpOf(Tree);
			if (Mutated.Equals(Baseline, ESearchCase::CaseSensitive))
			{
				AddError(FString::Printf(TEXT("Changing %s does not change the dump."), Mutation.Name));
			}
		}
		return true;
	}
}
