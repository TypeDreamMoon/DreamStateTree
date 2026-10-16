// Copyright DreamMoon. All Rights Reserved.

#include "Spike/SpikeCaptureHelpers.h"

#include "Spike/SpikeDump.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StateTreeComponentSchema.h"
#include "Editor.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Logging/TokenizedMessage.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeCompiler.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeDelegates.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeFactory.h"
#include "StateTreeViewModel.h"
#include "UObject/Package.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"

// StateTreeEditorModule.h includes BlueprintEditorModule.h from the Kismet module, which only the engine's own StateTree
// module has on its include path. When this module lacks that path, the three FStateTreeEditorModule events
// (OnPostInternalCompile, OnPreValidateStateTree, OnValidateStateTree) are not hooked and only the log category
// is declared by hand (it is all the silencer needs). Adding "Kismet" to PrivateIncludePathModuleNames turns them on.
#if __has_include("BlueprintEditorModule.h")
	#define DST_STATETREE_EDITOR_MODULE_HEADER 1
	#include "StateTreeEditorModule.h"
#else
	#define DST_STATETREE_EDITOR_MODULE_HEADER 0
	STATETREEEDITORMODULE_API DECLARE_LOG_CATEGORY_EXTERN(LogStateTreeEditor, Log, All);
#endif

namespace UE::DreamStateTree::Spike
{
	bool DstModuleHooksCompiledIn()
	{
		return DST_STATETREE_EDITOR_MODULE_HEADER != 0;
	}

	namespace
	{
		FString ChangeTypeToString(EPropertyChangeType::Type ChangeType)
		{
			FString Out;
			auto AddFlag = [&Out, ChangeType](EPropertyChangeType::Type Bit, const TCHAR* Name)
			{
				if ((ChangeType & Bit) != 0)
				{
					if (!Out.IsEmpty())
					{
						Out += TEXT("|");
					}
					Out += Name;
				}
			};
			AddFlag(EPropertyChangeType::Unspecified, TEXT("Unspecified"));
			AddFlag(EPropertyChangeType::ArrayAdd, TEXT("ArrayAdd"));
			AddFlag(EPropertyChangeType::ArrayRemove, TEXT("ArrayRemove"));
			AddFlag(EPropertyChangeType::ArrayClear, TEXT("ArrayClear"));
			AddFlag(EPropertyChangeType::ValueSet, TEXT("ValueSet"));
			AddFlag(EPropertyChangeType::Duplicate, TEXT("Duplicate"));
			AddFlag(EPropertyChangeType::Interactive, TEXT("Interactive"));
			AddFlag(EPropertyChangeType::Redirected, TEXT("Redirected"));
			AddFlag(EPropertyChangeType::ArrayMove, TEXT("ArrayMove"));
			AddFlag(EPropertyChangeType::ToggleEditable, TEXT("ToggleEditable"));
			AddFlag(EPropertyChangeType::ResetToDefault, TEXT("ResetToDefault"));
			if (Out.IsEmpty())
			{
				Out = TEXT("0");
			}
			return Out;
		}

		/** "Tasks[0].Instance.Duration"; array indices come from the event when there is one. */
		FString ChainToString(const FEditPropertyChain& Chain, const FPropertyChangedEvent* Event)
		{
			TArray<FString> Parts;
			for (const FEditPropertyChain::TDoubleLinkedListNode* Node = Chain.GetHead(); Node; Node = Node->GetNextNode())
			{
				const FProperty* Property = Node->GetValue();
				if (!Property)
				{
					continue;
				}
				FString Part = Property->GetName();
				if (Event)
				{
					const int32 Index = Event->GetArrayIndex(Part);
					if (Index != INDEX_NONE)
					{
						Part += FString::Printf(TEXT("[%d]"), Index);
					}
				}
				Parts.Add(Part);
			}
			return FString::Join(Parts, TEXT("."));
		}

