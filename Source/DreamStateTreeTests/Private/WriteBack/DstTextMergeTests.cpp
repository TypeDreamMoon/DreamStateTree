// Copyright DreamMoon. All Rights Reserved.
//
// The three-way merge used when the text changed on disk while the asset had changes of its own.

#include "DstTextMerge.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamStateTree::Tests::TextMergeTests
{
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDstTextMergeTest, "DreamStateTree.WriteBack.Merge",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDstTextMergeTest::RunTest(const FString& Parameters)
	{
		const FString Base = TEXT("Tree {\n    One = 1;\n    Two = 2;\n\n    Three = 3;\n    Four = 4;\n}\n");
		FString Merged;
		TArray<Compiler::FDstMergeConflict> Conflicts;

		// Apart: both are taken.
		TestTrue(TEXT("changes apart merge"), Compiler::MergeDstTexts(Base,
			TEXT("Tree {\n    One = 10;\n    Two = 2;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("Tree {\n    One = 1;\n    Two = 2;\n\n    Three = 3;\n    Four = 40;\n    Five = 5;\n}\n"), Merged, Conflicts));
		TestEqualSensitive(TEXT("both changes"), Merged, FString(TEXT("Tree {\n    One = 10;\n    Two = 2;\n\n    Three = 3;\n    Four = 40;\n    Five = 5;\n}\n")));

		// The same change on both sides is taken once.
		TestTrue(TEXT("the same change merges"), Compiler::MergeDstTexts(Base,
			TEXT("Tree {\n    One = 1;\n    Two = 20;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("Tree {\n    One = 1;\n    Two = 20;\n\n    Three = 3;\n    Four = 4;\n}\n"), Merged, Conflicts));
		TestEqualSensitive(TEXT("taken once"), Merged, FString(TEXT("Tree {\n    One = 1;\n    Two = 20;\n\n    Three = 3;\n    Four = 4;\n}\n")));

		// The same line changed differently: a conflict, nothing merged.
		TestFalse(TEXT("one line changed two ways conflicts"), Compiler::MergeDstTexts(Base,
			TEXT("Tree {\n    One = 1;\n    Two = 21;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("Tree {\n    One = 1;\n    Two = 22;\n\n    Three = 3;\n    Four = 4;\n}\n"), Merged, Conflicts));
		TestTrue(TEXT("nothing merged"), Merged.IsEmpty());
		if (TestEqual(TEXT("one conflict"), Conflicts.Num(), 1))
		{
			TestEqual(TEXT("at the line in the base"), Conflicts[0].Line, 3);
			TestEqualSensitive(TEXT("ours"), Conflicts[0].Ours, FString(TEXT("    Two = 21;")));
			TestEqualSensitive(TEXT("theirs"), Conflicts[0].Theirs, FString(TEXT("    Two = 22;")));
		}

		TestFalse(TEXT("case-distinct replacements conflict"), Compiler::MergeDstTexts(TEXT("Label = idle;\n"),
			TEXT("Label = Walk;\n"), TEXT("Label = walk;\n"), Merged, Conflicts));
		TestEqual(TEXT("case conflict is reported"), Conflicts.Num(), 1);
		TestTrue(TEXT("identical case-only edits merge"), Compiler::MergeDstTexts(TEXT("Label = Walk;\n"),
			TEXT("Label = walk;\n"), TEXT("Label = walk;\n"), Merged, Conflicts));
		TestEqualSensitive(TEXT("case-only edit survives"), Merged, FString(TEXT("Label = walk;\n")));

		// Neighbouring lines are not taken apart.
		TestFalse(TEXT("neighbouring lines conflict"), Compiler::MergeDstTexts(Base,
			TEXT("Tree {\n    One = 11;\n    Two = 2;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("Tree {\n    One = 1;\n    Two = 22;\n\n    Three = 3;\n    Four = 4;\n}\n"), Merged, Conflicts));

		// Line ends stay as they are.
		const FString CrlfBase = Base.Replace(TEXT("\n"), TEXT("\r\n"));
		TestTrue(TEXT("CRLF text merges"), Compiler::MergeDstTexts(CrlfBase,
			CrlfBase.Replace(TEXT("One = 1;"), TEXT("One = 10;")),
			CrlfBase.Replace(TEXT("Four = 4;"), TEXT("Four = 40;")), Merged, Conflicts));
		TestEqualSensitive(TEXT("CRLF kept"), Merged, CrlfBase.Replace(TEXT("One = 1;"), TEXT("One = 10;")).Replace(TEXT("Four = 4;"), TEXT("Four = 40;")));

		// A tree edited in the editor (a task added) and in a text editor (a state renamed) far apart merge.
		const FString TreeBase = TEXT("State Root {\n    State A { do Delay(1) }\n    State B { do Delay(2) }\n\n    State C { do Delay(3) }\n}\n");
		TestTrue(TEXT("editor change and text change merge"), Compiler::MergeDstTexts(TreeBase,
			TreeBase.Replace(TEXT("Delay(1)"), TEXT("Delay(1), Delay(5)")),
			TreeBase.Replace(TEXT("State C"), TEXT("State Last")), Merged, Conflicts));
		TestEqualSensitive(TEXT("both land"), Merged, TreeBase.Replace(TEXT("Delay(1)"), TEXT("Delay(1), Delay(5)")).Replace(TEXT("State C"), TEXT("State Last")));
		return true;
	}
}

#endif
