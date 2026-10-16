// Copyright DreamMoon. All Rights Reserved.

#include "Commandlet/DstCommandlet.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DstDriver.h"
#include "HAL/FileManager.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstSource.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "StateTree.h"
#include "UObject/Package.h"

#include <stdio.h>

DEFINE_LOG_CATEGORY_STATIC(LogDreamStateTreeCommandlet, Log, All);

namespace UE::DreamStateTree::Compiler::CommandletPrivate
{
	enum class EDstCommandletMode : uint8
	{
		Build,
		Check,
		Format,
		Decompile,
	};

	/** One run: what was asked, and what it found. */
	struct FDstCommandletRun
	{
		EDstCommandletMode Mode = EDstCommandletMode::Build;
		bool bNoSave = false;
		bool bWrite = false;
		bool bVerify = false;
		bool bStdout = false;
		bool bForce = false;
		FString File;
		TArray<FString> Roots;
		TArray<FString> Assets;
		FString Under;
		FString Out;
		FString Json;
		TArray<FString> TagPrefixes;

		/** Every diagnostic reported, for -Json. */
		Lang::FDstDiagnosticSink Collected;
		/** The lines already reported: a library with a mistake is read once for every tree that includes it. */
		TSet<FString> Reported;
		int32 NumErrors = 0;
		int32 NumWarnings = 0;