		FString StateNamesToString(const TSet<UStateTreeState*>& States)
		{
			TArray<FString> Names;
			for (const UStateTreeState* State : States)
			{
				Names.Add(State ? State->Name.ToString() : FString(TEXT("<root>")));
			}
			return FString::Join(Names, TEXT(","));
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// FDstCaptureRecorder
	// ------------------------------------------------------------------------------------------------------------

	FDstCaptureRecorder::FDstCaptureRecorder() = default;

	FDstCaptureRecorder::~FDstCaptureRecorder()
	{
		Stop();
		DetachViewModel();
	}

	void FDstCaptureRecorder::Track(const UStateTree* StateTree)
	{
		if (StateTree)
		{
			TrackedTrees.AddUnique(StateTree);
		}
	}

	void FDstCaptureRecorder::TrackPackage(const UPackage* Package)
	{
		if (Package)
		{
			TrackedPackages.AddUnique(Package);
		}
	}

	void FDstCaptureRecorder::Start()
	{
		if (bStarted)
		{
			return;
		}
		bStarted = true;

		FCoreUObjectDelegates::OnObjectModified.AddRaw(this, &FDstCaptureRecorder::HandleObjectModified);
		FCoreUObjectDelegates::OnPreObjectPropertyChanged.AddRaw(this, &FDstCaptureRecorder::HandlePreObjectPropertyChanged);
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &FDstCaptureRecorder::HandleObjectPropertyChanged);
		FCoreUObjectDelegates::OnPostObjectPropertyChanged.AddRaw(this, &FDstCaptureRecorder::HandlePostObjectPropertyChanged);
		FCoreUObjectDelegates::OnObjectTransacted.AddRaw(this, &FDstCaptureRecorder::HandleObjectTransacted);
		FCoreUObjectDelegates::OnObjectPreSave.AddRaw(this, &FDstCaptureRecorder::HandleObjectPreSave);
		UPackage::PackageMarkedDirtyEvent.AddRaw(this, &FDstCaptureRecorder::HandlePackageMarkedDirty);

		UE::StateTree::Delegates::OnIdentifierChanged.AddRaw(this, &FDstCaptureRecorder::HandleIdentifierChanged);
		UE::StateTree::Delegates::OnSchemaChanged.AddRaw(this, &FDstCaptureRecorder::HandleSchemaChanged);
		UE::StateTree::Delegates::OnParametersChanged.AddRaw(this, &FDstCaptureRecorder::HandleParametersChanged);
		UE::StateTree::Delegates::OnStateParametersChanged.AddRaw(this, &FDstCaptureRecorder::HandleStateParametersChanged);
		UE::StateTree::Delegates::OnGlobalDataChanged.AddRaw(this, &FDstCaptureRecorder::HandleGlobalDataChanged);
		UE::StateTree::Delegates::OnVisualThemeChanged.AddRaw(this, &FDstCaptureRecorder::HandleVisualThemeChanged);
		UE::StateTree::Delegates::OnBreakpointsChanged.AddRaw(this, &FDstCaptureRecorder::HandleBreakpointsChanged);
		UE::StateTree::Delegates::OnPostCompile.AddRaw(this, &FDstCaptureRecorder::HandlePostCompile);

#if DST_STATETREE_EDITOR_MODULE_HEADER
		FStateTreeEditorModule& EditorModule = FStateTreeEditorModule::GetModule();
		EditorModule.OnPostInternalCompile().AddRaw(this, &FDstCaptureRecorder::HandlePostInternalCompile);
		EditorModule.OnPreValidateStateTree().AddRaw(this, &FDstCaptureRecorder::HandlePreValidate);
		EditorModule.OnValidateStateTree().AddRaw(this, &FDstCaptureRecorder::HandleValidate);
#endif
	}

	void FDstCaptureRecorder::Stop()
	{
		if (!bStarted)
		{
			return;
		}
		bStarted = false;

		FCoreUObjectDelegates::OnObjectModified.RemoveAll(this);
		FCoreUObjectDelegates::OnPreObjectPropertyChanged.RemoveAll(this);
		FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
		FCoreUObjectDelegates::OnPostObjectPropertyChanged.RemoveAll(this);
		FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
		FCoreUObjectDelegates::OnObjectPreSave.RemoveAll(this);
		UPackage::PackageMarkedDirtyEvent.RemoveAll(this);

		UE::StateTree::Delegates::OnIdentifierChanged.RemoveAll(this);
		UE::StateTree::Delegates::OnSchemaChanged.RemoveAll(this);
		UE::StateTree::Delegates::OnParametersChanged.RemoveAll(this);
		UE::StateTree::Delegates::OnStateParametersChanged.RemoveAll(this);
		UE::StateTree::Delegates::OnGlobalDataChanged.RemoveAll(this);
		UE::StateTree::Delegates::OnVisualThemeChanged.RemoveAll(this);
		UE::StateTree::Delegates::OnBreakpointsChanged.RemoveAll(this);
		UE::StateTree::Delegates::OnPostCompile.RemoveAll(this);

#if DST_STATETREE_EDITOR_MODULE_HEADER
		if (FStateTreeEditorModule* EditorModule = FStateTreeEditorModule::GetModulePtr())
		{
			EditorModule->OnPostInternalCompile().RemoveAll(this);
			EditorModule->OnPreValidateStateTree().RemoveAll(this);
			EditorModule->OnValidateStateTree().RemoveAll(this);
		}
#endif
	}

	void FDstCaptureRecorder::AttachViewModel(const TSharedPtr<FStateTreeViewModel>& ViewModel)
	{
		DetachViewModel();
		if (!ViewModel.IsValid())
		{
			return;
		}
		AttachedViewModel = ViewModel;
		ViewModel->GetOnAssetChanged().AddRaw(this, &FDstCaptureRecorder::HandleViewModelAssetChanged);
		ViewModel->GetOnStatesChanged().AddRaw(this, &FDstCaptureRecorder::HandleViewModelStatesChanged);
		ViewModel->GetOnStateAdded().AddRaw(this, &FDstCaptureRecorder::HandleViewModelStateAdded);
		ViewModel->GetOnStatesRemoved().AddRaw(this, &FDstCaptureRecorder::HandleViewModelStatesRemoved);
		ViewModel->GetOnStatesMoved().AddRaw(this, &FDstCaptureRecorder::HandleViewModelStatesMoved);
		ViewModel->GetOnStateNodesChanged().AddRaw(this, &FDstCaptureRecorder::HandleViewModelStateNodesChanged);
		ViewModel->GetOnSelectionChanged().AddRaw(this, &FDstCaptureRecorder::HandleViewModelSelectionChanged);
	}

	void FDstCaptureRecorder::DetachViewModel()
	{
		if (const TSharedPtr<FStateTreeViewModel> ViewModel = AttachedViewModel.Pin())
		{
			ViewModel->GetOnAssetChanged().RemoveAll(this);
			ViewModel->GetOnStatesChanged().RemoveAll(this);
			ViewModel->GetOnStateAdded().RemoveAll(this);
			ViewModel->GetOnStatesRemoved().RemoveAll(this);
			ViewModel->GetOnStatesMoved().RemoveAll(this);
			ViewModel->GetOnStateNodesChanged().RemoveAll(this);
			ViewModel->GetOnSelectionChanged().RemoveAll(this);
		}
		AttachedViewModel.Reset();
	}

