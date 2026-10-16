// Copyright DreamMoon. All Rights Reserved.
//
// The raise: model -> syntax tree. The entry point, the builders of syntax tree nodes, and the parts of a file that are
// not a node call: the header, the sections (Tags, Settings, Parameters, Globals, Colors), the state cards with their
// clauses, and the transitions. Node calls are in DstRaiseNode.cpp, condition lists in DstRaiseCondition.cpp.
//
// Everything written here is read back by the semantic stage, so each choice follows what DstLower*.cpp accepts:
//   - a state is named by the shortest path whose first name the lowering's scoped lookup finds again;
//   - a tag is shortened with the declared prefixes only as far as ResolveDstTagName gives the same tag back;
//   - settings are written only when they differ from what the lowering starts a state from.

#include "DstRaise.h"

#include "DstRaisePrivate.h"

#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "Lang/DstPrinter.h"
#include "Lang/DstToken.h"
#include "Misc/PackageName.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorTypes.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler
{
	TUniquePtr<Lang::FDstFile> RaiseDst(const Model::FDstTree& Tree, const FDstRaiseOptions& Options, Lang::FDstDiagnosticSink& Sink)
	{
		// The names the text binds by are made on a copy: the caller's model stays as it was extracted.
		Model::FDstTree Working = Tree;
		EnsureDstBindNames(Working);
		Working.UpdatePaths();

		Raising::FDstRaiser Raiser(Working, Options, Sink);
		return Raiser.Run();
	}
}

namespace UE::DreamStateTree::Compiler::Raising
{
	// ------------------------------------------------------------------------------------------
	// Lookups
	// ------------------------------------------------------------------------------------------

