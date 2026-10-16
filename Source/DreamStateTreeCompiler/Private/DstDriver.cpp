// Copyright DreamMoon. All Rights Reserved.
//
// One source through the pipeline (parse, includes, templates, lowering, generation), several sources in the order their
// links need, formatting, the decompiler, and diagnostics in the forms tools read.

#include "DstDriver.h"

#include "DstDriverPrivate.h"
#include "DstExtract.h"
#include "DstRaise.h"
#include "DstValues.h"
#include "Lang/DstParser.h"
#include "Lang/DstPrinter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "StateTree.h"
#include "UObject/Package.h"

namespace UE::DreamStateTree::Compiler::DriverPrivate
{
	/** The roots to search: the options' when it names some, the project's and plugins' otherwise; normalised. */
	TArray<FString> GetEffectiveRoots(const TArray<FString>& OptionRoots)
	{
		if (!OptionRoots.IsEmpty())
		{
			return NormalizeDstRoots(OptionRoots);
		}
		const TArray<FString> Defaults = GetDstSourceRoots();
		return NormalizeDstRoots(Defaults);
	}

	/** Adds the file's own diagnostic to Result as an error. */
	void AddFileError(FDstFileResult& Result, const TCHAR* Code, const FString& Message, const FString& Hint = FString())
	{
		Result.Diagnostics.Error(Code, Lang::FDstSpan(), Message, Hint);
	}

	// ------------------------------------------------------------------------------------------
	// Which assets a file links
	// ------------------------------------------------------------------------------------------

	void CollectLinkedAssetNames(const Lang::FDstStateStmt& State, TSet<FString>& OutNames)
	{
		for (const Lang::FDstStmtPtr& Item : State.Body)
		{
			if (!Item)
			{
				continue;
			}
			if (const Lang::FDstStateStmt* Child = Item->As<Lang::FDstStateStmt>())
			{
				CollectLinkedAssetNames(*Child, OutNames);
				continue;
			}
			const Lang::FDstClauseStmt* Clause = Item->As<Lang::FDstClauseStmt>();
			if (!Clause || Clause->ClauseKind != Lang::EDstClauseKind::Link || !Clause->bLinkAsset || !Clause->Value)
			{
				continue;
			}
			FString BarePath;
			const bool bBarePath = TryGetDstAssetPath(*Clause->Value, BarePath);
			const Lang::FDstCallExpr* Call = bBarePath ? nullptr : Clause->Value->As<Lang::FDstCallExpr>();
			const Lang::FDstExpr* Callee = Call && Call->Callee ? Call->Callee.Get() : Clause->Value.Get();

			// `ST_Other` or `Path(Game, "AI/ST_Other")`: the asset's name either way.
			FString ObjectPath;
			if (TryGetDstAssetPath(*Callee, ObjectPath))
			{
				OutNames.Add(FPackageName::ObjectPathToObjectName(ObjectPath).ToLower());
			}
			else
			{
				TArray<FString> Segments;
				if (Lang::TryGetDstPath(*Callee, Segments) && !Segments.IsEmpty())
				{
					OutNames.Add(Segments.Last().ToLower());
				}
			}
		}
	}

	void CollectLinkedAssetNames(const Lang::FDstFile& File, TSet<FString>& OutNames)
	{
		if (!File.Document)
		{
			return;
		}
		for (const Lang::FDstStmtPtr& Member : File.Document->Members)
		{
			if (const Lang::FDstStateStmt* State = Member ? Member->As<Lang::FDstStateStmt>() : nullptr)
			{
				CollectLinkedAssetNames(*State, OutNames);
			}
		}
	}

