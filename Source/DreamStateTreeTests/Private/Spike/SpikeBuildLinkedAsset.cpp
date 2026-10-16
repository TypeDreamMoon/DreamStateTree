// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeBuildHelpers.h"
#include "Spike/SpikeDump.h"

#include "Fixtures/DstTestExecutionLog.h"
#include "StateTreeEditorData.h"

// A parent tree with a LinkedAsset state that runs another tree asset, with one of the child's root parameters
// overridden. The question is what order the two trees have to be compiled in, and which compile entry point
// (the compiler struct, or the compiler manager the editor uses) takes care of the child.

namespace UE::DreamStateTree::Spike
{
	namespace BuildLinkedAssetPrivate
	{
		FString BagPropertyNames(const FInstancedPropertyBag& Bag)
		{
			TArray<FString> Names;
			if (const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
				{
					Names.Add(Desc.Name.ToString());
				}
			}
			return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
		}

		/** Root > LinkedChild (a LinkedAsset state running Child). Overrides Radius when the state already knows it. */
		UStateTreeState& BuildParent(FDstBuildTreeBuilder& B, UStateTree* Child, bool& bOutOverridden)
		{
			B.EnsureSpeedParameter();
			UStateTreeState& Root = B.AddSubTree(TEXT("Root"));
			Root.SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
			UStateTreeState& Link = B.AddChild(Root, TEXT("LinkedChild"), EStateTreeStateType::LinkedAsset);
			Link.SetLinkedStateAsset(Child);

			bOutOverridden = false;
			const FGuid RadiusId = FDstBuildTreeBuilder::FindBagPropertyId(Link.Parameters.Parameters, TEXT("Radius"));
			if (RadiusId.IsValid())
			{
				Link.SetParametersPropertyOverridden(RadiusId, true);
				Link.Parameters.Parameters.SetValueFloat(TEXT("Radius"), 9.f);
				bOutOverridden = true;
			}
			return Link;
		}

		struct FLinkScenario
		{
			const TCHAR* Name;
			/** Compile with the compiler manager (the editor's path) instead of the compiler struct. */
			bool bViaManager;
			/** Compile the child before the parent links to it. */
			bool bCompileChildFirst;
			/** Compile the child after the parent was built and compiled once, then compile the parent again. */
			bool bCompileChildAfterwards;
		};
	}
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FDstSpikeBuild_LinkedAsset, FDstSpikeBuildTestBase, "DreamStateTree.Spike.Build.LinkedAsset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDstSpikeBuild_LinkedAsset::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;
	using UE::DreamStateTree::Tests::FExecutionLog;

	FDstBuildAssetKeeper Keeper;
	FDstBuildLogCapture LogCapture;
	FDstBuildReport Report;
	UDstTestOwner* Owner = Keeper.NewOwner();

	const UE::DreamStateTree::Spike::BuildLinkedAssetPrivate::FLinkScenario Scenarios[] = {
		{ TEXT("ChildFirst_Manager"), true, true, false },
		{ TEXT("ChildFirst_Direct"), false, true, false },
		{ TEXT("ParentFirst_Manager"), true, false, false },
		{ TEXT("ParentFirst_Direct"), false, false, false },
		{ TEXT("ParentFirst_ThenChild_Manager"), true, false, true },
		{ TEXT("ParentFirst_ThenChild_Direct"), false, false, true },
	};

