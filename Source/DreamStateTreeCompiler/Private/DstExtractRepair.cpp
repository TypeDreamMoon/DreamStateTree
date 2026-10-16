// Copyright DreamMoon. All Rights Reserved.

#include "DstExtractRepair.h"

#include "PropertyBindingPath.h"
#include "StateTreeEvents.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Extracting
{
	Model::FDstPropertyPath ToDstPropertyPath(const FPropertyBindingPath& Path)
	{
		Model::FDstPropertyPath Result;
		for (const FPropertyBindingPathSegment& Segment : Path.GetSegments())
		{
			Model::FDstPathSegment& Out = Result.Segments.AddDefaulted_GetRef();
			Out.Name = Segment.GetName().ToString();
			Out.ArrayIndex = Segment.GetArrayIndex() >= 0 ? Segment.GetArrayIndex() : INDEX_NONE;
			if (const UStruct* InstanceStruct = Segment.GetInstanceStruct())
			{
				Out.InstanceType = InstanceStruct->GetPathName();
			}
		}
		return Result;
	}

	namespace
	{
		/** Whether the event Candidate offers what the (event) binding path reads. */
		bool CandidateFitsPath(const FDstEventCandidate& Candidate, const FPropertyBindingPath& Path)
		{
			static const FName PayloadName(TEXT("Payload"));

			const FPropertyBindingPathSegment& First = Path.GetSegment(0);
			if (First.GetName() != PayloadName)
			{
				// Tag and Origin exist on every event.
				return true;
			}
			if (!Candidate.PayloadStruct)
			{
				return false;
			}
			if (const UStruct* RecordedType = First.GetInstanceStruct())
			{
				return RecordedType == Candidate.PayloadStruct;
			}
			if (Path.NumSegments() >= 2)
			{
				return Candidate.PayloadStruct->FindPropertyByName(Path.GetSegment(1).GetName()) != nullptr;
			}
			return true;
		}
	}

	EDstEventRepair RepairBrokenEventSource(const FPropertyBindingPath& SourcePath, const TConstArrayView<FDstEventCandidate> Candidates,
		Model::FDstSourceRef& OutSource, int32* OutNumMatches)
	{
		if (OutNumMatches)
		{
			*OutNumMatches = 0;
		}

		if (SourcePath.NumSegments() == 0 || !FStateTreeEvent::StaticStruct()->FindPropertyByName(SourcePath.GetSegment(0).GetName()))
		{
			return EDstEventRepair::NotAnEventPath;
		}

		const FDstEventCandidate* Match = nullptr;
		int32 NumMatches = 0;
		for (const FDstEventCandidate& Candidate : Candidates)
		{
			if (CandidateFitsPath(Candidate, SourcePath))
			{
				Match = Match ? Match : &Candidate;
				++NumMatches;
			}
		}

		if (OutNumMatches)
		{
			*OutNumMatches = NumMatches;
		}
		if (NumMatches == 0)
		{
			return EDstEventRepair::NoMatch;
		}
		if (NumMatches > 1)
		{
			return EDstEventRepair::Ambiguous;
		}

		OutSource = Model::FDstSourceRef();
		OutSource.Kind = Match->Kind;
		OutSource.Owner = Match->Owner;
		OutSource.Path = ToDstPropertyPath(SourcePath);
		return EDstEventRepair::Repaired;
	}
}
