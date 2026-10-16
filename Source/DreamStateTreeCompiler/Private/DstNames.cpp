// Copyright DreamMoon. All Rights Reserved.
//
// The names registry. It lists every node type the text can write (C++ node structs, Blueprint node classes) and
// every schema class, each under the names DstNames.h defines, and answers the two questions the semantic stage and
// the decompiler ask: what does this written name mean, and what is the shortest name that means this type.
//
// What is listed:
//   - Every USTRUCT derived from FStateTreeNodeBase that is a task, condition, evaluator, consideration or property
//     function by its base. Skipped, following what the engine's own node picker does: the base structs themselves,
//     structs marked meta=(Hidden) (FStateTreeTaskCommonBase and the other bases games derive from), the Blueprint
//     wrapper structs (they are reached through the Blueprint class), leftovers of a hot reload, and the engine's
//     own StateTreeTestSuite test nodes.
//   - Blueprint nodes found without loading anything: a Blueprint asset whose native parent class (a tag in the asset
//     registry) is one of the Blueprint node base classes. Native subclasses of those bases are listed too, so a node
//     written in C++ in the Blueprint style can be named.
//
// The registry rebuilds itself when something that changes its content happens (a Blueprint asset appears, goes or is
// renamed; a hot reload; a module loads), the next time Get() is called. Pointers into it stay valid until then.

#include "DstNames.h"

#include "Algo/Sort.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeNodeBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "DstSugar.h"
#include "Engine/Blueprint.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeSchema.h"
#include "StateTreeTaskBase.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

namespace UE::DreamStateTree::Compiler
{
	const TCHAR* LexToString(EDstNodeCategory Category)
	{
		switch (Category)
		{
		case EDstNodeCategory::Task: return TEXT("Task");
		case EDstNodeCategory::Condition: return TEXT("Condition");
		case EDstNodeCategory::Evaluator: return TEXT("Evaluator");
		case EDstNodeCategory::Consideration: return TEXT("Consideration");
		case EDstNodeCategory::PropertyFunction: return TEXT("PropertyFunction");
		default: return TEXT("Unknown");
		}
	}

	namespace NamesPrivate
	{
		constexpr const TCHAR* MetaName = TEXT("DreamStateTree");
		constexpr const TCHAR* MetaAliases = TEXT("DreamStateTreeAliases");
		constexpr const TCHAR* MetaArgs = TEXT("DreamStateTreeArgs");
		constexpr const TCHAR* MetaTags = TEXT("DreamStateTreeTags");
		constexpr const TCHAR* MetaNamespace = TEXT("DreamStateTreeNamespace");

		/** The engine's own test nodes and schemas live here; they are not something a tree should name. */
		constexpr const TCHAR* EngineTestSuitePrefix = TEXT("/Script/StateTreeTestSuite.");

		/** Set by the hooks below; Get() rebuilds before the next use. */
		FThreadSafeBool GStale(false);

		// ------------------------------------------------------------------------------------------------------------
		// Small helpers
		// ------------------------------------------------------------------------------------------------------------

