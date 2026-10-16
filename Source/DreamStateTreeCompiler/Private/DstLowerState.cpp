// Copyright DreamMoon. All Rights Reserved.
//
// States and their clauses: the settings in `[ ]`, `on`, `if`, `pick`, `utility`, `link`, `do` and `->`, in the order the
// engine needs them (the event first, because everything below reads it; the tasks before the transitions, because a
// transition can listen to them), and the subtrees a tree links, including the ones that live in an included library.

#include "DstLowerPrivate.h"

#include "DreamStateTreeExtension.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "Misc/PackageName.h"
#include "StateTreeTypes.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	using namespace Lang;
	using namespace Model;
	using Model::FDstNode;

	namespace
	{
		/** Whether a node setting key is one a transition's own `[ ]` does not know. */
		bool IsNodeSettingKey(const FString& Key)
		{
			return Key.Equals(TEXT("Name"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("ConsideredForCompletion"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("TransitionPriority"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("Evaluation"), ESearchCase::CaseSensitive);
		}

		bool ReadBoolSetting(const FDstMetaEntry& Entry, bool& OutValue)
		{
			if (!Entry.Value)
			{
				OutValue = true;
				return true;
			}
			if (const FDstLiteralExpr* Literal = Entry.Value->As<FDstLiteralExpr>(); Literal && Literal->LiteralKind == EDstLiteralKind::Bool)
			{
				OutValue = Literal->bBool;
				return true;
			}
			return false;
		}

		void AddUniqueName(TArray<FString>& Names, const FString& Name)
		{
			for (const FString& Existing : Names)
			{
				if (Existing.Equals(Name, ESearchCase::CaseSensitive))
				{
					return;
				}
			}
			Names.Add(Name);
		}

		/** Every name a `do` gives a task; with bStateNamesTasks (a single-task schema) also the name of each state that has one. */
		void CollectTaskNames(const FDstStateStmt& State, TArray<FString>& OutNames, const bool bStateNamesTasks)
		{
			for (const FDstStmtPtr& Item : State.Body)
			{
				if (!Item)
				{
					continue;
				}
				if (const FDstClauseStmt* Clause = Item->As<FDstClauseStmt>())
				{
					if (Clause->ClauseKind == EDstClauseKind::Do)
					{
						for (const TUniquePtr<FDstTaskItem>& Task : Clause->Tasks)
						{
							if (Task && !Task->BindName.IsEmpty())
							{
								AddUniqueName(OutNames, Task->BindName);
							}
						}
						if (bStateNamesTasks && !Clause->Tasks.IsEmpty())
						{
							AddUniqueName(OutNames, State.Name);
						}
					}
				}
				else if (const FDstStateStmt* Child = Item->As<FDstStateStmt>())
				{
					CollectTaskNames(*Child, OutNames, bStateNamesTasks);
				}
			}
		}

		void CollectStateNames(const TArray<FDstState>& List, TArray<FString>& OutNames)
		{
			for (const FDstState& State : List)
			{
				OutNames.AddUnique(State.Name);
				CollectStateNames(State.Children, OutNames);
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Top level
	// ------------------------------------------------------------------------------------------

	void FDstLowerer::CollectSubtreesAndTaskNames()
	{
		// In a schema that allows one task per state the engine names the task after its state on every compile.
		const bool bSingleTask = Schema.IsValid() && !Schema->AllowMultipleTasks();
		auto CollectFrom = [this, bSingleTask](const FDstFile& File, const FString& FilePath, const bool bLibrary)
		{
			if (!File.Document)
			{
				return;
			}
			for (const FDstStmtPtr& Member : File.Document->Members)
			{
				const FDstStateStmt* State = Member ? Member->As<FDstStateStmt>() : nullptr;
				if (!State)
				{
					continue;
				}
				CollectTaskNames(*State, KnownTaskNames, bSingleTask);
				if (State->bSubtree)
				{
					TUniquePtr<FSubtreeInfo> Info = MakeUnique<FSubtreeInfo>();
					Info->Name = State->Name;
					Info->Stmt = State;
					Info->File = FilePath;
					Info->bLibrary = bLibrary;
					Subtrees.Add(MoveTemp(Info));
				}
			}
		};

		if (Unit.Main)
		{
			CollectFrom(*Unit.Main, FString(), false);
		}
		for (const TUniquePtr<FDstFile>& Library : Unit.Libraries)
		{
			if (Library)
			{
				CollectFrom(*Library, Library->Path, true);
			}
		}
	}

	FDstLowerer::FSubtreeInfo* FDstLowerer::FindSubtree(const FString& Name, bool* bOutAmbiguous)
	{
		if (bOutAmbiguous)
		{
			*bOutAmbiguous = false;
		}
		FSubtreeInfo* Found = nullptr;
		for (const TUniquePtr<FSubtreeInfo>& Info : Subtrees)
		{
			if (!Info->Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (!Info->bLibrary)
			{
				return Info.Get();
			}
			if (Found && Found->File != Info->File)
			{
				if (bOutAmbiguous)
				{
					*bOutAmbiguous = true;
				}
				return nullptr;
			}
			if (!Found)
			{
				Found = Info.Get();
			}
		}
		return Found;
	}

	FDstLowerer::FSubtreeInfo* FDstLowerer::FindSubtreeByStmt(const FDstStateStmt& Stmt)
	{
		for (const TUniquePtr<FSubtreeInfo>& Info : Subtrees)
		{
			if (Info->Stmt == &Stmt)
			{
				return Info.Get();
			}
		}
		return nullptr;
	}

	bool FDstLowerer::EnsureSignature(FSubtreeInfo& Info)
	{
		if (Info.bSignatureBuilt)
		{
			return !Info.bSignatureFailed;
		}
		Info.bSignatureBuilt = true;

		const FString SavedFile = Sink.CurrentFile;
		Sink.CurrentFile = Info.File;

		TArray<FParamDecl> Decls;
		for (const TUniquePtr<FDstParam>& Param : Info.Stmt->Params)
		{
			if (!Param)
			{
				continue;
			}
			FParamDecl& Decl = Decls.AddDefaulted_GetRef();
			Decl.TypeName = Param->TypeName;
			Decl.TypeSpan = Param->TypeSpan;
			Decl.Name = Param->Name;
			Decl.NameSpan = Param->NameSpan;
			Decl.Default = Param->Default.Get();
			Decl.Span = Param->Span;
		}
		Info.bSignatureFailed = !LowerParameterList(Decls, Info.Signature, Info.Name + TEXT("/Param:"));

		Sink.CurrentFile = SavedFile;
		return !Info.bSignatureFailed;
	}

	bool FDstLowerer::LowerStates(const FDstDocument& Doc)
	{
		TArray<const FDstStateStmt*> Plain;
		TArray<const FDstStateStmt*> Subtree;
		for (const FDstStmtPtr& Member : Doc.Members)
		{
			if (const FDstStateStmt* State = Member ? Member->As<FDstStateStmt>() : nullptr)
			{
				(State->bSubtree ? Subtree : Plain).Add(State);
			}
		}

		if (Plain.IsEmpty())
		{
			Sink.Error(TEXT("DST6012"), Doc.KindSpan, TEXT("The tree has no root state."),
				TEXT("Write a `State Root { ... }` in the document: the first top-level State is where the tree starts."));
		}

		const int32 ErrorsBefore = Sink.Real.NumErrors();
		for (const FDstStateStmt* Stmt : Plain)
		{
			FDstState State;
			LowerStateStmt(*Stmt, FString(), State);
			Out.Tree.States.Add(MoveTemp(State));
		}
		for (const FDstStateStmt* Stmt : Subtree)
		{
			FDstState State;
			LowerStateStmt(*Stmt, FString(), State);
			Out.Tree.States.Add(MoveTemp(State));
		}
		DrainLibrarySubtrees();
		Out.Tree.UpdatePaths();
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	void FDstLowerer::DrainLibrarySubtrees()
	{
		while (!LibraryQueue.IsEmpty())
		{
			FSubtreeInfo* Info = LibraryQueue[0];
			LibraryQueue.RemoveAt(0);
			if (Info->bLowered)
			{
				continue;
			}
			Info->bLowered = true;

			const FString SavedFile = Sink.CurrentFile;
			Sink.CurrentFile = Info->File;
			FDstState State;
			LowerStateStmt(*Info->Stmt, FString(), State);
			Sink.CurrentFile = SavedFile;
			Out.Tree.States.Add(MoveTemp(State));
		}
	}

	// ------------------------------------------------------------------------------------------
	// One state
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerStateStmt(const FDstStateStmt& Stmt, const FString& ParentPath, FDstState& OutState)
	{
		const int32 ErrorsBefore = Sink.Real.NumErrors();
		const bool bTop = ParentPath.IsEmpty();

		OutState.Name = Stmt.Name;
		OutState.Path = PathOf(ParentPath, Stmt.Name);
		OutState.Type = Stmt.bSubtree ? EDstStateType::Subtree : EDstStateType::State;
		AddPlace(OutState.Path, Stmt.NameSpan);
		if (Stmt.bSubtree && !bTop)
		{
			// The parser has reported it (DST2020); lower it as an ordinary state.
			OutState.Type = EDstStateType::State;
		}

		FStateFrame Frame;
		Frame.Name = Stmt.Name;
		Frame.Path = OutState.Path;
		Frame.bSubtreeRoot = Stmt.bSubtree && bTop;
		if (Frame.bSubtreeRoot)
		{
			if (FSubtreeInfo* Info = FindSubtreeByStmt(Stmt))
			{
				if (EnsureSignature(*Info))
				{
					Frame.Params = &Info->Signature;
					OutState.Parameters = Info->Signature.Params;
				}
			}
		}

		// The clauses by kind. The text's order is kept inside each kind.
		TArray<const FDstClauseStmt*> Ons, Ifs, Picks, Utilities, Links, Dos, Transitions;
		TArray<const FDstStateStmt*> Children;
		for (const FDstStmtPtr& Item : Stmt.Body)
		{
			if (!Item)
			{
				continue;
			}
			if (const FDstStateStmt* Child = Item->As<FDstStateStmt>())
			{
				Children.Add(Child);
				continue;
			}
			const FDstClauseStmt* Clause = Item->As<FDstClauseStmt>();
			if (!Clause)
			{
				continue;
			}
			switch (Clause->ClauseKind)
			{
			case EDstClauseKind::On: Ons.Add(Clause); break;
			case EDstClauseKind::If: Ifs.Add(Clause); break;
			case EDstClauseKind::Pick: Picks.Add(Clause); break;
			case EDstClauseKind::Utility: Utilities.Add(Clause); break;
			case EDstClauseKind::Link: Links.Add(Clause); break;
			case EDstClauseKind::Do: Dos.Add(Clause); break;
			case EDstClauseKind::Transition: Transitions.Add(Clause); break;
			}
		}

		auto RejectExtra = [this](const TArray<const FDstClauseStmt*>& List, const TCHAR* Word)
		{
			for (int32 Index = 1; Index < List.Num(); ++Index)
			{
				Sink.Error(TEXT("DST6005"), List[Index]->Span, FString::Printf(TEXT("A state has at most one `%s` clause."), Word),
					TEXT("Put everything in the first one."));
			}
		};
		RejectExtra(Ons, TEXT("on"));
		RejectExtra(Picks, TEXT("pick"));
		RejectExtra(Utilities, TEXT("utility"));
		RejectExtra(Links, TEXT("link"));

		Frames.Add(&Frame);

		LowerStateSettings(Stmt, OutState, Frame);

		if (!Ons.IsEmpty())
		{
			LowerEventClause(*Ons[0], OutState, Frame);
		}
		else if (Frame.PendingConsumeEvent.IsSet())
		{
			Sink.Error(TEXT("DST4014"), Stmt.NameSpan, TEXT("`ConsumeEvent` is a setting of the state's `on` event, and the state has none."));
		}

		if (!Picks.IsEmpty())
		{
			LowerPickClause(*Picks[0], !Children.IsEmpty(), OutState);
		}

		// After `pick`: a link makes the engine select the linked tree itself (SetLinkedState), whatever was picked.
		if (!Links.IsEmpty())
		{
			LowerLinkClause(*Links[0], OutState, Frame);
		}

		// Kind checks.
		const bool bHasTasks = !Dos.IsEmpty();
		if (OutState.Type == EDstStateType::Group)
		{
			if (bHasTasks)
			{
				Sink.Error(TEXT("DST6003"), Dos[0]->Span, FString::Printf(TEXT("`%s` is a Group: it holds states, not tasks."), *Stmt.Name),
					TEXT("Remove [ Group ], or move the tasks into a child state."));
			}
		}
		if (OutState.Type == EDstStateType::Linked || OutState.Type == EDstStateType::LinkedAsset)
		{
			if (bHasTasks)
			{
				Sink.Error(TEXT("DST6004"), Dos[0]->Span, FString::Printf(TEXT("`%s` links another tree, so it runs that tree's tasks, not its own."), *Stmt.Name),
					TEXT("Move the tasks into the subtree, or into a state beside this one."));
			}
			if (!Children.IsEmpty())
			{
				Sink.Error(TEXT("DST6004"), Children[0]->Span, FString::Printf(TEXT("`%s` links another tree, so it cannot have child states."), *Stmt.Name),
					TEXT("Move the children out, or remove the link."));
			}
		}
		if (Schema.IsValid() && !Schema->IsStateTypeAllowed(static_cast<EStateTreeStateType>(OutState.Type)))
		{
			Sink.Error(TEXT("DST4015"), Stmt.NameSpan, FString::Printf(TEXT("The schema does not allow a %s state."), LexToString(OutState.Type)));
		}

		// Enter conditions: the `if` clauses, joined with `and`.
		if (!Ifs.IsEmpty())
		{
			if (Schema.IsValid() && !Schema->AllowEnterConditions())
			{
				Sink.Error(TEXT("DST4015"), Ifs[0]->Span, TEXT("The schema does not allow enter conditions."),
					TEXT("Put the condition on the transition that enters the state, or use another schema."));
			}
			TArray<const FDstExpr*> Expressions;
			for (const FDstClauseStmt* Clause : Ifs)
			{
				if (Clause->Value)
				{
					Expressions.Add(Clause->Value.Get());
				}
			}
			FBindScope Scope;
			Scope.Target = EBindTarget::EnterCondition;
			LowerConditionList(Expressions, OutState.Path + TEXT("/EnterCondition"), EDstNodeRole::EnterCondition, Scope, OutState.EnterConditions);
		}

		if (!Utilities.IsEmpty() && Utilities[0]->Value)
		{
			if (Schema.IsValid() && !Schema->AllowUtilityConsiderations())
			{
				Sink.Error(TEXT("DST4015"), Utilities[0]->Span, TEXT("The schema does not allow utility considerations."));
			}
			FBindScope Scope;
			Scope.Target = EBindTarget::Consideration;
			LowerConsiderationList(*Utilities[0]->Value, OutState.Path + TEXT("/Consideration"), Scope, OutState.Considerations);
		}

		LowerTasks(Dos, OutState, Frame);

		if (OutState.Type == EDstStateType::Linked || OutState.Type == EDstStateType::LinkedAsset)
		{
			// Link arguments are lowered with the rest of the link, above.
		}

		for (int32 Index = 0; Index < Transitions.Num(); ++Index)
		{
			FDstTransition Transition;
			LowerTransition(*Transitions[Index], Index, OutState, Frame, Transition);
			OutState.Transitions.Add(MoveTemp(Transition));
		}

		for (const FDstStateStmt* Child : Children)
		{
			FDstState ChildState;
			LowerStateStmt(*Child, OutState.Path, ChildState);
			OutState.Children.Add(MoveTemp(ChildState));
		}

		Frames.Pop();
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	// ------------------------------------------------------------------------------------------
	// Settings
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerStateSettings(const FDstStateStmt& Stmt, FDstState& State, FStateFrame& Frame)
	{
		if (!Stmt.Meta)
		{
			return true;
		}
		bool bOk = true;
		const UStateTreeState* StateDefaults = GetDefault<UStateTreeState>();
		const UClass* StateClass = UStateTreeState::StaticClass();

		for (const TUniquePtr<FDstMetaEntry>& Entry : Stmt.Meta->Entries)
		{
			if (!Entry)
			{
				continue;
			}
			const FString& Key = Entry->Key;
			auto BadValue = [this, &Entry, &bOk](const TCHAR* Message)
			{
				Sink.Error(TEXT("DST4008"), Entry->Value ? Entry->Value->Span : Entry->Span, Message);
				bOk = false;
			};
			auto NumberText = [this, &Entry, StateClass, StateDefaults](const TCHAR* PropertyName, FString& OutText) -> bool
			{
				const FProperty* Property = StateClass->FindPropertyByName(PropertyName);
				FString Error;
				if (!Entry->Value || !Property || !DstLiteralToExportText(*Entry->Value, *Property, ValueContext, OutText, Error))
				{
					Sink.Error(TEXT("DST4008"), Entry->Value ? Entry->Value->Span : Entry->Span, Error.IsEmpty() ? FString(TEXT("A number is expected.")) : Error);
					return false;
				}
				return true;
			};

			if (Key.Equals(TEXT("Tag"), ESearchCase::CaseSensitive))
			{
				FGameplayTag Tag;
				if (!Entry->Value || !ResolveTag(*Entry->Value, Tag))
				{
					bOk = false;
					continue;
				}
				State.Tag = Tag.ToString();
			}
			else if (Key.Equals(TEXT("Color"), ESearchCase::CaseSensitive))
			{
				const FString ColorName = Entry->Value ? GetDstPathString(*Entry->Value) : FString();
				if (ColorName.IsEmpty() || !ColorNames.Contains(ColorName))
				{
					Sink.Error(TEXT("DST4020"), Entry->Value ? Entry->Value->Span : Entry->Span,
						FString::Printf(TEXT("There is no color called `%s`."), *ColorName),
						ColorNames.IsEmpty() ? FString(TEXT("Declare colors in a `Colors = { Name = (R, G, B); }` section.")) : MakeSuggestionHint(ColorName, ColorNames));
					bOk = false;
					continue;
				}
				State.ColorName = ColorName;
			}
			else if (Key.Equals(TEXT("Description"), ESearchCase::CaseSensitive))
			{
				const FDstLiteralExpr* Literal = Entry->Value ? Entry->Value->As<FDstLiteralExpr>() : nullptr;
				if (!Literal || Literal->LiteralKind != EDstLiteralKind::String)
				{
					BadValue(TEXT("A description is a string."));
					continue;
				}
				State.Description = Literal->Text;
			}
			else if (Key.Equals(TEXT("Enabled"), ESearchCase::CaseSensitive))
			{
				bool bValue = true;
				if (!ReadBoolSetting(*Entry, bValue))
				{
					BadValue(TEXT("`Enabled` is true or false."));
					continue;
				}
				State.bEnabled = bValue;
			}
			else if (Key.Equals(TEXT("Complete"), ESearchCase::CaseSensitive))
			{
				const FString Word = Entry->Value ? GetDstPathString(*Entry->Value) : FString();
				if (!Word.Equals(TEXT("Any"), ESearchCase::CaseSensitive) && !Word.Equals(TEXT("All"), ESearchCase::CaseSensitive))
				{
					BadValue(TEXT("`Complete` is Any or All."));
					continue;
				}
				if (Schema.IsValid() && !Schema->AllowTasksCompletion() && !Word.Equals(TEXT("Any"), ESearchCase::CaseSensitive))
				{
					Sink.Error(TEXT("DST4015"), Entry->Span, TEXT("The schema does not let a state change how its tasks complete."),
						TEXT("Leave `Complete` out: it is always Any."));
					bOk = false;
					continue;
				}
				State.TasksCompletion = Word;
			}
			else if (Key.Equals(TEXT("TickRate"), ESearchCase::CaseSensitive))
			{
				FString Text;
				if (!NumberText(TEXT("CustomTickRate"), Text))
				{
					bOk = false;
					continue;
				}
				State.bCustomTickRate = true;
				State.TickRateText = Text;
			}
			else if (Key.Equals(TEXT("Weight"), ESearchCase::CaseSensitive))
			{
				FString Text;
				if (!NumberText(TEXT("Weight"), Text))
				{
					bOk = false;
					continue;
				}
				const FProperty* Property = StateClass->FindPropertyByName(TEXT("Weight"));
				if (!Property || Text != DstExportDefault(*Property, StateDefaults))
				{
					// A zero exports as an empty text, which the model reads as "the default": say 0 outright (as the extractor does).
					State.WeightText = Text.IsEmpty() ? FString(TEXT("0")) : Text;
				}
			}
			else if (Key.Equals(TEXT("Group"), ESearchCase::CaseSensitive))
			{
				bool bValue = true;
				if (!ReadBoolSetting(*Entry, bValue))
				{
					BadValue(TEXT("`Group` is a flag, or true / false."));
					continue;
				}
				if (bValue && State.Type == EDstStateType::State)
				{
					State.Type = EDstStateType::Group;
				}
			}
			else if (Key.Equals(TEXT("ConsumeEvent"), ESearchCase::CaseSensitive))
			{
				bool bValue = true;
				if (!ReadBoolSetting(*Entry, bValue))
				{
					BadValue(TEXT("`ConsumeEvent` is true or false."));
					continue;
				}
				Frame.PendingConsumeEvent = bValue;
			}
			else if (Key.Equals(TEXT("bCopyParameterBindingsOnTick"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("CopyParameterBindingsOnTick"), ESearchCase::CaseSensitive))
			{
				bool bValue = true;
				if (!ReadBoolSetting(*Entry, bValue))
				{
					BadValue(TEXT("This setting is true or false."));
					continue;
				}
				State.bCopyParameterBindingsOnTick = bValue;
			}
			else if (Key.Equals(TEXT("bCheckPrerequisitesWhenActivatingChildDirectly"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("CheckPrerequisitesWhenActivatingChildDirectly"), ESearchCase::CaseSensitive))
			{
				bool bValue = true;
				if (!ReadBoolSetting(*Entry, bValue))
				{
					BadValue(TEXT("This setting is true or false."));
					continue;
				}
				State.bCheckPrerequisitesWhenActivatingChildDirectly = bValue;
			}
			else if (Key.Equals(TEXT("bFixedLayout"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("FixedLayout"), ESearchCase::CaseSensitive))
			{
				bool bValue = true;
				if (!ReadBoolSetting(*Entry, bValue))
				{
					BadValue(TEXT("This setting is true or false."));
					continue;
				}
				State.bFixedLayout = bValue;
			}
			else
			{
				static const TCHAR* const Keys[] = {
					TEXT("Tag"), TEXT("Color"), TEXT("Description"), TEXT("Enabled"), TEXT("Complete"), TEXT("TickRate"), TEXT("Weight"), TEXT("Group"),
					TEXT("ConsumeEvent"), TEXT("bCopyParameterBindingsOnTick"), TEXT("bCheckPrerequisitesWhenActivatingChildDirectly"), TEXT("FixedLayout"),
				};
				TArray<FString> Names;
				for (const TCHAR* Name : Keys)
				{
					Names.Add(Name);
				}
				Sink.Error(TEXT("DST4014"), Entry->KeySpan, FString::Printf(TEXT("`%s` is not a state setting."), *Key), MakeSuggestionHint(Key, Names));
				bOk = false;
			}
		}
		return bOk;
	}

	// ------------------------------------------------------------------------------------------
	// `on`
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::ResolveTag(const FDstExpr& Expr, FGameplayTag& OutTag, const bool bReport)
	{
		FString Written;
		if (const FDstLiteralExpr* Literal = Expr.As<FDstLiteralExpr>())
		{
			if (Literal->LiteralKind == EDstLiteralKind::String)
			{
				Written = Literal->Text;
			}
		}
		else
		{
			Written = GetDstPathString(Expr);
		}
		if (Written.IsEmpty() || Written.Equals(TEXT("None"), ESearchCase::CaseSensitive))
		{
			if (bReport)
			{
				Sink.Error(TEXT("DST4010"), Expr.Span, TEXT("Expected a gameplay tag, such as Game.Event.Hit."));
			}
			return false;
		}
		FString Error;
		if (!ResolveDstTagName(Written, ValueContext.TagPrefixes, OutTag, Error) || !OutTag.IsValid())
		{
			if (bReport)
			{
				Sink.Error(TEXT("DST4010"), Expr.Span, Error.IsEmpty() ? FString::Printf(TEXT("`%s` is not a tag of the project."), *Written) : Error,
					TEXT("Tags must exist in the project's tag table; the text does not add them. Declare prefixes with `Tags = { Game.Event; }`."));
			}
			return false;
		}
		return true;
	}

	bool FDstLowerer::ReadEventSpec(const FDstExpr& Expr, FDstEvent& OutEvent, const UScriptStruct*& OutPayload)
	{
		OutPayload = nullptr;
		const FDstExpr* TagExpr = &Expr;
		const FDstExpr* PayloadExpr = nullptr;
		if (const FDstCallExpr* Call = Expr.As<FDstCallExpr>())
		{
			TagExpr = Call->Callee.Get();
			if (Call->Args.Num() != 1 || !Call->Args[0] || Call->Args[0]->IsNamed() || Call->Args[0]->bOutput || !Call->Args[0]->Value)
			{
				Sink.Error(TEXT("DST4011"), Call->ArgsSpan, TEXT("An event with a payload is written Tag(PayloadType): one type name."));
				return false;
			}
			PayloadExpr = Call->Args[0]->Value.Get();
		}
		if (!TagExpr)
		{
			return false;
		}

		FGameplayTag Tag;
		if (!ResolveTag(*TagExpr, Tag))
		{
			return false;
		}
		OutEvent.Tag = Tag.ToString();

		if (PayloadExpr)
		{
			const FString TypeName = GetDstPathString(*PayloadExpr);
			const UScriptStruct* Struct = TypeName.IsEmpty() ? nullptr : ResolveStructByName(TypeName);
			if (!Struct)
			{
				Sink.Error(TEXT("DST4011"), PayloadExpr->Span, FString::Printf(TEXT("`%s` is not a struct that can be an event payload."), *TypeName),
					TEXT("Name a USTRUCT (VillagerTradeOffer) or a user-defined struct asset."));
				return false;
			}
			OutPayload = Struct;
			OutEvent.PayloadType = Struct->GetPathName();
			return true;
		}

		// No type written: the game's extension may give the tag one.
		if (Schema.IsValid())
		{
			for (const UDreamStateTreeExtension* Extension : Extensions)
			{
				if (const UScriptStruct* Struct = Extension ? Extension->GetDefaultEventPayload(*Schema, Tag) : nullptr)
				{
					OutPayload = Struct;
					OutEvent.PayloadType = Struct->GetPathName();
					break;
				}
			}
		}
		return true;
	}

	bool FDstLowerer::LowerEventClause(const FDstClauseStmt& Clause, FDstState& State, FStateFrame& Frame)
	{
		if (!Clause.Value)
		{
			return false;
		}
		FDstEvent Event;
		const UScriptStruct* Payload = nullptr;
		if (!ReadEventSpec(*Clause.Value, Event, Payload))
		{
			return false;
		}
		if (Frame.PendingConsumeEvent.IsSet())
		{
			Event.bConsumeOnSelect = Frame.PendingConsumeEvent.GetValue();
		}
		State.Event = Event;
		AddPlace(State.Path + TEXT("#on"), Clause.Span);

		Frame.Event.bValid = true;
		Frame.Event.Kind = EDstSourceKind::StateEvent;
		Frame.Event.Owner = State.Path;
		Frame.Event.Payload = Payload;
		FString Error;
		ResolveDstTagName(Event.Tag, TConstArrayView<FString>(), Frame.Event.Tag, Error);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// `pick`
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerPickClause(const FDstClauseStmt& Clause, const bool bHasChildren, FDstState& State)
	{
		const FDstNameExpr* Word = Clause.Value ? Clause.Value->As<FDstNameExpr>() : nullptr;
		if (!Word)
		{
			return false;
		}
		const TCHAR* Engine = nullptr;
		TArray<FString> Words;
		for (const Sugar::FWord& Entry : Sugar::PickWords)
		{
			Words.Add(Entry.Written);
			if (Word->Name.Equals(Entry.Written, ESearchCase::CaseSensitive))
			{
				Engine = Entry.Engine;
			}
		}
		if (!Engine)
		{
			Sink.Error(TEXT("DST4008"), Clause.Value->Span, FString::Printf(TEXT("`%s` is not a way to pick a child."), *Word->Name),
				FString::Printf(TEXT("The ways are %s."), *FString::Join(Words, TEXT(", "))));
			return false;
		}

		if (Schema.IsValid())
		{
			if (const UEnum* Enum = StaticEnum<EStateTreeStateSelectionBehavior>())
			{
				const int64 Value = Enum->GetValueByNameString(Engine);
				if (Value != INDEX_NONE && !Schema->IsStateSelectionAllowed(static_cast<EStateTreeStateSelectionBehavior>(Value)))
				{
					Sink.Error(TEXT("DST4015"), Clause.Span, FString::Printf(TEXT("The schema does not allow `pick %s`."), *Word->Name));
					return false;
				}
			}
		}
		// Enter, None and FollowTransitions say how the state itself is entered, which a leaf has too.
		const FString EngineName(Engine);
		const bool bChoosesChild = EngineName != TEXT("TryEnterState") && EngineName != TEXT("None") && EngineName != TEXT("TryFollowTransitions");
		if (!bHasChildren && bChoosesChild)
		{
			Sink.Warning(TEXT("DST6010"), Clause.Span, TEXT("`pick` chooses among child states, and this state has none."),
				TEXT("Remove the clause, or add child states."));
		}
		// The model holds the name the engine prints for the value: UniformRandom is another spelling of Random.
		State.Selection = Engine;
		if (const UEnum* Enum = StaticEnum<EStateTreeStateSelectionBehavior>())
		{
			const int64 Value = Enum->GetValueByNameString(Engine);
			if (Value != INDEX_NONE)
			{
				const FString Canonical = Enum->GetNameStringByValue(Value);
				if (!Canonical.IsEmpty())
				{
					State.Selection = Canonical;
				}
			}
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// `do`
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerTasks(const TArray<const FDstClauseStmt*>& DoClauses, FDstState& State, FStateFrame& Frame)
	{
		int32 NumTasks = 0;
		for (const FDstClauseStmt* Clause : DoClauses)
		{
			NumTasks += Clause->Tasks.Num();
		}
		if (NumTasks > 1 && Schema.IsValid() && !Schema->AllowMultipleTasks())
		{
			Sink.Error(TEXT("DST4015"), DoClauses[0]->Span, TEXT("The schema allows one task per state."),
				TEXT("Split the tasks into child states, or use a schema that allows several."));
		}

		int32 Index = 0;
		for (const FDstClauseStmt* Clause : DoClauses)
		{
			for (const TUniquePtr<FDstTaskItem>& Item : Clause->Tasks)
			{
				if (!Item || !Item->Node)
				{
					continue;
				}
				const FString Key = FString::Printf(TEXT("%s/Task[%d]"), *State.Path, Index++);

				// A name must not be one another place already reads.
				if (!Item->BindName.IsEmpty())
				{
					bool bTaken = Item->BindName.Equals(TEXT("Params"), ESearchCase::CaseSensitive) || Item->BindName.Equals(TEXT("Event"), ESearchCase::CaseSensitive);
					for (const FBindableNode& Global : GlobalNodes)
					{
						bTaken |= Global.Name.Equals(Item->BindName, ESearchCase::CaseSensitive);
					}
					for (const FContext& Context : Contexts)
					{
						bTaken |= Context.Name.Equals(Item->BindName, ESearchCase::CaseSensitive);
					}
					for (const FStateFrame* Visible : Frames)
					{
						for (const FBindableNode& Task : Visible->Tasks)
						{
							bTaken |= Task.Name.Equals(Item->BindName, ESearchCase::CaseSensitive);
						}
					}
					if (bTaken)
					{
						Sink.Error(TEXT("DST6015"), Item->BindNameSpan, FString::Printf(TEXT("The name `%s` is already taken where this task can be seen."), *Item->BindName),
							TEXT("Names of tasks must differ from the tasks above, the global nodes, the contexts, `Params` and `Event`."));
					}
				}

				FBindScope Scope;
				Scope.Target = EBindTarget::Task;
				FDstNode Node;
				FNodeInfoPtr Info;
				LowerNode(*Item->Node, Item->BindName, Item->BindNameSpan, Key, EDstNodeRole::Task, EDstNodeCategory::Task, Scope, Node, &Info);
				AddPlace(Key, Item->Span);

				// A schema with one task per state: the engine renames the task to the state's name on every compile, so the
				// model holds that name (and `State.Output` is how other places read it).
				if (Schema.IsValid() && !Schema->AllowMultipleTasks())
				{
					if (!Node.BindName.IsEmpty() && !Node.BindName.Equals(State.Name, ESearchCase::CaseSensitive))
					{
						Sink.Warning(TEXT("DST4022"), Item->BindNameSpan,
							FString::Printf(TEXT("The schema allows one task per state, and the engine names that task after the state: `%s` becomes `%s`."),
								*Node.BindName, *State.Name),
							FString::Printf(TEXT("Read its outputs as %s.Something."), *State.Name));
					}
					Node.BindName = State.Name;
				}

				const FString NodeName = Node.BindName;
				if (!NodeName.IsEmpty())
				{
					FBindableNode Bindable;
					Bindable.Name = NodeName;
					Bindable.Key = Key;
					Bindable.Kind = EDstSourceKind::Node;
					Bindable.InstanceStruct = Info.IsValid() ? Info->InstanceStruct : nullptr;
					Bindable.bIsTask = true;
					Bindable.bLowered = true;
					Frame.Tasks.Add(MoveTemp(Bindable));
				}
				State.Tasks.Add(MoveTemp(Node));
			}
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// `link`
	// ------------------------------------------------------------------------------------------

	const FParamSet* FDstLowerer::LoadAssetSignature(const FString& ObjectPath, FString& OutError)
	{
		if (const TUniquePtr<FParamSet>* Cached = AssetSignatures.Find(ObjectPath))
		{
			return Cached->Get();
		}

		const UStateTree* Asset = LoadObject<UStateTree>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		const UStateTreeEditorData* EditorData = Asset ? Cast<UStateTreeEditorData>(Asset->EditorData) : nullptr;
		if (!EditorData)
		{
			OutError = FString::Printf(TEXT("`%s` is not a StateTree asset with editor data."), *ObjectPath);
			return nullptr;
		}

		TUniquePtr<FParamSet> Signature = MakeUnique<FParamSet>();
		const FInstancedPropertyBag& Bag = EditorData->GetRootParametersPropertyBag();
		const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
		TArray<const FPropertyBagPropertyDesc*> Descs;
		if (BagStruct)
		{
			for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
			{
				FDstParameter Parameter;
				Parameter.Name = Desc.Name.ToString();
				Parameter.Type = BagDescToCanonicalType(Desc);
				Signature->Params.Add(MoveTemp(Parameter));
				Descs.Add(&Desc);
			}
		}
		if (!Signature->Build(OutError))
		{
			return nullptr;
		}
		// The defaults the asset gives, read through the rebuilt bag so the text is the exporter's own.
		for (int32 Index = 0; Index < Signature->Params.Num(); ++Index)
		{
			FDstParameter& Parameter = Signature->Params[Index];
			const FProperty* Property = Signature->FindProperty(Parameter.Name);
			const FPropertyBagPropertyDesc* Desc = Descs[Index];
			if (Property && Desc->CachedProperty && Bag.GetValue().GetMemory())
			{
				FStructOnScope Scratch(Signature->Bag.Get());
				if (Property->ImportText_InContainer(*DstExportDefault(*Desc->CachedProperty, Bag.GetValue().GetMemory()), Scratch.GetStructMemory(), nullptr, PPF_None))
				{
					const FString Text = DstExportDefault(*Property, Scratch.GetStructMemory());
					if (Text != DstExportDefault(*Property, Signature->GetDefaultMemory()))
					{
						Parameter.ValueText = Text;
					}
				}
			}
		}
		const FParamSet* Result = Signature.Get();
		AssetSignatures.Add(ObjectPath, MoveTemp(Signature));
		return Result;
	}

	bool FDstLowerer::LowerLinkClause(const FDstClauseStmt& Clause, FDstState& State, FStateFrame& Frame)
	{
		if (!Clause.Value)
		{
			return false;
		}
		// `link asset Path(Game, "AI/ST_Other")` passes no parameters; `link asset Path(...)(X = 1)` does.
		FString BarePath;
		const bool bBarePath = Clause.bLinkAsset && TryGetDstAssetPath(*Clause.Value, BarePath);
		const FDstCallExpr* Call = bBarePath ? nullptr : Clause.Value->As<FDstCallExpr>();
		const FDstExpr* Callee = Call ? Call->Callee.Get() : Clause.Value.Get();
		if (!Callee)
		{
			return false;
		}
		AddPlace(State.Path + TEXT("#link"), Clause.Span);

		if (State.Type == EDstStateType::Group)
		{
			Sink.Error(TEXT("DST6003"), Clause.Span, FString::Printf(TEXT("`%s` is a Group: it holds states, it cannot link another tree."), *State.Name),
				TEXT("Remove [ Group ] or the link."));
			return false;
		}

		// What the engine does when a link is set (UStateTreeState::SetLinkedState): the parameters take the target's layout,
		// and the state selects the linked tree itself.
		auto MarkLinked = [&State](const EDstStateType Type)
		{
			State.Type = Type;
			State.Selection = TEXT("TryEnterState");
			State.bFixedLayout = true;
		};

		if (!Clause.bLinkAsset)
		{
			const FString Name = GetDstPathString(*Callee);
			bool bAmbiguous = false;
			FSubtreeInfo* Info = Name.IsEmpty() ? nullptr : FindSubtree(Name, &bAmbiguous);
			if (!Info)
			{
				TArray<FString> Names;
				for (const TUniquePtr<FSubtreeInfo>& Existing : Subtrees)
				{
					Names.AddUnique(Existing->Name);
				}
				Sink.Error(TEXT("DST6006"), Callee->Span,
					bAmbiguous ? FString::Printf(TEXT("`%s` is a subtree of more than one included library."), *Name)
					           : FString::Printf(TEXT("There is no subtree called `%s`."), *Name),
					bAmbiguous ? FString(TEXT("Rename one of them.")) : (Names.IsEmpty() ? FString(TEXT("Declare it with `Subtree Name(...) { ... }`, here or in an #included library. To link another asset, write `link asset Name`.")) : MakeSuggestionHint(Name, Names)));
				return false;
			}
			if (!EnsureSignature(*Info))
			{
				return false;
			}

			MarkLinked(EDstStateType::Linked);
			State.LinkTarget = Info->Name;
			if (Schema.IsValid() && !Schema->IsStateTypeAllowed(EStateTreeStateType::Linked))
			{
				Sink.Error(TEXT("DST4015"), Clause.Span, TEXT("The schema does not allow linked states."));
			}
			if (Info->bLibrary && !Info->bQueued)
			{
				Info->bQueued = true;
				LibraryQueue.Add(Info);
				// The copy has no place of its own in the main file: it is placed at the first link.
				AddPlace(Info->Name, Clause.Span);
			}
			return LowerLinkArguments(Call, Info->Signature, State, Frame, Info->Name, Clause.Span);
		}

		// `link asset ST_Other(...)`
		FString ObjectPath;
		FString Error;
		if (!TryGetDstAssetPath(*Callee, ObjectPath))
		{
			const FString Name = GetDstPathString(*Callee);
			if (Name.IsEmpty() || !FindDstAsset(Name, UStateTree::StaticClass(), ObjectPath, Error))
			{
				Sink.Error(TEXT("DST6008"), Callee->Span, Error.IsEmpty() ? FString::Printf(TEXT("`%s` is not a StateTree asset."), *Name) : Error,
					TEXT("Write the asset's name, or Path(Game, \"AI/ST_Other\"). The asset has to be built before a tree can link it."));
				return false;
			}
		}
		if (ObjectPath == Out.Tree.AssetPath)
		{
			Sink.Error(TEXT("DST6009"), Callee->Span, TEXT("A tree cannot link itself."), TEXT("Link a subtree of this tree instead."));
			return false;
		}
		const FParamSet* Signature = LoadAssetSignature(ObjectPath, Error);
		if (!Signature)
		{
			Sink.Error(TEXT("DST6008"), Callee->Span, Error);
			return false;
		}
		if (LinkedAssetsReach(ObjectPath, Out.Tree.AssetPath, 0))
		{
			Sink.Error(TEXT("DST6009"), Callee->Span, FString::Printf(TEXT("`%s` links back to this tree: the assets would link each other in a circle."), *ObjectPath));
			return false;
		}

		MarkLinked(EDstStateType::LinkedAsset);
		State.LinkTarget = ObjectPath;
		Out.LinkedAssets.AddUnique(ObjectPath);
		if (Schema.IsValid() && !Schema->IsStateTypeAllowed(EStateTreeStateType::LinkedAsset))
		{
			Sink.Error(TEXT("DST4015"), Clause.Span, TEXT("The schema does not allow linked assets."));
		}
		return LowerLinkArguments(Call, *Signature, State, Frame, FPackageName::ObjectPathToObjectName(ObjectPath), Clause.Span);
	}

	bool FDstLowerer::LinkedAssetsReach(const FString& FromAsset, const FString& TargetAsset, const int32 Depth)
	{
		if (Depth > 16)
		{
			return false;
		}
		const UStateTree* Asset = LoadObject<UStateTree>(nullptr, *FromAsset, nullptr, LOAD_NoWarn | LOAD_Quiet);
		const UStateTreeEditorData* EditorData = Asset ? Cast<UStateTreeEditorData>(Asset->EditorData) : nullptr;
		if (!EditorData)
		{
			return false;
		}
		bool bReached = false;
		EditorData->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*)
		{
			if (State.Type == EStateTreeStateType::LinkedAsset && State.LinkedAsset)
			{
				const FString Linked = State.LinkedAsset->GetPathName();
				if (Linked == TargetAsset || LinkedAssetsReach(Linked, TargetAsset, Depth + 1))
				{
					bReached = true;
					return EStateTreeVisitor::Break;
				}
			}
			return EStateTreeVisitor::Continue;
		});
		return bReached;
	}

	bool FDstLowerer::LowerLinkArguments(const FDstCallExpr* Call, const FParamSet& Target, FDstState& State, FStateFrame& Frame, const FString& TargetName, const FDstSpan& Span)
	{
		State.Parameters = Target.Params;
		if (!Call)
		{
			return true;
		}
		const int32 ErrorsBefore = Sink.Real.NumErrors();

		// The values are evaluated in this state's scope, before its own event and tasks exist.
		FDstNode Pseudo;
		// A property function's key starts with this: `Root/Hurt/Parameters/Fn:Toughness`.
		Pseudo.Key = State.Path + TEXT("/Parameters");
		FNodeBuild Build;
		Build.Node = &Pseudo;
		Build.Scope.Target = EBindTarget::LinkParameter;
		Build.CallSpan = Span;
		Build.DisplayName = TargetName;
		Build.bBagProperties = true;
		(void)Frame;

		int32 Positional = 0;
		TArray<FString> Given;
		for (const FDstArgumentPtr& Argument : Call->Args)
		{
			if (!Argument || !Argument->Value)
			{
				continue;
			}
			if (Argument->bOutput)
			{
				Sink.Error(TEXT("DST6007"), Argument->Span, TEXT("`->` is for the outputs of a node; a link takes `Name = value`."));
				continue;
			}

			FString Name;
			if (Argument->IsNamed())
			{
				Name = Argument->Name;
			}
			else if (Positional < Target.Params.Num())
			{
				Name = Target.Params[Positional++].Name;
			}
			else
			{
				Sink.Error(TEXT("DST6007"), Argument->Span, FString::Printf(TEXT("`%s` has only %d parameter(s)."), *TargetName, Target.Params.Num()));
				continue;
			}

			const FProperty* Property = Target.FindProperty(Name);
			int32 ParamIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Target.Params.Num(); ++Index)
			{
				if (Target.Params[Index].Name == Name)
				{
					ParamIndex = Index;
				}
			}
			if (!Property || ParamIndex == INDEX_NONE)
			{
				Sink.Error(TEXT("DST6007"), Argument->IsNamed() ? Argument->NameSpan : Argument->Span,
					FString::Printf(TEXT("`%s` has no parameter `%s`."), *TargetName, *Name), MakeSuggestionHint(Name, Target.GetNames()));
				continue;
			}
			if (Given.Contains(Name))
			{
				Sink.Error(TEXT("DST6007"), Argument->Span, FString::Printf(TEXT("`%s` is given twice."), *Name));
				continue;
			}
			Given.Add(Name);

			Pseudo.Args.Reset();
			if (!LowerValue(Build, EDstPropertySide::Instance, *Property, *Argument->Value))
			{
				continue;
			}
			for (FDstArg& Arg : Pseudo.Args)
			{
				if (Arg.Kind == EDstValueKind::Constant && Arg.Property.Segments.Num() == 1)
				{
					FDstParameter& Parameter = State.Parameters[ParamIndex];
					Parameter.bOverridden = true;
					Parameter.ValueText = Arg.ConstantText == DstExportDefault(*Property, Target.GetDefaultMemory()) ? FString() : Arg.ConstantText;
				}
				else
				{
					// Fed from elsewhere: a binding or a property function into the state's parameters.
					State.Parameters[ParamIndex].bOverridden = true;
					State.ParameterBindings.Add(MoveTemp(Arg));
				}
			}
		}
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	// ------------------------------------------------------------------------------------------
	// `->`
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerTransitionTrigger(const FDstExpr& Trigger, FDstTransition& OutTransition, FEventInfo& OutEvent, const FString& Owner, const FBindScope& Scope)
	{
		// The words: Succeeded, Failed, Completed, Tick.
		if (const FDstNameExpr* Word = Trigger.As<FDstNameExpr>(); Word && !Word->bQuoted)
		{
			for (const Sugar::FWord& Entry : Sugar::TriggerWords)
			{
				if (Word->Name.Equals(Entry.Written, ESearchCase::CaseSensitive))
				{
					if (Word->Name.Equals(TEXT("Completed"), ESearchCase::CaseSensitive)) { OutTransition.Trigger = EDstTransitionTrigger::OnStateCompleted; }
					else if (Word->Name.Equals(TEXT("Succeeded"), ESearchCase::CaseSensitive)) { OutTransition.Trigger = EDstTransitionTrigger::OnStateSucceeded; }
					else if (Word->Name.Equals(TEXT("Failed"), ESearchCase::CaseSensitive)) { OutTransition.Trigger = EDstTransitionTrigger::OnStateFailed; }
					else { OutTransition.Trigger = EDstTransitionTrigger::OnTick; }
					return true;
				}
			}
		}

		// Delegate(Task.Succeeds), Delegate(Info.OnTargetLost)
		if (const FDstCallExpr* Call = Trigger.As<FDstCallExpr>())
		{
			const FDstNameExpr* Callee = Call->Callee ? Call->Callee->As<FDstNameExpr>() : nullptr;
			if (Callee && !Callee->bQuoted && Callee->Name.Equals(TEXT("Delegate"), ESearchCase::CaseSensitive))
			{
				if (Call->Args.Num() != 1 || !Call->Args[0] || Call->Args[0]->IsNamed() || Call->Args[0]->bOutput || !Call->Args[0]->Value)
				{
					Sink.Error(TEXT("DST5018"), Call->ArgsSpan, TEXT("`Delegate(...)` takes one path."));
					return false;
				}
				FResolvedSource Source;
				EDstCompletion Completion = EDstCompletion::None;
				if (!ResolveDelegateSource(*Call->Args[0]->Value, Scope, Source, Completion))
				{
					return false;
				}
				OutTransition.Trigger = EDstTransitionTrigger::OnDelegate;
				OutTransition.Delegate = Source.Ref;
				OutTransition.Completion = Completion;
				return true;
			}
		}

		// An event: Tag, Tag(Payload).
		FDstEvent Event;
		const UScriptStruct* Payload = nullptr;
		if (!ReadEventSpec(Trigger, Event, Payload))
		{
			return false;
		}
		OutTransition.Trigger = EDstTransitionTrigger::OnEvent;
		OutTransition.Event = Event;
		OutEvent.bValid = true;
		OutEvent.Kind = EDstSourceKind::TransitionEvent;
		OutEvent.Owner = Owner;
		OutEvent.Payload = Payload;
		FString Error;
		ResolveDstTagName(Event.Tag, TConstArrayView<FString>(), OutEvent.Tag, Error);
		return true;
	}

	bool FDstLowerer::LowerTransitionOptions(const FDstMeta& OptionsMeta, FDstTransition& OutTransition)
	{
		bool bOk = true;
		const UScriptStruct* TransitionStruct = FStateTreeTransition::StaticStruct();

		for (const TUniquePtr<FDstMetaEntry>& Entry : OptionsMeta.Entries)
		{
			if (!Entry)
			{
				continue;
			}
			const FString& Key = Entry->Key;

			if (IsNodeSettingKey(Key))
			{
				Sink.Error(TEXT("DST4014"), Entry->KeySpan,
					FString::Printf(TEXT("`%s` is a setting of a node, and the last [ ] of a transition is the transition's own."), *Key),
					TEXT("To give the condition settings, write them before the options or put the call in parentheses."));
				bOk = false;
				continue;
			}

			auto ReadFloat = [this, &Entry, TransitionStruct](const TCHAR* PropertyName, FString& OutText) -> bool
			{
				const FProperty* Property = TransitionStruct->FindPropertyByName(PropertyName);
				FString Error;
				if (!Entry->Value || !Property || !DstLiteralToExportText(*Entry->Value, *Property, ValueContext, OutText, Error))
				{
					Sink.Error(TEXT("DST4008"), Entry->Value ? Entry->Value->Span : Entry->Span, Error.IsEmpty() ? FString(TEXT("A number of seconds is expected.")) : Error);
					return false;
				}
				return true;
			};

			if (Key.Equals(TEXT("Priority"), ESearchCase::CaseSensitive))
			{
				const FString Word = Entry->Value ? GetDstPathString(*Entry->Value) : FString();
				static const TCHAR* const Words[] = { TEXT("Low"), TEXT("Normal"), TEXT("Medium"), TEXT("High"), TEXT("Critical") };
				bool bKnown = false;
				for (const TCHAR* Candidate : Words)
				{
					bKnown |= Word.Equals(Candidate, ESearchCase::CaseSensitive);
				}
				if (!bKnown)
				{
					Sink.Error(TEXT("DST4008"), Entry->Value ? Entry->Value->Span : Entry->Span, TEXT("`Priority` is Low, Normal, Medium, High or Critical."));
					bOk = false;
					continue;
				}
				OutTransition.Priority = Word;
			}
			else if (Key.Equals(TEXT("Delay"), ESearchCase::CaseSensitive))
			{
				FString Text;
				if (!ReadFloat(TEXT("DelayDuration"), Text))
				{
					bOk = false;
					continue;
				}
				OutTransition.bDelay = true;
				OutTransition.DelayText = Text;
			}
			else if (Key.Equals(TEXT("Variance"), ESearchCase::CaseSensitive))
			{
				FString Text;
				if (!ReadFloat(TEXT("DelayRandomVariance"), Text))
				{
					bOk = false;
					continue;
				}
				OutTransition.bDelay = true;
				OutTransition.VarianceText = Text;
			}
			else if (Key.Equals(TEXT("Reactivate"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("Enabled"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("ConsumeEvent"), ESearchCase::CaseSensitive))
			{
				bool bValue = true;
				if (!ReadBoolSetting(*Entry, bValue))
				{
					Sink.Error(TEXT("DST4008"), Entry->Value ? Entry->Value->Span : Entry->Span, FString::Printf(TEXT("`%s` is a flag, or true / false."), *Key));
					bOk = false;
					continue;
				}
				if (Key.Equals(TEXT("Reactivate"), ESearchCase::CaseSensitive)) { OutTransition.bReactivate = bValue; }
				else if (Key.Equals(TEXT("Enabled"), ESearchCase::CaseSensitive)) { OutTransition.bEnabled = bValue; }
				else
				{
					if (OutTransition.Trigger != EDstTransitionTrigger::OnEvent)
					{
						Sink.Error(TEXT("DST4014"), Entry->KeySpan, TEXT("`ConsumeEvent` is for transitions that trigger on an event."));
						bOk = false;
						continue;
					}
					OutTransition.Event.bConsumeOnSelect = bValue;
				}
			}
			else
			{
				TArray<FString> Names;
				for (const Sugar::FWord& Word : Sugar::TransitionOptions)
				{
					Names.Add(Word.Written);
				}
				Sink.Error(TEXT("DST4014"), Entry->KeySpan, FString::Printf(TEXT("`%s` is not a transition option."), *Key),
					FString::Printf(TEXT("The options are %s."), *FString::Join(Names, TEXT(", "))));
				bOk = false;
			}
		}
		return bOk;
	}

	bool FDstLowerer::LowerTransition(const FDstClauseStmt& Clause, const int32 Index, FDstState& State, FStateFrame& Frame, FDstTransition& OutTransition)
	{
		const int32 ErrorsBefore = Sink.Real.NumErrors();
		const FString Owner = FString::Printf(TEXT("%s#%d"), *State.Path, Index);
		AddPlace(Owner, Clause.Span);

		// The trigger.
		FEventInfo TransitionEvent;
		if (Clause.Trigger)
		{
			FBindScope DelegateScope;
			DelegateScope.Target = EBindTarget::TransitionDelegate;
			LowerTransitionTrigger(*Clause.Trigger, OutTransition, TransitionEvent, Owner, DelegateScope);
		}

		// The target. Names of states are resolved once every state exists.
		if (Clause.Value)
		{
			const FDstNameExpr* Word = Clause.Value->As<FDstNameExpr>();
			bool bWord = false;
			if (Word && !Word->bQuoted)
			{
				for (const Sugar::FWord& Entry : Sugar::TargetWords)
				{
					if (Word->Name.Equals(Entry.Written, ESearchCase::CaseSensitive))
					{
						bWord = true;
						if (Word->Name.Equals(TEXT("Next"), ESearchCase::CaseSensitive)) { OutTransition.TargetKind = EDstTransitionTarget::NextState; }
						else if (Word->Name.Equals(TEXT("NextSelectable"), ESearchCase::CaseSensitive)) { OutTransition.TargetKind = EDstTransitionTarget::NextSelectableState; }
						else if (Word->Name.Equals(TEXT("Parent"), ESearchCase::CaseSensitive)) { OutTransition.TargetKind = EDstTransitionTarget::Parent; }
						else if (Word->Name.Equals(TEXT("NextParent"), ESearchCase::CaseSensitive)) { OutTransition.TargetKind = EDstTransitionTarget::NextParent; }
						else if (Word->Name.Equals(TEXT("NextSelectableParent"), ESearchCase::CaseSensitive)) { OutTransition.TargetKind = EDstTransitionTarget::NextSelectableParent; }
						else if (Word->Name.Equals(TEXT("TreeSucceeded"), ESearchCase::CaseSensitive)) { OutTransition.TargetKind = EDstTransitionTarget::Succeeded; }
						else { OutTransition.TargetKind = EDstTransitionTarget::Failed; }
					}
				}
			}
			if (!bWord)
			{
				TArray<FString> Segments;
				if (TryGetDstPath(*Clause.Value, Segments) && !Segments.IsEmpty())
				{
					OutTransition.TargetKind = EDstTransitionTarget::GotoState;
					FPendingTarget Pending;
					Pending.FromPath = State.Path;
					Pending.Index = Index;
					Pending.Segments = Segments;
					Pending.Span = Clause.Value->Span;
					Pending.File = Sink.CurrentFile;
					PendingTargets.Add(MoveTemp(Pending));
				}
				else
				{
					Sink.Error(TEXT("DST6002"), Clause.Value->Span, TEXT("The target of `->` is a state's name or path (Idle.Phone), or Next, Parent, NextParent, TreeSucceeded, TreeFailed."));
				}
			}
		}

		// The condition.
		if (Clause.Condition)
		{
			FBindScope Scope;
			Scope.Target = EBindTarget::TransitionCondition;
			Scope.TransitionEvent = TransitionEvent.bValid ? &TransitionEvent : nullptr;
			TArray<const FDstExpr*> Expressions;
			Expressions.Add(Clause.Condition.Get());
			LowerConditionList(Expressions, FString::Printf(TEXT("%s/Transition[%d]/Condition"), *State.Path, Index), EDstNodeRole::TransitionCondition, Scope, OutTransition.Conditions);
		}

		// The options.
		if (Clause.Meta)
		{
			LowerTransitionOptions(*Clause.Meta, OutTransition);
		}

		const bool bCompletion = OutTransition.Trigger == EDstTransitionTrigger::OnStateCompleted
			|| OutTransition.Trigger == EDstTransitionTrigger::OnStateSucceeded || OutTransition.Trigger == EDstTransitionTrigger::OnStateFailed;
		if (bCompletion && (OutTransition.bDelay || !OutTransition.Priority.Equals(TEXT("Normal"), ESearchCase::CaseSensitive)))
		{
			Sink.Warning(TEXT("DST6018"), Clause.Span, TEXT("The engine ignores delay and priority on a transition that triggers when the state completes."),
				TEXT("Use `on Tick` with a condition, or an event."));
		}
		(void)Frame;
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	// ------------------------------------------------------------------------------------------
	// Targets by name
	// ------------------------------------------------------------------------------------------

	void FDstLowerer::ResolvePendingTargets()
	{
		if (PendingTargets.IsEmpty())
		{
			return;
		}

		// Every state of the tree with its path split into names.
		struct FStateName
		{
			const FDstState* State = nullptr;
			TArray<FString> Parts;
		};
		TArray<FStateName> All;
		TFunction<void(const TArray<FDstState>&)> Collect = [&All, &Collect](const TArray<FDstState>& List)
		{
			for (const FDstState& State : List)
			{
				FStateName Item;
				Item.State = &State;
				State.Path.ParseIntoArray(Item.Parts, TEXT("/"), false);
				All.Add(MoveTemp(Item));
				Collect(State.Children);
			}
		};
		Collect(Out.Tree.States);

		for (const FPendingTarget& Pending : PendingTargets)
		{
			FDstState* From = Out.Tree.FindState(Pending.FromPath);
			if (!From || !From->Transitions.IsValidIndex(Pending.Index))
			{
				continue;
			}
			const FString SavedFile = Sink.CurrentFile;
			Sink.CurrentFile = Pending.File;

			// A name or path matches the states whose path ends with it: `Phone` every Phone, `Idle.Phone` the Phone in Idle.
			TArray<const FDstState*> Matches;
			for (const FStateName& Item : All)
			{
				if (Item.Parts.Num() < Pending.Segments.Num())
				{
					continue;
				}
				bool bEnds = true;
				const int32 Offset = Item.Parts.Num() - Pending.Segments.Num();
				for (int32 Index = 0; Index < Pending.Segments.Num() && bEnds; ++Index)
				{
					bEnds = Item.Parts[Offset + Index].Equals(Pending.Segments[Index], ESearchCase::CaseSensitive);
				}
				if (bEnds)
				{
					Matches.Add(Item.State);
				}
			}

			const FString Written = FString::Join(Pending.Segments, TEXT("."));
			if (Matches.Num() == 1)
			{
				From->Transitions[Pending.Index].TargetState = Matches[0]->Path;
			}
			else if (Matches.IsEmpty())
			{
				TArray<FString> Names;
				CollectStateNames(Out.Tree.States, Names);
				Sink.Error(TEXT("DST6002"), Pending.Span, FString::Printf(TEXT("There is no state called `%s` to go to."), *Written),
					MakeSuggestionHint(Pending.Segments.Last(), Names));
			}
			else
			{
				TArray<FString> Paths;
				for (const FDstState* Match : Matches)
				{
					Paths.Add(Match->Path.Replace(TEXT("/"), TEXT(".")));
				}
				Sink.Error(TEXT("DST6002"), Pending.Span,
					FString::Printf(TEXT("`%s` matches %d states: %s."), *Written, Matches.Num(), *FString::Join(Paths, TEXT(", "))),
					TEXT("Write more of the path, such as Parent.Name, until it names one."));
			}
			Sink.CurrentFile = SavedFile;
		}
		PendingTargets.Reset();
	}
}
