// Copyright DreamMoon. All Rights Reserved.
//
// The two pieces of the extractor that stand on their own, so a test can drive them without building an asset:
//
//   ToDstPropertyPath          a binding path as the model writes it (`Payload<Struct>.Amount`, `Items[2].X`);
//   RepairBrokenEventSource    the repair of an event-payload binding whose source struct no longer exists.
//
// Why the repair exists. The engine gives a state's "Enter Event" struct and a transition's event struct IDs derived from
// the state and transition IDs (Combine(ID, "Event")). Duplicating an asset (Save As, Duplicate) runs
// UStateTreeEditorData::DuplicateIDs, which hands every ID a new random value, the event IDs included, and then remaps
// the IDs stored in the data through that table. The state and the transition get their new ID from the table, but the
// event ID the bindings were remapped to is a second, different random value: it is not Combine(new ID, "Event"), so no
// struct owns it any more. Every binding that reads an event payload in the copy points at nothing.
//
// What the repair does with such a binding: if the first segment of its source path is a field of FStateTreeEvent
// (Tag, Payload, Origin) it is an event binding; of the event sources the binding could legitimately read from where it
// sits, the ones whose payload type fits the path are the candidates; exactly one candidate means the binding is
// resolved to it. Anything else stays unresolved and the caller reports it.
//
// Declared with the module's export macro so the test module can call them; the header itself is private.

#pragma once

#include "CoreMinimal.h"
#include "Model/DstModel.h"

class UScriptStruct;
struct FPropertyBindingPath;

namespace UE::DreamStateTree::Compiler::Extracting
{
	/** A binding path as the model writes it: names, array indices, and the struct path of each instanced segment. */
	DREAMSTATETREECOMPILER_API Model::FDstPropertyPath ToDstPropertyPath(const FPropertyBindingPath& Path);

	/** An event a binding could read from, given where the binding sits. */
	struct FDstEventCandidate
	{
		/** StateEvent or TransitionEvent. */
		Model::EDstSourceKind Kind = Model::EDstSourceKind::StateEvent;
		/** The state path, or `<state path>#<transition index>`. */
		FString Owner;
		/** The payload struct the event declares; null when it has none. */
		const UScriptStruct* PayloadStruct = nullptr;
	};

	enum class EDstEventRepair : uint8
	{
		/** The path does not start with a field of the event struct (or is empty): this is not an event binding. */
		NotAnEventPath,
		/** An event binding, but no candidate fits it (or there are none). */
		NoMatch,
		/** An event binding that more than one candidate fits. */
		Ambiguous,
		/** Exactly one candidate fits; OutSource is set. */
		Repaired,
	};

	/**
	 * Resolves SourcePath, a binding source whose struct ID nobody owns, to the one event Candidates offers that fits it.
	 * A path into `Payload` fits a candidate when the segment's recorded payload type is the candidate's payload struct;
	 * with no recorded type, when the candidate has a payload struct that has the next segment's property (or any payload
	 * struct, for a path that ends at `Payload`). `Tag` and `Origin` fit every candidate. OutNumMatches, when given,
	 * receives how many candidates fit.
	 */
	DREAMSTATETREECOMPILER_API EDstEventRepair RepairBrokenEventSource(const FPropertyBindingPath& SourcePath,
		TConstArrayView<FDstEventCandidate> Candidates, Model::FDstSourceRef& OutSource, int32* OutNumMatches = nullptr);
}
