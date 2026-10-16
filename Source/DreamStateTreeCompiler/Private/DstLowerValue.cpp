// Copyright DreamMoon. All Rights Reserved.
//
// One argument value. What is written decides what it becomes (DstSugar.h holds the tables):
//   a literal, a tuple, an enum value, a tag, an asset name   -> a constant: the export text of the whole property;
//   `Params.X`, `Info.Target`, `Event.Offer`                   -> a binding;
//   `ref Params.X`                                             -> a property reference;
//   `1 + Params.X * 0.5`, `-X`, `a and not b`                  -> a property function node bound into the property;
//   `GetActorLocation(Player.Pawn)`                            -> a property function call, likewise;
//   `Prop -> Params.X`                                         -> an output binding;
// and a whole operation of constants is folded into one constant first.
//
// A tuple, a list or a reference to a tree can be constant in some places and fed from elsewhere in others
// (`(Min = Params.A, Max = 5)`, `[(Value = Params.X)]`, `ST_Shoot(Target = Params.T, Range = 800)`). The constant is
// then the whole property with the fed places left at their defaults, and each fed place is an argument with a longer
// path (`Interval.Min`, `QueryConfig[0].Value`, `StateTree.Parameters.Value<PropertyBag>.Target`).

#include "DstLowerPrivate.h"

