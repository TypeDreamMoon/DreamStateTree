// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// The language: lexer, parser, syntax tree, printer, templates, diagnostics and the engine-free model.
// Core only, so its tests run without the editor.
public class DreamStateTreeLang : ModuleRules
{
	public DreamStateTreeLang(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
	}
}
