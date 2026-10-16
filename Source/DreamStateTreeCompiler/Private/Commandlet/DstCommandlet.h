// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DstCommandlet.generated.h"

/**
 * DreamStateTree without the editor UI.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=DreamStateTree [mode] [options]
 *
 * Modes
 *   (none)      build: compile the sources into StateTree assets and save them
 *   -Check      parse, includes, templates and semantic checks only; nothing is generated
 *   -Format     print each file's canonical text to stdout between `===== begin <file> =====` and
 *               `===== end <file> =====`; with -Write rewrite the files that differ (keeping each file's
 *               line ends, UTF-8 without BOM); with -Verify write nothing and count every file that is
 *               not canonical as an error
 *   -Decompile  StateTree assets to text: -Asset=<object or package path> (repeatable, or
 *               comma-separated) and/or -Under=<content path> (every StateTree asset below it)
 *
 * Options
 *   -File=<path>   one source (absolute, or relative to the project directory); a library (.dth) stands
 *                  for the trees (.dtr) that include it. Without it, every .dtr and .dth under the roots
 *   -Root=<dirs>   source roots instead of the project's and the enabled plugins' DTree/ (comma-separated)
 *   -NoSave        build: generate in memory without saving packages
 *   -Out=<path>    decompile: a folder (the text goes where it would below DTree/) or, for one asset, a file;
 *                  default: under the project's DTree/
 *   -Stdout        decompile: print the text instead of writing it (-stdout also makes the engine echo its
 *                  log to stdout)
 *   -Force         decompile: overwrite a file that exists and differs (it may be a hand-written source)
 *   -TagPrefix=<p> decompile: shorten gameplay tags that start with these prefixes (comma-separated) and
 *                  declare them in a Tags section
 *   -Json=<file>   also write every diagnostic as JSON (what the VS Code extension reads)
 *
 * Every diagnostic is logged as `<file>(<line>,<col>): error DSTnnnn: message` (a hint, when there is one, on a
 * line of its own), then a one-line summary. The exit code is 0 when nothing went wrong and 1 when anything did.
 */
UCLASS()
class UDreamStateTreeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDreamStateTreeCommandlet();

	virtual int32 Main(const FString& Params) override;
};
