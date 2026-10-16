// Copyright DreamMoon. All Rights Reserved.
//
// Menu entries: on a StateTree in the Content Browser (open its source, rebuild it, write its changes back; or write
// one not made from text as text), and under Tools for every source at once.

#include "DreamStateTreeEditorMenus.h"

#include "ContentBrowserMenuContexts.h"
#include "DreamStateTreeEditorSubsystem.h"
#include "DstDriver.h"
#include "Editor.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "StateTree.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "DreamStateTreeEditor"

namespace DreamStateTreeEditorMenus
{
	using namespace UE::DreamStateTree::Compiler;

	UDreamStateTreeEditorSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UDreamStateTreeEditorSubsystem>() : nullptr;
	}

	TArray<UStateTree*> LoadTrees(const TArray<FAssetData>& Assets)
	{
		TArray<UStateTree*> Trees;
		for (const FAssetData& Asset : Assets)
		{
			if (UStateTree* Tree = Cast<UStateTree>(Asset.GetAsset()))
			{
				Trees.Add(Tree);
			}
		}
		return Trees;
	}

	/**
	 * Trees not made from text, as text; an existing file is replaced only when the user says so. Each tree is then
	 * rebuilt from its new text, which is what links the two for write-back.
	 */
	void Decompile(const TArray<FAssetData>& Assets)
	{
		UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem();
		const TArray<UStateTree*> Trees = LoadTrees(Assets);
		if (!Subsystem || Trees.IsEmpty())
		{
			return;
		}
		TArray<FString> Existing;
		for (const UStateTree* Tree : Trees)
		{
			const FString Path = GetDefaultDstDecompilePath(*Tree);
			if (FPaths::FileExists(Path))
			{
				Existing.Add(Path);
			}
		}
		const bool bOverwrite = Existing.IsEmpty() || FMessageDialog::Open(EAppMsgType::YesNo,
			FText::Format(LOCTEXT("ReplaceSources", "These files exist already:\n\n{0}\n\nReplace them?"), FText::FromString(FString::Join(Existing, TEXT("\n"))))) == EAppReturnType::Yes;
		FString First;
		for (UStateTree* Tree : Trees)
		{
			const FString Path = Subsystem->Decompile(*Tree, bOverwrite, true);
			if (First.IsEmpty())
			{
				First = Path;
			}
		}
		if (!First.IsEmpty())
		{
			Subsystem->OpenSource(First);
		}
	}

	/** Runs Action on the source of every selected asset that has one. */
	void ForEachSource(const TArray<FAssetData>& Assets, TFunctionRef<void(UDreamStateTreeEditorSubsystem&, const FDstSourceEntry&)> Action)
	{
		if (UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem())
		{
			for (const FAssetData& Asset : Assets)
			{
				if (const FDstSourceEntry* Entry = Subsystem->GetIndex().FindByAsset(Asset.GetObjectPathString()))
				{
					// A copy: the actions below rescan the index.
					const FDstSourceEntry Copy = *Entry;
					Action(*Subsystem, Copy);
				}
			}
		}
	}

	void AddAssetSection(UToolMenu* Menu)
	{
		const UContentBrowserAssetContextMenuContext* Context = Menu->FindContext<UContentBrowserAssetContextMenuContext>();
		const UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem();
		if (!Context || !Subsystem)
		{
			return;
		}
		TArray<FAssetData> WithSource;
		TArray<FAssetData> WithoutSource;
		for (const FAssetData& Asset : Context->SelectedAssets)
		{
			(Subsystem->GetIndex().FindByAsset(Asset.GetObjectPathString()) ? WithSource : WithoutSource).Add(Asset);
		}
		if (WithSource.IsEmpty() && WithoutSource.IsEmpty())
		{
			return;
		}

		FToolMenuSection& Section = Menu->AddSection(TEXT("DreamStateTree"), LOCTEXT("MenuSection", "Dream State Tree"));
		if (!WithSource.IsEmpty())
		{
			Section.AddMenuEntry(TEXT("DreamStateTree_OpenSource"), LOCTEXT("OpenSource", "Open DreamStateTree Source"),
				LOCTEXT("OpenSourceTip", "Open the text the StateTree is made from."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WithSource]()
				{
					ForEachSource(WithSource, [](UDreamStateTreeEditorSubsystem& Current, const FDstSourceEntry& Entry)
					{
						Current.OpenSource(Entry.FilePath);
					});
				})));
			Section.AddMenuEntry(TEXT("DreamStateTree_Rebuild"), LOCTEXT("Rebuild", "Rebuild from Text"),
				LOCTEXT("RebuildTip", "Compile the text into the StateTree again; changes in the asset that are not in the text are dropped."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WithSource]()
				{
					ForEachSource(WithSource, [](UDreamStateTreeEditorSubsystem& Current, const FDstSourceEntry& Entry)
					{
						Current.Rebuild(Entry.FilePath);
					});
				})));
			Section.AddMenuEntry(TEXT("DreamStateTree_WriteBack"), LOCTEXT("WriteBack", "Write Back to Text"),
				LOCTEXT("WriteBackTip", "Write what changed in the StateTree into its text now."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WithSource]()
				{
					if (UDreamStateTreeEditorSubsystem* Current = GetSubsystem())
					{
						for (UStateTree* Tree : LoadTrees(WithSource))
						{
							Current->WriteBack(*Tree);
						}
					}
				})));
		}
		if (!WithoutSource.IsEmpty())
		{
			Section.AddMenuEntry(TEXT("DreamStateTree_Decompile"), LOCTEXT("Decompile", "Write as DreamStateTree Text"),
				LOCTEXT("DecompileTip", "Write the StateTree as a DreamStateTree source under the project's DTree folder, and rebuild it from that text so edits are written back to it."),
				FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([WithoutSource]() { Decompile(WithoutSource); })));
		}
	}

	void RegisterMenus()
	{
		if (UToolMenu* AssetMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UStateTree::StaticClass()))
		{
			AssetMenu->AddDynamicSection(TEXT("DreamStateTree"), FNewToolMenuDelegate::CreateStatic(&AddAssetSection));
		}

		if (UToolMenu* Tools = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
		{
			FToolMenuSection& Section = Tools->FindOrAddSection(TEXT("DreamStateTree"), LOCTEXT("ToolsSection", "Dream State Tree"));
			Section.AddMenuEntry(TEXT("DreamStateTree_RebuildAll"), LOCTEXT("RebuildAll", "Rebuild All DreamStateTree Sources"),
				LOCTEXT("RebuildAllTip", "Compile every .dtr under the DTree folders into its asset."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					if (UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem())
					{
						Subsystem->RebuildAll();
					}
				})));
			Section.AddMenuEntry(TEXT("DreamStateTree_Rescan"), LOCTEXT("Rescan", "Rescan DreamStateTree Sources"),
				LOCTEXT("RescanTip", "Find the sources under the DTree folders again."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					if (UDreamStateTreeEditorSubsystem* Subsystem = GetSubsystem())
					{
						Subsystem->RefreshSources();
					}
				})));
			Section.AddMenuEntry(TEXT("DreamStateTree_Log"), LOCTEXT("ShowLog", "DreamStateTree Log"),
				LOCTEXT("ShowLogTip", "What rebuilds and write-backs reported."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { FMessageLog(TEXT("DreamStateTree")).Open(EMessageSeverity::Info, true); })));
		}
	}
}

#undef LOCTEXT_NAMESPACE
