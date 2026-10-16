// Copyright DreamMoon. All Rights Reserved.
//
// What the compiler-stage tests share: where the corpus lives and how its cases are listed, the fixture tags a text
// may name, the assets a test generates and must give back, the directives corpus files carry in comments, and the
// small conversions the round trips compare through. Everything is inline in one namespace. The test module is built as
// a unity, so a helper of the same name in the anonymous namespace of two .cpp files would collide.
//
// Directives are `// WORD rest` comment lines anywhere in a corpus file:
//   // REQUIRE LinkChild.dtr     build this tree (into /Game/DreamStateTreeTests/Generated) before the case is judged,
//                                because the case links its asset
//   // RUN, // OWNER, // SEND    a case the behavior tests run (see DstBehaviorTests.cpp)

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DstDriver.h"
#include "DstExtract.h"
#include "DstGenerate.h"
#include "DstLower.h"
#include "Fixtures/DstTestTags.h"
#include "HAL/FileManager.h"
#include "Lang/DstLangTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Model/DstModel.h"
#include "StateTree.h"
#include "UObject/Package.h"

namespace UE::DreamStateTree::Tests::CompilerSupport
{
	// ------------------------------------------------------------------------------------------
	// Where things are
	// ------------------------------------------------------------------------------------------

	/** Absolute, `/`-separated, `..` collapsed. */
	inline FString NormalizeDirectory(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Result);
		FPaths::CollapseRelativeDirectories(Result);
		while (Result.Len() > 3 && Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1);
		}
		return Result;
	}

	/** <plugin>/Tests/Corpus/<SubDir>, or empty when the plugin cannot be found. */
	inline FString GetCorpusSubDir(const TCHAR* SubDir)
	{
		const FString Corpus = LangSupport::GetCorpusDir();
		return Corpus.IsEmpty() ? FString() : NormalizeDirectory(FPaths::Combine(Corpus, SubDir));
	}

	/** <project>/Saved/DreamStateTreeTests/Compiler/<Folder>: where the tests leave what a person needs to look at. */
	inline FString GetReportDir(const TCHAR* Folder)
	{
		return NormalizeDirectory(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamStateTreeTests"), TEXT("Compiler"), Folder));
	}

	/** Writes a report file (UTF-8) and returns its path. */
	inline FString WriteReport(const TCHAR* Folder, const FString& Name, const FString& Text)
	{
		const FString Flat = FPaths::MakeValidFileName(Name.Replace(TEXT("/"), TEXT("_")));
		const FString Path = FPaths::Combine(GetReportDir(Folder), Flat);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return Path;
	}

	/** CheckGolden that first makes the golden's folder: the decompiled goldens live in folders of their own. */
	inline bool CheckGoldenInFolder(FAutomationTestBase& Test, const FString& GoldenPath, const FString& Actual)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(GoldenPath), /*Tree*/ true);
		return LangSupport::CheckGolden(Test, GoldenPath, Actual);
	}

	// ------------------------------------------------------------------------------------------
	// Cases
	// ------------------------------------------------------------------------------------------

	inline bool IsCaseFileName(const FString& File, const bool bTrees, const bool bLibraries)
	{
		const FString Extension = FPaths::GetExtension(File);
		return (bTrees && Extension.Equals(TEXT("dtr"), ESearchCase::IgnoreCase))
			|| (bLibraries && Extension.Equals(TEXT("dth"), ESearchCase::IgnoreCase));
	}

	/** Every case file below Dir (normalised, sorted), leaving out files inside a folder called one of SkipFolders. */
	inline void CollectCaseFiles(const FString& Dir, const bool bTrees, const bool bLibraries, TConstArrayView<FString> SkipFolders, TArray<FString>& OutFullPaths)
	{
		const FString Root = NormalizeDirectory(Dir);
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Root, TEXT("*.*"), /*Files*/ true, /*Directories*/ false);
		for (const FString& File : Found)
		{
			if (!IsCaseFileName(File, bTrees, bLibraries))
			{
				continue;
			}
			FString Full = File;
			FPaths::NormalizeFilename(Full);

			FString Relative = Full.StartsWith(Root) ? Full.Mid(Root.Len()) : FPaths::GetCleanFilename(Full);
			Relative.RemoveFromStart(TEXT("/"));
			TArray<FString> Parts;
			Relative.ParseIntoArray(Parts, TEXT("/"));
			bool bSkip = false;
			for (int32 Index = 0; Index + 1 < Parts.Num() && !bSkip; ++Index)
			{
				bSkip = SkipFolders.ContainsByPredicate([&Parts, Index](const FString& Skip) { return Skip.Equals(Parts[Index], ESearchCase::IgnoreCase); });
			}
			if (!bSkip)
			{
				OutFullPaths.Add(Full);
			}
		}
		OutFullPaths.Sort();
	}

	/** Fills the two arrays a complex test's GetTests wants: names relative to Dir, commands = full paths. */
	inline void ListCases(const FString& Dir, const bool bTrees, const bool bLibraries, TConstArrayView<FString> SkipFolders,
		TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
	{
		TArray<FString> Files;
		if (!Dir.IsEmpty())
		{
			CollectCaseFiles(Dir, bTrees, bLibraries, SkipFolders, Files);
		}
		if (Files.IsEmpty())
		{
			// A missing corpus must fail loudly, not pass with nothing to run.
			OutBeautifiedNames.Add(TEXT("corpus_not_found"));
			OutTestCommands.Add(FString());
			return;
		}
		const FString Root = NormalizeDirectory(Dir);
		for (const FString& File : Files)
		{
			FString Relative = File.StartsWith(Root) ? File.Mid(Root.Len()) : FPaths::GetCleanFilename(File);
			Relative.RemoveFromStart(TEXT("/"));
			OutBeautifiedNames.Add(Relative);
			OutTestCommands.Add(File);
		}
	}

	/** The rest of every `// WORD rest` comment line, in file order. */
	inline TArray<FString> ReadDirectives(const FString& Text, const TCHAR* Word)
	{
		TArray<FString> Result;
		const FString Prefix = FString::Printf(TEXT("// %s"), Word);
		TArray<FString> Lines;
		LangSupport::NormalizeNewlines(Text).ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty*/ false);
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStart();
			if (Line.StartsWith(Prefix, ESearchCase::CaseSensitive) && (Line.Len() == Prefix.Len() || Line[Prefix.Len()] == TEXT(' ')))
			{
				Result.Add(Line.Mid(Prefix.Len()).TrimStartAndEnd());
			}
		}
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// Tags
	// ------------------------------------------------------------------------------------------

	/**
	 * The fixture tags are added to the tag manager the first time one is asked for. A text that names one (`on Hit`) has to find
	 * it already there, so every test that builds a text asks for all of them first.
	 */
	inline void EnsureTestTags()
	{
		(void)Tag_Event_Hit.GetTag();
		(void)Tag_Event_Hit_Light.GetTag();
		(void)Tag_Event_Hit_Heavy.GetTag();
		(void)Tag_Event_Ask.GetTag();
		(void)Tag_State_Combat.GetTag();
		(void)Tag_State_Explore.GetTag();
	}

	// ------------------------------------------------------------------------------------------
	// Generated assets
	// ------------------------------------------------------------------------------------------

	/** `/Game/DreamStateTreeTests/Generated/<Name>`: where the corpus trees are generated. */
	inline FString GetGeneratedPackagePath(const FString& Name)
	{
		return FString(TEXT("/Game/DreamStateTreeTests/Generated/")) + Name;
	}

	/**
	 * The assets a test generates, given back when it ends: each is taken out of the asset registry and moved to the transient
	 * package, the way the experiments clean up, so the next test finds neither the asset nor its name. Nothing is saved to disk
	 * (the tests build with bSave off).
	 */
	class FGeneratedAssets : public FNoncopyable
	{
	public:
		~FGeneratedAssets()
		{
			Release();
		}

		/** Remembers an asset (a package path or an object path); it does not matter whether it ever gets created. */
		void Track(const FString& AssetPath)
		{
			FString ObjectPath = AssetPath;
			if (!ObjectPath.Contains(TEXT(".")))
			{
				ObjectPath += TEXT(".") + FPackageName::GetShortName(AssetPath);
			}
			Paths.AddUnique(ObjectPath);
		}

		void Release()
		{
			// Newest first: a tree that links another goes before the tree it links.
			for (int32 Index = Paths.Num() - 1; Index >= 0; --Index)
			{
				UStateTree* Asset = FindObject<UStateTree>(nullptr, *Paths[Index]);
				if (!Asset)
				{
					continue;
				}
				UPackage* Package = Asset->GetOutermost();
				FAssetRegistryModule::AssetDeleted(Asset);
				UE::StateTree::Compiler::RenameObjectToTransientPackage(Asset);
				if (Package)
				{
					Package->SetDirtyFlag(false);
				}
			}
			Paths.Reset();
		}

	private:
		TArray<FString> Paths;
	};

	/** Options for building a text: Roots is where its #includes resolve; nothing is saved. */
	inline Compiler::FDstBuildOptions MakeBuildOptions(const FString& Root, const bool bGenerate, const FString& OverrideAssetPath = FString())
	{
		Compiler::FDstBuildOptions Options;
		Options.bGenerate = bGenerate;
		Options.bSave = false;
		if (!Root.IsEmpty())
		{
			Options.Roots.Add(Root);
		}
		Options.OverrideAssetPath = OverrideAssetPath;
		return Options;
	}

	/** One line per error of a sink, for a failure message. */
	inline FString DescribeErrors(const Lang::FDstDiagnosticSink& Sink)
	{
		TArray<FString> Lines;
		for (const Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			if (Diagnostic.Severity == Lang::EDstSeverity::Error)
			{
				Lines.Add(Lang::FDstDiagnosticSink::ToWireString(Diagnostic));
			}
		}
		return Lines.IsEmpty() ? FString(TEXT("(no error)")) : FString::Join(Lines, TEXT("; "));
	}

	/**
	 * Builds every tree the text asks for with `// REQUIRE <file>` (relative to CaseDir), each into
	 * /Game/DreamStateTreeTests/Generated/<its file name>, requirements first. The trees are tracked in Assets. False, with the
	 * test failed, when one does not build.
	 */
	inline bool BuildRequiredTrees(FAutomationTestBase& Test, const FString& CaseDir, const FString& CaseText, FGeneratedAssets& Assets, TSet<FString>& InOutDone)
	{
		for (const FString& Name : ReadDirectives(CaseText, TEXT("REQUIRE")))
		{
			const FString File = NormalizeDirectory(FPaths::Combine(CaseDir, Name));
			if (InOutDone.Contains(File))
			{
				continue;
			}
			InOutDone.Add(File);

			FString Text;
			if (!LangSupport::LoadText(File, Text))
			{
				Test.AddError(FString::Printf(TEXT("Cannot read the required tree %s."), *File));
				return false;
			}
			if (!BuildRequiredTrees(Test, CaseDir, Text, Assets, InOutDone))
			{
				return false;
			}

			const FString AssetPath = GetGeneratedPackagePath(FPaths::GetBaseFilename(File));
			Assets.Track(AssetPath);
			Compiler::FDstFileResult Result = Compiler::BuildDstText(File, Text, MakeBuildOptions(CaseDir, /*bGenerate*/ true, AssetPath));
			if (Result.Diagnostics.HasErrors())
			{
				Test.AddError(FString::Printf(TEXT("The required tree %s does not build: %s"), *Name, *DescribeErrors(Result.Diagnostics)));
				return false;
			}
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Models and compiled assets as text
	// ------------------------------------------------------------------------------------------

	/** The dump of a model once every node something binds to has a name. Both sides of a model comparison go through this. */
	inline FString DumpEnsured(Model::FDstTree& InOutTree)
	{
		Compiler::EnsureDstBindNames(InOutTree);
		return Model::DumpDstModel(InOutTree);
	}

	/**
	 * Fails the test when two texts differ, saying where, and leaves both in the report folder to be compared with a diff tool.
	 * True when they are equal.
	 */
	inline bool CheckSameText(FAutomationTestBase& Test, const FString& What, const TCHAR* ReportFolder, const FString& Name, const FString& Expected, const FString& Actual)
	{
		const FString Difference = LangSupport::DescribeDifference(Expected, Actual);
		if (Difference.IsEmpty())
		{
			return true;
		}
		WriteReport(ReportFolder, Name + TEXT(".expected.txt"), Expected);
		WriteReport(ReportFolder, Name + TEXT(".actual.txt"), Actual);
		Test.AddError(FString::Printf(TEXT("%s (%s). Both texts are in %s."), *What, *Difference, *GetReportDir(ReportFolder)));
		return false;
	}

	namespace Detail
	{
		inline bool IsHexChar(const TCHAR Char)
		{
			return (Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f')) || (Char >= TEXT('A') && Char <= TEXT('F'));
		}

		inline bool IsWordChar(const TCHAR Char)
		{
			return FChar::IsAlnum(Char) || Char == TEXT('_');
		}

		/** 32 hex digits, or 8-4-4-4-12 with hyphens, at Start. */
		inline int32 GuidLengthAt(const FString& Text, const int32 Start)
		{
			const int32 Length = Text.Len();
			int32 Digits = 0;
			int32 Index = Start;
			while (Index < Length && IsHexChar(Text[Index]))
			{
				++Digits;
				++Index;
			}
			if (Digits == 32 && (Index >= Length || !IsWordChar(Text[Index])))
			{
				return 32;
			}
			if (Start + 36 <= Length)
			{
				bool bHyphenated = true;
				for (int32 Offset = 0; Offset < 36 && bHyphenated; ++Offset)
				{
					const TCHAR Char = Text[Start + Offset];
					bHyphenated = (Offset == 8 || Offset == 13 || Offset == 18 || Offset == 23) ? Char == TEXT('-') : IsHexChar(Char);
				}
				if (bHyphenated && (Start + 36 >= Length || !IsWordChar(Text[Start + 36])))
				{
					return 36;
				}
			}
			return 0;
		}
	}

	/**
	 * Blanks every GUID in a dump, so two dumps of assets whose IDs differ compare equal; also the hash in the name of
	 * a property bag's transient struct (`PropertyBag_bc61c8006dbace15`), which the engine derives from the bag's layout
	 * and IDs.
	 */
	inline FString ScrubGuids(const FString& Text)
	{
		FString Result;
		Result.Reserve(Text.Len());
		int32 Index = 0;
		static const FString BagPrefix = TEXT("PropertyBag_");
		while (Index < Text.Len())
		{
			if (Text[Index] == TEXT('P') && FCString::Strncmp(*Text + Index, *BagPrefix, BagPrefix.Len()) == 0)
			{
				Result += TEXT("PropertyBag_<hash>");
				Index += BagPrefix.Len();
				while (Index < Text.Len() && Detail::IsHexChar(Text[Index]))
				{
					++Index;
				}
				continue;
			}
			const bool bWordStart = Detail::IsHexChar(Text[Index]) && (Index == 0 || !Detail::IsWordChar(Text[Index - 1]));
			const int32 GuidLength = bWordStart ? Detail::GuidLengthAt(Text, Index) : 0;
			if (GuidLength > 0)
			{
				Result += TEXT("<guid>");
				Index += GuidLength;
			}
			else
			{
				Result.AppendChar(Text[Index]);
				++Index;
			}
		}
		return Result;
	}
}