	int32 FDstCaptureRecorder::BeginSegment(const FString& Label)
	{
		SegmentLabels.Add(Label);
		CurrentSegment = SegmentLabels.Num() - 1;
		// Modify() notifications are de-duplicated per frame; a test runs inside one frame, so reset the filter to see
		// each operation's own Modify calls.
		FCoreUObjectDelegates::ObjectsModifiedThisFrame.Reset();
		return CurrentSegment;
	}

	const FString& FDstCaptureRecorder::GetSegmentLabel(int32 Segment) const
	{
		static const FString Empty;
		return SegmentLabels.IsValidIndex(Segment) ? SegmentLabels[Segment] : Empty;
	}

	void FDstCaptureRecorder::Note(const FString& Text)
	{
		Add(TEXT("Note"), nullptr, Text);
	}

	TArray<FDstCaptureEvent> FDstCaptureRecorder::GetSegmentEvents(int32 Segment) const
	{
		TArray<FDstCaptureEvent> Result;
		for (const FDstCaptureEvent& Event : Events)
		{
			if (Event.Segment == Segment)
			{
				Result.Add(Event);
			}
		}
		return Result;
	}

	int32 FDstCaptureRecorder::CountEvents(int32 Segment, const FString& Kind, const FString& ClassName) const
	{
		int32 Count = 0;
		for (const FDstCaptureEvent& Event : Events)
		{
			if (Event.Segment == Segment && Event.Kind == Kind && (ClassName.IsEmpty() || Event.ClassName == ClassName))
			{
				++Count;
			}
		}
		return Count;
	}

	bool FDstCaptureRecorder::HasEventMentioning(int32 Segment, const FString& Kind, const FString& Text) const
	{
		for (const FDstCaptureEvent& Event : Events)
		{
			if (Event.Segment == Segment && Event.Kind == Kind && Event.Detail.Contains(Text))
			{
				return true;
			}
		}
		return false;
	}

	FString FDstCaptureRecorder::SummarizeSegment(int32 Segment) const
	{
		struct FGroup
		{
			FString Kind;
			FString ClassName;
			int32 Count = 0;
			TArray<FString> Details;
		};
		TArray<FGroup> Groups;
		for (const FDstCaptureEvent& Event : Events)
		{
			if (Event.Segment != Segment)
			{
				continue;
			}
			FGroup* Group = Groups.FindByPredicate([&Event](const FGroup& Other)
			{
				return Other.Kind == Event.Kind && Other.ClassName == Event.ClassName;
			});
			if (!Group)
			{
				Group = &Groups.AddDefaulted_GetRef();
				Group->Kind = Event.Kind;
				Group->ClassName = Event.ClassName;
			}
			++Group->Count;
			if (!Event.Detail.IsEmpty())
			{
				Group->Details.AddUnique(Event.Detail);
			}
		}

		if (Groups.IsEmpty())
		{
			return TEXT("    (no events)");
		}

		FString Out;
		for (const FGroup& Group : Groups)
		{
			const FString ClassPart = Group.ClassName.IsEmpty() ? FString() : FString::Printf(TEXT("(%s)"), *Group.ClassName);
			Out += FString::Printf(TEXT("    %s%s x%d"), *Group.Kind, *ClassPart, Group.Count);
			const int32 MaxDetails = 3;
			for (int32 Index = 0; Index < Group.Details.Num() && Index < MaxDetails; ++Index)
			{
				Out += (Index == 0 ? TEXT("  { ") : TEXT(" ; "));
				Out += Group.Details[Index];
			}
			if (!Group.Details.IsEmpty())
			{
				Out += Group.Details.Num() > MaxDetails ? FString::Printf(TEXT(" ; +%d more }"), Group.Details.Num() - MaxDetails) : FString(TEXT(" }"));
			}
			Out += TEXT("\n");
		}
		Out.TrimEndInline();
		return Out;
	}

	FString FDstCaptureRecorder::FormatSegment(int32 Segment) const
	{
		FString Out;
		for (const FDstCaptureEvent& Event : Events)
		{
			if (Event.Segment != Segment)
			{
				continue;
			}
			Out += FString::Printf(TEXT("  #%03d %-24s %-22s %s%s%s\n"),
				Event.Sequence, *Event.Kind, *Event.ClassName, *Event.ObjectPath,
				Event.Detail.IsEmpty() ? TEXT("") : TEXT("  | "), *Event.Detail);
		}
		return Out;
	}

	FString FDstCaptureRecorder::FormatAll() const
	{
		FString Out = FString::Printf(TEXT("FStateTreeEditorModule events (OnPostInternalCompile, OnPreValidateStateTree, OnValidateStateTree) hooked: %s\n\n"),
			DstModuleHooksCompiledIn() ? TEXT("yes") : TEXT("NO - the Kismet include path is missing in this module, see SpikeCaptureHelpers.cpp"));
		for (int32 Segment = INDEX_NONE; Segment < SegmentLabels.Num(); ++Segment)
		{
			const FString Body = FormatSegment(Segment);
			if (Segment == INDEX_NONE && Body.IsEmpty())
			{
				continue;
			}
			Out += FString::Printf(TEXT("== Segment %d: %s ==\n"), Segment, Segment == INDEX_NONE ? TEXT("(before the first segment)") : *SegmentLabels[Segment]);
			Out += Body.IsEmpty() ? FString(TEXT("  (no events)\n")) : Body;
			Out += TEXT("\n");
		}
		return Out;
	}

