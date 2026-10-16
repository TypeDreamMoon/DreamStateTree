// Copyright DreamMoon. All Rights Reserved.

#include "DreamStateTreeEditorSubsystem.h"

#include "Algo/Count.h"
#include "DirectoryWatcherModule.h"
#include "DreamStateTreeEditorSettings.h"
#include "DstTextMerge.h"
#include "DstWriteBack.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IDirectoryWatcher.h"
#include "Lang/DstSource.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/TransactionObjectEvent.h"
#include "Modules/ModuleManager.h"
#include "StateTree.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "DreamStateTreeEditor"

using namespace UE::DreamStateTree;
using namespace UE::DreamStateTree::Compiler;

namespace DreamStateTreeEditorPrivate
{
	const FName LogName(TEXT("DreamStateTree"));
	/** A save often arrives as several file events; they are handled together once the file is quiet. */
	constexpr double SettleSeconds = 0.3;

	FString NormalizeSourcePath(const FString& Path)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Full);
		return Full;
	}

	bool LoadSourceText(const FString& Path, FString& OutText)
	{
		return FPaths::FileExists(Path) && FFileHelper::LoadFileToString(OutText, *Path);
	}

	bool HasUtf8ByteOrderMark(const FString& Path)
	{
		const TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader || Reader->TotalSize() < 3)
		{
			return false;
		}
		uint8 Head[3] = {};
		Reader->Serialize(Head, 3);
		return Head[0] == 0xEF && Head[1] == 0xBB && Head[2] == 0xBF;
	}

	FText FileText(const FString& Path)
	{
		return FText::FromString(FPaths::GetCleanFilename(Path));
	}

	void OpenLog()
	{
		FMessageLog(LogName).Open(EMessageSeverity::Info, true);
	}

	/** Whether a path is inside one of the folders sources are looked for in. */
	bool IsUnderAnyRoot(const FString& Path, const TArray<FString>& Roots)
	{
		for (const FString& Root : Roots)
		{
			FString Prefix = NormalizeSourcePath(Root);
			if (!Prefix.EndsWith(TEXT("/")))
			{
				Prefix += TEXT("/");
			}
			if (Path.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}

using namespace DreamStateTreeEditorPrivate;

// ---------------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------------

void UDreamStateTreeEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Index.Scan(GetActiveRoots());
	WatchRoots();
	TransactedHandle = FCoreUObjectDelegates::OnObjectTransacted.AddUObject(this, &UDreamStateTreeEditorSubsystem::OnObjectTransacted);
	PreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddUObject(this, &UDreamStateTreeEditorSubsystem::OnObjectPreSave);
}

void UDreamStateTreeEditorSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::OnObjectTransacted.Remove(TransactedHandle);
	FCoreUObjectDelegates::OnObjectPreSave.Remove(PreSaveHandle);
	UnwatchRoots();
	Super::Deinitialize();
}

TStatId UDreamStateTreeEditorSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UDreamStateTreeEditorSubsystem, STATGROUP_Tickables);
}

ETickableTickType UDreamStateTreeEditorSubsystem::GetTickableTickType() const
{
	return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Always;
}

void UDreamStateTreeEditorSubsystem::Tick(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();

	TArray<FString> Settled;
	for (auto It = ChangedSources.CreateIterator(); It; ++It)
	{
		if (Now - It.Value() >= SettleSeconds)
		{
			Settled.Add(It.Key());
			It.RemoveCurrent();
		}
	}
	if (!Settled.IsEmpty())
	{
		HandleChangedSources(Settled);
	}

	// Edits are written once they pause, never in the middle of a transaction or a drag.
	const UDreamStateTreeEditorSettings* Settings = GetDefault<UDreamStateTreeEditorSettings>();
	if (EditedTrees.IsEmpty() || !Settings->bWriteBackEditorChanges || (GEditor && GEditor->IsTransactionActive())
		|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetPressedMouseButtons().Num() > 0))
	{
		return;
	}
	TArray<UStateTree*> Due;
	for (auto It = EditedTrees.CreateIterator(); It; ++It)
	{
		UStateTree* Tree = It.Key().Get();
		if (!Tree)
		{
			It.RemoveCurrent();
		}
		else if (Now - It.Value() >= Settings->WriteBackDelay && !IsInConflict(*Tree))
		{
			Due.Add(Tree);
			It.RemoveCurrent();
		}
	}
	for (UStateTree* Tree : Due)
	{
		WriteBack(*Tree);
	}
}

