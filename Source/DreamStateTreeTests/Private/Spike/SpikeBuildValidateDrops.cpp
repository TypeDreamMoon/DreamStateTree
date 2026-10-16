// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeBuildHelpers.h"
#include "Spike/SpikeDump.h"

#include "Components/StateTreeComponentSchema.h"
#include "Fixtures/DstTestTags.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"

// Each case builds a tree that breaks one rule, then asks what the engine does about it in two ways that happen in
// the editor: the validation that runs when a tree is opened (UStateTreeEditingSubsystem::ValidateStateTree, which
// may silently repair or drop things) and the compile (which may reject it, or quietly normalize). The editor data
// is dumped before and after, so a silent change shows up as a diff.

namespace UE::DreamStateTree::Spike
{
	namespace BuildValidateDropsPrivate
	{
		int32 CountDiffLines(const FString& Diff)
		{
			if (Diff.IsEmpty())
			{
				return 0;
			}
			TArray<FString> Lines;
			Diff.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
			return Lines.Num();
		}

		struct FDropCase
		{
			FString Name;
			/** What the case asks, one sentence. */
			FString Question;
			TSubclassOf<UStateTreeSchema> Schema;
			/** Builds the tree, including the breakage. Called once per tree; the case builds two. */
			TFunction<void(FDstBuildTreeBuilder& Builder)> Build;
		};

		struct FDropResult
		{
			int32 ValidateChangedLines = 0;
			int32 CompileChangedLines = 0;
			FDstBuildCompileOutcome CompileOnly;
			FDstBuildCompileOutcome ValidateThenCompile;
			FString Conclusion;
		};

		UStateTreeEditorData& EditorDataOf(UStateTree& Tree)
		{
			return *CastChecked<UStateTreeEditorData>(Tree.EditorData);
		}

		/** The all-purpose base: the minimal tree under the schema chosen by the case, then the breakage. */
		TFunction<void(FDstBuildTreeBuilder&)> OnMinimalTree(TFunction<void(FDstBuildTreeBuilder&, const FDstBuildMinimalIds&)> Break)
		{
			return [Break = MoveTemp(Break)](FDstBuildTreeBuilder& Builder)
			{
				const FDstBuildMinimalIds Ids = DstBuildMinimalTree(Builder);
				Break(Builder, Ids);
			};
		}

		TArray<FDropCase> MakeCases()
		{
			using namespace UE::DreamStateTree::Tests;

			TArray<FDropCase> Cases;
			const TSubclassOf<UStateTreeSchema> Fixture = UDstTestSchema::StaticClass();

			auto Add = [&Cases](const TCHAR* Name, const TCHAR* Question, const TSubclassOf<UStateTreeSchema> Schema, TFunction<void(FDstBuildTreeBuilder&)> Build)
			{
				FDropCase& Case = Cases.AddDefaulted_GetRef();
				Case.Name = Name;
				Case.Question = Question;
				Case.Schema = Schema;
				Case.Build = MoveTemp(Build);
			};

			Add(TEXT("BindingFromMissingSourceProperty"), TEXT("A binding whose source property does not exist on the source struct."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.Bind(Ids.EvaluatorCounter, TEXT("NoSuchProperty"), Ids.EnterConditionA, TEXT("Left"));
				}));

			Add(TEXT("BindingToMissingTargetProperty"), TEXT("A binding whose target property does not exist on the target struct."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.Bind(B.GetRootParametersId(), TEXT("Speed"), Ids.TaskA, TEXT("NoSuchTarget"));
				}));