	bool FDstCaptureRecorder::IsTracked(const UObject* Object) const
	{
		for (const UObject* Current = Object; Current; Current = Current->GetOuter())
		{
			for (const TWeakObjectPtr<const UStateTree>& Tracked : TrackedTrees)
			{
				if (Tracked.Get() == Current)
				{
					return true;
				}
			}
			for (const TWeakObjectPtr<const UPackage>& Tracked : TrackedPackages)
			{
				if (Tracked.Get() == Current)
				{
					return true;
				}
			}
		}
		return false;
	}

	bool FDstCaptureRecorder::IsTrackedTree(const UStateTree* StateTree) const
	{
		return StateTree && IsTracked(StateTree);
	}

	FString FDstCaptureRecorder::Describe(const UObject* Object, FString* OutClassName) const
	{
		if (OutClassName)
		{
			*OutClassName = Object->GetClass()->GetName();
		}

		const UStateTree* Owner = nullptr;
		for (const UObject* Current = Object; Current; Current = Current->GetOuter())
		{
			if (const UStateTree* Tree = Cast<const UStateTree>(Current))
			{
				Owner = Tree;
				break;
			}
		}

		FString Path;
		if (Owner == Object)
		{
			Path = Owner->GetName();
		}
		else if (Owner)
		{
			Path = Owner->GetName() + TEXT(":") + Object->GetPathName(Owner);
		}
		else
		{
			Path = Object->GetPathName();
		}

		if (const UStateTreeState* State = Cast<const UStateTreeState>(Object))
		{
			Path += FString::Printf(TEXT(" '%s'"), *State->Name.ToString());
		}
		return Path;
	}

	FDstCaptureEvent* FDstCaptureRecorder::AddEvent(const TCHAR* Kind, const UObject* Object, const FString& Detail)
	{
		if (bPaused)
		{
			return nullptr;
		}
		FDstCaptureEvent& Event = Events.AddDefaulted_GetRef();
		Event.Sequence = NextSequence++;
		Event.Segment = CurrentSegment;
		Event.Kind = Kind;
		if (Object)
		{
			Event.ObjectPath = Describe(Object, &Event.ClassName);
		}
		Event.Detail = Detail;
		return &Event;
	}

	void FDstCaptureRecorder::Add(const TCHAR* Kind, const UObject* Object, const FString& Detail)
	{
		AddEvent(Kind, Object, Detail);
	}

	void FDstCaptureRecorder::AddTreeEvent(const TCHAR* Kind, const UStateTree& StateTree, const FString& Detail)
	{
		if (IsTrackedTree(&StateTree))
		{
			Add(Kind, &StateTree, Detail);
		}
	}

	void FDstCaptureRecorder::HandleObjectModified(UObject* Object)
	{
		if (IsTracked(Object))
		{
			Add(TEXT("Modify"), Object, FString());
		}
	}

	void FDstCaptureRecorder::HandlePreObjectPropertyChanged(UObject* Object, const FEditPropertyChain& Chain)
	{
		if (IsTracked(Object))
		{
			Add(TEXT("PreEditChain"), Object, FString::Printf(TEXT("Chain=%s"), *ChainToString(Chain, nullptr)));
		}
	}

	void FDstCaptureRecorder::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
	{
		if (IsTracked(Object))
		{
			Add(TEXT("PostEdit"), Object, FString::Printf(TEXT("Property=%s Member=%s ChangeType=%s"),
				Event.Property ? *Event.Property->GetName() : TEXT("<null>"),
				Event.MemberProperty ? *Event.MemberProperty->GetName() : TEXT("<null>"),
				*ChangeTypeToString(Event.ChangeType)));
		}
	}

	void FDstCaptureRecorder::HandlePostObjectPropertyChanged(UObject* Object, const FPropertyChangedChainEvent& Event)
	{
		if (IsTracked(Object))
		{
			Add(TEXT("PostEditChain"), Object, FString::Printf(TEXT("Chain=%s ChangeType=%s"),
				*ChainToString(Event.PropertyChain, &Event), *ChangeTypeToString(Event.ChangeType)));
		}
	}

	void FDstCaptureRecorder::HandleObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event)
	{
		if (!IsTracked(Object))
		{
			return;
		}

		const TCHAR* TypeName = TEXT("Snapshot");
		switch (Event.GetEventType())
		{
		case ETransactionObjectEventType::UndoRedo: TypeName = TEXT("UndoRedo"); break;
		case ETransactionObjectEventType::Finalized: TypeName = TEXT("Finalized"); break;
		case ETransactionObjectEventType::Snapshot: TypeName = TEXT("Snapshot"); break;
		}

		TArray<FString> Properties;
		for (const FName& Property : Event.GetChangedProperties())
		{
			Properties.Add(Property.ToString());
		}

		Add(TEXT("Transacted"), Object, FString::Printf(TEXT("Type=%s Props=[%s] NonProp=%d Name=%d Outer=%d Kill=%d"),
			TypeName, *FString::Join(Properties, TEXT(",")),
			Event.HasNonPropertyChanges(true) ? 1 : 0, Event.HasNameChange() ? 1 : 0,
			Event.HasOuterChange() ? 1 : 0, Event.HasPendingKillChange() ? 1 : 0));
	}

	void FDstCaptureRecorder::HandleObjectPreSave(UObject* Object, FObjectPreSaveContext Context)
	{
		if (IsTracked(Object))
		{
			Add(TEXT("PreSave"), Object, FString::Printf(TEXT("Procedural=%d AutoSave=%d"),
				Context.IsProceduralSave() ? 1 : 0, Context.IsFromAutoSave() ? 1 : 0));
		}
	}

