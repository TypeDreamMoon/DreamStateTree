// Copyright DreamMoon. All Rights Reserved.

#include "Fixtures/DstTestSchema.h"

#include "Fixtures/DstTestTypes.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeTaskBase.h"

namespace UE::DreamStateTree::Tests::Private
{
	// Fixed so that two trees built by the same code carry the same context ID.
	const FGuid OwnerContextId(0x44535400, 0x5EC0DE01, 0xB1D70A21, 0x00000001);
	const FName OwnerContextName(TEXT("Owner"));
}

UDstTestSchema::UDstTestSchema()
{
	ContextDataDescs.Add(FStateTreeExternalDataDesc(
		UE::DreamStateTree::Tests::Private::OwnerContextName,
		UDstTestOwner::StaticClass(),
		UE::DreamStateTree::Tests::Private::OwnerContextId));
}

const FGuid& UDstTestSchema::GetOwnerContextId()
{
	return UE::DreamStateTree::Tests::Private::OwnerContextId;
}

FName UDstTestSchema::GetOwnerContextName()
{
	return UE::DreamStateTree::Tests::Private::OwnerContextName;
}

bool UDstTestSchema::IsStructAllowed(const UScriptStruct* InScriptStruct) const
{
	return InScriptStruct->IsChildOf(FStateTreeConditionCommonBase::StaticStruct())
		|| InScriptStruct->IsChildOf(FStateTreeEvaluatorCommonBase::StaticStruct())
		|| InScriptStruct->IsChildOf(FStateTreeTaskCommonBase::StaticStruct())
		|| InScriptStruct->IsChildOf(FStateTreeConsiderationCommonBase::StaticStruct())
		|| InScriptStruct->IsChildOf(FStateTreePropertyFunctionCommonBase::StaticStruct());
}

bool UDstTestSchema::IsClassAllowed(const UClass* InClass) const
{
	return IsChildOfBlueprintBase(InClass);
}

bool UDstTestSchema::IsExternalItemAllowed(const UStruct& InStruct) const
{
	return InStruct.IsChildOf(UDstTestOwner::StaticClass());
}

bool UDstTestRestrictedSchema::IsStateTypeAllowed(EStateTreeStateType InStateType) const
{
	return InStateType != EStateTreeStateType::Group;
}

bool UDstTestRestrictedSchema::IsStateSelectionAllowed(EStateTreeStateSelectionBehavior InBehavior) const
{
	return InBehavior != EStateTreeStateSelectionBehavior::TryFollowTransitions;
}
