// Copyright DreamMoon. All Rights Reserved.
//
// The printer: syntax tree -> canonical text (Docs/Grammar.md, "Canonical format"), and the tree dump the corpus
// goldens use.
//
// Every layout decision is a function of the tree and of the comments and blank-line flags in the file's trivia
// (and, when the caller passes the source text, of which calls, lists, tuples and cards spanned lines there),
// never of the exact spacing the author used. The parse of the output carries the same tree and the same trivia,
// so printing it again makes the same decisions; that is the idempotence the tests hold the printer to.
//
//   * An expression prints on one line when it fits from the column it starts at, counting what follows it on that
//     line (`;`, `,`, metadata). Otherwise the container that does not fit is broken and each item gets the same
//     choice:
//       - a call, list or tuple puts one item per line, one indent in, with its closing bracket back at the indent
//         of the line it opened on;
//       - a chain of `and` (or of `or`) puts one operand per line with the operator leading, every line at the
//         column where the chain started -- for a clause that is the column after its keyword;
//       - the tasks of a `do` clause go one per line at that same column, each but the last ending in `,`.
//   * A state card prints on one line (`State X { on A  do B() }`) when it has no child states, no comments and no
//     blank lines between its clauses, at most two clauses besides a bare `-> Target`, and fits. Consecutive
//     one-line cards line their `{` up. Every other card is broken: `{` after the name, then its clauses, child
//     states and regions in source order (the printer never reorders; the decompiler writes the grammar's order).
//   * A node carrying a comment cannot sit on a shared line (a `//` would swallow the rest), and a block holding
//     `#Region` cannot be one line (a directive starts its line): every container around such a node breaks.
//   * Alignment pads the statements of a run so their `=` (or `{`) share a column. A statement that the padding
//     pushes onto several lines leaves the run and the runs are laid out again, so a statement is either aligned
//     on one line or printed on its own terms. A blank line ends a run; the blank lines the printer adds itself
//     are decided before the runs, so printing the result again forms the same runs.
//
// Text is built with `\n` and converted to FDstPrintOptions::LineEnd at the very end, so the width arithmetic can
// read a column off the last `\n`. The printer is total: a null child (what a broken parse leaves) prints as
// nothing rather than crashing.

#include "Lang/DstPrinter.h"

#include "Lang/DstAst.h"
#include "Lang/DstToken.h"

namespace UE::DreamStateTree::Lang
{
	// Kept apart from the other front-end files' helpers: the module may be built as one unity blob.
	namespace DstPrinterPrivate
	{
		// ------------------------------------------------------------------------------------------
		// Small text helpers
		// ------------------------------------------------------------------------------------------

		/** The escapes the lexer resolves, put back. */
		static FString EscapeString(const FString& Value)
		{
			FString Result;
			Result.Reserve(Value.Len() + 2);
			for (int32 Index = 0; Index < Value.Len(); ++Index)
			{
				const TCHAR Char = Value[Index];
				switch (Char)
				{
				case TEXT('\\'): Result += TEXT("\\\\"); break;
				case TEXT('\"'): Result += TEXT("\\\""); break;
				case TEXT('\n'): Result += TEXT("\\n");  break;
				case TEXT('\t'): Result += TEXT("\\t");  break;
				case TEXT('\r'): Result += TEXT("\\r");  break;
				default:         Result.AppendChar(Char); break;
				}
			}
			return Result;
		}

		static FString QuoteString(const FString& Value)
		{
			return FString(TEXT("\"")) + EscapeString(Value) + TEXT("\"");
		}

		static FString MakeSpaces(int32 Count)
		{
			return Count > 0 ? FString::ChrN(Count, TEXT(' ')) : FString();
		}

		static FString PadRight(const FString& Text, int32 Width)
		{
			return Text + MakeSpaces(Width - Text.Len());
		}

		static bool IsMultiLine(const FString& Text)
		{
			int32 BreakIndex = INDEX_NONE;
			return Text.FindChar(TEXT('\n'), BreakIndex);
		}

		/** The column right after Text when its first line starts at StartColumn (later lines carry their own indent). */
		static int32 GetEndColumn(const FString& Text, int32 StartColumn)
		{
			int32 LastBreak = INDEX_NONE;
			if (Text.FindLastChar(TEXT('\n'), LastBreak))
			{
				return Text.Len() - LastBreak - 1;
			}
			return StartColumn + Text.Len();
		}

