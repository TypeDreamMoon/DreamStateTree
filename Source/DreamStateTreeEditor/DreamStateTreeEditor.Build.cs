// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// The editor side: source folder watching, rebuild on save, write-back of edits, asset menus and settings.
public class DreamStateTreeEditor : ModuleRules
{
	public DreamStateTreeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"DeveloperSettings",
			"EditorSubsystem",
			"DreamStateTreeLang",
			"DreamStateTreeCompiler",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"AssetTools",
			"ContentBrowser",
			"DirectoryWatcher",
			"InputCore",
			"MessageLog",
			"Slate",
			"SlateCore",
			"StateTreeModule",
			"StateTreeEditorModule",
			"ToolMenus",
		});

		// StateTreeEditorModule.h includes BlueprintEditorModule.h, which lives in Kismet.
		PrivateIncludePathModuleNames.Add("Kismet");
	}
}
