// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DreamStateTreeEditorSettings.generated.h"

/** How the editor keeps StateTree assets and their DreamStateTree sources in step (Editor Preferences > Plugins > Dream State Tree). */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Dream State Tree"))
class DREAMSTATETREEEDITOR_API UDreamStateTreeEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Rebuild a StateTree when its source (or a library it includes) changes on disk. */
	UPROPERTY(config, EditAnywhere, Category = "Text to StateTree")
	bool bRebuildWhenTextChanges = true;

	/** Save an asset after rebuilding it, so the asset on disk matches the text. */
	UPROPERTY(config, EditAnywhere, Category = "Text to StateTree")
	bool bSaveRebuiltAssets = true;

	/** Write changes made to a StateTree in the editor back into its source. */
	UPROPERTY(config, EditAnywhere, Category = "StateTree to Text")
	bool bWriteBackEditorChanges = true;

	/** Also write added, removed and reordered states, tasks, conditions and transitions, by rewriting the card or section they are in. */
	UPROPERTY(config, EditAnywhere, Category = "StateTree to Text", meta = (EditCondition = "bWriteBackEditorChanges"))
	bool bWriteBackStructure = true;

	/** Seconds without further edits before the changes are written. */
	UPROPERTY(config, EditAnywhere, Category = "StateTree to Text", meta = (EditCondition = "bWriteBackEditorChanges", ClampMin = "0.1", ClampMax = "30"))
	float WriteBackDelay = 1.0f;

	/**
	 * How a source is opened at a line: {file}, {line} and {column} are replaced. Run through the shell, so
	 * a script on the PATH works (`code` is VS Code's). Empty opens the file with its associated program.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Sources")
	FString OpenSourceCommand = TEXT("code -g \"{file}:{line}:{column}\"");

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
