// Copyright DreamMoon. All Rights Reserved.
//
// What a path in the text means. A path starts with a root (`Params`, a context, a global node, a named task, `Event`,
// `State.Event`) and walks properties from there. Which roots a place can read follows the engine's own rules
// (UStateTreeEditorData::GetAccessibleStructsInExecutionPath): globals, the tree parameters, the parameters and events
// of the states on the way down from the subtree's root, the tasks of those states (never for conditions, utility or
// state parameters), and the event of the transition whose conditions these are.

#include "DstLowerPrivate.h"

#include "PropertyBindingTypes.h"
#include "StateTreeAnyEnum.h"
#include "StateTreeDelegate.h"
#include "StateTreeEvents.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeSchema.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	using namespace Lang;
	using namespace Model;
	using Model::FDstNode;

	namespace
	{
		FString JoinPath(const TArray<FString>& Segments)
		{
			return FString::Join(Segments, TEXT("."));
		}

		TArray<FString> NamesOfProperties(const UStruct* Struct)
		{
			TArray<FString> Names;
			if (Struct)
			{
				for (TFieldIterator<FProperty> It(Struct); It; ++It)
				{
					if (!It->HasAnyPropertyFlags(CPF_Deprecated))
					{
						Names.Add(It->GetName());
					}
				}
			}
			return Names;
		}

		EDstCompletion CompletionFromWord(const FString& Word)
		{
			for (const Sugar::FWord& Entry : Sugar::CompletionWords)
			{
				if (Word.Equals(Entry.Written, ESearchCase::CaseSensitive))
				{
					if (Word.Equals(TEXT("Succeeds"), ESearchCase::CaseSensitive)) { return EDstCompletion::Succeeds; }
					if (Word.Equals(TEXT("Fails"), ESearchCase::CaseSensitive)) { return EDstCompletion::Fails; }
					return EDstCompletion::Completes;
				}
			}
			return EDstCompletion::None;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Roots
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::IsBindingRoot(const FString& Name, const TArray<FString>& Segments) const
	{
		if (Name.Equals(TEXT("Params"), ESearchCase::CaseSensitive) || Name.Equals(TEXT("Event"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		for (const FContext& Context : Contexts)
		{
			if (Context.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		for (const FBindableNode& Node : GlobalNodes)
		{
			if (Node.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		for (const FString& TaskName : KnownTaskNames)
		{
			if (TaskName.Equals(Name, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		if (Segments.Num() >= 2 && Segments[1].Equals(TEXT("Event"), ESearchCase::CaseSensitive))
		{
			for (const FStateFrame* Frame : Frames)
			{
				if (Frame && Frame->Name.Equals(Name, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool FDstLowerer::IsBindingPath(const FDstExpr& Expr, const FBindScope& Scope) const
	{
		TArray<FString> Segments;
		return TryGetDstPath(Expr, Segments) && !Segments.IsEmpty() && IsBindingRoot(Segments[0], Segments);
	}

	const FBindableNode* FDstLowerer::FindNodeRoot(const FString& Name, const FBindScope& Scope, const FDstSpan& Span, const bool bReport, bool* bOutKnown)
	{
		if (bOutKnown)
		{
			*bOutKnown = false;
		}
		// Global nodes: every one before the node being lowered (all of them for anything else).
		for (int32 Index = 0; Index < GlobalNodes.Num(); ++Index)
		{
			const FBindableNode& Node = GlobalNodes[Index];
			if (!Node.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (bOutKnown)
			{
				*bOutKnown = true;
			}
			if (Index >= Scope.GlobalLimit || !Node.bLowered)
			{
				if (bReport)
				{
					Sink.Error(TEXT("DST5017"), Span, FString::Printf(TEXT("`%s` cannot be read here: it comes later in Globals."), *Name),
						TEXT("A global node reads the nodes before it; evaluators come before global tasks."));
				}
				return nullptr;
			}
			return &Node;
		}

		// Named tasks of the states on the way down.
		for (int32 FrameIndex = Frames.Num() - 1; FrameIndex >= 0; --FrameIndex)
		{
			const FStateFrame* Frame = Frames[FrameIndex];
			for (const FBindableNode& Node : Frame->Tasks)
			{
				if (!Node.Name.Equals(Name, ESearchCase::CaseSensitive))
				{
					continue;
				}
				if (bOutKnown)
				{
					*bOutKnown = true;
				}
				if (!Scope.AcceptsTaskOutputs())
				{
					if (bReport)
					{
						Sink.Error(TEXT("DST5014"), Span,
							FString::Printf(TEXT("`%s` is a task: conditions, utility scores and state parameters cannot read a task's output."), *Name),
							TEXT("The engine builds them before the task's data exists. Read it in a task's argument, or bind a parameter with `->`."));
					}
					return nullptr;
				}
				if (!Node.bLowered || Scope.Target == EBindTarget::Global)
				{
					if (bReport)
					{
						Sink.Error(TEXT("DST5017"), Span, FString::Printf(TEXT("`%s` cannot be read here: it comes later in the state."), *Name),
							TEXT("A task reads the named tasks of the states above it and the tasks before it in its own state."));
					}
					return nullptr;
				}
				return &Node;
			}
		}

		// A task name that is written somewhere else in the tree.
		for (const FString& TaskName : KnownTaskNames)
		{
			if (TaskName.Equals(Name, ESearchCase::CaseSensitive))
			{
				if (bOutKnown)
				{
					*bOutKnown = true;
				}
				if (bReport)
				{
					if (!Scope.AcceptsTaskOutputs())
					{
						Sink.Error(TEXT("DST5014"), Span,
							FString::Printf(TEXT("`%s` is a task: conditions, utility scores and state parameters cannot read a task's output."), *Name));
					}
					else
					{
						Sink.Error(TEXT("DST5017"), Span, FString::Printf(TEXT("`%s` is a task of another state and cannot be read here."), *Name),
							TEXT("Only the tasks of the states above, and the earlier tasks of the same state, are readable."));
					}
				}
				return nullptr;
			}
		}
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Walking properties
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::WalkPath(const FProperty* Start, const TArray<FString>& Segments, const int32 FirstIndex, const FDstSpan& Span,
		const FString& OwnerDisplay, FDstPropertyPath& InOutPath, const FProperty*& OutLeaf)
	{
		const FProperty* Current = Start;
		for (int32 Index = FirstIndex; Index < Segments.Num(); ++Index)
		{
			const UStruct* Inner = nullptr;
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Current))
			{
				Inner = StructProperty->Struct;
			}
			else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Current))
			{
				Inner = ObjectProperty->PropertyClass;
			}
			if (!Inner)
			{
				Sink.Error(TEXT("DST5002"), Span,
					FString::Printf(TEXT("`%s` (%s) has no members, so `%s` cannot be read from it."), *Current->GetName(), *DescribeProperty(Current), *Segments[Index]));
				return false;
			}
			const FProperty* Next = Inner->FindPropertyByName(FName(*Segments[Index]));
			if (!Next)
			{
				Sink.Error(TEXT("DST5002"), Span,
					FString::Printf(TEXT("`%s` has no property `%s`."), *Current->GetName(), *Segments[Index]),
					MakeSuggestionHint(Segments[Index], NamesOfProperties(Inner)));
				return false;
			}
			FDstPathSegment Segment;
			Segment.Name = Next->GetName();
			InOutPath.Segments.Add(MoveTemp(Segment));
			Current = Next;
		}
		OutLeaf = Current;
		return true;
	}

	bool FDstLowerer::ResolveInStruct(const UStruct* Root, const TArray<FString>& Segments, const int32 FirstIndex, const FDstSpan& Span,
		const FString& OwnerDisplay, FResolvedSource& OutSource)
	{
		if (!Root)
		{
			Sink.Error(TEXT("DST5002"), Span, FString::Printf(TEXT("`%s` has no data to read."), *OwnerDisplay));
			return false;
		}
		const FProperty* First = Root->FindPropertyByName(FName(*Segments[FirstIndex]));
		if (!First)
		{
			Sink.Error(TEXT("DST5002"), Span,
				FString::Printf(TEXT("`%s` has no property `%s`."), *OwnerDisplay, *Segments[FirstIndex]),
				MakeSuggestionHint(Segments[FirstIndex], NamesOfProperties(Root)));
			return false;
		}
		FDstPathSegment Segment;
		Segment.Name = First->GetName();
		OutSource.Ref.Path.Segments.Add(MoveTemp(Segment));
		OutSource.RootProperty = First;
		return WalkPath(First, Segments, FirstIndex + 1, Span, OwnerDisplay, OutSource.Ref.Path, OutSource.Leaf);
	}

	// ------------------------------------------------------------------------------------------
	// Events
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::ResolveEvent(const FDstExpr& Expr, const TArray<FString>& Segments, const int32 EventIndex, const FBindScope& Scope,
		const FStateFrame* ExplicitFrame, FResolvedSource& OutSource)
	{
		const FEventInfo* Info = nullptr;
		if (ExplicitFrame)
		{
			const bool bSelf = !Frames.IsEmpty() && Frames.Last() == ExplicitFrame;
			if (!(bSelf && Scope.Target == EBindTarget::LinkParameter))
			{
				Info = &ExplicitFrame->Event;
			}
		}
		else if (Scope.TransitionEvent && Scope.TransitionEvent->bValid)
		{
			Info = Scope.TransitionEvent;
		}
		else
		{
			for (int32 Index = Frames.Num() - 1; Index >= 0; --Index)
			{
				// The arguments of `link` are evaluated before the state's own event exists.
				if (Index == Frames.Num() - 1 && Scope.Target == EBindTarget::LinkParameter)
				{
					continue;
				}
				if (Frames[Index]->Event.bValid)
				{
					Info = &Frames[Index]->Event;
					break;
				}
			}
		}

		if (!Info || !Info->bValid)
		{
			Sink.Error(TEXT("DST5007"), Expr.Span,
				FString::Printf(TEXT("There is no event to read for `%s`."), *JoinPath(Segments)),
				TEXT("`Event.X` reads the event of the nearest state above that has `on Tag`, or of the event transition whose condition this is. A subtree does not see the event of the state that links it."));
			return false;
		}

		if (Segments.Num() <= EventIndex + 1)
		{
			Sink.Error(TEXT("DST5002"), Expr.Span, TEXT("`Event` needs a member: Event.Tag, or a field of the payload."));
			return false;
		}

		OutSource.Ref.Kind = Info->Kind;
		OutSource.Ref.Owner = Info->Owner;
		OutSource.Origin = Info->Kind == EDstSourceKind::TransitionEvent ? EBindOrigin::TransitionEvent : EBindOrigin::StateEvent;

		const FString& Member = Segments[EventIndex + 1];
		if (Member.Equals(TEXT("Tag"), ESearchCase::CaseSensitive) || Member.Equals(TEXT("Origin"), ESearchCase::CaseSensitive))
		{
			if (Segments.Num() > EventIndex + 2)
			{
				Sink.Error(TEXT("DST5002"), Expr.Span, FString::Printf(TEXT("`Event.%s` has no members."), *Member));
				return false;
			}
			const FProperty* Property = FStateTreeEvent::StaticStruct()->FindPropertyByName(FName(*Member));
			FDstPathSegment Segment;
			Segment.Name = Member;
			OutSource.Ref.Path.Segments.Add(MoveTemp(Segment));
			OutSource.RootProperty = Property;
			OutSource.Leaf = Property;
			return true;
		}

		if (!Info->Payload)
		{
			Sink.Error(TEXT("DST5002"), Expr.Span,
				FString::Printf(TEXT("The event %s carries no payload type, so `Event.%s` cannot be read."), *Info->Tag.ToString(), *Member),
				TEXT("Name the payload type: `on Tag(PayloadType)`, or let your extension give the tag a default one."));
			return false;
		}

		FDstPathSegment PayloadSegment;
		PayloadSegment.Name = TEXT("Payload");
		PayloadSegment.InstanceType = Info->Payload->GetPathName();
		OutSource.Ref.Path.Segments.Add(MoveTemp(PayloadSegment));
		return ResolveInStruct(Info->Payload, Segments, EventIndex + 1, Expr.Span, FString::Printf(TEXT("the payload %s"), *Info->Payload->GetName()), OutSource);
	}

	// ------------------------------------------------------------------------------------------
	// Paths
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::ResolvePath(const FDstExpr& Expr, const FBindScope& Scope, FResolvedSource& OutSource)
	{
		OutSource = FResolvedSource();
		TArray<FString> Segments;
		if (!TryGetDstPath(Expr, Segments) || Segments.IsEmpty())
		{
			Sink.Error(TEXT("DST5001"), Expr.Span, TEXT("Expected a path such as `Params.Speed` or `Info.Target`."));
			return false;
		}
		const FString& Root = Segments[0];
		OutSource.Display = JoinPath(Segments);

		// Tree and subtree parameters.
		if (Root.Equals(TEXT("Params"), ESearchCase::CaseSensitive))
		{
			if (Segments.Num() < 2)
			{
				Sink.Error(TEXT("DST5002"), Expr.Span, TEXT("`Params` needs the parameter's name: Params.Speed."));
				return false;
			}
			const FString& Name = Segments[1];
			const FParamSet* SubtreeParams = nullptr;
			FString SubtreePath;
			if (!Frames.IsEmpty() && Frames[0]->bSubtreeRoot && Frames[0]->Params)
			{
				SubtreeParams = Frames[0]->Params;
				SubtreePath = Frames[0]->Path;
			}
			const bool bInSubtree = SubtreeParams && SubtreeParams->Find(Name);
			const bool bInTree = TreeParams.Find(Name) != nullptr;
			if (bInSubtree && bInTree)
			{
				Sink.Error(TEXT("DST5001"), Expr.Span, FString::Printf(TEXT("`Params.%s` is both a parameter of the subtree and of the tree."), *Name),
					TEXT("Rename one of them."));
				return false;
			}
			if (!bInSubtree && !bInTree)
			{
				TArray<FString> Names = TreeParams.GetNames();
				if (SubtreeParams)
				{
					Names.Append(SubtreeParams->GetNames());
				}
				Sink.Error(TEXT("DST5002"), Expr.Span, FString::Printf(TEXT("There is no parameter called `%s`."), *Name), MakeSuggestionHint(Name, Names));
				return false;
			}
			const FParamSet& Set = bInSubtree ? *SubtreeParams : TreeParams;
			const FProperty* Property = Set.FindProperty(Name);
			if (!Property)
			{
				Sink.Error(TEXT("DST5002"), Expr.Span, FString::Printf(TEXT("The parameter `%s` has no usable type."), *Name));
				return false;
			}
			OutSource.Ref.Kind = bInSubtree ? EDstSourceKind::StateParameters : EDstSourceKind::Parameters;
			OutSource.Ref.Owner = bInSubtree ? SubtreePath : FString();
			OutSource.Origin = bInSubtree ? EBindOrigin::StateParameter : EBindOrigin::TreeParameter;
			FDstPathSegment Segment;
			Segment.Name = Property->GetName();
			OutSource.Ref.Path.Segments.Add(MoveTemp(Segment));
			OutSource.RootProperty = Property;
			return WalkPath(Property, Segments, 2, Expr.Span, Name, OutSource.Ref.Path, OutSource.Leaf);
		}

		// The event of the nearest state, or of the transition.
		if (Root.Equals(TEXT("Event"), ESearchCase::CaseSensitive))
		{
			return ResolveEvent(Expr, Segments, 0, Scope, nullptr, OutSource);
		}

		// `State.Event.X`
		if (Segments.Num() >= 2 && Segments[1].Equals(TEXT("Event"), ESearchCase::CaseSensitive))
		{
			for (const FStateFrame* Frame : Frames)
			{
				if (Frame && Frame->Name.Equals(Root, ESearchCase::CaseSensitive))
				{
					return ResolveEvent(Expr, Segments, 1, Scope, Frame, OutSource);
				}
			}
		}

		// The schema's contexts.
		for (const FContext& Context : Contexts)
		{
			if (!Context.Name.Equals(Root, ESearchCase::CaseSensitive))
			{
				continue;
			}
			OutSource.Ref.Kind = EDstSourceKind::Context;
			OutSource.Ref.Owner = Context.Name;
			OutSource.Origin = EBindOrigin::Context;
			if (Segments.Num() == 1)
			{
				OutSource.RootStruct = Context.Struct;
				return true;
			}
			return ResolveInStruct(Context.Struct, Segments, 1, Expr.Span, Context.Name, OutSource);
		}

		// Global nodes and named tasks.
		bool bKnownNode = false;
		if (const FBindableNode* Node = FindNodeRoot(Root, Scope, Expr.Span, true, &bKnownNode))
		{
			if (Segments.Num() < 2)
			{
				Sink.Error(TEXT("DST5002"), Expr.Span, FString::Printf(TEXT("`%s` is a node: read one of its properties, such as `%s.Result`."), *Root, *Root));
				return false;
			}
			OutSource.Node = Node;
			OutSource.Ref.Kind = Node->Kind;
			OutSource.Ref.Owner = Node->Key;
			OutSource.Origin = Node->Kind == EDstSourceKind::Global ? EBindOrigin::Global : EBindOrigin::Task;
			return ResolveInStruct(Node->InstanceStruct, Segments, 1, Expr.Span, Root, OutSource);
		}
		if (bKnownNode)
		{
			// FindNodeRoot has said why the name cannot be read here.
			return false;
		}

		// Nothing: say what the roots are.
		TArray<FString> Roots = { TEXT("Params"), TEXT("Event") };
		for (const FContext& Context : Contexts)
		{
			Roots.Add(Context.Name);
		}
		for (const FBindableNode& Global : GlobalNodes)
		{
			Roots.Add(Global.Name);
		}
		for (const FStateFrame* Frame : Frames)
		{
			for (const FBindableNode& Task : Frame->Tasks)
			{
				if (!Task.Name.IsEmpty())
				{
					Roots.AddUnique(Task.Name);
				}
			}
		}
		FString Hint = MakeSuggestionHint(Root, Roots);
		if (Hint.IsEmpty())
		{
			Hint = FString::Printf(TEXT("A path starts with Params, Event, a context (%s), a global node or a named task."),
				Contexts.IsEmpty() ? TEXT("none in this schema") : *FString::JoinBy(Contexts, TEXT(", "), [](const FContext& Context) { return Context.Name; }));
		}
		Sink.Error(TEXT("DST5001"), Expr.Span, FString::Printf(TEXT("`%s` is not something a path can start with."), *Root), Hint);
		return false;
	}

	// ------------------------------------------------------------------------------------------
	// Compatibility
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::CheckBindable(const FResolvedSource& Source, const FProperty& Target, const FDstSpan& Span, const FString& TargetDisplay)
	{
		bool bCompatible = false;

		if (IsDelegateListenerProperty(&Target))
		{
			bCompatible = IsDelegateDispatcherProperty(Source.Leaf);
		}
		else if (!Source.Leaf)
		{
			// A whole context object.
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Target))
			{
				bCompatible = Source.RootStruct && Source.RootStruct->IsChildOf(ObjectProperty->PropertyClass);
			}
			else if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Target))
			{
				bCompatible = Source.RootStruct && Source.RootStruct->IsChildOf(StructProperty->Struct);
			}
		}
		else if (IsAnyEnumProperty(&Target))
		{
			bCompatible = GetPropertyEnum(Source.Leaf) != nullptr;
		}
		else
		{
			bCompatible = UE::PropertyBinding::GetPropertyCompatibility(Source.Leaf, &Target) != UE::PropertyBinding::EPropertyCompatibility::Incompatible;
			// A struct bound to a base struct is as good as the same struct.
			if (!bCompatible)
			{
				const FStructProperty* From = CastField<FStructProperty>(Source.Leaf);
				const FStructProperty* To = CastField<FStructProperty>(&Target);
				bCompatible = From && To && From->Struct && To->Struct && From->Struct->IsChildOf(To->Struct);
			}
		}

		if (!bCompatible)
		{
			const FString SourceType = Source.Leaf ? DescribeProperty(Source.Leaf) : (Source.RootStruct ? Source.RootStruct->GetName() : FString(TEXT("?")));
			Sink.Error(TEXT("DST5003"), Span,
				FString::Printf(TEXT("Cannot bind `%s` (%s) to %s (%s): the types are not compatible."), *Source.Display, *SourceType, *TargetDisplay, *DescribeProperty(&Target)));
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Delegates
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::ResolveDelegateSource(const FDstExpr& Expr, const FBindScope& Scope, FResolvedSource& OutSource, EDstCompletion& OutCompletion)
	{
		OutCompletion = EDstCompletion::None;
		TArray<FString> Segments;
		if (!TryGetDstPath(Expr, Segments) || Segments.Num() < 2)
		{
			Sink.Error(TEXT("DST5018"), Expr.Span, TEXT("`Delegate(...)` takes a path to what it listens to: Delegate(Combo.Succeeds) or Delegate(Info.OnTargetLost)."));
			return false;
		}

		// `Task.Succeeds`, `Task.Fails`, `Task.Completes`: the completion of a task.
		const EDstCompletion Completion = CompletionFromWord(Segments.Last());
		if (Completion != EDstCompletion::None && Segments.Num() == 2)
		{
			bool bKnownNode = false;
			const FBindableNode* Node = FindNodeRoot(Segments[0], Scope, Expr.Span, true, &bKnownNode);
			if (!Node)
			{
				if (!bKnownNode)
				{
					Sink.Error(TEXT("DST5018"), Expr.Span, FString::Printf(TEXT("`%s` is not a task, so it has no completion to listen to."), *Segments[0]));
				}
				return false;
			}
			const bool bHasProperty = Node->InstanceStruct && Node->InstanceStruct->FindPropertyByName(FName(*Segments.Last())) != nullptr;
			if (!bHasProperty)
			{
				if (!Node->bIsTask)
				{
					Sink.Error(TEXT("DST5018"), Expr.Span, FString::Printf(TEXT("`%s` is not a task: only tasks complete."), *Segments[0]));
					return false;
				}
				OutSource.Ref.Kind = Node->Kind;
				OutSource.Ref.Owner = Node->Key;
				OutSource.Node = Node;
				OutSource.Display = JoinPath(Segments);
				OutCompletion = Completion;
				return true;
			}
		}

		if (!ResolvePath(Expr, Scope, OutSource))
		{
			return false;
		}
		if (!IsDelegateDispatcherProperty(OutSource.Leaf))
		{
			Sink.Error(TEXT("DST5018"), Expr.Span,
				FString::Printf(TEXT("`%s` is not a delegate dispatcher."), *OutSource.Display),
				TEXT("Listen to a node's FStateTreeDelegateDispatcher property, or to a task's .Succeeds / .Fails / .Completes."));
			return false;
		}
		return true;
	}
}
