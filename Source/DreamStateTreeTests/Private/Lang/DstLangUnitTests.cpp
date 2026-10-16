// Copyright DreamMoon. All Rights Reserved.
//
// Small checks of the front end's building blocks that the corpus cannot express well: source positions, the shape
// the parser gives a file, single expressions, operator precedence, the `and` / `or` / `not` rule, trivia, paths,
// integers, cloning, and the printer's helpers. The parser's shape tests assert on the tree through the public API,
// so they stand on their own and do not wait for a golden to be reviewed.

#include "DstLangTestSupport.h"

#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstParser.h"
#include "Lang/DstPrinter.h"
#include "Lang/DstSource.h"
#include "Misc/AutomationTest.h"

namespace UE::DreamStateTree::Tests::UnitTests
{
	using namespace UE::DreamStateTree::Lang;
	using namespace UE::DreamStateTree::Tests::LangSupport;

	namespace
	{
		struct FParsed
		{
			FDstSourceText Source;
			FDstDiagnosticSink Sink;
			TUniquePtr<FDstFile> File;
		};

		void ParseText(const FString& Text, FParsed& Out, const FDstParseOptions& Options = FDstParseOptions())
		{
			Out.Source = FDstSourceText(TEXT("unit.dtr"), Text);
			Out.Sink = FDstDiagnosticSink(TEXT("unit.dtr"));
			Out.File = ParseDst(Out.Source, Out.Sink, Options);
		}

		FDstExprPtr ParseExpr(const FString& Text, FDstDiagnosticSink& Sink)
		{
			const FDstSourceText Source(TEXT("expr"), Text);
			return ParseDstExpression(Source, Sink);
		}

		/** Node as T, or an error that says what was there instead. */
		template <typename T>
		const T* ExpectNode(FAutomationTestBase& Test, const FDstNode* Node, const TCHAR* What)
		{
			const T* Result = Node ? Node->As<T>() : nullptr;
			if (!Result)
			{
				Test.AddError(FString::Printf(TEXT("%s: expected a %s, got %s."), What, LexToString(T::StaticKind), Node ? LexToString(Node->Kind) : TEXT("nothing")));
			}
			return Result;
		}

		const FDstStateStmt* FindStateIn(const TArray<FDstStmtPtr>& Statements, const TCHAR* Name)
		{
			for (const FDstStmtPtr& Statement : Statements)
			{
				const FDstStateStmt* State = Statement->As<FDstStateStmt>();
				if (State && State->Name == Name)
				{
					return State;
				}
			}
			return nullptr;
		}

		const FDstSectionStmt* FindSection(const FDstDocument& Document, const TCHAR* Name)
		{
			for (const FDstStmtPtr& Statement : Document.Members)
			{
				const FDstSectionStmt* Section = Statement->As<FDstSectionStmt>();
				if (Section && Section->Name == Name)
				{
					return Section;
				}
			}
			return nullptr;
		}

		/** Spelled here rather than taken from LexToString, so these tests do not depend on how that names a kind. */
		FString ClauseName(const EDstClauseKind Kind)
		{
			switch (Kind)
			{
			case EDstClauseKind::On: return TEXT("On");
			case EDstClauseKind::If: return TEXT("If");
			case EDstClauseKind::Pick: return TEXT("Pick");
			case EDstClauseKind::Utility: return TEXT("Utility");
			case EDstClauseKind::Link: return TEXT("Link");
			case EDstClauseKind::Do: return TEXT("Do");
			case EDstClauseKind::Transition: return TEXT("Transition");
			default: return TEXT("?");
			}
		}

		/** The clauses of a state body, in order, with their kind names joined: `On If Pick`. States and regions show as `State` and `Region`. */
		FString BodyKinds(const FDstStateStmt& State)
		{
			TArray<FString> Parts;
			for (const FDstStmtPtr& Statement : State.Body)
			{
				if (const FDstClauseStmt* Clause = Statement->As<FDstClauseStmt>())
				{
					Parts.Add(ClauseName(Clause->ClauseKind));
				}
				else if (Statement->Is<FDstStateStmt>())
				{
					Parts.Add(FString(TEXT("State")));
				}
				else if (Statement->Is<FDstRegionStmt>())
				{
					Parts.Add(FString(TEXT("Region")));
				}
				else
				{
					Parts.Add(FString(LexToString(Statement->Kind)));
				}
			}
			return FString::Join(Parts, TEXT(" "));
		}

		const FDstClauseStmt* ClauseAt(const FDstStateStmt& State, const int32 Index)
		{
			return State.Body.IsValidIndex(Index) ? State.Body[Index]->As<FDstClauseStmt>() : nullptr;
		}

		FString ShapeOrNone(const FDstExprPtr& Expr)
		{
			return Expr ? ShapeOf(*Expr) : FString(TEXT("<none>"));
		}

		/** Each of the strings of a table must be what the function says; one failure per entry. */
		struct FShapeCase
		{
			const TCHAR* Source;
			const TCHAR* Shape;
		};
	}

