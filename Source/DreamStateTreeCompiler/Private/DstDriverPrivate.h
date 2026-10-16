// Copyright DreamMoon. All Rights Reserved.
//
// What the driver and the source index share beyond the public driver interface: path spelling, the header-to-asset rule
// and reading one source as far as the index needs.

#pragma once

#include "CoreMinimal.h"
#include "DstDriver.h"

namespace UE::DreamStateTree::Compiler::DriverPrivate
{
	/**
	 * Absolute (a relative path is taken from the project directory, not the engine's working directory), `/`-separated,
	 * `..` collapsed, no trailing slash. The include loader resolves to the same spelling, so paths from either compare equal.
	 */
	FString NormalizeDstPath(const FString& Path);

	/** The roots as the driver compares them: normalised, without duplicates. */
	TArray<FString> NormalizeDstRoots(TConstArrayView<FString> Roots);

	/**
	 * The object path of the asset a document header names: `/<mount>/A/B.B`. Empty without a Name, or with a Root that is
	 * not Game, Engine or Plugin.X.
	 */
	FString MakeDstDocumentAssetPath(const FString& Name, const FString& Root);

	/** An object path from an object path, a package path or an export text path (`StateTree'/Game/A.A'`). */
	FString ToDstObjectPath(const FString& AssetPath);

	/**
	 * Where `#include IncludePath` written in FromFile points, by the include loader's rule (the including file's folder,
	 * then each root). True when the file exists; otherwise OutPath is where it would be next to the including file, so a
	 * library that is missing still names the documents waiting for it.
	 */
	bool ResolveDstIncludePath(const FString& FromFile, const FString& IncludePath, TConstArrayView<FString> Roots, FString& OutPath);

	/** Reads one source as far as the index needs (kind, asset, direct includes). False when it cannot be read. */
	bool ReadDstSourceEntry(const FString& FilePath, TConstArrayView<FString> Roots, FDstSourceEntry& OutEntry);

	/** The path of a source relative to the first root that holds it; the whole path when none does. */
	FString MakeDstSourceRelativePath(const FString& FilePath, TConstArrayView<FString> Roots);
}
