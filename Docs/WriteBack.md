# Working in the editor

The text is the source of a StateTree made with DreamStateTree. While the editor runs it keeps the two in step both ways. Saving
a source rebuilds its asset. Changing the asset in the StateTree editor writes the change into its source. When both changed at
once, nothing is overwritten: the editor lists the conflict and you choose.

- [Sources and the assets they make](#sources-and-the-assets-they-make)
- [From text to asset](#from-text-to-asset)
- [From asset to text](#from-asset-to-text)
- [Rules of write-back](#rules-of-write-back)
- [Templates and libraries](#templates-and-libraries)
- [What is never written back](#what-is-never-written-back)
- [When both changed](#when-both-changed)
- [Settings](#settings)
- [Menus and the log](#menus-and-the-log)

## Sources and the assets they make

The editor finds sources under the project's `DTree/` folder and the `DTree/` folder of every enabled plugin. The header of a
source names the asset it makes: `StateTree(Name="AI/ST_Brawler")` makes `/Game/AI/ST_Brawler`, and `Root="Plugin.MyPlugin"` puts
it under that plugin's content. An asset is "made from text" when some source under those folders names it.

**Tools > Dream State Tree > Rescan DreamStateTree Sources** finds the sources again. The editor also watches the folders and
picks up files that appear.

The package of an asset made from text keeps the text it was last built from or written to (metadata `DreamStateTree.Source`)
and the path of that source (`DreamStateTree.SourcePath`). That text is the base the editor compares and merges against.

## From text to asset

When a `.dtr` or `.dth` file is saved, its asset is rebuilt a moment after the file settles. When a `.dth` library is saved,
every tree that includes it is rebuilt, and a tree that links another tree's asset is built after it.

- The asset is rebuilt in place: the `UStateTree` and its editor data object stay, so an asset editor that is open stays valid and
  shows the new tree.
- The rebuilt asset is saved, unless **Save Rebuilt Assets** is turned off.
- A source with errors does not touch its asset. The errors go to the **Dream State Tree** message log
  (**Tools > Dream State Tree > DreamStateTree Log**); each entry opens the source at its line.
- A file the editor wrote itself (a write-back, a merge) does not start a rebuild: it remembers what it wrote.
- What was changed in the asset and is not in the text is gone after the rebuild. When the asset has such changes the editor
  does not rebuild; it shows a conflict instead ([When both changed](#when-both-changed)).

**Rebuild from Text**, in the asset's context menu in the Content Browser, and **Tools > Dream State Tree > Rebuild All
DreamStateTree Sources** rebuild on demand. If **Rebuild When Text Changes** is off, only these rebuild.

## From asset to text

A change you make to an asset made from text is written into its source once your edits pause: **Write-Back Delay** seconds
with no further change, no transaction open and no mouse button held. **Write Back to Text** on the asset writes at once.
Undo and redo are changes like any other and are written too. Saving the asset in the editor writes the pending changes first,
so a change made outside a transaction (a script, another tool) reaches the text no later than the asset reaches disk. A cook
and a command line run do not write back.

The editor reads the asset again, extracts it, and compares it, value by value, with what its source says. The parts of the asset
are matched with the parts of the text by ID (the build derived each ID from the part's place in the text,
[Mapping.md](Mapping.md#identity)); when that fails, as it does after the asset was duplicated or saved under another name,
by state path together with a node's name, or its role and index; what matches neither is new.

What is written, and how, smallest edit first:

| Change in the editor | What changes in the text |
|---|---|
| A constant argument of a node (a duration, an enum, a number) | Just that literal. |
| An argument added or removed, or a binding or an operation changed, on one node | That node's call, reprinted. Arguments and settings that did not change keep their text. |
| A state's settings (`Tag`, `Color`, `Enabled`, ...) | The `[ ]` of the card's head. |
| Clauses, tasks, conditions or transitions added, removed, reordered or changed in a state | The card, reprinted from the asset. Clauses that still say the same thing keep their text and comments; child cards that did not change keep their text. |
| Child states added, removed or moved | The card they are in, reprinted in the same way. |
| The header's schema, a section (`Settings`, `Parameters`, `Globals`, `Colors`) | The entries whose values changed in place; otherwise that section, reprinted. |
| A top-level state added or removed | The state, added before the closing brace or removed with its comments. |

`Tags` is not part of the asset; write-back never touches it. Everything else stays as you wrote it: comments, blank lines,
aligned tables, and calls and cards you broke over lines. The file keeps its line ends and its byte order mark, if it had one.

## Rules of write-back

1. **The text must check.** If the source no longer checks (an error in it), nothing is compared and nothing written; the
   text's own diagnostics are shown.
2. **A fixed point.** The new text is built again (without generating) and its model must be identical to the asset's. If it is
   not, nothing is written and the asset keeps its changes (DST8101).
3. **Smallest edit first**, as in the table: a literal, then a call, then a card, then a section.
4. **Nothing is invented.** A change that has no text form is refused with a code and stays in the asset; see
   [What is never written back](#what-is-never-written-back).
5. **A refusal is reported once.** The notification and the log say what was refused; moving states around in the editor does
   not repeat it.

With **Write Back Structure** turned off, the structural changes (states, clauses, tasks, conditions or transitions added,
removed or reordered; a section's entries added or removed) are refused (DST8104) and only values are written.

## Templates and libraries

A node that comes from a template call is written through the call. A changed value that you passed to the template as an
argument is written at the call, in place of that argument.

A value written inside the template's body has no place at the call: changing it there would change every use of the template.
The change is refused (DST8102) with a hint to change the template, or to write the node out in place of the call. A call that
write-back reprints from the asset replaces the template use inside it with what it expands to, and a note says so (DST8106).

A subtree that was copied into the tree from an included library cannot be changed from the tree: the change is refused
(DST8103). Make it in the library's subtree, where every tree that links it gets it.

## What is never written back

A refusal is reported as a warning (the editor's notification and the log show it), because the rest of the changes are still
written. Only DST8101, DST8105 as an error and DST8108 mean that nothing was written at all.

| Code | What |
|---|---|
| DST8101 | The text written for the asset's changes would not rebuild to the asset (or broke the text); nothing was written. |
| DST8102 | The change is inside the expansion of a template. |
| DST8103 | The change is inside a subtree copied from a library. |
| DST8104 | A structural change while **Write Back Structure** is off. |
| DST8105 | The asset holds something that cannot be written as text: the other messages say what, or the place in the text was not found. |
| DST8107 | A card was reprinted and its `#Region` lines were dropped (they cannot be placed again). |
| DST8108 | Two planned edits overlapped, so nothing was written (a bug; the asset keeps its changes). |
| DST8109 | The top-level states are in another order in the asset; the text keeps its order (the first state is the root: reorder them in the text). |

What the model cannot hold at all is the decompiler's list ([Decompiling.md](Decompiling.md#what-cannot-be-expressed)); a
tree that holds one of them is not written back either. [Diagnostics.md](Diagnostics.md#dst8xxx-decompiling-and-write-back) has
every code and its fix.

A refused change stays in the asset, and the text keeps what it said. The next rebuild from the text drops the change.

## When both changed

If the source changes on disk while the asset has changes that are not in it, neither side is overwritten. This happens when the
text is edited before an edit in the editor was written back, and after a change was refused. The log lists the asset's
changes, and a notification waits for your choice:

- **Use Text** rebuilds the asset from the file. The asset's own changes are dropped.
- **Merge Both** writes the asset's changes into the text it was last in step with (the base), then merges that, line by line,
  with the file as it is now, writes the result and rebuilds the asset from it. Changes on the same or neighbouring lines are
  not merged; then nothing is written, the log shows both versions of each place, and you can edit the text by hand or use it as
  it is. Nothing is merged either when part of the asset's changes cannot be written (a refusal): merging the rest would drop
  it. The conflict stays until you choose **Use Text** or make the change in the text yourself.

A refused change counts as a change of the asset's own: a text saved on disk after a refusal is a conflict, not a rebuild that
would drop it. Until you choose, the asset's changes are not written back.

A change to a library the tree includes rebuilds the tree. If nothing in this editor session marked the tree as edited (after a
restart, say), the rebuild drops whatever the asset had that the text never held.

## Settings

**Editor Preferences > Plugins > Dream State Tree**:

| Setting | Default | Meaning |
|---|---|---|
| Rebuild When Text Changes | on | Rebuild an asset when its source (or a library it includes) changes on disk. |
| Save Rebuilt Assets | on | Save an asset after rebuilding it, so the asset on disk matches the text. |
| Write Back Editor Changes | on | Write changes made to an asset in the editor back into its source. |
| Write Back Structure | on | Also write added, removed and reordered states, tasks, conditions and transitions, by rewriting the card or section they are in. |
| Write Back Delay | 1.0 s | Seconds without further edits before changes are written (0.1 to 30). |
| Open Source Command | `code -g "{file}:{line}:{column}"` | How a source is opened at a line. `{file}`, `{line}` and `{column}` are replaced. Run through the shell, so a script on the `PATH` works. Empty opens the file with its associated program. |

## Menus and the log

On a StateTree in the Content Browser, in the **Dream State Tree** section of the context menu:

| Entry | For | What it does |
|---|---|---|
| Open DreamStateTree Source | an asset made from text | Opens the source with the Open Source Command. |
| Rebuild from Text | an asset made from text | Compiles the text into the asset again; changes in the asset that are not in the text are dropped. |
| Write Back to Text | an asset made from text | Writes what changed in the asset into its text now. |
| Write as DreamStateTree Text | an asset not made from text | Decompiles it to `DTree/...` and rebuilds it from that text so that edits are written back ([Decompiling.md](Decompiling.md#in-the-editor)). |

Under **Tools > Dream State Tree**: **Rebuild All DreamStateTree Sources**, **Rescan DreamStateTree Sources** and
**DreamStateTree Log**, which opens the **Dream State Tree** message log. Rebuilds, write-backs, merges and decompiles report
there, and an entry for a problem in a source opens the source at its line.

## Engine behaviour to know about

- Write-back reads the whole asset again each time and compares it with the text; it does not depend on the editor's undo
  stack. That matters because in 5.8 undoing "duplicate selected states" does not restore the data completely.
- Duplicating an asset, or saving it under another name, gives its states and nodes new IDs, and a 5.8 bug also breaks the
  bindings to event payloads in the copy so that the copy does not compile. Write-back copes with the new IDs by matching on
  names; for the copy, make a source of its own with another `Name` and build it, rather than duplicating the asset. The
  decompiler repairs such a binding where it can and says so (DST8006).
