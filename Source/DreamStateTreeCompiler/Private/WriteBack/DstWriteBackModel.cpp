// Copyright DreamMoon. All Rights Reserved.
//
// The model, looked at piece by piece: dumps of a state with parts left out (to tell what kind of difference two states
// have), the settings of a state's [ ], and the alignment of the asset's states and transitions with the text's.
//
// FString compares without regard to case; every comparison here that decides something is case-sensitive.

#include "WriteBack/DstWriteBackInternal.h"

namespace UE::DreamStateTree::Compiler::WriteBackPrivate
{
	void WbStripArgs(TArray<Model::FDstNode>& Nodes)
	{
		for (Model::FDstNode& Node : Nodes)
		{
			Node.Args.Reset();
		}
	}

	/** The settings of [ ] back to what a card starts from when it says nothing. */
	void WbBlankSettings(Model::FDstState& State)
	{
		const Model::FDstState Defaults;
		State.Tag.Reset();
		State.ColorName.Reset();
		State.Description.Reset();
		State.bEnabled = Defaults.bEnabled;
		State.TasksCompletion = Defaults.TasksCompletion;
		State.WeightText.Reset();
		State.bCustomTickRate = Defaults.bCustomTickRate;
		State.TickRateText.Reset();
		State.bCopyParameterBindingsOnTick = Defaults.bCopyParameterBindingsOnTick;
		State.bCheckPrerequisitesWhenActivatingChildDirectly = Defaults.bCheckPrerequisitesWhenActivatingChildDirectly;
		State.bFixedLayout = Defaults.bFixedLayout;
		if (State.Event.IsSet())
		{
			State.Event->bConsumeOnSelect = Model::FDstEvent().bConsumeOnSelect;
		}
		if (State.Type == Model::EDstStateType::Group)
		{
			State.Type = Model::EDstStateType::State;
		}
	}

	FString DumpStateView(const Model::FDstState& State, const FOwnView& View)
	{
		Model::FDstState Copy = State;
		if (!View.bWithChildren)
		{
			Copy.Children.Reset();
		}
		if (View.bStripArgs)
		{
			WbStripArgs(Copy.EnterConditions);
			WbStripArgs(Copy.Considerations);
			WbStripArgs(Copy.Tasks);
			for (Model::FDstTransition& Transition : Copy.Transitions)
			{
				WbStripArgs(Transition.Conditions);
			}
		}
		if (View.bBlankSettings)
		{
			WbBlankSettings(Copy);
		}
		Model::FDstTree Tree;
		Tree.States.Add(MoveTemp(Copy));
		return Model::DumpDstModel(Tree);
	}

	FString DumpNodeOf(const Model::FDstNode& Node)
	{
		Model::FDstState Probe;
		Probe.Name = TEXT("N");
		Probe.Tasks.Add(Node);
		Model::FDstTree Tree;
		Tree.States.Add(MoveTemp(Probe));
		return Model::DumpDstModel(Tree);
	}

	FString DumpNodesOf(const TArray<Model::FDstNode>& Nodes)
	{
		Model::FDstState Probe;
		Probe.Name = TEXT("N");
		Probe.Tasks = Nodes;
		Model::FDstTree Tree;
		Tree.States.Add(MoveTemp(Probe));
		return Model::DumpDstModel(Tree);
	}

	FString DumpTransitionOf(const Model::FDstTransition& Transition)
	{
		Model::FDstState Probe;
		Probe.Name = TEXT("N");
		Probe.Transitions.Add(Transition);
		Model::FDstTree Tree;
		Tree.States.Add(MoveTemp(Probe));
		return Model::DumpDstModel(Tree);
	}

	bool IsLinkedState(const Model::FDstState& State)
	{
		return State.Type == Model::EDstStateType::Linked || State.Type == Model::EDstStateType::LinkedAsset;
	}

