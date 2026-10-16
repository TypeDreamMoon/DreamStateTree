// Copyright DreamMoon. All Rights Reserved.
//
// The model: enum spellings, property paths and source references as text, lookups in a tree, and the canonical dump.
//
// The dump is what the corpus round trips compare, so it is built to be total and stable:
//
//  - One item per line, two spaces of indent per level, a fixed order of fields, and every field the model has.
//    A field that holds its default is left out (the default is read from a default-constructed struct, so the two
//    cannot drift apart); everything else is printed, so a model that differs anywhere dumps differently.
//    Where a field only means something for one kind of value (the constant of an argument that is a binding),
//    only the meaningful one is printed.
//  - Lists whose order means something keep it: parameters, nodes of a state, transitions, children. Maps are
//    sorted by key. The arguments of a node are a set the model already keeps ordered by (side, property path); the
//    dump sorts them the same way so a producer that orders them differently still compares equal.
//  - Free text (names, export text of values) is escaped onto one line (backslash, CR, LF, tab) and always comes
//    last on its line, so no line ends in a space and two different texts cannot print the same.
//  - State headings carry the path computed from the hierarchy, not the stored FDstState::Path (derived data; a
//    model whose paths were never filled dumps like one whose paths were). A stored path that disagrees with the
//    hierarchy is shown as a StalePath line.
//  - With FDstDumpOptions::bKeys off, node keys are not printed. A reference to a node (a binding or delegate whose
//    source is a global or a node) then names the node by its serial number in dump order, `#7`, and every node
//    prints `Id #7`: the wiring stays checkable while the spelling of the keys does not matter. References to
//    states (state parameters, state events) keep their state paths, which the State lines carry anyway.

#include "Model/DstModel.h"

namespace UE::DreamStateTree::Model
{
	// ------------------------------------------------------------------------------------------
	// Enum spellings
	// ------------------------------------------------------------------------------------------

