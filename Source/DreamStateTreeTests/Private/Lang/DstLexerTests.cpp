// Copyright DreamMoon. All Rights Reserved.
//
// Lexer tests: positions, names in backticks, numbers, strings, comments, directives, operators, Unicode names and
// line endings. What the parser makes of the tokens is covered by the corpus and the unit tests next to this file.

#include "DstLangTestSupport.h"

#include "Lang/DstDiagnostic.h"
#include "Lang/DstSource.h"
#include "Lang/DstToken.h"
#include "Misc/AutomationTest.h"

namespace UE::DreamStateTree::Tests::LexerTests
{
	using namespace UE::DreamStateTree::Lang;
	using namespace UE::DreamStateTree::Tests::LangSupport;

	namespace
	{
		TArray<FDstToken> LexText(const FString& Text, FDstDiagnosticSink& Sink, const bool bEmitComments = false)
		{
			const FDstSourceText Source(TEXT("t.dtr"), Text);
			FDstLexOptions Options;
			Options.bEmitComments = bEmitComments;
			return LexDst(Source, Sink, Options);
		}

		/** What one token is expected to be. A null Text is not checked. */
		struct FTokenSpec
		{
			EDstTokenKind Kind;
			const TCHAR* Text;
			int32 Line;
			int32 Column;
			int32 Length;
		};

		/** The tokens must be exactly Specs, then one EndOfFile. Reports every difference with the token's index. */
		bool CheckTokens(FAutomationTestBase& Test, const TCHAR* Context, const TArray<FDstToken>& Tokens, const TArray<FTokenSpec>& Specs)
		{
			bool bOk = true;
			if (Tokens.Num() != Specs.Num() + 1)
			{
				Test.AddError(FString::Printf(TEXT("%s: expected %d tokens and the end of the file, got %d tokens in all."),
					Context, Specs.Num(), Tokens.Num()));
				return false;
			}
			for (int32 Index = 0; Index < Specs.Num(); ++Index)
			{
				const FDstToken& Token = Tokens[Index];
				const FTokenSpec& Spec = Specs[Index];
				if (Token.Kind != Spec.Kind)
				{
					Test.AddError(FString::Printf(TEXT("%s: token %d is %s, expected %s."), Context, Index, LexToString(Token.Kind), LexToString(Spec.Kind)));
					bOk = false;
					continue;
				}
				if (Spec.Text && !Token.Text.Equals(Spec.Text, ESearchCase::CaseSensitive))
				{
					Test.AddError(FString::Printf(TEXT("%s: token %d has text `%s`, expected `%s`."), Context, Index, *Token.Text, Spec.Text));
					bOk = false;
				}
				if (Token.Span.Line != Spec.Line || Token.Span.Column != Spec.Column)
				{
					Test.AddError(FString::Printf(TEXT("%s: token %d starts at (%d,%d), expected (%d,%d)."),
						Context, Index, Token.Span.Line, Token.Span.Column, Spec.Line, Spec.Column));
					bOk = false;
				}
				if (Token.Span.Length != Spec.Length)
				{
					Test.AddError(FString::Printf(TEXT("%s: token %d is %d characters long, expected %d."), Context, Index, Token.Span.Length, Spec.Length));
					bOk = false;
				}
			}
			if (!Tokens.Last().Is(EDstTokenKind::EndOfFile))
			{
				Test.AddError(FString::Printf(TEXT("%s: the last token is %s, not EndOfFile."), Context, LexToString(Tokens.Last().Kind)));
				bOk = false;
			}
			return bOk;
		}

		/** Exactly one diagnostic, with this code, at this position. */
		void CheckSingleDiagnostic(FAutomationTestBase& Test, const TCHAR* Context, const FDstDiagnosticSink& Sink, const TCHAR* Code, const int32 Line, const int32 Column)
		{
			if (Sink.Num() != 1)
			{
				Test.AddError(FString::Printf(TEXT("%s: expected only %s, got %d diagnostics: %s"), Context, Code, Sink.Num(), *DescribeDiagnostics(Sink)));
				return;
			}
			const FDstDiagnostic& Diagnostic = Sink.GetDiagnostics()[0];
			if (!Diagnostic.Code.Equals(Code, ESearchCase::CaseSensitive) || Diagnostic.Span.Line != Line || Diagnostic.Span.Column != Column)
			{
				Test.AddError(FString::Printf(TEXT("%s: expected %s at (%d,%d), got %s"), Context, Code, Line, Column, *FDstDiagnosticSink::ToWireString(Diagnostic)));
			}
		}

