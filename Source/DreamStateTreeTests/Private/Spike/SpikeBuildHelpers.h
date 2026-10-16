// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/StrongObjectPtr.h"

#include "Fixtures/DstTestBlueprintNodes.h"
#include "Fixtures/DstTestNodes.h"
#include "Fixtures/DstTestSchema.h"
#include "Fixtures/DstTestTypes.h"

class UStateTreeTaskBlueprintBase;
struct FStateTreeExecutionContext;

/**
 * Base class of the experiments that build and compile trees. Many of them provoke engine errors on purpose (a
 * binding to a missing property, a schema violation), and the engine logs those. The experiments report what they
 * find through AddInfo and the saved report, so log errors and warnings are not test failures here.
 */
class FDstSpikeBuildTestBase : public FAutomationTestBase
{
public:
	FDstSpikeBuildTestBase(const FString& InName, const bool bInComplexTask)
		: FAutomationTestBase(InName, bInComplexTask)
	{
	}

	virtual bool SuppressLogErrors() override { return true; }
	virtual bool SuppressLogWarnings() override { return true; }
};

// Helpers of the build experiments: deterministic IDs, a builder that creates trees the way the editor data is
// meant to be filled from code, the stock trees the experiments share, compile and run wrappers that turn the
// engine's logs into text.
namespace UE::DreamStateTree::Spike
{
	/** The ID every object of the same logical path gets: the same path always gives the same GUID. */
	FGuid DstBuildGuid(const FString& Path);

	//----------------------------------------------------------------------//
	// Logs and compile results
	//----------------------------------------------------------------------//

	/**
	 * Collects the engine log lines of the StateTree and property binding categories while it is alive. The
	 * compiler writes its own messages to FStateTreeCompilerLog, but the editor-side validation only logs.
	 */
	class FDstBuildLogCapture final : public FOutputDevice
	{
	public:
		FDstBuildLogCapture();
		virtual ~FDstBuildLogCapture() override;

		//~ Begin FOutputDevice
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		//~ End FOutputDevice

		/** The lines captured so far, "[Verbosity] Category: Message". */
		TArray<FString> GetLines() const;
		void Reset();

	private:
		mutable FCriticalSection Lock;
		TArray<FString> Lines;
	};

	/** What a compile returned and what the compiler log said. */
	struct FDstBuildCompileOutcome
	{
		bool bSucceeded = false;
		int32 NumErrors = 0;
		int32 NumWarnings = 0;
		int32 NumInfos = 0;
		/** One line per compiler message, "[Severity] state / node: text". */
		TArray<FString> Lines;

		/** "compiled" or "FAILED", with the counts. */
		FString Summary() const;
		/** The first error line, or empty. */
		FString FirstError() const;
		/** Summary and every line. */
		FString ToString() const;
	};

	/** Compiles with the compiler struct directly. */
	FDstBuildCompileOutcome DstBuildCompile(UStateTree& Tree);
	/** Compiles through the compiler manager, the path the editor and the factory use. */
	FDstBuildCompileOutcome DstBuildCompileViaManager(UStateTree& Tree);

	//----------------------------------------------------------------------//
	// Trees
	//----------------------------------------------------------------------//

	/**
	 * Creates a tree with editor data, schema and editor schema, filled in the way the factory does it, except that
	 * the root parameters ID is deterministic. Outer decides where it lives: the transient package for a throw-away
	 * tree, a package made with CreatePackage for an asset another tree can link to. The tree has no states yet.
	 */
	UStateTree* DstBuildNewTree(UObject* Outer, const TCHAR* BaseName, TSubclassOf<UStateTreeSchema> SchemaClass, EObjectFlags Flags = RF_NoFlags);

	/**
	 * Keeps trees (and the packages they live in) alive for the length of a test, and lets them go at the end.
	 * Trees created in a named package are moved to the transient package on destruction, so they are neither public
	 * nor standalone any more and the garbage collector can take them.
	 */
	class FDstBuildAssetKeeper : public FNoncopyable
	{
	public:
		~FDstBuildAssetKeeper();

