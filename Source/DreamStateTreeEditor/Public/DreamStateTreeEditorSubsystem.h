// Copyright DreamMoon. All Rights Reserved.
//
// Keeps StateTree assets and their DreamStateTree sources in step while the editor runs. A source saved on disk
// rebuilds its asset (a library rebuilds the trees that include it). A change made to a tree in the StateTree editor
// is written back into its source once edits pause. Each asset keeps the text it was last built from or written to
// (GetDstGeneratedText); when the text changed on disk while the asset has changes that are not in it, nothing is
// overwritten: the conflict is listed and the user picks the text, or a merge of both.

#pragma once

#include "CoreMinimal.h"
#include "DstDriver.h"
#include "EditorSubsystem.h"
#include "Lang/DstDiagnostic.h"
#include "TickableEditorObject.h"
#include "DreamStateTreeEditorSubsystem.generated.h"

class FObjectPreSaveContext;
class SNotificationItem;
class UStateTree;
struct FFileChangeData;

UCLASS()
class DREAMSTATETREEEDITOR_API UDreamStateTreeEditorSubsystem : public UEditorSubsystem, public FTickableEditorObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;

	const UE::DreamStateTree::Compiler::FDstSourceIndex& GetIndex() const { return Index; }

	/** The source an asset is made from; empty when there is none. */
	FString FindSource(const UObject& Asset) const;

	/**
	 * Replaces the folders sources are looked for in (tests use a scratch folder); empty goes back to the project's
	 * and the plugins' DTree folders. Scans and watches the new ones.
	 */
	void SetSourceRoots(const TArray<FString>& Roots);

	/** Scans the source folders again and watches any that appeared. */
	void RefreshSources();

	/**
	 * Sources that changed on disk, as the folder watcher reports them once they settle: each rebuilds (a library, the
	 * trees that include it), unless the change is one this subsystem wrote or the asset has changes of its own (then
	 * a conflict is listed).
	 */
	void HandleChangedSources(const TArray<FString>& FilePaths);

	/** Compiles a source into its asset; with errors the asset is left as it is. */
	UStateTree* Rebuild(const FString& FilePath);
	/** Every source, libraries' dependents included, in dependency order. */
	void RebuildAll();

	/**
	 * The asset's changes into its source now. Refused when the file no longer holds the text the asset was last in
	 * step with: that is a conflict.
	 */
	bool WriteBack(UStateTree& Tree);

	/** The asset has changes that are not in the text it was last in step with. */
	bool HasEditorChanges(UStateTree& Tree, TArray<FString>* OutChanges = nullptr) const;
	/** Edits were seen in the editor and are not written back yet. */
	bool HasPendingWriteBack(const UStateTree& Tree) const;
	/** The asset waits for the user to pick between its changes and a text changed on disk. */
	bool IsInConflict(const UStateTree& Tree) const;
	/** Resolves a conflict for the text: the asset is rebuilt from the file. */
	UStateTree* UseText(UStateTree& Tree);
	/** Resolves a conflict with both: the editor's changes are merged into the file, then the asset is rebuilt from it. */
	bool MergeEditorChanges(UStateTree& Tree);

	/**
	 * Writes an asset not made from text as a source at its default place (DTree/...). bRebuild then rebuilds the
	 * asset from that text, which links the two for write-back. Returns the file, or empty.
	 */
	FString Decompile(UStateTree& Tree, bool bOverwrite, bool bRebuild = true);

	/** Opens a source at a line, with the command in the settings. */
	void OpenSource(const FString& FilePath, int32 Line = 1, int32 Column = 1) const;

	/** How many sources were built into assets since the editor started: a write that was only echoed back adds none. */
	int32 GetRebuildCount() const { return NumRebuilds; }

private:
	void WatchRoots();
	void UnwatchRoots();
	void OnDirectoryChanged(const TArray<FFileChangeData>& Changes);
	void OnObjectTransacted(UObject* Object, const class FTransactionObjectEvent& Event);
	/** Saving a tree catches what changed outside a transaction (scripts, tools) before it reaches disk. */
	void OnObjectPreSave(UObject* Object, FObjectPreSaveContext Context);

	/** The tree an object belongs to (itself, its editor data, states, schema, node instances) when that tree was generated from text. */
	static UStateTree* FindOwningTree(UObject* Object);
	UStateTree* FindLoadedTree(const FString& FilePath) const;
	TArray<FString> GetActiveRoots() const;

	/** Whether a changed document is rebuilt now; a conflict is listed instead when the asset has changes of its own. bViaLibrary: a library it includes changed. */
	bool AllowSourceRebuild(const FString& FilePath, bool bViaLibrary);
	/**
	 * Builds sources into their assets in dependency order (a library stands for the trees that include it). Returns
	 * how many were built; OutBuilt receives their assets by file.
	 */
	int32 RebuildFiles(const TArray<FString>& FilePaths, bool bNotify, TMap<FString, UStateTree*>* OutBuilt = nullptr);
	void ShowConflict(UStateTree& Tree, const FString& FilePath, const TArray<FString>& EditorChanges);
	void CloseConflict(UStateTree& Tree);
	bool SaveSource(const FString& FilePath, const FString& Text);
	/** The path the package metadata keeps: relative to the source root the file is under. */
	FString MakeMetadataSourcePath(const FString& FilePath) const;

	void Report(const FString& Title, const UE::DreamStateTree::Lang::FDstDiagnosticSink& Diagnostics) const;
	TSharedPtr<SNotificationItem> Notify(const FText& Message, bool bSuccess, bool bShowLogLink) const;

	UE::DreamStateTree::Compiler::FDstSourceIndex Index;
	/** Roots set by SetSourceRoots; empty means the default ones. */
	TArray<FString> RootsOverride;
	TMap<FString, FDelegateHandle> Watches;
	/** Sources the watcher reported, by when it last did: handled once they settle. */
	TMap<FString, double> ChangedSources;
	/** Trees edited in the editor, by when they last were: written back once edits pause. */
	TMap<TWeakObjectPtr<UStateTree>, double> EditedTrees;
	/** What this subsystem wrote to each file: the watcher's report of it is not a change. */
	TMap<FString, FString> WrittenTexts;
	/** Trees waiting for the user's choice in a conflict, with its notification. */
	TMap<TWeakObjectPtr<UStateTree>, TWeakPtr<SNotificationItem>> Conflicts;
	/** The last refusals reported for each tree, so moving states about does not repeat them. */
	TMap<TWeakObjectPtr<UStateTree>, FString> LastRefusals;
	/** Rebuilding: the generator's own edits are not the user's. */
	int32 RebuildDepth = 0;
	int32 NumRebuilds = 0;
	FDelegateHandle TransactedHandle;
	FDelegateHandle PreSaveHandle;
};
