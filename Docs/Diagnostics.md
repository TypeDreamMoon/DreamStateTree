# DreamStateTree diagnostics

Every problem DreamStateTree finds in a source, or meets in an asset, has a code `DSTnnnn`, a severity, a message and a position.
Many also carry a hint that says how to fix it.

The command line prints one line per diagnostic, in the form Visual Studio and VS Code problem matchers read, with the hint on
an indented line below it:

```
I:/MyGame/DTree/AI/ST_Brawler.dtr(12,24): error DST6003: `Idle` is a Group: it holds states, not tasks.
    hint: Remove [ Group ], or move the tasks into a child state.
```

`-Json <file>` (on `build`, `check`, `fmt` and `decompile`) writes the same diagnostics as JSON, hint included:

```
[{"file": ..., "line": ..., "column": ..., "length": ..., "severity": "error", "code": "DST6003", "message": ..., "hint": ...}]
```

Line and column are 1-based. A diagnostic that has no place in the text (a failure to save the asset, a report about the whole
tree) points at the file header. The decompiler's diagnostics are about an asset, not a file: they name the asset by its object
path, and have no line.

The exit code is 0 when there are no errors and 1 when there are; warnings do not change it. It is 2 when the run did not finish
(a crash or a timeout).

A few problems belong to the run, not to any text: a `-File` that does not exist or is not a source, a file that cannot be
written, options that contradict each other, an asset that cannot be loaded for `decompile`, a decompiled file that would
replace a different one without `-Force`, and a file `fmt -Verify` finds not canonical. They are printed as errors without a
code, and they count as errors.

| Severity | Meaning |
|---|---|
| error | The file cannot be built as written. |
| warning | The file builds, but something is probably wrong; for the decompiler, a detail was left out. |
| info | A note from the engine's compile. |

The checker keeps going after an error, so one run reports as much as it can. When one mistake causes several errors, fix the
first one and check again. A file with syntax errors (DST1xxx to DST2xxx) is not checked further: fix those first.