// ---------------------------------------------------------------------------------------------
// Sources on disk
// ---------------------------------------------------------------------------------------------

FString UDreamStateTreeEditorSubsystem::FindSource(const UObject& Asset) const
{
	const FDstSourceEntry* Entry = Index.FindByAsset(Asset.GetPathName());
	return Entry ? Entry->FilePath : FString();
}

UStateTree* UDreamStateTreeEditorSubsystem::FindLoadedTree(const FString& FilePath) const
{
	const FDstSourceEntry* Entry = Index.FindByFile(FilePath);
	return Entry && !Entry->AssetPath.IsEmpty() ? FindObject<UStateTree>(nullptr, *Entry->AssetPath) : nullptr;
}

TArray<FString> UDreamStateTreeEditorSubsystem::GetActiveRoots() const
{
	return RootsOverride.IsEmpty() ? GetDstSourceRoots() : RootsOverride;
}

void UDreamStateTreeEditorSubsystem::SetSourceRoots(const TArray<FString>& Roots)
{
	RootsOverride.Reset();
	for (const FString& Root : Roots)
	{
		RootsOverride.Add(NormalizeSourcePath(Root));
	}
	ChangedSources.Reset();
	RefreshSources();
}

void UDreamStateTreeEditorSubsystem::RefreshSources()
{
	Index.Scan(GetActiveRoots());
	WatchRoots();
}

void UDreamStateTreeEditorSubsystem::WatchRoots()
{
	IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
	if (!Watcher)
	{
		return;
	}
	// Folders that stopped being source roots (the roots were replaced) are let go.
	for (auto It = Watches.CreateIterator(); It; ++It)
	{
		if (!Index.GetRoots().Contains(It.Key()))
		{
			Watcher->UnregisterDirectoryChangedCallback_Handle(It.Key(), It.Value());
			It.RemoveCurrent();
		}
	}
	for (const FString& Root : Index.GetRoots())
	{
		if (Watches.Contains(Root) || !FPaths::DirectoryExists(Root))
		{
			continue;
		}
		FDelegateHandle Handle;
		if (Watcher->RegisterDirectoryChangedCallback_Handle(Root, IDirectoryWatcher::FDirectoryChanged::CreateUObject(this, &UDreamStateTreeEditorSubsystem::OnDirectoryChanged), Handle))
		{
			Watches.Add(Root, Handle);
		}
	}
}

void UDreamStateTreeEditorSubsystem::UnwatchRoots()
{
	FDirectoryWatcherModule* Module = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = Module ? Module->Get() : nullptr;
	for (const TPair<FString, FDelegateHandle>& Watch : Watches)
	{
		if (Watcher)
		{
			Watcher->UnregisterDirectoryChangedCallback_Handle(Watch.Key, Watch.Value);
		}
	}
	Watches.Reset();
}

void UDreamStateTreeEditorSubsystem::OnDirectoryChanged(const TArray<FFileChangeData>& Changes)
{
	const double Now = FPlatformTime::Seconds();
	for (const FFileChangeData& Change : Changes)
	{
		if (IsDstSourceFile(Change.Filename))
		{
			ChangedSources.Add(NormalizeSourcePath(Change.Filename), Now);
		}
	}
}

void UDreamStateTreeEditorSubsystem::HandleChangedSources(const TArray<FString>& FilePaths)
{
	// Each document once; one reached only through a library may still have its own text.
	TMap<FString, bool> Documents;
	for (const FString& RawPath : FilePaths)
	{
		const FString Path = NormalizeSourcePath(RawPath);
		FString Text;
		const bool bExists = LoadSourceText(Path, Text);
		Index.Rescan(Path);
		if (const FString* Written = WrittenTexts.Find(Path))
		{
			const bool bOurs = bExists && Text.Equals(*Written, ESearchCase::CaseSensitive);
			WrittenTexts.Remove(Path);
			if (bOurs)
			{
				continue;
			}
		}
		if (!bExists || !IsUnderAnyRoot(Path, Index.GetRoots()))
		{
			continue;
		}
		if (Lang::GetDstFileKindFromPath(Path) == Lang::EDstFileKind::Library)
		{
			for (const FString& Dependent : Index.FindDependents(Path))
			{
				Documents.FindOrAdd(NormalizeSourcePath(Dependent), true);
			}
		}
		else
		{
			Documents.Add(Path, false);
		}
	}
	if (Documents.IsEmpty() || !GetDefault<UDreamStateTreeEditorSettings>()->bRebuildWhenTextChanges)
	{
		return;
	}

	// Deciding first and building together keeps the build in dependency order.
	TArray<FString> ToBuild;
	for (const TPair<FString, bool>& Document : Documents)
	{
		if (AllowSourceRebuild(Document.Key, Document.Value))
		{
			ToBuild.Add(Document.Key);
		}
	}
	if (!ToBuild.IsEmpty())
	{
		RebuildFiles(ToBuild, true);
	}
}