	bool SettingsDiffer(const Model::FDstState& A, const Model::FDstState& B)
	{
		if (!A.Tag.Equals(B.Tag, ESearchCase::CaseSensitive) || !A.ColorName.Equals(B.ColorName, ESearchCase::CaseSensitive)
			|| !A.Description.Equals(B.Description, ESearchCase::CaseSensitive) || A.bEnabled != B.bEnabled
			|| !A.TasksCompletion.Equals(B.TasksCompletion, ESearchCase::CaseSensitive) || !A.WeightText.Equals(B.WeightText, ESearchCase::CaseSensitive)
			|| A.bCustomTickRate != B.bCustomTickRate || !A.TickRateText.Equals(B.TickRateText, ESearchCase::CaseSensitive)
			|| A.bCopyParameterBindingsOnTick != B.bCopyParameterBindingsOnTick
			|| A.bCheckPrerequisitesWhenActivatingChildDirectly != B.bCheckPrerequisitesWhenActivatingChildDirectly)
		{
			return true;
		}
		// A link sets the layout itself and the text does not write it.
		if (!IsLinkedState(A) && !IsLinkedState(B) && A.bFixedLayout != B.bFixedLayout)
		{
			return true;
		}
		if (A.Event.IsSet() && B.Event.IsSet() && A.Event->bConsumeOnSelect != B.Event->bConsumeOnSelect)
		{
			return true;
		}
		const bool bAGroup = A.Type == Model::EDstStateType::Group;
		const bool bBGroup = B.Type == Model::EDstStateType::Group;
		return bAGroup != bBGroup;
	}

	FString LinkKeyOf(const Model::FDstState& State)
	{
		if (!IsLinkedState(State))
		{
			return FString();
		}
		Model::FDstState Probe;
		Probe.Name = TEXT("L");
		Probe.Type = State.Type;
		Probe.LinkTarget = State.LinkTarget;
		Probe.Parameters = State.Parameters;
		Probe.ParameterBindings = State.ParameterBindings;
		Model::FDstTree Tree;
		Tree.States.Add(MoveTemp(Probe));
		return Model::DumpDstModel(Tree);
	}

	FString EventKeyOf(const Model::FDstState& State)
	{
		if (!State.Event.IsSet())
		{
			return FString();
		}
		Model::FDstState Probe;
		Probe.Name = TEXT("E");
		Probe.Event = State.Event;
		Probe.Event->bConsumeOnSelect = Model::FDstEvent().bConsumeOnSelect;
		Model::FDstTree Tree;
		Tree.States.Add(MoveTemp(Probe));
		return Model::DumpDstModel(Tree);
	}

	FString ParamsKeyOf(const Model::FDstState& State)
	{
		if (IsLinkedState(State))
		{
			return FString();
		}
		Model::FDstState Probe;
		Probe.Name = TEXT("P");
		Probe.Parameters = State.Parameters;
		Model::FDstTree Tree;
		Tree.States.Add(MoveTemp(Probe));
		return Model::DumpDstModel(Tree);
	}

	FString PickKeyOf(const Model::FDstState& State)
	{
		return IsLinkedState(State) ? FString() : State.Selection;
	}

	// ------------------------------------------------------------------------------------------
	// Alignment
	// ------------------------------------------------------------------------------------------

	void WbCollectStates(const TArray<Model::FDstState>& States, TArray<const Model::FDstState*>& Out)
	{
		for (const Model::FDstState& State : States)
		{
			Out.Add(&State);
			WbCollectStates(State.Children, Out);
		}
	}