		/** Keeps the object alive. Returns it for chaining. */
		template <typename T>
		T* Keep(T* Object)
		{
			KeepObject(Object);
			return Object;
		}

		/** Creates a package under /Temp/DreamStateTreeTests/<Folder> and a tree in it. Not saved, never written to disk. */
		UStateTree* NewPackagedTree(const TCHAR* Folder, const TCHAR* AssetName, TSubclassOf<UStateTreeSchema> SchemaClass);
		/** A tree in the transient package. */
		UStateTree* NewTransientTree(const TCHAR* BaseName, TSubclassOf<UStateTreeSchema> SchemaClass);
		/** The object that stands in for the execution owner. */
		UDstTestOwner* NewOwner();

	private:
		void KeepObject(UObject* Object);

		TArray<TStrongObjectPtr<UObject>> Objects;
	};

	/**
	 * Fills the editor data of a tree from code. Every GUID it creates (states, state parameters, nodes,
	 * transitions, parameter properties, property function nodes) is replaced at once by one derived from the
	 * logical path of the object, so building the same tree twice gives identical IDs. IDs are changed before any
	 * transition or link is made, because a link copies the ID of its target when it is created.
	 */
	class FDstBuildTreeBuilder
	{
	public:
		explicit FDstBuildTreeBuilder(UStateTree& InTree);

		UStateTree& GetTree() const { return *Tree; }
		UStateTreeEditorData& GetEditorData() const { return *EditorData; }
		/** ID of the root parameters struct, the source of bindings from root parameters. */
		FGuid GetRootParametersId() const { return EditorData->GetRootParametersGuid(); }
		/** The root parameter bag. The editor data only offers it read-only; adding properties from code is what the engine tests do too. */
		FInstancedPropertyBag& GetRootBag() const;

		//~ Parameters
		/** Adds a property to a bag with an ID derived from IdPath. */
		static void AddBagProperty(FInstancedPropertyBag& Bag, const FString& IdPath, const FName Name, const EPropertyBagPropertyType Type,
			const UObject* TypeObject = nullptr, const EPropertyBagContainerType Container = EPropertyBagContainerType::None);
		void AddRootParameter(const FName Name, const EPropertyBagPropertyType Type, const UObject* TypeObject = nullptr,
			const EPropertyBagContainerType Container = EPropertyBagContainerType::None);
		void AddStateParameter(UStateTreeState& State, const FName Name, const EPropertyBagPropertyType Type, const UObject* TypeObject = nullptr,
			const EPropertyBagContainerType Container = EPropertyBagContainerType::None);
		/** ID of the property of that name in the bag; invalid when absent. */
		static FGuid FindBagPropertyId(const FInstancedPropertyBag& Bag, const FName Name);
		/** Makes sure the root parameters hold the float "Speed" the stock tasks bind their input to. */
		void EnsureSpeedParameter();

		//~ States
		UStateTreeState& AddSubTree(const FName Name, const EStateTreeStateType Type = EStateTreeStateType::State);
		UStateTreeState& AddChild(UStateTreeState& Parent, const FName Name, const EStateTreeStateType Type = EStateTreeStateType::State);

		//~ Nodes. A returned reference stays valid only until the next node is added to the same container.
		template <typename T, typename... TArgs>
		TStateTreeEditorNode<T>& AddEvaluator(const FName NodeName, TArgs&&... Args)
		{
			TStateTreeEditorNode<T>& Node = EditorData->AddEvaluator<T>(Forward<TArgs>(Args)...);
			FinishNode(Node, FString::Printf(TEXT("Global/Evaluator:%d"), EditorData->Evaluators.Num() - 1), NodeName);
			return Node;
		}