bool UDreamStateTreeEditorSubsystem::AllowSourceRebuild(const FString& FilePath, bool bViaLibrary)
{
	FString Text;
	if (!LoadSourceText(FilePath, Text))
	{
		return false;
	}
	UStateTree* Tree = FindLoadedTree(FilePath);
	if (!Tree)
	{
		return true;
	}
	const bool bEdited = EditedTrees.Contains(Tree) || IsInConflict(*Tree) || !LastRefusals.FindRef(Tree).IsEmpty();
	FString Base;
	FString BaseSourcePath;
	GetDstGeneratedText(*Tree, Base, BaseSourcePath);
	if (!bViaLibrary && !bEdited && Base.Equals(Text, ESearchCase::CaseSensitive))
	{
		// Already in step: an echo of a save, or a change undone.
		return false;
	}
	// Compared with the text it was made from: a library that changed would make every dependent differ.
	TArray<FString> EditorChanges;
	if ((!bViaLibrary || bEdited) && HasEditorChanges(*Tree, &EditorChanges))
	{
		ShowConflict(*Tree, FilePath, EditorChanges);
		return false;
	}
	return true;
}

bool UDreamStateTreeEditorSubsystem::SaveSource(const FString& FilePath, const FString& Text)
{
	const FFileHelper::EEncodingOptions Encoding = HasUtf8ByteOrderMark(FilePath) ? FFileHelper::EEncodingOptions::ForceUTF8 : FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM;
	if (!FFileHelper::SaveStringToFile(Text, *FilePath, Encoding))
	{
		FMessageLog(LogName).Error(FText::Format(LOCTEXT("CannotWrite", "Cannot write {0}; is it read-only or open elsewhere?"), FText::FromString(FilePath)));
		Notify(FText::Format(LOCTEXT("CannotWriteShort", "Cannot write {0}."), FileText(FilePath)), false, true);
		return false;
	}
	WrittenTexts.Add(FilePath, Text);
	Index.Rescan(FilePath);
	return true;
}

FString UDreamStateTreeEditorSubsystem::MakeMetadataSourcePath(const FString& FilePath) const
{
	for (const FString& Root : Index.GetRoots())
	{
		FString Prefix = NormalizeSourcePath(Root);
		if (!Prefix.EndsWith(TEXT("/")))
		{
			Prefix += TEXT("/");
		}
		if (FilePath.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return FilePath.RightChop(Prefix.Len());
		}
	}
	return FPaths::GetCleanFilename(FilePath);
}

// ---------------------------------------------------------------------------------------------
// Text to StateTree
// ---------------------------------------------------------------------------------------------

UStateTree* UDreamStateTreeEditorSubsystem::Rebuild(const FString& FilePath)
{
	const FString Path = NormalizeSourcePath(FilePath);
	TMap<FString, UStateTree*> Built;
	RebuildFiles({ Path }, true, &Built);
	UStateTree* const* Found = Built.Find(Path);
	return Found ? *Found : nullptr;
}

