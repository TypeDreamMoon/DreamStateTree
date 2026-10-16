// Copyright DreamMoon. All Rights Reserved.
//
// The parser: recursive descent over LexDst's tokens, following Docs/Grammar.md.
//
// Shape of the grammar. A file is `#include` lines and one document. The document's braces hold members: sections
// (`Name = { entries; }`), templates, states, subtrees and regions. A state body holds clauses, child states and
// regions in source order. Clauses are not separated by anything and line breaks mean nothing: the parse of a clause
// stops at the first token that cannot continue it, and the next token must then start something of the body (a
// clause keyword, `->`, `State`, `Subtree`, a region directive or the closing `}`). Expressions end on their own,
// because the only way to continue one is an operator, a `.` or a `(`, and a clause keyword is none of those.
// There are no reserved words: the keywords are read by position, and at the top level of a clause expression (not
// inside brackets) an unquoted clause word is the next clause, never an operand. Inside brackets, or in backticks,
// it is an ordinary name.
//
// Metadata. `[ ... ]` is read after a state name, after a section entry, after the target/trigger/condition of a
// transition, and straight after the `)` of a call, where it is that call's node settings. A call at the very end
// of a transition clause is the one place where those two readings meet (`-> Root if Hour(22, 5) [ Delay = 1 ]`):
// there the bracket belongs to the transition when it names a transition-only option and nothing but a clause
// boundary follows it, and to the call otherwise. See CallTakesBracket.
//
// Error handling. Every "Expected X, found Y" goes through ShouldReportAtCurrent first, which keeps one mistake from
// being reported several times: a stray character the lexer already reported (an Unknown token) is not reported
// again, a token right after an unclosed string is not blamed for it, and the end of the file is reported once -- as
// DST2024 on the innermost bracket still open, or as the caller's own code when nothing is open. After a failed
// section entry the section skips to the next `;` at its own depth, or stops before its `}`; after a failed item of a
// state body the body skips to the next clause keyword, `State` or `}`; after a failed member of the document the
// document skips to the next member. A broken argument or list item skips to the next `,` of its list. Every loop
// makes progress on every iteration.
//
// Trivia. When asked to keep it, the lexer emits comments and the constructor moves them out of the token stream,
// remembering for every token how many comments precede it. Comments are then claimed strictly in source order:
//   - before a child of a container (member, entry, state item, task item, argument, list or tuple item, include)
//     is parsed, the unclaimed comments before its first token become its Leading;
//   - after the child (and its `;` or `,`), the first unclaimed comment that starts on the child's last line becomes
//     its Trailing;
//   - at the closing bracket, what is left becomes the container's Inner;
//   - any other comment (between a key and its `=`, inside metadata, inside a parameter list, ...) is swept up as the
//     cursor moves past it into the innermost child being parsed, and printed among its Leading comments.
// Nodes can still be destroyed while the parse recovers, so nothing is keyed by node address until the end:
// pending trivia is keyed by the offset of the child's first token (no two children start at the same token) or of
// a container's opening bracket (a section: its name), and CollectTrivia moves it onto the nodes that survived. The
// document is an anchor too: comments above it are its Leading, comments before the `)` of its header join them, and
// a comment after its `}` on that line is its Trailing. Comments after that, to the end of the file, have no slot of
// their own and join the Inner of the document.

#include "Lang/DstParser.h"

#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstSource.h"
#include "Lang/DstToken.h"

namespace UE::DreamStateTree::Lang
{
	namespace DstParserDetail
	{
		/** What a statement-parsing function did. */
		enum class EStmtResult : uint8
		{
			/** A node was produced (possibly after reporting a missing `;`). */
			Parsed,
			/** Tokens were consumed and reported or ignored; there is nothing to add and nothing to skip. */
			Skipped,
			/** An error was reported and the caller must skip to its recovery point. */
			Failed,
		};

		/** Trivia of one child, by comment index, until CollectTrivia turns it into FDstTrivia. */
		struct FPendingTrivia
		{
			TArray<int32> Leading;
			int32 BlankLinesBefore = 0;
			int32 Trailing = INDEX_NONE;
			TArray<int32> Inner;

			bool IsEmpty() const
			{
				return Leading.Num() == 0 && BlankLinesBefore == 0 && Trailing == INDEX_NONE && Inner.Num() == 0;
			}
		};

		/** What BeginAnchor claimed for a child before it was parsed. */
		struct FAnchorStart
		{
			TArray<int32> Leading;
			int32 BlankLinesBefore = 0;
		};

		struct FOpenBracket
		{
			FDstSpan Span;
			const TCHAR* Open = TEXT("");
			const TCHAR* Close = TEXT("");
		};

