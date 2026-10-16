// Copyright DreamMoon. All Rights Reserved.
//
// The syntax tree. It records what was written and where, nothing more: whether `Delay(1, 0.3)` names a task, a
// condition or a template, and whether `Hit` is a tag or a state, is decided by later stages. The grammar is in
// Docs/Grammar.md; the shape of a file is
//
//   #include "Common.dth"
//   StateTree(Name="AI/ST_Brawler", Schema=AIComponent(Actor = CombatEnemy))      <- Document, HeaderArgs
//   {
//       Parameters = { float AttackRange = 200; FVector WanderSpot; }              <- Section, Decl entries
//       Globals    = { Info = GetPlayerInfo(MaxRange = 2500); }                     <- Section, Assign entries
//       Tags       = { Game.Event.Villager; }                                       <- Section, Expr entries
//       Template Nervous = Chance(1 - Params.Courage);                              <- Template
//       State Root {                                                                <- State
//           -> Root                                                                 <- Clause (Transition)
//           State Attack [ Tag = Combat ] {
//               if ObjectIsValid(Info.Target) and Info.Distance < Params.AttackRange   <- Clause (If)
//               do Combo = ComboAttack() [ Name = "Combo" ], FaceActor(Info.Target)  <- Clause (Do), two task items
//               -> Recover on Succeeded if not Tired() [ Delay = 0.5 ]              <- Clause (Transition)
//           }
//       }
//       Subtree HitReaction(float Toughness = 1) { ... }                            <- State (subtree), Params
//   }
//
// Clauses are not separated by anything: a clause ends where the next clause keyword (`on`, `if`, `pick`,
// `utility`, `link`, `do`, `->`), a `State`, a `#Region`/`#EndRegion` or the closing `}` begins. Line breaks carry
// no meaning.
//
// Ownership: every child is a TUniquePtr held by its parent; nodes are not copyable (the Clone functions make deep
// copies). Node addresses are stable for the life of the FDstFile, which is what the trivia and origin side tables
// key on.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstSource.h"

namespace UE::DreamStateTree::Lang
{
	enum class EDstNodeKind : uint8
	{
		// expressions
		LiteralExpr,
		NameExpr,
		MemberExpr,
		CallExpr,
		ListExpr,
		TupleExpr,
		UnaryExpr,
		BinaryExpr,
		ParenExpr,
		RefExpr,

		// statements
		SectionStmt,
		AssignStmt,
		DeclStmt,
		ExprStmt,
		TemplateStmt,
		StateStmt,
		ClauseStmt,
		RegionStmt,

		// pieces
		Argument,
		MetaEntry,
		Meta,
		Param,
		TaskItem,

