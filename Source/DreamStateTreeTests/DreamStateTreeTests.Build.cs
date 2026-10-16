// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// Automation tests and their fixtures (test nodes and a test schema). UncookedOnly rather than Editor so the
// module also loads in an uncooked -game run.
public class DreamStateTreeTests : ModuleRules
{
	public DreamStateTreeTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",
			"StateTreeModule",
			"StateTreeEditorModule",
			"PropertyBindingUtils",
			"PropertyBindingUtilsEditor",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AIModule",
			"GameplayStateTreeModule",
			"UnrealEd",
			"AssetTools",
			"AssetRegistry",
			"DreamStateTreeLang",
			"DreamStateTreeCompiler",
		});
	}
}
