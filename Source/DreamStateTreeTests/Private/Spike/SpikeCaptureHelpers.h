// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Misc/NotNull.h"
#include "Misc/TransactionObjectEvent.h"
#include "StateTree.h"
#include "StateTreeState.h"
#include "StructUtils/PropertyBag.h"
#include "Templates/SharedPointer.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

class FStateTreeViewModel;
class UStateTreeEditorData;

namespace UE::StateTree::Compiler
{
	struct FPostInternalContext;
}

// Shared pieces of the editor-notification experiments: a recorder that listens to every engine hook an external
// observer could use, a fixture that builds a small real asset, and a helper that edits a property the way the
// Details panel does (transaction, PreEditChange with a property chain, change, PostEditChangeChainProperty).
namespace UE::DreamStateTree::Spike
{
	/** One notification seen by the recorder. */
	struct FDstCaptureEvent
	{
		/** Global order of arrival. */
		int32 Sequence = 0;
		/** Index of the segment (one segment per operation) the event arrived in. */
		int32 Segment = INDEX_NONE;
		/** Hook that fired, e.g. "Transacted", "PostEditChain", "ST.IdentifierChanged", "VM.StatesChanged". */
		FString Kind;
		/** Class of the object the event is about; empty for events that carry only the asset. */
		FString ClassName;
		/** Path of the object relative to the asset. */
		FString ObjectPath;
		/** Free-form details: changed property chain, transaction event type, change type. */
		FString Detail;
	};

	/**
	 * Listens to the engine and StateTree hooks that could drive a write-back, and keeps what they report.
	 * Only objects inside the tracked assets are recorded. Events are grouped in segments so each editor operation
	 * can be read back on its own.
	 */
	class FDstCaptureRecorder : public FNoncopyable
	{
	public:
		FDstCaptureRecorder();
		~FDstCaptureRecorder();

		/** Records events about this asset and everything inside it. */
		void Track(const UStateTree* StateTree);
		/** Records events about everything in this package; use it for an asset that does not exist yet (a copy being made). */
		void TrackPackage(const UPackage* Package);

		/** Subscribes to all hooks. */
		void Start();
		/** Unsubscribes; recorded events stay available. */
		void Stop();

		/** Also records the notifications of the view model the editor UI listens to. */
		void AttachViewModel(const TSharedPtr<FStateTreeViewModel>& ViewModel);
		void DetachViewModel();

		/** While paused, events are dropped. Used around bookkeeping calls such as a compile between two operations. */
		void SetPaused(bool bInPaused) { bPaused = bInPaused; }

		/** Starts a new segment; later events belong to it. Returns its index. */
		int32 BeginSegment(const FString& Label);
		int32 GetCurrentSegment() const { return CurrentSegment; }
		const FString& GetSegmentLabel(int32 Segment) const;

		/** Adds a free-form line to the current segment. */
		void Note(const FString& Text);

		TArray<FDstCaptureEvent> GetSegmentEvents(int32 Segment) const;
		int32 CountEvents(int32 Segment, const FString& Kind, const FString& ClassName = FString()) const;
		/** True if an event of that kind in the segment mentions the property name in its details. */
		bool HasEventMentioning(int32 Segment, const FString& Kind, const FString& Text) const;

		/** The segment's events grouped by kind and class, with counts and distinct details. Short enough for AddInfo. */
		FString SummarizeSegment(int32 Segment) const;
		/** Every event of the segment, one line each. */
		FString FormatSegment(int32 Segment) const;
		/** Every segment with all of its events. */
		FString FormatAll() const;

	private:
		bool IsTracked(const UObject* Object) const;
		bool IsTrackedTree(const UStateTree* StateTree) const;
		void Add(const TCHAR* Kind, const UObject* Object, const FString& Detail);
		FDstCaptureEvent* AddEvent(const TCHAR* Kind, const UObject* Object, const FString& Detail);
		void AddViewModelEvent(const TCHAR* Kind, const FString& Detail);
		void AddTreeEvent(const TCHAR* Kind, const UStateTree& StateTree, const FString& Detail);
		FString Describe(const UObject* Object, FString* OutClassName) const;

