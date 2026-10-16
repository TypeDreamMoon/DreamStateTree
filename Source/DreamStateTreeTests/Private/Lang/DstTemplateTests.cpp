// Copyright DreamMoon. All Rights Reserved.
//
// #include resolution and template expansion, run in memory: a fake IDstIncludeLoader serves library texts from a map,
// so no file system is involved. Covers the normal expansion (arguments, defaults, paths, nesting, templates without
// parentheses, where the expansion lands, origins and spans) and every code of DST3101-3106 and DST3201-3203.

#include "DstLangTestSupport.h"

#include "Lang/DstAst.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstParser.h"
#include "Lang/DstPrinter.h"
#include "Lang/DstSource.h"
#include "Lang/DstTemplates.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace UE::DreamStateTree::Tests::TemplateTests
{
	using namespace UE::DreamStateTree::Lang;
	using namespace UE::DreamStateTree::Tests::LangSupport;

	namespace
	{
		const TCHAR* const MainPath = TEXT("Mem/Main.dtr");

		/**
		 * Library texts by path, no file system. An include path is resolved against the including file's folder and
		 * normalised, so the same file always resolves to the same string (the loader contract).
		 */
		class FFakeIncludeLoader final : public IDstIncludeLoader
		{
		public:
			TMap<FString, FString> Files;

			void Add(const TCHAR* Path, const FString& Text)
			{
				Files.Add(Path, Text);
			}

			virtual bool Load(const FString& FromFile, const FString& IncludePath, FString& OutResolvedPath, FString& OutText) override
			{
				FString Candidate = FPaths::Combine(FPaths::GetPath(FromFile), IncludePath);
				FPaths::NormalizeFilename(Candidate);
				FPaths::CollapseRelativeDirectories(Candidate);
				if (const FString* Text = Files.Find(Candidate))
				{
					OutResolvedPath = Candidate;
					OutText = *Text;
					return true;
				}
				return false;
			}
		};

		/** A parsed unit after the include and template steps. */
		struct FRun
		{
			FDstCompilationUnit Unit;
			FDstDiagnosticSink Sink;
			bool bIncludesOk = false;
			bool bTemplatesOk = false;

			const FDstFile& Main() const { return *Unit.Main; }
		};

		void RunText(const FString& MainText, FFakeIncludeLoader& Loader, FRun& Out)
		{
			Out.Sink = FDstDiagnosticSink(MainPath);
			Out.Unit.MainSource = FDstSourceText(MainPath, MainText);
			Out.Unit.Main = ParseDst(Out.Unit.MainSource, Out.Sink);
			Out.bIncludesOk = ResolveDstIncludes(Out.Unit, Loader, Out.Sink);
			Out.bTemplatesOk = ExpandDstTemplates(Out.Unit, Out.Sink);
		}

		/** Members go below the header; the first of them is on line 3. */
		FString MainWith(const FString& Members)
		{
			return TEXT("StateTree(Name=\"T/T\", Schema=Villager)\n{\n") + Members + TEXT("}\n");
		}

		FString LibraryWith(const TCHAR* Name, const FString& Members)
		{
			return FString::Printf(TEXT("StateTreeLibrary(Name=\"%s\")\n{\n"), Name) + Members + TEXT("}\n");
		}

		const FDstStateStmt* FindTopState(const FDstFile& File, const TCHAR* Name)
		{
			if (!File.Document)
			{
				return nullptr;
			}
			for (const FDstStmtPtr& Member : File.Document->Members)
			{
				const FDstStateStmt* State = Member->As<FDstStateStmt>();
				if (State && State->Name == Name)
				{
					return State;
				}
			}
			return nullptr;
		}

		const FDstTemplateStmt* FindTemplate(const FDstFile& File, const TCHAR* Name)
		{
			if (!File.Document)
			{
				return nullptr;
			}
			for (const FDstStmtPtr& Member : File.Document->Members)
			{
				const FDstTemplateStmt* Template = Member->As<FDstTemplateStmt>();
				if (Template && Template->Name == Name)
				{
					return Template;
				}
			}
			return nullptr;
		}

		const FDstClauseStmt* ClauseOf(const FDstFile& File, const TCHAR* StateName, const int32 Index)
		{
			const FDstStateStmt* State = FindTopState(File, StateName);
			return State && State->Body.IsValidIndex(Index) ? State->Body[Index]->As<FDstClauseStmt>() : nullptr;
		}

		/** The shape of the clause's value: the condition of an `if`, the target of a `->`. */
		FString ValueShape(const FDstFile& File, const TCHAR* StateName, const int32 Index)
		{
			const FDstClauseStmt* Clause = ClauseOf(File, StateName, Index);
			return Clause && Clause->Value ? ShapeOf(*Clause->Value) : FString(TEXT("<missing>"));
		}

		/** The shape of one task's node of a `do` clause. */
		FString TaskShape(const FDstFile& File, const TCHAR* StateName, const int32 Index, const int32 Task)
		{
			const FDstClauseStmt* Clause = ClauseOf(File, StateName, Index);
			if (!Clause || !Clause->Tasks.IsValidIndex(Task) || !Clause->Tasks[Task]->Node)
			{
				return TEXT("<missing>");
			}
			return ShapeOf(*Clause->Tasks[Task]->Node);
		}

		FString TemplateBodyShape(const FDstFile& File, const TCHAR* Name)
		{
			const FDstTemplateStmt* Template = FindTemplate(File, Name);
			return Template && Template->Body ? ShapeOf(*Template->Body) : FString(TEXT("<missing>"));
		}

		/** NameExpr uses of Name below Node, template definitions left out. */
		void CountNameUses(const FDstNode& Node, const FString& Name, int32& InOutCount)
		{
			if (Node.Is<FDstTemplateStmt>())
			{
				return;
			}
			const FDstNameExpr* Use = Node.As<FDstNameExpr>();
			if (Use && !Use->bQuoted && Use->Name == Name)
			{
				++InOutCount;
			}
			ForEachChild(Node, [&Name, &InOutCount](const FDstNode& Child) { CountNameUses(Child, Name, InOutCount); });
		}

		/** Number literals with this lexeme anywhere below Node, template definitions included. */
		void CountNumbers(const FDstNode& Node, const FString& Lexeme, int32& InOutCount)
		{
			const FDstLiteralExpr* Literal = Node.As<FDstLiteralExpr>();
			if (Literal && Literal->LiteralKind == EDstLiteralKind::Number && Literal->Text == Lexeme)
			{
				++InOutCount;
			}
			ForEachChild(Node, [&Lexeme, &InOutCount](const FDstNode& Child) { CountNumbers(Child, Lexeme, InOutCount); });
		}

		/** A diagnostic with this code at this position, or an error naming what was there. Returns the diagnostic. */
		const FDstDiagnostic* ExpectDiagnostic(FAutomationTestBase& Test, const FDstDiagnosticSink& Sink, const TCHAR* Code, const int32 Line, const int32 Column)
		{
			const FDstDiagnostic* Found = FindDiagnostic(Sink, Code);
			if (!Found)
			{
				Test.AddError(FString::Printf(TEXT("Expected %s at (%d,%d); got: %s"), Code, Line, Column, *DescribeDiagnostics(Sink)));
				return nullptr;
			}
			if (Found->Span.Line != Line || Found->Span.Column != Column)
			{
				Test.AddError(FString::Printf(TEXT("Expected %s at (%d,%d), got %s"), Code, Line, Column, *FDstDiagnosticSink::ToWireString(*Found)));
			}
			return Found;
		}

		/** Template T1(X) = T2(X); ... Tn(X) = X; and a state using T1(5): n expansions deep. */
		FString ChainMembers(const int32 Length)
		{
			FString Members;
			for (int32 Index = 1; Index < Length; ++Index)
			{
				Members += FString::Printf(TEXT("    Template T%d(X) = T%d(X);\n"), Index, Index + 1);
			}
			Members += FString::Printf(TEXT("    Template T%d(X) = X;\n"), Length);
			Members += TEXT("    State Root {\n        utility T1(5)\n    }\n");
			return Members;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Expansion
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateExpandTest, "DreamStateTree.Lang.Templates.Expand",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateExpandTest::RunTest(const FString& Parameters)
	{
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Brisk = Walk(Factor = 1.5);
    Template Near(Who, Limit = 800) = Who.Distance < Limit;
    Template Both(A, B) = A and B;
    State Root {
        if Near(Player)
        do Brisk
        State Child {
            if Both(Near(Player, 100), Brisk)
            do Wait(1)
        }
    }
)DST")), Loader, Run);

		TestTrue(TEXT("includes resolve"), Run.bIncludesOk);
		TestTrue(TEXT("templates expand"), Run.bTemplatesOk);
		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main || !Run.Unit.Main->Document)
		{
			AddError(TEXT("The unit should have a main document."));
			return false;
		}
		const FDstFile& Main = Run.Main();

		TestEqualSensitive(TEXT("a default is used"), ValueShape(Main, TEXT("Root"), 0), FString(TEXT("(Player.Distance < 800)")));
		TestEqualSensitive(TEXT("a template without parentheses is replaced by its body"), TaskShape(Main, TEXT("Root"), 1, 0), FString(TEXT("Walk(Factor=1.5)")));

		const FDstStateStmt* Root = FindTopState(Main, TEXT("Root"));
		const FDstStateStmt* Child = nullptr;
		if (Root)
		{
			for (const FDstStmtPtr& Item : Root->Body)
			{
				if (const FDstStateStmt* State = Item->As<FDstStateStmt>())
				{
					Child = State;
				}
			}
		}
		const FDstClauseStmt* ChildIf = Child && Child->Body.Num() > 0 ? Child->Body[0]->As<FDstClauseStmt>() : nullptr;
		TestNotNull(TEXT("Child has an if"), ChildIf);
		if (ChildIf && ChildIf->Value)
		{
			TestEqualSensitive(TEXT("templates in arguments are expanded and then substituted"), ShapeOf(*ChildIf->Value),
				FString(TEXT("((Player.Distance < 100) and Walk(Factor=1.5))")));
		}

		// The definitions stay as written.
		TestEqualSensitive(TEXT("the definition of Near is left alone"), TemplateBodyShape(Main, TEXT("Near")), FString(TEXT("(Who.Distance < Limit)")));
		TestEqualSensitive(TEXT("the definition of Brisk is left alone"), TemplateBodyShape(Main, TEXT("Brisk")), FString(TEXT("Walk(Factor=1.5)")));
		TestEqualSensitive(TEXT("the definition of Both is left alone"), TemplateBodyShape(Main, TEXT("Both")), FString(TEXT("(A and B)")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateGroupingTest, "DreamStateTree.Lang.Templates.Grouping",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateGroupingTest::RunTest(const FString& Parameters)
	{
		// The expansion is a tree, not text: an argument stays whole inside the body, and the result stays whole inside its context.
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Sum(A, B) = A + B;
    Template Neg(X) = -X;
    State Root {
        utility Sum(1, 2) * 3
        if Neg(1 + 2) < 0
        if Sum(1, 2) * Sum(3, 4) > 0
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("the sum stays grouped under the product"), ValueShape(Main, TEXT("Root"), 0), FString(TEXT("((1 + 2) * 3)")));
		TestEqualSensitive(TEXT("a sum as an argument stays whole"), ValueShape(Main, TEXT("Root"), 1), FString(TEXT("((-(1 + 2)) < 0)")));
		TestEqualSensitive(TEXT("two expansions side by side"), ValueShape(Main, TEXT("Root"), 2), FString(TEXT("(((1 + 2) * (3 + 4)) > 0)")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplatePathPrefixTest, "DreamStateTree.Lang.Templates.PathPrefix",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplatePathPrefixTest::RunTest(const FString& Parameters)
	{
		// A parameter used as the start of a path is replaced by the argument's path.
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Dist(Who) = Who.Distance;
    Template Reach(Who) = Who.Pawn.Reach;
    State Root {
        if Dist(Player) < Dist(Info.Target)
        if Reach(Info.Target.Owner) > 0
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("a name argument"), ValueShape(Main, TEXT("Root"), 0), FString(TEXT("(Player.Distance < Info.Target.Distance)")));
		TestEqualSensitive(TEXT("a path argument under a longer body path"), ValueShape(Main, TEXT("Root"), 1), FString(TEXT("(Info.Target.Owner.Pawn.Reach > 0)")));

		const FDstClauseStmt* First = ClauseOf(Main, TEXT("Root"), 0);
		const FDstBinaryExpr* Less = First && First->Value ? First->Value->As<FDstBinaryExpr>() : nullptr;
		if (Less && Less->Lhs)
		{
			TArray<FString> Segments;
			TestTrue(TEXT("the result is still a plain path"), TryGetDstPath(*Less->Lhs, Segments));
			TestEqualSensitive(TEXT("with the segments of both"), FString::Join(Segments, TEXT("|")), FString(TEXT("Player|Distance")));
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateArgumentsTest, "DreamStateTree.Lang.Templates.Arguments",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateArgumentsTest::RunTest(const FString& Parameters)
	{
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Nice(Rate = 2, Cap = 10) = Rate * Cap;
    State Root {
        if Nice() > 0
        if Nice(3) > 0
        if Nice(Cap = 4) > 0
        if Nice(Cap = 4, Rate = 5) > 0
        if Nice(Rate = 7, Cap = Nice(Cap = 3)) > 0
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("both defaults"), ValueShape(Main, TEXT("Root"), 0), FString(TEXT("((2 * 10) > 0)")));
		TestEqualSensitive(TEXT("one positional argument"), ValueShape(Main, TEXT("Root"), 1), FString(TEXT("((3 * 10) > 0)")));
		TestEqualSensitive(TEXT("one named argument, the other default"), ValueShape(Main, TEXT("Root"), 2), FString(TEXT("((2 * 4) > 0)")));
		TestEqualSensitive(TEXT("named arguments in either order"), ValueShape(Main, TEXT("Root"), 3), FString(TEXT("((5 * 4) > 0)")));
		TestEqualSensitive(TEXT("a template call as an argument of the same template"), ValueShape(Main, TEXT("Root"), 4), FString(TEXT("((7 * (2 * 3)) > 0)")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateSubstitutionTest, "DreamStateTree.Lang.Templates.Substitution",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateSubstitutionTest::RunTest(const FString& Parameters)
	{
		// Only names in value position are replaced: an argument name is a key, not a use of the parameter.
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Mk(Value) = Walk(Value = Value);
    State Root {
        do Mk(5)
        do Mk(Params.Speed)
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("the argument name stays, its value is replaced"), TaskShape(Main, TEXT("Root"), 0, 0), FString(TEXT("Walk(Value=5)")));
		TestEqualSensitive(TEXT("with a path value"), TaskShape(Main, TEXT("Root"), 1, 0), FString(TEXT("Walk(Value=Params.Speed)")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateMetaTest, "DreamStateTree.Lang.Templates.MetaValues",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateMetaTest::RunTest(const FString& Parameters)
	{
		// A parameter used as the value of a node setting is replaced; the setting's key is not.
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Named(Label) = Walk(1) [ Name = Label ];
    State Root {
        do Named("fast")
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		TestEqualSensitive(TEXT("the setting's value is replaced, its key stays"), TaskShape(Run.Main(), TEXT("Root"), 0, 0), FString(TEXT("Walk(1)<Name=\"fast\">")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateNestedTest, "DreamStateTree.Lang.Templates.Nested",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateNestedTest::RunTest(const FString& Parameters)
	{
		// A template may use one defined after it.
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Outer(X) = Inner(X) + 1;
    Template Inner(Y) = Y * 2;
    State Root {
        utility Outer(3)
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("the inner template is expanded inside the outer"), ValueShape(Main, TEXT("Root"), 0), FString(TEXT("((3 * 2) + 1)")));
		TestEqualSensitive(TEXT("Outer's definition still calls Inner"), TemplateBodyShape(Main, TEXT("Outer")), FString(TEXT("(Inner(X) + 1)")));

		// One origin per expansion: Outer for the use, Inner for the call inside it.
		TestEqual(TEXT("two expansions, two origins"), Main.Origins.Num(), 2);
		TSet<FString> Names;
		for (const TPair<const FDstNode*, FDstExpansionOrigin>& Pair : Main.Origins)
		{
			Names.Add(Pair.Value.TemplateName);
		}
		TestTrue(TEXT("one of them is Outer"), Names.Contains(TEXT("Outer")));
		TestTrue(TEXT("and one is Inner"), Names.Contains(TEXT("Inner")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateNoParenthesesTest, "DreamStateTree.Lang.Templates.NoParentheses",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateNoParenthesesTest::RunTest(const FString& Parameters)
	{
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Nervous = Chance(1 - Params.Courage);
    Template Pause() = Wait(1);
    State Root {
        if Nervous
        if Nervous()
        do Pause, Pause()
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("a bare name replaces a template written without parentheses"), ValueShape(Main, TEXT("Root"), 0), FString(TEXT("Chance((1 - Params.Courage))")));
		TestEqualSensitive(TEXT("so does a call with no arguments"), ValueShape(Main, TEXT("Root"), 1), FString(TEXT("Chance((1 - Params.Courage))")));
		TestEqualSensitive(TEXT("a bare name does not replace a template written with parentheses"), TaskShape(Main, TEXT("Root"), 2, 0), FString(TEXT("Pause")));
		TestEqualSensitive(TEXT("its call does"), TaskShape(Main, TEXT("Root"), 2, 1), FString(TEXT("Wait(1)")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateEverywhereTest, "DreamStateTree.Lang.Templates.Everywhere",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateEverywhereTest::RunTest(const FString& Parameters)
	{
		// Every place an expression can stand: no use of the template is left, and each became its body.
		const FString Text = MainWith(TEXT(R"DST(    Template Five = 5;
    Parameters = { float A = Five; Array<float> B = [Five, Five]; }
    Globals = { Info = GetPlayerInfo(MaxRange = Five); }
    Colors = { Combat = (Five, 0, 0); }
    State Root [ Weight = Five ] {
        if Check(Five)
        pick Random
        utility Five * 0.5
        do Wait(Five) [ Name = "w" ]
        -> Root on Tick if Flag [ Delay = Five ]
        State Child { link Sub(P = Five) }
    }
    Subtree Sub(float P = Five) { State X { do Wait(P) } }
)DST"));
		const int32 ExpectedUses = 12;

		{
			FDstSourceText Source(MainPath, Text);
			FDstDiagnosticSink Sink(MainPath);
			const TUniquePtr<FDstFile> Parsed = ParseDst(Source, Sink);
			TestEqual(TEXT("parse: no diagnostics"), Sink.Num(), 0);
			int32 Uses = 0;
			if (Parsed && Parsed->Document)
			{
				CountNameUses(*Parsed->Document, TEXT("Five"), Uses);
			}
			TestEqual(TEXT("before expansion the traversal finds every use of Five"), Uses, ExpectedUses);
		}

		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(Text, Loader, Run);
		TestEqual(TEXT("expand: no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main || !Run.Unit.Main->Document)
		{
			return false;
		}
		int32 Left = 0;
		CountNameUses(*Run.Unit.Main->Document, TEXT("Five"), Left);
		TestEqual(TEXT("after expansion no use of Five is left"), Left, 0);

		// Each use became a 5, and the definition keeps its own.
		int32 Fives = 0;
		CountNumbers(*Run.Unit.Main->Document, TEXT("5"), Fives);
		TestEqual(TEXT("a 5 for every use, and the one in the definition"), Fives, ExpectedUses + 1);
		TestEqual(TEXT("one origin per use"), Run.Unit.Main->Origins.Num(), ExpectedUses);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateOriginsTest, "DreamStateTree.Lang.Templates.OriginsAndSpans",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateOriginsTest::RunTest(const FString& Parameters)
	{
		const FString Text = MainWith(TEXT(R"DST(    Template Near(Who) = Who.Distance < 100;
    State Root {
        if Near(Player)
    }
)DST"));
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(Text, Loader, Run);
		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		const FDstClauseStmt* If = ClauseOf(Main, TEXT("Root"), 0);
		if (!If || !If->Value)
		{
			AddError(TEXT("Root should have an if clause."));
			return false;
		}

		const FString CallText = TEXT("Near(Player)");
		const int32 CallOffset = Text.Find(CallText);

		// The expansion takes the span of the call it replaced, so a diagnostic about it points at what the author wrote.
		TestTrue(TEXT("the expansion starts where the call started"), If->Value->Span.Offset == CallOffset && If->Value->Span.Length == CallText.Len());
		TestTrue(TEXT("on the call's line and column"), If->Value->Span.Line == 5 && If->Value->Span.Column == 12);
		const FDstBinaryExpr* Less = If->Value->As<FDstBinaryExpr>();
		TestNotNull(TEXT("the body is a comparison"), Less);
		if (Less && Less->Lhs)
		{
			TestEqual(TEXT("so are the nodes of the body"), Less->Lhs->Span.Offset, CallOffset);
			const FDstMemberExpr* Member = Less->Lhs->As<FDstMemberExpr>();
			if (Member && Member->Object)
			{
				// An argument keeps its own place in the call.
				TestEqualSensitive(TEXT("an argument keeps its own span"), Run.Unit.MainSource.Slice(Member->Object->Span), FString(TEXT("Player")));
			}
		}

		TestEqual(TEXT("one origin"), Main.Origins.Num(), 1);
		const FDstExpansionOrigin* Origin = Main.Origins.Find(If->Value.Get());
		TestNotNull(TEXT("keyed by the root of the expansion"), Origin);
		if (Origin)
		{
			TestEqualSensitive(TEXT("template name"), Origin->TemplateName, FString(TEXT("Near")));
			TestEqualSensitive(TEXT("the call that was replaced"), Run.Unit.MainSource.Slice(Origin->CallSpan), CallText);
			TestEqual(TEXT("the definition's line"), Origin->DefinitionSpan.Line, 3);
			TestEqualSensitive(TEXT("the definition's file"), Origin->DefinitionFile, FString(MainPath));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Includes
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateIncludesTest, "DreamStateTree.Lang.Templates.Includes",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateIncludesTest::RunTest(const FString& Parameters)
	{
		FFakeIncludeLoader Loader;
		Loader.Add(TEXT("Mem/Lib/Common.dth"), TEXT("#include \"Base.dth\"\n") + LibraryWith(TEXT("Lib/Common"), TEXT(R"DST(    Template Daytime(From, To) = Hour(From, To) and not Raining();
    Template Twice(X) = Base(X) + Base(X);
    Subtree HitReaction(float Toughness = 1) { State Stagger { do Stagger(0.8) } }
)DST")));
		Loader.Add(TEXT("Mem/Lib/Base.dth"), LibraryWith(TEXT("Lib/Base"), TEXT("    Template Base(X) = X * 2;\n")));

		// Base is reached twice, once through Common and once directly: it is parsed once.
		FRun Run;
		RunText(TEXT("#include \"Lib/Common.dth\"\n#include \"Lib/Base.dth\"\n") + MainWith(TEXT(R"DST(    State Root {
        if Daytime(9, 17)
        utility Twice(3)
        link HitReaction
    }
)DST")), Loader, Run);

		TestTrue(TEXT("includes resolve"), Run.bIncludesOk);
		TestTrue(TEXT("templates expand"), Run.bTemplatesOk);
		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}

		TestEqual(TEXT("two libraries, each once"), Run.Unit.Libraries.Num(), 2);
		TestEqual(TEXT("a source for each"), Run.Unit.LibrarySources.Num(), Run.Unit.Libraries.Num());
		if (Run.Unit.Libraries.Num() == 2 && Run.Unit.LibrarySources.Num() == 2)
		{
			TestEqualSensitive(TEXT("first reached: Common"), Run.Unit.Libraries[0]->Path, FString(TEXT("Mem/Lib/Common.dth")));
			TestEqualSensitive(TEXT("then Base, reached through Common"), Run.Unit.Libraries[1]->Path, FString(TEXT("Mem/Lib/Base.dth")));
			TestEqualSensitive(TEXT("sources follow the libraries"), Run.Unit.LibrarySources[1].GetPath(), Run.Unit.Libraries[1]->Path);
			TestTrue(TEXT("a library is a library"),
				Run.Unit.Libraries[0]->Document && Run.Unit.Libraries[0]->Document->DocumentKind == EDstDocumentKind::StateTreeLibrary);
		}

		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("a library template with an operator and a call"), ValueShape(Main, TEXT("Root"), 0),
			FString(TEXT("(Hour(9,17) and (not Raining()))")));
		TestEqualSensitive(TEXT("a library template that uses another library's"), ValueShape(Main, TEXT("Root"), 1), FString(TEXT("((3 * 2) + (3 * 2))")));
		TestEqualSensitive(TEXT("a link to a library subtree is left for the next stage"), ValueShape(Main, TEXT("Root"), 2), FString(TEXT("HitReaction")));
		TestEqual(TEXT("the library's subtree is not copied into the main file"), Main.Document ? Main.Document->Members.Num() : -1, 1);

		// The origin names the file the definition is in.
		const FDstClauseStmt* Utility = ClauseOf(Main, TEXT("Root"), 1);
		const FDstExpansionOrigin* Origin = Utility && Utility->Value ? Main.Origins.Find(Utility->Value.Get()) : nullptr;
		TestNotNull(TEXT("the expansion has an origin"), Origin);
		if (Origin)
		{
			TestEqualSensitive(TEXT("template"), Origin->TemplateName, FString(TEXT("Twice")));
			TestEqualSensitive(TEXT("defined in the library"), Origin->DefinitionFile, FString(TEXT("Mem/Lib/Common.dth")));
			TestEqual(TEXT("on the library's line"), Origin->DefinitionSpan.Line, 5);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangIncludeErrorsTest, "DreamStateTree.Lang.Templates.IncludeErrors",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangIncludeErrorsTest::RunTest(const FString& Parameters)
	{
		// DST3201: the file is not there. The position is the path string.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(TEXT("#include \"Lib/Nowhere.dth\"\n") + MainWith(TEXT("    State Root { }\n")), Loader, Run);
			TestFalse(TEXT("DST3201: includes fail"), Run.bIncludesOk);
			const FDstDiagnostic* Diagnostic = ExpectDiagnostic(*this, Run.Sink, TEXT("DST3201"), 1, 10);
			if (Diagnostic)
			{
				TestEqualSensitive(TEXT("DST3201 is about the main file"), Diagnostic->FilePath, FString(MainPath));
				TestEqual(TEXT("DST3201 is an error"), static_cast<int32>(Diagnostic->Severity), static_cast<int32>(EDstSeverity::Error));
			}
			TestEqual(TEXT("DST3201: no library was added"), Run.Unit.Libraries.Num(), 0);
		}

		// DST3201 inside a library carries the library's path.
		{
			FFakeIncludeLoader Loader;
			Loader.Add(TEXT("Mem/Lib/Common.dth"), TEXT("#include \"Missing.dth\"\n") + LibraryWith(TEXT("Lib/Common"), FString()));
			FRun Run;
			RunText(TEXT("#include \"Lib/Common.dth\"\n") + MainWith(TEXT("    State Root { }\n")), Loader, Run);
			TestFalse(TEXT("DST3201 in a library: includes fail"), Run.bIncludesOk);
			const FDstDiagnostic* Diagnostic = ExpectDiagnostic(*this, Run.Sink, TEXT("DST3201"), 1, 10);
			if (Diagnostic)
			{
				TestEqualSensitive(TEXT("DST3201 in a library names the library"), Diagnostic->FilePath, FString(TEXT("Mem/Lib/Common.dth")));
			}
		}

		// DST3202: a cycle. The include that closes it is the one in the later file.
		{
			FFakeIncludeLoader Loader;
			Loader.Add(TEXT("Mem/Lib/A.dth"), TEXT("#include \"B.dth\"\n") + LibraryWith(TEXT("Lib/A"), FString()));
			Loader.Add(TEXT("Mem/Lib/B.dth"), TEXT("#include \"A.dth\"\n") + LibraryWith(TEXT("Lib/B"), FString()));
			FRun Run;
			RunText(TEXT("#include \"Lib/A.dth\"\n") + MainWith(TEXT("    State Root { }\n")), Loader, Run);
			TestFalse(TEXT("DST3202: includes fail"), Run.bIncludesOk);
			const FDstDiagnostic* Diagnostic = ExpectDiagnostic(*this, Run.Sink, TEXT("DST3202"), 1, 10);
			if (Diagnostic)
			{
				TestEqualSensitive(TEXT("DST3202 is reported where the cycle closes"), Diagnostic->FilePath, FString(TEXT("Mem/Lib/B.dth")));
			}
			TestEqual(TEXT("DST3202: reported once"), CountDiagnostics(Run.Sink, TEXT("DST3202")), 1);
		}
		{
			FFakeIncludeLoader Loader;
			Loader.Add(TEXT("Mem/Lib/Self.dth"), TEXT("#include \"Self.dth\"\n") + LibraryWith(TEXT("Lib/Self"), FString()));
			FRun Run;
			RunText(TEXT("#include \"Lib/Self.dth\"\n") + MainWith(TEXT("    State Root { }\n")), Loader, Run);
			TestFalse(TEXT("a library including itself: includes fail"), Run.bIncludesOk);
			TestEqual(TEXT("a library including itself: DST3202"), CountDiagnostics(Run.Sink, TEXT("DST3202")), 1);
		}

		// DST3203: an included file must hold a library.
		{
			FFakeIncludeLoader Loader;
			Loader.Add(TEXT("Mem/Lib/NotLib.dtr"), MainWith(TEXT("    State Other { }\n")));
			FRun Run;
			RunText(TEXT("#include \"Lib/NotLib.dtr\"\n") + MainWith(TEXT("    State Root { }\n")), Loader, Run);
			TestFalse(TEXT("DST3203: includes fail"), Run.bIncludesOk);
			const FDstDiagnostic* Diagnostic = ExpectDiagnostic(*this, Run.Sink, TEXT("DST3203"), 1, 10);
			if (Diagnostic)
			{
				TestEqualSensitive(TEXT("DST3203 is about the including file"), Diagnostic->FilePath, FString(MainPath));
			}
			TestEqual(TEXT("DST3203: the file is not kept as a library"), Run.Unit.Libraries.Num(), 0);
		}

		// A syntax error in a library is reported with the library's path and position.
		{
			FFakeIncludeLoader Loader;
			Loader.Add(TEXT("Mem/Lib/Broken.dth"), LibraryWith(TEXT("Lib/Broken"), TEXT("    Template T = 1 @;\n")));
			FRun Run;
			RunText(TEXT("#include \"Lib/Broken.dth\"\n") + MainWith(TEXT("    State Root { }\n")), Loader, Run);
			const FDstDiagnostic* Diagnostic = ExpectDiagnostic(*this, Run.Sink, TEXT("DST1001"), 3, 20);
			if (Diagnostic)
			{
				TestEqualSensitive(TEXT("a library's own error names the library"), Diagnostic->FilePath, FString(TEXT("Mem/Lib/Broken.dth")));
			}
			TestTrue(TEXT("and counts as an error"), Run.Sink.HasErrors());
		}

		// Nothing to include, nothing to say.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    State Root { }\n")), Loader, Run);
			TestTrue(TEXT("no includes: ok"), Run.bIncludesOk && Run.bTemplatesOk);
			TestEqual(TEXT("no includes: no diagnostics"), Run.Sink.Num(), 0);
			TestEqual(TEXT("no includes: no libraries"), Run.Unit.Libraries.Num(), 0);
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Template errors
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateArgumentErrorsTest, "DreamStateTree.Lang.Templates.ArgumentErrors",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateArgumentErrorsTest::RunTest(const FString& Parameters)
	{
		const FString Definition = TEXT("    Template Near(Who) = Who.Distance < 100;\n    State Root {\n");

		// DST3101: an argument is missing. The position is the call.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(Definition + TEXT("        if Near()\n    }\n")), Loader, Run);
			TestFalse(TEXT("DST3101: expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3101"), 5, 12);
			TestEqual(TEXT("DST3101: only that"), Run.Sink.Num(), 1);
		}

		// DST3102: an argument name the template does not have, and one given twice. The position is the name.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(Definition + TEXT("        if Near(Player, Bogus = 1)\n    }\n")), Loader, Run);
			TestFalse(TEXT("DST3102: expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3102"), 5, 25);
		}
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(Definition + TEXT("        if Near(Player, Who = Player)\n    }\n")), Loader, Run);
			TestFalse(TEXT("DST3102 (twice): expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3102"), 5, 25);
		}

		// DST3103: one argument too many. The position is the extra argument.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(Definition + TEXT("        if Near(Player, 1)\n    }\n")), Loader, Run);
			TestFalse(TEXT("DST3103: expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3103"), 5, 25);
		}
		{
			// A template written without parentheses takes no argument at all.
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template Five = 5;\n    State Root {\n        if Five(1)\n    }\n")), Loader, Run);
			TestFalse(TEXT("DST3103 (no parameters): expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3103"), 5, 17);
		}

		// A bad use does not stop the good ones.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(Definition + TEXT("        if Near()\n        if Near(Player)\n    }\n")), Loader, Run);
			TestFalse(TEXT("one bad use: expansion fails"), Run.bTemplatesOk);
			TestEqual(TEXT("one bad use: one diagnostic"), CountDiagnostics(Run.Sink, TEXT("DST3101")), 1);
			if (Run.Unit.Main)
			{
				TestEqualSensitive(TEXT("one bad use: the good use is still expanded"), ValueShape(Run.Main(), TEXT("Root"), 1), FString(TEXT("(Player.Distance < 100)")));
			}
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateDepthTest, "DreamStateTree.Lang.Templates.Depth",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateDepthTest::RunTest(const FString& Parameters)
	{
		// DST3104: more than 16 levels. A template that calls itself is the usual way there.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template Loop(X) = Loop(X);\n    State Root {\n        utility Loop(1)\n    }\n")), Loader, Run);
			TestFalse(TEXT("self recursion: expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3104"), 5, 17);
			TestEqual(TEXT("self recursion: reported once"), CountDiagnostics(Run.Sink, TEXT("DST3104")), 1);
		}
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template A(X) = B(X);\n    Template B(X) = A(X);\n    State Root {\n        utility A(1)\n    }\n")), Loader, Run);
			TestFalse(TEXT("mutual recursion: expansion fails"), Run.bTemplatesOk);
			TestEqual(TEXT("mutual recursion: DST3104"), CountDiagnostics(Run.Sink, TEXT("DST3104")), 1);
		}

		// Sixteen levels deep is the limit: the chain T1 -> ... -> T16 expands, one more does not.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(ChainMembers(16)), Loader, Run);
			TestTrue(TEXT("16 levels: expansion works"), Run.bTemplatesOk);
			TestEqual(TEXT("16 levels: no diagnostics"), Run.Sink.Num(), 0);
			if (Run.Unit.Main)
			{
				TestEqualSensitive(TEXT("16 levels: the argument comes out the other end"), ValueShape(Run.Main(), TEXT("Root"), 0), FString(TEXT("5")));
			}
		}
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(ChainMembers(17)), Loader, Run);
			TestFalse(TEXT("17 levels: expansion fails"), Run.bTemplatesOk);
			TestEqual(TEXT("17 levels: DST3104"), CountDiagnostics(Run.Sink, TEXT("DST3104")), 1);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateDefinitionErrorsTest, "DreamStateTree.Lang.Templates.DefinitionErrors",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateDefinitionErrorsTest::RunTest(const FString& Parameters)
	{
		// DST3105: two templates with one name. The position is the name of the second.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template Dup(X) = X;\n    Template Dup(Y) = Y;\n    State Root { }\n")), Loader, Run);
			TestFalse(TEXT("DST3105: expansion fails"), Run.bTemplatesOk);
			const FDstDiagnostic* Diagnostic = ExpectDiagnostic(*this, Run.Sink, TEXT("DST3105"), 4, 14);
			if (Diagnostic)
			{
				TestEqualSensitive(TEXT("DST3105 is about the main file"), Diagnostic->FilePath, FString(MainPath));
			}
			TestEqual(TEXT("DST3105: only that"), Run.Sink.Num(), 1);
		}

		// A library and the file that includes it clash too.
		{
			FFakeIncludeLoader Loader;
			Loader.Add(TEXT("Mem/Lib/Common.dth"), LibraryWith(TEXT("Lib/Common"), TEXT("    Template Dup(X) = X;\n")));
			FRun Run;
			RunText(TEXT("#include \"Lib/Common.dth\"\n") + MainWith(TEXT("    Template Dup(Y) = Y;\n    State Root { }\n")), Loader, Run);
			TestFalse(TEXT("DST3105 across files: expansion fails"), Run.bTemplatesOk);
			TestEqual(TEXT("DST3105 across files: reported once"), CountDiagnostics(Run.Sink, TEXT("DST3105")), 1);
		}

		// DST3106: a template without `= Body`. The position is its name.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template Empty(X);\n    State Root { }\n")), Loader, Run);
			// The parser raises it: a template without a body is not kept in the tree, so the expansion has nothing to refuse.
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3106"), 3, 14);
			TestEqual(TEXT("DST3106: only that"), Run.Sink.Num(), 1);
			if (Run.Unit.Main)
			{
				TestNull(TEXT("DST3106: the template is not kept"), FindTemplate(Run.Main(), TEXT("Empty")));
			}
		}

		// A template that is never used is never expanded, so its own mistakes in the body are not reported.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template Unused(X) = Missing(X, Y);\n    State Root { do Wait(1) }\n")), Loader, Run);
			TestTrue(TEXT("an unused template: expansion works"), Run.bTemplatesOk);
			TestEqual(TEXT("an unused template: no diagnostics"), Run.Sink.Num(), 0);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateUseSettingsTest, "DreamStateTree.Lang.Templates.UseSettings",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateUseSettingsTest::RunTest(const FString& Parameters)
	{
		// Settings written after a use go onto the call the template expands to; one with the same key replaces the template's own.
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(MainWith(TEXT(R"DST(    Template Pause() = Wait(1);
    Template Slow = Walk(1) [ Name = "slow"; Enabled = true ];
    State Root {
        do Pause() [ Name = "x" ]
        do Slow() [ Enabled = false ]
        do Slow
    }
)DST")), Loader, Run);

		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);
		if (!Run.Unit.Main)
		{
			return false;
		}
		const FDstFile& Main = Run.Main();
		TestEqualSensitive(TEXT("settings at a use are put on the call"), TaskShape(Main, TEXT("Root"), 0, 0), FString(TEXT("Wait(1)<Name=\"x\">")));
		TestEqualSensitive(TEXT("a setting with the template's own key replaces it"), TaskShape(Main, TEXT("Root"), 1, 0), FString(TEXT("Walk(1)<Name=\"slow\";Enabled=false>")));
		TestEqualSensitive(TEXT("without settings the body is as written"), TaskShape(Main, TEXT("Root"), 2, 0), FString(TEXT("Walk(1)<Name=\"slow\";Enabled=true>")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateUseErrorsTest, "DreamStateTree.Lang.Templates.UseErrors",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateUseErrorsTest::RunTest(const FString& Parameters)
	{
		// DST3107: a template takes values; `Who -> Path` copies a node's output into a path.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template Near(Who) = Who.Distance < 100;\n    State Root {\n        if Near(Who -> Params.W)\n    }\n")), Loader, Run);
			TestFalse(TEXT("DST3107: expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3107"), 5, 17);
		}

		// DST3108: settings at a use whose template is not a call have nothing to land on.
		{
			FFakeIncludeLoader Loader;
			FRun Run;
			RunText(MainWith(TEXT("    Template Cond = Alert and Armed;\n    State Root {\n        if Cond() [ Name = \"x\" ]\n    }\n")), Loader, Run);
			TestFalse(TEXT("DST3108: expansion fails"), Run.bTemplatesOk);
			ExpectDiagnostic(*this, Run.Sink, TEXT("DST3108"), 5, 19);
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstLangTemplateNoTemplatesTest, "DreamStateTree.Lang.Templates.NoTemplates",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstLangTemplateNoTemplatesTest::RunTest(const FString& Parameters)
	{
		// A file without templates is left exactly as parsed.
		const FString Text = MainWith(TEXT(R"DST(    Parameters = { float A = 1; }
    State Root {
        if Near(Player) and Check(2)
        do Wait(A)
    }
)DST"));
		FFakeIncludeLoader Loader;
		FRun Run;
		RunText(Text, Loader, Run);
		TestTrue(TEXT("expansion works"), Run.bIncludesOk && Run.bTemplatesOk);
		TestEqual(TEXT("no diagnostics"), Run.Sink.Num(), 0);

		FDstDiagnosticSink Sink(MainPath);
		const FDstSourceText Source(MainPath, Text);
		const TUniquePtr<FDstFile> Fresh = ParseDst(Source, Sink);
		if (Run.Unit.Main && Fresh)
		{
			TestEqualSensitive(TEXT("the tree is unchanged"), DumpDstTree(*Run.Unit.Main), DumpDstTree(*Fresh));
			TestEqual(TEXT("and there are no origins"), Run.Unit.Main->Origins.Num(), 0);
		}

		// A unit with no main file at all is not an error.
		FDstCompilationUnit Empty;
		FDstDiagnosticSink EmptySink;
		TestTrue(TEXT("no main: includes are fine"), ResolveDstIncludes(Empty, Loader, EmptySink));
		TestTrue(TEXT("no main: templates are fine"), ExpandDstTemplates(Empty, EmptySink));
		TestEqual(TEXT("no main: nothing reported"), EmptySink.Num(), 0);
		return true;
	}
}
