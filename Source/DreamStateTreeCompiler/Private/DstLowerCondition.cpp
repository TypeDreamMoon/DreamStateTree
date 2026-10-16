// Copyright DreamMoon. All Rights Reserved.
//
// Conditions and utility considerations. The engine keeps them as a flat list; each item has an Operand that joins it to
// what came before and an Indent that opens and closes parentheses. The list is evaluated strictly from left to right
// with no precedence (FStateTreeExecutionContext::TestAllConditionsInternal), so the text's precedence is turned into
// groups first and the groups into the list:
//
//   - an expression is a chain of operands joined by operators; an operand is a leaf or a parenthesised / higher-
//     precedence group;
//   - the first operand of a chain that is itself a group is flattened into the chain (it would evaluate the same way,
//     and it makes `(A or B) and C` and `A or B and C` the fixed points part2-brief.md describes);
//   - an item's Indent is the depth of its innermost group, minus one for the group that starts at it;
//   - an item's Operand is the operator that joins it, or its group, to what comes before; the first item is Copy.

#include "DstLowerPrivate.h"

#include "StateTreeAnyEnum.h"
#include "StateTreeTypes.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	using namespace Lang;
	using namespace Model;
	using Model::FDstNode;

	namespace
	{
		bool IsGroupOperator(const EDstBinaryOp Op, const bool bConsideration)
		{
			return Op == EDstBinaryOp::And || Op == EDstBinaryOp::Or || (bConsideration && Op == EDstBinaryOp::Multiply);
		}

		bool IsComparisonOperator(const EDstBinaryOp Op)
		{
			return Op == EDstBinaryOp::Equal || Op == EDstBinaryOp::NotEqual || Op == EDstBinaryOp::Less || Op == EDstBinaryOp::LessEqual
				|| Op == EDstBinaryOp::Greater || Op == EDstBinaryOp::GreaterEqual;
		}

		EDstOperand ToOperand(const EDstBinaryOp Op)
		{
			switch (Op)
			{
			case EDstBinaryOp::And: return EDstOperand::And;
			case EDstBinaryOp::Or: return EDstOperand::Or;
			default: return EDstOperand::Multiply;
			}
		}

		const Sugar::FComparisonSpelling* FindComparison(const TCHAR* Spelling)
		{
			for (const Sugar::FComparisonSpelling& Entry : Sugar::Comparisons)
			{
				if (FCString::Strcmp(Entry.Spelling, Spelling) == 0)
				{
					return &Entry;
				}
			}
			return nullptr;
		}

		/** A bool literal as an expression, to import `True` into a bool property through the same door as every constant. */
		TUniquePtr<FDstExpr> MakeBoolExpr(const bool bValue, const FDstSpan& Span)
		{
			TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
			Literal->LiteralKind = EDstLiteralKind::Bool;
			Literal->bBool = bValue;
			Literal->Text = bValue ? TEXT("true") : TEXT("false");
			Literal->Span = Span;
			return Literal;
		}

		TUniquePtr<FDstExpr> MakeNameExpr(const FString& Name, const FDstSpan& Span)
		{
			TUniquePtr<FDstNameExpr> Expr = MakeUnique<FDstNameExpr>();
			Expr->Name = Name;
			Expr->Span = Span;
			return Expr;
		}

		/** What an operand is as a value: its type when it names a property, else a guess from the operator. */
		struct FSideType
		{
			const FProperty* Property = nullptr;
			EValueDomain Domain = EValueDomain::Other;
			bool bKnown = false;
		};
	}

	// ------------------------------------------------------------------------------------------
	// Groups
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::BuildGroup(const FDstExpr& Expr, const bool bConsideration, FCondGroup& Group, bool& bOk)
	{
		const FDstExpr& E = StripParens(Expr);
		if (const FDstBinaryExpr* Binary = E.As<FDstBinaryExpr>(); Binary && IsGroupOperator(Binary->Op, bConsideration) && Binary->Lhs && Binary->Rhs)
		{
			// The left side is the first operand: a group there joins this chain.
			BuildGroup(*Binary->Lhs, bConsideration, Group, bOk);
			Group.Ops.Add(ToOperand(Binary->Op));
			AddOperand(*Binary->Rhs, bConsideration, Group, bOk);
			return true;
		}
		FCondOperand Operand;
		Operand.Leaf = &E;
		Group.Operands.Add(MoveTemp(Operand));
		return true;
	}

	bool FDstLowerer::AddOperand(const FDstExpr& Expr, const bool bConsideration, FCondGroup& Group, bool& bOk)
	{
		const FDstExpr& E = StripParens(Expr);
		FCondOperand Operand;
		if (const FDstBinaryExpr* Binary = E.As<FDstBinaryExpr>(); Binary && IsGroupOperator(Binary->Op, bConsideration))
		{
			Operand.Group = MakeUnique<FCondGroup>();
			BuildGroup(E, bConsideration, *Operand.Group, bOk);
		}
		else
		{
			Operand.Leaf = &E;
		}
		Group.Operands.Add(MoveTemp(Operand));
		return true;
	}

	void FDstLowerer::FlattenGroup(const FCondGroup& Group, const int32 Depth, const EDstOperand Connect, TArray<FCondItem>& Items)
	{
		for (int32 Index = 0; Index < Group.Operands.Num(); ++Index)
		{
			const EDstOperand Operand = Index == 0 ? Connect : Group.Ops[Index - 1];
			const FCondOperand& Item = Group.Operands[Index];
			if (Item.Leaf)
			{
				FCondItem Flat;
				Flat.Leaf = Item.Leaf;
				Flat.Operand = Operand;
				Flat.Indent = Depth;
				Items.Add(MoveTemp(Flat));
			}
			else if (Item.Group)
			{
				const int32 Start = Items.Num();
				FlattenGroup(*Item.Group, Depth + 1, Operand, Items);
				// The item that opens the group takes the parenthesis: one level less than the rest of the group.
				if (Items.IsValidIndex(Start))
				{
					Items[Start].Indent -= 1;
				}
			}
		}
	}

	bool FDstLowerer::LowerConditionList(const TArray<const FDstExpr*>& Clauses, const FString& KeyPrefix, const EDstNodeRole Role,
		const FBindScope& Scope, TArray<FDstNode>& OutNodes)
	{
		if (Clauses.IsEmpty() || !Clauses[0])
		{
			return true;
		}
		const int32 ErrorsBefore = Sink.Real.NumErrors();

		// Several `if` clauses on one state are joined with `and`; each after the first is one operand.
		FCondGroup Top;
		bool bOk = true;
		BuildGroup(*Clauses[0], false, Top, bOk);
		for (int32 Index = 1; Index < Clauses.Num(); ++Index)
		{
			if (Clauses[Index])
			{
				Top.Ops.Add(EDstOperand::And);
				AddOperand(*Clauses[Index], false, Top, bOk);
			}
		}

		TArray<FCondItem> Items;
		FlattenGroup(Top, 0, EDstOperand::Copy, Items);

		int32 MaxIndent = 0;
		for (const FCondItem& Item : Items)
		{
			MaxIndent = FMath::Max(MaxIndent, Item.Indent);
		}
		if (MaxIndent > UE::StateTree::MaxExpressionIndent)
		{
			Sink.Error(TEXT("DST5012"), Clauses[0]->Span,
				FString::Printf(TEXT("Parentheses are nested %d deep; the engine's condition list takes at most %d."), MaxIndent, UE::StateTree::MaxExpressionIndent),
				TEXT("Flatten the expression, or move part of it into a template or a subtree."));
		}

		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			FDstNode Node;
			const FString Key = FString::Printf(TEXT("%s[%d]"), *KeyPrefix, Index);
			LowerConditionLeaf(*Items[Index].Leaf, Key, Role, Scope, Node);
			Node.Key = Key;
			Node.Role = Role;
			Node.Operand = Items[Index].Operand;
			Node.Indent = Items[Index].Indent;
			OutNodes.Add(MoveTemp(Node));
		}
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	bool FDstLowerer::LowerConsiderationList(const FDstExpr& Expr, const FString& KeyPrefix, const FBindScope& Scope, TArray<FDstNode>& OutNodes)
	{
		const int32 ErrorsBefore = Sink.Real.NumErrors();
		FCondGroup Top;
		bool bOk = true;
		BuildGroup(Expr, true, Top, bOk);

		TArray<FCondItem> Items;
		FlattenGroup(Top, 0, EDstOperand::Copy, Items);

		int32 MaxIndent = 0;
		for (const FCondItem& Item : Items)
		{
			MaxIndent = FMath::Max(MaxIndent, Item.Indent);
		}
		if (MaxIndent > UE::StateTree::MaxExpressionIndent)
		{
			Sink.Error(TEXT("DST5012"), Expr.Span,
				FString::Printf(TEXT("Parentheses are nested %d deep; the engine's utility list takes at most %d."), MaxIndent, UE::StateTree::MaxExpressionIndent));
		}

		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			FDstNode Node;
			const FString Key = FString::Printf(TEXT("%s[%d]"), *KeyPrefix, Index);
			const FDstExpr& Leaf = *Items[Index].Leaf;
			FFoldedConstant Constant;
			if (Leaf.Kind != EDstNodeKind::CallExpr && TryFold(Leaf, Constant) && Constant.Kind == FFoldedConstant::EKind::Number)
			{
				LowerConstantConsideration(Leaf, Key, Scope, Node);
			}
			else if (Leaf.Kind == EDstNodeKind::CallExpr || Leaf.Kind == EDstNodeKind::NameExpr || Leaf.Kind == EDstNodeKind::MemberExpr)
			{
				LowerNode(Leaf, FString(), Leaf.Span, Key, EDstNodeRole::Consideration, EDstNodeCategory::Consideration, Scope, Node);
			}
			else
			{
				Sink.Error(TEXT("DST5013"), Leaf.Span, TEXT("This is not a utility consideration."),
					TEXT("A utility is made of consideration nodes and numbers, joined with `*`, `and` and `or`: utility FloatInput(Player.Distance, (0, 3000)) * 0.5."));
			}
			Node.Key = Key;
			Node.Role = EDstNodeRole::Consideration;
			Node.Operand = Items[Index].Operand;
			Node.Indent = Items[Index].Indent;
			OutNodes.Add(MoveTemp(Node));
		}
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	bool FDstLowerer::LowerConstantConsideration(const FDstExpr& Expr, const FString& Key, const FBindScope& Scope, FDstNode& OutNode)
	{
		FNodeInfoPtr Info;
		const FDstNodeType* Type = nullptr;
		if (!MakeSugarNode(Sugar::ConstantConsideration, Key, EDstNodeRole::Consideration, Expr.Span, OutNode, Info, Type))
		{
			return false;
		}
		FNodeBuild Build;
		Build.Node = &OutNode;
		Build.Info = Info;
		Build.Scope = Scope;
		Build.CallSpan = Expr.Span;
		Build.DisplayName = Type->Name;

		const FProperty* Property = Info->InstanceStruct ? Info->InstanceStruct->FindPropertyByName(Sugar::ConstantConsiderationValue) : nullptr;
		if (!Property)
		{
			return false;
		}
		return LowerValue(Build, EDstPropertySide::Instance, *Property, Expr);
	}

	// ------------------------------------------------------------------------------------------
	// Leaves
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::SetInvert(FNodeBuild& Build, const FDstSpan& Span)
	{
		const FProperty* Property = Build.Info.IsValid() && Build.Info->NodeStruct ? Build.Info->NodeStruct->FindPropertyByName(Sugar::InvertProperty) : nullptr;
		if (!Property || !Property->IsA<FBoolProperty>())
		{
			Sink.Error(TEXT("DST5010"), Span,
				FString::Printf(TEXT("`%s` has no bInvert, so `not` cannot be applied to it."), *Build.DisplayName),
				TEXT("Compare it to false, or use a condition node that can be inverted."));
			return false;
		}
		// Inverting twice cancels: drop the argument that is already there.
		for (int32 Index = 0; Index < Build.Node->Args.Num(); ++Index)
		{
			const FDstArg& Arg = Build.Node->Args[Index];
			if (Arg.Side == EDstPropertySide::Node && Arg.Property.ToString() == Sugar::InvertProperty)
			{
				Build.Node->Args.RemoveAt(Index);
				return true;
			}
		}
		const TUniquePtr<FDstExpr> True = MakeBoolExpr(true, Span);
		FString Text;
		FString Error;
		if (!DstLiteralToExportText(*True, *Property, ValueContext, Text, Error))
		{
			Sink.Error(TEXT("DST4008"), Span, Error);
			return false;
		}
		AddConstantArg(Build, EDstPropertySide::Node, *Property, Text);
		return true;
	}

	bool FDstLowerer::LowerConditionLeaf(const FDstExpr& Leaf, const FString& Key, const EDstNodeRole Role, const FBindScope& Scope, FDstNode& OutNode)
	{
		bool bInvert = false;
		const FDstExpr* E = &StripParens(Leaf);
		while (const FDstUnaryExpr* Unary = E->As<FDstUnaryExpr>())
		{
			if (Unary->Op != EDstUnaryOp::Not || !Unary->Operand)
			{
				break;
			}
			bInvert = !bInvert;
			E = &StripParens(*Unary->Operand);
		}

		if (const FDstBinaryExpr* Binary = E->As<FDstBinaryExpr>())
		{
			if (IsGroupOperator(Binary->Op, false))
			{
				Sink.Error(TEXT("DST5010"), E->Span, TEXT("`not` applies to one condition, not to a group of them."),
					TEXT("Write the negation on each condition, or negate a comparison: not (A and B) is not A or not B."));
				AddPlace(Key, E->Span);
				return false;
			}
			if (IsComparisonOperator(Binary->Op))
			{
				return LowerCompare(*Binary, bInvert, Key, Role, Scope, OutNode);
			}
			Sink.Error(TEXT("DST5013"), E->Span, TEXT("This is not a condition."),
				TEXT("A condition is a condition node call, a comparison or a bool path."));
			AddPlace(Key, E->Span);
			return false;
		}

		if (E->Kind == EDstNodeKind::CallExpr)
		{
			FNodeInfoPtr Info;
			const bool bOk = LowerNode(*E, FString(), E->Span, Key, Role, EDstNodeCategory::Condition, Scope, OutNode, &Info);
			if (bOk && bInvert)
			{
				FNodeBuild Build;
				Build.Node = &OutNode;
				Build.Info = Info;
				Build.Scope = Scope;
				Build.CallSpan = E->Span;
				Build.DisplayName = Info.IsValid() ? Info->Type->Name : FString();
				SetInvert(Build, E->Span);
				SortArgs(OutNode);
			}
			return bOk;
		}

		if (E->Kind == EDstNodeKind::NameExpr || E->Kind == EDstNodeKind::MemberExpr)
		{
			if (IsBindingPath(*E, Scope))
			{
				return LowerBoolPath(*E, bInvert, Key, Role, Scope, OutNode);
			}
			// A condition node written without parentheses.
			const FString Written = GetDstPathString(*E);
			if (!Written.IsEmpty() && !FDstNameRegistry::Get().ResolveNode(Written, Namespaces).IsEmpty())
			{
				FNodeInfoPtr Info;
				const bool bOk = LowerNode(*E, FString(), E->Span, Key, Role, EDstNodeCategory::Condition, Scope, OutNode, &Info);
				if (bOk && bInvert)
				{
					FNodeBuild Build;
					Build.Node = &OutNode;
					Build.Info = Info;
					Build.Scope = Scope;
					Build.CallSpan = E->Span;
					Build.DisplayName = Info.IsValid() ? Info->Type->Name : FString();
					SetInvert(Build, E->Span);
					SortArgs(OutNode);
				}
				return bOk;
			}
			TArray<FString> Segments;
			TryGetDstPath(*E, Segments);
			TArray<FString> Roots = { TEXT("Params"), TEXT("Event") };
			for (const FContext& Context : Contexts)
			{
				Roots.Add(Context.Name);
			}
			for (const FBindableNode& Global : GlobalNodes)
			{
				Roots.Add(Global.Name);
			}
			Sink.Error(TEXT("DST5001"), E->Span,
				FString::Printf(TEXT("`%s` is neither a path nor a condition node."), Written.IsEmpty() ? *DescribeExpr(*E) : *Written),
				MakeSuggestionHint(Segments.IsEmpty() ? Written : Segments[0], Roots));
			AddPlace(Key, E->Span);
			return false;
		}

		Sink.Error(TEXT("DST5013"), E->Span, TEXT("This is not a condition."),
			TEXT("A condition is a condition node call, a comparison or a bool path."));
		AddPlace(Key, E->Span);
		return false;
	}

	bool FDstLowerer::LowerBoolPath(const FDstExpr& PathExpr, const bool bInvert, const FString& Key, const EDstNodeRole Role, const FBindScope& Scope, FDstNode& OutNode)
	{
		FNodeInfoPtr Info;
		const FDstNodeType* Type = nullptr;
		if (!MakeSugarNode(Sugar::CompareBool.StructPath, Key, Role, PathExpr.Span, OutNode, Info, Type))
		{
			return false;
		}
		FNodeBuild Build;
		Build.Node = &OutNode;
		Build.Info = Info;
		Build.Scope = Scope;
		Build.CallSpan = PathExpr.Span;
		Build.DisplayName = Type->Name;

		FResolvedSource Source;
		if (!ResolvePath(PathExpr, Scope, Source))
		{
			return false;
		}
		if (GetValueDomain(Source.Leaf) != EValueDomain::Bool)
		{
			Sink.Error(TEXT("DST5013"), PathExpr.Span,
				FString::Printf(TEXT("`%s` is %s, not a bool, so it is not a condition on its own."), *Source.Display, Source.Leaf ? *DescribeProperty(Source.Leaf) : TEXT("an object")),
				FString::Printf(TEXT("Compare it: %s > 0, or == a value."), *Source.Display));
			return false;
		}

		const FProperty* Left = Info->InstanceStruct->FindPropertyByName(Sugar::CompareBool.Left);
		const FProperty* Right = Info->InstanceStruct->FindPropertyByName(Sugar::CompareBool.Right);
		if (!Left || !Right || !CheckBindable(Source, *Left, PathExpr.Span, FString::Printf(TEXT("`%s`"), *Build.DisplayName)))
		{
			return false;
		}
		MarkGiven(Build, EDstPropertySide::Instance, *Left);
		MarkGiven(Build, EDstPropertySide::Instance, *Right);
		FDstPropertyPath Path;
		FDstPathSegment Segment;
		Segment.Name = Left->GetName();
		Path.Segments.Add(MoveTemp(Segment));
		AddBindingArg(Build, EDstPropertySide::Instance, Path, EDstValueKind::Binding, Source.Ref);

		// The comparison is against true.
		const TUniquePtr<FDstExpr> True = MakeBoolExpr(true, PathExpr.Span);
		FString Text;
		FString Error;
		if (!DstLiteralToExportText(*True, *Right, ValueContext, Text, Error))
		{
			Sink.Error(TEXT("DST4008"), PathExpr.Span, Error);
			return false;
		}
		AddConstantArg(Build, EDstPropertySide::Instance, *Right, Text);

		bool bOk = true;
		if (bInvert)
		{
			bOk = SetInvert(Build, PathExpr.Span);
		}
		SortArgs(OutNode);
		return bOk;
	}

	// ------------------------------------------------------------------------------------------
	// Comparisons
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerCompare(const FDstBinaryExpr& Compare, bool bInvert, const FString& Key, const EDstNodeRole Role, const FBindScope& Scope, FDstNode& OutNode)
	{
		const FDstExpr& LeftExpr = StripParens(*Compare.Lhs);
		const FDstExpr& RightExpr = StripParens(*Compare.Rhs);
		const EValueClass LeftClass = ClassifyValue(LeftExpr, Scope);
		const EValueClass RightClass = ClassifyValue(RightExpr, Scope);
		AddPlace(Key, Compare.Span);

		if (LeftClass == EValueClass::Constant && RightClass == EValueClass::Constant)
		{
			Sink.Error(TEXT("DST5009"), Compare.Span, TEXT("Both sides of the comparison are constants."),
				TEXT("One side reads something that can change: a path, an operation on one, or a property function."));
			return false;
		}
		if (LeftClass == EValueClass::Ref || RightClass == EValueClass::Ref || LeftClass == EValueClass::Mixed || RightClass == EValueClass::Mixed)
		{
			Sink.Error(TEXT("DST5013"), Compare.Span, TEXT("A comparison compares values: paths, operations, property function calls or constants."));
			return false;
		}

		// The side that reads something goes left; a constant on the left swaps the sides and mirrors the operator.
		const bool bSwap = LeftClass == EValueClass::Constant;
		const FDstExpr& Bound = bSwap ? RightExpr : LeftExpr;
		const FDstExpr& Other = bSwap ? LeftExpr : RightExpr;
		const EValueClass BoundClass = bSwap ? RightClass : LeftClass;
		const EValueClass OtherClass = bSwap ? LeftClass : RightClass;

		const Sugar::FComparisonSpelling* Written = FindComparison(GetDstBinaryOpSpelling(Compare.Op));
		const Sugar::FComparisonSpelling* Effective = Written && bSwap ? FindComparison(Written->MirroredSpelling) : Written;
		if (!Effective)
		{
			Sink.Error(TEXT("DST5013"), Compare.Span, TEXT("This comparison operator is not supported."));
			return false;
		}

		// What the bound side is.
		auto TypeOf = [this, &Scope](const FDstExpr& Expr, const EValueClass Class, FResolvedSource* OutSource) -> FSideType
		{
			FSideType Result;
			if (Class == EValueClass::Binding)
			{
				FResolvedSource Source;
				if (ResolvePath(Expr, Scope, Source))
				{
					Result.Property = Source.Leaf;
					Result.Domain = Source.Leaf ? GetValueDomain(Source.Leaf) : EValueDomain::Object;
					Result.bKnown = true;
					if (OutSource)
					{
						*OutSource = Source;
					}
				}
			}
			else if (Class == EValueClass::Operation)
			{
				const FDstExpr& E = StripParens(Expr);
				const FDstBinaryExpr* Binary = E.As<FDstBinaryExpr>();
				const FDstUnaryExpr* Unary = E.As<FDstUnaryExpr>();
				const bool bLogic = (Binary && IsGroupOperator(Binary->Op, false)) || (Unary && Unary->Op == EDstUnaryOp::Not);
				Result.Domain = bLogic ? EValueDomain::Bool : EValueDomain::Float;
				Result.bKnown = true;
			}
			else if (Class == EValueClass::Call)
			{
				const FDstCallExpr* Call = StripParens(Expr).As<FDstCallExpr>();
				const FDstNodeType* Type = nullptr;
				if (Call && Call->Callee && ResolveNodeType(*Call->Callee, EDstNodeCategory::PropertyFunction, EDstNodeCategory::Unknown, Expr.Span, Type))
				{
					FNodeInfoPtr Info = GetNodeInfo(*Type);
					if (Info.IsValid() && Info->InstanceStruct)
					{
						const FProperty* Output = nullptr;
						static const TCHAR* const Names[] = { TEXT("Result"), TEXT("bResult"), TEXT("Output") };
						for (const TCHAR* Name : Names)
						{
							const FProperty* Candidate = Info->InstanceStruct->FindPropertyByName(Name);
							if (Candidate && GetPropertyUse(*Candidate) == EPropertyUse::Output)
							{
								Output = Candidate;
								break;
							}
						}
						Result.Property = Output;
						Result.Domain = GetValueDomain(Output);
						Result.bKnown = Output != nullptr;
					}
				}
			}
			return Result;
		};

		FResolvedSource BoundSource;
		const FSideType BoundType = TypeOf(Bound, BoundClass, &BoundSource);
		if (!BoundType.bKnown)
		{
			return false;
		}
		FSideType OtherType;
		if (OtherClass != EValueClass::Constant)
		{
			OtherType = TypeOf(Other, OtherClass, nullptr);
			if (!OtherType.bKnown)
			{
				return false;
			}
		}

		// Which Compare node.
		const Sugar::FCompareNode* Node = nullptr;
		EValueDomain Domain = BoundType.Domain;
		if (Domain == EValueDomain::Int)
		{
			// A float on the other side (a float path, or a fraction) makes it a float comparison.
			FFoldedConstant Folded;
			const bool bOtherFraction = OtherClass == EValueClass::Constant && TryFold(Other, Folded) && Folded.Kind == FFoldedConstant::EKind::Number && !Folded.bWhole;
			if (OtherType.Domain == EValueDomain::Float || bOtherFraction)
			{
				Domain = EValueDomain::Float;
			}
		}
		switch (Domain)
		{
		case EValueDomain::Float: Node = &Sugar::CompareFloat; break;
		case EValueDomain::Int: Node = &Sugar::CompareInt; break;
		case EValueDomain::Bool: Node = &Sugar::CompareBool; break;
		case EValueDomain::Enum: Node = &Sugar::CompareEnum; break;
		case EValueDomain::Name: Node = &Sugar::CompareName; break;
		default: break;
		}
		if (!Node)
		{
			Sink.Error(TEXT("DST5008"), Compare.Span,
				FString::Printf(TEXT("There is no comparison for %s."), BoundType.Property ? *DescribeProperty(BoundType.Property) : TEXT("this type")),
				TEXT("Comparisons exist for numbers, bools, enums and names. For an object, use ObjectIsValid."));
			return false;
		}

		// `==` and `!=` only, when the node has no operator.
		const bool bNoOperator = Node->OperatorProperty == nullptr;
		if (bNoOperator && FCString::Strcmp(Effective->Spelling, TEXT("==")) != 0 && FCString::Strcmp(Effective->Spelling, TEXT("!=")) != 0)
		{
			Sink.Error(TEXT("DST5008"), Compare.Span,
				FString::Printf(TEXT("`%s` does not work on %s: only `==` and `!=` do."), GetDstBinaryOpSpelling(Compare.Op),
					Domain == EValueDomain::Enum ? TEXT("an enum") : Domain == EValueDomain::Bool ? TEXT("a bool") : TEXT("a name")));
			return false;
		}
		if (bNoOperator && FCString::Strcmp(Effective->Spelling, TEXT("!=")) == 0)
		{
			bInvert = !bInvert;
		}

		FNodeInfoPtr Info;
		const FDstNodeType* Type = nullptr;
		if (!MakeSugarNode(Node->StructPath, Key, Role, Compare.Span, OutNode, Info, Type))
		{
			return false;
		}
		FNodeBuild Build;
		Build.Node = &OutNode;
		Build.Info = Info;
		Build.Scope = Scope;
		Build.CallSpan = Compare.Span;
		Build.DisplayName = Type->Name;

		const FProperty* LeftProperty = Info->InstanceStruct ? Info->InstanceStruct->FindPropertyByName(Node->Left) : nullptr;
		const FProperty* RightProperty = Info->InstanceStruct ? Info->InstanceStruct->FindPropertyByName(Node->Right) : nullptr;
		if (!LeftProperty || !RightProperty)
		{
			Sink.Error(TEXT("DST4007"), Compare.Span, FString::Printf(TEXT("`%s` does not have the properties a comparison needs."), *Type->Name));
			return false;
		}

		bool bOk = true;

		// Left reads. The properties of a Compare node are named by the sugar table, the user never sees them.
		MarkGiven(Build, EDstPropertySide::Instance, *LeftProperty);
		FDstPropertyPath LeftPath;
		FDstPathSegment LeftSegment;
		LeftSegment.Name = LeftProperty->GetName();
		LeftPath.Segments.Add(MoveTemp(LeftSegment));
		if (BoundClass == EValueClass::Binding)
		{
			bOk &= CheckBindable(BoundSource, *LeftProperty, Bound.Span, FString::Printf(TEXT("the left side of `%s`"), *Type->Name));
			if (bOk)
			{
				AddBindingArg(Build, EDstPropertySide::Instance, LeftPath, EDstValueKind::Binding, BoundSource.Ref);
			}
		}
		else if (BoundClass == EValueClass::Operation)
		{
			bOk &= LowerOperation(Build, EDstPropertySide::Instance, LeftPath, *LeftProperty, Bound);
		}
		else
		{
			bOk &= LowerFunctionCall(Build, EDstPropertySide::Instance, LeftPath, *LeftProperty, Bound);
		}

		// Right.
		if (Node == &Sugar::CompareEnum && OtherClass == EValueClass::Constant)
		{
			bOk &= LowerEnumRight(Build, *Info, *RightProperty, GetPropertyEnum(BoundType.Property), Other);
		}
		else
		{
			bOk &= LowerValue(Build, EDstPropertySide::Instance, *RightProperty, Other);
		}

		// The operator and the inversion.
		if (!bNoOperator)
		{
			const FProperty* OperatorProperty = Info->NodeStruct ? Info->NodeStruct->FindPropertyByName(Node->OperatorProperty) : nullptr;
			if (OperatorProperty)
			{
				const TUniquePtr<FDstExpr> Word = MakeNameExpr(Effective->OperatorName, Compare.Span);
				FString Text;
				FString Error;
				if (DstLiteralToExportText(*Word, *OperatorProperty, ValueContext, Text, Error))
				{
					AddConstantArg(Build, EDstPropertySide::Node, *OperatorProperty, Text);
				}
				else
				{
					Sink.Error(TEXT("DST4008"), Compare.Span, Error);
					bOk = false;
				}
			}
		}
		if (bInvert)
		{
			bOk &= SetInvert(Build, Compare.Span);
		}
		SortArgs(OutNode);
		return bOk;
	}

	bool FDstLowerer::LowerEnumRight(FNodeBuild& Build, const FNodeInfo& Info, const FProperty& RightProperty, const UEnum* Enum, const FDstExpr& Value)
	{
		MarkGiven(Build, EDstPropertySide::Instance, RightProperty);
		if (!Enum)
		{
			Sink.Error(TEXT("DST5008"), Value.Span, TEXT("The other side of the comparison is not an enum, so the value cannot be told which enum it is from."),
				TEXT("Compare an enum property with a value of its enum."));
			return false;
		}

		FString Written;
		if (const FDstLiteralExpr* Literal = Value.As<FDstLiteralExpr>())
		{
			Written = Literal->LiteralKind == EDstLiteralKind::String ? Literal->Text : FString();
		}
		else
		{
			Written = GetDstPathString(Value);
		}
		// `EMode::Walk` and `Walk` both name the value; the last segment is the value's name.
		int32 Colons = INDEX_NONE;
		if (Written.FindLastChar(TEXT('.'), Colons))
		{
			Written = Written.Mid(Colons + 1);
		}
		const int64 EnumValue = Written.IsEmpty() ? INDEX_NONE : Enum->GetValueByNameString(Written);
		if (EnumValue == INDEX_NONE)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				Names.Add(Enum->GetNameStringByIndex(Index));
			}
			Sink.Error(TEXT("DST4008"), Value.Span, FString::Printf(TEXT("`%s` is not a value of %s."), *DescribeExpr(Value), *Enum->GetName()),
				FString::Printf(TEXT("The values are %s."), *FString::Join(Names, TEXT(", "))));
			return false;
		}

		// The value is exported through the same door as every constant: a scratch copy of the node's data.
		const UScriptStruct* InstanceStruct = Cast<UScriptStruct>(Info.InstanceStruct);
		if (!InstanceStruct)
		{
			return false;
		}
		FStructOnScope Scratch(InstanceStruct);
		FStateTreeAnyEnum* AnyEnum = RightProperty.ContainerPtrToValuePtr<FStateTreeAnyEnum>(Scratch.GetStructMemory());
		AnyEnum->Enum = const_cast<UEnum*>(Enum);
		AnyEnum->Value = static_cast<uint32>(EnumValue);
		AddConstantArg(Build, EDstPropertySide::Instance, RightProperty, DstExportDefault(RightProperty, Scratch.GetStructMemory()));
		return true;
	}
}
