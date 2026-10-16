# DreamStateTree

StateTrees written as text, for Unreal Engine 5.8.

A `.dtr` file describes one StateTree. DreamStateTree builds it into an ordinary `UStateTree` asset, writes existing assets out
as text, and, while the editor runs, writes the edits you make in the StateTree editor back into the text. The text is the
source of truth: a build turns it into the asset, and each later build replaces that asset's content with what the text says.

| File | Document | Builds |
|---|---|---|
| `.dtr` | `StateTree(...)` | a StateTree asset |
| `.dth` | `StateTreeLibrary(...)` | nothing; a library of templates, subtrees and tag prefixes that other files `#include` |

Sources live in a `DTree/` folder: the project's, and the `DTree/` of every enabled plugin that has one.

## Example

```dtr
StateTree(Name="AI/ST_Brawler", Schema=AIComponent(Actor = CombatEnemy))
{
    Parameters = { float AttackRange = 200; FVector WanderSpot; }
    Globals    = { Info = GetPlayerInfo(MaxRange = 2500); }

    State Root {
        -> Root

        State Chase {
            if ObjectIsValid(Info.TargetPlayerCharacter)
            do MoveTo(TargetActor = Info.TargetPlayerCharacter, AcceptableRadius = Params.AttackRange * 0.75)
            -> Attack on Succeeded
            -> Root on Failed [ Delay = 0.5 ]
        }
        State Attack  { do ComboAttack()  -> Recover }
        State Recover { do Delay(1, 0.3) }
    }
}
```

`Parameters` are the tree's parameters, `Globals` holds a global task whose outputs are read as `Info.DistanceToTarget`. A state
is a card: its clauses (`on`, `if`, `pick`, `utility`, `link`, `do`, `->`) come first, child states after. A literal is a
constant and a path is a binding, so `Info.TargetPlayerCharacter` is bound and `Params.AttackRange * 0.75` is an operation the
build turns into a property function. `-> Attack on Succeeded` is a transition. The node names are from the engine's
third-person template; any node of your project works the same way. The whole language is in
[Docs/Language.md](Docs/Language.md).

## Status

Beta, version 0.1.0. Unreal Engine 5.8 only.

- Text to asset: the checker (names, types, bindings, structure), the generator (an asset created, or rebuilt in place) and the
  engine's own compile, with every message placed at the line of the text that caused it.
- Asset to text: the decompiler writes existing trees, the engine templates' among them, as text. The `RoundTrip` test preset
  rebuilds each corpus tree from its decompiled text and compares the two as text, as a model and as a compiled asset.
- In the editor: a saved source rebuilds its asset; a change made in the StateTree editor is written back into the source,
  values in place and structure by rewriting the card it is in; when both changed, the editor lists the conflict and merges on
  request. See [Docs/WriteBack.md](Docs/WriteBack.md).
- The canonical formatter, templates and `#include`, libraries with shared subtrees, and coded diagnostics with hints.

The plugin has **no runtime module**. The assets it builds are plain StateTrees; a packaged game does not need the plugin.

## Requirements

- Unreal Engine 5.8 (5.8 only).
- Windows and PowerShell 7 (`pwsh`) for `Tools/dst.ps1`.
- The plugin is not enabled by default. Enable it in the project's `.uproject`. It uses the `StateTree`, `GameplayStateTree`
  and `PropertyBindingUtils` plugins.

## Modules

| Module | Type | What it holds |
|---|---|---|
| `DreamStateTreeLang` | UncookedOnly | Lexer, parser, canonical printer, `#include` and template expansion, the model. Depends on Core only. |
| `DreamStateTreeCompiler` | UncookedOnly | The checker, the generator, the extractor and the decompiler, write-back, the driver the command line and the editor share, the `-run=DreamStateTree` commandlet, and the extension interface games implement. |
| `DreamStateTreeEditor` | Editor | Keeping assets and their sources in step (rebuild on save, write-back, conflicts), the Content Browser and Tools menu entries, the Dream State Tree message log, and the settings. |
| `DreamStateTreeTests` | UncookedOnly | Automation tests and their fixtures. UncookedOnly so they also load under `-game`. |

## Quick start

1. Write `DTree/AI/ST_Villager.dtr` in your project. Use a schema and nodes that exist in your project; the engine's own
   `Component` schema and its `Delay` task are enough to try it:

   ```dtr
   StateTree(Name="AI/ST_Villager", Schema=Component)
   {
       State Root {
           State Idle { do Delay(2)  -> Next }
           State Look { do Delay(1)  -> Root }
       }
   }
   ```