		TArray<FString> SplitList(const FString& Text, const TCHAR* Separator)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, Separator, /*InCullEmpty*/ true);
			TArray<FString> Result;
			for (FString& Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (!Part.IsEmpty())
				{
					Result.Add(MoveTemp(Part));
				}
			}
			return Result;
		}

		FString ReadMeta(const UField& Field, const TCHAR* Key)
		{
			const FString* Value = Field.FindMetaData(Key);
			return Value ? Value->TrimStartAndEnd() : FString();
		}

		bool ContainsExact(const TArray<FString>& Names, const FString& Name)
		{
			for (const FString& Existing : Names)
			{
				if (Existing.Equals(Name, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		void AddOtherName(FDstNodeType& Type, const FString& Name)
		{
			if (Name.IsEmpty() || Name.Equals(Type.Name, ESearchCase::CaseSensitive) || ContainsExact(Type.OtherNames, Name))
			{
				return;
			}
			Type.OtherNames.Add(Name);
		}

		bool HasName(const FDstNodeType& Type, const FString& Name)
		{
			return Type.Name.Equals(Name, ESearchCase::CaseSensitive) || ContainsExact(Type.OtherNames, Name);
		}

		bool StartsWithHotReloadPrefix(const FString& Name)
		{
			return Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("HOTRELOADED_")) || Name.StartsWith(TEXT("SKEL_"));
		}

		// ------------------------------------------------------------------------------------------------------------
		// Which structs and classes count
		// ------------------------------------------------------------------------------------------------------------

		EDstNodeCategory GetCategory(const UScriptStruct& Struct)
		{
			if (Struct.IsChildOf(FStateTreeTaskBase::StaticStruct())) return EDstNodeCategory::Task;
			if (Struct.IsChildOf(FStateTreeConditionBase::StaticStruct())) return EDstNodeCategory::Condition;
			if (Struct.IsChildOf(FStateTreeEvaluatorBase::StaticStruct())) return EDstNodeCategory::Evaluator;
			if (Struct.IsChildOf(FStateTreeConsiderationBase::StaticStruct())) return EDstNodeCategory::Consideration;
			if (Struct.IsChildOf(FStateTreePropertyFunctionBase::StaticStruct())) return EDstNodeCategory::PropertyFunction;
			return EDstNodeCategory::Unknown;
		}

		EDstNodeCategory GetBlueprintCategory(const UClass& Class)
		{
			if (Class.IsChildOf(UStateTreeTaskBlueprintBase::StaticClass())) return EDstNodeCategory::Task;
			if (Class.IsChildOf(UStateTreeConditionBlueprintBase::StaticClass())) return EDstNodeCategory::Condition;
			if (Class.IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass())) return EDstNodeCategory::Evaluator;
			if (Class.IsChildOf(UStateTreeConsiderationBlueprintBase::StaticClass())) return EDstNodeCategory::Consideration;
			return EDstNodeCategory::Unknown;
		}

		/** The struct a Blueprint node of this kind is stored as; property functions have no Blueprint form. */
		const UScriptStruct* GetWrapperStruct(EDstNodeCategory Category)
		{
			switch (Category)
			{
			case EDstNodeCategory::Task: return FStateTreeBlueprintTaskWrapper::StaticStruct();
			case EDstNodeCategory::Condition: return FStateTreeBlueprintConditionWrapper::StaticStruct();
			case EDstNodeCategory::Evaluator: return FStateTreeBlueprintEvaluatorWrapper::StaticStruct();
			case EDstNodeCategory::Consideration: return FStateTreeBlueprintConsiderationWrapper::StaticStruct();
			default: return nullptr;
			}
		}

		bool IsBlueprintWrapper(const UScriptStruct* Struct)
		{
			return Struct == FStateTreeBlueprintTaskWrapper::StaticStruct()
				|| Struct == FStateTreeBlueprintConditionWrapper::StaticStruct()
				|| Struct == FStateTreeBlueprintEvaluatorWrapper::StaticStruct()
				|| Struct == FStateTreeBlueprintConsiderationWrapper::StaticStruct();
		}

		bool IsRootBase(const UScriptStruct* Struct)
		{
			return Struct == FStateTreeNodeBase::StaticStruct()
				|| Struct == FStateTreeTaskBase::StaticStruct()
				|| Struct == FStateTreeConditionBase::StaticStruct()
				|| Struct == FStateTreeEvaluatorBase::StaticStruct()
				|| Struct == FStateTreeConsiderationBase::StaticStruct()
				|| Struct == FStateTreePropertyFunctionBase::StaticStruct();
		}

		bool IsNameableNodeStruct(const UScriptStruct& Struct)
		{
			if (IsRootBase(&Struct) || IsBlueprintWrapper(&Struct))
			{
				return false;
			}
			// The engine's node picker skips these too; they are the bases games derive their nodes from.
			if (Struct.HasMetaData(TEXT("Hidden")))
			{
				return false;
			}
			if (StartsWithHotReloadPrefix(Struct.GetName()) || Struct.GetPathName().StartsWith(EngineTestSuitePrefix))
			{
				return false;
			}
			return true;
		}

		TArray<FName> GetPositionalArgs(const UScriptStruct& Struct)
		{
			TArray<FName> Result;
			const FString Meta = ReadMeta(Struct, MetaArgs);
			if (!Meta.IsEmpty())
			{
				for (const FString& Part : SplitList(Meta, TEXT(",")))
				{
					Result.Add(FName(*Part));
				}
				return Result;
			}

			const FString Path = Struct.GetPathName();
			for (const Sugar::FPositional& Entry : Sugar::EnginePositionalArgs)
			{
				if (Path.Equals(Entry.StructPath, ESearchCase::CaseSensitive))
				{
					for (const FString& Part : SplitList(Entry.Properties, TEXT(",")))
					{
						Result.Add(FName(*Part));
					}
					break;
				}
			}
			return Result;
		}

		void ReadCommonMeta(FDstNodeType& Type, const UField& Field)
		{
			for (const FString& Alias : SplitList(ReadMeta(Field, MetaAliases), TEXT(";")))
			{
				AddOtherName(Type, Alias);
			}
			Type.Tags = SplitList(ReadMeta(Field, MetaTags), TEXT(";"));
		}

		// ------------------------------------------------------------------------------------------------------------
		// Name lookup
		// ------------------------------------------------------------------------------------------------------------

		/** The indices of the types that have exactly this name (case-sensitive). */
		void CollectExact(const TArray<FDstNodeType>& Types, const TMultiMap<FString, int32>& ByName, const FString& Query, TArray<int32>& Out)
		{
			Out.Reset();
			TArray<int32> Candidates;
			ByName.MultiFind(Query.ToLower(), Candidates);
			for (const int32 Index : Candidates)
			{
				if (HasName(Types[Index], Query))
				{
					Out.AddUnique(Index);
				}
			}
			Out.Sort();
		}

		struct FResolved
		{
			TArray<int32> Matches;
			FString MatchNamespace;
			/** Index and namespace of every match a winner hid. */
			TArray<TPair<int32, FString>> Shadowed;
		};

		/** The namespaces in order, then the global names; the first level that has the name wins. */
		FResolved ResolveCore(const TArray<FDstNodeType>& Types, const TMultiMap<FString, int32>& ByName, const FString& Written,
			TConstArrayView<FString> Namespaces, TConstArrayView<EDstNodeCategory> Categories = {})
		{
			FResolved Result;
			bool bFound = false;
			TArray<int32> Level;
			const auto Consider = [&](const FString& Query, const FString& Namespace)
			{
				CollectExact(Types, ByName, Query, Level);
				// Only the kinds the place takes count: a task's name does not hide a condition of the same name.
				if (!Categories.IsEmpty())
				{
					Level.RemoveAll([&Types, Categories](const int32 Index) { return !Categories.Contains(Types[Index].Category); });
				}
				if (Level.IsEmpty())
				{
					return;
				}
				if (!bFound)
				{
					bFound = true;
					Result.Matches = Level;
					Result.MatchNamespace = Namespace;
					return;
				}
				for (const int32 Index : Level)
				{
					const bool bAlreadyListed = Result.Matches.Contains(Index)
						|| Result.Shadowed.ContainsByPredicate([Index](const TPair<int32, FString>& Pair) { return Pair.Key == Index; });
					if (!bAlreadyListed)
					{
						Result.Shadowed.Emplace(Index, Namespace);
					}
				}
			};

			for (const FString& Namespace : Namespaces)
			{
				if (!Namespace.IsEmpty())
				{
					Consider(Namespace + TEXT(".") + Written, Namespace);
				}
			}
			Consider(Written, FString());
			return Result;
		}

		/** A Blueprint node's class is loaded the first time a name resolves to it. */
		void EnsureBlueprintClass(const FDstNodeType& Type)
		{
			if (Type.BlueprintClass || Type.BlueprintAssetPath.IsEmpty())
			{
				return;
			}
			UObject* Asset = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *Type.BlueprintAssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			{
				// Logical constness: the registry is a cache, this fills in what it had left for later.
				const_cast<FDstNodeType&>(Type).BlueprintClass = Blueprint->GeneratedClass.Get();
			}
		}

		// ------------------------------------------------------------------------------------------------------------
		// Schemas
		// ------------------------------------------------------------------------------------------------------------

		/** `UStateTreeAIComponentSchema` -> `AIComponent`; `UGameplayInteractionStateTreeSchema` -> `GameplayInteraction`. */
		FString MakeSchemaShortName(const UClass& Class)
		{
			FString Name = Class.GetName();
			if (Name.StartsWith(TEXT("StateTree"), ESearchCase::CaseSensitive) && Name.Len() > 9)
			{
				Name.RightChopInline(9);
			}
			if (Name.EndsWith(TEXT("Schema"), ESearchCase::CaseSensitive) && Name.Len() > 6)
			{
				Name.LeftChopInline(6);
			}
			if (Name.EndsWith(TEXT("StateTree"), ESearchCase::CaseSensitive) && Name.Len() > 9)
			{
				Name.LeftChopInline(9);
			}
			return Name;
		}

		struct FSchemaNames
		{
			FString Primary;
			/** The reflected class name (`StateTreeAIComponentSchema`) and the aliases. */
			TArray<FString> Others;
		};

		FSchemaNames GetSchemaNames(const UClass& Class)
		{
			FSchemaNames Names;
			const FString Meta = ReadMeta(Class, MetaName);
			Names.Primary = Meta.IsEmpty() ? MakeSchemaShortName(Class) : Meta;
			const auto AddOther = [&Names](const FString& Name)
			{
				if (!Name.IsEmpty() && !Name.Equals(Names.Primary, ESearchCase::CaseSensitive) && !ContainsExact(Names.Others, Name))
				{
					Names.Others.Add(Name);
				}
			};
			AddOther(Class.GetName());
			for (const FString& Alias : SplitList(ReadMeta(Class, MetaAliases), TEXT(";")))
			{
				AddOther(Alias);
			}
			return Names;
		}

		/** Every schema a tree could name: not abstract, not a hot-reload leftover, not the engine's test schema. */
		TArray<UClass*> CollectSchemaClasses()
		{
			TArray<UClass*> Derived;
			GetDerivedClasses(UStateTreeSchema::StaticClass(), Derived, /*bRecursive*/ true);
			TArray<UClass*> Result;
			for (UClass* Class : Derived)
			{
				if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Transient))
				{
					continue;
				}
				if (StartsWithHotReloadPrefix(Class->GetName()) || Class->GetPathName().StartsWith(EngineTestSuitePrefix))
				{
					continue;
				}
				Result.Add(Class);
			}
			Algo::Sort(Result, [](const UClass* A, const UClass* B)
			{
				return A->GetPathName().Compare(B->GetPathName(), ESearchCase::CaseSensitive) < 0;
			});
			return Result;
		}

		// ------------------------------------------------------------------------------------------------------------
		// Staleness
		// ------------------------------------------------------------------------------------------------------------

		/**
		 * Marks the registry stale when its content may have changed. Registered on first use and removed when the
		 * module's DLL goes away, so no delegate outlives its code.
		 */
		class FInvalidationHooks
		{
		public:
			FInvalidationHooks()
			{
				FModuleManager::Get().LoadModule(TEXT("AssetRegistry"));
				if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
				{
					AssetRegistry->OnAssetAdded().AddRaw(this, &FInvalidationHooks::OnAssetChanged);
					AssetRegistry->OnAssetRemoved().AddRaw(this, &FInvalidationHooks::OnAssetChanged);
					AssetRegistry->OnAssetRenamed().AddRaw(this, &FInvalidationHooks::OnAssetRenamed);
				}
				ReloadHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddRaw(this, &FInvalidationHooks::OnReloadComplete);
				ModulesHandle = FModuleManager::Get().OnModulesChanged().AddRaw(this, &FInvalidationHooks::OnModulesChanged);
			}

			~FInvalidationHooks()
			{
				if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
				{
					if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
					{
						AssetRegistry->OnAssetAdded().RemoveAll(this);
						AssetRegistry->OnAssetRemoved().RemoveAll(this);
						AssetRegistry->OnAssetRenamed().RemoveAll(this);
					}
				}
				FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadHandle);
				FModuleManager::Get().OnModulesChanged().Remove(ModulesHandle);
			}

		private:
			void OnAssetChanged(const FAssetData& AssetData)
			{
				// Only Blueprints can be nodes; every other asset (a StateTree asset a build just made) is none of our business.
				if (AssetData.FindTag(FBlueprintTags::NativeParentClassPath))
				{
					GStale = true;
				}
			}

			void OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
			{
				OnAssetChanged(AssetData);
			}

			void OnReloadComplete(EReloadCompleteReason Reason)
			{
				GStale = true;
			}

			void OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
			{
				GStale = true;
			}

			FDelegateHandle ReloadHandle;
			FDelegateHandle ModulesHandle;
		};
	}

	// ----------------------------------------------------------------------------------------------------------------
	// FDstNameRegistry
	// ----------------------------------------------------------------------------------------------------------------

	FDstNameRegistry& FDstNameRegistry::Get()
	{
		static FDstNameRegistry Registry;
		static NamesPrivate::FInvalidationHooks Hooks;
		if (!Registry.bBuilt || NamesPrivate::GStale)
		{
			// Cleared first: a change that arrives while building marks it stale again.
			NamesPrivate::GStale = false;
			Registry.Build();
		}
		return Registry;
	}

	void FDstNameRegistry::Refresh()
	{
		NamesPrivate::GStale = false;
		Build();
	}

	FString FDstNameRegistry::MakeShortName(const FString& StructName)
	{
		FString Name = StructName;
		// The C++ prefix. `StateTreeDelayTask` (the reflected name) has none, and `Focus` is not `ocus`.
		if (Name.Len() > 1 && Name[0] == TEXT('F') && FChar::IsUpper(Name[1]))
		{
			Name.RightChopInline(1);
		}
		if (Name.StartsWith(TEXT("StateTree"), ESearchCase::CaseSensitive) && Name.Len() > 9)
		{
			Name.RightChopInline(9);
		}
		static const TCHAR* const Suffixes[] = { TEXT("PropertyFunction"), TEXT("Consideration"), TEXT("Evaluator"), TEXT("Condition"), TEXT("Task") };
		for (const TCHAR* Suffix : Suffixes)
		{
			const int32 SuffixLength = FCString::Strlen(Suffix);
			if (Name.Len() > SuffixLength && Name.EndsWith(Suffix, ESearchCase::CaseSensitive))
			{
				Name.LeftChopInline(SuffixLength);
				break;
			}
		}
		return Name;
	}

	void FDstNameRegistry::Build()
	{
		using namespace NamesPrivate;

		Types.Reset();
		ByName.Reset();
		ByStruct.Reset();
		ByBlueprintPath.Reset();

		// ---- C++ node structs
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			const UScriptStruct* Struct = *It;
			if (!Struct->IsChildOf(FStateTreeNodeBase::StaticStruct()) || !IsNameableNodeStruct(*Struct))
			{
				continue;
			}
			const EDstNodeCategory Category = GetCategory(*Struct);
			if (Category == EDstNodeCategory::Unknown)
			{
				continue;
			}

			FDstNodeType Type;
			Type.Struct = Struct;
			Type.Category = Category;

			// Reflection drops the C++ prefix letter: the struct FStateTreeDelayTask is named StateTreeDelayTask.
			const FString ReflectedName = Struct->GetName();
			const FString Meta = ReadMeta(*Struct, MetaName);
			// A name from the meta replaces the short name; the reflected name always resolves as well.
			Type.Name = Meta.IsEmpty() ? FDstNameRegistry::MakeShortName(TEXT("F") + ReflectedName) : Meta;
			AddOtherName(Type, ReflectedName);
			ReadCommonMeta(Type, *Struct);
			Type.PositionalArgs = GetPositionalArgs(*Struct);
			Types.Add(MoveTemp(Type));
		}

		// ---- Blueprint nodes
		// Native classes that are, or derive from, a Blueprint node base: a Blueprint asset names the nearest native
		// one as its native parent in the registry.
		TMap<FString, const UClass*> NativeBases;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			const UClass* Class = *It;
			if (!Class->HasAnyClassFlags(CLASS_Native) || Class->HasAnyClassFlags(CLASS_NewerVersionExists)
				|| !Class->IsChildOf(UStateTreeNodeBlueprintBase::StaticClass()))
			{
				continue;
			}
			NativeBases.Add(Class->GetPathName(), Class);

			if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) || StartsWithHotReloadPrefix(Class->GetName())
				|| Class->GetPathName().StartsWith(EngineTestSuitePrefix))
			{
				continue;
			}
			const EDstNodeCategory Category = GetBlueprintCategory(*Class);
			if (Category == EDstNodeCategory::Unknown)
			{
				continue;
			}

			FDstNodeType Type;
			Type.Struct = GetWrapperStruct(Category);
			Type.BlueprintClass = Class;
			Type.Category = Category;
			const FString Meta = ReadMeta(*Class, MetaName);
			Type.Name = Meta.IsEmpty() ? Class->GetName() : Meta;
			ReadCommonMeta(Type, *Class);
			Types.Add(MoveTemp(Type));
		}

		FModuleManager::Get().LoadModule(TEXT("AssetRegistry"));
		if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
		{
			// A name that is not there yet would read as an unknown node.
			if (AssetRegistry->IsLoadingAssets())
			{
				AssetRegistry->WaitForCompletion();
			}

			FARFilter Filter;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			AssetRegistry->EnumerateAssets(Filter, [this, &NativeBases](const FAssetData& Asset)
			{
				FString NativeParent;
				if (Asset.IsRedirector() || !Asset.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParent))
				{
					return true;
				}
				const UClass* const* Base = NativeBases.Find(FPackageName::ExportTextPathToObjectPath(NativeParent));
				if (!Base)
				{
					return true;
				}
				const EDstNodeCategory Category = GetBlueprintCategory(**Base);
				if (Category == EDstNodeCategory::Unknown)
				{
					return true;
				}

				FDstNodeType Type;
				Type.Struct = GetWrapperStruct(Category);
				Type.Category = Category;
				Type.Name = Asset.AssetName.ToString();
				Type.BlueprintAssetPath = Asset.GetObjectPathString();

				// Already in memory? Then the class is at hand without loading anything.
				FString GeneratedClassPath;
				if (Asset.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClassPath))
				{
					const FString ClassPath = FPackageName::ExportTextPathToObjectPath(GeneratedClassPath);
					Type.BlueprintClass = Cast<UClass>(StaticFindObject(UClass::StaticClass(), nullptr, *ClassPath));
				}
				Types.Add(MoveTemp(Type));
				return true;
			});
		}

		// A stable order, so listings and the ambiguity candidates read the same on every run.
		Types.Sort([](const FDstNodeType& A, const FDstNodeType& B)
		{
			const int32 ByNameOrder = A.Name.Compare(B.Name, ESearchCase::CaseSensitive);
			if (ByNameOrder != 0)
			{
				return ByNameOrder < 0;
			}
			const FString KeyA = A.Struct ? A.Struct->GetPathName() + A.BlueprintAssetPath : A.BlueprintAssetPath;
			const FString KeyB = B.Struct ? B.Struct->GetPathName() + B.BlueprintAssetPath : B.BlueprintAssetPath;
			return KeyA.Compare(KeyB, ESearchCase::CaseSensitive) < 0;
		});

		for (int32 Index = 0; Index < Types.Num(); ++Index)
		{
			const FDstNodeType& Type = Types[Index];
			ByName.AddUnique(Type.Name.ToLower(), Index);
			for (const FString& Other : Type.OtherNames)
			{
				ByName.AddUnique(Other.ToLower(), Index);
			}

			if (!Type.BlueprintAssetPath.IsEmpty())
			{
				ByBlueprintPath.Add(Type.BlueprintAssetPath, Index);
			}
			else if (Type.BlueprintClass)
			{
				ByBlueprintPath.Add(Type.BlueprintClass->GetPathName(), Index);
			}
			else if (Type.Struct)
			{
				ByStruct.Add(Type.Struct, Index);
			}
		}

		bBuilt = true;
	}

	TArray<FDstNameMatch> FDstNameRegistry::ResolveNode(const FString& Written, TConstArrayView<FString> Namespaces,
		TArray<FDstNameMatch>* OutShadowed, TConstArrayView<EDstNodeCategory> Categories) const
	{
		using namespace NamesPrivate;

		if (OutShadowed)
		{
			OutShadowed->Reset();
		}
		TArray<FDstNameMatch> Result;
		if (Written.IsEmpty())
		{
			return Result;
		}

		const auto AddMatch = [this](TArray<FDstNameMatch>& Into, const int32 Index, const FString& Namespace)
		{
			FDstNameMatch Match;
			Match.Type = &Types[Index];
			Match.Namespace = Namespace;
			Into.Add(MoveTemp(Match));
		};

		// A path names exactly one type: a Blueprint asset (`/Game/AI/STT_Wander`, with or without `.STT_Wander`) or a
		// node struct (`/Script/StateTreeModule.StateTreeDelayTask`). The model stores node types this way.
		if (Written.StartsWith(TEXT("/")))
		{
			FString Path = Written;
			if (!Path.Contains(TEXT(".")))
			{
				Path += TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
			}
			if (const int32* Index = ByBlueprintPath.Find(Path))
			{
				AddMatch(Result, *Index, FString());
			}
			else if (const UScriptStruct* Struct = Cast<UScriptStruct>(StaticFindObject(UScriptStruct::StaticClass(), nullptr, *Path)))
			{
				if (const FDstNodeType* Type = FindByStruct(Struct))
				{
					FDstNameMatch Match;
					Match.Type = Type;
					Result.Add(MoveTemp(Match));
				}
			}
		}
		else
		{
			const FResolved Resolved = ResolveCore(Types, ByName, Written, Namespaces, Categories);
			for (const int32 Index : Resolved.Matches)
			{
				AddMatch(Result, Index, Resolved.MatchNamespace);
			}
			if (OutShadowed)
			{
				for (const TPair<int32, FString>& Pair : Resolved.Shadowed)
				{
					AddMatch(*OutShadowed, Pair.Key, Pair.Value);
				}
			}
		}

		for (const FDstNameMatch& Match : Result)
		{
			EnsureBlueprintClass(*Match.Type);
		}
		if (OutShadowed)
		{
			for (const FDstNameMatch& Match : *OutShadowed)
			{
				EnsureBlueprintClass(*Match.Type);
			}
		}
		return Result;
	}

	const FDstNodeType* FDstNameRegistry::FindByStruct(const UScriptStruct* Struct) const
	{
		const int32* Index = Struct ? ByStruct.Find(Struct) : nullptr;
		return Index ? &Types[*Index] : nullptr;
	}

	const FDstNodeType* FDstNameRegistry::FindByBlueprintClass(const UClass* Class) const
	{
		if (!Class)
		{
			return nullptr;
		}
		// A generated class (also its skeleton and reinstanced versions) knows its Blueprint; a native class is its own key.
		const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy);
		const FString Key = Blueprint ? Blueprint->GetPathName() : Class->GetPathName();
		const int32* Index = ByBlueprintPath.Find(Key);
		return Index ? &Types[*Index] : nullptr;
	}

	FString FDstNameRegistry::GetWrittenName(const FDstNodeType& Type, TConstArrayView<FString> Namespaces,
		TConstArrayView<EDstNodeCategory> Categories) const
	{
		using namespace NamesPrivate;

		// Candidates by how much they say about the type: its own name (also without a namespace the schema supplies),
		// then the reflected struct name, then the old names.
		struct FCandidate
		{
			FString Text;
			int32 Rank;
		};
		TArray<FCandidate> Candidates;
		const auto AddCandidate = [&Candidates](const FString& Text, const int32 Rank)
		{
			if (Text.IsEmpty())
			{
				return;
			}
			for (const FCandidate& Existing : Candidates)
			{
				if (Existing.Text.Equals(Text, ESearchCase::CaseSensitive))
				{
					return;
				}
			}
			Candidates.Add({ Text, Rank });
		};
		const auto AddForms = [&](const FString& Name, const int32 Rank)
		{
			AddCandidate(Name, Rank);
			for (const FString& Namespace : Namespaces)
			{
				const FString Prefix = Namespace + TEXT(".");
				if (!Namespace.IsEmpty() && Name.Len() > Prefix.Len() && Name.StartsWith(Prefix, ESearchCase::CaseSensitive))
				{
					AddCandidate(Name.RightChop(Prefix.Len()), Rank);
				}
			}
		};

		AddForms(Type.Name, 0);
		const FString ReflectedName = Type.Struct ? Type.Struct->GetName() : FString();
		for (const FString& Other : Type.OtherNames)
		{
			if (Other.Equals(ReflectedName, ESearchCase::CaseSensitive))
			{
				AddForms(Other, 1);
			}
		}
		for (const FString& Other : Type.OtherNames)
		{
			if (!Other.Equals(ReflectedName, ESearchCase::CaseSensitive))
			{
				AddForms(Other, 2);
			}
		}

		// First choice: a name that resolves to this type alone and hides nothing (a shadowed name compiles, with a
		// warning). Second choice: the same without the last condition.
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			for (int32 Rank = 0; Rank <= 2; ++Rank)
			{
				const FCandidate* Best = nullptr;
				for (const FCandidate& Candidate : Candidates)
				{
					if (Candidate.Rank != Rank || (Best && Candidate.Text.Len() >= Best->Text.Len()))
					{
						continue;
					}
					const FResolved Resolved = ResolveCore(Types, ByName, Candidate.Text, Namespaces, Categories);
					const bool bOnlyThis = Resolved.Matches.Num() == 1 && &Types[Resolved.Matches[0]] == &Type;
					if (bOnlyThis && (Pass == 1 || Resolved.Shadowed.IsEmpty()))
					{
						Best = &Candidate;
					}
				}
				if (Best)
				{
					return Best->Text;
				}
			}
		}

		// Nothing resolves back to it alone (two Blueprints with the same name, say). The caller checks with ResolveNode
		// and falls back to the asset path.
		return Type.Name;
	}

	TArray<UClass*> FDstNameRegistry::ResolveSchema(const FString& Written) const
	{
		using namespace NamesPrivate;

		TArray<UClass*> Result;
		if (Written.IsEmpty())
		{
			return Result;
		}
		for (UClass* Class : CollectSchemaClasses())
		{
			const FSchemaNames Names = GetSchemaNames(*Class);
			if (Names.Primary.Equals(Written, ESearchCase::CaseSensitive) || ContainsExact(Names.Others, Written))
			{
				Result.Add(Class);
			}
		}
		return Result;
	}

	FString FDstNameRegistry::GetSchemaName(const UClass* SchemaClass) const
	{
		using namespace NamesPrivate;

		if (!SchemaClass)
		{
			return FString();
		}
		const FSchemaNames Names = GetSchemaNames(*SchemaClass);
		if (ResolveSchema(Names.Primary).Num() == 1)
		{
			return Names.Primary;
		}
		// Two schemas claim the same short name: the reflected class name still tells them apart.
		const FString Reflected = SchemaClass->GetName();
		if (ResolveSchema(Reflected).Num() == 1)
		{
			return Reflected;
		}
		return Names.Primary;
	}

	TArray<FString> FDstNameRegistry::GetSchemaNamespaces(const UClass* SchemaClass) const
	{
		using namespace NamesPrivate;

		// A schema that derives from a game's schema keeps the namespaces unless it says otherwise.
		for (const UClass* Class = SchemaClass; Class; Class = Class->GetSuperClass())
		{
			if (const FString* Meta = Class->FindMetaData(MetaNamespace))
			{
				return SplitList(*Meta, TEXT(";"));
			}
		}
		return TArray<FString>();
	}
}

namespace UE::DreamStateTree::Compiler
{
	TArray<EDstNodeCategory> FDstNameRegistry::GetNamingCategories(const EDstNodeCategory Category)
	{
		if (Category == EDstNodeCategory::Task || Category == EDstNodeCategory::Evaluator)
		{
			return { EDstNodeCategory::Task, EDstNodeCategory::Evaluator };
		}
		if (Category == EDstNodeCategory::Unknown)
		{
			return {};
		}
		return { Category };
	}
}