		template <typename T, typename... TArgs>
		TStateTreeEditorNode<T>& AddGlobalTask(const FName NodeName, TArgs&&... Args)
		{
			TStateTreeEditorNode<T>& Node = EditorData->AddGlobalTask<T>(Forward<TArgs>(Args)...);
			FinishNode(Node, FString::Printf(TEXT("Global/Task:%d"), EditorData->GlobalTasks.Num() - 1), NodeName);
			return Node;
		}

		template <typename T, typename... TArgs>
		TStateTreeEditorNode<T>& AddTask(UStateTreeState& State, const FName NodeName, TArgs&&... Args)
		{
			TStateTreeEditorNode<T>& Node = State.AddTask<T>(Forward<TArgs>(Args)...);
			FinishNode(Node, FString::Printf(TEXT("%s/Task:%d"), *State.GetPath(), State.Tasks.Num() - 1), NodeName);
			return Node;
		}

		template <typename T, typename... TArgs>
		TStateTreeEditorNode<T>& AddEnterCondition(UStateTreeState& State, const FName NodeName, TArgs&&... Args)
		{
			TStateTreeEditorNode<T>& Node = State.AddEnterCondition<T>(Forward<TArgs>(Args)...);
			FinishNode(Node, FString::Printf(TEXT("%s/EnterCondition:%d"), *State.GetPath(), State.EnterConditions.Num() - 1), NodeName);
			return Node;
		}

		template <typename T, typename... TArgs>
		TStateTreeEditorNode<T>& AddConsideration(UStateTreeState& State, const FName NodeName, TArgs&&... Args)
		{
			TStateTreeEditorNode<T>& Node = State.AddConsideration<T>(Forward<TArgs>(Args)...);
			FinishNode(Node, FString::Printf(TEXT("%s/Consideration:%d"), *State.GetPath(), State.Considerations.Num() - 1), NodeName);
			return Node;
		}

		template <typename T, typename... TArgs>
		TStateTreeEditorNode<T>& AddTransitionCondition(UStateTreeState& State, FStateTreeTransition& Transition, const FName NodeName, TArgs&&... Args)
		{
			TStateTreeEditorNode<T>& Node = Transition.AddConditionWithOuter<T>(&State, Forward<TArgs>(Args)...);
			FinishNode(Node, FString::Printf(TEXT("%s/Transition:%d/Condition:%d"), *State.GetPath(), IndexOfTransition(State, Transition), Transition.Conditions.Num() - 1), NodeName);
			return Node;
		}

		/** Adds a task whose instance data is a UObject of the given class (the Blueprint node storage path). */
		FStateTreeEditorNode& AddBlueprintTask(UStateTreeState& State, const FName NodeName, TSubclassOf<UStateTreeTaskBlueprintBase> TaskClass);

		/**
		 * Adds a FDstTestTask and binds its Input to the root parameter "Speed", the way every stock tree does,
		 * because an unbound Input is a compile error.
		 */
		TStateTreeEditorNode<FDstTestTask>& AddBoundTask(UStateTreeState& State, const FName NodeName, const int32 TicksToComplete = 2,
			const EStateTreeRunStatus CompletionStatus = EStateTreeRunStatus::Succeeded);
		TStateTreeEditorNode<FDstTestTask>& AddBoundGlobalTask(const FName NodeName, const int32 TicksToComplete = 1000000);
		/** Adds a FDstTestCondition whose Left is bound to the output of the evaluator with this ID. */
		FStateTreeEditorNode& AddBoundEnterCondition(UStateTreeState& State, const FName NodeName, const FGuid& CounterEvaluatorId, const float Right = 0.f);

		//~ Transitions
		FStateTreeTransition& AddTransition(UStateTreeState& From, const EStateTreeTransitionTrigger Trigger, const EStateTreeTransitionType Type,
			const UStateTreeState* Target = nullptr);
		/** An OnEvent transition; Tag and PayloadStruct are what the transition requires. */
		FStateTreeTransition& AddEventTransition(UStateTreeState& From, const FGameplayTag Tag, const UScriptStruct* PayloadStruct,
			const EStateTreeTransitionType Type, const UStateTreeState* Target = nullptr);

