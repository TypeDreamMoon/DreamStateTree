// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"

// Gameplay tags the fixtures use as event tags and state tags. They are added to the tag manager the first time a
// test asks for one. Not UE_DEFINE_GAMEPLAY_TAG: native tags must live in a Runtime module (the engine ensures on
// anything else), and this module is UncookedOnly.
namespace UE::DreamStateTree::Tests
{
	class FDstTestTag
	{
	public:
		explicit FDstTestTag(const TCHAR* InName) : Name(InName) {}

		/** The tag, added to the tag manager on first use. */
		FGameplayTag GetTag() const;

	private:
		FName Name;
		mutable FGameplayTag Tag;
	};

	/** Parent of the hit events. A state or transition that requires this tag also accepts Hit.Light and Hit.Heavy. */
	extern const FDstTestTag Tag_Event_Hit;
	extern const FDstTestTag Tag_Event_Hit_Light;
	extern const FDstTestTag Tag_Event_Hit_Heavy;
	/** An event without payload. */
	extern const FDstTestTag Tag_Event_Ask;

	/** Tags a state can carry (FCompactStateTreeState::Tag). */
	extern const FDstTestTag Tag_State_Combat;
	extern const FDstTestTag Tag_State_Explore;
}