		// file level
		Include,
		Document,
	};

	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstNodeKind Kind);

	struct FDstNode
	{
		EDstNodeKind Kind;
		/** The whole construct as written. */
		FDstSpan Span;

		explicit FDstNode(EDstNodeKind InKind) : Kind(InKind) {}
		virtual ~FDstNode() = default;

		FDstNode(const FDstNode&) = delete;
		FDstNode& operator=(const FDstNode&) = delete;

		template <typename T> T* As() { return Kind == T::StaticKind ? static_cast<T*>(this) : nullptr; }
		template <typename T> const T* As() const { return Kind == T::StaticKind ? static_cast<const T*>(this) : nullptr; }
		template <typename T> bool Is() const { return Kind == T::StaticKind; }
	};

	struct FDstMeta;

	// ------------------------------------------------------------------------------------------
	// Expressions
	// ------------------------------------------------------------------------------------------

	struct FDstExpr : FDstNode
	{
		using FDstNode::FDstNode;
	};
	using FDstExprPtr = TUniquePtr<FDstExpr>;

	struct FDstStmt;
	using FDstStmtPtr = TUniquePtr<FDstStmt>;

	enum class EDstLiteralKind : uint8 { Number, String, Bool };

	struct FDstLiteralExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::LiteralExpr;
		FDstLiteralExpr() : FDstExpr(StaticKind) {}

		EDstLiteralKind LiteralKind = EDstLiteralKind::Number;
		/** Numbers: the lexeme as written (printed back verbatim). Strings: the value, escapes resolved. Bools: `true` / `false`. */
		FString Text;
		double Number = 0.0;
		bool bBool = false;
	};

	/** A bare identifier, or a name in backticks: `Params`, `Root`, `None`, `HighestUtility`, `` `Move To` ``. */
	struct FDstNameExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::NameExpr;
		FDstNameExpr() : FDstExpr(StaticKind) {}

		FString Name;
		/** Written between backticks. */
		bool bQuoted = false;
	};

	/** `Object.Member`. `Info.Target.Location` is Member(Member(Name Info, Target), Location); a tag `Game.Event.Hit` too. */
	struct FDstMemberExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::MemberExpr;
		FDstMemberExpr() : FDstExpr(StaticKind) {}

		FDstExprPtr Object;
		FString Member;
		FDstSpan MemberSpan;
		bool bMemberQuoted = false;
	};

	/**
	 * One argument of a call or one item of a tuple:
	 *   `Value`                  positional
	 *   `Name = Value`           named
	 *   `Name -> Path`           an output binding: the node's property Name is copied into Path (bOutput)
	 */
	struct FDstArgument final : FDstNode
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::Argument;
		FDstArgument() : FDstNode(StaticKind) {}

		/** Empty for a positional argument. */
		FString Name;
		FDstSpan NameSpan;
		FDstExprPtr Value;
		/** Written `Name -> Value`. */
		bool bOutput = false;

		bool IsNamed() const { return !Name.IsEmpty(); }
	};
	using FDstArgumentPtr = TUniquePtr<FDstArgument>;

	/**
	 * `Callee(Args...)`, optionally followed by node settings: `Combo(...) [ Name = "Combo"; Enabled = false ]`.
	 * Callee is a NameExpr (`Delay`) or a MemberExpr (`Person.Pace`).
	 */
	struct FDstCallExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::CallExpr;
		DREAMSTATETREELANG_API FDstCallExpr();
		DREAMSTATETREELANG_API virtual ~FDstCallExpr() override;

		FDstExprPtr Callee;
		TArray<FDstArgumentPtr> Args;
		/** `(` to `)` inclusive. */
		FDstSpan ArgsSpan;
		/** Null when no `[ ... ]` follows. */
		TUniquePtr<FDstMeta> Meta;
	};

	/** `[a, b, c]`. */
	struct FDstListExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::ListExpr;
		FDstListExpr() : FDstExpr(StaticKind) {}

		TArray<FDstExprPtr> Items;
	};

	/** `(a, b)` or `(Min = 0, Max = 1)`. A single unnamed item in parentheses is a ParenExpr instead. */
	struct FDstTupleExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::TupleExpr;
		FDstTupleExpr() : FDstExpr(StaticKind) {}

		TArray<FDstArgumentPtr> Items;
	};

	enum class EDstUnaryOp : uint8 { Not, Negate };
	/** `not` and `-`. */
	DREAMSTATETREELANG_API const TCHAR* GetDstUnaryOpSpelling(EDstUnaryOp Op);

	struct FDstUnaryExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::UnaryExpr;
		FDstUnaryExpr() : FDstExpr(StaticKind) {}

		EDstUnaryOp Op = EDstUnaryOp::Not;
		FDstExprPtr Operand;
	};

	enum class EDstBinaryOp : uint8
	{
		Or, And,
		Equal, NotEqual,
		Less, LessEqual, Greater, GreaterEqual,
		Add, Subtract,
		Multiply, Divide,
	};
	/** `or`, `and`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `+`, `-`, `*`, `/`. */
	DREAMSTATETREELANG_API const TCHAR* GetDstBinaryOpSpelling(EDstBinaryOp Op);
	/** Higher binds tighter: or 1, and 2, equality 3, comparison 4, additive 5, multiplicative 6. `not` binds tighter than all of them. */
	DREAMSTATETREELANG_API int32 GetDstBinaryOpPrecedence(EDstBinaryOp Op);

	struct FDstBinaryExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::BinaryExpr;
		FDstBinaryExpr() : FDstExpr(StaticKind) {}

		EDstBinaryOp Op = EDstBinaryOp::And;
		FDstExprPtr Lhs;
		FDstExprPtr Rhs;
		FDstSpan OpSpan;
	};

	/** Parentheses the author wrote around an expression. Kept so the printer gives them back. */
	struct FDstParenExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::ParenExpr;
		FDstParenExpr() : FDstExpr(StaticKind) {}

		FDstExprPtr Inner;
	};

	/** `ref Params.WanderSpot`: a property reference the node writes through. */
	struct FDstRefExpr final : FDstExpr
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::RefExpr;
		FDstRefExpr() : FDstExpr(StaticKind) {}

		FDstExprPtr Target;
	};

	// ------------------------------------------------------------------------------------------
	// Metadata
	// ------------------------------------------------------------------------------------------

	/** `Key = Value` or a bare `Flag` inside `[ ... ]`. */
	struct FDstMetaEntry final : FDstNode
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::MetaEntry;
		FDstMetaEntry() : FDstNode(StaticKind) {}

		FString Key;
		FDstSpan KeySpan;
		/** Null for a flag. */
		FDstExprPtr Value;
	};

	/** `[ Priority = High; Delay = 0.5 ]`, `[ Group ]`. Entries are separated by `;`. */
	struct FDstMeta final : FDstNode
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::Meta;
		FDstMeta() : FDstNode(StaticKind) {}

		TArray<TUniquePtr<FDstMetaEntry>> Entries;

		/** The entry with this key (case-sensitive), or null. */
		DREAMSTATETREELANG_API const FDstMetaEntry* Find(const TCHAR* Key) const;
	};

	// ------------------------------------------------------------------------------------------
	// Statements
	// ------------------------------------------------------------------------------------------

	struct FDstStmt : FDstNode
	{
		using FDstNode::FDstNode;
	};

	/** `Name = { entries }` at document level: Settings, Parameters, Globals, Tags, Colors (which ones are allowed is checked later). */
	struct FDstSectionStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::SectionStmt;
		FDstSectionStmt() : FDstStmt(StaticKind) {}

		FString Name;
		FDstSpan NameSpan;
		/** The `{` and the `}`; CloseSpan is empty when the section is not closed. */
		FDstSpan OpenSpan;
		FDstSpan CloseSpan;
		/** Assign, Decl, Expr and Region statements, in source order. */
		TArray<FDstStmtPtr> Entries;
	};

	/** `Key = Value [meta];` inside a section: a setting, a global node (`Info = GetPlayerInfo(...)`), a color. */
	struct FDstAssignStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::AssignStmt;
		FDstAssignStmt() : FDstStmt(StaticKind) {}

		FString Name;
		FDstSpan NameSpan;
		FDstExprPtr Value;
		/** Null when none was written. */
		TUniquePtr<FDstMeta> Meta;
	};

	/** `Type Name [= Value] [meta];` inside a section: a parameter. Type is as written, `Array<float>` and `Class<Actor>` included. */
	struct FDstDeclStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::DeclStmt;
		FDstDeclStmt() : FDstStmt(StaticKind) {}

		FString TypeName;
		FDstSpan TypeSpan;
		FString Name;
		FDstSpan NameSpan;
		/** Null when no `= Value` was written. */
		FDstExprPtr Value;
		TUniquePtr<FDstMeta> Meta;
	};

	/** `Value;` inside a section: a tag prefix in `Tags = { Game.Event.Villager; }`. */
	struct FDstExprStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::ExprStmt;
		FDstExprStmt() : FDstStmt(StaticKind) {}

		FDstExprPtr Expr;
	};

	/** A parameter of a template (`From`, `Rate = 1`) or of a subtree (`float Toughness = 1`). */
	struct FDstParam final : FDstNode
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::Param;
		FDstParam() : FDstNode(StaticKind) {}

		/** Empty for a template parameter. As written, `Array<float>` included. */
		FString TypeName;
		FDstSpan TypeSpan;
		FString Name;
		FDstSpan NameSpan;
		/** Null when the parameter has no default. */
		FDstExprPtr Default;
	};

	/** `Template Name(params) = Body;` at document level, in a tree or a library. */
	struct FDstTemplateStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::TemplateStmt;
		FDstTemplateStmt() : FDstStmt(StaticKind) {}

		FString Name;
		FDstSpan NameSpan;
		/** `Template X = ...` has none; bHasParams tells it from `Template X() = ...`. */
		bool bHasParams = false;
		TArray<TUniquePtr<FDstParam>> Params;
		FDstExprPtr Body;
	};

	/**
	 * A state card or a subtree:
	 *   `State Name [meta] { body }`
	 *   `Subtree Name(params) [meta] { body }`
	 * The body holds clauses, child states and regions in source order. The parser does not reorder them;
	 * the printer puts clauses before child states.
	 */
	struct FDstStateStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::StateStmt;
		DREAMSTATETREELANG_API FDstStateStmt();
		DREAMSTATETREELANG_API virtual ~FDstStateStmt() override;

		/** Written `Subtree`. */
		bool bSubtree = false;
		FDstSpan WordSpan;
		FString Name;
		FDstSpan NameSpan;
		bool bNameQuoted = false;
		bool bHasParams = false;
		TArray<TUniquePtr<FDstParam>> Params;
		TUniquePtr<FDstMeta> Meta;
		TArray<FDstStmtPtr> Body;
		/** The `{` and the `}`. */
		FDstSpan OpenSpan;
		FDstSpan CloseSpan;
	};

	/** One task of a `do` clause: `Combo = ComboAttack() [ Name = "Combo" ]` or `Wait(8)`. */
	struct FDstTaskItem final : FDstNode
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::TaskItem;
		FDstTaskItem() : FDstNode(StaticKind) {}

		/** The name other nodes bind to (`Combo.Succeeds`); empty when none was written. */
		FString BindName;
		FDstSpan BindNameSpan;
		/** A CallExpr (`Wait(8)`, with its settings) or a NameExpr / MemberExpr for a node written without parentheses. */
		FDstExprPtr Node;
	};

	enum class EDstClauseKind : uint8
	{
		/** `on Tag` / `on Tag(Payload)`. */
		On,
		/** `if Condition`. */
		If,
		/** `pick Mode`. */
		Pick,
		/** `utility Expression`. */
		Utility,
		/** `link Subtree(args)` / `link asset Asset(args)`. */
		Link,
		/** `do Task, Task`. */
		Do,
		/** `-> Target [on Trigger] [if Condition] [ options ]`. */
		Transition,
	};
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstClauseKind Kind);

	struct FDstClauseStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::ClauseStmt;
		DREAMSTATETREELANG_API FDstClauseStmt();
		DREAMSTATETREELANG_API virtual ~FDstClauseStmt() override;

		EDstClauseKind ClauseKind = EDstClauseKind::If;
		/** The keyword (`on`, `if`, ...) or the `->`. */
		FDstSpan KeywordSpan;

		/**
		 * On: the event (`Hit`, `Game.Event.Hit`, `"Game.Event.Hit"`, or a call `Hit(PayloadType)`).
		 * If: the condition. Pick: a NameExpr. Utility: the expression.
		 * Link: the target (`HitReaction`, `HitReaction(Toughness = 2)`, `ST_Other(...)`).
		 * Transition: the target (`Root`, `Next`, `Idle.Phone`, `` `Move To` ``).
		 */
		FDstExprPtr Value;

		/** Link only: written `link asset`. */
		bool bLinkAsset = false;
		FDstSpan AssetSpan;

		/** Do only. */
		TArray<TUniquePtr<FDstTaskItem>> Tasks;

		/** Transition only: what follows `on` (`Succeeded`, `Tick`, a tag, `Hit(Payload)`, `Delegate(Combo.Succeeds)`); null when none. */
		FDstExprPtr Trigger;
		FDstSpan OnSpan;
		/** Transition only: what follows `if`; null when none. */
		FDstExprPtr Condition;
		FDstSpan IfSpan;
		/** Transition only: `[ Priority = High; Delay = 1 ]`; null when none. */
		TUniquePtr<FDstMeta> Meta;
	};

	/** `#Region "Label"` or `#EndRegion`. Allowed in the document, in sections and in state bodies. */
	struct FDstRegionStmt final : FDstStmt
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::RegionStmt;
		FDstRegionStmt() : FDstStmt(StaticKind) {}

		bool bEnd = false;
		/** Empty for #EndRegion. */
		FString Label;
	};

	// ------------------------------------------------------------------------------------------
	// File
	// ------------------------------------------------------------------------------------------

	/** `#include "Common.dth"`. */
	struct FDstInclude final : FDstNode
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::Include;
		FDstInclude() : FDstNode(StaticKind) {}

		FString Path;
		FDstSpan PathSpan;
	};

	enum class EDstDocumentKind : uint8 { StateTree, StateTreeLibrary, Unknown };
	DREAMSTATETREELANG_API const TCHAR* LexToString(EDstDocumentKind Kind);

	/** `StateTree(Name="...", Root="...", Schema=...) { members }` or `StateTreeLibrary(Name="...") { members }`. */
	struct FDstDocument final : FDstNode
	{
		static constexpr EDstNodeKind StaticKind = EDstNodeKind::Document;
		FDstDocument() : FDstNode(StaticKind) {}

		EDstDocumentKind DocumentKind = EDstDocumentKind::Unknown;
		/** As written. */
		FString KindName;
		FDstSpan KindSpan;
		TArray<FDstArgumentPtr> HeaderArgs;
		/** Sections, templates, states, subtrees and regions, in source order. */
		TArray<FDstStmtPtr> Members;
		FDstSpan OpenSpan;
		FDstSpan CloseSpan;

		/** The value of a header argument written as a string literal (`Name`, `Root`); empty when absent. */
		DREAMSTATETREELANG_API FString GetHeaderString(const TCHAR* ArgName) const;
		/** The header argument with this name, or null. */
		DREAMSTATETREELANG_API const FDstArgument* FindHeaderArg(const TCHAR* ArgName) const;
	};

	/** One `//` or block comment kept as trivia. */
	struct FDstComment
	{
		/** Verbatim, delimiters included. */
		FString Text;
		FDstSpan Span;
		bool bBlock = false;
	};

	/**
	 * Comments and blank lines around one node, kept when the parse asked for them. Recorded for statements
	 * (members, section entries, clauses, states), task items, arguments, list and tuple items, and includes.
	 */
	struct FDstTrivia
	{
		/** Own-line comments above the node, in source order. */
		TArray<FDstComment> Leading;
		/** Blank lines above the node's first leading comment (or the node itself), from the previous sibling or the opening bracket. */
		int32 BlankLinesBefore = 0;
		/** A comment that starts on the node's last line, after it (after its `;` or `,`). */
		TOptional<FDstComment> Trailing;
		/** For a document, section, state, call, list or tuple: comments after its last child, before the closing bracket. */
		TArray<FDstComment> Inner;
	};

	/** Where an expression came from when template expansion produced it. */
	struct FDstExpansionOrigin
	{
		FString TemplateName;
		/** The template call that was replaced, in the file being compiled. */
		FDstSpan CallSpan;
		/** The template's definition, and the file it is in. */
		FDstSpan DefinitionSpan;
		FString DefinitionFile;
	};

	struct FDstFile
	{
		FString Path;
		TArray<TUniquePtr<FDstInclude>> Includes;
		/** Null when the file had no document (DST2002). */
		TUniquePtr<FDstDocument> Document;

		/** Filled only when the parse kept trivia. */
		TMap<const FDstNode*, FDstTrivia> Trivia;
		/** Filled by template expansion: expression nodes a template call became, keyed by the root of each expansion. */
		TMap<const FDstNode*, FDstExpansionOrigin> Origins;

		const FDstTrivia* FindTrivia(const FDstNode* Node) const { return Trivia.Find(Node); }
	};

	// ------------------------------------------------------------------------------------------
	// Utilities (DstAst.cpp, DstInteger.cpp)
	// ------------------------------------------------------------------------------------------

	/** Deep copies. Spans are copied as they are; trivia is not (it lives in the file's side table). */
	DREAMSTATETREELANG_API FDstExprPtr CloneExpr(const FDstExpr& Expr);
	DREAMSTATETREELANG_API FDstStmtPtr CloneStmt(const FDstStmt& Stmt);
	DREAMSTATETREELANG_API TUniquePtr<FDstMeta> CloneMeta(const FDstMeta& Meta);
	DREAMSTATETREELANG_API FDstArgumentPtr CloneArgument(const FDstArgument& Argument);
	DREAMSTATETREELANG_API TUniquePtr<FDstParam> CloneParam(const FDstParam& Param);
	DREAMSTATETREELANG_API TUniquePtr<FDstTaskItem> CloneTaskItem(const FDstTaskItem& Item);

	/**
	 * The whole number a constant spells (`-5`, `(3)`, `1e3`, `2.0`), read from the literal's digits so int64 values
	 * above 2^53 come out exact; false for a fraction, a value outside int64 or anything else.
	 */
	DREAMSTATETREELANG_API bool TryGetDstInteger(const FDstExpr& Expr, int64& OutValue);
	/** An integer literal that keeps every digit of Value, under a unary minus when it is negative. */
	DREAMSTATETREELANG_API FDstExprPtr MakeDstInteger(int64 Value);

	/** Calls Visit on every direct child node, in source order. */
	DREAMSTATETREELANG_API void ForEachChild(const FDstNode& Node, TFunctionRef<void(const FDstNode&)> Visit);

	/**
	 * `Info.Target.Location` as {"Info","Target","Location"} when the expression is a chain of names and members
	 * only; false for anything else (a call, a literal).
	 */
	DREAMSTATETREELANG_API bool TryGetDstPath(const FDstExpr& Expr, TArray<FString>& OutSegments);

	/** The same chain as one dotted string (`Info.Target.Location`); empty when it is not a path. */
	DREAMSTATETREELANG_API FString GetDstPathString(const FDstExpr& Expr);
}
