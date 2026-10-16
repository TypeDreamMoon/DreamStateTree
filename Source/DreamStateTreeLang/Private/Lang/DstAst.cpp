// Copyright DreamMoon. All Rights Reserved.
//
// The tree's out-of-line pieces: names for its enums, operator spellings and precedence, the lookups on metadata and
// the document header, the constructors and destructors of the nodes that own a type declared later in the header,
// deep copies, child traversal and path reading. ForEachChild visits children in source order; the tree dump and the
// later stages both rely on that order.

#include "Lang/DstAst.h"

#include "Misc/CString.h"

namespace UE::DreamStateTree::Lang
{
	// Kept apart from the other front-end files' helpers: the module may be built as one unity blob,
	// and names like CloneOptionalExpr are what any of them would pick.
	namespace DstAstPrivate
	{
		static FDstExprPtr CloneOptionalExpr(const FDstExprPtr& Expr)
		{
			if (!Expr)
			{
				return FDstExprPtr();
			}
			return CloneExpr(*Expr);
		}

		static TUniquePtr<FDstMeta> CloneOptionalMeta(const TUniquePtr<FDstMeta>& Meta)
		{
			if (!Meta)
			{
				return TUniquePtr<FDstMeta>();
			}
			return CloneMeta(*Meta);
		}

		static FDstArgumentPtr CloneOptionalArgument(const FDstArgumentPtr& Argument)
		{
			if (!Argument)
			{
				return FDstArgumentPtr();
			}
			return CloneArgument(*Argument);
		}

		static TArray<TUniquePtr<FDstParam>> CloneParams(const TArray<TUniquePtr<FDstParam>>& Params)
		{
			TArray<TUniquePtr<FDstParam>> Result;
			Result.Reserve(Params.Num());
			for (const TUniquePtr<FDstParam>& Param : Params)
			{
				Result.Add(Param ? CloneParam(*Param) : TUniquePtr<FDstParam>());
			}
			return Result;
		}

		static TArray<FDstStmtPtr> CloneStmts(const TArray<FDstStmtPtr>& Statements)
		{
			TArray<FDstStmtPtr> Result;
			Result.Reserve(Statements.Num());
			for (const FDstStmtPtr& Statement : Statements)
			{
				Result.Add(Statement ? CloneStmt(*Statement) : FDstStmtPtr());
			}
			return Result;
		}