		/** Logs a diagnostic in the form tools read, keeps it for -Json and counts it. FallbackFile names a diagnostic that carries no file of its own. */
		void Report(const Lang::FDstDiagnostic& Diagnostic, const FString& FallbackFile)
		{
			const FString& DiagnosticFile = Diagnostic.FilePath.IsEmpty() ? FallbackFile : Diagnostic.FilePath;
			const FString Line = FString::Printf(TEXT("%s(%d,%d): %s %s: %s"), *DiagnosticFile, Diagnostic.Span.Line, Diagnostic.Span.Column,
				Lang::LexToString(Diagnostic.Severity), *Diagnostic.Code, *Diagnostic.Message);
			bool bAlreadyReported = false;
			Reported.Add(Line, &bAlreadyReported);
			if (bAlreadyReported)
			{
				return;
			}

			Lang::FDstDiagnosticSink Kept(DiagnosticFile);
			switch (Diagnostic.Severity)
			{
			case Lang::EDstSeverity::Error:
				UE_LOG(LogDreamStateTreeCommandlet, Error, TEXT("%s"), *Line);
				++NumErrors;
				Kept.Error(*Diagnostic.Code, Diagnostic.Span, Diagnostic.Message, Diagnostic.Hint);
				break;
			case Lang::EDstSeverity::Warning:
				UE_LOG(LogDreamStateTreeCommandlet, Warning, TEXT("%s"), *Line);
				++NumWarnings;
				Kept.Warning(*Diagnostic.Code, Diagnostic.Span, Diagnostic.Message, Diagnostic.Hint);
				break;
			default:
				UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("%s"), *Line);
				Kept.Info(*Diagnostic.Code, Diagnostic.Span, Diagnostic.Message, Diagnostic.Hint);
				break;
			}
			if (!Diagnostic.Hint.IsEmpty())
			{
				UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("    hint: %s"), *Diagnostic.Hint);
			}
			Collected.Append(MoveTemp(Kept));
		}

		void Report(const Lang::FDstDiagnosticSink& Sink)
		{
			for (const Lang::FDstDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				Report(Diagnostic, Sink.GetFilePath());
			}
		}

		/** A problem of the run itself (a file that cannot be read or written, a bad option): not a diagnostic of any text, but an error all the same. */
		void Fail(const FString& Message)
		{
			UE_LOG(LogDreamStateTreeCommandlet, Error, TEXT("%s"), *Message);
			++NumErrors;
		}
	};

	// ---------------------------------------------------------------------------------------------
	// Paths
	// ---------------------------------------------------------------------------------------------

	/** Absolute, forward slashes. A relative path means the project directory, not the engine's binaries folder. */
	FString NormalizeDstPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return FString();
		}
		FString Result = FPaths::IsRelative(Path)
			? FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Path)
			: FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Result);
		FPaths::CollapseRelativeDirectories(Result);
		// A drive root keeps its slash: `C:` alone means the drive's current directory.
		while (Result.Len() > 3 && Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1);
		}
		return Result;
	}

	/** `/Game/AI/ST_Brawler` and `/Game/AI/ST_Brawler.ST_Brawler` and an export-text path all name the same object. */
	FString ToDstObjectPath(const FString& AssetPath)
	{
		FString Path = AssetPath.TrimStartAndEnd();
		if (Path.Contains(TEXT("'")))
		{
			Path = FPackageName::ExportTextPathToObjectPath(Path);
		}
		int32 Slash = INDEX_NONE;
		Path.FindLastChar(TEXT('/'), Slash);
		const bool bHasObjectName = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) > Slash;
		if (!bHasObjectName && Slash != INDEX_NONE && Slash + 1 < Path.Len())
		{
			Path += TEXT(".") + Path.Mid(Slash + 1);
		}
		return Path;
	}

	/** Where a package's text goes below a folder: the package path without the Game mount (a plugin keeps its name), as GetDefaultDstDecompilePath does below DTree/. */
	FString GetDstDecompileRelativePath(const FString& PackageName)
	{
		FString Relative = PackageName;
		Relative.RemoveFromStart(TEXT("/"));
		Relative.RemoveFromStart(TEXT("Game/"), ESearchCase::CaseSensitive);
		return Relative;
	}

	// ---------------------------------------------------------------------------------------------
	// Parameters
	// ---------------------------------------------------------------------------------------------

	/** Splits `a,b;c` into its non-empty, trimmed parts. */
	void AppendDstListValue(const FString& Value, TArray<FString>& Out)
	{
		TArray<FString> Parts;
		Value.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd().TrimQuotes();
			if (!Trimmed.IsEmpty())
			{
				Out.AddUnique(Trimmed);
			}
		}
	}

	/** False, with the reason reported, when the options contradict each other. */
	bool ParseDstCommandletParams(const FString& Params, FDstCommandletRun& Run)
	{
		// The switches one by one rather than ParseCommandLine's map: -Asset may be given several times.
		TArray<FString> Tokens;
		TArray<FString> Switches;
		UCommandlet::ParseCommandLine(*Params, Tokens, Switches);

		int32 NumModes = 0;
		bool bRootsGiven = false;
		TArray<FString> GivenRoots;
		for (const FString& Switch : Switches)
		{
			FString Key = Switch;
			FString Value;
			if (Switch.Split(TEXT("="), &Key, &Value))
			{
				Value = Value.TrimQuotes();
				if (Key == TEXT("File"))
				{
					Run.File = Value;
				}
				else if (Key == TEXT("Root"))
				{
					bRootsGiven = true;
					AppendDstListValue(Value, GivenRoots);
				}
				else if (Key == TEXT("Asset"))
				{
					AppendDstListValue(Value, Run.Assets);
				}
				else if (Key == TEXT("Under"))
				{
					Run.Under = Value;
				}
				else if (Key == TEXT("Out"))
				{
					Run.Out = Value;
				}
				else if (Key == TEXT("Json"))
				{
					Run.Json = Value;
				}
				else if (Key == TEXT("TagPrefix"))
				{
					AppendDstListValue(Value, Run.TagPrefixes);
				}
				continue;
			}

			if (Key == TEXT("Build"))
			{
				Run.Mode = EDstCommandletMode::Build;
				++NumModes;
			}
			else if (Key == TEXT("Check"))
			{
				Run.Mode = EDstCommandletMode::Check;
				++NumModes;
			}
			else if (Key == TEXT("Format"))
			{
				Run.Mode = EDstCommandletMode::Format;
				++NumModes;
			}
			else if (Key == TEXT("Decompile"))
			{
				Run.Mode = EDstCommandletMode::Decompile;
				++NumModes;
			}
			else if (Key == TEXT("NoSave"))
			{
				Run.bNoSave = true;
			}
			else if (Key == TEXT("Write"))
			{
				Run.bWrite = true;
			}
			else if (Key == TEXT("Verify"))
			{
				Run.bVerify = true;
			}
			else if (Key == TEXT("Stdout"))
			{
				Run.bStdout = true;
			}
			else if (Key == TEXT("Force"))
			{
				Run.bForce = true;
			}
		}

		if (bRootsGiven)
		{
			for (const FString& Root : GivenRoots)
			{
				Run.Roots.AddUnique(NormalizeDstPath(Root));
			}
		}
		else
		{
			Run.Roots = GetDstSourceRoots();
		}

		if (NumModes > 1)
		{
			Run.Fail(TEXT("Give at most one of -Build, -Check, -Format and -Decompile."));
			return false;
		}
		if (Run.bWrite && Run.bVerify)
		{
			Run.Fail(TEXT("-Write rewrites the files and -Verify only checks them; give one of the two."));
			return false;
		}
		if ((Run.bWrite || Run.bVerify) && Run.Mode != EDstCommandletMode::Format)
		{
			Run.Fail(TEXT("-Write and -Verify go with -Format."));
			return false;
		}
		if (!Run.TagPrefixes.IsEmpty() && Run.Mode != EDstCommandletMode::Decompile)
		{
			Run.Fail(TEXT("-TagPrefix goes with -Decompile."));
			return false;
		}
		if (Run.Mode == EDstCommandletMode::Decompile && Run.Assets.IsEmpty() && Run.Under.IsEmpty())
		{
			Run.Fail(TEXT("-Decompile needs -Asset=<object or package path> or -Under=<content path>."));
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Text on stdout
	// ---------------------------------------------------------------------------------------------

	/**
	 * Straight to stdout rather than through the log: a log line carries a category and maybe a time, and
	 * the log reaches stdout only with the engine's -stdout. The log is flushed first so the text lands
	 * after the lines logged before it.
	 */
	void WriteDstStdout(const FString& Text)
	{
		GLog->Flush();
		const FTCHARToUTF8 Utf8(*Text);
		fwrite(Utf8.Get(), 1, static_cast<size_t>(Utf8.Length()), stdout);
		fflush(stdout);
	}

	void PrintDstTextBlock(const FString& Name, const FString& Text)
	{
		FString Block = FString::Printf(TEXT("===== begin %s =====\n"), *Name);
		Block += Text;
		if (!Text.IsEmpty() && !Text.EndsWith(TEXT("\n")))
		{
			Block += TEXT("\n");
		}
		Block += FString::Printf(TEXT("===== end %s =====\n"), *Name);
		WriteDstStdout(Block);
	}

	// ---------------------------------------------------------------------------------------------
	// Line ends
	// ---------------------------------------------------------------------------------------------

	/** The line end a file uses, by its first one; `\n` for a file of one line. */
	FString DetectDstLineEnd(const FString& Text)
	{
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (Text[Index] == TEXT('\r'))
			{
				return (Index + 1 < Text.Len() && Text[Index + 1] == TEXT('\n')) ? TEXT("\r\n") : TEXT("\r");
			}
			if (Text[Index] == TEXT('\n'))
			{
				return TEXT("\n");
			}
		}
		return TEXT("\n");
	}

	/** Text with every line end made LineEnd (a block comment may carry the file's own). */
	FString WithDstLineEnds(const FString& Text, const FString& LineEnd)
	{
		FString Result = Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
		if (!LineEnd.Equals(TEXT("\n"), ESearchCase::CaseSensitive))
		{
			Result.ReplaceInline(TEXT("\n"), *LineEnd, ESearchCase::CaseSensitive);
		}
		return Result;
	}

	/** The 1-based line where two texts first differ, line ends aside; 1 when only the line ends differ. */
	int32 FindFirstDifferentDstLine(const FString& A, const FString& B)
	{
		TArray<FString> LinesA;
		TArray<FString> LinesB;
		WithDstLineEnds(A, TEXT("\n")).ParseIntoArray(LinesA, TEXT("\n"), false);
		WithDstLineEnds(B, TEXT("\n")).ParseIntoArray(LinesB, TEXT("\n"), false);
		for (int32 Index = 0; Index < FMath::Max(LinesA.Num(), LinesB.Num()); ++Index)
		{
			if (!LinesA.IsValidIndex(Index) || !LinesB.IsValidIndex(Index) || !LinesA[Index].Equals(LinesB[Index], ESearchCase::CaseSensitive))
			{
				return Index + 1;
			}
		}
		return 1;
	}

	// ---------------------------------------------------------------------------------------------
	// Which files
	// ---------------------------------------------------------------------------------------------

	/**
	 * The trees a build or check covers: -File (a library standing for the trees that include it), else every
	 * .dtr the index holds and every .dth no tree includes. False when -File does not name a source.
	 */
	bool GatherDstTrees(FDstCommandletRun& Run, TArray<FString>& OutFiles)
	{
		if (Run.File.IsEmpty())
		{
			FDstSourceIndex Index;
			Index.Scan(Run.Roots);
			for (const FDstSourceEntry& Entry : Index.GetEntries())
			{
				// A library is read through the trees that include it, or on its own when none does.
				if (Lang::GetDstFileKindFromPath(Entry.FilePath) != Lang::EDstFileKind::Library || Index.FindDependents(Entry.FilePath).IsEmpty())
				{
					OutFiles.Add(Entry.FilePath);
				}
			}
			return true;
		}

		const FString Path = NormalizeDstPath(Run.File);
		if (!IsDstSourceFile(Path))
		{
			Run.Fail(FString::Printf(TEXT("%s is not a DreamStateTree source (.dtr or .dth)."), *Path));
			return false;
		}
		if (!IFileManager::Get().FileExists(*Path))
		{
			Run.Fail(FString::Printf(TEXT("%s does not exist."), *Path));
			return false;
		}
		if (Lang::GetDstFileKindFromPath(Path) == Lang::EDstFileKind::Library)
		{
			// A library makes nothing itself; a change to it is a change to every tree that includes it.
			FDstSourceIndex Index;
			Index.Scan(Run.Roots);
			OutFiles = Index.FindDependents(Path);
			if (OutFiles.IsEmpty())
			{
				// Still read on its own, so a mistake in it shows before anything includes it.
				UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("%s is a library no tree under the roots includes; it is checked on its own."), *Path);
				OutFiles.Add(Path);
			}
			else
			{
				UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("%s is a library; %d tree(s) under the roots include it."), *Path, OutFiles.Num());
			}
			return true;
		}
		OutFiles.Add(Path);
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Modes
	// ---------------------------------------------------------------------------------------------

	void RunDstBuildOrCheck(FDstCommandletRun& Run)
	{
		const bool bBuild = Run.Mode == EDstCommandletMode::Build;
		const TCHAR* const ModeName = bBuild ? TEXT("build") : TEXT("check");

		TArray<FString> Files;
		if (!GatherDstTrees(Run, Files))
		{
			UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree %s: nothing done, %d error(s)"), ModeName, Run.NumErrors);
			return;
		}
		if (Files.IsEmpty() && Run.File.IsEmpty())
		{
			UE_LOG(LogDreamStateTreeCommandlet, Warning, TEXT("No .dtr or .dth sources under %s."),
				Run.Roots.IsEmpty() ? TEXT("any DTree/ folder (none in the project or its enabled plugins)") : *FString::Join(Run.Roots, TEXT(", ")));
		}

		FDstBuildOptions Options;
		Options.bGenerate = bBuild;
		Options.bSave = !Run.bNoSave;
		Options.Roots = Run.Roots;

		// The driver builds a tree after the trees it links to, whatever order the files come in.
		const TArray<FDstFileResult> Results = BuildDstFiles(Files, Options);

		int32 NumGenerated = 0;
		for (const FDstFileResult& Result : Results)
		{
			Run.Report(Result.Diagnostics);
			if (bBuild && Result.Succeeded() && Result.Generated.IsSet() && Result.Generated->Asset)
			{
				++NumGenerated;
				UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("Generated %s from %s"), *Result.Generated->Asset->GetPathName(), *Result.FilePath);
			}
		}

		if (bBuild)
		{
			UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree build: %d file(s), %d generated%s, %d error(s), %d warning(s)"),
				Results.Num(), NumGenerated, Run.bNoSave ? TEXT(" (in memory, not saved)") : TEXT(" and saved"), Run.NumErrors, Run.NumWarnings);
		}
		else
		{
			UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree check: %d file(s), %d error(s), %d warning(s)"),
				Results.Num(), Run.NumErrors, Run.NumWarnings);
		}
	}

	void RunDstFormat(FDstCommandletRun& Run)
	{
		TArray<FString> Files;
		if (!Run.File.IsEmpty())
		{
			const FString Path = NormalizeDstPath(Run.File);
			if (!IsDstSourceFile(Path))
			{
				Run.Fail(FString::Printf(TEXT("%s is not a DreamStateTree source (.dtr or .dth)."), *Path));
			}
			else
			{
				Files.Add(Path);
			}
		}
		else
		{
			FDstSourceIndex Index;
			Index.Scan(Run.Roots);
			for (const FDstSourceEntry& Entry : Index.GetEntries())
			{
				Files.Add(Entry.FilePath);
			}
		}

		int32 NumRewritten = 0;
		int32 NumNotCanonical = 0;
		for (const FString& File : Files)
		{
			FString Original;
			if (!FFileHelper::LoadFileToString(Original, *File))
			{
				Run.Fail(FString::Printf(TEXT("Cannot read %s."), *File));
				continue;
			}
			FString Canonical;
			Lang::FDstDiagnosticSink Sink(File);
			const bool bFormatted = FormatDstText(File, Original, Canonical, Sink);
			Run.Report(Sink);
			if (!bFormatted)
			{
				continue;
			}

			// The file's own line ends: a reformat should not also turn every line of a CRLF checkout into a change.
			const FString InFileStyle = WithDstLineEnds(Canonical, DetectDstLineEnd(Original));
			const bool bDiffers = !InFileStyle.Equals(Original, ESearchCase::CaseSensitive);
			if (Run.bWrite)
			{
				if (!bDiffers)
				{
					continue;
				}
				if (FFileHelper::SaveStringToFile(InFileStyle, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					++NumRewritten;
					UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("Formatted %s"), *File);
				}
				else
				{
					Run.Fail(FString::Printf(TEXT("Cannot write %s."), *File));
				}
			}
			else if (Run.bVerify)
			{
				if (bDiffers)
				{
					++NumNotCanonical;
					Run.Fail(FString::Printf(TEXT("%s(%d,1): error: not in canonical format (dst fmt -Write rewrites it)."),
						*File, FindFirstDifferentDstLine(Original, InFileStyle)));
				}
			}
			else
			{
				PrintDstTextBlock(File, WithDstLineEnds(Canonical, TEXT("\n")));
			}
		}

		if (Run.bWrite)
		{
			UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree format: %d file(s), %d rewritten, %d error(s), %d warning(s)"),
				Files.Num(), NumRewritten, Run.NumErrors, Run.NumWarnings);
		}
		else if (Run.bVerify)
		{
			UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree format: %d file(s), %d not canonical, %d error(s), %d warning(s)"),
				Files.Num(), NumNotCanonical, Run.NumErrors, Run.NumWarnings);
		}
		else
		{
			UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree format: %d file(s), %d error(s), %d warning(s)"),
				Files.Num(), Run.NumErrors, Run.NumWarnings);
		}
	}

	void RunDstDecompile(FDstCommandletRun& Run)
	{
		TArray<FString> ObjectPaths;
		for (const FString& Asset : Run.Assets)
		{
			ObjectPaths.AddUnique(ToDstObjectPath(Asset));
		}
		if (!Run.Under.IsEmpty())
		{
			FString Under = Run.Under;
			while (Under.Len() > 1 && Under.EndsWith(TEXT("/")))
			{
				Under.LeftChopInline(1);
			}
			// The registry dumps a callstack for a path outside every mount; say what is wrong instead.
			if (!FPackageName::IsValidPath(Under + TEXT("/")))
			{
				Run.Fail(FString::Printf(TEXT("%s is not a content path: it starts with a mount such as /Game/ or /StateTree/."), *Under));
				UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree decompile: nothing done, %d error(s)"), Run.NumErrors);
				return;
			}
			IAssetRegistry& Registry = IAssetRegistry::GetChecked();
			Registry.ScanPathsSynchronous({ Under });
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*Under));
			Filter.bRecursivePaths = true;
			Filter.ClassPaths.Add(UStateTree::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			TArray<FAssetData> Found;
			Registry.GetAssets(Filter, Found);
			Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
			if (Found.IsEmpty())
			{
				Run.Fail(FString::Printf(TEXT("Nothing to decompile under %s: no StateTree asset."), *Under));
			}
			for (const FAssetData& Asset : Found)
			{
				ObjectPaths.AddUnique(Asset.GetObjectPathString());
			}
		}

		const FString OutPath = Run.Out.IsEmpty() ? FString() : NormalizeDstPath(Run.Out);
		const bool bOutIsFile = !OutPath.IsEmpty() && IsDstSourceFile(OutPath);
		if (bOutIsFile && Lang::GetDstFileKindFromPath(OutPath) != Lang::EDstFileKind::StateTree)
		{
			Run.Fail(FString::Printf(TEXT("-Out names %s, a library; a decompiled tree is a .dtr."), *OutPath));
			ObjectPaths.Reset();
		}
		else if (bOutIsFile && ObjectPaths.Num() > 1 && !Run.bStdout)
		{
			Run.Fail(FString::Printf(TEXT("-Out names one file, but %d assets are being decompiled; give a folder."), ObjectPaths.Num()));
			ObjectPaths.Reset();
		}

		int32 NumDone = 0;
		int32 NumUnchanged = 0;
		for (const FString& ObjectPath : ObjectPaths)
		{
			const UStateTree* Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
			if (!Tree)
			{
				Run.Fail(FString::Printf(TEXT("Cannot load a StateTree asset at %s."), *ObjectPath));
				continue;
			}

			// Where the text will go decides the name in its header, so it is settled before the text is made.
			FString Target;
			if (!Run.bStdout)
			{
				Target = bOutIsFile ? OutPath
					: OutPath.IsEmpty() ? GetDefaultDstDecompilePath(*Tree)
					: NormalizeDstPath(FPaths::Combine(OutPath, GetDstDecompileRelativePath(Tree->GetPackage()->GetName()) + TEXT(".dtr")));
			}

			FDstDecompileOptions Options;
			Options.FilePath = Target;
			Options.TagPrefixes = Run.TagPrefixes;
			// The asset is the "file" of its own diagnostics.
			Lang::FDstDiagnosticSink Sink(ObjectPath);
			FString Decompiled;
			const int32 ErrorsBefore = Run.NumErrors;
			const bool bDecompiled = DecompileDstAsset(*Tree, Options, Decompiled, Sink);
			Run.Report(Sink);
			if (!bDecompiled || Sink.HasErrors() || Decompiled.IsEmpty())
			{
				if (Run.NumErrors == ErrorsBefore)
				{
					Run.Fail(FString::Printf(TEXT("%s could not be decompiled."), *ObjectPath));
				}
				continue;
			}

			if (Run.bStdout)
			{
				PrintDstTextBlock(Tree->GetPathName(), Decompiled);
				++NumDone;
				continue;
			}

			FString Text = Decompiled;
			FString Existing;
			bool bUnchanged = false;
			if (IFileManager::Get().FileExists(*Target) && FFileHelper::LoadFileToString(Existing, *Target))
			{
				Text = WithDstLineEnds(Text, DetectDstLineEnd(Existing));
				if (Text.Equals(Existing, ESearchCase::CaseSensitive))
				{
					bUnchanged = true;
				}
				// The default place is DTree/, where the sources live: never replace one without being told to.
				if (!bUnchanged && !Run.bForce)
				{
					Run.Fail(FString::Printf(TEXT("%s exists and differs from %s decompiled; -Force overwrites it (it may be a source written by hand)."),
						*Target, *ObjectPath));
					continue;
				}
			}
			if (!bUnchanged && !FFileHelper::SaveStringToFile(Text, *Target, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Run.Fail(FString::Printf(TEXT("Cannot write %s."), *Target));
				continue;
			}
			if (bUnchanged)
			{
				++NumUnchanged;
				UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("Unchanged %s"), *Target);
				continue;
			}
			++NumDone;
			UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("Wrote %s"), *Target);
		}

		UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree decompile: %d asset(s), %d %s, %d unchanged, %d error(s), %d warning(s)"),
			ObjectPaths.Num(), NumDone, Run.bStdout ? TEXT("printed") : TEXT("written"), NumUnchanged, Run.NumErrors, Run.NumWarnings);
	}
}

