// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureHelpers.h"
#include "Spike/SpikeCaptureOps.h"
#include "Spike/SpikeDump.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "StateTree.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// "Save As" / duplicate of a state tree asset: the engine gives the copy new IDs (UStateTreeEditorData::PostDuplicate
// runs DuplicateIDs). This measures how many of each kind change, and whether names and structure survive, to see if
// a write-back can align the copy's data with text by state path and node name instead of by ID.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstSpikeCaptureSaveAsIdsTest, "DreamStateTree.Spike.Capture.SaveAsIds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace UE::DreamStateTree::Spike
{
namespace
{

	struct FDstIdEntry
	{
		FString Kind;
		/** Position-based key, equal for the same logical item in the original and in the copy. */
		FString Key;
		/** Name of the item, to check names survive. */
		FString Label;
		FGuid Id;
	};

	class FDstIdCollector
	{
	public:
		void Add(const TCHAR* Kind, const FString& Key, const FString& Label, const FGuid& Id)
		{
			if (!Id.IsValid())
			{
				++InvalidIds;
				return;
			}
			FDstIdEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Kind = Kind;
			Entry.Key = Key;
			Entry.Label = Label;
			Entry.Id = Id;
		}

		void AddNodes(const TCHAR* Kind, const FString& OwnerKey, const TArray<FStateTreeEditorNode>& Nodes)
		{
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Add(Kind, FString::Printf(TEXT("%s|%s[%d]"), *OwnerKey, Kind, Index), Nodes[Index].GetName().ToString(), Nodes[Index].ID);
			}
		}

		void AddParameterProperties(const TCHAR* Kind, const FString& OwnerKey, const FInstancedPropertyBag& Bag)
		{
			if (const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
				{
					Add(Kind, FString::Printf(TEXT("%s|%s:%s"), *OwnerKey, Kind, *Desc.Name.ToString()), Desc.Name.ToString(), Desc.ID);
				}
			}
		}

		void AddState(const UStateTreeState& State, const FString& Key)
		{
			const FString Label = State.Name.ToString();
			Add(TEXT("State"), Key, Label, State.ID);
			Add(TEXT("StateEventId"), Key + TEXT("#event"), Label, State.GetEventID());
			Add(TEXT("StateParametersId"), Key + TEXT("#params"), Label, State.Parameters.ID);
			AddParameterProperties(TEXT("StateParameterProperty"), Key, State.Parameters.Parameters);
			AddNodes(TEXT("EnterCondition"), Key, State.EnterConditions);
			AddNodes(TEXT("Task"), Key, State.Tasks);
			AddNodes(TEXT("Consideration"), Key, State.Considerations);
			Add(TEXT("SingleTask"), Key + TEXT("|SingleTask"), State.SingleTask.GetName().ToString(), State.SingleTask.ID);
			for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
			{
				const FStateTreeTransition& Transition = State.Transitions[Index];
				const FString TransitionKey = FString::Printf(TEXT("%s|Transition[%d]"), *Key, Index);
				Add(TEXT("Transition"), TransitionKey, FString(), Transition.ID);
				Add(TEXT("TransitionEventId"), TransitionKey + TEXT("#event"), FString(), Transition.GetEventID());
				AddNodes(TEXT("TransitionCondition"), TransitionKey, Transition.Conditions);
			}
			for (int32 Index = 0; Index < State.Children.Num(); ++Index)
			{
				if (const UStateTreeState* Child = State.Children[Index])
				{
					AddState(*Child, FString::Printf(TEXT("%s/%d"), *Key, Index));
				}
			}
		}

		void AddEditorData(const UStateTreeEditorData& EditorData)
		{
			Add(TEXT("RootParametersId"), TEXT("Root#params"), FString(), EditorData.GetRootParametersGuid());
			AddParameterProperties(TEXT("RootParameterProperty"), TEXT("Root"), EditorData.GetRootParametersPropertyBag());
			AddNodes(TEXT("Evaluator"), TEXT("Global"), EditorData.Evaluators);
			AddNodes(TEXT("GlobalTask"), TEXT("Global"), EditorData.GlobalTasks);
			for (int32 Index = 0; Index < EditorData.SubTrees.Num(); ++Index)
			{
				if (const UStateTreeState* SubTree = EditorData.SubTrees[Index])
				{
					AddState(*SubTree, FString::Printf(TEXT("S%d"), Index));
				}
			}
		}

		TArray<FDstIdEntry> Entries;
		int32 InvalidIds = 0;
	};

	/** The structure of the editor data with names but without IDs. */
	FString DumpWithoutIds(const UStateTreeEditorData& EditorData)
	{
		FDumpOptions Options;
		Options.bIncludeIds = false;
		return DumpEditorData(EditorData, Options);
	}

	/** Each binding as text with its two ends named by position instead of ID, so original and copy can be compared. */
	TArray<FString> DescribeBindings(const UStateTreeEditorData& EditorData, const TArray<FDstIdEntry>& Entries, int32& OutDangling)
	{
		TMap<FGuid, FString> IdToKey;
		for (const FDstIdEntry& Entry : Entries)
		{
			IdToKey.Add(Entry.Id, Entry.Key);
		}
		auto Name = [&IdToKey, &OutDangling](const FGuid& Id)
		{
			if (const FString* Key = IdToKey.Find(Id))
			{
				return *Key;
			}
			++OutDangling;
			return FString::Printf(TEXT("<unknown %s>"), *DstShortGuid(Id));
		};

		TArray<FString> Result;
		for (const FStateTreePropertyPathBinding& Binding : EditorData.EditorBindings.GetBindings())
		{
			Result.Add(FString::Printf(TEXT("%s :: %s  ->  %s :: %s"),
				*Name(Binding.GetSourcePath().GetStructID()), *Binding.GetSourcePath().ToString(),
				*Name(Binding.GetTargetPath().GetStructID()), *Binding.GetTargetPath().ToString()));
		}
		Result.Sort();
		return Result;
	}

	/** Compares the ID tables of the original and a copy; appends a readable table to OutReport. Returns the number of changed IDs. */
	int32 CompareIds(const FDstIdCollector& Original, const FDstIdCollector& Copy, FString& OutReport, int32& OutTotal)
	{
		TMap<FString, const FDstIdEntry*> CopyByKey;
		for (const FDstIdEntry& Entry : Copy.Entries)
		{
			CopyByKey.Add(Entry.Key, &Entry);
		}

		struct FStat
		{
			int32 Total = 0;
			int32 Changed = 0;
			int32 MissingInCopy = 0;
			int32 LabelChanged = 0;
		};
		TArray<FString> KindOrder;
		TMap<FString, FStat> Stats;
		for (const FDstIdEntry& Entry : Original.Entries)
		{
			if (!Stats.Contains(Entry.Kind))
			{
				KindOrder.Add(Entry.Kind);
			}
			FStat& Stat = Stats.FindOrAdd(Entry.Kind);
			++Stat.Total;
			if (const FDstIdEntry* const* Found = CopyByKey.Find(Entry.Key))
			{
				Stat.Changed += (*Found)->Id != Entry.Id ? 1 : 0;
				Stat.LabelChanged += (*Found)->Label != Entry.Label ? 1 : 0;
			}
			else
			{
				++Stat.MissingInCopy;
			}
		}

		OutReport += FString::Printf(TEXT("    %-24s %6s %8s %10s %9s %9s\n"), TEXT("Kind"), TEXT("total"), TEXT("changed"), TEXT("unchanged"), TEXT("missing"), TEXT("renamed"));
		int32 TotalChanged = 0;
		OutTotal = 0;
		for (const FString& Kind : KindOrder)
		{
			const FStat& Stat = Stats.FindChecked(Kind);
			OutReport += FString::Printf(TEXT("    %-24s %6d %8d %10d %9d %9d\n"), *Kind, Stat.Total, Stat.Changed, Stat.Total - Stat.Changed - Stat.MissingInCopy, Stat.MissingInCopy, Stat.LabelChanged);
			TotalChanged += Stat.Changed;
			OutTotal += Stat.Total;
		}
		OutReport += FString::Printf(TEXT("    %-24s %6d %8d\n"), TEXT("ALL"), OutTotal, TotalChanged);

		// An ID that exists in both assets at different positions would make an ID-based alignment silently wrong.
		int32 CrossCollisions = 0;
		TMap<FGuid, FString> OriginalKeyById;
		for (const FDstIdEntry& Entry : Original.Entries)
		{
			OriginalKeyById.Add(Entry.Id, Entry.Key);
		}
		for (const FDstIdEntry& Entry : Copy.Entries)
		{
			if (const FString* OriginalKey = OriginalKeyById.Find(Entry.Id))
			{
				CrossCollisions += (*OriginalKey != Entry.Key) ? 1 : 0;
			}
		}
		OutReport += FString::Printf(TEXT("    IDs of the copy that appear in the original at a different position: %d\n"), CrossCollisions);
		OutReport += FString::Printf(TEXT("    Entries without an ID (not counted): original %d, copy %d\n"), Original.InvalidIds, Copy.InvalidIds);
		return TotalChanged;
	}

	/** Makes the copy the way the editor's duplicate does (ObjectTools::DuplicateSingleObject without the UI). */
	UStateTree* DuplicateLikeTheEditor(UStateTree* Source, const FString& NewAssetName, FDstCaptureRecorder& Recorder)
	{
		const FString PackageName = FString::Printf(TEXT("/Game/DreamStateTreeTests/Spike/Capture/%s"), *NewAssetName);
		UPackage* NewPackage = CreatePackage(*PackageName);
		if (!NewPackage)
		{
			return nullptr;
		}
		// Watch the new package before the copy exists, so PostDuplicate and what it triggers are recorded.
		Recorder.TrackPackage(NewPackage);

		FObjectDuplicationParameters Params = InitStaticDuplicateObjectParams(Source, NewPackage, FName(*NewAssetName), RF_AllFlags, nullptr, EDuplicateMode::Normal);
		UStateTree* Copy = Cast<UStateTree>(StaticDuplicateObjectEx(Params));
		if (Copy)
		{
			Copy->SetFlags(RF_Public | RF_Standalone);
			Recorder.Track(Copy);
		}
		return Copy;
	}
}
}

bool FDstSpikeCaptureSaveAsIdsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamStateTree::Spike;

	FDstCaptureFixture Fixture;
	FString Error;
	if (!Fixture.Initialize(TEXT("SaveAs"), Error))
	{
		AddError(FString::Printf(TEXT("Could not set up the asset: %s"), *Error));
		return false;
	}

	UStateTree* Original = Fixture.GetTree();
	UStateTreeEditorData* OriginalData = Fixture.GetEditorData();

	// Enrich the baseline so every kind of ID exists: a transition condition bound to a root parameter, a state
	// parameter, a global task.
	{
		UStateTreeState* A = Fixture.FindState(Fixture.Ids.A);
		UStateTreeState* B = Fixture.FindState(Fixture.Ids.B);
		FStateTreeTransition* Transition = Fixture.FindTransition(Fixture.Ids.A, Fixture.Ids.ATransition);
		UScriptStruct* RandomStruct = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeRandomCondition"));
		UScriptStruct* DebugText = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeDebugTextTask"));
		if (!A || !B || !Transition || !RandomStruct || !DebugText || !OriginalData)
		{
			AddError(TEXT("The baseline asset is missing parts the experiment needs."));
			return false;
		}
		FStateTreeEditorNode& TransitionCondition = Transition->Conditions.AddDefaulted_GetRef();
		TransitionCondition.InitializeAs(A, RandomStruct);
		TransitionCondition.SetNodeName(FName(TEXT("TransitionChance")));
		const FPropertyBindingPath Source(OriginalData->GetRootParametersGuid(), FDstCaptureFixture::SpeedParameterName);
		const FPropertyBindingPath Target(TransitionCondition.ID, FName(TEXT("Threshold")));
		OriginalData->AddPropertyBinding(Source, Target);

		(void)B->Parameters.Parameters.AddProperty(FName(TEXT("Mood")), EPropertyBagPropertyType::Bool);

		FStateTreeEditorNode& GlobalTask = OriginalData->GlobalTasks.AddDefaulted_GetRef();
		GlobalTask.InitializeAs(OriginalData, DebugText);
		GlobalTask.SetNodeName(FName(TEXT("GlobalSay")));

		OriginalData->UpdateBindings();
		UStateTreeEditingSubsystem::MarkAsPubliclyModified(Original);
		UStateTreeEditingSubsystem::ValidateStateTree(Original);

		FString Messages;
		const bool bCompiled = Fixture.Compile(Messages);
		AddInfo(FString::Printf(TEXT("Original asset: %d bindings, compile %s%s"), OriginalData->EditorBindings.GetBindings().Num(),
			bCompiled ? TEXT("succeeded") : TEXT("failed"), Messages.IsEmpty() ? TEXT("") : *(TEXT("\n") + Messages)));
	}

	FDstCaptureRecorder Recorder;
	Recorder.Track(Original);
	Recorder.Start();

	FString Report;
	const FString OriginalDump = DumpWithoutIds(*OriginalData);
	Report += TEXT("Original editor data (IDs hidden):\n") + OriginalDump + TEXT("\n\n");

	FDstIdCollector OriginalIds;
	OriginalIds.AddEditorData(*OriginalData);
	int32 DanglingOriginal = 0;
	const TArray<FString> OriginalBindings = DescribeBindings(*OriginalData, OriginalIds.Entries, DanglingOriginal);

	// ---- Copy 1: the engine's duplicate path (what Save As and Duplicate use) -------------------------------------
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("Duplicate with StaticDuplicateObjectEx (editor duplicate path)"));
		UStateTree* Copy = DuplicateLikeTheEditor(Original, TEXT("ST_SaveAs_Copy_Static"), Recorder);
		if (!Copy)
		{
			AddError(TEXT("StaticDuplicateObjectEx returned no asset."));
			return false;
		}
		Fixture.AdoptExtraTree(Copy, /*bRegisteredInAssetRegistry*/ false);

		FString Section = TEXT("== Copy made with StaticDuplicateObjectEx ==\n");
		Section += TEXT("  events during the duplication:\n") + Recorder.SummarizeSegment(Segment) + TEXT("\n");

		UStateTreeEditorData* CopyData = Cast<UStateTreeEditorData>(Copy->EditorData);
		if (!CopyData)
		{
			AddError(TEXT("The copy has no editor data."));
			return false;
		}
		Section += FString::Printf(TEXT("  copy: editor data outer is the copy=%s, ready to run (compiled)=%s, editor data dirty=%s\n"),
			CopyData->GetOuter() == Copy ? TEXT("yes") : TEXT("no"), Copy->IsReadyToRun() ? TEXT("yes") : TEXT("no"),
			Copy->IsEditorDataDirty() ? TEXT("yes") : TEXT("no"));

		FDstIdCollector CopyIds;
		CopyIds.AddEditorData(*CopyData);
		int32 Total = 0;
		const int32 Changed = CompareIds(OriginalIds, CopyIds, Section, Total);

		const FString CopyDump = DumpWithoutIds(*CopyData);
		const bool bSameStructure = CopyDump == OriginalDump;
		Section += FString::Printf(TEXT("  names and structure identical (dump without IDs equal): %s\n"), bSameStructure ? TEXT("yes") : TEXT("NO"));
		if (!bSameStructure)
		{
			Section += TEXT("  structure differences (only-original / only-copy):\n") + DiffLines(OriginalDump, CopyDump, 12) + TEXT("\n");
		}

		int32 DanglingCopy = 0;
		const TArray<FString> CopyBindings = DescribeBindings(*CopyData, CopyIds.Entries, DanglingCopy);
		const bool bSameBindings = CopyBindings == OriginalBindings;
		Section += FString::Printf(TEXT("  bindings: original %d, copy %d, same logical bindings (by position and property path)=%s, dangling struct IDs original=%d copy=%d\n"),
			OriginalBindings.Num(), CopyBindings.Num(), bSameBindings ? TEXT("yes") : TEXT("NO"), DanglingOriginal, DanglingCopy);
		if (!bSameBindings)
		{
			for (const FString& Line : OriginalBindings)
			{
				Section += TEXT("    original: ") + Line + TEXT("\n");
			}
			for (const FString& Line : CopyBindings)
			{
				Section += TEXT("    copy:     ") + Line + TEXT("\n");
			}
		}
		Section += FString::Printf(TEXT("  verdict: %d of %d IDs changed; alignment by state path + node name %s\n"), Changed, Total,
			(bSameStructure && bSameBindings) ? TEXT("is possible (names and structure survive, bindings still point at the same items)") : TEXT("is NOT safe (see differences)"));

		AddInfo(Section);
		Report += Section + TEXT("\n");
	}

	// ---- Copy 2: through AssetTools, which is what the Content Browser's Duplicate calls ---------------------------
	{
		const int32 Segment = Recorder.BeginSegment(TEXT("Duplicate with IAssetTools::DuplicateAsset"));
		const FString NewAssetName = TEXT("ST_SaveAs_Copy_AssetTools");

		// Not run: in the headless editor the engine crashed on the tick after IAssetTools::DuplicateAsset and the
		// fixture's cleanup (an access violation under TypedElementFramework). StaticDuplicateObjectEx above is the
		// code path DuplicateAsset ends in.
		UStateTree* Copy = nullptr;

		FString Section = TEXT("== Copy made with IAssetTools::DuplicateAsset ==\n");
		if (!Copy)
		{
			Section += TEXT("  DuplicateAsset returned no asset (it needs the UI services or refused the name in this run); the StaticDuplicateObjectEx result above is the same code path without dialogs.\n");
		}
		else
		{
			Recorder.Track(Copy);
			Fixture.AdoptExtraTree(Copy, /*bRegisteredInAssetRegistry*/ true);
			Section += TEXT("  events during the duplication:\n") + Recorder.SummarizeSegment(Segment) + TEXT("\n");

			UStateTreeEditorData* CopyData = Cast<UStateTreeEditorData>(Copy->EditorData);
			if (CopyData)
			{
				FDstIdCollector CopyIds;
				CopyIds.AddEditorData(*CopyData);
				int32 Total = 0;
				const int32 Changed = CompareIds(OriginalIds, CopyIds, Section, Total);
				const bool bSameStructure = DumpWithoutIds(*CopyData) == OriginalDump;
				Section += FString::Printf(TEXT("  %d of %d IDs changed; names and structure identical: %s\n"), Changed, Total, bSameStructure ? TEXT("yes") : TEXT("NO"));
			}
		}
		AddInfo(Section);
		Report += Section + TEXT("\n");
	}

	Report += TEXT("Full event log:\n") + Recorder.FormatAll();
	Recorder.Stop();

	AddInfo(FString::Printf(TEXT("Full report: %s"), *SaveReport(TEXT("Capture"), TEXT("SaveAsIds"), Report)));
	return true;
}