	/**
	 * The names of the assets a source links with `link asset`, its libraries' subtrees included (a linked subtree is
	 * copied into the tree). Read from the syntax tree, not from lowering: the asset may not exist yet, which is exactly
	 * when its order matters.
	 */
	TSet<FString> ReadLinkedAssetNames(const FString& FilePath, const TArray<FString>& Roots)
	{
		TSet<FString> Names;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
		{
			return Names;
		}
		Lang::FDstCompilationUnit Unit;
		Unit.MainSource = Lang::FDstSourceText(FilePath, MoveTemp(Text));
		Lang::FDstDiagnosticSink Ignored(FilePath);
		Lang::FDstParseOptions ParseOptions;
		ParseOptions.bKeepTrivia = false;
		Unit.Main = Lang::ParseDst(Unit.MainSource, Ignored, ParseOptions);
		Lang::FDstFileSystemIncludeLoader Loader(Roots);
		Lang::ResolveDstIncludes(Unit, Loader, Ignored);

		if (Unit.Main)
		{
			CollectLinkedAssetNames(*Unit.Main, Names);
		}
		for (const TUniquePtr<Lang::FDstFile>& Library : Unit.Libraries)
		{
			if (Library)
			{
				CollectLinkedAssetNames(*Library, Names);
			}
		}
		return Names;
	}

	// ------------------------------------------------------------------------------------------
	// JSON
	// ------------------------------------------------------------------------------------------

	void WriteDiagnosticJson(TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>& Writer, const Lang::FDstDiagnostic& Diagnostic, const FString& DefaultFile)
	{
		Writer.WriteObjectStart();
		Writer.WriteValue(TEXT("file"), Diagnostic.FilePath.IsEmpty() ? DefaultFile : Diagnostic.FilePath);
		Writer.WriteValue(TEXT("line"), static_cast<int32>(Diagnostic.Span.Line));
		Writer.WriteValue(TEXT("column"), static_cast<int32>(Diagnostic.Span.Column));
		Writer.WriteValue(TEXT("length"), static_cast<int32>(FMath::Max(Diagnostic.Span.Length, 0)));
		Writer.WriteValue(TEXT("severity"), FString(Lang::LexToString(Diagnostic.Severity)));
		Writer.WriteValue(TEXT("code"), Diagnostic.Code);
		Writer.WriteValue(TEXT("message"), Diagnostic.Message);
		Writer.WriteValue(TEXT("hint"), Diagnostic.Hint);
		Writer.WriteObjectEnd();
	}
}

namespace UE::DreamStateTree::Compiler
{
	// ------------------------------------------------------------------------------------------
	// One file
	// ------------------------------------------------------------------------------------------