| Range | Raised by |
|---|---|
| [DST1xxx](#dst1xxx-characters) | the lexer: characters, strings, comments, numbers, directives |
| [DST2xxx](#dst2xxx-syntax) | the parser: the shape of the file, sections, states, clauses, expressions, metadata |
| [DST3xxx](#dst3xxx-templates-and-includes) | template expansion (DST31xx) and `#include` (DST32xx) |
| [DST4xxx](#dst4xxx-names-and-types) | the checker: node types, schemas, tags, parameter types, settings, values |
| [DST5xxx](#dst5xxx-bindings-and-expressions) | the checker: paths, bindings, categories, comparisons, operations |
| [DST6xxx](#dst6xxx-structure) | the checker: states, clauses, targets, links, subtrees |
| [DST7xxx](#dst7xxx-generation-and-the-engines-compile) | the generator while it writes the asset, the engine's compile, and extensions |
| [DST8xxx](#dst8xxx-decompiling-and-write-back) | the decompiler (DST80xx to DST805x) and write-back (DST81xx) |

Language rules are in [Language.md](Language.md). A game's own checks (an extension's `Validate`, an editor schema's
`HandlePostInternalCompile`) arrive as DST7007, DST7003 and DST7004 with their own message and code
([Extending.md](Extending.md)).

The tables below are kept up to date by `Tools/Docs/Update-Diagnostics.ps1`, which reads every `TEXT("DSTnnnn")` in the source:
the Code, Severity and Message columns come from the source (the first message of a code; `…` marks a part the program fills
in), and the last column is written by hand.

## DST1xxx: characters

The lexer turns text into tokens. These errors point at the character where reading failed.

<!-- dst:begin 1 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST1001 | error | Unexpected character '…'. | A character that no token starts with: a single `&` or `\|`, a single quote, a stray symbol, a control character, or a `#` that is not the first thing on its line. Remove it. Write `and`, `or`, `not` for the logical operators and double quotes for strings; a directive starts its line. |
| DST1002 | error | The string is not closed on its line. | Add the closing `"`. A string cannot span lines; write `\n` for a line break. |
| DST1003 | error | The block comment is not closed; expected '*/'. | Add `*/`. The error points at the opening `/*`; everything after it is inside the comment, so later errors may come from this one. |
| DST1004 | error | Malformed number '…'. | Write numbers as `12`, `0.5`, `.5`, `1e-3` or `2.5E+4`, without a suffix. `1.`, `1e`, `0x1F`, `2px` and `1.2.3` are malformed. |
| DST1005 | error | Unknown directive '#…'; the directives are #include, #Region and #EndRegion. | The directives are `#include`, `#Region` and `#EndRegion`, and the case matters. The hint gives the right spelling when only the case differs. |
| DST1006 | error | Unknown escape '\…' in a string. | The escapes in a string are `\"`, `\\`, `\n`, `\t` and `\r`. Write a backslash as `\\`. |
| DST1007 | error | The name in backticks is not closed on its line. | Write the whole name between two backticks on one line, as in `Move To`. A name in backticks cannot be empty or span lines. |
<!-- dst:end 1 -->

## DST2xxx: syntax

The parser checks the shape of the file. The grammar is in [Grammar.md](Grammar.md). A file with syntax errors is not checked
further.

<!-- dst:begin 2 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST2001 | error | Expected …, found …. | The parser's general "expected X, found Y". Read what X is and fix the text at the position: a missing `{`, `(`, `)`, `]`, `,` or `=`, an expression, a name after `.`, a type, a template name, a label after `#Region`, a `;` after a template, or nesting that is too deep. The end of the file inside a bracket is DST2024. |
| DST2002 | error | The file has no document; expected StateTree(...) or StateTreeLibrary(...) followed by '{ ... }'. | Write `StateTree(Name="...", Schema=...) { ... }` (a `.dtr` file) or `StateTreeLibrary(Name="...") { ... }` (a `.dth` file). |
| DST2003 | error | The file already has a document; a file holds exactly one. | A file holds exactly one document. Move the second one to a file of its own. Anything written after the closing `}` of the document is reported with this code too. |
| DST2004 | error | Unknown document kind '…'; expected StateTree or StateTreeLibrary. | The document kind is `StateTree` or `StateTreeLibrary`; the case matters. |
| DST2005 | error | The document header takes only named arguments: Name = "...", Root = "...", Schema = ... . | Header arguments are named: `Name = "..."`, `Root = "..."`, `Schema = ...`. The context overrides of the schema are named too: `Schema=AIComponent(Actor = CombatEnemy)`. |
| DST2006 | error | '#include' must come before the document. | Move the `#include` line to the top of the file, above the document. |
| DST2007 | error | Expected …, found …. | `#include` takes a string path: `#include "VillagerCommon.dth"`. |
| DST2008 | warning | '#EndRegion' has no matching '#Region' in this block. | Pair every `#Region "label"` with an `#EndRegion` in the same block (the document, a section or a state card). |
| DST2009 | error | Expected …, found …. | A section entry is written `Key = Value;`, `Type Name [= Value];` or `Value;`, and each section takes one form: `Parameters` takes `Type Name`, `Globals`, `Settings` and `Colors` take `Key = Value`, `Tags` takes bare prefixes. A `Value;` entry takes no `[ ]`. |
| DST2010 | error | Expected …, found …. | End the section entry with `;`. |
| DST2011 | error | Expected …, found …. | The braces of the document hold sections (`Name = { ... }`), `Template`, `State`, `Subtree` and `#Region`. Clauses (`do`, `->`, ...) belong inside a `State`. |
| DST2012 | error | Expected …, found …. | Inside a state, every item starts with a clause keyword (`on`, `if`, `pick`, `utility`, `link`, `do`, `->`), with `State` or with `#Region`. Clauses are not separated by anything: join conditions with `and` and `or`, and tasks with `,`. |
| DST2013 | error | Expected …, found …. | A `do` clause lists its tasks separated by commas: `do Wait(1), Tell(Fled)`. Also raised when `do`, a `,` or `Name =` is followed by no task. |
| DST2014 | error | The parameter '…' is declared twice. | A parameter list names a parameter twice, or a parameter without a default follows one with a default (templates and subtrees). Rename the duplicate and put the parameters with defaults last. |
| DST2015 | error | Expected …, found …. | `->` needs a target: a state name or path, or `Next`, `NextSelectable`, `Parent`, `NextParent`, `NextSelectableParent`, `TreeSucceeded`, `TreeFailed`. A state called like one of those words is written in backticks. |
| DST2016 | error | Expected …, found …. | `link` needs a target: `link HitReaction(Toughness = 2)` for a subtree, `link asset ST_Other(...)` for another asset. |
| DST2017 | error | '…' is not an operator here; write '…'. | Write `and`, `or` and `not`. The parser goes on as if you had. |
| DST2018 | error | Expected …, found …. | Metadata is `[ Key = Value; Flag ]` with the entries separated by `;` (a comma is an error). Also raised for a `Name` setting that is not a string and for a parameter metadata value that is not a literal or a name. |
| DST2019 | error | A State takes no parameters; only a Subtree does ('…' is a State). | Only a `Subtree` has a parameter list. A state gets values through the subtree it links: `link Name(Param = value)`. |
| DST2020 | error | A Subtree can only be written at the top level of the document, not inside a state. | A `Subtree` is a member of the document or of a library, never of a state. Move it out and `link` it. |
| DST2021 | error | A positional argument cannot follow a named one. | In a call, a tuple or a link, put the positional arguments before the named ones. |
| DST2022 | error | Expected …, found …. | `State` and `Subtree` are followed by a name: an identifier, or any text in backticks. |
| DST2023 | error | The argument '…' is given twice. | An argument name is given twice in one call, tuple or header. Remove one. |
| DST2024 | error | This '…' is not closed before the end of the file. | Add the bracket the message names. The error points at the opening bracket that was still open at the end of the file. |
<!-- dst:end 2 -->

## DST3xxx: templates and includes

Includes are resolved and templates expanded after parsing and before any name is checked. DST31xx are template errors, DST32xx
include errors. A diagnostic in an expanded template points at the use.

<!-- dst:begin 3 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST3101 | error | Template '…' needs an argument for '…'. | Pass the argument (positionally or by name), or give the template parameter a default. |
| DST3102 | error | Template '…' has no parameter '…'. | Use the template's parameter names, each once. The hint lists them. |
| DST3103 | error | Template '…' takes …; this argument is extra. | Remove the extra arguments, or add parameters to the template. |
| DST3104 | error | Template '…' expands more than … levels deep; does it use itself? | Template expansion is more than 16 levels deep; a template probably uses itself. Remove the recursion. |
| DST3105 | error | Template '…' is already defined in … (line …). | Two templates have the same name in the file and its includes together. Rename one. |
| DST3106 | error | Template '…' has no body; write it as 'Template …… = expression;'. | Write `Template Name(params) = expression;` (or `Template Name = expression;`). A block body, or no body, is not a template. |
| DST3107 | error | A template argument cannot be an output binding ('… -> ...'); template '…' only takes values. | A template takes values only. Write `Name = value`, not `Name -> path`. |
| DST3108 | error | Template '…' does not expand to a call, so these settings have nothing to apply to. | Settings written after a template use (`Nervous() [ Enabled = false ]`) apply to a call. Make the template's body a call, or put the settings inside the template. |
| DST3201 | error | Cannot find the included file '…'. | The path is looked up next to the including file, then under each `DTree/` source root. Fix the path or add the root. |
| DST3202 | error | '…' includes itself through a cycle: … -> …. | The libraries include each other in a circle; the message lists the chain. Move what both need into a third library. |
| DST3203 | error | '…' is not a library: an included file must hold a StateTreeLibrary(...) document. | An included file must hold a `StateTreeLibrary(...)` document (a `.dth` file). A `.dtr` cannot be included. |
| DST3204 | error | Cannot read …. | The file could not be read. Check that it exists, is not locked, and that the path is right. |
<!-- dst:end 3 -->

## DST4xxx: names and types

The checker resolves every name against the engine's reflection and the asset registry.

<!-- dst:begin 4 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST4001 | error | `…` is not a Blueprint node. | No node of that name in this place: tasks follow `do`, conditions `if`, considerations `utility`, evaluators and global tasks go in `Globals`, property functions go in arguments. Also raised for a `Path(...)` that is not a Blueprint node and when no node is written at all. Names are case-sensitive; the hint suggests near ones. Check the schema's namespaces and that the plugin that declares the node is loaded. |
| DST4002 | error | `…` could be any of: …. | Several node types answer to the name. Write the struct name without the `F` (`StateTreeDelayTask`) or the namespace (`Villager.Walk`). |
| DST4003 | error | The schema … does not allow `…`. | The schema refuses the node (its `IsStructAllowed` or `IsClassAllowed`). Use a node the schema accepts, or another schema. |
| DST4004 | error | The schema is written as a name, such as AIComponent. | The schema in the header is unknown, ambiguous, or not written as a name. A schema name is its class name without `U`, `StateTree` and `Schema` (`AIComponent`), or its `DreamStateTree` metadata. The hint lists the schemas. |
| DST4005 | error | The StateTree header does not say which schema the tree uses. | Add `Schema=...` to the header, for example `Schema=AIComponent`. |
| DST4006 | error | The StateTree header has no argument `…`. | The header takes `Name`, `Root` and `Schema`, and nothing else. `Name` is required and ends in an asset name; `Root` is `"Game"`, `"Engine"` or `"Plugin.<Name>"` of an enabled plugin. |
| DST4007 | error | `…` does not have the properties a comparison needs. | The node has no property of that name. Arguments are properties of the node's instance data, then editable properties of the node struct itself. A positional argument that a node declares (`DreamStateTreeArgs`) must exist too. The hint lists the properties. |
| DST4008 | error | `…` is not a value of …. | A value does not fit its target: the wrong kind of literal, an unknown enum value, an asset, class or struct that is unknown or ambiguous, an unknown word after `pick` or `Priority =`, a tuple with too many or unknown fields. The message says what was expected. |
| DST4009 | error | `…` is not a type the property bag knows. | Parameter types are `bool`, `int`, `int64`, `float`, `double`, `Name`, `String`, `Text`, a struct (`Vector`), an enum, an object class (`Actor`), `Class<Actor>`, `SoftClass<T>`, `SoftObject<T>`, `Array<T>` and `Set<T>` (two container levels at most). Name a struct, enum or class that exists and is unique. |
| DST4010 | error | Expected a gameplay tag, such as Game.Event.Hit. | The tag is not in the project's tag table (the text never adds tags), or a declared prefix completes it in several ways. Add the tag in the project settings, or write its full name. Declare prefixes with `Tags = { Game.Event; }`. |
| DST4011 | error | An event with a payload is written Tag(PayloadType): one type name. | `on Tag(PayloadType)` takes one type name: a `USTRUCT` or a user-defined struct asset. |
| DST4013 | error | `…` is not a section. | The sections are `Settings`, `Parameters`, `Globals`, `Tags` and `Colors`, and each is written once. |
| DST4014 | error | `…` is not a setting of the tree. | Not a known key in a `[ ]`: a state setting, a transition option, a node setting or a tree setting (the hint lists the keys). Also raised for `ConsumeEvent` where there is no event, and for node settings in the options of a transition (put them before the options, or put the call in parentheses). |
| DST4015 | error | The schema does not let a tree change how global tasks complete. | The schema does not allow the feature: evaluators, enter conditions, utility, more than one task per state, tree parameters, a way of completing tasks, a state kind (Group, Linked, LinkedAsset), a `pick` mode. Remove it or use a schema that allows it. |
| DST4016 | error | `…` takes no positional arguments. | More positional arguments than the node declares (`DreamStateTreeArgs`, or the built-in table). Name the arguments. |
| DST4017 | error | `…` is given twice. | A property or setting is given twice, also when a `b` is dropped from a bool name or `Name` is set twice. Remove one. |
| DST4018 | warning | `…` means … here; it hides …. | The name exists in several places and the first namespace of the schema wins; the other is hidden. Write the full name to mean the other node. |
| DST4019 | error | The schema … has no context or setting called `…`. | The header's `Schema(...)` names a context or setting the schema does not have. The hint lists the contexts. |
| DST4020 | error | There is no color called `…`. | `Color = Name` needs an entry `Name = (R, G, B);` in the `Colors` section. |
| DST4021 | error | `…` is a …, but a … is expected here. | A node of another kind is written here. Tasks follow `do`, conditions `if`, considerations `utility`, evaluators and global tasks go in `Globals`. |
| DST4022 | warning | The schema allows one task per state, and the engine names that task after the state: `…` becomes `…`. | A schema that allows one task per state names that task after its state, whatever you wrote. Read its outputs as `StateName.Something`. |
<!-- dst:end 4 -->

## DST5xxx: bindings and expressions

Paths, categories, comparisons and operations. [Language.md](Language.md#paths-and-bindings) has the rules these enforce.

<!-- dst:begin 5 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST5001 | error | Expected a path such as `Params.Speed` or `Info.Target`. | A path starts with `Params`, `Event`, a context of the schema, a global node or a named task. Also raised for a name that is neither a path nor a value of the property, for a `Params.X` that is both a subtree parameter and a tree parameter, and for a path that is also a tag (write the tag as a string). |
| DST5002 | error | `…` (…) has no members, so `…` cannot be read from it. | The path walks into something that is not there: a parameter or property that does not exist, `Params` or `Event` with no member, a value with no members, or `Event.X` on an event that has no payload type (write `on Tag(PayloadType)`). |
| DST5003 | error | Cannot bind `…` (…) to … (…): the types are not compatible. | The types do not fit: a binding or a function result into an incompatible property, an output copy, a `ref`, or a Context property for which the schema has no context. Bind by hand, or change the type. |
| DST5004 | error | `…` of `…` is a property reference and needs a target: … = ref Params.Something. | An Input property must be bound to a path; a constant is not accepted. A property reference needs `ref path`. Write `Property = Params.Something`. |
| DST5005 | error | … is an Output: a node writes it, nothing writes into it. | A node writes its Output properties; nothing writes into them. To copy one somewhere, write `Property -> Params.Something`. |
| DST5007 | error | There is no event to read for `…`. | `Event.X` reads the event of the nearest state with `on Tag`, or of the event transition whose condition this is. Put `on Tag` on the state or an ancestor, or use `State.Event.X`. A subtree does not see the event of the state that links it. |
| DST5008 | error | There is no comparison for …. | There is no Compare node for that type (an object, for instance), or the operator does not work on it: bools, enums and names take only `==` and `!=`. Use `ObjectIsValid` for objects. |
| DST5009 | error | Both sides of the comparison are constants. | One side of a comparison must read something that can change: a path, an operation on one, or a property function call. |
| DST5010 | error | `…` has no bInvert, so `not` cannot be applied to it. | `not` needs a condition with a `bInvert` property, and applies to one condition: `not (A and B)` is an error. Negate each condition, or compare the value with `false`. |
| DST5011 | error | There is no property function for …: arithmetic and logic produce a number or a bool. | Arithmetic and `and` / `or` / `not` give a number or a bool. The target must be a float, an int or a bool; a comparison is a condition, not a value; a property function call needs one result (`Result`, `bResult`, `Output`, or its only Output property). |
| DST5012 | error | Parentheses are nested … deep; the engine's condition list takes at most …. | The engine's condition list takes at most 4 levels of parentheses. Flatten the expression or move part of it into a template or a subtree. |
| DST5013 | error | This is not a utility consideration. | Not a condition (a bare path that is not a bool, an arithmetic expression where a condition is expected, a `ref` or a tuple in a comparison), or not a utility consideration. A condition is a condition node call, a comparison or a bool path; a utility is consideration nodes and numbers joined with `*`, `and`, `or`. |
| DST5014 | error | `…` is a task: conditions, utility scores and state parameters cannot read a task's output. | Conditions, utility scores and state parameters are built before the task data exists. Read the task's output in the argument of a later task, or copy it to a parameter with `Prop -> Params.X`. |
| DST5015 | error | `ref` is for property references; … is not one. | `ref` is for property reference properties, and such a property needs `ref`. The target must be a parameter, an event or an Output of a node that runs earlier, not a context. |
| DST5016 | error | … is not an Output, so `->` cannot copy it. | In `Prop -> Params.X`, `Prop` must be an Output of the node (not a delegate or a reference), and the destination a parameter of the tree or of the subtree. |
| DST5017 | error | `…` cannot be read here: it comes later in Globals. | The node runs later than the reader. A global node reads the globals before it (evaluators come first); a task reads the named tasks of the states above and the tasks before it in its own state; a task of another state cannot be read. |
| DST5018 | error | `Delegate(...)` takes a path to what it listens to: Delegate(Combo.Succeeds) or Delegate(Info.OnTargetLost). | `Delegate(...)` takes one path: a task's `.Succeeds`, `.Fails` or `.Completes`, or a delegate dispatcher property of a node (`Delegate(Info.OnTargetLost)`). |
<!-- dst:end 5 -->

## DST6xxx: structure

What can only be judged once the whole tree is read: states, clauses, targets, links, subtrees.

<!-- dst:begin 6 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST6001 | error | There are two states called `…` …. | State names are unique among their siblings: a transition finds its target by name. Rename one. |
| DST6002 | error | The target of `->` is a state's name or path (Idle.Phone), or Next, Parent, NextParent, TreeSucceeded, TreeFailed. | The target of `->` is unknown, matches several states (the message lists their paths; write more of the path, like `Idle.Phone`), or is not a name at all. |
| DST6003 | error | `…` is a Group: it holds states, not tasks. | A Group holds states only. Remove `[ Group ]`, or move the tasks (or the link) into a child state. |
| DST6004 | error | `…` links another tree, so it runs that tree's tasks, not its own. | A linked state runs the tasks of what it links, so it takes no tasks and no child states. Move them to a state beside it, or remove the link. |
| DST6005 | error | A state has at most one `…` clause. | `on`, `pick`, `utility` and `link` are written once per state. Put everything in the first one. (`if`, `do` and `->` may be repeated.) |
| DST6006 | error | `…` is a subtree of more than one included library. | `link Name` needs a `Subtree Name` in this file or an included library, and only one library may declare it. For another asset write `link asset Name`. |
| DST6007 | error | `->` is for the outputs of a node; a link takes `Name = value`. | The arguments of `link` set the target's parameters: `link Hit(Toughness = 2)`. Name a parameter the target has, once; `->` is not allowed here. |
| DST6008 | error | `…` is not a StateTree asset. | `link asset` needs a StateTree asset that exists. Build it first, or write `Path(Game, "AI/ST_Other")`. |
| DST6009 | error | These trees link each other in a circle, so none of them can be built first: …. | Trees may not link each other in a circle, and a tree cannot link itself. |
| DST6010 | warning | `pick` chooses among child states, and this state has none. | `pick` chooses among child states. Remove it, or add children. |
| DST6012 | error | The tree has no root state. | The document needs a `State` at its top level; the first one is the root. |
| DST6013 | error | A library holds only Tags, Template and Subtree. | A library holds `Tags`, `Template` and `Subtree` only. Move anything else into the tree that uses it. |
| DST6014 | error | `…` is not a global task or an evaluator. | `Globals` holds evaluators and global tasks. Put other nodes in a state. |
| DST6015 | error | The parameter `…` is declared twice. | A name is taken twice: a parameter, a color, a global node, or a task name visible from here. Names must differ from each other and from `Params`, `Event` and the schema's contexts. |
| DST6016 | warning | `…` waits for the event …, but no transition `on …` leads into it. | `on Tag` is only the gate; the event does not enter the state by itself. Add `-> ThisState on Tag` here or on a state above. |
| DST6017 | error | The transition to `…` can never succeed: its event … and the event the state requires, …, do not agree. | A transition that goes straight to a state with `on` must send the same tag, and a payload type the state's is derived from. |
| DST6018 | warning | The engine ignores delay and priority on a transition that triggers when the state completes. | The engine ignores `Delay` and `Priority` on a transition that triggers when the state completes. Use `on Tick` with a condition, or an event. |
<!-- dst:end 6 -->

## DST7xxx: generation and the engine's compile

The generator checks its model before it touches the asset, then writes the asset, runs the engine's compile and places the
compile's messages at the text that caused them (a message that names a node, a parameter or an event by ID lands on that item's
line, one that names only a state on the state, any other on the header).

<!-- dst:begin 7 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST7001 | error | … cannot be created: `…` is not a StateTree schema class. | The asset path or the schema class is not usable: a path that is not a package under a mount (`/Game/...`), an asset not named like its package, or a class that is not a concrete schema. |
| DST7002 | error | … already exists as a …, not a StateTree. | The name in the header belongs to an asset that is not a StateTree. Choose another `Name`, or delete that asset. |
| DST7003 | error | The engine's compile of the StateTree failed without saying why. | Raised for the engine's compile errors and for the generator's own checks of the model. An engine message arrives at the line of the node, state or parameter it names; this includes what a game's editor schema writes in `HandlePostInternalCompile`. Fix what the message says. When it says the compile failed without a reason, look at the LogStateTree lines in the output log. |
| DST7004 | warning, info | (the engine's own message) | A warning (or a note) from the engine's compile, at the line of the item it names. |
| DST7005 | error, warning | The engine's compile changed the editor data the generator wrote (…). | The engine's compile changed the editor data the generator wrote, which means the generator wrote something the engine had to correct. Report it as a bug with the text; the message names the first difference. As a warning it says the check could not be made. |
| DST7006 | error | … cannot be saved to …. | The package could not be saved. Is the file read-only or locked by source control, or is an editor holding it? |
| DST7007 | error, warning, info | (the extension's own code and message) | A report of a `UDreamStateTreeExtension::Validate`. The message starts with the extension's own code (`GAME0001: ...`). Fix what that extension says. |
<!-- dst:end 7 -->

## DST8xxx: decompiling and write-back

DST80xx come from reading an asset, DST805x from writing its text, and DST81xx from write-back. The first two groups are about
an asset and have no line; they are explained in [Decompiling.md](Decompiling.md#what-cannot-be-expressed). The DST81xx are
explained in [WriteBack.md](WriteBack.md#what-is-never-written-back).
<!-- dst:begin 8 -->
| Code | Severity | Message | Cause and fix |
|---|---|---|---|
| DST8001 | error | The editor data of `…` is a `…`, not a plain UStateTreeEditorData, and the model cannot hold what a subclass adds. | The tree's editor data is a subclass of `UStateTreeEditorData` (another plugin's tree kind) and the model cannot hold what the subclass adds. Such a tree cannot be written as text. |
| DST8002 | error | The editor data of `…` carries the extension `…`, which the model does not know. | The editor data carries an extension other than the editor's view settings (`StateCentricViewEditorDataExtension`, which is ignored). Decompiling would lose it, so it stops. |
| DST8003 | error | `…` reads from `…`, which is … rather than the instance data of a node; the model cannot name that source. | A binding reads from something the text has no name for: a struct that is not in the tree any more, a node without a usable name, a transition event outside its own transition, a whole event payload, an event binding that cannot be repaired. Also raised when a global node has no name, or when a subtree parameter and a tree parameter share a name. Bind the property again in the editor, or rename the clashing names. |
| DST8004 | error, warning | The parameter `…` of … cannot be written: …. | A value or type has no text form: a parameter type, a fixed-size array that differs from its default, a transition trigger the model does not have, a property whose export text does not read back. As a warning: a transition that forces its target to stay (`ForceSustained`) was read as the default; only the default and `Reactivate` have a text form. Change the type or put the value back to the default. |
| DST8005 | error | State `…` has the same name as a sibling state, so both of them have the path `…`. | Two sibling states have the same name, so the model cannot tell them apart. Rename one in the editor. |
| DST8006 | warning | Repaired an event binding broken by duplicating the asset. | A binding to an event was broken by duplicating the asset (the engine gives the copy new event IDs) and was reconnected. Build the asset from its text again to fix it for good. |
| DST8007 | error | `…` listens to the completion of a task. The model holds that only for a transition that triggers on a delegate. | A binding the model has no place for: listening to a task's completion outside an `on Delegate` transition, a chain of property functions that is too deep, a binding into a transition other than its delegate listener, or into the parameters of a state that does not link. Give the parameter a constant, or bind inside the subtree. |
| DST8008 | warning | State `…` has a color that is not in the tree's color list; the color was left out. | Something the model has no field for was left out: a color that is not in the color list, a transition's selection fallback, a binding whose property function has no node. |
| DST8009 | error | … points at a state (ID …) that is not in this tree. | A link or transition points at a state that is not in the tree. Set it again in the editor. |
| DST8010 | error | `…` has no editor data, so there is nothing to extract. | The asset has no editor data (it was cooked without it) or no schema, so nothing can be extracted. |
| DST8051 | error | The node type `…` is not one the text can name. | A node type the text cannot name: a hidden base struct, or a module that is not loaded. Load the plugin that declares it. |
| DST8052 | warning | The name `…` does not read back as the node … alone. | Two node types answer to the same name, so the written name might be read as the other. Give one a distinct `DreamStateTree` metadata name. |
| DST8053 | error | … is not a property of the node struct, so it cannot be written as a setting. | A node-struct value that is not a property of the struct cannot be written as a setting. |
| DST8054 | error | … reads `…`, a member of an array element, which the text has no way to name. | The text cannot write how a value is fed: a member of an array element, a property function the model does not hold, a struct fed in parts for a property that is not a struct. Bind the whole property instead. |
| DST8055 | error | The schema class `…` cannot be found, so the header cannot name it. | The schema class cannot be found, or a schema or tree setting is not a property of its class. The module that declares the schema must be loaded. |
| DST8056 | error, warning | A transition of `…` goes to a state but names none. | A transition, link, event or selection mode has no text form: a transition that goes to a state but names none, a state that no ending of its path singles out (warning), a payload struct that cannot be found, a target the text has no word for. Set the target in the editor, or rename the states. |
| DST8057 | error | The type of the parameter `…` (…) has no spelling the text reads back as the same type. | A parameter type has no spelling the text reads back as the same type (two structs, enums or classes share a name). Give one of them another name. |
| DST8058 | warning | The list starting at `…` has a Copy operand after its first item; it is read as `and`. | An item of a condition list has the engine's `Copy` operand after the first item; the text cannot say it and reads it as `and`. |
| DST8101 | error | The asset differs from the text, and no edit was found for it: …. | Write-back could not produce a text that rebuilds to the asset, so nothing was written and the asset keeps its changes: the asset differs from the text in a way no edit was found for, or the new text no longer checks, or it still differs from the asset. Rebuild the asset from the text to drop the difference, or make the change in the text yourself. The last two forms are bugs in write-back; report them with the text. |
| DST8102 | warning | …: … is given by the template `…`; a change here would have to change every use of it. | The changed value, or node, comes from a template: from its body, or from a template call that holds it. Changing it in the asset would have to change every use of the template. Change the template, or write the value out in place of the template call. The change stays in the asset until the next rebuild. |
| DST8103 | warning | … is a subtree copied in from an included library; it changed in the editor. | The change is inside a subtree that was copied into this tree from an included library. Make it in the library's subtree, where every tree that links it gets it. |
| DST8104 | warning | … changed in the editor (…); structural changes are not written back. | A structural change (states, clauses, tasks, conditions or transitions added, removed or reordered, or entries of a section) while **Write Back Structure** is off. Make the change in the text, or turn the setting on. |
| DST8105 | error, warning | The text is not a StateTree document, so there is no tree to compare the asset with. | As an error: the text is not a StateTree document, or cannot be read back for editing, so there is nothing to compare the asset with; fix the text's own errors, which are shown with it. As a warning: a change could not be written from the asset (the asset holds something with no text form, see the other messages, or the place of the change in the text was not found). |
| DST8106 | warning | …: the call was reprinted from the asset; the use of the template `…` in it is written out. | A note rather than a refusal. A node's call was reprinted from the asset, so the use of a template inside it is now written out as what it expands to. Expected after changing a node that a template call produced. |
| DST8107 | warning | State … was written again; the #Region lines inside it are dropped. | A card was reprinted and the `#Region` lines inside it were dropped (they cannot be placed again). Put them back by hand. |
| DST8108 | error | Two of the edits overlap, so nothing was written. | A bug in write-back: two planned edits overlap, so nothing was written. The asset keeps its changes. Report it with the text. |
| DST8109 | warning | The top-level states are in another order in the asset; the text keeps its order. | The top-level states are in another order in the asset. The first state is the root: reorder them in the text. |
<!-- dst:end 8 -->
