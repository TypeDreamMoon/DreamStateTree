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

#include "DstLowerPrivate.h"

#include "StateTreePropertyBindings.h"
#include "StateTreePropertyRefHelpers.h"
#include "PropertyBindingTypes.h"
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

	EValueClass FDstLowerer::ClassifyValue(const FDstExpr& Value, const FBindScope& Scope) const
	{
		const FDstExpr& V = StripParens(Value);
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
			return ContainsDynamic(V, *this, Scope) ? EValueClass::MixedTuple : EValueClass::Constant;
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
		const EValueClass Class = ClassifyValue(V, Build.Scope);
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
		case EValueClass::MixedTuple:
			return LowerMixedTuple(Build, Side, Property, static_cast<const FDstTupleExpr&>(V));
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
	// A tuple with something dynamic in it
	// ------------------------------------------------------------------------------------------

	bool FDstLowerer::LowerMixedTuple(FNodeBuild& Build, const EDstPropertySide Side, const FProperty& Property, const FDstTupleExpr& Tuple)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
		if (!StructProperty || !StructProperty->Struct)
		{
			Sink.Error(TEXT("DST4008"), Tuple.Span, FString::Printf(TEXT("%s is not a struct, so a tuple does not fit it."), *DescribeTarget(Build, Property)));
			return false;
		}

		TArray<const FProperty*> Fields;
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Deprecated))
			{
				Fields.Add(*It);
			}
		}

		TUniquePtr<FDstTupleExpr> ConstantPart = MakeUnique<FDstTupleExpr>();
		ConstantPart->Span = Tuple.Span;
		bool bOk = true;

		for (int32 Index = 0; Index < Tuple.Items.Num(); ++Index)
		{
			const FDstArgument* Item = Tuple.Items[Index].Get();
			if (!Item || !Item->Value)
			{
				continue;
			}
			const FProperty* Field = nullptr;
			if (Item->IsNamed())
			{
				Field = StructProperty->Struct->FindPropertyByName(FName(*Item->Name));
			}
			else if (Fields.IsValidIndex(Index))
			{
				Field = Fields[Index];
			}
			if (!Field)
			{
				Sink.Error(TEXT("DST4008"), Item->Span, FString::Printf(TEXT("%s has no field %s."), *DescribeTarget(Build, Property),
					Item->IsNamed() ? *FString::Printf(TEXT("`%s`"), *Item->Name) : *FString::Printf(TEXT("number %d"), Index + 1)));
				bOk = false;
				continue;
			}

			const EValueClass Class = ClassifyValue(*Item->Value, Build.Scope);
			if (Class == EValueClass::Constant)
			{
				FDstArgumentPtr Copy = MakeUnique<FDstArgument>();
				Copy->Name = Field->GetName();
				Copy->NameSpan = Item->NameSpan;
				Copy->Value = CloneExpr(*Item->Value);
				Copy->Span = Item->Span;
				ConstantPart->Items.Add(MoveTemp(Copy));
				continue;
			}

			FDstPropertyPath Path;
			FDstPathSegment Outer;
			Outer.Name = Property.GetName();
			Path.Segments.Add(MoveTemp(Outer));
			FDstPathSegment Inner;
			Inner.Name = Field->GetName();
			Path.Segments.Add(MoveTemp(Inner));

			const FDstExpr& ItemValue = StripParens(*Item->Value);
			if (Class == EValueClass::Binding)
			{
				FResolvedSource Source;
				if (!ResolvePath(ItemValue, Build.Scope, Source) || !CheckBindable(Source, *Field, ItemValue.Span, FString::Printf(TEXT("`%s.%s`"), *Property.GetName(), *Field->GetName())))
				{
					bOk = false;
					continue;
				}
				AddBindingArg(Build, Side, Path, EDstValueKind::Binding, Source.Ref);
			}
			else if (Class == EValueClass::Operation)
			{
				bOk &= LowerOperation(Build, Side, Path, *Field, ItemValue);
			}
			else if (Class == EValueClass::Call)
			{
				bOk &= LowerFunctionCall(Build, Side, Path, *Field, ItemValue);
			}
			else
			{
				Sink.Error(TEXT("DST4008"), ItemValue.Span, TEXT("A field of a tuple can be a value, a path, an operation or a property function call, nothing more."));
				bOk = false;
			}
		}

		if (!ConstantPart->Items.IsEmpty())
		{
			FString Text;
			FString Error;
			if (DstLiteralToExportText(*ConstantPart, Property, ValueContext, Text, Error))
			{
				AddConstantArg(Build, Side, Property, Text);
			}
			else
			{
				Sink.Error(TEXT("DST4008"), Tuple.Span, Error);
				bOk = false;
			}
		}
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