	// ------------------------------------------------------------------------------------------
	// Source text
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangSourcePositionsTest, "DreamStateTree.Lang.Unit.SourcePositions",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangSourcePositionsTest::RunTest(const FString& Parameters)
	{
		// \r\n, a lone \r and \n each end one line; a byte-order mark is not part of the text.
		FString Text;
		Text.AppendChar(static_cast<TCHAR>(0xFEFF));
		Text += TEXT("a\r\nbb\rccc\nd");
		const FDstSourceText Source(TEXT("t.dtr"), Text);
		TestEqualSensitive(TEXT("path"), Source.GetPath(), FString(TEXT("t.dtr")));
		TestEqualSensitive(TEXT("BOM stripped"), Source.GetText().Left(1), FString(TEXT("a")));
		TestEqual(TEXT("length without the BOM"), Source.Len(), 11);
		TestEqual(TEXT("line count"), Source.GetLineCount(), 4);

		int32 Line = 0;
		int32 Column = 0;
		const int32 OffsetOfC = Source.GetText().Find(TEXT("ccc"));
		Source.GetLineAndColumn(OffsetOfC + 1, Line, Column);
		TestEqual(TEXT("line of the second c"), Line, 3);
		TestEqual(TEXT("column of the second c"), Column, 2);

		Source.GetLineAndColumn(0, Line, Column);
		TestTrue(TEXT("the first character is (1,1)"), Line == 1 && Column == 1);
		Source.GetLineAndColumn(100, Line, Column);
		TestTrue(TEXT("an offset past the end maps to the last position"), Line == 4 && Column == 2);

		TestEqualSensitive(TEXT("line 1 text"), Source.GetLineText(1), FString(TEXT("a")));
		TestEqualSensitive(TEXT("line 2 text"), Source.GetLineText(2), FString(TEXT("bb")));
		TestEqualSensitive(TEXT("line 3 text"), Source.GetLineText(3), FString(TEXT("ccc")));
		TestEqualSensitive(TEXT("line 4 text"), Source.GetLineText(4), FString(TEXT("d")));
		TestEqualSensitive(TEXT("line 0 is empty"), Source.GetLineText(0), FString());
		TestEqualSensitive(TEXT("a line past the end is empty"), Source.GetLineText(5), FString());

		const FDstSpan Span = Source.MakeSpan(OffsetOfC, 3);
		TestTrue(TEXT("a span knows its line and column"), Span.Line == 3 && Span.Column == 1 && Span.Offset == OffsetOfC && Span.Length == 3);
		TestEqualSensitive(TEXT("slice"), Source.Slice(Span), FString(TEXT("ccc")));
		TestEqual(TEXT("End"), Span.End(), OffsetOfC + 3);

		const FDstSpan Clamped = Source.MakeSpan(100, 5);
		TestTrue(TEXT("a span past the end is clamped"), Clamped.Offset == Source.Len() && Clamped.Length == 0 && Clamped.IsEmpty());
		TestEqualSensitive(TEXT("slice of a clamped span"), Source.Slice(Clamped), FString());

		const FDstSpan Second = Source.MakeSpan(3, 2);
		const FDstSpan Joined = FDstSpan::Join(Second, Span);
		TestTrue(TEXT("Join covers both"), Joined.Offset == 3 && Joined.End() == OffsetOfC + 3 && Joined.Line == 2 && Joined.Column == 1);
		const FDstSpan JoinedBackwards = FDstSpan::Join(Span, Second);
		TestTrue(TEXT("Join does not care about the order"), JoinedBackwards.Offset == Joined.Offset && JoinedBackwards.Length == Joined.Length
			&& JoinedBackwards.Line == Joined.Line && JoinedBackwards.Column == Joined.Column);

		const FDstSourceText Empty(TEXT("e.dtr"), FString());
		TestEqual(TEXT("an empty text has one line"), Empty.GetLineCount(), 1);
		const FDstSourceText Trailing(TEXT("n.dtr"), FString(TEXT("a\n")));
		TestEqual(TEXT("a trailing newline starts a line"), Trailing.GetLineCount(), 2);
		TestEqualSensitive(TEXT("and that line is empty"), Trailing.GetLineText(2), FString());
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangFileKindTest, "DreamStateTree.Lang.Unit.FileKind",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangFileKindTest::RunTest(const FString& Parameters)
	{
		struct FCase
		{
			const TCHAR* Path;
			EDstFileKind Kind;
		};
		const FCase Cases[] =
		{
			{ TEXT("AI/ST_Brawler.dtr"), EDstFileKind::StateTree },
			{ TEXT("AI/ST_Brawler.DTR"), EDstFileKind::StateTree },
			{ TEXT("C:/Some.Dir/ST_Brawler.dtr"), EDstFileKind::StateTree },
			{ TEXT("lib/VillagerCommon.dth"), EDstFileKind::Library },
			{ TEXT("lib/VillagerCommon.DTH"), EDstFileKind::Library },
			{ TEXT("ST_Brawler.dtr.bak"), EDstFileKind::Unknown },
			{ TEXT("ST_Brawler.dtree"), EDstFileKind::Unknown },
			{ TEXT("ST_Brawler"), EDstFileKind::Unknown },
			{ TEXT(""), EDstFileKind::Unknown },
		};
		for (const FCase& Case : Cases)
		{
			TestTrue(FString::Printf(TEXT("`%s` is %s"), Case.Path, LexToString(Case.Kind)), GetDstFileKindFromPath(Case.Path) == Case.Kind);
		}
		TestEqualSensitive(TEXT("StateTree name"), FString(LexToString(EDstFileKind::StateTree)), FString(TEXT("StateTree")));
		TestEqualSensitive(TEXT("Library name"), FString(LexToString(EDstFileKind::Library)), FString(TEXT("Library")));
		TestEqualSensitive(TEXT("Unknown name"), FString(LexToString(EDstFileKind::Unknown)), FString(TEXT("Unknown")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangEnumNamesTest, "DreamStateTree.Lang.Unit.EnumNames",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangEnumNamesTest::RunTest(const FString& Parameters)
	{
		const auto CheckNames = [this](const TCHAR* Enum, const int32 Last, const TFunctionRef<FString(int32)> NameOf)
		{
			TSet<FString> Seen;
			for (int32 Value = 0; Value <= Last; ++Value)
			{
				const FString Name = NameOf(Value);
				TestFalse(FString::Printf(TEXT("%s value %d has a name"), Enum, Value), Name.IsEmpty());
				TestFalse(FString::Printf(TEXT("%s name `%s` is unique"), Enum, *Name), Seen.Contains(Name));
				Seen.Add(Name);
			}
		};
		CheckNames(TEXT("EDstNodeKind"), static_cast<int32>(EDstNodeKind::Document),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstNodeKind>(Value))); });
		CheckNames(TEXT("EDstClauseKind"), static_cast<int32>(EDstClauseKind::Transition),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstClauseKind>(Value))); });
		CheckNames(TEXT("EDstDocumentKind"), static_cast<int32>(EDstDocumentKind::Unknown),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstDocumentKind>(Value))); });
		CheckNames(TEXT("EDstSeverity"), static_cast<int32>(EDstSeverity::Info),
			[](const int32 Value) { return FString(LexToString(static_cast<EDstSeverity>(Value))); });

		TestEqualSensitive(TEXT("error name"), FString(LexToString(EDstSeverity::Error)), FString(TEXT("error")));
		TestEqualSensitive(TEXT("warning name"), FString(LexToString(EDstSeverity::Warning)), FString(TEXT("warning")));
		TestEqualSensitive(TEXT("info name"), FString(LexToString(EDstSeverity::Info)), FString(TEXT("info")));
		TestEqualSensitive(TEXT("the transition clause is named Transition"), FString(LexToString(EDstClauseKind::Transition)), FString(TEXT("Transition")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangOptionDefaultsTest, "DreamStateTree.Lang.Unit.OptionDefaults",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangOptionDefaultsTest::RunTest(const FString& Parameters)
	{
		const FDstPrintOptions Print;
		TestEqual(TEXT("indent width"), Print.IndentWidth, 4);
		TestEqual(TEXT("max width"), Print.MaxWidth, 120);
		TestEqualSensitive(TEXT("line end"), Print.LineEnd, FString(TEXT("\n")));
		TestNull(TEXT("canonical printing keeps no layout"), Print.KeepBrokenIn);
		const FDstParseOptions Parse;
		TestTrue(TEXT("trivia is kept by default"), Parse.bKeepTrivia);
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// What the parser makes of a file
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseShapeFullTest, "DreamStateTree.Lang.Unit.ParseShape.Full",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseShapeFullTest::RunTest(const FString& Parameters)
	{
		const FString Text = TEXT(R"DST(#include "Common.dth"
StateTree(Name="AI/ST_Shape", Root="Engine", Schema=AIComponent(Actor = CombatEnemy))
{
    Parameters = { float Range = 200; FVector Spot; }
    Globals = { Info = GetPlayerInfo(MaxRange = 2500); }
    Tags = { Game.Event.Villager; }
    Template Near(Who) = Who.Distance < 100;
    State Root [ Tag = Combat ] {
        on Hit.Stagger
        if ObjectIsValid(Info.Target) and Info.Distance < Params.Range
        pick Random
        utility 0.5
        do Combo = Attack(1) [ Name = "c" ], Wait
        -> Next on Succeeded if not Tired [ Delay = 0.5 ]
        -> Root
        State Child { link asset ST_Other(Speed = 2) }
        State Sub { link HitReaction }
    }
    Subtree HitReaction(float Toughness = 1) {
        State Stagger { do Stagger(0.8) }
    }
}
)DST");
		FParsed Parsed;
		ParseText(Text, Parsed);
		TestEqual(TEXT("no diagnostics"), Parsed.Sink.Num(), 0);
		if (!Parsed.File || !Parsed.File->Document)
		{
			AddError(TEXT("The file has no document."));
			return false;
		}
		const FDstFile& File = *Parsed.File;
		const FDstDocument& Document = *File.Document;

		// The file and its header.
		TestEqualSensitive(TEXT("path"), File.Path, FString(TEXT("unit.dtr")));
		TestEqual(TEXT("one include"), File.Includes.Num(), 1);
		if (File.Includes.Num() == 1)
		{
			TestEqualSensitive(TEXT("include path"), File.Includes[0]->Path, FString(TEXT("Common.dth")));
			TestEqualSensitive(TEXT("include path span covers the string"), Parsed.Source.Slice(File.Includes[0]->PathSpan), FString(TEXT("\"Common.dth\"")));
		}
		TestTrue(TEXT("a StateTree document"), Document.DocumentKind == EDstDocumentKind::StateTree);
		TestEqualSensitive(TEXT("kind name"), Document.KindName, FString(TEXT("StateTree")));
		TestEqualSensitive(TEXT("kind span"), Parsed.Source.Slice(Document.KindSpan), FString(TEXT("StateTree")));
		TestEqual(TEXT("three header arguments"), Document.HeaderArgs.Num(), 3);
		TestEqualSensitive(TEXT("GetHeaderString Name"), Document.GetHeaderString(TEXT("Name")), FString(TEXT("AI/ST_Shape")));
		TestEqualSensitive(TEXT("GetHeaderString Root"), Document.GetHeaderString(TEXT("Root")), FString(TEXT("Engine")));
		TestEqualSensitive(TEXT("GetHeaderString of an absent argument"), Document.GetHeaderString(TEXT("Nope")), FString());
		TestEqualSensitive(TEXT("GetHeaderString of a non-string argument"), Document.GetHeaderString(TEXT("Schema")), FString());
		TestNull(TEXT("FindHeaderArg of an absent argument"), Document.FindHeaderArg(TEXT("Nope")));
		const FDstArgument* Schema = Document.FindHeaderArg(TEXT("Schema"));
		TestNotNull(TEXT("FindHeaderArg Schema"), Schema);
		if (Schema && Schema->Value)
		{
			TestEqualSensitive(TEXT("the schema argument"), ShapeOf(*Schema->Value), FString(TEXT("AIComponent(Actor=CombatEnemy)")));
			TestTrue(TEXT("the schema argument is named"), Schema->IsNamed() && !Schema->bOutput);
		}
		TestEqualSensitive(TEXT("the open brace"), Parsed.Source.Slice(Document.OpenSpan), FString(TEXT("{")));
		TestEqualSensitive(TEXT("the close brace"), Parsed.Source.Slice(Document.CloseSpan), FString(TEXT("}")));

		// Members, in source order.
		TestEqual(TEXT("six members"), Document.Members.Num(), 6);
		if (Document.Members.Num() != 6)
		{
			return false;
		}
		const FDstSectionStmt* ParametersSection = ExpectNode<FDstSectionStmt>(*this, Document.Members[0].Get(), TEXT("member 0"));
		const FDstSectionStmt* Globals = ExpectNode<FDstSectionStmt>(*this, Document.Members[1].Get(), TEXT("member 1"));
		const FDstSectionStmt* Tags = ExpectNode<FDstSectionStmt>(*this, Document.Members[2].Get(), TEXT("member 2"));
		const FDstTemplateStmt* Template = ExpectNode<FDstTemplateStmt>(*this, Document.Members[3].Get(), TEXT("member 3"));
		const FDstStateStmt* Root = ExpectNode<FDstStateStmt>(*this, Document.Members[4].Get(), TEXT("member 4"));
		const FDstStateStmt* Subtree = ExpectNode<FDstStateStmt>(*this, Document.Members[5].Get(), TEXT("member 5"));
		if (!ParametersSection || !Globals || !Tags || !Template || !Root || !Subtree)
		{
			return false;
		}

		// Sections.
		TestEqualSensitive(TEXT("Parameters name"), ParametersSection->Name, FString(TEXT("Parameters")));
		TestEqualSensitive(TEXT("Parameters name span"), Parsed.Source.Slice(ParametersSection->NameSpan), FString(TEXT("Parameters")));
		TestEqual(TEXT("two parameters"), ParametersSection->Entries.Num(), 2);
		if (ParametersSection->Entries.Num() == 2)
		{
			const FDstDeclStmt* Range = ExpectNode<FDstDeclStmt>(*this, ParametersSection->Entries[0].Get(), TEXT("parameter 0"));
			const FDstDeclStmt* Spot = ExpectNode<FDstDeclStmt>(*this, ParametersSection->Entries[1].Get(), TEXT("parameter 1"));
			if (Range && Spot)
			{
				TestEqualSensitive(TEXT("Range type"), Range->TypeName, FString(TEXT("float")));
				TestEqualSensitive(TEXT("Range name"), Range->Name, FString(TEXT("Range")));
				TestEqualSensitive(TEXT("Range default"), ShapeOrNone(Range->Value), FString(TEXT("200")));
				TestEqualSensitive(TEXT("Range type span"), Parsed.Source.Slice(Range->TypeSpan), FString(TEXT("float")));
				TestEqualSensitive(TEXT("Range name span"), Parsed.Source.Slice(Range->NameSpan), FString(TEXT("Range")));
				TestEqualSensitive(TEXT("Spot type"), Spot->TypeName, FString(TEXT("FVector")));
				TestEqualSensitive(TEXT("Spot has no default"), ShapeOrNone(Spot->Value), FString(TEXT("<none>")));
				TestNull(TEXT("Spot has no metadata"), Spot->Meta.Get());
			}
		}
		TestEqual(TEXT("one global"), Globals->Entries.Num(), 1);
		if (Globals->Entries.Num() == 1)
		{
			const FDstAssignStmt* Info = ExpectNode<FDstAssignStmt>(*this, Globals->Entries[0].Get(), TEXT("global 0"));
			if (Info)
			{
				TestEqualSensitive(TEXT("global name"), Info->Name, FString(TEXT("Info")));
				TestEqualSensitive(TEXT("global value"), ShapeOrNone(Info->Value), FString(TEXT("GetPlayerInfo(MaxRange=2500)")));
			}
		}
		TestEqual(TEXT("one tag"), Tags->Entries.Num(), 1);
		if (Tags->Entries.Num() == 1)
		{
			const FDstExprStmt* Tag = ExpectNode<FDstExprStmt>(*this, Tags->Entries[0].Get(), TEXT("tag 0"));
			if (Tag && Tag->Expr)
			{
				TestEqualSensitive(TEXT("tag path"), GetDstPathString(*Tag->Expr), FString(TEXT("Game.Event.Villager")));
			}
		}

		// The template.
		TestEqualSensitive(TEXT("template name"), Template->Name, FString(TEXT("Near")));
		TestTrue(TEXT("the template has a parameter list"), Template->bHasParams);
		TestEqual(TEXT("one template parameter"), Template->Params.Num(), 1);
		if (Template->Params.Num() == 1)
		{
			TestEqualSensitive(TEXT("parameter name"), Template->Params[0]->Name, FString(TEXT("Who")));
			TestEqualSensitive(TEXT("a template parameter has no type"), Template->Params[0]->TypeName, FString());
			TestNull(TEXT("and no default"), Template->Params[0]->Default.Get());
		}
		TestEqualSensitive(TEXT("template body"), ShapeOrNone(Template->Body), FString(TEXT("(Who.Distance < 100)")));

		// The state.
		TestFalse(TEXT("Root is not a subtree"), Root->bSubtree);
		TestEqualSensitive(TEXT("Root name"), Root->Name, FString(TEXT("Root")));
		TestFalse(TEXT("Root name is not quoted"), Root->bNameQuoted);
		TestFalse(TEXT("Root has no parameter list"), Root->bHasParams);
		TestEqualSensitive(TEXT("Root word span"), Parsed.Source.Slice(Root->WordSpan), FString(TEXT("State")));
		TestEqualSensitive(TEXT("Root name span"), Parsed.Source.Slice(Root->NameSpan), FString(TEXT("Root")));
		TestEqualSensitive(TEXT("Root open brace"), Parsed.Source.Slice(Root->OpenSpan), FString(TEXT("{")));
		TestEqualSensitive(TEXT("Root close brace"), Parsed.Source.Slice(Root->CloseSpan), FString(TEXT("}")));
		{
			const FString Whole = Parsed.Source.Slice(Root->Span);
			TestTrue(TEXT("Root span starts at `State Root [`"), Whole.StartsWith(TEXT("State Root [ Tag = Combat ] {")));
			TestTrue(TEXT("Root span ends at its closing brace"), Whole.EndsWith(TEXT("}")));
		}
		TestNotNull(TEXT("Root has metadata"), Root->Meta.Get());
		if (Root->Meta)
		{
			const FDstMetaEntry* TagEntry = Root->Meta->Find(TEXT("Tag"));
			TestNotNull(TEXT("Find Tag"), TagEntry);
			if (TagEntry)
			{
				TestEqualSensitive(TEXT("Tag key span"), Parsed.Source.Slice(TagEntry->KeySpan), FString(TEXT("Tag")));
				TestEqualSensitive(TEXT("Tag value"), ShapeOrNone(TagEntry->Value), FString(TEXT("Combat")));
			}
			TestNull(TEXT("Find is case-sensitive"), Root->Meta->Find(TEXT("tag")));
			TestNull(TEXT("Find of an absent key"), Root->Meta->Find(TEXT("Color")));
		}

		TestEqualSensitive(TEXT("Root body, in source order"), BodyKinds(*Root), FString(TEXT("On If Pick Utility Do Transition Transition State State")));
		const FDstClauseStmt* On = ClauseAt(*Root, 0);
		const FDstClauseStmt* If = ClauseAt(*Root, 1);
		const FDstClauseStmt* Pick = ClauseAt(*Root, 2);
		const FDstClauseStmt* Utility = ClauseAt(*Root, 3);
		const FDstClauseStmt* Do = ClauseAt(*Root, 4);
		const FDstClauseStmt* Next = ClauseAt(*Root, 5);
		const FDstClauseStmt* Loop = ClauseAt(*Root, 6);
		if (!On || !If || !Pick || !Utility || !Do || !Next || !Loop)
		{
			return false;
		}
		TestEqualSensitive(TEXT("on"), ShapeOrNone(On->Value), FString(TEXT("Hit.Stagger")));
		TestEqualSensitive(TEXT("on keyword span"), Parsed.Source.Slice(On->KeywordSpan), FString(TEXT("on")));
		TestEqualSensitive(TEXT("if"), ShapeOrNone(If->Value), FString(TEXT("(ObjectIsValid(Info.Target) and (Info.Distance < Params.Range))")));
		TestEqualSensitive(TEXT("if keyword span"), Parsed.Source.Slice(If->KeywordSpan), FString(TEXT("if")));
		TestEqualSensitive(TEXT("pick"), ShapeOrNone(Pick->Value), FString(TEXT("Random")));
		TestEqualSensitive(TEXT("pick keyword span"), Parsed.Source.Slice(Pick->KeywordSpan), FString(TEXT("pick")));
		TestEqualSensitive(TEXT("utility"), ShapeOrNone(Utility->Value), FString(TEXT("0.5")));
		TestEqualSensitive(TEXT("utility keyword span"), Parsed.Source.Slice(Utility->KeywordSpan), FString(TEXT("utility")));
		TestEqualSensitive(TEXT("do keyword span"), Parsed.Source.Slice(Do->KeywordSpan), FString(TEXT("do")));

		TestEqual(TEXT("two tasks"), Do->Tasks.Num(), 2);
		if (Do->Tasks.Num() == 2)
		{
			TestEqualSensitive(TEXT("task 0 bind name"), Do->Tasks[0]->BindName, FString(TEXT("Combo")));
			TestEqualSensitive(TEXT("task 0 bind name span"), Parsed.Source.Slice(Do->Tasks[0]->BindNameSpan), FString(TEXT("Combo")));
			TestEqualSensitive(TEXT("task 0 node, settings included"), ShapeOrNone(Do->Tasks[0]->Node), FString(TEXT("Attack(1)<Name=\"c\">")));
			TestEqualSensitive(TEXT("task 1 has no bind name"), Do->Tasks[1]->BindName, FString());
			TestEqualSensitive(TEXT("task 1 is a bare name"), ShapeOrNone(Do->Tasks[1]->Node), FString(TEXT("Wait")));
			TestTrue(TEXT("and a NameExpr node"), Do->Tasks[1]->Node && Do->Tasks[1]->Node->Is<FDstNameExpr>());
		}

		TestEqualSensitive(TEXT("-> keyword span"), Parsed.Source.Slice(Next->KeywordSpan), FString(TEXT("->")));
		TestEqualSensitive(TEXT("transition target"), ShapeOrNone(Next->Value), FString(TEXT("Next")));
		TestEqualSensitive(TEXT("transition trigger"), ShapeOrNone(Next->Trigger), FString(TEXT("Succeeded")));
		TestEqualSensitive(TEXT("transition on span"), Parsed.Source.Slice(Next->OnSpan), FString(TEXT("on")));
		TestEqualSensitive(TEXT("transition condition"), ShapeOrNone(Next->Condition), FString(TEXT("(not Tired)")));
		TestEqualSensitive(TEXT("transition if span"), Parsed.Source.Slice(Next->IfSpan), FString(TEXT("if")));
		TestEqualSensitive(TEXT("transition options"), ShapeOfMeta(Next->Meta.Get()), FString(TEXT("<Delay=0.5>")));
		TestEqualSensitive(TEXT("plain transition target"), ShapeOrNone(Loop->Value), FString(TEXT("Root")));
		TestNull(TEXT("plain transition: no trigger"), Loop->Trigger.Get());
		TestNull(TEXT("plain transition: no condition"), Loop->Condition.Get());
		TestNull(TEXT("plain transition: no options"), Loop->Meta.Get());

		// Child states and links.
		if (Root->Body.Num() < 9)
		{
			AddError(TEXT("Root's body should hold nine items."));
			return false;
		}
		const FDstStateStmt* Child = Root->Body[7]->As<FDstStateStmt>();
		const FDstStateStmt* Sub = Root->Body[8]->As<FDstStateStmt>();
		if (Child && Sub)
		{
			TestEqualSensitive(TEXT("Child name"), Child->Name, FString(TEXT("Child")));
			TestEqualSensitive(TEXT("Child body"), BodyKinds(*Child), FString(TEXT("Link")));
			const FDstClauseStmt* ChildLink = ClauseAt(*Child, 0);
			if (ChildLink)
			{
				TestTrue(TEXT("link asset"), ChildLink->bLinkAsset);
				TestEqualSensitive(TEXT("link asset span"), Parsed.Source.Slice(ChildLink->AssetSpan), FString(TEXT("asset")));
				TestEqualSensitive(TEXT("link asset target"), ShapeOrNone(ChildLink->Value), FString(TEXT("ST_Other(Speed=2)")));
			}
			const FDstClauseStmt* SubLink = ClauseAt(*Sub, 0);
			if (SubLink)
			{
				TestFalse(TEXT("link, not link asset"), SubLink->bLinkAsset);
				TestEqualSensitive(TEXT("link target"), ShapeOrNone(SubLink->Value), FString(TEXT("HitReaction")));
			}
		}
		else
		{
			AddError(TEXT("Root's last two body items should be the states Child and Sub."));
		}

		// The subtree.
		TestTrue(TEXT("the subtree is a subtree"), Subtree->bSubtree);
		TestEqualSensitive(TEXT("subtree word span"), Parsed.Source.Slice(Subtree->WordSpan), FString(TEXT("Subtree")));
		TestEqualSensitive(TEXT("subtree name"), Subtree->Name, FString(TEXT("HitReaction")));
		TestTrue(TEXT("the subtree has a parameter list"), Subtree->bHasParams);
		TestEqual(TEXT("one subtree parameter"), Subtree->Params.Num(), 1);
		if (Subtree->Params.Num() == 1)
		{
			TestEqualSensitive(TEXT("parameter type"), Subtree->Params[0]->TypeName, FString(TEXT("float")));
			TestEqualSensitive(TEXT("parameter name"), Subtree->Params[0]->Name, FString(TEXT("Toughness")));
			TestEqualSensitive(TEXT("parameter default"), ShapeOrNone(Subtree->Params[0]->Default), FString(TEXT("1")));
		}
		TestEqualSensitive(TEXT("subtree body"), BodyKinds(*Subtree), FString(TEXT("State")));
		TestNotNull(TEXT("FindStateIn finds a nested state"), FindStateIn(Subtree->Body, TEXT("Stagger")));
		TestNotNull(TEXT("FindSection finds Globals"), FindSection(Document, TEXT("Globals")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseShapeCardsTest, "DreamStateTree.Lang.Unit.ParseShape.Cards",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseShapeCardsTest::RunTest(const FString& Parameters)
	{
		// Line breaks carry no meaning: a clause ends at the next clause keyword, and an `and` at the start of a line goes on
		// with the line before it.
		const FString Text = TEXT(R"DST(StateTree(Name="T/Cards", Schema=Villager)
{
    State A { on Hit.Stagger  do Stagger(0.8) }
    State B {
        if Player.Sees
           and Player.Distance < 800
        do Flee(From = Player.Pawn,
                Distance = 2000)
        -> Root on Tick
    }
    State C { do A(), B()  -> A  -> B }
    State D { -> Root if X
              or Y
              do Wait(1) }
}
)DST");
		FParsed Parsed;
		ParseText(Text, Parsed);
		TestEqual(TEXT("no diagnostics"), Parsed.Sink.Num(), 0);
		if (!Parsed.File || !Parsed.File->Document)
		{
			AddError(TEXT("The file has no document."));
			return false;
		}
		const FDstDocument& Document = *Parsed.File->Document;
		const FDstStateStmt* A = FindStateIn(Document.Members, TEXT("A"));
		const FDstStateStmt* B = FindStateIn(Document.Members, TEXT("B"));
		const FDstStateStmt* C = FindStateIn(Document.Members, TEXT("C"));
		const FDstStateStmt* D = FindStateIn(Document.Members, TEXT("D"));
		if (!A || !B || !C || !D)
		{
			AddError(TEXT("The states A, B, C and D should all parse."));
			return false;
		}
		TestEqualSensitive(TEXT("A is a one-line card"), BodyKinds(*A), FString(TEXT("On Do")));
		TestEqualSensitive(TEXT("B"), BodyKinds(*B), FString(TEXT("If Do Transition")));
		TestEqualSensitive(TEXT("C"), BodyKinds(*C), FString(TEXT("Do Transition Transition")));
		TestEqualSensitive(TEXT("D"), BodyKinds(*D), FString(TEXT("Transition Do")));

		if (const FDstClauseStmt* If = ClauseAt(*B, 0))
		{
			TestEqualSensitive(TEXT("an `and` at the start of a line continues the condition"), ShapeOrNone(If->Value),
				FString(TEXT("(Player.Sees and (Player.Distance < 800))")));
		}
		if (const FDstClauseStmt* Do = ClauseAt(*B, 1))
		{
			TestEqual(TEXT("one task"), Do->Tasks.Num(), 1);
			if (Do->Tasks.Num() == 1)
			{
				TestEqualSensitive(TEXT("a call may continue on the next line"), ShapeOrNone(Do->Tasks[0]->Node),
					FString(TEXT("Flee(From=Player.Pawn,Distance=2000)")));
			}
		}
		if (const FDstClauseStmt* Do = ClauseAt(*C, 0))
		{
			TestEqual(TEXT("two tasks on a `do`"), Do->Tasks.Num(), 2);
		}
		if (const FDstClauseStmt* First = ClauseAt(*C, 1))
		{
			TestEqualSensitive(TEXT("first transition of C"), ShapeOrNone(First->Value), FString(TEXT("A")));
		}
		if (const FDstClauseStmt* Second = ClauseAt(*C, 2))
		{
			TestEqualSensitive(TEXT("second transition of C"), ShapeOrNone(Second->Value), FString(TEXT("B")));
		}
		if (const FDstClauseStmt* Transition = ClauseAt(*D, 0))
		{
			TestEqualSensitive(TEXT("a transition condition goes on over lines"), ShapeOrNone(Transition->Condition), FString(TEXT("(X or Y)")));
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseShapeOrderTest, "DreamStateTree.Lang.Unit.ParseShape.Order",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseShapeOrderTest::RunTest(const FString& Parameters)
	{
		// The parser keeps the order the author wrote; it is the printer that puts clauses before child states.
		const FString Text = TEXT(R"DST(StateTree(Name="T/Order", Schema=Villager)
{
    State A {
        State Child { }
        do Wait(1)
        #Region "later"
        -> Child
        #EndRegion
    }
}
)DST");
		FParsed Parsed;
		ParseText(Text, Parsed);
		TestEqual(TEXT("no diagnostics"), Parsed.Sink.Num(), 0);
		if (!Parsed.File || !Parsed.File->Document)
		{
			AddError(TEXT("The file has no document."));
			return false;
		}
		const FDstStateStmt* A = FindStateIn(Parsed.File->Document->Members, TEXT("A"));
		if (!A)
		{
			AddError(TEXT("State A should parse."));
			return false;
		}
		TestEqualSensitive(TEXT("source order"), BodyKinds(*A), FString(TEXT("State Do Region Transition Region")));
		if (A->Body.Num() == 5)
		{
			const FDstRegionStmt* Open = ExpectNode<FDstRegionStmt>(*this, A->Body[2].Get(), TEXT("body item 2"));
			const FDstRegionStmt* Close = ExpectNode<FDstRegionStmt>(*this, A->Body[4].Get(), TEXT("body item 4"));
			if (Open && Close)
			{
				TestFalse(TEXT("#Region is not an end"), Open->bEnd);
				TestEqualSensitive(TEXT("#Region label"), Open->Label, FString(TEXT("later")));
				TestTrue(TEXT("#EndRegion is an end"), Close->bEnd);
				TestEqualSensitive(TEXT("#EndRegion has no label"), Close->Label, FString());
			}
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseShapeNamesTest, "DreamStateTree.Lang.Unit.ParseShape.Names",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseShapeNamesTest::RunTest(const FString& Parameters)
	{
		const FString Text = TEXT(R"DST(StateTree(Name="T/Names", Schema=Villager)
{
    State `Move To` { -> `Idle`.`Phone Call` }
    State 空闲 { -> Next  -> `Next` }
}
)DST");
		FParsed Parsed;
		ParseText(Text, Parsed);
		TestEqual(TEXT("no diagnostics"), Parsed.Sink.Num(), 0);
		if (!Parsed.File || !Parsed.File->Document)
		{
			AddError(TEXT("The file has no document."));
			return false;
		}
		const FDstStateStmt* Move = FindStateIn(Parsed.File->Document->Members, TEXT("Move To"));
		const FDstStateStmt* Idle = FindStateIn(Parsed.File->Document->Members, TEXT("空闲"));
		TestNotNull(TEXT("a state named with backticks"), Move);
		TestNotNull(TEXT("a state with a Chinese name"), Idle);
		if (Move)
		{
			TestTrue(TEXT("the backtick name is marked"), Move->bNameQuoted);
			TestEqualSensitive(TEXT("and its span covers the backticks"), Parsed.Source.Slice(Move->NameSpan), FString(TEXT("`Move To`")));
			if (const FDstClauseStmt* Transition = ClauseAt(*Move, 0))
			{
				TestEqualSensitive(TEXT("a path through quoted names"), ShapeOrNone(Transition->Value), FString(TEXT("`Idle`.`Phone Call`")));
			}
		}
		if (Idle)
		{
			TestFalse(TEXT("a Chinese name needs no backticks"), Idle->bNameQuoted);
			const FDstClauseStmt* Keyword = ClauseAt(*Idle, 0);
			const FDstClauseStmt* Quoted = ClauseAt(*Idle, 1);
			if (Keyword && Quoted)
			{
				// `Next` and `Next` look alike to the parser; what tells a state called Next from the target keyword is the backticks.
				const FDstNameExpr* KeywordName = Keyword->Value ? Keyword->Value->As<FDstNameExpr>() : nullptr;
				const FDstNameExpr* QuotedName = Quoted->Value ? Quoted->Value->As<FDstNameExpr>() : nullptr;
				TestTrue(TEXT("-> Next is a bare name"), KeywordName && KeywordName->Name == TEXT("Next") && !KeywordName->bQuoted);
				TestTrue(TEXT("-> `Next` is a quoted name"), QuotedName && QuotedName->Name == TEXT("Next") && QuotedName->bQuoted);
			}
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseTransitionBracketsTest, "DreamStateTree.Lang.Unit.ParseShape.TransitionBrackets",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseTransitionBracketsTest::RunTest(const FString& Parameters)
	{
		// A `[ ... ]` after the call that ends a transition's condition is the transition's options; settings for that call
		// go in front of them, or the call goes in parentheses. Anywhere else a bracket after a call is that call's settings.
		const FString Text = TEXT(R"DST(StateTree(Name="T/Brackets", Schema=Villager)
{
    State A {
        -> Root on Tick if Hour(22, 5) [ Delay = 1 ]
        -> Root on Tick if Hour(22, 5) [ Name = "Night" ] [ Delay = 1 ]
        -> Root on Tick if (Hour(22, 5)) [ Delay = 1 ]
        -> Root if Random(0.3) [ Evaluation = ForcedTrue ] and Alert [ Delay = 2 ]
        -> Root on Tick if not Hour(22, 5) [ Delay = 1 ]
        if Hour(22, 5) [ Name = "Night" ]
    }
}
)DST");
		FParsed Parsed;
		ParseText(Text, Parsed);
		TestEqual(TEXT("no diagnostics"), Parsed.Sink.Num(), 0);
		const FDstStateStmt* A = Parsed.File && Parsed.File->Document ? FindStateIn(Parsed.File->Document->Members, TEXT("A")) : nullptr;
		if (!A)
		{
			AddError(TEXT("State A should parse."));
			return false;
		}
		TestEqualSensitive(TEXT("six clauses, in source order"), BodyKinds(*A), FString(TEXT("Transition Transition Transition Transition Transition If")));

		struct FExpected
		{
			const TCHAR* Condition;
			const TCHAR* Options;
		};
		const FExpected Expected[] =
		{
			{ TEXT("Hour(22,5)"), TEXT("<Delay=1>") },
			{ TEXT("Hour(22,5)<Name=\"Night\">"), TEXT("<Delay=1>") },
			{ TEXT("paren(Hour(22,5))"), TEXT("<Delay=1>") },
			{ TEXT("(Random(0.3)<Evaluation=ForcedTrue> and Alert)"), TEXT("<Delay=2>") },
			{ TEXT("(not Hour(22,5))"), TEXT("<Delay=1>") },
		};
		for (int32 Index = 0; Index < 5; ++Index)
		{
			const FDstClauseStmt* Clause = ClauseAt(*A, Index);
			if (!Clause)
			{
				AddError(FString::Printf(TEXT("Clause %d should be a transition."), Index));
				continue;
			}
			TestEqualSensitive(FString::Printf(TEXT("transition %d: the condition"), Index), ShapeOrNone(Clause->Condition), FString(Expected[Index].Condition));
			TestEqualSensitive(FString::Printf(TEXT("transition %d: the options"), Index), ShapeOfMeta(Clause->Meta.Get()), FString(Expected[Index].Options));
		}

		// In an `if` clause the bracket after a call is always that call's.
		if (const FDstClauseStmt* If = ClauseAt(*A, 5))
		{
			TestEqualSensitive(TEXT("an if clause: the call's settings"), ShapeOrNone(If->Value), FString(TEXT("Hour(22,5)<Name=\"Night\">")));
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseDocumentKindsTest, "DreamStateTree.Lang.Unit.ParseDocumentKinds",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseDocumentKindsTest::RunTest(const FString& Parameters)
	{
		{
			FParsed Parsed;
			ParseText(FString(), Parsed);
			TestNotNull(TEXT("an empty text still gives a file"), Parsed.File.Get());
			TestTrue(TEXT("and no document"), Parsed.File && !Parsed.File->Document);
			TestTrue(TEXT("DST2002"), CountDiagnostics(Parsed.Sink, TEXT("DST2002")) == 1 && Parsed.Sink.HasErrors());
		}
		{
			FParsed Parsed;
			ParseText(TEXT("// nothing here\n"), Parsed);
			TestTrue(TEXT("a comment is no document: DST2002"), CountDiagnostics(Parsed.Sink, TEXT("DST2002")) == 1);
		}
		{
			FParsed Parsed;
			ParseText(TEXT("StateTreeLibrary(Name=\"AI/Common\")\n{\n}\n"), Parsed);
			TestEqual(TEXT("library: no diagnostics"), Parsed.Sink.Num(), 0);
			TestTrue(TEXT("a library"), Parsed.File && Parsed.File->Document && Parsed.File->Document->DocumentKind == EDstDocumentKind::StateTreeLibrary);
			if (Parsed.File && Parsed.File->Document)
			{
				TestEqual(TEXT("library: no members"), Parsed.File->Document->Members.Num(), 0);
				TestEqual(TEXT("library: one header argument"), Parsed.File->Document->HeaderArgs.Num(), 1);
				TestEqualSensitive(TEXT("library: kind name"), Parsed.File->Document->KindName, FString(TEXT("StateTreeLibrary")));
			}
		}
		{
			FParsed Parsed;
			ParseText(TEXT("Widget(Name=\"X\")\n{\n}\n"), Parsed);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Parsed.Sink, TEXT("DST2004"));
			TestNotNull(TEXT("an unknown kind: DST2004"), Diagnostic);
			if (Diagnostic)
			{
				TestTrue(TEXT("DST2004 is on the kind word"), Diagnostic->Span.Line == 1 && Diagnostic->Span.Column == 1);
			}
			TestTrue(TEXT("the document is still there, with Unknown kind"),
				Parsed.File && Parsed.File->Document && Parsed.File->Document->DocumentKind == EDstDocumentKind::Unknown
				&& Parsed.File->Document->KindName == TEXT("Widget"));
		}
		{
			// The file's path is the source's.
			FParsed Parsed;
			ParseText(TEXT("StateTree(Name=\"T\", Schema=Villager)\n{\n}\n"), Parsed);
			TestEqualSensitive(TEXT("path"), Parsed.File->Path, FString(TEXT("unit.dtr")));
			TestEqual(TEXT("no includes"), Parsed.File->Includes.Num(), 0);
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Expressions
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseExpressionTest, "DreamStateTree.Lang.Unit.ParseExpression",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseExpressionTest::RunTest(const FString& Parameters)
	{
		// Precedence, high to low: unary, * /, + -, comparisons, == !=, and, or (GetDstBinaryOpPrecedence); and `not` first of all.
		const FShapeCase Cases[] =
		{
			{ TEXT("not a and b or c"), TEXT("(((not a) and b) or c)") },
			{ TEXT("a or b and c"), TEXT("(a or (b and c))") },
			{ TEXT("(a or b) and c"), TEXT("(paren((a or b)) and c)") },
			{ TEXT("a and not b"), TEXT("(a and (not b))") },
			{ TEXT("a + 1 < b * 2"), TEXT("((a + 1) < (b * 2))") },
			{ TEXT("a == b and c != d"), TEXT("((a == b) and (c != d))") },
			{ TEXT("a < b == c"), TEXT("((a < b) == c)") },
			{ TEXT("a - b - c"), TEXT("((a - b) - c)") },
			{ TEXT("a / b * c"), TEXT("((a / b) * c)") },
			{ TEXT("a + b * c"), TEXT("(a + (b * c))") },
			{ TEXT("-a * b"), TEXT("((-a) * b)") },
			{ TEXT("1 - -2"), TEXT("(1 - (-2))") },
			{ TEXT("not a == b"), TEXT("((not a) == b)") },
			{ TEXT("Params.AttackRange * 0.75"), TEXT("(Params.AttackRange * 0.75)") },
			{ TEXT("Chance(1 - Params.Courage)"), TEXT("Chance((1 - Params.Courage))") },
			{ TEXT("FloatInput(Player.Distance, (0, 3000)) * 0.5"), TEXT("(FloatInput(Player.Distance,tuple(0,3000)) * 0.5)") },
			{ TEXT("a.b.c"), TEXT("a.b.c") },
			{ TEXT("`Move To`.x"), TEXT("`Move To`.x") },
			{ TEXT("Info.Dist(1)"), TEXT("Info.Dist(1)") },
			{ TEXT("F()"), TEXT("F()") },
			{ TEXT("F(1, X = 2)"), TEXT("F(1,X=2)") },
			{ TEXT("F(Dist -> Params.D)"), TEXT("F(Dist->Params.D)") },
			{ TEXT("Delegate(Combo.Succeeds)"), TEXT("Delegate(Combo.Succeeds)") },
			{ TEXT("Walk(1) [ Name = \"x\"; Enabled = false ]"), TEXT("Walk(1)<Name=\"x\";Enabled=false>") },
			{ TEXT("(0, 3000)"), TEXT("tuple(0,3000)") },
			{ TEXT("(1)"), TEXT("paren(1)") },
			{ TEXT("(X = 1)"), TEXT("tuple(X=1)") },
			{ TEXT("(X = 1, Y = 2)"), TEXT("tuple(X=1,Y=2)") },
			{ TEXT("[]"), TEXT("list()") },
			{ TEXT("[1, 2, 3]"), TEXT("list(1,2,3)") },
			{ TEXT("ref Params.Spot"), TEXT("ref Params.Spot") },
			{ TEXT("true"), TEXT("true") },
			{ TEXT("None"), TEXT("None") },
			{ TEXT("\"a\\nb\""), TEXT("\"a\nb\"") },
			{ TEXT(".5 + 1e3"), TEXT("(.5 + 1e3)") },
			{ TEXT("  Hit.Stagger  // trailing comment"), TEXT("Hit.Stagger") },
		};
		for (const FShapeCase& Case : Cases)
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(Case.Source, Sink);
			if (!Expr || Sink.HasErrors())
			{
				AddError(FString::Printf(TEXT("`%s` should parse: %s"), Case.Source, *DescribeDiagnostics(Sink)));
				continue;
			}
			TestEqualSensitive(FString::Printf(TEXT("shape of `%s`"), Case.Source), ShapeOf(*Expr), FString(Case.Shape));
		}

		{
			// Literals know their kind.
			FDstDiagnosticSink Sink;
			const FDstExprPtr BoolExpr = ParseExpr(TEXT("true"), Sink);
			const FDstExprPtr NumberExpr = ParseExpr(TEXT("2.50"), Sink);
			const FDstExprPtr StringExpr = ParseExpr(TEXT("\"hi\""), Sink);
			const FDstExprPtr Capital = ParseExpr(TEXT("True"), Sink);
			const FDstLiteralExpr* TrueLiteral = BoolExpr ? BoolExpr->As<FDstLiteralExpr>() : nullptr;
			const FDstLiteralExpr* NumberLiteral = NumberExpr ? NumberExpr->As<FDstLiteralExpr>() : nullptr;
			const FDstLiteralExpr* StringLiteral = StringExpr ? StringExpr->As<FDstLiteralExpr>() : nullptr;
			TestTrue(TEXT("true is a Bool literal"), TrueLiteral && TrueLiteral->LiteralKind == EDstLiteralKind::Bool && TrueLiteral->bBool);
			TestTrue(TEXT("2.50 is a Number literal that keeps its lexeme"),
				NumberLiteral && NumberLiteral->LiteralKind == EDstLiteralKind::Number && NumberLiteral->Text == TEXT("2.50") && FMath::IsNearlyEqual(NumberLiteral->Number, 2.5));
			TestTrue(TEXT("\"hi\" is a String literal with its value"),
				StringLiteral && StringLiteral->LiteralKind == EDstLiteralKind::String && StringLiteral->Text == TEXT("hi"));
			TestTrue(TEXT("names are case-sensitive: True is a name"), Capital && Capital->Is<FDstNameExpr>());
		}

		{
			// The span of the whole expression covers its text, and a call knows where its arguments are.
			FDstDiagnosticSink Sink;
			const FString Text = TEXT("Delay(1, 0.3) [ Name = \"d\" ]");
			const FDstSourceText Source(TEXT("expr"), Text);
			const FDstExprPtr Expr = ParseDstExpression(Source, Sink);
			TestTrue(TEXT("the call parses"), Expr.IsValid() && !Sink.HasErrors());
			if (Expr)
			{
				TestTrue(TEXT("the span covers the text"), Expr->Span.Offset == 0 && Expr->Span.Length == Text.Len());
				const FDstCallExpr* Call = Expr->As<FDstCallExpr>();
				TestNotNull(TEXT("it is a call"), Call);
				if (Call)
				{
					TestEqualSensitive(TEXT("the arguments span runs from ( to )"), Source.Slice(Call->ArgsSpan), FString(TEXT("(1, 0.3)")));
					TestEqual(TEXT("two arguments"), Call->Args.Num(), 2);
					TestNotNull(TEXT("and settings"), Call->Meta.Get());
					if (Call->Meta)
					{
						TestEqual(TEXT("one setting"), Call->Meta->Entries.Num(), 1);
					}
				}
			}
		}

		{
			// An output argument is marked as such and has a name.
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(TEXT("Run(Dist -> Params.D, Speed = 3, 4.5)"), Sink);
			TestEqual(TEXT("a positional argument after a named one: DST2021"), CountDiagnostics(Sink, TEXT("DST2021")), 1);
			TestNull(TEXT("an expression that reported an error is not handed back"), Expr.Get());
		}
		{
			const FString CallText = TEXT("Run(4.5, Dist -> Params.D, Speed = 3)");
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(CallText, Sink);
			TestEqual(TEXT("positional first is fine"), Sink.Num(), 0);
			const FDstCallExpr* Call = Expr ? Expr->As<FDstCallExpr>() : nullptr;
			TestNotNull(TEXT("it is a call"), Call);
			if (Call && Call->Args.Num() == 3)
			{
				TestFalse(TEXT("the first argument is positional"), Call->Args[0]->IsNamed());
				TestTrue(TEXT("Dist is an output argument"), Call->Args[1]->bOutput && Call->Args[1]->IsNamed() && Call->Args[1]->Name == TEXT("Dist"));
				TestFalse(TEXT("Speed is not"), Call->Args[2]->bOutput);
				TestEqualSensitive(TEXT("an argument's name span"), FDstSourceText(TEXT("expr"), CallText).Slice(Call->Args[1]->NameSpan), FString(TEXT("Dist")));
			}
		}
		{
			// A name given twice: DST2023, at the second.
			FDstDiagnosticSink Sink;
			ParseExpr(TEXT("Run(Speed = 1, Speed = 2)"), Sink);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Sink, TEXT("DST2023"));
			TestNotNull(TEXT("a repeated argument name: DST2023"), Diagnostic);
			if (Diagnostic)
			{
				TestTrue(TEXT("on the second one"), Diagnostic->Span.Line == 1 && Diagnostic->Span.Column == 16);
			}
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangParseExpressionErrorsTest, "DreamStateTree.Lang.Unit.ParseExpressionErrors",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangParseExpressionErrorsTest::RunTest(const FString& Parameters)
	{
		const TCHAR* Bad[] = { TEXT("a b"), TEXT(""), TEXT("   "), TEXT("1 +"), TEXT("(1"), TEXT("F(1,"), TEXT("[1, 2"), TEXT("a b c"), TEXT(") 1") };
		for (const TCHAR* Case : Bad)
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(Case, Sink);
			TestTrue(FString::Printf(TEXT("`%s` is not one expression: an error is reported"), Case), Sink.HasErrors());
			TestNull(FString::Printf(TEXT("`%s` gives no expression"), Case), Expr.Get());
		}

		{
			// Where "the end of the expression" was expected: the second word.
			FDstDiagnosticSink Sink;
			ParseExpr(TEXT("a b"), Sink);
			const FDstDiagnostic* Diagnostic = FindDiagnostic(Sink, TEXT("DST2001"));
			TestNotNull(TEXT("`a b` is DST2001"), Diagnostic);
			if (Diagnostic)
			{
				TestTrue(TEXT("at the b"), Diagnostic->Span.Line == 1 && Diagnostic->Span.Column == 3);
			}
		}
		{
			// Surrounding whitespace and comments are not part of the expression.
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(TEXT("\n  /* lead */ a // tail\n"), Sink);
			TestTrue(TEXT("whitespace and comments around one expression are fine"), Expr.IsValid() && !Sink.HasErrors());
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangWordOperatorsTest, "DreamStateTree.Lang.Unit.WordOperators",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangWordOperatorsTest::RunTest(const FString& Parameters)
	{
		// `&&`, `||` and `!` are errors (DST2017), but they parse as and, or and not so the rest of the file is still checked.
		const FString Text = TEXT(R"DST(StateTree(Name="T/Words", Schema=Villager)
{
    State A {
        if a && b || !c
    }
}
)DST");
		FParsed Parsed;
		ParseText(Text, Parsed);
		TestEqual(TEXT("three DST2017"), CountDiagnostics(Parsed.Sink, TEXT("DST2017")), 3);
		TestEqual(TEXT("and nothing else"), Parsed.Sink.Num(), 3);
		TestTrue(TEXT("it is an error"), Parsed.Sink.HasErrors());

		TArray<FDstSpan> Spans;
		for (const FDstDiagnostic& Diagnostic : Parsed.Sink.GetDiagnostics())
		{
			if (Diagnostic.Code == TEXT("DST2017"))
			{
				Spans.Add(Diagnostic.Span);
			}
		}
		if (Spans.Num() == 3)
		{
			TestEqualSensitive(TEXT("&& is reported on the operator"), Parsed.Source.Slice(Spans[0]), FString(TEXT("&&")));
			TestEqualSensitive(TEXT("|| is reported on the operator"), Parsed.Source.Slice(Spans[1]), FString(TEXT("||")));
			TestEqualSensitive(TEXT("! is reported on the operator"), Parsed.Source.Slice(Spans[2]), FString(TEXT("!")));
			TestTrue(TEXT("&& is at (4,14)"), Spans[0].Line == 4 && Spans[0].Column == 14);
			TestTrue(TEXT("|| is at (4,19)"), Spans[1].Line == 4 && Spans[1].Column == 19);
			TestTrue(TEXT("! is at (4,22)"), Spans[2].Line == 4 && Spans[2].Column == 22);
		}

		const FDstStateStmt* A = Parsed.File && Parsed.File->Document ? FindStateIn(Parsed.File->Document->Members, TEXT("A")) : nullptr;
		const FDstClauseStmt* If = A ? ClauseAt(*A, 0) : nullptr;
		TestNotNull(TEXT("the clause still parses"), If);
		if (If)
		{
			TestEqualSensitive(TEXT("as and, or and not"), ShapeOrNone(If->Value), FString(TEXT("((a and b) or (not c))")));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Trivia
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTriviaTest, "DreamStateTree.Lang.Unit.Trivia",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTriviaTest::RunTest(const FString& Parameters)
	{
		const FString Text = TEXT(R"DST(// header comment
StateTree(Name="T/Trivia", Schema=Villager)
{
    // above A
    State A { }   // after A

    State B { }
    // at the end
}
)DST");
		{
			FParsed Parsed;
			ParseText(Text, Parsed);
			TestEqual(TEXT("no diagnostics"), Parsed.Sink.Num(), 0);
			if (!Parsed.File || !Parsed.File->Document || Parsed.File->Document->Members.Num() != 2)
			{
				AddError(TEXT("The file should have a document with two states."));
				return false;
			}
			const FDstDocument& Document = *Parsed.File->Document;
			const FDstTrivia* DocumentTrivia = Parsed.File->FindTrivia(&Document);
			TestNotNull(TEXT("the document has trivia"), DocumentTrivia);
			if (DocumentTrivia)
			{
				TestEqual(TEXT("one comment above the document"), DocumentTrivia->Leading.Num(), 1);
				if (DocumentTrivia->Leading.Num() == 1)
				{
					TestEqualSensitive(TEXT("its text, delimiters included"), DocumentTrivia->Leading[0].Text, FString(TEXT("// header comment")));
					TestFalse(TEXT("it is not a block comment"), DocumentTrivia->Leading[0].bBlock);
				}
			}

			const FDstTrivia* ATrivia = Parsed.File->FindTrivia(Document.Members[0].Get());
			TestNotNull(TEXT("A has trivia"), ATrivia);
			if (ATrivia)
			{
				TestEqual(TEXT("one comment above A"), ATrivia->Leading.Num(), 1);
				if (ATrivia->Leading.Num() == 1)
				{
					TestEqualSensitive(TEXT("its text"), ATrivia->Leading[0].Text, FString(TEXT("// above A")));
				}
				TestEqual(TEXT("A follows the opening brace directly"), ATrivia->BlankLinesBefore, 0);
				TestTrue(TEXT("A has a trailing comment"), ATrivia->Trailing.IsSet());
				if (ATrivia->Trailing.IsSet())
				{
					TestEqualSensitive(TEXT("its text"), ATrivia->Trailing.GetValue().Text, FString(TEXT("// after A")));
				}
			}

			const FDstTrivia* BTrivia = Parsed.File->FindTrivia(Document.Members[1].Get());
			TestNotNull(TEXT("B has trivia (a blank line above it)"), BTrivia);
			if (BTrivia)
			{
				TestEqual(TEXT("one blank line above B"), BTrivia->BlankLinesBefore, 1);
				TestEqual(TEXT("no comment above B"), BTrivia->Leading.Num(), 0);
			}

			bool bTailComment = false;
			if (DocumentTrivia)
			{
				for (const FDstComment& Comment : DocumentTrivia->Inner)
				{
					bTailComment |= Comment.Text == TEXT("// at the end");
				}
			}
			TestTrue(TEXT("the comment after the last state is the document's inner comment"), bTailComment);
		}
		{
			FDstParseOptions Options;
			Options.bKeepTrivia = false;
			FParsed Parsed;
			ParseText(Text, Parsed, Options);
			TestEqual(TEXT("without trivia: no diagnostics"), Parsed.Sink.Num(), 0);
			TestTrue(TEXT("without trivia: the side table is empty"), Parsed.File && Parsed.File->Trivia.IsEmpty());
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Paths, integers, clones, tables
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangPathsTest, "DreamStateTree.Lang.Unit.Paths",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangPathsTest::RunTest(const FString& Parameters)
	{
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(TEXT("Info.Target.Location"), Sink);
			TArray<FString> Segments;
			TestTrue(TEXT("a member chain is a path"), Expr && TryGetDstPath(*Expr, Segments));
			TestEqualSensitive(TEXT("segments"), FString::Join(Segments, TEXT("|")), FString(TEXT("Info|Target|Location")));
			TestEqualSensitive(TEXT("as one dotted string"), Expr ? GetDstPathString(*Expr) : FString(), FString(TEXT("Info.Target.Location")));
		}
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(TEXT("Params"), Sink);
			TArray<FString> Segments;
			TestTrue(TEXT("a single name is a path of one"), Expr && TryGetDstPath(*Expr, Segments) && Segments.Num() == 1 && Segments[0] == TEXT("Params"));
		}
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(TEXT("Game.Event.Villager.Hit"), Sink);
			TestEqualSensitive(TEXT("a tag is a path"), Expr ? GetDstPathString(*Expr) : FString(), FString(TEXT("Game.Event.Villager.Hit")));
		}
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(TEXT("`Idle`.`Phone Call`.Ring"), Sink);
			TArray<FString> Segments;
			TestTrue(TEXT("quoted names are segments"), Expr && TryGetDstPath(*Expr, Segments));
			TestEqualSensitive(TEXT("with their text"), FString::Join(Segments, TEXT("|")), FString(TEXT("Idle|Phone Call|Ring")));
		}
		const TCHAR* NotPaths[] = { TEXT("Self.GetParent()"), TEXT("1"), TEXT("\"a.b\""), TEXT("a + b"), TEXT("(a.b)"), TEXT("[a]"), TEXT("ref a.b") };
		for (const TCHAR* Case : NotPaths)
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(Case, Sink);
			TArray<FString> Segments;
			TestFalse(FString::Printf(TEXT("`%s` is not a path"), Case), Expr && TryGetDstPath(*Expr, Segments));
			TestEqualSensitive(FString::Printf(TEXT("`%s` has an empty path string"), Case), Expr ? GetDstPathString(*Expr) : FString(), FString());
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangIntegersTest, "DreamStateTree.Lang.Unit.Integers",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangIntegersTest::RunTest(const FString& Parameters)
	{
		struct FWhole
		{
			const TCHAR* Text;
			int64 Value;
		};
		const FWhole Whole[] =
		{
			{ TEXT("0"), 0 }, { TEXT("5"), 5 }, { TEXT("-5"), -5 }, { TEXT("(3)"), 3 }, { TEXT("1e3"), 1000 }, { TEXT("2.0"), 2 },
			// Above 2^53 a double would round these; the digits must be read as digits.
			{ TEXT("9007199254740993"), 9007199254740993LL }, { TEXT("-9007199254740993"), -9007199254740993LL },
			{ TEXT("9223372036854775807"), 9223372036854775807LL },
			{ TEXT("-9223372036854775808"), MIN_int64 },
		};
		for (const FWhole& Case : Whole)
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(Case.Text, Sink);
			int64 Value = 0;
			const bool bWhole = Expr && TryGetDstInteger(*Expr, Value);
			TestTrue(FString::Printf(TEXT("`%s` is a whole number"), Case.Text), bWhole);
			if (bWhole)
			{
				TestTrue(FString::Printf(TEXT("`%s` is %lld, got %lld"), Case.Text, Case.Value, Value), Value == Case.Value);
			}
		}

		const TCHAR* NotWhole[] = { TEXT("2.5"), TEXT("0.1"), TEXT("9223372036854775808"), TEXT("abc"), TEXT("\"3\""), TEXT("a + 1"), TEXT("true") };
		for (const TCHAR* Case : NotWhole)
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(Case, Sink);
			int64 Value = 0;
			TestFalse(FString::Printf(TEXT("`%s` is not a whole number"), Case), Expr && TryGetDstInteger(*Expr, Value));
		}

		// MakeDstInteger keeps every digit, and TryGetDstInteger reads it back.
		const int64 Made[] = { 0, 1, -1, 5, -5, 255, 9007199254740993LL, -9007199254740993LL, 9223372036854775807LL, MIN_int64 };
		for (const int64 Value : Made)
		{
			const FDstExprPtr Expr = MakeDstInteger(Value);
			int64 Back = 0;
			TestTrue(FString::Printf(TEXT("%lld reads back"), Value), Expr && TryGetDstInteger(*Expr, Back) && Back == Value);
		}
		{
			const FDstExprPtr Five = MakeDstInteger(5);
			const FDstExprPtr MinusFive = MakeDstInteger(-5);
			TestEqualSensitive(TEXT("5 prints as 5"), Five ? PrintDstExpr(*Five) : FString(), FString(TEXT("5")));
			TestEqualSensitive(TEXT("-5 prints as -5"), MinusFive ? PrintDstExpr(*MinusFive) : FString(), FString(TEXT("-5")));
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangCloneTest, "DreamStateTree.Lang.Unit.Clone",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangCloneTest::RunTest(const FString& Parameters)
	{
		const FString Text = TEXT(R"DST(#include "Common.dth"
StateTree(Name="T/Clone", Schema=AIComponent(Actor = CombatEnemy))
{
    Parameters = { float Range = 200 [ Category = "Combat" ]; Array<float> Weights = [1, 2.5]; }
    Globals = { Info = GetPlayerInfo(MaxRange = 2500); }
    Colors = { Combat = (0.8, 0.25, 0.2); }
    Tags = { Game.Event.Villager; }
    Template Near(Who, Limit = 100) = Who.Distance < Limit;
    State Root [ Tag = Combat ] {
        on Hit.Stagger
        if ObjectIsValid(Info.Target) and not Info.Lost
        pick Random
        utility FloatInput(Info.Distance, (0, 3000)) * 0.5
        do Combo = Attack(1, Hit -> Params.Last) [ Name = "c" ], Wait, Aim(ref Params.Spot)
        -> Next on Delegate(Combo.Succeeds) if not Tired [ Delay = 0.5; Reactivate ]
        State Child { link asset ST_Other(Speed = 2) }
        #Region "r"
        State Sub { link HitReaction(Toughness = 1 + Params.Courage) }
        #EndRegion
    }
    Subtree HitReaction(float Toughness = 1) { State Stagger { do Stagger(0.8 / Params.Toughness) } }
}
)DST");
		FParsed Parsed;
		ParseText(Text, Parsed);
		TestEqual(TEXT("clone source: no diagnostics"), Parsed.Sink.Num(), 0);
		if (!Parsed.File || !Parsed.File->Document)
		{
			AddError(TEXT("The clone source should have a document."));
			return false;
		}
		const FDstFile& File = *Parsed.File;

		// Clone every member and header argument into a second file and compare the dumps. Trivia is not cloned, and the
		// dump does not show it.
		FDstFile Copy;
		Copy.Path = File.Path;
		for (const TUniquePtr<FDstInclude>& Include : File.Includes)
		{
			TUniquePtr<FDstInclude> IncludeCopy = MakeUnique<FDstInclude>();
			IncludeCopy->Path = Include->Path;
			IncludeCopy->PathSpan = Include->PathSpan;
			IncludeCopy->Span = Include->Span;
			Copy.Includes.Add(MoveTemp(IncludeCopy));
		}
		Copy.Document = MakeUnique<FDstDocument>();
		Copy.Document->DocumentKind = File.Document->DocumentKind;
		Copy.Document->KindName = File.Document->KindName;
		for (const FDstArgumentPtr& Arg : File.Document->HeaderArgs)
		{
			Copy.Document->HeaderArgs.Add(CloneArgument(*Arg));
		}
		for (const FDstStmtPtr& Stmt : File.Document->Members)
		{
			Copy.Document->Members.Add(CloneStmt(*Stmt));
		}
		TestEqualSensitive(TEXT("a deep clone dumps the same"), DumpDstTree(Copy), DumpDstTree(File));

		// Expressions, metadata, parameters and task items clone on their own too.
		FDstDiagnosticSink Sink;
		const FDstExprPtr Expr = ParseExpr(TEXT("Run(A = [1, (2, 3)], B -> Params.B, C = ref Params.C) [ Name = \"n\"; Flag ]"), Sink);
		if (Expr)
		{
			const FDstExprPtr ExprCopy = CloneExpr(*Expr);
			TestTrue(TEXT("an expression clone is not the same node"), ExprCopy.Get() != Expr.Get());
			TestEqualSensitive(TEXT("an expression clone has the same shape"), ShapeOf(*ExprCopy), ShapeOf(*Expr));
			const FDstCallExpr* Call = Expr->As<FDstCallExpr>();
			if (Call && Call->Meta)
			{
				const TUniquePtr<FDstMeta> MetaCopy = CloneMeta(*Call->Meta);
				TestEqualSensitive(TEXT("a meta clone has the same entries"), ShapeOfMeta(MetaCopy.Get()), ShapeOfMeta(Call->Meta.Get()));
			}
		}
		else
		{
			AddError(TEXT("The clone expression should parse."));
		}

		// A clone does not share nodes with the original: changing one leaves the other alone.
		const FDstStateStmt* Root = FindStateIn(File.Document->Members, TEXT("Root"));
		if (Root)
		{
			FDstStmtPtr RootCopy = CloneStmt(*Root);
			FDstStateStmt* RootCopyState = RootCopy->As<FDstStateStmt>();
			TestNotNull(TEXT("a state clones as a state"), RootCopyState);
			if (RootCopyState)
			{
				RootCopyState->Name = TEXT("Renamed");
				RootCopyState->Body.Reset();
				TestEqualSensitive(TEXT("the original keeps its name"), Root->Name, FString(TEXT("Root")));
				TestTrue(TEXT("and its body"), Root->Body.Num() > 0);
			}
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangOperatorTablesTest, "DreamStateTree.Lang.Unit.OperatorTables",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangOperatorTablesTest::RunTest(const FString& Parameters)
	{
		struct FBinary
		{
			EDstBinaryOp Op;
			const TCHAR* Spelling;
			int32 Precedence;
		};
		// "Higher binds tighter: or 1, and 2, equality 3, comparison 4, additive 5, multiplicative 6."
		const FBinary Binaries[] =
		{
			{ EDstBinaryOp::Or, TEXT("or"), 1 }, { EDstBinaryOp::And, TEXT("and"), 2 },
			{ EDstBinaryOp::Equal, TEXT("=="), 3 }, { EDstBinaryOp::NotEqual, TEXT("!="), 3 },
			{ EDstBinaryOp::Less, TEXT("<"), 4 }, { EDstBinaryOp::LessEqual, TEXT("<="), 4 },
			{ EDstBinaryOp::Greater, TEXT(">"), 4 }, { EDstBinaryOp::GreaterEqual, TEXT(">="), 4 },
			{ EDstBinaryOp::Add, TEXT("+"), 5 }, { EDstBinaryOp::Subtract, TEXT("-"), 5 },
			{ EDstBinaryOp::Multiply, TEXT("*"), 6 }, { EDstBinaryOp::Divide, TEXT("/"), 6 },
		};
		for (const FBinary& Case : Binaries)
		{
			TestEqualSensitive(FString::Printf(TEXT("spelling of %s"), Case.Spelling), FString(GetDstBinaryOpSpelling(Case.Op)), FString(Case.Spelling));
			TestEqual(FString::Printf(TEXT("precedence of %s"), Case.Spelling), GetDstBinaryOpPrecedence(Case.Op), Case.Precedence);
		}
		TestEqualSensitive(TEXT("not"), FString(GetDstUnaryOpSpelling(EDstUnaryOp::Not)), FString(TEXT("not")));
		TestEqualSensitive(TEXT("negate"), FString(GetDstUnaryOpSpelling(EDstUnaryOp::Negate)), FString(TEXT("-")));

		const TCHAR* ClauseWords[] = { TEXT("on"), TEXT("if"), TEXT("pick"), TEXT("utility"), TEXT("link"), TEXT("do") };
		for (const TCHAR* Word : ClauseWords)
		{
			TestTrue(FString::Printf(TEXT("`%s` starts a clause"), Word), IsDstClauseWord(Word));
		}
		const TCHAR* NotClauseWords[] = { TEXT("State"), TEXT("Subtree"), TEXT("Template"), TEXT("and"), TEXT("asset"), TEXT("Wait"), TEXT("") };
		for (const TCHAR* Word : NotClauseWords)
		{
			TestFalse(FString::Printf(TEXT("`%s` does not start a clause"), Word), IsDstClauseWord(Word));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// The printer's helpers
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangQuoteNameTest, "DreamStateTree.Lang.Unit.QuoteName",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangQuoteNameTest::RunTest(const FString& Parameters)
	{
		const FShapeCase Cases[] =
		{
			{ TEXT("Move"), TEXT("Move") },
			{ TEXT("Move To"), TEXT("`Move To`") },
			{ TEXT("中文"), TEXT("中文") },
			{ TEXT("a-b"), TEXT("`a-b`") },
			{ TEXT("1st"), TEXT("`1st`") },
			{ TEXT("State"), TEXT("State") },
			{ TEXT(""), TEXT("") },
		};
		for (const FShapeCase& Case : Cases)
		{
			TestEqualSensitive(FString::Printf(TEXT("QuoteDstName(`%s`)"), Case.Source), QuoteDstName(Case.Source), FString(Case.Shape));
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangPrintExpressionTest, "DreamStateTree.Lang.Unit.PrintExpression",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangPrintExpressionTest::RunTest(const FString& Parameters)
	{
		// Canonical spacing: operators and `=` have spaces around them, commas are followed by one, brackets hug.
		const FShapeCase Cases[] =
		{
			{ TEXT("a+b"), TEXT("a + b") },
			{ TEXT("a  and   b or c"), TEXT("a and b or c") },
			{ TEXT("(a or b) and c"), TEXT("(a or b) and c") },
			{ TEXT("not   x"), TEXT("not x") },
			{ TEXT("-x"), TEXT("-x") },
			{ TEXT("1 - -2"), TEXT("1 - -2") },
			{ TEXT("F( 1 ,2 )"), TEXT("F(1, 2)") },
			{ TEXT("F()"), TEXT("F()") },
			{ TEXT("F(A=1,B =2)"), TEXT("F(A = 1, B = 2)") },
			{ TEXT("[1,2,3]"), TEXT("[1, 2, 3]") },
			{ TEXT("[]"), TEXT("[]") },
			{ TEXT("(0,3000)"), TEXT("(0, 3000)") },
			{ TEXT("(X=1,Y=2)"), TEXT("(X = 1, Y = 2)") },
			{ TEXT("(a)"), TEXT("(a)") },
			{ TEXT("ref Params.Spot"), TEXT("ref Params.Spot") },
			{ TEXT("Foo(Dist->Params.D)"), TEXT("Foo(Dist -> Params.D)") },
			{ TEXT("a<b"), TEXT("a < b") },
			{ TEXT("a<=b"), TEXT("a <= b") },
			{ TEXT("a==b"), TEXT("a == b") },
			{ TEXT("a!=b"), TEXT("a != b") },
			{ TEXT("a.b.c"), TEXT("a.b.c") },
			{ TEXT("`Move To`.x"), TEXT("`Move To`.x") },
			{ TEXT(".5+1e3"), TEXT(".5 + 1e3") },
			{ TEXT("2.50"), TEXT("2.50") },
			{ TEXT("true"), TEXT("true") },
			{ TEXT("\"a\\nb\""), TEXT("\"a\\nb\"") },
			{ TEXT("Walk(1)[Name=\"x\";Enabled=false]"), TEXT("Walk(1) [ Name = \"x\"; Enabled = false ]") },
			{ TEXT("Walk(1) [ Group ]"), TEXT("Walk(1) [ Group ]") },
		};
		for (const FShapeCase& Case : Cases)
		{
			FDstDiagnosticSink Sink;
			const FDstExprPtr Expr = ParseExpr(Case.Source, Sink);
			if (!Expr || Sink.HasErrors())
			{
				AddError(FString::Printf(TEXT("`%s` should parse: %s"), Case.Source, *DescribeDiagnostics(Sink)));
				continue;
			}
			const FString Printed = PrintDstExpr(*Expr);
			TestEqualSensitive(FString::Printf(TEXT("`%s` prints canonically"), Case.Source), Printed, FString(Case.Shape));

			// Printing keeps the tree, and printing the print changes nothing.
			FDstDiagnosticSink AgainSink;
			const FDstExprPtr Again = ParseExpr(Printed, AgainSink);
			if (!Again || AgainSink.HasErrors())
			{
				AddError(FString::Printf(TEXT("The print of `%s` does not parse: %s"), Case.Source, *DescribeDiagnostics(AgainSink)));
				continue;
			}
			TestEqualSensitive(FString::Printf(TEXT("printing `%s` keeps the shape"), Case.Source), ShapeOf(*Again), ShapeOf(*Expr));
			TestEqualSensitive(FString::Printf(TEXT("printing `%s` twice changes nothing"), Case.Source), PrintDstExpr(*Again), Printed);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangDumpTreeFormatTest, "DreamStateTree.Lang.Unit.DumpTreeFormat",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangDumpTreeFormatTest::RunTest(const FString& Parameters)
	{
		// The example in DstPrinter.h, as a file.
		{
			const FString Text = TEXT(R"DST(#include "Common.dth"
StateTree(Name="AI/ST_Brawler", Schema=AIComponent(Actor = CombatEnemy))
{
    Parameters = { float AttackRange = 200; }
    State Root {
        -> Root
        State Attack [ Tag = Combat ] {
            do Combo = ComboAttack()
        }
    }
    Subtree HitReaction(float Toughness = 1) { }
}
)DST");
			const FString Expected = TEXT(R"DST(Include "Common.dth"
Document StateTree
  Arg Name
    String "AI/ST_Brawler"
  Arg Schema
    Call
      Name AIComponent
      Arg Actor
        Name CombatEnemy
  Section Parameters
    Decl float AttackRange
      Number 200
  State Root
    Clause Transition
      Name Root
    State Attack
      Meta
        MetaEntry Tag
          Name Combat
      Clause Do
        Task Combo
          Call
            Name ComboAttack
  Subtree HitReaction
    Param float Toughness
      Number 1
)DST");
			FParsed Parsed;
			ParseText(Text, Parsed);
			TestEqual(TEXT("dump example: no diagnostics"), Parsed.Sink.Num(), 0);
			if (Parsed.File)
			{
				const FString Dump = DumpDstTree(*Parsed.File);
				const FString Difference = DescribeDifference(Expected.TrimEnd(), Dump.TrimEnd());
				TestTrue(FString::Printf(TEXT("the dump matches the example in DstPrinter.h (%s)"), *Difference), Difference.IsEmpty());
			}
		}

		// The other leaf spellings, by line: indentation and order are the golden files' business.
		{
			const FString Text = TEXT(R"DST(StateTree(Name="T/Spellings", Schema=Villager)
{
    Tags = { Game.Event.Hit; }
    Template Near(Who, Limit = 5) = Who.Distance < Limit;
    #Region "r"
    State A {
        if not X and Y
        -> `Move To` on Hit if Z [ Delay = 1 ]
        link asset ST_Other(P -> Q)
        do Wait(1, [2, 3], (4, 5), ref R, true, "s") [ Name = "w" ]
    }
    #EndRegion
}
)DST");
			FParsed Parsed;
			ParseText(Text, Parsed);
			TestEqual(TEXT("spellings: no diagnostics"), Parsed.Sink.Num(), 0);
			if (Parsed.File)
			{
				TArray<FString> Lines;
				DumpDstTree(*Parsed.File).ParseIntoArrayLines(Lines);
				for (FString& Line : Lines)
				{
					Line.TrimStartAndEndInline();
				}
				const TCHAR* Wanted[] =
				{
					TEXT("Document StateTree"), TEXT("Section Tags"), TEXT("Expr"), TEXT("Member Hit"), TEXT("Template Near"),
					TEXT("Param Who"), TEXT("Param Limit"), TEXT("Binary <"), TEXT("Region \"r\""), TEXT("EndRegion"), TEXT("State A"),
					TEXT("Clause If"), TEXT("Binary and"), TEXT("Unary not"), TEXT("Clause Transition"), TEXT("Name `Move To`"),
					TEXT("On"), TEXT("If"), TEXT("Meta"), TEXT("MetaEntry Delay"), TEXT("Clause Link asset"), TEXT("Arg P ->"),
					TEXT("Clause Do"), TEXT("Task"), TEXT("List"), TEXT("Tuple"), TEXT("Ref"), TEXT("Bool true"), TEXT("String \"s\""),
					TEXT("MetaEntry Name"), TEXT("Number 5"),
				};
				for (const TCHAR* Line : Wanted)
				{
					TestTrue(FString::Printf(TEXT("the dump has a line `%s`"), Line), Lines.Contains(Line));
				}
			}
		}
		return true;
	}
}
