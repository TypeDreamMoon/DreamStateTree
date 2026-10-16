// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureOps.h"

#include "Spike/SpikeDump.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeDelegates.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeViewModel.h"

namespace UE::DreamStateTree::Spike
{
	namespace
	{
		const TCHAR* DstYesNo(bool bValue)
		{
			return bValue ? TEXT("yes") : TEXT("no");
		}

		bool DstNeed(FString& Note, const void* Pointer, const TCHAR* What)
		{
			if (!Pointer)
			{
				Note = FString::Printf(TEXT("%s is not in the tree (an earlier operation of the cumulative run changed the baseline)."), What);
				return false;
			}
			return true;
		}

		bool DstNeedIndex(FString& Note, int32 Index, const TCHAR* What)
		{
			return DstNeed(Note, Index != INDEX_NONE ? &Index : nullptr, What);
		}

		int32 DstIndexOfNode(const TArray<FStateTreeEditorNode>& Nodes, const FGuid& Id)
		{
			return Nodes.IndexOfByPredicate([&Id](const FStateTreeEditorNode& Node)
			{
				return Node.ID == Id;
			});
		}

		int32 DstIndexOfTransition(const TArray<FStateTreeTransition>& Transitions, const FGuid& Id)
		{
			return Transitions.IndexOfByPredicate([&Id](const FStateTreeTransition& Transition)
			{
				return Transition.ID == Id;
			});
		}

		FString DstIndent(const FString& Text, const TCHAR* Prefix)
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
			FString Out;
			for (const FString& Line : Lines)
			{
				Out += Prefix + Line + TEXT("\n");
			}
			Out.TrimEndInline();
			return Out;
		}

		/** Edit for a member of the state itself, e.g. Name, SelectionBehavior. */
		bool DstEditStateMember(UStateTreeState& State, const TCHAR* PropertyName, const TCHAR* Title, FString& Note, TFunctionRef<void()> Mutate)
		{
			FProperty* Property = DstFindProperty(UStateTreeState::StaticClass(), PropertyName);
			if (!Property)
			{
				Note = FString::Printf(TEXT("UStateTreeState has no property '%s' in this engine version."), PropertyName);
				return false;
			}
			FDstPropertyEdit Edit;
			Edit.Owner = &State;
			Edit.Chain = { Property };
			Edit.TransactionText = FText::FromString(Title);
			DstApplyPropertyEdit(Edit, Mutate);
			return true;
		}

		/** Edit for a member of one element of a state's transition array, e.g. Transitions[0].Priority. */
		bool DstEditTransitionMember(UStateTreeState& State, int32 Index, const TCHAR* MemberName, const TCHAR* Title, FString& Note, TFunctionRef<void()> Mutate)
		{
			FProperty* ArrayProperty = DstFindProperty(UStateTreeState::StaticClass(), TEXT("Transitions"));
			const FArrayProperty* AsArray = CastField<FArrayProperty>(ArrayProperty);
			const FStructProperty* Inner = AsArray ? CastField<FStructProperty>(AsArray->Inner) : nullptr;
			FProperty* Member = Inner ? DstFindProperty(Inner->Struct, MemberName) : nullptr;
			if (!ArrayProperty || !Member)
			{
				Note = FString::Printf(TEXT("FStateTreeTransition has no property '%s' in this engine version."), MemberName);
				return false;
			}
			FDstPropertyEdit Edit;
			Edit.Owner = &State;
			Edit.Chain = { ArrayProperty, Member };
			Edit.ArrayIndices.Add(TEXT("Transitions"), Index);
			Edit.TransactionText = FText::FromString(Title);
			DstApplyPropertyEdit(Edit, Mutate);
			return true;
		}

		/** Edit for a member of one element of a node array (Tasks, EnterConditions), e.g. EnterConditions[1].ExpressionOperand. */
		bool DstEditNodeMember(UStateTreeState& State, const TCHAR* ArrayName, int32 Index, const TCHAR* MemberName, const TCHAR* Title, FString& Note, TFunctionRef<void()> Mutate)
		{
			FProperty* ArrayProperty = DstFindProperty(UStateTreeState::StaticClass(), ArrayName);
			FProperty* Member = DstFindProperty(DstGetEditorNodeStruct(), MemberName);
			if (!ArrayProperty || !Member)
			{
				Note = FString::Printf(TEXT("Could not find %s or FStateTreeEditorNode::%s."), ArrayName, MemberName);
				return false;
			}
			FDstPropertyEdit Edit;
			Edit.Owner = &State;
			Edit.Chain = { ArrayProperty, Member };
			Edit.ArrayIndices.Add(ArrayName, Index);
			Edit.TransactionText = FText::FromString(Title);
			DstApplyPropertyEdit(Edit, Mutate);
			return true;
		}

