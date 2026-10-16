// Copyright DreamMoon. All Rights Reserved.
//
// Where sources live and which file makes which asset: the source roots, the path spellings every part of the driver
// compares, and the source index.

#include "DstDriver.h"

#include "DstDriverPrivate.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lang/DstParser.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace UE::DreamStateTree::Compiler::DriverPrivate
{
	FString NormalizeDstPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return FString();
		}
		// FPaths::ConvertRelativePathToFull alone would resolve against the engine's binaries folder; a path a user writes
		// on the command line or in a setting means the project.
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

	TArray<FString> NormalizeDstRoots(TConstArrayView<FString> Roots)
	{
		TArray<FString> Result;
		for (const FString& Root : Roots)
		{
			const FString Normalized = NormalizeDstPath(Root);
			if (!Normalized.IsEmpty())
			{
				Result.AddUnique(Normalized);
			}
		}
		return Result;
	}

	FString MakeDstDocumentAssetPath(const FString& Name, const FString& Root)
	{
		FString Mount = TEXT("Game");
		if (!Root.IsEmpty())
		{
			if (Root == TEXT("Game") || Root == TEXT("Engine"))
			{
				Mount = Root;
			}
			else if (Root.StartsWith(TEXT("Plugin."), ESearchCase::CaseSensitive) && Root.Len() > 7)
			{
				Mount = Root.RightChop(7);
			}
			else
			{
				return FString();
			}
		}
		FString Relative = Name;
		Relative.TrimStartAndEndInline();
		Relative.RemoveFromStart(TEXT("/"));
		Relative.RemoveFromEnd(TEXT("/"));
		if (Relative.IsEmpty())
		{
			return FString();
		}
		int32 Slash = INDEX_NONE;
		Relative.FindLastChar(TEXT('/'), Slash);
		const FString AssetName = Relative.Mid(Slash + 1);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("/%s/%s.%s"), *Mount, *Relative, *AssetName);
	}

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

	bool ResolveDstIncludePath(const FString& FromFile, const FString& IncludePath, TConstArrayView<FString> Roots, FString& OutPath)
	{
		// The candidates and their order are FDstFileSystemIncludeLoader's: the index must name the file the build will read.
		TArray<FString> Candidates;
		if (!FPaths::IsRelative(IncludePath))
		{
			Candidates.Add(IncludePath);
		}
		else
		{
			Candidates.Add(FPaths::Combine(FPaths::GetPath(FromFile), IncludePath));
			for (const FString& Root : Roots)
			{
				Candidates.Add(FPaths::Combine(Root, IncludePath));
			}
		}
		for (const FString& Candidate : Candidates)
		{
			const FString Normalized = NormalizeDstPath(Candidate);
			if (IFileManager::Get().FileExists(*Normalized))
			{
				OutPath = Normalized;
				return true;
			}
		}
		OutPath = NormalizeDstPath(Candidates[0]);
		return false;
	}

	bool ReadDstSourceEntry(const FString& FilePath, TConstArrayView<FString> Roots, FDstSourceEntry& OutEntry)
	{
		OutEntry = FDstSourceEntry();
		OutEntry.FilePath = NormalizeDstPath(FilePath);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *OutEntry.FilePath))
		{
			return false;
		}

		const Lang::FDstSourceText Source(OutEntry.FilePath, MoveTemp(Text));
		Lang::FDstDiagnosticSink Sink(OutEntry.FilePath);
		Lang::FDstParseOptions ParseOptions;
		ParseOptions.bKeepTrivia = false;
		const TUniquePtr<Lang::FDstFile> File = Lang::ParseDst(Source, Sink, ParseOptions);

		// The document says what the file is; the extension answers for a file whose document did not parse.
		OutEntry.Kind = Lang::GetDstFileKindFromPath(OutEntry.FilePath);
		const Lang::FDstDocument* Document = File->Document.Get();
		if (Document)
		{
			if (Document->DocumentKind == Lang::EDstDocumentKind::StateTree)
			{
				OutEntry.Kind = Lang::EDstFileKind::StateTree;
				OutEntry.AssetPath = MakeDstDocumentAssetPath(Document->GetHeaderString(TEXT("Name")), Document->GetHeaderString(TEXT("Root")));
			}
			else if (Document->DocumentKind == Lang::EDstDocumentKind::StateTreeLibrary)
			{
				OutEntry.Kind = Lang::EDstFileKind::Library;
			}
		}
		for (const TUniquePtr<Lang::FDstInclude>& Include : File->Includes)
		{
			if (Include && !Include->Path.IsEmpty())
			{
				FString Resolved;
				ResolveDstIncludePath(OutEntry.FilePath, Include->Path, Roots, Resolved);
				OutEntry.Includes.AddUnique(Resolved);
			}
		}
		return true;
	}

	FString MakeDstSourceRelativePath(const FString& FilePath, TConstArrayView<FString> Roots)
	{
		const FString Path = NormalizeDstPath(FilePath);
		for (const FString& Root : Roots)
		{
			const FString Prefix = Root + TEXT("/");
			if (Path.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				return Path.RightChop(Prefix.Len());
			}
		}
		return Path;
	}
}

