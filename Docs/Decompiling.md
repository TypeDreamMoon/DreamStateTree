# Decompiling a StateTree

The decompiler reads a StateTree asset and writes the text that builds it. Use it to bring an existing tree to DreamStateTree;
from then on the text is the source. It does not touch the asset.

- [Running it](#running-it)
- [What you get](#what-you-get)
- [What becomes what](#what-becomes-what)
- [Names the decompiler chooses](#names-the-decompiler-chooses)
- [What cannot be expressed](#what-cannot-be-expressed)
- [In the editor](#in-the-editor)
- [Checking the result](#checking-the-result)
- [Migrating a tree safely](#migrating-a-tree-safely)

## Running it

```
Tools/dst.ps1 decompile -Asset /Game/AI/ST_Brawler
Tools/dst.ps1 decompile -Asset /Game/AI/ST_Brawler,/Game/AI/ST_Villager
Tools/dst.ps1 decompile -Under /Game/AI
Tools/dst.ps1 decompile -Asset /Game/AI/ST_Brawler -Stdout
```

| Option | Meaning |
|---|---|
| `-Asset A[,B]` | One or more StateTree assets, comma-separated: a package path (`/Game/AI/ST_Brawler`) or an object path (`/Game/AI/ST_Brawler.ST_Brawler`). |
| `-Under <content path>` | Every StateTree asset under a content folder. The path starts with a mount: `/Game/...`, `/MyPlugin/...`. |
| `-Out <dir or file>` | Where to write, relative to the project. A folder takes the same layout as `DTree/`; a file is allowed for one asset only. |
| `-Stdout` | Print the text instead of writing files. Several assets are printed between `===== begin <asset> =====` and `===== end <asset> =====` lines; `-Markers` writes them around a single text as well. |
| `-Force` | Replace a file that exists and differs. |
| `-TagPrefix P[,Q]` | Shorten gameplay tags that start with these prefixes and declare the prefixes in a `Tags` section ([below](#what-becomes-what)). |
| `-Json <file>` | Also write the diagnostics as JSON. |
| `-Project <uproject>` | The project to open; the default is the plugin's test host. |

Without `-Out` or `-Stdout`, each file goes under the project's `DTree/` folder, mirroring the package path, with the `Game`
mount dropped and any other mount kept as a folder:

| Asset | Text |
|---|---|
| `/Game/AI/ST_Brawler` | `DTree/AI/ST_Brawler.dtr` |
| `/MyPlugin/AI/ST_Brawler` | `DTree/MyPlugin/AI/ST_Brawler.dtr` |

`DTree/` is also where sources live, so the decompiler never replaces a file there by surprise. When the target file holds the
same text, it is left alone and reported unchanged. When it holds different text (a source you wrote by hand, perhaps) that is
an error unless `-Force` is given. The file is written as UTF-8 without a byte order mark, with the line ends of the file it
replaces.

The decompiler opens the asset in a headless editor, so a run takes about half a minute. A tree that holds something the text
cannot express (see [below](#what-cannot-be-expressed)) is not written; the others in the same run are.

## What you get

The text is in the canonical format, so `fmt` leaves it as it is. This is the engine's side-scrolling template tree
`/Game/Variant_SideScrolling/Blueprints/AI/ST_SideScrollingNPC`, decompiled with `-Stdout`:

```dtr
StateTree(Name="Variant_SideScrolling/Blueprints/AI/ST_SideScrollingNPC", Schema=AIComponent(AIController = BP_SideScrollingAIController, Actor = BP_SideScrollingNPC))
{
    Parameters = { double RangeMax = 500; Pawn Current_Target; bool Has_Valid_Target; }

    Colors = {
        `Default Color` = (R = 0.013702, G = 0.309469, B = 0.386429);
        `NPC Active`    = (R = 0.01648, G = 0.208333, B = 0.043083);
        `NPC Inactive`  = (R = 0.4, G = 0.015416, B = 0.015416);
    }

    State Root {
        State `NPC Behavior` {
            State `Select Target` {
                do GetPlayer(TargetPlayer -> Params.Current_Target, ValidTarget -> Params.Has_Valid_Target)
                -> Next on Succeeded
                -> Deactivated on Failed
            }
            State Active [ Color = `NPC Active` ] {
                if Params.Has_Valid_Target and Actor.bDeactivated == false
                -> Deactivated on Tick if Actor.bDeactivated
                State `Follow Player` [ Color = `NPC Active` ] {
                    do MoveTo(TargetActor = Params.Current_Target),
                       DebugText("Follow", ReferenceActor = Params.Current_Target)
                    -> Next
                }
                State Wait [ Color = `NPC Active` ] {
                    do Delay(0.16), DebugText("Wait", ReferenceActor = Actor)
                    -> Root
                }
            }
        }
        State Deactivated [ Color = `NPC Inactive` ] {
            do Delay(0.1), DebugText("Deactivated", ReferenceActor = Actor)
            -> Root
        }
    }
}
```

How to read it:

- The header's `Name` is the package path below the `Game` root; the schema names the AI component schema, and the two classes
  its contexts were set to are written (`AIController`, `Actor`). A tree whose contexts are at their defaults writes `Schema=`
  and the name only.
- `Parameters` lists the tree's parameters with their types and defaults. `Colors` lists the color table; names with spaces are
  in backticks.
- `GetPlayer(TargetPlayer -> Params.Current_Target, ...)` are output bindings: the task writes its outputs into parameters.
- `Actor.bDeactivated == false` is a Compare Bool condition; the bare `Actor.bDeactivated` in the transition is the same node
  against `true`.
- `DebugText("Follow", ...)` uses the node's positional argument `Text`; `Delay(0.16)` its `Duration`.
- `-> Next on Succeeded` and `-> Deactivated on Failed`: a state name is written as the shortest ending that points at one
  state.
- Settings that equal the default (`Enabled`, `Complete = Any`, a delay switched off, ...) are not written. The long `do` is
  broken because it does not fit in 120 columns.

## What becomes what

| In the asset | In the text |
|---|---|
| the schema and the context classes that differ from its defaults | `Schema=Name(Context = Class)` |
| `GlobalTasksCompletion` when it is not the default | `Settings` |
| root parameters | `Parameters`, with their category as metadata |
| evaluators and global tasks | `Globals`, named |
| colors | `Colors` |
| subtrees | top-level `Subtree`, with their parameters |
| states, in the order the editor holds them | `State` cards; a Group is `[ Group ]`; links are `link` and `link asset` |
| a required event | `on Tag`, `on Tag(Payload)` |
| enter conditions | one `if` for the whole list |
| the selection behavior, if not the default | `pick` |
| considerations | `utility` |
| tasks | one `do` for the whole list |
| transitions | `->` clauses, in order |

The clauses of a card are written in this order: `on`, `if`, `pick`, `utility`, `link`, `do`, the transitions; child states
follow.

The decompiler brings back every convenience of the text, each as the exact inverse of what the builder applies, so that
building the result gives the same tree again:

- Node types become the shortest name that resolves back to the same type under the schema's namespaces, with the namespace
  left out when the schema supplies it (`Walk`, not `Villager.Walk`).
- A node that declares positional arguments gets them, from the first, while each has a value; the rest are named. A bool
  argument drops its `b` unless another property would answer to the shorter name.
- A Compare node becomes a comparison, with `not` for an inverted condition, and a Compare Bool against `true` becomes the bare
  path. Operands and indents become `and`, `or` and the parentheses the precedence needs.
- A property function that is one of the arithmetic or logic functions and feeds a property becomes an operator expression;
  every other one becomes a call. A Constant consideration alone becomes `utility 0.5`.
- A constant becomes a literal: numbers in the shortest form that reads back, enum values by name, assets by short name when it
  is unique among assets of that class (else `Path(Root, "Rel")`), structs as tuples with only the fields that differ from the
  struct's default, tags in full, a reference to a tree as a call of its asset with the parameters it overrides.
- A binding that feeds a part of a property (a member of a struct, an element of an array, a parameter of a referenced tree) is
  written inside the property's tuple, list or call; the part the constant holds under it is the default, which the text leaves
  out when it can.
- A context binding the compiler would make by itself is left out; only a real override is written. (Existing trees often bind
  a Context property by hand.)
- An event binding becomes `Event.X`; a binding to a state's parameters becomes `Params.X`, as does one to the tree's.
- Output bindings become `Prop -> Params.X`, property references `ref path`, delegate listeners `Delegate(path)`.
- A transition target is the state's name or the shortest ending of its path that no other state has.

**Tags.** A tag is written in full. With `-TagPrefix Game.Event` (or several prefixes) tags that start with a prefix are
shortened and the prefixes are declared in a `Tags` section:

```dtr
Tags = { Game.Event; }
...
-> Hurt on Hit.Stagger
```

A prefix is used only as far as it reads back as the same tag.

## Names the decompiler chooses

- A name that is not an identifier (it has spaces, or starts with a digit) is written in backticks.
- Every global node is named. A task, condition or other node that something binds to or listens to, and that has no name in the
  asset, gets one from its type: `Delay`, then `Delay2` when that is taken. The name is written, and rebuilding the asset gives
  the node that name. Names also avoid `Params`, `Event` and the schema's contexts.
- A schema with one task per state names the task after its state; the decompiler leaves the name out.
- A tag whose first segment is a word the language uses (`Params`, `Event`, `on`, `do`, ...) or that is not made of plain
  identifiers is written as a string.

## What cannot be expressed

The decompiler does not write half a tree. A tree that holds something the text cannot say is reported with a `DST8xxx` code and
no text is produced for it; a few cases that lose only a detail are warnings, and the text is written.

| Code | The asset has |
|---|---|
| DST8001 | editor data that is a subclass of `UStateTreeEditorData` (another plugin's tree kind) |
| DST8002 | an editor data extension other than the editor's view settings (`StateCentricViewEditorDataExtension` is ignored) |
| DST8003 | a binding to something the text has no name for: a struct that is not in the tree, a node with no usable name, a whole event payload, an event binding that cannot be repaired |
| DST8004 | a value or type with no text form: a parameter type, a fixed-size array that differs from its default, a localized text, a transition forced to stay (warning) |
| DST8005 | two sibling states with the same name |
| DST8006 | (warning) an event binding that a duplicated asset broke; the decompiler repaired it |
| DST8007 | a binding with no place in the model: a task completion outside an `on Delegate` transition, property functions nested too deep, a binding into the parameters of a state that does not link |
| DST8008 | (warning) a detail left out: a color not in the list, a transition's selection fallback, a function without a node |
| DST8009 | a link or transition to a state that is not in the tree |
| DST8010 | no editor data, or no schema |
| DST8051 to DST8058 | things found while writing the text: a node type the text cannot name, a setting that is not a property, a binding into an array element, a schema class that cannot be found, a target or mode with no word, a parameter type with no unique spelling, a `Copy` operand in a condition list |

[Diagnostics.md](Diagnostics.md) has the full table.

A decompile with warnings has left something out; read them before you rebuild the asset from the text.

## In the editor

In the Content Browser, right-click a StateTree and choose **Write as DreamStateTree Text** in the **Dream State Tree** section
of the menu. The text is written to the default place, `DTree/...`. If a file is there already, you are asked before it is replaced. The asset is then
rebuilt from the text once, which links the two: from then on a change made in the StateTree editor is written back into the
text ([WriteBack.md](WriteBack.md)), and saving the text rebuilds the asset. The new file opens with the command in the
settings (`code -g "{file}:{line}:{column}"` by default).

A tree that is made from text has the other entries: **Open DreamStateTree Source**, **Rebuild from Text** and **Write Back to
Text**. Failures show as a notification and in the **Dream State Tree** message log (Tools > Dream State Tree > DreamStateTree
Log).

## Checking the result

Check what the decompiler wrote before you rely on it:

```
Tools/dst.ps1 check -File DTree/AI/ST_Brawler.dtr
Tools/dst.ps1 fmt -File DTree/AI/ST_Brawler.dtr -Verify
```

`check` reads the text through the same stages a build does. A decompiled file that does not check is a bug in the decompiler
(or a node whose module is not loaded). `fmt -Verify` fails if the file is not canonical; decompiled text is.

The plugin's `RoundTrip` test preset runs this against a corpus of real trees: every tree is decompiled, rebuilt from its text
and compared three ways: as text (decompile the rebuilt asset again: the same text), as a model (the same dump of every field)
and as a compiled asset. The `Behavior` preset runs the original and the rebuilt tree side by side with the same events and
ticks.

## Migrating a tree safely

1. Decompile with `-Stdout` and read the text. Look at the warnings.
2. Decompile into `DTree/`. Keep the original asset under source control.
3. `Tools/dst.ps1 check` the new file, then `build -NoSave` to see that it generates.
4. `Tools/dst.ps1 build` rebuilds the asset in place from its text. The asset object is kept, so references to it survive. The
   IDs of its states, nodes and transitions are replaced by IDs derived from their place in the text
   ([Mapping.md](Mapping.md#identity)); anything that stored an old ID is gone.
5. From then on edit the text, or edit the tree in the editor and let write-back update the text. Do not do both at once without
   looking at what the editor says about a conflict.

A build writes packages: close any editor that has the project open, or build from the editor itself.