		/** Edit for a value of a node's instance data, e.g. Tasks[0].Instance.Duration. */
		bool DstEditNodeInstanceValue(UStateTreeState& State, const TCHAR* ArrayName, int32 Index, FStateTreeEditorNode& Node, const TCHAR* ValueName, const TCHAR* Title, FString& Note, TFunctionRef<void()> Mutate)
		{
			FProperty* ArrayProperty = DstFindProperty(UStateTreeState::StaticClass(), ArrayName);
			FProperty* InstanceProperty = DstFindProperty(DstGetEditorNodeStruct(), TEXT("Instance"));
			FProperty* ValueProperty = DstFindProperty(Node.Instance.GetScriptStruct(), ValueName);
			if (!ArrayProperty || !InstanceProperty || !ValueProperty)
			{
				Note = FString::Printf(TEXT("Could not find %s.Instance.%s."), ArrayName, ValueName);
				return false;
			}
			FDstPropertyEdit Edit;
			Edit.Owner = &State;
			Edit.Chain = { ArrayProperty, InstanceProperty, ValueProperty };
			Edit.ArrayIndices.Add(ArrayName, Index);
			Edit.TransactionText = FText::FromString(Title);
			DstApplyPropertyEdit(Edit, Mutate);
			return true;
		}

		/** Adds a default element to a node array and gives it a type, in one undo step: the two property edits the UI performs. */
		bool DstAddNodeLikeDetails(UStateTreeState& State, const TCHAR* ArrayName, UScriptStruct* NodeStruct, const TCHAR* NodeName, FString& Note)
		{
			FProperty* ArrayProperty = DstFindProperty(UStateTreeState::StaticClass(), ArrayName);
			TArray<FStateTreeEditorNode>* Array = nullptr;
			if (FCString::Strcmp(ArrayName, TEXT("Tasks")) == 0)
			{
				Array = &State.Tasks;
			}
			else if (FCString::Strcmp(ArrayName, TEXT("EnterConditions")) == 0)
			{
				Array = &State.EnterConditions;
			}
			if (!ArrayProperty || !Array || !NodeStruct)
			{
				Note = FString::Printf(TEXT("Cannot add a node to %s: property or node struct missing."), ArrayName);
				return false;
			}

			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Add %s node"), ArrayName)));
			const int32 NewIndex = Array->Num();

			FDstPropertyEdit AddEdit;
			AddEdit.Owner = &State;
			AddEdit.Chain = { ArrayProperty };
			AddEdit.ArrayIndices.Add(ArrayName, NewIndex);
			AddEdit.ChangeType = EPropertyChangeType::ArrayAdd;
			DstApplyPropertyEdit(AddEdit, [Array]()
			{
				Array->AddDefaulted();
			});

			FDstPropertyEdit TypeEdit = AddEdit;
			TypeEdit.ChangeType = EPropertyChangeType::ValueSet;
			DstApplyPropertyEdit(TypeEdit, [Array, NewIndex, &State, NodeStruct, NodeName]()
			{
				FStateTreeEditorNode& Node = (*Array)[NewIndex];
				Node.InitializeAs(&State, NodeStruct);
				Node.SetNodeName(FName(NodeName));
			});
			return true;
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// View model operations
	// ------------------------------------------------------------------------------------------------------------

	TArray<FDstCaptureOp> MakeViewModelOps()
	{
		TArray<FDstCaptureOp> Ops;
		auto Add = [&Ops](const TCHAR* Name, TFunction<bool(FDstCaptureFixture&, FString&)> Run)
		{
			FDstCaptureOp& Op = Ops.AddDefaulted_GetRef();
			Op.Name = Name;
			Op.Run = MoveTemp(Run);
		};

		Add(TEXT("VM.AddState (after B)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			ViewModel->AddState(B);
			return true;
		});

		Add(TEXT("VM.AddChildState (under A1)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A1 = F.FindState(F.Ids.A1);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A1, TEXT("state A1")))
			{
				return false;
			}
			ViewModel->AddChildState(A1);
			return true;
		});

		Add(TEXT("VM.RenameState (A1 -> A1_Renamed)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A1 = F.FindState(F.Ids.A1);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A1, TEXT("state A1")))
			{
				return false;
			}
			ViewModel->RenameState(A1, FName(TEXT("A1_Renamed")));
			return true;
		});

		// The outliner has no "add task" call on the view model: tasks and conditions are added through the Details
		// panel's array and node picker, two property edits in one undo step. Included here because the outliner
		// session needs them.
		Add(TEXT("UI: add task (B gets a Debug Text task from the node picker)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* B = F.FindState(F.Ids.B);
			UScriptStruct* DebugText = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeDebugTextTask"));
			if (!DstNeed(Note, B, TEXT("state B")) || !DstNeed(Note, DebugText, TEXT("the Debug Text task struct")))
			{
				return false;
			}
			return DstAddNodeLikeDetails(*B, TEXT("Tasks"), DebugText, TEXT("Say"), Note);
		});

		Add(TEXT("UI: add enter condition (B gets a Random condition from the node picker)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* B = F.FindState(F.Ids.B);
			UScriptStruct* Random = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeRandomCondition"));
			if (!DstNeed(Note, B, TEXT("state B")) || !DstNeed(Note, Random, TEXT("the Random condition struct")))
			{
				return false;
			}
			return DstAddNodeLikeDetails(*B, TEXT("EnterConditions"), Random, TEXT("Chance"), Note);
		});

		Add(TEXT("VM.SetSelectedStatesEnabled (B off)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			ViewModel->SetSelection(B);
			ViewModel->SetSelectedStatesEnabled(false);
			return true;
		});

		Add(TEXT("VM.AddTransition (B -> A on succeeded)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			ViewModel->AddTransition(B, EStateTreeTransitionTrigger::OnStateSucceeded, EStateTreeTransitionType::GotoState, A);
			return true;
		});

		Add(TEXT("VM.RemoveTransition (A's transition)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, F.FindTransition(F.Ids.A, F.Ids.ATransition), TEXT("A's transition")))
			{
				return false;
			}
			ViewModel->RemoveTransition(A, F.Ids.ATransition);
			return true;
		});

		Add(TEXT("VM.DuplicateNode (A's task)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, F.FindNode(F.Ids.A, F.Ids.ATask), TEXT("A's task")))
			{
				return false;
			}
			ViewModel->DuplicateNode(A, F.Ids.ATask);
			return true;
		});

		Add(TEXT("VM.MoveNode (A's task -> B)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, B, TEXT("state B")) || !DstNeed(Note, F.FindNode(F.Ids.A, F.Ids.ATask), TEXT("A's task")))
			{
				return false;
			}
			ViewModel->MoveNode(A, B, F.Ids.ATask);
			return true;
		});

		Add(TEXT("VM.DeleteNode (B's task)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, B, TEXT("state B")) || !DstNeed(Note, F.FindNode(F.Ids.B, F.Ids.BTask), TEXT("B's task")))
			{
				return false;
			}
			ViewModel->DeleteNode(B, F.Ids.BTask);
			return true;
		});

		Add(TEXT("VM.DuplicateSelectedStates (A)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			ViewModel->SetSelection(A);
			ViewModel->DuplicateSelectedStates();
			return true;
		});

		Add(TEXT("VM.CopySelectedStates + PasteStatesFromClipboard (A1 after B)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A1 = F.FindState(F.Ids.A1);
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A1, TEXT("state A1")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			// The operation uses the system clipboard; keep the user's text.
			FDstClipboardGuard ClipboardGuard;
			ViewModel->SetSelection(A1);
			ViewModel->CopySelectedStates();
			const bool bCanPaste = ViewModel->CanPasteStatesFromClipboard();
			Note = FString::Printf(TEXT("CanPasteStatesFromClipboard after CopySelectedStates = %s (yes means the system clipboard works without a UI)"), DstYesNo(bCanPaste));
			if (bCanPaste)
			{
				ViewModel->PasteStatesFromClipboard(B);
			}
			return true;
		});

		Add(TEXT("VM.MoveSelectedStatesAfter (A after B)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			ViewModel->SetSelection(A);
			ViewModel->MoveSelectedStatesAfter(B);
			return true;
		});

		Add(TEXT("VM.MoveSelectedStatesInto (B into A)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			ViewModel->SetSelection(B);
			ViewModel->MoveSelectedStatesInto(A);
			return true;
		});

		Add(TEXT("VM.MoveSelectedStatesBefore (B before A)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			ViewModel->SetSelection(B);
			ViewModel->MoveSelectedStatesBefore(A);
			return true;
		});

		Add(TEXT("VM.RemoveSelectedStates (A1)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A1 = F.FindState(F.Ids.A1);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A1, TEXT("state A1")))
			{
				return false;
			}
			ViewModel->SetSelection(A1);
			ViewModel->RemoveSelectedStates();
			return true;
		});

		Add(TEXT("VM.RemoveSelectedStates (A with its tasks and conditions)"), [](FDstCaptureFixture& F, FString& Note)
		{
			const TSharedPtr<FStateTreeViewModel> ViewModel = F.GetOrCreateViewModel();
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, ViewModel.Get(), TEXT("view model")) || !DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			ViewModel->SetSelection(A);
			ViewModel->RemoveSelectedStates();
			return true;
		});

		return Ops;
	}