int32 UDreamStateTreeEditorSubsystem::RebuildFiles(const TArray<FString>& FilePaths, bool bNotify, TMap<FString, UStateTree*>* OutBuilt)
{
	const UDreamStateTreeEditorSettings* Settings = GetDefault<UDreamStateTreeEditorSettings>();

	// A library stands for the trees that include it.
	TArray<FString> Files;
	for (const FString& RawPath : FilePaths)
	{
		const FString Path = NormalizeSourcePath(RawPath);
		if (Lang::GetDstFileKindFromPath(Path) == Lang::EDstFileKind::Library)
		{
			for (const FString& Dependent : Index.FindDependents(Path))
			{
				Files.AddUnique(NormalizeSourcePath(Dependent));
			}
		}
		else
		{
			Files.AddUnique(Path);
		}
	}
	if (Files.IsEmpty())
	{
		return 0;
	}

	FDstBuildOptions Options;
	Options.bGenerate = true;
	Options.bSave = Settings->bSaveRebuiltAssets;
	Options.Roots = Index.GetRoots();

	++RebuildDepth;
	const TArray<FDstFileResult> Results = BuildDstFiles(Files, Options);
	--RebuildDepth;

	int32 NumBuilt = 0;
	UStateTree* FirstBuilt = nullptr;
	for (const FDstFileResult& Result : Results)
	{
		const FString ResultPath = NormalizeSourcePath(Result.FilePath);
		UStateTree* Tree = Result.Generated.IsSet() ? Result.Generated->Asset : nullptr;
		if (Tree && Result.Succeeded())
		{
			++NumBuilt;
			++NumRebuilds;
			FirstBuilt = FirstBuilt ? FirstBuilt : Tree;
			// What was edited in the editor is gone with the rebuild.
			EditedTrees.Remove(Tree);
			LastRefusals.Remove(Tree);
			CloseConflict(*Tree);
			if (OutBuilt)
			{
				OutBuilt->Add(ResultPath, Tree);
			}
		}
		Report(FString::Printf(TEXT("Rebuilding from %s"), *FPaths::GetCleanFilename(ResultPath)), Result.Diagnostics);
	}

	if (bNotify)
	{
		if (Results.Num() <= 1)
		{
			if (NumBuilt == 1 && FirstBuilt)
			{
				Notify(FText::Format(LOCTEXT("Rebuilt", "Rebuilt {0} from {1}"), FText::FromString(FirstBuilt->GetName()), FileText(Files[0])), true, false);
			}
			else
			{
				Notify(FText::Format(LOCTEXT("RebuildFailed", "{0} has errors; its asset was not rebuilt."), FileText(Files[0])), false, true);
			}
		}
		else
		{
			const bool bAll = NumBuilt == Results.Num();
			Notify(bAll ? FText::Format(LOCTEXT("RebuiltAll", "Rebuilt {0} StateTree(s) from their sources"), NumBuilt)
				: FText::Format(LOCTEXT("RebuiltSome", "Rebuilt {0} of {1} StateTrees; the rest have errors."), NumBuilt, Results.Num()), bAll, !bAll);
		}
	}
	return NumBuilt;
}

void UDreamStateTreeEditorSubsystem::RebuildAll()
{
	TArray<FString> Files;
	for (const FDstSourceEntry& Entry : Index.GetEntries())
	{
		if (Entry.Kind == Lang::EDstFileKind::StateTree)
		{
			Files.Add(Entry.FilePath);
		}
	}
	RebuildFiles(Files, true);
}

// ---------------------------------------------------------------------------------------------
// StateTree to text
// ---------------------------------------------------------------------------------------------

UStateTree* UDreamStateTreeEditorSubsystem::FindOwningTree(UObject* Object)
{
	UStateTree* Tree = Cast<UStateTree>(Object);
	if (!Tree && Object)
	{
		// Editor data, states, the schema and node instances all sit below the asset.
		Tree = Object->GetTypedOuter<UStateTree>();
	}
	if (!Tree || Tree->IsTemplate() || Tree->GetPackage() == GetTransientPackage())
	{
		return nullptr;
	}
	FString Text;
	FString SourcePath;
	return GetDstGeneratedText(*Tree, Text, SourcePath) ? Tree : nullptr;
}

void UDreamStateTreeEditorSubsystem::OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event)
{
	// A snapshot is a drag in progress; the finished edit follows.
	if (RebuildDepth > 0 || !Object || Event.GetEventType() == ETransactionObjectEventType::Snapshot)
	{
		return;
	}
	UStateTree* Tree = FindOwningTree(Object);
	if (Tree && !FindSource(*Tree).IsEmpty())
	{
		EditedTrees.Add(Tree, FPlatformTime::Seconds());
	}
}