		//~ Bindings
		/** Source path "Prop.Sub[0]" relative to the struct with this ID. */
		static FPropertyBindingPath MakePath(const FGuid& StructId, const TCHAR* PathText);
		void Bind(const FGuid& SourceId, const TCHAR* SourcePath, const FGuid& TargetId, const TCHAR* TargetPath);
		/** Output binding: the value of a task's Output property is copied into the destination (a parameter). */
		void BindOutput(const FGuid& DestinationId, const TCHAR* DestinationPath, const FGuid& TaskId, const TCHAR* OutputPath);
		/**
		 * Binds the result of a property function to the target path. FunctionStructName is the reflected name
		 * without the F, e.g. "StateTreeAddFloatPropertyFunction". Returns the ID of the function node, which is the
		 * struct ID to bind the function's own inputs from or to; invalid when the struct was not found.
		 */
		FGuid AddFunctionBinding(const TCHAR* FunctionStructName, const FPropertyBindingPath& Target, const FString& IdPath);
		/** Sets a numeric property of the instance data of the function node with this ID (a value returned by AddFunctionBinding). */
		bool SetFunctionNumber(const FGuid& FunctionId, const FName PropertyName, const double Value);

		/** Number of nodes, bindings and so on, for reports. */
		FString DescribeSize() const;

	private:
		void FinishNode(FStateTreeEditorNode& Node, const FString& IdPath, const FName NodeName);
		void FixStateIds(UStateTreeState& State);
		static int32 IndexOfTransition(const UStateTreeState& State, const FStateTreeTransition& Transition);

		UStateTree* Tree = nullptr;
		UStateTreeEditorData* EditorData = nullptr;
	};

	//----------------------------------------------------------------------//
	// Reading and editing editor data by reflection
	//----------------------------------------------------------------------//

	/** Sets a float or double property of a struct by name. Returns false when the property is missing or of another type. */
	bool DstBuildSetNumber(FStateTreeDataView Data, const FName PropertyName, const double Value);

	/** The reflected struct of an engine node that has no public header, e.g. "StateTreeAddFloatPropertyFunction". */
	UScriptStruct* DstBuildFindModuleStruct(const TCHAR* ModuleName, const TCHAR* StructName);

	/** One GUID of the editor data with a label that does not depend on the GUID itself. */
	struct FDstBuildIdEntry
	{
		FString Label;
		FGuid Id;
	};
	/** Every ID the editor data holds: root parameters, parameter properties, states, nodes, transitions, function nodes. */
	TArray<FDstBuildIdEntry> DstBuildCollectIds(const UStateTreeEditorData& EditorData);

	/** Counts of what a tree holds, for reports. */
	struct FDstBuildSizes
	{
		int32 States = 0;
		int32 Tasks = 0;
		int32 EnterConditions = 0;
		int32 Considerations = 0;
		int32 Transitions = 0;
		int32 TransitionConditions = 0;
		int32 Evaluators = 0;
		int32 GlobalTasks = 0;
		int32 Bindings = 0;
		int32 FunctionBindings = 0;
		FString ToString() const;
	};
	FDstBuildSizes DstBuildMeasure(const UStateTreeEditorData& EditorData);

	//----------------------------------------------------------------------//
	// Stock trees
	//----------------------------------------------------------------------//

	/** Handles into the tree a stock builder made, for the experiments that edit it afterwards. */
	struct FDstBuildMinimalIds
	{
		FGuid Root;
		FGuid StateA;
		FGuid StateB;
		FGuid TaskA;
		FGuid EvaluatorCounter;
		FGuid EnterConditionA;
		FGuid ConsiderationA;
		FGuid TransitionAToB;
	};

