// Copyright DreamMoon. All Rights Reserved.

#include "DstGenerateInternal.h"

#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Misc/StringOutputDevice.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyBindings.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::GeneratePrivate
{
	const TCHAR* const DefaultColorName = TEXT("Default Color");

	//------------------------------------------------------------------------------------------------------------------//
	// FGenContext
	//------------------------------------------------------------------------------------------------------------------//

	FGuid FGenContext::Assign(const FString& IdKey, const FString& HashKey, const TCHAR* Salt, const FString& PlaceKey)
	{
		const FGuid Id = MakeDstGuid(Tree.AssetPath, HashKey, Salt);
		Result.Ids.Add(IdKey, Id);
		IdToPlace.Add(Id, PlaceKey);
		return Id;
	}

	void FGenContext::MapId(const FGuid& Id, const FString& PlaceKey)
	{
		if (Id.IsValid() && !IdToPlace.Contains(Id))
		{
			IdToPlace.Add(Id, PlaceKey);
		}
	}

	FString FGenContext::FindPlaceKey(const FGuid& Id) const
	{
		const FString* PlaceKey = Id.IsValid() ? IdToPlace.Find(Id) : nullptr;
		return PlaceKey ? *PlaceKey : FString();
	}

	Lang::FDstSpan FGenContext::SpanFor(const FString& PlaceKey) const
	{
		FString Key = PlaceKey;
		for (;;)
		{
			if (const Lang::FDstSpan* Span = Places.Spans.Find(Key))
			{
				return *Span;
			}
			if (const FString* Alt = AltPlaceKeys.Find(Key))
			{
				if (const Lang::FDstSpan* AltSpan = Places.Spans.Find(*Alt))
				{
					return *AltSpan;
				}
			}
			// A property function is written inside the argument of the node it feeds; one that feeds a linked state's
			// parameters has `<state path>/Parameters` for its owner, and the state is where that is written.
			const int32 Cut = Key.Find(TEXT("/Fn:"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Cut != INDEX_NONE)
			{
				Key.LeftInline(Cut);
			}
			else if (Key.EndsWith(TEXT("/Parameters"), ESearchCase::CaseSensitive))
			{
				Key.LeftInline(Key.Len() - 11);
			}
			else
			{
				break;
			}
		}
		return Places.Find(Key);
	}

	void FGenContext::Dst7003(const FString& PlaceKey, const FString& Message, const FString& Hint)
	{
		++NumErrors;
		Sink.Error(TEXT("DST7003"), SpanFor(PlaceKey), Message, Hint);
	}

	void FGenContext::Dst7004(const FString& PlaceKey, const FString& Message, const FString& Hint)
	{
		Sink.Warning(TEXT("DST7004"), SpanFor(PlaceKey), Message, Hint);
	}

	//------------------------------------------------------------------------------------------------------------------//
	// Texts to engine things
	//------------------------------------------------------------------------------------------------------------------//

	namespace
	{
		/** The object at a path, loading the asset when it is not in memory. */
		UObject* FindOrLoadObject(const FString& Path)
		{
			if (Path.IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Found = FindObject<UObject>(nullptr, *Path))
			{
				return Found;
			}
			return LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
	}

	const UScriptStruct* FindStructByPath(const FString& Path)
	{
		return Cast<UScriptStruct>(FindOrLoadObject(Path));
	}

	UClass* FindClassByPath(const FString& Path)
	{
		if (UClass* Class = Cast<UClass>(FindOrLoadObject(Path)))
		{
			return Class;
		}
		// A Blueprint asset path names the Blueprint; its class carries a _C suffix.
		if (!Path.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
		{
			return Cast<UClass>(FindOrLoadObject(Path + TEXT("_C")));
		}
		return nullptr;
	}

	UEnum* FindEnumByPath(const FString& Path)
	{
		return Cast<UEnum>(FindOrLoadObject(Path));
	}

	bool ParseParameterType(const FString& Type, FParsedParameterType& Out, FString& OutError)
	{
		Out = FParsedParameterType();

		FString Rest = Type.TrimStartAndEnd();
		for (;;)
		{
			EPropertyBagContainerType Container = EPropertyBagContainerType::None;
			int32 PrefixLength = 0;
			if (Rest.StartsWith(TEXT("Array<"), ESearchCase::CaseSensitive))
			{
				Container = EPropertyBagContainerType::Array;
				PrefixLength = 6;
			}
			else if (Rest.StartsWith(TEXT("Set<"), ESearchCase::CaseSensitive))
			{
				Container = EPropertyBagContainerType::Set;
				PrefixLength = 4;
			}
			else
			{
				break;
			}

			if (!Rest.EndsWith(TEXT(">"), ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(TEXT("The parameter type `%s` has an unclosed `<`."), *Type);
				return false;
			}
			if (!Out.Containers.CanAdd())
			{
				OutError = FString::Printf(TEXT("The parameter type `%s` nests containers deeper than the engine's property bag allows (two)."), *Type);
				return false;
			}
			Out.Containers.Add(Container);
			Rest = Rest.Mid(PrefixLength, Rest.Len() - PrefixLength - 1).TrimStartAndEnd();
		}

		struct FSimpleType
		{
			const TCHAR* Name;
			EPropertyBagPropertyType Type;
		};
		static const FSimpleType SimpleTypes[] = {
			{ TEXT("bool"), EPropertyBagPropertyType::Bool },
			{ TEXT("uint8"), EPropertyBagPropertyType::Byte },
			{ TEXT("int8"), EPropertyBagPropertyType::Int8 },
			{ TEXT("int16"), EPropertyBagPropertyType::Int16 },
			{ TEXT("uint16"), EPropertyBagPropertyType::UInt16 },
			{ TEXT("int32"), EPropertyBagPropertyType::Int32 },
			{ TEXT("uint32"), EPropertyBagPropertyType::UInt32 },
			{ TEXT("int64"), EPropertyBagPropertyType::Int64 },
			{ TEXT("uint64"), EPropertyBagPropertyType::UInt64 },
			{ TEXT("float"), EPropertyBagPropertyType::Float },
			{ TEXT("double"), EPropertyBagPropertyType::Double },
			{ TEXT("Name"), EPropertyBagPropertyType::Name },
			{ TEXT("String"), EPropertyBagPropertyType::String },
			{ TEXT("Text"), EPropertyBagPropertyType::Text },
		};
		for (const FSimpleType& Simple : SimpleTypes)
		{
			if (Rest.Equals(Simple.Name, ESearchCase::CaseSensitive))
			{
				Out.ValueType = Simple.Type;
				return true;
			}
		}

		int32 Colon = INDEX_NONE;
		if (!Rest.FindChar(TEXT(':'), Colon))
		{
			OutError = FString::Printf(TEXT("The parameter type `%s` is not one the engine's property bag has."), *Type);
			return false;
		}
		const FString Kind = Rest.Left(Colon).TrimStartAndEnd();
		const FString Path = Rest.Mid(Colon + 1).TrimStartAndEnd();

		if (Kind.Equals(TEXT("Enum"), ESearchCase::CaseSensitive))
		{
			Out.ValueType = EPropertyBagPropertyType::Enum;
			Out.TypeObject = FindEnumByPath(Path);
		}
		else if (Kind.Equals(TEXT("Struct"), ESearchCase::CaseSensitive))
		{
			Out.ValueType = EPropertyBagPropertyType::Struct;
			Out.TypeObject = FindStructByPath(Path);
		}
		else if (Kind.Equals(TEXT("Object"), ESearchCase::CaseSensitive))
		{
			Out.ValueType = EPropertyBagPropertyType::Object;
			Out.TypeObject = FindClassByPath(Path);
		}
		else if (Kind.Equals(TEXT("SoftObject"), ESearchCase::CaseSensitive))
		{
			Out.ValueType = EPropertyBagPropertyType::SoftObject;
			Out.TypeObject = FindClassByPath(Path);
		}
		else if (Kind.Equals(TEXT("Class"), ESearchCase::CaseSensitive))
		{
			Out.ValueType = EPropertyBagPropertyType::Class;
			Out.TypeObject = FindClassByPath(Path);
		}
		else if (Kind.Equals(TEXT("SoftClass"), ESearchCase::CaseSensitive))
		{
			Out.ValueType = EPropertyBagPropertyType::SoftClass;
			Out.TypeObject = FindClassByPath(Path);
		}
		else
		{
			OutError = FString::Printf(TEXT("The parameter type `%s` is not one the engine's property bag has."), *Type);
			return false;
		}

		if (!Out.TypeObject)
		{
			OutError = FString::Printf(TEXT("The parameter type `%s` names `%s`, which does not exist."), *Type, *Path);
			return false;
		}
		return true;
	}

	bool ImportPropertyText(const FProperty& Property, void* Container, UObject* Owner, const FString& Text, FString& OutError)
	{
		FStringOutputDevice Errors;
		if (Property.ImportText_InContainer(*Text, Container, Owner, PPF_None, &Errors) != nullptr)
		{
			return true;
		}
		Errors.TrimStartAndEndInline();
		OutError = Errors.IsEmpty()
			? FString::Printf(TEXT("The engine does not read `%s` as a value of `%s`."), *Text, *Property.GetName())
			: FString::Printf(TEXT("The engine does not read `%s` as a value of `%s`: %s"), *Text, *Property.GetName(), *Errors);
		return false;
	}

	bool ResolveConstantTarget(const UStruct* Struct, void* Memory, const Model::FDstPropertyPath& Path, const FProperty*& OutProperty,
		void*& OutContainer, FString& OutError)
	{
		OutProperty = nullptr;
		OutContainer = nullptr;
		if (Path.Segments.IsEmpty())
		{
			OutError = TEXT("A constant has no property to be set on.");
			return false;
		}

		const UStruct* CurrentStruct = Struct;
		void* CurrentMemory = Memory;
		for (int32 Index = 0; Index < Path.Segments.Num(); ++Index)
		{
			const Model::FDstPathSegment& Segment = Path.Segments[Index];
			const FProperty* Property = CurrentStruct->FindPropertyByName(FName(*Segment.Name));
			if (!Property)
			{
				OutError = FString::Printf(TEXT("`%s` has no property `%s`."), *CurrentStruct->GetName(), *Segment.Name);
				return false;
			}
			if (Index == Path.Segments.Num() - 1)
			{
				OutProperty = Property;
				OutContainer = CurrentMemory;
				return true;
			}

			// Below a struct property the property is a member of that struct's value.
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty || Segment.ArrayIndex != INDEX_NONE || !Segment.InstanceType.IsEmpty())
			{
				OutError = FString::Printf(TEXT("A constant cannot be set below `%s`; it is not a plain struct property."), *Segment.Name);
				return false;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentMemory = StructProperty->ContainerPtrToValuePtr<void>(CurrentMemory);
		}
		return false;
	}

	FPropertyBindingPath MakeBindingPath(const FGuid& StructId, const Model::FDstPropertyPath& Path, const UStruct* RootStruct, const void* RootMemory)
	{
		FPropertyBindingPath Result(StructId);
		const UStruct* CurrentStruct = RootStruct;
		const void* CurrentMemory = RootMemory;
		for (const Model::FDstPathSegment& Segment : Path.Segments)
		{
			const FProperty* Property = CurrentStruct ? CurrentStruct->FindPropertyByName(FName(*Segment.Name)) : nullptr;
			const UStruct* InstanceType = nullptr;
			EPropertyBindingPropertyAccessType Access = EPropertyBindingPropertyAccessType::Unset;
			if (Segment.InstanceType.Equals(Model::PropertyBagInstanceType, ESearchCase::CaseSensitive))
			{
				// The bag of a reference to a tree: whatever struct the reference holds on the way (the engine made it from the
				// tree's parameters). Left unset when the data on the way does not hold one.
				const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (StructProperty && StructProperty->Struct == FInstancedStruct::StaticStruct() && CurrentMemory)
				{
					InstanceType = StructProperty->ContainerPtrToValuePtr<FInstancedStruct>(CurrentMemory)->GetScriptStruct();
					Access = EPropertyBindingPropertyAccessType::StructInstance;
				}
			}
			else if (!Segment.InstanceType.IsEmpty())
			{
				const UObject* Found = FindOrLoadObject(Segment.InstanceType);
				if (const UClass* Class = Cast<UClass>(Found))
				{
					InstanceType = Class;
					Access = EPropertyBindingPropertyAccessType::ObjectInstance;
				}
				else if (const UScriptStruct* Struct = Cast<UScriptStruct>(Found))
				{
					InstanceType = Struct;
					Access = EPropertyBindingPropertyAccessType::StructInstance;
				}
			}
			Result.AddPathSegment(FName(*Segment.Name), Segment.ArrayIndex, InstanceType, Access);

			// On into the struct this segment names, while the data on the way is known.
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (StructProperty && StructProperty->Struct && Segment.ArrayIndex == INDEX_NONE && InstanceType == nullptr)
			{
				CurrentStruct = StructProperty->Struct;
				CurrentMemory = CurrentMemory ? StructProperty->ContainerPtrToValuePtr<void>(CurrentMemory) : nullptr;
			}
			else
			{
				CurrentStruct = nullptr;
				CurrentMemory = nullptr;
			}
		}
		return Result;
	}

	FPropertyBindingPath MakeBindingPath(const FGuid& StructId, const Model::FDstPropertyPath& Path)
	{
		return MakeBindingPath(StructId, Path, nullptr, nullptr);
	}

	bool ResolveNodeType(const Model::FDstNode& Node, FResolvedNodeType& Out, FString& OutError)
	{
		Out = FResolvedNodeType();
		Out.Struct = FindStructByPath(Node.StructType);
		if (!Out.Struct)
		{
			OutError = FString::Printf(TEXT("The node type `%s` does not exist in the engine."), *Node.StructType);
			return false;
		}
		if (!Out.Struct->IsChildOf(FStateTreeNodeBase::StaticStruct()))
		{
			OutError = FString::Printf(TEXT("`%s` is not a StateTree node."), *Node.StructType);
			return false;
		}
		if (!Node.BlueprintClass.IsEmpty())
		{
			Out.BlueprintClass = FindClassByPath(Node.BlueprintClass);
			if (!Out.BlueprintClass)
			{
				OutError = FString::Printf(TEXT("The Blueprint node class `%s` does not exist."), *Node.BlueprintClass);
				return false;
			}
			if (!Out.BlueprintClass->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass())
				&& !Out.BlueprintClass->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass())
				&& !Out.BlueprintClass->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass())
				&& !Out.BlueprintClass->IsChildOf(UStateTreeConsiderationBlueprintBase::StaticClass()))
			{
				OutError = FString::Printf(TEXT("`%s` is not a StateTree Blueprint task, evaluator, condition or consideration."), *Node.BlueprintClass);
				return false;
			}
		}
		return true;
	}

	const FProperty* FindFunctionOutputProperty(const UStruct& InstanceStruct)
	{
		static const TCHAR* const OutputNames[] = { TEXT("Result"), TEXT("bResult"), TEXT("Output") };
		for (const TCHAR* Name : OutputNames)
		{
			if (const FProperty* Property = InstanceStruct.FindPropertyByName(Name))
			{
				return Property;
			}
		}
		return UE::StateTree::GetStructSingleOutputProperty(InstanceStruct);
	}

	FString DescribeFirstDifference(const FString& Before, const FString& After)
	{
		TArray<FString> BeforeLines;
		TArray<FString> AfterLines;
		Before.ParseIntoArrayLines(BeforeLines, /*bCullEmpty*/ false);
		After.ParseIntoArrayLines(AfterLines, /*bCullEmpty*/ false);

		const int32 Num = FMath::Max(BeforeLines.Num(), AfterLines.Num());
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const FString BeforeLine = BeforeLines.IsValidIndex(Index) ? BeforeLines[Index].TrimStartAndEnd() : FString(TEXT("<end>"));
			const FString AfterLine = AfterLines.IsValidIndex(Index) ? AfterLines[Index].TrimStartAndEnd() : FString(TEXT("<end>"));
			if (BeforeLine != AfterLine)
			{
				return FString::Printf(TEXT("line %d: before `%s`, after `%s`"), Index + 1, *BeforeLine, *AfterLine);
			}
		}
		return FString();
	}
}
