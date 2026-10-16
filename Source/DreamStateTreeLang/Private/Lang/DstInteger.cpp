// Copyright DreamMoon. All Rights Reserved.
//
// Integer constants read exactly. A number literal also carries a double, which holds every integer only
// up to 2^53; int64 values beyond that are read from the literal's digits instead.

#include "Lang/DstAst.h"

namespace UE::DreamStateTree::Lang::IntegerPrivate
{
	/** Expr without its parentheses and unary minus signs; bOutNegative tells whether the signs flip it. */
	const FDstExpr* StripSigns(const FDstExpr& Expr, bool& bOutNegative)
	{
		const FDstExpr* Current = &Expr;
		bOutNegative = false;
		for (;;)
		{
			if (const FDstParenExpr* Paren = Current->As<FDstParenExpr>(); Paren && Paren->Inner)
			{
				Current = Paren->Inner.Get();
			}
			else if (const FDstUnaryExpr* Unary = Current->As<FDstUnaryExpr>(); Unary && Unary->Op == EDstUnaryOp::Negate && Unary->Operand)
			{
				bOutNegative = !bOutNegative;
				Current = Unary->Operand.Get();
			}
			else
			{
				return Current;
			}
		}
	}

	/**
	 * The digits of a decimal literal (`123`, `1.50`, `9.007e15`) scaled to a whole number, without leading
	 * zeros; false when the literal is not a whole number or not decimal. The sign is folded into bNegative.
	 */
	bool ReadWholeDigits(const FString& Text, bool& bNegative, FString& OutDigits)
	{
		int32 Cursor = 0;
		if (Text.IsEmpty())
		{
			return false;
		}
		if (Text[Cursor] == TEXT('-') || Text[Cursor] == TEXT('+'))
		{
			bNegative ^= Text[Cursor++] == TEXT('-');
		}

		FString Digits;
		int32 FractionDigits = 0;
		bool bPoint = false;
		for (; Cursor < Text.Len(); ++Cursor)
		{
			const TCHAR C = Text[Cursor];
			if (C >= TEXT('0') && C <= TEXT('9'))
			{
				Digits.AppendChar(C);
				FractionDigits += bPoint ? 1 : 0;
			}
			else if (C == TEXT('.') && !bPoint)
			{
				bPoint = true;
			}
			else
			{
				break;
			}
		}
		if (Digits.IsEmpty())
		{
			return false;
		}

		int64 Exponent = 0;
		if (Cursor < Text.Len() && (Text[Cursor] == TEXT('e') || Text[Cursor] == TEXT('E')))
		{
			++Cursor;
			bool bNegativeExponent = false;
			if (Cursor < Text.Len() && (Text[Cursor] == TEXT('+') || Text[Cursor] == TEXT('-')))
			{
				bNegativeExponent = Text[Cursor++] == TEXT('-');
			}
			const int32 Start = Cursor;
			for (; Cursor < Text.Len() && Text[Cursor] >= TEXT('0') && Text[Cursor] <= TEXT('9'); ++Cursor)
			{
				// Anything past the int64 range fails below; capping keeps the arithmetic from overflowing.
				Exponent = FMath::Min<int64>(Exponent * 10 + Text[Cursor] - TEXT('0'), int64(MAX_int32) + 20);
			}
			if (Cursor == Start)
			{
				return false;
			}
			Exponent = bNegativeExponent ? -Exponent : Exponent;
		}
		if (Cursor != Text.Len())
		{
			return false;
		}

		int32 First = 0;
		while (First < Digits.Len() && Digits[First] == TEXT('0'))
		{
			++First;
		}
		Digits.RightChopInline(First);
		const int64 Scale = Exponent - FractionDigits;
		if (Digits.IsEmpty())
		{
			OutDigits.Reset();
			return true;
		}
		if (Scale < 0)
		{
			// The digits shifted out must all be zero: `1.50` is 15 tenths, a whole number only if they are.
			if (-Scale >= Digits.Len())
			{
				return false;
			}
			for (int32 Index = Digits.Len() + static_cast<int32>(Scale); Index < Digits.Len(); ++Index)
			{
				if (Digits[Index] != TEXT('0'))
				{
					return false;
				}
			}
			Digits.LeftChopInline(static_cast<int32>(-Scale));
		}
		else
		{
			// More than 19 digits never fits an int64.
			if (Digits.Len() + Scale > 19)
			{
				return false;
			}
			Digits.AppendChars(TEXT("0000000000000000000"), static_cast<int32>(Scale));
		}
		OutDigits = MoveTemp(Digits);
		return true;
	}
}

namespace UE::DreamStateTree::Lang
{
	bool TryGetDstInteger(const FDstExpr& Expr, int64& OutValue)
	{
		bool bNegative = false;
		const FDstLiteralExpr* Literal = IntegerPrivate::StripSigns(Expr, bNegative)->As<FDstLiteralExpr>();
		if (!Literal || Literal->LiteralKind != EDstLiteralKind::Number)
		{
			return false;
		}
		if (Literal->Text.IsEmpty())
		{
			// Made by code rather than read from text: the double is all there is.
			const double Value = bNegative ? -Literal->Number : Literal->Number;
			if (!FMath::IsFinite(Value) || Value < -9223372036854775808.0 || Value >= 9223372036854775808.0 || FMath::Frac(Value) != 0.0)
			{
				return false;
			}
			OutValue = static_cast<int64>(Value);
			return true;
		}

		FString Digits;
		if (!IntegerPrivate::ReadWholeDigits(Literal->Text, bNegative, Digits))
		{
			return false;
		}
		const uint64 Limit = bNegative ? uint64(MAX_int64) + 1 : uint64(MAX_int64);
		uint64 Magnitude = 0;
		for (const TCHAR C : Digits)
		{
			const uint64 Digit = C - TEXT('0');
			if (Magnitude > (Limit - Digit) / 10)
			{
				return false;
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		if (!bNegative)
		{
			OutValue = static_cast<int64>(Magnitude);
		}
		else
		{
			OutValue = Magnitude == uint64(MAX_int64) + 1 ? MIN_int64 : -static_cast<int64>(Magnitude);
		}
		return true;
	}

	FDstExprPtr MakeDstInteger(int64 Value)
	{
		// MIN_int64 has no positive counterpart, so the magnitude is taken without negating Value itself.
		const uint64 Magnitude = Value < 0 ? uint64(-(Value + 1)) + 1 : uint64(Value);
		TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
		Literal->Text = FString::Printf(TEXT("%llu"), Magnitude);
		Literal->Number = static_cast<double>(Magnitude);
		if (Value >= 0)
		{
			return Literal;
		}
		TUniquePtr<FDstUnaryExpr> Negative = MakeUnique<FDstUnaryExpr>();
		Negative->Op = EDstUnaryOp::Negate;
		Negative->Operand = MoveTemp(Literal);
		return Negative;
	}
}