	// ------------------------------------------------------------------------------------------------------------
	// Details panel operations
	// ------------------------------------------------------------------------------------------------------------

	TArray<FDstCaptureOp> MakeDetailsOps()
	{
		TArray<FDstCaptureOp> Ops;
		auto Add = [&Ops](const TCHAR* Name, TFunction<bool(FDstCaptureFixture&, FString&)> Run)
		{
			FDstCaptureOp& Op = Ops.AddDefaulted_GetRef();
			Op.Name = Name;
			Op.Run = MoveTemp(Run);
		};

		Add(TEXT("Details: task instance value (A.WaitA Duration = 3.5)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			const int32 Index = DstIndexOfNode(A->Tasks, F.Ids.ATask);
			if (!DstNeedIndex(Note, Index, TEXT("A's task")))
			{
				return false;
			}
			FStateTreeEditorNode& Task = A->Tasks[Index];
			return DstEditNodeInstanceValue(*A, TEXT("Tasks"), Index, Task, TEXT("Duration"), TEXT("Set Duration"), Note, [&Task]()
			{
				DstSetFloat(Task.Instance.GetScriptStruct(), Task.Instance.GetMutableMemory(), TEXT("Duration"), 3.5f);
			});
		});

		Add(TEXT("Details: task node name (WaitA -> WaitA_Renamed)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			const int32 Index = DstIndexOfNode(A->Tasks, F.Ids.ATask);
			if (!DstNeedIndex(Note, Index, TEXT("A's task")))
			{
				return false;
			}
			FProperty* ArrayProperty = DstFindProperty(UStateTreeState::StaticClass(), TEXT("Tasks"));
			if (!ArrayProperty)
			{
				Note = TEXT("UStateTreeState has no Tasks property.");
				return false;
			}
			FStateTreeEditorNode& Task = A->Tasks[Index];
			FDstPropertyEdit Edit;
			Edit.Owner = A;
			Edit.Chain = { ArrayProperty };
			Edit.ArrayIndices.Add(TEXT("Tasks"), Index);
			Edit.TransactionText = FText::FromString(TEXT("Set Name"));
			DstApplyPropertyEdit(Edit, [&Task]()
			{
				Task.SetNodeName(FName(TEXT("WaitA_Renamed")));
			});
			// The node name editor announces the change itself; the property edit alone does not.
			UE::StateTree::Delegates::OnIdentifierChanged.Broadcast(*F.GetTree());
			Note = TEXT("The UI broadcasts OnIdentifierChanged by hand after a node rename; replicated here.");
			return true;
		});

		Add(TEXT("Details: state name (A -> A_FromDetails)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			return DstEditStateMember(*A, TEXT("Name"), TEXT("Set State Name"), Note, [A]()
			{
				A->Name = FName(TEXT("A_FromDetails"));
			});
		});

