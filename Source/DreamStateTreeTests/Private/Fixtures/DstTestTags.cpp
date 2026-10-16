// Copyright DreamMoon. All Rights Reserved.

#include "Fixtures/DstTestTags.h"

#include "GameplayTagsManager.h"

namespace UE::DreamStateTree::Tests
{
	FGameplayTag FDstTestTag::GetTag() const
	{
		if (!Tag.IsValid())
		{
			Tag = UGameplayTagsManager::Get().AddNativeGameplayTag(Name, TEXT("DreamStateTree test fixture"));
		}
		return Tag;
	}

	const FDstTestTag Tag_Event_Hit(TEXT("DreamStateTreeTest.Event.Hit"));
	const FDstTestTag Tag_Event_Hit_Light(TEXT("DreamStateTreeTest.Event.Hit.Light"));
	const FDstTestTag Tag_Event_Hit_Heavy(TEXT("DreamStateTreeTest.Event.Hit.Heavy"));
	const FDstTestTag Tag_Event_Ask(TEXT("DreamStateTreeTest.Event.Ask"));

	const FDstTestTag Tag_State_Combat(TEXT("DreamStateTreeTest.State.Combat"));
	const FDstTestTag Tag_State_Explore(TEXT("DreamStateTreeTest.State.Explore"));
}
