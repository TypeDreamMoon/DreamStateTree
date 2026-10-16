// Copyright DreamMoon. All Rights Reserved.
//
// What the write-back files share: the text being edited (a fresh parse of the source, with its comments, whose nodes
// are found by the spans the checked text recorded), the edits collected against it, and the comparison context.
//
// Reading order of the files:
//   DstWriteBack.cpp           the entry points, the run, the final check, the package metadata
//   DstWriteBackText.cpp       the editable text, applying edits, comments that travel with a node
//   DstWriteBackModel.cpp      model views for comparing, aligning states and transitions
//   DstWriteBackNode.cpp       one node: a changed constant replaced in place, a call reprinted
//   DstWriteBackState.cpp      one state: settings, nodes, or the whole card reprinted around the cards that did not change
//   DstWriteBackDocument.cpp   the header, the sections, the top-level states

#pragma once

#include "CoreMinimal.h"
#include "DstDriver.h"
#include "DstExtract.h"
#include "DstGenerate.h"
#include "DstLower.h"
#include "DstNames.h"
#include "DstRaise.h"
#include "DstValues.h"
#include "DstWriteBack.h"
#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstParser.h"
#include "Lang/DstPrinter.h"
#include "Lang/DstSource.h"
#include "Lang/DstTemplates.h"
#include "Model/DstModel.h"

class UScriptStruct;
class FProperty;

namespace UE::DreamStateTree::Compiler::WriteBackPrivate
{
	// ------------------------------------------------------------------------------------------
	// Edits
	// ------------------------------------------------------------------------------------------

	/** [Offset, Offset + Length) of the original text becomes Text. Every edit is against the text as it was read. */
	struct FEdit
	{
		int32 Offset = 0;
		int32 Length = 0;
		FString Text;
		/** Tells inserts at one offset apart: they end up in the order they were made. */
		int32 Sequence = 0;
	};

	/**
	 * The edits applied from the back to the front. An edit inside another's span is dropped (the outer one is a reprint
	 * that holds it already); edits that overlap otherwise are a bug, and bOutOk is false.
	 */
	FString ApplyEdits(const FString& Text, TArray<FEdit> Edits, bool& bOutOk);

	/** The text of Span (offsets of Original) with the edits that lie inside it applied; the edits keep their absolute offsets. */
	FString SliceWithEdits(const FString& Original, const Lang::FDstSpan& Span, const TArray<FEdit>& Edits);

	/**
	 * Block as the first line of a nested piece moves from a line indented OldIndentChars to one indented NewIndent: the
	 * lines after the first lose the old indentation and take the new one. The first line carries none of its own.
	 */
	FString ReindentBlock(const FString& Block, int32 OldIndentChars, const FString& NewIndent);

	/** The comments and blank lines of From go to To (both nodes of File). */
	void CopyTrivia(Lang::FDstFile& File, const Lang::FDstNode* From, const Lang::FDstNode* To);

	// ------------------------------------------------------------------------------------------
	// The text
	// ------------------------------------------------------------------------------------------

	/**
	 * The source as a tree that can be changed and printed back in pieces. The tree is a parse of the original text
	 * (comments kept, templates not expanded), so what is printed from it is what the author wrote. Nodes are found by
	 * the spans the checked text recorded. Nodes taken out of the tree are kept until the end: the comments table is keyed
	 * by address, and an address that is reused would hand a stale comment to a new node.
	 */
	class FEditableText
	{
	public:
		FEditableText(const FString& InPath, const FString& InText);

		bool IsValid() const { return File.IsValid() && File->Document.IsValid(); }
		Lang::FDstFile& GetFile() const { return *File; }
		Lang::FDstDocument& GetDocument() const { return *File->Document; }
		const FString& GetText() const { return Source.GetText(); }
		const FString& GetLineEnd() const { return LineEnd; }

		/** The node with exactly this span and one of these kinds, or null. */
		Lang::FDstNode* FindNode(const Lang::FDstSpan& Span, std::initializer_list<Lang::EDstNodeKind> Kinds) const;
		Lang::FDstNode* GetParent(const Lang::FDstNode& Node) const;

		/** Offset of the first character of the line Offset is on. */
		int32 GetLineStart(int32 Offset) const;
		/** Offset of the line terminator that ends the line Offset is on (the end of the text on the last line). */
		int32 GetLineEndOffset(int32 Offset) const;
		/** The number of space and tab characters the line starts with. */
		int32 GetIndentChars(int32 Offset) const;
		/** The indentation of the line in levels of four columns (a tab counts as four). */
		int32 GetIndentLevel(int32 Offset) const;

		Lang::FDstPrintOptions MakePrintOptions() const;
		FString PrintStmt(const Lang::FDstStmt& Stmt, int32 Level) const;
		FString PrintExpr(const Lang::FDstExpr& Expr, int32 Level, int32 Column) const;