		/** The words that start a clause inside a state body; `->` is the seventh start. */
		static bool IsClauseWordText(const FString& Text)
		{
			static const TCHAR* const ClauseWords[] =
			{
				TEXT("on"), TEXT("if"), TEXT("pick"), TEXT("utility"), TEXT("link"), TEXT("do"),
			};
			for (const TCHAR* Word : ClauseWords)
			{
				if (Text.Equals(Word, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		class FDstParser
		{
		public:
			FDstParser(const FDstSourceText& InSource, TArray<FDstToken>&& InTokens, FDstDiagnosticSink& InSink,
				bool bInKeepTrivia, bool bInEndAlreadyExplained)
				: Source(InSource)
				, Sink(InSink)
				, bKeepTrivia(bInKeepTrivia)
				, bEndReported(bInEndAlreadyExplained)
			{
				Tokens.Reserve(InTokens.Num());
				CommentsBefore.Reserve(InTokens.Num());
				for (FDstToken& Token : InTokens)
				{
					if (Token.Is(EDstTokenKind::Comment))
					{
						Comments.Add(MoveTemp(Token));
						continue;
					}
					CommentsBefore.Add(Comments.Num());
					Tokens.Add(MoveTemp(Token));
				}

				// Every cursor operation relies on the last token being the end of the file.
				if (Tokens.Num() == 0 || !Tokens.Last().Is(EDstTokenKind::EndOfFile))
				{
					FDstToken End;
					End.Kind = EDstTokenKind::EndOfFile;
					End.Span = Source.MakeSpan(Source.Len(), 0);
					CommentsBefore.Add(Comments.Num());
					Tokens.Add(MoveTemp(End));
				}
			}

			// ------------------------------------------------------------------------------------
			// Entry points
			// ------------------------------------------------------------------------------------

			TUniquePtr<FDstFile> ParseFile()
			{
				TUniquePtr<FDstFile> File = MakeUnique<FDstFile>();
				File->Path = Source.GetPath();

				TArray<int32> Carry;
				// Junk before any document is reported only once a document turns up; with no document
				// at all, DST2002 alone says what is wrong.
				TArray<TPair<FDstSpan, FString>> JunkBeforeDocument;
				bool bInJunk = false;

				while (!AtEnd())
				{
					const int32 StepStart = Index;
					const FDstToken& First = Current();

					if (IsDirective(First, TEXT("include")))
					{
						bInJunk = false;
						FAnchorStart Anchor = BeginAnchor(Carry);
						TUniquePtr<FDstInclude> Include = ParseInclude();
						if (Include && File->Document)
						{
							Sink.Error(TEXT("DST2006"), Include->Span, TEXT("'#include' must come before the document."),
								TEXT("Move this line above the document."));
						}
						EndAnchor(MoveTemp(Anchor), Include.Get(), Carry);
						if (Include)
						{
							File->Includes.Add(MoveTemp(Include));
						}
					}
					else if (First.Is(EDstTokenKind::Directive) && !IsKnownDirective(First))
					{
						SkipUnknownDirective();
					}
					else if (IsDocumentStart(!File->Document.IsValid()))
					{
						bool bDocumentFailed = false;
						FAnchorStart Anchor = BeginAnchor(Carry);
						if (File->Document)
						{
							Sink.Error(TEXT("DST2003"), First.Span, TEXT("The file already has a document; a file holds exactly one."),
								TEXT("Move this document to a file of its own."));
							// Parsed for its diagnostics and to get past it, then dropped.
							ParseDocument(bDocumentFailed);
							EndAnchor(MoveTemp(Anchor), nullptr, Carry);
						}
						else
						{
							for (const TPair<FDstSpan, FString>& Junk : JunkBeforeDocument)
							{
								Sink.Error(TEXT("DST2001"), Junk.Key, Junk.Value);
							}
							JunkBeforeDocument.Reset();
							File->Document = ParseDocument(bDocumentFailed);
							EndAnchor(MoveTemp(Anchor), File->Document.Get(), Carry);
						}
						// After a broken header or a missing body, what follows is fallout, not news.
						bInJunk = bDocumentFailed;
					}
					else
					{
						if (!bInJunk && !First.Is(EDstTokenKind::Unknown))
						{
							if (File->Document)
							{
								Sink.Error(TEXT("DST2003"), First.Span, Expected(TEXT("the end of the file after the document")));
							}
							else
							{
								JunkBeforeDocument.Emplace(First.Span,
									Expected(TEXT("'#include' or a document such as 'StateTree(Name=\"...\", Schema=...) { ... }'")));
							}
						}
						bInJunk = true;
						SkipFileLevelJunk();
					}

					if (Index == StepStart && !AtEnd())
					{
						Advance();
					}
				}

				if (!File->Document)
				{
					const FDstSpan Span = JunkBeforeDocument.Num() > 0 ? JunkBeforeDocument[0].Key : Source.MakeSpan(0, 0);
					Sink.Error(TEXT("DST2002"), Span,
						TEXT("The file has no document; expected StateTree(...) or StateTreeLibrary(...) followed by '{ ... }'."),
						TEXT("A .dtr file holds StateTree(Name=\"...\", Schema=...) { ... }; a .dth library holds StateTreeLibrary(Name=\"...\") { ... }."));
				}
				else if (bKeepTrivia && File->Document->OpenSpan.Length > 0)
				{
					// FDstFile has no place for comments after the document, so they become the document's last
					// comments: moved inside the closing `}` by a reformat, but never lost.
					TArray<int32> Tail = MoveTemp(Carry);
					ClaimCommentsBefore(Index, Tail);
					if (Tail.Num() > 0)
					{
						PendingInner.FindOrAdd(File->Document->OpenSpan.Offset).Append(Tail);
					}
				}

				CollectTrivia(*File);
				return File;
			}

			FDstExprPtr ParseStandaloneExpression()
			{
				FDstExprPtr Expr = ParseExpression();
				if (!Expr)
				{
					return nullptr;
				}
				if (!AtEnd())
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("the end of the expression")));
					}
					return nullptr;
				}
				return Expr;
			}

		private:
			// ------------------------------------------------------------------------------------
			// Token classification. Static members rather than free functions: a member found by
			// ordinary lookup turns argument-dependent lookup off, so a unity build cannot pair these
			// calls with a same-named function of another file in UE::DreamStateTree::Lang.
			// ------------------------------------------------------------------------------------

			/** A word written in backticks is a name, never a word the parser reads by position. */
			static bool IsWord(const FDstToken& Token, const TCHAR* Word)
			{
				return Token.Is(EDstTokenKind::Identifier) && !Token.bQuoted && Token.Text.Equals(Word, ESearchCase::CaseSensitive);
			}

			static bool IsDirective(const FDstToken& Token, const TCHAR* Word)
			{
				return Token.Is(EDstTokenKind::Directive) && Token.Text.Equals(Word, ESearchCase::CaseSensitive);
			}

			static bool IsRegionDirective(const FDstToken& Token)
			{
				return IsDirective(Token, TEXT("Region")) || IsDirective(Token, TEXT("EndRegion"));
			}

			static bool IsKnownDirective(const FDstToken& Token)
			{
				return IsDirective(Token, TEXT("include")) || IsRegionDirective(Token);
			}

			static EDstDocumentKind GetDocumentKind(const FString& Name)
			{
				if (Name.Equals(TEXT("StateTree"), ESearchCase::CaseSensitive))
				{
					return EDstDocumentKind::StateTree;
				}
				if (Name.Equals(TEXT("StateTreeLibrary"), ESearchCase::CaseSensitive))
				{
					return EDstDocumentKind::StateTreeLibrary;
				}
				return EDstDocumentKind::Unknown;
			}

			static bool IsOpeningBracket(const EDstTokenKind Kind)
			{
				return Kind == EDstTokenKind::LeftParen || Kind == EDstTokenKind::LeftBracket || Kind == EDstTokenKind::LeftBrace;
			}

			static bool IsClosingBracket(const EDstTokenKind Kind)
			{
				return Kind == EDstTokenKind::RightParen || Kind == EDstTokenKind::RightBracket || Kind == EDstTokenKind::RightBrace;
			}

			/** Whether a token can be the first of an expression (`not` and `ref` are identifiers, so they count). */
			static bool CanStartExpression(const FDstToken& Token)
			{
				switch (Token.Kind)
				{
				case EDstTokenKind::Number:
				case EDstTokenKind::String:
				case EDstTokenKind::Identifier:
				case EDstTokenKind::LeftParen:
				case EDstTokenKind::LeftBracket:
				case EDstTokenKind::Bang:
				case EDstTokenKind::Minus:
					return true;
				default:
					return false;
				}
			}

			/** Whether a token can be the first of what follows `not`, `-` or `!`. */
			static bool CanStartOperand(const FDstToken& Token)
			{
				return CanStartExpression(Token);
			}

			/**
			 * Higher binds tighter, as GetDstBinaryOpPrecedence documents. `and` / `or` are words and `&&` / `||` the
			 * spellings DST2017 corrects; both read as the same operator so the parse can go on.
			 */
			static bool GetBinaryOperator(const FDstToken& Token, EDstBinaryOp& OutOp)
			{
				if (Token.Is(EDstTokenKind::Identifier) && !Token.bQuoted)
				{
					if (Token.Text.Equals(TEXT("or"), ESearchCase::CaseSensitive))
					{
						OutOp = EDstBinaryOp::Or;
						return true;
					}
					if (Token.Text.Equals(TEXT("and"), ESearchCase::CaseSensitive))
					{
						OutOp = EDstBinaryOp::And;
						return true;
					}
					return false;
				}
				switch (Token.Kind)
				{
				case EDstTokenKind::PipePipe: OutOp = EDstBinaryOp::Or; return true;
				case EDstTokenKind::AmpersandAmpersand: OutOp = EDstBinaryOp::And; return true;
				case EDstTokenKind::EqualEqual: OutOp = EDstBinaryOp::Equal; return true;
				case EDstTokenKind::BangEqual: OutOp = EDstBinaryOp::NotEqual; return true;
				case EDstTokenKind::Less: OutOp = EDstBinaryOp::Less; return true;
				case EDstTokenKind::LessEqual: OutOp = EDstBinaryOp::LessEqual; return true;
				case EDstTokenKind::Greater: OutOp = EDstBinaryOp::Greater; return true;
				case EDstTokenKind::GreaterEqual: OutOp = EDstBinaryOp::GreaterEqual; return true;
				case EDstTokenKind::Plus: OutOp = EDstBinaryOp::Add; return true;
				case EDstTokenKind::Minus: OutOp = EDstBinaryOp::Subtract; return true;
				case EDstTokenKind::Star: OutOp = EDstBinaryOp::Multiply; return true;
				case EDstTokenKind::Slash: OutOp = EDstBinaryOp::Divide; return true;
				default: return false;
				}
			}

			// ------------------------------------------------------------------------------------
			// Cursor
			// ------------------------------------------------------------------------------------

			const FDstToken& Peek(const int32 Ahead) const
			{
				return Tokens[FMath::Min(Index + Ahead, Tokens.Num() - 1)];
			}

			const FDstToken& Current() const
			{
				return Tokens[Index];
			}

			/** The last consumed token; the first token when nothing was consumed yet. */
			const FDstToken& Previous() const
			{
				return Tokens[FMath::Max(Index - 1, 0)];
			}

			bool AtEnd() const
			{
				return Current().Is(EDstTokenKind::EndOfFile);
			}

			bool Check(const EDstTokenKind Kind) const
			{
				return Current().Is(Kind);
			}

			/** Consumes the current token. Never moves past the end of the file. */
			const FDstToken& Advance()
			{
				const int32 Consumed = Index;
				if (Consumed < Tokens.Num() - 1)
				{
					SweepCommentsBefore(Consumed);
					++Index;
				}
				return Tokens[Consumed];
			}

			bool Match(const EDstTokenKind Kind)
			{
				if (!Check(Kind))
				{
					return false;
				}
				Advance();
				return true;
			}

			/** From the token at StartIndex to the last consumed one. */
			FDstSpan SpanFrom(const int32 StartIndex) const
			{
				const FDstSpan& StartSpan = Tokens[StartIndex].Span;
				if (Index <= StartIndex)
				{
					return StartSpan;
				}
				return FDstSpan::Join(StartSpan, Previous().Span);
			}

			// ------------------------------------------------------------------------------------
			// Where a clause, a state or the end of a body begins
			// ------------------------------------------------------------------------------------

			/**
			 * Whether the token Ahead of the cursor begins the next item of a state body, or ends it: `->`, an
			 * unquoted clause word, `State` / `Subtree` followed by a name (or by a `{`, a name that is missing), a
			 * directive, `}` or the end of the file. This is the whole definition of "where a clause ends".
			 */
			bool StartsItemAt(const int32 Ahead) const
			{
				const FDstToken& Token = Peek(Ahead);
				switch (Token.Kind)
				{
				case EDstTokenKind::EndOfFile:
				case EDstTokenKind::RightBrace:
				case EDstTokenKind::Arrow:
				case EDstTokenKind::Directive:
					return true;
				case EDstTokenKind::Identifier:
					break;
				default:
					return false;
				}

				if (Token.bQuoted)
				{
					return false;
				}
				if (IsClauseWordText(Token.Text))
				{
					return true;
				}
				if (Token.Text.Equals(TEXT("State"), ESearchCase::CaseSensitive) || Token.Text.Equals(TEXT("Subtree"), ESearchCase::CaseSensitive))
				{
					const FDstToken& Next = Peek(Ahead + 1);
					return Next.Is(EDstTokenKind::Identifier) || Next.Is(EDstTokenKind::String) || Next.Is(EDstTokenKind::LeftBrace);
				}
				return false;
			}

			/** The same, for the token after a clause keyword when the clause needs an expression there. */
			bool ExpressionStartsHere() const
			{
				return CanStartExpression(Current()) && !StartsItemAt(0);
			}

			// ------------------------------------------------------------------------------------
			// Diagnostics
			// ------------------------------------------------------------------------------------

			/** `'Priority'`, `the string "abc"`, `the end of the file`. */
			FString Describe(const FDstToken& Token) const
			{
				if (Token.Is(EDstTokenKind::EndOfFile))
				{
					return TEXT("the end of the file");
				}
				FString Text = Source.Slice(Token.Span);
				if (Text.Len() > 40)
				{
					Text = Text.Left(37) + TEXT("...");
				}
				if (Token.Is(EDstTokenKind::String))
				{
					return FString::Printf(TEXT("the string %s"), *Text);
				}
				return FString::Printf(TEXT("'%s'"), *Text);
			}

			/** "Expected <What>, found <current token>." */
			FString Expected(const FString& What) const
			{
				return FString::Printf(TEXT("Expected %s, found %s."), *What, *Describe(Current()));
			}

			/** A name in backticks whose closing backtick is missing: it ran to the end of its line (the lexer reported DST1007). */
			bool IsUnterminatedName(const FDstToken& Token) const
			{
				if (!Token.Is(EDstTokenKind::Identifier) || !Token.bQuoted)
				{
					return false;
				}
				const FString Written = Source.Slice(Token.Span);
				return Written.Len() < 2 || Written[Written.Len() - 1] != TEXT('`');
			}

			bool IsUnterminatedString(const FDstToken& Token) const
			{
				if (!Token.Is(EDstTokenKind::String))
				{
					return false;
				}
				const FString Written = Source.Slice(Token.Span);
				if (Written.Len() < 2 || Written[Written.Len() - 1] != TEXT('"'))
				{
					return true;
				}
				// A final quote behind an odd run of backslashes is escaped and closes nothing.
				int32 Backslashes = 0;
				for (int32 Position = Written.Len() - 2; Position >= 1 && Written[Position] == TEXT('\\'); --Position)
				{
					++Backslashes;
				}
				return (Backslashes % 2) == 1;
			}

			/**
			 * Whether an error about the current token says anything new. False for a character the lexer
			 * already reported, right after an unclosed string or name, and for every end-of-file error after the
			 * first; the first one, while a bracket is open, becomes DST2024 on that bracket.
			 */
			bool ShouldReportAtCurrent()
			{
				const FDstToken& Token = Current();
				if (Token.Is(EDstTokenKind::Unknown))
				{
					return false;
				}
				if (Index > 0 && (IsUnterminatedString(Previous()) || IsUnterminatedName(Previous())))
				{
					return false;
				}
				if (Token.Is(EDstTokenKind::EndOfFile))
				{
					if (bEndReported)
					{
						return false;
					}
					if (OpenBrackets.Num() > 0)
					{
						ReportUnclosedAtEnd();
						return false;
					}
					bEndReported = true;
				}
				return true;
			}

			/** DST2024 on the innermost open bracket, once per file. */
			void ReportUnclosedAtEnd()
			{
				if (bEndReported || OpenBrackets.Num() == 0)
				{
					return;
				}
				bEndReported = true;
				const FOpenBracket& Innermost = OpenBrackets.Last();
				Sink.Error(TEXT("DST2024"), Innermost.Span,
					FString::Printf(TEXT("This '%s' is not closed before the end of the file."), Innermost.Open),
					FString::Printf(TEXT("Add the matching '%s'."), Innermost.Close));
			}

			/** Keeps OpenBrackets in step with the brackets being parsed, whichever way a parse function returns. */
			struct FBracketScope
			{
				FDstParser& Owner;

				FBracketScope(FDstParser& InOwner, const FDstToken& OpenToken, const TCHAR* OpenSpelling, const TCHAR* CloseSpelling)
					: Owner(InOwner)
				{
					FOpenBracket& Bracket = Owner.OpenBrackets.AddDefaulted_GetRef();
					Bracket.Span = OpenToken.Span;
					Bracket.Open = OpenSpelling;
					Bracket.Close = CloseSpelling;
				}

				~FBracketScope()
				{
					Owner.OpenBrackets.Pop();
				}

				FBracketScope(const FBracketScope&) = delete;
				FBracketScope& operator=(const FBracketScope&) = delete;
			};

			/**
			 * Marks the expressions of one clause. While it lives, an unquoted clause word met where an operand is
			 * expected at the clause's own bracket depth is the next clause, not a name; and, for a transition, the call
			 * at the end of the clause has the bracket choice described in CallTakesBracket.
			 */
			struct FClauseScope
			{
				FDstParser& Owner;
				const int32 SavedBaseDepth;
				const bool bSavedTransition;

				FClauseScope(FDstParser& InOwner, const bool bTransition)
					: Owner(InOwner)
					, SavedBaseDepth(InOwner.ClauseBaseDepth)
					, bSavedTransition(InOwner.bInTransition)
				{
					Owner.ClauseBaseDepth = Owner.OpenBrackets.Num();
					Owner.bInTransition = bTransition;
				}

				~FClauseScope()
				{
					Owner.ClauseBaseDepth = SavedBaseDepth;
					Owner.bInTransition = bSavedTransition;
				}

				FClauseScope(const FClauseScope&) = delete;
				FClauseScope& operator=(const FClauseScope&) = delete;
			};

			/** Counts how deep expressions and states nest, so a hostile file cannot overflow the stack. */
			struct FNestScope
			{
				FDstParser& Owner;
				int32& Counter;

				FNestScope(FDstParser& InOwner, int32& InCounter)
					: Owner(InOwner)
					, Counter(InCounter)
				{
					++Counter;
				}

				~FNestScope()
				{
					--Counter;
				}

				FNestScope(const FNestScope&) = delete;
				FNestScope& operator=(const FNestScope&) = delete;
			};

			// ------------------------------------------------------------------------------------
			// Recovery
			// ------------------------------------------------------------------------------------

			/**
			 * After a failed section entry: skip past the next `;` at this depth, or stop before the `}` that closes
			 * the section.
			 */
			void SkipToEntryBoundary()
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const EDstTokenKind Kind = Current().Kind;
					if (Kind == EDstTokenKind::Semicolon && Depth == 0)
					{
						Advance();
						return;
					}
					if (Kind == EDstTokenKind::RightBrace && Depth == 0)
					{
						return;
					}
					if (IsOpeningBracket(Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
			}

			/**
			 * After a failed item of a state body: skip, keeping brackets balanced, to the next clause keyword, `->`,
			 * `State`, `Subtree`, directive or the `}` that closes the body.
			 */
			void SkipToItemBoundary()
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const FDstToken& Token = Current();
					if (Depth == 0 && StartsItemAt(0))
					{
						return;
					}
					if (IsOpeningBracket(Token.Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Token.Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
			}

			/** Whether the token Ahead of the cursor begins a member of the document (a section, Template, State, Subtree, directive). */
			bool StartsMemberAt(const int32 Ahead) const
			{
				const FDstToken& Token = Peek(Ahead);
				if (Token.Is(EDstTokenKind::Directive))
				{
					return true;
				}
				if (!Token.Is(EDstTokenKind::Identifier))
				{
					return false;
				}
				if (Peek(Ahead + 1).Is(EDstTokenKind::Assign))
				{
					return true;
				}
				if (IsWord(Token, TEXT("Template")) || IsWord(Token, TEXT("State")) || IsWord(Token, TEXT("Subtree")))
				{
					const FDstToken& Next = Peek(Ahead + 1);
					return Next.Is(EDstTokenKind::Identifier) || Next.Is(EDstTokenKind::LeftBrace);
				}
				return false;
			}

			/** After a failed member of the document: skip, keeping brackets balanced, to the next member or the `}` of the document. */
			void SkipToMemberBoundary()
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const FDstToken& Token = Current();
					if (Depth == 0 && (Token.Is(EDstTokenKind::RightBrace) || StartsMemberAt(0)))
					{
						return;
					}
					if (IsOpeningBracket(Token.Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Token.Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
			}

			/**
			 * After a broken argument or list item: skip past the next `,` of this list, or stop at its closer. False
			 * when a `;`, a `}`, a directive, a state card or the end of the file comes first -- the list was never
			 * closed and its owner gives up. bStopAtLineStartBrace serves the document header, whose body brace on its
			 * own line is a better place to resume than anything after it.
			 */
			bool SkipToListBoundary(const EDstTokenKind Closer, const bool bStopAtLineStartBrace)
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const FDstToken& Token = Current();
					if (Depth == 0)
					{
						if (Token.Is(EDstTokenKind::Comma))
						{
							Advance();
							return true;
						}
						if (Token.Is(Closer))
						{
							return true;
						}
						if (Token.Is(EDstTokenKind::Semicolon) || Token.Is(EDstTokenKind::RightBrace) || Token.Is(EDstTokenKind::Directive))
						{
							return false;
						}
						if (IsWord(Token, TEXT("State")) || IsWord(Token, TEXT("Subtree")))
						{
							if (StartsItemAt(0))
							{
								return false;
							}
						}
						if (bStopAtLineStartBrace && Token.Is(EDstTokenKind::LeftBrace) && Token.bAtLineStart)
						{
							return false;
						}
					}
					if (IsOpeningBracket(Token.Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Token.Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
				return false;
			}

			/** Inside metadata, where `;` separates entries: stop at the `]` of this depth; false at a `}` or the end of the file. */
			bool SkipToMetaEnd()
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const EDstTokenKind Kind = Current().Kind;
					if (Depth == 0 && Kind == EDstTokenKind::RightBracket)
					{
						return true;
					}
					if (Depth == 0 && Kind == EDstTokenKind::RightBrace)
					{
						return false;
					}
					if (IsOpeningBracket(Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
				return false;
			}

			/** At a `(`: through the matching `)`, or to the end of the file. */
			void SkipBalancedParens()
			{
				int32 Depth = 0;
				do
				{
					const EDstTokenKind Kind = Advance().Kind;
					if (Kind == EDstTokenKind::LeftParen)
					{
						++Depth;
					}
					else if (Kind == EDstTokenKind::RightParen)
					{
						--Depth;
					}
				}
				while (Depth > 0 && !AtEnd());
			}

			/** An unknown directive was reported by the lexer; drop it and whatever else is on its line. */
			void SkipUnknownDirective()
			{
				const int32 Line = Current().Span.Line;
				Advance();
				while (!AtEnd() && Current().Span.Line == Line && !Check(EDstTokenKind::LeftBrace) && !Check(EDstTokenKind::RightBrace))
				{
					Advance();
				}
			}

			/** Junk at file level: skip, keeping brackets balanced, to the next line that can start an include or a document. */
			void SkipFileLevelJunk()
			{
				int32 Depth = 0;
				do
				{
					const EDstTokenKind Kind = Current().Kind;
					if (IsOpeningBracket(Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
				while (!AtEnd() && !(Depth == 0
					&& (Check(EDstTokenKind::Directive) || (Current().bAtLineStart && IsDocumentStart(true)))));
			}

			/** The kind word of a document. Before the document any `Name(` counts (DST2004 names it); after it, only the real kinds do. */
			bool IsDocumentStart(const bool bAcceptAnyName) const
			{
				const FDstToken& Token = Current();
				if (!Token.Is(EDstTokenKind::Identifier) || Token.bQuoted)
				{
					return false;
				}
				if (GetDocumentKind(Token.Text) != EDstDocumentKind::Unknown)
				{
					return true;
				}
				return bAcceptAnyName && Peek(1).Is(EDstTokenKind::LeftParen);
			}

			// ------------------------------------------------------------------------------------
			// Trivia bookkeeping
			// ------------------------------------------------------------------------------------

			void ClaimCommentsBefore(const int32 TokenIndex, TArray<int32>& Out)
			{
				const int32 End = CommentsBefore[TokenIndex];
				for (; CommentCursor < End; ++CommentCursor)
				{
					Out.Add(CommentCursor);
				}
			}

			/** Comments the cursor walks past belong to the innermost child being parsed. */
			void SweepCommentsBefore(const int32 TokenIndex)
			{
				const int32 End = CommentsBefore[TokenIndex];
				if (CommentCursor >= End)
				{
					return;
				}
				if (Frames.Num() > 0)
				{
					ClaimCommentsBefore(TokenIndex, Frames.Last());
				}
				else
				{
					// Only junk outside the document gets here; its comments go with it.
					CommentCursor = End;
				}
			}

			/** Empty lines above a token or comment, from the token or comment before it. */
			int32 BlankLinesAbove(const FDstToken& Token, const bool bFirstInFile) const
			{
				return bFirstInFile ? Token.NewlinesBefore : FMath::Max(0, Token.NewlinesBefore - 1);
			}

			/** Before a child is parsed: its Leading comments and the blank lines above them. */
			FAnchorStart BeginAnchor(TArray<int32>& Carry)
			{
				FAnchorStart Start;
				Frames.AddDefaulted();
				if (!bKeepTrivia)
				{
					Carry.Reset();
					return Start;
				}

				Start.Leading = MoveTemp(Carry);
				Carry.Reset();
				ClaimCommentsBefore(Index, Start.Leading);

				if (Start.Leading.Num() > 0)
				{
					const int32 FirstComment = Start.Leading[0];
					// The first comment of the file has nothing above it but the start of the file.
					const bool bFirstInFile = FirstComment == 0 && CommentsBefore[0] > 0;
					Start.BlankLinesBefore = BlankLinesAbove(Comments[FirstComment], bFirstInFile);
				}
				else
				{
					const bool bFirstInFile = Index == 0 && CommentsBefore[0] == 0;
					Start.BlankLinesBefore = BlankLinesAbove(Current(), bFirstInFile);
				}
				return Start;
			}

			/**
			 * After a child and its separator. A child that failed passes null: its comments are carried to
			 * the next sibling (or the container's Inner) instead of being lost.
			 */
			void EndAnchor(FAnchorStart&& Start, const FDstNode* Node, TArray<int32>& Carry)
			{
				TArray<int32> Inside = Frames.Pop();
				if (!bKeepTrivia)
				{
					return;
				}

				if (!Node)
				{
					Carry = MoveTemp(Start.Leading);
					Carry.Append(Inside);
					return;
				}

				FPendingTrivia Pending;
				Pending.Leading = MoveTemp(Start.Leading);
				Pending.Leading.Append(Inside);
				Pending.BlankLinesBefore = Start.BlankLinesBefore;

				if (CommentCursor < CommentsBefore[Index])
				{
					int32 LastLine = 1;
					int32 LastColumn = 1;
					Source.GetLineAndColumn(FMath::Max(Node->Span.Offset, Node->Span.End() - 1), LastLine, LastColumn);
					if (Comments[CommentCursor].Span.Line == LastLine)
					{
						Pending.Trailing = CommentCursor++;
					}
				}

				// The document's header comments were recorded before the document itself ended.
				if (FPendingTrivia* Existing = PendingAnchors.Find(Node->Span.Offset))
				{
					Pending.Inner = MoveTemp(Existing->Inner);
				}

				if (!Pending.IsEmpty())
				{
					PendingAnchors.Add(Node->Span.Offset, MoveTemp(Pending));
				}
			}

			/** At a container's closing bracket: what is left before it is the container's Inner. */
			void ClaimInner(const int32 OpenOffset, TArray<int32>& Carry)
			{
				if (!bKeepTrivia)
				{
					Carry.Reset();
					return;
				}
				TArray<int32> Inner = MoveTemp(Carry);
				Carry.Reset();
				ClaimCommentsBefore(Index, Inner);
				if (Inner.Num() > 0)
				{
					PendingInner.FindOrAdd(OpenOffset).Append(Inner);
				}
			}

			/** A container that never closed: its carried comments go to whatever encloses it. */
			void ReleaseCarry(TArray<int32>& Carry)
			{
				if (Carry.Num() > 0 && Frames.Num() > 0)
				{
					Frames.Last().Append(Carry);
				}
				Carry.Reset();
			}

			/**
			 * The one item of a parenthesised expression was recorded as a tuple item; a ParenExpr has
			 * neither items nor Inner, so its comments go to the enclosing child.
			 */
			void ReleaseTrivia(const int32 AnchorOffset, const int32 OpenOffset)
			{
				if (!bKeepTrivia)
				{
					return;
				}
				TArray<int32> Released;
				FPendingTrivia Pending;
				if (PendingAnchors.RemoveAndCopyValue(AnchorOffset, Pending))
				{
					Released.Append(Pending.Leading);
					if (Pending.Trailing != INDEX_NONE)
					{
						Released.Add(Pending.Trailing);
					}
					Released.Append(Pending.Inner);
				}
				TArray<int32> Inner;
				if (PendingInner.RemoveAndCopyValue(OpenOffset, Inner))
				{
					Released.Append(Inner);
				}
				if (Released.Num() > 0 && Frames.Num() > 0)
				{
					Released.Sort();
					Frames.Last().Append(Released);
				}
			}

			// ------------------------------------------------------------------------------------
			// File level
			// ------------------------------------------------------------------------------------

			TUniquePtr<FDstInclude> ParseInclude()
			{
				const int32 Start = Index;
				Advance();

				if (!Check(EDstTokenKind::String))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2007"), Current().Span, Expected(TEXT("the path string after '#include'")),
							TEXT("Write #include \"Library.dth\"."));
					}
					return nullptr;
				}

				const FDstToken& PathToken = Advance();
				TUniquePtr<FDstInclude> Include = MakeUnique<FDstInclude>();
				Include->Path = PathToken.Text;
				Include->PathSpan = PathToken.Span;
				Include->Span = SpanFrom(Start);
				return Include;
			}

			TUniquePtr<FDstDocument> ParseDocument(bool& bOutFailed)
			{
				const int32 Start = Index;
				bOutFailed = false;

				TUniquePtr<FDstDocument> Document = MakeUnique<FDstDocument>();
				const FDstToken& KindToken = Advance();
				Document->KindName = KindToken.Text;
				Document->KindSpan = KindToken.Span;
				Document->DocumentKind = GetDocumentKind(KindToken.Text);

				if (Document->DocumentKind == EDstDocumentKind::Unknown)
				{
					Sink.Error(TEXT("DST2004"), KindToken.Span,
						FString::Printf(TEXT("Unknown document kind '%s'; expected StateTree or StateTreeLibrary."), *KindToken.Text));
				}

				if (Check(EDstTokenKind::LeftParen))
				{
					const int32 HeaderOpenOffset = Current().Span.Offset;
					bool bTrailingComma = false;
					if (ParseArgumentList(Document->HeaderArgs, bTrailingComma, true))
					{
						CheckDocumentHeader(*Document);
					}
					else
					{
						bOutFailed = true;
					}
					// The header is not a call node, so the comments before its `)` that no argument claimed join the
					// document's leading comments.
					TArray<int32> HeaderInner;
					if (bKeepTrivia && PendingInner.RemoveAndCopyValue(HeaderOpenOffset, HeaderInner) && Frames.Num() > 0)
					{
						Frames.Last().Append(HeaderInner);
					}
				}
				else
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span,
							Expected(FString::Printf(TEXT("'(' and the header after '%s'"), *KindToken.Text)),
							TEXT("Write the header as StateTree(Name=\"AI/ST_Brawler\", Schema=AIComponent)."));
					}
					bOutFailed = true;
				}

				if (Check(EDstTokenKind::LeftBrace))
				{
					ParseDocumentBody(*Document);
				}
				else
				{
					if (!bOutFailed && ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("'{' to open the body of the document")));
					}
					bOutFailed = true;
				}

				Document->Span = SpanFrom(Start);
				return Document;
			}

			/** Syntax only: which arguments the header needs, and what they hold, is checked when the tree is built. */
			void CheckDocumentHeader(const FDstDocument& Document)
			{
				for (const FDstArgumentPtr& Argument : Document.HeaderArgs)
				{
					if (!Argument->IsNamed())
					{
						Sink.Error(TEXT("DST2005"), Argument->Span,
							TEXT("The document header takes only named arguments: Name = \"...\", Root = \"...\", Schema = ... ."),
							TEXT("Write each argument as Key = Value."));
					}
				}
			}

			/** At the `{` of the document: sections, templates, states, subtrees and regions until the `}`. */
			void ParseDocumentBody(FDstDocument& Document)
			{
				const FDstToken& Open = Advance();
				Document.OpenSpan = Open.Span;
				FBracketScope Scope(*this, Open, TEXT("{"), TEXT("}"));

				TArray<int32> Carry;
				TArray<const FDstRegionStmt*> OpenRegions;

				while (!Check(EDstTokenKind::RightBrace) && !AtEnd())
				{
					const int32 StatementStart = Index;
					FAnchorStart Anchor = BeginAnchor(Carry);

					FDstStmtPtr Member;
					if (ParseMember(Member) == EStmtResult::Failed)
					{
						SkipToMemberBoundary();
					}

					EndAnchor(MoveTemp(Anchor), Member.Get(), Carry);

					if (Member)
					{
						NoteRegion(*Member, OpenRegions);
						Document.Members.Add(MoveTemp(Member));
					}

					if (Index == StatementStart && !AtEnd() && !Check(EDstTokenKind::RightBrace))
					{
						Advance();
					}
				}

				WarnUnclosedRegions(OpenRegions);

				if (Check(EDstTokenKind::RightBrace))
				{
					Document.CloseSpan = Current().Span;
					ClaimInner(Open.Span.Offset, Carry);
					Advance();
				}
				else
				{
					ReportUnclosedAtEnd();
					ReleaseCarry(Carry);
				}
			}

			/** Pairs `#EndRegion` with the `#Region` before it in the same container; one without a partner is a warning. */
			void NoteRegion(const FDstStmt& Statement, TArray<const FDstRegionStmt*>& OpenRegions)
			{
				const FDstRegionStmt* Region = Statement.As<FDstRegionStmt>();
				if (!Region)
				{
					return;
				}
				if (!Region->bEnd)
				{
					OpenRegions.Add(Region);
				}
				else if (OpenRegions.Num() > 0)
				{
					OpenRegions.Pop();
				}
				else
				{
					Sink.Warning(TEXT("DST2008"), Region->Span, TEXT("'#EndRegion' has no matching '#Region' in this block."));
				}
			}

			void WarnUnclosedRegions(const TArray<const FDstRegionStmt*>& OpenRegions)
			{
				for (const FDstRegionStmt* Region : OpenRegions)
				{
					Sink.Warning(TEXT("DST2008"), Region->Span,
						FString::Printf(TEXT("'#Region \"%s\"' has no matching '#EndRegion' in this block."), *Region->Label));
				}
			}

			/** One member of the document. */
			EStmtResult ParseMember(FDstStmtPtr& Out)
			{
				const FDstToken& First = Current();

				if (First.Is(EDstTokenKind::Directive))
				{
					return ParseDirectiveStatement(Out);
				}
				if (First.Is(EDstTokenKind::Identifier) && Peek(1).Is(EDstTokenKind::Assign))
				{
					return ParseSection(Out);
				}
				if (IsWord(First, TEXT("Template")))
				{
					return ParseTemplate(Out);
				}
				if (IsWord(First, TEXT("State")) || IsWord(First, TEXT("Subtree")))
				{
					return ParseState(Out, true);
				}

				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DST2011"), First.Span,
						Expected(TEXT("a section ('Name = { ... }'), 'Template', 'State', 'Subtree' or '#Region'")),
						TEXT("The document holds sections, templates, states and subtrees; clauses belong inside a State."));
				}
				return EStmtResult::Failed;
			}

