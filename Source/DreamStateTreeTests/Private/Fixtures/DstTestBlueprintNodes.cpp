// Copyright DreamMoon. All Rights Reserved.

#include "Fixtures/DstTestBlueprintNodes.h"

#include "Fixtures/DstTestExecutionLog.h"

UDstTestBlueprintTask::UDstTestBlueprintTask(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// The base class sets bShouldCallTick to false because Blueprint subclasses opt in by implementing Tick;
	// this task only needs EnterState and ExitState.
}

EStateTreeRunStatus UDstTestBlueprintTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition)
{
	UE::DreamStateTree::Tests::FExecutionLog::Get().Add(TEXT("DstTestBlueprintTask"), FString::Printf(TEXT("Enter:Strength=%d"), Strength));
	return Super::EnterState(Context, Transition);
}

void UDstTestBlueprintTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition)
{
	UE::DreamStateTree::Tests::FExecutionLog::Get().Add(TEXT("DstTestBlueprintTask"), FString::Printf(TEXT("Exit:Strength=%d"), Strength));
	Super::ExitState(Context, Transition);
}