	/**
	 * Smallest tree that has every part the experiments tamper with: root parameter Speed, evaluator Counter, state
	 * Root with children A (utility parent) and B, a bound task and enter condition in A, a consideration in B and
	 * an A -> B transition. It compiles without errors under the fixture schema.
	 */
	FDstBuildMinimalIds DstBuildMinimalTree(FDstBuildTreeBuilder& Builder);

	/** What the all-features tree contains, as lines for the report. */
	struct FDstBuildAllFeaturesInfo
	{
		TArray<FString> Features;
	};

	/** The tree with every authoring feature a text tree can express, built with deterministic IDs. */
	FDstBuildAllFeaturesInfo DstBuildAllFeaturesTree(FDstBuildTreeBuilder& Builder);

	/**
	 * A small tree to run. Idle and Patrol alternate; a Hit event (any Hit.*) makes Fight run Light or Heavy depending
	 * on the tag, then back to Idle. Global evaluator Counter ticks.
	 */
	void DstBuildEncounterTree(FDstBuildTreeBuilder& Builder);

	/** Wander picks one of four one-tick legs at random, again and again. */
	void DstBuildWanderTree(FDstBuildTreeBuilder& Builder);

	/** A tree meant to be linked as an asset: root parameters Radius and Count, a root state with a task bound to both. */
	void DstBuildLinkableChildTree(FDstBuildTreeBuilder& Builder);

	//----------------------------------------------------------------------//
	// Reports
	//----------------------------------------------------------------------//

	/** Text of a saved report: headings, plain lines and labelled indented blocks. */
	class FDstBuildReport
	{
	public:
		void Heading(const FString& Title)
		{
			Text += FString::Printf(TEXT("\n=== %s ===\n"), *Title);
		}

		void Line(const FString& Line)
		{
			Text += Line;
			Text += TEXT("\n");
		}

		void Lines(const TArray<FString>& InLines, const TCHAR* Prefix = TEXT("  "))
		{
			for (const FString& Line : InLines)
			{
				Text += Prefix;
				Text += Line;
				Text += TEXT("\n");
			}
		}

		/** "Label:" followed by the body indented, or "(none)" when the body is empty. */
		void Block(const FString& Label, const FString& Body)
		{
			Text += Label;
			Text += TEXT(":\n");
			if (Body.IsEmpty())
			{
				Text += TEXT("  (none)\n");
				return;
			}
			TArray<FString> BodyLines;
			Body.ParseIntoArrayLines(BodyLines, /*bCullEmpty*/ false);
			Lines(BodyLines);
		}

		const FString& ToString() const { return Text; }

	private:
		FString Text;
	};

	//----------------------------------------------------------------------//
	// Running
	//----------------------------------------------------------------------//

	struct FDstBuildRunOptions
	{
		/** Seed of the execution's random stream; the same seed must give the same run. */
		int32 RandomSeed = 1;
		int32 NumTicks = 10;
		float DeltaTime = 0.1f;
		/** Called before tick N (0-based) with the live context, e.g. to send an event. */
		TFunction<void(FStateTreeExecutionContext& Context, int32 TickIndex)> BeforeTick;
	};

	struct FDstBuildRunResult
	{
		bool bStarted = false;
		/** Active state names after Start and after every tick, joined with "/". Index 0 is after Start. */
		TArray<FString> ActiveStates;
		/** One line per step: status, active states, recorded transition count, what the nodes logged during the step. */
		TArray<FString> StepLines;
		/** Every execution log entry of the run. */
		FString Log;
		/** The same as StepLines joined; equal text means equal runs. */
		FString Trace;
		/** Names of the tree's state transitions the context recorded, "From > To" per recorded transition. */
		TArray<FString> RecordedTransitions;
	};

	/** Starts the tree with the owner as context, ticks it, stops it. Clears the execution log first. */
	FDstBuildRunResult DstBuildRunTree(UStateTree& Tree, UDstTestOwner& Owner, const FDstBuildRunOptions& Options);

	/** Active state names with consecutive duplicates removed. */
	TArray<FString> DstBuildCompressSequence(const TArray<FString>& Steps);
}