	void FDstCaptureRecorder::HandlePackageMarkedDirty(UPackage* Package, bool bWasDirty)
	{
		// The package is its own outer-chain root, so the generic check covers tracked trees and tracked packages alike.
		if (IsTracked(Package))
		{
			Add(TEXT("PackageDirty"), Package, FString::Printf(TEXT("WasDirty=%d"), bWasDirty ? 1 : 0));
			return;
		}
		for (const TWeakObjectPtr<const UStateTree>& Tracked : TrackedTrees)
		{
			if (const UStateTree* Tree = Tracked.Get())
			{
				if (Tree->GetPackage() == Package)
				{
					Add(TEXT("PackageDirty"), Package, FString::Printf(TEXT("WasDirty=%d"), bWasDirty ? 1 : 0));
					return;
				}
			}
		}
	}

	void FDstCaptureRecorder::HandleIdentifierChanged(const UStateTree& StateTree)
	{
		AddTreeEvent(TEXT("ST.IdentifierChanged"), StateTree, FString());
	}

	void FDstCaptureRecorder::HandleSchemaChanged(const UStateTree& StateTree)
	{
		AddTreeEvent(TEXT("ST.SchemaChanged"), StateTree, FString());
	}

	void FDstCaptureRecorder::HandleParametersChanged(const UStateTree& StateTree)
	{
		AddTreeEvent(TEXT("ST.ParametersChanged"), StateTree, FString());
	}

	void FDstCaptureRecorder::HandleStateParametersChanged(const UStateTree& StateTree, const FGuid StateID)
	{
		AddTreeEvent(TEXT("ST.StateParametersChanged"), StateTree, FString::Printf(TEXT("State=%s"), *DstShortGuid(StateID)));
	}

	void FDstCaptureRecorder::HandleGlobalDataChanged(const UStateTree& StateTree)
	{
		AddTreeEvent(TEXT("ST.GlobalDataChanged"), StateTree, FString());
	}

	void FDstCaptureRecorder::HandleVisualThemeChanged(const UStateTree& StateTree)
	{
		AddTreeEvent(TEXT("ST.VisualThemeChanged"), StateTree, FString());
	}

	void FDstCaptureRecorder::HandleBreakpointsChanged(const UStateTree& StateTree)
	{
		AddTreeEvent(TEXT("ST.BreakpointsChanged"), StateTree, FString());
	}

	void FDstCaptureRecorder::HandlePostCompile(const UStateTree& StateTree)
	{
		AddTreeEvent(TEXT("ST.PostCompile"), StateTree, FString());
	}

	void FDstCaptureRecorder::HandlePostInternalCompile(const UE::StateTree::Compiler::FPostInternalContext& Context)
	{
		const UStateTree* StateTree = Context.GetStateTree();
		if (IsTrackedTree(StateTree))
		{
			Add(TEXT("ST.PostInternalCompile"), StateTree, FString());
		}
	}

	void FDstCaptureRecorder::HandlePreValidate(TNotNull<UStateTree*> StateTree)
	{
		AddTreeEvent(TEXT("ST.PreValidate"), *StateTree, FString());
	}

	void FDstCaptureRecorder::HandleValidate(TNotNull<UStateTree*> StateTree)
	{
		AddTreeEvent(TEXT("ST.Validate"), *StateTree, FString());
	}

	void FDstCaptureRecorder::AddViewModelEvent(const TCHAR* Kind, const FString& Detail)
	{
		if (FDstCaptureEvent* Event = AddEvent(Kind, nullptr, Detail))
		{
			Event->ClassName = TEXT("FStateTreeViewModel");
		}
	}

	void FDstCaptureRecorder::HandleViewModelAssetChanged()
	{
		AddViewModelEvent(TEXT("VM.AssetChanged"), FString());
	}

	void FDstCaptureRecorder::HandleViewModelStatesChanged(const TSet<UStateTreeState*>& AffectedStates, const FPropertyChangedEvent& Event)
	{
		AddViewModelEvent(TEXT("VM.StatesChanged"), FString::Printf(TEXT("States=[%s] Property=%s ChangeType=%s"),
			*StateNamesToString(AffectedStates), *Event.GetPropertyName().ToString(), *ChangeTypeToString(Event.ChangeType)));
	}

	void FDstCaptureRecorder::HandleViewModelStateAdded(UStateTreeState* ParentState, UStateTreeState* NewState)
	{
		const FString ParentName = ParentState ? ParentState->Name.ToString() : FString(TEXT("<root>"));
		const FString NewName = NewState ? NewState->Name.ToString() : FString(TEXT("<null>"));
		AddViewModelEvent(TEXT("VM.StateAdded"), FString::Printf(TEXT("Parent='%s' New='%s'"), *ParentName, *NewName));
	}

	void FDstCaptureRecorder::HandleViewModelStatesRemoved(const TSet<UStateTreeState*>& AffectedParents)
	{
		AddViewModelEvent(TEXT("VM.StatesRemoved"), FString::Printf(TEXT("Parents=[%s]"), *StateNamesToString(AffectedParents)));
	}

	void FDstCaptureRecorder::HandleViewModelStatesMoved(const TSet<UStateTreeState*>& AffectedParents, const TSet<UStateTreeState*>& MovedStates)
	{
		AddViewModelEvent(TEXT("VM.StatesMoved"), FString::Printf(TEXT("Parents=[%s] Moved=[%s]"),
			*StateNamesToString(AffectedParents), *StateNamesToString(MovedStates)));
	}

	void FDstCaptureRecorder::HandleViewModelStateNodesChanged(const UStateTreeState* AffectedState)
	{
		const FString Name = AffectedState ? AffectedState->Name.ToString() : FString(TEXT("<null>"));
		AddViewModelEvent(TEXT("VM.StateNodesChanged"), FString::Printf(TEXT("State='%s'"), *Name));
	}