void UDreamStateTreeEditorSubsystem::OnObjectPreSave(UObject* Object, FObjectPreSaveContext Context)
{
	UStateTree* Tree = Cast<UStateTree>(Object);
	// Not the generator's own save, not a cook, not a command line build.
	if (!Tree || RebuildDepth > 0 || Context.IsProceduralSave() || Context.IsCooking() || IsRunningCommandlet()
		|| !GetDefault<UDreamStateTreeEditorSettings>()->bWriteBackEditorChanges || IsInConflict(*Tree) || FindOwningTree(Tree) != Tree
		|| FindSource(*Tree).IsEmpty())
	{
		return;
	}
	// Whatever changed since the last write, transacted or not, goes into the text with the asset.
	WriteBack(*Tree);
}

bool UDreamStateTreeEditorSubsystem::HasEditorChanges(UStateTree& Tree, TArray<FString>* OutChanges) const
{
	const FString Path = FindSource(Tree);
	FString Base;
	FString BaseSourcePath;
	GetDstGeneratedText(Tree, Base, BaseSourcePath);
	if (Path.IsEmpty() || Base.IsEmpty())
	{
		// Not made from text: nothing to compare with.
		return false;
	}
	const FDstWriteBackResult Result = WriteBackDstAsset(Tree, Path, Base);
	if (Result.IsComplete() && Result.Changes.IsEmpty())
	{
		return false;
	}
	if (OutChanges)
	{
		for (const FDstWriteBackChange& Change : Result.Changes)
		{
			OutChanges->Add(FString::Printf(TEXT("%s: %s"), *Change.Location, *Change.What));
		}
		// What was refused says why: a change inside a template or a library, a structural change while those are off, ...
		for (const Lang::FDstDiagnostic& Diagnostic : Result.Diagnostics.GetDiagnostics())
		{
			OutChanges->Add(FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message));
		}
		if (OutChanges->IsEmpty())
		{
			OutChanges->Add(Result.bCompared ? FString(TEXT("differences write-back cannot place"))
				: FString(TEXT("the text the asset was made from no longer checks, so it cannot be compared")));
		}
	}
	return true;
}

bool UDreamStateTreeEditorSubsystem::HasPendingWriteBack(const UStateTree& Tree) const
{
	return EditedTrees.Contains(TWeakObjectPtr<UStateTree>(const_cast<UStateTree*>(&Tree)));
}

bool UDreamStateTreeEditorSubsystem::WriteBack(UStateTree& Tree)
{
	const FString Path = FindSource(Tree);
	FString Text;
	if (Path.IsEmpty() || !LoadSourceText(Path, Text))
	{
		return false;
	}
	EditedTrees.Remove(&Tree);

	// The file changed since the two were last in step: writing now would undo that change.
	FString Base;
	FString BaseSourcePath;
	GetDstGeneratedText(Tree, Base, BaseSourcePath);
	if (!Base.IsEmpty() && !Base.Equals(Text, ESearchCase::CaseSensitive))
	{
		TArray<FString> EditorChanges;
		if (HasEditorChanges(Tree, &EditorChanges))
		{
			ShowConflict(Tree, Path, EditorChanges);
		}
		return false;
	}

	FDstWriteBackOptions Options;
	Options.bStructure = GetDefault<UDreamStateTreeEditorSettings>()->bWriteBackStructure;
	const FDstWriteBackResult Result = WriteBackDstAsset(Tree, Path, Text, Options);

	const bool bWrote = !Result.Text.Equals(Text, ESearchCase::CaseSensitive);
	if (bWrote)
	{
		if (!SaveSource(Path, Result.Text))
		{
			return false;
		}
		SetDstGeneratedText(Tree, Result.Text, BaseSourcePath.IsEmpty() ? MakeMetadataSourcePath(Path) : BaseSourcePath);
	}

	// Refusals are repeated only when they change: moving states about writes nothing and says nothing.
	TArray<FString> RefusalKeys;
	for (const Lang::FDstDiagnostic& Diagnostic : Result.Diagnostics.GetDiagnostics())
	{
		RefusalKeys.Add(FString::Printf(TEXT("%s@%d:%s"), *Diagnostic.Code, Diagnostic.Span.Line, *Diagnostic.Message));
	}
	const FString Refusals = FString::Join(RefusalKeys, TEXT("\n"));
	const bool bNewRefusals = !Refusals.IsEmpty() && LastRefusals.FindRef(&Tree) != Refusals;
	if (Refusals.IsEmpty())
	{
		LastRefusals.Remove(&Tree);
	}
	else
	{
		LastRefusals.Add(&Tree, Refusals);
	}
	if (bWrote || bNewRefusals)
	{
		Report(FString::Printf(TEXT("Writing %s back to %s"), *Tree.GetName(), *FPaths::GetCleanFilename(Path)), Result.Diagnostics);
	}

	if (bWrote)
	{
		const int32 NumWritten = Algo::CountIf(Result.Changes, [](const FDstWriteBackChange& Change) { return Change.bWritten; });
		Notify(FText::Format(LOCTEXT("WroteBack", "Wrote {0} change(s) in {1} to {2}"), NumWritten, FText::FromString(Tree.GetName()), FileText(Path)), true,
			Result.Diagnostics.Num() > 0);
	}
	if (bNewRefusals && FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::Format(LOCTEXT("NotWritten", "Some changes in {0} are not in {1}; the log says why."), FText::FromString(Tree.GetName()), FileText(Path)));
		Info.ExpireDuration = 8.0f;
		Info.bUseSuccessFailIcons = true;
		Info.Hyperlink = FSimpleDelegate::CreateStatic(&OpenLog);
		Info.HyperlinkText = LOCTEXT("ShowLog", "Show Log");
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}
	return bWrote;
}