			Add(TEXT("BindingSourceNodeRemoved"), TEXT("A binding whose source node (an evaluator) was deleted afterwards."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.GetEditorData().Evaluators.Reset();
				}));

			Add(TEXT("BindingTargetNodeRemoved"), TEXT("A binding whose target node (a task) was deleted afterwards."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.GetEditorData().GetMutableStateByID(Ids.StateA)->Tasks.Reset();
				}));

			Add(TEXT("NodeRefusedByFixtureSchema"), TEXT("A task that derives from FStateTreeTaskBase instead of the CommonBase, which the fixture schema refuses."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.AddTask<FDstTestForbiddenTask>(*B.GetEditorData().GetMutableStateByID(Ids.StateB), TEXT("Forbidden"));
				}));

			Add(TEXT("NodeRefusedByComponentSchema"), TEXT("The same refused task under the engine's StateTree Component schema."), UStateTreeComponentSchema::StaticClass(),
				[](FDstBuildTreeBuilder& B)
				{
					UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
					B.AddTask<FDstTestForbiddenTask>(Root, TEXT("Forbidden"));
				});

			Add(TEXT("AiNodeUnderComponentSchema"), TEXT("An AI-only task (MoveTo) under the StateTree Component schema, which has no AI controller in its context."), UStateTreeComponentSchema::StaticClass(),
				[](FDstBuildTreeBuilder& B)
				{
					UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
					if (UScriptStruct* MoveTo = DstBuildFindModuleStruct(TEXT("GameplayStateTreeModule"), TEXT("StateTreeMoveToTask")))
					{
						const UScriptStruct* MoveToStruct = MoveTo;
						UObject* Outer = &Root;
						FStateTreeEditorNode& Node = Root.Tasks.AddDefaulted_GetRef();
						Node.InitializeAs(Outer, MoveToStruct);
						Node.ID = DstBuildGuid(TEXT("Root/Task:0"));
					}
				});

			Add(TEXT("RestrictedSchemaFeatures"), TEXT("Evaluators, enter conditions, considerations and several tasks under a schema that allows none of them."), UDstTestRestrictedSchema::StaticClass(),
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.AddBoundTask(*B.GetEditorData().GetMutableStateByID(Ids.StateA), TEXT("TaskA2"), 1);
				}));

			Add(TEXT("RestrictedSchemaStateKinds"), TEXT("A Group state and a Try Follow Transitions state under a schema that refuses both."), UDstTestRestrictedSchema::StaticClass(),
				[](FDstBuildTreeBuilder& B)
				{
					UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
					Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
					B.AddChild(Root, TEXT("GroupChild"), EStateTreeStateType::Group);
					B.AddChild(Root, TEXT("Follower")).SelectionBehavior = EStateTreeStateSelectionBehavior::TryFollowTransitions;
				});

			Add(TEXT("DuplicateIds"), TEXT("Two states with the same ID and two tasks with the same ID."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					UStateTreeEditorData& EditorData = B.GetEditorData();
					UStateTreeState* StateA = EditorData.GetMutableStateByID(Ids.StateA);
					UStateTreeState* StateB = EditorData.GetMutableStateByID(Ids.StateB);
					StateB->ID = StateA->ID;
					StateB->Tasks[0].ID = StateA->Tasks[0].ID;
				}));

			Add(TEXT("FirstConditionOrAndIndent"), TEXT("The first enter condition is written as Or with indent 1."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					UStateTreeState* StateA = B.GetEditorData().GetMutableStateByID(Ids.StateA);
					StateA->EnterConditions[0].ExpressionOperand = EStateTreeExpressionOperand::Or;
					StateA->EnterConditions[0].ExpressionIndent = 1;
				}));

			Add(TEXT("UnboundInput"), TEXT("A task whose Input property has no binding."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.GetEditorData().RemovePropertyBinding(FDstBuildTreeBuilder::MakePath(Ids.TaskA, TEXT("InputValue")));
				}));

			Add(TEXT("ContextBoundToWrongType"), TEXT("A Context object property that is bound by hand to a float root parameter."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.Bind(B.GetRootParametersId(), TEXT("Speed"), Ids.TaskA, TEXT("Owner"));
				}));

			Add(TEXT("PlainBindingIntoOutputProperty"), TEXT("An ordinary binding whose target is an Output property (the output flag is missing)."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					B.AddRootParameter(TEXT("Level"), EPropertyBagPropertyType::Int32);
					B.Bind(B.GetRootParametersId(), TEXT("Level"), Ids.TaskA, TEXT("TickCount"));
				}));

			Add(TEXT("OutputBindingIntoInputProperty"), TEXT("An output binding whose target is an Input property (the output flag is set where it does not belong)."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					UStateTreeEditorData& EditorData = B.GetEditorData();
					EditorData.RemovePropertyBinding(FDstBuildTreeBuilder::MakePath(Ids.TaskA, TEXT("InputValue")));
					EditorData.EditorBindings.AddOutputBinding(FDstBuildTreeBuilder::MakePath(B.GetRootParametersId(), TEXT("Speed")),
						FDstBuildTreeBuilder::MakePath(Ids.TaskA, TEXT("InputValue")));
				}));

			Add(TEXT("TransitionToRemovedState"), TEXT("A transition to a state that was removed from the tree afterwards."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					UStateTreeEditorData& EditorData = B.GetEditorData();
					UStateTreeState* Root = EditorData.GetMutableStateByID(Ids.Root);
					UStateTreeState* StateB = EditorData.GetMutableStateByID(Ids.StateB);
					Root->Children.Remove(StateB);
				}));

			Add(TEXT("DelegateTransitionWithoutDispatcher"), TEXT("An OnDelegate transition that is not bound to any dispatcher."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					UStateTreeEditorData& EditorData = B.GetEditorData();
					UStateTreeState* StateA = EditorData.GetMutableStateByID(Ids.StateA);
					UStateTreeState* StateB = EditorData.GetMutableStateByID(Ids.StateB);
					B.AddTransition(*StateB, EStateTreeTransitionTrigger::OnDelegate, EStateTreeTransitionType::GotoState, StateA);
				}));

			Add(TEXT("EventTransitionToIncompatibleState"), TEXT("An OnEvent transition for Hit.Light to a state that requires Hit."), Fixture,
				OnMinimalTree([](FDstBuildTreeBuilder& B, const FDstBuildMinimalIds& Ids)
				{
					UStateTreeEditorData& EditorData = B.GetEditorData();
					UStateTreeState* StateA = EditorData.GetMutableStateByID(Ids.StateA);
					UStateTreeState* StateB = EditorData.GetMutableStateByID(Ids.StateB);
					StateB->bHasRequiredEventToEnter = true;
					StateB->RequiredEventToEnter.Tag = Tag_Event_Hit.GetTag();
					StateB->RequiredEventToEnter.PayloadStruct = FDstTestPayload::StaticStruct();
					B.AddEventTransition(*StateA, Tag_Event_Hit_Light.GetTag(), FDstTestPayload::StaticStruct(), EStateTreeTransitionType::GotoState, StateB);
				}));

			return Cases;
		}

		/** Builds the case's tree twice: one is compiled as it is, the other is validated first. */
		FDropResult RunCase(const FDropCase& Case, FDstBuildReport& Report)
		{
			FDropResult Result;
			FDstBuildAssetKeeper Keeper;
			FDstBuildLogCapture LogCapture;

			Report.Heading(Case.Name);
			Report.Line(Case.Question);

			// Compile without validating.
			UStateTree* CompileTree = Keeper.NewTransientTree(*(Case.Name + TEXT("_Compile")), Case.Schema);
			{
				FDstBuildTreeBuilder Builder(*CompileTree);
				Case.Build(Builder);
			}
			const FString CompileBefore = UE::DreamStateTree::Spike::DumpEditorData(EditorDataOf(*CompileTree));
			LogCapture.Reset();
			Result.CompileOnly = DstBuildCompile(*CompileTree);
			const TArray<FString> CompileLog = LogCapture.GetLines();
			const FString CompileAfter = UE::DreamStateTree::Spike::DumpEditorData(EditorDataOf(*CompileTree));
			const FString CompileDiff = UE::DreamStateTree::Spike::DiffLines(CompileBefore, CompileAfter);
			Result.CompileChangedLines = CountDiffLines(CompileDiff);

			// Validate, then compile.
			UStateTree* ValidateTree = Keeper.NewTransientTree(*(Case.Name + TEXT("_Validate")), Case.Schema);
			{
				FDstBuildTreeBuilder Builder(*ValidateTree);
				Case.Build(Builder);
			}
			const FString ValidateBefore = UE::DreamStateTree::Spike::DumpEditorData(EditorDataOf(*ValidateTree));
			LogCapture.Reset();
			UStateTreeEditingSubsystem::ValidateStateTree(ValidateTree);
			const TArray<FString> ValidateLog = LogCapture.GetLines();
			const FString ValidateAfter = UE::DreamStateTree::Spike::DumpEditorData(EditorDataOf(*ValidateTree));
			const FString ValidateDiff = UE::DreamStateTree::Spike::DiffLines(ValidateBefore, ValidateAfter);
			Result.ValidateChangedLines = CountDiffLines(ValidateDiff);

			LogCapture.Reset();
			Result.ValidateThenCompile = DstBuildCompile(*ValidateTree);
			const TArray<FString> ValidateCompileLog = LogCapture.GetLines();
			const FString FinalDiff = UE::DreamStateTree::Spike::DiffLines(ValidateAfter, UE::DreamStateTree::Spike::DumpEditorData(EditorDataOf(*ValidateTree)));

			Report.Block(TEXT("Validate: editor data changes (before -> after)"), ValidateDiff);
			Report.Block(TEXT("Validate: engine log"), FString::Join(ValidateLog, TEXT("\n")));
			Report.Line(TEXT("Compile only: ") + Result.CompileOnly.ToString());
			Report.Block(TEXT("Compile only: editor data changes (before -> after)"), CompileDiff);
			Report.Block(TEXT("Compile only: engine log"), FString::Join(CompileLog, TEXT("\n")));
			Report.Line(TEXT("Validate then compile: ") + Result.ValidateThenCompile.ToString());
			Report.Block(TEXT("Validate then compile: engine log"), FString::Join(ValidateCompileLog, TEXT("\n")));
			Report.Block(TEXT("Validate then compile: editor data changes made by the compile"), FinalDiff);

			Result.Conclusion = FString::Printf(TEXT("validate changed %d dump lines%s; compile alone %s and changed %d dump lines; validate then compile %s"),
				Result.ValidateChangedLines,
				ValidateLog.IsEmpty() ? TEXT("") : TEXT(" and logged"),
				*Result.CompileOnly.Summary(), Result.CompileChangedLines, *Result.ValidateThenCompile.Summary());
			const FString FirstError = !Result.CompileOnly.FirstError().IsEmpty() ? Result.CompileOnly.FirstError() : Result.ValidateThenCompile.FirstError();
			if (!FirstError.IsEmpty())
			{
				Result.Conclusion += FString::Printf(TEXT(" | first error: %s"), *FirstError);
			}
			return Result;
		}
	}
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstSpikeBuild_ValidateDrops, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Build.ValidateDrops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeBuild_ValidateDrops::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstBuildReport Report;
	Report.Line(TEXT("Each case is built twice. One tree is compiled as it is; the other goes through UStateTreeEditingSubsystem::ValidateStateTree first."));

	// The baseline must compile, or every other result is meaningless.
	{
		FDstBuildAssetKeeper Keeper;
		UStateTree* Baseline = Keeper.NewTransientTree(TEXT("Baseline"), UDstTestSchema::StaticClass());
		FDstBuildTreeBuilder Builder(*Baseline);
		DstBuildMinimalTree(Builder);
		const FDstBuildCompileOutcome Outcome = DstBuildCompile(*Baseline);
		Report.Heading(TEXT("Baseline (the unbroken minimal tree)"));
		Report.Line(Outcome.ToString());
		if (!Outcome.bSucceeded)
		{
			AddError(FString::Printf(TEXT("The unbroken baseline tree does not compile, the cases below mean nothing: %s"), *Outcome.FirstError()));
		}
		AddInfo(FString::Printf(TEXT("[ValidateDrops] Baseline: %s"), *Outcome.Summary()));
	}

	for (const UE::DreamStateTree::Spike::BuildValidateDropsPrivate::FDropCase& Case : UE::DreamStateTree::Spike::BuildValidateDropsPrivate::MakeCases())
	{
		const UE::DreamStateTree::Spike::BuildValidateDropsPrivate::FDropResult Result = UE::DreamStateTree::Spike::BuildValidateDropsPrivate::RunCase(Case, Report);
		AddInfo(FString::Printf(TEXT("[ValidateDrops] %s: %s"), *Case.Name, *Result.Conclusion));
	}

	const FString ReportPath = SaveReport(TEXT("Build"), TEXT("ValidateDrops"), Report.ToString());
	AddInfo(FString::Printf(TEXT("Report: %s"), *ReportPath));
	return true;
}
