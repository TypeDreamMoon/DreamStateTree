// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeDelegate.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreePropertyRef.h"
#include "StateTreeTaskBase.h"
#include "StateTreeTypes.h"

#include "Fixtures/DstTestTypes.h"

#include "DstTestNodes.generated.h"

// Nodes of the test trees. They are stateless like every StateTree node and write what they do to
// UE::DreamStateTree::Tests::FExecutionLog, which the tests clear before a run and read afterwards.
// The property categories decide how the compiler treats a property: Input must be bound, Output can be a binding
// source, Context is bound to the schema's context object automatically, anything else is a parameter.

/** Instance data of FDstTestTask. Covers the four property categories and the kinds of values a binding can carry. */
USTRUCT()
struct FDstTestTaskInstanceData
{
	GENERATED_BODY()

	/** Input: the compiler reports an error when this is not bound. */
	UPROPERTY(EditAnywhere, Category = "Input")
	float InputValue = 0.f;

	/** Input that may stay unbound. */
	UPROPERTY(EditAnywhere, Category = "Input", meta = (Optional))
	float InputOptional = 0.f;

	/** Number of ticks after which the task completes. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 TicksToComplete = 2;

	/** Status the task completes with: Failed, anything else means Succeeded. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	EStateTreeRunStatus CompletionStatus = EStateTreeRunStatus::Succeeded;

	/** A float parameter the tests bind to property functions. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	float Scale = 1.f;

	/** Parameters the tests bind to leaves of an event payload. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 PayloadAmount = 0;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	float PayloadSeverity = 0.f;

	/** One parameter per kind of root parameter the tests declare. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bFlag = false;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	EDstTestMode Mode = EDstTestMode::Idle;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	TObjectPtr<UDstTestOwner> Target = nullptr;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	TArray<int32> Route;

	/** State link inside the instance data; the compiler resolves it to a state handle. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	FStateTreeStateLink InstanceLink;

	/** Reference to an int32 property somewhere in the tree (an evaluator output, for instance). May stay unbound. */
	UPROPERTY(EditAnywhere, Category = "Parameter", meta = (RefType = "int32", Optional))
	FStateTreePropertyRef CounterRef;

	/** Broadcast when the task completes; transitions and other tasks can listen to it. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	FStateTreeDelegateDispatcher OnFinished;

	/** Output: number of ticks the task has run. */
	UPROPERTY(EditAnywhere, Category = "Output")
	int32 TickCount = 0;

	/** Context: bound automatically to the schema's owner. */
	UPROPERTY(EditAnywhere, Category = "Context")
	TObjectPtr<UDstTestOwner> Owner = nullptr;
};

/**
 * Logs "Enter", counts ticks and completes with CompletionStatus after TicksToComplete ticks, logs "Exit" when its
 * state is left. Broadcasts OnFinished on completion.
 */
USTRUCT(meta = (DisplayName = "Dst Test Task"))
struct FDstTestTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;

	/** State link on the node itself, as opposed to the one in the instance data. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	FStateTreeStateLink NodeLink;
};

USTRUCT()
struct FDstTestSendEventTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FGameplayTag Tag;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FDstTestPayload Payload;

	/** When false the event is sent without payload. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bSendPayload = true;
};

/** Sends an event to its own tree when its state is entered. Does not tick. */
USTRUCT(meta = (DisplayName = "Dst Test Send Event"))
struct FDstTestSendEventTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestSendEventTaskInstanceData;

	FDstTestSendEventTask()
	{
		bShouldCallTick = false;
	}

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};

USTRUCT()
struct FDstTestConditionInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Input")
	float Left = 0.f;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	float Right = 0.f;
};

/** Compares Left (input) with Right (parameter); bInvert negates the result. */
USTRUCT(meta = (DisplayName = "Dst Test Condition"))
struct FDstTestCondition : public FStateTreeConditionCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestConditionInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual bool TestCondition(FStateTreeExecutionContext& Context) const override;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	EDstTestCompare Compare = EDstTestCompare::Greater;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bInvert = false;
};

USTRUCT()
struct FDstTestEvaluatorInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Step = 1;

	/** Output: starts at 0 and grows by Step on every tick. */
	UPROPERTY(EditAnywhere, Category = "Output")
	int32 Counter = 0;
};

/** Counts: Counter grows by Step each tick. */
USTRUCT(meta = (DisplayName = "Dst Test Evaluator"))
struct FDstTestEvaluator : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestEvaluatorInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual void TreeStart(FStateTreeExecutionContext& Context) const override;
	virtual void TreeStop(FStateTreeExecutionContext& Context) const override;
	virtual void Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};

USTRUCT()
struct FDstTestConsiderationInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	float Score = 0.5f;
};

/** Scores a state with its Score parameter. */
USTRUCT(meta = (DisplayName = "Dst Test Consideration"))
struct FDstTestConsideration : public FStateTreeConsiderationCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestConsiderationInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

protected:
	virtual float GetScore(FStateTreeExecutionContext& Context) const override;
};

USTRUCT()
struct FDstTestForbiddenTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Value = 0;
};

/**
 * A task that derives from the engine's plain task base instead of FStateTreeTaskCommonBase. The fixture schema
 * refuses it (and so does the engine's component schema), which makes it the stand-in for "a node the schema does
 * not allow".
 */
USTRUCT(meta = (DisplayName = "Dst Test Forbidden Task"))
struct FDstTestForbiddenTask : public FStateTreeTaskBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestForbiddenTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};