	FDstFileResult BuildDstFile(const FString& FilePath, const FDstBuildOptions& Options)
	{
		const FString Path = DriverPrivate::NormalizeDstPath(FilePath);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			FDstFileResult Result;
			Result.FilePath = Path;
			Result.Diagnostics = Lang::FDstDiagnosticSink(Path);
			DriverPrivate::AddFileError(Result, TEXT("DST3204"), FString::Printf(TEXT("Cannot read %s."), *Path),
				TEXT("Check that the file exists and is not locked by another program."));
			return Result;
		}
		return BuildDstText(Path, Text, Options);
	}

	FDstFileResult BuildDstText(const FString& FilePath, const FString& Text, const FDstBuildOptions& Options)
	{
		FDstFileResult Result;
		Result.FilePath = DriverPrivate::NormalizeDstPath(FilePath);
		Result.Diagnostics = Lang::FDstDiagnosticSink(Result.FilePath);
		// On the heap from the start: write-back keeps pointers into these trees, and they must not move.
		Result.Unit = MakeShared<Lang::FDstCompilationUnit>();
		Lang::FDstCompilationUnit& Unit = *Result.Unit;
		Unit.MainSource = Lang::FDstSourceText(Result.FilePath, Text);
		Lang::FDstDiagnosticSink& Sink = Result.Diagnostics;

		const TArray<FString> Roots = DriverPrivate::GetEffectiveRoots(Options.Roots);

		Unit.Main = Lang::ParseDst(Unit.MainSource, Sink);
		{
			Lang::FDstFileSystemIncludeLoader Loader(Roots);
			Lang::ResolveDstIncludes(Unit, Loader, Sink);
		}
		Lang::ExpandDstTemplates(Unit, Sink);
		if (Sink.HasErrors())
		{
			// What the semantic stage would say about a file that does not parse is mostly echoes of the parse errors.
			return Result;
		}

		// A library is checked on its own as far as a library can be: what it may hold.
		const bool bLibrary = Unit.Main && Unit.Main->Document && Unit.Main->Document->DocumentKind == Lang::EDstDocumentKind::StateTreeLibrary;

		Result.Lowered.Emplace();
		FDstLowerResult& Lowered = Result.Lowered.GetValue();
		LowerDst(Unit, Lowered, Sink);
		if (bLibrary)
		{
			return Result;
		}
		if (!Options.OverrideAssetPath.IsEmpty())
		{
			Lowered.Tree.AssetPath = DriverPrivate::ToDstObjectPath(Options.OverrideAssetPath);
		}

		if (Options.bGenerate && !Sink.HasErrors())
		{
			FDstGenerateOptions GenerateOptions;
			GenerateOptions.bSave = Options.bSave;
			GenerateOptions.SourceText = Text;
			GenerateOptions.SourcePath = DriverPrivate::MakeDstSourceRelativePath(Result.FilePath, Roots);

			Result.Generated.Emplace();
			GenerateDst(Lowered.Tree, Lowered.Places, GenerateOptions, Result.Generated.GetValue(), Sink);
		}
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// Several files
	// ------------------------------------------------------------------------------------------

	TArray<FDstFileResult> BuildDstFiles(TConstArrayView<FString> FilePaths, const FDstBuildOptions& Options)
	{
		const TArray<FString> Roots = DriverPrivate::GetEffectiveRoots(Options.Roots);

		// A library stands for every tree that includes it (and is checked on its own when none does).
		FDstSourceIndex Index;
		Index.Scan(Roots);
		TArray<FString> Trees;
		TArray<FString> LonelyLibraries;
		for (const FString& FilePath : FilePaths)
		{
			const FString Path = DriverPrivate::NormalizeDstPath(FilePath);
			if (Lang::GetDstFileKindFromPath(Path) == Lang::EDstFileKind::Library)
			{
				const TArray<FString> Dependents = Index.FindDependents(Path);
				for (const FString& Dependent : Dependents)
				{
					Trees.AddUnique(Dependent);
				}
				if (Dependents.IsEmpty())
				{
					LonelyLibraries.AddUnique(Path);
				}
			}
			else
			{
				Trees.AddUnique(Path);
			}
		}
		Trees.Sort();
		LonelyLibraries.Sort();

		TArray<FDstFileResult> Results;

		if (!Options.bGenerate)
		{
			for (const FString& Tree : Trees)
			{
				Results.Add(BuildDstFile(Tree, Options));
			}
			for (const FString& Library : LonelyLibraries)
			{
				Results.Add(BuildDstFile(Library, Options));
			}
			return Results;
		}

		// Who links whom: a tree is built after the trees whose assets it links.
		struct FNode
		{
			FString File;
			FString AssetName;
			TSet<FString> Linked;
			TArray<int32> Dependencies;
		};
		TArray<FNode> Nodes;
		Nodes.SetNum(Trees.Num());
		for (int32 NodeIndex = 0; NodeIndex < Trees.Num(); ++NodeIndex)
		{
			FNode& Node = Nodes[NodeIndex];
			Node.File = Trees[NodeIndex];
			FDstSourceEntry Entry;
			if (DriverPrivate::ReadDstSourceEntry(Node.File, Roots, Entry) && !Entry.AssetPath.IsEmpty())
			{
				Node.AssetName = FPackageName::ObjectPathToObjectName(Entry.AssetPath).ToLower();
			}
			Node.Linked = DriverPrivate::ReadLinkedAssetNames(Node.File, Roots);
		}
		for (int32 From = 0; From < Nodes.Num(); ++From)
		{
			for (int32 To = 0; To < Nodes.Num(); ++To)
			{
				if (From != To && !Nodes[To].AssetName.IsEmpty() && Nodes[From].Linked.Contains(Nodes[To].AssetName))
				{
					Nodes[From].Dependencies.Add(To);
				}
			}
		}

		// Strongly connected components come out dependencies first (Tarjan): that is the build order, and a component of
		// more than one tree is a cycle of links.
		TArray<int32> Number;
		TArray<int32> Low;
		TArray<bool> OnStack;
		Number.Init(INDEX_NONE, Nodes.Num());
		Low.Init(0, Nodes.Num());
		OnStack.Init(false, Nodes.Num());
		TArray<int32> Stack;
		TArray<TArray<int32>> Components;
		int32 Counter = 0;
		TFunction<void(int32)> Connect = [&](const int32 Vertex)
		{
			Number[Vertex] = Low[Vertex] = Counter++;
			Stack.Push(Vertex);
			OnStack[Vertex] = true;
			for (const int32 Other : Nodes[Vertex].Dependencies)
			{
				if (Number[Other] == INDEX_NONE)
				{
					Connect(Other);
					Low[Vertex] = FMath::Min(Low[Vertex], Low[Other]);
				}
				else if (OnStack[Other])
				{
					Low[Vertex] = FMath::Min(Low[Vertex], Number[Other]);
				}
			}
			if (Low[Vertex] == Number[Vertex])
			{
				TArray<int32>& Component = Components.AddDefaulted_GetRef();
				int32 Member = INDEX_NONE;
				do
				{
					Member = Stack.Pop(EAllowShrinking::No);
					OnStack[Member] = false;
					Component.Add(Member);
				} while (Member != Vertex);
			}
		};
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			if (Number[NodeIndex] == INDEX_NONE)
			{
				Connect(NodeIndex);
			}
		}

		FDstBuildOptions CheckOnly = Options;
		CheckOnly.bGenerate = false;
		for (const TArray<int32>& Component : Components)
		{
			if (Component.Num() == 1)
			{
				Results.Add(BuildDstFile(Nodes[Component[0]].File, Options));
				continue;
			}

			// Every file on the cycle says so; none of them is built.
			TArray<FString> Names;
			for (const int32 Member : Component)
			{
				Names.Add(FPaths::GetCleanFilename(Nodes[Member].File));
			}
			Names.Sort();
			for (const int32 Member : Component)
			{
				FDstFileResult Result = BuildDstFile(Nodes[Member].File, CheckOnly);
				DriverPrivate::AddFileError(Result, TEXT("DST6009"),
					FString::Printf(TEXT("These trees link each other in a circle, so none of them can be built first: %s."), *FString::Join(Names, TEXT(", "))),
					TEXT("Break the circle: link a subtree of the same tree instead of the other asset."));
				Results.Add(MoveTemp(Result));
			}
		}
		for (const FString& Library : LonelyLibraries)
		{
			Results.Add(BuildDstFile(Library, Options));
		}
		return Results;
	}

	// ------------------------------------------------------------------------------------------
	// Formatting
	// ------------------------------------------------------------------------------------------

	bool FormatDstText(const FString& FilePath, const FString& Text, FString& OutText, Lang::FDstDiagnosticSink& Sink)
	{
		const FString Path = DriverPrivate::NormalizeDstPath(FilePath);
		const Lang::FDstSourceText Source(Path, Text);
		Lang::FDstDiagnosticSink Local(Path);
		const TUniquePtr<Lang::FDstFile> File = Lang::ParseDst(Source, Local);
		const bool bFailed = Local.HasErrors() || !File;
		Sink.Append(MoveTemp(Local));

		// A tree with errors in it prints without the statements that failed: the canonical text of a broken file would
		// silently drop what the author wrote.
		if (bFailed)
		{
			OutText.Reset();
			return false;
		}
		Lang::FDstPrintOptions PrintOptions;
		PrintOptions.KeepBrokenIn = &Source.GetText();
		OutText = Lang::PrintDst(*File, PrintOptions);
		return true;
	}

	bool FormatDstFile(const FString& FilePath, FString& OutText, Lang::FDstDiagnosticSink& Sink)
	{
		const FString Path = DriverPrivate::NormalizeDstPath(FilePath);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			Lang::FDstDiagnosticSink Local(Path);
			Local.Error(TEXT("DST3204"), Lang::FDstSpan(), FString::Printf(TEXT("Cannot read %s."), *Path),
				TEXT("Check that the file exists and is not locked by another program."));
			Sink.Append(MoveTemp(Local));
			OutText.Reset();
			return false;
		}
		return FormatDstText(Path, Text, OutText, Sink);
	}

	// ------------------------------------------------------------------------------------------
	// Decompiling
	// ------------------------------------------------------------------------------------------

	FString GetDefaultDstDecompilePath(const UStateTree& Asset)
	{
		// `/Game/AI/ST_Brawler` -> `AI/ST_Brawler`; any other mount keeps its name: `/MyPlugin/AI/ST` -> `MyPlugin/AI/ST`.
		FString Package = Asset.GetPackage()->GetName();
		Package.RemoveFromStart(TEXT("/"));
		FString Mount;
		FString Relative;
		if (!Package.Split(TEXT("/"), &Mount, &Relative))
		{
			Mount = TEXT("Game");
			Relative = Package;
		}
		if (!Mount.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
		{
			Relative = Mount + TEXT("/") + Relative;
		}
		return DriverPrivate::NormalizeDstPath(FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT("DTree"), Relative + TEXT(".dtr")));
	}

	bool DecompileDstAsset(const UStateTree& Asset, const FDstDecompileOptions& Options, FString& OutText, Lang::FDstDiagnosticSink& Sink)
	{
		OutText.Reset();

		// Extraction problems are about the asset, not a file: they name it by its object path.
		Lang::FDstDiagnosticSink Local(Asset.GetPathName());
		bool bSucceeded = false;

		Model::FDstTree Tree;
		if (ExtractDst(Asset, Tree, Local))
		{
			// RaiseDst names the nodes something binds to (EnsureDstBindNames) on its own copy of the model.
			FDstRaiseOptions RaiseOptions;
			RaiseOptions.FilePath = Options.FilePath.IsEmpty() ? GetDefaultDstDecompilePath(Asset) : DriverPrivate::NormalizeDstPath(Options.FilePath);
			RaiseOptions.TagPrefixes = Options.TagPrefixes;

			if (const TUniquePtr<Lang::FDstFile> File = RaiseDst(Tree, RaiseOptions, Local))
			{
				OutText = Lang::PrintDst(*File);
				bSucceeded = !Local.HasErrors();
			}
		}
		Sink.Append(MoveTemp(Local));
		return bSucceeded;
	}

	// ------------------------------------------------------------------------------------------
	// Diagnostics
	// ------------------------------------------------------------------------------------------

	FString FormatDstDiagnostics(const Lang::FDstDiagnosticSink& Sink)
	{
		TArray<FString> Lines;
		Lines.Reserve(Sink.Num());
		for (const Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			const FString& File = Diagnostic.FilePath.IsEmpty() ? Sink.GetFilePath() : Diagnostic.FilePath;
			Lines.Add(FString::Printf(TEXT("%s(%d,%d): %s %s: %s"), *File, Diagnostic.Span.Line, Diagnostic.Span.Column,
				Lang::LexToString(Diagnostic.Severity), *Diagnostic.Code, *Diagnostic.Message));
		}
		return FString::Join(Lines, TEXT("\n"));
	}

	FString FormatDstDiagnosticsJson(TConstArrayView<const Lang::FDstDiagnosticSink*> Sinks)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		Writer->WriteArrayStart();
		for (const Lang::FDstDiagnosticSink* Sink : Sinks)
		{
			if (!Sink)
			{
				continue;
			}
			for (const Lang::FDstDiagnostic& Diagnostic : Sink->GetDiagnostics())
			{
				DriverPrivate::WriteDiagnosticJson(*Writer, Diagnostic, Sink->GetFilePath());
			}
		}
		Writer->WriteArrayEnd();
		Writer->Close();
		return Json;
	}
}
