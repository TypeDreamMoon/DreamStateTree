// Copyright DreamMoon. All Rights Reserved.
//
// Write-back: a tree is generated from a small source, changed the way the editor changes it (the editor data is edited
// directly), and the change is written back into the text. Each case starts from a fresh asset; the text must come out
// with just that change in it, keep its comments, and check to what the asset holds.

#include "DstDriver.h"
#include "DstTextMerge.h"
#include "DstWriteBack.h"
#include "Fixtures/DstTestNodes.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamStateTree::Tests::WriteBackTests
{
	namespace Compiler = UE::DreamStateTree::Compiler;

	// ------------------------------------------------------------------------------------------
	// The source every case starts from. It is written the way the formatter writes it (Wbt::MakeBase formats it
	// anyway), with comments in the places a reprint could lose them.
	// ------------------------------------------------------------------------------------------

	const TCHAR* const WbtBodyText = TEXT(
		"{\n"
		"    Parameters = { float Speed = 1; int Count = 2; }\n"
		"\n"
		"    State Root {\n"
		"        -> Work\n"
		"\n"
		"        // Does the work.\n"
		"        State Work {\n"
		"            if DstTest(Left = Params.Speed, Right = 3)\n"
		"            // The task list.\n"
		"            do DstTest(InputValue = Params.Speed, TicksToComplete = 3), Delay(2)\n"
		"            -> Rest on Succeeded\n"
		"        }\n"
		"        State Rest { do Delay(1)  -> Work }\n"
		"    }\n"
		"}\n");

	FString WbtGetSourcePath(const TCHAR* CaseName)
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamStateTreeTests"), TEXT("WriteBack"),
			FString::Printf(TEXT("ST_%s.dtr"), CaseName)));
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	/** A source in the formatter's own form, so that the expectations below can be written against its text. */
	FString WbtMakeBase(FAutomationTestBase& Test, const TCHAR* CaseName, const TCHAR* BodyText, const TCHAR* Prefix)
	{
		const FString Source = FString::Printf(TEXT("%sStateTree(Name=\"DreamStateTreeTests/WriteBack/ST_%s\", Schema=TestAgent)\n%s"),
			Prefix, CaseName, BodyText);
		FString Canonical;
		Lang::FDstDiagnosticSink Sink(WbtGetSourcePath(CaseName));
		if (!Compiler::FormatDstText(WbtGetSourcePath(CaseName), Source, Canonical, Sink))
		{
			Test.AddError(FString::Printf(TEXT("%s: the source does not format\n%s"), CaseName, *Compiler::FormatDstDiagnostics(Sink)));
			return Source;
		}
		return Canonical;
	}

	/** One case: its source, and the asset generated from it. */
	struct FWbtCase
	{
		FAutomationTestBase& Test;
		FString CaseName;
		FString Base;
		UStateTree* Tree = nullptr;

		FWbtCase(FAutomationTestBase& InTest, const TCHAR* InCaseName, const TCHAR* BodyText = WbtBodyText, const TCHAR* Prefix = TEXT(""))
			: Test(InTest)
			, CaseName(InCaseName)
		{
			Base = WbtMakeBase(Test, InCaseName, BodyText, Prefix);

			Compiler::FDstBuildOptions Options;
			Options.bGenerate = true;
			Options.bSave = false;
			const Compiler::FDstFileResult Built = Compiler::BuildDstText(WbtGetSourcePath(InCaseName), Base, Options);
			if (Built.Diagnostics.HasErrors())
			{
				Test.AddError(FString::Printf(TEXT("%s: the source does not build\n%s"), InCaseName, *Compiler::FormatDstDiagnostics(Built.Diagnostics)));
			}
			Tree = Built.Generated.IsSet() ? Built.Generated->Asset : nullptr;
			if (!Tree)
			{
				Test.AddError(FString::Printf(TEXT("%s: nothing was generated"), InCaseName));
			}
		}

		bool IsValid() const { return Tree != nullptr; }

		Compiler::FDstWriteBackResult WriteBack(const Compiler::FDstWriteBackOptions& WriteOptions = Compiler::FDstWriteBackOptions()) const
		{
			return Compiler::WriteBackDstAsset(*Tree, WbtGetSourcePath(*CaseName), Base, WriteOptions);
		}

		UStateTreeEditorData* GetData() const { return Cast<UStateTreeEditorData>(Tree->EditorData); }

		UStateTreeState* FindState(const TCHAR* StateName) const
		{
			UStateTreeState* Found = nullptr;
			if (const UStateTreeEditorData* Data = GetData())
			{
				Data->VisitHierarchy([&Found, StateName](UStateTreeState& Candidate, UStateTreeState*) -> EStateTreeVisitor
				{
					if (Candidate.Name == FName(StateName))
					{
						Found = &Candidate;
						return EStateTreeVisitor::Break;
					}
					return EStateTreeVisitor::Continue;
				});
			}
			if (!Found)
			{
				Test.AddError(FString::Printf(TEXT("%s: no state called %s"), *CaseName, StateName));
			}
			return Found;
		}
	};

	/** Sets a property of a node's instance data by its export text, the way the details panel does. */
	bool WbtSetInstance(FAutomationTestBase& Test, FStateTreeEditorNode& Node, const TCHAR* PropertyName, const TCHAR* ExportText)
	{
		const FStateTreeDataView Instance = Node.GetInstance();
		const FProperty* Property = Instance.GetStruct() ? Instance.GetStruct()->FindPropertyByName(PropertyName) : nullptr;
		if (!Property || !Instance.GetMutableMemory())
		{
			Test.AddError(FString::Printf(TEXT("The node has no instance property %s"), PropertyName));
			return false;
		}
		return Property->ImportText_InContainer(ExportText, Instance.GetMutableMemory(), nullptr, PPF_None, GLog) != nullptr;
	}

	/** Sets a property of the node struct itself (bInvert, ...). */
	bool WbtSetNodeProperty(FAutomationTestBase& Test, FStateTreeEditorNode& Node, const TCHAR* PropertyName, const TCHAR* ExportText)
	{
		const UScriptStruct* Struct = Node.Node.GetScriptStruct();
		const FProperty* Property = Struct ? Struct->FindPropertyByName(PropertyName) : nullptr;
		if (!Property || !Node.Node.GetMutableMemory())
		{
			Test.AddError(FString::Printf(TEXT("The node has no property %s"), PropertyName));
			return false;
		}
		return Property->ImportText_InContainer(ExportText, Node.Node.GetMutableMemory(), nullptr, PPF_None, GLog) != nullptr;
	}

	FInstancedPropertyBag& WbtGetRootBag(const FWbtCase& Case)
	{
		return const_cast<FInstancedPropertyBag&>(Case.GetData()->GetRootParametersPropertyBag());
	}

	FString WbtDescribe(const Compiler::FDstWriteBackResult& Result)
	{
		TArray<FString> Lines;
		for (const Compiler::FDstWriteBackChange& Change : Result.Changes)
		{
			Lines.Add(FString::Printf(TEXT("  change %s: %s%s%s"), *Change.Location, *Change.What, Change.bWritten ? TEXT("") : TEXT(" (not written)"),
				Change.bReprinted ? TEXT(" (reprinted)") : TEXT("")));
		}
		Lines.Add(TEXT("  diagnostics:\n") + Compiler::FormatDstDiagnostics(Result.Diagnostics));
		Lines.Add(TEXT("  text:\n") + Result.Text);
		return FString::Join(Lines, TEXT("\n"));
	}

	bool WbtHasCode(const Compiler::FDstWriteBackResult& Result, const TCHAR* Code)
	{
		return Result.Diagnostics.GetDiagnostics().ContainsByPredicate([Code](const Lang::FDstDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	}

	/** Compared, written completely, a fixed point, and nothing to report. */
	bool WbtExpectWritten(FAutomationTestBase& Test, const FWbtCase& Case, const Compiler::FDstWriteBackResult& Result)
	{
		const bool bClean = Result.IsComplete() && Result.HasEdits() && Result.Diagnostics.Num() == 0;
		if (!bClean)
		{
			Test.AddError(FString::Printf(TEXT("%s: compared %d, fixed point %d, complete %d, edits %d, diagnostics %d\n%s"), *Case.CaseName, Result.bCompared,
				Result.bFixedPoint, Result.IsComplete(), Result.HasEdits(), Result.Diagnostics.Num(), *WbtDescribe(Result)));
		}
		return bClean;
	}

	/** The lines the text has that the base does not (trimmed), then `-` and the ones it lost. */
	TArray<FString> WbtChangedLines(const FString& Base, const FString& Text)
	{
		TArray<FString> Before;
		TArray<FString> After;
		Base.ParseIntoArray(Before, TEXT("\n"), false);
		Text.ParseIntoArray(After, TEXT("\n"), false);
		TArray<FString> Lost = Before;
		TArray<FString> Changed;
		for (const FString& Line : After)
		{
			if (Lost.RemoveSingle(Line) == 0)
			{
				Changed.Add(Line.TrimStartAndEnd());
			}
		}
		for (const FString& Line : Lost)
		{
			Changed.Add(TEXT("-") + Line.TrimStartAndEnd());
		}
		return Changed;
	}

	/** The base must hold what a case replaces, or the case tests nothing. */
	bool WbtRequire(FAutomationTestBase& Test, const FWbtCase& Case, const TCHAR* Piece)
	{
		if (!Case.Base.Contains(Piece, ESearchCase::CaseSensitive))
		{
			Test.AddError(FString::Printf(TEXT("%s: the formatted source has no `%s`:\n%s"), *Case.CaseName, Piece, *Case.Base));
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstWriteBackValuesTest, "DreamStateTree.WriteBack.Values",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstWriteBackValuesTest::RunTest(const FString& Parameters)
	{
		// Untouched: the text already says everything.
		{
			const FWbtCase Case(*this, TEXT("Same"));
			if (Case.IsValid())
			{
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				if (!(Result.bCompared && Result.bFixedPoint && !Result.HasEdits() && Result.Diagnostics.Num() == 0))
				{
					AddError(TEXT("an untouched asset already agrees with its text\n") + WbtDescribe(Result));
				}
				TestEqualSensitive(TEXT("an untouched asset leaves the text alone"), Result.Text, Case.Base);
			}
		}

		// A number on a task: just that number changes.
		{
			const FWbtCase Case(*this, TEXT("TaskNumber"));
			if (Case.IsValid() && WbtRequire(*this, Case, TEXT("TicksToComplete = 3")))
			{
				UStateTreeState* Work = Case.FindState(TEXT("Work"));
				if (Work && TestTrue(TEXT("Work has its tasks"), Work->Tasks.Num() == 2) && WbtSetInstance(*this, Work->Tasks[0], TEXT("TicksToComplete"), TEXT("5")))
				{
					const Compiler::FDstWriteBackResult Result = Case.WriteBack();
					WbtExpectWritten(*this, Case, Result);
					TestEqualSensitive(TEXT("only the number is replaced"), Result.Text, Case.Base.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 5")));
					if (TestEqual(TEXT("one change"), Result.Changes.Num(), 1))
					{
						TestFalse(TEXT("written where it stands, not reprinted"), Result.Changes[0].bReprinted);
					}
				}
			}
		}

		// A number passed by position.
		{
			const FWbtCase Case(*this, TEXT("PositionalNumber"));
			if (Case.IsValid() && WbtRequire(*this, Case, TEXT("Delay(2)")))
			{
				UStateTreeState* Work = Case.FindState(TEXT("Work"));
				if (Work && TestTrue(TEXT("Work has its tasks"), Work->Tasks.Num() == 2) && WbtSetInstance(*this, Work->Tasks[1], TEXT("Duration"), TEXT("2.5")))
				{
					const Compiler::FDstWriteBackResult Result = Case.WriteBack();
					WbtExpectWritten(*this, Case, Result);
					TestEqualSensitive(TEXT("the positional number is replaced"), Result.Text, Case.Base.Replace(TEXT("Delay(2)"), TEXT("Delay(2.5)")));
				}
			}
		}

		// A number in a condition.
		{
			const FWbtCase Case(*this, TEXT("ConditionNumber"));
			if (Case.IsValid() && WbtRequire(*this, Case, TEXT("Right = 3")))
			{
				UStateTreeState* Work = Case.FindState(TEXT("Work"));
				if (Work && TestTrue(TEXT("Work has its condition"), Work->EnterConditions.Num() == 1)
					&& WbtSetInstance(*this, Work->EnterConditions[0], TEXT("Right"), TEXT("7")))
				{
					const Compiler::FDstWriteBackResult Result = Case.WriteBack();
					WbtExpectWritten(*this, Case, Result);
					TestEqualSensitive(TEXT("only the number is replaced"), Result.Text, Case.Base.Replace(TEXT("Right = 3"), TEXT("Right = 7")));
				}
			}
		}

		// Two states, two numbers: both land.
		{
			const FWbtCase Case(*this, TEXT("TwoNumbers"));
			if (Case.IsValid() && WbtRequire(*this, Case, TEXT("TicksToComplete = 3")) && WbtRequire(*this, Case, TEXT("Delay(1)")))
			{
				UStateTreeState* Work = Case.FindState(TEXT("Work"));
				UStateTreeState* Rest = Case.FindState(TEXT("Rest"));
				if (Work && Rest && TestTrue(TEXT("the tasks are there"), Work->Tasks.Num() == 2 && Rest->Tasks.Num() == 1)
					&& WbtSetInstance(*this, Work->Tasks[0], TEXT("TicksToComplete"), TEXT("9")) && WbtSetInstance(*this, Rest->Tasks[0], TEXT("Duration"), TEXT("4")))
				{
					const Compiler::FDstWriteBackResult Result = Case.WriteBack();
					WbtExpectWritten(*this, Case, Result);
					TestEqualSensitive(TEXT("both numbers are replaced"), Result.Text,
						Case.Base.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 9")).Replace(TEXT("Delay(1)"), TEXT("Delay(4)")));
				}
			}
		}

		// A parameter's default: in the Parameters section, the one value.
		{
			const FWbtCase Case(*this, TEXT("ParameterDefault"));
			if (Case.IsValid() && WbtRequire(*this, Case, TEXT("float Speed = 1")))
			{
				WbtGetRootBag(Case).SetValueFloat(FName(TEXT("Speed")), 2.5f);
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestEqualSensitive(TEXT("the default is replaced"), Result.Text, Case.Base.Replace(TEXT("float Speed = 1"), TEXT("float Speed = 2.5")));
				if (TestEqual(TEXT("one change"), Result.Changes.Num(), 1))
				{
					TestFalse(TEXT("the section is not reprinted for a value"), Result.Changes[0].bReprinted);
				}
			}
		}

		// A state's settings: in the head of its card; the rest of the line stays.
		{
			const FWbtCase Case(*this, TEXT("StateSettings"));
			if (Case.IsValid() && WbtRequire(*this, Case, TEXT("State Rest { do Delay(1)  -> Work }")))
			{
				UStateTreeState* Rest = Case.FindState(TEXT("Rest"));
				if (Rest)
				{
					Rest->Weight = 2.f;
					Rest->Description = TEXT("Cools down");
					const Compiler::FDstWriteBackResult Result = Case.WriteBack();
					WbtExpectWritten(*this, Case, Result);
					const TArray<FString> Changed = WbtChangedLines(Case.Base, Result.Text);
					if (TestEqual(TEXT("one line replaced"), Changed.Num(), 2))
					{
						TestTrue(TEXT("the head has the settings and the card its clauses"), Changed[0].StartsWith(TEXT("State Rest ["))
							&& Changed[0].Contains(TEXT("Weight = 2")) && Changed[0].Contains(TEXT("Description = \"Cools down\""))
							&& Changed[0].EndsWith(TEXT("{ do Delay(1)  -> Work }")));
						TestEqualSensitive(TEXT("the old line is the one lost"), Changed[1], FString(TEXT("-State Rest { do Delay(1)  -> Work }")));
					}
					if (TestEqual(TEXT("one change"), Result.Changes.Num(), 1))
					{
						TestFalse(TEXT("the card is not reprinted for its settings"), Result.Changes[0].bReprinted);
					}
				}
			}
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstWriteBackStructureTest, "DreamStateTree.WriteBack.Structure",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstWriteBackStructureTest::RunTest(const FString& Parameters)
	{
		// A task added: the card is reprinted; its comments and the card beside it stay.
		{
			const FWbtCase Case(*this, TEXT("AddTask"));
			UStateTreeState* Work = Case.IsValid() ? Case.FindState(TEXT("Work")) : nullptr;
			if (Work)
			{
				TStateTreeEditorNode<FDstTestSendEventTask>& Added = Work->AddTask<FDstTestSendEventTask>();
				WbtSetInstance(*this, Added, TEXT("bSendPayload"), TEXT("False"));
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				const TArray<FString> Changed = WbtChangedLines(Case.Base, Result.Text);
				if (TestEqual(TEXT("one line replaced"), Changed.Num(), 2))
				{
					TestTrue(TEXT("the new task is on the task line"), Changed[0].StartsWith(TEXT("do DstTest(")) && Changed[0].Contains(TEXT("DstTestSendEvent(")));
					TestTrue(TEXT("the old task line is the one lost"), Changed[1].StartsWith(TEXT("-do DstTest(")));
				}
				TestTrue(TEXT("the comment above the card stays"), Result.Text.Contains(TEXT("// Does the work.")));
				TestTrue(TEXT("the comment above the clause stays on it"), Result.Text.Contains(TEXT("// The task list.\n            do DstTest(")));
				TestTrue(TEXT("the card beside it is untouched"), Result.Text.Contains(TEXT("State Rest { do Delay(1)  -> Work }")));
				if (TestEqual(TEXT("one change"), Result.Changes.Num(), 1))
				{
					TestTrue(TEXT("it is a reprint"), Result.Changes[0].bReprinted);
				}
			}
		}

		// A transition removed: its line is gone and nothing else moves.
		{
			const FWbtCase Case(*this, TEXT("RemoveTransition"));
			UStateTreeState* Work = Case.IsValid() ? Case.FindState(TEXT("Work")) : nullptr;
			if (Work && TestEqual(TEXT("Work has its transition"), Work->Transitions.Num(), 1))
			{
				Work->Transitions.RemoveAt(0);
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestEqualSensitive(TEXT("only that line changes"), FString::Join(WbtChangedLines(Case.Base, Result.Text), TEXT(" | ")),
					FString(TEXT("--> Rest on Succeeded")));
				TestTrue(TEXT("the comments stay"), Result.Text.Contains(TEXT("// Does the work.")) && Result.Text.Contains(TEXT("// The task list.")));
			}
		}

		// A transition added.
		{
			const FWbtCase Case(*this, TEXT("AddTransition"));
			UStateTreeState* Rest = Case.IsValid() ? Case.FindState(TEXT("Rest")) : nullptr;
			if (Rest)
			{
				Rest->AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::Succeeded);
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestTrue(TEXT("the card has the new transition after the old one"), Result.Text.Contains(TEXT("-> Work  -> TreeSucceeded")));
				TestTrue(TEXT("the cards above stay"), Result.Text.Contains(TEXT("// Does the work.")));
			}
		}

		// A condition changed in a way that is not a number: the card is reprinted.
		{
			const FWbtCase Case(*this, TEXT("InvertCondition"));
			UStateTreeState* Work = Case.IsValid() ? Case.FindState(TEXT("Work")) : nullptr;
			if (Work && TestEqual(TEXT("Work has its condition"), Work->EnterConditions.Num(), 1) && WbtSetNodeProperty(*this, Work->EnterConditions[0], TEXT("bInvert"), TEXT("True")))
			{
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				const TArray<FString> Changed = WbtChangedLines(Case.Base, Result.Text);
				if (TestEqual(TEXT("one line replaced"), Changed.Num(), 2))
				{
					TestEqualSensitive(TEXT("the condition is negated"), Changed[0], FString(TEXT("if not DstTest(Left = Params.Speed, Right = 3)")));
				}
			}
		}

		// A child state added under a card that has comments of its own.
		{
			const FWbtCase Case(*this, TEXT("AddChild"));
			UStateTreeState* Work = Case.IsValid() ? Case.FindState(TEXT("Work")) : nullptr;
			if (Work)
			{
				UStateTreeState& Extra = Work->AddChildState(FName(TEXT("Extra")));
				Extra.AddTask<FDstTestSendEventTask>();
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestTrue(TEXT("the new state is in the text"), Result.Text.Contains(TEXT("State Extra")) && Result.Text.Contains(TEXT("DstTestSendEvent(")));
				TestTrue(TEXT("the comments stay"), Result.Text.Contains(TEXT("// Does the work.")) && Result.Text.Contains(TEXT("// The task list.")));
				TestTrue(TEXT("the card beside it is untouched"), Result.Text.Contains(TEXT("State Rest { do Delay(1)  -> Work }")));
			}
		}

		// A state removed with the transition that led to it.
		{
			const FWbtCase Case(*this, TEXT("RemoveState"));
			UStateTreeState* Root = Case.IsValid() ? Case.FindState(TEXT("Root")) : nullptr;
			UStateTreeState* Work = Root ? Case.FindState(TEXT("Work")) : nullptr;
			UStateTreeState* Rest = Root ? Case.FindState(TEXT("Rest")) : nullptr;
			if (Root && Work && Rest)
			{
				Work->Transitions.Reset();
				Root->Children.RemoveAll([Rest](const TObjectPtr<UStateTreeState>& Child) { return Child == Rest; });
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestFalse(TEXT("the state is gone"), Result.Text.Contains(TEXT("State Rest")));
				TestFalse(TEXT("its transition is gone"), Result.Text.Contains(TEXT("-> Rest")));
				TestTrue(TEXT("the other state and its comments stay"), Result.Text.Contains(TEXT("State Work")) && Result.Text.Contains(TEXT("// Does the work."))
					&& Result.Text.Contains(TEXT("// The task list.")));
			}
		}

		// A state renamed in the editor: its ID follows it, and the transition into it follows the name.
		{
			const FWbtCase Case(*this, TEXT("RenameState"));
			UStateTreeState* Rest = Case.IsValid() ? Case.FindState(TEXT("Rest")) : nullptr;
			if (Rest)
			{
				Rest->Name = FName(TEXT("Cool"));
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestTrue(TEXT("the card has the new name"), Result.Text.Contains(TEXT("State Cool")));
				TestTrue(TEXT("the transition into it says the new name"), Result.Text.Contains(TEXT("-> Cool on Succeeded")));
				TestFalse(TEXT("the old name is gone"), Result.Text.Contains(TEXT("Rest")));
				TestTrue(TEXT("the comments stay"), Result.Text.Contains(TEXT("// Does the work.")) && Result.Text.Contains(TEXT("// The task list.")));
			}
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstWriteBackDocumentTest, "DreamStateTree.WriteBack.Document",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstWriteBackDocumentTest::RunTest(const FString& Parameters)
	{
		// A parameter added: the section is printed again, with the old ones in it.
		{
			const FWbtCase Case(*this, TEXT("AddParameter"));
			if (Case.IsValid())
			{
				FPropertyBagPropertyDesc Desc(FName(TEXT("Hurry")), EPropertyBagContainerType::None, EPropertyBagPropertyType::Float, nullptr);
				Desc.ID = FGuid::NewGuid();
				TArray<FPropertyBagPropertyDesc> Descs;
				Descs.Add(Desc);
				WbtGetRootBag(Case).AddProperties(Descs);
				WbtGetRootBag(Case).SetValueFloat(FName(TEXT("Hurry")), 0.5f);
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestTrue(TEXT("the new parameter is in the section"), Result.Text.Contains(TEXT("float Hurry = 0.5")));
				TestTrue(TEXT("the old ones are still there"), Result.Text.Contains(TEXT("float Speed = 1")) && Result.Text.Contains(TEXT("int Count = 2")));
				if (TestEqual(TEXT("one change"), Result.Changes.Num(), 1))
				{
					TestTrue(TEXT("the section is printed again"), Result.Changes[0].bReprinted);
				}
			}
		}

		// A setting the text does not have yet: a Settings section is added.
		{
			const FWbtCase Case(*this, TEXT("AddSettings"));
			UStateTreeEditorData* Data = Case.IsValid() ? Case.GetData() : nullptr;
			if (Data)
			{
				Data->GlobalTasksCompletion = EStateTreeTaskCompletionType::All;
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				WbtExpectWritten(*this, Case, Result);
				TestTrue(TEXT("the section is in the text"), Result.Text.Contains(TEXT("Settings")) && Result.Text.Contains(TEXT("GlobalTasksCompletion = All")));
				const TArray<FString> Changed = WbtChangedLines(Case.Base, Result.Text);
				if (TestEqual(TEXT("one line added and none lost"), Changed.Num(), 1))
				{
					TestTrue(TEXT("it is the section"), Changed[0].StartsWith(TEXT("Settings")) && Changed[0].Contains(TEXT("GlobalTasksCompletion = All")));
				}
			}
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstWriteBackSaveAsTest, "DreamStateTree.WriteBack.SaveAs",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstWriteBackSaveAsTest::RunTest(const FString& Parameters)
	{
		// The copy of an asset has new IDs everywhere: its states and nodes are found again by their places.
		const FWbtCase Case(*this, TEXT("SaveAs"));
		if (!Case.IsValid())
		{
			return false;
		}

		// Another name on every run: the package of an earlier run may still be in memory.
		const FString CopyName = TEXT("ST_SaveAs_Copy_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		UPackage* CopyPackage = CreatePackage(*FString::Printf(TEXT("/Game/DreamStateTreeTests/WriteBack/%s"), *CopyName));
		FObjectDuplicationParameters Params = InitStaticDuplicateObjectParams(Case.Tree, CopyPackage, FName(*CopyName), RF_AllFlags, nullptr, EDuplicateMode::Normal);
		UStateTree* Copy = Cast<UStateTree>(StaticDuplicateObjectEx(Params));
		if (!TestNotNull(TEXT("the copy"), Copy))
		{
			return false;
		}
		Copy->SetFlags(RF_Public | RF_Standalone);

		// A copy nobody touched says what the text says.
		const Compiler::FDstWriteBackResult Untouched = Compiler::WriteBackDstAsset(*Copy, WbtGetSourcePath(*Case.CaseName), Case.Base);
		if (!(Untouched.bCompared && Untouched.bFixedPoint && !Untouched.HasEdits() && Untouched.Diagnostics.Num() == 0))
		{
			AddError(TEXT("a copy nobody touched agrees with the text\n") + WbtDescribe(Untouched));
		}

		// A number changed in the copy.
		UStateTreeState* CopyWork = nullptr;
		if (UStateTreeEditorData* CopyData = Cast<UStateTreeEditorData>(Copy->EditorData))
		{
			CopyData->VisitHierarchy([&CopyWork](UStateTreeState& Candidate, UStateTreeState*) -> EStateTreeVisitor
			{
				if (Candidate.Name == FName(TEXT("Work")))
				{
					CopyWork = &Candidate;
					return EStateTreeVisitor::Break;
				}
				return EStateTreeVisitor::Continue;
			});
		}
		if (TestNotNull(TEXT("the copy's Work"), CopyWork) && TestTrue(TEXT("the copy's tasks"), CopyWork->Tasks.Num() == 2)
			&& WbtRequire(*this, Case, TEXT("TicksToComplete = 3")))
		{
			WbtSetInstance(*this, CopyWork->Tasks[0], TEXT("TicksToComplete"), TEXT("7"));
			const Compiler::FDstWriteBackResult Result = Compiler::WriteBackDstAsset(*Copy, WbtGetSourcePath(*Case.CaseName), Case.Base);
			WbtExpectWritten(*this, Case, Result);
			TestEqualSensitive(TEXT("the number is replaced in the copy's text"), Result.Text, Case.Base.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 7")));

			// And a state added to it.
			CopyWork->AddChildState(FName(TEXT("Extra"))).AddTask<FDstTestSendEventTask>();
			const Compiler::FDstWriteBackResult WithChild = Compiler::WriteBackDstAsset(*Copy, WbtGetSourcePath(*Case.CaseName), Case.Base);
			WbtExpectWritten(*this, Case, WithChild);
			TestTrue(TEXT("both changes are in the text"), WithChild.Text.Contains(TEXT("TicksToComplete = 7")) && WithChild.Text.Contains(TEXT("State Extra")));
			TestTrue(TEXT("the comments stay"), WithChild.Text.Contains(TEXT("// Does the work.")) && WithChild.Text.Contains(TEXT("// The task list.")));
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstWriteBackRefusalsTest, "DreamStateTree.WriteBack.Refusals",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstWriteBackRefusalsTest::RunTest(const FString& Parameters)
	{
		// A condition a template wrote: a change to it belongs to the template.
		{
			const TCHAR* const TemplateBody = TEXT(
				"{\n"
				"    Parameters = { float Speed = 1; }\n"
				"\n"
				"    Template Quick = DstTest(Left = Params.Speed, Right = 3);\n"
				"\n"
				"    State Root {\n"
				"        -> Work\n"
				"        State Work {\n"
				"            if Quick\n"
				"            do Delay(2)\n"
				"        }\n"
				"    }\n"
				"}\n");
			const FWbtCase Case(*this, TEXT("TemplateCondition"), TemplateBody);
			UStateTreeState* Work = Case.IsValid() ? Case.FindState(TEXT("Work")) : nullptr;
			if (Work && TestEqual(TEXT("Work has its condition"), Work->EnterConditions.Num(), 1) && WbtSetInstance(*this, Work->EnterConditions[0], TEXT("Right"), TEXT("9")))
			{
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				if (!WbtHasCode(Result, TEXT("DST8102")) || !Result.bRefused || Result.bFixedPoint || Result.IsComplete() || Result.HasEdits())
				{
					AddError(TEXT("a change inside a template is refused\n") + WbtDescribe(Result));
				}
				TestEqualSensitive(TEXT("a refused change leaves the text alone"), Result.Text, Case.Base);

				// A change next to it, outside the template, is still written.
				UStateTreeState* Root = Case.FindState(TEXT("Root"));
				if (Root && TestEqual(TEXT("Work has its tasks"), Work->Tasks.Num(), 1) && WbtSetInstance(*this, Work->Tasks[0], TEXT("Duration"), TEXT("6")))
				{
					const Compiler::FDstWriteBackResult Partial = Case.WriteBack();
					TestTrue(TEXT("the template change is refused again"), WbtHasCode(Partial, TEXT("DST8102")) && Partial.bRefused);
					TestTrue(TEXT("the other one is written"), Partial.HasEdits() && Partial.Text.Contains(TEXT("Delay(6)")) && !Partial.bFixedPoint);
					TestTrue(TEXT("the template stays as it was"), Partial.Text.Contains(TEXT("if Quick")) && Partial.Text.Contains(TEXT("Template Quick = DstTest(Left = Params.Speed, Right = 3);")));
				}
			}
		}

		// A subtree copied in from a library: the change belongs to the library.
		{
			const FString Directory = FPaths::GetPath(WbtGetSourcePath(TEXT("LibraryState")));
			IFileManager::Get().MakeDirectory(*Directory, true);
			const FString Library = TEXT(
				"StateTreeLibrary(Name=\"DreamStateTreeTests/WriteBack/WbLib\")\n"
				"{\n"
				"    Subtree Reaction(float Toughness = 1) {\n"
				"        State Stagger { do Delay(1) }\n"
				"    }\n"
				"}\n");
			FFileHelper::SaveStringToFile(Library, *FPaths::Combine(Directory, TEXT("WbLib.dth")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			const TCHAR* const LibraryBody = TEXT(
				"{\n"
				"    State Root {\n"
				"        State Hurt { link Reaction(Toughness = 2) }\n"
				"    }\n"
				"}\n");
			const FWbtCase Case(*this, TEXT("LibraryState"), LibraryBody, TEXT("#include \"WbLib.dth\"\n"));
			UStateTreeState* Stagger = Case.IsValid() ? Case.FindState(TEXT("Stagger")) : nullptr;
			if (Stagger && TestEqual(TEXT("Stagger has its task"), Stagger->Tasks.Num(), 1) && WbtSetInstance(*this, Stagger->Tasks[0], TEXT("Duration"), TEXT("4")))
			{
				const Compiler::FDstWriteBackResult Result = Case.WriteBack();
				if (!WbtHasCode(Result, TEXT("DST8103")) || !Result.bRefused || Result.bFixedPoint || Result.HasEdits())
				{
					AddError(TEXT("a change inside a library's subtree is refused\n") + WbtDescribe(Result));
				}
				TestEqualSensitive(TEXT("a refused change leaves the text alone"), Result.Text, Case.Base);
			}
		}

		// Structural changes turned off: a number is still written, a state's new transition is not.
		{
			const FWbtCase Case(*this, TEXT("NoStructure"));
			UStateTreeState* Work = Case.IsValid() ? Case.FindState(TEXT("Work")) : nullptr;
			UStateTreeState* Rest = Work ? Case.FindState(TEXT("Rest")) : nullptr;
			if (Work && Rest && TestTrue(TEXT("Work has its tasks"), Work->Tasks.Num() == 2) && WbtRequire(*this, Case, TEXT("TicksToComplete = 3"))
				&& WbtSetInstance(*this, Work->Tasks[0], TEXT("TicksToComplete"), TEXT("5")))
			{
				Rest->AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::Succeeded);
				Compiler::FDstWriteBackOptions WriteOptions;
				WriteOptions.bStructure = false;
				const Compiler::FDstWriteBackResult Result = Case.WriteBack(WriteOptions);
				if (!WbtHasCode(Result, TEXT("DST8104")) || !Result.bRefused || Result.bFixedPoint || Result.IsComplete() || !Result.HasEdits())
				{
					AddError(TEXT("a structural change is refused when asked to\n") + WbtDescribe(Result));
				}
				TestEqualSensitive(TEXT("only the number is written"), Result.Text, Case.Base.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 5")));

				// With structure on, the same asset is written completely.
				const Compiler::FDstWriteBackResult Full = Case.WriteBack();
				WbtExpectWritten(*this, Case, Full);
				TestTrue(TEXT("both changes are in the text"), Full.Text.Contains(TEXT("TicksToComplete = 5")) && Full.Text.Contains(TEXT("-> TreeSucceeded")));
			}
		}

		// A text that does not check cannot be compared with anything.
		{
			const FWbtCase Case(*this, TEXT("BrokenText"));
			if (Case.IsValid())
			{
				const FString Broken = Case.Base.Replace(TEXT("State Rest"), TEXT("State Rest ?"));
				const Compiler::FDstWriteBackResult Result = Compiler::WriteBackDstAsset(*Case.Tree, WbtGetSourcePath(*Case.CaseName), Broken);
				TestFalse(TEXT("nothing was compared"), Result.bCompared);
				TestTrue(TEXT("the text's own errors come back"), Result.Diagnostics.HasErrors());
				TestFalse(TEXT("so it is not complete"), Result.IsComplete());
				TestEqualSensitive(TEXT("and the text is as it was"), Result.Text, Broken);
			}
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstWriteBackMergeTest, "DreamStateTree.WriteBack.EditorMerge",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstWriteBackMergeTest::RunTest(const FString& Parameters)
	{
		// The editor changed a number; the file on disk changed another place meanwhile: both are kept.
		const FWbtCase Case(*this, TEXT("Merge"));
		UStateTreeState* Work = Case.IsValid() ? Case.FindState(TEXT("Work")) : nullptr;
		if (!Work || !TestEqual(TEXT("Work has its tasks"), Work->Tasks.Num(), 2) || !WbtRequire(*this, Case, TEXT("TicksToComplete = 3"))
			|| !WbtRequire(*this, Case, TEXT("int Count = 2")))
		{
			return false;
		}
		WbtSetInstance(*this, Work->Tasks[0], TEXT("TicksToComplete"), TEXT("5"));
		const Compiler::FDstWriteBackResult Ours = Case.WriteBack();
		WbtExpectWritten(*this, Case, Ours);

		FString Merged;
		TArray<Compiler::FDstMergeConflict> Conflicts;
		const FString TheirsApart = Case.Base.Replace(TEXT("int Count = 2"), TEXT("int Count = 4"));
		TestTrue(TEXT("changes apart merge"), Compiler::MergeDstTexts(Case.Base, Ours.Text, TheirsApart, Merged, Conflicts));
		TestEqualSensitive(TEXT("both changes are in the merge"), Merged,
			Case.Base.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 5")).Replace(TEXT("int Count = 2"), TEXT("int Count = 4")));

		// The same number changed both ways in the file: the editor's change and the file's change conflict.
		const FString TheirsSame = Case.Base.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 8"));
		TestFalse(TEXT("one place changed two ways conflicts"), Compiler::MergeDstTexts(Case.Base, Ours.Text, TheirsSame, Merged, Conflicts));
		if (TestEqual(TEXT("one conflict"), Conflicts.Num(), 1))
		{
			TestTrue(TEXT("ours is the editor's line"), Conflicts[0].Ours.Contains(TEXT("TicksToComplete = 5")));
			TestTrue(TEXT("theirs is the file's line"), Conflicts[0].Theirs.Contains(TEXT("TicksToComplete = 8")));
		}

		// The file already says what the editor did: nothing to merge, nothing to conflict with.
		TestTrue(TEXT("the same change on both sides merges"), Compiler::MergeDstTexts(Case.Base, Ours.Text, Ours.Text, Merged, Conflicts));
		TestEqualSensitive(TEXT("and is taken once"), Merged, Ours.Text);
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstWriteBackMetadataTest, "DreamStateTree.WriteBack.Metadata",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstWriteBackMetadataTest::RunTest(const FString& Parameters)
	{
		const FWbtCase Case(*this, TEXT("Metadata"));
		if (!Case.IsValid())
		{
			return false;
		}

		// The generator left the text the asset was made from in the package.
		FString Text;
		FString SourcePath;
		if (TestTrue(TEXT("a generated asset has its text"), Compiler::GetDstGeneratedText(*Case.Tree, Text, SourcePath)))
		{
			TestEqualSensitive(TEXT("the text is the source"), Text, Case.Base);
		}

		// Recorded again after a write-back; the package is marked dirty.
		const FString Other = Case.Base.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 4"));
		Case.Tree->GetPackage()->ClearDirtyFlag();
		Compiler::SetDstGeneratedText(*Case.Tree, Other, TEXT("AI/Other.dtr"));
		TestTrue(TEXT("the package is dirty"), Case.Tree->GetPackage()->IsDirty());
		FString ReadText;
		FString ReadPath;
		if (TestTrue(TEXT("the new text is there"), Compiler::GetDstGeneratedText(*Case.Tree, ReadText, ReadPath)))
		{
			TestEqualSensitive(TEXT("the text is the one recorded"), ReadText, Other);
			TestEqualSensitive(TEXT("and so is its path"), ReadPath, FString(TEXT("AI/Other.dtr")));
		}

		// An asset nobody generated has none.
		UStateTree* Plain = NewObject<UStateTree>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStateTree::StaticClass(), TEXT("ST_NotGenerated")), RF_Transient);
		TestFalse(TEXT("an asset made by hand has no generated text"), Compiler::GetDstGeneratedText(*Plain, ReadText, ReadPath));
		return true;
	}
}

#endif
