// Copyright DreamMoon. All Rights Reserved.

#include "Fixtures/DstTestNamedNodes.h"

#include "Fixtures/DstTestExecutionLog.h"
#include "StateTreeExecutionContext.h"

using UE::DreamStateTree::Tests::LogNode;

EStateTreeRunStatus FDstTestMoveTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.Travelled = 0.f;
	LogNode(*this, FString::Printf(TEXT("Enter:Speed=%g Sprint=%d"), Data.Speed, Data.bSprint ? 1 : 0));
	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FDstTestMoveTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.Travelled += Data.bSprint ? Data.Speed * 2.f : Data.Speed;
	LogNode(*this, FString::Printf(TEXT("Tick:Travelled=%g"), Data.Travelled));
	return EStateTreeRunStatus::Succeeded;
}
