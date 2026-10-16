// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"

#include "DstTestBlueprintNodes.generated.h"

/**
 * A task written as a Blueprint-style node class, but in C++ so the tests need no .uasset. A tree stores such a task
 * as an FStateTreeBlueprintTaskWrapper whose instance data is a UObject (FStateTreeEditorNode::InstanceObject), a
 * different storage path from the struct-based nodes.
 */
UCLASS(HideDropdown, meta = (DisplayName = "Dst Test Blueprint Task"))
class UDstTestBlueprintTask : public UStateTreeTaskBlueprintBase
{
	GENERATED_BODY()

public:
	UDstTestBlueprintTask(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Strength = 3;

protected:
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) override;
	virtual void ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) override;
};