2. Check it, then build it into `/Game/AI/ST_Villager`:

   ```
   Tools/dst.ps1 check -Project D:/MyGame/MyGame.uproject -File DTree/AI/ST_Villager.dtr
   Tools/dst.ps1 build -Project D:/MyGame/MyGame.uproject -File DTree/AI/ST_Villager.dtr
   ```

   With the editor open you can skip the command line: saving the file rebuilds the asset, and what you change in the asset is
   written back into the file ([Docs/WriteBack.md](Docs/WriteBack.md)).

3. Turn an existing StateTree into text, with **Write as DreamStateTree Text** on it in the Content Browser or from the command
   line. The file lands in `DTree/`, mirroring the package path:

   ```
   Tools/dst.ps1 decompile -Project D:/MyGame/MyGame.uproject -Asset /Game/AI/ST_Brawler
   ```

   Read the warnings: a decompile with warnings has left a detail out. See [Docs/Decompiling.md](Docs/Decompiling.md) before
   you rebuild a decompiled asset in place.

## Command line

`Tools/dst.ps1 <command> [options]`. `make` builds C++; `build` compiles text into assets.

| Command | What it does |
|---|---|
| `make` | Builds the test host's editor target: the plugin's C++. |
| `test [-Preset P] [-Filter F] [-NoBuild]` | Runs `make`, then the automation tests headless in the test host. Presets: `Quick` (the default: the language, the compiler's corpus and unit tests, write-back, the editor), `RoundTrip` (every corpus tree decompiled, rebuilt and compared as text, as a model and as a compiled asset), `Behavior` (rebuilt trees run side by side with the originals), `Spike` (the engine-behaviour experiments), `All`. `-Filter` replaces the preset's tests; several prefixes are joined with `+`. |
| `build [-File F] [-Root R] [-NoSave] [-Json J]` | Compiles the `.dtr` sources into StateTree assets and saves them. `-NoSave` keeps them in memory only. |
| `check [-File F] [-Root R] [-Json J]` | Parses and checks: includes, templates, names, bindings, structure. Nothing is generated. |
| `fmt [-File F] [-Root R] [-Write \| -Verify] [-Markers] [-Json J]` | Prints the canonical text. `-Write` rewrites the files that differ, keeping their line ends. `-Verify` writes nothing and fails on any file that is not canonical. |
| `decompile (-Asset A[,B] \| -Under /Game/Folder) [-Out O] [-TagPrefix P[,Q]] [-Stdout [-Markers]] [-Force] [-Json J]` | StateTree assets to text, under the project's `DTree/` unless `-Out`. `-Stdout` prints instead. A file that exists and differs is replaced only with `-Force`. `-TagPrefix` shortens tags and declares the prefixes. |

- `build`, `check`, `fmt` and `decompile` run `UnrealEditor-Cmd <project> -run=DreamStateTree`. The project is `-Project`, else the
  project above `-File`, else the test host. `make` and `test` use the test host unless `-Project` is given.
- Without `-File`, these commands cover every source under the source roots: the project's `DTree/` and the `DTree/` of every
  enabled plugin. `-Root` replaces those roots. `-File` is absolute, relative to the current folder, or relative to the project.
  A `.dth` given as `-File` stands for the trees that include it.
- `build` orders the sources: a tree that links another tree's asset is built after it.
- The engine follows the project's `EngineAssociation`, using registered engines or Epic Launcher's installation list.
  `-EngineRoot I:/UnrealEngine/UE_5.8` selects one explicitly.
- `#include` resolves against the including file's folder first, then the source roots.
- Diagnostics print as `<file>(<line>,<col>): error DSTnnnn: message`, with an indented `hint:` line when there is one. `-Json`
  writes them all as JSON. `fmt` and `decompile -Stdout` print their texts on the output stream between
  `===== begin <name> =====` and `===== end <name> =====` lines, and their status lines on the host; `-Markers` puts those lines
  around a single text too, which a tool that reads the output needs.
- A build writes packages. Close any editor that has the project open; the script warns when one does.
- The engine log of each run is kept in `<project>/Saved/DreamStateTree/`. `-Raw` shows the whole log.

| Exit code | `build`, `check`, `fmt`, `decompile` | `test` | `make` |
|---|---|---|---|
| 0 | no errors | green | built |
| 1 | errors (for `fmt -Verify`, a file not canonical) | red: a test failed or an ensure fired | |
| 2 | the run did not finish: a crash, a timeout, no summary | not judged: the build failed, the editor crashed or timed out, or too few tests ran | failed |

Test results land in `<host>/Saved/DreamStateTreeTests/<time>-<preset>/`: the engine's `index.json`, the run log and
`summary.md`.

## In the editor

With the plugin enabled, the editor keeps assets and sources in step ([Docs/WriteBack.md](Docs/WriteBack.md)):

- Saving a `.dtr` rebuilds its asset; saving a `.dth` rebuilds the trees that include it. Errors go to the **Dream State Tree**
  message log and the asset is left alone.
- A change made to an asset in the StateTree editor is written into its source once your edits pause. Comments, blank lines
  and your layout stay.
- In the Content Browser, on a StateTree: **Open DreamStateTree Source**, **Rebuild from Text**, **Write Back to Text**, and, for
  a tree not made from text, **Write as DreamStateTree Text**. Under **Tools > Dream State Tree**: rebuild all, rescan, the log.
- **Editor Preferences > Plugins > Dream State Tree** has the switches: rebuild when text changes, save rebuilt assets, write
  back editor changes, write back structure, the delay, and the command that opens a source at a line.

## VS Code

The extension **dreamstatetree-language-support** (a separate project) gives highlighting, a file outline, a tree preview that
draws the file as a StateTree and jumps to the text on click, go to definition and hover for transition targets, `link`
targets, parameters, globals, named tasks, templates and includes, and snippets. Diagnostics, formatting and decompiling call
`Tools/dst.ps1` (a headless editor start per run, about 20 seconds). It needs PowerShell 7 for those, and nothing for the rest.

## Test host

The tests run in a small code project of their own, away from any working project. `Tools/TestHost/New-DreamStateTreeTestHost.ps1`
creates it: a copy of `Tools/TestHost/Template`, this repository as a git worktree in `Plugins/DreamStateTree`, and the engine
templates' game modules and StateTree assets copied from the engine as a corpus of real trees (they are not part of this
repository, and neither is anything of a working project). Parameters: `-HostDir`, `-Branch`, `-EngineAssociation`,
`-EngineRoot`, `-CorpusDir`, `-RefreshCorpus`. The test schema and nodes in `Source/DreamStateTreeTests/Private/Fixtures/` stand
in for a game's own and exercise the extension layers ([Docs/Extending.md](Docs/Extending.md)).