		/** Keeps a node taken out of the tree alive. */
		template <typename T>
		void Bury(TUniquePtr<T>&& Node)
		{
			if (Node)
			{
				Graveyard.Add(TUniquePtr<Lang::FDstNode>(MoveTemp(Node)));
			}
		}

	private:
		Lang::FDstSourceText Source;
		TUniquePtr<Lang::FDstFile> File;
		FString LineEnd;
		TMultiMap<int32, Lang::FDstNode*> ByOffset;
		TMap<const Lang::FDstNode*, Lang::FDstNode*> Parents;
		TArray<TUniquePtr<Lang::FDstNode>> Graveyard;
	};

	// ------------------------------------------------------------------------------------------
	// Plans
	// ------------------------------------------------------------------------------------------

	struct FPendingRefusal
	{
		const TCHAR* Code = TEXT("");
		Lang::FDstSpan Span;
		FString Message;
		FString Hint;
		/** Something the text cannot hold (sets bRefused); false for a note on what was written. */
		bool bRefusal = true;
	};

	/**
	 * What writing one thing comes to, made without touching the result: the edits, the changes to list, and the
	 * refusals. A plan that needs the whole card reprinted is dropped and the card is reprinted instead.
	 */
	struct FPlan
	{
		TArray<FEdit> Edits;
		TArray<FDstWriteBackChange> Changes;
		TArray<FPendingRefusal> Refusals;
		/** Not writable in place. */
		bool bNeedsReprint = false;
	};

	// ------------------------------------------------------------------------------------------
	// Model views
	// ------------------------------------------------------------------------------------------

	/** What a state is dumped without, to tell which kind of difference two states have. */
	struct FOwnView
	{
		/** Nodes without their arguments: what is left is the shape of the state. */
		bool bStripArgs = false;
		/** The settings a state's [ ] holds at their defaults. */
		bool bBlankSettings = false;
		bool bWithChildren = false;
	};

	FString DumpStateView(const Model::FDstState& State, const FOwnView& View);
	FString DumpNodeOf(const Model::FDstNode& Node);
	FString DumpNodesOf(const TArray<Model::FDstNode>& Nodes);
	FString DumpTransitionOf(const Model::FDstTransition& Transition);
	/** The settings of a state's [ ] differ (Tag, Color, Description, Enabled, Complete, TickRate, Weight, Group, ConsumeEvent, the flags). */
	bool SettingsDiffer(const Model::FDstState& A, const Model::FDstState& B);
	/** Everything of a state that its link clause writes. */
	FString LinkKeyOf(const Model::FDstState& State);
	FString EventKeyOf(const Model::FDstState& State);
	/** A subtree's signature; empty for a linked state, whose parameters are part of its link. */
	FString ParamsKeyOf(const Model::FDstState& State);
	/** What `pick` says; empty for a linked state, which selects the link itself. */
	FString PickKeyOf(const Model::FDstState& State);
	bool IsLinkedState(const Model::FDstState& State);

	// ------------------------------------------------------------------------------------------
	// The comparison
	// ------------------------------------------------------------------------------------------

	/** One top-level property of a node and the arguments the two sides give it. */
	struct FArgGroup
	{
		Model::EDstPropertySide Side = Model::EDstPropertySide::Instance;
		FString Property;
		TArray<const Model::FDstArg*> TextArgs;
		TArray<const Model::FDstArg*> AssetArgs;
	};

	/** A template use in the text: the call's span and the template's name. */
	struct FExpansionSite
	{
		Lang::FDstSpan Span;
		FString TemplateName;
	};

	/** Everything one write-back compares and edits. */
	struct FWriteBack
	{
		FWriteBack(const UStateTree& InAsset, const FString& InFilePath, const FString& InBody, const FDstWriteBackOptions& InOptions,
			FDstWriteBackResult& InResult);

		const UStateTree& Asset;
		const FString FilePath;
		/** The text without a byte-order mark: the offsets of every span are into it. */
		const FString Body;
		const FDstWriteBackOptions& Options;
		FDstWriteBackResult& Result;

		// -- the text, checked
		FDstFileResult Built;
		/** The model the text checks to. */
		Model::FDstTree T;
		FDstPlaces Places;
		FDstValueContext ValueContext;

		// -- the text, editable
		TUniquePtr<FEditableText> Edit;
		TSet<FString> TemplateNames;
		/** Where template uses stood in the text. */
		TArray<FExpansionSite> Expansions;
		/** State path -> its card in the text. A top-level state of an included library has none. */
		TMap<FString, Lang::FDstStateStmt*> StateStmts;

		// -- the asset
		Model::FDstTree A;
		FDstExtractResult Extracted;
		/** The asset's model as text: where the new pieces come from. */
		TUniquePtr<Lang::FDstFile> Raised;
		bool bRaiseOk = false;
		TMap<FString, const Lang::FDstStateStmt*> RaisedStates;

		// -- alignment
		TMap<FString, FString> AToT;
		TMap<FString, FString> TToA;

		// -- edits
		TArray<FEdit> Edits;
		int32 NextSequence = 0;
		int32 NextKeep = 0;

