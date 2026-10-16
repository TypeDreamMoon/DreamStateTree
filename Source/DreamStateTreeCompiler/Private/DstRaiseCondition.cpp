// Copyright DreamMoon. All Rights Reserved.
//
// The raise, conditions and considerations. The engine keeps them as a flat list; every item has an Operand that joins it
// to what came before and an Indent that opens and closes parentheses (DstLowerCondition.cpp has the rules and
// part2-brief.md the table). Raising folds the list back into an expression:
//
//   - the items of one level are joined from left to right into a left-leaning tree, as the engine evaluates them;
//   - an item whose next item is deeper opens that many groups at the item, and a drop in indent closes that many after
//     it; a group becomes the right-hand operand of the operator the item that opened it carries (or the first operand
//     when nothing came before);
//   - the parentheses the text's precedence needs are made when the operator is (MakeBinaryExpr), so
//     `A and (B or C)` keeps its group and `A or B and C` does not grow one.
//
// A leaf is a comparison (`a < b`, `a != b`, a bare bool path) when the node is a Compare node the lowering would pick
// again for those operands, `not` for bInvert, a constant for a Constant consideration, a call otherwise.

#include "DstRaisePrivate.h"

#include "Misc/OutputDeviceNull.h"
#include "StateTreeAnyEnum.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Raising
{
	namespace
	{
		Lang::EDstBinaryOp GetRaiseJoinOp(const Model::EDstOperand Operand)
		{
			switch (Operand)
			{
			case Model::EDstOperand::Or: return Lang::EDstBinaryOp::Or;
			case Model::EDstOperand::Multiply: return Lang::EDstBinaryOp::Multiply;
			default: return Lang::EDstBinaryOp::And;
			}
		}

		Lang::EDstBinaryOp GetRaiseComparisonOp(const TCHAR* Spelling)
		{
			if (FCString::Strcmp(Spelling, TEXT("<")) == 0) { return Lang::EDstBinaryOp::Less; }
			if (FCString::Strcmp(Spelling, TEXT("<=")) == 0) { return Lang::EDstBinaryOp::LessEqual; }
			if (FCString::Strcmp(Spelling, TEXT(">")) == 0) { return Lang::EDstBinaryOp::Greater; }
			if (FCString::Strcmp(Spelling, TEXT(">=")) == 0) { return Lang::EDstBinaryOp::GreaterEqual; }
			if (FCString::Strcmp(Spelling, TEXT("!=")) == 0) { return Lang::EDstBinaryOp::NotEqual; }
			return Lang::EDstBinaryOp::Equal;
		}

		/** An operation that gives a bool (`and`, `or`, `not`): the lowering types such an operand as a bool, any other as a number. */
		bool IsRaiseLogicExpr(const Lang::FDstExpr& Expr)
		{
			if (const Lang::FDstBinaryExpr* Binary = Expr.As<Lang::FDstBinaryExpr>())
			{
				return Binary->Op == Lang::EDstBinaryOp::And || Binary->Op == Lang::EDstBinaryOp::Or;
			}
			if (const Lang::FDstUnaryExpr* Unary = Expr.As<Lang::FDstUnaryExpr>())
			{
				return Unary->Op == Lang::EDstUnaryOp::Not;
			}
			return false;
		}

		bool IsRaiseArithmeticExpr(const Lang::FDstExpr& Expr)
		{
			return (Expr.Is<Lang::FDstBinaryExpr>() || Expr.Is<Lang::FDstUnaryExpr>()) && !IsRaiseLogicExpr(Expr);
		}

		/** The Compare node whose struct path this is, or null. */
		const Sugar::FCompareNode* FindRaiseCompareSpec(const FString& StructType)
		{
			static const Sugar::FCompareNode* const Specs[] = {
				&Sugar::CompareFloat, &Sugar::CompareInt, &Sugar::CompareBool, &Sugar::CompareEnum, &Sugar::CompareName,
			};
			for (const Sugar::FCompareNode* Spec : Specs)
			{
				if (StructType.Equals(Spec->StructPath, ESearchCase::IgnoreCase))
				{
					return Spec;
				}
			}
			return nullptr;
		}

		/** The Compare node the lowering picks for a left side of this type; null when it has none. */
		const Sugar::FCompareNode* PickRaiseCompareSpec(const Lowering::EValueDomain Domain)
		{
			switch (Domain)
			{
			case Lowering::EValueDomain::Float: return &Sugar::CompareFloat;
			case Lowering::EValueDomain::Int: return &Sugar::CompareInt;
			case Lowering::EValueDomain::Bool: return &Sugar::CompareBool;
			case Lowering::EValueDomain::Enum: return &Sugar::CompareEnum;
			case Lowering::EValueDomain::Name: return &Sugar::CompareName;
			default: return nullptr;
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// The list
	// ------------------------------------------------------------------------------------------

	FExprPtr FDstRaiser::RaiseConditionList(const TArray<FModelNode>& Nodes, const FScope& Scope, const bool bConsideration)
	{
		if (Nodes.IsEmpty())
		{
			return nullptr;
		}

		struct FFrame
		{
			FExprPtr Acc;
			/** How the group joins the frame below it when it closes. */
			Model::EDstOperand Connect = Model::EDstOperand::Copy;
		};
		TArray<FFrame> Stack;
		Stack.AddDefaulted();

		bool bWarnedCopy = false;
		const auto Join = [this, &bWarnedCopy, &Nodes](FExprPtr& Acc, const Model::EDstOperand Operand, FExprPtr Rhs)
		{
			if (!Acc)
			{
				Acc = MoveTemp(Rhs);
				return;
			}
			if (Operand == Model::EDstOperand::Copy && !bWarnedCopy)
			{
				bWarnedCopy = true;
				Sink.Warning(TEXT("DST8058"), Lang::FDstSpan(),
					FString::Printf(TEXT("The list starting at `%s` has a Copy operand after its first item; it is read as `and`."), Nodes.IsEmpty() ? TEXT("") : *Nodes[0].Key),
					TEXT("The engine's Copy replaces what came before, which the text cannot say."));
			}
			Acc = MakeBinaryExpr(GetRaiseJoinOp(Operand), MoveTemp(Acc), MoveTemp(Rhs));
		};

		const int32 Count = Nodes.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FModelNode& Node = Nodes[Index];
			const Model::EDstOperand Operand = Index == 0 ? Model::EDstOperand::Copy : Node.Operand;
			const int32 Delta = Index + 1 < Count ? Nodes[Index + 1].Indent - Node.Indent : 0;

			FExprPtr Leaf = RaiseConditionLeaf(Node, Scope, bConsideration);
			if (!Leaf)
			{
				Leaf = MakeNameExpr(TEXT("Unknown"));
			}

			if (Delta > 0)
			{
				// The groups open at this item; the first of them joins what is below with this item's operator.
				for (int32 Open = 0; Open < Delta; ++Open)
				{
					FFrame& Frame = Stack.AddDefaulted_GetRef();
					Frame.Connect = Open == 0 ? Operand : Model::EDstOperand::Copy;
				}
				Stack.Last().Acc = MoveTemp(Leaf);
			}
			else
			{
				Join(Stack.Last().Acc, Operand, MoveTemp(Leaf));
			}

			const int32 Close = Index + 1 < Count ? FMath::Max(0, -Delta) : Stack.Num() - 1;
			for (int32 Done = 0; Done < Close && Stack.Num() > 1; ++Done)
			{
				FFrame Closed = MoveTemp(Stack.Last());
				Stack.Pop(EAllowShrinking::No);
				Join(Stack.Last().Acc, Closed.Connect, MoveTemp(Closed.Acc));
			}
		}

		// Whatever is still open (a list that ends deeper than it started) closes at the end.
		while (Stack.Num() > 1)
		{
			FFrame Closed = MoveTemp(Stack.Last());
			Stack.Pop(EAllowShrinking::No);
			Join(Stack.Last().Acc, Closed.Connect, MoveTemp(Closed.Acc));
		}
		return MoveTemp(Stack[0].Acc);
	}

	// ------------------------------------------------------------------------------------------
	// A leaf
	// ------------------------------------------------------------------------------------------

	FExprPtr FDstRaiser::RaiseConditionLeaf(const FModelNode& Node, const FScope& Scope, const bool bConsideration)
	{
		if (bConsideration)
		{
			if (FExprPtr Constant = TryRaiseConstantConsideration(Node))
			{
				return Constant;
			}
			FCallStyle Style;
			return RaiseNodeCall(Node, Scope, Style);
		}

		bool bInvert = false;
		for (const FModelArg& Arg : Node.Args)
		{
			if (Arg.Side == Model::EDstPropertySide::Node && Arg.Property.Segments.Num() == 1
				&& Arg.Property.Segments[0].Name.Equals(Sugar::InvertProperty, ESearchCase::CaseSensitive))
			{
				bInvert = Arg.Kind == Model::EDstValueKind::Constant && Arg.ConstantText.ToBool();
			}
		}

		if (FExprPtr Compare = TryRaiseCompare(Node, bInvert, Scope))
		{
			return Compare;
		}

		FCallStyle Style;
		Style.bSkipInvert = true;
		FExprPtr Call = RaiseNodeCall(Node, Scope, Style);
		return bInvert ? MakeUnaryExpr(Lang::EDstUnaryOp::Not, MoveTemp(Call)) : MoveTemp(Call);
	}

	FExprPtr FDstRaiser::TryRaiseConstantConsideration(const FModelNode& Node)
	{
		if (!Node.StructType.Equals(Sugar::ConstantConsideration, ESearchCase::IgnoreCase) || !Node.BindName.IsEmpty())
		{
			return nullptr;
		}
		const FNodeInfoPtr Info = GetNodeInfo(Node);
		if (!Info->InstanceStruct)
		{
			return nullptr;
		}
		const FProperty* Property = Info->InstanceStruct->FindPropertyByName(Sugar::ConstantConsiderationValue);
		if (!Property)
		{
			return nullptr;
		}

		const FModelArg* ValueArg = nullptr;
		for (const FModelArg& Arg : Node.Args)
		{
			if (Arg.Side != Model::EDstPropertySide::Instance || Arg.Property.Segments.Num() != 1
				|| !Arg.Property.Segments[0].Name.Equals(Sugar::ConstantConsiderationValue, ESearchCase::CaseSensitive)
				|| Arg.Kind != Model::EDstValueKind::Constant)
			{
				return nullptr;
			}
			ValueArg = &Arg;
		}

		// The number alone: `utility 0.5`. A constant at its default is spelled out all the same.
		const FString Text = ValueArg ? ValueArg->ConstantText : DstExportDefault(*Property, Info->GetInstanceDefaults());
		return MakeConstantExpr(Text, Property, FString::Printf(TEXT("the constant of %s"), *Node.Key));
	}

	// ------------------------------------------------------------------------------------------
	// Comparisons
	// ------------------------------------------------------------------------------------------

	FExprPtr FDstRaiser::TryRaiseCompare(const FModelNode& Node, const bool bInvert, const FScope& Scope)
	{
		const Sugar::FCompareNode* Spec = FindRaiseCompareSpec(Node.StructType);
		if (!Spec || !Node.BindName.IsEmpty())
		{
			return nullptr;
		}
		const FNodeInfoPtr Info = GetNodeInfo(Node);
		if (!Info->InstanceStruct || !Info->NodeStruct)
		{
			return nullptr;
		}
		const FProperty* LeftProperty = Info->InstanceStruct->FindPropertyByName(Spec->Left);
		const FProperty* RightProperty = Info->InstanceStruct->FindPropertyByName(Spec->Right);
		const FProperty* OperatorProperty = Spec->OperatorProperty ? Info->NodeStruct->FindPropertyByName(Spec->OperatorProperty) : nullptr;
		if (!LeftProperty || !RightProperty || (Spec->OperatorProperty && !OperatorProperty))
		{
			return nullptr;
		}

		// Only the left, the right, the operator and bInvert: anything else on the node needs the call form.
		const FModelArg* LeftArg = nullptr;
		const FModelArg* RightArg = nullptr;
		const FModelArg* OperatorArg = nullptr;
		for (const FModelArg& Arg : Node.Args)
		{
			if (Arg.Property.Segments.Num() != 1)
			{
				return nullptr;
			}
			const FString& Name = Arg.Property.Segments[0].Name;
			if (Arg.Side == Model::EDstPropertySide::Instance)
			{
				if (Name.Equals(Spec->Left, ESearchCase::CaseSensitive)) { LeftArg = &Arg; }
				else if (Name.Equals(Spec->Right, ESearchCase::CaseSensitive)) { RightArg = &Arg; }
				else { return nullptr; }
			}
			else if (Name.Equals(Sugar::InvertProperty, ESearchCase::CaseSensitive))
			{
				continue;
			}
			else if (Spec->OperatorProperty && Name.Equals(Spec->OperatorProperty, ESearchCase::CaseSensitive))
			{
				OperatorArg = &Arg;
			}
			else
			{
				return nullptr;
			}
		}
		if (!LeftArg || LeftArg->Kind != Model::EDstValueKind::Binding || (OperatorArg && OperatorArg->Kind != Model::EDstValueKind::Constant))
		{
			return nullptr;
		}
		if (RightArg && RightArg->Kind != Model::EDstValueKind::Constant && RightArg->Kind != Model::EDstValueKind::Binding
			&& RightArg->Kind != Model::EDstValueKind::Function)
		{
			return nullptr;
		}
		const bool bEnum = Spec == &Sugar::CompareEnum;
		if (bEnum && !RightArg)
		{
			// An enum right side at its default has no value the text could name.
			return nullptr;
		}

		// The operator, as the text spells it.
		const TCHAR* OperatorSpelling = TEXT("==");
		if (OperatorProperty)
		{
			const FString Text = OperatorArg ? OperatorArg->ConstantText : DstExportDefault(*OperatorProperty, Info->GetNodeDefaults());
			FString OperatorName = Text;
			if (Text.IsEmpty())
			{
				const UEnum* Enum = Lowering::GetPropertyEnum(OperatorProperty);
				OperatorName = Enum ? Enum->GetNameStringByValue(0) : FString();
			}
			int32 Colon = INDEX_NONE;
			if (OperatorName.FindLastChar(TEXT(':'), Colon))
			{
				OperatorName = OperatorName.Mid(Colon + 1);
			}
			OperatorSpelling = nullptr;
			for (const Sugar::FComparisonSpelling& Entry : Sugar::Comparisons)
			{
				if (OperatorName.Equals(Entry.OperatorName, ESearchCase::CaseSensitive))
				{
					OperatorSpelling = Entry.Spelling;
				}
			}
			if (!OperatorSpelling)
			{
				return nullptr;
			}
		}

		// The right side.
		const FSourceLeaf LeftLeaf = ResolveSourceLeaf(LeftArg->Source, Scope);
		const FString Display = FString::Printf(TEXT("the comparison %s"), *Node.Key);
		FExprPtr Right;
		bool bRightIsTrue = false;
		if (!RightArg)
		{
			Right = MakeConstantExpr(DstExportDefault(*RightProperty, Info->GetInstanceDefaults()), RightProperty, Display);
		}
		else if (RightArg->Kind == Model::EDstValueKind::Constant)
		{
			if (bEnum)
			{
				const UScriptStruct* InstanceStruct = Cast<UScriptStruct>(Info->InstanceStruct);
				if (!InstanceStruct)
				{
					return nullptr;
				}
				FStructOnScope Scratch(InstanceStruct);
				FOutputDeviceNull Null;
				if (!RightProperty->ImportText_InContainer(*RightArg->ConstantText, Scratch.GetStructMemory(), nullptr, PPF_None, &Null))
				{
					return nullptr;
				}
				const FStateTreeAnyEnum* AnyEnum = RightProperty->ContainerPtrToValuePtr<FStateTreeAnyEnum>(Scratch.GetStructMemory());
				if (!AnyEnum || !AnyEnum->Enum || (LeftLeaf.bKnown && LeftLeaf.Enum && LeftLeaf.Enum != AnyEnum->Enum))
				{
					return nullptr;
				}
				const FString ValueName = AnyEnum->Enum->GetNameStringByValue(static_cast<int64>(AnyEnum->Value));
				if (ValueName.IsEmpty())
				{
					return nullptr;
				}
				Right = MakeNameExpr(ValueName);
			}
			else
			{
				Right = MakeConstantExpr(RightArg->ConstantText, RightProperty, Display);
				bRightIsTrue = Spec == &Sugar::CompareBool && RightArg->ConstantText.ToBool();
			}
		}
		else if (RightArg->Kind == Model::EDstValueKind::Binding)
		{
			Right = RaiseSource(RightArg->Source, Scope, Display);
		}
		else if (RightArg->Function.IsValid())
		{
			Right = RaiseFunction(*RightArg->Function, RightProperty, Scope);
		}
		if (!Right)
		{
			return nullptr;
		}

		// The lowering picks the Compare node from the left side's type (and from a float on the right for an int on the
		// left); it has to pick this node again, or the text means another one.
		if (LeftLeaf.bKnown)
		{
			Lowering::EValueDomain Domain = LeftLeaf.Domain;
			if (Domain == Lowering::EValueDomain::Int)
			{
				bool bOtherIsFloat = false;
				if (RightArg && RightArg->Kind == Model::EDstValueKind::Binding)
				{
					const FSourceLeaf RightLeaf = ResolveSourceLeaf(RightArg->Source, Scope);
					bOtherIsFloat = RightLeaf.bKnown && RightLeaf.Domain == Lowering::EValueDomain::Float;
				}
				else if (RightArg && RightArg->Kind == Model::EDstValueKind::Function)
				{
					bOtherIsFloat = IsRaiseArithmeticExpr(*Right);
				}
				else if (Right->Is<Lang::FDstLiteralExpr>() || Right->Is<Lang::FDstUnaryExpr>())
				{
					int64 Whole = 0;
					bOtherIsFloat = !Lang::TryGetDstInteger(*Right, Whole);
				}
				if (bOtherIsFloat)
				{
					Domain = Lowering::EValueDomain::Float;
				}
			}
			if (PickRaiseCompareSpec(Domain) != Spec)
			{
				return nullptr;
			}
		}

		FExprPtr Left = RaiseSource(LeftArg->Source, Scope, Display);
		if (!Left)
		{
			return nullptr;
		}

		// A comparison of bools against true is the bare path.
		if (Spec == &Sugar::CompareBool && bRightIsTrue)
		{
			return bInvert ? MakeUnaryExpr(Lang::EDstUnaryOp::Not, MoveTemp(Left)) : MoveTemp(Left);
		}

		// Nodes without an operator test equality; their bInvert is `!=`. The others keep it as `not (a < b)`.
		const bool bEqualityOnly = Spec->OperatorProperty == nullptr;
		const TCHAR* Spelling = bEqualityOnly ? (bInvert ? TEXT("!=") : TEXT("==")) : OperatorSpelling;
		FExprPtr Comparison = MakeBinaryExpr(GetRaiseComparisonOp(Spelling), MoveTemp(Left), MoveTemp(Right));
		return !bEqualityOnly && bInvert ? MakeUnaryExpr(Lang::EDstUnaryOp::Not, MoveTemp(Comparison)) : MoveTemp(Comparison);
	}
}
