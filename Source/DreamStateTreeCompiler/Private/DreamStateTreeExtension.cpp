// Copyright DreamMoon. All Rights Reserved.

#include "DreamStateTreeExtension.h"

#include "StateTreeSchema.h"
#include "UObject/UObjectHash.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DreamStateTreeExtension)

void FDreamStateTreeDiagnostics::Report(EDreamStateTreeSeverity Severity, FName Code, const FGuid& Where, const FString& Message)
{
	FEntry& Entry = Entries.AddDefaulted_GetRef();
	Entry.Severity = Severity;
	Entry.Code = Code;
	Entry.Where = Where;
	Entry.Message = Message;
}

void FDreamStateTreeDiagnostics::Error(FName Code, const FGuid& Where, const FString& Message)
{
	Report(EDreamStateTreeSeverity::Error, Code, Where, Message);
}

void FDreamStateTreeDiagnostics::Warning(FName Code, const FGuid& Where, const FString& Message)
{
	Report(EDreamStateTreeSeverity::Warning, Code, Where, Message);
}

bool FDreamStateTreeDiagnostics::HasErrors() const
{
	return Entries.ContainsByPredicate([](const FEntry& Entry) { return Entry.Severity == EDreamStateTreeSeverity::Error; });
}

bool UDreamStateTreeExtension::AppliesTo(const UStateTreeSchema& Schema) const
{
	return false;
}

const UScriptStruct* UDreamStateTreeExtension::GetDefaultEventPayload(const UStateTreeSchema& Schema, const FGameplayTag& Tag) const
{
	return nullptr;
}

void UDreamStateTreeExtension::Validate(const UStateTreeEditorData& EditorData, FDreamStateTreeDiagnostics& Out) const
{
}

TArray<const UDreamStateTreeExtension*> UDreamStateTreeExtension::FindFor(const UStateTreeSchema& Schema)
{
	TArray<UClass*> Classes;
	GetDerivedClasses(StaticClass(), Classes, /*bRecursive*/ true);
	Classes.RemoveAll([](const UClass* Class)
	{
		return Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
	});
	Classes.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });

	TArray<const UDreamStateTreeExtension*> Result;
	for (const UClass* Class : Classes)
	{
		const UDreamStateTreeExtension* Extension = GetDefault<UDreamStateTreeExtension>(Class);
		if (Extension && Extension->AppliesTo(Schema))
		{
			Result.Add(Extension);
		}
	}
	return Result;
}
