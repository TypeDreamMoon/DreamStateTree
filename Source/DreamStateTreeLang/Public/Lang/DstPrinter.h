// Copyright DreamMoon. All Rights Reserved.
//
// The printer: syntax tree -> canonical text (Docs/Grammar.md, "Canonical format"), and a tree dump for the corpus
// goldens.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DstAst.h"

namespace UE::DreamStateTree::Lang
{
	struct FDstPrintOptions
	{
		int32 IndentWidth = 4;
		/** A clause, call, list or tuple that would end past this column is broken over lines. */
		int32 MaxWidth = 120;
		/** Written between lines. The file ends with exactly one. */
		FString LineEnd = TEXT("\n");
		/**
		 * The text the printed nodes were parsed from, when a reprint should keep its author's layout: a call, list or
		 * tuple that spans lines there, and a state card written over several lines, stay broken even where they would
		 * fit on one. Nodes made afresh have no span in it and print canonically. Canonical printing leaves it null.
		 */
		const FString* KeepBrokenIn = nullptr;
	};

	/**
	 * Name as it is written in source: itself when it is a plain identifier (IsDstPlainIdentifier), otherwise between
	 * backticks (`Move To`). An empty name stays empty.
	 */
	DREAMSTATETREELANG_API FString QuoteDstName(const FString& Name);

	/**
	 * The canonical text of a file: includes, then the document, with the file's trivia (comments, blank lines)
	 * where it was. Printing the parse of the result gives the same text again.
	 */
	DREAMSTATETREELANG_API FString PrintDst(const FDstFile& File, const FDstPrintOptions& Options = FDstPrintOptions());

	/**
	 * One expression in canonical form, as it would print starting at column StartColumn with the given indent level
	 * for any lines it breaks onto. Trivia, when File is given, is taken from it.
	 */
	DREAMSTATETREELANG_API FString PrintDstExpr(const FDstExpr& Expr, int32 IndentLevel = 0, int32 StartColumn = 0,
		const FDstFile* File = nullptr, const FDstPrintOptions& Options = FDstPrintOptions());

	/**
	 * One statement (a section, a state with its body, a clause, an entry) in canonical form at an indent level,
	 * without its own leading and trailing comments and without the alignment it would share with its neighbours.
	 */
	DREAMSTATETREELANG_API FString PrintDstStmt(const FDstStmt& Stmt, int32 IndentLevel = 0,
		const FDstFile* File = nullptr, const FDstPrintOptions& Options = FDstPrintOptions());

	/**
	 * A one-node-per-line dump for corpus goldens: two spaces of indent per depth, the node kind, then what it holds.
	 * No spans and no trivia, so a golden survives reformatting. Example:
	 *
	 *   Include "Common.dth"
	 *   Document StateTree
	 *     Arg Name
	 *       String "AI/ST_Brawler"
	 *     Arg Schema
	 *       Call
	 *         Name AIComponent
	 *         Arg Actor
	 *           Name CombatEnemy
	 *     Section Parameters
	 *       Decl float AttackRange
	 *         Number 200
	 *     State Root
	 *       Clause Transition
	 *         Name Root
	 *       State Attack
	 *         Meta
	 *           MetaEntry Tag
	 *             Name Combat
	 *         Clause Do
	 *           Task Combo
	 *             Call
	 *               Name ComboAttack
	 *     Subtree HitReaction
	 *       Param float Toughness
	 *         Number 1
	 *
	 * Leaf spellings: `Number <lexeme>`, `String "<escaped>"`, `Bool true`, `Name <id>`, `Member <id>` (child: the
	 * object), `Arg` / `Arg <name>` / `Arg <name> ->` (an output binding), `Unary not`, `Unary -`, `Binary <op>`,
	 * `Ref`, `Assign <name>`, `Decl <type> <name>`, `Expr`, `Template <name>` (children: params, then the body),
	 * `Region "<label>"`, `EndRegion`, `MetaEntry <key>`, `Param <name>` or `Param <type> <name>`, `Task` or
	 * `Task <bind name>`. A clause is `Clause <kind>` (`Clause Link asset` for `link asset`); a transition lists its
	 * target, then `On` with the trigger under it, `If` with the condition under it, then its Meta. Containers: `List`,
	 * `Tuple`, `Call` (its Meta, if any, last), `Paren`, `Meta`. A name that is not a plain identifier is shown in
	 * backticks.
	 */
	DREAMSTATETREELANG_API FString DumpDstTree(const FDstFile& File);
}
