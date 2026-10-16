// Copyright DreamMoon. All Rights Reserved.
//
// Node calls: which node type a name means (by the category of the place it is written), whether the schema allows it,
// its arguments (positional, named, output) and settings, and the checks the engine's compile would make on the result:
// Input properties bound, Output properties never given a value, Context properties with a matching context.

#include "DstLowerPrivate.h"

#include "StateTreeSchema.h"
#include "StateTreeExecutionTypes.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	using namespace Lang;
	using namespace Model;
	using Model::FDstNode;

	namespace
	{
		const TCHAR* CategoryWord(const EDstNodeCategory Category)
		{
			switch (Category)
			{
			case EDstNodeCategory::Task: return TEXT("task");
			case EDstNodeCategory::Condition: return TEXT("condition");
			case EDstNodeCategory::Evaluator: return TEXT("evaluator");
			case EDstNodeCategory::Consideration: return TEXT("utility consideration");
			case EDstNodeCategory::PropertyFunction: return TEXT("property function");
			default: return TEXT("node");
			}
		}

		/** Properties the compile fills in or the engine owns: never arguments. */
		bool IsNodeBaseOwner(const UStruct* Owner)
		{
			if (!Owner)
			{
				return false;
			}
			const FString Name = Owner->GetName();
			return Name.Equals(TEXT("StateTreeNodeBase"), ESearchCase::CaseSensitive) || Name.Equals(TEXT("StateTreeTaskBase"), ESearchCase::CaseSensitive) || Name.Equals(TEXT("StateTreeConditionBase"), ESearchCase::CaseSensitive)
				|| Name.Equals(TEXT("StateTreeConsiderationBase"), ESearchCase::CaseSensitive) || Name.Equals(TEXT("StateTreeEvaluatorBase"), ESearchCase::CaseSensitive) || Name.Equals(TEXT("StateTreePropertyFunctionBase"), ESearchCase::CaseSensitive);
		}

		bool IsInstanceArgumentCandidate(const FNodeInfo& Info, const FProperty* Property)
		{
			if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated))
			{
				return false;
			}
			if (Info.bBlueprint)
			{
				// A Blueprint node's variables, and the editable properties of a C++ class it derives from, but not what the
				// StateTree base classes declare (their settings go in `[ ]`).
				const UClass* Owner = Cast<UClass>(Property->GetOwnerStruct());
				if (!Owner)
				{
					return false;
				}
				if (!Owner->IsNative())
				{
					return true;
				}
				const FString OwnerName = Owner->GetName();
				const bool bEngineBase = OwnerName.Equals(TEXT("StateTreeNodeBlueprintBase"), ESearchCase::CaseSensitive)
					|| OwnerName.Equals(TEXT("StateTreeTaskBlueprintBase"), ESearchCase::CaseSensitive)
					|| OwnerName.Equals(TEXT("StateTreeConditionBlueprintBase"), ESearchCase::CaseSensitive)
					|| OwnerName.Equals(TEXT("StateTreeEvaluatorBlueprintBase"), ESearchCase::CaseSensitive)
					|| OwnerName.Equals(TEXT("StateTreeConsiderationBlueprintBase"), ESearchCase::CaseSensitive);
				return !bEngineBase && Property->HasAnyPropertyFlags(CPF_Edit);
			}
			return true;
		}

		bool IsNodeArgumentCandidate(const FProperty* Property, const bool bAllowBase)
		{
			if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated))
			{
				return false;
			}
			if (!bAllowBase && IsNodeBaseOwner(Property->GetOwnerStruct()))
			{
				return false;
			}
			return bAllowBase || Property->HasAnyPropertyFlags(CPF_Edit);
		}

		FString GivenKey(const EDstPropertySide Side, const FProperty& Property)
		{
			return FString::Printf(TEXT("%s:%s"), Side == EDstPropertySide::Instance ? TEXT("I") : TEXT("N"), *Property.GetName());
		}

		/** Names of the properties a node accepts as arguments, for suggestions. */
		TArray<FString> ArgumentNames(const FNodeInfo& Info)
		{
			TArray<FString> Names;
			if (Info.InstanceStruct)
			{
				for (TFieldIterator<FProperty> It(Info.InstanceStruct); It; ++It)
				{
					if (IsInstanceArgumentCandidate(Info, *It) && GetPropertyUse(**It) != EPropertyUse::Context)
					{
						Names.Add(It->GetName());
					}
				}
			}
			if (Info.NodeStruct)
			{
				for (TFieldIterator<FProperty> It(Info.NodeStruct); It; ++It)
				{
					if (IsNodeArgumentCandidate(*It, false))
					{
						Names.AddUnique(It->GetName());
					}
				}
			}
			return Names;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Which node
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::ResolveNodeType(const FDstExpr& Callee, const EDstNodeCategory Expected, const EDstNodeCategory AlsoExpected,
		const FDstSpan& Span, const FDstNodeType*& OutType, bool* bOutWrongKind)
	{
		OutType = nullptr;
		if (bOutWrongKind)
		{
			*bOutWrongKind = false;
		}
		FDstNameRegistry& Registry = FDstNameRegistry::Get();
		auto IsExpected = [Expected, AlsoExpected](const FDstNodeType* Type)
		{
			return Type && (Type->Category == Expected || (AlsoExpected != EDstNodeCategory::Unknown && Type->Category == AlsoExpected));
		};
		const FString ExpectedWord = AlsoExpected != EDstNodeCategory::Unknown
			? FString::Printf(TEXT("%s or %s"), CategoryWord(Expected), CategoryWord(AlsoExpected))
			: FString(CategoryWord(Expected));

		// `Path(Game, "AI/STT_Wander")`: a Blueprint node by its asset.
		FString AssetPath;
		if (TryGetDstAssetPath(Callee, AssetPath))
		{
			const UClass* Class = ResolveClassByName(AssetPath + TEXT("_C"));
			if (!Class)
			{
				Class = ResolveClassByName(AssetPath);
			}
			const FDstNodeType* Found = Class ? Registry.FindByBlueprintClass(Class) : nullptr;
			if (!Found)
			{
				Sink.Error(TEXT("DST4001"), Span, FString::Printf(TEXT("`%s` is not a Blueprint node."), *AssetPath),
					TEXT("Write the Blueprint task, condition or evaluator by its asset: Path(Game, \"AI/STT_Wander\")."));
				return false;
			}
			if (!IsExpected(Found))
			{
				if (bOutWrongKind)
				{
					*bOutWrongKind = true;
					return false;
				}
				Sink.Error(TEXT("DST4021"), Span, FString::Printf(TEXT("`%s` is a %s, but a %s is expected here."), *AssetPath, CategoryWord(Found->Category), *ExpectedWord));
				return false;
			}
			OutType = Found;
			return true;
		}

		const FString Written = GetDstPathString(Callee);
		if (Written.IsEmpty())
		{
			Sink.Error(TEXT("DST4001"), Span, FString::Printf(TEXT("Expected the name of a %s."), *ExpectedWord));
			return false;
		}

		TArray<FDstNameMatch> Shadowed;
		TArray<FDstNameMatch> All = Registry.ResolveNode(Written, Namespaces, &Shadowed);
		All.Append(Shadowed);

		TArray<FDstNameMatch> Filtered;
		for (const FDstNameMatch& Match : All)
		{
			if (IsExpected(Match.Type))
			{
				bool bSeen = false;
				for (const FDstNameMatch& Existing : Filtered)
				{
					bSeen |= Existing.Type == Match.Type;
				}
				if (!bSeen)
				{
					Filtered.Add(Match);
				}
			}
		}

		if (Filtered.IsEmpty())
		{
			if (!All.IsEmpty() && All[0].Type)
			{
				if (bOutWrongKind)
				{
					*bOutWrongKind = true;
					return false;
				}
				Sink.Error(TEXT("DST4021"), Span,
					FString::Printf(TEXT("`%s` is a %s, but a %s is expected here."), *Written, CategoryWord(All[0].Type->Category), *ExpectedWord),
					TEXT("Tasks go after `do`, conditions after `if`, utility considerations after `utility`."));
				return false;
			}
			TArray<FString> Names;
			for (const FDstNodeType& Type : Registry.GetAllTypes())
			{
				if (IsExpected(&Type))
				{
					Names.Add(Type.Name);
				}
			}
			Sink.Error(TEXT("DST4001"), Span, FString::Printf(TEXT("There is no %s called `%s`."), *ExpectedWord, *Written), MakeSuggestionHint(Written, Names));
			return false;
		}

		// The first namespace that has the name wins; later ones are hidden by it.
		auto Priority = [this](const FString& Namespace)
		{
			if (Namespace.IsEmpty())
			{
				return Namespaces.Num();
			}
			for (int32 Index = 0; Index < Namespaces.Num(); ++Index)
			{
				if (Namespaces[Index].Equals(Namespace, ESearchCase::IgnoreCase))
				{
					return Index;
				}
			}
			return Namespaces.Num();
		};
		int32 Best = MAX_int32;
		for (const FDstNameMatch& Match : Filtered)
		{
			Best = FMath::Min(Best, Priority(Match.Namespace));
		}
		TArray<FDstNameMatch> Chosen;
		TArray<FDstNameMatch> Hidden;
		for (const FDstNameMatch& Match : Filtered)
		{
			(Priority(Match.Namespace) == Best ? Chosen : Hidden).Add(Match);
		}

		if (Chosen.Num() > 1)
		{
			TArray<FString> Names;
			for (const FDstNameMatch& Match : Chosen)
			{
				Names.Add(Match.Type->Struct ? Match.Type->Struct->GetPathName() : Match.Type->Name);
			}
			Sink.Error(TEXT("DST4002"), Span, FString::Printf(TEXT("`%s` could be any of: %s."), *Written, *FString::Join(Names, TEXT(", "))),
				TEXT("Write the full name, such as the struct's name without the F."));
			return false;
		}

		for (const FDstNameMatch& Match : Hidden)
		{
			Sink.Warning(TEXT("DST4018"), Span,
				FString::Printf(TEXT("`%s` means %s here; it hides %s."), *Written, *Chosen[0].Type->Name, *Match.Type->Name),
				TEXT("The schema's namespaces are searched first. Write the full name to mean the other one."));
		}
		OutType = Chosen[0].Type;
		return true;
	}

	bool FDstLowerer::CheckSchemaAllowsNode(const FDstNodeType& Type, const FDstSpan& Span)
	{
		if (!Schema.IsValid())
		{
			return true;
		}
		bool bAllowed = false;
		if (Type.BlueprintClass || !Type.BlueprintAssetPath.IsEmpty())
		{
			const FNodeInfoPtr Info = GetNodeInfo(Type);
			const UClass* Class = Info.IsValid() ? Cast<UClass>(Info->InstanceStruct) : nullptr;
			bAllowed = Class && Schema->IsClassAllowed(Class);
		}
		else
		{
			bAllowed = Type.Struct && Schema->IsStructAllowed(Type.Struct);
		}
		if (!bAllowed)
		{
			Sink.Error(TEXT("DST4003"), Span,
				FString::Printf(TEXT("The schema %s does not allow `%s`."), *FDstNameRegistry::Get().GetSchemaName(SchemaClass), *Type.Name),
				TEXT("Use a node the schema accepts (its IsStructAllowed / IsClassAllowed), or another schema."));
		}
		return bAllowed;
	}

	bool FDstLowerer::MakeSugarNode(const TCHAR* StructPath, const FString& Key, const EDstNodeRole Role, const FDstSpan& Span,
		FDstNode& OutNode, FNodeInfoPtr& OutInfo, const FDstNodeType*& OutType)
	{
		const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, StructPath);
		if (!Struct)
		{
			Sink.Error(TEXT("DST4001"), Span, FString::Printf(TEXT("The engine node %s is not loaded."), StructPath),
				TEXT("The StateTree module that declares it must be loaded."));
			return false;
		}
		OutInfo = GetNodeInfoForStruct(Struct);
		OutType = OutInfo.IsValid() ? OutInfo->Type : nullptr;
		if (!OutType || !CheckSchemaAllowsNode(*OutType, Span))
		{
			return false;
		}
		OutNode = FDstNode();
		OutNode.Key = Key;
		OutNode.Role = Role;
		OutNode.StructType = Struct->GetPathName();
		AddPlace(Key, Span);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// A node call
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerNode(const FDstExpr& Expr, const FString& BindName, const FDstSpan& BindSpan, const FString& Key,
		const EDstNodeRole Role, const EDstNodeCategory Expected, const FBindScope& Scope, FDstNode& OutNode, FNodeInfoPtr* OutInfo,
		const FDstMeta* ExtraSettings)
	{
		const int32 ErrorsBefore = Sink.Real.NumErrors();
		OutNode = FDstNode();
		OutNode.Key = Key;
		OutNode.Role = Role;
		OutNode.BindName = BindName;
		AddPlace(Key, Expr.Span);

		// `Path(Game, "AI/STT_Wander")` alone names a Blueprint node and passes it nothing; with arguments it is the callee
		// of a call: `Path(Game, "AI/STT_Wander")(Radius = 800)`.
		FString BarePath;
		const bool bBarePath = TryGetDstAssetPath(Expr, BarePath);
		const FDstCallExpr* Call = bBarePath ? nullptr : Expr.As<FDstCallExpr>();
		const FDstExpr* Callee = Call ? Call->Callee.Get() : &Expr;
		if (!Callee)
		{
			Sink.Error(TEXT("DST4001"), Expr.Span, TEXT("Expected a node."));
			return false;
		}
		if (Call == nullptr && !bBarePath && Expr.Kind != EDstNodeKind::NameExpr && Expr.Kind != EDstNodeKind::MemberExpr)
		{
			Sink.Error(TEXT("DST4001"), Expr.Span, TEXT("Expected a node call such as `Wait(2)`."));
			return false;
		}

		const FDstNodeType* Type = nullptr;
		if (!ResolveNodeType(*Callee, Expected, EDstNodeCategory::Unknown, Expr.Span, Type) || !CheckSchemaAllowsNode(*Type, Expr.Span))
		{
			return false;
		}

		FNodeInfoPtr Info = GetNodeInfo(*Type);
		if (OutInfo)
		{
			*OutInfo = Info;
		}
		if (Type->Struct)
		{
			OutNode.StructType = Type->Struct->GetPathName();
		}
		if (Info->bBlueprint)
		{
			if (const UClass* Class = Cast<UClass>(Info->InstanceStruct))
			{
				OutNode.BlueprintClass = Class->GetPathName();
			}
		}

		FNodeBuild Build;
		Build.Node = &OutNode;
		Build.Info = Info;
		Build.Scope = Scope;
		Build.CallSpan = Expr.Span;
		Build.DisplayName = Type->Name;

		if (Call)
		{
			LowerNodeArguments(Build, Call->Args);
			LowerNodeSettings(Build, Call->Meta.Get(), Expr.Span);
		}
		else if (bBarePath)
		{
			LowerNodeSettings(Build, Expr.As<FDstCallExpr>()->Meta.Get(), Expr.Span);
		}
		if (ExtraSettings)
		{
			LowerNodeSettings(Build, ExtraSettings, Expr.Span);
		}
		CheckUnboundProperties(Build);
		FinishNode(Build);
		(void)BindSpan;
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	void FDstLowerer::FinishNode(FNodeBuild& Build)
	{
		SortArgs(*Build.Node);
	}

	void FDstLowerer::SortArgs(FDstNode& Node)
	{
		Node.Args.StableSort([](const FDstArg& A, const FDstArg& B)
		{
			if (A.Side != B.Side)
			{
				return static_cast<uint8>(A.Side) < static_cast<uint8>(B.Side);
			}
			return A.Property.ToString().Compare(B.Property.ToString(), ESearchCase::CaseSensitive) < 0;
		});
	}

	void FDstLowerer::MarkGiven(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property)
	{
		Build.Given.AddUnique(GivenKey(Side, Property));
	}

	bool FDstLowerer::WasGiven(const FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property) const
	{
		return Build.Given.Contains(GivenKey(Side, Property));
	}

	// ------------------------------------------------------------------------------------------
	// Arguments
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::FindArgumentProperty(const FNodeInfo& Info, const FString& Name, EDstPropertySide& OutSide, const FProperty*& OutProperty) const
	{
		auto FindInstance = [&Info](const FString& PropertyName) -> const FProperty*
		{
			if (!Info.InstanceStruct)
			{
				return nullptr;
			}
			const FProperty* Property = Info.InstanceStruct->FindPropertyByName(FName(*PropertyName));
			return IsInstanceArgumentCandidate(Info, Property) ? Property : nullptr;
		};
		auto FindNode = [&Info](const FString& PropertyName) -> const FProperty*
		{
			if (!Info.NodeStruct)
			{
				return nullptr;
			}
			const FProperty* Property = Info.NodeStruct->FindPropertyByName(FName(*PropertyName));
			return IsNodeArgumentCandidate(Property, false) ? Property : nullptr;
		};

		if (const FProperty* Property = FindInstance(Name))
		{
			OutSide = EDstPropertySide::Instance;
			OutProperty = Property;
			return true;
		}
		if (const FProperty* Property = FindNode(Name))
		{
			OutSide = EDstPropertySide::Node;
			OutProperty = Property;
			return true;
		}

		// A bool may drop its `b`.
		const FString WithB = TEXT("b") + Name;
		if (const FProperty* Property = FindInstance(WithB))
		{
			if (Property->IsA<FBoolProperty>())
			{
				OutSide = EDstPropertySide::Instance;
				OutProperty = Property;
				return true;
			}
		}
		if (const FProperty* Property = FindNode(WithB))
		{
			if (Property->IsA<FBoolProperty>())
			{
				OutSide = EDstPropertySide::Node;
				OutProperty = Property;
				return true;
			}
		}
		return false;
	}

	bool FDstLowerer::LowerNodeArguments(FNodeBuild& Build, const TArray<FDstArgumentPtr>& Arguments)
	{
		const FNodeInfo& Info = *Build.Info;
		const FDstNodeType& Type = *Info.Type;
		bool bOk = true;
		int32 PositionalIndex = 0;

		for (const FDstArgumentPtr& Pointer : Arguments)
		{
			if (!Pointer || !Pointer->Value)
			{
				continue;
			}
			const FDstArgument& Argument = *Pointer;

			if (Argument.bOutput)
			{
				bOk &= LowerOutputArgument(Build, Argument);
				continue;
			}

			EDstPropertySide Side = EDstPropertySide::Instance;
			const FProperty* Property = nullptr;
			if (!Argument.IsNamed())
			{
				if (PositionalIndex >= Type.PositionalArgs.Num())
				{
					const TArray<FString> Names = ArgumentNames(Info);
					Sink.Error(TEXT("DST4016"), Argument.Span,
						Type.PositionalArgs.IsEmpty()
							? FString::Printf(TEXT("`%s` takes no positional arguments."), *Build.DisplayName)
							: FString::Printf(TEXT("`%s` takes only %d positional argument(s)."), *Build.DisplayName, Type.PositionalArgs.Num()),
						Names.IsEmpty() ? FString() : FString::Printf(TEXT("Name the argument: its properties are %s."), *FString::Join(Names, TEXT(", "))));
					bOk = false;
					continue;
				}
				const FString PropertyName = Type.PositionalArgs[PositionalIndex++].ToString();
				if (!FindArgumentProperty(Info, PropertyName, Side, Property))
				{
					Sink.Error(TEXT("DST4007"), Argument.Span,
						FString::Printf(TEXT("`%s` declares the positional argument `%s`, but has no such property."), *Build.DisplayName, *PropertyName));
					bOk = false;
					continue;
				}
			}
			else if (!FindArgumentProperty(Info, Argument.Name, Side, Property))
			{
				Sink.Error(TEXT("DST4007"), Argument.NameSpan, FString::Printf(TEXT("`%s` has no property `%s`."), *Build.DisplayName, *Argument.Name),
					MakeSuggestionHint(Argument.Name, ArgumentNames(Info)));
				bOk = false;
				continue;
			}

			if (WasGiven(Build, Side, *Property))
			{
				Sink.Error(TEXT("DST4017"), Argument.Span, FString::Printf(TEXT("`%s` is given twice."), *Property->GetName()));
				bOk = false;
				continue;
			}
			bOk &= LowerValue(Build, Side, *Property, *Argument.Value);
		}
		return bOk;
	}

	// ------------------------------------------------------------------------------------------
	// Settings
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerNodeSettings(FNodeBuild& Build, const FDstMeta* Meta, const FDstSpan& Fallback)
	{
		if (!Meta)
		{
			return true;
		}
		const FNodeInfo& Info = *Build.Info;
		bool bOk = true;

		for (const TUniquePtr<FDstMetaEntry>& Entry : Meta->Entries)
		{
			if (!Entry)
			{
				continue;
			}

			// The sugar table's word -> the engine's property name.
			FString EngineName = Entry->Key;
			bool bFromTable = false;
			for (const Sugar::FWord& Word : Sugar::NodeSettings)
			{
				if (Entry->Key.Equals(Word.Written, ESearchCase::CaseSensitive))
				{
					EngineName = Word.Engine;
					bFromTable = true;
				}
			}

			if (EngineName.Equals(TEXT("Name"), ESearchCase::CaseSensitive))
			{
				FString NameText;
				if (Entry->Value)
				{
					if (const FDstLiteralExpr* Literal = Entry->Value->As<FDstLiteralExpr>())
					{
						NameText = Literal->Text;
					}
					else
					{
						NameText = GetDstPathString(*Entry->Value);
					}
				}
				if (NameText.IsEmpty())
				{
					Sink.Error(TEXT("DST2018"), Entry->Span, TEXT("`Name` is a string: [ Name = \"Combo\" ]."));
					bOk = false;
				}
				else if (!Build.Node->BindName.IsEmpty() && Build.Node->BindName != NameText)
				{
					Sink.Error(TEXT("DST4017"), Entry->Span,
						FString::Printf(TEXT("The node is already named `%s`; its settings say `%s`."), *Build.Node->BindName, *NameText));
					bOk = false;
				}
				else
				{
					Build.Node->BindName = NameText;
				}
				continue;
			}

			// The property of the node struct. The table's names are declared on the base structs; any other key must be an
			// editable property of the node itself.
			EDstPropertySide Side = EDstPropertySide::Node;
			const FProperty* Property = Info.NodeStruct ? Info.NodeStruct->FindPropertyByName(FName(*EngineName)) : nullptr;
			if (!Property && !bFromTable)
			{
				Property = Info.NodeStruct ? Info.NodeStruct->FindPropertyByName(FName(*(TEXT("b") + EngineName))) : nullptr;
				if (Property && !Property->IsA<FBoolProperty>())
				{
					Property = nullptr;
				}
			}
			// A Blueprint task keeps this one on its instance object; the wrapper struct's own copy does nothing.
			bool bOnInstance = false;
			if (Info.bBlueprint && EngineName.Equals(TEXT("bConsideredForCompletion"), ESearchCase::CaseSensitive) && Info.InstanceStruct)
			{
				if (const FProperty* InstanceProperty = Info.InstanceStruct->FindPropertyByName(FName(*EngineName)))
				{
					Property = InstanceProperty;
					Side = EDstPropertySide::Instance;
					bOnInstance = true;
				}
			}
			if (!Property || (!bOnInstance && !IsNodeArgumentCandidate(Property, bFromTable)))
			{
				Sink.Error(TEXT("DST4014"), Entry->KeySpan,
					FString::Printf(TEXT("`%s` is not a setting of `%s`."), *Entry->Key, *Build.DisplayName),
					bFromTable
						? TEXT("Enabled, ConsideredForCompletion and TransitionPriority are for tasks; Evaluation is for conditions.")
						: TEXT("Node settings are Name, Enabled, ConsideredForCompletion, TransitionPriority and Evaluation, or an editable property of the node itself."));
				bOk = false;
				continue;
			}
			if (WasGiven(Build, Side, *Property))
			{
				Sink.Error(TEXT("DST4017"), Entry->Span, FString::Printf(TEXT("`%s` is given twice."), *Entry->Key));
				bOk = false;
				continue;
			}
			MarkGiven(Build, Side, *Property);

			// A flag with no value means true.
			TUniquePtr<FDstExpr> FlagValue;
			const FDstExpr* ValueExpr = Entry->Value.Get();
			if (!ValueExpr)
			{
				FFoldedConstant True;
				True.Kind = FFoldedConstant::EKind::Bool;
				True.bBool = true;
				FlagValue = MakeConstantExpr(True, Entry->Span);
				ValueExpr = FlagValue.Get();
			}
			bOk &= LowerConstant(Build, Side, *Property, *ValueExpr);
		}
		(void)Fallback;
		return bOk;
	}

	// ------------------------------------------------------------------------------------------
	// What is left unwritten
	// ------------------------------------------------------------------------------------------

	void FDstLowerer::CheckUnboundProperties(FNodeBuild& Build)
	{
		const FNodeInfo& Info = *Build.Info;
		if (!Info.InstanceStruct)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Info.InstanceStruct); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsInstanceArgumentCandidate(Info, Property) || WasGiven(Build, EDstPropertySide::Instance, *Property))
			{
				continue;
			}

			if (IsPropertyRefProperty(*Property))
			{
				if (!IsOptionalProperty(*Property))
				{
					Sink.Error(TEXT("DST5004"), Build.CallSpan,
						FString::Printf(TEXT("`%s` of `%s` is a property reference and needs a target: %s = ref Params.Something."), *Property->GetName(), *Build.DisplayName, *Property->GetName()));
				}
				continue;
			}

			switch (GetPropertyUse(*Property))
			{
			case EPropertyUse::Input:
				if (!IsOptionalProperty(*Property))
				{
					Sink.Error(TEXT("DST5004"), Build.CallSpan,
						FString::Printf(TEXT("`%s` of `%s` is an Input and must be bound."), *Property->GetName(), *Build.DisplayName),
						FString::Printf(TEXT("Write %s = <path>, for example %s = Params.Something."), *Property->GetName(), *Property->GetName()));
				}
				break;

			case EPropertyUse::Context:
			{
				// The compiler binds a Context property to the context of its type by itself.
				const UStruct* Wanted = nullptr;
				if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					Wanted = StructProperty->Struct;
				}
				else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
				{
					Wanted = ObjectProperty->PropertyClass;
				}
				bool bFound = false;
				for (const FContext& Context : Contexts)
				{
					bFound |= Wanted && Context.Struct && Context.Struct->IsChildOf(Wanted);
				}
				if (!bFound)
				{
					Sink.Error(TEXT("DST5003"), Build.CallSpan,
						FString::Printf(TEXT("`%s` of `%s` needs a context of type %s, and the schema has none."), *Property->GetName(), *Build.DisplayName, Wanted ? *Wanted->GetName() : TEXT("?")),
						TEXT("Bind it by hand: write the property = a path, or use a schema that provides that context."));
				}
				break;
			}

			default:
				break;
			}
		}
	}
}
