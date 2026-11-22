// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeBuildHelpers.h"

#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Fixtures/DstTestExecutionLog.h"
#include "Fixtures/DstTestTags.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "StateTreeCompiler.h"
#include "StateTreeCompilerManager.h"
#include "StateTreeEditorSchema.h"
#include "StateTreeExecutionContext.h"
#include "StateTreeInstanceData.h"
#include "StateTreePropertyBindings.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Spike
{
	namespace BuildHelpersPrivate
	{
		/** The compiler log keeps its messages protected; this exposes them for reading. */
		struct FDstBuildCompilerLog : public FStateTreeCompilerLog
		{
			TConstArrayView<FStateTreeCompilerLogMessage> GetAllMessages() const { return Messages; }
		};

		const TCHAR* SeverityName(const int32 Severity)
		{
			switch (static_cast<EMessageSeverity::Type>(Severity))
			{
			case EMessageSeverity::Error: return TEXT("Error");
			case EMessageSeverity::PerformanceWarning: return TEXT("PerformanceWarning");
			case EMessageSeverity::Warning: return TEXT("Warning");
			case EMessageSeverity::Info: return TEXT("Info");
			default: return TEXT("Unknown");
			}
		}

		FDstBuildCompileOutcome ToOutcome(const bool bSucceeded, const FDstBuildCompilerLog& Log)
		{
			FDstBuildCompileOutcome Outcome;
			Outcome.bSucceeded = bSucceeded;
			for (const FStateTreeCompilerLogMessage& Message : Log.GetAllMessages())
			{
				switch (static_cast<EMessageSeverity::Type>(Message.Severity))
				{
				case EMessageSeverity::Error:
					++Outcome.NumErrors;
					break;
				case EMessageSeverity::PerformanceWarning:
				case EMessageSeverity::Warning:
					++Outcome.NumWarnings;
					break;
				default:
					++Outcome.NumInfos;
					break;
				}

				const FString StatePath = Message.State ? Message.State->GetPath() : FString();
				const FString ItemName = Message.Item.Name.IsNone() ? FString() : Message.Item.Name.ToString();
				FString Where = StatePath;
				if (!ItemName.IsEmpty())
				{
					Where += Where.IsEmpty() ? ItemName : FString::Printf(TEXT(" / %s"), *ItemName);
				}
				Outcome.Lines.Add(FString::Printf(TEXT("[%s] %s%s%s"), SeverityName(Message.Severity),
					*Where, Where.IsEmpty() ? TEXT("") : TEXT(": "), *Message.Message));
			}
			return Outcome;
		}
	}

	FGuid DstBuildGuid(const FString& Path)
	{
		return FGuid::NewDeterministicGuid(Path);
	}

	//----------------------------------------------------------------------//
	// FDstBuildLogCapture
	//----------------------------------------------------------------------//

	FDstBuildLogCapture::FDstBuildLogCapture()
	{
		if (GLog)
		{
			GLog->AddOutputDevice(this);
		}
	}

	FDstBuildLogCapture::~FDstBuildLogCapture()
	{
		if (GLog)
		{
			GLog->RemoveOutputDevice(this);
		}
	}

	void FDstBuildLogCapture::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
	{
		const FString CategoryName = Category.ToString();
		if (!CategoryName.StartsWith(TEXT("LogStateTree")) && !CategoryName.StartsWith(TEXT("LogPropertyBinding")))
		{
			return;
		}

		FScopeLock ScopeLock(&Lock);
		Lines.Add(FString::Printf(TEXT("[%s] %s: %s"), ::ToString(Verbosity), *CategoryName, Message));
	}

	TArray<FString> FDstBuildLogCapture::GetLines() const
	{
		FScopeLock ScopeLock(&Lock);
		return Lines;
	}

	void FDstBuildLogCapture::Reset()
	{
		FScopeLock ScopeLock(&Lock);
		Lines.Reset();
	}

	//----------------------------------------------------------------------//
	// Compile
	//----------------------------------------------------------------------//

	FString FDstBuildCompileOutcome::Summary() const
	{
		return FString::Printf(TEXT("%s (%d errors, %d warnings, %d infos)"),
			bSucceeded ? TEXT("compiled") : TEXT("FAILED"), NumErrors, NumWarnings, NumInfos);
	}

	FString FDstBuildCompileOutcome::FirstError() const
	{
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("[Error]")) || Line.StartsWith(TEXT("[CriticalError]")))
			{
				return Line;
			}
		}
		return FString();
	}

	FString FDstBuildCompileOutcome::ToString() const
	{
		FString Result = Summary();
		for (const FString& Line : Lines)
		{
			Result += TEXT("\n  ") + Line;
		}
		return Result;
	}

	FDstBuildCompileOutcome DstBuildCompile(UStateTree& Tree)
	{
		using namespace BuildHelpersPrivate;

		FDstBuildCompilerLog Log;
		FStateTreeCompiler Compiler(Log);
		const bool bResult = Compiler.Compile(Tree);
		return ToOutcome(bResult, Log);
	}

	FDstBuildCompileOutcome DstBuildCompileViaManager(UStateTree& Tree)
	{
		using namespace BuildHelpersPrivate;

		FDstBuildCompilerLog Log;
		const bool bResult = UE::StateTree::Compiler::FCompilerManager::CompileSynchronously(&Tree, Log);
		return ToOutcome(bResult, Log);
	}

	//----------------------------------------------------------------------//
	// Tree creation
	//----------------------------------------------------------------------//

	UStateTree* DstBuildNewTree(UObject* Outer, const TCHAR* BaseName, TSubclassOf<UStateTreeSchema> SchemaClass, EObjectFlags Flags)
	{
		UObject* TreeOuter = Outer ? Outer : GetTransientPackage();
		const FName TreeName = MakeUniqueObjectName(TreeOuter, UStateTree::StaticClass(), FName(BaseName));

		UStateTree* Tree = NewObject<UStateTree>(TreeOuter, TreeName, Flags);
		UStateTreeEditorData* EditorData = NewObject<UStateTreeEditorData>(Tree, UStateTreeEditorData::StaticClass(), NAME_None, RF_Transactional);
		Tree->EditorData = EditorData;
		EditorData->Schema = NewObject<UStateTreeSchema>(EditorData, SchemaClass.Get(), NAME_None, RF_Transactional);
		EditorData->EditorSchema = NewObject<UStateTreeEditorSchema>(EditorData, UStateTreeEditorSchema::StaticClass(), NAME_None, RF_Transactional);

		// The ID of the root parameters is a private property that PostInitProperties fills with a random GUID.
		// Replace it before anything binds to it.
		if (FStructProperty* GuidProperty = CastField<FStructProperty>(UStateTreeEditorData::StaticClass()->FindPropertyByName(TEXT("RootParametersGuid"))))
		{
			if (GuidProperty->Struct == TBaseStructure<FGuid>::Get())
			{
				*GuidProperty->ContainerPtrToValuePtr<FGuid>(EditorData) = DstBuildGuid(TEXT("Params"));
			}
		}

		return Tree;
	}

	FDstBuildAssetKeeper::~FDstBuildAssetKeeper()
	{
		for (TStrongObjectPtr<UObject>& Object : Objects)
		{
			UStateTree* Tree = Cast<UStateTree>(Object.Get());
			if (Tree && Tree->GetOutermost() != GetTransientPackage())
			{
				UPackage* Package = Tree->GetOutermost();
				UE::StateTree::Compiler::RenameObjectToTransientPackage(Tree);
				if (Package)
				{
					Package->SetDirtyFlag(false);
				}
			}
		}
		Objects.Reset();
	}

	void FDstBuildAssetKeeper::KeepObject(UObject* Object)
	{
		if (Object)
		{
			Objects.Emplace(Object);
		}
	}

	UStateTree* FDstBuildAssetKeeper::NewPackagedTree(const TCHAR* Folder, const TCHAR* AssetName, TSubclassOf<UStateTreeSchema> SchemaClass)
	{
		const FString PackageName = FString::Printf(TEXT("/Temp/DreamStateTreeTests/%s/%s"), Folder, AssetName);
		UPackage* Package = CreatePackage(*PackageName);
		UStateTree* Tree = DstBuildNewTree(Package, AssetName, SchemaClass, RF_Public | RF_Standalone | RF_Transactional);
		return Keep(Tree);
	}

	UStateTree* FDstBuildAssetKeeper::NewTransientTree(const TCHAR* BaseName, TSubclassOf<UStateTreeSchema> SchemaClass)
	{
		return Keep(DstBuildNewTree(nullptr, BaseName, SchemaClass));
	}

	UDstTestOwner* FDstBuildAssetKeeper::NewOwner()
	{
		return Keep(NewObject<UDstTestOwner>(GetTransientPackage()));
	}

	//----------------------------------------------------------------------//
	// FDstBuildTreeBuilder
	//----------------------------------------------------------------------//

	FDstBuildTreeBuilder::FDstBuildTreeBuilder(UStateTree& InTree)
		: Tree(&InTree)
		, EditorData(Cast<UStateTreeEditorData>(InTree.EditorData))
	{
		check(EditorData);
	}

	FInstancedPropertyBag& FDstBuildTreeBuilder::GetRootBag() const
	{
		return const_cast<FInstancedPropertyBag&>(EditorData->GetRootParametersPropertyBag());
	}

	void FDstBuildTreeBuilder::AddBagProperty(FInstancedPropertyBag& Bag, const FString& IdPath, const FName Name, const EPropertyBagPropertyType Type,
		const UObject* TypeObject, const EPropertyBagContainerType Container)
	{
		FPropertyBagPropertyDesc Desc(Name, Container, Type, TypeObject);
		// A valid ID in the descriptor is kept by AddProperties; an invalid one is replaced by a random GUID.
		Desc.ID = DstBuildGuid(IdPath);

		TArray<FPropertyBagPropertyDesc> Descs;
		Descs.Add(Desc);
		Bag.AddProperties(Descs);
	}

	void FDstBuildTreeBuilder::AddRootParameter(const FName Name, const EPropertyBagPropertyType Type, const UObject* TypeObject, const EPropertyBagContainerType Container)
	{
		AddBagProperty(GetRootBag(), FString::Printf(TEXT("Params/%s"), *Name.ToString()), Name, Type, TypeObject, Container);
	}

	void FDstBuildTreeBuilder::AddStateParameter(UStateTreeState& State, const FName Name, const EPropertyBagPropertyType Type, const UObject* TypeObject, const EPropertyBagContainerType Container)
	{
		AddBagProperty(State.Parameters.Parameters, FString::Printf(TEXT("%s/Parameters/%s"), *State.GetPath(), *Name.ToString()), Name, Type, TypeObject, Container);
	}

	FGuid FDstBuildTreeBuilder::FindBagPropertyId(const FInstancedPropertyBag& Bag, const FName Name)
	{
		const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(Name);
		return Desc ? Desc->ID : FGuid();
	}

	void FDstBuildTreeBuilder::EnsureSpeedParameter()
	{
		static const FName SpeedName(TEXT("Speed"));
		if (!FindBagPropertyId(GetRootBag(), SpeedName).IsValid())
		{
			AddRootParameter(SpeedName, EPropertyBagPropertyType::Float);
			GetRootBag().SetValueFloat(SpeedName, 4.f);
		}
	}

	void FDstBuildTreeBuilder::FixStateIds(UStateTreeState& State)
	{
		const FString Path = State.GetPath();
		State.ID = DstBuildGuid(Path);
		State.Parameters.ID = DstBuildGuid(Path + TEXT("/Parameters"));
	}

	UStateTreeState& FDstBuildTreeBuilder::AddSubTree(const FName Name, const EStateTreeStateType Type)
	{
		UStateTreeState& State = EditorData->AddSubTree(Name, Type);
		FixStateIds(State);
		return State;
	}

	UStateTreeState& FDstBuildTreeBuilder::AddChild(UStateTreeState& Parent, const FName Name, const EStateTreeStateType Type)
	{
		UStateTreeState& State = Parent.AddChildState(Name, Type);
		FixStateIds(State);
		return State;
	}

	void FDstBuildTreeBuilder::FinishNode(FStateTreeEditorNode& Node, const FString& IdPath, const FName NodeName)
	{
		Node.ID = DstBuildGuid(IdPath);
		Node.SetNodeName(NodeName);
	}

	int32 FDstBuildTreeBuilder::IndexOfTransition(const UStateTreeState& State, const FStateTreeTransition& Transition)
	{
		return State.Transitions.IndexOfByPredicate([&Transition](const FStateTreeTransition& Other)
			{
				return &Other == &Transition;
			});
	}

	FStateTreeEditorNode& FDstBuildTreeBuilder::AddBlueprintTask(UStateTreeState& State, const FName NodeName, TSubclassOf<UStateTreeTaskBlueprintBase> TaskClass)
	{
		FStateTreeBlueprintTaskWrapper Wrapper;
		Wrapper.TaskClass = TaskClass;
		TStateTreeEditorNode<FStateTreeBlueprintTaskWrapper>& Node = State.AddTask<FStateTreeBlueprintTaskWrapper>(MoveTemp(Wrapper));
		FinishNode(Node, FString::Printf(TEXT("%s/Task:%d"), *State.GetPath(), State.Tasks.Num() - 1), NodeName);
		return Node;
	}

	TStateTreeEditorNode<FDstTestTask>& FDstBuildTreeBuilder::AddBoundTask(UStateTreeState& State, const FName NodeName, const int32 TicksToComplete, const EStateTreeRunStatus CompletionStatus)
	{
		EnsureSpeedParameter();
		TStateTreeEditorNode<FDstTestTask>& Node = AddTask<FDstTestTask>(State, NodeName);
		Node.GetInstanceData().TicksToComplete = TicksToComplete;
		Node.GetInstanceData().CompletionStatus = CompletionStatus;
		Bind(GetRootParametersId(), TEXT("Speed"), Node.ID, TEXT("InputValue"));
		return Node;
	}

	TStateTreeEditorNode<FDstTestTask>& FDstBuildTreeBuilder::AddBoundGlobalTask(const FName NodeName, const int32 TicksToComplete)
	{
		EnsureSpeedParameter();
		TStateTreeEditorNode<FDstTestTask>& Node = AddGlobalTask<FDstTestTask>(NodeName);
		Node.GetInstanceData().TicksToComplete = TicksToComplete;
		Bind(GetRootParametersId(), TEXT("Speed"), Node.ID, TEXT("InputValue"));
		return Node;
	}

	FStateTreeEditorNode& FDstBuildTreeBuilder::AddBoundEnterCondition(UStateTreeState& State, const FName NodeName, const FGuid& CounterEvaluatorId, const float Right)
	{
		TStateTreeEditorNode<FDstTestCondition>& Node = AddEnterCondition<FDstTestCondition>(State, NodeName);
		Node.GetInstanceData().Right = Right;
		Bind(CounterEvaluatorId, TEXT("Counter"), Node.ID, TEXT("Left"));
		return Node;
	}

	FStateTreeTransition& FDstBuildTreeBuilder::AddTransition(UStateTreeState& From, const EStateTreeTransitionTrigger Trigger, const EStateTreeTransitionType Type, const UStateTreeState* Target)
	{
		FStateTreeTransition& Transition = From.AddTransition(Trigger, Type, Target);
		Transition.ID = DstBuildGuid(FString::Printf(TEXT("%s/Transition:%d"), *From.GetPath(), From.Transitions.Num() - 1));
		return Transition;
	}

	FStateTreeTransition& FDstBuildTreeBuilder::AddEventTransition(UStateTreeState& From, const FGameplayTag Tag, const UScriptStruct* PayloadStruct,
		const EStateTreeTransitionType Type, const UStateTreeState* Target)
	{
		FStateTreeTransition& Transition = From.AddTransition(EStateTreeTransitionTrigger::OnEvent, Tag, Type, Target);
		Transition.RequiredEvent.PayloadStruct = PayloadStruct;
		Transition.ID = DstBuildGuid(FString::Printf(TEXT("%s/Transition:%d"), *From.GetPath(), From.Transitions.Num() - 1));
		return Transition;
	}

	FPropertyBindingPath FDstBuildTreeBuilder::MakePath(const FGuid& StructId, const TCHAR* PathText)
	{
		FPropertyBindingPath Path;
		Path.SetStructID(StructId);
		Path.FromString(PathText);
		return Path;
	}

	void FDstBuildTreeBuilder::Bind(const FGuid& SourceId, const TCHAR* SourcePath, const FGuid& TargetId, const TCHAR* TargetPath)
	{
		EditorData->AddPropertyBinding(MakePath(SourceId, SourcePath), MakePath(TargetId, TargetPath));
	}

	void FDstBuildTreeBuilder::BindOutput(const FGuid& DestinationId, const TCHAR* DestinationPath, const FGuid& TaskId, const TCHAR* OutputPath)
	{
		EditorData->EditorBindings.AddOutputBinding(MakePath(DestinationId, DestinationPath), MakePath(TaskId, OutputPath));
	}

	FGuid FDstBuildTreeBuilder::AddFunctionBinding(const TCHAR* FunctionStructName, const FPropertyBindingPath& Target, const FString& IdPath)
	{
		UScriptStruct* FunctionStruct = DstBuildFindModuleStruct(TEXT("StateTreeModule"), FunctionStructName);
		if (!FunctionStruct)
		{
			return FGuid();
		}

		const FPropertyBindingPathSegment ResultSegment(TEXT("Result"));
		EditorData->AddPropertyBinding(FunctionStruct, MakeArrayView(&ResultSegment, 1), Target);

		// The function node got a random ID when it was created. The binding that owns the node was added last.
		TArrayView<FStateTreePropertyPathBinding> Bindings = EditorData->EditorBindings.GetMutableBindings();
		if (Bindings.IsEmpty())
		{
			return FGuid();
		}
		FStateTreePropertyPathBinding& Binding = Bindings.Last();
		FStateTreeEditorNode* FunctionNode = Binding.GetMutablePropertyFunctionNode().GetPtr<FStateTreeEditorNode>();
		if (!FunctionNode)
		{
			return FGuid();
		}

		FunctionNode->ID = DstBuildGuid(IdPath);
		// The source of the binding is the function's instance data, addressed by the node ID.
		Binding.GetMutableSourcePath().SetStructID(FunctionNode->GetInstanceDataID());
		return FunctionNode->GetInstanceDataID();
	}

	bool FDstBuildTreeBuilder::SetFunctionNumber(const FGuid& FunctionId, const FName PropertyName, const double Value)
	{
		for (FStateTreePropertyPathBinding& Binding : EditorData->EditorBindings.GetMutableBindings())
		{
			FStateTreeEditorNode* FunctionNode = Binding.GetMutablePropertyFunctionNode().GetPtr<FStateTreeEditorNode>();
			if (FunctionNode && FunctionNode->GetInstanceDataID() == FunctionId)
			{
				return DstBuildSetNumber(FunctionNode->GetInstance(), PropertyName, Value);
			}
		}
		return false;
	}

	FString FDstBuildTreeBuilder::DescribeSize() const
	{
		return DstBuildMeasure(*EditorData).ToString();
	}

	//----------------------------------------------------------------------//
	// Reflection helpers
	//----------------------------------------------------------------------//

	bool DstBuildSetNumber(FStateTreeDataView Data, const FName PropertyName, const double Value)
	{
		if (!Data.IsValid())
		{
			return false;
		}
		const FNumericProperty* Property = CastField<FNumericProperty>(Data.GetStruct()->FindPropertyByName(PropertyName));
		if (!Property)
		{
			return false;
		}

		void* ValueAddress = Property->ContainerPtrToValuePtr<void>(Data.GetMutableMemory());
		if (Property->IsFloatingPoint())
		{
			Property->SetFloatingPointPropertyValue(ValueAddress, Value);
		}
		else
		{
			Property->SetIntPropertyValue(ValueAddress, static_cast<int64>(Value));
		}
		return true;
	}

	UScriptStruct* DstBuildFindModuleStruct(const TCHAR* ModuleName, const TCHAR* StructName)
	{
		return FindObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/%s.%s"), ModuleName, StructName));
	}

	TArray<FDstBuildIdEntry> DstBuildCollectIds(const UStateTreeEditorData& EditorData)
	{
		TArray<FDstBuildIdEntry> Entries;
		auto Add = [&Entries](const FString& Label, const FGuid& Id)
		{
			FDstBuildIdEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Label = Label;
			Entry.Id = Id;
		};
		auto AddBag = [&Add](const FString& Prefix, const FInstancedPropertyBag& Bag)
		{
			if (const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
				{
					Add(FString::Printf(TEXT("%s/%s"), *Prefix, *Desc.Name.ToString()), Desc.ID);
				}
			}
		};

		Add(TEXT("RootParametersGuid"), EditorData.GetRootParametersGuid());
		AddBag(TEXT("RootParam"), EditorData.GetRootParametersPropertyBag());

		for (int32 Index = 0; Index < EditorData.Evaluators.Num(); ++Index)
		{
			Add(FString::Printf(TEXT("Global/Evaluator:%d"), Index), EditorData.Evaluators[Index].ID);
		}
		for (int32 Index = 0; Index < EditorData.GlobalTasks.Num(); ++Index)
		{
			Add(FString::Printf(TEXT("Global/Task:%d"), Index), EditorData.GlobalTasks[Index].ID);
		}

		EditorData.VisitHierarchy([&Add, &AddBag](UStateTreeState& State, UStateTreeState* /*Parent*/)
		{
			const FString Path = State.GetPath();
			Add(Path, State.ID);
			Add(Path + TEXT("/Parameters"), State.Parameters.ID);
			AddBag(Path + TEXT("/Param"), State.Parameters.Parameters);

			for (int32 Index = 0; Index < State.Tasks.Num(); ++Index)
			{
				Add(FString::Printf(TEXT("%s/Task:%d"), *Path, Index), State.Tasks[Index].ID);
			}
			for (int32 Index = 0; Index < State.EnterConditions.Num(); ++Index)
			{
				Add(FString::Printf(TEXT("%s/EnterCondition:%d"), *Path, Index), State.EnterConditions[Index].ID);
			}
			for (int32 Index = 0; Index < State.Considerations.Num(); ++Index)
			{
				Add(FString::Printf(TEXT("%s/Consideration:%d"), *Path, Index), State.Considerations[Index].ID);
			}
			if (State.SingleTask.GetNode().IsValid())
			{
				Add(Path + TEXT("/SingleTask"), State.SingleTask.ID);
			}
			for (int32 TransitionIndex = 0; TransitionIndex < State.Transitions.Num(); ++TransitionIndex)
			{
				const FStateTreeTransition& Transition = State.Transitions[TransitionIndex];
				Add(FString::Printf(TEXT("%s/Transition:%d"), *Path, TransitionIndex), Transition.ID);
				for (int32 ConditionIndex = 0; ConditionIndex < Transition.Conditions.Num(); ++ConditionIndex)
				{
					Add(FString::Printf(TEXT("%s/Transition:%d/Condition:%d"), *Path, TransitionIndex, ConditionIndex), Transition.Conditions[ConditionIndex].ID);
				}
			}
			return EStateTreeVisitor::Continue;
		});

		int32 BindingIndex = 0;
		for (const FStateTreePropertyPathBinding& Binding : EditorData.EditorBindings.GetBindings())
		{
			if (const FStateTreeEditorNode* FunctionNode = Binding.GetPropertyFunctionNode().GetPtr<FStateTreeEditorNode>())
			{
				Add(FString::Printf(TEXT("Binding:%d/Function[%s]"), BindingIndex, *Binding.GetTargetPath().ToString()), FunctionNode->ID);
			}
			++BindingIndex;
		}

		return Entries;
	}

	FString FDstBuildSizes::ToString() const
	{
		return FString::Printf(TEXT("states=%d tasks=%d enterConditions=%d considerations=%d transitions=%d transitionConditions=%d evaluators=%d globalTasks=%d bindings=%d (functions=%d)"),
			States, Tasks, EnterConditions, Considerations, Transitions, TransitionConditions, Evaluators, GlobalTasks, Bindings, FunctionBindings);
	}

	FDstBuildSizes DstBuildMeasure(const UStateTreeEditorData& EditorData)
	{
		FDstBuildSizes Sizes;
		Sizes.Evaluators = EditorData.Evaluators.Num();
		Sizes.GlobalTasks = EditorData.GlobalTasks.Num();
		EditorData.VisitHierarchy([&Sizes](UStateTreeState& State, UStateTreeState* /*Parent*/)
		{
			++Sizes.States;
			Sizes.Tasks += State.Tasks.Num();
			Sizes.EnterConditions += State.EnterConditions.Num();
			Sizes.Considerations += State.Considerations.Num();
			Sizes.Transitions += State.Transitions.Num();
			for (const FStateTreeTransition& Transition : State.Transitions)
			{
				Sizes.TransitionConditions += Transition.Conditions.Num();
			}
			return EStateTreeVisitor::Continue;
		});
		for (const FStateTreePropertyPathBinding& Binding : EditorData.EditorBindings.GetBindings())
		{
			++Sizes.Bindings;
			if (Binding.GetPropertyFunctionNode().IsValid())
			{
				++Sizes.FunctionBindings;
			}
		}
		return Sizes;
	}

	//----------------------------------------------------------------------//
	// Running
	//----------------------------------------------------------------------//

	namespace BuildHelpersPrivate
	{
		FString JoinNames(const TArray<FName>& Names)
		{
			FString Result;
			for (const FName& Name : Names)
			{
				Result += Result.IsEmpty() ? Name.ToString() : FString::Printf(TEXT("/%s"), *Name.ToString());
			}
			return Result;
		}

		FString StatusName(const EStateTreeRunStatus Status)
		{
			return StaticEnum<EStateTreeRunStatus>()->GetNameStringByValue(static_cast<int64>(Status));
		}

		FString OneLine(const FString& Text)
		{
			FString Result = Text;
			Result.TrimEndInline();
			Result.ReplaceInline(TEXT("\r\n"), TEXT(" ; "));
			Result.ReplaceInline(TEXT("\n"), TEXT(" ; "));
			return Result;
		}

		FString DescribeRecorded(const UStateTree& Tree, const FRecordedStateTreeTransitionResult& Recorded)
		{
			FString Path;
			for (const FRecordedActiveState& ActiveState : Recorded.States)
			{
				const UStateTree* OwnerTree = ActiveState.StateTree ? ActiveState.StateTree.Get() : &Tree;
				const FCompactStateTreeState* State = OwnerTree->GetStateFromHandle(ActiveState.State);
				const FString Name = State ? State->Name.ToString() : ActiveState.State.Describe();
				Path += Path.IsEmpty() ? Name : FString::Printf(TEXT("/%s"), *Name);
			}
			return Path;
		}
	}

	FDstBuildRunResult DstBuildRunTree(UStateTree& Tree, UDstTestOwner& Owner, const FDstBuildRunOptions& Options)
	{
		using namespace BuildHelpersPrivate;
		using UE::DreamStateTree::Tests::FExecutionLog;

		FDstBuildRunResult Result;
		FExecutionLog& Log = FExecutionLog::Get();
		Log.Reset();

		// The instance data outlives the context; declare it first.
		FStateTreeInstanceData InstanceData;
		FStateTreeExecutionContext Context(Owner, Tree, InstanceData, FOnCollectStateTreeExternalData(), EStateTreeRecordTransitions::Yes);
		Context.SetContextDataByName(UDstTestSchema::GetOwnerContextName(), FStateTreeDataView(&Owner));
		if (!Context.IsValid() || !Context.AreContextDataViewsValid())
		{
			return Result;
		}

		auto AddStep = [&](const FString& Label, const EStateTreeRunStatus Status, const int32 LogMark)
		{
			const FString Active = JoinNames(Context.GetActiveStateNames());
			Result.ActiveStates.Add(Active);
			Result.StepLines.Add(FString::Printf(TEXT("%-8s %s | %s | recorded=%d | %s"), *Label, *StatusName(Status), *Active,
				Context.GetRecordedTransitions().Num(), *OneLine(Log.ToString(LogMark))));
		};

		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		FStateTreeExecutionContext::FStartParameters StartParameters;
		StartParameters.RandomSeed = Options.RandomSeed;
		const EStateTreeRunStatus StartStatus = Context.Start(MoveTemp(StartParameters));
		PRAGMA_ENABLE_DEPRECATION_WARNINGS

		Result.bStarted = StartStatus == EStateTreeRunStatus::Running;
		AddStep(TEXT("start"), StartStatus, 0);

		for (int32 TickIndex = 0; TickIndex < Options.NumTicks; ++TickIndex)
		{
			const int32 LogMark = Log.Num();
			if (Options.BeforeTick)
			{
				Options.BeforeTick(Context, TickIndex);
			}
			const EStateTreeRunStatus Status = Context.Tick(Options.DeltaTime);
			AddStep(FString::Printf(TEXT("tick %d"), TickIndex), Status, LogMark);
		}

		{
			const int32 LogMark = Log.Num();
			const EStateTreeRunStatus StopStatus = Context.Stop();
			Result.StepLines.Add(FString::Printf(TEXT("%-8s %s | recorded=%d | %s"), TEXT("stop"), *StatusName(StopStatus),
				Context.GetRecordedTransitions().Num(), *OneLine(Log.ToString(LogMark))));
		}

		for (const FRecordedStateTreeTransitionResult& Recorded : Context.GetRecordedTransitions())
		{
			Result.RecordedTransitions.Add(DescribeRecorded(Tree, Recorded));
		}

		Result.Log = Log.ToString();
		Result.Trace = FString::Join(Result.StepLines, TEXT("\n"));
		return Result;
	}

	TArray<FString> DstBuildCompressSequence(const TArray<FString>& Steps)
	{
		TArray<FString> Result;
		for (const FString& Step : Steps)
		{
			if (Result.IsEmpty() || Result.Last() != Step)
			{
				Result.Add(Step);
			}
		}
		return Result;
	}
}
