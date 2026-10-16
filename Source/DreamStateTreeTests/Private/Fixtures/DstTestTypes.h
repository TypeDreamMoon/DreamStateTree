// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "DstTestTypes.generated.h"

// Data types shared by the fixture schema and the fixture nodes. The reflected types live at global scope because
// the test module does not enable UObject types in namespaces.

/** Enum the fixtures expose as a root parameter, a task parameter and a property of the context object. */
UENUM()
enum class EDstTestMode : uint8
{
	Idle,
	Patrol,
	Combat,
};

/** How a DstTest condition compares its two values. */
UENUM()
enum class EDstTestCompare : uint8
{
	Greater,
	GreaterOrEqual,
	Less,
	Equal,
};

/** Event payload used by the fixtures. A few fields of different types so bindings can pick a leaf by name. */
USTRUCT()
struct FDstTestPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Default")
	int32 Amount = 0;

	UPROPERTY(EditAnywhere, Category = "Default")
	float Severity = 0.f;

	UPROPERTY(EditAnywhere, Category = "Default")
	FName Source;

	UPROPERTY(EditAnywhere, Category = "Default")
	bool bCritical = false;
};

/**
 * The object the fixture schema declares as its context ("Owner"). It carries one value of each kind a binding
 * test needs: an enum, a float, an int and a bool.
 */
UCLASS(HideDropdown)
class UDstTestOwner : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Default")
	EDstTestMode Mode = EDstTestMode::Patrol;

	UPROPERTY(EditAnywhere, Category = "Default")
	float Health = 100.f;

	UPROPERTY(EditAnywhere, Category = "Default")
	int32 Level = 1;

	UPROPERTY(EditAnywhere, Category = "Default")
	bool bAlerted = false;
};
