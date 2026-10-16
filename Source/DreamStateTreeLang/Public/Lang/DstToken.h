// Copyright DreamMoon. All Rights Reserved.
//
// Tokens. There are no reserved words at this level: `true`, `State`, `Settings` and every other word
// reach the parser as identifiers, and the parser recognises them by position (Docs/Grammar.md).

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstSource.h"

namespace UE::DreamStateTree::Lang
{
	enum class EDstTokenKind : uint8
	{
		EndOfFile,

		Identifier,
		/** Text is the lexeme as written; Number holds the value. */
		Number,
		/** Text is the value with escapes resolved; the span covers the quotes. */
		String,
		/**
		 * `#include`, `#Region` or `#EndRegion` at the start of a line. Text is the directive word
		 * without the `#` (`include`, `Region`, `EndRegion`). Its argument, if any, is the next token.
		 */
		Directive,

		// -- punctuation
		LeftParen, RightParen, LeftBrace, RightBrace, LeftBracket, RightBracket,
		Comma, Semicolon, Colon, Dot, Question,
		Assign,
		Arrow,

		// -- operators
		Plus, Minus, Star, Slash, Percent, Bang,
		AmpersandAmpersand, PipePipe,
		Less, Greater, LessEqual, GreaterEqual, EqualEqual, BangEqual,

		/** `// ...` or a block comment, delimiters included in Text. Only with FDstLexOptions::bEmitComments. */
		Comment,

		/** A character no token starts with. Reported as DST1001 and emitted so the parser fails at the right spot. */
		Unknown,
	};

	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstTokenKind Kind);
	/** The source spelling of a punctuation or operator kind (`->`, `&&`); empty for the others. */
	DREAMSTATETREELANG_API const TCHAR* GetDstTokenSpelling(EDstTokenKind Kind);

	struct FDstToken
	{
		EDstTokenKind Kind = EDstTokenKind::EndOfFile;
		FDstSpan Span;
		/** See EDstTokenKind for what each kind keeps here. Empty for punctuation. */
		FString Text;
		double Number = 0.0;
		/** True when the token is the first non-whitespace thing on its line. */
		bool bAtLineStart = false;
		/** Number of line breaks between the previous token (or comment) and this one. */
		int32 NewlinesBefore = 0;
		/** An Identifier written between backticks; Text is what is between them, spaces and all. */
		bool bQuoted = false;

		bool Is(EDstTokenKind InKind) const { return Kind == InKind; }
		bool IsIdentifier(const TCHAR* Name) const { return Kind == EDstTokenKind::Identifier && Text.Equals(Name, ESearchCase::CaseSensitive); }
	};

	struct FDstLexOptions
	{
		/** Emit Comment tokens instead of skipping comments. The parser asks for them when it keeps trivia. */
		bool bEmitComments = false;
	};

	/**
	 * Cuts a source text into tokens. Always ends with exactly one EndOfFile token. Problems are
	 * reported to the sink and lexing goes on (an unknown character becomes an Unknown token).
	 */
	/**
	 * Whether Name reads back as one identifier token: a letter or `_`, then letters, digits and `_`, with any
	 * Unicode letter counting. The rule is the lexer's; it lives there (DstLexer.cpp) so the two cannot differ.
	 */
	DREAMSTATETREELANG_API bool IsDstPlainIdentifier(const FString& Name);

	DREAMSTATETREELANG_API TArray<FDstToken> LexDst(const FDstSourceText& Source, class FDstDiagnosticSink& Sink, const FDstLexOptions& Options = FDstLexOptions());
}
