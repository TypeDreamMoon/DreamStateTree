// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FStateTreeNodeBase;

namespace UE::DreamStateTree::Tests
{
	/** One thing a fixture node did while the tree ran. */
	struct FExecutionLogEntry
	{
		/** Name of the node that wrote the entry. */
		FName Node;
		/** What happened, e.g. "Enter:Input=4 Amount=7". */
		FString Message;
	};

	/**
	 * Process-wide record of what the fixture nodes did, in the order they did it. The nodes are stateless structs
	 * that run inside an execution context the test does not own, so they write here instead of into external data.
	 * Tests call Reset before a run and read the entries afterwards. Single threaded: StateTree runs on the caller.
	 */
	class FExecutionLog
	{
	public:
		static FExecutionLog& Get();

		void Reset();
		void Add(const FName Node, const FString& Message);

		/** Number of entries so far. Use it as a mark to read only what a later step added. */
		int32 Num() const { return Entries.Num(); }
		TConstArrayView<FExecutionLogEntry> GetEntries() const { return Entries; }

		/** Index of the first entry at or after StartIndex written by Node with exactly this message; INDEX_NONE when absent. */
		int32 IndexOf(const FName Node, const FString& Message, const int32 StartIndex = 0) const;
		/** Index of the first entry at or after StartIndex written by Node whose message contains the text; INDEX_NONE when absent. */
		int32 IndexOfContaining(const FName Node, const FString& Text, const int32 StartIndex = 0) const;
		bool Contains(const FName Node, const FString& Message) const { return IndexOf(Node, Message) != INDEX_NONE; }
		bool ContainsText(const FName Node, const FString& Text) const { return IndexOfContaining(Node, Text) != INDEX_NONE; }
		/** Number of entries written by Node whose message starts with the prefix. */
		int32 CountWithPrefix(const FName Node, const FString& Prefix) const;

		/** One line per entry from StartIndex on, "Node: Message". */
		FString ToString(const int32 StartIndex = 0) const;

	private:
		TArray<FExecutionLogEntry> Entries;
	};

	/** Writes an entry named after the node. */
	void LogNode(const FStateTreeNodeBase& Node, const FString& Message);
}