		/** A comment as it is written back: verbatim, with its own line breaks as `\n` and a line comment's trailing blanks dropped. */
		static FString GetCommentText(const FDstComment& Comment)
		{
			FString Text = Comment.Text.Replace(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
			Text.ReplaceInline(TEXT("\r"), TEXT("\n"), ESearchCase::CaseSensitive);
			if (!Comment.bBlock)
			{
				Text.TrimEndInline();
			}
			return Text;
		}

		static FString GetNumberText(const FDstLiteralExpr& Literal)
		{
			// The lexeme is the truth; the value is only a fallback for a tree built by hand.
			return Literal.Text.IsEmpty() ? FString::SanitizeFloat(Literal.Number) : Literal.Text;
		}

		static FString GetLiteralText(const FDstLiteralExpr& Literal)
		{
			switch (Literal.LiteralKind)
			{
			case EDstLiteralKind::String: return QuoteString(Literal.Text);
			case EDstLiteralKind::Bool:   return Literal.bBool ? FString(TEXT("true")) : FString(TEXT("false"));
			default:                      return GetNumberText(Literal);
			}
		}

		static FString GetDocumentKindText(const FDstDocument& Document)
		{
			if (!Document.KindName.IsEmpty())
			{
				return Document.KindName;
			}
			switch (Document.DocumentKind)
			{
			case EDstDocumentKind::StateTree:        return TEXT("StateTree");
			case EDstDocumentKind::StateTreeLibrary: return TEXT("StateTreeLibrary");
			default:                                 return TEXT("Unknown");
			}
		}

		static FString GetRegionText(const FDstRegionStmt& Region)
		{
			return Region.bEnd ? FString(TEXT("#EndRegion")) : FString(TEXT("#Region ")) + QuoteString(Region.Label);
		}

		/**
		 * A declared type as written. A generic type (`Array<float>`, `Class<Actor>`) already carries the backticks of
		 * its quoted parts; a qualified name is quoted one segment at a time; a single name is quoted when it is not a
		 * plain identifier.
		 */
		static FString GetTypeText(const FString& TypeName)
		{
			if (TypeName.IsEmpty() || TypeName.Contains(TEXT("<")) || TypeName.Contains(TEXT("`")))
			{
				return TypeName;
			}
			TArray<FString> Segments;
			TypeName.ParseIntoArray(Segments, TEXT("."), false);
			for (FString& Segment : Segments)
			{
				Segment = QuoteDstName(Segment);
			}
			return FString::Join(Segments, TEXT("."));
		}

		/** The words that mean something in expression position; a name spelled like one can only have been written in backticks. */
		static bool IsExpressionWord(const FString& Name)
		{
			static const TCHAR* const Words[] = { TEXT("and"), TEXT("or"), TEXT("not"), TEXT("ref"), TEXT("true"), TEXT("false") };
			for (const TCHAR* Word : Words)
			{
				if (Name.Equals(Word, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * A name in expression position. Backticks the author wrote stay: `` `Next` `` names a state called Next, while
		 * bare `Next` is the transition target of that name, so dropping them would change what the text means.
		 */
		static FString GetNameExprText(const FDstNameExpr& Name)
		{
			if (Name.Name.IsEmpty())
			{
				return FString();
			}
			if (Name.bQuoted || !IsDstPlainIdentifier(Name.Name) || IsExpressionWord(Name.Name))
			{
				return FString(TEXT("`")) + Name.Name + TEXT("`");
			}
			return Name.Name;
		}

		static FString GetMemberText(const FDstMemberExpr& Member)
		{
			if (Member.Member.IsEmpty())
			{
				return FString();
			}
			if (Member.bMemberQuoted || !IsDstPlainIdentifier(Member.Member))
			{
				return FString(TEXT("`")) + Member.Member + TEXT("`");
			}
			return Member.Member;
		}

		template <typename T>
		static bool HasAnyChild(const TArray<TUniquePtr<T>>& Children)
		{
			for (const TUniquePtr<T>& Child : Children)
			{
				if (Child)
				{
					return true;
				}
			}
			return false;
		}

		template <typename T>
		static TArray<const FDstStmt*> GatherStatements(const TArray<TUniquePtr<T>>& Children)
		{
			TArray<const FDstStmt*> Result;
			Result.Reserve(Children.Num());
			for (const TUniquePtr<T>& Child : Children)
			{
				if (Child)
				{
					Result.Add(Child.Get());
				}
			}
			return Result;
		}

		static bool IsAlignableKind(EDstNodeKind Kind)
		{
			return Kind == EDstNodeKind::SectionStmt || Kind == EDstNodeKind::AssignStmt || Kind == EDstNodeKind::DeclStmt
				|| Kind == EDstNodeKind::StateStmt;
		}

		static FString GetClauseKeyword(const FDstClauseStmt& Clause)
		{
			switch (Clause.ClauseKind)
			{
			case EDstClauseKind::On:         return TEXT("on");
			case EDstClauseKind::If:         return TEXT("if");
			case EDstClauseKind::Pick:       return TEXT("pick");
			case EDstClauseKind::Utility:    return TEXT("utility");
			case EDstClauseKind::Link:       return Clause.bLinkAsset ? FString(TEXT("link asset")) : FString(TEXT("link"));
			case EDstClauseKind::Do:         return TEXT("do");
			case EDstClauseKind::Transition: return TEXT("->");
			}
			return FString();
		}

		/** `-> Target` and nothing else: it does not count toward the two clauses a one-line card may hold. */
		static bool IsBareTransition(const FDstClauseStmt& Clause)
		{
			return Clause.ClauseKind == EDstClauseKind::Transition && !Clause.Trigger && !Clause.Condition && !Clause.Meta;
		}

		/**
		 * A card's body in source order. The printer never moves a clause: regions group clauses, comments sit between
		 * them, and `do` and `->` order matters. The decompiler writes clauses in the grammar's order to begin with.
		 */
		static TArray<const FDstStmt*> OrderStateBody(const FDstStateStmt& State)
		{
			TArray<const FDstStmt*> Ordered;
			Ordered.Reserve(State.Body.Num());
			for (const FDstStmtPtr& Item : State.Body)
			{
				if (Item)
				{
					Ordered.Add(Item.Get());
				}
			}
			return Ordered;
		}

		// ------------------------------------------------------------------------------------------
		// Children
		// ------------------------------------------------------------------------------------------

		/** Calls Visit on every direct child node in source order (a transition: target, trigger, condition, options). */
		static void VisitChildren(const FDstNode& Node, TFunctionRef<void(const FDstNode&)> Visit)
		{
			auto One = [&Visit](const FDstNode* Child)
			{
				if (Child)
				{
					Visit(*Child);
				}
			};

			switch (Node.Kind)
			{
			case EDstNodeKind::MemberExpr:
				One(static_cast<const FDstMemberExpr&>(Node).Object.Get());
				break;
			case EDstNodeKind::CallExpr:
			{
				const FDstCallExpr& Call = static_cast<const FDstCallExpr&>(Node);
				One(Call.Callee.Get());
				for (const FDstArgumentPtr& Argument : Call.Args)
				{
					One(Argument.Get());
				}
				One(Call.Meta.Get());
				break;
			}
			case EDstNodeKind::ListExpr:
				for (const FDstExprPtr& Item : static_cast<const FDstListExpr&>(Node).Items)
				{
					One(Item.Get());
				}
				break;
			case EDstNodeKind::TupleExpr:
				for (const FDstArgumentPtr& Item : static_cast<const FDstTupleExpr&>(Node).Items)
				{
					One(Item.Get());
				}
				break;
			case EDstNodeKind::UnaryExpr:
				One(static_cast<const FDstUnaryExpr&>(Node).Operand.Get());
				break;
			case EDstNodeKind::BinaryExpr:
			{
				const FDstBinaryExpr& Binary = static_cast<const FDstBinaryExpr&>(Node);
				One(Binary.Lhs.Get());
				One(Binary.Rhs.Get());
				break;
			}
			case EDstNodeKind::ParenExpr:
				One(static_cast<const FDstParenExpr&>(Node).Inner.Get());
				break;
			case EDstNodeKind::RefExpr:
				One(static_cast<const FDstRefExpr&>(Node).Target.Get());
				break;
			case EDstNodeKind::SectionStmt:
				for (const FDstStmtPtr& Entry : static_cast<const FDstSectionStmt&>(Node).Entries)
				{
					One(Entry.Get());
				}
				break;
			case EDstNodeKind::AssignStmt:
			{
				const FDstAssignStmt& Assign = static_cast<const FDstAssignStmt&>(Node);
				One(Assign.Value.Get());
				One(Assign.Meta.Get());
				break;
			}
			case EDstNodeKind::DeclStmt:
			{
				const FDstDeclStmt& Decl = static_cast<const FDstDeclStmt&>(Node);
				One(Decl.Value.Get());
				One(Decl.Meta.Get());
				break;
			}
			case EDstNodeKind::ExprStmt:
				One(static_cast<const FDstExprStmt&>(Node).Expr.Get());
				break;
			case EDstNodeKind::TemplateStmt:
			{
				const FDstTemplateStmt& Template = static_cast<const FDstTemplateStmt&>(Node);
				for (const TUniquePtr<FDstParam>& Param : Template.Params)
				{
					One(Param.Get());
				}
				One(Template.Body.Get());
				break;
			}
			case EDstNodeKind::StateStmt:
			{
				const FDstStateStmt& State = static_cast<const FDstStateStmt&>(Node);
				for (const TUniquePtr<FDstParam>& Param : State.Params)
				{
					One(Param.Get());
				}
				One(State.Meta.Get());
				for (const FDstStmtPtr& Item : State.Body)
				{
					One(Item.Get());
				}
				break;
			}
			case EDstNodeKind::ClauseStmt:
			{
				const FDstClauseStmt& Clause = static_cast<const FDstClauseStmt&>(Node);
				One(Clause.Value.Get());
				for (const TUniquePtr<FDstTaskItem>& Task : Clause.Tasks)
				{
					One(Task.Get());
				}
				One(Clause.Trigger.Get());
				One(Clause.Condition.Get());
				One(Clause.Meta.Get());
				break;
			}
			case EDstNodeKind::Argument:
				One(static_cast<const FDstArgument&>(Node).Value.Get());
				break;
			case EDstNodeKind::MetaEntry:
				One(static_cast<const FDstMetaEntry&>(Node).Value.Get());
				break;
			case EDstNodeKind::Meta:
				for (const TUniquePtr<FDstMetaEntry>& Entry : static_cast<const FDstMeta&>(Node).Entries)
				{
					One(Entry.Get());
				}
				break;
			case EDstNodeKind::Param:
				One(static_cast<const FDstParam&>(Node).Default.Get());
				break;
			case EDstNodeKind::TaskItem:
				One(static_cast<const FDstTaskItem&>(Node).Node.Get());
				break;
			case EDstNodeKind::Document:
			{
				const FDstDocument& Document = static_cast<const FDstDocument&>(Node);
				for (const FDstArgumentPtr& Argument : Document.HeaderArgs)
				{
					One(Argument.Get());
				}
				for (const FDstStmtPtr& Member : Document.Members)
				{
					One(Member.Get());
				}
				break;
			}
			default:
				break;
			}
		}

		// ------------------------------------------------------------------------------------------
		// Operator grouping
		// ------------------------------------------------------------------------------------------
		//
		// A parsed tree never needs a parenthesis the author did not write (its grouping is a ParenExpr), so for it
		// these all answer false. A tree made by hand -- the decompiler's -- or by template expansion can hold `a + b`
		// as the operand of `*`; printing it must not change what it means.

		static constexpr int32 UnaryPrecedence = 7;
		static constexpr int32 PostfixPrecedence = 8;

		static int32 GetOperandPrecedence(const FDstExpr& Expr)
		{
			switch (Expr.Kind)
			{
			case EDstNodeKind::BinaryExpr: return GetDstBinaryOpPrecedence(static_cast<const FDstBinaryExpr&>(Expr).Op);
			case EDstNodeKind::UnaryExpr:
			case EDstNodeKind::RefExpr:    return UnaryPrecedence;
			default:                       return PostfixPrecedence;
			}
		}

		/** The object of `.Member` or the callee of `(...)`. */
		static bool NeedsPostfixParens(const FDstExpr* Operand)
		{
			return Operand && GetOperandPrecedence(*Operand) < PostfixPrecedence;
		}

		static bool NeedsUnaryParens(const FDstExpr* Operand)
		{
			return Operand && GetOperandPrecedence(*Operand) < UnaryPrecedence;
		}

		/** Binary operators associate to the left, so an equal-strength operand needs parentheses on the right only. */
		static bool NeedsBinaryParens(const FDstExpr* Operand, EDstBinaryOp Op, bool bRightHandSide)
		{
			if (!Operand)
			{
				return false;
			}
			const int32 OperandPrecedence = GetOperandPrecedence(*Operand);
			const int32 OpPrecedence = GetDstBinaryOpPrecedence(Op);
			return bRightHandSide ? OperandPrecedence <= OpPrecedence : OperandPrecedence < OpPrecedence;
		}

		/** The operands of a left-leaning chain of one `and` / `or`, in source order. */
		static void CollectChain(const FDstExpr& Expr, EDstBinaryOp Op, TArray<const FDstExpr*>& OutOperands)
		{
			const FDstBinaryExpr* Binary = Expr.As<FDstBinaryExpr>();
			if (Binary && Binary->Op == Op && Binary->Lhs && Binary->Rhs)
			{
				CollectChain(*Binary->Lhs, Op, OutOperands);
				OutOperands.Add(Binary->Rhs.Get());
			}
			else
			{
				OutOperands.Add(&Expr);
			}
		}

		// ------------------------------------------------------------------------------------------
		// The layout printer
		// ------------------------------------------------------------------------------------------

		/** Column widths shared by one run of aligned statements; zero means "no padding". */
		struct FDstAlign
		{
			/** Sections and assignments: the name, so the `=` lines up. */
			int32 KeyWidth = 0;
			/** Declarations: the type, so the names line up. */
			int32 TypeWidth = 0;
			/** Declarations with a value: the name, so the `=` lines up. */
			int32 NameWidth = 0;
			/** One-line state cards: the head up to the `{`, so the braces line up. */
			int32 HeadWidth = 0;
		};

		/** Which blank lines the printer adds on top of the ones the author wrote. */
		enum class EDstBlankPolicy : uint8
		{
			/** Only the author's. */
			Author,
			/** The members of a document: a blank line between different kinds, and between top-level states. */
			Document,
		};

		/**
		 * Formatting functions return text whose first line carries no indentation (the caller has already written
		 * whatever precedes it) and whose later lines are fully indented, with no line end after the last line. The
		 * Append* functions add whole lines, each ending with `\n`.
		 *
		 * Columns are absolute. LineBase is the column a broken bracket hangs from: the indent of the line the
		 * expression starts on (or, inside a chain or a task list, the column that block started at). Column is
		 * where the expression itself starts.
		 */
		class FDstLayoutPrinter
		{
		public:
			FDstLayoutPrinter(const FDstPrintOptions& InOptions, const FDstFile* InTriviaFile)
				: Options(InOptions)
				, TriviaFile(InTriviaFile)
				, IndentWidth(FMath::Max(InOptions.IndentWidth, 0))
			{
			}

			FString PrintFile(const FDstFile& File);

			/** Expr starting at Column, breaking onto lines indented from Level. */
			FString FormatExprAtLevel(const FDstExpr& Expr, int32 Level, int32 Column)
			{
				return FormatExpr(Expr, GetIndentColumn(Level), Column, 0);
			}

			/** A statement on its own: no run to align with. */
			FString FormatStatementAlone(const FDstStmt& Stmt, int32 Level) { return FormatStatement(Stmt, Level, FDstAlign()); }

			FString ToOutputLineEnds(FString Text) const
			{
				if (!Options.LineEnd.Equals(TEXT("\n"), ESearchCase::CaseSensitive))
				{
					Text.ReplaceInline(TEXT("\n"), *Options.LineEnd, ESearchCase::CaseSensitive);
				}
				return Text;
			}

		private:
			// -- trivia

			const FDstTrivia* FindTrivia(const FDstNode* Node) const
			{
				return (TriviaFile && Node) ? TriviaFile->FindTrivia(Node) : nullptr;
			}

			/** A Leading or Trailing comment: the node cannot share its line with anything after it. */
			bool HasOwnComments(const FDstNode* Node) const
			{
				const FDstTrivia* Trivia = FindTrivia(Node);
				return Trivia && (Trivia->Leading.Num() > 0 || Trivia->Trailing.IsSet());
			}

			bool HasLeadingComments(const FDstNode* Node) const
			{
				const FDstTrivia* Trivia = FindTrivia(Node);
				return Trivia && Trivia->Leading.Num() > 0;
			}

			bool HasInnerComments(const FDstNode* Node) const
			{
				const FDstTrivia* Trivia = FindTrivia(Node);
				return Trivia && Trivia->Inner.Num() > 0;
			}

			bool HasBlankLineBefore(const FDstNode* Node) const
			{
				const FDstTrivia* Trivia = FindTrivia(Node);
				return Trivia && Trivia->BlankLinesBefore > 0;
			}

			void AppendLeadingComments(FString& Out, int32 IndentColumn, const FDstNode* Node) const
			{
				if (const FDstTrivia* Trivia = FindTrivia(Node))
				{
					for (const FDstComment& Comment : Trivia->Leading)
					{
						Out += MakeSpaces(IndentColumn);
						Out += GetCommentText(Comment);
						Out += TEXT("\n");
					}
				}
			}

			/** After the node's text on the same line, its `;` or `,` already written. */
			void AppendTrailingComment(FString& Out, const FDstNode* Node) const
			{
				const FDstTrivia* Trivia = FindTrivia(Node);
				if (Trivia && Trivia->Trailing.IsSet())
				{
					Out += TEXT(" ");
					Out += GetCommentText(Trivia->Trailing.GetValue());
				}
			}

			/** A container's comments after its last child, each on its own line before the closing bracket. */
			void AppendInnerComments(FString& Out, int32 IndentColumn, const FDstNode* Node) const
			{
				if (const FDstTrivia* Trivia = FindTrivia(Node))
				{
					for (const FDstComment& Comment : Trivia->Inner)
					{
						Out += MakeSpaces(IndentColumn);
						Out += GetCommentText(Comment);
						Out += TEXT("\n");
					}
				}
			}

			/** True when Node cannot print on one line: it holds a comment or a region somewhere inside, or was broken where layout is kept. */
			bool ForcesBreak(const FDstNode& Node);
			/** A call, list, tuple or state card that spans lines in Options.KeepBrokenIn. */
			bool WasBroken(const FDstNode& Node) const;

			// -- names

			FString NameText(const FString& Name) const { return QuoteDstName(Name); }

			// -- one-line forms

			FString Flat(const FDstExpr& Expr);
			FString FlatOrEmpty(const FDstExpr* Expr) { return Expr ? Flat(*Expr) : FString(); }
			FString FlatOperand(const FDstExpr* Operand, bool bParenthesize);
			FString BuildFlat(const FDstExpr& Expr);
			FString FlatArgument(const FDstArgument& Argument, bool bHeaderStyle);
			FString FlatArguments(const TArray<FDstArgumentPtr>& Arguments, bool bHeaderStyle);
			FString FlatMeta(const FDstMeta& Meta);
			/** ` [ ... ]` with its leading space, or nothing. */
			FString MetaSuffix(const FDstMeta* Meta) { return Meta ? FString(TEXT(" ")) + FlatMeta(*Meta) : FString(); }
			FString FlatTaskItem(const FDstTaskItem& Task);
			FString FlatClause(const FDstClauseStmt& Clause);
			FString FlatEntry(const FDstStmt& Stmt);
			FString FlatSection(const FDstSectionStmt& Section);
			/** `Type Name = Default` for each parameter (`Name = Default` for a template's). */
			TArray<FString> GetParamTexts(const TArray<TUniquePtr<FDstParam>>& Params);
			/** `State Name [ ... ]` or `Subtree Name(params) [ ... ]`, without the `{`. */
			FString StateHead(const FDstStateStmt& State);
			/** The head with its parameters one per line, for a head too wide for one line. */
			FString StateHeadBroken(const FDstStateStmt& State, int32 Level);

			// -- broken forms

			FString FormatExpr(const FDstExpr& Expr, int32 LineBase, int32 Column, int32 SuffixWidth);
			FString FormatOperand(const FDstExpr* Operand, bool bParenthesize, int32 LineBase, int32 Column, int32 SuffixWidth);
			FString FormatLogicalChain(const FDstBinaryExpr& Binary, int32 LineBase, int32 Column, int32 SuffixWidth);
			FString FormatBrokenArguments(const TCHAR* Open, const TCHAR* Close, const TArray<FDstArgumentPtr>& Arguments,
				int32 LineBase, const FDstNode* Owner, bool bHeaderStyle);
			FString FormatBrokenList(const FDstListExpr& List, int32 LineBase);
			FString FormatTaskItem(const FDstTaskItem& Task, int32 LineBase, int32 Column, int32 SuffixWidth);

			// -- statements

			FString FormatStatement(const FDstStmt& Stmt, int32 Level, const FDstAlign& Align);
			FString FormatSection(const FDstSectionStmt& Section, int32 Level, const FDstAlign& Align);
			FString FormatState(const FDstStateStmt& State, int32 Level, const FDstAlign& Align);
			FString FormatBrokenState(const FDstStateStmt& State, int32 Level, const FString& HeadText);
			FString FormatClause(const FDstClauseStmt& Clause, int32 Level);
			FString FormatDoClause(const FDstClauseStmt& Clause, int32 BaseColumn);
			FString FormatTransition(const FDstClauseStmt& Clause, int32 BaseColumn);
			/** Whether the card could print on one line, not counting the width. */
			bool CanBeOneLineCard(const FDstStateStmt& State);
			FDstAlign ComputeAlign(const TArray<const FDstStmt*>& Statements, int32 RunStart, int32 RunEnd);
			bool NeedsDocumentBlank(const FDstStmt& Previous, const FDstStmt& Current, const FString& PreviousText, const FString& CurrentText) const;
			void AppendStatements(FString& Out, const TArray<const FDstStmt*>& Statements, int32 Level, EDstBlankPolicy Policy);
			FString FormatDocumentHeader(const FDstDocument& Document);

			int32 GetIndentColumn(int32 Level) const { return Level * IndentWidth; }

			const FDstPrintOptions& Options;
			/** Where trivia comes from; null when printing without it. */
			const FDstFile* TriviaFile = nullptr;
			int32 IndentWidth = 4;
			TMap<const FDstNode*, bool> BreakCache;
			TMap<const FDstExpr*, FString> FlatCache;
		};

		bool FDstLayoutPrinter::WasBroken(const FDstNode& Node) const
		{
			if (!Options.KeepBrokenIn || Node.Span.IsEmpty()
				|| !(Node.Is<FDstCallExpr>() || Node.Is<FDstListExpr>() || Node.Is<FDstTupleExpr>() || Node.Is<FDstStateStmt>()))
			{
				return false;
			}
			const FString& Source = *Options.KeepBrokenIn;
			const int32 End = FMath::Min(Node.Span.End(), Source.Len());
			for (int32 Index = FMath::Max(Node.Span.Offset, 0); Index < End; ++Index)
			{
				const TCHAR Char = Source[Index];
				if (Char == TEXT('\n') || Char == TEXT('\r'))
				{
					return true;
				}
			}
			return false;
		}

		bool FDstLayoutPrinter::ForcesBreak(const FDstNode& Node)
		{
			if (const bool* Cached = BreakCache.Find(&Node))
			{
				return *Cached;
			}

			bool bBreak = Node.Is<FDstRegionStmt>() || HasInnerComments(&Node) || WasBroken(Node);
			if (!bBreak)
			{
				// Metadata always prints on one line, so nothing inside it can force its owner to break.
				VisitChildren(Node, [this, &bBreak](const FDstNode& Child)
				{
					if (!bBreak && Child.Kind != EDstNodeKind::Meta && (HasOwnComments(&Child) || ForcesBreak(Child)))
					{
						bBreak = true;
					}
				});
			}

			BreakCache.Add(&Node, bBreak);
			return bBreak;
		}

		// ------------------------------------------------------------------------------------------
		// One-line forms
		// ------------------------------------------------------------------------------------------

		FString FDstLayoutPrinter::Flat(const FDstExpr& Expr)
		{
			// Every fit test asks for the one-line width of the subtree it stands on; caching keeps that linear.
			if (const FString* Cached = FlatCache.Find(&Expr))
			{
				return *Cached;
			}
			FString Text = BuildFlat(Expr);
			FlatCache.Add(&Expr, Text);
			return Text;
		}

		FString FDstLayoutPrinter::FlatOperand(const FDstExpr* Operand, bool bParenthesize)
		{
			if (!Operand)
			{
				return FString();
			}
			return bParenthesize ? FString(TEXT("(")) + Flat(*Operand) + TEXT(")") : Flat(*Operand);
		}

		FString FDstLayoutPrinter::FlatArgument(const FDstArgument& Argument, bool bHeaderStyle)
		{
			if (!Argument.IsNamed())
			{
				return FlatOrEmpty(Argument.Value.Get());
			}
			const TCHAR* Separator = Argument.bOutput ? TEXT(" -> ") : (bHeaderStyle ? TEXT("=") : TEXT(" = "));
			return NameText(Argument.Name) + Separator + FlatOrEmpty(Argument.Value.Get());
		}

		FString FDstLayoutPrinter::FlatArguments(const TArray<FDstArgumentPtr>& Arguments, bool bHeaderStyle)
		{
			TArray<FString> Parts;
			Parts.Reserve(Arguments.Num());
			for (const FDstArgumentPtr& Argument : Arguments)
			{
				if (Argument)
				{
					Parts.Add(FlatArgument(*Argument, bHeaderStyle));
				}
			}
			return FString::Join(Parts, TEXT(", "));
		}

		FString FDstLayoutPrinter::FlatMeta(const FDstMeta& Meta)
		{
			TArray<FString> Parts;
			Parts.Reserve(Meta.Entries.Num());
			for (const TUniquePtr<FDstMetaEntry>& Entry : Meta.Entries)
			{
				if (!Entry)
				{
					continue;
				}
				FString Part = NameText(Entry->Key);
				if (Entry->Value)
				{
					Part += TEXT(" = ");
					Part += Flat(*Entry->Value);
				}
				Parts.Add(MoveTemp(Part));
			}
			if (Parts.Num() == 0)
			{
				return TEXT("[]");
			}
			return FString(TEXT("[ ")) + FString::Join(Parts, TEXT("; ")) + TEXT(" ]");
		}

		TArray<FString> FDstLayoutPrinter::GetParamTexts(const TArray<TUniquePtr<FDstParam>>& Params)
		{
			TArray<FString> Parts;
			Parts.Reserve(Params.Num());
			for (const TUniquePtr<FDstParam>& Param : Params)
			{
				if (!Param)
				{
					continue;
				}
				FString Part;
				if (!Param->TypeName.IsEmpty())
				{
					Part = GetTypeText(Param->TypeName) + TEXT(" ");
				}
				Part += NameText(Param->Name);
				if (Param->Default)
				{
					Part += TEXT(" = ");
					Part += Flat(*Param->Default);
				}
				Parts.Add(MoveTemp(Part));
			}
			return Parts;
		}

		FString FDstLayoutPrinter::FlatTaskItem(const FDstTaskItem& Task)
		{
			FString Text;
			if (!Task.BindName.IsEmpty())
			{
				Text = NameText(Task.BindName) + TEXT(" = ");
			}
			Text += FlatOrEmpty(Task.Node.Get());
			return Text;
		}

		FString FDstLayoutPrinter::FlatClause(const FDstClauseStmt& Clause)
		{
			FString Text = GetClauseKeyword(Clause);
			switch (Clause.ClauseKind)
			{
			case EDstClauseKind::Do:
			{
				TArray<FString> Parts;
				for (const TUniquePtr<FDstTaskItem>& Task : Clause.Tasks)
				{
					if (Task)
					{
						Parts.Add(FlatTaskItem(*Task));
					}
				}
				if (Parts.Num() > 0)
				{
					Text += TEXT(" ");
					Text += FString::Join(Parts, TEXT(", "));
				}
				break;
			}
			case EDstClauseKind::Transition:
				if (Clause.Value)
				{
					Text += TEXT(" ");
					Text += Flat(*Clause.Value);
				}
				if (Clause.Trigger)
				{
					Text += TEXT(" on ");
					Text += Flat(*Clause.Trigger);
				}
				if (Clause.Condition)
				{
					Text += TEXT(" if ");
					Text += Flat(*Clause.Condition);
				}
				Text += MetaSuffix(Clause.Meta.Get());
				break;
			default:
				if (Clause.Value)
				{
					Text += TEXT(" ");
					Text += Flat(*Clause.Value);
				}
				break;
			}
			return Text;
		}

		FString FDstLayoutPrinter::FlatEntry(const FDstStmt& Stmt)
		{
			switch (Stmt.Kind)
			{
			case EDstNodeKind::AssignStmt:
			{
				const FDstAssignStmt& Assign = static_cast<const FDstAssignStmt&>(Stmt);
				return NameText(Assign.Name) + TEXT(" = ") + FlatOrEmpty(Assign.Value.Get()) + MetaSuffix(Assign.Meta.Get()) + TEXT(";");
			}
			case EDstNodeKind::DeclStmt:
			{
				const FDstDeclStmt& Decl = static_cast<const FDstDeclStmt&>(Stmt);
				FString Text = GetTypeText(Decl.TypeName) + TEXT(" ") + NameText(Decl.Name);
				if (Decl.Value)
				{
					Text += TEXT(" = ");
					Text += Flat(*Decl.Value);
				}
				Text += MetaSuffix(Decl.Meta.Get());
				Text += TEXT(";");
				return Text;
			}
			case EDstNodeKind::ExprStmt:
				return FlatOrEmpty(static_cast<const FDstExprStmt&>(Stmt).Expr.Get()) + TEXT(";");
			case EDstNodeKind::RegionStmt:
				// Never reached through a fitting section (a region forces it to break); kept total.
				return GetRegionText(static_cast<const FDstRegionStmt&>(Stmt));
			default:
				return FString();
			}
		}

		FString FDstLayoutPrinter::FlatSection(const FDstSectionStmt& Section)
		{
			TArray<FString> Parts;
			Parts.Reserve(Section.Entries.Num());
			for (const FDstStmtPtr& Entry : Section.Entries)
			{
				if (Entry)
				{
					Parts.Add(FlatEntry(*Entry));
				}
			}
			return Parts.Num() == 0 ? FString(TEXT("{}")) : FString(TEXT("{ ")) + FString::Join(Parts, TEXT(" ")) + TEXT(" }");
		}

		FString FDstLayoutPrinter::StateHead(const FDstStateStmt& State)
		{
			FString Head = State.bSubtree ? FString(TEXT("Subtree ")) : FString(TEXT("State "));
			Head += NameText(State.Name);
			if (State.bHasParams)
			{
				Head += TEXT("(");
				Head += FString::Join(GetParamTexts(State.Params), TEXT(", "));
				Head += TEXT(")");
			}
			Head += MetaSuffix(State.Meta.Get());
			return Head;
		}

		FString FDstLayoutPrinter::StateHeadBroken(const FDstStateStmt& State, int32 Level)
		{
			FString Head = State.bSubtree ? FString(TEXT("Subtree ")) : FString(TEXT("State "));
			Head += NameText(State.Name);

			const TArray<FString> Parts = GetParamTexts(State.Params);
			Head += TEXT("(");
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				Head += TEXT("\n");
				Head += MakeSpaces(GetIndentColumn(Level + 1));
				Head += Parts[Index];
				if (Index + 1 < Parts.Num())
				{
					Head += TEXT(",");
				}
			}
			Head += TEXT("\n");
			Head += MakeSpaces(GetIndentColumn(Level));
			Head += TEXT(")");
			Head += MetaSuffix(State.Meta.Get());
			return Head;
		}

		FString FDstLayoutPrinter::BuildFlat(const FDstExpr& Expr)
		{
			switch (Expr.Kind)
			{
			case EDstNodeKind::LiteralExpr:
				return GetLiteralText(static_cast<const FDstLiteralExpr&>(Expr));
			case EDstNodeKind::NameExpr:
				return GetNameExprText(static_cast<const FDstNameExpr&>(Expr));
			case EDstNodeKind::MemberExpr:
			{
				const FDstMemberExpr& Member = static_cast<const FDstMemberExpr&>(Expr);
				return FlatOperand(Member.Object.Get(), NeedsPostfixParens(Member.Object.Get())) + TEXT(".") + GetMemberText(Member);
			}
			case EDstNodeKind::CallExpr:
			{
				const FDstCallExpr& Call = static_cast<const FDstCallExpr&>(Expr);
				return FlatOperand(Call.Callee.Get(), NeedsPostfixParens(Call.Callee.Get()))
					+ TEXT("(") + FlatArguments(Call.Args, false) + TEXT(")")
					+ MetaSuffix(Call.Meta.Get());
			}
			case EDstNodeKind::ListExpr:
			{
				const FDstListExpr& List = static_cast<const FDstListExpr&>(Expr);
				TArray<FString> Parts;
				Parts.Reserve(List.Items.Num());
				for (const FDstExprPtr& Item : List.Items)
				{
					if (Item)
					{
						Parts.Add(Flat(*Item));
					}
				}
				return FString(TEXT("[")) + FString::Join(Parts, TEXT(", ")) + TEXT("]");
			}
			case EDstNodeKind::TupleExpr:
				return FString(TEXT("(")) + FlatArguments(static_cast<const FDstTupleExpr&>(Expr).Items, false) + TEXT(")");
			case EDstNodeKind::UnaryExpr:
			{
				const FDstUnaryExpr& Unary = static_cast<const FDstUnaryExpr&>(Expr);
				// `not` is a word and needs a gap; `-` hugs its operand.
				const TCHAR* Gap = Unary.Op == EDstUnaryOp::Not ? TEXT(" ") : TEXT("");
				return FString(GetDstUnaryOpSpelling(Unary.Op)) + Gap + FlatOperand(Unary.Operand.Get(), NeedsUnaryParens(Unary.Operand.Get()));
			}
			case EDstNodeKind::BinaryExpr:
			{
				const FDstBinaryExpr& Binary = static_cast<const FDstBinaryExpr&>(Expr);
				return FlatOperand(Binary.Lhs.Get(), NeedsBinaryParens(Binary.Lhs.Get(), Binary.Op, false))
					+ TEXT(" ") + GetDstBinaryOpSpelling(Binary.Op) + TEXT(" ")
					+ FlatOperand(Binary.Rhs.Get(), NeedsBinaryParens(Binary.Rhs.Get(), Binary.Op, true));
			}
			case EDstNodeKind::ParenExpr:
				return FString(TEXT("(")) + FlatOrEmpty(static_cast<const FDstParenExpr&>(Expr).Inner.Get()) + TEXT(")");
			case EDstNodeKind::RefExpr:
			{
				const FDstRefExpr& Ref = static_cast<const FDstRefExpr&>(Expr);
				return FString(TEXT("ref ")) + FlatOperand(Ref.Target.Get(), NeedsUnaryParens(Ref.Target.Get()));
			}
			default:
				return FString();
			}
		}

		// ------------------------------------------------------------------------------------------
		// Broken forms
		// ------------------------------------------------------------------------------------------

		FString FDstLayoutPrinter::FormatExpr(const FDstExpr& Expr, int32 LineBase, int32 Column, int32 SuffixWidth)
		{
			if (!ForcesBreak(Expr))
			{
				const FString FlatText = Flat(Expr);
				if (Column + FlatText.Len() + SuffixWidth <= Options.MaxWidth)
				{
					return FlatText;
				}
			}

			switch (Expr.Kind)
			{
			case EDstNodeKind::CallExpr:
			{
				const FDstCallExpr& Call = static_cast<const FDstCallExpr&>(Expr);
				// `Foo()` has nothing to put on lines of its own.
				if (!HasAnyChild(Call.Args) && !HasInnerComments(&Call))
				{
					return Flat(Expr);
				}
				FString Text = FormatOperand(Call.Callee.Get(), NeedsPostfixParens(Call.Callee.Get()), LineBase, Column, 1);
				Text += FormatBrokenArguments(TEXT("("), TEXT(")"), Call.Args, LineBase, &Call, false);
				Text += MetaSuffix(Call.Meta.Get());
				return Text;
			}
			case EDstNodeKind::ListExpr:
			{
				const FDstListExpr& List = static_cast<const FDstListExpr&>(Expr);
				if (!HasAnyChild(List.Items) && !HasInnerComments(&List))
				{
					return Flat(Expr);
				}
				return FormatBrokenList(List, LineBase);
			}
			case EDstNodeKind::TupleExpr:
			{
				const FDstTupleExpr& Tuple = static_cast<const FDstTupleExpr&>(Expr);
				if (!HasAnyChild(Tuple.Items) && !HasInnerComments(&Tuple))
				{
					return Flat(Expr);
				}
				return FormatBrokenArguments(TEXT("("), TEXT(")"), Tuple.Items, LineBase, &Tuple, false);
			}
			case EDstNodeKind::MemberExpr:
			{
				const FDstMemberExpr& Member = static_cast<const FDstMemberExpr&>(Expr);
				const FString MemberText = GetMemberText(Member);
				FString Text = FormatOperand(Member.Object.Get(), NeedsPostfixParens(Member.Object.Get()), LineBase, Column,
					SuffixWidth + 1 + MemberText.Len());
				Text += TEXT(".");
				Text += MemberText;
				return Text;
			}
			case EDstNodeKind::UnaryExpr:
			{
				const FDstUnaryExpr& Unary = static_cast<const FDstUnaryExpr&>(Expr);
				FString Text = GetDstUnaryOpSpelling(Unary.Op);
				if (Unary.Op == EDstUnaryOp::Not)
				{
					Text += TEXT(" ");
				}
				const int32 OperandColumn = Column + Text.Len();
				Text += FormatOperand(Unary.Operand.Get(), NeedsUnaryParens(Unary.Operand.Get()), LineBase, OperandColumn, SuffixWidth);
				return Text;
			}
			case EDstNodeKind::BinaryExpr:
			{
				const FDstBinaryExpr& Binary = static_cast<const FDstBinaryExpr&>(Expr);
				if ((Binary.Op == EDstBinaryOp::And || Binary.Op == EDstBinaryOp::Or) && Binary.Lhs && Binary.Rhs)
				{
					return FormatLogicalChain(Binary, LineBase, Column, SuffixWidth);
				}
				// Comparisons and arithmetic have no line to break at of their own; only their operands break.
				const FString OpText = FString(TEXT(" ")) + GetDstBinaryOpSpelling(Binary.Op) + TEXT(" ");
				FString Text = FormatOperand(Binary.Lhs.Get(), NeedsBinaryParens(Binary.Lhs.Get(), Binary.Op, false), LineBase, Column, OpText.Len());
				Text += OpText;
				const int32 RhsColumn = GetEndColumn(Text, Column);
				Text += FormatOperand(Binary.Rhs.Get(), NeedsBinaryParens(Binary.Rhs.Get(), Binary.Op, true), LineBase, RhsColumn, SuffixWidth);
				return Text;
			}
			case EDstNodeKind::ParenExpr:
			{
				const FDstParenExpr& Paren = static_cast<const FDstParenExpr&>(Expr);
				return FormatOperand(Paren.Inner.Get(), true, LineBase, Column, SuffixWidth);
			}
			case EDstNodeKind::RefExpr:
			{
				const FDstRefExpr& Ref = static_cast<const FDstRefExpr&>(Expr);
				FString Text = TEXT("ref ");
				const int32 TargetColumn = Column + Text.Len();
				Text += FormatOperand(Ref.Target.Get(), NeedsUnaryParens(Ref.Target.Get()), LineBase, TargetColumn, SuffixWidth);
				return Text;
			}
			default:
				// Names and literals have no line to break at: they run past the width.
				return Flat(Expr);
			}
		}

		FString FDstLayoutPrinter::FormatOperand(const FDstExpr* Operand, bool bParenthesize, int32 LineBase, int32 Column, int32 SuffixWidth)
		{
			if (!Operand)
			{
				return bParenthesize ? FString(TEXT("()")) : FString();
			}
			if (!bParenthesize)
			{
				return FormatExpr(*Operand, LineBase, Column, SuffixWidth);
			}
			return FString(TEXT("(")) + FormatExpr(*Operand, LineBase, Column + 1, SuffixWidth + 1) + TEXT(")");
		}

		FString FDstLayoutPrinter::FormatLogicalChain(const FDstBinaryExpr& Binary, int32 LineBase, int32 Column, int32 SuffixWidth)
		{
			TArray<const FDstExpr*> Operands;
			CollectChain(Binary, Binary.Op, Operands);

			const FString OpWord = GetDstBinaryOpSpelling(Binary.Op);
			// What decides whether the operators break is the width alone: a call inside that has to break (a comment, an
			// authored layout) breaks on its own and leaves the line as it is.
			const bool bFitsFlat = Column + Flat(Binary).Len() + SuffixWidth <= Options.MaxWidth;

			FString Text;
			for (int32 Index = 0; Index < Operands.Num(); ++Index)
			{
				const FDstExpr& Operand = *Operands[Index];
				const bool bParenthesize = NeedsBinaryParens(&Operand, Binary.Op, Index > 0);
				const bool bLast = Index + 1 == Operands.Num();

				int32 OperandBase = LineBase;
				int32 OperandSuffix = 0;
				if (bFitsFlat)
				{
					if (Index > 0)
					{
						Text += TEXT(" ");
						Text += OpWord;
						Text += TEXT(" ");
					}
					OperandSuffix = bLast ? SuffixWidth : OpWord.Len() + 2;
				}
				else
				{
					// One operand per line, the operator leading, every line at the column the chain started at.
					if (Index > 0)
					{
						Text += TEXT("\n");
						Text += MakeSpaces(Column);
						Text += OpWord;
						Text += TEXT(" ");
					}
					OperandBase = Column;
					OperandSuffix = bLast ? SuffixWidth : 0;
				}
				const int32 OperandColumn = GetEndColumn(Text, Column);
				Text += FormatOperand(&Operand, bParenthesize, OperandBase, OperandColumn, OperandSuffix);
			}
			return Text;
		}

		FString FDstLayoutPrinter::FormatBrokenArguments(const TCHAR* Open, const TCHAR* Close, const TArray<FDstArgumentPtr>& Arguments,
			int32 LineBase, const FDstNode* Owner, bool bHeaderStyle)
		{
			const int32 ItemBase = LineBase + IndentWidth;

			// The `=` of every named argument in the list shares one column.
			int32 NameWidth = 0;
			int32 LastIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Arguments.Num(); ++Index)
			{
				if (const FDstArgument* Argument = Arguments[Index].Get())
				{
					LastIndex = Index;
					if (Argument->IsNamed())
					{
						NameWidth = FMath::Max(NameWidth, NameText(Argument->Name).Len());
					}
				}
			}

			FString Text = Open;
			Text += TEXT("\n");
			bool bFirst = true;
			for (int32 Index = 0; Index <= LastIndex; ++Index)
			{
				const FDstArgument* Argument = Arguments[Index].Get();
				if (!Argument)
				{
					continue;
				}
				if (!bFirst && HasBlankLineBefore(Argument))
				{
					Text += TEXT("\n");
				}
				bFirst = false;
				AppendLeadingComments(Text, ItemBase, Argument);

				FString Line = MakeSpaces(ItemBase);
				if (Argument->IsNamed())
				{
					const FString Name = NameText(Argument->Name);
					if (Argument->bOutput)
					{
						Line += PadRight(Name, NameWidth) + TEXT(" -> ");
					}
					else if (bHeaderStyle)
					{
						Line += Name + TEXT("=");
					}
					else
					{
						Line += PadRight(Name, NameWidth) + TEXT(" = ");
					}
				}
				const bool bLast = Index == LastIndex;
				if (Argument->Value)
				{
					const int32 ValueColumn = Line.Len();
					Line += FormatExpr(*Argument->Value, ItemBase, ValueColumn, bLast ? 0 : 1);
				}
				if (!bLast)
				{
					Line += TEXT(",");
				}
				Text += Line;
				AppendTrailingComment(Text, Argument);
				Text += TEXT("\n");
			}
			AppendInnerComments(Text, ItemBase, Owner);
			Text += MakeSpaces(LineBase);
			Text += Close;
			return Text;
		}

		FString FDstLayoutPrinter::FormatBrokenList(const FDstListExpr& List, int32 LineBase)
		{
			const int32 ItemBase = LineBase + IndentWidth;
			int32 LastIndex = INDEX_NONE;
			for (int32 Index = 0; Index < List.Items.Num(); ++Index)
			{
				if (List.Items[Index])
				{
					LastIndex = Index;
				}
			}

			FString Text = TEXT("[\n");
			bool bFirst = true;
			for (int32 Index = 0; Index <= LastIndex; ++Index)
			{
				const FDstExpr* Item = List.Items[Index].Get();
				if (!Item)
				{
					continue;
				}
				if (!bFirst && HasBlankLineBefore(Item))
				{
					Text += TEXT("\n");
				}
				bFirst = false;
				AppendLeadingComments(Text, ItemBase, Item);

				const bool bLast = Index == LastIndex;
				FString Line = MakeSpaces(ItemBase);
				const int32 ItemColumn = Line.Len();
				Line += FormatExpr(*Item, ItemBase, ItemColumn, bLast ? 0 : 1);
				if (!bLast)
				{
					Line += TEXT(",");
				}
				Text += Line;
				AppendTrailingComment(Text, Item);
				Text += TEXT("\n");
			}
			AppendInnerComments(Text, ItemBase, &List);
			Text += MakeSpaces(LineBase);
			Text += TEXT("]");
			return Text;
		}

		FString FDstLayoutPrinter::FormatTaskItem(const FDstTaskItem& Task, int32 LineBase, int32 Column, int32 SuffixWidth)
		{
			FString Text;
			if (!Task.BindName.IsEmpty())
			{
				Text = NameText(Task.BindName) + TEXT(" = ");
			}
			if (Task.Node)
			{
				const int32 NodeColumn = Column + Text.Len();
				Text += FormatExpr(*Task.Node, LineBase, NodeColumn, SuffixWidth);
			}
			return Text;
		}

		// ------------------------------------------------------------------------------------------
		// Clauses
		// ------------------------------------------------------------------------------------------

		FString FDstLayoutPrinter::FormatClause(const FDstClauseStmt& Clause, int32 Level)
		{
			const int32 BaseColumn = GetIndentColumn(Level);
			switch (Clause.ClauseKind)
			{
			case EDstClauseKind::Do:
				return FormatDoClause(Clause, BaseColumn);
			case EDstClauseKind::Transition:
				return FormatTransition(Clause, BaseColumn);
			default:
			{
				FString Text = GetClauseKeyword(Clause);
				if (Clause.Value)
				{
					Text += TEXT(" ");
					const int32 ValueColumn = BaseColumn + Text.Len();
					Text += FormatExpr(*Clause.Value, BaseColumn, ValueColumn, 0);
				}
				return Text;
			}
			}
		}

		FString FDstLayoutPrinter::FormatDoClause(const FDstClauseStmt& Clause, int32 BaseColumn)
		{
			TArray<const FDstTaskItem*> Tasks;
			Tasks.Reserve(Clause.Tasks.Num());
			for (const TUniquePtr<FDstTaskItem>& Task : Clause.Tasks)
			{
				if (Task)
				{
					Tasks.Add(Task.Get());
				}
			}
			if (Tasks.Num() == 0)
			{
				return TEXT("do");
			}

			// The column after `do `; the tasks of a broken clause all start there.
			const int32 ContentColumn = BaseColumn + 3;

			bool bForced = false;
			for (const FDstTaskItem* Task : Tasks)
			{
				if (HasOwnComments(Task) || ForcesBreak(*Task))
				{
					bForced = true;
					break;
				}
			}
			if (!bForced)
			{
				const FString FlatText = FlatClause(Clause);
				if (BaseColumn + FlatText.Len() <= Options.MaxWidth)
				{
					return FlatText;
				}
			}

			// A single task has no comma to break at: its own brackets break instead.
			if (Tasks.Num() == 1 && !HasLeadingComments(Tasks[0]))
			{
				FString Text = TEXT("do ");
				Text += FormatTaskItem(*Tasks[0], BaseColumn, ContentColumn, 0);
				AppendTrailingComment(Text, Tasks[0]);
				return Text;
			}

			// Comments above the first task cannot stay on the `do` line: the tasks start on the next one.
			const bool bTasksOnNewLine = HasLeadingComments(Tasks[0]);
			FString Text = TEXT("do");
			for (int32 Index = 0; Index < Tasks.Num(); ++Index)
			{
				const FDstTaskItem* Task = Tasks[Index];
				if (Index == 0 && !bTasksOnNewLine)
				{
					Text += TEXT(" ");
				}
				else
				{
					Text += TEXT("\n");
					if (Index > 0 && HasBlankLineBefore(Task))
					{
						Text += TEXT("\n");
					}
					AppendLeadingComments(Text, ContentColumn, Task);
					Text += MakeSpaces(ContentColumn);
				}
				const bool bLast = Index + 1 == Tasks.Num();
				Text += FormatTaskItem(*Task, ContentColumn, ContentColumn, bLast ? 0 : 1);
				if (!bLast)
				{
					Text += TEXT(",");
				}
				AppendTrailingComment(Text, Task);
			}
			return Text;
		}

		FString FDstLayoutPrinter::FormatTransition(const FDstClauseStmt& Clause, int32 BaseColumn)
		{
			if (!ForcesBreak(Clause))
			{
				const FString FlatText = FlatClause(Clause);
				if (BaseColumn + FlatText.Len() <= Options.MaxWidth)
				{
					return FlatText;
				}
			}

			// The parts follow one another on the line; only the one that does not fit breaks, and the options (which
			// stay on one line) count toward the fit of the last.
			const FString MetaText = MetaSuffix(Clause.Meta.Get());
			const FDstExpr* LastPart = Clause.Condition ? Clause.Condition.Get() : (Clause.Trigger ? Clause.Trigger.Get() : Clause.Value.Get());

			FString Text = TEXT("->");
			auto AppendPart = [this, &Text, &MetaText, LastPart, BaseColumn](const TCHAR* Gap, const FDstExpr* Part)
			{
				if (!Part)
				{
					return;
				}
				Text += Gap;
				const int32 PartColumn = GetEndColumn(Text, BaseColumn);
				Text += FormatExpr(*Part, BaseColumn, PartColumn, Part == LastPart ? MetaText.Len() : 0);
			};
			AppendPart(TEXT(" "), Clause.Value.Get());
			AppendPart(TEXT(" on "), Clause.Trigger.Get());
			AppendPart(TEXT(" if "), Clause.Condition.Get());
			Text += MetaText;
			return Text;
		}

		// ------------------------------------------------------------------------------------------
		// Statements
		// ------------------------------------------------------------------------------------------

		bool FDstLayoutPrinter::CanBeOneLineCard(const FDstStateStmt& State)
		{
			if (ForcesBreak(State))
			{
				return false;
			}

			// Clauses only, at most three, at most two of them more than a bare `-> Target`; and the blank lines the
			// author left between clauses (not the one after the `{`) keep the card broken.
			const TArray<const FDstStmt*> Ordered = OrderStateBody(State);
			int32 ClauseCount = 0;
			int32 CountedClauses = 0;
			for (int32 Index = 0; Index < Ordered.Num(); ++Index)
			{
				const FDstClauseStmt* Clause = Ordered[Index]->As<FDstClauseStmt>();
				if (!Clause)
				{
					return false;
				}
				if (Index > 0 && HasBlankLineBefore(Clause))
				{
					return false;
				}
				// An `on` or `if` right after a transition on the same line would read as the transition's own.
				if (Index > 0 && (Clause->ClauseKind == EDstClauseKind::On || Clause->ClauseKind == EDstClauseKind::If))
				{
					const FDstClauseStmt* Before = Ordered[Index - 1]->As<FDstClauseStmt>();
					if (Before && Before->ClauseKind == EDstClauseKind::Transition)
					{
						return false;
					}
				}
				++ClauseCount;
				if (!IsBareTransition(*Clause))
				{
					++CountedClauses;
				}
			}
			return ClauseCount <= 3 && CountedClauses <= 2;
		}

		FString FDstLayoutPrinter::FormatState(const FDstStateStmt& State, int32 Level, const FDstAlign& Align)
		{
			const int32 BaseColumn = GetIndentColumn(Level);
			const FString HeadText = StateHead(State);

			if (CanBeOneLineCard(State))
			{
				TArray<FString> Parts;
				for (const FDstStmt* Item : OrderStateBody(State))
				{
					Parts.Add(FlatClause(static_cast<const FDstClauseStmt&>(*Item)));
				}

				FString Line = PadRight(HeadText, Align.HeadWidth);
				Line += Parts.Num() == 0 ? FString(TEXT(" {}")) : FString(TEXT(" { ")) + FString::Join(Parts, TEXT("  ")) + TEXT(" }");
				// A card that only fits unpadded comes back broken here, which takes it out of its run of aligned cards.
				if (BaseColumn + Line.Len() <= Options.MaxWidth)
				{
					return Line;
				}
			}
			return FormatBrokenState(State, Level, HeadText);
		}

		FString FDstLayoutPrinter::FormatBrokenState(const FDstStateStmt& State, int32 Level, const FString& HeadText)
		{
			const int32 BaseColumn = GetIndentColumn(Level);

			FString Text = HeadText;
			// A head too wide for its line takes its parameters one per line.
			if (State.bHasParams && !State.Params.IsEmpty() && BaseColumn + HeadText.Len() + 2 > Options.MaxWidth)
			{
				Text = StateHeadBroken(State, Level);
			}
			Text += TEXT(" {\n");
			AppendStatements(Text, OrderStateBody(State), Level + 1, EDstBlankPolicy::Author);
			AppendInnerComments(Text, GetIndentColumn(Level + 1), &State);
			Text += MakeSpaces(BaseColumn);
			Text += TEXT("}");
			return Text;
		}

		FString FDstLayoutPrinter::FormatSection(const FDstSectionStmt& Section, int32 Level, const FDstAlign& Align)
		{
			const int32 BaseColumn = GetIndentColumn(Level);
			const FString HeadText = PadRight(NameText(Section.Name), Align.KeyWidth) + TEXT(" = ");

			// An empty section is `{}`: there is nothing to put between two lines.
			if (!HasAnyChild(Section.Entries) && !HasInnerComments(&Section))
			{
				return HeadText + TEXT("{}");
			}
			if (!ForcesBreak(Section))
			{
				const FString FlatText = FlatSection(Section);
				if (BaseColumn + HeadText.Len() + FlatText.Len() <= Options.MaxWidth)
				{
					return HeadText + FlatText;
				}
			}

			FString Text = HeadText + TEXT("{\n");
			AppendStatements(Text, GatherStatements(Section.Entries), Level + 1, EDstBlankPolicy::Author);
			AppendInnerComments(Text, GetIndentColumn(Level + 1), &Section);
			Text += MakeSpaces(BaseColumn);
			Text += TEXT("}");
			return Text;
		}

		FString FDstLayoutPrinter::FormatStatement(const FDstStmt& Stmt, int32 Level, const FDstAlign& Align)
		{
			const int32 BaseColumn = GetIndentColumn(Level);

			switch (Stmt.Kind)
			{
			case EDstNodeKind::SectionStmt:
				return FormatSection(static_cast<const FDstSectionStmt&>(Stmt), Level, Align);
			case EDstNodeKind::AssignStmt:
			{
				const FDstAssignStmt& Assign = static_cast<const FDstAssignStmt&>(Stmt);
				const FString MetaText = MetaSuffix(Assign.Meta.Get());

				FString Text = PadRight(NameText(Assign.Name), Align.KeyWidth) + TEXT(" = ");
				if (Assign.Value)
				{
					const int32 ValueColumn = BaseColumn + Text.Len();
					Text += FormatExpr(*Assign.Value, BaseColumn, ValueColumn, MetaText.Len() + 1);
				}
				Text += MetaText;
				Text += TEXT(";");
				return Text;
			}
			case EDstNodeKind::DeclStmt:
			{
				const FDstDeclStmt& Decl = static_cast<const FDstDeclStmt&>(Stmt);
				const FString MetaText = MetaSuffix(Decl.Meta.Get());

				FString Text = PadRight(GetTypeText(Decl.TypeName), Align.TypeWidth) + TEXT(" ");
				if (Decl.Value)
				{
					Text += PadRight(NameText(Decl.Name), Align.NameWidth);
					Text += TEXT(" = ");
					const int32 ValueColumn = BaseColumn + Text.Len();
					Text += FormatExpr(*Decl.Value, BaseColumn, ValueColumn, MetaText.Len() + 1);
				}
				else
				{
					Text += NameText(Decl.Name);
				}
				Text += MetaText;
				Text += TEXT(";");
				return Text;
			}
			case EDstNodeKind::ExprStmt:
			{
				const FDstExprStmt& ExprStmt = static_cast<const FDstExprStmt&>(Stmt);
				FString Text;
				if (ExprStmt.Expr)
				{
					Text += FormatExpr(*ExprStmt.Expr, BaseColumn, BaseColumn, 1);
				}
				Text += TEXT(";");
				return Text;
			}
			case EDstNodeKind::TemplateStmt:
			{
				const FDstTemplateStmt& Template = static_cast<const FDstTemplateStmt&>(Stmt);
				FString Text = FString(TEXT("Template ")) + NameText(Template.Name);
				if (Template.bHasParams)
				{
					Text += TEXT("(");
					Text += FString::Join(GetParamTexts(Template.Params), TEXT(", "));
					Text += TEXT(")");
				}
				if (Template.Body)
				{
					Text += TEXT(" = ");
					const int32 BodyColumn = BaseColumn + Text.Len();
					Text += FormatExpr(*Template.Body, BaseColumn, BodyColumn, 1);
				}
				Text += TEXT(";");
				return Text;
			}
			case EDstNodeKind::StateStmt:
				return FormatState(static_cast<const FDstStateStmt&>(Stmt), Level, Align);
			case EDstNodeKind::ClauseStmt:
				return FormatClause(static_cast<const FDstClauseStmt&>(Stmt), Level);
			case EDstNodeKind::RegionStmt:
				return GetRegionText(static_cast<const FDstRegionStmt&>(Stmt));
			default:
				return FString();
			}
		}

		FDstAlign FDstLayoutPrinter::ComputeAlign(const TArray<const FDstStmt*>& Statements, int32 RunStart, int32 RunEnd)
		{
			FDstAlign Align;
			for (int32 Index = RunStart; Index < RunEnd; ++Index)
			{
				const FDstStmt* Stmt = Statements[Index];
				if (const FDstSectionStmt* Section = Stmt->As<FDstSectionStmt>())
				{
					Align.KeyWidth = FMath::Max(Align.KeyWidth, NameText(Section->Name).Len());
				}
				else if (const FDstAssignStmt* Assign = Stmt->As<FDstAssignStmt>())
				{
					Align.KeyWidth = FMath::Max(Align.KeyWidth, NameText(Assign->Name).Len());
				}
				else if (const FDstDeclStmt* Decl = Stmt->As<FDstDeclStmt>())
				{
					Align.TypeWidth = FMath::Max(Align.TypeWidth, GetTypeText(Decl->TypeName).Len());
					if (Decl->Value)
					{
						Align.NameWidth = FMath::Max(Align.NameWidth, NameText(Decl->Name).Len());
					}
				}
				else if (const FDstStateStmt* State = Stmt->As<FDstStateStmt>())
				{
					Align.HeadWidth = FMath::Max(Align.HeadWidth, StateHead(*State).Len());
				}
			}
			return Align;
		}

		bool FDstLayoutPrinter::NeedsDocumentBlank(const FDstStmt& Previous, const FDstStmt& Current,
			const FString& PreviousText, const FString& CurrentText) const
		{
			// Sections, then templates, then states and subtrees; a region directive belongs to whatever it wraps.
			auto GetGroup = [](const FDstStmt& Stmt)
			{
				switch (Stmt.Kind)
				{
				case EDstNodeKind::SectionStmt:  return 0;
				case EDstNodeKind::TemplateStmt: return 1;
				case EDstNodeKind::StateStmt:    return 2;
				case EDstNodeKind::RegionStmt:   return -1;
				default:                         return 3;
				}
			};
			const int32 PreviousGroup = GetGroup(Previous);
			const int32 CurrentGroup = GetGroup(Current);
			if (PreviousGroup < 0 || CurrentGroup < 0)
			{
				return false;
			}
			// Different kinds are apart; so are top-level states and subtrees, whatever their size.
			if (PreviousGroup != CurrentGroup || CurrentGroup == 2)
			{
				return true;
			}
			// Adjacent sections or templates stay together unless one of them is several lines.
			return IsMultiLine(PreviousText) || IsMultiLine(CurrentText);
		}

		void FDstLayoutPrinter::AppendStatements(FString& Out, const TArray<const FDstStmt*>& Statements, int32 Level, EDstBlankPolicy Policy)
		{
			const int32 Count = Statements.Num();
			const int32 IndentColumn = GetIndentColumn(Level);

			// Each statement on its own terms first; only one-line sections, assignments, declarations and cards can join a run.
			TArray<FString> PlainTexts;
			TArray<bool> AlignableFlags;
			PlainTexts.SetNum(Count);
			AlignableFlags.Init(false, Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				PlainTexts[Index] = FormatStatement(*Statements[Index], Level, FDstAlign());
				AlignableFlags[Index] = IsAlignableKind(Statements[Index]->Kind) && !IsMultiLine(PlainTexts[Index]);
			}

			// The blank lines, the author's and the added ones, settled before the runs: a run never spans one, which is
			// what makes printing the printed text form the same runs. None above the first statement.
			TArray<bool> BlankBefore;
			BlankBefore.Init(false, Count);
			for (int32 Index = 1; Index < Count; ++Index)
			{
				BlankBefore[Index] = HasBlankLineBefore(Statements[Index])
					|| (Policy == EDstBlankPolicy::Document
						&& NeedsDocumentBlank(*Statements[Index - 1], *Statements[Index], PlainTexts[Index - 1], PlainTexts[Index]));
			}
			auto StartsNewRun = [this, &BlankBefore, &Statements](int32 Index)
			{
				return BlankBefore[Index] || HasLeadingComments(Statements[Index]);
			};

			// Lay the runs out; a statement that padding breaks over lines drops out and the runs are formed again.
			// Every pass drops at least one statement or ends, so this terminates.
			TArray<FString> Texts;
			for (;;)
			{
				Texts = PlainTexts;
				bool bDroppedAny = false;
				int32 RunStart = 0;
				while (RunStart < Count)
				{
					if (!AlignableFlags[RunStart])
					{
						++RunStart;
						continue;
					}

					const EDstNodeKind RunKind = Statements[RunStart]->Kind;
					int32 RunEnd = RunStart + 1;
					while (RunEnd < Count
						&& AlignableFlags[RunEnd]
						&& Statements[RunEnd]->Kind == RunKind
						&& !StartsNewRun(RunEnd))
					{
						++RunEnd;
					}

					if (RunEnd - RunStart > 1)
					{
						const FDstAlign Align = ComputeAlign(Statements, RunStart, RunEnd);
						for (int32 Index = RunStart; Index < RunEnd; ++Index)
						{
							FString Aligned = FormatStatement(*Statements[Index], Level, Align);
							if (IsMultiLine(Aligned))
							{
								AlignableFlags[Index] = false;
								bDroppedAny = true;
							}
							else
							{
								Texts[Index] = MoveTemp(Aligned);
							}
						}
					}
					RunStart = RunEnd;
				}

				if (!bDroppedAny)
				{
					break;
				}
			}

			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FDstStmt* Stmt = Statements[Index];
				// One blank line at most, and never right after the opening `{`.
				if (BlankBefore[Index])
				{
					Out += TEXT("\n");
				}
				AppendLeadingComments(Out, IndentColumn, Stmt);
				Out += MakeSpaces(IndentColumn);
				Out += Texts[Index];
				AppendTrailingComment(Out, Stmt);
				Out += TEXT("\n");
			}
		}

		FString FDstLayoutPrinter::FormatDocumentHeader(const FDstDocument& Document)
		{
			const FString KindText = GetDocumentKindText(Document);

			// The header is one line whatever its width; only a comment among its arguments, or an argument the author
			// broke over lines, breaks it.
			bool bBreak = false;
			for (const FDstArgumentPtr& Argument : Document.HeaderArgs)
			{
				if (Argument && (HasOwnComments(Argument.Get()) || (Argument->Value && ForcesBreak(*Argument->Value))))
				{
					bBreak = true;
					break;
				}
			}

			if (!bBreak)
			{
				return KindText + TEXT("(") + FlatArguments(Document.HeaderArgs, true) + TEXT(")");
			}
			return KindText + FormatBrokenArguments(TEXT("("), TEXT(")"), Document.HeaderArgs, 0, nullptr, true);
		}

		FString FDstLayoutPrinter::PrintFile(const FDstFile& File)
		{
			FString Out;

			bool bAnyInclude = false;
			for (const TUniquePtr<FDstInclude>& Include : File.Includes)
			{
				if (!Include)
				{
					continue;
				}
				if (bAnyInclude && HasBlankLineBefore(Include.Get()))
				{
					Out += TEXT("\n");
				}
				bAnyInclude = true;
				AppendLeadingComments(Out, 0, Include.Get());
				Out += TEXT("#include ");
				Out += QuoteString(Include->Path);
				AppendTrailingComment(Out, Include.Get());
				Out += TEXT("\n");
			}

			if (const FDstDocument* Document = File.Document.Get())
			{
				if (bAnyInclude)
				{
					Out += TEXT("\n");
				}
				AppendLeadingComments(Out, 0, Document);
				Out += FormatDocumentHeader(*Document);
				Out += TEXT("\n{\n");
				AppendStatements(Out, GatherStatements(Document->Members), 1, EDstBlankPolicy::Document);
				AppendInnerComments(Out, GetIndentColumn(1), Document);
				Out += TEXT("}");
				AppendTrailingComment(Out, Document);
				Out += TEXT("\n");
			}

			return Out;
		}

		// ------------------------------------------------------------------------------------------
		// Tree dump
		// ------------------------------------------------------------------------------------------

		static const TCHAR* GetClauseKindName(EDstClauseKind Kind)
		{
			switch (Kind)
			{
			case EDstClauseKind::On:         return TEXT("On");
			case EDstClauseKind::If:         return TEXT("If");
			case EDstClauseKind::Pick:       return TEXT("Pick");
			case EDstClauseKind::Utility:    return TEXT("Utility");
			case EDstClauseKind::Link:       return TEXT("Link");
			case EDstClauseKind::Do:         return TEXT("Do");
			case EDstClauseKind::Transition: return TEXT("Transition");
			}
			return TEXT("Unknown");
		}

		/** `Name`, or `Type Name` for a parameter that has a type. */
		static FString GetParamName(const FDstParam& Param)
		{
			const FString Name = QuoteDstName(Param.Name);
			return Param.TypeName.IsEmpty() ? Name : GetTypeText(Param.TypeName) + TEXT(" ") + Name;
		}

		static FString DescribeNode(const FDstNode& Node)
		{
			switch (Node.Kind)
			{
			case EDstNodeKind::LiteralExpr:
			{
				const FDstLiteralExpr& Literal = static_cast<const FDstLiteralExpr&>(Node);
				switch (Literal.LiteralKind)
				{
				case EDstLiteralKind::String: return FString(TEXT("String ")) + QuoteString(Literal.Text);
				case EDstLiteralKind::Bool:   return Literal.bBool ? FString(TEXT("Bool true")) : FString(TEXT("Bool false"));
				default:                      return FString(TEXT("Number ")) + GetNumberText(Literal);
				}
			}
			case EDstNodeKind::NameExpr:
				return FString(TEXT("Name ")) + QuoteDstName(static_cast<const FDstNameExpr&>(Node).Name);
			case EDstNodeKind::MemberExpr:
				return FString(TEXT("Member ")) + QuoteDstName(static_cast<const FDstMemberExpr&>(Node).Member);
			case EDstNodeKind::CallExpr:   return TEXT("Call");
			case EDstNodeKind::ListExpr:   return TEXT("List");
			case EDstNodeKind::TupleExpr:  return TEXT("Tuple");
			case EDstNodeKind::ParenExpr:  return TEXT("Paren");
			case EDstNodeKind::RefExpr:    return TEXT("Ref");
			case EDstNodeKind::UnaryExpr:
				return FString(TEXT("Unary ")) + GetDstUnaryOpSpelling(static_cast<const FDstUnaryExpr&>(Node).Op);
			case EDstNodeKind::BinaryExpr:
				return FString(TEXT("Binary ")) + GetDstBinaryOpSpelling(static_cast<const FDstBinaryExpr&>(Node).Op);
			case EDstNodeKind::SectionStmt:
				return FString(TEXT("Section ")) + QuoteDstName(static_cast<const FDstSectionStmt&>(Node).Name);
			case EDstNodeKind::AssignStmt:
				return FString(TEXT("Assign ")) + QuoteDstName(static_cast<const FDstAssignStmt&>(Node).Name);
			case EDstNodeKind::DeclStmt:
			{
				const FDstDeclStmt& Decl = static_cast<const FDstDeclStmt&>(Node);
				return FString(TEXT("Decl ")) + GetTypeText(Decl.TypeName) + TEXT(" ") + QuoteDstName(Decl.Name);
			}
			case EDstNodeKind::ExprStmt:
				return TEXT("Expr");
			case EDstNodeKind::TemplateStmt:
			{
				// `Template X()` and `Template X` are different trees, so an empty list still shows.
				const FDstTemplateStmt& Template = static_cast<const FDstTemplateStmt&>(Node);
				FString Text = FString(TEXT("Template ")) + QuoteDstName(Template.Name);
				if (Template.bHasParams && Template.Params.IsEmpty())
				{
					Text += TEXT(" ()");
				}
				return Text;
			}
			case EDstNodeKind::StateStmt:
			{
				const FDstStateStmt& State = static_cast<const FDstStateStmt&>(Node);
				FString Text = (State.bSubtree ? FString(TEXT("Subtree ")) : FString(TEXT("State "))) + QuoteDstName(State.Name);
				if (State.bHasParams && State.Params.IsEmpty())
				{
					Text += TEXT(" ()");
				}
				return Text;
			}
			case EDstNodeKind::ClauseStmt:
			{
				const FDstClauseStmt& Clause = static_cast<const FDstClauseStmt&>(Node);
				FString Text = FString(TEXT("Clause ")) + GetClauseKindName(Clause.ClauseKind);
				if (Clause.ClauseKind == EDstClauseKind::Link && Clause.bLinkAsset)
				{
					Text += TEXT(" asset");
				}
				return Text;
			}
			case EDstNodeKind::RegionStmt:
			{
				const FDstRegionStmt& Region = static_cast<const FDstRegionStmt&>(Node);
				return Region.bEnd ? FString(TEXT("EndRegion")) : FString(TEXT("Region ")) + QuoteString(Region.Label);
			}
			case EDstNodeKind::Argument:
			{
				const FDstArgument& Argument = static_cast<const FDstArgument&>(Node);
				if (!Argument.IsNamed())
				{
					return TEXT("Arg");
				}
				return FString(TEXT("Arg ")) + QuoteDstName(Argument.Name) + (Argument.bOutput ? TEXT(" ->") : TEXT(""));
			}
			case EDstNodeKind::MetaEntry:
				return FString(TEXT("MetaEntry ")) + QuoteDstName(static_cast<const FDstMetaEntry&>(Node).Key);
			case EDstNodeKind::Meta:
				return TEXT("Meta");
			case EDstNodeKind::Param:
				return FString(TEXT("Param ")) + GetParamName(static_cast<const FDstParam&>(Node));
			case EDstNodeKind::TaskItem:
			{
				const FDstTaskItem& Task = static_cast<const FDstTaskItem&>(Node);
				return Task.BindName.IsEmpty() ? FString(TEXT("Task")) : FString(TEXT("Task ")) + QuoteDstName(Task.BindName);
			}
			case EDstNodeKind::Include:
				return FString(TEXT("Include ")) + QuoteString(static_cast<const FDstInclude&>(Node).Path);
			case EDstNodeKind::Document:
				return FString(TEXT("Document ")) + GetDocumentKindText(static_cast<const FDstDocument&>(Node));
			}
			return LexToString(Node.Kind);
		}

		static void DumpNode(const FDstNode& Node, int32 Depth, FString& Out)
		{
			Out += MakeSpaces(Depth * 2);
			Out += DescribeNode(Node);
			Out += TEXT("\n");

			// A transition shows its trigger and its condition under a line of their own: both are optional and both are
			// expressions, so without the label a tree with only a condition would read like one with only a trigger.
			const FDstClauseStmt* Clause = Node.As<FDstClauseStmt>();
			if (Clause && Clause->ClauseKind == EDstClauseKind::Transition)
			{
				if (Clause->Value)
				{
					DumpNode(*Clause->Value, Depth + 1, Out);
				}
				if (Clause->Trigger)
				{
					Out += MakeSpaces((Depth + 1) * 2);
					Out += TEXT("On\n");
					DumpNode(*Clause->Trigger, Depth + 2, Out);
				}
				if (Clause->Condition)
				{
					Out += MakeSpaces((Depth + 1) * 2);
					Out += TEXT("If\n");
					DumpNode(*Clause->Condition, Depth + 2, Out);
				}
				if (Clause->Meta)
				{
					DumpNode(*Clause->Meta, Depth + 1, Out);
				}
				return;
			}

			VisitChildren(Node, [Depth, &Out](const FDstNode& Child)
			{
				DumpNode(Child, Depth + 1, Out);
			});
		}
	}

	FString QuoteDstName(const FString& Name)
	{
		if (Name.IsEmpty() || IsDstPlainIdentifier(Name))
		{
			return Name;
		}
		return FString(TEXT("`")) + Name + TEXT("`");
	}

	FString PrintDst(const FDstFile& File, const FDstPrintOptions& Options)
	{
		DstPrinterPrivate::FDstLayoutPrinter Printer(Options, &File);
		return Printer.ToOutputLineEnds(Printer.PrintFile(File));
	}

	FString PrintDstExpr(const FDstExpr& Expr, int32 IndentLevel, int32 StartColumn, const FDstFile* File, const FDstPrintOptions& Options)
	{
		DstPrinterPrivate::FDstLayoutPrinter Printer(Options, File);
		return Printer.ToOutputLineEnds(Printer.FormatExprAtLevel(Expr, FMath::Max(IndentLevel, 0), FMath::Max(StartColumn, 0)));
	}

	FString PrintDstStmt(const FDstStmt& Stmt, int32 IndentLevel, const FDstFile* File, const FDstPrintOptions& Options)
	{
		DstPrinterPrivate::FDstLayoutPrinter Printer(Options, File);
		return Printer.ToOutputLineEnds(Printer.FormatStatementAlone(Stmt, FMath::Max(IndentLevel, 0)));
	}

	FString DumpDstTree(const FDstFile& File)
	{
		FString Out;
		for (const TUniquePtr<FDstInclude>& Include : File.Includes)
		{
			if (Include)
			{
				DstPrinterPrivate::DumpNode(*Include, 0, Out);
			}
		}
		if (File.Document)
		{
			DstPrinterPrivate::DumpNode(*File.Document, 0, Out);
		}
		return Out;
	}
}