	for (const UE::DreamStateTree::Spike::BuildLinkedAssetPrivate::FLinkScenario& Scenario : Scenarios)
	{
		Report.Heading(Scenario.Name);
		LogCapture.Reset();

		auto Compile = [&Scenario](UStateTree& Tree)
		{
			return Scenario.bViaManager ? DstBuildCompileViaManager(Tree) : DstBuildCompile(Tree);
		};

		UStateTree* Child = Keeper.NewPackagedTree(TEXT("LinkedAsset"), *FString::Printf(TEXT("Child_%s"), Scenario.Name), UDstTestSchema::StaticClass());
		{
			FDstBuildTreeBuilder ChildBuilder(*Child);
			DstBuildLinkableChildTree(ChildBuilder);
		}

		FString ChildStepReport;
		if (Scenario.bCompileChildFirst)
		{
			const FDstBuildCompileOutcome ChildOutcome = Compile(*Child);
			Report.Line(TEXT("child compile: ") + ChildOutcome.ToString());
			ChildStepReport = ChildOutcome.Summary();
		}
		else
		{
			Report.Line(FString::Printf(TEXT("child not compiled; ready to run: %s"), Child->IsReadyToRun() ? TEXT("yes") : TEXT("no")));
			ChildStepReport = TEXT("not compiled");
		}

		UStateTree* Parent = Keeper.NewPackagedTree(TEXT("LinkedAsset"), *FString::Printf(TEXT("Parent_%s"), Scenario.Name), UDstTestSchema::StaticClass());
		FDstBuildTreeBuilder ParentBuilder(*Parent);
		bool bOverridden = false;
		UStateTreeState& Link = UE::DreamStateTree::Spike::BuildLinkedAssetPrivate::BuildParent(ParentBuilder, Child, bOverridden);
		const FString LinkTimeParameters = UE::DreamStateTree::Spike::BuildLinkedAssetPrivate::BagPropertyNames(Link.Parameters.Parameters);
		const bool bOverriddenAtLinkTime = bOverridden;
		Report.Line(FString::Printf(TEXT("linked state parameters after SetLinkedStateAsset: %s; Radius overridden: %s"),
			*LinkTimeParameters, bOverriddenAtLinkTime ? TEXT("yes") : TEXT("no")));

		const FDstBuildCompileOutcome ParentOutcome = Compile(*Parent);
		Report.Line(TEXT("parent compile: ") + ParentOutcome.ToString());
		Report.Line(FString::Printf(TEXT("child ready to run after the parent compile: %s"), Child->IsReadyToRun() ? TEXT("yes") : TEXT("no")));

		FString AfterwardsReport;
		if (Scenario.bCompileChildAfterwards)
		{
			const FDstBuildCompileOutcome ChildOutcome = Compile(*Child);
			Report.Line(TEXT("child compile afterwards: ") + ChildOutcome.ToString());
			Report.Line(FString::Printf(TEXT("linked state parameters after the child compile: %s"), *UE::DreamStateTree::Spike::BuildLinkedAssetPrivate::BagPropertyNames(Link.Parameters.Parameters)));

			// The override can be made now that the state knows the layout.
			const FGuid RadiusId = FDstBuildTreeBuilder::FindBagPropertyId(Link.Parameters.Parameters, TEXT("Radius"));
			if (RadiusId.IsValid())
			{
				Link.SetParametersPropertyOverridden(RadiusId, true);
				Link.Parameters.Parameters.SetValueFloat(TEXT("Radius"), 9.f);
				bOverridden = true;
			}
			const FDstBuildCompileOutcome SecondParentOutcome = Compile(*Parent);
			Report.Line(TEXT("parent compile again: ") + SecondParentOutcome.ToString());
			AfterwardsReport = FString::Printf(TEXT("; after compiling the child: link parameters [%s], parent again %s"),
				*UE::DreamStateTree::Spike::BuildLinkedAssetPrivate::BagPropertyNames(Link.Parameters.Parameters), *SecondParentOutcome.Summary());
		}

		// Run whatever compiled: does the override reach the child's task?
		FString RunReport = TEXT("not run");
		if (Parent->IsReadyToRun())
		{
			FDstBuildRunOptions Options;
			Options.NumTicks = 3;
			const FDstBuildRunResult Run = DstBuildRunTree(*Parent, *Owner, Options);
			Report.Heading(FString::Printf(TEXT("%s: run"), Scenario.Name));
			Report.Lines(Run.StepLines, TEXT(""));
			const bool bChildRan = Run.Log.Contains(TEXT("ChildTask: Enter:"));
			const bool bOverrideSeen = Run.Log.Contains(TEXT("ChildTask: Enter:Input=4 Scale=9"));
			RunReport = FString::Printf(TEXT("started=%s, child task ran=%s, Radius override reached the task=%s"),
				Run.bStarted ? TEXT("yes") : TEXT("no"), bChildRan ? TEXT("yes") : TEXT("no"), bOverrideSeen ? TEXT("yes") : TEXT("no"));

			// The path the editor takes: child compiled first, then the parent, through the compiler manager.
			if (FCString::Strcmp(Scenario.Name, TEXT("ChildFirst_Manager")) == 0)
			{
				TestTrue(TEXT("ChildFirst_Manager: the parent starts"), Run.bStarted);
				TestTrue(TEXT("ChildFirst_Manager: the linked asset's task runs"), bChildRan);
				TestTrue(TEXT("ChildFirst_Manager: the overridden Radius reaches the linked asset's task"), bOverrideSeen);
			}
		}
		else if (FCString::Strcmp(Scenario.Name, TEXT("ChildFirst_Manager")) == 0)
		{
			AddError(FString::Printf(TEXT("ChildFirst_Manager: the parent is not ready to run: %s"), *ParentOutcome.ToString()));
		}

		Report.Block(TEXT("engine log"), FString::Join(LogCapture.GetLines(), TEXT("\n")));
		AddInfo(FString::Printf(TEXT("[LinkedAsset] %s: child %s; link parameters after SetLinkedStateAsset [%s], overridden=%s; parent %s%s; run: %s"),
			Scenario.Name, *ChildStepReport, *LinkTimeParameters, bOverriddenAtLinkTime ? TEXT("yes") : TEXT("no"),
			*ParentOutcome.Summary(), *AfterwardsReport, *RunReport));
	}

	const FString ReportPath = SaveReport(TEXT("Build"), TEXT("LinkedAsset"), Report.ToString());
	AddInfo(FString::Printf(TEXT("Report: %s"), *ReportPath));
	return true;
}