		static bool AppendPathSegments(const FDstExpr& Expr, TArray<FString>& OutSegments)
		{
			if (const FDstNameExpr* Name = Expr.As<FDstNameExpr>())
			{
				OutSegments.Add(Name->Name);
				return true;
			}
			if (const FDstMemberExpr* Member = Expr.As<FDstMemberExpr>())
			{
				if (!Member->Object || !AppendPathSegments(*Member->Object, OutSegments))
				{
					return false;
				}
				OutSegments.Add(Member->Member);
				return true;
			}
			return false;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Names
	// ------------------------------------------------------------------------------------------

	const TCHAR* LexToString(EDstNodeKind Kind)
	{
		switch (Kind)
		{
		case EDstNodeKind::LiteralExpr:  return TEXT("LiteralExpr");
		case EDstNodeKind::NameExpr:     return TEXT("NameExpr");
		case EDstNodeKind::MemberExpr:   return TEXT("MemberExpr");
		case EDstNodeKind::CallExpr:     return TEXT("CallExpr");
		case EDstNodeKind::ListExpr:     return TEXT("ListExpr");
		case EDstNodeKind::TupleExpr:    return TEXT("TupleExpr");
		case EDstNodeKind::UnaryExpr:    return TEXT("UnaryExpr");
		case EDstNodeKind::BinaryExpr:   return TEXT("BinaryExpr");
		case EDstNodeKind::ParenExpr:    return TEXT("ParenExpr");
		case EDstNodeKind::RefExpr:      return TEXT("RefExpr");
		case EDstNodeKind::SectionStmt:  return TEXT("SectionStmt");
		case EDstNodeKind::AssignStmt:   return TEXT("AssignStmt");
		case EDstNodeKind::DeclStmt:     return TEXT("DeclStmt");
		case EDstNodeKind::ExprStmt:     return TEXT("ExprStmt");
		case EDstNodeKind::TemplateStmt: return TEXT("TemplateStmt");
		case EDstNodeKind::StateStmt:    return TEXT("StateStmt");
		case EDstNodeKind::ClauseStmt:   return TEXT("ClauseStmt");
		case EDstNodeKind::RegionStmt:   return TEXT("RegionStmt");
		case EDstNodeKind::Argument:     return TEXT("Argument");
		case EDstNodeKind::MetaEntry:    return TEXT("MetaEntry");
		case EDstNodeKind::Meta:         return TEXT("Meta");
		case EDstNodeKind::Param:        return TEXT("Param");
		case EDstNodeKind::TaskItem:     return TEXT("TaskItem");
		case EDstNodeKind::Include:      return TEXT("Include");
		case EDstNodeKind::Document:     return TEXT("Document");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(EDstClauseKind Kind)
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

	const TCHAR* LexToString(EDstDocumentKind Kind)
	{
		switch (Kind)
		{
		case EDstDocumentKind::StateTree:        return TEXT("StateTree");
		case EDstDocumentKind::StateTreeLibrary: return TEXT("StateTreeLibrary");
		case EDstDocumentKind::Unknown:          return TEXT("Unknown");
		}
		return TEXT("Unknown");
	}

	const TCHAR* GetDstUnaryOpSpelling(EDstUnaryOp Op)
	{
		switch (Op)
		{
		case EDstUnaryOp::Not:    return TEXT("not");
		case EDstUnaryOp::Negate: return TEXT("-");
		}
		return TEXT("");
	}

	const TCHAR* GetDstBinaryOpSpelling(EDstBinaryOp Op)
	{
		switch (Op)
		{
		case EDstBinaryOp::Or:           return TEXT("or");
		case EDstBinaryOp::And:          return TEXT("and");
		case EDstBinaryOp::Equal:        return TEXT("==");
		case EDstBinaryOp::NotEqual:     return TEXT("!=");
		case EDstBinaryOp::Less:         return TEXT("<");
		case EDstBinaryOp::LessEqual:    return TEXT("<=");
		case EDstBinaryOp::Greater:      return TEXT(">");
		case EDstBinaryOp::GreaterEqual: return TEXT(">=");
		case EDstBinaryOp::Add:          return TEXT("+");
		case EDstBinaryOp::Subtract:     return TEXT("-");
		case EDstBinaryOp::Multiply:     return TEXT("*");
		case EDstBinaryOp::Divide:       return TEXT("/");
		}
		return TEXT("");
	}

	int32 GetDstBinaryOpPrecedence(EDstBinaryOp Op)
	{
		switch (Op)
		{
		case EDstBinaryOp::Or:           return 1;
		case EDstBinaryOp::And:          return 2;
		case EDstBinaryOp::Equal:
		case EDstBinaryOp::NotEqual:     return 3;
		case EDstBinaryOp::Less:
		case EDstBinaryOp::LessEqual:
		case EDstBinaryOp::Greater:
		case EDstBinaryOp::GreaterEqual: return 4;
		case EDstBinaryOp::Add:
		case EDstBinaryOp::Subtract:     return 5;
		case EDstBinaryOp::Multiply:
		case EDstBinaryOp::Divide:       return 6;
		}
		return 0;
	}

	// ------------------------------------------------------------------------------------------
	// Nodes that own a type declared further down the header
	// ------------------------------------------------------------------------------------------

	// Defined here so that FDstMeta is a complete type where TUniquePtr<FDstMeta> is destroyed.

	FDstCallExpr::FDstCallExpr()
		: FDstExpr(StaticKind)
	{
	}

	FDstCallExpr::~FDstCallExpr() = default;

	FDstStateStmt::FDstStateStmt()
		: FDstStmt(StaticKind)
	{
	}

	FDstStateStmt::~FDstStateStmt() = default;

	FDstClauseStmt::FDstClauseStmt()
		: FDstStmt(StaticKind)
	{
	}

	FDstClauseStmt::~FDstClauseStmt() = default;

	// ------------------------------------------------------------------------------------------
	// Lookups
	// ------------------------------------------------------------------------------------------

	const FDstMetaEntry* FDstMeta::Find(const TCHAR* Key) const
	{
		if (!Key)
		{
			return nullptr;
		}
		for (const TUniquePtr<FDstMetaEntry>& Entry : Entries)
		{
			if (Entry && FCString::Strcmp(*Entry->Key, Key) == 0)
			{
				return Entry.Get();
			}
		}
		return nullptr;
	}

	const FDstArgument* FDstDocument::FindHeaderArg(const TCHAR* ArgName) const
	{
		if (!ArgName)
		{
			return nullptr;
		}
		for (const FDstArgumentPtr& Argument : HeaderArgs)
		{
			if (Argument && Argument->IsNamed() && FCString::Strcmp(*Argument->Name, ArgName) == 0)
			{
				return Argument.Get();
			}
		}
		return nullptr;
	}

	FString FDstDocument::GetHeaderString(const TCHAR* ArgName) const
	{
		const FDstArgument* Argument = FindHeaderArg(ArgName);
		if (!Argument || !Argument->Value)
		{
			return FString();
		}
		const FDstLiteralExpr* Literal = Argument->Value->As<FDstLiteralExpr>();
		if (Literal && Literal->LiteralKind == EDstLiteralKind::String)
		{
			return Literal->Text;
		}
		return FString();
	}

	// ------------------------------------------------------------------------------------------
	// Deep copies
	// ------------------------------------------------------------------------------------------

	FDstExprPtr CloneExpr(const FDstExpr& Expr)
	{
		switch (Expr.Kind)
		{
		case EDstNodeKind::LiteralExpr:
		{
			const FDstLiteralExpr& Source = static_cast<const FDstLiteralExpr&>(Expr);
			TUniquePtr<FDstLiteralExpr> Result = MakeUnique<FDstLiteralExpr>();
			Result->Span = Source.Span;
			Result->LiteralKind = Source.LiteralKind;
			Result->Text = Source.Text;
			Result->Number = Source.Number;
			Result->bBool = Source.bBool;
			return MoveTemp(Result);
		}
		case EDstNodeKind::NameExpr:
		{
			const FDstNameExpr& Source = static_cast<const FDstNameExpr&>(Expr);
			TUniquePtr<FDstNameExpr> Result = MakeUnique<FDstNameExpr>();
			Result->Span = Source.Span;
			Result->Name = Source.Name;
			Result->bQuoted = Source.bQuoted;
			return MoveTemp(Result);
		}
		case EDstNodeKind::MemberExpr:
		{
			const FDstMemberExpr& Source = static_cast<const FDstMemberExpr&>(Expr);
			TUniquePtr<FDstMemberExpr> Result = MakeUnique<FDstMemberExpr>();
			Result->Span = Source.Span;
			Result->Object = DstAstPrivate::CloneOptionalExpr(Source.Object);
			Result->Member = Source.Member;
			Result->MemberSpan = Source.MemberSpan;
			Result->bMemberQuoted = Source.bMemberQuoted;
			return MoveTemp(Result);
		}
		case EDstNodeKind::CallExpr:
		{
			const FDstCallExpr& Source = static_cast<const FDstCallExpr&>(Expr);
			TUniquePtr<FDstCallExpr> Result = MakeUnique<FDstCallExpr>();
			Result->Span = Source.Span;
			Result->Callee = DstAstPrivate::CloneOptionalExpr(Source.Callee);
			Result->Args.Reserve(Source.Args.Num());
			for (const FDstArgumentPtr& Argument : Source.Args)
			{
				Result->Args.Add(DstAstPrivate::CloneOptionalArgument(Argument));
			}
			Result->ArgsSpan = Source.ArgsSpan;
			Result->Meta = DstAstPrivate::CloneOptionalMeta(Source.Meta);
			return MoveTemp(Result);
		}
		case EDstNodeKind::ListExpr:
		{
			const FDstListExpr& Source = static_cast<const FDstListExpr&>(Expr);
			TUniquePtr<FDstListExpr> Result = MakeUnique<FDstListExpr>();
			Result->Span = Source.Span;
			Result->Items.Reserve(Source.Items.Num());
			for (const FDstExprPtr& Item : Source.Items)
			{
				Result->Items.Add(DstAstPrivate::CloneOptionalExpr(Item));
			}
			return MoveTemp(Result);
		}
		case EDstNodeKind::TupleExpr:
		{
			const FDstTupleExpr& Source = static_cast<const FDstTupleExpr&>(Expr);
			TUniquePtr<FDstTupleExpr> Result = MakeUnique<FDstTupleExpr>();
			Result->Span = Source.Span;
			Result->Items.Reserve(Source.Items.Num());
			for (const FDstArgumentPtr& Item : Source.Items)
			{
				Result->Items.Add(DstAstPrivate::CloneOptionalArgument(Item));
			}
			return MoveTemp(Result);
		}
		case EDstNodeKind::UnaryExpr:
		{
			const FDstUnaryExpr& Source = static_cast<const FDstUnaryExpr&>(Expr);
			TUniquePtr<FDstUnaryExpr> Result = MakeUnique<FDstUnaryExpr>();
			Result->Span = Source.Span;
			Result->Op = Source.Op;
			Result->Operand = DstAstPrivate::CloneOptionalExpr(Source.Operand);
			return MoveTemp(Result);
		}
		case EDstNodeKind::BinaryExpr:
		{
			const FDstBinaryExpr& Source = static_cast<const FDstBinaryExpr&>(Expr);
			TUniquePtr<FDstBinaryExpr> Result = MakeUnique<FDstBinaryExpr>();
			Result->Span = Source.Span;
			Result->Op = Source.Op;
			Result->Lhs = DstAstPrivate::CloneOptionalExpr(Source.Lhs);
			Result->Rhs = DstAstPrivate::CloneOptionalExpr(Source.Rhs);
			Result->OpSpan = Source.OpSpan;
			return MoveTemp(Result);
		}
		case EDstNodeKind::ParenExpr:
		{
			const FDstParenExpr& Source = static_cast<const FDstParenExpr&>(Expr);
			TUniquePtr<FDstParenExpr> Result = MakeUnique<FDstParenExpr>();
			Result->Span = Source.Span;
			Result->Inner = DstAstPrivate::CloneOptionalExpr(Source.Inner);
			return MoveTemp(Result);
		}
		case EDstNodeKind::RefExpr:
		{
			const FDstRefExpr& Source = static_cast<const FDstRefExpr&>(Expr);
			TUniquePtr<FDstRefExpr> Result = MakeUnique<FDstRefExpr>();
			Result->Span = Source.Span;
			Result->Target = DstAstPrivate::CloneOptionalExpr(Source.Target);
			return MoveTemp(Result);
		}
		default:
			break;
		}

		// Not an expression kind: nothing sensible to copy.
		return FDstExprPtr();
	}

	FDstStmtPtr CloneStmt(const FDstStmt& Stmt)
	{
		switch (Stmt.Kind)
		{
		case EDstNodeKind::SectionStmt:
		{
			const FDstSectionStmt& Source = static_cast<const FDstSectionStmt&>(Stmt);
			TUniquePtr<FDstSectionStmt> Result = MakeUnique<FDstSectionStmt>();
			Result->Span = Source.Span;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->OpenSpan = Source.OpenSpan;
			Result->CloseSpan = Source.CloseSpan;
			Result->Entries = DstAstPrivate::CloneStmts(Source.Entries);
			return MoveTemp(Result);
		}
		case EDstNodeKind::AssignStmt:
		{
			const FDstAssignStmt& Source = static_cast<const FDstAssignStmt&>(Stmt);
			TUniquePtr<FDstAssignStmt> Result = MakeUnique<FDstAssignStmt>();
			Result->Span = Source.Span;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->Value = DstAstPrivate::CloneOptionalExpr(Source.Value);
			Result->Meta = DstAstPrivate::CloneOptionalMeta(Source.Meta);
			return MoveTemp(Result);
		}
		case EDstNodeKind::DeclStmt:
		{
			const FDstDeclStmt& Source = static_cast<const FDstDeclStmt&>(Stmt);
			TUniquePtr<FDstDeclStmt> Result = MakeUnique<FDstDeclStmt>();
			Result->Span = Source.Span;
			Result->TypeName = Source.TypeName;
			Result->TypeSpan = Source.TypeSpan;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->Value = DstAstPrivate::CloneOptionalExpr(Source.Value);
			Result->Meta = DstAstPrivate::CloneOptionalMeta(Source.Meta);
			return MoveTemp(Result);
		}
		case EDstNodeKind::ExprStmt:
		{
			const FDstExprStmt& Source = static_cast<const FDstExprStmt&>(Stmt);
			TUniquePtr<FDstExprStmt> Result = MakeUnique<FDstExprStmt>();
			Result->Span = Source.Span;
			Result->Expr = DstAstPrivate::CloneOptionalExpr(Source.Expr);
			return MoveTemp(Result);
		}
		case EDstNodeKind::TemplateStmt:
		{
			const FDstTemplateStmt& Source = static_cast<const FDstTemplateStmt&>(Stmt);
			TUniquePtr<FDstTemplateStmt> Result = MakeUnique<FDstTemplateStmt>();
			Result->Span = Source.Span;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->bHasParams = Source.bHasParams;
			Result->Params = DstAstPrivate::CloneParams(Source.Params);
			Result->Body = DstAstPrivate::CloneOptionalExpr(Source.Body);
			return MoveTemp(Result);
		}
		case EDstNodeKind::StateStmt:
		{
			const FDstStateStmt& Source = static_cast<const FDstStateStmt&>(Stmt);
			TUniquePtr<FDstStateStmt> Result = MakeUnique<FDstStateStmt>();
			Result->Span = Source.Span;
			Result->bSubtree = Source.bSubtree;
			Result->WordSpan = Source.WordSpan;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->bNameQuoted = Source.bNameQuoted;
			Result->bHasParams = Source.bHasParams;
			Result->Params = DstAstPrivate::CloneParams(Source.Params);
			Result->Meta = DstAstPrivate::CloneOptionalMeta(Source.Meta);
			Result->Body = DstAstPrivate::CloneStmts(Source.Body);
			Result->OpenSpan = Source.OpenSpan;
			Result->CloseSpan = Source.CloseSpan;
			return MoveTemp(Result);
		}
		case EDstNodeKind::ClauseStmt:
		{
			const FDstClauseStmt& Source = static_cast<const FDstClauseStmt&>(Stmt);
			TUniquePtr<FDstClauseStmt> Result = MakeUnique<FDstClauseStmt>();
			Result->Span = Source.Span;
			Result->ClauseKind = Source.ClauseKind;
			Result->KeywordSpan = Source.KeywordSpan;
			Result->Value = DstAstPrivate::CloneOptionalExpr(Source.Value);
			Result->bLinkAsset = Source.bLinkAsset;
			Result->AssetSpan = Source.AssetSpan;
			Result->Tasks.Reserve(Source.Tasks.Num());
			for (const TUniquePtr<FDstTaskItem>& Task : Source.Tasks)
			{
				Result->Tasks.Add(Task ? CloneTaskItem(*Task) : TUniquePtr<FDstTaskItem>());
			}
			Result->Trigger = DstAstPrivate::CloneOptionalExpr(Source.Trigger);
			Result->OnSpan = Source.OnSpan;
			Result->Condition = DstAstPrivate::CloneOptionalExpr(Source.Condition);
			Result->IfSpan = Source.IfSpan;
			Result->Meta = DstAstPrivate::CloneOptionalMeta(Source.Meta);
			return MoveTemp(Result);
		}
		case EDstNodeKind::RegionStmt:
		{
			const FDstRegionStmt& Source = static_cast<const FDstRegionStmt&>(Stmt);
			TUniquePtr<FDstRegionStmt> Result = MakeUnique<FDstRegionStmt>();
			Result->Span = Source.Span;
			Result->bEnd = Source.bEnd;
			Result->Label = Source.Label;
			return MoveTemp(Result);
		}
		default:
			break;
		}

		return FDstStmtPtr();
	}

	TUniquePtr<FDstMeta> CloneMeta(const FDstMeta& Meta)
	{
		TUniquePtr<FDstMeta> Result = MakeUnique<FDstMeta>();
		Result->Span = Meta.Span;
		Result->Entries.Reserve(Meta.Entries.Num());
		for (const TUniquePtr<FDstMetaEntry>& Entry : Meta.Entries)
		{
			if (!Entry)
			{
				Result->Entries.Add(TUniquePtr<FDstMetaEntry>());
				continue;
			}
			TUniquePtr<FDstMetaEntry> Copy = MakeUnique<FDstMetaEntry>();
			Copy->Span = Entry->Span;
			Copy->Key = Entry->Key;
			Copy->KeySpan = Entry->KeySpan;
			Copy->Value = DstAstPrivate::CloneOptionalExpr(Entry->Value);
			Result->Entries.Add(MoveTemp(Copy));
		}
		return Result;
	}

	FDstArgumentPtr CloneArgument(const FDstArgument& Argument)
	{
		FDstArgumentPtr Result = MakeUnique<FDstArgument>();
		Result->Span = Argument.Span;
		Result->Name = Argument.Name;
		Result->NameSpan = Argument.NameSpan;
		Result->Value = DstAstPrivate::CloneOptionalExpr(Argument.Value);
		Result->bOutput = Argument.bOutput;
		return Result;
	}

	TUniquePtr<FDstParam> CloneParam(const FDstParam& Param)
	{
		TUniquePtr<FDstParam> Result = MakeUnique<FDstParam>();
		Result->Span = Param.Span;
		Result->TypeName = Param.TypeName;
		Result->TypeSpan = Param.TypeSpan;
		Result->Name = Param.Name;
		Result->NameSpan = Param.NameSpan;
		Result->Default = DstAstPrivate::CloneOptionalExpr(Param.Default);
		return Result;
	}

	TUniquePtr<FDstTaskItem> CloneTaskItem(const FDstTaskItem& Item)
	{
		TUniquePtr<FDstTaskItem> Result = MakeUnique<FDstTaskItem>();
		Result->Span = Item.Span;
		Result->BindName = Item.BindName;
		Result->BindNameSpan = Item.BindNameSpan;
		Result->Node = DstAstPrivate::CloneOptionalExpr(Item.Node);
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// Traversal
	// ------------------------------------------------------------------------------------------

	void ForEachChild(const FDstNode& Node, TFunctionRef<void(const FDstNode&)> Visit)
	{
		auto VisitIfSet = [&Visit](const FDstNode* Child)
		{
			if (Child)
			{
				Visit(*Child);
			}
		};

		switch (Node.Kind)
		{
		case EDstNodeKind::MemberExpr:
			VisitIfSet(static_cast<const FDstMemberExpr&>(Node).Object.Get());
			break;
		case EDstNodeKind::CallExpr:
		{
			const FDstCallExpr& Call = static_cast<const FDstCallExpr&>(Node);
			VisitIfSet(Call.Callee.Get());
			for (const FDstArgumentPtr& Argument : Call.Args)
			{
				VisitIfSet(Argument.Get());
			}
			VisitIfSet(Call.Meta.Get());
			break;
		}
		case EDstNodeKind::ListExpr:
			for (const FDstExprPtr& Item : static_cast<const FDstListExpr&>(Node).Items)
			{
				VisitIfSet(Item.Get());
			}
			break;
		case EDstNodeKind::TupleExpr:
			for (const FDstArgumentPtr& Item : static_cast<const FDstTupleExpr&>(Node).Items)
			{
				VisitIfSet(Item.Get());
			}
			break;
		case EDstNodeKind::UnaryExpr:
			VisitIfSet(static_cast<const FDstUnaryExpr&>(Node).Operand.Get());
			break;
		case EDstNodeKind::BinaryExpr:
		{
			const FDstBinaryExpr& Binary = static_cast<const FDstBinaryExpr&>(Node);
			VisitIfSet(Binary.Lhs.Get());
			VisitIfSet(Binary.Rhs.Get());
			break;
		}
		case EDstNodeKind::ParenExpr:
			VisitIfSet(static_cast<const FDstParenExpr&>(Node).Inner.Get());
			break;
		case EDstNodeKind::RefExpr:
			VisitIfSet(static_cast<const FDstRefExpr&>(Node).Target.Get());
			break;
		case EDstNodeKind::SectionStmt:
			for (const FDstStmtPtr& Entry : static_cast<const FDstSectionStmt&>(Node).Entries)
			{
				VisitIfSet(Entry.Get());
			}
			break;
		case EDstNodeKind::AssignStmt:
		{
			const FDstAssignStmt& Assign = static_cast<const FDstAssignStmt&>(Node);
			VisitIfSet(Assign.Value.Get());
			VisitIfSet(Assign.Meta.Get());
			break;
		}
		case EDstNodeKind::DeclStmt:
		{
			const FDstDeclStmt& Decl = static_cast<const FDstDeclStmt&>(Node);
			VisitIfSet(Decl.Value.Get());
			VisitIfSet(Decl.Meta.Get());
			break;
		}
		case EDstNodeKind::ExprStmt:
			VisitIfSet(static_cast<const FDstExprStmt&>(Node).Expr.Get());
			break;
		case EDstNodeKind::TemplateStmt:
		{
			const FDstTemplateStmt& Template = static_cast<const FDstTemplateStmt&>(Node);
			for (const TUniquePtr<FDstParam>& Param : Template.Params)
			{
				VisitIfSet(Param.Get());
			}
			VisitIfSet(Template.Body.Get());
			break;
		}
		case EDstNodeKind::StateStmt:
		{
			const FDstStateStmt& State = static_cast<const FDstStateStmt&>(Node);
			for (const TUniquePtr<FDstParam>& Param : State.Params)
			{
				VisitIfSet(Param.Get());
			}
			VisitIfSet(State.Meta.Get());
			for (const FDstStmtPtr& Item : State.Body)
			{
				VisitIfSet(Item.Get());
			}
			break;
		}
		case EDstNodeKind::ClauseStmt:
		{
			// Source order: the value (a target, an event, a condition), the tasks of a `do`, then what a transition
			// adds after its target: `on` trigger, `if` condition, `[ options ]`.
			const FDstClauseStmt& Clause = static_cast<const FDstClauseStmt&>(Node);
			VisitIfSet(Clause.Value.Get());
			for (const TUniquePtr<FDstTaskItem>& Task : Clause.Tasks)
			{
				VisitIfSet(Task.Get());
			}
			VisitIfSet(Clause.Trigger.Get());
			VisitIfSet(Clause.Condition.Get());
			VisitIfSet(Clause.Meta.Get());
			break;
		}
		case EDstNodeKind::Argument:
			VisitIfSet(static_cast<const FDstArgument&>(Node).Value.Get());
			break;
		case EDstNodeKind::MetaEntry:
			VisitIfSet(static_cast<const FDstMetaEntry&>(Node).Value.Get());
			break;
		case EDstNodeKind::Meta:
			for (const TUniquePtr<FDstMetaEntry>& Entry : static_cast<const FDstMeta&>(Node).Entries)
			{
				VisitIfSet(Entry.Get());
			}
			break;
		case EDstNodeKind::Param:
			VisitIfSet(static_cast<const FDstParam&>(Node).Default.Get());
			break;
		case EDstNodeKind::TaskItem:
			VisitIfSet(static_cast<const FDstTaskItem&>(Node).Node.Get());
			break;
		case EDstNodeKind::Document:
		{
			const FDstDocument& Document = static_cast<const FDstDocument&>(Node);
			for (const FDstArgumentPtr& Argument : Document.HeaderArgs)
			{
				VisitIfSet(Argument.Get());
			}
			for (const FDstStmtPtr& Member : Document.Members)
			{
				VisitIfSet(Member.Get());
			}
			break;
		}
		case EDstNodeKind::LiteralExpr:
		case EDstNodeKind::NameExpr:
		case EDstNodeKind::RegionStmt:
		case EDstNodeKind::Include:
			break;
		}
	}

	bool TryGetDstPath(const FDstExpr& Expr, TArray<FString>& OutSegments)
	{
		OutSegments.Reset();
		if (!DstAstPrivate::AppendPathSegments(Expr, OutSegments))
		{
			OutSegments.Reset();
			return false;
		}
		return true;
	}

	FString GetDstPathString(const FDstExpr& Expr)
	{
		TArray<FString> Segments;
		if (!TryGetDstPath(Expr, Segments))
		{
			return FString();
		}
		return FString::Join(Segments, TEXT("."));
	}
}
