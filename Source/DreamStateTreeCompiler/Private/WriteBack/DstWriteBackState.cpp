// Copyright DreamMoon. All Rights Reserved.
//
// One state, text against asset, smallest edit first:
//   - nothing differs inside the card: the children are looked at;
//   - only the settings of the `[ ]` and the arguments of nodes differ: the `[ ]` is replaced, the nodes are written
//     where they stand (DstWriteBackNode.cpp);
//   - anything else (clauses, tasks, conditions or transitions added, removed, reordered or changed; child states added,
//     removed or moved): the card is printed again from the asset. What did not change keeps its text: clauses of the
//     original card that still say the same thing are put back as they were, with their comments, and a child card that
//     did not change (or changed only in place) is put back as it was written.
//
// Every edit is against the original text, so a child's edits can be folded into its parent's reprint by applying them
// to the child's slice of the original.

#include "WriteBack/DstWriteBackInternal.h"

namespace UE::DreamStateTree::Compiler::WriteBackPrivate
{
	// ------------------------------------------------------------------------------------------
	// Lookups
	// ------------------------------------------------------------------------------------------

	Lang::FDstStateStmt* FWriteBack::FindStateStmt(const FString& TPath) const
	{
		Lang::FDstStateStmt* const* Found = StateStmts.Find(TPath);
		return Found ? *Found : nullptr;
	}

	const Lang::FDstStateStmt* FWriteBack::FindRaisedState(const FString& APath) const
	{
		const Lang::FDstStateStmt* const* Found = RaisedStates.Find(APath);
		return Found ? *Found : nullptr;
	}

	const Lang::FDstClauseStmt* WbFindClause(const Lang::FDstStateStmt& Card, const Lang::EDstClauseKind Kind, const int32 Nth)
	{
		int32 Seen = 0;
		for (const Lang::FDstStmtPtr& Item : Card.Body)
		{
			const Lang::FDstClauseStmt* Clause = Item ? Item->As<Lang::FDstClauseStmt>() : nullptr;
			if (Clause && Clause->ClauseKind == Kind && Seen++ == Nth)
			{
				return Clause;
			}
		}
		return nullptr;
	}

	const Lang::FDstExpr* FWriteBack::FindRaisedTaskExpr(const FString& APath, const int32 Index) const
	{
		const Lang::FDstStateStmt* Card = FindRaisedState(APath);
		if (!Card)
		{
			return nullptr;
		}
		int32 Count = 0;
		for (const Lang::FDstStmtPtr& Item : Card->Body)
		{
			const Lang::FDstClauseStmt* Clause = Item ? Item->As<Lang::FDstClauseStmt>() : nullptr;
			if (!Clause || Clause->ClauseKind != Lang::EDstClauseKind::Do)
			{
				continue;
			}
			for (const TUniquePtr<Lang::FDstTaskItem>& Task : Clause->Tasks)
			{
				if (Count++ == Index)
				{
					return Task ? Task->Node.Get() : nullptr;
				}
			}
		}
		return nullptr;
	}

	const Lang::FDstExpr* FWriteBack::FindRaisedGlobalExpr(const int32 Index) const
	{
		const Lang::FDstSectionStmt* Section = FindRaisedSection(TEXT("Globals"));
		if (!Section || !Section->Entries.IsValidIndex(Index) || !Section->Entries[Index])
		{
			return nullptr;
		}
		const Lang::FDstAssignStmt* Assign = Section->Entries[Index]->As<Lang::FDstAssignStmt>();
		return Assign ? Assign->Value.Get() : nullptr;
	}

