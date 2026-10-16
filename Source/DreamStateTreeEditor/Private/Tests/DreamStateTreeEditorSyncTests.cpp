// Copyright DreamMoon. All Rights Reserved.
//
// The editor keeping a StateTree and its source in step: rebuilding from a source in a scratch source folder, writing
// an edit made in a transaction back into just its line, ignoring the echo of its own write, a text changed on disk
// while the asset has a change of its own (merged when they touch different lines, the text's when they do not), a
// library change rebuilding the trees that include it, and writing an asset as text.
//
// The nodes are the test module's fixtures, named in the text (DstTestTask); the module is not a dependency, the
// fixtures are only looked up by name when a source is built.

#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamStateTreeEditorSettings.h"
#include "DreamStateTreeEditorSubsystem.h"
#include "DstWriteBack.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Logging/MessageLog.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DreamStateTreeEditorSyncTests
{
	using namespace UE::DreamStateTree::Compiler;

	/**
	 * One state with one task. Speed feeds the task's required input; TicksToComplete is the value the tests edit, on a
	 * line of its own so a write-back is seen to change nothing else.
	 */
	const TCHAR* const SourceText = TEXT(
		"StateTree(Name=\"DreamStateTreeTests/Sync/ST_Sync\", Schema=TestAgent)\n"
		"{\n"
		"    Parameters = { float Speed = 0; }\n"
		"\n"
		"    State Root {\n"
		"        State Work {\n"
		"            do DstTestTask(InputValue = Params.Speed, TicksToComplete = 3)\n"
		"        }\n"
		"    }\n"
		"}\n");

	FString Load(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	void Save(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString GeneratedText(const UStateTree& Tree)
	{
		FString Text;
		FString SourcePath;
		GetDstGeneratedText(Tree, Text, SourcePath);
		return Text;
	}

	FString ScratchRoot(const TCHAR* Name)
	{
		return FPaths::ProjectSavedDir() / TEXT("DreamStateTreeTests") / TEXT("Editor") / Name;
	}

	UStateTreeState* FindStateIn(UStateTreeState* State, FName Name)
	{
		if (!State)
		{
			return nullptr;
		}
		if (State->Name == Name)
		{
			return State;
		}
		for (UStateTreeState* Child : State->Children)
		{
			if (UStateTreeState* Found = FindStateIn(Child, Name))
			{
				return Found;
			}
		}
		return nullptr;
	}

	UStateTreeState* FindState(UStateTree& Tree, FName Name)
	{
		const UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(Tree.EditorData);
		if (EditorData)
		{
			for (UStateTreeState* Root : EditorData->SubTrees)
			{
				if (UStateTreeState* Found = FindStateIn(Root, Name))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}

	/** The Work state's task and its TicksToComplete, reached through reflection like the Details panel does. */
	struct FTicksRef
	{
		UStateTreeState* State = nullptr;
		const FIntProperty* Property = nullptr;
		void* Memory = nullptr;

		bool IsValid() const { return State && Property && Memory; }
	};

	FTicksRef FindTicks(UStateTree& Tree)
	{
		FTicksRef Ref;
		Ref.State = FindState(Tree, TEXT("Work"));
		if (Ref.State && !Ref.State->Tasks.IsEmpty())
		{
			auto Instance = Ref.State->Tasks[0].GetInstance();
			if (Instance.GetStruct())
			{
				Ref.Property = FindFProperty<FIntProperty>(Instance.GetStruct(), TEXT("TicksToComplete"));
				Ref.Memory = Instance.GetMutableMemory();
			}
		}
		return Ref;
	}

	int32 GetTicks(UStateTree& Tree)
	{
		const FTicksRef Ref = FindTicks(Tree);
		return Ref.IsValid() ? Ref.Property->GetPropertyValue_InContainer(Ref.Memory) : INDEX_NONE;
	}

	/** Sets the value as the Details panel would: in a transaction that records the state, or without one (a script, a tool). */
	bool SetTicks(FAutomationTestBase& Test, UStateTree& Tree, int32 Value, bool bTransacted)
	{
		const FTicksRef Ref = FindTicks(Tree);
		if (!Ref.IsValid())
		{
			Test.AddError(TEXT("the Work state's task and its TicksToComplete"));
			return false;
		}
		TOptional<FScopedTransaction> Transaction;
		if (bTransacted)
		{
			Transaction.Emplace(NSLOCTEXT("DreamStateTreeEditorTests", "EditTicks", "Edit Ticks To Complete"));
			Ref.State->Modify();
		}
		Ref.Property->SetPropertyValue_InContainer(Ref.Memory, Value);
		return true;
	}

	/**
	 * A scratch place for one test: a source folder that replaces the project's for the test, the assets the test makes,
	 * and the settings it changes. Everything is put back when it goes out of scope (or on Release).
	 */
	struct FScratch
	{
		UDreamStateTreeEditorSubsystem& Subsystem;
		FString Root;
		FString Dir;
		bool bRootExisted = false;
		bool bSavedRebuilt = false;
		bool bRebuildOnChange = true;
		bool bWriteBack = true;
		bool bStructure = true;
		float Delay = 1.0f;
		TArray<FString> AssetPaths;
		TUniquePtr<FMessageLogScopedOverride> LogOverride;
		bool bReleased = false;

		FScratch(UDreamStateTreeEditorSubsystem& InSubsystem, const FString& InRoot, const TCHAR* Group)
			: Subsystem(InSubsystem)
		{
			Root = FPaths::ConvertRelativePathToFull(InRoot);
			FPaths::NormalizeFilename(Root);
			Dir = Root / TEXT("DreamStateTreeTests") / Group;
			bRootExisted = FPaths::DirectoryExists(Root);

			UDreamStateTreeEditorSettings* Settings = GetMutableDefault<UDreamStateTreeEditorSettings>();
			bSavedRebuilt = Settings->bSaveRebuiltAssets;
			bRebuildOnChange = Settings->bRebuildWhenTextChanges;
			bWriteBack = Settings->bWriteBackEditorChanges;
			bStructure = Settings->bWriteBackStructure;
			Delay = Settings->WriteBackDelay;
			Settings->bSaveRebuiltAssets = false;
			Settings->bRebuildWhenTextChanges = true;
			Settings->bWriteBackEditorChanges = true;
			Settings->bWriteBackStructure = true;
			Settings->WriteBackDelay = 0.2f;

			// Refusals and conflicts are logged as warnings; the test framework would count them as the test's own.
			LogOverride = MakeUnique<FMessageLogScopedOverride>(TEXT("DreamStateTree"));
			LogOverride->SuppressLoggingToOutputLog(true);

			IFileManager::Get().MakeDirectory(*Dir, true);
		}

		~FScratch()
		{
			Release();
		}

		/** Object path of an asset the test makes (`/Game/DreamStateTreeTests/Sync/ST_Sync.ST_Sync`). */
		void AddAsset(const TCHAR* ObjectPath)
		{
			AssetPaths.Add(ObjectPath);
		}

		FString Write(const TCHAR* FileName, const FString& Text) const
		{
			const FString Path = Dir / FileName;
			Save(Path, Text);
			return Path;
		}

		/** Makes the scratch folder the only source folder. */
		void Activate()
		{
			Subsystem.SetSourceRoots({ Root });
		}

		void Release()
		{
			if (bReleased)
			{
				return;
			}
			bReleased = true;

			Subsystem.SetSourceRoots({});
			IFileManager::Get().DeleteDirectory(*(bRootExisted ? Root / TEXT("DreamStateTreeTests") : Root), false, true);

			// The undo buffer holds references to the objects about to go.
			if (GEditor)
			{
				GEditor->ResetTransaction(NSLOCTEXT("DreamStateTreeEditorTests", "Cleanup", "DreamStateTree editor tests: cleanup"));
			}
			bool bHadAssets = false;
			for (const FString& AssetPath : AssetPaths)
			{
				if (UStateTree* Tree = FindObject<UStateTree>(nullptr, *AssetPath))
				{
					bHadAssets = true;
					FAssetRegistryModule::AssetDeleted(Tree);
					if (UPackage* Package = Tree->GetOutermost())
					{
						Package->SetDirtyFlag(false);
					}
					// Without these flags nothing keeps the asset alive, so the next garbage collection drops it with its package.
					Tree->ClearFlags(RF_Public | RF_Standalone);
				}
			}
			AssetPaths.Reset();
			if (bHadAssets)
			{
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}

			UDreamStateTreeEditorSettings* Settings = GetMutableDefault<UDreamStateTreeEditorSettings>();
			Settings->bSaveRebuiltAssets = bSavedRebuilt;
			Settings->bRebuildWhenTextChanges = bRebuildOnChange;
			Settings->bWriteBackEditorChanges = bWriteBack;
			Settings->bWriteBackStructure = bStructure;
			Settings->WriteBackDelay = Delay;
			LogOverride.Reset();
		}
	};

	UDreamStateTreeEditorSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UDreamStateTreeEditorSubsystem>() : nullptr;
	}
}

// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamStateTreeEditorSyncTest, "DreamStateTree.Editor.Sync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamStateTreeEditorSyncTest::RunTest(const FString& Parameters)
{
	using namespace DreamStateTreeEditorSyncTests;

	UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem();
	if (!TestNotNull(TEXT("the subsystem"), Subsystem))
	{
		return false;
	}

	FScratch Scratch(*Subsystem, ScratchRoot(TEXT("Sync")), TEXT("Sync"));
	Scratch.AddAsset(TEXT("/Game/DreamStateTreeTests/Sync/ST_Sync.ST_Sync"));
	const FString Path = Scratch.Write(TEXT("ST_Sync.dtr"), SourceText);
	Scratch.Activate();

	// The source builds its asset, and the asset keeps the text it came from.
	UStateTree* Tree = Subsystem->Rebuild(Path);
	if (!TestNotNull(TEXT("the asset is built from the source"), Tree))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the asset keeps the text it was built from"), GeneratedText(*Tree), FString(SourceText));
	TestTrue(TEXT("the asset finds its source"), Subsystem->FindSource(*Tree).Equals(Path, ESearchCase::IgnoreCase));
	TestEqual(TEXT("the text's value is in the asset"), GetTicks(*Tree), 3);
	TestFalse(TEXT("fresh from the text, it has no changes of its own"), Subsystem->HasEditorChanges(*Tree));
	const int32 Built = Subsystem->GetRebuildCount();

	// An edit made in a transaction is noticed and written back: the file changes on that one line.
	SetTicks(*this, *Tree, 5, true);
	TestTrue(TEXT("the transaction is noticed"), Subsystem->HasPendingWriteBack(*Tree));
	TestTrue(TEXT("the edit is a change the text does not have"), Subsystem->HasEditorChanges(*Tree));
	TestTrue(TEXT("the edit is written"), Subsystem->WriteBack(*Tree));
	const FString Written = FString(SourceText).Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 5"));
	TestEqualSensitive(TEXT("the file has the edit and nothing else changed"), Load(Path), Written);
	TestEqualSensitive(TEXT("the asset keeps the written text"), GeneratedText(*Tree), Written);
	TestFalse(TEXT("nothing waits to be written"), Subsystem->HasPendingWriteBack(*Tree));
	TestFalse(TEXT("the asset is in step with the new text"), Subsystem->HasEditorChanges(*Tree));

	// The watcher's report of that write is not a change.
	Subsystem->HandleChangedSources({ Path });
	TestFalse(TEXT("its own write is no conflict"), Subsystem->IsInConflict(*Tree));
	TestEqual(TEXT("its own write rebuilds nothing"), Subsystem->GetRebuildCount(), Built);
	TestEqual(TEXT("and the asset keeps its value"), GetTicks(*Tree), 5);

	// The text and the asset both change, on different lines: a conflict, then a merge of both.
	SetTicks(*this, *Tree, 7, true);
	const FString Outside = Written.Replace(TEXT("float Speed = 0;"), TEXT("float Speed = 2;"));
	Save(Path, Outside);
	Subsystem->HandleChangedSources({ Path });
	TestTrue(TEXT("a text changed under the asset's own change is a conflict"), Subsystem->IsInConflict(*Tree));
	TestEqualSensitive(TEXT("nothing is overwritten while it waits"), Load(Path), Outside);
	TestEqual(TEXT("the asset keeps its change while it waits"), GetTicks(*Tree), 7);
	TestEqual(TEXT("and was not rebuilt"), Subsystem->GetRebuildCount(), Built);
	TestFalse(TEXT("no write-back while it waits"), Subsystem->WriteBack(*Tree));
	TestEqualSensitive(TEXT("the file is still the text's"), Load(Path), Outside);
	TestTrue(TEXT("different lines merge"), Subsystem->MergeEditorChanges(*Tree));
	const FString Merged = Outside.Replace(TEXT("TicksToComplete = 5"), TEXT("TicksToComplete = 7"));
	TestEqualSensitive(TEXT("the file has both changes"), Load(Path), Merged);
	TestFalse(TEXT("the conflict is over"), Subsystem->IsInConflict(*Tree));
	TestEqualSensitive(TEXT("the asset was rebuilt from the merge"), GeneratedText(*Tree), Merged);
	TestEqual(TEXT("with the edit in it"), GetTicks(*Tree), 7);
	TestFalse(TEXT("the merge is what the asset holds"), Subsystem->HasEditorChanges(*Tree));

	// Both change the same line: no merge, nothing written; the text wins when asked.
	SetTicks(*this, *Tree, 9, true);
	const FString SameLine = Merged.Replace(TEXT("TicksToComplete = 7"), TEXT("TicksToComplete = 8"));
	Save(Path, SameLine);
	Subsystem->HandleChangedSources({ Path });
	TestTrue(TEXT("the same line is a conflict"), Subsystem->IsInConflict(*Tree));
	TestFalse(TEXT("the same line does not merge"), Subsystem->MergeEditorChanges(*Tree));
	TestEqualSensitive(TEXT("a failed merge writes nothing"), Load(Path), SameLine);
	TestTrue(TEXT("a failed merge keeps the conflict"), Subsystem->IsInConflict(*Tree));
	TestTrue(TEXT("the text is used when asked"), Subsystem->UseText(*Tree) != nullptr);
	TestFalse(TEXT("then the conflict is over"), Subsystem->IsInConflict(*Tree));
	TestEqualSensitive(TEXT("and the asset is the text's"), GeneratedText(*Tree), SameLine);
	TestEqual(TEXT("with its value"), GetTicks(*Tree), 8);
	TestFalse(TEXT("and none of its own change left"), Subsystem->HasEditorChanges(*Tree));

	// A text changed on disk with no change in the asset just rebuilds it.
	const FString Plain = SameLine.Replace(TEXT("float Speed = 2;"), TEXT("float Speed = 4;"));
	Save(Path, Plain);
	Subsystem->HandleChangedSources({ Path });
	TestFalse(TEXT("no conflict without a change of its own"), Subsystem->IsInConflict(*Tree));
	TestEqualSensitive(TEXT("rebuilt from the new text"), GeneratedText(*Tree), Plain);

	// With rebuilding off, the change waits.
	UDreamStateTreeEditorSettings* Settings = GetMutableDefault<UDreamStateTreeEditorSettings>();
	Settings->bRebuildWhenTextChanges = false;
	const FString Later = Plain.Replace(TEXT("float Speed = 4;"), TEXT("float Speed = 5;"));
	Save(Path, Later);
	Subsystem->HandleChangedSources({ Path });
	TestEqualSensitive(TEXT("a change is not built while rebuilding is off"), GeneratedText(*Tree), Plain);
	Settings->bRebuildWhenTextChanges = true;
	Subsystem->HandleChangedSources({ Path });
	TestEqualSensitive(TEXT("and is built once it is on"), GeneratedText(*Tree), Later);

	// A text with errors leaves the asset as it is.
	const FString Broken = Later.Replace(TEXT("TicksToComplete = 8)"), TEXT("TicksToComplete = 8"));
	const int32 BeforeBroken = Subsystem->GetRebuildCount();
	Save(Path, Broken);
	Subsystem->HandleChangedSources({ Path });
	TestEqual(TEXT("a text with errors builds nothing"), Subsystem->GetRebuildCount(), BeforeBroken);
	TestEqualSensitive(TEXT("the asset keeps the text it was built from"), GeneratedText(*Tree), Later);
	TestEqual(TEXT("and its value"), GetTicks(*Tree), 8);
	TestFalse(TEXT("no conflict without a change of its own"), Subsystem->IsInConflict(*Tree));
	Save(Path, Later);

	// A change made without a transaction (a script, a tool) is written when the asset is saved.
	SetTicks(*this, *Tree, 11, false);
	TestFalse(TEXT("nothing saw that change"), Subsystem->HasPendingWriteBack(*Tree));
	const FString SavedFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamStateTreeTests") / TEXT("Editor") / TEXT("Sync") / TEXT("ST_Sync.uasset"));
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	TestTrue(TEXT("the asset saves"), UPackage::SavePackage(Tree->GetPackage(), Tree, *SavedFile, SaveArgs));
	IFileManager::Get().Delete(*SavedFile, false, true, true);
	const FString SavedText = Later.Replace(TEXT("TicksToComplete = 8"), TEXT("TicksToComplete = 11"));
	TestEqualSensitive(TEXT("saving writes the change that no transaction saw"), Load(Path), SavedText);
	TestEqualSensitive(TEXT("and the asset is in step with it"), GeneratedText(*Tree), SavedText);

	// Leave nothing waiting.
	Subsystem->UseText(*Tree);
	return true;
}

// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamStateTreeEditorLiveTest, "DreamStateTree.Editor.Live",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The same over frames, as it happens while someone works: the folder watcher reports a saved file and the asset is
 * rebuilt; an edit made in a transaction is written back once edits pause, and the watcher's report of that write
 * rebuilds nothing.
 */
bool FDreamStateTreeEditorLiveTest::RunTest(const FString& Parameters)
{
	using namespace DreamStateTreeEditorSyncTests;

	UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem();
	if (!TestNotNull(TEXT("the subsystem"), Subsystem))
	{
		return false;
	}

	const TSharedRef<FScratch> Scratch = MakeShared<FScratch>(*Subsystem, ScratchRoot(TEXT("Live")), TEXT("Live"));
	Scratch->AddAsset(TEXT("/Game/DreamStateTreeTests/Live/ST_Live.ST_Live"));
	const FString Text = FString(SourceText).Replace(TEXT("Sync/ST_Sync"), TEXT("Live/ST_Live"));
	const FString Path = Scratch->Write(TEXT("ST_Live.dtr"), Text);
	Scratch->Activate();

	UStateTree* Tree = Subsystem->Rebuild(Path);
	if (!TestNotNull(TEXT("the asset is built from the source"), Tree))
	{
		return false;
	}
	const TWeakObjectPtr<UStateTree> WeakTree(Tree);
	const TSharedRef<int32> BuiltCount = MakeShared<int32>(Subsystem->GetRebuildCount());

	// Saved by someone else: the watcher reports it and the asset is rebuilt.
	const FString Saved = Text.Replace(TEXT("float Speed = 0;"), TEXT("float Speed = 7;"));
	Save(Path, Saved);
	const TSharedRef<double> Start = MakeShared<double>(FPlatformTime::Seconds());
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, WeakTree, Saved, Start, Subsystem, BuiltCount]()
	{
		if (WeakTree.IsValid() && GeneratedText(*WeakTree).Equals(Saved, ESearchCase::CaseSensitive))
		{
			*BuiltCount = Subsystem->GetRebuildCount();
			return true;
		}
		if (FPlatformTime::Seconds() - *Start > 15.0)
		{
			AddError(TEXT("a saved source rebuilds its asset"));
			return true;
		}
		return false;
	}));

	// Edited in the editor: written back once edits pause.
	const FString Edited = Saved.Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = 6"));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, WeakTree]()
	{
		if (UStateTree* Live = WeakTree.Get())
		{
			SetTicks(*this, *Live, 6, true);
		}
		return true;
	}));
	const TSharedRef<double> EditStart = MakeShared<double>(0.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Path, Edited, EditStart]()
	{
		if (*EditStart == 0.0)
		{
			*EditStart = FPlatformTime::Seconds();
		}
		if (Load(Path).Equals(Edited, ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - *EditStart > 30.0)
		{
			AddError(FString::Printf(TEXT("an edit in a transaction is written back; the file holds:\n%s"), *Load(Path)));
			return true;
		}
		return false;
	}));

	// Then the watcher reports the write; it is not a change.
	const TSharedRef<double> EchoStart = MakeShared<double>(0.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, WeakTree, Edited, Subsystem, BuiltCount, EchoStart]()
	{
		if (*EchoStart == 0.0)
		{
			*EchoStart = FPlatformTime::Seconds();
		}
		if (FPlatformTime::Seconds() - *EchoStart < 1.5)
		{
			return false;
		}
		UStateTree* Live = WeakTree.Get();
		if (TestNotNull(TEXT("the asset is still there"), Live))
		{
			TestFalse(TEXT("its own write is not taken for a conflict"), Subsystem->IsInConflict(*Live));
			TestEqual(TEXT("its own write rebuilds nothing"), Subsystem->GetRebuildCount(), *BuiltCount);
			TestEqualSensitive(TEXT("the asset keeps the written text"), GeneratedText(*Live), Edited);
			TestEqual(TEXT("and its value"), GetTicks(*Live), 6);
			TestFalse(TEXT("nothing waits to be written"), Subsystem->HasPendingWriteBack(*Live));
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Scratch]()
	{
		Scratch->Release();
		return true;
	}));
	return true;
}

// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamStateTreeEditorLibraryTest, "DreamStateTree.Editor.Library",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** A library that changed rebuilds the trees that include it, though their own text did not. */
bool FDreamStateTreeEditorLibraryTest::RunTest(const FString& Parameters)
{
	using namespace DreamStateTreeEditorSyncTests;

	UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem();
	if (!TestNotNull(TEXT("the subsystem"), Subsystem))
	{
		return false;
	}

	FScratch Scratch(*Subsystem, ScratchRoot(TEXT("Library")), TEXT("Lib"));
	Scratch.AddAsset(TEXT("/Game/DreamStateTreeTests/Lib/ST_Lib.ST_Lib"));
	const FString LibraryBefore = TEXT("StateTreeLibrary(Name=\"DreamStateTreeTests/Lib/Common\")\n{\n    Template Ticks = 4;\n}\n");
	const FString TreeText = TEXT("#include \"Common.dth\"\n")
		+ FString(SourceText).Replace(TEXT("Sync/ST_Sync"), TEXT("Lib/ST_Lib")).Replace(TEXT("TicksToComplete = 3"), TEXT("TicksToComplete = Ticks"));
	const FString LibraryPath = Scratch.Write(TEXT("Common.dth"), LibraryBefore);
	const FString TreePath = Scratch.Write(TEXT("ST_Lib.dtr"), TreeText);
	Scratch.Activate();

	UStateTree* Tree = Subsystem->Rebuild(TreePath);
	if (!TestNotNull(TEXT("the asset is built from the source and its library"), Tree))
	{
		return false;
	}
	TestEqual(TEXT("the library's value is in the asset"), GetTicks(*Tree), 4);
	const int32 Built = Subsystem->GetRebuildCount();

	const FString LibraryAfter = LibraryBefore.Replace(TEXT("Ticks = 4;"), TEXT("Ticks = 6;"));
	Save(LibraryPath, LibraryAfter);
	Subsystem->HandleChangedSources({ LibraryPath });
	TestEqual(TEXT("a changed library rebuilds the tree that includes it"), Subsystem->GetRebuildCount(), Built + 1);
	TestEqual(TEXT("with the library's new value"), GetTicks(*Tree), 6);
	TestEqualSensitive(TEXT("the tree's own text is as it was"), GeneratedText(*Tree), TreeText);
	TestFalse(TEXT("no conflict without a change of its own"), Subsystem->IsInConflict(*Tree));
	return true;
}

// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamStateTreeEditorDecompileTest, "DreamStateTree.Editor.Decompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * Writing an asset as text: the file goes to the default place under the project's DTree folder, an existing file is
 * replaced only when asked, and the asset is rebuilt from the new text so that it is in step with it.
 */
bool FDreamStateTreeEditorDecompileTest::RunTest(const FString& Parameters)
{
	using namespace DreamStateTreeEditorSyncTests;

	UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem();
	if (!TestNotNull(TEXT("the subsystem"), Subsystem))
	{
		return false;
	}

	// The default place for /Game/DreamStateTreeTests/Decompile/ST_Decompile is this very file, written by hand with
	// an indentation the printer does not use, so that writing it out again is a change.
	FScratch Scratch(*Subsystem, FPaths::ProjectDir() / TEXT("DTree"), TEXT("Decompile"));
	Scratch.AddAsset(TEXT("/Game/DreamStateTreeTests/Decompile/ST_Decompile.ST_Decompile"));
	const FString Odd = FString(SourceText).Replace(TEXT("Sync/ST_Sync"), TEXT("Decompile/ST_Decompile")).Replace(TEXT("    "), TEXT(" "));
	const FString Path = Scratch.Write(TEXT("ST_Decompile.dtr"), Odd);
	Scratch.Activate();

	UStateTree* Tree = Subsystem->Rebuild(Path);
	if (!TestNotNull(TEXT("the asset is built from the source"), Tree))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the asset keeps the text it was built from"), GeneratedText(*Tree), Odd);

	const int32 Built = Subsystem->GetRebuildCount();
	TestTrue(TEXT("a file that differs is not replaced unasked"), Subsystem->Decompile(*Tree, false).IsEmpty());
	TestEqualSensitive(TEXT("so the file is as it was"), Load(Path), Odd);
	TestEqual(TEXT("and nothing was rebuilt"), Subsystem->GetRebuildCount(), Built);

	const FString Written = Subsystem->Decompile(*Tree, true);
	TestTrue(TEXT("asked, it writes the file at the default place"), Written.Equals(Path, ESearchCase::IgnoreCase));
	const FString Canonical = Load(Path);
	TestFalse(TEXT("the file is the printer's text now"), Canonical.Equals(Odd, ESearchCase::CaseSensitive));
	TestTrue(TEXT("and has the header of the asset"), Canonical.Contains(TEXT("DreamStateTreeTests/Decompile/ST_Decompile")));
	TestEqual(TEXT("the asset was rebuilt from it"), Subsystem->GetRebuildCount(), Built + 1);
	TestEqualSensitive(TEXT("so it keeps that text"), GeneratedText(*Tree), Canonical);
	TestEqual(TEXT("with its value"), GetTicks(*Tree), 3);
	TestFalse(TEXT("and is in step with it"), Subsystem->HasEditorChanges(*Tree));
	TestFalse(TEXT("with nothing waiting"), Subsystem->IsInConflict(*Tree));
	return true;
}

#endif