	void FWriteBack::AlignStates()
	{
		TArray<const Model::FDstState*> TStates;
		TArray<const Model::FDstState*> AStates;
		WbCollectStates(T.States, TStates);
		WbCollectStates(A.States, AStates);

		TSet<FString> UsedT;
		const auto Pair = [this, &UsedT](const Model::FDstState& AState, const Model::FDstState& TState)
		{
			AToT.Add(AState.Path, TState.Path);
			TToA.Add(TState.Path, AState.Path);
			UsedT.Add(TState.Path);
		};
		// A top-level state and a nested one are never the same state: moving one across that line is a remove and an add.
		const auto SameLevel = [](const Model::FDstState& AState, const Model::FDstState& TState)
		{
			return AState.Path.Contains(TEXT("/")) == TState.Path.Contains(TEXT("/"));
		};

		// By the ID the generator derived from the state's place in the text: it follows a state through a rename or a move.
		TMap<FGuid, const Model::FDstState*> TByGuid;
		for (const Model::FDstState* State : TStates)
		{
			TByGuid.Add(MakeDstGuid(T.AssetPath, State->Path), State);
		}
		for (const Model::FDstState* AState : AStates)
		{
			const FGuid* Id = Extracted.Ids.Find(AState->Path);
			if (!Id || !Id->IsValid())
			{
				continue;
			}
			const Model::FDstState* const* Hit = TByGuid.Find(*Id);
			if (Hit && !UsedT.Contains((*Hit)->Path) && !AToT.Contains(AState->Path) && SameLevel(*AState, **Hit))
			{
				Pair(*AState, **Hit);
			}
		}

		// An asset saved under another name has new IDs everywhere: then the path is what is left.
		for (const Model::FDstState* AState : AStates)
		{
			if (AToT.Contains(AState->Path))
			{
				continue;
			}
			const Model::FDstState* TState = T.FindState(AState->Path);
			if (TState && !UsedT.Contains(TState->Path) && SameLevel(*AState, *TState))
			{
				Pair(*AState, *TState);
			}
		}
	}

	bool FWriteBack::ChildrenAligned(const Model::FDstState& AState, const Model::FDstState& TState) const
	{
		if (AState.Children.Num() != TState.Children.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < AState.Children.Num(); ++Index)
		{
			const FString* Mapped = AToT.Find(AState.Children[Index].Path);
			if (!Mapped || !Mapped->Equals(TState.Children[Index].Path, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	TArray<int32> FWriteBack::AlignTransitions(const Model::FDstState& AState, const Model::FDstState& TState) const
	{
		TArray<int32> Matches;
		Matches.Init(INDEX_NONE, AState.Transitions.Num());
		TArray<bool> TUsed;
		TUsed.Init(false, TState.Transitions.Num());

		// By ID.
		for (int32 AIndex = 0; AIndex < AState.Transitions.Num(); ++AIndex)
		{
			const FGuid* Id = Extracted.Ids.Find(FString::Printf(TEXT("%s#%d"), *AState.Path, AIndex));
			if (!Id || !Id->IsValid())
			{
				continue;
			}
			for (int32 TIndex = 0; TIndex < TState.Transitions.Num(); ++TIndex)
			{
				if (!TUsed[TIndex] && MakeDstGuid(T.AssetPath, FString::Printf(TEXT("%s#%d"), *TState.Path, TIndex)) == *Id)
				{
					Matches[AIndex] = TIndex;
					TUsed[TIndex] = true;
					break;
				}
			}
		}

		// The same transition, wherever it stands now.
		for (int32 AIndex = 0; AIndex < AState.Transitions.Num(); ++AIndex)
		{
			if (Matches[AIndex] != INDEX_NONE)
			{
				continue;
			}
			const FString Wanted = DumpTransitionOf(AState.Transitions[AIndex]);
			for (int32 TIndex = 0; TIndex < TState.Transitions.Num(); ++TIndex)
			{
				if (!TUsed[TIndex] && DumpTransitionOf(TState.Transitions[TIndex]).Equals(Wanted, ESearchCase::CaseSensitive))
				{
					Matches[AIndex] = TIndex;
					TUsed[TIndex] = true;
					break;
				}
			}
		}

		// What is left of both sides, in order: a transition changed where it stands.
		int32 NextT = 0;
		for (int32 AIndex = 0; AIndex < AState.Transitions.Num(); ++AIndex)
		{
			if (Matches[AIndex] != INDEX_NONE)
			{
				continue;
			}
			while (NextT < TState.Transitions.Num() && TUsed[NextT])
			{
				++NextT;
			}
			if (NextT < TState.Transitions.Num())
			{
				Matches[AIndex] = NextT;
				TUsed[NextT] = true;
			}
		}
		return Matches;
	}
}
