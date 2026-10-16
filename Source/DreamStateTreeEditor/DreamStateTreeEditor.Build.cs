// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// The editor side: source folder watching, rebuild on save, write-back of edits, asset menus and settings.
public class DreamStateTreeEditor : ModuleRules
{
	public DreamStateTreeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"StateTreeModule",
			"StateTreeEditorModule",
			"DreamStateTreeLang",
			"DreamStateTreeCompiler",
		});
	}
}