	/** `[ ... ]` the way a card's head prints it. */
	FString FWriteBack::PrintMetaOf(const Lang::FDstMeta& Meta) const
	{
		Lang::FDstStateStmt Probe;
		Probe.Name = TEXT("M");
		Probe.Meta = Lang::CloneMeta(Meta);
		// `State M [ ... ] {}`: the settings sit between the name and the body's brace.
		const FString Printed = Lang::PrintDstStmt(Probe, 0, nullptr, Edit->MakePrintOptions());
		const int32 Start = FString(TEXT("State M ")).Len();
		const int32 Close = Printed.Find(TEXT(" {"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Close == INDEX_NONE || Close < Start)
		{
			return FString();
		}
		return Printed.Mid(Start, Close - Start);
	}

	// ------------------------------------------------------------------------------------------
	// Settings and nodes in place
	// ------------------------------------------------------------------------------------------

	void FWriteBack::PlanStateSettings(const Model::FDstState& AState, const Model::FDstState& TState, Lang::FDstStateStmt& Stmt,
		const FString& Location, FPlan& Plan)
	{
		(void)TState;
		const Lang::FDstStateStmt* Card = bRaiseOk ? FindRaisedState(AState.Path) : nullptr;
		if (!Card)
		{
			Plan.bNeedsReprint = true;
			return;
		}

		const bool bWants = Card->Meta.IsValid() && !Card->Meta->Entries.IsEmpty();
		FEdit Replacement;
		if (bWants)
		{
			const FString MetaText = PrintMetaOf(*Card->Meta);
			if (MetaText.IsEmpty())
			{
				Plan.bNeedsReprint = true;
				return;
			}
			if (Stmt.Meta.IsValid())
			{
				Replacement.Offset = Stmt.Meta->Span.Offset;
				Replacement.Length = Stmt.Meta->Span.Length;
				Replacement.Text = MetaText;
			}
			else
			{
				// `State X {` becomes `State X [ Weight = 2 ] {`.
				Replacement.Offset = Stmt.OpenSpan.Offset;
				Replacement.Length = 0;
				Replacement.Text = MetaText + TEXT(" ");
			}
		}
		else if (Stmt.Meta.IsValid())
		{
			// Every setting is back at its default: the `[ ]` goes, with the space in front of it.
			const FString& Source = Edit->GetText();
			int32 Start = Stmt.Meta->Span.Offset;
			while (Start > 0 && (Source[Start - 1] == TEXT(' ') || Source[Start - 1] == TEXT('\t')))
			{
				--Start;
			}
			Replacement.Offset = Start;
			Replacement.Length = Stmt.Meta->Span.End() - Start;
		}
		else
		{
			Plan.bNeedsReprint = true;
			return;
		}

		Plan.Edits.Add(MoveTemp(Replacement));
		FDstWriteBackChange Change;
		Change.Location = Location;
		Change.What = TEXT("the settings");
		Change.bWritten = true;
		Plan.Changes.Add(MoveTemp(Change));
	}

	void FWriteBack::PlanStateNodes(const Model::FDstState& AState, const Model::FDstState& TState, const FString& Location, FPlan& Plan)
	{
		const auto Label = [&Location](const TCHAR* Word, const Model::FDstNode& Node, const int32 Index)
		{
			return Node.BindName.IsEmpty() ? FString::Printf(TEXT("%s: %s #%d"), *Location, Word, Index)
				: FString::Printf(TEXT("%s: %s %s"), *Location, Word, *Node.BindName);
		};

		for (int32 Index = 0; Index < FMath::Min(TState.Tasks.Num(), AState.Tasks.Num()) && !Plan.bNeedsReprint; ++Index)
		{
			PlanNode(TState.Tasks[Index], AState.Tasks[Index], Label(TEXT("Task"), TState.Tasks[Index], Index), FindRaisedTaskExpr(AState.Path, Index), Plan);
		}
		for (int32 Index = 0; Index < FMath::Min(TState.EnterConditions.Num(), AState.EnterConditions.Num()) && !Plan.bNeedsReprint; ++Index)
		{
			PlanNode(TState.EnterConditions[Index], AState.EnterConditions[Index], Label(TEXT("Enter condition"), TState.EnterConditions[Index], Index), nullptr, Plan);
		}
		for (int32 Index = 0; Index < FMath::Min(TState.Considerations.Num(), AState.Considerations.Num()) && !Plan.bNeedsReprint; ++Index)
		{
			PlanNode(TState.Considerations[Index], AState.Considerations[Index], Label(TEXT("Consideration"), TState.Considerations[Index], Index), nullptr, Plan);
		}
		for (int32 TransitionIndex = 0; TransitionIndex < FMath::Min(TState.Transitions.Num(), AState.Transitions.Num()) && !Plan.bNeedsReprint; ++TransitionIndex)
		{
			const TArray<Model::FDstNode>& TextConditions = TState.Transitions[TransitionIndex].Conditions;
			const TArray<Model::FDstNode>& AssetConditions = AState.Transitions[TransitionIndex].Conditions;
			for (int32 Index = 0; Index < FMath::Min(TextConditions.Num(), AssetConditions.Num()) && !Plan.bNeedsReprint; ++Index)
			{
				const FString Word = FString::Printf(TEXT("Transition #%d condition"), TransitionIndex);
				PlanNode(TextConditions[Index], AssetConditions[Index], Label(*Word, TextConditions[Index], Index), nullptr, Plan);
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// A state
	// ------------------------------------------------------------------------------------------

	void FWriteBack::PlanLibraryState(const Model::FDstState& AState, const Model::FDstState& TState)
	{
		FOwnView Whole;
		Whole.bWithChildren = true;
		if (DumpStateView(AState, Whole).Equals(DumpStateView(TState, Whole), ESearchCase::CaseSensitive))
		{
			return;
		}

		const FString Location = TEXT("State ") + AState.Path;
		FString RootName = TState.Path;
		int32 Slash = INDEX_NONE;
		if (RootName.FindChar(TEXT('/'), Slash))
		{
			RootName.LeftInline(Slash);
		}
		if (StateStmts.Contains(RootName))
		{
			// The card is in the text and was not found: nothing to say about a library.
			AddChange(Location, TEXT("the state"), false, false);
			Refuse(TEXT("DST8105"), Places.Find(TState.Path), FString::Printf(TEXT("%s changed in the editor, but its place in the text was not found."), *Location));
			return;
		}
		AddChange(Location, TEXT("the subtree"), false, false);
		Refuse(TEXT("DST8103"), Places.Find(TState.Path),
			FString::Printf(TEXT("%s is a subtree copied in from an included library; it changed in the editor."), *Location),
			TEXT("Make the change in the library's subtree, where every tree that links it gets it."));
	}

	void FWriteBack::PlanChildren(const Model::FDstState& AState, const Model::FDstState& TState, TArray<FEdit>& Out)
	{
		(void)TState;
		for (const Model::FDstState& AChild : AState.Children)
		{
			const FString* TPath = AToT.Find(AChild.Path);
			const Model::FDstState* TChild = TPath ? T.FindState(*TPath) : nullptr;
			if (TChild)
			{
				PlanState(AChild, *TChild, Out);
			}
		}
	}

	FString FWriteBack::DescribeStateChange(const Model::FDstState& AState, const Model::FDstState& TState, const bool bChildrenChanged) const
	{
		const auto Differs = [](const FString& Left, const FString& Right) { return !Left.Equals(Right, ESearchCase::CaseSensitive); };

		TArray<FString> Parts;
		if (Differs(AState.Name, TState.Name))
		{
			Parts.Add(TEXT("the name"));
		}
		if (Differs(EventKeyOf(AState), EventKeyOf(TState)))
		{
			Parts.Add(TEXT("the event"));
		}
		if (Differs(DumpNodesOf(AState.EnterConditions), DumpNodesOf(TState.EnterConditions)))
		{
			Parts.Add(TEXT("the enter conditions"));
		}
		if (Differs(PickKeyOf(AState), PickKeyOf(TState)))
		{
			Parts.Add(TEXT("the way children are picked"));
		}
		if (Differs(DumpNodesOf(AState.Considerations), DumpNodesOf(TState.Considerations)))
		{
			Parts.Add(TEXT("the utility"));
		}
		if (Differs(LinkKeyOf(AState), LinkKeyOf(TState)))
		{
			Parts.Add(TEXT("the link"));
		}
		if (Differs(ParamsKeyOf(AState), ParamsKeyOf(TState)))
		{
			Parts.Add(TEXT("the parameters"));
		}
		if (Differs(DumpNodesOf(AState.Tasks), DumpNodesOf(TState.Tasks)))
		{
			Parts.Add(TEXT("the tasks"));
		}
		bool bTransitionsChanged = AState.Transitions.Num() != TState.Transitions.Num();
		for (int32 Index = 0; !bTransitionsChanged && Index < AState.Transitions.Num(); ++Index)
		{
			bTransitionsChanged = Differs(DumpTransitionOf(AState.Transitions[Index]), DumpTransitionOf(TState.Transitions[Index]));
		}
		if (bTransitionsChanged)
		{
			Parts.Add(TEXT("the transitions"));
		}
		if (SettingsDiffer(AState, TState))
		{
			Parts.Add(TEXT("the settings"));
		}
		if (bChildrenChanged)
		{
			Parts.Add(TEXT("the child states"));
		}
		return Parts.IsEmpty() ? FString(TEXT("the card")) : FString::Join(Parts, TEXT(", "));
	}

	void FWriteBack::PlanState(const Model::FDstState& AState, const Model::FDstState& TState, TArray<FEdit>& Out)
	{
		const FString Location = TEXT("State ") + AState.Path;
		Lang::FDstStateStmt* Stmt = FindStateStmt(TState.Path);
		if (!Stmt)
		{
			PlanLibraryState(AState, TState);
			return;
		}

		FOwnView Rest;
		Rest.bStripArgs = true;
		Rest.bBlankSettings = true;
		FOwnView NoSettings;
		NoSettings.bBlankSettings = true;
		const bool bRestEqual = DumpStateView(AState, Rest).Equals(DumpStateView(TState, Rest), ESearchCase::CaseSensitive);
		const bool bChildrenAligned = ChildrenAligned(AState, TState);
		const bool bSettingsDiffer = SettingsDiffer(AState, TState);
		const bool bArgsDiffer = bRestEqual && !DumpStateView(AState, NoSettings).Equals(DumpStateView(TState, NoSettings), ESearchCase::CaseSensitive);

		if (bRestEqual && bChildrenAligned)
		{
			if (!bSettingsDiffer && !bArgsDiffer)
			{
				PlanChildren(AState, TState, Out);
				return;
			}

			// Only the `[ ]` and the arguments of nodes differ: written where they stand when every one of them can be.
			FPlan Plan;
			if (bSettingsDiffer)
			{
				PlanStateSettings(AState, TState, *Stmt, Location, Plan);
			}
			if (!Plan.bNeedsReprint && bArgsDiffer)
			{
				PlanStateNodes(AState, TState, Location, Plan);
			}
			if (!Plan.bNeedsReprint)
			{
				Commit(Plan, Out);
				PlanChildren(AState, TState, Out);
				return;
			}
		}

		// Anything else: the card is printed again.
		const FString What = DescribeStateChange(AState, TState, !bChildrenAligned);
		if (!Options.bStructure)
		{
			AddChange(Location, What, false, true);
			Refuse(TEXT("DST8104"), Places.Find(TState.Path),
				FString::Printf(TEXT("%s: %s changed in the editor; structural changes are not written back."), *Location, *What),
				TEXT("Make the change in the text, or turn on writing structural changes back."));
			// A card inside this one that changed only in place is still written where it stands.
			PlanChildren(AState, TState, Out);
			return;
		}

		FString CardText;
		if (!BuildCardText(AState, TState, *Stmt, CardText))
		{
			AddChange(Location, What, false, true);
			Refuse(TEXT("DST8105"), Places.Find(TState.Path),
				FString::Printf(TEXT("%s: %s changed in the editor, but the card could not be written from the asset."), *Location, *What),
				TEXT("The asset holds something the text has no spelling for; see the other messages."));
			return;
		}
		AddReplace(Out, Stmt->Span, CardText);
		AddChange(Location, What, true, true);
	}

	// ------------------------------------------------------------------------------------------
	// A card, printed again
	// ------------------------------------------------------------------------------------------

	FString WbTaskKey(const Lang::FDstTaskItem& Item)
	{
		return Item.BindName + TEXT("=") + (Item.Node ? Lang::PrintDstExpr(*Item.Node) : FString());
	}

	/** What the original body is made of, by kind of clause. Kinds that can only be written once have at most one in a good text. */
	struct FBodyParts
	{
		TArray<Lang::FDstStmtPtr> Ons;
		TArray<Lang::FDstStmtPtr> Ifs;
		TArray<Lang::FDstStmtPtr> Picks;
		TArray<Lang::FDstStmtPtr> Utilities;
		TArray<Lang::FDstStmtPtr> Links;
		TArray<Lang::FDstStmtPtr> Dos;
		TArray<Lang::FDstStmtPtr> Transitions;
		TArray<Lang::FDstStmtPtr> Children;
		TArray<Lang::FDstStmtPtr> Others;
	};

	/** A child card that goes back as it was written (with the edits made inside it). */
	struct FKeptCard
	{
		FString Name;
		FString Text;
		int32 OldIndentChars = 0;
	};

	bool FWriteBack::BuildCardText(const Model::FDstState& AState, const Model::FDstState& TState, Lang::FDstStateStmt& Stmt, FString& OutText)
	{
		const Lang::FDstStateStmt* Card = bRaiseOk ? FindRaisedState(AState.Path) : nullptr;
		if (!Card)
		{
			return false;
		}
		Lang::FDstFile& File = Edit->GetFile();
		const Lang::FDstSpan CardSpan = Stmt.Span;

		// -- What each kind of clause comes to: kept as written, or written again from the asset. Settled before anything is
		// -- moved, so a card that cannot be written is left whole.
		struct FDecision
		{
			bool bSame = false;
			const Lang::FDstClauseStmt* Wanted = nullptr;
			bool bNeeded = false;
		};
		const auto Decide = [&](const bool bSame, const Lang::EDstClauseKind Kind, const bool bNeeded)
		{
			FDecision Decision;
			Decision.bSame = bSame;
			Decision.bNeeded = bNeeded;
			Decision.Wanted = WbFindClause(*Card, Kind, 0);
			return Decision;
		};
		const auto Same = [](const FString& Left, const FString& Right) { return Left.Equals(Right, ESearchCase::CaseSensitive); };

		const FDecision OnDecision = Decide(Same(EventKeyOf(AState), EventKeyOf(TState)), Lang::EDstClauseKind::On, AState.Event.IsSet());
		const FDecision IfDecision = Decide(Same(DumpNodesOf(AState.EnterConditions), DumpNodesOf(TState.EnterConditions)), Lang::EDstClauseKind::If,
			!AState.EnterConditions.IsEmpty());
		const FDecision PickDecision = Decide(Same(PickKeyOf(AState), PickKeyOf(TState)), Lang::EDstClauseKind::Pick, false);
		const FDecision UtilityDecision = Decide(Same(DumpNodesOf(AState.Considerations), DumpNodesOf(TState.Considerations)), Lang::EDstClauseKind::Utility,
			!AState.Considerations.IsEmpty());
		const FDecision LinkDecision = Decide(Same(LinkKeyOf(AState), LinkKeyOf(TState)), Lang::EDstClauseKind::Link, IsLinkedState(AState));
		const FDecision DoDecision = Decide(Same(DumpNodesOf(AState.Tasks), DumpNodesOf(TState.Tasks)), Lang::EDstClauseKind::Do, !AState.Tasks.IsEmpty());
		for (const FDecision* Decision : { &OnDecision, &IfDecision, &PickDecision, &UtilityDecision, &LinkDecision, &DoDecision })
		{
			if (!Decision->bSame && Decision->bNeeded && !Decision->Wanted)
			{
				return false;
			}
		}

		int32 NumOriginalTransitions = 0;
		for (const Lang::FDstStmtPtr& Item : Stmt.Body)
		{
			const Lang::FDstClauseStmt* Clause = Item ? Item->As<Lang::FDstClauseStmt>() : nullptr;
			NumOriginalTransitions += (Clause && Clause->ClauseKind == Lang::EDstClauseKind::Transition) ? 1 : 0;
		}
		const TArray<int32> TransitionMatch = AlignTransitions(AState, TState);
		for (int32 Index = 0; Index < AState.Transitions.Num(); ++Index)
		{
			const int32 Counterpart = TransitionMatch[Index];
			const bool bKept = Counterpart != INDEX_NONE && Counterpart < NumOriginalTransitions
				&& Same(DumpTransitionOf(AState.Transitions[Index]), DumpTransitionOf(TState.Transitions[Counterpart]));
			if (!bKept && !WbFindClause(*Card, Lang::EDstClauseKind::Transition, Index))
			{
				return false;
			}
		}

		for (const Model::FDstState& AChild : AState.Children)
		{
			const FString* TPath = AToT.Find(AChild.Path);
			const bool bHasOld = TPath && FindStateStmt(*TPath) && T.FindState(*TPath);
			if (!bHasOld && !FindRaisedState(AChild.Path))
			{
				return false;
			}
		}

		// -- The body, taken apart.
		FBodyParts Parts;
		for (Lang::FDstStmtPtr& Item : Stmt.Body)
		{
			if (!Item)
			{
				continue;
			}
			const Lang::FDstClauseStmt* Clause = Item->As<Lang::FDstClauseStmt>();
			if (Item->Is<Lang::FDstStateStmt>())
			{
				Parts.Children.Add(MoveTemp(Item));
			}
			else if (!Clause)
			{
				Parts.Others.Add(MoveTemp(Item));
			}
			else
			{
				switch (Clause->ClauseKind)
				{
				case Lang::EDstClauseKind::On: Parts.Ons.Add(MoveTemp(Item)); break;
				case Lang::EDstClauseKind::If: Parts.Ifs.Add(MoveTemp(Item)); break;
				case Lang::EDstClauseKind::Pick: Parts.Picks.Add(MoveTemp(Item)); break;
				case Lang::EDstClauseKind::Utility: Parts.Utilities.Add(MoveTemp(Item)); break;
				case Lang::EDstClauseKind::Link: Parts.Links.Add(MoveTemp(Item)); break;
				case Lang::EDstClauseKind::Do: Parts.Dos.Add(MoveTemp(Item)); break;
				case Lang::EDstClauseKind::Transition: Parts.Transitions.Add(MoveTemp(Item)); break;
				}
			}
		}
		Stmt.Body.Reset();

		const auto BuryAll = [this](TArray<Lang::FDstStmtPtr>& List)
		{
			for (Lang::FDstStmtPtr& Item : List)
			{
				Edit->Bury(MoveTemp(Item));
			}
			List.Reset();
		};

		if (!Parts.Others.IsEmpty())
		{
			Refuse(TEXT("DST8107"), Parts.Others[0]->Span, FString::Printf(TEXT("State %s was written again; the #Region lines inside it are dropped."), *AState.Path),
				FString(), /*bRefusal*/ false);
			BuryAll(Parts.Others);
		}

		// -- The clauses.
		TArray<Lang::FDstStmtPtr> NewBody;
		const auto KeepOrReplace = [&](const FDecision& Decision, TArray<Lang::FDstStmtPtr>& Originals)
		{
			if (Decision.bSame)
			{
				for (Lang::FDstStmtPtr& Item : Originals)
				{
					NewBody.Add(MoveTemp(Item));
				}
				Originals.Reset();
				return;
			}
			if (Decision.Wanted)
			{
				Lang::FDstStmtPtr Fresh = Lang::CloneStmt(*Decision.Wanted);
				if (!Originals.IsEmpty())
				{
					CopyTrivia(File, Originals[0].Get(), Fresh.Get());
					// Comments between the tasks go with the task they were on.
					Lang::FDstClauseStmt* FreshClause = Fresh->As<Lang::FDstClauseStmt>();
					if (FreshClause && FreshClause->ClauseKind == Lang::EDstClauseKind::Do)
					{
						TSet<const Lang::FDstTaskItem*> Taken;
						for (const TUniquePtr<Lang::FDstTaskItem>& NewItem : FreshClause->Tasks)
						{
							if (!NewItem)
							{
								continue;
							}
							const FString NewKey = WbTaskKey(*NewItem);
							bool bDone = false;
							for (const Lang::FDstStmtPtr& OldClauseStmt : Originals)
							{
								const Lang::FDstClauseStmt* OldClause = OldClauseStmt ? OldClauseStmt->As<Lang::FDstClauseStmt>() : nullptr;
								for (int32 OldIndex = 0; OldClause && !bDone && OldIndex < OldClause->Tasks.Num(); ++OldIndex)
								{
									const Lang::FDstTaskItem* OldItem = OldClause->Tasks[OldIndex].Get();
									if (OldItem && !Taken.Contains(OldItem) && WbTaskKey(*OldItem).Equals(NewKey, ESearchCase::CaseSensitive))
									{
										CopyTrivia(File, OldItem, NewItem.Get());
										Taken.Add(OldItem);
										bDone = true;
									}
								}
							}
						}
					}
				}
				NewBody.Add(MoveTemp(Fresh));
			}
			BuryAll(Originals);
		};

		KeepOrReplace(OnDecision, Parts.Ons);
		KeepOrReplace(IfDecision, Parts.Ifs);
		KeepOrReplace(PickDecision, Parts.Picks);
		KeepOrReplace(UtilityDecision, Parts.Utilities);
		KeepOrReplace(LinkDecision, Parts.Links);
		KeepOrReplace(DoDecision, Parts.Dos);

		// -- The transitions, in the asset's order: the ones that say the same keep their text.
		for (int32 Index = 0; Index < AState.Transitions.Num(); ++Index)
		{
			const int32 Counterpart = TransitionMatch[Index];
			Lang::FDstStmtPtr* Old = (Counterpart != INDEX_NONE && Parts.Transitions.IsValidIndex(Counterpart) && Parts.Transitions[Counterpart])
				? &Parts.Transitions[Counterpart] : nullptr;
			if (Old && Same(DumpTransitionOf(AState.Transitions[Index]), DumpTransitionOf(TState.Transitions[Counterpart])))
			{
				NewBody.Add(MoveTemp(*Old));
				continue;
			}
			const Lang::FDstClauseStmt* Wanted = WbFindClause(*Card, Lang::EDstClauseKind::Transition, Index);
			if (!Wanted)
			{
				continue;
			}
			Lang::FDstStmtPtr Fresh = Lang::CloneStmt(*Wanted);
			if (Old)
			{
				CopyTrivia(File, Old->Get(), Fresh.Get());
			}
			NewBody.Add(MoveTemp(Fresh));
		}
		BuryAll(Parts.Transitions);

		// -- The head: name, signature, settings.
		Stmt.Name = AState.Name;
		Stmt.bNameQuoted = Card->bNameQuoted;
		if (SettingsDiffer(AState, TState))
		{
			Edit->Bury(MoveTemp(Stmt.Meta));
			if (Card->Meta.IsValid() && !Card->Meta->Entries.IsEmpty())
			{
				Stmt.Meta = Lang::CloneMeta(*Card->Meta);
			}
		}
		if (!Same(ParamsKeyOf(AState), ParamsKeyOf(TState)))
		{
			for (TUniquePtr<Lang::FDstParam>& Param : Stmt.Params)
			{
				Edit->Bury(MoveTemp(Param));
			}
			Stmt.Params.Reset();
			for (const TUniquePtr<Lang::FDstParam>& Param : Card->Params)
			{
				if (Param)
				{
					Stmt.Params.Add(Lang::CloneParam(*Param));
				}
			}
			Stmt.bHasParams = Card->bHasParams;
		}

		// -- The children: a card that is still there goes back as it was written, the others are printed.
		TArray<FKeptCard> Kept;
		for (const Model::FDstState& AChild : AState.Children)
		{
			const FString* TPath = AToT.Find(AChild.Path);
			Lang::FDstStateStmt* OldChild = TPath ? FindStateStmt(*TPath) : nullptr;
			const Model::FDstState* TChild = TPath ? T.FindState(*TPath) : nullptr;
			if (!OldChild || !TChild)
			{
				NewBody.Add(Lang::CloneStmt(*FindRaisedState(AChild.Path)));
				continue;
			}

			TArray<FEdit> ChildEdits;
			PlanState(AChild, *TChild, ChildEdits);

			FKeptCard Entry;
			Entry.Name = FString::Printf(TEXT("__DstKeep%d__"), NextKeep++);
			Entry.Text = SliceWithEdits(Edit->GetText(), OldChild->Span, ChildEdits);
			Entry.OldIndentChars = Edit->GetIndentChars(OldChild->Span.Offset);

			TUniquePtr<Lang::FDstStateStmt> Holder = MakeUnique<Lang::FDstStateStmt>();
			Holder->Name = Entry.Name;
			if (const Lang::FDstTrivia* Found = File.FindTrivia(OldChild))
			{
				// The comments above the card and the blank lines before it; what was inside it comes with its text.
				Lang::FDstTrivia Copy = *Found;
				Copy.Inner.Reset();
				File.Trivia.FindOrAdd(Holder.Get()) = MoveTemp(Copy);
			}
			NewBody.Add(MoveTemp(Holder));
			Kept.Add(MoveTemp(Entry));
		}
		BuryAll(Parts.Children);

		Stmt.Body = MoveTemp(NewBody);

		// -- Print, then put the kept cards in.
		FString Printed = Edit->PrintStmt(Stmt, Edit->GetIndentLevel(CardSpan.Offset));
		for (const FKeptCard& Keep : Kept)
		{
			const FString Marker = TEXT("State ") + Keep.Name;
			const int32 At = Printed.Find(*Marker, ESearchCase::CaseSensitive);
			const int32 Open = At == INDEX_NONE ? INDEX_NONE : Printed.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
			const int32 Close = Open == INDEX_NONE ? INDEX_NONE : Printed.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
			if (Close == INDEX_NONE)
			{
				return false;
			}
			int32 LineBegin = At;
			while (LineBegin > 0 && Printed[LineBegin - 1] != TEXT('\n'))
			{
				--LineBegin;
			}
			const FString NewIndent = Printed.Mid(LineBegin, At - LineBegin);
			Printed = Printed.Left(At) + ReindentBlock(Keep.Text, Keep.OldIndentChars, NewIndent) + Printed.Mid(Close + 1);
		}

		(void)TState;
		OutText = MoveTemp(Printed);
		return true;
	}
}
