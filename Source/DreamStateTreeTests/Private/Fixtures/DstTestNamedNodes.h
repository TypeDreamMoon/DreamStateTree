// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "StateTreeTypes.h"

#include "DstTestNamedNodes.generated.h"

// Task nodes that carry DreamStateTree metadata, so the name registry has something to resolve beyond the engine's
// own nodes:
//   - a namespaced name under the test schema's namespace (`Move` means `TestAgent.Move`), positional arguments, an
//     alias inside the namespace (`Run`) and a global alias (`Sprint`);
//   - two nodes with one name (`Twin`), which the text cannot tell apart;
//   - a namespaced name that hides a global one (`Echo`).
// They share one instance data struct; only Move does anything when it runs.

/** Instance data of the named test tasks. */
USTRUCT()
struct FDstTestNamedTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	float Speed = 1.f;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bSprint = false;

	/** Output: how far the task has moved. */
	UPROPERTY(EditAnywhere, Category = "Output")
	float Travelled = 0.f;
};

/** Logs when it is entered, moves once and succeeds on its first tick. */
USTRUCT(meta = (DisplayName = "Dst Test Move", DreamStateTree = "TestAgent.Move", DreamStateTreeArgs = "Speed,bSprint", DreamStateTreeAliases = "TestAgent.Run;Sprint"))
struct FDstTestMoveTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestNamedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};

/** One of two nodes that answer to `Twin`. */
USTRUCT(meta = (DisplayName = "Dst Test Twin A", DreamStateTree = "Twin"))
struct FDstTestTwinATask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestNamedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

/** The other one of two nodes that answer to `Twin`. */
USTRUCT(meta = (DisplayName = "Dst Test Twin B", DreamStateTree = "Twin"))
struct FDstTestTwinBTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestNamedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

/** `Echo` under the test schema: found first, and it hides the global `Echo`. */
USTRUCT(meta = (DisplayName = "Dst Test Echo", DreamStateTree = "TestAgent.Echo"))
struct FDstTestEchoTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestNamedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

/** The global `Echo` that the namespaced one hides. */
USTRUCT(meta = (DisplayName = "Dst Test Echo Global", DreamStateTree = "Echo"))
struct FDstTestEchoGlobalTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestNamedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};
