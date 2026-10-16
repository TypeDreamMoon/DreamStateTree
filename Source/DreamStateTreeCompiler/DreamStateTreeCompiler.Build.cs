// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// Text to StateTree and back: semantic checks, the generator, the decompiler, write-back, the driver and the
// commandlet. Also the extension interface other projects implement in their own editor modules.
public class DreamStateTreeCompiler : ModuleRules
{
	public DreamStateTreeCompiler(ReadOnlyTargetRules Target) : base(Target)
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
			"DreamStateTreeLang",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"PropertyBindingUtils",
			"PropertyBindingUtilsEditor",
			"UnrealEd",
			"AssetTools",
			"AssetRegistry",
			"Projects",
			"Json",
		});

		// StateTreeEditorModule.h includes BlueprintEditorModule.h, which lives in Kismet.
		PrivateIncludePathModuleNames.Add("Kismet");
	}
}
