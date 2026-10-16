// Copyright DreamMoon. All Rights Reserved.
//
// One node, text against asset. The arguments are compared property by property. A constant that changed is replaced
// where it stands (just the literal); anything else a task or a global node changed (an argument added or removed, a
// binding, a setting) reprints the node's call from the asset's own text. A condition or a consideration that changed in
// any other way is left to the card: its place in an expression is not something to cut out of the middle.

#include "WriteBack/DstWriteBackInternal.h"

#include "DstSugar.h"
#include "StateTreeNodeBase.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::WriteBackPrivate
{
	// ------------------------------------------------------------------------------------------
	// Arguments
	// ------------------------------------------------------------------------------------------

	FString WbArgSignature(const Model::FDstArg& Arg)
	{
		FString Signature = FString::Printf(TEXT("%d\n%s\n%d\n"), static_cast<int32>(Arg.Side), *Arg.Property.ToString(), static_cast<int32>(Arg.Kind));
		switch (Arg.Kind)
		{
		case Model::EDstValueKind::Constant:
			Signature += Arg.ConstantText;
			break;
		case Model::EDstValueKind::Function:
			Signature += Arg.Function.IsValid() ? DumpNodeOf(*Arg.Function) : FString(TEXT("<missing>"));
			break;
		default:
			Signature += Arg.Source.ToString();
			break;
		}
		return Signature;
	}

	FString WbGroupSignature(const TArray<const Model::FDstArg*>& Args)
	{
		TArray<FString> Parts;
		for (const Model::FDstArg* Arg : Args)
		{
			Parts.Add(WbArgSignature(*Arg));
		}
		Parts.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		return FString::Join(Parts, TEXT("\n--\n"));
	}

	/** The properties whose arguments differ between the two nodes. */
	void WbBuildChangedGroups(const Model::FDstNode& TextNode, const Model::FDstNode& AssetNode, TArray<FArgGroup>& OutChanged)
	{
		TArray<FArgGroup> All;
		const auto FindGroup = [&All](const Model::EDstPropertySide Side, const FString& Property) -> FArgGroup&
		{
			for (FArgGroup& Group : All)
			{
				if (Group.Side == Side && Group.Property.Equals(Property, ESearchCase::CaseSensitive))
				{
					return Group;
				}
			}
			FArgGroup& Added = All.AddDefaulted_GetRef();
			Added.Side = Side;
			Added.Property = Property;
			return Added;
		};
		const auto NameOf = [](const Model::FDstArg& Arg) -> FString
		{
			return Arg.Property.Segments.IsEmpty() ? FString() : Arg.Property.Segments[0].Name;
		};

		for (const Model::FDstArg& Arg : TextNode.Args)
		{
			FindGroup(Arg.Side, NameOf(Arg)).TextArgs.Add(&Arg);
		}
		for (const Model::FDstArg& Arg : AssetNode.Args)
		{
			FindGroup(Arg.Side, NameOf(Arg)).AssetArgs.Add(&Arg);
		}
		for (FArgGroup& Group : All)
		{
			if (!WbGroupSignature(Group.TextArgs).Equals(WbGroupSignature(Group.AssetArgs), ESearchCase::CaseSensitive))
			{
				OutChanged.Add(MoveTemp(Group));
			}
		}
	}

	/** The whole property is one constant on both sides. */
	bool WbIsConstantChange(const FArgGroup& Group)
	{
		if (Group.Side != Model::EDstPropertySide::Instance || Group.TextArgs.Num() != 1 || Group.AssetArgs.Num() != 1)
		{
			return false;
		}
		const Model::FDstArg& TextArg = *Group.TextArgs[0];
		const Model::FDstArg& AssetArg = *Group.AssetArgs[0];
		return TextArg.Kind == Model::EDstValueKind::Constant && AssetArg.Kind == Model::EDstValueKind::Constant
			&& TextArg.Property.Segments.Num() == 1 && AssetArg.Property.Segments.Num() == 1
			&& TextArg.Property.Segments[0].ArrayIndex == INDEX_NONE && AssetArg.Property.Segments[0].ArrayIndex == INDEX_NONE;
	}

	// ------------------------------------------------------------------------------------------
	// Reading the engine's side
	// ------------------------------------------------------------------------------------------

	const UScriptStruct* WbFindNodeStruct(const Model::FDstNode& Node)
	{
		// A Blueprint node's instance data is an object of its class: its constants are not edited in place.
		if (!Node.BlueprintClass.IsEmpty() || Node.StructType.IsEmpty())
		{
			return nullptr;
		}
		return FindObject<UScriptStruct>(nullptr, *Node.StructType);
	}

	/** The struct the node keeps its instance data in; asked of a default node, the way the engine asks it. */
	const UStruct* WbGetInstanceStruct(const UScriptStruct& NodeStruct)
	{
		if (!NodeStruct.IsChildOf(FStateTreeNodeBase::StaticStruct()))
		{
			return nullptr;
		}
		const FStructOnScope Scope(&NodeStruct);
		const FStateTreeNodeBase* Base = reinterpret_cast<const FStateTreeNodeBase*>(Scope.GetStructMemory());
		return Base ? Base->GetInstanceDataType() : nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Reading the text's side
	// ------------------------------------------------------------------------------------------

	/** The argument of a call that gives the property: named (a bool without its b, too) or by position. */
	const Lang::FDstArgument* WbFindCallArgument(const Lang::FDstCallExpr& Call, const FDstNodeType* Type, const FString& PropertyName)
	{
		int32 Positional = 0;
		for (const Lang::FDstArgumentPtr& Argument : Call.Args)
		{
			if (!Argument || !Argument->Value || Argument->bOutput)
			{
				continue;
			}
			FString Resolved;
			if (Argument->IsNamed())
			{
				Resolved = Argument->Name;
			}
			else
			{
				if (Type && Positional < Type->PositionalArgs.Num())
				{
					Resolved = Type->PositionalArgs[Positional].ToString();
				}
				++Positional;
			}
			if (Resolved.IsEmpty())
			{
				continue;
			}
			if (Resolved.Equals(PropertyName, ESearchCase::CaseSensitive)
				|| (PropertyName.Len() > 1 && PropertyName[0] == TEXT('b') && Resolved.Equals(PropertyName.Mid(1), ESearchCase::CaseSensitive)))
			{
				return Argument.Get();
			}
		}
		return nullptr;
	}

	const Lang::FDstExpr& WbStripParens(const Lang::FDstExpr& Expr)
	{
		const Lang::FDstExpr* Current = &Expr;
		while (const Lang::FDstParenExpr* Paren = Current->As<Lang::FDstParenExpr>())
		{
			if (!Paren->Inner)
			{
				break;
			}
			Current = Paren->Inner.Get();
		}
		return *Current;
	}

	/** A number as written: `3`, `-0.5`, `(2)`. */
	bool WbIsNumberLiteral(const Lang::FDstExpr& Expr)
	{
		const Lang::FDstExpr& Inner = WbStripParens(Expr);
		if (const Lang::FDstLiteralExpr* Literal = Inner.As<Lang::FDstLiteralExpr>())
		{
			return Literal->LiteralKind == Lang::EDstLiteralKind::Number;
		}
		if (const Lang::FDstUnaryExpr* Unary = Inner.As<Lang::FDstUnaryExpr>())
		{
			return Unary->Op == Lang::EDstUnaryOp::Negate && Unary->Operand && WbIsNumberLiteral(*Unary->Operand);
		}
		return false;
	}

	bool WbIsComparison(const Lang::EDstBinaryOp Op)
	{
		return Op == Lang::EDstBinaryOp::Equal || Op == Lang::EDstBinaryOp::NotEqual || Op == Lang::EDstBinaryOp::Less
			|| Op == Lang::EDstBinaryOp::LessEqual || Op == Lang::EDstBinaryOp::Greater || Op == Lang::EDstBinaryOp::GreaterEqual;
	}

	// ------------------------------------------------------------------------------------------
	// Where a node is in the text, and whether a template wrote it
	// ------------------------------------------------------------------------------------------

	const Lang::FDstExpr* FWriteBack::FindNodeExpr(const Model::FDstNode& TextNode, const Lang::FDstNode*& OutOwner) const
	{
		OutOwner = nullptr;
		const Lang::FDstSpan* Span = Places.Spans.Find(TextNode.Key);
		if (!Span || Span->IsEmpty() || !Edit.IsValid())
		{
			return nullptr;
		}

		switch (TextNode.Role)
		{
		case Model::EDstNodeRole::Task:
		{
			Lang::FDstNode* Found = Edit->FindNode(*Span, { Lang::EDstNodeKind::TaskItem });
			const Lang::FDstTaskItem* Item = Found ? Found->As<Lang::FDstTaskItem>() : nullptr;
			OutOwner = Item;
			return Item ? Item->Node.Get() : nullptr;
		}
		case Model::EDstNodeRole::Evaluator:
		case Model::EDstNodeRole::GlobalTask:
		{
			Lang::FDstNode* Found = Edit->FindNode(*Span, { Lang::EDstNodeKind::AssignStmt });
			const Lang::FDstAssignStmt* Assign = Found ? Found->As<Lang::FDstAssignStmt>() : nullptr;
			OutOwner = Assign;
			return Assign ? Assign->Value.Get() : nullptr;
		}
		default:
		{
			Lang::FDstNode* Found = Edit->FindNode(*Span, {
				Lang::EDstNodeKind::CallExpr, Lang::EDstNodeKind::BinaryExpr, Lang::EDstNodeKind::NameExpr, Lang::EDstNodeKind::MemberExpr,
				Lang::EDstNodeKind::LiteralExpr, Lang::EDstNodeKind::UnaryExpr, Lang::EDstNodeKind::ParenExpr });
			OutOwner = Found;
			return Found ? static_cast<const Lang::FDstExpr*>(Found) : nullptr;
		}
		}
	}

	bool FWriteBack::IsInExpansion(const Lang::FDstSpan& Span, FString* OutName) const
	{
		for (const FExpansionSite& Site : Expansions)
		{
			if (Site.Span.Offset <= Span.Offset && Span.End() <= Site.Span.End())
			{
				if (OutName)
				{
					*OutName = Site.TemplateName;
				}
				return true;
			}
		}
		return false;
	}

	bool FWriteBack::UsesTemplate(const Lang::FDstNode& Node, const bool bDeep, FString* OutName) const
	{
		if (TemplateNames.IsEmpty())
		{
			return false;
		}
		FString Name;
		if (const Lang::FDstCallExpr* Call = Node.As<Lang::FDstCallExpr>())
		{
			const Lang::FDstNameExpr* Callee = Call->Callee ? Call->Callee->As<Lang::FDstNameExpr>() : nullptr;
			if (Callee && TemplateNames.Contains(Callee->Name))
			{
				Name = Callee->Name;
			}
		}
		else if (const Lang::FDstNameExpr* Written = Node.As<Lang::FDstNameExpr>())
		{
			if (TemplateNames.Contains(Written->Name))
			{
				Name = Written->Name;
			}
		}
		if (!Name.IsEmpty())
		{
			if (OutName)
			{
				*OutName = Name;
			}
			return true;
		}
		if (!bDeep)
		{
			return false;
		}

		bool bFound = false;
		Lang::ForEachChild(Node, [this, &bFound, OutName](const Lang::FDstNode& Child)
		{
			if (!bFound && UsesTemplate(Child, true, OutName))
			{
				bFound = true;
			}
		});
		return bFound;
	}

	// ------------------------------------------------------------------------------------------
	// A node
	// ------------------------------------------------------------------------------------------

	enum class EConstantPlan : uint8 { Done, Refused, CannotDo };

	/** Every changed property is a whole constant: each is replaced where the text gives it. */
	EConstantPlan WbPlanConstants(FWriteBack& WB, const Model::FDstNode& TextNode, const TArray<FArgGroup>& Groups, const Lang::FDstExpr& Expr,
		const FString& Location, const FString& What, FPlan& Plan)
	{
		const UScriptStruct* NodeStruct = WbFindNodeStruct(TextNode);
		const UStruct* InstanceStruct = NodeStruct ? WbGetInstanceStruct(*NodeStruct) : nullptr;
		if (!InstanceStruct)
		{
			return EConstantPlan::CannotDo;
		}
		const FDstNodeType* Type = FDstNameRegistry::Get().FindByStruct(NodeStruct);

		TArray<FEdit> Replaced;
		for (const FArgGroup& Group : Groups)
		{
			const FProperty* Property = InstanceStruct->FindPropertyByName(FName(*Group.Property));
			if (!Property)
			{
				return EConstantPlan::CannotDo;
			}
			const Lang::FDstExprPtr Literal = DstExportTextToLiteral(Group.AssetArgs[0]->ConstantText, *Property, WB.ValueContext);
			if (!Literal)
			{
				return EConstantPlan::CannotDo;
			}

			// The text that gives this property: an argument of the call, the number in a comparison, or the whole number.
			const Lang::FDstExpr* Target = nullptr;
			if (const Lang::FDstCallExpr* Call = Expr.As<Lang::FDstCallExpr>())
			{
				const Lang::FDstArgument* Argument = WbFindCallArgument(*Call, Type, Group.Property);
				Target = Argument ? Argument->Value.Get() : nullptr;
			}
			else if (const Lang::FDstBinaryExpr* Binary = Expr.As<Lang::FDstBinaryExpr>())
			{
				if (WbIsComparison(Binary->Op) && Group.Property.Equals(TEXT("Right"), ESearchCase::CaseSensitive) && Binary->Lhs && Binary->Rhs)
				{
					// Written `x < 3` or `3 > x`: the number is the side that is one.
					const bool bLeftNumber = WbIsNumberLiteral(*Binary->Lhs);
					const bool bRightNumber = WbIsNumberLiteral(*Binary->Rhs);
					if (bLeftNumber != bRightNumber)
					{
						Target = bLeftNumber ? Binary->Lhs.Get() : Binary->Rhs.Get();
					}
				}
			}
			else if (WbIsNumberLiteral(Expr) && Group.Property.Equals(Sugar::ConstantConsiderationValue, ESearchCase::CaseSensitive))
			{
				Target = &Expr;
			}
			if (!Target)
			{
				return EConstantPlan::CannotDo;
			}

			FString TemplateName;
			if (WB.UsesTemplate(*Target, /*bDeep*/ true, &TemplateName))
			{
				FPendingRefusal Refusal;
				Refusal.Code = TEXT("DST8102");
				Refusal.Span = Target->Span;
				Refusal.Message = FString::Printf(TEXT("%s: %s is given by the template `%s`; a change here would have to change every use of it."),
					*Location, *Group.Property, *TemplateName);
				Refusal.Hint = TEXT("Change the template, or write the value out in place of the template call.");
				Plan.Refusals.Add(MoveTemp(Refusal));
				FDstWriteBackChange Change;
				Change.Location = Location;
				Change.What = What;
				Plan.Changes.Add(MoveTemp(Change));
				return EConstantPlan::Refused;
			}

			FEdit Replacement;
			Replacement.Offset = Target->Span.Offset;
			Replacement.Length = Target->Span.Length;
			Replacement.Text = WB.Edit->PrintExpr(*Literal, WB.Edit->GetIndentLevel(Target->Span.Offset), Target->Span.Column - 1);
			Replaced.Add(MoveTemp(Replacement));
		}

		Plan.Edits.Append(MoveTemp(Replaced));
		FDstWriteBackChange Change;
		Change.Location = Location;
		Change.What = What;
		Change.bWritten = true;
		Plan.Changes.Add(MoveTemp(Change));
		return EConstantPlan::Done;
	}

	void FWriteBack::PlanNode(const Model::FDstNode& TextNode, const Model::FDstNode& AssetNode, const FString& Location,
		const Lang::FDstExpr* RaisedExpr, FPlan& Plan)
	{
		if (Plan.bNeedsReprint)
		{
			return;
		}

		// What the arguments cannot say: a node that is another node, called another name, or placed another way in its list.
		if (!TextNode.StructType.Equals(AssetNode.StructType, ESearchCase::CaseSensitive)
			|| !TextNode.BlueprintClass.Equals(AssetNode.BlueprintClass, ESearchCase::CaseSensitive)
			|| !TextNode.BindName.Equals(AssetNode.BindName, ESearchCase::CaseSensitive)
			|| TextNode.Operand != AssetNode.Operand || TextNode.Indent != AssetNode.Indent || TextNode.Role != AssetNode.Role)
		{
			Plan.bNeedsReprint = true;
			return;
		}

		TArray<FArgGroup> Groups;
		WbBuildChangedGroups(TextNode, AssetNode, Groups);
		if (Groups.IsEmpty())
		{
			return;
		}

		const Lang::FDstNode* Owner = nullptr;
		const Lang::FDstExpr* Expr = FindNodeExpr(TextNode, Owner);
		if (!Expr)
		{
			Plan.bNeedsReprint = true;
			return;
		}

		TArray<FString> Names;
		bool bAllConstants = true;
		for (const FArgGroup& Group : Groups)
		{
			Names.AddUnique(Group.Property);
			bAllConstants &= WbIsConstantChange(Group);
		}
		const FString What = FString::Join(Names, TEXT(", "));

		// A node a template wrote: what is changed in the asset belongs to the template.
		FString TemplateName;
		if (IsInExpansion(Expr->Span, &TemplateName) || UsesTemplate(*Expr, /*bDeep*/ false, &TemplateName))
		{
			FPendingRefusal Refusal;
			Refusal.Code = TEXT("DST8102");
			Refusal.Span = Expr->Span;
			Refusal.Message = FString::Printf(TEXT("%s: the node is written by the template `%s`; a change here would have to change every use of it."),
				*Location, *TemplateName);
			Refusal.Hint = TEXT("Change the template, or write the node out in place of the template call.");
			Plan.Refusals.Add(MoveTemp(Refusal));
			FDstWriteBackChange Change;
			Change.Location = Location;
			Change.What = What;
			Plan.Changes.Add(MoveTemp(Change));
			return;
		}

		if (bAllConstants)
		{
			FPlan Constants;
			const EConstantPlan Outcome = WbPlanConstants(*this, TextNode, Groups, *Expr, Location, What, Constants);
			if (Outcome != EConstantPlan::CannotDo)
			{
				Plan.Edits.Append(MoveTemp(Constants.Edits));
				Plan.Changes.Append(MoveTemp(Constants.Changes));
				Plan.Refusals.Append(MoveTemp(Constants.Refusals));
				return;
			}
		}

		// The call as the asset's own text spells it. Only the nodes of a list can be cut out whole; a part of a condition cannot.
		const bool bWholeCall = TextNode.Role == Model::EDstNodeRole::Task || TextNode.Role == Model::EDstNodeRole::Evaluator
			|| TextNode.Role == Model::EDstNodeRole::GlobalTask;
		if (!bWholeCall || !RaisedExpr || !bRaiseOk)
		{
			Plan.bNeedsReprint = true;
			return;
		}

		if (UsesTemplate(*Expr, /*bDeep*/ true, &TemplateName))
		{
			FPendingRefusal Note;
			Note.Code = TEXT("DST8106");
			Note.Span = Expr->Span;
			Note.Message = FString::Printf(TEXT("%s: the call was reprinted from the asset; the use of the template `%s` in it is written out."), *Location, *TemplateName);
			Note.bRefusal = false;
			Plan.Refusals.Add(MoveTemp(Note));
		}

		FEdit Replaced;
		Replaced.Offset = Expr->Span.Offset;
		Replaced.Length = Expr->Span.Length;
		Replaced.Text = Edit->PrintExpr(*RaisedExpr, Edit->GetIndentLevel(Expr->Span.Offset), Expr->Span.Column - 1);
		Plan.Edits.Add(MoveTemp(Replaced));

		FDstWriteBackChange Change;
		Change.Location = Location;
		Change.What = What;
		Change.bWritten = true;
		Plan.Changes.Add(MoveTemp(Change));
	}
}
