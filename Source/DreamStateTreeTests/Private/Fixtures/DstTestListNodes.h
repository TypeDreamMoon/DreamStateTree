// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "StateTreeTypes.h"

#include "DstTestListNodes.generated.h"

// A task whose properties are the shapes a binding can reach into: a list of structs, a list of numbers, and a struct
// whose own default is not the type's default. Their parts can be fed from elsewhere one by one
// (`Waypoints[1].Weight`, `Weights[2]`, `Range.Min`); the task logs what it ended up with, so a run shows whether the
// copies landed.

/** One entry of the list the task carries. The defaults are not the zero values on purpose. */
USTRUCT()
struct FDstTestWaypoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Default")
	FName Label;

	UPROPERTY(EditAnywhere, Category = "Default")
	float Weight = 1.f;

	UPROPERTY(EditAnywhere, Category = "Default")
	int32 Count = 1;

	UPROPERTY(EditAnywhere, Category = "Default")
	bool bActive = true;

	/** A struct inside the entry, so a path can go two levels below the list element. */
	UPROPERTY(EditAnywhere, Category = "Default")
	FVector Offset = FVector::ZeroVector;

	/** A colour starts opaque in the text, which a field the text leaves out does not share. */
	UPROPERTY(EditAnywhere, Category = "Default")
	FLinearColor Tint = FLinearColor::White;

	/** An array inside the entry: a path can pick an element of the list and then an element of this one. */
	UPROPERTY(EditAnywhere, Category = "Default")
	TArray<float> Stops;
};

/** A range that starts at 1..2, not at the zeros a tuple that names no field would give. */
USTRUCT()
struct FDstTestRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Default")
	float Min = 1.f;

	UPROPERTY(EditAnywhere, Category = "Default")
	float Max = 2.f;
};

USTRUCT()
struct FDstTestListTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	TArray<FDstTestWaypoint> Waypoints;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	TArray<float> Weights;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FDstTestRange Range;

	/** A set: its elements have no index a path could pick. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	TSet<int32> Marks;

	/** Output: the weights and the entries' weights times their counts, added up. */
	UPROPERTY(EditAnywhere, Category = "Output")
	float Total = 0.f;
};

/** Logs the list it holds when it is entered and succeeds on its first tick. */
USTRUCT(meta = (DisplayName = "Dst Test List"))
struct FDstTestListTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FDstTestListTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};
