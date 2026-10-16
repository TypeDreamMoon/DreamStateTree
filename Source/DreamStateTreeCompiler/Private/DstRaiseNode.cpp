// Copyright DreamMoon. All Rights Reserved.
//
// The raise, node calls. A node of the model becomes `Name(args) [ settings ]`:
//   - the name is the shortest one that resolves back to the node's type under the schema's namespaces;
//   - the instance-side arguments are positional while the type declares positions and the arguments are given without
//     a gap, named otherwise (a bool drops its `b` when nothing else would then answer to the name);
//   - the node-side arguments (bTaskEnabled, TransitionHandlingPriority, an editable property of the node struct itself)
//     are the settings; so is the Name of a node that has nowhere else to put it;
//   - a value is a literal (DstExportTextToLiteral), a path (`Params.X`, `Info.Target`, `Event.Offer`), `ref path`,
//     `Prop -> path`, or the operation / call of the property function that feeds it;
//   - a property fed in parts is its constant as a literal (a tuple, a list, a call of a tree's asset) with each fed part written
//     in at its place (InsertDeepArg).
// Each choice is the inverse of what DstLowerNode.cpp / DstLowerValue.cpp / DstLowerBinding.cpp accept.

#include "DstRaisePrivate.h"

#include "Engine/Blueprint.h"
#include "StateTree.h"
#include "StateTreeEvents.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeNodeBase.h"
#include "StateTreeReference.h"
#include "StateTreeSchema.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Raising
{
	namespace
	{
		/** Where a settings key sorts: the table's words first, in the table's order, then the node's own properties by name. */
		int32 GetRaiseSettingOrder(const FString& EngineName)
		{
			int32 Order = 0;
			for (const Sugar::FWord& Word : Sugar::NodeSettings)
			{
				if (EngineName.Equals(Word.Engine, ESearchCase::CaseSensitive))
				{
					return Order;
				}
				++Order;
			}
			return 100;
		}

		/** The word the text uses for an engine node property, or the property's own name. */
		FString GetRaiseSettingKey(const FString& EngineName)
		{
			for (const Sugar::FWord& Word : Sugar::NodeSettings)
			{
				if (EngineName.Equals(Word.Engine, ESearchCase::CaseSensitive))
				{
					return Word.Written;
				}
			}
			return EngineName;
		}

		/** A node struct property the settings table names (Enabled, ConsideredForCompletion, ...). */
		bool IsRaiseSettingProperty(const FString& EngineName)
		{
			for (const Sugar::FWord& Word : Sugar::NodeSettings)
			{
				if (EngineName.Equals(Word.Engine, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		bool IsRaiseBoolName(const FString& Name)
		{
			return Name.Len() > 1 && Name[0] == TEXT('b') && FChar::IsUpper(Name[1]);
		}
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

	void FDstRaiser::ReportOnce(const FString& Key, const TCHAR* Code, const FString& Message, const FString& Hint, const bool bWarning)
	{
		bool bAlready = false;
		Reported.Add(FString(Code) + TEXT("|") + Key, &bAlready);
		if (bAlready)
		{
			return;
		}
		if (bWarning)
		{
			Sink.Warning(Code, Lang::FDstSpan(), Message, Hint);
		}
		else
		{
			Sink.Error(Code, Lang::FDstSpan(), Message, Hint);
		}
	}

	FNodeInfoPtr FDstRaiser::GetNodeInfo(const FModelNode& Node)
	{
		// Globals take a task or an evaluator, so a global node's name has to tell the two apart; a state's `do` takes
		// tasks only. The written name differs between the two, so they are cached apart.
		const bool bGlobal = Node.Role == Model::EDstNodeRole::GlobalTask || Node.Role == Model::EDstNodeRole::Evaluator;
		const FString CacheKey = Node.StructType + TEXT("|") + Node.BlueprintClass + (bGlobal ? TEXT("|Global") : TEXT(""));
		if (const FNodeInfoPtr* Cached = NodeInfoCache.Find(CacheKey))
		{
			return *Cached;
		}

		FNodeInfoPtr Info = MakeShared<FNodeInfo>();
		Info->NodeStruct = FindStructByPath(Node.StructType);
		Info->bBlueprint = !Node.BlueprintClass.IsEmpty();
		const UClass* BlueprintClass = Info->bBlueprint ? FindClassByPath(Node.BlueprintClass) : nullptr;

		if (BlueprintClass)
		{
			Info->Type = Registry.FindByBlueprintClass(BlueprintClass);
		}
		else if (Info->NodeStruct)
		{
			Info->Type = Registry.FindByStruct(Info->NodeStruct);
		}

		if (Info->NodeStruct)
		{
			Info->NodeDefaults = MakeShared<FStructOnScope>(Info->NodeStruct);
		}
		if (Info->bBlueprint)
		{
			Info->InstanceStruct = BlueprintClass;
			Info->InstanceCDO = BlueprintClass ? BlueprintClass->GetDefaultObject() : nullptr;
		}
		else if (Info->NodeStruct && Info->NodeStruct->IsChildOf(FStateTreeNodeBase::StaticStruct())
			&& Info->NodeDefaults.IsValid() && Info->NodeDefaults->IsValid())
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

		// The name, and whether it reads back as this type alone.
		if (Info->NodeStruct)
		{
			Info->DisplayName = FDstNameRegistry::MakeShortName(Info->NodeStruct->GetPrefixCPP() + Info->NodeStruct->GetName());
		}
		else if (BlueprintClass)
		{
			Info->DisplayName = BlueprintClass->GetName();
			Info->DisplayName.RemoveFromEnd(TEXT("_C"));
		}
		else
		{
			Info->DisplayName = Node.StructType;
		}

		if (Info->Type)
		{
			const TArray<EDstNodeCategory> Categories = bGlobal ? FDstNameRegistry::GetNamingCategories(Info->Type->Category)
				: TArray<EDstNodeCategory>{ Info->Type->Category };
			Info->WrittenName = Registry.GetWrittenName(*Info->Type, Namespaces, Categories);
			const TArray<FDstNameMatch> Back = Registry.ResolveNode(Info->WrittenName, Namespaces, nullptr, Categories);
			const bool bResolvesBack = Back.Num() == 1 && Back[0].Type == Info->Type;
			if (!bResolvesBack)
			{
				if (Info->bBlueprint)
				{
					Info->bUseAssetCallee = true;
					Info->BlueprintAssetPath = !Info->Type->BlueprintAssetPath.IsEmpty() ? Info->Type->BlueprintAssetPath
						: (BlueprintClass && BlueprintClass->ClassGeneratedBy ? BlueprintClass->ClassGeneratedBy->GetPathName() : FString());
				}
				else
				{
					ReportOnce(Node.StructType, TEXT("DST8052"),
						FString::Printf(TEXT("The name `%s` does not read back as the node %s alone."), *Info->WrittenName, *Info->DisplayName),
						TEXT("Two node types answer to the same name; give one of them a distinct DreamStateTree meta name."), /*bWarning*/ true);
				}
			}
		}
		else
		{
			ReportOnce(Node.StructType + TEXT("|") + Node.BlueprintClass, TEXT("DST8051"),
				FString::Printf(TEXT("The node type `%s` is not one the text can name."), Node.BlueprintClass.IsEmpty() ? *Node.StructType : *Node.BlueprintClass),
				TEXT("It is not listed as a task, condition, evaluator, consideration or property function (a Hidden base struct, or a module that is not loaded)."));
			Info->WrittenName = Info->DisplayName;
		}

		NodeInfoCache.Add(CacheKey, Info);
		return Info;
	}

	FExprPtr FDstRaiser::MakeCalleeExpr(const FModelNode& Node, const FNodeInfo& Info)
	{
		if (Info.bUseAssetCallee && !Info.BlueprintAssetPath.IsEmpty())
		{
			if (FExprPtr Asset = MakeDstAssetExpr(Info.BlueprintAssetPath, UBlueprint::StaticClass()))
			{
				return Asset;
			}
		}
		TArray<FString> Segments;
		Info.WrittenName.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.IsEmpty())
		{
			Segments.Add(Info.DisplayName.IsEmpty() ? FString(TEXT("Node")) : Info.DisplayName);
		}
		return MakePathExpr(Segments);
	}

	// ------------------------------------------------------------------------------------------
	// Lookups
	// ------------------------------------------------------------------------------------------

	const FModelNode* FDstRaiser::FindNodeByKey(const FString& NodeKey) const
	{
		return Tree.FindNode(NodeKey);
	}

	FString FDstRaiser::FindBindName(const FString& NodeKey) const
	{
		const FModelNode* Node = FindNodeByKey(NodeKey);
		return Node ? Node->BindName : FString();
	}

	// ------------------------------------------------------------------------------------------
	// Constants
	// ------------------------------------------------------------------------------------------

	namespace
	{
		/**
		 * The value an empty export text stands for. The engine's exporter leaves a value that equals zero out, so a 0, a
		 * false, an empty name or a None is an empty text; the literal spells it out.
		 */
		FExprPtr MakeRaiseZeroLiteral(const FProperty& Property)
		{
			if (Property.IsA<FBoolProperty>())
			{
				return MakeBoolExpr(false);
			}
			if (const UEnum* Enum = Lowering::GetPropertyEnum(&Property))
			{
				const FString ValueName = Enum->GetNameStringByValue(0);
				return ValueName.IsEmpty() ? nullptr : MakeNameExpr(ValueName);
			}
			if (Property.IsA<FNumericProperty>())
			{
				return MakeNumberExpr(0.0);
			}
			if (Property.IsA<FNameProperty>() || Property.IsA<FObjectPropertyBase>())
			{
				return MakeNameExpr(TEXT("None"));
			}
			if (Property.IsA<FStrProperty>() || Property.IsA<FTextProperty>())
			{
				return MakeStringExpr(FString());
			}
			if (Property.IsA<FArrayProperty>() || Property.IsA<FSetProperty>())
			{
				return FExprPtr(MakeUnique<Lang::FDstListExpr>());
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
			{
				if (StructProperty->Struct == TBaseStructure<FGameplayTag>::Get())
				{
					return MakeNameExpr(TEXT("None"));
				}
				if (StructProperty->Struct == FGameplayTagContainer::StaticStruct())
				{
					return FExprPtr(MakeUnique<Lang::FDstListExpr>());
				}
				return FExprPtr(MakeUnique<Lang::FDstTupleExpr>());
			}
			return nullptr;
		}
	}

	FExprPtr FDstRaiser::MakeConstantExpr(const FString& ExportText, const FProperty* Property, const FString& What)
	{
		if (Property)
		{
			if (FExprPtr Literal = DstExportTextToLiteral(ExportText, *Property, ValueContext))
			{
				return Literal;
			}
			if (ExportText.IsEmpty())
			{
				if (FExprPtr Zero = MakeRaiseZeroLiteral(*Property))
				{
					return Zero;
				}
			}
		}
		Sink.Error(TEXT("DST8004"), Lang::FDstSpan(),
			FString::Printf(TEXT("The value of %s (`%s`) cannot be written as text."), *What, *ExportText),
			Property ? FString::Printf(TEXT("The property is %s; the engine's export text of it is kept as a string."), *Lowering::DescribeProperty(Property))
				: FString(TEXT("The property is not known, so the engine's export text of it is kept as a string.")));
		return MakeStringExpr(ExportText);
	}

	// ------------------------------------------------------------------------------------------
	// Binding sources
	// ------------------------------------------------------------------------------------------

	FExprPtr FDstRaiser::RaiseSource(const Model::FDstSourceRef& Source, const FScope& Scope, const FString& What)
	{
		TArray<FString> Segments;
		bool bOk = true;
		auto AppendPath = [&](const int32 SkipFirst)
		{
			for (int32 Index = SkipFirst; Index < Source.Path.Segments.Num(); ++Index)
			{
				const Model::FDstPathSegment& Segment = Source.Path.Segments[Index];
				if (Segment.ArrayIndex != INDEX_NONE)
				{
					Sink.Error(TEXT("DST8054"), Lang::FDstSpan(),
						FString::Printf(TEXT("%s reads `%s`, a member of an array element, which the text has no way to name."), *What, *Source.ToString()));
					bOk = false;
					return;
				}
				Segments.Add(Segment.Name);
			}
		};

		switch (Source.Kind)
		{
		case Model::EDstSourceKind::Parameters:
			Segments.Add(TEXT("Params"));
			AppendPath(0);
			break;

		case Model::EDstSourceKind::StateParameters:
			Segments.Add(TEXT("Params"));
			AppendPath(0);
			if (!Source.Path.IsEmpty())
			{
				const FString& ParameterName = Source.Path.Segments[0].Name;
				const bool bAlsoTree = Tree.Parameters.ContainsByPredicate([&ParameterName](const Model::FDstParameter& Parameter)
				{
					return Parameter.Name.Equals(ParameterName, ESearchCase::CaseSensitive);
				});
				if (bAlsoTree)
				{
					Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
						FString::Printf(TEXT("%s reads the parameter `%s` of a subtree, and the tree has a parameter of that name: `Params.%s` cannot tell them apart."), *What, *ParameterName, *ParameterName),
						TEXT("Rename one of the two parameters."));
					bOk = false;
				}
			}
			break;

		case Model::EDstSourceKind::Context:
			Segments.Add(Source.Owner);
			AppendPath(0);
			break;

		case Model::EDstSourceKind::Global:
		case Model::EDstSourceKind::Node:
		{
			const FString Name = FindBindName(Source.Owner);
			if (Name.IsEmpty())
			{
				Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
					FString::Printf(TEXT("%s reads from `%s`, a node that is not in the tree or has no name the text could use."), *What, *Source.Owner));
				bOk = false;
				break;
			}
			if (IsReservedRootName(Name))
			{
				Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
					FString::Printf(TEXT("%s reads from a node called `%s`, which the text reads as something else."), *What, *Name),
					TEXT("Rename the node."));
				bOk = false;
				break;
			}
			Segments.Add(Name);
			AppendPath(0);
			break;
		}

		case Model::EDstSourceKind::StateEvent:
		case Model::EDstSourceKind::TransitionEvent:
		{
			if (Source.Kind == Model::EDstSourceKind::TransitionEvent)
			{
				if (!Scope.TransitionEventOwner.Equals(Source.Owner, ESearchCase::CaseSensitive))
				{
					Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
						FString::Printf(TEXT("%s reads the event of the transition `%s`, which is not the transition it sits in."), *What, *Source.Owner));
					bOk = false;
					break;
				}
				Segments.Add(TEXT("Event"));
			}
			else
			{
				// `Event` is the nearest state's event, or the transition's own in an event transition's conditions; any other
				// state's event is read by naming the state: `Haggle.Event.Offer`.
				const FModelState* Nearest = FindNearestEventState(Scope);
				const bool bImplicit = Scope.TransitionEventOwner.IsEmpty() && Nearest && Nearest->Path.Equals(Source.Owner, ESearchCase::CaseSensitive);
				if (bImplicit)
				{
					Segments.Add(TEXT("Event"));
				}
				else
				{
					const FModelState* Owner = FindStateByPath(Source.Owner);
					const FModelState* FirstByName = nullptr;
					if (Owner)
					{
						for (const FModelState* Frame : Scope.Chain)
						{
							if (Frame->Name.Equals(Owner->Name, ESearchCase::CaseSensitive))
							{
								FirstByName = Frame;
								break;
							}
						}
					}
					if (!Owner || FirstByName != Owner)
					{
						Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
							FString::Printf(TEXT("%s reads the event of `%s`, which is not a state above it that the text can name."), *What, *Source.Owner),
							TEXT("Only the states from the root of the subtree down to the reader can be named."));
						bOk = false;
						break;
					}
					Segments.Add(Owner->Name);
					Segments.Add(TEXT("Event"));
				}
			}

			// The payload's members follow `Event`; `Tag` and `Origin` are the event's own.
			const bool bPayload = !Source.Path.IsEmpty() && Source.Path.Segments[0].Name == TEXT("Payload");
			AppendPath(bPayload ? 1 : 0);
			if (bOk && Segments.Last().Equals(TEXT("Event"), ESearchCase::CaseSensitive))
			{
				Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
					FString::Printf(TEXT("%s reads a whole event payload, and the text reads one of its members (`Event.X`)."), *What));
				bOk = false;
			}
			break;
		}

		default:
			Sink.Error(TEXT("DST8003"), Lang::FDstSpan(), FString::Printf(TEXT("%s reads from a source the text has no name for."), *What));
			bOk = false;
			break;
		}

		if (!bOk || Segments.IsEmpty())
		{
			return nullptr;
		}
		if (Segments.Num() == 1 && Source.Kind != Model::EDstSourceKind::Context)
		{
			// `Params` alone, or a node alone: the lowering needs a member.
			Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
				FString::Printf(TEXT("%s reads a whole `%s`, and the text reads one of its members."), *What, *Segments[0]));
			return nullptr;
		}
		return MakePathExpr(Segments);
	}

	FSourceLeaf FDstRaiser::ResolveSourceLeaf(const Model::FDstSourceRef& Source, const FScope& Scope)
	{
		FSourceLeaf Result;
		const Model::FDstPropertyPath& Path = Source.Path;
		if (Path.IsEmpty())
		{
			return Result;
		}

		// A subtree's parameters live in a bag that has to stay alive while the path is followed.
		Lowering::FParamSet StateSet;

		const FProperty* Current = nullptr;
		int32 Next = 1;
		switch (Source.Kind)
		{
		case Model::EDstSourceKind::Parameters:
			Current = bTreeParamsBuilt ? TreeParams.FindProperty(Path.Segments[0].Name) : nullptr;
			break;

		case Model::EDstSourceKind::StateParameters:
			if (const FModelState* Owner = FindStateByPath(Source.Owner))
			{
				StateSet.Params = Owner->Parameters;
				FString Error;
				if (StateSet.Build(Error))
				{
					Current = StateSet.FindProperty(Path.Segments[0].Name);
				}
			}
			break;

		case Model::EDstSourceKind::Context:
		{
			const UStateTreeSchema* Defaults = SchemaClass ? Cast<UStateTreeSchema>(SchemaClass->GetDefaultObject()) : nullptr;
			if (Defaults)
			{
				for (const FStateTreeExternalDataDesc& Desc : Defaults->GetContextDataDescs())
				{
					if (Desc.Name.ToString().Equals(Source.Owner, ESearchCase::CaseSensitive) && Desc.Struct)
					{
						Current = Desc.Struct->FindPropertyByName(FName(*Path.Segments[0].Name));
					}
				}
			}
			break;
		}

		case Model::EDstSourceKind::Global:
		case Model::EDstSourceKind::Node:
			if (const FModelNode* Owner = FindNodeByKey(Source.Owner))
			{
				const FNodeInfoPtr Info = GetNodeInfo(*Owner);
				Current = Info->InstanceStruct ? Info->InstanceStruct->FindPropertyByName(FName(*Path.Segments[0].Name)) : nullptr;
			}
			break;

		case Model::EDstSourceKind::StateEvent:
		case Model::EDstSourceKind::TransitionEvent:
			if (Path.Segments[0].Name == TEXT("Payload") && Path.Segments.Num() > 1 && !Path.Segments[0].InstanceType.IsEmpty())
			{
				if (const UScriptStruct* Payload = FindStructByPath(Path.Segments[0].InstanceType))
				{
					Current = Payload->FindPropertyByName(FName(*Path.Segments[1].Name));
					Next = 2;
				}
			}
			else if (Path.Segments.Num() == 1)
			{
				Current = FStateTreeEvent::StaticStruct()->FindPropertyByName(FName(*Path.Segments[0].Name));
			}
			break;

		default:
			break;
		}

		// The rest of the path walks into structs and objects.
		for (int32 Index = Next; Current && Index < Path.Segments.Num(); ++Index)
		{
			const UStruct* Inner = nullptr;
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Current))
			{
				Inner = StructProperty->Struct;
			}
			else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Current))
			{
				Inner = ObjectProperty->PropertyClass;
			}
			Current = Inner ? Inner->FindPropertyByName(FName(*Path.Segments[Index].Name)) : nullptr;
		}

		if (Current)
		{
			Result.bKnown = true;
			Result.Domain = Lowering::GetValueDomain(Current);
			Result.Enum = Lowering::GetPropertyEnum(Current);
		}
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// Arguments
	// ------------------------------------------------------------------------------------------

	FExprPtr FDstRaiser::RaiseWholeArg(const FModelArg& Arg, const FProperty* Property, const FScope& Scope, bool& bOutOutput, const FString& Display)
	{
		switch (Arg.Kind)
		{
		case Model::EDstValueKind::Constant:
			return MakeConstantExpr(Arg.ConstantText, Property, Display);

		case Model::EDstValueKind::Binding:
			return RaiseSource(Arg.Source, Scope, Display);

		case Model::EDstValueKind::Ref:
		{
			FExprPtr Target = RaiseSource(Arg.Source, Scope, Display);
			if (!Target)
			{
				return nullptr;
			}
			TUniquePtr<Lang::FDstRefExpr> Ref = MakeUnique<Lang::FDstRefExpr>();
			Ref->Target = MoveTemp(Target);
			return FExprPtr(MoveTemp(Ref));
		}

		case Model::EDstValueKind::Output:
			bOutOutput = true;
			return RaiseSource(Arg.Source, Scope, Display);

		case Model::EDstValueKind::Function:
			if (!Arg.Function.IsValid())
			{
				Sink.Error(TEXT("DST8054"), Lang::FDstSpan(), FString::Printf(TEXT("%s is fed by a property function the model does not hold."), *Display));
				return nullptr;
			}
			return RaiseFunction(*Arg.Function, Property, Scope);
		}
		return nullptr;
	}

	namespace
	{
		/** The argument of a call or the item of a tuple called Name; a new, empty one at the end when there is none. */
		Lang::FDstArgument& FindOrAddRaiseArgument(TArray<Lang::FDstArgumentPtr>& Arguments, const FString& Name)
		{
			for (const Lang::FDstArgumentPtr& Item : Arguments)
			{
				if (Item && Item->Name.Equals(Name, ESearchCase::CaseSensitive))
				{
					return *Item;
				}
			}
			Arguments.Add(MakeArgument(Name, nullptr));
			return *Arguments.Last();
		}

		/** The items of a tuple in the order the struct declares its fields in; an item that names no field stays after them. */
		void SortRaiseTupleItems(TArray<Lang::FDstArgumentPtr>& Items, const UStruct& Struct)
		{
			const TArray<const FProperty*> Fields = GetDstTupleFields(Struct);
			const auto OrderOf = [&Fields](const Lang::FDstArgumentPtr& Item)
			{
				for (int32 Index = 0; Index < Fields.Num(); ++Index)
				{
					if (Item && Fields[Index]->GetName().Equals(Item->Name, ESearchCase::CaseSensitive))
					{
						return Index;
					}
				}
				return Fields.Num();
			};
			Items.StableSort([&OrderOf](const Lang::FDstArgumentPtr& A, const Lang::FDstArgumentPtr& B) { return OrderOf(A) < OrderOf(B); });
		}

		/** The object path of the tree a reference's text names: its callee (or the whole text) is a name or a Path(...). */
		bool ResolveRaiseTreePath(const Lang::FDstExpr& AssetExpr, FString& OutObjectPath)
		{
			if (TryGetDstAssetPath(AssetExpr, OutObjectPath))
			{
				return true;
			}
			FString Error;
			if (const Lang::FDstNameExpr* Name = AssetExpr.As<Lang::FDstNameExpr>())
			{
				return FindDstAsset(Name->Name, UStateTree::StaticClass(), OutObjectPath, Error);
			}
			if (const Lang::FDstLiteralExpr* Literal = AssetExpr.As<Lang::FDstLiteralExpr>();
				Literal && Literal->LiteralKind == Lang::EDstLiteralKind::String && Literal->Text.StartsWith(TEXT("/")))
			{
				OutObjectPath = Literal->Text;
				return true;
			}
			return false;
		}
	}

	bool FDstRaiser::InsertDeepArg(FExprPtr& Slot, const FProperty* Property, const TArray<Model::FDstPathSegment>& Segments, const int32 Index,
		const FModelArg& Arg, const FScope& Scope, const FString& Display)
	{
		const Model::FDstPathSegment& Segment = Segments[Index];
		auto Fail = [this, &Display, &Arg](const TCHAR* Why)
		{
			Sink.Error(TEXT("DST8054"), Lang::FDstSpan(),
				FString::Printf(TEXT("%s is fed through `%s`, which the text has no way to write: %s"), *Display, *Arg.Property.ToString(), Why));
			return false;
		};
		if (!Property || !Segment.InstanceType.IsEmpty())
		{
			return Fail(TEXT("the path enters a value whose type only the instance knows."));
		}

		// An element of an array: the constant has the list the element sits in.
		const FProperty* Element = Property;
		FExprPtr* ElementSlot = &Slot;
		if (Segment.ArrayIndex != INDEX_NONE)
		{
			const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
			Lang::FDstListExpr* List = Slot ? Slot->As<Lang::FDstListExpr>() : nullptr;
			if (!ArrayProperty || !List || !List->Items.IsValidIndex(Segment.ArrayIndex) || !List->Items[Segment.ArrayIndex])
			{
				return Fail(TEXT("the constant has no such element."));
			}
			Element = ArrayProperty->Inner;
			ElementSlot = &List->Items[Segment.ArrayIndex];
		}

		if (Index + 1 == Segments.Num())
		{
			bool bOutput = false;
			FExprPtr Value = RaiseWholeArg(Arg, Element, Scope, bOutput, Display);
			if (!Value)
			{
				return false;
			}
			*ElementSlot = MoveTemp(Value);
			return true;
		}

		const Model::FDstPathSegment& Next = Segments[Index + 1];
		const FStructProperty* StructProperty = CastField<FStructProperty>(Element);
		if (!StructProperty || !StructProperty->Struct)
		{
			return Fail(TEXT("the value has no members."));
		}

		// A reference to a tree: `Parameters`, `Value<PropertyBag>`, the parameter. The text names the parameter in the call.
		if (StructProperty->Struct == FStateTreeReference::StaticStruct())
		{
			const bool bShape = Segments.Num() >= Index + 4 && Next.Name.Equals(TEXT("Parameters"), ESearchCase::CaseSensitive)
				&& Segments[Index + 2].Name.Equals(TEXT("Value"), ESearchCase::CaseSensitive)
				&& Segments[Index + 2].InstanceType.Equals(Model::PropertyBagInstanceType, ESearchCase::CaseSensitive);
			if (!bShape || !*ElementSlot)
			{
				return Fail(TEXT("a path into a reference to a tree goes through Parameters.Value<PropertyBag> and a parameter."));
			}

			FString TreePath;
			Lang::FDstCallExpr* Call = (*ElementSlot)->As<Lang::FDstCallExpr>();
			const Lang::FDstExpr* AssetExpr = ElementSlot->Get();
			if (Call && !TryGetDstAssetPath(*AssetExpr, TreePath))
			{
				AssetExpr = Call->Callee.Get();
			}
			if (!AssetExpr || !ResolveRaiseTreePath(*AssetExpr, TreePath))
			{
				return Fail(TEXT("the reference names no tree."));
			}
			const FString& ParameterName = Segments[Index + 3].Name;
			const FProperty* Parameter = FindDstTreeParameter(TreePath, ParameterName);
			if (!Parameter)
			{
				return Fail(TEXT("the tree has no such parameter."));
			}
			if (!Call)
			{
				// Only the tree so far: `ST_Shoot` becomes `ST_Shoot(Parameter = ...)`.
				TUniquePtr<Lang::FDstCallExpr> Wrapped = MakeUnique<Lang::FDstCallExpr>();
				Call = Wrapped.Get();
				Call->Callee = MoveTemp(*ElementSlot);
				*ElementSlot = FExprPtr(MoveTemp(Wrapped));
			}
			Lang::FDstArgument& ParameterArgument = FindOrAddRaiseArgument(Call->Args, ParameterName);
			return InsertDeepArg(ParameterArgument.Value, Parameter, Segments, Index + 3, Arg, Scope, Display);
		}

		const FProperty* Member = StructProperty->Struct->FindPropertyByName(FName(*Next.Name));
		if (!Member)
		{
			return Fail(TEXT("the struct has no such member."));
		}
		if (!*ElementSlot)
		{
			*ElementSlot = FExprPtr(MakeUnique<Lang::FDstTupleExpr>());
		}
		Lang::FDstTupleExpr* Tuple = (*ElementSlot)->As<Lang::FDstTupleExpr>();
		if (!Tuple)
		{
			return Fail(TEXT("the constant of the struct is not a tuple."));
		}
		const int32 NumBefore = Tuple->Items.Num();
		Lang::FDstArgument& Item = FindOrAddRaiseArgument(Tuple->Items, Next.Name);
		if (Tuple->Items.Num() != NumBefore)
		{
			// A field the constant leaves out because it holds the default goes where the struct declares it.
			SortRaiseTupleItems(Tuple->Items, *StructProperty->Struct);
		}
		return InsertDeepArg(Item.Value, Member, Segments, Index + 1, Arg, Scope, Display);
	}

	FExprPtr FDstRaiser::RaiseMixedArg(const FProperty* Property, const FModelArg* Whole, const TArray<const FModelArg*>& Deep, const FScope& Scope,
		const FString& Display, const void* DefaultContainer)
	{
		const bool bAggregate = Property && (Property->IsA<FStructProperty>() || Property->IsA<FArrayProperty>());
		if (!bAggregate)
		{
			Sink.Error(TEXT("DST8054"), Lang::FDstSpan(),
				FString::Printf(TEXT("%s is fed in parts, and it is not a struct or an array, so the text has no tuple or list to write for it."), *Display));
			return nullptr;
		}

		// The constant first: the whole property with the parts fed from elsewhere at their defaults. When the model has none, the
		// property is at its default; a struct or array at the plain default starts from nothing written.
		FExprPtr Base;
		if (Whole && Whole->Kind == Model::EDstValueKind::Constant)
		{
			Base = MakeConstantExpr(Whole->ConstantText, Property, Display);
		}
		else
		{
			if (Whole)
			{
				Sink.Error(TEXT("DST8054"), Lang::FDstSpan(),
					FString::Printf(TEXT("%s is fed as a whole and in parts at once; the parts are written and the whole is not."), *Display));
			}
			const FString DefaultText = DstExportDefault(*Property, DefaultContainer);
			if (!IsDstStateTreeReferenceProperty(*Property) && DefaultText.Equals(DstExportDefault(*Property, nullptr), ESearchCase::CaseSensitive))
			{
				Base = Property->IsA<FArrayProperty>() ? FExprPtr(MakeUnique<Lang::FDstListExpr>()) : FExprPtr(MakeUnique<Lang::FDstTupleExpr>());
			}
			else
			{
				Base = MakeConstantExpr(DefaultText, Property, Display);
			}
		}

		for (const FModelArg* Arg : Deep)
		{
			const FString FieldDisplay = Display + TEXT(".") + Arg->Property.ToString();
			if (Arg->Kind == Model::EDstValueKind::Output)
			{
				Sink.Error(TEXT("DST8054"), Lang::FDstSpan(),
					FString::Printf(TEXT("%s is fed through `%s`, which the text has no way to write."), *Display, *Arg->Property.ToString()));
				continue;
			}
			InsertDeepArg(Base, Property, Arg->Property.Segments, 0, *Arg, Scope, FieldDisplay);
		}
		return Base;
	}

	FExprPtr FDstRaiser::RaiseArgGroup(const FProperty* Property, const FString& PropertyName, const TArray<const FModelArg*>& Group,
		const FScope& Scope, bool& bOutOutput, const FString& Display, const void* DefaultContainer)
	{
		bOutOutput = false;
		const FModelArg* Whole = nullptr;
		TArray<const FModelArg*> Deep;
		for (const FModelArg* Arg : Group)
		{
			if (Arg->Property.Segments.Num() == 1 && Arg->Property.Segments[0].ArrayIndex == INDEX_NONE)
			{
				Whole = Arg;
			}
			else
			{
				Deep.Add(Arg);
			}
		}

		if (Deep.IsEmpty())
		{
			return Whole ? RaiseWholeArg(*Whole, Property, Scope, bOutOutput, Display) : nullptr;
		}
		return RaiseMixedArg(Property, Whole, Deep, Scope, Display, DefaultContainer);
	}

	// ------------------------------------------------------------------------------------------
	// A call
	// ------------------------------------------------------------------------------------------

	FExprPtr FDstRaiser::RaiseNodeCall(const FModelNode& Node, const FScope& Scope, const FCallStyle& Style)
	{
		const FNodeInfoPtr Info = GetNodeInfo(Node);
		TUniquePtr<Lang::FDstCallExpr> Call = MakeUnique<Lang::FDstCallExpr>();
		Call->Callee = MakeCalleeExpr(Node, *Info);

		// A constant of the node struct's own that is not a setting reads as an argument (`DebugText("Dead")`), unless
		// the instance data has a property of the same name, which an argument would mean instead.
		auto IsNodeArgument = [&Info, &Style](const FModelArg& Arg)
		{
			if (Arg.Side != Model::EDstPropertySide::Node || Arg.Kind != Model::EDstValueKind::Constant || Arg.Property.Segments.Num() != 1)
			{
				return false;
			}
			const FString& Name = Arg.Property.Segments[0].Name;
			if (IsRaiseSettingProperty(Name) || (Style.bSkipInvert && Name.Equals(Sugar::InvertProperty, ESearchCase::CaseSensitive)))
			{
				return false;
			}
			if (!Info->NodeStruct || !Info->NodeStruct->FindPropertyByName(FName(*Name)))
			{
				return false;
			}
			return !Info->InstanceStruct || !Info->InstanceStruct->FindPropertyByName(FName(*Name));
		};

		// The arguments by the top-level property they feed: the instance data's, then the node struct's own.
		struct FGroup
		{
			FString Name;
			const FProperty* Property = nullptr;
			int32 Order = MAX_int32;
			/** The group is a property of the node struct itself (an argument of it), not of the instance data. */
			bool bNodeSide = false;
			TArray<const FModelArg*> Args;
		};
		// A Blueprint task keeps ConsideredForCompletion on its instance object, but the text writes it as a setting.
		auto IsInstanceSetting = [&Info](const FModelArg& Arg)
		{
			return Info->bBlueprint && Arg.Side == Model::EDstPropertySide::Instance && Arg.Property.Segments.Num() == 1
				&& Arg.Property.Segments[0].Name.Equals(TEXT("bConsideredForCompletion"), ESearchCase::CaseSensitive);
		};

		TArray<FGroup> Groups;
		for (const FModelArg& Arg : Node.Args)
		{
			if (IsInstanceSetting(Arg))
			{
				continue;
			}
			const bool bNodeArgument = IsNodeArgument(Arg);
			if ((Arg.Side != Model::EDstPropertySide::Instance && !bNodeArgument) || Arg.Property.IsEmpty())
			{
				continue;
			}
			const FString& Name = Arg.Property.Segments[0].Name;
			FGroup* Group = Groups.FindByPredicate([&Name](const FGroup& Existing) { return Existing.Name.Equals(Name, ESearchCase::CaseSensitive); });
			if (!Group)
			{
				Group = &Groups.AddDefaulted_GetRef();
				Group->Name = Name;
				const UStruct* Owner = bNodeArgument ? static_cast<const UStruct*>(Info->NodeStruct) : static_cast<const UStruct*>(Info->InstanceStruct);
				Group->Property = Owner ? Owner->FindPropertyByName(FName(*Name)) : nullptr;
				Group->bNodeSide = bNodeArgument;
			}
			Group->Args.Add(&Arg);
		}

		// The order the structs declare their properties in: the instance data's first.
		int32 Order = 0;
		for (const UStruct* Owner : { static_cast<const UStruct*>(Info->InstanceStruct), static_cast<const UStruct*>(Info->NodeStruct) })
		{
			if (!Owner)
			{
				continue;
			}
			for (TFieldIterator<FProperty> It(Owner); It; ++It, ++Order)
			{
				for (FGroup& Group : Groups)
				{
					if (Group.Property == *It && Group.Order == MAX_int32)
					{
						Group.Order = Order;
					}
				}
			}
		}
		Groups.StableSort([](const FGroup& A, const FGroup& B) { return A.Order < B.Order; });

		auto HasOutput = [](const FGroup& Group)
		{
			return Group.Args.ContainsByPredicate([](const FModelArg* Arg) { return Arg->Kind == Model::EDstValueKind::Output; });
		};
		auto FindGroup = [&Groups](const FString& Name) -> FGroup*
		{
			return Groups.FindByPredicate([&Name](const FGroup& Group) { return Group.Name.Equals(Name, ESearchCase::CaseSensitive); });
		};

		// Positional: the type's declared positions, from the first, while each has a value.
		TArray<FGroup*> Positional;
		if (Info->Type)
		{
			for (const FName& PositionName : Info->Type->PositionalArgs)
			{
				FGroup* Group = FindGroup(PositionName.ToString());
				if (!Group || HasOutput(*Group))
				{
					break;
				}
				Positional.Add(Group);
			}
		}

		TSet<const FGroup*> Done;
		for (FGroup* Group : Positional)
		{
			bool bOutput = false;
			FExprPtr Value = RaiseArgGroup(Group->Property, Group->Name, Group->Args, Scope, bOutput,
				FString::Printf(TEXT("%s of %s"), *Group->Name, *Info->DisplayName),
				Group->bNodeSide ? Info->GetNodeDefaults() : Info->GetInstanceDefaults());
			if (!Value)
			{
				// A gap: what is left goes by name, so no later argument shifts into this one's place.
				break;
			}
			Call->Args.Add(MakeArgument(FString(), MoveTemp(Value)));
			Done.Add(Group);
		}

		// Named: a bool drops its `b` when no other property answers to what is left.
		for (const FGroup& Group : Groups)
		{
			if (Done.Contains(&Group))
			{
				continue;
			}
			bool bOutput = false;
			FExprPtr Value = RaiseArgGroup(Group.Property, Group.Name, Group.Args, Scope, bOutput,
				FString::Printf(TEXT("%s of %s"), *Group.Name, *Info->DisplayName),
				Group.bNodeSide ? Info->GetNodeDefaults() : Info->GetInstanceDefaults());
			if (!Value)
			{
				continue;
			}

			FString WrittenName = Group.Name;
			if (Group.Property && Group.Property->IsA<FBoolProperty>() && IsRaiseBoolName(Group.Name))
			{
				const FString Stripped = Group.Name.RightChop(1);
				bool bTaken = (Info->InstanceStruct && Info->InstanceStruct->FindPropertyByName(FName(*Stripped)))
					|| (Info->NodeStruct && Info->NodeStruct->FindPropertyByName(FName(*Stripped)));
				for (const FGroup& Other : Groups)
				{
					bTaken |= Other.Name.Equals(Stripped, ESearchCase::IgnoreCase);
				}
				if (!bTaken)
				{
					WrittenName = Stripped;
				}
			}
			Call->Args.Add(MakeArgument(WrittenName, MoveTemp(Value), bOutput));
		}

		// Settings: the node struct's own properties, and the name when it has no place outside.
		struct FSetting
		{
			int32 Order = 0;
			FString Key;
			FExprPtr Value;
		};
		TArray<FSetting> Settings;
		if (Style.bNameInSettings && !Node.BindName.IsEmpty())
		{
			FSetting& Setting = Settings.AddDefaulted_GetRef();
			Setting.Order = -1;
			Setting.Key = TEXT("Name");
			Setting.Value = MakeStringExpr(Node.BindName);
		}
		for (const FModelArg& Arg : Node.Args)
		{
			const bool bInstanceSetting = IsInstanceSetting(Arg);
			if ((Arg.Side != Model::EDstPropertySide::Node && !bInstanceSetting) || Arg.Property.IsEmpty() || IsNodeArgument(Arg))
			{
				continue;
			}
			const FString& Name = Arg.Property.Segments[0].Name;
			if (Style.bSkipInvert && Name.Equals(Sugar::InvertProperty, ESearchCase::CaseSensitive))
			{
				continue;
			}
			const FString Display = FString::Printf(TEXT("%s of %s"), *Name, *Info->DisplayName);
			const UStruct* SettingOwner = bInstanceSetting ? static_cast<const UStruct*>(Info->InstanceStruct) : static_cast<const UStruct*>(Info->NodeStruct);
			const FProperty* Property = SettingOwner ? SettingOwner->FindPropertyByName(FName(*Name)) : nullptr;
			if (!Property)
			{
				Sink.Error(TEXT("DST8053"), Lang::FDstSpan(), FString::Printf(TEXT("%s is not a property of the node struct, so it cannot be written as a setting."), *Display));
				continue;
			}
			if (Arg.Property.Segments.Num() != 1 || Arg.Kind != Model::EDstValueKind::Constant)
			{
				Sink.Error(TEXT("DST8054"), Lang::FDstSpan(), FString::Printf(TEXT("%s is a setting of the node struct and is fed from elsewhere; the text writes settings as constants."), *Display));
				continue;
			}
			FSetting& Setting = Settings.AddDefaulted_GetRef();
			Setting.Order = GetRaiseSettingOrder(Name);
			Setting.Key = GetRaiseSettingKey(Name);
			Setting.Value = MakeConstantExpr(Arg.ConstantText, Property, Display);
		}
		Settings.StableSort([](const FSetting& A, const FSetting& B)
		{
			return A.Order != B.Order ? A.Order < B.Order : A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0;
		});
		for (FSetting& Setting : Settings)
		{
			AddMetaEntry(Call->Meta, Setting.Key, MoveTemp(Setting.Value));
		}

		return FExprPtr(MoveTemp(Call));
	}

	TUniquePtr<Lang::FDstTaskItem> FDstRaiser::RaiseTask(const FModelNode& Node, const FScope& Scope)
	{
		TUniquePtr<Lang::FDstTaskItem> Item = MakeUnique<Lang::FDstTaskItem>();
		Item->BindName = Node.BindName;
		// A schema with one task per state names the task after its state; the lowering gives it that name again.
		if (bSingleTaskSchema && !Scope.Chain.IsEmpty() && Node.BindName.Equals(Scope.Chain.Last()->Name, ESearchCase::CaseSensitive))
		{
			Item->BindName.Reset();
		}

		FCallStyle Style;
		Style.bNameInSettings = false;
		Item->Node = RaiseNodeCall(Node, Scope, Style);
		return Item;
	}

	// ------------------------------------------------------------------------------------------
	// Property functions
	// ------------------------------------------------------------------------------------------

	FExprPtr FDstRaiser::RaiseFunction(const FModelNode& Function, const FProperty* Target, const FScope& Scope)
	{
		if (FExprPtr Operation = TryRaiseOperation(Function, Target, Scope))
		{
			return Operation;
		}
		FCallStyle Style;
		Style.bNameInSettings = true;
		return RaiseNodeCall(Function, Scope, Style);
	}

	FExprPtr FDstRaiser::TryRaiseOperation(const FModelNode& Function, const FProperty* Target, const FScope& Scope)
	{
		if (!Function.BindName.IsEmpty())
		{
			return nullptr;
		}

		// Which of the sugar's functions it is, and what the property it feeds has to be for the lowering to pick it again.
		const Sugar::FBinaryFunction* Binary = nullptr;
		const Sugar::FUnaryFunction* Unary = nullptr;
		Lowering::EValueDomain Wanted = Lowering::EValueDomain::Other;
		const auto FindIn = [&](const Sugar::FBinaryFunction* Table, const int32 Count, const Lowering::EValueDomain Domain)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Function.StructType.Equals(Table[Index].StructPath, ESearchCase::IgnoreCase))
				{
					Binary = &Table[Index];
					Wanted = Domain;
				}
			}
		};
		FindIn(Sugar::FloatFunctions, UE_ARRAY_COUNT(Sugar::FloatFunctions), Lowering::EValueDomain::Float);
		FindIn(Sugar::IntFunctions, UE_ARRAY_COUNT(Sugar::IntFunctions), Lowering::EValueDomain::Int);
		FindIn(Sugar::BooleanFunctions, UE_ARRAY_COUNT(Sugar::BooleanFunctions), Lowering::EValueDomain::Bool);
		if (!Binary)
		{
			for (const Sugar::FUnaryFunction& Candidate : Sugar::UnaryFunctions)
			{
				if (Function.StructType.Equals(Candidate.StructPath, ESearchCase::IgnoreCase))
				{
					Unary = &Candidate;
					const FString Path = Candidate.StructPath;
					Wanted = Path.Contains(TEXT("Float")) ? Lowering::EValueDomain::Float : Path.Contains(TEXT("Int")) ? Lowering::EValueDomain::Int : Lowering::EValueDomain::Bool;
				}
			}
		}
		if (!Binary && !Unary)
		{
			return nullptr;
		}
		if (Target && Lowering::GetValueDomain(Target) != Wanted)
		{
			return nullptr;
		}

		// Only the operands: anything else on the node needs the call form.
		const TCHAR* FirstName = Binary ? Binary->Left : Unary->Input;
		const TCHAR* SecondName = Binary ? Binary->Right : nullptr;
		bool bAnyDynamic = false;
		for (const FModelArg& Arg : Function.Args)
		{
			if (Arg.Side != Model::EDstPropertySide::Instance || Arg.Property.Segments.Num() != 1)
			{
				return nullptr;
			}
			const FString& Name = Arg.Property.Segments[0].Name;
			const bool bIsOperand = Name.Equals(FirstName, ESearchCase::CaseSensitive) || (SecondName && Name.Equals(SecondName, ESearchCase::CaseSensitive));
			if (!bIsOperand)
			{
				return nullptr;
			}
			switch (Arg.Kind)
			{
			case Model::EDstValueKind::Constant:
				break;
			case Model::EDstValueKind::Binding:
			case Model::EDstValueKind::Function:
				bAnyDynamic = true;
				break;
			default:
				return nullptr;
			}
		}
		// An operation of constants is folded into one constant by the lowering, so it cannot come back as an operation.
		if (!bAnyDynamic)
		{
			return nullptr;
		}

		const FNodeInfoPtr Info = GetNodeInfo(Function);
		if (!Info->InstanceStruct)
		{
			return nullptr;
		}

		const auto MakeOperand = [&](const TCHAR* PropertyName) -> FExprPtr
		{
			const FProperty* Property = Info->InstanceStruct->FindPropertyByName(PropertyName);
			if (!Property)
			{
				return nullptr;
			}
			const FModelArg* Found = Function.Args.FindByPredicate([PropertyName](const FModelArg& Arg)
			{
				return Arg.Property.Segments[0].Name.Equals(PropertyName, ESearchCase::CaseSensitive);
			});
			const FString Display = FString::Printf(TEXT("%s of %s"), PropertyName, *Info->DisplayName);
			if (!Found)
			{
				// Left at its default, which the operation has to say.
				return MakeConstantExpr(DstExportDefault(*Property, Info->GetInstanceDefaults()), Property, Display);
			}
			bool bOutput = false;
			return RaiseWholeArg(*Found, Property, Scope, bOutput, Display);
		};

		FExprPtr First = MakeOperand(FirstName);
		FExprPtr Second = SecondName ? MakeOperand(SecondName) : nullptr;
		if (!First || (SecondName && !Second))
		{
			return nullptr;
		}

		if (Unary)
		{
			return MakeUnaryExpr(FCString::Strcmp(Unary->Spelling, TEXT("not")) == 0 ? Lang::EDstUnaryOp::Not : Lang::EDstUnaryOp::Negate, MoveTemp(First));
		}

		Lang::EDstBinaryOp Op = Lang::EDstBinaryOp::Add;
		const TCHAR* Spelling = Binary->Spelling;
		if (FCString::Strcmp(Spelling, TEXT("+")) == 0) { Op = Lang::EDstBinaryOp::Add; }
		else if (FCString::Strcmp(Spelling, TEXT("-")) == 0) { Op = Lang::EDstBinaryOp::Subtract; }
		else if (FCString::Strcmp(Spelling, TEXT("*")) == 0) { Op = Lang::EDstBinaryOp::Multiply; }
		else if (FCString::Strcmp(Spelling, TEXT("/")) == 0) { Op = Lang::EDstBinaryOp::Divide; }
		else if (FCString::Strcmp(Spelling, TEXT("and")) == 0) { Op = Lang::EDstBinaryOp::And; }
		else { Op = Lang::EDstBinaryOp::Or; }
		return MakeBinaryExpr(Op, MoveTemp(First), MoveTemp(Second));
	}
}
