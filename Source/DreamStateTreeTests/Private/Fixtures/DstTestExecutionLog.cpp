// Copyright DreamMoon. All Rights Reserved.

#include "Fixtures/DstTestExecutionLog.h"

#include "StateTreeNodeBase.h"

namespace UE::DreamStateTree::Tests
{
	FExecutionLog& FExecutionLog::Get()
	{
		static FExecutionLog Instance;
		return Instance;
	}

	void FExecutionLog::Reset()
	{
		Entries.Reset();
	}

	void FExecutionLog::Add(const FName Node, const FString& Message)
	{
		FExecutionLogEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Node = Node;
		Entry.Message = Message;
	}

	int32 FExecutionLog::IndexOf(const FName Node, const FString& Message, const int32 StartIndex) const
	{
		for (int32 Index = FMath::Max(StartIndex, 0); Index < Entries.Num(); ++Index)
		{
			if (Entries[Index].Node == Node && Entries[Index].Message == Message)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	int32 FExecutionLog::IndexOfContaining(const FName Node, const FString& Text, const int32 StartIndex) const
	{
		for (int32 Index = FMath::Max(StartIndex, 0); Index < Entries.Num(); ++Index)
		{
			if (Entries[Index].Node == Node && Entries[Index].Message.Contains(Text))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	int32 FExecutionLog::CountWithPrefix(const FName Node, const FString& Prefix) const
	{
		int32 Count = 0;
		for (const FExecutionLogEntry& Entry : Entries)
		{
			if (Entry.Node == Node && Entry.Message.StartsWith(Prefix))
			{
				++Count;
			}
		}
		return Count;
	}

	FString FExecutionLog::ToString(const int32 StartIndex) const
	{
		FString Result;
		for (int32 Index = FMath::Max(StartIndex, 0); Index < Entries.Num(); ++Index)
		{
			Result += FString::Printf(TEXT("%s: %s\n"), *Entries[Index].Node.ToString(), *Entries[Index].Message);
		}
		return Result;
	}

	void LogNode(const FStateTreeNodeBase& Node, const FString& Message)
	{
		FExecutionLog::Get().Add(Node.Name, Message);
	}
}
