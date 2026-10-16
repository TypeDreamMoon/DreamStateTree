// Copyright DreamMoon. All Rights Reserved.
//
// What the language front end's tests share: where the corpus lives, how texts and diagnostics are compared, how a
// golden is checked. Everything is inline in one namespace. The test module is built as a unity, so a helper of the
// same name in the anonymous namespace of two .cpp files would collide.

#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstSource.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamStateTree::Tests::LangSupport
{
	/** <plugin>/Tests/Corpus, or empty when the plugin cannot be found. */
	inline FString GetCorpusDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamStateTree"));
		return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Corpus")) : FString();
	}

	inline FString NormalizeNewlines(const FString& Text)
	{
		return Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
	}

	/** The first differing line of two texts, for a readable failure. Empty when they are equal (newline style aside). */
	inline FString DescribeDifference(const FString& Expected, const FString& Actual)
	{
		TArray<FString> ExpectedLines;
		TArray<FString> ActualLines;
		NormalizeNewlines(Expected).ParseIntoArray(ExpectedLines, TEXT("\n"), false);
		NormalizeNewlines(Actual).ParseIntoArray(ActualLines, TEXT("\n"), false);
		const int32 Count = FMath::Max(ExpectedLines.Num(), ActualLines.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FString E = ExpectedLines.IsValidIndex(Index) ? ExpectedLines[Index] : TEXT("<end>");
			const FString A = ActualLines.IsValidIndex(Index) ? ActualLines[Index] : TEXT("<end>");
			if (!E.Equals(A, ESearchCase::CaseSensitive))
			{
				return FString::Printf(TEXT("line %d: expected `%s`, got `%s`"), Index + 1, *E, *A);
			}
		}
		return FString();
	}

	inline bool LoadText(const FString& Path, FString& OutText)
	{
		return FFileHelper::LoadFileToString(OutText, *Path);
	}

	inline void WriteActual(const FString& GoldenPath, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *(GoldenPath + TEXT(".actual")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/**
	 * Compares Actual with the golden at GoldenPath, writing `<golden>.actual` next to it on a miss and failing the
	 * test with the path in the message. FallbackExpected, when given, stands in for a golden that does not exist
	 * (a canonical input is its own golden). True when equal.
	 */
	inline bool CheckGolden(FAutomationTestBase& Test, const FString& GoldenPath, const FString& Actual, const FString* FallbackExpected = nullptr)
	{
		FString Golden;
		if (!LoadText(GoldenPath, Golden))
		{
			if (!FallbackExpected)
			{
				WriteActual(GoldenPath, Actual);
				Test.AddError(FString::Printf(TEXT("No golden %s; wrote %s.actual for review."),
					*FPaths::GetCleanFilename(GoldenPath), *GoldenPath));
				return false;
			}
			Golden = *FallbackExpected;
		}
		const FString Difference = DescribeDifference(Golden, Actual);
		if (!Difference.IsEmpty())
		{
			WriteActual(GoldenPath, Actual);
			Test.AddError(FString::Printf(TEXT("%s differs (%s); wrote %s.actual."),
				*FPaths::GetCleanFilename(GoldenPath), *Difference, *GoldenPath));
			return false;
		}
		IFileManager::Get().Delete(*(GoldenPath + TEXT(".actual")), false, false, true);
		return true;
	}

	/** One `// EXPECT DSTnnnn` or `// EXPECT DSTnnnn (line,col)` comment of a corpus file. */
	struct FExpectation
	{
		FString Code;
		int32 Line = 0;
		int32 Column = 0;
		bool bHasPosition = false;
	};

	/** The expectations of a text, one per comment line, anywhere in the file. */
	inline TArray<FExpectation> ReadExpectations(const FString& Text)
	{
		TArray<FExpectation> Result;
		TArray<FString> Lines;
		NormalizeNewlines(Text).ParseIntoArray(Lines, TEXT("\n"), false);
		for (const FString& RawLine : Lines)
		{
			const int32 At = RawLine.Find(TEXT("// EXPECT "), ESearchCase::CaseSensitive);
			if (At == INDEX_NONE)
			{
				continue;
			}
			FString Rest = RawLine.Mid(At + 10).TrimStartAndEnd();
			FExpectation Expectation;
			int32 Space = INDEX_NONE;
			if (Rest.FindChar(TEXT(' '), Space))
			{
				Expectation.Code = Rest.Left(Space);
				FString Position = Rest.Mid(Space + 1).TrimStartAndEnd();
				FString LineText;
				FString ColumnText;
				if (Position.StartsWith(TEXT("(")) && Position.EndsWith(TEXT(")"))
					&& Position.Mid(1, Position.Len() - 2).Split(TEXT(","), &LineText, &ColumnText))
				{
					Expectation.Line = FCString::Atoi(*LineText.TrimStartAndEnd());
					Expectation.Column = FCString::Atoi(*ColumnText.TrimStartAndEnd());
					Expectation.bHasPosition = true;
				}
			}
			else
			{
				Expectation.Code = Rest;
			}
			Result.Add(Expectation);
		}
		return Result;
	}

	/**
	 * Every expectation must match one diagnostic and every diagnostic one expectation, so a file without
	 * expectations must be clean (warnings included). A diagnostic about another file (an #included library) matches on
	 * its code alone, because the position in that file is not the one the expectation was written against.
	 */
	inline void CheckDiagnostics(FAutomationTestBase& Test, const FString& MainPath, const TArray<FExpectation>& Expected,
		TConstArrayView<UE::DreamStateTree::Lang::FDstDiagnostic> Diagnostics)
	{
		using UE::DreamStateTree::Lang::FDstDiagnostic;
		using UE::DreamStateTree::Lang::FDstDiagnosticSink;

		TArray<FDstDiagnostic> Unmatched(Diagnostics);
		for (const FExpectation& Expectation : Expected)
		{
			const int32 Index = Unmatched.IndexOfByPredicate([&Expectation, &MainPath](const FDstDiagnostic& Diagnostic)
			{
				if (!Diagnostic.Code.Equals(Expectation.Code, ESearchCase::CaseSensitive))
				{
					return false;
				}
				const bool bOtherFile = !Diagnostic.FilePath.IsEmpty() && !FPaths::IsSamePath(Diagnostic.FilePath, MainPath);
				return !Expectation.bHasPosition || bOtherFile
					|| (Diagnostic.Span.Line == Expectation.Line && Diagnostic.Span.Column == Expectation.Column);
			});
			if (Index == INDEX_NONE)
			{
				Test.AddError(Expectation.bHasPosition
					? FString::Printf(TEXT("Expected %s at (%d,%d), not raised."), *Expectation.Code, Expectation.Line, Expectation.Column)
					: FString::Printf(TEXT("Expected %s, not raised."), *Expectation.Code));
			}
			else
			{
				Unmatched.RemoveAt(Index);
			}
		}
		for (const FDstDiagnostic& Diagnostic : Unmatched)
		{
			Test.AddError(FString::Printf(TEXT("Unexpected %s"), *FDstDiagnosticSink::ToWireString(Diagnostic)));
		}
	}

	/** Every diagnostic of a sink as one wire string per line, for a failure message. */
	inline FString DescribeDiagnostics(const UE::DreamStateTree::Lang::FDstDiagnosticSink& Sink)
	{
		TArray<FString> Lines;
		for (const UE::DreamStateTree::Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			Lines.Add(UE::DreamStateTree::Lang::FDstDiagnosticSink::ToWireString(Diagnostic));
		}
		return Lines.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Lines, TEXT("; "));
	}

	// ------------------------------------------------------------------------------------------
	// A shape string for expressions: fully parenthesised, independent of the printer, so a test can say what the
	// parser or the template expander built without going through the code under test twice.
	//
	//   not a and b or c        (((not a) and b) or c)
	//   F(1, X = 2) [ Name = "n" ]    F(1,X=2)<Name="n">
	//   (0, 3000)               tuple(0,3000)
	//   (a)                     paren(a)
	//   [1, 2]                  list(1,2)
	//   `Move To`.x             `Move To`.x
	// ------------------------------------------------------------------------------------------

	inline FString ShapeOf(const UE::DreamStateTree::Lang::FDstExpr& Expr);

	inline FString ShapeOfArgument(const UE::DreamStateTree::Lang::FDstArgument& Argument)
	{
		const FString Value = Argument.Value ? ShapeOf(*Argument.Value) : FString(TEXT("<null>"));
		if (!Argument.IsNamed())
		{
			return Value;
		}
		return FString::Printf(TEXT("%s%s%s"), *Argument.Name, Argument.bOutput ? TEXT("->") : TEXT("="), *Value);
	}

	inline FString ShapeOfMeta(const UE::DreamStateTree::Lang::FDstMeta* Meta)
	{
		if (!Meta)
		{
			return FString();
		}
		TArray<FString> Parts;
		for (const TUniquePtr<UE::DreamStateTree::Lang::FDstMetaEntry>& Entry : Meta->Entries)
		{
			Parts.Add(Entry->Value ? Entry->Key + TEXT("=") + ShapeOf(*Entry->Value) : Entry->Key);
		}
		return FString::Printf(TEXT("<%s>"), *FString::Join(Parts, TEXT(";")));
	}

	inline FString ShapeOf(const UE::DreamStateTree::Lang::FDstExpr& Expr)
	{
		using namespace UE::DreamStateTree::Lang;

		switch (Expr.Kind)
		{
		case EDstNodeKind::LiteralExpr:
		{
			const FDstLiteralExpr& Literal = static_cast<const FDstLiteralExpr&>(Expr);
			return Literal.LiteralKind == EDstLiteralKind::String ? FString::Printf(TEXT("\"%s\""), *Literal.Text) : Literal.Text;
		}
		case EDstNodeKind::NameExpr:
		{
			const FDstNameExpr& Name = static_cast<const FDstNameExpr&>(Expr);
			return Name.bQuoted ? FString::Printf(TEXT("`%s`"), *Name.Name) : Name.Name;
		}
		case EDstNodeKind::MemberExpr:
		{
			const FDstMemberExpr& Member = static_cast<const FDstMemberExpr&>(Expr);
			return ShapeOf(*Member.Object) + TEXT(".") + (Member.bMemberQuoted ? FString::Printf(TEXT("`%s`"), *Member.Member) : Member.Member);
		}
		case EDstNodeKind::CallExpr:
		{
			const FDstCallExpr& Call = static_cast<const FDstCallExpr&>(Expr);
			TArray<FString> Args;
			for (const FDstArgumentPtr& Argument : Call.Args)
			{
				Args.Add(ShapeOfArgument(*Argument));
			}
			return ShapeOf(*Call.Callee) + TEXT("(") + FString::Join(Args, TEXT(",")) + TEXT(")") + ShapeOfMeta(Call.Meta.Get());
		}
		case EDstNodeKind::ListExpr:
		{
			TArray<FString> Items;
			for (const FDstExprPtr& Item : static_cast<const FDstListExpr&>(Expr).Items)
			{
				Items.Add(ShapeOf(*Item));
			}
			return TEXT("list(") + FString::Join(Items, TEXT(",")) + TEXT(")");
		}
		case EDstNodeKind::TupleExpr:
		{
			TArray<FString> Items;
			for (const FDstArgumentPtr& Item : static_cast<const FDstTupleExpr&>(Expr).Items)
			{
				Items.Add(ShapeOfArgument(*Item));
			}
			return TEXT("tuple(") + FString::Join(Items, TEXT(",")) + TEXT(")");
		}
		case EDstNodeKind::UnaryExpr:
		{
			const FDstUnaryExpr& Unary = static_cast<const FDstUnaryExpr&>(Expr);
			const FString Spelling = GetDstUnaryOpSpelling(Unary.Op);
			// `not` is a word and needs a space; `-` sticks to its operand.
			return TEXT("(") + Spelling + (Unary.Op == EDstUnaryOp::Not ? TEXT(" ") : TEXT("")) + ShapeOf(*Unary.Operand) + TEXT(")");
		}
		case EDstNodeKind::BinaryExpr:
		{
			const FDstBinaryExpr& Binary = static_cast<const FDstBinaryExpr&>(Expr);
			return TEXT("(") + ShapeOf(*Binary.Lhs) + TEXT(" ") + GetDstBinaryOpSpelling(Binary.Op) + TEXT(" ") + ShapeOf(*Binary.Rhs) + TEXT(")");
		}
		case EDstNodeKind::ParenExpr:
			return TEXT("paren(") + ShapeOf(*static_cast<const FDstParenExpr&>(Expr).Inner) + TEXT(")");
		case EDstNodeKind::RefExpr:
			return TEXT("ref ") + ShapeOf(*static_cast<const FDstRefExpr&>(Expr).Target);
		default:
			return FString::Printf(TEXT("<%s>"), LexToString(Expr.Kind));
		}
	}

	/** The first diagnostic with this code, or null. */
	inline const UE::DreamStateTree::Lang::FDstDiagnostic* FindDiagnostic(const UE::DreamStateTree::Lang::FDstDiagnosticSink& Sink, const TCHAR* Code)
	{
		for (const UE::DreamStateTree::Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			if (Diagnostic.Code.Equals(Code, ESearchCase::CaseSensitive))
			{
				return &Diagnostic;
			}
		}
		return nullptr;
	}

	/** How many diagnostics carry this code. */
	inline int32 CountDiagnostics(const UE::DreamStateTree::Lang::FDstDiagnosticSink& Sink, const TCHAR* Code)
	{
		int32 Count = 0;
		for (const UE::DreamStateTree::Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			if (Diagnostic.Code.Equals(Code, ESearchCase::CaseSensitive))
			{
				++Count;
			}
		}
		return Count;
	}
}