		int32 CountKind(const TArray<FDstToken>& Tokens, const EDstTokenKind Kind)
		{
			int32 Count = 0;
			for (const FDstToken& Token : Tokens)
			{
				Count += Token.Kind == Kind ? 1 : 0;
			}
			return Count;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Positions
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerPositionsTest, "DreamStateTree.Lang.Lexer.Positions",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerPositionsTest::RunTest(const FString& Parameters)
	{
		// Columns count characters: a tab is one, a Chinese character is one.
		const FString Text = TEXT("State Root {\n\t-> 空闲  on Tick\n}");
		FDstDiagnosticSink Sink;
		const TArray<FDstToken> Tokens = LexText(Text, Sink);
		TestEqual(TEXT("no diagnostics"), Sink.Num(), 0);

		CheckTokens(*this, TEXT("positions"), Tokens,
		{
			{ EDstTokenKind::Identifier, TEXT("State"), 1, 1, 5 },
			{ EDstTokenKind::Identifier, TEXT("Root"), 1, 7, 4 },
			{ EDstTokenKind::LeftBrace, nullptr, 1, 12, 1 },
			{ EDstTokenKind::Arrow, nullptr, 2, 2, 2 },
			{ EDstTokenKind::Identifier, TEXT("空闲"), 2, 5, 2 },
			{ EDstTokenKind::Identifier, TEXT("on"), 2, 9, 2 },
			{ EDstTokenKind::Identifier, TEXT("Tick"), 2, 12, 4 },
			{ EDstTokenKind::RightBrace, nullptr, 3, 1, 1 },
		});

		// Offsets are authoritative: slicing the source with a span gives the token back.
		const FDstSourceText Source(TEXT("t.dtr"), Text);
		for (const FDstToken& Token : Tokens)
		{
			if (Token.Is(EDstTokenKind::Identifier))
			{
				TestEqualSensitive(FString::Printf(TEXT("slice of %s"), *Token.Text), Source.Slice(Token.Span), Token.Text);
			}
		}
		TestEqual(TEXT("the end of the file is at the end of the text"), Tokens.Last().Span.Offset, Text.Len());
		TestEqual(TEXT("the end of the file is empty"), Tokens.Last().Span.Length, 0);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerEndOfFileTest, "DreamStateTree.Lang.Lexer.EndOfFile",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerEndOfFileTest::RunTest(const FString& Parameters)
	{
		const TCHAR* Cases[] = { TEXT(""), TEXT("   "), TEXT("a"), TEXT("a\n"), TEXT("a // c"), TEXT("\"unclosed"), TEXT("/* open") };
		for (const TCHAR* Case : Cases)
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Case, Sink);
			TestEqual(FString::Printf(TEXT("exactly one EndOfFile in `%s`"), Case), CountKind(Tokens, EDstTokenKind::EndOfFile), 1);
			TestTrue(FString::Printf(TEXT("EndOfFile is last in `%s`"), Case), Tokens.Num() > 0 && Tokens.Last().Is(EDstTokenKind::EndOfFile));
		}

		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT(""), Sink);
			CheckTokens(*this, TEXT("empty text"), Tokens, {});
			TestEqual(TEXT("empty text: line"), Tokens[0].Span.Line, 1);
			TestEqual(TEXT("empty text: column"), Tokens[0].Span.Column, 1);
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("a\n"), Sink);
			CheckTokens(*this, TEXT("trailing newline"), Tokens, { { EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 } });
			TestEqual(TEXT("trailing newline: the end is on the next line"), Tokens.Last().Span.Line, 2);
			TestEqual(TEXT("trailing newline: at its first column"), Tokens.Last().Span.Column, 1);
			TestEqual(TEXT("trailing newline: at the end of the text"), Tokens.Last().Span.Offset, 2);
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Line endings
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerLineEndingsTest, "DreamStateTree.Lang.Lexer.LineEndings",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerLineEndingsTest::RunTest(const FString& Parameters)
	{
		{
			// \n, \r\n and a lone \r each end one line.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("a\nb\r\nc\rd"), Sink);
			CheckTokens(*this, TEXT("mixed endings"), Tokens,
			{
				{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
				{ EDstTokenKind::Identifier, TEXT("b"), 2, 1, 1 },
				{ EDstTokenKind::Identifier, TEXT("c"), 3, 1, 1 },
				{ EDstTokenKind::Identifier, TEXT("d"), 4, 1, 1 },
			});
			for (int32 Index = 1; Index < 4; ++Index)
			{
				TestEqual(FString::Printf(TEXT("one line break before token %d"), Index), Tokens[Index].NewlinesBefore, 1);
				TestTrue(FString::Printf(TEXT("token %d starts its line"), Index), Tokens[Index].bAtLineStart);
			}
			TestEqual(TEXT("no break before the first token"), Tokens[0].NewlinesBefore, 0);
			TestTrue(TEXT("the first token starts its line"), Tokens[0].bAtLineStart);
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("a\n\n\nb"), Sink);
			TestTrue(TEXT("blank lines: enough tokens"), Tokens.Num() == 3);
			if (Tokens.Num() == 3)
			{
				TestEqual(TEXT("blank lines are counted"), Tokens[1].NewlinesBefore, 3);
				TestEqual(TEXT("blank lines move the line number"), Tokens[1].Span.Line, 4);
			}
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("a\r\r\rb"), Sink);
			TestTrue(TEXT("lone CRs: enough tokens"), Tokens.Num() == 3);
			if (Tokens.Num() == 3)
			{
				TestEqual(TEXT("each lone CR is a line"), Tokens[1].Span.Line, 4);
			}
		}
		{
			// The same text with LF and with CRLF has the same lines and columns.
			FDstDiagnosticSink LfSink;
			FDstDiagnosticSink CrlfSink;
			const TArray<FDstToken> Lf = LexText(TEXT("State A {\n    do Wait(1)\n}\n"), LfSink);
			const TArray<FDstToken> Crlf = LexText(TEXT("State A {\r\n    do Wait(1)\r\n}\r\n"), CrlfSink);
			TestEqual(TEXT("CRLF: same token count"), Crlf.Num(), Lf.Num());
			if (Crlf.Num() == Lf.Num())
			{
				for (int32 Index = 0; Index < Lf.Num(); ++Index)
				{
					const bool bSame = Lf[Index].Kind == Crlf[Index].Kind
						&& Lf[Index].Span.Line == Crlf[Index].Span.Line
						&& Lf[Index].Span.Column == Crlf[Index].Span.Column
						&& Lf[Index].Text == Crlf[Index].Text
						&& Lf[Index].NewlinesBefore == Crlf[Index].NewlinesBefore;
					TestTrue(FString::Printf(TEXT("CRLF: token %d matches the LF token"), Index), bSame);
				}
			}
		}
		{
			// A byte-order mark is not text.
			FString WithBom;
			WithBom.AppendChar(static_cast<TCHAR>(0xFEFF));
			WithBom += TEXT("a");
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(WithBom, Sink);
			CheckTokens(*this, TEXT("byte-order mark"), Tokens, { { EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 } });
			TestEqual(TEXT("byte-order mark: no diagnostics"), Sink.Num(), 0);
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Names
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerBacktickNamesTest, "DreamStateTree.Lang.Lexer.BacktickNames",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerBacktickNamesTest::RunTest(const FString& Parameters)
	{
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("`Move To` `a-b.c` plain"), Sink);
			TestEqual(TEXT("no diagnostics"), Sink.Num(), 0);
			if (CheckTokens(*this, TEXT("backticks"), Tokens,
				{
					{ EDstTokenKind::Identifier, TEXT("Move To"), 1, 1, 9 },
					{ EDstTokenKind::Identifier, TEXT("a-b.c"), 1, 11, 7 },
					{ EDstTokenKind::Identifier, TEXT("plain"), 1, 19, 5 },
				}))
			{
				TestTrue(TEXT("a name in backticks is quoted"), Tokens[0].bQuoted);
				TestTrue(TEXT("any characters are allowed between the backticks"), Tokens[1].bQuoted);
				TestFalse(TEXT("a plain identifier is not quoted"), Tokens[2].bQuoted);
			}
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("`中 文` `State`"), Sink);
			TestEqual(TEXT("Chinese: no diagnostics"), Sink.Num(), 0);
			CheckTokens(*this, TEXT("Chinese and keyword in backticks"), Tokens,
			{
				{ EDstTokenKind::Identifier, TEXT("中 文"), 1, 1, 5 },
				{ EDstTokenKind::Identifier, TEXT("State"), 1, 7, 7 },
			});
		}
		{
			// Not closed on its line: it runs to the end of the line and the next line is untouched.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("`Idle\nnext"), Sink);
			CheckSingleDiagnostic(*this, TEXT("unclosed backtick"), Sink, TEXT("DST1007"), 1, 1);
			CheckTokens(*this, TEXT("unclosed backtick"), Tokens,
			{
				{ EDstTokenKind::Identifier, TEXT("Idle"), 1, 1, 5 },
				{ EDstTokenKind::Identifier, TEXT("next"), 2, 1, 4 },
			});
			if (Tokens.Num() > 0)
			{
				TestTrue(TEXT("unclosed backtick: still a quoted name"), Tokens[0].bQuoted);
			}
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("``"), Sink);
			CheckSingleDiagnostic(*this, TEXT("empty backticks"), Sink, TEXT("DST1007"), 1, 1);
			CheckTokens(*this, TEXT("empty backticks"), Tokens, { { EDstTokenKind::Identifier, TEXT(""), 1, 1, 2 } });
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerIdentifiersTest, "DreamStateTree.Lang.Lexer.Identifiers",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerIdentifiersTest::RunTest(const FString& Parameters)
	{
		{
			const TArray<FString> Names = { TEXT("_x1"), TEXT("Wait2"), TEXT("移动速度"), TEXT("Ünïcode"), TEXT("ＡＢ"), TEXT("a1b2") };
			const FString Text = FString::Join(Names, TEXT(" "));
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Text, Sink);
			TestEqual(TEXT("no diagnostics"), Sink.Num(), 0);
			TestEqual(TEXT("one token per name"), Tokens.Num(), Names.Num() + 1);
			for (int32 Index = 0; Index < Names.Num() && Index < Tokens.Num(); ++Index)
			{
				TestTrue(FString::Printf(TEXT("%s is an identifier"), *Names[Index]), Tokens[Index].Is(EDstTokenKind::Identifier));
				TestEqualSensitive(FString::Printf(TEXT("%s keeps its spelling"), *Names[Index]), Tokens[Index].Text, Names[Index]);
				TestTrue(FString::Printf(TEXT("%s is a plain identifier"), *Names[Index]), IsDstPlainIdentifier(Names[Index]));
			}
		}
		{
			// No reserved words: every keyword of the grammar is an identifier to the lexer.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("State true not and Settings on if do None"), Sink);
			TestEqual(TEXT("keywords: no diagnostics"), Sink.Num(), 0);
			TestEqual(TEXT("keywords: nine identifiers"), CountKind(Tokens, EDstTokenKind::Identifier), 9);
			if (Tokens.Num() > 0)
			{
				TestTrue(TEXT("IsIdentifier compares the text"), Tokens[0].IsIdentifier(TEXT("State")));
				TestFalse(TEXT("IsIdentifier is case-sensitive"), Tokens[0].IsIdentifier(TEXT("state")));
				TestFalse(TEXT("a keyword is not quoted"), Tokens[0].bQuoted);
			}
		}
		{
			// Non-ASCII spacing separates tokens: the ideographic space a Chinese input method types, and the no-break space.
			const FString Text = TEXT("A") + FString::Chr(static_cast<TCHAR>(0x3000)) + TEXT("B") + FString::Chr(static_cast<TCHAR>(0x00A0)) + TEXT("C");
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Text, Sink);
			TestEqual(TEXT("Unicode spaces: no diagnostics"), Sink.Num(), 0);
			CheckTokens(*this, TEXT("Unicode spaces"), Tokens,
			{
				{ EDstTokenKind::Identifier, TEXT("A"), 1, 1, 1 },
				{ EDstTokenKind::Identifier, TEXT("B"), 1, 3, 1 },
				{ EDstTokenKind::Identifier, TEXT("C"), 1, 5, 1 },
			});
		}
		{
			// A digit cannot start a name: `1a` is a malformed number.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("1a"), Sink);
			CheckSingleDiagnostic(*this, TEXT("1a"), Sink, TEXT("DST1004"), 1, 1);
			CheckTokens(*this, TEXT("1a"), Tokens, { { EDstTokenKind::Number, TEXT("1a"), 1, 1, 2 } });
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerPlainIdentifierTest, "DreamStateTree.Lang.Lexer.PlainIdentifier",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerPlainIdentifierTest::RunTest(const FString& Parameters)
	{
		const TCHAR* Plain[] = { TEXT("Move"), TEXT("_"), TEXT("_a1"), TEXT("中文"), TEXT("Wait2"), TEXT("a_b"), TEXT("State") };
		for (const TCHAR* Name : Plain)
		{
			TestTrue(FString::Printf(TEXT("`%s` reads back as one identifier"), Name), IsDstPlainIdentifier(Name));
		}
		const TCHAR* NotPlain[] = { TEXT(""), TEXT("Move To"), TEXT("1a"), TEXT("a-b"), TEXT("a.b"), TEXT("`x`"), TEXT("Next."), TEXT("a\tb") };
		for (const TCHAR* Name : NotPlain)
		{
			TestFalse(FString::Printf(TEXT("`%s` needs backticks"), Name), IsDstPlainIdentifier(Name));
		}

		// Whatever the rule says is plain really lexes as one unquoted identifier, and the rest does not.
		for (const TCHAR* Name : Plain)
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Name, Sink);
			TestTrue(FString::Printf(TEXT("`%s` lexes as one identifier"), Name),
				Tokens.Num() == 2 && Tokens[0].Is(EDstTokenKind::Identifier) && !Tokens[0].bQuoted && Tokens[0].Text == Name);
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Numbers and strings
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerNumbersTest, "DreamStateTree.Lang.Lexer.Numbers",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerNumbersTest::RunTest(const FString& Parameters)
	{
		struct FGood
		{
			const TCHAR* Text;
			double Value;
		};
		const FGood Good[] =
		{
			{ TEXT("0"), 0.0 }, { TEXT("12"), 12.0 }, { TEXT("200"), 200.0 }, { TEXT("0.5"), 0.5 }, { TEXT("0.75"), 0.75 },
			{ TEXT(".5"), 0.5 }, { TEXT("1e3"), 1000.0 }, { TEXT("1E3"), 1000.0 }, { TEXT("2.5E+4"), 25000.0 }, { TEXT("1e-3"), 0.001 },
		};
		for (const FGood& Case : Good)
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Case.Text, Sink);
			TestEqual(FString::Printf(TEXT("%s: no diagnostics"), Case.Text), Sink.Num(), 0);
			if (Tokens.Num() == 2 && Tokens[0].Is(EDstTokenKind::Number))
			{
				TestEqualSensitive(FString::Printf(TEXT("%s: the lexeme is kept"), Case.Text), Tokens[0].Text, FString(Case.Text));
				TestTrue(FString::Printf(TEXT("%s: the value is %g, got %g"), Case.Text, Case.Value, Tokens[0].Number),
					FMath::IsNearlyEqual(Tokens[0].Number, Case.Value, 1e-9));
				TestEqual(FString::Printf(TEXT("%s: the span covers the lexeme"), Case.Text), Tokens[0].Span.Length, FString(Case.Text).Len());
			}
			else
			{
				AddError(FString::Printf(TEXT("%s: expected one Number token."), Case.Text));
			}
		}

		// Whatever is glued to a number belongs to it and makes it malformed, so the mistake is one token and one error.
		const TCHAR* Bad[] = { TEXT("1."), TEXT("1e"), TEXT("1e+"), TEXT("0x1F"), TEXT("2px"), TEXT("1.2.3"), TEXT("1.5f") };
		for (const TCHAR* Case : Bad)
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Case, Sink);
			CheckSingleDiagnostic(*this, Case, Sink, TEXT("DST1004"), 1, 1);
			CheckTokens(*this, Case, Tokens, { { EDstTokenKind::Number, Case, 1, 1, FString(Case).Len() } });
		}

		{
			// A minus sign is an operator, not part of the number.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("-5"), Sink);
			CheckTokens(*this, TEXT("-5"), Tokens,
			{
				{ EDstTokenKind::Minus, nullptr, 1, 1, 1 },
				{ EDstTokenKind::Number, TEXT("5"), 1, 2, 1 },
			});
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("Delay(1,0.3)"), Sink);
			CheckTokens(*this, TEXT("Delay(1,0.3)"), Tokens,
			{
				{ EDstTokenKind::Identifier, TEXT("Delay"), 1, 1, 5 },
				{ EDstTokenKind::LeftParen, nullptr, 1, 6, 1 },
				{ EDstTokenKind::Number, TEXT("1"), 1, 7, 1 },
				{ EDstTokenKind::Comma, nullptr, 1, 8, 1 },
				{ EDstTokenKind::Number, TEXT("0.3"), 1, 9, 3 },
				{ EDstTokenKind::RightParen, nullptr, 1, 12, 1 },
			});
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerStringsTest, "DreamStateTree.Lang.Lexer.Strings",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerStringsTest::RunTest(const FString& Parameters)
	{
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("\"plain\" \"\" \"走路\""), Sink);
			TestEqual(TEXT("plain strings: no diagnostics"), Sink.Num(), 0);
			CheckTokens(*this, TEXT("plain strings"), Tokens,
			{
				{ EDstTokenKind::String, TEXT("plain"), 1, 1, 7 },
				{ EDstTokenKind::String, TEXT(""), 1, 9, 2 },
				{ EDstTokenKind::String, TEXT("走路"), 1, 12, 4 },
			});
		}
		{
			// Source text: "a\"b\\c\nd\te\rf" -- every escape the language has.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("\"a\\\"b\\\\c\\nd\\te\\rf\""), Sink);
			TestEqual(TEXT("escapes: no diagnostics"), Sink.Num(), 0);
			if (Tokens.Num() == 2 && Tokens[0].Is(EDstTokenKind::String))
			{
				TestEqualSensitive(TEXT("escapes are resolved"), Tokens[0].Text, FString(TEXT("a\"b\\c\nd\te\rf")));
				TestEqual(TEXT("the span covers the source, not the value"), Tokens[0].Span.Length, 18);
			}
			else
			{
				AddError(TEXT("escapes: expected one String token."));
			}
		}
		{
			// Source text: "a\qb" -- \q is no escape. Both characters stay so the value shows what was written.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("\"a\\qb\""), Sink);
			CheckSingleDiagnostic(*this, TEXT("bad escape"), Sink, TEXT("DST1006"), 1, 3);
			CheckTokens(*this, TEXT("bad escape"), Tokens, { { EDstTokenKind::String, TEXT("a\\qb"), 1, 1, 6 } });
		}
		{
			// A string never spans a line: a missing quote costs one line, not the rest of the file.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("\"abc\nx"), Sink);
			CheckSingleDiagnostic(*this, TEXT("unclosed string"), Sink, TEXT("DST1002"), 1, 1);
			CheckTokens(*this, TEXT("unclosed string"), Tokens,
			{
				{ EDstTokenKind::String, TEXT("abc"), 1, 1, 4 },
				{ EDstTokenKind::Identifier, TEXT("x"), 2, 1, 1 },
			});
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("\"abc"), Sink);
			CheckSingleDiagnostic(*this, TEXT("unclosed string at the end"), Sink, TEXT("DST1002"), 1, 1);
			CheckTokens(*this, TEXT("unclosed string at the end"), Tokens, { { EDstTokenKind::String, TEXT("abc"), 1, 1, 4 } });
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Comments and directives
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerCommentsTest, "DreamStateTree.Lang.Lexer.Comments",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerCommentsTest::RunTest(const FString& Parameters)
	{
		const FString Text = TEXT("a // x\nb /* y\nz */ c");
		{
			// Skipped: the tokens around a comment are the same, and a comment counts as something on its line.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Text, Sink);
			TestEqual(TEXT("skipped: no diagnostics"), Sink.Num(), 0);
			if (CheckTokens(*this, TEXT("skipped comments"), Tokens,
				{
					{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
					{ EDstTokenKind::Identifier, TEXT("b"), 2, 1, 1 },
					{ EDstTokenKind::Identifier, TEXT("c"), 3, 6, 1 },
				}))
			{
				TestEqual(TEXT("one break between a and b"), Tokens[1].NewlinesBefore, 1);
				TestTrue(TEXT("b starts its line"), Tokens[1].bAtLineStart);
				TestEqual(TEXT("the break inside the block comment counts"), Tokens[2].NewlinesBefore, 1);
				TestFalse(TEXT("c follows a comment on its line"), Tokens[2].bAtLineStart);
			}
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Text, Sink, /*bEmitComments*/ true);
			TestEqual(TEXT("emitted: no diagnostics"), Sink.Num(), 0);
			if (CheckTokens(*this, TEXT("emitted comments"), Tokens,
				{
					{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
					{ EDstTokenKind::Comment, TEXT("// x"), 1, 3, 4 },
					{ EDstTokenKind::Identifier, TEXT("b"), 2, 1, 1 },
					{ EDstTokenKind::Comment, TEXT("/* y\nz */"), 2, 3, 9 },
					{ EDstTokenKind::Identifier, TEXT("c"), 3, 6, 1 },
				}))
			{
				TestEqual(TEXT("one break between the comment and b"), Tokens[2].NewlinesBefore, 1);
				TestFalse(TEXT("a trailing comment does not start its line"), Tokens[1].bAtLineStart);
			}
		}
		{
			// Not closed: the error is at the opening `/*`, the comment runs to the end of the file.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("a /* never closed\nb"), Sink);
			CheckSingleDiagnostic(*this, TEXT("unclosed block comment"), Sink, TEXT("DST1003"), 1, 3);
			CheckTokens(*this, TEXT("unclosed block comment"), Tokens, { { EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 } });

			FDstDiagnosticSink EmitSink;
			const TArray<FDstToken> WithComments = LexText(TEXT("a /* never closed\nb"), EmitSink, /*bEmitComments*/ true);
			CheckTokens(*this, TEXT("unclosed block comment, emitted"), WithComments,
			{
				{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
				{ EDstTokenKind::Comment, TEXT("/* never closed\nb"), 1, 3, 17 },
			});
		}
		{
			// A slash that does not start a comment is an operator.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("a/b a / b"), Sink);
			TestEqual(TEXT("division: no diagnostics"), Sink.Num(), 0);
			TestEqual(TEXT("division: two slashes"), CountKind(Tokens, EDstTokenKind::Slash), 2);
			TestEqual(TEXT("division: no comments"), CountKind(Tokens, EDstTokenKind::Comment), 0);
		}
		{
			// A comment is not a string and a string is not a comment.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("\"a // b\" c"), Sink);
			CheckTokens(*this, TEXT("// inside a string"), Tokens,
			{
				{ EDstTokenKind::String, TEXT("a // b"), 1, 1, 8 },
				{ EDstTokenKind::Identifier, TEXT("c"), 1, 10, 1 },
			});
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerDirectivesTest, "DreamStateTree.Lang.Lexer.Directives",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerDirectivesTest::RunTest(const FString& Parameters)
	{
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("#include \"a.dth\"\n    #Region \"x\"\n#EndRegion\n"), Sink);
			TestEqual(TEXT("directives: no diagnostics"), Sink.Num(), 0);
			CheckTokens(*this, TEXT("directives"), Tokens,
			{
				{ EDstTokenKind::Directive, TEXT("include"), 1, 1, 8 },
				{ EDstTokenKind::String, TEXT("a.dth"), 1, 10, 7 },
				{ EDstTokenKind::Directive, TEXT("Region"), 2, 5, 7 },
				{ EDstTokenKind::String, TEXT("x"), 2, 13, 3 },
				{ EDstTokenKind::Directive, TEXT("EndRegion"), 3, 1, 10 },
			});
		}
		{
			// Only spaces and tabs may come before it, and a skipped comment line does not count against the next line.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("// c\n#include \"x\""), Sink);
			TestEqual(TEXT("after a comment line: no diagnostics"), Sink.Num(), 0);
			CheckTokens(*this, TEXT("after a comment line"), Tokens,
			{
				{ EDstTokenKind::Directive, TEXT("include"), 2, 1, 8 },
				{ EDstTokenKind::String, TEXT("x"), 2, 10, 3 },
			});
		}
		{
			// In the middle of a line `#` is not a directive.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("x #Region"), Sink);
			CheckSingleDiagnostic(*this, TEXT("mid-line #"), Sink, TEXT("DST1001"), 1, 3);
			CheckTokens(*this, TEXT("mid-line #"), Tokens,
			{
				{ EDstTokenKind::Identifier, TEXT("x"), 1, 1, 1 },
				{ EDstTokenKind::Unknown, nullptr, 1, 3, 1 },
				{ EDstTokenKind::Identifier, TEXT("Region"), 1, 4, 6 },
			});
		}
		{
			// A block comment before it, on the same line, is something on that line.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("/* c */ #include"), Sink);
			CheckSingleDiagnostic(*this, TEXT("# after a comment"), Sink, TEXT("DST1001"), 1, 9);
			TestTrue(TEXT("# after a comment is not a directive"), Tokens.Num() > 0 && Tokens[0].Is(EDstTokenKind::Unknown));
		}
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("# x"), Sink);
			CheckSingleDiagnostic(*this, TEXT("# alone"), Sink, TEXT("DST1001"), 1, 1);
			TestTrue(TEXT("# alone is an Unknown token"), Tokens.Num() > 0 && Tokens[0].Is(EDstTokenKind::Unknown));
		}
		{
			// An unknown word is still a Directive token, so the parser can skip it quietly.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("#pragma once"), Sink);
			CheckSingleDiagnostic(*this, TEXT("unknown directive"), Sink, TEXT("DST1005"), 1, 1);
			CheckTokens(*this, TEXT("unknown directive"), Tokens,
			{
				{ EDstTokenKind::Directive, TEXT("pragma"), 1, 1, 7 },
				{ EDstTokenKind::Identifier, TEXT("once"), 1, 9, 4 },
			});
		}
		{
			// A wrong case suggests the right spelling.
			FDstDiagnosticSink Sink;
			LexText(TEXT("#region \"x\""), Sink);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Sink, TEXT("DST1005"));
			TestNotNull(TEXT("#region is reported"), Diagnostic);
			if (Diagnostic)
			{
				TestTrue(TEXT("#region: the hint names #Region"), Diagnostic->Hint.Contains(TEXT("#Region")));
			}
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Operators and stray characters
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerOperatorsTest, "DreamStateTree.Lang.Lexer.Operators",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerOperatorsTest::RunTest(const FString& Parameters)
	{
		{
			const EDstTokenKind Expected[] =
			{
				EDstTokenKind::LeftParen, EDstTokenKind::RightParen, EDstTokenKind::LeftBrace, EDstTokenKind::RightBrace,
				EDstTokenKind::LeftBracket, EDstTokenKind::RightBracket, EDstTokenKind::Comma, EDstTokenKind::Semicolon,
				EDstTokenKind::Colon, EDstTokenKind::Dot, EDstTokenKind::Question, EDstTokenKind::Assign, EDstTokenKind::Arrow,
				EDstTokenKind::Plus, EDstTokenKind::Minus, EDstTokenKind::Star, EDstTokenKind::Slash, EDstTokenKind::Percent,
				EDstTokenKind::Bang, EDstTokenKind::AmpersandAmpersand, EDstTokenKind::PipePipe, EDstTokenKind::Less,
				EDstTokenKind::Greater, EDstTokenKind::LessEqual, EDstTokenKind::GreaterEqual, EDstTokenKind::EqualEqual,
				EDstTokenKind::BangEqual,
			};
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("( ) { } [ ] , ; : . ? = -> + - * / % ! && || < > <= >= == !="), Sink);
			TestEqual(TEXT("operators: no diagnostics"), Sink.Num(), 0);
			const int32 ExpectedCount = UE_ARRAY_COUNT(Expected);
			TestEqual(TEXT("operators: token count"), Tokens.Num(), ExpectedCount + 1);
			for (int32 Index = 0; Index < ExpectedCount && Index < Tokens.Num(); ++Index)
			{
				TestTrue(FString::Printf(TEXT("operator %d is %s, got %s"), Index, LexToString(Expected[Index]), LexToString(Tokens[Index].Kind)),
					Tokens[Index].Kind == Expected[Index]);
			}

			// Each spelling lexes back to its own kind, whole.
			for (const EDstTokenKind Kind : Expected)
			{
				const FString Spelling = GetDstTokenSpelling(Kind);
				FDstDiagnosticSink SpellingSink;
				const TArray<FDstToken> One = LexText(Spelling, SpellingSink);
				TestTrue(FString::Printf(TEXT("`%s` is one %s"), *Spelling, LexToString(Kind)),
					One.Num() == 2 && One[0].Kind == Kind && One[0].Span.Length == Spelling.Len() && SpellingSink.Num() == 0);
			}
		}
		{
			// The longest spelling wins, and no spaces are needed around an operator.
			FDstDiagnosticSink Sink;
			CheckTokens(*this, TEXT("a->b"), LexText(TEXT("a->b"), Sink),
			{
				{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
				{ EDstTokenKind::Arrow, nullptr, 1, 2, 2 },
				{ EDstTokenKind::Identifier, TEXT("b"), 1, 4, 1 },
			});
			CheckTokens(*this, TEXT("x<-1"), LexText(TEXT("x<-1"), Sink),
			{
				{ EDstTokenKind::Identifier, TEXT("x"), 1, 1, 1 },
				{ EDstTokenKind::Less, nullptr, 1, 2, 1 },
				{ EDstTokenKind::Minus, nullptr, 1, 3, 1 },
				{ EDstTokenKind::Number, TEXT("1"), 1, 4, 1 },
			});
			CheckTokens(*this, TEXT("x-->y"), LexText(TEXT("x-->y"), Sink),
			{
				{ EDstTokenKind::Identifier, TEXT("x"), 1, 1, 1 },
				{ EDstTokenKind::Minus, nullptr, 1, 2, 1 },
				{ EDstTokenKind::Arrow, nullptr, 1, 3, 2 },
				{ EDstTokenKind::Identifier, TEXT("y"), 1, 5, 1 },
			});
			CheckTokens(*this, TEXT("a<=b!=c"), LexText(TEXT("a<=b!=c"), Sink),
			{
				{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
				{ EDstTokenKind::LessEqual, nullptr, 1, 2, 2 },
				{ EDstTokenKind::Identifier, TEXT("b"), 1, 4, 1 },
				{ EDstTokenKind::BangEqual, nullptr, 1, 5, 2 },
				{ EDstTokenKind::Identifier, TEXT("c"), 1, 7, 1 },
			});
			CheckTokens(*this, TEXT("Params.AttackRange*0.75"), LexText(TEXT("Params.AttackRange*0.75"), Sink),
			{
				{ EDstTokenKind::Identifier, TEXT("Params"), 1, 1, 6 },
				{ EDstTokenKind::Dot, nullptr, 1, 7, 1 },
				{ EDstTokenKind::Identifier, TEXT("AttackRange"), 1, 8, 11 },
				{ EDstTokenKind::Star, nullptr, 1, 19, 1 },
				{ EDstTokenKind::Number, TEXT("0.75"), 1, 20, 4 },
			});
			TestEqual(TEXT("operators without spaces: no diagnostics"), Sink.Num(), 0);
		}
		{
			// `&&`, `||` and `!` are tokens: it is the parser that tells the author to write and, or and not.
			FDstDiagnosticSink Sink;
			CheckTokens(*this, TEXT("a && !b || c"), LexText(TEXT("a && !b || c"), Sink),
			{
				{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
				{ EDstTokenKind::AmpersandAmpersand, nullptr, 1, 3, 2 },
				{ EDstTokenKind::Bang, nullptr, 1, 6, 1 },
				{ EDstTokenKind::Identifier, TEXT("b"), 1, 7, 1 },
				{ EDstTokenKind::PipePipe, nullptr, 1, 9, 2 },
				{ EDstTokenKind::Identifier, TEXT("c"), 1, 12, 1 },
			});
			TestEqual(TEXT("&& and ||: the lexer has nothing to say"), Sink.Num(), 0);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerStrayCharactersTest, "DreamStateTree.Lang.Lexer.StrayCharacters",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerStrayCharactersTest::RunTest(const FString& Parameters)
	{
		const TCHAR* Stray[] = { TEXT("@"), TEXT("$"), TEXT("~"), TEXT("\\"), TEXT("^"), TEXT("'"), TEXT("&"), TEXT("|") };
		for (const TCHAR* Case : Stray)
		{
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(Case, Sink);
			CheckSingleDiagnostic(*this, Case, Sink, TEXT("DST1001"), 1, 1);
			CheckTokens(*this, Case, Tokens, { { EDstTokenKind::Unknown, Case, 1, 1, 1 } });
		}

		{
			// The lexer goes on after a stray character, and positions stay right.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(TEXT("a @ b"), Sink);
			CheckSingleDiagnostic(*this, TEXT("a @ b"), Sink, TEXT("DST1001"), 1, 3);
			CheckTokens(*this, TEXT("a @ b"), Tokens,
			{
				{ EDstTokenKind::Identifier, TEXT("a"), 1, 1, 1 },
				{ EDstTokenKind::Unknown, TEXT("@"), 1, 3, 1 },
				{ EDstTokenKind::Identifier, TEXT("b"), 1, 5, 1 },
			});
		}
		{
			FDstDiagnosticSink Sink;
			LexText(TEXT("&"), Sink);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Sink, TEXT("DST1001"));
			TestTrue(TEXT("a lone & hints at &&"), Diagnostic && Diagnostic->Hint.Contains(TEXT("&&")));
		}
		{
			FDstDiagnosticSink Sink;
			LexText(TEXT("|"), Sink);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Sink, TEXT("DST1001"));
			TestTrue(TEXT("a lone | hints at ||"), Diagnostic && Diagnostic->Hint.Contains(TEXT("||")));
		}
		{
			FDstDiagnosticSink Sink;
			LexText(TEXT("'"), Sink);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Sink, TEXT("DST1001"));
			TestTrue(TEXT("a single quote hints at double quotes"), Diagnostic && Diagnostic->Hint.Contains(TEXT("double")));
		}
		{
			// A control character is named by its code point.
			FDstDiagnosticSink Sink;
			const TArray<FDstToken> Tokens = LexText(FString::Chr(static_cast<TCHAR>(0x01)), Sink);
			CheckSingleDiagnostic(*this, TEXT("control character"), Sink, TEXT("DST1001"), 1, 1);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Sink, TEXT("DST1001"));
			TestTrue(TEXT("a control character is named by its code point"), Diagnostic && Diagnostic->Message.Contains(TEXT("U+0001")));
			TestTrue(TEXT("a control character is an Unknown token"), Tokens.Num() > 0 && Tokens[0].Is(EDstTokenKind::Unknown));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Names of token kinds
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangLexerKindNamesTest, "DreamStateTree.Lang.Lexer.KindNames",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangLexerKindNamesTest::RunTest(const FString& Parameters)
	{
		// LexToString falls back to "Unknown" for a kind it has no case for, so a new kind that was forgotten there shows up here.
		TSet<FString> Seen;
		for (int32 Value = 0; Value <= static_cast<int32>(EDstTokenKind::Unknown); ++Value)
		{
			const EDstTokenKind Kind = static_cast<EDstTokenKind>(Value);
			const FString Name = LexToString(Kind);
			TestFalse(FString::Printf(TEXT("token kind %d has a name"), Value), Name.IsEmpty());
			if (Kind != EDstTokenKind::Unknown)
			{
				TestFalse(FString::Printf(TEXT("token kind %d (%s) has a case in LexToString"), Value, *Name), Name == TEXT("Unknown"));
			}
			TestFalse(FString::Printf(TEXT("token kind name `%s` is unique"), *Name), Seen.Contains(Name));
			Seen.Add(Name);
		}

		// Only punctuation and operators have a spelling.
		TestEqualSensitive(TEXT("an identifier has no spelling"), FString(GetDstTokenSpelling(EDstTokenKind::Identifier)), FString());
		TestEqualSensitive(TEXT("a comment has no spelling"), FString(GetDstTokenSpelling(EDstTokenKind::Comment)), FString());
		TestEqualSensitive(TEXT("-> spelling"), FString(GetDstTokenSpelling(EDstTokenKind::Arrow)), FString(TEXT("->")));
		TestEqualSensitive(TEXT("&& spelling"), FString(GetDstTokenSpelling(EDstTokenKind::AmpersandAmpersand)), FString(TEXT("&&")));
		TestEqualSensitive(TEXT("!= spelling"), FString(GetDstTokenSpelling(EDstTokenKind::BangEqual)), FString(TEXT("!=")));
		return true;
	}
}
