// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"

#include "DreamStateTreeExtension.generated.h"

class UStateTreeEditorData;
class UStateTreeSchema;
class UScriptStruct;

/** How bad a diagnostic an extension reports is. */
UENUM()
enum class EDreamStateTreeSeverity : uint8
{
	Error,
	Warning,
	Info,
};

/**
 * Where an extension reports what it found. DreamStateTree places each entry in the source text: `Where` is any ID
 * of the tree's editor data (a state's ID, an FStateTreeEditorNode's ID, a transition's ID, a parameter property's
 * ID) and lands on the line that wrote it. An invalid FGuid lands on the file header.
 */
struct DREAMSTATETREECOMPILER_API FDreamStateTreeDiagnostics
{
	struct FEntry
	{
		EDreamStateTreeSeverity Severity = EDreamStateTreeSeverity::Error;
		/** The extension's own code, such as GAME0001. */
		FName Code;
		FGuid Where;
		FString Message;
	};

	void Report(EDreamStateTreeSeverity Severity, FName Code, const FGuid& Where, const FString& Message);
	void Error(FName Code, const FGuid& Where, const FString& Message);
	void Warning(FName Code, const FGuid& Where, const FString& Message);

	const TArray<FEntry>& GetEntries() const { return Entries; }
	bool HasErrors() const;

private:
	TArray<FEntry> Entries;
};

/**
 * Lets a game or another plugin teach DreamStateTree about its own schemas. Subclass it in an editor module; there
 * is nothing to register: every class that is not abstract is found, and its default object answers.
 *
 * Rules every tree of a schema must keep, hand-built or generated, belong in that schema's editor schema
 * (UStateTreeEditorSchema::HandlePostInternalCompile), where they fail the engine's compile; DreamStateTree places
 * those compile messages in the text as well. This class is for what only text needs.
 */
UCLASS(Abstract)
class DREAMSTATETREECOMPILER_API UDreamStateTreeExtension : public UObject
{
	GENERATED_BODY()

public:
	/** Whether this extension serves trees of the schema. Asked once per tree and build. */
	virtual bool AppliesTo(const UStateTreeSchema& Schema) const;

	/** The payload type `on <Tag>` gets when the text names none; nullptr when there is no default. */
	virtual const UScriptStruct* GetDefaultEventPayload(const UStateTreeSchema& Schema, const FGameplayTag& Tag) const;

	/** Further checks once the tree is generated and compiled. */
	virtual void Validate(const UStateTreeEditorData& EditorData, FDreamStateTreeDiagnostics& Out) const;

	/** The extensions that apply to the schema, in a stable order (by class path). */
	static TArray<const UDreamStateTreeExtension*> FindFor(const UStateTreeSchema& Schema);
};