#include "Misc/PackageName.h"
#include "PropertyBindingTypes.h"
#include "StateTree.h"
#include "StateTreePropertyBindings.h"
#include "StateTreePropertyRefHelpers.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	using namespace Lang;
	using namespace Model;
	using Model::FDstNode;

	namespace
	{
		bool IsComparison(const EDstBinaryOp Op)
		{
			return Op == EDstBinaryOp::Equal || Op == EDstBinaryOp::NotEqual || Op == EDstBinaryOp::Less || Op == EDstBinaryOp::LessEqual
				|| Op == EDstBinaryOp::Greater || Op == EDstBinaryOp::GreaterEqual;
		}

		FString DescribeTarget(const FNodeBuild& Build, const FProperty& Property)
		{
			return FString::Printf(TEXT("`%s.%s`"), *Build.DisplayName, *Property.GetName());
		}

		/** The property a property function writes its result to: Result, bResult, Output, or the only Output property. */
		const FProperty* FindFunctionOutput(const FNodeInfo& Info)
		{
			if (!Info.InstanceStruct)
			{
				return nullptr;
			}
			static const TCHAR* const Names[] = { TEXT("Result"), TEXT("bResult"), TEXT("Output") };
			for (const TCHAR* Name : Names)
			{
				const FProperty* Property = Info.InstanceStruct->FindPropertyByName(Name);
				if (Property && GetPropertyUse(*Property) == EPropertyUse::Output)
				{
					return Property;
				}
			}
			return UE::StateTree::GetStructSingleOutputProperty(*Info.InstanceStruct);
		}

		bool ContainsDynamic(const FDstExpr& Expr, const FDstLowerer& Lowerer, const FBindScope& Scope);

		bool ContainsDynamicItem(const FDstArgument& Item, const FDstLowerer& Lowerer, const FBindScope& Scope)
		{
			return Item.Value && ContainsDynamic(*Item.Value, Lowerer, Scope);
		}

		/** Whether anything in the expression is not a constant: a path, an operation on one, a call. */
		bool ContainsDynamic(const FDstExpr& Expr, const FDstLowerer& Lowerer, const FBindScope& Scope)
		{
			const FDstExpr& E = StripParens(Expr);
			switch (E.Kind)
			{
			case EDstNodeKind::NameExpr:
			case EDstNodeKind::MemberExpr:
				return Lowerer.IsBindingPath(E, Scope);
			case EDstNodeKind::RefExpr:
				return true;
			case EDstNodeKind::CallExpr:
			{
				const FDstCallExpr& Call = static_cast<const FDstCallExpr&>(E);
				const FDstNameExpr* Name = Call.Callee ? Call.Callee->As<FDstNameExpr>() : nullptr;
				return !(Name && Name->Name.Equals(TEXT("Path"), ESearchCase::CaseSensitive));
			}
			case EDstNodeKind::UnaryExpr:
				return ContainsDynamic(*static_cast<const FDstUnaryExpr&>(E).Operand, Lowerer, Scope);
			case EDstNodeKind::BinaryExpr:
			{
				const FDstBinaryExpr& Binary = static_cast<const FDstBinaryExpr&>(E);
				return ContainsDynamic(*Binary.Lhs, Lowerer, Scope) || ContainsDynamic(*Binary.Rhs, Lowerer, Scope);
			}
			case EDstNodeKind::TupleExpr:
				for (const FDstArgumentPtr& Item : static_cast<const FDstTupleExpr&>(E).Items)
				{
					if (Item && ContainsDynamicItem(*Item, Lowerer, Scope))
					{
						return true;
					}
				}
				return false;
			case EDstNodeKind::ListExpr:
				for (const FDstExprPtr& Item : static_cast<const FDstListExpr&>(E).Items)
				{
					if (Item && ContainsDynamic(*Item, Lowerer, Scope))
					{
						return true;
					}
				}
				return false;
			default:
				return false;
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Constants
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::TryFold(const FDstExpr& Expr, FFoldedConstant& OutValue, const EValueDomain Domain) const
	{
		const FDstExpr& E = StripParens(Expr);
		OutValue = FFoldedConstant();
		switch (E.Kind)
		{
		case EDstNodeKind::LiteralExpr:
		{
			const FDstLiteralExpr& Literal = static_cast<const FDstLiteralExpr&>(E);
			if (Literal.LiteralKind == EDstLiteralKind::Number)
			{
				int64 Whole = 0;
				OutValue.Kind = FFoldedConstant::EKind::Number;
				OutValue.Number = Literal.Number;
				OutValue.bWhole = TryGetDstInteger(E, Whole);
				if (OutValue.bWhole)
				{
					OutValue.Number = static_cast<double>(Whole);
				}
				return true;
			}
			if (Literal.LiteralKind == EDstLiteralKind::Bool)
			{
				OutValue.Kind = FFoldedConstant::EKind::Bool;
				OutValue.bBool = Literal.bBool;
				return true;
			}
			return false;
		}
		case EDstNodeKind::UnaryExpr:
		{
			const FDstUnaryExpr& Unary = static_cast<const FDstUnaryExpr&>(E);
			FFoldedConstant Operand;
			if (!Unary.Operand || !TryFold(*Unary.Operand, Operand, Domain))
			{
				return false;
			}
			if (Unary.Op == EDstUnaryOp::Negate && Operand.Kind == FFoldedConstant::EKind::Number)
			{
				OutValue = Operand;
				OutValue.Number = -Operand.Number;
				return true;
			}
			if (Unary.Op == EDstUnaryOp::Not && Operand.Kind == FFoldedConstant::EKind::Bool)
			{
				OutValue = Operand;
				OutValue.bBool = !Operand.bBool;
				return true;
			}
			return false;
		}
		case EDstNodeKind::BinaryExpr:
		{
			const FDstBinaryExpr& Binary = static_cast<const FDstBinaryExpr&>(E);
			FFoldedConstant Left;
			FFoldedConstant Right;
			if (!Binary.Lhs || !Binary.Rhs || !TryFold(*Binary.Lhs, Left, Domain) || !TryFold(*Binary.Rhs, Right, Domain) || Left.Kind != Right.Kind)
			{
				return false;
			}
			if (Left.Kind == FFoldedConstant::EKind::Bool)
			{
				if (Binary.Op == EDstBinaryOp::And || Binary.Op == EDstBinaryOp::Or)
				{
					OutValue.Kind = FFoldedConstant::EKind::Bool;
					OutValue.bBool = Binary.Op == EDstBinaryOp::And ? (Left.bBool && Right.bBool) : (Left.bBool || Right.bBool);
					return true;
				}
				return false;
			}

			double Value = 0.0;
			switch (Binary.Op)
			{
			case EDstBinaryOp::Add: Value = Left.Number + Right.Number; break;
			case EDstBinaryOp::Subtract: Value = Left.Number - Right.Number; break;
			case EDstBinaryOp::Multiply: Value = Left.Number * Right.Number; break;
			case EDstBinaryOp::Divide:
				if (Right.Number == 0.0)
				{
					return false;
				}
				Value = Left.Number / Right.Number;
				// Whole numbers divide like the engine's Int function does: the fraction is dropped.
				if (Domain == EValueDomain::Int && Left.bWhole && Right.bWhole)
				{
					Value = FMath::TruncToDouble(Value);
				}
				break;
			default:
				return false;
			}
			OutValue.Kind = FFoldedConstant::EKind::Number;
			OutValue.Number = Value;
			OutValue.bWhole = Left.bWhole && Right.bWhole && Value == FMath::RoundToDouble(Value);
			return true;
		}
		default:
			return false;
		}
	}

	TUniquePtr<FDstExpr> FDstLowerer::MakeConstantExpr(const FFoldedConstant& Value, const FDstSpan& Span) const
	{
		auto MakeLiteral = [&Span](const double Number) -> TUniquePtr<FDstLiteralExpr>
		{
			TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
			Literal->LiteralKind = EDstLiteralKind::Number;
			Literal->Number = Number;
			Literal->Text = FormatDstNumber(Number);
			Literal->Span = Span;
			return Literal;
		};

		if (Value.Kind == FFoldedConstant::EKind::Bool)
		{
			TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
			Literal->LiteralKind = EDstLiteralKind::Bool;
			Literal->bBool = Value.bBool;
			Literal->Text = Value.bBool ? TEXT("true") : TEXT("false");
			Literal->Span = Span;
			return Literal;
		}

		// A negative number is a minus in front of its digits, as the parser reads it.
		if (Value.Number < 0.0)
		{
			if (Value.bWhole && Value.Number >= -9007199254740992.0)
			{
				FDstExprPtr Whole = MakeDstInteger(static_cast<int64>(Value.Number));
				if (Whole)
				{
					Whole->Span = Span;
					return Whole;
				}
			}
			TUniquePtr<FDstUnaryExpr> Negate = MakeUnique<FDstUnaryExpr>();
			Negate->Op = EDstUnaryOp::Negate;
			Negate->Operand = MakeLiteral(-Value.Number);
			Negate->Span = Span;
			return Negate;
		}
		return MakeLiteral(Value.Number);
	}

	bool FDstLowerer::ConstantToText(const FDstExpr& Value, const FProperty& Property, FString& OutText, const FDstSpan& Span)
	{
		const FDstExpr& V = StripParens(Value);
		TUniquePtr<FDstExpr> Folded;
		const FDstExpr* Source = &V;
		if (V.Kind == EDstNodeKind::UnaryExpr || V.Kind == EDstNodeKind::BinaryExpr)
		{
			FFoldedConstant Constant;
			if (TryFold(V, Constant, GetValueDomain(&Property)))
			{
				Folded = MakeConstantExpr(Constant, V.Span);
				Source = Folded.Get();
			}
		}

		FString Error;
		if (DstLiteralToExportText(*Source, Property, ValueContext, OutText, Error))
		{
			return true;
		}

		// A bare name that is neither a path nor a value of the property's type is most likely a mistyped path.
		const EValueDomain Domain = GetValueDomain(&Property);
		const bool bNameLike = V.Kind == EDstNodeKind::NameExpr || V.Kind == EDstNodeKind::MemberExpr;
		const bool bNamesAreValues = Domain == EValueDomain::Enum || Domain == EValueDomain::Object || Domain == EValueDomain::Name
			|| Domain == EValueDomain::Struct || IsGameplayTagProperty(&Property);
		if (bNameLike && !bNamesAreValues)
		{
			TArray<FString> Roots = { TEXT("Params"), TEXT("Event") };
			for (const FContext& Context : Contexts)
			{
				Roots.Add(Context.Name);
			}
			for (const FBindableNode& Global : GlobalNodes)
			{
				Roots.Add(Global.Name);
			}
			const FString Path = GetDstPathString(V);
			TArray<FString> Segments;
			TryGetDstPath(V, Segments);
			Sink.Error(TEXT("DST5001"), Span,
				FString::Printf(TEXT("`%s` is not something a path can start with, and not a %s value either."), Segments.IsEmpty() ? *Path : *Segments[0], *DescribeProperty(&Property)),
				MakeSuggestionHint(Segments.IsEmpty() ? Path : Segments[0], Roots));
			return false;
		}
		Sink.Error(TEXT("DST4008"), Span, Error);
		return false;
	}

	void FDstLowerer::AddConstantArg(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property, const FString& Text)
	{
		// Only what differs from the default is recorded.
		if (Build.Info.IsValid())
		{
			const void* Defaults = Side == EDstPropertySide::Instance ? Build.Info->GetInstanceDefaults() : Build.Info->GetNodeDefaults();
			if (Defaults && DstExportDefault(Property, Defaults) == Text)
			{
				return;
			}
		}
		FDstArg Arg;
		Arg.Side = Side;
		FDstPathSegment Segment;
		Segment.Name = Property.GetName();
		Arg.Property.Segments.Add(MoveTemp(Segment));
		Arg.Kind = EDstValueKind::Constant;
		Arg.ConstantText = Text;
		Build.Node->Args.Add(MoveTemp(Arg));
	}

	void FDstLowerer::AddBindingArg(FNodeBuild& Build, const EDstPropertySide Side, const FDstPropertyPath& Path, const EDstValueKind Kind, const FDstSourceRef& Source)
	{
		FDstArg Arg;
		Arg.Side = Side;
		Arg.Property = Path;
		Arg.Kind = Kind;
		Arg.Source = Source;
		Build.Node->Args.Add(MoveTemp(Arg));
	}

	bool FDstLowerer::LowerConstant(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property, const FDstExpr& Value)
	{
		const FDstExpr& V = StripParens(Value);
		if (Side == EDstPropertySide::Instance && GetPropertyUse(Property) == EPropertyUse::Input && !Build.bInputsMayBeConstant)
		{
			Sink.Error(TEXT("DST5004"), V.Span,
				FString::Printf(TEXT("%s is an Input: it is bound to a path, not given a value."), *DescribeTarget(Build, Property)),
				FString::Printf(TEXT("Write %s = Params.Something (or a context, a global node, a named task)."), *Property.GetName()));
			return false;
		}
		FString Text;
		if (!ConstantToText(V, Property, Text, V.Span))
		{
			return false;
		}
		AddConstantArg(Build, Side, Property, Text);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Classification
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::TryGetReferenceParts(const FDstExpr& Value, const FBindScope& Scope, const FDstExpr*& OutAsset, const TArray<FDstArgumentPtr>*& OutArguments) const
	{
		OutAsset = nullptr;
		OutArguments = nullptr;
		const FDstExpr& V = StripParens(Value);

		if (const FDstNameExpr* Name = V.As<FDstNameExpr>(); Name && !Name->bQuoted && Name->Name.Equals(TEXT("None"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (const FDstCallExpr* Call = V.As<FDstCallExpr>())
		{
			FString Ignored;
			if (TryGetDstAssetPath(V, Ignored))
			{
				OutAsset = &V;
				return true;
			}
			if (!Call->Callee)
			{
				return false;
			}
			OutAsset = Call->Callee.Get();
			OutArguments = &Call->Args;
			return true;
		}
		if (V.Is<FDstNameExpr>() || V.Is<FDstMemberExpr>())
		{
			if (IsBindingPath(V, Scope))
			{
				return false;
			}
			OutAsset = &V;
			return true;
		}
		if (const FDstLiteralExpr* Literal = V.As<FDstLiteralExpr>(); Literal && Literal->LiteralKind == EDstLiteralKind::String)
		{
			OutAsset = &V;
			return true;
		}
		return false;
	}

	EValueClass FDstLowerer::ClassifyValue(const FDstExpr& Value, const FBindScope& Scope, const FProperty* Property) const
	{
		const FDstExpr& V = StripParens(Value);

		// A reference to a tree is written as a call, which is no property function there. It goes the way of a value with parts
		// fed from elsewhere, whether or not it has any: that is where its tree and its parameters are checked.
		if (Property && IsDstStateTreeReferenceProperty(*Property))
		{
			const FDstExpr* Asset = nullptr;
			const TArray<FDstArgumentPtr>* Arguments = nullptr;
			if (TryGetReferenceParts(V, Scope, Asset, Arguments))
			{
				return EValueClass::Mixed;
			}
		}

		switch (V.Kind)
		{
		case EDstNodeKind::RefExpr:
			return EValueClass::Ref;
		case EDstNodeKind::NameExpr:
		case EDstNodeKind::MemberExpr:
			return IsBindingPath(V, Scope) ? EValueClass::Binding : EValueClass::Constant;
		case EDstNodeKind::UnaryExpr:
		case EDstNodeKind::BinaryExpr:
		{
			FFoldedConstant Folded;
			if (TryFold(V, Folded))
			{
				return EValueClass::Constant;
			}
			return EValueClass::Operation;
		}
		case EDstNodeKind::CallExpr:
		{
			const FDstCallExpr& Call = static_cast<const FDstCallExpr&>(V);
			const FDstNameExpr* Name = Call.Callee ? Call.Callee->As<FDstNameExpr>() : nullptr;
			return (Name && Name->Name.Equals(TEXT("Path"), ESearchCase::CaseSensitive)) ? EValueClass::Constant : EValueClass::Call;
		}
		case EDstNodeKind::TupleExpr:
		case EDstNodeKind::ListExpr:
			return ContainsDynamic(V, *this, Scope) ? EValueClass::Mixed : EValueClass::Constant;
		default:
			return EValueClass::Constant;
		}
	}

	// ------------------------------------------------------------------------------------------
	// The entry point
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerValue(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property, const FDstExpr& Value)
	{
		MarkGiven(Build, Side, Property);
		const FDstExpr& V = StripParens(Value);
		const EPropertyUse Use = Side == EDstPropertySide::Instance && !Build.bBagProperties ? GetPropertyUse(Property) : EPropertyUse::Parameter;

		if (Use == EPropertyUse::Output)
		{
			Sink.Error(TEXT("DST5005"), V.Span,
				FString::Printf(TEXT("%s is an Output: a node writes it, nothing writes into it."), *DescribeTarget(Build, Property)),
				FString::Printf(TEXT("To copy it somewhere, write %s -> Params.Something."), *Property.GetName()));
			return false;
		}

		const bool bRefProperty = Side == EDstPropertySide::Instance && !Build.bBagProperties && IsPropertyRefProperty(Property);
		const EValueClass Class = ClassifyValue(V, Build.Scope, &Property);
		if (Class == EValueClass::Ref)
		{
			if (!bRefProperty)
			{
				Sink.Error(TEXT("DST5015"), V.Span,
					FString::Printf(TEXT("`ref` is for property references; %s is not one."), *DescribeTarget(Build, Property)),
					TEXT("Write the path alone to copy its value."));
				return false;
			}
			return LowerRef(Build, Side, Property, static_cast<const FDstRefExpr&>(V));
		}
		if (bRefProperty)
		{
			Sink.Error(TEXT("DST5015"), V.Span,
				FString::Printf(TEXT("%s is a property reference: write `ref` and the path it points to."), *DescribeTarget(Build, Property)),
				FString::Printf(TEXT("%s = ref Params.Something"), *Property.GetName()));
			return false;
		}

		FDstPropertyPath Path;
		FDstPathSegment Segment;
		Segment.Name = Property.GetName();
		Path.Segments.Add(MoveTemp(Segment));

		switch (Class)
		{
		case EValueClass::Binding:
			return LowerBinding(Build, Side, Property, V);
		case EValueClass::Operation:
			return LowerOperation(Build, Side, Path, Property, V);
		case EValueClass::Call:
			return LowerFunctionCall(Build, Side, Path, Property, V);
		case EValueClass::Mixed:
			return LowerMixed(Build, Side, Property, V);
		default:
			return LowerConstant(Build, Side, Property, V);
		}
	}

	// ------------------------------------------------------------------------------------------
	// Bindings
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerBinding(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property, const FDstExpr& Value)
	{
		FResolvedSource Source;
		if (!ResolvePath(Value, Build.Scope, Source))
		{
			return false;
		}

		// A tag property written with a path that is also a tag: the text cannot tell which is meant.
		if (IsGameplayTagProperty(&Property))
		{
			FGameplayTag Tag;
			FString Error;
			if (ResolveDstTagName(Source.Display, ValueContext.TagPrefixes, Tag, Error) && Tag.IsValid())
			{
				Sink.Error(TEXT("DST5001"), Value.Span,
					FString::Printf(TEXT("`%s` is both a path and a tag."), *Source.Display),
					FString::Printf(TEXT("Write the tag as a string, \"%s\", to mean the tag."), *Source.Display));
				return false;
			}
		}

		if (!CheckBindable(Source, Property, Value.Span, DescribeTarget(Build, Property)))
		{
			return false;
		}

		FDstPropertyPath Path;
		FDstPathSegment Segment;
		Segment.Name = Property.GetName();
		Path.Segments.Add(MoveTemp(Segment));
		AddBindingArg(Build, Side, Path, EDstValueKind::Binding, Source.Ref);
		return true;
	}

	bool FDstLowerer::LowerRef(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property, const FDstRefExpr& Ref)
	{
		if (!Ref.Target)
		{
			return false;
		}
		FResolvedSource Source;
		if (!ResolvePath(*Ref.Target, Build.Scope, Source))
		{
			return false;
		}
		if (!Source.Leaf)
		{
			Sink.Error(TEXT("DST5015"), Ref.Span, TEXT("A reference points to a property, not to a whole context object."));
			return false;
		}

		// What the engine lets a reference point at: parameters and events, and the outputs of nodes that run before.
		switch (Source.Origin)
		{
		case EBindOrigin::Context:
			Sink.Error(TEXT("DST5015"), Ref.Span, TEXT("A reference cannot point into a context: the engine only lets it point at parameters, events and node outputs."));
			return false;
		case EBindOrigin::Global:
		case EBindOrigin::Task:
			if (!(Source.RootProperty && (GetPropertyUse(*Source.RootProperty) == EPropertyUse::Output || IsPropertyRefProperty(*Source.RootProperty))))
			{
				Sink.Error(TEXT("DST5015"), Ref.Span, FString::Printf(TEXT("`%s` is not an Output of its node, so it cannot be referenced."), *Source.Display));
				return false;
			}
			break;
		default:
			break;
		}

		const void* RefAddress = nullptr;
		if (Build.Info.IsValid() && Build.Info->GetInstanceDefaults() && Side == EDstPropertySide::Instance)
		{
			RefAddress = Property.ContainerPtrToValuePtr<void>(Build.Info->GetInstanceDefaults());
		}
		if (RefAddress && !UE::StateTree::PropertyRefHelpers::IsPropertyRefCompatibleWithProperty(Property, *Source.Leaf, RefAddress, nullptr))
		{
			Sink.Error(TEXT("DST5003"), Ref.Span,
				FString::Printf(TEXT("%s cannot reference `%s` (%s): the types do not match."), *DescribeTarget(Build, Property), *Source.Display, *DescribeProperty(Source.Leaf)));
			return false;
		}

		FDstPropertyPath Path;
		FDstPathSegment Segment;
		Segment.Name = Property.GetName();
		Path.Segments.Add(MoveTemp(Segment));
		AddBindingArg(Build, Side, Path, EDstValueKind::Ref, Source.Ref);
		return true;
	}

	bool FDstLowerer::LowerOutputArgument(FNodeBuild& Build, const FDstArgument& Argument)
	{
		EDstPropertySide Side = EDstPropertySide::Instance;
		const FProperty* Property = nullptr;
		if (!Build.Info.IsValid() || !FindArgumentProperty(*Build.Info, Argument.Name, Side, Property))
		{
			Sink.Error(TEXT("DST4007"), Argument.NameSpan, FString::Printf(TEXT("`%s` has no property `%s`."), *Build.DisplayName, *Argument.Name));
			return false;
		}
		if (WasGiven(Build, Side, *Property))
		{
			Sink.Error(TEXT("DST4017"), Argument.Span, FString::Printf(TEXT("`%s` is given twice."), *Property->GetName()));
			return false;
		}
		MarkGiven(Build, Side, *Property);

		if (Side != EDstPropertySide::Instance || GetPropertyUse(*Property) != EPropertyUse::Output)
		{
			Sink.Error(TEXT("DST5016"), Argument.Span,
				FString::Printf(TEXT("%s is not an Output, so `->` cannot copy it."), *DescribeTarget(Build, *Property)),
				TEXT("`Prop -> Params.X` copies one of the node's Output properties into a parameter."));
			return false;
		}
		if (IsDelegateDispatcherProperty(Property) || IsDelegateListenerProperty(Property) || IsPropertyRefProperty(*Property))
		{
			Sink.Error(TEXT("DST5016"), Argument.Span, TEXT("An output binding cannot copy a delegate or a property reference."));
			return false;
		}

		FResolvedSource Destination;
		if (!ResolvePath(*Argument.Value, Build.Scope, Destination))
		{
			return false;
		}
		if (!Destination.Leaf || (Destination.Origin != EBindOrigin::TreeParameter && Destination.Origin != EBindOrigin::StateParameter))
		{
			Sink.Error(TEXT("DST5016"), Argument.Value->Span,
				FString::Printf(TEXT("`%s` cannot receive an output: only the tree's or a subtree's parameters can."), *Destination.Display),
				TEXT("Write Prop -> Params.Name."));
			return false;
		}
		// The copy goes from the node's property to the parameter.
		if (UE::PropertyBinding::GetPropertyCompatibility(Property, Destination.Leaf) == UE::PropertyBinding::EPropertyCompatibility::Incompatible)
		{
			Sink.Error(TEXT("DST5003"), Argument.Span,
				FString::Printf(TEXT("Cannot copy %s (%s) into `%s` (%s): the types are not compatible."), *DescribeTarget(Build, *Property), *DescribeProperty(Property),
					*Destination.Display, *DescribeProperty(Destination.Leaf)));
			return false;
		}

		FDstPropertyPath Path;
		FDstPathSegment Segment;
		Segment.Name = Property->GetName();
		Path.Segments.Add(MoveTemp(Segment));
		AddBindingArg(Build, Side, Path, EDstValueKind::Output, Destination.Ref);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// A value that is partly fed from elsewhere
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerMixed(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property, const FDstExpr& Value)
	{
		FDstPropertyPath Path;
		FDstPathSegment Segment;
		Segment.Name = Property.GetName();
		Path.Segments.Add(MoveTemp(Segment));

		TUniquePtr<FDstExpr> Constant;
		if (!SplitValue(Build, Side, Path, Property, Value, DescribeTarget(Build, Property), Constant))
		{
			return false;
		}
		if (!Constant)
		{
			return true;
		}

		// The constant is always the whole property with the fed places at their defaults, even when it says nothing more
		// than the default does: what an asset holds there is then the same whichever way the property was fed.
		FString Text;
		if (!ConstantToText(*Constant, Property, Text, Value.Span))
		{
			return false;
		}
		AddConstantArg(Build, Side, Property, Text);
		return true;
	}

	bool FDstLowerer::SplitValue(FNodeBuild& Build, const EDstPropertySide Side, const FDstPropertyPath& Path, const FProperty& Property,
		const FDstExpr& Value, const FString& Display, TUniquePtr<FDstExpr>& OutConstant)
	{
		OutConstant.Reset();
		const FDstExpr& V = StripParens(Value);

		if (IsDstStateTreeReferenceProperty(Property))
		{
			const FDstExpr* Asset = nullptr;
			const TArray<FDstArgumentPtr>* Arguments = nullptr;
			if (TryGetReferenceParts(V, Build.Scope, Asset, Arguments))
			{
				return SplitReference(Build, Side, Path, V, Display, OutConstant);
			}
		}

		switch (ClassifyValue(V, Build.Scope, &Property))
		{
		case EValueClass::Constant:
			OutConstant = CloneExpr(V);
			return true;

		case EValueClass::Binding:
		{
			FResolvedSource Source;
			if (!ResolvePath(V, Build.Scope, Source) || !CheckBindable(Source, Property, V.Span, Display))
			{
				return false;
			}
			AddBindingArg(Build, Side, Path, EDstValueKind::Binding, Source.Ref);
			return true;
		}

		case EValueClass::Operation:
			return LowerOperation(Build, Side, Path, Property, V);

		case EValueClass::Call:
			return LowerFunctionCall(Build, Side, Path, Property, V);

		case EValueClass::Mixed:
			if (const FDstTupleExpr* Tuple = V.As<FDstTupleExpr>())
			{
				return SplitTuple(Build, Side, Path, Property, *Tuple, Display, OutConstant);
			}
			if (const FDstListExpr* List = V.As<FDstListExpr>())
			{
				return SplitList(Build, Side, Path, Property, *List, Display, OutConstant);
			}
			break;

		default:
			break;
		}
		Sink.Error(TEXT("DST4008"), V.Span, TEXT("A field of a tuple or an element of a list can be a value, a path, an operation or a property function call, nothing more."));
		return false;
	}

	bool FDstLowerer::SplitTuple(FNodeBuild& Build, const EDstPropertySide Side, const FDstPropertyPath& Path, const FProperty& Property,
		const FDstTupleExpr& Tuple, const FString& Display, TUniquePtr<FDstExpr>& OutConstant)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
		if (!StructProperty || !StructProperty->Struct)
		{
			Sink.Error(TEXT("DST4008"), Tuple.Span, FString::Printf(TEXT("%s is not a struct, so a tuple does not fit it."), *Display));
			return false;
		}

		const TArray<const FProperty*> PositionalFields = GetDstTupleFields(*StructProperty->Struct);
		TUniquePtr<FDstTupleExpr> ConstantPart = MakeUnique<FDstTupleExpr>();
		ConstantPart->Span = Tuple.Span;
		TArray<const FProperty*> Seen;
		int32 NextPositional = 0;
		bool bOk = true;

		for (const FDstArgumentPtr& Item : Tuple.Items)
		{
			if (!Item || !Item->Value)
			{
				continue;
			}
			const FProperty* Field = nullptr;
			if (Item->IsNamed())
			{
				Field = StructProperty->Struct->FindPropertyByName(FName(*Item->Name));
			}
			else if (PositionalFields.IsValidIndex(NextPositional))
			{
				Field = PositionalFields[NextPositional];
			}
			if (!Item->IsNamed())
			{
				++NextPositional;
			}
			if (!Field)
			{
				Sink.Error(TEXT("DST4008"), Item->Span, FString::Printf(TEXT("%s has no field %s."), *Display,
					Item->IsNamed() ? *FString::Printf(TEXT("`%s`"), *Item->Name) : *FString::Printf(TEXT("number %d"), NextPositional)));
				bOk = false;
				continue;
			}
			if (Seen.Contains(Field))
			{
				Sink.Error(TEXT("DST4017"), Item->Span, FString::Printf(TEXT("The field `%s` of %s is given twice."), *Field->GetName(), *Display));
				bOk = false;
				continue;
			}
			Seen.Add(Field);

			FDstPropertyPath FieldPath = Path;
			FDstPathSegment FieldSegment;
			FieldSegment.Name = Field->GetName();
			FieldPath.Segments.Add(MoveTemp(FieldSegment));

			TUniquePtr<FDstExpr> FieldConstant;
			if (!SplitValue(Build, Side, FieldPath, *Field, *Item->Value, FString::Printf(TEXT("`%s`"), *FieldPath.ToString()), FieldConstant))
			{
				bOk = false;
				continue;
			}
			if (FieldConstant)
			{
				FDstArgumentPtr Copy = MakeUnique<FDstArgument>();
				Copy->Name = Field->GetName();
				Copy->NameSpan = Item->NameSpan;
				Copy->Value = MoveTemp(FieldConstant);
				Copy->Span = Item->Span;
				ConstantPart->Items.Add(MoveTemp(Copy));
			}
		}
		OutConstant = MoveTemp(ConstantPart);
		return bOk;
	}

	bool FDstLowerer::SplitList(FNodeBuild& Build, const EDstPropertySide Side, const FDstPropertyPath& Path, const FProperty& Property,
		const FDstListExpr& List, const FString& Display, TUniquePtr<FDstExpr>& OutConstant)
	{
		const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property);
		if (!ArrayProperty)
		{
			if (CastField<FSetProperty>(&Property))
			{
				Sink.Error(TEXT("DST4024"), List.Span,
					FString::Printf(TEXT("%s cannot have something fed from elsewhere in its list: only the elements of an array can be fed one by one."), *Display),
					TEXT("A set's elements have no index for a binding path to pick. Write the whole set as constants, or bind the set as a whole."));
			}
			else
			{
				Sink.Error(TEXT("DST4008"), List.Span, FString::Printf(TEXT("%s is not an array or a set, so a list does not fit it."), *Display));
			}
			return false;
		}

		TUniquePtr<FDstListExpr> ConstantList = MakeUnique<FDstListExpr>();
		ConstantList->Span = List.Span;
		bool bOk = true;
		for (int32 Index = 0; Index < List.Items.Num(); ++Index)
		{
			const FDstExpr* Item = List.Items[Index].Get();
			if (!Item)
			{
				bOk = false;
				continue;
			}
			FDstPropertyPath ElementPath = Path;
			ElementPath.Segments.Last().ArrayIndex = Index;

			TUniquePtr<FDstExpr> ElementConstant;
			if (!SplitValue(Build, Side, ElementPath, *ArrayProperty->Inner, *Item, FString::Printf(TEXT("`%s`"), *ElementPath.ToString()), ElementConstant))
			{
				bOk = false;
				continue;
			}
			if (!ElementConstant)
			{
				// The element is fed as a whole: the constant keeps its place with the element's default.
				ElementConstant = MakeDstDefaultLiteral(*ArrayProperty->Inner, ValueContext);
				if (!ElementConstant)
				{
					Sink.Error(TEXT("DST4008"), Item->Span, FString::Printf(TEXT("The elements of %s cannot be fed one by one: they have no default value for the constant to hold meanwhile."), *Display));
					bOk = false;
					continue;
				}
			}
			ConstantList->Items.Add(MoveTemp(ElementConstant));
		}
		OutConstant = MoveTemp(ConstantList);
		return bOk;
	}

	bool FDstLowerer::SplitReference(FNodeBuild& Build, const EDstPropertySide Side, const FDstPropertyPath& Path, const FDstExpr& Value,
		const FString& Display, TUniquePtr<FDstExpr>& OutConstant)
	{
		const FDstExpr* AssetExpr = nullptr;
		const TArray<FDstArgumentPtr>* Arguments = nullptr;
		if (!TryGetReferenceParts(Value, Build.Scope, AssetExpr, Arguments))
		{
			Sink.Error(TEXT("DST4008"), Value.Span, FString::Printf(TEXT("Expected the name of a StateTree asset for %s."), *Display));
			return false;
		}
		if (!AssetExpr)
		{
			// `None`: no tree.
			OutConstant = CloneExpr(Value);
			return true;
		}

		// The tree.
		const FDstExpr& AssetName = StripParens(*AssetExpr);
		FString ObjectPath;
		FString Error;
		if (TryGetDstAssetPath(AssetName, ObjectPath))
		{
		}
		else if (const FDstNameExpr* Name = AssetName.As<FDstNameExpr>())
		{
			if (!FindDstAsset(Name->Name, UStateTree::StaticClass(), ObjectPath, Error))
			{
				Sink.Error(TEXT("DST4008"), AssetName.Span, Error, TEXT("A reference names a StateTree asset that has been built, or writes it as Path(Game, \"AI/ST_Other\")."));
				return false;
			}
		}
		else if (const FDstLiteralExpr* Literal = AssetName.As<FDstLiteralExpr>(); Literal && Literal->LiteralKind == EDstLiteralKind::String && Literal->Text.StartsWith(TEXT("/")))
		{
			ObjectPath = Literal->Text;
		}
		else
		{
			Sink.Error(TEXT("DST4008"), AssetName.Span, FString::Printf(TEXT("Expected the name of a StateTree asset or Path(Root, \"Rel\") for %s."), *Display));
			return false;
		}
		const FParamSet* Signature = LoadAssetSignature(ObjectPath, Error);
		if (!Signature)
		{
			Sink.Error(TEXT("DST4008"), AssetName.Span, Error, TEXT("Only a StateTree asset can be referenced."));
			return false;
		}

		if (!Arguments || Arguments->IsEmpty())
		{
			OutConstant = CloneExpr(Value);
			return true;
		}

		// The parameters it gives values to. A fed one stays in the constant at its default: it is overridden all the same.
		TUniquePtr<FDstCallExpr> Constant = MakeUnique<FDstCallExpr>();
		Constant->Span = Value.Span;
		if (const FDstCallExpr* Call = Value.As<FDstCallExpr>())
		{
			Constant->ArgsSpan = Call->ArgsSpan;
		}
		Constant->Callee = CloneExpr(*AssetExpr);

		const FString TreeName = FPackageName::ObjectPathToObjectName(ObjectPath);
		TArray<FString> Given;
		int32 NextPositional = 0;
		bool bOk = true;
		for (const FDstArgumentPtr& Argument : *Arguments)
		{
			if (!Argument || !Argument->Value)
			{
				continue;
			}
			if (Argument->bOutput)
			{
				Sink.Error(TEXT("DST4008"), Argument->Span, TEXT("A reference to a StateTree takes `Parameter = value`; `->` is for the outputs of a node."));
				bOk = false;
				continue;
			}

			FString Name;
			if (Argument->IsNamed())
			{
				Name = Argument->Name;
			}
			else if (NextPositional < Signature->Params.Num())
			{
				Name = Signature->Params[NextPositional++].Name;
			}
			else
			{
				Sink.Error(TEXT("DST4016"), Argument->Span, FString::Printf(TEXT("`%s` has only %d parameter(s)."), *TreeName, Signature->Params.Num()));
				bOk = false;
				continue;
			}
			const FProperty* Parameter = Signature->FindProperty(Name);
			if (!Parameter)
			{
				Sink.Error(TEXT("DST4023"), Argument->IsNamed() ? Argument->NameSpan : Argument->Span,
					FString::Printf(TEXT("The tree `%s` has no parameter `%s`."), *TreeName, *Name),
					Signature->Params.IsEmpty() ? FString(TEXT("The tree has no parameters.")) : MakeSuggestionHint(Name, Signature->GetNames()));
				bOk = false;
				continue;
			}
			if (Given.Contains(Name))
			{
				Sink.Error(TEXT("DST4017"), Argument->Span, FString::Printf(TEXT("`%s` is given twice."), *Name));
				bOk = false;
				continue;
			}
			Given.Add(Name);

			// `StateTree.Parameters.Value<PropertyBag>.Name`: the parameter inside the reference.
			FDstPropertyPath ParameterPath = Path;
			FDstPathSegment ParametersSegment;
			ParametersSegment.Name = TEXT("Parameters");
			ParameterPath.Segments.Add(MoveTemp(ParametersSegment));
			FDstPathSegment ValueSegment;
			ValueSegment.Name = TEXT("Value");
			ValueSegment.InstanceType = PropertyBagInstanceType;
			ParameterPath.Segments.Add(MoveTemp(ValueSegment));
			FDstPathSegment NameSegment;
			NameSegment.Name = Parameter->GetName();
			ParameterPath.Segments.Add(MoveTemp(NameSegment));

			TUniquePtr<FDstExpr> ParameterConstant;
			if (!SplitValue(Build, Side, ParameterPath, *Parameter, *Argument->Value, FString::Printf(TEXT("the parameter `%s` of `%s`"), *Name, *TreeName), ParameterConstant))
			{
				bOk = false;
				continue;
			}
			if (!ParameterConstant)
			{
				ParameterConstant = MakeDstDefaultLiteral(*Parameter, ValueContext);
				if (!ParameterConstant)
				{
					Sink.Error(TEXT("DST4008"), Argument->Span, FString::Printf(TEXT("The parameter `%s` of `%s` cannot be fed from elsewhere: it has no default value for the constant to hold meanwhile."), *Name, *TreeName));
					bOk = false;
					continue;
				}
			}
			FDstArgumentPtr Copy = MakeUnique<FDstArgument>();
			Copy->Name = Name;
			Copy->NameSpan = Argument->NameSpan;
			Copy->Value = MoveTemp(ParameterConstant);
			Copy->Span = Argument->Span;
			Constant->Args.Add(MoveTemp(Copy));
		}
		OutConstant = MoveTemp(Constant);
		return bOk;
	}

	// ------------------------------------------------------------------------------------------
	// Operations: property functions
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerOperation(FNodeBuild& Build, const EDstPropertySide Side, const FDstPropertyPath& TargetPath, const FProperty& Leaf, const FDstExpr& Value)
	{
		TSharedPtr<FDstNode> Function;
		if (!BuildOperationNode(Value, Leaf, Build, TargetPath.ToString(), Function))
		{
			return false;
		}
		FDstArg Arg;
		Arg.Side = Side;
		Arg.Property = TargetPath;
		Arg.Kind = EDstValueKind::Function;
		Arg.Function = Function;
		Build.Node->Args.Add(MoveTemp(Arg));
		return true;
	}

	bool FDstLowerer::BuildOperationNode(const FDstExpr& Expression, const FProperty& Target, const FNodeBuild& Owner, const FString& PropertyName,
		TSharedPtr<FDstNode>& OutFunction)
	{
		const FDstExpr& E = StripParens(Expression);
		const EValueDomain Domain = GetValueDomain(&Target);
		if (Domain != EValueDomain::Bool && Domain != EValueDomain::Int && Domain != EValueDomain::Float)
		{
			Sink.Error(TEXT("DST5011"), E.Span,
				FString::Printf(TEXT("There is no property function for %s: arithmetic and logic produce a number or a bool."), *DescribeTarget(Owner, Target)),
				TEXT("The property is a float, an int or a bool for `+ - * /`, `and`, `or` and `not` to work."));
			return false;
		}

		const TCHAR* StructPath = nullptr;
		const TCHAR* FirstName = nullptr;
		const TCHAR* SecondName = nullptr;
		const FDstExpr* FirstOperand = nullptr;
		const FDstExpr* SecondOperand = nullptr;

		if (const FDstUnaryExpr* Unary = E.As<FDstUnaryExpr>())
		{
			const TCHAR* Word = Unary->Op == EDstUnaryOp::Negate ? TEXT("-") : TEXT("not");
			for (const Sugar::FUnaryFunction& Function : Sugar::UnaryFunctions)
			{
				const FString Path = Function.StructPath;
				const bool bFloat = Path.Contains(TEXT("Float"));
				const bool bInt = Path.Contains(TEXT("Int"));
				const bool bBool = Path.Contains(TEXT("Boolean"));
				const bool bDomainMatches = (Domain == EValueDomain::Float && bFloat) || (Domain == EValueDomain::Int && bInt) || (Domain == EValueDomain::Bool && bBool);
				if (FCString::Strcmp(Function.Spelling, Word) == 0 && bDomainMatches)
				{
					StructPath = Function.StructPath;
					FirstName = Function.Input;
					break;
				}
			}
			FirstOperand = Unary->Operand.Get();
		}
		else if (const FDstBinaryExpr* Binary = E.As<FDstBinaryExpr>())
		{
			if (IsComparison(Binary->Op))
			{
				Sink.Error(TEXT("DST5011"), E.Span, TEXT("A comparison is a condition, not a value: it has no property function."),
					TEXT("Use it after `if`."));
				return false;
			}
			const TCHAR* Word = GetDstBinaryOpSpelling(Binary->Op);
			const Sugar::FBinaryFunction* Table = nullptr;
			int32 Count = 0;
			if (Domain == EValueDomain::Float) { Table = Sugar::FloatFunctions; Count = UE_ARRAY_COUNT(Sugar::FloatFunctions); }
			else if (Domain == EValueDomain::Int) { Table = Sugar::IntFunctions; Count = UE_ARRAY_COUNT(Sugar::IntFunctions); }
			else { Table = Sugar::BooleanFunctions; Count = UE_ARRAY_COUNT(Sugar::BooleanFunctions); }
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (FCString::Strcmp(Table[Index].Spelling, Word) == 0)
				{
					StructPath = Table[Index].StructPath;
					FirstName = Table[Index].Left;
					SecondName = Table[Index].Right;
					break;
				}
			}
			FirstOperand = Binary->Lhs.Get();
			SecondOperand = Binary->Rhs.Get();
		}

		if (!StructPath)
		{
			Sink.Error(TEXT("DST5011"), E.Span,
				FString::Printf(TEXT("There is no property function for this operation on %s."), *DescribeProperty(&Target)),
				Domain == EValueDomain::Bool ? FString(TEXT("A bool takes `and`, `or` and `not`.")) : FString(TEXT("A number takes `+`, `-`, `*`, `/` and a leading `-`.")));
			return false;
		}

		const FString Key = Owner.Node->Key + TEXT("/Fn:") + PropertyName;
		TSharedRef<FDstNode> Function = MakeShared<FDstNode>();
		FNodeInfoPtr Info;
		const FDstNodeType* Type = nullptr;
		if (!MakeSugarNode(StructPath, Key, EDstNodeRole::PropertyFunction, E.Span, *Function, Info, Type))
		{
			return false;
		}

		FNodeBuild Build;
		Build.Node = &*Function;
		Build.Info = Info;
		Build.Scope = Owner.Scope;
		Build.CallSpan = E.Span;
		Build.DisplayName = Type->Name;

		bool bOk = true;
		const FProperty* First = Info->InstanceStruct ? Info->InstanceStruct->FindPropertyByName(FirstName) : nullptr;
		if (!First || !FirstOperand)
		{
			return false;
		}
		bOk &= LowerValue(Build, EDstPropertySide::Instance, *First, *FirstOperand);
		if (SecondName)
		{
			const FProperty* Second = Info->InstanceStruct->FindPropertyByName(SecondName);
			if (!Second || !SecondOperand)
			{
				return false;
			}
			bOk &= LowerValue(Build, EDstPropertySide::Instance, *Second, *SecondOperand);
		}
		SortArgs(*Function);
		OutFunction = Function;
		return bOk;
	}

	bool FDstLowerer::LowerFunctionCall(FNodeBuild& Build, const EDstPropertySide Side, const FDstPropertyPath& TargetPath, const FProperty& Leaf, const FDstExpr& Value)
	{
		const FString Key = Build.Node->Key + TEXT("/Fn:") + TargetPath.ToString();
		TSharedRef<FDstNode> Function = MakeShared<FDstNode>();
		FNodeInfoPtr Info;
		if (!LowerNode(Value, FString(), Value.Span, Key, EDstNodeRole::PropertyFunction, EDstNodeCategory::PropertyFunction, Build.Scope, *Function, &Info) || !Info.IsValid())
		{
			return false;
		}

		const FProperty* Output = FindFunctionOutput(*Info);
		if (!Output)
		{
			Sink.Error(TEXT("DST5011"), Value.Span,
				FString::Printf(TEXT("Cannot tell which output of `%s` is its result."), *Info->Type->Name),
				TEXT("A property function has one Output property, or one called Result."));
			return false;
		}
		if (UE::PropertyBinding::GetPropertyCompatibility(Output, &Leaf) == UE::PropertyBinding::EPropertyCompatibility::Incompatible)
		{
			Sink.Error(TEXT("DST5003"), Value.Span,
				FString::Printf(TEXT("`%s` gives %s, which does not fit %s (%s)."), *Info->Type->Name, *DescribeProperty(Output), *DescribeTarget(Build, Leaf), *DescribeProperty(&Leaf)));
			return false;
		}

		FDstArg Arg;
		Arg.Side = Side;
		Arg.Property = TargetPath;
		Arg.Kind = EDstValueKind::Function;
		Arg.Function = Function;
		Build.Node->Args.Add(MoveTemp(Arg));
		return true;
	}
}
