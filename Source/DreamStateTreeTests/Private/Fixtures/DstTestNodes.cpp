// Copyright DreamMoon. All Rights Reserved.

#include "Fixtures/DstTestNodes.h"

#include "Fixtures/DstTestExecutionLog.h"
#include "StateTreeExecutionContext.h"

using UE::DreamStateTree::Tests::LogNode;

namespace UE::DreamStateTree::Tests::Private
{
	// Log lines must not contain object names or pointers: two runs of the same tree have to produce the same text.
	FString DescribeOwner(const UDstTestOwner* Owner)
	{
		return Owner ? FString::Printf(TEXT("Owner(Level=%d)"), Owner->Level) : FString(TEXT("null"));
	}
}

//----------------------------------------------------------------------//
// FDstTestTask
//----------------------------------------------------------------------//

EStateTreeRunStatus FDstTestTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.TickCount = 0;

	FString RefText(TEXT("unbound"));
	if (const int32* RefValue = Data.CounterRef.GetMutablePtr<int32>(Context))
	{
		RefText = FString::FromInt(*RefValue);
	}

	LogNode(*this, FString::Printf(TEXT("Enter:Input=%g Scale=%g Amount=%d Severity=%g Mode=%d Route=%d Ref=%s %s"),
		Data.InputValue, Data.Scale, Data.PayloadAmount, Data.PayloadSeverity, static_cast<int32>(Data.Mode), Data.Route.Num(), *RefText,
		*UE::DreamStateTree::Tests::Private::DescribeOwner(Data.Owner)));

	return EStateTreeRunStatus::Running;
}

void FDstTestTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);
	LogNode(*this, FString::Printf(TEXT("Exit:TickCount=%d"), Data.TickCount));
}

EStateTreeRunStatus FDstTestTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	++Data.TickCount;
	LogNode(*this, FString::Printf(TEXT("Tick:%d"), Data.TickCount));

	if (Data.TickCount < Data.TicksToComplete)
	{
		return EStateTreeRunStatus::Running;
	}

	Context.BroadcastDelegate(Data.OnFinished);

	const EStateTreeRunStatus Result = Data.CompletionStatus == EStateTreeRunStatus::Failed
		? EStateTreeRunStatus::Failed
		: EStateTreeRunStatus::Succeeded;
	LogNode(*this, Result == EStateTreeRunStatus::Failed ? TEXT("Done:Failed") : TEXT("Done:Succeeded"));
	return Result;
}

//----------------------------------------------------------------------//
// FDstTestSendEventTask
//----------------------------------------------------------------------//

EStateTreeRunStatus FDstTestSendEventTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	const FName Origin(TEXT("DstTestSendEvent"));
	if (Data.bSendPayload)
	{
		Context.SendEvent(Data.Tag, FConstStructView::Make(Data.Payload), Origin);
	}
	else
	{
		Context.SendEvent(Data.Tag, FConstStructView(), Origin);
	}

	LogNode(*this, FString::Printf(TEXT("Sent:%s Payload=%s Amount=%d"), *Data.Tag.ToString(), Data.bSendPayload ? TEXT("yes") : TEXT("no"), Data.Payload.Amount));
	return EStateTreeRunStatus::Running;
}

//----------------------------------------------------------------------//
// FDstTestCondition
//----------------------------------------------------------------------//

bool FDstTestCondition::TestCondition(FStateTreeExecutionContext& Context) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	bool bResult = false;
	switch (Compare)
	{
	case EDstTestCompare::Greater:
		bResult = Data.Left > Data.Right;
		break;
	case EDstTestCompare::GreaterOrEqual:
		bResult = Data.Left >= Data.Right;
		break;
	case EDstTestCompare::Less:
		bResult = Data.Left < Data.Right;
		break;
	case EDstTestCompare::Equal:
		bResult = FMath::IsNearlyEqual(Data.Left, Data.Right);
		break;
	}

	if (bInvert)
	{
		bResult = !bResult;
	}

	LogNode(*this, FString::Printf(TEXT("Test:Left=%g Right=%g Result=%s"), Data.Left, Data.Right, bResult ? TEXT("true") : TEXT("false")));
	return bResult;
}

//----------------------------------------------------------------------//
// FDstTestEvaluator
//----------------------------------------------------------------------//

void FDstTestEvaluator::TreeStart(FStateTreeExecutionContext& Context) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.Counter = 0;
	LogNode(*this, FString::Printf(TEXT("TreeStart:Step=%d"), Data.Step));
}

void FDstTestEvaluator::TreeStop(FStateTreeExecutionContext& Context) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);
	LogNode(*this, FString::Printf(TEXT("TreeStop:Counter=%d"), Data.Counter));
}

void FDstTestEvaluator::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.Counter += Data.Step;
	LogNode(*this, FString::Printf(TEXT("Tick:Counter=%d"), Data.Counter));
}

//----------------------------------------------------------------------//
// FDstTestConsideration
//----------------------------------------------------------------------//

float FDstTestConsideration::GetScore(FStateTreeExecutionContext& Context) const
{
	return Context.GetInstanceData(*this).Score;
}