	const UScriptStruct* FindStructByPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (const UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *Path))
		{
			return Found;
		}
		return LoadObject<UScriptStruct>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	const UClass* FindClassByPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (const UClass* Found = FindObject<UClass>(nullptr, *Path))
		{
			return Found;
		}
		return LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	bool IsReservedRootName(const FString& Name)
	{
		return Name == TEXT("Params") || Name == TEXT("Event");
	}

	namespace
	{
		/** Words that cannot start a tag path in the text: they would be read as a clause, an operator or a binding root. */
		bool IsRaiseReservedWord(const FString& Word)
		{
			static const TCHAR* const Reserved[] = {
				TEXT("Params"), TEXT("Event"), TEXT("None"), TEXT("true"), TEXT("false"), TEXT("on"), TEXT("if"), TEXT("pick"),
				TEXT("utility"), TEXT("link"), TEXT("asset"), TEXT("do"), TEXT("and"), TEXT("or"), TEXT("not"), TEXT("ref"),
				TEXT("State"), TEXT("Subtree"), TEXT("Template"),
			};
			for (const TCHAR* Candidate : Reserved)
			{
				if (Word.Equals(Candidate, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		/** The words after `->` that mean a transition type: a state called like one is written in backticks. */
		bool IsTargetWord(const FString& Name)
		{
			for (const Sugar::FWord& Word : Sugar::TargetWords)
			{
				if (Name.Equals(Word.Written, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		/** The name the engine prints for a selection behavior's value: deprecated aliases have the value of another name. */
		FString GetCanonicalSelectionName(const FString& Name)
		{
			if (const UEnum* Enum = StaticEnum<EStateTreeStateSelectionBehavior>())
			{
				const int64 Value = Enum->GetValueByNameString(Name);
				if (Value != INDEX_NONE)
				{
					const FString Canonical = Enum->GetNameStringByValue(Value);
					if (!Canonical.IsEmpty())
					{
						return Canonical;
					}
				}
			}
			return Name;
		}

		/** The mount a Root value means: `Game`, `Engine`, `Plugin.X` -> X. Empty for anything else. */
		FString GetMountOfRoot(const FString& Root)
		{
			if (Root == TEXT("Game") || Root == TEXT("Engine"))
			{
				return Root;
			}
			if (Root.StartsWith(TEXT("Plugin.")) && Root.Len() > 7)
			{
				return Root.RightChop(7);
			}
			return FString();
		}

		/** The spelling of one parameter type; Variant picks among the spellings the lowering accepts for a struct, an enum or a class. */
		FString SpellParameterType(const FString& Canonical, const int32 Variant, bool& bOutOk)
		{
			static const TCHAR* const Containers[] = { TEXT("Array"), TEXT("Set") };
			for (const TCHAR* Container : Containers)
			{
				const FString Open = FString(Container) + TEXT("<");
				if (Canonical.StartsWith(Open) && Canonical.EndsWith(TEXT(">")))
				{
					const FString Inner = SpellParameterType(Canonical.Mid(Open.Len(), Canonical.Len() - Open.Len() - 1), Variant, bOutOk);
					return Open + Inner + TEXT(">");
				}
			}

			struct FScalar
			{
				const TCHAR* Canonical;
				const TCHAR* Written;
			};
			static const FScalar Scalars[] = {
				{ TEXT("bool"), TEXT("bool") }, { TEXT("int32"), TEXT("int") }, { TEXT("int64"), TEXT("int64") }, { TEXT("float"), TEXT("float") },
				{ TEXT("double"), TEXT("double") }, { TEXT("Name"), TEXT("Name") }, { TEXT("String"), TEXT("String") }, { TEXT("Text"), TEXT("Text") },
			};
			for (const FScalar& Scalar : Scalars)
			{
				if (Canonical == Scalar.Canonical)
				{
					return Scalar.Written;
				}
			}

			int32 Colon = INDEX_NONE;
			if (!Canonical.FindChar(TEXT(':'), Colon))
			{
				bOutOk = false;
				return Canonical;
			}
			const FString Kind = Canonical.Left(Colon);
			const FString Path = Canonical.Mid(Colon + 1);

			if (Kind == TEXT("Struct"))
			{
				const UScriptStruct* Struct = FindStructByPath(Path);
				if (!Struct)
				{
					bOutOk = false;
					return Path;
				}
				return Variant == 1 ? Struct->GetPrefixCPP() + Struct->GetName() : Struct->GetName();
			}
			if (Kind == TEXT("Enum"))
			{
				const UEnum* Enum = FindObject<UEnum>(nullptr, *Path);
				if (!Enum)
				{
					Enum = LoadObject<UEnum>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
				}
				if (!Enum)
				{
					bOutOk = false;
					return Path;
				}
				return Enum->GetName();
			}

			const UClass* Class = FindClassByPath(Path);
			if (!Class)
			{
				bOutOk = false;
				return Path;
			}
			FString ClassName = Class->GetName();
			if (Variant == 1)
			{
				ClassName.RemoveFromEnd(TEXT("_C"));
			}
			else if (Variant == 2)
			{
				ClassName = Class->GetPrefixCPP() + ClassName;
			}
			if (Kind == TEXT("Object"))
			{
				return ClassName;
			}
			if (Kind == TEXT("SoftObject"))
			{
				return FString::Printf(TEXT("SoftObject<%s>"), *ClassName);
			}
			if (Kind == TEXT("Class"))
			{
				return FString::Printf(TEXT("Class<%s>"), *ClassName);
			}
			if (Kind == TEXT("SoftClass"))
			{
				return FString::Printf(TEXT("SoftClass<%s>"), *ClassName);
			}
			bOutOk = false;
			return Canonical;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Builders
	// ------------------------------------------------------------------------------------------

	FExprPtr MakeNameExpr(const FString& Name, const bool bForceQuoted)
	{
		TUniquePtr<Lang::FDstNameExpr> Expr = MakeUnique<Lang::FDstNameExpr>();
		Expr->Name = Name;
		Expr->bQuoted = bForceQuoted || !Lang::IsDstPlainIdentifier(Name);
		return FExprPtr(MoveTemp(Expr));
	}

	FExprPtr MakePathExpr(TConstArrayView<FString> Segments, const bool bQuoteFirst)
	{
		if (Segments.IsEmpty())
		{
			return MakeNameExpr(TEXT("None"), true);
		}
		FExprPtr Path = MakeNameExpr(Segments[0], bQuoteFirst);
		for (int32 Index = 1; Index < Segments.Num(); ++Index)
		{
			TUniquePtr<Lang::FDstMemberExpr> Member = MakeUnique<Lang::FDstMemberExpr>();
			Member->Object = MoveTemp(Path);
			Member->Member = Segments[Index];
			Member->bMemberQuoted = !Lang::IsDstPlainIdentifier(Segments[Index]);
			Path = FExprPtr(MoveTemp(Member));
		}
		return Path;
	}

	FExprPtr MakeStringExpr(const FString& Text)
	{
		TUniquePtr<Lang::FDstLiteralExpr> Expr = MakeUnique<Lang::FDstLiteralExpr>();
		Expr->LiteralKind = Lang::EDstLiteralKind::String;
		Expr->Text = Text;
		return FExprPtr(MoveTemp(Expr));
	}

	FExprPtr MakeBoolExpr(const bool bValue)
	{
		TUniquePtr<Lang::FDstLiteralExpr> Expr = MakeUnique<Lang::FDstLiteralExpr>();
		Expr->LiteralKind = Lang::EDstLiteralKind::Bool;
		Expr->bBool = bValue;
		Expr->Text = bValue ? TEXT("true") : TEXT("false");
		return FExprPtr(MoveTemp(Expr));
	}

	FExprPtr MakeNumberExpr(const double Value)
	{
		const double Magnitude = FMath::Abs(Value);
		TUniquePtr<Lang::FDstLiteralExpr> Literal = MakeUnique<Lang::FDstLiteralExpr>();
		Literal->LiteralKind = Lang::EDstLiteralKind::Number;
		Literal->Number = Magnitude;
		Literal->Text = FormatDstNumber(Magnitude);
		if (Value >= 0.0)
		{
			return FExprPtr(MoveTemp(Literal));
		}
		return MakeUnaryExpr(Lang::EDstUnaryOp::Negate, FExprPtr(MoveTemp(Literal)));
	}

	Lang::FDstArgumentPtr MakeArgument(const FString& Name, FExprPtr Value, const bool bOutput)
	{
		Lang::FDstArgumentPtr Argument = MakeUnique<Lang::FDstArgument>();
		Argument->Name = Name;
		Argument->Value = MoveTemp(Value);
		Argument->bOutput = bOutput;
		return Argument;
	}

	FExprPtr MakeParenExpr(FExprPtr Inner)
	{
		TUniquePtr<Lang::FDstParenExpr> Paren = MakeUnique<Lang::FDstParenExpr>();
		Paren->Inner = MoveTemp(Inner);
		return FExprPtr(MoveTemp(Paren));
	}

	namespace
	{
		/** How tightly an expression binds as an operand: a binary operator by its precedence, anything else as tight as it gets. */
		int32 GetRaiseOperandPrecedence(const Lang::FDstExpr& Expr)
		{
			if (const Lang::FDstBinaryExpr* Binary = Expr.As<Lang::FDstBinaryExpr>())
			{
				return Lang::GetDstBinaryOpPrecedence(Binary->Op);
			}
			return MAX_int32;
		}
	}

	FExprPtr MakeUnaryExpr(const Lang::EDstUnaryOp Op, FExprPtr Operand)
	{
		const bool bNeedsParens = Operand && (Operand->Is<Lang::FDstBinaryExpr>()
			|| (Op == Lang::EDstUnaryOp::Negate && Operand->Is<Lang::FDstUnaryExpr>()));
		if (bNeedsParens)
		{
			Operand = MakeParenExpr(MoveTemp(Operand));
		}
		TUniquePtr<Lang::FDstUnaryExpr> Unary = MakeUnique<Lang::FDstUnaryExpr>();
		Unary->Op = Op;
		Unary->Operand = MoveTemp(Operand);
		return FExprPtr(MoveTemp(Unary));
	}

	FExprPtr MakeBinaryExpr(const Lang::EDstBinaryOp Op, FExprPtr Lhs, FExprPtr Rhs)
	{
		const int32 Precedence = Lang::GetDstBinaryOpPrecedence(Op);
		// The operators associate to the left: an equally tight operand needs parentheses on the right only.
		if (Lhs && GetRaiseOperandPrecedence(*Lhs) < Precedence)
		{
			Lhs = MakeParenExpr(MoveTemp(Lhs));
		}
		if (Rhs && GetRaiseOperandPrecedence(*Rhs) <= Precedence)
		{
			Rhs = MakeParenExpr(MoveTemp(Rhs));
		}
		TUniquePtr<Lang::FDstBinaryExpr> Binary = MakeUnique<Lang::FDstBinaryExpr>();
		Binary->Op = Op;
		Binary->Lhs = MoveTemp(Lhs);
		Binary->Rhs = MoveTemp(Rhs);
		return FExprPtr(MoveTemp(Binary));
	}

	void AddMetaEntry(TUniquePtr<Lang::FDstMeta>& Meta, const FString& Key, FExprPtr Value)
	{
		if (!Meta)
		{
			Meta = MakeUnique<Lang::FDstMeta>();
		}
		TUniquePtr<Lang::FDstMetaEntry> Entry = MakeUnique<Lang::FDstMetaEntry>();
		Entry->Key = Key;
		Entry->Value = MoveTemp(Value);
		Meta->Entries.Add(MoveTemp(Entry));
	}

	// ------------------------------------------------------------------------------------------
	// The raiser: the file
	// ------------------------------------------------------------------------------------------

	FDstRaiser::FDstRaiser(const FModelTree& InTree, const FDstRaiseOptions& InOptions, Lang::FDstDiagnosticSink& InSink)
		: Tree(InTree)
		, Options(InOptions)
		, Sink(InSink)
		, Registry(FDstNameRegistry::Get())
	{
		ValueContext.TagPrefixes = Options.TagPrefixes;
	}

	TUniquePtr<Lang::FDstFile> FDstRaiser::Run()
	{
		TUniquePtr<Lang::FDstFile> File = MakeUnique<Lang::FDstFile>();
		File->Path = Options.FilePath;

		TUniquePtr<Lang::FDstDocument> Document = MakeUnique<Lang::FDstDocument>();
		Document->DocumentKind = Lang::EDstDocumentKind::StateTree;
		Document->KindName = TEXT("StateTree");

		ResolveSchema();
		BuildHeader(*Document);

		RaiseTagsSection(*Document);
		RaiseSettingsSection(*Document);
		RaiseParametersSection(*Document);
		RaiseGlobalsSection(*Document);
		RaiseColorsSection(*Document);

		AllStatePaths.Reset();
		CollectStatePaths(Tree.States);
		for (const FModelState& State : Tree.States)
		{
			TArray<const FModelState*> Chain;
			Document->Members.Add(RaiseState(State, Chain));
		}

		File->Document = MoveTemp(Document);
		return File;
	}

	void FDstRaiser::ResolveSchema()
	{
		SchemaClass = FindClassByPath(Tree.SchemaClass);
		if (!SchemaClass)
		{
			Sink.Error(TEXT("DST8055"), Lang::FDstSpan(),
				FString::Printf(TEXT("The schema class `%s` cannot be found, so the header cannot name it."), *Tree.SchemaClass),
				TEXT("The module that declares the schema has to be loaded."));
			return;
		}
		Namespaces = Registry.GetSchemaNamespaces(SchemaClass);
		if (const UStateTreeSchema* Defaults = Cast<UStateTreeSchema>(SchemaClass->GetDefaultObject()))
		{
			bSingleTaskSchema = !Defaults->AllowMultipleTasks();
		}
	}

	void FDstRaiser::BuildHeader(Lang::FDstDocument& Document)
	{
		// `/Game/AI/ST_Brawler.ST_Brawler` -> mount `Game`, name `AI/ST_Brawler`.
		FString Package = FPackageName::ObjectPathToPackageName(Tree.AssetPath);
		Package.RemoveFromStart(TEXT("/"));
		FString Mount;
		FString Relative;
		if (!Package.Split(TEXT("/"), &Mount, &Relative))
		{
			Mount = TEXT("Game");
			Relative = Package;
		}

		FString RootText;
		if (!Options.Root.IsEmpty() && GetMountOfRoot(Options.Root).Equals(Mount, ESearchCase::IgnoreCase))
		{
			RootText = Options.Root;
		}
		else if (Mount.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
		{
			RootText = TEXT("Engine");
		}
		else if (!Mount.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
		{
			RootText = TEXT("Plugin.") + Mount;
		}
		if (RootText == TEXT("Game"))
		{
			RootText.Reset();
		}

		Document.HeaderArgs.Add(MakeArgument(TEXT("Name"), MakeStringExpr(Relative)));
		if (!RootText.IsEmpty())
		{
			Document.HeaderArgs.Add(MakeArgument(TEXT("Root"), MakeStringExpr(RootText)));
		}
		Document.HeaderArgs.Add(MakeArgument(TEXT("Schema"), MakeSchemaExpr()));
	}

	FExprPtr FDstRaiser::MakeSchemaExpr()
	{
		FString SchemaName;
		if (SchemaClass)
		{
			SchemaName = Registry.GetSchemaName(SchemaClass);
		}
		else
		{
			Tree.SchemaClass.Split(TEXT("."), nullptr, &SchemaName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		}
		TArray<FString> NameSegments;
		SchemaName.ParseIntoArray(NameSegments, TEXT("."), true);
		FExprPtr Name = MakePathExpr(NameSegments);

		if (Tree.SchemaProperties.IsEmpty() || !SchemaClass)
		{
			return Name;
		}

		TArray<FString> Keys;
		Tree.SchemaProperties.GetKeys(Keys);
		Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

		TUniquePtr<Lang::FDstCallExpr> Call = MakeUnique<Lang::FDstCallExpr>();
		Call->Callee = MoveTemp(Name);
		for (const FString& Key : Keys)
		{
			const FProperty* Property = SchemaClass->FindPropertyByName(FName(*Key));
			if (!Property)
			{
				Sink.Error(TEXT("DST8055"), Lang::FDstSpan(),
					FString::Printf(TEXT("The schema %s has no property `%s`, so its value cannot be written in the header."), *SchemaName, *Key));
				continue;
			}

			// The context a property sets is the key the text prefers; the lowering looks it up the same way.
			FString ArgumentName = Key;
			if (Property->HasMetaData(TEXT("DreamStateTreeContext")))
			{
				ArgumentName = Property->GetMetaData(TEXT("DreamStateTreeContext"));
			}
			else if (Key == TEXT("ContextActorClass"))
			{
				ArgumentName = TEXT("Actor");
			}
			else if (Key == TEXT("AIControllerClass"))
			{
				ArgumentName = TEXT("AIController");
			}

			Call->Args.Add(MakeArgument(ArgumentName,
				MakeConstantExpr(Tree.SchemaProperties.FindChecked(Key), Property, FString::Printf(TEXT("the schema setting %s"), *Key))));
		}
		return FExprPtr(MoveTemp(Call));
	}

	void FDstRaiser::RaiseTagsSection(Lang::FDstDocument& Document)
	{
		if (Options.TagPrefixes.IsEmpty())
		{
			return;
		}
		TUniquePtr<Lang::FDstSectionStmt> Section = MakeUnique<Lang::FDstSectionStmt>();
		Section->Name = TEXT("Tags");
		for (const FString& Prefix : Options.TagPrefixes)
		{
			TArray<FString> Segments;
			Prefix.ParseIntoArray(Segments, TEXT("."), true);
			bool bPath = !Segments.IsEmpty() && !IsRaiseReservedWord(Segments[0]);
			for (const FString& Segment : Segments)
			{
				bPath = bPath && Lang::IsDstPlainIdentifier(Segment);
			}
			TUniquePtr<Lang::FDstExprStmt> Entry = MakeUnique<Lang::FDstExprStmt>();
			Entry->Expr = bPath ? MakePathExpr(Segments) : MakeStringExpr(Prefix);
			Section->Entries.Add(MoveTemp(Entry));
		}
		Document.Members.Add(MoveTemp(Section));
	}

	void FDstRaiser::RaiseSettingsSection(Lang::FDstDocument& Document)
	{
		if (Tree.Settings.IsEmpty())
		{
			return;
		}
		TArray<FString> Keys;
		Tree.Settings.GetKeys(Keys);
		Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

		TUniquePtr<Lang::FDstSectionStmt> Section = MakeUnique<Lang::FDstSectionStmt>();
		Section->Name = TEXT("Settings");
		for (const FString& Key : Keys)
		{
			const FProperty* Property = UStateTreeEditorData::StaticClass()->FindPropertyByName(FName(*Key));
			if (!Property)
			{
				Sink.Error(TEXT("DST8055"), Lang::FDstSpan(),
					FString::Printf(TEXT("The tree setting `%s` is not a property of the editor data, so it cannot be written."), *Key));
				continue;
			}
			TUniquePtr<Lang::FDstAssignStmt> Entry = MakeUnique<Lang::FDstAssignStmt>();
			Entry->Name = Key;
			Entry->Value = MakeConstantExpr(Tree.Settings.FindChecked(Key), Property, FString::Printf(TEXT("the setting %s"), *Key));
			Section->Entries.Add(MoveTemp(Entry));
		}
		if (!Section->Entries.IsEmpty())
		{
			Document.Members.Add(MoveTemp(Section));
		}
	}

	FString FDstRaiser::MakeParameterTypeText(const Model::FDstParameter& Parameter)
	{
		FString First;
		for (int32 Variant = 0; Variant < 3; ++Variant)
		{
			bool bOk = true;
			const FString Candidate = SpellParameterType(Parameter.Type, Variant, bOk);
			if (First.IsEmpty())
			{
				First = Candidate;
			}
			if (!bOk)
			{
				continue;
			}
			FString Canonical;
			FString Error;
			if (Lowering::ParseParameterType(Candidate, Canonical, Error) && Canonical == Parameter.Type)
			{
				return Candidate;
			}
		}
		Sink.Error(TEXT("DST8057"), Lang::FDstSpan(),
			FString::Printf(TEXT("The type of the parameter `%s` (%s) has no spelling the text reads back as the same type."), *Parameter.Name, *Parameter.Type),
			TEXT("Rename the type so that its name is unique among structs, enums and classes."));
		return First.IsEmpty() ? Parameter.Type : First;
	}

	FExprPtr FDstRaiser::MakeParameterDefault(const Model::FDstParameter& Parameter, const FProperty* Property)
	{
		return MakeConstantExpr(Parameter.ValueText, Property, FString::Printf(TEXT("the default of the parameter %s"), *Parameter.Name));
	}

	void FDstRaiser::RaiseParametersSection(Lang::FDstDocument& Document)
	{
		if (Tree.Parameters.IsEmpty())
		{
			return;
		}

		TreeParams.Params = Tree.Parameters;
		FString BuildError;
		bTreeParamsBuilt = TreeParams.Build(BuildError);
		if (!bTreeParamsBuilt)
		{
			Sink.Error(TEXT("DST8057"), Lang::FDstSpan(), FString::Printf(TEXT("The tree's parameters cannot be typed: %s"), *BuildError));
		}

		TUniquePtr<Lang::FDstSectionStmt> Section = MakeUnique<Lang::FDstSectionStmt>();
		Section->Name = TEXT("Parameters");
		for (const Model::FDstParameter& Parameter : Tree.Parameters)
		{
			TUniquePtr<Lang::FDstDeclStmt> Decl = MakeUnique<Lang::FDstDeclStmt>();
			Decl->TypeName = MakeParameterTypeText(Parameter);
			Decl->Name = Parameter.Name;
			if (!Parameter.ValueText.IsEmpty())
			{
				Decl->Value = MakeParameterDefault(Parameter, bTreeParamsBuilt ? TreeParams.FindProperty(Parameter.Name) : nullptr);
			}

			TArray<FString> MetaKeys;
			Parameter.Meta.GetKeys(MetaKeys);
			MetaKeys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
			for (const FString& Key : MetaKeys)
			{
				AddMetaEntry(Decl->Meta, Key, MakeStringExpr(Parameter.Meta.FindChecked(Key)));
			}
			Section->Entries.Add(MoveTemp(Decl));
		}
		Document.Members.Add(MoveTemp(Section));
	}

	void FDstRaiser::RaiseGlobalsSection(Lang::FDstDocument& Document)
	{
		if (Tree.Globals.IsEmpty())
		{
			return;
		}

		FScope Scope;
		FCallStyle Style;
		Style.bNameInSettings = false;

		TUniquePtr<Lang::FDstSectionStmt> Section = MakeUnique<Lang::FDstSectionStmt>();
		Section->Name = TEXT("Globals");
		for (int32 Index = 0; Index < Tree.Globals.Num(); ++Index)
		{
			const FModelNode& Node = Tree.Globals[Index];
			TUniquePtr<Lang::FDstAssignStmt> Entry = MakeUnique<Lang::FDstAssignStmt>();
			Entry->Name = Node.BindName;
			if (Entry->Name.IsEmpty())
			{
				// EnsureDstBindNames names every global; this is a guard.
				Entry->Name = FString::Printf(TEXT("Global%d"), Index);
				Sink.Error(TEXT("DST8003"), Lang::FDstSpan(), FString::Printf(TEXT("The global node `%s` has no name, and the text needs one."), *Node.Key));
			}
			Entry->Value = RaiseNodeCall(Node, Scope, Style);
			Section->Entries.Add(MoveTemp(Entry));
		}
		Document.Members.Add(MoveTemp(Section));
	}

	void FDstRaiser::RaiseColorsSection(Lang::FDstDocument& Document)
	{
		if (Tree.Colors.IsEmpty())
		{
			return;
		}
		const FProperty* ColorProperty = FStateTreeEditorColor::StaticStruct()->FindPropertyByName(TEXT("Color"));

		TUniquePtr<Lang::FDstSectionStmt> Section = MakeUnique<Lang::FDstSectionStmt>();
		Section->Name = TEXT("Colors");
		for (const Model::FDstColor& Color : Tree.Colors)
		{
			TUniquePtr<Lang::FDstAssignStmt> Entry = MakeUnique<Lang::FDstAssignStmt>();
			Entry->Name = Color.Name;
			Entry->Value = MakeConstantExpr(Color.ColorText, ColorProperty, FString::Printf(TEXT("the color %s"), *Color.Name));
			Section->Entries.Add(MoveTemp(Entry));
		}
		Document.Members.Add(MoveTemp(Section));
	}

	// ------------------------------------------------------------------------------------------
	// States
	// ------------------------------------------------------------------------------------------

	const FModelState* FDstRaiser::FindStateByPath(const FString& Path) const
	{
		return Tree.FindState(Path);
	}

	const FModelState* FDstRaiser::FindNearestEventState(const FScope& Scope) const
	{
		// The arguments of a `link` are evaluated before the state's own event exists.
		const int32 Last = Scope.bLinkParameter ? Scope.Chain.Num() - 2 : Scope.Chain.Num() - 1;
		for (int32 Index = Last; Index >= 0; --Index)
		{
			if (Scope.Chain[Index]->Event.IsSet())
			{
				return Scope.Chain[Index];
			}
		}
		return nullptr;
	}

	void FDstRaiser::CollectStatePaths(const TArray<FModelState>& States)
	{
		for (const FModelState& State : States)
		{
			TArray<FString>& Parts = AllStatePaths.AddDefaulted_GetRef();
			State.Path.ParseIntoArray(Parts, TEXT("/"), false);
			CollectStatePaths(State.Children);
		}
	}

	FExprPtr FDstRaiser::MakeStateTargetExpr(const FString& FromPath, const FString& ToPath)
	{
		TArray<FString> To;
		ToPath.ParseIntoArray(To, TEXT("/"), false);
		if (To.IsEmpty())
		{
			Sink.Error(TEXT("DST8056"), Lang::FDstSpan(), FString::Printf(TEXT("A transition of `%s` goes to a state but names none."), *FromPath));
			return MakeNameExpr(TEXT("None"), true);
		}

		// The lowering matches a written name or path against the end of every state's path (a subtree is its own root, so
		// a path never runs past it). The shortest ending that exactly one state has is the state's own name with as many
		// parents as it takes.
		int32 Take = To.Num();
		bool bFound = false;
		for (int32 Count = 1; Count <= To.Num() && !bFound; ++Count)
		{
			int32 Matches = 0;
			for (const TArray<FString>& Other : AllStatePaths)
			{
				if (Other.Num() < Count)
				{
					continue;
				}
				bool bEnds = true;
				for (int32 Index = 0; Index < Count && bEnds; ++Index)
				{
					bEnds = Other[Other.Num() - Count + Index].Equals(To[To.Num() - Count + Index], ESearchCase::CaseSensitive);
				}
				Matches += bEnds ? 1 : 0;
			}
			if (Matches == 1)
			{
				Take = Count;
				bFound = true;
			}
		}
		if (!bFound)
		{
			Sink.Warning(TEXT("DST8056"), Lang::FDstSpan(),
				FString::Printf(TEXT("The state `%s` cannot be told from another state by any ending of its path; the text spells the whole path."), *ToPath),
				TEXT("Two states have the same path; rename one of them."));
		}

		TArray<FString> Segments;
		for (int32 Index = To.Num() - Take; Index < To.Num(); ++Index)
		{
			Segments.Add(To[Index]);
		}
		return MakePathExpr(Segments, IsTargetWord(Segments[0]));
	}

	FExprPtr FDstRaiser::MakeTagExpr(const FString& TagName)
	{
		if (TagName.IsEmpty())
		{
			return MakeNameExpr(TEXT("None"), false);
		}
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), false);
		const FString Written = Tag.IsValid() ? ShortenDstTagName(Tag, Options.TagPrefixes) : TagName;

		TArray<FString> Segments;
		Written.ParseIntoArray(Segments, TEXT("."), false);
		bool bPath = !Segments.IsEmpty() && !IsRaiseReservedWord(Segments[0]);
		for (const FString& Segment : Segments)
		{
			bPath = bPath && Lang::IsDstPlainIdentifier(Segment);
		}
		return bPath ? MakePathExpr(Segments) : MakeStringExpr(Written);
	}

	FExprPtr FDstRaiser::MakeStructNameExpr(const FString& StructPath)
	{
		const UScriptStruct* Struct = FindStructByPath(StructPath);
		FString Name;
		if (Struct)
		{
			Name = Struct->GetName();
			if (Lowering::ResolveStructByName(Name) != Struct)
			{
				const FString WithPrefix = Struct->GetPrefixCPP() + Name;
				if (Lowering::ResolveStructByName(WithPrefix) == Struct)
				{
					Name = WithPrefix;
				}
				else
				{
					Sink.Warning(TEXT("DST8056"), Lang::FDstSpan(),
						FString::Printf(TEXT("The struct `%s` has no name the text reads back as that struct."), *StructPath),
						TEXT("Two types share its name; give one of them another."));
				}
			}
		}
		else
		{
			Sink.Error(TEXT("DST8056"), Lang::FDstSpan(), FString::Printf(TEXT("The payload struct `%s` cannot be found."), *StructPath));
			StructPath.Split(TEXT("."), nullptr, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		}
		return MakeNameExpr(Name);
	}

	FExprPtr FDstRaiser::MakeEventTriggerExpr(const Model::FDstEvent& Event)
	{
		if (Event.Tag.IsEmpty())
		{
			Sink.Error(TEXT("DST8056"), Lang::FDstSpan(), TEXT("An event trigger has no tag, so the text has nothing to write after `on`."));
		}
		FExprPtr Tag = MakeTagExpr(Event.Tag);
		if (Event.PayloadType.IsEmpty())
		{
			return Tag;
		}
		TUniquePtr<Lang::FDstCallExpr> Call = MakeUnique<Lang::FDstCallExpr>();
		Call->Callee = MoveTemp(Tag);
		Call->Args.Add(MakeArgument(FString(), MakeStructNameExpr(Event.PayloadType)));
		return FExprPtr(MoveTemp(Call));
	}

	TUniquePtr<Lang::FDstClauseStmt> FDstRaiser::MakeEventClause(const Model::FDstEvent& Event)
	{
		TUniquePtr<Lang::FDstClauseStmt> Clause = MakeUnique<Lang::FDstClauseStmt>();
		Clause->ClauseKind = Lang::EDstClauseKind::On;
		Clause->Value = MakeEventTriggerExpr(Event);
		return Clause;
	}

	TUniquePtr<Lang::FDstMeta> FDstRaiser::MakeStateSettings(const FModelState& State)
	{
		TUniquePtr<Lang::FDstMeta> Meta;
		const UClass* StateClass = UStateTreeState::StaticClass();

		// What a state starts from when the text says nothing: only what differs from it is written.
		const FModelState Defaults;
		const bool bLinked = State.Type == Model::EDstStateType::Linked || State.Type == Model::EDstStateType::LinkedAsset;
		const auto AddBoolSetting = [&Meta](const TCHAR* Key, const bool bValue, const bool bDefault)
		{
			if (bValue != bDefault)
			{
				// A flag alone is true.
				AddMetaEntry(Meta, Key, bValue ? FExprPtr() : MakeBoolExpr(false));
			}
		};

		if (!State.Tag.IsEmpty())
		{
			AddMetaEntry(Meta, TEXT("Tag"), MakeTagExpr(State.Tag));
		}
		if (!State.ColorName.IsEmpty())
		{
			AddMetaEntry(Meta, TEXT("Color"), MakeNameExpr(State.ColorName));
		}
		if (!State.Description.IsEmpty())
		{
			AddMetaEntry(Meta, TEXT("Description"), MakeStringExpr(State.Description));
		}
		AddBoolSetting(TEXT("Enabled"), State.bEnabled, Defaults.bEnabled);
		if (!State.TasksCompletion.Equals(Defaults.TasksCompletion, ESearchCase::CaseSensitive))
		{
			AddMetaEntry(Meta, TEXT("Complete"), MakeNameExpr(State.TasksCompletion));
		}
		if (State.bCustomTickRate)
		{
			AddMetaEntry(Meta, TEXT("TickRate"),
				MakeConstantExpr(State.TickRateText, StateClass->FindPropertyByName(TEXT("CustomTickRate")), FString::Printf(TEXT("the tick rate of %s"), *State.Path)));
		}
		if (!State.WeightText.IsEmpty())
		{
			AddMetaEntry(Meta, TEXT("Weight"),
				MakeConstantExpr(State.WeightText, StateClass->FindPropertyByName(TEXT("Weight")), FString::Printf(TEXT("the weight of %s"), *State.Path)));
		}
		if (State.Type == Model::EDstStateType::Group)
		{
			AddMetaEntry(Meta, TEXT("Group"), nullptr);
		}
		if (State.Event.IsSet())
		{
			AddBoolSetting(TEXT("ConsumeEvent"), State.Event->bConsumeOnSelect, Model::FDstEvent().bConsumeOnSelect);
		}
		AddBoolSetting(TEXT("bCopyParameterBindingsOnTick"), State.bCopyParameterBindingsOnTick, Defaults.bCopyParameterBindingsOnTick);
		AddBoolSetting(TEXT("bCheckPrerequisitesWhenActivatingChildDirectly"), State.bCheckPrerequisitesWhenActivatingChildDirectly,
			Defaults.bCheckPrerequisitesWhenActivatingChildDirectly);
		// Setting a link gives the state the target's layout; the lowering does that again, so it is not written.
		if (!bLinked)
		{
			AddBoolSetting(TEXT("bFixedLayout"), State.bFixedLayout, Defaults.bFixedLayout);
		}
		return Meta;
	}

	TUniquePtr<Lang::FDstClauseStmt> FDstRaiser::MakeLinkClause(const FModelState& State, const FScope& Scope)
	{
		const bool bAsset = State.Type == Model::EDstStateType::LinkedAsset;

		TUniquePtr<Lang::FDstClauseStmt> Clause = MakeUnique<Lang::FDstClauseStmt>();
		Clause->ClauseKind = Lang::EDstClauseKind::Link;
		Clause->bLinkAsset = bAsset;

		FExprPtr Target;
		if (bAsset)
		{
			Target = MakeDstAssetExpr(State.LinkTarget, UStateTree::StaticClass());
		}
		else
		{
			TArray<FString> Segments;
			State.LinkTarget.ParseIntoArray(Segments, TEXT("/"), false);
			Target = MakePathExpr(Segments);
		}
		if (!Target)
		{
			Sink.Error(TEXT("DST8056"), Lang::FDstSpan(), FString::Printf(TEXT("The link of `%s` points at `%s`, which has no name in the text."), *State.Path, *State.LinkTarget));
			Target = MakeNameExpr(TEXT("None"), true);
		}

		// The overridden parameters of the target are the arguments.
		Lowering::FParamSet Signature;
		Signature.Params = State.Parameters;
		FString BuildError;
		const bool bTyped = Signature.Build(BuildError);
		if (!bTyped)
		{
			Sink.Error(TEXT("DST8057"), Lang::FDstSpan(), FString::Printf(TEXT("The parameters of the link of `%s` cannot be typed: %s"), *State.Path, *BuildError));
		}

		FScope LinkScope = Scope;
		LinkScope.bLinkParameter = true;

		// The overridden parameters, in the order of the target's signature: a constant override is the parameter's own
		// value, one fed from elsewhere (a path, an operation) is in ParameterBindings.
		TArray<Lang::FDstArgumentPtr> Arguments;
		for (const Model::FDstParameter& Parameter : State.Parameters)
		{
			if (!Parameter.bOverridden)
			{
				continue;
			}
			const FProperty* Property = bTyped ? Signature.FindProperty(Parameter.Name) : nullptr;
			const FString What = FString::Printf(TEXT("the parameter %s of %s"), *Parameter.Name, *State.Path);

			TArray<const FModelArg*> Group;
			for (const FModelArg& Arg : State.ParameterBindings)
			{
				if (!Arg.Property.IsEmpty() && Arg.Property.Segments[0].Name.Equals(Parameter.Name, ESearchCase::CaseSensitive))
				{
					Group.Add(&Arg);
				}
			}

			FExprPtr Value;
			if (Group.IsEmpty())
			{
				Value = MakeConstantExpr(Parameter.ValueText, Property, What);
			}
			else
			{
				// A struct parameter with some fields fed from elsewhere keeps its constant fields as the parameter's own value.
				FModelArg ConstantPart;
				const bool bHasWhole = Group.ContainsByPredicate([](const FModelArg* Arg) { return Arg->Property.Segments.Num() == 1 && Arg->Property.Segments[0].ArrayIndex == INDEX_NONE; });
				if (!bHasWhole && !Parameter.ValueText.IsEmpty())
				{
					ConstantPart.Side = Model::EDstPropertySide::Instance;
					Model::FDstPathSegment ParameterSegment;
					ParameterSegment.Name = Group[0]->Property.Segments[0].Name;
					ConstantPart.Property.Segments.Add(MoveTemp(ParameterSegment));
					ConstantPart.Kind = Model::EDstValueKind::Constant;
					ConstantPart.ConstantText = Parameter.ValueText;
					Group.Add(&ConstantPart);
				}
				bool bOutput = false;
				Value = RaiseArgGroup(Property, Parameter.Name, Group, LinkScope, bOutput, What);
				if (!Value)
				{
					Value = MakeConstantExpr(Parameter.ValueText, Property, What);
				}
			}
			Arguments.Add(MakeArgument(Parameter.Name, MoveTemp(Value)));
		}

		if (Arguments.IsEmpty())
		{
			Clause->Value = MoveTemp(Target);
		}
		else
		{
			TUniquePtr<Lang::FDstCallExpr> Call = MakeUnique<Lang::FDstCallExpr>();
			Call->Callee = MoveTemp(Target);
			Call->Args = MoveTemp(Arguments);
			Clause->Value = FExprPtr(MoveTemp(Call));
		}
		return Clause;
	}

	FExprPtr FDstRaiser::MakeDelegateTriggerExpr(const Model::FDstTransition& Transition, const FScope& Scope)
	{
		FExprPtr Path;
		if (Transition.Completion != Model::EDstCompletion::None)
		{
			// `Combo.Succeeds`: the completion of a task.
			const FString Name = FindBindName(Transition.Delegate.Owner);
			if (Name.IsEmpty())
			{
				Sink.Error(TEXT("DST8003"), Lang::FDstSpan(),
					FString::Printf(TEXT("A transition listens to the completion of `%s`, which has no name the text could use."), *Transition.Delegate.Owner));
				Path = MakeNameExpr(TEXT("None"), true);
			}
			else
			{
				const TCHAR* Word = TEXT("Completes");
				for (const Sugar::FWord& Entry : Sugar::CompletionWords)
				{
					const TCHAR* Engine = Transition.Completion == Model::EDstCompletion::Succeeds ? TEXT("Succeeds")
						: Transition.Completion == Model::EDstCompletion::Fails ? TEXT("Fails") : TEXT("Completes");
					if (FCString::Strcmp(Entry.Engine, Engine) == 0)
					{
						Word = Entry.Written;
					}
				}
				TArray<FString> Segments;
				Segments.Add(Name);
				Segments.Add(Word);
				Path = MakePathExpr(Segments);
			}
		}
		else
		{
			Path = RaiseSource(Transition.Delegate, Scope, TEXT("the delegate of a transition"));
			if (!Path)
			{
				Path = MakeNameExpr(TEXT("None"), true);
			}
		}

		TUniquePtr<Lang::FDstCallExpr> Call = MakeUnique<Lang::FDstCallExpr>();
		Call->Callee = MakeNameExpr(TEXT("Delegate"));
		Call->Args.Add(MakeArgument(FString(), MoveTemp(Path)));
		return FExprPtr(MoveTemp(Call));
	}

	namespace
	{
		/** The call at the right end of a condition, outside any parentheses; its `[ ]` would be read as the transition's options. */
		const Lang::FDstCallExpr* FindRaiseRightmostCall(const Lang::FDstExpr& Expr)
		{
			switch (Expr.Kind)
			{
			case Lang::EDstNodeKind::CallExpr:
				return static_cast<const Lang::FDstCallExpr*>(&Expr);
			case Lang::EDstNodeKind::BinaryExpr:
			{
				const Lang::FDstBinaryExpr& Binary = static_cast<const Lang::FDstBinaryExpr&>(Expr);
				return Binary.Rhs ? FindRaiseRightmostCall(*Binary.Rhs) : nullptr;
			}
			case Lang::EDstNodeKind::UnaryExpr:
			{
				const Lang::FDstUnaryExpr& Unary = static_cast<const Lang::FDstUnaryExpr&>(Expr);
				return Unary.Operand ? FindRaiseRightmostCall(*Unary.Operand) : nullptr;
			}
			default:
				return nullptr;
			}
		}
	}

	TUniquePtr<Lang::FDstClauseStmt> FDstRaiser::RaiseTransition(const FModelState& State, const int32 Index, const FScope& InScope)
	{
		const Model::FDstTransition& Transition = State.Transitions[Index];

		TUniquePtr<Lang::FDstClauseStmt> Clause = MakeUnique<Lang::FDstClauseStmt>();
		Clause->ClauseKind = Lang::EDstClauseKind::Transition;

		// The target.
		if (Transition.TargetKind == Model::EDstTransitionTarget::GotoState)
		{
			Clause->Value = MakeStateTargetExpr(State.Path, Transition.TargetState);
		}
		else
		{
			const FString EngineName = Model::LexToString(Transition.TargetKind);
			const TCHAR* Written = nullptr;
			for (const Sugar::FWord& Word : Sugar::TargetWords)
			{
				if (EngineName.Equals(Word.Engine, ESearchCase::CaseSensitive))
				{
					Written = Word.Written;
				}
			}
			if (Written)
			{
				Clause->Value = MakeNameExpr(Written);
			}
			else
			{
				Sink.Error(TEXT("DST8056"), Lang::FDstSpan(),
					FString::Printf(TEXT("A transition of `%s` has the target `%s`, which the text has no word for."), *State.Path, *EngineName),
					TEXT("Give the transition a target state in the editor."));
				Clause->Value = MakeNameExpr(EngineName, true);
			}
		}

		// The trigger.
		FScope Scope = InScope;
		switch (Transition.Trigger)
		{
		case Model::EDstTransitionTrigger::OnStateCompleted:
			break;
		case Model::EDstTransitionTrigger::OnStateSucceeded:
			Clause->Trigger = MakeNameExpr(TEXT("Succeeded"));
			break;
		case Model::EDstTransitionTrigger::OnStateFailed:
			Clause->Trigger = MakeNameExpr(TEXT("Failed"));
			break;
		case Model::EDstTransitionTrigger::OnTick:
			Clause->Trigger = MakeNameExpr(TEXT("Tick"));
			break;
		case Model::EDstTransitionTrigger::OnEvent:
			Clause->Trigger = MakeEventTriggerExpr(Transition.Event);
			Scope.TransitionEventOwner = FString::Printf(TEXT("%s#%d"), *State.Path, Index);
			break;
		case Model::EDstTransitionTrigger::OnDelegate:
			Clause->Trigger = MakeDelegateTriggerExpr(Transition, Scope);
			break;
		}

		// The options: what differs from a transition the lowering starts from.
		const Model::FDstTransition DefaultTransition;
		TUniquePtr<Lang::FDstMeta> OptionList;
		if (!Transition.Priority.Equals(DefaultTransition.Priority, ESearchCase::CaseSensitive))
		{
			AddMetaEntry(OptionList, TEXT("Priority"), MakeNameExpr(Transition.Priority));
		}
		if (Transition.bDelay)
		{
			const FProperty* DelayProperty = FStateTreeTransition::StaticStruct()->FindPropertyByName(TEXT("DelayDuration"));
			const FProperty* VarianceProperty = FStateTreeTransition::StaticStruct()->FindPropertyByName(TEXT("DelayRandomVariance"));
			const FString What = FString::Printf(TEXT("the delay of a transition of %s"), *State.Path);
			// Variance alone switches the delay on as well; with neither value the delay is the zero it was set to.
			if (!Transition.DelayText.IsEmpty() || Transition.VarianceText.IsEmpty())
			{
				AddMetaEntry(OptionList, TEXT("Delay"), MakeConstantExpr(Transition.DelayText, DelayProperty, What));
			}
			if (!Transition.VarianceText.IsEmpty())
			{
				AddMetaEntry(OptionList, TEXT("Variance"), MakeConstantExpr(Transition.VarianceText, VarianceProperty, What));
			}
		}
		if (Transition.bReactivate != DefaultTransition.bReactivate)
		{
			AddMetaEntry(OptionList, TEXT("Reactivate"), Transition.bReactivate ? FExprPtr() : MakeBoolExpr(false));
		}
		if (Transition.bEnabled != DefaultTransition.bEnabled)
		{
			AddMetaEntry(OptionList, TEXT("Enabled"), MakeBoolExpr(Transition.bEnabled));
		}
		if (Transition.Trigger == Model::EDstTransitionTrigger::OnEvent && Transition.Event.bConsumeOnSelect != DefaultTransition.Event.bConsumeOnSelect)
		{
			AddMetaEntry(OptionList, TEXT("ConsumeEvent"), MakeBoolExpr(Transition.Event.bConsumeOnSelect));
		}

		// The condition. Settings of the call at its right end would be read as the options.
		if (!Transition.Conditions.IsEmpty())
		{
			Clause->Condition = RaiseConditionList(Transition.Conditions, Scope, /*bConsideration*/ false);
			if (Clause->Condition && !OptionList)
			{
				const Lang::FDstCallExpr* Last = FindRaiseRightmostCall(*Clause->Condition);
				if (Last && Last->Meta)
				{
					Clause->Condition = MakeParenExpr(MoveTemp(Clause->Condition));
				}
			}
		}
		Clause->Meta = MoveTemp(OptionList);
		return Clause;
	}

	TUniquePtr<Lang::FDstStateStmt> FDstRaiser::RaiseState(const FModelState& State, TArray<const FModelState*>& Chain)
	{
		Chain.Add(&State);
		FScope Scope;
		Scope.Chain = Chain;

		TUniquePtr<Lang::FDstStateStmt> Stmt = MakeUnique<Lang::FDstStateStmt>();
		Stmt->bSubtree = State.Type == Model::EDstStateType::Subtree && Chain.Num() == 1;
		Stmt->Name = State.Name;
		Stmt->bNameQuoted = !Lang::IsDstPlainIdentifier(State.Name);

		// A subtree's signature.
		if (Stmt->bSubtree && !State.Parameters.IsEmpty())
		{
			Lowering::FParamSet Signature;
			Signature.Params = State.Parameters;
			FString BuildError;
			const bool bTyped = Signature.Build(BuildError);
			if (!bTyped)
			{
				Sink.Error(TEXT("DST8057"), Lang::FDstSpan(), FString::Printf(TEXT("The parameters of the subtree `%s` cannot be typed: %s"), *State.Name, *BuildError));
			}
			Stmt->bHasParams = true;
			for (const Model::FDstParameter& Parameter : State.Parameters)
			{
				TUniquePtr<Lang::FDstParam> Param = MakeUnique<Lang::FDstParam>();
				Param->TypeName = MakeParameterTypeText(Parameter);
				Param->Name = Parameter.Name;
				if (!Parameter.ValueText.IsEmpty())
				{
					Param->Default = MakeParameterDefault(Parameter, bTyped ? Signature.FindProperty(Parameter.Name) : nullptr);
				}
				Stmt->Params.Add(MoveTemp(Param));
			}
		}

		Stmt->Meta = MakeStateSettings(State);

		// The clauses, in the order the grammar lists them.
		if (State.Event.IsSet())
		{
			Stmt->Body.Add(MakeEventClause(State.Event.GetValue()));
		}
		if (!State.EnterConditions.IsEmpty())
		{
			if (FExprPtr Condition = RaiseConditionList(State.EnterConditions, Scope, /*bConsideration*/ false))
			{
				TUniquePtr<Lang::FDstClauseStmt> Clause = MakeUnique<Lang::FDstClauseStmt>();
				Clause->ClauseKind = Lang::EDstClauseKind::If;
				Clause->Value = MoveTemp(Condition);
				Stmt->Body.Add(MoveTemp(Clause));
			}
		}
		// A link makes the state select the linked tree itself, which the lowering does again: no `pick` for it.
		const bool bLinkedState = State.Type == Model::EDstStateType::Linked || State.Type == Model::EDstStateType::LinkedAsset;
		if (!bLinkedState && !State.Selection.Equals(FModelState().Selection, ESearchCase::CaseSensitive))
		{
			const FString WantedName = GetCanonicalSelectionName(State.Selection);
			const TCHAR* Written = nullptr;
			for (const Sugar::FWord& Word : Sugar::PickWords)
			{
				// The first word with that value: an alias (UniformRandom, RelativeUtility) is only read, never written.
				if (!Written && GetCanonicalSelectionName(Word.Engine).Equals(WantedName, ESearchCase::CaseSensitive))
				{
					Written = Word.Written;
				}
			}
			if (Written)
			{
				TUniquePtr<Lang::FDstClauseStmt> Clause = MakeUnique<Lang::FDstClauseStmt>();
				Clause->ClauseKind = Lang::EDstClauseKind::Pick;
				Clause->Value = MakeNameExpr(Written);
				Stmt->Body.Add(MoveTemp(Clause));
			}
			else
			{
				Sink.Error(TEXT("DST8056"), Lang::FDstSpan(),
					FString::Printf(TEXT("The state `%s` picks its children with `%s`, which the text has no word for."), *State.Path, *State.Selection));
			}
		}
		if (!State.Considerations.IsEmpty())
		{
			if (FExprPtr Utility = RaiseConditionList(State.Considerations, Scope, /*bConsideration*/ true))
			{
				TUniquePtr<Lang::FDstClauseStmt> Clause = MakeUnique<Lang::FDstClauseStmt>();
				Clause->ClauseKind = Lang::EDstClauseKind::Utility;
				Clause->Value = MoveTemp(Utility);
				Stmt->Body.Add(MoveTemp(Clause));
			}
		}
		if (State.Type == Model::EDstStateType::Linked || State.Type == Model::EDstStateType::LinkedAsset)
		{
			Stmt->Body.Add(MakeLinkClause(State, Scope));
		}
		if (!State.Tasks.IsEmpty())
		{
			TUniquePtr<Lang::FDstClauseStmt> Clause = MakeUnique<Lang::FDstClauseStmt>();
			Clause->ClauseKind = Lang::EDstClauseKind::Do;
			for (const FModelNode& Task : State.Tasks)
			{
				Clause->Tasks.Add(RaiseTask(Task, Scope));
			}
			Stmt->Body.Add(MoveTemp(Clause));
		}
		for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
		{
			Stmt->Body.Add(RaiseTransition(State, Index, Scope));
		}

		for (const FModelState& Child : State.Children)
		{
			Stmt->Body.Add(RaiseState(Child, Chain));
		}

		Chain.Pop();
		return Stmt;
	}
}
