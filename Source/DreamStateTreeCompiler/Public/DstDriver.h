// Copyright DreamMoon. All Rights Reserved.
//
// The driver: the whole pipeline for files on disk, used by the commandlet, the editor and the tests.
//
//   check      read -> parse -> includes -> templates -> lower                (no asset touched)
//   build      check -> generate -> save                                       (in dependency order)
//   format     read -> parse -> print                                          (the canonical text)
//   decompile  asset -> extract -> raise -> print                              (text for an existing asset)
//
// Sources live under the source roots: the project's DTree/ folder and the DTree/ folder of every enabled plugin that
// has one. A `.dtr` builds one asset; a `.dth` is a library, built only through the files that include it.

#pragma once

#include "CoreMinimal.h"
#include "DstGenerate.h"
#include "DstLower.h"
#include "Lang/DstDiagnostic.h"
#include "Lang/DstSource.h"
#include "Lang/DstTemplates.h"

class UStateTree;

namespace UE::DreamStateTree::Compiler
{
	/** The DTree/ folders of the project and of every enabled plugin that has one, absolute. */
	DREAMSTATETREECOMPILER_API TArray<FString> GetDstSourceRoots();

	/** `.dtr` or `.dth`. */
	DREAMSTATETREECOMPILER_API bool IsDstSourceFile(const FString& FilePath);

	/** Every `.dtr` and `.dth` under Roots (default: GetDstSourceRoots()), absolute paths, sorted. */
	DREAMSTATETREECOMPILER_API TArray<FString> FindDstSources(TConstArrayView<FString> Roots = {});

	/** One source as the index knows it: read only as far as its includes and its document header. */
	struct FDstSourceEntry
	{
		/** Absolute, normalised (forward slashes). */
		FString FilePath;
		Lang::EDstFileKind Kind = Lang::EDstFileKind::Unknown;
		/** The asset a `.dtr` makes (`/Game/AI/ST_Brawler.ST_Brawler`); empty for a library or a header that does not read. */
		FString AssetPath;
		/** Resolved paths of the files it #includes directly. */
		TArray<FString> Includes;
	};

	/** Every source under some roots, by file and by the asset it makes: what the editor watches and the build orders by. */
	class DREAMSTATETREECOMPILER_API FDstSourceIndex
	{
	public:
		void Scan(TConstArrayView<FString> Roots);
		/** One file changed, appeared or went away. */
		void Rescan(const FString& FilePath);

		const FDstSourceEntry* FindByFile(const FString& FilePath) const;
		/** By object path (`/Game/A/B.B`) or package path (`/Game/A/B`). */
		const FDstSourceEntry* FindByAsset(const FString& AssetPath) const;
		/** The `.dtr` files that include FilePath, directly or through other libraries: what a `.dth` change rebuilds. */
		TArray<FString> FindDependents(const FString& FilePath) const;

		const TArray<FDstSourceEntry>& GetEntries() const { return Entries; }
		const TArray<FString>& GetRoots() const { return Roots; }

	private:
		TArray<FString> Roots;
		TArray<FDstSourceEntry> Entries;
	};

	struct FDstFileResult
	{
		FString FilePath;
		/** Every diagnostic of the file, its libraries' included (each with its own file path). */
		Lang::FDstDiagnosticSink Diagnostics;
		/** The parsed file and its libraries, templates expanded; null when the file could not be read. Write-back reads it. */
		TSharedPtr<Lang::FDstCompilationUnit> Unit;
		/** Filled once lowering ran. */
		TOptional<FDstLowerResult> Lowered;
		/** Filled once generation ran. */
		TOptional<FDstGenerateResult> Generated;

		bool Succeeded() const { return !Diagnostics.HasErrors(); }
	};

	struct FDstBuildOptions
	{
		bool bGenerate = true;
		bool bSave = true;
		/** Replace the source roots; empty to use GetDstSourceRoots(). */
		TArray<FString> Roots;
		/** Generate into this object path instead of the one the header names (tests, round trips). */
		FString OverrideAssetPath;
	};

	/** Checks (and with bGenerate builds) one source file. A `.dth` is checked on its own as far as a library can be. */
	DREAMSTATETREECOMPILER_API FDstFileResult BuildDstFile(const FString& FilePath, const FDstBuildOptions& Options = FDstBuildOptions());

	/** The same for text held in memory (an editor buffer, write-back's candidate text, a test); FilePath names it and resolves includes. */
	DREAMSTATETREECOMPILER_API FDstFileResult BuildDstText(const FString& FilePath, const FString& Text, const FDstBuildOptions& Options = FDstBuildOptions());

	/**
	 * Builds several files in dependency order: a file whose tree links another tree's asset builds after it. A `.dth`
	 * in the list stands for every `.dtr` that includes it. A cycle of links is DST6009 on every file in it.
	 */
	DREAMSTATETREECOMPILER_API TArray<FDstFileResult> BuildDstFiles(TConstArrayView<FString> FilePaths, const FDstBuildOptions& Options = FDstBuildOptions());

	/** The canonical text of a source; false (with diagnostics) when it does not parse. */
	DREAMSTATETREECOMPILER_API bool FormatDstFile(const FString& FilePath, FString& OutText, Lang::FDstDiagnosticSink& Sink);
	/** The same for text held in memory. */
	DREAMSTATETREECOMPILER_API bool FormatDstText(const FString& FilePath, const FString& Text, FString& OutText, Lang::FDstDiagnosticSink& Sink);

	struct FDstDecompileOptions
	{
		/** Where the text will be written (decides the header's Name relative to its root); empty for GetDefaultDstDecompilePath. */
		FString FilePath;
		/** Shorten tags with these prefixes and declare them in a `Tags` section. */
		TArray<FString> TagPrefixes;
	};

	/** The text for an existing asset. False (with DST8xxx diagnostics) when the asset holds something the text cannot. */
	DREAMSTATETREECOMPILER_API bool DecompileDstAsset(const UStateTree& Asset, const FDstDecompileOptions& Options, FString& OutText,
		Lang::FDstDiagnosticSink& Sink);

	/**
	 * Where a decompiled asset's text goes by default: under the project's DTree/, the package path after the mount
	 * (`/Game/AI/ST_Brawler` -> `<project>/DTree/AI/ST_Brawler.dtr`; a plugin mount adds its name:
	 * `DTree/<Plugin>/AI/ST_Brawler.dtr`).
	 */
	DREAMSTATETREECOMPILER_API FString GetDefaultDstDecompilePath(const UStateTree& Asset);

	/** `<file>(<line>,<col>): error DSTnnnn: <message>` lines, the form the command line and the corpus print. */
	DREAMSTATETREECOMPILER_API FString FormatDstDiagnostics(const Lang::FDstDiagnosticSink& Sink);
	/**
	 * Every diagnostic as JSON, for the VS Code extension: `[{"file", "line", "column", "length", "severity", "code",
	 * "message", "hint"}, ...]`, 1-based line and column.
	 */
	DREAMSTATETREECOMPILER_API FString FormatDstDiagnosticsJson(TConstArrayView<const Lang::FDstDiagnosticSink*> Sinks);
}