		// Engine hooks.
		void HandleObjectModified(UObject* Object);
		void HandlePreObjectPropertyChanged(UObject* Object, const FEditPropertyChain& Chain);
		void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
		void HandlePostObjectPropertyChanged(UObject* Object, const FPropertyChangedChainEvent& Event);
		void HandleObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event);
		void HandleObjectPreSave(UObject* Object, FObjectPreSaveContext Context);
		void HandlePackageMarkedDirty(UPackage* Package, bool bWasDirty);

		// StateTree hooks.
		void HandleIdentifierChanged(const UStateTree& StateTree);
		void HandleSchemaChanged(const UStateTree& StateTree);
		void HandleParametersChanged(const UStateTree& StateTree);
		void HandleStateParametersChanged(const UStateTree& StateTree, const FGuid StateID);
		void HandleGlobalDataChanged(const UStateTree& StateTree);
		void HandleVisualThemeChanged(const UStateTree& StateTree);
		void HandleBreakpointsChanged(const UStateTree& StateTree);
		void HandlePostCompile(const UStateTree& StateTree);
		void HandlePostInternalCompile(const UE::StateTree::Compiler::FPostInternalContext& Context);
		void HandlePreValidate(TNotNull<UStateTree*> StateTree);
		void HandleValidate(TNotNull<UStateTree*> StateTree);

		// View model hooks (what the editor UI is told).
		void HandleViewModelAssetChanged();
		void HandleViewModelStatesChanged(const TSet<UStateTreeState*>& AffectedStates, const FPropertyChangedEvent& Event);
		void HandleViewModelStateAdded(UStateTreeState* ParentState, UStateTreeState* NewState);
		void HandleViewModelStatesRemoved(const TSet<UStateTreeState*>& AffectedParents);
		void HandleViewModelStatesMoved(const TSet<UStateTreeState*>& AffectedParents, const TSet<UStateTreeState*>& MovedStates);
		void HandleViewModelStateNodesChanged(const UStateTreeState* AffectedState);
		void HandleViewModelSelectionChanged(const TArray<TWeakObjectPtr<UStateTreeState>>& SelectedStates);