// ---------------------------------------------------------------------------------------------
// Conflicts
// ---------------------------------------------------------------------------------------------

bool UDreamStateTreeEditorSubsystem::IsInConflict(const UStateTree& Tree) const
{
	return Conflicts.Contains(TWeakObjectPtr<UStateTree>(const_cast<UStateTree*>(&Tree)));
}

void UDreamStateTreeEditorSubsystem::ShowConflict(UStateTree& Tree, const FString& FilePath, const TArray<FString>& EditorChanges)
{
	EditedTrees.Remove(&Tree);
	CloseConflict(Tree);

	FMessageLog Log(LogName);
	Log.Warning(FText::Format(LOCTEXT("ConflictLog", "{0} changed on disk while {1} has changes that are not in it. Nothing was overwritten; pick the text or merge both."),
		FText::FromString(FilePath), FText::FromString(Tree.GetName())));
	for (const FString& Change : EditorChanges)
	{
		Log.Info(FText::FromString(TEXT("    ") + Change));
	}

	TSharedPtr<SNotificationItem> Item;
	if (FSlateApplication::IsInitialized())
	{
		const TWeakObjectPtr<UDreamStateTreeEditorSubsystem> WeakThis(this);
		const TWeakObjectPtr<UStateTree> WeakTree(&Tree);
		FNotificationInfo Info(FText::Format(LOCTEXT("Conflict", "{0} changed on disk, and {1} has changes that are not in it."),
			FileText(FilePath), FText::FromString(Tree.GetName())));
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.bUseSuccessFailIcons = false;
		Info.Hyperlink = FSimpleDelegate::CreateStatic(&OpenLog);
		Info.HyperlinkText = LOCTEXT("ShowChanges", "Show the Asset's Changes");
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("UseText", "Use Text"),
			LOCTEXT("UseTextTip", "Rebuild the asset from the file; its own changes are dropped."),
			FSimpleDelegate::CreateLambda([WeakThis, WeakTree]()
			{
				if (WeakThis.IsValid() && WeakTree.IsValid())
				{
					WeakThis->UseText(*WeakTree);
				}
			})));
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("Merge", "Merge Both"),
			LOCTEXT("MergeTip", "Write the asset's changes into the file next to the file's own, then rebuild the asset from it."),
			FSimpleDelegate::CreateLambda([WeakThis, WeakTree]()
			{
				if (WeakThis.IsValid() && WeakTree.IsValid())
				{
					WeakThis->MergeEditorChanges(*WeakTree);
				}
			})));
		Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item)
		{
			Item->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
	Conflicts.Add(&Tree, Item);
}

void UDreamStateTreeEditorSubsystem::CloseConflict(UStateTree& Tree)
{
	TWeakPtr<SNotificationItem> Item;
	if (Conflicts.RemoveAndCopyValue(&Tree, Item))
	{
		if (const TSharedPtr<SNotificationItem> Pinned = Item.Pin())
		{
			Pinned->SetCompletionState(SNotificationItem::CS_None);
			Pinned->ExpireAndFadeout();
		}
	}
}