	const TCHAR* LexToString(const EDstSourceKind Kind)
	{
		switch (Kind)
		{
		case EDstSourceKind::None: return TEXT("None");
		case EDstSourceKind::Parameters: return TEXT("Parameters");
		case EDstSourceKind::Context: return TEXT("Context");
		case EDstSourceKind::Global: return TEXT("Global");
		case EDstSourceKind::Node: return TEXT("Node");
		case EDstSourceKind::StateParameters: return TEXT("StateParameters");
		case EDstSourceKind::StateEvent: return TEXT("StateEvent");
		case EDstSourceKind::TransitionEvent: return TEXT("TransitionEvent");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(const EDstValueKind Kind)
	{
		switch (Kind)
		{
		case EDstValueKind::Constant: return TEXT("Constant");
		case EDstValueKind::Binding: return TEXT("Binding");
		case EDstValueKind::Function: return TEXT("Function");
		case EDstValueKind::Ref: return TEXT("Ref");
		case EDstValueKind::Output: return TEXT("Output");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(const EDstNodeRole Role)
	{
		switch (Role)
		{
		case EDstNodeRole::Task: return TEXT("Task");
		case EDstNodeRole::EnterCondition: return TEXT("EnterCondition");
		case EDstNodeRole::Consideration: return TEXT("Consideration");
		case EDstNodeRole::TransitionCondition: return TEXT("TransitionCondition");
		case EDstNodeRole::Evaluator: return TEXT("Evaluator");
		case EDstNodeRole::GlobalTask: return TEXT("GlobalTask");
		case EDstNodeRole::PropertyFunction: return TEXT("PropertyFunction");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(const EDstOperand Operand)
	{
		switch (Operand)
		{
		case EDstOperand::Copy: return TEXT("Copy");
		case EDstOperand::And: return TEXT("And");
		case EDstOperand::Or: return TEXT("Or");
		case EDstOperand::Multiply: return TEXT("Multiply");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(const EDstTransitionTrigger Trigger)
	{
		switch (Trigger)
		{
		case EDstTransitionTrigger::OnStateCompleted: return TEXT("OnStateCompleted");
		case EDstTransitionTrigger::OnStateSucceeded: return TEXT("OnStateSucceeded");
		case EDstTransitionTrigger::OnStateFailed: return TEXT("OnStateFailed");
		case EDstTransitionTrigger::OnTick: return TEXT("OnTick");
		case EDstTransitionTrigger::OnEvent: return TEXT("OnEvent");
		case EDstTransitionTrigger::OnDelegate: return TEXT("OnDelegate");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(const EDstTransitionTarget Target)
	{
		switch (Target)
		{
		case EDstTransitionTarget::GotoState: return TEXT("GotoState");
		case EDstTransitionTarget::Succeeded: return TEXT("Succeeded");
		case EDstTransitionTarget::Failed: return TEXT("Failed");
		case EDstTransitionTarget::Parent: return TEXT("Parent");
		case EDstTransitionTarget::NextState: return TEXT("NextState");
		case EDstTransitionTarget::NextSelectableState: return TEXT("NextSelectableState");
		case EDstTransitionTarget::NextParent: return TEXT("NextParent");
		case EDstTransitionTarget::NextSelectableParent: return TEXT("NextSelectableParent");
		case EDstTransitionTarget::None: return TEXT("None");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(const EDstStateType Type)
	{
		switch (Type)
		{
		case EDstStateType::State: return TEXT("State");
		case EDstStateType::Group: return TEXT("Group");
		case EDstStateType::Linked: return TEXT("Linked");
		case EDstStateType::LinkedAsset: return TEXT("LinkedAsset");
		case EDstStateType::Subtree: return TEXT("Subtree");
		}
		return TEXT("Unknown");
	}

	// ------------------------------------------------------------------------------------------
	// Property paths and source references
	// ------------------------------------------------------------------------------------------

	// A segment prints as `Name[Index]<InstanceType>`: the index picks the element, the type says what the element
	// holds. Names must not contain `.`, `[` or `<`; property names never do.
	FString FDstPropertyPath::ToString() const
	{
		FString Result;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FDstPathSegment& Segment = Segments[Index];
			if (Index > 0)
			{
				Result += TEXT('.');
			}
			Result += Segment.Name;
			if (Segment.ArrayIndex != INDEX_NONE)
			{
				Result += FString::Printf(TEXT("[%d]"), Segment.ArrayIndex);
			}
			if (!Segment.InstanceType.IsEmpty())
			{
				Result += TEXT('<');
				Result += Segment.InstanceType;
				Result += TEXT('>');
			}
		}
		return Result;
	}

	FDstPropertyPath FDstPropertyPath::FromString(const FString& Text)
	{
		FDstPropertyPath Result;
		const int32 Len = Text.Len();
		if (Len == 0)
		{
			return Result;
		}

		int32 Pos = 0;
		while (true)
		{
			FDstPathSegment Segment;

			const int32 NameStart = Pos;
			while (Pos < Len && Text[Pos] != TEXT('.') && Text[Pos] != TEXT('[') && Text[Pos] != TEXT('<'))
			{
				++Pos;
			}
			Segment.Name = Text.Mid(NameStart, Pos - NameStart);

			// `[n]` and `<type>`, in either order. The type may hold brackets of its own, so match them.
			while (Pos < Len && (Text[Pos] == TEXT('[') || Text[Pos] == TEXT('<')))
			{
				const int32 Open = Pos;
				if (Text[Open] == TEXT('['))
				{
					int32 Close = Open + 1;
					while (Close < Len && Text[Close] != TEXT(']'))
					{
						++Close;
					}
					Segment.ArrayIndex = FCString::Atoi(*Text.Mid(Open + 1, Close - Open - 1));
					Pos = FMath::Min(Close + 1, Len);
				}
				else
				{
					int32 Nesting = 0;
					int32 Close = Open;
					for (; Close < Len; ++Close)
					{
						if (Text[Close] == TEXT('<'))
						{
							++Nesting;
						}
						else if (Text[Close] == TEXT('>') && --Nesting == 0)
						{
							break;
						}
					}
					Segment.InstanceType = Text.Mid(Open + 1, Close - Open - 1);
					Pos = FMath::Min(Close + 1, Len);
				}
			}
			Result.Segments.Add(MoveTemp(Segment));

			if (Pos >= Len)
			{
				break;
			}
			if (Text[Pos] == TEXT('.'))
			{
				// A trailing `.` leaves one more, empty, segment: what ToString wrote for it.
				++Pos;
			}
		}
		return Result;
	}

	FString FDstSourceRef::ToString() const
	{
		FString Result = LexToString(Kind);
		if (!Owner.IsEmpty())
		{
			Result += TEXT(':');
			Result += Owner;
		}
		if (!Path.IsEmpty())
		{
			Result += TEXT(':');
			Result += Path.ToString();
		}
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// The tree
	// ------------------------------------------------------------------------------------------

	namespace
	{
		void UpdateStatePaths(TArray<FDstState>& States, const FString* ParentPath)
		{
			for (FDstState& State : States)
			{
				State.Path = ParentPath ? *ParentPath + TEXT("/") + State.Name : State.Name;
				UpdateStatePaths(State.Children, &State.Path);
			}
		}

		// Compares against the path the hierarchy gives, not the stored one, so it works before UpdatePaths. Only a
		// state whose path is a prefix of the query is entered.
		const FDstState* FindStateIn(const TArray<FDstState>& States, const FString* ParentPath, const FString& Query)
		{
			for (const FDstState& State : States)
			{
				const FString Path = ParentPath ? *ParentPath + TEXT("/") + State.Name : State.Name;
				if (Path.Equals(Query, ESearchCase::CaseSensitive))
				{
					return &State;
				}
				if (Query.Len() > Path.Len() && Query[Path.Len()] == TEXT('/') && Query.StartsWith(Path, ESearchCase::CaseSensitive))
				{
					if (const FDstState* Found = FindStateIn(State.Children, &Path, Query))
					{
						return Found;
					}
				}
			}
			return nullptr;
		}

		const FDstNode* FindNodeIn(const FDstNode& Node, const FString& Key)
		{
			if (Node.Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				return &Node;
			}
			// A property function hangs off the argument it computes.
			for (const FDstArg& Arg : Node.Args)
			{
				if (Arg.Function.IsValid())
				{
					if (const FDstNode* Found = FindNodeIn(*Arg.Function, Key))
					{
						return Found;
					}
				}
			}
			return nullptr;
		}

		const FDstNode* FindNodeIn(const TArray<FDstNode>& Nodes, const FString& Key)
		{
			for (const FDstNode& Node : Nodes)
			{
				if (const FDstNode* Found = FindNodeIn(Node, Key))
				{
					return Found;
				}
			}
			return nullptr;
		}

		const FDstNode* FindNodeIn(const FDstState& State, const FString& Key)
		{
			for (const FDstArg& Arg : State.ParameterBindings)
			{
				if (Arg.Function.IsValid())
				{
					if (const FDstNode* Found = FindNodeIn(*Arg.Function, Key))
					{
						return Found;
					}
				}
			}
			if (const FDstNode* Found = FindNodeIn(State.EnterConditions, Key))
			{
				return Found;
			}
			if (const FDstNode* Found = FindNodeIn(State.Considerations, Key))
			{
				return Found;
			}
			if (const FDstNode* Found = FindNodeIn(State.Tasks, Key))
			{
				return Found;
			}
			for (const FDstTransition& Transition : State.Transitions)
			{
				if (const FDstNode* Found = FindNodeIn(Transition.Conditions, Key))
				{
					return Found;
				}
			}
			for (const FDstState& Child : State.Children)
			{
				if (const FDstNode* Found = FindNodeIn(Child, Key))
				{
					return Found;
				}
			}
			return nullptr;
		}
	}

	void FDstTree::UpdatePaths()
	{
		UpdateStatePaths(States, nullptr);
	}

	const FDstState* FDstTree::FindState(const FString& Path) const
	{
		return FindStateIn(States, nullptr, Path);
	}

	FDstState* FDstTree::FindState(const FString& Path)
	{
		return const_cast<FDstState*>(static_cast<const FDstTree*>(this)->FindState(Path));
	}

	const FDstNode* FDstTree::FindNode(const FString& Key) const
	{
		if (const FDstNode* Found = FindNodeIn(Globals, Key))
		{
			return Found;
		}
		for (const FDstState& State : States)
		{
			if (const FDstNode* Found = FindNodeIn(State, Key))
			{
				return Found;
			}
		}
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// The dump
	// ------------------------------------------------------------------------------------------

	namespace
	{
		// FString's operator< ignores case; a canonical dump must not depend on it.
		int32 CompareText(const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::CaseSensitive);
		}

		bool LessText(const FString& A, const FString& B)
		{
			return CompareText(A, B) < 0;
		}

		/** One line however long the text is. */
		FString Escape(const FString& Text)
		{
			FString Result = Text.Replace(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
			Result.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
			Result.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
			Result.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
			return Result;
		}

		/** The prefix alone when Value is empty, so no line ends in a space; otherwise the prefix, a space and the escaped value. */
		FString WithValue(const FString& Prefix, const FString& Value)
		{
			return Value.IsEmpty() ? Prefix : Prefix + TEXT(" ") + Escape(Value);
		}

		const TCHAR* BoolText(const bool bValue)
		{
			return bValue ? TEXT("true") : TEXT("false");
		}

		const TCHAR* CompletionText(const EDstCompletion Completion)
		{
			switch (Completion)
			{
			case EDstCompletion::None: return TEXT("None");
			case EDstCompletion::Succeeds: return TEXT("Succeeds");
			case EDstCompletion::Fails: return TEXT("Fails");
			case EDstCompletion::Completes: return TEXT("Completes");
			}
			return TEXT("Unknown");
		}

		/**
		 * Key -> number by exact spelling. FString keys hash and compare without regard to case, which would merge
		 * `Root/a/Task[0]` with `Root/A/Task[0]`; the buckets keep spellings apart.
		 */
		struct FKeyIndex
		{
			TMap<FString, TArray<TPair<FString, int32>>> Buckets;

			void Set(const FString& Key, const int32 Value)
			{
				TArray<TPair<FString, int32>>& Bucket = Buckets.FindOrAdd(Key);
				for (TPair<FString, int32>& Entry : Bucket)
				{
					if (Entry.Key.Equals(Key, ESearchCase::CaseSensitive))
					{
						Entry.Value = Value;
						return;
					}
				}
				Bucket.Emplace(Key, Value);
			}

			const int32* Find(const FString& Key) const
			{
				if (const TArray<TPair<FString, int32>>* Bucket = Buckets.Find(Key))
				{
					for (const TPair<FString, int32>& Entry : *Bucket)
					{
						if (Entry.Key.Equals(Key, ESearchCase::CaseSensitive))
						{
							return &Entry.Value;
						}
					}
				}
				return nullptr;
			}
		};

		struct FDumper
		{
			const FDstDumpOptions& Options;
			FString Out;
			FKeyIndex Serials;
			int32 NextSerial = 0;

			// What a field holds when nobody set it.
			const FDstState DefaultState{};
			const FDstTransition DefaultTransition{};
			const FDstEvent DefaultEvent{};

			explicit FDumper(const FDstDumpOptions& InOptions) : Options(InOptions) {}

			void Line(const int32 Depth, const FString& Text)
			{
				for (int32 Level = 0; Level < Depth; ++Level)
				{
					Out += TEXT("  ");
				}
				Out += Text;
				Out += TEXT("\n");
			}

			void FlagLine(const int32 Depth, const TCHAR* Word, const bool bValue, const bool bDefault)
			{
				if (bValue != bDefault)
				{
					Line(Depth, FString::Printf(TEXT("%s %s"), Word, BoolText(bValue)));
				}
			}

			FString SourceText(const FDstSourceRef& Source) const
			{
				if (Options.bKeys || (Source.Kind != EDstSourceKind::Global && Source.Kind != EDstSourceKind::Node))
				{
					return Escape(Source.ToString());
				}
				FDstSourceRef Numbered = Source;
				const int32* Serial = Serials.Find(Source.Owner);
				Numbered.Owner = Serial ? FString::Printf(TEXT("#%d"), *Serial) : FString(TEXT("#?"));
				return Escape(Numbered.ToString());
			}

			void DumpMap(const int32 Depth, const TCHAR* Word, const TMap<FString, FString>& Map)
			{
				TArray<FString> Keys;
				Map.GetKeys(Keys);
				Keys.StableSort(LessText);
				for (const FString& Key : Keys)
				{
					Line(Depth, WithValue(FString::Printf(TEXT("%s %s ="), Word, *Escape(Key)), Map.FindChecked(Key)));
				}
			}

			void DumpParameter(const int32 Depth, const FDstParameter& Parameter)
			{
				FString Head = FString::Printf(TEXT("Parameter %s %s"), *Escape(Parameter.Type), *Escape(Parameter.Name));
				if (!Parameter.ValueText.IsEmpty())
				{
					Head += TEXT(" = ") + Escape(Parameter.ValueText);
				}
				Line(Depth, Head);
				if (Parameter.bOverridden)
				{
					Line(Depth + 1, TEXT("Overridden"));
				}
				DumpMap(Depth + 1, TEXT("Meta"), Parameter.Meta);
			}

			void DumpEvent(const int32 Depth, const FDstEvent& Event)
			{
				Line(Depth, FString::Printf(TEXT("Event tag=%s payload=%s consume=%s"),
					*Escape(Event.Tag), *Escape(Event.PayloadType), BoolText(Event.bConsumeOnSelect)));
			}

			/**
			 * Word is what the list the node sits in calls its members. The node's own role is printed only when it is
			 * not the one that list expects (a list of globals expects none, so it always is).
			 */
			void DumpNode(const int32 Depth, const TCHAR* Word, const TOptional<EDstNodeRole> ExpectedRole, const FDstNode& Node)
			{
				// Numbered in the order printed; the first pass of a keyless dump only fills Serials.
				const int32 Serial = NextSerial++;
				if (!Options.bKeys)
				{
					Serials.Set(Node.Key, Serial);
				}

				Line(Depth, WithValue(Word, Node.StructType));
				const int32 Inner = Depth + 1;
				if (!ExpectedRole.IsSet() || ExpectedRole.GetValue() != Node.Role)
				{
					Line(Inner, FString::Printf(TEXT("Role %s"), LexToString(Node.Role)));
				}
				if (Options.bKeys)
				{
					Line(Inner, WithValue(TEXT("Key"), Node.Key));
				}
				else
				{
					Line(Inner, FString::Printf(TEXT("Id #%d"), Serial));
				}
				if (!Node.BlueprintClass.IsEmpty())
				{
					Line(Inner, WithValue(TEXT("Blueprint"), Node.BlueprintClass));
				}
				if (!Node.BindName.IsEmpty())
				{
					Line(Inner, WithValue(TEXT("Bind"), Node.BindName));
				}
				if (Node.Operand != EDstOperand::Copy)
				{
					Line(Inner, FString::Printf(TEXT("Operand %s"), LexToString(Node.Operand)));
				}
				if (Node.Indent != 0)
				{
					Line(Inner, FString::Printf(TEXT("Indent %d"), Node.Indent));
				}

				DumpArgs(Inner, Node.Args);
			}

			/** Args sorted by (side, property path), so their order in the model does not matter. */
			void DumpArgs(const int32 Inner, const TArray<FDstArg>& Args)
			{
				TArray<const FDstArg*> Sorted;
				for (const FDstArg& Arg : Args)
				{
					Sorted.Add(&Arg);
				}
				Sorted.StableSort([](const FDstArg& A, const FDstArg& B)
				{
					if (A.Side != B.Side)
					{
						return static_cast<uint8>(A.Side) < static_cast<uint8>(B.Side);
					}
					return LessText(A.Property.ToString(), B.Property.ToString());
				});

				for (const FDstArg* Arg : Sorted)
				{
					const FString Head = FString::Printf(TEXT("Arg %s %s"),
						Arg->Side == EDstPropertySide::Instance ? TEXT("Instance") : TEXT("Node"), *Escape(Arg->Property.ToString()));
					switch (Arg->Kind)
					{
					case EDstValueKind::Constant:
						Line(Inner, WithValue(Head + TEXT(" ="), Arg->ConstantText));
						break;
					case EDstValueKind::Binding:
						Line(Inner, Head + TEXT(" <- ") + SourceText(Arg->Source));
						break;
					case EDstValueKind::Ref:
						Line(Inner, Head + TEXT(" ref ") + SourceText(Arg->Source));
						break;
					case EDstValueKind::Output:
						Line(Inner, Head + TEXT(" -> ") + SourceText(Arg->Source));
						break;
					case EDstValueKind::Function:
						Line(Inner, Head + TEXT(" fn"));
						if (Arg->Function.IsValid())
						{
							DumpNode(Inner + 1, TEXT("PropertyFunction"), EDstNodeRole::PropertyFunction, *Arg->Function);
						}
						else
						{
							Line(Inner + 1, TEXT("Missing"));
						}
						break;
					}
				}
			}

			void DumpTransition(const int32 Depth, const FDstTransition& Transition)
			{
				Line(Depth, WithValue(FString::Printf(TEXT("Transition %s -> %s"),
					LexToString(Transition.Trigger), LexToString(Transition.TargetKind)), Transition.TargetState));

				const int32 Inner = Depth + 1;
				const bool bEventDiffers = Transition.Event.Tag != DefaultEvent.Tag
					|| Transition.Event.PayloadType != DefaultEvent.PayloadType
					|| Transition.Event.bConsumeOnSelect != DefaultEvent.bConsumeOnSelect;
				if (Transition.Trigger == EDstTransitionTrigger::OnEvent || bEventDiffers)
				{
					DumpEvent(Inner, Transition.Event);
				}
				if (Transition.Delegate.IsSet())
				{
					Line(Inner, TEXT("Delegate ") + SourceText(Transition.Delegate));
				}
				if (Transition.Completion != EDstCompletion::None)
				{
					Line(Inner, FString::Printf(TEXT("Completion %s"), CompletionText(Transition.Completion)));
				}
				if (!Transition.Priority.Equals(DefaultTransition.Priority, ESearchCase::CaseSensitive))
				{
					Line(Inner, WithValue(TEXT("Priority"), Transition.Priority));
				}
				if (Transition.bDelay || !Transition.DelayText.IsEmpty() || !Transition.VarianceText.IsEmpty())
				{
					Line(Inner, FString::Printf(TEXT("Delay enabled=%s duration=%s variance=%s"),
						BoolText(Transition.bDelay), *Escape(Transition.DelayText), *Escape(Transition.VarianceText)));
				}
				FlagLine(Inner, TEXT("Reactivate"), Transition.bReactivate, DefaultTransition.bReactivate);
				FlagLine(Inner, TEXT("Enabled"), Transition.bEnabled, DefaultTransition.bEnabled);
				for (const FDstNode& Condition : Transition.Conditions)
				{
					DumpNode(Inner, TEXT("TransitionCondition"), EDstNodeRole::TransitionCondition, Condition);
				}
			}

			void DumpState(const int32 Depth, const FString* ParentPath, const FDstState& State)
			{
				const FString Path = ParentPath ? *ParentPath + TEXT("/") + State.Name : State.Name;
				Line(Depth, WithValue(TEXT("State"), Path));

				const int32 Inner = Depth + 1;
				if (!State.Path.IsEmpty() && !State.Path.Equals(Path, ESearchCase::CaseSensitive))
				{
					Line(Inner, WithValue(TEXT("StalePath"), State.Path));
				}
				Line(Inner, FString::Printf(TEXT("Type %s"), LexToString(State.Type)));
				Line(Inner, WithValue(TEXT("Selection"), State.Selection));
				Line(Inner, WithValue(TEXT("Complete"), State.TasksCompletion));
				if (!State.Tag.IsEmpty())
				{
					Line(Inner, WithValue(TEXT("Tag"), State.Tag));
				}
				if (!State.ColorName.IsEmpty())
				{
					Line(Inner, WithValue(TEXT("Color"), State.ColorName));
				}
				if (!State.Description.IsEmpty())
				{
					Line(Inner, WithValue(TEXT("Description"), State.Description));
				}
				FlagLine(Inner, TEXT("Enabled"), State.bEnabled, DefaultState.bEnabled);
				if (!State.WeightText.IsEmpty())
				{
					Line(Inner, WithValue(TEXT("Weight"), State.WeightText));
				}
				if (State.bCustomTickRate || !State.TickRateText.IsEmpty())
				{
					Line(Inner, FString::Printf(TEXT("TickRate enabled=%s value=%s"), BoolText(State.bCustomTickRate), *Escape(State.TickRateText)));
				}
				FlagLine(Inner, TEXT("CopyParameterBindingsOnTick"), State.bCopyParameterBindingsOnTick, DefaultState.bCopyParameterBindingsOnTick);
				FlagLine(Inner, TEXT("CheckPrerequisitesWhenActivatingChildDirectly"),
					State.bCheckPrerequisitesWhenActivatingChildDirectly, DefaultState.bCheckPrerequisitesWhenActivatingChildDirectly);
				FlagLine(Inner, TEXT("FixedLayout"), State.bFixedLayout, DefaultState.bFixedLayout);
				if (!State.LinkTarget.IsEmpty())
				{
					Line(Inner, WithValue(TEXT("Link"), State.LinkTarget));
				}
				if (State.Event.IsSet())
				{
					DumpEvent(Inner, State.Event.GetValue());
				}

				for (const FDstParameter& Parameter : State.Parameters)
				{
					DumpParameter(Inner, Parameter);
				}
				if (!State.ParameterBindings.IsEmpty())
				{
					Line(Inner, TEXT("ParameterBindings"));
					DumpArgs(Inner + 1, State.ParameterBindings);
				}
				for (const FDstNode& Node : State.EnterConditions)
				{
					DumpNode(Inner, TEXT("EnterCondition"), EDstNodeRole::EnterCondition, Node);
				}
				for (const FDstNode& Node : State.Considerations)
				{
					DumpNode(Inner, TEXT("Consideration"), EDstNodeRole::Consideration, Node);
				}
				for (const FDstNode& Node : State.Tasks)
				{
					DumpNode(Inner, TEXT("Task"), EDstNodeRole::Task, Node);
				}
				for (const FDstTransition& Transition : State.Transitions)
				{
					DumpTransition(Inner, Transition);
				}
				for (const FDstState& Child : State.Children)
				{
					DumpState(Inner, &Path, Child);
				}
			}

			void Run(const FDstTree& Tree)
			{
				Out.Reset();
				NextSerial = 0;

				Line(0, WithValue(TEXT("Tree"), Tree.AssetPath));
				Line(1, WithValue(TEXT("Schema"), Tree.SchemaClass));
				DumpMap(1, TEXT("SchemaProperty"), Tree.SchemaProperties);
				DumpMap(1, TEXT("Setting"), Tree.Settings);
				for (const FDstParameter& Parameter : Tree.Parameters)
				{
					DumpParameter(1, Parameter);
				}
				for (const FDstNode& Node : Tree.Globals)
				{
					DumpNode(1, TEXT("Global"), TOptional<EDstNodeRole>(), Node);
				}
				for (const FDstColor& Color : Tree.Colors)
				{
					Line(1, WithValue(FString::Printf(TEXT("Color %s ="), *Escape(Color.Name)), Color.ColorText));
				}
				for (const FDstState& State : Tree.States)
				{
					DumpState(1, nullptr, State);
				}
			}
		};
	}

	FString DumpDstModel(const FDstTree& Tree, const FDstDumpOptions& Options)
	{
		FDumper Dumper(Options);
		if (!Options.bKeys)
		{
			// The first pass numbers the nodes, so a reference to a node further down can be printed in the second.
			Dumper.Run(Tree);
		}
		Dumper.Run(Tree);
		return MoveTemp(Dumper.Out);
	}
}
