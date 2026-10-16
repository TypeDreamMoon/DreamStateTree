# DreamStateTree language reference

This page is for people writing `.dtr` and `.dth` files. The exact grammar and the canonical format are in
[Grammar.md](Grammar.md). How every construct maps onto the engine's StateTree is in [Mapping.md](Mapping.md).
Error codes are in [Diagnostics.md](Diagnostics.md). Adding your own nodes and schemas is in [Extending.md](Extending.md).

- [A first file](#a-first-file)
- [Files and the document header](#files-and-the-document-header)
- [Libraries](#libraries)
- [Sections](#sections) · [Settings](#settings) · [Parameters](#parameters) · [Globals](#globals) · [Tags](#tags) · [Colors](#colors)
- [States](#states) · [Kinds of state](#kinds-of-state) · [State settings](#state-settings)
- [Clauses](#clauses) · [on](#on) · [if](#if) · [pick](#pick) · [utility](#utility) · [link](#link) · [do](#do)
- [Transitions](#transitions)
- [Names](#names) · [Node names](#node-names-and-namespaces) · [Schemas](#schemas) · [Tags in the text](#tags-in-the-text)
- [Values](#values) · [Constants](#constants) · [Paths and bindings](#paths-and-bindings) · [Arguments](#arguments)
- [Operations](#operations-property-functions) · [Conditions](#conditions) · [Output bindings and ref](#output-bindings-and-ref)
- [Templates and #include](#templates-and-include) · [Regions and comments](#regions-and-comments)
- [Identifiers](#identifiers) · [Canonical format](#canonical-format)

## A first file

```dtr
StateTree(Name="AI/ST_Brawler", Schema=AIComponent(Actor = CombatEnemy))
{
    Parameters = { float AttackRange = 200; FVector WanderSpot; }
    Globals    = { Info = GetPlayerInfo(MaxRange = 2500); }

    State Root {
        -> Root

        State Attack {
            if ObjectIsValid(Info.TargetPlayerCharacter) and Info.DistanceToTarget < Params.AttackRange
            pick Random
            do FaceActor(ActorToFaceTowards = Info.TargetPlayerCharacter)
            -> Recover
            State Combo   { do ComboAttack() }
            State Charged { do ChargedAttack() }
        }
        State Chase {
            if ObjectIsValid(Info.TargetPlayerCharacter)
            do SetCharacterSpeed(Speed = 500)
            do MoveTo(TargetActor = Info.TargetPlayerCharacter, AcceptableRadius = Params.AttackRange * 0.75)
            -> Attack on Succeeded
            -> Root on Failed [ Delay = 0.5 ]
        }
        State Patrol {
            State FindSpot { do RunEnvQuery(EQ_Wander, Result = ref Params.WanderSpot)  -> Next on Succeeded }
            State Walk     { do SetCharacterSpeed(Speed = 200), MoveTo(Destination = Params.WanderSpot) }
        }
        State Recover { do Delay(1, 0.3) }
    }
}
```

The node names (`GetPlayerInfo`, `FaceActor`, `ComboAttack`, ...) are those of the engine's third-person template; any
node of your project works the same way.

- The header names the asset, `/Game/AI/ST_Brawler`, and its schema, the AI component schema. `Actor = CombatEnemy` sets the
  class of the schema's `Actor` context.
- `Parameters` are the tree's parameters. `Globals` holds global nodes; `Info` is a global task, and its outputs are read as
  `Info.DistanceToTarget`.
- `State Root` is the root. Its children are tried in the order written. `-> Root`, with no trigger, means "when the state
  completes, go to `Root`". When a child completes, the engine looks for a transition from the leaf upwards, so this one line
  catches all the children.
- `Info.DistanceToTarget < Params.AttackRange` is a comparison; it becomes a Compare Float condition. `Params.AttackRange * 0.75`
  is an operation; it becomes a Multiply Float property function bound to `AcceptableRadius`.
- `Result = ref Params.WanderSpot` hands the node a reference to a parameter it writes to.
- A clause is one thing in a state: `on`, `if`, `pick`, `utility`, `link`, `do` or `->`. Clauses are not separated by anything.
  A card holds its clauses first and its child states after them.

## Files and the document header

| Extension | Document | Builds |
|---|---|---|
| `.dtr` | `StateTree(...)` | one StateTree asset |
| `.dth` | `StateTreeLibrary(...)` | nothing; other files `#include` it |

Text is UTF-8 (a byte order mark is accepted). Lines may end in `\n`, `\r\n` or `\r`.

Sources live in the source roots: the project's `DTree/` folder and the `DTree/` folder of every enabled plugin that has one.
The command line and the editor find them there.

A file holds optional `#include` lines, then exactly one document (DST2002, DST2003). The canonical format puts the braces of
the document on lines of their own.

### The header

```dtr
StateTree(Name="AI/ST_Villager", Root="Game", Schema=Villager)
StateTree(Name="AI/ST_Brawler", Schema=AIComponent(Actor = CombatEnemy, AIController = AIC_Brawler))
```

| Argument | Meaning |
|---|---|
| `Name` | The asset's path below its root, without an extension. The last segment is the asset's name. Required. |
| `Root` | Where the path starts: `"Game"` (the project's content, the default), `"Engine"`, or `"Plugin.<Name>"` (that plugin's content). |
| `Schema` | The schema of the tree, by name ([Schemas](#schemas)). Required. The parentheses after it set the schema's contexts. |

| Header | Asset |
|---|---|
| `StateTree(Name="AI/ST_Brawler", Schema=AIComponent)` | `/Game/AI/ST_Brawler` |
| `StateTree(Name="AI/ST_Brawler", Root="Plugin.Combat", Schema=AIComponent)` | `/Combat/AI/ST_Brawler` |

The header takes only `Name`, `Root` and `Schema`, all of them named (DST2005, DST4006). A missing `Schema` is DST4005.

A build creates the asset when it does not exist and rebuilds it in place when it does: the asset object and its editor data
stay, so references to them and an open editor survive. Anything edited by hand in a generated tree and not written back to the
text is lost at the next build ([WriteBack.md](WriteBack.md)).

Inside the parentheses of the schema, a name sets one of the schema's contexts: `Actor` and `AIController` for the engine's
component schemas, the names a game schema declares for its own ([Extending.md](Extending.md#contexts)), or the name of a schema
property. The value is a class: `Actor = CombatEnemy`, `AIController = BP_BrawlerController`, `Actor = Path(Game, "AI/BP_Enemy")`.
A name the schema does not have is DST4019.

## Libraries

```dtr
StateTreeLibrary(Name="AI/VillagerCommon")
{
    Tags = { Game.Event.Villager; Game.Ask.Villager; }

    Template Nervous = Chance(1 - Params.Courage);
    Template Daytime(From, To) = Hour(From, To) and not Raining();

    Subtree HitReaction(float Toughness = 1) {
        if ObjectIsValid(Actor)
        State Stagger   { on Hit.Stagger  do Stagger(0.8 / Params.Toughness) }
        State KnockDown { on Hit.KnockDown  do Fall()  -> Next }
        State GetUp     { do GetUp() }
    }
}
```

A library holds `Tags`, `Template` and `Subtree`, and `#Region` lines. Anything else is DST6013.

- A **template** is an expression that is expanded where it is used ([Templates and #include](#templates-and-include)).
- A **subtree** in a library is copied into every tree that links it with `link` ([link](#link)). The engine's subtrees exist
  inside one asset, so the copy is how a subtree is shared.
- `Tags` declares tag prefixes; they apply in the files that include the library ([Tags](#tags)).

A library is checked on its own as far as a library can be: its syntax, its includes, its templates and what it may hold. A tree
that includes it is checked in full. Building a `.dth` builds the trees that include it.

## Sections

The braces of the document start with sections, then states and subtrees. A section is `Name = { entries }`; every entry ends
in `;`, and a `Key = Value` or `Type Name` entry may carry `[ metadata ]` before the `;`. Sections can come in any order and
each is written once (DST4013).

| Section | Entries | What it is |
|---|---|---|
| `Settings` | `Key = Value;` | settings of the tree |
| `Parameters` | `Type Name [= Default] [ meta ];` | the tree's parameters |
| `Globals` | `Name = Node(...);` | global tasks and evaluators |
| `Tags` | `Prefix;` | tag prefixes for this file |
| `Colors` | `Name = (R, G, B);` | the colors states can use |

An entry of the wrong form for its section is DST2009. The printer aligns the `=` of consecutive sections:

```dtr
Settings   = { GlobalTasksCompletion = Any; }
Parameters = { float Courage = 0.5 [ Category = "Character" ]; FVector Home; }
Globals    = { Player = NearestPlayer(); }
Tags       = { Game.Event.Villager; Game.Ask.Villager; }
Colors     = { Combat = (0.8, 0.25, 0.2); }
```

### Settings

`Key = Value;` sets a property of the tree's editor data. The key is the property's name, for example
`GlobalTasksCompletion = Any;` (`Any` or `All`: when the tree's global tasks count as complete). The value is a constant
([Constants](#constants)). A key that is not an editable property of the editor data is DST4014. A setting at its default is
not recorded in the asset, and the decompiler does not write it. A schema that does not let the tree choose how global tasks
complete refuses a different value (DST4015).

### Parameters

```dtr
Parameters = { float AttackRange = 200; FVector WanderSpot; Array<float> Weights = [1, 2]; }
```

`Type Name = Default;` declares a parameter of the tree. The default is optional and constant. Read a parameter as
`Params.Name` ([Paths and bindings](#paths-and-bindings)).

| Type | Written |
|---|---|
| bool, numbers | `bool`, `int` (or `int32`), `int64`, `float`, `double` |
| text | `Name`, `String`, `Text` |
| struct | its name without `F`: `Vector`, `FVector` |
| enum | its name: `EMyMode`, `MyMode` |
| object | a class name: `Actor`, `Pawn`, `BP_Enemy` (a reference to an instance) |
| class | `Class<Actor>` (also `SubclassOf<Actor>`), `SoftClass<Actor>` |
| soft object | `SoftObject<Texture2D>` |
| containers | `Array<T>`, `Set<T>`; two levels at most |

A struct is looked up first, then an enum, then a class; the name must be unique among them. A type the property bag cannot hold,
or a name that does not exist, is DST4009. A parameter declared twice is DST6015.

Metadata on a parameter, `[ Category = "Character"; Tooltip = "..." ]`, is kept as the property's metadata; its values are
strings, numbers, `true`, `false` or names.

The schema may refuse parameters on the tree altogether (DST4015).

### Globals

```dtr
Globals = { Info = GetPlayerInfo(MaxRange = 2500); Player = NearestPlayer(); }
```

`Name = Node(...);` adds a global node. Evaluators go to the tree's evaluators, tasks to its global tasks. The name is required;
it is how the rest of the file reads the node's outputs (`Info.DistanceToTarget`).

- A node that is neither an evaluator nor a task is DST6014.
- Evaluators run before global tasks, whatever order they are written in; a global node can read only the global nodes before
  it in that order (DST5017).
- A global name cannot be `Params` or `Event`, the name of a context, or the name of another global node (DST6015).
- Settings go after the call: `Info = GetPlayerInfo() [ Name = "info" ];`.
- A schema can forbid evaluators (DST4015).

### Tags

`Tags = { Game.Event.Villager; }` declares tag prefixes. Inside this file, and in the files that include a library that declares
them, a tag can be written without its prefix ([Tags in the text](#tags-in-the-text)). The section adds nothing to the asset
and nothing to the project's tag table. An entry is a path or a string: `Game.Event.Villager;` or `"Game.Event.Villager";`.

### Colors

```dtr
Colors = { Combat = (0.8, 0.25, 0.2); Calm = (R = 0.1, G = 0.4, B = 0.2, A = 1); }
```

A color is a tuple `(R, G, B)` or `(R, G, B, A)`, by position or by name; alpha is 1 unless written. States use a color with
`[ Color = Combat ]` ([State settings](#state-settings)). The name `Default Color` (in backticks) stands for the engine's
default color. A color declared twice is DST6015.

## States

```dtr
State Name [ settings ] {
    clause
    clause
    State Child { ... }
}
State Name [ settings ] { clause  clause }
```

A state is a card: its name, optional settings in `[ ]`, and a body in braces. The body holds clauses, child states and
`#Region` lines. A card with a few clauses and no child states may be written on one line, the clauses separated by two spaces;
the decompiler writes short cards that way ([Canonical format](#canonical-format)).

- The document may hold several top-level states and several subtrees. The first top-level `State` is the tree's root, where
  the tree starts (DST6012 when there is none). The other top-level states are further roots, reached by transitions.
- State names are unique among siblings (DST6001). A name that is not an identifier is written in backticks:
  ``State `Move To` { ... }``.
- A state name is how a transition finds its target ([Transitions](#transitions)).
- Line breaks mean nothing, with one exception. A clause ends where the next clause keyword, `State`, `Subtree`, `#Region`,
  `#EndRegion` or the closing `}` begins. Long clauses may be broken over lines anywhere inside them; `fmt` lays the
  clauses out again. The exception: a transition's `on` and `if` belong to it only on the line the transition has reached.
  On a line of their own they are the state's clauses, so `-> Root` followed by `if Ready` on the next line is an enter
  condition of the state, not a condition of the transition.

```dtr
State Flee {
    if Player.Sees and Player.Distance < 800
       and Chance(1 - Params.Courage)
    do Flee(From = Player.Pawn,
            Distance = 2000)
}
```

### Kinds of state

| Written | Engine kind |
|---|---|
| a card | State |
| `State Name [ Group ] { ... }` | Group: holds child states only, no tasks (DST6003) |
| a card with `link Subtree(...)` | Linked: runs a subtree of this tree |
| a card with `link asset Asset(...)` | LinkedAsset: runs another StateTree asset |
| `Subtree Name(params) { ... }` at the top level | Subtree: only entered through a link |

A linked state has no tasks and no children (DST6004). A schema can refuse a kind (DST4015).

```dtr
Subtree Knockback(FVector Direction, float Force = 600) [ Tag = Combat ] {
    pick Random
    State Light { if Force < 300  do Push(Force) }
    State Heavy { do Push(Force * 2), Fall() }
}
```

A `Subtree` is written at the top level of a document or library (DST2020 elsewhere). Its parameters are `Type Name [= Default]`;
a subtree without parameters needs no parentheses. Inside it, `Params.Name` is the subtree's parameter or the tree's; a name
that both have is DST5001. Only a `Subtree` takes a parameter list (DST2019).

### State settings

`State Name [ Key = Value; Flag ]`. Entries are separated by `;`. A key without a value is `true`.

| Key | Value | Default | Meaning |
|---|---|---|---|
| `Tag` | a gameplay tag | none | the state's tag |
| `Color` | a name from `Colors` | the default color | the state's color (DST4020 for an unknown name) |
| `Description` | `"text"` | empty | the state's description |
| `Enabled` | `true` / `false` | `true` | a disabled state is never selected |
| `Complete` | `Any` / `All` | `Any` | when the state's tasks count as complete: any of them, or all |
| `TickRate` | seconds | not set | the state ticks at its own rate |
| `Weight` | number | 1 | the state's weight for the weighted utility selection |
| `Group` | flag | | makes the state a Group |
| `ConsumeEvent` | `true` / `false` | `true` | whether selecting the state consumes the event of its `on`; only with an `on` clause |
| `bCopyParameterBindingsOnTick` | flag or bool | `false` | state parameter bindings are copied on every task tick, not only on entering and leaving |
| `bCheckPrerequisitesWhenActivatingChildDirectly` | flag or bool | `true` | the state's `on` event and enter conditions are checked when a transition leads straight to one of its children |
| `bFixedLayout` | flag or bool | `false` | the state's parameter layout is fixed (a linked state takes its target's layout, so it is set for you) |

The three `b` settings also answer to their names without the `b`.

Any other key is DST4014. A value of the wrong kind is DST4008. The decompiler writes only what differs from the default.

## Clauses

| Clause | Written | Times per state |
|---|---|---|
| [on](#on) | `on Tag`, `on Tag(PayloadType)` | once |
| [if](#if) | `if condition` | any number; they are joined with `and` |
| [pick](#pick) | `pick Mode` | once |
| [utility](#utility) | `utility expression` | once |
| [link](#link) | `link Subtree(args)`, `link asset Asset(args)` | once |
| [do](#do) | `do Task, Task` | any number; the tasks run in the order written |
| [->](#transitions) | `-> Target [on Trigger] [if condition] [ options ]` | any number; the order is the engine's priority order |

A second `on`, `pick`, `utility` or `link` is DST6005. The decompiler writes the clauses in the order of this table; you may
write them in any order, and the printer never moves them. The order of `do` and `->` clauses is the order of tasks and
transitions in the engine.

### on

```dtr
State Stagger { on Hit.Stagger  do Stagger(0.8) }
State Haggle  { on Trade(VillagerTradeOffer) }
```

`on Tag` is a gate: the state can be entered only while an event with that tag is being handled. The state and its descendants
read the event's payload as `Event.X` ([Paths and bindings](#paths-and-bindings)).

**`on` does not make the tree enter the state by itself.** An event arriving does not make the tree choose a state again. A
transition triggered by the same event and leading to the state is needed, usually on a parent: `-> Hurt on Hit`. The event is
still there when the state is chosen, and the gate lets it in. When no `on Tag` transition can reach a state that has
`on Tag`, you get warning DST6016.

The payload type goes in parentheses: `on Trade(VillagerTradeOffer)`, a struct (`USTRUCT`, or a user-defined struct asset by its
name). A game's extension can supply a default payload type per tag ([Extending.md](Extending.md#l3-extensions)); a type written
here wins. A tag that is not in the project is DST4010; a type that is not a struct is DST4011.

### if

```dtr
State Attack {
    if ObjectIsValid(Info.Target) and Info.Distance < Params.AttackRange
    if not Tired()
}
```

Enter conditions: the state can be selected only when they hold. Several `if` clauses on a state are joined with `and`. The
syntax of a condition is in [Conditions](#conditions). A schema can forbid enter conditions (DST4015).

### pick

| Written | Engine selection behavior |
|---|---|
| `pick InOrder` (the default, not printed) | Try Select Children In Order |
| `pick Random` | Try Select Children At Random |
| `pick HighestUtility` | Try Select Children With Highest Utility |
| `pick WeightedUtility` | Try Select Children At Random Weighted By Utility |
| `pick FollowTransitions` | Try Follow Transitions |
| `pick Enter` | Try Enter State |
| `pick None` | None: the state cannot be selected directly |

`pick UniformRandom` is read as `Random` and `pick RelativeUtility` as `WeightedUtility`: the engine keeps those two as old
names of the same values. The decompiler writes the current names. Any other word is DST4008; a mode the schema forbids is
DST4015. A `pick` on a state with no children is warning DST6010.

### utility

```dtr
State Chat {
    utility FloatInput(Player.Distance, (0, 3000)) * 0.5
}
State Wander { utility 0.5 }
```

The utility score of the state, used by `pick HighestUtility` and `pick WeightedUtility`. The expression is made of
consideration nodes and constants joined with `*`, `and` and `or`; parentheses group. A constant is a Constant consideration.
Operators bind as `*`, then `and`, then `or`. The state's own weight is a setting: `[ Weight = 2 ]`. A schema can forbid
considerations (DST4015). Considerations cannot read the output of a task (DST5014).

### link

```dtr
State Hurt  { link HitReaction(Toughness = 1 + Params.Courage) }
State Child { link asset ST_Other(Speed = 2) }
```

`link Name(args)` links a subtree. It must be a `Subtree Name` of this file or of an included library (DST6006); if it comes
from a library, the whole subtree is copied into the tree. `link asset Asset(args)` links another StateTree asset, by its name
or `Path(Game, "AI/ST_Other")`; that asset must exist, so it is built first (DST6008), and links cannot form a circle (DST6009).

The arguments set the target's parameters, by name or by position, in the order of the target's parameters. A constant overrides
the parameter's default. A path or an operation feeds the parameter from the linking state's scope. An unknown parameter, one
given twice or one too many is DST6007.

A state that links takes the target's parameter layout and selects the link; it takes no tasks and no children (DST6004), and
`pick` is not written for it. The arguments are evaluated before the state's own `on`, so they cannot read the state's own event.

### do

```dtr
do FaceActor(ActorToFaceTowards = Info.TargetPlayerCharacter)
do Combo = ComboAttack() [ ConsideredForCompletion = false ]
do `Combo Attack` = ComboAttack()
do Wait(20, 10), Tell(Fled)
```

A `do` clause lists tasks separated by commas, and a state may have several `do` clauses; the tasks run in the order written. A
schema that allows one task per state (DST4015 for more) names the task after its state.

A task is a call of a task node, or a task node's name without parentheses (`do Wait`). It may be named: `Combo = ComboAttack()`.
The name is the node's name in the engine, the one the editor shows. A named task's outputs are read as `Combo.Output`, and its
completion can be listened to: `-> Next on Delegate(Combo.Succeeds)`. A name that is not an identifier is written in backticks.
Task names must differ from the names in scope (DST6015): `Params`, `Event`, the contexts, the global nodes and the tasks
visible from here.

The settings of a node follow its call, in brackets:

| Setting | Meaning |
|---|---|
| `Name = "Combo"` | the node's name (the same as writing it before the `=`) |
| `Enabled = false` | the task is disabled |
| `ConsideredForCompletion = false` | the task does not decide when the state completes |
| `TransitionPriority = High` | the priority of the transitions this task triggers |
| `Evaluation = ForcedTrue` | for conditions: `Evaluated` (the default), `ForcedTrue` or `ForcedFalse` |
| any other editable property of the node struct | set as written |

The decompiler gives a name to any node that something binds to or listens to and that has none, from the node's type
(`Delay`, `Delay2`, ...).

## Transitions

```dtr
-> Target
-> Target on Trigger
-> Target if condition
-> Target on Trigger if condition [ options ]
```

| Part | Written |
|---|---|
| target | a state name or path; `Next`, `NextSelectable`, `Parent`, `NextParent`, `NextSelectableParent`; `TreeSucceeded`, `TreeFailed` |
| trigger | none: when the state completes; `on Succeeded`, `on Failed`, `on Completed`, `on Tick`; `on Tag` or `on Tag(PayloadType)`; `on Delegate(path)` |
| condition | `if condition` ([Conditions](#conditions)) |
| options | `Priority = Low` / `Normal` / `Medium` / `High` / `Critical`; `Delay = 1`; `Variance = 0.2`; `Reactivate`; `Enabled = false`; `ConsumeEvent = false` |

### Targets

A name or path is matched against the **end of the path of every state of the tree**. `Phone` matches every state called Phone;
`Idle.Phone` matches the Phone whose parent is Idle. It must match exactly one state. A name that matches none, or several, is
DST6002; the message lists the candidates, and the fix is to write more of the path. The decompiler writes the shortest ending
that is unique.

A state called like a keyword target (`Next`, `Parent`, ...) is written as a path or in backticks.

### Triggers

| Trigger | Meaning |
|---|---|
| none | the state completes, with success or failure |
| `Succeeded`, `Failed` | the state completes with that result |
| `Completed` | the state completes (the same as no trigger, written out) |
| `Tick` | every tick, while the condition holds |
| a tag | an event with that tag is handled |
| a tag with payload | `on Hit(MyPayload)`: the event carries that struct |
| `Delegate(path)` | a delegate fires: a task's `.Succeeds`, `.Fails` or `.Completes`, or a node's delegate dispatcher property |

`on Delegate(Combo.Succeeds)` listens to the task named `Combo` succeeding; `.Fails` and `.Completes` likewise.
`on Delegate(Info.OnTargetLost)` listens to the dispatcher property `OnTargetLost` of the node `Info`. Anything else is
DST5018.

In the condition of a transition triggered by an event, `Event.X` reads the payload of **that** transition's event. A transition
straight to a state that has `on` must send the same tag and a compatible payload (DST6017).

### Options

| Option | Engine field |
|---|---|
| `Priority = High` | the transition's priority |
| `Delay = 1` | delay in seconds (switches the delay on) |
| `Variance = 0.2` | random variance of the delay (switches the delay on) |
| `Reactivate` | the target state is reactivated even if it is active already |
| `Enabled = false` | the transition is disabled |
| `ConsumeEvent = false` | an event transition does not consume its event (DST4014 for other triggers) |

`Delay` and `Priority` do nothing on a transition that triggers when the state completes (`-> X`, `on Succeeded`, `on Failed`,
`on Completed`); the engine ignores them and the checker warns (DST6018). Use `on Tick` with a condition, or an event.

The last `[ ]` of a transition is always the transition's options, even when it follows the call of a condition. To give that
condition node settings, write them in front of the options or put the call in parentheses:

```dtr
-> Root on Tick if not Hour(22, 5) [ Delay = 1 ]
-> Root on Tick if Hour(22, 5) [ Name = "Night" ] [ Delay = 1 ]
-> Root on Tick if (Hour(22, 5) [ Name = "Night" ])
```

A node setting key (`Name`, `ConsideredForCompletion`, `TransitionPriority`, `Evaluation`) in the options is DST4014.

## Names

### Node names and namespaces

A node is written by its type name. The rules:

1. A node struct's name without the `F`, without a leading `StateTree`, and without a trailing `Task`, `Condition`, `Evaluator`,
   `Consideration` or `PropertyFunction`: `FStateTreeDelayTask` is `Delay`; `FStateTreeAddFloatPropertyFunction` is `AddFloat`.
2. A node struct with `meta = (DreamStateTree = "Villager.Walk")` is called what the metadata says; the name may include a
   namespace. `DreamStateTreeAliases` lists old names that still resolve.
3. The struct's own name without the `F` always works: `StateTreeDelayTask`.
4. A Blueprint node is its asset's short name, `STT_Wander`, or `Path(Game, "AI/STT_Wander")`.

Names are case-sensitive. The kind of place decides which nodes can stand there: tasks after `do`, conditions after `if` and in
transitions, considerations after `utility`, evaluators and global tasks in `Globals`, property functions in arguments. A node
of the wrong kind is DST4021, a name that is nothing is DST4001.

A schema may declare default namespaces (`DreamStateTreeNamespace = "Villager;Game"`). In a tree of that schema, `Walk` finds
`Villager.Walk` first, then `Game.Walk`, then a global `Walk`; the first match wins and the others are hidden with warning
DST4018. Writing the full name (`Game.Walk`) always reaches it. A name that fits several node types is DST4002 and lists them. A
node the schema refuses (`IsStructAllowed`, `IsClassAllowed`) is DST4003.

### Schemas

The schema of the header is the class name without `U`, `StateTree` and `Schema` (`UStateTreeAIComponentSchema` is
`AIComponent`, `UStateTreeComponentSchema` is `Component`), or the short name from the class's `DreamStateTree` metadata. The
reflected class name works too. An unknown or ambiguous schema is DST4004.

### Tags in the text

A tag is written as a path, `Game.Event.Villager.Hit`, or as a string, `"Game.Event.Villager.Hit"`; the string works in every
tag position. `None` is the empty tag.

- A prefix declared in `Tags` (in this file or an included library) may be left out: `on Hit` for
  `Game.Event.Villager.Hit` when `Game.Event.Villager` is declared. The prefixes are tried in turn; when different prefixes give
  different tags, the name is ambiguous (DST4010).
- In an argument, a tag path that is also a binding path (`Params`, a context, a node name) is an error (DST5001): write the
  tag as a string.
- The tag must be in the project's tag table. The text never adds tags (DST4010).

## Values

An argument, a setting or a default is one of these:

| Written | It is |
|---|---|
| `2`, `0.5`, `-1`, `1e3`, `true`, `"text"`, `None` | a constant |
| `Walk` | a constant: an enum value, when the property is an enum |
| `(0, 3000)`, `(X = 1, Y = 2)` | a struct constant |
| `[1, 2, 3]` | an array or set constant |
| `EQ_Wander`, `Path(Game, "AI/EQ_Wander")` | an asset reference (constant) |
| `Params.Home`, `Info.DistanceToTarget`, `Event.Clear` | a binding |
| `1 + Params.Hurry * 0.5`, `GetActorLocation(Player.Pawn)` | a property function bound into the property |
| `ref Params.WanderSpot` | a property reference |
| `Prop -> Params.Dist` | an output binding |

A literal is a constant, a path is a binding. That is the whole rule.

### Constants

How a constant is read depends on the type of the property it is for.

| Property | Constant |
|---|---|
| bool | `true`, `false` |
| int, int64 | a whole number; a fraction is an error |
| float, double | any number |
| `FName` | `"text"`, or a plain name |
| `FString`, `FText` | `"text"` (a localized text has no text form) |
| enum | the value's name, `Walk`; also qualified, `EMode.Walk` or `Mode.Walk`. A user-defined enum also answers to its display names |
| gameplay tag | a tag, as in [Tags in the text](#tags-in-the-text); `None` for none |
| tag container | `[Tag, Tag]` |
| struct | a tuple, by position in declaration order, `(0, 3000)`, or by field name, `(X = 1, Y = 2)`; a field not written keeps the struct's default (`FLinearColor` and `FColor` start fully opaque) |
| array, set | `[a, b, c]` |
| object, soft object | an asset by short name (when it is unique in the project), `Path(Game, "AI/EQ_Wander")`, or `None` |
| class, soft class | a class name (`Actor`, `BP_Enemy`), `Path(...)`, or `None` |

`Path(Root, "Rel")` names an asset: `Root` is `Game`, `Engine` or `Plugin.<Name>`, and `Rel` is the path below it.

Numbers are written `12`, `0.5`, `.5`, `1e-3`; a leading `-` is an operator. A value that does not fit its property (the wrong
kind of literal, an unknown enum value, an asset that does not exist or is ambiguous) is DST4008; the message says what was
expected. A fixed-size array and an instanced subobject have no text form.

Two constants combined with an operator are computed when the text is read: `2 * 0.75` is `1.5`. Whole numbers divide like the
engine's Int property function does, dropping the fraction.

### Paths and bindings

A path starts with a **root** and walks properties with `.`:

| Root | What it is |
|---|---|
| `Params.X` | a parameter of the tree; inside a `Subtree`, a parameter of the subtree or of the tree, and a name that both have is an error (DST5001) |
| a context name | a context of the schema: `Actor`, `AIController`, or what a game schema declares |
| a name from `Globals` | a global node; read its outputs (`Info.DistanceToTarget`) |
| a named task | a task of an ancestor state or an earlier task of this state; read its outputs (`Combo.Result`) |
| `Event.X` | the event of the nearest state, from this one up, that has `on`; `X` is `Tag`, `Origin` or a field of the payload |
| `StateName.Event.X` | the event of that named ancestor state |

The rules the engine sets:

- A path with just a context (`Actor`) binds the whole context object to a property of that type.
- The members after the root are properties; they walk into structs and objects.
- **Conditions, utility scores and state parameters cannot read a task's output** (DST5014): the engine builds them before the
  task's data exists. Tasks can read outputs of tasks above and before them.
- A task cannot read tasks that come later, or tasks of other states (DST5017). A global node reads only the global nodes
  before it (DST5017).
- `Event.X` does not cross the boundary of a subtree: a subtree cannot read the event of the state that links it (DST5007).
  Inside the condition of an event transition, `Event` is that transition's event.
- A path that has a type that does not fit the property it is bound to is DST5003. An unknown root is DST5001; an unknown
  property is DST5002.

### Arguments

A node call has arguments, written `Name = value` or by position:

```dtr
do MoveTo(TargetActor = Info.TargetPlayerCharacter, AcceptableRadius = 150)
do Delay(1, 0.3)
do Walk(Run = true)
```

- **By name.** The name is a property of the node's instance data, then, failing that, an editable property of the node struct
  itself. An unknown name is DST4007; the hint lists the properties.
- **A bool may drop its `b`**: `Walk(Run = true)` sets `bRun`. The decompiler writes the short form unless it would collide with
  another property.
- **By position**, for nodes that declare positions. Positional arguments come first, then named ones (DST2021). A node declares
  its positions with the `DreamStateTreeArgs` metadata ([Extending.md](Extending.md#l1-metadata)). The engine's common nodes
  have a built-in table:

| Node | Positions |
|---|---|
| `Delay` | `Duration`, `RandomDeviation` |
| `DebugText` | `Text` |
| `Random` (condition) | `Threshold` |
| `ObjectIsValid` | `Object` |
| `FloatInput` | `Input`, `Interval` |
| `Constant` | `Constant` |
| `MoveTo` | `Destination` |
| `RunEnvQuery` | `QueryTemplate` |
| `GetActorLocation` | `Input` |

More positional arguments than a node declares is DST4016; the same property given twice is DST4017.

What an argument may be depends on the property's category in the node:

| Category | Meaning |
|---|---|
| Input | **must be bound** to a path; a constant is DST5004 |
| Parameter | a constant or a binding, as you like |
| Output | cannot be written; copy it out with `->` (DST5005) |
| Context | not written: the compiler binds it to the context of its type; write it to override |
| property reference | needs `ref` ([Output bindings and ref](#output-bindings-and-ref)) |

An Input or a property reference left out is DST5004. A Context property for which the schema has no context is DST5003. Existing
assets often bind Context properties by hand in the editor; the decompiler leaves out the bindings the compiler would make by
itself and writes only the real overrides.

### Operations (property functions)

An operator in an argument becomes a property function node, bound into the property:

```dtr
do Walk(Factor = 1 + Params.Hurry * 0.5)
do Wait(Params.Duration / 2)
do Pace(Fast = Params.A and not Params.B)
```

- `+ - * /`, a leading `-`, and `and`, `or`, `not` become Add, Subtract, Multiply, Divide, Invert Float or Int, and Boolean And,
  Or, Not property functions. The type of the property being fed picks the family: float or double, int, or bool. Any other
  property type is DST5011.
- Precedence is the usual: `*` and `/` bind tighter than `+` and `-`.
- An operand that is a constant becomes that function's constant value; a path becomes a binding; a nested operation becomes a
  nested function.
- An operation of constants only is computed when the text is read.
- Any other property function is a call: `Location = GetActorLocation(Player.Pawn)`. Its output is the property named
  `Result`, `bResult` or `Output`, or its only Output property (DST5011 when it cannot be told).
- A comparison is a condition, not a value (DST5011).

Operators in parentheses: `(Params.A + 1) * 2` groups as usual.

### Conditions

The condition of `if`, and of a transition's `if`, is a condition node call, a comparison or a bool path, combined with `and`,
`or`, `not` and parentheses.

```dtr
if ObjectIsValid(Info.Target) and Info.Distance < Params.AttackRange
if (Alert or Armed) and not Hidden
if Player.Sees                           // a bool path
if Actor.bDeactivated == false
```

| Form | Becomes |
|---|---|
| `a < b`, `<=`, `==`, `!=`, `>=`, `>` | a Compare Int, Float, Bool, Enum or Name condition, chosen by the type of the side that reads something |
| a bool path | Compare Bool, `path == true` |
| `not C` | the condition with its invert flag set |
| a node call | that condition node |

- The side of a comparison that reads something (a path, an operation, a property function call) is the left side. A constant on
  the left swaps the sides and mirrors the operator: `3 < x` is `x > 3`. Two constants is DST5009.
- Int compared with a float, or with a fraction, is a Float comparison.
- Bools, enums and names allow `==` and `!=` only (DST5008); `!=` is the same node with the invert flag.
- An enum constant is read as a value of the enum on the other side: `Mode == Walk`.
- There is no comparison for objects; use `ObjectIsValid` (DST5008).
- `not` applies to one condition: `not (A and B)` is DST5010, as is `not` on a node that has no invert flag. `not not C`
  cancels itself.
- **Precedence**, tightest first: `not` and a leading `-`; `*` `/`; `+` `-`; `<` `<=` `>` `>=`; `==` `!=`; `and`; `or`.
  `not` binds to what comes right after it, so `not Info.Distance < 800` reads as `(not Info.Distance) < 800` and fails
  (DST5008: `<` does not work on a bool). To negate a comparison put it in parentheses: `not (Info.Distance < 800)`.
- Parentheses group; the engine's list takes at most four levels of them (DST5012).
- A name that is a condition node without parentheses (`if Alert`) is that node.

The engine evaluates the condition list strictly from left to right with no precedence, with an operand and an indent on each
item. The text's precedence and parentheses are turned into those, and back, so that writing a tree and decompiling it agree
(see [Mapping.md](Mapping.md#conditions-operands-and-indents)). The two spellings `(A or B) and C` and `A or B and C` are
different lists: the second is `A or (B and C)`, because `and` binds tighter than `or`.

Settings of a condition node follow its call: `Random(0.3) [ Evaluation = ForcedTrue ]`.

### Output bindings and ref

**Output binding.** `Prop -> Params.Dist` in an argument list copies the node's Output property `Prop` into a parameter of the tree
or of the subtree the state is in:

```dtr
do GetPlayer(TargetPlayer -> Params.Current_Target, ValidTarget -> Params.Has_Valid_Target)
```

`Prop` must be an Output (DST5016); the destination must be a parameter (DST5016); the types must be compatible (DST5003).

**Property reference.** A property that is a reference to another property (the engine's property ref) is given `ref` and a path:
`Result = ref Params.WanderSpot`. The node writes through the reference. The path must be a parameter, an event or an Output of a
node that runs earlier, not a context (DST5015). Without `ref`, or with `ref` on a property that is not a reference, it is
DST5015.

## Templates and #include

```dtr
#include "VillagerCommon.dth"

Template Nervous = Chance(1 - Params.Courage);
Template Daytime(From, To) = Hour(From, To) and not Raining();
```

`#include "file"` comes before the document, one per line. The path is looked up next to the including file first, then in each
`DTree/` source root. A library can include libraries; a cycle is DST3202, a missing file DST3201, a file that is not a library
DST3203. Each library is read once.

A **template** is `Template Name(params) = expression;`, in a tree or a library. Parameters are bare names with optional
defaults (`Rate = 1`). A use is a call of the template's name, `Daytime(9, 17)` (arguments by position or name), or the bare name
of a template without parameters, `Nervous`. The body replaces the use.

- Parameters are substituted wherever the body names them, including as the first part of a path.
- Templates can use templates, up to 16 levels deep (DST3104).
- A template expands wherever a value is written: the expression of `on`, `if`, `utility`, the tasks of `do`, the trigger and
  condition of a transition, arguments, section entry values and subtree defaults, and the values in `[ ]`. It does not expand
  in the word after `pick`, in the target after `->`, in the name after `link`, or in `Tags` entries.
- Settings written after a use apply to the call the template expands to (DST3108 when it is not a call).
- Two templates with one name in a file and its includes together is DST3105. A missing argument is DST3101, an unknown or
  doubled name DST3102, an extra argument DST3103, an output binding as an argument DST3107.

Templates are expanded after parsing and before names are checked. A diagnostic in an expanded template points at the use.

## Regions and comments

`// to the end of the line` and `/* ... */` (not nested) are comments. The formatter keeps them.

`#Region "Label"` and `#EndRegion` lines group statements in the document, in a section and in a state body. They must pair
inside one block (warning DST2008). A directive is the first thing on its line.

## Identifiers

An identifier starts with a letter or `_` and continues with letters, digits and `_`. Any character from U+0080 up that is not a
space counts as a letter, so names in any language work. Names are case-sensitive.

A name that is not an identifier is written in backticks: `` `Move To` ``. It can stand wherever a name can, and it is never a
keyword: `` `do` `` is a name. A name in backticks cannot be empty, cannot contain a backtick or a line break (DST1007). The
decompiler writes backticks for names the engine has with spaces.

There are no reserved words. Keywords count only where they can appear: `on`, `if`, `pick`, `utility`, `link`, `do` begin a
clause where a clause begins, and are plain names inside brackets or after a dot. At the top level of a clause, an unquoted
clause word is the start of the next clause; a state or argument called `do` is written `` `do` ``. A state called like a target
word (`Next`, `Parent`, ...) is written as a path or in backticks after `->`.

## Canonical format

`dst.ps1 fmt` prints every file in one form. Printing twice gives the same text. Comments and blank lines are kept, the order of
clauses and states is kept.

- 4 spaces of indentation. The header on one line; the document's `{` and `}` on lines of their own.
- The members of the document keep the order written. A blank line separates sections from templates from states, and every
  top-level state and subtree from the next; adjacent one-line sections stay together with their `=` aligned.
- A state's `{` follows its name.
- A card with no child states, no comments and no blank lines between its clauses, with at most three clauses, of which at most
  two are more than a bare `-> Target`, and that fits in 120 columns, is written on one line, the clauses separated by two
  spaces. Consecutive one-line cards line their `{` up.
- A line longer than 120 columns is broken inside its brackets, one argument per line with the closing bracket back at the
  indent of the line; a chain of `and` or of `or` takes one operand per line with the operator in front, under the first
  operand; the tasks of a `do` take one line each, all but the last ending in `,`.
- A call, list, tuple or card that you spread over several lines stays spread when `fmt` prints your file; the decompiler joins
  everything that fits.
- Settings in `[ ]` are always on one line: `[ Key = Value; Flag ]`.
- Blank lines the author wrote between clauses stay (several become one).
- Parameters and similar section entries are aligned by type and name.
- Line ends: `fmt -Write` keeps the file's own.
