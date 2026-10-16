// Copyright DreamMoon. All Rights Reserved.
//
// Checks over the finished model: what can only be judged once every state exists (names that repeat, an event a state
// waits for that no transition sends, a transition whose event the target state would refuse).

#include "DstLowerPrivate.h"

#include "GameplayTagsManager.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	using namespace Lang;
	using namespace Model;
	using Model::FDstNode;

	namespace
	{
		FGameplayTag RequestTag(const FString& Name)
		{
			if (Name.IsEmpty())
			{
				return FGameplayTag();
			}
			return UGameplayTagsManager::Get().RequestGameplayTag(FName(*Name), false);
		}

		/** One event can satisfy both tags: they are the same, or one is a parent of the other. */
		bool TagsRelated(const FGameplayTag& A, const FGameplayTag& B)
		{
			if (!A.IsValid() || !B.IsValid())
			{
				return true;
			}
			return A.MatchesTag(B) || B.MatchesTag(A);
		}

		bool IsAncestorOrSelf(const FString& Ancestor, const FString& Path)
		{
			return Path.Equals(Ancestor, ESearchCase::CaseSensitive)
				|| (Path.Len() > Ancestor.Len() && Path[Ancestor.Len()] == TEXT('/') && Path.StartsWith(Ancestor, ESearchCase::CaseSensitive));
		}

		/** One of them is in the other's lineage. */
		bool Related(const FString& A, const FString& B)
		{
			return IsAncestorOrSelf(A, B) || IsAncestorOrSelf(B, A);
		}

		FString RootOf(const FString& Path)
		{
			int32 Slash = INDEX_NONE;
			return Path.FindChar(TEXT('/'), Slash) ? Path.Left(Slash) : Path;
		}

		struct FFlatState
		{
			const FDstState* State = nullptr;
		};

		void Flatten(const TArray<FDstState>& List, TArray<const FDstState*>& Out)
		{
			for (const FDstState& State : List)
			{
				Out.Add(&State);
				Flatten(State.Children, Out);
			}
		}
	}

	void FDstLowerer::CheckDuplicateStateNames(const TArray<FDstState>& States, const FString& ParentPath)
	{
		for (int32 Index = 0; Index < States.Num(); ++Index)
		{
			for (int32 Earlier = 0; Earlier < Index; ++Earlier)
			{
				if (States[Earlier].Name.Equals(States[Index].Name, ESearchCase::CaseSensitive))
				{
					Sink.Error(TEXT("DST6001"), Out.Places.Find(PathOf(ParentPath, States[Index].Name)),
						FString::Printf(TEXT("There are two states called `%s` %s."), *States[Index].Name,
							ParentPath.IsEmpty() ? TEXT("at the top level") : *FString::Printf(TEXT("in `%s`"), *ParentPath)),
						TEXT("State names are unique among their siblings: a transition finds its target by name."));
					break;
				}
			}
			CheckDuplicateStateNames(States[Index].Children, PathOf(ParentPath, States[Index].Name));
		}
	}

	void FDstLowerer::CheckStructure()
	{
		CheckDuplicateStateNames(Out.Tree.States, FString());
		CheckEventReachability();
	}

	void FDstLowerer::CheckEventReachability()
	{
		TArray<const FDstState*> All;
		Flatten(Out.Tree.States, All);

		// Every transition an event can trigger, with the tag resolved once.
		struct FEventTransition
		{
			const FDstState* From = nullptr;
			int32 Index = 0;
			FGameplayTag Tag;
		};
		TArray<FEventTransition> EventTransitions;
		for (const FDstState* State : All)
		{
			for (int32 Index = 0; Index < State->Transitions.Num(); ++Index)
			{
				const FDstTransition& Transition = State->Transitions[Index];
				if (Transition.Trigger == EDstTransitionTrigger::OnEvent)
				{
					FEventTransition Item;
					Item.From = State;
					Item.Index = Index;
					Item.Tag = RequestTag(Transition.Event.Tag);
					EventTransitions.Add(MoveTemp(Item));
				}
			}
		}

		// Can the target be reached from outside: it is the state, a state around it or inside it.
		auto IsRelativeTarget = [](const FDstTransition& Transition)
		{
			return Transition.TargetKind != EDstTransitionTarget::GotoState && Transition.TargetKind != EDstTransitionTarget::Succeeded
				&& Transition.TargetKind != EDstTransitionTarget::Failed && Transition.TargetKind != EDstTransitionTarget::None;
		};

		for (const FDstState* State : All)
		{
			if (!State->Event.IsSet())
			{
				continue;
			}
			const FGameplayTag StateTag = RequestTag(State->Event->Tag);

			// The places a transition may aim at to get into this state: itself, and the states that link the subtree it is in.
			TArray<FString> Anchors;
			Anchors.Add(State->Path);
			TArray<FString> Pending;
			Pending.Add(RootOf(State->Path));
			TArray<FString> Visited;
			while (!Pending.IsEmpty())
			{
				const FString Root = Pending.Pop();
				if (Visited.Contains(Root))
				{
					continue;
				}
				Visited.Add(Root);
				for (const FDstState* Linker : All)
				{
					if (Linker->Type == EDstStateType::Linked && Linker->LinkTarget.Equals(Root, ESearchCase::CaseSensitive))
					{
						Anchors.AddUnique(Linker->Path);
						Pending.Add(RootOf(Linker->Path));
					}
				}
			}

			bool bReachable = false;
			for (const FEventTransition& Item : EventTransitions)
			{
				if (!TagsRelated(StateTag, Item.Tag))
				{
					continue;
				}
				const FDstTransition& Transition = Item.From->Transitions[Item.Index];
				if (IsRelativeTarget(Transition))
				{
					bReachable = true;
					break;
				}
				if (Transition.TargetKind == EDstTransitionTarget::GotoState)
				{
					for (const FString& Anchor : Anchors)
					{
						if (Related(Transition.TargetState, Anchor))
						{
							bReachable = true;
							break;
						}
					}
				}
				if (bReachable)
				{
					break;
				}
			}
			if (!bReachable)
			{
				const FDstSpan Span = Out.Places.Find(State->Path + TEXT("#on"));
				Sink.Warning(TEXT("DST6016"), Span,
					FString::Printf(TEXT("`%s` waits for the event %s, but no transition `on %s` leads into it."), *State->Name, *State->Event->Tag, *State->Event->Tag),
					TEXT("`on` is only the gate: the event does not enter the state by itself. Add `-> This on Tag` here or on a state above."));
			}
		}

		// A transition straight to a state that needs an event: the engine refuses it unless the events agree.
		for (const FEventTransition& Item : EventTransitions)
		{
			const FDstTransition& Transition = Item.From->Transitions[Item.Index];
			if (Transition.TargetKind != EDstTransitionTarget::GotoState)
			{
				continue;
			}
			const FDstState* Target = Out.Tree.FindState(Transition.TargetState);
			if (!Target || !Target->Event.IsSet())
			{
				continue;
			}
			const FGameplayTag TargetTag = RequestTag(Target->Event->Tag);
			bool bAgree = true;
			if (TargetTag.IsValid() && Item.Tag.IsValid())
			{
				bAgree = TargetTag.MatchesTag(Item.Tag) && Item.Tag.MatchesTag(TargetTag);
			}
			if (bAgree && !Target->Event->PayloadType.IsEmpty() && !Transition.Event.PayloadType.IsEmpty())
			{
				const UScriptStruct* StatePayload = FindObject<UScriptStruct>(nullptr, *Target->Event->PayloadType);
				const UScriptStruct* TransitionPayload = FindObject<UScriptStruct>(nullptr, *Transition.Event.PayloadType);
				bAgree = !StatePayload || !TransitionPayload || StatePayload->IsChildOf(TransitionPayload);
			}
			if (!bAgree)
			{
				Sink.Error(TEXT("DST6017"), Out.Places.Find(FString::Printf(TEXT("%s#%d"), *Item.From->Path, Item.Index)),
					FString::Printf(TEXT("The transition to `%s` can never succeed: its event %s and the event the state requires, %s, do not agree."),
						*Target->Name, *Transition.Event.Tag, *Target->Event->Tag),
					TEXT("A transition that goes straight to a state with `on` must send the same tag, and a payload type the state's is derived from."));
			}
		}
	}
}