UDreamStateTreeCommandlet::UDreamStateTreeCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	// The diagnostics are already logged once each, and the exit code says whether the sources were fine,
	// not whether the engine happened to log an error of its own along the way.
	ShowErrorCount = false;
	UseCommandletResultAsExitCode = true;
}

int32 UDreamStateTreeCommandlet::Main(const FString& Params)
{
	using namespace UE::DreamStateTree::Compiler;
	using namespace UE::DreamStateTree::Compiler::CommandletPrivate;

	FDstCommandletRun Run;
	if (!ParseDstCommandletParams(Params, Run))
	{
		UE_LOG(LogDreamStateTreeCommandlet, Display, TEXT("DreamStateTree: nothing done, %d error(s)"), Run.NumErrors);
		return 1;
	}

	// A commandlet's asset registry starts empty; the assets a tree names (Blueprint nodes, linked trees, enums)
	// are looked up in it. The editor scans on its own.
	if (Run.Mode != EDstCommandletMode::Format && IsRunningCommandlet())
	{
		IAssetRegistry::GetChecked().SearchAllAssets(true);
	}

	switch (Run.Mode)
	{
	case EDstCommandletMode::Build:
	case EDstCommandletMode::Check:
		RunDstBuildOrCheck(Run);
		break;
	case EDstCommandletMode::Format:
		RunDstFormat(Run);
		break;
	case EDstCommandletMode::Decompile:
		RunDstDecompile(Run);
		break;
	}

	if (!Run.Json.IsEmpty())
	{
		const FString JsonPath = NormalizeDstPath(Run.Json);
		TArray<const UE::DreamStateTree::Lang::FDstDiagnosticSink*> Sinks;
		Sinks.Add(&Run.Collected);
		if (!FFileHelper::SaveStringToFile(FormatDstDiagnosticsJson(Sinks), *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Run.Fail(FString::Printf(TEXT("Cannot write %s."), *JsonPath));
		}
	}
	return Run.NumErrors > 0 ? 1 : 0;
}