		// -- run (DstWriteBack.cpp)
		bool PrepareText();
		bool PrepareAsset();
		void PrepareRaise();
		void Compare();

		// -- results
		void AddChange(const FString& Location, const FString& What, bool bWritten, bool bReprinted);
		void Refuse(const TCHAR* Code, const Lang::FDstSpan& Span, const FString& Message, const FString& Hint = FString(), bool bRefusal = true);
		/** Makes the plan's edits (numbered), changes and refusals real; the edits go to Out. */
		void Commit(FPlan& Plan, TArray<FEdit>& Out);
		void AddEdit(TArray<FEdit>& Out, int32 Offset, int32 Length, const FString& NewText);
		void AddReplace(TArray<FEdit>& Out, const Lang::FDstSpan& Span, const FString& NewText) { AddEdit(Out, Span.Offset, Span.Length, NewText); }
		/** The edit that takes a statement out with its comments, whole lines when it has them to itself. */
		void AddRemoveStatement(TArray<FEdit>& Out, const Lang::FDstNode& Node);
		/** Printed text at the start of the line Offset is on, in front of what is there. */
		void AddInsertLines(TArray<FEdit>& Out, int32 AtOffset, const FString& Lines);

		// -- alignment (DstWriteBackModel.cpp)
		void AlignStates();
		bool ChildrenAligned(const Model::FDstState& AState, const Model::FDstState& TState) const;
		/** For each transition of A, the index of its counterpart in T or INDEX_NONE. */
		TArray<int32> AlignTransitions(const Model::FDstState& AState, const Model::FDstState& TState) const;

		// -- lookups (DstWriteBackState.cpp)
		Lang::FDstStateStmt* FindStateStmt(const FString& TPath) const;
		const Lang::FDstStateStmt* FindRaisedState(const FString& APath) const;
		const Lang::FDstExpr* FindRaisedTaskExpr(const FString& APath, int32 Index) const;
		const Lang::FDstExpr* FindRaisedGlobalExpr(int32 Index) const;
		bool UsesTemplate(const Lang::FDstNode& Node, bool bDeep, FString* OutName = nullptr) const;
		bool IsInExpansion(const Lang::FDstSpan& Span, FString* OutName = nullptr) const;

		// -- nodes (DstWriteBackNode.cpp)
		/**
		 * The difference between a node of the text and the same node of the asset, written as small as it goes: a constant
		 * replaced where it stands, or the call reprinted from the asset. RaisedExpr is the node as the asset's own text
		 * would spell it (null for a node that is only part of a condition).
		 */
		void PlanNode(const Model::FDstNode& TextNode, const Model::FDstNode& AssetNode, const FString& Location,
			const Lang::FDstExpr* RaisedExpr, FPlan& Plan);
		const Lang::FDstExpr* FindNodeExpr(const Model::FDstNode& TextNode, const Lang::FDstNode*& OutOwner) const;

		// -- states (DstWriteBackState.cpp)
		/** The edits that turn the card of T into the card of A (a matched pair), appended to Out. */
		void PlanState(const Model::FDstState& AState, const Model::FDstState& TState, TArray<FEdit>& Out);
		void PlanChildren(const Model::FDstState& AState, const Model::FDstState& TState, TArray<FEdit>& Out);
		void PlanLibraryState(const Model::FDstState& AState, const Model::FDstState& TState);
		void PlanStateSettings(const Model::FDstState& AState, const Model::FDstState& TState, Lang::FDstStateStmt& Stmt,
			const FString& Location, FPlan& Plan);
		void PlanStateNodes(const Model::FDstState& AState, const Model::FDstState& TState, const FString& Location, FPlan& Plan);
		/** The card of TState printed from the asset's state, keeping the text of what did not change. False when it cannot be made. */
		bool BuildCardText(const Model::FDstState& AState, const Model::FDstState& TState, Lang::FDstStateStmt& Stmt, FString& OutText);
		FString DescribeStateChange(const Model::FDstState& AState, const Model::FDstState& TState, bool bChildrenChanged) const;
		FString PrintMetaOf(const Lang::FDstMeta& Meta) const;

		// -- document (DstWriteBackDocument.cpp)
		void CompareHeader();
		void CompareSettings();
		void CompareParameters();
		void CompareGlobals();
		void CompareColors();
		void CompareStates();
		/**
		 * One section: values changed in place when only values changed, otherwise the section reprinted, added or removed.
		 * ChangedNames are the entries whose model differs.
		 */
		void PlanSection(const TCHAR* SectionName, const TSet<FString>& ChangedNames);
		bool TryPlanSectionValues(const Lang::FDstSectionStmt& TextSection, const Lang::FDstSectionStmt& NewSection, const TSet<FString>& ChangedNames,
			TArray<FEdit>& Out, TArray<FString>& OutChanged);
		Lang::FDstSectionStmt* FindTextSection(const TCHAR* Name) const;
		const Lang::FDstSectionStmt* FindRaisedSection(const TCHAR* Name) const;
	};
}