## Documentation

| Page | Contents |
|---|---|
| [Docs/Language.md](Docs/Language.md) | Writing `.dtr` and `.dth` files: every section, clause, transition, value and rule. |
| [Docs/Grammar.md](Docs/Grammar.md) | The formal grammar and the canonical format. |
| [Docs/Mapping.md](Docs/Mapping.md) | How the text maps onto the engine's StateTree editor data: states, transitions, nodes, bindings, identity. |
| [Docs/Decompiling.md](Docs/Decompiling.md) | Turning existing StateTrees into text, what comes out, and what cannot be written. |
| [Docs/WriteBack.md](Docs/WriteBack.md) | Working in the editor: rebuilds, write-back, conflicts, settings. |
| [Docs/Extending.md](Docs/Extending.md) | For games and other plugins: metadata, libraries, the extension class, editor schemas, where sources live. |
| [Docs/Diagnostics.md](Docs/Diagnostics.md) | Every `DSTnnnn` code and how to fix it. |

## Layout

| Path | What |
|---|---|
| `Source/DreamStateTreeLang` | the language: lexer, parser, printer, templates, diagnostics, model (Core only) |
| `Source/DreamStateTreeCompiler` | text to StateTree and back, write-back, command line; the extension interface |
| `Source/DreamStateTreeEditor` | rebuild on save, write-back of editor edits, asset menus, settings |
| `Source/DreamStateTreeTests` | automation tests and their fixtures |
| `Docs/` | the pages above |
| `Tests/Corpus/` | parser and formatter cases |
| `Tools/dst.ps1` | the command line |
| `Tools/TestHost/` | the test host project and the script that creates it |
| `Tools/Docs/Update-Diagnostics.ps1` | keeps `Docs/Diagnostics.md` in step with the codes in the source |