UStateTree* UDreamStateTreeEditorSubsystem::UseText(UStateTree& Tree)
{
	const FString Path = FindSource(Tree);
	CloseConflict(Tree);
	return Path.IsEmpty() ? nullptr : Rebuild(Path);
}

bool UDreamStateTreeEditorSubsystem::MergeEditorChanges(UStateTree& Tree)
{
	const FString Path = FindSource(Tree);
	FString Base;
	FString BaseSourcePath;
	GetDstGeneratedText(Tree, Base, BaseSourcePath);
	FString Theirs;
	if (Path.IsEmpty() || Base.IsEmpty() || !LoadSourceText(Path, Theirs))
	{
		return false;
	}

	// The editor's changes written into the text both started from, then both sides' changes to it together.
	FDstWriteBackOptions Options;
	Options.bStructure = GetDefault<UDreamStateTreeEditorSettings>()->bWriteBackStructure;
	const FDstWriteBackResult Ours = WriteBackDstAsset(Tree, Path, Base, Options);
	if (!Ours.IsComplete())
	{
		Report(FString::Printf(TEXT("Cannot merge %s into %s"), *Tree.GetName(), *FPaths::GetCleanFilename(Path)), Ours.Diagnostics);
		Notify(LOCTEXT("MergeIncomplete", "Some changes in the asset cannot be written to text; nothing was merged. Resolve those changes before merging."), false, true);
		return false;
	}
	FString Merged;
	TArray<FDstMergeConflict> MergeConflicts;
	if (!MergeDstTexts(Base, Ours.Text, Theirs, Merged, MergeConflicts))
	{
		FMessageLog Log(LogName);
		Log.Warning(FText::Format(LOCTEXT("MergeFailedLog", "{0}: the file and {1} changed the same lines; nothing was merged."), FText::FromString(Path),
			FText::FromString(Tree.GetName())));
		for (const FDstMergeConflict& Conflict : MergeConflicts)
		{
			Log.Info(FText::FromString(FString::Printf(TEXT("    line %d: the file has `%s`, the asset `%s`"), Conflict.Line, *Conflict.Theirs, *Conflict.Ours)));
		}
		Notify(FText::Format(LOCTEXT("MergeFailed", "{0} and {1} changed the same lines; edit the text by hand or use it as it is."), FileText(Path),
			FText::FromString(Tree.GetName())), false, true);
		return false;
	}
	if (Ours.Diagnostics.Num() > 0)
	{
		Report(FString::Printf(TEXT("Merging %s into %s"), *Tree.GetName(), *FPaths::GetCleanFilename(Path)), Ours.Diagnostics);
	}
	if (!SaveSource(Path, Merged))
	{
		return false;
	}
	CloseConflict(Tree);
	return Rebuild(Path) != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Decompiling and opening sources
// ---------------------------------------------------------------------------------------------

FString UDreamStateTreeEditorSubsystem::Decompile(UStateTree& Tree, bool bOverwrite, bool bRebuild)
{
	const FString Path = NormalizeSourcePath(GetDefaultDstDecompilePath(Tree));
	FDstDecompileOptions Options;
	Options.FilePath = Path;
	FString Text;
	Lang::FDstDiagnosticSink Sink;
	const bool bSucceeded = DecompileDstAsset(Tree, Options, Text, Sink);
	Report(FString::Printf(TEXT("Writing %s as text"), *Tree.GetName()), Sink);
	if (!bSucceeded)
	{
		Notify(FText::Format(LOCTEXT("DecompileFailed", "{0} could not be written as text."), FText::FromString(Tree.GetName())), false, true);
		return FString();
	}
	FString Existing;
	if (LoadSourceText(Path, Existing) && !Existing.Equals(Text, ESearchCase::CaseSensitive) && !bOverwrite)
	{
		Notify(FText::Format(LOCTEXT("DecompileExists", "{0} exists already and was not replaced."), FileText(Path)), false, false);
		return FString();
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!SaveSource(Path, Text))
	{
		return FString();
	}
	RefreshSources();

	// Write-back follows the names the text gives: the asset is built from its text once, which links the two.
	if (bRebuild)
	{
		Rebuild(Path);
	}
	else
	{
		Notify(FText::Format(LOCTEXT("Decompiled", "Wrote {0} as {1}"), FText::FromString(Tree.GetName()), FileText(Path)), true, false);
	}
	return Path;
}

void UDreamStateTreeEditorSubsystem::OpenSource(const FString& FilePath, int32 Line, int32 Column) const
{
	const FString Full = NormalizeSourcePath(FilePath);
	const FString Command = GetDefault<UDreamStateTreeEditorSettings>()->OpenSourceCommand.TrimStartAndEnd();
	if (!Command.IsEmpty())
	{
		const FString Expanded = Command.Replace(TEXT("{file}"), *Full).Replace(TEXT("{line}"), *LexToString(FMath::Max(Line, 1)))
			.Replace(TEXT("{column}"), *LexToString(FMath::Max(Column, 1)));
#if PLATFORM_WINDOWS
		// Through the shell, so `code` (a .cmd on the PATH) is found; cmd strips the outer quotes.
		FProcHandle Process = FPlatformProcess::CreateProc(TEXT("cmd.exe"), *FString::Printf(TEXT("/c \"%s\""), *Expanded), true, true, true, nullptr, 0, nullptr, nullptr);
#else
		FProcHandle Process = FPlatformProcess::CreateProc(TEXT("/bin/sh"), *FString::Printf(TEXT("-c \"%s\""), *Expanded.Replace(TEXT("\""), TEXT("\\\""))), true, true, true, nullptr, 0, nullptr, nullptr);
#endif
		if (Process.IsValid())
		{
			FPlatformProcess::CloseProc(Process);
			return;
		}
	}
	FPlatformProcess::LaunchFileInDefaultExternalApplication(*Full);
}

// ---------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------

void UDreamStateTreeEditorSubsystem::Report(const FString& Title, const Lang::FDstDiagnosticSink& Diagnostics) const
{
	if (Diagnostics.Num() == 0)
	{
		return;
	}
	FMessageLog Log(LogName);
	Log.Info(FText::FromString(Title));
	const TWeakObjectPtr<const UDreamStateTreeEditorSubsystem> WeakThis(this);
	for (const Lang::FDstDiagnostic& Diagnostic : Diagnostics.GetDiagnostics())
	{
		const EMessageSeverity::Type Severity = Diagnostic.Severity == Lang::EDstSeverity::Error ? EMessageSeverity::Error
			: Diagnostic.Severity == Lang::EDstSeverity::Warning ? EMessageSeverity::Warning : EMessageSeverity::Info;
		const TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(Severity);
		const FString Where = FString::Printf(TEXT("%s(%d,%d)"), *FPaths::GetCleanFilename(Diagnostic.FilePath), Diagnostic.Span.Line, Diagnostic.Span.Column);
		if (FPaths::FileExists(Diagnostic.FilePath))
		{
			const FString File = Diagnostic.FilePath;
			const int32 Line = Diagnostic.Span.Line;
			const int32 Column = Diagnostic.Span.Column;
			Message->AddToken(FActionToken::Create(FText::FromString(Where), LOCTEXT("OpenAtLine", "Open the source at this line"),
				FOnActionTokenExecuted::CreateLambda([WeakThis, File, Line, Column]()
				{
					if (WeakThis.IsValid())
					{
						WeakThis->OpenSource(File, Line, Column);
					}
				})));
		}
		else
		{
			Message->AddToken(FTextToken::Create(FText::FromString(Where)));
		}
		Message->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message))));
		if (!Diagnostic.Hint.IsEmpty())
		{
			Message->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("(%s)"), *Diagnostic.Hint))));
		}
		Log.AddMessage(Message);
	}
}

TSharedPtr<SNotificationItem> UDreamStateTreeEditorSubsystem::Notify(const FText& Message, bool bSuccess, bool bShowLogLink) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}
	FNotificationInfo Info(Message);
	Info.ExpireDuration = bSuccess ? 3.0f : 6.0f;
	Info.bUseSuccessFailIcons = true;
	if (bShowLogLink)
	{
		Info.Hyperlink = FSimpleDelegate::CreateStatic(&OpenLog);
		Info.HyperlinkText = LOCTEXT("ShowLog", "Show Log");
	}
	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item)
	{
		Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
	return Item;
}

#undef LOCTEXT_NAMESPACE