		Add(TEXT("Details: state SelectionBehavior (Root -> TrySelectChildrenAtRandom)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* Root = F.FindState(F.Ids.Root);
			if (!DstNeed(Note, Root, TEXT("the root state")))
			{
				return false;
			}
			return DstEditStateMember(*Root, TEXT("SelectionBehavior"), TEXT("Set Selection Behavior"), Note, [Root]()
			{
				Root->SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom;
			});
		});

		Add(TEXT("Details: state enabled flag (B off)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			return DstEditStateMember(*B, TEXT("bEnabled"), TEXT("Set Enabled"), Note, [B]()
			{
				B->bEnabled = false;
			});
		});

		Add(TEXT("Details: state description (A)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			return DstEditStateMember(*A, TEXT("Description"), TEXT("Set Description"), Note, [A]()
			{
				A->Description = TEXT("Edited from the details panel");
			});
		});

		Add(TEXT("Details: transition Priority (A's transition -> High)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			const int32 Index = DstIndexOfTransition(A->Transitions, F.Ids.ATransition);
			if (!DstNeedIndex(Note, Index, TEXT("A's transition")))
			{
				return false;
			}
			return DstEditTransitionMember(*A, Index, TEXT("Priority"), TEXT("Set Priority"), Note, [A, Index]()
			{
				A->Transitions[Index].Priority = EStateTreeTransitionPriority::High;
			});
		});

		Add(TEXT("Details: transition Trigger (A's transition -> OnTick)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			const int32 Index = DstIndexOfTransition(A->Transitions, F.Ids.ATransition);
			if (!DstNeedIndex(Note, Index, TEXT("A's transition")))
			{
				return false;
			}
			return DstEditTransitionMember(*A, Index, TEXT("Trigger"), TEXT("Set Trigger"), Note, [A, Index]()
			{
				A->Transitions[Index].Trigger = EStateTreeTransitionTrigger::OnTick;
			});
		});

		Add(TEXT("Details: transition target (A's transition -> B)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, A, TEXT("state A")) || !DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			const int32 Index = DstIndexOfTransition(A->Transitions, F.Ids.ATransition);
			if (!DstNeedIndex(Note, Index, TEXT("A's transition")))
			{
				return false;
			}
			return DstEditTransitionMember(*A, Index, TEXT("State"), TEXT("Set Transition Target"), Note, [A, B, Index]()
			{
				A->Transitions[Index].State = B->GetLinkToState();
			});
		});

		Add(TEXT("Details: enter condition Operand (A's second condition -> Or)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			const int32 Index = DstIndexOfNode(A->EnterConditions, F.Ids.ACondSecond);
			if (!DstNeedIndex(Note, Index, TEXT("A's second enter condition")))
			{
				return false;
			}
			return DstEditNodeMember(*A, TEXT("EnterConditions"), Index, TEXT("ExpressionOperand"), TEXT("Set Operand"), Note, [A, Index]()
			{
				A->EnterConditions[Index].ExpressionOperand = EStateTreeExpressionOperand::Or;
			});
		});

		Add(TEXT("Details: enter condition value (A's first condition Threshold = 0.9)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* A = F.FindState(F.Ids.A);
			if (!DstNeed(Note, A, TEXT("state A")))
			{
				return false;
			}
			const int32 Index = DstIndexOfNode(A->EnterConditions, F.Ids.ACondFirst);
			if (!DstNeedIndex(Note, Index, TEXT("A's first enter condition")))
			{
				return false;
			}
			FStateTreeEditorNode& Condition = A->EnterConditions[Index];
			return DstEditNodeInstanceValue(*A, TEXT("EnterConditions"), Index, Condition, TEXT("Threshold"), TEXT("Set Threshold"), Note, [&Condition]()
			{
				DstSetFloat(Condition.Instance.GetScriptStruct(), Condition.Instance.GetMutableMemory(), TEXT("Threshold"), 0.9f);
			});
		});

		Add(TEXT("Details: add task (B gets a Debug Text task)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* B = F.FindState(F.Ids.B);
			UScriptStruct* DebugText = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeDebugTextTask"));
			if (!DstNeed(Note, B, TEXT("state B")) || !DstNeed(Note, DebugText, TEXT("the Debug Text task struct")))
			{
				return false;
			}
			return DstAddNodeLikeDetails(*B, TEXT("Tasks"), DebugText, TEXT("Say"), Note);
		});

		Add(TEXT("Details: add enter condition (B gets a Random condition)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* B = F.FindState(F.Ids.B);
			UScriptStruct* Random = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeRandomCondition"));
			if (!DstNeed(Note, B, TEXT("state B")) || !DstNeed(Note, Random, TEXT("the Random condition struct")))
			{
				return false;
			}
			return DstAddNodeLikeDetails(*B, TEXT("EnterConditions"), Random, TEXT("Chance"), Note);
		});

		Add(TEXT("Details: add property binding (Speed -> B task RandomDeviation)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeEditorData* EditorData = F.GetEditorData();
			FStateTreeEditorNode* BTask = F.FindNode(F.Ids.B, F.Ids.BTask);
			if (!DstNeed(Note, EditorData, TEXT("editor data")) || !DstNeed(Note, BTask, TEXT("B's task")))
			{
				return false;
			}
			// What the binding widget does: transaction, Modify on the owner, add to the collection, tell the owner.
			FScopedTransaction Transaction(FText::FromString(TEXT("Add Binding")));
			EditorData->Modify();
			const FPropertyBindingPath SourcePath(EditorData->GetRootParametersGuid(), FDstCaptureFixture::SpeedParameterName);
			const FPropertyBindingPath TargetPath(BTask->ID, FName(TEXT("RandomDeviation")));
			EditorData->AddPropertyBinding(SourcePath, TargetPath);
			EditorData->OnPropertyBindingChanged(SourcePath, TargetPath);
			return true;
		});

		Add(TEXT("Details: remove property binding (Speed -> A task Duration)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeEditorData* EditorData = F.GetEditorData();
			if (!DstNeed(Note, EditorData, TEXT("editor data")) || !DstNeed(Note, F.FindNode(F.Ids.A, F.Ids.ATask), TEXT("A's task")))
			{
				return false;
			}
			FScopedTransaction Transaction(FText::FromString(TEXT("Remove Binding")));
			EditorData->Modify();
			const FPropertyBindingPath TargetPath(F.Ids.ATask, FName(TEXT("Duration")));
			EditorData->RemovePropertyBinding(TargetPath);
			EditorData->OnPropertyBindingChanged(FPropertyBindingPath(), TargetPath);
			return true;
		});

		Add(TEXT("Details: root parameter default (Speed = 5)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeEditorData* EditorData = F.GetEditorData();
			if (!DstNeed(Note, EditorData, TEXT("editor data")))
			{
				return false;
			}
			FInstancedPropertyBag* Bag = DstGetRootParameterBag(*EditorData);
			FProperty* BagProperty = DstFindProperty(UStateTreeEditorData::StaticClass(), TEXT("RootParameterPropertyBag"));
			const FStructProperty* BagStructProperty = CastField<FStructProperty>(BagProperty);
			if (!DstNeed(Note, Bag, TEXT("the root parameter bag")) || !DstNeed(Note, BagStructProperty, TEXT("the RootParameterPropertyBag property")))
			{
				return false;
			}
			// The property editor's chain for a value inside a property bag: bag, its Value, the parameter.
			FDstPropertyEdit Edit;
			Edit.Owner = EditorData;
			Edit.Chain = { BagProperty };
			if (FProperty* ValueProperty = DstFindProperty(BagStructProperty->Struct, TEXT("Value")))
			{
				Edit.Chain.Add(ValueProperty);
				if (FProperty* ParameterProperty = DstFindProperty(Bag->GetPropertyBagStruct(), *FDstCaptureFixture::SpeedParameterName.ToString()))
				{
					Edit.Chain.Add(ParameterProperty);
				}
			}
			Note = FString::Printf(TEXT("property chain length used: %d (3 = bag.Value.Speed)"), Edit.Chain.Num());
			Edit.TransactionText = FText::FromString(TEXT("Set Parameter Default"));
			DstApplyPropertyEdit(Edit, [Bag]()
			{
				(void)Bag->SetValueFloat(FDstCaptureFixture::SpeedParameterName, 5.0f);
			});
			return true;
		});

		Add(TEXT("Details: add root parameter (Gravity)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeEditorData* EditorData = F.GetEditorData();
			if (!DstNeed(Note, EditorData, TEXT("editor data")))
			{
				return false;
			}
			FInstancedPropertyBag* Bag = DstGetRootParameterBag(*EditorData);
			FProperty* BagProperty = DstFindProperty(UStateTreeEditorData::StaticClass(), TEXT("RootParameterPropertyBag"));
			if (!DstNeed(Note, Bag, TEXT("the root parameter bag")) || !DstNeed(Note, BagProperty, TEXT("the RootParameterPropertyBag property")))
			{
				return false;
			}
			FDstPropertyEdit Edit;
			Edit.Owner = EditorData;
			Edit.Chain = { BagProperty };
			Edit.TransactionText = FText::FromString(TEXT("Add Parameter"));
			DstApplyPropertyEdit(Edit, [Bag]()
			{
				(void)Bag->AddProperty(FName(TEXT("Gravity")), EPropertyBagPropertyType::Float);
			});
			return true;
		});

		Add(TEXT("Details: add state parameter (B gets Mood)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* B = F.FindState(F.Ids.B);
			if (!DstNeed(Note, B, TEXT("state B")))
			{
				return false;
			}
			return DstEditStateMember(*B, TEXT("Parameters"), TEXT("Add State Parameter"), Note, [B]()
			{
				(void)B->Parameters.Parameters.AddProperty(FName(TEXT("Mood")), EPropertyBagPropertyType::Bool);
			});
		});

		Add(TEXT("Details: add global task (editor data GlobalTasks)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeEditorData* EditorData = F.GetEditorData();
			UScriptStruct* DebugText = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeDebugTextTask"));
			FProperty* ArrayProperty = DstFindProperty(UStateTreeEditorData::StaticClass(), TEXT("GlobalTasks"));
			if (!DstNeed(Note, EditorData, TEXT("editor data")) || !DstNeed(Note, DebugText, TEXT("the Debug Text task struct")) || !DstNeed(Note, ArrayProperty, TEXT("the GlobalTasks property")))
			{
				return false;
			}
			FScopedTransaction Transaction(FText::FromString(TEXT("Add Global Task")));
			const int32 NewIndex = EditorData->GlobalTasks.Num();

			FDstPropertyEdit AddEdit;
			AddEdit.Owner = EditorData;
			AddEdit.Chain = { ArrayProperty };
			AddEdit.ArrayIndices.Add(TEXT("GlobalTasks"), NewIndex);
			AddEdit.ChangeType = EPropertyChangeType::ArrayAdd;
			DstApplyPropertyEdit(AddEdit, [EditorData]()
			{
				EditorData->GlobalTasks.AddDefaulted();
			});

			FDstPropertyEdit TypeEdit = AddEdit;
			TypeEdit.ChangeType = EPropertyChangeType::ValueSet;
			DstApplyPropertyEdit(TypeEdit, [EditorData, NewIndex, DebugText]()
			{
				FStateTreeEditorNode& Node = EditorData->GlobalTasks[NewIndex];
				Node.InitializeAs(EditorData, DebugText);
				Node.SetNodeName(FName(TEXT("GlobalSay")));
			});
			return true;
		});

		Add(TEXT("Details: add color (editor data Colors)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeEditorData* EditorData = F.GetEditorData();
			FProperty* ColorsProperty = DstFindProperty(UStateTreeEditorData::StaticClass(), TEXT("Colors"));
			if (!DstNeed(Note, EditorData, TEXT("editor data")) || !DstNeed(Note, ColorsProperty, TEXT("the Colors property")))
			{
				return false;
			}
			FDstPropertyEdit Edit;
			Edit.Owner = EditorData;
			Edit.Chain = { ColorsProperty };
			Edit.ChangeType = EPropertyChangeType::ArrayAdd;
			Edit.TransactionText = FText::FromString(TEXT("Add Color"));
			DstApplyPropertyEdit(Edit, [EditorData]()
			{
				FStateTreeEditorColor NewColor;
				NewColor.DisplayName = TEXT("Spike Red");
				NewColor.Color = FLinearColor(0.8f, 0.1f, 0.1f);
				EditorData->Colors.Add(NewColor);
			});
			return true;
		});

		// Last: later operations of a cumulative run still need B's task.
		Add(TEXT("Details: delete task (B's task, array remove)"), [](FDstCaptureFixture& F, FString& Note)
		{
			UStateTreeState* B = F.FindState(F.Ids.B);
			UStateTreeEditorData* EditorData = F.GetEditorData();
			if (!DstNeed(Note, B, TEXT("state B")) || !DstNeed(Note, EditorData, TEXT("editor data")))
			{
				return false;
			}
			const int32 Index = DstIndexOfNode(B->Tasks, F.Ids.BTask);
			FProperty* ArrayProperty = DstFindProperty(UStateTreeState::StaticClass(), TEXT("Tasks"));
			if (!DstNeedIndex(Note, Index, TEXT("B's task")) || !DstNeed(Note, ArrayProperty, TEXT("the Tasks property")))
			{
				return false;
			}
			FDstPropertyEdit Edit;
			Edit.Owner = B;
			Edit.Chain = { ArrayProperty };
			Edit.ArrayIndices.Add(TEXT("Tasks"), Index);
			Edit.ChangeType = EPropertyChangeType::ArrayRemove;
			Edit.TransactionText = FText::FromString(TEXT("Delete Task"));
			DstApplyPropertyEdit(Edit, [B, EditorData, Index]()
			{
				// The UI's delete also touches the editor data because bindings can disappear with the node.
				EditorData->Modify();
				B->Tasks.RemoveAt(Index);
				EditorData->RemoveInvalidBindings();
			});
			return true;
		});

		return Ops;
	}

	// ------------------------------------------------------------------------------------------------------------
	// Running and reporting
	// ------------------------------------------------------------------------------------------------------------

	FDstOpRecord DstRunOp(FDstCaptureFixture& Fixture, FDstCaptureRecorder& Recorder, const FDstCaptureOp& Op)
	{
		FDstOpRecord Record;
		Record.Name = Op.Name;

		UStateTree* Tree = Fixture.GetTree();
		UPackage* Package = Fixture.GetPackage();

		// An empty undo buffer makes "records added by this operation" a plain count. It also drops the redo records
		// an earlier undo left behind, which the next transaction would otherwise silently truncate.
		if (GEditor)
		{
			GEditor->ResetTransaction(FText::FromString(TEXT("DreamStateTree capture spike: next operation")));
		}

		Record.PreDump = Fixture.Dump();
		const uint32 HashBefore = Fixture.EditorDataHash();
		const int32 QueueBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}

		Record.Segment = Recorder.BeginSegment(Op.Name);
		Record.bRan = Op.Run(Fixture, Record.Note);

		const int32 QueueAfter = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
		Record.NewTransactions = QueueAfter - QueueBefore;
		Record.PostDump = Fixture.Dump();
		Record.bDataChanged = Record.PreDump != Record.PostDump;
		Record.bHashChanged = HashBefore != Fixture.EditorDataHash();
		Record.bTreeDirtyAfter = Tree && Tree->IsEditorDataDirty();
		Record.bNeedsRecompileAfter = Tree && UStateTreeEditingSubsystem::NeedsRecompile(Tree);
		Record.bPackageDirtyAfter = Package && Package->IsDirty();
		if (Record.bDataChanged)
		{
			Record.DataDiff = DiffLines(Record.PreDump, Record.PostDump, 8);
		}
		return Record;
	}

	void DstQuietCompile(FDstCaptureFixture& Fixture, FDstCaptureRecorder& Recorder)
	{
		Recorder.SetPaused(true);
		FString Messages;
		Fixture.Compile(Messages);
		Recorder.SetPaused(false);
	}

	FString DstFormatOpRecord(const FDstOpRecord& Record, const FDstCaptureRecorder& Recorder)
	{
		FString Out = FString::Printf(TEXT("[%s]"), *Record.Name);
		if (!Record.bRan)
		{
			Out += FString::Printf(TEXT(" NOT RUN: %s"), *Record.Note);
			return Out;
		}
		Out += FString::Printf(TEXT(" undo-records=%d data-changed=%s engine-hash-changed=%s tree-dirty=%s needs-recompile=%s package-dirty=%s"),
			Record.NewTransactions, DstYesNo(Record.bDataChanged), DstYesNo(Record.bHashChanged), DstYesNo(Record.bTreeDirtyAfter),
			DstYesNo(Record.bNeedsRecompileAfter), DstYesNo(Record.bPackageDirtyAfter));
		if (!Record.Note.IsEmpty())
		{
			Out += TEXT("\n  note: ") + Record.Note;
		}
		Out += TEXT("\n  events:\n") + Recorder.SummarizeSegment(Record.Segment);
		if (!Record.DataDiff.IsEmpty())
		{
			Out += TEXT("\n  data change:\n") + DstIndent(Record.DataDiff, TEXT("    "));
		}
		return Out;
	}

	FString DstFormatCoverageMatrix(const TArray<FDstOpRecord>& Records, const FDstCaptureRecorder& Recorder)
	{
		struct FColumn
		{
			const TCHAR* Header;
			const TCHAR* Kind;
		};
		const FColumn Columns[] =
		{
			{ TEXT("Mod"), TEXT("Modify") },
			{ TEXT("Pre"), TEXT("PreEditChain") },
			{ TEXT("Edit"), TEXT("PostEdit") },
			{ TEXT("Chain"), TEXT("PostEditChain") },
			{ TEXT("Trans"), TEXT("Transacted") },
			{ TEXT("PkgD"), TEXT("PackageDirty") },
			{ TEXT("Ident"), TEXT("ST.IdentifierChanged") },
			{ TEXT("Param"), TEXT("ST.ParametersChanged") },
			{ TEXT("SPar"), TEXT("ST.StateParametersChanged") },
			{ TEXT("Glob"), TEXT("ST.GlobalDataChanged") },
			{ TEXT("Theme"), TEXT("ST.VisualThemeChanged") },
		};

		FString Out = FString::Printf(TEXT("%-64s"), TEXT("Operation"));
		for (const FColumn& Column : Columns)
		{
			Out += FString::Printf(TEXT("%6s"), Column.Header);
		}
		Out += FString::Printf(TEXT("%6s%6s%6s\n"), TEXT("VM.*"), TEXT("Hash"), TEXT("Dirty"));

		for (const FDstOpRecord& Record : Records)
		{
			Out += FString::Printf(TEXT("%-64s"), *Record.Name.Left(63));
			if (!Record.bRan)
			{
				Out += TEXT("  (not run)\n");
				continue;
			}
			for (const FColumn& Column : Columns)
			{
				const int32 Count = Recorder.CountEvents(Record.Segment, Column.Kind);
				Out += FString::Printf(TEXT("%6s"), Count > 0 ? *FString::FromInt(Count) : TEXT("."));
			}
			int32 ViewModelEvents = 0;
			for (const FDstCaptureEvent& Event : Recorder.GetSegmentEvents(Record.Segment))
			{
				ViewModelEvents += Event.Kind.StartsWith(TEXT("VM.")) ? 1 : 0;
			}
			Out += FString::Printf(TEXT("%6s%6s%6s\n"),
				ViewModelEvents > 0 ? *FString::FromInt(ViewModelEvents) : TEXT("."),
				Record.bHashChanged ? TEXT("Y") : TEXT("."),
				Record.bTreeDirtyAfter ? TEXT("Y") : TEXT("."));
		}
		return Out;
	}

	FDstUndoRedoRecord DstRunUndoRedo(FDstCaptureFixture& Fixture, FDstCaptureRecorder& Recorder, const FDstOpRecord& Record)
	{
		FDstUndoRedoRecord Result;
		if (!GEditor || Record.NewTransactions <= 0)
		{
			return Result;
		}

		Recorder.SetPaused(false);
		Result.UndoSegment = Recorder.BeginSegment(Record.Name + TEXT(" [undo]"));
		Result.bUndoReturnedTrue = true;
		for (int32 Step = 0; Step < Record.NewTransactions; ++Step)
		{
			Result.bUndoReturnedTrue &= GEditor->UndoTransaction();
		}
		const FString AfterUndo = Fixture.Dump();
		Result.bUndoRestoredData = AfterUndo == Record.PreDump;
		if (!Result.bUndoRestoredData)
		{
			Result.UndoDiff = DiffLines(Record.PreDump, AfterUndo, 8);
		}

		Result.RedoSegment = Recorder.BeginSegment(Record.Name + TEXT(" [redo]"));
		Result.bRedoReturnedTrue = true;
		for (int32 Step = 0; Step < Record.NewTransactions; ++Step)
		{
			Result.bRedoReturnedTrue &= GEditor->RedoTransaction();
		}
		const FString AfterRedo = Fixture.Dump();
		Result.bRedoRestoredData = AfterRedo == Record.PostDump;
		if (!Result.bRedoRestoredData)
		{
			Result.RedoDiff = DiffLines(Record.PostDump, AfterRedo, 8);
		}

		// Back to the baseline for the next operation; this undo is not part of the report.
		Recorder.SetPaused(true);
		for (int32 Step = 0; Step < Record.NewTransactions; ++Step)
		{
			GEditor->UndoTransaction();
		}
		Recorder.SetPaused(false);
		Result.bBackAtBaseline = Fixture.Dump() == Record.PreDump;
		return Result;
	}
}