namespace UE::DreamStateTree::Compiler
{
	TArray<FString> GetDstSourceRoots()
	{
		TArray<FString> Roots;
		const auto AddRoot = [&Roots](const FString& BaseDir)
		{
			const FString Root = DriverPrivate::NormalizeDstPath(FPaths::Combine(FPaths::ConvertRelativePathToFull(BaseDir), TEXT("DTree")));
			if (IFileManager::Get().DirectoryExists(*Root))
			{
				Roots.AddUnique(Root);
			}
		};

		AddRoot(FPaths::ProjectDir());
		// By name, so the order includes are tried in does not depend on how the plugins were discovered.
		TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
		Plugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B) { return A->GetName() < B->GetName(); });
		for (const TSharedRef<IPlugin>& Plugin : Plugins)
		{
			AddRoot(Plugin->GetBaseDir());
		}
		return Roots;
	}

	bool IsDstSourceFile(const FString& FilePath)
	{
		return Lang::GetDstFileKindFromPath(FilePath) != Lang::EDstFileKind::Unknown;
	}

	TArray<FString> FindDstSources(TConstArrayView<FString> Roots)
	{
		TArray<FString> DefaultRoots;
		if (Roots.IsEmpty())
		{
			DefaultRoots = GetDstSourceRoots();
		}
		const TArray<FString> SearchRoots = DriverPrivate::NormalizeDstRoots(Roots.IsEmpty() ? TConstArrayView<FString>(DefaultRoots) : Roots);

		TSet<FString> Seen;
		TArray<FString> Files;
		for (const FString& Root : SearchRoots)
		{
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Root, TEXT("*"), /*Files*/ true, /*Directories*/ false);
			for (const FString& File : Found)
			{
				if (!IsDstSourceFile(File))
				{
					continue;
				}
				FString Normalized = DriverPrivate::NormalizeDstPath(File);
				bool bAlreadySeen = false;
				Seen.Add(Normalized, &bAlreadySeen);
				if (!bAlreadySeen)
				{
					Files.Add(MoveTemp(Normalized));
				}
			}
		}
		Files.Sort();
		return Files;
	}

	void FDstSourceIndex::Scan(TConstArrayView<FString> InRoots)
	{
		Roots = DriverPrivate::NormalizeDstRoots(InRoots);
		Entries.Reset();

		const TArray<FString> Files = FindDstSources(Roots);
		Entries.Reserve(Files.Num());
		for (const FString& File : Files)
		{
			FDstSourceEntry Entry;
			if (DriverPrivate::ReadDstSourceEntry(File, Roots, Entry))
			{
				Entries.Add(MoveTemp(Entry));
			}
		}
	}

	void FDstSourceIndex::Rescan(const FString& FilePath)
	{
		const FString Path = DriverPrivate::NormalizeDstPath(FilePath);
		const int32 Index = Entries.IndexOfByPredicate([&Path](const FDstSourceEntry& Entry) { return Entry.FilePath.Equals(Path, ESearchCase::IgnoreCase); });
		const bool bExists = IsDstSourceFile(Path) && IFileManager::Get().FileExists(*Path);

		if (Index != INDEX_NONE && bExists)
		{
			// An edit changes only what this file says; every other entry still resolves the same way.
			FDstSourceEntry Entry;
			if (DriverPrivate::ReadDstSourceEntry(Path, Roots, Entry))
			{
				Entries[Index] = MoveTemp(Entry);
				return;
			}
		}
		else if (Index == INDEX_NONE && !bExists)
		{
			return;
		}

		// A file appeared or went away: an #include anywhere may now resolve to another file (an earlier root shadows a
		// later one), so every entry is read again.
		const TArray<FString> SavedRoots = Roots;
		Scan(SavedRoots);
	}

	const FDstSourceEntry* FDstSourceIndex::FindByFile(const FString& FilePath) const
	{
		const FString Path = DriverPrivate::NormalizeDstPath(FilePath);
		return Entries.FindByPredicate([&Path](const FDstSourceEntry& Entry) { return Entry.FilePath.Equals(Path, ESearchCase::IgnoreCase); });
	}

	const FDstSourceEntry* FDstSourceIndex::FindByAsset(const FString& AssetPath) const
	{
		const FString ObjectPath = DriverPrivate::ToDstObjectPath(AssetPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		// Package names are case-insensitive, as the engine compares them.
		return Entries.FindByPredicate([&ObjectPath](const FDstSourceEntry& Entry)
		{
			return !Entry.AssetPath.IsEmpty() && Entry.AssetPath.Equals(ObjectPath, ESearchCase::IgnoreCase);
		});
	}

	TArray<FString> FDstSourceIndex::FindDependents(const FString& FilePath) const
	{
		const FString Start = DriverPrivate::NormalizeDstPath(FilePath);
		TSet<FString> Reached;
		Reached.Add(Start.ToLower());
		TArray<FString> Frontier;
		Frontier.Add(Start);
		TArray<FString> Dependents;

		while (!Frontier.IsEmpty())
		{
			const FString Current = Frontier.Pop(EAllowShrinking::No);
			for (const FDstSourceEntry& Entry : Entries)
			{
				if (Reached.Contains(Entry.FilePath.ToLower())
					|| !Entry.Includes.ContainsByPredicate([&Current](const FString& Include) { return Include.Equals(Current, ESearchCase::IgnoreCase); }))
				{
					continue;
				}
				Reached.Add(Entry.FilePath.ToLower());
				// A library passes the change on to whatever includes it; only trees are rebuilt.
				Frontier.Add(Entry.FilePath);
				if (Entry.Kind == Lang::EDstFileKind::StateTree)
				{
					Dependents.Add(Entry.FilePath);
				}
			}
		}
		Dependents.Sort();
		return Dependents;
	}
}