			EStmtResult ParseDirectiveStatement(FDstStmtPtr& Out)
			{
				const FDstToken& Directive = Current();

				if (IsRegionDirective(Directive))
				{
					const int32 Start = Index;
					Advance();
					TUniquePtr<FDstRegionStmt> Region = MakeUnique<FDstRegionStmt>();
					Region->bEnd = IsDirective(Directive, TEXT("EndRegion"));
					if (!Region->bEnd)
					{
						// The label must be on the directive's line; a string on the next line belongs to what follows.
						if (Check(EDstTokenKind::String) && Current().Span.Line == Directive.Span.Line)
						{
							Region->Label = Advance().Text;
						}
						else if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("a label string after '#Region'")),
								TEXT("Write #Region \"Label\"."));
						}
					}
					Region->Span = SpanFrom(Start);
					Out = MoveTemp(Region);
					return EStmtResult::Parsed;
				}

				if (IsDirective(Directive, TEXT("include")))
				{
					TUniquePtr<FDstInclude> Include = ParseInclude();
					if (Include)
					{
						Sink.Error(TEXT("DST2006"), Include->Span, TEXT("'#include' must come before the document, not inside it."),
							TEXT("Move this line to the top of the file."));
					}
					return EStmtResult::Skipped;
				}

				SkipUnknownDirective();
				return EStmtResult::Skipped;
			}

			/**
			 * A statement that parsed but lacks its `;`. The statement is kept; when the next token begins
			 * a new line it most likely begins the next statement, so parsing resumes there. bEntry: a section entry
			 * (DST2010); otherwise a member of the document (DST2001).
			 */
			void ReportMissingSemicolon(const bool bEntry, const FString& What, const FString& Hint = FString())
			{
				if (ShouldReportAtCurrent())
				{
					if (bEntry)
					{
						Sink.Error(TEXT("DST2010"), Current().Span, Expected(What), Hint);
					}
					else
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(What), Hint);
					}
				}
				if (AtEnd() || Check(EDstTokenKind::RightBrace) || Current().Span.Line > Previous().Span.Line)
				{
					return;
				}
				if (bEntry)
				{
					SkipToEntryBoundary();
				}
				else
				{
					SkipToMemberBoundary();
				}
			}

			// ------------------------------------------------------------------------------------
			// Sections
			// ------------------------------------------------------------------------------------

			/** `Name = { entries }`. */
			EStmtResult ParseSection(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& NameToken = Advance();
				Advance(); // =

				TUniquePtr<FDstSectionStmt> Section = MakeUnique<FDstSectionStmt>();
				Section->Name = NameToken.Text;
				Section->NameSpan = NameToken.Span;

				if (!Check(EDstTokenKind::LeftBrace))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span,
							Expected(FString::Printf(TEXT("'{' after '%s ='"), *NameToken.Text)),
							FString::Printf(TEXT("A section is written %s = { entry; entry; }."), *NameToken.Text));
					}
					return EStmtResult::Failed;
				}

				const FDstToken& Open = Advance();
				Section->OpenSpan = Open.Span;
				FBracketScope Scope(*this, Open, TEXT("{"), TEXT("}"));

				TArray<int32> Carry;
				TArray<const FDstRegionStmt*> OpenRegions;

				while (!Check(EDstTokenKind::RightBrace) && !AtEnd())
				{
					const int32 StatementStart = Index;
					FAnchorStart Anchor = BeginAnchor(Carry);

					FDstStmtPtr Entry;
					if (ParseSectionEntry(Entry) == EStmtResult::Failed)
					{
						SkipToEntryBoundary();
					}

					EndAnchor(MoveTemp(Anchor), Entry.Get(), Carry);

					if (Entry)
					{
						NoteRegion(*Entry, OpenRegions);
						Section->Entries.Add(MoveTemp(Entry));
					}

					if (Index == StatementStart && !AtEnd() && !Check(EDstTokenKind::RightBrace))
					{
						Advance();
					}
				}

				WarnUnclosedRegions(OpenRegions);

				if (Check(EDstTokenKind::RightBrace))
				{
					// A section has no node for its brace, so its Inner is keyed by its first token.
					ClaimInner(NameToken.Span.Offset, Carry);
					Section->CloseSpan = Advance().Span;
				}
				else
				{
					ReportUnclosedAtEnd();
					ReleaseCarry(Carry);
				}

				Section->Span = SpanFrom(Start);
				Out = MoveTemp(Section);
				return EStmtResult::Parsed;
			}

			/** One entry of a section: `Key = Value [meta];`, `Type Name [= Value] [meta];` or `Value;`. */
			EStmtResult ParseSectionEntry(FDstStmtPtr& Out)
			{
				const FDstToken& First = Current();

				if (First.Is(EDstTokenKind::Directive))
				{
					return ParseDirectiveStatement(Out);
				}
				if (First.Is(EDstTokenKind::Identifier) && Peek(1).Is(EDstTokenKind::Assign))
				{
					return ParseAssignEntry(Out);
				}
				if (First.Is(EDstTokenKind::Identifier))
				{
					const int32 TypeEnd = ScanTypeEnd(0);
					if (TypeEnd != INDEX_NONE && Peek(TypeEnd).Is(EDstTokenKind::Identifier))
					{
						return ParseDeclEntry(Out);
					}
				}
				if (CanStartExpression(First))
				{
					return ParseExprEntry(Out);
				}

				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DST2009"), First.Span,
						Expected(TEXT("a section entry")),
						TEXT("An entry is written 'Key = Value;', 'Type Name [= Value];' or 'Value;'."));
				}
				return EStmtResult::Failed;
			}

			/** `Key = Value [meta] ;` */
			EStmtResult ParseAssignEntry(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				TUniquePtr<FDstAssignStmt> Assign = MakeUnique<FDstAssignStmt>();

				const FDstToken& Key = Advance();
				Assign->Name = Key.Text;
				Assign->NameSpan = Key.Span;
				Advance(); // =

				Assign->Value = ParseExpression();
				if (!Assign->Value)
				{
					return EStmtResult::Failed;
				}

				if (Check(EDstTokenKind::LeftBracket))
				{
					Assign->Meta = ParseMeta();
					if (!Assign->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				if (!Match(EDstTokenKind::Semicolon))
				{
					ReportMissingSemicolon(true, FString::Printf(TEXT("';' after the entry '%s'"), *Assign->Name));
				}

				Assign->Span = SpanFrom(Start);
				Out = MoveTemp(Assign);
				return EStmtResult::Parsed;
			}

			/** `Type Name [= Value] [meta] ;` */
			EStmtResult ParseDeclEntry(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				TUniquePtr<FDstDeclStmt> Decl = MakeUnique<FDstDeclStmt>();

				// The caller has seen a name after the type, so this cannot fail on the way to it.
				if (!ParseTypeName(Decl->TypeName, Decl->TypeSpan, false))
				{
					return EStmtResult::Failed;
				}
				const FDstToken& Name = Advance();
				Decl->Name = Name.Text;
				Decl->NameSpan = Name.Span;

				if (Match(EDstTokenKind::Assign))
				{
					Decl->Value = ParseExpression();
					if (!Decl->Value)
					{
						return EStmtResult::Failed;
					}
				}

				if (Check(EDstTokenKind::LeftBracket))
				{
					Decl->Meta = ParseMeta();
					if (!Decl->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				if (!Match(EDstTokenKind::Semicolon))
				{
					ReportMissingSemicolon(true, Decl->Value || Decl->Meta
						? FString::Printf(TEXT("';' after the declaration of '%s'"), *Decl->Name)
						: FString::Printf(TEXT("'=' or ';' after the declaration of '%s'"), *Decl->Name));
				}

				Decl->Span = SpanFrom(Start);
				Out = MoveTemp(Decl);
				return EStmtResult::Parsed;
			}

			/** `Value ;` -- a tag prefix. */
			EStmtResult ParseExprEntry(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				FDstExprPtr Expr = ParseExpression();
				if (!Expr)
				{
					return EStmtResult::Failed;
				}

				if (Check(EDstTokenKind::LeftBracket))
				{
					// The statement has no place for settings; they are read so the entry can go on.
					Sink.Error(TEXT("DST2009"), Current().Span, TEXT("A 'Value;' entry takes no [ settings ]."),
						TEXT("Settings follow 'Key = Value' and 'Type Name' entries."));
					if (!ParseMeta())
					{
						return EStmtResult::Failed;
					}
				}

				TUniquePtr<FDstExprStmt> Entry = MakeUnique<FDstExprStmt>();
				Entry->Expr = MoveTemp(Expr);
				if (!Match(EDstTokenKind::Semicolon))
				{
					ReportMissingSemicolon(true, TEXT("';' after the entry"));
				}

				Entry->Span = SpanFrom(Start);
				Out = MoveTemp(Entry);
				return EStmtResult::Parsed;
			}

			// ------------------------------------------------------------------------------------
			// Templates
			// ------------------------------------------------------------------------------------

			/** `Template Name [(params)] = Body ;` */
			EStmtResult ParseTemplate(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				Advance(); // Template

				if (!Check(EDstTokenKind::Identifier))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("a template name after 'Template'")),
							TEXT("Write Template Name(params) = expression;."));
					}
					return EStmtResult::Failed;
				}

				TUniquePtr<FDstTemplateStmt> Template = MakeUnique<FDstTemplateStmt>();
				const FDstToken& NameToken = Advance();
				Template->Name = NameToken.Text;
				Template->NameSpan = NameToken.Span;

				if (Check(EDstTokenKind::LeftParen))
				{
					Template->bHasParams = true;
					if (!ParseParamList(Template->Params, false))
					{
						return EStmtResult::Failed;
					}
				}

				if (!Match(EDstTokenKind::Assign))
				{
					// Raised here rather than when templates are expanded: a template without a body is not kept in the
					// tree, so the later stage would never see it.
					Sink.Error(TEXT("DST3106"), Template->NameSpan,
						FString::Printf(TEXT("Template '%s' has no body; write it as 'Template %s%s = expression;'."),
							*Template->Name, *Template->Name, Template->bHasParams ? TEXT("(...)") : TEXT("")));
					if (Match(EDstTokenKind::Semicolon))
					{
						return EStmtResult::Skipped;
					}
					return EStmtResult::Failed;
				}

				Template->Body = ParseExpression();
				if (!Template->Body)
				{
					return EStmtResult::Failed;
				}

				if (!Match(EDstTokenKind::Semicolon))
				{
					ReportMissingSemicolon(false, FString::Printf(TEXT("';' after the template '%s'"), *Template->Name));
				}

				Template->Span = SpanFrom(Start);
				Out = MoveTemp(Template);
				return EStmtResult::Parsed;
			}

			/**
			 * At the `(` of a template or a subtree: `Param [= Default] { , Param [= Default] }` then `)`. A template
			 * parameter is a bare name; a subtree parameter is `Type Name`.
			 */
			bool ParseParamList(TArray<TUniquePtr<FDstParam>>& OutParams, const bool bTyped)
			{
				const FDstToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("("), TEXT(")"));

				if (Match(EDstTokenKind::RightParen))
				{
					return true;
				}

				bool bSawDefault = false;
				while (true)
				{
					const int32 ParamStart = Index;
					TUniquePtr<FDstParam> Param = MakeUnique<FDstParam>();

					if (bTyped)
					{
						if (!ParseTypeName(Param->TypeName, Param->TypeSpan, false))
						{
							return false;
						}
						if (!Check(EDstTokenKind::Identifier))
						{
							if (ShouldReportAtCurrent())
							{
								Sink.Error(TEXT("DST2001"), Current().Span,
									Expected(FString::Printf(TEXT("the name of the parameter after its type '%s'"), *Param->TypeName)),
									TEXT("A subtree parameter is written 'float Toughness = 1': its type, then its name."));
							}
							return false;
						}
					}
					else if (!Check(EDstTokenKind::Identifier))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("a parameter name")));
						}
						return false;
					}

					const FDstToken& NameToken = Advance();
					Param->Name = NameToken.Text;
					Param->NameSpan = NameToken.Span;

					if (Match(EDstTokenKind::Assign))
					{
						Param->Default = ParseExpression();
						if (!Param->Default)
						{
							return false;
						}
					}
					Param->Span = SpanFrom(ParamStart);

					bool bDuplicate = false;
					for (const TUniquePtr<FDstParam>& Earlier : OutParams)
					{
						if (Earlier->Name.Equals(Param->Name, ESearchCase::CaseSensitive))
						{
							bDuplicate = true;
							break;
						}
					}
					if (bDuplicate)
					{
						Sink.Error(TEXT("DST2014"), Param->NameSpan,
							FString::Printf(TEXT("The parameter '%s' is declared twice."), *Param->Name));
					}
					else if (!Param->Default && bSawDefault)
					{
						Sink.Error(TEXT("DST2014"), Param->NameSpan,
							FString::Printf(TEXT("The parameter '%s' has no default, but a parameter before it has one."), *Param->Name),
							TEXT("Parameters with defaults come last."));
					}
					bSawDefault |= Param->Default.IsValid();

					const FString ParamName = Param->Name;
					OutParams.Add(MoveTemp(Param));

					if (Match(EDstTokenKind::Comma))
					{
						continue;
					}
					if (Match(EDstTokenKind::RightParen))
					{
						return true;
					}
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span,
							Expected(FString::Printf(TEXT("',' or ')' after the parameter '%s'"), *ParamName)));
					}
					return false;
				}
			}

			// ------------------------------------------------------------------------------------
			// Types and metadata
			// ------------------------------------------------------------------------------------

			/**
			 * At Ahead: a type name, with its `<...>` arguments when it is a generic type, as far as a lookahead can
			 * tell. Returns the index of the token after it (as an offset from the cursor), or INDEX_NONE when no type
			 * starts there.
			 */
			int32 ScanTypeEnd(const int32 Ahead, const int32 Depth = 0) const
			{
				if (!Peek(Ahead).Is(EDstTokenKind::Identifier) || Depth > MaxTypeDepth)
				{
					return INDEX_NONE;
				}
				int32 At = Ahead + 1;
				if (Peek(At).Is(EDstTokenKind::Less))
				{
					++At;
					while (true)
					{
						const int32 InnerEnd = ScanTypeEnd(At, Depth + 1);
						if (InnerEnd == INDEX_NONE)
						{
							return INDEX_NONE;
						}
						At = InnerEnd;
						if (!Peek(At).Is(EDstTokenKind::Comma))
						{
							break;
						}
						++At;
					}
					if (!Peek(At).Is(EDstTokenKind::Greater))
					{
						return INDEX_NONE;
					}
					++At;
				}
				return At;
			}

			/**
			 * At an identifier: a type name, with its type arguments when it has them (`Array<float>`, `Class<Actor>`,
			 * nested too); the text keeps them without spaces. A part written in backticks keeps its backticks inside a
			 * generic type, so the text can be printed as it is. False, after reporting, when no type starts here.
			 */
			bool ParseTypeName(FString& OutText, FDstSpan& OutSpan, const bool bNested, const int32 Depth = 0)
			{
				const int32 Start = Index;
				if (!Check(EDstTokenKind::Identifier))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("a type name")));
					}
					return false;
				}

				const FDstToken& Head = Advance();
				const bool bGeneric = Check(EDstTokenKind::Less);
				OutText = (Head.bQuoted && (bNested || bGeneric)) ? FString::Printf(TEXT("`%s`"), *Head.Text) : Head.Text;
				if (!bGeneric)
				{
					OutSpan = Head.Span;
					return true;
				}

				if (Depth >= MaxTypeDepth)
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("a type with fewer type arguments inside each other")));
					}
					return false;
				}

				Advance(); // <
				OutText += TEXT("<");
				while (true)
				{
					FString Inner;
					FDstSpan InnerSpan;
					if (!ParseTypeName(Inner, InnerSpan, true, Depth + 1))
					{
						return false;
					}
					OutText += Inner;
					if (!Match(EDstTokenKind::Comma))
					{
						break;
					}
					OutText += TEXT(",");
				}

				if (!Check(EDstTokenKind::Greater))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(FString::Printf(TEXT("'>' to close the type '%s'"), *OutText)));
					}
					return false;
				}
				Advance(); // >
				OutText += TEXT(">");
				OutSpan = SpanFrom(Start);
				return true;
			}

			/** At a `[`: `Key [= Expr]` entries separated by `;`, a trailing one allowed. Null, after reporting, when the `]` is never found. */
			TUniquePtr<FDstMeta> ParseMeta()
			{
				const int32 Start = Index;
				const FDstToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("["), TEXT("]"));

				TUniquePtr<FDstMeta> Meta = MakeUnique<FDstMeta>();
				while (!Check(EDstTokenKind::RightBracket))
				{
					if (!Check(EDstTokenKind::Identifier))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2018"), Current().Span, Expected(TEXT("a setting name")),
								TEXT("Settings are written as [ Key = Value; Flag ]."));
						}
						// One report per metadata list; the rest of it is skipped.
						if (!SkipToMetaEnd())
						{
							return nullptr;
						}
						break;
					}

					const int32 EntryStart = Index;
					TUniquePtr<FDstMetaEntry> Entry = MakeUnique<FDstMetaEntry>();
					const FDstToken& Key = Advance();
					Entry->Key = Key.Text;
					Entry->KeySpan = Key.Span;

					bool bValueFailed = false;
					if (Match(EDstTokenKind::Assign))
					{
						Entry->Value = ParseExpression();
						bValueFailed = !Entry->Value;
					}
					Entry->Span = SpanFrom(EntryStart);

					if (bValueFailed)
					{
						// The expression reported; resume at the `]` instead of letting the owner's recovery
						// stop at a `;` that separates metadata entries.
						if (!SkipToMetaEnd())
						{
							return nullptr;
						}
						break;
					}

					const FString KeyText = Entry->Key;
					Meta->Entries.Add(MoveTemp(Entry));

					if (Match(EDstTokenKind::Semicolon))
					{
						continue;
					}
					if (Check(EDstTokenKind::RightBracket))
					{
						break;
					}

					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2018"), Current().Span,
							Expected(FString::Printf(TEXT("';' or ']' after the setting '%s'"), *KeyText)),
							Check(EDstTokenKind::Comma) ? FString(TEXT("Settings are separated by ';', as in [ Delay = 1; Reactivate ].")) : FString());
					}
					if (!SkipToMetaEnd())
					{
						return nullptr;
					}
					break;
				}

				Advance(); // ]
				Meta->Span = SpanFrom(Start);
				return Meta;
			}

			// ------------------------------------------------------------------------------------
			// States
			// ------------------------------------------------------------------------------------

			/**
			 * At `State` or `Subtree`: `State Name [meta] { body }` or `Subtree Name(params) [meta] { body }`. A State
			 * with parameters (DST2019) and a Subtree below the document level (DST2020) are reported and then parsed
			 * like the real thing, so what is inside them is still checked; the second is dropped from the tree.
			 */
			EStmtResult ParseState(FDstStmtPtr& Out, const bool bAtDocumentLevel)
			{
				const int32 Start = Index;
				const FDstToken& Word = Advance();

				TUniquePtr<FDstStateStmt> State = MakeUnique<FDstStateStmt>();
				State->bSubtree = Word.Text.Equals(TEXT("Subtree"), ESearchCase::CaseSensitive);
				State->WordSpan = Word.Span;

				bool bDrop = false;
				if (State->bSubtree && !bAtDocumentLevel)
				{
					Sink.Error(TEXT("DST2020"), Word.Span, TEXT("A Subtree can only be written at the top level of the document, not inside a state."),
						TEXT("Move it out of the state and 'link' it where it is used."));
					bDrop = true;
				}

				if (!Check(EDstTokenKind::Identifier))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2022"), Current().Span,
							Expected(FString::Printf(TEXT("the name of the %s after '%s'"), State->bSubtree ? TEXT("subtree") : TEXT("state"), *Word.Text)),
							TEXT("A name is an identifier, or any text in backticks: State `Move To` { ... }."));
					}
					return EStmtResult::Failed;
				}

				const FDstToken& NameToken = Advance();
				State->Name = NameToken.Text;
				State->NameSpan = NameToken.Span;
				State->bNameQuoted = NameToken.bQuoted;

				if (Check(EDstTokenKind::LeftParen))
				{
					if (State->bSubtree)
					{
						State->bHasParams = true;
						if (!ParseParamList(State->Params, true))
						{
							return EStmtResult::Failed;
						}
					}
					else
					{
						Sink.Error(TEXT("DST2019"), Current().Span,
							FString::Printf(TEXT("A State takes no parameters; only a Subtree does ('%s' is a State)."), *State->Name),
							TEXT("Write 'Subtree Name(Type Param = Default) { ... }' at the top level and 'link Name(Param = value)' where it is used."));
						SkipBalancedParens();
					}
				}

				if (Check(EDstTokenKind::LeftBracket))
				{
					State->Meta = ParseMeta();
					if (!State->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				if (!Check(EDstTokenKind::LeftBrace))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span,
							Expected(FString::Printf(TEXT("'{' to open the body of '%s'"), *State->Name)),
							TEXT("A state is a card: State Name { clauses and child states }."));
					}
					return EStmtResult::Failed;
				}

				ParseStateBody(*State);
				State->Span = SpanFrom(Start);

				if (bDrop)
				{
					return EStmtResult::Skipped;
				}
				Out = MoveTemp(State);
				return EStmtResult::Parsed;
			}

			/** At the `{` of a state: clauses, child states and regions until the `}`. */
			void ParseStateBody(FDstStateStmt& State)
			{
				const FDstToken& Open = Advance();
				State.OpenSpan = Open.Span;
				FBracketScope Scope(*this, Open, TEXT("{"), TEXT("}"));

				TArray<int32> Carry;
				TArray<const FDstRegionStmt*> OpenRegions;

				if (StateDepth >= MaxNestingDepth)
				{
					// Reported once; the rest of the body is skipped without parsing it, to keep the stack small.
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Open.Span, TEXT("States are nested too deeply."),
							FString::Printf(TEXT("Nest states at most %d levels deep."), MaxNestingDepth));
					}
					int32 Depth = 1;
					while (!AtEnd() && Depth > 0)
					{
						if (Check(EDstTokenKind::LeftBrace))
						{
							++Depth;
						}
						else if (Check(EDstTokenKind::RightBrace))
						{
							--Depth;
							if (Depth == 0)
							{
								break;
							}
						}
						Advance();
					}
				}
				else
				{
					FNestScope Nest(*this, StateDepth);
					while (!Check(EDstTokenKind::RightBrace) && !AtEnd())
					{
						const int32 StepStart = Index;
						FAnchorStart Anchor = BeginAnchor(Carry);

						FDstStmtPtr Item;
						if (ParseStateItem(Item) == EStmtResult::Failed)
						{
							SkipToItemBoundary();
						}

						EndAnchor(MoveTemp(Anchor), Item.Get(), Carry);

						if (Item)
						{
							NoteRegion(*Item, OpenRegions);
							State.Body.Add(MoveTemp(Item));
						}

						if (Index == StepStart && !AtEnd() && !Check(EDstTokenKind::RightBrace))
						{
							Advance();
						}
					}
				}

				WarnUnclosedRegions(OpenRegions);

				if (Check(EDstTokenKind::RightBrace))
				{
					State.CloseSpan = Current().Span;
					ClaimInner(Open.Span.Offset, Carry);
					Advance();
				}
				else
				{
					ReportUnclosedAtEnd();
					ReleaseCarry(Carry);
				}
			}

			/** One item of a state body: a clause, a child state or a region. */
			EStmtResult ParseStateItem(FDstStmtPtr& Out)
			{
				const FDstToken& First = Current();

				if (First.Is(EDstTokenKind::Directive))
				{
					return ParseDirectiveStatement(Out);
				}
				if (First.Is(EDstTokenKind::Arrow))
				{
					return ParseTransitionClause(Out);
				}
				if (IsWord(First, TEXT("State")) || IsWord(First, TEXT("Subtree")))
				{
					return ParseState(Out, false);
				}
				if (First.Is(EDstTokenKind::Identifier) && !First.bQuoted)
				{
					if (First.Text.Equals(TEXT("on"), ESearchCase::CaseSensitive))
					{
						return ParseOnClause(Out);
					}
					if (First.Text.Equals(TEXT("if"), ESearchCase::CaseSensitive))
					{
						return ParseIfClause(Out);
					}
					if (First.Text.Equals(TEXT("pick"), ESearchCase::CaseSensitive))
					{
						return ParsePickClause(Out);
					}
					if (First.Text.Equals(TEXT("utility"), ESearchCase::CaseSensitive))
					{
						return ParseUtilityClause(Out);
					}
					if (First.Text.Equals(TEXT("link"), ESearchCase::CaseSensitive))
					{
						return ParseLinkClause(Out);
					}
					if (First.Text.Equals(TEXT("do"), ESearchCase::CaseSensitive))
					{
						return ParseDoClause(Out);
					}
				}

				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DST2012"), First.Span,
						Expected(TEXT("a clause (on, if, pick, utility, link, do or ->), 'State' or '}'")),
						First.Is(EDstTokenKind::Identifier)
							? FString(TEXT("Clauses are not separated by anything: each starts with its own keyword. Join conditions with 'and' / 'or' and tasks with ','."))
							: FString());
				}
				return EStmtResult::Failed;
			}

			// ------------------------------------------------------------------------------------
			// Clauses
			// ------------------------------------------------------------------------------------

			/** After a clause keyword that needs an expression: reports DST2001 and returns false when none starts here. */
			bool ExpectExpression(const FString& What, const TCHAR* Hint)
			{
				if (ExpressionStartsHere())
				{
					return true;
				}
				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DST2001"), Current().Span, Expected(What), FString(Hint));
				}
				return false;
			}

			/** `on Event` / `on Event(PayloadType)` */
			EStmtResult ParseOnClause(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& Keyword = Advance();
				TUniquePtr<FDstClauseStmt> Clause = MakeUnique<FDstClauseStmt>();
				Clause->ClauseKind = EDstClauseKind::On;
				Clause->KeywordSpan = Keyword.Span;

				if (!ExpectExpression(TEXT("the event after 'on'"), TEXT("Write the event's tag: on Hit.Stagger, or on Trade(PayloadType).")))
				{
					return EStmtResult::Failed;
				}

				{
					const FClauseScope Scope(*this, false);
					Clause->Value = ParsePostfix();
				}
				if (!Clause->Value)
				{
					return EStmtResult::Failed;
				}

				Clause->Span = SpanFrom(Start);
				Out = MoveTemp(Clause);
				return EStmtResult::Parsed;
			}

			/** `if Condition` */
			EStmtResult ParseIfClause(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& Keyword = Advance();
				TUniquePtr<FDstClauseStmt> Clause = MakeUnique<FDstClauseStmt>();
				Clause->ClauseKind = EDstClauseKind::If;
				Clause->KeywordSpan = Keyword.Span;

				if (!ExpectExpression(TEXT("a condition after 'if'"), TEXT("Write a condition node call, a comparison or a bool path: if Player.Sees and Player.Distance < 800.")))
				{
					return EStmtResult::Failed;
				}

				{
					const FClauseScope Scope(*this, false);
					Clause->Value = ParseExpression();
				}
				if (!Clause->Value)
				{
					return EStmtResult::Failed;
				}

				Clause->Span = SpanFrom(Start);
				Out = MoveTemp(Clause);
				return EStmtResult::Parsed;
			}

			/** `pick Mode` */
			EStmtResult ParsePickClause(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& Keyword = Advance();
				TUniquePtr<FDstClauseStmt> Clause = MakeUnique<FDstClauseStmt>();
				Clause->ClauseKind = EDstClauseKind::Pick;
				Clause->KeywordSpan = Keyword.Span;

				if (!Check(EDstTokenKind::Identifier) || StartsItemAt(0))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("a selection mode after 'pick'")),
							TEXT("The modes are InOrder, Random, UniformRandom, HighestUtility, WeightedUtility, RelativeUtility, FollowTransitions, Enter and None."));
					}
					return EStmtResult::Failed;
				}

				const FDstToken& Mode = Advance();
				TUniquePtr<FDstNameExpr> Name = MakeUnique<FDstNameExpr>();
				Name->Name = Mode.Text;
				Name->bQuoted = Mode.bQuoted;
				Name->Span = Mode.Span;
				Clause->Value = MoveTemp(Name);

				Clause->Span = SpanFrom(Start);
				Out = MoveTemp(Clause);
				return EStmtResult::Parsed;
			}

			/** `utility Expression` */
			EStmtResult ParseUtilityClause(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& Keyword = Advance();
				TUniquePtr<FDstClauseStmt> Clause = MakeUnique<FDstClauseStmt>();
				Clause->ClauseKind = EDstClauseKind::Utility;
				Clause->KeywordSpan = Keyword.Span;

				if (!ExpectExpression(TEXT("a utility expression after 'utility'"), TEXT("Write consideration nodes and constants: utility FloatInput(Player.Distance, (0, 3000)) * 0.5.")))
				{
					return EStmtResult::Failed;
				}

				{
					const FClauseScope Scope(*this, false);
					Clause->Value = ParseExpression();
				}
				if (!Clause->Value)
				{
					return EStmtResult::Failed;
				}

				Clause->Span = SpanFrom(Start);
				Out = MoveTemp(Clause);
				return EStmtResult::Parsed;
			}

			/** `link Subtree(args)` / `link asset Asset(args)` */
			EStmtResult ParseLinkClause(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& Keyword = Advance();
				TUniquePtr<FDstClauseStmt> Clause = MakeUnique<FDstClauseStmt>();
				Clause->ClauseKind = EDstClauseKind::Link;
				Clause->KeywordSpan = Keyword.Span;

				// `asset` is the keyword only when a name follows it; a subtree called `asset` is `link asset` alone
				// or `link asset(...)`.
				if (IsWord(Current(), TEXT("asset")) && Peek(1).Is(EDstTokenKind::Identifier) && !StartsItemAt(1))
				{
					Clause->bLinkAsset = true;
					Clause->AssetSpan = Advance().Span;
				}

				if (!Check(EDstTokenKind::Identifier) || StartsItemAt(0))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2016"), Current().Span,
							Expected(Clause->bLinkAsset ? TEXT("the asset after 'link asset'") : TEXT("the subtree after 'link'")),
							Clause->bLinkAsset
								? TEXT("Write link asset ST_Other(Param = value), or link asset Path(Game, \"AI/ST_Other\").")
								: TEXT("Write link HitReaction(Param = value)."));
					}
					return EStmtResult::Failed;
				}

				{
					const FClauseScope Scope(*this, false);
					Clause->Value = ParsePostfix();
				}
				if (!Clause->Value)
				{
					return EStmtResult::Failed;
				}

				Clause->Span = SpanFrom(Start);
				Out = MoveTemp(Clause);
				return EStmtResult::Parsed;
			}

			/** `do Task, Task` */
			EStmtResult ParseDoClause(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& Keyword = Advance();
				TUniquePtr<FDstClauseStmt> Clause = MakeUnique<FDstClauseStmt>();
				Clause->ClauseKind = EDstClauseKind::Do;
				Clause->KeywordSpan = Keyword.Span;

				const FClauseScope Scope(*this, false);
				TArray<int32> Carry;

				while (true)
				{
					if (!Check(EDstTokenKind::Identifier) || StartsItemAt(0))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2013"), Current().Span,
								Expected(Clause->Tasks.Num() == 0 ? TEXT("a task after 'do'") : TEXT("a task after ','")),
								TEXT("Write do Wait(1), Tell(Fled): a node call, or a node name without parentheses; tasks are separated by ','."));
						}
						ReleaseCarry(Carry);
						if (Clause->Tasks.Num() == 0)
						{
							return EStmtResult::Failed;
						}
						// What was read stays; whatever is here, the body skips it.
						if (!StartsItemAt(0))
						{
							SkipToItemBoundary();
						}
						break;
					}

					FAnchorStart Anchor = BeginAnchor(Carry);
					TUniquePtr<FDstTaskItem> Task = ParseTaskItem();
					const bool bComma = Task.IsValid() && Match(EDstTokenKind::Comma);
					EndAnchor(MoveTemp(Anchor), Task.Get(), Carry);

					if (!Task)
					{
						ReleaseCarry(Carry);
						if (Clause->Tasks.Num() == 0)
						{
							return EStmtResult::Failed;
						}
						SkipToItemBoundary();
						break;
					}
					Clause->Tasks.Add(MoveTemp(Task));

					if (bComma)
					{
						continue;
					}
					if (StartsItemAt(0))
					{
						break;
					}
					if (Check(EDstTokenKind::Identifier))
					{
						// `do Wait(1) Tell(Fled)`: the comma is missing. Said once, then read on as if it were there.
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2013"), Current().Span,
								Expected(TEXT("',' between the tasks of a 'do'")),
								TEXT("Separate tasks with ','; a new clause starts with its keyword (on, if, pick, utility, link, do, ->)."));
						}
						continue;
					}
					break;
				}

				ReleaseCarry(Carry);
				Clause->Span = SpanFrom(Start);
				Out = MoveTemp(Clause);
				return EStmtResult::Parsed;
			}

			/** `[BindName =] Node`, Node being a call (with its settings) or a name or path. Null, after reporting, when it fails. */
			TUniquePtr<FDstTaskItem> ParseTaskItem()
			{
				const int32 Start = Index;
				TUniquePtr<FDstTaskItem> Item = MakeUnique<FDstTaskItem>();

				if (Check(EDstTokenKind::Identifier) && Peek(1).Is(EDstTokenKind::Assign))
				{
					const FDstToken& Bind = Advance();
					Item->BindName = Bind.Text;
					Item->BindNameSpan = Bind.Span;
					Advance(); // =

					if (!Check(EDstTokenKind::Identifier) || StartsItemAt(0))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2013"), Current().Span,
								Expected(FString::Printf(TEXT("the task after '%s ='"), *Item->BindName)),
								TEXT("Write do Combo = ComboAttack()."));
						}
						return nullptr;
					}
				}

				Item->Node = ParsePostfix();
				if (!Item->Node)
				{
					return nullptr;
				}
				Item->Span = SpanFrom(Start);
				return Item;
			}

			/** `-> Target [on Trigger] [if Condition] [ options ]` */
			EStmtResult ParseTransitionClause(FDstStmtPtr& Out)
			{
				const int32 Start = Index;
				const FDstToken& Arrow = Advance();
				TUniquePtr<FDstClauseStmt> Clause = MakeUnique<FDstClauseStmt>();
				Clause->ClauseKind = EDstClauseKind::Transition;
				Clause->KeywordSpan = Arrow.Span;

				if (!Check(EDstTokenKind::Identifier) || StartsItemAt(0))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2015"), Current().Span, Expected(TEXT("the target state after '->'")),
							TEXT("The target is a state name or path (Idle.Phone), Next, NextSelectable, Parent, NextParent, NextSelectableParent, TreeSucceeded or TreeFailed. A state named like a keyword is written in backticks."));
					}
					return EStmtResult::Failed;
				}

				const FClauseScope Scope(*this, true);

				Clause->Value = ParsePostfix();
				if (!Clause->Value)
				{
					return EStmtResult::Failed;
				}

				// `on` and `if` after the target belong to the transition: a clause keyword cannot follow a target any
				// other way, and the formatter writes the state's own `on` and `if` before its transitions.
				if (IsWord(Current(), TEXT("on")))
				{
					Clause->OnSpan = Advance().Span;
					if (!ExpectExpression(TEXT("the trigger after 'on'"), TEXT("The trigger is Succeeded, Failed, Completed, Tick, an event tag, Tag(PayloadType) or Delegate(Path).")))
					{
						return EStmtResult::Failed;
					}
					Clause->Trigger = ParsePostfix();
					if (!Clause->Trigger)
					{
						return EStmtResult::Failed;
					}
				}

				if (IsWord(Current(), TEXT("if")))
				{
					Clause->IfSpan = Advance().Span;
					if (!ExpectExpression(TEXT("a condition after 'if'"), TEXT("Write a condition node call, a comparison or a bool path.")))
					{
						return EStmtResult::Failed;
					}
					Clause->Condition = ParseExpression();
					if (!Clause->Condition)
					{
						return EStmtResult::Failed;
					}
				}

				if (Check(EDstTokenKind::LeftBracket))
				{
					Clause->Meta = ParseMeta();
					if (!Clause->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				Clause->Span = SpanFrom(Start);
				Out = MoveTemp(Clause);
				return EStmtResult::Parsed;
			}

			// ------------------------------------------------------------------------------------
			// Arguments and lists
			// ------------------------------------------------------------------------------------

			/** At a `(`: arguments through `)`. False, after reporting, when the list could not be closed. */
			bool ParseArgumentList(TArray<FDstArgumentPtr>& OutArguments, bool& bOutTrailingComma, const bool bDocumentHeader = false)
			{
				const FDstToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("("), TEXT(")"));
				TArray<int32> Carry;

				while (!Check(EDstTokenKind::RightParen))
				{
					if (AtEnd())
					{
						ReportUnclosedAtEnd();
						ReleaseCarry(Carry);
						return false;
					}

					FAnchorStart Anchor = BeginAnchor(Carry);
					FDstArgumentPtr Argument = ParseArgument();
					bool bRecover = !Argument;
					if (Argument && !Match(EDstTokenKind::Comma) && !Check(EDstTokenKind::RightParen))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("',' or ')' after the argument")));
						}
						bRecover = true;
					}
					EndAnchor(MoveTemp(Anchor), Argument.Get(), Carry);
					if (Argument)
					{
						OutArguments.Add(MoveTemp(Argument));
					}

					if (bRecover && !SkipToListBoundary(EDstTokenKind::RightParen, bDocumentHeader))
					{
						ReleaseCarry(Carry);
						return false;
					}
				}

				bOutTrailingComma = OutArguments.Num() > 0 && Previous().Is(EDstTokenKind::Comma);
				ClaimInner(Open.Span.Offset, Carry);
				Advance(); // )

				CheckArgumentNames(OutArguments, bDocumentHeader);
				return true;
			}

			/** `Value`, `Name = Value` or `Name -> Path`. */
			FDstArgumentPtr ParseArgument()
			{
				const int32 Start = Index;
				FDstArgumentPtr Argument = MakeUnique<FDstArgument>();

				if (Check(EDstTokenKind::Identifier) && (Peek(1).Is(EDstTokenKind::Assign) || Peek(1).Is(EDstTokenKind::Arrow)))
				{
					const FDstToken& Key = Advance();
					Argument->Name = Key.Text;
					Argument->NameSpan = Key.Span;
					Argument->bOutput = Check(EDstTokenKind::Arrow);
					Advance(); // = or ->
				}

				Argument->Value = ParseExpression();
				if (!Argument->Value)
				{
					return nullptr;
				}
				Argument->Span = SpanFrom(Start);
				return Argument;
			}

			/**
			 * DST2021 once per list, DST2023 for every repeated name (case-sensitive, like every name). A document header
			 * takes no positional argument at all, which DST2005 reports, so DST2021 stays out of it.
			 */
			void CheckArgumentNames(const TArray<FDstArgumentPtr>& Arguments, const bool bDocumentHeader)
			{
				bool bSawNamed = false;
				bool bReportedOrder = false;
				for (int32 ArgIndex = 0; ArgIndex < Arguments.Num(); ++ArgIndex)
				{
					const FDstArgument& Argument = *Arguments[ArgIndex];
					if (!Argument.IsNamed())
					{
						if (bSawNamed && !bReportedOrder && !bDocumentHeader)
						{
							Sink.Error(TEXT("DST2021"), Argument.Span, TEXT("A positional argument cannot follow a named one."),
								TEXT("Put the positional arguments first, or name this one."));
							bReportedOrder = true;
						}
						continue;
					}

					bSawNamed = true;
					for (int32 EarlierIndex = 0; EarlierIndex < ArgIndex; ++EarlierIndex)
					{
						const FDstArgument& Earlier = *Arguments[EarlierIndex];
						if (Earlier.IsNamed() && Earlier.Name.Equals(Argument.Name, ESearchCase::CaseSensitive))
						{
							Sink.Error(TEXT("DST2023"), Argument.NameSpan,
								FString::Printf(TEXT("The argument '%s' is given twice."), *Argument.Name));
							break;
						}
					}
				}
			}

			/** At a `(` in expression position: one unnamed item without a trailing comma is a grouping, anything else a tuple. */
			FDstExprPtr ParseParenOrTuple()
			{
				const int32 Start = Index;
				const int32 OpenOffset = Current().Span.Offset;

				TArray<FDstArgumentPtr> Items;
				bool bTrailingComma = false;
				if (!ParseArgumentList(Items, bTrailingComma))
				{
					return nullptr;
				}

				// `()` is the empty tuple: a struct with every field at its default.
				if (Items.Num() == 1 && !Items[0]->IsNamed() && !bTrailingComma)
				{
					ReleaseTrivia(Items[0]->Span.Offset, OpenOffset);
					TUniquePtr<FDstParenExpr> Paren = MakeUnique<FDstParenExpr>();
					Paren->Inner = MoveTemp(Items[0]->Value);
					Paren->Span = SpanFrom(Start);
					return MoveTemp(Paren);
				}

				TUniquePtr<FDstTupleExpr> Tuple = MakeUnique<FDstTupleExpr>();
				Tuple->Items = MoveTemp(Items);
				Tuple->Span = SpanFrom(Start);
				return MoveTemp(Tuple);
			}

			/** At a `[` in expression position. */
			FDstExprPtr ParseList()
			{
				const int32 Start = Index;
				const FDstToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("["), TEXT("]"));

				TUniquePtr<FDstListExpr> List = MakeUnique<FDstListExpr>();
				TArray<int32> Carry;

				while (!Check(EDstTokenKind::RightBracket))
				{
					if (AtEnd())
					{
						ReportUnclosedAtEnd();
						ReleaseCarry(Carry);
						return nullptr;
					}

					FAnchorStart Anchor = BeginAnchor(Carry);
					FDstExprPtr Item = ParseExpression();
					bool bRecover = !Item;
					if (Item && !Match(EDstTokenKind::Comma) && !Check(EDstTokenKind::RightBracket))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("',' or ']' after the list item")));
						}
						bRecover = true;
					}
					EndAnchor(MoveTemp(Anchor), Item.Get(), Carry);
					if (Item)
					{
						List->Items.Add(MoveTemp(Item));
					}

					if (bRecover && !SkipToListBoundary(EDstTokenKind::RightBracket, false))
					{
						ReleaseCarry(Carry);
						return nullptr;
					}
				}

				ClaimInner(Open.Span.Offset, Carry);
				Advance(); // ]
				List->Span = SpanFrom(Start);
				return MoveTemp(List);
			}

			// ------------------------------------------------------------------------------------
			// Expressions
			// ------------------------------------------------------------------------------------

			/** Where a `[` after a call goes: see CallTakesBracket. */
			bool BracketBelongsToTransition() const
			{
				if (!bInTransition || ClauseBaseDepth != OpenBrackets.Num())
				{
					return false;
				}

				// Find where the group ends.
				int32 Depth = 0;
				for (int32 Ahead = 0; ; ++Ahead)
				{
					const FDstToken& Token = Peek(Ahead);
					if (Token.Is(EDstTokenKind::EndOfFile) || Token.Is(EDstTokenKind::Directive) || Token.Is(EDstTokenKind::RightBrace))
					{
						return false;
					}
					if (IsOpeningBracket(Token.Kind))
					{
						++Depth;
						continue;
					}
					if (IsClosingBracket(Token.Kind) && --Depth <= 0)
					{
						// Another group right after it means the first was the call's.
						return !Peek(Ahead + 1).Is(EDstTokenKind::LeftBracket) && StartsItemAt(Ahead + 1);
					}
				}
			}

			/**
			 * At a `[` right after the `)` of a call: whether the bracket is the call's node settings. It is, except for
			 * the call at the very end of a transition clause, where the last bracket is always the transition's options
			 * (`-> Root on Tick if not Hour(22, 5) [ Delay = 1 ]`). Node settings for that call go in front of the
			 * options (`... Hour(22, 5) [ Name = "Night" ] [ Delay = 1 ]`), or the call goes in parentheses.
			 */
			bool CallTakesBracket() const
			{
				return !BracketBelongsToTransition();
			}

			FDstExprPtr ParseExpression()
			{
				if (ExpressionDepth >= MaxNestingDepth)
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Current().Span, TEXT("The expression is nested too deeply."),
							FString::Printf(TEXT("Nest expressions at most %d levels deep."), MaxNestingDepth));
					}
					return nullptr;
				}
				FNestScope Nest(*this, ExpressionDepth);
				return ParseBinary(1);
			}

			/** Precedence climbing over GetBinaryOperator; every level is left-associative. */
			FDstExprPtr ParseBinary(const int32 MinPrecedence)
			{
				const int32 Start = Index;
				FDstExprPtr Left = ParseUnary();
				if (!Left)
				{
					return nullptr;
				}

				while (true)
				{
					EDstBinaryOp Op = EDstBinaryOp::Or;
					if (!GetBinaryOperator(Current(), Op))
					{
						break;
					}
					const int32 Precedence = GetDstBinaryOpPrecedence(Op);
					if (Precedence < MinPrecedence)
					{
						break;
					}

					const FDstToken& OpToken = Advance();
					if (OpToken.Is(EDstTokenKind::AmpersandAmpersand))
					{
						ReportSymbolicLogic(OpToken, TEXT("and"));
					}
					else if (OpToken.Is(EDstTokenKind::PipePipe))
					{
						ReportSymbolicLogic(OpToken, TEXT("or"));
					}

					FDstExprPtr Right = ParseBinary(Precedence + 1);
					if (!Right)
					{
						return nullptr;
					}

					TUniquePtr<FDstBinaryExpr> Node = MakeUnique<FDstBinaryExpr>();
					Node->Op = Op;
					Node->OpSpan = OpToken.Span;
					Node->Lhs = MoveTemp(Left);
					Node->Rhs = MoveTemp(Right);
					Node->Span = SpanFrom(Start);
					Left = MoveTemp(Node);
				}
				return Left;
			}

			/** DST2017: `&&`, `||` and `!` read as the word operators so the parse can go on. */
			void ReportSymbolicLogic(const FDstToken& Token, const TCHAR* Word)
			{
				Sink.Error(TEXT("DST2017"), Token.Span,
					FString::Printf(TEXT("'%s' is not an operator here; write '%s'."), GetDstTokenSpelling(Token.Kind), Word),
					TEXT("Conditions are combined with the words 'and', 'or' and 'not'."));
			}

			FDstExprPtr ParseUnary()
			{
				const FDstToken& Token = Current();
				const bool bBang = Token.Is(EDstTokenKind::Bang);
				const bool bMinus = Token.Is(EDstTokenKind::Minus);
				// `not` is the operator when an operand follows; in `Tell(not)` it is a name.
				const bool bNot = IsWord(Token, TEXT("not")) && CanStartOperand(Peek(1));
				if (!bBang && !bMinus && !bNot)
				{
					return ParsePostfix();
				}

				if (ExpressionDepth >= MaxNestingDepth)
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DST2001"), Token.Span, TEXT("The expression is nested too deeply."),
							FString::Printf(TEXT("Nest expressions at most %d levels deep."), MaxNestingDepth));
					}
					return nullptr;
				}
				FNestScope Nest(*this, ExpressionDepth);

				const int32 Start = Index;
				if (bBang)
				{
					ReportSymbolicLogic(Token, TEXT("not"));
				}
				const EDstUnaryOp Op = bMinus ? EDstUnaryOp::Negate : EDstUnaryOp::Not;
				Advance();

				FDstExprPtr Operand = ParseUnary();
				if (!Operand)
				{
					return nullptr;
				}

				TUniquePtr<FDstUnaryExpr> Node = MakeUnique<FDstUnaryExpr>();
				Node->Op = Op;
				Node->Operand = MoveTemp(Operand);
				Node->Span = SpanFrom(Start);
				return MoveTemp(Node);
			}

			/**
			 * `Primary { . Ident | ( Args ) [ Settings ] }`. A `[` is read only straight after a call's `)`, and only as
			 * that call's node settings (or, at the end of a transition, left for the transition: CallTakesBracket).
			 */
			FDstExprPtr ParsePostfix()
			{
				const int32 Start = Index;
				FDstExprPtr Expr = ParsePrimary();
				if (!Expr)
				{
					return nullptr;
				}

				while (true)
				{
					if (Check(EDstTokenKind::Dot))
					{
						Advance();
						if (!Check(EDstTokenKind::Identifier))
						{
							if (ShouldReportAtCurrent())
							{
								Sink.Error(TEXT("DST2001"), Current().Span, Expected(TEXT("a member name after '.'")));
							}
							return nullptr;
						}
						const FDstToken& Member = Advance();
						TUniquePtr<FDstMemberExpr> Node = MakeUnique<FDstMemberExpr>();
						Node->Object = MoveTemp(Expr);
						Node->Member = Member.Text;
						Node->MemberSpan = Member.Span;
						Node->bMemberQuoted = Member.bQuoted;
						Node->Span = SpanFrom(Start);
						Expr = MoveTemp(Node);
					}
					else if (Check(EDstTokenKind::LeftParen))
					{
						const int32 OpenIndex = Index;
						TUniquePtr<FDstCallExpr> Call = MakeUnique<FDstCallExpr>();
						Call->Callee = MoveTemp(Expr);
						bool bTrailingComma = false;
						if (!ParseArgumentList(Call->Args, bTrailingComma))
						{
							return nullptr;
						}
						Call->ArgsSpan = SpanFrom(OpenIndex);

						if (Check(EDstTokenKind::LeftBracket) && CallTakesBracket())
						{
							Call->Meta = ParseMeta();
							if (!Call->Meta)
							{
								return nullptr;
							}
						}

						Call->Span = SpanFrom(Start);
						Expr = MoveTemp(Call);
					}
					else
					{
						break;
					}
				}
				return Expr;
			}

			FDstExprPtr ParsePrimary()
			{
				const FDstToken& Token = Current();
				switch (Token.Kind)
				{
				case EDstTokenKind::Number:
				{
					TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
					Literal->LiteralKind = EDstLiteralKind::Number;
					Literal->Text = Token.Text;
					Literal->Number = Token.Number;
					Literal->Span = Token.Span;
					Advance();
					return MoveTemp(Literal);
				}
				case EDstTokenKind::String:
				{
					TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
					Literal->LiteralKind = EDstLiteralKind::String;
					Literal->Text = Token.Text;
					Literal->Span = Token.Span;
					Advance();
					return MoveTemp(Literal);
				}
				case EDstTokenKind::Identifier:
				{
					// In backticks `true` is a name like any other.
					if (!Token.bQuoted && (Token.Text.Equals(TEXT("true"), ESearchCase::CaseSensitive) || Token.Text.Equals(TEXT("false"), ESearchCase::CaseSensitive)))
					{
						TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
						Literal->LiteralKind = EDstLiteralKind::Bool;
						Literal->Text = Token.Text;
						Literal->bBool = Token.Text.Equals(TEXT("true"), ESearchCase::CaseSensitive);
						Literal->Span = Token.Span;
						Advance();
						return MoveTemp(Literal);
					}

					// Where an operand is expected at the top level of a clause, a clause word is the next clause and the
					// operand is missing: `if A and` followed by `do ...`. Naming something `do` takes backticks.
					if (ClauseBaseDepth == OpenBrackets.Num() && StartsItemAt(0))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DST2001"), Token.Span, Expected(TEXT("an expression")),
								TEXT("This starts the next clause, so the expression before it is unfinished. A name that looks like a keyword is written in backticks."));
						}
						return nullptr;
					}

					if (!Token.bQuoted && Token.Text.Equals(TEXT("ref"), ESearchCase::CaseSensitive) && Peek(1).Is(EDstTokenKind::Identifier))
					{
						return ParseRef();
					}

					TUniquePtr<FDstNameExpr> Name = MakeUnique<FDstNameExpr>();
					Name->Name = Token.Text;
					Name->bQuoted = Token.bQuoted;
					Name->Span = Token.Span;
					Advance();
					return MoveTemp(Name);
				}
				case EDstTokenKind::LeftParen:
					return ParseParenOrTuple();
				case EDstTokenKind::LeftBracket:
					return ParseList();
				default:
					break;
				}

				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DST2001"), Token.Span, Expected(TEXT("an expression")));
				}
				return nullptr;
			}

			/** At `ref`: `ref Params.WanderSpot`. The target must be a path. */
			FDstExprPtr ParseRef()
			{
				const int32 Start = Index;
				Advance(); // ref

				FDstExprPtr Target = ParsePostfix();
				if (!Target)
				{
					return nullptr;
				}
				TArray<FString> Segments;
				if (!TryGetDstPath(*Target, Segments))
				{
					Sink.Error(TEXT("DST2001"), Target->Span, TEXT("Expected a path after 'ref', such as 'ref Params.WanderSpot'."));
					return nullptr;
				}

				TUniquePtr<FDstRefExpr> Ref = MakeUnique<FDstRefExpr>();
				Ref->Target = MoveTemp(Target);
				Ref->Span = SpanFrom(Start);
				return MoveTemp(Ref);
			}

			// ------------------------------------------------------------------------------------
			// Trivia collection
			// ------------------------------------------------------------------------------------

			FDstComment MakeComment(const int32 CommentIndex) const
			{
				const FDstToken& Token = Comments[CommentIndex];
				FDstComment Comment;
				Comment.Text = Token.Text;
				Comment.Span = Token.Span;
				Comment.bBlock = Token.Text.StartsWith(TEXT("/*"), ESearchCase::CaseSensitive);
				return Comment;
			}

			void TakeAnchorTrivia(FDstFile& File, const FDstNode* Node)
			{
				FPendingTrivia Pending;
				if (!Node || !PendingAnchors.RemoveAndCopyValue(Node->Span.Offset, Pending))
				{
					return;
				}
				FDstTrivia& Trivia = File.Trivia.FindOrAdd(Node);
				for (const int32 CommentIndex : Pending.Leading)
				{
					Trivia.Leading.Add(MakeComment(CommentIndex));
				}
				Trivia.BlankLinesBefore = Pending.BlankLinesBefore;
				if (Pending.Trailing != INDEX_NONE)
				{
					Trivia.Trailing.Emplace(MakeComment(Pending.Trailing));
				}
				for (const int32 CommentIndex : Pending.Inner)
				{
					Trivia.Inner.Add(MakeComment(CommentIndex));
				}
			}

			void TakeInnerTrivia(FDstFile& File, const FDstNode* Node, const int32 OpenOffset)
			{
				TArray<int32> Inner;
				if (!PendingInner.RemoveAndCopyValue(OpenOffset, Inner))
				{
					return;
				}
				FDstTrivia& Trivia = File.Trivia.FindOrAdd(Node);
				for (const int32 CommentIndex : Inner)
				{
					Trivia.Inner.Add(MakeComment(CommentIndex));
				}
			}

			/** Moves pending trivia onto the nodes of the finished tree, in the roles each one plays there. */
			void CollectTrivia(FDstFile& File)
			{
				if (!bKeepTrivia)
				{
					return;
				}
				for (const TUniquePtr<FDstInclude>& Include : File.Includes)
				{
					TakeAnchorTrivia(File, Include.Get());
				}
				if (const FDstDocument* Document = File.Document.Get())
				{
					TakeAnchorTrivia(File, Document);
					if (Document->OpenSpan.Length > 0)
					{
						TakeInnerTrivia(File, Document, Document->OpenSpan.Offset);
					}
					for (const FDstArgumentPtr& Argument : Document->HeaderArgs)
					{
						TakeAnchorTrivia(File, Argument.Get());
						WalkExpr(File, Argument->Value.Get());
					}
					for (const FDstStmtPtr& Member : Document->Members)
					{
						if (Member)
						{
							TakeAnchorTrivia(File, Member.Get());
							WalkStmt(File, *Member);
						}
					}
				}
			}

			void WalkMeta(FDstFile& File, const FDstMeta* Meta)
			{
				if (!Meta)
				{
					return;
				}
				for (const TUniquePtr<FDstMetaEntry>& Entry : Meta->Entries)
				{
					WalkExpr(File, Entry->Value.Get());
				}
			}

			void WalkParams(FDstFile& File, const TArray<TUniquePtr<FDstParam>>& Params)
			{
				for (const TUniquePtr<FDstParam>& Param : Params)
				{
					WalkExpr(File, Param->Default.Get());
				}
			}

			void WalkStmt(FDstFile& File, const FDstStmt& Statement)
			{
				switch (Statement.Kind)
				{
				case EDstNodeKind::SectionStmt:
				{
					const FDstSectionStmt& Section = static_cast<const FDstSectionStmt&>(Statement);
					TakeInnerTrivia(File, &Section, Section.NameSpan.Offset);
					for (const FDstStmtPtr& Entry : Section.Entries)
					{
						if (Entry)
						{
							TakeAnchorTrivia(File, Entry.Get());
							WalkStmt(File, *Entry);
						}
					}
					break;
				}
				case EDstNodeKind::AssignStmt:
				{
					const FDstAssignStmt& Assign = static_cast<const FDstAssignStmt&>(Statement);
					WalkExpr(File, Assign.Value.Get());
					WalkMeta(File, Assign.Meta.Get());
					break;
				}
				case EDstNodeKind::DeclStmt:
				{
					const FDstDeclStmt& Decl = static_cast<const FDstDeclStmt&>(Statement);
					WalkExpr(File, Decl.Value.Get());
					WalkMeta(File, Decl.Meta.Get());
					break;
				}
				case EDstNodeKind::ExprStmt:
					WalkExpr(File, static_cast<const FDstExprStmt&>(Statement).Expr.Get());
					break;
				case EDstNodeKind::TemplateStmt:
				{
					const FDstTemplateStmt& Template = static_cast<const FDstTemplateStmt&>(Statement);
					WalkParams(File, Template.Params);
					WalkExpr(File, Template.Body.Get());
					break;
				}
				case EDstNodeKind::StateStmt:
				{
					const FDstStateStmt& State = static_cast<const FDstStateStmt&>(Statement);
					WalkParams(File, State.Params);
					WalkMeta(File, State.Meta.Get());
					TakeInnerTrivia(File, &State, State.OpenSpan.Offset);
					for (const FDstStmtPtr& Item : State.Body)
					{
						if (Item)
						{
							TakeAnchorTrivia(File, Item.Get());
							WalkStmt(File, *Item);
						}
					}
					break;
				}
				case EDstNodeKind::ClauseStmt:
				{
					const FDstClauseStmt& Clause = static_cast<const FDstClauseStmt&>(Statement);
					WalkExpr(File, Clause.Value.Get());
					for (const TUniquePtr<FDstTaskItem>& Task : Clause.Tasks)
					{
						TakeAnchorTrivia(File, Task.Get());
						WalkExpr(File, Task->Node.Get());
					}
					WalkExpr(File, Clause.Trigger.Get());
					WalkExpr(File, Clause.Condition.Get());
					WalkMeta(File, Clause.Meta.Get());
					break;
				}
				default:
					break;
				}
			}

			void WalkExpr(FDstFile& File, const FDstExpr* Expr)
			{
				if (!Expr)
				{
					return;
				}
				switch (Expr->Kind)
				{
				case EDstNodeKind::MemberExpr:
					WalkExpr(File, static_cast<const FDstMemberExpr*>(Expr)->Object.Get());
					break;
				case EDstNodeKind::CallExpr:
				{
					const FDstCallExpr* Call = static_cast<const FDstCallExpr*>(Expr);
					WalkExpr(File, Call->Callee.Get());
					TakeInnerTrivia(File, Call, Call->ArgsSpan.Offset);
					for (const FDstArgumentPtr& Argument : Call->Args)
					{
						TakeAnchorTrivia(File, Argument.Get());
						WalkExpr(File, Argument->Value.Get());
					}
					WalkMeta(File, Call->Meta.Get());
					break;
				}
				case EDstNodeKind::ListExpr:
				{
					const FDstListExpr* List = static_cast<const FDstListExpr*>(Expr);
					TakeInnerTrivia(File, List, List->Span.Offset);
					for (const FDstExprPtr& Item : List->Items)
					{
						TakeAnchorTrivia(File, Item.Get());
						WalkExpr(File, Item.Get());
					}
					break;
				}
				case EDstNodeKind::TupleExpr:
				{
					const FDstTupleExpr* Tuple = static_cast<const FDstTupleExpr*>(Expr);
					TakeInnerTrivia(File, Tuple, Tuple->Span.Offset);
					for (const FDstArgumentPtr& Item : Tuple->Items)
					{
						TakeAnchorTrivia(File, Item.Get());
						WalkExpr(File, Item->Value.Get());
					}
					break;
				}
				case EDstNodeKind::UnaryExpr:
					WalkExpr(File, static_cast<const FDstUnaryExpr*>(Expr)->Operand.Get());
					break;
				case EDstNodeKind::BinaryExpr:
				{
					const FDstBinaryExpr* Binary = static_cast<const FDstBinaryExpr*>(Expr);
					WalkExpr(File, Binary->Lhs.Get());
					WalkExpr(File, Binary->Rhs.Get());
					break;
				}
				case EDstNodeKind::ParenExpr:
					WalkExpr(File, static_cast<const FDstParenExpr*>(Expr)->Inner.Get());
					break;
				case EDstNodeKind::RefExpr:
					WalkExpr(File, static_cast<const FDstRefExpr*>(Expr)->Target.Get());
					break;
				default:
					break;
				}
			}

			// ------------------------------------------------------------------------------------
			// State
			// ------------------------------------------------------------------------------------

			const FDstSourceText& Source;
			FDstDiagnosticSink& Sink;
			/** Every token but comments; the last one is always EndOfFile. */
			TArray<FDstToken> Tokens;
			int32 Index = 0;

			bool bKeepTrivia = false;
			/** How deep type arguments may nest (`Array<Array<float>>` is two). */
			static constexpr int32 MaxTypeDepth = 16;
			/** How deep states and expressions may nest before the parser refuses to go on. */
			static constexpr int32 MaxNestingDepth = 200;
			/** Set once the end of the file has been reported (or another diagnostic already explains it). */
			bool bEndReported = false;
			TArray<FOpenBracket> OpenBrackets;

			/** The bracket depth (OpenBrackets.Num()) of the clause being parsed; INDEX_NONE outside a clause. */
			int32 ClauseBaseDepth = INDEX_NONE;
			/** The clause being parsed is a transition. */
			bool bInTransition = false;
			int32 ExpressionDepth = 0;
			int32 StateDepth = 0;

			/** Comment tokens in source order. */
			TArray<FDstToken> Comments;
			/** Per token: how many comments come before it in the source. */
			TArray<int32> CommentsBefore;
			/** Comments before this index are claimed. */
			int32 CommentCursor = 0;
			/** Per child being parsed, innermost last: the comments swept up inside it. */
			TArray<TArray<int32>> Frames;
			/** By the offset of a child's first token. */
			TMap<int32, FPendingTrivia> PendingAnchors;
			/** By the offset of a container's opening bracket (a section: its name). */
			TMap<int32, TArray<int32>> PendingInner;
		};

		/** Lexer and parser report into their own sinks; the caller gets both, in source order. */
		static void MergeDiagnostics(FDstDiagnosticSink& Sink, const FDstDiagnosticSink& Lexical, const FDstDiagnosticSink& Parsed)
		{
			TArray<FDstDiagnostic> All;
			All.Reserve(Lexical.Num() + Parsed.Num());
			All.Append(Lexical.GetDiagnostics());
			All.Append(Parsed.GetDiagnostics());
			All.StableSort([](const FDstDiagnostic& A, const FDstDiagnostic& B)
			{
				return A.Span.Offset < B.Span.Offset;
			});

			for (FDstDiagnostic& Diagnostic : All)
			{
				switch (Diagnostic.Severity)
				{
				case EDstSeverity::Error:
					Sink.ErrorIn(*Diagnostic.Code, Diagnostic.FilePath, Diagnostic.Span, MoveTemp(Diagnostic.Message), MoveTemp(Diagnostic.Hint));
					break;
				case EDstSeverity::Warning:
					Sink.WarningIn(*Diagnostic.Code, Diagnostic.FilePath, Diagnostic.Span, MoveTemp(Diagnostic.Message), MoveTemp(Diagnostic.Hint));
					break;
				case EDstSeverity::Info:
				default:
					Sink.Info(*Diagnostic.Code, Diagnostic.Span, MoveTemp(Diagnostic.Message), MoveTemp(Diagnostic.Hint));
					break;
				}
			}
		}

		/** An unclosed block comment ran to the end of the file and took the closing brackets with it. */
		static bool CommentSwallowedTheEnd(const FDstDiagnosticSink& Lexical)
		{
			for (const FDstDiagnostic& Diagnostic : Lexical.GetDiagnostics())
			{
				if (Diagnostic.Code.Equals(TEXT("DST1003"), ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}
	}

	TUniquePtr<FDstFile> ParseDst(const FDstSourceText& Source, FDstDiagnosticSink& Sink, const FDstParseOptions& Options)
	{
		FDstDiagnosticSink Lexical(Sink.GetFilePath());
		FDstDiagnosticSink Parsed(Sink.GetFilePath());

		FDstLexOptions LexOptions;
		LexOptions.bEmitComments = Options.bKeepTrivia;
		TArray<FDstToken> Tokens = LexDst(Source, Lexical, LexOptions);

		DstParserDetail::FDstParser Parser(Source, MoveTemp(Tokens), Parsed, Options.bKeepTrivia,
			DstParserDetail::CommentSwallowedTheEnd(Lexical));
		TUniquePtr<FDstFile> File = Parser.ParseFile();

		DstParserDetail::MergeDiagnostics(Sink, Lexical, Parsed);
		return File;
	}

	FDstExprPtr ParseDstExpression(const FDstSourceText& Source, FDstDiagnosticSink& Sink)
	{
		FDstDiagnosticSink Lexical(Sink.GetFilePath());
		FDstDiagnosticSink Parsed(Sink.GetFilePath());

		TArray<FDstToken> Tokens = LexDst(Source, Lexical);

		DstParserDetail::FDstParser Parser(Source, MoveTemp(Tokens), Parsed, false,
			DstParserDetail::CommentSwallowedTheEnd(Lexical));
		FDstExprPtr Expr = Parser.ParseStandaloneExpression();

		const bool bFailed = Lexical.HasErrors() || Parsed.HasErrors();
		DstParserDetail::MergeDiagnostics(Sink, Lexical, Parsed);

		// A partial expression (a call that kept its good arguments) is still not the expression written.
		if (bFailed)
		{
			return nullptr;
		}
		return Expr;
	}

	bool IsDstClauseWord(const FString& Word)
	{
		return DstParserDetail::IsClauseWordText(Word);
	}
}
