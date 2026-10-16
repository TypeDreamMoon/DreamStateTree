// Copyright DreamMoon. All Rights Reserved.
//
// The semantic stage's dealings with the engine's types: what kind of value a property holds, what a type written in a
// parameter list means, the property bag that gives a parameter list real properties, and the node type information the
// argument checks read.

#include "DstLowerPrivate.h"

#include "Algo/LevenshteinDistance.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "StateTreeAnyEnum.h"
#include "StateTreeDelegate.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreePropertyRefHelpers.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	// ------------------------------------------------------------------------------------------
	// Diagnostics
	// ------------------------------------------------------------------------------------------

	bool FLowerSink::Error(const TCHAR* Code, const Lang::FDstSpan& Span, FString Message, FString Hint)
	{
		if (CurrentFile.IsEmpty())
		{
			return Real.Error(Code, Span, MoveTemp(Message), MoveTemp(Hint));
		}
		return Real.ErrorIn(Code, CurrentFile, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	void FLowerSink::Warning(const TCHAR* Code, const Lang::FDstSpan& Span, FString Message, FString Hint)
	{
		if (CurrentFile.IsEmpty())
		{
			Real.Warning(Code, Span, MoveTemp(Message), MoveTemp(Hint));
			return;
		}
		Real.WarningIn(Code, CurrentFile, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	// ------------------------------------------------------------------------------------------
	// Property classification
	// ------------------------------------------------------------------------------------------

	bool IsAnyEnumProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && StructProperty->Struct == FStateTreeAnyEnum::StaticStruct();
	}

	bool IsGameplayTagProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && (StructProperty->Struct == TBaseStructure<FGameplayTag>::Get() || StructProperty->Struct == FGameplayTagContainer::StaticStruct());
	}

	bool IsDelegateDispatcherProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && StructProperty->Struct == FStateTreeDelegateDispatcher::StaticStruct();
	}

	bool IsDelegateListenerProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && StructProperty->Struct == FStateTreeDelegateListener::StaticStruct();
	}

	bool IsPropertyRefProperty(const FProperty& Property)
	{
		return UE::StateTree::PropertyRefHelpers::IsPropertyRef(Property);
	}

	bool IsOptionalProperty(const FProperty& Property)
	{
		return Property.HasMetaData(TEXT("Optional"));
	}

	const UEnum* GetPropertyEnum(const FProperty* Property)
	{
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->GetIntPropertyEnum();
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		return nullptr;
	}

	EValueDomain GetValueDomain(const FProperty* Property)
	{
		if (!Property)
		{
			return EValueDomain::Other;
		}
		if (IsAnyEnumProperty(Property))
		{
			return EValueDomain::Enum;
		}
		if (Property->IsA<FBoolProperty>())
		{
			return EValueDomain::Bool;
		}
		if (GetPropertyEnum(Property))
		{
			return EValueDomain::Enum;
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			return Numeric->IsFloatingPoint() ? EValueDomain::Float : EValueDomain::Int;
		}
		if (Property->IsA<FNameProperty>())
		{
			return EValueDomain::Name;
		}
		if (Property->IsA<FStrProperty>() || Property->IsA<FTextProperty>())
		{
			return EValueDomain::String;
		}
		if (Property->IsA<FObjectPropertyBase>())
		{
			return EValueDomain::Object;
		}
		if (Property->IsA<FStructProperty>())
		{
			return EValueDomain::Struct;
		}
		return EValueDomain::Other;
	}

	FString DescribeProperty(const FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("<none>");
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return StructProperty->Struct ? StructProperty->Struct->GetName() : FString(TEXT("struct"));
		}
		if (const UEnum* Enum = GetPropertyEnum(Property))
		{
			return Enum->GetName();
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			return ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetName() : FString(TEXT("object"));
		}
		return Property->GetCPPType();
	}

	EPropertyUse GetPropertyUse(const FProperty& Property)
	{
		switch (UE::StateTree::GetUsageFromMetaData(&Property))
		{
		case EStateTreePropertyUsage::Input:
			return EPropertyUse::Input;
		case EStateTreePropertyUsage::Output:
			return EPropertyUse::Output;
		case EStateTreePropertyUsage::Context:
			return EPropertyUse::Context;
		default:
			return EPropertyUse::Parameter;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Suggestions
	// ------------------------------------------------------------------------------------------

	TArray<FString> FindSimilarNames(const FString& Wanted, const TArray<FString>& Candidates, const int32 Max)
	{
		struct FScored
		{
			FString Name;
			int32 Distance = 0;
		};
		TArray<FScored> Scored;
		const FString WantedLower = Wanted.ToLower();
		for (const FString& Candidate : Candidates)
		{
			const FString Lower = Candidate.ToLower();
			int32 Distance = Algo::LevenshteinDistance(WantedLower, Lower);
			if (Lower.Contains(WantedLower) || WantedLower.Contains(Lower))
			{
				Distance = FMath::Min(Distance, 1);
			}
			// Far-off names are noise: allow about a third of the letters to differ.
			if (Distance <= FMath::Max(2, WantedLower.Len() / 3))
			{
				Scored.Add({ Candidate, Distance });
			}
		}
		Scored.StableSort([](const FScored& A, const FScored& B) { return A.Distance < B.Distance; });

		TArray<FString> Result;
		for (const FScored& Item : Scored)
		{
			if (Result.Num() >= Max)
			{
				break;
			}
			Result.AddUnique(Item.Name);
		}
		return Result;
	}

	FString MakeSuggestionHint(const FString& Wanted, const TArray<FString>& Candidates)
	{
		const TArray<FString> Similar = FindSimilarNames(Wanted, Candidates);
		if (Similar.IsEmpty())
		{
			return FString();
		}
		TArray<FString> Quoted;
		for (const FString& Name : Similar)
		{
			Quoted.Add(FString::Printf(TEXT("`%s`"), *Name));
		}
		return FString::Printf(TEXT("Did you mean %s?"), *FString::Join(Quoted, TEXT(", ")));
	}

	// ------------------------------------------------------------------------------------------
	// Names -> engine types
	// ------------------------------------------------------------------------------------------

	namespace
	{
		IAssetRegistry& GetAssetRegistry()
		{
			return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		}

		/** The assets of a class (and its subclasses) whose name is Name, ignoring case. */
		TArray<FAssetData> FindAssetsNamed(const FTopLevelAssetPath& ClassPath, const FString& Name, const bool bSubclasses)
		{
			TArray<FAssetData> All;
			GetAssetRegistry().GetAssetsByClass(ClassPath, All, bSubclasses);
			TArray<FAssetData> Result;
			for (const FAssetData& Asset : All)
			{
				if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					Result.Add(Asset);
				}
			}
			return Result;
		}

		bool StartsWithPrefixLetter(const FString& Name, const TCHAR Letter)
		{
			return Name.Len() > 1 && Name[0] == Letter && FChar::IsUpper(Name[1]);
		}
	}

	const UScriptStruct* ResolveStructByName(const FString& Written)
	{
		const FString Name = Written.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (Name.StartsWith(TEXT("/")))
		{
			if (const UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *Name))
			{
				return Found;
			}
			return LoadObject<UScriptStruct>(nullptr, *Name, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		TArray<FString> Variants;
		Variants.Add(Name);
		if (StartsWithPrefixLetter(Name, TEXT('F')))
		{
			Variants.Add(Name.Mid(1));
		}
		for (const FString& Variant : Variants)
		{
			if (const UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Variant, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging))
			{
				return Found;
			}
		}
		for (const FString& Variant : Variants)
		{
			const TArray<FAssetData> Assets = FindAssetsNamed(UUserDefinedStruct::StaticClass()->GetClassPathName(), Variant, false);
			if (Assets.Num() == 1)
			{
				return Cast<UScriptStruct>(Assets[0].GetAsset());
			}
		}
		return nullptr;
	}

	const UEnum* ResolveEnumByName(const FString& Written)
	{
		const FString Name = Written.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (Name.StartsWith(TEXT("/")))
		{
			if (const UEnum* Found = FindObject<UEnum>(nullptr, *Name))
			{
				return Found;
			}
			return LoadObject<UEnum>(nullptr, *Name, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		TArray<FString> Variants;
		Variants.Add(Name);
		if (!StartsWithPrefixLetter(Name, TEXT('E')))
		{
			Variants.Add(TEXT("E") + Name);
		}
		for (const FString& Variant : Variants)
		{
			if (const UEnum* Found = FindFirstObject<UEnum>(*Variant, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging))
			{
				return Found;
			}
		}
		for (const FString& Variant : Variants)
		{
			const TArray<FAssetData> Assets = FindAssetsNamed(UUserDefinedEnum::StaticClass()->GetClassPathName(), Variant, false);
			if (Assets.Num() == 1)
			{
				return Cast<UEnum>(Assets[0].GetAsset());
			}
		}
		return nullptr;
	}

	UClass* ResolveClassByName(const FString& Written)
	{
		const FString Name = Written.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (Name.StartsWith(TEXT("/")))
		{
			if (UClass* Found = FindObject<UClass>(nullptr, *Name))
			{
				return Found;
			}
			return LoadObject<UClass>(nullptr, *Name, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		TArray<FString> Variants;
		Variants.Add(Name);
		if (StartsWithPrefixLetter(Name, TEXT('A')) || StartsWithPrefixLetter(Name, TEXT('U')))
		{
			Variants.Add(Name.Mid(1));
		}
		for (const FString& Variant : Variants)
		{
			if (UClass* Found = FindFirstObject<UClass>(*Variant, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging))
			{
				return Found;
			}
		}
		const TArray<FAssetData> Assets = FindAssetsNamed(UBlueprint::StaticClass()->GetClassPathName(), Name, true);
		if (Assets.Num() == 1)
		{
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Assets[0].GetAsset()))
			{
				return Blueprint->GeneratedClass;
			}
		}
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Parameter types
	// ------------------------------------------------------------------------------------------

	namespace
	{
		/** Removes the blanks outside names: `Array < float >` -> `Array<float>`. */
		FString CompactTypeText(const FString& Text)
		{
			FString Result;
			Result.Reserve(Text.Len());
			for (const TCHAR Char : Text)
			{
				if (!FChar::IsWhitespace(Char))
				{
					Result.AppendChar(Char);
				}
			}
			return Result;
		}

		bool ParseTypeRecursive(const FString& Compact, FString& Out, FString& Error, const int32 Depth)
		{
			if (Compact.IsEmpty())
			{
				Error = TEXT("The type is empty.");
				return false;
			}
			if (Depth > 4)
			{
				Error = TEXT("The type is nested too deeply.");
				return false;
			}

			int32 Open = INDEX_NONE;
			if (Compact.FindChar(TEXT('<'), Open) && Compact.EndsWith(TEXT(">")))
			{
				const FString Head = Compact.Left(Open);
				const FString Inner = Compact.Mid(Open + 1, Compact.Len() - Open - 2);
				if (Head.Equals(TEXT("Array"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("TArray"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("Set"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("TSet"), ESearchCase::CaseSensitive))
				{
					FString InnerCanonical;
					if (!ParseTypeRecursive(Inner, InnerCanonical, Error, Depth + 1))
					{
						return false;
					}
					const bool bArray = Head.EndsWith(TEXT("Array"));
					Out = FString::Printf(TEXT("%s<%s>"), bArray ? TEXT("Array") : TEXT("Set"), *InnerCanonical);
					return true;
				}

				const bool bClass = Head.Equals(TEXT("Class"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("SubclassOf"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("TSubclassOf"), ESearchCase::CaseSensitive);
				const bool bSoftClass = Head.Equals(TEXT("SoftClass"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("TSoftClassPtr"), ESearchCase::CaseSensitive);
				const bool bSoftObject = Head.Equals(TEXT("SoftObject"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("TSoftObjectPtr"), ESearchCase::CaseSensitive);
				const bool bObject = Head.Equals(TEXT("Object"), ESearchCase::CaseSensitive) || Head.Equals(TEXT("TObjectPtr"), ESearchCase::CaseSensitive);
				if (bClass || bSoftClass || bSoftObject || bObject)
				{
					const UClass* Class = ResolveClassByName(Inner);
					if (!Class)
					{
						Error = FString::Printf(TEXT("`%s` is not a class."), *Inner);
						return false;
					}
					const TCHAR* Prefix = bClass ? TEXT("Class") : bSoftClass ? TEXT("SoftClass") : bSoftObject ? TEXT("SoftObject") : TEXT("Object");
					Out = FString::Printf(TEXT("%s:%s"), Prefix, *Class->GetPathName());
					return true;
				}
				Error = FString::Printf(TEXT("`%s<...>` is not a parameter type."), *Head);
				return false;
			}

			struct FKeyword
			{
				const TCHAR* Written;
				const TCHAR* Canonical;
			};
			static const FKeyword Keywords[] = {
				{ TEXT("bool"), TEXT("bool") }, { TEXT("int"), TEXT("int32") }, { TEXT("int32"), TEXT("int32") },
				{ TEXT("int64"), TEXT("int64") }, { TEXT("float"), TEXT("float") }, { TEXT("double"), TEXT("double") },
				{ TEXT("Name"), TEXT("Name") }, { TEXT("FName"), TEXT("Name") }, { TEXT("String"), TEXT("String") },
				{ TEXT("FString"), TEXT("String") }, { TEXT("Text"), TEXT("Text") }, { TEXT("FText"), TEXT("Text") },
			};
			for (const FKeyword& Keyword : Keywords)
			{
				if (Compact == Keyword.Written)
				{
					Out = Keyword.Canonical;
					return true;
				}
			}

			// A struct, then an enum, then a class: the first that has the name. (Looking for a class can scan every
			// Blueprint in the project, so it is the last resort.)
			if (const UScriptStruct* Struct = ResolveStructByName(Compact))
			{
				Out = TEXT("Struct:") + Struct->GetPathName();
				return true;
			}
			if (const UEnum* Enum = ResolveEnumByName(Compact))
			{
				Out = TEXT("Enum:") + Enum->GetPathName();
				return true;
			}
			if (const UClass* Class = ResolveClassByName(Compact))
			{
				Out = TEXT("Object:") + Class->GetPathName();
				return true;
			}
			Error = FString::Printf(TEXT("`%s` is not a type the property bag knows."), *Compact);
			return false;
		}

		UObject* FindTypeObject(const FString& Path)
		{
			if (UObject* Found = FindObject<UObject>(nullptr, *Path))
			{
				return Found;
			}
			return LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		bool MakeBagDesc(const FName Name, const FString& Canonical, FPropertyBagPropertyDesc& Out, FString& Error)
		{
			FPropertyBagContainerTypes Containers;
			FString Rest = Canonical;
			while (true)
			{
				if (Rest.StartsWith(TEXT("Array<")) && Rest.EndsWith(TEXT(">")))
				{
					if (!Containers.CanAdd())
					{
						Error = TEXT("Containers nest at most two levels deep.");
						return false;
					}
					Containers.Add(EPropertyBagContainerType::Array);
					Rest = Rest.Mid(6, Rest.Len() - 7);
				}
				else if (Rest.StartsWith(TEXT("Set<")) && Rest.EndsWith(TEXT(">")))
				{
					if (!Containers.CanAdd())
					{
						Error = TEXT("Containers nest at most two levels deep.");
						return false;
					}
					Containers.Add(EPropertyBagContainerType::Set);
					Rest = Rest.Mid(4, Rest.Len() - 5);
				}
				else
				{
					break;
				}
			}

			EPropertyBagPropertyType Type = EPropertyBagPropertyType::None;
			const UObject* TypeObject = nullptr;
			struct FScalar
			{
				const TCHAR* Written;
				EPropertyBagPropertyType Type;
			};
			static const FScalar Scalars[] = {
				{ TEXT("bool"), EPropertyBagPropertyType::Bool }, { TEXT("int32"), EPropertyBagPropertyType::Int32 },
				{ TEXT("int64"), EPropertyBagPropertyType::Int64 }, { TEXT("float"), EPropertyBagPropertyType::Float },
				{ TEXT("double"), EPropertyBagPropertyType::Double }, { TEXT("Name"), EPropertyBagPropertyType::Name },
				{ TEXT("String"), EPropertyBagPropertyType::String }, { TEXT("Text"), EPropertyBagPropertyType::Text },
			};
			bool bFound = false;
			for (const FScalar& Scalar : Scalars)
			{
				if (Rest == Scalar.Written)
				{
					Type = Scalar.Type;
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				struct FTyped
				{
					const TCHAR* Prefix;
					EPropertyBagPropertyType Type;
				};
				static const FTyped Typed[] = {
					{ TEXT("Enum:"), EPropertyBagPropertyType::Enum }, { TEXT("Struct:"), EPropertyBagPropertyType::Struct },
					{ TEXT("Object:"), EPropertyBagPropertyType::Object }, { TEXT("SoftObject:"), EPropertyBagPropertyType::SoftObject },
					{ TEXT("Class:"), EPropertyBagPropertyType::Class }, { TEXT("SoftClass:"), EPropertyBagPropertyType::SoftClass },
				};
				for (const FTyped& Item : Typed)
				{
					if (Rest.StartsWith(Item.Prefix))
					{
						Type = Item.Type;
						TypeObject = FindTypeObject(Rest.Mid(FCString::Strlen(Item.Prefix)));
						bFound = TypeObject != nullptr;
						if (!bFound)
						{
							Error = FString::Printf(TEXT("`%s` cannot be loaded."), *Rest.Mid(FCString::Strlen(Item.Prefix)));
							return false;
						}
						break;
					}
				}
			}
			if (!bFound)
			{
				Error = FString::Printf(TEXT("`%s` is not a parameter type."), *Canonical);
				return false;
			}

			Out = FPropertyBagPropertyDesc(Name, Containers, Type, TypeObject);
			return true;
		}
	}

	bool ParseParameterType(const FString& Written, FString& OutCanonical, FString& OutError)
	{
		return ParseTypeRecursive(CompactTypeText(Written), OutCanonical, OutError, 0);
	}

	FString BagDescToCanonicalType(const FPropertyBagPropertyDesc& Desc)
	{
		FString Scalar;
		switch (Desc.ValueType)
		{
		case EPropertyBagPropertyType::Bool: Scalar = TEXT("bool"); break;
		case EPropertyBagPropertyType::Int32: Scalar = TEXT("int32"); break;
		case EPropertyBagPropertyType::Int64: Scalar = TEXT("int64"); break;
		case EPropertyBagPropertyType::Float: Scalar = TEXT("float"); break;
		case EPropertyBagPropertyType::Double: Scalar = TEXT("double"); break;
		case EPropertyBagPropertyType::Name: Scalar = TEXT("Name"); break;
		case EPropertyBagPropertyType::String: Scalar = TEXT("String"); break;
		case EPropertyBagPropertyType::Text: Scalar = TEXT("Text"); break;
		case EPropertyBagPropertyType::Enum: Scalar = TEXT("Enum:") + (Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString()); break;
		case EPropertyBagPropertyType::Struct: Scalar = TEXT("Struct:") + (Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString()); break;
		case EPropertyBagPropertyType::Object: Scalar = TEXT("Object:") + (Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString()); break;
		case EPropertyBagPropertyType::SoftObject: Scalar = TEXT("SoftObject:") + (Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString()); break;
		case EPropertyBagPropertyType::Class: Scalar = TEXT("Class:") + (Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString()); break;
		case EPropertyBagPropertyType::SoftClass: Scalar = TEXT("SoftClass:") + (Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString()); break;
		default: Scalar = TEXT("?"); break;
		}

		// ContainerTypes[0] is the outermost container.
		for (int32 Index = static_cast<int32>(Desc.ContainerTypes.Num()) - 1; Index >= 0; --Index)
		{
			switch (Desc.ContainerTypes[Index])
			{
			case EPropertyBagContainerType::Array: Scalar = FString::Printf(TEXT("Array<%s>"), *Scalar); break;
			case EPropertyBagContainerType::Set: Scalar = FString::Printf(TEXT("Set<%s>"), *Scalar); break;
			default: Scalar = TEXT("?"); break;
			}
		}
		return Scalar;
	}

	// ------------------------------------------------------------------------------------------
	// FParamSet
	// ------------------------------------------------------------------------------------------

	bool FParamSet::Build(FString& OutError)
	{
		TArray<FPropertyBagPropertyDesc> Descs;
		for (const Model::FDstParameter& Param : Params)
		{
			FPropertyBagPropertyDesc Desc;
			FString Error;
			if (!MakeBagDesc(FName(*Param.Name), Param.Type, Desc, Error))
			{
				OutError = FString::Printf(TEXT("Parameter `%s`: %s"), *Param.Name, *Error);
				return false;
			}
			Descs.Add(MoveTemp(Desc));
		}
		Bag.Reset();
		Defaults.Reset();
		if (Descs.IsEmpty())
		{
			// No parameters: nothing to look up, nothing to default.
			return true;
		}
		const UPropertyBag* Created = UPropertyBag::GetOrCreateFromDescs(Descs);
		if (!Created)
		{
			OutError = TEXT("The property bag for the parameters could not be created.");
			return false;
		}
		Bag = TStrongObjectPtr<UPropertyBag>(const_cast<UPropertyBag*>(Created));
		Defaults = MakeShared<FStructOnScope>(Created);
		return true;
	}

	const FProperty* FParamSet::FindProperty(const FString& Name) const
	{
		if (!Bag.IsValid())
		{
			return nullptr;
		}
		const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(FName(*Name));
		return Desc ? Desc->CachedProperty : nullptr;
	}

	const Model::FDstParameter* FParamSet::Find(const FString& Name) const
	{
		for (const Model::FDstParameter& Param : Params)
		{
			if (Param.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return &Param;
			}
		}
		return nullptr;
	}

	const void* FParamSet::GetDefaultMemory() const
	{
		return Defaults.IsValid() ? Defaults->GetStructMemory() : nullptr;
	}

	TArray<FString> FParamSet::GetNames() const
	{
		TArray<FString> Names;
		for (const Model::FDstParameter& Param : Params)
		{
			Names.Add(Param.Name);
		}
		return Names;
	}

	// ------------------------------------------------------------------------------------------
	// Node types
	// ------------------------------------------------------------------------------------------

	const void* FNodeInfo::GetInstanceDefaults() const
	{
		if (InstanceCDO)
		{
			return InstanceCDO;
		}
		return InstanceDefaults.IsValid() ? InstanceDefaults->GetStructMemory() : nullptr;
	}

	const void* FNodeInfo::GetNodeDefaults() const
	{
		return NodeDefaults.IsValid() ? NodeDefaults->GetStructMemory() : nullptr;
	}

	FString GetNodeTypeShortName(const FDstNodeType& Type)
	{
		if (Type.Struct && Type.BlueprintClass == nullptr && Type.BlueprintAssetPath.IsEmpty())
		{
			return FDstNameRegistry::MakeShortName(Type.Struct->GetPrefixCPP() + Type.Struct->GetName());
		}
		FString Name;
		if (Type.BlueprintClass)
		{
			Name = Type.BlueprintClass->GetName();
		}
		else
		{
			Name = FPackageName::ObjectPathToObjectName(Type.BlueprintAssetPath);
		}
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	FNodeInfoPtr FDstLowerer::GetNodeInfo(const FDstNodeType& Type)
	{
		if (const FNodeInfoPtr* Cached = NodeInfoCache.Find(&Type))
		{
			return *Cached;
		}

		FNodeInfoPtr Info = MakeShared<FNodeInfo>();
		Info->Type = &Type;
		Info->NodeStruct = Type.Struct;
		Info->bBlueprint = Type.BlueprintClass != nullptr || !Type.BlueprintAssetPath.IsEmpty();

		if (Type.Struct)
		{
			Info->NodeDefaults = MakeShared<FStructOnScope>(Type.Struct);
		}

		if (Info->bBlueprint)
		{
			const UClass* Class = Type.BlueprintClass;
			if (!Class && !Type.BlueprintAssetPath.IsEmpty())
			{
				if (const UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Type.BlueprintAssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
				{
					Class = Blueprint->GeneratedClass;
				}
			}
			Info->InstanceStruct = Class;
			Info->InstanceCDO = Class ? Class->GetDefaultObject() : nullptr;
		}
		else if (Info->NodeDefaults.IsValid() && Info->NodeDefaults->IsValid())
		{
			const FStateTreeNodeBase* Base = static_cast<const FStateTreeNodeBase*>(static_cast<const void*>(Info->NodeDefaults->GetStructMemory()));
			Info->InstanceStruct = Base->GetInstanceDataType();
			if (const UClass* InstanceClass = Cast<UClass>(Info->InstanceStruct))
			{
				Info->InstanceCDO = InstanceClass->GetDefaultObject();
			}
			else if (const UScriptStruct* InstanceScriptStruct = Cast<UScriptStruct>(Info->InstanceStruct))
			{
				Info->InstanceDefaults = MakeShared<FStructOnScope>(InstanceScriptStruct);
			}
		}

		NodeInfoCache.Add(&Type, Info);
		return Info;
	}

	FNodeInfoPtr FDstLowerer::GetNodeInfoForStruct(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return nullptr;
		}
		if (const FDstNodeType* Registered = FDstNameRegistry::Get().FindByStruct(Struct))
		{
			return GetNodeInfo(*Registered);
		}

		// A type the registry does not list: describe it from the struct alone.
		TUniquePtr<FDstNodeType> Synthetic = MakeUnique<FDstNodeType>();
		Synthetic->Struct = Struct;
		Synthetic->Name = FDstNameRegistry::MakeShortName(Struct->GetPrefixCPP() + Struct->GetName());
		const FDstNodeType* Raw = Synthetic.Get();
		SyntheticTypes.Add(MoveTemp(Synthetic));
		return GetNodeInfo(*Raw);
	}
}