		TArray<TWeakObjectPtr<const UStateTree>> TrackedTrees;
		TArray<TWeakObjectPtr<const UPackage>> TrackedPackages;
		TWeakPtr<FStateTreeViewModel> AttachedViewModel;
		TArray<FDstCaptureEvent> Events;
		TArray<FString> SegmentLabels;
		int32 CurrentSegment = INDEX_NONE;
		int32 NextSequence = 0;
		bool bStarted = false;
		bool bPaused = false;
	};

	/**
	 * A small real asset in a real package (never saved), and the handles needed to edit and read it:
	 * Root > [A > [A1], B]. A has two enter conditions, a Delay task and one transition; B has a Delay task.
	 * The root parameter "Speed" is bound to the Duration of A's task. Created through the same factory the
	 * Content Browser uses, so editor data, editor schema and schema are what the editor would produce.
	 */
	class FDstCaptureFixture
	{
	public:
		FDstCaptureFixture();
		~FDstCaptureFixture();

		/** Creates the asset and the baseline content. On failure OutError says what was missing. */
		bool Initialize(const TCHAR* BaseName, FString& OutError);

		/** Unloads everything the fixture made. Safe to call twice; also run by the destructor. */
		void Destroy();

		UStateTree* GetTree() const { return StateTree.Get(); }
		UStateTreeEditorData* GetEditorData() const;
		UPackage* GetPackage() const;

		UStateTreeState* FindState(const FGuid& StateId) const;
		FStateTreeEditorNode* FindNode(const FGuid& StateId, const FGuid& NodeId) const;
		FStateTreeTransition* FindTransition(const FGuid& StateId, const FGuid& TransitionId) const;

		/** The view model the editor would use for this asset (found or created through the editing subsystem). */
		TSharedPtr<FStateTreeViewModel> GetOrCreateViewModel();
		void ResetViewModel();

		/** Text dump of the editor data. */
		FString Dump(bool bIncludeIds = true) const;
		/** Hash of the editor data as the engine computes it to decide whether a recompile is needed. */
		uint32 EditorDataHash() const;
		/** Compiles through the editing subsystem; OutMessages gets the compiler log. */
		bool Compile(FString& OutMessages) const;

		/** Keeps another asset (a copy made by a test) alive and unloads it with the fixture. */
		void AdoptExtraTree(UStateTree* ExtraTree, bool bRegisteredInAssetRegistry);

		struct FBaselineIds
		{
			FGuid Root;
			FGuid A;
			FGuid A1;
			FGuid B;
			FGuid ATask;
			FGuid BTask;
			FGuid ACondFirst;
			FGuid ACondSecond;
			FGuid ATransition;
		};
		FBaselineIds Ids;

		/** Name of the float root parameter used by the baseline binding. */
		static const FName SpeedParameterName;

	private:
		bool BuildBaseline(FString& OutError);

		TStrongObjectPtr<UStateTree> StateTree;
		TSharedPtr<FStateTreeViewModel> ViewModel;

		struct FExtraTree
		{
			TStrongObjectPtr<UStateTree> Tree;
			bool bRegisteredInAssetRegistry = false;
		};
		TArray<FExtraTree> ExtraTrees;
	};

	/** What a Details-panel style edit touches. */
	struct FDstPropertyEdit
	{
		/** Object whose PreEditChange / PostEditChangeChainProperty are called. */
		UObject* Owner = nullptr;
		/** Property chain, head is the member property of Owner, tail is the property that changed. */
		TArray<FProperty*> Chain;
		/** Array index per array property of the chain, keyed by property name (what FPropertyChangedEvent::GetArrayIndex reads). */
		TMap<FString, int32> ArrayIndices;
		EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet;
		/** When not empty the edit opens its own transaction with this title. */
		FText TransactionText;
	};

	/** False when the module events of FStateTreeEditorModule could not be compiled in (see the .cpp). */
	bool DstModuleHooksCompiledIn();

	/** Looks a property up by name; nullptr when the engine no longer has it. */
	FProperty* DstFindProperty(const UStruct* Struct, const TCHAR* Name);

	/** The reflection data of a StateTree node struct such as "StateTreeDelayTask" in module "StateTreeModule". */
	UScriptStruct* DstFindNodeStruct(const TCHAR* ModuleName, const TCHAR* StructName);

	/** The editor node struct (FStateTreeEditorNode), found through reflection because its StaticStruct is not exported. */
	UScriptStruct* DstGetEditorNodeStruct();

	/** Calls Modify, PreEditChange(chain), Mutate, PostEditChangeChainProperty the way the property editor does. */
	void DstApplyPropertyEdit(const FDstPropertyEdit& Edit, TFunctionRef<void()> Mutate);

	/** Sets a float property of an instance-data struct by name. */
	bool DstSetFloat(const UStruct* Struct, void* Memory, const TCHAR* PropertyName, float Value);
	bool DstSetBool(const UStruct* Struct, void* Memory, const TCHAR* PropertyName, bool bValue);

	/** Mutable access to the root parameter bag, which is a private property of the editor data. */
	FInstancedPropertyBag* DstGetRootParameterBag(UStateTreeEditorData& EditorData);

	/** Saves the system clipboard on construction and restores it on destruction, so the experiments do not eat the user's clipboard. */
	class FDstClipboardGuard
	{
	public:
		FDstClipboardGuard();
		~FDstClipboardGuard();

	private:
		FString Saved;
	};

	/**
	 * Silences LogStateTreeEditor and LogStateTree while alive. The compiler logs every compile error as an Error line, and the
	 * automation framework fails a test for any logged error; the experiments compile trees in half-edited states on
	 * purpose and read the compiler log from the compile result instead.
	 */
	class FDstLogSilencer
	{
	public:
		FDstLogSilencer();
		~FDstLogSilencer();

	private:
		ELogVerbosity::Type Previous;
		ELogVerbosity::Type PreviousRuntime;
	};

	/** Shortens a GUID for reports. */
	FString DstShortGuid(const FGuid& Id);
}
