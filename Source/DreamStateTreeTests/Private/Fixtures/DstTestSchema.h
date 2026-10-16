// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeSchema.h"
#include "StateTreeTypes.h"

#include "DstTestSchema.generated.h"

/**
 * Schema of the test trees. The context is one UDstTestOwner named "Owner", which the execution context must be
 * given before the tree starts. All nodes derived from the engine's CommonBase structs are allowed, plus the
 * Blueprint node base classes; anything else is refused (see FDstTestForbiddenTask).
 *
 * The DreamStateTree metadata is what the semantic layer will read to learn which names a text tree may use.
 */
UCLASS(HideDropdown, meta = (DreamStateTree = "TestAgent", DreamStateTreeNamespace = "TestAgent"))
class UDstTestSchema : public UStateTreeSchema
{
	GENERATED_BODY()

public:
	UDstTestSchema();

	/** ID of the context entry; bindings from the owner use it as their source struct ID. */
	static const FGuid& GetOwnerContextId();
	/** Name the execution context knows the owner by (FStateTreeExecutionContext::SetContextDataByName). */
	static FName GetOwnerContextName();

	//~ Begin UStateTreeSchema
	virtual bool IsStructAllowed(const UScriptStruct* InScriptStruct) const override;
	virtual bool IsClassAllowed(const UClass* InClass) const override;
	virtual bool IsExternalItemAllowed(const UStruct& InStruct) const override;
	virtual bool IsScheduledTickAllowed() const override { return true; }
	virtual TConstArrayView<FStateTreeExternalDataDesc> GetContextDataDescs() const override { return ContextDataDescs; }
	//~ End UStateTreeSchema

private:
	UPROPERTY()
	TArray<FStateTreeExternalDataDesc> ContextDataDescs;
};

/**
 * The same schema with every restriction the engine lets a schema express: no evaluators, no enter conditions,
 * no utility considerations, a single task per state, no global parameters, no task completion control, no Group
 * states and no Try Follow Transitions. Used to see what the editor does with a tree that breaks them.
 */
UCLASS(HideDropdown, meta = (DreamStateTree = "RestrictedAgent", DreamStateTreeNamespace = "TestAgent"))
class UDstTestRestrictedSchema : public UDstTestSchema
{
	GENERATED_BODY()

public:
	//~ Begin UStateTreeSchema
	virtual bool IsStateTypeAllowed(EStateTreeStateType InStateType) const override;
	virtual bool IsStateSelectionAllowed(EStateTreeStateSelectionBehavior InBehavior) const override;
#if WITH_EDITOR
	virtual bool AllowEnterConditions() const override { return false; }
	virtual bool AllowUtilityConsiderations() const override { return false; }
	virtual bool AllowEvaluators() const override { return false; }
	virtual bool AllowMultipleTasks() const override { return false; }
	virtual bool AllowGlobalParameters() const override { return false; }
	virtual bool AllowTasksCompletion() const override { return false; }
#endif
	//~ End UStateTreeSchema
};