	void FDstCaptureRecorder::HandleViewModelSelectionChanged(const TArray<TWeakObjectPtr<UStateTreeState>>& SelectedStates)
	{
		TArray<FString> Names;
		for (const TWeakObjectPtr<UStateTreeState>& Weak : SelectedStates)
		{
			const UStateTreeState* State = Weak.Get();
			Names.Add(State ? State->Name.ToString() : FString(TEXT("<null>")));
		}
		AddViewModelEvent(TEXT("VM.SelectionChanged"), FString::Printf(TEXT("Selected=[%s]"), *FString::Join(Names, TEXT(","))));
	}

	// ------------------------------------------------------------------------------------------------------------
	// FDstCaptureFixture
	// ------------------------------------------------------------------------------------------------------------

	const FName FDstCaptureFixture::SpeedParameterName(TEXT("Speed"));

	FDstCaptureFixture::FDstCaptureFixture() = default;

	FDstCaptureFixture::~FDstCaptureFixture()
	{
		Destroy();
	}

	UStateTreeEditorData* FDstCaptureFixture::GetEditorData() const
	{
		return StateTree.IsValid() ? Cast<UStateTreeEditorData>(StateTree->EditorData) : nullptr;
	}

	UPackage* FDstCaptureFixture::GetPackage() const
	{
		return StateTree.IsValid() ? StateTree->GetOutermost() : nullptr;
	}

	UStateTreeState* FDstCaptureFixture::FindState(const FGuid& StateId) const
	{
		UStateTreeEditorData* EditorData = GetEditorData();
		return EditorData ? EditorData->GetMutableStateByID(StateId) : nullptr;
	}

	FStateTreeEditorNode* FDstCaptureFixture::FindNode(const FGuid& StateId, const FGuid& NodeId) const
	{
		UStateTreeState* State = FindState(StateId);
		if (!State)
		{
			return nullptr;
		}
		for (TArray<FStateTreeEditorNode>* Array : { &State->EnterConditions, &State->Tasks, &State->Considerations })
		{
			for (FStateTreeEditorNode& Node : *Array)
			{
				if (Node.ID == NodeId)
				{
					return &Node;
				}
			}
		}
		return nullptr;
	}

	FStateTreeTransition* FDstCaptureFixture::FindTransition(const FGuid& StateId, const FGuid& TransitionId) const
	{
		UStateTreeState* State = FindState(StateId);
		if (!State)
		{
			return nullptr;
		}
		return State->Transitions.FindByPredicate([&TransitionId](const FStateTreeTransition& Transition)
		{
			return Transition.ID == TransitionId;
		});
	}

	TSharedPtr<FStateTreeViewModel> FDstCaptureFixture::GetOrCreateViewModel()
	{
		if (ViewModel.IsValid())
		{
			return ViewModel;
		}
		UStateTree* Tree = GetTree();
		if (!Tree || !GEditor)
		{
			return nullptr;
		}

		if (UStateTreeEditingSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStateTreeEditingSubsystem>())
		{
			ViewModel = Subsystem->FindOrAddViewModel(Tree);
		}
		else
		{
			// The editing subsystem is not instantiated: build the view model by hand, which is all the subsystem does.
			ViewModel = MakeShared<FStateTreeViewModel>();
			ViewModel->Init(GetEditorData());
		}
		return ViewModel;
	}

	void FDstCaptureFixture::ResetViewModel()
	{
		ViewModel.Reset();
	}

	FString FDstCaptureFixture::Dump(bool bIncludeIds) const
	{
		const UStateTreeEditorData* EditorData = GetEditorData();
		if (!EditorData)
		{
			return FString();
		}
		FDumpOptions Options;
		Options.bIncludeIds = bIncludeIds;
		return DumpEditorData(*EditorData, Options);
	}

	uint32 FDstCaptureFixture::EditorDataHash() const
	{
		const UStateTree* Tree = GetTree();
		return Tree ? UStateTreeEditingSubsystem::CalculateStateTreeHash(Tree) : 0u;
	}

	bool FDstCaptureFixture::Compile(FString& OutMessages) const
	{
		UStateTree* Tree = GetTree();
		if (!Tree)
		{
			return false;
		}
		FDstLogSilencer Silencer;
		FStateTreeCompilerLog Log;
		const bool bResult = UStateTreeEditingSubsystem::CompileStateTree(Tree, Log);
		for (const TSharedRef<FTokenizedMessage>& Message : Log.ToTokenizedMessages())
		{
			OutMessages += FString::Printf(TEXT("[severity %d] %s\n"), static_cast<int32>(Message->GetSeverity()), *Message->ToText().ToString());
		}
		return bResult;
	}

	void FDstCaptureFixture::AdoptExtraTree(UStateTree* ExtraTree, bool bRegisteredInAssetRegistry)
	{
		if (ExtraTree)
		{
			FExtraTree& Entry = ExtraTrees.AddDefaulted_GetRef();
			Entry.Tree.Reset(ExtraTree);
			Entry.bRegisteredInAssetRegistry = bRegisteredInAssetRegistry;
		}
	}

	bool FDstCaptureFixture::Initialize(const TCHAR* BaseName, FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is null: the test needs the editor.");
			return false;
		}

