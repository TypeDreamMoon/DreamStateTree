// Copyright DreamMoon. All Rights Reserved.

#include "Fixtures/DstTestListNodes.h"

#include "Fixtures/DstTestExecutionLog.h"
#include "StateTreeExecutionContext.h"

using UE::DreamStateTree::Tests::LogNode;

EStateTreeRunStatus FDstTestListTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	Data.Total = 0.f;
	TArray<FString> Entries;
	for (const FDstTestWaypoint& Waypoint : Data.Waypoints)
	{
		Data.Total += Waypoint.Weight * Waypoint.Count;
		TArray<FString> Stops;
		for (const float Stop : Waypoint.Stops)
		{
			Stops.Add(FString::Printf(TEXT("%g"), Stop));
		}
		Entries.Add(FString::Printf(TEXT("%s:%g*%d%s@%g,%g,%g tint=%g,%g,%g,%g stops=%s"), *Waypoint.Label.ToString(), Waypoint.Weight, Waypoint.Count,
			Waypoint.bActive ? TEXT("") : TEXT("(off)"), Waypoint.Offset.X, Waypoint.Offset.Y, Waypoint.Offset.Z,
			Waypoint.Tint.R, Waypoint.Tint.G, Waypoint.Tint.B, Waypoint.Tint.A, *FString::Join(Stops, TEXT("/"))));
	}
	TArray<FString> Weights;
	for (const float Weight : Data.Weights)
	{
		Data.Total += Weight;
		Weights.Add(FString::Printf(TEXT("%g"), Weight));
	}

	TArray<int32> Marks = Data.Marks.Array();
	Marks.Sort();
	TArray<FString> MarkTexts;
	for (const int32 Mark : Marks)
	{
		MarkTexts.Add(FString::FromInt(Mark));
	}

	LogNode(*this, FString::Printf(TEXT("Enter:Waypoints=[%s] Weights=[%s] Range=%g..%g Marks={%s} Total=%g"), *FString::Join(Entries, TEXT("; ")),
		*FString::Join(Weights, TEXT(",")), Data.Range.Min, Data.Range.Max, *FString::Join(MarkTexts, TEXT(",")), Data.Total));
	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FDstTestListTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	LogNode(*this, TEXT("Tick"));
	return EStateTreeRunStatus::Succeeded;
}