		static int32 AssetCounter = 0;
		const FString AssetName = FString::Printf(TEXT("ST_%s_%d"), BaseName, ++AssetCounter);
		const FString PackageName = FString::Printf(TEXT("/Game/DreamStateTreeTests/Spike/Capture/%s"), *AssetName);

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("CreatePackage failed for %s."), *PackageName);
			return false;
		}

		// The same factory the Content Browser uses: creates the asset, editor data, editor schema, a root state, and compiles.
		TStrongObjectPtr<UStateTreeFactory> Factory(NewObject<UStateTreeFactory>());
		Factory->SetSchemaClass(UStateTreeComponentSchema::StaticClass());
		UObject* Created = Factory->FactoryCreateNew(UStateTree::StaticClass(), Package, FName(*AssetName),
			RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
		UStateTree* NewTree = Cast<UStateTree>(Created);
		if (!NewTree)
		{
			OutError = TEXT("UStateTreeFactory::FactoryCreateNew returned no asset (its initial compile failed or the schema was rejected).");
			return false;
		}
		StateTree.Reset(NewTree);

		if (!BuildBaseline(OutError))
		{
			return false;
		}

		// Start from a clean undo stack and a clean package so that every operation's effects are the only ones visible.
		GEditor->ResetTransaction(FText::FromString(TEXT("DreamStateTree capture spike: fixture ready")));
		if (UPackage* AssetPackage = GetPackage())
		{
			AssetPackage->SetDirtyFlag(false);
		}
		return true;
	}

	bool FDstCaptureFixture::BuildBaseline(FString& OutError)
	{
		UStateTree* Tree = GetTree();
		UStateTreeEditorData* EditorData = GetEditorData();
		if (!Tree || !EditorData || EditorData->SubTrees.IsEmpty() || !EditorData->SubTrees[0])
		{
			OutError = TEXT("The factory made an asset without editor data or without a root state.");
			return false;
		}

		UScriptStruct* DelayStruct = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeDelayTask"));
		UScriptStruct* RandomStruct = DstFindNodeStruct(TEXT("StateTreeModule"), TEXT("StateTreeRandomCondition"));
		if (!DelayStruct || !RandomStruct)
		{
			OutError = TEXT("Could not find the engine's Delay task or Random condition struct by name.");
			return false;
		}

		UStateTreeState* Root = EditorData->SubTrees[0];
		UStateTreeState& A = Root->AddChildState(TEXT("A"));
		UStateTreeState& A1 = A.AddChildState(TEXT("A1"));
		UStateTreeState& B = Root->AddChildState(TEXT("B"));
		Ids.Root = Root->ID;
		Ids.A = A.ID;
		Ids.A1 = A1.ID;
		Ids.B = B.ID;

		auto AddNode = [](UStateTreeState& Owner, TArray<FStateTreeEditorNode>& Array, UScriptStruct* NodeStruct, const TCHAR* Name) -> FGuid
		{
			FStateTreeEditorNode& Node = Array.AddDefaulted_GetRef();
			Node.InitializeAs(&Owner, NodeStruct);
			Node.SetNodeName(FName(Name));
			return Node.ID;
		};

		Ids.ATask = AddNode(A, A.Tasks, DelayStruct, TEXT("WaitA"));
		Ids.BTask = AddNode(B, B.Tasks, DelayStruct, TEXT("WaitB"));
		Ids.ACondFirst = AddNode(A, A.EnterConditions, RandomStruct, TEXT("ChanceFirst"));
		Ids.ACondSecond = AddNode(A, A.EnterConditions, RandomStruct, TEXT("ChanceSecond"));
		if (FStateTreeEditorNode* Second = FindNode(Ids.A, Ids.ACondSecond))
		{
			Second->ExpressionOperand = EStateTreeExpressionOperand::And;
		}
		if (FStateTreeEditorNode* Task = FindNode(Ids.A, Ids.ATask))
		{
			if (!DstSetFloat(Task->Instance.GetScriptStruct(), Task->Instance.GetMutableMemory(), TEXT("Duration"), 1.0f))
			{
				OutError = TEXT("The Delay task instance data has no float property 'Duration'.");
				return false;
			}
		}

		FStateTreeTransition& Transition = A.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::NextState);
		Ids.ATransition = Transition.ID;

		// Root parameter "Speed" bound to the Duration of A's task.
		FInstancedPropertyBag* RootBag = DstGetRootParameterBag(*EditorData);
		if (!RootBag
			|| RootBag->AddProperty(SpeedParameterName, EPropertyBagPropertyType::Float) != EPropertyBagAlterationResult::Success
			|| RootBag->SetValueFloat(SpeedParameterName, 2.0f) != EPropertyBagResult::Success)
		{
			OutError = TEXT("Could not add the root parameter 'Speed'.");
			return false;
		}
		const FPropertyBindingPath SourcePath(EditorData->GetRootParametersGuid(), SpeedParameterName);
		const FPropertyBindingPath TargetPath(Ids.ATask, FName(TEXT("Duration")));
		EditorData->AddPropertyBinding(SourcePath, TargetPath);
		EditorData->UpdateBindings();

		UStateTreeEditingSubsystem::MarkAsPubliclyModified(Tree);
		// Same normalisation the editor runs when it opens an asset.
		UStateTreeEditingSubsystem::ValidateStateTree(Tree);

		if (EditorData->EditorBindings.GetBindings().Num() != 1)
		{
			OutError = FString::Printf(TEXT("The baseline binding did not survive validation (bindings left: %d)."), EditorData->EditorBindings.GetBindings().Num());
			return false;
		}
		return true;
	}

	void FDstCaptureFixture::Destroy()
	{
		ViewModel.Reset();

		// The undo buffer holds references to the objects we are about to drop.
		if (GEditor)
		{
			GEditor->ResetTransaction(FText::FromString(TEXT("DreamStateTree capture spike: cleanup")));
		}

		auto Release = [](UStateTree* Tree, bool bRegisteredInAssetRegistry)
		{
			if (!Tree)
			{
				return;
			}
			if (bRegisteredInAssetRegistry)
			{
				FAssetRegistryModule::AssetDeleted(Tree);
			}
			if (UPackage* Package = Tree->GetOutermost())
			{
				Package->SetDirtyFlag(false);
			}
			// Without these flags nothing keeps the asset alive, so the next garbage collection drops it with its package.
			Tree->ClearFlags(RF_Public | RF_Standalone);
		};

		for (FExtraTree& Extra : ExtraTrees)
		{
			Release(Extra.Tree.Get(), Extra.bRegisteredInAssetRegistry);
			Extra.Tree.Reset();
		}
		ExtraTrees.Reset();

		Release(StateTree.Get(), false);
		const bool bHadTree = StateTree.IsValid();
		StateTree.Reset();

		if (bHadTree)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// Free helpers
	// ------------------------------------------------------------------------------------------------------------

	FProperty* DstFindProperty(const UStruct* Struct, const TCHAR* Name)
	{
		return Struct ? Struct->FindPropertyByName(FName(Name)) : nullptr;
	}

	UScriptStruct* DstFindNodeStruct(const TCHAR* ModuleName, const TCHAR* StructName)
	{
		const FString PackagePath = FString::Printf(TEXT("/Script/%s"), ModuleName);
		return FindObject<UScriptStruct>(FTopLevelAssetPath(FName(*PackagePath), FName(StructName)));
	}

	UScriptStruct* DstGetEditorNodeStruct()
	{
		return DstFindNodeStruct(TEXT("StateTreeEditorModule"), TEXT("StateTreeEditorNode"));
	}

	void DstApplyPropertyEdit(const FDstPropertyEdit& Edit, TFunctionRef<void()> Mutate)
	{
		check(Edit.Owner);
		check(!Edit.Chain.IsEmpty());

		TOptional<FScopedTransaction> Transaction;
		if (!Edit.TransactionText.IsEmpty())
		{
			Transaction.Emplace(Edit.TransactionText);
		}

		// What the property editor does: PreEditChange with the full chain (which also calls Modify), change, then
		// PostEditChangeChainProperty with array indices and the active member set.
		Edit.Owner->Modify();

		FEditPropertyChain Chain;
		for (FProperty* Property : Edit.Chain)
		{
			Chain.AddTail(Property);
		}
		Chain.SetActiveMemberPropertyNode(Edit.Chain[0]);
		Edit.Owner->PreEditChange(Chain);

		Mutate();

		TArray<TMap<FString, int32>> ArrayIndicesPerObject;
		if (!Edit.ArrayIndices.IsEmpty())
		{
			ArrayIndicesPerObject.Add(Edit.ArrayIndices);
		}
		FPropertyChangedEvent ChangedEvent(Chain.GetActiveNode()->GetValue(), Edit.ChangeType);
		ChangedEvent.SetArrayIndexPerObject(ArrayIndicesPerObject);
		if (!ArrayIndicesPerObject.IsEmpty())
		{
			ChangedEvent.ObjectIteratorIndex = 0;
		}
		FPropertyChangedChainEvent ChainEvent(Chain, ChangedEvent);
		Edit.Owner->PostEditChangeChainProperty(ChainEvent);
	}

	bool DstSetFloat(const UStruct* Struct, void* Memory, const TCHAR* PropertyName, float Value)
	{
		if (const FFloatProperty* Property = CastField<FFloatProperty>(DstFindProperty(Struct, PropertyName)))
		{
			Property->SetPropertyValue_InContainer(Memory, Value);
			return true;
		}
		return false;
	}

	bool DstSetBool(const UStruct* Struct, void* Memory, const TCHAR* PropertyName, bool bValue)
	{
		if (const FBoolProperty* Property = CastField<FBoolProperty>(DstFindProperty(Struct, PropertyName)))
		{
			Property->SetPropertyValue_InContainer(Memory, bValue);
			return true;
		}
		return false;
	}

	FInstancedPropertyBag* DstGetRootParameterBag(UStateTreeEditorData& EditorData)
	{
		// RootParameterPropertyBag is private; reflection reaches it the same way the property editor does.
		const FStructProperty* Property = CastField<FStructProperty>(DstFindProperty(UStateTreeEditorData::StaticClass(), TEXT("RootParameterPropertyBag")));
		return Property ? Property->ContainerPtrToValuePtr<FInstancedPropertyBag>(&EditorData) : nullptr;
	}

	FDstClipboardGuard::FDstClipboardGuard()
	{
		FPlatformApplicationMisc::ClipboardPaste(Saved);
	}

	FDstClipboardGuard::~FDstClipboardGuard()
	{
		FPlatformApplicationMisc::ClipboardCopy(*Saved);
	}

	FDstLogSilencer::FDstLogSilencer()
		: Previous(LogStateTreeEditor.GetVerbosity())
		, PreviousRuntime(LogStateTree.GetVerbosity())
	{
		LogStateTreeEditor.SetVerbosity(ELogVerbosity::Fatal);
		LogStateTree.SetVerbosity(ELogVerbosity::Fatal);
	}

	FDstLogSilencer::~FDstLogSilencer()
	{
		LogStateTreeEditor.SetVerbosity(Previous);
		LogStateTree.SetVerbosity(PreviousRuntime);
	}

	FString DstShortGuid(const FGuid& Id)
	{
		return Id.IsValid() ? Id.ToString(EGuidFormats::Digits).Left(8) : FString(TEXT("--------"));
	}
}
